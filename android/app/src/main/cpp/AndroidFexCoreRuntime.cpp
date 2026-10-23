#include "GuestCpuFexPrivateSymbols.h"
#include "GuestCpuRuntime.h"
#include "HostCallFastIndex.h"
#include "AndroidFexCoreRuntime.h"
#include "FexStructuralDiagnostics.h"
#include "FexFpBoundaryProbePrograms.h"
#include "FexHostFeaturePolicy.h"
#include "FexBlockConfiguration.h"
#include "FexTranslationCache.h"
#include "FexPerformanceTrace.h"
#include "FexHleTrace.h"
#include "CriticalPathTrace.h"

#if defined(__ANDROID__)
#include "AndroidFexCoreLabAdmission.h"
#endif
#if defined(__ANDROID__) || defined(LUMEN_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED)
#include "MemoryHandoff.h"
#endif
#include "AndroidFexCoreMutationBarrier.h"
#if defined(__ANDROID__) || defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
#include "MacOSFexCoreSignalPolicy.h"
#include "MacOSFexCoreFaultStaticGprs.h"
#endif
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
#include "MacOSFexCoreMemoryAccess.h"
#endif

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/HLE/SourcecodeResolver.h>
#include <FEXCore/Utils/Allocator.h>
#if defined(__ANDROID__) || defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
#include <FEXCore/Utils/LumenJitCacheDiagnostics.h>
#endif
#if defined(__ANDROID__) || defined(LUMEN_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED)
#include <FEXCore/Utils/LumenMemoryHandoffAbi.h>
#endif
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/SignalScopeGuards.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/SHMStats.h>
#include <FEXCore/Utils/Threads.h>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <Interface/Core/Frontend.h>
#include <Interface/Core/CPUBackend.h>
#include <Interface/Core/LookupCache.h>
#include <Interface/Context/Context.h>
#include <Interface/IR/IREmitter.h>
#include <Interface/IR/RegisterAllocationData.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <rpmalloc/rpmalloc.h>

#include <algorithm>
#if defined(__ANDROID__)
#include <android/log.h>
#endif
#include <array>
#include <atomic>
#include <cerrno>
#include <cfenv>
#include <chrono>
#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <mutex>
#include <new>
#include <optional>
#include <pthread.h>
#include <signal.h>
#include <thread>
#if defined(__ANDROID__)
#include <sys/auxv.h>
#include <link.h>
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <pthread/qos.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/sysctl.h>
#endif
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
// Keep the adapter's existing call sites shared with Android while routing
// diagnostics to the managed macOS runtime log through stderr.
enum {
    ANDROID_LOG_INFO = 4,
    ANDROID_LOG_ERROR = 6,
    ANDROID_LOG_FATAL = 7,
};

int __android_log_print(int priority, const char* tag,
                        const char* format, ...) noexcept {
    std::fprintf(stderr, "[%s] level=%d ",
                 tag != nullptr ? tag : "LumenFexCoreRuntime", priority);
    va_list arguments;
    va_start(arguments, format);
    const int result = std::vfprintf(stderr, format, arguments);
    va_end(arguments);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    return result;
}
#endif

#if !defined(LUMEN_ANDROID_FEXCORE_OPTIMIZATION_LEVEL) || \
    LUMEN_ANDROID_FEXCORE_OPTIMIZATION_LEVEL != 2
#error "The Android FEXCore runtime must be compiled with the pinned -O2 profile"
#endif
#if !defined(__OPTIMIZE__)
#error "The Android FEXCore runtime was compiled without optimization"
#endif

#if __has_include(<asm/hwcap.h>)
#include <asm/hwcap.h>
#endif

#if defined(__APPLE__) && defined(LUMEN_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED)
// Darwin's static-archive linker requires a definition for optional weak
// imports. Cold no-op definitions keep product linking strict; a synthetic
// fixture may supply a strong observer without changing admission semantics.
extern "C" __attribute__((weak, noinline)) void FEXLumenGuestMemoryBulkDecisionV1(
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint32_t) noexcept {}
extern "C" __attribute__((weak, noinline)) void FEXLumenGuestMemoryBulkWriterArmedV1(std::uint64_t) noexcept {}
extern "C" __attribute__((weak, noinline)) void FEXLumenGuestMemoryPreserveAllObservedV1(std::uint32_t) noexcept {}
extern "C" __attribute__((weak, noinline)) void FEXLumenGuestMemoryHandoffDeniedSignalV1(std::uint64_t) noexcept {}
#elif defined(__ANDROID__)
// Optional synthetic-fixture witness. It grants no admission and is absent
// from the application; execution remains counted across the call.
extern "C" void FEXLumenGuestMemoryBulkDecisionV1(std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint32_t) noexcept __attribute__((weak));
extern "C" void FEXLumenGuestMemoryBulkWriterArmedV1(std::uint64_t) noexcept __attribute__((weak));
#endif

namespace {

constexpr std::uint32_t kRuntimeAbiVersion = 1;
constexpr std::uint64_t kExpectedResult = UINT64_C(0x12345678);
constexpr std::uint32_t kExpectedFourthArgument = UINT32_C(0x6e726378);
constexpr std::size_t kBridgeOffset = 0;
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
constexpr std::size_t kBridgeSyscallOffset = 7;
#else
constexpr std::size_t kBridgeSyscallOffset = 6;
#endif
constexpr std::size_t kHltOffset = 16;
constexpr std::size_t kHostCallReturnOffset = 32;
constexpr std::size_t kProbeOffset = 64;
constexpr std::size_t kFaultProbeOffset = 96;
constexpr std::size_t kDeniedSyscallProbeOffset = 128;
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
constexpr std::size_t kUnalignedAtomicProbeOffset = 192;
#endif
constexpr std::size_t kRootStateBootstrapOffset = 256;
constexpr std::size_t kGuestGprCount = 16;

void InstallStructuralAssertDiagnostics() noexcept {
    namespace Diagnostics = Lumen::FexCore::StructuralDiagnostics;
    // Installed once during initialization, before FEX context creation. The
    // disabled path preserves the existing Android logger and silent Mac path.
    if (!Diagnostics::ExactOne(std::getenv("LUMEN_FEX_STRUCTURAL_ASSERT_TRACE"))) {
        return;
    }
    LogMan::Throw::InstallHandler([](const char*) {
        char output[Diagnostics::kOutputSize];
        Diagnostics::Format(Diagnostics::Kind::Throw, {}, output);
        __android_log_print(ANDROID_LOG_FATAL, "LumenFexCoreRuntime", "%s", output);
    });
    LogMan::Msg::InstallHandler([](LogMan::DebugLevels level, const char* message) {
        if (level != LogMan::ASSERT && level != LogMan::ERROR) {
            return;
        }
        static thread_local bool lastErrorScanTruncated = false;
        Diagnostics::Summary summary;
        Diagnostics::StageSummary stage;
        const bool hasStage = level == LogMan::ERROR && Diagnostics::ParseStage(message, stage);
        if (level == LogMan::ASSERT) {
            summary.scanTruncated = lastErrorScanTruncated;
        } else if (!hasStage) {
            summary = Diagnostics::Summarize(message);
            lastErrorScanTruncated = summary.scanTruncated;
            if (!summary.errorsSection) {
                return;
            }
        }
        // Ordinary FEX messages cannot consume this bounded failure budget.
        // Fatal assertion markers remain visible and the FEX trap is unchanged.
        if (level != LogMan::ASSERT) {
            static std::atomic<unsigned> emitted {0};
            unsigned count = emitted.load(std::memory_order_relaxed);
            for (;;) {
                if (count >= 65) {
                    return;
                }
                if (emitted.compare_exchange_weak(count, count + 1,
                                                 std::memory_order_relaxed)) {
                    break;
                }
            }
            if (count == 64) {
                __android_log_print(ANDROID_LOG_ERROR, "LumenFexCoreRuntime",
                    "LUMEN_FEX_STRUCTURAL_ASSERT_V1 diagnostic_cap=64");
                return;
            }
        }
        char output[Diagnostics::kOutputSize];
        if (hasStage) {
            Diagnostics::FormatStage(stage, output);
        } else {
            Diagnostics::Format(level == LogMan::ASSERT ? Diagnostics::Kind::Assert
                                                       : Diagnostics::Kind::Validation,
                                summary, output);
        }
        __android_log_print(ANDROID_LOG_ERROR, "LumenFexCoreRuntime", "%s", output);
    });
}

bool ExactEnvironmentFlag(const char* name) noexcept {
    const char* const value = std::getenv(name);
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

bool ExactEnvironmentFlagWithDefault(const char* name,
                                     bool defaultValue) noexcept {
    const char* const value = std::getenv(name);
    if (value == nullptr) {
        return defaultValue;
    }
    return value[0] == '1' && value[1] == '\0';
}

// Diagnostic console question only: may a guest range that is writable be
// admitted as executable? The immutable-RX policy below stays the default and
// loses no SMC protection while this selection is absent or not exactly "1".
// The selection is process-latched, non-promotable and shared by both FEX
// hosts so the same oracle can be exercised locally.
bool WritableExecutionExperimentEnabled() noexcept {
    static const bool enabled = [] {
        const char* const value = std::getenv(
            "LUMEN_EXPERIMENTAL_FEX_WRITABLE_EXECUTION");
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

bool HostCallBarrierCoalescingEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv(
            "LUMEN_EXPERIMENTAL_FEX_HLE_BARRIER_COALESCING");
        // Adopted on both active FEX hosts; explicit non-1 values retain
        // the separate transitions for rollback.
        const bool requested =
            value == nullptr || (value[0] == '1' && value[1] == '\0');
        if (requested) {
            std::fputs("LUMEN_FEX_HLE_BARRIER_COALESCING_V1 enabled=1 "
                       "barrier_entry_locks_per_host_call=2\n", stderr);
        }
        return requested;
    }();
    return enabled;
}

bool ShortHostCallAdmissionEnabled() noexcept {
    static const bool enabled = [] {
        const bool requested = ExactEnvironmentFlagWithDefault("LUMEN_FEX_HLE_SHORT_ADMISSION", true);
        std::fprintf(stderr, "LUMEN_FEX_HLE_SHORT_ADMISSION_V1 enabled=%u default=1 "
                     "reader=retained-execution gate=thread-epoch\n", requested ? 1U : 0U);
        return requested;
    }();
    return enabled;
}

bool SynchronousMemoryShortAdmissionEnabled() noexcept {
    static const bool enabled = [] {
        const bool selected = ExactEnvironmentFlagWithDefault(
            "LUMEN_EXPERIMENTAL_FEX_HLE_SYNC_MEMORY_SHORT", true);
        std::fprintf(stderr, "LUMEN_FEX_HLE_SYNC_MEMORY_SHORT_V1 enabled=%u default=1 "
            "protocol=explicit-frozen-exclusive contract=synchronous-guest-memory\n", selected ? 1U : 0U);
        return selected;
    }();
    return enabled;
}

#if defined(__APPLE__)
bool DirectUnalignedTsoEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("LUMEN_FEX_MACOS_DIRECT_UNALIGNED");
        // Adopted on macOS after the comparable Get Started qualification.
        // Explicit zero retains the original signal path for rollback;
        // InitializeRuntime rejects every malformed selector before execution.
        return value == nullptr || std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
#endif

#if defined(__APPLE__)
constexpr std::size_t HostPageSize() noexcept { return 16384; }
#else
// Written once by InitializeRuntime after admission, before any mapping or
// FEX context is created. All readers run behind g_runtime.once.
constinit std::size_t g_hostPageSize = 0;
std::size_t HostPageSize() noexcept { return g_hostPageSize; }
#endif
constexpr std::size_t kCallRetStackSize =
    FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
static_assert(kCallRetStackSize % 16384 == 0);
#if defined(__ANDROID__) && !defined(LUMEN_PS5_FEX_C13)
static_assert(alignof(FEXCore::Core::InternalThreadState) >= 16384);
static_assert(offsetof(FEXCore::Core::InternalThreadState, InterruptFaultPage) % 16384 == 0);
static_assert(sizeof(FEXCore::Core::InternalThreadState::InterruptFaultPage) == 16384);
#endif
constexpr std::uint32_t kMinimumLogicalCores = 2;
constexpr std::uint32_t kMaximumCpuidLogicalCores = 255;
constexpr auto kGuestMutationSafepointTimeout = std::chrono::seconds(2);

#if defined(__ANDROID__) || defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
constexpr std::size_t kInterruptPageOffset =
    offsetof(FEXCore::Core::InternalThreadState, InterruptFaultPage) -
    offsetof(FEXCore::Core::InternalThreadState, BaseFrameState);
constexpr std::uint32_t kInterruptDoorbellStore =
    Lumen::MacOS::FexCore::EncodeInterruptDoorbellStore(
        kInterruptPageOffset);
static_assert(kInterruptPageOffset % sizeof(std::uint64_t) == 0);
static_assert(kInterruptPageOffset <= 32760);
static_assert(kInterruptDoorbellStore != 0);
#endif

static_assert(FEXCore::X86State::REG_RAX == 0);
static_assert(FEXCore::X86State::REG_RCX == 1);
static_assert(FEXCore::X86State::REG_RSP == 4);
static_assert(FEXCore::X86State::REG_R10 == 10);
static_assert(FEXCore::X86State::REG_R11 == 11);
static_assert(FEXCore::Core::CPUState::NUM_GPRS == kGuestGprCount);

using Lumen::Android::FexCoreLab::ExecutionBarrierLease;
using Lumen::Android::FexCoreLab::GuestMemoryMutationBarrier;
using Lumen::Android::FexCoreLab::GuestMemoryMutationScope;
using Lumen::Android::FexCoreLab::NormalizeGuestInvalidationRange;

GuestMemoryMutationBarrier& GuestMutationBarrier() {
    // The VM hooks may be reached during another translation unit's static
    // initialization. Lazily construct and intentionally retain the barrier so
    // neither initialization nor destruction order can expose an unbuilt lock.
    static GuestMemoryMutationBarrier* const barrier =
        new GuestMemoryMutationBarrier;
    return *barrier;
}

constinit std::atomic_bool g_contextReadyForInvalidation {false};
constinit std::atomic<std::uint64_t> g_boundaryInterruptSequence {0};
constinit std::atomic<LumenAndroidFexCoreBoundaryDeliveryV1>
    g_boundaryDelivery {nullptr};
static_assert(decltype(g_boundaryDelivery)::is_always_lock_free);
constinit std::atomic<LumenAndroidFexCoreFreshCallbackDataAccessQueryV1>
    g_freshCallbackDataAccessQuery {nullptr};
static_assert(decltype(g_freshCallbackDataAccessQuery)::is_always_lock_free);
constinit std::atomic_bool g_freshCallbackBeginReported {false};
constinit std::atomic_bool g_freshCallbackRestoreReported {false};
#if defined(__ANDROID__)
constinit std::atomic<std::uint32_t> g_lastAndroidSafepointSignalCode {0};
#endif
constinit std::atomic<std::uint64_t> g_runSequence {0};
#if defined(__APPLE__)
constinit std::atomic<LumenMacOSFexCoreDataAccessQueryV1> g_dataAccessQuery {nullptr};
static_assert(decltype(g_dataAccessQuery)::is_always_lock_free);
#endif

// Captured once at Run admission. All lookup storage is owned and immutable, so
// nested Runs and teardown cannot borrow a reallocated global registry buffer.
// Preserve registration order for Custom-IR precompile; membership has its own index.
class HostTargetSnapshot {
public:
    explicit HostTargetSnapshot(const std::vector<std::uint64_t>& targets)
        : order_(targets),
          fastIndex_(Lumen::GuestCpu::HostCallFastIndexEnabled()
              ? Lumen::GuestCpu::HostCallFastIndex(targets) : Lumen::GuestCpu::HostCallFastIndex{}),
          fastIndexEnabled_(Lumen::GuestCpu::HostCallFastIndexEnabled() && fastIndex_.IsUsable()),
          sorted_(fastIndexEnabled_ ? std::vector<std::uint64_t>{} : Sorted(targets)) {}

    bool Contains(std::uint64_t target) const noexcept {
        if (target == 0 || target == UINT64_MAX) return false;
        return fastIndexEnabled_
            ? fastIndex_.Find(target) != Lumen::GuestCpu::HostCallFastIndex::NotFound
            : std::binary_search(sorted_.begin(), sorted_.end(), target);
    }
    const std::vector<std::uint64_t>& RegistrationOrder() const noexcept {
        return order_;
    }

private:
    static std::vector<std::uint64_t> Sorted(std::vector<std::uint64_t> targets) {
        std::sort(targets.begin(), targets.end());
        return targets;
    }
    const std::vector<std::uint64_t> order_;
    const Lumen::GuestCpu::HostCallFastIndex fastIndex_;
    const bool fastIndexEnabled_;
    const std::vector<std::uint64_t> sorted_;
};

enum class HostTargetPublication { Rejected, Existing, Added };

enum class DiagnosticGuestSignalRoute : sig_atomic_t {
    None,
    Ill,
    Trap,
    Segv,
};

// Caller holds activeThreadsMutex then targetMutex. Even an exact duplicate
// is refused during an active Run; only successful Custom-IR registration may
// publish a new authorized address. The fixture supplies a nonexecuting owner.
template <typename RegisterCustomIr>
HostTargetPublication PublishHostTarget(std::vector<std::uint64_t>& targets,
    std::uint64_t target, bool active, RegisterCustomIr&& registerCustomIr) {
    if (active || target == 0 || target == UINT64_MAX) {
        return HostTargetPublication::Rejected;
    }
    if (std::find(targets.begin(), targets.end(), target) != targets.end()) {
        return HostTargetPublication::Existing;
    }
    if (!registerCustomIr()) return HostTargetPublication::Rejected;
    targets.push_back(target);
    return HostTargetPublication::Added;
}

struct InlineJitUnit {
    std::uint64_t begin {};
    std::uint64_t size {};
    std::uint64_t guestEntry {};
    bool valid {};

    bool Contains(std::uint64_t address, std::uint64_t width = sizeof(std::uint32_t)) const noexcept {
        return valid && address >= begin && address - begin >= sizeof(FEXCore::CPU::CPUBackend::JITCodeHeader) &&
            address - begin < size && width != 0 && width <= size - (address - begin);
    }
};

#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
struct SafepointCodeWitness {
    InlineJitUnit unit;
    std::uint64_t stateRip {};
    std::uint64_t pcGuestRip {};
    std::uint64_t branchTarget {};
    std::uint64_t branchGuestRip {};
    std::uint32_t followingInstruction {};
    bool branchTargetInBuffer {};
};
#endif

struct ActiveRun {
    ActiveRun* parentRun {};
    FEXCore::Context::Context* context {};
    FEXCore::Core::InternalThreadState* thread {};
    LumenAndroidFexCoreHostCallV1 hostCall {};
    LumenAndroidFexCoreHostCallV2 hostCallV2 {};
    LumenAndroidFexCoreExecutableQueryV1 executableQuery {};
    void* opaque {};
    std::uint64_t exitAddress {};
    volatile sig_atomic_t failure {
        LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1};
    volatile sig_atomic_t faultSignal {};
    volatile std::uintptr_t faultAddress {};
#if defined(__APPLE__)
    // Set by a direct scalar helper only on rejection. The generated BRK
    // consumes this descriptor after it restores every guest register.
    volatile std::uint64_t directUnalignedDeniedRip {};
    volatile std::uintptr_t directUnalignedDeniedAddress {};
#endif
    volatile sig_atomic_t forcedStop {};
    bool faultContextEnabled {};
    bool diagnosticGuestRoute {};
    std::uint64_t runId {};
    std::atomic<std::uint32_t> diagnosticCaptureState {};
    volatile sig_atomic_t diagnosticSignal {};
    volatile sig_atomic_t diagnosticSignalCode {};
    volatile std::uintptr_t diagnosticFaultAddress {};
    volatile std::uintptr_t diagnosticHostPc {};
    volatile std::uint64_t diagnosticGuestRip {};
    volatile std::uint64_t diagnosticR10 {};
    volatile std::uint64_t diagnosticR11 {};
    volatile sig_atomic_t diagnosticActiveHostCall {};
    volatile std::uint64_t diagnosticHostCallTarget {};
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD) || defined(__ANDROID__)
    bool faultStaticGprsEnabled {};
    std::uint16_t diagnosticStaticGprCount {};
    std::array<std::uint8_t, 16> diagnosticStaticGprMapping {};
    Lumen::MacOS::FexCore::RawStaticGprSnapshot diagnosticStaticGprs {};
    bool diagnosticHostRegistersValid {};
    std::array<std::uint64_t, 31> diagnosticHostRegisters {};
    std::uint64_t diagnosticHostSp {};
#if defined(__ANDROID__)
    Lumen::MacOS::FexCore::RawAndroidHostCodeSnapshot diagnosticHostCode {};
#else
    Lumen::MacOS::FexCore::RawHostCodeSnapshot diagnosticHostCode {};
#endif
#endif
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
    bool faultHostCodeExtendedEnabled {};
    Lumen::MacOS::FexCore::RawExtendedHostCodeSnapshot diagnosticExtendedHostCode {};
    std::atomic<std::uint64_t> diagnosticDeniedExecutableAddress {};
    std::atomic<std::uint64_t> diagnosticSourceCompilationEntry {};
    volatile sig_atomic_t diagnosticGuestSignalRoute {
        static_cast<sig_atomic_t>(DiagnosticGuestSignalRoute::None)};
    volatile sig_atomic_t diagnosticSyncFaultGenerated {};
    volatile sig_atomic_t diagnosticSyncFaultSignal {};
    volatile sig_atomic_t diagnosticSyncFaultTrapNo {};
    volatile sig_atomic_t diagnosticSyncFaultSiCode {};
    volatile sig_atomic_t diagnosticSyncFaultErrorCode {};
    volatile sig_atomic_t diagnosticGuestCsIndex {};
    volatile sig_atomic_t diagnosticGuestIoplBit12 {};
#endif
    volatile sig_atomic_t diagnosticInJit {};
    volatile sig_atomic_t diagnosticInSyscall {};
    volatile sig_atomic_t activeHostCall {};
    volatile std::uint64_t activeHostCallTarget {};
    // Cross-thread timeout observations are independent of the owner-thread
    // protocol fields above. They identify candidates, never grant admission.
    std::atomic<std::uint64_t> observedHostCallTarget {};
    std::atomic<std::uint64_t> observedHostWaitToken {};
    std::atomic<std::uint64_t> observedFreshCallbackToken {};
    std::atomic<std::uint64_t> safepointEpoch {};
    volatile sig_atomic_t safepointInFlight {};
    std::atomic<std::uint64_t> safepointResumeRip {};
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
    volatile std::uint64_t diagnosticSafepointHostPc {};
    volatile std::uint64_t diagnosticSafepointResumeHostPc {};
    SafepointCodeWitness diagnosticSafepointCodeWitness {};
    volatile sig_atomic_t diagnosticSafepointRejected {};
    unsigned diagnosticSafepointCount {};
#endif
    // Host pthread identity captured at admission (the value this thread
    // observes from pthread_self) and the one-shot token of an armed
    // generated-code boundary interrupt. Request/cancel mutate the token
    // under activeThreadsMutex; the fault path and the pause handler read it
    // atomically. The fault path admits a protection fault while either the
    // mutation epoch or this token is armed.
    std::uint64_t hostThread {};
    std::uint64_t nativeThreadId {};
    std::atomic<std::uint64_t> boundaryInterruptToken {};
    const std::fenv_t* hostFpEnvironment {};
    bool runtimeProbe {};
    std::uint64_t probeDataBase {};
    std::uint64_t probeDataSize {};
    std::uint64_t ownedStackBase {};
    std::uint64_t ownedStackSize {};
    LumenMacOSFexCoreOwnedStackAccessQueryV1 ownedStackQuery {};
    std::optional<HostTargetSnapshot> hostTargets {};
    // Token and CPU state are owner-thread-only. Watch publication and saved
    // ranges use activeThreadsMutex; mapping revocation is monotonic until the
    // owning Run leaves and can be observed without borrowing that mutex.
    std::uint64_t freshCallbackToken {};
    std::uint64_t freshCallbackReturn {};
    std::uint64_t freshCallbackRsp {};
    std::uint64_t freshCallbackFs {};
    std::uint64_t freshCodeBase {}, freshCodeSize {};
    std::uint64_t freshStackBase {}, freshStackSize {};
    std::uint64_t freshTlsBase {}, freshTlsSize {};
    std::atomic_bool freshCallbackLifetimeRevoked {false};
    bool freshCallbackWatchActive {};
    bool freshCallbackRestoreFailed {};
    bool performanceTraceEnabled {};
    FEXCore::SHMStats::ThreadStats* performanceStats {};
    std::uint64_t performanceCompileTicks {}, performanceDiskTicks {},
        performanceJitCount {}, performanceNativeLoads {}, performanceWaitToken {};
    std::uint64_t performanceCacheMisses {}, performanceCacheReadLockTicks {},
        performanceCacheWriteLockTicks {}, performanceDiskMisses {};
    std::optional<Lumen::Performance::Scope> performanceWait {};
    std::uint64_t shortHostCalls {}, shortHostCallFallbacks {}, ordinaryHostCalls {};
    std::uint64_t syncMemoryShortEntries {}, syncMemoryShortCompleted {},
        syncMemoryShortDemoted {}, syncMemoryShortFallbacks {};
    bool activeSyncMemoryShort {}, activeShortDemoted {};
    unsigned activeHleTraceSlot {Lumen::Performance::HleWindow::Overflow};
    std::unique_ptr<Lumen::Performance::HleWindow> hleWindow {};
};

void SamplePerformanceStats(ActiveRun& run) noexcept {
    if (!run.performanceTraceEnabled || run.performanceStats == nullptr) return;
    const auto& stats = *run.performanceStats;
    const auto compile = stats.AccumulatedJITTime;
#if defined(LUMEN_PS5_FEX_C13)
    constexpr std::uint64_t disk = 0, loads = 0, diskMisses = 0;
#else
    const auto disk = stats.AccumulatedDiskCacheLookupTime;
    const auto loads = stats.AccumulatedDiskCacheHitCount;
    const auto diskMisses = stats.AccumulatedDiskCacheMissCount;
#endif
    const auto count = stats.AccumulatedJITCount;
    const auto cacheMisses = stats.AccumulatedCacheMissCount;
    const auto readLock = stats.AccumulatedCacheReadLockTime;
    const auto writeLock = stats.AccumulatedCacheWriteLockTime;
    std::uint64_t frequency {};
    asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    Lumen::Performance::CompileStats(compile - run.performanceCompileTicks,
        disk - run.performanceDiskTicks, count - run.performanceJitCount,
        loads - run.performanceNativeLoads, frequency);
    Lumen::Performance::CacheStats(cacheMisses - run.performanceCacheMisses,
        readLock - run.performanceCacheReadLockTicks,
        writeLock - run.performanceCacheWriteLockTicks,
        diskMisses - run.performanceDiskMisses);
    run.performanceCompileTicks = compile;
    run.performanceDiskTicks = disk;
    run.performanceJitCount = count;
    run.performanceNativeLoads = loads;
    run.performanceCacheMisses = cacheMisses;
    run.performanceCacheReadLockTicks = readLock;
    run.performanceCacheWriteLockTicks = writeLock;
    run.performanceDiskMisses = diskMisses;
}

InlineJitUnit CaptureInlineJitUnit(const ActiveRun& run) noexcept {
    using Backend = FEXCore::CPU::CPUBackend;
    const auto begin = run.thread->CurrentFrame->State.InlineJITBlockHeader;
    const auto readable = [&](std::uint64_t address, std::uint64_t size) {
        return address != 0 && size != 0 && address <= UINT64_MAX - (size - 1) &&
            run.context->IsAddressInCodeBuffer(run.thread, address) &&
            run.context->IsAddressInCodeBuffer(run.thread, address + size - 1);
    };
    if (begin % alignof(Backend::JITCodeHeader) != 0 ||
        !readable(begin, sizeof(Backend::JITCodeHeader))) return {};
    Backend::JITCodeHeader header;
    std::memcpy(&header, reinterpret_cast<const void*>(begin), sizeof(header));
    if (header.OffsetToBlockTail < sizeof(header) ||
        begin > UINT64_MAX - header.OffsetToBlockTail) return {};
    const auto tailAddress = begin + header.OffsetToBlockTail;
    if (tailAddress % alignof(Backend::JITCodeTail) != 0 ||
        !readable(tailAddress, sizeof(Backend::JITCodeTail))) return {};
    Backend::JITCodeTail tail;
    std::memcpy(&tail, reinterpret_cast<const void*>(tailAddress), sizeof(tail));
    if (tail.Size < header.OffsetToBlockTail ||
        tail.Size - header.OffsetToBlockTail < sizeof(tail) ||
        tail.OffsetToRIPEntries < sizeof(tail) ||
        tail.OffsetToRIPEntries > tail.Size - header.OffsetToBlockTail ||
        tail.NumberOfRIPEntries > tail.Size - header.OffsetToBlockTail - tail.OffsetToRIPEntries ||
        !readable(begin, tail.Size)) return {};
    // The executable span ends before the tail and its RIP metadata.
    return {begin, header.OffsetToBlockTail, tail.RIP, true};
}

#if defined(__APPLE__)
bool HasGuestDataAccessUnderExecutionBarrier(const ActiveRun& run,
    std::uint64_t address, std::uint64_t size, bool write) noexcept {
    if (address == 0 || size == 0 || address > UINT64_MAX - size) return false;
    // The startup probe owns this private raw RW stack interval. It never
    // admits an unmanaged pointer supplied by an ordinary guest Run.
    if (run.runtimeProbe && run.probeDataBase != 0 &&
        address >= run.probeDataBase && address - run.probeDataBase < run.probeDataSize &&
        size <= run.probeDataSize - (address - run.probeDataBase)) return true;
    const auto query = g_dataAccessQuery.load(std::memory_order_acquire);
    if (query != nullptr && query(address, size, write ? 1 : 0) != 0) return true;
    // Synchronous nested Runs may use an argument in a still-live ancestor
    // stack. Each owner supplies its own exact interval and permission proof.
    for (const ActiveRun* owner = &run; owner != nullptr; owner = owner->parentRun) {
        if (owner->ownedStackQuery != nullptr && owner->ownedStackBase != 0 &&
            address >= owner->ownedStackBase && address - owner->ownedStackBase < owner->ownedStackSize &&
            size <= owner->ownedStackSize - (address - owner->ownedStackBase) &&
            owner->ownedStackQuery(address, size, write ? 1 : 0,
                                    owner->ownedStackBase, owner->ownedStackSize) != 0) return true;
    }
    return false;
}
#endif

bool FreshCallbackDataAllowed(const ActiveRun& run, std::uint64_t address,
    std::uint64_t size, bool write) noexcept {
#if defined(__APPLE__)
    return HasGuestDataAccessUnderExecutionBarrier(run, address, size, write);
#else
    if (address == 0 || size == 0 || address > UINT64_MAX - size) return false;
    const auto query = g_freshCallbackDataAccessQuery.load(std::memory_order_acquire);
    if (query != nullptr && query(address, size, write ? 1U : 0U) != 0) return true;
    for (const ActiveRun* owner = &run; owner != nullptr; owner = owner->parentRun) {
        if (owner->ownedStackQuery != nullptr && owner->ownedStackBase != 0 &&
            address >= owner->ownedStackBase && address - owner->ownedStackBase < owner->ownedStackSize &&
            size <= owner->ownedStackSize - (address - owner->ownedStackBase) &&
            owner->ownedStackQuery(address, size, write ? 1U : 0U,
                owner->ownedStackBase, owner->ownedStackSize) != 0) return true;
    }
    return false;
#endif
}

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
std::once_flag g_faultContextConfigurationOnce;

const char* DiagnosticGuestSignalRouteName(sig_atomic_t route) noexcept {
    switch (static_cast<DiagnosticGuestSignalRoute>(route)) {
    case DiagnosticGuestSignalRoute::Ill:
        return "ill";
    case DiagnosticGuestSignalRoute::Trap:
        return "trap";
    case DiagnosticGuestSignalRoute::Segv:
        return "segv";
    case DiagnosticGuestSignalRoute::None:
    default:
        return "none";
    }
}

[[gnu::noinline]] void ReportFaultContext(const ActiveRun& run) noexcept {
    const auto hostPc = static_cast<std::uintptr_t>(
        run.diagnosticHostPc);
    const auto target = static_cast<std::uint64_t>(
        run.diagnosticHostCallTarget);
    const auto deniedExecutableAddress =
        run.diagnosticDeniedExecutableAddress.load(
            std::memory_order_acquire);
    const auto sourceCompilationEntry =
        run.diagnosticSourceCompilationEntry.load(
            std::memory_order_relaxed);
    const bool sourceCompilationEntryValid =
        sourceCompilationEntry != 0 &&
        sourceCompilationEntry != deniedExecutableAddress &&
        deniedExecutableAddress == run.diagnosticGuestRip;
    std::array<char, 1024> report {};
    const int reportSize = std::snprintf(
        report.data(), report.size(),
        "[MACOS_FEXCORE_FAULT_CONTEXT_V4] run=%llu scope=%s signal=%d "
        "signalCode=%d fault=0x%016llx hostPc=0x%016llx "
        "guestRip=0x%016llx r10=0x%016llx r11=0x%016llx "
        "capturedGprsValid=%u inJit=%u inSyscall=%u "
        "activeHostCall=%u target=0x%016llx "
        "guestSignalRoute=%s syncFaultGenerated=%u syncFaultSignal=%d "
        "syncFaultMetadataValid=%u syncFaultTrapNo=%u syncFaultSiCode=%u "
        "syncFaultErrorCode=0x%04x guestCsIndex=0x%04x "
        "guestIoplBit12=%u "
        "sourceCompilationEntryValid=%u sourceCompilationEntry=0x%016llx "
        "transitionTarget=0x%016llx\n",
        static_cast<unsigned long long>(run.runId),
        run.diagnosticGuestRoute ? "guest" : "runtime-probe",
        static_cast<int>(run.diagnosticSignal),
        static_cast<int>(run.diagnosticSignalCode),
        static_cast<unsigned long long>(run.diagnosticFaultAddress),
        static_cast<unsigned long long>(hostPc),
        static_cast<unsigned long long>(run.diagnosticGuestRip),
        static_cast<unsigned long long>(run.diagnosticR10),
        static_cast<unsigned long long>(run.diagnosticR11),
        static_cast<unsigned>(run.diagnosticInJit == 0),
        static_cast<unsigned>(run.diagnosticInJit != 0),
        static_cast<unsigned>(run.diagnosticInSyscall != 0),
        static_cast<unsigned>(target != 0),
        static_cast<unsigned long long>(target),
        DiagnosticGuestSignalRouteName(
            run.diagnosticGuestSignalRoute),
        static_cast<unsigned>(
            run.diagnosticSyncFaultGenerated != 0),
        static_cast<int>(run.diagnosticSyncFaultSignal),
        static_cast<unsigned>(
            run.diagnosticSyncFaultGenerated != 0),
        static_cast<unsigned>(run.diagnosticSyncFaultTrapNo),
        static_cast<unsigned>(run.diagnosticSyncFaultSiCode),
        static_cast<unsigned>(run.diagnosticSyncFaultErrorCode),
        static_cast<unsigned>(run.diagnosticGuestCsIndex),
        static_cast<unsigned>(run.diagnosticGuestIoplBit12),
        static_cast<unsigned>(sourceCompilationEntryValid),
        static_cast<unsigned long long>(
            sourceCompilationEntryValid ? sourceCompilationEntry : 0),
        static_cast<unsigned long long>(deniedExecutableAddress));
    if (reportSize > 0) {
        const size_t byteCount = std::min(
            static_cast<size_t>(reportSize), report.size() - 1);
        (void)::write(STDERR_FILENO, report.data(), byteCount);
    }
    if (run.diagnosticSafepointRejected != 0) {
        const auto& witness = run.diagnosticSafepointCodeWitness;
        const int size = std::snprintf(report.data(), report.size(),
            "[MACOS_FEXCORE_SAFEPOINT_REJECTED_CODE_WITNESS_V1] run=%llu scope=%s "
            "unitValid=%u unitBegin=0x%016llx unitSize=%llu unitGuestEntry=0x%016llx "
            "hostPc=0x%016llx pcInUnit=%u stateRip=0x%016llx reconstructedRipAvailable=0\n",
            static_cast<unsigned long long>(run.runId), run.diagnosticGuestRoute ? "guest" : "runtime-probe",
            witness.unit.valid, static_cast<unsigned long long>(witness.unit.begin),
            static_cast<unsigned long long>(witness.unit.size), static_cast<unsigned long long>(witness.unit.guestEntry),
            static_cast<unsigned long long>(run.diagnosticSafepointHostPc),
            witness.unit.Contains(run.diagnosticSafepointHostPc), static_cast<unsigned long long>(witness.stateRip));
        if (size > 0) (void)::write(STDERR_FILENO, report.data(), std::min(static_cast<std::size_t>(size), report.size() - 1));
    }
    if (run.faultStaticGprsEnabled) {
        const auto& values = run.diagnosticStaticGprs.values;
        std::array<char, 1024> staticReport {};
        const int staticReportSize = std::snprintf(
            staticReport.data(), staticReport.size(),
            "[MACOS_FEXCORE_FAULT_STATIC_GPRS_V1] run=%llu scope=%s "
            "rawOnly=1 staticMapValid=%u count=%u inJit=%u "
            "map=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u "
            "rax=%016llx rcx=%016llx rdx=%016llx rbx=%016llx "
            "rsp=%016llx rbp=%016llx rsi=%016llx rdi=%016llx "
            "r8=%016llx r9=%016llx r10=%016llx r11=%016llx "
            "r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n",
            static_cast<unsigned long long>(run.runId),
            run.diagnosticGuestRoute ? "guest" : "runtime-probe",
            static_cast<unsigned>(run.diagnosticStaticGprs.valid),
            static_cast<unsigned>(run.diagnosticStaticGprCount),
            static_cast<unsigned>(run.diagnosticInJit != 0),
            static_cast<unsigned>(run.diagnosticStaticGprMapping[0]), static_cast<unsigned>(run.diagnosticStaticGprMapping[1]),
            static_cast<unsigned>(run.diagnosticStaticGprMapping[2]), static_cast<unsigned>(run.diagnosticStaticGprMapping[3]),
            static_cast<unsigned>(run.diagnosticStaticGprMapping[4]), static_cast<unsigned>(run.diagnosticStaticGprMapping[5]),
            static_cast<unsigned>(run.diagnosticStaticGprMapping[6]), static_cast<unsigned>(run.diagnosticStaticGprMapping[7]),
            static_cast<unsigned>(run.diagnosticStaticGprMapping[8]), static_cast<unsigned>(run.diagnosticStaticGprMapping[9]),
            static_cast<unsigned>(run.diagnosticStaticGprMapping[10]), static_cast<unsigned>(run.diagnosticStaticGprMapping[11]),
            static_cast<unsigned>(run.diagnosticStaticGprMapping[12]), static_cast<unsigned>(run.diagnosticStaticGprMapping[13]),
            static_cast<unsigned>(run.diagnosticStaticGprMapping[14]), static_cast<unsigned>(run.diagnosticStaticGprMapping[15]),
            static_cast<unsigned long long>(values[0]),
            static_cast<unsigned long long>(values[1]),
            static_cast<unsigned long long>(values[2]),
            static_cast<unsigned long long>(values[3]),
            static_cast<unsigned long long>(values[4]),
            static_cast<unsigned long long>(values[5]),
            static_cast<unsigned long long>(values[6]),
            static_cast<unsigned long long>(values[7]),
            static_cast<unsigned long long>(values[8]),
            static_cast<unsigned long long>(values[9]),
            static_cast<unsigned long long>(values[10]),
            static_cast<unsigned long long>(values[11]),
            static_cast<unsigned long long>(values[12]),
            static_cast<unsigned long long>(values[13]),
            static_cast<unsigned long long>(values[14]),
            static_cast<unsigned long long>(values[15]));
        if (staticReportSize > 0) {
            (void)::write(STDERR_FILENO, staticReport.data(), std::min(
                static_cast<std::size_t>(staticReportSize), staticReport.size() - 1));
        }
        std::array<char, 1024> hostReport {};
        const int hostHeaderSize = std::snprintf(
            hostReport.data(), hostReport.size(),
            "[MACOS_FEXCORE_FAULT_HOST_CONTEXT_V1] run=%llu scope=%s "
            "rawOnly=1 hostRegistersValid=%u count=31 inJit=%u "
            "hostPc=0x%016llx hostSp=0x%016llx "
            "faultInstructionValid=%u faultInstruction=0x%08x radius=%zu",
            static_cast<unsigned long long>(run.runId),
            run.diagnosticGuestRoute ? "guest" : "runtime-probe",
            static_cast<unsigned>(run.diagnosticHostRegistersValid),
            static_cast<unsigned>(run.diagnosticInJit != 0),
            static_cast<unsigned long long>(hostPc),
            static_cast<unsigned long long>(run.diagnosticHostSp),
            static_cast<unsigned>(run.diagnosticHostCode.faultInstructionValid),
            static_cast<unsigned>(run.diagnosticHostCode.instructions[
                Lumen::MacOS::FexCore::RawHostCodeRadius]),
            Lumen::MacOS::FexCore::RawHostCodeRadius);
        std::size_t hostBytes = hostHeaderSize > 0 ? std::min(
            static_cast<std::size_t>(hostHeaderSize), hostReport.size() - 1) : 0;
        for (std::size_t index = 0; index != run.diagnosticHostRegisters.size(); ++index) {
            const int fieldSize = std::snprintf(hostReport.data() + hostBytes,
                hostReport.size() - hostBytes, " x%zu=%016llx", index,
                static_cast<unsigned long long>(run.diagnosticHostRegisters[index]));
            if (fieldSize <= 0) break;
            hostBytes += std::min(static_cast<std::size_t>(fieldSize),
                                 hostReport.size() - hostBytes - 1);
        }
        if (hostBytes < hostReport.size() - 1) hostReport[hostBytes++] = '\n';
        (void)::write(STDERR_FILENO, hostReport.data(), hostBytes);
        for (std::size_t index = 0; index != run.diagnosticHostCode.available.size(); ++index) {
            if (!run.diagnosticHostCode.available[index]) continue;
            const int offset = static_cast<int>(index) -
                static_cast<int>(Lumen::MacOS::FexCore::RawHostCodeRadius);
            const auto address = offset < 0
                ? hostPc - static_cast<std::uint64_t>(-offset) * sizeof(std::uint32_t)
                : hostPc + static_cast<std::uint64_t>(offset) * sizeof(std::uint32_t);
            std::array<char, 256> codeReport {};
            const int codeSize = std::snprintf(codeReport.data(), codeReport.size(),
                "[MACOS_FEXCORE_FAULT_HOST_CODE_V1] run=%llu scope=%s rawOnly=1 "
                "offset=%d address=0x%016llx instruction=0x%08x\n",
                static_cast<unsigned long long>(run.runId),
                run.diagnosticGuestRoute ? "guest" : "runtime-probe", offset,
                static_cast<unsigned long long>(address),
                static_cast<unsigned>(run.diagnosticHostCode.instructions[index]));
            if (codeSize > 0) (void)::write(STDERR_FILENO, codeReport.data(), std::min(
                static_cast<std::size_t>(codeSize), codeReport.size() - 1));
        }
        if (run.faultHostCodeExtendedEnabled) {
            const auto& extended = run.diagnosticExtendedHostCode;
            std::array<char, 512> summary {};
            const int summarySize = std::snprintf(summary.data(), summary.size(),
                "[MACOS_FEXCORE_FAULT_HOST_CODE_EXTENDED_V1] run=%llu scope=%s rawOnly=1 "
                "faultInstructionValid=%u maximum_before=512 maximum_after=128 native_page_size=%zu "
                "page_first=0x%016llx page_last=0x%016llx captured=%u before=%u after=%u "
                "page_clamped_before=%u page_clamped_after=%u owner_omitted=%u\n",
                static_cast<unsigned long long>(run.runId),
                run.diagnosticGuestRoute ? "guest" : "runtime-probe",
                static_cast<unsigned>(extended.faultInstructionValid), HostPageSize(),
                static_cast<unsigned long long>(extended.pageFirst),
                static_cast<unsigned long long>(extended.pageLast),
                static_cast<unsigned>(extended.before + extended.after + (extended.faultInstructionValid ? 1 : 0)),
                static_cast<unsigned>(extended.before), static_cast<unsigned>(extended.after),
                static_cast<unsigned>(extended.pageClampedBefore), static_cast<unsigned>(extended.pageClampedAfter),
                static_cast<unsigned>(extended.ownerOmitted));
            if (summarySize > 0) (void)::write(STDERR_FILENO, summary.data(), std::min(
                static_cast<std::size_t>(summarySize), summary.size() - 1));
            for (std::size_t index = 0; index != extended.available.size(); ++index) {
                if (!extended.available[index]) continue;
                const int offset = static_cast<int>(index) -
                    static_cast<int>(Lumen::MacOS::FexCore::RawExtendedHostCodeBefore);
                const auto address = offset < 0
                    ? hostPc - static_cast<std::uint64_t>(-offset) * sizeof(std::uint32_t)
                    : hostPc + static_cast<std::uint64_t>(offset) * sizeof(std::uint32_t);
                std::array<char, 256> word {};
                const int wordSize = std::snprintf(word.data(), word.size(),
                    "[MACOS_FEXCORE_FAULT_HOST_CODE_EXTENDED_WORD_V1] run=%llu scope=%s rawOnly=1 "
                    "offset=%d address=0x%016llx instruction=0x%08x\n",
                    static_cast<unsigned long long>(run.runId),
                    run.diagnosticGuestRoute ? "guest" : "runtime-probe", offset,
                    static_cast<unsigned long long>(address), static_cast<unsigned>(extended.instructions[index]));
                if (wordSize > 0) (void)::write(STDERR_FILENO, word.data(), std::min(
                    static_cast<std::size_t>(wordSize), word.size() - 1));
            }
        }
    }
}

void ReportFaultContextConfiguration() noexcept {
    std::call_once(g_faultContextConfigurationOnce, [] {
        std::array<char, 160> report {};
        const int reportSize = std::snprintf(
            report.data(), report.size(),
            "[MACOS_FEXCORE_FAULT_CONTEXT_CONFIG_V1] "
            "mainImageBase=0x%016llx\n",
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(
                _dyld_get_image_header(0))));
        if (reportSize > 0) {
            const size_t byteCount = std::min(
                static_cast<size_t>(reportSize), report.size() - 1);
            (void)::write(STDERR_FILENO, report.data(), byteCount);
        }
    });
}
#elif defined(__ANDROID__)
[[gnu::noinline]] void ReportAndroidFaultContext(
    const ActiveRun& run) noexcept {
    std::array<char, 512> report {};
    const int reportSize = std::snprintf(
        report.data(), report.size(),
        "[ANDROID_FEXCORE_FAULT_CONTEXT_V1] scope=%s signal=%d "
        "signalCode=%d fault=0x%016llx hostPc=0x%016llx "
        "guestRip=0x%016llx capturedGprsValid=%u inJit=%u "
        "inSyscall=%u activeHostCall=%u target=0x%016llx nativeTid=%llu rawOnly=1\n",
        run.diagnosticGuestRoute ? "guest" : "runtime-probe",
        static_cast<int>(run.diagnosticSignal),
        static_cast<int>(run.diagnosticSignalCode),
        static_cast<unsigned long long>(run.diagnosticFaultAddress),
        static_cast<unsigned long long>(run.diagnosticHostPc),
        static_cast<unsigned long long>(run.diagnosticGuestRip),
        static_cast<unsigned>(run.diagnosticInJit == 0),
        static_cast<unsigned>(run.diagnosticInJit != 0),
        static_cast<unsigned>(run.diagnosticInSyscall != 0),
        static_cast<unsigned>(run.diagnosticActiveHostCall),
        static_cast<unsigned long long>(run.diagnosticHostCallTarget),
        static_cast<unsigned long long>(run.nativeThreadId));
    if (reportSize > 0) {
        const size_t byteCount = std::min(
            static_cast<size_t>(reportSize), report.size() - 1);
        (void)::write(STDERR_FILENO, report.data(), byteCount);
        __android_log_write(ANDROID_LOG_ERROR, "LumenFexCoreRuntime", report.data());
    }

    // Report only after the signal has returned through FEX's stop handler.
    // The static bank may lag intermediate SSA values and mapped RIP may be
    // a cached block location. These native observations are never substituted
    // into the execution state or claimed as precise guest registers.
    if (!run.faultStaticGprsEnabled || !run.diagnosticHostRegistersValid) return;
    const auto emit = [&](const char* format, auto... fields) {
        std::array<char, 1024> line {};
        if (std::snprintf(line.data(), line.size(), format, fields...) > 0)
            __android_log_write(ANDROID_LOG_ERROR, "LumenFexCoreRuntime", line.data());
    };
    const auto nativeTid = static_cast<unsigned long long>(run.nativeThreadId);
    const auto& values = run.diagnosticStaticGprs.values;
    emit("[ANDROID_FEXCORE_FAULT_STATIC_GPRS_V1] nativeTid=%llu rawOnly=1 "
         "staticMapValid=%u count=%u inJit=%u "
         "map=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u "
         "rax=%016llx rcx=%016llx rdx=%016llx rbx=%016llx "
         "rsp=%016llx rbp=%016llx rsi=%016llx rdi=%016llx "
         "r8=%016llx r9=%016llx r10=%016llx r11=%016llx "
         "r12=%016llx r13=%016llx r14=%016llx r15=%016llx",
         nativeTid, static_cast<unsigned>(run.diagnosticStaticGprs.valid),
         static_cast<unsigned>(run.diagnosticStaticGprCount),
         static_cast<unsigned>(run.diagnosticInJit != 0),
         static_cast<unsigned>(run.diagnosticStaticGprMapping[0]), static_cast<unsigned>(run.diagnosticStaticGprMapping[1]),
         static_cast<unsigned>(run.diagnosticStaticGprMapping[2]), static_cast<unsigned>(run.diagnosticStaticGprMapping[3]),
         static_cast<unsigned>(run.diagnosticStaticGprMapping[4]), static_cast<unsigned>(run.diagnosticStaticGprMapping[5]),
         static_cast<unsigned>(run.diagnosticStaticGprMapping[6]), static_cast<unsigned>(run.diagnosticStaticGprMapping[7]),
         static_cast<unsigned>(run.diagnosticStaticGprMapping[8]), static_cast<unsigned>(run.diagnosticStaticGprMapping[9]),
         static_cast<unsigned>(run.diagnosticStaticGprMapping[10]), static_cast<unsigned>(run.diagnosticStaticGprMapping[11]),
         static_cast<unsigned>(run.diagnosticStaticGprMapping[12]), static_cast<unsigned>(run.diagnosticStaticGprMapping[13]),
         static_cast<unsigned>(run.diagnosticStaticGprMapping[14]), static_cast<unsigned>(run.diagnosticStaticGprMapping[15]),
         static_cast<unsigned long long>(values[0]), static_cast<unsigned long long>(values[1]),
         static_cast<unsigned long long>(values[2]), static_cast<unsigned long long>(values[3]),
         static_cast<unsigned long long>(values[4]), static_cast<unsigned long long>(values[5]),
         static_cast<unsigned long long>(values[6]), static_cast<unsigned long long>(values[7]),
         static_cast<unsigned long long>(values[8]), static_cast<unsigned long long>(values[9]),
         static_cast<unsigned long long>(values[10]), static_cast<unsigned long long>(values[11]),
         static_cast<unsigned long long>(values[12]), static_cast<unsigned long long>(values[13]),
         static_cast<unsigned long long>(values[14]), static_cast<unsigned long long>(values[15]));
    emit("[ANDROID_FEXCORE_FAULT_HOST_CONTEXT_V1] nativeTid=%llu rawOnly=1 "
         "hostRegistersValid=1 count=31 hostPc=0x%016llx hostSp=0x%016llx "
         "faultInstructionValid=%u faultInstruction=0x%08x before=%zu after=%zu",
         nativeTid, static_cast<unsigned long long>(run.diagnosticHostPc),
         static_cast<unsigned long long>(run.diagnosticHostSp),
         static_cast<unsigned>(run.diagnosticHostCode.faultInstructionValid),
         run.diagnosticHostCode.instructions[Lumen::MacOS::FexCore::RawAndroidHostCodeSnapshot::BeforeWords],
         Lumen::MacOS::FexCore::RawAndroidHostCodeSnapshot::BeforeWords,
         Lumen::MacOS::FexCore::RawAndroidHostCodeSnapshot::AfterWords);
    const auto& registers = run.diagnosticHostRegisters;
    for (std::size_t first = 0; first < 28; first += 4)
        emit("[ANDROID_FEXCORE_FAULT_HOST_REGISTERS_V1] nativeTid=%llu rawOnly=1 "
             "x%zu=%016llx x%zu=%016llx x%zu=%016llx x%zu=%016llx",
             nativeTid,
             first, static_cast<unsigned long long>(registers[first]),
             first + 1, static_cast<unsigned long long>(registers[first + 1]),
             first + 2, static_cast<unsigned long long>(registers[first + 2]),
             first + 3, static_cast<unsigned long long>(registers[first + 3]));
    emit("[ANDROID_FEXCORE_FAULT_HOST_REGISTERS_V1] nativeTid=%llu rawOnly=1 "
         "x28=%016llx x29=%016llx x30=%016llx sp=%016llx",
         nativeTid, static_cast<unsigned long long>(registers[28]),
         static_cast<unsigned long long>(registers[29]),
         static_cast<unsigned long long>(registers[30]),
         static_cast<unsigned long long>(run.diagnosticHostSp));
    constexpr std::size_t CodeWordsPerRecord = 32;
    for (std::size_t first = 0; first < run.diagnosticHostCode.available.size(); first += CodeWordsPerRecord) {
        const auto count = std::min(CodeWordsPerRecord, run.diagnosticHostCode.available.size() - first);
        const int offset = static_cast<int>(first) -
            static_cast<int>(Lumen::MacOS::FexCore::RawAndroidHostCodeSnapshot::BeforeWords);
        // CaptureRawHostCodeWindow proved complete owned words before reading them.
        // Bit zero describes offsetBegin, including missing words. Compact
        // records avoid logd burst suppression without any later memory reads.
        std::uint32_t availabilityMask {};
        std::array<char, CodeWordsPerRecord * 9 + 1> instructions {};
        for (std::size_t word = 0; word < count; ++word) {
            if (run.diagnosticHostCode.available[first + word])
                availabilityMask |= UINT32_C(1) << word;
            std::snprintf(instructions.data() + word * 9, instructions.size() - word * 9,
                word + 1 == count ? "%08x" : "%08x,", run.diagnosticHostCode.instructions[first + word]);
        }
        emit("[ANDROID_FEXCORE_FAULT_HOST_CODE_BLOCK_V1] nativeTid=%llu rawOnly=1 "
             "offsetBegin=%d count=%zu availabilityMask32=0x%08x instructions=%s",
             nativeTid, offset, count, availabilityMask, instructions.data());
    }
}
#endif

