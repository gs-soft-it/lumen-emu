#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace Libs::Graphics::MemoryBudget {

inline constexpr uint64_t GiB = 1024ull * 1024 * 1024;

// Return a total allocation ceiling, comparable with heap usage. Subtracting
// current usage here would instead produce free space and count usage twice.
[[nodiscard]] constexpr uint64_t RendererBudget(bool discrete, uint64_t heap_capacity,
                                               uint64_t reported_budget) {
	const auto budget = std::min(heap_capacity, reported_budget);
	// Unified heaps also hold guest/CPU memory and the rest of the operating system.
	// Never restore the full local heap size after reserving that shared capacity.
	const auto reserve = discrete ? std::min(budget / 8, GiB)
	                              : std::min(budget / 2, 8 * GiB);
	return budget - reserve;
}

[[nodiscard]] constexpr uint64_t Subtract(uint64_t value, uint64_t amount) {
	return value > amount ? value - amount : 0;
}

struct GcThresholds {
	uint64_t trigger;
	uint64_t pressure;
	uint64_t critical;
};

[[nodiscard]] constexpr GcThresholds BufferThresholds(uint64_t budget) {
	const auto threshold = std::min(budget, 8 * GiB);
	const auto expected = std::min(Subtract(budget, 6 * threshold / 10), Subtract(budget, GiB));
	const auto critical = std::min(Subtract(budget, 2 * threshold / 10), Subtract(budget, GiB / 2));
	const auto trigger = std::min(budget, std::max(expected, GiB));
	return {trigger, trigger, std::min(budget, std::max(critical, 2 * GiB))};
}

[[nodiscard]] constexpr GcThresholds TextureThresholds(uint64_t budget) {
	const auto threshold = std::min(budget, 8 * GiB);
	const auto pressure = std::min(Subtract(budget, 6 * threshold / 10), Subtract(budget, GiB));
	const auto critical = std::min(Subtract(budget, 2 * threshold / 10), Subtract(budget, GiB / 2));
	return {(budget - threshold) / 2,
	        std::min(budget, std::max(pressure, GiB + GiB / 2)),
	        std::min(budget, std::max(critical, 3 * GiB))};
}

// The LRU includes entries at the cutoff. Clamping age to tick would produce
// cutoff zero during startup and immediately collect newly inserted tick-zero
// entries, before their minimum retention interval has elapsed.
[[nodiscard]] constexpr std::optional<uint64_t> CollectionCutoff(uint64_t tick,
                                                               uint64_t minimum_age) {
	if (tick < minimum_age) return std::nullopt;
	return tick - minimum_age;
}

} // namespace Libs::Graphics::MemoryBudget
