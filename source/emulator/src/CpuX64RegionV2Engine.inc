// Included by CpuX64.cpp after the canonical FastBlock executor has been
// defined.  This file intentionally consumes CpuX64.cpp's private FastBlock and
// X64ThreadContext types; the pure scalar frame/executor lives in the adjacent
// header and has no dependency on the dispatcher cache.

constexpr size_t REGION_V2_CACHE_ENTRIES = 32;
constexpr size_t REGION_V2_MAX_SEGMENTS = 8;
constexpr size_t REGION_V2_MAX_OPERATIONS = 64;
constexpr size_t REGION_V2_MAX_ENVELOPE_BYTES = UINT8_MAX;
constexpr size_t REGION_V2_MAX_RECORDING_DEPTH = 8;

#if defined(_MSC_VER)
#define LUMEN_REGION_V2_NOINLINE __declspec(noinline)
#define LUMEN_REGION_V2_ALWAYS_INLINE __forceinline
#elif defined(__clang__) || defined(__GNUC__)
#define LUMEN_REGION_V2_NOINLINE __attribute__((noinline))
#define LUMEN_REGION_V2_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define LUMEN_REGION_V2_NOINLINE
#define LUMEN_REGION_V2_ALWAYS_INLINE inline
#endif

struct RegionV2Segment
{
	uint64_t rip = 0;
	uint64_t expected_next_rip = 0;
	uint8_t code_offset = 0;
	uint8_t byte_count = 0;
	uint8_t operation_begin = 0;
	uint8_t operation_count = 0;
};

struct RegionV2Trace
{
	uint64_t start_rip = 0;
	uint64_t page_guest_begin = 0;
	uint64_t mapping_identity = 0;
	uint64_t publication_generation = 0;
	RegionV2Segment segments[REGION_V2_MAX_SEGMENTS] {};
	FastOp operations[REGION_V2_MAX_OPERATIONS] {};
	uint8_t code[REGION_V2_MAX_ENVELOPE_BYTES] {};
	uint8_t segment_count = 0;
	uint8_t operation_count = 0;
	uint8_t envelope_size = 0;
	bool valid = false;
};

struct RegionV2Stats
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

#if LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2_REPORT
bool RegionV2EnvironmentValueEnabled(const char* value);

bool RegionV2ReportRequested()
{
	static const bool requested = RegionV2EnvironmentValueEnabled(
	    std::getenv("LUMEN_X64_A64_REGION_ENGINE_V2_REPORT"));
	return requested;
}

std::atomic_bool g_region_v2_configuration_reported {false};
std::atomic_bool g_region_v2_build_reported {false};
std::atomic_bool g_region_v2_commit_reported {false};
std::atomic_bool g_region_v2_lease_rejection_reported {false};

void ReportRegionV2Configuration(
	bool fast_blocks, bool requested, bool effective,
	bool dynarec_config_requested, bool dynarec_effective,
	bool diagnostics, uint64_t profile_interval,
	bool telemetry_compatible, size_t chain_limit)
{
	if (g_region_v2_configuration_reported.exchange(
	        true, std::memory_order_relaxed))
	{
		return;
	}
	std::fprintf(
	    stderr,
	    "[REGION_V2_CONFIG] compiled=1 requested=%u effective=%u "
	    "fast_blocks=%u chain_limit=%zu dynarec_config_requested=%u "
	    "dynarec_effective=%u diagnostics=%u profile_interval=%" PRIu64
	    " telemetry_compatible=%u lease=apple-immutable-rx-only\n",
	    requested ? 1u : 0u, effective ? 1u : 0u,
	    fast_blocks ? 1u : 0u, chain_limit,
	    dynarec_config_requested ? 1u : 0u,
	    dynarec_effective ? 1u : 0u, diagnostics ? 1u : 0u,
	    profile_interval, telemetry_compatible ? 1u : 0u);
}

void ReportRegionV2Build(const RegionV2Trace& trace)
{
	if (g_region_v2_build_reported.exchange(true, std::memory_order_relaxed))
	{
		return;
	}
	std::fprintf(
	    stderr,
	    "[REGION_V2_BUILD] start=0x%016" PRIx64
	    " segments=%u operations=%u envelope=%u mapping=%" PRIu64
	    " generation=%" PRIu64 "\n",
	    trace.start_rip, static_cast<unsigned>(trace.segment_count),
	    static_cast<unsigned>(trace.operation_count),
	    static_cast<unsigned>(trace.envelope_size), trace.mapping_identity,
	    trace.publication_generation);
}