#if defined(__APPLE__)
// Use the same nesting counter as FEX itself.  A separate adapter-local
// counter would let an inner lazy CompileBlock close the Darwin write window
// while an outer initialization/publication operation is still emitting.
using JitWriteScope = FEXCore::Allocator::ScopedJITWriteProtection;
#else
// The pinned Android lab uses an executable RW mapping and its upstream FEX
// checkout deliberately has no Darwin JIT publisher. Keep the shared adapter
// calls source-compatible without imposing the local macOS port on Android.
class JitWriteScope final {
public:
    JitWriteScope() noexcept = default;
    ~JitWriteScope() noexcept {}
};
#endif

decltype(auto) HostContextPc(ucontext_t* context) noexcept {
#if defined(__APPLE__)
    return (context->uc_mcontext->__ss.__pc);
#else
    return (context->uc_mcontext.pc);
#endif
}

decltype(auto) HostContextSp(ucontext_t* context) noexcept {
#if defined(__APPLE__)
    return (context->uc_mcontext->__ss.__sp);
#else
    return (context->uc_mcontext.sp);
#endif
}

std::uint64_t* HostContextRegisters(ucontext_t* context) noexcept {
#if defined(__APPLE__)
    return context->uc_mcontext->__ss.__x;
#else
    return reinterpret_cast<std::uint64_t*>(context->uc_mcontext.regs);
#endif
}

struct ExecutableCodeRange {
    std::uintptr_t begin {};
    std::uintptr_t end {};
};

#if defined(__APPLE__)
constexpr std::size_t kMaximumMainExecutableCodeRanges = 8;

std::size_t CaptureMainExecutableCodeRanges(
    std::array<ExecutableCodeRange,
               kMaximumMainExecutableCodeRanges>* ranges) noexcept {
    if (ranges == nullptr) {
        return 0;
    }
    const auto* header = reinterpret_cast<const mach_header_64*>(
        _dyld_get_image_header(0));
    if (header == nullptr || header->magic != MH_MAGIC_64) {
        return 0;
    }

    const auto* commandBytes = reinterpret_cast<const std::uint8_t*>(
        header + 1);
    std::size_t remaining = header->sizeofcmds;
    std::size_t rangeCount = 0;
    const std::intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    for (std::uint32_t index = 0; index < header->ncmds; ++index) {
        if (remaining < sizeof(load_command)) {
            return 0;
        }
        const auto* command =
            reinterpret_cast<const load_command*>(commandBytes);
        if (command->cmdsize < sizeof(load_command) ||
            command->cmdsize > remaining) {
            return 0;
        }
        if (command->cmd == LC_SEGMENT_64) {
            if (command->cmdsize < sizeof(segment_command_64)) {
                return 0;
            }
            const auto* segment =
                reinterpret_cast<const segment_command_64*>(command);
            const vm_prot_t requiredProtection =
                VM_PROT_READ | VM_PROT_EXECUTE;
            if ((segment->initprot & requiredProtection) ==
                    requiredProtection &&
                segment->vmsize != 0) {
                if (rangeCount == ranges->size()) {
                    return 0;
                }
                const std::uintptr_t begin =
                    static_cast<std::uintptr_t>(segment->vmaddr) +
                    static_cast<std::uintptr_t>(slide);
                if (segment->vmsize > UINTPTR_MAX - begin) {
                    return 0;
                }
                (*ranges)[rangeCount++] = {
                    begin,
                    begin + static_cast<std::uintptr_t>(segment->vmsize),
                };
            }
        }
        commandBytes += command->cmdsize;
        remaining -= command->cmdsize;
    }
    return rangeCount;
}
#elif defined(__ANDROID__)
constexpr std::size_t kMaximumNativeExecutableCodeRanges = 8;

std::size_t CaptureNativeExecutableCodeRanges(
    std::array<ExecutableCodeRange,
               kMaximumNativeExecutableCodeRanges>* ranges,
    std::uintptr_t anchor) noexcept {
    if (ranges == nullptr || anchor == 0) {
        return 0;
    }
    struct Capture {
        std::array<ExecutableCodeRange,
                   kMaximumNativeExecutableCodeRanges>* output;
        std::uintptr_t anchor;
        std::size_t count {};
    } capture {ranges, anchor};
    (void)dl_iterate_phdr([](dl_phdr_info* image, std::size_t,
                            void* opaque) noexcept -> int {
        auto& capture = *static_cast<Capture*>(opaque);
        std::array<ExecutableCodeRange,
                   kMaximumNativeExecutableCodeRanges> executable {};
        std::size_t count = 0;
        bool containsAnchor = false;
        for (std::size_t index = 0; index < image->dlpi_phnum; ++index) {
            const auto& segment = image->dlpi_phdr[index];
            if (segment.p_type != PT_LOAD ||
                (segment.p_flags & (PF_R | PF_X)) != (PF_R | PF_X) ||
                segment.p_memsz == 0) {
                continue;
            }
            const auto imageBase = static_cast<std::uintptr_t>(image->dlpi_addr);
            if (segment.p_vaddr > UINTPTR_MAX - imageBase) {
                return 0;
            }
            const auto begin = imageBase +
                static_cast<std::uintptr_t>(segment.p_vaddr);
            if (segment.p_memsz > UINTPTR_MAX - begin ||
                count == executable.size()) {
                return 0;
            }
            const auto end = begin +
                static_cast<std::uintptr_t>(segment.p_memsz);
            executable[count++] = {begin, end};
            containsAnchor |= capture.anchor >= begin &&
                capture.anchor < end;
        }
        if (!containsAnchor) {
            return 0;
        }
        *capture.output = executable;
        capture.count = count;
        return 1;
    }, &capture);
    return capture.count;
}
#endif

#if defined(__APPLE__)

bool HostContextRegisterValue(const ucontext_t* context,
                              std::uint32_t registerIndex,
                              std::uintptr_t* value) noexcept {
    if (context == nullptr || value == nullptr) {
        return false;
    }
    if (registerIndex < 29) {
        *value = context->uc_mcontext->__ss.__x[registerIndex];
        return true;
    }
    switch (registerIndex) {
    case 29:
        *value = context->uc_mcontext->__ss.__fp;
        return true;
    case 30:
        *value = context->uc_mcontext->__ss.__lr;
        return true;
    case 31:
        *value = context->uc_mcontext->__ss.__sp;
        return true;
    default:
        return false;
    }
}
#endif

thread_local ActiveRun* g_activeRun = nullptr;

#if defined(__ANDROID__) || defined(LUMEN_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED)
bool MemoryHandoffInstrumentationEnabled() noexcept {
    // Provider lookups occur during emission for every memory instruction.
    // Freeze once; never acquire the configuration mutex per emitted access.
    static const bool enabled = Lumen::GuestCpu::FreezeMemoryHandoffRegistration();
    return enabled;
}

std::uint32_t DirectUnwatchedMemoryBulk(std::uint64_t destination, std::uint64_t source,
    std::uint64_t count, std::uint64_t shape) noexcept {
    // Only DF=0, non-atomic REP lowering calls this predicate. An execution
    // reader excludes resource snapshots/guard enrollment until the whole
    // native operation finishes or reaches its existing safepoint. Never
    // publish a watched cumulative range ahead of its individual accesses.
    const auto element = shape & 0xffU;
    const bool copy = (shape & 0x100U) != 0;
    bool allowed = shape == (element | (copy ? 0x100U : 0U)) &&
        (element == 1 || element == 2 || element == 4 || element == 8) &&
        count != 0 && count <= UINT64_MAX / element &&
        GuestMutationBarrier().CurrentThreadOwnsExecutionLease();
    if (allowed) {
        const auto bytes = count * element;
        // Also preserve the final pointer without wrapping. This is stricter
        // than just the last touched byte and denies huge/sign-bit counts.
        allowed = count <= INT64_MAX && destination <= UINT64_MAX - bytes &&
            (!copy || source <= UINT64_MAX - bytes) &&
            !Lumen::GuestCpu::NeedsHandoff(destination, bytes) &&
            // The complete clean ledger is stable while this execution lease
            // lives. It can waive only source READ enrollment; destination
            // WRITE (including source/destination aliases) remains checked.
            (!copy || Lumen::GuestCpu::MemoryHandoffReadGuardDirectory()->load(
                std::memory_order_acquire) == nullptr ||
             !Lumen::GuestCpu::NeedsHandoff(source, bytes,
                Lumen::GuestCpu::MemoryAccessIntent::Read));
    }
    if (FEXLumenGuestMemoryBulkDecisionV1 != nullptr)
        FEXLumenGuestMemoryBulkDecisionV1(destination, source, count, shape, allowed ? 1U : 0U);
    return allowed ? 1U : 0U;
}

// Optional cold test witness. It observes the completed preserving call and
// cannot grant admission or alter the renderer's ownership decision.
#if defined(__ANDROID__)
extern "C" __attribute__((weak)) void
FEXLumenGuestMemoryPreserveAllObservedV1(std::uint32_t result) noexcept;

extern "C" __attribute__((weak)) void
FEXLumenGuestMemoryHandoffDeniedSignalV1(std::uint64_t guestRip) noexcept;
#endif

std::uint32_t DirectMemoryHandoff(std::uint64_t address, std::uint64_t bytes,
    std::uint32_t write, std::uint64_t guestRip) noexcept {
    if (write > 1 || bytes == 0 || bytes > 4096 ||
        address > UINT64_MAX - (bytes - 1) || guestRip == 0) return 0;
    if (const auto* run = g_activeRun; run != nullptr && run->runtimeProbe &&
        run->probeDataBase != 0 && address >= run->probeDataBase &&
        address - run->probeDataBase < run->probeDataSize &&
        bytes <= run->probeDataSize - (address - run->probeDataBase)) return 1;
    // A direct store has conservatively shared read/write intent. In
    // particular, CAS and all other RMW families must publish prior GPU data
    // before reading it, including failed CAS and same-value stores.
    return Lumen::GuestCpu::PrepareCpuMemoryAccess(address, bytes,
        write != 0 ? Lumen::GuestCpu::MemoryAccessIntent::ReadWrite
                   : Lumen::GuestCpu::MemoryAccessIntent::Read) ? 1U : 0U;
}
#if FEX_LUMEN_HAS_MEMORY_PRESERVE_ALL_ABI
FEX_LUMEN_MEMORY_PRESERVE_ALL_ABI __attribute__((noinline))
std::uint32_t DirectMemoryHandoffPreserveAll(std::uint64_t address, std::uint64_t bytes,
    std::uint32_t write, std::uint64_t guestRip) noexcept {
    // The compiler preserves the declared native ABI around this ordinary
    // callback. JIT scratch/status, x18/x30 and SVE upper halves remain covered
    // by the caller's explicit frame and upstream preserving-call helpers.
    const auto result = DirectMemoryHandoff(address, bytes, write, guestRip);
    if (FEXLumenGuestMemoryPreserveAllObservedV1 != nullptr)
        FEXLumenGuestMemoryPreserveAllObservedV1(result);
    return result;
}
#endif
#endif

#if defined(__APPLE__)
std::uint32_t DirectUnalignedTso(std::uint64_t address, std::uint64_t bytes,
    std::uint32_t write, std::uint64_t input, std::uint64_t* output,
    std::uint64_t guestRip) noexcept {
    auto* run = g_activeRun;
    if (run == nullptr || run->thread == nullptr) return 0;
    // This ordinary JIT helper may use clocks and the nested scope ledger;
    // the separate native signal handler retains only lock-free counters.
    std::optional<Lumen::Performance::Scope> timing;
    if (run->performanceTraceEnabled)
        timing.emplace(Lumen::Performance::Phase::DirectUnaligned);
    const auto denied = [&] {
        run->directUnalignedDeniedAddress = address;
        run->directUnalignedDeniedRip = guestRip;
        if (run->performanceTraceEnabled)
            Lumen::Performance::DirectUnaligned(write != 0,
                static_cast<std::uint32_t>(bytes), false);
        return 0U;
    };
    using namespace Lumen::MacOS::FexCore;
    std::uintptr_t first {}, length {};
    if (write > 1 || output == nullptr || guestRip == 0 ||
        bytes > UINT32_MAX || !IsDirectUnalignedScalarAccess(address, bytes) ||
        !ResolveHelperMemoryEnvelope(address, bytes, &first, &length) ||
        !HasGuestDataAccessUnderExecutionBarrier(*run, first, length, false) ||
        (write != 0 && !HasGuestDataAccessUnderExecutionBarrier(*run, first, length, true)))
        return denied();

    // This is the same pinned atomic acquire/release emulation as Darwin's
    // signal route. The validated descriptor supplies its address/value, and
    // no executable memory or guest code is patched. LOCK/RMW instructions
    // retain their original independent lowering and signal route.
    const std::uint32_t instruction = DirectUnalignedScalarInstruction(bytes, write != 0);
    std::array<std::uint64_t, 32> registers {};
    registers[0] = input;
    registers[1] = address;
    const auto handled = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
        run->thread, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier,
        reinterpret_cast<std::uintptr_t>(&instruction), registers.data(), false);
    if (!handled.has_value() || *handled != static_cast<std::int32_t>(sizeof(std::uint32_t))) return denied();
    *output = registers[0];
    if (run->performanceTraceEnabled)
        Lumen::Performance::DirectUnaligned(write != 0,
            static_cast<std::uint32_t>(bytes), true);
    return 1;
}
#endif
thread_local FEXCore::Core::InternalThreadState* g_allocatorThread = nullptr;
constinit std::byte g_hostTargetCustomIrCreatorTag {};

#if defined(__ANDROID__)
bool PrepareExecutableProbeChild() noexcept {
    // ART interposes sigaction/sigprocmask. Its SIGSEGV chain may retain a
    // lock owned by a vanished parent thread after fork. The canary child
    // must only use the kernel ABI, never re-enter application signal chains.
    // Use the NDK kernel type: the POSIX sigaction layout differs on arm64.
    static_assert(sizeof(sigset_t) == 8);
    static_assert(sizeof(__kernel_sigaction) == 32);
    const __kernel_sigaction defaultAction {};
    constexpr std::array<int, 4> kProbeFaultSignals {
        SIGILL, SIGSEGV, SIGBUS, SIGTRAP,
    };
    for (const int signal : kProbeFaultSignals) {
        if (syscall(SYS_rt_sigaction, signal, &defaultAction, nullptr,
                    sizeof(sigset_t)) != 0) {
            return false;
        }
    }

    const sigset_t emptyMask {};
    if (syscall(SYS_rt_sigprocmask, SIG_SETMASK, &emptyMask, nullptr,
                sizeof(emptyMask)) != 0) {
        return false;
    }
    return true;
}

bool WaitForExecutableProbeChild(pid_t child, int* childStatus, int* waitError) noexcept {
    if (child <= 0 || childStatus == nullptr) {
        return false;
    }

    constexpr int kAttempts = 100;
    constexpr timespec kInterval {0, 10 * 1000 * 1000};
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        const pid_t waited = waitpid(child, childStatus, WNOHANG);
        if (waited == child) {
            return true;
        }
        if (waited < 0 && errno != EINTR) {
            *waitError = errno;
            return false;
        }

        timespec remaining = kInterval;
        while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
        }
    }

    // The child executes two fixed AArch64 instructions. A timeout is a
    // failed capability canary, never workload latency. Reap it before the
    // parent can continue toward FEX context creation.
    *waitError = ETIMEDOUT;
    (void)kill(child, SIGKILL);
    pid_t waited = -1;
    do {
        waited = waitpid(child, childStatus, 0);
    } while (waited < 0 && errno == EINTR);
    return false;
}
#endif

class Signals final : public FEXCore::SignalDelegator {
public:
    bool Install() noexcept {
        if (installed_) {
            return true;
        }
        if (instance_ != nullptr && instance_ != this) {
            return false;
        }
#if defined(__APPLE__)
        nativeCodeRangeCount_ = CaptureMainExecutableCodeRanges(
            &nativeCodeRanges_);
        if (nativeCodeRangeCount_ == 0) {
            return false;
        }
#elif defined(__ANDROID__)
        nativeCodeRangeCount_ = CaptureNativeExecutableCodeRanges(
            &nativeCodeRanges_,
            reinterpret_cast<std::uintptr_t>(&Signals::Handler));
        if (nativeCodeRangeCount_ == 0) {
            return false;
        }
#endif

        struct sigaction action {};
        action.sa_sigaction = &Signals::Handler;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);

        instance_ = this;
        std::size_t installedCount = 0;
        for (; installedCount < kSignals.size(); ++installedCount) {
            if (sigaction(kSignals[installedCount], &action,
                          &oldActions_[installedCount]) != 0) {
                break;
            }
        }
        if (installedCount != kSignals.size()) {
            while (installedCount != 0) {
                --installedCount;
                sigaction(kSignals[installedCount],
                          &oldActions_[installedCount], nullptr);
            }
            instance_ = nullptr;
            return false;
        }
        installed_ = true;
        return true;
    }

