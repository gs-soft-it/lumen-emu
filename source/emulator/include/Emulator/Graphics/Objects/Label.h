#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_LABEL_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_LABEL_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"

#include <array>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

struct Label;
class CommandBuffer;
struct GraphicContext;
struct GraphicsGuestLeaseResult;
struct LabelWriteBackProofScope;
struct LabelPublicationSnapshot;

struct LabelScalarRecordingIdentity
{
	const void* processor = nullptr;
	const void* buffer = nullptr;
	const void* parent = nullptr;
	const void* pool = nullptr;
	int current_index = -1, processor_queue = -1, buffer_queue = -1;
	uint32_t pool_index = UINT32_MAX;
	uint64_t submit_id = 0, processor_epoch = 0, assigned_serial = 0;
	uint64_t recording = 0, session = 0, serial = 0, ordinal = 0, submitted_cutoff = 0, completed_cutoff = 0;
	bool exclusive = false, valid = false;
};

// Only an exclusive packet parser may create this pre-enrollment scalar
// capture. It retains the first VM token, payload and exact recording; it is
// not a completion witness or authority for any callback effects.
class LabelScalarCaptureTicket
{
public:
	bool IsCaptured() const { return m_address != 0; }
	uint64_t Address() const { return m_address; }
	uint64_t Size() const { return m_size; }
	uint64_t Generation() const { return m_generation; }
	uint64_t Value() const { return m_value; }
	bool MatchesRecording(const LabelScalarRecordingIdentity& recording) const;
	bool MatchesPublication(const LabelPublicationSnapshot& publication) const;

private:
	friend class CommandProcessor;
	friend bool LabelScalarCaptureContractTest();
	uint64_t m_address = 0, m_size = 0, m_generation = UINT64_MAX, m_value = 0;
	LabelScalarRecordingIdentity m_recording;
};

// Borrowed only during callback_1 of the exact detached worker delivery.
// Direct callback execution, nested delivery, foreign CP and later phases do
// not carry this capability. Validation never grants GPU completion authority.
const LabelWriteBackProofScope* LabelGetActiveWriteBackProofScope(CommandProcessor* cp);
bool LabelValidateWriteBackProofScope(const LabelWriteBackProofScope* scope, CommandProcessor* cp,
                                     GraphicsGuestLeaseResult* result = nullptr);

// Completion of callback delivery is distinct from VkEvent becoming SET.
// These counters do not prove destination lifetime or resource coherence.
struct LabelPublicationSnapshot
{
	uint64_t instance_id = 0;
	uint64_t queued = 0;
	uint64_t completed = 0;
	uint64_t cancelled = 0;
	uint64_t pending = 0;
	CommandProcessor* cp = nullptr;
	int queue = -1;
	uint64_t session_epoch = 0;
	uint64_t submit_serial = 0;
	bool identity_ambiguous = false;
};

// Host diagnostics for the currently registered delivery, captured under the
// manager lock. This does not describe every older detached delivery or grant
// callback-effect, destination-continuity or disjointness authority.
struct LabelPublicationHostMetadata
{
	uint64_t manager_identity = 0;
	uint64_t publication_identity = 0;
	uint64_t registered_sequence = 0;
	uint64_t registered_destination = 0;
	uint64_t registered_write_bytes = 0;
	uint64_t registered_destination_generation = UINT64_MAX;
	uint64_t registered_callback_1 = 0;
	uint64_t registered_callback_2 = 0;
	uint64_t registered_preparation_role = 0;
};

struct LabelPublicationCompletionTicketState;
// Retains one exact enrollment. Only a pre-effect renderer admission that has
// separately proved the native producer prefix completed may use this ticket.
// It does not complete callbacks, enroll a newer sequence or grant VM authority.
class LabelPublicationCompletionTicket
{
public:
	static constexpr uint32_t MaxRetainedBytes = 128;
private:
	friend class LabelManager;
	std::shared_ptr<LabelPublicationCompletionTicketState> m_state;
};
bool LabelCapturePublicationCompletionTicket(Label* label, const LabelPublicationSnapshot& original,
                                            LabelPublicationCompletionTicket* ticket);
