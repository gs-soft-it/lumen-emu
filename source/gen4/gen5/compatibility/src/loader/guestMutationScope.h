// Lumen, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "loader/guestCoordinationFailure.h"
#include "loader/guestWriterGate.h"
#if defined(LUMEN_PS5_FEXCORE)
#include "AndroidFexCoreRuntime.h"
#endif

#include <cstdint>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <thread>

namespace Loader::GuestExecution {

// Acquire before any loader/TLS mutex whose owner can mutate guest mappings.
// Declare before the local lock so its destructor unlocks first. Nested VM
// operations publish/invalidate their own ranges while this outer epoch keeps
// admission closed. This scope publishes no additional range at close.
// No guest Invoke, callback or application-heap callback may run inside it.
class GuestMutationScope final {
public:
    enum class Admission { Strict, FreshScalarEntry, NativeRootEntry };

    // FreshScalarEntry is an explicit caller promise: before this scope,
    // retain only scalar/address values or native-owned snapshots, with no
    // guest/VM leases, local locks or effects awaiting replay. A failed Begin
    // may donate only an eligible top-level HLE reader while a competing
    // mutation finishes. End readmits that reader before the next Begin;
    // callers must then revalidate every guest input/output under the epoch.
    // Retained outer callback frames remain ineligible and fail closed.
    // Retries are bounded by count; the wait ABI has no independent timeout.
    explicit GuestMutationScope(Admission admission = Admission::Strict) noexcept
        : gate_(admission == Admission::FreshScalarEntry
                    ? GuestWriterGate::Admission::FreshScalarEntry
                : admission == Admission::NativeRootEntry
                    ? GuestWriterGate::Admission::NativeRootEntry
                    : GuestWriterGate::Admission::Strict) {
#if defined(LUMEN_PS5_DIRECT_FEX)
        if (!gate_.Acquired())
            LUMEN_PS5_GUEST_COORDINATION_FAILURE("PS5 loader writer lock denied\n");
#elif defined(LUMEN_PS5_FEXCORE)
        if (!gate_.Acquired())
            LUMEN_PS5_GUEST_COORDINATION_FAILURE("PS5 loader writer-gate admission denied\n");
        token_ = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        std::uint32_t retries = 0;
        while (token_ == 0 && admission == Admission::FreshScalarEntry && retries < 8) {
            const auto wait_token = LumenAndroidFexCoreBeginQuiescentHostWaitV1();
            if (wait_token == 0) break;
            // Avoid repeatedly readmitting a herd of scalar HLE mutators in
            // the same scheduling quantum. No guest lease is held here.
            std::this_thread::sleep_for(std::chrono::milliseconds(1u << retries));
            if (LumenAndroidFexCoreEndQuiescentHostWaitV1(wait_token) == 0)
                LUMEN_PS5_GUEST_COORDINATION_FAILURE("PS5 loader guest-memory mutation readmission failed\n");
            ++retries;
            token_ = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        }
        if (token_ == 0) LUMEN_PS5_GUEST_COORDINATION_FAILURE("PS5 loader guest-memory mutation admission denied\n");
        if (retries != 0) {
            static std::atomic<std::uint32_t> receipts {0};
            if (receipts.fetch_add(1, std::memory_order_relaxed) < 32)
                std::fprintf(stderr, "PS5_LOADER_ADMISSION_RETRY_COMPLETED retries=%u\n", retries);
        }
#else
        (void)admission;
#endif
    }
    ~GuestMutationScope() {
#if defined(LUMEN_PS5_FEXCORE) && !defined(LUMEN_PS5_DIRECT_FEX)
        if (LumenAndroidFexCoreEndGuestMemoryMutationV2(token_, 0, 0, 0, nullptr, nullptr) == 0)
            LUMEN_PS5_GUEST_COORDINATION_FAILURE("PS5 loader guest-memory mutation close failed\n");
#endif
    }
    GuestMutationScope(const GuestMutationScope&) = delete;
    GuestMutationScope& operator=(const GuestMutationScope&) = delete;
private:
    GuestWriterGate gate_;
    std::uint64_t token_ {};
};

} // namespace Loader::GuestExecution