private:
    static constexpr std::array<int, 4> kSignals {
        SIGSEGV, SIGBUS, SIGILL, SIGTRAP,
    };

    static std::size_t SignalIndex(int signal) noexcept {
        for (std::size_t index = 0; index < kSignals.size(); ++index) {
            if (kSignals[index] == signal) {
                return index;
            }
        }
        return kSignals.size();
    }

    bool StopActiveRun(int signal, siginfo_t* info,
                       void* rawContext) noexcept {
#if defined(__aarch64__)
        auto* run = g_activeRun;
        if (run == nullptr || run->context == nullptr ||
            run->thread == nullptr || rawContext == nullptr) {
            return false;
        }

        auto* frame = run->thread->CurrentFrame;
        auto* context = static_cast<ucontext_t*>(rawContext);
        const std::uint64_t pc = HostContextPc(context);
        const bool inJit =
            run->context->IsAddressInCodeBuffer(run->thread, pc);
        const auto& syncFault = frame->SynchronousFaultData;
        const auto captureDiagnostic = [&](std::uint64_t guestRip,
                DiagnosticGuestSignalRoute guestSignalRoute) noexcept {
            if (!run->faultContextEnabled) return;
            std::uint32_t expected = 0;
            if (!run->diagnosticCaptureState.compare_exchange_strong(
                    expected, 1, std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return;
            }
            run->diagnosticSignal = signal;
            run->diagnosticSignalCode = info != nullptr ? info->si_code : 0;
            run->diagnosticFaultAddress = info != nullptr
                ? reinterpret_cast<std::uintptr_t>(info->si_addr)
                : 0;
            run->diagnosticHostPc = pc;
            run->diagnosticGuestRip = guestRip;
            run->diagnosticR10 = frame->State.gregs[
                FEXCore::X86State::REG_R10];
            run->diagnosticR11 = frame->State.gregs[
                FEXCore::X86State::REG_R11];
            run->diagnosticActiveHostCall = run->activeHostCall;
            run->diagnosticHostCallTarget = run->activeHostCallTarget;
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
            run->diagnosticGuestSignalRoute =
                static_cast<sig_atomic_t>(guestSignalRoute);
            run->diagnosticSyncFaultGenerated =
                syncFault.FaultToTopAndGeneratedException ? 1 : 0;
            run->diagnosticSyncFaultSignal =
                static_cast<sig_atomic_t>(syncFault.Signal);
            const bool generatedSyncFault =
                syncFault.FaultToTopAndGeneratedException;
            run->diagnosticSyncFaultTrapNo = generatedSyncFault
                ? static_cast<sig_atomic_t>(syncFault.TrapNo) : 0;
            run->diagnosticSyncFaultSiCode = generatedSyncFault
                ? static_cast<sig_atomic_t>(syncFault.si_code) : 0;
            run->diagnosticSyncFaultErrorCode = generatedSyncFault
                ? static_cast<sig_atomic_t>(syncFault.err_code) : 0;
            run->diagnosticGuestCsIndex =
                static_cast<sig_atomic_t>(frame->State.cs_idx);
            run->diagnosticGuestIoplBit12 = static_cast<sig_atomic_t>(
                frame->State.flags[FEXCore::X86State::RFLAG_IOPL_LOC] & 1U);
#endif
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD) || defined(__ANDROID__)
            const bool captureNativeRegisters = run->faultStaticGprsEnabled
#if !defined(__ANDROID__)
                && inJit
#endif
                ;
            if (captureNativeRegisters) {
                const auto& config = GetConfig();
                run->diagnosticStaticGprCount = config.SRAGPRCount;
                run->diagnosticStaticGprMapping = config.SRAGPRMapping;
                std::array<std::uint64_t, 31> hostRegisters {};
#if defined(__APPLE__)
                std::copy_n(context->uc_mcontext->__ss.__x, 29,
                            hostRegisters.begin());
                hostRegisters[29] = context->uc_mcontext->__ss.__fp;
                hostRegisters[30] = context->uc_mcontext->__ss.__lr;
#else
                std::copy_n(HostContextRegisters(context), hostRegisters.size(),
                            hostRegisters.begin());
#endif
                run->diagnosticHostRegisters = hostRegisters;
                run->diagnosticHostRegistersValid = true;
                run->diagnosticHostSp = HostContextSp(context);
                run->diagnosticHostCode =
#if defined(__ANDROID__)
                    Lumen::MacOS::FexCore::CaptureRawHostCodeWindow<
                        Lumen::MacOS::FexCore::RawAndroidHostCodeSnapshot::BeforeWords,
                        Lumen::MacOS::FexCore::RawAndroidHostCodeSnapshot::AfterWords>(pc,
#else
                    Lumen::MacOS::FexCore::CaptureRawHostCode(pc,
#endif
                    [&](std::uint64_t address) noexcept {
                        return run->context->IsAddressInCodeBuffer(run->thread, address);
                    }, [](std::uint64_t address) noexcept {
                        return *reinterpret_cast<const volatile std::uint32_t*>(address);
                    });
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
                if (run->faultHostCodeExtendedEnabled) {
                    run->diagnosticExtendedHostCode = Lumen::MacOS::FexCore::CaptureRawExtendedHostCode(
                        true, pc, HostPageSize(),
                        [&](std::uint64_t address) noexcept {
                            return run->context->IsAddressInCodeBuffer(run->thread, address);
                        }, [](std::uint64_t address) noexcept {
                            return *reinterpret_cast<const volatile std::uint32_t*>(address);
                        });
                }
#endif
                run->diagnosticStaticGprs = Lumen::MacOS::FexCore::CaptureRawStaticGprs(
                    config.SRAGPRCount, config.SRAGPRMapping, hostRegisters);
            }
#endif
            run->diagnosticInJit = inJit ? 1 : 0;
            run->diagnosticInSyscall =
                frame->InSyscallInfo != 0 ? 1 : 0;
            run->diagnosticCaptureState.store(
                2, std::memory_order_release);
        };

        const std::uint64_t safepointEpoch =
            run->safepointEpoch.load(std::memory_order_acquire);
        const bool exactDoorbellAddress =
            info != nullptr && info->si_addr == static_cast<void*>(
                run->thread->InterruptFaultPage);
#if defined(__APPLE__)
        // Darwin may surface a write to the read-only doorbell as any of the
        // protection codes below. Every admitted spelling must still be the
        // exact 64-bit zero store targeting that page. This covers x28-relative
        // JIT/dispatcher probes and native DeferredSignalRefCountGuard stores
        // without turning the private address alone into an admission token.
        const bool establishedProtectionFault = info != nullptr &&
            ((signal == SIGSEGV && info->si_code == SEGV_ACCERR) ||
             (signal == SIGBUS && info->si_code == BUS_ADRERR));
        const bool busAlignmentFault = info != nullptr &&
            signal == SIGBUS && info->si_code == BUS_ADRALN;
        const bool supportedDoorbellSignal =
            establishedProtectionFault || busAlignmentFault;
        const bool readableJitInstruction = inJit &&
            pc <= UINT64_MAX - (sizeof(std::uint32_t) - 1) &&
            run->context->IsAddressInCodeBuffer(
                run->thread, pc + sizeof(std::uint32_t) - 1);
        const auto& signalConfig = GetConfig();
        const bool readableDispatcherInstruction =
            pc >= signalConfig.DispatcherBegin &&
            pc < signalConfig.DispatcherEnd &&
            signalConfig.DispatcherEnd - pc >= sizeof(std::uint32_t);
        bool readableNativeInstruction = false;
        for (std::size_t index = 0; index < nativeCodeRangeCount_; ++index) {
            const auto& range = nativeCodeRanges_[index];
            if (pc >= range.begin && pc < range.end &&
                range.end - pc >= sizeof(std::uint32_t)) {
                readableNativeInstruction = true;
                break;
            }
        }
        const bool instructionAvailable = exactDoorbellAddress &&
            supportedDoorbellSignal &&
            pc % alignof(std::uint32_t) == 0 &&
            (readableJitInstruction || readableDispatcherInstruction ||
             readableNativeInstruction);
        // The admitted native __TEXT and generated MAP_JIT ranges retain read
        // access. This aligned volatile load is one signal-safe operation;
        // unknown or execute-only PCs fail closed without being dereferenced.
        const std::uint32_t faultingInstruction = instructionAvailable
            ? *reinterpret_cast<const volatile std::uint32_t*>(pc)
            : 0;
        const auto decodedStore =
            Lumen::MacOS::FexCore::DecodeStoreZeroUnsignedImmediate(
                faultingInstruction);
        std::uintptr_t storeBase = 0;
        const bool storeBaseAvailable = instructionAvailable &&
            decodedStore.valid && HostContextRegisterValue(
                context, decodedStore.baseRegister, &storeBase);
        const bool storeZeroTargetsDoorbell = storeBaseAvailable &&
            Lumen::MacOS::FexCore::StoreZeroTargetsAddress(
                faultingInstruction, storeBase,
                reinterpret_cast<std::uintptr_t>(
                    run->thread->InterruptFaultPage));
        const bool exactDoorbellFault =
            Lumen::MacOS::FexCore::ShouldHandleDoorbellFault(
                exactDoorbellAddress, establishedProtectionFault,
                busAlignmentFault, storeZeroTargetsDoorbell);
#else
        // Some Android vendor kernels report a write to an mprotect-ed
        // read-only anonymous page as SEGV_MAPERR instead of SEGV_ACCERR.
        // The generated-code route requires the exact x28-relative doorbell
        // store emitted by FEX and an effective address on this thread's
        // page. This covers block-entry and backedge probes; the native
        // guard route below has its own executable-range and opcode checks.
        const bool establishedDoorbellFault = exactDoorbellAddress &&
            signal == SIGSEGV && info->si_code == SEGV_ACCERR;
        const bool readableJitInstruction = inJit &&
            pc % alignof(std::uint32_t) == 0 &&
            pc <= UINT64_MAX - (sizeof(std::uint32_t) - 1) &&
            run->context->IsAddressInCodeBuffer(
                run->thread, pc + sizeof(std::uint32_t) - 1);
        const bool vendorMapErrorCandidate = exactDoorbellAddress &&
            signal == SIGSEGV && info->si_code == SEGV_MAPERR &&
            readableJitInstruction;
        std::uint32_t faultingInstruction = 0;
        if (vendorMapErrorCandidate) {
            std::memcpy(&faultingInstruction,
                        reinterpret_cast<const void*>(pc),
                        sizeof(faultingInstruction));
        }
        const bool exactInterruptDoorbellStore =
            vendorMapErrorCandidate &&
            faultingInstruction == kInterruptDoorbellStore;
        const bool storeTargetsDoorbell = exactInterruptDoorbellStore &&
            Lumen::MacOS::FexCore::StoreZeroTargetsAddress(
                faultingInstruction,
                static_cast<std::uintptr_t>(
                    HostContextRegisters(context)[28]),
                reinterpret_cast<std::uintptr_t>(
                    run->thread->InterruptFaultPage));
        const auto vendorMapErrorDisposition =
            Lumen::Android::FexCoreLab::
                ClassifyVendorMapErrorDoorbellFault(
                    safepointEpoch, exactDoorbellAddress,
                    signal == SIGSEGV && info->si_code == SEGV_MAPERR,
                    inJit, readableJitInstruction,
                    exactInterruptDoorbellStore, storeTargetsDoorbell);
        const bool vendorMapErrorDoorbellFault =
            vendorMapErrorDisposition !=
            Lumen::Android::FexCoreLab::
                VendorMapErrorDoorbellDisposition::Reject;
        // The reviewed host-page overlay emits the same exact zero store in
        // the dispatcher after its deferred-signal guard. The dispatcher is
        // outside IsAddressInCodeBuffer; qualify its own captured RX interval
        // before reading the opcode. It remains a probe, not a JIT safepoint.
        const auto& signalConfig = GetConfig();
        const bool readableDispatcherInstruction = !inJit &&
            pc % alignof(std::uint32_t) == 0 &&
            pc >= signalConfig.DispatcherBegin &&
            pc < signalConfig.DispatcherEnd &&
            signalConfig.DispatcherEnd - pc >= sizeof(std::uint32_t);
        const bool dispatcherMapErrorCandidate = exactDoorbellAddress &&
            signal == SIGSEGV && info->si_code == SEGV_MAPERR &&
            readableDispatcherInstruction;
        std::uint32_t dispatcherInstruction = 0;
        if (dispatcherMapErrorCandidate) {
            std::memcpy(&dispatcherInstruction,
                        reinterpret_cast<const void*>(pc),
                        sizeof(dispatcherInstruction));
        }
        const bool dispatcherMapErrorDoorbellFault =
            Lumen::Android::FexCoreLab::IsDispatcherMapErrorDoorbellFault(
                exactDoorbellAddress,
                signal == SIGSEGV && info != nullptr && info->si_code == SEGV_MAPERR,
                readableDispatcherInstruction,
                dispatcherInstruction == kInterruptDoorbellStore,
                Lumen::MacOS::FexCore::StoreZeroTargetsAddress(
                    dispatcherInstruction,
                    static_cast<std::uintptr_t>(HostContextRegisters(context)[28]),
                    reinterpret_cast<std::uintptr_t>(run->thread->InterruptFaultPage)));
        // FEX's native DeferredSignalRefCountGuard also writes a zero to the
        // interrupt page. On this kernel the protected write can be MAPERR;
        // a JIT-only check rejects that legitimate pending-interrupt probe.
        // Admit native code only from this module's captured readable RX
        // segments, and only when the actual instruction and host register
        // compute this thread's exact interrupt-page address.
        bool readableNativeInstruction = false;
        if (!inJit && pc % alignof(std::uint32_t) == 0) {
            for (std::size_t index = 0; index < nativeCodeRangeCount_;
                 ++index) {
                const auto& range = nativeCodeRanges_[index];
                if (pc >= range.begin && pc < range.end &&
                    range.end - pc >= sizeof(std::uint32_t)) {
                    readableNativeInstruction = true;
                    break;
                }
            }
        }
        const bool nativeMapErrorCandidate = exactDoorbellAddress &&
            signal == SIGSEGV && info->si_code == SEGV_MAPERR &&
            readableNativeInstruction;
        std::uint32_t nativeInstruction = 0;
        if (nativeMapErrorCandidate) {
            std::memcpy(&nativeInstruction,
                        reinterpret_cast<const void*>(pc),
                        sizeof(nativeInstruction));
        }
        const auto decodedNativeStore =
            Lumen::MacOS::FexCore::DecodeStoreZeroUnsignedImmediate(
                nativeInstruction);
        const bool nativeMapErrorDoorbellFault =
            nativeMapErrorCandidate && decodedNativeStore.valid &&
            decodedNativeStore.baseRegister < 31 &&
            Lumen::MacOS::FexCore::StoreZeroTargetsAddress(
                nativeInstruction,
                static_cast<std::uintptr_t>(HostContextRegisters(
                    context)[decodedNativeStore.baseRegister]),
                reinterpret_cast<std::uintptr_t>(
                    run->thread->InterruptFaultPage));
        const bool exactDoorbellFault = establishedDoorbellFault ||
            vendorMapErrorDoorbellFault || dispatcherMapErrorDoorbellFault ||
            nativeMapErrorDoorbellFault;
#endif
        if (exactDoorbellFault) {
            const bool boundaryInterruptArmed =
                run->boundaryInterruptToken.load(
                    std::memory_order_acquire) != 0;
            if (safepointEpoch == 0 && !boundaryInterruptArmed) {
                // Disarm makes the page writable before publishing epoch zero.
                // A late delivery must only retry the original probe: changing
                // protection here could race a newer Arm and silently disarm
                // that epoch after it has made the page read-only. A pending
                // boundary interrupt keeps the page protected on purpose, so
                // it still routes through the pause path below.
                return true;
            }
            if (!inJit) {
                // InterruptFaultPage is also touched by FEX's deferred-signal
                // guards in dispatcher/C++ code. The store is only a pending
                // interrupt probe; skip it and keep the page armed until this
                // execution reaches a real generated-code boundary.
                HostContextPc(context) += sizeof(std::uint32_t);
                return true;
            }
            if (run->safepointInFlight != 0 ||
                frame->InSyscallInfo != 0) {
                return false;
            }

            // RestoreRIPFromHostPC only owns the currently published inline
            // unit. Its fallback is State.rip, which can still name a completed
            // SYSCALL. Never use that fallback for an unrelated JIT address.
            const auto inlineUnit = CaptureInlineJitUnit(*run);
            if (!inlineUnit.Contains(pc)) {
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
                if (run->faultContextEnabled) {
                    run->diagnosticSafepointHostPc = pc;
                    run->diagnosticSafepointCodeWitness = {inlineUnit, frame->State.rip};
                    run->diagnosticSafepointRejected = 1;
                }
#endif
                // This is our proven armed JIT doorbell, but its state cannot
                // be reconstructed. Stop at FEX's existing spill/return site;
                // chaining an inherited signal handler could resume it with
                // an ambiguous RIP or retry the still-protected store.
                captureDiagnostic(0, DiagnosticGuestSignalRoute::None);
                run->failure = LUMEN_ANDROID_FEXCORE_RUNTIME_THREAD_SETUP_FAILED_V1;
                run->faultSignal = signal;
                run->faultAddress = info != nullptr ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0;
                frame->SignalHandlerRefCounter = 0;
                HostContextSp(context) = frame->ReturningStackLocation;
                HostContextRegisters(context)[28] = reinterpret_cast<std::uintptr_t>(frame);
                HostContextPc(context) = GetConfig().ThreadStopHandlerAddressSpillSRA;
                run->forcedStop = 0;
                return true;
            }

            // A local backward edge follows already-completed guest work.
            // An entry can instead be followed by a linked B into another
            // unit; that branch has not executed and the entry remains the
            // correct boundary. Reconstruct before spilling the static regs.
            std::uint64_t resumeHostPc = pc;
            std::uint32_t followingInstruction = 0;
            if (pc <= UINT64_MAX - 2 * sizeof(std::uint32_t) &&
                inlineUnit.Contains(pc + sizeof(std::uint32_t))) {
                std::memcpy(&followingInstruction,
                            reinterpret_cast<const void*>(
                                pc + sizeof(std::uint32_t)),
                            sizeof(followingInstruction));
                if ((followingInstruction & UINT32_C(0xfc000000)) ==
                    UINT32_C(0x14000000)) {
                    const std::int32_t signedImmediate =
                        static_cast<std::int32_t>(
                            followingInstruction << 6) >> 6;
                    const std::uint64_t branchPc =
                        pc + sizeof(std::uint32_t);
                    const std::uint64_t branchTarget = branchPc +
                        static_cast<std::int64_t>(signedImmediate) * 4;
                    if (branchTarget < pc && inlineUnit.Contains(branchTarget)) {
                        // A backedge check follows already-completed guest
                        // work. Resume from its branch target, not the final
                        // source instruction, or effects could repeat.
                        resumeHostPc = branchTarget;
                    }
                }
            }
            const std::uint64_t resumeGuestRip =
                run->context->RestoreRIPFromHostPC(
                    run->thread, resumeHostPc);
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
            if (run->faultContextEnabled) {
                run->diagnosticSafepointHostPc = pc;
                run->diagnosticSafepointResumeHostPc = resumeHostPc;
                if (run->diagnosticSafepointCount < 64) {
                    SafepointCodeWitness witness;
                    witness.unit = inlineUnit;
                    witness.stateRip = frame->State.rip;
                    witness.followingInstruction = followingInstruction;
                    if (witness.unit.valid) {
                        witness.pcGuestRip = run->context->RestoreRIPFromHostPC(run->thread, pc);
                    }
                    if ((followingInstruction & UINT32_C(0xfc000000)) == UINT32_C(0x14000000)) {
                        const auto immediate = static_cast<std::int32_t>(followingInstruction << 6) >> 6;
                        witness.branchTarget = pc + sizeof(std::uint32_t) + static_cast<std::int64_t>(immediate) * 4;
                        witness.branchTargetInBuffer = run->context->IsAddressInCodeBuffer(run->thread, witness.branchTarget);
                        if (witness.unit.valid && witness.branchTargetInBuffer) {
                            witness.branchGuestRip = run->context->RestoreRIPFromHostPC(run->thread, witness.branchTarget);
                        }
                    }
                    run->diagnosticSafepointCodeWitness = witness;
                }
            }
#endif
            run->safepointResumeRip.store(
                resumeGuestRip, std::memory_order_relaxed);
            frame->State.rip = resumeGuestRip;
            if (mprotect(run->thread->InterruptFaultPage,
                         sizeof(run->thread->InterruptFaultPage),
                         PROT_READ | PROT_WRITE) != 0) {
                _exit(125);
            }
#if defined(__ANDROID__)
            g_lastAndroidSafepointSignalCode.store(
                static_cast<std::uint32_t>(info->si_code),
                std::memory_order_relaxed);
#endif
            run->safepointInFlight = 1;
            HostContextSp(context) = frame->ReturningStackLocation;
            HostContextRegisters(context)[28] =
                reinterpret_cast<std::uintptr_t>(frame);
            HostContextPc(context) =
                GetConfig().ThreadPauseHandlerAddressSpillSRA;
            return true;
        }

        const auto pauseReturn = Lumen::MacOS::FexCore::ClassifyBoundaryPauseReturn(
            signal == SIGILL, run->safepointInFlight != 0,
            frame->Thread == run->thread, pc, GetConfig().PauseReturnInstruction,
            run->failure != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1);
        if (pauseReturn != Lumen::MacOS::FexCore::BoundaryPauseReturnDisposition::Reject) {
            // The peer has been released only after guest mappings and both
            // shared/per-thread lookup caches were published. Abandon the old
            // JIT frame and dispatch from the reconstructed guest RIP so stale
            // translated code is never resumed.
            frame->State.rip = run->safepointResumeRip.load(
                std::memory_order_relaxed);
            HostContextSp(context) = frame->ReturningStackLocation;
            HostContextRegisters(context)[28] =
                reinterpret_cast<std::uintptr_t>(frame);
            HostContextRegisters(context)[1] = 0;
            // The pause stub already spilled the guest state. A failed run
            // stops at this exact existing return site without a native
            // SIGTRAP route (SleepThread is not an InSyscallInfo boundary).
            if (pauseReturn == Lumen::MacOS::FexCore::BoundaryPauseReturnDisposition::Stop)
                frame->SignalHandlerRefCounter = 0;
            HostContextPc(context) = pauseReturn == Lumen::MacOS::FexCore::BoundaryPauseReturnDisposition::Stop
                ? GetConfig().ThreadStopHandlerAddress
                : GetConfig().AbsoluteLoopTopAddressFillSRA;
            run->safepointInFlight = 0;
            return true;
        }

        bool dataAlignmentFault = signal == SIGBUS && inJit &&
            info != nullptr && info->si_code == BUS_ADRALN;
#if defined(__APPLE__)
        std::uint32_t alignmentFamily {}, alignmentWidth {};
        // Darwin also reports permission faults as BUS_ADRALN. In particular,
        // a release store to an aligned read-only page has ESR 0x9200004f;
        // sending it to the alignment helper can spin forever on a failed
        // exclusive store. Only the data-abort alignment syndrome is eligible.
        dataAlignmentFault = dataAlignmentFault &&
            Lumen::MacOS::FexCore::IsDataAlignmentFault(context->uc_mcontext->__es.__esr);
        if (dataAlignmentFault) {
            const bool instructionAvailable = pc % alignof(std::uint32_t) == 0 &&
                pc <= UINT64_MAX - sizeof(std::uint32_t) &&
                run->context->IsAddressInCodeBuffer(run->thread, pc + sizeof(std::uint32_t) - 1);
            const auto instruction = instructionAvailable
                ? *reinterpret_cast<const volatile std::uint32_t*>(pc) : 0U;
            const auto access = instructionAvailable
                ? Lumen::MacOS::FexCore::DecodeMemoryAccess(instruction)
                : Lumen::MacOS::FexCore::MemoryAccess {};
            alignmentWidth = access.valid ? access.width : 0;
            alignmentFamily = !access.valid ? 0 :
                ((instruction & 0x3ffffc00U) == 0x08dffc00U ||
                 (instruction & 0x3ffffc00U) == 0x089ffc00U) ? 1 :
                ((instruction & 0x3ffffc00U) == 0x38bfc000U ||
                 (instruction & 0x3fe00c00U) == 0x19400000U ||
                 (instruction & 0x3fe00c00U) == 0x19000000U) ? 2 : 3;
            if (run->performanceTraceEnabled)
                Lumen::Performance::AlignmentFault(false, alignmentFamily, alignmentWidth);
            std::uintptr_t base = 0;
            std::uintptr_t effective = 0;
            bool permitted = false;
            if (access.valid && access.width != 0 &&
                HostContextRegisterValue(context, access.baseRegister, &base) &&
                Lumen::MacOS::FexCore::ResolveMemoryAccessAddress(access, base, &effective)) {
                // The pinned helper reads or read-modify-writes aligned
                // granules around the architectural operand (up to two
                // 16-byte granules). Prove its complete byte footprint and
                // read permission too, including for emulated stores.
                std::uintptr_t first {}, length {};
                if (Lumen::MacOS::FexCore::ResolveHelperMemoryEnvelope(
                        effective, access.width, &first, &length)) {
                    permitted = HasGuestDataAccessUnderExecutionBarrier(
                        *run, first, length, false) &&
                        (!access.write || HasGuestDataAccessUnderExecutionBarrier(
                            *run, first, length, true));
                }
            }
            dataAlignmentFault = permitted;
        }
#endif
        if (dataAlignmentFault) {
#if defined(__APPLE__)
            // Darwin exposes x0..x28 in __x and stores fp/lr separately,
            // while FEX expects one contiguous x0..x30 array. Normalize the
            // signal context and select FEX's non-JIT path: it emulates the
            // single atomic instruction without backpatching MAP_JIT memory
            // from signal context.
            std::array<std::uint64_t, 32> registers {};
            std::copy_n(context->uc_mcontext->__ss.__x, 29,
                        registers.begin());
            registers[29] = context->uc_mcontext->__ss.__fp;
            registers[30] = context->uc_mcontext->__ss.__lr;
            registers[31] = context->uc_mcontext->__ss.__sp;
            const auto handled =
                FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
                    run->thread,
                    FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::
                        HalfBarrier,
                    pc, registers.data(), false);
            if (handled.has_value()) {
                if (run->performanceTraceEnabled)
                    Lumen::Performance::AlignmentFault(true, alignmentFamily, alignmentWidth);
                std::copy_n(registers.begin(), 29,
                            context->uc_mcontext->__ss.__x);
                context->uc_mcontext->__ss.__fp = registers[29];
                context->uc_mcontext->__ss.__lr = registers[30];
                // Slot 31 supplies SP for address operands, but a helper can
                // also write it as an XZR result (for example CASP). None of
                // the admitted instructions has address writeback; preserve SP.
                HostContextPc(context) =
                    pc + static_cast<std::int64_t>(*handled);
                return true;
            }
#else
            // Android's AArch64 mcontext exposes x0..x30 as one contiguous
            // array.  FEX updates those host registers while emulating or
            // backpatching the faulting unaligned atomic instruction.
            const auto handled =
                FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
                    run->thread,
                    FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::
                        HalfBarrier,
                    pc,
                    HostContextRegisters(context));
            if (handled.has_value()) {
                HostContextPc(context) =
                    pc + static_cast<std::int64_t>(*handled);
                return true;
            }
#endif
        }
        // Guest-signal entrypoints spill the static register set before the
        // trapping HLT/BRK/load.  The kernel therefore reports a PC inside
        // the contiguous dispatcher stub, not at its first instruction.
        const bool inGuestSigIll =
            pc >= frame->Pointers.GuestSignal_SIGILL &&
            pc < frame->Pointers.GuestSignal_SIGTRAP;
        const bool inGuestSigTrap =
            pc >= frame->Pointers.GuestSignal_SIGTRAP &&
            pc < frame->Pointers.GuestSignal_SIGSEGV;
        const bool inGuestSigSegv =
            pc >= frame->Pointers.GuestSignal_SIGSEGV &&
            pc < GetConfig().ThreadPauseHandlerAddressSpillSRA;
        const bool spilledGuestTrap =
            inGuestSigIll || inGuestSigTrap || inGuestSigSegv;
        const bool forcedStop =
            run->forcedStop != 0 && signal == SIGTRAP &&
            frame->InSyscallInfo != 0;
        if (!forcedStop && !inJit && !spilledGuestTrap) {
            // A native HLE executes outside the JIT. Let FEX's existing
            // handler decide whether the signal represents a guest fault, but
            // retain the native boundary that would otherwise be lost when
            // execution returns at the synthetic syscall bridge.
            if (run->activeHostCall != 0) {
                captureDiagnostic(frame->State.rip,
                    DiagnosticGuestSignalRoute::None);
            }
            return false;
        }

        const auto guestSignalRoute = inGuestSigIll
            ? DiagnosticGuestSignalRoute::Ill
            : inGuestSigTrap
                ? DiagnosticGuestSignalRoute::Trap
                : inGuestSigSegv
                    ? DiagnosticGuestSignalRoute::Segv
                    : DiagnosticGuestSignalRoute::None;
        const bool gracefulRootReturn =
            signal == SIGSEGV && inGuestSigSegv &&
            syncFault.FaultToTopAndGeneratedException &&
            syncFault.Signal == FEXCore::Core::FAULT_SIGSEGV &&
            frame->State.rip == run->exitAddress;
#if defined(__APPLE__)
        const bool directUnalignedDenied = signal == SIGTRAP && inJit &&
            run->directUnalignedDeniedRip != 0 &&
            pc % alignof(std::uint32_t) == 0 &&
            pc <= UINTPTR_MAX - sizeof(std::uint32_t) &&
            run->context->IsAddressInCodeBuffer(run->thread, pc + sizeof(std::uint32_t) - 1) &&
            *reinterpret_cast<const volatile std::uint32_t*>(pc) ==
                Lumen::MacOS::FexCore::DirectUnalignedDeniedInstruction;
        if (directUnalignedDenied) frame->State.rip = run->directUnalignedDeniedRip;
#else
        constexpr bool directUnalignedDenied = false;
#endif
        if (!gracefulRootReturn) {
            const auto diagnosticGuestRip = directUnalignedDenied ? frame->State.rip : inJit
                ? run->context->RestoreRIPFromHostPC(run->thread, pc)
                : frame->State.rip;
#if defined(__ANDROID__) || defined(LUMEN_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED)
            // Cold optional signal witness for the existing inline handoff
            // denial BRK. It only observes the actual reconstructed RIP.
            constexpr std::uint32_t handoffDeniedBrk = UINT32_C(0xd4200000) | (UINT32_C(0x4c4d) << 5);
            if (signal == SIGTRAP && inJit && FEXLumenGuestMemoryHandoffDeniedSignalV1 != nullptr &&
                pc % alignof(std::uint32_t) == 0 && pc <= UINTPTR_MAX - sizeof(std::uint32_t) &&
                run->context->IsAddressInCodeBuffer(run->thread, pc + sizeof(std::uint32_t) - 1) &&
                *reinterpret_cast<const volatile std::uint32_t*>(pc) == handoffDeniedBrk)
                FEXLumenGuestMemoryHandoffDeniedSignalV1(diagnosticGuestRip);
#endif
            captureDiagnostic(diagnosticGuestRip, guestSignalRoute);
            if (run->failure ==
                LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) {
                run->failure =
                    LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1;
            }
#if defined(__APPLE__)
            run->faultSignal = directUnalignedDenied ? SIGSEGV : signal;
            run->faultAddress = directUnalignedDenied ? run->directUnalignedDeniedAddress : info != nullptr
                ? reinterpret_cast<std::uintptr_t>(info->si_addr)
                : 0;
#else
            run->faultSignal = signal;
            run->faultAddress = info != nullptr
                ? reinterpret_cast<std::uintptr_t>(info->si_addr)
                : 0;
#endif
        }

        frame->SignalHandlerRefCounter = 0;
        HostContextSp(context) = frame->ReturningStackLocation;
        HostContextRegisters(context)[28] =
            reinterpret_cast<std::uintptr_t>(frame);
        HostContextPc(context) = inJit && !forcedStop
            ? GetConfig().ThreadStopHandlerAddressSpillSRA
            : GetConfig().ThreadStopHandlerAddress;
        run->forcedStop = 0;
        return true;
#else
        (void)signal;
        (void)info;
        (void)rawContext;
        return false;
#endif
    }

    void Chain(int signal, siginfo_t* info, void* rawContext) noexcept {
        const std::size_t index = SignalIndex(signal);
        if (index >= oldActions_.size()) {
            return;
        }
        const auto& oldAction = oldActions_[index];
        if (oldAction.sa_handler == SIG_IGN) {
            return;
        }
        if (oldAction.sa_handler == SIG_DFL) {
            sigaction(signal, &oldAction, nullptr);
            return;
        }
        if ((oldAction.sa_flags & SA_SIGINFO) != 0 &&
            oldAction.sa_sigaction != nullptr) {
            oldAction.sa_sigaction(signal, info, rawContext);
        } else if (oldAction.sa_handler != nullptr) {
            oldAction.sa_handler(signal);
        }
    }

    static void Handler(int signal, siginfo_t* info,
                        void* rawContext) noexcept {
        auto* instance = instance_;
        if (instance != nullptr &&
            instance->StopActiveRun(signal, info, rawContext)) {
            return;
        }
        if (instance != nullptr) {
            instance->Chain(signal, info, rawContext);
        }
    }

    std::array<struct sigaction, kSignals.size()> oldActions_ {};
#if defined(__APPLE__) || defined(__ANDROID__)
    std::array<ExecutableCodeRange,
#if defined(__APPLE__)
               kMaximumMainExecutableCodeRanges> nativeCodeRanges_ {};
#else
               kMaximumNativeExecutableCodeRanges> nativeCodeRanges_ {};
#endif
    std::size_t nativeCodeRangeCount_ {};
#endif
    bool installed_ {};
    static inline Signals* instance_ {};
};

[[noreturn]] void ForceStopActiveRunFromSyscall() noexcept;
void InvalidateActiveGuestCodeRange(
    FEXCore::Core::InternalThreadState* thread,
    std::uint64_t start, std::uint64_t length);
bool ArmActiveGuestSafepoints(std::uint64_t epoch) noexcept;
void DisarmActiveGuestSafepoints(std::uint64_t epoch) noexcept;
std::uint32_t InvokeHostCall(ActiveRun& run, std::uint64_t target,
    FEXCore::Core::CPUState& state, std::uint64_t* nextRip) noexcept;
bool InvokeBoundaryDelivery(ActiveRun& run, LumenAndroidFexCoreBoundaryDeliveryV1 delivery,
    const std::uint64_t* gprs, std::uint64_t rip, std::uint32_t& disposition) noexcept;
void RestoreBoundaryInterruptProtection(ActiveRun& run) noexcept;
bool FreshCallbackParentValid(ActiveRun& run, bool captureReturn) noexcept;
bool FreshCallbackChildEntryValid(ActiveRun& run,
    const LumenAndroidFexCoreGuestStateV1& guest) noexcept;
bool FinishFreshCallbackHostCall(ActiveRun& run) noexcept;
void ReportGuestBarrierFailure(const char* site, const ActiveRun& run,
    const GuestMemoryMutationBarrier::BarrierFailureDiagnostic& diagnostic,
    std::uint64_t hostTarget = 0, std::uint64_t guestRip = 0) noexcept;

class Syscalls final : public FEXCore::HLE::SyscallHandler {
public:
    Syscalls() {
        OSABI = FEXCore::HLE::SyscallOSABI::OS_GENERIC;
    }

    void SetInternalExecutableRange(std::uintptr_t base,
                                    std::size_t size) noexcept {
        internalBase_ = base;
        internalSize_ = size;
    }

    void SetExitAddress(std::uint64_t address) noexcept {
        exitAddress_ = address;
    }

    std::uint64_t ExitAddress() const noexcept {
        return exitAddress_;
    }

    std::uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame* frame,
                                FEXCore::HLE::SyscallArguments*) override {
        auto* run = g_activeRun;
        if (run == nullptr || frame == nullptr) {
            return 0;
        }

        auto& state = frame->State;
        const std::uint64_t bridge = internalBase_ + kBridgeOffset;
        const std::uint64_t target =
            state.gregs[FEXCore::X86State::REG_R10];
        if (state.rip != bridge + kBridgeSyscallOffset) {
            run->failure =
                LUMEN_ANDROID_FEXCORE_RUNTIME_SYSCALL_DENIED_V1;
            ForceStopActiveRunFromSyscall();
        }
        if (!run->hostTargets || !run->hostTargets->Contains(target)) {
            run->failure =
                LUMEN_ANDROID_FEXCORE_RUNTIME_SYSCALL_DENIED_V1;
            state.gregs[FEXCore::X86State::REG_R11] = exitAddress_;
            return 0;
        }

        if (run->hostCall == nullptr && run->hostCallV2 == nullptr) {
            run->failure =
                LUMEN_ANDROID_FEXCORE_RUNTIME_HOST_CALL_FAILED_V1;
            state.gregs[FEXCore::X86State::REG_R11] = exitAddress_;
            return 0;
        }

        // x86 SYSCALL writes the address after the instruction to RCX.  The
        // bridge saved the SysV fourth argument in RAX, which is otherwise a
        // caller-saved return register for the fixed GP HLE ABI.
        state.gregs[FEXCore::X86State::REG_RCX] =
            state.gregs[FEXCore::X86State::REG_RAX];
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
        // The Darwin bridge has just pushed the incoming RAX while holding
        // the execution lease. Read it before any safepoint can permit a VM
        // mutation, and remove this private slot before presenting the guest
        // CALL frame to HLE. AL carries the variadic SSE-register count.
        auto& rsp = state.gregs[FEXCore::X86State::REG_RSP];
        if (!HasGuestDataAccessUnderExecutionBarrier(
                *run, rsp, sizeof(std::uint64_t), false)) {
            run->failure = LUMEN_ANDROID_FEXCORE_RUNTIME_HOST_CALL_FAILED_V1;
            ForceStopActiveRunFromSyscall();
        }
        std::memcpy(&state.gregs[FEXCore::X86State::REG_RAX],
                    reinterpret_cast<const void*>(rsp), sizeof(std::uint64_t));
        rsp += sizeof(std::uint64_t);
#endif

        const bool coalesceBarrier = HostCallBarrierCoalescingEnabled();
        const bool shortAdmissionEnabled = ShortHostCallAdmissionEnabled();
        const bool syncMemoryShortEnabled = SynchronousMemoryShortAdmissionEnabled();
        SamplePerformanceStats(*run);
        auto& barrier = GuestMutationBarrier();
        bool shortHostCall = false;
        bool shortHostCallFallback = false;
        bool shortBoundaryCompleted = false;
        run->activeSyncMemoryShort = false;
        run->activeShortDemoted = false;
        using HleWindow = Lumen::Performance::HleWindow;
        const bool hleTraceEnabled = run->hleWindow != nullptr;
        const auto hleSlot = hleTraceEnabled
            ? run->hleWindow->Begin(target, {}) : HleWindow::Overflow;
        auto* hleAdmission = hleTraceEnabled ? run->hleWindow->ActiveInterval(HleWindow::Phase::Admission) : nullptr;
        auto* hleBody = hleTraceEnabled ? run->hleWindow->ActiveInterval(HleWindow::Phase::Body) : nullptr;
        auto* hleReadmission = hleTraceEnabled ? run->hleWindow->ActiveInterval(HleWindow::Phase::Readmission) : nullptr;
        run->activeHleTraceSlot = hleSlot;
        const auto finishHleTrace = [&](bool failed, Lumen::Performance::Stamp end) {
            if (!hleTraceEnabled) return;
            run->hleWindow->End(hleSlot, failed, shortBoundaryCompleted && !run->activeShortDemoted);
            run->hleWindow->Report(run->runId, run->nativeThreadId, end, false);
        };
        GuestMemoryMutationBarrier::BarrierFailureDiagnostic admissionDiagnostic;
        const bool hostCallAdmitted = [&] {
            Lumen::CriticalPath::Scope critical(Lumen::CriticalPath::Reason::HleAdmission, target);
            Lumen::Performance::Scope timing(Lumen::Performance::Phase::HostAdmission, 0,
                hleAdmission);
            if (shortAdmissionEnabled || syncMemoryShortEnabled) {
                using namespace Lumen::GuestCpu;
                // One cached target lookup supplies an atomic effects+contract
                // snapshot. The old selector-OFF path retains its effects ABI.
                const auto policy = syncMemoryShortEnabled ? QueryHostCallAdmissionPolicyV1(target) :
                    MakeHostCallAdmissionPolicy(static_cast<HostCallEffect>(QueryHostCallEffects(target)));
                const bool noEffects = shortAdmissionEnabled && AdmissionPolicyEffects(policy) == HostCallEffect::None;
                const bool syncMemory = syncMemoryShortEnabled &&
                    AdmissionPolicyEffects(policy) == HostCallEffect::GuestMemory &&
                    AdmissionPolicyContract(policy) == HostCallShortContract::SynchronousGuestMemory;
                if (noEffects || syncMemory) {
                    if ((!syncMemory || ExclusiveMemoryPublicationFrozen()) && barrier.TryBeginShortHostCall()) {
                        shortHostCall = true;
                        run->activeSyncMemoryShort = syncMemory;
                        if (syncMemory) ++run->syncMemoryShortEntries;
                        ++run->shortHostCalls;
                        // Sparse owner-thread diagnostics verify actual uptake
                        // without a global atomic increment on the common path.
                        if (run->shortHostCalls == 1 ||
                            (run->shortHostCalls & 0xfffffU) == 0)
                            std::fprintf(stderr, "LUMEN_FEX_HLE_SHORT_COUNTS_V1 "
                                "run=%llu short=%llu fallback=%llu ordinary=%llu target=%#llx\n",
                                static_cast<unsigned long long>(run->runId),
                                static_cast<unsigned long long>(run->shortHostCalls),
                                static_cast<unsigned long long>(run->shortHostCallFallbacks),
                                static_cast<unsigned long long>(run->ordinaryHostCalls),
                                static_cast<unsigned long long>(target));
                        return true;
                    }
                    ++run->shortHostCallFallbacks;
                    if (syncMemory) ++run->syncMemoryShortFallbacks;
                    shortHostCallFallback = true;
                }
                ++run->ordinaryHostCalls;
            }
            return coalesceBarrier
                ? barrier.CheckpointAndSuspendExecutionForHostCall(&admissionDiagnostic)
                : (barrier.CheckpointSafepoint(&admissionDiagnostic) &&
                   barrier.SuspendExecutionForHostCall(&admissionDiagnostic));
        }();
        if (hleTraceEnabled) {
            run->hleWindow->Admit(hleSlot, shortHostCall ? HleWindow::Admission::Short :
                shortHostCallFallback ? HleWindow::Admission::ShortFallback : HleWindow::Admission::Ordinary);
            run->hleWindow->Charge(hleSlot, HleWindow::Phase::Admission, hleAdmission->start, hleAdmission->end);
        }
        if (!hostCallAdmitted) {
            finishHleTrace(true, hleTraceEnabled ? hleAdmission->end : Lumen::Performance::Stamp {});
            ReportGuestBarrierFailure("host-call-admission", *run, admissionDiagnostic, target);
            run->failure =
                LUMEN_ANDROID_FEXCORE_RUNTIME_MUTATION_BARRIER_DENIED_V1;
            state.gregs[FEXCore::X86State::REG_R11] = exitAddress_;
            return 0;
        }

        std::uint64_t nextRip = 0;
        // Protocol ownership must not depend on fault-context diagnostics.
        run->activeHostCallTarget = target;
        run->activeHostCall = 1;
        run->observedHostCallTarget.store(target, std::memory_order_release);
        const std::uint32_t disposition = [&] {
            Lumen::CriticalPath::Scope critical(Lumen::CriticalPath::Reason::HleCall, target);
            Lumen::Performance::Scope timing(Lumen::Performance::Phase::HostCall, 0,
                hleBody);
            return InvokeHostCall(*run, target, state, &nextRip);
        }();
        if (hleTraceEnabled)
            run->hleWindow->Charge(hleSlot, HleWindow::Phase::Body, hleBody->start, hleBody->end);
        run->activeHostCall = 0;
        run->activeHostCallTarget = 0;
        run->observedHostCallTarget.store(0, std::memory_order_release);
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD) || defined(__ANDROID__)
        if (run->faultContextEnabled) {
            // A signal which the delegated handler recovered from must not be
            // misattributed to a later failure in this Run.
            run->diagnosticCaptureState.store(
                0, std::memory_order_release);
        }
