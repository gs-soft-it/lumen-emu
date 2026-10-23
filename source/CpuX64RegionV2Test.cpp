#include "CpuX64.h"
#include "Lumen/Core/VirtualMemory.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

using Lumen::Cpu::X64GetRegionEngineV2StatsForTest;
using Lumen::Cpu::X64Interpreter;
using Lumen::Cpu::X64RegionEngineV2EnvironmentValueEnabledForTest;
using Lumen::Cpu::X64RegionEngineV2StatsForTest;
using Lumen::Cpu::X64ResetFastBlockCacheForTest;
using Lumen::Cpu::X64ResetRegionEngineV2ForTest;
using Lumen::Cpu::X64RunResult;
using Lumen::Cpu::X64SetRegionEngineV2PostValidationHookForTest;
using Lumen::Cpu::X64SetRegionEngineV2SegmentValidationHookForTest;
using Lumen::Cpu::X64SetRegionEngineV2UnmanagedCodeLeaseBypassForTest;
using Lumen::Cpu::X64State;
using Lumen::Cpu::X64ValidateRegionEngineV2ScalarParityForTest;
namespace VirtualMemory = Lumen::Core::VirtualMemory;

constexpr const char* REGION_ENGINE_ENVIRONMENT =
    "LUMEN_X64_A64_REGION_ENGINE_V2";

bool RegionStatsInactive(const X64RegionEngineV2StatsForTest& stats);

void SetEnvironment(const char* name, const char* value)
{
#if defined(_WIN32)
	_putenv_s(name, value);
#else
	setenv(name, value, 1);
#endif
}

bool TestExactOneSelector()
{
	struct TestCase
	{
		const char* value;
		bool        expected;
	};
	constexpr std::array<TestCase, 9> cases {{
	    {nullptr, false},
	    {"", false},
	    {"0", false},
	    {"1", true},
	    {"01", false},
	    {"+1", false},
	    {"true", false},
	    {"1 ", false},
	    {" 1", false},
	}};
	for (const auto& test: cases)
	{
		if (X64RegionEngineV2EnvironmentValueEnabledForTest(test.value) !=
		    test.expected)
		{
			std::fprintf(stderr, "selector parser mismatch for %s\n",
			             test.value == nullptr ? "<null>" : test.value);
			return false;
		}
	}
	return true;
}

void ConfigureNoJitFastBlocks()
{
	SetEnvironment("LUMEN_X64_FAST_BLOCKS", "1");
	SetEnvironment("LUMEN_X64_FAST_BLOCK_CHAIN", "1");
	SetEnvironment("LUMEN_X64_FAST_BLOCK_EXTENSIONS", "1");
	SetEnvironment("LUMEN_X64_FAST_BLOCK_HOT_THRESHOLD", "2");
	SetEnvironment("LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION", "0");
	SetEnvironment("LUMEN_X64_A64_DYNAREC", "0");
}

// Three basic blocks joined by direct jumps. RET is the final barrier.
//
//   block 0: eax = 5;  jump block 1
//   block 1: eax += 7; jump block 2
//   block 2: eax *= 3; return
constexpr std::array<uint8_t, 4096> REGION_TRACE_TEMPLATE = {
    0xb8, 0x05, 0x00, 0x00, 0x00, // MOV eax, 5
    0xeb, 0x00,                   // JMP block 1
    0x83, 0xc0, 0x07,             // ADD eax, 7
    0xeb, 0x00,                   // JMP block 2
    0x6b, 0xc0, 0x03,             // IMUL eax, eax, 3
    0xc3,                         // RET
};
alignas(4096) std::array<uint8_t, 4096> REGION_TRACE = REGION_TRACE_TEMPLATE;

std::thread g_region_patch_worker;
std::atomic_bool g_region_patch_started {false};
std::atomic_bool g_region_patch_done {false};
std::atomic_bool g_region_patch_pending_observed {false};
uint64_t g_region_patch_address = 0;
uint64_t g_region_patch_value = 0;
bool g_region_patch_result = false;

void StartPatchWhileRegionLeaseHeld()
{
	g_region_patch_worker = std::thread([]
	{
		g_region_patch_started.store(true, std::memory_order_release);
		g_region_patch_result = VirtualMemory::PatchReplace(
		    g_region_patch_address, g_region_patch_value);
		g_region_patch_done.store(true, std::memory_order_release);
	});

	for (uint32_t attempt = 0; attempt < 1000000; attempt++)
	{
		if (!g_region_patch_started.load(std::memory_order_acquire))
		{
			std::this_thread::yield();
			continue;
		}
		VirtualMemory::CodeReadLease probe;
		if (!VirtualMemory::TryAcquireCodeReadLease(
		        g_region_patch_address, 1, &probe))
		{
			g_region_patch_pending_observed.store(
			    true, std::memory_order_release);
			return;
		}
		VirtualMemory::ReleaseCodeReadLease(&probe);
		std::this_thread::yield();
	}
}

X64RunResult RunRegionTraceWithLimit(
	size_t max_steps, bool cancelled, X64State* output)
{
	std::array<uint64_t, 4> stack {};
	X64State               state {};
	state.rip = reinterpret_cast<uint64_t>(REGION_TRACE.data());
	state.gpr[4] = reinterpret_cast<uint64_t>(stack.data());
	state.execution_cancel_requested = cancelled;
	const X64RunResult result = X64Interpreter {}.Run(&state, max_steps);
	if (output != nullptr) *output = state;
	return result;
}

