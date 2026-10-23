#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HTILECPUOBSERVATION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HTILECPUOBSERVATION_H_

#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>

namespace Lumen::Libs::Graphics {

inline constexpr uint32_t HTILE_CPU_OBSERVATION_MAX_BYTES = 1024u * 1024u;
inline constexpr uint32_t CPU_OBSERVATION_HARD_MAX_BYTES = 16u * 1024u * 1024u;
inline constexpr uint32_t HTILE_CPU_OBSERVATION_MAX_SNAPSHOTS = 8;
inline constexpr uint32_t HTILE_CPU_OBSERVATION_MAX_EVENTS = 4;
inline constexpr uint32_t HTILE_CPU_OBSERVATION_MAX_BINS = 16;

[[nodiscard]] inline bool HtileCpuObservationSelectorEnabled(const char* value)
{
	return value != nullptr && value[0] == '1' && value[1] == '\0';
}

[[nodiscard]] inline bool HtileCpuObservationEnabled()
{
	static const bool enabled = HtileCpuObservationSelectorEnabled(std::getenv("LUMEN_GFX_HTILE_CPU_OBSERVATION"));
	return enabled;
}

struct HtileCpuWordBin
{
	uint32_t value = 0;
	uint32_t count = 0;
};

struct HtileCpuSummary
{
	uint32_t byte_count = 0;
	uint32_t word_count = 0;
	uint32_t bin_count = 0;
	uint32_t other_words = 0;
	uint64_t fnv1a64 = 0;
	uint8_t first_byte = 0;
	bool byte_uniform = false;
	std::array<HtileCpuWordBin, HTILE_CPU_OBSERVATION_MAX_BINS> bins {};

	[[nodiscard]] bool IsUniform() const { return word_count != 0 && bin_count == 1 && other_words == 0; }
};

// Consume only a caller-owned, readable host buffer. The bins describe the
// first sixteen distinct little-endian words, not HTile encoding semantics or
// the most frequent values. Every word belongs to one bin or other_words.
// The observation is not an atomic snapshot of concurrent CPU/GPU writes.
[[nodiscard]] inline bool SummarizeHtileCpuBytes(const void* data, uint64_t size, HtileCpuSummary* output,
	                                            uint64_t byte_limit = HTILE_CPU_OBSERVATION_MAX_BYTES)
{
	if (data == nullptr || output == nullptr || byte_limit == 0 || byte_limit > CPU_OBSERVATION_HARD_MAX_BYTES ||
	    size == 0 || size > byte_limit || size % 4u != 0)
	{
		return false;
	}
	HtileCpuSummary summary {};
	summary.byte_count = static_cast<uint32_t>(size);
	summary.word_count = static_cast<uint32_t>(size / 4u);
	summary.fnv1a64 = 14695981039346656037ull;
	summary.byte_uniform = true;
	const auto* source = static_cast<const uint8_t*>(data);
	for (uint32_t offset = 0; offset < summary.byte_count; offset += 4u)
	{
		uint32_t word = 0;
		for (uint32_t byte = 0; byte < 4; ++byte)
		{
			const uint8_t value = source[offset + byte];
			if (offset == 0 && byte == 0) summary.first_byte = value;
			if (value != summary.first_byte) summary.byte_uniform = false;
			word |= static_cast<uint32_t>(value) << (byte * 8u);
			summary.fnv1a64 = (summary.fnv1a64 ^ value) * 1099511628211ull;
		}
		uint32_t bin = 0;
		while (bin < summary.bin_count && summary.bins[bin].value != word) ++bin;
		if (bin < summary.bin_count)
		{
			++summary.bins[bin].count;
		} else if (summary.bin_count < summary.bins.size())
		{
			summary.bins[summary.bin_count++] = {word, 1};
		} else
		{
			++summary.other_words;
		}
	}
	*output = summary;
	return true;
}

struct HtileStorageWritebackSnapshot
{
	uint64_t sequence = 0;
	uint64_t address = 0;
	uint64_t size = 0;
	uint64_t source_memory_id = 0;
	HtileCpuSummary summary;
};

// Diagnostic history only: exact address/size association does not prove the
// current guest allocation, its lifetime, or the completion of its GPU writer.
// No guest pointer is retained. The mutex never spans summary reads, logging,
// VM operations, GPU operations, or another subsystem's lock acquisition.
class HtileStorageWritebackHistory
{
public:
	[[nodiscard]] bool Record(uint64_t address, uint64_t size, uint64_t source_memory_id, const void* host_data)
	{
		if (address == 0 || size == 0 || size > HTILE_CPU_OBSERVATION_MAX_BYTES || size % 4u != 0 ||
		    address > (std::numeric_limits<uint64_t>::max)() - size || source_memory_id == 0 || host_data == nullptr)
		{
			return false;
		}
		uint32_t slot = 0;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_reserved == m_snapshots.size()) return false;
			slot = m_reserved++;
		}
		HtileStorageWritebackSnapshot snapshot {};
		snapshot.sequence = slot + 1u;
		snapshot.address = address;
		snapshot.size = size;
		snapshot.source_memory_id = source_memory_id;
		if (!SummarizeHtileCpuBytes(host_data, size, &snapshot.summary)) return false;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_snapshots[slot] = snapshot;
		}
		return true;
	}

	[[nodiscard]] bool Find(uint64_t address, uint64_t size, HtileStorageWritebackSnapshot* output)
	{
		if (output == nullptr || address == 0 || size == 0) return false;
		std::lock_guard<std::mutex> lock(m_mutex);
		const HtileStorageWritebackSnapshot* latest = nullptr;
		for (const auto& snapshot: m_snapshots)
		{
			if (snapshot.sequence != 0 && snapshot.address == address && snapshot.size == size &&
			    (latest == nullptr || snapshot.sequence > latest->sequence)) latest = &snapshot;
		}
		if (latest == nullptr) return false;
		*output = *latest;
		return true;
	}

private:
	std::mutex m_mutex;
	uint32_t m_reserved = 0;
	std::array<HtileStorageWritebackSnapshot, HTILE_CPU_OBSERVATION_MAX_SNAPSHOTS> m_snapshots {};
};

inline HtileStorageWritebackHistory& GetHtileStorageWritebackHistory()
{
	static HtileStorageWritebackHistory history;
	return history;
}

} // namespace Lumen::Libs::Graphics

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HTILECPUOBSERVATION_H_