void ReportRegionV2Commit(
	const RegionV2Trace& trace, uint8_t retired, uint64_t lease_generation)
{
	if (g_region_v2_commit_reported.exchange(true, std::memory_order_relaxed))
	{
		return;
	}
	std::fprintf(
	    stderr,
	    "[REGION_V2_COMMIT] start=0x%016" PRIx64
	    " segments=%u operations=%u retired=%u mapping=%" PRIu64
	    " publication_generation=%" PRIu64
	    " lease_generation=%" PRIu64 "\n",
	    trace.start_rip, static_cast<unsigned>(trace.segment_count),
	    static_cast<unsigned>(trace.operation_count),
	    static_cast<unsigned>(retired), trace.mapping_identity,
	    trace.publication_generation, lease_generation);
}

void ReportRegionV2LeaseRejection(uint64_t start_rip, uint8_t envelope_size)
{
	if (g_region_v2_lease_rejection_reported.exchange(
	        true, std::memory_order_relaxed))
	{
		return;
	}
	std::fprintf(
	    stderr,
	    "[REGION_V2_LEASE_REJECT] start=0x%016" PRIx64
	    " envelope=%u\n",
	    start_rip, static_cast<unsigned>(envelope_size));
}
#endif

// Compile counters and deterministic race seams out of production objects.
#if defined(LUMEN_CPU_X64_TEST)
thread_local RegionV2Stats g_region_v2_stats {};
thread_local X64RegionEngineV2PostValidationHookForTest
    g_region_v2_post_validation_hook_for_test = nullptr;
thread_local X64RegionEngineV2SegmentValidationHookForTest
    g_region_v2_segment_validation_hook_for_test = nullptr;
thread_local bool g_region_v2_unmanaged_code_lease_bypass_for_test = false;
#define LUMEN_REGION_V2_STAT_INCREMENT(field) \
	(++g_region_v2_stats.field)
#define LUMEN_REGION_V2_STAT_ADD(field, value) \
	(g_region_v2_stats.field += (value))
#else
#define LUMEN_REGION_V2_STAT_INCREMENT(field) ((void)0)
#define LUMEN_REGION_V2_STAT_ADD(field, value) ((void)0)
#endif

bool RegionV2EnvironmentValueEnabled(const char* value)
{
	return value != nullptr && std::strcmp(value, "1") == 0;
}

bool RegionV2Requested()
{
	// Trace identity, cache layout and execution policy are one process contract.
	// Freeze the exact-one selector before the first lookup, including nested Run.
	static const bool requested = RegionV2EnvironmentValueEnabled(
	    std::getenv("LUMEN_X64_A64_REGION_ENGINE_V2"));
	return requested;
}

size_t RegionV2CacheIndex(uint64_t rip)
{
	uint64_t key = rip >> 1u;
	key ^= key >> 13u;
	key ^= key >> 27u;
	return static_cast<size_t>(key) & (REGION_V2_CACHE_ENTRIES - 1u);
}

uint64_t RegionV2CodeAddress(
	const X64ThreadContext& context, uint64_t page_guest_begin,
	uint64_t start_rip, uint8_t envelope_size)
{
	if (envelope_size == 0 || start_rip < page_guest_begin ||
	    context.instruction_page_size != INSTRUCTION_PAGE_SIZE ||
	    context.instruction_page_guest_begin != page_guest_begin ||
	    context.instruction_page_host_begin == 0)
	{
		return 0;
	}
	const uint64_t offset = start_rip - page_guest_begin;
	if (offset > INSTRUCTION_PAGE_SIZE - envelope_size ||
	    context.instruction_page_host_begin > UINT64_MAX - offset)
		return 0;
	return context.instruction_page_host_begin + offset;
}

class RegionV2CodeReadLease final
{
public:
	RegionV2CodeReadLease() = default;
	~RegionV2CodeReadLease()
	{
		Core::VirtualMemory::ReleaseCodeReadLease(&m_lease);
	}

	RegionV2CodeReadLease(const RegionV2CodeReadLease&) = delete;
	RegionV2CodeReadLease& operator=(const RegionV2CodeReadLease&) = delete;

