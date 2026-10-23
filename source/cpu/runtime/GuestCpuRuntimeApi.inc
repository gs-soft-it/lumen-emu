// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
// Repeated with LUMEN_GUEST_CPU_API: requirement, field, legacy ABI, result,
// parameters, call arguments, fail-closed result. No implementation is inferred
// from a matching name; the selected provider supplies each production method.
LUMEN_GUEST_CPU_API(Required, runtimeProbe, LumenAndroidFexCoreRuntimeProbeV1,
    std::uint32_t, (LumenAndroidFexCoreRuntimeStatusV1* status), (status), DeniedProbe(status))
LUMEN_GUEST_CPU_API(Required, queryGuestTsc, LumenAndroidFexCoreQueryGuestTscV1,
    std::uint32_t, (LumenAndroidFexCoreGuestTscV1* output), (output), DeniedTsc(output))
LUMEN_GUEST_CPU_API(Optional, fpBoundaryProbe, LumenAndroidFexCoreFpBoundaryProbeV1,
    std::uint32_t, (LumenAndroidFexCoreFpBoundaryProbeStatusV1* status), (status), DeniedFp(status))
LUMEN_GUEST_CPU_API(Required, runtimeReceipt, LumenAndroidFexCoreRuntimeReceiptV1,
    const char*, (), (), DeniedReceipt())
LUMEN_GUEST_CPU_API(Optional, lastSafepointSignalCode, LumenAndroidFexCoreLastSafepointSignalCodeV1,
    std::uint32_t, (), (), 0)
LUMEN_GUEST_CPU_API(Optional, hostTargetSnapshotContractTest, LumenAndroidFexCoreHostTargetSnapshotContractTestV1,
    std::uint32_t, (), (), 0)
LUMEN_GUEST_CPU_API(Required, registerHostCallTarget, LumenAndroidFexCoreRegisterHostCallTargetV1,
    std::uint32_t, (std::uint64_t target), (target), 0)
LUMEN_GUEST_CPU_API(Required, rootReturnAddress, LumenAndroidFexCoreRootReturnAddressV1,
    std::uint64_t, (), (), 0)
LUMEN_GUEST_CPU_API(Required, runGuestV1, LumenAndroidFexCoreRunGuestV1,
    std::uint32_t, (LumenAndroidFexCoreGuestStateV1* state, LumenAndroidFexCoreHostCallV1 hostCall,
    LumenAndroidFexCoreExecutableQueryV1 executableQuery, void* opaque),
    (state, hostCall, executableQuery, opaque), LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1)
LUMEN_GUEST_CPU_API(Required, runGuestV2, LumenAndroidFexCoreRunGuestV2,
    std::uint32_t, (LumenAndroidFexCoreGuestStateV1* state, LumenAndroidFexCoreHostCallV2 hostCall,
    LumenAndroidFexCoreExecutableQueryV1 executableQuery, void* opaque),
    (state, hostCall, executableQuery, opaque), LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1)
LUMEN_GUEST_CPU_API(Required, runGuestV3, LumenAndroidFexCoreRunGuestV3,
    std::uint32_t, (LumenAndroidFexCoreGuestStateV3* state, LumenAndroidFexCoreHostCallV2 hostCall,
    LumenAndroidFexCoreExecutableQueryV1 executableQuery, void* opaque),
    (state, hostCall, executableQuery, opaque), LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1)
LUMEN_GUEST_CPU_API(Required, beginMemoryMutationV1, LumenAndroidFexCoreBeginGuestMemoryMutationV1,
    std::uint64_t, (), (), 0)
LUMEN_GUEST_CPU_API(Required, beginMemoryMutationV2, LumenAndroidFexCoreBeginGuestMemoryMutationV2,
    std::uint64_t, (std::uint32_t kind), (kind), 0)
LUMEN_GUEST_CPU_API(Required, endMemoryMutationV1, LumenAndroidFexCoreEndGuestMemoryMutationV1,
    void, (std::uint64_t token, std::uint64_t rangeBase, std::uint64_t rangeSize,
    std::uint32_t committed), (token, rangeBase, rangeSize, committed), static_cast<void>(0))
LUMEN_GUEST_CPU_API(Required, endMemoryMutationV2, LumenAndroidFexCoreEndGuestMemoryMutationV2,
    std::uint32_t, (std::uint64_t token, std::uint64_t rangeBase, std::uint64_t rangeSize,
    std::uint32_t committed, LumenAndroidFexCoreMutationLocalReleaseV2 releaseLocalGate,
    void* opaque), (token, rangeBase, rangeSize, committed, releaseLocalGate, opaque), 0)
LUMEN_GUEST_CPU_API(Required, registerBoundaryDelivery, LumenAndroidFexCoreRegisterBoundaryDeliveryV1,
    std::uint32_t, (LumenAndroidFexCoreBoundaryDeliveryV1 delivery), (delivery), 0)
LUMEN_GUEST_CPU_API(Required, requestBoundaryInterrupt, LumenAndroidFexCoreRequestGuestBoundaryInterruptV1,
    std::uint64_t, (std::uint64_t hostThread), (hostThread), 0)
LUMEN_GUEST_CPU_API(Required, cancelBoundaryInterrupt, LumenAndroidFexCoreCancelGuestBoundaryInterruptV1,
    std::uint32_t, (std::uint64_t token), (token), 0)
LUMEN_GUEST_CPU_API(Required, beginQuiescentHostWait, LumenAndroidFexCoreBeginQuiescentHostWaitV1,
    std::uint64_t, (), (), 0)
