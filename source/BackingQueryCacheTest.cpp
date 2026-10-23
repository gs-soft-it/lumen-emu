#include "Emulator/Graphics/BackingQueryCache.h"

#include <cstdio>

namespace Lumen::Libs::Graphics {

bool BackingQueryCacheContractTest()
{
	using Cache = BackingQueryCache;
	using Span = Cache::Span;
	bool okay = true;
	auto check = [&okay](bool condition, const char* reason) {
		if (!condition) std::fprintf(stderr, "[BACKING_QUERY_CACHE_TEST] failure=%s\n", reason);
		okay = condition && okay;
	};
	Cache cache;
	Cache::Context context {11, 1};
	uint64_t identity = 7, calls = 0;
	bool available = true;
	auto current = [&] { return context; };
	auto backing = [&](uint64_t address, uint64_t maximum_bytes, Span* result) {
		++calls;
		if (result != nullptr) *result = {};
		if (!available || result == nullptr || maximum_bytes == 0 || address > UINT64_MAX - maximum_bytes) return false;
		const uint64_t base = address >= 0x20000 ? 0x20000 : 0x10000;
		if (address < base || address - base >= 0x4000) return false;
		const uint64_t delta = address - base;
		*result = {identity, 0x1000 + delta, address, std::min(maximum_bytes, 0x4000 - delta), context.generation};
		return true;
	};
	auto query = [&](uint64_t address, uint64_t bytes, Span* span) {
		return cache.Resolve(address, bytes, span, current, backing);
	};
	Span span;
	check(query(0x10000, 64, &span) && span.identity == 7 && span.offset == 0x1000 && calls == 1, "initial-query");
	span.identity = 999;
	check(query(0x10000, 64, &span) && span.identity == 7 && calls == 1, "owned-result-copy");
	check(query(0x10010, 16, &span) && span.offset == 0x1010 && span.address == 0x10010 && calls == 1, "contained-subrange");
	check(query(0x10000, 128, &span) && span.size == 128 && calls == 2, "grow-clipped-request");
	check(query(0x10000, 128, &span) && calls == 2, "grown-request-reuse");
	check(query(0x13ff0, 64, &span) && span.size == 16 && calls == 3, "partial-backing-tail");
	check(query(0x13ff0, 128, &span) && span.size == 16 && calls == 3, "known-partial-tail-reuse");
	check(query(0x20000, 64, &span) && span.offset == 0x1000 && span.identity == 7 && calls == 4, "alias-address-distinct");
	check(query(0x20010, 16, &span) && span.offset == 0x1010 && calls == 4, "alias-offset-preserved");
	context.token = 12;
	check(query(0x20000, 64, &span) && calls == 5, "token-change-invalidates");
	context.generation = 2;
	identity = 8;
	check(query(0x20000, 64, &span) && span.identity == 8 && calls == 6, "mapping-replacement-invalidates");
	context.generation = 3;
	available = false;
	check(!query(0x20000, 64, &span) && calls == 7 && span.identity == 0, "retired-mapping-rejects");
	check(!query(0x20000, 64, &span) && calls == 8, "negative-result-not-retained");
	available = true;
	context.token = 0;
	check(query(0x20000, 64, &span) && query(0x20000, 64, &span) && calls == 10, "no-owner-always-queries");
	context = {12, 3};
	check(query(0x20000, 64, &span) && calls == 11, "release-and-readmission-invalidates");
	check(!query(0x20000, 64, nullptr) && calls == 12, "null-output-delegated");
	check(!query(UINT64_MAX - 1, 64, &span) && calls == 13, "overflow-delegated");
	check(cache.Stats().bypasses == 4 && cache.Stats().hits >= 5, "instrumentation");

	// No result produced while ownership changes may be published into the
	// new context, even if the callback itself returned successful metadata.
	Cache changed;
	unsigned changed_calls = 0;
	context = {31, 5};
	auto releasing_query = [&](uint64_t address, uint64_t bytes, Span* result) {
		++changed_calls;
		*result = {19, 0, address, bytes, context.generation};
		context.token = 32;
		return true;
	};
	check(changed.Resolve(0x30000, 64, &span, current, releasing_query) && changed_calls == 1, "release-during-query");
	check(changed.Resolve(0x30000, 64, &span, current, releasing_query) && changed_calls == 2, "reject-stale-publication");
	check(changed.Resolve(0x30000, 64, &span, current, releasing_query) && changed_calls == 2, "new-context-reuse");

	// A malformed successful provider result must never create a reusable
	// entry. Preserve the original return contract and query again next time.
	Cache malformed;
	unsigned malformed_calls = 0;
	auto invalid_query = [&](uint64_t address, uint64_t bytes, Span* result) {
		++malformed_calls;
		*result = {19, UINT64_MAX, address, bytes, context.generation};
		return true;
	};
	malformed.Resolve(0x30000, 64, &span, current, invalid_query);
	malformed.Resolve(0x30000, 64, &span, current, invalid_query);
	check(malformed_calls == 2 && malformed.Stats().hits == 0, "malformed-result-not-retained");
	auto wrong_generation = [&](uint64_t address, uint64_t bytes, Span* result) {
		++malformed_calls;
		*result = {19, 0, address, bytes, context.generation - 1};
		return true;
	};
	malformed.Resolve(0x30000, 64, &span, current, wrong_generation);
	malformed.Resolve(0x30000, 64, &span, current, wrong_generation);
	check(malformed_calls == 4 && malformed.Stats().hits == 0, "inconsistent-generation-not-retained");
	context.generation = UINT64_MAX;
	const auto before_saturation = calls;
	check(query(0x20000, 64, &span) && query(0x20000, 64, &span) && calls == before_saturation + 2,
	      "saturated-generation-bypasses");
	context.generation = 7;

	// More distinct positive intervals than capacity forces collisions and
	// replacement without relying on the implementation's hash function.
	Cache bounded;
	uint64_t bounded_calls = 0;
	auto distinct_query = [&](uint64_t address, uint64_t bytes, Span* result) {
		++bounded_calls;
		*result = {address + 1, address * 2, address, bytes, context.generation};
		return true;
	};
	for (uint64_t index = 1; index <= Cache::CAPACITY * 4; ++index)
	{
		const uint64_t address = index * 4096;
		check(bounded.Resolve(address, 1, &span, current, distinct_query) &&
		      span.identity == address + 1 && span.offset == address * 2, "bounded-insert-exact");
	}
	const auto before_revisit = bounded_calls;
	for (uint64_t index = 1; index <= Cache::CAPACITY * 4; ++index)
	{
		const uint64_t address = index * 4096;
		check(bounded.Resolve(address, 1, &span, current, distinct_query) &&
		      span.identity == address + 1 && span.offset == address * 2, "collision-never-aliases");
	}
	check(bounded.Stats().evictions != 0 && bounded_calls > before_revisit, "bounded-eviction");
	std::fprintf(stderr, "[BACKING_QUERY_CACHE_TEST] result=%s entries=%zu token=exact generation=exact aliases=offset failures=uncached publication=revalidated\n",
	             okay ? "PASS" : "FAIL", Cache::CAPACITY);
	return okay;
}

} // namespace Lumen::Libs::Graphics