// A true result proves fixed identity/enrollment and monotonic completion only.
// pending != 0 requires another existing preflight backoff before any reproof.
bool LabelValidatePublicationCompletionTicket(const LabelPublicationCompletionTicket& ticket,
                                             LabelPublicationSnapshot* current, const char** reason = nullptr);

struct LabelScalarPublicationFootprint
{
	LabelPublicationSnapshot enrollment;
	uint64_t address = 0, bytes = 0, value = 0, original_generation = UINT64_MAX;
	bool scalar64 = false;
};

struct LabelScalarPublicationCompletionTicketState;
// A fixed, first-enrollment scalar delivery with no callbacks. Capturing this
// ticket does not prove native completion or disjointness. A caller must prove
// both before its GPU effects and retain that separate authority through gaps.
class LabelScalarPublicationCompletionTicket
{
public:
	static constexpr uint32_t MaxRetainedBytes = 256;
	bool IsCaptured() const { return m_state != nullptr; }
private:
	friend class LabelManager;
	std::shared_ptr<LabelScalarPublicationCompletionTicketState> m_state;
};
bool LabelCaptureScalarPublicationCompletionTicket(Label* label, const LabelPublicationSnapshot& original,
    LabelScalarPublicationCompletionTicket* ticket, LabelScalarPublicationFootprint* footprint,
    GraphicsGuestLeaseResult* mapping_result = nullptr, const char** reason = nullptr);
// Only actual monotonic completion is observed; a still-pending delivery stays
// pending. The original scalar payload, enrollment and VM token are retained.
bool LabelValidateScalarPublicationCompletionTicket(const LabelScalarPublicationCompletionTicket& ticket,
    LabelPublicationSnapshot* current, GraphicsGuestLeaseResult* mapping_result = nullptr, const char** reason = nullptr);

struct LabelScalarPublicationCensusEntry
{
	Label* key = nullptr;
	LabelScalarPublicationFootprint footprint;
};
// Optional host-only receipt from the original capture pass. Unevaluated
// short-circuit checks stay explicitly unchecked; this grants no authority.
struct LabelScalarPublicationCensusCaptureWitness
{
	const char* reason = nullptr;
	Label* key = nullptr;
	uint64_t publication_identity = 0;
	LabelPublicationSnapshot original, delivery_enrollment;
	LabelPublicationHostMetadata registered;
	uint32_t state_count = 0, scalar_count = 0, row_index = UINT32_MAX;
	bool complete = false, discovery_complete = false, row_present = false, sealed_checked = false, sealed = false;
	bool description_checked = false, described = false;
	bool seal_checked = false, seal_equal = false;
	bool enrollment_checked = false, enrollment_equal = false;
};
static_assert(sizeof(LabelScalarPublicationCensusCaptureWitness) <= 384);
struct LabelScalarPublicationCensusBankState;
// Captured before GPU effects. Every enrolled state is retained, including
// completed states; only qualified pending scalars export an effect footprint.
// A pending scalar seals its exact latest enrollment only after every earlier
// delivery completed; the separate standalone ticket remains first-only.
// The caller must retain renderer exclusion across validation and its proof,
// and independently establish native completion and complete disjointness.
class LabelScalarPublicationCensusBank
{
public:
	static constexpr uint32_t MaxEnrolledStates = 1024;
	static constexpr uint32_t MaxVisitedStates = 65536;
	static constexpr uint64_t MaxCombinedRetainedMetadataBytes = 4ull * 1024 * 1024;
	static constexpr uint64_t SharedStateAccountingOverheadBytes = 64;
	bool IsCaptured() const { return m_state != nullptr; }
	uint32_t StateCount() const;
	uint32_t ScalarCount() const;
	// Logical peak cost includes the retained bank/publications and a complete
	// validation discovery vector/maps while that original bank remains alive.
	// Allocation capacity/bookkeeping follows the shared metadata budget policy.
	// Scalar footprints/completion progress are embedded, not separate tickets.
	uint64_t RetainedMetadataBytes() const;
	bool GetScalarEntry(uint32_t index, LabelScalarPublicationCensusEntry* entry) const;
private:
	friend class LabelManager;
	std::shared_ptr<LabelScalarPublicationCensusBankState> m_state;
};
// A caller may reserve the remaining part of its combined metadata budget.
// Zero or a budget above the common maximum rejects before discovery/copy/VM.
bool LabelCaptureScalarPublicationCensusBank(LabelScalarPublicationCensusBank* bank, const char** reason = nullptr,
    uint64_t retained_metadata_budget = LabelScalarPublicationCensusBank::MaxCombinedRetainedMetadataBytes,
    LabelScalarPublicationCensusCaptureWitness* witness = nullptr);