	bool Acquire(const X64ThreadContext& context, uint64_t page_guest_begin,
	             uint64_t start_rip, uint8_t envelope_size)
	{
		const uint64_t address = RegionV2CodeAddress(
		    context, page_guest_begin, start_rip, envelope_size);
		if (address == 0) return false;
		if (Core::VirtualMemory::TryAcquireCodeReadLease(
		        address, envelope_size, &m_lease))
		{
			if (m_lease.HostBegin() == address &&
			    m_lease.ByteCount() == envelope_size)
			{
				m_code = reinterpret_cast<const uint8_t*>(
				    static_cast<uintptr_t>(m_lease.HostBegin()));
				return true;
			}
			Core::VirtualMemory::ReleaseCodeReadLease(&m_lease);
		}
	#if defined(LUMEN_CPU_X64_TEST)
		// The semantic corpus intentionally mutates static arrays through test-only
		// seams. Production and VM integration tests fail closed on unmanaged code.
		if (g_region_v2_unmanaged_code_lease_bypass_for_test)
		{
			m_test_bypass = true;
			m_code = reinterpret_cast<const uint8_t*>(
			    static_cast<uintptr_t>(address));
			return true;
		}
	#endif
		return false;
	}

	const uint8_t* Code() const { return m_code; }
	uint64_t MappingIdentity() const
	{
	#if defined(LUMEN_CPU_X64_TEST)
		if (m_test_bypass) return UINT64_MAX;
	#endif
		return m_lease.MappingIdentity();
	}
	uint64_t MutationGeneration() const
	{
		return m_lease.MutationGeneration();
	}

private:
	Core::VirtualMemory::CodeReadLease m_lease {};
	const uint8_t* m_code = nullptr;
	#if defined(LUMEN_CPU_X64_TEST)
	bool m_test_bypass = false;
	#endif
};

LUMEN_REGION_V2_ALWAYS_INLINE bool RegionV2SegmentBytesMatch(
	const uint8_t* current, const uint8_t* expected, uint8_t byte_count)
{
	// Keep the future-segment coherency check inside the regional loop. Calling
	// libc memcmp here would force the four guest GPRs out of host registers at
	// every old FastBlock boundary and recreate the resident-scalar failure mode.
	for (uint8_t index = 0; index < byte_count; index++)
	{
		if (current[index] != expected[index]) return false;
	}
	return true;
}

bool RegionV2BlockEligible(const FastBlock& block)
{
	return block.valid && !block.instruction_window && block.op_count != 0 &&
	       (block.successor_replace & FAST_BLOCK_REGION_V2_PURE_SCALAR) != 0 &&
	       (block.successor_replace & FAST_BLOCK_DEAD_RFLAGS_ELISION_PRESENT) == 0 &&
	       (block.vmovd_reg32_to_xmm & FAST_BLOCK_EXECUTION_REENTRY_SAFE) != 0 &&
	       !block.byte_mask_scan && !block.byte_zero_scan &&
	       !block.byte_table_mask_scan && !block.byte_copy_loop &&
	       !block.bit_count_loop;
}

struct RegionV2RecordedBlock
{
	const FastBlock* block = nullptr;
	uint64_t rip = 0;
	uint64_t expected_next_rip = 0;
	uint8_t byte_count = 0;
	uint8_t operation_count = 0;
};

struct RegionV2RecordingState
{
	X64ThreadContext* context = nullptr;
	RegionV2RecordedBlock blocks[REGION_V2_MAX_SEGMENTS] {};
	uint64_t start_rip = 0;
	uint64_t page_guest_begin = 0;
	uint64_t last_next_rip = 0;
	uint64_t envelope_end = 0;
	uint8_t segment_count = 0;
	uint8_t operation_count = 0;
	bool sealed = false;
};

// Resolve one compact TLS owner once per Run, then pass it through every
// regional lookup and recorder. The 80 KiB trace cache and recording stack are
// still lazy heap allocations; selector OFF never resolves this TLS object.
struct RegionV2ThreadState
{
	std::unique_ptr<RegionV2Trace[]> cache;
	std::unique_ptr<RegionV2RecordingState[]> recording_states;
	uint8_t recording_depth = 0;
	#if LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2_REPORT
	bool report_requested = RegionV2ReportRequested();
	#endif
};

thread_local RegionV2ThreadState g_region_v2_thread_state;

RegionV2ThreadState& RegionV2CurrentThreadState()
{
	return g_region_v2_thread_state;
}

