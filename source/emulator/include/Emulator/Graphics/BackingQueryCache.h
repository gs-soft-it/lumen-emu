#ifndef EMULATOR_GRAPHICS_BACKING_QUERY_CACHE_H_
#define EMULATOR_GRAPHICS_BACKING_QUERY_CACHE_H_

#include "Lumen/Core/VirtualMemory.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace Lumen::Libs::Graphics {

// Metadata only. The caller must supply Core's current exclusive-owner scope
// identity, never a remembered FEX token or a permission/admission approximation.
// This cache grants neither a guest pointer nor resource-content authority.
// Each instance belongs to one native thread; no entry survives a scope or
// Core mapping-generation change. Failed queries are deliberately not cached.
class BackingQueryCache
{
public:
	using Span = Core::VirtualMemory::GuestBackingSpan;
	struct Context
	{
		uint64_t token = 0;
		uint64_t generation = 0;
		bool operator==(const Context& other) const { return token == other.token && generation == other.generation; }
	};
	struct Counters
	{
		uint64_t queries = 0, hits = 0, misses = 0, bypasses = 0, invalidations = 0, evictions = 0;
	};
	static constexpr size_t SETS = 64;
	static constexpr size_t WAYS = 4;
	static constexpr size_t CAPACITY = SETS * WAYS;

	// Providers are synchronous. Re-read the context after the underlying
	// query so a callback that releases its epoch cannot publish a stale entry.
	template <typename ContextProvider, typename Query>
	bool Resolve(uint64_t address, uint64_t maximum_bytes, Span* result,
	             ContextProvider&& current_context, Query&& query)
	{
		++counters.queries;
		const Context before = current_context();
		SelectContext(before);
		if (before.token == 0 || before.generation == UINT64_MAX || result == nullptr || address == 0 || maximum_bytes == 0 ||
		    address > UINT64_MAX - maximum_bytes)
		{
			++counters.bypasses;
			return query(address, maximum_bytes, result);
		}
		if (Lookup(address, maximum_bytes, result))
		{
			++counters.hits;
			return true;
		}
		++counters.misses;
		const bool success = query(address, maximum_bytes, result);
		const Context after = current_context();
		SelectContext(after);
		if (success && before == after && after.token != 0 && result->generation == before.generation &&
		    Valid(*result, address, maximum_bytes))
			Insert(*result, maximum_bytes);
		return success;
	}

	const Counters& Stats() const { return counters; }

private:
	struct Entry
	{
		Span span {};
		uint64_t serial = 0;
		// A shortened successful answer reaches a known backing/region boundary.
		// An answer clipped only by the original request must be enlarged by a
		// fresh query before a later, larger request may reuse it.
		bool known_tail = false;
	};
	std::array<Entry, CAPACITY> entries {};
	std::array<uint8_t, SETS> replacements {};
	Context context {};
	uint64_t serial = 1;
	size_t recent = CAPACITY;
	Counters counters {};

	static size_t SetFor(uint64_t address)
	{
		address ^= address >> 33;
		address *= UINT64_C(0xff51afd7ed558ccd);
		address ^= address >> 33;
		return static_cast<size_t>(address) & (SETS - 1);
	}
	static bool Valid(const Span& span, uint64_t address, uint64_t maximum_bytes)
	{
		return span.identity != 0 && span.address == address && span.size != 0 && span.size <= maximum_bytes &&
		       span.address <= UINT64_MAX - span.size && span.offset <= UINT64_MAX - span.size;
	}
	void SelectContext(Context next)
	{
		if (next == context) return;
		context = next;
		++counters.invalidations;
		recent = CAPACITY;
		if (++serial == 0)
		{
			// This path is unreachable in an ordinary session, but wraparound
			// must not turn an old retained entry into current metadata.
			for (auto& entry: entries) entry.serial = 0;
			serial = 1;
		}
	}
	bool ReadEntry(size_t index, uint64_t address, uint64_t maximum_bytes, Span* result)
	{
		const auto& entry = entries[index];
		if (entry.serial != serial || address < entry.span.address) return false;
		const uint64_t delta = address - entry.span.address;
		if (delta >= entry.span.size) return false;
		const uint64_t remaining = entry.span.size - delta;
		if (maximum_bytes > remaining && !entry.known_tail) return false;
		*result = {entry.span.identity, entry.span.offset + delta, address,
		           std::min(maximum_bytes, remaining), entry.span.generation};
		recent = index;
		return true;
	}
	bool Lookup(uint64_t address, uint64_t maximum_bytes, Span* result)
	{
		if (recent < CAPACITY && ReadEntry(recent, address, maximum_bytes, result)) return true;
		const size_t first = SetFor(address) * WAYS;
		for (size_t way = 0; way < WAYS; ++way)
			if (ReadEntry(first + way, address, maximum_bytes, result)) return true;
		return false;
	}
	void Insert(const Span& span, uint64_t maximum_bytes)
	{
		const size_t set = SetFor(span.address), first = set * WAYS;
		size_t selected = CAPACITY;
		for (size_t way = 0; way < WAYS; ++way)
		{
			const auto& entry = entries[first + way];
			if (entry.serial == serial && entry.span.address == span.address) { selected = first + way; break; }
			if (entry.serial != serial && selected == CAPACITY) selected = first + way;
		}
		if (selected == CAPACITY)
		{
			selected = first + replacements[set];
			replacements[set] = static_cast<uint8_t>((replacements[set] + 1) % WAYS);
			++counters.evictions;
		}
		entries[selected] = {span, serial, span.size < maximum_bytes};
		recent = selected;
	}
};

} // namespace Lumen::Libs::Graphics

#endif