#endif
        // Even a caller ignoring a failed End cannot resume this parent. A
        // still-donated token is restored only to balance the barrier before
        // forcing the failed Run to leave; it never authorizes guest access.
        if (run->freshCallbackToken != 0) {
            run->freshCallbackRestoreFailed = true;
            if (barrier.EndFreshCallback(run->freshCallbackToken))
            {
                run->freshCallbackToken = 0;
                run->observedFreshCallbackToken.store(0, std::memory_order_release);
            }
        }
        GuestMemoryMutationBarrier::BarrierFailureDiagnostic readmissionDiagnostic;
        const bool executionReadmitted = [&] {
            Lumen::CriticalPath::Scope critical(Lumen::CriticalPath::Reason::HleReadmission, target);
            Lumen::Performance::Scope timing(Lumen::Performance::Phase::Readmission, 0,
                hleReadmission);
            if (shortHostCall) return barrier.EndShortHostCall(&readmissionDiagnostic);
            return coalesceBarrier
                ? barrier.ResumeExecutionAfterHostCallAndCheckpoint(&readmissionDiagnostic)
                : (barrier.ResumeExecutionAfterHostCall(&readmissionDiagnostic) &&
                   barrier.CheckpointSafepoint(&readmissionDiagnostic));
        }();
        if (hleTraceEnabled)
            run->hleWindow->Charge(hleSlot, HleWindow::Phase::Readmission,
                hleReadmission->start, hleReadmission->end);
        if (!executionReadmitted) {
            finishHleTrace(true, hleTraceEnabled ? hleReadmission->end : Lumen::Performance::Stamp {});
            ReportGuestBarrierFailure("host-call-readmission", *run, readmissionDiagnostic, target);
            run->failure =
                LUMEN_ANDROID_FEXCORE_RUNTIME_MUTATION_BARRIER_DENIED_V1;
            state.gregs[FEXCore::X86State::REG_R11] = exitAddress_;
            return 0;
        }
        shortBoundaryCompleted = shortHostCall;
        if (run->activeSyncMemoryShort) {
            if (!run->activeShortDemoted) ++run->syncMemoryShortCompleted;
            if (run->syncMemoryShortEntries == 1 ||
                (run->syncMemoryShortEntries & 0xfffffU) == 0 ||
                (run->activeShortDemoted && run->syncMemoryShortDemoted == 1)) {
                std::fprintf(stderr, "LUMEN_FEX_HLE_SYNC_MEMORY_COUNTS_V1 run=%llu target=%#llx "
                    "entries=%llu completed_short=%llu demoted=%llu fallback=%llu\n",
                    static_cast<unsigned long long>(run->runId), static_cast<unsigned long long>(target),
                    static_cast<unsigned long long>(run->syncMemoryShortEntries),
                    static_cast<unsigned long long>(run->syncMemoryShortCompleted),
                    static_cast<unsigned long long>(run->syncMemoryShortDemoted),
                    static_cast<unsigned long long>(run->syncMemoryShortFallbacks));
            }
        }
        if (!FinishFreshCallbackHostCall(*run)) {
            finishHleTrace(true, hleTraceEnabled ? hleReadmission->end : Lumen::Performance::Stamp {});
            run->failure = LUMEN_ANDROID_FEXCORE_RUNTIME_MUTATION_BARRIER_DENIED_V1;
            ForceStopActiveRunFromSyscall();
        }
        const bool guestReturn = disposition == LUMEN_ANDROID_FEXCORE_HOST_CALL_RETURN_GUEST_V1;
        if (guestReturn || (disposition == LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1 &&
            nextRip != 0)) {
            // Both return dispositions consume one 16-byte <guest RIP,
            // host PC> entry. CONTINUE mirrors RET's increment here;
            // RETURN_GUEST leaves consumption and prediction to the JIT.
            const std::uint64_t callRetBase = reinterpret_cast<std::uintptr_t>(
                frame->Thread->CallRetStackBase);
            const std::uint64_t callRetEnd =
                callRetBase + kCallRetStackSize;
            if (callRetBase == 0 || state.callret_sp < callRetBase ||
                state.callret_sp > callRetEnd - 16) {
                finishHleTrace(true, hleTraceEnabled ? hleReadmission->end : Lumen::Performance::Stamp {});
                run->failure =
                    LUMEN_ANDROID_FEXCORE_RUNTIME_THREAD_SETUP_FAILED_V1;
                state.gregs[FEXCore::X86State::REG_R11] = exitAddress_;
                return 0;
            }
            if (guestReturn) {
                // The normal RET must run after readmission: it reads the
                // current guest slot, performs the memory handoff and consumes
                // exactly one shadow-stack entry in FEX's Return branch.
                state.gregs[FEXCore::X86State::REG_R11] = internalBase_ + kHostCallReturnOffset;
            } else {
                state.callret_sp += 16;
                state.gregs[FEXCore::X86State::REG_R11] = nextRip;
            }
        } else if (disposition ==
                   LUMEN_ANDROID_FEXCORE_HOST_CALL_STOP_V1) {
            state.gregs[FEXCore::X86State::REG_R11] = exitAddress_;
        } else {
            run->failure =
                LUMEN_ANDROID_FEXCORE_RUNTIME_HOST_CALL_FAILED_V1;
            state.gregs[FEXCore::X86State::REG_R11] = exitAddress_;
        }
        finishHleTrace(run->failure != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1,
            hleTraceEnabled ? hleReadmission->end : Lumen::Performance::Stamp {});
        return 0;
    }

    void SleepThread(FEXCore::Context::Context*,
                     FEXCore::Core::CpuStateFrame* frame) override {
        Lumen::Performance::Scope timing(Lumen::Performance::Phase::Safepoint);
        auto* run = g_activeRun;
        if (run != nullptr) SamplePerformanceStats(*run);
        GuestMemoryMutationBarrier::BarrierFailureDiagnostic checkpointDiagnostic;
        const bool validFrame = run != nullptr && frame != nullptr && frame->Thread == run->thread;
        if (!validFrame ||
            !GuestMutationBarrier().CheckpointSafepoint(&checkpointDiagnostic)) {
            if (run != nullptr) {
                if (!validFrame) checkpointDiagnostic = {};
                ReportGuestBarrierFailure("sleep-checkpoint", *run, checkpointDiagnostic);
                run->failure =
                    LUMEN_ANDROID_FEXCORE_RUNTIME_MUTATION_BARRIER_DENIED_V1;
            }
            return;
        }
        // One armed boundary interrupt delivers here: the SRA spill already
        // published the complete interrupted state and any pending mutation
        // epoch parked this reader above. The runtime callback consumes its
        // queue with exactly this boundary state; a later raise re-arms.
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
        if (run->faultContextEnabled && run->diagnosticSafepointCount < 64) {
            ++run->diagnosticSafepointCount;
            std::fprintf(stderr,
                "[MACOS_FEXCORE_SAFEPOINT_RESUME_V1] run=%llu scope=%s index=%u hostPc=0x%016llx resumeHostPc=0x%016llx resumeGuestRip=0x%016llx spilledRip=0x%016llx rsi=0x%016llx r9=0x%016llx rdi=0x%016llx rsp=0x%016llx\n",
                static_cast<unsigned long long>(run->runId), run->runtimeProbe ? "runtime-probe" : "guest",
                run->diagnosticSafepointCount,
                static_cast<unsigned long long>(run->diagnosticSafepointHostPc),
                static_cast<unsigned long long>(run->diagnosticSafepointResumeHostPc),
                static_cast<unsigned long long>(run->safepointResumeRip.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(frame->State.rip),
                static_cast<unsigned long long>(frame->State.gregs[FEXCore::X86State::REG_RSI]),
                static_cast<unsigned long long>(frame->State.gregs[FEXCore::X86State::REG_R9]),
                static_cast<unsigned long long>(frame->State.gregs[FEXCore::X86State::REG_RDI]),
                static_cast<unsigned long long>(frame->State.gregs[FEXCore::X86State::REG_RSP]));
            const auto& witness = run->diagnosticSafepointCodeWitness;
            std::fprintf(stderr,
                "[MACOS_FEXCORE_SAFEPOINT_CODE_WITNESS_V1] run=%llu scope=%s index=%u "
                "unitValid=%u unitBegin=0x%016llx unitSize=%llu unitGuestEntry=0x%016llx "
                "pcInUnit=%u pcGuestRip=0x%016llx stateRip=0x%016llx followingInstruction=0x%08x "
                "branchTarget=0x%016llx branchInBuffer=%u branchInUnit=%u branchGuestRip=0x%016llx\n",
                static_cast<unsigned long long>(run->runId), run->runtimeProbe ? "runtime-probe" : "guest",
                run->diagnosticSafepointCount, witness.unit.valid,
                static_cast<unsigned long long>(witness.unit.begin),
                static_cast<unsigned long long>(witness.unit.size),
                static_cast<unsigned long long>(witness.unit.guestEntry),
                witness.unit.Contains(run->diagnosticSafepointHostPc),
                static_cast<unsigned long long>(witness.pcGuestRip),
                static_cast<unsigned long long>(witness.stateRip), witness.followingInstruction,
                static_cast<unsigned long long>(witness.branchTarget), witness.branchTargetInBuffer,
                witness.unit.Contains(witness.branchTarget),
                static_cast<unsigned long long>(witness.branchGuestRip));
        }
#endif
        if (run->boundaryInterruptToken.exchange(
                0, std::memory_order_acq_rel) == 0) {
            return;
        }
        struct ProtectionRestore {
            ActiveRun& run;
            ~ProtectionRestore() { RestoreBoundaryInterruptProtection(run); }
        } protectionRestore {*run};
        const auto delivery = g_boundaryDelivery.load(
            std::memory_order_acquire);
        if (delivery != nullptr) {
            // The spilled outer execution is paused, but its TLS execution
            // lease is still depth one. Delivery can synchronously enter a
            // nested guest handler whose first HLE call needs its own depth
            // one execution. Retain the outer frame as a host-memory reader,
            // exactly as the ordinary HLE boundary does; do not weaken the
            // barrier's nesting checks or replay the consumed request.
            auto& barrier = GuestMutationBarrier();
            const bool coalesced = HostCallBarrierCoalescingEnabled();
            GuestMemoryMutationBarrier::BarrierFailureDiagnostic deliveryAdmissionDiagnostic;
            const bool suspended = coalesced
                ? barrier.CheckpointAndSuspendExecutionForHostCall(&deliveryAdmissionDiagnostic)
                : (barrier.CheckpointSafepoint(&deliveryAdmissionDiagnostic) &&
                   barrier.SuspendExecutionForHostCall(&deliveryAdmissionDiagnostic));
            if (!suspended) {
                ReportGuestBarrierFailure("boundary-delivery-admission", *run, deliveryAdmissionDiagnostic);
                run->failure = LUMEN_ANDROID_FEXCORE_RUNTIME_MUTATION_BARRIER_DENIED_V1;
                return;
            }
            std::uint32_t disposition = LUMEN_ANDROID_FEXCORE_BOUNDARY_IDLE_V1;
            const bool delivered = [&] {
                Lumen::Performance::Scope hostTiming(Lumen::Performance::Phase::HostCall);
                return InvokeBoundaryDelivery(*run, delivery, frame->State.gregs,
                                              frame->State.rip, disposition);
            }();
            // Always restore execution admission after an admitted callback,
            // including a native FP-boundary failure, before returning to JIT.
            GuestMemoryMutationBarrier::BarrierFailureDiagnostic deliveryReadmissionDiagnostic;
            const bool resumed = coalesced
                ? barrier.ResumeExecutionAfterHostCallAndCheckpoint(&deliveryReadmissionDiagnostic)
                : (barrier.ResumeExecutionAfterHostCall(&deliveryReadmissionDiagnostic) &&
                   barrier.CheckpointSafepoint(&deliveryReadmissionDiagnostic));
            if (!resumed) {
                ReportGuestBarrierFailure("boundary-delivery-readmission", *run, deliveryReadmissionDiagnostic);
                run->failure = LUMEN_ANDROID_FEXCORE_RUNTIME_MUTATION_BARRIER_DENIED_V1;
                // A rejected readmission leaves invalid TLS ownership; the
                // execution lease destructor cannot release a suspended host
                // reader. Returning structured state would preserve that
                // broken ownership in this process. Never guess a reset or
                // resume guest code after such a protocol violation.
                __android_log_print(ANDROID_LOG_FATAL, "LumenFexCoreRuntime",
                    "failed FEX boundary delivery readmission: invalid barrier ownership; refusing suspended-reader return");
                _exit(125);
            }
            if (!delivered) {
                run->failure = LUMEN_ANDROID_FEXCORE_RUNTIME_HOST_CALL_FAILED_V1;
                return;
            }
            if (disposition == LUMEN_ANDROID_FEXCORE_BOUNDARY_STOP_V1) {
                // The pause-return handler reconstructs RIP from this saved
                // slot, overwriting State.rip. Redirect both to the existing
                // root sentinel only after balancing execution admission.
                // No interrupted guest instruction runs after handler exit.
                frame->State.rip = run->exitAddress;
                run->safepointResumeRip.store(run->exitAddress, std::memory_order_relaxed);
            }
        }
    }

    void PreCompile() override {
        Lumen::Performance::Scope timing(Lumen::Performance::Phase::Safepoint);
        GuestMemoryMutationBarrier::BarrierFailureDiagnostic diagnostic;
        if (!GuestMutationBarrier().CheckpointSafepoint(&diagnostic)) {
            if (auto* run = g_activeRun; run != nullptr) {
                ReportGuestBarrierFailure("precompile-checkpoint", *run, diagnostic);
                run->failure =
                    LUMEN_ANDROID_FEXCORE_RUNTIME_MUTATION_BARRIER_DENIED_V1;
            }
            return;
        }
    }

    void InvalidateGuestCodeRange(
        FEXCore::Core::InternalThreadState* thread,
        std::uint64_t start, std::uint64_t length) override {
        InvalidateActiveGuestCodeRange(thread, start, length);
    }

    FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(
        FEXCore::Core::InternalThreadState* thread,
        std::uint64_t address) override {
        if (internalBase_ != 0 && address >= internalBase_ &&
            address - internalBase_ < internalSize_) {
            return {internalBase_, internalSize_, false};
        }

        auto* run = g_activeRun;
        const auto deny = [&]() noexcept {
            if (run != nullptr) {
                run->failure =
                    LUMEN_ANDROID_FEXCORE_RUNTIME_EXECUTABLE_RANGE_DENIED_V1;
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
                if (run->faultContextEnabled && run->context != nullptr &&
                    thread != nullptr && thread == run->thread) {
                    // A runtime miss enters CompileBlock while the source
                    // compilation unit is still the active inline JIT frame.
                    // Capture its root entry before the failed target compile
                    // unwinds. Initial/precompile denials have no active unit
                    // and remain explicitly unavailable.
                    const auto sourceCompilationEntry =
                        run->context->GetGuestBlockEntry(thread);
                    run->diagnosticSourceCompilationEntry.store(
                        sourceCompilationEntry, std::memory_order_relaxed);
                    run->diagnosticDeniedExecutableAddress.store(
                        address, std::memory_order_release);
                }
#endif
            }
            return FEXCore::HLE::ExecutableRangeInfo {};
        };
        if (run == nullptr || run->executableQuery == nullptr) {
            return deny();
        }

        std::uint64_t base = 0;
        std::uint64_t size = 0;
        std::uint32_t writable = 0;
        if (run->executableQuery(run->opaque, address, &base, &size,
                                 &writable) == 0 ||
            base == 0 || size == 0 || address < base ||
            address - base >= size) {
            return deny();
        }
        // MTRACK is paired with an immutable-RX policy in this adapter.  The
        // Linux FEX frontend has a write-fault tracker for W+X pages; Android
        // does not, so accepting such a range would silently lose SMC
        // detection.  Supported code changes instead transition through a
        // non-executable writable mapping under the mutation barrier.
        if (writable != 0) {
            if (!WritableExecutionExperimentEnabled()) {
                return deny();
            }
            // Console-semantics experiment: admit the writable range so the
            // oracle question can be exercised locally. SMC detection for
            // such pages is unavailable here, so the selection stays
            // non-promotable and every product default keeps the denial.
            return {base, size, true};
        }
        return {base, size, false};
    }

    std::optional<FEXCore::ExecutableFileSectionInfo>
    LookupExecutableFileSection(FEXCore::Core::InternalThreadState*,
                                std::uint64_t address) override {
        if (!translationCacheEnabled_ || address <= 1 ||
            (internalBase_ != 0 && address >= internalBase_ && address - internalBase_ < internalSize_)) {
            return std::nullopt;
        }
        const auto* run = g_activeRun;
        if (run == nullptr || run->executableQuery == nullptr ||
            (run->hostTargets && run->hostTargets->Contains(address))) {
            // Custom-IR native HLE targets are regenerated for each process.
            return std::nullopt;
        }
        std::uint64_t base {}, size {};
        std::uint32_t writable {};
        // This is optional cache metadata, not an execution admission query.
        // FEX's relocation filter also asks about non-executable data targets;
        // rejecting cache storage must never poison the running CPU root.
        if (run->executableQuery(run->opaque, address, &base, &size, &writable) == 0 ||
            base == 0 || size == 0 || writable != 0 || address < base ||
            address - base >= size || size > UINT64_MAX - base) return std::nullopt;
        try {
            std::lock_guard lock(translationFilesMutex_);
            const auto key = std::pair {base, size};
            auto found = translationFiles_.find(key);
            if (found == translationFiles_.end()) {
                auto info = std::make_unique<FEXCore::ExecutableFileInfo>();
                // FileId admits relocations only within this proven RX interval.
                info->FileId = translationFiles_.size() + 1;
                info->Filename = "lumen-immutable-rx";
                found = translationFiles_.emplace(key, std::move(info)).first;
            }
            // The incremental FEX key omits FileId and hashes only ModuleOffset.
            // A constant nonzero origin makes ModuleOffset address-1 unique across
            // all guest mappings. Relocations, entrypoints and code pages remain
            // relative to GuestRIP in this cache format, independently of origin.
            // This deliberately permits persistence only at the same guest VA.
            return FEXCore::ExecutableFileSectionInfo {*found->second, 1, base, base + size};
        } catch (...) {
            return std::nullopt;
        }
    }

    void SetTranslationCacheEnabled(bool enabled) noexcept { translationCacheEnabled_ = enabled; }

private:
    std::uint64_t internalBase_ {};
    std::uint64_t internalSize_ {};
    std::uint64_t exitAddress_ {};
    bool translationCacheEnabled_ = false;
    std::mutex translationFilesMutex_;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::unique_ptr<FEXCore::ExecutableFileInfo>> translationFiles_;
};

std::atomic<bool> g_cacheWorkerThreadCreationFailed {};

// FEXCore deliberately leaves its background-thread factory to the frontend.
// The incremental cache writer is a native worker, separate from guest roots.
class FexNativeWorkerThread final : public FEXCore::Threads::Thread {
public:
    FexNativeWorkerThread(FEXCore::Threads::ThreadFunc function, void* argument, bool lowPriority) noexcept {
        auto start = std::unique_ptr<StartData>(new (std::nothrow) StartData {function, argument, lowPriority});
        if (!start) return;
        active = pthread_create(&thread, nullptr, Entry, start.get()) == 0;
        if (active) start.release();
    }
    ~FexNativeWorkerThread() override { if (active) (void)detach(); }
    bool joinable() override { return active; }
    bool join(void** result) override {
        if (!active || IsSelf() || pthread_join(thread, result) != 0) return false;
        active = false;
        return true;
    }
    bool detach() override {
        if (!active || pthread_detach(thread) != 0) return false;
        active = false;
        return true;
    }
    bool IsSelf() override { return active && pthread_equal(thread, pthread_self()) != 0; }
private:
    struct StartData {
        FEXCore::Threads::ThreadFunc function;
        void* argument;
        bool lowPriority;
    };
    static void* Entry(void* opaque) {
        FEXCore::Allocator::InitializeThread();
        std::unique_ptr<StartData> start(static_cast<StartData*>(opaque));
        const auto function = start->function;
        void* argument = start->argument;
        if (start->lowPriority) {
#if defined(__APPLE__)
            (void)pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(__ANDROID__)
            (void)setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 19);
#endif
        }
        // No entrypoint state refers to the Thread object: a detached worker
        // may safely outlive that object, following the public pthread API.
        start.reset();
        void* result = function(argument);
        rpmalloc_thread_finalize();
        return result;
    }
    pthread_t thread {};
    bool active = false;
};

fextl::unique_ptr<FEXCore::Threads::Thread> CreateFexNativeWorkerThread(
    FEXCore::Threads::ThreadFunc function, void* argument
#if !defined(LUMEN_PS5_FEX_C13)
    , bool lowPriority
#endif
    ) {
#if defined(LUMEN_PS5_FEX_C13)
    constexpr bool lowPriority = false;
#endif
    void* storage = FEXCore::Allocator::aligned_alloc(alignof(FexNativeWorkerThread), sizeof(FexNativeWorkerThread));
    if (storage != nullptr) {
        auto thread = fextl::unique_ptr<FexNativeWorkerThread>(
            ::new (storage) FexNativeWorkerThread(function, argument, lowPriority));
        if (thread->joinable()) return thread;
    }
    g_cacheWorkerThreadCreationFailed.store(true, std::memory_order_relaxed);
    return nullptr;
}

void CleanupFexNativeWorkerAfterFork() {
    // Native pthread stacks and TLS are owned by libc; unlike FEX's Linux
    // clone frontend, this adapter has no custom stack tracker to reclaim.
}

struct RuntimeState {
    RuntimeState() noexcept {
        status.structSize = sizeof(LumenAndroidFexCoreRuntimeStatusV1);
        status.abiVersion = kRuntimeAbiVersion;
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1;
    }

    ~RuntimeState() {
        if (translationCache.Path().empty()) return;
        translationCache.StopCheckpointing();
        bool sealed = false;
        {
            std::lock_guard lock(activeThreadsMutex);
            if (activeRuns.empty()) {
                // Closing the native context drains the asynchronous FOZ
                // writer before any snapshot is hashed or published.
                context.reset();
                try { sealed = translationCache.Seal(); } catch (...) { sealed = false; }
            }
        }
        __android_log_print(ANDROID_LOG_INFO, "LumenFexCoreRuntime",
            "LUMEN_FEX_TRANSLATION_CACHE_COMMIT_V1 sealed=%u", sealed ? 1U : 0U);
    }

    std::once_flag once;
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    // Set once before context/cache creation and retained for truthful receipts.
    std::uint32_t maximumBlockInstructions {};
    // Published once after the complete readiness qualification. A release
    // frequency store makes the immutable scale visible to any host thread.
    std::atomic<std::uint64_t> guestTscFrequency {};
    std::atomic<std::uint32_t> guestTscScale {};
    Lumen::FexCore::TranslationCache::Session translationCache;
    fextl::unique_ptr<FEXCore::Context::Context> context;
    Signals signals;
    Syscalls syscalls;
    void* bridgePage {};
    std::mutex activeThreadsMutex;
    std::vector<ActiveRun*> activeRuns;
    std::mutex targetMutex;
    std::vector<std::uint64_t> hostTargets;
};

RuntimeState g_runtime;

bool FreshCallbackContains(std::uint64_t base, std::uint64_t size,
    std::uint64_t address, std::uint64_t length) noexcept {
    return base != 0 && size != 0 && size <= UINT64_MAX - base &&
        address >= base && address - base < size && length != 0 &&
        length <= size - (address - base);
}

bool FreshCallbackParentValid(ActiveRun& run, bool captureReturn) noexcept {
    if (run.thread == nullptr || run.thread->CurrentFrame == nullptr ||
        run.executableQuery == nullptr ||
        run.freshCallbackLifetimeRevoked.load(std::memory_order_acquire)) return false;
    const auto& state = run.thread->CurrentFrame->State;
    const auto rsp = state.gregs[FEXCore::X86State::REG_RSP];
    if (state.gs_cached != 0 || rsp < 128 || rsp > UINT64_MAX - 8 ||
        !FreshCallbackContains(run.freshStackBase, run.freshStackSize, rsp - 128, 136) ||
        !FreshCallbackDataAllowed(run, run.freshStackBase, run.freshStackSize, false) ||
        !FreshCallbackDataAllowed(run, run.freshStackBase, run.freshStackSize, true)) return false;
    const auto returnSlot = captureReturn ? rsp : run.freshCallbackRsp;
    if (!FreshCallbackContains(run.freshStackBase, run.freshStackSize, returnSlot, 8)) return false;
    std::uint64_t continuation {};
    std::memcpy(&continuation, reinterpret_cast<const void*>(returnSlot), sizeof(continuation));
    if (!captureReturn && (continuation != run.freshCallbackReturn ||
        state.fs_cached != run.freshCallbackFs)) return false;
    std::uint64_t codeBase {}, codeSize {};
    std::uint32_t writable {};
    if (run.executableQuery(run.opaque, continuation, &codeBase, &codeSize, &writable) != 1 ||
        !FreshCallbackContains(codeBase, codeSize, continuation, 1) || writable != 0) return false;
    if (state.fs_cached != 0 &&
        (!FreshCallbackContains(run.freshTlsBase, run.freshTlsSize, state.fs_cached, 8) ||
         !FreshCallbackDataAllowed(run, run.freshTlsBase, run.freshTlsSize, false) ||
         !FreshCallbackDataAllowed(run, run.freshTlsBase, run.freshTlsSize, true))) return false;
    if (captureReturn) {
        std::lock_guard lock(g_runtime.activeThreadsMutex);
        run.freshCallbackReturn = continuation;
        run.freshCallbackRsp = rsp;
        run.freshCallbackFs = state.fs_cached;
        run.freshCodeBase = codeBase;
        run.freshCodeSize = codeSize;
    }
    return !run.freshCallbackLifetimeRevoked.load(std::memory_order_acquire);
}

bool FreshCallbackChildEntryValid(ActiveRun& run,
    const LumenAndroidFexCoreGuestStateV1& guest) noexcept {
    if (!GuestMutationBarrier().CurrentThreadOwnsExecutionOrMutationLease()) return false;
    bool fresh = false;
    for (auto* parent = run.parentRun; parent != nullptr; parent = parent->parentRun) {
        bool watched;
        {
            std::lock_guard lock(g_runtime.activeThreadsMutex);
            watched = parent->freshCallbackWatchActive;
        }
        if (!watched) continue;
        fresh = true;
        if (parent->freshCallbackRestoreFailed ||
            !FreshCallbackParentValid(*parent, false)) return false;
    }
    if (!fresh) return true;
    // The child is admitted as a new root only after an exclusive publisher
    // has left. Prove its entry, return/scratch slots and TLS under that lease
    // before FEX precompile can read or execute any child guest memory.
    const auto rsp = guest.gprs[4];
    if (rsp < 16 || rsp > UINT64_MAX - 8 ||
        !FreshCallbackDataAllowed(run, rsp - 16, 24, false) ||
        !FreshCallbackDataAllowed(run, rsp - 16, 24, true)) return false;
    for (const auto tls : {guest.fsBase, guest.gsBase})
        if (tls != 0 && !FreshCallbackDataAllowed(run, tls, 8, false)) return false;
    std::uint64_t base {}, size {};
    std::uint32_t writable {};
    return run.executableQuery != nullptr &&
        run.executableQuery(run.opaque, guest.rip, &base, &size, &writable) == 1 &&
        FreshCallbackContains(base, size, guest.rip, 1) && writable == 0;
}

bool FinishFreshCallbackHostCall(ActiveRun& run) noexcept {
    // Only the owning host thread changes these fields; mapping publishers
    // only read the watch and set its atomic revocation flag. Ordinary HLE
    // calls need no registry lock when this Run has never donated its reader.
    if (!run.freshCallbackWatchActive && !run.freshCallbackRestoreFailed &&
        run.freshCallbackToken == 0) return true;
    bool watched;
    {
        std::lock_guard lock(g_runtime.activeThreadsMutex);
        watched = run.freshCallbackWatchActive;
    }
    if (!watched && !run.freshCallbackRestoreFailed && run.freshCallbackToken == 0) return true;
    const bool valid = !run.freshCallbackRestoreFailed && run.freshCallbackToken == 0 &&
        GuestMutationBarrier().CurrentThreadOwnsExecutionOrMutationLease() &&
        FreshCallbackParentValid(run, false);
    if (!valid) {
        const auto& state = run.thread->CurrentFrame->State;
        __android_log_print(ANDROID_LOG_ERROR, "LumenFexCoreRuntime",
            "FEX_FRESH_CALLBACK_READMISSION_FAILED restored=%u token=%llu revoked=%u "
            "rsp=0x%llx savedRsp=0x%llx fs=0x%llx savedFs=0x%llx "
            "stack=0x%llx+0x%llx tls=0x%llx+0x%llx continuation=0x%llx",
            !run.freshCallbackRestoreFailed,
            static_cast<unsigned long long>(run.freshCallbackToken),
            run.freshCallbackLifetimeRevoked.load(std::memory_order_acquire),
            static_cast<unsigned long long>(state.gregs[FEXCore::X86State::REG_RSP]),
            static_cast<unsigned long long>(run.freshCallbackRsp),
            static_cast<unsigned long long>(state.fs_cached),
            static_cast<unsigned long long>(run.freshCallbackFs),
            static_cast<unsigned long long>(run.freshStackBase),
            static_cast<unsigned long long>(run.freshStackSize),
            static_cast<unsigned long long>(run.freshTlsBase),
            static_cast<unsigned long long>(run.freshTlsSize),
            static_cast<unsigned long long>(run.freshCallbackReturn));
        run.freshCallbackRestoreFailed = true;
        return false;
    }
    {
        std::lock_guard lock(g_runtime.activeThreadsMutex);
        run.freshCallbackWatchActive = false;
    }
    if (!g_freshCallbackRestoreReported.exchange(true, std::memory_order_relaxed))
        __android_log_print(ANDROID_LOG_INFO, "LumenFexCoreRuntime",
            "FEX_FRESH_GUEST_CALLBACK_V1 restore validated=1");
    return true;
}

[[maybe_unused]] void CheckpointTranslationCacheAtQuickExit() noexcept {
    if (g_runtime.translationCache.Path().empty()) return;
    g_runtime.translationCache.StopCheckpointing();
    bool sealed = false;
    // CPU roots and queued cache jobs can still exist. The helper takes the
    // pinned native writer's file locks and copies only completed FOZ pairs;
    // it neither destroys the context nor publishes files still being written.
    try { sealed = g_runtime.translationCache.Seal(); } catch (...) { sealed = false; }
    __android_log_print(ANDROID_LOG_INFO, "LumenFexCoreRuntime",
        "LUMEN_FEX_TRANSLATION_CACHE_COMMIT_V1 sealed=%u quick_exit=1", sealed ? 1U : 0U);
}

bool ConfigureTranslationCache(const FEXCore::HostFeatures& features) noexcept {
#if defined(LUMEN_PS5_FEX_C13)
    (void)features;
    // This pin predates upstream disk caching. Refuse an enabled or malformed
    // request instead of reusing incompatible cache bytes or silently ignoring it.
    FEXCore::Config::Set(FEXCore::Config::CONFIG_ENABLECODECACHINGWIP, "0");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_ENABLECODECACHEVALIDATION, "0");
    const char* selector = std::getenv("LUMEN_FEX_TRANSLATION_CACHE");
    const bool supported = selector == nullptr || std::strcmp(selector, "0") == 0;
    __android_log_print(supported ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
        "LumenFexCoreRuntime",
        "LUMEN_FEX_TRANSLATION_CACHE_V1 enabled=0 denied=%u reason=c13-pin-no-disk-cache",
        supported ? 0U : 1U);
    return supported;
#else
    // Never inherit external FEX cache settings. Game launchers prepare a
    // validated identity and directory before enabling the baseline cache.
    // Primitive probes without that prepared launch remain uncached.
    FEXCore::Config::Set(FEXCore::Config::CONFIG_DISKCACHE, "0");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_DISKCACHEFILEMAPPING, "0");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_DISKCACHERELOCATIONFILTER, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_DISKCACHERODBNAMES, "");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_ENABLECODECACHINGWIP, "0");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_ENABLECODECACHEVALIDATION, "0");
    const char* selector = std::getenv("LUMEN_FEX_TRANSLATION_CACHE");
    if (selector == nullptr || std::strcmp(selector, "0") == 0) return true;
    const auto denied = [](const char* reason) {
        __android_log_print(ANDROID_LOG_ERROR, "LumenFexCoreRuntime",
            "LUMEN_FEX_TRANSLATION_CACHE_V1 enabled=0 denied=1 reason=%s", reason);
        return false;
    };
    if (std::strcmp(selector, "1") != 0) return denied("selector");
#if !defined(LUMEN_FEX_TRANSLATION_CACHE_BUILD_ID)
    return denied("build-identity");
