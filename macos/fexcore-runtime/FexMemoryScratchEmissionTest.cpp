// Lumen, 2026-10-11. SPDX-License-Identifier: GPL-2.0-only
// Compile original guest instructions, never execute the synthetic callbacks.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "Interface/Context/Context.h"
#include "Interface/Core/JIT/DebugData.h"
#include "Interface/Core/CPUBackend.h"
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/LumenMemoryHandoffAbi.h>
#include <rpmalloc/rpmalloc.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <set>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace {
bool preserving;
alignas(64) std::array<std::uint8_t, 1U << 20> guards {};
std::atomic<const std::uint8_t*> directory {guards.data()};
std::uint32_t Ordinary(std::uint64_t, std::uint64_t, std::uint32_t, std::uint64_t) noexcept { return 1; }
FEX_LUMEN_MEMORY_PRESERVE_ALL_ABI std::uint32_t Preserving(
    std::uint64_t, std::uint64_t, std::uint32_t, std::uint64_t) noexcept { return 1; }

class CompileOnlySyscalls final: public FEXCore::HLE::SyscallHandler {
public:
    CompileOnlySyscalls(std::uint64_t base, std::uint64_t bytes): base_(base), bytes_(bytes) {
        OSABI = FEXCore::HLE::SyscallOSABI::OS_GENERIC;
    }
    std::uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame*, FEXCore::HLE::SyscallArguments*) override { return 0; }
    FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(
        FEXCore::Core::InternalThreadState*, std::uint64_t address) override {
        return address >= base_ && address - base_ < bytes_ ?
            FEXCore::HLE::ExecutableRangeInfo {base_, bytes_, false} : FEXCore::HLE::ExecutableRangeInfo {0, 0, false};
    }
    std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(
        FEXCore::Core::InternalThreadState*, std::uint64_t) override { return std::nullopt; }
private:
    std::uint64_t base_, bytes_;
};

// Verify the actual short branches in the 96-access fixture, not a source
// estimate: count retired instructions when each guard outcome is selected.
// The callback's target must be the compact frame, excluding data literals
// and unrelated generated loads that happen to use the same register.
bool RetainedAddressPaths(const std::uint8_t* code, std::size_t bytes) {
    const auto word = [&](std::size_t offset) {
        std::uint32_t value {};
        if (offset + 4 <= bytes) std::memcpy(&value, code + offset, 4);
        return value;
    };
    const auto target = [&](std::size_t offset, unsigned bits) {
        const auto mask = (UINT32_C(1) << bits) - 1;
        const auto encoded = (word(offset) >> 5) & mask;
        const auto signedOffset = static_cast<std::int64_t>(encoded) -
            ((encoded & (UINT32_C(1) << (bits - 1))) ? (INT64_C(1) << bits) : 0);
        return static_cast<std::int64_t>(offset) + 4 * signedOffset;
    };
    unsigned reads {}, writes {};
    bool valid = true;
    for (std::size_t start = 0; start + 72 <= bytes; start += 4) {
        if ((word(start) & UINT32_C(0xff00001f)) != UINT32_C(0x58000002)) continue; // ldr x2,literal
        const bool read = word(start + 4) == UINT32_C(0xc8dffc42); // ldar x2,[x2]
        const auto probe = start + (read ? 16 : 8); // one MOV for [rdi]/[rsi]
        if (word(probe) != UINT32_C(0xd34c7c20) || // ubfx x0,x1,#12,#20
            word(probe + 4) != UINT32_C(0x8b000040) || // add x0,x2,x0
            word(probe + 8) != UINT32_C(0x08dffc00) || // ldarb w0,[x0]
            (word(probe + 12) & UINT32_C(0xff00001f)) != UINT32_C(0x35000000)) continue;
        const auto slow = target(probe + 12, 19);
        if (slow < 0 || word(static_cast<std::size_t>(slow)) != UINT32_C(0xd10103ff)) continue;
        const auto pageBranch = probe + 24;
        const auto done = target(pageBranch, 14);
        valid &= (word(pageBranch) & UINT32_C(0xfff8001f)) == UINT32_C(0x36600000); // tbz x0,#12
        valid &= done > slow && static_cast<std::uint64_t>(done) < bytes;
        valid &= word(pageBranch + 8) == UINT32_C(0xd34c7c00); // second page: ubfx x0,x0,#12,#20
        valid &= target(pageBranch + 20, 19) == slow;
        const auto finish = pageBranch + 24;
        valid &= (word(finish) & UINT32_C(0xfc000000)) == UINT32_C(0x14000000);
        const auto branch = word(finish) & UINT32_C(0x03ffffff);
        valid &= static_cast<std::int64_t>(finish) + 4 * static_cast<std::int64_t>(branch) == done;
        if (read) {
            ++reads;
            valid &= (word(start + 8) & UINT32_C(0xff00001f)) == UINT32_C(0xb4000002);
            valid &= target(start + 8, 19) == done; // clean directory exits in exactly three instructions
        } else ++writes;
        valid &= (pageBranch - start) / 4 + 1 == (read ? 11 : 9);
        valid &= (finish - start) / 4 + 1 == (read ? 17 : 15);
    }
    std::printf("%s memory_guard_paths reads=%u writes=%u clean_read=3 read_same=11 write_same=9"
        " read_cross=17 write_cross=15 guest_execution=0\n", valid && reads == 48 && writes == 48 ? "PASS" : "FAIL", reads, writes);
    return valid && reads == 48 && writes == 48;
}

