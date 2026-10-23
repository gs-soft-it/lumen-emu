// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/alignment.h"
#include "common/arch.h"
#include "common/assert.h"
#include "common/elf_info.h"
#include "common/logging/formatter.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "common/string_util.h"
#include "common/thread.h"
#include "core/aerolib/aerolib.h"
#include "core/aerolib/stubs.h"
#include "core/devtools/widget/module_list.h"
#include "core/emulator_settings.h"
#include "core/debug_state.h"
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/fex_guest_cpu.h"
#include "core/guest_cpu/fex_hle_bridge.h"
#include "core/guest_cpu/guest_memory_validation_cache.h"
#include "core/guest_cpu/hle_call_adapter.h"
#endif
#ifdef LUMEN_GEN4_DIRECT_FEX
#include "core/libraries/fiber/fiber.h"
#endif
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/kernel/sync/host_wait.h"
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include "core/fex/fex_guest_engine.h"
#include "core/fex/guest_runtime_setup.h"
#endif
#include "core/libraries/kernel/threads.h"
#include "core/libraries/sysmodule/sysmodule.h"
#include "core/linker.h"
#ifdef LUMEN_GEN4_DIRECT_FEX
#include "core/file_sys/backends/host_fs.h"
#include "core/file_sys/fs.h"
#endif
#include "core/memory.h"
#include "core/tls.h"
#include "ipc/ipc.h"

#ifndef _WIN32
#include <signal.h>
#endif

