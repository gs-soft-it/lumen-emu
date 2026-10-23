#pragma once

#include <cstddef>
#include <cstdint>

enum LumenAndroidFexCoreRuntimeCodeV1 : std::uint32_t {
    LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 = 0,
    LUMEN_ANDROID_FEXCORE_RUNTIME_WRONG_PAGE_SIZE_V1 = 1,
    LUMEN_ANDROID_FEXCORE_RUNTIME_RWX_DENIED_V1 = 2,
    LUMEN_ANDROID_FEXCORE_RUNTIME_RWX_EXECUTION_FAILED_V1 = 3,
    LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1 = 4,
    LUMEN_ANDROID_FEXCORE_RUNTIME_SELF_TEST_FAILED_V1 = 5,
    LUMEN_ANDROID_FEXCORE_RUNTIME_SIGNAL_SETUP_FAILED_V1 = 6,
    LUMEN_ANDROID_FEXCORE_RUNTIME_BRIDGE_FAILED_V1 = 7,
    LUMEN_ANDROID_FEXCORE_RUNTIME_HOST_CALL_FAILED_V1 = 8,
    LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 = 9,
    LUMEN_ANDROID_FEXCORE_RUNTIME_SYSCALL_DENIED_V1 = 10,
    LUMEN_ANDROID_FEXCORE_RUNTIME_THREAD_SETUP_FAILED_V1 = 11,
    LUMEN_ANDROID_FEXCORE_RUNTIME_EXECUTABLE_RANGE_DENIED_V1 = 12,
    LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1 = 13,
    LUMEN_ANDROID_FEXCORE_RUNTIME_UNSUPPORTED_ANDROID_API_V1 = 14,
    LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1 = 15,
    LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1 = 16,
    LUMEN_ANDROID_FEXCORE_RUNTIME_MUTATION_BARRIER_DENIED_V1 = 17,
    LUMEN_ANDROID_FEXCORE_RUNTIME_UNSUPPORTED_FP_CONTROL_V3 = 18,
};

enum LumenAndroidFexCoreHostCallDispositionV1 : std::uint32_t {
    LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1 = 0,
    LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1 = 1,
    LUMEN_ANDROID_FEXCORE_HOST_CALL_STOP_V1 = 2,
    // Leave the guest CALL frame intact. After execution readmission the
    // bridge executes an ordinary guest RET, including its memory handoff,
    // fault delivery, RSP update and FEX call-return prediction. nextGuestRip
    // is ignored. Existing CONTINUE callbacks still pop their own CALL frame.
    LUMEN_ANDROID_FEXCORE_HOST_CALL_RETURN_GUEST_V1 = 3,
};

struct LumenAndroidFexCoreRuntimeStatusV1 {
    std::uint32_t structSize;
    std::uint32_t abiVersion;
    std::uint32_t code;
    std::uint32_t hostPageSize;
    std::uint32_t rwxMapped;
    std::uint32_t rwxExecuted;
    std::uint32_t contextInitialized;
    std::uint32_t signalStopInstalled;
    std::uint32_t hostBridgeReady;
    std::uint32_t hostBridgeExecuted;
    std::uint32_t backendRouteReady;
    std::uint32_t registeredHostTargets;
    std::uint32_t guestExecuted;
    std::uint32_t logicalCores;
    std::uint64_t guestResult;
};