#else
    const char* directory = std::getenv("LUMEN_FEX_TRANSLATION_CACHE_DIR");
    if (directory == nullptr || directory[0] != '/') return denied("directory");
    try {
        namespace Cache = Lumen::FexCore::TranslationCache;
        if (!Cache::Hex(LUMEN_FEX_TRANSLATION_CACHE_BUILD_ID, 64)) return denied("build-identity");
        const char* contentId = std::getenv("LUMEN_FEX_TRANSLATION_CACHE_CONTENT_ID");
        if (contentId == nullptr || std::strlen(contentId) != 64) return denied("content-identity");
        std::string normalizedContentId(contentId, 64);
        for (auto& value : normalizedContentId) {
            if (value >= 'A' && value <= 'F') value += 'a' - 'A';
        }
        if (!Cache::Hex(normalizedContentId, 64)) return denied("content-identity");
        const auto config = FEXCore::Config::SerializeForCache();
        std::string identity = LUMEN_FEX_TRANSLATION_CACHE_BUILD_ID;
        identity.append(normalizedContentId);
        identity.append(config.data(), config.size());
        const std::uint64_t hostHash = features.HashForCaching();
        identity.append(reinterpret_cast<const char*>(&hostHash), sizeof(hostHash));
        // Renderer coherence changes emitted host instructions independently
        // of upstream FEX options. Separate both modes and all pre-capability
        // cache domains before the native loader can see any persisted code.
        Cache::AppendMemoryHandoffDomain(identity, MemoryHandoffInstrumentationEnabled());
#if defined(__APPLE__)
        // The direct scalar branch changes host code independently of FEX's
        // upstream feature/config hash and must never share its cache domain.
        const std::uint8_t directUnaligned = DirectUnalignedTsoEnabled() ? 1 : 0;
        identity.append(reinterpret_cast<const char*>(&directUnaligned), sizeof(directUnaligned));
#endif
        std::uint64_t counterFrequency {};
        asm volatile("mrs %0, cntfrq_el0" : "=r"(counterFrequency));
        if (counterFrequency == 0) return denied("counter-frequency");
        // CycleCounter lowering bakes the derived TSC shift into host code.
        // HostFeatures.HashForCaching and SerializeForCache omit CNTFRQ.
        identity.append(reinterpret_cast<const char*>(&counterFrequency), sizeof(counterFrequency));
        if (!g_runtime.translationCache.Start(directory, Cache::Digest(identity))) return denied("storage");
        FEXCore::Config::Set(FEXCore::Config::CONFIG_DISKCACHEPATH, g_runtime.translationCache.Path() + "/");
        FEXCore::Config::Set(FEXCore::Config::CONFIG_DISKCACHE, "1");
        g_runtime.syscalls.SetTranslationCacheEnabled(true);
        __android_log_print(ANDROID_LOG_INFO, "LumenFexCoreRuntime",
            "LUMEN_FEX_TRANSLATION_CACHE_V1 enabled=1 snapshot_loaded=%u origin=absolute-guest-va",
            g_runtime.translationCache.Loaded() ? 1U : 0U);
        return true;
    } catch (...) {
        return denied("storage");
    }
#endif
#endif
}

class HostFloatingPointEnvironment {
public:
    HostFloatingPointEnvironment() noexcept : saved(std::fegetenv(&environment) == 0) {}
    ~HostFloatingPointEnvironment() { if (saved) (void)std::fesetenv(&environment); }
    bool Valid() const noexcept { return saved; }
    const std::fenv_t* Snapshot() const noexcept { return &environment; }
    bool Restore() noexcept {
        if (!saved || std::fesetenv(&environment) != 0) return false;
        saved = false;
        return true;
    }
private:
    std::fenv_t environment {};
    bool saved;
};

bool HasScalarSseView(const FEXCore::Context::Context* context) noexcept {
    // This adapter already requires ContextImpl for its mutation and Custom-IR
    // integration. Read its current features rather than the creation input.
    const auto* impl = static_cast<const FEXCore::Context::ContextImpl*>(context);
    return impl != nullptr && !impl->HostFeatures.SupportsSVE256;
}

std::uint32_t InvokeHostCallWithGuestState(ActiveRun& run, std::uint64_t target,
    FEXCore::Core::CPUState& state, std::uint64_t* nextRip) noexcept {
    if (run.hostCallV2 == nullptr) {
        return run.hostCall != nullptr
            ? run.hostCall(run.opaque, target, state.gregs, nextRip)
            : LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    }
    if (!HasScalarSseView(run.context)) {
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    }
    // Qualified scalar profiles have SVE256 disabled. At the pinned FEX state
    // boundaries this selects the 16-byte SSE view; upper AVX storage stays
    // outside this borrowed view and is never written by this bridge.
    static_assert(sizeof(state.xmm.sse.data[0]) == 16);
    static_assert(sizeof(state.xmm.sse.data) == 16 * 16);
    LumenAndroidFexCoreSimdViewV2 simd {
        sizeof(LumenAndroidFexCoreSimdViewV2), 16, 16, 16,
        reinterpret_cast<std::uint8_t*>(state.xmm.sse.data),
    };
    return run.hostCallV2(run.opaque, target, state.gregs, &simd, nextRip);
}

std::uint32_t InvokeHostCall(ActiveRun& run, std::uint64_t target,
    FEXCore::Core::CPUState& state, std::uint64_t* nextRip) noexcept {
    if (run.hostFpEnvironment != nullptr) {
        HostFloatingPointEnvironment guestEnvironment;
        if (!guestEnvironment.Valid() || std::fesetenv(run.hostFpEnvironment) != 0) {
            return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
        }
        const std::uint32_t result = InvokeHostCallWithGuestState(run, target, state, nextRip);
        return guestEnvironment.Restore() ? result : LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    }
    return InvokeHostCallWithGuestState(run, target, state, nextRip);
}

bool InvokeBoundaryDelivery(ActiveRun& run, LumenAndroidFexCoreBoundaryDeliveryV1 delivery,
    const std::uint64_t* gprs, std::uint64_t rip, std::uint32_t& disposition) noexcept {
    if (run.hostFpEnvironment != nullptr) {
        HostFloatingPointEnvironment guestEnvironment;
        if (!guestEnvironment.Valid() || std::fesetenv(run.hostFpEnvironment) != 0) return false;
        // A zero callback result means no installed/queued handler ran. It is
        // not a protocol failure and must not replay the consumed token.
        disposition = delivery(gprs, rip);
        return guestEnvironment.Restore();
    }
    disposition = delivery(gprs, rip);
    return true;
}

std::uint64_t HostThreadIdentity() noexcept {
#if defined(__APPLE__)
    return reinterpret_cast<std::uint64_t>(pthread_self());
#else
    return static_cast<std::uint64_t>(pthread_self());
#endif
}

// The innermost run of the requesting host thread owns the executing
// translated code; nested Runs push after their parent.
ActiveRun* FindActiveRunForHostThreadLocked(
    std::uint64_t hostThread) noexcept {
    if (hostThread == 0) return nullptr;
    for (auto it = g_runtime.activeRuns.rbegin();
         it != g_runtime.activeRuns.rend(); ++it) {
        ActiveRun* run = *it;
        if (run != nullptr && run->thread != nullptr &&
            run->hostThread == hostThread) {
            return run;
        }
    }
    return nullptr;
}

// A pending one-shot request survives both the mutation disarm and the fault
// handler's page restoration only when its page is protected again before
// execution is admitted. Caller holds activeThreadsMutex.
void RearmBoundaryInterruptLocked(ActiveRun& run) noexcept {
    if (run.boundaryInterruptToken.load(std::memory_order_acquire) == 0 ||
        run.safepointEpoch.load(std::memory_order_acquire) != 0) {
        return;
    }
    if (mprotect(run.thread->InterruptFaultPage,
                 sizeof(run.thread->InterruptFaultPage),
                 PROT_READ) != 0) {
        __android_log_print(
            ANDROID_LOG_FATAL, "LumenFexCoreRuntime",
            "failed to arm FEX boundary interrupt page: errno=%d", errno);
        _exit(125);
    }
}

void RestoreBoundaryInterruptProtection(ActiveRun& run) noexcept {
    // Request/cancel and mutation arm/disarm use the same registry lock.
    // A callback may arm a second token while the first one is consumed;
    // neither that token nor a new mutation epoch may lose its protection
    // between this observation and mprotect. No barrier lock is held here.
    std::lock_guard threadsLock(g_runtime.activeThreadsMutex);
    const bool armed = run.boundaryInterruptToken.load(std::memory_order_acquire) != 0 ||
        run.safepointEpoch.load(std::memory_order_acquire) != 0;
    if (mprotect(run.thread->InterruptFaultPage,
                 sizeof(run.thread->InterruptFaultPage),
                 armed ? PROT_READ : PROT_READ | PROT_WRITE) != 0) {
        __android_log_print(
            ANDROID_LOG_FATAL, "LumenFexCoreRuntime",
            "failed to restore FEX boundary interrupt page: errno=%d", errno);
        _exit(125);
    }
}

bool ArmActiveGuestSafepoints(std::uint64_t epoch) noexcept {
    if (epoch == 0 ||
        !g_contextReadyForInvalidation.load(std::memory_order_acquire)) {
        return epoch != 0;
    }

    std::lock_guard threadsLock(g_runtime.activeThreadsMutex);
    auto* const owner = g_activeRun;
    for (auto* run : g_runtime.activeRuns) {
        if (run == nullptr || run == owner || run->thread == nullptr) {
            continue;
        }
        std::uint64_t expected = 0;
        if (!run->safepointEpoch.compare_exchange_strong(
                expected, epoch, std::memory_order_release,
                std::memory_order_relaxed) ||
            mprotect(run->thread->InterruptFaultPage,
                     sizeof(run->thread->InterruptFaultPage),
                     PROT_READ) != 0) {
            for (auto* rollback : g_runtime.activeRuns) {
                if (rollback == nullptr || rollback->thread == nullptr ||
                    rollback->safepointEpoch.load(
                        std::memory_order_acquire) != epoch) {
                    continue;
                }
                if (mprotect(rollback->thread->InterruptFaultPage,
                             sizeof(rollback->thread->InterruptFaultPage),
                             PROT_READ | PROT_WRITE) != 0) {
                    __android_log_print(
                        ANDROID_LOG_FATAL, "LumenFexCoreRuntime",
                        "failed to roll back FEX safepoint page: errno=%d",
                        errno);
                    _exit(125);
                }
                rollback->safepointEpoch.store(
                    0, std::memory_order_release);
            }
            return false;
        }
    }
#if defined(__ANDROID__) || defined(LUMEN_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED)
    if (FEXLumenGuestMemoryBulkWriterArmedV1 != nullptr)
        FEXLumenGuestMemoryBulkWriterArmedV1(epoch);
#endif
    return true;
}

void DisarmActiveGuestSafepoints(std::uint64_t epoch) noexcept {
    if (epoch == 0 ||
        !g_contextReadyForInvalidation.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard threadsLock(g_runtime.activeThreadsMutex);
    for (auto* run : g_runtime.activeRuns) {
        if (run == nullptr || run->thread == nullptr ||
            run->safepointEpoch.load(std::memory_order_acquire) != epoch) {
            continue;
        }
        if (mprotect(run->thread->InterruptFaultPage,
                     sizeof(run->thread->InterruptFaultPage),
                     PROT_READ | PROT_WRITE) != 0) {
            __android_log_print(
                ANDROID_LOG_FATAL, "LumenFexCoreRuntime",
                "failed to release FEX safepoint page: errno=%d", errno);
            _exit(125);
        }
        run->safepointEpoch.store(0, std::memory_order_release);
        // A boundary interrupt armed while this epoch was active kept its
        // token under the mutation's page protection. Restore that protection
        // now, while admission is still closed, or the request would never
        // reach a generated-code boundary.
        RearmBoundaryInterruptLocked(*run);
    }
}

[[noreturn]] void ForceStopActiveRunFromSyscall() noexcept {
    g_activeRun->forcedStop = 1;
    if (raise(SIGTRAP) != 0) {
        _exit(127);
    }
    __builtin_unreachable();
}

void InvalidateActiveGuestCodeRange(
    FEXCore::Core::InternalThreadState* thread,
    std::uint64_t start, std::uint64_t length) {
    if (length == 0) {
        return;
    }
    // FEX computes Start + Length - 1 internally. Publish a full-range
    // invalidation instead of allowing a wrapping guest range to invert its
    // ordered-cache iterators.
    const auto normalized = NormalizeGuestInvalidationRange(start, length);
    start = normalized.base;
    length = normalized.size;
#if defined(__ANDROID__) || defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
    Lumen::Performance::JitCacheEvent(
        static_cast<std::uint32_t>(FEXLumenJitCacheEventV1::InvalidateRequest),
        length, normalized.base == 0 && normalized.size == UINT64_MAX ? 1U : 0U);
#endif
    if (thread == nullptr &&
        !g_contextReadyForInvalidation.load(std::memory_order_acquire)) {
        return;
    }

    // Match FEX's ThreadManager lock ordering: keep the thread registry stable,
    // then take the code-invalidation lock before touching shared and per-thread
    // caches.  ExecuteThread itself is deliberately not serialized: blocking
    // HLE calls must not prevent another guest thread from making progress.
    std::lock_guard threadsLock(g_runtime.activeThreadsMutex);
    auto* context = thread != nullptr
        ? thread->CTX
        : g_runtime.context.get();
    if (context == nullptr) {
        return;
    }
    const auto invalidate = [&] {
        context->InvalidateCodeBuffersCodeRange(start, length);
        for (auto* activeRun : g_runtime.activeRuns) {
            if (activeRun == nullptr || activeRun->thread == nullptr) {
                continue;
            }
            context->InvalidateThreadCachedCodeRange(
                activeRun->thread, start, length);
        }
    };
    if (thread != nullptr) {
        auto codeLock = FEXCore::GuardSignalDeferringSection(
            context->GetCodeInvalidationMutex(), thread);
        invalidate();
    } else {
        auto codeLock = FEXCore::MaskSignalsAndLockMutex(
            context->GetCodeInvalidationMutex());
        invalidate();
    }
}

#if defined(__APPLE__)
bool ReadSysctlU64(const char* name, std::uint64_t* value) noexcept {
    if (name == nullptr || value == nullptr) {
        return false;
    }
    std::uint64_t storage {};
    std::size_t size = sizeof(storage);
    if (sysctlbyname(name, &storage, &size, nullptr, 0) != 0) {
        return false;
    }
    if (size == sizeof(std::uint32_t)) {
        std::uint32_t narrow {};
        std::memcpy(&narrow, &storage, sizeof(narrow));
        *value = narrow;
        return true;
    }
    if (size != sizeof(storage)) {
        return false;
    }
    *value = storage;
    return true;
}
#endif

FEXCore::HostFeatures BuildHostFeatures() {
    FEXCore::HostFeatures features {};
#if defined(__APPLE__)
    std::uint64_t observedCacheLine {};
    std::uint64_t observedLogicalCores {};
    if (!ReadSysctlU64("hw.cachelinesize", &observedCacheLine) ||
        !ReadSysctlU64("hw.logicalcpu", &observedLogicalCores) ||
        observedCacheLine < sizeof(std::uint32_t) ||
        (observedCacheLine & (observedCacheLine - 1)) != 0 ||
        observedLogicalCores < kMinimumLogicalCores ||
        observedLogicalCores > kMaximumCpuidLogicalCores) {
        return features;
    }
    const long cacheLine = static_cast<long>(observedCacheLine);
#else
    long cacheLine = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    if (cacheLine <= 0) {
        cacheLine = 64;
    }
#endif
    // FEX stores the CTR_EL0 DminLine encoding: log2(cache-line words),
    // rather than log2(cache-line bytes).
    [[maybe_unused]] std::uint32_t log2 = 0;
    for (std::uint64_t value = sizeof(std::uint32_t);
         value < static_cast<std::uint64_t>(cacheLine) && log2 < 15;
         value <<= 1, ++log2) {
    }
#if defined(LUMEN_PS5_FEX_C13)
    features.DCacheLineSize = static_cast<std::uint32_t>(cacheLine);
#else
    features.DCacheLineLog2 = log2;
#endif
    features.SupportsCacheMaintenanceOps = true;
#if defined(__APPLE__)
    // AFP provides independent input/output denormal control. Probe each
    // optional lowering from the capability API rather than a CPU model.
    std::uint64_t observedAfp {};
    features.SupportsAFP = ReadSysctlU64("hw.optional.arm.FEAT_AFP", &observedAfp) && observedAfp == 1;
    const auto available = [](const char* name) noexcept {
        std::uint64_t observed {};
        const bool queried = ReadSysctlU64(name, &observed);
        return Lumen::FexCore::ObservedHostCapability(queried, observed);
    };
    features.SupportsAtomics = available("hw.optional.arm.FEAT_LSE");
    features.SupportsFlagM = available("hw.optional.arm.FEAT_FlagM");
    features.SupportsFlagM2 = available("hw.optional.arm.FEAT_FlagM2");
    features.SupportsFRINTTS = available("hw.optional.arm.FEAT_FRINTTS");
    if (ExactEnvironmentFlag("LUMEN_FEX_MACOS_RCPC")) {
        // Both instruction families and signed offsets have native footprint
        // gates. Keep this candidate explicit until its real game delta is
        // measured; this does not assert hardware TSO or change TSO policy.
        features.SupportsRCPC = available("hw.optional.arm.FEAT_LRCPC");
        features.SupportsTSOImm9 = features.SupportsRCPC &&
            available("hw.optional.arm.FEAT_LRCPC2");
    }
    features.HostType = FEXCore::HostFeatures::HostTypeEnum::Unknown;
#else
    features.HostType = FEXCore::HostFeatures::HostTypeEnum::Linux;

    const unsigned long hwcap = getauxval(AT_HWCAP);
    const unsigned long hwcap2 = getauxval(AT_HWCAP2);
#ifdef HWCAP_AES
    features.SupportsAES = (hwcap & HWCAP_AES) != 0;
#endif
#ifdef HWCAP_PMULL
    features.SupportsPMULL_128Bit = (hwcap & HWCAP_PMULL) != 0;
#endif
#ifdef HWCAP_CRC32
    features.SupportsCRC = (hwcap & HWCAP_CRC32) != 0;
#endif
#ifdef HWCAP_ATOMICS
    features.SupportsAtomics = (hwcap & HWCAP_ATOMICS) != 0;
#endif
#ifdef HWCAP_LRCPC
    features.SupportsRCPC = (hwcap & HWCAP_LRCPC) != 0;
#endif
#ifdef HWCAP_ILRCPC
    features.SupportsTSOImm9 = (hwcap & HWCAP_ILRCPC) != 0;
#endif
#ifdef HWCAP_FCMA
    features.SupportsFCMA = (hwcap & HWCAP_FCMA) != 0;
#endif
#if defined(HWCAP_SHA1) && defined(HWCAP_SHA2)
    features.SupportsSHA =
        (hwcap & HWCAP_SHA1) != 0 && (hwcap & HWCAP_SHA2) != 0;
#endif
#ifdef HWCAP_FLAGM
    features.SupportsFlagM = (hwcap & HWCAP_FLAGM) != 0;
#endif
#ifdef HWCAP2_RNG
    features.SupportsRAND = (hwcap2 & HWCAP2_RNG) != 0;
#endif
#ifdef HWCAP2_FLAGM2
    features.SupportsFlagM2 = (hwcap2 & HWCAP2_FLAGM2) != 0;
#endif
#ifdef HWCAP2_FRINT
    features.SupportsFRINTTS = (hwcap2 & HWCAP2_FRINT) != 0;
#endif
#ifdef HWCAP2_RPRES
    features.SupportsRPRES = (hwcap2 & HWCAP2_RPRES) != 0;
#endif
#ifdef HWCAP2_AFP
    features.SupportsAFP = (hwcap2 & HWCAP2_AFP) != 0;
#endif
#ifdef HWCAP2_ECV
    features.SupportsECV = (hwcap2 & HWCAP2_ECV) != 0;
#endif
#ifdef HWCAP2_WFXT
    features.SupportsWFXT = (hwcap2 & HWCAP2_WFXT) != 0;
#endif
#ifdef HWCAP2_CSSC
    features.SupportsCSSC = (hwcap2 & HWCAP2_CSSC) != 0;
#endif
#ifdef HWCAP2_SVEBITPERM
    features.SupportsSVEBitPerm =
        (hwcap2 & HWCAP2_SVEBITPERM) != 0;
#endif
#ifdef HWCAP2_MOPS
    features.SupportsMOPS = (hwcap2 & HWCAP2_MOPS) != 0;
#endif
#endif
    const Lumen::FexCore::NativeHostFeatures availableNative {
        features.SupportsAtomics != 0,
        features.SupportsFlagM != 0,
        features.SupportsFlagM2 != 0,
        features.SupportsFRINTTS != 0,
    };
    const bool nativeFeaturesEnabled = Lumen::FexCore::NativeHostFeaturesEnabled();
    const auto selectedNative = Lumen::FexCore::SelectNativeHostFeatures(
        availableNative, nativeFeaturesEnabled);
    features.SupportsAtomics = selectedNative.atomics;
    features.SupportsFlagM = selectedNative.flagM;
    features.SupportsFlagM2 = selectedNative.flagM2;
    features.SupportsFRINTTS = selectedNative.frintts;
    __android_log_print(ANDROID_LOG_INFO, "LumenFexCoreRuntime",
        "LUMEN_FEX_NATIVE_HOST_FEATURES_V1 enabled=%u "
        "available_lse=%u available_flagm=%u available_flagm2=%u available_frintts=%u "
        "effective_lse=%u effective_flagm=%u effective_flagm2=%u effective_frintts=%u",
        nativeFeaturesEnabled ? 1U : 0U,
        availableNative.atomics ? 1U : 0U, availableNative.flagM ? 1U : 0U,
        availableNative.flagM2 ? 1U : 0U, availableNative.frintts ? 1U : 0U,
        selectedNative.atomics ? 1U : 0U, selectedNative.flagM ? 1U : 0U,
        selectedNative.flagM2 ? 1U : 0U, selectedNative.frintts ? 1U : 0U);
#ifdef FEX_HAS_PRESERVE_ALL_ATTR
    features.SupportsPreserveAllABI = FEX_HAS_PRESERVE_ALL_ATTR != 0;
#endif
    // AVX guest decode is always supported by the AArch64 backend.  SVE256
    // only changes its lowering strategy; NEON pairs are the baseline path.
    features.SupportsAVX = true;
    features.SupportsAES256 = features.SupportsAES;
    features.Supports3DNow = true;
#if defined(__APPLE__)
    features.CPUMIDRs.resize(
        static_cast<std::size_t>(observedLogicalCores), 0);
#else
    const long configuredProcessors = sysconf(_SC_NPROCESSORS_CONF);
    if (configuredProcessors >= kMinimumLogicalCores &&
        configuredProcessors <= kMaximumCpuidLogicalCores) {
        features.CPUMIDRs.resize(
            static_cast<std::size_t>(configuredProcessors), 0);
    }
#endif
    return features;
}

bool RegisterHostTargetInternal(std::uint64_t target) noexcept {
    if (target == 0 || target == UINT64_MAX ||
        g_runtime.context == nullptr ||
        g_runtime.bridgePage == nullptr) {
        return false;
    }

    // The current thread/code-buffer lookup cache must resolve every
    // registered Custom-IR thunk before guest execution starts.  Keep
    // registration outside that window so no thread can observe a target
    // which it did not precompile.
    std::lock_guard threadsLock(g_runtime.activeThreadsMutex);
    std::lock_guard targetLock(g_runtime.targetMutex);
    const auto publication = PublishHostTarget(g_runtime.hostTargets, target,
        !g_runtime.activeRuns.empty(), [&] {
        const std::uintptr_t guestBridge =
            reinterpret_cast<std::uintptr_t>(g_runtime.bridgePage) +
            kBridgeOffset;
        auto* context = static_cast<FEXCore::Context::ContextImpl*>(
            g_runtime.context.get());
        const auto existing = context->AddCustomIREntrypoint(
            target,
            [guestBridge](std::uintptr_t entrypoint,
                          FEXCore::IR::IREmitter* emit) {
                auto* run = g_activeRun;
                if (run == nullptr || run->thread == nullptr ||
                    run->thread->FrontendDecoder.get() == nullptr ||
                    emit == nullptr) {
                    __android_log_print(
                        ANDROID_LOG_FATAL, "LumenFexCoreRuntime",
                        "Custom-IR target compiled outside an active FEX run");
                    _exit(124);
                }

                // Pinned FEX bypasses the frontend decoder for Custom-IR but still
                // consumes its range metadata in CompileBlock. Set the range in
                // the handler itself so every generation is correct, including a
                // lazy recompile after the shared code buffer rolls over.
                run->thread->FrontendDecoder->DecodedMinAddress = entrypoint;
                run->thread->FrontendDecoder->DecodedMaxAddress = entrypoint + 1;

                auto header = emit->_IRHeader(
                    emit->Invalid(), entrypoint, 0, 0, 0, 0);
                auto block = emit->CreateCodeNode(true, 0);
                header.first->Blocks = emit->WrapNode(block);
                emit->SetCurrentCodeBlock(block);

                FEXCore::IR::Ref targetRegister = emit->_StoreRegister(
                    emit->Constant(entrypoint), FEXCore::IR::OpSize::i64Bit);
                targetRegister->Reg = FEXCore::IR::PhysicalRegister(
                    FEXCore::IR::RegClass::GPRFixed,
                    FEXCore::X86State::REG_R11).Raw;
                emit->_ExitFunction(
                    FEXCore::IR::OpSize::i64Bit,
                    emit->Constant(guestBridge),
                    FEXCore::IR::BranchHint::None,
                    emit->Invalid(), emit->Invalid()
#if !defined(LUMEN_PS5_FEX_C13)
                    , false
#endif
                    );
            },
            &g_hostTargetCustomIrCreatorTag,
            reinterpret_cast<void*>(guestBridge));
        // A conflicting Custom-IR owner must never become an authorized target.
        return !existing.has_value();
    });
    if (publication == HostTargetPublication::Added) {
        g_runtime.status.registeredHostTargets =
            static_cast<std::uint32_t>(g_runtime.hostTargets.size());
    }
    return publication != HostTargetPublication::Rejected;
}

bool PrecompileRegisteredHostTargets(
    FEXCore::Core::InternalThreadState* thread,
    const HostTargetSnapshot& targets) noexcept {
    if (thread == nullptr || thread->FrontendDecoder.get() == nullptr ||
        g_runtime.context == nullptr) {
        return false;
    }

    for (const std::uint64_t target : targets.RegistrationOrder()) {
        // Eagerly populate the first shared code-buffer generation. The
        // Custom-IR handler repeats this metadata assignment on every later
        // lazy compile, including after a code-buffer rollover.
        thread->FrontendDecoder->DecodedMinAddress = target;
        thread->FrontendDecoder->DecodedMaxAddress = target + 1;
        JitWriteScope writableJit;
        g_runtime.context->CompileRIP(thread, target);
    }
    return true;
}

void ConfigureLongMode(FEXCore::Core::CPUState& state) noexcept {
    state.segment_arrays[
        FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] =
        state.private_gdt;
    state.segment_arrays[
        FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] =
        state.private_gdt;
    state.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
    auto* codeSegment =
        FEXCore::Core::CPUState::GetSegmentFromIndex(state, state.cs_idx);
    FEXCore::Core::CPUState::SetGDTBase(codeSegment, 0);
    FEXCore::Core::CPUState::SetGDTLimit(codeSegment, 0xf'ffffU);
    codeSegment->L = 1;
    codeSegment->D = 0;
    state.cs_cached =
        FEXCore::Core::CPUState::CalculateGDTBase(*codeSegment);
}

std::uint32_t RunGuestInternal(
    LumenAndroidFexCoreGuestStateV1* guest,
    LumenAndroidFexCoreHostCallV1 hostCall,
    LumenAndroidFexCoreExecutableQueryV1 executableQuery,
    void* opaque,
    bool forceCodeCacheRolloverForProbe,
    LumenAndroidFexCoreHostCallV2 hostCallV2 = nullptr,
    bool runtimeProbe = false,
    LumenAndroidFexCoreGuestStateV3* checkpoint = nullptr,
    const std::fenv_t* hostFpEnvironment = nullptr,
    std::uint64_t probeDataBase = 0,
    std::uint64_t probeDataSize = 0,
    std::uint64_t ownedStackBase = 0,
    std::uint64_t ownedStackSize = 0,
    LumenMacOSFexCoreOwnedStackAccessQueryV1 ownedStackQuery = nullptr) noexcept {
    Lumen::Performance::Scope rootTiming(Lumen::Performance::Phase::RootSetup);
    if (guest == nullptr || guest->structSize != sizeof(*guest) ||
        guest->abiVersion != kRuntimeAbiVersion || guest->rip == 0 ||
        g_runtime.context == nullptr) {
        return LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1;
    }
    // A fresh V1/V2 root declares default MXCSR in CPUState, but FEX's
    // dispatcher does not import its rounding/FTZ bits into native FPCR.
    // Save the native caller before any setup, then isolate every HLE and
    // nested root from the running guest's native FP environment.
    HostFloatingPointEnvironment nativeEnvironment;
    if (!nativeEnvironment.Valid()) {
        return LUMEN_ANDROID_FEXCORE_RUNTIME_THREAD_SETUP_FAILED_V1;
    }
    FEXCore::Allocator::InitializeThread();
    const std::size_t callRetAllocationSize =
        kCallRetStackSize + 2 * HostPageSize();
    void* callRetAllocation = mmap(nullptr, callRetAllocationSize,
                                   PROT_NONE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (callRetAllocation == MAP_FAILED) {
        return LUMEN_ANDROID_FEXCORE_RUNTIME_THREAD_SETUP_FAILED_V1;
    }
    auto* callRetBase = static_cast<std::uint8_t*>(callRetAllocation) +
        HostPageSize();
    if (mprotect(callRetBase, kCallRetStackSize,
                 PROT_READ | PROT_WRITE) != 0) {
        munmap(callRetAllocation, callRetAllocationSize);
        return LUMEN_ANDROID_FEXCORE_RUNTIME_THREAD_SETUP_FAILED_V1;
    }

    FEXCore::Core::CPUState seed {};
    seed.rip = guest->rip;
    std::copy_n(guest->gprs, kGuestGprCount, seed.gregs);
    seed.fs_cached = guest->fsBase;
    seed.gs_cached = guest->gsBase;
    if (checkpoint != nullptr) {
        static_assert(sizeof(seed.xmm.sse.data[0]) == 16);
        static_assert(sizeof(seed.avx_high[0]) == 16);
        static_assert(sizeof(seed.mm) == sizeof(checkpoint->x87Registers));
        static_assert(sizeof(seed.flags) == sizeof(checkpoint->flags));
        for (std::size_t index = 0; index != 16; ++index) {
            std::memcpy(seed.xmm.sse.data[index], checkpoint->vectorRegisters[index], 16);
            std::memcpy(seed.avx_high[index], checkpoint->vectorRegisters[index] + 16, 16);
        }
        seed.mxcsr = checkpoint->mxcsr;
        seed.FCW = checkpoint->x87ControlWord;
        seed.AbridgedFTW = checkpoint->x87AbridgedTag;
        std::memcpy(seed.mm, checkpoint->x87Registers, sizeof(seed.mm));
        std::memcpy(seed.flags, checkpoint->flags, sizeof(seed.flags));
        seed.pf_raw = checkpoint->parityRaw;
        seed.af_raw = checkpoint->auxiliaryCarryRaw;
        // CPUState's MXCSR storage alone does not initialize native FP control.
        // Execute the pinned translator's guest LDMXCSR before entering user
        // code. The immutable bootstrap restores R10/R11/RSP and leaves flags
        // unchanged; its temporary stack accesses remain ordinary guest accesses.
        seed.gregs[FEXCore::X86State::REG_R10] = reinterpret_cast<std::uintptr_t>(checkpoint);
        seed.rip = reinterpret_cast<std::uintptr_t>(g_runtime.bridgePage) + kRootStateBootstrapOffset;
    }

    FEXCore::Core::InternalThreadState* thread = nullptr;
    {
        JitWriteScope writableJit;
#if defined(LUMEN_PS5_FEX_C13)
        thread = g_runtime.context->CreateThread(seed.rip,
            seed.gregs[FEXCore::X86State::REG_RSP], &seed);
#else
        thread = g_runtime.context->CreateThread(&seed);
#endif
    }
    if (thread == nullptr) {
        munmap(callRetAllocation, callRetAllocationSize);
        return LUMEN_ANDROID_FEXCORE_RUNTIME_THREAD_SETUP_FAILED_V1;
    }
#if defined(__APPLE__)
    thread->CurrentFrame->Pointers.LumenDirectUnalignedTso =
        reinterpret_cast<std::uintptr_t>(&DirectUnalignedTso);
#endif
    thread->CallRetStackBase = callRetBase;
    alignas(16) FEXCore::SHMStats::ThreadStats cacheStats {};
    auto* previousStats = thread->ThreadStats;
    bool cacheStatsAttached = false;
    auto& state = thread->CurrentFrame->State;
    const std::uintptr_t defaultCallRetSp =
        reinterpret_cast<std::uintptr_t>(callRetBase) +
        kCallRetStackSize / 4;
    // The synthetic root RET still consumes one FEX call-ret entry.  Seed
    // an invalid pair so it misses the fast return path, dispatches the root
    // sentinel normally, and restores the default shadow-stack pointer.
    std::memset(reinterpret_cast<void*>(defaultCallRetSp - 16), 0, 16);
    state.callret_sp = defaultCallRetSp - 16;
    ConfigureLongMode(state);

    sigset_t trapSet {};
    sigset_t previousSignalMask {};
    sigemptyset(&trapSet);
    sigaddset(&trapSet, SIGTRAP);
    if (pthread_sigmask(SIG_UNBLOCK, &trapSet,
                        &previousSignalMask) != 0) {
        g_runtime.context->DestroyThread(thread);
        munmap(callRetAllocation, callRetAllocationSize);
        return LUMEN_ANDROID_FEXCORE_RUNTIME_SIGNAL_SETUP_FAILED_V1;
    }
    ActiveRun run {
        g_activeRun, g_runtime.context.get(), thread, hostCall, hostCallV2, executableQuery, opaque,
        g_runtime.syscalls.ExitAddress(),
    };
    run.hostThread = HostThreadIdentity();
#if defined(__ANDROID__)
    run.nativeThreadId = static_cast<std::uint64_t>(syscall(SYS_gettid));
#endif
    run.hostFpEnvironment = hostFpEnvironment != nullptr
        ? hostFpEnvironment : nativeEnvironment.Snapshot();
    run.runtimeProbe = runtimeProbe;
    run.performanceTraceEnabled = Lumen::Performance::Enabled();
    if (run.performanceTraceEnabled) {
#if !defined(LUMEN_PS5_FEX_C13)
        Lumen::Performance::MarkJitCacheHooksAvailable();
#endif
        run.hleWindow.reset(new (std::nothrow) Lumen::Performance::HleWindow);
        if (run.hleWindow == nullptr)
            std::fputs("FEX_HLE_CENSUS_DENIED reason=allocation-failed\n", stderr);
    }
    run.probeDataBase = runtimeProbe ? probeDataBase : 0;
    run.probeDataSize = runtimeProbe ? probeDataSize : 0;
    run.ownedStackBase = ownedStackBase;
    run.ownedStackSize = ownedStackSize;
    run.ownedStackQuery = ownedStackQuery;
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
    run.faultContextEnabled = ExactEnvironmentFlag(
        "LUMEN_MACOS_FEXCORE_FAULT_CONTEXT");
    run.faultStaticGprsEnabled = run.faultContextEnabled && ExactEnvironmentFlag(
        "LUMEN_MACOS_FEXCORE_FAULT_STATIC_GPRS");
    run.faultHostCodeExtendedEnabled = Lumen::MacOS::FexCore::RawExtendedHostCodeEnabled(
        run.faultContextEnabled, run.faultStaticGprsEnabled,
        std::getenv("LUMEN_MACOS_FEXCORE_FAULT_HOST_CODE_EXTENDED"));
    if (run.faultContextEnabled) {
        ReportFaultContextConfiguration();
        run.diagnosticGuestRoute = !runtimeProbe;
        run.runId = g_runSequence.fetch_add(
            1, std::memory_order_relaxed) + 1;
    }
#elif defined(__ANDROID__)
    // The ordinary Android guest path has no debugger-side mapping from a
    // generated host PC back to the precise x86 instruction. Preserve FEX's
    // signal-safe reconstruction in one bounded failure record. Deliberate
    // runtime-probe faults stay silent.
    run.faultContextEnabled = !runtimeProbe;
    run.diagnosticGuestRoute = !runtimeProbe;
    run.faultStaticGprsEnabled = !runtimeProbe;
#endif
    if (run.performanceTraceEnabled && run.runId == 0)
        run.runId = g_runSequence.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(__APPLE__)
    if (run.performanceTraceEnabled) pthread_threadid_np(nullptr, &run.nativeThreadId);
#endif
    ActiveRun* previousRun = g_activeRun;
    auto* previousAllocatorThread = g_allocatorThread;
    g_activeRun = &run;
    g_allocatorThread = thread;
    FEXCore::Allocator::RegisterTLSData(thread);
    {
        std::lock_guard threadsLock(g_runtime.activeThreadsMutex);
        std::lock_guard targetLock(g_runtime.targetMutex);
        // Capture and admission are one publication window. Registration is
        // forbidden until every Run leaves; the owned snapshot also survives
        // our removal from activeRuns during the remaining TLS cleanup.
        run.hostTargets.emplace(g_runtime.hostTargets);
        g_runtime.activeRuns.push_back(&run);
    }
    {
        GuestMemoryMutationBarrier::BarrierFailureDiagnostic diagnostic;
        Lumen::Performance::Scope admissionTiming(Lumen::Performance::Phase::ExecutionAdmission);
        ExecutionBarrierLease executionLease(GuestMutationBarrier(), &diagnostic);
        admissionTiming.End();
        if (!executionLease.Acquired()) {
            ReportGuestBarrierFailure("execution-entry", run, diagnostic, 0, guest->rip);
            run.failure =
                LUMEN_ANDROID_FEXCORE_RUNTIME_MUTATION_BARRIER_DENIED_V1;
        } else if (!FreshCallbackChildEntryValid(run, *guest)) {
            run.failure = LUMEN_ANDROID_FEXCORE_RUNTIME_MUTATION_BARRIER_DENIED_V1;
        } else if (![&] {
            Lumen::Performance::Scope timing(Lumen::Performance::Phase::RegisteredTargets);
            return PrecompileRegisteredHostTargets(thread, *run.hostTargets);
        }()) {
            run.failure =
                LUMEN_ANDROID_FEXCORE_RUNTIME_THREAD_SETUP_FAILED_V1;
        } else {
            const char* cacheTrace = std::getenv("LUMEN_FEX_TRANSLATION_CACHE_TRACE");
            if (run.performanceTraceEnabled) {
                thread->ThreadStats = &cacheStats;
                cacheStatsAttached = true;
                run.performanceStats = &cacheStats;
            }
#if !defined(LUMEN_PS5_FEX_C13)
            if (cacheTrace != nullptr && std::strcmp(cacheTrace, "1") == 0 &&
                !g_runtime.translationCache.Path().empty()) {
                const auto section = g_runtime.syscalls.LookupExecutableFileSection(thread, guest->rip);
                const bool hit = section && static_cast<FEXCore::Context::ContextImpl*>(g_runtime.context.get())
                    ->DiskCache.Lookup(thread, *section, guest->rip).has_value();
                __android_log_print(ANDROID_LOG_INFO, "LumenFexCoreRuntime",
                    "LUMEN_FEX_TRANSLATION_CACHE_ROOT_V1 rip=0x%llx native_hit=%u",
                    static_cast<unsigned long long>(guest->rip), hit ? 1U : 0U);
                // The pinned core's counters increment only when this public
                // pointer is nonnull. Scope it to diagnostic guest execution;
                // precompiled HLE thunks and the availability probe are excluded.
                thread->ThreadStats = &cacheStats;
                cacheStatsAttached = true;
            }
#else
            (void)cacheTrace;
#endif
            if (forceCodeCacheRolloverForProbe) {
                const std::uintptr_t previousCache =
                    reinterpret_cast<std::uintptr_t>(
                        thread->LookupCache->Shared);
                {
                    JitWriteScope writableJit;
                    g_runtime.context->ClearCodeCache(thread);
                }
                if (previousCache == reinterpret_cast<std::uintptr_t>(
                        thread->LookupCache->Shared)) {
                    run.failure =
                        LUMEN_ANDROID_FEXCORE_RUNTIME_SELF_TEST_FAILED_V1;
                } else {
                    // Reproduce the stale-decoder precondition from the real
                    // 16 MiB rollover. The first guest compile also changes
                    // this range; the subsequent Custom-IR handler must repair
                    // it before compiling the native host target.
                    thread->FrontendDecoder->DecodedMinAddress = guest->rip;
                    thread->FrontendDecoder->DecodedMaxAddress =
                        guest->rip + 1;
                    // Every shared code-buffer generation needs native host
                    // targets compiled into that generation before a guest
                    // CALL can populate FEX's call-ret shadow entry.  Keeping
                    // a pointer into the retired generation becomes an
                    // allocator-metadata jump on Darwin.
                    if (!PrecompileRegisteredHostTargets(thread, *run.hostTargets)) {
                        run.failure =
                            LUMEN_ANDROID_FEXCORE_RUNTIME_THREAD_SETUP_FAILED_V1;
                    }
                }
            }
            if (run.failure == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) {
#if defined(__APPLE__)
                // All initialization/precompile scopes must be closed before
                // control enters a MAP_JIT dispatcher.  Make the execute
                // state explicit at this boundary; Darwin faults instruction
                // fetches while the current thread is still writable.
                if (FEXCore::Allocator::JITWriteScopeDepth != 0) {
                    run.failure =
                        LUMEN_ANDROID_FEXCORE_RUNTIME_THREAD_SETUP_FAILED_V1;
                } else {
                    pthread_jit_write_protect_np(1);
                }
#endif
            }
            if (run.failure == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) {
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
                if (ExactEnvironmentFlag("LUMEN_MACOS_FEXCORE_TRACE")) {
                    std::fprintf(
                        stderr,
                        "[MACOS_FEXCORE_EXECUTE_V1] rip=0x%016llx "
                        "dispatcher=0x%016llx loop=0x%016llx interrupt=%p "
                        "writeDepth=%u rollover=%u\n",
                        static_cast<unsigned long long>(state.rip),
                        static_cast<unsigned long long>(
                            thread->CurrentFrame->Pointers.DispatcherLoopTop),
                        static_cast<unsigned long long>(
                            thread->CurrentFrame->Pointers.DispatcherLoopTopFillSRA),
                        static_cast<void*>(thread->InterruptFaultPage),
                        FEXCore::Allocator::JITWriteScopeDepth,
                        forceCodeCacheRolloverForProbe ? 1U : 0U);
                    std::fflush(stderr);
                }
#endif
                // Checkpoint roots execute the existing LDMXCSR bootstrap.
                // Default roots need nearest rounding, gradual underflow and
                // cleared native status, matching their default 0x1f80 seed.
                if (checkpoint == nullptr && std::fesetenv(FE_DFL_ENV) != 0) {
                    run.failure = LUMEN_ANDROID_FEXCORE_RUNTIME_THREAD_SETUP_FAILED_V1;
                } else {
                    Lumen::Performance::Scope timing(Lumen::Performance::Phase::ExecuteEnvelope);
                    g_runtime.context->ExecuteThread(thread);
                }
            }
        }
    }
    if (run.hleWindow != nullptr)
        run.hleWindow->Report(run.runId, run.nativeThreadId, Lumen::Performance::Now(), true);
    if (cacheStatsAttached) {
        SamplePerformanceStats(run);
        run.performanceWait.reset();
        run.performanceStats = nullptr;
        thread->ThreadStats = previousStats;
        if (ExactEnvironmentFlag("LUMEN_FEX_TRANSLATION_CACHE_TRACE"))
            __android_log_print(ANDROID_LOG_INFO, "LumenFexCoreRuntime",
            "LUMEN_FEX_TRANSLATION_CACHE_RUN_V1 rip=0x%llx jit_compilations=%llu native_loads=%llu",
            static_cast<unsigned long long>(guest->rip),
            static_cast<unsigned long long>(cacheStats.AccumulatedJITCount),
#if defined(LUMEN_PS5_FEX_C13)
            0ULL);
#else
            static_cast<unsigned long long>(cacheStats.AccumulatedDiskCacheHitCount));
#endif
    }
    if (run.failure == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
        thread->CurrentFrame->SynchronousFaultData
            .FaultToTopAndGeneratedException &&
        state.rip != run.exitAddress) {
        // The internal root HLT is the only generated exception accepted as
        // a clean return.  Every other guest exception remains fail-stop.
        run.failure = LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1;
        run.faultSignal =
            thread->CurrentFrame->SynchronousFaultData.Signal;
        run.faultAddress = state.rip;
    }
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
    if (run.failure != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
        run.diagnosticCaptureState.load(std::memory_order_acquire) == 2) {
        ReportFaultContext(run);
    }
#elif defined(__ANDROID__)
    if (run.failure != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
        run.diagnosticCaptureState.load(std::memory_order_acquire) == 2) {
        ReportAndroidFaultContext(run);
    }
#endif
    {
        std::lock_guard threadsLock(g_runtime.activeThreadsMutex);
        const auto active = std::find(
            g_runtime.activeRuns.begin(), g_runtime.activeRuns.end(), &run);
        if (active != g_runtime.activeRuns.end()) {
            if (run.safepointEpoch.load(std::memory_order_acquire) != 0) {
                if (mprotect(thread->InterruptFaultPage,
                             sizeof(thread->InterruptFaultPage),
                             PROT_READ | PROT_WRITE) != 0) {
                    __android_log_print(
                        ANDROID_LOG_FATAL, "LumenFexCoreRuntime",
                        "failed to release exiting FEX safepoint: errno=%d",
                        errno);
                    _exit(125);
                }
                run.safepointEpoch.store(0, std::memory_order_release);
            }
            if (run.boundaryInterruptToken.load(
                    std::memory_order_acquire) != 0) {
                // The run is leaving; no delivery can reach this page. Keep
                // the protection consistent with the cleared token so the
                // thread state teardown never inherits a protected probe.
                if (mprotect(thread->InterruptFaultPage,
                             sizeof(thread->InterruptFaultPage),
                             PROT_READ | PROT_WRITE) != 0) {
                    __android_log_print(
                        ANDROID_LOG_FATAL, "LumenFexCoreRuntime",
                        "failed to release exiting FEX boundary interrupt: "
                        "errno=%d",
                        errno);
                    _exit(125);
                }
                run.boundaryInterruptToken.store(0, std::memory_order_release);
            }
            g_runtime.activeRuns.erase(active);
        }
    }

    if (previousAllocatorThread != nullptr) {
        FEXCore::Allocator::RegisterTLSData(previousAllocatorThread);
    } else {
        FEXCore::Allocator::UninstallTLSData(thread);
    }
    g_allocatorThread = previousAllocatorThread;
    g_activeRun = previousRun;
    if (pthread_sigmask(SIG_SETMASK, &previousSignalMask, nullptr) != 0 &&
        run.failure == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) {
        run.failure =
            LUMEN_ANDROID_FEXCORE_RUNTIME_SIGNAL_SETUP_FAILED_V1;
    }

    guest->rip = state.rip;
    std::copy_n(state.gregs, kGuestGprCount, guest->gprs);
    guest->fsBase = state.fs_cached;
    guest->gsBase = state.gs_cached;
    guest->faultAddress = run.faultAddress;
    guest->faultSignal = static_cast<std::uint32_t>(run.faultSignal);
    if (checkpoint != nullptr) {
        for (std::size_t index = 0; index != 16; ++index) {
            std::memcpy(checkpoint->vectorRegisters[index], state.xmm.sse.data[index], 16);
            std::memcpy(checkpoint->vectorRegisters[index] + 16, state.avx_high[index], 16);
        }
        checkpoint->mxcsr = state.mxcsr;
        checkpoint->x87ControlWord = state.FCW;
        checkpoint->x87AbridgedTag = state.AbridgedFTW;
        std::memcpy(checkpoint->x87Registers, state.mm, sizeof(state.mm));
        std::memcpy(checkpoint->flags, state.flags, sizeof(state.flags));
        checkpoint->parityRaw = state.pf_raw;
        checkpoint->auxiliaryCarryRaw = state.af_raw;
    }

    g_runtime.context->DestroyThread(thread);
    munmap(callRetAllocation, callRetAllocationSize);
    if (!nativeEnvironment.Restore()) {
        return LUMEN_ANDROID_FEXCORE_RUNTIME_BRIDGE_FAILED_V1;
    }
    return static_cast<std::uint32_t>(run.failure);
}

struct ProbeCallbackState {
    std::uint64_t target;
    std::uintptr_t stackBase;
    std::size_t stackSize;
    std::uint32_t callCount {};
};

void ProbeTargetIdentity() noexcept {
}

std::uint32_t ProbeHostCall(void* opaque, std::uint64_t target,
                            std::uint64_t* gprs,
                            std::uint64_t* nextRip) noexcept {
    auto* probe = static_cast<ProbeCallbackState*>(opaque);
    if (probe == nullptr || gprs == nullptr || nextRip == nullptr ||
        target != probe->target ||
        gprs[FEXCore::X86State::REG_RCX] != kExpectedFourthArgument) {
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    }
    const std::uint64_t rsp = gprs[FEXCore::X86State::REG_RSP];
    if (rsp < probe->stackBase ||
        rsp > probe->stackBase + probe->stackSize - sizeof(std::uint64_t)) {
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    }
    std::memcpy(nextRip, reinterpret_cast<const void*>(rsp),
                sizeof(*nextRip));
    gprs[FEXCore::X86State::REG_RSP] += sizeof(std::uint64_t);
    gprs[FEXCore::X86State::REG_RAX] = kExpectedResult;
    ++probe->callCount;
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
}

void InitializeRuntime() noexcept {
    auto& status = g_runtime.status;
    g_runtime.maximumBlockInstructions = Lumen::FexCore::ParseMaximumBlockInstructions(
        std::getenv("LUMEN_FEX_MAX_INST"));
    if (g_runtime.maximumBlockInstructions == 0) {
        __android_log_print(ANDROID_LOG_ERROR, "LumenFexCoreRuntime",
            "LUMEN_FEX_MAX_INST_DENIED reason=invalid-selector allowed=128,5000");
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
        return;
    }
#if defined(__APPLE__)
    const long observedHostPageSize = sysconf(_SC_PAGESIZE);
    status.hostPageSize = observedHostPageSize > 0
        ? static_cast<std::uint32_t>(observedHostPageSize)
        : 0;
    if (!ExactEnvironmentFlagWithDefault("LUMEN_MACOS_FEXCORE_RUN", true)) {
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1;
        return;
    }
    if (observedHostPageSize != static_cast<long>(HostPageSize())) {
        status.code =
            LUMEN_ANDROID_FEXCORE_RUNTIME_WRONG_PAGE_SIZE_V1;
        return;
    }
#ifndef MAP_JIT
    status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_RWX_DENIED_V1;
    return;
#else
    if (pthread_jit_write_protect_supported_np() == 0) {
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_RWX_DENIED_V1;
        return;
    }
    pthread_jit_write_protect_np(1);
#endif
#else
    const auto admission =
        Lumen::Android::FexCoreLab::CollectAdmissionReceipt();
    const auto& inputs = admission.inputs;
    status.hostPageSize = inputs.hostPageSize > 0
        ? static_cast<std::uint32_t>(inputs.hostPageSize)
        : 0;
    if (inputs.androidApi <
        Lumen::Android::FexCoreLab::kMinimumAndroidApi) {
        status.code =
            LUMEN_ANDROID_FEXCORE_RUNTIME_UNSUPPORTED_ANDROID_API_V1;
        return;
    }
    if (!Lumen::Android::FexCoreLab::IsSupportedHostPageSize(
            inputs.hostPageSize)) {
        status.code =
            LUMEN_ANDROID_FEXCORE_RUNTIME_WRONG_PAGE_SIZE_V1;
        return;
    }
    if (!admission.admitted) {
        status.code =
            LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1;
        return;
    }
    // Preserve FEX's assertion fail-stop and make the original diagnostic
    // visible in logcat before the trap, including during context creation.
    LogMan::Throw::InstallHandler([](const char* message) {
        __android_log_print(ANDROID_LOG_FATAL, "LumenFexCoreRuntime", "%s", message);
    });
    LogMan::Msg::InstallHandler([](LogMan::DebugLevels level, const char* message) {
        __android_log_print(level <= LogMan::ERROR ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO,
                            "LumenFexCoreRuntime", "%s", message);
    });
    g_hostPageSize = static_cast<std::size_t>(inputs.hostPageSize);
#if defined(LUMEN_PS5_FEX_C13)
    if (HostPageSize() != 4096) {
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_WRONG_PAGE_SIZE_V1;
        return;
    }
    // No interval hooks are emitted by the C13 core. The renderer must select
    // exclusive publication before any provider query or JIT compilation.
    if (MemoryHandoffInstrumentationEnabled()) {
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
        return;
    }
#else
    if (FEXCore::Utils::FEX_HOST_PAGE_SIZE != HostPageSize()) {
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
        return;
    }
#endif
#endif

    InstallStructuralAssertDiagnostics();

#if defined(__APPLE__)
    constexpr int executableFlags =
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT;
#else
    constexpr int executableFlags = MAP_PRIVATE | MAP_ANONYMOUS;
#endif
    void* executable = mmap(nullptr, HostPageSize(),
                            PROT_READ | PROT_WRITE | PROT_EXEC,
                            executableFlags, -1, 0);
    if (executable == MAP_FAILED) {
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_RWX_DENIED_V1;
        return;
    }
    status.rwxMapped = 1;
    constexpr std::array<std::uint32_t, 2> hostProbe {
        UINT32_C(0xd503245f), UINT32_C(0xd65f03c0),
    };
    {
        JitWriteScope writableJit;
        std::memcpy(executable, hostProbe.data(), sizeof(hostProbe));
#if defined(__APPLE__)
        sys_icache_invalidate(executable, sizeof(hostProbe));
#else
        __builtin___clear_cache(static_cast<char*>(executable),
                                static_cast<char*>(executable) +
                                    sizeof(hostProbe));
#endif
    }

#if defined(__APPLE__)
    reinterpret_cast<void (*)()>(executable)();
    if (munmap(executable, HostPageSize()) != 0) {
        status.code =
            LUMEN_ANDROID_FEXCORE_RUNTIME_RWX_EXECUTION_FAILED_V1;
        return;
    }
    status.rwxExecuted = 1;
#else
    // Never execute the RWX canary in either long-lived Android process. A
    // short child inherits the exact mapping; SIGILL/SIGSEGV/SIGBUS and hangs
    // are converted into a bounded diagnostic result in the parent.
    const pid_t executableProbeChild = fork();
    if (executableProbeChild == 0) {
        // Do not let an application handler or an inherited blocked mask turn
        // an instruction-fetch fault into a successful child exit.
        if (!PrepareExecutableProbeChild()) {
            _exit(126);
        }
        reinterpret_cast<void (*)()>(executable)();
        _exit(0);
    }
    const int executableProbeForkError = executableProbeChild < 0 ? errno : 0;
    const bool executableProbeUnmapped =
        munmap(executable, HostPageSize()) == 0;
    if (executableProbeChild < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "LumenFexCoreRuntime",
                            "executableProbe forkFailed errno=%d", executableProbeForkError);
        status.code =
            LUMEN_ANDROID_FEXCORE_RUNTIME_RWX_EXECUTION_FAILED_V1;
        return;
    }

    int executableProbeStatus = 0;
    int executableProbeWaitError = 0;
    const bool executableProbeWaited = WaitForExecutableProbeChild(
        executableProbeChild, &executableProbeStatus, &executableProbeWaitError);
    if (!executableProbeUnmapped || !executableProbeWaited ||
        !WIFEXITED(executableProbeStatus) ||
        WEXITSTATUS(executableProbeStatus) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, "LumenFexCoreRuntime",
                            "executableProbe failed unmapped=%u waited=%u waitErrno=%d childStatus=%d",
                            executableProbeUnmapped ? 1U : 0U,
                            executableProbeWaited ? 1U : 0U,
                            executableProbeWaitError, executableProbeStatus);
        status.code =
            LUMEN_ANDROID_FEXCORE_RUNTIME_RWX_EXECUTION_FAILED_V1;
        return;
    }
    status.rwxExecuted = 1;
#endif

    if (rpmalloc_initialize(nullptr) != 0) {
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1;
        return;
    }
    FEXCore::Allocator::InitializeThread();

    g_runtime.bridgePage = mmap(nullptr, HostPageSize(),
                                PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_runtime.bridgePage == MAP_FAILED) {
        g_runtime.bridgePage = nullptr;
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_BRIDGE_FAILED_V1;
        return;
    }
    auto* bridgeBytes = static_cast<std::uint8_t*>(g_runtime.bridgePage);
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
    constexpr std::array<std::uint8_t, 12> syscallBridge {
        0x50,            // push rax (preserve AL for variadic host calls)
#else
    constexpr std::array<std::uint8_t, 11> syscallBridge {
#endif
        0x48, 0x89, 0xc8, // mov rax, rcx
        0x4d, 0x89, 0xda, // mov r10, r11
        0x0f, 0x05,       // syscall
        0x41, 0xff, 0xe3, // jmp r11
    };
    std::memcpy(bridgeBytes + kBridgeOffset, syscallBridge.data(),
                syscallBridge.size());
    bridgeBytes[kHltOffset] = 0xf4;
    bridgeBytes[kHostCallReturnOffset] = 0xc3;
    bridgeBytes[kFaultProbeOffset] = 0x0f;
    bridgeBytes[kFaultProbeOffset + 1] = 0x0b;
    bridgeBytes[kDeniedSyscallProbeOffset] = 0x0f;
    bridgeBytes[kDeniedSyscallProbeOffset + 1] = 0x05;

    std::array<std::uint8_t, 40> rootStateBootstrap {
        0x41, 0x53,                         // push r11
        0x4d, 0x8b, 0x9a, 0, 0, 0, 0,       // mov r11, [r10 + root RIP offset]
        0x41, 0x53,                         // push r11
        0x41, 0x0f, 0xae, 0x92, 0, 0, 0, 0, // ldmxcsr [r10 + MXCSR offset]
        0x4d, 0x8b, 0x92, 0, 0, 0, 0,       // mov r10, [r10 + original R10 offset]
        0x4c, 0x8b, 0x5c, 0x24, 0x08,       // mov r11, [rsp + 8]
        0x48, 0x8d, 0x64, 0x24, 0x10,       // lea rsp, [rsp + 16]
        0xff, 0x64, 0x24, 0xf0,             // jmp [rsp - 16]
    };
    constexpr std::uint32_t rootRipOffset = offsetof(LumenAndroidFexCoreGuestStateV3, gp) +
        offsetof(LumenAndroidFexCoreGuestStateV1, rip);
    constexpr std::uint32_t rootMxcsrOffset = offsetof(LumenAndroidFexCoreGuestStateV3, mxcsr);
    constexpr std::uint32_t rootR10Offset = offsetof(LumenAndroidFexCoreGuestStateV3, gp) +
        offsetof(LumenAndroidFexCoreGuestStateV1, gprs) + 10 * sizeof(std::uint64_t);
    std::memcpy(rootStateBootstrap.data() + 5, &rootRipOffset, sizeof(rootRipOffset));
    std::memcpy(rootStateBootstrap.data() + 15, &rootMxcsrOffset, sizeof(rootMxcsrOffset));
    std::memcpy(rootStateBootstrap.data() + 22, &rootR10Offset, sizeof(rootR10Offset));
    std::memcpy(bridgeBytes + kRootStateBootstrapOffset, rootStateBootstrap.data(), rootStateBootstrap.size());
    static_assert(kRootStateBootstrapOffset + rootStateBootstrap.size() <= 4096);

    const std::uint64_t probeTarget =
        reinterpret_cast<std::uintptr_t>(&ProbeTargetIdentity);
    static_assert(syscallBridge.size() <= kHltOffset - kBridgeOffset);

    std::array<std::uint8_t, 25> guestProbe {
        0xb9, 0, 0, 0, 0,                    // mov ecx, imm32
        0x48, 0xbb, 0, 0, 0, 0, 0, 0, 0, 0, // mov rbx, imm64
        0xff, 0xd3,                           // call rbx
        0xb9, 0, 0, 0, 0,                    // mov ecx, imm32
        0xff, 0xd3,                           // call rbx
        0xc3,                                 // ret
    };
    std::memcpy(guestProbe.data() + 1, &kExpectedFourthArgument,
                sizeof(kExpectedFourthArgument));
    std::memcpy(guestProbe.data() + 7, &probeTarget,
                sizeof(probeTarget));
    std::memcpy(guestProbe.data() + 18, &kExpectedFourthArgument,
                sizeof(kExpectedFourthArgument));
    std::memcpy(bridgeBytes + kProbeOffset, guestProbe.data(),
                guestProbe.size());
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
    constexpr std::uint64_t unalignedAtomicExpected =
        UINT64_C(0x1122334455667788);
    constexpr std::uint64_t unalignedAtomicDesired =
        UINT64_C(0x8877665544332211);
    std::array<std::uint8_t, 29> unalignedAtomicProbe {
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, // movabs rax, expected
        0x48, 0xb9, 0, 0, 0, 0, 0, 0, 0, 0, // movabs rcx, desired
        0xf0, 0x48, 0x0f, 0xb1, 0x0f,       // lock cmpxchg [rdi], rcx
        0x48, 0x8b, 0x07,                   // mov rax, [rdi]
        0xc3,                               // ret
    };
    std::memcpy(unalignedAtomicProbe.data() + 2,
                &unalignedAtomicExpected,
                sizeof(unalignedAtomicExpected));
    std::memcpy(unalignedAtomicProbe.data() + 12,
                &unalignedAtomicDesired,
                sizeof(unalignedAtomicDesired));
    std::memcpy(bridgeBytes + kUnalignedAtomicProbeOffset,
                unalignedAtomicProbe.data(), unalignedAtomicProbe.size());
#endif
    if (mprotect(g_runtime.bridgePage, HostPageSize(), PROT_READ) != 0) {
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_BRIDGE_FAILED_V1;
        return;
    }
    status.hostBridgeReady = 1;

    FEXCore::Config::Initialize();
    FEXCore::Config::Load();
    FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLETELEMETRY, "1");
    FEXCore::Config::Set(
        FEXCore::Config::CONFIG_DISABLE_VIXL_INDIRECT_RUNTIME_CALLS, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_MULTIBLOCK, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_MAXINST,
        g_runtime.maximumBlockInstructions == 128 ? "128" : "5000");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_SMCCHECKS, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_MONOHACKS, "0");