namespace Core {

static PS4_SYSV_ABI void ProgramExitFunc() {
    LOG_ERROR(Core_Linker, "Exit function called");
}

#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
static bool ValidateGuestMemory(void* context, std::uintptr_t address, std::size_t size,
                                bool writable) {
    if (context == nullptr || address == 0 || size == 0 || address > UINTPTR_MAX - size) {
        return false;
    }
    auto* memory = static_cast<MemoryManager*>(context);
    const auto required = static_cast<u32>(MemoryProt::CpuRead) |
                          (writable ? static_cast<u32>(MemoryProt::CpuWrite) : 0);
    static thread_local GuestCpu::GuestMemoryValidationCache validation_cache;
    const auto generation = memory->MappingGeneration();
    if (validation_cache.Contains(memory, generation, address, size, required)) {
        return true;
    }
    const auto end = address + size;
    auto cursor = address;
    while (cursor < end) {
        void* range_start{};
        void* range_end{};
        u32 protection{};
        u64 range_generation{};
        if (memory->QueryProtection(cursor, &range_start, &range_end, &protection,
                                    &range_generation) != ORBIS_OK) {
            return false;
        }
        const auto mapped_begin = reinterpret_cast<std::uintptr_t>(range_start);
        const auto mapped_end = reinterpret_cast<std::uintptr_t>(range_end);
        if (mapped_end <= cursor || (protection & required) != required) {
            return false;
        }
        validation_cache.Store(memory, range_generation, mapped_begin, mapped_end, protection);
        cursor = std::min(end, mapped_end);
    }
    return true;
}

static bool SealGuestExecutableMemory(MemoryManager& memory, std::uintptr_t begin,
                                      std::size_t size) {
    const auto result = memory.SealGuestExecutable(begin, size);
    if (result == ORBIS_OK) return true;
    LOG_ERROR(Core_Linker, "Unable to seal FEX guest code begin={:#x} size={:#x}: {}", begin,
              size, result);
    return false;
}

// HLE veneers live in host mmap pages outside the guest VMM. Dynamic PRX
// loads allocate more of them after the FEX thread snapshot is taken, so the
// dynamic QueryGuestExecutableRange path must see them here. Context layout
// matches Linker::FexExecutableQueryContext.
static std::optional<GuestExecutionRange> QueryGuestExecutableMemory(void* context,
                                                                     std::uintptr_t address) {
    if (context == nullptr || address == 0) return std::nullopt;
    auto* query = static_cast<FexExecutableQueryContext*>(context);
    if (query->veneers != nullptr) {
        if (auto range = query->veneers->QueryExecutableRange(address)) {
            return range;
        }
    }
    if (query->memory == nullptr) return std::nullopt;
    auto* memory = query->memory;
    void* range_start{};
    void* range_end{};
    u32 protection{};
    if (memory->QueryProtection(address, &range_start, &range_end, &protection) != ORBIS_OK ||
        (protection & static_cast<u32>(MemoryProt::CpuExec)) == 0 ||
        (protection & static_cast<u32>(MemoryProt::CpuWrite)) != 0) {
        return std::nullopt;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(range_start);
    const auto end = reinterpret_cast<std::uintptr_t>(range_end);
    if (begin == 0 || end <= begin) return std::nullopt;
#ifdef LUMEN_GEN4_DIRECT_FEX
    if (!SealGuestExecutableMemory(*memory, begin, end - begin)) return std::nullopt;
#endif
    return GuestExecutionRange{begin, end - begin, true, false};
}

static void ReportGuestHleFailure(void*, const GuestCpu::HleCallFailure& failure) {
    LOG_ERROR(Core_Linker, "FEX HLE call {} failed: {}", failure.name, failure.error);
    Common::Log::Flush();
}
#endif

static PS4_SYSV_ABI void* RunMainEntry [[noreturn]] (EntryParams* params) {
#ifdef ARCH_X86_64
    // Start shared library modules
    asm volatile("andq $-16, %%rsp\n" // Align to 16 bytes
                 "subq $8, %%rsp\n"   // videoout_basic expects the stack to be misaligned

                 // Kernel also pushes some more things here during process init
                 // at least: environment, auxv, possibly other things

                 "pushq 8(%1)\n" // copy EntryParams to top of stack like the kernel does
                 "pushq 0(%1)\n" // OpenOrbis expects to find it there

                 "movq %1, %%rdi\n" // also pass params and exit func
                 "movq %2, %%rsi\n" // as before

                 "jmp *%0\n" // can't use call here, as that would mangle the prepared stack.
                             // there's no coming back
                 :
                 : "r"(params->entry_addr), "r"(params), "r"(ProgramExitFunc)
                 : "rax", "rsi", "rdi");
    UNREACHABLE();
#else
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    auto* linker = Common::Singleton<Linker>::Instance();
#ifndef LUMEN_GEN4_DIRECT_FEX
    try {
#endif
        const auto result = linker->RunGuestMain(params);
        if (const auto* failure = std::get_if<GuestExecutionFailure>(&result)) {
#ifdef LUMEN_GEN4_DIRECT_FEX
            LOG_CRITICAL(Core_Linker, "FEX main entry failed at stage {}: {}",
                         static_cast<int>(failure->Stage), failure->Error);
#else
            LOG_CRITICAL(Core_Linker,
                         "FEX main entry failed at stage {}: {} code={} rip={:#x} rsp={:#x} "
                         "signal={} fault={:#x}", static_cast<int>(failure->Stage), failure->Error,
                         failure->RuntimeCode, failure->Rip, failure->Rsp,
                         failure->FaultSignal, failure->FaultAddress);
#endif
        } else {
            LOG_CRITICAL(Core_Linker, "FEX main entry returned unexpectedly with {:#x}",
                         std::get<u64>(result));
        }
        Common::Log::Flush();
        std::abort();
#ifndef LUMEN_GEN4_DIRECT_FEX
    } catch (const Fex::GuestThreadExit&) {
        DebugState.RemoveCurrentThreadFromGuestList();
    }
    Libraries::Kernel::CompletePthreadExit();
#endif
#else
    UNREACHABLE_MSG("RunMainEntry requires an x86-64 host or FEX guest CPU support.");
#endif
#endif
}

Linker::Linker() : memory{Memory::Instance()} {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    m_hle_veneers = std::make_unique<GuestCpu::HleVeneerAllocator>();
#endif
}

Linker::~Linker() = default;

#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
namespace {

constexpr std::size_t FexTemporaryStackSize = 1_MB;

class GuestStackMapping final {
public:
    GuestStackMapping(MemoryManager& memory_, std::size_t size_, std::string_view name)
        : memory{memory_}, size{size_} {
        void* mapped{};
        const auto result = memory.MapMemory(&mapped, 0, size, MemoryProt::CpuReadWrite,
                                             MemoryMapFlags::NoFlags, VMAType::Stack, name);
        if (result == ORBIS_OK) {
            base = reinterpret_cast<VAddr>(mapped);
        } else {
            error = result;
        }
    }

    GuestStackMapping(const GuestStackMapping&) = delete;
    GuestStackMapping& operator=(const GuestStackMapping&) = delete;

    ~GuestStackMapping() {
        if (base != 0) {
            const auto result = memory.UnmapMemory(base, size);
            if (result != ORBIS_OK) {
                LOG_ERROR(Core_Linker, "Unable to unmap FEX guest stack {:#x}: {}", base,
                          result);
            }
        }
    }

    bool IsValid() const {
        return base != 0;
    }

    VAddr Top() const {
        return base + size;
    }

    int Error() const {
        return error;
    }

private:
    MemoryManager& memory;
    std::size_t size;
    VAddr base{};
    int error{};
};

std::optional<GuestExecutionRange> QueryGuestMemoryRange(MemoryManager& memory,
                                                         std::uintptr_t address,
                                                         bool executable, bool writable) {
    void* range_start{};
    void* range_end{};
    u32 protection{};
    if (memory.QueryProtection(address, &range_start, &range_end, &protection) != ORBIS_OK) {
        return std::nullopt;
    }
    if (executable && ((protection & static_cast<u32>(MemoryProt::CpuExec)) == 0 ||
                       (protection & static_cast<u32>(MemoryProt::CpuWrite)) != 0)) {
        return std::nullopt;
    }
    if (writable && ((protection & static_cast<u32>(MemoryProt::CpuWrite)) == 0 ||
                     (protection & static_cast<u32>(MemoryProt::CpuExec)) != 0)) {
        return std::nullopt;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(range_start);
    const auto end = reinterpret_cast<std::uintptr_t>(range_end);
    if (begin == 0 || end <= begin) return std::nullopt;
#ifdef LUMEN_GEN4_DIRECT_FEX
    if (executable && !SealGuestExecutableMemory(memory, begin, end - begin)) {
        return std::nullopt;
    }
#endif
    return GuestExecutionRange{begin, end - begin, executable, writable};
}

std::optional<GuestExecutionFailure> NormalizeGuestRanges(
    std::vector<GuestExecutionRange>& ranges) {
    std::ranges::sort(ranges, {}, &GuestExecutionRange::Begin);
#ifndef LUMEN_GEN4_DIRECT_FEX
    // A user-provided stack and its TLS allocation may share the same heap
    // mapping. Identical permission proofs are one range, not an overlap.
    const auto duplicates = std::ranges::unique(ranges, [](const auto& a, const auto& b) {
        return a.Begin == b.Begin && a.Size == b.Size &&
               a.Executable == b.Executable && a.Writable == b.Writable;
    });
    ranges.erase(duplicates.begin(), duplicates.end());
#endif
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        const auto& range = ranges[index];
        if (range.Begin == 0 || range.Size == 0 || range.Begin + range.Size < range.Begin) {
            LOG_ERROR(Core_Linker,
                      "Invalid FEX guest range index={} begin={:#x} size={:#x} executable={} "
                      "writable={}",
                      index, range.Begin, range.Size, range.Executable, range.Writable);
            return GuestExecutionFailure{GuestExecutionStage::Mapping, EINVAL};
        }
        if (index != 0) {
            const auto& previous = ranges[index - 1];
            if (previous.Begin + previous.Size > range.Begin) {
                LOG_ERROR(Core_Linker,
                          "FEX guest range overlap previous_index={} previous_begin={:#x} "
                          "previous_size={:#x} previous_executable={} previous_writable={} "
                          "index={} begin={:#x} size={:#x} executable={} writable={}",
                          index - 1, previous.Begin, previous.Size, previous.Executable,
                          previous.Writable, index, range.Begin, range.Size, range.Executable,
                          range.Writable);
                return GuestExecutionFailure{GuestExecutionStage::Mapping, EACCES};
            }
        }
    }
    return std::nullopt;
}

void SetGuestIntegerArguments(GuestExecutionRequest& request,
                              std::span<const u64> arguments) {
    constexpr std::array<std::size_t, 6> registers{7, 6, 2, 1, 8, 9};
    const auto count = std::min(arguments.size(), registers.size());
    for (std::size_t index = 0; index < count; ++index) {
        request.Gpr[registers[index]] = arguments[index];
    }
}

} // namespace

std::optional<GuestExecutionFailure> Linker::InitializeFexRuntime() {
    std::scoped_lock lock{m_fex_runtime_mutex};
    if (m_fex_backend != nullptr) return std::nullopt;

    auto& registry = m_hle_symbols.GetHleCallRegistry();
    if (m_fex_exit_veneer == 0) {
        const auto adapter = registry.Register(GuestCpu::MakeHleCallAdapter(ProgramExitFunc),
                                               "lumen_host.program_exit");
        if (adapter == nullptr) {
            return GuestExecutionFailure{GuestExecutionStage::Execute, EIO};
        }
        const auto veneer = m_hle_veneers->Allocate(*adapter);
        if (const auto* failure = std::get_if<GuestCpu::HleVeneerFailure>(&veneer)) {
            return GuestExecutionFailure{GuestExecutionStage::Mapping, failure->error};
        }
        m_fex_exit_veneer = std::get<u64>(veneer);
    }

#ifdef LUMEN_GEN4_DIRECT_FEX
    if (m_fex_fiber_return_veneer == 0) {
        const auto adapter = registry.Register(Libraries::Fiber::MakeFiberEntryReturnAdapter(),
                                               "lumen.fiber_entry_return");
        if (adapter == nullptr) {
            return GuestExecutionFailure{GuestExecutionStage::Execute, EIO};
        }
        const auto veneer = m_hle_veneers->Allocate(*adapter);
        if (const auto* failure = std::get_if<GuestCpu::HleVeneerFailure>(&veneer)) {
            return GuestExecutionFailure{GuestExecutionStage::Mapping, failure->error};
        }
        m_fex_fiber_return_veneer = std::get<u64>(veneer);
        Libraries::Fiber::SetFiberEntryReturnAddress(m_fex_fiber_return_veneer);
    }
#endif

    m_fex_exec_query = {memory, m_hle_veneers.get()};
    m_fex_bridge = std::make_unique<GuestCpu::HleGuestBridge>(
        registry, ValidateGuestMemory, memory, ReportGuestHleFailure, nullptr,
        QueryGuestExecutableMemory, &m_fex_exec_query);
#ifdef LUMEN_GEN4_DIRECT_FEX
    auto backend = FexGuestCpuBackend::Create(*m_fex_bridge);
#else
    auto backend = Fex::CreateGuestBackend(*m_fex_bridge);
#endif
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&backend)) {
        m_fex_bridge.reset();
        return *failure;
    }
    m_fex_backend =
        std::move(std::get<std::unique_ptr<FexGuestCpuBackend>>(backend));
    return std::nullopt;
}

