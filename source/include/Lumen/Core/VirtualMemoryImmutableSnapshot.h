#ifndef INCLUDE_LUMEN_CORE_VIRTUALMEMORYIMMUTABLESNAPSHOT_H_
#define INCLUDE_LUMEN_CORE_VIRTUALMEMORYIMMUTABLESNAPSHOT_H_

#include "Lumen/Core/VirtualMemory.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || \
    LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || \
    LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID

namespace Lumen::Core::VirtualMemory::ImmutableSnapshot {

struct EnvironmentRequest
{
	bool present = false;
	bool exact_one = false;
};

inline const EnvironmentRequest& RequestedEnvironmentValue()
{
	// Copy the semantics rather than retaining getenv's raw pointer: later
	// setenv/unsetenv calls may invalidate that storage, while this selector and
	// its receipt are process-latched to the first observation.
	static const EnvironmentRequest request = []
	{
		const char* value =
		    std::getenv("LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT");
		return EnvironmentRequest {
		    value != nullptr,
		    value != nullptr && std::strcmp(value, "1") == 0};
	}();
	return request;
}

inline bool Requested()
{
	// Freeze the product choice before the first VM operation. The promoted
	// macOS and Android baselines enable the snapshot when the selector is
	// absent. iOS retains its previous default. Exact one enables everywhere;
	// any other explicit value selects the historical registry path.
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	constexpr bool product_default = true;
#else
	constexpr bool product_default = false;
#endif
	static const bool requested = [=]
	{
		const auto& value = RequestedEnvironmentValue();
		return !value.present ? product_default : value.exact_one;
	}();
	return requested;
}

struct Entry
{
	uint64_t           begin = 0;
	uint64_t           end = 0;
	uint64_t           owner = 0;
	ManagedRegionState state = ManagedRegionState::Unmanaged;
	Mode               mode = Mode::NoAccess;
};

inline bool ClipRegionToPageWindow(uint64_t address, uint64_t page_size,
	                               uint64_t preceding_page_limit,
	                               uint64_t following_page_limit,
	                               ManagedRegion* region)
{
	if (region == nullptr || region->begin >= region->end || page_size == 0)
	{
		return false;
	}
	const uint64_t target_page = address / page_size;
	const uint64_t first_page = target_page > preceding_page_limit
	                                ? target_page - preceding_page_limit
	                                : 0;
	const uint64_t first = first_page * page_size;
	if (region->begin < first) region->begin = first;

	const uint64_t max_page = (std::numeric_limits<uint64_t>::max)() / page_size;
	if (following_page_limit < max_page &&
	    target_page <= max_page - following_page_limit - 1)
	{
		const uint64_t end =
		    (target_page + following_page_limit + 1) * page_size;
		if (region->end > end) region->end = end;
	}
	return region->begin < region->end && address >= region->begin &&
	       address < region->end;
}

class Builder final
{
public:
	Builder() = default;
	~Builder() { std::free(m_entries); }

	Builder(const Builder&) = delete;
	Builder& operator=(const Builder&) = delete;

	bool Reserve(size_t capacity)
	{
		if (capacity <= m_capacity) return true;
		if (capacity > (std::numeric_limits<size_t>::max)() / sizeof(Entry))
		{
			return false;
		}
		void* resized = std::realloc(m_entries, capacity * sizeof(Entry));
		if (resized == nullptr) return false;
		m_entries = static_cast<Entry*>(resized);
		m_capacity = capacity;
		return true;
	}

	bool Append(const Entry& entry)
	{
		if (entry.begin >= entry.end || entry.owner == 0 ||
		    entry.state == ManagedRegionState::Unmanaged)
		{
			return false;
		}
		if (m_size == m_capacity)
		{
			constexpr size_t initial_capacity = 16;
			const size_t max_capacity =
			    (std::numeric_limits<size_t>::max)() / sizeof(Entry);
			if (m_capacity >= max_capacity) return false;
			const size_t capacity = m_capacity == 0
			                            ? initial_capacity
			                            : (m_capacity > max_capacity / 2
			                                   ? max_capacity
			                                   : m_capacity * 2);
			if (capacity <= m_capacity || !Reserve(capacity)) return false;
		}
		m_entries[m_size++] = entry;
		return true;
	}