LUMEN_GUEST_CPU_API(Required, endQuiescentHostWait, LumenAndroidFexCoreEndQuiescentHostWaitV1,
    std::uint32_t, (std::uint64_t token), (token), 0)
LUMEN_GUEST_CPU_API(Required, beginFreshGuestCallback, LumenAndroidFexCoreBeginFreshGuestCallbackV1,
    std::uint64_t, (std::uint64_t stackBase, std::uint64_t stackSize,
    std::uint64_t tlsBase, std::uint64_t tlsSize), (stackBase, stackSize, tlsBase, tlsSize), 0)
LUMEN_GUEST_CPU_API(Required, endFreshGuestCallback, LumenAndroidFexCoreEndFreshGuestCallbackV1,
    std::uint32_t, (std::uint64_t token), (token), 0)
LUMEN_GUEST_CPU_API(Required, validateFreshGuestCallbackParent, LumenAndroidFexCoreValidateFreshGuestCallbackParentV1,
    std::uint32_t, (), (), 0)
LUMEN_GUEST_CPU_API(Required, notifyFreshGuestCallbackMappingChange, LumenAndroidFexCoreNotifyFreshGuestCallbackMappingChangeV1,
    void, (std::uint64_t rangeBase, std::uint64_t rangeSize),
    (rangeBase, rangeSize), static_cast<void>(0))
LUMEN_GUEST_CPU_API(Required, registerFreshCallbackDataAccessQuery, LumenAndroidFexCoreRegisterFreshCallbackDataAccessQueryV1,
    std::uint32_t, (LumenAndroidFexCoreFreshCallbackDataAccessQueryV1 query), (query), 0)
LUMEN_GUEST_CPU_API(Required, retainGuestDataLease, LumenAndroidFexCoreRetainGuestDataLeaseV1,
    std::uint32_t, (), (), 0)
LUMEN_GUEST_CPU_API(Required, releaseGuestDataLease, LumenAndroidFexCoreReleaseGuestDataLeaseV1,
    std::uint32_t, (), (), 0)
LUMEN_GUEST_CPU_API(Required, tryBeginNativeDataLease, LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV1,
    std::uint64_t, (), (), 0)
LUMEN_GUEST_CPU_API(Required, endNativeDataLease, LumenAndroidFexCoreEndNativeGuestDataLeaseV1,
    std::uint32_t, (std::uint64_t token), (token), 0)
#if defined(__APPLE__)
LUMEN_GUEST_CPU_API(Required, registerDataAccessQuery, LumenMacOSFexCoreRegisterDataAccessQueryV1,
    std::uint32_t, (LumenMacOSFexCoreDataAccessQueryV1 query), (query), 0)
LUMEN_GUEST_CPU_API(Required, runGuestWithOwnedStackV3, LumenMacOSFexCoreRunGuestWithOwnedStackV3,
    std::uint32_t, (LumenAndroidFexCoreGuestStateV3* state, LumenAndroidFexCoreHostCallV2 hostCall,
    LumenAndroidFexCoreExecutableQueryV1 executableQuery, void* opaque, std::uint64_t ownedBase,
    std::uint64_t ownedSize, LumenMacOSFexCoreOwnedStackAccessQueryV1 ownedStackQuery),
    (state, hostCall, executableQuery, opaque, ownedBase, ownedSize, ownedStackQuery),
    LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1)
LUMEN_GUEST_CPU_API(Required, tryReadOwnedStack, LumenMacOSFexCoreTryReadOwnedStackForHostCallV1,
    std::uint32_t, (std::uint64_t address, void* destination, std::uint64_t size),
    (address, destination, size), 0)
LUMEN_GUEST_CPU_API(Optional, observeMemsetOwnedStack, LumenMacOSFexCoreObserveMemsetOwnedStackCoverageV1,
    void, (std::uint64_t address, std::uint64_t size), (address, size), static_cast<void>(0))
LUMEN_GUEST_CPU_API(Optional, probeActiveCodeCache, LumenMacOSFexCoreProbeActiveCodeCacheV1,
    std::uint64_t, (), (), 0)
#endif
// Append capabilities so existing field offsets remain stable. The table's
// structSize admits only a complete known versioned layout.
// Providers without V2 retain their own V1 admission, without retry metadata.
LUMEN_GUEST_CPU_API(Optional, tryBeginNativeDataLeaseV2, LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV2,
    std::uint64_t, (std::uint32_t* mutation_pending), (mutation_pending),
    ((mutation_pending != nullptr ? *mutation_pending = 0U : 0U),
     LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV1()))
LUMEN_GUEST_CPU_API(Optional, beginMemoryMutationV3, LumenAndroidFexCoreBeginGuestMemoryMutationV3,
    std::uint64_t, (std::uint32_t kind, std::uint32_t* retryable), (kind, retryable),
    ((retryable != nullptr ? *retryable = 0U : 0U),
     LumenAndroidFexCoreBeginGuestMemoryMutationV2(kind)))
LUMEN_GUEST_CPU_API(Optional, configureMemoryHandoff, LumenAndroidFexCoreConfigureMemoryHandoffV1,
    std::uint32_t, (std::uint32_t mode), (mode), 0)

LUMEN_GUEST_CPU_API(Optional, setHostCallDiagnostic, LumenAndroidFexCoreSetHostCallDiagnosticV1,
    void, (std::uint64_t operation, const char* name), (operation, name), static_cast<void>(0))