// This validates the original whole census. It never captures new enrollments,
// refreshes a VM token, waits for callbacks or synthesizes completion.
bool LabelValidateScalarPublicationCensusBank(const LabelScalarPublicationCensusBank& bank, const char** reason = nullptr);

struct LabelCompletedRetirementFootprint
{
	LabelPublicationSnapshot enrollment;
	uint64_t address = 0, bytes = 0, original_generation = UINT64_MAX;
};
struct LabelCompletedRetirementTicketState;
// Retains an already completed registered Label and the original whole census.
// This metadata proof never runs callbacks, removes the registry entry or proves
// native completion. The caller must separately prove its producer prefix and
// every pending scalar disjoint from the full incoming CPU snapshot range.
class LabelCompletedRetirementTicket
{
public:
	static constexpr uint32_t MaxRetainedBytes = 384;
	bool IsCaptured() const { return m_state != nullptr; }
private:
	friend class LabelManager;
	std::shared_ptr<LabelCompletedRetirementTicketState> m_state;
};
bool LabelCaptureCompletedRetirementTicket(Label* label, uint64_t expected_instance, uint64_t address, uint64_t bytes,
    const uint64_t* tracked_params, const LabelScalarPublicationCensusBank& census,
    LabelCompletedRetirementTicket* ticket, LabelCompletedRetirementFootprint* footprint,
    GraphicsGuestLeaseResult* mapping_result = nullptr, const char** reason = nullptr);
// Rechecks strict registered membership, payload, complete counters, whole
// census and continuity of the original Set token. No VM token is refreshed.
bool LabelValidateCompletedRetirementTicket(const LabelCompletedRetirementTicket& ticket,
    GraphicsGuestLeaseResult* mapping_result = nullptr, const char** reason = nullptr);
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
// The bank/key are borrowed only for this synchronous callback. Native event
// readiness is modeled; deliver performs the real detached scalar publication.
// sequence > 1 performs all earlier stores before sealing the pending delivery.
bool LabelScalarPublicationCensusInventoryContractTest(CommandProcessor* cp, int queue, uint64_t epoch, uint64_t serial,
    uint64_t address, uint64_t value, bool scalar64,
    bool (*observe)(void*, Label*, const LabelScalarPublicationCensusBank&, bool (*deliver)(Label*)), void* context,
    uint64_t sequence = 1);
bool LabelScalarPublicationCensusPairInventoryContractTest(CommandProcessor* cp, int queue, uint64_t epoch, uint64_t serial,
    const uint64_t* addresses, const uint64_t* values, bool scalar64,
    bool (*observe)(void*, Label* const*, const LabelScalarPublicationCensusBank&, bool (*deliver)(Label*)), void* context,
    uint64_t sequence = 1);
#endif

// Exact registry membership is checked before dereferencing the supplied key.
bool LabelGetPublicationSnapshot(Label* label, LabelPublicationSnapshot* snapshot,
                                 LabelPublicationHostMetadata* diagnostic = nullptr);
