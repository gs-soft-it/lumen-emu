#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUESTDATALEASERETRY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUESTDATALEASERETRY_H_

#include "Lumen/Core/VirtualMemory.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Lumen::Libs::Graphics {

struct GraphicsGuestLeaseResult
{
	const char* reason = nullptr;
	uint64_t observed_generation = UINT64_MAX;
	uint32_t attempts = 0;
};

struct GraphicsGuestLeaseAcquire
{
	bool operator()(uint64_t begin, uint64_t bytes, Core::VirtualMemory::Mode mode,
	                Core::VirtualMemory::GuestDataLease* output) const
	{
		return Core::VirtualMemory::TryAcquireGuestDataLease(begin, bytes, mode, output, false);
	}
};

// This is a bounded retry of the nonblocking borrow only. Subsystem locks may
// remain held: there is no sleep, yield, unlock, callback or GPU operation.
// A false VM result can also mean denied rights; it is never called proof of
// contention. An available immutable snapshot can reject a known denial, but
// only a real lease authorizes the later complete mapping/protection check.
namespace Detail {

enum class GraphicsGuestLeasePolicy { FixedGeneration, CurrentReadObservation, CurrentDestinationCapture, OriginalRangeContinuity };

template <typename Acquire>
bool TryAcquireGraphicsGuestLeaseWithPolicy(uint64_t address, uint64_t bytes, Core::VirtualMemory::Mode mode,
	                                         uint64_t expected_generation, Core::VirtualMemory::GuestDataLease* lease,
	                                         GraphicsGuestLeaseResult* result, Acquire&& acquire, GraphicsGuestLeasePolicy policy)
{
	using namespace Core::VirtualMemory;
	if (result != nullptr) *result = {};
	const auto reject = [result](const char* reason, uint32_t attempts, uint64_t observed = UINT64_MAX) {
		if (result != nullptr) *result = {reason, observed, attempts};
		return false;
	};
	if (lease == nullptr || lease->IsActive()) return reject("lease-state", 0);
	if (address == 0 || bytes == 0 || address > UINTPTR_MAX || bytes > UINTPTR_MAX - address ||
	    bytes > std::numeric_limits<size_t>::max() ||
	    (mode != Mode::Read && mode != Mode::Write && mode != Mode::ReadWrite)) return reject("destination-shape", 0);
	const bool fixed_generation = policy == GraphicsGuestLeasePolicy::FixedGeneration;
	const bool original_continuity = policy == GraphicsGuestLeasePolicy::OriginalRangeContinuity;
	const bool current_capture = policy == GraphicsGuestLeasePolicy::CurrentDestinationCapture;
	if (policy == GraphicsGuestLeasePolicy::CurrentReadObservation && mode != Mode::Read) return reject("observation-policy", 0);
	if (policy == GraphicsGuestLeasePolicy::CurrentDestinationCapture && mode != Mode::Write && mode != Mode::ReadWrite)
		return reject("capture-policy", 0);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	if ((fixed_generation || original_continuity) && expected_generation == UINT64_MAX) return reject("mapping-generation-unavailable", 0);
	constexpr uint32_t max_attempts = 16;
	const auto required = static_cast<uint32_t>(mode);
	for (uint32_t attempt = 1; attempt <= max_attempts; ++attempt)
	{
		if (fixed_generation && GetMutationGeneration() != expected_generation) return reject("generation-changed", attempt - 1);
		if (acquire(address, bytes, mode, lease))
		{
			const auto observed = lease->MutationGeneration();
			const char* reason = nullptr;
			if (!lease->IsActive() || lease->HostBegin() != address || lease->ByteCount() != bytes) reason = "lease-shape";
			else if (observed == UINT64_MAX) reason = "mapping-generation-unavailable";
			else if (fixed_generation && observed != expected_generation) reason = "generation-changed";
			uint64_t cursor = address;
			// Preserve the existing graphics destination coverage bound.
			uint32_t regions = 0;
			while (reason == nullptr && cursor < address + bytes)
			{
				ManagedRegion region {};
				if (++regions > 4096) reason = "coverage-capacity";
				else if (QueryManagedRegion(cursor, &region) != ManagedRegionState::Visible) reason = "destination-unowned";
				else if (region.begin > cursor || region.end <= cursor) reason = "destination-range";
				else if (IsExecute(region.mode)) reason = "destination-executable";
				else if ((static_cast<uint32_t>(region.mode) & required) != required) reason = "destination-protection";
				else cursor = std::min(address + bytes, region.end);
			}
			if (reason == nullptr && (original_continuity || current_capture) &&
			    !ValidateGuestDataLeaseRangeContinuity(*lease, address, bytes, original_continuity ? expected_generation : observed))
				reason = "mapping-continuity-changed";
			if (reason != nullptr)
			{
				ReleaseGuestDataLease(lease);
				return reject(reason, attempt, observed);
			}
			if (result != nullptr) *result = {nullptr, observed, attempt};
			return true;
		}
		// Test acquisitions may return false after a real validation. Production
		// TryAcquire returns without an active lease on failure.
		ReleaseGuestDataLease(lease);
		if (fixed_generation && GetMutationGeneration() != expected_generation) return reject("generation-changed", attempt);
		if (!fixed_generation && GetMutationGeneration() == UINT64_MAX) return reject("mapping-generation-unavailable", attempt);
		if (CurrentThreadHasMemoryLease()) return reject("retry-memory-lease-held", attempt);
		// Never enter the blocking registry fallback while no lease is held.
		uint64_t cursor = address;
		uint32_t regions = 0;
		while (cursor < address + bytes)
		{
			ManagedRegionState state = ManagedRegionState::Unmanaged;
			ManagedRegion region {};
			if (!TryQueryManagedRegionSnapshot(cursor, &state, &region)) break;
			if (++regions > 4096) return reject("coverage-capacity", attempt);
			if (state != ManagedRegionState::Visible) return reject("destination-unowned", attempt);
			if (region.begin > cursor || region.end <= cursor) return reject("destination-range", attempt);
			if (IsExecute(region.mode)) return reject("destination-executable", attempt);
			if ((static_cast<uint32_t>(region.mode) & required) != required) return reject("destination-protection", attempt);
			cursor = std::min(address + bytes, region.end);
		}
		if (attempt == max_attempts) return reject("lease-acquisition-retry-exhausted", attempt);
	}
#else
	(void)expected_generation; (void)acquire; (void)fixed_generation; (void)original_continuity; (void)current_capture;
	return reject("mapping-lease-capability-unavailable", 0);
#endif
	return false;
}

} // namespace Detail

