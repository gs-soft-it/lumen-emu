#ifndef EMULATOR_INCLUDE_CPUX64_H_
#define EMULATOR_INCLUDE_CPUX64_H_

#include <cstddef>
#include <cstdint>

namespace Lumen::Cpu {

struct X64CallStackTransaction
{
	uint64_t original_rsp        = 0;
	uint64_t slot_address        = 0;
	uint64_t slot_host_address   = 0;
	uint64_t original_slot_value = 0;
	bool     active              = false;
};

struct X64State
{
	struct HostCallOnly {};

	X64State() noexcept
	    : host_call_rip {}, host_call_target {}, host_call_result {},
	      host_call_arg0 {}, host_call_arg1 {}, host_call_arg2 {}
	{}

	// FEX creates a fresh bridge state for every HLE call. It neither records
	// execution history nor passes this state to the interpreter or a state
	// snapshot. Leave only the six unused history arrays uninitialized; all
	// other members retain their ordinary defaults, including instruction_trace
	// (also read by host-wait tracing) and the inactive stack transaction.
	// Do not copy this whole state or enable diagnostic_history_enabled.
	explicit X64State(HostCallOnly) noexcept {}

	uint64_t gpr[16] = {};
	// Keep the historical xmm name, but retain all four qwords so AVX YMM
	// instructions can share the same low 128-bit state with SSE code.
	uint64_t xmm[16][4] = {};
	uint64_t rip       = 0;
	uint64_t rflags   = 0x202;
	uint64_t fs_base   = 0;
	uint64_t gs_base   = 0;
	uint32_t mxcsr     = 0x1f80;
	uint16_t x87_control_word = 0x037f;
	long double x87[8] = {};
	uint8_t x87_depth = 0;
	uint64_t instruction_trace[32] = {};
	uint32_t instruction_trace_index = 0;
	uint64_t memory_fault_address = 0;
	uint32_t memory_fault_size    = 0;
	bool     memory_fault_write   = false;
	uint64_t last_data_read_rip     = 0;
	uint64_t last_data_read_address = 0;
	uint64_t last_data_read_value   = 0;
	uint32_t last_data_read_size    = 0;
	uint64_t data_read_rip[16]     = {};
	uint64_t data_read_address[16] = {};
	uint64_t data_read_value[16]   = {};
	uint32_t data_read_size[16]    = {};
	uint32_t data_read_index       = 0;
	uint64_t host_call_rip[256];
	uint64_t host_call_target[256];
	uint64_t host_call_result[256];
	uint64_t host_call_arg0[256];
	uint64_t host_call_arg1[256];
	uint64_t host_call_arg2[256];
	uint32_t host_call_index       = 0;
	uint64_t x87_instruction_rip[16] = {};
	uint64_t x87_instruction_count = 0;
	// Expensive execution history used only by opt-in diagnostics.
	bool diagnostic_history_enabled = false;
	// A host-handled CALL is tentative until its HLE argument preflight has
	// completed. This snapshot permits a fault to restore both RSP and the
	// qword overwritten by the CALL without requiring guest-read permission.
	X64CallStackTransaction host_call_stack_transaction {};
	// A cooperative host-call cancellation returns through the normal
	// interpreter epilogue, preserving stack-transaction and run-context cleanup.
	bool execution_cancel_requested = false;
};

enum class X64RunResult
{
	Returned,
	StepLimit,
	UnsupportedInstruction,
	MemoryFault,
	InternalError,
};

#if defined(LUMEN_CPU_X64_TEST) && \
    defined(LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2) && \
    LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2
struct X64RegionEngineV2StatsForTest
{
	uint64_t lookups = 0;
	uint64_t hits = 0;
	uint64_t builds = 0;
	uint64_t executions = 0;
	uint64_t retired_operations = 0;
	uint64_t commits = 0;
	uint64_t path_side_exits = 0;
	uint64_t pre_signature_misses = 0;
	uint64_t segment_signature_misses = 0;
	uint64_t post_signature_rollbacks = 0;
	uint64_t lease_rejections = 0;
	uint64_t lease_identity_misses = 0;
	uint64_t barriers = 0;
};

void X64ResetRegionEngineV2ForTest();
X64RegionEngineV2StatsForTest X64GetRegionEngineV2StatsForTest();
bool X64RegionEngineV2EnvironmentValueEnabledForTest(const char* value);
using X64RegionEngineV2PostValidationHookForTest = void (*)();
void X64SetRegionEngineV2PostValidationHookForTest(
	X64RegionEngineV2PostValidationHookForTest hook);
using X64RegionEngineV2SegmentValidationHookForTest = void (*)();
void X64SetRegionEngineV2SegmentValidationHookForTest(
	X64RegionEngineV2SegmentValidationHookForTest hook);
void X64SetRegionEngineV2UnmanagedCodeLeaseBypassForTest(bool enabled);
bool X64ValidateRegionEngineV2ScalarParityForTest();
#endif

struct X64MemoryWriteInfo
{
	uint64_t rip     = 0;
	uint64_t address = 0;
	uint64_t value   = 0;
	uint32_t size    = 0;
};

enum class X64MemoryPermission : uint32_t
{
	Read  = 1u << 0u,
	Write = 1u << 1u,
};

struct X64MemoryTranslationRegion
{
	uint64_t guest_begin = 0;
	uint64_t guest_end   = 0;
	uint64_t host_begin  = 0;
	uint32_t permissions = 0;
};

// Address translation is conservatively callback-capable unless its owner
// supplies the explicit production contract. The capability is intentionally
// independent from callback identity: equal function pointers do not imply an
// equal reentry contract.
enum class X64AddressTranslatorReentryCapability : uint8_t
{
	MayReenter = 0,
	NonReentrant = 1,
};

struct X64AddressTranslatorCapabilities
{
	X64AddressTranslatorReentryCapability reentry =
	    X64AddressTranslatorReentryCapability::MayReenter;
};

// RegionQuery returns the mapped interval containing or following address.
// X64ValidateAddressRange accepts an access only when every byte belongs to
// contiguous regions that permit the requested access type.
using X64AddressRegionQuery = bool (*)(uint64_t address, bool write, uint64_t* start, uint64_t* end);
bool X64ValidateAddressRange(uint64_t address, size_t size, bool write, X64AddressRegionQuery query);

// Read through the translator of the active X64Interpreter::Run. This is for
// host callbacks entered by that interpreter; base + offset is checked before
// addition, and an invalid guest interval sets precise read-fault metadata.
bool X64ReadGuestMemory(uint64_t base, size_t offset, void* destination, size_t size);
void X64RollbackHostCallStack(X64State* state);

// Available when LUMEN_X64_MEMORY_TRACE=1. Intended for diagnosing the first
// divergence of interpreted guests without installing host watchpoints.
bool X64FindLastMemoryWrite(uint64_t address, size_t size, X64MemoryWriteInfo* out);

#if defined(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY)
void X64BeginA64DynarecStartupTelemetry();
bool X64ClaimA64DynarecFirstVideoSubmit();
void X64FinishA64DynarecFirstVideoSubmit();
bool X64ExcludeA64DynarecStartupFallbackForHostCall();
#if defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
bool X64BeginA64DynarecStartupVideoBoundaryForTest();
bool X64A64DynarecStartupTelemetryIsOpenForTest();
bool X64CloseA64DynarecStartupVideoBoundaryForTest();
#endif
#endif

#if defined(LUMEN_CPU_X64_TEST) && \
    defined(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY)
enum class X64A64DynarecStartupCostForTest : uint8_t
{
	Lower,
	Compile,
	Reset,
};

enum class X64A64DynarecStartupFallbackForTest : uint8_t
{
	Ordinary,
	Precise,
};

enum class X64A64DynarecStartupReasonForTest : uint8_t
{
	Lowering,
	Eligibility,
	DelayedAdmission,
	Busy,
	Precheck,
	SignatureChanged,
	Other,
};

struct X64A64DynarecStartupAdmissionForTest
{
	uintptr_t shard_token = 0;
	uint64_t generation = 0;
	uint64_t begin_ns = 0;
};

struct X64A64DynarecStartupCounterForTest
{
	uint64_t events = 0;
	uint64_t steps = 0;
	uint64_t nanoseconds = 0;
};

struct X64A64DynarecStartupSnapshotForTest
{
	bool accepting = false;
	bool closed = false;
	bool complete = false;
	bool invalid = false;
	uint64_t epoch_begin_ns = 0;
	uint64_t cutoff_ns = 0;
	uint64_t epoch_wall_ns = 0;
	uint64_t measured_ns = 0;
	uint64_t active_writers = 0;
	uint64_t reports = 0;
	uint64_t shard_count = 0;
	uint64_t shard_drops = 0;
	uint64_t pair_count = 0;
	uint64_t pair_drops = 0;
	uint64_t overflow = 0;
	uint64_t invalid_records = 0;
	X64A64DynarecStartupCounterForTest lower {};
	X64A64DynarecStartupCounterForTest compile {};
	X64A64DynarecStartupCounterForTest reset {};
	X64A64DynarecStartupCounterForTest ordinary {};
	X64A64DynarecStartupCounterForTest precise {};
	X64A64DynarecStartupCounterForTest singleton_total {};
	X64A64DynarecStartupCounterForTest pair_total {};
	X64A64DynarecStartupCounterForTest three_plus {};
	X64A64DynarecStartupCounterForTest zero {};
	X64A64DynarecStartupCounterForTest queried_singleton {};
	X64A64DynarecStartupCounterForTest queried_pair {};
};

bool X64BeginA64DynarecStartupTelemetryForTest(
	uint64_t begin_ns, uint32_t shard_limit, uint32_t pair_limit);
X64A64DynarecStartupAdmissionForTest
X64AdmitA64DynarecStartupTelemetryForTest(uint64_t begin_ns);
bool X64RecordA64DynarecStartupCostForTest(
	X64A64DynarecStartupAdmissionForTest* admission,
	X64A64DynarecStartupCostForTest cost, uint64_t end_ns);
bool X64RecordA64DynarecStartupFallbackForTest(
	X64A64DynarecStartupAdmissionForTest* admission,
	X64A64DynarecStartupFallbackForTest fallback,
	X64A64DynarecStartupReasonForTest reason, const uint16_t* kinds,
	size_t kind_count, uint64_t steps, uint64_t end_ns);
bool X64CloseA64DynarecStartupTelemetryForTest(uint64_t cutoff_ns);
bool X64ClaimA64DynarecFirstVideoSubmitForTest(uint64_t cutoff_ns);
X64A64DynarecStartupSnapshotForTest
X64GetA64DynarecStartupTelemetryForTest(
	uint16_t singleton_query, uint16_t pair_first, uint16_t pair_second);
bool X64A64DynarecStartupTelemetryEnvironmentRequestedForTest();
bool X64ValidateA64DynarecStartupScopeNestingForTest();
bool X64ValidateA64DynarecStartupHostCallExclusionForTest();
bool X64EmitA64DynarecStartupTelemetryReportForTest();
#endif

#if defined(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
void X64BeginA64DynarecPhase6AFTelemetry();
bool X64ClaimA64DynarecPhase6AFFirstVideoSubmit();
void X64FinishA64DynarecPhase6AFFirstVideoSubmit();
bool X64ExcludeA64DynarecPhase6AFSampleForHostCall();
#if defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
bool X64BeginA64DynarecPhase6AFVideoBoundaryForTest();
bool X64A64DynarecPhase6AFTelemetryIsOpenForTest();
bool X64CloseA64DynarecPhase6AFVideoBoundaryForTest();
bool X64ValidateA64DynarecPhase6AFVideoBoundaryReportForTest();
#endif
#endif

#if defined(LUMEN_A64_DYNAREC_PHASE6CM_PROFILE)
// A host callback can wait or re-enter the guest. Exclude the complete current
// 6CM sample window before crossing that boundary.
bool X64ExcludeA64DynarecPhase6CMSampleForHostCall();
#endif

#if defined(LUMEN_A64_DYNAREC_PHASE6CO_PROFILE)
// The snapshot diagnostic excludes a complete cluster before crossing an HLE
// boundary because a callback can wait or enter a nested guest Run.
bool X64ExcludeA64DynarecPhase6COSampleForHostCall();
#endif

#if defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE)
// The prepare boundary arms allocation lifecycle tracking before module
// initializers can enter the guest. Begin fixes the measured entry boundary;
// the first accepted VideoOut submit claims its cutoff under the queue mutex,
// while Finish freezes and serializes the registry only after that mutex has
// been released.
void X64PrepareA64DynarecHostPcProfile();
void X64BeginA64DynarecHostPcProfile();
bool X64ClaimA64DynarecHostPcProfileFirstSubmit();
void X64FinishA64DynarecHostPcProfileFirstSubmit();
#if defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE_TEST)
bool X64ValidateA64DynarecHostPcProfileSnapshotForTest();
bool X64ValidateA64DynarecIrStorageProfileCloseRaceForTest();
#endif
#endif

#if defined(LUMEN_CPU_X64_TEST) && \
    defined(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
struct X64A64DynarecPhase6AFCostForTest
{
	uint64_t events = 0;
	uint64_t raw_nanoseconds = 0;
	uint64_t corrected_nanoseconds = 0;
};

struct X64A64DynarecPhase6AFStratumForTest
{
	X64A64DynarecPhase6AFCostForTest try_paired {};
	X64A64DynarecPhase6AFCostForTest compiled_execute {};
	X64A64DynarecPhase6AFCostForTest paired_surrounding {};
};

enum class X64A64DynarecPhase6AFDispositionForTest : uint8_t
{
	Complete,
	HleExcluded,
	ReentrantExcluded,
	RecordDrop,
};

struct X64A64DynarecPhase6AFSnapshotForTest
{
	bool accepting = false;
	bool closed = false;
	bool complete = false;
	bool invalid = false;
	uint64_t epoch_begin_ns = 0;
	uint64_t cutoff_ns = 0;
	uint64_t epoch_wall_ns = 0;
	uint64_t clock_floor_ns = 0;
	uint64_t clock_pairs = 0;
	uint64_t block_size = 0;
	uint64_t stratum_count = 0;
	uint64_t replica_seed = 0;
	uint64_t generation = 0;
	uint64_t active_samples = 0;
	uint64_t reports = 0;
	uint64_t scheduled_samples = 0;
	uint64_t complete_samples = 0;
	uint64_t paired_samples = 0;
	uint64_t unpaired_samples = 0;
	uint64_t excluded_hle_samples = 0;
	uint64_t excluded_reentrant_samples = 0;
	uint64_t excluded_cutoff_samples = 0;
	uint64_t record_drops = 0;
	uint64_t cutoff_clipped_samples = 0;
	uint64_t h_ppm = 0;
	uint64_t represented_calls = 0;
	uint64_t paired_steps = 0;
	uint64_t paired_direct_steps = 0;
	uint64_t all_direct_samples = 0;
	uint64_t helper_backed_samples = 0;
	uint64_t zero_step_samples = 0;
	uint64_t shard_count = 0;
	uint64_t shard_drops = 0;
	uint64_t run_configurations = 0;
	uint64_t fast_blocks_enabled = 0;
	uint64_t dynarec_requested = 0;
	uint64_t dynarec_enabled = 0;
	uint64_t diagnostic_history_enabled = 0;
	uint64_t delayed_admission_enabled = 0;
	uint64_t archive_enabled = 0;
	uint64_t max_sample_raw_nanoseconds = 0;
	uint64_t max_sample_corrected_nanoseconds = 0;
	uint64_t overflow = 0;
	uint64_t underflow = 0;
	uint64_t invalid_records = 0;
	uint64_t q_raw_ppm = 0;
	uint64_t q_corrected_ppm = 0;
	uint64_t checksum = 0;
	uint64_t first_owner_ordinal = 0;
	uint64_t first_owner_seed = 0;
	uint64_t first_owner_block_index = 0;
	uint64_t first_owner_draw_count = 0;
	uint64_t first_owner_represented_calls = 0;
	uint64_t first_owner_position = 0;
	bool classifiable = false;
	bool eligible = false;
	X64A64DynarecPhase6AFCostForTest try_all {};
	X64A64DynarecPhase6AFCostForTest try_paired {};
	X64A64DynarecPhase6AFCostForTest compiled_execute {};
	X64A64DynarecPhase6AFCostForTest paired_surrounding {};
	X64A64DynarecPhase6AFCostForTest try_unpaired {};
	X64A64DynarecPhase6AFStratumForTest strata[16] {};
};

bool X64BeginA64DynarecPhase6AFTelemetryForTest(
	uint64_t begin_ns, uint32_t shard_limit, uint64_t replica_seed,
	uint64_t clock_floor_ns);
bool X64RecordA64DynarecPhase6AFSampleForTest(
	uint64_t outer_start_ns, bool paired, uint64_t compiled_start_ns,
	uint64_t compiled_end_ns, uint64_t outer_end_ns, uint32_t steps,
	uint32_t direct_steps,
	X64A64DynarecPhase6AFDispositionForTest disposition);
using X64A64DynarecPhase6AFCallbackForTest = bool (*)(void* context);
bool X64RunSelectedA64DynarecPhase6AFSampleForTest(
	bool paired, X64A64DynarecPhase6AFCallbackForTest callback,
	void* context);
bool X64A64DynarecPhase6AFSamplerPositionsForTest(
	uint64_t replica_seed, uint64_t generation, uint64_t owner_ordinal,
	uint16_t* positions, size_t position_count);
bool X64CloseA64DynarecPhase6AFTelemetryForTest(uint64_t cutoff_ns);
bool X64ClaimA64DynarecPhase6AFFirstVideoSubmitForTest(uint64_t cutoff_ns);
X64A64DynarecPhase6AFSnapshotForTest
X64GetA64DynarecPhase6AFTelemetryForTest();
bool X64A64DynarecPhase6AFTelemetryEnvironmentRequestedForTest();
bool X64ValidateA64DynarecPhase6AFCoreForTest();
bool X64EmitA64DynarecPhase6AFTelemetryReportForTest();
#endif

#if defined(LUMEN_CPU_X64_TEST) || defined(LUMEN_CPU_X64_FAST_BLOCK_TEST_HOOK)
void     X64SetFastBlockTestTarget(uint64_t rip);
uint64_t X64FastBlockTestTargetInstructionCount();
#endif

#if defined(LUMEN_CPU_X64_TEST) && defined(LUMEN_FAST_BLOCK_TELEMETRY)
struct X64FastBlockTelemetryTestStats
{
	bool enabled = false;
	bool finalized = false;
	bool activation_signal = false;
	bool armed = false;
	uint64_t attempts = 0;
	uint64_t hits = 0;
	uint64_t instructions = 0;
	uint64_t ordinary_blocks = 0;
	uint64_t operation_labels = 0;
	uint64_t pair_labels = 0;
	uint64_t kind_count = 0;
	uint64_t pair_count = 0;
	uint64_t top_pair_key = 0;
	uint64_t top_pair_count = 0;
	uint64_t recognizer_count = 0;
	uint64_t recognizer_steps = 0;
	uint64_t rejected_attempts = 0;
	uint64_t excluded_smc = 0;
	uint64_t excluded_untracked = 0;
	uint64_t dropped = 0;
	uint64_t dropped_unclassified = 0;
	uint64_t dropped_sidecar = 0;
	uint64_t dropped_reject_capacity = 0;
	uint64_t dropped_thread_registry = 0;
	uint64_t dropped_internal = 0;
	uint64_t thread_count = 0;
	uint64_t threads_dropped = 0;
	uint64_t snapshots = 0;
	uint64_t final_snapshots = 0;
	uint64_t reject_family_count = 0;
	uint64_t top_reject_key = 0;
	uint64_t top_reject_count = 0;
};

void X64ResetFastBlockTelemetryForTest(uint64_t interval, uint64_t limit,
	                                   bool wait_for_signal = false);
X64FastBlockTelemetryTestStats X64GetFastBlockTelemetryTestStats();
uint64_t X64NormalizeFastBlockRejectFamilyForTest(const uint8_t* code,
	                                              size_t available);
void X64RecordFastBlockRejectSampleForTest(const uint8_t* code,
	                                       size_t available);
void X64RecordFastBlockRejectFamilyForTest(uint64_t family_key);
void X64RecordFastBlockTelemetryAttemptForTest(bool hit, size_t instructions);
void X64RecordFastBlockTelemetryTouchedOverflowForTest();
uint8_t X64FastBlockTelemetryThreadOrdinalForTest();
bool X64FastBlockTelemetrySignalActivationSupportedForTest();
bool X64SignalFastBlockTelemetryForTest();
bool X64ValidateFastBlockTelemetrySnapshotFormatForTest();
bool X64ValidateFastBlockTelemetryDeadRflagsNormalizationForTest();
#endif

#if defined(LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY)
void X64MarkA64DynarecPhase6ALPrimaryThread();
#endif

#if defined(LUMEN_CPU_X64_TEST) && \
    defined(LUMEN_A64_DYNAREC_RFLAGS_PROFILE)
namespace A64Dynarec {
struct IrBlock;
}

struct X64A64DynarecRflagsProfileAnalysisForTest
{
	bool classified = false;
	bool touches_flags = false;
	bool possibly_writes = false;
	bool eligible = false;
	uint64_t baseline_unconditional_accesses = 0;
	uint64_t candidate_cost = 0;
	uint64_t writers = 0;
	uint64_t unconditional_writers = 0;
	uint64_t pure_readers = 0;
	uint64_t subtract_with_borrow = 0;
	uint64_t shift_cl = 0;
};

uint64_t X64A64DynarecRflagsProfileSampleIntervalForTest();
uint16_t X64A64DynarecRflagsProfileWindowOffsetForTest(
	uint64_t owner_ordinal, uint64_t window);
bool X64A64DynarecRflagsProfilePositionSelectedForTest(
	uint64_t owner_ordinal, uint64_t window, uint16_t position);
bool X64AnalyzeA64DynarecRflagsProfileForTest(
	const A64Dynarec::IrBlock& block,
	X64A64DynarecRflagsProfileAnalysisForTest* output);
bool X64AddA64DynarecRflagsProfileBoundedForTest(
	uint64_t current, uint64_t value, uint64_t* result);
bool X64ValidateA64DynarecRflagsProfileInvalidOwnerSnapshotForTest();
#endif

#if defined(LUMEN_CPU_X64_TEST) && \
    defined(LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE)
namespace A64Dynarec {
struct IrBlock;
struct BlockGprCachePlan;
}

struct X64A64DynarecGprCacheProfileSnapshotForTest
{
	uint64_t reports = 0;
	uint64_t sampled_entries = 0;
	uint64_t sampled_steps = 0;
	uint64_t classified_entries = 0;
	uint64_t classified_steps = 0;
	uint64_t rejected_entries = 0;
	uint64_t rejected_steps = 0;
	uint64_t gpr_touch_entries = 0;
	uint64_t gpr_touch_steps = 0;
	uint64_t eligible_entries = 0;
	uint64_t eligible_steps = 0;
	uint64_t partial_hazard_entries = 0;
	uint64_t shift_cl_hazard_entries = 0;
	uint64_t eligible_baseline_loads = 0;
	uint64_t eligible_baseline_stores = 0;
	uint64_t eligible_baseline_accesses = 0;
	uint64_t eligible_candidate_initial_loads = 0;
	uint64_t eligible_candidate_final_stores = 0;
	uint64_t eligible_candidate_accesses = 0;
	uint64_t candidate_prepare_words = 0;
	uint64_t candidate_commit_words = 0;
	uint64_t eligible_candidate_cache_words = 0;
	uint64_t eliminated_access_floor = 0;
	uint64_t eliminated_word_floor = 0;
	uint64_t selected_read_only = 0;
	uint64_t selected_write_only = 0;
	uint64_t selected_readwrite = 0;
	uint64_t selected_lazy_init = 0;
	uint64_t selected_partial_width = 0;
	uint64_t selected_alias = 0;
	uint64_t selected_shift_cl = 0;
	uint64_t selected_conditional_write = 0;
	uint64_t selected_gpr[16] = {};
	uint64_t cache_owners = 0;
	uint64_t cache_configuration_invalid = 0;
	bool overflow = false;
};

uint64_t X64A64DynarecGprCacheProfileSampleIntervalForTest();
uint16_t X64A64DynarecGprCacheProfileWindowOffsetForTest(
	uint64_t owner_ordinal, uint64_t window);
bool X64A64DynarecGprCacheProfilePositionSelectedForTest(
	uint64_t owner_ordinal, uint64_t window, uint16_t position);
void X64ResetA64DynarecGprCacheProfileSamplerForTest(
	uint64_t owner_ordinal, uint64_t window);
void X64SetA64DynarecGprCacheProfileForceGenericForTest(bool force_generic);
bool X64AddA64DynarecGprCacheProfileBoundedForTest(
	uint64_t current, uint64_t value, uint64_t* result);
bool X64ValidateA64DynarecGprCacheProfilePlanForTest(
	const A64Dynarec::IrBlock& block,
	const A64Dynarec::BlockGprCachePlan& plan);
void X64AccumulateA64DynarecGprCacheProfileForTest(
	const A64Dynarec::IrBlock& block, uint32_t native_steps);
X64A64DynarecGprCacheProfileSnapshotForTest
X64GetA64DynarecGprCacheProfileSnapshotForTest();
bool X64ValidateA64DynarecGprCacheProfileInvalidOwnerSnapshotForTest();
bool X64ValidateA64DynarecGprCacheProfileStickySaturationForTest();
#endif

// Drain the opt-in phase-6CG worker before desktop's deliberate std::_Exit.
// The production selector absent path is a no-op.
bool X64FinalizeA64DynarecDenseAsyncCompilation();

// Emit the exact-once phase-6CL coverage receipt before desktop's deliberate
// std::_Exit. The selector-absent path is a no-op.
bool X64FinalizeA64DynarecStableNegativeRoute();

// Emit the exact-once profitable admission route coverage receipt before
// desktop's deliberate std::_Exit. The selector-absent path is a no-op.
bool X64FinalizeA64DynarecProfitableBlockAdmission();

// Emit the exact-once FastBlock-primary/native-only Dynarec admission receipt
// before desktop's deliberate std::_Exit. The selector-absent path is a no-op.
bool X64FinalizeA64DynarecNativeOnlyAdmission();

// Emit the exact-once phase-6CP coverage receipt before desktop's deliberate
// std::_Exit. The selector-absent path is a no-op.
bool X64FinalizeA64DynarecGuestMemoryOperationSnapshotElision();

// Emit the exact-once phase-6CQ admission/execution receipt before desktop's
// deliberate std::_Exit. The selector-absent path is a no-op.
bool X64FinalizeA64DynarecScalarMemoryFallthroughAdmission();

// Emit the exact-once phase-6CS coverage receipt before desktop's deliberate
// std::_Exit. The selector-absent path is a no-op.
bool X64FinalizeFastBlockDeadRflagsElision();

// Emit the exact-once phase-6DA directory coverage/coherence receipt before
// desktop's deliberate std::_Exit. The selector-absent path is a no-op.
bool X64FinalizeFastBlockDenseDirectory();

// Emit the exact-once inline fixed-signature coverage receipt before desktop's
// deliberate std::_Exit. The selector-absent path is a no-op.
bool X64FinalizeFastBlockInlineFixedSignatureMatch();

// Emit the exact-once UFSB dependency/comparison/chunk receipt before desktop's
// deliberate std::_Exit. The selector-absent path is a no-op.
bool X64FinalizeFastBlockUnrolledFixedSignatureBatching();

// Emit the exact-once idempotent FastBlock MRU-store coverage receipt before
// desktop's deliberate std::_Exit. The selector-absent path is a no-op.
bool X64FinalizeFastBlockIdempotentMruStoreElision();

// Emit the exact-once callback-free ordinary specialization receipt before
// desktop's deliberate std::_Exit. The selector-absent path is a no-op.
bool X64FinalizeFastBlockCallbackFreeOrdinarySpecialization();

#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
// Phase-6CK diagnostic snapshots are polled only after VideoOut releases its
// mutex. Keep the API absent from ordinary builds so their frame and shutdown
// paths retain the production shape.
void X64PollA64DynarecPhase6CKProfile(uint64_t present,
	                                  bool checkpoint_reached);
bool X64FinalizeA64DynarecPhase6CKProfile();
#endif

#if defined(LUMEN_A64_DYNAREC_PHASE6CM_PROFILE)
// Phase-6CM uses the same post-VideoOut-mutex polling boundary as phase-6CK.
// Keep the diagnostic API absent from ordinary production builds.
void X64PollA64DynarecPhase6CMProfile(uint64_t present,
	                                  bool checkpoint_reached);
bool X64FinalizeA64DynarecPhase6CMProfile();
#endif

#if defined(LUMEN_A64_DYNAREC_PHASE6CO_PROFILE)
// Phase-6CO snapshots are emitted only after the VideoOut mutex is released.
void X64PollA64DynarecPhase6COProfile(uint64_t present,
	                                  bool checkpoint_reached);
bool X64FinalizeA64DynarecPhase6COProfile();
#endif

#if defined(LUMEN_CPU_X64_TEST)
// Isolate independent test cases that may reuse one host stack address for
// different short guest byte arrays. Dedicated within-test SMC cases retain
// the cache between their own runs.
void X64ResetFastBlockCacheForTest();

struct X64A64DynarecCacheTestStats
{
	uint64_t build_attempts   = 0;
	uint64_t compile_attempts = 0;
	uint64_t positive_hits    = 0;
	uint64_t negative_hits    = 0;
	uint64_t evictions        = 0;
	uint64_t positive_evictions = 0;
	uint64_t negative_evictions = 0;
	uint64_t admission_deferrals = 0;
	uint64_t admission_promotions = 0;
	uint64_t native_chain_continuations = 0;
	uint64_t prepared_direct_entries = 0;
	uint64_t direct_context_initializations = 0;
	uint64_t direct_context_reuses = 0;
	uint64_t direct_block_binding_probes = 0;
	uint64_t direct_block_binding_hits = 0;
	uint64_t direct_block_binding_installs = 0;
	uint64_t direct_block_binding_stale = 0;
	uint64_t dense_resident_probes = 0;
	uint64_t dense_resident_hits = 0;
	uint64_t dense_resident_builds = 0;
	uint64_t dense_resident_bypasses = 0;
	uint64_t dense_resident_fallbacks = 0;
	uint64_t dense_victim_archive_probes = 0;
	uint64_t dense_victim_archive_hits = 0;
	uint64_t dense_victim_archive_misses = 0;
	uint64_t dense_victim_archive_stores = 0;
	uint64_t dense_victim_archive_drops = 0;
	uint64_t dense_victim_archive_reclaims = 0;
	uint64_t dense_victim_archive_smc_purges = 0;
	uint64_t dense_victim_archive_resident = 0;
	uint64_t dense_victim_archive_resident_peak = 0;
	uint64_t dense_victim_archive_last_code_capacity = 0;
	uint64_t dense_async_enqueue_attempts = 0;
	uint64_t dense_async_enqueued = 0;
	uint64_t dense_async_pending_fallbacks = 0;
	uint64_t dense_async_duplicate_suppressed = 0;
	uint64_t dense_async_queue_full = 0;
	uint64_t dense_async_completed = 0;
	uint64_t dense_async_installed = 0;
	uint64_t dense_async_stale_drops = 0;
	uint64_t dense_async_capacity_waits = 0;
	uint64_t dense_async_bypasses = 0;
	uint64_t dense_async_failures = 0;
	uint64_t dense_async_distinct_worker = 0;
	uint64_t dense_async_max_outstanding = 0;
	uint64_t dense_async_sync_eligible_compiles = 0;
	uint64_t dense_async_probation_started = 0;
	uint64_t dense_async_probation_promoted = 0;
	uint64_t dense_async_probation_dropped = 0;
	uint64_t hybrid_direct_cpp_classifications = 0;
	uint64_t hybrid_direct_cpp_hits = 0;
	uint64_t hybrid_direct_fused_entries = 0;
	uint64_t stable_negative_route_classifications = 0;
	uint64_t stable_negative_route_hits = 0;
	uint64_t stable_negative_route_live_mismatches = 0;
	uint64_t operation_snapshot_copies = 0;
	uint64_t operation_snapshot_elisions = 0;
	uint64_t execution_signature_snapshots = 0;
	uint64_t ordinary_native_probe_attempts = 0;
	uint64_t dead_rflags_elision_blocks = 0;
	uint64_t dead_rflags_elision_operations = 0;
	uint64_t contextless_dense_builds = 0;
	uint64_t contextless_dense_entries = 0;
	uint64_t contextless_dense_failures = 0;
	uint64_t archive_probes = 0;
	uint64_t archive_hits = 0;
	uint64_t archive_misses = 0;
	uint64_t archive_puts = 0;
	uint64_t archive_stored = 0;
	uint64_t archive_drops = 0;
	uint64_t archive_budget_drops = 0;
	uint64_t archive_invalid = 0;
	uint64_t archive_resident = 0;
	uint64_t archive_resident_peak = 0;
	uint64_t archive_code_resident = 0;
	uint64_t archive_code_peak = 0;
	uint64_t archive_code_stored = 0;
	uint64_t archive_code_hit = 0;
	uint64_t archive_code_drop = 0;
	uint64_t archive_capacity_resident = 0;
	uint64_t archive_capacity_peak = 0;
	uint64_t archive_capacity_stored = 0;
	uint64_t archive_capacity_hit = 0;
	uint64_t archive_capacity_drop = 0;
	uint64_t archive_global_capacity = 0;
	uint64_t archive_global_peak = 0;
	uint64_t archive_global_invalid = 0;
	uint64_t archive_entry_limit = 0;
	uint64_t archive_set_count = 0;
	uint64_t ir_instruction_validations = 0;
	uint64_t ir_full_block_verifications = 0;
	uint64_t ir_structural_block_verifications = 0;
	uint64_t ir_raw_storage_constructions = 0;
	uint64_t ir_unused_raw_placeholder_constructions = 0;
	uint64_t ir_verified_handoffs = 0;
	bool     separate_negative_cache = false;
	bool     delayed_positive_admission = false;
	bool     verified_ir_pipeline = false;
	bool     lazy_ir_storage = false;
	bool     compiled_archive_requested = false;
	bool     compiled_archive_enabled = false;
	bool     healthy          = true;
};

void     X64SetA64DynarecTestTarget(uint64_t rip);
uint64_t X64A64DynarecTestTargetInstructionCount();
uint64_t X64A64DynarecTestTargetDirectInstructionCount();
bool     X64A64DynarecAvailableForTest();
bool     X64A64DynarecDefaultRequestedForTest();
bool     X64A64DynarecDirectContextReuseEnabledForTest();
bool     X64A64DynarecDirectBlockBindingRequestedForTest();
bool     X64A64DynarecDirectBlockBindingEnvironmentValueEnabledForTest(
             const char* value);
bool     X64A64DynarecDenseResidentCacheRequestedForTest();
bool     X64A64DynarecHybridDirectEntryEnvironmentValueEnabledForTest(
             const char* value);
bool     X64A64DynarecHybridDirectEntryRequestedForTest();
uint8_t  X64A64DynarecHybridDirectEntryMaxOpsForTest();
bool     X64A64DynarecContextlessDenseEntryEnvironmentValueEnabledForTest(
             const char* value);
bool     X64A64DynarecContextlessDenseEntryRequestedForTest();
bool     X64A64DynarecDenseVictimArchiveEnvironmentValueEnabledForTest(
             const char* value);
bool     X64A64DynarecDenseVictimArchiveRequestedForTest();
size_t   X64A64DynarecDenseVictimArchiveEntriesForTest();
size_t   X64A64DynarecDenseVictimArchiveWaysForTest();
bool     X64A64DynarecDenseAsyncCompilationEnvironmentValueEnabledForTest(
             const char* value);
#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
bool     X64A64DynarecPhase6CKProfileEnvironmentValueEnabledForTest(
             const char* value);
bool     X64A64DynarecPhase6CKConfigurationReadyForTest();
struct X64A64DynarecPhase6CKProfileReuseSnapshot
{
	uint64_t identity_first = 0;
	uint64_t identity_second = 0;
	uint64_t identity_third = 0;
	uint64_t reuse_second = 0;
	uint64_t reuse_third = 0;
	uint64_t retired_once = 0;
	uint64_t retired_twice = 0;
	uint64_t retired_three_plus = 0;
	uint64_t live_once = 0;
	uint64_t live_twice = 0;
	uint64_t live_three_plus = 0;
};
bool     X64CaptureA64DynarecPhase6CKProfileReuseForTest(
	         X64A64DynarecPhase6CKProfileReuseSnapshot* snapshot);
struct X64A64DynarecPhase6CKProfileFallbackSnapshot
{
	uint64_t sample_interval = 0;
	uint64_t batch_size = 0;
	uint64_t active_owners = 0;
	uint64_t unflushed_blocks_bound = 0;
	uint64_t ordinary_blocks = 0;
	uint64_t ordinary_steps = 0;
	uint64_t ordinary_samples = 0;
	uint64_t ordinary_sample_steps = 0;
	uint64_t ordinary_sample_ns = 0;
	uint64_t ordinary_flushes = 0;
	uint64_t precise_blocks = 0;
	uint64_t precise_steps = 0;
	uint64_t precise_samples = 0;
	uint64_t precise_sample_steps = 0;
	uint64_t precise_sample_ns = 0;
	uint64_t precise_flushes = 0;
};
bool     X64CaptureA64DynarecPhase6CKProfileFallbackForTest(
	         X64A64DynarecPhase6CKProfileFallbackSnapshot* snapshot);
bool     X64RecordA64DynarecPhase6CKFallbackForTest(
	         bool precise, uint64_t steps, uint64_t sampled_ns);
bool     X64FlushA64DynarecPhase6CKFallbackForTest();
bool     X64StressA64DynarecPhase6CKSnapshotConsistencyForTest();
bool     X64StressA64DynarecPhase6CKFinalizationForTest();
#endif
bool     X64A64DynarecDenseAsyncCompilationRequestedForTest();
bool     X64A64DynarecDenseAsyncStabilityAdmissionEnvironmentValueEnabledForTest(
             const char* value);
bool     X64A64DynarecDenseAsyncStabilityAdmissionRequestedForTest();
bool     X64A64DynarecStableNegativeRouteEnvironmentValueEnabledForTest(
             const char* value);
bool     X64A64DynarecStableNegativeRouteRequestedForTest();
bool     X64A64DynarecProfitableBlockAdmissionEnvironmentValueEnabledForTest(
             const char* value);
bool     X64A64DynarecProfitableBlockAdmissionRequestedForTest();
bool     X64A64DynarecNativeOnlyAdmissionEnvironmentValueEnabledForTest(
             const char* value);
bool     X64A64DynarecNativeOnlyAdmissionEnvironmentValueEnabledWithDefaultForTest(
             const char* value, bool default_enabled);
bool     X64A64DynarecNativeOnlyAdmissionRequestedForTest();
bool     X64A64DynarecNativeOnlyAdmissionStrictCoverageRequestedForTest();
bool     X64ValidateA64DynarecNativeOnlyAdmissionDefaultFinalizerForTest();
bool     X64A64DynarecGuestMemoryOperationSnapshotElisionEnvironmentValueEnabledForTest(
             const char* value);
bool     X64A64DynarecGuestMemoryOperationSnapshotElisionRequestedForTest();
bool     X64FastBlockNoJitOperationSnapshotElisionRequestedForTest();
struct X64OperationSnapshotCoverageForTest
{
	bool legacy_elided = false;
	bool legacy_signature = false;
	bool nojit_elided = false;
	bool nojit_signature = false;
	uint32_t nojit_receipt_emissions = 0;
};
X64OperationSnapshotCoverageForTest X64GetOperationSnapshotCoverageForTest();
bool     X64A64DynarecScalarMemoryFallthroughAdmissionEnvironmentValueEnabledForTest(
             const char* value);
bool     X64A64DynarecScalarMemoryFallthroughAdmissionRequestedForTest();
bool     X64FastBlockDeadRflagsElisionEnvironmentValueEnabledForTest(
             const char* value);
bool     X64FastBlockDeadRflagsElisionRequestedForTest();
size_t   X64FastBlockDeadRflagsElisionCountForTest(uint64_t rip);
bool     X64FastBlockInlineFixedSignatureMatchEnvironmentValueEnabledForTest(
             const char* value);
bool     X64FastBlockInlineFixedSignatureMatchRequestedForTest();
bool     X64FastBlockCodeMatchesForTest(
             const uint8_t* code, const uint8_t* expected, uint8_t size,
             bool inline_fixed);
bool     X64FastBlockUnrolledFixedSignatureBatchingEnvironmentValueEnabledForTest(
             const char* value);
bool     X64FastBlockUnrolledFixedSignatureBatchingRequestedForTest();
bool     X64FastBlockUnrolledFixedSignatureBatchingDependencySatisfiedForTest();
bool     X64FastBlockUnrolledFixedSignatureBatchingCodeMatchesForTest(
             const uint8_t* code, const uint8_t* expected, uint8_t size);
bool     X64FastBlockIdempotentMruStoreElisionEnvironmentValueEnabledForTest(
             const char* value);
bool     X64FastBlockIdempotentMruStoreElisionRequestedForTest();
bool     X64FastBlockCallbackFreeOrdinarySpecializationEnvironmentValueEnabledForTest(
             const char* value);
bool     X64FastBlockCallbackFreeOrdinarySpecializationRequestedForTest();
bool     X64FastBlockCallbackFreeOrdinarySpecializationReentrySafeForTest();
bool     X64A64CompactFastOpTierEnvironmentValueEnabledForTest(
             const char* value);
bool     X64A64CompactFastOpTierRequestedForTest();
struct X64A64CompactFastOpTierStatsForTest
{
	uint64_t hot_operations = 0;
	uint64_t cold_fallback_operations = 0;
	uint64_t compare_branch_pairs = 0;
	uint64_t stack_pairs = 0;
	uint64_t fp32_pairs = 0;
};
void X64ResetA64CompactFastOpTierStatsForTest();
X64A64CompactFastOpTierStatsForTest
X64GetA64CompactFastOpTierStatsForTest();
struct X64FastBlockMruStoreStatsForTest
{
	uint64_t stores[4] = {};
	uint64_t elisions[4] = {};
};
X64FastBlockMruStoreStatsForTest
X64GetFastBlockMruStoreStatsForTest();
bool     X64FastBlockDenseDirectoryEnvironmentValueEnabledForTest(
             const char* value);
bool     X64FastBlockDenseDirectoryRequestedForTest();
bool     X64FastBlockDenseDirectoryCoherentForTest();
size_t   X64FastBlockDenseDirectoryBytesForTest();
size_t   X64FastBlockDenseDirectoryEntriesForTest();
bool     X64CorruptFastBlockDenseDirectoryForTest(uint64_t rip);
bool     X64SwapFastBlockDenseDirectoryTagsForTest(uint64_t rip);
bool     X64A64DynarecFastBlockOperationSnapshotElisionEligibleForTest(
             uint64_t rip);
size_t   X64A64DynarecDenseAsyncWorkersForTest();
size_t   X64A64DynarecDenseAsyncQueueLimitForTest();
size_t   X64A64DynarecDenseAsyncOwnerLimitForTest();
void     X64SetA64DynarecDenseAsyncWorkerPausedForTest(bool paused);
bool     X64DrainA64DynarecDenseAsyncForTest();
size_t   X64A64DynarecDenseAsyncOutstandingForTest();
uint64_t X64A64DynarecDenseAsyncCancelledBeforeStartForTest();
size_t   X64FastBlockCacheSetForTest(uint64_t rip);
void     X64SetA64DynarecDirectBlockBindingOverrideForTest(int state);
void     X64PoisonA64DynarecDirectBlockBindingForTest(uint64_t rip);
bool     X64A64DynarecDirectBlockBindingPresentForTest(uint64_t rip);
bool     X64A64DynarecVerifiedIrPipelineRequestedForTest();
bool     X64A64DynarecLazyIrStorageRequestedForTest();
bool     X64A64DynarecCompiledArchiveRequestedForTest();
size_t   X64A64DynarecCompiledArchiveEntriesForTest();
size_t   X64A64DynarecCompiledArchiveSetsForTest();
size_t   X64A64DynarecCompiledArchiveWaysForTest();
uint64_t X64A64DynarecCompiledArchiveBudgetForTest();
void     X64SetA64DynarecCompiledArchiveOverrideForTest(int state);
bool     X64A64DynarecSeparateNegativeCacheAvailableForTest();
bool     X64A64DynarecDirectFallthroughAvailableForTest();
void     X64ResetA64DynarecCacheForTest();
X64A64DynarecCacheTestStats X64GetA64DynarecCacheTestStats();
size_t   X64A64DynarecCacheSetForTest(uint64_t rip);
const void* X64A64DynarecPositiveOwnerForTest(uint64_t rip);
uint8_t  X64A64DynarecFastBlockFlagsForTest(uint64_t rip);
bool     X64ValidateA64DynarecCompiledArchiveForTest();
#if defined(LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY)
bool     X64A64DynarecCrossOwnerTelemetryEnvironmentRequestedForTest();
bool     X64A64DynarecCrossOwnerTelemetryRuntimeEnabledForTest();
bool     X64ValidateA64DynarecCrossOwnerTelemetryForTest();
#endif
#if defined(LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY)
bool     X64A64DynarecPhase6ALTelemetryEnvironmentRequestedForTest();
bool     X64ValidateA64DynarecPhase6ALTelemetryForTest();
bool     X64EmitA64DynarecPhase6ALTelemetryReportForTest();
bool     X64ValidateA64DynarecPhase6ALHiddenWorkerInvalidForTest();
bool     X64EmitA64DynarecPhase6ALPrimaryAfterWorkerForTest();
bool     X64ValidateA64DynarecPhase6ALPostCompileBoundaryForTest();
#endif
#if defined(LUMEN_A64_DYNAREC_TELEMETRY)
bool     X64ValidateA64DynarecTelemetryVictimAccountingForTest();
#endif
void     X64SetA64DynarecPoisonRipForTest(uint64_t rip);
void     X64SetA64DynarecZeroAccountingRipForTest(uint64_t rip);
void     X64SetA64DynarecPartialCompletedRipForTest(uint64_t rip);
void     X64SetA64DynarecBadResumeRipForTest(uint64_t rip);
void     X64SetA64DynarecBusyRipForTest(uint64_t rip);
void     X64SetA64DynarecStableNegativeLiveMismatchRipForTest(uint64_t rip);
void     X64SetA64DynarecArchiveMutationRipForTest(uint64_t rip);
void     X64SetA64DynarecCompileFailureRipForTest(uint64_t rip);
#endif

class X64Interpreter
{
public:
	using HostCall = bool (*)(uint64_t target, X64State* state);
	// Return zero when the complete guest interval cannot be accessed.  The
	// size and access type are part of the contract so callers never need to
	// probe host memory with a potentially invalid memcpy.
	using AddressTranslate = uint64_t (*)(uint64_t address, size_t size, bool write);
	using ContextAddressTranslate = uint64_t (*)(void* context, uint64_t address, size_t size, bool write);
	// Export up to capacity authoritative cached regions for the current
	// translator context. The dynarec copies them into one entry invocation;
	// callers must synchronize any VM generation before returning them.
	using ContextMemoryRegions = size_t (*)(void* context, X64MemoryTranslationRegion* regions,
	                                      size_t capacity);
	// Diagnostic callbacks are used only when the corresponding opt-in
	// environment variables are present. ModuleResolve returns the mapped
	// guest interval for a loader-owned module name; DiagnosticLocation emits
	// the loader's module-relative location and guest frame chain.
	using ModuleResolve = bool (*)(const char* module_name, uint64_t* base, uint64_t* size);
	using DiagnosticLocation = void (*)(uint64_t rip, uint64_t frame_ptr);

	explicit X64Interpreter(HostCall host_call = nullptr, AddressTranslate address_translate = nullptr,
	                       ModuleResolve module_resolve = nullptr, DiagnosticLocation diagnostic_location = nullptr,
	                       bool diagnostic_history = false,
	                       ContextAddressTranslate context_address_translate = nullptr,
	                       void* address_translate_context = nullptr,
	                       ContextMemoryRegions context_memory_regions = nullptr,
	                       X64AddressTranslatorCapabilities address_translator_capabilities = {})
		: m_host_call(host_call), m_address_translate(address_translate), m_module_resolve(module_resolve),
		  m_diagnostic_location(diagnostic_location), m_diagnostic_history(diagnostic_history),
		  m_context_address_translate(context_address_translate),
		  m_address_translate_context(address_translate_context),
		  m_context_memory_regions(context_memory_regions),
		  m_address_translator_capabilities(address_translator_capabilities)
	{
	}
	X64RunResult Run(X64State* state, size_t max_steps = 1000000) const;

private:
	bool ExecuteOne(X64State* state) const;
	static uint64_t LegacyAddressTranslate(void* context, uint64_t address, size_t size, bool write);
	bool HasAddressTranslator() const;
	uint64_t TranslateGuestAddress(uint64_t address, size_t size, bool write) const;
	HostCall m_host_call = nullptr;
	AddressTranslate m_address_translate = nullptr;
	ModuleResolve m_module_resolve = nullptr;
	DiagnosticLocation m_diagnostic_location = nullptr;
	bool m_diagnostic_history = false;
	ContextAddressTranslate m_context_address_translate = nullptr;
	void* m_address_translate_context = nullptr;
	ContextMemoryRegions m_context_memory_regions = nullptr;
	X64AddressTranslatorCapabilities m_address_translator_capabilities {};
};

#if defined(LUMEN_CPU_X64_TEST)
// Test-only observation of the normalized capability currently scoped to Run.
X64AddressTranslatorReentryCapability
X64ActiveAddressTranslatorReentryCapabilityForTest();
#endif

} // namespace Lumen::Cpu

#endif