namespace Lumen::Android::FexCoreLab {

// Android vendor kernels may report the FEX interrupt-page protection fault
// as SEGV_MAPERR.  Epoch zero is not a rejection by itself: Disarm publishes
// it after restoring write access, so a signal already raised by the original
// store can arrive late and must simply retry that store.  Keep this small
// classifier constexpr so the late-delivery contract is covered without
// manufacturing an asynchronous signal race in the runtime test.
enum class VendorMapErrorDoorbellDisposition : std::uint8_t {
    Reject,
    RetryLateDelivery,
    HandleArmed,
};

constexpr VendorMapErrorDoorbellDisposition
ClassifyVendorMapErrorDoorbellFault(
    std::uint64_t safepointEpoch, bool exactDoorbellAddress,
    bool isSegvMapError, bool inJit, bool readableJitInstruction,
    bool exactInterruptDoorbellStore, bool storeTargetsDoorbell) noexcept {
    if (!exactDoorbellAddress || !isSegvMapError || !inJit ||
        !readableJitInstruction || !exactInterruptDoorbellStore ||
        !storeTargetsDoorbell) {
        return VendorMapErrorDoorbellDisposition::Reject;
    }
    return safepointEpoch == 0
        ? VendorMapErrorDoorbellDisposition::RetryLateDelivery
        : VendorMapErrorDoorbellDisposition::HandleArmed;
}

// Dispatcher probes never reconstruct guest state or release a reader. They
// may only retry a late store or skip this proven pending-interrupt probe on
// the way to an existing generated-code boundary.
constexpr bool IsDispatcherMapErrorDoorbellFault(
    bool exactDoorbellAddress, bool isSegvMapError,
    bool readableDispatcherInstruction, bool exactInterruptDoorbellStore,
    bool storeTargetsDoorbell) noexcept {
    return exactDoorbellAddress && isSegvMapError &&
        readableDispatcherInstruction && exactInterruptDoorbellStore &&
        storeTargetsDoorbell;
}

} // namespace Lumen::Android::FexCoreLab

struct LumenAndroidFexCoreGuestStateV1 {
    std::uint32_t structSize;
    std::uint32_t abiVersion;
    std::uint64_t rip;
    std::uint64_t gprs[16];
    std::uint64_t fsBase;
    std::uint64_t gsBase;
    std::uint64_t faultAddress;
    std::uint32_t faultSignal;
    std::uint32_t reserved;
};

using LumenAndroidFexCoreHostCallV1 = std::uint32_t (*)(
    void* opaque, std::uint64_t target, std::uint64_t* guestGprs,
    std::uint64_t* nextGuestRip) noexcept;

// A borrowed, writable view of the low 128 bits of each guest vector
// register. The view is valid only for the duration of this host callback;
// bytes outside registerBytes (including upper AVX lanes) are not exposed.
struct LumenAndroidFexCoreSimdViewV2 {
    std::uint32_t structSize;
    std::uint32_t registerCount;
    std::uint32_t registerStride;
    std::uint32_t registerBytes;
    std::uint8_t* registers;
};

using LumenAndroidFexCoreHostCallV2 = std::uint32_t (*)(
    void* opaque, std::uint64_t target, std::uint64_t* guestGprs,
    LumenAndroidFexCoreSimdViewV2* simd,
    std::uint64_t* nextGuestRip) noexcept;

using LumenAndroidFexCoreExecutableQueryV1 = std::uint32_t (*)(
    void* opaque, std::uint64_t address, std::uint64_t* rangeBase,
    std::uint64_t* rangeSize, std::uint32_t* writable) noexcept;

#if defined(LUMEN_FEXCORE_RUNTIME) || defined(LUMEN_MACOS_FEXCORE_RUNTIME_BUILD) || defined(LUMEN_ANDROID_FEXCORE_RUNTIME)
// Pure helper fixture: no global registration or execution of synthetic targets.
extern "C" std::uint32_t LumenAndroidFexCoreHostTargetSnapshotContractTestV1() noexcept;
#endif

extern "C" std::uint32_t LumenAndroidFexCoreRuntimeProbeV1(
    LumenAndroidFexCoreRuntimeStatusV1* status) noexcept;

