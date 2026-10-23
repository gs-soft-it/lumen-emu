#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_WRITE_DESTINATION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_WRITE_DESTINATION_H_

#include "Emulator/Graphics/GuestDataLeaseRetry.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace Lumen::Libs::Graphics {

// A value snapshot, never a lease transferred across threads or GPU waits.
// Capture before preparation; Publish accepts an already materialized host
// source whose lifetime and non-overlap remain the caller's responsibility.
// Only the Mac VM currently pins mappings. Other backends retain their legacy
// mapping-lifetime limit while sharing shape checks and staging in the caller.
class GuestWriteDestination
{
public:
	bool Capture(uint64_t address, uint64_t bytes, uint64_t alignment = 1, GraphicsGuestLeaseResult* result = nullptr,
	             uint64_t original_generation = UINT64_MAX)
	{
		*this = {};
		if (result != nullptr) *result = {};
		constexpr uint64_t pointer_max = std::numeric_limits<uintptr_t>::max();
		if (address == 0 || bytes == 0 || alignment == 0 || address % alignment != 0 ||
		    address > pointer_max || bytes > pointer_max - address || bytes > std::numeric_limits<size_t>::max())
		{
			if (result != nullptr) result->reason = "destination-shape";
			return false;
		}
		Core::VirtualMemory::GuestDataLease lease;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		if (original_generation == UINT64_MAX)
		{
			if (!TryAcquireGraphicsGuestCaptureLease(address, bytes, Core::VirtualMemory::Mode::Write, &lease, result)) return false;
		} else if (!TryAcquireGraphicsGuestContinuityLease(address, bytes, Core::VirtualMemory::Mode::Write,
		                                                  original_generation, &lease, result)) return false;
#else
		(void)original_generation;
		if (!Borrow(address, bytes, 0, &lease, result)) return false;
#endif
		m_address = address;
		m_size = bytes;
		m_generation = original_generation != UINT64_MAX ? original_generation : lease.MutationGeneration();
		return true;
	}

	bool Validate(GraphicsGuestLeaseResult* result = nullptr) const
	{
		if (result != nullptr) *result = {};
		if (m_address == 0 || m_size == 0)
		{
			if (result != nullptr) result->reason = "destination-shape";
			return false;
		}
		Core::VirtualMemory::GuestDataLease lease;
		return Borrow(m_address, m_size, m_generation, &lease, result);
	}

	// Capture under an exact managed caller pin before any producer effects.
	// Child write rights and the original token are proved independently.
	bool CaptureUnderParent(uint64_t address, uint64_t bytes, uint64_t alignment, uint64_t original_generation,
	    const Core::VirtualMemory::GuestDataLease& parent, GraphicsGuestLeaseResult* result = nullptr)
	{
		*this = {};
		if (result != nullptr) *result = {};
		if (address == 0 || bytes == 0 || alignment == 0 || address % alignment != 0 ||
		    address > UINTPTR_MAX || bytes > UINTPTR_MAX - address || bytes > std::numeric_limits<size_t>::max())
		{
			if (result != nullptr) result->reason = "destination-shape";
			return false;
		}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		Core::VirtualMemory::GuestDataLease child;
		if (!TryAcquireGraphicsGuestContinuityLeaseUnderParent(address, bytes, Core::VirtualMemory::Mode::Write,
		        original_generation, parent, &child, result)) return false;
		m_address = address;
		m_size = bytes;
		m_generation = original_generation;
		return true;
#else
		(void)original_generation; (void)parent;
		if (result != nullptr) result->reason = "parent-lease-capability-unavailable";
		return false;
#endif
	}

	bool Publish(const void* source, GraphicsGuestLeaseResult* result = nullptr) const
	{
		return PublishWithAcquisition(source, result, GraphicsGuestLeaseAcquire {}, [](const auto&) { return true; });
	}

	// The final footprint check and the one store share the destination lease.
	// The guard must inspect only current metadata/bytes: no wait, GPU operation,
	// preparation or external callback. Failure publishes nothing.
	template <typename ValidateFootprint>
	bool PublishGuarded(const void* source, ValidateFootprint&& validate_footprint, GraphicsGuestLeaseResult* result = nullptr) const
	{
		return PublishWithAcquisition(source, result, GraphicsGuestLeaseAcquire {}, static_cast<ValidateFootprint&&>(validate_footprint));
	}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	template <typename Acquire>
	bool PublishWithAcquisitionForTest(const void* source, GraphicsGuestLeaseResult* result, Acquire&& acquire) const
	{
		return PublishWithAcquisition(source, result, static_cast<Acquire&&>(acquire), [](const auto&) { return true; });
	}
#endif

	uint64_t Address() const { return m_address; }
	uint64_t Size() const { return m_size; }
	uint64_t Generation() const { return m_generation; }

private:
	template <typename Acquire, typename ValidateFootprint>
	bool PublishWithAcquisition(const void* source, GraphicsGuestLeaseResult* result, Acquire&& acquire,
	                            ValidateFootprint&& validate_footprint) const
	{
		if (result != nullptr) *result = {};
		Core::VirtualMemory::GuestDataLease lease;
		if (source == nullptr || m_address == 0 || m_size == 0)
		{
			if (result != nullptr) result->reason = "publication-shape";
			return false;
		}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		if (!TryAcquireGraphicsGuestContinuityLease(m_address, m_size, Core::VirtualMemory::Mode::Write, m_generation,
		                                           &lease, result, static_cast<Acquire&&>(acquire))) return false;
#else
		(void)acquire;
		if (!Borrow(m_address, m_size, m_generation, &lease, result)) return false;
#endif
		if (!validate_footprint(lease))
		{
			if (result != nullptr) result->reason = "publication-footprint-invalid";
			return false;
		}
		std::memcpy(reinterpret_cast<void*>(m_address), source, static_cast<size_t>(m_size));
		return true;
	}

	static bool Borrow(uint64_t address, uint64_t bytes, uint64_t generation,
	                   Core::VirtualMemory::GuestDataLease* lease, GraphicsGuestLeaseResult* result)
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		return TryAcquireGraphicsGuestContinuityLease(address, bytes, Core::VirtualMemory::Mode::Write, generation, lease, result);
#else
		(void)address; (void)bytes; (void)generation; (void)lease; (void)result;
#endif
		return true;
	}

	uint64_t m_address = 0;
	uint64_t m_size = 0;
	uint64_t m_generation = 0;
};

} // namespace Lumen::Libs::Graphics

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_WRITE_DESTINATION_H_
