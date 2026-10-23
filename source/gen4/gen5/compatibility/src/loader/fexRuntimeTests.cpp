// Lumen, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#include "loader/guestExecution.h"

#if defined(LUMEN_PS5_FEXCORE)
#include "GuestCpuRuntime.h"
#include "MemoryHandoff.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "kernel/semaphore.h"
#include "kernel/eventQueue.h"
#include "kernel/eventFlag.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/agc.h"
#include "libs/network.h"
#include "libs/audio_internal.h"
#include "loader/hostCall.h"
#include "loader/hostCallShaderRegister.h"
#include "loader/hostObject.h"
#include "loader/guestMutationScope.h"
#include "loader/guestWriterGate.h"
#include "loader/runtimeLinker.h"
#include "loader/unresolvedImportFallback.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.h"
#include "graphics/presentation/videoOut.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <thread>
#include <vector>
#include <xxhash.h>

namespace Libs::Posix {
int KYTY_SYSV_ABI shutdown(int s, int how);
int KYTY_SYSV_ABI fcntl(int d, int command, uint64_t argument);
void InitLibKernel_1_Posix(Loader::SymbolDatabase* symbols);
}
namespace Libs {
void InitLibKernel_1_Pthread(Loader::SymbolDatabase* symbols);
}
namespace Libs::LibKernel {
bool SanitizerReplacementObjectsSelfTest(std::uint64_t stack_top);
}
namespace Libs::LibcInternalExt {
bool HeapTraceObjectsSelfTest(std::uint64_t stack_top);
}

namespace Libs::LibKernel {
int KYTY_SYSV_ABI KernelInstallExceptionHandler(int signum, void* handler);
int KYTY_SYSV_ABI KernelRemoveExceptionHandler(int signum);
}

namespace Loader::GuestExecution {
namespace Memory = Libs::LibKernel::Memory;
namespace Vm = Common::VirtualMemory;

static bool ConstantLaneMaskSelfTest() {
    using namespace Libs::Graphics::ShaderRecompiler::IR;
    bool pass = true;
    for (const auto wave: {32u, 64u}) {
        for (const auto mask: {0u, UINT32_MAX, 0x55555555u}) {
            Libs::Graphics::ShaderRecompiler::IR::Program program;
            program.block_storage.push_back(std::make_unique<Block>());
            auto* block = program.block_storage.back().get();
            program.blocks.push_back(block);
            auto& lane = block->AppendNewInst(ValueOpcode::LaneId);
            auto& index = block->AppendNewInst(ValueOpcode::BitwiseAnd32,
                                                {Value(&lane), Value(31u)});
            auto& shifted = block->AppendNewInst(ValueOpcode::ShiftRightLogical32,
                                                  {Value(mask), Value(&index)});
            auto& bit = block->AppendNewInst(ValueOpcode::BitwiseAnd32,
                                               {Value(&shifted), Value(1u)});
            auto& predicate = block->AppendNewInst(ValueOpcode::INotEqual32,
                                                     {Value(&bit), Value(0u)});
            auto& user = block->AppendNewInst(ValueOpcode::SelectU32,
                                                 {Value(&predicate), Value(7u), Value(9u)});
            auto& output = block->AppendNewInst(ValueOpcode::ReferenceU32, {Value(&user)});
            ConstantPropagationPass(program.blocks, wave);
            const auto result = output.Arg(0).Resolve();
            pass &= mask == 0x55555555u ? !result.IsImmediate()
                : result.IsImmediate() && result.U32() == (mask != 0 ? 7u : 9u);
        }
    }
    return pass;
}

static bool BackingAliasVisibilitySelfTest() noexcept {
    constexpr uint64_t page = 0x4000;
    constexpr int gpu_rw = 0x30;
    bool pass = true;
    const auto check = [&](const char* name, bool valid) {
        std::printf("PS5_VM_ALIAS_TEST case=%s result=%s\n", name, valid ? "PASS" : "FAIL");
        pass &= valid;
    };
    std::vector<Memory::GuestBackingAlias> aliases {{1, 1}};
    check("invalid-empty-output-visible", !Memory::QueryGuestBackingAliases(0, page, &aliases) &&
        aliases.empty() && Memory::QueryGuestBackingCpuVisible(0, page) &&
        !Memory::QueryGuestBackingAliases(UINT64_MAX - 1, 8, &aliases) && aliases.empty() &&
        Memory::QueryGuestBackingCpuVisible(UINT64_MAX - 1, 8));
    int64_t physical = -1;
    if (Memory::KernelAllocateDirectMemory(0,
            static_cast<int64_t>(Memory::KernelGetDirectMemorySize()), page * 4, page,
            11, &physical) != ::OK) return false;
    void* source = nullptr;
    if (Memory::KernelMapDirectMemory(&source, page * 4, gpu_rw, 0, physical, page) != ::OK) {
        (void)Memory::KernelCheckedReleaseDirectMemory(physical, page * 4);
        return false;
    }
    const auto base = reinterpret_cast<uint64_t>(source);
    check("gpu-only-direct-invisible", !Memory::QueryGuestBackingCpuVisible(base, page * 4));
    void* alias = nullptr;
    if (Memory::KernelMapDirectMemory(&alias, page * 2, 0x02, 0,
            physical + page, page) != ::OK) {
        (void)Memory::KernelCheckedReleaseDirectMemory(physical, page * 4);
        return false;
    }
    const auto alias_base = reinterpret_cast<uint64_t>(alias);
    const auto contains = [&](uint64_t address, uint64_t size, bool source_span) {
        return std::any_of(aliases.begin(), aliases.end(), [=](const auto& span) {
            return span.source == source_span && address >= span.address && address - span.address <= span.size &&
                size <= span.size - (address - span.address);
        });
    };
    const bool queried = Memory::QueryGuestBackingAliases(base + page + 8, 32, &aliases);
    uint64_t alias_bytes = 0;
    for (const auto& span: aliases) alias_bytes += span.size;
    check("physical-offset-alias-clipping", queried && contains(base + page + 8, 32, true) &&
        contains(alias_base + 8, 32, false) && alias_bytes == 64);
    check("write-only-alias-implies-cpu-visible", Memory::QueryGuestBackingCpuVisible(base, page * 4) &&
        !Memory::QueryGuestBackingCpuVisible(base, page) &&
        Memory::QueryGuestBackingCpuVisible(base + page, page));
    check("split-rights-excludes-nonoverlap", Memory::KernelMprotect(
        reinterpret_cast<void*>(alias_base + page), page, gpu_rw) == ::OK &&
        Memory::QueryGuestBackingCpuVisible(base + page, page) &&
        !Memory::QueryGuestBackingCpuVisible(base + page * 2, page));
    check("revoked-alias-restores-invisibility", Memory::KernelMprotect(alias, page, gpu_rw) == ::OK &&
        !Memory::QueryGuestBackingCpuVisible(base, page * 4));
    check("execute-alias-visible", Memory::KernelMprotect(
        reinterpret_cast<void*>(alias_base + page), page, 0x04) == ::OK &&
        Memory::QueryGuestBackingCpuVisible(base + page * 2, page) &&
        !Memory::QueryGuestBackingCpuVisible(base + page, page));
    check("execute-alias-rejects-gpu-write", !Memory::QueryGpuBackingWrite(base + page * 2, page) &&
        Memory::QueryGpuBackingWrite(base + page, page));
    check("alias-unmap-removes-visibility", Memory::KernelMunmap(alias_base + page, page) == ::OK &&
        !Memory::QueryGuestBackingCpuVisible(base, page * 4) &&
        !Memory::QueryGuestBackingAliases(alias_base + page, page, &aliases) && aliases.empty() &&
        Memory::QueryGuestBackingCpuVisible(alias_base + page, page));
    check("source-hole-fails-closed", Memory::KernelMunmap(base + page, page) == ::OK &&
        !Memory::QueryGuestBackingAliases(base, page * 4, &aliases) && aliases.empty() &&
        Memory::QueryGuestBackingCpuVisible(base, page * 4));
    void* repeated = nullptr;
    if (Memory::KernelReserveVirtualRange(&repeated, page * 2, 0, page) != ::OK) {
        (void)Memory::KernelCheckedReleaseDirectMemory(physical, page * 4);
        return false;
    }
    const auto repeated_base = reinterpret_cast<uint64_t>(repeated);
    void* repeated_left = repeated;
    void* repeated_right = reinterpret_cast<void*>(repeated_base + page);
    if (Memory::KernelMapDirectMemory(&repeated_left, page, gpu_rw, 0x10, physical, page) != ::OK ||
        Memory::KernelMapDirectMemory(&repeated_right, page, gpu_rw, 0x10, physical, page) != ::OK) {
        (void)Memory::KernelMunmap(repeated_base, page * 2);
        (void)Memory::KernelCheckedReleaseDirectMemory(physical, page * 4);
        return false;
    }
    check("repeated-source-physical-bytes-rejected", !Memory::QueryGuestBackingAliases(
        repeated_base, page * 2, &aliases) && aliases.empty() &&
        Memory::QueryGuestBackingCpuVisible(repeated_base, page * 2) &&
        !Memory::QueryGpuBackingWrite(repeated_base, page * 2));
    check("release-removes-all-aliases", Memory::KernelCheckedReleaseDirectMemory(physical, page * 4) == ::OK &&
        !Memory::QueryGuestBackingAliases(alias_base, page, &aliases) && aliases.empty());
    void* flexible = nullptr;
    if (Memory::KernelMapFlexibleMemory(&flexible, page, gpu_rw, 0) != ::OK) return false;
    const auto flexible_base = reinterpret_cast<uint64_t>(flexible);
    check("non-direct-source-span", Memory::QueryGuestBackingAliases(flexible_base + 8, 32, &aliases) &&
        aliases.size() == 1 && aliases[0].address == flexible_base + 8 && aliases[0].size == 32 && aliases[0].source &&
        !Memory::QueryGuestBackingCpuVisible(flexible_base, page));
    check("non-direct-rights-change-visible", Memory::KernelMprotect(flexible, page, 0x01) == ::OK &&
        Memory::QueryGuestBackingCpuVisible(flexible_base, page));
    check("non-direct-cleanup", Memory::KernelMunmap(flexible_base, page) == ::OK);
    return pass;
}

#include "fexBackingIdentityTests.inc"
#include "fexBackingImportTests.inc"
#include "fexBackingHashTests.inc"

bool MemoryAccessSelfTest() noexcept {
    bool pass = Memory::InitializeFexMemoryAccess();
    pass &= BackingAliasVisibilitySelfTest();
    pass &= BackingIdentitySelfTest();
    pass &= BackingImportSelfTest();
    pass &= BackingHashSelfTest();
    const auto allocation = Memory::AllocateRuntimeMemory(0, 0x8000, Vm::Mode::ReadWrite,
                                                         "guest-memory-access-test");
    if (allocation == 0) return false;
    constexpr std::array<unsigned char, 4> input {0x10, 0x20, 0x30, 0x40};
    std::array<unsigned char, 4> output {};
    pass &= Memory::WriteGuestMemory(nullptr, allocation + 0x3ffe, input.data(), input.size());
    pass &= Memory::ReadGuestMemory(nullptr, allocation + 0x3ffe, output.data(), output.size());
    pass &= output == input;
    pass &= Memory::ProtectGuestMemory(allocation + 0x4000, 0x4000, Vm::Mode::Read);
    pass &= !Memory::WriteGuestMemory(nullptr, allocation + 0x3ffe, input.data(), input.size());
    pass &= Memory::ReadGuestMemory(nullptr, allocation + 0x3ffe, output.data(), output.size());
    pass &= output == input;
    pass &= Memory::ProtectGuestMemory(allocation, 0x4000, Vm::Mode::NoAccess);
    output.fill(0xa5);
    pass &= !Memory::ReadGuestMemory(nullptr, allocation, output.data(), output.size());
    pass &= output == std::array<unsigned char, 4> {0xa5, 0xa5, 0xa5, 0xa5};
    pass &= !Memory::QueryGuestAccess(UINT64_MAX - 1, 8, Vm::Mode::Read);

    void* reservation = nullptr;
    pass &= Memory::KernelReserveVirtualRange(&reservation, 0x4000, 0, 0x4000) ==
            ::OK;
    const auto reserved_address = reinterpret_cast<uint64_t>(reservation);
    pass &= !Memory::ReadGuestMemory(nullptr, reserved_address, output.data(), output.size());
    pass &= Memory::KernelMunmap(reserved_address, 0x4000) == ::OK;

    const auto code = Memory::AllocateProgramMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                   "guest-code-rights-test");
    uint64_t base = 0;
    uint64_t size = 0;
    uint32_t writable = 0;
    pass &= code != 0 && Memory::QueryGuestExecutable(nullptr, code, &base, &size, &writable) == 0;
    if (code != 0) {
        Memory::SetProgramMemoryProtection(code, 0x4000, Vm::Mode::ExecuteRead);
        pass &= Memory::QueryGuestExecutable(nullptr, code, &base, &size, &writable) == 1;
        pass &= base == code && size == 0x4000 && writable == 0;
        pass &= !Memory::WriteGuestMemory(nullptr, code, input.data(), input.size());
        pass &= Memory::FreeGuestMemory(code, 0x4000);
        pass &= Memory::QueryGuestExecutable(nullptr, code, &base, &size, &writable) == 0;
    }
    pass &= Memory::FreeGuestMemory(allocation, 0x8000);
    pass &= !Memory::ReadGuestMemory(nullptr, allocation, output.data(), output.size());

