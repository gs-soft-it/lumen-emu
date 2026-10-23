#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>

namespace Lumen::Cpu::Phase6AL {

constexpr size_t kSignatureCapacity = 64;
constexpr size_t kHistoryCapacity = 65536;
constexpr size_t kFullyAssociativeEntries = 512;
constexpr size_t kLargeArchiveEntries = 1024;
constexpr size_t kArchiveWays = 4;
constexpr uint64_t kBudget16 = 16u * 1024u * 1024u;
constexpr uint64_t kBudget32 = 32u * 1024u * 1024u;

struct Identity
{
	uint64_t hash = 0;
	uint64_t rip = 0;
	std::array<uint8_t, kSignatureCapacity> signature {};
	uint32_t code_size = 0;
	uint32_t code_capacity = 0;
	uint8_t byte_count = 0;

	bool Valid() const
	{
		return hash != 0 && byte_count != 0 &&
		       byte_count <= signature.size();
	}

	bool SameExact(const Identity& other) const
	{
		return hash == other.hash && rip == other.rip &&
		       byte_count == other.byte_count &&
		       std::memcmp(signature.data(), other.signature.data(),
		                   byte_count) == 0;
	}
};

enum class LossKind : uint8_t
{
	None,
	SetEvict,
	BudgetDrop,
	Reset,
};

enum class Cause : uint8_t
{
	Cold,
	Variant,
	Conflict,
	Capacity,
	Budget,
	Count,
};

enum class OwnerRole : uint8_t
{
	Standalone,
	Primary,
	Worker,
};

inline const char* OwnerRoleName(OwnerRole role)
{
	switch (role)
	{
		case OwnerRole::Standalone: return "standalone";
		case OwnerRole::Primary: return "primary";
		case OwnerRole::Worker: return "worker";
		default: return "invalid";
	}
}

constexpr size_t CauseIndex(Cause cause)
{
	return static_cast<size_t>(cause);
}

inline const char* CauseName(Cause cause)
{
	switch (cause)
	{
		case Cause::Cold: return "cold";
		case Cause::Variant: return "variant";
		case Cause::Conflict: return "conflict";
		case Cause::Capacity: return "capacity";
		case Cause::Budget: return "budget";
		default: return "invalid";
	}
}

struct GlobalBudget
{
	std::mutex mutex;
	uint64_t limit = 0;
	uint64_t resident = 0;
	uint64_t peak = 0;
	uint64_t invalid = 0;

	explicit GlobalBudget(uint64_t selected_limit): limit(selected_limit) {}

	bool Exchange(uint64_t outgoing, uint64_t incoming)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (resident > limit || outgoing > resident)
		{
			invalid++;
			return false;
		}
		const uint64_t base = resident - outgoing;
		if (incoming > limit - base) return false;
		resident = base + incoming;
		peak = (std::max)(peak, resident);
		return true;
	}

	void Snapshot(uint64_t* current, uint64_t* high, uint64_t* errors)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (current != nullptr) *current = resident;
		if (high != nullptr) *high = peak;
		if (errors != nullptr) *errors = invalid;
	}
};

struct ProcessState
{
	GlobalBudget k1024_16 {kBudget16};
	GlobalBudget k1024_32 {kBudget32};
	std::mutex report_mutex;
	std::mutex sequence_mutex;
	uint64_t next_owner = 0;
	uint64_t primary_markers = 0;
	uint64_t primary_allocations = 0;
	uint64_t primary_owner = 0;
	uint64_t role_invalid = 0;
	std::atomic<uint64_t> process_invalid {0};
};

inline ProcessState& State()
{
	// Match the actual archive's late-TLS teardown policy.  A guest worker may
	// destroy its cache during process shutdown, after ordinary static objects
	// would already be gone.
	static auto* state = new (std::nothrow) ProcessState;
	if (state == nullptr) std::abort();
	return *state;
}

inline void MarkProcessActivationInvalid()
{
	auto& state = State();
	std::lock_guard<std::mutex> lock(state.sequence_mutex);
	state.process_invalid.fetch_add(1, std::memory_order_relaxed);
}