template <typename Acquire>
bool TryAcquireGraphicsGuestLease(uint64_t address, uint64_t bytes, Core::VirtualMemory::Mode mode,
	                               uint64_t expected_generation, Core::VirtualMemory::GuestDataLease* lease,
	                               GraphicsGuestLeaseResult* result, Acquire&& acquire)
{
	return Detail::TryAcquireGraphicsGuestLeaseWithPolicy(address, bytes, mode, expected_generation, lease, result, acquire,
	                                                    Detail::GraphicsGuestLeasePolicy::FixedGeneration);
}

// A fresh read has no previously captured VM generation to preserve. Only the
// successful lease authorizes its one current content observation; a failed
// borrow performs no read and cannot renew a publication or resource ticket.
// Managed coverage, physical permissions and non-executable rights retain the
// same validation as fixed-generation borrows, with the same bounded retry.
template <typename Acquire>
bool TryAcquireGraphicsGuestReadObservation(uint64_t address, uint64_t bytes,
	                                         Core::VirtualMemory::GuestDataLease* lease,
	                                         GraphicsGuestLeaseResult* result, Acquire&& acquire)
{
	return Detail::TryAcquireGraphicsGuestLeaseWithPolicy(address, bytes, Core::VirtualMemory::Mode::Read,
	                                                    UINT64_MAX, lease, result, acquire,
	                                                    Detail::GraphicsGuestLeasePolicy::CurrentReadObservation);
}

// Capture is the initial checked borrow, before any preparation or effects.
// Its successful lease supplies the original token; retries capture no token.
// Core continuity at that token also refuses unresolved mutation watermarks or
// mutation-owner borrows before a caller can enroll any deferred operation.
template <typename Acquire>
bool TryAcquireGraphicsGuestCaptureLease(uint64_t address, uint64_t bytes, Core::VirtualMemory::Mode mode,
	                                      Core::VirtualMemory::GuestDataLease* lease, GraphicsGuestLeaseResult* result, Acquire&& acquire)
{
	return Detail::TryAcquireGraphicsGuestLeaseWithPolicy(address, bytes, mode, UINT64_MAX, lease, result, acquire,
	                                                    Detail::GraphicsGuestLeasePolicy::CurrentDestinationCapture);
}