    // Flexible mappings prove the checked copy uses the registered backing
    // alias, while CPU rights remain distinct from the GPU permission bits.
    void* flexible = nullptr;
    pass &= Memory::KernelMapFlexibleMemory(&flexible, 0x4000, 0x02, 0) == ::OK;
    const auto flexible_address = reinterpret_cast<uint64_t>(flexible);
    pass &= Memory::WriteGuestMemory(nullptr, flexible_address, input.data(), input.size());
    pass &= Memory::ReadGuestMemory(nullptr, flexible_address, output.data(), output.size());
    pass &= output == input;
    pass &= Memory::KernelMprotect(flexible, 0x4000, 0x10) == ::OK;
    pass &= !Memory::ReadGuestMemory(nullptr, flexible_address, output.data(), output.size());
    pass &= !Memory::WriteGuestMemory(nullptr, flexible_address, input.data(), input.size());
    pass &= Memory::KernelMunmap(flexible_address, 0x4000) == ::OK;

    std::puts(pass ? "PS5 checked memory access: PASS" : "PS5 checked memory access: FAIL");
    return pass;
}

namespace {
std::uint64_t NativeSum8(std::uint64_t a, std::uint64_t b, std::uint64_t c,
    std::uint64_t d, std::uint64_t e, std::uint64_t f, std::uint64_t g, std::uint64_t h) {
    return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h;
}
std::uint64_t NativeNested(std::uint64_t target, std::uint64_t argument) {
    std::uint64_t value {};
    return Invoke(target, &argument, 1, &value) ? value + 3 : UINT64_MAX;
}
void NativeThreadExit(std::uint64_t value) { RequestThreadExit(value); }
constexpr std::uint64_t join_return_value = 0x2468ac;
constexpr std::uint64_t join_mutation_marker = 0x6a6f696e766d3031ULL;
std::atomic<bool> join_started {};
std::atomic<std::uint64_t> join_mutation_address {};
std::atomic<std::int64_t> join_start_deadline {};
std::uint64_t NativeJoinMutation() {
    if (!join_started.load(std::memory_order_acquire)) {
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return now < join_start_deadline.load(std::memory_order_acquire) ? 0 : UINT64_MAX;
    }
    const auto address = Memory::AllocateRuntimeMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                       "fex-join-peer-mutation");
    join_mutation_address.store(address, std::memory_order_release);
    return address != 0 && Memory::WriteGuestMemory(nullptr, address,
        &join_mutation_marker, sizeof(join_mutation_marker)) ? join_return_value : UINT64_MAX;
}
std::uint64_t NativeJoinParent(std::uint64_t child_entry, std::uint64_t result_slot) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    join_start_deadline.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
        deadline.time_since_epoch()).count(), std::memory_order_release);
    Libs::LibKernel::Pthread child = nullptr;
    const auto created = Libs::LibKernel::PthreadCreate(&child, nullptr,
        reinterpret_cast<Libs::LibKernel::pthread_entry_func_t>(child_entry), nullptr,
        "fex-join-vm-peer");
    if (created != ::OK) return static_cast<std::uint64_t>(created);
    join_started.store(true, std::memory_order_release);
    return static_cast<std::uint64_t>(Libs::LibKernel::PthreadJoin(child,
        reinterpret_cast<void**>(result_slot)));
}
constexpr std::size_t competing_mapping_count = 4;
constexpr std::uint64_t competing_marker = 0x564d434f4d504554ULL;
struct CompetingMapping {
    std::int64_t physical {-1};
    std::uint64_t mapped {};
};
struct CompetingVmRecord {
    std::uint64_t worker {};
    std::array<CompetingMapping, competing_mapping_count> mappings {};
};
static_assert(sizeof(CompetingVmRecord) == 72 &&
              offsetof(CompetingVmRecord, mappings) == 8);