RegionV2Trace* RegionV2Cache(RegionV2ThreadState& thread_state)
{
	if (thread_state.cache == nullptr)
	{
		thread_state.cache.reset(new (std::nothrow)
		                         RegionV2Trace[REGION_V2_CACHE_ENTRIES]);
	}
	return thread_state.cache.get();
}

class RegionV2Recorder final
{
public:
	RegionV2Recorder(RegionV2ThreadState& thread_state,
	                 X64ThreadContext& context, uint64_t start_rip)
	    : m_thread_state(&thread_state)
	{
		if (thread_state.recording_depth >= REGION_V2_MAX_RECORDING_DEPTH)
		{
			return;
		}
		if (thread_state.recording_states == nullptr)
		{
			thread_state.recording_states.reset(new (std::nothrow)
			    RegionV2RecordingState[REGION_V2_MAX_RECORDING_DEPTH]);
			if (thread_state.recording_states == nullptr) return;
		}
		m_slot = thread_state.recording_depth++;
		m_state = &thread_state.recording_states[m_slot];
		m_state->context = &context;
		m_state->start_rip = start_rip;
		m_state->page_guest_begin =
		    start_rip & ~(INSTRUCTION_PAGE_SIZE - 1u);
		m_state->last_next_rip = 0;
		m_state->envelope_end = start_rip;
		m_state->segment_count = 0;
		m_state->operation_count = 0;
		m_state->sealed = false;
	}

	~RegionV2Recorder()
	{
		if (m_state == nullptr) return;
		if (m_state->segment_count >= 2 && m_state->operation_count >= 2)
			Publish();
		assert(m_thread_state != nullptr &&
		       m_thread_state->recording_depth == m_slot + 1u);
		m_thread_state->recording_depth--;
	}
	RegionV2Recorder(const RegionV2Recorder&) = delete;
	RegionV2Recorder& operator=(const RegionV2Recorder&) = delete;

	void Barrier()
	{
		if (m_state == nullptr || m_state->sealed) return;
		m_state->sealed = true;
		LUMEN_REGION_V2_STAT_INCREMENT(barriers);
	}

