#pragma once

#include "CpuA64Dynarec.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

// Diagnostic-only, observation-only ownership telemetry.  The table never
// returns an artifact or a policy decision to the dynarec.
namespace Lumen::Cpu::CrossOwnerTelemetry {

constexpr size_t kSignatureCapacity = 64;
constexpr size_t kRuntimeShards = 1024;
constexpr size_t kRuntimeSlotsPerShard = 64;
constexpr size_t kRuntimeActivePerIdentity = 8;
constexpr size_t kValidatedOwnersPerIdentity = 32;
constexpr uint64_t kReportInterval = 4096;
constexpr uint64_t kArchiveReportInterval = 1048576;

inline uint64_t NowNs()
{
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(
	        std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline void HashByte(uint64_t* hash, uint8_t value)
{
	*hash ^= value;
	*hash *= UINT64_C(1099511628211);
}

inline void HashU32(uint64_t* hash, uint32_t value)
{
	for (unsigned shift = 0; shift < 32; shift += 8)
		HashByte(hash, static_cast<uint8_t>(value >> shift));
}

inline void HashU64(uint64_t* hash, uint64_t value)
{
	for (unsigned shift = 0; shift < 64; shift += 8)
		HashByte(hash, static_cast<uint8_t>(value >> shift));
}

inline uint64_t CanonicalIrHash(const A64Dynarec::IrBlock& block)
{
	uint64_t hash = UINT64_C(1469598103934665603);
	HashU64(&hash, block.rip);
	HashU64(&hash, block.fallthrough_rip);
	HashU32(&hash, block.semantic_mask);
	HashByte(&hash, block.instruction_count);
	HashByte(&hash, block.has_terminator ? 1u : 0u);
	const size_t count = (std::min<size_t>)(
	    block.instruction_count, block.instructions.size());
	for (size_t index = 0; index < count; index++)
	{
		const auto& instruction = block.instructions[index];
		HashByte(&hash, static_cast<uint8_t>(instruction.opcode));
		HashByte(&hash, instruction.width);
		HashByte(&hash, instruction.destination);
		HashByte(&hash, instruction.source);
		HashByte(&hash, instruction.source_width);
		HashByte(&hash, instruction.source2);
		HashByte(&hash, instruction.condition);
		HashByte(&hash, instruction.vector_flags);
		HashByte(&hash, instruction.length);
		HashByte(&hash, instruction.step_cost);
		HashByte(&hash, instruction.effects);
		HashByte(&hash, instruction.address.base);
		HashByte(&hash, instruction.address.index);
		HashByte(&hash, instruction.address.scale);
		HashU32(&hash, static_cast<uint32_t>(instruction.address.displacement));
		HashU64(&hash, instruction.address.absolute);
		HashU64(&hash, instruction.rip);
		HashU64(&hash, instruction.immediate);
		HashU64(&hash, instruction.target);
	}
	return hash == 0 ? UINT64_C(1) : hash;
}

inline bool IrAddressesEqual(const A64Dynarec::IrAddress& left,
	                          const A64Dynarec::IrAddress& right)
{
	return left.base == right.base && left.index == right.index &&
	       left.scale == right.scale &&
	       left.displacement == right.displacement &&
	       left.absolute == right.absolute;
}

inline bool IrInstructionsEqual(const A64Dynarec::IrInstruction& left,
	                             const A64Dynarec::IrInstruction& right)
{
	return left.opcode == right.opcode && left.width == right.width &&
	       left.destination == right.destination && left.source == right.source &&
	       left.source_width == right.source_width && left.source2 == right.source2 &&
	       left.condition == right.condition &&
	       left.vector_flags == right.vector_flags && left.length == right.length &&
	       left.step_cost == right.step_cost && left.effects == right.effects &&
	       IrAddressesEqual(left.address, right.address) && left.rip == right.rip &&
	       left.immediate == right.immediate && left.target == right.target;
}

inline bool IrBlocksEqual(const A64Dynarec::IrBlock& left,
	                       const A64Dynarec::IrBlock& right)
{
	if (left.rip != right.rip || left.fallthrough_rip != right.fallthrough_rip ||
	    left.semantic_mask != right.semantic_mask ||
	    left.instruction_count != right.instruction_count ||
	    left.has_terminator != right.has_terminator ||
	    left.instruction_count > left.instructions.size())
	{
		return false;
	}
	for (size_t index = 0; index < left.instruction_count; index++)
	{
		if (!IrInstructionsEqual(left.instructions[index], right.instructions[index]))
			return false;
	}
	return true;
}

inline uint64_t GuestIdentityHash(uint64_t rip, const uint8_t* bytes,
	                              uint8_t byte_count)
{
	uint64_t hash = UINT64_C(1469598103934665603);
	HashU64(&hash, rip);
	HashByte(&hash, byte_count);
	for (uint8_t index = 0; index < byte_count; index++) HashByte(&hash, bytes[index]);
	return hash == 0 ? UINT64_C(1) : hash;
}

struct Ticket
{
	uint16_t shard = 0;
	uint8_t slot = 0;
	uint8_t active = 0;
	uint64_t event_id = 0;
	uint64_t owner = 0;
	bool valid = false;
	bool finished = false;
};

struct Stats
{
	uint64_t report = 0;
	uint64_t complete_events = 0;
	uint64_t archive_probe = 0;
	uint64_t archive_hit = 0;
	uint64_t archive_miss = 0;
	uint64_t archive_invalid = 0;
	uint64_t compile_attempt = 0;
	uint64_t compile_ok = 0;
	uint64_t compile_fail = 0;
	uint64_t compiling = 0;
	uint64_t pending_validation = 0;
	uint64_t untracked_attempt = 0;
	uint64_t validated = 0;
	uint64_t stale = 0;
	uint64_t abandoned_after_ok = 0;
	uint64_t abandoned_before_finish = 0;
	uint64_t identity_new = 0;
	uint64_t same_owner_repeat = 0;
	uint64_t cross_owner_sequential = 0;
	uint64_t cross_owner_overlap = 0;
	uint64_t build_ns = 0;
	uint64_t identity_new_ns = 0;
	uint64_t same_owner_repeat_ns = 0;
	uint64_t cross_owner_sequential_ns = 0;
	uint64_t cross_owner_overlap_ns = 0;
	uint64_t code_bytes = 0;
	uint64_t resident = 0;
	uint64_t owner_count = 0;
	uint64_t owner_shape_failure = 0;
	uint64_t shard_full_drop = 0;
	uint64_t active_slot_overflow = 0;
	uint64_t allocation_failure = 0;
	uint64_t owner_overflow = 0;
	uint64_t ticket_invalid = 0;
	uint64_t ir_variant = 0;
	uint64_t hash_collision = 0;
	uint64_t lock_wait_ns = 0;
};

template <size_t ShardCount, size_t SlotsPerShard, size_t ActivePerIdentity>
class Table final
{
public:
	static_assert(ShardCount != 0 && ShardCount <= UINT16_MAX + size_t {1});
	static_assert(SlotsPerShard != 0 && SlotsPerShard <= UINT8_MAX + size_t {1});
	static_assert(ActivePerIdentity != 0 && ActivePerIdentity <= UINT8_MAX + size_t {1});

	explicit Table(bool emit_reports = false): m_emit_reports(emit_reports) {}

	Ticket Begin(uint64_t owner, uint64_t rip, const uint8_t* bytes,
	             uint8_t byte_count, const A64Dynarec::IrBlock& ir,
	             bool verified_path, A64Dynarec::EligibilityPolicy policy,
	             uint64_t forced_guest_hash = 0, uint64_t forced_ir_hash = 0)
	{
		Ticket ticket {};
		if (owner == 0 || bytes == nullptr || byte_count == 0 ||
		    byte_count > kSignatureCapacity ||
		    ir.instruction_count > ir.instructions.size())
		{
			{
				std::lock_guard<std::mutex> stats_lock(m_stats_mutex);
				m_stats.compile_attempt++;
				m_stats.untracked_attempt++;
				m_stats.ticket_invalid++;
				if (owner == 0) m_stats.owner_overflow++;
			}
			ReportCoverageFailureOnce();
			return ticket;
		}

		const uint64_t guest_hash = forced_guest_hash != 0
		                                ? forced_guest_hash
		                                : GuestIdentityHash(rip, bytes, byte_count);
		uint64_t canonical_ir_hash = CanonicalIrHash(ir);
		HashByte(&canonical_ir_hash, verified_path ? 1u : 0u);
		HashByte(&canonical_ir_hash, static_cast<uint8_t>(policy));
		const uint64_t ir_hash = forced_ir_hash != 0
		                             ? forced_ir_hash
		                             : canonical_ir_hash;
		const size_t shard_index = static_cast<size_t>(
		    (guest_hash ^ (guest_hash >> 32u)) % ShardCount);
		auto& shard = m_shards[shard_index];
		const uint64_t wait_begin = NowNs();
		std::unique_lock<std::mutex> shard_lock(shard.mutex);
		const uint64_t lock_wait = NowNs() - wait_begin;

		size_t selected = SlotsPerShard;
		size_t empty = SlotsPerShard;
		uint64_t variants = 0;
		uint64_t collisions = 0;
		for (size_t slot = 0; slot < SlotsPerShard; slot++)
		{
			if (!shard.index[slot].occupied)
			{
				if (empty == SlotsPerShard) empty = slot;
				continue;
			}
			if (shard.index[slot].guest_hash != guest_hash) continue;
			auto* record = shard.records[slot].get();
			if (record == nullptr)
			{
				collisions++;
				continue;
			}
			const bool guest_exact = record->rip == rip &&
			                         record->byte_count == byte_count &&
			                         std::memcmp(record->signature.data(), bytes,
			                                     byte_count) == 0;
			if (!guest_exact)
			{
				collisions++;
				continue;
			}
			if (shard.index[slot].ir_hash != ir_hash)
			{
				variants++;
				continue;
			}
			const bool ir_exact = record->verified_path == verified_path &&
			                      record->policy == policy &&
			                      IrBlocksEqual(record->ir, ir);
			if (!ir_exact)
			{
				variants++;
				collisions++;
				continue;
			}
			selected = slot;
			break;
		}

		bool new_record = false;
		bool full_drop = false;
		bool allocation_failure = false;
		if (selected == SlotsPerShard)
		{
			if (empty == SlotsPerShard)
			{
				full_drop = true;
			} else
			{
				auto record = std::unique_ptr<Record>(new (std::nothrow) Record);
				if (record == nullptr)
				{
					allocation_failure = true;
				} else
				{
					record->rip = rip;
					record->byte_count = byte_count;
					std::memcpy(record->signature.data(), bytes, byte_count);
					record->ir = ir;
					record->verified_path = verified_path;
					record->policy = policy;
					selected = empty;
					shard.records[selected] = std::move(record);
					shard.index[selected] = {guest_hash, ir_hash, true};
					new_record = true;
				}
			}
		}

		bool active_overflow = false;
		if (selected != SlotsPerShard)
		{
			auto& record = *shard.records[selected];
			size_t active_index = ActivePerIdentity;
			for (size_t index = 0; index < ActivePerIdentity; index++)
			{
				if (record.active[index].stage == Stage::Empty)
				{
					active_index = index;
					break;
				}
			}
			if (active_index == ActivePerIdentity)
			{
				active_overflow = true;
			} else
			{
				const uint64_t event_id = ++record.last_event_id;
				auto& event = record.active[active_index];
				event.event_id = event_id;
				event.owner = owner;
				event.begin_ns = NowNs();
				event.prior_validated_foreign = false;
				for (size_t index = 0; index < record.validated_owner_count; index++)
				{
					if (record.validated_owners[index] != owner)
					{
						event.prior_validated_foreign = true;
						break;
					}
				}
				for (auto& other: record.active)
				{
					if (other.stage != Stage::Empty && other.owner != owner)
					{
						event.foreign_overlap = true;
						other.foreign_overlap = true;
					}
				}
				event.stage = Stage::Compiling;
				ticket.shard = static_cast<uint16_t>(shard_index);
				ticket.slot = static_cast<uint8_t>(selected);
				ticket.active = static_cast<uint8_t>(active_index);
				ticket.event_id = event_id;
				ticket.owner = owner;
				ticket.valid = true;
			}
		}
		shard_lock.unlock();

		{
			std::lock_guard<std::mutex> stats_lock(m_stats_mutex);
			m_stats.compile_attempt++;
			m_stats.lock_wait_ns += lock_wait;
			m_stats.ir_variant += variants;
			m_stats.hash_collision += collisions;
			if (new_record) m_stats.resident++;
			if (ticket.valid)
				m_stats.compiling++;
			else
			{
				m_stats.untracked_attempt++;
				if (full_drop) m_stats.shard_full_drop++;
				if (active_overflow) m_stats.active_slot_overflow++;
				if (allocation_failure) m_stats.allocation_failure++;
			}
		}
		if (!ticket.valid) ReportCoverageFailureOnce();
		return ticket;
	}

	void Finish(Ticket* ticket, bool success, size_t code_size,
	            size_t code_capacity, uint64_t build_ns)
	{
		if (ticket == nullptr || !ticket->valid) return;
		Event completed {};
		bool valid = false;
		const uint64_t wait_begin = NowNs();
		uint64_t lock_wait = 0;
		auto& shard = m_shards[ticket->shard];
		{
			std::unique_lock<std::mutex> shard_lock(shard.mutex);
			lock_wait = NowNs() - wait_begin;
			auto* event = FindEventLocked(shard, *ticket);
			if (event != nullptr && event->stage == Stage::Compiling)
			{
				event->build_ns = build_ns;
				event->code_size = code_size;
				event->code_capacity = code_capacity;
				completed = *event;
				if (success)
					event->stage = Stage::PendingValidation;
				else
					*event = {};
				valid = true;
			}
		}
		if (!valid)
		{
			InvalidateTicket(ticket, lock_wait);
			return;
		}
		ticket->finished = success;
		{
			std::lock_guard<std::mutex> stats_lock(m_stats_mutex);
			m_stats.lock_wait_ns += lock_wait;
			if (m_stats.compiling != 0) m_stats.compiling--;
			else m_stats.ticket_invalid++;
			if (success)
			{
				m_stats.compile_ok++;
				m_stats.pending_validation++;
				m_stats.build_ns += completed.build_ns;
				m_stats.code_bytes += completed.code_size;
			} else
			{
				m_stats.compile_fail++;
				m_stats.complete_events++;
				ticket->valid = false;
			}
		}
		if (!success) MaybeReport();
	}

	void Validate(Ticket* ticket, bool signature_current)
	{
		if (ticket == nullptr || !ticket->valid || !ticket->finished) return;
		Event completed {};
		Class classification = Class::None;
		bool valid = false;
		bool owner_overflow = false;
		const uint64_t wait_begin = NowNs();
		uint64_t lock_wait = 0;
		auto& shard = m_shards[ticket->shard];
		{
			std::unique_lock<std::mutex> shard_lock(shard.mutex);
			lock_wait = NowNs() - wait_begin;
			auto* record = FindRecordLocked(shard, *ticket);
			auto* event = record != nullptr ? FindEventLocked(shard, *ticket) : nullptr;
			if (record != nullptr && event != nullptr &&
			    event->stage == Stage::PendingValidation)
			{
				completed = *event;
				if (signature_current)
				{
					bool owner_seen = false;
					for (size_t index = 0;
					     index < record->validated_owner_count; index++)
					{
						owner_seen = owner_seen ||
						             record->validated_owners[index] == event->owner;
					}
					if (record->validated_successes == 0)
					{
						classification = Class::IdentityNew;
					} else if (owner_seen)
					{
						classification = Class::SameOwner;
					} else if (!event->prior_validated_foreign &&
					           event->foreign_overlap)
					{
						classification = Class::CrossOverlap;
					} else
					{
						classification = Class::CrossSequential;
					}
					record->validated_successes++;
					if (!owner_seen)
					{
						if (record->validated_owner_count <
						    record->validated_owners.size())
						{
							record->validated_owners[
							    record->validated_owner_count++] = event->owner;
						} else
						{
							owner_overflow = true;
						}
					}
				}
				*event = {};
				valid = true;
			}
		}
		if (!valid)
		{
			InvalidateTicket(ticket, lock_wait);
			return;
		}
		ticket->valid = false;
		{
			std::lock_guard<std::mutex> stats_lock(m_stats_mutex);
			m_stats.lock_wait_ns += lock_wait;
			if (owner_overflow) m_stats.owner_overflow++;
			if (m_stats.pending_validation != 0) m_stats.pending_validation--;
			else m_stats.ticket_invalid++;
			if (!signature_current)
			{
				m_stats.stale++;
			} else
			{
				m_stats.validated++;
				RecordClassLocked(classification, completed.build_ns);
			}
			m_stats.complete_events++;
		}
		if (owner_overflow) ReportCoverageFailureOnce();
		else MaybeReport();
	}

	void Abandon(Ticket* ticket)
	{
		if (ticket == nullptr || !ticket->valid) return;
		Stage stage = Stage::Empty;
		const uint64_t wait_begin = NowNs();
		uint64_t lock_wait = 0;
		auto& shard = m_shards[ticket->shard];
		{
			std::unique_lock<std::mutex> shard_lock(shard.mutex);
			lock_wait = NowNs() - wait_begin;
			auto* event = FindEventLocked(shard, *ticket);
			if (event != nullptr)
			{
				stage = event->stage;
				*event = {};
			}
		}
		ticket->valid = false;
		{
			std::lock_guard<std::mutex> stats_lock(m_stats_mutex);
			m_stats.lock_wait_ns += lock_wait;
			if (stage == Stage::PendingValidation)
			{
				if (m_stats.pending_validation != 0) m_stats.pending_validation--;
				m_stats.abandoned_after_ok++;
			} else if (stage == Stage::Compiling)
			{
				if (m_stats.compiling != 0) m_stats.compiling--;
				m_stats.abandoned_before_finish++;
			} else
			{
				m_stats.ticket_invalid++;
			}
			m_stats.complete_events++;
		}
		if (stage != Stage::PendingValidation) ReportCoverageFailureOnce();
		else MaybeReport();
	}

	void RecordOwnerShape(bool production_shape)
	{
		{
			std::lock_guard<std::mutex> stats_lock(m_stats_mutex);
			m_stats.owner_count++;
			if (!production_shape) m_stats.owner_shape_failure++;
		}
		if (!production_shape) ReportCoverageFailureOnce();
	}

	void RecordArchive(bool hit, bool invalid)
	{
		bool report_due = false;
		{
			std::lock_guard<std::mutex> stats_lock(m_stats_mutex);
			m_stats.archive_probe++;
			if (hit) m_stats.archive_hit++;
			else m_stats.archive_miss++;
			if (invalid) m_stats.archive_invalid++;
			report_due = invalid || m_stats.archive_probe >= m_next_archive_report;
			while (m_next_archive_report <= m_stats.archive_probe &&
			       m_next_archive_report <= UINT64_MAX - kArchiveReportInterval)
				m_next_archive_report += kArchiveReportInterval;
		}
		if (invalid) ReportCoverageFailureOnce();
		else if (report_due) MaybeReport(true);
	}

	Stats Snapshot() const
	{
		std::lock_guard<std::mutex> stats_lock(m_stats_mutex);
		return m_stats;
	}

	void EmitCurrent(bool final = false)
	{
		std::lock_guard<std::mutex> report_lock(m_report_mutex);
		Stats snapshot {};
		{
			std::lock_guard<std::mutex> stats_lock(m_stats_mutex);
			m_stats.report++;
			snapshot = m_stats;
		}
		EmitSnapshot(snapshot, final);
	}

private:
	enum class Stage : uint8_t { Empty, Compiling, PendingValidation };
	enum class Class : uint8_t
	{
		None,
		IdentityNew,
		SameOwner,
		CrossSequential,
		CrossOverlap,
	};

	struct Event
	{
		uint64_t event_id = 0;
		uint64_t owner = 0;
		uint64_t begin_ns = 0;
		uint64_t build_ns = 0;
		size_t code_size = 0;
		size_t code_capacity = 0;
		bool prior_validated_foreign = false;
		bool foreign_overlap = false;
		Stage stage = Stage::Empty;
	};

	struct Record
	{
		uint64_t rip = 0;
		std::array<uint8_t, kSignatureCapacity> signature {};
		uint8_t byte_count = 0;
		A64Dynarec::IrBlock ir {};
		bool verified_path = false;
		A64Dynarec::EligibilityPolicy policy = A64Dynarec::EligibilityPolicy::Existing;
		uint64_t validated_successes = 0;
		uint64_t last_event_id = 0;
		std::array<uint64_t, kValidatedOwnersPerIdentity> validated_owners {};
		size_t validated_owner_count = 0;
		std::array<Event, ActivePerIdentity> active {};
	};

	struct Index
	{
		uint64_t guest_hash = 0;
		uint64_t ir_hash = 0;
		bool occupied = false;
	};

	struct Shard
	{
		std::mutex mutex;
		std::array<Index, SlotsPerShard> index {};
		std::array<std::unique_ptr<Record>, SlotsPerShard> records {};
	};

	Record* FindRecordLocked(Shard& shard, const Ticket& ticket)
	{
		if (ticket.slot >= SlotsPerShard) return nullptr;
		return shard.records[ticket.slot].get();
	}

	Event* FindEventLocked(Shard& shard, const Ticket& ticket)
	{
		auto* record = FindRecordLocked(shard, ticket);
		if (record == nullptr || ticket.active >= ActivePerIdentity) return nullptr;
		auto& event = record->active[ticket.active];
		return event.event_id == ticket.event_id && event.owner == ticket.owner
		           ? &event
		           : nullptr;
	}

	void InvalidateTicket(Ticket* ticket, uint64_t lock_wait)
	{
		ticket->valid = false;
		{
			std::lock_guard<std::mutex> stats_lock(m_stats_mutex);
			m_stats.lock_wait_ns += lock_wait;
			m_stats.ticket_invalid++;
		}
		ReportCoverageFailureOnce();
	}

	void RecordClassLocked(Class classification, uint64_t build_ns)
	{
		switch (classification)
		{
			case Class::IdentityNew:
				m_stats.identity_new++;
				m_stats.identity_new_ns += build_ns;
				break;
			case Class::SameOwner:
				m_stats.same_owner_repeat++;
				m_stats.same_owner_repeat_ns += build_ns;
				break;
			case Class::CrossSequential:
				m_stats.cross_owner_sequential++;
				m_stats.cross_owner_sequential_ns += build_ns;
				break;
			case Class::CrossOverlap:
				m_stats.cross_owner_overlap++;
				m_stats.cross_owner_overlap_ns += build_ns;
				break;
			case Class::None: m_stats.ticket_invalid++; break;
		}
	}

	static bool CoverageComplete(const Stats& stats)
	{
		return stats.shard_full_drop == 0 &&
		       stats.active_slot_overflow == 0 &&
		       stats.allocation_failure == 0 &&
		       stats.owner_shape_failure == 0 &&
		       stats.owner_overflow == 0 && stats.ticket_invalid == 0 &&
		       stats.abandoned_before_finish == 0 &&
		       stats.archive_invalid == 0;
	}

	void MaybeReport(bool force = false)
	{
		if (!m_emit_reports) return;
		std::lock_guard<std::mutex> report_lock(m_report_mutex);
		Stats snapshot {};
		{
			std::lock_guard<std::mutex> stats_lock(m_stats_mutex);
			const bool first_due = !m_first_complete_reported &&
			                       m_stats.complete_events >= 1;
			if (!force && !first_due &&
			    m_stats.complete_events < m_next_report)
				return;
			if (m_stats.complete_events >= 1) m_first_complete_reported = true;
			while (m_next_report <= m_stats.complete_events &&
			       m_next_report <= UINT64_MAX - kReportInterval)
				m_next_report += kReportInterval;
			m_stats.report++;
			snapshot = m_stats;
		}
		EmitSnapshot(snapshot, false);
	}

	void ReportCoverageFailureOnce()
	{
		if (!m_emit_reports) return;
		bool expected = false;
		if (m_coverage_failure_reported.compare_exchange_strong(
		        expected, true, std::memory_order_acq_rel,
		        std::memory_order_relaxed))
			MaybeReport(true);
	}

	static void EmitSnapshot(const Stats& stats, bool final)
	{
		flockfile(stderr);
		std::fprintf(stderr,
		    "[A64_CROSS_OWNER] report=%" PRIu64 " final=%u complete=%" PRIu64
		    " compile_attempt=%" PRIu64 " compile_ok=%" PRIu64
		    " compile_fail=%" PRIu64 " compiling=%" PRIu64
		    " pending_validation=%" PRIu64 " untracked_attempt=%" PRIu64
		    " validated=%" PRIu64 " stale=%" PRIu64
		    " abandoned_after_ok=%" PRIu64 " abandoned_before_finish=%" PRIu64
		    " build_ns=%" PRIu64 " code_bytes=%" PRIu64 "\n",
		    stats.report, final ? 1u : 0u, stats.complete_events,
		    stats.compile_attempt, stats.compile_ok, stats.compile_fail,
		    stats.compiling, stats.pending_validation,
		    stats.untracked_attempt, stats.validated, stats.stale,
		    stats.abandoned_after_ok, stats.abandoned_before_finish,
		    stats.build_ns, stats.code_bytes);
		std::fprintf(stderr,
		    "[A64_CROSS_OWNER_ARCHIVE] report=%" PRIu64
		    " probe=%" PRIu64 " hit=%" PRIu64 " miss=%" PRIu64
		    " invalid=%" PRIu64 "\n",
		    stats.report, stats.archive_probe, stats.archive_hit,
		    stats.archive_miss, stats.archive_invalid);
		std::fprintf(stderr,
		    "[A64_CROSS_OWNER_CLASS] report=%" PRIu64
		    " identity_new=%" PRIu64 " same_owner_repeat=%" PRIu64
		    " cross_owner_sequential=%" PRIu64
		    " cross_owner_overlap=%" PRIu64
		    " identity_new_ns=%" PRIu64 " same_owner_repeat_ns=%" PRIu64
		    " cross_owner_sequential_ns=%" PRIu64
		    " cross_owner_overlap_ns=%" PRIu64 "\n",
		    stats.report, stats.identity_new, stats.same_owner_repeat,
		    stats.cross_owner_sequential, stats.cross_owner_overlap,
		    stats.identity_new_ns, stats.same_owner_repeat_ns,
		    stats.cross_owner_sequential_ns, stats.cross_owner_overlap_ns);
		std::fprintf(stderr,
		    "[A64_CROSS_OWNER_COVERAGE] report=%" PRIu64
		    " complete=%u resident=%" PRIu64 " owner_count=%" PRIu64
		    " owner_shape_failure=%" PRIu64
		    " shard_full_drop=%" PRIu64 " active_slot_overflow=%" PRIu64
		    " allocation_failure=%" PRIu64 " owner_overflow=%" PRIu64
		    " ticket_invalid=%" PRIu64 " ir_variant=%" PRIu64
		    " hash_collision=%" PRIu64 " lock_wait_ns=%" PRIu64 "\n",
		    stats.report, CoverageComplete(stats) ? 1u : 0u,
		    stats.resident, stats.owner_count, stats.owner_shape_failure,
		    stats.shard_full_drop,
		    stats.active_slot_overflow, stats.allocation_failure,
		    stats.owner_overflow, stats.ticket_invalid, stats.ir_variant,
		    stats.hash_collision, stats.lock_wait_ns);
		std::fflush(stderr);
		funlockfile(stderr);
	}

	std::array<Shard, ShardCount> m_shards {};
	mutable std::mutex m_stats_mutex;
	std::mutex m_report_mutex;
	Stats m_stats {};
	uint64_t m_next_report = kReportInterval;
	uint64_t m_next_archive_report = kArchiveReportInterval;
	bool m_first_complete_reported = false;
	std::atomic<bool> m_coverage_failure_reported {false};
	bool m_emit_reports = false;
};

using RuntimeTable =
    Table<kRuntimeShards, kRuntimeSlotsPerShard, kRuntimeActivePerIdentity>;

inline bool EnvironmentRequested()
{
	static const bool requested = []
	{
		const char* value = std::getenv(
		    "LUMEN_X64_A64_DYNAREC_CROSS_OWNER_TELEMETRY");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return requested;
}

inline RuntimeTable* Runtime()
{
	static RuntimeTable* table = EnvironmentRequested()
	                                 ? new (std::nothrow) RuntimeTable(true)
	                                 : nullptr;
	return table;
}

inline std::atomic<bool>& RuntimeEnabledLatch()
{
	static std::atomic<bool> enabled {false};
	return enabled;
}

inline uint64_t AllocateOwnerToken()
{
	static std::atomic<uint64_t> next {1};
	const uint64_t token = next.fetch_add(1, std::memory_order_relaxed);
	return token == 0 ? 0 : token;
}

inline void Configure(bool dynarec_enabled, bool archive_enabled,
	                  size_t archive_entries, bool recycler_enabled,
	                  bool verified_ir, bool lazy_ir, bool delayed_admission,
	                  bool separate_negative)
{
	static std::once_flag once;
	std::call_once(once, [&]
	{
		const bool requested = EnvironmentRequested();
		auto* table = requested ? Runtime() : nullptr;
		const bool production_shape = dynarec_enabled && archive_enabled &&
		                              archive_entries == 1024 && recycler_enabled &&
		                              verified_ir && lazy_ir && delayed_admission &&
		                              separate_negative;
		const bool enabled = requested && table != nullptr && production_shape;
		RuntimeEnabledLatch().store(enabled, std::memory_order_release);
		std::fprintf(stderr,
		    "[A64_CROSS_OWNER] configuration version=1 requested=%u enabled=%u "
		    "shards=%zu slots_per_shard=%zu active_per_identity=%zu "
		    "validated_owner_capacity=%zu "
		    "archive=%u archive_entries=%zu recycler=%u verified_ir=%u lazy_ir=%u "
		    "delayed_admission=%u separate_negative=%u\n",
		    requested ? 1u : 0u, enabled ? 1u : 0u, kRuntimeShards,
		    kRuntimeSlotsPerShard, kRuntimeActivePerIdentity,
		    kValidatedOwnersPerIdentity,
		    archive_enabled ? 1u : 0u, archive_entries,
		    recycler_enabled ? 1u : 0u, verified_ir ? 1u : 0u,
		    lazy_ir ? 1u : 0u, delayed_admission ? 1u : 0u,
		    separate_negative ? 1u : 0u);
		std::fflush(stderr);
	});
}

inline bool Enabled()
{
	return RuntimeEnabledLatch().load(std::memory_order_acquire);
}

class Guard final
{
public:
	Guard() = default;

	void Begin(uint64_t owner, uint64_t rip, const uint8_t* bytes,
	           uint8_t byte_count, const A64Dynarec::IrBlock& ir,
	           bool verified_path, A64Dynarec::EligibilityPolicy policy,
	           uint64_t guest_hash)
	{
		if (!Enabled()) return;
		auto* table = Runtime();
		if (table != nullptr)
			m_ticket = table->Begin(owner, rip, bytes, byte_count, ir,
			                        verified_path, policy, guest_hash);
	}

	void Finish(bool success, size_t code_size, size_t code_capacity,
	            uint64_t build_ns)
	{
		if (auto* table = Runtime(); table != nullptr)
			table->Finish(&m_ticket, success, code_size, code_capacity, build_ns);
	}

	void Validate(bool signature_current)
	{
		if (auto* table = Runtime(); table != nullptr)
			table->Validate(&m_ticket, signature_current);
	}

	~Guard()
	{
		if (auto* table = Runtime(); table != nullptr)
			table->Abandon(&m_ticket);
	}

	Guard(const Guard&) = delete;
	Guard& operator=(const Guard&) = delete;

private:
	Ticket m_ticket {};
};

inline void RecordArchive(bool hit, bool invalid)
{
	if (!Enabled()) return;
	if (auto* table = Runtime(); table != nullptr) table->RecordArchive(hit, invalid);
}

inline void RecordOwnerShape(bool production_shape)
{
	if (!Enabled()) return;
	if (auto* table = Runtime(); table != nullptr)
		table->RecordOwnerShape(production_shape);
}

inline bool ValidateCoreForTest()
{
	using TestTable = Table<1, 12, 2>;
	A64Dynarec::IrBlock ir {};
	ir.rip = UINT64_C(0x1000);
	ir.fallthrough_rip = UINT64_C(0x1005);
	ir.semantic_mask = A64Dynarec::SemanticMask(A64Dynarec::SemanticClass::Integer);
	ir.instruction_count = 1;
	ir.has_terminator = true;
	auto& instruction = ir.instructions[0];
	instruction.opcode = A64Dynarec::IrOpcode::MoveImmediate;
	instruction.width = 8;
	instruction.destination = 1;
	instruction.source = 2;
	instruction.source_width = 4;
	instruction.source2 = 3;
	instruction.condition = 4;
	instruction.vector_flags = 1;
	instruction.length = 5;
	instruction.step_cost = 2;
	instruction.effects = 3;
	instruction.address = {4, 5, 2, -7, UINT64_C(0x12345678)};
	instruction.rip = UINT64_C(0x1000);
	instruction.immediate = UINT64_C(0xabcdef);
	instruction.target = UINT64_C(0x2000);

	const uint64_t base_hash = CanonicalIrHash(ir);
	auto changed = [&](const A64Dynarec::IrBlock& candidate)
	{
		return !IrBlocksEqual(ir, candidate) && CanonicalIrHash(candidate) != base_hash;
	};
	A64Dynarec::IrBlock mutation = ir;
#define LUMEN_CROSS_OWNER_CHECK_MUTATION(expression) \
	do { mutation = ir; expression; if (!changed(mutation)) return false; } while (false)
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.rip++);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.fallthrough_rip++);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.semantic_mask ^= 2u);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instruction_count = 0);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.has_terminator = false);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instructions[0].opcode = A64Dynarec::IrOpcode::AddImmediate);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instructions[0].width++);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instructions[0].destination++);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instructions[0].source++);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instructions[0].source_width++);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instructions[0].source2++);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instructions[0].condition++);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instructions[0].vector_flags++);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instructions[0].length++);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instructions[0].step_cost++);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instructions[0].effects++);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instructions[0].address.base++);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instructions[0].address.index++);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instructions[0].address.scale++);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instructions[0].address.displacement++);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instructions[0].address.absolute++);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instructions[0].rip++);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instructions[0].immediate++);
	LUMEN_CROSS_OWNER_CHECK_MUTATION(mutation.instructions[0].target++);