// Renderer coherence contract, never a title or environment selector. The
// exclusive-publication engine must retain its CPU/GPU exclusion and cannot
// register an interval handoff ledger. Configure before runtime initialization
// or provider lookup; the unconfigured default is INTERVAL_HANDOFF. Repeating
// an explicit identical selection is idempotent, even after initialization;
// conflicting selections fail without changing the already emitted code or
// cache domain, including after an implicit default freeze.
inline constexpr std::uint32_t LUMEN_FEX_MEMORY_EXCLUSIVE_PUBLICATION_V1 = 0;
inline constexpr std::uint32_t LUMEN_FEX_MEMORY_INTERVAL_HANDOFF_V1 = 1;
extern "C" std::uint32_t LumenAndroidFexCoreConfigureMemoryHandoffV1(std::uint32_t mode) noexcept;

// Pinned overlay and live provider capability. Android and macOS emit inline
// ownership guards for direct scalar, atomic, SIMD, string and x87 accesses.
// GPU ownership may be acquired only inside a real exclusive mutation epoch.
extern "C" std::uint32_t
LumenAndroidFexCoreMemoryHandoffInstrumentationReadyV1() noexcept;

struct LumenAndroidFexCoreGuestTscV1 {
    std::uint32_t structSize {sizeof(LumenAndroidFexCoreGuestTscV1)};
    std::uint32_t abiVersion {1};
    std::uint64_t counter {};
    std::uint64_t frequency {};
    std::uint64_t nativeFrequency {};
    std::uint32_t scale {};
    std::uint32_t reserved {};
};

// The ready process runtime publishes the exact CNTVCT/TSCScale domain used
// by guest RDTSC/RDTSCP and CPUID leaf 15h. This query does not initialize the
// runtime and needs no active-root TLS, so VideoOut/GPU host threads may use
// it too. Invalid ABI or unpublished/invalid controls fail closed; there is
// no fallback to a host clock, frequency calibration or rebased origin.
extern "C" std::uint32_t LumenAndroidFexCoreQueryGuestTscV1(
    LumenAndroidFexCoreGuestTscV1* output) noexcept;

// Explicit diagnostic only: three original programs exercise the V2 default
// root and HLE/nested-root FP boundaries without a ROM. The native caller's
// environment is restored before return. Invocation requires a quiescent
// runtime; this is never executed automatically during game launch.
struct LumenAndroidFexCoreFpBoundaryProbeStatusV1 {
    std::uint32_t structSize {sizeof(LumenAndroidFexCoreFpBoundaryProbeStatusV1)};
    std::uint32_t abiVersion {1};
    std::uint32_t runtimeCode {};
    std::uint32_t executedCases {};
    // Bits: root-default=1, guest-HLE-control=2, nested-root-default=4,
    // native-HLE-entry=8, native-return=16. Full conformance is 31.
    std::uint32_t conformanceMask {};
    std::int32_t rootResult {}, hleBefore {}, hleAfter {};
    std::int32_t nestedResult {}, nestedOuterBefore {}, nestedOuterAfter {};
    std::int32_t hleEntryRounding {}, nestedEntryRounding {};
    std::int32_t rootReturnRounding {}, hleReturnRounding {}, nestedReturnRounding {};
    std::uint32_t rootMxcsr {}, hleMxcsrBefore {}, hleMxcsrAfter {};
    std::uint32_t nestedMxcsr {}, nestedOuterMxcsrBefore {}, nestedOuterMxcsrAfter {};
};

// One means all programs executed and observations are valid; conformance is
// reported independently. Zero means setup/runtime failure, not a CPU gap.
extern "C" std::uint32_t LumenAndroidFexCoreFpBoundaryProbeV1(
    LumenAndroidFexCoreFpBoundaryProbeStatusV1* status) noexcept;

extern "C" const char* LumenAndroidFexCoreRuntimeReceiptV1() noexcept;

// Diagnostic receipt for the most recent accepted Android safepoint-doorbell
// fault. Linux SEGV_MAPERR/SEGV_ACCERR values remain distinguishable; zero
// means that this process has not accepted a doorbell fault.
extern "C" std::uint32_t
LumenAndroidFexCoreLastSafepointSignalCodeV1() noexcept;