bool RunRegionTrace(X64State* output, uint32_t expected_eax = 36u)
{
	const X64RunResult result = RunRegionTraceWithLimit(32, false, output);
	if (output == nullptr) return false;
	const auto& state = *output;
	return result == X64RunResult::Returned && state.rip == 0 &&
	       static_cast<uint32_t>(state.gpr[0]) == expected_eax;
}

bool RunTraceRepeatedly()
{
	for (uint8_t attempt = 0; attempt < 12; attempt++)
	{
		X64State state {};
		if (!RunRegionTrace(&state))
		{
			std::fprintf(stderr,
			             "region trace mismatch at attempt %u: rip=%llx eax=%08x\n",
			             static_cast<unsigned>(attempt),
			             static_cast<unsigned long long>(state.rip),
			             static_cast<unsigned>(state.gpr[0]));
			return false;
		}
	}
	return true;
}

bool TrainRegionTrace()
{
	REGION_TRACE = REGION_TRACE_TEMPLATE;
	X64ResetFastBlockCacheForTest();
	X64ResetRegionEngineV2ForTest();
	if (!RunTraceRepeatedly()) return false;
	const auto stats = X64GetRegionEngineV2StatsForTest();
	if (stats.executions == 0 || stats.commits == 0)
	{
		std::fprintf(stderr, "region trace training did not reach v2\n");
		return false;
	}
	return true;
}

alignas(4096) std::array<uint8_t, 4096> RESIDENT_REGISTER_TRACE {};

bool TestResidentRegisterCommitAndZeroExtension()
{
	// Two eligible blocks followed by RET.  Patching the MOV opcode and ADD
	// ModRM exercises each resident destination through the real regional
	// commit path, including x86-64's required zero-extension for 32-bit writes.
	constexpr std::array<uint8_t, 13> trace_template {{
	    0xb8, 0x05, 0x00, 0x00, 0x00, // MOV r32, 5
	    0xeb, 0x00,                   // JMP block 1
	    0x83, 0xc0, 0x07,             // ADD r32, 7
	    0xeb, 0x00,                   // JMP RET barrier
	    0xc3,                         // RET
	}};
	constexpr uint64_t high_bits = UINT64_C(0xdeadbeef00000000);
	for (uint8_t selected = 0; selected < 4; selected++)
	{
		RESIDENT_REGISTER_TRACE.fill(0);
		std::memcpy(RESIDENT_REGISTER_TRACE.data(), trace_template.data(),
		            trace_template.size());
		RESIDENT_REGISTER_TRACE[0] =
		    static_cast<uint8_t>(0xb8u + selected);
		RESIDENT_REGISTER_TRACE[8] =
		    static_cast<uint8_t>(0xc0u + selected);
		X64ResetFastBlockCacheForTest();
		X64ResetRegionEngineV2ForTest();
		for (uint8_t attempt = 0; attempt < 12; attempt++)
		{
			std::array<uint64_t, 4> stack {};
			X64State state {};
			for (uint8_t reg = 0; reg < 4; reg++)
				state.gpr[reg] = high_bits | (UINT64_C(0x100) + reg);
			state.rip = reinterpret_cast<uint64_t>(
			    RESIDENT_REGISTER_TRACE.data());
			state.gpr[4] = reinterpret_cast<uint64_t>(stack.data());
			const auto result = X64Interpreter {}.Run(&state, 16);
			if (result != X64RunResult::Returned || state.rip != 0 ||
			    state.gpr[selected] != 12u)
			{
				std::fprintf(
				    stderr,
				    "resident commit mismatch: reg=%u attempt=%u result=%u rip=%llx value=%llx\n",
				    static_cast<unsigned>(selected),
				    static_cast<unsigned>(attempt),
				    static_cast<unsigned>(result),
				    static_cast<unsigned long long>(state.rip),
				    static_cast<unsigned long long>(state.gpr[selected]));
				return false;
			}
			for (uint8_t reg = 0; reg < 4; reg++)
			{
				if (reg != selected &&
				    state.gpr[reg] !=
				        (high_bits | (UINT64_C(0x100) + reg)))
				{
					std::fprintf(stderr,
					             "resident commit clobbered reg=%u while writing reg=%u\n",
					             static_cast<unsigned>(reg),
					             static_cast<unsigned>(selected));
					return false;
				}
			}
		}
		const auto stats = X64GetRegionEngineV2StatsForTest();
		if (stats.executions == 0 || stats.commits == 0)
		{
			std::fprintf(stderr,
			             "resident commit trace did not execute for reg=%u\n",
			             static_cast<unsigned>(selected));
			return false;
		}
	}
	return true;
}

bool TestPreSignatureInvalidation()
{
	if (!TrainRegionTrace()) return false;
	const auto before = X64GetRegionEngineV2StatsForTest();
	REGION_TRACE[1] = 0x06; // MOV eax,5 -> MOV eax,6.
	X64State state {};
	const bool replayed_new_code = RunRegionTrace(&state, 39u);
	REGION_TRACE = REGION_TRACE_TEMPLATE;
	const auto after = X64GetRegionEngineV2StatsForTest();
	if (!replayed_new_code)
	{
		std::fprintf(stderr,
		             "pre-signature invalidation replay mismatch: rip=%llx eax=%08x\n",
		             static_cast<unsigned long long>(state.rip),
		             static_cast<unsigned>(state.gpr[0]));
		return false;
	}
	if (after.pre_signature_misses != before.pre_signature_misses + 1u ||
	    after.commits != before.commits)
	{
		std::fprintf(stderr,
		             "pre-signature mismatch did not reject the stale region\n");
		return false;
	}
	return true;
}

