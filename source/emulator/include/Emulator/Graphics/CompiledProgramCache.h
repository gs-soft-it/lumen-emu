#ifndef INCLUDE_EMULATOR_GRAPHICS_COMPILEDPROGRAMCACHE_H_
#define INCLUDE_EMULATOR_GRAPHICS_COMPILEDPROGRAMCACHE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Lumen::Libs::Graphics {

// Owns only compiler input text/options and output words. It confers no guest
// memory authority, descriptor validity, native object lifetime or write proof.
class CompiledProgramCache
{
public:
	struct Key
	{
		std::string source;
		int optimization = 0;
		bool structural = false;
		bool validation = false;

		bool operator==(const Key& other) const
		{
			return optimization == other.optimization && structural == other.structural && validation == other.validation &&
			       source == other.source;
		}
	};

	struct Result
	{
		bool success = false;
		std::vector<uint32_t> words;
		std::string error;
	};

	using Hasher = std::function<size_t(const Key&)>;
	static constexpr size_t DEFAULT_MAX_BYTES = 32u * 1024u * 1024u;

	explicit CompiledProgramCache(size_t max_bytes = DEFAULT_MAX_BYTES, Hasher hasher = HashKey)
	    : m_max_bytes(max_bytes), m_hasher(std::move(hasher)) {}

	~CompiledProgramCache() { Reset(); }
	CompiledProgramCache(const CompiledProgramCache&) = delete;
	CompiledProgramCache& operator=(const CompiledProgramCache&) = delete;

	bool TryGet(const Key& key, Result* result)
	{
		if (result == nullptr) return false;
		const size_t hash = m_hasher(key);
		std::lock_guard<std::mutex> lock(m_mutex);
		Entry* entry = Find(key, hash);
		if (entry == nullptr) return false;
		Touch(entry);
		*result = entry->result;
		return true;
	}

	template <typename Compiler>
	Result Compile(const Key& key, Compiler&& compiler)
	{
		const size_t hash = m_hasher(key);
		uint64_t generation = 0;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (Entry* entry = Find(key, hash))
			{
				Touch(entry);
				return entry->result;
			}
			generation = m_generation;
		}

		// This callback may compile independently or recursively use the cache.
		// Never hold the cache mutex across compiler/library work.
		Result result = std::forward<Compiler>(compiler)();
		if (!result.success || result.words.empty() || MinimumCost(key, result) > m_max_bytes) return result;

		// Build owned cache storage before taking the publication lock. The result
		// returned to this caller remains independent from the retained copy.
		auto pending = std::make_unique<Entry>(key, result, hash);
		if (pending->cost > m_max_bytes) return result;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (generation != m_generation) return result;
			if (Entry* existing = Find(key, hash))
			{
				Touch(existing);
				return result;
			}
			while (m_retained_bytes > m_max_bytes - pending->cost) Remove(m_tail);
			Entry* entry = pending.release();
			const size_t bucket = hash % BUCKET_COUNT;
			entry->bucket_next = m_buckets[bucket];
			m_buckets[bucket] = entry;
			entry->next = m_head;
			if (m_head != nullptr) m_head->previous = entry;
			else m_tail = entry;
			m_head = entry;
			m_retained_bytes += entry->cost;
			++m_entry_count;
		}
		return result;
	}

	void Reset()
	{
		Entry* retired = nullptr;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			++m_generation;
			retired = m_head;
			m_head = nullptr;
			m_tail = nullptr;
			m_buckets.fill(nullptr);
			m_retained_bytes = 0;
			m_entry_count = 0;
		}
		while (retired != nullptr)
		{
			Entry* next = retired->next;
			delete retired;
			retired = next;
		}
	}

	// The budget charges complete entry objects plus owned string/vector
	// capacities, conservatively counting inline string capacity too. Fixed hash
	// buckets live inside this cache object; there are no unbounded index nodes.
	size_t RetainedBytes() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_retained_bytes;
	}

	size_t EntryCount() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_entry_count;
	}

private:
	struct Entry
	{
		Entry(const Key& input, const Result& output, size_t input_hash): key(input), result(output), hash(input_hash)
		{
			cost = StorageCost(key.source.capacity(), result.words.capacity(), result.error.capacity());
		}
		Key key;
		Result result;
		size_t hash = 0;
		size_t cost = 0;
		Entry* previous = nullptr;
		Entry* next = nullptr;
		Entry* bucket_next = nullptr;
	};

	static constexpr size_t BUCKET_COUNT = 256;

	static size_t HashKey(const Key& key)
	{
		size_t hash = std::hash<std::string>{}(key.source);
		const auto mix = [&](size_t value) { hash ^= value + size_t {0x9e3779b9u} + (hash << 6u) + (hash >> 2u); };
		mix(std::hash<int>{}(key.optimization));
		mix(key.structural ? 1u : 0u);
		mix(key.validation ? 1u : 0u);
		return hash;
	}

	static size_t StorageCost(size_t source_bytes, size_t word_count, size_t error_bytes)
	{
		constexpr size_t MAX = std::numeric_limits<size_t>::max();
		size_t cost = sizeof(Entry);
		for (const size_t bytes: {source_bytes, error_bytes, size_t {2}})
		{
			if (bytes > MAX - cost) return MAX;
			cost += bytes;
		}
		if (word_count > (MAX - cost) / sizeof(uint32_t)) return MAX;
		return cost + word_count * sizeof(uint32_t);
	}

	static size_t MinimumCost(const Key& key, const Result& result)
	{
		return StorageCost(key.source.size(), result.words.size(), result.error.size());
	}

	Entry* Find(const Key& key, size_t hash) const
	{
		for (Entry* entry = m_buckets[hash % BUCKET_COUNT]; entry != nullptr; entry = entry->bucket_next)
		{
			if (entry->hash == hash && entry->key == key) return entry;
		}
		return nullptr;
	}

	void Touch(Entry* entry)
	{
		if (entry == m_head) return;
		entry->previous->next = entry->next;
		if (entry->next != nullptr) entry->next->previous = entry->previous;
		else m_tail = entry->previous;
		entry->previous = nullptr;
		entry->next = m_head;
		m_head->previous = entry;
		m_head = entry;
	}

	void Remove(Entry* entry)
	{
		Entry** slot = &m_buckets[entry->hash % BUCKET_COUNT];
		while (*slot != entry) slot = &(*slot)->bucket_next;
		*slot = entry->bucket_next;
		if (entry->previous != nullptr) entry->previous->next = entry->next;
		else m_head = entry->next;
		if (entry->next != nullptr) entry->next->previous = entry->previous;
		else m_tail = entry->previous;
		m_retained_bytes -= entry->cost;
		--m_entry_count;
		delete entry;
	}

	const size_t m_max_bytes;
	const Hasher m_hasher;
	mutable std::mutex m_mutex;
	std::array<Entry*, BUCKET_COUNT> m_buckets {};
	Entry* m_head = nullptr;
	Entry* m_tail = nullptr;
	size_t m_retained_bytes = 0;
	size_t m_entry_count = 0;
	uint64_t m_generation = 0;
};

} // namespace Lumen::Libs::Graphics

#endif