#if defined(__ANDROID__) && !defined(NDEBUG)
    const char* jitSymbolDirectory =
        std::getenv("LUMEN_FEX_JIT_SYMBOL_DIRECTORY");
    const bool jitSymbolsRequested =
        jitSymbolDirectory != nullptr && jitSymbolDirectory[0] == '/';
    FEXCore::Config::Set(FEXCore::Config::CONFIG_BLOCKJITNAMING,
                         jitSymbolsRequested ? "1" : "0");
    __android_log_print(ANDROID_LOG_INFO, "LumenFexCoreRuntime",
                        "LUMEN_ANDROID_FEX_JIT_SYMBOLS_V1 requested=%u blockNaming=%u",
                        jitSymbolsRequested ? 1U : 0U,
                        jitSymbolsRequested ? 1U : 0U);
#endif


    const auto configuredMaximumInstructions = static_cast<std::uint64_t>(
        FEXCore::Config::Get_MAXINST());
    const auto configuredSmcMode = static_cast<std::uint64_t>(
        FEXCore::Config::Get_SMCCHECKS());
    const bool configuredMonoHacks = static_cast<bool>(
        FEXCore::Config::Get_MONOHACKS());
    if (configuredMaximumInstructions != g_runtime.maximumBlockInstructions ||
        configuredSmcMode != FEXCore::Config::CONFIG_SMC_MTRACK ||
        configuredMonoHacks) {
        status.code =
            LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
        return;
    }

    // MaxInst is an upstream AffectsCodeGen option. SerializeForCache binds
    // this selected value into both the adapter snapshot and FEX disk bucket.
    __android_log_print(ANDROID_LOG_INFO, "LumenFexCoreRuntime",
        "LUMEN_FEX_MAX_INST_V1 instructions=%u default=%u cache_domain=config smc=mtrack-rx",
        g_runtime.maximumBlockInstructions, Lumen::FexCore::kDefaultMaximumBlockInstructions);

    const auto* syncMemoryShort = std::getenv("LUMEN_EXPERIMENTAL_FEX_HLE_SYNC_MEMORY_SHORT");
    if (syncMemoryShort != nullptr && std::strcmp(syncMemoryShort, "0") != 0 &&
        std::strcmp(syncMemoryShort, "1") != 0) {
        __android_log_print(ANDROID_LOG_ERROR, "LumenFexCoreRuntime",
            "LUMEN_FEX_HLE_SYNC_MEMORY_SHORT_DENIED reason=invalid-selector");
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
        return;
    }
#if defined(__APPLE__)
    const auto* rcpc = std::getenv("LUMEN_FEX_MACOS_RCPC");
    if (rcpc != nullptr && std::strcmp(rcpc, "0") != 0 && std::strcmp(rcpc, "1") != 0) {
        __android_log_print(ANDROID_LOG_ERROR, "LumenFexCoreRuntime",
            "LUMEN_FEX_MACOS_RCPC_DENIED reason=invalid-selector");
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
        return;
    }
    const auto* directUnaligned = std::getenv("LUMEN_FEX_MACOS_DIRECT_UNALIGNED");
    if (directUnaligned != nullptr && std::strcmp(directUnaligned, "0") != 0 &&
        std::strcmp(directUnaligned, "1") != 0) {
        __android_log_print(ANDROID_LOG_ERROR, "LumenFexCoreRuntime",
            "LUMEN_FEX_MACOS_DIRECT_UNALIGNED_DENIED reason=invalid-selector");
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, "LumenFexCoreRuntime",
        "LUMEN_FEX_MACOS_DIRECT_UNALIGNED_V1 enabled=%u scalar_widths=2,4,8 split_granule=16 hardware_tso=0",
        DirectUnalignedTsoEnabled() ? 1U : 0U);
#endif
    auto features = BuildHostFeatures();
#if defined(__APPLE__)
    const bool rcpcRequested = rcpc != nullptr && std::strcmp(rcpc, "1") == 0;
    if (rcpcRequested && (!features.SupportsRCPC || !features.SupportsTSOImm9)) {
        __android_log_print(ANDROID_LOG_ERROR, "LumenFexCoreRuntime",
            "LUMEN_FEX_MACOS_RCPC_DENIED reason=host-capability-unavailable");
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, "LumenFexCoreRuntime",
        "LUMEN_FEX_MACOS_RCPC_V1 requested=%u effective_rcpc=%u effective_imm9=%u hardware_tso=0",
        rcpcRequested ? 1U : 0U, features.SupportsRCPC ? 1U : 0U, features.SupportsTSOImm9 ? 1U : 0U);
#endif
    if (features.CPUMIDRs.size() < kMinimumLogicalCores ||
        features.CPUMIDRs.size() > kMaximumCpuidLogicalCores) {
        status.code =
            LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
        return;
    }
    const auto configuredLogicalCores = static_cast<std::uint32_t>(
        features.CPUMIDRs.size());
    if (!ConfigureTranslationCache(features)) {
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
        return;
    }
    if (!g_runtime.translationCache.Path().empty()) {
        g_cacheWorkerThreadCreationFailed.store(false, std::memory_order_relaxed);
        FEXCore::Threads::Thread::SetInternalPointers({
            .CreateThread = CreateFexNativeWorkerThread,
            .CleanupAfterFork = CleanupFexNativeWorkerAfterFork,
        });
    }
    g_runtime.context =
        FEXCore::Context::Context::CreateNewContext(features);
    if (g_runtime.context == nullptr) {
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1;
        return;
    }
    if (g_cacheWorkerThreadCreationFailed.load(std::memory_order_relaxed)) {
        g_runtime.context.reset();
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_THREAD_SETUP_FAILED_V1;
        return;
    }
    // The pinned FEX JIT already owns a per-thread interrupt page and emits a
    // write at every block entry/backedge when this flag is enabled. The lab
    // uses those upstream checkpoints to quiesce peers for guest VM changes.
    static_cast<FEXCore::Context::ContextImpl*>(g_runtime.context.get())
        ->Config.NeedsPendingInterruptFaultCheck = true;
    g_runtime.syscalls.SetInternalExecutableRange(
        reinterpret_cast<std::uintptr_t>(g_runtime.bridgePage),
        HostPageSize());
    g_runtime.syscalls.SetExitAddress(
        reinterpret_cast<std::uintptr_t>(g_runtime.bridgePage) +
            kHltOffset);
    g_runtime.context->SetSignalDelegator(&g_runtime.signals);
    g_runtime.context->SetSyscallHandler(&g_runtime.syscalls);
    bool coreInitialized = false;
    {
        JitWriteScope writableJit;
        coreInitialized = g_runtime.context->InitCore();
    }
    if (!coreInitialized) {
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1;
        return;
    }
    g_contextReadyForInvalidation.store(true, std::memory_order_release);
    const auto cpuidLeafOne = g_runtime.context->RunCPUIDFunction(1, 0);
    const std::uint32_t reportedLogicalCores =
        (cpuidLeafOne.ebx >> 16) & 0xffU;
    if (reportedLogicalCores != configuredLogicalCores ||
        reportedLogicalCores < kMinimumLogicalCores) {
        status.code =
            LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
        return;
    }
    status.logicalCores = reportedLogicalCores;
    status.contextInitialized = 1;
    if (!g_runtime.signals.Install()) {
        status.code =
            LUMEN_ANDROID_FEXCORE_RUNTIME_SIGNAL_SETUP_FAILED_V1;
        return;
    }
    status.signalStopInstalled = 1;

    if (!RegisterHostTargetInternal(probeTarget)) {
        status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_BRIDGE_FAILED_V1;
        return;
    }

    void* probeStack = mmap(nullptr, HostPageSize(),
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (probeStack == MAP_FAILED) {
        status.code =
            LUMEN_ANDROID_FEXCORE_RUNTIME_THREAD_SETUP_FAILED_V1;
        return;
    }
    const std::uintptr_t stackBase =
        reinterpret_cast<std::uintptr_t>(probeStack);
    const std::uint64_t initialRsp = stackBase + HostPageSize() - 8;
    const std::uint64_t hltAddress =
        reinterpret_cast<std::uintptr_t>(g_runtime.bridgePage) +
        kHltOffset;
    std::memcpy(reinterpret_cast<void*>(initialRsp), &hltAddress,
                sizeof(hltAddress));

    LumenAndroidFexCoreGuestStateV1 guest {
        sizeof(LumenAndroidFexCoreGuestStateV1),
        kRuntimeAbiVersion,
        reinterpret_cast<std::uintptr_t>(g_runtime.bridgePage) +
            kProbeOffset,
        {},
        0,
        0,
        0,
        0,
        0,
    };
    guest.gprs[FEXCore::X86State::REG_RSP] = initialRsp;
    ProbeCallbackState probe {probeTarget, stackBase, HostPageSize()};
    const std::uint32_t runCode = RunGuestInternal(
        &guest, &ProbeHostCall, nullptr, &probe, true, nullptr, true,
        nullptr, nullptr, stackBase, HostPageSize());
    __android_log_print(
        ANDROID_LOG_INFO, "LumenFexCoreRuntime",
        "bridgeProbe code=%u calls=%u rollover=1 rip=0x%016llx "
        "exit=0x%016llx "
        "faultSignal=%u faultAddress=0x%016llx",
        static_cast<unsigned>(runCode),
        static_cast<unsigned>(probe.callCount),
        static_cast<unsigned long long>(guest.rip),
        static_cast<unsigned long long>(hltAddress),
        static_cast<unsigned>(guest.faultSignal),
        static_cast<unsigned long long>(guest.faultAddress));

    status.guestResult = guest.gprs[FEXCore::X86State::REG_RAX];
    status.hostBridgeExecuted =
        runCode == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
        status.guestResult == kExpectedResult && probe.callCount == 2;

    bool faultStopVerified = false;
    bool deniedSyscallStopVerified = false;
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
    bool unalignedAtomicVerified = false;
#else
    constexpr bool unalignedAtomicVerified = true;
#endif
    if (status.hostBridgeExecuted != 0) {
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
        constexpr std::uint64_t unalignedAtomicExpected =
            UINT64_C(0x1122334455667788);
        constexpr std::uint64_t unalignedAtomicDesired =
            UINT64_C(0x8877665544332211);
        const std::uintptr_t unalignedAtomicAddress = stackBase + 4;
        std::memcpy(reinterpret_cast<void*>(unalignedAtomicAddress),
                    &unalignedAtomicExpected,
                    sizeof(unalignedAtomicExpected));
        LumenAndroidFexCoreGuestStateV1 unalignedAtomicGuest {
            sizeof(LumenAndroidFexCoreGuestStateV1),
            kRuntimeAbiVersion,
            reinterpret_cast<std::uintptr_t>(g_runtime.bridgePage) +
                kUnalignedAtomicProbeOffset,
            {},
            0,
            0,
            0,
            0,
            0,
        };
        unalignedAtomicGuest.gprs[FEXCore::X86State::REG_RDI] =
            unalignedAtomicAddress;
        unalignedAtomicGuest.gprs[FEXCore::X86State::REG_RSP] = initialRsp;
        const std::uint32_t unalignedAtomicCode = RunGuestInternal(
            &unalignedAtomicGuest, nullptr, nullptr, nullptr, false, nullptr,
            true, nullptr, nullptr, stackBase, HostPageSize());
        std::uint64_t unalignedAtomicMemory = 0;
        std::memcpy(&unalignedAtomicMemory,
                    reinterpret_cast<const void*>(unalignedAtomicAddress),
                    sizeof(unalignedAtomicMemory));
        unalignedAtomicVerified =
            unalignedAtomicCode ==
                LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
            unalignedAtomicGuest.gprs[FEXCore::X86State::REG_RAX] ==
                unalignedAtomicDesired &&
            unalignedAtomicMemory == unalignedAtomicDesired;
        __android_log_print(
            ANDROID_LOG_INFO, "LumenFexCoreRuntime",
            "unalignedAtomicProbe code=%u verified=%u result=0x%016llx "
            "memory=0x%016llx",
            static_cast<unsigned>(unalignedAtomicCode),
            unalignedAtomicVerified ? 1U : 0U,
            static_cast<unsigned long long>(
                unalignedAtomicGuest.gprs[FEXCore::X86State::REG_RAX]),
            static_cast<unsigned long long>(unalignedAtomicMemory));
#endif
        LumenAndroidFexCoreGuestStateV1 faultGuest {
            sizeof(LumenAndroidFexCoreGuestStateV1),
            kRuntimeAbiVersion,
            reinterpret_cast<std::uintptr_t>(g_runtime.bridgePage) +
                kFaultProbeOffset,
            {},
            0,
            0,
            0,
            0,
            0,
        };
        faultGuest.gprs[FEXCore::X86State::REG_RSP] = initialRsp;
        const std::uint32_t faultCode = RunGuestInternal(
            &faultGuest, nullptr, nullptr, nullptr, false, nullptr, true);
        __android_log_print(
            ANDROID_LOG_INFO, "LumenFexCoreRuntime",
            "faultProbe code=%u signal=%u rip=0x%016llx "
            "faultAddress=0x%016llx",
            static_cast<unsigned>(faultCode),
            static_cast<unsigned>(faultGuest.faultSignal),
            static_cast<unsigned long long>(faultGuest.rip),
            static_cast<unsigned long long>(faultGuest.faultAddress));
        faultStopVerified =
            faultCode == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
            faultGuest.faultSignal == SIGILL;

        LumenAndroidFexCoreGuestStateV1 deniedSyscallGuest {
            sizeof(LumenAndroidFexCoreGuestStateV1),
            kRuntimeAbiVersion,
            reinterpret_cast<std::uintptr_t>(g_runtime.bridgePage) +
                kDeniedSyscallProbeOffset,
            {},
            0,
            0,
            0,
            0,
            0,
        };
        deniedSyscallGuest.gprs[FEXCore::X86State::REG_RSP] = initialRsp;
        const std::uint32_t deniedSyscallCode = RunGuestInternal(
            &deniedSyscallGuest, nullptr, nullptr, nullptr, false, nullptr,
            true);
        __android_log_print(
            ANDROID_LOG_INFO, "LumenFexCoreRuntime",
            "deniedSyscallProbe code=%u signal=%u rip=0x%016llx",
            static_cast<unsigned>(deniedSyscallCode),
            static_cast<unsigned>(deniedSyscallGuest.faultSignal),
            static_cast<unsigned long long>(deniedSyscallGuest.rip));
        deniedSyscallStopVerified =
            deniedSyscallCode ==
                LUMEN_ANDROID_FEXCORE_RUNTIME_SYSCALL_DENIED_V1 &&
            deniedSyscallGuest.rip ==
                reinterpret_cast<std::uintptr_t>(g_runtime.bridgePage) +
                    kDeniedSyscallProbeOffset;
    }
    munmap(probeStack, HostPageSize());

    status.guestExecuted =
        status.hostBridgeExecuted != 0 && faultStopVerified &&
        deniedSyscallStopVerified && unalignedAtomicVerified;
    status.backendRouteReady = status.guestExecuted;
    status.code = status.backendRouteReady != 0
        ? LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1
        : LUMEN_ANDROID_FEXCORE_RUNTIME_SELF_TEST_FAILED_V1;
    if (status.code == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) {
        const auto scale = static_cast<FEXCore::Context::ContextImpl*>(
            g_runtime.context.get())->Config.TSCScale;
        std::uint64_t nativeFrequency {};
        asm volatile("mrs %0, cntfrq_el0" : "=r"(nativeFrequency));
        // CPUID's scale numerator is 32-bit. Reject any undefined shift,
        // native overflow or disagreement instead of fabricating a domain.
        if (nativeFrequency == 0 || scale > 31 ||
            nativeFrequency > (UINT64_MAX >> scale)) {
            status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
            status.backendRouteReady = 0;
            return;
        }
        const auto leaf = g_runtime.context->RunCPUIDFunction(0x15, 0);
        const auto frequency = nativeFrequency << scale;
        const auto cpuidNumerator = static_cast<std::uint64_t>(leaf.ecx) * leaf.ebx;
        if (leaf.eax == 0 || cpuidNumerator % leaf.eax != 0 || cpuidNumerator / leaf.eax != frequency) {
            status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
            status.backendRouteReady = 0;
            return;
        }
        if (!g_runtime.translationCache.Path().empty() &&
            (std::at_quick_exit(CheckpointTranslationCacheAtQuickExit) != 0 ||
             !g_runtime.translationCache.StartCheckpointing())) {
            status.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
            status.backendRouteReady = 0;
            return;
        }
        g_runtime.guestTscScale.store(static_cast<std::uint32_t>(scale), std::memory_order_relaxed);
        g_runtime.guestTscFrequency.store(frequency, std::memory_order_release);
    }
}