extern "C" std::uint32_t LumenAndroidFexCoreRegisterHostCallTargetV1(
    std::uint64_t target) noexcept;

extern "C" std::uint64_t
LumenAndroidFexCoreRootReturnAddressV1() noexcept;

extern "C" std::uint32_t LumenAndroidFexCoreRunGuestV1(
    LumenAndroidFexCoreGuestStateV1* state,
    LumenAndroidFexCoreHostCallV1 hostCall,
    LumenAndroidFexCoreExecutableQueryV1 executableQuery,
    void* opaque) noexcept;

// Root arguments/results retain the existing V1 GP-state contract. V2 adds
// vector state only at host-call boundaries and preserves the V1 entry point.
extern "C" std::uint32_t LumenAndroidFexCoreRunGuestV2(
    LumenAndroidFexCoreGuestStateV1* state,
    LumenAndroidFexCoreHostCallV2 hostCall,
    LumenAndroidFexCoreExecutableQueryV1 executableQuery,
    void* opaque) noexcept;

// Root checkpoint for the pinned SSE128/AVX-high representation on both hosts.
// vectorRegisters contain architectural YMM bytes. x87Registers and flags
// are opaque pinned-FEX storage: callers use the value initializer or restore
// an unchanged prior checkpoint. They are not host long doubles or RFLAGS.
// Entry is a fresh root call: 16 bytes below RSP must be writable scratch and
// are overwritten by the MXCSR bootstrap. This is not an arbitrary suspended
// instruction resume API. The patched Darwin translator applies DAZ/FTZ
// independently, including guest MXCSR changes on non-AFP hosts. The numerical
// qualification covers masked exceptions; accrued FP status and unmasked
// exceptions are outside that qualification.
// V1/V2 retain their ABI and GP-only root contract. V3 also supports Android
// with the same translated LDMXCSR entry bootstrap and native FP isolation.
struct LumenAndroidFexCoreGuestStateV3 {
    std::uint32_t structSize {sizeof(LumenAndroidFexCoreGuestStateV3)};
    std::uint32_t abiVersion {3};
    LumenAndroidFexCoreGuestStateV1 gp {
        sizeof(LumenAndroidFexCoreGuestStateV1), 1, 0, {}, 0, 0, 0, 0, 0,
    };
    std::uint8_t vectorRegisters[16][32] {};
    std::uint32_t mxcsr {0x1f80};
    std::uint16_t x87ControlWord {0x037f};
    std::uint8_t x87AbridgedTag {};
    std::uint8_t reserved {};
    std::uint8_t x87Registers[8][16] {};
    std::uint8_t flags[48] {};
    std::uint32_t parityRaw {1};
    std::uint32_t auxiliaryCarryRaw {};
};

extern "C" std::uint32_t LumenAndroidFexCoreRunGuestV3(
    LumenAndroidFexCoreGuestStateV3* state,
    LumenAndroidFexCoreHostCallV2 hostCall,
    LumenAndroidFexCoreExecutableQueryV1 executableQuery,
    void* opaque) noexcept;

using LumenMacOSFexCoreOwnedStackAccessQueryV1 = std::uint32_t (*)(
    std::uint64_t address, std::uint64_t size, std::uint32_t write,
    std::uint64_t ownedBase, std::uint64_t ownedSize) noexcept;