#if defined(LUMEN_GEN4_DIRECT_FEX) && !defined(__APPLE__)
Linker::GuestFunctionResult Linker::RunGuestFunction(VAddr entry,
                                                      std::span<const u64> arguments,
                                                      VAddr stack_top) {
    if (const auto failure = InitializeFexRuntime()) return *failure;

    auto nested = m_fex_backend->CallGuest(entry, arguments);
    if (const auto* state = std::get_if<GuestExecutionState>(&nested)) {
        return state->Gpr[0];
    }
    const auto nestedFailure = std::get<GuestExecutionFailure>(nested);
    if (nestedFailure.Stage != GuestExecutionStage::Thread || nestedFailure.Error != ENXIO) {
        return nestedFailure;
    }

    std::optional<GuestStackMapping> temporaryStack;
    if (stack_top == 0) {
        temporaryStack.emplace(*memory, FexTemporaryStackSize, "FEX guest call stack");
        if (!temporaryStack->IsValid()) {
            return GuestExecutionFailure{GuestExecutionStage::Mapping,
                                         temporaryStack->Error() == 0 ? ENOMEM
                                                                      : temporaryStack->Error()};
        }
        stack_top = temporaryStack->Top();
    }
    if (arguments.size() > 32 || stack_top < 0x1000) {
        return GuestExecutionFailure{GuestExecutionStage::Request, E2BIG};
    }

    const auto stackArgumentBytes =
        (arguments.size() > 6 ? arguments.size() - 6 : 0) * sizeof(u64);
    if (stack_top < stackArgumentBytes + sizeof(u64)) {
        return GuestExecutionFailure{GuestExecutionStage::Request, EOVERFLOW};
    }
    const auto argumentTop = Common::AlignDown(stack_top - stackArgumentBytes, 16ULL);
    const auto rsp = argumentTop - sizeof(u64);
    const auto returnAddress = m_fex_backend->ReturnAddress();
    if (!ValidateGuestMemory(memory, rsp, sizeof(u64) + stackArgumentBytes, true) ||
        returnAddress == 0) {
        return GuestExecutionFailure{GuestExecutionStage::Mapping, EFAULT};
    }
    std::memcpy(reinterpret_cast<void*>(rsp), &returnAddress, sizeof(returnAddress));
    for (std::size_t index = 6; index < arguments.size(); ++index) {
        std::memcpy(reinterpret_cast<void*>(rsp + sizeof(u64) * (index - 5)),
                    &arguments[index], sizeof(u64));
    }

    const auto codeRange = QueryGuestMemoryRange(*memory, entry, true, false);
    const auto stackRange = QueryGuestMemoryRange(*memory, rsp, false, true);
    if (!codeRange || !stackRange) {
        return GuestExecutionFailure{GuestExecutionStage::Mapping, EFAULT};
    }

    GuestExecutionRequest request;
    request.Rip = entry;
    request.Rsp = rsp;
    request.Rflags = 1U << 1;
    request.FsBase = reinterpret_cast<std::uintptr_t>(GetTcbBase());
    SetGuestIntegerArguments(request, arguments);
    request.MappedRanges = {*codeRange, *stackRange, m_fex_backend->ReturnRange(),
                            m_fex_backend->CallbackReturnRange()};
    const auto veneerRanges = m_hle_veneers->GetExecutableRanges();
    request.MappedRanges.insert(request.MappedRanges.end(), veneerRanges.begin(),
                                veneerRanges.end());
    if (const auto failure = NormalizeGuestRanges(request.MappedRanges)) return *failure;

    const auto result = m_fex_backend->Run(request);
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&result)) return *failure;
    const auto& state = std::get<GuestExecutionState>(result);
    if (state.StopReason != GuestStopReason::Halted ||
        state.Rip < returnAddress || state.Rip >= returnAddress + 0x1000) {
        LOG_CRITICAL(Core_Linker,
                     "FEX guest function did not halt at return veneer entry={:#x} rip={:#x} "
                     "last_rip={:#x} rsp={:#x} stop={} return={:#x}",
                     entry, state.Rip, state.LastRip, state.Rsp,
                     static_cast<int>(state.StopReason), returnAddress);
        return GuestExecutionFailure{GuestExecutionStage::Execute, EPROTO};
    }
    return state.Gpr[0];
}
#else
Linker::GuestFunctionResult Linker::RunGuestFunction(VAddr entry,
                                                      std::span<const u64> arguments,
                                                      VAddr stack_top,
                                                      std::span<const std::array<u64, 2>> xmm) {
    if (arguments.size() > 32 || xmm.size() > 8)
        return GuestExecutionFailure{GuestExecutionStage::Request, E2BIG};
    if (const auto failure = InitializeFexRuntime()) return *failure;

    auto nested = m_fex_backend->CallGuest(entry, arguments, xmm);
    if (const auto* state = std::get_if<GuestExecutionState>(&nested)) {
        if (state->StopReason == GuestStopReason::ThreadExited) throw Fex::GuestThreadExit{};
        return state->Gpr[0];
    }
    const auto nestedFailure = std::get<GuestExecutionFailure>(nested);
    if (nestedFailure.Stage != GuestExecutionStage::Thread || nestedFailure.Error != ENXIO) {
        return nestedFailure;
    }

    std::optional<GuestStackMapping> temporaryStack;
    if (stack_top == 0) {
        temporaryStack.emplace(*memory, FexTemporaryStackSize, "FEX guest call stack");
        if (!temporaryStack->IsValid()) {
            return GuestExecutionFailure{GuestExecutionStage::Mapping,
                                         temporaryStack->Error() == 0 ? ENOMEM
                                                                      : temporaryStack->Error()};
        }
        stack_top = temporaryStack->Top();
    }
    if (arguments.size() > 32 || stack_top < 0x1000) {
        return GuestExecutionFailure{GuestExecutionStage::Request, E2BIG};
    }

    const auto stackArgumentBytes =
        (arguments.size() > 6 ? arguments.size() - 6 : 0) * sizeof(u64);
    if (stack_top < stackArgumentBytes + sizeof(u64)) {
        return GuestExecutionFailure{GuestExecutionStage::Request, EOVERFLOW};
    }
    const auto argumentTop = Common::AlignDown(stack_top - stackArgumentBytes, 16ULL);
    const auto rsp = argumentTop - sizeof(u64);
    const auto returnAddress = m_fex_backend->ReturnAddress();
    if (!ValidateGuestMemory(memory, rsp, sizeof(u64) + stackArgumentBytes, true) ||
        returnAddress == 0) {
        return GuestExecutionFailure{GuestExecutionStage::Mapping, EFAULT};
    }
    std::memcpy(reinterpret_cast<void*>(rsp), &returnAddress, sizeof(returnAddress));
    for (std::size_t index = 6; index < arguments.size(); ++index) {
        std::memcpy(reinterpret_cast<void*>(rsp + sizeof(u64) * (index - 5)),
                    &arguments[index], sizeof(u64));
    }

    const auto codeRange = QueryGuestMemoryRange(*memory, entry, true, false);
    const auto stackRange = QueryGuestMemoryRange(*memory, rsp, false, true);
    if (!codeRange || !stackRange) {
        return GuestExecutionFailure{GuestExecutionStage::Mapping, EFAULT};
    }

    GuestExecutionRequest request;
    request.Rip = entry;
    request.Rsp = rsp;
    request.Rflags = 1U << 1;
    request.FsBase = reinterpret_cast<std::uintptr_t>(GetTcbBase());
    SetGuestIntegerArguments(request, arguments);
    std::copy(xmm.begin(), xmm.end(), request.Xmm.begin());
    request.MappedRanges = {*codeRange, *stackRange, m_fex_backend->ReturnRange()};
#ifdef LUMEN_GEN4_DIRECT_FEX
    request.MappedRanges.push_back(m_fex_backend->CallbackReturnRange());
#endif
#ifndef LUMEN_GEN4_DIRECT_FEX
    if (request.FsBase != 0) {
        const auto tlsRange = QueryGuestMemoryRange(*memory, request.FsBase, false, true);
        if (!tlsRange) return GuestExecutionFailure{GuestExecutionStage::Mapping, EFAULT};
        request.MappedRanges.push_back(*tlsRange);
    }
#endif
    const auto veneerRanges = m_hle_veneers->GetExecutableRanges();
    request.MappedRanges.insert(request.MappedRanges.end(), veneerRanges.begin(),
                                veneerRanges.end());
    if (const auto failure = NormalizeGuestRanges(request.MappedRanges)) return *failure;

    const auto result = m_fex_backend->Run(request);
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&result)) return *failure;
    const auto& state = std::get<GuestExecutionState>(result);
    if (state.StopReason == GuestStopReason::ThreadExited) throw Fex::GuestThreadExit{};
    if (state.StopReason != GuestStopReason::Halted ||
        state.Rip < returnAddress || state.Rip >= returnAddress + 0x1000) {
        LOG_CRITICAL(Core_Linker,
                     "FEX guest function did not halt at return veneer entry={:#x} rip={:#x} "
                     "last_rip={:#x} rsp={:#x} stop={} return={:#x}",
                     entry, state.Rip, state.LastRip, state.Rsp,
                     static_cast<int>(state.StopReason), returnAddress);
        return GuestExecutionFailure{GuestExecutionStage::Execute, EPROTO};
    }
    return state.Gpr[0];
}
#endif