void MutateRegionTraceBeforePostValidation()
{
	// MOV eax,5 -> MOV eax,6.  The speculative frame must be discarded before
	// the canonical path observes and executes this new exact signature.
	REGION_TRACE[1] = 0x06;
}

bool TestPostSignatureRollback()
{
	if (!TrainRegionTrace()) return false;
	const auto before = X64GetRegionEngineV2StatsForTest();
	X64SetRegionEngineV2PostValidationHookForTest(
	    MutateRegionTraceBeforePostValidation);
	X64State state {};
	const bool replayed_new_code = RunRegionTrace(&state, 39u);
	REGION_TRACE = REGION_TRACE_TEMPLATE;
	if (!replayed_new_code)
	{
		std::fprintf(stderr,
		             "post-signature rollback replay mismatch: rip=%llx eax=%08x\n",
		             static_cast<unsigned long long>(state.rip),
		             static_cast<unsigned>(state.gpr[0]));
		return false;
	}
	const auto after = X64GetRegionEngineV2StatsForTest();
	if (after.post_signature_rollbacks !=
	    before.post_signature_rollbacks + 1u)
	{
		std::fprintf(stderr, "post-signature rollback was not recorded\n");
		return false;
	}
	return true;
}

void MutateFutureSegmentBeforeValidation()
{
	// ADD eax,7 -> ADD eax,8 in region segment 1. Segment 0 has already run
	// speculatively, but neither segment may commit under the stale identity.
	REGION_TRACE[9] = 0x08;
}

bool TestFutureSegmentSignatureInvalidation()
{
	if (!TrainRegionTrace()) return false;
	const auto before = X64GetRegionEngineV2StatsForTest();
	X64SetRegionEngineV2SegmentValidationHookForTest(
	    MutateFutureSegmentBeforeValidation);
	X64State state {};
	const bool replayed_new_code = RunRegionTrace(&state, 39u);
	REGION_TRACE = REGION_TRACE_TEMPLATE;
	const auto after = X64GetRegionEngineV2StatsForTest();
	if (!replayed_new_code)
	{
		std::fprintf(stderr,
		             "future-segment invalidation replay mismatch: rip=%llx eax=%08x\n",
		             static_cast<unsigned long long>(state.rip),
		             static_cast<unsigned>(state.gpr[0]));
		return false;
	}
	if (after.segment_signature_misses !=
	        before.segment_signature_misses + 1u ||
	    after.executions != before.executions ||
	    after.commits != before.commits)
	{
		std::fprintf(stderr,
		             "future segment executed under a stale regional identity\n");
		return false;
	}
	return true;
}

bool TestBudgetAndCancelGates()
{
	if (!TrainRegionTrace()) return false;
	const auto before_budget = X64GetRegionEngineV2StatsForTest();
	X64State budget_state {};
	const auto budget_result =
	    RunRegionTraceWithLimit(3, false, &budget_state);
	const auto after_budget = X64GetRegionEngineV2StatsForTest();
	const uint64_t budget_rip =
	    reinterpret_cast<uint64_t>(REGION_TRACE.data() + 10);
	if (budget_result != X64RunResult::StepLimit ||
	    budget_state.rip != budget_rip ||
	    static_cast<uint32_t>(budget_state.gpr[0]) != 12u ||
	    after_budget.lookups <= before_budget.lookups ||
	    after_budget.executions != before_budget.executions ||
	    after_budget.retired_operations != before_budget.retired_operations ||
	    after_budget.commits != before_budget.commits)
	{
		std::fprintf(stderr,
		             "budget gate mismatch: result=%u rip=%llx eax=%08x\n",
		             static_cast<unsigned>(budget_result),
		             static_cast<unsigned long long>(budget_state.rip),
		             static_cast<unsigned>(budget_state.gpr[0]));
		return false;
	}

	const auto before_cancel = X64GetRegionEngineV2StatsForTest();
	X64State cancel_state {};
	const auto cancel_result =
	    RunRegionTraceWithLimit(32, true, &cancel_state);
	const auto after_cancel = X64GetRegionEngineV2StatsForTest();
	if (cancel_result != X64RunResult::StepLimit ||
	    cancel_state.rip != reinterpret_cast<uint64_t>(REGION_TRACE.data()) ||
	    cancel_state.gpr[0] != 0 ||
	    !cancel_state.execution_cancel_requested ||
	    after_cancel.lookups != before_cancel.lookups ||
	    after_cancel.executions != before_cancel.executions ||
	    after_cancel.retired_operations != before_cancel.retired_operations ||
	    after_cancel.commits != before_cancel.commits)
	{
		std::fprintf(stderr,
		             "cancel gate mismatch: result=%u rip=%llx eax=%08x\n",
		             static_cast<unsigned>(cancel_result),
		             static_cast<unsigned long long>(cancel_state.rip),
		             static_cast<unsigned>(cancel_state.gpr[0]));
		return false;
	}
	return true;
}