#if defined(__APPLE__)
// Trusted embedding API: ownedBase/ownedSize identify the allocator's exact
// private RW stack allocation, never an interval inferred from guest RSP.
// Its owner keeps the allocation alive through this synchronous Run. The
// callback runs under the execution barrier and must deny managed guards,
// reservations or insufficient logical rights, including inside the claimed
// interval. Missing proof and out-of-range accesses fail closed. Ordinary V3
// has no owned-stack exception; this adds no general unmanaged-pointer access.
extern "C" std::uint32_t LumenMacOSFexCoreRunGuestWithOwnedStackV3(
    LumenAndroidFexCoreGuestStateV3* state,
    LumenAndroidFexCoreHostCallV2 hostCall,
    LumenAndroidFexCoreExecutableQueryV1 executableQuery,
    void* opaque, std::uint64_t ownedBase, std::uint64_t ownedSize,
    LumenMacOSFexCoreOwnedStackAccessQueryV1 ownedStackQuery) noexcept;

// Optional synchronous host-call read from the active Run's exact owned stack
// or a still-live ancestor's. It is enabled by default; any explicit value
// other than exact "1" for LUMEN_EXPERIMENTAL_FEX_HLE_OWNED_STACK_READ selects
// the checked fallback. The host caller owns a writable destination buffer of
// at least size bytes; this API is not a registered guest HLE target. One
// means the copy completed. Zero leaves the destination unchanged and requires
// the ordinary checked reader as fallback.
// Requires a counted host-memory reader, never a quiescent wait or mutation,
// and rechecks the owner's logical-permission proof for the complete range.
// It does not cache arbitrary native permissions or admit other guest buffers.
extern "C" std::uint32_t LumenMacOSFexCoreTryReadOwnedStackForHostCallV1(
    std::uint64_t address, void* destination, std::uint64_t size) noexcept;

// Diagnostic host observer only; the caller selects exact "1" for
// LUMEN_FEX_MEMSET_OWNED_STACK_TRACE. Samples one in 64 calls and emits bounded
// aggregates, never a pointer, fill, lease or access authorization. Checks the
// same live owner/host-reader invariants and logical write proof as a possible
// synchronous owned-stack fill would require. The caller must still perform
// the ordinary checked write regardless of the observation.
extern "C" void LumenMacOSFexCoreObserveMemsetOwnedStackCoverageV1(
    std::uint64_t address, std::uint64_t size) noexcept;
#endif

enum LumenAndroidFexCoreGuestMemoryMutationKindV2 : std::uint32_t {
    LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2 = 0,
    LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_ADDITIVE_MAPPING_V2 = 1,
};

// Android Core::VirtualMemory calls this optional ABI around every managed
// mapping mutation. A zero token denies the mutation. V1 is conservatively
// exclusive; V2 permits non-replacing allocations to select the additive
// scope. End publishes the affected range while admission is still closed;
// committed reports a mapping or executable-byte effect requiring translator
// invalidation. A failed/empty mapping effect, or a CPU data copy proven to
// reach no executable physical alias, publishes zero. The writer epoch and
// local-release callback still close normally for an empty publication.
extern "C" std::uint64_t
LumenAndroidFexCoreBeginGuestMemoryMutationV1() noexcept;

// V2 classifies mappings which only add a previously unused address range.
// V1 remains ABI-stable and always requests an exclusive mutation.
// Same-thread mutations may nest under an existing exclusive epoch. Each End
// still publishes its own range and local-release callback; only the final
// close disarms safepoints and readmits guest execution. The owner must not
// enter guest code/callbacks inside an epoch. An additive epoch cannot upgrade
// to exclusive, and a denied begin authorizes no mutation.
extern "C" std::uint64_t LumenAndroidFexCoreBeginGuestMemoryMutationV2(
    std::uint32_t kind) noexcept;

// Optional effect-free denial metadata. An additive or exclusive request
// competing with an exclusive mutation in a top-level HLE context eligible for
// quiescent host wait sets retryable=1.
// The caller must release its own locks/borrows before that wait and retry
// only after readmission. Success, invalid context and other denials set 0.
extern "C" std::uint64_t LumenAndroidFexCoreBeginGuestMemoryMutationV3(
    std::uint32_t kind, std::uint32_t* retryable) noexcept;