Linker::GuestFunctionResult Linker::RunGuestMain(EntryParams* params) {
    if (params == nullptr || params->entry_addr == 0 || params->argc < 0 || params->argc > 33) {
        return GuestExecutionFailure{GuestExecutionStage::Request, EINVAL};
    }
    if (const auto failure = InitializeFexRuntime()) return *failure;

    std::size_t stackSize = FexTemporaryStackSize;
    if (const auto* proc = GetProcParam(); proc != nullptr && proc->main_thread_stack_size != nullptr) {
        stackSize = std::clamp<std::size_t>(*proc->main_thread_stack_size, 64_KB, 64_MB);
        stackSize = Common::AlignUp(stackSize, 16_KB);
    }
    GuestStackMapping stack{*memory, stackSize, "FEX main guest stack"};
    if (!stack.IsValid()) {
        return GuestExecutionFailure{GuestExecutionStage::Mapping,
                                     stack.Error() == 0 ? ENOMEM : stack.Error()};
    }

    EntryParams guestParams = *params;
    auto cursor = stack.Top();
    for (int index = params->argc - 1; index >= 0; --index) {
        if (params->argv[index] == nullptr) {
            guestParams.argv[index] = nullptr;
            continue;
        }
        const auto length = std::strlen(params->argv[index]) + 1;
#ifdef LUMEN_GEN4_DIRECT_FEX
        if (cursor < length) {
#else
        if (length > cursor - (stack.Top() - stackSize) ||
            cursor - length < stack.Top() - stackSize + sizeof(EntryParams) + 64) {
#endif
            return GuestExecutionFailure{GuestExecutionStage::Mapping, EOVERFLOW};
        }
        cursor -= length;
        std::memcpy(reinterpret_cast<void*>(cursor), params->argv[index], length);
        guestParams.argv[index] = reinterpret_cast<const char*>(cursor);
    }
    cursor = Common::AlignDown(cursor - sizeof(EntryParams), 16ULL);
    std::memcpy(reinterpret_cast<void*>(cursor), &guestParams, sizeof(guestParams));
    const auto guestParamsAddress = cursor;
    if (cursor < 24) {
        return GuestExecutionFailure{GuestExecutionStage::Mapping, EOVERFLOW};
    }
    const auto rsp = Common::AlignDown(cursor - 24, 16ULL) + 8;
    std::memcpy(reinterpret_cast<void*>(rsp), &guestParams, 16);

    const auto codeRange = QueryGuestMemoryRange(*memory, params->entry_addr, true, false);
    const auto stackRange = QueryGuestMemoryRange(*memory, rsp, false, true);
    if (!codeRange || !stackRange) {
        return GuestExecutionFailure{GuestExecutionStage::Mapping, EFAULT};
    }

    GuestExecutionRequest request;
    request.Rip = params->entry_addr;
    request.Rsp = rsp;
    request.Rflags = 1U << 1;
    request.Gpr[7] = guestParamsAddress;
    request.Gpr[6] = m_fex_exit_veneer;
    request.FsBase = reinterpret_cast<std::uintptr_t>(GetTcbBase());
    request.MappedRanges = {*codeRange, *stackRange, m_fex_backend->ReturnRange()};
#ifdef LUMEN_GEN4_DIRECT_FEX
    request.MappedRanges.push_back(m_fex_backend->CallbackReturnRange());
#endif
#ifndef LUMEN_GEN4_DIRECT_FEX
    if (request.FsBase != 0) {
        const auto tlsRange = QueryGuestMemoryRange(*memory, request.FsBase, false, true);
        if (!tlsRange) return GuestExecutionFailure{GuestExecutionStage::Mapping, EFAULT};
        request.MappedRanges.push_back(*tlsRange);
    }
#endif
    const auto veneerRanges = m_hle_veneers->GetExecutableRanges();
    request.MappedRanges.insert(request.MappedRanges.end(), veneerRanges.begin(),
                                veneerRanges.end());
    if (const auto failure = NormalizeGuestRanges(request.MappedRanges)) return *failure;

    const auto result = m_fex_backend->Run(request);
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&result)) return *failure;
    const auto& state = std::get<GuestExecutionState>(result);
#ifndef LUMEN_GEN4_DIRECT_FEX
    if (state.StopReason == GuestStopReason::ThreadExited) throw Fex::GuestThreadExit{};
#endif
    return state.Gpr[0];
}
#endif

