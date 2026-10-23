#ifndef EMULATOR_LOADER_PRELINKED_PLT_H_
#define EMULATOR_LOADER_PRELINKED_PLT_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace Lumen::Loader::PrelinkedPlt {

// Experimental recovery of a complete, dense import table, not a decoder or
// a general text-relocation rule. Callers must validate every declared GOT
// slot and symbol before providing them. All offsets are image-relative.
struct Repair
{
	uint64_t offset;
	int32_t displacement;
	uint64_t expected;
};

struct Plan
{
	bool matched = false;
	bool ambiguous = false;
	std::vector<Repair> repairs;
};

inline bool RelativeTarget(uint64_t next, int32_t displacement, uint64_t* target)
{
	if (displacement < 0)
	{
		const uint64_t amount = static_cast<uint64_t>(-static_cast<int64_t>(displacement));
		if (next < amount) return false;
		*target = next - amount;
	} else
	{
		if (next > UINT64_MAX - static_cast<uint32_t>(displacement)) return false;
		*target = next + static_cast<uint32_t>(displacement);
	}
	return true;
}

inline bool Displacement(uint64_t next, uint64_t target, int32_t* result)
{
	if (target >= next)
	{
		if (target - next > INT32_MAX) return false;
		*result = static_cast<int32_t>(target - next);
	} else
	{
		if (next - target > uint64_t {INT32_MAX} + 1) return false;
		*result = static_cast<int32_t>(-static_cast<int64_t>(next - target));
	}
	return true;
}

inline Plan Find(const uint8_t* bytes, size_t size, uint64_t segment_offset,
	             uint64_t image_size, const std::vector<uint64_t>& slots)
{
	Plan result;
	constexpr size_t stride = 16;
	if (bytes == nullptr || slots.size() < 5 || slots.size() > 65536 ||
	    size < slots.size() * stride || segment_offset > image_size ||
	    size > image_size - segment_offset || image_size < 8) return result;
	for (size_t i = 0; i < slots.size(); ++i)
	{
		if (slots[i] > image_size - 8 || (slots[i] & 7) != 0 ||
		    (i != 0 && (slots[i - 1] > UINT64_MAX - 8 || slots[i] != slots[i - 1] + 8))) return result;
	}
	const size_t table_size = slots.size() * stride;
	bool found = false;
	// The first two and last two entries must remain indirect anchors. This avoids
	// extrapolating an inferred table through an unanchored edge.
	for (size_t start = 0; start <= size - table_size; ++start)
	{
		if (bytes[start] != 0xff || bytes[start + 1] != 0x25) continue;
		int32_t first_disp = 0;
		std::memcpy(&first_disp, bytes + start + 2, sizeof(first_disp));
		uint64_t first_target = 0;
		if (!RelativeTarget(segment_offset + start + 6, first_disp, &first_target) ||
		    first_target != slots.front()) continue;
		std::vector<Repair> candidate;
		bool valid = true;
		for (size_t i = 0; i < slots.size() && valid; ++i)
		{
			const auto* entry = bytes + start + i * stride;
			const uint64_t offset = segment_offset + start + i * stride;
			for (size_t p = 6; p < stride; ++p)
			{
				if (entry[p] != 0xcc) valid = false;
			}
			if (!valid) break;
			if (entry[0] == 0xff && entry[1] == 0x25)
			{
				int32_t disp = 0;
				uint64_t target = 0;
				std::memcpy(&disp, entry + 2, sizeof(disp));
				valid = RelativeTarget(offset + 6, disp, &target) && target == slots[i];
			} else if (entry[0] == 0xe9 && entry[5] == 0x90 && i >= 2 && i + 2 < slots.size())
			{
				int32_t old_disp = 0;
				uint64_t old_target = 0;
				std::memcpy(&old_disp, entry + 1, sizeof(old_disp));
				// Do not replace local tail jumps. A direct external binding may
				// still be intentional, hence the explicit opt-in at the caller.
				if (RelativeTarget(offset + 5, old_disp, &old_target) && old_target < image_size)
				{
					valid = false;
					break;
				}
				int32_t disp = 0;
				valid = Displacement(offset + 6, slots[i], &disp);
				uint64_t expected = 0;
				std::memcpy(&expected, entry, sizeof(expected));
				if (valid) candidate.push_back({offset, disp, expected});
			} else
			{
				valid = false;
			}
		}
		if (!valid) continue;
		if (found)
		{
			result.ambiguous = true;
			result.repairs.clear();
			return result;
		}
		found = true;
		result.matched = true;
		result.repairs = std::move(candidate);
	}
	return result;
}

} // namespace Lumen::Loader::PrelinkedPlt
#endif