// Authoritative current-thread ownership query; stale or foreign tokens fail.
extern "C" std::uint32_t LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(
    std::uint64_t token) noexcept;

// Borrow only an already-held top-level native exclusive epoch. While borrowed,
// nested VM mutation and epoch close/readmission fail before effects. The token
// may move to a native completion thread, which releases it only after the GPU
// fence and destruction of every pointer-bearing imported object. This grants
// no memory rights, independent lifetime, or CPU/GPU concurrency. Zero denies.
extern "C" std::uint64_t LumenAndroidFexCoreRetainExclusivePublicationBorrowV1() noexcept;
extern "C" std::uint32_t LumenAndroidFexCoreReleaseExclusivePublicationBorrowV1(
    std::uint64_t token) noexcept;

// Current-thread proof of a counted CPU execution or HLE memory reader.
// The caller must retain that admission through its actual memory access.
// Quiescent waits, mutation owners and native Core borrows alone return zero.
// This query neither acquires admission nor relies on diagnostic run flags.
extern "C" std::uint32_t
LumenAndroidFexCoreOwnsGuestMemoryReadLeaseV1() noexcept;

// Thread-affine identity of one continuously counted FEX execution reader.
// Changes on release/readmission; zero outside execution, during HLE/mutation
// or on nonce exhaustion. Unlike the ownership query, this may key metadata
// reuse only while that same execution reader remains live through the access.
extern "C" std::uint64_t
LumenAndroidFexCoreGuestExecutionReadAdmissionEpochV1() noexcept;

extern "C" void LumenAndroidFexCoreEndGuestMemoryMutationV1(
    std::uint64_t token, std::uint64_t rangeBase,
    std::uint64_t rangeSize, std::uint32_t committed) noexcept;

using LumenAndroidFexCoreMutationLocalReleaseV2 =
    void (*)(void* opaque) noexcept;

// Publishes the range, then invokes releaseLocalGate on the mutation owner
// before disarming safepoints and reopening FEX execution admission. The
// callback also runs for an aborted/empty publication, but never for an
// invalid, cross-thread, reused or reentrant close. It must not throw, acquire
// a new guest-memory lease, or reenter guest execution/mutation. Returns one
// only after a valid close; a zero result leaves local gate cleanup to the
// caller's fail-closed path. V1 retains its existing no-callback behavior.
extern "C" std::uint32_t LumenAndroidFexCoreEndGuestMemoryMutationV2(
    std::uint64_t token, std::uint64_t rangeBase,
    std::uint64_t rangeSize, std::uint32_t committed,
    LumenAndroidFexCoreMutationLocalReleaseV2 releaseLocalGate,
    void* opaque) noexcept;

// Generated-code boundary interrupt, shared by the active FEX hosts.
//
// A guest thread executing translated code cannot observe a queue written by
// another thread until it reaches the next block boundary. The run captures
// its host pthread identity at admission (the value its own thread observes
// from pthread_self) and the requester passes that identity. A nonzero return
// is the one-shot token of the armed boundary; zero means no active run
// matched, so the caller keeps its ordinary delivery route. The armed run
// delivers once through the registered callback and then resumes at the
// reconstructed guest boundary state. Cancel clears exactly that token and is
// idempotent; it must never be used to clear a newer request.
using LumenAndroidFexCoreBoundaryDeliveryV1 = std::uint32_t (*)(
    const std::uint64_t* gprs, std::uint64_t rip) noexcept;

enum LumenAndroidFexCoreBoundaryDispositionV1 : std::uint32_t {
    LUMEN_ANDROID_FEXCORE_BOUNDARY_IDLE_V1 = 0,
    LUMEN_ANDROID_FEXCORE_BOUNDARY_DELIVERED_V1 = 1,
    LUMEN_ANDROID_FEXCORE_BOUNDARY_STOP_V1 = 2,
};