inline bool TryAcquireGraphicsGuestCaptureLease(uint64_t address, uint64_t bytes, Core::VirtualMemory::Mode mode,
	                                             Core::VirtualMemory::GuestDataLease* lease, GraphicsGuestLeaseResult* result = nullptr)
{
	return TryAcquireGraphicsGuestCaptureLease(address, bytes, mode, lease, result, GraphicsGuestLeaseAcquire {});
}

// Preserve the original token across waits/preparation. A newer global lease
// generation is admitted only when Core proves this complete managed range
// has had no mapping, protection or patch mutation since that original token.
template <typename Acquire>
bool TryAcquireGraphicsGuestContinuityLease(uint64_t address, uint64_t bytes, Core::VirtualMemory::Mode mode,
	                                         uint64_t original_generation, Core::VirtualMemory::GuestDataLease* lease,
	                                         GraphicsGuestLeaseResult* result, Acquire&& acquire)
{
	return Detail::TryAcquireGraphicsGuestLeaseWithPolicy(address, bytes, mode, original_generation, lease, result, acquire,
	                                                    Detail::GraphicsGuestLeasePolicy::OriginalRangeContinuity);
}

inline bool TryAcquireGraphicsGuestContinuityLease(uint64_t address, uint64_t bytes, Core::VirtualMemory::Mode mode,
	                                                uint64_t original_generation, Core::VirtualMemory::GuestDataLease* lease,
	                                                GraphicsGuestLeaseResult* result = nullptr)
{
	return TryAcquireGraphicsGuestContinuityLease(address, bytes, mode, original_generation, lease, result, GraphicsGuestLeaseAcquire {});
}

// Only an exact managed lease already owned by this thread may extend the
// reader gate beneath subsystem locks. Core's ordinary acquisition skips its
// writer condition-variable wait for that proven parent, even with a writer
// pending. The first borrow still uses the nonblocking helper above. Complete
// child coverage, permissions and original-token continuity remain unchanged.
inline bool TryAcquireGraphicsGuestContinuityLeaseUnderParent(uint64_t address, uint64_t bytes, Core::VirtualMemory::Mode mode,
	                                                           uint64_t original_generation,
	                                                           const Core::VirtualMemory::GuestDataLease& parent,
	                                                           Core::VirtualMemory::GuestDataLease* lease,
	                                                           GraphicsGuestLeaseResult* result = nullptr)
{
	if (result != nullptr) *result = {};
	if (lease == &parent || !Core::VirtualMemory::ValidateGuestDataLeaseRangeContinuity(parent,
	        parent.HostBegin(), parent.ByteCount(), original_generation))
	{
		if (result != nullptr) *result = {"parent-lease-continuity-unproven", parent.MutationGeneration(), 0};
		return false;
	}
	return TryAcquireGraphicsGuestContinuityLease(address, bytes, mode, original_generation, lease, result,
	    [](uint64_t begin, uint64_t count, Core::VirtualMemory::Mode required, Core::VirtualMemory::GuestDataLease* output) {
		    return Core::VirtualMemory::TryAcquireGuestDataLease(begin, count, required, output, true);
	    });
}

inline bool TryAcquireGraphicsGuestReadObservation(uint64_t address, uint64_t bytes,
	                                                Core::VirtualMemory::GuestDataLease* lease,
	                                                GraphicsGuestLeaseResult* result = nullptr)
{
	return TryAcquireGraphicsGuestReadObservation(address, bytes, lease, result, GraphicsGuestLeaseAcquire {});
}

inline bool TryAcquireGraphicsGuestLease(uint64_t address, uint64_t bytes, Core::VirtualMemory::Mode mode,
	                                      uint64_t expected_generation, Core::VirtualMemory::GuestDataLease* lease,
	                                      GraphicsGuestLeaseResult* result = nullptr)
{
	return TryAcquireGraphicsGuestLease(address, bytes, mode, expected_generation, lease, result, GraphicsGuestLeaseAcquire {});
}

} // namespace Lumen::Libs::Graphics

#endif