void Linker::Execute(const std::vector<std::string>& args) {
    if (EmulatorSettings.IsDebugDump()) {
        DebugDump();
    }

#ifdef LUMEN_GEN4_DIRECT_FEX
    // Calculate static TLS size.
    Module* module = m_modules[0].get();
    static_tls_size = module->tls.offset = module->tls.image_size;
#else
    // Freeze the TLS layout before creating any guest thread. Preloaded
    // libraries need their images even before the application registers malloc.
    Module* module = m_modules[0].get();
    std::vector<ThreadLocalImage> static_images;
    for (const auto* loaded : static_modules) static_images.push_back(loaded->tls);
    static_tls_layout = BuildStaticTlsLayout(static_images);
    for (auto* loaded : static_modules) {
        const auto image = std::ranges::find(static_tls_layout.images, loaded->tls.modid,
                                             &ThreadLocalImage::modid);
        if (image != static_tls_layout.images.end()) loaded->tls.offset = image->offset;
    }
    static_tls_finalized = true;
#endif

    // Relocate all modules
    for (const auto& m : m_modules) {
        Relocate(m.get());
    }

    // Configure the direct and flexible memory regions.
    u64 fmem_size = ORBIS_KERNEL_FLEXIBLE_MEMORY_SIZE;
    bool use_extended_mem1 = true, use_extended_mem2 = true;

    const auto* proc_param = GetProcParam();
    ASSERT(proc_param);

    Core::OrbisKernelMemParam mem_param{};
    if (proc_param->size >= offsetof(OrbisProcParam, mem_param) + sizeof(OrbisKernelMemParam*)) {
        if (proc_param->mem_param) {
            mem_param = *proc_param->mem_param;
            if (mem_param.size >=
                offsetof(OrbisKernelMemParam, flexible_memory_size) + sizeof(u64*)) {
                if (const auto* flexible_size = mem_param.flexible_memory_size) {
                    fmem_size = *flexible_size + ORBIS_KERNEL_FLEXIBLE_MEMORY_BASE;
                }
            }
        }
    }

    if (mem_param.size < offsetof(OrbisKernelMemParam, extended_memory_1) + sizeof(u64*)) {
        mem_param.extended_memory_1 = nullptr;
    }
    if (mem_param.size < offsetof(OrbisKernelMemParam, extended_memory_2) + sizeof(u64*)) {
        mem_param.extended_memory_2 = nullptr;
    }

    const u64 sdk_ver = proc_param->sdk_version;
    if (sdk_ver < Common::ElfInfo::FW_500) {
        use_extended_mem1 = mem_param.extended_memory_1 ? *mem_param.extended_memory_1 : false;
        use_extended_mem2 = mem_param.extended_memory_2 ? *mem_param.extended_memory_2 : false;
    }

    memory->SetupMemoryRegions(fmem_size, use_extended_mem1, use_extended_mem2);

    main_thread.Run([this, module, &args](std::stop_token) {
        Common::SetCurrentThreadName("Game:Main");
        std::set_terminate(Common::Log::Terminate);

#ifndef _WIN32 // Clear any existing signal mask for game threads.
        sigset_t emptyset;
        sigemptyset(&emptyset);
        pthread_sigmask(SIG_SETMASK, &emptyset, nullptr);
#endif
        if (auto& ipc = IPC::Instance()) {
            ipc.WaitForStart();
        }

        // Have libSceSysmodule preload our libraries.
        Libraries::SysModule::sceSysmodulePreloadModuleForLibkernel();

        // Simulate libSceGnmDriver initialization, which maps a chunk of direct memory.
        // Some games fail without accurately emulating this behavior.
        s64 phys_addr{};
        s32 result = Libraries::Kernel::sceKernelAllocateDirectMemory(
            0, Libraries::Kernel::sceKernelGetDirectMemorySize(), 0x10000, 0x10000, 3, &phys_addr);
        if (result == 0) {
            void* addr{reinterpret_cast<void*>(0xfe0000000)};
            result = Libraries::Kernel::sceKernelMapNamedDirectMemory(
                &addr, 0x10000, 0x13, 0, phys_addr, 0x10000, "SceGnmDriver");
        }
        ASSERT_MSG(result == 0, "Unable to emulate libSceGnmDriver initialization");

        // Add all guest arguments, we will always have the executable path in argv[0]
        EntryParams& params = Libraries::Kernel::entry_params;
        constexpr int MaxArgs = sizeof(params.argv) / sizeof(params.argv[0]);
        params.argc = std::min<int>(args.size(), MaxArgs);
        for (int i = 0; i < params.argc; i++) {
            params.argv[i] = args[i].c_str();
        }

        // Run the game's entry function
        params.entry_addr = module->GetEntryAddress();
        RunMainEntry(&params);
    });
}

s32 Linker::LoadModule(const std::filesystem::path& elf_name, bool is_dynamic) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    Fex::MemoryMutationScope publication;
    if (!publication.Valid()) return -1;
#endif
    std::scoped_lock lk{mutex};

    return LoadModuleLocked(elf_name, is_dynamic);
}

s32 Linker::LoadModuleLocked(const std::filesystem::path& elf_name, bool is_dynamic) {

#ifndef LUMEN_GEN4_DIRECT_FEX
    if (!is_dynamic && static_tls_finalized) {
        LOG_ERROR(Core_Linker, "Cannot change the static TLS layout after thread startup");
        return -1;
    }
#endif

#ifdef LUMEN_GEN4_DIRECT_FEX
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const std::string as_guest = elf_name.generic_string();
    std::unique_ptr<Core::FileSys::IFile> handle;
    if (!as_guest.empty() && as_guest.front() == '/') {
        handle = mnt->Open(as_guest, /*writable=*/false);
    }
    if (!handle) {
        if (!std::filesystem::exists(elf_name)) {
            LOG_ERROR(Core_Linker, "Provided file {} does not exist", elf_name.string());
            return -1;
        }
        auto host = std::make_unique<Core::FileSys::HostFile>(
            elf_name, Common::FS::FileAccessMode::Read, /*read_only=*/true);
        if (!host->IsOpen()) {
            LOG_ERROR(Core_Linker, "Provided file {} could not be opened", elf_name.string());
            return -1;
        }
        handle = std::move(host);
    }
    auto module = std::make_unique<Module>(memory, elf_name, std::move(handle), max_tls_index);
#else
    if (!std::filesystem::exists(elf_name)) {
        LOG_ERROR(Core_Linker, "Provided file {} does not exist", elf_name.string());
        return -1;
    }

    auto module = std::make_unique<Module>(memory, elf_name, max_tls_index);
#endif
    if (!module->IsValid()) {
        LOG_ERROR(Core_Linker, "Provided file {} is not valid ELF file", elf_name.string());
        return -1;
    }

#ifndef LUMEN_GEN4_DIRECT_FEX
    if (!is_dynamic) static_modules.push_back(module.get());
#endif
    m_modules.emplace_back(std::move(module));

    Core::Devtools::Widget::ModuleList::AddModule(elf_name.filename().string(), elf_name);

    return m_modules.size() - 1;
}

s32 Linker::LoadAndStartModule(const std::filesystem::path& path, u64 args, const void* argp,
                               int* pRes) {
    u32 handle = FindByName(path);
    if (handle != -1) {
        return handle;
    }
    handle = LoadModule(path, true);
    if (handle == -1) {
        return -1;
    }
    auto* module = GetModule(handle);
    RelocateAnyImports(module);

    // If the new module has a TLS image, trigger its load when TlsGetAddr is called.
    if (module->tls.image_size != 0) {
        AdvanceGenerationCounter();
    }

    // Retrieve and verify proc param according to libkernel.
    auto* param = module->GetProcParam<OrbisProcParam*>();
    ASSERT_MSG(!param || param->size >= 0x18, "Invalid module param size: {}", param->size);
    s32 ret = module->Start(args, argp, param);
    if (pRes) {
        *pRes = ret;
    }

    return handle;
}

Module* Linker::FindByAddress(VAddr address) {
    for (auto& module : m_modules) {
        const VAddr base = module->GetBaseAddress();
        if (address >= base && address < base + module->aligned_base_size) {
            return module.get();
        }
    }
    return nullptr;
}

void Linker::Relocate(Module* module) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    Fex::MemoryMutationScope publication;
    ASSERT_MSG(publication.Valid(), "Module relocation admission denied");