bool TestChainLimitGate()
{
	if (!TrainRegionTrace()) return false;
	const auto before = X64GetRegionEngineV2StatsForTest();
	SetEnvironment("LUMEN_X64_FAST_BLOCK_CHAIN", "0");
	X64State state {};
	const bool canonical_replay = RunRegionTrace(&state);
	const auto after = X64GetRegionEngineV2StatsForTest();
	SetEnvironment("LUMEN_X64_FAST_BLOCK_CHAIN", "1");
	if (!canonical_replay || after.lookups != before.lookups ||
	    after.hits != before.hits || after.builds != before.builds ||
	    after.executions != before.executions ||
	    after.commits != before.commits || after.barriers != before.barriers)
	{
		std::fprintf(stderr,
		             "chain-off Run entered or learned a multi-block region\n");
		return false;
	}

	SetEnvironment("LUMEN_X64_FAST_BLOCK_PROFILE_INTERVAL", "1");
	X64State profiled_state {};
	const bool profiled_replay = RunRegionTrace(&profiled_state);
	const auto after_profile = X64GetRegionEngineV2StatsForTest();
	SetEnvironment("LUMEN_X64_FAST_BLOCK_PROFILE_INTERVAL", "0");
	if (!profiled_replay || after_profile.lookups != after.lookups ||
	    after_profile.hits != after.hits ||
	    after_profile.builds != after.builds ||
	    after_profile.executions != after.executions ||
	    after_profile.commits != after.commits ||
	    after_profile.barriers != after.barriers)
	{
		std::fprintf(stderr,
		             "FastBlock profiling Run entered or learned a region\n");
		return false;
	}
	return true;
}

// The successor starts at offset 5, inside the [0, 10) byte envelope of the
// first block.  It is a valid canonical loop, but not a valid region trace:
// recording it would associate overlapping byte identities with two FastOp
// segments.
alignas(4096) constexpr std::array<uint8_t, 4096> OVERLAP_TRACE = {
	0xb8, 0x01, 0x00, 0x00, 0x00, // MOV eax, 1
	0x83, 0xc0, 0x02,             // inside successor: ADD eax, 2
	0xeb, 0xfb,                   // JMP offset 5
};

X64RunResult RunOverlapTrace(bool fast_blocks, X64State* output)
{
	SetEnvironment("LUMEN_X64_FAST_BLOCKS", fast_blocks ? "1" : "0");
	std::array<uint64_t, 4> stack {};
	X64State state {};
	state.rip = reinterpret_cast<uint64_t>(OVERLAP_TRACE.data());
	state.gpr[4] = reinterpret_cast<uint64_t>(stack.data());
	const auto result = X64Interpreter {}.Run(&state, 9);
	if (output != nullptr) *output = state;
	return result;
}

bool TestOverlappingSuccessorRejected()
{
	X64ResetFastBlockCacheForTest();
	X64ResetRegionEngineV2ForTest();
	X64State canonical {};
	const auto canonical_result = RunOverlapTrace(false, &canonical);
	if (canonical_result != X64RunResult::StepLimit ||
	    canonical.rip != reinterpret_cast<uint64_t>(OVERLAP_TRACE.data() + 5) ||
	    static_cast<uint32_t>(canonical.gpr[0]) != 9u)
	{
		std::fprintf(stderr,
		             "overlap canonical baseline mismatch: result=%u rip=%llx eax=%08x\n",
		             static_cast<unsigned>(canonical_result),
		             static_cast<unsigned long long>(canonical.rip),
		             static_cast<unsigned>(canonical.gpr[0]));
		return false;
	}

	SetEnvironment("LUMEN_X64_FAST_BLOCKS", "1");
	X64ResetFastBlockCacheForTest();
	X64ResetRegionEngineV2ForTest();
	for (uint8_t attempt = 0; attempt < 12; attempt++)
	{
		X64State fast {};
		const auto result = RunOverlapTrace(true, &fast);
		if (result != canonical_result || fast.rip != canonical.rip ||
		    fast.gpr[0] != canonical.gpr[0] || fast.rflags != canonical.rflags)
		{
			std::fprintf(stderr,
			             "overlap fast path mismatch at attempt %u: result=%u rip=%llx eax=%08x\n",
			             static_cast<unsigned>(attempt),
			             static_cast<unsigned>(result),
			             static_cast<unsigned long long>(fast.rip),
			             static_cast<unsigned>(fast.gpr[0]));
			return false;
		}
	}
	const auto stats = X64GetRegionEngineV2StatsForTest();
	if (stats.lookups == 0 || stats.barriers == 0 || stats.builds != 0 ||
	    stats.hits != 0 || stats.executions != 0 || stats.commits != 0)
	{
		std::fprintf(stderr,
		             "overlapping successor was not rejected before publication\n");
		return false;
	}
	return true;
}

