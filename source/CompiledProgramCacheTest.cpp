#include "Emulator/Graphics/CompiledProgramCache.h"

#include <condition_variable>
#include <cstdio>
#include <thread>

namespace Lumen::Libs::Graphics {

bool CompiledProgramCacheContractTest()
{
	using Cache = CompiledProgramCache;
	unsigned calls = 0;
	auto compile = [&] {
		++calls;
		return Cache::Result {true, {0x07230203u, 17u, 23u}, {}};
	};
	Cache collision_cache(Cache::DEFAULT_MAX_BYTES, [](const Cache::Key&) { return size_t {0}; });
	const Cache::Key base {std::string("a\0bc", 4), 0, false, false};
	const std::array<Cache::Key, 5> keys {{base, {std::string("a\0bd", 4), 0, false, false},
	                                    {base.source, 1, false, false}, {base.source, 0, true, false},
	                                    {base.source, 0, false, true}}};
	bool okay = true;
	for (const auto& key: keys) okay = collision_cache.Compile(key, compile).success && okay;
	for (const auto& key: keys) okay = collision_cache.Compile(key, compile).words == std::vector<uint32_t>({0x07230203u, 17u, 23u}) && okay;
	okay = calls == keys.size() && collision_cache.EntryCount() == keys.size() && okay;
	Cache::Result found;
	okay = collision_cache.TryGet(base, &found) && found.words == std::vector<uint32_t>({0x07230203u, 17u, 23u}) && okay;
	const Cache::Key absent {"missing", 0, false, false};
	okay = !collision_cache.TryGet(absent, &found) && found.success && !collision_cache.TryGet(base, nullptr) && okay;
	// Returning a mutable value must not expose or corrupt the retained entry.
	auto changed = collision_cache.Compile(base, compile);
	changed.words[0] = 0;
	okay = collision_cache.Compile(base, compile).words[0] == 0x07230203u && okay;

	Cache failures;
	unsigned failed_calls = 0;
	for (unsigned i = 0; i < 2; ++i)
	{
		const auto result = failures.Compile(base, [&] { ++failed_calls; return Cache::Result {false, {}, "diagnostic"}; });
		okay = !result.success && result.error == "diagnostic" && okay;
	}
	okay = failed_calls == 2 && failures.EntryCount() == 0 && okay;
	for (unsigned i = 0; i < 2; ++i)
		failures.Compile(base, [&] { ++failed_calls; return Cache::Result {true, {}, {}}; });
	okay = failed_calls == 4 && failures.EntryCount() == 0 && okay;

	Cache too_small(1);
	too_small.Compile(base, compile);
	too_small.Compile(base, compile);
	okay = too_small.EntryCount() == 0 && too_small.RetainedBytes() == 0 && okay;
	Cache cost_probe;
	cost_probe.Compile(base, compile);
	const size_t entry_cost = cost_probe.RetainedBytes();
	Cache bounded(entry_cost * 2);
	unsigned bounded_calls = 0;
	auto bounded_compile = [&] { ++bounded_calls; return Cache::Result {true, {0x07230203u, 17u, 23u}, {}}; };
	bounded.Compile(keys[0], bounded_compile);
	bounded.Compile(keys[1], bounded_compile);
	okay = bounded.TryGet(keys[0], &found) && okay; // Refresh A; B is now the oldest.
	bounded.Compile(keys[2], bounded_compile);
	bounded.Compile(keys[0], bounded_compile);
	okay = bounded_calls == 3 && bounded.EntryCount() == 2 && bounded.RetainedBytes() <= entry_cost * 2 && okay;
	bounded.Compile(keys[1], bounded_compile);
	okay = bounded_calls == 4 && bounded.EntryCount() == 2 && bounded.RetainedBytes() <= entry_cost * 2 && okay;

	Cache reset_cache;
	std::mutex rendezvous;
	std::condition_variable changed_state;
	bool entered = false;
	bool resume = false;
	Cache::Result in_flight;
	std::thread worker([&] {
		in_flight = reset_cache.Compile(base, [&] {
			std::unique_lock<std::mutex> lock(rendezvous);
			entered = true;
			changed_state.notify_all();
			changed_state.wait(lock, [&] { return resume; });
			return Cache::Result {true, {0x07230203u, 99u}, {}};
		});
	});
	{
		std::unique_lock<std::mutex> lock(rendezvous);
		changed_state.wait(lock, [&] { return entered; });
	}
	// Also proves Reset can obtain the mutex while compilation is blocked.
	reset_cache.Reset();
	{
		std::lock_guard<std::mutex> lock(rendezvous);
		resume = true;
	}
	changed_state.notify_all();
	worker.join();
	okay = in_flight.success && in_flight.words.back() == 99 && reset_cache.EntryCount() == 0 && okay;
	unsigned after_reset = 0;
	reset_cache.Compile(base, [&] { ++after_reset; return Cache::Result {true, {0x07230203u, 111u}, {}}; });
	okay = after_reset == 1 && reset_cache.EntryCount() == 1 && okay;
	reset_cache.Reset();
	okay = reset_cache.EntryCount() == 0 && reset_cache.RetainedBytes() == 0 && okay;
	std::fprintf(stderr, "[COMPILED_PROGRAM_CACHE_TEST] result=%s collision=exact options=exact failures=uncached reset=reject-stale\n",
	             okay ? "PASS" : "FAIL");
	return okay;
}

} // namespace Lumen::Libs::Graphics