#endif
    module->ForEachRelocation([&](elf_relocation* rel, u32 i, bool is_jmp_rel) {
        const u32 num_relocs = module->dynamic_info.relocation_table_size / sizeof(elf_relocation);
        const u32 bit_idx = (is_jmp_rel ? num_relocs : 0) + i;
        if (module->TestRelaBit(bit_idx)) {
            return;
        }
        auto type = rel->GetType();
        auto symbol = rel->GetSymbol();
        auto addend = rel->rel_addend;
        auto* symbol_table = module->dynamic_info.symbol_table;
        auto* names_tlb = module->dynamic_info.str_table;

        const VAddr rel_base_virtual_addr = module->GetBaseAddress();
        const VAddr rel_virtual_addr = rel_base_virtual_addr + rel->rel_offset;
        bool rel_is_resolved = false;
        u64 rel_value = 0;
        Loader::SymbolType rel_sym_type = Loader::SymbolType::Unknown;
        std::string rel_name;

        switch (type) {
        case R_X86_64_RELATIVE:
            rel_value = rel_base_virtual_addr + addend;
            rel_is_resolved = true;
            module->SetRelaBit(bit_idx);
            break;
        case R_X86_64_DTPMOD64:
            rel_value = static_cast<u64>(module->tls.modid);
            rel_is_resolved = true;
            rel_sym_type = Loader::SymbolType::Tls;
            module->SetRelaBit(bit_idx);
            break;
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
            addend = 0;
        case R_X86_64_64: {
            auto sym = symbol_table[symbol];
            auto sym_bind = sym.GetBind();
            auto sym_type = sym.GetType();
            auto sym_visibility = sym.GetVisibility();
            u64 symbol_virtual_addr = 0;
            Loader::SymbolRecord symrec{};
            switch (sym_type) {
            case STT_FUN:
                rel_sym_type = Loader::SymbolType::Function;
                break;
            case STT_OBJECT:
                rel_sym_type = Loader::SymbolType::Object;
                break;
            case STT_NOTYPE:
                rel_sym_type = Loader::SymbolType::NoType;
                break;
            default:
                ASSERT_MSG(0, "unknown symbol type {}", sym_type);
            }

            if (sym_visibility != 0) {
                LOG_INFO(Core_Linker, "symbol visibility !=0");
            }

            switch (sym_bind) {
            case STB_LOCAL:
                symbol_virtual_addr = rel_base_virtual_addr + sym.st_value;
                module->SetRelaBit(bit_idx);
                break;
            case STB_GLOBAL:
            case STB_WEAK: {
                rel_name = names_tlb + sym.st_name;
                if (Resolve(rel_name, rel_sym_type, module, &symrec)) {
                    // Only set the rela bit if the symbol was actually resolved and not stubbed.
                    module->SetRelaBit(bit_idx);
                }
                symbol_virtual_addr = symrec.virtual_address;
                break;
            }
            default:
                UNREACHABLE_MSG("Unknown bind type {}", sym_bind);
            }
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
            if (rel_sym_type == Loader::SymbolType::Function && symrec.hle_adapter != nullptr) {
                if (m_hle_veneers == nullptr) {
                    m_hle_veneers = std::make_unique<GuestCpu::HleVeneerAllocator>();
                }
                const auto veneer = m_hle_veneers->Allocate(*symrec.hle_adapter);
                if (const auto* failure = std::get_if<GuestCpu::HleVeneerFailure>(&veneer)) {
                    LOG_ERROR(Core_Linker, "Unable to allocate FEX HLE veneer for {}: {}", symrec.name,
                              failure->error);
                } else {
                    symbol_virtual_addr = std::get<u64>(veneer);
                }
            }
#endif
            rel_is_resolved = (symbol_virtual_addr != 0);
            const u64 resolved_base = rel_is_resolved ? symbol_virtual_addr : 0;
            rel_value = resolved_base + addend;
            rel_name = symrec.name;
            break;
        }
        default:
            LOG_INFO(Core_Linker, "UNK type {:#010x} rel symbol : {:#010x}", type, symbol);
        }

        if (rel_is_resolved) {
#if defined(LUMEN_GEN4_ENABLE_FEX_GUEST_CPU) && !defined(LUMEN_GEN4_DIRECT_FEX)
            ASSERT_MSG(memory->WriteModuleRelocation(rel_virtual_addr, &rel_value, sizeof(rel_value)),
                       "Unable to publish relocation at {:#x}", rel_virtual_addr);
#else
            std::memcpy(reinterpret_cast<void*>(rel_virtual_addr), &rel_value, sizeof(rel_value));
#endif
        } else {
            LOG_INFO(Core_Linker, "Function not patched! {}", rel_name);
        }
    });
#if defined(LUMEN_GEN4_ENABLE_FEX_GUEST_CPU) && !defined(LUMEN_GEN4_DIRECT_FEX)
    for (const auto& segment : module->elf.GetProgramHeader()) {
        if ((segment.p_type != PT_LOAD && segment.p_type != PT_SCE_RELRO) ||
            !(segment.p_flags & PF_EXEC) || segment.p_memsz == 0) continue;
        void* start{};
        void* end{};
        ASSERT(memory->QueryProtection(module->GetBaseAddress() + segment.p_vaddr,
                                       &start, &end, nullptr) == ORBIS_OK);
        ASSERT_MSG(SealGuestExecutableMemory(*memory, reinterpret_cast<VAddr>(start),
                                            reinterpret_cast<VAddr>(end) - reinterpret_cast<VAddr>(start)),
                   "Unable to seal relocated module {}", module->name);
    }
#endif
}

bool Linker::Resolve(const std::string& name, Loader::SymbolType sym_type, Module* m,
                     Loader::SymbolRecord* return_info) {
    const auto ids = Common::SplitString(name, '#');
    if (ids.size() != 3) {
        return_info->virtual_address = 0;
        return_info->name = name;
        LOG_ERROR(Core_Linker, "Not Resolved {}", name);
        return false;
    }

    const LibraryInfo* library = m->FindLibrary(ids[1]);
    const ModuleInfo* module = m->FindModule(ids[2]);
    ASSERT_MSG(library && module, "Unable to find library and module");

    Loader::SymbolResolver sr{};
    sr.name = ids.at(0);
    sr.library = library->name;
    sr.library_version = library->version;
    sr.module = module->name;
    sr.type = sym_type;

    const auto* record = m_hle_symbols.FindSymbol(sr);
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    if (record != nullptr && !record->hle_fallback) {
#else
    if (record != nullptr) {
#endif
        *return_info = *record;
        Core::Devtools::Widget::ModuleList::AddModule(sr.library);
        return true;
    }

    // Check if it an exported function from one of our loaded libraries
    for (const auto& mod : m_modules) {
        if (!std::ranges::contains(mod->GetExportLibs(), *library) ||
            !std::ranges::contains(mod->GetExportModules(), *module)) {
            continue;
        }
        if (mod->export_sym.GetSize() == 0) {
            continue;
        }
        record = mod->export_sym.FindSymbol(sr);
        if (record) {
            *return_info = *record;
            return true;
        }
    }

    const auto aeronid = AeroLib::FindByNid(sr.name.c_str());
    if (sym_type == Loader::SymbolType::Object) {
        return_info->name = aeronid ? aeronid->name : "Unknown object";
        return_info->virtual_address = 0;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    } else if (sym_type == Loader::SymbolType::Function) {
        return_info->name = aeronid ? aeronid->name : "Unknown function";
        return_info->virtual_address = 0;
        if (record != nullptr && record->hle_fallback) {
            return_info->hle_adapter = record->hle_adapter;
        } else {
            return_info->hle_adapter = m_hle_symbols.AddUnsupportedFunction(sr);
        }
        LOG_WARNING(Core_Linker, "FEX: unresolved HLE {} uses temporary ENOSYS fallback",
                    return_info->name);
        return false;
#endif
    } else if (aeronid) {
        return_info->name = aeronid->name;
        return_info->virtual_address = AeroLib::GetStub(aeronid->nid);
    } else {
        return_info->virtual_address = AeroLib::GetStub(sr.name.c_str());
        return_info->name = "Unknown !!!";
    }
    if (library->name != "libc" && library->name != "libSceFios2") {
        LOG_WARNING(Core_Linker, "Linker: Stub resolved {} as {} (lib: {}, mod: {})", sr.name,
                    return_info->name, library->name, module->name);
    }
    return false;
}

#ifndef LUMEN_GEN4_DIRECT_FEX
DtvEntry* Linker::AllocateDtvTable(u32 count) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    const u64 size = Common::AlignUp((static_cast<u64>(count) + 2) * sizeof(DtvEntry), 16_KB);
    void* address{};
    const auto result = memory->MapMemory(&address, 0, size, MemoryProt::CpuReadWrite,
        MemoryMapFlags::NoCoalesce, VMAType::Flexible, "Guest TLS directory");
    ASSERT_MSG(result == ORBIS_OK, "Unable to map guest TLS directory: {}", result);
    std::memset(address, 0, size);
    return static_cast<DtvEntry*>(address);
#else
    return new DtvEntry[count + 2]{};
#endif
}