std::atomic<bool> competing_started {};
std::atomic<unsigned> competing_prestart {};
std::atomic<unsigned> competing_ready {};
std::atomic<std::uint64_t> competing_create_entry {};
std::atomic<unsigned> competing_children_joined {};
std::atomic<std::int64_t> competing_deadline {};
std::array<std::uint64_t, 2> competing_thread_results {};
std::int64_t SteadyNanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::uint64_t NativeCompetingVmWorker(std::uint64_t record_address) {
    std::uint64_t worker {};
    if (!Memory::ReadGuestMemory(nullptr, record_address, &worker, sizeof(worker)) || worker > 1)
        return UINT64_MAX;
    competing_prestart.fetch_or(1U << worker, std::memory_order_acq_rel);
    if (!competing_started.load(std::memory_order_acquire)) {
        return SteadyNanoseconds() < competing_deadline.load(std::memory_order_acquire)
            ? 0 : UINT64_MAX;
    }
    competing_ready.fetch_add(1, std::memory_order_acq_rel);
    // Both real HLE readers must be present before either mutation begins.
    // The first epoch cannot finish until the competing entry releases its
    // reader. This bounded handshake performs no guest access while waiting.
    while (competing_ready.load(std::memory_order_acquire) != 2) {
        if (SteadyNanoseconds() >= competing_deadline.load(std::memory_order_acquire))
            return UINT64_MAX;
        std::this_thread::yield();
    }
    if (const auto entry = competing_create_entry.load(std::memory_order_acquire); entry != 0) {
        for (unsigned i = 0; i != 2; ++i) {
            Libs::LibKernel::Pthread child = nullptr;
            if (Libs::LibKernel::PthreadCreate(&child, nullptr,
                    reinterpret_cast<Libs::LibKernel::pthread_entry_func_t>(entry), nullptr,
                    "fex-concurrent-create-child") != ::OK) return UINT64_MAX;
            void* result = nullptr;
            if (Libs::LibKernel::PthreadJoinHost(child, &result) != ::OK ||
                reinterpret_cast<std::uint64_t>(result) != 42) return UINT64_MAX;
            competing_children_joined.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    for (std::size_t i = 0; i < competing_mapping_count; ++i) {
        const auto pair = record_address + offsetof(CompetingVmRecord, mappings) +
                          i * sizeof(CompetingMapping);
        for (unsigned cycle = 0; cycle != 2; ++cycle) {
        if (Memory::KernelAllocateDirectMemory(0,
                static_cast<std::int64_t>(Memory::KernelGetDirectMemorySize()),
                0x4000, 0x4000, 0, reinterpret_cast<std::int64_t*>(pair)) != ::OK)
            return UINT64_MAX;
        std::int64_t physical {-1};
        if (!Memory::ReadGuestMemory(nullptr, pair, &physical, sizeof(physical)) ||
            Memory::KernelMapDirectMemory(reinterpret_cast<void**>(pair + 8),
                0x4000, 0x02, 0, physical, 0x4000) != ::OK)
            return UINT64_MAX;
        if (cycle == 0) {
            std::uint64_t mapped {};
            if (!Memory::ReadGuestMemory(nullptr, pair + 8, &mapped, sizeof(mapped)) ||
                Memory::KernelMunmap(mapped, 0x4000) != ::OK ||
                Memory::KernelCheckedReleaseDirectMemory(physical, 0x4000) != ::OK)
                return UINT64_MAX;
        }
        }
    }
    // The guest writes each mapped witness after this callback returns.
    // Each peer has also retired temporary virtual and physical owners in HLE.
    return record_address;
}
std::uint64_t NativeCompetingVmParent(std::uint64_t child_entry, std::uint64_t records) {
    competing_started.store(false, std::memory_order_release);
    competing_prestart.store(0, std::memory_order_release);
    competing_ready.store(0, std::memory_order_release);
    competing_thread_results.fill(UINT64_MAX);
    competing_deadline.store(SteadyNanoseconds() + 2'000'000'000LL,
                              std::memory_order_release);
    std::array<Libs::LibKernel::Pthread, 2> children {};
    unsigned created = 0;
    for (; created != children.size(); ++created) {
        if (Libs::LibKernel::PthreadCreate(&children[created], nullptr,
                reinterpret_cast<Libs::LibKernel::pthread_entry_func_t>(child_entry),
                reinterpret_cast<void*>(records + created * 128),
                "fex-competing-vm-worker") != ::OK) break;
    }
    // Complete both root Invoke preflights before allowing either
    // worker retain a handshake HLE reader until both have completed it and
    // entered their real guest/HLE polling loop.
    while (created == children.size() &&
           competing_prestart.load(std::memory_order_acquire) != 3 &&
           SteadyNanoseconds() < competing_deadline.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    bool joined = created == children.size() &&
        competing_prestart.load(std::memory_order_acquire) == 3;
    competing_started.store(true, std::memory_order_release);
    for (unsigned i = 0; i != created; ++i) {
        void* native_result = nullptr;
        joined &= Libs::LibKernel::PthreadJoinHost(children[i], &native_result) == ::OK;
        competing_thread_results[i] = reinterpret_cast<std::uint64_t>(native_result);
    }
    return joined ? ::OK : UINT64_MAX;
}
namespace Sema = Libs::LibKernel::Semaphore;
constexpr std::uint64_t host_wait_marker = 0x484f535457414954ULL;
std::atomic<bool> host_wait_started {};
std::atomic<unsigned> host_wait_prestart {};
std::atomic<unsigned> host_wait_entered {};
std::atomic<unsigned> host_wait_finished {};
std::atomic<unsigned> mutation_precedes_wait_end {};
std::atomic<bool> host_wait_mutation_done {};
std::atomic<std::int64_t> host_wait_deadline {};
Sema::KernelSema host_wait_sem {};
namespace Events = Libs::LibKernel::EventQueue;
Events::KernelEqueue host_wait_queue {};
std::array<Libs::LibKernel::PthreadMutex, 2> host_wait_mutex {};
std::array<Libs::LibKernel::PthreadCond, 2> host_wait_cond {};
std::atomic<bool> host_wait_destroy_busy {};
std::uint64_t host_wait_output {};
bool HostWaitPollStart(unsigned bit) {
    host_wait_prestart.fetch_or(bit, std::memory_order_acq_rel);
    return host_wait_started.load(std::memory_order_acquire);
}
std::uint64_t NativeHostWaitReader(std::uint64_t mode) {
    if (mode > 4) return UINT64_MAX;
    const unsigned bit = 1U << mode;
    if (!HostWaitPollStart(bit)) {
        return SteadyNanoseconds() < host_wait_deadline.load(std::memory_order_acquire)
            ? 0 : UINT64_MAX;
    }
    host_wait_entered.fetch_or(bit, std::memory_order_acq_rel);
    int result {};
    if (mode == 0) result = Libs::LibKernel::KernelUsleep(250000);
    else if (mode == 1) result = Sema::KernelWaitSema(host_wait_sem, 1, nullptr);
    else if (mode == 4) result = Events::KernelWaitEqueue(host_wait_queue,
        reinterpret_cast<Events::KernelEvent*>(host_wait_output + 64), 1,
        reinterpret_cast<int*>(host_wait_output + 128), nullptr);
    else {
        const auto index = mode - 2;
        if (Libs::LibKernel::PthreadMutexLock(&host_wait_mutex[index]) != ::OK)
            return UINT64_MAX;
        result = mode == 2 ? Libs::LibKernel::PthreadCondTimedwait(
            &host_wait_cond[index], &host_wait_mutex[index], 250000)
            : Libs::LibKernel::PthreadCondWait(&host_wait_cond[index], &host_wait_mutex[index]);
        if (Libs::LibKernel::PthreadMutexUnlock(&host_wait_mutex[index]) != ::OK)
            return UINT64_MAX;
    }
    host_wait_finished.fetch_or(bit, std::memory_order_acq_rel);
    return result == (mode == 2 ? Libs::LibKernel::KERNEL_ERROR_ETIMEDOUT : ::OK) ? 1 : UINT64_MAX;
}
std::uint64_t NativeHostWaitMutation() {
    if (!HostWaitPollStart(32)) {
        return SteadyNanoseconds() < host_wait_deadline.load(std::memory_order_acquire)
            ? 0 : UINT64_MAX;
    }
    while (host_wait_entered.load(std::memory_order_acquire) != 31) {
        if (SteadyNanoseconds() >= host_wait_deadline.load(std::memory_order_acquire))
            return UINT64_MAX;
        std::this_thread::yield();
    }
    if (Memory::KernelAllocateDirectMemory(0,
            static_cast<std::int64_t>(Memory::KernelGetDirectMemorySize()), 0x4000, 0x4000,
            0, reinterpret_cast<std::int64_t*>(host_wait_output)) != ::OK)
        return UINT64_MAX;
    std::int64_t physical {-1};
    if (!Memory::ReadGuestMemory(nullptr, host_wait_output, &physical, sizeof(physical)) ||
        Memory::KernelMapDirectMemory(reinterpret_cast<void**>(host_wait_output + 8),
            0x4000, 0x02, 0, physical, 0x4000) != ::OK)
        return UINT64_MAX;
    std::uint64_t mapped {};
    if (!Memory::ReadGuestMemory(nullptr, host_wait_output + 8, &mapped, sizeof(mapped)))
        return UINT64_MAX;
    mutation_precedes_wait_end.store(31U & ~host_wait_finished.load(std::memory_order_acquire),
                                     std::memory_order_release);
    host_wait_mutation_done.store(true, std::memory_order_release);
    return mapped;
}
unsigned TestHostWaitReaders(std::uint64_t reader_entry, std::uint64_t mutation_entry,
                            std::uint64_t output) {
    host_wait_started.store(false, std::memory_order_release);
    host_wait_prestart.store(0, std::memory_order_release);
    host_wait_entered.store(0, std::memory_order_release);
    host_wait_finished.store(0, std::memory_order_release);
    mutation_precedes_wait_end.store(0, std::memory_order_release);
    host_wait_mutation_done.store(false, std::memory_order_release);
    host_wait_output = output;
    host_wait_destroy_busy.store(false, std::memory_order_release);
    host_wait_deadline.store(SteadyNanoseconds() + 2'000'000'000LL, std::memory_order_release);
    if (Sema::KernelCreateSema(&host_wait_sem, "fex-host-wait-test", 1, 0, 1, nullptr) != ::OK)
        return 0;
    for (std::size_t i = 0; i != 2; ++i) {
        if (Libs::LibKernel::PthreadMutexInit(&host_wait_mutex[i], nullptr, "fex-wait-mutex") != ::OK ||
            Libs::LibKernel::PthreadCondInit(&host_wait_cond[i], nullptr, "fex-wait-cond") != ::OK)
            return 0;
    }
    if (Events::KernelCreateEqueue(&host_wait_queue, "fex-host-wait-equeue") != ::OK ||
        Events::KernelAddUserEvent(host_wait_queue, 42) != ::OK) return 0;
    std::array<Libs::LibKernel::Pthread, 6> children {};
    unsigned created = 0;
    for (; created != children.size(); ++created) {
        const auto entry = created == 5 ? mutation_entry : reader_entry;
        if (Libs::LibKernel::PthreadCreate(&children[created], nullptr,
                reinterpret_cast<Libs::LibKernel::pthread_entry_func_t>(entry),
                reinterpret_cast<void*>(created == 5 ? 0U : created),
                "fex-host-wait-peer") != ::OK) break;
    }
    while (created == children.size() && host_wait_prestart.load(std::memory_order_acquire) != 63 &&
           SteadyNanoseconds() < host_wait_deadline.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    bool pass = created == children.size() &&
        host_wait_prestart.load(std::memory_order_acquire) == 63;
    // The native controller bounds the otherwise infinite semaphore wait.
    // It wakes on a real completed VM mutation, or after 500ms so a regression
    // fails its ordering witness without retaining any HLE reader indefinitely.
    std::thread signaler([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (!host_wait_mutation_done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // The infinite condition waiter must pin both native objects, even
        // after a wake removes it from the condition's logical waiter list.
        host_wait_destroy_busy.store(
            Libs::LibKernel::PthreadCondDestroy(&host_wait_cond[1]) == Libs::LibKernel::KERNEL_ERROR_EBUSY &&
            Libs::LibKernel::PthreadMutexDestroy(&host_wait_mutex[1]) == Libs::LibKernel::KERNEL_ERROR_EBUSY,
            std::memory_order_release);
        if (Sema::KernelSignalSema(host_wait_sem, 1) != ::OK ||
            Libs::LibKernel::PthreadCondBroadcast(&host_wait_cond[1]) != ::OK ||
            Events::KernelTriggerUserEvent(host_wait_queue, 42, nullptr) != ::OK) std::abort();
    });
    host_wait_started.store(true, std::memory_order_release);
    for (unsigned i = 0; i != created; ++i) {
        void* result = nullptr;
        pass &= Libs::LibKernel::PthreadJoinHost(children[i], &result) == ::OK &&
                reinterpret_cast<std::uint64_t>(result) == 1;
    }
    signaler.join();
    pass &= host_wait_destroy_busy.load(std::memory_order_acquire);
    for (std::size_t i = 0; i != 2; ++i) {
        pass &= Libs::LibKernel::PthreadCondDestroy(&host_wait_cond[i]) == ::OK;
        pass &= Libs::LibKernel::PthreadMutexDestroy(&host_wait_mutex[i]) == ::OK;
    }
    pass &= Sema::KernelDeleteSema(host_wait_sem) == ::OK;
    host_wait_sem = nullptr;
    Events::KernelEvent event {};
    int event_count {};
    pass &= Memory::ReadGuestMemory(nullptr, output + 64, &event, sizeof(event)) &&
        Memory::ReadGuestMemory(nullptr, output + 128, &event_count, sizeof(event_count)) &&
        event_count == 1 && event.ident == 42 && event.filter == Events::KERNEL_EVFILT_USER;
    pass &= Events::KernelDeleteEqueue(host_wait_queue) == ::OK;
    host_wait_queue = Events::KERNEL_EQUEUE_INVALID;
    CompetingMapping mapping {};
    std::uint64_t marker {};
    pass &= Memory::ReadGuestMemory(nullptr, output, &mapping, sizeof(mapping)) &&
        mapping.physical >= 0 && mapping.mapped != 0 &&
        Memory::ReadGuestMemory(nullptr, mapping.mapped, &marker, sizeof(marker)) &&
        marker == host_wait_marker && host_wait_finished.load(std::memory_order_acquire) == 31;
    if (mapping.mapped != 0) pass &= Memory::KernelMunmap(mapping.mapped, 0x4000) == ::OK;
    if (mapping.physical >= 0)
        pass &= Memory::KernelCheckedReleaseDirectMemory(mapping.physical, 0x4000) == ::OK;
    return pass ? mutation_precedes_wait_end.load(std::memory_order_acquire) : 0;
}
using AgcRegister = Libs::Graphics::ShaderRegister;
AgcRegister last_marshaled_register {};
AgcRegister NativeRegisterRoundtrip(std::uint64_t tag, AgcRegister reg) {
    last_marshaled_register = reg;
    return {reg.value ^ static_cast<std::uint32_t>(tag), reg.offset + 3};
}
std::uint32_t DenyHostCall(void*, std::uint64_t, std::uint64_t*,
    LumenAndroidFexCoreSimdViewV2*, std::uint64_t*) noexcept {
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
}
void Put64(std::array<std::uint8_t, 1024>& code, std::size_t offset, std::uint64_t value) {
    std::memcpy(code.data() + offset, &value, 8);
}
RunRequest TestRequest(std::uint64_t rip, std::uint64_t stack_top) {
    RunRequest request {};
    request.state.rip = rip;
    request.state.gprs[4] = (stack_top & ~std::uint64_t {15}) - 8;
    request.host_call = DenyHostCall;
    request.executable_query = Memory::QueryGuestExecutable;
    request.read_memory = Memory::ReadGuestMemory;
    request.write_memory = Memory::WriteGuestMemory;
    return request;
}
void CheckCase(const char* name, bool passed, bool& aggregate) {
    std::printf("PS5_FEX_RUNTIME_TEST case=%s result=%s\n", name, passed ? "PASS" : "FAIL");
    aggregate &= passed;
}

struct ClockSample {
    std::uint64_t wall_ns {}, tsc {}, process_us {}, process_counter {};
};
struct ClockWaitObservation {
    ClockSample before {}, after {};
    unsigned calls {};
};
thread_local ClockWaitObservation clock_wait;

ClockSample ReadClockSample() {
    const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return {static_cast<std::uint64_t>(wall), Libs::LibKernel::KernelReadTsc(),
        Libs::LibKernel::KernelGetProcessTime(),
        Libs::LibKernel::KernelGetProcessTimeCounter()};
}

std::uint64_t NativeClockWait() {
    ++clock_wait.calls;
    clock_wait.before = ReadClockSample();
    const auto result = Libs::LibKernel::KernelUsleep(50000);
    clock_wait.after = ReadClockSample();
    return static_cast<std::uint64_t>(result);
}

bool GuestThreadIdSelfTest(std::uint64_t stack_top) {
    namespace Kernel = Libs::LibKernel;
    Loader::SymbolDatabase symbols;
    Libs::InitLibKernel_1_Pthread(&symbols);
    const auto target = reinterpret_cast<std::uint64_t>(Kernel::PthreadGetthreadid);
    const auto* imported = symbols.FindByNid("EI-5-jlq2dE", Loader::SymbolType::Func);
    bool pass = true;
    CheckCase("thread-id-production-import-has-no-effects",
        imported != nullptr && imported->vaddr == target && HostCallEffectsFor(target) ==
            Lumen::GuestCpu::EffectMask(Lumen::GuestCpu::HostCallEffect::None), pass);
    const auto expected = static_cast<std::uint64_t>(Kernel::PthreadGetthreadid());
    bool native_empty = false;
    std::thread outside([&] { native_empty = Kernel::PthreadGetthreadid() == 0; });
    outside.join();
    CheckCase("thread-id-without-guest-thread-is-zero", native_empty, pass);

    const auto code_base = Memory::AllocateProgramMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                         "fex-thread-id-code");
    if (code_base == 0) return false;
    std::array<std::uint8_t, 1024> code {0x48,0x83,0xec,0x08,0x48,0xb8};
    Put64(code, 6, target);
    constexpr std::uint8_t tail[] {0xff,0xd0,0x48,0x83,0xc4,0x08,0xc3};
    std::memcpy(code.data() + 14, tail, sizeof(tail));
    pass &= Memory::WriteGuestMemory(nullptr, code_base, code.data(), code.size());
    Memory::SetProgramMemoryProtection(code_base, 0x4000, Vm::Mode::ExecuteRead);
    std::uint64_t first {}, second {};
    CheckCase("thread-id-guest-call-preserves-current-native-id", expected != 0 &&
        Invoke(code_base, nullptr, 0, &first, stack_top) && first == expected, pass);
    Kernel::Pthread child = nullptr;
    const bool created = Kernel::PthreadCreate(&child, nullptr,
        reinterpret_cast<Kernel::pthread_entry_func_t>(code_base), nullptr,
        "fex-thread-id-child") == ::OK;
    void* child_value = nullptr;
    const bool joined = created && Kernel::PthreadJoinHost(child, &child_value) == ::OK;
    CheckCase("thread-id-real-child-uses-distinct-current-thread", joined &&
        reinterpret_cast<std::uint64_t>(child_value) != 0 &&
        reinterpret_cast<std::uint64_t>(child_value) != expected, pass);
    if (created && !joined) return false; // Do not retire code still owned by an unjoined child.
    CheckCase("thread-id-parent-remains-stable-after-child", Invoke(code_base, nullptr, 0,
        &second, stack_top) && second == expected &&
        static_cast<std::uint64_t>(Kernel::PthreadGetthreadid()) == expected, pass);
    pass = Memory::FreeGuestMemory(code_base, 0x4000) && pass;
    return pass;
}

bool GuestTscSelfTest(std::uint64_t stack_top) {
    namespace Kernel = Libs::LibKernel;
    if (!RegisterHostFunction<Kernel::KernelReadTsc>("self-test-read-tsc") ||
        !RegisterHostFunction<Kernel::KernelGetTscFrequency>("self-test-tsc-frequency") ||
        !RegisterHostFunction<NativeClockWait>("self-test-clock-wait")) return false;
    struct Output {
        std::array<std::uint32_t, 4> cpuid {};
        std::uint64_t rdtsc_before {}, kernel_before {}, frequency {}, wait_result {},
            kernel_after {}, rdtsc_after {}, final_counter {};
    };
    static_assert(sizeof(Output) == 72);
    const auto code_base = Memory::AllocateProgramMemory(0, 0x4000, Vm::Mode::ReadWrite,
        "fex-clock-domain-code");
    const auto output_base = Memory::AllocateRuntimeMemory(0, 0x4000, Vm::Mode::ReadWrite,
        "fex-clock-domain-output");
    if (code_base == 0 || output_base == 0) {
        if (code_base != 0) Memory::FreeGuestMemory(code_base, 0x4000);
        if (output_base != 0) Memory::FreeGuestMemory(output_base, 0x4000);
        return false;
    }
    std::array<std::uint8_t, 1024> code {};
    std::size_t cursor = 0;
    const auto emit = [&](std::initializer_list<std::uint8_t> bytes) {
        for (const auto byte: bytes) code[cursor++] = byte;
    };
    const auto store_rax = [&](std::uint8_t offset) {
        emit({0x49, 0x89, 0x44, 0x24, offset}); // MOV [R12+disp8], RAX
    };
    const auto call = [&](std::uint64_t target, std::uint8_t offset) {
        emit({0x48, 0xb8});
        Put64(code, cursor, target); cursor += 8;
        emit({0xff, 0xd0});
        store_rax(offset);
    };
    const auto read_counter = [&](bool final) {
        emit({0x0f, 0xae, 0xe8}); // LFENCE before the instruction bracket.
#if defined(__APPLE__)
        (void)final;
        emit({0x0f, 0x31}); // Darwin's admitted adapter excludes RDTSCP.
#else
        if (final) emit({0x0f, 0x01, 0xf9}); else emit({0x0f, 0x31});
#endif
        emit({0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xd0});
    };
    // Preserve RBX across CPUID and R12 across typed HLE calls; CALL frames
    // retain SysV alignment. All output bytes belong to the embedding VM.
    emit({0x53, 0x41, 0x54, 0x48, 0x83, 0xec, 0x08, 0x49, 0x89, 0xfc});
    emit({0xb8, 0x15, 0, 0, 0, 0x31, 0xc9, 0x0f, 0xa2});
    emit({0x41, 0x89, 0x04, 0x24, 0x41, 0x89, 0x5c, 0x24, 0x04,
          0x41, 0x89, 0x4c, 0x24, 0x08, 0x41, 0x89, 0x54, 0x24, 0x0c});
    read_counter(false); store_rax(16);
    call(reinterpret_cast<std::uint64_t>(Kernel::KernelReadTsc), 24);
    call(reinterpret_cast<std::uint64_t>(Kernel::KernelGetTscFrequency), 32);
    call(reinterpret_cast<std::uint64_t>(NativeClockWait), 40);
    call(reinterpret_cast<std::uint64_t>(Kernel::KernelReadTsc), 48);
    read_counter(false); store_rax(56);
    read_counter(true); store_rax(64);
    emit({0x48, 0x83, 0xc4, 0x08, 0x41, 0x5c, 0x5b, 0xc3});

    bool pass = Memory::WriteGuestMemory(nullptr, code_base, code.data(), cursor);
    Memory::SetProgramMemoryProtection(code_base, 0x4000, Vm::Mode::ExecuteRead);
    const auto process_frequency = Kernel::KernelGetProcessTimeCounterFrequency();
    bool host_thread_queries = true;
    std::thread host_thread([&] {
        // VideoOut queries the clock from a native thread without guest TLS.
        for (unsigned index = 0; index < 16; ++index) {
            LumenAndroidFexCoreGuestTscV1 before, after;
            host_thread_queries &= LumenAndroidFexCoreQueryGuestTscV1(&before) ==
                LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1;
            const auto counter = Kernel::KernelReadTsc();
            const auto frequency = Kernel::KernelGetTscFrequency();
            host_thread_queries &= LumenAndroidFexCoreQueryGuestTscV1(&after) ==
                LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 && before.counter <= counter &&
                counter <= after.counter && frequency == before.frequency &&
                frequency == after.frequency;
        }
    });
    host_thread.join();
    CheckCase("tsc-native-thread-without-guest-tls", host_thread_queries, pass);
    const auto to_ns = [](std::uint64_t ticks, std::uint64_t frequency) {
        return frequency == 0 ? 0.0L :
            static_cast<long double>(ticks) * 1000000000.0L / frequency;
    };
    const auto close = [](long double measured, std::uint64_t wall) {
        const auto difference = measured > wall ? measured - wall : wall - measured;
        return wall != 0 && difference <= static_cast<long double>(wall) / 20 + 1000000;
    };
    std::printf("PS5_FEX_CLOCK_SCOPE finalInstruction=%s rdtscpQualified=%u\n",
#if defined(__APPLE__)
        "RDTSC", 0u);
#else
        "RDTSCP", 1u);
#endif
    for (unsigned index = 0; index < 3 && pass; ++index) {
        clock_wait = {};
        Output output {};
        pass &= Memory::WriteGuestMemory(nullptr, output_base, &output, sizeof(output));
        LumenAndroidFexCoreGuestTscV1 before, after;
        pass &= LumenAndroidFexCoreQueryGuestTscV1(&before) ==
            LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1;
        const std::uint64_t arguments[] {output_base};
        std::uint64_t value {};
        const bool invoked = pass && Invoke(code_base, arguments, 1, &value, stack_top);
        pass &= LumenAndroidFexCoreQueryGuestTscV1(&after) ==
            LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1;
        pass &= invoked && clock_wait.calls == 1 &&
            Memory::ReadGuestMemory(nullptr, output_base, &output, sizeof(output));
        const auto numerator = static_cast<std::uint64_t>(output.cpuid[2]) * output.cpuid[1];
        const auto declared = output.cpuid[0] != 0 ? numerator / output.cpuid[0] : 0;
        const bool frequency_agreement = output.cpuid[0] != 0 &&
            numerator % output.cpuid[0] == 0 && declared != 0 &&
            declared == output.frequency && declared == before.frequency &&
            declared == after.frequency;
        const bool origin_agreement = before.counter <= output.rdtsc_before &&
            output.rdtsc_before <= output.kernel_before &&
            output.kernel_before <= clock_wait.before.tsc &&
            clock_wait.before.tsc <= clock_wait.after.tsc &&
            clock_wait.after.tsc <= output.kernel_after &&
            output.kernel_after <= output.rdtsc_after &&
            output.rdtsc_after <= output.final_counter && output.final_counter <= after.counter;
        const auto wall = clock_wait.after.wall_ns - clock_wait.before.wall_ns;
        const auto tsc_delta = clock_wait.after.tsc - clock_wait.before.tsc;
        const auto guest_delta = output.rdtsc_after - output.rdtsc_before;
        const auto process_us = clock_wait.after.process_us - clock_wait.before.process_us;
        const auto process_counter = clock_wait.after.process_counter - clock_wait.before.process_counter;
        // Guest brackets include typed calls and continuation compilation;
        // compare their containment, not their rate against the shorter wait.
        const bool elapsed_agreement = wall >= 50000000 && wall < 2000000000 &&
            output.wait_result == static_cast<std::uint64_t>(::OK) &&
            tsc_delta <= guest_delta && guest_delta <= after.counter - before.counter &&
            close(to_ns(tsc_delta, output.frequency), wall) &&
            close(to_ns(process_counter, process_frequency), wall) &&
            close(static_cast<long double>(process_us) * 1000, wall);
        std::printf("PS5_FEX_CLOCK_SAMPLE index=%u nativeFrequency=%llu scale=%u "
            "cpuidFrequency=%llu kernelFrequency=%llu processFrequency=%llu wallNs=%llu "
            "frequencyAgreement=%u originAgreement=%u elapsedAgreement=%u\n", index,
            static_cast<unsigned long long>(before.nativeFrequency), before.scale,
            static_cast<unsigned long long>(declared),
            static_cast<unsigned long long>(output.frequency),
            static_cast<unsigned long long>(process_frequency),
            static_cast<unsigned long long>(wall), static_cast<unsigned>(frequency_agreement),
            static_cast<unsigned>(origin_agreement), static_cast<unsigned>(elapsed_agreement));
        pass &= frequency_agreement && origin_agreement && elapsed_agreement;
    }
    pass &= Memory::FreeGuestMemory(code_base, 0x4000);
    pass &= Memory::FreeGuestMemory(output_base, 0x4000);
    return pass;
}


std::uint64_t NativeOwnedAudioQueue() {
    return Libs::Audio::AudioInternal::OwnedOutputQueueSelfTest() ? 1 : UINT64_MAX;
}
} // namespace

#include "fexCallbackStackTests.inc"
#include "fexDirectAllocationTests.inc"
#include "fexDirectMapTests.inc"
#include "fexVirtualQueryTests.inc"

namespace {
std::array<std::array<std::uint64_t, 2>, 6> direct_release_arguments {};
std::array<std::uint32_t, 6> direct_release_results {};
bool direct_release_gate_denied {}, direct_release_lease_preserved {};

std::uint64_t NativeDirectReleaseOuter(std::uint64_t callback, std::uint64_t gate_callback) {
    bool pass = true;
    for (std::size_t index = 0; index < direct_release_arguments.size(); ++index) {
        std::uint64_t result {};
        pass = Invoke(callback, direct_release_arguments[index].data(), 2, &result) && pass;
        direct_release_results[index] = static_cast<std::uint32_t>(result);
    }
    std::atomic<bool> held {}, release {};
    std::thread owner([&] {
        GuestWriterGate gate(GuestWriterGate::Admission::NativeRootEntry);
        held.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!release.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    while (!held.load(std::memory_order_acquire)) std::this_thread::yield();
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t denied {};
    const bool invoked = Invoke(gate_callback, nullptr, 0, &denied);
    direct_release_gate_denied = invoked && denied == 1 &&
        std::chrono::steady_clock::now() - start < std::chrono::seconds(1);
    release.store(true, std::memory_order_release);
    owner.join();
    return pass && direct_release_gate_denied ? 1 : 0;
}

std::uint64_t NativeNestedGateProbe() {
    GuestWriterGate gate(GuestWriterGate::Admission::FreshScalarEntry);
    const bool retained = LumenAndroidFexCoreRetainGuestDataLeaseV1() != 0;
    direct_release_lease_preserved = retained && LumenAndroidFexCoreReleaseGuestDataLeaseV1() != 0;
    return !gate.Acquired() && direct_release_lease_preserved ? 1 : 0;
}

bool TestDirectReleaseNested(std::uint64_t stack_top) {
    if (!RegisterHostFunction<Memory::KernelCheckedReleaseDirectMemory>("self-test-direct-release") ||
        !RegisterHostFunction<NativeDirectReleaseOuter>("self-test-direct-release-outer") ||
        !RegisterHostFunction<NativeNestedGateProbe>("self-test-nested-gate-probe")) return false;
    constexpr std::uint64_t page = 0x4000, marker = 0x72656c6561736501ULL;
    std::array<std::int64_t, 4> physical {-1, -1, -1, -1};
    for (std::size_t index = 0; index < physical.size(); ++index)
        if (Memory::KernelAllocateDirectMemory(0, Memory::KernelGetDirectMemorySize(),
                index == 3 ? page : 2 * page, page, 3, &physical[index]) != ::OK) return false;
    std::array<void*, 3> aliases {};
    bool pass = Memory::KernelMapDirectMemory(&aliases[0], 2 * page, 3, 0, physical[2], page) == ::OK &&
        Memory::KernelMapDirectMemory(&aliases[1], page, 3, 0, physical[3], page) == ::OK &&
        Memory::KernelMapDirectMemory(&aliases[2], page, 3, 0, physical[3], page) == ::OK;
    for (const auto address: aliases)
        pass = address != nullptr && Memory::WriteGuestMemory(nullptr,
            reinterpret_cast<std::uint64_t>(address), &marker, sizeof(marker)) && pass;
    const auto code_base = Memory::AllocateProgramMemory(0, page, Vm::Mode::ReadWrite,
                                                        "fex-direct-release-code");
    if (!pass || code_base == 0) return false;
    std::array<std::uint8_t, 1024> code {};
    const auto relay = [&](std::size_t offset, std::uint64_t target) {
        constexpr std::uint8_t prefix[] {0x48,0x83,0xec,0x08,0x48,0xb8};
        constexpr std::uint8_t tail[] {0xff,0xd0,0x48,0x83,0xc4,0x08,0xc3};
        std::memcpy(code.data() + offset, prefix, sizeof(prefix));
        Put64(code, offset + sizeof(prefix), target);
        std::memcpy(code.data() + offset + sizeof(prefix) + 8, tail, sizeof(tail));
    };
    relay(0, reinterpret_cast<std::uint64_t>(NativeDirectReleaseOuter));
    relay(64, reinterpret_cast<std::uint64_t>(Memory::KernelCheckedReleaseDirectMemory));
    relay(128, reinterpret_cast<std::uint64_t>(NativeNestedGateProbe));
    pass = Memory::WriteGuestMemory(nullptr, code_base, code.data(), code.size()) &&
        Memory::ProtectGuestMemory(code_base, page, Vm::Mode::ExecuteRead) && pass;
    direct_release_arguments = {{
        {static_cast<std::uint64_t>(physical[0]), 2 * page},
        {static_cast<std::uint64_t>(physical[1]), page},
        {static_cast<std::uint64_t>(physical[2]), 2 * page},
        {static_cast<std::uint64_t>(physical[2]) + page, page},
        {static_cast<std::uint64_t>(physical[3]), page},
        {static_cast<std::uint64_t>(physical[3]), page + 1}
    }};
    direct_release_results = {};
    direct_release_gate_denied = direct_release_lease_preserved = false;
    const std::array<std::uint64_t, 2> callbacks {code_base + 64, code_base + 128};
    std::uint64_t value {};
    const bool invoked = Invoke(code_base, callbacks.data(), callbacks.size(), &value, stack_top);
    CheckCase("direct-release-nested-unmapped-complete-and-partial-metadata",
        invoked && value == 1 && direct_release_results[0] == ::OK && direct_release_results[1] == ::OK, pass);
    CheckCase("direct-release-nested-mapped-partial-and-alias-return-busy",
        direct_release_results[2] == static_cast<std::uint32_t>(Libs::LibKernel::KERNEL_ERROR_EBUSY) &&
        direct_release_results[3] == static_cast<std::uint32_t>(Libs::LibKernel::KERNEL_ERROR_EBUSY) &&
        direct_release_results[4] == static_cast<std::uint32_t>(Libs::LibKernel::KERNEL_ERROR_EBUSY) &&
        direct_release_results[5] == static_cast<std::uint32_t>(Libs::LibKernel::KERNEL_ERROR_EINVAL), pass);
    CheckCase("writer-gate-nested-borrower-denied-without-wait-or-lease-loss",
        direct_release_gate_denied && direct_release_lease_preserved, pass);
    for (const auto address: aliases) {
        std::uint64_t witness {};
        Memory::VirtualQueryInfo info {};
        pass = Memory::ReadGuestMemory(nullptr, reinterpret_cast<std::uint64_t>(address),
            &witness, sizeof(witness)) && witness == marker &&
            Memory::KernelVirtualQuery(address, 0, &info, sizeof(info)) == ::OK && info.is_direct && pass;
    }
    CheckCase("direct-release-busy-keeps-all-mapping-aliases-and-bytes", pass, pass);
    pass = Memory::KernelCheckedReleaseDirectMemory(physical[0], 2 * page) ==
        Libs::LibKernel::KERNEL_ERROR_ENOENT && pass;
    pass = Memory::KernelCheckedReleaseDirectMemory(physical[1] + page, page) == ::OK && pass;
    pass = Memory::KernelCheckedReleaseDirectMemory(physical[1], page) ==
        Libs::LibKernel::KERNEL_ERROR_ENOENT && pass;
    for (std::size_t index = 2; index < physical.size(); ++index)
        pass = Memory::KernelCheckedReleaseDirectMemory(physical[index], index == 3 ? page : 2 * page) == ::OK && pass;
    pass = Memory::FreeGuestMemory(code_base, page) && pass;
    return pass;
}
} // namespace
#include "fexSyncHandleTests.inc"
#include "fexShortGuestMemoryTests.inc"
#include "fexRwlockWaitTests.inc"
#include "fexFrameWaitTests.inc"
#include "fexNetworkWaitTests.inc"
#include "fexNetCtlCallbackTests.inc"
#include "fexFreshCallbackMappingTests.inc"
#include "fexCpuDataWriteTests.inc"
#include "fexSocketFlagsTests.inc"
#include "fexReserveTests.inc"

namespace {
std::uint64_t NativeErrnoValue() {
    return static_cast<std::uint32_t>(*Libs::Posix::GetErrorAddr());
}

bool ManagedErrnoSelfTest(std::uint64_t stack_top) {
    if (!RegisterHostFunction<Libs::Posix::GetErrorAddr>("self-test-owned-errno") ||
        !RegisterHostFunction<NativeErrnoValue>("self-test-native-errno-read")) return false;
    Libs::Posix::PrepareCurrentThreadError();
    auto* main_error = Libs::Posix::GetErrorAddr();
    *main_error = 0;
    const auto code = Memory::AllocateProgramMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                    "managed errno contract code");
    const auto child_stack = Memory::AllocateGuestStackMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                         "managed errno child stack");
    if (code == 0 || child_stack == 0) return false;
    std::vector<std::uint8_t> bytes;
    const auto emit = [&](std::initializer_list<std::uint8_t> values) { bytes.insert(bytes.end(), values); };
    const auto word = [&](std::uint64_t value) {
        for (unsigned shift = 0; shift != 64; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    const auto call = [&](auto target) {
        emit({0x48, 0xb8}); word(reinterpret_cast<std::uint64_t>(target)); emit({0xff, 0xd0});
    };
    emit({0x48, 0x83, 0xec, 8});
    call(Libs::Posix::GetErrorAddr);
    emit({0xc7, 0x00, 0x68, 0x24, 0x57, 0x13}); // guest store into its errno cell
    call(NativeErrnoValue);
    emit({0x48, 0x83, 0xc4, 8, 0xc3});
    const auto read_entry = code + bytes.size();
    emit({0x48, 0x83, 0xec, 8}); call(Libs::Posix::GetErrorAddr);
    emit({0x8b, 0x00, 0x48, 0x83, 0xc4, 8, 0xc3});
    bool pass = Memory::WriteGuestMemory(nullptr, code, bytes.data(), bytes.size()) &&
        Memory::ProtectGuestMemory(code, 0x4000, Vm::Mode::ExecuteRead);
    std::uint64_t result {};
    pass = pass && GuestExecution::Invoke(code, nullptr, 0, &result, stack_top) &&
        result == 0x13572468U && Libs::Posix::GetErrorAddr() == main_error &&
        *main_error == 0x13572468;
    *main_error = 0x24681357;
    pass = GuestExecution::Invoke(read_entry, nullptr, 0, &result, stack_top) &&
        result == 0x24681357U && pass;
    std::uint64_t child_error {};
    bool child_pass = false;
    std::thread child([&] {
        Libs::Posix::PrepareCurrentThreadError();
        child_error = reinterpret_cast<std::uint64_t>(Libs::Posix::GetErrorAddr());
        std::uint64_t value {};
        child_pass = child_error != reinterpret_cast<std::uint64_t>(main_error) &&
            *Libs::Posix::GetErrorAddr() == 0 &&
            GuestExecution::Invoke(code, nullptr, 0, &value, child_stack + 0x4000) &&
            value == 0x13572468U;
    });
    child.join();
    pass = child_pass && *main_error == 0x24681357 &&
        !Memory::QueryGuestAccess(child_error, sizeof(int), Vm::Mode::Read) && pass;
    *main_error = 0;
    pass = Memory::FreeGuestMemory(code, 0x4000) && pass;
    // Guest stack ownership has its own unmap contract; FreeGuestMemory is
    // deliberately limited to Code and Runtime ranges.
    pass = Memory::KernelMunmap(child_stack, 0x4000) == ::OK && pass;
    pass = !Memory::QueryGuestAccess(child_stack, 8, Vm::Mode::Read) && pass;
    return pass;
}
} // namespace

bool RuntimeSelfTest() noexcept {
    Common::InitializeThreads();
    // Production constructs the loader on the main native thread before
    // pthread entry/TLS cleanup can consult it from a guest worker.
    (void)Common::Singleton<Loader::RuntimeLinker>::Instance();
    Common::Subsystems subsystems;
    subsystems.Initialize<Config::Lifecycle>();
    Config::ConfigOptions options;
    options.printf_direction = Config::LogDirection::Silent;
    Config::Load(options);
    subsystems.Initialize<Log::Lifecycle>();
    subsystems.Initialize<Libs::Network::Lifecycle>();
    subsystems.Initialize<Memory::Lifecycle>();
    subsystems.Initialize<Libs::LibKernel::FileSystem::Lifecycle>();
    subsystems.Initialize<Libs::LibKernel::PthreadLifecycle>();
    Libs::LibKernel::PthreadInitSelfForMainThread();
    const auto stack_top = reinterpret_cast<std::uint64_t>(Libs::LibKernel::PthreadCreateMainGuestStack());
    bool pass = MemoryAccessSelfTest();
    CheckCase("constant-lane-mask-wave32-wave64", ConstantLaneMaskSelfTest(), pass);
    CheckCase("tls-image-thread-isolation", Loader::TestThreadTlsIsolation(), pass);
    if (!InitializeRuntime() || !RegisterHostFunction<NativeSum8>("self-test-sum-eight") ||
        !RegisterHostFunction<NativeNested>("self-test-nested-callback") ||
        !RegisterHostFunction<Libs::Posix::pthread_once>("self-test-pthread-once") ||
        !RegisterHostFunction<Libs::Posix::shutdown>("self-test-posix-shutdown") ||
        !RegisterHostFunction<NativeThreadExit>("self-test-thread-exit") ||
        !RegisterHostFunction<NativeJoinParent>("self-test-real-pthread-join") ||
        !RegisterHostFunction<NativeJoinMutation>("self-test-join-peer-vm-mutation") ||
        !RegisterHostFunction<NativeCompetingVmWorker>("self-test-competing-vm-worker") ||
        !RegisterHostFunction<NativeHostWaitReader>("self-test-host-wait-reader") ||
        !RegisterHostFunction<NativeHostWaitMutation>("self-test-host-wait-vm-peer") ||
        !RegisterHostFunction<NativeOwnedAudioQueue>("self-test-owned-audio-queue") ||
        !RegisterHostFunction<NativeRegisterRoundtrip>("self-test-integer-eightbyte")) return false;
    CheckCase("guest-owned-sanitizer-replacement-abi", Libs::LibKernel::SanitizerReplacementObjectsSelfTest(stack_top), pass);
    CheckCase("guest-owned-libc-heap-trace-pointer-graph", Libs::LibcInternalExt::HeapTraceObjectsSelfTest(stack_top), pass);
    CheckCase("guest-owned-errno-thread-contract", ManagedErrnoSelfTest(stack_top), pass);
    CheckCase("guest-owned-agc-register-defaults-pointer-graph", Libs::Graphics::Gen5::RegisterDefaultsSelfTest(stack_top), pass);
    CheckCase("guest-tsc-hle-domain-and-process-units", GuestTscSelfTest(stack_top), pass);
    CheckCase("guest-thread-id-scalar-contract", GuestThreadIdSelfTest(stack_top), pass);
    CheckCase("prepared-module-tls-nested-callbacks", Loader::TestNestedThreadTlsLookup(stack_top), pass);
    CheckCase("callback-stack-contract", CallbackStackSelfTest(stack_top), pass);
    CheckCase("direct-allocation-nested-contract", TestDirectAllocationNested(stack_top), pass);
    CheckCase("direct-map-nested-contract", TestDirectMapNested(stack_top), pass);
    CheckCase("direct-release-nested-contract", TestDirectReleaseNested(stack_top), pass);
    CheckCase("virtual-query-nested-contract", TestVirtualQueryNested(stack_top), pass);
    CheckCase("sync-handle-contract", TestSyncHandles(stack_top), pass);
    CheckCase("short-guest-memory-unlock-contract", TestShortGuestMemoryUnlock(stack_top), pass);
    CheckCase("rwlock-native-wait-contract", TestRwlockHostWait(), pass);
    CheckCase("frame-event-native-wait-contract", TestFrameEventHostWait(stack_top), pass);
    CheckCase("network-native-receive-wait-contract", TestNetworkReceiveWait(stack_top), pass);
    CheckCase("netctl-callback-reentrancy-and-generation", TestNetCtlCallbacks(stack_top), pass);
#if defined(LUMEN_WITH_QEMU_TCG) && LUMEN_WITH_QEMU_TCG
    if (Lumen::GuestCpu::Selected() == Lumen::GuestCpu::Provider::QemuTcg) {
        CheckCase("fresh-callback-actual-mapping-lifetime", TestFreshCallbackMappingLifetime(stack_top), pass);
        CheckCase("fresh-callback-actual-code-write-lifetime", TestFreshCallbackCodeWriteLifetime(stack_top), pass);
    }
#endif
    CheckCase("posix-fcntl-descriptor-contract", TestSocketFlags(stack_top), pass);
    CheckCase("cpu-data-write-publication-contract", TestCpuDataWritePublication(stack_top), pass);
    CheckCase("reserve-virtual-range-contract", TestReserveVirtualRange(stack_top), pass);
    CheckCase("owned-hle-objects", Loader::HostObjectSelfTest(), pass);
    CheckCase("upstream-function-fallback", Loader::UnresolvedFallbackSelfTest(stack_top), pass);
    const auto code_base = Memory::AllocateProgramMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                         "fex-runtime-test-code");
    const auto tls = Memory::AllocateRuntimeMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                  "fex-runtime-test-tls");
    const auto denied_write = Memory::AllocateRuntimeMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                           "fex-runtime-test-read-only");
    const auto exception_witness = Memory::AllocateRuntimeMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                                "fex-runtime-test-exception");
    const auto competing_records = Memory::AllocateRuntimeMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                                 "fex-competing-vm-records");
    if (code_base == 0 || tls == 0 || denied_write == 0 || exception_witness == 0 ||
        competing_records == 0) return false;
    constexpr std::uint64_t untouched = 0xaabbccddeeff0102ULL;
    pass &= Memory::WriteGuestMemory(nullptr, tls, &tls, 8);
    pass &= Memory::WriteGuestMemory(nullptr, denied_write, &untouched, 8);
    pass &= Memory::ProtectGuestMemory(denied_write, 0x4000, Vm::Mode::Read);
    std::array<std::uint64_t, 3> delivered {};
    pass &= Memory::WriteGuestMemory(nullptr, exception_witness, delivered.data(),
                                    sizeof(delivered));

    std::array<std::uint8_t, 1024> code {};
    // Relay all six register arguments and both stack arguments to typed HLE.
    constexpr std::uint8_t sum_prefix[] {
        0x48,0x83,0xec,0x18, 0x48,0x8b,0x44,0x24,0x20, 0x48,0x89,0x04,0x24,
        0x48,0x8b,0x44,0x24,0x28, 0x48,0x89,0x44,0x24,0x08, 0x48,0xb8
    };
    std::memcpy(code.data(), sum_prefix, sizeof(sum_prefix));
    Put64(code, sizeof(sum_prefix), reinterpret_cast<std::uint64_t>(NativeSum8));
    constexpr std::uint8_t call_return[] {0xff,0xd0,0x48,0x83,0xc4,0x18,0xc3};
    std::memcpy(code.data() + sizeof(sum_prefix) + 8, call_return, sizeof(call_return));
    constexpr std::uint8_t nested_guest[] {0x48,0x8d,0x47,0x05,0xc3};
    std::memcpy(code.data() + 64, nested_guest, sizeof(nested_guest));
    constexpr std::uint8_t root_return[] {0xb8,0x2a,0x00,0x00,0x00,0xc3};
    std::memcpy(code.data() + 96, root_return, sizeof(root_return));
    constexpr std::uint8_t fs_load[] {0x64,0x48,0x8b,0x04,0x25,0x00,0x00,0x00,0x00,0xc3};
    std::memcpy(code.data() + 112, fs_load, sizeof(fs_load));
    code[128] = 0x0f; code[129] = 0x0b; // UD2 must remain a real guest fault.
    code[160] = 0x48; code[161] = 0xb8;
    Put64(code, 162, denied_write);
    constexpr std::uint8_t write_bytes[] {0x48,0xc7,0x00,0x44,0x33,0x22,0x11,0xc3};
    std::memcpy(code.data() + 170, write_bytes, sizeof(write_bytes));
    constexpr std::uint8_t nested_prefix[] {0x48,0x83,0xec,0x08,0x48,0xb8};
    std::memcpy(code.data() + 192, nested_prefix, sizeof(nested_prefix));
    Put64(code, 198, reinterpret_cast<std::uint64_t>(NativeNested));
    constexpr std::uint8_t nested_tail[] {0xff,0xd0,0x48,0x83,0xc4,0x08,0x48,0x83,0xc0,0x07,0xc3};
    std::memcpy(code.data() + 206, nested_tail, sizeof(nested_tail));
    // The actual guest handler reads its managed ucontext and records the
    // delivered signal, original fault RIP and temporary guest record address.
    code[224] = 0x48; code[225] = 0xb8;
    Put64(code, 226, exception_witness);
    constexpr std::uint8_t exception_handler[] {
        0x48,0x89,0x38, 0x48,0x8b,0x8e,0xa0,0x00,0x00,0x00,
        0x48,0x89,0x48,0x08, 0x48,0x89,0x70,0x10, 0xc3
    };
    std::memcpy(code.data() + 234, exception_handler, sizeof(exception_handler));
    // A native HLE exit request must stop FEX before the following guest MOV.
    std::memcpy(code.data() + 288, nested_prefix, sizeof(nested_prefix));
    Put64(code, 294, reinterpret_cast<std::uint64_t>(NativeThreadExit));
    constexpr std::uint8_t exit_tail[] {0xff,0xd0,0x48,0x83,0xc4,0x08,0xb8,0x63,0,0,0,0xc3};
    std::memcpy(code.data() + 302, exit_tail, sizeof(exit_tail));
    constexpr std::uint8_t program_write[] {0x48,0xc7,0x07,0x44,0x33,0x22,0x11,0xc3};
    std::memcpy(code.data() + 320, program_write, sizeof(program_write));
    std::memcpy(code.data() + 352, nested_prefix, sizeof(nested_prefix));
    Put64(code, 358, reinterpret_cast<std::uint64_t>(NativeRegisterRoundtrip));
    constexpr std::uint8_t aggregate_tail[] {0xff,0xd0,0x48,0x83,0xc4,0x08,0xc3};
    std::memcpy(code.data() + 366, aggregate_tail, sizeof(aggregate_tail));
    std::memcpy(code.data() + 400, nested_prefix, sizeof(nested_prefix));
    Put64(code, 406, reinterpret_cast<std::uint64_t>(NativeJoinParent));
    std::memcpy(code.data() + 414, aggregate_tail, sizeof(aggregate_tail));
    std::memcpy(code.data() + 432, nested_prefix, sizeof(nested_prefix));
    Put64(code, 438, reinterpret_cast<std::uint64_t>(NativeJoinMutation));
    // Retry only the pre-join handshake in guest instructions, with no native
    // blocking wait that would itself retain an HLE reader. Once started, an
    // allocation/copy failure returns UINT64_MAX and cannot enter this loop.
    constexpr std::uint8_t join_child_tail[] {
        0xff,0xd0,0x48,0x85,0xc0,0x74,0xef,0x48,0x83,0xc4,0x08,0xc3
    };
    std::memcpy(code.data() + 446, join_child_tail, sizeof(join_child_tail));
    std::memcpy(code.data() + 560, nested_prefix, sizeof(nested_prefix));
    Put64(code, 566, reinterpret_cast<std::uint64_t>(NativeCompetingVmWorker));
    constexpr std::uint8_t competing_child_start[] {
        0xff,0xd0,0x48,0x85,0xc0,0x74,0xef, // CALL; retry guest loop only for pre-start zero
        0x48,0x83,0xf8,0xff,0x74,0x30,      // explicit failure goes straight to epilogue
        0x48,0xba
    };
    std::memcpy(code.data() + 574, competing_child_start, sizeof(competing_child_start));
    Put64(code, 589, competing_marker);
    constexpr std::uint8_t competing_child_write[] {
        0x48,0x03,0x10,                    // ADD RDX,[RAX]: distinct marker per worker
        0x48,0x8b,0x48,0x10,0x48,0x89,0x11,
        0x48,0x8b,0x48,0x20,0x48,0x89,0x11,
        0x48,0x8b,0x48,0x30,0x48,0x89,0x11,
        0x48,0x8b,0x48,0x40,0x48,0x89,0x11,
        0x48,0x8b,0x00,0x48,0x83,0xc0,0x01, // result is worker index + 1
        0x48,0x83,0xc4,0x08,0xc3
    };
    std::memcpy(code.data() + 597, competing_child_write, sizeof(competing_child_write));
    std::memcpy(code.data() + 672, nested_prefix, sizeof(nested_prefix));
    Put64(code, 678, reinterpret_cast<std::uint64_t>(NativeHostWaitReader));
    constexpr std::uint8_t host_wait_reader_tail[] {
        0xff,0xd0,0x48,0x85,0xc0,0x74,0xef,0x48,0x83,0xc4,0x08,0xc3
    };
    std::memcpy(code.data() + 686, host_wait_reader_tail, sizeof(host_wait_reader_tail));
    std::memcpy(code.data() + 736, nested_prefix, sizeof(nested_prefix));
    Put64(code, 742, reinterpret_cast<std::uint64_t>(NativeHostWaitMutation));
    constexpr std::uint8_t host_wait_mutation_start[] {
        0xff,0xd0,0x48,0x85,0xc0,0x74,0xef,0x48,0x83,0xf8,0xff,0x74,0x12,0x48,0xba
    };
    std::memcpy(code.data() + 750, host_wait_mutation_start, sizeof(host_wait_mutation_start));
    Put64(code, 765, host_wait_marker);
    constexpr std::uint8_t host_wait_mutation_tail[] {
        0x48,0x89,0x10,0xb8,0x01,0,0,0,0x48,0x83,0xc4,0x08,0xc3
    };
    std::memcpy(code.data() + 773, host_wait_mutation_tail, sizeof(host_wait_mutation_tail));
    std::memcpy(code.data() + 816, nested_prefix, sizeof(nested_prefix));
    Put64(code, 822, reinterpret_cast<std::uint64_t>(NativeOwnedAudioQueue));
    std::memcpy(code.data() + 830, aggregate_tail, sizeof(aggregate_tail));
    // Exercise the actual once HLE from translated code; its initializer is
    // another x86 function, never a native ARM callback.
    std::memcpy(code.data() + 864, nested_prefix, sizeof(nested_prefix));
    Put64(code, 870, reinterpret_cast<std::uint64_t>(Libs::Posix::pthread_once));
    std::memcpy(code.data() + 878, aggregate_tail, sizeof(aggregate_tail));
    code[896] = 0x48; code[897] = 0xb8;
    const auto once_counter_address = exception_witness + 64;
    Put64(code, 898, once_counter_address);
    constexpr std::uint8_t once_initializer[] {0x83,0x00,0x01,0xc3};
    std::memcpy(code.data() + 906, once_initializer, sizeof(once_initializer));
    std::memcpy(code.data() + 928, nested_prefix, sizeof(nested_prefix));
    Put64(code, 934, reinterpret_cast<std::uint64_t>(Libs::Posix::shutdown));
    std::memcpy(code.data() + 942, aggregate_tail, sizeof(aggregate_tail));
    pass &= Memory::WriteGuestMemory(nullptr, code_base, code.data(), code.size());
    Memory::SetProgramMemoryProtection(code_base, 0x4000, Vm::Mode::ExecuteRead);

    // Program code uses the real native read-only protection on FEX too.
    // Its relocation writer must temporarily open and restore that protection.
    std::uint64_t code_word {};
    bool relocation_changed = true;
    pass &= Memory::ReadGuestMemory(nullptr, code_base + 344, &code_word, 8);
    {
        GuestMutationScope loader_epoch;
        pass &= Memory::PatchProgramMemory64(code_base + 344, code_word, &relocation_changed);
    }
    CheckCase("relocation-preserves-program-rx", !relocation_changed &&
        !Memory::WriteGuestMemory(nullptr, code_base + 344, &code_word, 8), pass);
    auto program_write_request = TestRequest(code_base + 320, stack_top);
    program_write_request.state.gprs[7] = code_base + 344;
    const auto program_write_run = Run(program_write_request);
    CheckCase("guest-aligned-write-to-program-rx-faults", !program_write_run.completed &&
        program_write_run.code == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
        program_write_run.state.faultAddress == code_base + 344, pass);

    std::uint64_t value {};
    CheckCase("entry-ret", Invoke(code_base + 96, nullptr, 0, &value, stack_top) && value == 42, pass);
    const std::array<std::uint64_t, 8> args {1,2,3,4,5,6,7,8};
    CheckCase("typed-hle-eight-arguments", Invoke(code_base, args.data(), args.size(), &value,
                                                stack_top) && value == 204, pass);
    const AgcRegister register_input {0x10203040, 0x8090a0b0};
    std::uint64_t register_word {};
    std::memcpy(&register_word, &register_input, sizeof(register_input));
    const std::array<std::uint64_t, 2> register_args {0x55667788, register_word};
    const bool register_invoked = Invoke(code_base + 352, register_args.data(),
                                         register_args.size(), &value, stack_top);
    AgcRegister register_output {};
    std::memcpy(&register_output, &value, sizeof(register_output));
    CheckCase("typed-hle-declared-integer-eightbyte", register_invoked &&
        last_marshaled_register.offset == register_input.offset &&
        last_marshaled_register.value == register_input.value &&
        register_output.offset == (register_input.value ^ 0x55667788) &&
        register_output.value == register_input.offset + 3, pass);
    const std::array<std::uint64_t, 2> nested_args {code_base + 64, 27};
    CheckCase("nested-guest-callback", Invoke(code_base + 192, nested_args.data(), 2, &value,
                                              stack_top) && value == 42, pass);
    const auto once_control = exception_witness + 80;
    const auto second_once_control = exception_witness + 96;
    const std::array<std::uint64_t, 4> zero_once_controls {};
    std::uint32_t once_counter = 0;
    pass &= Memory::WriteGuestMemory(nullptr, once_control, zero_once_controls.data(),
                                     sizeof(zero_once_controls));
    pass &= Memory::WriteGuestMemory(nullptr, once_counter_address, &once_counter,
                                     sizeof(once_counter));
    auto check_once = [&](std::uint64_t control, std::uint32_t expected_count) {
        const std::array<std::uint64_t, 2> once_args {control, code_base + 896};
        std::uint32_t state = 0;
        return Invoke(code_base + 864, once_args.data(), once_args.size(), &value, stack_top) &&
            value == ::OK &&
            Memory::ReadGuestMemory(nullptr, once_counter_address, &once_counter,
                                    sizeof(once_counter)) && once_counter == expected_count &&
            Memory::ReadGuestMemory(nullptr, control, &state, sizeof(state)) && state == 1;
    };
    CheckCase("pthread-once-translated-initializer", check_once(once_control, 1), pass);
    CheckCase("pthread-once-completed-not-repeated", check_once(once_control, 1), pass);
    CheckCase("pthread-once-independent-control", check_once(second_once_control, 2), pass);
    const std::array<std::uint64_t, 2> null_once_args {0, code_base + 896};
    CheckCase("pthread-once-null-control-rejected", Invoke(code_base + 864,
        null_once_args.data(), null_once_args.size(), &value, stack_top) &&
        value == Libs::Posix::POSIX_EINVAL, pass);
    const std::uint64_t exit_value = 0x1234;
    const auto check_shutdown = [&](int fd, int how, int expected_errno) {
        *Libs::Posix::GetErrorAddr() = 0;
        const std::array<std::uint64_t, 2> shutdown_args {
            static_cast<std::uint64_t>(fd), static_cast<std::uint64_t>(how)};
        return Invoke(code_base + 928, shutdown_args.data(), shutdown_args.size(),
                      &value, stack_top) && static_cast<int>(value) == -1 &&
               *Libs::Posix::GetErrorAddr() == expected_errno;
    };
    CheckCase("posix-shutdown-invalid-descriptor-keeps-errno",
        check_shutdown(-1, 0, Libs::Posix::POSIX_EBADF), pass);
    const auto socket = Libs::Network::Net::Socket(2, 1, 0);
    CheckCase("posix-shutdown-invalid-how-keeps-errno", socket >= 0 &&
        check_shutdown(socket, 3, Libs::Posix::POSIX_EINVAL), pass);
#if !defined(_WIN32)
    CheckCase("posix-shutdown-unimplemented-backend-keeps-enosys", socket >= 0 &&
        check_shutdown(socket, 0, Libs::Posix::POSIX_ENOSYS), pass);
#endif
    if (socket >= 0) pass &= Libs::Network::Net::SocketClose(socket) == ::OK;
    CheckCase("thread-exit-stops-guest", Invoke(code_base + 288, &exit_value, 1, &value,
              stack_top) && value == exit_value, pass);
    const std::array<std::uint64_t, 2> nested_exit_args {code_base + 288, exit_value};
    CheckCase("nested-thread-exit-propagates", Invoke(code_base + 192, nested_exit_args.data(), 2,
              &value, stack_top) && value == exit_value, pass);
    CheckCase("thread-exit-state-cleared", Invoke(code_base + 96, nullptr, 0, &value,
              stack_top) && value == 42, pass);
    // The guest parent enters the real join HLE, and the PthreadCreate child
    // enters a separate real FEX run. Its checked VM operation must finish
    // while the parent waits; retaining the parent's HLE reader causes the
    // existing bounded mutation-admission timeout instead. Neither thread
    // accesses the guest output slot during the quiescent native wait.
    const auto join_result_slot = exception_witness + 32;
    constexpr std::uint64_t join_output_before = UINT64_MAX;
    bool join_output_valid = Memory::WriteGuestMemory(nullptr, join_result_slot,
        &join_output_before, sizeof(join_output_before));
    join_started.store(false, std::memory_order_release);
    join_mutation_address.store(0, std::memory_order_release);
    const std::array<std::uint64_t, 2> join_arguments {code_base + 432, join_result_slot};
    const auto join_begin = std::chrono::steady_clock::now();
    const bool join_invoked = Invoke(code_base + 400, join_arguments.data(),
                                     join_arguments.size(), &value, stack_top);
    const auto join_elapsed = std::chrono::steady_clock::now() - join_begin;
    std::uint64_t joined_value {}, mutation_value {};
    join_output_valid &= Memory::ReadGuestMemory(nullptr, join_result_slot, &joined_value,
                                                sizeof(joined_value));
    const auto mutation_address = join_mutation_address.load(std::memory_order_acquire);
    const bool mutation_valid = mutation_address != 0 &&
        Memory::ReadGuestMemory(nullptr, mutation_address, &mutation_value,
                                sizeof(mutation_value));
    CheckCase("pthread-join-quiescent-vm-mutation", join_invoked && value == ::OK &&
        join_started.load(std::memory_order_acquire) && join_output_valid &&
        joined_value == join_return_value && mutation_valid &&
        mutation_value == join_mutation_marker && join_elapsed < std::chrono::seconds(5), pass);
    if (mutation_address != 0) pass &= Memory::FreeGuestMemory(mutation_address, 0x4000);
    auto competing_regression = [&](bool create_children) {
    competing_create_entry.store(create_children ? code_base + 96 : 0, std::memory_order_release);
    competing_children_joined.store(0, std::memory_order_release);
    std::array<CompetingVmRecord, 2> competing_output {};
    bool competing_valid = true;
    for (std::size_t i = 0; i != competing_output.size(); ++i) {
        competing_output[i].worker = i;
        competing_valid &= Memory::WriteGuestMemory(nullptr, competing_records + i * 128,
            &competing_output[i], sizeof(competing_output[i]));
    }
    const auto competing_begin = std::chrono::steady_clock::now();
    // Create both real guest workers from native main: their initial Run
    // preflight and stack allocation complete before the competing scalar
    // allocation/map/unmap/release sequence. The separate join
    // testcase above exercises a real guest parent waiting in HLE.
    competing_valid &= NativeCompetingVmParent(code_base + 560, competing_records) == ::OK;
    const auto competing_elapsed = std::chrono::steady_clock::now() - competing_begin;
    competing_valid &= competing_ready.load(std::memory_order_acquire) == 2 &&
        competing_thread_results == std::array<std::uint64_t, 2> {1, 2} &&
        competing_elapsed < std::chrono::seconds(5);
    std::vector<std::uint64_t> seen_mappings;
    std::vector<std::int64_t> seen_physical;
    for (std::size_t i = 0; i != competing_output.size(); ++i) {
        competing_valid &= Memory::ReadGuestMemory(nullptr, competing_records + i * 128,
            &competing_output[i], sizeof(competing_output[i]));
        for (const auto& item: competing_output[i].mappings) {
            std::uint64_t marker {};
            competing_valid &= item.physical >= 0 && item.mapped != 0 &&
                std::find(seen_mappings.begin(), seen_mappings.end(), item.mapped) == seen_mappings.end() &&
                std::find(seen_physical.begin(), seen_physical.end(), item.physical) == seen_physical.end() &&
                Memory::ReadGuestMemory(nullptr, item.mapped, &marker, sizeof(marker)) &&
                marker == competing_marker + i;
            seen_mappings.push_back(item.mapped);
            seen_physical.push_back(item.physical);
            // Cleanup runs on the native main thread after both real guest
            // children have joined; the retained mapping witnesses are checked above.
            if (item.mapped != 0)
                competing_valid &= Memory::KernelMunmap(item.mapped, 0x4000) == ::OK;
            if (item.physical >= 0)
                competing_valid &= Memory::KernelCheckedReleaseDirectMemory(item.physical, 0x4000) == ::OK;
        }
    }
    competing_valid &= competing_children_joined.load(std::memory_order_acquire) ==
        (create_children ? 4U : 0U);
    CheckCase(create_children ? "competing-pthread-create-join-vm-mutators" :
        "competing-hle-direct-memory-mutators", competing_valid, pass);
    };
    competing_regression(false);
    competing_regression(true);
    const CompetingMapping empty_wait_mapping {};
    bool host_wait_valid = Memory::WriteGuestMemory(nullptr, competing_records,
        &empty_wait_mapping, sizeof(empty_wait_mapping));
    const auto host_wait_begin = std::chrono::steady_clock::now();
    const auto host_wait_witness = TestHostWaitReaders(code_base + 672, code_base + 736,
                                                       competing_records);
    host_wait_valid &= std::chrono::steady_clock::now() - host_wait_begin < std::chrono::seconds(2);
    CheckCase("usleep-hle-reader-yields-before-vm-mutation", host_wait_valid &&
              (host_wait_witness & 1) != 0, pass);
    CheckCase("semaphore-hle-reader-yields-before-vm-mutation", host_wait_valid &&
              (host_wait_witness & 2) != 0, pass);
    CheckCase("condition-timedwait-reader-yields-before-vm-mutation", host_wait_valid &&
              (host_wait_witness & 4) != 0, pass);
    CheckCase("condition-wait-reader-yields-and-pins-native-objects", host_wait_valid &&
              (host_wait_witness & 8) != 0, pass);
    CheckCase("equeue-reader-yields-and-checks-copyout-after-vm-mutation", host_wait_valid &&
              (host_wait_witness & 16) != 0, pass);
    CheckCase("owned-audio-delivers-pcm-before-readmission",
        Invoke(code_base + 816, nullptr, 0, &value, stack_top) && value == 1, pass);
    auto tls_request = TestRequest(code_base + 112, stack_top);
    tls_request.state.fsBase = tls;
    const auto tls_run = Run(tls_request);
    CheckCase("guest-fs-tls", tls_run.completed && tls_run.state.gprs[0] == tls &&
              tls_run.state.fsBase == tls, pass);

    const auto illegal = Run(TestRequest(code_base + 128, stack_top));
    CheckCase("guest-illegal-instruction", !illegal.completed &&
              illegal.code == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
              illegal.state.faultSignal == SIGILL, pass);
    const bool exception_registered = Libs::LibKernel::KernelInstallExceptionHandler(
        4, reinterpret_cast<void*>(code_base + 224)) == ::OK;
    const bool duplicate_exception_rejected = exception_registered &&
        Libs::LibKernel::KernelInstallExceptionHandler(4,
            reinterpret_cast<void*>(code_base + 224)) ==
                Libs::LibKernel::KERNEL_ERROR_EAGAIN;
    const bool exception_stays_terminal = exception_registered &&
        !Invoke(code_base + 128, nullptr, 0, &value, stack_top);
    const bool exception_witness_read = Memory::ReadGuestMemory(nullptr, exception_witness,
        delivered.data(), sizeof(delivered));
    const bool exception_removed = Libs::LibKernel::KernelRemoveExceptionHandler(4) == ::OK;
    CheckCase("guest-exception-handler-terminal-delivery", exception_registered &&
        duplicate_exception_rejected && exception_stays_terminal && exception_witness_read &&
        delivered[0] == 4 && delivered[1] == code_base + 128 && delivered[2] != 0 &&
        !Memory::QueryGuestAccess(delivered[2], 8, Vm::Mode::Read) && exception_removed, pass);
    const auto access = Run(TestRequest(code_base + 160, stack_top));
    std::uint64_t after {};
    CheckCase("guest-read-only-write-fault", !access.completed &&
              access.code == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
              (access.state.faultSignal == SIGSEGV || access.state.faultSignal == SIGBUS) &&
              access.state.faultAddress == denied_write &&
              Memory::ReadGuestMemory(nullptr, denied_write, &after, 8) && after == untouched, pass);

    Memory::SetProgramMemoryProtection(code_base, 0x4000, Vm::Mode::ReadWrite);
    const std::uint8_t new_constant = 43;
    pass &= Memory::WriteGuestMemory(nullptr, code_base + 97, &new_constant, 1);
    Memory::SetProgramMemoryProtection(code_base, 0x4000, Vm::Mode::ExecuteRead);
    CheckCase("code-mutation-invalidates-fex", Invoke(code_base + 96, nullptr, 0, &value,
                                                    stack_top) && value == 43, pass);
    pass &= Memory::FreeGuestMemory(code_base, 0x4000);
    pass &= Memory::FreeGuestMemory(tls, 0x4000);
    pass &= Memory::FreeGuestMemory(denied_write, 0x4000);
    pass &= Memory::FreeGuestMemory(exception_witness, 0x4000);
    pass &= Memory::FreeGuestMemory(competing_records, 0x4000);
    CheckCase("synthetic-tests-do-not-publish-readiness", !FullRuntimeReady(), pass);
    std::puts(pass ? "PS5 FEX full runtime self-test: PASS" : "PS5 FEX full runtime self-test: FAIL");
    return pass;
}
} // namespace Loader::GuestExecution
#endif