// The final block writes BL into the first block's MOV immediate.
// Diagnostic warm-up keeps v2 disabled while establishing canonical FastBlock
// identities.  Changing only the input register then causes the first normal
// run to mutate block 0 after it was recorded but before region publication,
// without invalidating the already-hot store block.
constexpr std::array<uint8_t, 4096> STALE_PAIR_TRACE_TEMPLATE = {
	0xb8, 0x05, 0x00, 0x00, 0x00, // MOV eax, 5
	0xeb, 0x00,                   // JMP block 1
	0x83, 0xc0, 0x07,             // ADD eax, 7
	0xeb, 0x00,                   // JMP block 2
	0x88, 0x1d, 0xef, 0xff, 0xff, 0xff, // MOV byte [RIP-17], bl
	0xc3,                         // RET
};
alignas(4096) std::array<uint8_t, 4096> STALE_PAIR_TRACE =
	STALE_PAIR_TRACE_TEMPLATE;

bool RunStalePairTrace(
	bool diagnostic, uint8_t stored_byte, uint32_t expected_eax)
{
	std::array<uint64_t, 4> stack {};
	X64State state {};
	state.rip = reinterpret_cast<uint64_t>(STALE_PAIR_TRACE.data());
	state.gpr[4] = reinterpret_cast<uint64_t>(stack.data());
	state.gpr[3] = stored_byte;
	const auto result =
	    X64Interpreter(nullptr, nullptr, nullptr, nullptr, diagnostic)
	        .Run(&state, 32);
	const bool valid = result == X64RunResult::Returned && state.rip == 0 &&
	                   static_cast<uint32_t>(state.gpr[0]) == expected_eax;
	if (!valid)
	{
		std::fprintf(stderr,
		             "stale-pair run mismatch: diagnostic=%u result=%u rip=%llx eax=%08x code=%02x stored=%02x\n",
		             diagnostic ? 1u : 0u, static_cast<unsigned>(result),
		             static_cast<unsigned long long>(state.rip),
		             static_cast<unsigned>(state.gpr[0]),
		             static_cast<unsigned>(STALE_PAIR_TRACE[1]),
		             static_cast<unsigned>(stored_byte));
	}
	return valid;
}

bool TestStaleFastOpsCannotPairWithNewSignature()
{
	constexpr size_t first_immediate = 1;
	STALE_PAIR_TRACE = STALE_PAIR_TRACE_TEMPLATE;
	SetEnvironment("LUMEN_X64_FAST_BLOCKS", "1");
	X64ResetFastBlockCacheForTest();
	X64ResetRegionEngineV2ForTest();
	for (uint8_t attempt = 0; attempt < 6; attempt++)
	{
		if (!RunStalePairTrace(true, 5u, 12u))
		{
			std::fprintf(stderr, "stale-pair diagnostic warm-up mismatch\n");
			return false;
		}
	}
	if (!RegionStatsInactive(X64GetRegionEngineV2StatsForTest()))
	{
		std::fprintf(stderr, "diagnostic warm-up entered region v2\n");
		return false;
	}

	// Preserve every decoded block identity, but make the later store write a
	// new value into block 0 while the first regional candidate is being
	// learned. Depending on successor-link warmth, the old candidate may be
	// published just before the store; it must never execute afterward.
	X64ResetRegionEngineV2ForTest();
	const bool mutation_run = RunStalePairTrace(false, 6u, 12u);
	const auto after_mutation = X64GetRegionEngineV2StatsForTest();
	if (!mutation_run || STALE_PAIR_TRACE[first_immediate] != 0x06 ||
	    after_mutation.executions != 0)
	{
		std::fprintf(stderr,
		             "code changed between observation/publication was not rejected: run=%u code=%02x builds=%llu executions=%llu barriers=%llu\n",
		             mutation_run ? 1u : 0u,
		             static_cast<unsigned>(STALE_PAIR_TRACE[first_immediate]),
		             static_cast<unsigned long long>(after_mutation.builds),
		             static_cast<unsigned long long>(after_mutation.executions),
		             static_cast<unsigned long long>(after_mutation.barriers));
		STALE_PAIR_TRACE = STALE_PAIR_TRACE_TEMPLATE;
		return false;
	}

	// The next pass must reject any already-published old signature, decode
	// MOV eax,6 and may publish only that new identity.
	if (!RunStalePairTrace(false, 6u, 13u))
	{
		STALE_PAIR_TRACE = STALE_PAIR_TRACE_TEMPLATE;
		std::fprintf(stderr, "new signature canonical replay mismatch\n");
		return false;
	}
	const auto rebuilt = X64GetRegionEngineV2StatsForTest();
	if ((after_mutation.builds != 0 &&
	     rebuilt.pre_signature_misses <= after_mutation.pre_signature_misses) ||
	    rebuilt.builds == 0 || !RunStalePairTrace(false, 6u, 13u))
	{
		STALE_PAIR_TRACE = STALE_PAIR_TRACE_TEMPLATE;
		std::fprintf(stderr, "new signature region was not rebuilt coherently\n");
		return false;
	}
	const auto executed = X64GetRegionEngineV2StatsForTest();
	STALE_PAIR_TRACE = STALE_PAIR_TRACE_TEMPLATE;
	if (executed.executions <= rebuilt.executions ||
	    executed.commits <= rebuilt.commits)
	{
		std::fprintf(stderr,
		             "rebuilt signature did not execute its new FastOps\n");
		return false;
	}
	return true;
}