void Linker::FreeDtvTable(DtvEntry* table) {
    if (table == nullptr) return;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    const auto size = Common::AlignUp((table[1].counter + 2) * sizeof(DtvEntry), 16_KB);
    ASSERT_MSG(memory->UnmapMemory(reinterpret_cast<VAddr>(table), size) == ORBIS_OK,
               "Unable to unmap guest TLS directory");
#else
    delete[] table;
#endif
}

#endif

#ifdef LUMEN_GEN4_DIRECT_FEX
void* Linker::TlsGetAddr(u64 module_index, u64 offset) {
    std::scoped_lock lk{mutex};

    DtvEntry* dtv_table = GetTcbBase()->tcb_dtv;
    if (dtv_table[0].counter != dtv_generation_counter) {
        // Generation counter changed, a dynamic module was either loaded or unloaded.
        const u32 old_num_dtvs = dtv_table[1].counter;
        ASSERT_MSG(max_tls_index > old_num_dtvs, "Module unloading unsupported");
        // Module was loaded, increase DTV table size.
        DtvEntry* new_dtv_table = new DtvEntry[max_tls_index + 2]{};
        std::memcpy(new_dtv_table + 2, dtv_table + 2, old_num_dtvs * sizeof(DtvEntry));
        new_dtv_table[0].counter = dtv_generation_counter;
        new_dtv_table[1].counter = max_tls_index;
        delete[] dtv_table;

        // Update TCB pointer.
        GetTcbBase()->tcb_dtv = new_dtv_table;
        dtv_table = new_dtv_table;
    }

    u8* addr = dtv_table[module_index + 1].pointer;
    Module* module = m_modules[module_index - 1].get();
    if (!addr) {
        // Module was just loaded by above code. Allocate TLS block for it.
        const u32 init_image_size = module->tls.init_image_size;
        u8* dest = reinterpret_cast<u8*>(heap_api && heap_api->heap_malloc
                                               ? CallAppHeapMalloc(module->tls.image_size)
                                               : std::malloc(module->tls.image_size));
        const u8* src = reinterpret_cast<const u8*>(module->tls.image_virtual_addr);
        std::memcpy(dest, src, init_image_size);
        std::memset(dest + init_image_size, 0, module->tls.image_size - init_image_size);
        dtv_table[module_index + 1].pointer = dest;
        addr = dest;
    }
    return addr + offset;
}
#else
void* Linker::TlsGetAddr(u64 module_index, u64 offset) {
    auto* tcb = GetTcbBase();
    ASSERT(tcb != nullptr);
    // Mapping can park guest execution. Never request publication while the
    // linker lock is held: another admitted HLE thread may need that lock.
    for (;;) {
        std::unique_lock lock{mutex};
        auto* old_table = tcb->tcb_dtv;
        if (old_table[0].counter == dtv_generation_counter) break;
        const u32 count = max_tls_index;
        lock.unlock();
        auto* replacement = AllocateDtvTable(count);
        replacement[1].counter = count;
        lock.lock();
        old_table = tcb->tcb_dtv;
        if (count != max_tls_index || old_table[0].counter == dtv_generation_counter) {
            lock.unlock();
            FreeDtvTable(replacement);
            continue;
        }
        const auto old_count = old_table[1].counter;
        ASSERT_MSG(count >= old_count, "Module unloading unsupported");
        std::memcpy(replacement + 2, old_table + 2, old_count * sizeof(DtvEntry));
        replacement[0].counter = dtv_generation_counter;
        tcb->tcb_dtv = replacement;
        lock.unlock();
        FreeDtvTable(old_table);
    }

    Module* module;
    {
        std::scoped_lock lock{mutex};
        ASSERT_MSG(module_index != 0 && module_index <= max_tls_index,
                   "Invalid guest TLS module {}", module_index);
        // TLS module IDs are assigned only to PT_TLS images, while module
        // handles also include libraries with no TLS.
        const auto found = std::ranges::find_if(m_modules, [module_index](const auto& candidate) {
            return candidate->tls.modid == module_index;
        });
        ASSERT_MSG(found != m_modules.end(), "Missing TLS module {}", module_index);
        module = found->get();
        ASSERT_MSG(offset < module->tls.image_size, "Invalid guest TLS offset {:#x}", offset);
        if (auto* address = tcb->tcb_dtv[module_index + 1].pointer) return address + offset;
    }
    // The application allocator can itself enter HLE or load another module.
    // Modules are stable until process teardown; reacquire the current table
    // after the callback instead of retaining a possibly replaced directory.
    const auto size = module->tls.image_size;
    auto* destination = reinterpret_cast<u8*>(CallAppHeapMalloc(size));
    ASSERT_MSG(destination != nullptr, "Guest TLS allocation failed");
    std::memcpy(destination, reinterpret_cast<const void*>(module->tls.image_virtual_addr),
                module->tls.init_image_size);
    std::memset(destination + module->tls.init_image_size, 0, size - module->tls.init_image_size);
    tcb->tcb_dtv[module_index + 1].pointer = destination;
    return destination + offset;
}
#endif

void* Linker::CallAppHeapMalloc(u64 size) {
    ASSERT_MSG(heap_api != nullptr && heap_api->heap_malloc != nullptr,
               "Guest heap malloc callback is unavailable");
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    const std::array<u64, 1> arguments{size};
    const auto result =
        RunGuestFunction(reinterpret_cast<VAddr>(heap_api->heap_malloc), arguments);
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&result)) {
#ifdef LUMEN_GEN4_DIRECT_FEX
        LOG_CRITICAL(Core_Linker, "FEX guest heap malloc failed at stage {}: {}",
                     static_cast<int>(failure->Stage), failure->Error);
#else
        LOG_CRITICAL(Core_Linker, "FEX guest heap malloc failed at stage {}: {} code={} rip={:#x}",
                     static_cast<int>(failure->Stage), failure->Error,
                     failure->RuntimeCode, failure->Rip);
#endif
        Common::Log::Flush();
        std::abort();
    }
    return reinterpret_cast<void*>(std::get<u64>(result));
#else
    return heap_api->heap_malloc(size);
#endif
}

#ifdef LUMEN_GEN4_DIRECT_FEX
void Linker::CallAppHeapFree(void* pointer) {
    ASSERT_MSG(heap_api != nullptr && heap_api->heap_free != nullptr,
               "Guest heap free callback is unavailable");
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    const std::array<u64, 1> arguments{reinterpret_cast<u64>(pointer)};
    const auto result = RunGuestFunction(reinterpret_cast<VAddr>(heap_api->heap_free), arguments);
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&result)) {
        LOG_CRITICAL(Core_Linker, "FEX guest heap free failed at stage {}: {}",
                     static_cast<int>(failure->Stage), failure->Error);
        std::abort();
    }
#else
    heap_api->heap_free(pointer);
