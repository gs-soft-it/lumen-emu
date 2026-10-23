// Lumen, 2026. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace Lumen::Libs::LibKernel {

// A cold writer replaces only one immutable bucket. Readers publish a private
// hazard before examining it, then take a pin on the selected native record.
// A pin protects object lifetime, not guest addresses or native mutex ownership.
template<class T, size_t BucketCount = 256>
class ReadMostlyObjectDirectory final
{
	static_assert(BucketCount != 0 && (BucketCount & (BucketCount - 1)) == 0);
	static constexpr uint64_t Active = uint64_t {1} << 63;
	static constexpr uint64_t PinMask = Active - 1;
	struct Bucket;
	struct Hazard
	{
		std::atomic<const Bucket*> bucket {nullptr};
		Hazard* next = nullptr;
		bool used = false; // Accessed only by this hazard's thread.
	};
	struct HazardDomain
	{
		std::mutex mutex;
		Hazard* first = nullptr;
	};
	static HazardDomain& Domain() { static HazardDomain domain; return domain; }
	struct ThreadHazards
	{
		std::vector<std::unique_ptr<Hazard>> entries;
		~ThreadHazards()
		{
			auto& domain = Domain();
			std::lock_guard lock(domain.mutex);
			for (const auto& entry: entries)
			{
				assert(!entry->used && entry->bucket.load() == nullptr);
				auto** cursor = &domain.first;
				while (*cursor != entry.get()) { assert(*cursor != nullptr); cursor = &(*cursor)->next; }
				*cursor = entry->next;
			}
		}
		Hazard* Acquire()
		{
			for (const auto& entry: entries)
				if (!entry->used) { entry->used = true; return entry.get(); }
			auto created = std::make_unique<Hazard>();
			auto* result = created.get();
			entries.push_back(std::move(created));
			auto& domain = Domain();
			std::lock_guard lock(domain.mutex);
			result->next = domain.first;
			domain.first = result;
			result->used = true;
			return result;
		}
	};
	class ReadScope
	{
	public:
		explicit ReadScope(const std::atomic<Bucket*>& source)
		{
			// Construct the domain before the TLS owner, including its destructor.
			(void)Domain();
			static thread_local ThreadHazards hazards;
			m_hazard = hazards.Acquire();
			do {
				m_bucket = source.load(std::memory_order_seq_cst);
				m_hazard->bucket.store(m_bucket, std::memory_order_seq_cst);
			} while (m_bucket != source.load(std::memory_order_seq_cst));
		}
		~ReadScope()
		{
			m_hazard->bucket.store(nullptr, std::memory_order_seq_cst);
			m_hazard->used = false;
		}
		const Bucket* Get() const { return m_bucket; }
		ReadScope(const ReadScope&) = delete;
		ReadScope& operator=(const ReadScope&) = delete;
	private:
		Hazard* m_hazard;
		const Bucket* m_bucket;
	};

public:
	using Key = uintptr_t;
	using Reclaimer = void (*)(T*);
	struct Record
	{
		const Key key;
		T* const value;
		std::atomic<uint64_t> state {Active};
	private:
		friend class ReadMostlyObjectDirectory;
		size_t snapshot_references = 0; // Directory writer only.
		Reclaimer reclaim = nullptr;
		Record* previous = nullptr;
		Record* next = nullptr;
		Record(Key k, T* v): key(k), value(v) {}
	};
	// Callbacks may take other locks or touch VM; execute them after ALL caller
	// locks are dropped. Collection never invokes a callback under the writer.
	class Garbage final
	{
	public:
		Garbage() = default;
		Garbage(const Garbage&) = delete;
		Garbage& operator=(const Garbage&) = delete;
		Garbage(Garbage&&) = delete;
		Garbage& operator=(Garbage&&) = delete;
		~Garbage() { Drain(); }
		void Drain()
		{
			for (const auto& item: m_items) if (item.second != nullptr) item.second(item.first);
			m_items.clear();
		}
	private:
		friend class ReadMostlyObjectDirectory;
		std::vector<std::pair<T*, Reclaimer>> m_items;
	};
	enum class Lookup { Missing, Pinned, Closing };
	struct Result { Lookup status; Record* record; };

	ReadMostlyObjectDirectory() = default;
	ReadMostlyObjectDirectory(const ReadMostlyObjectDirectory&) = delete;
	ReadMostlyObjectDirectory& operator=(const ReadMostlyObjectDirectory&) = delete;
	~ReadMostlyObjectDirectory()
	{
		// Destruction requires joined readers, just as destruction of a mutex.
		// Do not use Domain(): process TLS/domain destruction may precede us.
		Garbage garbage;
		for (auto& slot: m_buckets) DropBucket(slot.load());
		for (auto* bucket: m_retired_buckets) DropBucket(bucket);
		while (m_first_record != nullptr)
		{
			auto* record = m_first_record;
			m_first_record = record->next;
			assert((record->state.load() & PinMask) == 0 && record->snapshot_references == 0);
			if (record->reclaim != nullptr) garbage.m_items.emplace_back(record->value, record->reclaim);
			delete record;
		}
	}

