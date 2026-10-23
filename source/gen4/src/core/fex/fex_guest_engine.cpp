// SPDX-License-Identifier: MIT

#include "fex_guest_engine.h"
#include "guest_memory_scope.h"
#include "core/libraries/kernel/threads/exception.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

namespace Core::Fex {
namespace {

std::atomic<GuestBridge*> MemoryBridge{};

[[gnu::noinline]] void HostDispatchAnchor() { std::abort(); }

bool Contains(const GuestExecutionRange& range, std::uintptr_t address, std::size_t size) {
    return range.Begin != 0 && range.Size != 0 && size != 0 &&
        range.Size <= UINTPTR_MAX - range.Begin && address >= range.Begin &&
        address - range.Begin < range.Size && size <= range.Size - (address - range.Begin);
}

// These are the documented opaque checkpoint fields of the pinned common
// runtime, not the native host flags. NZCV carry is inverted in that ABI.
void EncodeFlags(LumenAndroidFexCoreGuestStateV3& state, std::uint64_t flags) {
    for (unsigned bit = 0; bit != 22; ++bit) state.flags[bit] = (flags >> bit) & 1;
    state.flags[1] = 1;
    state.flags[10] = (flags & (1U << 10)) != 0 ? 0xff : 1;
    const std::uint32_t nzcv = (((flags >> 7) & 1) << 31) |
        (((flags >> 6) & 1) << 30) | (((flags & 1) ^ 1) << 29) |
        (((flags >> 11) & 1) << 28);
    std::memcpy(state.flags + 24, &nzcv, sizeof(nzcv));
    state.parityRaw = (flags & 4) != 0 ? 0 : 1;
    state.auxiliaryCarryRaw = state.parityRaw ^ (flags & 16);
}

std::uint64_t DecodeFlags(const LumenAndroidFexCoreGuestStateV3& state) {
    std::uint64_t flags = 2;
    constexpr std::uint32_t raw = (1U << 0) | (1U << 2) | (1U << 4) |
        (1U << 6) | (1U << 7) | (1U << 8) | (1U << 10) | (1U << 11);
    for (unsigned bit = 0; bit != 22; ++bit)
        if ((raw & (1U << bit)) == 0) flags |= std::uint64_t{state.flags[bit] & 1U} << bit;
    std::uint32_t nzcv{};
    std::memcpy(&nzcv, state.flags + 24, sizeof(nzcv));
    flags |= ((std::uint64_t{nzcv} >> 29) & 1) ^ 1;
    flags |= ((std::uint64_t{nzcv} >> 30) & 1) << 6;
    flags |= ((std::uint64_t{nzcv} >> 31) & 1) << 7;
    flags |= ((std::uint64_t{nzcv} >> 28) & 1) << 11;
    flags |= std::uint64_t{std::popcount((state.parityRaw & 255U) ^ 1U) & 1U} << 2;
    flags |= (state.auxiliaryCarryRaw ^ state.parityRaw) & 16;
    flags |= std::uint64_t{state.flags[8] & 1U} << 8;
    if (state.flags[10] & 0x80) flags |= 1U << 10;
    return flags;
}

std::uint32_t QueryData(std::uint64_t address, std::uint64_t size,
                        std::uint32_t write) noexcept {
    auto* bridge = MemoryBridge.load(std::memory_order_acquire);
    if (bridge == nullptr || size > SIZE_MAX) return 0;
    try { return bridge->QueryMemoryAccess(address, size, write != 0); }
    catch (...) { return 0; }
}

struct ActiveExecution {
    GuestEngine* engine{};
    GuestBridge* bridge{};
    const GuestExecutionRequest* request{};
    const std::uint64_t* gprs{};
    std::uint64_t rip{};
    ActiveExecution* previous{};
    bool hleDispatch{};
    bool threadExitRequested{};
};
thread_local ActiveExecution* Active{};
struct PendingSignal {
    std::array<std::atomic<std::uintptr_t>, 129> handlers{};
    std::array<std::atomic<bool>, 129> native_wakes{};
    bool delivering{};
};
thread_local PendingSignal Pending;
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
std::mutex SignalThreadsMutex;
std::unordered_map<std::uintptr_t, PendingSignal*> SignalThreads;

bool HasPendingSignal() noexcept {
    for (const auto& handler : Pending.handlers)
        if (handler.load(std::memory_order_acquire) != 0) return true;
    return false;
}

class ActiveScope final {
public:
    explicit ActiveScope(ActiveExecution& state) : state{state} {
        state.previous = Active;
        if (Active == nullptr) {
            std::lock_guard lock{SignalThreadsMutex};
            SignalThreads.emplace(std::bit_cast<std::uintptr_t>(pthread_self()), &Pending);
        }
        Active = &state;
    }
    ~ActiveScope() {
        Active = state.previous;
        if (Active == nullptr) {
            std::lock_guard lock{SignalThreadsMutex};
            SignalThreads.erase(std::bit_cast<std::uintptr_t>(pthread_self()));
        }
    }
private:
    ActiveExecution& state;
};

std::uint32_t QueryExecutable(void* opaque, std::uint64_t address,
    std::uint64_t* base, std::uint64_t* size, std::uint32_t* writable) noexcept {
    auto& active = *static_cast<ActiveExecution*>(opaque);
    try {
        // VM owner is authoritative, including dynamically loaded modules.
        auto range = active.bridge->QueryExecutableRange(address);
        if (!range || !range->Executable || range->Writable || !Contains(*range, address, 1))
            return 0;
        *base = range->Begin; *size = range->Size; *writable = 0;
        return 1;
    } catch (...) { return 0; }
}

std::uint32_t Dispatch(void* opaque, std::uint64_t target, std::uint64_t* gprs,
    LumenAndroidFexCoreSimdViewV2* simd, std::uint64_t*) noexcept {
    auto& active = *static_cast<ActiveExecution*>(opaque);
    if (target != HostDispatchAddress() || gprs == nullptr || simd == nullptr ||
        simd->registerCount != 16 || simd->registerBytes != 16 || simd->registerStride < 16 ||
        simd->registers == nullptr || !QueryData(gprs[4], sizeof(std::uint64_t), 0)) return 0;
    try {
        GuestCpu::HleCallFrame frame{};
        // The veneer saves R12 before placing the operation in it. This also
        // works with the Android V2 bridge, which does not preserve incoming RAX.
        frame.operation = gprs[12];
        if (frame.operation == 0 || frame.operation > 65535) return 0;
        std::memcpy(&gprs[12], reinterpret_cast<const void*>(gprs[4]), sizeof(std::uint64_t));
        gprs[4] += sizeof(std::uint64_t);
        std::copy_n(gprs, 16, frame.gpr.begin());
        frame.rsp = gprs[4];
        for (std::size_t i = 0; i != 16; ++i)
            std::memcpy(frame.xmm[i].data(), simd->registers + i * simd->registerStride, 16);
        struct DispatchScope {
            ActiveExecution& active;
            const std::uint64_t* previousGprs;
            bool previousDispatch;
            ~DispatchScope() {
                active.gprs = previousGprs;
                active.hleDispatch = previousDispatch;
            }
        } dispatchScope{active, active.gprs, active.hleDispatch};
        active.gprs = gprs;
        active.hleDispatch = true;
        const auto result = active.bridge->Invoke(frame);
        if (active.threadExitRequested) return LUMEN_ANDROID_FEXCORE_HOST_CALL_STOP_V1;
        FlushPendingGuestOrbisSignal();
        if (std::holds_alternative<EngineFailure>(result)) return 0;
        std::copy(frame.gpr.begin(), frame.gpr.end(), gprs);
        // Stack ABI belongs to the translator. Typed HLE cannot redirect RET.
        gprs[4] = frame.rsp;
        for (std::size_t i = 0; i != 16; ++i)
            std::memcpy(simd->registers + i * simd->registerStride, frame.xmm[i].data(), 16);
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_RETURN_GUEST_V1;
    } catch (const GuestThreadExit&) {
        active.threadExitRequested = true;
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_STOP_V1;
    } catch (...) { return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1; }
}

std::uint32_t DeliverAtBoundary(const std::uint64_t* gprs, std::uint64_t rip) noexcept {
    if (Active == nullptr || gprs == nullptr) return 0;
    const auto* saved = Active->gprs;
    const auto savedRip = Active->rip;
    Active->gprs = gprs; Active->rip = rip;
    const bool pending = HasPendingSignal();
    bool exited{};
    try {
        FlushPendingGuestOrbisSignal();
    } catch (const GuestThreadExit&) {
        Active->threadExitRequested = true;
        exited = true;
    }
    Active->gprs = saved; Active->rip = savedRip;
    return exited ? LUMEN_ANDROID_FEXCORE_BOUNDARY_STOP_V1 : (pending ? 1 : 0);
}

} // namespace

std::uintptr_t HostDispatchAddress() noexcept {
    return reinterpret_cast<std::uintptr_t>(&HostDispatchAnchor);
}

class GuestEngine::Thread final {
public:
    GuestExecutionRequest request;
    std::thread::id owner{std::this_thread::get_id()};
    bool running{};
};

class GuestEngine::Impl final {
public:
    explicit Impl(GuestBridge& bridge) : bridge{bridge} {}
    GuestBridge& bridge;
    std::mutex mutex;
    std::unordered_set<Thread*> threads;
};

GuestEngine::GuestEngine(std::unique_ptr<Impl> impl) : ImplState{std::move(impl)} {}
GuestEngine::~GuestEngine() {
    if (std::holds_alternative<EngineFailure>(Shutdown())) std::abort();
}

EngineResult<std::unique_ptr<GuestEngine>> GuestEngine::Create(GuestBridge& bridge) {
    const auto page = sysconf(_SC_PAGESIZE);
    if (page != 4096 && page != 16384) return EngineFailure{EngineStage::Config, ENOTSUP};
    if (!LumenAndroidFexCoreConfigureMemoryHandoffV1(LUMEN_FEX_MEMORY_EXCLUSIVE_PUBLICATION_V1))
        return EngineFailure{EngineStage::Config, EBUSY};
    GuestBridge* expected{};
    if (!MemoryBridge.compare_exchange_strong(expected, &bridge))
        return EngineFailure{EngineStage::Context, EBUSY};
    const auto failed = [&](EngineStage stage, int error) -> EngineResult<std::unique_ptr<GuestEngine>> {
        MemoryBridge.store(nullptr, std::memory_order_release);
        return EngineFailure{stage, error};
    };
    if (!Lumen::GuestCpu::RegisterMemoryAccessQuery(QueryData) ||
        !LumenAndroidFexCoreRegisterFreshCallbackDataAccessQueryV1(QueryData) ||
        !LumenAndroidFexCoreRegisterBoundaryDeliveryV1(DeliverAtBoundary))
        return failed(EngineStage::Config, EBUSY);
#if defined(__APPLE__)
    if (!LumenMacOSFexCoreRegisterDataAccessQueryV1(QueryData))
        return failed(EngineStage::Config, EBUSY);
#endif
    LumenAndroidFexCoreRuntimeStatusV1 status{};
    status.structSize = sizeof(status); status.abiVersion = 1;
    if (LumenAndroidFexCoreRuntimeProbeV1(&status) != 0 || status.code != 0)
        return failed(EngineStage::Context, EIO);
    if (!LumenAndroidFexCoreRegisterHostCallTargetV1(HostDispatchAddress()))
        return failed(EngineStage::Bridge, EIO);
    return std::unique_ptr<GuestEngine>{new GuestEngine{std::make_unique<Impl>(bridge)}};
}

EngineResult<GuestEngine::Thread*> GuestEngine::CreateThread(const GuestExecutionRequest& request) {
    if (!ImplState) return EngineFailure{EngineStage::Teardown, ESHUTDOWN};
    if (request.Rip == 0 || request.Rsp < 16 ||
        !ImplState->bridge.QueryMemoryAccess(request.Rsp - 16, 24, true))
        return EngineFailure{EngineStage::Request, EFAULT};
    const auto code = ImplState->bridge.QueryExecutableRange(request.Rip);
    if (!code || !code->Executable || code->Writable || !Contains(*code, request.Rip, 1))
        return EngineFailure{EngineStage::Mapping, EACCES};
    auto thread = std::make_unique<Thread>();
    thread->request = request;
    std::lock_guard lock{ImplState->mutex};
    ImplState->threads.insert(thread.get());
    return thread.release();
}

EngineResult<GuestExecutionState> GuestEngine::Run(Thread& thread) {
    if (!ImplState) return EngineFailure{EngineStage::Teardown, ESHUTDOWN};
    {
        std::lock_guard lock{ImplState->mutex};
        if (!ImplState->threads.contains(&thread) || thread.running ||
            thread.owner != std::this_thread::get_id()) return EngineFailure{EngineStage::Thread, EPERM};
        thread.running = true;
    }
    LumenAndroidFexCoreGuestStateV3 checkpoint{};
    checkpoint.gp.rip = thread.request.Rip;
    std::copy(thread.request.Gpr.begin(), thread.request.Gpr.end(), checkpoint.gp.gprs);
    checkpoint.gp.gprs[4] = thread.request.Rsp;
    checkpoint.gp.fsBase = thread.request.FsBase; checkpoint.gp.gsBase = thread.request.GsBase;
    EncodeFlags(checkpoint, thread.request.Rflags);
    for (std::size_t i = 0; i != 16; ++i)
        std::memcpy(checkpoint.vectorRegisters[i], thread.request.Xmm[i].data(), 16);
    ActiveExecution active{this, &ImplState->bridge, &thread.request, nullptr, thread.request.Rip};
    std::uint32_t code;
    {
        ActiveScope scope{active};
        code = LumenAndroidFexCoreRunGuestV3(&checkpoint, Dispatch, QueryExecutable, &active);
    }
    {
        std::lock_guard lock{ImplState->mutex};
        thread.running = false;
    }
    if (code != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1)
        return EngineFailure{EngineStage::Execute,
            code == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 ? EFAULT : EIO,
            code, checkpoint.gp.rip, checkpoint.gp.gprs[4], checkpoint.gp.faultAddress,
            checkpoint.gp.faultSignal};
    GuestExecutionState result{};
    result.FirstRip = thread.request.Rip; result.Rip = checkpoint.gp.rip;
    result.LastRip = checkpoint.gp.rip; result.Rsp = checkpoint.gp.gprs[4];
    std::copy_n(checkpoint.gp.gprs, 16, result.Gpr.begin());
    result.Rflags = DecodeFlags(checkpoint);
    for (std::size_t i = 0; i != 16; ++i)
        std::memcpy(result.Xmm[i].data(), checkpoint.vectorRegisters[i], 16);
    result.StopReason = active.threadExitRequested ? GuestStopReason::ThreadExited
                                                   : GuestStopReason::Halted;
    return result;
}

EngineResult<GuestExecutionState> GuestEngine::CallGuest(
    std::uintptr_t rip, std::span<const std::uint64_t> arguments,
    std::span<const std::array<std::uint64_t, 2>> xmm) {
    if (!ImplState || Active == nullptr || Active->engine != this || Active->gprs == nullptr)
        return EngineFailure{EngineStage::Thread, ENXIO};
    if (arguments.size() > 32 || xmm.size() > 8)
        return EngineFailure{EngineStage::Request, E2BIG};
    const auto parentRsp = Active->gprs[4];
    const auto stackBytes = arguments.size() > 6 ? (arguments.size() - 6) * sizeof(std::uint64_t) : 0;
    if (parentRsp < 256 + stackBytes + 32) return EngineFailure{EngineStage::Mapping, EFAULT};
    GuestExecutionRequest request{};
    request.Rip = rip;
    request.Rsp = ((parentRsp - 256 - stackBytes) & ~std::uintptr_t{15}) - 8;
    if (!ImplState->bridge.QueryMemoryAccess(request.Rsp - 16, 32 + stackBytes, true))
        return EngineFailure{EngineStage::Mapping, EFAULT};
    const auto returnAddress = ReturnAddress();
    std::memcpy(reinterpret_cast<void*>(request.Rsp), &returnAddress, 8);
    constexpr std::array<std::size_t, 6> registers{7, 6, 2, 1, 8, 9};
    for (std::size_t i = 0; i != std::min(arguments.size(), registers.size()); ++i)
        request.Gpr[registers[i]] = arguments[i];
    for (std::size_t i = 6; i < arguments.size(); ++i)
        std::memcpy(reinterpret_cast<void*>(request.Rsp + (i - 5) * 8), &arguments[i], 8);
    std::copy(xmm.begin(), xmm.end(), request.Xmm.begin());
    request.Gpr[4] = request.Rsp; request.Rflags = 2;
    request.FsBase = Active->request->FsBase; request.GsBase = Active->request->GsBase;
    request.MappedRanges = Active->request->MappedRanges;
    auto created = CreateThread(request);
    if (auto* failure = std::get_if<EngineFailure>(&created)) return *failure;
    auto* thread = std::get<Thread*>(created);
    // A synchronous HLE callback must donate its reader before entering a
    // fresh root. Retaining it can deadlock a publisher waiting for all HLE
    // readers, and the runtime correctly refuses that nested admission.
    std::uint64_t callbackToken{};
    if (Active->hleDispatch &&
        LumenAndroidFexCoreOwnsGuestMemoryReadLeaseV1() != 0 &&
        LumenAndroidFexCoreGuestExecutionReadAdmissionEpochV1() == 0) {
        const auto& ranges = request.MappedRanges;
        const auto stack = std::ranges::find_if(ranges, [parentRsp](const auto& range) {
            return range.Writable && Contains(range, parentRsp - 128, 136);
        });
        const auto tls = std::ranges::find_if(ranges, [&](const auto& range) {
            return range.Writable && Contains(range, request.FsBase, 8);
        });
        if (stack != ranges.end() && (request.FsBase == 0 || tls != ranges.end())) {
            callbackToken = LumenAndroidFexCoreBeginFreshGuestCallbackV1(
                stack->Begin, stack->Size,
                request.FsBase == 0 ? 0 : tls->Begin,
                request.FsBase == 0 ? 0 : tls->Size);
        }
        if (callbackToken == 0) {
            DestroyThread(thread);
            return EngineFailure{EngineStage::Bridge, EBUSY};
        }
    }
    auto result = Run(*thread);
    if (callbackToken != 0 &&
        LumenAndroidFexCoreEndFreshGuestCallbackV1(callbackToken) == 0) {
        DestroyThread(thread);
        return EngineFailure{EngineStage::Bridge, EFAULT};
    }
    auto destroyed = DestroyThread(thread);
    if (auto* failure = std::get_if<EngineFailure>(&destroyed)) return *failure;
    if (auto* state = std::get_if<GuestExecutionState>(&result)) {
        if (state->StopReason == GuestStopReason::ThreadExited) {
            // Keep the stop sticky even if an unrelated HLE catches the host
            // control-transfer exception while unwinding a nested callback.
            Active->threadExitRequested = true;
        } else {
            state->StopReason = GuestStopReason::Returned;
        }
    }
    return result;
}

EngineResult<bool> GuestEngine::Invalidate(Thread& thread, std::uintptr_t begin, std::size_t size) {
    if (!ImplState || begin == 0 || size == 0 || size > UINTPTR_MAX - begin)
        return EngineFailure{EngineStage::Request, EINVAL};
    {
        std::lock_guard lock{ImplState->mutex};
        if (!ImplState->threads.contains(&thread) || thread.running)
            return EngineFailure{EngineStage::Thread, EBUSY};
    }
    MemoryMutationScope mutation;
    if (!mutation.Valid()) return EngineFailure{EngineStage::Invalidate, EBUSY};
    mutation.Publish(begin, size);
    return true;
}

EngineResult<bool> GuestEngine::DestroyThread(Thread*& thread) {
    if (!ImplState || thread == nullptr) return EngineFailure{EngineStage::Thread, EINVAL};
    std::lock_guard lock{ImplState->mutex};
    if (!ImplState->threads.contains(thread) || thread->running ||
        thread->owner != std::this_thread::get_id()) return EngineFailure{EngineStage::Thread, EBUSY};
    ImplState->threads.erase(thread); delete thread; thread = nullptr;
    return true;
}

EngineResult<bool> GuestEngine::Shutdown() {
    if (!ImplState) return true;
    {
        std::lock_guard lock{ImplState->mutex};
        if (!ImplState->threads.empty()) return EngineFailure{EngineStage::Teardown, EBUSY};
    }
    MemoryBridge.store(nullptr, std::memory_order_release);
    ImplState.reset();
    return true;
}

std::uintptr_t GuestEngine::ReturnAddress() const {
    return ImplState ? LumenAndroidFexCoreRootReturnAddressV1() : 0;
}
GuestExecutionRange GuestEngine::ReturnRange() const { return {ReturnAddress(), 1, true, false}; }
GuestExecutionRange GuestEngine::CallbackReturnRange() const { return ReturnRange(); }

#ifndef _WIN32
bool HandleGuestSignal(int, siginfo_t*, void*) noexcept {
    // The common runtime installs and chains its own signal handler. A signal
    // reaching the engine's predecessor was not handled by the translator.
    return false;
}
bool LumenHostQueryGuestRipSyscall(std::uint64_t* rip, std::uint64_t* syscall) noexcept {
    if (!Active || !Active->gprs) return false;
    if (rip) *rip = Active->rip;
    if (syscall) *syscall = Active->gprs[0];
    return true;
}
int SendGuestOrbisSignal(std::uintptr_t host_thread, int native_signal, int signal,
                        std::uintptr_t handler) noexcept {
    if (signal < 1 || signal >= Pending.handlers.size() || handler <= 1) return -1;
    {
        std::lock_guard lock{SignalThreadsMutex};
        const auto found = SignalThreads.find(host_thread);
        if (found == SignalThreads.end()) return -1;
        auto& target = *found->second;
        // A POSIX non-real-time signal coalesces while the same signal is pending.
        if (target.handlers[signal].load(std::memory_order_acquire) != 0) return 0;
        target.native_wakes[signal].store(true, std::memory_order_release);
        target.handlers[signal].store(handler, std::memory_order_release);
        const int result = pthread_kill(std::bit_cast<pthread_t>(host_thread), native_signal);
        if (result != 0) {
            target.handlers[signal].store(0, std::memory_order_release);
            target.native_wakes[signal].store(false, std::memory_order_release);
            return result;
        }
    }
    // Wake blocked HLE above; interrupt a JIT loop at a reconstructed boundary.
    // If Run ended meanwhile, its ordinary native wake already carries delivery.
    LumenAndroidFexCoreRequestGuestBoundaryInterruptV1(host_thread);
    return 0;
}

bool DeliverGuestOrbisSignal(int signal, siginfo_t*, void*, std::uintptr_t handler) noexcept {
    if (handler <= 1 || Active == nullptr || signal < 1 || signal >= Pending.handlers.size())
        return false;
    if (Pending.native_wakes[signal].exchange(false, std::memory_order_acq_rel)) return true;
    Pending.handlers[signal].store(handler, std::memory_order_release);
    return true;
}
void FlushPendingGuestOrbisSignal() {
    if (!Active || !Active->gprs || Pending.delivering) return;
    Pending.delivering = true;
    struct DeliveryRestore {
        ~DeliveryRestore() { Pending.delivering = false; }
    } deliveryRestore;
    // One bounded pass drains distinct pending signals even when their
    // native wakes coalesce into a single translated boundary interrupt.
    for (int signal = 1; signal < Pending.handlers.size(); ++signal) {
        const auto handler = Pending.handlers[signal].exchange(0, std::memory_order_acq_rel);
        if (handler == 0) continue;
        const auto rsp = Active->gprs[4];
        constexpr auto contextSize = (sizeof(Libraries::Kernel::Ucontext) + 15) & ~std::size_t{15};
        if (rsp < contextSize + 512) std::abort();
        const auto address = (rsp - contextSize - 128) & ~std::uintptr_t{15};
        if (!QueryData(address, contextSize, 1)) std::abort();
        Libraries::Kernel::Ucontext context{};
        auto& m = context.uc_mcontext;
        const auto* g = Active->gprs;
        m.mc_rax=g[0]; m.mc_rcx=g[1]; m.mc_rdx=g[2]; m.mc_rbx=g[3];
        m.mc_rsp=g[4]; m.mc_rbp=g[5]; m.mc_rsi=g[6]; m.mc_rdi=g[7];
        m.mc_r8=g[8]; m.mc_r9=g[9]; m.mc_r10=g[10]; m.mc_r11=g[11];
        m.mc_r12=g[12]; m.mc_r13=g[13]; m.mc_r14=g[14]; m.mc_r15=g[15];
        m.mc_rip=Active->rip; m.mc_len=sizeof(m);
        m.mc_fsbase=Active->request->FsBase; m.mc_gsbase=Active->request->GsBase;
        std::memcpy(reinterpret_cast<void*>(address), &context, sizeof(context));
        // Keep the new callback stack below the ucontext and the interrupted red zone.
        std::array<std::uint64_t, 16> callbackGprs{};
        std::copy_n(g, 16, callbackGprs.begin()); callbackGprs[4] = address;
        const auto* previous = Active->gprs; Active->gprs = callbackGprs.data();
        const std::array<std::uint64_t, 2> args{static_cast<std::uint64_t>(signal), address};
        const auto result = Active->engine->CallGuest(handler, args);
        Active->gprs = previous;
        if (std::holds_alternative<EngineFailure>(result)) std::abort();
        if (std::get<GuestExecutionState>(result).StopReason == GuestStopReason::ThreadExited)
            throw GuestThreadExit{};
    }
}
#endif

EngineResult<GuestRunResult> GuestEngine::RunControlledHarness() {
    // The shared runtime probe is the CPU conformance harness. Engine-specific
    // dispatch, callbacks and mapping tests execute through the public adapter.
    LumenAndroidFexCoreRuntimeStatusV1 status{};
    status.structSize = sizeof(status); status.abiVersion = 1;
    if (LumenAndroidFexCoreRuntimeProbeV1(&status) != 0 || status.code != 0)
        return EngineFailure{EngineStage::Execute, EIO};
    GuestRunResult result{};
    result.Gpr = status.guestExecuted != 0;
    result.Bridge = status.hostBridgeExecuted != 0;
    return result;
}

} // namespace Core::Fex