// The trained edge is the fall-through path.  Changing EBX exercises a
// committed path side exit at the first region segment.
alignas(4096) constexpr std::array<uint8_t, 4096> SIDE_EXIT_TRACE = {
	0x83, 0xfb, 0x00,             // CMP ebx, 0
	0x75, 0x07,                   // JNE alternate
	0xb8, 0x0a, 0x00, 0x00, 0x00, // MOV eax, 10
	0xeb, 0x07,                   // JMP join
	0xb8, 0x14, 0x00, 0x00, 0x00, // alternate: MOV eax, 20
	0xeb, 0x00,                   // JMP join
	0x83, 0xc0, 0x01,             // join: ADD eax, 1
	0x6b, 0xc0, 0x03,             // IMUL eax, eax, 3
	0xc3,                         // RET
};

bool RunSideExitTrace(uint32_t ebx, uint32_t expected_eax)
{
	std::array<uint64_t, 4> stack {};
	X64State state {};
	state.rip = reinterpret_cast<uint64_t>(SIDE_EXIT_TRACE.data());
	state.gpr[4] = reinterpret_cast<uint64_t>(stack.data());
	state.gpr[3] = ebx;
	const auto result = X64Interpreter {}.Run(&state, 32);
	return result == X64RunResult::Returned && state.rip == 0 &&
	       static_cast<uint32_t>(state.gpr[0]) == expected_eax;
}

bool TestPathSideExit()
{
	X64ResetFastBlockCacheForTest();
	X64ResetRegionEngineV2ForTest();
	for (uint8_t attempt = 0; attempt < 12; attempt++)
	{
		if (!RunSideExitTrace(0, 33u)) return false;
	}
	const auto trained = X64GetRegionEngineV2StatsForTest();
	if (trained.executions == 0) return false;
	if (!RunSideExitTrace(1, 63u))
	{
		std::fprintf(stderr, "region path side-exit state mismatch\n");
		return false;
	}
	const auto divergent = X64GetRegionEngineV2StatsForTest();
	if (divergent.path_side_exits <= trained.path_side_exits)
	{
		std::fprintf(stderr, "region path side exit was not recorded\n");
		return false;
	}
	return true;
}

void PrintStats(const X64RegionEngineV2StatsForTest& stats)
{
	std::printf(
	    "region-v2 stats: lookups=%llu hits=%llu builds=%llu executions=%llu "
	    "retired=%llu commits=%llu side_exits=%llu pre_misses=%llu "
	    "segment_misses=%llu rollbacks=%llu lease_rejections=%llu "
	    "lease_identity_misses=%llu barriers=%llu\n",
	    static_cast<unsigned long long>(stats.lookups),
	    static_cast<unsigned long long>(stats.hits),
	    static_cast<unsigned long long>(stats.builds),
	    static_cast<unsigned long long>(stats.executions),
	    static_cast<unsigned long long>(stats.retired_operations),
	    static_cast<unsigned long long>(stats.commits),
	    static_cast<unsigned long long>(stats.path_side_exits),
	    static_cast<unsigned long long>(stats.pre_signature_misses),
	    static_cast<unsigned long long>(stats.segment_signature_misses),
	    static_cast<unsigned long long>(stats.post_signature_rollbacks),
	    static_cast<unsigned long long>(stats.lease_rejections),
	    static_cast<unsigned long long>(stats.lease_identity_misses),
	    static_cast<unsigned long long>(stats.barriers));
}

bool RegionStatsInactive(const X64RegionEngineV2StatsForTest& stats)
{
	return stats.lookups == 0 && stats.hits == 0 && stats.builds == 0 &&
	       stats.executions == 0 && stats.retired_operations == 0 &&
	       stats.commits == 0 && stats.path_side_exits == 0 &&
	       stats.pre_signature_misses == 0 &&
	       stats.segment_signature_misses == 0 &&
	       stats.post_signature_rollbacks == 0 &&
	       stats.lease_rejections == 0 &&
	       stats.lease_identity_misses == 0 && stats.barriers == 0;
}

bool TestEnabledSelectorLatch()
{
	SetEnvironment(REGION_ENGINE_ENVIRONMENT, "0");
	X64ResetFastBlockCacheForTest();
	X64ResetRegionEngineV2ForTest();
	const bool trace_ok = RunTraceRepeatedly();
	const auto stats = X64GetRegionEngineV2StatsForTest();
	SetEnvironment(REGION_ENGINE_ENVIRONMENT, "1");
	if (!trace_ok || stats.executions == 0 || stats.commits == 0)
	{
		std::fprintf(stderr,
		             "enabled selector was not latched for the process\n");
		return false;
	}
	return true;
}

bool TestManagedCodeLeaseRegionTrace()
{
	const uint64_t mapping = VirtualMemory::Alloc(
	    0, REGION_TRACE_TEMPLATE.size(),
	    VirtualMemory::Mode::ExecuteReadWrite);
	if (mapping == 0) return false;
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(mapping)),
	            REGION_TRACE_TEMPLATE.data(), REGION_TRACE_TEMPLATE.size());
	VirtualMemory::Mode old_mode = VirtualMemory::Mode::NoAccess;
	if (!VirtualMemory::Protect(
	        mapping, REGION_TRACE_TEMPLATE.size(),
	        VirtualMemory::Mode::ExecuteRead, &old_mode))
	{
		VirtualMemory::Free(mapping);
		return false;
	}

	X64ResetFastBlockCacheForTest();
	X64ResetRegionEngineV2ForTest();
	bool run_ok = true;
	for (uint8_t attempt = 0; attempt < 12; attempt++)
	{
		std::array<uint64_t, 4> stack {};
		X64State state {};
		state.rip = mapping;
		state.gpr[4] = reinterpret_cast<uint64_t>(stack.data());
		const auto result = X64Interpreter {}.Run(&state, 32);
		run_ok = run_ok && result == X64RunResult::Returned && state.rip == 0 &&
		         static_cast<uint32_t>(state.gpr[0]) == 36u;
	}
	const auto stats = X64GetRegionEngineV2StatsForTest();
	const bool lease_path = stats.builds != 0 && stats.executions != 0 &&
	                        stats.commits != 0 &&
	                        stats.lease_rejections == 0 &&
	                        stats.lease_identity_misses == 0;
	const bool freed = VirtualMemory::Free(mapping);
	return run_ok && lease_path && freed;
}