	void Observe(const FastBlock& block, uint64_t next_rip)
	{
		if (m_state == nullptr || m_state->sealed) return;
		const uint64_t page_end =
		    m_state->page_guest_begin + INSTRUCTION_PAGE_SIZE;
		if (!RegionV2BlockEligible(block) ||
		    block.rip < m_state->page_guest_begin || block.rip >= page_end ||
		    block.rip > UINT64_MAX - block.byte_count ||
		    block.rip + block.byte_count > page_end ||
		    (m_state->segment_count == 0 &&
		     block.rip != m_state->start_rip) ||
		    (m_state->segment_count != 0 &&
		     (block.rip != m_state->last_next_rip ||
		      block.rip < m_state->envelope_end)))
		{
			Barrier();
			return;
		}

		const uint64_t envelope_end = block.rip + block.byte_count;
		const uint64_t envelope_size = envelope_end - m_state->start_rip;
		if (m_state->segment_count >= REGION_V2_MAX_SEGMENTS ||
		    static_cast<size_t>(m_state->operation_count) + block.op_count >
		        REGION_V2_MAX_OPERATIONS ||
		    envelope_size == 0 ||
		    envelope_size > REGION_V2_MAX_ENVELOPE_BYTES)
		{
			Barrier();
			return;
		}

		auto& recorded = m_state->blocks[m_state->segment_count++];
		recorded.block = &block;
		recorded.rip = block.rip;
		recorded.expected_next_rip = next_rip;
		recorded.byte_count = block.byte_count;
		recorded.operation_count = block.op_count;
		m_state->operation_count = static_cast<uint8_t>(
		    m_state->operation_count + block.op_count);
		m_state->last_next_rip = next_rip;
		m_state->envelope_end = envelope_end;

		const bool leaves_page =
		    next_rip < m_state->page_guest_begin || next_rip >= page_end;
		const bool back_edge = next_rip <= block.rip;
		if (leaves_page || back_edge ||
		    m_state->segment_count == REGION_V2_MAX_SEGMENTS ||
		    m_state->operation_count == REGION_V2_MAX_OPERATIONS)
		{
			m_state->sealed = true;
		}
	}

private:
	LUMEN_REGION_V2_NOINLINE void Publish()
	{
		if (m_state == nullptr || m_state->context == nullptr ||
		    m_state->segment_count < 2 || m_state->operation_count < 2 ||
		    m_state->envelope_end <= m_state->start_rip)
		{
			return;
		}

		const uint64_t envelope_size =
		    m_state->envelope_end - m_state->start_rip;
		if (envelope_size > REGION_V2_MAX_ENVELOPE_BYTES) return;
		for (uint8_t index = 0; index < m_state->segment_count; index++)
		{
			const auto& recorded = m_state->blocks[index];
			if (recorded.block == nullptr || !RegionV2BlockEligible(*recorded.block) ||
			    recorded.block->rip != recorded.rip ||
			    recorded.block->byte_count != recorded.byte_count ||
			    recorded.block->op_count != recorded.operation_count)
			{
				return;
			}
		}

		if (m_thread_state == nullptr) return;
			auto* cache = RegionV2Cache(*m_thread_state);
			if (cache == nullptr) return;
			auto& trace = cache[RegionV2CacheIndex(m_state->start_rip)];
			trace.valid = false;
			trace.start_rip = m_state->start_rip;
			trace.page_guest_begin = m_state->page_guest_begin;
			trace.mapping_identity = 0;
			trace.publication_generation = 0;
			trace.segment_count = 0;
			trace.operation_count = 0;
			trace.envelope_size = static_cast<uint8_t>(envelope_size);
			RegionV2CodeReadLease lease;
			if (!lease.Acquire(*m_state->context, trace.page_guest_begin,
			                   trace.start_rip, trace.envelope_size))
			{
				LUMEN_REGION_V2_STAT_INCREMENT(lease_rejections);
				#if LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2_REPORT
				if (m_thread_state->report_requested)
				{
					ReportRegionV2LeaseRejection(
					    trace.start_rip, trace.envelope_size);
				}
				#endif
				return;
			}
			const auto* code = lease.Code();
			trace.mapping_identity = lease.MappingIdentity();
			trace.publication_generation = lease.MutationGeneration();
			if (trace.mapping_identity == 0) return;
			std::memcpy(trace.code, code, trace.envelope_size);

		for (uint8_t index = 0; index < m_state->segment_count; index++)
		{
			const auto& recorded = m_state->blocks[index];
			const auto& block = *recorded.block;
			const uint64_t code_offset = block.rip - trace.start_rip;
			if (block.byte_count > trace.envelope_size ||
			    code_offset > trace.envelope_size - block.byte_count ||
			    std::memcmp(
			        trace.code + code_offset, block.bytes,
			        block.byte_count) != 0)
			{
				return;
			}
			auto& segment = trace.segments[trace.segment_count++];
			segment.rip = block.rip;
			segment.expected_next_rip = recorded.expected_next_rip;
			segment.code_offset = static_cast<uint8_t>(code_offset);
			segment.byte_count = block.byte_count;
			segment.operation_begin = trace.operation_count;
			segment.operation_count = block.op_count;
			std::memcpy(
			    trace.operations + trace.operation_count, block.ops,
			    static_cast<size_t>(block.op_count) * sizeof(FastOp));
			trace.operation_count = static_cast<uint8_t>(
			    trace.operation_count + block.op_count);
		}

			if (std::memcmp(code, trace.code, trace.envelope_size) != 0)
		{
			return;
		}
		trace.valid = true;
		LUMEN_REGION_V2_STAT_INCREMENT(builds);
		#if LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2_REPORT
		if (m_thread_state->report_requested) ReportRegionV2Build(trace);
		#endif
	}

	RegionV2ThreadState* m_thread_state = nullptr;
	RegionV2RecordingState* m_state = nullptr;
	uint8_t m_slot = 0;
};

constexpr uint32_t REGION_V2_CORE_RETIRED_MASK = UINT32_C(0xff);
constexpr uint32_t REGION_V2_CORE_EXECUTED = UINT32_C(1) << 8u;
constexpr uint32_t REGION_V2_CORE_SIDE_EXIT = UINT32_C(1) << 9u;
constexpr uint32_t REGION_V2_CORE_INVALID_TRACE = UINT32_C(1) << 10u;
constexpr uint32_t REGION_V2_CORE_SEGMENT_SIGNATURE_MISS = UINT32_C(1) << 11u;

struct RegionV2CoreOutput
{
	uint64_t rax;
	uint64_t rcx;
	uint64_t rdx;
	uint64_t rbx;
	uint64_t rip;
	uint64_t rflags;
	uint16_t dirty_gprs;
	bool rflags_dirty;
};