// Process-lifetime registration; only the same callback is accepted again.
// The callback runs on the guest thread at the boundary, inside the pause
// path, with the guest x86-64 GP state (0=rax .. 4=rsp .. 15=r15) and the
// reconstructed resume RIP. It may run the handler as a nested guest call and
// returns DELIVERED when it consumed a pending request, or STOP to terminate
// this guest run after ordinary execution readmission and runtime teardown.
// IDLE means no handler ran and is not a protocol failure. It must not throw,
// allocate signal-unsafe state or longjmp out of the call.
extern "C" std::uint32_t LumenAndroidFexCoreRegisterBoundaryDeliveryV1(
    LumenAndroidFexCoreBoundaryDeliveryV1 delivery) noexcept;

extern "C" std::uint64_t LumenAndroidFexCoreRequestGuestBoundaryInterruptV1(
    std::uint64_t hostThread) noexcept;
extern "C" std::uint32_t LumenAndroidFexCoreCancelGuestBoundaryInterruptV1(
    std::uint64_t token) noexcept;

// Diagnostic identity for multiplexed HLE dispatch. The name must be immutable
// registry storage alive until all runtime calls finish. A nested call restores
// its parent's identity on return. This never changes admission or ownership.
extern "C" void LumenAndroidFexCoreSetHostCallDiagnosticV1(
    std::uint64_t operation, const char* name) noexcept;

// Explicit top-level HLE wait with no guest-memory accesses or outstanding VM
// leases. Inputs must already be copied and native objects kept alive through
// the wait. Begin returns zero outside an eligible active Run, preserving the
// ordinary host-memory reader. A nonzero token is thread-affine and single-use;
// End returns one only after host-memory readmission. Guest pointers must then
// be revalidated before use. A failed End must not resume guest-pointer access.
extern "C" std::uint64_t
LumenAndroidFexCoreBeginQuiescentHostWaitV1() noexcept;
extern "C" std::uint32_t LumenAndroidFexCoreEndQuiescentHostWaitV1(
    std::uint64_t token) noexcept;

// Explicit synchronous callback into a fresh guest root. Begin requires the
// active top-level HLE reader, proven RW parent stack/TLS bounds and an RX
// continuation. It watches their mapping lifetimes before donating the reader.
// The nonzero token is thread-affine and single-use. End restores the reader
// and revalidates the parent; a matched End failure permanently denies its CPU
// readmission. Foreign, wrong and consumed tokens grant nothing and leave the
// original donation intact.
// Validate is available only while the donated parent owns an execution or
// mutation lease. Mapping publishers notify actual mapping changes, including
// same-permission changes and reuse; zero/overflowing size revokes every watch.
extern "C" std::uint64_t LumenAndroidFexCoreBeginFreshGuestCallbackV1(
    std::uint64_t stackBase, std::uint64_t stackSize,
    std::uint64_t tlsBase, std::uint64_t tlsSize) noexcept;
extern "C" std::uint32_t LumenAndroidFexCoreEndFreshGuestCallbackV1(
    std::uint64_t token) noexcept;
extern "C" std::uint32_t
LumenAndroidFexCoreValidateFreshGuestCallbackParentV1() noexcept;
extern "C" void LumenAndroidFexCoreNotifyFreshGuestCallbackMappingChangeV1(
    std::uint64_t base, std::uint64_t size) noexcept;

// Complete managed-data permission proof, called only under a counted HLE,
// execution or mutation lease, outside signal handlers. It must not touch
// unchecked guest bytes. Android fresh callbacks require this proof (or an
// exact owned stack proof); Apple also uses its existing managed-data query.
// Registration is process-lifetime, single-assignment and same-query idempotent.
using LumenAndroidFexCoreFreshCallbackDataAccessQueryV1 = std::uint32_t (*)(
    std::uint64_t address, std::uint64_t size, std::uint32_t write) noexcept;