	bool AppendAll(const Entry* entries, size_t size)
	{
		if (entries == nullptr && size != 0) return false;
		if (size > (std::numeric_limits<size_t>::max)() - m_size ||
		    !Reserve(m_size + size))
		{
			return false;
		}
		for (size_t index = 0; index < size; index++)
		{
			if (!Append(entries[index])) return false;
		}
		return true;
	}

	bool Normalize()
	{
		if (m_size > 1)
		{
			std::sort(m_entries, m_entries + m_size,
			          [](const Entry& left, const Entry& right)
			          {
				          if (left.begin != right.begin) return left.begin < right.begin;
				          if (left.end != right.end) return left.end < right.end;
				          return left.owner < right.owner;
			          });
		}

		size_t output = 0;
		for (size_t index = 0; index < m_size; index++)
		{
			const Entry entry = m_entries[index];
			if (entry.begin >= entry.end || entry.owner == 0 ||
			    entry.state == ManagedRegionState::Unmanaged)
			{
				return false;
			}
			if (output != 0)
			{
				Entry& previous = m_entries[output - 1];
				if (entry.begin < previous.end) return false;
				if (entry.begin == previous.end && entry.owner == previous.owner &&
				    entry.state == previous.state && entry.mode == previous.mode)
				{
					previous.end = entry.end;
					continue;
				}
			}
			m_entries[output++] = entry;
		}
		m_size = output;
		return true;
	}

	const Entry* Data() const { return m_entries; }
	size_t Size() const { return m_size; }

private:
	Entry* m_entries = nullptr;
	size_t m_size = 0;
	size_t m_capacity = 0;
};

class Publisher final
{
public:
	Publisher() = default;
	~Publisher()
	{
		FreeSnapshot(m_current.load(std::memory_order_relaxed));
		while (m_retired != nullptr)
		{
			Snapshot* next = m_retired->next_retired;
			FreeSnapshot(m_retired);
			m_retired = next;
		}
	}

	Publisher(const Publisher&) = delete;
	Publisher& operator=(const Publisher&) = delete;

	bool Publish(Builder& builder)
	{
		if (!builder.Normalize())
		{
			Invalidate();
			return false;
		}

		const size_t count = builder.Size();
		if (count > ((std::numeric_limits<size_t>::max)() - sizeof(Snapshot)) /
		                sizeof(Entry))
		{
			Invalidate();
			return false;
		}
		const size_t bytes = sizeof(Snapshot) + count * sizeof(Entry);
		auto* snapshot = static_cast<Snapshot*>(std::malloc(bytes));
		if (snapshot == nullptr)
		{
			Invalidate();
			return false;
		}
		snapshot->next_retired = nullptr;
		snapshot->entry_count = count;
		if (count != 0)
		{
			std::memcpy(snapshot->Entries(), builder.Data(), count * sizeof(Entry));
		}

		Retire(m_current.exchange(snapshot, std::memory_order_seq_cst));
		ReclaimIfQuiescent();
		return true;
	}

	void Invalidate()
	{
		Retire(m_current.exchange(nullptr, std::memory_order_seq_cst));
		ReclaimIfQuiescent();
	}

	// Writers call this while holding the authoritative platform registry lock.
	// New snapshot readers divert to that same legacy lock, while already active
	// readers drain before a physical protection or lifetime mutation begins.
	void BeginMutation()
	{
		m_mutation_active.store(true, std::memory_order_seq_cst);
		while (m_readers.load(std::memory_order_seq_cst) != 0)
		{
			std::this_thread::yield();
		}
	}

	void EndMutation()
	{
		m_mutation_active.store(false, std::memory_order_seq_cst);
	}

	const Entry* CurrentEntriesForWriter(size_t* count) const
	{
		const Snapshot* snapshot = m_current.load(std::memory_order_relaxed);
		if (count != nullptr) *count = snapshot == nullptr ? 0 : snapshot->entry_count;
		return snapshot == nullptr ? nullptr : snapshot->Entries();
	}