// Keep the entire regional transaction in one leaf.  The caller performs the
// two whole-envelope memcmp calls; this core keeps the private guest frame in
// host registers across every former FastBlock boundary and materializes it
// exactly once, after a successful exit.
LUMEN_REGION_V2_NOINLINE uint32_t ExecuteRegionV2Core(
	const RegionV2Trace& trace, const uint8_t* leased_code,
	const X64State& state,
	RegionV2CoreOutput* output)
{
	X64RegionV2::Frame frame;
	frame.rax = state.gpr[0];
	frame.rcx = state.gpr[1];
	frame.rdx = state.gpr[2];
	frame.rbx = state.gpr[3];
	frame.rip = state.rip;
	frame.rflags = state.rflags;
	uint8_t retired = 0;
	bool side_exit = false;
	for (uint8_t segment_index = 0;
	     segment_index < trace.segment_count; segment_index++)
	{
		if (state.execution_cancel_requested)
		{
			if (retired == 0) return 0;
			side_exit = true;
			break;
		}
		const auto& segment = trace.segments[segment_index];
		if (segment.byte_count == 0 ||
		    segment.byte_count > trace.envelope_size ||
		    segment.code_offset > trace.envelope_size - segment.byte_count ||
		    frame.rip != segment.rip || segment.operation_count == 0 ||
		    static_cast<size_t>(segment.operation_begin) +
		            segment.operation_count >
		        trace.operation_count)
		{
			return REGION_V2_CORE_INVALID_TRACE;
		}
		if (segment_index != 0)
		{
#if defined(LUMEN_CPU_X64_TEST)
			// Deterministic seam for mutation of a future segment after the
			// envelope precheck but before that segment begins.
			if (g_region_v2_segment_validation_hook_for_test != nullptr)
			{
				const auto hook =
				    g_region_v2_segment_validation_hook_for_test;
				g_region_v2_segment_validation_hook_for_test = nullptr;
				hook();
			}
#endif
			if (leased_code == nullptr || !RegionV2SegmentBytesMatch(
			        leased_code + segment.code_offset,
			        trace.code + segment.code_offset, segment.byte_count))
			{
				return REGION_V2_CORE_INVALID_TRACE |
				       REGION_V2_CORE_SEGMENT_SIGNATURE_MISS;
			}
		}
		bool control_transfer = false;
		for (uint8_t operation_index = 0;
		     operation_index < segment.operation_count; operation_index++)
		{
			const auto& operation = trace.operations[
			    segment.operation_begin + operation_index];
			const auto result = X64RegionV2::Execute(frame, operation);
			if (result == X64RegionV2::OpResult::Unsupported)
				return REGION_V2_CORE_INVALID_TRACE;
			retired++;
			if (result == X64RegionV2::OpResult::ControlTransfer)
			{
				if (operation_index + 1 != segment.operation_count)
					return REGION_V2_CORE_INVALID_TRACE;
				control_transfer = true;
			}
		}
		if (!control_transfer)
		{
			const auto& last = trace.operations[
			    segment.operation_begin + segment.operation_count - 1u];
			frame.rip = last.rip + last.length;
		}
		if (frame.rip != segment.expected_next_rip)
		{
			side_exit = true;
			break;
		}
		if (segment_index + 1 < trace.segment_count &&
		    frame.rip != trace.segments[segment_index + 1].rip)
		{
			side_exit = true;
			break;
		}
	}

	output->rax = frame.rax;
	output->rcx = frame.rcx;
	output->rdx = frame.rdx;
	output->rbx = frame.rbx;
	output->rip = frame.rip;
	output->rflags = frame.rflags;
	output->dirty_gprs = frame.dirty_gprs;
	output->rflags_dirty = frame.rflags_dirty;
	return REGION_V2_CORE_EXECUTED |
	       (side_exit ? REGION_V2_CORE_SIDE_EXIT : 0) | retired;
}