// Includes callbacks detached from the registry; zero alone is not a fence.
uint64_t LabelPendingPublicationCount();
// The caller has retired this exact processor prefix and holds no processor,
// tracker or VM lock. Detached callback snapshots remain part of the wait.
bool LabelWaitForPublicationPrefix(CommandProcessor* cp, int queue, uint64_t epoch, uint64_t serial,
                                   uint64_t* publications);
bool LabelPublicationContractTest();
bool LabelDestinationContractTest();

void LabelInit();
// Session reset is intentionally split around command-buffer retirement:
// quiesce prevents new polling and registry removals while the worker drains
// destruction it already owns; command buffers then retire their wrappers and
// finish owns final deletion of labels still registered with the manager.
bool LabelBeginSessionReset();
bool LabelFinishSessionReset();
// Headless contract used by the graphics-state suite.  Production destruction
// uses the same snapshot rule so a retirement callback can enqueue another
// label without recursively entering the Vulkan destroy path.
bool LabelDestroyQueueContractTest();

constexpr int LABEL_ARGS_MAX = 4;

class LabelGpuObject: public GpuObject
{
public:
	// Only host producers whose preparation calls writeback without holding any
	// subsystem lock may declare this role. Ordinary callbacks cannot infer it.
	enum class PreparationRole: uint64_t { Ordinary, LockFreeWriteBack };
	// Before publication, callbacks may wait and materialize only this private
	// host buffer. The post-publication callback receives nullptr/zero.
	using callback_t = bool (*)(const uint64_t* args, void* output, uint64_t output_size);

	// Maximum materialization is the local GDS buffer (0x3000 DWORDs).
	static constexpr uint64_t MAX_WRITE_BYTES = 0x3000 * sizeof(uint32_t);

	static constexpr int PARAM_VALUE      = 0;
	static constexpr int PARAM_CALLBACK_1 = 1;
	static constexpr int PARAM_CALLBACK_2 = 2;
	static constexpr int PARAM_ARG_1      = 3;
	static constexpr int PARAM_ARG_2      = 4;
	static constexpr int PARAM_ARG_3      = 5;
	static constexpr int PARAM_ARG_4      = 6;
	static constexpr int PARAM_WRITE_BYTES = 7;
	static constexpr int PARAM_PREPARATION_ROLE = 8;
	static_assert(PARAM_PREPARATION_ROLE < GpuObject::PARAMS_MAX);

	explicit LabelGpuObject(uint64_t value, callback_t callback_1, callback_t callback_2, const uint64_t* args = nullptr,
	                        uint64_t write_bytes = 0, PreparationRole preparation_role = PreparationRole::Ordinary)
	{
		params[PARAM_VALUE]      = value;
		params[PARAM_WRITE_BYTES] = write_bytes;
		params[PARAM_PREPARATION_ROLE] = static_cast<uint64_t>(preparation_role);
		params[PARAM_CALLBACK_1] = reinterpret_cast<uint64_t>(callback_1);
		params[PARAM_CALLBACK_2] = reinterpret_cast<uint64_t>(callback_2);
		if (args != nullptr)
		{
			params[PARAM_ARG_1] = args[0];
			params[PARAM_ARG_2] = args[1];
			params[PARAM_ARG_3] = args[2];
			params[PARAM_ARG_4] = args[3];
		}
		check_hash = false;
		type       = Graphics::GpuMemoryObjectType::Label;
	}

	bool Equal(const uint64_t* other) const override;

	[[nodiscard]] create_func_t              GetCreateFunc() const override;
	[[nodiscard]] create_from_objects_func_t GetCreateFromObjectsFunc() const override { return nullptr; };
	[[nodiscard]] write_back_func_t          GetWriteBackFunc() const override { return nullptr; };
	[[nodiscard]] delete_func_t              GetDeleteFunc() const override;
	[[nodiscard]] update_func_t              GetUpdateFunc() const override;
};