	Result FindAndPin(Key key, void (*before_pin)(void*) = nullptr, void* context = nullptr)
	{
		ReadScope read(m_buckets[Index(key)]);
		const auto* bucket = read.Get();
		if (bucket == nullptr) return {Lookup::Missing, nullptr};
		for (auto* record: bucket->records)
		{
			if (record->key != key) continue;
			if (before_pin != nullptr) before_pin(context);
			auto state = record->state.load(std::memory_order_acquire);
			for (;;)
			{
				if ((state & Active) == 0 || (state & PinMask) == PinMask) return {Lookup::Closing, nullptr};
				if (record->state.compare_exchange_weak(state, state + 1, std::memory_order_acquire,
				                                      std::memory_order_relaxed)) return {Lookup::Pinned, record};
			}
		}
		return {Lookup::Missing, nullptr};
	}
	static void Release(Record* record)
	{
		[[maybe_unused]] const auto previous = record->state.fetch_sub(1, std::memory_order_release);
		assert((previous & PinMask) != 0);
	}
	static bool TryClose(Record* record)
	{
		uint64_t expected = Active;
		return record->state.compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
		                                             std::memory_order_relaxed);
	}
	static void Reopen(Record* record)
	{
		assert(record->state.load(std::memory_order_relaxed) == 0);
		record->state.store(Active, std::memory_order_release);
	}

	Record* Insert(Key key, T* value, Garbage& garbage,
	               void (*before_publish)(Record*, void*) = nullptr, void* context = nullptr)
	{
		std::lock_guard lock(m_writer);
		const auto index = Index(key);
		auto* previous = m_buckets[index].load();
		if (previous != nullptr)
			for (auto* record: previous->records) if (record->key == key) return nullptr;
		auto record = std::unique_ptr<Record>(new Record(key, value));
		auto replacement = std::make_unique<Bucket>();
		if (previous != nullptr) replacement->records = previous->records;
		replacement->records.push_back(record.get());
		if (before_publish != nullptr) before_publish(record.get(), context);
		record->next = m_first_record;
		if (m_first_record != nullptr) m_first_record->previous = record.get();
		m_first_record = record.get();
		++m_record_count;
		Publish(index, replacement.release());
		auto* result = record.release();
		CollectLocked(garbage);
		return result;
	}
	bool Remove(Record* record, Reclaimer reclaim, Garbage& garbage)
	{
		std::lock_guard lock(m_writer);
		const auto index = Index(record->key);
		auto* previous = m_buckets[index].load();
		if (previous == nullptr || std::find(previous->records.begin(), previous->records.end(), record) == previous->records.end())
			return false;
		auto replacement = std::make_unique<Bucket>();
		for (auto* entry: previous->records) if (entry != record) replacement->records.push_back(entry);
		// Rollback of an unpublished slot may still meet a reader of an old
		// handle. Stop new pins; existing pins and snapshots retain the wrapper.
		record->state.fetch_and(PinMask, std::memory_order_acq_rel);
		record->reclaim = reclaim;
		m_retired_records.push_back(record);
		Publish(index, replacement->records.empty() ? nullptr : replacement.release());
		CollectLocked(garbage);
		return true;
	}
	// Mutations collect automatically. If the final mutation meets a reader,
	// its bounded retired snapshots/pins remain until a later cold collection
	// or directory teardown; releasing a hot pin never enters a global lock.
	void Collect(Garbage& garbage) { std::lock_guard lock(m_writer); CollectLocked(garbage); }
	struct Counts { size_t records; size_t retired_buckets; size_t entries_copied; };
	Counts Inspect() const
	{
		std::lock_guard lock(m_writer);
		return {m_record_count, m_retired_buckets.size(), m_entries_copied};
	}
	static size_t Index(Key key)
	{
		uint64_t mixed = static_cast<uint64_t>(key);
		mixed ^= mixed >> 33; mixed *= UINT64_C(0xff51afd7ed558ccd);
		mixed ^= mixed >> 33;
		return static_cast<size_t>(mixed) & (BucketCount - 1);
	}

private:
	struct Bucket { std::vector<Record*> records; };
	void Publish(size_t index, Bucket* replacement)
	{
		if (replacement != nullptr)
			for (auto* record: replacement->records) { ++record->snapshot_references; ++m_entries_copied; }
		auto* previous = m_buckets[index].exchange(replacement, std::memory_order_seq_cst);
		if (previous != nullptr) m_retired_buckets.push_back(previous);
	}
	static void DropBucket(Bucket* bucket)
	{
		if (bucket == nullptr) return;
		for (auto* record: bucket->records) { assert(record->snapshot_references != 0); --record->snapshot_references; }
		delete bucket;
	}
	void CollectLocked(Garbage& garbage)
	{
		auto& domain = Domain();
		std::lock_guard hazards_lock(domain.mutex);
		for (auto it = m_retired_buckets.begin(); it != m_retired_buckets.end();)
		{
			bool protected_bucket = false;
			for (auto* hazard = domain.first; hazard != nullptr; hazard = hazard->next)
				if (hazard->bucket.load(std::memory_order_seq_cst) == *it) { protected_bucket = true; break; }
			if (protected_bucket) { ++it; continue; }
			DropBucket(*it);
			it = m_retired_buckets.erase(it);
		}
		for (auto it = m_retired_records.begin(); it != m_retired_records.end();)
		{
			auto* record = *it;
			if (record->snapshot_references != 0 || record->state.load(std::memory_order_acquire) != 0)
				{ ++it; continue; }
			garbage.m_items.emplace_back(record->value, record->reclaim);
			if (record->previous != nullptr) record->previous->next = record->next;
			else m_first_record = record->next;
			if (record->next != nullptr) record->next->previous = record->previous;
			--m_record_count;
			delete record;
			it = m_retired_records.erase(it);
		}
	}
	std::array<std::atomic<Bucket*>, BucketCount> m_buckets {};
	mutable std::mutex m_writer;
	std::vector<Bucket*> m_retired_buckets;
	std::vector<Record*> m_retired_records;
	Record* m_first_record = nullptr;
	size_t m_record_count = 0;
	size_t m_entries_copied = 0;
};

} // namespace Lumen::Libs::LibKernel
