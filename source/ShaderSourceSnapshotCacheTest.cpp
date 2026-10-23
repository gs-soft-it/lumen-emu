#include "Emulator/Graphics/ShaderSourceSnapshotCache.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <type_traits>
#include <vector>

using namespace Lumen::Libs::Graphics;

static_assert(std::is_nothrow_copy_constructible_v<ShaderSourceWords>);
static_assert(std::is_nothrow_copy_assignable_v<ShaderSourceWords>);
static_assert(std::is_nothrow_move_constructible_v<ShaderSourceWords>);
static_assert(std::is_nothrow_move_assignable_v<ShaderSourceWords>);

static void Require(bool value, const char* label)
{
	if (!value) { std::fprintf(stderr, "FAIL: %s\n", label); std::abort(); }
}

static void Contracts()
{
	ShaderSourceSnapshotCache cache;
	std::vector<uint32_t> input(1024, 0x10203040u);
	const uint64_t address = UINT64_C(0x100000000);
	const auto original = cache.Capture(address, input.data(), input.size() * sizeof(uint32_t), 1);
	Require(bool(original), "initial capture");
	ShaderSourceWords assigned;
	assigned = original;
	Require(assigned.SameStorage(original), "copy assignment retains immutable ownership");
	ShaderSourceWords moved;
	moved = std::move(assigned);
	Require(moved.SameStorage(original) && !assigned, "move assignment transfers immutable ownership");
	Require(original.SameStorage(cache.Capture(address, input.data(), original.Bytes(), 1)), "exact current bytes reuse");
	for (const size_t index: {size_t {0}, size_t {513}, input.size() - 1})
	{
		input[index] ^= 1;
		const auto changed = cache.Capture(address, input.data(), original.Bytes(), 1);
		Require(bool(changed) && !changed.SameStorage(original) && changed.Data()[index] == input[index], "full extent mutation");
		Require(original.Data()[index] != input[index], "retained capture immutable");
		input[index] ^= 1;
	}
	const auto before_reset = cache.Capture(address, input.data(), original.Bytes(), 1);
	const auto after_reset = cache.Capture(address, input.data(), original.Bytes(), 2);
	Require(!after_reset.SameStorage(before_reset), "session generation removes retained candidate");
	const auto shorter = cache.Capture(address, input.data(), original.Bytes() / 2, 2);
	Require(shorter.Bytes() == original.Bytes() / 2 && !shorter.SameStorage(original), "new extent at same address");
	Require(!cache.Capture(0, input.data(), original.Bytes(), 2) &&
	    !cache.Capture(address, nullptr, original.Bytes(), 2) &&
	    !cache.Capture(address, input.data(), 3, 2), "invalid source extent");
	// More live immutable captures than cache slots remain valid after eviction.
	std::vector<ShaderSourceWords> retained;
	for (uint64_t index = 0; index != 128; ++index)
	{
		input[513] = static_cast<uint32_t>(index);
		retained.push_back(cache.Capture(address + index * 65536, input.data(), original.Bytes(), 2));
	}
	for (size_t index = 0; index != retained.size(); ++index)
		Require(retained[index].Data()[513] == index, "evicted source cannot be recycled while retained");
	std::vector<uint32_t> large(384 * 1024 / sizeof(uint32_t), 0x87654321u);
	for (uint64_t index = 0; index != 64; ++index)
	{
		const auto captured = cache.Capture(address + index * 1048576, large.data(), large.size() * sizeof(uint32_t), 2);
		Require(bool(captured) && cache.RetainedBytes() <= ShaderSourceSnapshotCache::BYTE_CAP, "global payload budget");
	}
	large.resize(ShaderSourceSnapshotCache::BYTE_CAP / sizeof(uint32_t) + 1);
	const auto uncached = cache.Capture(address, large.data(), large.size() * sizeof(uint32_t), 2);
	const auto uncached_again = cache.Capture(address, large.data(), large.size() * sizeof(uint32_t), 2);
	Require(bool(uncached) && bool(uncached_again) && !uncached.SameStorage(uncached_again) &&
	    cache.RetainedBytes() <= ShaderSourceSnapshotCache::BYTE_CAP, "oversize accepted without retention");
	// Sharing is allowed across threads; the cache itself remains worker-local.
	std::array<std::thread, 4> readers;
	for (auto& reader: readers) reader = std::thread([original] {
		for (size_t index = 0; index != 10000; ++index)
		{
			const ShaderSourceWords held = original;
			Require(held.SameStorage(original) && held.Data()[513] == 0x10203040u, "cross-thread immutable ownership");
		}
	});
	for (auto& reader: readers) reader.join();
	cache.Clear();
	Require(cache.RetainedBytes() == 0 && original.Data()[513] == 0x10203040u, "cache reset retains live readers");
	std::printf("PASS full-extent mutations, lifetime/eviction, generation, budget, cross-thread ownership\n");
}

