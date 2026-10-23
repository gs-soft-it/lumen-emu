// SPDX-License-Identifier: GPL-2.0-only
#include "DirectFexRuntime.h"
#include "DirectFexSelection.h"
#include "DirectFexVeneerCache.h"
#include "DirectFexHostCallTable.h"
#include "DirectPerformanceOptions.h"

// The Gen4 implementation remains unchanged. Its public Thread definition and
// current-execution scope are needed for the PS5 full-register callback ABI
// and dynamic module invalidation, which its narrow harness API does not expose.
#include "../../src/core/fex/fex_guest_engine_direct.cpp"
#include <Interface/Context/Context.h>

namespace Lumen::Ps5::DirectFex {
bool HandleHostSignal(int signal, siginfo_t* info, void* context) noexcept {
    return Core::Fex::HandleGuestSignal(signal, info, context);
}
namespace {
using Engine = Core::Fex::GuestEngine;
using NativeThread = FEXCore::Core::InternalThreadState;
constexpr std::uint64_t ProbeOperation = 0xfffffffffffffff0ULL;
constexpr std::size_t PageSize = 4096;

struct Root {
    LumenAndroidFexCoreGuestStateV1* state {};
    LumenAndroidFexCoreHostCallV1 host1 {};
    LumenAndroidFexCoreHostCallV2 host2 {};
    LumenAndroidFexCoreExecutableQueryV1 query {};
    void* opaque {};
    Root* parent {};
    std::uint32_t failure {};
};
thread_local Root* activeRoot {};
thread_local bool probing {};
thread_local unsigned mutationDepth {};
thread_local Core::GuestCpu::HleCallFrame* activeHleFrame {};
thread_local std::optional<HleContext> pendingHleContext;
thread_local const HostTargetRecord* activeHostTarget {};

class Bridge final : public Core::Fex::GuestBridge {
public:
    Core::Fex::EngineResult<bool> Invoke(Core::GuestCpu::HleCallFrame& frame) override;
    std::optional<Core::GuestExecutionRange> QueryExecutableRange(std::uintptr_t address) override;
};

struct Runtime {
    std::once_flag once;
    Bridge bridge;
    std::unique_ptr<Engine> engine;
    FEXCore::Context::Context* context {};
    LumenAndroidFexCoreRuntimeStatusV1 status {
        sizeof(LumenAndroidFexCoreRuntimeStatusV1), 1,
        LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1};
    std::mutex veneersMutex, threadsMutex;
    std::unordered_map<std::uint64_t, HostTargetRecord> veneers;
    std::unique_ptr<HostCallTable> indexedHostCalls;
    std::uint64_t nextOperation {1};
    std::unordered_set<Engine::Thread*> threads;
    std::array<struct sigaction, 2> previousSignals {};
    std::uint64_t frequency {};
    std::uint32_t scale {};
    std::uint32_t maxInstOptIn {};
    bool veneerCacheEnabled {};
    std::array<char, 768> receipt {};
};

Runtime& Data() {
    // Gen4's context and allocator are process-owned. PS5 teardown terminates
    // its isolated runtime process, so static destruction must not race workers.
    static Runtime* runtime = new Runtime;
    return *runtime;
}

std::uint32_t FailureCode(const Core::Fex::EngineFailure& failure) {
    switch (failure.Stage) {
    case Core::Fex::EngineStage::Request: return LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1;
    case Core::Fex::EngineStage::Mapping: return LUMEN_ANDROID_FEXCORE_RUNTIME_EXECUTABLE_RANGE_DENIED_V1;
    case Core::Fex::EngineStage::Thread: return LUMEN_ANDROID_FEXCORE_RUNTIME_THREAD_SETUP_FAILED_V1;
    case Core::Fex::EngineStage::Bridge: return LUMEN_ANDROID_FEXCORE_RUNTIME_HOST_CALL_FAILED_V1;
    default: return LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1;
    }
}

void Signals(int signal, siginfo_t* info, void* raw) {
    if (Core::Fex::HandleGuestSignal(signal, info, raw)) return;
    const auto& previous = Data().previousSignals[signal == SIGBUS ? 1 : 0];
    if (previous.sa_handler == SIG_IGN) return;
    if (previous.sa_handler != SIG_DFL && previous.sa_handler != nullptr) {
        if (previous.sa_flags & SA_SIGINFO) previous.sa_sigaction(signal, info, raw);
        else previous.sa_handler(signal);
        return;
    }
    // Preserve the native failure when direct Gen4 does not handle the fault.
    sigaction(signal, &previous, nullptr);
    raise(signal);
}

bool InstallSignals() {
    struct sigaction action {};
    action.sa_sigaction = Signals;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
#if defined(__linux__)
    sigaddset(&action.sa_mask, SIGRTMAX); // Finish GPU/translator fault repair before cancellation.
#endif
    if (sigaction(SIGSEGV, &action, &Data().previousSignals[0]) != 0) return false;
    if (sigaction(SIGBUS, &action, &Data().previousSignals[1]) != 0) {
        sigaction(SIGSEGV, &Data().previousSignals[0], nullptr);
        return false;
    }
    return true;
}

bool Contains(std::uint64_t base, std::uint64_t size, std::uint64_t address) {
    return size != 0 && address >= base && address - base < size;
}

bool ConfigureOptIns(Runtime& runtime) {
    const auto& performance = DirectPerformance::Get();
    if (!performance.valid) return false;
    if (performance.hle_indexed_dispatch)
        runtime.indexedHostCalls = std::make_unique<HostCallTable>();
    if (const auto* value = std::getenv("LUMEN_PS5_FEX_VENEER_CACHE")) {
        if (std::strcmp(value, "1") == 0) runtime.veneerCacheEnabled = true;
        else if (std::strcmp(value, "0") != 0) return false;
    }
    if (const auto* value = std::getenv("LUMEN_FEX_MAX_INST")) {
        if (std::strcmp(value, "128") == 0) runtime.maxInstOptIn = 128;
        else if (std::strcmp(value, "5000") == 0) runtime.maxInstOptIn = 5000;
        else return false;
        // Gen4 loads its environment layer during Engine::Create. An absent
        // Lumen selector leaves its existing environment and defaults intact.
        if (setenv("FEX_MAXINST", value, 1) != 0) return false;
    }
    // Dense IDs already carry the return address; the optional old lookup
    // cache is unused in this mode. Report the effective path in the receipt.
    if (runtime.indexedHostCalls) runtime.veneerCacheEnabled = false;
    return true;
}

std::uint64_t FindReturnStub(Runtime& runtime, std::uint64_t target) {
    // Veneers and their executable mappings are process-owned and append-only.
    // A positive lookup remains valid across later map rehashes/registrations.
    thread_local VeneerReturnCache cache;
    if (runtime.veneerCacheEnabled) {
        if (const auto stub = cache.Find(target)) return stub;
    }
    std::lock_guard lock(runtime.veneersMutex);
    const auto found = runtime.veneers.find(target);
    if (found == runtime.veneers.end()) return 0;
    const auto stub = found->second.address + 19;
    if (runtime.veneerCacheEnabled) cache.Remember(target, stub);
    return stub;
}

std::optional<Core::GuestExecutionRange> Bridge::QueryExecutableRange(std::uintptr_t address) {
    auto& runtime = Data();
    if (runtime.engine) {
        for (const auto range : {runtime.engine->ReturnRange(), runtime.engine->CallbackReturnRange()})
            if (Contains(range.Begin, range.Size, address)) return range;
    }
    {
        std::lock_guard lock(runtime.veneersMutex);
        for (const auto& [target, veneer] : runtime.veneers) {
            (void)target;
            if (Contains(veneer.address, PageSize, address))
                return Core::GuestExecutionRange {veneer.address, PageSize, true, false};
        }
    }
    for (auto* root = activeRoot; root != nullptr; root = root->parent) {
        std::uint64_t base {}, size {};
        std::uint32_t writable {};
        if (root->query(root->opaque, address, &base, &size, &writable) == 1 &&
            Contains(base, size, address) && writable <= 1)
            return Core::GuestExecutionRange {base, size, true, writable != 0};
    }
    return std::nullopt;
}

Core::Fex::EngineResult<bool> Bridge::Invoke(Core::GuestCpu::HleCallFrame& frame) {
    if (probing && frame.operation == ProbeOperation) {
        frame.gpr[0] = 42;
        Data().status.hostBridgeExecuted = 1;
        return true;
    }
    auto* root = activeRoot;
    if (root == nullptr) return Core::Fex::EngineFailure {Core::Fex::EngineStage::Bridge, EPERM};
    auto& runtime = Data();
    const auto* indexedTarget = runtime.indexedHostCalls
        ? runtime.indexedHostCalls->Find(frame.operation) : nullptr;
    const auto target = indexedTarget ? indexedTarget->target : frame.operation;
    const auto returnStub = runtime.indexedHostCalls
        ? (indexedTarget ? indexedTarget->address + 19 : 0)
        : FindReturnStub(runtime, frame.operation);
    const auto stop = [&] {
        frame.gpr[11] = root->parent != nullptr
            ? runtime.engine->CallbackReturnRange().Begin : runtime.engine->ReturnAddress();
    };
    if (returnStub == 0 || frame.rsp == 0 || frame.rsp > UINT64_MAX - 8) {
        root->failure = LUMEN_ANDROID_FEXCORE_RUNTIME_HOST_CALL_FAILED_V1;
        stop();
        return true;
    }
    // The PS5 veneer saves incoming RAX/AL before loading the syscall operation.
    // HLE must see the original CALL stack and original variadic SSE count.
    std::memcpy(&frame.gpr[0], reinterpret_cast<const void*>(frame.rsp), 8);
    frame.rsp += 8;
    frame.gpr[4] = frame.rsp;
    std::uint64_t next {};
    LumenAndroidFexCoreSimdViewV2 simd {
        sizeof(simd), 16, 16, 16, reinterpret_cast<std::uint8_t*>(frame.xmm.data())};
    struct FrameScope {
        Core::GuestCpu::HleCallFrame* previous;
        std::optional<HleContext> previousPending;
        const HostTargetRecord* previousTarget;
        FrameScope(Core::GuestCpu::HleCallFrame& current, const HostTargetRecord* target)
            : previous(activeHleFrame), previousPending(std::move(pendingHleContext)),
              previousTarget(activeHostTarget) {
            activeHleFrame = &current;
            pendingHleContext.reset();
            activeHostTarget = target;
        }
        ~FrameScope() {
            activeHleFrame = previous;
            pendingHleContext = std::move(previousPending);
            activeHostTarget = previousTarget;
        }
    } frameScope(frame, indexedTarget);
    const auto disposition = root->host2 != nullptr
        ? root->host2(root->opaque, target, frame.gpr.data(), &simd, &next)
        : root->host1(root->opaque, target, frame.gpr.data(), &next);
    if (pendingHleContext && (disposition == LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1 ||
                              disposition == LUMEN_ANDROID_FEXCORE_HOST_CALL_RETURN_GUEST_V1)) {
        const auto& saved = *pendingHleContext;
        if (saved.reset_simd && frame.reset_simd) frame.reset_simd(frame.reset_simd_context);
        std::copy(std::begin(saved.gprs), std::end(saved.gprs), frame.gpr.begin());
        std::memcpy(frame.xmm.data(), saved.xmm, sizeof(saved.xmm));
        frame.rsp = frame.gpr[4];
        frame.gpr[11] = saved.rip;
        const auto end = frame.callret_stack_base + frame.callret_stack_size;
        frame.callret_sp = saved.callret_sp >= frame.callret_stack_base && saved.callret_sp <= end
            ? saved.callret_sp : frame.callret_stack_base + frame.callret_stack_size / 4;
        frame.fcw = saved.fcw;
        frame.mxcsr = (saved.mxcsr & ~0x3fu) | (frame.mxcsr & 0x3fu);
        return true;
    }
    frame.rsp = frame.gpr[4];
    switch (disposition) {
    case LUMEN_ANDROID_FEXCORE_HOST_CALL_RETURN_GUEST_V1:
        frame.gpr[11] = returnStub;
        return true;
    case LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1:
        if (next != 0) {
            // PS5 HLE already consumed the guest return slot. Balance the
            // same FEX shadow-stack entry that an ordinary guest RET consumes.
            if (frame.callret_sp < frame.callret_stack_base ||
                frame.callret_sp > frame.callret_stack_base + frame.callret_stack_size - 16)
                break;
            frame.callret_sp += 16;
            frame.gpr[11] = next;
            return true;
        }
        break;
    case LUMEN_ANDROID_FEXCORE_HOST_CALL_STOP_V1:
        stop();
        return true;
    default: break;
    }
    root->failure = LUMEN_ANDROID_FEXCORE_RUNTIME_HOST_CALL_FAILED_V1;
    stop();
    return true;
}

void Initialize() {
    auto& runtime = Data();
    if (!LockSelection()) {
        runtime.status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
        return;
    }
    runtime.status.hostPageSize = static_cast<std::uint32_t>(sysconf(_SC_PAGESIZE));
    if (runtime.status.hostPageSize != PageSize) {
        runtime.status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_WRONG_PAGE_SIZE_V1;
        return;
    }
    if (!ConfigureOptIns(runtime)) {
        runtime.status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
        return;
    }
    if (!InstallSignals()) {
        runtime.status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_SIGNAL_SETUP_FAILED_V1;
        return;
    }
    auto created = Engine::Create(runtime.bridge);
    if (const auto* failure = std::get_if<Core::Fex::EngineFailure>(&created)) {
        runtime.status.code = FailureCode(*failure);
        return;
    }
    runtime.engine = std::move(std::get<std::unique_ptr<Engine>>(created));
    runtime.status.contextInitialized = 1;
    runtime.status.hostBridgeReady = 1;
    // Same direct syscall boundary as Gen4; this original fixture proves it
    // without publishing capabilities for facade safepoints or PS5 exceptions.
    Core::Fex::Mapping code(PageSize, PROT_READ | PROT_WRITE);
    Core::Fex::Mapping stack(PageSize, PROT_READ | PROT_WRITE);
    if (!code.IsValid() || !stack.IsValid()) return;
    std::array<std::uint8_t, 13> bytes {0x48, 0xb8};
    std::memcpy(bytes.data() + 2, &ProbeOperation, 8);
    bytes[10] = 0x0f; bytes[11] = 0x05; bytes[12] = 0xf4;
    std::memcpy(code.Get(), bytes.data(), bytes.size());
    if (std::holds_alternative<Core::Fex::EngineFailure>(code.Protect(PROT_READ | PROT_EXEC))) return;
    Core::GuestExecutionRequest request;
    request.Rip = reinterpret_cast<std::uintptr_t>(code.Get());
    request.Rsp = reinterpret_cast<std::uintptr_t>(stack.Get()) + PageSize - 8;
    request.MappedRanges = {{request.Rip, PageSize, true, false},
        {reinterpret_cast<std::uintptr_t>(stack.Get()), PageSize, false, true}};
    auto made = runtime.engine->CreateThread(request);
    if (const auto* failure = std::get_if<Core::Fex::EngineFailure>(&made)) {
        runtime.status.code = FailureCode(*failure);
        return;
    }
    auto* thread = std::get<Engine::Thread*>(made);
    runtime.context = static_cast<FEXCore::Context::Context*>(thread->Native->CTX);
    probing = true;
    auto result = runtime.engine->Run(*thread);
    probing = false;
    const auto* executed = std::get_if<Core::GuestExecutionState>(&result);
    const bool passed = executed != nullptr && executed->Gpr[0] == 42 && runtime.status.hostBridgeExecuted;
    {
        // The bootstrap mapping is about to retire. The direct engine shares
        // translated blocks between threads, so a subsequent mmap at this
        // address must not execute the probe's cached syscall.
        std::scoped_lock lock(runtime.context->GetCodeInvalidationMutex());
        runtime.context->InvalidateCodeBuffersCodeRange(request.Rip, PageSize);
        runtime.context->InvalidateThreadCachedCodeRange(thread->Native, request.Rip, PageSize);
    }
    runtime.engine->DestroyThread(thread);
    if (!passed) {
        runtime.status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_SELF_TEST_FAILED_V1;
        return;
    }
    const auto leaf = runtime.context->RunCPUIDFunction(0x15, 0);
    std::uint64_t nativeFrequency {};
    asm volatile("mrs %0, cntfrq_el0" : "=r"(nativeFrequency));
    runtime.scale = static_cast<FEXCore::Context::ContextImpl*>(runtime.context)->Config.TSCScale;
    if (runtime.scale > 31 || nativeFrequency == 0 || nativeFrequency > (UINT64_MAX >> runtime.scale) ||
        leaf.eax == 0 || static_cast<std::uint64_t>(leaf.ecx) * leaf.ebx / leaf.eax != (nativeFrequency << runtime.scale)) {
        runtime.status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
        return;
    }
    runtime.frequency = nativeFrequency << runtime.scale;
    runtime.status.guestExecuted = runtime.status.backendRouteReady = 1;
    runtime.status.guestResult = 42;
    runtime.status.logicalCores = static_cast<std::uint32_t>(std::max(1L, sysconf(_SC_NPROCESSORS_ONLN)));
    runtime.status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1;
}

void CopyResult(LumenAndroidFexCoreGuestStateV1& output, const FEXCore::Core::CPUState& native) {
    output.rip = native.rip;
    std::copy(std::begin(native.gregs), std::end(native.gregs), output.gprs);
    output.fsBase = native.fs_cached;
    output.gsBase = native.gs_cached;
    output.faultAddress = output.faultSignal = 0;
}

void CopyArchitecturalState(FEXCore::Core::CPUState& to, const FEXCore::Core::CPUState& from) {
    // CPUState also contains live cache pointers and a signal refcounter; those
    // must not be rewound when a nested callback compiles or invalidates code.
    to.pf_raw = from.pf_raw; to.af_raw = from.af_raw; to.rip = from.rip;
    std::copy(std::begin(from.gregs), std::end(from.gregs), std::begin(to.gregs));
    to.callret_sp = from.callret_sp;
    std::memcpy(to.avx_high, from.avx_high, sizeof(to.avx_high));
    to.xmm = from.xmm;
    to.es_idx = from.es_idx; to.cs_idx = from.cs_idx; to.ss_idx = from.ss_idx;
    to.ds_idx = from.ds_idx; to.gs_idx = from.gs_idx; to.fs_idx = from.fs_idx;
    to.mxcsr = from.mxcsr;
    to.es_cached = from.es_cached; to.cs_cached = from.cs_cached;
    to.ss_cached = from.ss_cached; to.ds_cached = from.ds_cached;
    to.gs_cached = from.gs_cached; to.fs_cached = from.fs_cached;
    std::memcpy(to.flags, from.flags, sizeof(to.flags));
    std::memcpy(to.mm, from.mm, sizeof(to.mm));
    to.FCW = from.FCW; to.AbridgedFTW = from.AbridgedFTW;
}

std::uint32_t Run(LumenAndroidFexCoreGuestStateV1* state,
    LumenAndroidFexCoreHostCallV1 host1, LumenAndroidFexCoreHostCallV2 host2,
    LumenAndroidFexCoreExecutableQueryV1 query, void* opaque,
    const Core::GuestExecutionRange* ownedStack = nullptr) {
    if (state == nullptr || state->structSize != sizeof(*state) || state->abiVersion != 1 ||
        state->rip == 0 || query == nullptr || (host1 == nullptr && host2 == nullptr) || mutationDepth != 0)
        return LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1;
    if (const auto code = Probe(); code != 0) return code;
    auto& runtime = Data();
    Root root {state, host1, host2, query, opaque, activeRoot};
    struct RestoreRoot { Root* previous; ~RestoreRoot() { activeRoot = previous; } } restore {activeRoot};
    activeRoot = &root;
    const auto executable = runtime.bridge.QueryExecutableRange(state->rip);
    if (!executable || executable->Writable) return LUMEN_ANDROID_FEXCORE_RUNTIME_EXECUTABLE_RANGE_DENIED_V1;
    if (root.parent != nullptr) {
        auto* native = Core::Fex::ActiveFexExecution.Thread;
        auto* context = Core::Fex::ActiveFexExecution.Context;
        if (native == nullptr || context != runtime.context || state->gprs[4] > UINT64_MAX - 16)
            return LUMEN_ANDROID_FEXCORE_RUNTIME_THREAD_SETUP_FAILED_V1;
        auto& frame = native->CurrentFrame->State;
        FEXCore::Core::CPUState saved;
        CopyArchitecturalState(saved, frame);
        std::copy(std::begin(state->gprs), std::end(state->gprs), std::begin(frame.gregs));
        frame.fs_cached = state->fsBase;
        frame.gs_cached = state->gsBase;
        // ExecuteJITCallback pushes its return trampoline at RSP-16. PS5 has
        // already prepared the complete callback frame at the requested RSP.
        frame.gregs[4] = state->gprs[4] + 16;
        Core::Fex::BridgeSyscallHandler::InvocationState invocation;
        Core::Fex::BridgeSyscallHandler::InvocationScope scope {
            *Core::Fex::ActiveFexExecution.Syscalls, invocation, native};
        struct NestedGuestScope {
            Core::GuestCpu::HleCallFrame* parent = activeHleFrame;
            const HostTargetRecord* parentTarget = activeHostTarget;
            NestedGuestScope() { activeHleFrame = nullptr; activeHostTarget = nullptr; }
            ~NestedGuestScope() { activeHleFrame = parent; activeHostTarget = parentTarget; }
        } nested_guest;
        context->HandleCallback(native, state->rip);
        CopyResult(*state, frame);
        CopyArchitecturalState(frame, saved);
        if (invocation.Failure) return FailureCode(*invocation.Failure);
        if (root.failure != 0) return root.failure;
        state->rip = runtime.engine->ReturnAddress();
        return LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1;
    }
    Core::GuestExecutionRequest request;
    request.Rip = state->rip; request.Rsp = state->gprs[4];
    request.FsBase = state->fsBase; request.GsBase = state->gsBase;
    std::copy(std::begin(state->gprs), std::end(state->gprs), request.Gpr.begin());
    request.MappedRanges.push_back(*executable);
    Core::GuestExecutionRange stack {};
    if (ownedStack != nullptr) {
        stack = *ownedStack;
        if (stack.Begin == 0 || stack.Size == 0 || stack.Size > UINT64_MAX - stack.Begin ||
            !Contains(stack.Begin, stack.Size, request.Rsp))
            return LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1;
        const auto end = stack.Begin + stack.Size;
        if (end > UINT64_MAX - (PageSize - 1))
            return LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1;
        const auto pageBegin = stack.Begin & ~std::uint64_t {PageSize - 1};
        const auto pageEnd = (end + PageSize - 1) & ~std::uint64_t {PageSize - 1};
        // Validate only pages belonging to the actual guest stack. Capturing a
        // coalesced host VMA here also includes unrelated stacks whose guards
        // another guest thread can protect before CreateThread revalidates it.
        request.MappedRanges.push_back({pageBegin, pageEnd - pageBegin, false, true});
    } else {
        // Compatibility entry for stand-alone CPU fixtures without a VM owner.
        const auto mappings = Core::Fex::CaptureHostMappings();
        const auto* snapshot = std::get_if<std::vector<Core::Fex::detail::HostMapping>>(&mappings);
        if (snapshot == nullptr) return LUMEN_ANDROID_FEXCORE_RUNTIME_EXECUTABLE_RANGE_DENIED_V1;
        const auto hostStack = std::find_if(snapshot->begin(), snapshot->end(), [&](const auto& mapping) {
            return mapping.Writable && !mapping.Executable && request.Rsp >= mapping.Begin && request.Rsp < mapping.End;
        });
        if (hostStack == snapshot->end()) return LUMEN_ANDROID_FEXCORE_RUNTIME_EXECUTABLE_RANGE_DENIED_V1;
        stack = {hostStack->Begin, hostStack->End - hostStack->Begin, false, true};
        request.MappedRanges.push_back(stack);
    }
    Engine::Thread* thread {};
    {
        std::lock_guard lock(runtime.threadsMutex);
        auto made = runtime.engine->CreateThread(request);
        if (const auto* failure = std::get_if<Core::Fex::EngineFailure>(&made)) return FailureCode(*failure);
        thread = std::get<Engine::Thread*>(made);
        // Gen4 validates the initial request against host mappings, then keeps
        // its ranges as an executable lookup shortcut. PS5 modules can later
        // change permissions or retire mappings: only PS5's live query may
        // authorize their code after creation. Keep the stack for the direct
        // callback ABI, and remove that immutable executable shortcut.
        auto* syscalls = static_cast<Core::Fex::BridgeSyscallHandler*>(
            static_cast<FEXCore::Context::ContextImpl*>(runtime.context)->SyscallHandler);
        syscalls->UnregisterThread(thread->Native);
        const auto registered = syscalls->RegisterThread(thread->Native, {stack});
        if (const auto* failure = std::get_if<Core::Fex::EngineFailure>(&registered)) {
            runtime.engine->DestroyThread(thread);
            return FailureCode(*failure);
        }
        runtime.threads.insert(thread);
    }
    struct NativeExitCleanup { Runtime* runtime; Engine::Thread* thread; } cleanup {&runtime, thread};
    Core::Fex::EngineResult<Core::GuestExecutionState> result;
    pthread_cleanup_push([](void* opaque) {
        auto* cleanup = static_cast<NativeExitCleanup*>(opaque);
        activeRoot = nullptr;
        activeHleFrame = nullptr;
        activeHostTarget = nullptr;
        pendingHleContext.reset();
        Core::Fex::ActiveFexExecution = {};
        std::lock_guard lock(cleanup->runtime->threadsMutex);
        cleanup->runtime->threads.erase(cleanup->thread);
        cleanup->runtime->engine->DestroyThread(cleanup->thread);
    }, &cleanup);
    result = runtime.engine->Run(*thread);
    CopyResult(*state, thread->Native->CurrentFrame->State);
    pthread_cleanup_pop(0);
    {
        std::lock_guard lock(runtime.threadsMutex);
        runtime.threads.erase(thread);
        runtime.engine->DestroyThread(thread);
    }
    if (const auto* failure = std::get_if<Core::Fex::EngineFailure>(&result)) return FailureCode(*failure);
    if (root.failure != 0) return root.failure;
    if (Contains(runtime.engine->ReturnRange().Begin, runtime.engine->ReturnRange().Size, state->rip))
        state->rip = runtime.engine->ReturnAddress();
    return LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1;
}
} // namespace

bool CaptureHleContext(HleContext* output) noexcept {
    const auto* frame = activeHleFrame;
    if (output == nullptr || frame == nullptr || frame->gpr[4] == 0 ||
        frame->gpr[4] > UINT64_MAX - 8 || frame->callret_sp > UINT64_MAX - 16) return false;
    output->reset_simd = false;
    std::copy(frame->gpr.begin(), frame->gpr.end(), std::begin(output->gprs));
    std::memcpy(output->xmm, frame->xmm.data(), sizeof(output->xmm));
    // The PS5 dispatcher checked this CALL frame before invoking the service.
    std::memcpy(&output->rip, reinterpret_cast<const void*>(frame->gpr[4]), 8);
    output->gprs[4] += 8;
    output->gprs[0] = 0;
    output->callret_sp = frame->callret_sp + 16;
    output->fcw = frame->fcw;
    output->mxcsr = frame->mxcsr;
    return true;
}

bool SwitchHleContext(const HleContext& context) noexcept {
    if (activeHleFrame == nullptr || context.rip == 0 || context.gprs[4] == 0 ||
        !Data().bridge.QueryExecutableRange(context.rip)) return false;
    pendingHleContext = context;
    return true;
}

std::uint32_t Probe(LumenAndroidFexCoreRuntimeStatusV1* status) noexcept {
    try { std::call_once(Data().once, Initialize); }
    catch (...) { Data().status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1; }
    if (status != nullptr) {
        *status = Data().status;
        std::lock_guard lock(Data().veneersMutex);
        status->registeredHostTargets = static_cast<std::uint32_t>(Data().veneers.size());
    }
    return Data().status.code;
}

const char* Receipt() noexcept {
    const auto code = Probe();
    static std::once_flag once;
    std::call_once(once, [code] {
        std::snprintf(Data().receipt.data(), Data().receipt.size(),
            "LUMEN_PS5_DIRECT_FEX_RUNTIME_V1 ready=%u pin=f2b679f6 engine=gen4-direct "
            "maxInst=%llu maxInstOptIn=%u multiBlock=%u tso=%u halfBarrierTso=%u vectorTso=%u "
            "veneerCache=%u hostPages=4096 diskCache=0 facade=0 mutationSafepoint=0 "
            "guestExceptions=unqualified hle=syscall-ps5-abi fallback=0 "
            "performanceValid=%u fastSignalCheck=%u srtArm64=%u hleIndexedDispatch=%u asyncPipelineCache=%u "
            "barycentricFallbackRequested=%u filesystemCaseCache=%u releaseMemBatch=%u uniformMetadata=%u",
            code == 0 ? 1U : 0U,
            code == 0 ? static_cast<unsigned long long>(FEXCore::Config::Get_MAXINST()()) : 0ULL,
            Data().maxInstOptIn,
            code == 0 ? static_cast<unsigned>(FEXCore::Config::Get_MULTIBLOCK()()) : 0U,
            code == 0 ? static_cast<unsigned>(FEXCore::Config::Get_TSOENABLED()()) : 0U,
            code == 0 ? static_cast<unsigned>(FEXCore::Config::Get_HALFBARRIERTSOENABLED()()) : 0U,
            code == 0 ? static_cast<unsigned>(FEXCore::Config::Get_VECTORTSOENABLED()()) : 0U,
            Data().veneerCacheEnabled ? 1U : 0U,
            DirectPerformance::Get().valid ? 1U : 0U,
            DirectPerformance::Get().fast_signal_check ? 1U : 0U,
            DirectPerformance::Get().srt_arm64 ? 1U : 0U,
            DirectPerformance::Get().hle_indexed_dispatch ? 1U : 0U,
            DirectPerformance::Get().async_pipeline_cache ? 1U : 0U,
            DirectPerformance::Get().barycentric_fallback ? 1U : 0U,
            DirectPerformance::Get().filesystem_case_cache ? 1U : 0U,
            DirectPerformance::Get().release_mem_batch ? 1U : 0U,
            DirectPerformance::Get().uniform_metadata ? 1U : 0U);
    });
    return Data().receipt.data();
}

bool QueryOwnedExecutable(std::uint64_t address, std::uint64_t* base, std::uint64_t* size) noexcept {
    if (base == nullptr || size == nullptr) return false;
    auto& runtime = Data();
    std::lock_guard lock(runtime.veneersMutex);
    for (const auto& [target, veneer] : runtime.veneers) {
        if (Contains(veneer.address, PageSize, address)) {
            *base = veneer.address;
            *size = PageSize;
            return true;
        }
    }
    return false;
}

bool RegisterHostTarget(std::uint64_t target, const void* binding) noexcept {
    if (target == 0 || target == ProbeOperation || Probe() != 0) return false;
    try {
        auto& runtime = Data();
        std::lock_guard lock(runtime.veneersMutex);
        if (const auto existing = runtime.veneers.find(target); existing != runtime.veneers.end())
            return existing->second.Bind(binding);
        const auto operation = runtime.indexedHostCalls ? runtime.nextOperation : target;
        if (runtime.indexedHostCalls && operation >= HostCallTable::Capacity) return false;
        void* page = mmap(nullptr, PageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) return false;
        // Gen4 syscall veneer plus a private RAX slot for PS5 variadic calls
        // and a dynamic continuation for the existing PS5 HLE dispositions.
        std::array<std::uint8_t, 20> bytes {
            0x50, 0x49, 0x89, 0xca, 0x48, 0xb8, 0,0,0,0,0,0,0,0,
            0x0f,0x05, 0x41,0xff,0xe3, 0xc3};
        std::memcpy(bytes.data() + 6, &operation, 8);
        std::memcpy(page, bytes.data(), bytes.size());
        if (mprotect(page, PageSize, PROT_READ | PROT_EXEC) != 0) { munmap(page, PageSize); return false; }
        try {
            auto [entry, inserted] = runtime.veneers.try_emplace(target, target,
                reinterpret_cast<std::uint64_t>(page), binding);
            if (!inserted || (runtime.indexedHostCalls &&
                !runtime.indexedHostCalls->Publish(operation, &entry->second))) {
                if (inserted) runtime.veneers.erase(entry);
                munmap(page, PageSize);
                return false;
            }
        } catch (...) {
            munmap(page, PageSize);
            return false;
        }
        if (runtime.indexedHostCalls) ++runtime.nextOperation;
        return true;
    } catch (...) { return false; }
}

const void* ActiveHostTargetBinding(std::uint64_t target) noexcept {
    return activeHostTarget && activeHostTarget->target == target
        ? activeHostTarget->binding.load(std::memory_order_acquire) : nullptr;
}

std::uint64_t ResolveHostTarget(std::uint64_t target) noexcept {
    std::lock_guard lock(Data().veneersMutex);
    const auto found = Data().veneers.find(target);
    return found == Data().veneers.end() ? 0 : found->second.address;
}

std::uint64_t RootReturn() noexcept { return Probe() == 0 ? Data().engine->ReturnAddress() : 0; }

std::uint32_t RunGuestV1(LumenAndroidFexCoreGuestStateV1* state,
    LumenAndroidFexCoreHostCallV1 call, LumenAndroidFexCoreExecutableQueryV1 query, void* opaque) noexcept {
    try { return Run(state, call, nullptr, query, opaque); }
    catch (...) { return LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1; }
}
std::uint32_t RunGuestV2(LumenAndroidFexCoreGuestStateV1* state,
    LumenAndroidFexCoreHostCallV2 call, LumenAndroidFexCoreExecutableQueryV1 query, void* opaque) noexcept {
    try { return Run(state, nullptr, call, query, opaque); }
    catch (...) { return LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1; }
}
std::uint32_t RunGuestV2WithStack(LumenAndroidFexCoreGuestStateV1* state,
    LumenAndroidFexCoreHostCallV2 call, LumenAndroidFexCoreExecutableQueryV1 query, void* opaque,
    std::uint64_t stack_base, std::uint64_t stack_size) noexcept {
    const Core::GuestExecutionRange stack {stack_base, stack_size, false, true};
    try { return Run(state, nullptr, call, query, opaque, &stack); }
    catch (...) { return LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1; }
}

std::uint32_t QueryGuestTsc(LumenAndroidFexCoreGuestTscV1* output) noexcept {
    if (output == nullptr || output->structSize != sizeof(*output) || output->abiVersion != 1 || output->reserved != 0)
        return LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1;
    auto& runtime = Data();
    if (runtime.frequency == 0) return LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1;
    std::uint64_t counter {}, nativeFrequency {};
    asm volatile("mrs %0, cntfrq_el0" : "=r"(nativeFrequency));
    asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(counter));
    if ((nativeFrequency << runtime.scale) != runtime.frequency)
        return LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
    *output = {};
    output->counter = counter << runtime.scale; output->frequency = runtime.frequency;
    output->nativeFrequency = nativeFrequency; output->scale = runtime.scale;
    return LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1;
}