bool RunManagedTraceOnce(uint64_t mapping, uint32_t expected_eax)
{
	std::array<uint64_t, 4> stack {};
	X64State state {};
	state.rip = mapping;
	state.gpr[4] = reinterpret_cast<uint64_t>(stack.data());
	const auto result = X64Interpreter {}.Run(&state, 32);
	return result == X64RunResult::Returned && state.rip == 0 &&
	       static_cast<uint32_t>(state.gpr[0]) == expected_eax;
}

uint64_t AllocateManagedRegionTrace()
{
	const uint64_t mapping = VirtualMemory::Alloc(
	    0, REGION_TRACE_TEMPLATE.size(),
	    VirtualMemory::Mode::ExecuteReadWrite);
	if (mapping == 0) return 0;
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(mapping)),
	            REGION_TRACE_TEMPLATE.data(), REGION_TRACE_TEMPLATE.size());
	if (!VirtualMemory::Protect(
	        mapping, REGION_TRACE_TEMPLATE.size(),
	        VirtualMemory::Mode::ExecuteRead))
	{
		VirtualMemory::Free(mapping);
		return 0;
	}
	return mapping;
}

bool TrainManagedRegionTrace(uint64_t mapping, uint32_t expected_eax)
{
	for (uint8_t attempt = 0; attempt < 12; attempt++)
	{
		if (!RunManagedTraceOnce(mapping, expected_eax)) return false;
	}
	const auto stats = X64GetRegionEngineV2StatsForTest();
	return stats.builds != 0 && stats.executions != 0 && stats.commits != 0 &&
	       stats.lease_rejections == 0 && stats.lease_identity_misses == 0;
}

bool TestManagedWriterWaitsThroughRegionCommit()
{
	const uint64_t mapping = AllocateManagedRegionTrace();
	if (mapping == 0) return false;
	X64ResetFastBlockCacheForTest();
	X64ResetRegionEngineV2ForTest();
	if (!TrainManagedRegionTrace(mapping, 36u))
	{
		VirtualMemory::Free(mapping);
		return false;
	}
	const auto trained = X64GetRegionEngineV2StatsForTest();

	std::array<uint8_t, sizeof(uint64_t)> patched_prefix {};
	std::memcpy(patched_prefix.data(), REGION_TRACE_TEMPLATE.data(),
	            patched_prefix.size());
	patched_prefix[1] = 6;
	g_region_patch_address = mapping;
	std::memcpy(&g_region_patch_value, patched_prefix.data(),
	            sizeof(g_region_patch_value));
	g_region_patch_started.store(false, std::memory_order_relaxed);
	g_region_patch_done.store(false, std::memory_order_relaxed);
	g_region_patch_pending_observed.store(false, std::memory_order_relaxed);
	g_region_patch_result = false;
	X64SetRegionEngineV2PostValidationHookForTest(
	    &StartPatchWhileRegionLeaseHeld);
	const bool outer_committed_old_bytes = RunManagedTraceOnce(mapping, 36u);
	if (g_region_patch_worker.joinable()) g_region_patch_worker.join();
	X64SetRegionEngineV2PostValidationHookForTest(nullptr);
	const auto after_outer = X64GetRegionEngineV2StatsForTest();
	const bool next_run_observes_patch = RunManagedTraceOnce(mapping, 39u);
	const bool freed = VirtualMemory::Free(mapping);
	return outer_committed_old_bytes &&
	       g_region_patch_pending_observed.load(std::memory_order_acquire) &&
	       g_region_patch_done.load(std::memory_order_acquire) &&
	       g_region_patch_result && after_outer.commits > trained.commits &&
	       next_run_observes_patch && freed;
}

bool TestManagedRemapRejectsTraceIdentity()
{
	const uint64_t mapping = AllocateManagedRegionTrace();
	if (mapping == 0) return false;
	X64ResetFastBlockCacheForTest();
	X64ResetRegionEngineV2ForTest();
	if (!TrainManagedRegionTrace(mapping, 36u))
	{
		VirtualMemory::Free(mapping);
		return false;
	}
	const auto trained = X64GetRegionEngineV2StatsForTest();
	if (!VirtualMemory::Free(mapping) ||
	    !VirtualMemory::AllocFixed(
	        mapping, REGION_TRACE_TEMPLATE.size(),
	        VirtualMemory::Mode::ExecuteReadWrite))
	{
		return false;
	}
	std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(mapping)),
	            REGION_TRACE_TEMPLATE.data(), REGION_TRACE_TEMPLATE.size());
	if (!VirtualMemory::Protect(
	        mapping, REGION_TRACE_TEMPLATE.size(),
	        VirtualMemory::Mode::ExecuteRead))
	{
		VirtualMemory::Free(mapping);
		return false;
	}
	const bool run_ok = RunManagedTraceOnce(mapping, 36u);
	const auto remapped = X64GetRegionEngineV2StatsForTest();
	const bool freed = VirtualMemory::Free(mapping);
	return run_ok && remapped.lease_identity_misses >
	                     trained.lease_identity_misses &&
	       freed;
}