void EnsureRuntime() noexcept {
#if defined(__ANDROID__) || defined(LUMEN_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED)
    (void)MemoryHandoffInstrumentationEnabled();
#endif
    std::call_once(g_runtime.once, InitializeRuntime);
}

enum class FpProbeHostAction { ChangeRounding, NestedRoot };

struct FpProbeState {
    std::uint64_t codeBase {}, codeSize {}, dataBase {}, dataSize {}, target {};
    FpProbeHostAction action {FpProbeHostAction::ChangeRounding};
    unsigned calls {};
    int entryRounding {};
    std::uint32_t nestedCode {LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1};
};

void FpProbeTargetIdentity() {}

std::uint32_t FpProbeExecutableQuery(void* opaque, std::uint64_t address,
    std::uint64_t* base, std::uint64_t* size, std::uint32_t* writable) noexcept {
    const auto* probe = static_cast<const FpProbeState*>(opaque);
    if (probe == nullptr || base == nullptr || size == nullptr || writable == nullptr ||
        address < probe->codeBase || address - probe->codeBase >= probe->codeSize) return 0;
    *base = probe->codeBase;
    *size = probe->codeSize;
    *writable = 0;
    return 1;
}

std::uint32_t FpProbeHostCall(void* opaque, std::uint64_t target,
    std::uint64_t* gprs, LumenAndroidFexCoreSimdViewV2*, std::uint64_t* nextRip) noexcept;

std::uint32_t RunFpProbeProgram(FpProbeState& probe, bool acrossHostCall,
    bool nested) noexcept {
    const auto stack = probe.dataBase + (nested ? probe.dataSize : probe.dataSize / 2) - 8;
    const auto sentinel = reinterpret_cast<std::uintptr_t>(g_runtime.bridgePage) + kHltOffset;
    std::memcpy(reinterpret_cast<void*>(stack), &sentinel, sizeof(sentinel));
    LumenAndroidFexCoreGuestStateV1 guest {
        sizeof(LumenAndroidFexCoreGuestStateV1), kRuntimeAbiVersion,
        probe.codeBase + (acrossHostCall ? 64 : 0), {}, 0, 0, 0, 0, 0,
    };
    guest.gprs[FEXCore::X86State::REG_RSP] = stack;
    guest.gprs[FEXCore::X86State::REG_RDI] = probe.dataBase;
    guest.gprs[FEXCore::X86State::REG_RSI] = probe.dataBase + (nested ? 64 : 32);
    guest.gprs[FEXCore::X86State::REG_RBX] = probe.target;
#if defined(__APPLE__)
    // The Mac translator checks every raw data access. This fixture owns the
    // complete two-page data/stack interval until every nested Run returns.
    // All other V2 seed and FP-boundary behavior stays unchanged.
    return RunGuestInternal(&guest, nullptr, &FpProbeExecutableQuery, &probe,
        false, &FpProbeHostCall, true, nullptr, nullptr, probe.dataBase, probe.dataSize);
#else
    return LumenAndroidFexCoreRunGuestV2(&guest, &FpProbeHostCall,
                                       &FpProbeExecutableQuery, &probe);
#endif
}

std::uint32_t FpProbeHostCall(void* opaque, std::uint64_t target,
    std::uint64_t* gprs, LumenAndroidFexCoreSimdViewV2*, std::uint64_t* nextRip) noexcept {
    auto* probe = static_cast<FpProbeState*>(opaque);
    if (probe == nullptr || target != probe->target || gprs == nullptr || nextRip == nullptr ||
        probe->calls != 0) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    const auto stack = gprs[FEXCore::X86State::REG_RSP];
    if (stack < probe->dataBase || stack - probe->dataBase > probe->dataSize - 8)
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    std::uint64_t returnRip = 0;
    std::memcpy(&returnRip, reinterpret_cast<const void*>(stack), sizeof(returnRip));
    if (returnRip < probe->codeBase || returnRip - probe->codeBase >= probe->codeSize)
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    ++probe->calls;
    probe->entryRounding = std::fegetround();
    if (probe->action == FpProbeHostAction::ChangeRounding) {
        if (std::fesetround(FE_UPWARD) != 0) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    } else {
        probe->nestedCode = RunFpProbeProgram(*probe, false, true);
        if (probe->nestedCode != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1)
            return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    }
    gprs[FEXCore::X86State::REG_RSP] += 8;
    *nextRip = returnRip;
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
}

class FpProbeNativeEnvironment final {
public:
    FpProbeNativeEnvironment() noexcept : valid_(std::fegetenv(&saved_) == 0) {}
    ~FpProbeNativeEnvironment() { if (valid_) (void)std::fesetenv(&saved_); }
    bool Valid() const noexcept { return valid_; }
    bool Restore() noexcept {
        if (!valid_ || std::fesetenv(&saved_) != 0) return false;
        valid_ = false;
        return true;
    }
private:
    std::fenv_t saved_ {};
    bool valid_ {};
};

bool CloseFpProbeMappings(FpProbeState& probe) noexcept {
    const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    if (token == 0) return false; // Keep owned storage live on a denied close.
    struct CloseState { FpProbeState* probe; bool closed; } close {&probe, false};
    const auto end = LumenAndroidFexCoreEndGuestMemoryMutationV2(
        token, probe.codeBase, probe.codeSize, 1,
        +[](void* opaque) noexcept {
            auto& state = *static_cast<CloseState*>(opaque);
            const bool codeClosed = munmap(reinterpret_cast<void*>(state.probe->codeBase),
                                          state.probe->codeSize) == 0;
            const bool dataClosed = munmap(reinterpret_cast<void*>(state.probe->dataBase),
                                          state.probe->dataSize) == 0;
            state.closed = codeClosed && dataClosed;
        }, &close);
    return end != 0 && close.closed;
}

const char* MutationDenialReasonName(
    GuestMemoryMutationBarrier::MutationDenialReason reason) noexcept {
    using Reason = GuestMemoryMutationBarrier::MutationDenialReason;
    switch (reason) {
    case Reason::None: return "none";
    case Reason::HostWaitActive: return "host-wait-active";
    case Reason::GuestDataLeaseActive: return "guest-data-lease-active";
    case Reason::NativeDataLeaseActive: return "native-data-lease-active";
    case Reason::ExclusivePublicationBorrowActive: return "exclusive-publication-borrow-active";
    case Reason::NestedMutationDenied: return "nested-mutation-denied";
    case Reason::ForeignExecutionBarrier: return "foreign-execution-barrier";
    case Reason::ForeignHostCallBarrier: return "foreign-host-call-barrier";
    case Reason::ShortHostCallActive: return "short-host-call-active";
    case Reason::HostReaderOwnershipMismatch: return "host-reader-ownership-mismatch";
    case Reason::NestedExclusiveMutation: return "nested-exclusive-mutation";
    case Reason::CompetingExclusiveMutation: return "competing-exclusive-mutation";
    case Reason::PendingMutationTimeout: return "pending-mutation-timeout";
    case Reason::RetainedExecutionTimeout: return "retained-execution-timeout";
    case Reason::NativeDataLeaseTimeout: return "native-data-lease-timeout";
    case Reason::ArmSafepointsFailed: return "arm-safepoints-failed";
    case Reason::RequestEpochChanged: return "request-epoch-changed";
    case Reason::QuiescenceTimeout: return "quiescence-timeout";
    case Reason::ExecutionUpgradeMismatch: return "execution-upgrade-mismatch";
    case Reason::HostCallUpgradeMismatch: return "host-call-upgrade-mismatch";
    }
    return "unknown";
}

void ReportGuestBarrierFailure(const char* site, const ActiveRun& run,
    const GuestMemoryMutationBarrier::BarrierFailureDiagnostic& diagnostic,
    std::uint64_t hostTarget, std::uint64_t guestRip) noexcept {
    using Phase = GuestMemoryMutationBarrier::BarrierFailurePhase;
    const char* phase = "uncaptured";
    switch (diagnostic.phase) {
    case Phase::None: break;
    case Phase::ExecutionEntry: phase = "execution-entry"; break;
    case Phase::Checkpoint: phase = "checkpoint"; break;
    case Phase::HostCallSuspend: phase = "host-call-suspend"; break;
    case Phase::HostCallResume: phase = "host-call-resume"; break;
    }
    // The POD was captured at the failed transition under the barrier mutex.
    // Reporting happens only after that method returned and released its lock.
    if (guestRip == 0 && run.thread != nullptr && run.thread->CurrentFrame != nullptr)
        guestRip = run.thread->CurrentFrame->State.rip;
    __android_log_print(ANDROID_LOG_ERROR, "LumenFexCoreRuntime",
        "guest barrier failure site=%s phase=%s runId=%llu parentRunId=%llu hostThread=%llu "
        "guestRip=0x%llx hostTarget=0x%llx "
        "executionDepth=%u hostCallDepth=%u ownedHostReaders=%u retainedDepth=%u guestDataLeases=%u "
        "mutationDepth=%u mutationScope=%u publishing=%u executionOwner=%u hostCallOwner=%u "
        "mutationOwner=%u requestOwner=%u hostWaitOwner=%u hostWait=%u executionCounted=%u "
        "mutationEpoch=%llu ownerRequestEpoch=%llu readers=%zu hostReaders=%zu retainedExecutions=%zu "
        "parked=%zu waiting=%zu requestEpoch=%llu requestScope=%u writer=%u fullIntent=%u",
        site, phase, static_cast<unsigned long long>(run.runId),
        static_cast<unsigned long long>(run.parentRun != nullptr ? run.parentRun->runId : 0),
        static_cast<unsigned long long>(HostThreadIdentity()),
        static_cast<unsigned long long>(guestRip),
        static_cast<unsigned long long>(hostTarget),
        diagnostic.executionDepth, diagnostic.hostCallDepth,
        diagnostic.hostCallReadersOwned, diagnostic.retainedExecutionDepth,
        diagnostic.guestDataLeases, diagnostic.mutationDepth,
        static_cast<unsigned>(diagnostic.mutationScope), diagnostic.mutationPublishing ? 1U : 0U,
        diagnostic.executionOwnerMatches ? 1U : 0U, diagnostic.hostCallOwnerMatches ? 1U : 0U,
        diagnostic.mutationOwnerMatches ? 1U : 0U, diagnostic.requestOwnerMatches ? 1U : 0U,
        diagnostic.hostWaitOwnerMatches ? 1U : 0U, diagnostic.hostWaitActive ? 1U : 0U,
        diagnostic.executionCounted ? 1U : 0U,
        static_cast<unsigned long long>(diagnostic.mutationEpoch),
        static_cast<unsigned long long>(diagnostic.ownerRequestEpoch),
        diagnostic.readers, diagnostic.hostCallReaders, diagnostic.retainedExecutions,
        diagnostic.parked, diagnostic.waitingEntrants,
        static_cast<unsigned long long>(diagnostic.requestEpoch),
        static_cast<unsigned>(diagnostic.requestScope), diagnostic.writer ? 1U : 0U,
        diagnostic.fullIntent ? 1U : 0U);
}

void ReportObservedHostCallsAfterMutationDenial(
    const GuestMemoryMutationBarrier::MutationDenialDiagnostic& diagnostic) noexcept {
    if (diagnostic.hostCallReaders == 0) return;
    static std::atomic<unsigned> reports {};
    auto report = reports.load(std::memory_order_relaxed);
    while (report < 8 && !reports.compare_exchange_weak(report, report + 1,
        std::memory_order_relaxed, std::memory_order_relaxed)) {}
    if (report >= 8) return;
    struct Observation {
        std::uint64_t runId, parentRunId, hostThread, nativeThreadId;
        std::uint64_t target, hostWait, freshCallback;
        bool targetStable;
    };
    std::array<Observation, 64> observations {};
    std::size_t count = 0, activeCount = 0;
    {
        // Registry ownership keeps every run and its immutable identity live.
        // Only dedicated atomic observations may be read across host threads.
        std::lock_guard lock(g_runtime.activeThreadsMutex);
        activeCount = g_runtime.activeRuns.size();
        for (const auto* run : g_runtime.activeRuns) {
            if (run == nullptr || count == observations.size()) continue;
            const auto target = run->observedHostCallTarget.load(std::memory_order_acquire);
            const auto wait = run->observedHostWaitToken.load(std::memory_order_acquire);
            const auto fresh = run->observedFreshCallbackToken.load(std::memory_order_acquire);
            observations[count++] = {run->runId,
                run->parentRun != nullptr ? run->parentRun->runId : 0,
                run->hostThread, run->nativeThreadId, target, wait, fresh,
                target == run->observedHostCallTarget.load(std::memory_order_acquire)};
        }
    }
    // BeginMutation already canceled the request. These observations can race
    // with readmission and are not an authoritative census of barrier readers.
    // Neither symbol lookup nor logging holds the registry or barrier lock.
    __android_log_print(ANDROID_LOG_ERROR, "LumenFexCoreRuntime",
        "LUMEN_FEX_MUTATION_HOST_CALLS_V1 phase=observed-after-denial requestEpoch=%llu "
        "deniedHostReaders=%zu activeRuns=%zu copied=%zu truncated=%u report=%u limit=8",
        static_cast<unsigned long long>(diagnostic.requestEpoch), diagnostic.hostCallReaders,
        activeCount, count, activeCount > observations.size() ? 1U : 0U, report + 1);
    for (std::size_t index = 0; index != count; ++index) {
        const auto& value = observations[index];
        Dl_info symbol {};
        const bool resolved = value.target != 0 &&
            dladdr(reinterpret_cast<const void*>(value.target), &symbol) != 0;
        const auto base = resolved ? reinterpret_cast<std::uintptr_t>(symbol.dli_fbase) : 0;
        __android_log_print(ANDROID_LOG_ERROR, "LumenFexCoreRuntime",
            "LUMEN_FEX_MUTATION_HOST_CALL_V1 phase=observed-after-denial requestEpoch=%llu "
            "runId=%llu parentRunId=%llu hostThread=%llu nativeTid=%llu target=0x%llx "
            "moduleOffset=0x%llx symbol=%s targetStable=%u hostWaitToken=%llu freshCallbackToken=%llu "
            "candidate=%u index=%zu",
            static_cast<unsigned long long>(diagnostic.requestEpoch),
            static_cast<unsigned long long>(value.runId), static_cast<unsigned long long>(value.parentRunId),
            static_cast<unsigned long long>(value.hostThread), static_cast<unsigned long long>(value.nativeThreadId),
            static_cast<unsigned long long>(value.target),
            static_cast<unsigned long long>(base != 0 && value.target >= base ? value.target - base : 0),
            resolved && symbol.dli_sname != nullptr ? symbol.dli_sname : "unresolved",
            value.targetStable ? 1U : 0U, static_cast<unsigned long long>(value.hostWait),
            static_cast<unsigned long long>(value.freshCallback),
            value.targetStable && value.target != 0 && value.hostWait == 0 && value.freshCallback == 0 ? 1U : 0U,
            index);
    }
}

std::uint64_t BeginGuestMemoryMutation(
    GuestMemoryMutationScope scope, std::uint32_t* retryable = nullptr) noexcept {
    if (retryable != nullptr) *retryable = 0;
    const auto started = GuestMemoryMutationBarrier::Clock::now();
    GuestMemoryMutationBarrier::MutationDenialDiagnostic diagnostic {};
    const std::uint64_t token = GuestMutationBarrier().BeginMutation(
        scope, &ArmActiveGuestSafepoints,
        &DisarmActiveGuestSafepoints,
        kGuestMutationSafepointTimeout, &diagnostic);
    if (token == 0) {
        // CompetingExclusiveMutation is captured under the barrier mutex,
        // before any new request, mapping change or safepoint is published.
        // Its preceding checks exclude native pins and foreign barriers. Do
        // not turn nested HLE, retained callbacks, or permanent denial into
        // permission to yield somebody else's guest-memory reader.
        if (retryable != nullptr &&
            GuestMemoryMutationBarrier::MutationDenialAllowsQuiescentRetry(diagnostic) &&
            g_activeRun != nullptr && g_activeRun->thread != nullptr && g_activeRun->parentRun == nullptr &&
            g_activeRun->activeHostCallTarget != 0) *retryable = 1;
        std::size_t activeRuns = 0;
        {
            std::lock_guard threadsLock(g_runtime.activeThreadsMutex);
            activeRuns = g_runtime.activeRuns.size();
        }
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                GuestMemoryMutationBarrier::Clock::now() - started);
        __android_log_print(
            ANDROID_LOG_ERROR, "LumenFexCoreRuntime",
            "guest memory mutation denied scope=%s reason=%s elapsedMs=%lld "
            "executionDepth=%u hostCallDepth=%u ownedHostReaders=%u retainedDepth=%u "
            "guestDataLeases=%u mutationDepth=%u mutationScope=%u publishing=%u mutationOwner=%u "
            "hostWait=%u executionCounted=%u readers=%zu hostReaders=%zu retainedExecutions=%zu "
            "parked=%zu waiting=%zu requestEpoch=%llu requestScope=%u writer=%u fullIntent=%u "
            "activeRuns=%zu callerInGuest=%u hostThread=%llu runId=%llu hostTarget=0x%llx monotonicMs=%lld",
            scope == GuestMemoryMutationScope::AdditiveMapping
                ? "additive"
                : "exclusive",
            MutationDenialReasonName(diagnostic.reason),
            static_cast<long long>(elapsed.count()),
            diagnostic.executionDepth, diagnostic.hostCallDepth,
            diagnostic.hostCallReadersOwned, diagnostic.retainedExecutionDepth,
            diagnostic.guestDataLeases, diagnostic.mutationDepth,
            static_cast<unsigned>(diagnostic.mutationScope),
            diagnostic.mutationPublishing ? 1U : 0U,
            diagnostic.mutationOwnerMatches ? 1U : 0U,
            diagnostic.hostWaitActive ? 1U : 0U,
            diagnostic.executionCounted ? 1U : 0U,
            diagnostic.readers, diagnostic.hostCallReaders,
            diagnostic.retainedExecutions, diagnostic.parked,
            diagnostic.waitingEntrants,
            static_cast<unsigned long long>(diagnostic.requestEpoch),
            static_cast<unsigned>(diagnostic.requestScope),
            diagnostic.writer ? 1U : 0U, diagnostic.fullIntent ? 1U : 0U,
            activeRuns,
            g_activeRun != nullptr ? 1U : 0U,
            static_cast<unsigned long long>(HostThreadIdentity()),
            static_cast<unsigned long long>(g_activeRun != nullptr ? g_activeRun->runId : 0),
            static_cast<unsigned long long>(g_activeRun != nullptr ? g_activeRun->activeHostCallTarget : 0),
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                GuestMemoryMutationBarrier::Clock::now().time_since_epoch()).count()));
        for (std::size_t index = 0; index != diagnostic.hostReadersCaptured; ++index) {
            const auto& reader = diagnostic.hostReadersAtDenial[index];
            __android_log_print(ANDROID_LOG_ERROR, "LumenFexCoreRuntime",
                "LUMEN_FEX_MUTATION_READER_V1 phase=at-denial requestEpoch=%llu "
                "nativeTid=%llu operation=%llu name=%s ownedHostReaders=%u "
                "identityStable=%u index=%zu truncated=%u",
                static_cast<unsigned long long>(diagnostic.requestEpoch),
                static_cast<unsigned long long>(reader.nativeThread),
                static_cast<unsigned long long>(reader.operation), reader.name.data(),
                reader.readersOwned, reader.identityStable ? 1U : 0U, index,
                diagnostic.hostReadersTruncated ? 1U : 0U);
        }
        ReportObservedHostCallsAfterMutationDenial(diagnostic);
    }
    return token;
}

} // namespace

#if defined(__ANDROID__) || defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
extern "C" void FEXLumenJitCacheActivityV1(std::uint32_t kind,
    std::uint64_t value, std::uint64_t detail) noexcept {
    Lumen::Performance::JitCacheEvent(kind, value, detail);
}
#endif

#if defined(__ANDROID__) || defined(LUMEN_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED)
// Named native-cache bindings: the exclusive-publication contract emits no
// retirement checks. Interval handoff readers use these before another writer
// is allowed to re-arm a newly admitted CPU's safepoint doorbell.
extern "C" const std::uint32_t* FEXLumenGuestProgressFlagProviderV1() noexcept {
    static const auto* const flag = [] {
        const auto* result = MemoryHandoffInstrumentationEnabled()
            ? GuestMutationBarrier().EnableGuestProgressWitness() : nullptr;
        std::fprintf(stderr, "LUMEN_FEX_GUEST_PROGRESS_V1 enabled=%u witness=retired-work cache_domain=2\n",
            result != nullptr ? 1U : 0U);
        return result;
    }();
    static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));
    return reinterpret_cast<const std::uint32_t*>(flag);
}
extern "C" void FEXLumenGuestProgressV1() noexcept {
    GuestMutationBarrier().ReportGuestProgress();
}
extern "C" auto FEXLumenGuestProgressCallbackProviderV1() noexcept -> void (*)() noexcept {
    return MemoryHandoffInstrumentationEnabled() ? FEXLumenGuestProgressV1 : nullptr;
}

using DirectMemoryHandoffCheck = std::uint32_t (*)(std::uint64_t,
    std::uint64_t, std::uint32_t, std::uint64_t) noexcept;
extern "C" DirectMemoryHandoffCheck FEXLumenGuestMemoryCheckProviderV1() noexcept {
    return MemoryHandoffInstrumentationEnabled() ? DirectMemoryHandoff : nullptr;
}

extern "C" FEXLumenMemoryPreserveAllCheckV1 FEXLumenGuestMemoryPreserveAllProviderV1() noexcept {
#if FEX_LUMEN_HAS_MEMORY_PRESERVE_ALL_ABI
    return MemoryHandoffInstrumentationEnabled() ? DirectMemoryHandoffPreserveAll : nullptr;
#else
    return nullptr;
#endif
}



using DirectMemoryBulkCheck = std::uint32_t (*)(std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t) noexcept;
extern "C" DirectMemoryBulkCheck FEXLumenGuestMemoryBulkProviderV1() noexcept {
    return MemoryHandoffInstrumentationEnabled() ? DirectUnwatchedMemoryBulk : nullptr;
}

extern "C" const std::uint8_t* FEXLumenGuestMemoryHandoffGuardProviderV1() noexcept {
    static_assert(sizeof(std::atomic<std::uint8_t>) == sizeof(std::uint8_t));
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
    static_assert(Lumen::GuestCpu::MemoryHandoffPageShift == 12);
    static_assert(Lumen::GuestCpu::MemoryHandoffGuardCount == (1U << 20));
    return MemoryHandoffInstrumentationEnabled() ? Lumen::GuestCpu::MemoryHandoffGuardTable() : nullptr;
}

extern "C" FEXLumenMemoryReadGuardDirectoryV1 FEXLumenGuestMemoryReadGuardProviderV1() noexcept {
    // The JIT loads this process-lifetime atomic pointer with acquire. A null
    // value is the tracker's complete clean-read proof, never write admission.
    return MemoryHandoffInstrumentationEnabled() ? Lumen::GuestCpu::MemoryHandoffReadGuardDirectory() : nullptr;
}
#endif

extern "C" std::uint32_t LumenAndroidFexCoreConfigureMemoryHandoffV1(std::uint32_t mode) noexcept {
#if defined(LUMEN_PS5_FEX_C13)
    if (mode != LUMEN_FEX_MEMORY_EXCLUSIVE_PUBLICATION_V1) return 0;
#endif
#if defined(__ANDROID__) || defined(LUMEN_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED)
    if (mode != LUMEN_FEX_MEMORY_EXCLUSIVE_PUBLICATION_V1 && mode != LUMEN_FEX_MEMORY_INTERVAL_HANDOFF_V1) return 0;
    return Lumen::GuestCpu::ConfigureMemoryHandoffRegistration(mode == LUMEN_FEX_MEMORY_INTERVAL_HANDOFF_V1) ? 1U : 0U;
#else
    (void)mode;
    return 0;
#endif
}

extern "C" std::uint32_t
LumenAndroidFexCoreMemoryHandoffInstrumentationReadyV1() noexcept {
#if (defined(__ANDROID__) && defined(LUMEN_ANDROID_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED) && \
     LUMEN_ANDROID_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED == 1) || \
    (defined(__APPLE__) && defined(LUMEN_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED) && \
     LUMEN_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED == 1)
    // The pinned overlay supplies code generation; all associated process
    // providers must also resolve to the live shared ownership metadata.
    return FEXLumenGuestMemoryCheckProviderV1() != nullptr &&
        FEXLumenGuestMemoryBulkProviderV1() != nullptr &&
        FEXLumenGuestMemoryHandoffGuardProviderV1() == Lumen::GuestCpu::MemoryHandoffGuardTable() &&
        FEXLumenGuestMemoryReadGuardProviderV1() == Lumen::GuestCpu::MemoryHandoffReadGuardDirectory()
#if FEX_LUMEN_HAS_MEMORY_PRESERVE_ALL_ABI
        && FEXLumenGuestMemoryPreserveAllProviderV1() != nullptr
#endif
        ? 1U : 0U;
#else
    return 0;
#endif
}

bool Lumen::GuestCpu::OwnsShortHostCallAdmission() noexcept {
    return Selected() == Provider::Fex && GuestMutationBarrier().CurrentThreadOwnsShortHostCallLease();
}
bool Lumen::GuestCpu::EnsureOrdinaryHostCallAdmission() noexcept {
    if (Selected() != Provider::Fex) return true;
    auto& barrier = GuestMutationBarrier();
    const bool wasShort = barrier.CurrentThreadOwnsShortHostCallLease();
    if (!barrier.EnsureOrdinaryHostCallAdmission()) return false;
    if (wasShort && g_activeRun != nullptr) {
        auto& run = *g_activeRun;
        run.activeShortDemoted = true;
        if (run.activeSyncMemoryShort) ++run.syncMemoryShortDemoted;
        if (run.hleWindow) run.hleWindow->Demote(run.activeHleTraceSlot);
    }
    return true;
}

extern "C" std::uint32_t
LumenAndroidFexCoreOwnsGuestMemoryReadLeaseV1() noexcept {
    const auto& barrier = GuestMutationBarrier();
    return barrier.CurrentThreadOwnsExecutionLease() ||
        barrier.CurrentThreadOwnsHostMemoryReadLease() ? 1U : 0U;
}

extern "C" std::uint64_t
LumenAndroidFexCoreGuestExecutionReadAdmissionEpochV1() noexcept {
    return GuestMutationBarrier().CurrentThreadExecutionReadAdmissionEpoch();
}

#if defined(__APPLE__)
extern "C" __attribute__((noinline)) std::uint32_t
FEXLumenDirectUnalignedTsoEnabledV1() noexcept {
    return DirectUnalignedTsoEnabled() ? 1U : 0U;
}
#endif

