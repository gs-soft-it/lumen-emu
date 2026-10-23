// Lumen, 2026-10-07. SPDX-License-Identifier: GPL-2.0-only
// Original synthetic x86 fixtures. This executable never loads game inputs.
#include "AndroidFexCoreRuntime.h"
#include "MemoryHandoff.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#if __has_warning("-Wmissing-designated-field-initializers")
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#endif
#include "Interface/Context/Context.h"
#include "Interface/Core/JIT/DebugData.h"
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/Allocator.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <FEXCore/Utils/LumenMemoryHandoffAbi.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

static_assert(FEX_LUMEN_HAS_MEMORY_PRESERVE_ALL_ABI == 1, "Android fixture requires the configured preserving ABI");

using BulkCheck = std::uint32_t (*)(std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t) noexcept;
extern "C" BulkCheck FEXLumenGuestMemoryBulkProviderV1() noexcept;

namespace {
using Lumen::GuestCpu::MemoryAccessIntent;
struct Access { std::uint64_t address, bytes; MemoryAccessIntent intent; };
struct BulkPause {
    std::uint64_t destination {};
    std::atomic<bool> entered {}, armed {}, release {}, valid {true};
};
struct Probe {
    std::uint64_t code {}, data {}, hostPage {}, returnAddress {};
    std::uint64_t stackOverride {};
    std::vector<Access> accesses;
    bool recordAccesses {true};
    bool validateReadDirectory {}, readDirectoryValid {true};
    std::uint64_t callbackCount {};
    bool deny {};
    bool clobberPreserveAll {};
    std::uint64_t preserveAllowed {}, preserveDenied {}, preserveStatusSamples {};
    std::uint64_t preserveFpcr {}, preserveFpsr {};
    bool preserveStatusRestored {true};
    bool observePlatformRegister {};
    std::uint64_t hostPlatformRegister {}, firstPlatformMismatch {};
    std::uint64_t nativeCallbacks {}, platformMismatches {};
    std::uint64_t bulkAllowed {}, bulkDenied {};
    bool checkBulkBounds {}, bulkBoundsValid {true};
    BulkPause* bulkPause {};
};
Probe* active {};
std::atomic<unsigned> farCompilationRestarts {}, bufferCompilationRestarts {};
std::atomic<std::uint64_t> preserveDeniedSignalRip {};

std::uint64_t NativePlatformRegister() noexcept {
#if defined(__ANDROID__) && defined(__aarch64__)
    std::uint64_t value;
    asm volatile("mov %0, x18" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

bool Handoff(std::uint64_t address, std::uint64_t bytes, MemoryAccessIntent intent) noexcept {
    const auto platformRegister = NativePlatformRegister();
    auto& probe = *active;
    if (probe.validateReadDirectory && intent == MemoryAccessIntent::Read &&
        Lumen::GuestCpu::MemoryHandoffReadGuardDirectory()->load(std::memory_order_acquire) == nullptr)
        probe.readDirectoryValid = false;
    if (probe.observePlatformRegister) {
        ++probe.nativeCallbacks;
        if (platformRegister != probe.hostPlatformRegister) {
            if (probe.platformMismatches++ == 0) probe.firstPlatformMismatch = platformRegister;
        }
    }
    if (address >= probe.data && address - probe.data < 6 * probe.hostPage &&
        bytes <= 6 * probe.hostPage - (address - probe.data)) {
        ++probe.callbackCount;
        if (probe.recordAccesses) probe.accesses.push_back({address, bytes, intent});
        return !probe.deny;
    }
    return true; // Conservative hash collision outside the test's resource.
}
std::uint32_t Executable(void* opaque, std::uint64_t address, std::uint64_t* base,
    std::uint64_t* bytes, std::uint32_t* writable) noexcept {
    const auto& probe = *static_cast<Probe*>(opaque);
    if (address < probe.code || address - probe.code >= probe.hostPage) return 0;
    *base = probe.code; *bytes = probe.hostPage; *writable = 0;
    return 1;
}
std::uint32_t HostCall(void*, std::uint64_t, std::uint64_t*, std::uint64_t*) noexcept {
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
}
bool Run(Probe& probe, std::uint64_t offset, std::uint64_t destination,
    std::uint64_t source, std::uint64_t value, std::uint64_t count,
    std::uint64_t desired, std::uint64_t* result = nullptr,
    LumenAndroidFexCoreGuestStateV1* finalState = nullptr) {
    probe.accesses.clear();
    const auto stack = probe.stackOverride != 0 ? probe.stackOverride : probe.data + 4 * probe.hostPage - 8;
    std::memcpy(reinterpret_cast<void*>(stack), &probe.returnAddress, 8);
    LumenAndroidFexCoreGuestStateV1 state {};
    state.structSize = sizeof(state); state.abiVersion = 1;
    state.rip = probe.code + offset;
    state.gprs[0] = value; state.gprs[1] = desired != 0 ? desired : count;
    state.gprs[4] = stack; state.gprs[6] = source; state.gprs[7] = destination;
    const auto status = LumenAndroidFexCoreRunGuestV1(&state, HostCall, Executable, &probe);
    if (result != nullptr) *result = state.gprs[0];
    if (finalState != nullptr) *finalState = state;
    if (status != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) {
        std::printf("FAIL run offset=%llu status=%u\n", static_cast<unsigned long long>(offset), status);
        return false;
    }
    return true;
}
bool Covers(const Probe& probe, std::uint64_t address, std::uint64_t bytes, bool write) {
    for (std::uint64_t offset = 0; offset < bytes; ++offset) {
        if (std::none_of(probe.accesses.begin(), probe.accesses.end(), [&](const Access& access) {
            return address + offset >= access.address && address + offset - access.address < access.bytes &&
                Lumen::GuestCpu::WritesMemory(access.intent) == write;
        })) return false;
    }
    return true;
}
bool Check(bool success, const char* name) {
    std::printf("%s memory_handoff %s\n", success ? "PASS" : "FAIL", name);
    return success;
}

std::vector<std::uint8_t> PlatformPressureProgram() {
    // Original synthetic x86 instructions. Keep eight values live across
    // watched RMW operations, including carry/borrow, shifts and CAS pairs.
    // The callback only reads x18; it neither invents nor writes platform state.
    std::vector<std::uint8_t> code;
    const auto append = [&](std::initializer_list<std::uint8_t> bytes) {
        code.insert(code.end(), bytes);
    };
    const auto offset = [&](std::uint32_t value) {
        for (unsigned shift = 0; shift != 32; shift += 8)
            code.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    for (unsigned reg = 0; reg != 8; ++reg) {
        append({0x4c, 0x8b, static_cast<std::uint8_t>(0x87 + 8 * reg)});
        offset(8 * reg); // mov r8..r15, [rdi+offset]
    }
    append({0x48, 0xc7, 0xc0, 3, 0, 0, 0});
    append({0x48, 0xc7, 0xc2, 5, 0, 0, 0});
    append({0x48, 0xc7, 0xc3, 7, 0, 0, 0});
    append({0x48, 0xc7, 0xc1, 11, 0, 0, 0});
    for (unsigned reg = 0; reg != 8; ++reg) {
        append({0x4d, 0x39, 0xc8}); // cmp r8, r9
        append({0x4c, 0x11, static_cast<std::uint8_t>(0x87 + 8 * reg)});
        offset(0x80 + 8 * reg); // adc [rdi+offset], r8..r15
        append({0x4c, 0x19, static_cast<std::uint8_t>(0x87 + 8 * reg)});
        offset(0xc0 + 8 * reg); // sbb [rdi+offset], r8..r15
        append({0x4c, 0x31, static_cast<std::uint8_t>(0x87 + 8 * reg)});
        offset(0x100 + 8 * reg); // xor [rdi+offset], r8..r15
        append({0x4c, 0x0f, 0xa4, 0xb7});
        offset(0x140); append({3}); // shld [rdi+offset], r14, 3
        append({0xf0, 0x48, 0x0f, 0xc7, 0x8f});
        offset(0x180); // lock cmpxchg16b [rdi+offset]
    }
    append({0xb8, 0x18, 0, 0, 0, 0xc3});
    return code;
}

std::vector<std::uint8_t> ConcurrentVirtualReloadProgram() {
    std::vector<std::uint8_t> code;
    const auto append = [&](std::initializer_list<std::uint8_t> bytes) {
        code.insert(code.end(), bytes);
    };
    append({0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}); // push r12..r15
    append({0x49, 0x89, 0xfd, 0x49, 0x89, 0xce, 0x49, 0x89, 0xf7}); // owner/index/control
    append({0x45, 0x31, 0xe4}); // xor r12d, r12d
    const auto loop = code.size();
    append({0x4d, 0x8b, 0x45, 0x00, 0x41, 0xff, 0x50, 0x18}); // virtual call clobbers r8
    append({0x0f, 0xb6, 0xc0, 0x8d, 0x14, 0x00});
    append({0x4d, 0x8b, 0x45, 0x38}); // reload table from owner
    append({0x43, 0x0b, 0x14, 0xb0, 0x81, 0xca, 0x00, 0x00, 0x00, 0x20});
    append({0x43, 0x89, 0x14, 0xb0});
    append({0x41, 0x8b, 0x47, 0x04, 0xa8, 0x01}); // test control.stop bit 0
    // One has odd parity. This forward JNP must cross the inflated body with
    // FEX's CondJumpBit/TBNZ lowering (32 KiB range). A conditional backedge
    // uses a short inverted branch around the handoff interrupt check instead.
    append({0x0f, 0x8b}); // jnp return
    const auto exitDisplacement = code.size();
    append({0, 0, 0, 0});
    // Keep the decoded loop under one guest page while the real handoff
    // fragments exceed TBZ's 32 KiB range. Its compilation must restart with
    // far branches; an ordinary short virtual-call fixture does not cover it.
    for (unsigned index = 0; index != 160; ++index)
        append({0x43, 0x83, 0x0c, 0xb0, 0x00}); // or dword [r8+r14*4], 0
    append({0x41, 0xff, 0xc4, 0x45, 0x89, 0x27}); // inc r12d; publish progress
    append({0x41, 0x81, 0xfc, 0x00, 0x00, 0x01, 0x00, 0x73, 0x05}); // bounded fallback at 65536 iterations
    append({0xe9}); // jmp loop
    const auto backedge = static_cast<std::int32_t>(loop) - static_cast<std::int32_t>(code.size() + 4);
    for (unsigned shift = 0; shift != 32; shift += 8)
        code.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(backedge) >> shift));
    const auto exitEdge = static_cast<std::uint32_t>(code.size() - (exitDisplacement + 4));
    for (unsigned shift = 0; shift != 32; shift += 8)
        code[exitDisplacement + shift / 8] = static_cast<std::uint8_t>(exitEdge >> shift);
    append({0x44, 0x89, 0xe0}); // return exact iteration count
    append({0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0xc3});
    return code;
}

struct ConcurrentControl {
    std::atomic<std::uint32_t> progress {0}, stop {0};
};
static_assert(sizeof(ConcurrentControl) == 8);

class MetadataSyscalls final : public FEXCore::HLE::SyscallHandler {
public:
    MetadataSyscalls(std::uint64_t base, std::uint64_t bytes) : base_(base), bytes_(bytes) {
        OSABI = FEXCore::HLE::SyscallOSABI::OS_GENERIC;
    }
    std::uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame*, FEXCore::HLE::SyscallArguments*) override { return 0; }
    FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(
        FEXCore::Core::InternalThreadState*, std::uint64_t address) override {
        return address >= base_ && address - base_ < bytes_
            ? FEXCore::HLE::ExecutableRangeInfo {base_, bytes_, false}
            : FEXCore::HLE::ExecutableRangeInfo {0, 0, false};
    }
    std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(
        FEXCore::Core::InternalThreadState*, std::uint64_t) override { return std::nullopt; }
private:
    std::uint64_t base_, bytes_;
};

bool RestartMetadataInvariant(std::uint64_t page) {
    // Compile only this new owned page. The context cannot query other guest
    // code and no generated instruction, helper or syscall is executed.
    auto* mapping = static_cast<std::uint8_t*>(mmap(nullptr, page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (mapping == MAP_FAILED) return false;
    std::vector<std::uint8_t> code {0x8b, 0x07, 0xa8, 0x01, 0x0f, 0x8b, 0, 0, 0, 0}; // test/jnp footer
    for (unsigned index = 0; index != 160; ++index)
        code.insert(code.end(), {0x83, 0x4f, 0x08, 0x00}); // or dword [rdi+8],0
    code.insert(code.end(), {0xb8, 0x34, 0x12, 0x00, 0x00});
    const auto exitDisplacement = static_cast<std::uint32_t>(code.size() - 10);
    for (unsigned shift = 0; shift != 32; shift += 8)
        code[6 + shift / 8] = static_cast<std::uint8_t>(exitDisplacement >> shift);
    code.push_back(0xc3);
    if (code.size() > page) { munmap(mapping, page); return false; }
    std::memcpy(mapping, code.data(), code.size());
    if (mprotect(mapping, page, PROT_READ) != 0) { munmap(mapping, page); return false; }
    const auto entry = reinterpret_cast<std::uintptr_t>(mapping);
    MetadataSyscalls syscalls(entry, page);
    FEXCore::SignalDelegator signals;
    // Conservative features suffice for compilation; this context executes
    // nothing and makes no claim about the device's optional capabilities.
    FEXCore::HostFeatures features {};
    features.DCacheLineLog2 = 4;
    features.HostType = FEXCore::HostFeatures::HostTypeEnum::Linux;
    features.CPUMIDRs.push_back(0);
    auto context = FEXCore::Context::Context::CreateNewContext(features);
    if (!context) { munmap(mapping, page); return false; }
    auto* impl = static_cast<FEXCore::Context::ContextImpl*>(context.get());
    impl->Config.NeedsPendingInterruptFaultCheck = true;
    context->SetSignalDelegator(&signals);
    context->SetSyscallHandler(&syscalls);
    if (!context->InitCore()) { munmap(mapping, page); return false; }
    auto* thread = context->CreateThread();
    if (thread == nullptr) { munmap(mapping, page); return false; }
    // The public CreateThread default has no segment-table pointers. The
    // decoder reads CS even for compile-only work, so provide an owned long
    // mode descriptor using this frame's built-in GDT storage.
    auto& state = thread->CurrentFrame->State;
    state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = state.private_gdt;
    state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = state.private_gdt;
    state.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
    auto* segment = FEXCore::Core::CPUState::GetSegmentFromIndex(state, state.cs_idx);
    FEXCore::Core::CPUState::SetGDTBase(segment, 0);
    FEXCore::Core::CPUState::SetGDTLimit(segment, 0xf'ffffU);
    segment->L = 1;
    segment->D = 0;
    state.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*segment);
    FEXCore::Allocator::RegisterTLSData(thread);
    const auto restartsBefore = farCompilationRestarts.load();
    auto compiled = impl->CompileCode(thread, entry);
    const auto restarts = farCompilationRestarts.load() - restartsBefore;
    bool monotonic = compiled.DebugData != nullptr, subblocks = monotonic;
    std::uint64_t previousHost {}, previousEnd {};
    std::size_t opcodeCount {}, subblockCount {};
    if (compiled.DebugData) {
        opcodeCount = compiled.DebugData->GuestOpcodes.size();
        subblockCount = compiled.DebugData->Subblocks.size();
        for (const auto& opcode : compiled.DebugData->GuestOpcodes) {
            monotonic &= opcode.HostEntryOffset >= 0 &&
                static_cast<std::uint64_t>(opcode.HostEntryOffset) >= previousHost &&
                static_cast<std::uint64_t>(opcode.HostEntryOffset) < compiled.CompiledCode.Size;
            previousHost = static_cast<std::uint64_t>(opcode.HostEntryOffset);
        }
        for (const auto& block : compiled.DebugData->Subblocks) {
            const auto end = static_cast<std::uint64_t>(block.HostCodeOffset) + block.HostCodeSize;
            subblocks &= block.HostCodeOffset >= previousEnd && end <= compiled.CompiledCode.Size;
            previousEnd = end;
        }
    }
    const bool valid = compiled.CompiledCode.BlockBegin != nullptr && opcodeCount != 0 && subblockCount != 0 &&
        restarts != 0 && monotonic && subblocks;
    std::printf("FEX_RESTART_METADATA far_restarts=%u opcodes=%zu subblocks=%zu monotonic=%u extents_valid=%u compiled=%u\n",
        restarts, opcodeCount, subblockCount, monotonic ? 1U : 0U, subblocks ? 1U : 0U,
        compiled.CompiledCode.BlockBegin != nullptr ? 1U : 0U);
    FEXCore::Allocator::UninstallTLSData(thread);
    context->DestroyThread(thread);
    munmap(mapping, page);
    return valid;
}

bool ConcurrentVirtualReload(Probe& probe, std::uint64_t codeOffset,
    std::uint64_t object, std::uint64_t table, ConcurrentControl& control, bool watched, bool toggleReadDirectory = false) {
    constexpr unsigned MutationCount = 32;
    constexpr std::uint64_t Index = 7;
    const auto callbacksBefore = probe.callbackCount;
    const auto restartsBefore = farCompilationRestarts.load();
    std::array<std::uint32_t, 16> original {}, expected {}, actual {};
    for (std::size_t index = 0; index != original.size(); ++index)
        original[index] = UINT32_C(0x0055aa00) + (static_cast<std::uint32_t>(index) << 8);
    expected = original;
    expected[Index] |= UINT32_C(0x20000002);
    std::memcpy(reinterpret_cast<void*>(table), original.data(), sizeof(original));
    control.progress.store(0);
    control.stop.store(0);
    probe.recordAccesses = false;
    probe.validateReadDirectory = toggleReadDirectory;
    probe.readDirectoryValid = true;
    std::atomic<bool> completed {false};
    bool ran = false;
    std::uint64_t result {};
    std::thread guest([&] {
        ran = Run(probe, codeOffset, object, reinterpret_cast<std::uintptr_t>(&control), 0, Index, 0, &result);
        completed.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (control.progress.load(std::memory_order_acquire) == 0 &&
           !completed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const bool started = control.progress.load(std::memory_order_acquire) != 0;
    unsigned mutations {}, failedMutations {};
    bool ownerProof = true;
    for (; mutations < MutationCount && !completed.load(std::memory_order_acquire); ++mutations) {
        const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
            LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
        if (token == 0) { ++failedMutations; break; }
        ownerProof &= LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(token) == 1;
        if (toggleReadDirectory) Lumen::GuestCpu::PublishMemoryHandoffGpuDirty((mutations & 1U) != 0);
        const auto stoppedProgress = control.progress.load(std::memory_order_acquire);
        if (mutations + 1 == MutationCount) control.stop.store(1, std::memory_order_release);
        LumenAndroidFexCoreEndGuestMemoryMutationV1(token, 0, 0, 0);
        if (mutations + 1 != MutationCount) {
            while (control.progress.load(std::memory_order_acquire) <= stoppedProgress &&
                   !completed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            if (control.progress.load(std::memory_order_acquire) <= stoppedProgress) {
                ++failedMutations;
                break;
            }
        }
    }
    // Only the atomic control word may change without a successful writer
    // epoch, so a denied mutation can still terminate this bounded fixture.
    control.stop.store(1, std::memory_order_release);
    guest.join();
    probe.recordAccesses = true;
    probe.validateReadDirectory = false;
    Lumen::GuestCpu::PublishMemoryHandoffGpuDirty(true);
    std::memcpy(actual.data(), reinterpret_cast<const void*>(table), sizeof(actual));
    std::uint64_t ownerField {};
    std::memcpy(&ownerField, reinterpret_cast<const void*>(object + 0x38), sizeof(ownerField));
    const auto callbacks = probe.callbackCount - callbacksBefore;
    const auto restarts = farCompilationRestarts.load() - restartsBefore;
    const auto progress = control.progress.load(std::memory_order_acquire);
    std::printf("FEX_VIRTUAL_RELOAD_CONCURRENT guard=%u mutations=%u failed_mutations=%u"
        " iterations=%u result=%llu far_restarts=%u callbacks=%llu exact_table=%u owner_valid=%u\n",
        watched ? 1U : 0U, mutations, failedMutations, progress,
        static_cast<unsigned long long>(result), restarts, static_cast<unsigned long long>(callbacks),
        actual == expected ? 1U : 0U, ownerField == table ? 1U : 0U);
    return ran && started && ownerProof && probe.readDirectoryValid && mutations == MutationCount && failedMutations == 0 && progress != 0 &&
        result == progress && actual == expected && ownerField == table &&
        (watched ? callbacks != 0 : callbacks == 0) && (watched || restarts != 0);
}

unsigned UnwatchedBulkContracts(Probe& probe) {
    // Keep this fresh mapping alive until process exit: conservative guards
    // never clear and must not contaminate the other fixtures' clean pages.
    auto* memory = static_cast<std::uint8_t*>(mmap(nullptr, 6 * probe.hostPage,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (memory == MAP_FAILED) return 0;
    const auto originalData = probe.data;
    probe.data = reinterpret_cast<std::uintptr_t>(memory);
    const auto destination = probe.data + 4096 - 32;
    const auto source = probe.data + 2 * probe.hostPage + 128;
    const auto clean = probe.data + probe.hostPage + 256;
    unsigned passed {};
    LumenAndroidFexCoreGuestStateV1 state {};
    std::uint64_t result {};
    const auto resetWitness = [&] { probe.bulkAllowed = probe.bulkDenied = 0; };
    const auto enroll = [&](std::uint64_t address, std::uint64_t bytes) {
        const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
            LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
        if (token == 0) return false;
        const bool owned = LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(token) == 1;
        if (owned) Lumen::GuestCpu::MarkHandoffRange(address, bytes);
        return LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 1 && owned;
    };
    probe.hostPlatformRegister = NativePlatformRegister();
    const auto platformBefore = probe.platformMismatches;
    probe.observePlatformRegister = true;
    probe.checkBulkBounds = true;
    resetWitness();
    std::memset(reinterpret_cast<void*>(clean), 0, 512);
    passed += Check(Run(probe, 896, clean, 0, 0x5a, 512, 0, &result, &state) &&
        result == 1 && state.gprs[1] == 0 && state.gprs[7] == clean + 512 &&
        probe.bulkAllowed == 1 && probe.bulkDenied == 0 && probe.accesses.empty() && probe.bulkBoundsValid &&
        probe.platformMismatches == platformBefore && NativePlatformRegister() == probe.hostPlatformRegister &&
        std::all_of(reinterpret_cast<const std::uint8_t*>(clean), reinterpret_cast<const std::uint8_t*>(clean + 512),
            [](auto byte) { return byte == 0x5a; }), "rep_bulk_stos_flags_count_pointer_x18_bounds");
    resetWitness();
    const std::uint64_t pattern = UINT64_C(0x123456789abcdef0);
    std::memset(reinterpret_cast<void*>(clean), 0, 512);
    bool stosq = Run(probe, 1000, clean, 0, pattern, 64, 0, nullptr, &state);
    for (std::uint64_t index = 0; index != 64; ++index)
        stosq &= std::memcmp(reinterpret_cast<const void*>(clean + index * 8), &pattern, 8) == 0;
    passed += Check(stosq && state.gprs[1] == 0 && state.gprs[7] == clean + 512 &&
        probe.bulkAllowed == 1 && probe.bulkDenied == 0 && probe.accesses.empty(), "rep_bulk_stos_qword_exact");
    for (unsigned index = 0; index != 512; ++index)
        reinterpret_cast<std::uint8_t*>(source)[index] = static_cast<std::uint8_t>(index * 17 + 3);
    resetWitness();
    passed += Check(Run(probe, 960, clean, source, 0, 64, 0, nullptr, &state) &&
        state.gprs[1] == 0 && state.gprs[7] == clean + 512 && state.gprs[6] == source + 512 &&
        probe.bulkAllowed == 1 && probe.bulkDenied == 0 && probe.accesses.empty() &&
        std::memcmp(reinterpret_cast<const void*>(clean), reinterpret_cast<const void*>(source), 512) == 0,
        "rep_bulk_movs_qword_exact_registers");
    const auto overlap = probe.data + 3 * probe.hostPage + 128;
    const auto seedOverlap = [&] {
        for (unsigned index = 0; index != 96; ++index)
            reinterpret_cast<std::uint8_t*>(overlap)[index] = static_cast<std::uint8_t>(index + 1);
    };
    seedOverlap();
    resetWitness();
    passed += Check(Run(probe, 256, overlap + 1, overlap, 0, 64, 0, nullptr, &state) &&
        state.gprs[1] == 0 && state.gprs[7] == overlap + 65 && state.gprs[6] == overlap + 64 &&
        probe.bulkAllowed == 1 && probe.bulkDenied == 0 && probe.accesses.empty() &&
        std::all_of(reinterpret_cast<const std::uint8_t*>(overlap), reinterpret_cast<const std::uint8_t*>(overlap + 65),
            [](auto byte) { return byte == 1; }), "rep_bulk_forward_overlap_scalar_semantics");
    seedOverlap();
    resetWitness();
    bool reverse = Run(probe, 320, overlap + 64, overlap + 63, 0, 64, 0, nullptr, &state);
    for (unsigned index = 0; index != 64; ++index)
        reverse &= reinterpret_cast<const std::uint8_t*>(overlap)[index + 1] == index + 1;
    passed += Check(reverse && state.gprs[1] == 0 && state.gprs[7] == overlap && state.gprs[6] == overlap - 1 &&
        probe.bulkAllowed == 0 && probe.bulkDenied == 0 && probe.accesses.empty(), "rep_reverse_unwatched_original_loop");

    // Reuse precisely the same byte-MOVS translation as each touched guard is
    // enrolled. The first page is initially clean; the second-page guard must
    // deny the complete bulk route even though its first 32 bytes are clean.
    resetWitness();
    passed += Check(Run(probe, 256, destination, source, 0, 128, 0) &&
        probe.bulkAllowed == 1 && probe.bulkDenied == 0 && probe.accesses.empty() &&
        std::memcmp(reinterpret_cast<const void*>(destination), reinterpret_cast<const void*>(source), 128) == 0,
        "rep_bulk_cached_cross_page_initial");
    const bool secondEnrolled = enroll(probe.data + 4096, 4096);
    resetWitness();
    std::memset(reinterpret_cast<void*>(destination), 0, 128);
    passed += Check(secondEnrolled && Run(probe, 256, destination, source, 0, 128, 0) &&
        probe.bulkAllowed == 0 && probe.bulkDenied == 1 && Covers(probe, probe.data + 4096, 96, true) &&
        std::none_of(probe.accesses.begin(), probe.accesses.end(), [&](const Access& access) {
            return access.address < probe.data + 4096 || access.bytes != 1;
        }) && std::memcmp(reinterpret_cast<const void*>(destination), reinterpret_cast<const void*>(source), 128) == 0,
        "rep_cached_second_touched_page_fallback");
    const bool firstEnrolled = enroll(probe.data, 4096);
    resetWitness();
    std::memset(reinterpret_cast<void*>(destination), 0, 128);
    passed += Check(firstEnrolled && Run(probe, 256, destination, source, 0, 128, 0) &&
        probe.bulkAllowed == 0 && probe.bulkDenied == 1 && Covers(probe, destination, 128, true) &&
        std::memcmp(reinterpret_cast<const void*>(destination), reinterpret_cast<const void*>(source), 128) == 0,
        "rep_cached_first_touched_page_fallback");
    resetWitness();
    passed += Check(Run(probe, 896, destination, 0, 0x5a, 512, 0, &result, &state) &&
        result == 1 && state.gprs[1] == 0 && state.gprs[7] == destination + 512 &&
        probe.bulkAllowed == 0 && probe.bulkDenied == 1 && Covers(probe, destination, 512, true) &&
        probe.platformMismatches == platformBefore && NativePlatformRegister() == probe.hostPlatformRegister &&
        std::all_of(reinterpret_cast<const std::uint8_t*>(destination), reinterpret_cast<const std::uint8_t*>(destination + 512),
            [](auto byte) { return byte == 0x5a; }), "rep_cached_watched_stos_flags_count_pointer_x18");
    const bool sourceEnrolled = enroll(source, 512);
    const auto cleanDestination = probe.data + 4 * probe.hostPage + 128;
    resetWitness();
    passed += Check(sourceEnrolled && Run(probe, 256, cleanDestination, source, 0, 128, 0) &&
        probe.bulkAllowed == 0 && probe.bulkDenied == 1 && Covers(probe, source, 128, false) &&
        std::all_of(probe.accesses.begin(), probe.accesses.end(), [](const Access& access) {
            return access.intent == MemoryAccessIntent::Read;
        }) && std::memcmp(reinterpret_cast<const void*>(cleanDestination), reinterpret_cast<const void*>(source), 128) == 0,
        "rep_watched_source_unwatched_destination_fallback");
    const bool overlapEnrolled = enroll(overlap, 96);
    seedOverlap();
    resetWitness();
    reverse = Run(probe, 320, overlap + 64, overlap + 63, 0, 64, 0, nullptr, &state);
    for (unsigned index = 0; index != 64; ++index)
        reverse &= reinterpret_cast<const std::uint8_t*>(overlap)[index + 1] == index + 1;
    passed += Check(overlapEnrolled && reverse && state.gprs[1] == 0 &&
        probe.bulkAllowed == 0 && probe.bulkDenied == 0 && Covers(probe, overlap, 64, false) &&
        Covers(probe, overlap + 1, 64, true), "rep_reverse_watched_overlap_original_loop");
    // The overlap enrollment also watches the normal return stack's page on
    // a 4 KiB host. RET's own read is legitimate even when REP's count is zero.
    // Confirm that receipt, then isolate RET on a clean page and retain the
    // stronger empty-access oracle for the zero-count instruction itself.
    std::array<std::uint8_t, 128> sourceBefore {}, destinationBefore {};
    std::memcpy(sourceBefore.data(), reinterpret_cast<const void*>(source), sourceBefore.size());
    std::memcpy(destinationBefore.data(), reinterpret_cast<const void*>(destination), destinationBefore.size());
    const auto zeroStateExact = [&] {
        return state.gprs[1] == 0 && state.gprs[7] == destination && state.gprs[6] == source &&
            std::memcmp(sourceBefore.data(), reinterpret_cast<const void*>(source), sourceBefore.size()) == 0 &&
            std::memcmp(destinationBefore.data(), reinterpret_cast<const void*>(destination), destinationBefore.size()) == 0;
    };
    resetWitness();
    const auto normalStack = probe.data + 4 * probe.hostPage - 8;
    const bool watchedStack = Lumen::GuestCpu::NeedsHandoff(normalStack, 8);
    const bool zeroWithReturnReceipt = Run(probe, 256, destination, source, 0, 0, 0, nullptr, &state) &&
        zeroStateExact() && probe.bulkAllowed == 0 && probe.bulkDenied == 0 &&
        (watchedStack ? !probe.accesses.empty() : probe.accesses.empty()) &&
        std::all_of(probe.accesses.begin(), probe.accesses.end(), [&](const Access& access) {
            return access.address == normalStack && access.bytes == 8 && access.intent == MemoryAccessIntent::Read;
        });
    const auto returnReceipts = probe.accesses.size();
    probe.stackOverride = probe.data + 5 * probe.hostPage - 8;
    const bool cleanReturnStack = !Lumen::GuestCpu::NeedsHandoff(probe.stackOverride, 8);
    resetWitness();
    const bool zeroIsolated = cleanReturnStack && Run(probe, 256, destination, source, 0, 0, 0, nullptr, &state) &&
        zeroStateExact() && probe.bulkAllowed == 0 && probe.bulkDenied == 0 && probe.accesses.empty();
    probe.stackOverride = 0;
    std::printf("FEX_REP_ZERO watched_return_stack=%u return_read_receipts=%zu isolated_stack_clean=%u"
        " isolated_callbacks=%zu bulk_allowed=%llu bulk_denied=%llu count_pointer_bytes_exact=%u\n",
        watchedStack ? 1U : 0U, returnReceipts, cleanReturnStack ? 1U : 0U, probe.accesses.size(),
        static_cast<unsigned long long>(probe.bulkAllowed), static_cast<unsigned long long>(probe.bulkDenied),
        zeroStateExact() ? 1U : 0U);
    passed += Check(zeroWithReturnReceipt && zeroIsolated, "rep_bulk_zero_count_no_admission");
    probe.observePlatformRegister = false;
    const auto check = FEXLumenGuestMemoryBulkProviderV1();
    passed += Check(check != nullptr && check(cleanDestination, source, 1, 0x101) == 0 &&
        check(cleanDestination, 0, 0, 1) == 0 && check(UINT64_MAX, 0, 2, 1) == 0 &&
        check(cleanDestination, 0, UINT64_MAX, 8) == 0 && check(cleanDestination, 0, 1, 0x201) == 0,
        "rep_bulk_requires_counted_execution_and_valid_bounds");

    // Hold a real guest execution inside the admitted predicate. The writer
    // signals only after arming the actual request; it cannot acquire and
    // enroll a watched guard until the admitted native copy has completed.
    const auto writerDestination = probe.data + 5 * probe.hostPage + 128;
    const auto writerSource = probe.data + 5 * probe.hostPage + 2048;
    for (unsigned index = 0; index != 512; ++index)
        reinterpret_cast<std::uint8_t*>(writerSource)[index] = static_cast<std::uint8_t>(index * 11 + 7);
    std::memset(reinterpret_cast<void*>(writerDestination), 0, 512);
    BulkPause pause;
    pause.destination = writerDestination;
    probe.bulkPause = &pause;
    resetWitness();
    std::atomic<bool> guestComplete {}, writerAcquired {};
    bool guestRan = false, writerValid = false, writerSawComplete = false;
    std::thread guest([&] {
        guestRan = Run(probe, 256, writerDestination, writerSource, 0, 512, 0);
        guestComplete.store(true, std::memory_order_release);
    });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
    while (!pause.entered.load(std::memory_order_acquire) && !guestComplete.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const bool entered = pause.entered.load(std::memory_order_acquire);
    std::thread writer([&] {
        const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
            LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
        if (token == 0) return;
        writerAcquired.store(true, std::memory_order_release);
        writerValid = LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(token) == 1;
        writerSawComplete = std::memcmp(reinterpret_cast<const void*>(writerDestination),
            reinterpret_cast<const void*>(writerSource), 512) == 0;
        if (writerValid) Lumen::GuestCpu::MarkHandoffRange(writerDestination, 512);
        writerValid &= LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 1;
    });
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
    while (!pause.armed.load(std::memory_order_acquire) && !writerAcquired.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const bool blocked = pause.armed.load(std::memory_order_acquire) && !writerAcquired.load(std::memory_order_acquire);
    pause.release.store(true, std::memory_order_release);
    guest.join(); writer.join();
    probe.bulkPause = nullptr;
    const bool admitted = probe.bulkAllowed == 1 && probe.bulkDenied == 0;
    resetWitness();
    std::memset(reinterpret_cast<void*>(writerDestination), 0, 512);
    const bool cachedFallback = Run(probe, 256, writerDestination, writerSource, 0, 512, 0) &&
        probe.bulkAllowed == 0 && probe.bulkDenied == 1 && Covers(probe, writerDestination, 512, true) &&
        std::memcmp(reinterpret_cast<const void*>(writerDestination), reinterpret_cast<const void*>(writerSource), 512) == 0;
    std::printf("FEX_REP_BULK writer_armed=%u reader_held=%u writer_complete_bytes=%u cached_fallback=%u bounds=%u\n",
        pause.armed.load() ? 1U : 0U, blocked ? 1U : 0U, writerSawComplete ? 1U : 0U,
        cachedFallback ? 1U : 0U, probe.bulkBoundsValid ? 1U : 0U);
    passed += Check(entered && blocked && pause.valid.load() && guestRan && admitted && writerValid && writerSawComplete &&
        cachedFallback, "rep_bulk_writer_quiescence_before_enrollment_cached_fallback");
    probe.data = originalData;
    return passed;
}

std::vector<std::uint8_t> PreserveAllPressureProgram(std::size_t* firstWatchedOffset) {
    // Original code: sixteen distinct YMM values and eight GP values remain
    // live across two cold checks. Output stores and seed loads are unwatched.
    std::vector<std::uint8_t> code;
    const auto append = [&](std::initializer_list<std::uint8_t> bytes) { code.insert(code.end(), bytes); };
    const auto offset = [&](std::uint32_t value) {
        for (unsigned shift = 0; shift != 32; shift += 8) code.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    for (unsigned reg = 0; reg != 16; ++reg) {
        append({0xc5, static_cast<std::uint8_t>(reg < 8 ? 0xfe : 0x7e), 0x6f,
            static_cast<std::uint8_t>(0x86 + 8 * (reg & 7))});
        offset(32 * reg); // vmovdqu ymm0..15,[rsi+disp32]
    }
    for (unsigned reg = 0; reg != 8; ++reg) {
        append({0x4c, 0x8b, static_cast<std::uint8_t>(0x86 + 8 * reg)});
        offset(512 + 8 * reg); // mov r8..15,[rsi+disp32]
    }
    append({0x48, 0x8d, 0x96}); offset(4096); // replaced with real host-page displacement below
    append({0x31, 0xc0, 0x39, 0xc0}); // zero RAX; ZF=1, CF=0
    *firstWatchedOffset = code.size();
    append({0x48, 0x8b, 0x07, 0x48, 0x8b, 0x4f, 0x08}); // two watched scalar reads
    append({0x0f, 0x94, 0xc0, 0x0f, 0xb6, 0xc0, 0x48, 0x89, 0x82}); offset(512); // retain ZF
    append({0xf3, 0x0f, 0x2d, 0xc0, 0x48, 0x89, 0x82}); offset(520); // CVTSS2SI 1.75 => 2 (nearest)
    for (unsigned reg = 0; reg != 16; ++reg) {
        append({0xc5, static_cast<std::uint8_t>(reg < 8 ? 0xfe : 0x7e), 0x7f,
            static_cast<std::uint8_t>(0x82 + 8 * (reg & 7))});
        offset(32 * reg); // vmovdqu [rdx+disp32],ymm0..15
    }
    append({0xc5, 0xf8, 0x77, 0xb8, 0x34, 0x12, 0, 0, 0xc3});
    return code;
}

unsigned PreserveAllContracts(Probe& probe) {
    const bool provider = FEX_LUMEN_HAS_MEMORY_PRESERVE_ALL_ABI && FEXLumenGuestMemoryPreserveAllProviderV1() != nullptr;
    unsigned passed = Check(provider, "preserve_all_typed_provider_available");
    if (!provider) return passed;
    auto* code = static_cast<std::uint8_t*>(mmap(nullptr, probe.hostPage, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    auto* data = static_cast<std::uint8_t*>(mmap(nullptr, 6 * probe.hostPage, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (code == MAP_FAILED || data == MAP_FAILED) return passed;
    const auto oldCode = probe.code, oldData = probe.data;
    probe.code = reinterpret_cast<std::uintptr_t>(code); probe.data = reinterpret_cast<std::uintptr_t>(data);
    std::size_t firstWatchedOffset {};
    auto program = PreserveAllPressureProgram(&firstWatchedOffset);
    // The LEA follows 16 vector loads (8 bytes each) and 8 GP loads (7 bytes).
    const std::uint32_t pageDisplacement = static_cast<std::uint32_t>(probe.hostPage);
    std::memcpy(program.data() + 16 * 8 + 8 * 7 + 3, &pageDisplacement, 4);
    if (program.size() > probe.hostPage) return passed;
    std::memcpy(code, program.data(), program.size());
    if (mprotect(code, probe.hostPage, PROT_READ) != 0) return passed;
    const auto watched = probe.data + 64, seeds = probe.data + probe.hostPage + 128;
    const auto output = seeds + probe.hostPage;
    std::array<std::uint8_t, 512> vectors;
    for (unsigned index = 0; index != vectors.size(); ++index) vectors[index] = static_cast<std::uint8_t>(index * 17 + 3);
    const std::uint32_t oneAndThreeQuarters = 0x3fe00000;
    std::memcpy(vectors.data(), &oneAndThreeQuarters, 4);
    std::memcpy(reinterpret_cast<void*>(seeds), vectors.data(), vectors.size());
    std::array<std::uint64_t, 8> gp;
    for (unsigned index = 0; index != gp.size(); ++index) gp[index] = UINT64_C(0x123456789abcdef0) + index;
    std::memcpy(reinterpret_cast<void*>(seeds + 512), gp.data(), sizeof(gp));
    const std::array<std::uint64_t, 2> watchedBytes {UINT64_C(0x1122334455667788), UINT64_C(0x8877665544332211)};
    std::memcpy(reinterpret_cast<void*>(watched), watchedBytes.data(), sizeof(watchedBytes));
    LumenAndroidFexCoreGuestStateV1 state {};
    std::uint64_t result {};
    const auto reset = [&] {
        probe.preserveAllowed = probe.preserveDenied = probe.preserveStatusSamples = 0;
        probe.preserveStatusRestored = true;
        std::memset(reinterpret_cast<void*>(output), 0xa5, 528);
    };
    const auto exact = [&] {
        std::uint64_t flags {}, rounded {};
        std::memcpy(&flags, reinterpret_cast<const void*>(output + 512), 8);
        std::memcpy(&rounded, reinterpret_cast<const void*>(output + 520), 8);
        return result == 0x1234 && flags == 1 && rounded == 2 &&
            std::memcmp(reinterpret_cast<const void*>(output), vectors.data(), vectors.size()) == 0 &&
            std::equal(gp.begin(), gp.end(), state.gprs + 8);
    };
    probe.clobberPreserveAll = true;
    probe.hostPlatformRegister = NativePlatformRegister();
    const auto platformBefore = probe.platformMismatches;
    probe.observePlatformRegister = true;
    reset();
    const bool clean = !Lumen::GuestCpu::NeedsHandoff(watched, sizeof(watchedBytes)) &&
        Run(probe, 0, watched, seeds, 0, 0, 0, &result, &state) && exact() && probe.accesses.empty() &&
        probe.preserveAllowed == 0 && probe.preserveDenied == 0 && probe.preserveStatusSamples == 0;
    passed += Check(clean, "preserve_all_guard0_no_cold_call_live_registers");
    const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
    bool enrolled = token != 0 && LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(token) == 1;
    if (enrolled) Lumen::GuestCpu::MarkHandoffRange(watched, sizeof(watchedBytes));
    if (token != 0) enrolled &= LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 1;
    reset();
    const bool watchedExact = enrolled && Run(probe, 0, watched, seeds, 0, 0, 0, &result, &state) && exact() &&
        probe.preserveAllowed == 2 && probe.preserveDenied == 0 && probe.preserveStatusSamples == 2 &&
        probe.preserveStatusRestored && probe.accesses.size() == 2 && Covers(probe, watched, sizeof(watchedBytes), false) &&
        probe.platformMismatches == platformBefore && NativePlatformRegister() == probe.hostPlatformRegister;
    passed += Check(watchedExact, "preserve_all_watched_live_gp_ymm_x18_nzcv_fpcr_fpsr");
    reset();
    // This fault path calls RunGuestV1 directly instead of Run(), so it must
    // also start a new receipt window. The previous successful call has two
    // legitimate receipts that cannot belong to the denied instruction.
    const auto priorReceipts = probe.accesses.size();
    probe.accesses.clear();
    probe.deny = true;
    preserveDeniedSignalRip.store(0, std::memory_order_relaxed);
    const auto stack = probe.data + 4 * probe.hostPage - 8;
    std::memcpy(reinterpret_cast<void*>(stack), &probe.returnAddress, 8);
    state = {}; state.structSize = sizeof(state); state.abiVersion = 1;
    state.rip = probe.code; state.gprs[4] = stack; state.gprs[6] = seeds; state.gprs[7] = watched;
    const auto status = LumenAndroidFexCoreRunGuestV1(&state, HostCall, Executable, &probe);
    probe.deny = false;
    // V1's failure result does not reconstruct live SSA/SRA GPRs into the
    // exported frame. The guard1 success above proves live registers after
    // restoration; denial instead requires the actual inline trap/RIP and
    // unchanged bytes before any later guest effects.
    const bool preciseRip = preserveDeniedSignalRip.load(std::memory_order_relaxed) == probe.code + firstWatchedOffset;
    const bool receiptExact = probe.accesses.size() == 1 &&
        probe.accesses[0].address == watched && probe.accesses[0].bytes == 8 &&
        probe.accesses[0].intent == MemoryAccessIntent::Read;
    const bool outputExact = std::all_of(reinterpret_cast<const std::uint8_t*>(output),
        reinterpret_cast<const std::uint8_t*>(output + 528), [](auto byte) { return byte == 0xa5; });
    const bool watchedBytesExact = std::memcmp(reinterpret_cast<const void*>(watched), watchedBytes.data(), sizeof(watchedBytes)) == 0;
    const bool vectorSeedsExact = std::memcmp(reinterpret_cast<const void*>(seeds), vectors.data(), vectors.size()) == 0;
    const bool gpSeedsExact = std::memcmp(reinterpret_cast<const void*>(seeds + 512), gp.data(), sizeof(gp)) == 0;
    const bool platformExact = NativePlatformRegister() == probe.hostPlatformRegister;
    const bool denied = status == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 && state.faultSignal == SIGTRAP &&
        preciseRip && probe.preserveAllowed == 0 && probe.preserveDenied == 1 && probe.preserveStatusSamples == 1 &&
        probe.preserveStatusRestored && receiptExact && outputExact && watchedBytesExact && vectorSeedsExact && gpSeedsExact && platformExact;
    std::printf("FEX_PRESERVE_ALL_DENY status=%u expected_status=%u signal=%u expected_signal=%u"
        " precise_rip=%u allowed=%llu denied=%llu prior_receipts_cleared=%zu current_receipts=%zu"
        " receipt_address=%llx expected_address=%llx receipt_bytes=%llu expected_bytes=8 receipt_intent=%u"
        " expected_intent=%u receipt_exact=%u output_canary=%u watched_canary=%u vector_seeds=%u gp_seeds=%u"
        " status_samples=%llu status_restored=%u platform=%u\n",
        status, static_cast<unsigned>(LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1), state.faultSignal,
        static_cast<unsigned>(SIGTRAP), preciseRip ? 1U : 0U,
        static_cast<unsigned long long>(probe.preserveAllowed), static_cast<unsigned long long>(probe.preserveDenied),
        priorReceipts, probe.accesses.size(),
        static_cast<unsigned long long>(probe.accesses.empty() ? 0 : probe.accesses[0].address),
        static_cast<unsigned long long>(watched),
        static_cast<unsigned long long>(probe.accesses.empty() ? 0 : probe.accesses[0].bytes),
        probe.accesses.empty() ? UINT32_MAX : static_cast<unsigned>(probe.accesses[0].intent),
        static_cast<unsigned>(MemoryAccessIntent::Read), receiptExact ? 1U : 0U, outputExact ? 1U : 0U,
        watchedBytesExact ? 1U : 0U, vectorSeedsExact ? 1U : 0U, gpSeedsExact ? 1U : 0U,
        static_cast<unsigned long long>(probe.preserveStatusSamples), probe.preserveStatusRestored ? 1U : 0U,
        platformExact ? 1U : 0U);
    passed += Check(denied, "preserve_all_denied_inline_trap_precise_rip_no_access");
    std::printf("FEX_PRESERVE_ALL typed=%u guard0=%u watched=%u deny=%u status_samples=%llu"
        " observed_denied=%llu precise_rip=%u vectors=16 vector_bytes=32 gp_live=8 same_program=1"
        " denied_gp_snapshot_asserted=0\n",
        provider ? 1U : 0U, clean ? 1U : 0U, watchedExact ? 1U : 0U, denied ? 1U : 0U,
        static_cast<unsigned long long>(probe.preserveStatusSamples), static_cast<unsigned long long>(probe.preserveDenied),
        preserveDeniedSignalRip.load(std::memory_order_relaxed) == probe.code + firstWatchedOffset ? 1U : 0U);
    probe.clobberPreserveAll = false; probe.observePlatformRegister = false;
    probe.code = oldCode; probe.data = oldData;
    // Conservative guard entries never clear. Keep mappings alive until exit.
    return passed;
}

unsigned ReadDirectoryContracts(Probe& probe) {
    auto* code = static_cast<std::uint8_t*>(mmap(nullptr, probe.hostPage, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    auto* data = static_cast<std::uint8_t*>(mmap(nullptr, 6 * probe.hostPage, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (code == MAP_FAILED || data == MAP_FAILED) return 0;
    constexpr std::array<std::uint8_t, 4> read {0x48, 0x8b, 0x07, 0xc3};
    constexpr std::array<std::uint8_t, 7> write {0x48, 0x89, 0x07, 0x48, 0x8b, 0x07, 0xc3};
    constexpr std::array<std::uint8_t, 4> copy {0xfc, 0xf3, 0xa4, 0xc3};
    std::memcpy(code, read.data(), read.size());
    std::memcpy(code + 32, write.data(), write.size());
    std::memcpy(code + 64, copy.data(), copy.size());
    if (mprotect(code, probe.hostPage, PROT_READ) != 0) return 0;
    const auto oldCode = probe.code, oldData = probe.data;
    probe.code = reinterpret_cast<std::uintptr_t>(code);
    probe.data = reinterpret_cast<std::uintptr_t>(data);
    const auto crossing = probe.data + probe.hostPage - 4;
    const auto publish = [&](bool dirty) {
        const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
            LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
        if (token == 0) return false;
        const bool owns = LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(token) == 1;
        if (owns) {
            Lumen::GuestCpu::MarkHandoffRange(probe.data, 2 * probe.hostPage);
            Lumen::GuestCpu::PublishMemoryHandoffGpuDirty(dirty);
        }
        return LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 1 && owns;
    };
    constexpr std::uint64_t value = UINT64_C(0x8123456789abcdef);
    std::memcpy(reinterpret_cast<void*>(crossing), &value, sizeof(value));
    std::uint64_t result {};
    unsigned passed {};
    passed += Check(FEXLumenGuestMemoryReadGuardProviderV1() == Lumen::GuestCpu::MemoryHandoffReadGuardDirectory(),
        "read_directory_typed_provider_configured");
    passed += Check(publish(true) && Run(probe, 0, crossing, 0, 0, 0, 0, &result) && result == value &&
        probe.accesses.size() == 1 && Covers(probe, crossing, 8, false), "read_directory_dirty_cross_page_exact");
    const bool cleanPublished = publish(false);
    probe.deny = true;
    const auto preservingBefore = probe.preserveAllowed + probe.preserveDenied;
    passed += Check(cleanPublished && Run(probe, 0, crossing, 0, 0, 0, 0, &result) && result == value &&
        probe.accesses.empty() && probe.preserveAllowed + probe.preserveDenied == preservingBefore,
        "read_directory_clean_same_translation_cross_page_no_callback");
    probe.deny = false;
    passed += Check(Run(probe, 32, crossing, 0, value, 0, 0, &result) && result == value &&
        probe.accesses.size() == 1 && probe.accesses[0].intent == MemoryAccessIntent::ReadWrite &&
        Covers(probe, crossing, 8, true), "read_directory_clean_same_value_store_still_checked");
    for (unsigned i = 0; i != 13; ++i) data[32 + i] = static_cast<std::uint8_t>(0x80 + i);
    LumenAndroidFexCoreGuestStateV1 finalState {};
    passed += Check(Run(probe, 64, probe.data + 96, probe.data + 32, 0, 13, 0, nullptr, &finalState) &&
        std::memcmp(data + 32, data + 96, 13) == 0 && probe.accesses.size() == 13 &&
        std::all_of(probe.accesses.begin(), probe.accesses.end(), [](const Access& access) {
            return Lumen::GuestCpu::WritesMemory(access.intent);
        }) && finalState.gprs[1] == 0 && finalState.gprs[6] == probe.data + 45 && finalState.gprs[7] == probe.data + 109,
        "read_directory_clean_REP_restores_scratch_write_intent");
    const auto denied = [&](std::uint64_t offset) {
        std::array<std::uint8_t, 16> before {};
        std::memcpy(before.data(), reinterpret_cast<const void*>(crossing - 4), before.size());
        probe.accesses.clear(); probe.deny = true;
        preserveDeniedSignalRip.store(0, std::memory_order_relaxed);
        LumenAndroidFexCoreGuestStateV1 state {};
        state.structSize = sizeof(state); state.abiVersion = 1;
        state.rip = probe.code + offset; state.gprs[0] = ~value;
        state.gprs[4] = probe.data + 4 * probe.hostPage - 8; state.gprs[7] = crossing;
        std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &probe.returnAddress, 8);
        const auto platform = NativePlatformRegister();
        const auto status = LumenAndroidFexCoreRunGuestV1(&state, HostCall, Executable, &probe);
        probe.deny = false;
        return status == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
            preserveDeniedSignalRip.load(std::memory_order_relaxed) == probe.code + offset &&
            NativePlatformRegister() == platform && probe.accesses.size() == 1 &&
            probe.accesses[0].address == crossing && probe.accesses[0].bytes == 8 &&
            Lumen::GuestCpu::WritesMemory(probe.accesses[0].intent) == (offset != 0) &&
            std::memcmp(reinterpret_cast<const void*>(crossing - 4), before.data(), before.size()) == 0;
    };
    passed += Check(denied(32), "read_directory_clean_denied_store_precise_RIP_unchanged_bytes");
    passed += Check(publish(true) && denied(0), "read_directory_republished_writer_denied_same_translation");
    Lumen::GuestCpu::PublishMemoryHandoffGpuDirty(true);
    probe.code = oldCode; probe.data = oldData;
    // Guard enrollment is process-lifetime; retain the fixture mappings.
    return passed;
}

unsigned IndexedPageBoundaryContracts(Probe& probe) {
    auto* code = static_cast<std::uint8_t*>(mmap(nullptr, probe.hostPage, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    auto* data = static_cast<std::uint8_t*>(mmap(nullptr, 6 * probe.hostPage, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (code == MAP_FAILED || data == MAP_FAILED) return 0;
    // Original indexed/displaced 64-bit store and reload. RCX=3 selects +48:
    // the lowering must use exactly that address for the page-end check.
    constexpr std::array<std::uint8_t, 11> program {
        0x48, 0x89, 0x44, 0xcf, 0x18, // mov [rdi+rcx*8+24], rax
        0x48, 0x8b, 0x44, 0xcf, 0x18, // mov rax, [rdi+rcx*8+24]
        0xc3};
    std::memcpy(code, program.data(), program.size());
    if (mprotect(code, probe.hostPage, PROT_READ) != 0) return 0;
    const auto oldCode = probe.code, oldData = probe.data;
    probe.code = reinterpret_cast<std::uintptr_t>(code);
    probe.data = reinterpret_cast<std::uintptr_t>(data);
    const auto boundary = probe.data + 2 * probe.hostPage;
    const auto last = boundary - 8, crossing = boundary - 4;
    constexpr std::uint64_t value = UINT64_C(0x1122334455667788);
    std::uint64_t result {};
    unsigned passed {};
    const auto exact = [&](std::uint64_t address) {
        std::uint64_t actual {};
        std::memcpy(&actual, reinterpret_cast<const void*>(address), sizeof(actual));
        return actual == value && result == value;
    };
    passed += Check(!Lumen::GuestCpu::NeedsHandoff(last, 12) &&
        Run(probe, 0, last - 48, 0, value, 3, 0, &result) && exact(last) && probe.accesses.empty(),
        "inline_indexed_displaced_exact_page_end_clean");
    passed += Check(Run(probe, 0, crossing - 48, 0, value, 3, 0, &result) &&
        exact(crossing) && probe.accesses.empty(), "inline_indexed_displaced_cross_page_clean");
    const auto enroll = [&](std::uint64_t address) {
        const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
            LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
        if (token == 0) return false;
        const bool owned = LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(token) == 1;
        if (owned) Lumen::GuestCpu::MarkHandoffRange(address, 4096);
        return LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 1 && owned;
    };
    const bool secondWatched = enroll(boundary);
    passed += Check(secondWatched && Run(probe, 0, last - 48, 0, value, 3, 0, &result) &&
        exact(last) && probe.accesses.empty(), "inline_exact_end_excludes_watched_next_page");
    passed += Check(secondWatched && Run(probe, 0, crossing - 48, 0, value, 3, 0, &result) &&
        exact(crossing) && probe.accesses.size() == 2 && Covers(probe, crossing, 8, true) &&
        Covers(probe, crossing, 8, false), "inline_indexed_second_page_guard_exact_footprints");
    std::array<std::uint8_t, 16> before {};
    std::memcpy(before.data(), reinterpret_cast<const void*>(crossing - 4), before.size());
    probe.accesses.clear(); probe.deny = true;
    preserveDeniedSignalRip.store(0, std::memory_order_relaxed);
    LumenAndroidFexCoreGuestStateV1 state {};
    state.structSize = sizeof(state); state.abiVersion = 1;
    state.rip = probe.code; state.gprs[0] = ~value; state.gprs[1] = 3;
    state.gprs[4] = probe.data + 4 * probe.hostPage - 8; state.gprs[7] = crossing - 48;
    std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &probe.returnAddress, 8);
    const auto platform = NativePlatformRegister();
    const auto status = LumenAndroidFexCoreRunGuestV1(&state, HostCall, Executable, &probe);
    probe.deny = false;
    passed += Check(secondWatched && status == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
        preserveDeniedSignalRip.load(std::memory_order_relaxed) == probe.code &&
        NativePlatformRegister() == platform && probe.accesses.size() == 1 &&
        probe.accesses[0].address == crossing && probe.accesses[0].bytes == 8 &&
        probe.accesses[0].intent == MemoryAccessIntent::ReadWrite &&
        std::memcmp(reinterpret_cast<const void*>(crossing - 4), before.data(), before.size()) == 0,
        "inline_second_page_denied_precise_rip_x18_no_store");
    const bool firstWatched = enroll(last & ~UINT64_C(4095));
    passed += Check(firstWatched && Run(probe, 0, last - 48, 0, value, 3, 0, &result) && exact(last) &&
        probe.accesses.size() == 2 && Covers(probe, last, 8, true) && Covers(probe, last, 8, false),
        "inline_indexed_first_page_guard_exact_footprints");
    probe.code = oldCode; probe.data = oldData;
    // Guards are permanent, so these mappings remain owned until process exit.
    return passed;
}
}

extern "C" void FEXLumenGuestMemoryBulkDecisionV1(std::uint64_t destination, std::uint64_t source,
    std::uint64_t count, std::uint64_t shape, std::uint32_t allowed) noexcept {
    static thread_local bool recursive {};
    if (active == nullptr || recursive) return;
    auto& probe = *active;
    allowed != 0 ? ++probe.bulkAllowed : ++probe.bulkDenied;
    if (probe.observePlatformRegister) {
        ++probe.nativeCallbacks;
        if (NativePlatformRegister() != probe.hostPlatformRegister) ++probe.platformMismatches;
    }
    if (allowed != 0 && probe.checkBulkBounds) {
        probe.checkBulkBounds = false;
        recursive = true;
        const auto check = FEXLumenGuestMemoryBulkProviderV1();
        probe.bulkBoundsValid &= check != nullptr && check(UINT64_MAX, source, 2, shape) == 0 &&
            check(destination, UINT64_MAX, 2, 0x101) == 0 && check(destination, source, UINT64_MAX, 8) == 0 &&
            check(destination, source, 0, shape) == 0 && check(destination, source, count, 0x201) == 0;
        recursive = false;
    }
    auto* pause = probe.bulkPause;
    if (allowed == 0 || pause == nullptr || destination != pause->destination) return;
    pause->entered.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (!pause->release.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    if (!pause->release.load(std::memory_order_acquire)) pause->valid.store(false, std::memory_order_release);
}

extern "C" void FEXLumenGuestMemoryBulkWriterArmedV1(std::uint64_t epoch) noexcept {
    // Called while the runtime owns its registry lock: observe atomically and
    // return immediately, without taking another lock or changing admission.
    if (epoch != 0 && active != nullptr && active->bulkPause != nullptr)
        active->bulkPause->armed.store(true, std::memory_order_release);
}


extern "C" void FEXLumenGuestMemoryPreserveAllObservedV1(std::uint32_t allowed) noexcept {
    if (active == nullptr) return;
    auto& probe = *active;
    allowed != 0 ? ++probe.preserveAllowed : ++probe.preserveDenied;
    if (!probe.clobberPreserveAll) return;
    std::uint64_t fpcr {}, fpsr {};
    asm volatile("mrs %0, fpcr\nmrs %1, fpsr" : "=r"(fpcr), "=r"(fpsr));
    if (probe.preserveStatusSamples++ == 0) { probe.preserveFpcr = fpcr; probe.preserveFpsr = fpsr; }
    else probe.preserveStatusRestored &= fpcr == probe.preserveFpcr && fpsr == probe.preserveFpsr;
    // Ordinary native ABI deliberately clobbers its caller-saved state. The
    // preserving wrapper must protect extra GP/SIMD values, and the inline
    // frame must protect flags/status. Never invent or write platform x18.
    const std::uint64_t truncateFpcr = 3U << 22, accruedFpsr = 0x0800009fU, nzcv = 0xa0000000U;
    asm volatile(
        "mov x0, xzr\nmov x1, xzr\nmov x2, xzr\nmov x3, xzr\nmov x4, xzr\nmov x5, xzr\n"
        "mov x6, xzr\nmov x7, xzr\nmov x8, xzr\nmov x9, xzr\nmov x10, xzr\nmov x11, xzr\n"
        "mov x12, xzr\nmov x13, xzr\nmov x14, xzr\nmov x15, xzr\nmov x16, xzr\nmov x17, xzr\n"
        "movi v0.16b, #0xcc\nmovi v1.16b, #0xcc\nmovi v2.16b, #0xcc\nmovi v3.16b, #0xcc\n"
        "movi v4.16b, #0xcc\nmovi v5.16b, #0xcc\nmovi v6.16b, #0xcc\nmovi v7.16b, #0xcc\n"
        "movi v8.16b, #0xcc\nmovi v9.16b, #0xcc\nmovi v10.16b, #0xcc\nmovi v11.16b, #0xcc\n"
        "movi v12.16b, #0xcc\nmovi v13.16b, #0xcc\nmovi v14.16b, #0xcc\nmovi v15.16b, #0xcc\n"
        "movi v16.16b, #0xcc\nmovi v17.16b, #0xcc\nmovi v18.16b, #0xcc\nmovi v19.16b, #0xcc\n"
        "movi v20.16b, #0xcc\nmovi v21.16b, #0xcc\nmovi v22.16b, #0xcc\nmovi v23.16b, #0xcc\n"
        "movi v24.16b, #0xcc\nmovi v25.16b, #0xcc\nmovi v26.16b, #0xcc\nmovi v27.16b, #0xcc\n"
        "movi v28.16b, #0xcc\nmovi v29.16b, #0xcc\nmovi v30.16b, #0xcc\nmovi v31.16b, #0xcc\n"
        "msr fpcr, %0\nmsr fpsr, %1\nmsr nzcv, %2"
        : : "r"(truncateFpcr), "r"(accruedFpsr), "r"(nzcv)
        : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15",
          "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "cc");
}

extern "C" void FEXLumenGuestMemoryHandoffDeniedSignalV1(std::uint64_t rip) noexcept {
    // Signal-context observation: one lock-free atomic store, no state change.
    preserveDeniedSignalRip.store(rip, std::memory_order_relaxed);
}

extern "C" void FEXLumenJitCompilationRestartedV1(std::uint32_t reason) noexcept {
    if (reason == 1) farCompilationRestarts.fetch_add(1, std::memory_order_relaxed);
    if (reason == 2) bufferCompilationRestarts.fetch_add(1, std::memory_order_relaxed);
}

int main() {
    const auto originalPlatformRegister = NativePlatformRegister();
    LumenAndroidFexCoreRuntimeStatusV1 runtime {};
    runtime.structSize = sizeof(runtime); runtime.abiVersion = 1;
    if (LumenAndroidFexCoreRuntimeProbeV1(&runtime) != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) return 1;
    // The Core adoption bridge must receive authority from the real FEX
    // provider, with no permission inferred from an arbitrary token value.
    bool ownerProof = LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(0) == 0;
    const auto exclusive = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
        LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
    ownerProof = ownerProof && exclusive != 0 &&
        LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(exclusive) == 1 &&
        LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(exclusive + 1) == 0;
    bool foreignDenied = false;
    std::thread foreign([&] {
        foreignDenied = LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(exclusive) == 0;
    });
    foreign.join();
    ownerProof = ownerProof && foreignDenied;
    LumenAndroidFexCoreEndGuestMemoryMutationV1(exclusive, 0, 0, 0);
    ownerProof = ownerProof && LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(exclusive) == 0;
    const auto additive = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
        LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_ADDITIVE_MAPPING_V2);
    ownerProof = ownerProof && additive != 0 &&
        LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(additive) == 0;
    LumenAndroidFexCoreEndGuestMemoryMutationV1(additive, 0, 0, 0);
    if (!Check(ownerProof, "exclusive_owner_authority_foreign_stale_additive_denied")) return 6;
    Probe probe;
    probe.hostPage = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    const bool fixedCode = std::getenv("LUMEN_FEX_MEMORY_HANDOFF_TEST_FIXED_CODE") != nullptr;
    void* codeHint = fixedCode ? reinterpret_cast<void*>(UINT64_C(0x700000000)) : nullptr;
    auto* code = static_cast<std::uint8_t*>(mmap(codeHint, probe.hostPage, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    auto* data = static_cast<std::uint8_t*>(mmap(nullptr, 6 * probe.hostPage, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (code == MAP_FAILED || data == MAP_FAILED) return 2;
    if (fixedCode && code != codeHint) return 2;
    probe.code = reinterpret_cast<std::uintptr_t>(code);
    probe.data = reinterpret_cast<std::uintptr_t>(data);
    std::printf("FEX_MEMORY_HANDOFF process=%u code=%p guards=%p fixed_code=%u instrumentation=%u\n",
        static_cast<unsigned>(getpid()), static_cast<void*>(code),
        static_cast<const void*>(Lumen::GuestCpu::MemoryHandoffGuardTable()), fixedCode ? 1u : 0u,
        LumenAndroidFexCoreMemoryHandoffInstrumentationReadyV1());
    probe.returnAddress = LumenAndroidFexCoreRootReturnAddressV1();
    // Scalar, CAS, SIMD, forward REP STOS/MOVS, backward REP MOVS, and
    // NZCV preservation across both the unguarded and watched paths.
    constexpr std::array<std::uint8_t, 4> scalar {0x48, 0x89, 0x07, 0xc3};
    constexpr std::array<std::uint8_t, 6> cas {0xf0, 0x48, 0x0f, 0xb1, 0x0f, 0xc3};
    constexpr std::array<std::uint8_t, 9> simd {0x66, 0x0f, 0xef, 0xc0, 0xf3, 0x0f, 0x7f, 0x07, 0xc3};
    constexpr std::array<std::uint8_t, 3> stos {0xf3, 0xaa, 0xc3};
    constexpr std::array<std::uint8_t, 3> movs {0xf3, 0xa4, 0xc3};
    constexpr std::array<std::uint8_t, 5> backwards {0xfd, 0xf3, 0xa4, 0xfc, 0xc3};
    constexpr std::array<std::uint8_t, 12> flags {0x31, 0xc9, 0x39, 0xc9, 0x48, 0x89, 0x07, 0x0f, 0x94, 0xc0, 0xc3, 0x90};
    constexpr std::array<std::uint8_t, 17> masked {
        0xc5, 0xfa, 0x6f, 0x0e, 0xc5, 0xf9, 0xef, 0xc0,
        0xc4, 0xe2, 0x71, 0x2e, 0x07, 0xc5, 0xf8, 0x77, 0xc3};
    constexpr std::array<std::uint8_t, 22> gather {
        0xc5, 0xfa, 0x6f, 0x0e, 0xc5, 0xe9, 0x76, 0xd2,
        0xc5, 0xf9, 0xef, 0xc0, 0xc4, 0xe2, 0x69, 0x92, 0x04, 0x8f,
        0xc5, 0xf8, 0x77, 0xc3};
    constexpr std::array<std::uint8_t, 12> simd256 {
        0xc5, 0xfd, 0xef, 0xc0, 0xc5, 0xfe, 0x7f, 0x07, 0xc5, 0xf8, 0x77, 0xc3};
    constexpr std::array<std::uint8_t, 5> extended {0xdb, 0x2e, 0xdb, 0x3f, 0xc3};
    constexpr std::array<std::uint8_t, 4> load {0x48, 0x8b, 0x07, 0xc3};
    constexpr std::array<std::uint8_t, 6> cas16 {0xf0, 0x48, 0x0f, 0xc7, 0x0f, 0xc3};
    constexpr std::array<std::uint8_t, 6> addZero {0xf0, 0x48, 0x83, 0x07, 0x00, 0xc3};
    constexpr std::array<std::uint8_t, 11> bulkFlags {
        0x39, 0xc0, 0xf3, 0xaa, 0x0f, 0x94, 0xc2, 0x0f, 0xb6, 0xc2, 0xc3};
    constexpr std::array<std::uint8_t, 4> movsq {0xf3, 0x48, 0xa5, 0xc3};
    constexpr std::array<std::uint8_t, 4> stosq {0xf3, 0x48, 0xab, 0xc3};
    std::memcpy(code, scalar.data(), scalar.size());
    std::memcpy(code + 64, cas.data(), cas.size());
    std::memcpy(code + 128, simd.data(), simd.size());
    std::memcpy(code + 192, stos.data(), stos.size());
    std::memcpy(code + 256, movs.data(), movs.size());
    std::memcpy(code + 320, backwards.data(), backwards.size());
    std::memcpy(code + 384, flags.data(), flags.size());
    std::memcpy(code + 448, masked.data(), masked.size());
    std::memcpy(code + 512, gather.data(), gather.size());
    std::memcpy(code + 576, simd256.data(), simd256.size());
    std::memcpy(code + 640, extended.data(), extended.size());
    std::memcpy(code + 704, load.data(), load.size());
    std::memcpy(code + 768, cas16.data(), cas16.size());
    std::memcpy(code + 832, addZero.data(), addZero.size());
    std::memcpy(code + 896, bulkFlags.data(), bulkFlags.size());
    std::memcpy(code + 960, movsq.data(), movsq.size());
    std::memcpy(code + 1000, stosq.data(), stosq.size());
    const auto platformPressure = PlatformPressureProgram();
    if (platformPressure.size() > probe.hostPage - 1024) return 2;
    std::memcpy(code + 1024, platformPressure.data(), platformPressure.size());
    // Original guest virtual call: the callee deliberately clobbers volatile
    // R8, then the caller reloads its table pointer from an independent owner.
    // The indexed load/store must use that reloaded pointer in both guard paths.
    const std::vector<std::uint8_t> virtualReload {
        0x41, 0x55,                         // push r13
        0x41, 0x56,                         // push r14
        0x49, 0x89, 0xfd,                   // mov r13, rdi (object)
        0x49, 0x89, 0xce,                   // mov r14, rcx (index)
        0x4c, 0x8b, 0x07,                   // mov r8, [rdi] (vtable)
        0x41, 0xff, 0x50, 0x18,             // call [r8+0x18]
        0x0f, 0xb6, 0xc0,                   // movzx eax, al
        0x8d, 0x14, 0x00,                   // lea edx, [rax+rax]
        0x4d, 0x8b, 0x45, 0x38,             // mov r8, [r13+0x38] (table)
        0x43, 0x0b, 0x14, 0xb0,             // or edx, [r8+r14*4]
        0x81, 0xca, 0x00, 0x00, 0x00, 0x20, // or edx, 0x20000000
        0x43, 0x89, 0x14, 0xb0,             // mov [r8+r14*4], edx
        0x89, 0xd0,                         // mov eax, edx (exact result)
        0x41, 0x5e,                         // pop r14
        0x41, 0x5d,                         // pop r13
        0xc3                                // ret
    };
    constexpr std::array<std::uint8_t, 9> virtualCallee {
        0x41, 0xb8, 0x04, 0x00, 0x00, 0x00, // mov r8d, 4
        0xb0, 0x01,                         // mov al, 1
        0xc3                                // ret
    };
    constexpr std::uint64_t VirtualCallerOffset = 2048, VirtualCalleeOffset = 2112;
    if (virtualReload.size() > VirtualCalleeOffset - VirtualCallerOffset ||
        VirtualCalleeOffset + virtualCallee.size() > probe.hostPage) return 2;
    std::memcpy(code + VirtualCallerOffset, virtualReload.data(), virtualReload.size());
    std::memcpy(code + VirtualCalleeOffset, virtualCallee.data(), virtualCallee.size());
    constexpr std::uint64_t ConcurrentCallerOffset = 2304;
    const auto concurrentReload = ConcurrentVirtualReloadProgram();
    if (ConcurrentCallerOffset + concurrentReload.size() > probe.hostPage) return 2;
    std::memcpy(code + ConcurrentCallerOffset, concurrentReload.data(), concurrentReload.size());
    if (mprotect(code, probe.hostPage, PROT_READ) != 0) return 3;
    active = &probe;
    if (!Lumen::GuestCpu::RegisterMemoryHandoffQuery(Handoff)) return 4;
    unsigned passed {};
    passed += PreserveAllContracts(probe);
    passed += UnwatchedBulkContracts(probe);
    passed += IndexedPageBoundaryContracts(probe);
    passed += ReadDirectoryContracts(probe);
    std::uint64_t value = UINT64_C(0x123456789abcdef0), result {};
    // Execute a translation while no data page is watched. Then guard a page
    // and reuse precisely that cached block: ownership changes require no JIT
    // replacement and must not be compile-time latched away.
    passed += Check(Run(probe, 0, probe.data, 0, value, 0, 0) && probe.accesses.empty(), "unguarded_inline");
    Lumen::GuestCpu::MarkHandoffRange(probe.data, 4096);
    passed += Check(Run(probe, 0, probe.data, 0, value, 0, 0) &&
        Covers(probe, probe.data, 8, true) && std::memcmp(data, &value, 8) == 0, "same_value_cached_scalar");
    passed += Check(Run(probe, 64, probe.data, 0, value + 1, 0, value + 2) &&
        Covers(probe, probe.data, 8, true) && std::memcmp(data, &value, 8) == 0, "failed_cas_write_intent");
    passed += Check(Run(probe, 64, probe.data, 0, value, 0, value) &&
        Covers(probe, probe.data, 8, true) && std::memcmp(data, &value, 8) == 0, "same_value_atomic");
    std::memset(data + 32, 0, 16);
    passed += Check(Run(probe, 128, probe.data + 32, 0, 0, 0, 0) &&
        Covers(probe, probe.data + 32, 16, true), "same_value_simd128");
    passed += Check(Run(probe, 192, probe.data + 64, 0, 0x55, 13, 0) &&
        Covers(probe, probe.data + 64, 13, true) &&
        std::all_of(data + 64, data + 77, [](auto byte) { return byte == 0x55; }), "rep_stos");
    for (unsigned index = 0; index < 13; ++index) data[96 + index] = static_cast<std::uint8_t>(index + 1);
    passed += Check(Run(probe, 256, probe.data + 128, probe.data + 96, 0, 13, 0) &&
        Covers(probe, probe.data + 96, 13, false) && Covers(probe, probe.data + 128, 13, true) &&
        std::memcmp(data + 96, data + 128, 13) == 0, "rep_movs");
    passed += Check(Run(probe, 320, probe.data + 172, probe.data + 108, 0, 13, 0) &&
        Covers(probe, probe.data + 96, 13, false) && Covers(probe, probe.data + 160, 13, true) &&
        std::memcmp(data + 96, data + 160, 13) == 0, "rep_movs_backward");
    const auto crossing = probe.data + 8192 - 4;
    Lumen::GuestCpu::MarkHandoffRange(probe.data + 8192, 4096);
    passed += Check(Run(probe, 0, crossing, 0, value, 0, 0) &&
        Covers(probe, crossing, 8, true) && std::memcmp(data + 8192 - 4, &value, 8) == 0,
        "unaligned_cross_page_second_guard");
    passed += Check(Run(probe, 384, probe.data + 3 * probe.hostPage, 0, 0, 0, 0, &result) &&
        (result & 255U) == 1, "unguarded_nzcv_preserved");
    passed += Check(Run(probe, 384, probe.data + 192, 0, 0, 0, 0, &result) &&
        (result & 255U) == 1 && Covers(probe, probe.data + 192, 8, true), "watched_nzcv_preserved");
    passed += Check(Run(probe, 192, probe.data + 64, 0, 0x55, 0, 0) &&
        probe.accesses.empty(), "rep_zero_count_no_intent");
    const auto auxiliary = probe.data + 3 * probe.hostPage + 256;
    const std::array<std::uint32_t, 4> mask {0x80000000U, 0, 0x80000000U, 0};
    std::memcpy(reinterpret_cast<void*>(auxiliary), mask.data(), sizeof(mask));
    std::memset(data + 224, 0x77, 16);
    passed += Check(Run(probe, 448, probe.data + 224, auxiliary, 0, 0, 0) &&
        Covers(probe, probe.data + 224, 4, true) && Covers(probe, probe.data + 232, 4, true) &&
        std::all_of(probe.accesses.begin(), probe.accesses.end(), [&](const Access& access) {
            return access.bytes == 4 && (access.address == probe.data + 224 || access.address == probe.data + 232);
        }) && data[228] == 0x77 && data[236] == 0x77, "simd_mask_active_lanes_only");
    const std::array<std::uint32_t, 4> indexes {0, 3, 7, 9};
    std::memcpy(reinterpret_cast<void*>(auxiliary), indexes.data(), sizeof(indexes));
    bool gathered = Run(probe, 512, probe.data + 272, auxiliary, 0, 0, 0);
    for (auto index : indexes) gathered &= Covers(probe, probe.data + 272 + 4 * index, 4, false);
    passed += Check(gathered, "simd_gather_lane_addresses");
    std::memset(data + 320, 0, 32);
    passed += Check(Run(probe, 576, probe.data + 320, 0, 0, 0, 0) &&
        Covers(probe, probe.data + 320, 32, true), "simd256_split_preflight");
    const std::array<std::uint8_t, 10> one80 {0, 0, 0, 0, 0, 0, 0, 0x80, 0xff, 0x3f};
    std::memcpy(reinterpret_cast<void*>(auxiliary), one80.data(), one80.size());
    passed += Check(Run(probe, 640, probe.data + 360, auxiliary, 0, 0, 0) &&
        Covers(probe, probe.data + 360, 10, true) &&
        std::memcmp(data + 360, one80.data(), one80.size()) == 0, "x87_extended80_exact_footprint");
    std::memcpy(data + 400, &value, 8);
    passed += Check(Run(probe, 704, probe.data + 400, 0, 0, 0, 0, &result) &&
        result == value && Covers(probe, probe.data + 400, 8, false), "scalar_read_intent");
    std::memset(data + 448, 0, 16);
    passed += Check(Run(probe, 768, probe.data + 448, 0, 0, 0, 0) &&
        Covers(probe, probe.data + 448, 16, true), "same_value_atomic_pair");
    passed += Check(Run(probe, 832, probe.data + 448, 0, 0, 0, 0) &&
        Covers(probe, probe.data + 448, 8, true), "same_value_atomic_add_zero");
    const auto pressureData = probe.data + 1024;
    std::memset(reinterpret_cast<void*>(pressureData), 0, 512);
    for (std::uint64_t index = 0; index != 8; ++index) {
        const auto seed = UINT64_C(0x123456789abcdef0) + index;
        std::memcpy(reinterpret_cast<void*>(pressureData + 8 * index), &seed, sizeof(seed));
    }
    probe.accesses.reserve(1024);
    probe.hostPlatformRegister = originalPlatformRegister;
    probe.observePlatformRegister = true;
    const bool pressureRan = Run(probe, 1024, pressureData, 0, 0, 0, 0, &result);
    probe.observePlatformRegister = false;
    const auto afterPlatformRegister = NativePlatformRegister();
    std::printf("FEX_PLATFORM_REGISTER native_x18=%016llx callbacks=%llu mismatches=%llu first_mismatch=%016llx after=%016llx guest_result=%llu\n",
        static_cast<unsigned long long>(originalPlatformRegister),
        static_cast<unsigned long long>(probe.nativeCallbacks),
        static_cast<unsigned long long>(probe.platformMismatches),
        static_cast<unsigned long long>(probe.firstPlatformMismatch),
        static_cast<unsigned long long>(afterPlatformRegister),
        static_cast<unsigned long long>(result));
    passed += Check(pressureRan && result == 0x18 && probe.nativeCallbacks != 0 &&
        probe.platformMismatches == 0 && afterPlatformRegister == originalPlatformRegister,
        "native_x18_preserved_under_watched_rmw_pressure");
    // This host page has not been watched by any earlier case. Retain object,
    // vtable, table and code addresses while enabling its guard between runs.
    const auto virtualData = probe.data + probe.hostPage;
    const auto virtualObject = virtualData + 128;
    const auto virtualVtable = virtualData + 256;
    const auto virtualTable = virtualData + 512;
    const auto virtualTarget = probe.code + VirtualCalleeOffset;
    constexpr std::uint64_t VirtualIndex = 7;
    std::array<std::uint32_t, 16> originalTable {}, expectedTable {}, actualTable {};
    for (std::size_t index = 0; index < originalTable.size(); ++index)
        originalTable[index] = UINT32_C(0x0055aa00) + (static_cast<std::uint32_t>(index) << 8);
    expectedTable = originalTable;
    expectedTable[VirtualIndex] |= UINT32_C(0x20000002);
    std::memcpy(reinterpret_cast<void*>(virtualObject), &virtualVtable, sizeof(virtualVtable));
    std::memcpy(reinterpret_cast<void*>(virtualObject + 0x38), &virtualTable, sizeof(virtualTable));
    std::memcpy(reinterpret_cast<void*>(virtualVtable + 0x18), &virtualTarget, sizeof(virtualTarget));
    const auto tableResultCorrect = [&] {
        std::memcpy(actualTable.data(), reinterpret_cast<const void*>(virtualTable), sizeof(actualTable));
        std::uint64_t ownerField {}, callTarget {};
        std::memcpy(&ownerField, reinterpret_cast<const void*>(virtualObject + 0x38), sizeof(ownerField));
        std::memcpy(&callTarget, reinterpret_cast<const void*>(virtualVtable + 0x18), sizeof(callTarget));
        return result == expectedTable[VirtualIndex] && actualTable == expectedTable &&
            ownerField == virtualTable && callTarget == virtualTarget;
    };
    std::memcpy(reinterpret_cast<void*>(virtualTable), originalTable.data(), sizeof(originalTable));
    const bool unguardedReload = Run(probe, VirtualCallerOffset, virtualObject, 0, 0, VirtualIndex, 0, &result) &&
        probe.accesses.empty() && tableResultCorrect();
    const auto unguardedResult = result;
    Lumen::GuestCpu::MarkHandoffRange(virtualData, 4096);
    std::memcpy(reinterpret_cast<void*>(virtualTable), originalTable.data(), sizeof(originalTable));
    const bool watchedReload = Run(probe, VirtualCallerOffset, virtualObject, 0, 0, VirtualIndex, 0, &result) &&
        tableResultCorrect() && Covers(probe, virtualObject, 8, false) &&
        Covers(probe, virtualVtable + 0x18, 8, false) && Covers(probe, virtualObject + 0x38, 8, false) &&
        Covers(probe, virtualTable + 4 * VirtualIndex, 4, false) &&
        Covers(probe, virtualTable + 4 * VirtualIndex, 4, true);
    const bool exactReloadTable = tableResultCorrect();
    std::printf("FEX_VIRTUAL_RELOAD guard0=%u guard1=%u field_offset=0x38 index=%llu"
        " expected=%08x guard0_result=%08llx guard1_result=%08llx watched_callbacks=%zu"
        " table_exact=%u same_program=1 callee_r8=4\n",
        unguardedReload ? 1u : 0u, watchedReload ? 1u : 0u,
        static_cast<unsigned long long>(VirtualIndex), expectedTable[VirtualIndex],
        static_cast<unsigned long long>(unguardedResult), static_cast<unsigned long long>(result), probe.accesses.size(),
        exactReloadTable ? 1u : 0u);
    passed += Check(unguardedReload && watchedReload, "virtual_call_r8_reload_indexed_or_cached_guard_paths");
    const auto concurrentData = probe.data + 4 * probe.hostPage;
    const auto concurrentObject = concurrentData + 128, concurrentVtable = concurrentData + 256;
    const auto concurrentTable = concurrentData + 512;
    auto* concurrentControl = new (reinterpret_cast<void*>(concurrentData + 1024)) ConcurrentControl;
    std::memcpy(reinterpret_cast<void*>(concurrentObject), &concurrentVtable, sizeof(concurrentVtable));
    std::memcpy(reinterpret_cast<void*>(concurrentObject + 0x38), &concurrentTable, sizeof(concurrentTable));
    std::memcpy(reinterpret_cast<void*>(concurrentVtable + 0x18), &virtualTarget, sizeof(virtualTarget));
    passed += Check(ConcurrentVirtualReload(probe, ConcurrentCallerOffset, concurrentObject, concurrentTable,
        *concurrentControl, false), "virtual_call_reload_far_restart_exclusive_writer_unguarded");
    Lumen::GuestCpu::MarkHandoffRange(concurrentData, probe.hostPage);
    passed += Check(ConcurrentVirtualReload(probe, ConcurrentCallerOffset, concurrentObject, concurrentTable,
        *concurrentControl, true), "virtual_call_reload_far_restart_exclusive_writer_watched");
    passed += Check(ConcurrentVirtualReload(probe, ConcurrentCallerOffset, concurrentObject, concurrentTable,
        *concurrentControl, true, true), "read_directory_concurrent_exclusive_publish_readmission");
    passed += Check(RestartMetadataInvariant(probe.hostPage), "discarded_emission_metadata_monotonic_nonoverlapping");
    std::printf("FEX_MEMORY_HANDOFF cases=%u/56 host_page=%llu far_restarts=%u buffer_restarts=%u\n", passed,
        static_cast<unsigned long long>(probe.hostPage), farCompilationRestarts.load(), bufferCompilationRestarts.load());
    // Process exit retires these native fixtures; no surviving CPU/GPU owner.
    return passed == 56 ? 0 : 5;
}