bool Compile(const FEXCore::HostFeatures& features, const std::vector<std::uint8_t>& program,
    const char* name, unsigned ordinaryAccesses, bool baseline) {
    const auto page = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    auto* code = static_cast<std::uint8_t*>(mmap(nullptr, page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (code == MAP_FAILED || program.size() > page) return false;
    std::memcpy(code, program.data(), program.size());
    if (mprotect(code, page, PROT_READ) != 0) { munmap(code, page); return false; }
    const auto base = reinterpret_cast<std::uintptr_t>(code);
#ifdef __APPLE__
    FEXCore::Allocator::ScopedJITWriteProtection write;
#endif
    FEXCore::SignalDelegator signals;
    CompileOnlySyscalls syscalls(base, page);
    auto context = FEXCore::Context::Context::CreateNewContext(features);
    if (!context) { munmap(code, page); return false; }
    auto* impl = static_cast<FEXCore::Context::ContextImpl*>(context.get());
    context->SetSignalDelegator(&signals);
    context->SetSyscallHandler(&syscalls);
    if (!context->InitCore()) { munmap(code, page); return false; }
    auto* thread = context->CreateThread();
    if (!thread) { munmap(code, page); return false; }
    auto& state = thread->CurrentFrame->State;
    state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = state.private_gdt;
    state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = state.private_gdt;
    state.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
    auto* segment = FEXCore::Core::CPUState::GetSegmentFromIndex(state, state.cs_idx);
    FEXCore::Core::CPUState::SetGDTBase(segment, 0);
    FEXCore::Core::CPUState::SetGDTLimit(segment, 0xf'ffffU);
    segment->L = 1; segment->D = 0;
    state.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*segment);
    FEXCore::Allocator::RegisterTLSData(thread);
    const auto compiled = impl->CompileCode(thread, base);
    const auto bytes = compiled.CompiledCode.BlockBegin != nullptr ? compiled.CompiledCode.Size : 0;
    state.InlineJITBlockHeader = reinterpret_cast<std::uintptr_t>(compiled.CompiledCode.BlockBegin);
    state.rip = 0; // Never accept RestoreRIP's fallback as a successful map.
    unsigned compact {}, full {}, traps {};
    std::set<std::uint64_t> trapRips;
    bool exact = bytes != 0;
    for (std::size_t offset = 0; offset + 4 <= bytes; offset += 4) {
        std::uint32_t word {};
        std::memcpy(&word, compiled.CompiledCode.BlockBegin + offset, 4);
        compact += word == UINT32_C(0xd10103ff); // sub sp,sp,#64, shared-thunk caller
        full += word == UINT32_C(0xd10283ff); // sub sp,sp,#160
        if (word != (UINT32_C(0xd4200000) | (UINT32_C(0x4c4d) << 5))) continue;
        ++traps;
        const auto rip = context->RestoreRIPFromHostPC(thread,
            reinterpret_cast<std::uintptr_t>(compiled.CompiledCode.BlockBegin + offset));
        exact &= rip >= base && rip - base < program.size();
        trapRips.insert(rip);
    }
    bool shared = true;
    if (!baseline && bytes != 0) {
        using Backend = FEXCore::CPU::CPUBackend;
        const auto* header = reinterpret_cast<const Backend::JITCodeHeader*>(compiled.CompiledCode.BlockBegin);
        auto* tail = reinterpret_cast<Backend::JITCodeTail*>(compiled.CompiledCode.BlockBegin + header->OffsetToBlockTail);
        const auto begin = tail->LumenMemoryThunkOffset;
        const auto size = tail->LumenMemoryThunkSize;
        shared = (compact != 0) == (size != 0);
        if (size != 0) {
            shared &= begin >= sizeof(*header) && begin < header->OffsetToBlockTail &&
                size <= header->OffsetToBlockTail - begin;
            const auto host = reinterpret_cast<std::uintptr_t>(compiled.CompiledCode.BlockBegin);
            // Two different callers must retain their own RIP at every PC in
            // the shared body. Adjacent code keeps its static instruction map.
            shared &= trapRips.size() >= 2;
            for (const auto caller : {*trapRips.begin(), *trapRips.rbegin()}) {
                state.rip = caller;
                for (std::uint64_t off = begin; off < std::uint64_t(begin) + size; off += 4)
                    shared &= context->RestoreRIPFromHostPC(thread, host + off) == caller;
            }
            constexpr std::uint64_t sentinel = UINT64_C(0xfedcba9876543210);
            state.rip = sentinel;
            shared &= context->RestoreRIPFromHostPC(thread, host + begin - 4) != sentinel;
            shared &= context->RestoreRIPFromHostPC(thread, host + begin + size) != sentinel;
            // Corrupt or disabled range metadata must not widen the dynamic
            // interpretation to other code or the block's metadata tail.
            tail->LumenMemoryThunkSize = 0;
            shared &= context->RestoreRIPFromHostPC(thread, host + begin) != sentinel;
            tail->LumenMemoryThunkSize = UINT32_MAX;
            shared &= context->RestoreRIPFromHostPC(thread, host + begin) != sentinel;
            tail->LumenMemoryThunkOffset = 0;
            tail->LumenMemoryThunkSize = begin + size;
            shared &= context->RestoreRIPFromHostPC(thread, host + begin) != sentinel;
            tail->LumenMemoryThunkOffset = begin + 1;
            tail->LumenMemoryThunkSize = size;
            shared &= context->RestoreRIPFromHostPC(thread, host + begin + 4) != sentinel;
            tail->LumenMemoryThunkOffset = header->OffsetToBlockTail;
            tail->LumenMemoryThunkSize = 4;
            shared &= context->RestoreRIPFromHostPC(thread, host + header->OffsetToBlockTail) != sentinel;
            tail->LumenMemoryThunkOffset = begin;
            tail->LumenMemoryThunkSize = size;
        }
        std::printf("%s memory_shared_thunk callers=%u offset=%u bytes=%u dynamic_rip=two_callers"
            " boundaries=before/inside/after malformed_range=reject guest_execution=0\n",
            shared ? "PASS" : "FAIL", compact, begin, size);
    }
    const bool shape = baseline ? compact == 0 && full > ordinaryAccesses :
        compact >= ordinaryAccesses && (ordinaryAccesses != 0 || compact == 0) && full != 0;
    const bool paths = ordinaryAccesses != 96 || baseline || RetainedAddressPaths(compiled.CompiledCode.BlockBegin, bytes);
    const bool pass = exact && shape && paths && shared && traps >= ordinaryAccesses + 1 &&
        (ordinaryAccesses != 96 || trapRips.size() == 97);
    std::printf("%s memory_scratch_emission program=%s abi=%s sve256=%u tso=%u bytes=%llu"
        " compact_frames=%u full_frames=%u local_traps=%u distinct_rips=%zu host_page=%llu"
        " guest_execution=0 fps_attribution=0 baseline=%u\n", pass ? "PASS" : "FAIL", name,
        preserving ? "preserve_all" : "ordinary", features.SupportsSVE256 ? 1U : 0U,
        impl->IsAtomicTSOEnabled() ? 1U : 0U,
        static_cast<unsigned long long>(bytes), compact, full, traps, trapRips.size(),
        static_cast<unsigned long long>(page), baseline ? 1U : 0U);
    FEXCore::Allocator::UninstallTLSData(thread);
    context->DestroyThread(thread);
    context.reset();
    munmap(code, page);
    return pass;
}
} // namespace

extern "C" FEXLumenMemoryCheckV1 FEXLumenGuestMemoryCheckProviderV1() noexcept { return Ordinary; }
extern "C" const std::uint8_t* FEXLumenGuestMemoryHandoffGuardProviderV1() noexcept { return guards.data(); }
extern "C" FEXLumenMemoryPreserveAllCheckV1 FEXLumenGuestMemoryPreserveAllProviderV1() noexcept {
    return preserving ? Preserving : nullptr;
}
extern "C" FEXLumenMemoryReadGuardDirectoryV1 FEXLumenGuestMemoryReadGuardProviderV1() noexcept { return &directory; }
using BulkCheck = std::uint32_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t) noexcept;
extern "C" BulkCheck FEXLumenGuestMemoryBulkProviderV1() noexcept { return nullptr; }
extern "C" const std::uint32_t* FEXLumenGuestProgressFlagProviderV1() noexcept { return nullptr; }
extern "C" auto FEXLumenGuestProgressCallbackProviderV1() noexcept -> void (*)() noexcept { return nullptr; }