	// Returns false when publication is unavailable or a mutation is active and
	// the caller must use the authoritative legacy registry. A true return may
	// still describe an unmanaged address.
	bool Lookup(uint64_t address, ManagedRegionState* state, ManagedRegion* region,
	            bool emit_query_coverage, bool pinned_by_execution_barrier = false,
	            uint64_t* next_region_begin = nullptr) const
	{
		// The FEX signal reader is already pinned by its execution barrier:
		// exclusive VM mutations cannot proceed, while additive publication may
		// replace this snapshot without changing any existing mapping. It may
		// read the prior snapshot during publication, still using the reclamation
		// counter. Ordinary readers retain the historical mutation exclusion.
		if (!pinned_by_execution_barrier && m_mutation_active.load(std::memory_order_seq_cst)) return false;
		m_readers.fetch_add(1, std::memory_order_seq_cst);
		if (!pinned_by_execution_barrier && m_mutation_active.load(std::memory_order_seq_cst))
		{
			m_readers.fetch_sub(1, std::memory_order_seq_cst);
			return false;
		}
		const Snapshot* snapshot = m_current.load(std::memory_order_seq_cst);
		if (snapshot == nullptr)
		{
			m_readers.fetch_sub(1, std::memory_order_seq_cst);
			return false;
		}

		ManagedRegionState result_state = ManagedRegionState::Unmanaged;
		ManagedRegion result_region {0, 0, Mode::NoAccess};
		size_t begin = 0;
		size_t end = snapshot->entry_count;
		while (begin < end)
		{
			const size_t middle = begin + (end - begin) / 2;
			if (snapshot->Entries()[middle].begin <= address)
			{
				begin = middle + 1;
			} else
			{
				end = middle;
			}
		}
		if (next_region_begin != nullptr)
		{
			*next_region_begin = begin < snapshot->entry_count
			    ? snapshot->Entries()[begin].begin : (std::numeric_limits<uint64_t>::max)();
		}
		if (begin != 0)
		{
			const Entry& entry = snapshot->Entries()[begin - 1];
			if (address >= entry.begin && address < entry.end)
			{
				result_state = entry.state;
				result_region = ManagedRegion {entry.begin, entry.end, entry.mode};
			}
		}

		if (state != nullptr) *state = result_state;
		if (region != nullptr) *region = result_region;
		m_readers.fetch_sub(1, std::memory_order_seq_cst);

		if (emit_query_coverage && result_state != ManagedRegionState::Unmanaged)
		{
			EmitCoverageOnce();
		}
		return true;
	}

private:
	struct Snapshot
	{
		Snapshot* next_retired;
		size_t entry_count;

		Entry* Entries() { return reinterpret_cast<Entry*>(this + 1); }
		const Entry* Entries() const { return reinterpret_cast<const Entry*>(this + 1); }
	};

	static void FreeSnapshot(Snapshot* snapshot) { std::free(snapshot); }

	void Retire(Snapshot* snapshot)
	{
		if (snapshot == nullptr) return;
		snapshot->next_retired = m_retired;
		m_retired = snapshot;
	}

	void ReclaimIfQuiescent()
	{
		if (m_readers.load(std::memory_order_seq_cst) != 0) return;
		while (m_retired != nullptr)
		{
			Snapshot* next = m_retired->next_retired;
			FreeSnapshot(m_retired);
			m_retired = next;
		}
	}

	static void EmitCoverageOnce()
	{
		static std::atomic_bool emitted {false};
		if (emitted.load(std::memory_order_relaxed)) return;
		bool expected = false;
		if (emitted.compare_exchange_strong(expected, true,
		                                    std::memory_order_relaxed,
		                                    std::memory_order_relaxed))
		{
			const auto& requested = RequestedEnvironmentValue();
			std::fprintf(stderr,
			             "[IMMUTABLE_VM_REGION_SNAPSHOT_FINAL] requested=%s effective=1 published=positive lookup=positive status=clean coverage=positive\n",
			             requested.present ? "1" : "absent");
		}
	}

	std::atomic<Snapshot*> m_current {nullptr};
	mutable std::atomic<uint64_t> m_readers {0};
	mutable std::atomic_bool m_mutation_active {false};
	// Writers already hold the platform VM registry lock, so this list requires
	// no second lock. Retired snapshots are reclaimed only at a zero-reader
	// quiescent point.
	Snapshot* m_retired = nullptr;
};

} // namespace Lumen::Core::VirtualMemory::ImmutableSnapshot

#endif

#endif /* INCLUDE_LUMEN_CORE_VIRTUALMEMORYIMMUTABLESNAPSHOT_H_ */