#endif
}
#else
void Linker::CallAppHeapFree(void* pointer, decltype(HeapAPI::heap_free) callback) {
    if (callback == nullptr && heap_api != nullptr) callback = heap_api->heap_free;
    ASSERT_MSG(callback != nullptr,
               "Guest heap free callback is unavailable");
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    const std::array<u64, 1> arguments{reinterpret_cast<u64>(pointer)};
    const auto result = RunGuestFunction(reinterpret_cast<VAddr>(callback), arguments);
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&result)) {
        LOG_CRITICAL(Core_Linker, "FEX guest heap free failed at stage {}: {} code={} rip={:#x}",
                     static_cast<int>(failure->Stage), failure->Error,
                     failure->RuntimeCode, failure->Rip);
        Common::Log::Flush();
        std::abort();
    }
#else
    callback(pointer);
#endif
}
#endif

#ifdef LUMEN_GEN4_DIRECT_FEX
void* Linker::AllocateTlsForThread(bool is_primary) {
    static constexpr size_t TcbSize = 0x40;
    static constexpr size_t TlsAllocAlign = 0x20;
    const size_t total_tls_size = Common::AlignUp(static_tls_size, TlsAllocAlign) + TcbSize;

    // If sceKernelMapNamedFlexibleMemory is being called from libkernel and addr = 0
    // it automatically places mappings in system reserved area instead of managed.
    // Since the system reserved area already has a mapping in it, this address is slightly higher.
    static constexpr VAddr KernelAllocBase = 0x881000000ULL;

    // The kernel module has a few different paths for TLS allocation.
    // For SDK < 1.7 it allocates both main and secondary thread blocks using libc mspace/malloc.
    // In games compiled with newer SDK, the main thread gets mapped from flexible memory,
    // with addr = 0, so system managed area. Here we will only implement the latter.
    void* addr_out{reinterpret_cast<void*>(KernelAllocBase)};
    if (is_primary) {
        const size_t tls_aligned = Common::AlignUp(total_tls_size, 16_KB);
        const int ret = Libraries::Kernel::sceKernelMapNamedFlexibleMemory(
            &addr_out, tls_aligned, 3, 0, "SceKernelPrimaryTcbTls");
        ASSERT_MSG(ret == 0, "Unable to allocate TLS+TCB for the primary thread");
    } else {
        if (heap_api && heap_api->heap_malloc) {
            addr_out = CallAppHeapMalloc(total_tls_size);
        } else {
            addr_out = std::malloc(total_tls_size);
        }
    }
    return addr_out;
}
#else
void* Linker::AllocateTlsForThread(bool is_primary) {
    ASSERT_MSG(static_tls_finalized, "Guest thread created before static TLS layout was finalized");
    const u64 total_tls_size = StaticTlsAllocationSize(static_tls_layout);
    TlsAllocation allocation{};

    // If sceKernelMapNamedFlexibleMemory is being called from libkernel and addr = 0
    // it automatically places mappings in system reserved area instead of managed.
    // Since the system reserved area already has a mapping in it, this address is slightly higher.
    static constexpr VAddr KernelAllocBase = 0x881000000ULL;

    // The kernel module has a few different paths for TLS allocation.
    // For SDK < 1.7 it allocates both main and secondary thread blocks using libc mspace/malloc.
    // In games compiled with newer SDK, the main thread gets mapped from flexible memory,
    // with addr = 0, so system managed area. Here we will only implement the latter.
    void* addr_out{reinterpret_cast<void*>(KernelAllocBase)};
    if (is_primary) {
        const size_t tls_aligned = Common::AlignUp(total_tls_size, 16_KB);
        const int ret = Libraries::Kernel::sceKernelMapNamedFlexibleMemory(
            &addr_out, tls_aligned, 3, 0, "SceKernelPrimaryTcbTls");
        ASSERT_MSG(ret == 0, "Unable to allocate TLS+TCB for the primary thread");
        allocation.mapping_size = tls_aligned;
    } else {
        if (heap_api) {
            ASSERT_MSG(heap_api->heap_free != nullptr, "Guest heap free callback is unavailable");
            allocation.heap_free = heap_api->heap_free;
            addr_out = CallAppHeapMalloc(total_tls_size);
        } else {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
            const auto size = Common::AlignUp(total_tls_size, 16_KB);
            const auto result = memory->MapMemory(&addr_out, 0, size, MemoryProt::CpuReadWrite,
                MemoryMapFlags::NoCoalesce, VMAType::Flexible, "Guest secondary TLS");
            ASSERT_MSG(result == ORBIS_OK, "Unable to map secondary TLS: {}", result);
            allocation.mapping_size = size;
#else
            addr_out = std::malloc(total_tls_size);
#endif
        }
    }
    if (addr_out == nullptr) return nullptr;
    allocation.original = addr_out;
    auto* aligned = reinterpret_cast<void*>(AlignStaticTlsAllocation(
        static_tls_layout, reinterpret_cast<uintptr_t>(addr_out)));
    {
        std::scoped_lock lock{mutex};
        ASSERT_MSG(tls_allocations.emplace(aligned, allocation).second,
                   "Duplicate guest TLS allocation");
    }
    return aligned;
}
#endif

#ifndef LUMEN_GEN4_DIRECT_FEX
void Linker::FreeTlsForThread(void* base) {
    TlsAllocation allocation{};
    {
        std::scoped_lock lock{mutex};
        const auto found = tls_allocations.find(base);
        ASSERT_MSG(found != tls_allocations.end(), "Unknown guest TLS allocation");
        allocation = found->second;
        tls_allocations.erase(found);
    }
    // Preserve allocation ownership across late registration of the app heap.
    // No native linker mutex may span a VM publication or guest free callback.
    if (allocation.mapping_size != 0) {
        ASSERT_MSG(memory->UnmapMemory(reinterpret_cast<VAddr>(allocation.original),
                                       allocation.mapping_size) == ORBIS_OK,
                   "Unable to unmap thread TLS allocation");
    } else if (allocation.heap_free != nullptr) {
        CallAppHeapFree(allocation.original, allocation.heap_free);
    } else {
        std::free(allocation.original);
    }
}
#endif

#ifdef LUMEN_GEN4_DIRECT_FEX
void Linker::FreeTlsForNonPrimaryThread(void* pointer) {
    if (heap_api && heap_api->heap_free) {
        CallAppHeapFree(pointer);
    } else {
        std::free(pointer);
    }
}
#else
void Linker::FreeTlsForNonPrimaryThread(void* pointer) {
    if (heap_api) {
        CallAppHeapFree(pointer);
    } else {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
        void* begin{};
        void* end{};
        ASSERT(memory->QueryProtection(reinterpret_cast<VAddr>(pointer), &begin, &end, nullptr) == ORBIS_OK);
        ASSERT(begin == pointer);
        ASSERT(memory->UnmapMemory(reinterpret_cast<VAddr>(begin),
            reinterpret_cast<VAddr>(end) - reinterpret_cast<VAddr>(begin)) == ORBIS_OK);
#else
        std::free(pointer);
#endif
    }
}
#endif

void Linker::DebugDump() {
    const auto& log_dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
    const std::filesystem::path debug(log_dir / "debugdump");
    std::filesystem::create_directory(debug);
    for (const auto& m : m_modules) {
        Module* module = m.get();
        auto& elf = module->elf;
        const std::filesystem::path filepath(debug / module->file.stem());
        std::filesystem::create_directory(filepath);
        module->import_sym.DebugDump(filepath / "imports.txt");
        module->export_sym.DebugDump(filepath / "exports.txt");
        if (elf.IsSelfFile()) {
            elf.SelfHeaderDebugDump(filepath / "selfHeader.txt");
            elf.SelfSegHeaderDebugDump(filepath / "selfSegHeaders.txt");
        }
        elf.ElfHeaderDebugDump(filepath / "elfHeader.txt");
        elf.PHeaderDebugDump(filepath / "elfPHeaders.txt");
    }
}

} // namespace Core