int main(int argc, char** argv) {
    const bool baseline = argc == 2 && std::strcmp(argv[1], "--baseline") == 0;
    if (argc != 1 && !baseline) return 1;
    if (rpmalloc_initialize(nullptr) != 0) return 1;
    FEXCore::Allocator::InitializeThread();
    FEXCore::Config::Initialize(); FEXCore::Config::Load();
    FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLETELEMETRY, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_MULTIBLOCK, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_MAXINST, "5000");
    std::vector<std::uint8_t> scalar {0x66,0x0f,0xef,0xc0};
    const std::array<std::uint8_t,14> accesses {0x48,0x8b,0x07,0xf3,0x0f,0x6f,0x0e,0x48,0x89,0x07,0xf3,0x0f,0x7f,0x06};
    for (unsigned i = 0; i != 24; ++i) scalar.insert(scalar.end(), accesses.begin(), accesses.end());
    scalar.push_back(0xc3);
    bool pass = true;
    for (bool tso: {false, true}) {
      FEXCore::Config::Set(FEXCore::Config::CONFIG_TSOENABLED, tso ? "1" : "0");
      for (bool sve: {false, true}) {
        FEXCore::HostFeatures features {};
        features.DCacheLineLog2 = 4; features.CPUMIDRs.push_back(0);
        features.SupportsAVX = true;
        features.SupportsSVE128 = sve; features.SupportsSVE256 = sve;
        for (bool preserve: {false, true}) {
            preserving = preserve;
            pass = Compile(features, scalar, "scalar_simd_96", 96, baseline) && pass;
            pass = Compile(features, {0x48,0x8b,0x44,0x8f,0x08,0x48,0x89,0x44,0x8e,0x10,0xc3},
                "scaled_register_offset", 2, baseline) && pass;
            pass = Compile(features, {0xf0,0x48,0x0f,0xb1,0x0f,0xc3}, "locked_cmpxchg", 0, baseline) && pass;
            pass = Compile(features, {0xfc,0xf3,0xa4,0xc3}, "rep_movsb", 0, baseline) && pass;
            pass = Compile(features, {0xc4,0xe2,0x71,0x2e,0x07,0xc3}, "masked_store", 0, baseline) && pass;
        }
      }
    }
    FEXCore::Config::Shutdown();
    return pass ? 0 : 3;
}
