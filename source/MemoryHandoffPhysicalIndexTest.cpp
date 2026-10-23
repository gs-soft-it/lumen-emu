// SPDX-License-Identifier: GPL-2.0-only
#include "Emulator/Graphics/MemoryHandoffPhysicalIndex.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

using Lumen::Libs::Graphics::MemoryHandoffPhysicalIndex;
using Span = MemoryHandoffPhysicalIndex::Span;

static void Require(bool value, const char* what)
{
	if (!value) { std::fprintf(stderr, "MemoryHandoffPhysicalIndex FAIL: %s\n", what); std::exit(1); }
}

int main()
{
	MemoryHandoffPhysicalIndex index;
	std::map<uint64_t, std::vector<Span>> oracle;
	uint64_t rng = 0x936529efab987ULL;
	const auto random = [&] { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
	uint64_t queries = 0;
	const auto query = [&](uint64_t identity, uint64_t begin, uint64_t end) {
		std::vector<uint64_t> expected, actual;
		for (const auto& resource: oracle)
			for (const auto& span: resource.second)
				if (span.identity == identity && span.begin < end && span.end > begin) expected.push_back(resource.first);
		Require(index.Visit(identity, begin, end, [&](uint64_t lifetime) { actual.push_back(lifetime); return true; }), "query");
		std::sort(actual.begin(), actual.end());
		Require(expected == actual, "brute-force multiset oracle");
		if (!expected.empty())
		{
			size_t calls = 0;
			Require(!index.Visit(identity, begin, end, [&](uint64_t) { ++calls; return false; }) && calls == 1, "early denial");
		}
		++queries;
	};
	// Multiple identities, nested/enclosing intervals, identical fragments and
	// maximum endpoints; compare actual bytes, not the AVL's internal summaries.
	oracle[1] = {{1, 0, UINT64_MAX}, {1, 12, 16}, {1, 12, 16}, {3, 12, 16}};
	Require(index.Replace(1, oracle[1]), "initial owner");
	query(1, 12, 16); query(1, UINT64_MAX - 1, UINT64_MAX); query(2, 0, UINT64_MAX);
	Require(!index.Replace(1, {{0, 0, 1}}) && !index.Replace(1, {{1, 3, 3}}) &&
	    !index.Replace(0, {{1, 0, 1}}) && !index.Replace(1, {}), "invalid replacement");
	query(1, 12, 16); // Failed preflight leaves the previous owner intact.
	Require(!index.Visit(0, 0, 1, [](uint64_t) { return true; }) &&
	    !index.Visit(1, 1, 1, [](uint64_t) { return true; }), "invalid query");
	for (unsigned mutation = 0; mutation < 20000; ++mutation)
	{
		const uint64_t lifetime = 1 + random() % 512;
		if (random() % 4 == 0) { index.Erase(lifetime); oracle.erase(lifetime); }
		else
		{
			std::vector<Span> spans;
			const unsigned count = 1 + random() % 5;
			for (unsigned fragment = 0; fragment < count; ++fragment)
			{
				const uint64_t start = random() % 1024;
				spans.push_back({1 + random() % 7, start, start + 1 + random() % 2048});
			}
			oracle[lifetime] = spans;
			Require(index.Replace(lifetime, spans), "replacement");
		}
		Require(index.ResourceCount() == oracle.size() && index.Validate(), "AVL balance, ordering, max-end and reverse identity");
		const uint64_t begin = random() % 3200;
		query(1 + random() % 9, begin, begin + 1 + random() % 64);
	}
	for (const auto& resource: oracle) index.Erase(resource.first);
	Require(index.Size() == 0 && index.ResourceCount() == 0 && index.Validate(), "retire all");

	// Deterministic work bounds establish scaling independently of machine load.
	// Single-identity sorted insertion is the worst shape for an unbalanced tree.
	for (uint64_t population: {256ULL, 4096ULL, 65536ULL})
	{
		index.Clear();
		for (uint64_t owner = 1; owner <= population; ++owner)
			Require(index.Replace(owner, {{1, owner * 16, owner * 16 + 8}}), "scaling initial population");
		Require(index.Validate(), "scaling population AVL");
		index.TakeWork();
		const auto start = std::chrono::steady_clock::now();
		constexpr uint64_t operations = 2048;
		for (uint64_t operation = 0; operation < operations; ++operation)
		{
			const uint64_t owner = 1 + (operation * 311) % population;
			Require(index.Replace(owner, {{1, owner * 16 + operation % 2, owner * 16 + 8}}), "scaling replacement");
		}
		const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
		const auto work = index.TakeWork();
		uint64_t logarithm = 0;
		for (uint64_t n = population; n != 0; n >>= 1) ++logarithm;
		Require(work.nodes <= operations * logarithm * 6 && index.Validate(), "replacement logarithmic work bound");
		std::printf("handoff-index population=%llu replacements=%llu node_visits=%llu rotations=%llu elapsed_us=%lld full_audits=0\n",
		    static_cast<unsigned long long>(population), static_cast<unsigned long long>(operations),
		    static_cast<unsigned long long>(work.nodes), static_cast<unsigned long long>(work.rotations), static_cast<long long>(elapsed));
	}
	std::printf("MemoryHandoffPhysicalIndex PASS randomized_mutations=20000 oracle_queries=%llu invalid/duplicate/nested/early-denial=pass\n",
	    static_cast<unsigned long long>(queries));
}
