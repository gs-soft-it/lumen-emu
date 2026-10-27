#include "../src/common/memoryBudget.h"
#include "../src/common/lruCache.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
using namespace Libs::Graphics::MemoryBudget;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "MemoryBudgetTests: %s\n", message);
		std::abort();
	}
}

void CheckThresholds(uint64_t budget, GcThresholds thresholds) {
	Check(thresholds.trigger <= thresholds.pressure, "trigger exceeds pressure threshold");
	Check(thresholds.pressure <= thresholds.critical, "pressure exceeds critical threshold");
	Check(thresholds.critical <= budget, "collection thresholds exceed available budget");
}
} // namespace

int main() {
	// A 16 GiB unified heap with a driver budget of 14 GiB must not restore the
	// physical heap capacity after reserving memory for the guest and host.
	Check(RendererBudget(false, 16 * GiB, 14 * GiB) == 7 * GiB,
	      "unified heap capacity cancels shared-memory reserve");
	Check(RendererBudget(false, 4 * GiB, 4 * GiB) == 2 * GiB,
	      "small unified heap cannot satisfy a fixed eight-GiB reserve");
	Check(RendererBudget(false, 32 * GiB, 24 * GiB) == 16 * GiB,
	      "large unified heap reserve is not capped at eight GiB");
	Check(RendererBudget(false, 8 * GiB, 16 * GiB) == 4 * GiB,
	      "reported budget exceeds physical heap capacity");
	Check(RendererBudget(true, 16 * GiB, 14 * GiB) == 13 * GiB,
	      "discrete-GPU reserve changed");
	Check(RendererBudget(true, 4 * GiB, 4 * GiB) == 7 * GiB / 2,
	      "small discrete-GPU reserve changed");
	Check(RendererBudget(false, 16 * GiB, 0) == 0,
	      "exhausted driver budget falls back to the whole heap");

	// Check tiny/exhausted budgets and arithmetic boundaries as well as normal
	// hardware sizes. Fixed GC floors must not postpone collection beyond a cap.
	constexpr std::array<uint64_t, 11> boundaries {
	    0, 1, GiB / 2, GiB, GiB + GiB / 2, 2 * GiB, 3 * GiB,
	    8 * GiB, 16 * GiB, 32 * GiB, std::numeric_limits<uint64_t>::max()};
	for (const auto budget: boundaries) {
		CheckThresholds(budget, BufferThresholds(budget));
		CheckThresholds(budget, TextureThresholds(budget));
	}
	GcThresholds previous_buffer {}, previous_texture {};
	for (uint64_t budget = 0; budget <= 64 * GiB; budget += GiB / 16) {
		const auto buffer = BufferThresholds(budget);
		const auto texture = TextureThresholds(budget);
		CheckThresholds(budget, buffer);
		CheckThresholds(budget, texture);
		Check(buffer.trigger >= previous_buffer.trigger && buffer.critical >= previous_buffer.critical,
		      "buffer collection becomes later when a budget shrinks");
		Check(texture.trigger >= previous_texture.trigger && texture.pressure >= previous_texture.pressure &&
		          texture.critical >= previous_texture.critical,
		      "texture collection becomes later when a budget shrinks");
		previous_buffer = buffer;
		previous_texture = texture;
	}

	const auto initial = RendererBudget(false, 16 * GiB, 16 * GiB);
	const auto reduced = RendererBudget(false, 16 * GiB, 8 * GiB);
	constexpr auto usage = 4 * GiB;
	Check(usage < BufferThresholds(initial).critical && usage >= BufferThresholds(reduced).critical,
	      "a lower live budget does not activate critical buffer collection");
	Check(usage < TextureThresholds(initial).critical && usage >= TextureThresholds(reduced).critical,
	      "a lower live budget does not activate critical texture collection");
	// The LRU visits entries whose last-use tick is <= cutoff. Tick-zero images
	// must survive the complete retention interval, even when collection is
	// enabled from startup by a small memory budget.
	for (const uint64_t age: {16u, 80u, 160u}) {
		Common::LeastRecentlyUsedCache<unsigned, uint64_t> lru;
		(void)lru.Insert(1u, 0);
		const auto recent = lru.Insert(2u, 0);
		const auto eligible = [&](uint64_t tick) {
			unsigned mask = 0;
			if (const auto cutoff = CollectionCutoff(tick, age)) {
				lru.ForEachItemBelow(*cutoff, [&](unsigned item) { mask |= item; });
			}
			return mask;
		};
		Check(eligible(0) == 0,
		      "newly used tick-zero images become eligible before minimum age");
		lru.Touch(recent, 2);
		Check(eligible(age - 1) == 0,
		      "startup collection begins before the full retention interval");
		Check(eligible(age) == 1,
		      "tick-zero images are not eligible after the full retention interval");
		Check(eligible(age + 1) == 1 && eligible(age + 2) == 3,
		      "touching an image does not retain it for its full minimum age");
	}
	Check(CollectionCutoff(std::numeric_limits<uint64_t>::max(), 160) ==
	          std::numeric_limits<uint64_t>::max() - 160,
	      "collection cutoff overflows for a long-running session");
	std::puts("MemoryBudgetTests PASS");
	return 0;
}