#undef LUMEN_CROSS_OWNER_CHECK_MUTATION

	const std::array<uint8_t, 4> a {{1, 2, 3, 4}};
	const std::array<uint8_t, 4> b {{4, 3, 2, 1}};
	TestTable table;
	table.RecordOwnerShape(true);
	auto complete = [&](uint64_t owner, uint64_t rip,
	                    const std::array<uint8_t, 4>& bytes,
	                    const A64Dynarec::IrBlock& block, bool current = true,
	                    uint64_t guest_hash = 0, uint64_t ir_hash = 0)
	{
		auto ticket = table.Begin(owner, rip, bytes.data(), bytes.size(), block,
		                          true, A64Dynarec::EligibilityPolicy::Existing,
		                          guest_hash, ir_hash);
		if (!ticket.valid) return false;
		table.Finish(&ticket, true, 64, 16384, 10);
		table.Validate(&ticket, current);
		return !ticket.valid;
	};
	if (!complete(1, UINT64_C(0x1000), a, ir) ||
	    !complete(1, UINT64_C(0x1000), a, ir) ||
	    !complete(2, UINT64_C(0x1000), a, ir) ||
	    !complete(2, UINT64_C(0x1000), a, ir) ||
	    !complete(1, UINT64_C(0x1000), a, ir)) return false;

	// A true overlap has no validated predecessor at Begin. Exactly one event
	// becomes the new identity and the other is the redundant overlap.
	auto overlap_first = table.Begin(3, UINT64_C(0x3000), b.data(), b.size(), ir,
	                                 true, A64Dynarec::EligibilityPolicy::Existing);
	auto overlap_second = table.Begin(4, UINT64_C(0x3000), b.data(), b.size(), ir,
	                                  true, A64Dynarec::EligibilityPolicy::Existing);
	if (!overlap_first.valid || !overlap_second.valid) return false;
	table.Finish(&overlap_first, true, 64, 16384, 10);
	table.Finish(&overlap_second, true, 64, 16384, 10);
	table.Validate(&overlap_first, true);
	table.Validate(&overlap_second, true);

	// Failure, stale and post-success abandonment never become a reusable
	// predecessor.
	auto failed = table.Begin(5, UINT64_C(0x4000), a.data(), a.size(), ir, true,
	                          A64Dynarec::EligibilityPolicy::Existing);
	table.Finish(&failed, false, 0, 0, 10);
	auto stale = table.Begin(5, UINT64_C(0x5000), a.data(), a.size(), ir, true,
	                         A64Dynarec::EligibilityPolicy::Existing);
	table.Finish(&stale, true, 64, 16384, 10);
	table.Validate(&stale, false);
	auto abandoned = table.Begin(5, UINT64_C(0x6000), a.data(), a.size(), ir,
	                             true, A64Dynarec::EligibilityPolicy::Existing);
	table.Finish(&abandoned, true, 64, 16384, 10);
	table.Abandon(&abandoned);

	// Forced hash collision and a same-guest IR variant must remain distinct.
	if (!complete(6, UINT64_C(0x7000), a, ir, true, 7, 11) ||
	    !complete(6, UINT64_C(0x7001), b, ir, true, 7, 11)) return false;
	mutation = ir;
	mutation.instructions[0].immediate++;
	if (!complete(6, UINT64_C(0x7000), a, mutation, true, 7, 12)) return false;

	const auto stats = table.Snapshot();
	if (stats.identity_new != 5 || stats.same_owner_repeat != 3 ||
	    stats.cross_owner_sequential != 1 || stats.cross_owner_overlap != 1 ||
	    stats.compile_fail != 1 || stats.stale != 1 ||
	    stats.abandoned_after_ok != 1 || stats.hash_collision == 0 ||
	    stats.ir_variant == 0 || stats.ticket_invalid != 0 ||
	    stats.owner_count != 1 || stats.owner_shape_failure != 0) return false;

	Table<1, 1, 1> bounded;
	auto bounded_first = bounded.Begin(
	    1, UINT64_C(0x8000), a.data(), static_cast<uint8_t>(a.size()), ir,
	    true, A64Dynarec::EligibilityPolicy::Existing);
	if (!bounded_first.valid) return false;
	bounded.Finish(&bounded_first, true, 64, 16384, 10);
	bounded.Validate(&bounded_first, true);
	auto full = bounded.Begin(1, UINT64_C(0x8001), b.data(), b.size(), ir, true,
	                          A64Dynarec::EligibilityPolicy::Existing);
	if (full.valid || bounded.Snapshot().shard_full_drop != 1) return false;

	Table<1, 1, 1> active_bounded;
	auto active = active_bounded.Begin(1, UINT64_C(0x9000), a.data(), a.size(), ir,
	                                  true, A64Dynarec::EligibilityPolicy::Existing);
	auto overflow = active_bounded.Begin(2, UINT64_C(0x9000), a.data(), a.size(), ir,
	                                    true, A64Dynarec::EligibilityPolicy::Existing);
	active_bounded.Finish(&active, true, 64, 16384, 10);
	active_bounded.Validate(&active, true);
	if (overflow.valid || active_bounded.Snapshot().active_slot_overflow != 1)
		return false;

	// Exercise real concurrent locking and reciprocal overlap marking.
	Table<1, 2, 2> concurrent;
	std::atomic<unsigned> ready {0};
	auto worker = [&](uint64_t owner)
	{
		auto ticket = concurrent.Begin(owner, UINT64_C(0xa000), a.data(), a.size(),
		                               ir, true,
		                               A64Dynarec::EligibilityPolicy::Existing);
		ready.fetch_add(1, std::memory_order_release);
		while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
		concurrent.Finish(&ticket, true, 64, 16384, 10);
		concurrent.Validate(&ticket, true);
	};
	std::thread first(worker, 1);
	std::thread second(worker, 2);
	first.join();
	second.join();
	const auto concurrent_stats = concurrent.Snapshot();
	if (concurrent_stats.identity_new != 1 ||
	    concurrent_stats.cross_owner_overlap != 1 ||
	    concurrent_stats.ticket_invalid != 0) return false;

	Table<1, 1, 1> owner_bounded;
	for (uint64_t owner = 1; owner <= kValidatedOwnersPerIdentity + 1; owner++)
	{
		auto ticket = owner_bounded.Begin(
		    owner, UINT64_C(0xb000), a.data(), static_cast<uint8_t>(a.size()),
		    ir, true, A64Dynarec::EligibilityPolicy::Existing);
		if (!ticket.valid) return false;
		owner_bounded.Finish(&ticket, true, 64, 16384, 10);
		owner_bounded.Validate(&ticket, true);
	}
	const auto owner_stats = owner_bounded.Snapshot();
	Table<1, 1, 1> bad_shape;
	bad_shape.RecordOwnerShape(false);
	const auto bad_shape_stats = bad_shape.Snapshot();
	return owner_stats.identity_new == 1 &&
	       owner_stats.cross_owner_sequential == kValidatedOwnersPerIdentity &&
	       owner_stats.owner_overflow == 1 &&
	       bad_shape_stats.owner_count == 1 &&
	       bad_shape_stats.owner_shape_failure == 1;
}

} // namespace Lumen::Cpu::CrossOwnerTelemetry