bool CaptureCurrentGuestState(LumenAndroidFexCoreGuestStateV1* output) noexcept {
    if (output == nullptr || Core::Fex::ActiveFexExecution.Thread == nullptr) return false;
    *output = {sizeof(*output), 1};
    CopyResult(*output, Core::Fex::ActiveFexExecution.Thread->CurrentFrame->State);
    return true;
}
bool QueryGuestRipFromHostPc(std::uint64_t host_pc, std::uint64_t* guest_rip) noexcept {
    const auto& execution = Core::Fex::ActiveFexExecution;
    if (guest_rip == nullptr || execution.Context == nullptr || execution.Thread == nullptr ||
        execution.Thread->CurrentFrame == nullptr) return false;
    *guest_rip = execution.Context->RestoreRIPFromHostPC(execution.Thread, host_pc);
    return true;
}
bool IsActive() noexcept { return activeRoot != nullptr; }
bool CanCancelFromSignal() noexcept {
    return activeRoot != nullptr && activeHleFrame == nullptr && mutationDepth == 0;
}

bool BeginCodeMutation() noexcept {
    if (mutationDepth != 0) { ++mutationDepth; return true; }
    if (Probe() != 0) return false;
    Data().threadsMutex.lock();
    Data().context->GetCodeInvalidationMutex().lock();
    mutationDepth = 1;
    return true;
}

bool EndCodeMutation(std::uint64_t begin, std::uint64_t size, bool committed) noexcept {
    if (mutationDepth == 0 || (committed && (size == 0 || begin > UINT64_MAX - size))) return false;
    if (committed) {
        Data().context->InvalidateCodeBuffersCodeRange(begin, size);
        for (auto* thread : Data().threads)
            Data().context->InvalidateThreadCachedCodeRange(thread->Native, begin, size);
    }
    if (--mutationDepth == 0) {
        Data().context->GetCodeInvalidationMutex().unlock();
        Data().threadsMutex.unlock();
    }
    return true;
}

bool Invalidate(std::uint64_t begin, std::uint64_t size) noexcept {
    if (size == 0 || begin > UINT64_MAX - size || !BeginCodeMutation()) return false;
    return EndCodeMutation(begin, size, true);
}
} // namespace Lumen::Ps5::DirectFex