LUMEN_REGION_V2_NOINLINE bool TryExecuteRegionV2(
	RegionV2ThreadState& thread_state, X64ThreadContext& context,
	X64State* state, size_t remaining_steps, size_t* executed)
{
	if (state == nullptr || executed == nullptr ||
	    state->execution_cancel_requested ||
	    state->host_call_stack_transaction.active)
	{
		return false;
	}
	LUMEN_REGION_V2_STAT_INCREMENT(lookups);
	if (thread_state.cache == nullptr) return false;
	auto& trace = thread_state.cache[RegionV2CacheIndex(state->rip)];
	if (!trace.valid || trace.start_rip != state->rip ||
	    trace.segment_count < 2 ||
	    trace.segment_count > REGION_V2_MAX_SEGMENTS ||
	    trace.operation_count == 0 ||
	    trace.operation_count > REGION_V2_MAX_OPERATIONS ||
	    trace.envelope_size == 0 ||
	    remaining_steps < trace.operation_count)
	{
		return false;
	}

	RegionV2CodeReadLease lease;
	if (!lease.Acquire(context, trace.page_guest_begin, trace.start_rip,
	                   trace.envelope_size))
	{
		LUMEN_REGION_V2_STAT_INCREMENT(lease_rejections);
		#if LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2_REPORT
		if (thread_state.report_requested)
			ReportRegionV2LeaseRejection(trace.start_rip, trace.envelope_size);
		#endif
		return false;
	}
	const auto* pre_code = lease.Code();
	if (lease.MappingIdentity() != trace.mapping_identity)
	{
		trace.valid = false;
		LUMEN_REGION_V2_STAT_INCREMENT(lease_identity_misses);
		return false;
	}
	if (std::memcmp(pre_code, trace.code, trace.envelope_size) != 0)
	{
		trace.valid = false;
		LUMEN_REGION_V2_STAT_INCREMENT(pre_signature_misses);
		return false;
	}
	RegionV2CoreOutput frame;
	const uint32_t core_result = ExecuteRegionV2Core(
	    trace, pre_code, *state, &frame);
	if ((core_result & REGION_V2_CORE_INVALID_TRACE) != 0)
	{
		trace.valid = false;
		if ((core_result & REGION_V2_CORE_SEGMENT_SIGNATURE_MISS) != 0)
			LUMEN_REGION_V2_STAT_INCREMENT(segment_signature_misses);
		return false;
	}
	if ((core_result & REGION_V2_CORE_EXECUTED) == 0) return false;
	const uint8_t retired =
	    static_cast<uint8_t>(core_result & REGION_V2_CORE_RETIRED_MASK);
	const bool side_exit =
	    (core_result & REGION_V2_CORE_SIDE_EXIT) != 0;

#if defined(LUMEN_CPU_X64_TEST)
	// Deterministic test seam for the otherwise concurrent post-signature race.
	// It is one-shot and absent from every production object.
	if (g_region_v2_post_validation_hook_for_test != nullptr)
	{
		const auto hook = g_region_v2_post_validation_hook_for_test;
		g_region_v2_post_validation_hook_for_test = nullptr;
		hook();
	}
#endif

	if (std::memcmp(pre_code, trace.code, trace.envelope_size) != 0)
	{
		trace.valid = false;
		LUMEN_REGION_V2_STAT_INCREMENT(post_signature_rollbacks);
		return false;
	}

	if ((frame.dirty_gprs & (1u << 0u)) != 0) state->gpr[0] = frame.rax;
	if ((frame.dirty_gprs & (1u << 1u)) != 0) state->gpr[1] = frame.rcx;
	if ((frame.dirty_gprs & (1u << 2u)) != 0) state->gpr[2] = frame.rdx;
	if ((frame.dirty_gprs & (1u << 3u)) != 0) state->gpr[3] = frame.rbx;
	if (frame.rflags_dirty) state->rflags = frame.rflags;
	state->rip = frame.rip;
	*executed = retired;
	LUMEN_REGION_V2_STAT_INCREMENT(hits);
	LUMEN_REGION_V2_STAT_INCREMENT(executions);
	LUMEN_REGION_V2_STAT_ADD(retired_operations, retired);
	LUMEN_REGION_V2_STAT_INCREMENT(commits);
	#if LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2_REPORT
	if (thread_state.report_requested)
		ReportRegionV2Commit(trace, retired, lease.MutationGeneration());
	#endif
	if (side_exit) LUMEN_REGION_V2_STAT_INCREMENT(path_side_exits);
	return retired != 0;
}

#undef LUMEN_REGION_V2_STAT_INCREMENT
#undef LUMEN_REGION_V2_STAT_ADD
#undef LUMEN_REGION_V2_NOINLINE
#undef LUMEN_REGION_V2_ALWAYS_INLINE