static void PrefixContracts()
{
	ShaderSourceSnapshotCache cache;
	std::vector<uint32_t> current(1024, 0x10203040u);
	const uint32_t prefix[2] {0xbeeb03ffu, 507u};
	current[0] = prefix[0];
	current[1] = prefix[1];
	constexpr uint64_t address = UINT64_C(0x100000000);
	const size_t bytes = current.size() * sizeof(uint32_t);
	const auto capture = [&] { return cache.CaptureWithPrefix(address, current.data(), bytes, prefix, 1); };
	const auto original = capture();
	Require(bool(original) && capture().SameStorage(original), "complete private copy shares owned snapshot");
	for (size_t changed: {size_t {0}, size_t {1}})
	{
		current[changed] ^= 1;
		// Even a new generation must not alter the cache on failed consistency.
		Require(!cache.CaptureWithPrefix(address, current.data(), bytes, prefix, 2), "changed prefix denies capture");
		current[changed] ^= 1;
		Require(capture().SameStorage(original), "prefix denial leaves earlier cache unchanged");
	}
	Require(!cache.CaptureWithPrefix(address, nullptr, bytes, prefix, 1) &&
	    !cache.CaptureWithPrefix(address, current.data(), 4, prefix, 1), "prefix requires full readable header");
	for (size_t changed: {size_t {513}, current.size() - 1})
	{
		current[changed] ^= 1;
		const auto fresh = capture();
		Require(bool(fresh) && !fresh.SameStorage(original) && fresh.Data()[changed] == current[changed] &&
		    original.Data()[changed] != current[changed], "private copy interior/tail invalidates immutable cache");
		current[changed] ^= 1;
	}
	Require(std::memcmp(capture().Data(), original.Data(), bytes) == 0, "reused scratch cannot expose previous copy");
	std::printf("PASS full-prefix consistency before cache, private-copy interior/tail invalidation\n");
}

static volatile uint64_t sink = 0;

static void Benchmark(size_t bytes, size_t mutate_period)
{
	constexpr size_t iterations = 100000;
	std::array<std::vector<uint32_t>, 2> current;
	for (const bool cached: {false, true})
	{
		for (size_t index = 0; index != current.size(); ++index) current[index].assign(bytes / sizeof(uint32_t), 0x12345678u + index);
		ShaderSourceSnapshotCache snapshots;
		std::array<std::vector<uint32_t>, 2> previous = current;
		std::array<ShaderSourceWords, 2> previous_owned;
		size_t shared_hits = 0, changed = 0;
		uint64_t checksum = 0;
		const auto start = std::chrono::steady_clock::now();
		for (size_t iteration = 0; iteration != iterations; ++iteration)
		{
			const size_t index = iteration & 1;
			auto& source = current[index];
			if (mutate_period != 0 && iteration % mutate_period == 0) source[source.size() / 2] ^= 1;
			if (cached)
			{
				auto snapshot = snapshots.Capture(UINT64_C(0x100000000) + index * 65536, source.data(), bytes, 1);
				Require(bool(snapshot), "benchmark allocation");
				if (snapshot.SameStorage(previous_owned[index])) ++shared_hits;
				else { previous_owned[index] = snapshot; ++changed; }
				checksum += snapshot.Data()[source.size() / 2];
			} else
			{
				// Historical owned draw copy followed by the decoder cache's full
				// byte comparison and independently owned miss source.
				auto snapshot = std::unique_ptr<uint32_t[]>(new uint32_t[source.size()]);
				std::memcpy(snapshot.get(), source.data(), bytes);
				if (std::memcmp(previous[index].data(), snapshot.get(), bytes) != 0)
				{ previous[index].assign(snapshot.get(), snapshot.get() + source.size()); ++changed; }
				checksum += snapshot[source.size() / 2];
			}
		}
		const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
		sink = checksum;
		std::printf("bytes=%zu mutate_period=%zu cached=%u ns_per_capture=%.3f shared_hits=%zu changed=%zu checksum=%llu\n",
		    bytes, mutate_period, cached ? 1u : 0u, elapsed * 1000.0 / iterations, shared_hits, changed,
		    static_cast<unsigned long long>(checksum));
	}
}

int main(int argc, char** argv)
{
	Contracts();
	PrefixContracts();
	if (argc == 2 && std::strcmp(argv[1], "--benchmark") == 0)
		for (const size_t bytes: {size_t {4096}, size_t {16384}, size_t {65536}})
			for (const size_t period: {size_t {0}, size_t {257}, size_t {1}}) Benchmark(bytes, period);
	return 0;
}