// Initial natural-width destination capture only. Callback pointers and arguments
// are opaque immutable payload, never permission to execute them during retry.
// Callers invoke this before renderer/tracker/Label enrollment; GDS and
// destination-free notification wrappers retain their separate routes.
class LabelDeferredCaptureTicket
{
public:
	bool IsCaptured() const { return m_address != 0; }
	uint64_t Address() const { return m_address; }
	uint64_t Size() const { return m_size; }
	uint64_t Generation() const { return m_generation; }
	const auto& Params() const { return m_params; }
	const LabelScalarRecordingIdentity& Recording() const { return m_recording; }
	bool MatchesRecording(const LabelScalarRecordingIdentity& current) const;

private:
	friend bool LabelCaptureDeferredDestination(CommandBuffer*, uint64_t, uint64_t,
	    const LabelGpuObject&, LabelDeferredCaptureTicket*, const char**);
	friend bool LabelDeferredCaptureContractTest();
	uint64_t m_address = 0, m_size = 0, m_generation = UINT64_MAX;
	std::array<uint64_t, LabelGpuObject::PARAM_PREPARATION_ROLE + 1> m_params {};
	LabelScalarRecordingIdentity m_recording;
};

// A parser in a DCB/CCB batch keeps CP locked during every initial pause;
// an exclusive parser may release CP with RunLock retained. An unqualified
// generic caller keeps legacy Set capture. A qualified failure never falls back.
bool LabelCaptureDeferredDestination(CommandBuffer* buffer, uint64_t address, uint64_t natural_bytes,
    const LabelGpuObject& description, LabelDeferredCaptureTicket* ticket, const char** reason = nullptr);
bool LabelDeferredCaptureContractTest();

// Headless contract exercising the same completion snapshot and callback order
// as the worker. The observer sees both pending and completed states.
bool LabelCompletionContractTest(const LabelGpuObject& description, uint32_t* destination,
                                 bool (*observe)(void*, bool), void* state);
bool LabelCompletionContractTest64(const LabelGpuObject& description, uint64_t* destination,
                                   bool (*observe)(void*, bool), void* state);
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
// A zero address allocates an owned managed VM page and selects its interior.
// Earlier enrollments and the final store/callback actually complete before
// observe; native event readiness and the callback's GPU preparation are modeled.
// A nonzero pending_offset additionally enrolls an actual callback-free scalar
// at target+offset without publishing it; the fixture cancels it after observe.
bool LabelCompletedRetirementInventoryContractTest(CommandProcessor* cp, int queue, uint64_t epoch, uint64_t serial,
    uint64_t address, uint64_t value, bool scalar64,
    bool (*observe)(void*, Label*, const LabelScalarPublicationCensusBank&, const LabelGpuObject&), void* context,
    uint64_t sequence = 1, uint64_t pending_offset = 0);
// Registers one real headless pending Label for the observer. Completion uses
// the production counters; no worker, Vulkan event or destination store runs.
bool LabelPendingInventoryContractTest(CommandProcessor* cp, int queue, uint64_t epoch, uint64_t serial,
    bool (*observe)(void*, Label*, bool (*complete)(Label*)), void* state);
#endif

Label* LabelCreate64(GraphicContext* ctx, uint64_t* dst_gpu_addr, uint64_t value, LabelGpuObject::callback_t callback_1,
                     LabelGpuObject::callback_t callback_2, const uint64_t* args, uint64_t write_bytes = 0,
                     LabelGpuObject::PreparationRole preparation_role = LabelGpuObject::PreparationRole::Ordinary);
Label* LabelCreate32(GraphicContext* ctx, uint32_t* dst_gpu_addr, uint32_t value, LabelGpuObject::callback_t callback_1,
                     LabelGpuObject::callback_t callback_2, const uint64_t* args, uint64_t write_bytes = 0,
                     LabelGpuObject::PreparationRole preparation_role = LabelGpuObject::PreparationRole::Ordinary);
void   LabelDelete(Label* label);
void   LabelSet(CommandBuffer* buffer, Label* label, const LabelScalarCaptureTicket* scalar_capture = nullptr,
                const LabelDeferredCaptureTicket* deferred_capture = nullptr);

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_OBJECTS_LABEL_H_ */
