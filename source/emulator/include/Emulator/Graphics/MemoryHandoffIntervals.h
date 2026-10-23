#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Lumen::Libs::Graphics {

// Half-open byte offsets in one guest plane. Do not round these to pages:
// publishing a page can overwrite CPU-owned bytes beside a GPU dependency.
struct MemoryHandoffInterval
{
	uint64_t begin = 0;
	uint64_t end = 0;
};

class MemoryHandoffIntervals
{
public:
	void Mark(uint64_t begin, uint64_t bytes)
	{
		if (bytes == 0 || begin > UINT64_MAX - bytes) return;
		MemoryHandoffInterval incoming {begin, begin + bytes};
		auto first = std::lower_bound(m_ranges.begin(), m_ranges.end(), incoming.begin,
		    [](const auto& range, uint64_t offset) { return range.end < offset; });
		auto last = first;
		while (last != m_ranges.end() && last->begin <= incoming.end)
		{
			incoming.begin = std::min(incoming.begin, last->begin);
			incoming.end = std::max(incoming.end, last->end);
			++last;
		}
		m_ranges.insert(m_ranges.erase(first, last), incoming);
	}

	void Subtract(uint64_t begin, uint64_t bytes)
	{
		if (bytes == 0 || begin > UINT64_MAX - bytes) return;
		const uint64_t end = begin + bytes;
		std::vector<MemoryHandoffInterval> retained;
		retained.reserve(m_ranges.size() + 1);
		for (const auto& range: m_ranges)
		{
			if (range.end <= begin || range.begin >= end) retained.push_back(range);
			else
			{
				if (range.begin < begin) retained.push_back({range.begin, begin});
				if (range.end > end) retained.push_back({end, range.end});
			}
		}
		m_ranges.swap(retained);
	}

	std::vector<MemoryHandoffInterval> Intersections(uint64_t begin, uint64_t bytes) const
	{
		std::vector<MemoryHandoffInterval> result;
		if (bytes == 0 || begin > UINT64_MAX - bytes) return result;
		const uint64_t end = begin + bytes;
		for (const auto& range: m_ranges)
		{
			if (range.begin >= end) break;
			const auto first = std::max(begin, range.begin), last = std::min(end, range.end);
			if (first < last) result.push_back({first, last});
		}
		return result;
	}

	bool Empty() const { return m_ranges.empty(); }
	void Clear() { m_ranges.clear(); }
	const auto& Ranges() const { return m_ranges; }

private:
	std::vector<MemoryHandoffInterval> m_ranges;
};

} // namespace Lumen::Libs::Graphics