extern "C" std::uint32_t LumenAndroidFexCoreRegisterFreshCallbackDataAccessQueryV1(
    LumenAndroidFexCoreFreshCallbackDataAccessQueryV1 query) noexcept;

// Retains only a currently counted synchronous HLE host-memory reader. Core
// must separately validate all logical guest pages and physical permissions.
// Nested borrows are counted; each successful retain requires one release on
// the same thread before HLE return, guest reentry, mutation or quiescent wait.
// Zero grants no pointer access. Release returns zero outside the owning HLE
// reader or without a matching retain; Core must enforce per-borrow affinity.
extern "C" std::uint32_t
LumenAndroidFexCoreRetainGuestDataLeaseV1() noexcept;
extern "C" std::uint32_t
LumenAndroidFexCoreReleaseGuestDataLeaseV1() noexcept;

// Native-worker mapping lifetime pin. Unlike RetainGuestDataLeaseV1, this
// admits an independent native root without a synchronous HLE reader and does
// not park translated CPU execution. TryBegin never blocks: zero grants no
// pointer access. A nonzero token is thread-affine and single-use, and forbids
// guest Run, mutation, HLE reentry or quiescent-wait donation on its owner.
// Every logical range/permission and physical executable alias must still be
// validated by Core. Concurrent guest bytes are not a coherent snapshot.
// Exclusive mutations close native admission and wait for all pins before
// arming CPU safepoints. Proven fresh additive maps retain existing mappings
// and access snapshots; they may coexist with an already admitted pin.
// Complete every guest-pointer callback before End. A delegated publisher
// running under another thread's CPU-exclusive mutation takes no native pin.
extern "C" std::uint64_t
LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV1() noexcept;
// Same nonblocking admission as V1. Reset output to zero; set it to one only
// for a valid native context denied by a pending/active exclusive mutation.
// It grants no pointer access or permission to retry while holding other locks.
extern "C" std::uint64_t LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV2(
    std::uint32_t* mutation_pending) noexcept;
// Atomically replace the calling native worker's outer exclusive mutation
// with a native mapping pin, after disarming that mutation's safepoints. Zero
// leaves the mutation unchanged. Only a native root with no guest leases,
// nested mutation, HLE/execution upgrade or publication borrow is eligible.
// Byte-access claims must already be published; the pin does not replace them.
extern "C" std::uint64_t LumenAndroidFexCoreDowngradeExclusiveToNativeGuestDataLeaseV1(
    std::uint64_t mutation_token) noexcept;
// Thread-local ownership query only; this does not acquire or retain a lease.
extern "C" std::uint32_t LumenAndroidFexCoreOwnsNativeGuestDataLeaseV1(
    std::uint64_t token) noexcept;
extern "C" std::uint32_t LumenAndroidFexCoreEndNativeGuestDataLeaseV1(
    std::uint64_t token) noexcept;

#if defined(__APPLE__)
// Signal-safe permission proof for a complete managed guest data interval.
// Called only under the active FEX execution/mutation barrier, before the
// unaligned-access helper or native bridge stack reader can touch memory.
// The callback must never lock,
// allocate, emit diagnostics or inspect memory through an unvalidated pointer.
// Registration is process-lifetime and accepts only the same callback again.
using LumenMacOSFexCoreDataAccessQueryV1 = std::uint32_t (*)(
    std::uint64_t address, std::uint64_t size, std::uint32_t write) noexcept;
extern "C" std::uint32_t LumenMacOSFexCoreRegisterDataAccessQueryV1(
    LumenMacOSFexCoreDataAccessQueryV1 query) noexcept;

// Diagnostic identity, borrowed only inside a synchronous guest host callback.
// Zero means no active FEX run. The token must never be dereferenced or retained
// as a cache handle: storage can be recycled after the active run ends.
extern "C" std::uint64_t LumenMacOSFexCoreProbeActiveCodeCacheV1() noexcept;
#endif