bool TestEnabledRegionTrace()
{
	const char* selector = std::getenv(REGION_ENGINE_ENVIRONMENT);
	if (!X64RegionEngineV2EnvironmentValueEnabledForTest(selector))
	{
		std::fprintf(stderr, "%s must be exactly 1 for the enabled test\n",
		             REGION_ENGINE_ENVIRONMENT);
		return false;
	}
	ConfigureNoJitFastBlocks();
	X64SetRegionEngineV2UnmanagedCodeLeaseBypassForTest(false);
	X64ResetFastBlockCacheForTest();
	X64ResetRegionEngineV2ForTest();
	if (!RunTraceRepeatedly()) return false;
	const auto unmanaged = X64GetRegionEngineV2StatsForTest();
	if (unmanaged.builds != 0 || unmanaged.executions != 0 ||
	    unmanaged.commits != 0 || unmanaged.lease_rejections == 0)
	{
		std::fprintf(stderr,
		             "unmanaged code was not rejected by the production lease gate\n");
		return false;
	}
	if (!TestManagedCodeLeaseRegionTrace())
	{
		std::fprintf(stderr,
		             "managed ExecuteRead trace did not train and hit through the real lease\n");
		return false;
	}
	if (!TestManagedWriterWaitsThroughRegionCommit())
	{
		std::fprintf(stderr,
		             "managed writer did not wait through the Region commit\n");
		return false;
	}
	if (!TestManagedRemapRejectsTraceIdentity())
	{
		std::fprintf(stderr,
		             "managed remap reused a stale Region trace identity\n");
		return false;
	}
	X64SetRegionEngineV2UnmanagedCodeLeaseBypassForTest(true);
	X64ResetFastBlockCacheForTest();
	X64ResetRegionEngineV2ForTest();
	if (!RunTraceRepeatedly()) return false;
	const auto stats = X64GetRegionEngineV2StatsForTest();
	PrintStats(stats);
	if (stats.executions == 0 || stats.commits == 0)
	{
		std::fprintf(stderr,
		             "enabled selector did not execute and commit a region\n");
		return false;
	}
	if (!X64ValidateRegionEngineV2ScalarParityForTest())
	{
		std::fprintf(stderr, "region v2 scalar parity gate failed\n");
		return false;
	}
	if (!TestResidentRegisterCommitAndZeroExtension()) return false;
	if (!TestEnabledSelectorLatch()) return false;
	if (!TestPreSignatureInvalidation()) return false;
	if (!TestPostSignatureRollback()) return false;
	if (!TestFutureSegmentSignatureInvalidation()) return false;
	if (!TestBudgetAndCancelGates()) return false;
	if (!TestChainLimitGate()) return false;
	if (!TestOverlappingSuccessorRejected()) return false;
	if (!TestStaleFastOpsCannotPairWithNewSignature()) return false;
	if (!TestPathSideExit()) return false;
	return true;
}

bool TestDisabledRegionTrace()
{
	const char* selector = std::getenv(REGION_ENGINE_ENVIRONMENT);
	if (X64RegionEngineV2EnvironmentValueEnabledForTest(selector))
	{
		std::fprintf(stderr, "%s must not be exactly 1 for the disabled test\n",
		             REGION_ENGINE_ENVIRONMENT);
		return false;
	}
	ConfigureNoJitFastBlocks();
	X64ResetFastBlockCacheForTest();
	X64ResetRegionEngineV2ForTest();
	if (!RunTraceRepeatedly()) return false;
	const auto stats = X64GetRegionEngineV2StatsForTest();
	PrintStats(stats);
	if (!RegionStatsInactive(stats))
	{
		std::fprintf(stderr, "disabled selector entered the region engine\n");
		return false;
	}

	// The request is a process contract: changing the environment after the
	// first dispatch must not turn a disabled engine on.
	SetEnvironment(REGION_ENGINE_ENVIRONMENT, "1");
	X64ResetFastBlockCacheForTest();
	X64ResetRegionEngineV2ForTest();
	if (!RunTraceRepeatedly()) return false;
	const auto latched = X64GetRegionEngineV2StatsForTest();
	if (!RegionStatsInactive(latched))
	{
		std::fprintf(stderr,
		             "disabled selector was not latched for the process\n");
		return false;
	}
	return true;
}

} // namespace

int main(int argc, char** argv)
{
	if (!TestExactOneSelector()) return 1;
	if (argc == 2 && std::strcmp(argv[1], "--selector-off-only") == 0)
		return TestDisabledRegionTrace() ? 0 : 1;
	if (argc != 1)
	{
		std::fprintf(stderr, "usage: %s [--selector-off-only]\n", argv[0]);
		return 2;
	}
	return TestEnabledRegionTrace() ? 0 : 1;
}
