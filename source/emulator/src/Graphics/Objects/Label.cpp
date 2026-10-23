#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/GuestWriteDestination.h"
#include "../../../../cpu/runtime/MemoryHandoff.h"
#if defined(LUMEN_FEXCORE_RUNTIME)
#include "../../../../cpu/runtime/GuestCpuRuntime.h"
#endif

#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Vector.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/GuestDataLeaseRetry.h"
#include "Emulator/Graphics/HostPolling.h"
#include "Emulator/Graphics/LabelEventPool.h"
#include "Emulator/Profiler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

class CommandProcessor;

bool LabelScalarCaptureTicket::MatchesRecording(const LabelScalarRecordingIdentity& current) const
{
	const auto& original = m_recording;
	return IsCaptured() && m_generation != UINT64_MAX && (m_size == 4 || m_size == 8) &&
	       original.valid && current.valid && original.exclusive && current.exclusive &&
	       std::tie(original.processor, original.buffer, original.parent, original.pool, original.current_index,
	           original.processor_queue, original.buffer_queue, original.pool_index, original.submit_id,
	           original.processor_epoch, original.assigned_serial, original.recording, original.session,
	           original.serial, original.ordinal, original.submitted_cutoff, original.completed_cutoff) ==
	       std::tie(current.processor, current.buffer, current.parent, current.pool, current.current_index,
	           current.processor_queue, current.buffer_queue, current.pool_index, current.submit_id,
	           current.processor_epoch, current.assigned_serial, current.recording, current.session,
	           current.serial, current.ordinal, current.submitted_cutoff, current.completed_cutoff);
}

bool LabelScalarCaptureTicket::MatchesPublication(const LabelPublicationSnapshot& publication) const
{
	return IsCaptured() && m_generation != UINT64_MAX && !publication.identity_ambiguous &&
	       m_recording.valid && m_recording.exclusive && publication.cp == m_recording.processor &&
	       publication.queue == m_recording.buffer_queue && publication.session_epoch == m_recording.session &&
	       publication.submit_serial == m_recording.serial;
}

bool LabelDeferredCaptureTicket::MatchesRecording(const LabelScalarRecordingIdentity& current) const
{
	const auto& original = m_recording;
	return IsCaptured() && m_generation != UINT64_MAX && (m_size == 4 || m_size == 8) &&
	    original.valid && current.valid && original.exclusive == current.exclusive &&
	    std::tie(original.processor, original.buffer, original.parent, original.pool, original.current_index,
	        original.processor_queue, original.buffer_queue, original.pool_index, original.submit_id,
	        original.processor_epoch, original.assigned_serial, original.recording, original.session,
	        original.serial, original.ordinal, original.submitted_cutoff, original.completed_cutoff) ==
	    std::tie(current.processor, current.buffer, current.parent, current.pool, current.current_index,
	        current.processor_queue, current.buffer_queue, current.pool_index, current.submit_id,
	        current.processor_epoch, current.assigned_serial, current.recording, current.session,
	        current.serial, current.ordinal, current.submitted_cutoff, current.completed_cutoff);
}

enum LabelStatus
{
	New,
	Active,
	ActiveDeleted,
	NotActive,
};

static std::atomic<uint64_t> g_pending_publications {0};
static std::atomic<uint64_t> g_publication_instance {1};

struct LabelPublicationState
{
	LabelPublicationState()
	{
		identity.instance_id = g_publication_instance.fetch_add(1, std::memory_order_relaxed);
		EXIT_IF(identity.instance_id == 0 || identity.instance_id == UINT64_MAX);
	}
	// Enrollment is protected by the manager mutex; queued/identity snapshots
	// additionally use identity_mutex for detached proof validation. Cancellation
	// occurs only after worker quiescence; the worker retains a shared reference
	// while delivering callbacks and publishes completion atomically.
	uint64_t queued = 0;
	// Manager operations may replace the latest enqueue identity while an
	// older detached callback runs. The proof witness snapshots this host
	// metadata without acquiring the manager or any graphics lock.
	mutable Core::Mutex identity_mutex;
	std::atomic<uint64_t> completed {0};
	std::atomic<uint64_t> cancelled {0};
	LabelPublicationSnapshot identity;
	// Written once by the real first Set/enrollment, before event delivery.
	// Later enrollments never replace the effect descriptor of that delivery.
	LabelScalarPublicationFootprint scalar_first_delivery;
	bool scalar_first_delivery_sealed = false;
	// Separate from the first-only standalone ticket. Real enrollment seals
	// the latest delivery only when every earlier delivery has completed.
	LabelScalarPublicationFootprint scalar_single_delivery;
	bool scalar_single_delivery_sealed = false;
	std::atomic<uint64_t>* aggregate = &g_pending_publications;
};

static bool QueueLabelPublication(LabelPublicationState* state, CommandProcessor* cp, int queue,
                                  uint64_t epoch, uint64_t serial)
{
	if (state == nullptr) return false;
	Core::LockGuard identity_lock(state->identity_mutex);
	if (state->queued == UINT64_MAX || state->cancelled != 0) return false;
	auto& identity = state->identity;
	if (cp == nullptr || queue < 0 || epoch == 0 || serial == 0 ||
	    (state->queued != 0 && (identity.cp != cp || identity.queue != queue || identity.session_epoch != epoch ||
	                            serial < identity.submit_serial))) identity.identity_ambiguous = true;
	identity.cp = cp;
	identity.queue = queue;
	identity.session_epoch = epoch;
	identity.submit_serial = serial;
	if (state->aggregate->fetch_add(1, std::memory_order_acq_rel) == UINT64_MAX) EXIT("Label publication counter overflow\n");
	++state->queued;
	return true;
}

static bool CompleteLabelPublication(LabelPublicationState* state, uint64_t sequence)
{
	if (state == nullptr || sequence == 0 || state->cancelled.load(std::memory_order_acquire) != 0) return false;
	uint64_t expected = sequence - 1;
	if (!state->completed.compare_exchange_strong(expected, sequence, std::memory_order_acq_rel)) return false;
	EXIT_IF(state->aggregate->fetch_sub(1, std::memory_order_acq_rel) == 0);
	return true;
}

static LabelPublicationSnapshot SnapshotLabelPublication(const LabelPublicationState& state)
{
	Core::LockGuard identity_lock(state.identity_mutex);
	auto result = state.identity;
	result.queued = state.queued;
	result.completed = state.completed.load(std::memory_order_acquire);
	result.cancelled = state.cancelled.load(std::memory_order_acquire);
	EXIT_IF(result.completed > result.queued || result.cancelled > result.queued - result.completed);
	result.pending = result.queued - result.completed - result.cancelled;
	return result;
}

static void CancelLabelPublication(LabelPublicationState* state)
{
	// Only after the worker is quiescent and device idle has discarded commands.
	const auto pending = SnapshotLabelPublication(*state).pending;
	state->cancelled.fetch_add(pending, std::memory_order_release);
	EXIT_IF(state->aggregate->fetch_sub(pending, std::memory_order_acq_rel) < pending);
}

static constexpr size_t LabelPublicationCompactionMinGrowth = 64;

static size_t LabelPublicationCompactionWatermark(size_t retained)
{
	const size_t growth = std::max(LabelPublicationCompactionMinGrowth, retained);
	const size_t maximum = std::numeric_limits<size_t>::max();
	return growth > maximum - retained ? maximum : retained + growth;
}

static size_t AppendLabelPublication(std::vector<std::weak_ptr<LabelPublicationState>>* publications,
                                    size_t* compaction_watermark, size_t registered_labels,
                                    const std::shared_ptr<LabelPublicationState>& publication)
{
	// Registrations only append. Sweep after geometric growth, rather than
	// rescanning every live/detached publication for every fresh Label. Expiry
	// remains the sole removal authority; completion or detachment never is.
	const size_t maximum_visits = LabelScalarPublicationCensusBank::MaxVisitedStates;
	const bool visit_headroom = registered_labels < maximum_visits &&
	    publications->size() < maximum_visits - registered_labels;
	size_t visited = 0;
	if (publications->size() >= *compaction_watermark || !visit_headroom)
	{
		visited = publications->size();
		publications->erase(std::remove_if(publications->begin(), publications->end(),
		    [](const auto& state) { return state.expired(); }), publications->end());
		*compaction_watermark = LabelPublicationCompactionWatermark(publications->size());
	}
	// The forced sweep above reserves room for this entry in the existing
	// weak-list + Label visit budget whenever the live census itself fits.
	// Consumers retain their original capacity checks, including oversized
	// registries: no proof can prune its way around that admission limit.
	publications->emplace_back(publication);
	return visited;
}

struct LabelPublicationTicket
{
	std::shared_ptr<LabelPublicationState> state;
	uint64_t sequence = 0;
};

struct LabelPublicationCompletionTicketState
{
	std::shared_ptr<LabelPublicationState> publication;
	Label* label = nullptr;
	class LabelManager* manager = nullptr;
	LabelPublicationSnapshot original;
	uint64_t last_completed = 0;
};
static_assert(sizeof(LabelPublicationCompletionTicketState) <= LabelPublicationCompletionTicket::MaxRetainedBytes);

static bool LabelPublicationEnrollmentEqual(const LabelPublicationSnapshot& a, const LabelPublicationSnapshot& b)
{
	return a.instance_id == b.instance_id && a.queued == b.queued && a.cancelled == b.cancelled &&
	       a.cp == b.cp && a.queue == b.queue && a.session_epoch == b.session_epoch &&
	       a.submit_serial == b.submit_serial && a.identity_ambiguous == b.identity_ambiguous;
}

static const char* ValidateLabelCompletionSnapshot(LabelPublicationCompletionTicketState* ticket,
                                                 const LabelPublicationSnapshot& current)
{
	if (!LabelPublicationEnrollmentEqual(current, ticket->original)) return "label-completion-ticket-enrollment-changed";
	if (current.cancelled != 0 || current.identity_ambiguous || current.completed < ticket->last_completed ||
	    current.completed > ticket->original.queued || current.pending != current.queued - current.completed)
		return "label-completion-ticket-sequence-changed";
	// This is observed progress, never a recapture of the original enrollment.
	ticket->last_completed = current.completed;
	return nullptr;
}

static bool CaptureLabelPublicationPrefix(const std::vector<std::weak_ptr<LabelPublicationState>>& states,
                                         CommandProcessor* cp, int queue, uint64_t epoch, uint64_t serial,
                                         std::vector<LabelPublicationTicket>* tickets)
{
	if (cp == nullptr || queue < 0 || epoch == 0 || serial == 0 || tickets == nullptr) return false;
	for (const auto& weak: states)
	{
		auto state = weak.lock();
		if (state == nullptr || state->queued == 0) continue;
		const auto identity = SnapshotLabelPublication(*state);
		// An ambiguous history may contain this processor even when its latest
		// use names a different one. Never silently exclude it from the prefix.
		if (identity.identity_ambiguous) return false;
		if (identity.cp != cp) continue;
		if (identity.queue != queue || identity.session_epoch != epoch || identity.submit_serial > serial ||
		    identity.cancelled != 0) return false;
		tickets->push_back({std::move(state), identity.queued});
	}
	return true;
}

static bool LabelPublicationPrefixComplete(const std::vector<LabelPublicationTicket>& tickets, bool* cancelled)
{
	*cancelled = false;
	bool complete = true;
	for (const auto& ticket: tickets)
	{
		if (ticket.state->cancelled.load(std::memory_order_acquire) != 0) *cancelled = true;
		complete &= ticket.state->completed.load(std::memory_order_acquire) >= ticket.sequence;
	}
	return complete && !*cancelled;
}

static bool EopPublicationPrefixSelectorEnabled(const char* host_checkpoint, const char* virtual_clock, const char* immediate32_checkpoint)
{
	return (host_checkpoint != nullptr && std::strcmp(host_checkpoint, "1") == 0) ||
	       (virtual_clock != nullptr && std::strcmp(virtual_clock, "1") == 0) ||
	       (immediate32_checkpoint != nullptr && std::strcmp(immediate32_checkpoint, "1") == 0);
}

static bool EopPublicationPrefixEnabled()
{
	static const bool enabled = [] {
		return EopPublicationPrefixSelectorEnabled(std::getenv("LUMEN_EXPERIMENTAL_GFX_EOP_HOST_CHECKPOINT"),
		                                          std::getenv("LUMEN_EXPERIMENTAL_GFX_EOP_VIRTUAL_CLOCK"),
		                                          std::getenv("LUMEN_EXPERIMENTAL_GFX_EOP_IMMEDIATE32_CHECKPOINT"));
	}();
	return enabled;
}

struct LabelDeferredCapturedPayload
{
	std::array<uint64_t, LabelGpuObject::PARAM_PREPARATION_ROLE + 1> params {};
	uint64_t address = 0, bytes = 0, generation = UINT64_MAX;
	const void* processor = nullptr;
	int queue = -1;
	uint64_t epoch = 0, serial = 0;
	bool IsCaptured() const { return address != 0; }
};

struct LabelCallbacks
{
	uint64_t*                  dst_gpu_addr64       = nullptr;
	uint64_t                   value64              = 0;
	uint32_t*                  dst_gpu_addr32       = nullptr;
	uint32_t                   value32              = 0;
	LabelGpuObject::callback_t callback_1           = nullptr;
	LabelGpuObject::callback_t callback_2           = nullptr;
	uint64_t                   args[LABEL_ARGS_MAX] = {};
	std::shared_ptr<LabelPublicationState> publication;
	uint64_t publication_sequence = 0;
	uint64_t write_bytes = 0;
	uint64_t destination_generation = UINT64_MAX;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	// Detached deliveries share a lifetime seal, not a persistent data lease.
	// Each publication still acquires exact native admission before touching it.
	std::shared_ptr<Core::VirtualMemory::GuestDataRangeSeal> destination_seal;
#endif
	// Filled only by Set from its actual CommandBuffer; args are never an
	// authority carrier. Detachment copies the immutable producer and counters.
	LabelPublicationSnapshot delivery_identity;
	LabelGpuObject::PreparationRole preparation_role = LabelGpuObject::PreparationRole::Ordinary;
	LabelScalarCaptureTicket scalar_capture;
	LabelDeferredCapturedPayload deferred_capture;
};

struct LabelScalarPublicationCompletionTicketState
{
	std::shared_ptr<LabelPublicationState> publication;
	Label* label = nullptr;
	class LabelManager* manager = nullptr;
	LabelScalarPublicationFootprint delivery;
	uint64_t last_completed = 0;
};
static_assert(sizeof(LabelScalarPublicationCompletionTicketState) <= LabelScalarPublicationCompletionTicket::MaxRetainedBytes);

struct LabelScalarPublicationCensusBankState
{
	struct Retained
	{
		std::shared_ptr<LabelPublicationState> publication;
		Label* label = nullptr;
		LabelPublicationSnapshot original;
		LabelScalarPublicationFootprint delivery;
		uint64_t last_completed = 0;
		bool scalar = false;
	};
	class LabelManager* manager = nullptr;
	uint64_t enrollment_epoch = 0, reset_epoch = 0;
	uint32_t scalars = 0;
	std::vector<Retained> states;
};

using LabelCensusRegistered = std::unordered_map<LabelPublicationState*, Label*>;
using LabelCensusInstances = std::unordered_map<uint64_t, LabelPublicationState*>;
static constexpr uint64_t LabelCensusFixedDiscoveryScratchBytes = 1024;

static constexpr uint64_t LabelCensusRetainedMetadataBytes(uint64_t states, uint64_t capacity)
{
	return sizeof(LabelScalarPublicationCensusBank) + sizeof(LabelScalarPublicationCensusBankState) +
	    LabelScalarPublicationCensusBank::SharedStateAccountingOverheadBytes +
	    capacity * sizeof(LabelScalarPublicationCensusBankState::Retained) +
	    states * (sizeof(LabelPublicationState) + LabelScalarPublicationCensusBank::SharedStateAccountingOverheadBytes);
}

static constexpr uint64_t LabelCensusDiscoveryMetadataBytes(uint64_t states, uint64_t capacity)
{
	// The two complete maps and the row vector are discovery scratch, including
	// fixed snapshot/local storage. Node/control allowance is logical; allocator
	// bucket capacity and bookkeeping follow the common resource-proof policy.
	return sizeof(std::vector<LabelScalarPublicationCensusBankState::Retained>) + sizeof(LabelCensusRegistered) +
	    sizeof(LabelCensusInstances) + LabelCensusFixedDiscoveryScratchBytes +
	    capacity * sizeof(LabelScalarPublicationCensusBankState::Retained) +
	    states * (sizeof(LabelCensusRegistered::value_type) + sizeof(LabelCensusInstances::value_type) +
	              2 * LabelScalarPublicationCensusBank::SharedStateAccountingOverheadBytes);
}

static constexpr uint64_t LabelCensusCombinedMetadataBytes(uint64_t states, uint64_t capacity)
{
	// Validation retains the original bank and discovers the same census once.
	// Shared publication allocations are already counted by the retained bank.
	return LabelCensusRetainedMetadataBytes(states, capacity) + LabelCensusDiscoveryMetadataBytes(states, capacity);
}

struct LabelCompletedRetirementTicketState
{
	std::shared_ptr<LabelPublicationState> publication;
	Label* label = nullptr;
	class LabelManager* manager = nullptr;
	LabelScalarPublicationCensusBank census;
	LabelCompletedRetirementFootprint footprint;
	std::array<uint64_t, LabelGpuObject::PARAM_PREPARATION_ROLE + 1> params {};
	VkDevice device = nullptr;
	VkEvent event = nullptr;
};
static_assert(sizeof(LabelCompletedRetirementTicketState) <= LabelCompletedRetirementTicket::MaxRetainedBytes);
static_assert(LabelCensusCombinedMetadataBytes(LabelScalarPublicationCensusBank::MaxEnrolledStates,
    LabelScalarPublicationCensusBank::MaxEnrolledStates) <
    LabelScalarPublicationCensusBank::MaxCombinedRetainedMetadataBytes);

struct LabelWriteBackProofScope
{
	LabelCallbacks delivery;
};

static thread_local uint32_t g_label_worker_delivery_depth = 0;
static thread_local uint32_t g_label_preparation_depth = 0;
static thread_local const LabelWriteBackProofScope* g_label_writeback_scope = nullptr;

struct Label
{
	VkDevice          device = nullptr;
	VkEvent           event  = nullptr;
	LabelStatus       status = LabelStatus::New;
	LabelCallbacks    callbacks;
	CommandProcessor* cp = nullptr;
};

// The list owns registry nodes, not Labels. Its exact pointer index is always
// checked before a caller may dereference a Label supplied by another owner.
// Every operation and iteration is protected by LabelManager::m_mutex.
class OrderedLabelRegistry
{
public:
	using List = std::list<Label*>;

private:
	struct Entry { List::iterator position; uint64_t order; };
	struct ActiveEntry { Label* label; uint64_t order; };

public:
	OrderedLabelRegistry() = default;
	LUMEN_CLASS_NO_COPY(OrderedLabelRegistry);

	void Add(Label* label)
	{
		EXIT_IF(label == nullptr || m_index.find(label) != m_index.end());
		EXIT_IF(m_next_order == UINT64_MAX);
		m_order.push_back(label);
		const bool inserted = m_index.emplace(label, Entry {std::prev(m_order.end()), ++m_next_order}).second;
		EXIT_IF(!inserted);
	}
	Label* Find(Label* label) const
	{
		const auto found = m_index.find(label);
		return found == m_index.end() ? nullptr : *found->second.position;
	}
	bool Activate(Label* label)
	{
		// Membership remains the authority before dereferencing a supplied key.
		const auto found = m_index.find(label);
		if (found == m_index.end()) return false;
		if (label->status != LabelStatus::Active && label->status != LabelStatus::ActiveDeleted) return false;
		const auto position = ActivePosition(found->second.order);
		if (position != m_active.end() && position->order == found->second.order) return false;
		m_active.insert(position, ActiveEntry {label, found->second.order});
		return true;
	}
	// The manager owns its lock for this entire scan. The visitor may only
	// snapshot completion and collect detached work, never call guest code or
	// modify registry membership. Stable compaction preserves registration order
	// even when labels were activated/re-enrolled in a different order.
	template <typename Visit>
	void ScanActive(Visit&& visit)
	{
		size_t retained = 0;
		size_t scanned = 0;
		for (const auto entry: m_active)
		{
			if (visit(entry.label))
			{
				if (retained != scanned) m_active[retained] = entry;
				++retained;
			}
			++scanned;
		}
		m_active.resize(retained);
	}
	void PruneCompletedActive()
	{
		ScanActive([](Label* label) {
			EXIT_IF(label->status != LabelStatus::Active && label->status != LabelStatus::ActiveDeleted &&
			        label->status != LabelStatus::NotActive);
			return label->status != LabelStatus::NotActive;
		});
	}
	bool Remove(Label* label)
	{
		const auto found = m_index.find(label);
		if (found == m_index.end()) return false;
		const auto active = ActivePosition(found->second.order);
		if (active != m_active.end() && active->order == found->second.order) m_active.erase(active);
		m_order.erase(found->second.position);
		m_index.erase(found);
		return true;
	}
	void TakeAll(List* destination)
	{
		EXIT_IF(destination == nullptr || !destination->empty());
		m_active.clear();
		m_index.clear();
		destination->splice(destination->end(), m_order);
	}
	size_t Size() const { return m_order.size(); }
	size_t ActiveSize() const { return m_active.size(); }
	auto begin() const { return m_order.begin(); }
	auto end() const { return m_order.end(); }

private:
	std::vector<ActiveEntry>::iterator ActivePosition(uint64_t order)
	{
		return std::lower_bound(m_active.begin(), m_active.end(), order,
		    [](const ActiveEntry& entry, uint64_t value) { return entry.order < value; });
	}
	List m_order;
	std::unordered_map<Label*, Entry> m_index;
	std::vector<ActiveEntry> m_active;
	uint64_t m_next_order = 0;
};

enum class LabelRemoval { Missing, Invalid, ResetDeferred, ActiveDeferred, Removed };

static LabelRemoval RemoveRegisteredLabel(OrderedLabelRegistry* registry, Label* pointer, bool resetting)
{
	auto* label = registry->Find(pointer);
	if (label == nullptr) return LabelRemoval::Missing;
	if (label->event == nullptr || label->device == nullptr) return LabelRemoval::Invalid;
	if (resetting) return LabelRemoval::ResetDeferred;
	if (label->status != LabelStatus::NotActive && label->status != LabelStatus::Active) return LabelRemoval::Invalid;
	if (label->status == LabelStatus::Active)
	{
		label->status = LabelStatus::ActiveDeleted;
		return LabelRemoval::ActiveDeferred;
	}
	EXIT_IF(!registry->Remove(label));
	return LabelRemoval::Removed;
}

using RecycledLabelEventPool = LabelEventPool<VkDevice, VkEvent>;

static bool ExactOneEnvironmentValue(const char* value)
{
	return value != nullptr && std::strcmp(value, "1") == 0;
}

static bool LabelRegistryTraceEnabled()
{
	static const bool enabled = ExactOneEnvironmentValue(std::getenv("LUMEN_LABEL_REGISTRY_TRACE"));
	return enabled;
}

static bool LabelActiveFrontierEnabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_GFX_LABEL_ACTIVE_FRONTIER");
		return value == nullptr || ExactOneEnvironmentValue(value);
	}();
	return enabled;
}

static bool LabelBatchWaitEnabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_GFX_LABEL_BATCH_WAIT");
		if (value != nullptr) return ExactOneEnvironmentValue(value);
#if ((LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID) && \
     LUMEN_ABI == LUMEN_ABI_ARM64_V8A) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
		return true;
#else
		return false;
#endif
	}();
	return enabled;
}

enum class LabelBatchWaitResult { Invalid, Waited, Reused };

// Only detached Labels from one immutable worker snapshot may share this proof.
// Set requires registry membership, so their command processors cannot acquire
// new uses of these events after the snapshot was taken. Reentrant retirement
// queues another snapshot, which must get a fresh instance and wait again.
class LabelBatchWait
{
public:
	explicit LabelBatchWait(bool enabled): m_enabled(enabled) {}
	template <typename Wait>
	LabelBatchWaitResult WaitFor(CommandProcessor* cp, Wait&& wait)
	{
		if (cp == nullptr) return LabelBatchWaitResult::Invalid;
		if (m_enabled && std::find(m_completed.begin(), m_completed.end(), cp) != m_completed.end())
			return LabelBatchWaitResult::Reused;
		if (!wait(cp)) return LabelBatchWaitResult::Invalid;
		if (m_enabled) m_completed.push_back(cp);
		return LabelBatchWaitResult::Waited;
	}
	template <typename Wait>
	LabelBatchWaitResult WaitForPrefix(const LabelPublicationSnapshot& identity, Wait&& wait)
	{
		if (identity.cp == nullptr || identity.queue < 0 || identity.queue >= GraphicContext::QUEUES_NUM ||
		    identity.session_epoch == 0 || identity.submit_serial == 0 || identity.queued == 0 ||
		    identity.cancelled != 0 || identity.identity_ambiguous) return LabelBatchWaitResult::Invalid;
		if (m_enabled)
		{
			// A full wait covers all event uses of this already-detached batch.
			if (std::find(m_completed.begin(), m_completed.end(), identity.cp) != m_completed.end())
				return LabelBatchWaitResult::Reused;
			for (const auto& completed: m_prefixes)
				if (completed.cp == identity.cp && completed.queue == identity.queue &&
				    completed.epoch == identity.session_epoch && completed.serial >= identity.submit_serial)
					return LabelBatchWaitResult::Reused;
		}
		if (!wait(identity)) return LabelBatchWaitResult::Invalid;
		if (m_enabled)
		{
			for (auto& completed: m_prefixes)
				if (completed.cp == identity.cp && completed.queue == identity.queue && completed.epoch == identity.session_epoch)
				{
					completed.serial = std::max(completed.serial, identity.submit_serial);
					return LabelBatchWaitResult::Waited;
				}
			m_prefixes.push_back({identity.cp, identity.queue, identity.session_epoch, identity.submit_serial});
		}
		return LabelBatchWaitResult::Waited;
	}

private:
	struct CompletedPrefix { CommandProcessor* cp; int queue; uint64_t epoch, serial; };
	const bool m_enabled;
	std::vector<CommandProcessor*> m_completed;
	std::vector<CompletedPrefix> m_prefixes;
};

struct LabelRegistryTrace
{
	uint64_t checks = 0;
	uint32_t records = 0;
	std::chrono::steady_clock::time_point start {};
	std::chrono::steady_clock::time_point last {};
	size_t peak = 0;
	std::array<uint64_t, 4> states {};
	uint64_t creates = 0, sets = 0, remove_attempts = 0, removed = 0, deferred = 0, resets = 0;
	uint64_t scans = 0, visited = 0, event_queries = 0, event_errors = 0, ready = 0, delivered = 0;
	uint64_t destroy_started = 0, destroy_finished = 0;
	uint64_t wait_calls = 0, wait_avoided = 0;
	uint64_t event_query_ns = 0, callback_queue_us = 0, callback_us = 0, destroy_us = 0;
};

static bool AndroidFexEventPoolEnabled()
{
#if defined(__ANDROID__) && defined(__aarch64__) && defined(LUMEN_ANDROID_FEXCORE_RUNTIME)
	static const bool enabled =
	    ExactOneEnvironmentValue(std::getenv("LUMEN_EXPERIMENTAL_GFX_ANDROID_FEX_EVENT_POOL"));
	return enabled;
#else
	return false;
#endif
}

static bool TakeRecycledLabelEvent(RecycledLabelEventPool* events, VkDevice device, VkEvent* event)
{
	EXIT_IF(events == nullptr || device == nullptr || event == nullptr);
	return events->Take(device, event);
}

static bool ShouldLogEventPoolCount(uint64_t count)
{
	return count <= 3 || (count != 0 && (count & (count - 1)) == 0);
}

class LabelManager
{
public:
	LabelManager()
	{
		EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());
		Core::Thread t(ThreadRun, this);
		t.Detach();
	}
	virtual ~LabelManager()
	{
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		if (m_test_instance) return;
#endif
		LUMEN_NOT_IMPLEMENTED;
	}
	LUMEN_CLASS_NO_COPY(LabelManager);

	Label* Create64(GraphicContext* ctx, uint64_t* dst_gpu_addr, uint64_t value, LabelGpuObject::callback_t callback_1,
	                LabelGpuObject::callback_t callback_2, const uint64_t* args, uint64_t write_bytes,
	                LabelGpuObject::PreparationRole preparation_role);
	Label* Create32(GraphicContext* ctx, uint32_t* dst_gpu_addr, uint32_t value, LabelGpuObject::callback_t callback_1,
	                LabelGpuObject::callback_t callback_2, const uint64_t* args, uint64_t write_bytes,
	                LabelGpuObject::PreparationRole preparation_role);
	void   Delete(Label* label);
	void   Set(CommandBuffer* buffer, Label* label, const LabelScalarCaptureTicket* scalar_capture,
	           const LabelDeferredCaptureTicket* deferred_capture);
	bool   BeginSessionReset();
	bool   FinishSessionReset();
	bool   GetPublicationSnapshot(Label* label, LabelPublicationSnapshot* snapshot,
	                              LabelPublicationHostMetadata* diagnostic = nullptr);
	bool   CapturePublicationCompletionTicket(Label* label, const LabelPublicationSnapshot& original,
	                                         LabelPublicationCompletionTicket* ticket);
	bool   ValidatePublicationCompletionTicket(const LabelPublicationCompletionTicket& ticket,
	                                          LabelPublicationSnapshot* current, const char** reason);
	bool   CaptureScalarPublicationCompletionTicket(Label* label, const LabelPublicationSnapshot& original,
	    LabelScalarPublicationCompletionTicket* ticket, LabelScalarPublicationFootprint* footprint,
	    GraphicsGuestLeaseResult* mapping_result, const char** reason);
	bool   ValidateScalarPublicationCompletionTicket(const LabelScalarPublicationCompletionTicket& ticket,
	    LabelPublicationSnapshot* current, GraphicsGuestLeaseResult* mapping_result, const char** reason);
	bool   CaptureScalarPublicationCensusBank(LabelScalarPublicationCensusBank* bank, const char** reason,
	    uint64_t retained_metadata_budget, LabelScalarPublicationCensusCaptureWitness* witness);
	bool   ValidateScalarPublicationCensusBank(const LabelScalarPublicationCensusBank& bank, const char** reason);
	bool   CaptureCompletedRetirementTicket(Label* label, uint64_t expected_instance, uint64_t address, uint64_t bytes,
	    const uint64_t* tracked_params, const LabelScalarPublicationCensusBank& census,
	    LabelCompletedRetirementTicket* ticket, LabelCompletedRetirementFootprint* footprint,
	    GraphicsGuestLeaseResult* mapping_result, const char** reason);
	bool   ValidateCompletedRetirementTicket(const LabelCompletedRetirementTicket& ticket,
	    GraphicsGuestLeaseResult* mapping_result, const char** reason);
	bool   WaitForPublicationPrefix(CommandProcessor* cp, int queue, uint64_t epoch, uint64_t serial,
	                                uint64_t* publications);

private:
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	friend bool LabelPendingInventoryContractTest(CommandProcessor*, int, uint64_t, uint64_t,
	    bool (*)(void*, Label*, bool (*)(Label*)), void*);
	friend bool LabelScalarPublicationCompletionContractTest();
	friend bool LabelScalarPublicationCensusContractTest();
	friend bool LabelCompletedRetirementContractTest();
	friend bool LabelCompletedRetirementInventoryContractTest(CommandProcessor*, int, uint64_t, uint64_t,
	    uint64_t, uint64_t, bool,
	    bool (*)(void*, Label*, const LabelScalarPublicationCensusBank&, const LabelGpuObject&), void*, uint64_t, uint64_t);
	friend bool LabelScalarPublicationCensusInventoryContractTest(CommandProcessor*, int, uint64_t, uint64_t,
	    uint64_t, uint64_t, bool,
	    bool (*)(void*, Label*, const LabelScalarPublicationCensusBank&, bool (*)(Label*)), void*, uint64_t);
	friend bool LabelScalarPublicationCensusPairInventoryContractTest(CommandProcessor*, int, uint64_t, uint64_t,
	    const uint64_t*, const uint64_t*, bool,
	    bool (*)(void*, Label* const*, const LabelScalarPublicationCensusBank&, bool (*)(Label*)), void*, uint64_t);
	struct TestInstance {};
	explicit LabelManager(TestInstance): m_test_instance(true) {}
	bool m_test_instance = false;
	uint64_t m_test_census_row_copies = 0, m_test_census_mapping_checks = 0, m_test_census_bank_constructions = 0;
	uint64_t m_test_publication_compaction_visits = 0, m_test_publication_compactions = 0;
#endif
	static void ThreadRun(void* data);

	bool        Remove(Label* label, bool track_destroy = false);
	void        QueueDestroy(Label* label);
	void        Destroy(Label* label, LabelBatchWait* batch_wait);
	VkEvent     AcquireEventLocked(VkDevice device);
	void        RecycleOrDestroyEvent(VkDevice device, VkEvent event);
	void        RegisterLabelLocked(Label* label);
	bool        EnrollPublicationLocked(Label* label, CommandProcessor* cp, int queue, uint64_t epoch, uint64_t serial);
	bool        ScanPublicationCensusLocked(std::vector<LabelScalarPublicationCensusBankState::Retained>* states,
	                                      const char** reason, uint64_t retained_metadata_budget,
	                                      const LabelScalarPublicationCensusBankState* retained_bank = nullptr,
	                                      LabelScalarPublicationCensusCaptureWitness* witness = nullptr);
	bool        TraceEnabled() const { return m_trace_enabled && m_trace.records < 128; }
	void        TraceLocked(const char* operation);

	Core::Mutex               m_mutex;
	Core::CondVar             m_cond_var;
	OrderedLabelRegistry      m_labels;
	std::vector<std::weak_ptr<LabelPublicationState>> m_publications;
	size_t                    m_publication_compaction_watermark = LabelPublicationCompactionMinGrowth;
	Vector<Label*>            m_destroy_queue;
	RecycledLabelEventPool     m_recycled_events;
	bool                      m_session_resetting = false;
	uint32_t                  m_worker_batches    = 0;
	uint64_t                  m_enrollment_epoch  = 1;
	uint64_t                  m_reset_epoch       = 1;
	bool                      m_census_epoch_exhausted = false;
	uint64_t                  m_event_creates     = 0;
	uint64_t                  m_event_reuses      = 0;
	uint64_t                  m_event_recycles    = 0;
	const bool                m_trace_enabled     = LabelRegistryTraceEnabled();
	LabelRegistryTrace        m_trace;
};

static LabelManager* g_label_manager = nullptr;

static bool TakeLabelDestroyBatch(Vector<Label*>* queue, Vector<Label*>* batch)
{
	EXIT_IF(queue == nullptr || batch == nullptr || !batch->IsEmpty());
	if (queue->IsEmpty())
	{
		return false;
	}
	*batch = std::move(*queue);
	*queue = Vector<Label*>();
	return true;
}

struct LabelWorkerScratch
{
	Vector<Label*> deleted;
	Vector<LabelCallbacks> fired;
	Vector<Label*> ready_deleted;

	void FinishScan() { ready_deleted.Clear(); }
	void FinishRound()
	{
		// Preserve the old end-of-iteration destruction order and release every
		// callback/publication reference. Only empty storage enters the next round.
		fired.Clear();
		deleted.Clear();
	}
};

// Both batches are already snapshots owned by the worker. Callback reentry may
// enqueue more destruction, but cannot extend this batch or destroy recursively.
// Completion releases each reservation only after its own work has finished.
template <typename Fire, typename Destroy, typename Complete>
static void RunLabelWorkerBatch(const Vector<LabelCallbacks>& fired, const Vector<Label*>& deleted,
                               Fire&& fire, Destroy&& destroy, Complete&& complete)
{
	if (!fired.IsEmpty())
	{
		for (const auto& callbacks: fired) fire(callbacks);
		complete(true, fired.Size());
	}
	for (auto* label: deleted)
	{
		destroy(label);
		complete(false, 1u);
	}
}

static bool SnapshotCompletedLabel(Label* label, VkResult event_status, LabelCallbacks* snapshot)
{
	if ((label->status != LabelStatus::Active && label->status != LabelStatus::ActiveDeleted) || event_status != VK_EVENT_SET)
		return false;
	*snapshot = label->callbacks;
	if (snapshot->publication != nullptr && snapshot->delivery_identity.cp != nullptr)
	{
		const auto current = SnapshotLabelPublication(*snapshot->publication);
		auto& retained = snapshot->delivery_identity;
		retained.identity_ambiguous |= current.identity_ambiguous || label->cp != retained.cp ||
		    current.instance_id != retained.instance_id || current.cp != retained.cp || current.queue != retained.queue ||
		    current.session_epoch != retained.session_epoch || current.submit_serial != retained.submit_serial;
		retained.queued = current.queued;
		retained.completed = current.completed;
		retained.cancelled = current.cancelled;
		retained.pending = current.pending;
	}
	label->status = LabelStatus::NotActive;
	return true;
}

static bool LabelObjectWriteShapeIsValid(uint64_t object_size, uint64_t write_bytes)
{
	return write_bytes == 0 ? object_size == 4 || object_size == 8
	                        : object_size == write_bytes && write_bytes >= sizeof(uint32_t) &&
	                          write_bytes <= LabelGpuObject::MAX_WRITE_BYTES && write_bytes % sizeof(uint32_t) == 0;
}

static bool DecodeLabelPreparationRole(uint64_t value, LabelGpuObject::PreparationRole* role)
{
	if (role == nullptr || (value != static_cast<uint64_t>(LabelGpuObject::PreparationRole::Ordinary) &&
	    value != static_cast<uint64_t>(LabelGpuObject::PreparationRole::LockFreeWriteBack))) return false;
	*role = static_cast<LabelGpuObject::PreparationRole>(value);
	return true;
}

static uint64_t LabelDestinationAddress(const LabelCallbacks& label)
{
	return reinterpret_cast<uint64_t>(label.dst_gpu_addr64 != nullptr ? static_cast<void*>(label.dst_gpu_addr64)
	                                                                : static_cast<void*>(label.dst_gpu_addr32));
}

static void RecordLabelCensusCaptureRow(LabelScalarPublicationCensusCaptureWitness* witness, Label* label,
    const LabelPublicationState* publication, const LabelPublicationSnapshot& original, uint32_t row)
{
	if (witness == nullptr || witness->reason != nullptr) return;
	witness->row_present = true;
	witness->row_index = row;
	witness->key = label;
	witness->publication_identity = reinterpret_cast<uint64_t>(publication);
	witness->original = original;
	if (label == nullptr) return;
	const auto& delivery = label->callbacks;
	witness->delivery_enrollment = delivery.delivery_identity;
	witness->registered.publication_identity = witness->publication_identity;
	witness->registered.registered_sequence = delivery.publication_sequence;
	witness->registered.registered_destination = LabelDestinationAddress(delivery);
	witness->registered.registered_write_bytes = delivery.write_bytes;
	witness->registered.registered_destination_generation = delivery.destination_generation;
	witness->registered.registered_callback_1 = reinterpret_cast<uint64_t>(delivery.callback_1);
	witness->registered.registered_callback_2 = reinterpret_cast<uint64_t>(delivery.callback_2);
	witness->registered.registered_preparation_role = static_cast<uint64_t>(delivery.preparation_role);
}

static bool DescribeSingleScalarDelivery(const LabelCallbacks& delivery, LabelScalarPublicationFootprint* footprint)
{
	const auto& enrollment = delivery.delivery_identity;
	const bool scalar64 = delivery.dst_gpu_addr64 != nullptr && delivery.dst_gpu_addr32 == nullptr;
	const bool scalar32 = delivery.dst_gpu_addr32 != nullptr && delivery.dst_gpu_addr64 == nullptr;
	const uint64_t bytes = scalar64 ? sizeof(uint64_t) : sizeof(uint32_t);
	const uint64_t address = LabelDestinationAddress(delivery);
	if ((!scalar64 && !scalar32) || address == 0 || address % bytes != 0 || delivery.write_bytes != bytes ||
	    address > UINT64_MAX - bytes || delivery.destination_generation == UINT64_MAX ||
	    delivery.callback_1 != nullptr || delivery.callback_2 != nullptr ||
	    delivery.preparation_role != LabelGpuObject::PreparationRole::Ordinary || delivery.publication == nullptr ||
	    delivery.publication_sequence == 0 || enrollment.instance_id == 0 || enrollment.queued != delivery.publication_sequence ||
	    enrollment.completed != enrollment.queued - 1 || enrollment.cancelled != 0 || enrollment.pending != 1 || enrollment.cp == nullptr ||
	    enrollment.queue < 0 || enrollment.session_epoch == 0 || enrollment.submit_serial == 0 || enrollment.identity_ambiguous)
		return false;
	*footprint = {enrollment, address, bytes, scalar64 ? delivery.value64 : delivery.value32,
	              delivery.destination_generation, scalar64};
	return true;
}

static bool DescribeFirstScalarDelivery(const LabelCallbacks& delivery, LabelScalarPublicationFootprint* footprint)
{
	return delivery.publication_sequence == 1 && DescribeSingleScalarDelivery(delivery, footprint);
}

static bool ScalarDeliverySealEqual(const LabelScalarPublicationFootprint& a, const LabelScalarPublicationFootprint& b)
{
	return LabelPublicationEnrollmentEqual(a.enrollment, b.enrollment) &&
	    a.enrollment.completed == b.enrollment.completed && a.enrollment.pending == b.enrollment.pending &&
	    std::tie(a.address, a.bytes, a.value, a.original_generation, a.scalar64) ==
	    std::tie(b.address, b.bytes, b.value, b.original_generation, b.scalar64);
}

static bool QueueLabelPublicationDelivery(LabelCallbacks* delivery, CommandProcessor* cp, int queue,
                                         uint64_t epoch, uint64_t serial)
{
	if (!QueueLabelPublication(delivery->publication.get(), cp, queue, epoch, serial)) return false;
	delivery->publication_sequence = delivery->publication->queued;
	delivery->delivery_identity = SnapshotLabelPublication(*delivery->publication);
	// Only Set/enrollment creates this immutable description. A later enqueue
	// cannot retroactively characterize an older detached worker delivery.
	if (delivery->publication_sequence == 1)
	{
		delivery->publication->scalar_first_delivery_sealed =
		    DescribeFirstScalarDelivery(*delivery, &delivery->publication->scalar_first_delivery);
	}
	// A copied older worker delivery may still run after registry reuse. Its
	// incomplete counter prevents this latest enrollment from acquiring a seal.
	delivery->publication->scalar_single_delivery = {};
	delivery->publication->scalar_single_delivery_sealed =
	    DescribeSingleScalarDelivery(*delivery, &delivery->publication->scalar_single_delivery);
	return true;
}

static const char* ValidateScalarCompletionSnapshot(LabelScalarPublicationCompletionTicketState* ticket,
                                                   const LabelPublicationSnapshot& current)
{
	const auto& original = ticket->delivery.enrollment;
	if (!LabelPublicationEnrollmentEqual(current, original)) return "scalar-completion-enrollment-changed";
	if (current.cancelled != 0 || current.identity_ambiguous || current.completed < ticket->last_completed ||
	    current.completed > original.queued || current.pending != current.queued - current.completed)
		return "scalar-completion-sequence-changed";
	return nullptr;
}

static bool ScalarDeliveryMappingContinuous(const LabelScalarPublicationFootprint& delivery,
                                            GraphicsGuestLeaseResult* mapping_result, const char** reason)
{
	Core::VirtualMemory::GuestDataLease lease;
	GraphicsGuestLeaseResult result;
	const bool continuous = TryAcquireGraphicsGuestContinuityLease(delivery.address, delivery.bytes,
	    Core::VirtualMemory::Mode::Write, delivery.original_generation, &lease, &result);
	if (mapping_result != nullptr) *mapping_result = result;
	if (!continuous && reason != nullptr) *reason = result.reason != nullptr ? result.reason : "scalar-completion-mapping-unproved";
	return continuous;
}

static bool LabelScalarCapturePayloadMatches(const LabelCallbacks& label, const LabelScalarCaptureTicket& ticket)
{
	const bool scalar64 = label.dst_gpu_addr64 != nullptr && label.dst_gpu_addr32 == nullptr;
	const bool scalar32 = label.dst_gpu_addr32 != nullptr && label.dst_gpu_addr64 == nullptr;
	return ticket.IsCaptured() && (scalar64 || scalar32) && ticket.Size() == (scalar64 ? 8u : 4u) &&
	       ticket.Address() == LabelDestinationAddress(label) && ticket.Size() == label.write_bytes &&
	       ticket.Value() == (scalar64 ? label.value64 : label.value32) && ticket.Generation() != UINT64_MAX &&
	       label.callback_1 == nullptr && label.callback_2 == nullptr &&
	       label.preparation_role == LabelGpuObject::PreparationRole::Ordinary;
}

static bool InstallLabelScalarCapture(LabelCallbacks* label, const LabelScalarCaptureTicket& ticket,
                                     const LabelScalarRecordingIdentity& recording)
{
	if (label == nullptr || !LabelScalarCapturePayloadMatches(*label, ticket) || !ticket.MatchesRecording(recording)) return false;
	// Capture happened before renderer/registry enrollment. No mapping lease is
	// renewed here: the worker must reprove this original token before effects.
	label->destination_generation = ticket.Generation();
	label->scalar_capture = ticket;
	return true;
}

static bool LabelDeferredPayloadMatches(const LabelCallbacks& label, const LabelDeferredCapturedPayload& captured)
{
	const bool scalar64 = label.dst_gpu_addr64 != nullptr && label.dst_gpu_addr32 == nullptr;
	const bool scalar32 = label.dst_gpu_addr32 != nullptr && label.dst_gpu_addr64 == nullptr;
	if (!captured.IsCaptured() || captured.generation == UINT64_MAX || (!scalar64 && !scalar32) ||
	    captured.bytes != (scalar64 ? 8u : 4u) || captured.address != LabelDestinationAddress(label) ||
	    label.write_bytes != captured.bytes || captured.params[LabelGpuObject::PARAM_WRITE_BYTES] != 0 ||
	    captured.params[LabelGpuObject::PARAM_VALUE] != (scalar64 ? label.value64 : label.value32) ||
	    captured.params[LabelGpuObject::PARAM_CALLBACK_1] != reinterpret_cast<uint64_t>(label.callback_1) ||
	    captured.params[LabelGpuObject::PARAM_CALLBACK_2] != reinterpret_cast<uint64_t>(label.callback_2) ||
	    captured.params[LabelGpuObject::PARAM_PREPARATION_ROLE] != static_cast<uint64_t>(label.preparation_role)) return false;
	for (uint32_t i = 0; i < LABEL_ARGS_MAX; ++i)
		if (captured.params[LabelGpuObject::PARAM_ARG_1 + i] != label.args[i]) return false;
	return true;
}

static bool InstallLabelDeferredCapture(LabelCallbacks* label, const LabelDeferredCaptureTicket& ticket,
                                        const LabelScalarRecordingIdentity& recording)
{
	if (label == nullptr || !ticket.MatchesRecording(recording)) return false;
	const auto& producer = ticket.Recording();
	LabelDeferredCapturedPayload captured {ticket.Params(), ticket.Address(), ticket.Size(), ticket.Generation(),
	    producer.processor, producer.buffer_queue, producer.session, producer.serial};
	if (!LabelDeferredPayloadMatches(*label, captured)) return false;
	label->destination_generation = captured.generation;
	label->deferred_capture = captured;
	return true;
}

static bool BorrowLabelDestination(const LabelCallbacks& label, Core::VirtualMemory::GuestDataLease* lease,
                                   const char** rejection = nullptr, bool* lease_acquisition_unavailable = nullptr)
{
	if (lease_acquisition_unavailable != nullptr) *lease_acquisition_unavailable = false;
	const auto reject = [rejection](const char* reason) {
		if (rejection != nullptr) *rejection = reason;
		return false;
	};
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	using namespace Core::VirtualMemory;
	const uint64_t address = LabelDestinationAddress(label);
	if (address == 0 || label.write_bytes == 0 || address > UINT64_MAX - label.write_bytes) return reject("destination-shape");
	GuestDataLeaseDenialMetadata denial;
	if (!TryAcquireGuestDataLease(address, label.write_bytes, Mode::Write, lease, false, &denial))
	{
		if (lease_acquisition_unavailable != nullptr)
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
			*lease_acquisition_unavailable = denial.retryable;
#else
			*lease_acquisition_unavailable = true;
#endif
		return reject("lease-unavailable");
	}
	if (lease->HostBegin() != address || lease->ByteCount() != label.write_bytes) return reject("lease-shape");
	if (lease->MutationGeneration() == UINT64_MAX) return reject("mapping-generation-unavailable");
	uint64_t cursor = address;
	while (cursor < address + label.write_bytes)
	{
		ManagedRegion region {};
		if (QueryManagedRegion(cursor, &region) != ManagedRegionState::Visible) return reject("destination-unowned");
		if (region.begin > cursor || region.end <= cursor) return reject("destination-range");
		if (IsExecute(region.mode)) return reject("destination-executable");
		if ((static_cast<uint32_t>(region.mode) & static_cast<uint32_t>(Mode::Write)) == 0)
			return reject("destination-protection");
		cursor = std::min(address + label.write_bytes, region.end);
	}
	return true;
#else
	// These VM backends do not yet pin deferred destinations. Preserve their
	// current route explicitly; a query alone must never claim lease protection.
	(void)label; (void)lease; (void)reject;
	return true;
#endif
}

struct LabelCaptureVmDenials
{
	static constexpr uint32_t MaxAttempts = 16;
	std::array<uint32_t, static_cast<size_t>(Core::VirtualMemory::GuestDataLeaseDenialReason::Count)> counts {};
	uint32_t attempts = 0, failures = 0, unclassified = 0, discarded = 0, last_failure_attempt = 0;
	Core::VirtualMemory::GuestDataLeaseDenialMetadata last;

	void Observe(bool acquired, const Core::VirtualMemory::GuestDataLeaseDenialMetadata& denial)
	{
		if (attempts == MaxAttempts) { ++discarded; return; }
		++attempts;
		if (acquired) return;
		++failures;
		last = denial;
		last_failure_attempt = attempts;
		const auto category = static_cast<size_t>(denial.reason);
		if (category == 0 || category >= counts.size()) ++unclassified;
		else ++counts[category];
	}
};

struct LabelCaptureVmAcquire
{
	LabelCaptureVmDenials* denials;
	bool operator()(uint64_t begin, uint64_t bytes, Core::VirtualMemory::Mode mode,
	                Core::VirtualMemory::GuestDataLease* lease) const
	{
		Core::VirtualMemory::GuestDataLeaseDenialMetadata denial;
		const bool acquired = Core::VirtualMemory::TryAcquireGuestDataLease(begin, bytes, mode, lease, false, &denial);
		denials->Observe(acquired, denial);
		return acquired;
	}
};

struct LabelDeferredCaptureResult
{
	bool captured = false;
	uint32_t rounds = 0, total_attempts = 0;
	GraphicsGuestLeaseResult acquisition;
	LabelCaptureVmDenials denials;
};

template <typename Acquire, typename Current, typename Gap>
static LabelDeferredCaptureResult CaptureLabelDeferredWithContinuation(const LabelDeferredCaptureTicket& original,
                                                                       Acquire acquire, Current current, Gap gap)
{
	LabelDeferredCaptureResult result;
	const auto reject = [&](const char* reason) { result.acquisition.reason = reason; return result; };
	if (original.Generation() == UINT64_MAX) return reject("mapping-generation-unavailable");
	if (!original.MatchesRecording(current())) return reject("deferred-label-recording-changed");
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	for (uint32_t round = 1; round <= 16; ++round)
	{
		if (CurrentThreadHasMemoryLease()) return reject("retry-memory-lease-held");
		result.rounds = round;
		result.denials = {};
		GuestDataLease lease;
		const bool captured = TryAcquireGraphicsGuestContinuityLease(original.Address(), original.Size(), Mode::Write,
		    original.Generation(), &lease, &result.acquisition,
		    [&](uint64_t address, uint64_t bytes, Mode mode, GuestDataLease* output) {
			    GuestDataLeaseDenialMetadata denial;
			    const bool acquired = acquire(address, bytes, mode, output, &denial);
			    result.denials.Observe(acquired, denial);
			    return acquired;
		    });
		result.total_attempts += result.acquisition.attempts;
		ReleaseGuestDataLease(&lease);
		if (!original.MatchesRecording(current())) return reject("deferred-label-recording-changed");
		if (captured) { result.captured = true; return result; }
		// Admission can be transiently closed by an active VM transaction.
		// Retain the original continuity token across the bounded pause: a
		// completed mutation of this destination still fails the next proof.
		// Rights, pending writers, unknown and malformed leases stay terminal.
		if (result.acquisition.reason == nullptr ||
		    std::strcmp(result.acquisition.reason, "lease-acquisition-retry-exhausted") != 0 ||
		    result.acquisition.attempts != LabelCaptureVmDenials::MaxAttempts ||
		    result.denials.attempts != LabelCaptureVmDenials::MaxAttempts ||
		    result.denials.failures != LabelCaptureVmDenials::MaxAttempts ||
		    result.denials.unclassified != 0 || result.denials.discarded != 0 ||
		    result.denials.counts[static_cast<size_t>(GuestDataLeaseDenialReason::GateTrylockUnavailable)] +
		        result.denials.counts[static_cast<size_t>(GuestDataLeaseDenialReason::GateMutationActive)] !=
		        LabelCaptureVmDenials::MaxAttempts || round == 16) return result;
		if (CurrentThreadHasMemoryLease()) return reject("retry-memory-lease-held");
		gap();
		if (!original.MatchesRecording(current())) return reject("deferred-label-recording-changed");
	}
#else
	(void)acquire; (void)gap;
#endif
	return result;
}

bool LabelCaptureDeferredDestination(CommandBuffer* buffer, uint64_t address, uint64_t natural_bytes,
                                     const LabelGpuObject& description, LabelDeferredCaptureTicket* ticket,
                                     const char** reason)
{
	if (reason != nullptr) *reason = nullptr;
	if (ticket == nullptr) { if (reason != nullptr) *reason = "deferred-label-ticket-unavailable"; return false; }
	*ticket = {};
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	if (buffer == nullptr) return true;
	auto* cp = buffer->GetParent();
	const auto recording = GraphicsRunCommandProcessorDeferredLabelIdentity(cp, buffer);
	if (!recording.valid) return true;
	const auto reject = [&](const char* why) { if (reason != nullptr) *reason = why; return false; };
	if (address == 0 || (natural_bytes != 4 && natural_bytes != 8) || address % natural_bytes != 0 ||
	    address > UINTPTR_MAX || natural_bytes > UINTPTR_MAX - address ||
	    description.type != GpuMemoryObjectType::Label || description.params[LabelGpuObject::PARAM_WRITE_BYTES] != 0 ||
	    (natural_bytes == 4 && description.params[LabelGpuObject::PARAM_VALUE] > UINT32_MAX) ||
	    description.params[LabelGpuObject::PARAM_PREPARATION_ROLE] >
	        static_cast<uint64_t>(LabelGpuObject::PreparationRole::LockFreeWriteBack)) return reject("deferred-label-payload-shape");
	LabelDeferredCaptureTicket original;
	original.m_address = address; original.m_size = natural_bytes;
	original.m_generation = Core::VirtualMemory::GetMutationGeneration();
	original.m_recording = recording;
	std::copy_n(description.params, original.m_params.size(), original.m_params.begin());
	const auto captured = CaptureLabelDeferredWithContinuation(original,
	    [](uint64_t begin, uint64_t bytes, Core::VirtualMemory::Mode mode,
	       Core::VirtualMemory::GuestDataLease* lease, Core::VirtualMemory::GuestDataLeaseDenialMetadata* denial) {
		    return Core::VirtualMemory::TryAcquireGuestDataLease(begin, bytes, mode, lease, false, denial);
	    }, [&] { return GraphicsRunCommandProcessorDeferredLabelIdentity(cp, buffer); }, [cp, &recording] {
		    if (!recording.exclusive)
		    {
			    // A CCB may run beside this DCB. Keep CP locked so neither
			    // parser can rotate or change the current recording. The VM
			    // admission may reopen during this bounded pre-renderer pause.
			    // No lease, callback or GPU effect crosses it; a transaction
			    // that cannot finish without CP still exhausts the same budget.
			    Core::Thread::SleepMicro(1000);
			    return;
		    }
		    // The exact exclusive parser owns CP1 and retains its RunLock. The
		    // renderer calls before any renderer/tracker/manager lock or creation.
		    struct ReleaseProcessorLock
		    {
			    CommandProcessor* cp;
			    explicit ReleaseProcessorLock(CommandProcessor* value): cp(value) { GraphicsRunCommandProcessorUnlock(cp); }
			    ~ReleaseProcessorLock() { GraphicsRunCommandProcessorLock(cp); }
		    } release(cp);
		    Core::Thread::SleepMicro(1000);
	    });
	if (!captured.captured || captured.rounds > 1)
	{
		static std::atomic<uint32_t> records {0};
		if (!captured.captured || records.fetch_add(1, std::memory_order_relaxed) < 32)
			std::fprintf(stderr, "[GFX_LABEL_DEFERRED_INITIAL_CAPTURE] bytes=%" PRIu64 " rounds=%u local_attempts=%u total_attempts=%u"
			    " captured=%u reason=%s original_generation=%" PRIu64 " callback1=%016" PRIx64 " callback2=%016" PRIx64
			    " role=%" PRIu64 " original_serial=%" PRIu64 " processor_retained=%u vm_denial=%s vm_failures=%u vm_gate_trylock=%u"
			    " enrollment=0 gpu_operations=0 callback_effects=0 guest_stores=0 scalar_completion_authority=0\n",
			    natural_bytes, captured.rounds, captured.acquisition.attempts, captured.total_attempts, captured.captured ? 1u : 0u,
			    captured.acquisition.reason != nullptr ? captured.acquisition.reason : "none", original.Generation(),
			    original.Params()[LabelGpuObject::PARAM_CALLBACK_1], original.Params()[LabelGpuObject::PARAM_CALLBACK_2],
			    original.Params()[LabelGpuObject::PARAM_PREPARATION_ROLE], recording.serial, recording.exclusive ? 0u : 1u,
			    Core::VirtualMemory::GuestDataLeaseDenialReasonName(captured.denials.last.reason), captured.denials.failures,
			    captured.denials.counts[static_cast<size_t>(Core::VirtualMemory::GuestDataLeaseDenialReason::GateTrylockUnavailable)]);
	}
	if (!captured.captured) return reject(captured.acquisition.reason != nullptr ? captured.acquisition.reason : "deferred-label-capture-refused");
	*ticket = original;
#else
	(void)buffer; (void)address; (void)natural_bytes; (void)description;
#endif
	return true;
}

template <typename Acquire = GraphicsGuestLeaseAcquire>
static bool CaptureLabelDestination(LabelCallbacks* label, const char** rejection = nullptr,
                                    GraphicsGuestLeaseResult* acquisition_result = nullptr, Acquire acquire = {})
{
	if (acquisition_result != nullptr) *acquisition_result = {};
	const auto reject = [rejection, acquisition_result](const char* reason) {
		if (rejection != nullptr) *rejection = reason;
		if (acquisition_result != nullptr) acquisition_result->reason = reason;
		return false;
	};
	if (LabelDestinationAddress(*label) == 0) return label->write_bytes == 0 || reject("destination-null");
	if ((LabelDestinationAddress(*label) % (label->dst_gpu_addr64 != nullptr ? sizeof(uint64_t) : sizeof(uint32_t))) != 0 ||
	    label->write_bytes < (label->dst_gpu_addr64 != nullptr ? sizeof(uint64_t) : sizeof(uint32_t)) ||
	    label->write_bytes > LabelGpuObject::MAX_WRITE_BYTES) return reject("destination-shape");
	Core::VirtualMemory::GuestDataLease lease;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	// This initial borrow precedes preparation, enrollment and any store. Only
	// its successful checked lease establishes the destination's original token.
	GraphicsGuestLeaseResult result;
	const bool acquired = TryAcquireGraphicsGuestCaptureLease(LabelDestinationAddress(*label), label->write_bytes,
	    Core::VirtualMemory::Mode::Write, &lease, &result, acquire);
	if (acquisition_result != nullptr) *acquisition_result = result;
	if (!acquired) return reject(result.reason);
	label->destination_generation = lease.MutationGeneration();
#else
	(void)acquire;
	if (!BorrowLabelDestination(*label, &lease, rejection)) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	auto seal = std::make_shared<Core::VirtualMemory::GuestDataRangeSeal>();
	if (!Core::VirtualMemory::CaptureGuestDataRangeSeal(lease, seal.get())) return reject("mapping-seal-unavailable");
	label->destination_generation = lease.MutationGeneration();
	label->destination_seal = std::move(seal);
#endif
#endif
	return true;
}

static bool ValidateLabelDestination(const LabelCallbacks& label, Core::VirtualMemory::GuestDataLease* lease,
                                     const char** rejection = nullptr, bool* lease_acquisition_unavailable = nullptr)
{
	if (lease_acquisition_unavailable != nullptr) *lease_acquisition_unavailable = false;
	const auto reject = [rejection](const char* reason) {
		if (rejection != nullptr) *rejection = reason;
		return false;
	};
	if (LabelDestinationAddress(label) == 0) return label.write_bytes == 0 || reject("destination-null");
	const auto alignment = label.dst_gpu_addr64 != nullptr ? sizeof(uint64_t) : sizeof(uint32_t);
	if (label.write_bytes < alignment || label.write_bytes > LabelGpuObject::MAX_WRITE_BYTES ||
	    LabelDestinationAddress(label) % alignment != 0 ||
	    LabelDestinationAddress(label) > UINT64_MAX - label.write_bytes) return reject("destination-shape");
	if (!BorrowLabelDestination(label, lease, rejection, lease_acquisition_unavailable)) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	return Core::VirtualMemory::ValidateGuestDataLeaseRangeContinuity(*lease, LabelDestinationAddress(label),
	    label.write_bytes, label.destination_generation) || reject("mapping-continuity-changed");
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	return (label.destination_seal != nullptr &&
	    Core::VirtualMemory::ValidateGuestDataRangeSeal(*lease, *label.destination_seal)) || reject("mapping-continuity-changed");
#else
	return true;
#endif
}

struct LabelPublicationFailure
{
	const char* phase = "preflight";
	const char* reason = "unknown";
	uint64_t observed_generation = UINT64_MAX;
	uint32_t attempts = 0;
	uint32_t preflight_attempts = 0;
	uint32_t publication_attempts = 0;
};

static const char* LabelPublicationTicketRejection(const LabelCallbacks& label)
{
	if (label.deferred_capture.IsCaptured())
	{
		const auto& original = label.deferred_capture;
		const auto& current = label.delivery_identity;
		if (!LabelDeferredPayloadMatches(label, original) || label.destination_generation != original.generation ||
		    current.identity_ambiguous || current.cp != original.processor || current.queue != original.queue ||
		    current.session_epoch != original.epoch || current.submit_serial != original.serial)
			return "deferred-capture-delivery-changed";
	}
	if (label.scalar_capture.IsCaptured() &&
	    (!LabelScalarCapturePayloadMatches(label, label.scalar_capture) ||
	     label.destination_generation != label.scalar_capture.Generation() ||
	     !label.scalar_capture.MatchesPublication(label.delivery_identity))) return "scalar-capture-delivery-changed";
	if (label.publication == nullptr) return nullptr;
	if (label.publication->cancelled.load(std::memory_order_acquire) != 0) return "publication-cancelled";
	if (label.publication_sequence == 0 ||
	    label.publication->completed.load(std::memory_order_acquire) != label.publication_sequence - 1)
		return "publication-sequence";
	return nullptr;
}

class LabelCallbackPreparationScope
{
public:
	explicit LabelCallbackPreparationScope(const LabelCallbacks& delivery): m_previous(g_label_writeback_scope)
	{
		++g_label_preparation_depth;
		g_label_writeback_scope = nullptr;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		const auto& identity = delivery.delivery_identity;
		if (g_label_worker_delivery_depth == 1 && g_label_preparation_depth == 1 &&
		    delivery.preparation_role == LabelGpuObject::PreparationRole::LockFreeWriteBack && delivery.publication != nullptr &&
		    identity.cp != nullptr && identity.queue >= 0 && identity.session_epoch != 0 && identity.submit_serial != 0 &&
		    identity.instance_id != 0 && !identity.identity_ambiguous && delivery.publication_sequence != 0 &&
		    LabelDestinationAddress(delivery) != 0 && delivery.write_bytes != 0 && delivery.destination_generation != UINT64_MAX)
		{
			m_scope = std::make_unique<LabelWriteBackProofScope>();
			m_scope->delivery = delivery;
			g_label_writeback_scope = m_scope.get();
		}
#else
		(void)delivery; // No managed-lease capability or witness allocation.
#endif
	}
	~LabelCallbackPreparationScope()
	{
		g_label_writeback_scope = m_previous;
		--g_label_preparation_depth;
	}
	LUMEN_CLASS_NO_COPY(LabelCallbackPreparationScope);
private:
	const LabelWriteBackProofScope* m_previous;
	std::unique_ptr<LabelWriteBackProofScope> m_scope;
};

const LabelWriteBackProofScope* LabelGetActiveWriteBackProofScope(CommandProcessor* cp)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	if (cp == nullptr || g_label_worker_delivery_depth != 1 || g_label_preparation_depth != 1 ||
	    g_label_writeback_scope == nullptr || Core::VirtualMemory::CurrentThreadHasMemoryLease()) return nullptr;
	return g_label_writeback_scope->delivery.delivery_identity.cp == cp ? g_label_writeback_scope : nullptr;
#else
	(void)cp;
	return nullptr;
#endif
}

bool LabelValidateWriteBackProofScope(const LabelWriteBackProofScope* scope, CommandProcessor* cp,
                                     GraphicsGuestLeaseResult* result)
{
	if (result != nullptr) *result = {};
	const auto reject = [result](const char* reason) {
		if (result != nullptr) result->reason = reason;
		return false;
	};
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	// A foreign/stale pointer is never dereferenced. The opaque pointer may be
	// borrowed only by this synchronous callback, never by a later delivery.
	if (scope == nullptr || scope != g_label_writeback_scope || cp == nullptr ||
	    g_label_worker_delivery_depth != 1 || g_label_preparation_depth != 1)
		return reject("label-writeback-scope-inactive");
	if (Core::VirtualMemory::CurrentThreadHasMemoryLease()) return reject("label-writeback-memory-lease-held");
	const auto& delivery = scope->delivery;
	const auto& original = delivery.delivery_identity;
	if (original.cp != cp || delivery.preparation_role != LabelGpuObject::PreparationRole::LockFreeWriteBack ||
	    delivery.publication == nullptr || original.instance_id == 0 || original.identity_ambiguous ||
	    original.queue < 0 || original.session_epoch == 0 || original.submit_serial == 0)
		return reject("label-writeback-producer-identity");
	const auto current = SnapshotLabelPublication(*delivery.publication);
	if (current.instance_id != original.instance_id || current.identity_ambiguous || current.cp != original.cp ||
	    current.queue != original.queue || current.session_epoch != original.session_epoch ||
	    current.submit_serial != original.submit_serial || current.queued != original.queued)
		return reject("label-writeback-ticket-identity-changed");
	if (delivery.publication_sequence == 0 || current.queued < delivery.publication_sequence || current.cancelled != 0 ||
	    current.completed != delivery.publication_sequence - 1)
		return reject("label-writeback-ticket-sequence-changed");
	const uint64_t address = LabelDestinationAddress(delivery);
	const uint64_t alignment = delivery.dst_gpu_addr64 != nullptr ? sizeof(uint64_t) : sizeof(uint32_t);
	if (address == 0 || address % alignment != 0 || delivery.write_bytes < alignment ||
	    delivery.write_bytes > LabelGpuObject::MAX_WRITE_BYTES || address > UINT64_MAX - delivery.write_bytes ||
	    delivery.destination_generation == UINT64_MAX) return reject("label-writeback-destination-shape");
	Core::VirtualMemory::GuestDataLease target;
	return TryAcquireGraphicsGuestContinuityLease(address, delivery.write_bytes, Core::VirtualMemory::Mode::Write,
	    delivery.destination_generation, &target, result);
#else
	(void)scope; (void)cp;
	return reject("label-writeback-lease-capability-unavailable");
#endif
}

// Retry only this acquisition phase. The detached delivery, original VM
// generation and host staging remain unchanged; no callback is replayed.
// The worker owns no manager/processor/tracker lock at this boundary.
template <typename Validate, typename Backoff>
static bool ValidateLabelDestinationWithRetry(const LabelCallbacks& label, const char* phase,
    Core::VirtualMemory::GuestDataLease* lease, LabelPublicationFailure* failure, Validate&& validate, Backoff&& backoff,
    uint32_t* phase_attempts)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	constexpr uint32_t max_attempts = 30000;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
#else
	constexpr uint32_t max_attempts = 16;
#endif
	for (uint32_t attempt = 1; attempt <= max_attempts; ++attempt)
	{
		const auto reject = [&](const char* reason, uint64_t generation = UINT64_MAX) {
			if (failure != nullptr) *failure = {phase, reason, generation, attempt};
			return false;
		};
		if (const char* reason = LabelPublicationTicketRejection(label); reason != nullptr) return reject(reason);
		const char* reason = "unknown";
		bool lease_acquisition_unavailable = false;
		if (validate(label, lease, &reason, &lease_acquisition_unavailable))
		{
			if (const char* ticket_reason = LabelPublicationTicketRejection(label); ticket_reason != nullptr)
			{
				const auto observed = lease->IsActive() ? lease->MutationGeneration() : UINT64_MAX;
				Core::VirtualMemory::ReleaseGuestDataLease(lease);
				return reject(ticket_reason, observed);
			}
			if (phase_attempts != nullptr) *phase_attempts = attempt;
			return true;
		}
		const auto observed = lease->IsActive() ? lease->MutationGeneration() : UINT64_MAX;
		Core::VirtualMemory::ReleaseGuestDataLease(lease);
		if (!lease_acquisition_unavailable) return reject(reason, observed);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		if (label.destination_generation == UINT64_MAX) return reject("mapping-generation-unavailable", observed);
		if (Core::VirtualMemory::CurrentThreadHasMemoryLease()) return reject("retry-memory-lease-held", observed);
		const auto continuity_rejection = [&]() -> const char* {
			Core::VirtualMemory::GuestDataLease continuity;
			GraphicsGuestLeaseResult result;
			// This probe never authorizes a store and requests only destination
			// rights. A known coverage/protection/lifetime refusal is terminal;
			// an unavailable borrow leaves admission to the next worker attempt.
			if (TryAcquireGraphicsGuestContinuityLease(LabelDestinationAddress(label), label.write_bytes,
			    Core::VirtualMemory::Mode::Write, label.destination_generation, &continuity, &result)) return nullptr;
			return result.reason != nullptr && std::strcmp(result.reason, "lease-acquisition-retry-exhausted") == 0
			    ? nullptr : result.reason;
		};
		if (const char* continuity_reason = continuity_rejection(); continuity_reason != nullptr) return reject(continuity_reason, observed);
		if (const char* ticket_reason = LabelPublicationTicketRejection(label); ticket_reason != nullptr)
			return reject(ticket_reason, observed);
		if (attempt == max_attempts) return reject("lease-acquisition-retry-exhausted", observed);
		backoff();
		if (Core::VirtualMemory::CurrentThreadHasMemoryLease()) return reject("retry-memory-lease-held", observed);
		if (const char* continuity_reason = continuity_rejection(); continuity_reason != nullptr) return reject(continuity_reason, observed);
		if (const char* ticket_reason = LabelPublicationTicketRejection(label); ticket_reason != nullptr)
			return reject(ticket_reason, observed);
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		if (Core::VirtualMemory::CurrentThreadHasMemoryLease()) return reject("retry-memory-lease-held", observed);
		if (attempt == max_attempts || std::chrono::steady_clock::now() >= deadline)
			return reject("lease-acquisition-retry-exhausted", observed);
		backoff();
		if (Core::VirtualMemory::CurrentThreadHasMemoryLease()) return reject("retry-memory-lease-held", observed);
#else
		return reject(reason, observed);
#endif
	}
	return false;
}

template <typename Validate, typename Backoff>
static bool TryFireLabelCallbacksWithLeaseRetry(const LabelCallbacks& label, LabelPublicationFailure* failure,
                                               Validate&& validate, Backoff&& backoff)
{
	const auto reject = [failure](const char* phase, const char* reason, uint64_t generation = UINT64_MAX) {
		if (failure != nullptr) *failure = {phase, reason, generation};
		return false;
	};
	if (label.write_bytes > LabelGpuObject::MAX_WRITE_BYTES) return reject("preflight", "write-size");
	uint32_t preflight_attempts = 0, publication_attempts = 0;
	// Reject stale/RO/unowned destinations before preparation can cause effects.
	// With no preparation callback, only private host staging precedes the final
	// publication proof below. That one fresh lease covers the complete store.
	// No lease survives preparation, writeback or any GPU wait.
	if (label.callback_1 != nullptr)
	{
		Core::VirtualMemory::GuestDataLease preflight;
		if (!ValidateLabelDestinationWithRetry(label, "preflight", &preflight, failure, validate, backoff, &preflight_attempts))
			return false;
	}
	uint64_t scalar = label.dst_gpu_addr64 != nullptr ? label.value64 : label.value32;
	std::vector<uint64_t> range;
	if (label.write_bytes > sizeof(scalar)) range.resize((label.write_bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0);
	void* output = label.write_bytes == 0 ? nullptr : (range.empty() ? static_cast<void*>(&scalar) : range.data());
	if (label.write_bytes != 0)
	{
		if (label.dst_gpu_addr64 != nullptr) std::memcpy(output, &label.value64, sizeof(label.value64));
		else std::memcpy(output, &label.value32, sizeof(label.value32));
	}
	bool write = true;
	if (label.callback_1 != nullptr)
	{
		// Preflight has released its lease. The witness ends before final
		// publication and never changes callback false's existing skip-store role.
		LabelCallbackPreparationScope preparation(label);
		write = label.callback_1(label.args, output, label.write_bytes);
	}
	{
		Core::VirtualMemory::GuestDataLease publication;
		if (!ValidateLabelDestinationWithRetry(label, "publication", &publication, failure, validate, backoff, &publication_attempts))
			return false;
		if (write && label.write_bytes != 0)
		{
			// The final admitted lease stabilizes the backing. Declare the actual
			// store explicitly even if this delivery happens on an epoch owner
			// whose automatic Core checks are bypassed. No Label manager is held.
			if (!GraphicsRenderPrepareCpuMemoryAccess(LabelDestinationAddress(label), label.write_bytes,
			    GuestCpu::MemoryAccessIntent::Write)) return reject("publication", "cpu-gpu-write-conflict");
			if (label.dst_gpu_addr64 != nullptr && label.write_bytes == sizeof(uint64_t)) *reinterpret_cast<uint64_t*>(LabelDestinationAddress(label)) = scalar;
			else if (label.dst_gpu_addr32 != nullptr && label.write_bytes == sizeof(uint32_t)) *reinterpret_cast<uint32_t*>(LabelDestinationAddress(label)) = static_cast<uint32_t>(scalar);
			else std::memcpy(reinterpret_cast<void*>(LabelDestinationAddress(label)), output, label.write_bytes);
		}
	}
	if (write && label.write_bytes != 0 && Config::GetPrintfDirection() != Log::Direction::Silent)
		printf(FG_BRIGHT_GREEN "EndOfPipe Signal!!! [0x%016" PRIx64 "] bytes=%" PRIu64 "\n" FG_DEFAULT,
		       LabelDestinationAddress(label), label.write_bytes);
	// Notify after the last store, without a VM lease or subsystem lock held.
	if (label.callback_2 != nullptr) label.callback_2(label.args, nullptr, 0);
	if (label.publication != nullptr && !CompleteLabelPublication(label.publication.get(), label.publication_sequence))
		return reject("completion", "publication-completion");
	if (failure != nullptr)
	{
		failure->preflight_attempts = preflight_attempts;
		failure->publication_attempts = publication_attempts;
	}
	return true;
}

static bool TryFireLabelCallbacks(const LabelCallbacks& label, LabelPublicationFailure* failure = nullptr)
{
	return TryFireLabelCallbacksWithLeaseRetry(label, failure, ValidateLabelDestination,
	    [] { Core::Thread::SleepMicro(1000); });
}

static void ReportLabelPublicationFailure(const LabelCallbacks& label, const LabelPublicationFailure& failure)
{
	// All validation/publication leases ended with the helper. Instance ID is
	// immutable; only atomic ticket fields are read outside the manager lock.
	static std::atomic<uint32_t> records {0};
	if (records.fetch_add(1, std::memory_order_relaxed) < 32)
		std::fprintf(stderr, "[GFX_LABEL_PUBLICATION_BLOCK] phase=%s reason=%s"
		                     " destination=0x%016" PRIx64 " bytes=%" PRIu64 " expected_generation=%" PRIu64
		                     " observed_generation=%" PRIu64 " current_generation=%" PRIu64
		                     " publication_instance=%" PRIu64 " sequence=%" PRIu64
		                     " completed=%" PRIu64 " cancelled=%" PRIu64 " phase_attempts=%u"
		                     " callback1_present=%u callback2_present=%u preparation_role=%" PRIu64
		                     " scalar64=%u write_value=0x%016" PRIx64 " scalar_capture=%u\n",
		             failure.phase, failure.reason, LabelDestinationAddress(label), label.write_bytes,
		             label.destination_generation, failure.observed_generation, Core::VirtualMemory::GetMutationGeneration(),
		             label.publication != nullptr ? label.publication->identity.instance_id : 0, label.publication_sequence,
		             label.publication != nullptr ? label.publication->completed.load(std::memory_order_acquire) : 0,
		             label.publication != nullptr ? label.publication->cancelled.load(std::memory_order_acquire) : 0,
		             failure.attempts, label.callback_1 != nullptr ? 1u : 0u, label.callback_2 != nullptr ? 1u : 0u,
		             static_cast<uint64_t>(label.preparation_role), label.dst_gpu_addr64 != nullptr ? 1u : 0u,
		             label.dst_gpu_addr64 != nullptr ? label.value64 : label.value32, label.scalar_capture.IsCaptured() ? 1u : 0u);
	std::fflush(stderr);
}

static void ReportLabelCaptureVmDenials(const LabelCallbacks& label, CommandBuffer* buffer,
                                       const LabelCaptureVmDenials& denials)
{
	// Capture's complete local acquisition ended before this metadata-only
	// report. Existing manager/caller locks remain unchanged; no VM query runs.
	static std::atomic<uint32_t> records {0};
	if (records.fetch_add(1, std::memory_order_relaxed) >= 32) return;
	const auto& last = denials.last;
	const auto count = [&](Core::VirtualMemory::GuestDataLeaseDenialReason reason) {
		return denials.counts[static_cast<size_t>(reason)];
	};
	using Reason = Core::VirtualMemory::GuestDataLeaseDenialReason;
	std::fprintf(stderr, "[GFX_LABEL_CAPTURE_VM_DENIAL] destination=0x%016" PRIx64 " bytes=%" PRIu64
	    " required_mode=%u original_token=%" PRIu64 " attempts=%u failures=%u unclassified=%u discarded=%u"
	    " invalid=%u trylock=%u mutation_active=%u writer_pending=%u reader_overflow=%u"
	    " logical_snapshot=%u logical_registry=%u physical=%u capability=%u"
	    " last_failure_attempt=%u last_phase=%s last_reason=%s last_address=0x%016" PRIx64
	    " last_bytes=%" PRIu64 " last_mode=%u acquisition_thread=0x%016" PRIx64 " generation_gauge=%" PRIu64
	    " gate_observed=%u gate_mutation_active=%u waiting_mutations=%" PRIu64 " code_readers=%" PRIu64
	    " physical_observed=%u physical_result=%d physical_protection=%u"
	    " cp=0x%016" PRIx64 " buffer=0x%016" PRIx64 " pool=0x%016" PRIx64
	    " buffer_index=%u queue=%d recording=%" PRIu64 " epoch=%" PRIu64 " serial=%" PRIu64 " ordinal=%" PRIu64
	    " publication_instance=%" PRIu64 " sequence=%" PRIu64
	    " callback1=0x%016" PRIx64 " callback2=0x%016" PRIx64 " preparation_role=%" PRIu64
	    " scalar64=%u write_value=0x%016" PRIx64 " arg0=0x%016" PRIx64 " arg1=0x%016" PRIx64
	    " observation_only=1 retry_authority=0\n",
	    LabelDestinationAddress(label), label.write_bytes, static_cast<uint32_t>(Core::VirtualMemory::Mode::Write), UINT64_MAX,
	    denials.attempts, denials.failures, denials.unclassified, denials.discarded,
	    count(Reason::LeaseOrRangeInvalid), count(Reason::GateTrylockUnavailable), count(Reason::GateMutationActive),
	    count(Reason::GateWriterPending), count(Reason::GateReaderOverflow), count(Reason::LogicalSnapshotDenied),
	    count(Reason::LogicalRegistryDenied), count(Reason::PhysicalQueryOrRightsDenied), count(Reason::MappingLeaseCapabilityUnavailable),
	    denials.last_failure_attempt, Core::VirtualMemory::GuestDataLeaseDenialPhaseName(last.reason),
	    Core::VirtualMemory::GuestDataLeaseDenialReasonName(last.reason), last.address, last.bytes, static_cast<uint32_t>(last.required),
	    static_cast<uint64_t>(last.acquisition_thread), last.generation, last.gate_observed ? 1u : 0u,
	    last.mutation_active ? 1u : 0u, last.waiting_mutations, last.code_readers, last.physical_observed ? 1u : 0u,
	    last.physical_result, last.physical_protection, reinterpret_cast<uint64_t>(buffer->GetParent()),
	    reinterpret_cast<uint64_t>(buffer), reinterpret_cast<uint64_t>(buffer->GetPool()), buffer->GetIndex(), buffer->GetQueue(),
	    buffer->GetRecordingId(), buffer->GetSubmissionSessionEpoch(), buffer->GetRecordingSubmitSerial(), buffer->GetTrackedCommandOrdinal(),
	    label.publication != nullptr ? label.publication->identity.instance_id : 0, label.publication_sequence,
	    reinterpret_cast<uint64_t>(label.callback_1), reinterpret_cast<uint64_t>(label.callback_2), static_cast<uint64_t>(label.preparation_role),
	    label.dst_gpu_addr64 != nullptr ? 1u : 0u, label.dst_gpu_addr64 != nullptr ? label.value64 : label.value32, label.args[0], label.args[1]);
}

static void FireLabelCallbacks(const LabelCallbacks& label)
{
	LabelPublicationFailure failure;
	if (!TryFireLabelCallbacks(label, &failure))
	{
		ReportLabelPublicationFailure(label, failure);
		EXIT("Label deferred destination rejected before publication\n");
	}
	else if (failure.preflight_attempts > 1 || failure.publication_attempts > 1)
	{
		static std::atomic<uint32_t> recovered_records {0};
		if (recovered_records.fetch_add(1, std::memory_order_relaxed) < 32)
			std::fprintf(stderr, "[GFX_LABEL_LEASE_RECOVERED] preflight_attempts=%u publication_attempts=%u"
			                     " expected_generation=%" PRIu64 " sequence=%" PRIu64 " callbacks_replayed=0\n",
			             failure.preflight_attempts, failure.publication_attempts, label.destination_generation,
			             label.publication_sequence);
	}
}

template <typename Fire>
static void DeliverLabelWorkerCallbacksWithWitness(const LabelCallbacks& callbacks, Fire&& fire)
{
	struct WorkerDelivery
	{
		WorkerDelivery() { ++g_label_worker_delivery_depth; }
		~WorkerDelivery() { --g_label_worker_delivery_depth; }
	} worker;
	fire(callbacks);
}

static void DeliverLabelWorkerCallbacks(const LabelCallbacks& callbacks)
{
	DeliverLabelWorkerCallbacksWithWitness(callbacks, [](const LabelCallbacks& delivery) { FireLabelCallbacks(delivery); });
}

bool LabelManager::GetPublicationSnapshot(Label* label, LabelPublicationSnapshot* snapshot,
                                         LabelPublicationHostMetadata* diagnostic)
{
	if (diagnostic != nullptr) *diagnostic = {};
	if (snapshot == nullptr) return false;
	*snapshot = {};
	Core::LockGuard lock(m_mutex);
	label = m_labels.Find(label);
	if (m_session_resetting || label == nullptr || label->callbacks.publication == nullptr) return false;
	*snapshot = SnapshotLabelPublication(*label->callbacks.publication);
	if (diagnostic != nullptr)
	{
		const auto& delivery = label->callbacks;
		diagnostic->manager_identity = reinterpret_cast<uint64_t>(this);
		diagnostic->publication_identity = reinterpret_cast<uint64_t>(delivery.publication.get());
		diagnostic->registered_sequence = delivery.publication_sequence;
		diagnostic->registered_destination = LabelDestinationAddress(delivery);
		diagnostic->registered_write_bytes = delivery.write_bytes;
		diagnostic->registered_destination_generation = delivery.destination_generation;
		diagnostic->registered_callback_1 = reinterpret_cast<uint64_t>(delivery.callback_1);
		diagnostic->registered_callback_2 = reinterpret_cast<uint64_t>(delivery.callback_2);
		diagnostic->registered_preparation_role = static_cast<uint64_t>(delivery.preparation_role);
	}
	return true;
}

bool LabelGetPublicationSnapshot(Label* label, LabelPublicationSnapshot* snapshot,
                                 LabelPublicationHostMetadata* diagnostic)
{
	if (snapshot != nullptr) *snapshot = {};
	if (diagnostic != nullptr) *diagnostic = {};
	return g_label_manager != nullptr && g_label_manager->GetPublicationSnapshot(label, snapshot, diagnostic);
}

bool LabelManager::CapturePublicationCompletionTicket(Label* label, const LabelPublicationSnapshot& original,
                                                     LabelPublicationCompletionTicket* ticket)
{
	if (ticket == nullptr) return false;
	ticket->m_state.reset();
	if (original.instance_id == 0 || original.cp == nullptr || original.queue < 0 || original.session_epoch == 0 ||
	    original.submit_serial == 0 || original.identity_ambiguous || original.cancelled != 0 ||
	    original.completed >= original.queued || original.pending != original.queued - original.completed) return false;
	Core::LockGuard lock(m_mutex);
	label = m_labels.Find(label);
	if (m_session_resetting || label == nullptr || label->callbacks.publication == nullptr) return false;
	auto retained = std::make_shared<LabelPublicationCompletionTicketState>();
	retained->publication = label->callbacks.publication;
	retained->label = label;
	retained->manager = this;
	retained->original = original;
	retained->last_completed = original.completed;
	if (ValidateLabelCompletionSnapshot(retained.get(), SnapshotLabelPublication(*retained->publication)) != nullptr) return false;
	ticket->m_state = std::move(retained);
	return true;
}

bool LabelManager::ValidatePublicationCompletionTicket(const LabelPublicationCompletionTicket& ticket,
                                                      LabelPublicationSnapshot* current, const char** reason)
{
	if (current != nullptr) *current = {};
	if (reason != nullptr) *reason = nullptr;
	const auto reject = [reason](const char* value) { if (reason != nullptr) *reason = value; return false; };
	if (current == nullptr || ticket.m_state == nullptr || ticket.m_state->manager != this)
		return reject("label-completion-ticket-unavailable");
	Core::LockGuard lock(m_mutex);
	const auto& retained = *ticket.m_state;
	// Registry lookup precedes any Label dereference. The shared publication
	// state also retains the exact detached delivery after a worker snapshot.
	const auto* label = m_labels.Find(retained.label);
	if (m_session_resetting || label == nullptr || label->callbacks.publication != retained.publication)
		return reject("label-completion-ticket-membership-changed");
	*current = SnapshotLabelPublication(*retained.publication);
	if (const char* value = ValidateLabelCompletionSnapshot(ticket.m_state.get(), *current); value != nullptr) return reject(value);
	return true;
}

bool LabelCapturePublicationCompletionTicket(Label* label, const LabelPublicationSnapshot& original,
                                            LabelPublicationCompletionTicket* ticket)
{
	return g_label_manager != nullptr && g_label_manager->CapturePublicationCompletionTicket(label, original, ticket);
}

bool LabelValidatePublicationCompletionTicket(const LabelPublicationCompletionTicket& ticket,
                                             LabelPublicationSnapshot* current, const char** reason)
{
	if (g_label_manager == nullptr)
	{
		if (current != nullptr) *current = {};
		if (reason != nullptr) *reason = "label-completion-ticket-manager-unavailable";
		return false;
	}
	return g_label_manager->ValidatePublicationCompletionTicket(ticket, current, reason);
}

bool LabelManager::CaptureScalarPublicationCompletionTicket(Label* label, const LabelPublicationSnapshot& original,
    LabelScalarPublicationCompletionTicket* ticket, LabelScalarPublicationFootprint* footprint,
    GraphicsGuestLeaseResult* mapping_result, const char** reason)
{
	if (footprint != nullptr) *footprint = {};
	if (mapping_result != nullptr) *mapping_result = {};
	if (reason != nullptr) *reason = nullptr;
	const auto reject = [reason](const char* value) { if (reason != nullptr) *reason = value; return false; };
	if (ticket == nullptr) return reject("scalar-completion-ticket-unavailable");
	ticket->m_state.reset();
	if (footprint == nullptr || original.instance_id == 0 || original.cp == nullptr || original.queue < 0 ||
	    original.session_epoch == 0 || original.submit_serial == 0 || original.identity_ambiguous ||
	    original.queued != 1 || original.completed != 0 || original.cancelled != 0 || original.pending != 1)
		return reject("scalar-completion-original-enrollment-unqualified");
	Core::LockGuard lock(m_mutex);
	label = m_labels.Find(label);
	if (m_session_resetting || label == nullptr || label->callbacks.publication == nullptr)
		return reject("scalar-completion-membership-unavailable");
	const auto& publication = label->callbacks.publication;
	if (!publication->scalar_first_delivery_sealed) return reject("scalar-completion-seal-unavailable");
	LabelScalarPublicationFootprint registered;
	if (!DescribeFirstScalarDelivery(label->callbacks, &registered) ||
	    !ScalarDeliverySealEqual(registered, publication->scalar_first_delivery) ||
	    !LabelPublicationEnrollmentEqual(original, registered.enrollment))
		return reject("scalar-completion-delivery-changed");
	auto retained = std::make_shared<LabelScalarPublicationCompletionTicketState>();
	retained->publication = publication;
	retained->label = label;
	retained->manager = this;
	retained->delivery = publication->scalar_first_delivery;
	const auto current = SnapshotLabelPublication(*publication);
	if (const char* value = ValidateScalarCompletionSnapshot(retained.get(), current); value != nullptr) return reject(value);
	if (!ScalarDeliveryMappingContinuous(retained->delivery, mapping_result, reason)) return false;
	// Retain the source Set token even when the checked lease has a newer gauge.
	retained->last_completed = current.completed;
	*footprint = retained->delivery;
	ticket->m_state = std::move(retained);
	return true;
}

bool LabelManager::ValidateScalarPublicationCompletionTicket(const LabelScalarPublicationCompletionTicket& ticket,
    LabelPublicationSnapshot* current, GraphicsGuestLeaseResult* mapping_result, const char** reason)
{
	if (current != nullptr) *current = {};
	if (mapping_result != nullptr) *mapping_result = {};
	if (reason != nullptr) *reason = nullptr;
	const auto reject = [reason](const char* value) { if (reason != nullptr) *reason = value; return false; };
	if (current == nullptr || ticket.m_state == nullptr || ticket.m_state->manager != this)
		return reject("scalar-completion-ticket-unavailable");
	Core::LockGuard lock(m_mutex);
	const auto& retained = *ticket.m_state;
	// Membership precedes every Label dereference; a retained publication does
	// not allow pointer reuse, replacement or detached registry membership.
	const auto* label = m_labels.Find(retained.label);
	if (m_session_resetting || label == nullptr || label->callbacks.publication != retained.publication)
		return reject("scalar-completion-membership-changed");
	LabelScalarPublicationFootprint registered;
	if (!retained.publication->scalar_first_delivery_sealed ||
	    !ScalarDeliverySealEqual(retained.delivery, retained.publication->scalar_first_delivery) ||
	    !DescribeFirstScalarDelivery(label->callbacks, &registered) || !ScalarDeliverySealEqual(retained.delivery, registered))
		return reject("scalar-completion-delivery-changed");
	const auto observed = SnapshotLabelPublication(*retained.publication);
	if (const char* value = ValidateScalarCompletionSnapshot(ticket.m_state.get(), observed); value != nullptr) return reject(value);
	if (!ScalarDeliveryMappingContinuous(retained.delivery, mapping_result, reason)) return false;
	ticket.m_state->last_completed = observed.completed;
	*current = observed;
	return true;
}

bool LabelCaptureScalarPublicationCompletionTicket(Label* label, const LabelPublicationSnapshot& original,
    LabelScalarPublicationCompletionTicket* ticket, LabelScalarPublicationFootprint* footprint,
    GraphicsGuestLeaseResult* mapping_result, const char** reason)
{
	if (g_label_manager == nullptr)
	{
		if (ticket != nullptr) *ticket = {};
		if (footprint != nullptr) *footprint = {};
		if (mapping_result != nullptr) *mapping_result = {};
		if (reason != nullptr) *reason = "scalar-completion-manager-unavailable";
		return false;
	}
	return g_label_manager->CaptureScalarPublicationCompletionTicket(label, original, ticket, footprint, mapping_result, reason);
}

bool LabelValidateScalarPublicationCompletionTicket(const LabelScalarPublicationCompletionTicket& ticket,
    LabelPublicationSnapshot* current, GraphicsGuestLeaseResult* mapping_result, const char** reason)
{
	if (g_label_manager == nullptr)
	{
		if (current != nullptr) *current = {};
		if (mapping_result != nullptr) *mapping_result = {};
		if (reason != nullptr) *reason = "scalar-completion-manager-unavailable";
		return false;
	}
	return g_label_manager->ValidateScalarPublicationCompletionTicket(ticket, current, mapping_result, reason);
}

uint32_t LabelScalarPublicationCensusBank::StateCount() const
{
	return m_state != nullptr ? static_cast<uint32_t>(m_state->states.size()) : 0;
}

uint32_t LabelScalarPublicationCensusBank::ScalarCount() const
{
	return m_state != nullptr ? m_state->scalars : 0;
}

uint64_t LabelScalarPublicationCensusBank::RetainedMetadataBytes() const
{
	return m_state != nullptr ? LabelCensusCombinedMetadataBytes(m_state->states.size(), m_state->states.capacity()) : 0;
}

bool LabelScalarPublicationCensusBank::GetScalarEntry(uint32_t index, LabelScalarPublicationCensusEntry* entry) const
{
	if (entry != nullptr) *entry = {};
	if (entry == nullptr || m_state == nullptr || index >= m_state->scalars) return false;
	for (const auto& state: m_state->states)
	{
		if (!state.scalar) continue;
		if (index-- == 0) { *entry = {state.label, state.delivery}; return true; }
	}
	return false;
}

bool LabelManager::ScanPublicationCensusLocked(std::vector<LabelScalarPublicationCensusBankState::Retained>* states,
    const char** reason, uint64_t retained_metadata_budget, const LabelScalarPublicationCensusBankState* retained_bank,
    LabelScalarPublicationCensusCaptureWitness* witness)
{
	states->clear();
	const auto reject = [reason, witness, states](const char* value) {
		if (reason != nullptr) *reason = value;
		if (witness != nullptr && witness->reason == nullptr)
		{
			witness->reason = value;
			witness->state_count = static_cast<uint32_t>(states->size());
		}
		return false;
	};
	if (retained_metadata_budget == 0 ||
	    retained_metadata_budget > LabelScalarPublicationCensusBank::MaxCombinedRetainedMetadataBytes)
		return reject("scalar-census-metadata-budget-invalid");
	const auto account = [&](size_t count) {
		// A bounded geometric capacity makes the original and validation row
		// reservations identical. Check before every map entry/owned row copy;
		// no additional census pass or weak-list upper bound is needed.
		size_t capacity = 1;
		while (capacity < count) capacity *= 2;
		if (count == 0) capacity = 0;
		capacity = std::max(capacity, states->capacity());
		const auto bytes = [&](size_t actual_capacity) {
			return retained_bank != nullptr ?
			    LabelCensusRetainedMetadataBytes(retained_bank->states.size(), retained_bank->states.capacity()) +
			        LabelCensusDiscoveryMetadataBytes(count, actual_capacity) :
			    LabelCensusCombinedMetadataBytes(count, actual_capacity);
		};
		static_assert(sizeof(bytes) + sizeof(LabelPublicationSnapshot) + sizeof(std::shared_ptr<LabelPublicationState>) +
		    8 * sizeof(void*) + 4 * sizeof(size_t) <= LabelCensusFixedDiscoveryScratchBytes);
		if (capacity > LabelScalarPublicationCensusBank::MaxEnrolledStates || bytes(capacity) > retained_metadata_budget)
			return reject("scalar-census-metadata-capacity");
		if (states->capacity() < capacity) states->reserve(capacity);
		// An allocator's actual vector capacity is always part of the quota.
		if (states->capacity() > LabelScalarPublicationCensusBank::MaxEnrolledStates ||
		    bytes(states->capacity()) > retained_metadata_budget) return reject("scalar-census-metadata-capacity");
		return true;
	};
	static_assert(sizeof(account) + sizeof(LabelPublicationSnapshot) + sizeof(LabelScalarPublicationFootprint) +
	    sizeof(std::shared_ptr<LabelPublicationState>) + 8 * sizeof(void*) + 4 * sizeof(size_t) <=
	    LabelCensusFixedDiscoveryScratchBytes);
	if (!account(0)) return false;
	if (m_session_resetting || m_census_epoch_exhausted) return reject("scalar-census-session-unavailable");
	// Both lists are scanned completely. Capacity limits disable this proof,
	// never enrollment, event signaling or the ordinary publication path.
	if (m_labels.Size() > LabelScalarPublicationCensusBank::MaxVisitedStates ||
	    m_publications.size() > LabelScalarPublicationCensusBank::MaxVisitedStates - m_labels.Size())
		return reject("scalar-census-visit-capacity");
	LabelCensusRegistered registered;
	for (auto* label: m_labels)
	{
		const auto& publication = label->callbacks.publication;
		if (publication == nullptr) return reject("scalar-census-registry-unqualified");
		// queued changes only under this manager mutex; completion is atomic.
		if (publication->queued == 0) continue;
		if (registered.size() == LabelScalarPublicationCensusBank::MaxEnrolledStates)
			return reject("scalar-census-registry-capacity-or-duplicate");
		if (!account(registered.size() + 1)) return false;
		if (!registered.emplace(publication.get(), label).second) return reject("scalar-census-registry-capacity-or-duplicate");
	}
	const auto registered_count = registered.size();
	LabelCensusInstances instances;
	for (const auto& weak: m_publications)
	{
		auto publication = weak.lock();
		// Every copied worker delivery retains its shared publication. Expiry
		// proves absence of such a reference; pending==0 or cancel never does.
		if (publication == nullptr || publication->queued == 0) continue;
		if (states->size() == LabelScalarPublicationCensusBank::MaxEnrolledStates)
			return reject("scalar-census-enrolled-capacity");
		if (!account(std::max(registered_count, states->size() + 1))) return false;
		const auto snapshot = SnapshotLabelPublication(*publication);
		RecordLabelCensusCaptureRow(witness, nullptr, publication.get(), snapshot, static_cast<uint32_t>(states->size()));
		if (snapshot.instance_id == 0 || !instances.emplace(snapshot.instance_id, publication.get()).second)
			return reject("scalar-census-instance-unqualified");
		if (snapshot.cancelled != 0) return reject("scalar-census-cancelled-state");
		const auto member = registered.find(publication.get());
		Label* label = member != registered.end() ? member->second : nullptr;
		if (member != registered.end()) registered.erase(member);
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		if (m_test_instance) ++m_test_census_row_copies;
#endif
		states->push_back({std::move(publication), label, snapshot, {}, snapshot.completed, false});
	}
	if (!registered.empty()) return reject("scalar-census-untracked-enrollment");
	return true;
}

bool LabelManager::CaptureScalarPublicationCensusBank(LabelScalarPublicationCensusBank* bank, const char** reason,
    uint64_t retained_metadata_budget, LabelScalarPublicationCensusCaptureWitness* witness)
{
	if (reason != nullptr) *reason = nullptr;
	if (witness != nullptr) *witness = {};
	const auto reject = [reason, witness](const char* value) {
		if (reason != nullptr) *reason = value;
		if (witness != nullptr && witness->reason == nullptr) witness->reason = value;
		return false;
	};
	if (bank == nullptr) return reject("scalar-census-bank-unavailable");
	bank->m_state.reset();
	Core::LockGuard lock(m_mutex);
	std::vector<LabelScalarPublicationCensusBankState::Retained> states;
	if (!ScanPublicationCensusLocked(&states, reason, retained_metadata_budget, nullptr, witness)) return false;
	if (witness != nullptr)
	{
		*witness = {};
		witness->state_count = static_cast<uint32_t>(states.size());
		witness->discovery_complete = true;
	}
	uint32_t scalars = 0;
	for (auto& state: states)
	{
		if (witness != nullptr)
		{
			// Copy the already discovered row, never another publication snapshot.
			witness->registered = {};
			witness->delivery_enrollment = {};
			witness->sealed_checked = witness->sealed = witness->description_checked = witness->described = false;
			witness->seal_checked = witness->seal_equal = witness->enrollment_checked = witness->enrollment_equal = false;
			RecordLabelCensusCaptureRow(witness, state.label, state.publication.get(), state.original,
			    static_cast<uint32_t>(&state - states.data()));
			witness->registered.manager_identity = reinterpret_cast<uint64_t>(this);
		}
		if (state.original.pending == 0)
		{
			// Completion publishes only after the scalar store and both
			// callbacks returned. Cancelled states were rejected by the census.
			if (state.original.completed != state.original.queued) return reject("scalar-census-incomplete-state");
			continue;
		}
		const auto& original = state.original;
		if (state.label == nullptr || original.queued == 0 || original.completed != original.queued - 1 || original.pending != 1 ||
		    original.cp == nullptr || original.queue < 0 || original.session_epoch == 0 || original.submit_serial == 0 ||
		    original.identity_ambiguous) return reject("scalar-census-pending-state-unqualified");
		const auto& publication = state.publication;
		LabelScalarPublicationFootprint registered;
		const bool sealed = publication->scalar_single_delivery_sealed;
		// Preserve the original left-to-right short circuit and query count.
		const bool described = sealed && DescribeSingleScalarDelivery(state.label->callbacks, &registered);
		const bool seal_equal = described && ScalarDeliverySealEqual(registered, publication->scalar_single_delivery);
		const bool enrollment_equal = seal_equal && LabelPublicationEnrollmentEqual(original, registered.enrollment);
		if (witness != nullptr)
		{
			witness->sealed_checked = true;
			witness->sealed = sealed;
			witness->description_checked = sealed; witness->described = described;
			witness->seal_checked = described; witness->seal_equal = seal_equal;
			witness->enrollment_checked = seal_equal; witness->enrollment_equal = enrollment_equal;
		}
		if (!enrollment_equal)
			return reject("scalar-census-pending-delivery-unqualified");
		state.delivery = publication->scalar_single_delivery;
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		if (m_test_instance) ++m_test_census_mapping_checks;
#endif
		const char* mapping_reason = nullptr;
		const auto mapping_output = reason != nullptr ? reason : witness != nullptr ? &mapping_reason : nullptr;
		if (!ScalarDeliveryMappingContinuous(state.delivery, nullptr, mapping_output))
			return reject(mapping_output != nullptr && *mapping_output != nullptr ? *mapping_output : "scalar-completion-mapping-unproved");
		state.scalar = true;
		++scalars;
		if (witness != nullptr) witness->scalar_count = scalars;
	}
	auto retained = std::make_shared<LabelScalarPublicationCensusBankState>();
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	if (m_test_instance) ++m_test_census_bank_constructions;
#endif
	retained->manager = this;
	retained->enrollment_epoch = m_enrollment_epoch;
	retained->reset_epoch = m_reset_epoch;
	retained->scalars = scalars;
	retained->states = std::move(states);
	bank->m_state = std::move(retained);
	if (bank->RetainedMetadataBytes() > retained_metadata_budget)
	{
		bank->m_state.reset();
		return reject("scalar-census-metadata-capacity");
	}
	if (witness != nullptr)
	{
		// Success has no offending row; retain only original whole-pass counts.
		*witness = {};
		witness->complete = true;
		witness->discovery_complete = true;
		witness->state_count = bank->StateCount(); witness->scalar_count = scalars;
	}
	return true;
}

bool LabelManager::ValidateScalarPublicationCensusBank(const LabelScalarPublicationCensusBank& bank, const char** reason)
{
	if (reason != nullptr) *reason = nullptr;
	const auto reject = [reason](const char* value) { if (reason != nullptr) *reason = value; return false; };
	if (bank.m_state == nullptr || bank.m_state->manager != this) return reject("scalar-census-bank-unavailable");
	Core::LockGuard lock(m_mutex);
	if (m_session_resetting || m_census_epoch_exhausted || bank.m_state->enrollment_epoch != m_enrollment_epoch ||
	    bank.m_state->reset_epoch != m_reset_epoch) return reject("scalar-census-enrollment-or-session-changed");
	std::vector<LabelScalarPublicationCensusBankState::Retained> current;
	if (!ScanPublicationCensusLocked(&current, reason, bank.RetainedMetadataBytes(), bank.m_state.get())) return false;
	if (current.size() != bank.m_state->states.size()) return reject("scalar-census-state-set-changed");
	// Strong references keep every original weak entry alive. Order can only
	// change by adding/removing entries, which this complete re-census rejects.
	for (size_t i = 0; i != current.size(); ++i)
	{
		auto& original = bank.m_state->states[i];
		const auto& observed = current[i];
		const auto report_state = [&](const char* stage, const char* value, bool retirement = false) {
			static std::atomic<uint32_t> rejected_records {0}, retirement_records {0};
			auto& records = retirement ? retirement_records : rejected_records;
			if (records.fetch_add(1, std::memory_order_relaxed) >= (retirement ? 16u : 32u)) return;
			const auto& a = original.original;
			const auto& b = observed.original;
			std::fprintf(stderr, "[GFX_LABEL_SCALAR_CENSUS_STATE] stage=%s reason=%s index=%zu states=%zu"
			    " scalar=%u publication=%p:%p membership=%p:%p instance=%" PRIu64 ":%" PRIu64
			    " queued=%" PRIu64 ":%" PRIu64 " completed=%" PRIu64 ":%" PRIu64
			    " cancelled=%" PRIu64 ":%" PRIu64 " pending=%" PRIu64 ":%" PRIu64
			    " cp=%p:%p queue=%d:%d epoch=%" PRIu64 ":%" PRIu64 " serial=%" PRIu64 ":%" PRIu64
			    " ambiguous=%u:%u enrollment_epoch=%" PRIu64 ":%" PRIu64 " reset_epoch=%" PRIu64 ":%" PRIu64
			    " last_completed=%" PRIu64 " metadata_only=1 guest_reads=0 VM_queries=0 native_queries=0\n",
			    stage, value, i, current.size(), original.scalar ? 1u : 0u,
			    static_cast<void*>(original.publication.get()), static_cast<void*>(observed.publication.get()),
			    static_cast<void*>(original.label), static_cast<void*>(observed.label), a.instance_id, b.instance_id,
			    a.queued, b.queued, a.completed, b.completed, a.cancelled, b.cancelled, a.pending, b.pending,
			    static_cast<void*>(a.cp), static_cast<void*>(b.cp), a.queue, b.queue, a.session_epoch, b.session_epoch,
			    a.submit_serial, b.submit_serial, a.identity_ambiguous ? 1u : 0u, b.identity_ambiguous ? 1u : 0u,
			    bank.m_state->enrollment_epoch, m_enrollment_epoch, bank.m_state->reset_epoch, m_reset_epoch, original.last_completed);
		};
		static_assert(sizeof(report_state) + sizeof(LabelScalarPublicationCompletionTicketState) +
		    sizeof(LabelScalarPublicationFootprint) + sizeof(LabelPublicationSnapshot) +
		    2 * sizeof(std::shared_ptr<LabelPublicationState>) + 8 * sizeof(void*) + 4 * sizeof(uint64_t) <=
		    LabelCensusFixedDiscoveryScratchBytes);
		// This state had completed before Capture, after its store and both
		// callbacks returned. Ordinary retirement may remove its Label key;
		// the strong publication remains in the same exact census position.
		// An originally pending scalar never gains this retirement permission.
		const bool completed_detached = !original.scalar && original.label != nullptr && observed.label == nullptr &&
		    original.publication == observed.publication && LabelPublicationEnrollmentEqual(original.original, observed.original) &&
		    original.original.cancelled == 0 && original.original.pending == 0 &&
		    original.original.completed == original.original.queued && observed.original.cancelled == 0 &&
		    observed.original.pending == 0 && observed.original.completed == original.original.completed;
		if (original.publication != observed.publication ||
		    (original.label != observed.label && !completed_detached) ||
		    !LabelPublicationEnrollmentEqual(original.original, observed.original))
		{
			report_state("identity", "scalar-census-state-identity-changed");
			return reject("scalar-census-state-identity-changed");
		}
		if (!original.scalar)
		{
			if (observed.original.completed != original.original.completed || observed.original.pending != 0)
			{
				report_state("completed", "scalar-census-completed-state-changed");
				return reject("scalar-census-completed-state-changed");
			}
			if (completed_detached) report_state("completed-detached", "original-completed-state-no-future-effects", true);
			continue;
		}
		const auto* label = m_labels.Find(original.label);
		LabelScalarPublicationFootprint registered;
		if (label == nullptr || label->callbacks.publication != original.publication ||
		    !original.publication->scalar_single_delivery_sealed ||
		    !ScalarDeliverySealEqual(original.delivery, original.publication->scalar_single_delivery) ||
		    !DescribeSingleScalarDelivery(label->callbacks, &registered) || !ScalarDeliverySealEqual(original.delivery, registered))
		{
			report_state("delivery", "scalar-census-delivery-changed");
			return reject("scalar-census-delivery-changed");
		}
		LabelScalarPublicationCompletionTicketState scalar;
		scalar.delivery = original.delivery;
		scalar.last_completed = original.last_completed;
		if (const char* value = ValidateScalarCompletionSnapshot(&scalar, observed.original); value != nullptr)
		{
			report_state("scalar-progress", value);
			return reject(value);
		}
		if (!ScalarDeliveryMappingContinuous(original.delivery, nullptr, reason)) return false;
	}
	// Commit observed monotonic progress only after the whole validation passed.
	for (size_t i = 0; i != current.size(); ++i) bank.m_state->states[i].last_completed = current[i].original.completed;
	return true;
}

bool LabelCaptureScalarPublicationCensusBank(LabelScalarPublicationCensusBank* bank, const char** reason,
    uint64_t retained_metadata_budget, LabelScalarPublicationCensusCaptureWitness* witness)
{
	if (g_label_manager == nullptr)
	{
		if (bank != nullptr) *bank = {};
		if (reason != nullptr) *reason = "scalar-census-manager-unavailable";
		if (witness != nullptr) { *witness = {}; witness->reason = "scalar-census-manager-unavailable"; }
		return false;
	}
	return g_label_manager->CaptureScalarPublicationCensusBank(bank, reason, retained_metadata_budget, witness);
}

bool LabelValidateScalarPublicationCensusBank(const LabelScalarPublicationCensusBank& bank, const char** reason)
{
	if (g_label_manager == nullptr)
	{
		if (reason != nullptr) *reason = "scalar-census-manager-unavailable";
		return false;
	}
	return g_label_manager->ValidateScalarPublicationCensusBank(bank, reason);
}

static bool CompletedLabelPayloadMatches(const Label* label, const LabelCompletedRetirementFootprint& footprint,
                                        const uint64_t* params)
{
	const auto& delivery = label->callbacks;
	const bool wide = footprint.bytes == 8;
	return params != nullptr && (footprint.bytes == 4 || wide) && footprint.address != 0 &&
	    footprint.address <= UINT64_MAX - footprint.bytes && footprint.address % footprint.bytes == 0 &&
	    (wide ? delivery.dst_gpu_addr64 != nullptr && delivery.dst_gpu_addr32 == nullptr
	          : delivery.dst_gpu_addr32 != nullptr && delivery.dst_gpu_addr64 == nullptr) &&
	    LabelDestinationAddress(delivery) == footprint.address && delivery.write_bytes == footprint.bytes &&
	    delivery.destination_generation == footprint.original_generation && footprint.original_generation != UINT64_MAX &&
	    params[LabelGpuObject::PARAM_WRITE_BYTES] == 0 &&
	    params[LabelGpuObject::PARAM_VALUE] == (wide ? delivery.value64 : delivery.value32) &&
	    params[LabelGpuObject::PARAM_CALLBACK_1] == reinterpret_cast<uint64_t>(delivery.callback_1) &&
	    params[LabelGpuObject::PARAM_CALLBACK_2] == reinterpret_cast<uint64_t>(delivery.callback_2) &&
	    std::equal(std::begin(delivery.args), std::end(delivery.args), params + LabelGpuObject::PARAM_ARG_1) &&
	    params[LabelGpuObject::PARAM_PREPARATION_ROLE] == static_cast<uint64_t>(delivery.preparation_role) &&
	    (delivery.preparation_role == LabelGpuObject::PreparationRole::Ordinary ||
	     delivery.preparation_role == LabelGpuObject::PreparationRole::LockFreeWriteBack) &&
	    LabelPublicationEnrollmentEqual(delivery.delivery_identity, footprint.enrollment) &&
	    delivery.publication_sequence == footprint.enrollment.queued && label->cp == footprint.enrollment.cp;
}

static bool CompletedLabelMappingContinuous(const LabelCompletedRetirementFootprint& footprint,
                                           GraphicsGuestLeaseResult* mapping_result, const char** reason)
{
	Core::VirtualMemory::GuestDataLease lease;
	GraphicsGuestLeaseResult result;
	const bool continuous = TryAcquireGraphicsGuestContinuityLease(footprint.address, footprint.bytes,
	    Core::VirtualMemory::Mode::Read, footprint.original_generation, &lease, &result);
	if (mapping_result != nullptr) *mapping_result = result;
	if (!continuous && reason != nullptr)
		*reason = result.reason != nullptr ? result.reason : "completed-retirement-mapping-unproved";
	return continuous;
}

bool LabelManager::CaptureCompletedRetirementTicket(Label* label, uint64_t expected_instance, uint64_t address, uint64_t bytes,
    const uint64_t* tracked_params, const LabelScalarPublicationCensusBank& census,
    LabelCompletedRetirementTicket* ticket, LabelCompletedRetirementFootprint* footprint,
    GraphicsGuestLeaseResult* mapping_result, const char** reason)
{
	if (footprint != nullptr) *footprint = {};
	if (mapping_result != nullptr) *mapping_result = {};
	if (reason != nullptr) *reason = nullptr;
	const auto reject = [reason](const char* value) { if (reason != nullptr) *reason = value; return false; };
	if (ticket == nullptr) return reject("completed-retirement-ticket-unavailable");
	ticket->m_state.reset();
	if (footprint == nullptr || expected_instance == 0 || tracked_params == nullptr ||
	    census.m_state == nullptr || census.m_state->manager != this)
		return reject("completed-retirement-original-unqualified");
	Core::LockGuard lock(m_mutex);
	if (!ValidateScalarPublicationCensusBank(census, reason)) return false;
	label = m_labels.Find(label);
	if (label == nullptr || label->callbacks.publication == nullptr || label->status != LabelStatus::NotActive ||
	    label->device == nullptr || label->event == nullptr) return reject("completed-retirement-membership-unqualified");
	const auto original = SnapshotLabelPublication(*label->callbacks.publication);
	if (original.instance_id != expected_instance || original.cp == nullptr || original.queue < 0 ||
	    original.session_epoch == 0 || original.submit_serial == 0 || original.identity_ambiguous ||
	    original.queued == 0 || original.completed != original.queued || original.cancelled != 0 || original.pending != 0)
		return reject("completed-retirement-enrollment-unqualified");
	const auto member = std::find_if(census.m_state->states.begin(), census.m_state->states.end(), [&](const auto& state) {
		return state.label == label && state.publication == label->callbacks.publication;
	});
	if (member == census.m_state->states.end() || member->scalar ||
	    !LabelPublicationEnrollmentEqual(member->original, original) || member->original.completed != original.completed ||
	    member->original.pending != 0) return reject("completed-retirement-original-census-unqualified");
	auto retained = std::make_shared<LabelCompletedRetirementTicketState>();
	retained->publication = label->callbacks.publication;
	retained->label = label;
	retained->manager = this;
	retained->census = census;
	retained->footprint = {original, address, bytes, label->callbacks.destination_generation};
	std::copy_n(tracked_params, retained->params.size(), retained->params.begin());
	retained->device = label->device;
	retained->event = label->event;
	if (!CompletedLabelPayloadMatches(label, retained->footprint, retained->params.data()))
		return reject("completed-retirement-payload-unqualified");
	if (!CompletedLabelMappingContinuous(retained->footprint, mapping_result, reason)) return false;
	*footprint = retained->footprint;
	ticket->m_state = std::move(retained);
	return true;
}

bool LabelManager::ValidateCompletedRetirementTicket(const LabelCompletedRetirementTicket& ticket,
    GraphicsGuestLeaseResult* mapping_result, const char** reason)
{
	if (mapping_result != nullptr) *mapping_result = {};
	if (reason != nullptr) *reason = nullptr;
	const auto reject = [reason](const char* value) { if (reason != nullptr) *reason = value; return false; };
	if (ticket.m_state == nullptr || ticket.m_state->manager != this)
		return reject("completed-retirement-ticket-unavailable");
	Core::LockGuard lock(m_mutex);
	const auto& retained = *ticket.m_state;
	if (!ValidateScalarPublicationCensusBank(retained.census, reason)) return false;
	const auto* label = m_labels.Find(retained.label);
	if (label == nullptr || label->callbacks.publication != retained.publication || label->status != LabelStatus::NotActive ||
	    label->device != retained.device || label->event != retained.event)
		return reject("completed-retirement-membership-changed");
	const auto current = SnapshotLabelPublication(*retained.publication);
	const auto& original = retained.footprint.enrollment;
	if (!LabelPublicationEnrollmentEqual(current, original) || current.completed != original.completed || current.pending != 0)
		return reject("completed-retirement-enrollment-changed");
	if (!CompletedLabelPayloadMatches(label, retained.footprint, retained.params.data()))
		return reject("completed-retirement-payload-changed");
	return CompletedLabelMappingContinuous(retained.footprint, mapping_result, reason);
}

bool LabelCaptureCompletedRetirementTicket(Label* label, uint64_t expected_instance, uint64_t address, uint64_t bytes,
    const uint64_t* tracked_params, const LabelScalarPublicationCensusBank& census,
    LabelCompletedRetirementTicket* ticket, LabelCompletedRetirementFootprint* footprint,
    GraphicsGuestLeaseResult* mapping_result, const char** reason)
{
	if (g_label_manager == nullptr)
	{
		if (ticket != nullptr) *ticket = {};
		if (footprint != nullptr) *footprint = {};
		if (mapping_result != nullptr) *mapping_result = {};
		if (reason != nullptr) *reason = "completed-retirement-manager-unavailable";
		return false;
	}
	return g_label_manager->CaptureCompletedRetirementTicket(label, expected_instance, address, bytes, tracked_params, census,
	    ticket, footprint, mapping_result, reason);
}

bool LabelValidateCompletedRetirementTicket(const LabelCompletedRetirementTicket& ticket,
    GraphicsGuestLeaseResult* mapping_result, const char** reason)
{
	if (g_label_manager == nullptr)
	{
		if (mapping_result != nullptr) *mapping_result = {};
		if (reason != nullptr) *reason = "completed-retirement-manager-unavailable";
		return false;
	}
	return g_label_manager->ValidateCompletedRetirementTicket(ticket, mapping_result, reason);
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool LabelScalarPublicationCensusInventoryContractTest(CommandProcessor* cp, int queue, uint64_t epoch, uint64_t serial,
    uint64_t address, uint64_t value, bool scalar64,
    bool (*observe)(void*, Label*, const LabelScalarPublicationCensusBank&, bool (*deliver)(Label*)), void* context,
    uint64_t sequence)
{
#if LUMEN_PLATFORM != LUMEN_PLATFORM_OSX
	(void)cp; (void)queue; (void)epoch; (void)serial; (void)address; (void)value; (void)scalar64; (void)observe; (void)context; (void)sequence;
	return false;
#else
	if (observe == nullptr || sequence == 0 || sequence > 64) return false;
	LabelManager manager(LabelManager::TestInstance {});
	struct RestoreManager
	{
		LabelManager* previous = g_label_manager;
		~RestoreManager() { g_label_manager = previous; }
	} restore;
	g_label_manager = &manager;
	std::atomic<uint64_t> pending {0};
	Label label;
	label.cp = cp;
	label.callbacks.publication = std::make_shared<LabelPublicationState>();
	label.callbacks.publication->aggregate = &pending;
	label.callbacks.dst_gpu_addr64 = scalar64 ? reinterpret_cast<uint64_t*>(address) : nullptr;
	label.callbacks.dst_gpu_addr32 = scalar64 ? nullptr : reinterpret_cast<uint32_t*>(address);
	label.callbacks.value64 = value;
	label.callbacks.value32 = static_cast<uint32_t>(value);
	label.callbacks.write_bytes = scalar64 ? 8 : 4;
	struct RegistryEntry
	{
		Core::Mutex& mutex;
		OrderedLabelRegistry& registry;
		Label* label;
		std::shared_ptr<LabelPublicationState> publication;
		~RegistryEntry() { Core::LockGuard lock(mutex); registry.Remove(label); CancelLabelPublication(publication.get()); }
	} registration {manager.m_mutex, manager.m_labels, &label, label.callbacks.publication};
	{
		Core::LockGuard lock(manager.m_mutex);
		manager.RegisterLabelLocked(&label);
	}
	for (uint64_t current = 1; current <= sequence; ++current)
	{
		{
			Core::LockGuard lock(manager.m_mutex);
			// Earlier real stores preserve the fixture's guard sentinel. The
			// final enrollment seals the requested value without resetting guest.
			label.callbacks.value64 = current == sequence ? value : UINT64_C(0xa5a5a5a5a5a5a5a5);
			label.callbacks.value32 = static_cast<uint32_t>(label.callbacks.value64);
			if (!CaptureLabelDestination(&label.callbacks) || !manager.EnrollPublicationLocked(&label, cp, queue, epoch, serial)) return false;
			label.status = LabelStatus::Active;
		}
		if (current == sequence) break;
		LabelCallbacks delivery;
		{
			Core::LockGuard lock(manager.m_mutex);
			if (!SnapshotCompletedLabel(&label, VK_EVENT_SET, &delivery)) return false;
		}
		bool fired = false;
		DeliverLabelWorkerCallbacksWithWitness(delivery, [&](const LabelCallbacks& actual) { fired = TryFireLabelCallbacks(actual); });
		if (!fired || pending.load() != 0) return false;
	}
	LabelScalarPublicationCensusBank bank;
	if (!LabelCaptureScalarPublicationCensusBank(&bank)) return false;
	const bool result = observe(context, &label, bank, +[](Label* key) {
		LabelCallbacks delivery;
		{
			Core::LockGuard lock(g_label_manager->m_mutex);
			auto* member = g_label_manager->m_labels.Find(key);
			if (member == nullptr || !SnapshotCompletedLabel(member, VK_EVENT_SET, &delivery)) return false;
		}
		bool fired = false;
		DeliverLabelWorkerCallbacksWithWitness(delivery, [&](const LabelCallbacks& actual) { fired = TryFireLabelCallbacks(actual); });
		return fired;
	});
	return result && !Core::VirtualMemory::CurrentThreadHasMemoryLease();
#endif
}

bool LabelScalarPublicationCensusPairInventoryContractTest(CommandProcessor* cp, int queue, uint64_t epoch, uint64_t serial,
    const uint64_t* addresses, const uint64_t* values, bool scalar64,
    bool (*observe)(void*, Label* const*, const LabelScalarPublicationCensusBank&, bool (*deliver)(Label*)), void* context,
    uint64_t sequence)
{
#if LUMEN_PLATFORM != LUMEN_PLATFORM_OSX
	(void)cp; (void)queue; (void)epoch; (void)serial; (void)addresses; (void)values; (void)scalar64; (void)observe; (void)context;
	(void)sequence;
	return false;
#else
	if (observe == nullptr || addresses == nullptr || values == nullptr || sequence == 0 || sequence > 64) return false;
	LabelManager manager(LabelManager::TestInstance {});
	struct RestoreManager
	{
		LabelManager* previous = g_label_manager;
		~RestoreManager() { g_label_manager = previous; }
	} restore;
	g_label_manager = &manager;
	std::atomic<uint64_t> pending {0};
	std::array<Label, 2> labels;
	std::array<Label*, 2> keys {&labels[0], &labels[1]};
	struct RegistryEntries
	{
		Core::Mutex& mutex;
		OrderedLabelRegistry& registry;
		std::array<Label, 2>& labels;
		~RegistryEntries()
		{
			Core::LockGuard lock(mutex);
			for (auto& label: labels)
			{
				registry.Remove(&label);
				if (label.callbacks.publication != nullptr) CancelLabelPublication(label.callbacks.publication.get());
			}
		}
	} registration {manager.m_mutex, manager.m_labels, labels};
	{
		Core::LockGuard lock(manager.m_mutex);
		for (uint32_t i = 0; i != labels.size(); ++i)
		{
			auto& label = labels[i];
			label.cp = cp;
			label.callbacks.publication = std::make_shared<LabelPublicationState>();
			label.callbacks.publication->aggregate = &pending;
			label.callbacks.dst_gpu_addr64 = scalar64 ? reinterpret_cast<uint64_t*>(addresses[i]) : nullptr;
			label.callbacks.dst_gpu_addr32 = scalar64 ? nullptr : reinterpret_cast<uint32_t*>(addresses[i]);
			label.callbacks.value64 = values[i];
			label.callbacks.value32 = static_cast<uint32_t>(values[i]);
			label.callbacks.write_bytes = scalar64 ? 8 : 4;
			manager.RegisterLabelLocked(&label);
		}
	}
	for (uint64_t current = 1; current <= sequence; ++current)
	{
		{
			Core::LockGuard lock(manager.m_mutex);
			for (uint32_t i = 0; i != labels.size(); ++i)
			{
				auto& label = labels[i];
				label.callbacks.value64 = current == sequence ? values[i] : UINT64_C(0xa5a5a5a5a5a5a5a5);
				label.callbacks.value32 = static_cast<uint32_t>(label.callbacks.value64);
				if (!CaptureLabelDestination(&label.callbacks) || !manager.EnrollPublicationLocked(&label, cp, queue, epoch, serial)) return false;
				label.status = LabelStatus::Active;
			}
		}
		if (current == sequence) break;
		for (auto& label: labels)
		{
			LabelCallbacks delivery;
			{
				Core::LockGuard lock(manager.m_mutex);
				if (!SnapshotCompletedLabel(&label, VK_EVENT_SET, &delivery)) return false;
			}
			bool fired = false;
			DeliverLabelWorkerCallbacksWithWitness(delivery, [&](const LabelCallbacks& actual) { fired = TryFireLabelCallbacks(actual); });
			if (!fired) return false;
		}
		if (pending.load() != 0) return false;
	}
	LabelScalarPublicationCensusBank bank;
	if (!LabelCaptureScalarPublicationCensusBank(&bank) || bank.StateCount() != 2 || bank.ScalarCount() != 2) return false;
	const bool result = observe(context, keys.data(), bank, +[](Label* key) {
		LabelCallbacks delivery;
		{
			Core::LockGuard lock(g_label_manager->m_mutex);
			auto* member = g_label_manager->m_labels.Find(key);
			if (member == nullptr || !SnapshotCompletedLabel(member, VK_EVENT_SET, &delivery)) return false;
		}
		bool fired = false;
		DeliverLabelWorkerCallbacksWithWitness(delivery, [&](const LabelCallbacks& actual) { fired = TryFireLabelCallbacks(actual); });
		return fired;
	});
	return result && !Core::VirtualMemory::CurrentThreadHasMemoryLease();
#endif
}

bool LabelPendingInventoryContractTest(CommandProcessor* cp, int queue, uint64_t epoch, uint64_t serial,
    bool (*observe)(void*, Label*, bool (*complete)(Label*)), void* context)
{
	if (observe == nullptr) return false;
	LabelManager manager(LabelManager::TestInstance {});
	struct RestoreManager
	{
		LabelManager* previous = g_label_manager;
		~RestoreManager() { g_label_manager = previous; }
	} restore;
	g_label_manager = &manager;
	std::atomic<uint64_t> pending {0};
	Label label;
	label.callbacks.publication = std::make_shared<LabelPublicationState>();
	label.callbacks.publication->aggregate = &pending;
	if (!QueueLabelPublication(label.callbacks.publication.get(), cp, queue, epoch, serial)) return false;
	manager.m_labels.Add(&label);
	LabelPublicationCompletionTicket retained;
	const auto original = SnapshotLabelPublication(*label.callbacks.publication);
	LabelPublicationSnapshot observed;
	LabelPublicationHostMetadata host;
	if (!manager.GetPublicationSnapshot(&label, &observed, &host) ||
	    host.manager_identity != reinterpret_cast<uint64_t>(&manager) ||
	    host.publication_identity != reinterpret_cast<uint64_t>(label.callbacks.publication.get()) ||
	    host.registered_sequence != label.callbacks.publication_sequence || host.registered_destination != 0 ||
	    host.registered_write_bytes != 0 || host.registered_destination_generation != label.callbacks.destination_generation ||
	    host.registered_callback_1 != 0 || host.registered_callback_2 != 0 ||
	    host.registered_preparation_role != static_cast<uint64_t>(label.callbacks.preparation_role)) return false;
	if (!manager.CapturePublicationCompletionTicket(&label, original, &retained) ||
	    !manager.ValidatePublicationCompletionTicket(retained, &observed, nullptr) || observed.pending != 1) return false;
	const bool result = observe(context, &label, [](Label* key) {
		return CompleteLabelPublication(key->callbacks.publication.get(), 1);
	});
	bool valid = manager.ValidatePublicationCompletionTicket(retained, &observed, nullptr);
	manager.m_session_resetting = true;
	valid = !manager.ValidatePublicationCompletionTicket(retained, &observed, nullptr) && valid;
	valid = !manager.GetPublicationSnapshot(&label, &observed, &host) && host.manager_identity == 0 &&
	    host.publication_identity == 0 && host.registered_destination == 0 && observed.instance_id == 0 && valid;
	manager.m_session_resetting = false;
	const bool removed = manager.m_labels.Remove(&label);
	valid = !manager.ValidatePublicationCompletionTicket(retained, &observed, nullptr) && valid;
	valid = !manager.GetPublicationSnapshot(&label, &observed, &host) && host.manager_identity == 0 &&
	    host.publication_identity == 0 && host.registered_destination == 0 && observed.instance_id == 0 && valid;
	CancelLabelPublication(label.callbacks.publication.get());
	return result && valid && removed && pending.load() == 0;
}
#endif

uint64_t LabelPendingPublicationCount()
{
	return g_pending_publications.load(std::memory_order_acquire);
}

bool LabelManager::WaitForPublicationPrefix(CommandProcessor* cp, int queue, uint64_t epoch, uint64_t serial,
                                          uint64_t* publications)
{
	if (!EopPublicationPrefixEnabled() || publications == nullptr) return false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	std::vector<LabelPublicationTicket> tickets;
	while (!m_mutex.TryLock())
	{
		if (std::chrono::steady_clock::now() >= deadline) return false;
		Core::Thread::SleepMicro(100);
	}
	{
		struct ReleaseManagerLock
		{
			Core::Mutex& mutex;
			~ReleaseManagerLock() { mutex.Unlock(); }
		} release {m_mutex};
		if (m_session_resetting || !CaptureLabelPublicationPrefix(m_publications, cp, queue, epoch, serial, &tickets)) return false;
	}
	*publications = 0;
	for (const auto& ticket: tickets) *publications += ticket.sequence;
	for (;;)
	{
		bool cancelled = false;
		if (std::chrono::steady_clock::now() >= deadline) return false;
		if (LabelPublicationPrefixComplete(tickets, &cancelled)) return true;
		if (cancelled) return false;
		// No lock is retained: a completion callback may itself need to flush,
		// wait or materialize data through the same processor and tracker.
		Core::Thread::SleepMicro(100);
	}
}

bool LabelWaitForPublicationPrefix(CommandProcessor* cp, int queue, uint64_t epoch, uint64_t serial,
                                   uint64_t* publications)
{
	return g_label_manager != nullptr && g_label_manager->WaitForPublicationPrefix(cp, queue, epoch, serial, publications);
}

class LabelTestAllocation
{
public:
	LabelTestAllocation()
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		Core::VirtualMemory::Init();
		address = Core::VirtualMemory::Alloc(0, 65536, Core::VirtualMemory::Mode::ReadWrite);
#else
		address = reinterpret_cast<uint64_t>(native.data());
#endif
	}
	~LabelTestAllocation()
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		if (address != 0) Core::VirtualMemory::Free(address);
#endif
	}
	uint64_t address = 0;
private:
#if LUMEN_PLATFORM != LUMEN_PLATFORM_OSX
	std::array<uint64_t, 8192> native {};
#endif
};

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool LabelCompletedRetirementInventoryContractTest(CommandProcessor* cp, int queue, uint64_t epoch, uint64_t serial,
    uint64_t address, uint64_t value, bool scalar64,
    bool (*observe)(void*, Label*, const LabelScalarPublicationCensusBank&, const LabelGpuObject&), void* context,
	uint64_t sequence, uint64_t pending_offset)
{
#if LUMEN_PLATFORM != LUMEN_PLATFORM_OSX
	(void)cp; (void)queue; (void)epoch; (void)serial; (void)address; (void)value; (void)scalar64;
	(void)observe; (void)context; (void)sequence; (void)pending_offset;
	return false;
#else
	if (observe == nullptr || sequence == 0 || sequence > 16 || pending_offset > 4096 || pending_offset % 4 != 0) return false;
	LabelTestAllocation owned;
	if (owned.address == 0) return false;
	if (address == 0)
	{
		std::memset(reinterpret_cast<void*>(owned.address), 0xa5, 65536);
		address = owned.address + 128;
	}
	LabelManager manager(LabelManager::TestInstance {});
	struct RestoreManager { LabelManager* previous = g_label_manager; ~RestoreManager() { g_label_manager = previous; } } restore;
	g_label_manager = &manager;
	std::atomic<uint64_t> pending {0};
	uint64_t preparations = 0;
	const uint64_t args[LABEL_ARGS_MAX] = {reinterpret_cast<uint64_t>(&preparations), 0, 0, 0};
	const LabelGpuObject description(scalar64 ? value : static_cast<uint32_t>(value),
	    +[](const uint64_t* args, void*, uint64_t) { ++*reinterpret_cast<uint64_t*>(args[0]); return true; },
	    nullptr, args, 0, LabelGpuObject::PreparationRole::LockFreeWriteBack);
	Label label;
	label.device = reinterpret_cast<VkDevice>(uintptr_t {11});
	label.event = reinterpret_cast<VkEvent>(uintptr_t {21});
	label.cp = cp;
	label.callbacks.publication = std::make_shared<LabelPublicationState>();
	label.callbacks.publication->aggregate = &pending;
	label.callbacks.dst_gpu_addr64 = scalar64 ? reinterpret_cast<uint64_t*>(address) : nullptr;
	label.callbacks.dst_gpu_addr32 = scalar64 ? nullptr : reinterpret_cast<uint32_t*>(address);
	label.callbacks.value64 = value;
	label.callbacks.value32 = static_cast<uint32_t>(value);
	label.callbacks.write_bytes = scalar64 ? 8 : 4;
	label.callbacks.callback_1 = reinterpret_cast<LabelGpuObject::callback_t>(description.params[LabelGpuObject::PARAM_CALLBACK_1]);
	label.callbacks.preparation_role = LabelGpuObject::PreparationRole::LockFreeWriteBack;
	std::copy_n(args, LABEL_ARGS_MAX, label.callbacks.args);
	struct Registration
	{
		LabelManager& manager;
		Label& label;
		~Registration() { Core::LockGuard lock(manager.m_mutex); manager.m_labels.Remove(&label); CancelLabelPublication(label.callbacks.publication.get()); }
	} registration {manager, label};
	{
		Core::LockGuard lock(manager.m_mutex);
		manager.RegisterLabelLocked(&label);
	}
	for (uint64_t current = 1; current <= sequence; ++current)
	{
		LabelCallbacks delivery;
		{
			Core::LockGuard lock(manager.m_mutex);
			if (!CaptureLabelDestination(&label.callbacks) || !manager.EnrollPublicationLocked(&label, cp, queue, epoch, serial)) return false;
			label.status = LabelStatus::Active;
			if (!SnapshotCompletedLabel(&label, VK_EVENT_SET, &delivery)) return false;
		}
		bool fired = false;
		DeliverLabelWorkerCallbacksWithWitness(delivery, [&](const LabelCallbacks& actual) { fired = TryFireLabelCallbacks(actual); });
		if (!fired || preparations != current || pending != 0) return false;
	}
	Label pending_label;
	pending_label.cp = cp;
	pending_label.callbacks.publication = std::make_shared<LabelPublicationState>();
	pending_label.callbacks.publication->aggregate = &pending;
	pending_label.callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(address + pending_offset);
	pending_label.callbacks.value32 = 0x12345678;
	pending_label.callbacks.write_bytes = 4;
	Registration pending_registration {manager, pending_label};
	if (pending_offset != 0)
	{
		Core::LockGuard lock(manager.m_mutex);
		manager.RegisterLabelLocked(&pending_label);
		if (!CaptureLabelDestination(&pending_label.callbacks) ||
		    !manager.EnrollPublicationLocked(&pending_label, cp, queue, epoch, serial)) return false;
		pending_label.status = LabelStatus::Active;
	}
	LabelScalarPublicationCensusBank census;
	if (!LabelCaptureScalarPublicationCensusBank(&census)) return false;
	return observe(context, &label, census, description) && preparations == sequence && pending == (pending_offset != 0 ? 1u : 0u) &&
	    !Core::VirtualMemory::CurrentThreadHasMemoryLease();
#endif
}
#endif

bool LabelScalarPublicationCensusContractTest()
{
#if LUMEN_PLATFORM != LUMEN_PLATFORM_OSX || !defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	return true;
#else
	using namespace Core::VirtualMemory;
	{
		LabelManager manager(LabelManager::TestInstance {});
		std::atomic<uint64_t> pending {0};
		std::array<Label, 3> labels;
		auto* cp = reinterpret_cast<CommandProcessor*>(uintptr_t {0x1234});
		// Defer the geometric sweep so the second registration must use the
		// physical visit-budget headroom, independent of its normal watermark.
		manager.m_publications.resize(LabelScalarPublicationCensusBank::MaxVisitedStates - 2);
		manager.m_publication_compaction_watermark = std::numeric_limits<size_t>::max();
		for (uint32_t i = 0; i != 2; ++i)
		{
			labels[i].callbacks.publication = std::make_shared<LabelPublicationState>();
			auto* state = labels[i].callbacks.publication.get();
			state->aggregate = &pending;
			if (!QueueLabelPublication(state, cp, GraphicContext::QUEUE_GFX, 5, 9) ||
			    !CompleteLabelPublication(state, 1)) return false;
			Core::LockGuard lock(manager.m_mutex);
			manager.RegisterLabelLocked(&labels[i]);
			if (i == 0 && (manager.m_test_publication_compactions != 0 ||
			    manager.m_publications.size() + manager.m_labels.Size() !=
			        LabelScalarPublicationCensusBank::MaxVisitedStates)) return false;
		}
		if (manager.m_test_publication_compactions != 1 ||
		    manager.m_test_publication_compaction_visits != LabelScalarPublicationCensusBank::MaxVisitedStates - 1 ||
		    manager.m_publications.size() != 2 || pending != 0) return false;
		LabelScalarPublicationCensusBank original;
		const char* reason = nullptr;
		if (!manager.CaptureScalarPublicationCensusBank(&original, &reason,
		        LabelScalarPublicationCensusBank::MaxCombinedRetainedMetadataBytes, nullptr) || reason != nullptr ||
		    original.StateCount() != 2 || original.ScalarCount() != 0) return false;
		const uint64_t retained_bytes = original.RetainedMetadataBytes();
		// A detached worker payload and the original census both retain this
		// completed publication. Neither completion nor registry removal may
		// make compaction discard it while those references still exist.
		if (!manager.m_labels.Remove(&labels[0])) return false;
		LabelCallbacks detached = std::move(labels[0].callbacks);
		const auto detached_identity = SnapshotLabelPublication(*detached.publication);
		const auto registered_identity = SnapshotLabelPublication(*labels[1].callbacks.publication);
		std::vector<std::unique_ptr<Label>> live_unqueued;
		constexpr uint32_t retained_unqueued = 128;
		for (uint32_t i = 0; i != retained_unqueued; ++i)
		{
			auto label = std::make_unique<Label>();
			label->callbacks.publication = std::make_shared<LabelPublicationState>();
			Core::LockGuard lock(manager.m_mutex);
			manager.RegisterLabelLocked(label.get());
			live_unqueued.emplace_back(std::move(label));
		}
		const auto visits_before = manager.m_test_publication_compaction_visits;
		const auto sweeps_before = manager.m_test_publication_compactions;
		constexpr uint32_t insertions = 8192;
		for (uint32_t i = 0; i != insertions; ++i)
		{
			Label transient;
			transient.callbacks.publication = std::make_shared<LabelPublicationState>();
			Core::LockGuard lock(manager.m_mutex);
			manager.RegisterLabelLocked(&transient);
			if (!manager.m_labels.Remove(&transient)) return false;
			// This weak entry expires after registration, as ordinary retirement
			// does. Unqueued publications never affect the captured census.
		}
		const auto visits = manager.m_test_publication_compaction_visits - visits_before;
		const auto sweeps = manager.m_test_publication_compactions - sweeps_before;
		if (visits > 4u * insertions || sweeps == 0 || sweeps >= insertions / 2 ||
		    manager.m_publications.size() > 2u * (2u + retained_unqueued) ||
		    manager.m_labels.Size() != 1 + retained_unqueued || pending != 0 ||
		    !manager.ValidateScalarPublicationCensusBank(original, &reason) || reason != nullptr) return false;
		std::vector<LabelPublicationTicket> prefix;
		bool cancelled = false;
		if (!CaptureLabelPublicationPrefix(manager.m_publications, cp, GraphicContext::QUEUE_GFX, 5, 9, &prefix) ||
		    prefix.size() != 2 || prefix[0].state != detached.publication ||
		    prefix[1].state != labels[1].callbacks.publication ||
		    !LabelPublicationPrefixComplete(prefix, &cancelled) || cancelled ||
		    !LabelPublicationEnrollmentEqual(detached_identity, SnapshotLabelPublication(*detached.publication)) ||
		    !LabelPublicationEnrollmentEqual(registered_identity, SnapshotLabelPublication(*labels[1].callbacks.publication)))
			return false;
		LabelScalarPublicationCensusBank exact;
		if (!manager.CaptureScalarPublicationCensusBank(&exact, &reason, retained_bytes, nullptr) ||
		    reason != nullptr || exact.StateCount() != 2 || exact.ScalarCount() != 0 ||
		    exact.RetainedMetadataBytes() != retained_bytes ||
		    !manager.ValidateScalarPublicationCensusBank(exact, &reason) || reason != nullptr) return false;
		// An artificially oversized input still rejects before discovery. The
		// consumer cannot sweep expired entries to bypass its original visit cap.
		manager.m_publications.resize(LabelScalarPublicationCensusBank::MaxVisitedStates);
		const auto copies_before = manager.m_test_census_row_copies;
		const auto compactions_before = manager.m_test_publication_compactions;
		LabelScalarPublicationCensusBank refused = original;
		if (manager.CaptureScalarPublicationCensusBank(&refused, &reason,
		        LabelScalarPublicationCensusBank::MaxCombinedRetainedMetadataBytes, nullptr) || refused.IsCaptured() ||
		    reason == nullptr || std::strcmp(reason, "scalar-census-visit-capacity") != 0 ||
		    manager.m_test_census_row_copies != copies_before ||
		    manager.m_test_publication_compactions != compactions_before) return false;
		labels[2].callbacks.publication = std::make_shared<LabelPublicationState>();
		{
			Core::LockGuard lock(manager.m_mutex);
			manager.RegisterLabelLocked(&labels[2]);
		}
		if (manager.m_publications.size() != 3 + retained_unqueued ||
		    !manager.ValidateScalarPublicationCensusBank(original, &reason) || reason != nullptr ||
		    !manager.m_labels.Remove(&labels[1]) || !manager.m_labels.Remove(&labels[2]) || pending != 0 ||
		    LabelPublicationCompactionWatermark(std::numeric_limits<size_t>::max()) !=
		        std::numeric_limits<size_t>::max()) return false;
		for (const auto& label: live_unqueued)
			if (!manager.m_labels.Remove(label.get())) return false;
		std::fprintf(stderr, "[GFX_LABEL_PUBLICATION_REGISTRY_TEST] insertions=%u swept_entries=%" PRIu64
		    " sweeps=%" PRIu64 " live_unqueued=%u live/detached=retained order/enrollment=unchanged"
		    " census-budget=exact visit-cap=reject-before-discovery registration-headroom=preserved\n",
		    insertions, visits, sweeps, retained_unqueued);
	}
	enum class Scenario
	{
		Progress, DetachedPending, CancelledDetached, UnknownPending, UntrackedPending, PayloadChanged,
		ProtectRestore, Remap, SecondEnrollment, EqualCountAba, ResetRoundtrip,
		VisitCapacity, EnrolledCapacity, EpochOverflow, NewUnqueued, NewUnqueuedThenEnrolled,
		MultipleOutstanding, Callback1, Callback2, Role, Token, LatestUnsealed,
		CompletedDetached, CompletedChanged, CompletedScalarDetached, CompletedReplacedKey,
		ChurnPending, ChurnDetachedPending, ChurnCancelledDetached, ChurnBankOnlyCompleted,
		ChurnNewUnqueuedThenEnrolled
	};
	const auto run = [&](bool wide, uint64_t sequence, Scenario scenario) {
		LabelManager manager(LabelManager::TestInstance {});
		struct RestoreManager
		{
			LabelManager* previous = g_label_manager;
			~RestoreManager() { g_label_manager = previous; }
		} restore;
		g_label_manager = &manager;
		LabelTestAllocation owned;
		if (owned.address == 0) return false;
		std::memset(reinterpret_cast<void*>(owned.address), 0xa5, 65536);
		std::atomic<uint64_t> pending {0};
		std::array<Label, 3> labels;
		std::array<std::shared_ptr<LabelPublicationState>, 3> publications;
		std::vector<std::unique_ptr<Label>> extras;
		uint32_t callbacks = 0;
		struct Cleanup
		{
			Core::Mutex& mutex;
			OrderedLabelRegistry& registry;
			bool& resetting;
			std::array<Label, 3>& labels;
			std::array<std::shared_ptr<LabelPublicationState>, 3>& publications;
			std::vector<std::unique_ptr<Label>>& extras;
			~Cleanup()
			{
				Core::LockGuard lock(mutex);
				resetting = false;
				for (auto& label: labels) registry.Remove(&label);
				for (const auto& publication: publications) if (publication != nullptr) CancelLabelPublication(publication.get());
				for (const auto& label: extras) { registry.Remove(label.get()); CancelLabelPublication(label->callbacks.publication.get()); }
			}
		} cleanup {manager.m_mutex, manager.m_labels, manager.m_session_resetting, labels, publications, extras};
		const auto init = [&](Label* label, uint64_t address, bool scalar64, bool unknown, bool tracked) {
			label->cp = reinterpret_cast<CommandProcessor*>(uintptr_t {0x1234});
			label->callbacks.publication = std::make_shared<LabelPublicationState>();
			label->callbacks.publication->aggregate = &pending;
			label->callbacks.dst_gpu_addr64 = scalar64 ? reinterpret_cast<uint64_t*>(address) : nullptr;
			label->callbacks.dst_gpu_addr32 = scalar64 ? nullptr : reinterpret_cast<uint32_t*>(address);
			label->callbacks.value64 = 0x0123456789abcdefull;
			label->callbacks.value32 = 0x87654321;
			label->callbacks.write_bytes = scalar64 ? 8 : 4;
			if (unknown)
			{
				label->callbacks.args[0] = reinterpret_cast<uint64_t>(&callbacks);
				label->callbacks.callback_1 = label->callbacks.callback_2 = +[](const uint64_t* args, void*, uint64_t) {
					++*reinterpret_cast<uint32_t*>(args[0]); return true;
				};
			}
			Core::LockGuard lock(manager.m_mutex);
			if (tracked) manager.RegisterLabelLocked(label);
			else manager.m_labels.Add(label);
		};
		const auto enroll = [&](Label* label, uint64_t serial) {
			Core::LockGuard lock(manager.m_mutex);
			if (!CaptureLabelDestination(&label->callbacks) ||
			    !manager.EnrollPublicationLocked(label, label->cp, GraphicContext::QUEUE_GFX, 5, serial)) return false;
			label->status = LabelStatus::Active;
			return true;
		};
		const auto deliver = [&](Label* label) {
			LabelCallbacks delivery;
			{
				Core::LockGuard lock(manager.m_mutex);
				if (!SnapshotCompletedLabel(label, VK_EVENT_SET, &delivery)) return false;
			}
			bool fired = false;
			DeliverLabelWorkerCallbacksWithWitness(delivery, [&](const LabelCallbacks& actual) { fired = TryFireLabelCallbacks(actual); });
			return fired;
		};
		const uint64_t address = owned.address + 16384;
		init(&labels[0], address, wide, false, true);
		publications[0] = labels[0].callbacks.publication;
		init(&labels[1], address + 32, false, true, true);
		publications[1] = labels[1].callbacks.publication;
		if (!enroll(&labels[1], 9) || !deliver(&labels[1]) || callbacks != 2 || pending != 0) return false;
		LabelScalarPublicationFootprint first_seal;
		for (uint64_t current = 1; current <= sequence; ++current)
		{
			labels[0].callbacks.value64 = UINT64_C(0x0123456789abcdef) + sequence - current;
			labels[0].callbacks.value32 = UINT32_C(0x87654321) + static_cast<uint32_t>(sequence - current);
			if (!enroll(&labels[0], 9)) return false;
			if (current == 1) first_seal = publications[0]->scalar_first_delivery;
			if (!publications[0]->scalar_first_delivery_sealed ||
			    !ScalarDeliverySealEqual(first_seal, publications[0]->scalar_first_delivery) ||
			    !publications[0]->scalar_single_delivery_sealed ||
			    publications[0]->scalar_single_delivery.enrollment.queued != current ||
			    publications[0]->scalar_single_delivery.enrollment.completed != current - 1) return false;
			if (current == sequence) break;
			if (!deliver(&labels[0]) || pending != 0 || callbacks != 2) return false;
			uint64_t stored = 0;
			std::memcpy(&stored, reinterpret_cast<void*>(address), wide ? 8u : 4u);
			if (stored != (wide ? labels[0].callbacks.value64 : labels[0].callbacks.value32)) return false;
		}
		if (pending != 1) return false;
		uint32_t prior_value = 0;
		std::memcpy(&prior_value, reinterpret_cast<void*>(address + 32), sizeof(prior_value));
		if (prior_value != labels[1].callbacks.value32) return false;
		std::array<uint8_t, 65536> expected;
		std::memcpy(expected.data(), reinterpret_cast<void*>(owned.address), expected.size());
		LabelScalarPublicationCensusBank bank;
		const char* reason = nullptr;
		LabelScalarPublicationCensusEntry entry;
		LabelScalarPublicationCensusCaptureWitness capture_witness;
		const auto capture_copies = manager.m_test_census_row_copies;
		const auto capture_mappings = manager.m_test_census_mapping_checks;
		const auto capture_constructions = manager.m_test_census_bank_constructions;
		if (!LabelCaptureScalarPublicationCensusBank(&bank, &reason,
		        LabelScalarPublicationCensusBank::MaxCombinedRetainedMetadataBytes, &capture_witness) ||
		    reason != nullptr || !bank.IsCaptured() || !capture_witness.complete || !capture_witness.discovery_complete ||
		    capture_witness.reason != nullptr || capture_witness.row_present || capture_witness.state_count != 2 ||
		    capture_witness.scalar_count != 1 || manager.m_test_census_row_copies != capture_copies + 2 ||
		    manager.m_test_census_mapping_checks != capture_mappings + 1 ||
		    manager.m_test_census_bank_constructions != capture_constructions + 1 ||
		    bank.StateCount() != 2 || bank.ScalarCount() != 1 || !bank.GetScalarEntry(0, &entry) || entry.key != &labels[0] ||
		    entry.footprint.address != address || entry.footprint.scalar64 != wide || entry.footprint.bytes != (wide ? 8u : 4u) ||
		    entry.footprint.original_generation != labels[0].callbacks.destination_generation ||
		    entry.footprint.enrollment.queued != sequence || entry.footprint.enrollment.completed != sequence - 1 ||
		    entry.footprint.enrollment.pending != 1 ||
		    bank.RetainedMetadataBytes() == 0 || bank.RetainedMetadataBytes() > LabelScalarPublicationCensusBank::MaxCombinedRetainedMetadataBytes ||
		    !LabelValidateScalarPublicationCensusBank(bank, &reason) || reason != nullptr || CurrentThreadHasMemoryLease()) return false;
		const auto original = entry;
		if (scenario == Scenario::Progress)
		{
			const auto required = bank.RetainedMetadataBytes();
			const auto copies = manager.m_test_census_row_copies;
			const auto mappings = manager.m_test_census_mapping_checks;
			const auto constructions = manager.m_test_census_bank_constructions;
			LabelScalarPublicationCensusBank exact;
			if (!LabelCaptureScalarPublicationCensusBank(&exact, &reason, required) || reason != nullptr ||
			    exact.RetainedMetadataBytes() != required || exact.StateCount() != bank.StateCount() ||
			    exact.ScalarCount() != bank.ScalarCount() || manager.m_test_census_row_copies != copies + 2 ||
			    manager.m_test_census_mapping_checks != mappings + 1 ||
			    manager.m_test_census_bank_constructions != constructions + 1 ||
			    !LabelValidateScalarPublicationCensusBank(exact, &reason) || reason != nullptr) return false;
			for (const uint64_t budget: {required - 1, UINT64_C(0),
			         LabelScalarPublicationCensusBank::MaxCombinedRetainedMetadataBytes + 1, UINT64_MAX})
			{
				// Start with a captured output to require rejection to clear it.
				LabelScalarPublicationCensusBank refused = exact;
				const auto before_copies = manager.m_test_census_row_copies;
				const auto before_mappings = manager.m_test_census_mapping_checks;
				const auto before_constructions = manager.m_test_census_bank_constructions;
				if (LabelCaptureScalarPublicationCensusBank(&refused, &reason, budget) || reason == nullptr ||
				    std::strcmp(reason, budget == required - 1 ? "scalar-census-metadata-capacity" :
				        "scalar-census-metadata-budget-invalid") != 0 || refused.IsCaptured() ||
				    refused.StateCount() != 0 || refused.ScalarCount() != 0 || refused.RetainedMetadataBytes() != 0 ||
				    manager.m_test_census_row_copies != before_copies ||
				    manager.m_test_census_mapping_checks != before_mappings ||
				    manager.m_test_census_bank_constructions != before_constructions || pending != 1 || callbacks != 2 ||
				    std::memcmp(reinterpret_cast<void*>(owned.address), expected.data(), expected.size()) != 0 ||
				    CurrentThreadHasMemoryLease()) return false;
			}
			if (!LabelValidateScalarPublicationCensusBank(bank, &reason) || reason != nullptr) return false;
		}
		if (sequence > 1)
		{
			LabelScalarPublicationCompletionTicket standalone;
			LabelScalarPublicationFootprint unavailable;
			const auto current = SnapshotLabelPublication(*publications[0]);
			if (LabelCaptureScalarPublicationCompletionTicket(&labels[0], current, &standalone, &unavailable, nullptr, &reason) ||
			    standalone.IsCaptured() || reason == nullptr || unavailable.address != 0 ||
			    std::strcmp(reason, "scalar-completion-original-enrollment-unqualified") != 0) return false;
		}
		const auto init_third = [&](bool unknown = false, bool tracked = true) {
			init(&labels[2], address + 64, wide, unknown, tracked);
			publications[2] = labels[2].callbacks.publication;
		};
		const auto churn = [&] {
			const uint64_t pending_before = pending.load();
			const auto compactions_before = manager.m_test_publication_compactions;
			// These live, unqueued publications force growth beyond the minimum
			// sweep interval. They must stay tracked for any later enrollment.
			for (uint32_t i = 0; i != 128; ++i)
			{
				extras.push_back(std::make_unique<Label>());
				init(extras.back().get(), address + 128, wide, false, true);
			}
			for (uint32_t i = 0; i != 1024; ++i)
			{
				Label transient;
				init(&transient, address + 128, wide, false, true);
				Core::LockGuard lock(manager.m_mutex);
				if (!manager.m_labels.Remove(&transient)) return false;
			}
			return manager.m_test_publication_compactions > compactions_before && pending == pending_before &&
			    callbacks == 2 && std::memcmp(reinterpret_cast<void*>(owned.address), expected.data(), expected.size()) == 0 &&
			    !CurrentThreadHasMemoryLease();
		};
		const auto pending_prefix = [&] {
			std::vector<LabelPublicationTicket> prefix;
			bool cancelled = false;
			return CaptureLabelPublicationPrefix(manager.m_publications, labels[0].cp, GraphicContext::QUEUE_GFX, 5, 9, &prefix) &&
			    prefix.size() == 2 && prefix[0].state == publications[0] && prefix[0].sequence == sequence &&
			    prefix[1].state == publications[1] && prefix[1].sequence == 1 &&
			    !LabelPublicationPrefixComplete(prefix, &cancelled) && !cancelled;
		};
		LabelCallbacks detached;
		bool accepted = false;
		switch (scenario)
		{
			case Scenario::Progress:
			{
				const auto other = Alloc(0, 65536, Mode::ReadWrite);
				if (other == 0 || !Free(other) || !LabelValidateScalarPublicationCensusBank(bank, &reason) ||
				    !deliver(&labels[0]) || pending != 0 || callbacks != 2) return false;
				std::memcpy(expected.data() + 16384, &original.footprint.value, static_cast<size_t>(original.footprint.bytes));
				accepted = true;
				break;
			}
			case Scenario::ChurnPending:
			{
				LabelScalarPublicationCensusBank fresh;
				if (!churn() || !pending_prefix() || !LabelValidateScalarPublicationCensusBank(bank, &reason) ||
				    !LabelCaptureScalarPublicationCensusBank(&fresh, &reason, bank.RetainedMetadataBytes()) || reason != nullptr ||
				    fresh.StateCount() != 2 || fresh.ScalarCount() != 1 || !deliver(&labels[0]) || pending != 0 || callbacks != 2)
					return false;
				std::memcpy(expected.data() + 16384, &original.footprint.value, static_cast<size_t>(original.footprint.bytes));
				accepted = true;
				break;
			}
			case Scenario::DetachedPending:
			case Scenario::CancelledDetached:
			case Scenario::ChurnDetachedPending:
			case Scenario::ChurnCancelledDetached:
				if (!SnapshotCompletedLabel(&labels[0], VK_EVENT_SET, &detached) || !manager.m_labels.Remove(&labels[0])) return false;
				if (scenario == Scenario::CancelledDetached || scenario == Scenario::ChurnCancelledDetached)
					{ CancelLabelPublication(publications[0].get()); if (pending != 0) return false; }
				if (scenario == Scenario::ChurnDetachedPending && (!churn() || !pending_prefix() || pending != 1)) return false;
				if (scenario == Scenario::ChurnCancelledDetached)
				{
					std::vector<LabelPublicationTicket> prefix;
					if (!churn() || CaptureLabelPublicationPrefix(manager.m_publications, labels[0].cp,
					        GraphicContext::QUEUE_GFX, 5, 9, &prefix) || pending != 0) return false;
				}
				break;
			case Scenario::UnknownPending: init_third(true); if (!enroll(&labels[2], 10)) return false; break;
			case Scenario::UntrackedPending: init_third(false, false); if (!enroll(&labels[2], 10)) return false; break;
			case Scenario::PayloadChanged: ++labels[0].callbacks.value32; ++labels[0].callbacks.value64; break;
			case Scenario::ProtectRestore:
				if (!Protect(owned.address, 65536, Mode::Read) || !Protect(owned.address, 65536, Mode::ReadWrite)) return false; break;
			case Scenario::Remap:
				if (!Free(owned.address) || !AllocFixed(owned.address, 65536, Mode::ReadWrite)) return false;
				std::memcpy(reinterpret_cast<void*>(owned.address), expected.data(), expected.size()); break;
			case Scenario::SecondEnrollment:
				if (!deliver(&labels[0]) || !enroll(&labels[0], 10)) return false;
				std::memcpy(expected.data() + 16384, &original.footprint.value, static_cast<size_t>(original.footprint.bytes));
				{
					LabelScalarPublicationCensusBank next;
					LabelScalarPublicationCensusEntry next_entry;
					if (!LabelCaptureScalarPublicationCensusBank(&next, &reason) || !next.GetScalarEntry(0, &next_entry) ||
					    next_entry.key != original.key || next_entry.footprint.enrollment.queued != sequence + 1 ||
					    next_entry.footprint.enrollment.completed != sequence || next_entry.footprint.enrollment.pending != 1 ||
					    !LabelValidateScalarPublicationCensusBank(next, &reason)) return false;
				}
				break;
			case Scenario::EqualCountAba:
				if (!deliver(&labels[0])) return false;
				init_third();
				if (!enroll(&labels[2], 10) || pending != 1) return false;
				std::memcpy(expected.data() + 16384, &original.footprint.value, static_cast<size_t>(original.footprint.bytes)); break;
			case Scenario::ResetRoundtrip:
				// Begin is production; finishing the flag is modeled because this
				// headless registry has no native device/events to destroy.
				if (!manager.BeginSessionReset()) return false;
				manager.m_session_resetting = false; break;
			case Scenario::VisitCapacity:
				init_third();
				manager.m_publications.resize(LabelScalarPublicationCensusBank::MaxVisitedStates + 1);
				if (LabelValidateScalarPublicationCensusBank(bank, &reason) || !enroll(&labels[2], 10) || pending != 2) return false; break;
			case Scenario::EnrolledCapacity:
				for (uint32_t i = 0; i != LabelScalarPublicationCensusBank::MaxEnrolledStates - 1; ++i)
				{
					extras.push_back(std::make_unique<Label>());
					init(extras.back().get(), address + 64, wide, false, true);
					if (!enroll(extras.back().get(), 10)) return false;
				}
				if (pending != LabelScalarPublicationCensusBank::MaxEnrolledStates) return false;
				break;
			case Scenario::EpochOverflow:
				init_third(); manager.m_enrollment_epoch = UINT64_MAX;
				if (!enroll(&labels[2], 10) || pending != 2 || !manager.m_census_epoch_exhausted) return false; break;
			case Scenario::NewUnqueued: init_third(); accepted = true; break;
			case Scenario::NewUnqueuedThenEnrolled: init_third(); if (!enroll(&labels[2], 10)) return false; break;
			case Scenario::ChurnNewUnqueuedThenEnrolled:
			{
				init_third();
				LabelScalarPublicationCensusBank fresh;
				if (!churn() || !LabelValidateScalarPublicationCensusBank(bank, &reason) ||
				    !LabelCaptureScalarPublicationCensusBank(&fresh, &reason) || fresh.StateCount() != 2 || fresh.ScalarCount() != 1 ||
				    !enroll(&labels[2], 10) || pending != 2 ||
				    !LabelCaptureScalarPublicationCensusBank(&fresh, &reason) || fresh.StateCount() != 3 || fresh.ScalarCount() != 2 ||
				    !LabelValidateScalarPublicationCensusBank(fresh, &reason)) return false;
				break;
			}
			case Scenario::MultipleOutstanding:
				if (!SnapshotCompletedLabel(&labels[0], VK_EVENT_SET, &detached) || !enroll(&labels[0], 10) ||
				    pending != 2 || publications[0]->scalar_single_delivery_sealed) return false;
				break;
			case Scenario::Callback1: labels[0].callbacks.callback_1 = +[](const uint64_t*, void*, uint64_t) { return true; }; break;
			case Scenario::Callback2: labels[0].callbacks.callback_2 = +[](const uint64_t*, void*, uint64_t) { return true; }; break;
			case Scenario::Role: labels[0].callbacks.preparation_role = LabelGpuObject::PreparationRole::LockFreeWriteBack; break;
			case Scenario::Token: ++labels[0].callbacks.destination_generation; break;
			case Scenario::LatestUnsealed: publications[0]->scalar_single_delivery_sealed = false; break;
			case Scenario::CompletedDetached:
				if (!manager.m_labels.Remove(&labels[1])) return false;
				accepted = true;
				break;
			case Scenario::ChurnBankOnlyCompleted:
			{
				if (!manager.m_labels.Remove(&labels[1])) return false;
				const auto completed_identity = SnapshotLabelPublication(*publications[1]);
				const std::weak_ptr<LabelPublicationState> bank_only = publications[1];
				labels[1].callbacks.publication.reset();
				publications[1].reset();
				// The captured census is now the sole owner of this completed
				// callback state. Compaction must still preserve its prefix entry.
				if (bank_only.use_count() != 1 || !churn() || bank_only.use_count() != 1) return false;
				{
					std::vector<LabelPublicationTicket> prefix;
					bool cancelled = false;
					if (!CaptureLabelPublicationPrefix(manager.m_publications, labels[0].cp, GraphicContext::QUEUE_GFX, 5, 9, &prefix) ||
					    prefix.size() != 2 || prefix[0].state != publications[0] || prefix[1].state != bank_only.lock() ||
					    !LabelPublicationEnrollmentEqual(SnapshotLabelPublication(*prefix[1].state), completed_identity) ||
					    LabelPublicationPrefixComplete(prefix, &cancelled) || cancelled) return false;
				}
				LabelScalarPublicationCensusBank fresh;
				if (!LabelCaptureScalarPublicationCensusBank(&fresh, &reason, bank.RetainedMetadataBytes()) ||
				    fresh.StateCount() != 2 || fresh.ScalarCount() != 1) return false;
				accepted = true;
				break;
			}
			case Scenario::CompletedChanged:
				if (!manager.m_labels.Remove(&labels[1])) return false;
				++publications[1]->identity.submit_serial;
				break;
			case Scenario::CompletedScalarDetached:
				if (!deliver(&labels[0]) || !manager.m_labels.Remove(&labels[0]) || pending != 0) return false;
				std::memcpy(expected.data() + 16384, &original.footprint.value, static_cast<size_t>(original.footprint.bytes));
				break;
			case Scenario::CompletedReplacedKey:
				if (!manager.m_labels.Remove(&labels[1])) return false;
				labels[2].callbacks.publication = publications[1];
				manager.m_labels.Add(&labels[2]);
				break;
		}
		if (LabelValidateScalarPublicationCensusBank(bank, &reason) != accepted ||
		    (accepted ? reason != nullptr : reason == nullptr) || CurrentThreadHasMemoryLease()) return false;
		if (!accepted && scenario != Scenario::EqualCountAba && scenario != Scenario::ResetRoundtrip && scenario != Scenario::SecondEnrollment &&
		    scenario != Scenario::NewUnqueuedThenEnrolled && scenario != Scenario::ChurnNewUnqueuedThenEnrolled && scenario != Scenario::CompletedChanged &&
		    scenario != Scenario::CompletedScalarDetached && scenario != Scenario::CompletedReplacedKey)
		{
			LabelScalarPublicationCensusBank refused;
			LabelScalarPublicationCensusCaptureWitness denied;
			const auto before_mappings = manager.m_test_census_mapping_checks;
			const auto before_constructions = manager.m_test_census_bank_constructions;
			if (LabelCaptureScalarPublicationCensusBank(&refused, &reason,
			        LabelScalarPublicationCensusBank::MaxCombinedRetainedMetadataBytes, &denied) ||
			    refused.IsCaptured() || reason == nullptr || denied.reason != reason || denied.complete ||
			    manager.m_test_census_bank_constructions != before_constructions) return false;
			if (scenario == Scenario::LatestUnsealed || scenario == Scenario::Callback1 || scenario == Scenario::Callback2 ||
			    scenario == Scenario::Role || scenario == Scenario::Token || scenario == Scenario::PayloadChanged ||
			    scenario == Scenario::MultipleOutstanding)
			{
				if (!denied.discovery_complete || !denied.row_present || denied.key != &labels[0] || denied.row_index != 0 ||
				    denied.original.instance_id != original.footprint.enrollment.instance_id ||
				    denied.registered.registered_sequence != labels[0].callbacks.publication_sequence ||
				    denied.delivery_enrollment.completed != labels[0].callbacks.delivery_identity.completed ||
				    denied.delivery_enrollment.pending != labels[0].callbacks.delivery_identity.pending ||
				    manager.m_test_census_mapping_checks != before_mappings) return false;
				if (scenario == Scenario::LatestUnsealed && (denied.sealed || denied.description_checked)) return false;
				if ((scenario == Scenario::Callback1 || scenario == Scenario::Callback2 || scenario == Scenario::Role) &&
				    (!denied.sealed || !denied.description_checked || denied.described || denied.seal_checked)) return false;
				if ((scenario == Scenario::Token || scenario == Scenario::PayloadChanged) &&
				    (!denied.described || !denied.seal_checked || denied.seal_equal || denied.enrollment_checked)) return false;
				if (scenario == Scenario::MultipleOutstanding &&
				    (denied.original.queued != sequence + 1 || denied.original.completed != sequence - 1 ||
				     denied.original.pending != 2 || denied.description_checked)) return false;
			}
		}
		if (!bank.GetScalarEntry(0, &entry) || entry.key != original.key || !ScalarDeliverySealEqual(entry.footprint, original.footprint) ||
		    !capture_witness.complete || capture_witness.reason != nullptr || capture_witness.state_count != 2 ||
		    capture_witness.scalar_count != 1 || capture_witness.row_present ||
		    callbacks != 2 || std::memcmp(reinterpret_cast<void*>(owned.address), expected.data(), expected.size()) != 0 ||
		    CurrentThreadHasMemoryLease()) return false;
		LabelScalarPublicationCensusEntry unavailable {&labels[0], original.footprint};
		if (bank.GetScalarEntry(1, &unavailable) || unavailable.key != nullptr || unavailable.footprint.address != 0) return false;
		return true;
	};
	uint32_t cases = 0;
	for (const bool wide: {false, true})
	for (const uint64_t sequence: {UINT64_C(1), UINT64_C(2), UINT64_C(5)})
	for (const auto scenario: {Scenario::Progress, Scenario::DetachedPending, Scenario::CancelledDetached, Scenario::UnknownPending,
	                          Scenario::UntrackedPending, Scenario::PayloadChanged, Scenario::ProtectRestore, Scenario::Remap,
	                          Scenario::SecondEnrollment, Scenario::EqualCountAba, Scenario::ResetRoundtrip, Scenario::VisitCapacity,
	                          Scenario::EnrolledCapacity, Scenario::EpochOverflow, Scenario::NewUnqueued, Scenario::NewUnqueuedThenEnrolled,
	                          Scenario::MultipleOutstanding, Scenario::Callback1, Scenario::Callback2, Scenario::Role, Scenario::Token, Scenario::LatestUnsealed,
	                          Scenario::CompletedDetached, Scenario::CompletedChanged, Scenario::CompletedScalarDetached, Scenario::CompletedReplacedKey,
	                          Scenario::ChurnPending, Scenario::ChurnDetachedPending, Scenario::ChurnCancelledDetached,
	                          Scenario::ChurnBankOnlyCompleted, Scenario::ChurnNewUnqueuedThenEnrolled})
	{
		if (!run(wide, sequence, scenario))
		{
			std::fprintf(stderr, "[LABEL_SCALAR_CENSUS_TEST_FAILURE] scalar64=%u sequence=%" PRIu64 " scenario=%u\n",
			    wide ? 1u : 0u, sequence, static_cast<uint32_t>(scenario));
			return false;
		}
		++cases;
	}
	std::fprintf(stderr, "[LABEL_SCALAR_CENSUS_TEST] whole-weak-list=pass strong-original-states=pass"
	    " original-capture-witness/checked-short-circuit/frozen-enrollment=checked witness-extra-capture/VM-query=0"
	    " latest-single-enrollment=1/2/5 prior-real-stores=pass first-only-ticket/first-seal=preserved"
	    " real-enrollment/actual-store32/64=pass pending-retained/monotonic-completion=pass completed-unknown=zero-effect"
	    " detached/cancelled/unknown/untracked/payload/VM=reject multiple-outstanding/callback/role/token/unsealed=reject equal-count-enrollment-ABA=reject"
	    " reset-begin=actual reset-finish=modeled new-unenrolled=accepted later-enrollment=reject"
	    " original-completed-nonscalar-detach=accepted completed-identity/key-change/pending-scalar-detach=reject"
	    " geometric-churn-pending/detached/cancelled/bank-only/later-enrollment=checked zero-effects-before-delivery=pass"
	    " visited/enrolled/epoch-capacity=unavailable ordinary-enrollment-continues=pass"
	    " remaining-budget=exact-accepted one-byte-below/zero/over-max/overflow=empty-before-owned-copy/VM/bank"
	    " retained-plus-validation-scratch=accounted budget-cases=30"
	    " original-token=retained guard-bytes=pass native-event=modeled native-prefix/disjointness-authority=0 cases=%u\n", cases);
	return true;
#endif
}

bool LabelScalarPublicationCompletionContractTest()
{
#if LUMEN_PLATFORM != LUMEN_PLATFORM_OSX || !defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	return true;
#else
	using namespace Core::VirtualMemory;
	LabelManager manager(LabelManager::TestInstance {});
	struct RestoreManager
	{
		LabelManager* previous = g_label_manager;
		~RestoreManager() { g_label_manager = previous; }
	} restore;
	g_label_manager = &manager;
	enum class Scenario
	{
		ActualStore, Value, Address, Width, Token, Callback1, Callback2, Role,
		SecondEnrollment, Cancel, Reset, Removed, Replaced, Unsealed, ReadOnly,
		ProtectRestore, Remap
	};
	const auto run = [&](bool wide, Scenario scenario) {
		LabelTestAllocation owned;
		if (owned.address == 0) return false;
		std::memset(reinterpret_cast<void*>(owned.address), 0xa5, 65536);
		const uint64_t address = owned.address + 16384;
		std::atomic<uint64_t> pending {0};
		Label label;
		label.cp = reinterpret_cast<CommandProcessor*>(uintptr_t {0x1234});
		label.callbacks.publication = std::make_shared<LabelPublicationState>();
		label.callbacks.publication->aggregate = &pending;
		label.callbacks.dst_gpu_addr64 = wide ? reinterpret_cast<uint64_t*>(address) : nullptr;
		label.callbacks.dst_gpu_addr32 = wide ? nullptr : reinterpret_cast<uint32_t*>(address);
		label.callbacks.value64 = 0x0123456789abcdefull;
		label.callbacks.value32 = 0x87654321;
		label.callbacks.write_bytes = wide ? 8u : 4u;
		struct RegistryEntry
		{
			OrderedLabelRegistry& registry;
			bool& resetting;
			Label* label;
			std::shared_ptr<LabelPublicationState> publication;
			~RegistryEntry() { resetting = false; registry.Remove(label); CancelLabelPublication(publication.get()); }
		} registration {manager.m_labels, manager.m_session_resetting, &label, label.callbacks.publication};
		manager.m_labels.Add(&label);
		if (!CaptureLabelDestination(&label.callbacks)) return false;
		{
			Core::LockGuard lock(manager.m_mutex);
			// The production Set calls this same enrollment/seal helper.
			if (!QueueLabelPublicationDelivery(&label.callbacks, label.cp, GraphicContext::QUEUE_GFX, 5, 9)) return false;
			label.status = LabelStatus::Active;
		}
		const auto original = SnapshotLabelPublication(*label.callbacks.publication);
		const uint64_t original_token = label.callbacks.destination_generation;
		LabelScalarPublicationCompletionTicket ticket;
		LabelScalarPublicationFootprint footprint;
		GraphicsGuestLeaseResult mapping;
		const char* reason = nullptr;
		if (!LabelCaptureScalarPublicationCompletionTicket(&label, original, &ticket, &footprint, &mapping, &reason) ||
		    !ticket.IsCaptured() || reason != nullptr || footprint.address != address || footprint.bytes != (wide ? 8u : 4u) ||
		    footprint.value != (wide ? label.callbacks.value64 : label.callbacks.value32) ||
		    footprint.original_generation != original_token || footprint.scalar64 != wide || CurrentThreadHasMemoryLease()) return false;
		LabelPublicationSnapshot observed;
		if (!LabelValidateScalarPublicationCompletionTicket(ticket, &observed, &mapping, &reason) ||
		    observed.completed != 0 || observed.pending != 1 || pending.load() != 1 || reason != nullptr) return false;
		if (scenario == Scenario::ActualStore)
		{
			// An unrelated VM change must not refresh or revoke the source token.
			const auto other = Alloc(0, 65536, Mode::ReadWrite);
			if (other == 0 || !Free(other) || GetMutationGeneration() <= original_token ||
			    !LabelValidateScalarPublicationCompletionTicket(ticket, &observed, &mapping, &reason) ||
			    observed.pending != 1 || footprint.original_generation != original_token) return false;
			LabelCallbacks delivery;
			{
				Core::LockGuard lock(manager.m_mutex);
				// VkEvent completion is modeled; the detached copy, VM leases,
				// scalar store and publication counters use production helpers.
				if (!SnapshotCompletedLabel(&label, VK_EVENT_SET, &delivery)) return false;
			}
			LabelPublicationFailure failure;
			bool fired = false;
			DeliverLabelWorkerCallbacksWithWitness(delivery, [&](const LabelCallbacks& actual) {
				fired = TryFireLabelCallbacks(actual, &failure);
			});
			if (!fired || !LabelValidateScalarPublicationCompletionTicket(ticket, &observed, &mapping, &reason) ||
			    observed.queued != 1 || observed.completed != 1 || observed.pending != 0 || pending.load() != 0 ||
			    CurrentThreadHasMemoryLease() || footprint.original_generation != original_token) return false;
			uint64_t value = 0;
			std::memcpy(&value, reinterpret_cast<void*>(address), static_cast<size_t>(footprint.bytes));
			if (value != footprint.value) return false;
			const auto* bytes = reinterpret_cast<const uint8_t*>(owned.address);
			for (uint64_t i = 0; i != 65536; ++i)
				if ((i < 16384 || i >= 16384 + footprint.bytes) && bytes[i] != 0xa5) return false;
			// The same ticket cannot regress after actual observed completion.
			LabelScalarPublicationCompletionTicketState regression;
			regression.delivery = footprint; regression.last_completed = 1;
			if (ValidateScalarCompletionSnapshot(&regression, original) == nullptr) return false;
			return true;
		}
		switch (scenario)
		{
			case Scenario::Value: if (wide) ++label.callbacks.value64; else ++label.callbacks.value32; break;
			case Scenario::Address:
				if (wide) label.callbacks.dst_gpu_addr64 = reinterpret_cast<uint64_t*>(address + 8);
				else label.callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(address + 4); break;
			case Scenario::Width: label.callbacks.write_bytes += 4; break;
			case Scenario::Token: ++label.callbacks.destination_generation; break;
			case Scenario::Callback1: label.callbacks.callback_1 = +[](const uint64_t*, void*, uint64_t) { return true; }; break;
			case Scenario::Callback2: label.callbacks.callback_2 = +[](const uint64_t*, void*, uint64_t) { return true; }; break;
			case Scenario::Role: label.callbacks.preparation_role = LabelGpuObject::PreparationRole::LockFreeWriteBack; break;
			case Scenario::SecondEnrollment:
				if (!QueueLabelPublicationDelivery(&label.callbacks, label.cp, GraphicContext::QUEUE_GFX, 5, 10)) return false; break;
			case Scenario::Cancel: CancelLabelPublication(label.callbacks.publication.get()); break;
			case Scenario::Reset: manager.m_session_resetting = true; break;
			case Scenario::Removed: if (!manager.m_labels.Remove(&label)) return false; break;
			case Scenario::Replaced: label.callbacks.publication = std::make_shared<LabelPublicationState>(); break;
			case Scenario::Unsealed: label.callbacks.publication->scalar_first_delivery_sealed = false; break;
			case Scenario::ReadOnly: if (!Protect(owned.address, 65536, Mode::Read)) return false; break;
			case Scenario::ProtectRestore:
				if (!Protect(owned.address, 65536, Mode::Read) || !Protect(owned.address, 65536, Mode::ReadWrite)) return false; break;
			case Scenario::Remap:
				if (!Free(owned.address) || !AllocFixed(owned.address, 65536, Mode::ReadWrite)) return false;
				std::memset(reinterpret_cast<void*>(owned.address), 0xa5, 65536); break;
			case Scenario::ActualStore: return false;
		}
		if (LabelValidateScalarPublicationCompletionTicket(ticket, &observed, &mapping, &reason) ||
		    observed.instance_id != 0 || reason == nullptr || CurrentThreadHasMemoryLease()) return false;
		LabelScalarPublicationCompletionTicket refused;
		footprint.address = 123;
		if (LabelCaptureScalarPublicationCompletionTicket(&label, original, &refused, &footprint, &mapping, &reason) ||
		    refused.IsCaptured() || footprint.address != 0 || reason == nullptr || CurrentThreadHasMemoryLease()) return false;
		const auto* bytes = reinterpret_cast<const uint8_t*>(owned.address);
		for (uint64_t i = 0; i != 65536; ++i) if (bytes[i] != 0xa5) return false;
		return true;
	};
	uint32_t cases = 0;
	for (const bool wide: {false, true})
	for (const auto scenario: {Scenario::ActualStore, Scenario::Value, Scenario::Address, Scenario::Width, Scenario::Token,
	                          Scenario::Callback1, Scenario::Callback2, Scenario::Role, Scenario::SecondEnrollment,
	                          Scenario::Cancel, Scenario::Reset, Scenario::Removed, Scenario::Replaced, Scenario::Unsealed,
	                          Scenario::ReadOnly, Scenario::ProtectRestore, Scenario::Remap})
	{
		if (!run(wide, scenario))
		{
			std::fprintf(stderr, "[LABEL_SCALAR_COMPLETION_TEST_FAILURE] scalar64=%u scenario=%u\n",
			    wide ? 1u : 0u, static_cast<uint32_t>(scenario));
			return false;
		}
		++cases;
	}
	std::fprintf(stderr, "[LABEL_SCALAR_COMPLETION_TEST] real-registry=pass production-enrollment-seal=pass"
	    " detached-actual-store32/64=pass guard-bytes=pass pending-retained/actual-completion=pass"
	    " payload/callback/role/enrollment/cancel/reset/membership/replace=reject"
	    " readonly/protect-restore/remap=reject unrelated-VM=accepted original-token=retained"
	    " native-event=modeled native-prefix/disjointness-authority=0 cases=%u\n", cases);
	return true;
#endif
}

bool LabelCompletedRetirementContractTest()
{
#if LUMEN_PLATFORM != LUMEN_PLATFORM_OSX || !defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	return true;
#else
	using namespace Core::VirtualMemory;
	enum class Scenario
	{
		Unchanged, UnrelatedVm, ProtectRestore, Remap, Value, Address, Width, Callback1, Callback2,
		Arg0, Arg1, Arg2, Arg3, Role, Token, Active, ActiveDeleted, Event, Device, Removed, Replaced,
		NewEnrollment, OtherEnrollment, NewUnqueued, ResetRoundtrip, Epoch, CounterRegression, UnknownPending, DetachedPending
	};
	uint32_t cases = 0;
	for (const bool wide: {false, true})
	for (const uint64_t sequence: {uint64_t {1}, uint64_t {5}})
	for (const auto scenario: {Scenario::Unchanged, Scenario::UnrelatedVm, Scenario::ProtectRestore, Scenario::Remap,
	    Scenario::Value, Scenario::Address, Scenario::Width, Scenario::Callback1, Scenario::Callback2,
	    Scenario::Arg0, Scenario::Arg1, Scenario::Arg2, Scenario::Arg3, Scenario::Role, Scenario::Token,
	    Scenario::Active, Scenario::ActiveDeleted, Scenario::Event, Scenario::Device, Scenario::Removed, Scenario::Replaced,
	    Scenario::NewEnrollment, Scenario::OtherEnrollment, Scenario::NewUnqueued, Scenario::ResetRoundtrip, Scenario::Epoch,
	    Scenario::CounterRegression, Scenario::UnknownPending, Scenario::DetachedPending})
	{
		struct Context { Scenario scenario; uint64_t sequence; bool wide; } context {scenario, sequence, wide};
		if (!LabelCompletedRetirementInventoryContractTest(reinterpret_cast<CommandProcessor*>(uintptr_t {0x1234}),
		    GraphicContext::QUEUE_GFX, 3, 9, 0, UINT64_C(0x1122334455667788), wide,
		    +[](void* opaque, Label* label, const LabelScalarPublicationCensusBank& census, const LabelGpuObject& description) {
			const auto& context = *static_cast<Context*>(opaque);
			auto& manager = *g_label_manager;
			LabelPublicationSnapshot original;
			LabelPublicationHostMetadata host;
			if (!LabelGetPublicationSnapshot(label, &original, &host) || original.queued != context.sequence ||
			    original.completed != context.sequence || original.pending != 0 || census.StateCount() != 1 || census.ScalarCount() != 0)
				return false;
			const uint64_t address = host.registered_destination, bytes = host.registered_write_bytes;
			const uint64_t allocation = address - 128;
			const auto stored = [&](uint64_t offset) {
				return *reinterpret_cast<const uint8_t*>(allocation + offset);
			};
			std::array<uint8_t, 65536> before {};
			std::memcpy(before.data(), reinterpret_cast<void*>(allocation), before.size());
			const uint64_t callback_count = *reinterpret_cast<uint64_t*>(description.params[LabelGpuObject::PARAM_ARG_1]);
			LabelCompletedRetirementTicket ticket;
			LabelCompletedRetirementFootprint footprint;
			const char* reason = nullptr;
			const auto capture = [&](LabelCompletedRetirementTicket* output, uint64_t instance, uint64_t target,
			    uint64_t length, const uint64_t* params, const LabelScalarPublicationCensusBank& bank) {
				return LabelCaptureCompletedRetirementTicket(label, instance, target, length, params, bank, output, &footprint, nullptr, &reason);
			};
			if (!capture(&ticket, original.instance_id, address, bytes, description.params, census) || !ticket.IsCaptured() ||
			    footprint.original_generation != host.registered_destination_generation || footprint.enrollment.completed != context.sequence ||
			    !LabelValidateCompletedRetirementTicket(ticket)) return false;
			LabelCompletedRetirementTicket rejected;
			if (capture(&rejected, original.instance_id + 1, address, bytes, description.params, census) || rejected.IsCaptured() ||
			    capture(&rejected, original.instance_id, address + bytes, bytes, description.params, census) ||
			    capture(&rejected, original.instance_id, address, context.wide ? 4 : 8, description.params, census) ||
			    capture(&rejected, original.instance_id, address, bytes, description.params, LabelScalarPublicationCensusBank {})) return false;
			for (uint32_t field = 0; field < GpuObject::PARAMS_MAX; ++field)
			{
				std::array<uint64_t, GpuObject::PARAMS_MAX> changed;
				std::copy_n(description.params, changed.size(), changed.begin());
				changed[field] ^= 1;
				if (capture(&rejected, original.instance_id, address, bytes, changed.data(), census) || rejected.IsCaptured()) return false;
			}
			Label extra;
			extra.cp = original.cp;
			extra.callbacks.publication = std::make_shared<LabelPublicationState>();
			extra.callbacks.publication->aggregate = label->callbacks.publication->aggregate;
			extra.callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(address + 64);
			extra.callbacks.value32 = 0x12345678;
			extra.callbacks.write_bytes = 4;
			const auto original_publication = label->callbacks.publication;
			struct Cleanup
			{
				LabelManager& manager; Label& extra; std::shared_ptr<LabelPublicationState> original;
				~Cleanup() { Core::LockGuard lock(manager.m_mutex); manager.m_labels.Remove(&extra); CancelLabelPublication(extra.callbacks.publication.get()); CancelLabelPublication(original.get()); }
			} cleanup {manager, extra, original_publication};
			{
				Core::LockGuard lock(manager.m_mutex);
				switch (context.scenario)
				{
					case Scenario::Value: ++label->callbacks.value64; ++label->callbacks.value32; break;
					case Scenario::Address:
						if (context.wide) label->callbacks.dst_gpu_addr64 = reinterpret_cast<uint64_t*>(address + bytes);
						else label->callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(address + bytes);
						break;
					case Scenario::Width: ++label->callbacks.write_bytes; break;
					case Scenario::Callback1: label->callbacks.callback_1 = nullptr; break;
					case Scenario::Callback2: label->callbacks.callback_2 = +[](const uint64_t*, void*, uint64_t) { return true; }; break;
					case Scenario::Arg0: case Scenario::Arg1: case Scenario::Arg2: case Scenario::Arg3:
						++label->callbacks.args[static_cast<uint32_t>(context.scenario) - static_cast<uint32_t>(Scenario::Arg0)]; break;
					case Scenario::Role: label->callbacks.preparation_role = LabelGpuObject::PreparationRole::Ordinary; break;
					case Scenario::Token: ++label->callbacks.destination_generation; break;
					case Scenario::Active: label->status = LabelStatus::Active; break;
					case Scenario::ActiveDeleted: label->status = LabelStatus::ActiveDeleted; break;
					case Scenario::Event: label->event = reinterpret_cast<VkEvent>(uintptr_t {22}); break;
					case Scenario::Device: label->device = reinterpret_cast<VkDevice>(uintptr_t {12}); break;
					case Scenario::Removed: if (!manager.m_labels.Remove(label)) return false; break;
					case Scenario::Replaced:
						if (!manager.m_labels.Remove(label)) return false;
						label->callbacks.publication = std::make_shared<LabelPublicationState>();
						label->callbacks.publication->aggregate = original_publication->aggregate;
						manager.RegisterLabelLocked(label);
						break;
					case Scenario::NewEnrollment:
						if (!manager.EnrollPublicationLocked(label, original.cp, original.queue, original.session_epoch, original.submit_serial)) return false;
						label->status = LabelStatus::Active; break;
					case Scenario::OtherEnrollment: case Scenario::UnknownPending: case Scenario::DetachedPending:
						if (context.scenario == Scenario::UnknownPending || context.scenario == Scenario::DetachedPending)
							extra.callbacks.callback_1 = +[](const uint64_t*, void*, uint64_t) { return true; };
						manager.RegisterLabelLocked(&extra);
						if (!CaptureLabelDestination(&extra.callbacks) ||
						    !manager.EnrollPublicationLocked(&extra, original.cp, original.queue, original.session_epoch, original.submit_serial)) return false;
						extra.status = LabelStatus::Active;
						if (context.scenario == Scenario::DetachedPending)
						{
							LabelCallbacks detached;
							if (!SnapshotCompletedLabel(&extra, VK_EVENT_SET, &detached) || !manager.m_labels.Remove(&extra)) return false;
						}
						break;
					case Scenario::NewUnqueued: manager.RegisterLabelLocked(&extra); break;
					case Scenario::ResetRoundtrip: manager.m_session_resetting = true; ++manager.m_reset_epoch; manager.m_session_resetting = false; break;
					case Scenario::Epoch: ++manager.m_enrollment_epoch; break;
					case Scenario::CounterRegression:
						original_publication->completed.store(original.queued - 1);
						original_publication->aggregate->fetch_add(1); break;
					default: break;
				}
			}
			if (context.scenario == Scenario::UnrelatedVm)
			{
				const uint64_t unrelated = Alloc(0, 65536, Mode::ReadWrite);
				if (unrelated == 0 || !Free(unrelated)) return false;
			}
			if (context.scenario == Scenario::ProtectRestore &&
			    (!Protect(allocation, 65536, Mode::Read) || !Protect(allocation, 65536, Mode::ReadWrite))) return false;
			if (context.scenario == Scenario::Remap)
			{
				if (!Free(allocation) || !AllocFixed(allocation, 65536, Mode::ReadWrite)) return false;
				std::memcpy(reinterpret_cast<void*>(allocation), before.data(), before.size());
			}
			const bool expected = context.scenario == Scenario::Unchanged || context.scenario == Scenario::UnrelatedVm ||
			    context.scenario == Scenario::NewUnqueued;
			if (LabelValidateCompletedRetirementTicket(ticket, nullptr, &reason) != expected || CurrentThreadHasMemoryLease() ||
			    *reinterpret_cast<uint64_t*>(description.params[LabelGpuObject::PARAM_ARG_1]) != callback_count) return false;
			if (context.scenario == Scenario::UnknownPending || context.scenario == Scenario::DetachedPending)
			{
				LabelScalarPublicationCensusBank fresh;
				if (LabelCaptureScalarPublicationCensusBank(&fresh) || fresh.IsCaptured()) return false;
			}
			for (size_t offset = 0; offset != before.size(); ++offset) if (stored(offset) != before[offset]) return false;
			// Restore only fixture counters so the helper can verify the original
			// delivery; no store, callback or publication is replayed by the ticket.
			if (context.scenario == Scenario::CounterRegression)
			{
				original_publication->completed.store(original.queued);
				original_publication->aggregate->fetch_sub(1);
			}
			if (context.scenario == Scenario::NewEnrollment) CancelLabelPublication(original_publication.get());
			return true;
		    }, &context, sequence)) return false;
		++cases;
	}
	std::fprintf(stderr, "[GFX_LABEL_COMPLETED_RETIREMENT_TEST] actual-stores/callback-before-completion=pass scalar4/8=pass sequences=1/5"
	    " original-instance/params9/range/width/census=reject retained-bank/payload/membership/status/event/epoch/counters=reject"
	    " opaque/detached-pending=closed protect-restore/remap=reject unrelated-VM/new-unqueued=accepted"
	    " original-token=retained guest-bytes/callback-count=unchanged native-completion-authority=0 retirement-effects=0 cases=%u\n", cases);
	return true;
#endif
}

bool LabelPublicationContractTest()
{
	if (!LabelCompletedRetirementContractTest()) return false;
	if (!LabelScalarPublicationCensusContractTest()) return false;
	if (!LabelScalarPublicationCompletionContractTest()) return false;
	{
		LabelPublicationCompletionTicketState retained;
		retained.original.instance_id = 7; retained.original.cp = reinterpret_cast<CommandProcessor*>(uintptr_t {1});
		retained.original.queue = GraphicContext::QUEUE_GFX; retained.original.session_epoch = 5;
		retained.original.submit_serial = 9; retained.original.queued = 3; retained.original.completed = 1;
		retained.original.pending = 2; retained.last_completed = 1;
		auto current = retained.original;
		if (ValidateLabelCompletionSnapshot(&retained, current) != nullptr) return false;
		current.completed = 2; current.pending = 1;
		if (ValidateLabelCompletionSnapshot(&retained, current) != nullptr) return false;
		auto regressed = retained.original;
		if (ValidateLabelCompletionSnapshot(&retained, regressed) == nullptr) return false;
		for (uint32_t change = 0; change < 11; ++change)
		{
			auto invalid = current;
			switch (change)
			{
				case 0: ++invalid.instance_id; break;
				case 1: ++invalid.queued; ++invalid.pending; break;
				case 2: ++invalid.cancelled; --invalid.pending; break;
				case 3: invalid.cp = nullptr; break;
				case 4: ++invalid.queue; break;
				case 5: ++invalid.session_epoch; break;
				case 6: ++invalid.submit_serial; break;
				case 7: invalid.identity_ambiguous = true; break;
				case 8: invalid.completed = 4; invalid.pending = 0; break;
				case 9: invalid.pending = 0; break;
				case 10: invalid.completed = 0; invalid.pending = 3; break;
			}
			if (ValidateLabelCompletionSnapshot(&retained, invalid) == nullptr || retained.last_completed != 2) return false;
		}
		current.completed = 3; current.pending = 0;
		if (ValidateLabelCompletionSnapshot(&retained, current) != nullptr || retained.original.completed != 1 ||
		    retained.original.queued != 3 || retained.last_completed != 3) return false;
		std::fprintf(stderr, "[GFX_LABEL_COMPLETION_TICKET_TEST] fixed-enrollment=retained completion=monotonic"
		                     " requeue/cancel/instance/processor/queue/epoch/serial/ambiguity/regression/counts=reject"
		                     " store/callback/native-effects=0 original-sequence=unchanged\n");
	}
	constexpr std::array<const char*, 9> disabled_values {nullptr, "", "0", "01", "10", "true", "1 ", " 1", "1x"};
	for (const auto* host_checkpoint: disabled_values)
	{
		for (const auto* virtual_clock: disabled_values)
			for (const auto* immediate32_checkpoint: disabled_values)
			{
				if (EopPublicationPrefixSelectorEnabled(host_checkpoint, virtual_clock, immediate32_checkpoint) ||
				    !EopPublicationPrefixSelectorEnabled("1", virtual_clock, immediate32_checkpoint) ||
				    !EopPublicationPrefixSelectorEnabled(host_checkpoint, "1", immediate32_checkpoint) ||
				    !EopPublicationPrefixSelectorEnabled(host_checkpoint, virtual_clock, "1")) return false;
			}
	}
	if (!EopPublicationPrefixSelectorEnabled("1", "1", "1") ||
	    EopPublicationPrefixEnabled() !=
	        EopPublicationPrefixSelectorEnabled(std::getenv("LUMEN_EXPERIMENTAL_GFX_EOP_HOST_CHECKPOINT"),
	                                            std::getenv("LUMEN_EXPERIMENTAL_GFX_EOP_VIRTUAL_CLOCK"),
	                                            std::getenv("LUMEN_EXPERIMENTAL_GFX_EOP_IMMEDIATE32_CHECKPOINT"))) return false;
	std::atomic<uint64_t> pending {0};
	auto state = std::make_shared<LabelPublicationState>();
	state->aggregate = &pending;
	auto* cp = reinterpret_cast<CommandProcessor*>(uintptr_t {0x1234});
	if (SnapshotLabelPublication(*state).pending != 0 ||
	    !QueueLabelPublication(state.get(), cp, 8, 1, 2)) return false;
	Label label;
	label.status = LabelStatus::Active;
	label.callbacks.publication = state;
	label.callbacks.publication_sequence = 1;
	LabelTestAllocation owned_destination;
	if (owned_destination.address == 0) return false;
	auto& destination = *reinterpret_cast<uint32_t*>(owned_destination.address);
	destination = 0x11111111;
	label.callbacks.dst_gpu_addr32 = &destination;
	label.callbacks.value32 = 0x22222222;
	label.callbacks.write_bytes = sizeof(destination);
	if (!CaptureLabelDestination(&label.callbacks)) return false;
	LabelCallbacks first;
	if (SnapshotCompletedLabel(&label, VK_EVENT_RESET, &first) ||
	    SnapshotCompletedLabel(&label, VK_ERROR_DEVICE_LOST, &first) ||
	    !SnapshotCompletedLabel(&label, VK_EVENT_SET, &first) ||
	    SnapshotLabelPublication(*state).pending != 1 || pending.load() != 1) return false;
	std::vector<std::weak_ptr<LabelPublicationState>> prefix_states {state};
	std::vector<LabelPublicationTicket> first_prefix, both_prefix;
	if (!CaptureLabelPublicationPrefix(prefix_states, cp, 8, 1, 2, &first_prefix) || first_prefix.size() != 1)
		return false;
	// VkEvent completion permits a new event use, but not disappearance of the
	// old publication. Both immutable deliveries survive producer replacement.
	if (!QueueLabelPublication(state.get(), cp, 8, 1, 3) || CompleteLabelPublication(state.get(), 2) ||
	    pending.load() != 2) return false;
	std::vector<LabelPublicationTicket> invalid_prefix;
	if (CaptureLabelPublicationPrefix(prefix_states, cp, 8, 1, 2, &invalid_prefix) ||
	    CaptureLabelPublicationPrefix(prefix_states, cp, 8, 2, 3, &invalid_prefix) ||
	    CaptureLabelPublicationPrefix(prefix_states, cp, 7, 1, 3, &invalid_prefix) ||
	    !CaptureLabelPublicationPrefix(prefix_states, cp, 8, 1, 3, &both_prefix)) return false;
	LabelCallbacks second = first;
	second.publication_sequence = 2;
	second.value32 = 0x33333333;
	label.callbacks = {};
	struct Delay
	{
		std::atomic<bool> entered {false};
		std::atomic<bool> release {false};
		bool post_store = false;
		uint32_t* destination = nullptr;
		std::atomic<uint64_t>* pending = nullptr;
	} delay;
	delay.destination = &destination;
	delay.pending = &pending;
	first.args[0] = reinterpret_cast<uint64_t>(&delay);
	first.callback_1 = +[](const uint64_t* args, void* /*output*/, uint64_t /*output_size*/)
	{
		auto* delay = reinterpret_cast<Delay*>(args[0]);
		delay->entered.store(true, std::memory_order_release);
		while (!delay->release.load(std::memory_order_acquire)) std::this_thread::yield();
		return true;
	};
	first.callback_2 = +[](const uint64_t* args, void* /*output*/, uint64_t /*output_size*/)
	{
		auto* delay = reinterpret_cast<Delay*>(args[0]);
		delay->post_store = *delay->destination == 0x22222222 && delay->pending->load() == 2;
		return true;
	};
	std::thread worker([&] { FireLabelCallbacks(first); });
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!delay.entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	const auto blocked = SnapshotLabelPublication(*state);
	bool ok = delay.entered.load() && blocked.queued == 2 && blocked.completed == 0 &&
	          blocked.pending == 2 && pending.load() == 2 && destination == 0x11111111;
	bool prefix_cancelled = false;
	ok = ok && !LabelPublicationPrefixComplete(first_prefix, &prefix_cancelled) && !prefix_cancelled;
	delay.release.store(true, std::memory_order_release);
	worker.join();
	ok = ok && delay.post_store && SnapshotLabelPublication(*state).pending == 1 && pending.load() == 1;
	// Registry detachment and later reuse cannot extend the captured prefix.
	ok = ok && LabelPublicationPrefixComplete(first_prefix, &prefix_cancelled) &&
	     !LabelPublicationPrefixComplete(both_prefix, &prefix_cancelled) && !prefix_cancelled;
	FireLabelCallbacks(second);
	ok = ok && destination == 0x33333333 && SnapshotLabelPublication(*state).completed == 2 && pending.load() == 0 &&
	     !CompleteLabelPublication(state.get(), 2);
	ok = ok && LabelPublicationPrefixComplete(both_prefix, &prefix_cancelled);
	// A foreign processor or reversed serial must never become a no-write proof.
	ok = ok && QueueLabelPublication(state.get(), reinterpret_cast<CommandProcessor*>(uintptr_t {0x5678}), 8, 2, 1) &&
	     SnapshotLabelPublication(*state).identity_ambiguous;
	CancelLabelPublication(state.get());
	ok = ok && !LabelPublicationPrefixComplete(both_prefix, &prefix_cancelled) && prefix_cancelled &&
	     !CaptureLabelPublicationPrefix(prefix_states, cp, 8, 1, 3, &invalid_prefix);
	const auto cancelled = SnapshotLabelPublication(*state);
	ok = ok && cancelled.cancelled == 1 && cancelled.pending == 0 && pending.load() == 0 &&
	     !QueueLabelPublication(state.get(), cp, 8, 1, 4) && !CompleteLabelPublication(state.get(), 3);
	LabelPublicationState invalid;
	invalid.aggregate = &pending;
	ok = ok && QueueLabelPublication(&invalid, nullptr, -1, 0, 0) &&
	     SnapshotLabelPublication(invalid).identity_ambiguous;
	CancelLabelPublication(&invalid);
	return ok && pending.load() == 0;
}

static bool LabelLeaseRetryContractTest()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	enum class Scenario { Recover, NoWrite, Exhaust, Protect, Remap, UnrelatedGeneration, Cancel, Sequence,
	                      Requeue, Nonretryable, RealAcquisitionFailure, HeldLease };
	for (const auto scenario: {Scenario::Recover, Scenario::NoWrite, Scenario::Exhaust, Scenario::Protect,
	                         Scenario::Remap, Scenario::UnrelatedGeneration, Scenario::Cancel, Scenario::Sequence,
	                         Scenario::Requeue, Scenario::Nonretryable, Scenario::RealAcquisitionFailure, Scenario::HeldLease})
	{
		for (const bool retry_preflight: {true, false})
		{
			for (const uint64_t bytes: {4u, 8u, 64u, 49152u})
			{
				LabelTestAllocation owned;
				if (owned.address == 0) return false;
				struct Probe
				{
					Core::Mutex callback_mutex;
					uint32_t prepare = 0;
					uint32_t notify = 0;
					uint64_t address = 0;
					uint64_t bytes = 0;
					bool write = true;
					bool valid = true;
				} probe;
				probe.address = owned.address + (bytes == 49152 ? 0 : 16384);
				probe.bytes = bytes;
				probe.write = scenario != Scenario::NoWrite;
				std::memset(reinterpret_cast<void*>(owned.address), 0xa5, 65536);
				LabelCallbacks callbacks;
				callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(probe.address);
				callbacks.write_bytes = bytes;
				callbacks.args[0] = reinterpret_cast<uint64_t>(&probe);
				callbacks.callback_1 = +[](const uint64_t* args, void* output, uint64_t size)
				{
					auto& probe = *reinterpret_cast<Probe*>(args[0]);
					Core::LockGuard lock(probe.callback_mutex);
					++probe.prepare;
					probe.valid = probe.valid && !CurrentThreadHasMemoryLease() && output != nullptr &&
					              reinterpret_cast<uint64_t>(output) != probe.address && size == probe.bytes;
					std::memset(output, 0x3c, size);
					return probe.write;
				};
				callbacks.callback_2 = +[](const uint64_t* args, void* output, uint64_t size)
				{
					auto& probe = *reinterpret_cast<Probe*>(args[0]);
					++probe.notify;
					probe.valid = probe.valid && !CurrentThreadHasMemoryLease() && output == nullptr && size == 0;
					const auto* data = reinterpret_cast<const uint8_t*>(probe.address);
					for (uint64_t i = 0; i != probe.bytes; ++i)
						probe.valid = probe.valid && data[i] == (probe.write ? 0x3c : 0xa5);
					return true;
				};
				std::atomic<uint64_t> pending {0};
				auto publication = std::make_shared<LabelPublicationState>();
				publication->aggregate = &pending;
				callbacks.publication = publication;
				if (!QueueLabelPublication(publication.get(), reinterpret_cast<CommandProcessor*>(uintptr_t {1}), 0, 1, 1) ||
				    !CaptureLabelDestination(&callbacks)) return false;
				callbacks.publication_sequence = 1;
				// Retain a delivery independent of registry reuse, as the worker does.
				Label label;
				label.status = LabelStatus::Active;
				label.callbacks = callbacks;
				LabelCallbacks delivery;
				if (!SnapshotCompletedLabel(&label, VK_EVENT_SET, &delivery)) return false;
				const uint64_t original_generation = delivery.destination_generation;
				const uint64_t original_address = LabelDestinationAddress(delivery);
				const uint64_t original_bytes = delivery.write_bytes;
				const uint64_t original_sequence = delivery.publication_sequence;
				uint32_t preflight_calls = 0, publication_calls = 0, waits = 0, real_denials = 0;
				MutationTransaction transaction;
				ReservationHandle reservation;
				Core::VirtualMemory::GuestDataLease held;
				const auto validate = [&](const LabelCallbacks& snapshot, GuestDataLease* lease, const char** reason,
				                          bool* lease_acquisition_unavailable)
				{
					const bool preflight = probe.prepare == 0;
					auto& calls = preflight ? preflight_calls : publication_calls;
					++calls;
					const bool selected = preflight == retry_preflight;
					if (selected && scenario == Scenario::RealAcquisitionFailure && calls == 1)
						probe.valid = probe.valid && BeginReserveAligned(0, 65536, 65536, &reservation, &transaction);
					const bool valid = ValidateLabelDestination(snapshot, lease, reason, lease_acquisition_unavailable);
					if (!valid)
					{
						if (*lease_acquisition_unavailable) ++real_denials;
						return false;
					}
					if (!selected) return true;
					if (scenario == Scenario::HeldLease && calls == 1)
						probe.valid = probe.valid && TryAcquireGuestDataLease(owned.address, 4, Mode::Read, &held);
					if (scenario == Scenario::Exhaust || calls <= 2 || scenario == Scenario::Nonretryable)
					{
						// Test-only acquisition outcome injection. Every call first
						// validates the real mapping; the runtime has no injector.
						ReleaseGuestDataLease(lease);
						*reason = scenario == Scenario::Nonretryable ? "destination-protection" : "lease-unavailable";
						*lease_acquisition_unavailable = scenario != Scenario::Nonretryable;
						return false;
					}
					return true;
				};
				const auto backoff = [&]
				{
					++waits;
					probe.valid = probe.valid && !CurrentThreadHasMemoryLease() && probe.notify == 0 &&
					              publication->completed.load(std::memory_order_acquire) == 0 &&
					              delivery.destination_generation == original_generation &&
					              LabelDestinationAddress(delivery) == original_address && delivery.write_bytes == original_bytes &&
					              delivery.publication_sequence == original_sequence;
					const auto* data = reinterpret_cast<const uint8_t*>(owned.address);
					for (uint64_t i = 0; i != 65536; ++i) probe.valid = probe.valid && data[i] == 0xa5;
					bool unlocked = false;
					std::thread observer([&]
					{
						unlocked = probe.callback_mutex.TryLock();
						if (unlocked) probe.callback_mutex.Unlock();
					});
					observer.join();
					probe.valid = probe.valid && unlocked;
					if (waits != 1) return;
					if (scenario == Scenario::Protect) probe.valid = Protect(owned.address, 65536, Mode::Read) && probe.valid;
					if (scenario == Scenario::Remap)
					{
						probe.valid = Free(owned.address) && AllocFixed(owned.address, 65536, Mode::ReadWrite) && probe.valid;
						if (probe.valid) std::memset(reinterpret_cast<void*>(owned.address), 0x97, 65536);
					}
					if (scenario == Scenario::UnrelatedGeneration)
					{
						const auto other = Alloc(0, 65536, Mode::ReadWrite);
						probe.valid = other != 0 && Free(other) && probe.valid;
					}
					// Cancellation/sequence mutations model forbidden ticket state
					// changes; normal reset waits for worker-batch quiescence.
					if (scenario == Scenario::Cancel) publication->cancelled.store(1, std::memory_order_release);
					if (scenario == Scenario::Sequence) publication->completed.store(1, std::memory_order_release);
					if (scenario == Scenario::Requeue)
					{
						probe.valid = QueueLabelPublication(publication.get(), reinterpret_cast<CommandProcessor*>(uintptr_t {1}),
						                                  0, 1, 2) && probe.valid;
						label.callbacks = {};
					}
				};
				LabelPublicationFailure failure;
				const bool fired = TryFireLabelCallbacksWithLeaseRetry(delivery, &failure, validate, backoff);
				ReleaseGuestDataLease(&held);
				if (transaction.IsActive() && !transaction.Abort()) return false;
				const bool success = scenario == Scenario::Recover || scenario == Scenario::NoWrite || scenario == Scenario::Requeue ||
				                     scenario == Scenario::UnrelatedGeneration;
				const uint32_t selected_calls = retry_preflight ? preflight_calls : publication_calls;
				// Without an immutable denial snapshot, RO may remain an opaque
				// unavailable Write borrow until the bounded worker phase exhausts.
				const bool exhaust = scenario == Scenario::Exhaust || scenario == Scenario::RealAcquisitionFailure ||
				                     (scenario == Scenario::Protect && selected_calls == 16);
				const uint32_t expected_calls = success ? 3 : (exhaust ? 16 : 1);
				const uint32_t expected_waits = success ? 2 : (exhaust ? 15 :
					(scenario == Scenario::Protect || scenario == Scenario::Remap || scenario == Scenario::UnrelatedGeneration ||
					 scenario == Scenario::Cancel || scenario == Scenario::Sequence ? 1 : 0));
				const uint32_t expected_prepare = retry_preflight && !success ? 0 : 1;
				if (!probe.valid || CurrentThreadHasMemoryLease() || fired != success || selected_calls != expected_calls ||
				    waits != expected_waits || probe.prepare != expected_prepare || probe.notify != (success ? 1u : 0u) ||
				    delivery.destination_generation != original_generation || LabelDestinationAddress(delivery) != original_address ||
				    delivery.write_bytes != original_bytes || delivery.publication_sequence != original_sequence)
				{
					std::fprintf(stderr, "[LABEL_LEASE_RETRY_FAILURE] scenario=%u phase=%s bytes=%" PRIu64
					                     " valid=%u fired=%u expected_success=%u calls=%u expected_calls=%u waits=%u expected_waits=%u"
					                     " prepare=%u expected_prepare=%u notify=%u reason=%s\n",
					             static_cast<unsigned>(scenario), retry_preflight ? "preflight" : "publication", bytes,
					             probe.valid ? 1u : 0u, fired ? 1u : 0u, success ? 1u : 0u, selected_calls, expected_calls,
					             waits, expected_waits, probe.prepare, expected_prepare, probe.notify,
					             failure.reason != nullptr ? failure.reason : "none");
					return false;
				}
				const auto* data = reinterpret_cast<const uint8_t*>(owned.address);
				for (uint64_t i = 0; i != 65536; ++i)
				{
					const bool output_byte = i >= probe.address - owned.address && i < probe.address - owned.address + bytes;
					const uint8_t expected = scenario == Scenario::Remap ? 0x97 :
						(success && probe.write && output_byte ? 0x3c : 0xa5);
					if (data[i] != expected) return false;
				}
				if (success)
				{
					if (publication->completed != 1 || pending != (scenario == Scenario::Requeue ? 1u : 0u)) return false;
				}
				else
				{
					const char* expected_reason = "mapping-continuity-changed";
					if (exhaust) expected_reason = "lease-acquisition-retry-exhausted";
					if (scenario == Scenario::Protect && !exhaust) expected_reason = "destination-protection";
					if (scenario == Scenario::Cancel) expected_reason = "publication-cancelled";
					if (scenario == Scenario::Sequence) expected_reason = "publication-sequence";
					if (scenario == Scenario::Nonretryable) expected_reason = "destination-protection";
					if (scenario == Scenario::HeldLease) expected_reason = "retry-memory-lease-held";
					if (std::strcmp(failure.reason, expected_reason) != 0 ||
					    std::strcmp(failure.phase, retry_preflight ? "preflight" : "publication") != 0 ||
					    failure.attempts != expected_calls || pending != 1 ||
					    publication->completed != (scenario == Scenario::Sequence ? 1u : 0u)) return false;
				}
				if (scenario == Scenario::RealAcquisitionFailure && real_denials != 16) return false;
			}
		}
	}
	std::fprintf(stderr, "[LABEL_LEASE_RETRY_TEST] phases=preflight/publication attempts=16 waits=15"
	                     " original-target-and-ticket=retained callback-counts=once staging=retained"
	                     " real-acquisition-denial=16 unrelated-mutation=accepted same-map/protect/remap/cancel/sequence=reject"
	                     " leases-or-callback-lock-during-backoff=none\n");
#endif
	return true;
}

static bool LabelCaptureAcquisitionContractTest()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	enum class Scenario { Recover, RecoverLast, Exhaust, Revoke, RevokeRestore, Remap, UnrelatedGeneration,
	                      WrongScope, InactiveSuccess, HeldLease, ReadOnly, Executable, CrossReadOnly, Unmanaged,
	                      Misaligned32, Misaligned64, TooSmall, TooLarge, Overflow, Null };
	for (const auto scenario: {Scenario::Recover, Scenario::RecoverLast, Scenario::Exhaust, Scenario::Revoke,
	                          Scenario::RevokeRestore, Scenario::Remap, Scenario::UnrelatedGeneration,
	                          Scenario::WrongScope, Scenario::InactiveSuccess, Scenario::HeldLease,
	                          Scenario::ReadOnly, Scenario::Executable, Scenario::CrossReadOnly, Scenario::Unmanaged,
	                          Scenario::Misaligned32, Scenario::Misaligned64, Scenario::TooSmall,
	                          Scenario::TooLarge, Scenario::Overflow, Scenario::Null})
	for (const uint64_t bytes: {4u, 8u, 64u, 49152u})
	{
		LabelTestAllocation owned;
		if (owned.address == 0) return false;
		std::memset(reinterpret_cast<void*>(owned.address), 0xa5, 65536);
		struct Probe { uint32_t prepare = 0; uint32_t notify = 0; } probe;
		std::atomic<uint64_t> pending {0};
		auto publication = std::make_shared<LabelPublicationState>();
		publication->aggregate = &pending;
		LabelCallbacks callbacks;
		const uint64_t address = owned.address + (bytes == 49152 ? 0 : 16384);
		if (bytes == 8) callbacks.dst_gpu_addr64 = reinterpret_cast<uint64_t*>(address);
		else callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(address);
		callbacks.value64 = 0x123456789abcdef0ull;
		callbacks.value32 = 0x87654321;
		callbacks.write_bytes = bytes;
		callbacks.destination_generation = 123;
		callbacks.publication = publication;
		callbacks.args[0] = reinterpret_cast<uint64_t>(&probe);
		for (uint32_t i = 1; i < LABEL_ARGS_MAX; ++i) callbacks.args[i] = i;
		callbacks.callback_1 = +[](const uint64_t* args, void*, uint64_t) {
			++reinterpret_cast<Probe*>(args[0])->prepare; return true;
		};
		callbacks.callback_2 = +[](const uint64_t* args, void*, uint64_t) {
			++reinterpret_cast<Probe*>(args[0])->notify; return true;
		};
		uint64_t native = 0;
		if (scenario == Scenario::ReadOnly && !Protect(owned.address, 65536, Mode::Read)) return false;
		if (scenario == Scenario::Executable && !Protect(owned.address, 65536, Mode::ExecuteReadWrite)) return false;
		if (scenario == Scenario::CrossReadOnly)
		{
			// Force a footprint crossing writable/read-only logical regions.
			callbacks.dst_gpu_addr64 = nullptr;
			callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(owned.address + 16380);
			callbacks.write_bytes = 64;
			if (!Protect(owned.address + 16384, 16384, Mode::Read)) return false;
		}
		if (scenario == Scenario::Unmanaged)
		{
			callbacks.dst_gpu_addr64 = &native;
			callbacks.dst_gpu_addr32 = nullptr;
			callbacks.write_bytes = sizeof(native);
		}
		if (scenario == Scenario::Misaligned32 || scenario == Scenario::TooSmall || scenario == Scenario::Overflow ||
		    scenario == Scenario::Null)
		{
			callbacks.dst_gpu_addr64 = nullptr;
			callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(scenario == Scenario::Overflow ? UINTPTR_MAX - 3 :
			    scenario == Scenario::Null ? 0 : owned.address + 1);
			callbacks.write_bytes = scenario == Scenario::TooSmall ? 3 : 4;
			if (scenario == Scenario::TooSmall) callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(owned.address);
		}
		if (scenario == Scenario::Misaligned64)
		{
			callbacks.dst_gpu_addr64 = reinterpret_cast<uint64_t*>(owned.address + 4);
			callbacks.dst_gpu_addr32 = nullptr;
			callbacks.write_bytes = 8;
		}
		if (scenario == Scenario::TooLarge) callbacks.write_bytes = LabelGpuObject::MAX_WRITE_BYTES + 4;
		const LabelCallbacks original = callbacks;
		const auto same_fields = [](const LabelCallbacks& actual, const LabelCallbacks& expected) {
			if (actual.dst_gpu_addr64 != expected.dst_gpu_addr64 || actual.dst_gpu_addr32 != expected.dst_gpu_addr32 ||
			    actual.value64 != expected.value64 || actual.value32 != expected.value32 ||
			    actual.callback_1 != expected.callback_1 || actual.callback_2 != expected.callback_2 ||
			    actual.publication != expected.publication || actual.publication_sequence != expected.publication_sequence ||
			    actual.write_bytes != expected.write_bytes || actual.destination_generation != expected.destination_generation)
				return false;
			for (uint32_t i = 0; i < LABEL_ARGS_MAX; ++i) if (actual.args[i] != expected.args[i]) return false;
			return true;
		};
		const uint64_t generation = GetMutationGeneration();
		uint32_t calls = 0;
		bool valid = true;
		GuestDataLease held;
		const auto acquire = [&](uint64_t begin, uint64_t size, Mode mode, GuestDataLease* lease) {
			++calls;
			valid &= !CurrentThreadHasMemoryLease() && begin == LabelDestinationAddress(original) &&
			         size == original.write_bytes && mode == Mode::Write && same_fields(callbacks, original) &&
			         probe.prepare == 0 && probe.notify == 0 && pending == 0 && publication->queued == 0 &&
			         publication->completed == 0 && publication->cancelled == 0;
			if ((scenario == Scenario::Revoke || scenario == Scenario::RevokeRestore) && calls == 1)
			{
				valid &= Protect(owned.address, 65536, Mode::Read);
				if (scenario == Scenario::RevokeRestore) valid &= Protect(owned.address, 65536, Mode::ReadWrite);
				return false;
			}
			if (scenario == Scenario::Remap && calls == 1)
			{
				valid &= Free(owned.address) && AllocFixed(owned.address, 65536, Mode::ReadWrite);
				if (valid) std::memset(reinterpret_cast<void*>(owned.address), 0xa5, 65536);
				return false;
			}
			if (scenario == Scenario::UnrelatedGeneration && calls == 1)
			{
				const auto other = Alloc(0, 65536, Mode::ReadWrite);
				valid &= other != 0 && Free(other);
				return false;
			}
			if (scenario == Scenario::HeldLease)
			{
				valid &= TryAcquireGuestDataLease(owned.address, 4, Mode::Read, &held, false);
				return false;
			}
			if (scenario == Scenario::InactiveSuccess) return true;
			if (scenario == Scenario::WrongScope)
				return TryAcquireGuestDataLease(owned.address + 4, size, mode, lease, false);
			const bool acquired = TryAcquireGuestDataLease(begin, size, mode, lease, false);
			if (scenario == Scenario::Recover || scenario == Scenario::RecoverLast || scenario == Scenario::Exhaust)
			{
				// Every injected false first acquires a real valid lease. The
				// production helper must release it before another attempt.
				valid &= acquired && lease->IsActive() && lease->MutationGeneration() == generation;
				if (scenario == Scenario::Exhaust || calls < (scenario == Scenario::Recover ? 3u : 16u)) return false;
			}
			return acquired;
		};
		const char* reason = nullptr;
		GraphicsGuestLeaseResult result;
		const bool captured = CaptureLabelDestination(&callbacks, &reason, &result, acquire);
		ReleaseGuestDataLease(&held);
		const bool success = scenario == Scenario::Recover || scenario == Scenario::RecoverLast ||
		    scenario == Scenario::RevokeRestore || scenario == Scenario::Remap || scenario == Scenario::UnrelatedGeneration;
		const bool invalid_shape = scenario == Scenario::Misaligned32 || scenario == Scenario::Misaligned64 ||
		    scenario == Scenario::TooSmall || scenario == Scenario::TooLarge || scenario == Scenario::Overflow ||
		    scenario == Scenario::Null;
		const uint32_t expected_calls = scenario == Scenario::Recover ? 3u :
		    (scenario == Scenario::RecoverLast || scenario == Scenario::Exhaust) ? 16u : invalid_shape ? 0u :
		    (success ? 2u : scenario == Scenario::Revoke || scenario == Scenario::ReadOnly || scenario == Scenario::CrossReadOnly ? calls : 1u);
		LabelCallbacks expected = original;
		if (success) expected.destination_generation = result.observed_generation;
		if (!valid || CurrentThreadHasMemoryLease() || captured != success || calls != expected_calls ||
		    result.attempts != expected_calls || !same_fields(callbacks, expected) || probe.prepare != 0 || probe.notify != 0 ||
		    pending != 0 || publication->queued != 0 || publication->completed != 0 || publication->cancelled != 0 || native != 0 ||
		    calls > 16)
		{
			std::fprintf(stderr, "[LABEL_CAPTURE_RETRY_FAILURE] scenario=%u valid=%u captured=%u expected_success=%u"
			                     " calls=%u expected_calls=%u reason=%s\n", static_cast<unsigned>(scenario), valid ? 1u : 0u,
			             captured ? 1u : 0u, success ? 1u : 0u, calls, expected_calls, result.reason != nullptr ? result.reason : "none");
			return false;
		}
		const auto* data = reinterpret_cast<const uint8_t*>(owned.address);
		for (uint64_t i = 0; i != 65536; ++i) if (data[i] != 0xa5) return false;
		const char* expected_reason = scenario == Scenario::Null ? "destination-null" :
		    invalid_shape ? "destination-shape" : scenario == Scenario::Exhaust ? "lease-acquisition-retry-exhausted" :
		    scenario == Scenario::WrongScope || scenario == Scenario::InactiveSuccess ? "lease-shape" :
		    scenario == Scenario::HeldLease ? "retry-memory-lease-held" :
		    scenario == Scenario::ReadOnly || scenario == Scenario::CrossReadOnly ? "destination-protection" :
		    scenario == Scenario::Executable ? "destination-executable" :
		    scenario == Scenario::Unmanaged ? "destination-unowned" : "destination-protection";
		if ((scenario == Scenario::Revoke || scenario == Scenario::ReadOnly || scenario == Scenario::CrossReadOnly) && calls == 16)
			expected_reason = "lease-acquisition-retry-exhausted";
		if (success)
		{
			if (result.reason != nullptr || reason != nullptr || result.observed_generation < generation ||
			    (scenario != Scenario::Recover && scenario != Scenario::RecoverLast && result.observed_generation == generation)) return false;
		}
		else if (reason == nullptr || result.reason == nullptr || std::strcmp(reason, expected_reason) != 0 ||
		         std::strcmp(result.reason, expected_reason) != 0) return false;
	}
	std::fprintf(stderr, "[LABEL_CAPTURE_RETRY_TEST] production-helper=pass actual-vm=pass widths=4/8/64/49152"
	                     " initial-token=successful-capture recover=3/16 exhausted=16 target=retained"
	                     " fresh-before-effects=restore/remap/unrelated-accepted stale-after-capture=separate-validation"
	                     " shape/revoke/rights/executable/unmanaged=reject"
	                     " callbacks/stores/enrollment=none failure-fields=unchanged leases-between-attempts=none\n");
#endif
	return true;
}

static bool LabelWriteBackProofScopeContractTest()
{
	const LabelGpuObject ordinary(0, nullptr, nullptr);
	const LabelGpuObject marked(0, nullptr, nullptr, nullptr, 0, LabelGpuObject::PreparationRole::LockFreeWriteBack);
	const LabelGpuObject marked_copy = marked;
	LabelGpuObject::PreparationRole decoded = LabelGpuObject::PreparationRole::Ordinary;
	if (!DecodeLabelPreparationRole(ordinary.params[LabelGpuObject::PARAM_PREPARATION_ROLE], &decoded) ||
	    decoded != LabelGpuObject::PreparationRole::Ordinary ||
	    !DecodeLabelPreparationRole(marked_copy.params[LabelGpuObject::PARAM_PREPARATION_ROLE], &decoded) ||
	    decoded != LabelGpuObject::PreparationRole::LockFreeWriteBack || ordinary.Equal(marked.params) ||
	    marked.Equal(ordinary.params) || !marked.Equal(marked_copy.params) ||
	    DecodeLabelPreparationRole(2, &decoded) || decoded != LabelGpuObject::PreparationRole::LockFreeWriteBack) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	enum class Scenario { DetachedReuse, SkipStore, Direct, Unmarked, MissingCarrier, WrongCp, Nested, ForeignThread,
	                      HeldLease, Cancel, Sequence, Requeue, ProtectRestore, Remap, UnrelatedMutation, LeaseExhaust };
	for (const auto scenario: {Scenario::DetachedReuse, Scenario::SkipStore, Scenario::Direct, Scenario::Unmarked, Scenario::MissingCarrier,
	                         Scenario::WrongCp, Scenario::Nested, Scenario::ForeignThread, Scenario::HeldLease,
	                         Scenario::Cancel, Scenario::Sequence, Scenario::Requeue, Scenario::ProtectRestore,
	                         Scenario::Remap, Scenario::UnrelatedMutation, Scenario::LeaseExhaust})
	{
		LabelTestAllocation owned;
		if (owned.address == 0) return false;
		std::memset(reinterpret_cast<void*>(owned.address), 0xa5, 65536);
		std::atomic<uint64_t> pending {0};
		auto publication = std::make_shared<LabelPublicationState>();
		publication->aggregate = &pending;
		// CP/Vulkan event are modeled identities. Managed mapping, publication
		// state, mutexes, detached snapshot and callback/store path are real.
		auto* cp = reinterpret_cast<CommandProcessor*>(uintptr_t {0x6123});
		auto* foreign_cp = reinterpret_cast<CommandProcessor*>(uintptr_t {0x7456});
		if (!QueueLabelPublication(publication.get(), cp, 0, 7, 11)) return false;
		struct Probe
		{
			Scenario scenario;
			Core::Mutex mutex;
			CommandProcessor* cp = nullptr;
			CommandProcessor* foreign_cp = nullptr;
			uint64_t base = 0, original_generation = UINT64_MAX;
			uint32_t prepares = 0, notifications = 0, nested_prepares = 0;
			bool valid = true;
			const LabelWriteBackProofScope* borrowed = nullptr;
			std::shared_ptr<LabelPublicationState> publication;
			LabelCallbacks nested;
		} probe {scenario};
		probe.cp = cp; probe.foreign_cp = foreign_cp; probe.base = owned.address; probe.publication = publication;
		Label label;
		label.cp = cp;
		label.status = LabelStatus::Active;
		label.callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(owned.address);
		label.callbacks.write_bytes = 4;
		label.callbacks.publication = publication;
		label.callbacks.publication_sequence = 1;
		label.callbacks.delivery_identity = SnapshotLabelPublication(*publication);
		if (scenario != Scenario::Unmarked && !DecodeLabelPreparationRole(
		    marked_copy.params[LabelGpuObject::PARAM_PREPARATION_ROLE], &label.callbacks.preparation_role)) return false;
		label.callbacks.args[0] = reinterpret_cast<uint64_t>(&probe);
		label.callbacks.callback_1 = +[](const uint64_t* args, void* output, uint64_t bytes) {
			auto& p = *reinterpret_cast<Probe*>(args[0]);
			Core::LockGuard callback_lock(p.mutex);
			++p.prepares;
			p.valid = p.valid && !CurrentThreadHasMemoryLease() && output != nullptr && bytes == 4;
			const auto* scope = LabelGetActiveWriteBackProofScope(p.cp);
			p.borrowed = scope;
			const bool expected_scope = p.scenario != Scenario::Direct && p.scenario != Scenario::Unmarked && p.scenario != Scenario::MissingCarrier &&
			    p.scenario != Scenario::WrongCp;
			p.valid = p.valid && (scope != nullptr) == expected_scope && LabelGetActiveWriteBackProofScope(p.foreign_cp) == nullptr;
			GraphicsGuestLeaseResult result;
			p.valid = p.valid && !LabelValidateWriteBackProofScope(
			    reinterpret_cast<const LabelWriteBackProofScope*>(uintptr_t {1}), p.cp, &result) &&
			    std::strcmp(result.reason, "label-writeback-scope-inactive") == 0;
			if (scope != nullptr)
			{
				p.valid = p.valid && LabelValidateWriteBackProofScope(scope, p.cp, &result) && !CurrentThreadHasMemoryLease() &&
				    !LabelValidateWriteBackProofScope(scope, p.foreign_cp, &result) &&
				    std::strcmp(result.reason, "label-writeback-producer-identity") == 0;
				if (p.scenario == Scenario::Nested)
				{
					LabelPublicationFailure failure;
					p.valid = p.valid && TryFireLabelCallbacks(p.nested, &failure);
					bool delivered = false;
					DeliverLabelWorkerCallbacksWithWitness(p.nested, [&](const LabelCallbacks& nested) {
						delivered = TryFireLabelCallbacks(nested, &failure);
					});
					p.valid = p.valid && delivered && p.nested_prepares == 2 && LabelGetActiveWriteBackProofScope(p.cp) == scope;
				}
				if (p.scenario == Scenario::ForeignThread)
				{
					bool foreign_rejected = false;
					std::thread other([&] {
						GraphicsGuestLeaseResult foreign;
						const bool mutex_available = p.mutex.TryLock();
						if (mutex_available) p.mutex.Unlock();
						foreign_rejected = !mutex_available && LabelGetActiveWriteBackProofScope(p.cp) == nullptr &&
						    !LabelValidateWriteBackProofScope(scope, p.cp, &foreign) &&
						    std::strcmp(foreign.reason, "label-writeback-scope-inactive") == 0;
					});
					other.join();
					p.valid = p.valid && foreign_rejected;
				}
				if (p.scenario == Scenario::HeldLease)
				{
					GuestDataLease held;
					p.valid = p.valid && TryAcquireGuestDataLease(p.base, 4, Mode::Read, &held) &&
					    LabelGetActiveWriteBackProofScope(p.cp) == nullptr && !LabelValidateWriteBackProofScope(scope, p.cp, &result) &&
					    std::strcmp(result.reason, "label-writeback-memory-lease-held") == 0;
				}
				if (p.scenario == Scenario::Cancel) CancelLabelPublication(p.publication.get());
				if (p.scenario == Scenario::Sequence) p.publication->completed.store(1, std::memory_order_release);
				if (p.scenario == Scenario::Requeue)
					p.valid = p.valid && QueueLabelPublication(p.publication.get(), p.cp, 0, 7, 12);
				if (p.scenario == Scenario::ProtectRestore)
					p.valid = p.valid && Protect(p.base, 65536, Mode::Read) && Protect(p.base, 65536, Mode::ReadWrite);
				if (p.scenario == Scenario::Remap)
				{
					p.valid = p.valid && Free(p.base) && AllocFixed(p.base, 65536, Mode::ReadWrite);
					if (p.valid) std::memset(reinterpret_cast<void*>(p.base), 0xa5, 65536);
				}
				if (p.scenario == Scenario::UnrelatedMutation)
				{
					const uint64_t unrelated = Alloc(0, 65536, Mode::ReadWrite);
					p.valid = p.valid && unrelated != 0 && Free(unrelated);
				}
				if (p.scenario == Scenario::LeaseExhaust)
				{
					ReservationHandle reservation;
					MutationTransaction mutation;
					p.valid = p.valid && BeginReserveAligned(0, 65536, 65536, &reservation, &mutation) &&
					    !LabelValidateWriteBackProofScope(scope, p.cp, &result) && result.attempts == 16 &&
					    std::strcmp(result.reason, "lease-acquisition-retry-exhausted") == 0 &&
					    !CurrentThreadHasMemoryLease() && mutation.Abort();
				}
				const bool expected_validation = p.scenario != Scenario::Cancel && p.scenario != Scenario::Sequence &&
				    p.scenario != Scenario::Requeue && p.scenario != Scenario::ProtectRestore && p.scenario != Scenario::Remap;
				p.valid = p.valid && LabelValidateWriteBackProofScope(scope, p.cp, &result) == expected_validation &&
				    !CurrentThreadHasMemoryLease();
			}
			std::memset(output, 0x3c, bytes);
			return p.scenario != Scenario::SkipStore;
		};
		label.callbacks.callback_2 = +[](const uint64_t* args, void*, uint64_t) {
			auto& p = *reinterpret_cast<Probe*>(args[0]);
			++p.notifications;
			GraphicsGuestLeaseResult result;
			p.valid = p.valid && !CurrentThreadHasMemoryLease() && LabelGetActiveWriteBackProofScope(p.cp) == nullptr &&
			    !LabelValidateWriteBackProofScope(p.borrowed, p.cp, &result) &&
			    std::strcmp(result.reason, "label-writeback-scope-inactive") == 0;
			return true;
		};
		if (!CaptureLabelDestination(&label.callbacks)) return false;
		probe.original_generation = label.callbacks.destination_generation;
		probe.nested.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(owned.address + 4096);
		probe.nested.write_bytes = 4;
		probe.nested.args[0] = reinterpret_cast<uint64_t>(&probe);
		probe.nested.callback_1 = +[](const uint64_t* args, void*, uint64_t) {
			auto& p = *reinterpret_cast<Probe*>(args[0]);
			++p.nested_prepares;
			GraphicsGuestLeaseResult result;
			p.valid = p.valid && LabelGetActiveWriteBackProofScope(p.cp) == nullptr &&
			    !LabelValidateWriteBackProofScope(p.borrowed, p.cp, &result) &&
			    std::strcmp(result.reason, "label-writeback-scope-inactive") == 0;
			return false;
		};
		if (!CaptureLabelDestination(&probe.nested)) return false;
		OrderedLabelRegistry registry;
		Core::Mutex registry_mutex;
		LabelCallbacks delivery;
		{
			Core::LockGuard registry_lock(registry_mutex);
			registry.Add(&label);
			if (scenario == Scenario::WrongCp) label.cp = foreign_cp;
			if (!SnapshotCompletedLabel(&label, VK_EVENT_SET, &delivery) || !registry.Remove(&label)) return false;
			// The same registry address now names a different publication. Neither
			// its target nor instance may replace the detached worker's witness.
			label.callbacks = {};
			label.callbacks.publication = std::make_shared<LabelPublicationState>();
			registry.Add(&label);
		}
		if (scenario == Scenario::MissingCarrier) delivery.delivery_identity = {};
		LabelPublicationFailure failure;
		bool delivered = false;
		const auto fire = [&](const LabelCallbacks& snapshot) {
			const bool registry_available = registry_mutex.TryLock();
			if (registry_available) registry_mutex.Unlock();
			probe.valid = probe.valid && registry_available;
			delivered = TryFireLabelCallbacks(snapshot, &failure);
		};
		if (scenario == Scenario::Direct) fire(delivery);
		else DeliverLabelWorkerCallbacksWithWitness(delivery, fire);
		const bool expected_delivery = scenario != Scenario::Cancel && scenario != Scenario::Sequence &&
		    scenario != Scenario::ProtectRestore && scenario != Scenario::Remap;
		const bool expected_store = expected_delivery && scenario != Scenario::SkipStore;
		GraphicsGuestLeaseResult inactive;
		if (delivered != expected_delivery || !probe.valid || probe.prepares != 1 ||
		    probe.notifications != (expected_delivery ? 1u : 0u) ||
		    *reinterpret_cast<uint32_t*>(owned.address) != (expected_store ? 0x3c3c3c3cu : 0xa5a5a5a5u) ||
		    delivery.destination_generation != probe.original_generation || LabelGetActiveWriteBackProofScope(cp) != nullptr ||
		    LabelValidateWriteBackProofScope(probe.borrowed, cp, &inactive) || CurrentThreadHasMemoryLease()) return false;
		if (scenario == Scenario::Sequence) publication->completed.store(0, std::memory_order_release);
		if (expected_delivery && SnapshotLabelPublication(*publication).completed != 1) return false;
		CancelLabelPublication(publication.get());
		if (pending.load() != 0 || !Free(owned.address)) return false;
	}
	std::fprintf(stderr, "[GFX_LABEL_WRITEBACK_SCOPE_TEST] production-worker-witness=pass callback1-only=pass"
	                     " actual-vm/mutex/publication/store=pass cp/vulkan=modeled detached/replaced-registry=pass"
	                     " typed-role=copy/factory-decoder/equality/default-ordinary-closed"
	                     " direct/missing/foreign/nested/afterstore=closed original-target-token=retained"
	                     " lease-exhaust=16/recover proof-protect-restore/remap/ticket-change=reject"
	                     " successful-prepare/notify/store/completed=once skip-store-semantics=preserved\n");
#else
	GraphicsGuestLeaseResult unavailable;
	if (LabelGetActiveWriteBackProofScope(reinterpret_cast<CommandProcessor*>(uintptr_t {1})) != nullptr ||
	    LabelValidateWriteBackProofScope(reinterpret_cast<const LabelWriteBackProofScope*>(uintptr_t {1}),
	        reinterpret_cast<CommandProcessor*>(uintptr_t {1}), &unavailable) || unavailable.reason == nullptr ||
	    std::strcmp(unavailable.reason, "label-writeback-lease-capability-unavailable") != 0) return false;
#endif
	return true;
}

bool LabelScalarCaptureContractTest()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	for (const uint64_t bytes: {uint64_t {4}, uint64_t {8}})
	for (uint32_t scenario = 0; scenario != 8; ++scenario)
	{
		LabelTestAllocation owned;
		if (owned.address == 0) return false;
		std::memset(reinterpret_cast<void*>(owned.address), 0xa5, 65536);
		GuestWriteDestination destination;
		if (!destination.Capture(owned.address + 128, bytes, bytes)) return false;
		int buffer_key = 0, pool_key = 0;
		auto* cp = reinterpret_cast<CommandProcessor*>(uintptr_t {0x1000});
		LabelScalarCaptureTicket ticket;
		ticket.m_address = destination.Address(); ticket.m_size = destination.Size();
		ticket.m_generation = destination.Generation();
		ticket.m_value = bytes == 4 ? 0x55667788ull : 0x1122334455667788ull;
		auto& recording = ticket.m_recording;
		recording.processor = recording.parent = cp; recording.buffer = &buffer_key; recording.pool = &pool_key;
		recording.current_index = 0; recording.pool_index = 0;
		recording.processor_queue = recording.buffer_queue = GraphicContext::QUEUE_GFX;
		recording.submit_id = 9; recording.processor_epoch = recording.session = 3;
		recording.assigned_serial = recording.serial = 5; recording.recording = 7; recording.ordinal = 11;
		recording.submitted_cutoff = 4; recording.completed_cutoff = 2; recording.exclusive = recording.valid = true;
		LabelCallbacks callbacks;
		if (bytes == 8) { callbacks.dst_gpu_addr64 = reinterpret_cast<uint64_t*>(ticket.Address()); callbacks.value64 = ticket.Value(); }
		else { callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(ticket.Address()); callbacks.value32 = static_cast<uint32_t>(ticket.Value()); }
		callbacks.write_bytes = bytes;
		if (InstallLabelScalarCapture(&callbacks, LabelScalarCaptureTicket {}, recording) || callbacks.scalar_capture.IsCaptured()) return false;
		for (uint32_t field = 0; field != 19; ++field)
		{
			auto changed = recording;
			switch (field)
			{
				case 0: changed.processor = nullptr; break;
				case 1: changed.buffer = nullptr; break;
				case 2: changed.parent = nullptr; break;
				case 3: changed.pool = nullptr; break;
				case 4: ++changed.current_index; break;
				case 5: ++changed.processor_queue; break;
				case 6: ++changed.buffer_queue; break;
				case 7: ++changed.pool_index; break;
				case 8: ++changed.submit_id; break;
				case 9: ++changed.processor_epoch; break;
				case 10: ++changed.assigned_serial; break;
				case 11: ++changed.recording; break;
				case 12: ++changed.session; break;
				case 13: ++changed.serial; break;
				case 14: ++changed.ordinal; break;
				case 15: ++changed.submitted_cutoff; break;
				case 16: ++changed.completed_cutoff; break;
				case 17: changed.exclusive = false; break;
				case 18: changed.valid = false; break;
			}
			if (InstallLabelScalarCapture(&callbacks, ticket, changed) || callbacks.scalar_capture.IsCaptured() ||
			    callbacks.destination_generation != UINT64_MAX) return false;
		}
		for (uint32_t field = 0; field != 6; ++field)
		{
			auto changed = callbacks;
			switch (field)
			{
				case 0: ++changed.write_bytes; break;
				case 1: ++changed.value64; ++changed.value32; break;
				case 2: changed.dst_gpu_addr64 = reinterpret_cast<uint64_t*>(ticket.Address() + 8); changed.dst_gpu_addr32 = nullptr; break;
				case 3: changed.callback_1 = +[](const uint64_t*, void*, uint64_t) { return true; }; break;
				case 4: changed.callback_2 = +[](const uint64_t*, void*, uint64_t) { return true; }; break;
				case 5: changed.preparation_role = LabelGpuObject::PreparationRole::LockFreeWriteBack; break;
			}
			if (InstallLabelScalarCapture(&changed, ticket, recording) || changed.scalar_capture.IsCaptured() ||
			    changed.destination_generation != UINT64_MAX) return false;
		}
		if (!InstallLabelScalarCapture(&callbacks, ticket, recording) || callbacks.destination_generation != destination.Generation() ||
		    callbacks.scalar_capture.Generation() != destination.Generation() || CurrentThreadHasMemoryLease()) return false;
		std::atomic<uint64_t> pending {0};
		callbacks.publication = std::make_shared<LabelPublicationState>();
		callbacks.publication->aggregate = &pending;
		if (!QueueLabelPublication(callbacks.publication.get(), cp, recording.buffer_queue, recording.session, recording.serial)) return false;
		callbacks.publication_sequence = 1;
		callbacks.delivery_identity = SnapshotLabelPublication(*callbacks.publication);
		uint32_t callback_effects = 0;
		callbacks.args[0] = reinterpret_cast<uint64_t>(&callback_effects);
		if (scenario == 1 && (!Protect(owned.address, 65536, Mode::Read) || !Protect(owned.address, 65536, Mode::ReadWrite))) return false;
		if (scenario == 2)
		{
			if (!Free(owned.address) || !AllocFixed(owned.address, 65536, Mode::ReadWrite)) return false;
			std::memset(reinterpret_cast<void*>(owned.address), 0xa5, 65536);
		}
		if (scenario == 3)
		{
			const uint64_t unrelated = Alloc(0, 65536, Mode::ReadWrite);
			if (unrelated == 0 || !Free(unrelated)) return false;
		}
		if (scenario == 4) callbacks.callback_1 = +[](const uint64_t* args, void*, uint64_t) {
			++*reinterpret_cast<uint32_t*>(args[0]); return true;
		};
		if (scenario == 5) { ++callbacks.value64; ++callbacks.value32; }
		if (scenario == 6) ++callbacks.delivery_identity.submit_serial;
		if (scenario == 7) ++callbacks.destination_generation;
		LabelPublicationFailure failure;
		const bool delivered = TryFireLabelCallbacksWithLeaseRetry(callbacks, &failure, ValidateLabelDestination, [] {});
		const bool expected = scenario == 0 || scenario == 3;
		if (delivered != expected || callbacks.publication->completed.load() != uint64_t(expected) ||
		    callback_effects != 0 || CurrentThreadHasMemoryLease() || callbacks.scalar_capture.Generation() != destination.Generation()) return false;
		for (uint64_t offset = 0; offset != 65536; ++offset)
		{
			const uint8_t expected_byte = expected && offset >= 128 && offset < 128 + bytes
			    ? static_cast<uint8_t>(ticket.Value() >> (8 * (offset - 128))) : 0xa5;
			if (*reinterpret_cast<const uint8_t*>(owned.address + offset) != expected_byte) return false;
		}
		if (expected && (TryFireLabelCallbacksWithLeaseRetry(callbacks, &failure, ValidateLabelDestination, [] {}) ||
		    callbacks.publication->completed.load() != 1 || CurrentThreadHasMemoryLease())) return false;
		CancelLabelPublication(callbacks.publication.get());
		if (pending.load() != 0) return false;
	}
	std::fprintf(stderr, "[GFX_LABEL_SCALAR_CAPTURE_TEST] scalar4/8=pass original-token=retained enrollment-identity-fields19=reject"
	    " width/value/address/callbacks/role=reject worker-producer/payload/token=reject protect-restore/remap=no-store"
	    " unrelated-VM=accepted store/completion=once whole-allocation-sentinel=preserved callback-effects=0 cases=16"
	    " recording/Vulkan=modeled VM/store/publication=real\n");
#endif
	return true;
}

bool LabelDeferredCaptureContractTest()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
#if defined(LUMEN_FEXCORE_RUNTIME)
	// Mirror the natural Label producer's admission order: FEX exclusive,
	// adopted Core owner, bypass, Run lock and a recursively held CP lock.
	// The recording itself is modeled; acquisition, continuity, enrollment and
	// deferred publication all use their production implementations.
	struct ExternalEpoch
	{
		uint64_t token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
		    LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
		~ExternalEpoch()
		{
			if (token != 0 && LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) != 1)
				std::abort();
		}
	};
	enum class OwnerScenario { Stable, Unrelated, ProtectRestore, Remap, WorkerDuringOwner };
	uint32_t external_owner_cases = 0;
	for (const uint64_t bytes: {uint64_t {4}, uint64_t {8}})
	for (const auto scenario: {OwnerScenario::Stable, OwnerScenario::Unrelated, OwnerScenario::ProtectRestore,
	    OwnerScenario::Remap, OwnerScenario::WorkerDuringOwner})
	{
		const bool worker_case = scenario == OwnerScenario::WorkerDuringOwner;
		LabelTestAllocation owned;
		if (owned.address == 0) return false;
		std::memset(reinterpret_cast<void*>(owned.address), 0xa5, 65536);
		const uint64_t value = bytes == 4 ? 0x11223343ull : 0x1122334455667743ull;
		const LabelGpuObject description(value, nullptr, nullptr);
		LabelDeferredCaptureTicket original;
		original.m_address = owned.address + 128; original.m_size = bytes;
		std::copy_n(description.params, original.m_params.size(), original.m_params.begin());
		int buffer_key = 0, pool_key = 0;
		auto* cp = reinterpret_cast<CommandProcessor*>(uintptr_t {0x6132});
		auto& recording = original.m_recording;
		recording.processor = recording.parent = cp; recording.buffer = &buffer_key; recording.pool = &pool_key;
		recording.current_index = recording.pool_index = 0;
		recording.processor_queue = recording.buffer_queue = GraphicContext::QUEUE_GFX;
		recording.submit_id = 9; recording.processor_epoch = recording.session = 3;
		recording.assigned_serial = recording.serial = 5; recording.recording = 7; recording.ordinal = 0;
		recording.submitted_cutoff = 4; recording.completed_cutoff = 2;
		recording.exclusive = false; recording.valid = true;
		std::atomic<uint64_t> pending {0};
		LabelCallbacks callbacks;
		if (bytes == 8) { callbacks.dst_gpu_addr64 = reinterpret_cast<uint64_t*>(original.Address()); callbacks.value64 = value; }
		else { callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(original.Address()); callbacks.value32 = static_cast<uint32_t>(value); }
		callbacks.write_bytes = bytes;
		callbacks.publication = std::make_shared<LabelPublicationState>();
		callbacks.publication->aggregate = &pending;
		uint32_t calls = 0, gaps = 0;
		bool worker_delivered = false;
		LabelPublicationFailure failure;
		{
			ExternalEpoch epoch;
			if (epoch.token == 0 || LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(epoch.token) != 1) return false;
			GuestMemoryMutationOwnerScope owner(epoch.token);
			if (!owner.IsActive()) return false;
			GuestCpu::MemoryHandoffBypass bypass;
			Core::Mutex processor_mutex, run_mutex;
			Core::LockGuard run_lock(run_mutex), processor_lock(processor_mutex), nested_processor_lock(processor_mutex);
			bool locks_retained = false;
			std::thread observer([&] {
				const bool processor_available = processor_mutex.TryLock();
				const bool run_available = run_mutex.TryLock();
				locks_retained = !processor_available && !run_available;
				if (run_available) run_mutex.Unlock();
				if (processor_available) processor_mutex.Unlock();
			});
			observer.join();
			if (!locks_retained || CurrentThreadHasMemoryLease()) return false;
			original.m_generation = GetMutationGeneration();
			const auto captured = CaptureLabelDeferredWithContinuation(original,
			    [&](uint64_t begin, uint64_t length, Mode mode, GuestDataLease* lease, GuestDataLeaseDenialMetadata* denial) {
				    ++calls;
				    return TryAcquireGuestDataLease(begin, length, mode, lease, false, denial);
			    }, [&] { return recording; }, [&] { ++gaps; });
			if (!captured.captured || captured.rounds != 1 || captured.total_attempts != 1 || calls != 1 || gaps != 0 ||
			    captured.denials.failures != 0 || CurrentThreadHasMemoryLease() || pending != 0 ||
			    callbacks.publication->queued != 0 || callbacks.publication->completed != 0)
			{
				std::fprintf(stderr, "[GFX_LABEL_EXTERNAL_OWNER_FAILURE] scenario=%u bytes=%" PRIu64
				    " captured=%u rounds=%u attempts=%u gaps=%u reason=%s\n", static_cast<unsigned>(scenario), bytes,
				    captured.captured ? 1u : 0u, captured.rounds, captured.total_attempts, gaps,
				    captured.acquisition.reason != nullptr ? captured.acquisition.reason : "none");
				return false;
			}
			if (!InstallLabelDeferredCapture(&callbacks, original, recording) || callbacks.scalar_capture.IsCaptured() ||
			    callbacks.destination_generation != original.Generation()) return false;
			{
				GuestDataLease checked;
				if (!ValidateLabelDestination(callbacks, &checked) || !checked.IsActive()) return false;
			}
			if (CurrentThreadHasMemoryLease() ||
			    !QueueLabelPublication(callbacks.publication.get(), cp, recording.buffer_queue, recording.session, recording.serial)) return false;
			callbacks.publication_sequence = 1;
			callbacks.delivery_identity = SnapshotLabelPublication(*callbacks.publication);
			// Capture and enrollment alone authorize no destination store.
			for (uint64_t offset = 0; offset != 65536; ++offset)
				if (*reinterpret_cast<const uint8_t*>(owned.address + offset) != 0xa5) return false;
			if (worker_case)
			{
				// The completion thread inherits neither the producer's FEX token
				// nor its bypass. Exercise the full checked Label publication while
				// that producer retains its epoch and recursive processor locks.
				bool worker_released = false;
				std::thread completion([&] {
					worker_delivered = TryFireLabelCallbacksWithLeaseRetry(callbacks, &failure, ValidateLabelDestination, [] {});
					worker_released = !CurrentThreadHasMemoryLease();
				});
				completion.join();
				if (!worker_delivered || !worker_released || failure.preflight_attempts != 0 || failure.publication_attempts != 1 ||
				    pending != 0 || callbacks.publication->completed != 1 ||
				    LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(epoch.token) != 1)
				{
					std::fprintf(stderr, "[GFX_LABEL_EXTERNAL_OWNER_WORKER_FAILURE] bytes=%" PRIu64
					    " delivered=%u released=%u phase=%s reason=%s preflight_attempts=%u publication_attempts=%u\n",
					    bytes, worker_delivered ? 1u : 0u, worker_released ? 1u : 0u, failure.phase,
					    failure.reason != nullptr ? failure.reason : "none", failure.preflight_attempts, failure.publication_attempts);
					return false;
				}
			}
		}
		if (CurrentThreadHasMemoryLease() || pending != uint64_t(!worker_case) || callbacks.publication->queued != 1 ||
		    callbacks.publication->completed != uint64_t(worker_case)) return false;
		if (scenario == OwnerScenario::Unrelated)
		{
			const uint64_t unrelated = Alloc(0, 65536, Mode::ReadWrite);
			if (unrelated == 0 || !Free(unrelated)) return false;
		}
		if (scenario == OwnerScenario::ProtectRestore &&
		    (!Protect(owned.address, 65536, Mode::Read) || !Protect(owned.address, 65536, Mode::ReadWrite))) return false;
		if (scenario == OwnerScenario::Remap)
		{
			if (!Free(owned.address) || !AllocFixed(owned.address, 65536, Mode::ReadWrite)) return false;
			std::memset(reinterpret_cast<void*>(owned.address), 0xa5, 65536);
		}
		const bool delivered = worker_case ? worker_delivered :
		    TryFireLabelCallbacksWithLeaseRetry(callbacks, &failure, ValidateLabelDestination, [] {});
		const bool expected = scenario == OwnerScenario::Stable || scenario == OwnerScenario::Unrelated || worker_case;
		if (delivered != expected || callbacks.publication->completed != uint64_t(expected) ||
		    callbacks.destination_generation != original.Generation() || callbacks.scalar_capture.IsCaptured() ||
		    CurrentThreadHasMemoryLease() || calls != 1 || gaps != 0 ||
		    (!expected && (failure.reason == nullptr || std::strcmp(failure.reason, "mapping-continuity-changed") != 0))) return false;
		for (uint64_t offset = 0; offset != 65536; ++offset)
		{
			const uint8_t expected_byte = expected && offset >= 128 && offset < 128 + bytes ?
			    static_cast<uint8_t>(value >> (8 * (offset - 128))) : 0xa5;
			if (*reinterpret_cast<const uint8_t*>(owned.address + offset) != expected_byte) return false;
		}
		if (expected && (TryFireLabelCallbacksWithLeaseRetry(callbacks, &failure, ValidateLabelDestination, [] {}) ||
		    callbacks.publication->completed != 1 || CurrentThreadHasMemoryLease())) return false;
		CancelLabelPublication(callbacks.publication.get());
		if (pending != 0) return false;
		++external_owner_cases;
	}
	std::fprintf(stderr, "[GFX_LABEL_EXTERNAL_OWNER_TEST] cases=%u natural4/8=pass nonexclusive-recording=pass"
	    " FEX-exclusive/Core-owner/bypass=real CP-depth2/Run-lock=real initial-attempts=1 gaps=0"
	    " worker-during-owner4/8=pass worker-bypass=none worker-preflight/publication-attempts=0/1"
	    " capture/enrollment-effects=0 original-token=retained scalar-authority=none"
	    " unrelated-VM=accepted protect-restore/remap=no-store-no-completion delivery/completion=once"
	    " whole-allocation-canaries=pass recording/Vulkan=modeled VM/store/publication=real\n", external_owner_cases);
#endif
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
	// Force the real Core admission mutex to remain owned by another thread.
	// The real-contention cases inject no lease result or denial cause.
	struct GateHolder
	{
		std::atomic_bool entered {false}, release {false}, expired {false};
		std::thread worker;
		GateHolder(): worker([this] {
			WithGuestDataGateLockedForTest(+[](void* context) {
				auto& holder = *static_cast<GateHolder*>(context);
				holder.entered.store(true, std::memory_order_release);
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
				while (!holder.release.load(std::memory_order_acquire))
				{
					if (std::chrono::steady_clock::now() >= deadline)
					{
						holder.expired.store(true, std::memory_order_release);
						break;
					}
					std::this_thread::yield();
				}
			}, this);
		}) {}
		bool Await()
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (!entered.load(std::memory_order_acquire))
			{
				if (std::chrono::steady_clock::now() >= deadline) return false;
				std::this_thread::yield();
			}
			return !expired.load(std::memory_order_acquire);
		}
		void Release()
		{
			release.store(true, std::memory_order_release);
			if (worker.joinable()) worker.join();
		}
		~GateHolder() { Release(); }
	};
#endif
	enum class Scenario { Recover, RecoverLast, Exhaust, Protect, ProtectRestore, Remap, Unrelated,
	    RecordingBeforeGap, RecordingAfterGap, HeldLease, MutationActive, MutationRecover, MutationChanged,
	    OtherDenial, InactiveLease, WrongRange, RealRecover, RealExhaust,
	    PostProtectRestore, PostRemap, PostPayload, PostToken, PostProducer, PostCancel, PostSequence };
	uint32_t cases = 0, real_cases = 0;
	for (const uint64_t bytes: {uint64_t {4}, uint64_t {8}})
	for (const bool exclusive: {false, true})
	for (const bool notify: {false, true})
	for (const auto scenario: {Scenario::Recover, Scenario::RecoverLast, Scenario::Exhaust, Scenario::Protect,
	    Scenario::ProtectRestore, Scenario::Remap, Scenario::Unrelated, Scenario::RecordingBeforeGap,
	    Scenario::RecordingAfterGap, Scenario::HeldLease, Scenario::MutationActive, Scenario::MutationRecover,
	    Scenario::MutationChanged, Scenario::OtherDenial,
	    Scenario::InactiveLease, Scenario::WrongRange,
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
	    Scenario::RealRecover, Scenario::RealExhaust,
#endif
	    Scenario::PostProtectRestore, Scenario::PostRemap,
	    Scenario::PostPayload, Scenario::PostToken, Scenario::PostProducer, Scenario::PostCancel, Scenario::PostSequence})
	{
		++cases;
		const bool real_contention = scenario == Scenario::RealRecover || scenario == Scenario::RealExhaust;
		if (real_contention) ++real_cases;
		LabelTestAllocation owned;
		if (owned.address == 0) return false;
		std::memset(reinterpret_cast<void*>(owned.address), 0xa5, 65536);
		struct Probe { uint32_t prepares = 0, notifications = 0; bool valid = true; } probe;
		const uint64_t args[LABEL_ARGS_MAX] = {reinterpret_cast<uint64_t>(&probe), 1, 2, 3};
		const auto preparation = +[](const uint64_t* args, void*, uint64_t) {
			auto& value = *reinterpret_cast<Probe*>(args[0]);
			++value.prepares; value.valid &= !CurrentThreadHasMemoryLease(); return true;
		};
		const auto notification = +[](const uint64_t* args, void*, uint64_t) {
			auto& value = *reinterpret_cast<Probe*>(args[0]);
			++value.notifications; value.valid &= !CurrentThreadHasMemoryLease(); return true;
		};
		const uint64_t value = bytes == 4 ? 0x11223343ull : 0x1122334455667743ull;
		const LabelGpuObject description(value, preparation, notify ? notification : nullptr, args, 0,
		    notify ? LabelGpuObject::PreparationRole::LockFreeWriteBack : LabelGpuObject::PreparationRole::Ordinary);
		LabelDeferredCaptureTicket original;
		original.m_address = owned.address + 128; original.m_size = bytes; original.m_generation = GetMutationGeneration();
		std::copy_n(description.params, original.m_params.size(), original.m_params.begin());
		int buffer_key = 0, pool_key = 0;
		auto* cp = reinterpret_cast<CommandProcessor*>(uintptr_t {0x6132});
		auto& recording = original.m_recording;
		recording.processor = recording.parent = cp; recording.buffer = &buffer_key; recording.pool = &pool_key;
		recording.current_index = 0; recording.pool_index = 0;
		recording.processor_queue = recording.buffer_queue = GraphicContext::QUEUE_GFX;
		recording.submit_id = 9; recording.processor_epoch = recording.session = 3;
		recording.assigned_serial = recording.serial = 5; recording.recording = 7; recording.ordinal = 0;
		recording.submitted_cutoff = 4; recording.completed_cutoff = 2; recording.exclusive = exclusive; recording.valid = true;
		auto generic = original;
		const char* generic_reason = "stale";
		if (!LabelCaptureDeferredDestination(nullptr, original.Address(), bytes, description, &generic, &generic_reason) ||
		    generic.IsCaptured() || generic_reason != nullptr) return false;
		auto current = recording;
		Core::Mutex processor_mutex, run_mutex, renderer_mutex, tracker_mutex, manager_mutex;
		Core::LockGuard processor_lock(processor_mutex), run_lock(run_mutex);
		std::atomic<uint64_t> pending {0};
		auto publication = std::make_shared<LabelPublicationState>();
		publication->aggregate = &pending;
		GuestDataLease held;
		ReservationHandle reservation;
		MutationTransaction mutation;
		if (scenario == Scenario::HeldLease && !TryAcquireGuestDataLease(owned.address, 4, Mode::Read, &held, false)) return false;
		const bool mutation_case = scenario == Scenario::MutationActive || scenario == Scenario::MutationRecover ||
		    scenario == Scenario::MutationChanged;
		if (mutation_case && !BeginReserveAligned(0, 65536, 65536, &reservation, &mutation)) return false;
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
		std::unique_ptr<GateHolder> gate_holder;
		if (real_contention)
		{
			gate_holder = std::make_unique<GateHolder>();
			if (!gate_holder->Await()) return false;
		}
#endif
		uint32_t calls = 0, waits = 0, actual_mutation_denials = 0, actual_trylock_denials = 0;
		const auto captured = CaptureLabelDeferredWithContinuation(original,
		    [&](uint64_t begin, uint64_t length, Mode mode, GuestDataLease* lease, GuestDataLeaseDenialMetadata* denial) {
			    ++calls;
			    probe.valid &= probe.prepares == 0 && probe.notifications == 0 && pending == 0 && publication->queued == 0;
			    if (mutation_case && mutation.IsActive())
			    {
				    const bool acquired = TryAcquireGuestDataLease(begin, length, mode, lease, false, denial);
				    probe.valid &= !acquired && denial->reason == GuestDataLeaseDenialReason::GateMutationActive;
				    if (!acquired) ++actual_mutation_denials;
				    return acquired;
			    }
			    if (scenario == Scenario::InactiveLease) return true;
			    if (scenario == Scenario::WrongRange) return TryAcquireGuestDataLease(begin + 8, length, mode, lease, false, denial);
			    const bool acquired = TryAcquireGuestDataLease(begin, length, mode, lease, false, denial);
			    if (real_contention)
			    {
				    uint64_t destination = 0;
				    std::memcpy(&destination, reinterpret_cast<const void*>(original.Address()), sizeof(destination));
				    probe.valid &= begin == original.Address() && length == bytes && mode == Mode::Write &&
				        publication->completed == 0 && publication->cancelled == 0 && destination == 0xa5a5a5a5a5a5a5a5ull;
				    if (calls <= 16 || scenario == Scenario::RealExhaust)
				    {
					    probe.valid &= !acquired && !lease->IsActive() &&
					        denial->reason == GuestDataLeaseDenialReason::GateTrylockUnavailable &&
					        denial->address == begin && denial->bytes == length && denial->required == mode &&
					        denial->acquisition_thread != 0 && !denial->gate_observed && !denial->mutation_active &&
					        denial->waiting_mutations == UINT64_MAX && denial->code_readers == UINT64_MAX && !denial->physical_observed;
					    if (!acquired && denial->reason == GuestDataLeaseDenialReason::GateTrylockUnavailable) ++actual_trylock_denials;
				    }
				    else probe.valid &= acquired && lease->IsActive() && lease->HostBegin() == begin && lease->ByteCount() == bytes;
				    return acquired;
			    }
			    const uint32_t modeled = scenario == Scenario::Exhaust ? 256u : scenario == Scenario::RecoverLast ? 240u : 16u;
			    if (calls <= modeled && acquired)
			    {
				    // Real checked leases exercise release/no-effects. Only the
				    // deterministic trylock classification is modeled; Run21 is
				    // the separate actual runtime witness of this Core gate cause.
				    probe.valid &= lease->IsActive() && lease->HostBegin() == original.Address() && lease->ByteCount() == bytes;
				    denial->reason = scenario == Scenario::OtherDenial && calls == 8 ?
				        GuestDataLeaseDenialReason::GateWriterPending : GuestDataLeaseDenialReason::GateTrylockUnavailable;
				    if (scenario == Scenario::RecordingBeforeGap && calls == 16) ++current.serial;
				    return false;
			    }
			    return acquired;
		    }, [&] { return current; }, [&] {
			    ++waits;
			    probe.valid &= !CurrentThreadHasMemoryLease() && probe.prepares == 0 && probe.notifications == 0 &&
			        pending == 0 && publication->queued == 0;
			    if (exclusive) processor_mutex.Unlock();
			    bool locks_valid = false;
			    std::thread other([&] {
				    const bool processor_available = processor_mutex.TryLock();
				    const bool renderer_available = renderer_mutex.TryLock();
				    const bool tracker_available = tracker_mutex.TryLock();
				    const bool manager_available = manager_mutex.TryLock();
				    const bool run_available = run_mutex.TryLock();
				    locks_valid = processor_available == exclusive && renderer_available && tracker_available && manager_available && !run_available;
				    if (run_available) run_mutex.Unlock();
				    if (manager_available) manager_mutex.Unlock();
				    if (tracker_available) tracker_mutex.Unlock();
				    if (renderer_available) renderer_mutex.Unlock();
				    if (processor_available) processor_mutex.Unlock();
			    });
			    other.join();
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
			    if (scenario == Scenario::RealRecover) gate_holder->Release();
#endif
			    if (exclusive) processor_mutex.Lock();
			    probe.valid &= locks_valid;
			    if (scenario == Scenario::RecordingAfterGap) ++current.serial;
			    if (scenario == Scenario::MutationRecover || scenario == Scenario::MutationChanged)
			    {
				    probe.valid &= mutation.Abort();
				    if (scenario == Scenario::MutationChanged)
					    probe.valid &= Protect(owned.address, 65536, Mode::Read) && Protect(owned.address, 65536, Mode::ReadWrite);
			    }
			    if (scenario == Scenario::Protect || scenario == Scenario::ProtectRestore)
				    probe.valid &= Protect(owned.address, 65536, Mode::Read) &&
				        (scenario != Scenario::ProtectRestore || Protect(owned.address, 65536, Mode::ReadWrite));
			    if (scenario == Scenario::Remap)
			    {
				    probe.valid &= Free(owned.address) && AllocFixed(owned.address, 65536, Mode::ReadWrite);
				    if (probe.valid) std::memset(reinterpret_cast<void*>(owned.address), 0xa5, 65536);
			    }
			    if (scenario == Scenario::Unrelated)
			    {
				    const uint64_t unrelated = Alloc(0, 65536, Mode::ReadWrite);
				    probe.valid &= unrelated != 0 && Free(unrelated);
			    }
		    });
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
		if (gate_holder)
		{
			gate_holder->Release();
			probe.valid &= !gate_holder->expired.load(std::memory_order_acquire);
		}
#endif
		ReleaseGuestDataLease(&held);
		if (mutation.IsActive() && !mutation.Abort()) return false;
		const bool capture_success = scenario == Scenario::Recover || scenario == Scenario::RecoverLast ||
		    scenario == Scenario::Unrelated || scenario == Scenario::RealRecover || scenario == Scenario::MutationRecover ||
		    scenario >= Scenario::PostProtectRestore;
		ManagedRegionState denied_state = ManagedRegionState::Unmanaged;
		ManagedRegion denied_region {};
		// A known immutable rights denial ends the next round on attempt one.
		// Without that snapshot, Core refuses all 16 borrows of the read-only
		// destination; the helper must exhaust them without a second gap.
		const bool denied_snapshot_available = scenario == Scenario::Protect &&
		    TryQueryManagedRegionSnapshot(original.Address(), &denied_state, &denied_region);
		const uint32_t expected_calls = (scenario == Scenario::Exhaust || scenario == Scenario::RealExhaust || scenario == Scenario::MutationActive) ? 256u : scenario == Scenario::RecoverLast ? 241u :
		    scenario == Scenario::HeldLease ? 0u : scenario == Scenario::InactiveLease || scenario == Scenario::WrongRange ? 1u :
		    scenario == Scenario::RecordingBeforeGap || scenario == Scenario::RecordingAfterGap ||
		    scenario == Scenario::OtherDenial ? 16u :
		    scenario == Scenario::Protect && !denied_snapshot_available ? 32u : 17u;
		const uint32_t expected_waits = scenario == Scenario::Exhaust || scenario == Scenario::RealExhaust ||
		    scenario == Scenario::RecoverLast || scenario == Scenario::MutationActive ? 15u :
		    scenario == Scenario::HeldLease || scenario == Scenario::InactiveLease || scenario == Scenario::WrongRange ||
		    scenario == Scenario::RecordingBeforeGap || scenario == Scenario::OtherDenial ? 0u : 1u;
		if (!probe.valid || captured.captured != capture_success || calls != expected_calls || waits != expected_waits ||
		    captured.total_attempts != calls || CurrentThreadHasMemoryLease() || pending != 0 || publication->queued != 0 ||
		    probe.prepares != 0 || probe.notifications != 0 ||
		    (mutation_case && actual_mutation_denials != (scenario == Scenario::MutationActive ? 256u : 16u)) ||
		    (scenario == Scenario::Protect && (captured.acquisition.reason == nullptr ||
		        std::strcmp(captured.acquisition.reason, denied_snapshot_available ?
		            "destination-protection" : "lease-acquisition-retry-exhausted") != 0)) ||
		    (real_contention && (actual_trylock_denials != (scenario == Scenario::RealExhaust ? 256u : 16u) ||
		        captured.rounds != (scenario == Scenario::RealExhaust ? 16u : 2u) ||
		        (scenario == Scenario::RealRecover ? captured.acquisition.reason != nullptr :
		            captured.acquisition.reason == nullptr || std::strcmp(captured.acquisition.reason, "lease-acquisition-retry-exhausted") != 0))))
		{
			std::fprintf(stderr, "[GFX_LABEL_DEFERRED_CAPTURE_FAILURE] scenario=%u bytes=%" PRIu64 " notify=%u"
			    " valid=%u captured=%u expected=%u calls=%u:%u waits=%u:%u total_attempts=%u reason=%s\n",
			    static_cast<unsigned>(scenario), bytes, notify ? 1u : 0u, probe.valid ? 1u : 0u, captured.captured ? 1u : 0u,
			    capture_success ? 1u : 0u, calls, expected_calls, waits, expected_waits, captured.total_attempts,
			    captured.acquisition.reason != nullptr ? captured.acquisition.reason : "none");
			return false;
		}
		LabelCallbacks callbacks;
		if (bytes == 8) { callbacks.dst_gpu_addr64 = reinterpret_cast<uint64_t*>(original.Address()); callbacks.value64 = value; }
		else { callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(original.Address()); callbacks.value32 = static_cast<uint32_t>(value); }
		callbacks.callback_1 = preparation; callbacks.callback_2 = notify ? notification : nullptr;
		std::copy_n(args, LABEL_ARGS_MAX, callbacks.args); callbacks.write_bytes = bytes;
		callbacks.preparation_role = notify ? LabelGpuObject::PreparationRole::LockFreeWriteBack : LabelGpuObject::PreparationRole::Ordinary;
		if (capture_success)
		{
			for (uint32_t field = 0; field != 9; ++field)
			{
				auto changed = callbacks;
				switch (field)
				{
					case 0: ++changed.value32; ++changed.value64; break;
					case 1: changed.callback_1 = notification; break;
					case 2: changed.callback_2 = notify ? nullptr : notification; break;
					case 7: ++changed.write_bytes; break;
					case 8: changed.preparation_role = notify ? LabelGpuObject::PreparationRole::Ordinary : LabelGpuObject::PreparationRole::LockFreeWriteBack; break;
					default: ++changed.args[field - 3]; break;
				}
				if (InstallLabelDeferredCapture(&changed, original, recording) || changed.deferred_capture.IsCaptured() ||
				    changed.destination_generation != UINT64_MAX) return false;
			}
			for (uint32_t field = 0; field != 19; ++field)
			{
				auto changed = recording;
				switch (field)
				{
					case 0: changed.processor = nullptr; break; case 1: changed.buffer = nullptr; break;
					case 2: changed.parent = nullptr; break; case 3: changed.pool = nullptr; break;
					case 4: ++changed.current_index; break; case 5: ++changed.processor_queue; break;
					case 6: ++changed.buffer_queue; break; case 7: ++changed.pool_index; break;
					case 8: ++changed.submit_id; break; case 9: ++changed.processor_epoch; break;
					case 10: ++changed.assigned_serial; break; case 11: ++changed.recording; break;
					case 12: ++changed.session; break; case 13: ++changed.serial; break;
					case 14: ++changed.ordinal; break; case 15: ++changed.submitted_cutoff; break;
					case 16: ++changed.completed_cutoff; break; case 17: changed.exclusive = !exclusive; break;
					case 18: changed.valid = false; break;
				}
				if (InstallLabelDeferredCapture(&callbacks, original, changed) || callbacks.deferred_capture.IsCaptured() ||
				    callbacks.destination_generation != UINT64_MAX) return false;
			}
			if (!InstallLabelDeferredCapture(&callbacks, original, recording) || callbacks.scalar_capture.IsCaptured() ||
			    callbacks.destination_generation != original.Generation()) return false;
			callbacks.publication = publication;
			if (!QueueLabelPublication(publication.get(), cp, recording.buffer_queue, recording.session, recording.serial)) return false;
			callbacks.publication_sequence = 1; callbacks.delivery_identity = SnapshotLabelPublication(*publication);
			if (scenario == Scenario::PostProtectRestore && (!Protect(owned.address, 65536, Mode::Read) ||
			    !Protect(owned.address, 65536, Mode::ReadWrite))) return false;
			if (scenario == Scenario::PostRemap)
			{
				if (!Free(owned.address) || !AllocFixed(owned.address, 65536, Mode::ReadWrite)) return false;
				std::memset(reinterpret_cast<void*>(owned.address), 0xa5, 65536);
			}
			if (scenario == Scenario::PostPayload) ++callbacks.args[3];
			if (scenario == Scenario::PostToken) ++callbacks.destination_generation;
			if (scenario == Scenario::PostProducer) ++callbacks.delivery_identity.submit_serial;
			if (scenario == Scenario::PostCancel) CancelLabelPublication(publication.get());
			if (scenario == Scenario::PostSequence) publication->completed.store(1, std::memory_order_release);
			LabelPublicationFailure failure;
			const bool delivered = TryFireLabelCallbacksWithLeaseRetry(callbacks, &failure, ValidateLabelDestination, [] {});
			const bool expected = scenario == Scenario::Recover || scenario == Scenario::RecoverLast ||
			    scenario == Scenario::Unrelated || scenario == Scenario::RealRecover || scenario == Scenario::MutationRecover;
			if (delivered != expected || !probe.valid || probe.prepares != uint32_t(expected) ||
			    probe.notifications != uint32_t(expected && notify) || CurrentThreadHasMemoryLease() ||
			    (expected && (publication->completed != 1 || pending != 0 ||
			        TryFireLabelCallbacksWithLeaseRetry(callbacks, &failure, ValidateLabelDestination, [] {}))))
			{
				std::fprintf(stderr, "[GFX_LABEL_DEFERRED_DELIVERY_FAILURE] scenario=%u bytes=%" PRIu64 " notify=%u"
				    " delivered=%u expected=%u prepares=%u notifications=%u reason=%s\n",
				    static_cast<unsigned>(scenario), bytes, notify ? 1u : 0u, delivered ? 1u : 0u, expected ? 1u : 0u,
				    probe.prepares, probe.notifications, failure.reason != nullptr ? failure.reason : "none");
				return false;
			}
			if (scenario == Scenario::PostSequence) publication->completed.store(0, std::memory_order_release);
			CancelLabelPublication(publication.get());
		}
		for (uint64_t offset = 0; offset != 65536; ++offset)
		{
			const bool stored = capture_success && (scenario == Scenario::Recover || scenario == Scenario::RecoverLast ||
			    scenario == Scenario::Unrelated || scenario == Scenario::RealRecover || scenario == Scenario::MutationRecover);
			const uint8_t expected = stored && offset >= 128 && offset < 128 + bytes ?
			    static_cast<uint8_t>(value >> (8 * (offset - 128))) : 0xa5;
			if (*reinterpret_cast<const uint8_t*>(owned.address + offset) != expected) return false;
		}
		if (pending != 0) return false;
	}
	std::fprintf(stderr, "[GFX_LABEL_DEFERRED_CAPTURE_TEST] cases=%u natural4/8=pass callback1-only-ordinary=pass"
	    " callbacks/args/value/role9=exact recording-fields19=exact original-generation=retained"
	    " pure-trylock16/256=modeled+real-Core-mutex recover-round2/16=pass active-mutation16/256=real/bounded"
	    " active-mutation-unrelated-recover=pass active-mutation-destination-changed=closed"
	    " exclusive-CP-release/shared-CCB-CP-retained=real-mutex-modeled-CP run-retained=pass"
	    " no-renderer/tracker/manager/VM-lease=pass"
	    " protect/restore/remap/payload/token/producer/cancel/sequence=closed unrelated-VM=accepted"
	    " real-VM/store/publication=pass callback/store/completion=once scalar-authority=none"
	    " actual-Core-gate-cases=%u bounded-latch/RAII-join=pass whole-allocation-canaries=pass\n", cases, real_cases);

#endif
	return true;
}

static bool LabelSinglePublicationLeaseContractTest()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	enum class Scenario { Stable, ContentChanged, Protect, ProtectRestore, Remap, Unrelated, Cancel, Sequence };
	uint32_t cases = 0;
	for (const uint64_t bytes: {uint64_t {4}, uint64_t {8}, uint64_t {64}, uint64_t {49152}})
	for (const bool preparation: {false, true})
	for (const auto scenario: {Scenario::Stable, Scenario::ContentChanged, Scenario::Protect, Scenario::ProtectRestore,
	    Scenario::Remap, Scenario::Unrelated, Scenario::Cancel, Scenario::Sequence})
	{
		LabelTestAllocation owned;
		if (owned.address == 0) return false;
		std::memset(reinterpret_cast<void*>(owned.address), 0xa5, 65536);
		std::atomic<uint64_t> pending {0};
		LabelCallbacks callbacks;
		callbacks.publication = std::make_shared<LabelPublicationState>();
		callbacks.publication->aggregate = &pending;
		struct Probe
		{
			uint64_t address = 0, bytes = 0, value = 0, scalar_bytes = 0;
			LabelPublicationState* publication = nullptr;
			std::atomic<uint64_t>* pending = nullptr;
			uint32_t prepares = 0, notifications = 0;
			bool valid = true;
		} probe;
		probe.address = owned.address + (bytes == 49152 ? 0 : 128);
		probe.bytes = bytes;
		probe.value = bytes == 8 ? 0x1122334455667788ull : 0x55667788ull;
		probe.scalar_bytes = bytes == 8 ? 8 : 4;
		probe.publication = callbacks.publication.get();
		probe.pending = &pending;
		if (bytes == 8) { callbacks.dst_gpu_addr64 = reinterpret_cast<uint64_t*>(probe.address); callbacks.value64 = probe.value; }
		else { callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(probe.address); callbacks.value32 = static_cast<uint32_t>(probe.value); }
		callbacks.write_bytes = bytes;
		callbacks.args[0] = reinterpret_cast<uint64_t>(&probe);
		if (preparation) callbacks.callback_1 = +[](const uint64_t* args, void* output, uint64_t size) {
			auto& p = *reinterpret_cast<Probe*>(args[0]);
			++p.prepares;
			p.valid = p.valid && !CurrentThreadHasMemoryLease() && output != nullptr && size == p.bytes &&
			    reinterpret_cast<uint64_t>(output) != p.address;
			return true;
		};
		callbacks.callback_2 = +[](const uint64_t* args, void* output, uint64_t size) {
			auto& p = *reinterpret_cast<Probe*>(args[0]);
			++p.notifications;
			p.valid = p.valid && !CurrentThreadHasMemoryLease() && output == nullptr && size == 0 &&
			    p.pending->load() == 1 && p.publication->completed.load() == 0;
			for (uint64_t offset = 0; offset != p.bytes; ++offset)
			{
				const uint8_t expected = offset < p.scalar_bytes ? static_cast<uint8_t>(p.value >> (8 * offset)) : 0;
				p.valid = p.valid && *reinterpret_cast<const uint8_t*>(p.address + offset) == expected;
			}
			return true;
		};
		if (!CaptureLabelDestination(&callbacks) || CurrentThreadHasMemoryLease() ||
		    !QueueLabelPublication(callbacks.publication.get(), reinterpret_cast<CommandProcessor*>(uintptr_t {1}), 0, 1, 1)) return false;
		callbacks.publication_sequence = 1;
		const auto original_generation = callbacks.destination_generation;
		uint8_t untouched = 0xa5;
		if (scenario == Scenario::ContentChanged)
		{
			// Ordinary guest contents are mutable; only the captured mapping and
			// fresh publication permissions authorize this later overwrite.
			untouched = 0x96;
			std::memset(reinterpret_cast<void*>(owned.address), untouched, 65536);
		}
		if ((scenario == Scenario::Protect || scenario == Scenario::ProtectRestore) &&
		    !Protect(owned.address, 65536, Mode::Read)) return false;
		if (scenario == Scenario::ProtectRestore && !Protect(owned.address, 65536, Mode::ReadWrite)) return false;
		if (scenario == Scenario::Remap)
		{
			if (!Free(owned.address) || !AllocFixed(owned.address, 65536, Mode::ReadWrite)) return false;
			std::memset(reinterpret_cast<void*>(owned.address), untouched, 65536);
		}
		if (scenario == Scenario::Unrelated)
		{
			const uint64_t other = Alloc(0, 65536, Mode::ReadWrite);
			if (other == 0 || !Free(other)) return false;
		}
		if (scenario == Scenario::Cancel) CancelLabelPublication(callbacks.publication.get());
		if (scenario == Scenario::Sequence) callbacks.publication_sequence = 2;
		uint32_t validations = 0, acquired = 0;
		LabelPublicationFailure failure;
		const bool delivered = TryFireLabelCallbacksWithLeaseRetry(callbacks, &failure,
		    [&](const LabelCallbacks& label, GuestDataLease* lease, const char** reason, bool* unavailable) {
			    ++validations;
			    const bool valid = ValidateLabelDestination(label, lease, reason, unavailable);
			    if (valid) { ++acquired; probe.valid = probe.valid && lease->IsActive() && CurrentThreadHasMemoryLease(); }
			    return valid;
		    }, [] {});
		const bool expected = scenario == Scenario::Stable || scenario == Scenario::ContentChanged || scenario == Scenario::Unrelated;
		const uint32_t expected_acquisitions = expected ? (preparation ? 2u : 1u) : 0u;
		if (delivered != expected || !probe.valid || acquired != expected_acquisitions ||
		    probe.prepares != uint32_t(expected && preparation) || probe.notifications != uint32_t(expected) ||
		    CurrentThreadHasMemoryLease() || callbacks.destination_generation != original_generation ||
		    callbacks.publication->completed.load() != uint64_t(expected) ||
		    (expected && (validations != expected_acquisitions || failure.preflight_attempts != uint32_t(preparation) ||
		        failure.publication_attempts != 1 || pending.load() != 0)) ||
		    ((scenario == Scenario::Cancel || scenario == Scenario::Sequence) && validations != 0))
		{
			std::fprintf(stderr, "[GFX_LABEL_SINGLE_LEASE_FAILURE] scenario=%u bytes=%" PRIu64 " preparation=%u"
			    " delivered=%u valid=%u validations=%u acquired=%u prepares=%u notifications=%u reason=%s\n",
			    static_cast<unsigned>(scenario), bytes, preparation ? 1u : 0u, delivered ? 1u : 0u, probe.valid ? 1u : 0u,
			    validations, acquired, probe.prepares, probe.notifications, failure.reason != nullptr ? failure.reason : "none");
			return false;
		}
		for (uint64_t offset = 0; offset != 65536; ++offset)
		{
			uint8_t expected_byte = untouched;
			if (expected && owned.address + offset >= probe.address && owned.address + offset < probe.address + bytes)
			{
				const auto relative = owned.address + offset - probe.address;
				expected_byte = relative < probe.scalar_bytes ? static_cast<uint8_t>(probe.value >> (8 * relative)) : 0;
			}
			if (*reinterpret_cast<const uint8_t*>(owned.address + offset) != expected_byte) return false;
		}
		CancelLabelPublication(callbacks.publication.get());
		if (pending.load() != 0 || (scenario == Scenario::Protect && !Protect(owned.address, 65536, Mode::ReadWrite))) return false;
		++cases;
	}
	std::fprintf(stderr, "[GFX_LABEL_SINGLE_LEASE_TEST] cases=%u scalar4/8/range64/49152=pass"
	    " callback1-null=one-acquisition callback1-present=two-acquisitions"
	    " callback2=after-store-before-completion-without-lease"
	    " protect/protect-restore/remap/cancel/sequence=no-store-no-callback-no-completion"
	    " content-mutation/unrelated-VM=accepted whole-allocation-canaries=pass VM/store/publication=real\n", cases);
#endif
	return true;
}

bool LabelDestinationContractTest()
{
	if (!LabelCaptureAcquisitionContractTest() || !LabelLeaseRetryContractTest() || !LabelWriteBackProofScopeContractTest() ||
	    !LabelScalarCaptureContractTest() || !LabelDeferredCaptureContractTest() || !LabelSinglePublicationLeaseContractTest()) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	LabelTestAllocation owned;
	if (owned.address == 0) return false;
	struct Probe
	{
		uint64_t base = 0;
		uint64_t address = 0;
		uint64_t bytes = 0;
		uint32_t prepare = 0;
		uint32_t notify = 0;
		int mutation = 0;
		bool valid = true;
	} probe;
	probe.base = owned.address;
	const auto make = [&](uint64_t offset, uint64_t bytes)
	{
		probe.address = owned.address + offset;
		probe.bytes = bytes;
		probe.prepare = probe.notify = 0;
		probe.valid = true;
		std::memset(reinterpret_cast<void*>(owned.address), 0xa5, 65536);
		LabelCallbacks callbacks;
		callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(probe.address);
		callbacks.write_bytes = bytes;
		callbacks.args[0] = reinterpret_cast<uint64_t>(&probe);
		callbacks.callback_1 = +[](const uint64_t* args, void* output, uint64_t size)
		{
			auto& probe = *reinterpret_cast<Probe*>(args[0]);
			++probe.prepare;
			probe.valid = probe.valid && !CurrentThreadHasMemoryLease() && size == probe.bytes && output != nullptr &&
			              reinterpret_cast<uint64_t>(output) != probe.address;
			std::memset(output, 0x3c, size);
			if (probe.mutation == 1) probe.valid = Protect(probe.base, 65536, Mode::Read) && probe.valid;
			if (probe.mutation == 2)
			{
				probe.valid = Free(probe.base) && probe.valid;
				probe.valid = AllocFixed(probe.base, 65536, Mode::ReadWrite) && probe.valid;
				if (probe.valid) std::memset(reinterpret_cast<void*>(probe.base), 0x97, 65536);
			}
			if (probe.mutation == 3)
			{
				const uint64_t other = Alloc(0, 65536, Mode::ReadWrite);
				probe.valid = other != 0 && Free(other) && probe.valid;
			}
			return true;
		};
		callbacks.callback_2 = +[](const uint64_t* args, void* output, uint64_t size)
		{
			auto& probe = *reinterpret_cast<Probe*>(args[0]);
			++probe.notify;
			probe.valid = probe.valid && !CurrentThreadHasMemoryLease() && output == nullptr && size == 0;
			return true;
		};
		return callbacks;
	};
	const auto intact = [&](uint8_t expected)
	{
		const auto* data = reinterpret_cast<const uint8_t*>(owned.address);
		for (uint64_t i = 0; i != 65536; ++i) if (data[i] != expected) return false;
		return true;
	};
	for (uint64_t bytes: {4u, 8u, 64u, 49152u})
	{
		probe.mutation = 0;
		auto callbacks = make(bytes == 49152u ? 0 : 16380, bytes);
		if (!CaptureLabelDestination(&callbacks) || CurrentThreadHasMemoryLease() || !TryFireLabelCallbacks(callbacks) ||
		    !probe.valid || probe.prepare != 1 || probe.notify != 1) return false;
		const auto* data = reinterpret_cast<const uint8_t*>(owned.address);
		for (uint64_t i = 0; i != 65536; ++i)
			if (data[i] != (i >= probe.address - owned.address && i < probe.address - owned.address + bytes ? 0x3c : 0xa5)) return false;
	}
	// Revocation before preparation prevents every callback and publication.
	auto callbacks = make(0, 8);
	if (!CaptureLabelDestination(&callbacks) || !Protect(owned.address, 65536, Mode::Read) ||
	    TryFireLabelCallbacks(callbacks) || probe.prepare != 0 || probe.notify != 0 || !intact(0xa5) ||
	    !Protect(owned.address, 65536, Mode::ReadWrite) || TryFireLabelCallbacks(callbacks)) return false;
	for (int mutation: {1, 2, 3})
	{
		probe.mutation = mutation;
		callbacks = make(16380, 64);
		std::atomic<uint64_t> pending {0};
		auto publication = std::make_shared<LabelPublicationState>();
		publication->aggregate = &pending;
		callbacks.publication = publication;
		if (!QueueLabelPublication(publication.get(), reinterpret_cast<CommandProcessor*>(uintptr_t {1}), 0, 1, 1) ||
		    !CaptureLabelDestination(&callbacks)) return false;
		callbacks.publication_sequence = 1;
		const auto original_generation = callbacks.destination_generation;
		const bool expected_success = mutation == 3;
		if (TryFireLabelCallbacks(callbacks) != expected_success || !probe.valid || probe.prepare != 1 ||
		    probe.notify != (expected_success ? 1u : 0u) || callbacks.destination_generation != original_generation ||
		    SnapshotLabelPublication(*publication).completed != (expected_success ? 1u : 0u) ||
		    pending != (expected_success ? 0u : 1u)) return false;
		const auto* data = reinterpret_cast<const uint8_t*>(owned.address);
		for (uint64_t i = 0; i != 65536; ++i)
			if (data[i] != (mutation == 2 ? uint8_t {0x97} :
			    expected_success && i >= 16380 && i < 16444 ? uint8_t {0x3c} : uint8_t {0xa5})) return false;
		if (!expected_success)
		{
			CancelLabelPublication(publication.get());
			if (pending != 0 || SnapshotLabelPublication(*publication).cancelled != 1) return false;
		}
		if (mutation == 1 && !Protect(owned.address, 65536, Mode::ReadWrite)) return false;
	}
	probe.mutation = 0;
	callbacks = make(16380, 64);
	if (!Protect(owned.address + 16384, 16384, Mode::Read) || CaptureLabelDestination(&callbacks) ||
	    !Protect(owned.address + 16384, 16384, Mode::ReadWrite)) return false;
	callbacks = make(65532, 8);
	if (CaptureLabelDestination(&callbacks)) return false;
	callbacks = make(0, 8);
	if (!Protect(owned.address, 65536, Mode::ExecuteReadWrite) || CaptureLabelDestination(&callbacks) ||
	    !Protect(owned.address, 65536, Mode::ReadWrite)) return false;
	for (uint64_t bytes: {4u, 8u, 64u, 49152u})
	{
		if (!LabelObjectWriteShapeIsValid(bytes, bytes)) return false;
		for (uint64_t object_size: {0u, 4u, 8u, 64u, 49152u})
			if (object_size != bytes && LabelObjectWriteShapeIsValid(object_size, bytes)) return false;
	}
	if (!LabelObjectWriteShapeIsValid(4, 0) || !LabelObjectWriteShapeIsValid(8, 0) ||
	    LabelObjectWriteShapeIsValid(64, 0) || LabelObjectWriteShapeIsValid(3, 3) ||
	    LabelObjectWriteShapeIsValid(5, 5) || LabelObjectWriteShapeIsValid(49156, 49156)) return false;
	const LabelGpuObject small(0, nullptr, nullptr, nullptr, 4);
	const LabelGpuObject large(0, nullptr, nullptr, nullptr, 64);
	if (small.Equal(large.params) || !small.Equal(small.params)) return false;
	callbacks.dst_gpu_addr64 = reinterpret_cast<uint64_t*>(owned.address + 4);
	callbacks.dst_gpu_addr32 = nullptr;
	if (CaptureLabelDestination(&callbacks)) return false;
	callbacks.dst_gpu_addr64 = nullptr;
	callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(owned.address + 1);
	if (CaptureLabelDestination(&callbacks)) return false;
	uint64_t native = 0;
	callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(&native);
	if (CaptureLabelDestination(&callbacks)) return false;
	callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(UINT64_MAX - 3);
	if (CaptureLabelDestination(&callbacks)) return false;
	callbacks = make(0, LabelGpuObject::MAX_WRITE_BYTES + 4);
	if (CaptureLabelDestination(&callbacks) || TryFireLabelCallbacks(callbacks) || probe.prepare != 0) return false;
	// Two distinct adjacent Core mappings are valid only when the full footprint
	// is visible and writable under the same borrowed generation.
	if (!Free(owned.address) || !AllocFixed(owned.address, 16384, Mode::ReadWrite) ||
	    !AllocFixed(owned.address + 16384, 16384, Mode::ReadWrite)) return false;
	callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(owned.address + 16380);
	callbacks.write_bytes = 8;
	probe.address = owned.address + 16380;
	probe.bytes = 8;
	probe.prepare = probe.notify = 0;
	std::memset(reinterpret_cast<void*>(owned.address), 0xa5, 32768);
	if (!CaptureLabelDestination(&callbacks) || !TryFireLabelCallbacks(callbacks) || !probe.valid ||
	    probe.prepare != 1 || probe.notify != 1 ||
	    *reinterpret_cast<uint32_t*>(owned.address + 16376) != 0xa5a5a5a5 ||
	    *reinterpret_cast<uint32_t*>(owned.address + 16380) != 0x3c3c3c3c ||
	    *reinterpret_cast<uint32_t*>(owned.address + 16384) != 0x3c3c3c3c ||
	    *reinterpret_cast<uint32_t*>(owned.address + 16388) != 0xa5a5a5a5 ||
	    !Protect(owned.address + 16384, 16384, Mode::Read) || CaptureLabelDestination(&callbacks) ||
	    !Free(owned.address + 16384)) return false;
	std::fprintf(stderr, "[LABEL_DESTINATION_TEST] full-footprint=pass staging=pass preflight=pass revoke=reject remap=reject"
	                     " unrelated-mutation-during-prepare=accepted original-token=retained callbacks=once"
	                     " cross-region=pass unmanaged=reject lease-during-prepare-or-irq=none\n");
#endif
	return true;
}

static bool RunLabelCompletionContractTest(const LabelGpuObject& description, uint32_t* destination32, uint64_t* destination64,
                                          bool (*observe)(void*, bool), void* state)
{
	Label label;
	label.status = LabelStatus::Active;
	label.callbacks.dst_gpu_addr32 = destination32;
	label.callbacks.dst_gpu_addr64 = destination64;
	label.callbacks.value64 = description.params[LabelGpuObject::PARAM_VALUE];
	label.callbacks.value32 = static_cast<uint32_t>(description.params[LabelGpuObject::PARAM_VALUE]);
	label.callbacks.callback_1 = reinterpret_cast<LabelGpuObject::callback_t>(description.params[LabelGpuObject::PARAM_CALLBACK_1]);
	label.callbacks.callback_2 = reinterpret_cast<LabelGpuObject::callback_t>(description.params[LabelGpuObject::PARAM_CALLBACK_2]);
	if (!DecodeLabelPreparationRole(description.params[LabelGpuObject::PARAM_PREPARATION_ROLE],
	    &label.callbacks.preparation_role)) return false;
	for (int i = 0; i != LABEL_ARGS_MAX; ++i) label.callbacks.args[i] = description.params[LabelGpuObject::PARAM_ARG_1 + i];
	label.callbacks.write_bytes = destination64 != nullptr ? sizeof(uint64_t) : sizeof(uint32_t);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	Init();
	const uint64_t owned = Alloc(0, 65536, Mode::ReadWrite);
	if (owned == 0) return false;
	struct Owner { uint64_t address; ~Owner() { Free(address); } } owner {owned};
	if (destination64 != nullptr) { std::memcpy(reinterpret_cast<void*>(owned), destination64, sizeof(*destination64)); label.callbacks.dst_gpu_addr64 = reinterpret_cast<uint64_t*>(owned); }
	else { std::memcpy(reinterpret_cast<void*>(owned), destination32, sizeof(*destination32)); label.callbacks.dst_gpu_addr32 = reinterpret_cast<uint32_t*>(owned); }
#endif
	if (!CaptureLabelDestination(&label.callbacks)) return false;
	LabelCallbacks snapshot;
	if (SnapshotCompletedLabel(&label, VK_ERROR_DEVICE_LOST, &snapshot) || !observe(state, false)) return false;
	if (SnapshotCompletedLabel(&label, VK_EVENT_RESET, &snapshot) || !observe(state, false)) return false;
	if (!SnapshotCompletedLabel(&label, VK_EVENT_SET, &snapshot)) return false;
	// Simulate reuse of the producer record before deferred delivery: the worker
	// must retain its own argument values and exactly one completion.
	for (auto& arg: label.callbacks.args) arg = UINT64_MAX;
	FireLabelCallbacks(snapshot);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	if (destination64 != nullptr) std::memcpy(destination64, reinterpret_cast<void*>(owned), sizeof(*destination64));
	else std::memcpy(destination32, reinterpret_cast<void*>(owned), sizeof(*destination32));
#endif
	return observe(state, true) && !SnapshotCompletedLabel(&label, VK_EVENT_SET, &snapshot);
}

bool LabelCompletionContractTest(const LabelGpuObject& description, uint32_t* destination,
                                 bool (*observe)(void*, bool), void* state)
{
	return RunLabelCompletionContractTest(description, destination, nullptr, observe, state);
}

bool LabelCompletionContractTest64(const LabelGpuObject& description, uint64_t* destination,
                                   bool (*observe)(void*, bool), void* state)
{
	return RunLabelCompletionContractTest(description, nullptr, destination, observe, state);
}

void LabelManager::TraceLocked(const char* operation)
{
	if (!TraceEnabled()) return;
	const auto check = ++m_trace.checks;
	if (check > 3 && (check & 4095u) != 0) return;
	const auto now = std::chrono::steady_clock::now();
	if (m_trace.records == 0) m_trace.start = now;
	if (check > 3 && now - m_trace.last < std::chrono::seconds(5)) return;
	m_trace.last = now;
	++m_trace.records;
	std::fprintf(stderr,
	             "[LABEL_REGISTRY_TRACE] record=%u elapsedMs=%lld operation=%s registered=%zu peak=%zu "
	             "new=%" PRIu64 " active=%" PRIu64 " activeDeleted=%" PRIu64 " notActive=%" PRIu64
	             " destroyQueued=%u workerBatches=%u creates=%" PRIu64 " sets=%" PRIu64
	             " removeAttempts=%" PRIu64 " removed=%" PRIu64 " deferred=%" PRIu64 " resets=%" PRIu64
	             " scans=%" PRIu64 " visited=%" PRIu64 " eventQueries=%" PRIu64 " eventErrors=%" PRIu64 " ready=%" PRIu64
	             " delivered=%" PRIu64 " destroyStarted=%" PRIu64 " destroyFinished=%" PRIu64
	             " eventQueryUs=%" PRIu64 " callbackQueueUs=%" PRIu64 " callbackUs=%" PRIu64 " destroyUs=%" PRIu64
	             " batchWait=%u waitCalls=%" PRIu64 " waitAvoided=%" PRIu64 "\n",
	             m_trace.records, static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - m_trace.start).count()),
	             operation, m_labels.Size(), m_trace.peak, m_trace.states[New], m_trace.states[Active],
	             m_trace.states[ActiveDeleted], m_trace.states[NotActive], m_destroy_queue.Size(), m_worker_batches,
	             m_trace.creates, m_trace.sets, m_trace.remove_attempts, m_trace.removed, m_trace.deferred, m_trace.resets,
	             m_trace.scans, m_trace.visited, m_trace.event_queries, m_trace.event_errors, m_trace.ready, m_trace.delivered,
	             m_trace.destroy_started, m_trace.destroy_finished, m_trace.event_query_ns / 1000, m_trace.callback_queue_us,
	             m_trace.callback_us, m_trace.destroy_us, LabelBatchWaitEnabled() ? 1u : 0u,
	             m_trace.wait_calls, m_trace.wait_avoided);
}

void LabelManager::RegisterLabelLocked(Label* label)
{
	m_labels.Add(label);
	// Complete tracking is independent of selectors. Detached callbacks keep
	// these weak entries alive through their own shared publication reference.
	const size_t visited = AppendLabelPublication(&m_publications, &m_publication_compaction_watermark,
	    m_labels.Size(), label->callbacks.publication);
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	if (m_test_instance && visited != 0)
	{
		m_test_publication_compaction_visits += visited;
		++m_test_publication_compactions;
	}
#else
	(void)visited;
#endif
	if (TraceEnabled())
	{
		++m_trace.creates;
		++m_trace.states[New];
		m_trace.peak = std::max(m_trace.peak, m_labels.Size());
		TraceLocked("create");
	}
}

bool LabelManager::EnrollPublicationLocked(Label* label, CommandProcessor* cp, int queue, uint64_t epoch, uint64_t serial)
{
	if (!QueueLabelPublicationDelivery(&label->callbacks, cp, queue, epoch, serial)) return false;
	if (m_enrollment_epoch == UINT64_MAX) m_census_epoch_exhausted = true;
	else ++m_enrollment_epoch;
	return true;
}

void LabelManager::ThreadRun(void* data)
{
	auto* manager = static_cast<LabelManager*>(data);
	LabelWorkerScratch scratch;
	auto& deleted_labels = scratch.deleted;
	auto& fired_labels = scratch.fired;
	auto& ready_deleted_labels = scratch.ready_deleted;

	for (;;)
	{
		manager->m_mutex.Lock();
		int                    active_count = 0;
		(void)TakeLabelDestroyBatch(&manager->m_destroy_queue, &deleted_labels);

		if (!manager->m_session_resetting)
		{
			if (manager->TraceEnabled()) ++manager->m_trace.scans;
			const auto poll = [&](Label* label)
			{
				if (manager->TraceEnabled()) ++manager->m_trace.visited;
				if (label->status == LabelStatus::Active || label->status == LabelStatus::ActiveDeleted)
				{
					active_count++;
					if (manager->TraceEnabled()) ++manager->m_trace.event_queries;

					const bool trace_event = manager->TraceEnabled();
					const auto event_start = trace_event ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
					const auto event_status = vkGetEventStatus(label->device, label->event);
					if (trace_event)
					{
						manager->m_trace.event_query_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
						    std::chrono::steady_clock::now() - event_start).count());
						if (event_status != VK_EVENT_SET && event_status != VK_EVENT_RESET) ++manager->m_trace.event_errors;
					}
					// Device loss and driver errors cannot complete a guest label,
					// and must not leave the worker polling a failed event forever.
					EXIT_NOT_IMPLEMENTED(event_status != VK_EVENT_SET && event_status != VK_EVENT_RESET);
					if (event_status == VK_EVENT_SET)
					{
						if (label->status == LabelStatus::ActiveDeleted)
						{
							ready_deleted_labels.Add(label);
						}

						if (manager->TraceEnabled())
						{
							--manager->m_trace.states[label->status];
							++manager->m_trace.states[NotActive];
							++manager->m_trace.ready;
						}
						LabelCallbacks snapshot;
						EXIT_IF(!SnapshotCompletedLabel(label, event_status, &snapshot));
						fired_labels.Add(snapshot);
						return false;
					}
					return true;
				}
				return false;
			};
			if (LabelActiveFrontierEnabled())
			{
				manager->m_labels.ScanActive([&](Label* label) {
					EXIT_IF(label->status != LabelStatus::Active && label->status != LabelStatus::ActiveDeleted);
					return poll(label);
				});
			} else
			{
				for (auto* label: manager->m_labels) (void)poll(label);
				// Rollback scans the historical whole registry, but keeps the
				// frontier synchronized for its invariant/lifetime checks.
				manager->m_labels.PruneCompletedActive();
			}

			for (auto& label: ready_deleted_labels)
			{
				bool removed = manager->Remove(label, true);
				EXIT_NOT_IMPLEMENTED(!removed);
				if (removed)
				{
					deleted_labels.Add(label);
				}
			}
			scratch.FinishScan();
		}

		const bool callback_batch = !fired_labels.IsEmpty();
		if (callback_batch)
		{
			manager->m_worker_batches++;
		}
		manager->TraceLocked("worker");
		if (deleted_labels.IsEmpty() && !callback_batch &&
		    (manager->m_session_resetting || active_count == 0))
		{
			manager->m_cond_var.Wait(&manager->m_mutex);
		}

		const bool trace_batch = manager->TraceEnabled();
		const auto queued_at = trace_batch ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
		manager->m_mutex.Unlock();
		auto callback_start = queued_at;
		auto destroy_start = queued_at;
		bool first_callback = true;
		LabelBatchWait batch_wait(LabelBatchWaitEnabled());
		RunLabelWorkerBatch(fired_labels, deleted_labels,
		    [&](const LabelCallbacks& callbacks)
		    {
			    if (trace_batch && first_callback) callback_start = std::chrono::steady_clock::now();
			    first_callback = false;
			    DeliverLabelWorkerCallbacks(callbacks);
		    },
		    [&](Label* label)
		    {
			    if (trace_batch) destroy_start = std::chrono::steady_clock::now();
			    manager->Destroy(label, &batch_wait);
		    },
		    [&](bool callbacks, uint32_t count)
		    {
			    Core::LockGuard lock(manager->m_mutex);
			    EXIT_IF(manager->m_worker_batches == 0);
			    --manager->m_worker_batches;
			    if (manager->TraceEnabled())
			    {
				    const auto now = std::chrono::steady_clock::now();
				    if (callbacks)
				    {
					    manager->m_trace.delivered += count;
					    manager->m_trace.callback_queue_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(callback_start - queued_at).count());
					    manager->m_trace.callback_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - callback_start).count());
				    } else
				    {
					    ++manager->m_trace.destroy_finished;
					    manager->m_trace.destroy_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - destroy_start).count());
				    }
				    manager->TraceLocked(callbacks ? "callbacks_finish" : "destroy_finish");
			    }
			    manager->m_cond_var.SignalAll();
		    });

		GraphicsHostPollSleep(100);
		// The previous vectors were destroyed here, after the poll sleep and
		// before the next manager lock. Clear releases the same payloads while
		// retaining worker-private allocation capacity and its three mutexes.
		scratch.FinishRound();
	}
}

bool LabelManager::BeginSessionReset()
{
	constexpr auto reset_timeout = std::chrono::seconds(10);
	const auto     deadline      = std::chrono::steady_clock::now() + reset_timeout;

	// The worker queries VkEvent while owning this mutex. A driver stall must not
	// turn the main UIKit teardown into an unbounded lock acquisition: on expiry
	// leave every label/session object alive and let the runtime fail closed.
	while (!m_mutex.TryLock())
	{
		if (std::chrono::steady_clock::now() >= deadline)
		{
			std::fprintf(stderr, "iOS runtime label manager lock did not become available before the session-reset deadline\n");
			std::fflush(stderr);
			return false;
		}
		Core::Thread::SleepMicro(100);
	}
	if (m_session_resetting)
	{
		m_mutex.Unlock();
		return false;
	}
	m_session_resetting = true;
	if (m_reset_epoch == UINT64_MAX) m_census_epoch_exhausted = true;
	else ++m_reset_epoch;
	m_cond_var.Signal();
	while (m_worker_batches != 0)
	{
		const auto now = std::chrono::steady_clock::now();
		if (now >= deadline)
		{
			m_mutex.Unlock();
			std::fprintf(stderr, "iOS runtime label worker did not quiesce before the session-reset deadline\n");
			std::fflush(stderr);
			return false;
		}

		const auto remaining   = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
		const auto wait_micros = static_cast<uint32_t>(std::min<int64_t>(remaining.count(), 100000));
		(void)m_cond_var.WaitFor(&m_mutex, wait_micros);
	}
	m_mutex.Unlock();
	return true;
}

VkEvent LabelManager::AcquireEventLocked(VkDevice device)
{
	EXIT_IF(device == nullptr);

	VkEvent event = nullptr;
	if (AndroidFexEventPoolEnabled() && TakeRecycledLabelEvent(&m_recycled_events, device, &event))
	{
		m_event_reuses++;
		if (ShouldLogEventPoolCount(m_event_reuses))
		{
			std::fprintf(stderr,
			             "LUMEN_ANDROID_FEX_EVENT_POOL_V1 action=reuse count=%" PRIu64 " pooled=%zu\n",
			             m_event_reuses, m_recycled_events.Size());
			std::fflush(stderr);
		}
		return event;
	}

	VkEventCreateInfo create_info {};
	create_info.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
	create_info.pNext = nullptr;
	create_info.flags = 0;
	const VkResult result = vkCreateEvent(device, &create_info, nullptr, &event);
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS || event == nullptr);

	if (AndroidFexEventPoolEnabled())
	{
		m_event_creates++;
		if (ShouldLogEventPoolCount(m_event_creates))
		{
			std::fprintf(stderr,
			             "LUMEN_ANDROID_FEX_EVENT_POOL_V1 action=create count=%" PRIu64 " pooled=%zu\n",
			             m_event_creates, m_recycled_events.Size());
			std::fflush(stderr);
		}
	}
	return event;
}

void LabelManager::RecycleOrDestroyEvent(VkDevice device, VkEvent event)
{
	EXIT_IF(device == nullptr || event == nullptr);
	if (!AndroidFexEventPoolEnabled())
	{
		vkDestroyEvent(device, event, nullptr);
		return;
	}

	Core::LockGuard lock(m_mutex);
	// Device-local membership rejects duplicate ownership without scanning the
	// idle pool. Destroy still proves completion before entering this function.
	EXIT_IF(!m_recycled_events.Recycle(device, event));
	m_event_recycles++;
	if (ShouldLogEventPoolCount(m_event_recycles))
	{
		std::fprintf(stderr,
		             "LUMEN_ANDROID_FEX_EVENT_POOL_V1 action=recycle count=%" PRIu64 " pooled=%zu\n",
		             m_event_recycles, m_recycled_events.Size());
		std::fflush(stderr);
	}
}

bool LabelManager::FinishSessionReset()
{
	OrderedLabelRegistry::List labels;
	m_mutex.Lock();
	if (!m_session_resetting || m_worker_batches != 0 || !m_destroy_queue.IsEmpty())
	{
		m_mutex.Unlock();
		return false;
	}
	for (auto* label: m_labels)
	{
		if (label == nullptr || label->device == nullptr || label->event == nullptr)
		{
			m_mutex.Unlock();
			return false;
		}
	}
	m_labels.TakeAll(&labels);
	if (TraceEnabled())
	{
		m_trace.states.fill(0);
		++m_trace.resets;
		TraceLocked("reset_detach");
	}
	m_mutex.Unlock();

	// WindowFinishIOSRuntimeSession has already waited for the VkDevice, so no
	// command can still reference these events. Command-buffer retirement has
	// also run while deletion was deferred, so the manager is now the sole owner
	// of every remaining label. Guest writeback callbacks are intentionally
	// discarded during a session reset.
	for (auto* label: labels)
	{
		CancelLabelPublication(label->callbacks.publication.get());
		const VkDevice device = label->device;
		const VkEvent  event  = label->event;
		label->event           = nullptr;
		RecycleOrDestroyEvent(device, event);
		delete label;
	}

	m_mutex.Lock();
	m_session_resetting = false;
	m_cond_var.Signal();
	m_mutex.Unlock();
	return true;
}

void LabelManager::QueueDestroy(Label* label)
{
	EXIT_IF(label == nullptr);
	Core::LockGuard lock(m_mutex);
	EXIT_IF(m_destroy_queue.Contains(label));
	m_destroy_queue.Add(label);
	// BeginSessionReset and the worker share this condition.  Wake both: a
	// removal can reserve m_worker_batches immediately before reset starts, and
	// the worker must still drain that already-owned label while reset is gated.
	m_cond_var.SignalAll();
}

Label* LabelManager::Create64(GraphicContext* ctx, uint64_t* dst_gpu_addr, uint64_t value, LabelGpuObject::callback_t callback_1,
                              LabelGpuObject::callback_t callback_2, const uint64_t* args, uint64_t write_bytes,
                              LabelGpuObject::PreparationRole preparation_role)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(args == nullptr);
	LabelGpuObject::PreparationRole role;
	EXIT_NOT_IMPLEMENTED(!DecodeLabelPreparationRole(static_cast<uint64_t>(preparation_role), &role));

	Core::LockGuard lock(m_mutex);
	EXIT_IF(m_session_resetting);

	auto* label = new Label;

	label->status                   = LabelStatus::New;
	label->callbacks.dst_gpu_addr64 = dst_gpu_addr;
	label->callbacks.value64        = value;
	label->callbacks.dst_gpu_addr32 = nullptr;
	label->callbacks.value32        = 0;
	label->event                    = nullptr;
	label->device                   = ctx->device;
	label->callbacks.callback_1     = callback_1;
	label->callbacks.callback_2     = callback_2;
	label->callbacks.preparation_role = role;
	label->cp                       = nullptr;
	label->callbacks.publication    = std::make_shared<LabelPublicationState>();
	label->callbacks.write_bytes = dst_gpu_addr == nullptr ? 0 : (write_bytes == 0 ? sizeof(*dst_gpu_addr) : write_bytes);

	for (int i = 0; i < LABEL_ARGS_MAX; i++)
	{
		label->callbacks.args[i] = args[i];
	}

	label->event = AcquireEventLocked(ctx->device);

	RegisterLabelLocked(label);

	return label;
}

Label* LabelManager::Create32(GraphicContext* ctx, uint32_t* dst_gpu_addr, uint32_t value, LabelGpuObject::callback_t callback_1,
                              LabelGpuObject::callback_t callback_2, const uint64_t* args, uint64_t write_bytes,
                              LabelGpuObject::PreparationRole preparation_role)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(args == nullptr);
	LabelGpuObject::PreparationRole role;
	EXIT_NOT_IMPLEMENTED(!DecodeLabelPreparationRole(static_cast<uint64_t>(preparation_role), &role));

	Core::LockGuard lock(m_mutex);
	EXIT_IF(m_session_resetting);

	auto* label = new Label;

	label->status                   = LabelStatus::New;
	label->callbacks.dst_gpu_addr32 = dst_gpu_addr;
	label->callbacks.value32        = value;
	label->callbacks.dst_gpu_addr64 = nullptr;
	label->callbacks.value64        = 0;
	label->event                    = nullptr;
	label->device                   = ctx->device;
	label->callbacks.callback_1     = callback_1;
	label->callbacks.callback_2     = callback_2;
	label->callbacks.preparation_role = role;
	label->cp                       = nullptr;
	label->callbacks.publication    = std::make_shared<LabelPublicationState>();
	label->callbacks.write_bytes = dst_gpu_addr == nullptr ? 0 : (write_bytes == 0 ? sizeof(*dst_gpu_addr) : write_bytes);

	for (int i = 0; i < LABEL_ARGS_MAX; i++)
	{
		label->callbacks.args[i] = args[i];
	}

	label->event = AcquireEventLocked(ctx->device);

	RegisterLabelLocked(label);

	return label;
}

bool LabelManager::Remove(Label* label, bool track_destroy)
{
	EXIT_IF(label == nullptr);

	Core::LockGuard lock(m_mutex);
	const auto result = RemoveRegisteredLabel(&m_labels, label, m_session_resetting);
	EXIT_NOT_IMPLEMENTED(result == LabelRemoval::Missing || result == LabelRemoval::Invalid);
	if (TraceEnabled()) ++m_trace.remove_attempts;
	if (result == LabelRemoval::ResetDeferred)
	{
		// During a session reset CommandBuffer retirement releases only its
		// wrapper. FinishSessionReset keeps ownership of the registered label and
		// destroys it exactly once after all retired callbacks have completed.
		return false;
	}

	if (result == LabelRemoval::ActiveDeferred)
	{
		if (TraceEnabled())
		{
			--m_trace.states[Active];
			++m_trace.states[ActiveDeleted];
			++m_trace.deferred;
			TraceLocked("delete_deferred");
		}
		return false;
	}
	if (result != LabelRemoval::Removed) return false;
	if (track_destroy)
	{
		m_worker_batches++;
	}
	if (TraceEnabled())
	{
		--m_trace.states[NotActive];
		++m_trace.removed;
		TraceLocked("remove");
	}

	return true;
}

void LabelManager::Destroy(Label* label, LabelBatchWait* batch_wait)
{
	EXIT_IF(label == nullptr);
	EXIT_IF(label->event == nullptr);
	EXIT_IF(label->device == nullptr);
	EXIT_IF(label->cp == nullptr);
	EXIT_IF(batch_wait == nullptr);
	if (m_trace_enabled)
	{
		Core::LockGuard lock(m_mutex);
		if (TraceEnabled())
		{
			++m_trace.destroy_started;
			TraceLocked("destroy_start");
		}
	}

	// Registry detachment prevents any later Set. Complete the last accepted
	// submission that used this event: VK_EVENT_SET alone does not prove that
	// vkCmdSetEvent has stopped accessing it. Keep the full wait for an ambiguous
	// enrollment or unqualified native timeline, including legacy backends.
	LabelPublicationSnapshot identity;
	if (label->callbacks.publication != nullptr) identity = SnapshotLabelPublication(*label->callbacks.publication);
	if (identity.cp != label->cp) identity.identity_ambiguous = true;
	auto wait_result = batch_wait->WaitForPrefix(identity, [&](const LabelPublicationSnapshot& prefix)
	{
		if (m_trace_enabled)
		{
			Core::LockGuard lock(m_mutex);
			if (TraceEnabled()) ++m_trace.wait_calls;
		}
		return GraphicsRunCommandProcessorWaitForPrefix(prefix.cp, prefix.queue, prefix.session_epoch, prefix.submit_serial);
	});
	if (wait_result == LabelBatchWaitResult::Invalid)
	{
		wait_result = batch_wait->WaitFor(label->cp, [&](CommandProcessor* cp)
		{
			if (m_trace_enabled)
			{
				Core::LockGuard lock(m_mutex);
				if (TraceEnabled()) ++m_trace.wait_calls;
			}
			GraphicsRunCommandProcessorWait(cp);
			return true; // The ordinary wait fails closed on a Vulkan fence error.
		});
	}
	EXIT_IF(wait_result == LabelBatchWaitResult::Invalid);
	if (wait_result == LabelBatchWaitResult::Reused && m_trace_enabled)
	{
		Core::LockGuard lock(m_mutex);
		if (TraceEnabled()) ++m_trace.wait_avoided;
	}

	const VkDevice device = label->device;
	const VkEvent  event  = label->event;
	label->event           = nullptr;
	RecycleOrDestroyEvent(device, event);

	delete label;
}

void LabelManager::Delete(Label* label)
{
	if (Remove(label, true))
	{
		// Never destroy inline from a command-buffer retirement callback.  Destroy
		// waits the owning command processor, whose fence drain can delete more
		// labels; queuing here keeps physical VkEvent destruction at worker depth 1.
		QueueDestroy(label);
	}
}

static bool OrderedLabelRegistryContractTest()
{
	std::array<Label, 4> labels;
	for (auto& label: labels)
	{
		label.device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(11));
		label.event = reinterpret_cast<VkEvent>(static_cast<uintptr_t>(21));
		label.status = LabelStatus::NotActive;
	}
	OrderedLabelRegistry registry;
	for (auto& label: labels) registry.Add(&label);
	const auto ordered = [&](std::initializer_list<Label*> expected)
	{
		return registry.Size() == expected.size() && std::equal(registry.begin(), registry.end(), expected.begin());
	};
	// Membership rejection must not dereference even an entirely unreadable key.
	auto* foreign = reinterpret_cast<Label*>(static_cast<uintptr_t>(1));
	if (registry.Find(foreign) != nullptr || registry.Remove(foreign) ||
	    RemoveRegisteredLabel(&registry, foreign, false) != LabelRemoval::Missing) return false;
	if (RemoveRegisteredLabel(&registry, &labels[1], false) != LabelRemoval::Removed ||
	    registry.Find(&labels[1]) != nullptr || registry.Remove(&labels[1]) ||
	    !ordered({&labels[0], &labels[2], &labels[3]})) return false;
	labels[0].status = LabelStatus::Active;
	if (RemoveRegisteredLabel(&registry, &labels[0], false) != LabelRemoval::ActiveDeferred ||
	    labels[0].status != LabelStatus::ActiveDeleted || registry.Find(&labels[0]) != &labels[0] ||
	    RemoveRegisteredLabel(&registry, &labels[0], false) != LabelRemoval::Invalid) return false;
	// Event-ready processing owns the transition before unlinking ActiveDeleted.
	labels[0].status = LabelStatus::NotActive;
	if (RemoveRegisteredLabel(&registry, &labels[0], false) != LabelRemoval::Removed ||
	    RemoveRegisteredLabel(&registry, &labels[3], false) != LabelRemoval::Removed || !ordered({&labels[2]})) return false;
	registry.Add(&labels[1]);
	if (!ordered({&labels[2], &labels[1]}) ||
	    RemoveRegisteredLabel(&registry, &labels[2], true) != LabelRemoval::ResetDeferred ||
	    !ordered({&labels[2], &labels[1]})) return false;
	labels[1].status = LabelStatus::New;
	if (RemoveRegisteredLabel(&registry, &labels[1], false) != LabelRemoval::Invalid) return false;
	OrderedLabelRegistry::List detached;
	registry.TakeAll(&detached);
	if (registry.Size() != 0 || detached.size() != 2 || detached.front() != &labels[2] || detached.back() != &labels[1] ||
	    registry.Find(&labels[2]) != nullptr || registry.Find(&labels[1]) != nullptr) return false;
	detached.clear();
	registry.Add(&labels[2]);
	return ordered({&labels[2]}) && registry.Remove(&labels[2]) && registry.Size() == 0;
}

static bool LabelActiveFrontierContractTest()
{
	// Model VkEvent results, exercising actual membership, activation,
	// snapshots and stable compaction over a large inactive registry.
	constexpr size_t inactive_count = 4096;
	std::vector<Label> labels(inactive_count + 4);
	OrderedLabelRegistry registry;
	uint32_t destination = 0xa5a5a5a5;
	uint32_t callbacks = 0;
	for (auto& label: labels)
	{
		label.device = reinterpret_cast<VkDevice>(uintptr_t {11});
		label.event = reinterpret_cast<VkEvent>(uintptr_t {21});
		label.callbacks.dst_gpu_addr32 = &destination;
		label.callbacks.value32 = 7;
		label.callbacks.args[0] = reinterpret_cast<uint64_t>(&callbacks);
		label.callbacks.callback_1 = +[](const uint64_t* args, void*, uint64_t) {
			++*reinterpret_cast<uint32_t*>(args[0]);
			return true;
		};
		registry.Add(&label);
	}
	Label* const foreign = reinterpret_cast<Label*>(uintptr_t {1});
	if (registry.Activate(foreign) || registry.Activate(&labels[0]) || registry.ActiveSize() != 0) return false;
	for (size_t i = labels.size(); i != inactive_count; --i)
	{
		labels[i - 1].status = LabelStatus::Active;
		if (!registry.Activate(&labels[i - 1]) || registry.Activate(&labels[i - 1])) return false;
	}
	std::vector<Label*> visited;
	visited.reserve(4);
	registry.ScanActive([&](Label* label) { visited.push_back(label); return true; });
	if (visited != std::vector<Label*> {&labels[inactive_count], &labels[inactive_count + 1],
	                                  &labels[inactive_count + 2], &labels[inactive_count + 3]}) return false;
	if (RemoveRegisteredLabel(&registry, &labels[inactive_count + 1], false) != LabelRemoval::ActiveDeferred ||
	    registry.ActiveSize() != 4 || labels[inactive_count + 1].status != LabelStatus::ActiveDeleted) return false;
	std::vector<Label*> fired;
	fired.reserve(4);
	uint64_t queries = 0;
	bool snapshots_valid = true;
	registry.ScanActive([&](Label* label) {
		++queries;
		LabelCallbacks snapshot;
		// Errors and RESET never authorize completion, including active-deleted.
		if (SnapshotCompletedLabel(label, VK_ERROR_DEVICE_LOST, &snapshot) ||
		    SnapshotCompletedLabel(label, VK_EVENT_RESET, &snapshot)) snapshots_valid = false;
		if (label == &labels[inactive_count + 2]) return true;
		if (!SnapshotCompletedLabel(label, VK_EVENT_SET, &snapshot)) { snapshots_valid = false; return true; }
		fired.push_back(label);
		return false;
	});
	if (!snapshots_valid || queries != 4 || registry.ActiveSize() != 1 ||
	    fired != std::vector<Label*> {&labels[inactive_count], &labels[inactive_count + 1], &labels[inactive_count + 3]} ||
	    destination != 0xa5a5a5a5 || callbacks != 0 ||
	    RemoveRegisteredLabel(&registry, &labels[inactive_count + 1], false) != LabelRemoval::Removed ||
	    registry.Find(&labels[inactive_count + 1]) != nullptr || registry.Activate(&labels[inactive_count + 1])) return false;
	// Re-enrollment preserves original registration order. Removal/re-add gets
	// a fresh ordinal; an old key cannot inherit its prior ordering proof.
	labels[inactive_count].status = LabelStatus::Active;
	if (!registry.Activate(&labels[inactive_count])) return false;
	registry.Add(&labels[inactive_count + 1]);
	labels[inactive_count + 1].status = LabelStatus::Active;
	if (!registry.Activate(&labels[inactive_count + 1])) return false;
	visited.clear();
	registry.ScanActive([&](Label* label) { visited.push_back(label); return true; });
	if (visited != std::vector<Label*> {&labels[inactive_count], &labels[inactive_count + 2], &labels[inactive_count + 1]})
		return false;
	// Rollback completes through the whole registry and prunes without replay.
	LabelCallbacks snapshot;
	if (!SnapshotCompletedLabel(&labels[inactive_count], VK_EVENT_SET, &snapshot)) return false;
	registry.PruneCompletedActive();
	if (registry.ActiveSize() != 2) return false;
	if (!registry.Remove(&labels[inactive_count + 2]) || registry.ActiveSize() != 1) return false;
	OrderedLabelRegistry::List detached;
	registry.TakeAll(&detached);
	if (registry.Size() != 0 || registry.ActiveSize() != 0 || registry.Activate(&labels[inactive_count + 1])) return false;
	queries = 0;
	registry.ScanActive([&](Label*) { ++queries; return true; });
	if (queries != 0 || callbacks != 0 || destination != 0xa5a5a5a5) return false;
	std::fprintf(stderr, "[LABEL_ACTIVE_FRONTIER_TEST] inactive=%zu active=4 visits=4 whole-registry-visits=%zu"
	    " reverse-activation/reenrollment-order=pass duplicate/foreign/inactive=reject"
	    " event-reset/error=retained active-deleted=retained-until-set stable-compaction=pass"
	    " removal/readd/reset=pass rollback-prune=pass guest-effects/replay=0 native-event=modeled\n",
	    inactive_count, labels.size());
	return true;
}

static bool LabelWorkerBatchContractTest()
{
	struct Probe
	{
		Core::Mutex mutex;
		Vector<Label*> pending;
		std::atomic<uint32_t> outstanding {3}; // One callback batch and two removals.
		std::atomic<uint32_t> callbacks {0};
		std::atomic<bool> destroy_entered {false};
		std::atomic<bool> release_destroy {false};
		std::array<uint32_t, 4> callback_order {};
		Vector<Label*> destroy_order;
		bool valid = true;
	} probe;
	Label label_a, label_b, label_c;
	LabelTestAllocation owned_destination;
	if (owned_destination.address == 0) return false;
	std::memset(reinterpret_cast<void*>(owned_destination.address), 0xa5, 65536);
	auto& destination = *reinterpret_cast<uint64_t*>(owned_destination.address);
	destination = 0;
	std::atomic<uint64_t> publications_pending {0};
	std::array<std::weak_ptr<LabelPublicationState>, 2> publications;
	LabelWorkerScratch scratch;
	auto& fired = scratch.fired;
	auto& deleted = scratch.deleted;
	for (uint64_t index = 0; index != 2; ++index)
	{
		LabelCallbacks callbacks;
		callbacks.args[0] = reinterpret_cast<uint64_t>(&probe);
		callbacks.args[1] = index;
		callbacks.args[2] = reinterpret_cast<uint64_t>(&label_c);
		callbacks.dst_gpu_addr64 = &destination;
		callbacks.value64 = index + 1;
		callbacks.write_bytes = sizeof(destination);
		if (!CaptureLabelDestination(&callbacks)) return false;
		callbacks.callback_1 = +[](const uint64_t* args, void* /*output*/, uint64_t /*output_size*/)
		{
			auto& state = *reinterpret_cast<Probe*>(args[0]);
			const auto index = state.callbacks.fetch_add(1);
			state.callback_order[index] = static_cast<uint32_t>(args[1]);
			if (args[1] == 0)
			{
				// Reenter the producer side during callback delivery. The current
				// destroy snapshot must remain unchanged, with no recursive wait.
				Core::LockGuard lock(state.mutex);
				state.pending.Add(reinterpret_cast<Label*>(args[2]));
				++state.outstanding;
			}
			return args[1] == 0; // A false pre-callback suppresses only its write.
		};
		callbacks.callback_2 = +[](const uint64_t* args, void* /*output*/, uint64_t /*output_size*/)
		{
			auto& state = *reinterpret_cast<Probe*>(args[0]);
			const auto index = state.callbacks.fetch_add(1);
			state.callback_order[index] = static_cast<uint32_t>(args[1]) + 10;
			return true;
		};
		callbacks.publication = std::make_shared<LabelPublicationState>();
		callbacks.publication->aggregate = &publications_pending;
		if (!QueueLabelPublicationDelivery(&callbacks, reinterpret_cast<CommandProcessor*>(uintptr_t {0x1234}),
		        GraphicContext::QUEUE_GFX, 5, 9)) return false;
		publications[index] = callbacks.publication;
		fired.Add(callbacks);
	}
	deleted.Add(&label_a);
	deleted.Add(&label_b);
	scratch.ready_deleted.Add(&label_a);
	scratch.FinishScan();
	if (!scratch.ready_deleted.IsEmpty() || publications_pending != 2 || destination != 0 || probe.callbacks != 0)
		return false;
	const auto callback_capacity = fired.Capacity();
	const auto ready_capacity = scratch.ready_deleted.Capacity();
	const auto await = [](const std::atomic<bool>& flag)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!flag.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
		return flag.load();
	};
	const auto complete = [&](bool callbacks, uint32_t count)
	{
		probe.valid = probe.valid && probe.outstanding != 0 && (callbacks ? count == 2 : count == 1);
		--probe.outstanding;
	};
	std::thread worker([&]
	{
		RunLabelWorkerBatch(fired, deleted, DeliverLabelWorkerCallbacks,
		    [&](Label* label)
		    {
			    if (label == &label_a)
			    {
				    probe.destroy_entered.store(true);
				    probe.valid = await(probe.release_destroy) && probe.valid;
			    }
			    probe.destroy_order.Add(label);
		    }, complete);
	});
	const bool entered = await(probe.destroy_entered);
	bool callbacks_before_destroy = false;
	if (entered)
	{
		Core::LockGuard lock(probe.mutex);
		callbacks_before_destroy = probe.callbacks == 4 && destination == 1 &&
		    probe.callback_order == std::array<uint32_t, 4>{0, 10, 1, 11} && probe.pending.Size() == 1 &&
		    probe.pending.At(0) == &label_c && probe.outstanding == 3;
	}
	probe.release_destroy.store(true);
	worker.join();
	if (!entered || !callbacks_before_destroy || !probe.valid || probe.outstanding != 1 ||
	    publications_pending != 0 || probe.destroy_order.Size() != 2 ||
	    probe.destroy_order.At(0) != &label_a || probe.destroy_order.At(1) != &label_b) return false;
	// The old loop retained these payloads through its poll sleep, then released
	// them before reacquiring the manager mutex. The reusable scratch must do
	// the same: its two rows are now the only publication owners.
	for (const auto& publication: publications)
	{
		if (publication.use_count() != 1) return false;
		const auto state = publication.lock();
		const auto completed = SnapshotLabelPublication(*state);
		if (completed.queued != 1 || completed.completed != 1 || completed.cancelled != 0 || completed.pending != 0)
			return false;
	}
	scratch.FinishRound();
	if (!fired.IsEmpty() || !deleted.IsEmpty() || fired.Capacity() != callback_capacity ||
	    scratch.ready_deleted.Capacity() != ready_capacity ||
	    !publications[0].expired() || !publications[1].expired()) return false;
	if (!TakeLabelDestroyBatch(&probe.pending, &deleted) || deleted.Size() != 1 || deleted.At(0) != &label_c) return false;
	RunLabelWorkerBatch(fired, deleted, DeliverLabelWorkerCallbacks,
	    [&](Label* label) { probe.destroy_order.Add(label); }, complete);
	scratch.FinishRound();
	// Empty rounds neither retain old callbacks nor replay delivery/destruction.
	for (uint32_t round = 0; round != 32; ++round)
	{
		RunLabelWorkerBatch(fired, deleted, DeliverLabelWorkerCallbacks,
		    [&](Label*) { probe.valid = false; }, [&](bool, uint32_t) { probe.valid = false; });
		scratch.FinishScan();
		scratch.FinishRound();
	}
	const auto* bytes = reinterpret_cast<const uint8_t*>(owned_destination.address);
	for (uint32_t i = sizeof(destination); i != 65536; ++i) if (bytes[i] != 0xa5) return false;
	return probe.valid && probe.outstanding == 0 && probe.pending.IsEmpty() && probe.callbacks == 4 && destination == 1 &&
	       publications_pending == 0 && fired.Capacity() == callback_capacity && scratch.ready_deleted.Capacity() == ready_capacity &&
	       probe.destroy_order.Size() == 3 && probe.destroy_order.At(2) == &label_c;
}

static bool LabelBatchWaitContractTest()
{
	auto* cp_a = reinterpret_cast<CommandProcessor*>(static_cast<uintptr_t>(31));
	auto* cp_b = reinterpret_cast<CommandProcessor*>(static_cast<uintptr_t>(32));
	for (const bool enabled: {false, true})
	{
		LabelBatchWait invalid(enabled);
		uint32_t waits = 0;
		const auto waited = [&](CommandProcessor*) { ++waits; return true; };
		if (invalid.WaitFor(nullptr, waited) != LabelBatchWaitResult::Invalid || waits != 0 ||
		    invalid.WaitFor(cp_a, [](CommandProcessor*) { return false; }) != LabelBatchWaitResult::Invalid ||
		    invalid.WaitFor(cp_a, waited) != LabelBatchWaitResult::Waited || waits != 1) return false;

		std::array<Label, 4> labels;
		labels[0].cp = cp_a;
		labels[1].cp = cp_b;
		labels[2].cp = cp_a; // Equal processors need not be adjacent in the batch.
		labels[3].cp = cp_a;
		Vector<LabelCallbacks> no_callbacks;
		Vector<Label*> empty, pending, order;
		uint32_t outstanding = 0, completed = 0, avoided = 0;
		bool valid = true;
		waits = 0;
		const auto complete = [&](bool callbacks, uint32_t count)
		{
			valid = valid && !callbacks && count == 1 && outstanding != 0;
			if (outstanding != 0) --outstanding;
			++completed;
		};
		const auto run = [&](const Vector<Label*>& batch, bool reenter)
		{
			LabelBatchWait proof(enabled);
			RunLabelWorkerBatch(no_callbacks, batch, FireLabelCallbacks,
			    [&](Label* label)
			    {
				    const auto result = proof.WaitFor(label->cp, [&](CommandProcessor* cp)
				    {
					    ++waits;
					    valid = valid && outstanding != 0 && (cp == cp_a || cp == cp_b);
					    if (reenter && waits == 1)
					    {
						    pending.Add(&labels[3]);
						    ++outstanding;
					    }
					    return true;
				    });
				    valid = valid && result != LabelBatchWaitResult::Invalid;
				    if (result == LabelBatchWaitResult::Reused) ++avoided;
				    order.Add(label);
			    }, complete);
		};
		run(empty, false);
		if (!valid || waits != 0 || completed != 0 || !order.IsEmpty()) return false;
		Vector<Label*> first;
		first.Add(&labels[0]);
		first.Add(&labels[1]);
		first.Add(&labels[2]);
		outstanding = 3;
		run(first, true);
		if (!valid || waits != (enabled ? 2u : 3u) || avoided != (enabled ? 1u : 0u) || completed != 3 ||
		    outstanding != 1 || pending.Size() != 1 || order.Size() != 3 || order.At(0) != &labels[0] ||
		    order.At(1) != &labels[1] || order.At(2) != &labels[2]) return false;
		Vector<Label*> next;
		if (!TakeLabelDestroyBatch(&pending, &next) || next.Size() != 1 || next.At(0) != &labels[3]) return false;
		run(next, false);
		if (!valid || waits != (enabled ? 3u : 4u) || completed != 4 || outstanding != 0 || !pending.IsEmpty() ||
		    order.Size() != 4 || order.At(3) != &labels[3]) return false;
	}
	for (const bool enabled: {false, true})
	{
		LabelBatchWait proof(enabled);
		LabelPublicationSnapshot identity;
		identity.cp = cp_a; identity.queue = GraphicContext::QUEUE_GFX;
		identity.session_epoch = 3; identity.submit_serial = 10; identity.queued = 1; identity.completed = 1;
		uint32_t waits = 0;
		const auto wait = [&](const LabelPublicationSnapshot&) { ++waits; return true; };
		if (proof.WaitForPrefix(identity, wait) != LabelBatchWaitResult::Waited || waits != 1) return false;
		// Same CP is insufficient: serial 10 cannot retire an event used at 12.
		identity.submit_serial = 12;
		if (proof.WaitForPrefix(identity, wait) != LabelBatchWaitResult::Waited || waits != 2) return false;
		identity.submit_serial = 11;
		if (proof.WaitForPrefix(identity, wait) != (enabled ? LabelBatchWaitResult::Reused : LabelBatchWaitResult::Waited) ||
		    waits != (enabled ? 2u : 3u)) return false;
		identity.queue = GraphicContext::QUEUE_COMPUTE_START;
		if (proof.WaitForPrefix(identity, wait) != LabelBatchWaitResult::Waited) return false;
		identity.queue = GraphicContext::QUEUE_GFX; identity.session_epoch = 4;
		if (proof.WaitForPrefix(identity, wait) != LabelBatchWaitResult::Waited) return false;
		identity.cp = cp_b;
		if (proof.WaitForPrefix(identity, wait) != LabelBatchWaitResult::Waited) return false;
		identity.cp = cp_a; identity.session_epoch = 3; identity.submit_serial = 14;
		const auto before_failed = waits;
		if (proof.WaitForPrefix(identity, [&](const LabelPublicationSnapshot&) { ++waits; return false; }) != LabelBatchWaitResult::Invalid ||
		    proof.WaitForPrefix(identity, wait) != LabelBatchWaitResult::Waited || waits != before_failed + 2) return false;
		for (unsigned bad = 0; bad != 7; ++bad)
		{
			auto changed = identity;
			switch (bad)
			{
				case 0: changed.cp = nullptr; break;
				case 1: changed.queue = -1; break;
				case 2: changed.session_epoch = 0; break;
				case 3: changed.submit_serial = 0; break;
				case 4: changed.queued = 0; break;
				case 5: changed.cancelled = 1; break;
				case 6: changed.identity_ambiguous = true; break;
			}
			const auto before = waits;
			if (proof.WaitForPrefix(changed, wait) != LabelBatchWaitResult::Invalid || waits != before) return false;
		}
		// Fallback still proves every event use in this immutable detached batch.
		if (proof.WaitFor(cp_a, [](CommandProcessor*) { return true; }) != LabelBatchWaitResult::Waited) return false;
		identity.submit_serial = 16;
		const auto before_full = waits;
		if (proof.WaitForPrefix(identity, wait) != (enabled ? LabelBatchWaitResult::Reused : LabelBatchWaitResult::Waited) ||
		    waits != before_full + (enabled ? 0u : 1u)) return false;
		// A new worker batch never inherits either native completion cache.
		LabelBatchWait next(enabled);
		if (next.WaitForPrefix(identity, wait) != LabelBatchWaitResult::Waited) return false;
	}
	std::fprintf(stderr, "[GFX_LABEL_PREFIX_WAIT_TEST] serial10-then12=wait smaller-cutoff=reuse cp/queue/epoch=separate"
	    " ambiguous/cancelled/missing=fallback failed-wait=uncached full-wait-dominates=pass next-batch=fresh callbacks-order=unchanged\n");
	return true;
}

bool LabelDestroyQueueContractTest()
{
	if (!OrderedLabelRegistryContractTest() || !LabelActiveFrontierContractTest() ||
	    !LabelWorkerBatchContractTest() || !LabelBatchWaitContractTest()) return false;
	auto* label_a = reinterpret_cast<Label*>(static_cast<uintptr_t>(1));
	auto* label_b = reinterpret_cast<Label*>(static_cast<uintptr_t>(2));
	auto* label_c = reinterpret_cast<Label*>(static_cast<uintptr_t>(3));

	Vector<Label*> pending;
	pending.Add(label_a);
	Vector<Label*> order;
	uint32_t       outstanding = 1;
	uint32_t       depth       = 0;
	uint32_t       max_depth   = 0;
	for (uint32_t round = 0; round < 3; round++)
	{
		Vector<Label*> batch;
		if (!TakeLabelDestroyBatch(&pending, &batch))
		{
			break;
		}
		for (auto* label: batch)
		{
			depth++;
			max_depth = std::max(max_depth, depth);
			if (label == label_a)
			{
				// This models retirement callbacks reached by Destroy(A)'s CP wait.
				// They publish ownership to the next worker snapshot, never recurse.
				pending.Add(label_b);
				pending.Add(label_c);
				outstanding += 2;
			}
			order.Add(label);
			depth--;
			outstanding--;
		}
	}

	Vector<Label*> reset_pending;
	reset_pending.Add(label_a);
	Vector<Label*> reset_batch;
	const bool reset_drained = TakeLabelDestroyBatch(&reset_pending, &reset_batch);

	auto device_a = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(11));
	auto device_b = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(12));
	auto event_a  = reinterpret_cast<VkEvent>(static_cast<uintptr_t>(21));
	auto event_b  = reinterpret_cast<VkEvent>(static_cast<uintptr_t>(22));
	auto event_c  = reinterpret_cast<VkEvent>(static_cast<uintptr_t>(23));
	RecycledLabelEventPool recycled;
	if (!recycled.Recycle(device_a, event_a) || !recycled.Recycle(device_b, event_b) ||
	    !recycled.Recycle(device_a, event_c) || recycled.Recycle(device_a, event_a)) return false;
	VkEvent taken_a = nullptr;
	VkEvent taken_b = nullptr;
	const bool took_a = TakeRecycledLabelEvent(&recycled, device_a, &taken_a);
	const bool took_b = TakeRecycledLabelEvent(&recycled, device_b, &taken_b);
	VkEvent remaining = nullptr;
	const bool retained_one = recycled.Size() == 1 && recycled.KnownSize() == 3 &&
	    TakeRecycledLabelEvent(&recycled, device_a, &remaining) && remaining == event_a && recycled.Size() == 0;
	return max_depth == 1 && depth == 0 && outstanding == 0 && pending.IsEmpty() &&
	       order.Size() == 3 && order.At(0) == label_a && order.At(1) == label_b &&
	       order.At(2) == label_c && reset_drained && reset_pending.IsEmpty() &&
	       reset_batch.Size() == 1 && reset_batch.At(0) == label_a && took_a && took_b &&
	       taken_a == event_c && taken_b == event_b && retained_one &&
	       !ExactOneEnvironmentValue(nullptr) && !ExactOneEnvironmentValue("") &&
	       !ExactOneEnvironmentValue("0") && ExactOneEnvironmentValue("1") &&
	       !ExactOneEnvironmentValue("01") && !ExactOneEnvironmentValue("true");
}

void LabelManager::Set(CommandBuffer* buffer, Label* label, const LabelScalarCaptureTicket* scalar_capture,
                        const LabelDeferredCaptureTicket* deferred_capture)
{
	EXIT_IF(label == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(m_mutex);
	EXIT_IF(m_session_resetting);

	label = m_labels.Find(label);
	EXIT_NOT_IMPLEMENTED(label == nullptr);
	EXIT_IF(label->event == nullptr);
	EXIT_IF(label->device == nullptr);

	EXIT_NOT_IMPLEMENTED(label->status != LabelStatus::New && label->status != LabelStatus::NotActive);

	const char* capture_rejection = "unknown";
	GraphicsGuestLeaseResult capture_result;
	LabelCaptureVmDenials capture_vm_denials;
	EXIT_NOT_IMPLEMENTED(scalar_capture != nullptr && deferred_capture != nullptr);
	if (deferred_capture != nullptr)
	{
		EXIT_NOT_IMPLEMENTED(!InstallLabelDeferredCapture(&label->callbacks, *deferred_capture,
		    GraphicsRunCommandProcessorDeferredLabelIdentity(buffer->GetParent(), buffer)));
	} else if (scalar_capture != nullptr)
	{
		EXIT_NOT_IMPLEMENTED(!InstallLabelScalarCapture(&label->callbacks, *scalar_capture,
		    GraphicsRunCommandProcessorScalarLabelIdentity(buffer->GetParent(), buffer)));
	} else if (!CaptureLabelDestination(&label->callbacks, &capture_rejection, &capture_result,
	    LabelCaptureVmAcquire {&capture_vm_denials}))
	{
		ReportLabelCaptureVmDenials(label->callbacks, buffer, capture_vm_denials);
		ReportLabelPublicationFailure(label->callbacks, {"capture", capture_rejection,
		    capture_result.observed_generation, capture_result.attempts});
		EXIT("Label deferred destination rejected at capture\n");
	}
	if (scalar_capture == nullptr) label->callbacks.scalar_capture = {};
	if (deferred_capture == nullptr) label->callbacks.deferred_capture = {};

	if (TraceEnabled())
	{
		--m_trace.states[label->status];
		++m_trace.states[Active];
		++m_trace.sets;
		TraceLocked("set");
	}
	label->status = LabelStatus::Active;

	EXIT_IF(label->event == nullptr);

	label->cp = buffer->GetParent();
	// The host producer is carried by the validated buffer, never by args or
	// the processor later supplied to the getter. Detachment retains this copy.
	EXIT_IF(!EnrollPublicationLocked(label, label->cp, buffer->GetQueue(),
	                                      buffer->GetSubmissionSessionEpoch(), buffer->GetRecordingSubmitSerial()));

	auto* vk_buffer = buffer->GetBufferOutsideRenderPass();

	EXIT_NOT_IMPLEMENTED(vk_buffer == nullptr);

	EXIT_NOT_IMPLEMENTED(vkResetEvent(label->device, label->event) != VK_SUCCESS);
	vkCmdSetEvent(vk_buffer, label->event, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	EXIT_IF(!m_labels.Activate(label));

	m_cond_var.Signal();
}

void LabelInit()
{
	EXIT_IF(g_label_manager != nullptr);

	g_label_manager = new LabelManager;
}

bool LabelBeginSessionReset()
{
	return g_label_manager != nullptr && g_label_manager->BeginSessionReset();
}

bool LabelFinishSessionReset()
{
	return g_label_manager != nullptr && g_label_manager->FinishSessionReset();
}

Label* LabelCreate64(GraphicContext* ctx, uint64_t* dst_gpu_addr, uint64_t value, LabelGpuObject::callback_t callback_1,
                     LabelGpuObject::callback_t callback_2, const uint64_t* args, uint64_t write_bytes,
                     LabelGpuObject::PreparationRole preparation_role)
{
	EXIT_IF(g_label_manager == nullptr);

	return g_label_manager->Create64(ctx, dst_gpu_addr, value, callback_1, callback_2, args, write_bytes, preparation_role);
}

Label* LabelCreate32(GraphicContext* ctx, uint32_t* dst_gpu_addr, uint32_t value, LabelGpuObject::callback_t callback_1,
                     LabelGpuObject::callback_t callback_2, const uint64_t* args, uint64_t write_bytes,
                     LabelGpuObject::PreparationRole preparation_role)
{
	EXIT_IF(g_label_manager == nullptr);

	return g_label_manager->Create32(ctx, dst_gpu_addr, value, callback_1, callback_2, args, write_bytes, preparation_role);
}

void LabelDelete(Label* label)
{
	EXIT_IF(g_label_manager == nullptr);

	g_label_manager->Delete(label);
}

void LabelSet(CommandBuffer* buffer, Label* label, const LabelScalarCaptureTicket* scalar_capture,
               const LabelDeferredCaptureTicket* deferred_capture)
{
	EXIT_IF(g_label_manager == nullptr);

	g_label_manager->Set(buffer, label, scalar_capture, deferred_capture);
}

static void* create_func(GraphicContext* ctx, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                         VulkanMemory* /*mem*/)
{
	LUMEN_PROFILER_BLOCK("LabelGpuObject::Create");

	EXIT_IF(vaddr_num != 1 || params == nullptr || size == nullptr || vaddr == nullptr || *vaddr == 0);

	EXIT_NOT_IMPLEMENTED(!LabelObjectWriteShapeIsValid(*size, params[LabelGpuObject::PARAM_WRITE_BYTES]));

	auto value      = params[LabelGpuObject::PARAM_VALUE];
	auto callback_1 = reinterpret_cast<LabelGpuObject::callback_t>(params[LabelGpuObject::PARAM_CALLBACK_1]);
	auto callback_2 = reinterpret_cast<LabelGpuObject::callback_t>(params[LabelGpuObject::PARAM_CALLBACK_2]);
	LabelGpuObject::PreparationRole preparation_role;
	EXIT_NOT_IMPLEMENTED(!DecodeLabelPreparationRole(params[LabelGpuObject::PARAM_PREPARATION_ROLE], &preparation_role));

	auto* label_obj = (*size == 8 && params[LabelGpuObject::PARAM_WRITE_BYTES] == 0 ? LabelCreate64(ctx, reinterpret_cast<uint64_t*>(*vaddr), value, callback_1, callback_2,
	                                              params + LabelGpuObject::PARAM_ARG_1, params[LabelGpuObject::PARAM_WRITE_BYTES], preparation_role)
	                              : (*size == 4 || *size == params[LabelGpuObject::PARAM_WRITE_BYTES] ? LabelCreate32(ctx, reinterpret_cast<uint32_t*>(*vaddr), static_cast<uint32_t>(value),
	                                                            callback_1, callback_2, params + LabelGpuObject::PARAM_ARG_1, params[LabelGpuObject::PARAM_WRITE_BYTES], preparation_role)
	                                            : nullptr));

	EXIT_NOT_IMPLEMENTED(label_obj == nullptr);

	return label_obj;
}

static void update_func(GraphicContext* /*ctx*/, const uint64_t* /*params*/, void* /*obj*/, const uint64_t* /*vaddr*/,
                        const uint64_t* /*size*/, int /*vaddr_num*/)
{
	LUMEN_PROFILER_BLOCK("LabelGpuObject::update_func");

	LUMEN_NOT_IMPLEMENTED;
}

static void delete_func(GraphicContext* /*ctx*/, void* obj, VulkanMemory* /*mem*/)
{
	LUMEN_PROFILER_BLOCK("LabelGpuObject::delete_func");

	auto* label_obj = reinterpret_cast<Label*>(obj);

	EXIT_IF(label_obj == nullptr);

	LabelDelete(label_obj);
}

bool LabelGpuObject::Equal(const uint64_t* other) const
{
	return (params[PARAM_VALUE] == other[PARAM_VALUE] && params[PARAM_CALLBACK_1] == other[PARAM_CALLBACK_1] &&
	        params[PARAM_CALLBACK_2] == other[PARAM_CALLBACK_2] && params[PARAM_ARG_1] == other[PARAM_ARG_1] &&
	        params[PARAM_ARG_2] == other[PARAM_ARG_2] && params[PARAM_ARG_3] == other[PARAM_ARG_3] &&
	        params[PARAM_ARG_4] == other[PARAM_ARG_4] && params[PARAM_WRITE_BYTES] == other[PARAM_WRITE_BYTES] &&
	        params[PARAM_PREPARATION_ROLE] == other[PARAM_PREPARATION_ROLE]);
}

GpuObject::create_func_t LabelGpuObject::GetCreateFunc() const
{
	return create_func;
}

GpuObject::delete_func_t LabelGpuObject::GetDeleteFunc() const
{
	return delete_func;
}

GpuObject::update_func_t LabelGpuObject::GetUpdateFunc() const
{
	return update_func;
}

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED
