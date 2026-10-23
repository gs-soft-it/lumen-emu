#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_UPLOADSTAGINGARENA_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_UPLOADSTAGINGARENA_H_

#include <array>
#include <cstdint>
#include <limits>

namespace Lumen::Libs::Graphics {

// A recording exclusively owns a slab. Its cursor never wraps while commands
// can still read earlier slices; only retirement makes the whole slab reusable.
struct UploadStagingCursor
{
	uint64_t capacity = 0;
	uint64_t used = 0;

	bool Allocate(uint64_t bytes, uint64_t alignment, uint64_t* offset)
	{
		if (offset == nullptr || bytes == 0 || alignment == 0 || used > capacity) return false;
		const uint64_t padding = used % alignment == 0 ? 0 : alignment - used % alignment;
		if (padding > capacity - used || bytes > capacity - used - padding) return false;
		*offset = used + padding;
		used = *offset + bytes;
		return true;
	}
};

// Native allocations and the command-buffer completion router live outside
// this bookkeeping. Callers serialize it; allocation failures cancel their
// reservation, and only a proven fence/discard may Complete a busy lease.
class UploadStagingPoolPolicy
{
public:
	static constexpr uint64_t SlabBytes = 4u * 1024u * 1024u;
	static constexpr uint64_t MaxBytes = 64u * 1024u * 1024u;
	static constexpr uint32_t MaxSlabs = 16;
	struct Lease
	{
		uint32_t slot = MaxSlabs;
		uint64_t generation = 0;
		bool Valid() const { return slot < MaxSlabs && generation != 0; }
	};

	Lease Reserve(uint64_t bytes)
	{
		if (bytes == 0 || bytes > MaxBytes - m_bytes) return {};
		for (uint32_t i = 0; i < MaxSlabs; ++i)
		{
			auto& slot = m_slots[i];
			if (slot.bytes != 0 || slot.generation == std::numeric_limits<uint64_t>::max()) continue;
			slot.bytes = bytes;
			slot.available = false;
			++slot.generation;
			m_bytes += bytes;
			return {i, slot.generation};
		}
		return {};
	}

	Lease Acquire(uint32_t index)
	{
		if (index >= MaxSlabs) return {};
		auto& slot = m_slots[index];
		if (slot.bytes == 0 || !slot.available || slot.generation == std::numeric_limits<uint64_t>::max()) return {};
		slot.available = false;
		return {index, ++slot.generation};
	}

	bool Complete(Lease lease)
	{
		if (!Busy(lease)) return false;
		m_slots[lease.slot].available = true;
		return true;
	}

	bool CancelReservation(Lease lease)
	{
		if (!Busy(lease)) return false;
		auto& slot = m_slots[lease.slot];
		m_bytes -= slot.bytes;
		slot.bytes = 0;
		return true;
	}

	bool Busy(Lease lease) const
	{
		return lease.Valid() && m_slots[lease.slot].bytes != 0 &&
		       m_slots[lease.slot].generation == lease.generation && !m_slots[lease.slot].available;
	}
	bool Available(uint32_t index) const
	{
		return index < MaxSlabs && m_slots[index].bytes != 0 && m_slots[index].available;
	}
	bool CanReset() const
	{
		for (const auto& slot: m_slots) if (slot.bytes != 0 && !slot.available) return false;
		return true;
	}
	bool RemoveAvailable(uint32_t index)
	{
		if (!Available(index)) return false;
		auto& slot = m_slots[index];
		m_bytes -= slot.bytes;
		slot.bytes = 0;
		slot.available = false;
		return true;
	}
	bool Reset()
	{
		if (!CanReset()) return false;
		// Keep generations across sessions so stale callbacks cannot match ABA.
		for (auto& slot: m_slots) { slot.bytes = 0; slot.available = false; }
		m_bytes = 0;
		return true;
	}
	uint64_t Bytes() const { return m_bytes; }

private:
	struct Slot { uint64_t bytes = 0, generation = 0; bool available = false; };
	std::array<Slot, MaxSlabs> m_slots {};
	uint64_t m_bytes = 0;
};

} // namespace Lumen::Libs::Graphics

#endif