inline void MarkPrimaryThread(bool local_cache_already_allocated = false)
{
	auto& state = State();
	std::lock_guard<std::mutex> lock(state.sequence_mutex);
	if (state.primary_markers == UINT64_MAX)
	{
		state.role_invalid++;
		state.process_invalid.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	state.primary_markers++;
	if (state.primary_markers != 1 || local_cache_already_allocated)
	{
		state.role_invalid++;
		state.process_invalid.fetch_add(1, std::memory_order_relaxed);
	}
}

inline uint64_t AllocateOwner(OwnerRole role)
{
	auto& state = State();
	std::lock_guard<std::mutex> lock(state.sequence_mutex);
	if (state.next_owner == UINT64_MAX) std::abort();
	const uint64_t owner = ++state.next_owner;
	if (role == OwnerRole::Primary)
	{
		if (state.primary_allocations == UINT64_MAX)
		{
			state.role_invalid++;
			state.process_invalid.fetch_add(1, std::memory_order_relaxed);
			return owner;
		}
		state.primary_allocations++;
		if (state.primary_markers != 1 ||
		    state.primary_allocations != 1 || state.primary_owner != 0)
		{
			state.role_invalid++;
			state.process_invalid.fetch_add(1, std::memory_order_relaxed);
		}
		else
			state.primary_owner = owner;
	}
	return owner;
}

class StderrGuard final
{
public:
	StderrGuard()
	{
#if defined(_WIN32)
		::_lock_file(stderr);
#else
		::flockfile(stderr);
#endif
	}

	~StderrGuard()
	{
#if defined(_WIN32)
		::_unlock_file(stderr);
#else
		::funlockfile(stderr);
#endif
	}

	StderrGuard(const StderrGuard&) = delete;
	StderrGuard& operator=(const StderrGuard&) = delete;
};

struct ModelStats
{
	uint64_t probes = 0;
	uint64_t hits = 0;
	uint64_t misses = 0;
	uint64_t puts = 0;
	uint64_t stored = 0;
	uint64_t drops = 0;
	uint64_t budget_drops = 0;
	uint64_t resident = 0;
	uint64_t resident_peak = 0;
	uint64_t capacity_resident = 0;
	uint64_t capacity_peak = 0;
	uint64_t saved_events = 0;
	uint64_t saved_nanoseconds = 0;
	uint64_t global_capacity = 0;
	uint64_t global_peak = 0;
	uint64_t global_invalid = 0;
	uint64_t invalid = 0;
};

class FullyAssociative512
{
public:
	FullyAssociative512() { ResetStorage(); }
	~FullyAssociative512() = default;

	FullyAssociative512(const FullyAssociative512&) = delete;
	FullyAssociative512& operator=(const FullyAssociative512&) = delete;

	bool Take(uint32_t token)
	{
		m_stats.probes++;
		const uint16_t index = Find(token);
		if (index == kNil)
		{
			m_stats.misses++;
			return false;
		}
		RemoveLive(index);
		EraseMap(token);
		ReleaseNode(index);
		m_stats.hits++;
		m_stats.resident--;
		return true;
	}

	bool Put(uint32_t token)
	{
		m_stats.puts++;
		if (token == 0 || token == kTombstone || Find(token) != kNil)
		{
			m_stats.invalid++;
			return false;
		}
		if (m_free == kNil)
		{
			if (m_tail == kNil)
			{
				m_stats.invalid++;
				return false;
			}
			const uint16_t outgoing = m_tail;
			const uint32_t outgoing_token = m_nodes[outgoing].token;
			RemoveLive(outgoing);
			EraseMap(outgoing_token);
			ReleaseNode(outgoing);
			m_stats.drops++;
		} else
		{
			m_stats.resident++;
			m_stats.resident_peak =
			    (std::max)(m_stats.resident_peak, m_stats.resident);
		}
		const uint16_t incoming = AcquireNode();
		if (incoming == kNil || !InsertMap(token, incoming))
		{
			m_stats.invalid++;
			return false;
		}
		m_nodes[incoming].token = token;
		InsertFront(incoming);
		m_stats.stored++;
		return true;
	}

	void Clear()
	{
		ResetStorage();
		m_stats.resident = 0;
	}

	const ModelStats& Stats() const { return m_stats; }
	ModelStats& MutableStats() { return m_stats; }

private:
	static constexpr uint16_t kNil = UINT16_MAX;
	static constexpr uint32_t kTombstone = UINT32_MAX;
	static constexpr size_t kMapCapacity = 1024;

	struct Node
	{
		uint32_t token = 0;
		uint16_t previous = kNil;
		uint16_t next = kNil;
		uint16_t free_next = kNil;
	};

	struct MapSlot
	{
		uint32_t token = 0;
		uint16_t node = kNil;
	};

	static size_t MapIndex(uint32_t token)
	{
		return static_cast<size_t>(token * 2654435761u) &
		       (kMapCapacity - 1u);
	}

	uint16_t Find(uint32_t token) const
	{
		if (token == 0 || token == kTombstone) return kNil;
		size_t slot = MapIndex(token);
		for (size_t probe = 0; probe < kMapCapacity; probe++)
		{
			const auto& item = m_map[slot];
			if (item.token == 0) return kNil;
			if (item.token == token) return item.node;
			slot = (slot + 1u) & (kMapCapacity - 1u);
		}
		return kNil;
	}

	bool InsertMap(uint32_t token, uint16_t node)
	{
		size_t slot = MapIndex(token);
		size_t tombstone = kMapCapacity;
		for (size_t probe = 0; probe < kMapCapacity; probe++)
		{
			auto& item = m_map[slot];
			if (item.token == token) return false;
			if (item.token == kTombstone && tombstone == kMapCapacity)
				tombstone = slot;
			if (item.token == 0)
			{
				const size_t destination =
				    tombstone == kMapCapacity ? slot : tombstone;
				m_map[destination] = {token, node};
				return true;
			}
			slot = (slot + 1u) & (kMapCapacity - 1u);
		}
		if (tombstone != kMapCapacity)
		{
			m_map[tombstone] = {token, node};
			return true;
		}
		return false;
	}

	void EraseMap(uint32_t token)
	{
		size_t slot = MapIndex(token);
		for (size_t probe = 0; probe < kMapCapacity; probe++)
		{
			auto& item = m_map[slot];
			if (item.token == 0) break;
			if (item.token == token)
			{
				item = {kTombstone, kNil};
				return;
			}
			slot = (slot + 1u) & (kMapCapacity - 1u);
		}
		m_stats.invalid++;
	}

	void ResetStorage()
	{
		m_map = {};
		m_nodes = {};
		for (size_t index = 0; index < m_nodes.size(); index++)
		{
			m_nodes[index].free_next =
			    index + 1u < m_nodes.size()
			        ? static_cast<uint16_t>(index + 1u)
			        : kNil;
			m_nodes[index].previous = kNil;
			m_nodes[index].next = kNil;
		}
		m_free = 0;
		m_head = kNil;
		m_tail = kNil;
	}

	uint16_t AcquireNode()
	{
		if (m_free == kNil) return kNil;
		const uint16_t node = m_free;
		m_free = m_nodes[node].free_next;
		m_nodes[node].free_next = kNil;
		return node;
	}

	void ReleaseNode(uint16_t node)
	{
		m_nodes[node] = {};
		m_nodes[node].previous = kNil;
		m_nodes[node].next = kNil;
		m_nodes[node].free_next = m_free;
		m_free = node;
	}

	void InsertFront(uint16_t node)
	{
		m_nodes[node].previous = kNil;
		m_nodes[node].next = m_head;
		if (m_head != kNil) m_nodes[m_head].previous = node;
		m_head = node;
		if (m_tail == kNil) m_tail = node;
	}

	void RemoveLive(uint16_t node)
	{
		const uint16_t previous = m_nodes[node].previous;
		const uint16_t next = m_nodes[node].next;
		if (previous == kNil)
			m_head = next;
		else
			m_nodes[previous].next = next;
		if (next == kNil)
			m_tail = previous;
		else
			m_nodes[next].previous = previous;
	}

	std::array<Node, kFullyAssociativeEntries> m_nodes {};
	std::array<MapSlot, kMapCapacity> m_map {};
	uint16_t m_free = kNil;
	uint16_t m_head = kNil;
	uint16_t m_tail = kNil;
	ModelStats m_stats {};
};

template <size_t EntryCount>
class SetAssociativeArchive
{
public:
	static_assert(EntryCount % kArchiveWays == 0,
	              "phase-6AL archive geometry must have complete sets");
	static constexpr size_t kSetCount = EntryCount / kArchiveWays;
	static_assert((kSetCount & (kSetCount - 1u)) == 0,
	              "phase-6AL set count must be a power of two");

	explicit SetAssociativeArchive(GlobalBudget* budget): m_budget(budget) {}
	~SetAssociativeArchive() { Clear(); }

	SetAssociativeArchive(const SetAssociativeArchive&) = delete;
	SetAssociativeArchive& operator=(const SetAssociativeArchive&) = delete;

	bool Take(uint64_t hash, uint32_t token)
	{
		m_stats.probes++;
		if (hash == 0 || token == 0 || m_budget == nullptr)
		{
			m_stats.invalid++;
			m_stats.misses++;
			return false;
		}
		auto& set = m_sets[SetIndex(hash)];
		for (size_t way = 0; way < set.size(); way++)
		{
			if (set[way].token != token) continue;
			const Item item = set[way];
			if (!m_budget->Exchange(item.code_capacity, 0) ||
			    m_stats.resident == 0 ||
			    m_stats.capacity_resident < item.code_capacity)
			{
				m_stats.invalid++;
			} else
			{
				m_stats.resident--;
				m_stats.capacity_resident -= item.code_capacity;
			}
			for (size_t move = way; move + 1u < set.size(); move++)
				set[move] = set[move + 1u];
			set.back() = {};
			m_stats.hits++;
			return true;
		}
		m_stats.misses++;
		return false;
	}

	bool Put(uint64_t hash, uint32_t token, uint32_t code_size,
	         uint32_t code_capacity)
	{
		m_stats.puts++;
		if (hash == 0 || token == 0 || code_size == 0 ||
		    code_capacity < code_size || m_budget == nullptr)
		{
			m_stats.invalid++;
			return false;
		}
		auto& set = m_sets[SetIndex(hash)];
		size_t occupied = 0;
		bool empty_seen = false;
		for (const auto& item: set)
		{
			if (item.token == token)
			{
				m_stats.invalid++;
				return false;
			}
			if (item.token == 0)
				empty_seen = true;
			else
			{
				if (empty_seen) m_stats.invalid++;
				occupied++;
			}
		}
		const bool full = occupied == set.size();
		const Item outgoing = full ? set.back() : Item {};
		if (!m_budget->Exchange(outgoing.code_capacity, code_capacity))
		{
			m_stats.budget_drops++;
			return false;
		}
		if (full)
		{
			if (m_stats.capacity_resident < outgoing.code_capacity)
				m_stats.invalid++;
			else
				m_stats.capacity_resident -= outgoing.code_capacity;
			m_stats.drops++;
		} else
		{
			m_stats.resident++;
			m_stats.resident_peak =
			    (std::max)(m_stats.resident_peak, m_stats.resident);
		}
		const size_t last = full ? set.size() - 1u : occupied;
		for (size_t way = last; way > 0; way--) set[way] = set[way - 1u];
		set[0] = {token, code_size, code_capacity};
		m_stats.capacity_resident += code_capacity;
		m_stats.capacity_peak =
		    (std::max)(m_stats.capacity_peak, m_stats.capacity_resident);
		m_stats.stored++;
		return true;
	}

	void Clear()
	{
		if (m_budget != nullptr && m_stats.capacity_resident != 0 &&
		    !m_budget->Exchange(m_stats.capacity_resident, 0))
		{
			m_stats.invalid++;
		}
		m_sets = {};
		m_stats.resident = 0;
		m_stats.capacity_resident = 0;
	}

	const ModelStats& Stats() const { return m_stats; }
	ModelStats& MutableStats() { return m_stats; }

private:
	struct Item
	{
		uint32_t token = 0;
		uint32_t code_size = 0;
		uint32_t code_capacity = 0;
	};

	static size_t SetIndex(uint64_t hash)
	{
		return static_cast<size_t>(hash ^ (hash >> 32u)) &
		       (kSetCount - 1u);
	}

	std::array<std::array<Item, kArchiveWays>, kSetCount> m_sets {};
	GlobalBudget* m_budget = nullptr;
	ModelStats m_stats {};
};

struct ProbeTicket
{
	Identity target {};
	Identity victim {};
	uint32_t token = 0;
	LossKind previous_loss = LossKind::None;
	bool valid = false;
	bool victim_valid = false;
	bool first_exact = false;
	bool rip_seen = false;
	bool fa512_hit = false;
	bool k1024_16_hit = false;
	bool k1024_32_hit = false;
	bool actual_resident = false;
	bool actual_miss = false;
};

struct CauseStats
{
	uint64_t events = 0;
	uint64_t build_nanoseconds = 0;
	uint64_t code_capacity = 0;
	uint64_t fa512_hits = 0;
	uint64_t fa512_saved_nanoseconds = 0;
	uint64_t k1024_16_hits = 0;
	uint64_t k1024_16_saved_nanoseconds = 0;
	uint64_t k1024_32_hits = 0;
	uint64_t k1024_32_saved_nanoseconds = 0;
	uint64_t k1024_32_only_hits = 0;
	uint64_t k1024_32_only_saved_nanoseconds = 0;
	uint64_t k1024_16_only_hits = 0;
	uint64_t k1024_16_only_saved_nanoseconds = 0;
};

struct ActualStats
{
	uint64_t probes = 0;
	uint64_t hits = 0;
	uint64_t misses = 0;
	uint64_t puts = 0;
	uint64_t stored = 0;
	uint64_t set_drops = 0;
	uint64_t budget_drops = 0;
	uint64_t resets = 0;
	uint64_t resident = 0;
	uint64_t resident_peak = 0;
	uint64_t capacity_resident = 0;
	uint64_t capacity_peak = 0;
	uint64_t global_capacity = 0;
	uint64_t global_peak = 0;
	uint64_t global_invalid = 0;
	uint64_t invalid = 0;
};

struct Snapshot
{
	ActualStats actual {};
	ModelStats fa512 {};
	ModelStats k1024_16 {};
	ModelStats k1024_32 {};
	std::array<CauseStats, CauseIndex(Cause::Count)> causes {};
	uint64_t compile_ok = 0;
	uint64_t compile_fail = 0;
	uint64_t compile_nanoseconds = 0;
	uint64_t one_shot = 0;
	uint64_t one_shot_nanoseconds = 0;
	uint64_t signature_changed_admission = 0;
	uint64_t signature_changed_pre_restore = 0;
	uint64_t signature_changed_post_compile = 0;
	uint64_t wasted_build_nanoseconds = 0;
	uint64_t history_entries = 0;
	uint64_t history_dropped = 0;
	uint64_t duplicate = 0;
	uint64_t token_stale = 0;
	uint64_t unclassified = 0;
	uint64_t invalid = 0;
	bool valid = false;
};

class Telemetry
{
public:
	using SequenceLock = std::unique_lock<std::mutex>;
	using ActualGlobalSnapshot =
	    void (*)(void*, uint64_t*, uint64_t*, uint64_t*);

	explicit Telemetry(uint64_t interval = 100000,
	                   void* actual_global_context = nullptr,
	                   ActualGlobalSnapshot actual_global_snapshot = nullptr,
	                   OwnerRole role = OwnerRole::Standalone):
	    m_k1024_16(&State().k1024_16),
	    m_k1024_32(&State().k1024_32),
	    m_interval(interval == 0 ? 100000 : interval),
	    m_actual_global_context(actual_global_context),
	    m_actual_global_snapshot(actual_global_snapshot),
	    m_role(role)
	{
		m_owner = AllocateOwner(m_role);
		m_next_report = 1;
	}

	~Telemetry()
	{
		auto sequence = LockSequence();
		const uint64_t k16_invalid = m_k1024_16.Stats().invalid;
		const uint64_t k32_invalid = m_k1024_32.Stats().invalid;
		m_fa512.Clear();
		m_k1024_16.Clear();
		m_k1024_32.Clear();
		if (m_k1024_16.Stats().invalid != k16_invalid ||
		    m_k1024_32.Stats().invalid != k32_invalid)
			MarkProcessInvalid();
	}

	Telemetry(const Telemetry&) = delete;
	Telemetry& operator=(const Telemetry&) = delete;

	SequenceLock LockSequence()
	{
		return SequenceLock(State().sequence_mutex);
	}

	ProbeTicket BeginProbe(const Identity& target, const Identity* victim,
	                       bool sequence_already_locked = false)
	{
		SequenceLock sequence;
		if (!sequence_already_locked) sequence = LockSequence();
		ProbeTicket ticket {};
		ticket.target = target;
		if (victim != nullptr && victim->Valid() &&
		    victim->code_size != 0 &&
		    victim->code_capacity >= victim->code_size)
		{
			ticket.victim = *victim;
			ticket.victim_valid = true;
		}
		bool first_exact = false;
		bool rip_seen = false;
		HistoryRecord* record = Intern(target, true, &first_exact, &rip_seen);
		if (record == nullptr)
		{
			RecordInternalInvalid();
			return ticket;
		}
		ticket.valid = true;
		ticket.token = record->token;
		ticket.previous_loss = record->last_loss;
		ticket.actual_resident = record->resident;
		ticket.first_exact = first_exact;
		ticket.rip_seen = rip_seen;
		const uint64_t fa_invalid = m_fa512.Stats().invalid;
		const uint64_t k16_invalid = m_k1024_16.Stats().invalid;
		const uint64_t k32_invalid = m_k1024_32.Stats().invalid;
		ticket.fa512_hit = m_fa512.Take(ticket.token);
		ticket.k1024_16_hit =
		    m_k1024_16.Take(target.hash, ticket.token);
		ticket.k1024_32_hit =
		    m_k1024_32.Take(target.hash, ticket.token);
		if (m_fa512.Stats().invalid != fa_invalid ||
		    m_k1024_16.Stats().invalid != k16_invalid ||
		    m_k1024_32.Stats().invalid != k32_invalid)
			MarkProcessInvalid();
		return ticket;
	}

	void RecordActualResult(ProbeTicket* ticket, bool hit, bool invalid)
	{
		if (ticket == nullptr || !ticket->valid)
		{
			RecordInternalInvalid();
			return;
		}
		m_actual.probes++;
		if (invalid)
		{
			RecordActualInternalInvalid();
			return;
		}
		if (hit != ticket->actual_resident)
			RecordActualInternalInvalid();
		if (hit)
			m_actual.hits++;
		else
		{
			m_actual.misses++;
			ticket->actual_miss = true;
		}
	}

	void CompleteExchange(const ProbeTicket& ticket,
	                      bool sequence_already_locked = false)
	{
		SequenceLock sequence;
		if (!sequence_already_locked) sequence = LockSequence();
		if (!ticket.valid)
		{
			RecordInternalInvalid();
			return;
		}
		if (ticket.victim_valid)
		{
			HistoryRecord* victim =
			    Existing(ticket.victim);
			if (victim == nullptr || victim->probe_count == 0)
			{
				RecordInternalInvalid();
				return;
			}
			if (!m_fa512.Put(victim->token)) RecordDuplicate();
			const uint64_t k16_invalid = m_k1024_16.Stats().invalid;
			m_k1024_16.Put(ticket.victim.hash, victim->token,
			                  ticket.victim.code_size,
			                  ticket.victim.code_capacity);
			if (m_k1024_16.Stats().invalid != k16_invalid) RecordDuplicate();
			const uint64_t k32_invalid = m_k1024_32.Stats().invalid;
			m_k1024_32.Put(ticket.victim.hash, victim->token,
			                  ticket.victim.code_size,
			                  ticket.victim.code_capacity);
			if (m_k1024_32.Stats().invalid != k32_invalid) RecordDuplicate();
		}
	}

	void FinishEvent(bool sequence_already_locked = false)
	{
		SequenceLock sequence;
		if (!sequence_already_locked) sequence = LockSequence();
		MaybeReport();
	}

	void RecordCompileSuccess(const ProbeTicket& ticket, uint64_t nanoseconds,
	                          uint32_t code_size, uint32_t code_capacity)
	{
		if (!ticket.valid || !ticket.actual_miss || code_size == 0 ||
		    code_capacity < code_size)
		{
			RecordUnclassified();
			return;
		}
		const Cause cause = Classify(ticket);
		const size_t index = CauseIndex(cause);
		if (index >= m_causes.size())
		{
			RecordUnclassified();
			return;
		}
		m_compile_ok++;
		m_compile_nanoseconds += nanoseconds;
		m_causes[index].events++;
		m_causes[index].build_nanoseconds += nanoseconds;
		m_causes[index].code_capacity += code_capacity;
		if (ticket.fa512_hit)
		{
			m_causes[index].fa512_hits++;
			m_causes[index].fa512_saved_nanoseconds += nanoseconds;
		}
		if (ticket.k1024_16_hit)
		{
			m_causes[index].k1024_16_hits++;
			m_causes[index].k1024_16_saved_nanoseconds += nanoseconds;
		}
		if (ticket.k1024_32_hit)
		{
			m_causes[index].k1024_32_hits++;
			m_causes[index].k1024_32_saved_nanoseconds += nanoseconds;
		}
		if (ticket.k1024_32_hit && !ticket.k1024_16_hit)
		{
			m_causes[index].k1024_32_only_hits++;
			m_causes[index].k1024_32_only_saved_nanoseconds += nanoseconds;
		}
		if (ticket.k1024_16_hit && !ticket.k1024_32_hit)
		{
			m_causes[index].k1024_16_only_hits++;
			m_causes[index].k1024_16_only_saved_nanoseconds += nanoseconds;
		}
		HistoryRecord* record = RecordForToken(ticket.token);
		if (record == nullptr || !record->identity.SameExact(ticket.target))
		{
			RecordTokenStale();
			return;
		}
		record->compile_events++;
		record->compile_nanoseconds += nanoseconds;
		if (record->compile_events == 1) record->first_cause = cause;
		if (ticket.fa512_hit)
		{
			m_fa512.MutableStats().saved_events++;
			m_fa512.MutableStats().saved_nanoseconds += nanoseconds;
		}
		if (ticket.k1024_16_hit)
		{
			m_k1024_16.MutableStats().saved_events++;
			m_k1024_16.MutableStats().saved_nanoseconds += nanoseconds;
		}
		if (ticket.k1024_32_hit)
		{
			m_k1024_32.MutableStats().saved_events++;
			m_k1024_32.MutableStats().saved_nanoseconds += nanoseconds;
		}
	}

	void RecordCompileFailure(const ProbeTicket& ticket)
	{
		if (!ticket.valid || !ticket.actual_miss) RecordInternalInvalid();
		m_compile_fail++;
		MarkProcessInvalid();
		auto sequence = LockSequence();
		MaybeReport();
	}

	void RecordActualPut() { m_actual.puts++; }

	void RecordActualTake(const Identity& identity)
	{
		HistoryRecord* record = Existing(identity);
		if (record == nullptr || !record->resident ||
		    m_actual.resident == 0 ||
		    m_actual.capacity_resident < identity.code_capacity)
		{
			RecordActualInternalInvalid();
			return;
		}
		record->resident = false;
		record->last_loss = LossKind::None;
		m_actual.resident--;
		m_actual.capacity_resident -= identity.code_capacity;
	}

	void RecordActualStore(const Identity& identity)
	{
		HistoryRecord* record = Existing(identity);
		if (record == nullptr || record->probe_count == 0 ||
		    record->resident || identity.code_size == 0 ||
		    identity.code_capacity < identity.code_size)
		{
			RecordActualInternalInvalid();
			return;
		}
		record->resident = true;
		record->last_loss = LossKind::None;
		m_actual.stored++;
		m_actual.resident++;
		m_actual.capacity_resident += identity.code_capacity;
		m_actual.resident_peak =
		    (std::max)(m_actual.resident_peak, m_actual.resident);
		m_actual.capacity_peak =
		    (std::max)(m_actual.capacity_peak,
		               m_actual.capacity_resident);
	}

	void RecordActualSetDrop(const Identity& identity)
	{
		m_actual.set_drops++;
		RecordActualLoss(identity, LossKind::SetEvict);
	}

	void RecordActualBudgetDrop(const Identity& identity)
	{
		m_actual.budget_drops++;
		HistoryRecord* record = Existing(identity);
		if (record == nullptr || record->probe_count == 0 || record->resident)
		{
			RecordActualInternalInvalid();
			return;
		}
		record->resident = false;
		record->last_loss = LossKind::BudgetDrop;
	}

	void RecordActualReset(const Identity& identity)
	{
		m_actual.resets++;
		RecordActualLoss(identity, LossKind::Reset);
	}

	void RecordDuplicate()
	{
		m_duplicate++;
		MarkProcessInvalid();
	}

	void RecordActualInvalid()
	{
		RecordActualInternalInvalid();
	}

	void RecordActualGlobal(uint64_t resident, uint64_t peak, uint64_t invalid)
	{
		m_actual.global_capacity = resident;
		m_actual.global_peak = peak;
		m_actual.global_invalid = invalid;
	}

	enum class SignaturePoint : uint8_t
	{
		Admission,
		PreRestore,
		PostCompile,
	};

	void RecordSignatureChanged(SignaturePoint point,
	                            uint64_t wasted_build_nanoseconds = 0)
	{
		switch (point)
		{
			case SignaturePoint::Admission:
				m_signature_changed_admission++;
				break;
			case SignaturePoint::PreRestore:
				m_signature_changed_pre_restore++;
				break;
			case SignaturePoint::PostCompile:
				m_signature_changed_post_compile++;
				m_wasted_build_nanoseconds += wasted_build_nanoseconds;
				break;
		}
	}

	void FinalizeAndReport(bool sequence_already_locked = false)
	{
		SequenceLock sequence;
		if (!sequence_already_locked) sequence = LockSequence();
		if (m_finalized) return;
		m_finalized = true;
		const uint64_t k16_invalid = m_k1024_16.Stats().invalid;
		const uint64_t k32_invalid = m_k1024_32.Stats().invalid;
		m_fa512.Clear();
		m_k1024_16.Clear();
		m_k1024_32.Clear();
		if (m_k1024_16.Stats().invalid != k16_invalid ||
		    m_k1024_32.Stats().invalid != k32_invalid)
			MarkProcessInvalid();
		Report(true);
	}

	void Report(bool final)
	{
		if (final && m_final_reported) return;
		std::lock_guard<std::mutex> report_lock(State().report_mutex);
		StderrGuard stderr_guard;
		RefreshActualGlobal();
		const Snapshot snapshot = GetSnapshot(final);
		const auto& state = State();
		const uint64_t owners = state.next_owner;
		const uint64_t process_invalid =
		    state.process_invalid.load(std::memory_order_relaxed);
		const bool scoped_owner = m_role != OwnerRole::Standalone;
		const bool primary_pending =
		    state.primary_allocations == 0 && state.primary_owner == 0;
		const bool primary_bound =
		    state.primary_allocations == 1 && state.primary_owner != 0 &&
		    state.primary_owner <= owners;
		const bool role_valid =
		    !scoped_owner ||
		    (state.primary_markers == 1 && state.role_invalid == 0 &&
		     process_invalid == 0 &&
		     ((primary_pending && m_role == OwnerRole::Worker) ||
		      (primary_bound &&
		       ((m_role == OwnerRole::Primary && m_owner == state.primary_owner) ||
		        (m_role == OwnerRole::Worker && m_owner != state.primary_owner)))));
		const bool report_valid = snapshot.valid && role_valid;
		m_reports++;
		if (final) m_final_reported = true;
		std::fprintf(
		    stderr,
		    "[A64_DYNAREC_PHASE6AL] version=2 owner=%" PRIu64
		    " owners=%" PRIu64
		    " role=%s primary_markers=%" PRIu64
		    " primary_allocations=%" PRIu64
		    " primary_owner=%" PRIu64 " role_invalid=%" PRIu64
		    " process_invalid=%" PRIu64 " process_valid=%u"
		    " final=%u report=%" PRIu64 " interval=%" PRIu64
		    " valid=%u actual_probe=%" PRIu64 " actual_hit=%" PRIu64
		    " actual_miss=%" PRIu64 " compile_ok=%" PRIu64
		    " compile_fail=%" PRIu64 " compile_ns=%" PRIu64
		    " put=%" PRIu64 " stored=%" PRIu64
		    " set_drop=%" PRIu64 " budget_drop=%" PRIu64
		    " reset=%" PRIu64
		    " resident=%" PRIu64 " peak=%" PRIu64
		    " capacity=%" PRIu64 " capacity_peak=%" PRIu64
		    " global_capacity=%" PRIu64 " global_peak=%" PRIu64
		    " global_invalid=%" PRIu64 " history=%" PRIu64
		    " history_drop=%" PRIu64 " one_shot=%" PRIu64
		    " one_shot_ns=%" PRIu64 " sig_admission=%" PRIu64
		    " sig_pre_restore=%" PRIu64 " sig_post_compile=%" PRIu64
		    " wasted_build_ns=%" PRIu64 " duplicate=%" PRIu64
		    " token_stale=%" PRIu64 " unclassified=%" PRIu64
		    " invalid=%" PRIu64 "\n",
		    m_owner, owners, OwnerRoleName(m_role), state.primary_markers,
		    state.primary_allocations, state.primary_owner,
		    state.role_invalid, process_invalid, role_valid ? 1u : 0u,
		    final ? 1u : 0u, m_reports, m_interval,
		    report_valid ? 1u : 0u, snapshot.actual.probes,
		    snapshot.actual.hits, snapshot.actual.misses,
		    snapshot.compile_ok, snapshot.compile_fail,
		    snapshot.compile_nanoseconds, snapshot.actual.puts,
		    snapshot.actual.stored, snapshot.actual.set_drops,
		    snapshot.actual.budget_drops, snapshot.actual.resets,
		    snapshot.actual.resident,
		    snapshot.actual.resident_peak,
		    snapshot.actual.capacity_resident,
		    snapshot.actual.capacity_peak,
		    snapshot.actual.global_capacity, snapshot.actual.global_peak,
		    snapshot.actual.global_invalid, snapshot.history_entries,
		    snapshot.history_dropped, snapshot.one_shot,
		    snapshot.one_shot_nanoseconds,
		    snapshot.signature_changed_admission,
		    snapshot.signature_changed_pre_restore,
		    snapshot.signature_changed_post_compile,
		    snapshot.wasted_build_nanoseconds, snapshot.duplicate,
		    snapshot.token_stale, snapshot.unclassified, snapshot.invalid);
		for (size_t index = 0; index < snapshot.causes.size(); index++)
		{
			const auto& cause = snapshot.causes[index];
			std::fprintf(stderr,
			             "[A64_DYNAREC_PHASE6AL_CAUSE] version=2 owner=%" PRIu64
			             " cause=%s events=%" PRIu64 " build_ns=%" PRIu64
			             " code_capacity=%" PRIu64
			             " fa_hit=%" PRIu64 " fa_saved_ns=%" PRIu64
			             " k16_hit=%" PRIu64 " k16_saved_ns=%" PRIu64
			             " k32_hit=%" PRIu64 " k32_saved_ns=%" PRIu64
			             " k32_only_hit=%" PRIu64
			             " k32_only_saved_ns=%" PRIu64
			             " k16_only_hit=%" PRIu64
			             " k16_only_saved_ns=%" PRIu64 "\n",
			             m_owner,
			             CauseName(static_cast<Cause>(index)), cause.events,
			             cause.build_nanoseconds, cause.code_capacity,
			             cause.fa512_hits, cause.fa512_saved_nanoseconds,
			             cause.k1024_16_hits,
			             cause.k1024_16_saved_nanoseconds,
			             cause.k1024_32_hits,
			             cause.k1024_32_saved_nanoseconds,
			             cause.k1024_32_only_hits,
			             cause.k1024_32_only_saved_nanoseconds,
			             cause.k1024_16_only_hits,
			             cause.k1024_16_only_saved_nanoseconds);
		}
		ReportModel(m_owner, "FA512", 512, 0, snapshot.fa512);
		ReportModel(m_owner, "K1024_16", 1024, kBudget16,
		            snapshot.k1024_16);
		ReportModel(m_owner, "K1024_32", 1024, kBudget32,
		            snapshot.k1024_32);
		std::fprintf(stderr,
		             "[A64_DYNAREC_PHASE6AL_END] version=2 owner=%" PRIu64
		             " complete=%u valid=%u report=%" PRIu64 "\n",
		             m_owner, 1u,
		             report_valid ? 1u : 0u, m_reports);
		std::fflush(stderr);
	}

	Snapshot GetSnapshot(bool final = false) const
	{
		Snapshot result {};
		result.actual = m_actual;
		result.fa512 = m_fa512.Stats();
		result.k1024_16 = m_k1024_16.Stats();
		result.k1024_32 = m_k1024_32.Stats();
		State().k1024_16.Snapshot(
		    &result.k1024_16.global_capacity,
		    &result.k1024_16.global_peak,
		    &result.k1024_16.global_invalid);
		State().k1024_32.Snapshot(
		    &result.k1024_32.global_capacity,
		    &result.k1024_32.global_peak,
		    &result.k1024_32.global_invalid);
		result.causes = m_causes;
		result.compile_ok = m_compile_ok;
		result.compile_fail = m_compile_fail;
		result.compile_nanoseconds = m_compile_nanoseconds;
		result.signature_changed_admission = m_signature_changed_admission;
		result.signature_changed_pre_restore =
		    m_signature_changed_pre_restore;
		result.signature_changed_post_compile =
		    m_signature_changed_post_compile;
		result.wasted_build_nanoseconds = m_wasted_build_nanoseconds;
		result.history_entries = m_history_entries;
		result.history_dropped = m_history_dropped;
		result.duplicate = m_duplicate;
		result.token_stale = m_token_stale;
		result.unclassified = m_unclassified;
		result.invalid = m_invalid;
		uint64_t cause_events = 0;
		uint64_t cause_nanoseconds = 0;
		uint64_t fa_saved_events = 0;
		uint64_t fa_saved_nanoseconds = 0;
		uint64_t k16_saved_events = 0;
		uint64_t k16_saved_nanoseconds = 0;
		uint64_t k32_saved_events = 0;
		uint64_t k32_saved_nanoseconds = 0;
		for (size_t cause_index = 0; cause_index < m_causes.size();
		     cause_index++)
		{
			const auto& cause = m_causes[cause_index];
			cause_events += cause.events;
			cause_nanoseconds += cause.build_nanoseconds;
			fa_saved_events += cause.fa512_hits;
			fa_saved_nanoseconds += cause.fa512_saved_nanoseconds;
			k16_saved_events += cause.k1024_16_hits;
			k16_saved_nanoseconds +=
			    cause.k1024_16_saved_nanoseconds;
			k32_saved_events += cause.k1024_32_hits;
			k32_saved_nanoseconds +=
			    cause.k1024_32_saved_nanoseconds;
			if (cause.fa512_hits > cause.events ||
			    cause.k1024_16_hits > cause.events ||
			    cause.k1024_32_hits > cause.events ||
			    (cause.fa512_hits == 0 &&
			     cause.fa512_saved_nanoseconds != 0) ||
			    (cause.k1024_16_hits == 0 &&
			     cause.k1024_16_saved_nanoseconds != 0) ||
			    (cause.k1024_32_hits == 0 &&
			     cause.k1024_32_saved_nanoseconds != 0) ||
			    cause.k1024_32_only_hits > cause.k1024_32_hits ||
			    cause.k1024_16_only_hits > cause.k1024_16_hits ||
			    (cause.k1024_32_only_hits == 0 &&
			     cause.k1024_32_only_saved_nanoseconds != 0) ||
			    (cause.k1024_16_only_hits == 0 &&
			     cause.k1024_16_only_saved_nanoseconds != 0) ||
			    cause.k1024_32_only_hits >
			        cause.events - cause.k1024_16_hits ||
			    cause.k1024_16_only_hits >
			        cause.events - cause.k1024_32_hits ||
			    cause.k1024_16_hits - cause.k1024_16_only_hits !=
			        cause.k1024_32_hits - cause.k1024_32_only_hits ||
			    cause.fa512_saved_nanoseconds > cause.build_nanoseconds ||
			    cause.k1024_16_saved_nanoseconds > cause.build_nanoseconds ||
			    cause.k1024_32_saved_nanoseconds > cause.build_nanoseconds ||
			    cause.k1024_32_only_saved_nanoseconds >
			        cause.k1024_32_saved_nanoseconds ||
			    cause.k1024_16_only_saved_nanoseconds >
			        cause.k1024_16_saved_nanoseconds ||
			    cause.k1024_16_saved_nanoseconds -
			            cause.k1024_16_only_saved_nanoseconds !=
			        cause.k1024_32_saved_nanoseconds -
			            cause.k1024_32_only_saved_nanoseconds ||
			    cause.k1024_32_only_saved_nanoseconds >
			        cause.build_nanoseconds -
			            cause.k1024_16_saved_nanoseconds ||
			    cause.k1024_16_only_saved_nanoseconds >
			        cause.build_nanoseconds -
			            cause.k1024_32_saved_nanoseconds)
			{
				result.invalid++;
			}
			const bool empty_payload =
			    cause.build_nanoseconds == 0 && cause.code_capacity == 0 &&
			    cause.fa512_hits == 0 &&
			    cause.fa512_saved_nanoseconds == 0 &&
			    cause.k1024_16_hits == 0 &&
			    cause.k1024_16_saved_nanoseconds == 0 &&
			    cause.k1024_32_hits == 0 &&
			    cause.k1024_32_saved_nanoseconds == 0 &&
			    cause.k1024_32_only_hits == 0 &&
			    cause.k1024_32_only_saved_nanoseconds == 0 &&
			    cause.k1024_16_only_hits == 0 &&
			    cause.k1024_16_only_saved_nanoseconds == 0;
			if ((cause.events == 0 && !empty_payload) ||
			    (cause.events != 0 && cause.code_capacity < cause.events))
			{
				result.invalid++;
			}
			const bool no_shadow_recovery =
			    cause.fa512_hits == 0 &&
			    cause.fa512_saved_nanoseconds == 0 &&
			    cause.k1024_16_hits == 0 &&
			    cause.k1024_16_saved_nanoseconds == 0 &&
			    cause.k1024_32_hits == 0 &&
			    cause.k1024_32_saved_nanoseconds == 0 &&
			    cause.k1024_32_only_hits == 0 &&
			    cause.k1024_32_only_saved_nanoseconds == 0 &&
			    cause.k1024_16_only_hits == 0 &&
			    cause.k1024_16_only_saved_nanoseconds == 0;
			if (((cause_index == CauseIndex(Cause::Cold) ||
			      cause_index == CauseIndex(Cause::Variant)) &&
			     !no_shadow_recovery) ||
			    (cause_index == CauseIndex(Cause::Conflict) &&
			     (cause.fa512_hits != cause.events ||
			      cause.fa512_saved_nanoseconds !=
			          cause.build_nanoseconds)) ||
			    (cause_index == CauseIndex(Cause::Capacity) &&
			     (cause.fa512_hits != 0 ||
			      cause.fa512_saved_nanoseconds != 0)))
			{
				result.invalid++;
			}
		}
		for (const auto& record: m_history)
		{
			if (!record.occupied || record.probe_count != 1 ||
			    record.compile_events != 1 ||
			    (record.first_cause != Cause::Cold &&
			     record.first_cause != Cause::Variant))
			{
				continue;
			}
			result.one_shot++;
			result.one_shot_nanoseconds += record.compile_nanoseconds;
		}
		const auto model_valid = [](const ModelStats& stats,
		                            size_t entry_limit,
		                            uint64_t budget,
		                            uint64_t compile_nanoseconds)
		{
			return stats.probes == stats.hits + stats.misses &&
			       stats.puts == stats.stored + stats.budget_drops &&
			       stats.drops <= stats.stored &&
			       stats.hits <= stats.stored - stats.drops &&
			       stats.saved_events <= stats.hits &&
			       (stats.saved_events != 0 ||
			        stats.saved_nanoseconds == 0) &&
			       stats.saved_nanoseconds <= compile_nanoseconds &&
			       stats.resident <= stats.resident_peak &&
			       stats.resident_peak <= stats.stored &&
			       stats.resident_peak <= entry_limit &&
			       (budget == 0
			            ? stats.budget_drops == 0 &&
			                  stats.capacity_resident == 0 &&
			                  stats.capacity_peak == 0 &&
			                  stats.global_capacity == 0 &&
			                  stats.global_peak == 0
			            : stats.capacity_resident >= stats.resident &&
			                  ((stats.resident == 0) ==
			                   (stats.capacity_resident == 0)) &&
			                  stats.capacity_resident <= stats.capacity_peak &&
			                  stats.resident_peak <= stats.capacity_peak &&
			                  stats.capacity_peak <= budget &&
			                  stats.capacity_resident <=
			                      stats.global_capacity &&
			                  stats.capacity_peak <= stats.global_peak &&
			                  stats.global_capacity <= stats.global_peak &&
			                  stats.global_peak <= budget) &&
			       stats.global_invalid == 0 &&
			       stats.invalid == 0;
		};
		result.valid =
		    m_interval != 0 &&
		    result.actual.probes == result.actual.hits + result.actual.misses &&
		    result.actual.puts ==
		        result.actual.stored + result.actual.budget_drops &&
		    result.actual.stored ==
		        result.actual.resident + result.actual.hits +
		            result.actual.set_drops + result.actual.resets &&
		    result.actual.resident <= result.actual.resident_peak &&
		    result.actual.resident_peak <= result.actual.stored &&
		    result.actual.resident_peak <= kFullyAssociativeEntries &&
		    result.actual.capacity_resident >= result.actual.resident &&
		    ((result.actual.resident == 0) ==
		     (result.actual.capacity_resident == 0)) &&
		    result.actual.capacity_resident <=
		        result.actual.capacity_peak &&
		    result.actual.resident_peak <= result.actual.capacity_peak &&
		    result.actual.capacity_peak <= kBudget16 &&
		    result.actual.capacity_resident <=
		        result.actual.global_capacity &&
		    result.actual.capacity_peak <= result.actual.global_peak &&
		    result.actual.global_capacity <= result.actual.global_peak &&
		    result.actual.global_peak <= kBudget16 &&
		    result.actual.misses == result.compile_ok &&
		    result.compile_fail == 0 &&
		    result.compile_ok == cause_events &&
		    result.compile_nanoseconds == cause_nanoseconds &&
		    result.history_entries ==
		        result.causes[CauseIndex(Cause::Cold)].events +
		            result.causes[CauseIndex(Cause::Variant)].events &&
		    result.one_shot <= result.history_entries &&
		    (result.one_shot != 0 ||
		     result.one_shot_nanoseconds == 0) &&
		    result.signature_changed_post_compile <= result.compile_ok &&
		    (result.signature_changed_post_compile != 0 ||
		     result.wasted_build_nanoseconds == 0) &&
		    result.wasted_build_nanoseconds <=
		        result.compile_nanoseconds &&
		    result.fa512.saved_events == fa_saved_events &&
		    result.fa512.saved_nanoseconds == fa_saved_nanoseconds &&
		    result.k1024_16.saved_events == k16_saved_events &&
		    result.k1024_16.saved_nanoseconds == k16_saved_nanoseconds &&
		    result.k1024_32.saved_events == k32_saved_events &&
		    result.k1024_32.saved_nanoseconds == k32_saved_nanoseconds &&
		    result.one_shot <=
		        result.causes[CauseIndex(Cause::Cold)].events +
		            result.causes[CauseIndex(Cause::Variant)].events &&
		    result.one_shot_nanoseconds <=
		        result.causes[CauseIndex(Cause::Cold)].build_nanoseconds +
		            result.causes[CauseIndex(Cause::Variant)].build_nanoseconds &&
		    model_valid(result.fa512, 512, 0,
		                result.compile_nanoseconds) &&
		    model_valid(result.k1024_16, 1024, kBudget16,
		                result.compile_nanoseconds) &&
		    model_valid(result.k1024_32, 1024, kBudget32,
		                result.compile_nanoseconds) &&
		    result.fa512.probes == result.actual.probes &&
		    result.k1024_16.probes == result.actual.probes &&
		    result.k1024_32.probes == result.actual.probes &&
		    result.fa512.puts == result.actual.puts &&
		    result.k1024_16.puts == result.actual.puts &&
		    result.k1024_32.puts == result.actual.puts &&
		    (final ||
		     (result.fa512.resident ==
		          result.fa512.stored - result.fa512.hits -
		              result.fa512.drops &&
		      result.k1024_16.resident ==
		          result.k1024_16.stored - result.k1024_16.hits -
		              result.k1024_16.drops &&
		      result.k1024_32.resident ==
		          result.k1024_32.stored - result.k1024_32.hits -
		              result.k1024_32.drops)) &&
		    result.history_entries <= kHistoryCapacity &&
		    result.history_dropped == 0 && result.duplicate == 0 &&
		    result.token_stale == 0 && result.unclassified == 0 &&
		    result.invalid == 0 && result.actual.invalid == 0 &&
		    result.actual.global_invalid == 0;
		return result;
	}

	static Cause Classify(const ProbeTicket& ticket)
	{
		if (!ticket.first_exact &&
		    ticket.previous_loss == LossKind::BudgetDrop)
			return Cause::Budget;
		if (!ticket.first_exact)
			return ticket.fa512_hit ? Cause::Conflict : Cause::Capacity;
		return ticket.rip_seen ? Cause::Variant : Cause::Cold;
	}

	void ClearModelsForTest()
	{
		m_fa512.Clear();
		m_k1024_16.Clear();
		m_k1024_32.Clear();
	}

	uint64_t ReportsForTest() const { return m_reports; }

private:
	void MarkProcessInvalid()
	{
		if (m_role == OwnerRole::Standalone) return;
		State().process_invalid.fetch_add(1, std::memory_order_relaxed);
	}

	void RecordInternalInvalid()
	{
		m_invalid++;
		MarkProcessInvalid();
	}

	void RecordActualInternalInvalid()
	{
		m_actual.invalid++;
		m_invalid++;
		MarkProcessInvalid();
	}

	void RecordUnclassified()
	{
		m_unclassified++;
		MarkProcessInvalid();
	}

	void RecordTokenStale()
	{
		m_token_stale++;
		MarkProcessInvalid();
	}

	void RecordHistoryDrop()
	{
		m_history_dropped++;
		MarkProcessInvalid();
	}

	void RefreshActualGlobal()
	{
		if (m_actual_global_snapshot == nullptr) return;
		uint64_t resident = 0;
		uint64_t peak = 0;
		uint64_t invalid = 0;
		m_actual_global_snapshot(m_actual_global_context, &resident, &peak,
		                         &invalid);
		RecordActualGlobal(resident, peak, invalid);
	}

	struct HistoryRecord
	{
		Identity identity {};
		uint64_t probe_count = 0;
		uint64_t compile_events = 0;
		uint64_t compile_nanoseconds = 0;
		uint32_t token = 0;
		LossKind last_loss = LossKind::None;
		Cause first_cause = Cause::Count;
		bool occupied = false;
		bool resident = false;
	};

	struct RipRecord
	{
		uint64_t rip = 0;
		bool occupied = false;
	};

	static size_t HistoryIndex(uint64_t hash)
	{
		return static_cast<size_t>(hash ^ (hash >> 32u)) &
		       (kHistoryCapacity - 1u);
	}

	static size_t RipIndex(uint64_t rip)
	{
		uint64_t value = rip;
		value ^= value >> 33u;
		value *= UINT64_C(0xff51afd7ed558ccd);
		value ^= value >> 33u;
		return static_cast<size_t>(value) & (kHistoryCapacity - 1u);
	}

	bool RipAlreadySeen(uint64_t rip)
	{
		size_t slot = RipIndex(rip);
		for (size_t probe = 0; probe < m_rips.size(); probe++)
		{
			auto& record = m_rips[slot];
			if (!record.occupied)
			{
				record = {rip, true};
				return false;
			}
			if (record.rip == rip) return true;
			slot = (slot + 1u) & (m_rips.size() - 1u);
		}
		RecordHistoryDrop();
		return false;
	}

	HistoryRecord* Existing(const Identity& identity)
	{
		if (!identity.Valid()) return nullptr;
		size_t slot = HistoryIndex(identity.hash);
		for (size_t probe = 0; probe < m_history.size(); probe++)
		{
			auto& record = m_history[slot];
			if (!record.occupied) return nullptr;
			if (record.identity.SameExact(identity)) return &record;
			slot = (slot + 1u) & (m_history.size() - 1u);
		}
		return nullptr;
	}

	HistoryRecord* Intern(const Identity& identity, bool target_probe,
	                      bool* first_exact, bool* rip_seen)
	{
		if (first_exact != nullptr) *first_exact = false;
		if (rip_seen != nullptr) *rip_seen = false;
		if (!identity.Valid())
		{
			RecordInternalInvalid();
			return nullptr;
		}
		size_t slot = HistoryIndex(identity.hash);
		for (size_t probe = 0; probe < m_history.size(); probe++)
		{
			auto& record = m_history[slot];
			if (!record.occupied)
			{
				const bool seen_rip = RipAlreadySeen(identity.rip);
				if (m_history_entries >= m_history.size())
				{
					RecordHistoryDrop();
					return nullptr;
				}
				record.identity = identity;
				record.token = static_cast<uint32_t>(slot + 1u);
				record.occupied = true;
				if (target_probe) record.probe_count = 1;
				m_history_entries++;
				if (first_exact != nullptr) *first_exact = true;
				if (rip_seen != nullptr) *rip_seen = seen_rip;
				return &record;
			}
			if (record.identity.SameExact(identity))
			{
				if (target_probe) record.probe_count++;
				if (rip_seen != nullptr) *rip_seen = true;
				return &record;
			}
			slot = (slot + 1u) & (m_history.size() - 1u);
		}
		RecordHistoryDrop();
		return nullptr;
	}

	HistoryRecord* RecordForToken(uint32_t token)
	{
		if (token == 0 || token > m_history.size()) return nullptr;
		auto& record = m_history[token - 1u];
		return record.occupied && record.token == token ? &record : nullptr;
	}

	void RecordActualLoss(const Identity& identity, LossKind loss)
	{
		HistoryRecord* record = Existing(identity);
		if (record == nullptr || !record->resident ||
		    m_actual.resident == 0 ||
		    m_actual.capacity_resident < identity.code_capacity)
		{
			RecordActualInternalInvalid();
			return;
		}
		record->resident = false;
		record->last_loss = loss;
		m_actual.resident--;
		m_actual.capacity_resident -= identity.code_capacity;
	}

	void MaybeReport()
	{
		if (m_finalized || m_actual.probes < m_next_report) return;
		Report(false);
		while (m_actual.probes >= m_next_report)
		{
			if (m_next_report > UINT64_MAX - m_interval)
			{
				m_next_report = UINT64_MAX;
				break;
			}
			m_next_report += m_interval;
		}
	}

	static void ReportModel(uint64_t owner, const char* name, size_t entries,
	                        uint64_t budget, const ModelStats& stats)
	{
		std::fprintf(
		    stderr,
		    "[A64_DYNAREC_PHASE6AL_MODEL] version=2 owner=%" PRIu64
		    " model=%s entries=%zu"
		    " ways=%zu budget=%" PRIu64 " probe=%" PRIu64
		    " hit=%" PRIu64 " miss=%" PRIu64 " put=%" PRIu64
		    " stored=%" PRIu64 " drop=%" PRIu64
		    " budget_drop=%" PRIu64 " resident=%" PRIu64
		    " peak=%" PRIu64 " capacity=%" PRIu64
		    " capacity_peak=%" PRIu64 " saved=%" PRIu64
		    " saved_ns=%" PRIu64 " invalid=%" PRIu64
		    " global_capacity=%" PRIu64 " global_peak=%" PRIu64
		    " global_invalid=%" PRIu64 "\n",
		    owner, name, entries,
		    std::strcmp(name, "FA512") == 0 ? entries : kArchiveWays,
		    budget, stats.probes, stats.hits, stats.misses, stats.puts,
		    stats.stored, stats.drops, stats.budget_drops, stats.resident,
		    stats.resident_peak, stats.capacity_resident,
		    stats.capacity_peak, stats.saved_events, stats.saved_nanoseconds,
		    stats.invalid, stats.global_capacity, stats.global_peak,
		    stats.global_invalid);
	}

	std::array<HistoryRecord, kHistoryCapacity> m_history {};
	std::array<RipRecord, kHistoryCapacity> m_rips {};
	FullyAssociative512 m_fa512 {};
	SetAssociativeArchive<kLargeArchiveEntries> m_k1024_16;
	SetAssociativeArchive<kLargeArchiveEntries> m_k1024_32;
	ActualStats m_actual {};
	std::array<CauseStats, CauseIndex(Cause::Count)> m_causes {};
	uint64_t m_compile_ok = 0;
	uint64_t m_compile_fail = 0;
	uint64_t m_compile_nanoseconds = 0;
	uint64_t m_signature_changed_admission = 0;
	uint64_t m_signature_changed_pre_restore = 0;
	uint64_t m_signature_changed_post_compile = 0;
	uint64_t m_wasted_build_nanoseconds = 0;
	uint64_t m_history_entries = 0;
	uint64_t m_history_dropped = 0;
	uint64_t m_duplicate = 0;
	uint64_t m_token_stale = 0;
	uint64_t m_unclassified = 0;
	uint64_t m_invalid = 0;
	uint64_t m_interval = 0;
	uint64_t m_next_report = 0;
	uint64_t m_reports = 0;
	uint64_t m_owner = 0;
	void* m_actual_global_context = nullptr;
	ActualGlobalSnapshot m_actual_global_snapshot = nullptr;
	OwnerRole m_role = OwnerRole::Standalone;
	bool m_finalized = false;
	bool m_final_reported = false;
};

inline Identity SyntheticIdentity(uint64_t rip, uint8_t byte, uint64_t hash,
	                                uint32_t code_capacity = 4096)
{
	Identity identity {};
	identity.hash = hash == 0 ? 1 : hash;
	identity.rip = rip;
	identity.byte_count = 1;
	identity.signature[0] = byte;
	identity.code_size = code_capacity == 0 ? 0 : 1;
	identity.code_capacity = code_capacity;
	return identity;
}

inline bool ValidateCoreOracle()
{
	// Pure classifier receipts: the five causes are mutually exclusive and
	// budget has precedence only for a previously observed exact identity.
	ProbeTicket cold {};
	cold.valid = true;
	cold.first_exact = true;
	ProbeTicket variant = cold;
	variant.rip_seen = true;
	ProbeTicket conflict = cold;
	conflict.first_exact = false;
	conflict.fa512_hit = true;
	ProbeTicket capacity = conflict;
	capacity.fa512_hit = false;
	ProbeTicket budget = conflict;
	budget.previous_loss = LossKind::BudgetDrop;
	if (Telemetry::Classify(cold) != Cause::Cold ||
	    Telemetry::Classify(variant) != Cause::Variant ||
	    Telemetry::Classify(conflict) != Cause::Conflict ||
	    Telemetry::Classify(capacity) != Cause::Capacity ||
	    Telemetry::Classify(budget) != Cause::Budget)
	{
		return false;
	}

	// Exact FA-LRU: a recent member is recoverable while the 513th insertion
	// evicts only the true tail.
	FullyAssociative512 fa;
	for (uint32_t token = 1; token <= 512; token++)
	{
		if (!fa.Put(token)) return false;
	}
	if (!fa.Take(512) || !fa.Put(512) || !fa.Put(513) || fa.Take(1) ||
	    !fa.Take(512))
	{
		return false;
	}
	GlobalBudget capacity_budget {kBudget16};
	SetAssociativeArchive<1024> capacity_model(&capacity_budget);
	FullyAssociative512 capacity_fa;
	for (uint32_t token = 1; token <= 513; token++)
	{
		if (!capacity_fa.Put(token) ||
		    !capacity_model.Put(token, token, 1, 4096))
		{
			return false;
		}
	}
	if (capacity_fa.Take(1) || !capacity_model.Take(1, 1)) return false;
	capacity_model.Clear();

	// The same 17 MiB artifact is rejected without an LRU mutation at 16 MiB
	// and retained by the otherwise identical 32 MiB geometry.
	GlobalBudget budget16 {kBudget16};
	GlobalBudget budget32 {kBudget32};
	SetAssociativeArchive<1024> k16(&budget16);
	SetAssociativeArchive<1024> k32(&budget32);
	const uint32_t large = 17u * 1024u * 1024u;
	if (k16.Put(0x100, 1, 1, large) ||
	    k16.Stats().resident != 0 ||
	    k16.Stats().budget_drops != 1 ||
	    !k32.Put(0x100, 1, 1, large) ||
	    k32.Stats().resident != 1 || !k32.Take(0x100, 1))
	{
		return false;
	}
	k16.Clear();
	k32.Clear();
	uint64_t resident16 = 1;
	uint64_t resident32 = 1;
	budget16.Snapshot(&resident16, nullptr, nullptr);
	budget32.Snapshot(&resident32, nullptr, nullptr);
	if (resident16 != 0 || resident32 != 0) return false;

	// Byte-exact A/B/A identity churn: A is archived while B is compiled, then
	// restored.  Only B is a one-shot compile and the third probe is a real
	// archive hit, not an impossible second miss of an active block.
	{
		auto telemetry = std::unique_ptr<Telemetry>(
		    new (std::nothrow) Telemetry(UINT64_MAX));
		if (telemetry == nullptr) return false;
		const Identity first = SyntheticIdentity(0x1000, 0x10, 0x101);
		// Deliberately collide the hash: byte-exact comparison, not the FNV
		// prefilter, must establish the same-RIP variant.
		const Identity second = SyntheticIdentity(0x1000, 0x20, 0x101);
		auto first_ticket = telemetry->BeginProbe(first, nullptr);
		telemetry->RecordActualResult(&first_ticket, false, false);
		telemetry->RecordCompileSuccess(first_ticket, 10, 1, 4096);
		telemetry->CompleteExchange(first_ticket);
		auto second_ticket = telemetry->BeginProbe(second, &first);
		telemetry->RecordActualResult(&second_ticket, false, false);
		telemetry->RecordCompileSuccess(second_ticket, 20, 1, 4096);
		telemetry->RecordActualPut();
		telemetry->RecordActualStore(first);
		telemetry->CompleteExchange(second_ticket);
		auto restored_ticket = telemetry->BeginProbe(first, &second);
		telemetry->RecordActualResult(&restored_ticket, true, false);
		telemetry->RecordActualTake(first);
		telemetry->RecordActualPut();
		telemetry->RecordActualStore(second);
		telemetry->CompleteExchange(restored_ticket);
		telemetry->RecordActualGlobal(4096, 4096, 0);
		const Snapshot snapshot = telemetry->GetSnapshot();
		if (!snapshot.valid || snapshot.compile_ok != 2 ||
		    snapshot.actual.hits != 1 || snapshot.actual.resident != 1 ||
		    snapshot.causes[CauseIndex(Cause::Cold)].events != 1 ||
		    snapshot.causes[CauseIndex(Cause::Variant)].events != 1 ||
		    snapshot.one_shot != 1 || snapshot.one_shot_nanoseconds != 20)
		{
			return false;
		}
	}

	// Five same-set archive residents force a real set loss while the ideal
	// FA512 model retains the exact identity.  Its reprobe must be conflict.
	{
		auto telemetry = std::unique_ptr<Telemetry>(
		    new (std::nothrow) Telemetry(UINT64_MAX));
		if (telemetry == nullptr) return false;
		std::array<Identity, 6> identities {};
		for (size_t index = 0; index < identities.size(); index++)
		{
			identities[index] = SyntheticIdentity(
			    0x2000 + index, static_cast<uint8_t>(0x30 + index),
			    static_cast<uint64_t>(index + 1u) << 8u, 1);
			auto ticket = telemetry->BeginProbe(
			    identities[index], index == 0 ? nullptr : &identities[index - 1u]);
			telemetry->RecordActualResult(&ticket, false, false);
			telemetry->RecordCompileSuccess(ticket, 1, 1, 1);
			if (index != 0)
			{
				telemetry->RecordActualPut();
				if (index == identities.size() - 1u)
					telemetry->RecordActualSetDrop(identities[0]);
				telemetry->RecordActualStore(identities[index - 1u]);
			}
			telemetry->CompleteExchange(ticket);
		}
		auto conflict_ticket =
		    telemetry->BeginProbe(identities[0], &identities.back());
		telemetry->RecordActualResult(&conflict_ticket, false, false);
		telemetry->RecordCompileSuccess(conflict_ticket, 7, 1, 1);
		telemetry->RecordActualPut();
		telemetry->RecordActualSetDrop(identities[1]);
		telemetry->RecordActualStore(identities.back());
		telemetry->CompleteExchange(conflict_ticket);
		telemetry->RecordActualGlobal(4, 4, 0);
		const Snapshot snapshot = telemetry->GetSnapshot();
		const auto& cause = snapshot.causes[CauseIndex(Cause::Conflict)];
		if (!snapshot.valid || cause.events != 1 || cause.fa512_hits != 1 ||
		    cause.k1024_16_hits != 0 || cause.k1024_32_hits != 0 ||
		    snapshot.actual.set_drops != 2 || snapshot.actual.resident != 4)
		{
			return false;
		}
	}

	// A 513-entry reuse misses exact FA512 but remains in the uniformly
	// distributed K1024 models.  The actual K512 receipt identifies capacity.
	{
		auto telemetry = std::unique_ptr<Telemetry>(
		    new (std::nothrow) Telemetry(UINT64_MAX));
		if (telemetry == nullptr) return false;
		const Identity first = SyntheticIdentity(0x3001, 2, 1, 1);
		const Identity second = SyntheticIdentity(0x3002, 3, 2, 1);
		Identity previous {};
		for (uint32_t index = 1; index <= 514; index++)
		{
			const Identity target = SyntheticIdentity(
			    0x3000 + index, static_cast<uint8_t>((index % 251u) + 1u),
			    index, 1);
			auto ticket = telemetry->BeginProbe(
			    target, index == 1 ? nullptr : &previous);
			telemetry->RecordActualResult(&ticket, false, false);
			telemetry->RecordCompileSuccess(ticket, 1, 1, 1);
			if (index != 1)
			{
				telemetry->RecordActualPut();
				if (index == 514) telemetry->RecordActualSetDrop(first);
				telemetry->RecordActualStore(previous);
			}
			telemetry->CompleteExchange(ticket);
			previous = target;
		}
		auto capacity_ticket = telemetry->BeginProbe(first, &previous);
		telemetry->RecordActualResult(&capacity_ticket, false, false);
		telemetry->RecordCompileSuccess(capacity_ticket, 9, 1, 1);
		telemetry->RecordActualPut();
		telemetry->RecordActualSetDrop(second);
		telemetry->RecordActualStore(previous);
		telemetry->CompleteExchange(capacity_ticket);
		telemetry->RecordActualGlobal(512, 512, 0);
		const Snapshot snapshot = telemetry->GetSnapshot();
		const auto& cause = snapshot.causes[CauseIndex(Cause::Capacity)];
		if (!snapshot.valid || cause.events != 1 || cause.fa512_hits != 0 ||
		    cause.k1024_16_hits != 1 || cause.k1024_32_hits != 1 ||
		    cause.k1024_32_only_hits != 0 ||
		    snapshot.actual.resident != 512 ||
		    snapshot.actual.resident_peak != 512)
		{
			return false;
		}
	}

	// An actual 16 MiB budget rejection is retained only by K1024@32.  The
	// joint receipt, rather than a subtraction of marginals, is the election
	// evidence for expanding the byte budget.
	{
		auto telemetry = std::unique_ptr<Telemetry>(
		    new (std::nothrow) Telemetry(UINT64_MAX));
		if (telemetry == nullptr) return false;
		const Identity large =
		    SyntheticIdentity(0x4000, 0x41, 0x401, 17u * 1024u * 1024u);
		const Identity small = SyntheticIdentity(0x4001, 0x42, 0x402, 1);
		auto large_ticket = telemetry->BeginProbe(large, nullptr);
		telemetry->RecordActualResult(&large_ticket, false, false);
		telemetry->RecordCompileSuccess(large_ticket, 100, 1,
		                                large.code_capacity);
		telemetry->CompleteExchange(large_ticket);
		auto small_ticket = telemetry->BeginProbe(small, &large);
		telemetry->RecordActualResult(&small_ticket, false, false);
		telemetry->RecordCompileSuccess(small_ticket, 200, 1, 1);
		telemetry->RecordActualPut();
		telemetry->RecordActualBudgetDrop(large);
		telemetry->CompleteExchange(small_ticket);
		auto budget_ticket = telemetry->BeginProbe(large, &small);
		telemetry->RecordActualResult(&budget_ticket, false, false);
		telemetry->RecordCompileSuccess(budget_ticket, 300, 1,
		                                large.code_capacity);
		telemetry->RecordActualPut();
		telemetry->RecordActualStore(small);
		telemetry->CompleteExchange(budget_ticket);
		telemetry->RecordActualGlobal(1, 1, 0);
		const Snapshot snapshot = telemetry->GetSnapshot();
		const auto& cause = snapshot.causes[CauseIndex(Cause::Budget)];
		if (!snapshot.valid || cause.events != 1 ||
		    cause.k1024_16_hits != 0 || cause.k1024_32_hits != 1 ||
		    cause.k1024_32_only_hits != 1 ||
		    cause.k1024_32_only_saved_nanoseconds != 300 ||
		    cause.k1024_16_only_hits != 0 ||
		    snapshot.actual.budget_drops != 1)
		{
			return false;
		}
	}

	// Extra K1024@32 admissions can also perturb same-set LRU in the opposite
	// direction.  Freeze that non-monotone case explicitly: X survives only in
	// K1024@16 after the 17 MiB identity is rejected there but admitted at 32.
	{
		auto telemetry = std::unique_ptr<Telemetry>(
		    new (std::nothrow) Telemetry(UINT64_MAX));
		if (telemetry == nullptr) return false;
		std::array<Identity, 7> identities {
		    SyntheticIdentity(0x5000, 0x50, 0x100, 1),
		    SyntheticIdentity(0x5001, 0x51, 0x200,
		                      17u * 1024u * 1024u),
		    SyntheticIdentity(0x5002, 0x52, 0x300, 1),
		    SyntheticIdentity(0x5003, 0x53, 0x400, 1),
		    SyntheticIdentity(0x5004, 0x54, 0x500, 1),
		    SyntheticIdentity(0x5005, 0x55, 0x080, 1),
		    SyntheticIdentity(0x5006, 0x56, 0x180, 1),
		};
		for (size_t index = 0; index < identities.size(); index++)
		{
			auto ticket = telemetry->BeginProbe(
			    identities[index], index == 0 ? nullptr : &identities[index - 1u]);
			telemetry->RecordActualResult(&ticket, false, false);
			telemetry->RecordCompileSuccess(
			    ticket, index + 1u, 1, identities[index].code_capacity);
			if (index != 0)
			{
				telemetry->RecordActualPut();
				if (index == 2)
					telemetry->RecordActualBudgetDrop(identities[1]);
				else
				{
					if (index == identities.size() - 1u)
						telemetry->RecordActualSetDrop(identities[0]);
					telemetry->RecordActualStore(identities[index - 1u]);
				}
			}
			telemetry->CompleteExchange(ticket);
		}
		auto reverse_ticket =
		    telemetry->BeginProbe(identities[0], &identities.back());
		telemetry->RecordActualResult(&reverse_ticket, false, false);
		telemetry->RecordCompileSuccess(reverse_ticket, 50, 1, 1);
		telemetry->RecordActualPut();
		telemetry->RecordActualSetDrop(identities[2]);
		telemetry->RecordActualStore(identities.back());
		telemetry->CompleteExchange(reverse_ticket);
		telemetry->RecordActualGlobal(4, 4, 0);
		const Snapshot snapshot = telemetry->GetSnapshot();
		const auto& cause = snapshot.causes[CauseIndex(Cause::Conflict)];
		if (!snapshot.valid || cause.events != 1 || cause.fa512_hits != 1 ||
		    cause.k1024_16_hits != 1 || cause.k1024_32_hits != 0 ||
		    cause.k1024_16_only_hits != 1 ||
		    cause.k1024_16_only_saved_nanoseconds != 50 ||
		    cause.k1024_32_only_hits != 0 ||
		    snapshot.actual.budget_drops != 1 ||
		    snapshot.actual.set_drops != 2)
		{
			return false;
		}
	}
	return true;
}

inline bool EmitReportOracle()
{
	auto telemetry = std::unique_ptr<Telemetry>(
	    new (std::nothrow) Telemetry(UINT64_MAX));
	if (telemetry == nullptr) return false;
	const Identity first = SyntheticIdentity(0x2000, 0x31, 0x201);
	const Identity second = SyntheticIdentity(0x2000, 0x32, 0x202);
	auto first_ticket = telemetry->BeginProbe(first, nullptr);
	telemetry->RecordActualResult(&first_ticket, false, false);
	telemetry->RecordCompileSuccess(first_ticket, 100, 1, 4096);
	auto second_ticket = telemetry->BeginProbe(second, nullptr);
	telemetry->RecordActualResult(&second_ticket, false, false);
	telemetry->RecordCompileSuccess(second_ticket, 200, 1, 4096);
	const Snapshot before = telemetry->GetSnapshot();
	telemetry->FinalizeAndReport();
	return before.valid && before.compile_ok == 2 && before.one_shot == 2;
}

inline bool ValidatePostCompileReportBoundaryOracle()
{
	auto telemetry = std::unique_ptr<Telemetry>(
	    new (std::nothrow) Telemetry(UINT64_MAX));
	if (telemetry == nullptr) return false;
	const Identity target = SyntheticIdentity(0x2100, 0x41, 0x211);
	auto ticket = telemetry->BeginProbe(target, nullptr);
	telemetry->RecordActualResult(&ticket, false, false);
	telemetry->RecordCompileSuccess(ticket, 100, 1, 4096);
	telemetry->CompleteExchange(ticket);
	if (telemetry->ReportsForTest() != 0) return false;
	telemetry->RecordSignatureChanged(
	    Telemetry::SignaturePoint::PostCompile, 100);
	const Snapshot before_finish = telemetry->GetSnapshot();
	if (!before_finish.valid || before_finish.signature_changed_post_compile != 1 ||
	    before_finish.wasted_build_nanoseconds != 100)
	{
		return false;
	}
	telemetry->FinishEvent();
	return telemetry->ReportsForTest() == 1;
}

inline bool ValidateHiddenWorkerInvalidOracle()
{
	auto& state = State();
	{
		std::lock_guard<std::mutex> lock(state.sequence_mutex);
		if (state.next_owner != 0 || state.primary_markers != 0 ||
		    state.primary_allocations != 0 || state.primary_owner != 0 ||
		    state.role_invalid != 0 ||
		    state.process_invalid.load(std::memory_order_relaxed) != 0)
		{
			return false;
		}
	}
	MarkPrimaryThread();
	auto primary = std::unique_ptr<Telemetry>(new (std::nothrow) Telemetry(
	    UINT64_MAX, nullptr, nullptr, OwnerRole::Primary));
	auto hidden_worker = std::unique_ptr<Telemetry>(new (std::nothrow) Telemetry(
	    UINT64_MAX, nullptr, nullptr, OwnerRole::Worker));
	if (primary == nullptr || hidden_worker == nullptr) return false;
	hidden_worker->RecordActualInvalid();
	// Model the cache-constructor path in which a requested worker cannot attach
	// a Ready production archive and is destroyed before its first report.
	MarkProcessActivationInvalid();
	std::lock_guard<std::mutex> lock(state.sequence_mutex);
	return state.next_owner == 2 && state.primary_markers == 1 &&
	       state.primary_allocations == 1 && state.primary_owner == 1 &&
	       state.role_invalid == 0 &&
	       state.process_invalid.load(std::memory_order_relaxed) == 2;
}

inline bool EmitPrimaryAfterWorkerOracle()
{
	auto& state = State();
	{
		std::lock_guard<std::mutex> lock(state.sequence_mutex);
		if (state.next_owner != 0 || state.primary_markers != 0 ||
		    state.primary_allocations != 0 || state.primary_owner != 0 ||
		    state.role_invalid != 0 ||
		    state.process_invalid.load(std::memory_order_relaxed) != 0)
		{
			return false;
		}
	}
	MarkPrimaryThread();
	auto worker = std::unique_ptr<Telemetry>(new (std::nothrow) Telemetry(
	    100000, nullptr, nullptr, OwnerRole::Worker));
	if (worker == nullptr) return false;
	{
		auto sequence = worker->LockSequence();
		worker->Report(false);
	}
	std::fprintf(stderr, "[FRAME_PIPELINE] present=120 synthetic=1\n");
	std::fflush(stderr);
	auto primary = std::unique_ptr<Telemetry>(new (std::nothrow) Telemetry(
	    100000, nullptr, nullptr, OwnerRole::Primary));
	if (primary == nullptr) return false;
	{
		auto sequence = primary->LockSequence();
		primary->Report(false);
	}
	std::lock_guard<std::mutex> lock(state.sequence_mutex);
	return state.next_owner == 2 && state.primary_markers == 1 &&
	       state.primary_allocations == 1 && state.primary_owner == 2 &&
	       state.role_invalid == 0 &&
	       state.process_invalid.load(std::memory_order_relaxed) == 0;
}

} // namespace Lumen::Cpu::Phase6AL