// Exercises production publication/snapshot helpers without touching the
// global runtime, registering Custom-IR, or executing synthetic addresses.
extern "C" std::uint32_t LumenAndroidFexCoreHostTargetSnapshotContractTestV1() noexcept {
    std::vector<std::uint64_t> registry;
    std::mutex mutex;
    unsigned active = 0;
    unsigned ownerCalls = 0;
    const auto publish = [&](std::uint64_t target, bool ownerAccepts = true) {
        std::lock_guard lock(mutex);
        return PublishHostTarget(registry, target, active != 0, [&] {
            ++ownerCalls;
            return ownerAccepts;
        });
    };
    const HostTargetSnapshot empty(registry);
    if (empty.Contains(0) || empty.Contains(UINT64_MAX) || empty.Contains(0x1000) ||
        !empty.RegistrationOrder().empty()) return 0;
    // Exercise full-key collisions in the production selected lookup, without
    // registering or executing these synthetic identities in the real runtime.
    std::vector<std::uint64_t> collidingTargets;
    std::uint64_t candidate = 1;
    while (collidingTargets.size() != 16) {
        if ((Lumen::GuestCpu::HostCallFastIndex::Hash(candidate) & 63) == 0)
            collidingTargets.push_back(candidate);
        ++candidate;
    }
    while ((Lumen::GuestCpu::HostCallFastIndex::Hash(candidate) & 63) != 0) ++candidate;
    const HostTargetSnapshot collisions(collidingTargets);
    for (const auto target: collidingTargets) if (!collisions.Contains(target)) return 0;
    collidingTargets.assign(1024, candidate);
    if (collisions.Contains(candidate) || collisions.Contains(0) || collisions.Contains(UINT64_MAX) ||
        collisions.RegistrationOrder().size() != 16) return 0;
    if (publish(0) != HostTargetPublication::Rejected ||
        publish(UINT64_MAX) != HostTargetPublication::Rejected || ownerCalls != 0 ||
        publish(0x3000) != HostTargetPublication::Added ||
        publish(0x1000) != HostTargetPublication::Added ||
        publish(0x2000) != HostTargetPublication::Added || ownerCalls != 3 ||
        publish(0x1000) != HostTargetPublication::Existing || ownerCalls != 3 ||
        publish(0x4000, false) != HostTargetPublication::Rejected || ownerCalls != 4 ||
        registry != std::vector<std::uint64_t>{0x3000, 0x1000, 0x2000}) return 0;

    std::optional<HostTargetSnapshot> outer;
    std::optional<HostTargetSnapshot> nested;
    const auto admit = [&](std::optional<HostTargetSnapshot>& snapshot) {
        std::lock_guard lock(mutex);
        snapshot.emplace(registry);
        ++active;
    };
    const auto leave = [&] {
        std::lock_guard lock(mutex);
        --active;
    };
    admit(outer);
    admit(nested);
    const auto exact = [](const HostTargetSnapshot& snapshot) {
        return snapshot.Contains(0x1000) && snapshot.Contains(0x2000) && snapshot.Contains(0x3000) &&
            !snapshot.Contains(0) && !snapshot.Contains(UINT64_MAX) &&
            !snapshot.Contains(0x0fff) && !snapshot.Contains(0x1001) && !snapshot.Contains(0x4000) &&
            snapshot.RegistrationOrder() == std::vector<std::uint64_t>{0x3000, 0x1000, 0x2000};
    };
    if (!exact(*outer) || !exact(*nested) ||
        publish(0x1000) != HostTargetPublication::Rejected ||
        publish(0x4000) != HostTargetPublication::Rejected || ownerCalls != 4) return 0;
    leave(); // Model a nested fault/STOP releasing only its own admission.
    if (publish(0x4000) != HostTargetPublication::Rejected || ownerCalls != 4 || !exact(*outer)) return 0;
    leave();

    // Retain old owned snapshots across the next registration/admission.
    // Concurrent readers must not borrow registry storage or gain new authority.
    std::atomic<unsigned> ready {0};
    std::atomic<bool> published {false};
    std::atomic<bool> valid {true};
    std::array<std::thread, 2> readers;
    for (auto& reader: readers) {
        reader = std::thread([&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            ready.fetch_add(1);
            do {
                if (!exact(*outer) || !exact(*nested)) valid.store(false);
                std::this_thread::yield();
            } while (!published.load() && std::chrono::steady_clock::now() < deadline);
            if (!published.load()) valid.store(false);
            for (unsigned repeat = 0; repeat != 1024; ++repeat) {
                if (!exact(*outer) || !exact(*nested)) valid.store(false);
            }
        });
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (ready.load() != readers.size() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    if (ready.load() != readers.size() || publish(0x4000) != HostTargetPublication::Added || ownerCalls != 5) valid.store(false);
    std::optional<HostTargetSnapshot> next;
    admit(next);
    published.store(true);
    for (auto& reader: readers) reader.join();
    if (!valid.load() || !exact(*outer) || !exact(*nested) || !next->Contains(0x4000) ||
        next->Contains(0x4001) || next->RegistrationOrder() != registry ||
        next->RegistrationOrder() != std::vector<std::uint64_t>{0x3000, 0x1000, 0x2000, 0x4000} ||
        publish(0x4000) != HostTargetPublication::Rejected || ownerCalls != 5) return 0;
    leave();
    return active == 0 ? 1u : 0u;
}

extern "C" std::uint32_t LumenAndroidFexCoreRuntimeProbeV1(
    LumenAndroidFexCoreRuntimeStatusV1* status) noexcept {
    EnsureRuntime();
    if (status != nullptr) {
        std::lock_guard targetLock(g_runtime.targetMutex);
        *status = g_runtime.status;
    }
    return g_runtime.status.code;
}

extern "C" std::uint32_t LumenAndroidFexCoreQueryGuestTscV1(
    LumenAndroidFexCoreGuestTscV1* output) noexcept {
    if (output == nullptr || output->structSize != sizeof(*output) ||
        output->abiVersion != 1 || output->reserved != 0) {
        return LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1;
    }
    *output = {};
    const auto frequency = g_runtime.guestTscFrequency.load(std::memory_order_acquire);
    if (frequency == 0) return LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1;
    const auto scale = g_runtime.guestTscScale.load(std::memory_order_relaxed);
    std::uint64_t nativeFrequency {}, counter {};
    asm volatile("mrs %0, cntfrq_el0" : "=r"(nativeFrequency));
    if (scale > 31 || nativeFrequency == 0 ||
        nativeFrequency > (UINT64_MAX >> scale) ||
        (nativeFrequency << scale) != frequency) {
        return LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
    }
    // Preserve the same hardware origin and modulo-64-bit scaling as FEX's
    // instruction lowering. Ordering the read is safe for either x86 query.
    asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(counter));
    output->counter = counter << scale;
    output->frequency = frequency;
    output->nativeFrequency = nativeFrequency;
    output->scale = scale;
    return LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1;
}

extern "C" std::uint32_t LumenAndroidFexCoreFpBoundaryProbeV1(
    LumenAndroidFexCoreFpBoundaryProbeStatusV1* output) noexcept {
    if (output == nullptr || output->structSize != sizeof(*output) || output->abiVersion != 1 ||
        g_activeRun != nullptr) return 0;
    *output = {};
    FpProbeNativeEnvironment callerEnvironment;
    if (!callerEnvironment.Valid()) return 0;
    EnsureRuntime();
    output->runtimeCode = g_runtime.status.code;
    if (output->runtimeCode != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) return 0;

    FpProbeState probe;
    probe.codeSize = HostPageSize();
    probe.dataSize = 2 * HostPageSize();
    probe.target = reinterpret_cast<std::uintptr_t>(&FpProbeTargetIdentity);
    if (!RegisterHostTargetInternal(probe.target)) return 0;
    void* const code = mmap(nullptr, probe.codeSize, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) return 0;
    void* const data = mmap(nullptr, probe.dataSize, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED) { munmap(code, probe.codeSize); return 0; }
    probe.codeBase = reinterpret_cast<std::uintptr_t>(code);
    probe.dataBase = reinterpret_cast<std::uintptr_t>(data);
    namespace Programs = Lumen::FexCore::FpBoundaryProbe;
    std::memcpy(code, Programs::RootConversion.data(), Programs::RootConversion.size());
    std::memcpy(static_cast<std::uint8_t*>(code) + 64,
                Programs::AcrossHostCall.data(), Programs::AcrossHostCall.size());
    if (mprotect(code, probe.codeSize, PROT_READ) != 0) {
        munmap(code, probe.codeSize); munmap(data, probe.dataSize); return 0;
    }
    auto* const words = static_cast<std::uint32_t*>(data);
    words[0] = Programs::InputBits;
    words[1] = Programs::GuestDownwardMxcsr;
    const auto resetResults = [&]() { std::memset(words + 8, 0, 48); probe.calls = 0; };
    bool executed = std::fesetround(FE_DOWNWARD) == 0;
    std::uint32_t codeResult = executed
        ? RunFpProbeProgram(probe, false, false)
        : LUMEN_ANDROID_FEXCORE_RUNTIME_THREAD_SETUP_FAILED_V1;
    executed = codeResult == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1;
    if (executed) {
        ++output->executedCases;
        output->rootResult = static_cast<std::int32_t>(words[8]);
        output->rootMxcsr = words[9];
        output->rootReturnRounding = std::fegetround();
        if (output->rootResult == 2 && (output->rootMxcsr & 0xffc0U) == Programs::DefaultMxcsr)
            output->conformanceMask |= 1;
    }
    resetResults();
    probe.action = FpProbeHostAction::ChangeRounding;
    if (executed && std::fesetround(FE_TOWARDZERO) == 0) {
        codeResult = RunFpProbeProgram(probe, true, false);
        executed = codeResult == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 && probe.calls == 1;
        if (executed) {
            ++output->executedCases;
            output->hleBefore = static_cast<std::int32_t>(words[8]);
            output->hleMxcsrBefore = words[9];
            output->hleAfter = static_cast<std::int32_t>(words[10]);
            output->hleMxcsrAfter = words[11];
            output->hleEntryRounding = probe.entryRounding;
            output->hleReturnRounding = std::fegetround();
            if (output->hleBefore == 1 && output->hleAfter == 1 &&
                (output->hleMxcsrBefore & 0xffc0U) == Programs::GuestDownwardMxcsr &&
                (output->hleMxcsrAfter & 0xffc0U) == Programs::GuestDownwardMxcsr)
                output->conformanceMask |= 2;
        }
    } else executed = false;
    resetResults();
    probe.action = FpProbeHostAction::NestedRoot;
    if (executed && std::fesetround(FE_TOWARDZERO) == 0) {
        codeResult = RunFpProbeProgram(probe, true, false);
        executed = codeResult == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 && probe.calls == 1 &&
            probe.nestedCode == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1;
        if (executed) {
            ++output->executedCases;
            output->nestedOuterBefore = static_cast<std::int32_t>(words[8]);
            output->nestedOuterMxcsrBefore = words[9];
            output->nestedOuterAfter = static_cast<std::int32_t>(words[10]);
            output->nestedOuterMxcsrAfter = words[11];
            output->nestedResult = static_cast<std::int32_t>(words[16]);
            output->nestedMxcsr = words[17];
            output->nestedEntryRounding = probe.entryRounding;
            output->nestedReturnRounding = std::fegetround();
            if (output->nestedResult == 2 && output->nestedOuterBefore == 1 &&
                output->nestedOuterAfter == 1 && (output->nestedMxcsr & 0xffc0U) == Programs::DefaultMxcsr &&
                (output->nestedOuterMxcsrBefore & 0xffc0U) == Programs::GuestDownwardMxcsr &&
                (output->nestedOuterMxcsrAfter & 0xffc0U) == Programs::GuestDownwardMxcsr)
                output->conformanceMask |= 4;
        }
    } else executed = false;
    if (executed && output->hleEntryRounding == FE_TOWARDZERO &&
        output->nestedEntryRounding == FE_TOWARDZERO) output->conformanceMask |= 8;
    if (executed && output->rootReturnRounding == FE_DOWNWARD &&
        output->hleReturnRounding == FE_TOWARDZERO && output->nestedReturnRounding == FE_TOWARDZERO)
        output->conformanceMask |= 16;
    output->runtimeCode = codeResult;
    const bool closed = CloseFpProbeMappings(probe);
    const bool restored = callerEnvironment.Restore();
    return executed && closed && restored ? 1U : 0U;
}

extern "C" const char* LumenAndroidFexCoreRuntimeReceiptV1() noexcept {
    EnsureRuntime();
    if (g_runtime.status.code != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) {
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
        return "LUMEN_MACOS_FEXCORE_RUNTIME_V1 ready=0 linked=1 "
               "failClosed=1 pin=c9c5a132 fallback=0 nodaRoute=0";
#elif defined(LUMEN_PS5_FEX_C13)
        return "LUMEN_ANDROID_FEXCORE_RUNTIME_V1 ready=0 linked=1 "
               "failClosed=1 pin=f2b679f6 optIn=ps5-c13 fallback=0";
#else
        return "LUMEN_ANDROID_FEXCORE_RUNTIME_V1 ready=0 linked=1 "
               "failClosed=1 pin=e17fdf9";
#endif
    }
    static const auto receipt = [] {
        std::array<char, 1536> text {};
        const int written = std::snprintf(text.data(), text.size(),
#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
          "LUMEN_MACOS_FEXCORE_RUNTIME_V1 ready=1 linked=1 "
          "context=1 signalStop=1 hostBridge=1 backendRoute=1 "
          "syntheticGuestExecuted=1 opt=O2 maxInst=%u smc=mtrack-rx "
          "monoHacks=0 mutationBarrier=1 mutationSafepoint=1 "
          "mutationScopes=additive-exclusive "
          "customIRPrecompile=1 customIRRegenerate=1 rolloverProbe=1 "
          "unalignedAtomicProbe=cmpxchg64-signal-emulated "
          "guestStateBridge=v1-gpr-v3-checkpoint hleStateBridge=gpr-sse128 "
          "rootStateV3=ymm-mxcsr-x87-flags rootMxcsrBootstrap=ldmxcsr "
          "rootDazFtz=paired-or-afp generalDazFtz=unqualified "
          "hostFpBoundary=isolated varargsAl=preserved "
          "scalarHostCallV2=darwin-sse128 scalarMaxArity=16 "
          "logicalCores=host mapJit=1 threadWx=1 "
          "runDefault=1 enableSelector=absent-or-exact-one "
          "denialSelector=non-exact-one "
          "jitPublisher=thread-toggle-diagnostic qualification=diagnostic "
          "nonPromotable=1 "
          "pin=c9c5a132 upstream=e17fdf9 fallback=0 nodaRoute=0",
#elif defined(LUMEN_PS5_FEX_C13)
          "LUMEN_ANDROID_FEXCORE_RUNTIME_V1 ready=1 linked=1 "
          "context=1 signalStop=1 hostBridge=1 backendRoute=1 "
          "guestExecuted=1 opt=O2 maxInst=%u smc=mtrack-rx "
          "monoHacks=0 mutationBarrier=1 mutationSafepoint=1 "
          "mutationScopes=additive-exclusive "
          "customIRPrecompile=1 customIRRegenerate=1 rolloverProbe=1 "
          "logicalCores=host wx=rwx-runtime-probed "
          "cpuGpuHandoff=exclusive-publication intervalInstrumentation=0 "
          "hostPages=4096 guestPage=4096 hostPagePort=0 diskCache=unsupported "
          "pin=f2b679f6 optIn=ps5-c13 fallback=0",
#else
          "LUMEN_ANDROID_FEXCORE_RUNTIME_V1 ready=1 linked=1 "
          "context=1 signalStop=1 hostBridge=1 backendRoute=1 "
          "guestExecuted=1 opt=O2 maxInst=%u smc=mtrack-rx "
          "monoHacks=0 mutationBarrier=1 mutationSafepoint=1 "
          "mutationScopes=additive-exclusive "
          "customIRPrecompile=1 customIRRegenerate=1 rolloverProbe=1 "
          "logicalCores=host wx=rwx-runtime-probed "
          "cpuGpuHandoff=interval-alias directMemory=inline-page-guard "
          "hostPages=4096,16384 guestPage=4096 hostPagePort=1 "
          "pin=e17fdf9",
#endif
          g_runtime.maximumBlockInstructions);
        if (written <= 0 || static_cast<std::size_t>(written) >= text.size()) {
            std::snprintf(text.data(), text.size(),
                "LUMEN_FEXCORE_RUNTIME_V1 ready=0 report=overflow");
        }
        return text;
    }();
    return receipt.data();
}

extern "C" std::uint32_t
LumenAndroidFexCoreLastSafepointSignalCodeV1() noexcept {
#if defined(__ANDROID__)
    return g_lastAndroidSafepointSignalCode.load(
        std::memory_order_relaxed);
#else
    return 0;
#endif
}

#if defined(__APPLE__)
extern "C" std::uint32_t LumenMacOSFexCoreRegisterDataAccessQueryV1(
    LumenMacOSFexCoreDataAccessQueryV1 query) noexcept {
    if (query == nullptr) return 0;
    LumenMacOSFexCoreDataAccessQueryV1 expected = nullptr;
    return g_dataAccessQuery.compare_exchange_strong(expected, query,
               std::memory_order_release, std::memory_order_acquire) || expected == query
        ? 1U : 0U;
}
#endif

extern "C" std::uint32_t LumenAndroidFexCoreRegisterHostCallTargetV1(
    std::uint64_t target) noexcept {
    EnsureRuntime();
    if (g_runtime.status.code !=
        LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) {
        return 0;
    }
    return RegisterHostTargetInternal(target) ? 1u : 0u;
}

extern "C" std::uint64_t
LumenAndroidFexCoreRootReturnAddressV1() noexcept {
    EnsureRuntime();
    if (g_runtime.status.code !=
            LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 ||
        g_runtime.bridgePage == nullptr) {
        return 0;
    }
    return reinterpret_cast<std::uintptr_t>(g_runtime.bridgePage) +
        kHltOffset;
}

extern "C" std::uint32_t LumenAndroidFexCoreRunGuestV1(
    LumenAndroidFexCoreGuestStateV1* state,
    LumenAndroidFexCoreHostCallV1 hostCall,
    LumenAndroidFexCoreExecutableQueryV1 executableQuery,
    void* opaque) noexcept {
    EnsureRuntime();
    if (g_runtime.status.code !=
        LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) {
        return g_runtime.status.code;
    }
    if (hostCall == nullptr || executableQuery == nullptr) {
        return LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1;
    }
    return RunGuestInternal(
        state, hostCall, executableQuery, opaque, false);
}

extern "C" std::uint32_t LumenAndroidFexCoreRunGuestV2(
    LumenAndroidFexCoreGuestStateV1* state,
    LumenAndroidFexCoreHostCallV2 hostCall,
    LumenAndroidFexCoreExecutableQueryV1 executableQuery,
    void* opaque) noexcept {
    EnsureRuntime();
    if (g_runtime.status.code != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) {
        return g_runtime.status.code;
    }
    if (hostCall == nullptr || executableQuery == nullptr) {
        return LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1;
    }
    if (!HasScalarSseView(g_runtime.context.get())) {
        return LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
    }
    return RunGuestInternal(state, nullptr, executableQuery, opaque, false, hostCall);
}

static std::uint32_t RunGuestCheckpoint(
    LumenAndroidFexCoreGuestStateV3* state,
    LumenAndroidFexCoreHostCallV2 hostCall,
    LumenAndroidFexCoreExecutableQueryV1 executableQuery,
    void* opaque, std::uint64_t ownedStackBase, std::uint64_t ownedStackSize,
    LumenMacOSFexCoreOwnedStackAccessQueryV1 ownedStackQuery) noexcept {
    if (state == nullptr || state->structSize != sizeof(*state) ||
        state->abiVersion != 3 || state->reserved != 0 ||
        (state->mxcsr & 0xffff0000U) != 0 ||
        hostCall == nullptr || executableQuery == nullptr) {
        return LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1;
    }
#if defined(__APPLE__) || defined(__ANDROID__)
    HostFloatingPointEnvironment hostEnvironment;
    if (!hostEnvironment.Valid()) {
        return LUMEN_ANDROID_FEXCORE_RUNTIME_THREAD_SETUP_FAILED_V1;
    }
    EnsureRuntime();
    if (g_runtime.status.code != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) {
        return g_runtime.status.code;
    }
    if (!HasScalarSseView(g_runtime.context.get())) {
        return LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
    }
    // The reviewed Mac translator legalizes independent DAZ/FTZ in IR on
    // non-AFP hosts, including control changes executed after root entry.
    const std::uint32_t result = RunGuestInternal(
        &state->gp, nullptr, executableQuery, opaque, false, hostCall, false, state,
        hostEnvironment.Snapshot(), 0, 0, ownedStackBase, ownedStackSize, ownedStackQuery);
    return hostEnvironment.Restore()
        ? result : LUMEN_ANDROID_FEXCORE_RUNTIME_BRIDGE_FAILED_V1;
#else
    (void)ownedStackBase;
    (void)ownedStackSize;
    (void)ownedStackQuery;
    return LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1;
#endif
}

extern "C" std::uint32_t LumenAndroidFexCoreRunGuestV3(
    LumenAndroidFexCoreGuestStateV3* state,
    LumenAndroidFexCoreHostCallV2 hostCall,
    LumenAndroidFexCoreExecutableQueryV1 executableQuery,
    void* opaque) noexcept {
    return RunGuestCheckpoint(state, hostCall, executableQuery, opaque, 0, 0, nullptr);
}

#if defined(__APPLE__)
extern "C" std::uint32_t LumenMacOSFexCoreRunGuestWithOwnedStackV3(
    LumenAndroidFexCoreGuestStateV3* state,
    LumenAndroidFexCoreHostCallV2 hostCall,
    LumenAndroidFexCoreExecutableQueryV1 executableQuery,
    void* opaque, std::uint64_t ownedBase, std::uint64_t ownedSize,
    LumenMacOSFexCoreOwnedStackAccessQueryV1 ownedStackQuery) noexcept {
    if (ownedBase == 0 || ownedSize == 0 || ownedBase > UINT64_MAX - ownedSize ||
        ownedStackQuery == nullptr) return LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1;
    return RunGuestCheckpoint(state, hostCall, executableQuery, opaque,
                              ownedBase, ownedSize, ownedStackQuery);
}

extern "C" std::uint32_t LumenMacOSFexCoreTryReadOwnedStackForHostCallV1(
    std::uint64_t address, void* destination, std::uint64_t size) noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("LUMEN_EXPERIMENTAL_FEX_HLE_OWNED_STACK_READ");
        return value == nullptr || (value[0] == '1' && value[1] == '\0');
    }();
    if (!enabled || destination == nullptr || address == 0 || size == 0 ||
        size > (std::numeric_limits<std::size_t>::max)() ||
        address > UINT64_MAX - size ||
        reinterpret_cast<std::uintptr_t>(destination) > UINTPTR_MAX - size ||
        g_activeRun == nullptr || g_activeRun->thread == nullptr ||
        (!GuestMutationBarrier().CurrentThreadOwnsHostMemoryReadLease() &&
         !GuestMutationBarrier().CurrentThreadOwnsShortHostCallLease())) return 0;

    for (const ActiveRun* owner = g_activeRun; owner != nullptr; owner = owner->parentRun) {
        if (owner->ownedStackQuery == nullptr || owner->ownedStackBase == 0 ||
            owner->ownedStackSize == 0 ||
            owner->ownedStackBase > UINT64_MAX - owner->ownedStackSize ||
            address < owner->ownedStackBase ||
            address - owner->ownedStackBase >= owner->ownedStackSize ||
            size > owner->ownedStackSize - (address - owner->ownedStackBase)) continue;
        if (owner->ownedStackQuery(address, size, 0,
                                   owner->ownedStackBase, owner->ownedStackSize) == 0) continue;
        // Ownership keeps this exact allocation RW and alive. The retained
        // host-memory reader excludes destructive Core mutations until after
        // this synchronous copy; the callback still checks managed guards.
        std::memmove(destination, reinterpret_cast<const void*>(address),
                     static_cast<std::size_t>(size));
        return 1;
    }
    return 0;
}

extern "C" void LumenMacOSFexCoreObserveMemsetOwnedStackCoverageV1(
    std::uint64_t address, std::uint64_t size) noexcept {
    enum Category : std::size_t {
        Current, Ancestor, Outside, Refused, NoContext, Invalid, CategoryCount
    };
    struct Counters {
        std::uint32_t calls {};
        std::uint32_t samples {};
        std::uint32_t threadSlot {};
        std::array<std::uint32_t, CategoryCount> categories {};
        std::array<std::uint32_t, 6> sizes {};
        std::array<std::uint32_t, 6> ownedSizes {};
    };
    static std::atomic<std::uint32_t> emitted {0};
    static std::atomic<std::uint32_t> nextThreadSlot {1};
    thread_local Counters counters;
    // Bounds both work and output. Coverage is sampled, process-lifetime and
    // cumulative per host thread; it is not an exact call or byte percentage.
    constexpr std::uint32_t maxCalls = 1U << 24;
    if (counters.calls == maxCalls) return;
    ++counters.calls;
    if ((counters.calls & 63U) != 0 ||
        emitted.load(std::memory_order_relaxed) >= 128) return;
    if (counters.threadSlot == 0)
        counters.threadSlot = nextThreadSlot.fetch_add(1, std::memory_order_relaxed);
    ++counters.samples;
    const std::size_t sizeClass = size <= 64 ? 0 : size <= 256 ? 1 :
        size <= 4096 ? 2 : size <= 65536 ? 3 : size <= (1U << 20) ? 4 : 5;
    ++counters.sizes[sizeClass];

    Category category = Outside;
    if (address == 0 || size == 0 || address > UINT64_MAX - size ||
        size > (std::numeric_limits<std::size_t>::max)()) {
        category = Invalid;
    } else if (g_activeRun == nullptr || g_activeRun->thread == nullptr ||
               !GuestMutationBarrier().CurrentThreadOwnsHostMemoryReadLease()) {
        category = NoContext;
    } else {
        for (const ActiveRun* owner = g_activeRun; owner != nullptr; owner = owner->parentRun) {
            if (owner->ownedStackQuery == nullptr || owner->ownedStackBase == 0 ||
                owner->ownedStackSize == 0 ||
                owner->ownedStackBase > UINT64_MAX - owner->ownedStackSize ||
                address < owner->ownedStackBase ||
                address - owner->ownedStackBase >= owner->ownedStackSize ||
                size > owner->ownedStackSize - (address - owner->ownedStackBase)) continue;
            // Snapshot logical proof only: no physical query, lease or memory
            // access. The ordinary hle_memset lease remains authoritative.
            if (owner->ownedStackQuery(address, size, 1,
                                      owner->ownedStackBase, owner->ownedStackSize) == 0) {
                category = Refused;
                continue;
            }
            category = owner == g_activeRun ? Current : Ancestor;
            ++counters.ownedSizes[sizeClass];
            break;
        }
    }
    ++counters.categories[category];
    if (counters.calls < 4096 || (counters.calls & (counters.calls - 1)) != 0) return;
    auto budget = emitted.load(std::memory_order_relaxed);
    do {
        if (budget >= 128) return;
    } while (!emitted.compare_exchange_weak(budget, budget + 1,
                                           std::memory_order_relaxed));
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::fprintf(stderr,
        "[FEX_MEMSET_OWNED_STACK] thread_slot=%u monotonic_ms=%lld calls=%u samples=%u stride=64 "
        "current=%u ancestor=%u outside=%u refused=%u no_context=%u invalid=%u "
        "size_le64=%u size_le256=%u size_le4096=%u size_le65536=%u size_le1048576=%u size_gt1048576=%u "
        "owned_le64=%u owned_le256=%u owned_le4096=%u owned_le65536=%u owned_le1048576=%u owned_gt1048576=%u "
        "terminal=%u\n",
        counters.threadSlot, static_cast<long long>(now), counters.calls, counters.samples,
        counters.categories[Current], counters.categories[Ancestor], counters.categories[Outside],
        counters.categories[Refused], counters.categories[NoContext], counters.categories[Invalid],
        counters.sizes[0], counters.sizes[1], counters.sizes[2], counters.sizes[3], counters.sizes[4], counters.sizes[5],
        counters.ownedSizes[0], counters.ownedSizes[1], counters.ownedSizes[2],
        counters.ownedSizes[3], counters.ownedSizes[4], counters.ownedSizes[5],
        counters.calls == maxCalls || budget + 1 == 128 ? 1U : 0U);
}
#endif

extern "C" std::uint64_t
LumenAndroidFexCoreBeginGuestMemoryMutationV1() noexcept {
    return BeginGuestMemoryMutation(GuestMemoryMutationScope::Exclusive);
}

extern "C" std::uint64_t LumenAndroidFexCoreBeginGuestMemoryMutationV2(
    std::uint32_t kind) noexcept {
    return LumenAndroidFexCoreBeginGuestMemoryMutationV3(kind, nullptr);
}

extern "C" std::uint64_t LumenAndroidFexCoreBeginGuestMemoryMutationV3(
    std::uint32_t kind, std::uint32_t* retryable) noexcept {
    if (retryable != nullptr) *retryable = 0;
    switch (kind) {
        case LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2:
            return BeginGuestMemoryMutation(
                GuestMemoryMutationScope::Exclusive, retryable);
        case LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_ADDITIVE_MAPPING_V2:
            return BeginGuestMemoryMutation(
                GuestMemoryMutationScope::AdditiveMapping, retryable);
        default:
            __android_log_print(
                ANDROID_LOG_ERROR, "LumenFexCoreRuntime",
                "guest memory mutation denied invalidKind=%u", kind);
            return 0;
    }
}

extern "C" std::uint32_t LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(
    std::uint64_t token) noexcept {
    return GuestMutationBarrier().OwnsExclusiveMutation(token) ? 1U : 0U;
}

extern "C" std::uint64_t LumenAndroidFexCoreRetainExclusivePublicationBorrowV1() noexcept {
    return GuestMutationBarrier().RetainExclusivePublicationBorrow();
}

extern "C" std::uint32_t LumenAndroidFexCoreReleaseExclusivePublicationBorrowV1(
    std::uint64_t token) noexcept {
    return GuestMutationBarrier().ReleaseExclusivePublicationBorrow(token) ? 1U : 0U;
}

extern "C" void LumenAndroidFexCoreEndGuestMemoryMutationV1(
    std::uint64_t token, std::uint64_t rangeBase,
    std::uint64_t rangeSize, std::uint32_t committed) noexcept {
    (void)LumenAndroidFexCoreEndGuestMemoryMutationV2(
        token, rangeBase, rangeSize, committed, nullptr, nullptr);
}

extern "C" std::uint32_t LumenAndroidFexCoreEndGuestMemoryMutationV2(
    std::uint64_t token, std::uint64_t rangeBase,
    std::uint64_t rangeSize, std::uint32_t committed,
    LumenAndroidFexCoreMutationLocalReleaseV2 releaseLocalGate,
    void* opaque) noexcept {
    return GuestMutationBarrier().PublishAndEndMutation(
        token,
        [&]() noexcept {
            if (committed != 0 && rangeSize != 0) {
                auto* callingThread = g_activeRun != nullptr
                    ? g_activeRun->thread
                    : nullptr;
                InvalidateActiveGuestCodeRange(
                    callingThread, rangeBase, rangeSize);
            }
            if (releaseLocalGate != nullptr) releaseLocalGate(opaque);
        },
        &DisarmActiveGuestSafepoints) ? 1U : 0U;
}

extern "C" void LumenAndroidFexCoreSetHostCallDiagnosticV1(
    std::uint64_t operation, const char* name) noexcept {
    GuestMutationBarrier().SetCurrentHostCallDiagnostic(
        g_activeRun != nullptr ? g_activeRun->nativeThreadId : 0, operation, name);
}

extern "C" std::uint64_t
LumenAndroidFexCoreBeginQuiescentHostWaitV1() noexcept {
    if (g_activeRun == nullptr) return 0;
    const auto token = GuestMutationBarrier().BeginQuiescentHostWait();
    if (token != 0)
        g_activeRun->observedHostWaitToken.store(token, std::memory_order_release);
    if (token != 0 && g_activeRun->performanceTraceEnabled &&
        !g_activeRun->performanceWait.has_value()) {
        g_activeRun->performanceWaitToken = token;
        g_activeRun->performanceWait.emplace(Lumen::Performance::Phase::HostWait);
    }
    return token;
}

extern "C" std::uint32_t LumenAndroidFexCoreRegisterFreshCallbackDataAccessQueryV1(
    LumenAndroidFexCoreFreshCallbackDataAccessQueryV1 query) noexcept {
    if (query == nullptr) return 0;
    LumenAndroidFexCoreFreshCallbackDataAccessQueryV1 expected = nullptr;
    return g_freshCallbackDataAccessQuery.compare_exchange_strong(expected, query,
        std::memory_order_release, std::memory_order_acquire) || expected == query ? 1U : 0U;
}

extern "C" std::uint64_t LumenAndroidFexCoreBeginFreshGuestCallbackV1(
    std::uint64_t stackBase, std::uint64_t stackSize,
    std::uint64_t tlsBase, std::uint64_t tlsSize) noexcept {
    auto* run = g_activeRun;
    auto& barrier = GuestMutationBarrier();
    if (run == nullptr || run->activeHostCall == 0 || run->activeHostCallTarget == 0 ||
        run->freshCallbackToken != 0 || run->freshCallbackRestoreFailed ||
        run->freshCallbackLifetimeRevoked.load(std::memory_order_acquire) ||
        stackBase == 0 || stackSize == 0 || stackSize > UINT64_MAX - stackBase ||
        (tlsBase == 0) != (tlsSize == 0) || tlsSize > UINT64_MAX - tlsBase ||
        !barrier.CurrentThreadOwnsHostMemoryReadLease()) return 0;
    std::array<std::uint64_t, 9> previousBounds;
    bool previousWatch;
    {
        std::lock_guard lock(g_runtime.activeThreadsMutex);
        previousBounds = {run->freshCallbackReturn, run->freshCallbackRsp, run->freshCallbackFs,
            run->freshCodeBase, run->freshCodeSize, run->freshStackBase, run->freshStackSize,
            run->freshTlsBase, run->freshTlsSize};
        previousWatch = run->freshCallbackWatchActive;
        run->freshStackBase = stackBase;
        run->freshStackSize = stackSize;
        run->freshTlsBase = tlsBase;
        run->freshTlsSize = tlsSize;
    }
    const auto restorePreviousWatch = [&]() noexcept {
        std::lock_guard lock(g_runtime.activeThreadsMutex);
        run->freshCallbackReturn = previousBounds[0];
        run->freshCallbackRsp = previousBounds[1];
        run->freshCallbackFs = previousBounds[2];
        run->freshCodeBase = previousBounds[3];
        run->freshCodeSize = previousBounds[4];
        run->freshStackBase = previousBounds[5];
        run->freshStackSize = previousBounds[6];
        run->freshTlsBase = previousBounds[7];
        run->freshTlsSize = previousBounds[8];
        run->freshCallbackWatchActive = previousWatch;
    };
    if (!FreshCallbackParentValid(*run, true)) {
        restorePreviousWatch();
        return 0;
    }
    // Publish the lifetime watch before donating the reader. Never acquire
    // the barrier while holding the registry: publishers take the reverse
    // order. The independent watch covers token publication and restoration.
    {
        std::lock_guard lock(g_runtime.activeThreadsMutex);
        run->freshCallbackWatchActive = true;
    }
    const auto token = barrier.BeginFreshCallback();
    run->freshCallbackToken = token;
    if (token != 0)
        run->observedFreshCallbackToken.store(token, std::memory_order_release);
    if (token == 0) {
        restorePreviousWatch();
    } else if (!g_freshCallbackBeginReported.exchange(true, std::memory_order_relaxed)) {
        __android_log_print(ANDROID_LOG_INFO, "LumenFexCoreRuntime",
            "FEX_FRESH_GUEST_CALLBACK_V1 begin validated=1");
    }
    return token;
}

extern "C" std::uint32_t LumenAndroidFexCoreEndFreshGuestCallbackV1(
    std::uint64_t token) noexcept {
    auto* run = g_activeRun;
    if (run == nullptr || token == 0 || run->freshCallbackToken != token) return 0;
    if (!GuestMutationBarrier().EndFreshCallback(token)) {
        run->freshCallbackRestoreFailed = true;
        run->failure = LUMEN_ANDROID_FEXCORE_RUNTIME_MUTATION_BARRIER_DENIED_V1;
        return 0;
    }
    run->freshCallbackToken = 0;
    run->observedFreshCallbackToken.store(0, std::memory_order_release);
    const bool valid = !run->freshCallbackRestoreFailed && FreshCallbackParentValid(*run, false);
    if (!valid) {
        run->freshCallbackRestoreFailed = true;
        run->failure = LUMEN_ANDROID_FEXCORE_RUNTIME_MUTATION_BARRIER_DENIED_V1;
    }
    // Keep the watch armed until host-reader -> execution readmission and its
    // checkpoint have both completed. A writer can still revoke in that gap.
    return valid ? 1U : 0U;
}

extern "C" std::uint32_t
LumenAndroidFexCoreValidateFreshGuestCallbackParentV1() noexcept {
    auto* run = g_activeRun;
    if (run == nullptr || run->freshCallbackToken == 0 ||
        run->freshCallbackRestoreFailed ||
        !GuestMutationBarrier().CurrentThreadOwnsExecutionOrMutationLease()) return 0;
    {
        std::lock_guard lock(g_runtime.activeThreadsMutex);
        if (!run->freshCallbackWatchActive) return 0;
    }
    return FreshCallbackParentValid(*run, false) ? 1U : 0U;
}

extern "C" void LumenAndroidFexCoreNotifyFreshGuestCallbackMappingChangeV1(
    std::uint64_t base, std::uint64_t size) noexcept {
    std::lock_guard lock(g_runtime.activeThreadsMutex);
    for (auto* run : g_runtime.activeRuns) {
        if (run == nullptr || !run->freshCallbackWatchActive) continue;
        const auto overlaps = [base, size](std::uint64_t savedBase, std::uint64_t savedSize) {
            if (savedSize == 0) return false;
            if (size == 0 || size > UINT64_MAX - base) return true;
            return base < savedBase + savedSize && savedBase < base + size;
        };
        if (overlaps(run->freshCodeBase, run->freshCodeSize) ||
            overlaps(run->freshStackBase, run->freshStackSize) ||
            overlaps(run->freshTlsBase, run->freshTlsSize))
            run->freshCallbackLifetimeRevoked.store(true, std::memory_order_release);
    }
}

extern "C" std::uint32_t LumenAndroidFexCoreEndQuiescentHostWaitV1(
    std::uint64_t token) noexcept {
    if (g_activeRun == nullptr) return 0;
    bool restored;
    {
        Lumen::Performance::Scope timing(Lumen::Performance::Phase::Readmission);
        restored = GuestMutationBarrier().EndQuiescentHostWait(token);
    }
    if (restored && g_activeRun->performanceWaitToken == token) {
        g_activeRun->performanceWait.reset();
        g_activeRun->performanceWaitToken = 0;
    }
    if (restored)
        g_activeRun->observedHostWaitToken.store(0, std::memory_order_release);
    return restored ? 1U : 0U;
}

extern "C" std::uint32_t
LumenAndroidFexCoreRetainGuestDataLeaseV1() noexcept {
    return GuestMutationBarrier().RetainGuestDataLease() ? 1U : 0U;
}

extern "C" std::uint32_t
LumenAndroidFexCoreReleaseGuestDataLeaseV1() noexcept {
    return GuestMutationBarrier().ReleaseGuestDataLease() ? 1U : 0U;
}

extern "C" std::uint64_t
LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV2(std::uint32_t* mutation_pending) noexcept {
    if (mutation_pending != nullptr) *mutation_pending = 0;
    const char* denial = nullptr;
    bool pending = false;
    const auto token = GuestMutationBarrier().TryBeginNativeGuestDataLease(&denial, &pending);
    if (token == 0 && pending && mutation_pending != nullptr) *mutation_pending = 1;
    if (token == 0) {
        // Report after releasing the barrier mutex, including in a normal
        // launch where rendering diagnostics are disabled. Admission is unchanged.
        thread_local std::uint32_t reports = 0;
        if (reports < 64) {
            ++reports;
            __android_log_print(ANDROID_LOG_INFO, "LumenFexCoreRuntime",
                "LUMEN_FEX_NATIVE_DATA_DENIED_V1 reason=%s report=%u",
                denial != nullptr ? denial : "unknown", reports);
        }
    }
    return token;
}

extern "C" std::uint64_t
LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV1() noexcept {
    return LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV2(nullptr);
}

extern "C" std::uint32_t LumenAndroidFexCoreEndNativeGuestDataLeaseV1(
    std::uint64_t token) noexcept {
    return GuestMutationBarrier().EndNativeGuestDataLease(token) ? 1U : 0U;
}

extern "C" std::uint64_t LumenAndroidFexCoreDowngradeExclusiveToNativeGuestDataLeaseV1(
    std::uint64_t mutation_token) noexcept {
    return GuestMutationBarrier().DowngradeExclusiveToNativeGuestDataLease(
        mutation_token, &DisarmActiveGuestSafepoints);
}

extern "C" std::uint32_t LumenAndroidFexCoreOwnsNativeGuestDataLeaseV1(
    std::uint64_t token) noexcept {
    return GuestMutationBarrier().OwnsNativeGuestDataLease(token) ? 1U : 0U;
}

extern "C" std::uint32_t LumenAndroidFexCoreRegisterBoundaryDeliveryV1(
    LumenAndroidFexCoreBoundaryDeliveryV1 delivery) noexcept {
    if (delivery == nullptr) return 0;
    LumenAndroidFexCoreBoundaryDeliveryV1 expected = nullptr;
    return g_boundaryDelivery.compare_exchange_strong(
               expected, delivery, std::memory_order_release,
               std::memory_order_acquire) || expected == delivery
        ? 1U : 0U;
}

extern "C" std::uint64_t
LumenAndroidFexCoreRequestGuestBoundaryInterruptV1(
    std::uint64_t hostThread) noexcept {
    if (hostThread == 0) return 0;
    std::lock_guard threadsLock(g_runtime.activeThreadsMutex);
    ActiveRun* const run = FindActiveRunForHostThreadLocked(hostThread);
    if (run == nullptr) return 0;
    std::uint64_t expected = 0;
    std::uint64_t sequence =
        g_boundaryInterruptSequence.fetch_add(
            1, std::memory_order_relaxed) + 1;
    if (sequence == 0) {
        sequence = g_boundaryInterruptSequence.fetch_add(
                       1, std::memory_order_relaxed) + 1;
    }
    if (!run->boundaryInterruptToken.compare_exchange_strong(
            expected, sequence, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        // A previously armed request covers this queue entry too: standard
        // signals coalesce on one pending slot.
        return expected;
    }
    if (run->safepointEpoch.load(std::memory_order_acquire) == 0 &&
        mprotect(run->thread->InterruptFaultPage,
                 sizeof(run->thread->InterruptFaultPage),
                 PROT_READ) != 0) {
        // The page stays writable: the ordinary boundary route remains and
        // no request is armed.
        run->boundaryInterruptToken.store(0, std::memory_order_release);
        return 0;
    }
    return sequence;
}

extern "C" std::uint32_t
LumenAndroidFexCoreCancelGuestBoundaryInterruptV1(
    std::uint64_t token) noexcept {
    if (token == 0) return 0;
    std::lock_guard threadsLock(g_runtime.activeThreadsMutex);
    for (auto* run : g_runtime.activeRuns) {
        if (run == nullptr || run->thread == nullptr) continue;
        std::uint64_t expected = token;
        if (!run->boundaryInterruptToken.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            continue;
        }
        if (run->safepointEpoch.load(std::memory_order_acquire) == 0 &&
            mprotect(run->thread->InterruptFaultPage,
                     sizeof(run->thread->InterruptFaultPage),
                     PROT_READ | PROT_WRITE) != 0) {
            // Leaving the page protected with no armed request would fault
            // every resumed block; fail closed like the other releases.
            __android_log_print(
                ANDROID_LOG_FATAL, "LumenFexCoreRuntime",
                "failed to release cancelled FEX boundary interrupt: "
                "errno=%d",
                errno);
            _exit(125);
        }
        return 1;
    }
    return 0;
}

#if defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD)
extern "C" std::uint64_t LumenMacOSFexCoreProbeActiveCodeCacheV1() noexcept {
    if (g_activeRun == nullptr || g_activeRun->thread == nullptr) {
        return 0;
    }
    return reinterpret_cast<std::uintptr_t>(
        g_activeRun->thread->LookupCache->Shared);
}
#endif
