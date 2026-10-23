#include "CpuA64Dynarec.h"
#include "CpuX64.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

#if defined(__ANDROID__) && defined(__aarch64__) && \
    defined(LUMEN_ANDROID_DYNAREC_POLICY_SELECTIVE_NODA)
#include <android/log.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && defined(__aarch64__) && TARGET_OS_OSX && !TARGET_OS_IPHONE
#define LUMEN_A64_DYNAREC_MACOS 1
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#else
#define LUMEN_A64_DYNAREC_MACOS 0
#endif

#if defined(__linux__) && defined(__aarch64__)
#define LUMEN_A64_DYNAREC_LINUX 1
#include <sys/mman.h>
#include <unistd.h>
#else
#define LUMEN_A64_DYNAREC_LINUX 0
#endif

#if defined(_WIN32) && defined(_M_ARM64)
#define LUMEN_A64_DYNAREC_WINDOWS 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#define LUMEN_A64_DYNAREC_WINDOWS 0
#endif

#if defined(__ANDROID__) && defined(__aarch64__) && \
    defined(LUMEN_ANDROID_DYNAREC_POLICY_SELECTIVE_NODA)
extern "C" void LumenAndroidNodaPcRangeTrap(void*);
extern "C" const char LumenAndroidNodaPcRangeTrapEnd[];
__asm__(
    ".pushsection .text\n"
    ".balign 4\n"
    ".hidden LumenAndroidNodaPcRangeTrap\n"
    ".hidden LumenAndroidNodaPcRangeTrapEnd\n"
    ".type LumenAndroidNodaPcRangeTrap,%function\n"
    "LumenAndroidNodaPcRangeTrap:\n"
    ".inst 0xd503245f\n" // BTI c when branch-target enforcement is active.
    ".inst 0x00000000\n" // Architecturally undefined: deterministic SIGILL.
    "ret\n"
    "LumenAndroidNodaPcRangeTrapEnd:\n"
    ".size LumenAndroidNodaPcRangeTrap,"
    "LumenAndroidNodaPcRangeTrapEnd-LumenAndroidNodaPcRangeTrap\n"
    ".popsection\n");
#endif

namespace Lumen::Cpu::A64Dynarec {

namespace {

constexpr uint32_t A64_BTI_CALL      = 0xd503245f;
constexpr uint32_t A64_BTI_JUMP_CALL = 0xd50324df;
constexpr uint32_t A64_MOVN_64   = 0x92800000;
constexpr uint32_t A64_MOVZ_64   = 0xd2800000;
constexpr uint32_t A64_MOVK_64   = 0xf2800000;
constexpr uint32_t A64_RET       = 0xd65f0000;
constexpr uint32_t A64_MOV_X9_X0 = 0xaa0003e9;
constexpr uint32_t A64_LDR_W     = 0xb9400000;
constexpr uint32_t A64_STR_W     = 0xb9000000;
constexpr size_t   A64_WORD_SIZE = sizeof(uint32_t);
constexpr uint64_t X64_FLAG_CF   = 1ULL << 0u;
constexpr uint64_t X64_FLAG_PF   = 1ULL << 2u;
constexpr uint64_t X64_FLAG_AF   = 1ULL << 4u;
constexpr uint64_t X64_FLAG_ZF   = 1ULL << 6u;
constexpr uint64_t X64_FLAG_SF   = 1ULL << 7u;
constexpr uint64_t X64_FLAG_DF   = 1ULL << 10u;
constexpr uint64_t X64_FLAG_OF   = 1ULL << 11u;
constexpr uint64_t X64_ARITHMETIC_FLAGS =
    X64_FLAG_CF | X64_FLAG_PF | X64_FLAG_AF | X64_FLAG_ZF | X64_FLAG_SF | X64_FLAG_OF;

static_assert(std::is_standard_layout<EntryContext>::value, "EntryContext must have a fixed standard layout");
static_assert(sizeof(ExitReason) == sizeof(uint32_t), "native exits store a 32-bit reason");
static_assert(static_cast<uint32_t>(ExitReason::Completed) == 0, "native code stores WZR for Completed");
static_assert(static_cast<uint32_t>(StepAction::Continue) == 0, "native code branches on non-zero StepAction");
static_assert((offsetof(EntryContext, remaining_steps) % sizeof(uint32_t)) == 0, "unaligned step budget");
static_assert((offsetof(EntryContext, executed_steps) % sizeof(uint32_t)) == 0, "unaligned step accounting");
static_assert((offsetof(EntryContext, exit_reason) % sizeof(uint32_t)) == 0, "unaligned exit reason");
static_assert((offsetof(EntryContext, direct_steps) % sizeof(uint32_t)) == 0, "unaligned direct-step accounting");
static_assert((offsetof(EntryContext, scalar_memory_regions) % sizeof(uint64_t)) == 0,
              "unaligned scalar-memory regions");
static_assert((offsetof(EntryContext, scalar_memory_host_address) % sizeof(uint64_t)) == 0,
              "unaligned scalar-memory host address");
static_assert((offsetof(EntryContext, scalar_memory_read_host_address) % sizeof(uint64_t)) == 0,
              "unaligned scalar-memory read host address");
static_assert(sizeof(ScalarMemoryRegion) == 32, "native scalar-memory region layout changed");
static_assert(offsetof(EntryContext, exit_reason) <= 4095u * sizeof(uint32_t), "EntryContext exceeds A64 LDR/STR range");
static_assert(offsetof(X64State, gpr) == 0, "native GPR addressing assumes X64State starts with gpr");
static_assert((offsetof(X64State, rip) % sizeof(uint64_t)) == 0, "unaligned native RIP");
static_assert((offsetof(X64State, rflags) % sizeof(uint64_t)) == 0, "unaligned native RFLAGS");
static_assert((offsetof(X64State, xmm) % 16u) == 0, "native vector state must be 128-bit aligned");
static_assert((sizeof(((X64State*)nullptr)->xmm[0]) % 16u) == 0, "native vector register stride must preserve alignment");

std::atomic<uint64_t> g_next_epoch {1};

#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || defined(LUMEN_CPU_X64_TEST)
thread_local IrVerificationTestCounters g_ir_verification_test_counters {};
#endif

uint64_t NextEpoch()
{
	uint64_t result = g_next_epoch.fetch_add(1, std::memory_order_relaxed);
	if (result == 0)
	{
		result = g_next_epoch.fetch_add(1, std::memory_order_relaxed);
	}
	return result;
}

#if defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE) && LUMEN_A64_DYNAREC_MACOS
constexpr uint32_t HOST_PC_PROFILE_SCHEMA_VERSION = 1;
constexpr uint64_t HOST_PC_PROFILE_FNV_OFFSET = 14695981039346656037ULL;
constexpr uint64_t HOST_PC_PROFILE_FNV_PRIME = 1099511628211ULL;

enum class HostPcProfilePhase : uint8_t
{
	Dormant,
	Preparing,
	Recording,
	ClosingClaimed,
	Closing,
	Finishing,
	Frozen,
};

constexpr unsigned HOST_PC_PROFILE_CONTROL_PHASE_SHIFT = 56;
constexpr uint64_t HOST_PC_PROFILE_CONTROL_WRITER_MASK =
	(UINT64_C(1) << HOST_PC_PROFILE_CONTROL_PHASE_SHIFT) - 1;

constexpr uint64_t HostPcProfileControl(
	HostPcProfilePhase phase, uint64_t writers)
{
	return (static_cast<uint64_t>(phase)
	        << HOST_PC_PROFILE_CONTROL_PHASE_SHIFT) |
	       writers;
}

constexpr HostPcProfilePhase HostPcProfileControlPhase(uint64_t control)
{
	return static_cast<HostPcProfilePhase>(
	    control >> HOST_PC_PROFILE_CONTROL_PHASE_SHIFT);
}

constexpr uint64_t HostPcProfileControlWriters(uint64_t control)
{
	return control & HOST_PC_PROFILE_CONTROL_WRITER_MASK;
}

struct HostPcProfileSlot
{
	uint64_t base           = 0;
	uint64_t published_size = 0;
	uint64_t capacity       = 0;
	uint64_t epoch          = 0;
	uint64_t publish_ns     = 0;
	std::atomic<uint64_t> retire_ns {0};
	std::atomic<uint64_t> ready_generation {0};
};

static_assert(HOST_PC_PROFILE_RECORD_CAPACITY < UINT32_MAX,
	          "host-PC record indices must fit the generation token");
static_assert(static_cast<uint64_t>(HostPcProfilePhase::Frozen) <
	              (UINT64_C(1) <<
	               (64 - HOST_PC_PROFILE_CONTROL_PHASE_SHIFT)),
	          "host-PC phases must fit the packed control word");
static_assert(std::atomic<uint32_t>::is_always_lock_free &&
	              std::atomic<uint64_t>::is_always_lock_free,
	          "host-PC publication and close coordination must remain lock-free");

std::array<HostPcProfileSlot,
	       static_cast<size_t>(HOST_PC_PROFILE_RECORD_CAPACITY)>
	g_host_pc_profile_records {};
// Phase and admitted-writer count share one modification order. A close CAS
// therefore cannot miss a writer that successfully linearized its admission,
// and a stale pre-close writer CAS cannot succeed after freeze.
std::atomic<uint64_t> g_host_pc_profile_control {
	HostPcProfileControl(HostPcProfilePhase::Dormant, 0)};
std::atomic<uint64_t> g_host_pc_profile_next_generation {0};
std::atomic<uint64_t> g_host_pc_profile_generation {0};
std::atomic<uint64_t> g_host_pc_profile_pid {0};
std::atomic<uint64_t> g_host_pc_profile_begin_ns {0};
std::atomic<uint64_t> g_host_pc_profile_cutoff_ns {0};
std::atomic<uint64_t> g_host_pc_profile_next_record {0};
std::atomic<uint64_t> g_host_pc_profile_retired {0};
std::atomic<uint64_t> g_host_pc_profile_overflow {0};
std::atomic<uint64_t> g_host_pc_profile_invalid {0};
HostPcProfileSnapshot g_host_pc_profile_frozen_snapshot {};

#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
std::atomic<int32_t> g_host_pc_profile_test_environment {-1};
std::atomic<uint64_t> g_host_pc_profile_test_capacity {
	HOST_PC_PROFILE_RECORD_CAPACITY};
std::atomic<bool> g_host_pc_profile_test_clock_enabled {false};
std::atomic<uint64_t> g_host_pc_profile_test_clock_ns {0};
std::atomic<bool> g_host_pc_profile_test_pause_writer {false};
std::atomic<bool> g_host_pc_profile_test_writer_paused {false};
std::atomic<bool> g_host_pc_profile_test_release_writer {true};
std::atomic<bool> g_host_pc_profile_test_finish_waiting {false};
std::atomic<bool> g_host_pc_profile_test_pause_pre_admission {false};
std::atomic<bool> g_host_pc_profile_test_pre_admission_paused {false};
std::atomic<bool> g_host_pc_profile_test_release_pre_admission {true};
#endif

bool HostPcProfileEnvironmentValueEnabled(const char* value)
{
	return value != nullptr && std::strcmp(value, "1") == 0;
}

void HostPcProfileSaturatingIncrement(std::atomic<uint64_t>* value)
{
	if (value == nullptr) return;
	uint64_t current = value->load(std::memory_order_relaxed);
	while (current != UINT64_MAX &&
	       !value->compare_exchange_weak(
	           current, current + 1, std::memory_order_relaxed,
	           std::memory_order_relaxed))
	{
	}
}

void HostPcProfileMarkInvalid()
{
	HostPcProfileSaturatingIncrement(&g_host_pc_profile_invalid);
}

uint64_t HostPcProfileEffectiveCapacity()
{
#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
	return g_host_pc_profile_test_capacity.load(std::memory_order_acquire);
#else
	return HOST_PC_PROFILE_RECORD_CAPACITY;
#endif
}

uint64_t HostPcProfileNowNs()
{
#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
	if (g_host_pc_profile_test_clock_enabled.load(std::memory_order_acquire))
	{
		return g_host_pc_profile_test_clock_ns.load(std::memory_order_acquire);
	}
#endif
	const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count();
	return count > 0 ? static_cast<uint64_t>(count) : 0;
}

bool HostPcProfileEnterWriter()
{
	uint64_t control =
	    g_host_pc_profile_control.load(std::memory_order_acquire);
	for (;;)
	{
		if (HostPcProfileControlPhase(control) !=
		    HostPcProfilePhase::Recording)
		{
			return false;
		}
		const uint64_t writers = HostPcProfileControlWriters(control);
		if (writers == HOST_PC_PROFILE_CONTROL_WRITER_MASK)
		{
			HostPcProfileMarkInvalid();
			return false;
		}
#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
		if (g_host_pc_profile_test_pause_pre_admission.load(
		        std::memory_order_acquire))
		{
			g_host_pc_profile_test_pre_admission_paused.store(
			    true, std::memory_order_release);
			while (!g_host_pc_profile_test_release_pre_admission.load(
			    std::memory_order_acquire))
			{
				std::this_thread::yield();
			}
			g_host_pc_profile_test_pre_admission_paused.store(
			    false, std::memory_order_release);
		}
#endif
		const uint64_t desired = HostPcProfileControl(
		    HostPcProfilePhase::Recording, writers + 1);
		if (g_host_pc_profile_control.compare_exchange_weak(
		        control, desired, std::memory_order_acquire,
		        std::memory_order_acquire))
		{
			return true;
		}
	}
}

void HostPcProfileLeaveWriter()
{
	uint64_t control =
	    g_host_pc_profile_control.load(std::memory_order_acquire);
	for (;;)
	{
		const uint64_t writers = HostPcProfileControlWriters(control);
		if (writers == 0)
		{
			HostPcProfileMarkInvalid();
			return;
		}
		const uint64_t desired = HostPcProfileControl(
		    HostPcProfileControlPhase(control), writers - 1);
		if (g_host_pc_profile_control.compare_exchange_weak(
		        control, desired, std::memory_order_acq_rel,
		        std::memory_order_acquire))
		{
			return;
		}
	}
}

class HostPcProfileWriterGuard final
{
public:
	HostPcProfileWriterGuard(): m_active(HostPcProfileEnterWriter()) {}
	~HostPcProfileWriterGuard()
	{
		if (m_active) HostPcProfileLeaveWriter();
	}

	HostPcProfileWriterGuard(const HostPcProfileWriterGuard&) = delete;
	HostPcProfileWriterGuard& operator=(
	    const HostPcProfileWriterGuard&) = delete;

	explicit operator bool() const { return m_active; }

private:
	bool m_active = false;
};

void HostPcProfilePauseWriterForTest()
{
#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
	if (!g_host_pc_profile_test_pause_writer.load(std::memory_order_acquire))
		return;
	g_host_pc_profile_test_writer_paused.store(true,
	                                          std::memory_order_release);
	while (!g_host_pc_profile_test_release_writer.load(
	    std::memory_order_acquire))
	{
		std::this_thread::yield();
	}
	g_host_pc_profile_test_writer_paused.store(false,
	                                          std::memory_order_release);
#endif
}

bool HostPcProfileReserveRecord(uint64_t* index)
{
	if (index == nullptr)
	{
		HostPcProfileMarkInvalid();
		return false;
	}
	const uint64_t capacity = HostPcProfileEffectiveCapacity();
	uint64_t next = g_host_pc_profile_next_record.load(
	    std::memory_order_relaxed);
	while (next < capacity)
	{
		if (g_host_pc_profile_next_record.compare_exchange_weak(
		        next, next + 1, std::memory_order_relaxed,
		        std::memory_order_relaxed))
		{
			*index = next;
			return true;
		}
	}
	HostPcProfileSaturatingIncrement(&g_host_pc_profile_overflow);
	HostPcProfileMarkInvalid();
	return false;
}

uint64_t HostPcProfileToken(uint64_t generation, uint64_t index)
{
	if (generation == 0 || generation > UINT32_MAX || index >= UINT32_MAX)
		return 0;
	return (generation << 32u) | (index + 1u);
}

uint64_t RegisterHostPcProfileMapping(
	uint64_t base, uint64_t published_size, uint64_t capacity,
	uint64_t epoch)
{
	HostPcProfileWriterGuard writer;
	if (!writer) return 0;
	HostPcProfilePauseWriterForTest();

	const uint64_t publish_ns = HostPcProfileNowNs();
	if (base == 0 || published_size == 0 || capacity == 0 ||
	    published_size > capacity || epoch == 0 || publish_ns == 0 ||
	    base > UINT64_MAX - published_size ||
	    base > UINT64_MAX - capacity)
	{
		HostPcProfileMarkInvalid();
		return 0;
	}
	uint64_t index = 0;
	if (!HostPcProfileReserveRecord(&index)) return 0;
	const uint64_t generation =
	    g_host_pc_profile_generation.load(std::memory_order_acquire);
	auto& slot = g_host_pc_profile_records[static_cast<size_t>(index)];
	if (generation == 0 || generation > UINT32_MAX ||
	    slot.ready_generation.load(std::memory_order_acquire) != 0)
	{
		HostPcProfileMarkInvalid();
		return 0;
	}
	slot.base           = base;
	slot.published_size = published_size;
	slot.capacity       = capacity;
	slot.epoch          = epoch;
	slot.publish_ns     = publish_ns;
	slot.retire_ns.store(0, std::memory_order_relaxed);
	slot.ready_generation.store(generation, std::memory_order_release);
	return HostPcProfileToken(generation, index);
}

bool RetireHostPcProfileMapping(uint64_t token)
{
	if (token == 0) return false;
	HostPcProfileWriterGuard writer;
	if (!writer) return false;
	HostPcProfilePauseWriterForTest();

	const uint64_t generation = token >> 32u;
	const uint64_t encoded_index = token & UINT32_MAX;
	if (generation == 0 || encoded_index == 0)
	{
		HostPcProfileMarkInvalid();
		return false;
	}
	const uint64_t index = encoded_index - 1;
	if (index >= g_host_pc_profile_next_record.load(
	                 std::memory_order_acquire) ||
	    index >= HostPcProfileEffectiveCapacity() ||
	    generation !=
	        g_host_pc_profile_generation.load(std::memory_order_acquire))
	{
		HostPcProfileMarkInvalid();
		return false;
	}
	auto& slot = g_host_pc_profile_records[static_cast<size_t>(index)];
	if (slot.ready_generation.load(std::memory_order_acquire) != generation)
	{
		HostPcProfileMarkInvalid();
		return false;
	}
	const uint64_t retire_ns = HostPcProfileNowNs();
	if (retire_ns == 0 || retire_ns <= slot.publish_ns)
	{
		HostPcProfileMarkInvalid();
		return false;
	}
	uint64_t expected = 0;
	if (!slot.retire_ns.compare_exchange_strong(
	        expected, retire_ns, std::memory_order_release,
	        std::memory_order_relaxed))
	{
		HostPcProfileMarkInvalid();
		return false;
	}
	HostPcProfileSaturatingIncrement(&g_host_pc_profile_retired);
	return true;
}

uint64_t HostPcProfileHashWord(uint64_t hash, uint64_t value)
{
	for (unsigned byte = 0; byte < 8; byte++)
	{
		hash ^= static_cast<uint8_t>(value >> (byte * 8u));
		hash *= HOST_PC_PROFILE_FNV_PRIME;
	}
	return hash;
}

uint64_t HostPcProfileChecksum(const HostPcProfileSnapshot& snapshot)
{
	uint64_t hash = HOST_PC_PROFILE_FNV_OFFSET;
	hash = HostPcProfileHashWord(hash, snapshot.schema_version);
	hash = HostPcProfileHashWord(hash, snapshot.generation);
	hash = HostPcProfileHashWord(hash, snapshot.pid);
	hash = HostPcProfileHashWord(hash, snapshot.begin_ns);
	hash = HostPcProfileHashWord(hash, snapshot.cutoff_ns);
	hash = HostPcProfileHashWord(hash, snapshot.capacity);
	hash = HostPcProfileHashWord(hash, snapshot.record_count);
	hash = HostPcProfileHashWord(hash, snapshot.retired_count);
	hash = HostPcProfileHashWord(hash, snapshot.overflow_count);
	hash = HostPcProfileHashWord(hash, snapshot.invalid_count);
	for (uint64_t index = 0; index < snapshot.record_count; index++)
	{
		const auto& slot =
		    g_host_pc_profile_records[static_cast<size_t>(index)];
		hash = HostPcProfileHashWord(hash, index);
		hash = HostPcProfileHashWord(hash, slot.base);
		hash = HostPcProfileHashWord(hash, slot.published_size);
		hash = HostPcProfileHashWord(hash, slot.capacity);
		hash = HostPcProfileHashWord(hash, slot.epoch);
		hash = HostPcProfileHashWord(hash, slot.publish_ns);
		hash = HostPcProfileHashWord(
		    hash, slot.retire_ns.load(std::memory_order_acquire));
	}
	return hash;
}

bool HostPcProfileSnapshotsMatch(const HostPcProfileSnapshot& first,
	                              const HostPcProfileSnapshot& second)
{
	return first.schema_version == second.schema_version &&
	       first.complete == second.complete &&
	       first.generation == second.generation &&
	       first.pid == second.pid && first.begin_ns == second.begin_ns &&
	       first.cutoff_ns == second.cutoff_ns &&
	       first.capacity == second.capacity &&
	       first.record_count == second.record_count &&
	       first.retired_count == second.retired_count &&
	       first.overflow_count == second.overflow_count &&
	       first.invalid_count == second.invalid_count &&
	       first.checksum == second.checksum;
}
#endif

bool RoundUp(size_t value, size_t alignment, size_t* result)
{
	if (result == nullptr || value == 0 || alignment == 0 || value > (std::numeric_limits<size_t>::max)() - (alignment - 1))
	{
		return false;
	}
	*result = ((value + alignment - 1) / alignment) * alignment;
	return true;
}

size_t HostPageSize()
{
#if LUMEN_A64_DYNAREC_MACOS || LUMEN_A64_DYNAREC_LINUX
	const long value = sysconf(_SC_PAGESIZE);
	return value > 0 ? static_cast<size_t>(value) : 0;
#elif LUMEN_A64_DYNAREC_WINDOWS
	SYSTEM_INFO info {};
	GetSystemInfo(&info);
	return static_cast<size_t>(info.dwPageSize);
#else
	return 0;
#endif
}

#if LUMEN_A64_DYNAREC_MACOS || \
    defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_CPU_X64_TEST) || \
    defined(LUMEN_JIT_PAGE_RECYCLE_BENCHMARK)
bool JitPageRecycleEnvironmentValueEnabled(const char* value)
{
	// Production defaults to recycling. Exact zero is the narrow rollback;
	// missing, empty and malformed values retain the promoted policy.
	return value == nullptr || std::strcmp(value, "0") != 0;
}
#endif

#if LUMEN_A64_DYNAREC_MACOS || \
    defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_IR_VALIDATOR_BENCHMARK)
bool BranchLocalIrValidationEnvironmentValueEnabled(const char* value)
{
	// Production defaults to the branch-local predicate. Exact zero is the
	// narrow rollback; every other value retains the promoted policy.
	return value == nullptr || std::strcmp(value, "0") != 0;
}
#endif

#if LUMEN_A64_DYNAREC_MACOS || defined(LUMEN_CPU_A64_DYNAREC_TEST)
bool BatchDirectRetirementEnvironmentValueEnabled(const char* value)
{
	// Phase-6BL is intentionally stricter than the older promoted selectors:
	// absent or exact one selects the candidate, exact zero and every malformed
	// spelling fail closed to the per-instruction rollback.
	return value == nullptr || std::strcmp(value, "1") == 0;
}

bool DirectLeafAbiEnvironmentValueEnabled(const char* value)
{
	// Phase-6BM is restricted to the exact preregistered spellings: absent or
	// exact one selects the leaf ABI, while exact zero and every malformed
	// value fail closed to the historical framed entry.
	return value == nullptr || std::strcmp(value, "1") == 0;
}

bool CompactImmediateEnvironmentValueEnabled(const char* value)
{
	// Phase-6BU improves the complete p2300 wall but regresses the preregistered
	// T600 sensitivity, so it remains opt-in. Only exact one selects compact
	// codegen; every other spelling keeps the historical four-wide-move path.
	return value != nullptr && std::strcmp(value, "1") == 0;
}

bool Packed256EnvironmentValueEnabled(const char* value)
{
	// Phase-6BV is an independently reversible width-family experiment. Only
	// exact one admits 256-bit packed VMOV/VXOR lowering; absent, empty and every
	// malformed spelling retain the complete 128-bit-only rollback.
	return value != nullptr && std::strcmp(value, "1") == 0;
}

bool BlockRflagsCacheEnvironmentValueEnabled(const char* value)
{
	return value != nullptr && std::strcmp(value, "1") == 0;
}

bool BlockGprCacheEnvironmentValueEnabled(const char* value)
{
	return value != nullptr && std::strcmp(value, "1") == 0;
}

bool MultiSegmentArtifactEnvironmentValueEnabled(const char* value)
{
	return value != nullptr && std::strcmp(value, "1") == 0;
}

bool DenseResidentCacheEnvironmentValueEnabled(const char* value)
{
	// Phase-6CC is promoted in production. Absence or exact one enables both
	// its FastBlock-parallel resident tier and bounded dense JIT arena; exact
	// zero and malformed values fail closed to the complete legacy rollback.
	return value == nullptr || std::strcmp(value, "1") == 0;
}
#endif

#if (LUMEN_A64_DYNAREC_MACOS && \
     !defined(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)) || \
    defined(LUMEN_CPU_A64_DYNAREC_TEST)
bool DirectContextReuseEnvironmentValueEnabled(const char* value)
{
	// Phase-6BN shares the strict production-candidate spelling contract:
	// absent/exact one enables reuse; exact zero and malformed values retain
	// the complete historical per-entry wrapper.
	return value == nullptr || std::strcmp(value, "1") == 0;
}
#endif

bool BranchLocalIrValidationRequested()
{
#if LUMEN_A64_DYNAREC_MACOS
	// The validator implementation is a process contract. Exact zero selects
	// the eager rollback; nested Run calls and later cache owners must not
	// switch proof semantics mid-process.
	static const bool requested =
	    BranchLocalIrValidationEnvironmentValueEnabled(std::getenv(
	        "LUMEN_X64_A64_DYNAREC_BRANCH_LOCAL_IR_VALIDATION"));
	return requested;
#else
	return false;
#endif
}

bool JitPageRecycleRequested()
{
#if LUMEN_A64_DYNAREC_MACOS && \
    !defined(LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY)
	// Exact zero selects the historical mmap/munmap lifetime. Keep the promoted
	// policy process-latched so a nested Run or later TLS cache owner cannot
	// change executable-memory ownership.
	static const bool requested = JitPageRecycleEnvironmentValueEnabled(
	    std::getenv("LUMEN_X64_A64_DYNAREC_JIT_PAGE_RECYCLE"));
	return requested;
#else
	return false;
#endif
}

bool BatchDirectRetirementRequested()
{
#if LUMEN_A64_DYNAREC_MACOS
	// Generated code and its retirement contract must not change while a
	// process owns compiled blocks or archive entries.
	static const bool requested = BatchDirectRetirementEnvironmentValueEnabled(
	    std::getenv("LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT"));
	return requested;
#else
	return false;
#endif
}

bool DirectLeafAbiRequested()
{
#if LUMEN_A64_DYNAREC_MACOS
	// The entry ABI is part of immutable generated code and therefore remains
	// process-latched for every live cache/archive owner.
	static const bool requested = DirectLeafAbiEnvironmentValueEnabled(
	    std::getenv("LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI"));
	return requested;
#else
	return false;
#endif
}

bool CompactImmediateRequested()
{
#if LUMEN_A64_DYNAREC_MACOS
	// Immediate materialization is part of immutable published code. Latch the
	// selection once so nested Runs and later cache/archive owners cannot mix
	// codegen policies in one process.
	static const bool requested = CompactImmediateEnvironmentValueEnabled(std::getenv("LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES"));
	return requested;
#else
	return false;
#endif
}

bool Packed256Requested()
{
#if LUMEN_A64_DYNAREC_MACOS
	// Packed width is part of the immutable lowered/compiled block contract.
	// Latch it once so every producer and execution-snapshot classification in
	// the process observes the same width policy.
	static const bool requested = Packed256EnvironmentValueEnabled(
	    std::getenv("LUMEN_X64_A64_DYNAREC_PACKED_256"));
	return requested;
#else
	return false;
#endif
}

bool BlockRflagsCacheRequested()
{
#if LUMEN_A64_DYNAREC_MACOS
	static const bool requested = BlockRflagsCacheEnvironmentValueEnabled(
	    std::getenv("LUMEN_X64_A64_DYNAREC_BLOCK_RFLAGS_CACHE"));
	return requested;
#else
	return false;
#endif
}

bool BlockGprCacheRequested()
{
#if LUMEN_A64_DYNAREC_MACOS
	static const bool requested = BlockGprCacheEnvironmentValueEnabled(
	    std::getenv("LUMEN_X64_A64_DYNAREC_BLOCK_GPR_CACHE"));
	return requested;
#else
	return false;
#endif
}

bool MultiSegmentArtifactRequested()
{
#if LUMEN_A64_DYNAREC_MACOS
	static const bool requested = MultiSegmentArtifactEnvironmentValueEnabled(
	    std::getenv("LUMEN_X64_A64_DYNAREC_MULTI_SEGMENT_ARTIFACT"));
	return requested;
#else
	return false;
#endif
}

bool DenseResidentCacheRequested()
{
#if LUMEN_A64_DYNAREC_MACOS
	// Arena geometry, slot ownership and generated-code lifetime form one
	// process contract. Never permit a nested Run to change it underneath live
	// entries.
	static const bool requested = DenseResidentCacheEnvironmentValueEnabled(
	    std::getenv("LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE"));
	return requested;
#else
	return false;
#endif
}

bool DirectContextReuseRequested()
{
#if LUMEN_A64_DYNAREC_MACOS && \
    !defined(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
	// Reused entry state is a chain-wide process contract. Keep phase-6AF on
	// its frozen one-outer/one-Execute diagnostic wrapper.
	static const bool requested = DirectContextReuseEnvironmentValueEnabled(
	    std::getenv("LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE"));
	return requested;
#else
	return false;
#endif
}

#if LUMEN_A64_DYNAREC_MACOS
constexpr size_t JIT_PAGE_RECYCLE_SLOT_CAPACITY = 64;

struct JitPageRecyclePool
{
	std::mutex mutex;
	std::array<void*, JIT_PAGE_RECYCLE_SLOT_CAPACITY> pages {};
	size_t resident = 0;
#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_CPU_X64_TEST) || \
    defined(LUMEN_JIT_PAGE_RECYCLE_BENCHMARK)
	uint64_t acquire_hits = 0;
	uint64_t acquire_misses = 0;
	uint64_t retire_attempts = 0;
	uint64_t retire_stored = 0;
	uint64_t retire_overflows = 0;
	uint64_t retire_rejected = 0;
#endif
};

JitPageRecyclePool* JitPageRecycleGlobal()
{
	// Process-lifetime by design: TLS archives can retire pages during late
	// teardown, after ordinary function-local static destructors would run.
	static auto* pool = new (std::nothrow) JitPageRecyclePool;
	return pool;
}

size_t JitPageRecyclePageSize()
{
	static const size_t page_size = HostPageSize();
	return page_size;
}

void* AcquireRecycledJitPage(size_t capacity)
{
	if (!JitPageRecycleRequested() || capacity != JitPageRecyclePageSize())
		return nullptr;
	auto* pool = JitPageRecycleGlobal();
	if (pool == nullptr) return nullptr;
	std::lock_guard<std::mutex> lock(pool->mutex);
	if (pool->resident == 0)
	{
#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_CPU_X64_TEST) || \
    defined(LUMEN_JIT_PAGE_RECYCLE_BENCHMARK)
		pool->acquire_misses++;
#endif
		return nullptr;
	}
	void* address = pool->pages[--pool->resident];
	pool->pages[pool->resident] = nullptr;
#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_CPU_X64_TEST) || \
    defined(LUMEN_JIT_PAGE_RECYCLE_BENCHMARK)
	pool->acquire_hits++;
#endif
	return address;
}

bool StoreRecycledJitPage(void* address, size_t capacity)
{
	if (address == nullptr) return false;
	auto* pool = JitPageRecycleGlobal();
	if (pool == nullptr) return false;
	std::lock_guard<std::mutex> lock(pool->mutex);
#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_CPU_X64_TEST) || \
    defined(LUMEN_JIT_PAGE_RECYCLE_BENCHMARK)
	pool->retire_attempts++;
#endif
	if (!JitPageRecycleRequested() || capacity != JitPageRecyclePageSize())
	{
#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_CPU_X64_TEST) || \
    defined(LUMEN_JIT_PAGE_RECYCLE_BENCHMARK)
		pool->retire_rejected++;
#endif
		return false;
	}
	if (pool->resident == pool->pages.size())
	{
#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_CPU_X64_TEST) || \
    defined(LUMEN_JIT_PAGE_RECYCLE_BENCHMARK)
		pool->retire_overflows++;
#endif
		return false;
	}
	pool->pages[pool->resident++] = address;
#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_CPU_X64_TEST) || \
    defined(LUMEN_JIT_PAGE_RECYCLE_BENCHMARK)
	pool->retire_stored++;
#endif
	return true;
}

constexpr size_t DENSE_RESIDENT_MIN_SLOT = 256;

struct DenseResidentArenaPage
{
	void* address = nullptr;
	size_t slot_size = 0;
	uint64_t occupied = 0;
	uint8_t slot_count = 0;
};

struct DenseResidentArena
{
	std::mutex mutex;
	std::unique_ptr<DenseResidentArenaPage[]> pages;
	size_t page_capacity = 0;
	size_t page_count = 0;
	size_t mapped_bytes = 0;
	uint64_t acquire_hits = 0;
	uint64_t acquire_misses = 0;
	uint64_t releases = 0;
	uint64_t live_slices = 0;
	std::atomic<uint64_t> release_generation {0};
};

DenseResidentArena* DenseResidentArenaGlobal()
{
	// Process-lifetime storage is intentional. Executable slices can be owned
	// by TLS caches whose teardown order is later than ordinary statics; the OS
	// reclaims the bounded arena at process exit.
	static auto* arena = new (std::nothrow) DenseResidentArena;
	return arena;
}

uint64_t DenseResidentFullMask(uint8_t slot_count)
{
	return slot_count == 64 ? UINT64_MAX
	                        : ((UINT64_C(1) << slot_count) - 1u);
}

size_t DenseResidentSlotSize(size_t minimum_capacity, size_t page_size)
{
	if (minimum_capacity > (std::numeric_limits<size_t>::max)() - A64_WORD_SIZE)
		return 0;
	size_t required = minimum_capacity + A64_WORD_SIZE;
	size_t slot_size = DENSE_RESIDENT_MIN_SLOT;
	while (slot_size < required && slot_size <= page_size / 2u)
		slot_size *= 2u;
	return slot_size >= required && slot_size <= page_size ? slot_size : 0;
}

bool AcquireDenseResidentSlice(size_t minimum_capacity, void** address,
	                            size_t* capacity, uint32_t* page_index,
	                            uint8_t* slot_index)
{
	if (address == nullptr || capacity == nullptr || page_index == nullptr ||
	    slot_index == nullptr || !DenseResidentCacheRequested())
	{
		return false;
	}
	const size_t page_size = JitPageRecyclePageSize();
	const size_t slot_size = DenseResidentSlotSize(minimum_capacity, page_size);
	if (page_size == 0 || slot_size == 0 ||
	    page_size > DENSE_RESIDENT_EXECUTABLE_BUDGET)
	{
		return false;
	}
	auto* arena = DenseResidentArenaGlobal();
	if (arena == nullptr) return false;
	std::lock_guard<std::mutex> lock(arena->mutex);
	if (arena->pages == nullptr)
	{
		arena->page_capacity = DENSE_RESIDENT_EXECUTABLE_BUDGET / page_size;
		arena->pages.reset(new (std::nothrow)
		                       DenseResidentArenaPage[arena->page_capacity]);
		if (arena->pages == nullptr || arena->page_capacity == 0) return false;
	}

	DenseResidentArenaPage* selected = nullptr;
	size_t selected_page = 0;
	DenseResidentArenaPage* empty_page = nullptr;
	size_t empty_page_index = 0;
	for (size_t index = 0; index < arena->page_count; index++)
	{
		auto& page = arena->pages[index];
		if (page.slot_size == slot_size &&
		    page.occupied != DenseResidentFullMask(page.slot_count))
		{
			selected = &page;
			selected_page = index;
			arena->acquire_hits++;
			break;
		}
		if (empty_page == nullptr && page.occupied == 0)
		{
			empty_page = &page;
			empty_page_index = index;
		}
	}
	if (selected == nullptr && empty_page != nullptr)
	{
		// A page with no live slice can safely serve another size class. Without
		// this reclassification a changing code-size distribution could strand
		// the fixed 16 MiB budget in empty pages of obsolete classes.
		selected = empty_page;
		selected_page = empty_page_index;
		selected->slot_size = slot_size;
		selected->slot_count = static_cast<uint8_t>(page_size / slot_size);
		arena->acquire_hits++;
	}
	if (selected == nullptr)
	{
		arena->acquire_misses++;
		if (arena->page_count == arena->page_capacity ||
		    arena->mapped_bytes > DENSE_RESIDENT_EXECUTABLE_BUDGET - page_size)
		{
			return false;
		}
		void* mapping = AcquireRecycledJitPage(page_size);
		if (mapping == nullptr)
		{
			mapping = mmap(nullptr, page_size,
			               PROT_READ | PROT_WRITE | PROT_EXEC,
			               MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
			if (mapping == MAP_FAILED) mapping = nullptr;
		}
		if (mapping == nullptr) return false;
		selected_page = arena->page_count++;
		selected = &arena->pages[selected_page];
		selected->address = mapping;
		selected->slot_size = slot_size;
		selected->slot_count = static_cast<uint8_t>(page_size / slot_size);
		selected->occupied = 0;
		arena->mapped_bytes += page_size;
	}

	uint8_t selected_slot = 0;
	while (selected_slot < selected->slot_count &&
	       (selected->occupied & (UINT64_C(1) << selected_slot)) != 0)
	{
		selected_slot++;
	}
	if (selected_slot == selected->slot_count) return false;
	selected->occupied |= UINT64_C(1) << selected_slot;
	arena->live_slices++;
	*address = static_cast<uint8_t*>(selected->address) +
	           static_cast<size_t>(selected_slot) * selected->slot_size;
	*capacity = selected->slot_size;
	*page_index = static_cast<uint32_t>(selected_page);
	*slot_index = selected_slot;
	return true;
}

void ReleaseDenseResidentSlice(uint32_t page_index, uint8_t slot_index)
{
	auto* arena = DenseResidentArenaGlobal();
	if (arena == nullptr) return;
	std::lock_guard<std::mutex> lock(arena->mutex);
	if (arena->pages == nullptr || page_index >= arena->page_count) return;
	auto& page = arena->pages[page_index];
	if (slot_index >= page.slot_count) return;
	const uint64_t bit = UINT64_C(1) << slot_index;
	if ((page.occupied & bit) == 0) return;
	page.occupied &= ~bit;
	if (arena->live_slices != 0) arena->live_slices--;
	arena->releases++;
	arena->release_generation.fetch_add(1, std::memory_order_relaxed);
}
#endif

#if LUMEN_A64_DYNAREC_MACOS
struct JitCopyContext
{
	void*       destination = nullptr;
	size_t      capacity    = 0;
	const void* source      = nullptr;
	size_t      size        = 0;
	bool        terminate_tail = false;
};

#if defined(LUMEN_JIT_COPY_LEGACY_REGISTRY_BENCHMARK)
// Benchmark-only reproduction of the phase-2 registry. Production targets
// never define this macro; keeping the baseline beside the candidate makes the
// preregistered B/C executable pair reproducible from one source revision.
struct JitCopySlot: JitCopyContext
{
	bool occupied = false;
};

std::atomic_flag             g_jit_copy_registry_lock = ATOMIC_FLAG_INIT;
std::array<JitCopySlot, 256> g_jit_copy_registry {};

class JitCopyRegistryGuard final
{
public:
	JitCopyRegistryGuard() noexcept
	{
		while (g_jit_copy_registry_lock.test_and_set(std::memory_order_acquire))
		{
		}
	}
	~JitCopyRegistryGuard()
	{
		g_jit_copy_registry_lock.clear(std::memory_order_release);
	}

	JitCopyRegistryGuard(const JitCopyRegistryGuard&)            = delete;
	JitCopyRegistryGuard& operator=(const JitCopyRegistryGuard&) = delete;
};

class RegisteredJitCopy final
{
public:
	RegisteredJitCopy(void* destination, size_t capacity, const void* source,
	                  size_t size, bool terminate_tail = false)
	{
		JitCopyRegistryGuard guard;
		for (size_t index = 0; index < g_jit_copy_registry.size(); index++)
		{
			auto& slot = g_jit_copy_registry[index];
			if (slot.occupied) continue;
			slot.destination = destination;
			slot.capacity    = capacity;
			slot.source      = source;
			slot.size        = size;
			slot.terminate_tail = terminate_tail;
			slot.occupied    = true;
			m_slot           = index;
			break;
		}
	}

	~RegisteredJitCopy() { Release(); }

	void Release() noexcept
	{
		if (!IsRegistered()) return;
		JitCopyRegistryGuard guard;
		g_jit_copy_registry[m_slot] = {};
		m_slot = (std::numeric_limits<size_t>::max)();
	}

	RegisteredJitCopy(const RegisteredJitCopy&)            = delete;
	RegisteredJitCopy& operator=(const RegisteredJitCopy&) = delete;

	bool IsRegistered() const { return m_slot < g_jit_copy_registry.size(); }
	void* Opaque() const
	{
		return reinterpret_cast<void*>(static_cast<uintptr_t>(m_slot + 1));
	}

private:
	size_t m_slot = (std::numeric_limits<size_t>::max)();
};

__attribute__((noinline)) int CopyJitCode(void* opaque) noexcept
{
	const uintptr_t token = reinterpret_cast<uintptr_t>(opaque);
	if (token == 0 || token > g_jit_copy_registry.size()) return EINVAL;
	JitCopyRegistryGuard guard;
	const auto& context = g_jit_copy_registry[token - 1];
	if (!context.occupied || context.destination == nullptr || context.source == nullptr ||
	    context.size == 0 || context.size > context.capacity)
	{
		return EINVAL;
	}

	const auto destination = reinterpret_cast<uintptr_t>(context.destination);
	const auto source      = reinterpret_cast<uintptr_t>(context.source);
	const bool terminate_tail = context.terminate_tail &&
	                            context.capacity - context.size >= A64_WORD_SIZE;
	const size_t destination_write_size =
	    context.size + (terminate_tail ? A64_WORD_SIZE : 0);
	if (destination > UINTPTR_MAX - destination_write_size ||
	    source > UINTPTR_MAX - context.size)
	{
		return EOVERFLOW;
	}
	const uintptr_t destination_end = destination + destination_write_size;
	const uintptr_t source_end      = source + context.size;
	if (destination < source_end && source < destination_end) return EINVAL;

	std::memcpy(context.destination, context.source, context.size);
	if (terminate_tail)
	{
		// A freshly mapped page is zero-filled. Preserve that fail-closed
		// boundary when this mapping previously held a longer artifact.
		std::memset(static_cast<uint8_t*>(context.destination) + context.size,
		            0, A64_WORD_SIZE);
	}
	return 0;
}

PTHREAD_JIT_WRITE_ALLOW_CALLBACKS_NP(CopyJitCode);
#else
// pthread_jit_write_with_callback_np treats its opaque argument as
// attacker-controlled. Pass only a bounded index/generation token and keep
// every pointer in fixed, trusted process storage. Independent slots have
// independent leases so distinct MAP_JIT publications never serialize their
// copies on one process-wide cache line.
enum class JitCopyState : uint8_t
{
	Free,
	Reserved,
	Ready,
	Copying,
	Consumed,
	Failed,
};

constexpr uint64_t JitCopyControl(uint32_t generation, JitCopyState state)
{
	return (static_cast<uint64_t>(generation) << 8u) |
	       static_cast<uint8_t>(state);
}

constexpr uint32_t JitCopyGeneration(uint64_t control)
{
	return static_cast<uint32_t>(control >> 8u);
}

constexpr JitCopyState JitCopyControlState(uint64_t control)
{
	return static_cast<JitCopyState>(control & 0xffu);
}

constexpr size_t JIT_COPY_SLOT_ALIGNMENT = 128;

struct alignas(JIT_COPY_SLOT_ALIGNMENT) JitCopySlot: JitCopyContext
{
	std::atomic<uint64_t> control {JitCopyControl(0, JitCopyState::Free)};
};

constexpr size_t JIT_COPY_SLOT_COUNT = 256;
constexpr uintptr_t JIT_COPY_SLOT_MASK = JIT_COPY_SLOT_COUNT - 1;
constexpr unsigned JIT_COPY_GENERATION_SHIFT = 8;
static_assert((JIT_COPY_SLOT_COUNT & (JIT_COPY_SLOT_COUNT - 1)) == 0,
              "JIT copy registry must remain power-of-two bounded");
static_assert(JIT_COPY_SLOT_COUNT == (size_t {1} << JIT_COPY_GENERATION_SHIFT),
              "the token split must dedicate exactly eight bits to the slot");
static_assert(std::numeric_limits<uintptr_t>::digits >=
                  JIT_COPY_GENERATION_SHIFT + std::numeric_limits<uint32_t>::digits,
              "the complete slot/generation token must fit uintptr_t");
static_assert(alignof(JitCopySlot) >= JIT_COPY_SLOT_ALIGNMENT &&
                  (sizeof(JitCopySlot) % JIT_COPY_SLOT_ALIGNMENT) == 0,
              "independent JIT copy leases must not share a cache line");
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "JIT copy lease transitions must not call a hidden lock");

std::array<JitCopySlot, JIT_COPY_SLOT_COUNT> g_jit_copy_registry {};

size_t JitCopyStartSlot(const void* destination) noexcept
{
	uintptr_t value = reinterpret_cast<uintptr_t>(destination) >> 12u;
	value ^= value >> 17u;
	value ^= value >> 9u;
	return static_cast<size_t>(value & JIT_COPY_SLOT_MASK);
}

#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
std::atomic<uint32_t>  g_jit_copy_test_expected {0};
std::atomic<uint32_t>  g_jit_copy_test_arrivals {0};
std::atomic<uint32_t>  g_jit_copy_test_active {0};
std::atomic<uint32_t>  g_jit_copy_test_max_concurrent {0};
std::atomic<bool>      g_jit_copy_test_release {true};
std::atomic<uintptr_t> g_jit_copy_test_last_token {0};
static_assert(std::atomic<uint32_t>::is_always_lock_free &&
                  std::atomic<bool>::is_always_lock_free &&
                  std::atomic<uintptr_t>::is_always_lock_free,
              "test barriers inside the writable JIT interval must remain lock-free");

void EnterJitCopyTestBarrier() noexcept
{
	const uint32_t expected = g_jit_copy_test_expected.load(std::memory_order_acquire);
	if (expected == 0) return;

	const uint32_t active = g_jit_copy_test_active.fetch_add(1, std::memory_order_acq_rel) + 1;
	uint32_t observed = g_jit_copy_test_max_concurrent.load(std::memory_order_relaxed);
	while (observed < active &&
	       !g_jit_copy_test_max_concurrent.compare_exchange_weak(
	           observed, active, std::memory_order_relaxed, std::memory_order_relaxed))
	{
	}
	const uint32_t arrivals = g_jit_copy_test_arrivals.fetch_add(1, std::memory_order_acq_rel) + 1;
	if (arrivals >= expected) g_jit_copy_test_release.store(true, std::memory_order_release);
	while (!g_jit_copy_test_release.load(std::memory_order_acquire))
	{
	}
	g_jit_copy_test_active.fetch_sub(1, std::memory_order_acq_rel);
}
#endif

class RegisteredJitCopy final
{
public:
	RegisteredJitCopy(void* destination, size_t capacity, const void* source,
	                  size_t size, bool terminate_tail = false)
	{
		const size_t start = JitCopyStartSlot(destination);
		for (size_t offset = 0; offset < g_jit_copy_registry.size(); offset++)
		{
			const size_t index = (start + offset) & JIT_COPY_SLOT_MASK;
			auto& slot = g_jit_copy_registry[index];
			uint64_t control = slot.control.load(std::memory_order_acquire);
			if (JitCopyControlState(control) != JitCopyState::Free) continue;
			const uint32_t previous_generation = JitCopyGeneration(control);
			if (previous_generation == UINT32_MAX) continue;
			const uint32_t generation = previous_generation + 1;
			if (!slot.control.compare_exchange_strong(
			        control, JitCopyControl(generation, JitCopyState::Reserved),
			        std::memory_order_acq_rel, std::memory_order_acquire))
			{
				continue;
			}
			slot.destination = destination;
			slot.capacity    = capacity;
			slot.source      = source;
			slot.size        = size;
			slot.terminate_tail = terminate_tail;
			slot.control.store(JitCopyControl(generation, JitCopyState::Ready),
			                   std::memory_order_release);
			m_slot           = index;
			m_generation     = generation;
#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
			g_jit_copy_test_last_token.store(
			    reinterpret_cast<uintptr_t>(Opaque()), std::memory_order_release);
#endif
			break;
		}
	}

	~RegisteredJitCopy() { Release(); }

	void Release() noexcept
	{
		if (!IsRegistered()) return;
		auto& slot = g_jit_copy_registry[m_slot];
		const uint64_t reserved = JitCopyControl(m_generation, JitCopyState::Reserved);
		bool owns_slot = false;
		for (;;)
		{
			uint64_t control = slot.control.load(std::memory_order_acquire);
			if (JitCopyGeneration(control) != m_generation) break;
			const JitCopyState state = JitCopyControlState(control);
			if (state == JitCopyState::Copying) continue;
			if (state != JitCopyState::Ready && state != JitCopyState::Consumed &&
			    state != JitCopyState::Failed)
			{
				break;
			}
			if (slot.control.compare_exchange_weak(
			        control, reserved, std::memory_order_acq_rel,
			        std::memory_order_acquire))
			{
				owns_slot = true;
				break;
			}
		}
		if (owns_slot)
		{
			slot.destination = nullptr;
			slot.capacity = 0;
			slot.source = nullptr;
			slot.size = 0;
			slot.control.store(JitCopyControl(m_generation, JitCopyState::Free),
			                   std::memory_order_release);
		}
		m_slot = (std::numeric_limits<size_t>::max)();
		m_generation = 0;
	}

	RegisteredJitCopy(const RegisteredJitCopy&)            = delete;
	RegisteredJitCopy& operator=(const RegisteredJitCopy&) = delete;

	bool IsRegistered() const
	{
		return m_slot < g_jit_copy_registry.size() && m_generation != 0;
	}
	void* Opaque() const
	{
		const uintptr_t token =
		    (static_cast<uintptr_t>(m_generation) << JIT_COPY_GENERATION_SHIFT) |
		    static_cast<uintptr_t>(m_slot);
		return reinterpret_cast<void*>(token);
	}

private:
	size_t m_slot = (std::numeric_limits<size_t>::max)();
	uint32_t m_generation = 0;
};

__attribute__((noinline)) int CopyJitCode(void* opaque) noexcept
{
	const uintptr_t token = reinterpret_cast<uintptr_t>(opaque);
	const uintptr_t generation_value = token >> JIT_COPY_GENERATION_SHIFT;
	if (generation_value == 0 || generation_value > UINT32_MAX) return EINVAL;
	const size_t slot_index = static_cast<size_t>(token & JIT_COPY_SLOT_MASK);
	auto& context = g_jit_copy_registry[slot_index];
	const uint32_t generation = static_cast<uint32_t>(generation_value);
	uint64_t expected = JitCopyControl(generation, JitCopyState::Ready);
	if (!context.control.compare_exchange_strong(
	        expected, JitCopyControl(generation, JitCopyState::Copying),
	        std::memory_order_acquire, std::memory_order_relaxed))
	{
		return expected == JitCopyControl(generation, JitCopyState::Copying)
		           ? EBUSY
		           : EINVAL;
	}
	if (context.destination == nullptr || context.source == nullptr || context.size == 0 ||
	    context.size > context.capacity)
	{
		context.control.store(JitCopyControl(generation, JitCopyState::Failed),
		                      std::memory_order_release);
		return EINVAL;
	}

	const auto destination = reinterpret_cast<uintptr_t>(context.destination);
	const auto source      = reinterpret_cast<uintptr_t>(context.source);
	const bool terminate_tail = context.terminate_tail &&
	                            context.capacity - context.size >= A64_WORD_SIZE;
	const size_t destination_write_size =
	    context.size + (terminate_tail ? A64_WORD_SIZE : 0);
	if (destination > UINTPTR_MAX - destination_write_size ||
	    source > UINTPTR_MAX - context.size)
	{
		context.control.store(JitCopyControl(generation, JitCopyState::Failed),
		                      std::memory_order_release);
		return EOVERFLOW;
	}
	const uintptr_t destination_end = destination + destination_write_size;
	const uintptr_t source_end      = source + context.size;
	if (destination < source_end && source < destination_end)
	{
		context.control.store(JitCopyControl(generation, JitCopyState::Failed),
		                      std::memory_order_release);
		return EINVAL;
	}

#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
	EnterJitCopyTestBarrier();
#endif
	std::memcpy(context.destination, context.source, context.size);
	if (terminate_tail)
	{
		// Do not leave a reachable instruction from the previous generation
		// immediately after the newly published block.
		std::memset(static_cast<uint8_t*>(context.destination) + context.size,
		            0, A64_WORD_SIZE);
	}
	context.control.store(JitCopyControl(generation, JitCopyState::Consumed),
	                      std::memory_order_release);
	return 0;
}

PTHREAD_JIT_WRITE_ALLOW_CALLBACKS_NP(CopyJitCode);
#endif
#endif

constexpr bool IsRegister(uint8_t value)
{
	return value < 16;
}

constexpr bool HasEffect(const IrInstruction& instruction, IrEffect effect)
{
	return (instruction.effects & IrEffectMask(effect)) != 0;
}

constexpr bool HasVectorFlag(const IrInstruction& instruction, IrVectorFlag flag)
{
	return (instruction.vector_flags & IrVectorFlagMask(flag)) != 0;
}

uint8_t ExpectedEffects(IrOpcode opcode)
{
	const uint8_t host_call = IrEffectMask(IrEffect::HostCall);
	const uint8_t read_memory = IrEffectMask(IrEffect::ReadGuestMemory);
	const uint8_t write_memory = IrEffectMask(IrEffect::WriteGuestMemory);
	const uint8_t read_flags = IrEffectMask(IrEffect::ReadFlags);
	const uint8_t write_flags = IrEffectMask(IrEffect::WriteFlags);
	const uint8_t may_fault = IrEffectMask(IrEffect::MayFault);
	const uint8_t terminator = IrEffectMask(IrEffect::Terminator);
	const uint8_t stack = IrEffectMask(IrEffect::Stack);
	switch (opcode)
	{
		case IrOpcode::Nop:
		case IrOpcode::MoveRegister:
		case IrOpcode::MoveImmediate:
		case IrOpcode::VectorMoveGpr32ToXmm:
		case IrOpcode::VectorPermuteDwordsImmediate:
		case IrOpcode::VectorMoveRegister:
		case IrOpcode::VectorMoveScalarRegister:
		case IrOpcode::VectorXorRegister:
		case IrOpcode::ZeroExtendRegister:
		case IrOpcode::SignExtendRegister:
		case IrOpcode::EffectiveAddress:
		case IrOpcode::ByteSwapRegister:
		case IrOpcode::BitwiseNot: return 0;
		case IrOpcode::Load:
		case IrOpcode::ZeroExtendMemory:
		case IrOpcode::SignExtendMemory:
		case IrOpcode::VectorLoad:
		case IrOpcode::VectorLoadScalar:
		case IrOpcode::VectorXorMemory:
			return IrEffectMask(IrEffect::ReadGuestMemory) | IrEffectMask(IrEffect::MayFault);
		case IrOpcode::AddRegisterMemory:
		case IrOpcode::SubRegisterMemory:
		case IrOpcode::CompareRegisterMemory:
		case IrOpcode::CompareMemoryRegister:
		case IrOpcode::CompareMemoryImmediate:
			return IrEffectMask(IrEffect::ReadGuestMemory) | IrEffectMask(IrEffect::WriteFlags) |
			       IrEffectMask(IrEffect::MayFault);
		case IrOpcode::OrRegisterMemory:
		case IrOpcode::AndRegisterMemory:
		case IrOpcode::XorRegisterMemory:
		case IrOpcode::TestMemoryImmediate:
		case IrOpcode::SignedMultiplyRegisterMemory:
		case IrOpcode::SignedMultiplyMemoryImmediate:
			return IrEffectMask(IrEffect::ReadGuestMemory) | IrEffectMask(IrEffect::ReadFlags) |
			       IrEffectMask(IrEffect::WriteFlags) | IrEffectMask(IrEffect::MayFault);
		case IrOpcode::AddMemoryRegister:
		case IrOpcode::SubtractMemoryRegister:
			return IrEffectMask(IrEffect::ReadGuestMemory) |
			       IrEffectMask(IrEffect::WriteGuestMemory) |
			       IrEffectMask(IrEffect::WriteFlags) |
			       IrEffectMask(IrEffect::MayFault);
		case IrOpcode::AndMemoryRegister:
		case IrOpcode::AndMemoryImmediate:
		case IrOpcode::IncrementMemory:
			return IrEffectMask(IrEffect::ReadGuestMemory) |
			       IrEffectMask(IrEffect::WriteGuestMemory) |
			       IrEffectMask(IrEffect::ReadFlags) |
			       IrEffectMask(IrEffect::WriteFlags) |
			       IrEffectMask(IrEffect::MayFault);
		case IrOpcode::SetConditionMemory:
			return IrEffectMask(IrEffect::WriteGuestMemory) |
			       IrEffectMask(IrEffect::ReadFlags) |
			       IrEffectMask(IrEffect::MayFault);
		case IrOpcode::Store:
		case IrOpcode::VectorStore:
		case IrOpcode::VectorStoreScalar:
			return IrEffectMask(IrEffect::WriteGuestMemory) | IrEffectMask(IrEffect::MayFault);
		case IrOpcode::AddImmediate:
		case IrOpcode::SubImmediate:
		case IrOpcode::CompareImmediate:
		case IrOpcode::AddRegister:
		case IrOpcode::SubRegister:
		case IrOpcode::CompareRegister:
		case IrOpcode::Negate:
		case IrOpcode::TestImmediate:
		case IrOpcode::AndRegister:
		case IrOpcode::OrRegister:
		case IrOpcode::XorRegister:
		case IrOpcode::TestRegister: return IrEffectMask(IrEffect::WriteFlags);
		case IrOpcode::ShiftLeftImmediate:
		case IrOpcode::ShiftLeftCl:
		case IrOpcode::ShiftRightLogicalImmediate:
		case IrOpcode::ShiftRightLogicalCl:
		case IrOpcode::ShiftRightArithmeticImmediate:
		case IrOpcode::AndImmediate:
		case IrOpcode::OrImmediate:
		case IrOpcode::XorImmediate:
		case IrOpcode::BitTestRegister:
		case IrOpcode::BitTestImmediate:
		case IrOpcode::Increment:
		case IrOpcode::Decrement:
		case IrOpcode::SubtractWithBorrowRegister:
		case IrOpcode::BitExtractRegister:
		case IrOpcode::AndNotRegister:
		case IrOpcode::SignedMultiplyRegister:
		case IrOpcode::SignedMultiplyImmediate:
		case IrOpcode::ClearDirectionFlag:
			return IrEffectMask(IrEffect::ReadFlags) | IrEffectMask(IrEffect::WriteFlags);
		case IrOpcode::SetCondition:
		case IrOpcode::ConditionalMove: return IrEffectMask(IrEffect::ReadFlags);
		case IrOpcode::PushRegister:
			return IrEffectMask(IrEffect::WriteGuestMemory) | IrEffectMask(IrEffect::Stack) |
			       IrEffectMask(IrEffect::MayFault);
		case IrOpcode::PopRegister:
			return IrEffectMask(IrEffect::ReadGuestMemory) | IrEffectMask(IrEffect::Stack) |
			       IrEffectMask(IrEffect::MayFault);
		case IrOpcode::BranchCondition:
			return IrEffectMask(IrEffect::ReadFlags) | IrEffectMask(IrEffect::Terminator);
		case IrOpcode::JumpImmediate: return IrEffectMask(IrEffect::Terminator);
		case IrOpcode::CallImmediate:
			return IrEffectMask(IrEffect::ReadGuestMemory) | IrEffectMask(IrEffect::WriteGuestMemory) |
			       IrEffectMask(IrEffect::Stack) |
			       IrEffectMask(IrEffect::Terminator) | IrEffectMask(IrEffect::MayFault) |
			       IrEffectMask(IrEffect::HostCall);
		case IrOpcode::Return:
			return IrEffectMask(IrEffect::ReadGuestMemory) | IrEffectMask(IrEffect::Stack) |
			       IrEffectMask(IrEffect::Terminator) | IrEffectMask(IrEffect::MayFault);
		case IrOpcode::VectorConvertFloatToIntegerRegister:
		case IrOpcode::VectorConvertIntegerToFloatRegister:
		case IrOpcode::VectorBinaryRegister:
		case IrOpcode::VectorHorizontalAddRegister:
		case IrOpcode::VectorMoveGpr64ToXmm:
			return host_call;
		case IrOpcode::VectorCompareRegister:
			return host_call | read_flags | write_flags;
		case IrOpcode::VectorConvertFloatToIntegerMemory:
		case IrOpcode::VectorConvertIntegerToFloatMemory:
		case IrOpcode::VectorBinaryMemory:
		case IrOpcode::VectorUnpackLowDwordsMemory:
			return host_call | read_memory | may_fault;
		case IrOpcode::VectorCompareMemory:
			return host_call | read_memory | read_flags | write_flags | may_fault;
		case IrOpcode::CallIndirectRegister:
		case IrOpcode::CallIndirectMemory:
			// The helper owns the complete CALL/HLE transaction. Besides publishing
			// the return-address write, a handled host call may consume the matching
			// stack entry before it returns, so both memory directions are explicit
			// even when the branch target itself comes from a register.
			return host_call | read_memory | write_memory | stack | terminator | may_fault;
		case IrOpcode::JumpIndirectRegister:
			return host_call | read_memory | stack | terminator | may_fault;
		case IrOpcode::JumpIndirectMemory:
			return host_call | read_memory | stack | terminator | may_fault;
		case IrOpcode::RepeatMove:
			return host_call | read_memory | write_memory | read_flags | terminator | may_fault;
		case IrOpcode::RepeatStore:
			return host_call | write_memory | read_flags | terminator | may_fault;
		case IrOpcode::AtomicCompareExchangeMemory:
			return host_call | read_memory | write_memory | read_flags | write_flags |
			       terminator | may_fault;
		case IrOpcode::Count: break;
	}
	return 0xff;
}

uint32_t InstructionSemanticMask(IrOpcode opcode)
{
	const uint32_t integer = SemanticMask(SemanticClass::Integer);
	const uint32_t flags = SemanticMask(SemanticClass::Flags);
	const uint32_t memory = SemanticMask(SemanticClass::GuestMemory);
	const uint32_t control = SemanticMask(SemanticClass::StackControl);
	const uint32_t vector = SemanticMask(SemanticClass::SimdFloatingPoint);
	const uint32_t atomics = SemanticMask(SemanticClass::Atomics);
	const uint32_t strings = SemanticMask(SemanticClass::StringOperations);
	const uint32_t host_calls = SemanticMask(SemanticClass::HostCalls);
	const uint32_t precise = SemanticMask(SemanticClass::PreciseExit);
	const uint32_t smc = SemanticMask(SemanticClass::SelfModifyingCode);
	switch (opcode)
	{
		case IrOpcode::Nop: return 0;
		case IrOpcode::MoveRegister:
		case IrOpcode::MoveImmediate:
		case IrOpcode::ZeroExtendRegister:
		case IrOpcode::SignExtendRegister:
		case IrOpcode::EffectiveAddress:
		case IrOpcode::ByteSwapRegister:
		case IrOpcode::BitwiseNot: return integer;
		case IrOpcode::Load:
		case IrOpcode::ZeroExtendMemory:
		case IrOpcode::SignExtendMemory: return integer | memory | precise;
		case IrOpcode::AddRegisterMemory:
		case IrOpcode::SubRegisterMemory:
		case IrOpcode::OrRegisterMemory:
		case IrOpcode::AndRegisterMemory:
		case IrOpcode::XorRegisterMemory:
		case IrOpcode::CompareRegisterMemory:
		case IrOpcode::CompareMemoryRegister:
		case IrOpcode::CompareMemoryImmediate:
		case IrOpcode::TestMemoryImmediate:
		case IrOpcode::SignedMultiplyRegisterMemory:
		case IrOpcode::SignedMultiplyMemoryImmediate:
			return integer | flags | memory | precise;
		case IrOpcode::AddMemoryRegister:
		case IrOpcode::SubtractMemoryRegister:
		case IrOpcode::AndMemoryRegister:
		case IrOpcode::AndMemoryImmediate:
		case IrOpcode::IncrementMemory:
			return integer | flags | memory | precise | smc;
		case IrOpcode::SetConditionMemory:
			return flags | memory | precise | smc;
		case IrOpcode::Store: return memory | precise | smc;
		case IrOpcode::AddImmediate:
		case IrOpcode::SubImmediate:
		case IrOpcode::CompareImmediate:
		case IrOpcode::AddRegister:
		case IrOpcode::SubRegister:
		case IrOpcode::CompareRegister:
		case IrOpcode::ShiftLeftImmediate:
		case IrOpcode::ShiftLeftCl:
		case IrOpcode::ShiftRightLogicalImmediate:
		case IrOpcode::ShiftRightLogicalCl:
		case IrOpcode::ShiftRightArithmeticImmediate:
		case IrOpcode::AndImmediate:
		case IrOpcode::OrImmediate:
		case IrOpcode::XorImmediate:
		case IrOpcode::BitTestRegister:
		case IrOpcode::BitTestImmediate:
		case IrOpcode::Increment:
		case IrOpcode::Decrement:
		case IrOpcode::SetCondition:
		case IrOpcode::ConditionalMove:
		case IrOpcode::Negate:
		case IrOpcode::TestImmediate:
		case IrOpcode::AndRegister:
		case IrOpcode::OrRegister:
		case IrOpcode::XorRegister:
		case IrOpcode::TestRegister:
		case IrOpcode::SubtractWithBorrowRegister:
		case IrOpcode::BitExtractRegister:
		case IrOpcode::AndNotRegister:
		case IrOpcode::SignedMultiplyRegister:
		case IrOpcode::SignedMultiplyImmediate: return integer | flags;
		case IrOpcode::ClearDirectionFlag: return flags;
		case IrOpcode::PushRegister: return memory | control | precise | smc;
		case IrOpcode::PopRegister:
		case IrOpcode::Return: return memory | control | precise;
		case IrOpcode::BranchCondition: return flags | control | precise;
		case IrOpcode::JumpImmediate: return control | precise;
		case IrOpcode::CallImmediate:
			return memory | control | precise | smc | SemanticMask(SemanticClass::HostCalls);
		case IrOpcode::VectorMoveGpr32ToXmm:
		case IrOpcode::VectorPermuteDwordsImmediate:
		case IrOpcode::VectorMoveRegister:
		case IrOpcode::VectorMoveScalarRegister:
		case IrOpcode::VectorXorRegister: return vector;
		case IrOpcode::VectorLoad:
		case IrOpcode::VectorLoadScalar:
		case IrOpcode::VectorXorMemory: return vector | memory | precise;
		case IrOpcode::VectorStore:
		case IrOpcode::VectorStoreScalar: return vector | memory | precise | smc;
		case IrOpcode::VectorConvertFloatToIntegerRegister:
		case IrOpcode::VectorConvertIntegerToFloatRegister:
		case IrOpcode::VectorBinaryRegister:
		case IrOpcode::VectorHorizontalAddRegister:
		case IrOpcode::VectorMoveGpr64ToXmm:
			return vector | host_calls;
		case IrOpcode::VectorCompareRegister:
			return vector | flags | host_calls;
		case IrOpcode::VectorConvertFloatToIntegerMemory:
		case IrOpcode::VectorConvertIntegerToFloatMemory:
		case IrOpcode::VectorBinaryMemory:
		case IrOpcode::VectorUnpackLowDwordsMemory:
			return vector | memory | precise | host_calls;
		case IrOpcode::VectorCompareMemory:
			return vector | flags | memory | precise | host_calls;
		case IrOpcode::CallIndirectRegister:
		case IrOpcode::CallIndirectMemory:
			return memory | control | precise | smc | host_calls;
		case IrOpcode::JumpIndirectRegister:
			return memory | control | precise | host_calls;
		case IrOpcode::JumpIndirectMemory:
			return memory | control | precise | host_calls;
		case IrOpcode::RepeatMove:
			return flags | memory | strings | precise | smc | host_calls;
		case IrOpcode::RepeatStore:
			return flags | memory | strings | precise | smc | host_calls;
		case IrOpcode::AtomicCompareExchangeMemory:
			return integer | flags | memory | atomics | precise | smc | host_calls;
		case IrOpcode::Count: break;
	}
	return 0;
}

bool IsDefaultAddress(const IrAddress& address)
{
	return address.base == IR_NO_REGISTER && address.index == IR_NO_REGISTER && address.scale == 0 &&
	       address.displacement == 0 && address.absolute == 0;
}

bool IsValidAddress(const IrAddress& address, bool allow_absolute)
{
	if (address.base == IR_NO_REGISTER)
	{
		return allow_absolute && address.index == IR_NO_REGISTER && address.scale == 0 && address.displacement == 0;
	}
	if (!IsRegister(address.base) || (address.index != IR_NO_REGISTER && !IsRegister(address.index)) || address.scale > 3 ||
	    address.absolute != 0)
	{
		return false;
	}
	return address.index != IR_NO_REGISTER || address.scale == 0;
}

bool IsNormalizedImmediateForWidth(uint8_t width, uint64_t immediate)
{
	if (width == 1) return immediate <= UINT8_MAX;
	if (width == 2) return immediate <= UINT16_MAX;
	if (width == 4) return immediate <= UINT32_MAX;
	if (width != 8) return false;
	const uint64_t low32 = immediate & UINT32_MAX;
	const uint64_t sign_extended = (low32 & UINT64_C(0x80000000)) != 0
	                                   ? low32 | UINT64_C(0xffffffff00000000)
	                                   : low32;
	return immediate == sign_extended;
}

bool IsTypedHelperOpcode(IrOpcode opcode)
{
	switch (opcode)
	{
		case IrOpcode::VectorConvertFloatToIntegerRegister:
		case IrOpcode::VectorConvertFloatToIntegerMemory:
		case IrOpcode::VectorConvertIntegerToFloatRegister:
		case IrOpcode::VectorConvertIntegerToFloatMemory:
		case IrOpcode::VectorBinaryRegister:
		case IrOpcode::VectorBinaryMemory:
		case IrOpcode::VectorHorizontalAddRegister:
		case IrOpcode::VectorUnpackLowDwordsMemory:
		case IrOpcode::VectorMoveGpr64ToXmm:
		case IrOpcode::VectorCompareRegister:
		case IrOpcode::VectorCompareMemory:
		case IrOpcode::CallIndirectRegister:
		case IrOpcode::CallIndirectMemory:
		case IrOpcode::JumpIndirectRegister:
		case IrOpcode::JumpIndirectMemory:
		case IrOpcode::RepeatMove:
		case IrOpcode::RepeatStore:
		case IrOpcode::AtomicCompareExchangeMemory: return true;
		default: return false;
	}
}

bool IsTypedHelperRegisterOnlyOpcode(IrOpcode opcode)
{
	switch (opcode)
	{
		case IrOpcode::VectorConvertFloatToIntegerRegister:
		case IrOpcode::VectorConvertIntegerToFloatRegister:
		case IrOpcode::VectorBinaryRegister:
		case IrOpcode::VectorHorizontalAddRegister:
		case IrOpcode::VectorMoveGpr64ToXmm:
		case IrOpcode::VectorCompareRegister: return true;
		default: return false;
	}
}

bool ValidateTypedHelperInstruction(const IrInstruction& instruction)
{
	const bool default_address = IsDefaultAddress(instruction.address);
	const bool default_immediate = instruction.immediate == 0;
	const bool default_target = instruction.target == 0;
	const bool no_registers = instruction.destination == IR_NO_REGISTER &&
	                          instruction.source == IR_NO_REGISTER &&
	                          instruction.source2 == IR_NO_REGISTER;
	const uint8_t zero_upper = IrVectorFlagMask(IrVectorFlag::ZeroUpper);
	const auto default_tail = [default_immediate, default_target]()
	{
		return default_immediate && default_target;
	};
	const auto valid_binary_shape = [&instruction](bool memory)
	{
		const auto operation = static_cast<IrVectorBinaryOperation>(instruction.condition);
		if (instruction.condition > static_cast<uint8_t>(IrVectorBinaryOperation::Divide))
			return false;
		if (instruction.width == 4 || instruction.width == 8)
		{
			return instruction.source_width == 0 && instruction.vector_flags == zero_upper;
		}
		if (instruction.width == 16)
		{
			const bool packed_float = instruction.source_width == 4 &&
			                          operation != IrVectorBinaryOperation::Divide;
			const bool packed_double_memory = memory && instruction.source_width == 8 &&
			                                  operation == IrVectorBinaryOperation::Subtract;
			return (packed_float || packed_double_memory) &&
			       instruction.vector_flags == zero_upper;
		}
		return instruction.width == 32 && instruction.source_width == 4 &&
		       operation != IrVectorBinaryOperation::Divide && instruction.vector_flags == 0;
	};

	switch (instruction.opcode)
	{
		case IrOpcode::VectorConvertFloatToIntegerRegister:
			return (instruction.width == 4 || instruction.width == 8) &&
			       IsRegister(instruction.destination) && IsRegister(instruction.source) &&
			       instruction.source_width == 4 && instruction.source2 == IR_NO_REGISTER &&
			       instruction.vector_flags == 0 && default_address && instruction.condition == 0 &&
			       default_tail();
		case IrOpcode::VectorConvertFloatToIntegerMemory:
			return (instruction.width == 4 || instruction.width == 8) &&
			       IsRegister(instruction.destination) && instruction.source == IR_NO_REGISTER &&
			       instruction.source_width == 4 && instruction.source2 == IR_NO_REGISTER &&
			       instruction.vector_flags == 0 && IsValidAddress(instruction.address, true) &&
			       instruction.condition == 0 && default_tail();
		case IrOpcode::VectorConvertIntegerToFloatRegister:
			return instruction.width == 4 && IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) && IsRegister(instruction.source2) &&
			       (instruction.source_width == 4 || instruction.source_width == 8) &&
			       instruction.vector_flags == zero_upper && default_address &&
			       instruction.condition == 0 && default_tail();
		case IrOpcode::VectorConvertIntegerToFloatMemory:
			return instruction.width == 4 && IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) && instruction.source2 == IR_NO_REGISTER &&
			       (instruction.source_width == 4 || instruction.source_width == 8) &&
			       instruction.vector_flags == zero_upper && IsValidAddress(instruction.address, true) &&
			       instruction.condition == 0 && default_tail();
		case IrOpcode::VectorBinaryRegister:
			return IsRegister(instruction.destination) && IsRegister(instruction.source) &&
			       IsRegister(instruction.source2) && default_address && default_tail() &&
			       valid_binary_shape(false);
		case IrOpcode::VectorBinaryMemory:
			return IsRegister(instruction.destination) && IsRegister(instruction.source) &&
			       instruction.source2 == IR_NO_REGISTER && IsValidAddress(instruction.address, true) &&
			       default_tail() && valid_binary_shape(true);
		case IrOpcode::VectorHorizontalAddRegister:
			return instruction.width == 16 && instruction.source_width == 8 &&
			       IsRegister(instruction.destination) && IsRegister(instruction.source) &&
			       IsRegister(instruction.source2) && instruction.vector_flags == zero_upper &&
			       default_address && instruction.condition == 0 && default_tail();
		case IrOpcode::VectorUnpackLowDwordsMemory:
			return instruction.width == 16 && instruction.source_width == 4 &&
			       IsRegister(instruction.destination) && IsRegister(instruction.source) &&
			       instruction.source2 == IR_NO_REGISTER && instruction.vector_flags == zero_upper &&
			       IsValidAddress(instruction.address, true) && instruction.condition == 0 &&
			       default_tail();
		case IrOpcode::VectorMoveGpr64ToXmm:
			return instruction.width == 8 && IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) && instruction.source2 == IR_NO_REGISTER &&
			       instruction.source_width == 0 && instruction.vector_flags == 0 &&
			       default_address && instruction.condition == 0 && default_tail();
		case IrOpcode::VectorCompareRegister:
			return (instruction.width == 4 || instruction.width == 8) &&
			       IsRegister(instruction.destination) && IsRegister(instruction.source) &&
			       instruction.source2 == IR_NO_REGISTER && instruction.source_width == 0 &&
			       instruction.vector_flags == 0 && default_address && instruction.condition == 0 &&
			       default_tail();
		case IrOpcode::VectorCompareMemory:
			return (instruction.width == 4 || instruction.width == 8) &&
			       IsRegister(instruction.destination) && instruction.source == IR_NO_REGISTER &&
			       instruction.source2 == IR_NO_REGISTER && instruction.source_width == 0 &&
			       instruction.vector_flags == 0 && IsValidAddress(instruction.address, true) &&
			       instruction.condition == 0 && default_tail();
		case IrOpcode::CallIndirectRegister:
		case IrOpcode::JumpIndirectRegister:
			return instruction.width == 8 && instruction.destination == IR_NO_REGISTER &&
			       IsRegister(instruction.source) && instruction.source2 == IR_NO_REGISTER &&
			       instruction.source_width == 0 && instruction.vector_flags == 0 &&
			       default_address && instruction.condition == 0 && default_tail();
		case IrOpcode::CallIndirectMemory:
		case IrOpcode::JumpIndirectMemory:
			return instruction.width == 8 && no_registers && instruction.source_width == 0 &&
			       instruction.vector_flags == 0 && IsValidAddress(instruction.address, true) &&
			       instruction.condition == 0 && default_tail();
		case IrOpcode::RepeatMove:
		case IrOpcode::RepeatStore:
			return (instruction.width == 1 || instruction.width == 8) && no_registers &&
			       instruction.source_width == 0 && instruction.vector_flags == 0 &&
			       default_address && instruction.condition == 0 && default_tail();
		case IrOpcode::AtomicCompareExchangeMemory:
			return instruction.width == 8 && instruction.destination == IR_NO_REGISTER &&
			       IsRegister(instruction.source) && instruction.source2 == IR_NO_REGISTER &&
			       instruction.source_width == 0 && instruction.vector_flags == 0 &&
			       IsValidAddress(instruction.address, true) && instruction.condition == 0 &&
			       default_tail();
		default: return false;
	}
}

bool ValidateInstructionEagerCore(const IrInstruction& instruction)
{
	if (instruction.length == 0 || instruction.length > 15 || instruction.step_cost != 1 ||
	    instruction.rip > UINT64_MAX - instruction.length || instruction.effects != ExpectedEffects(instruction.opcode))
	{
		return false;
	}
	if (IsTypedHelperOpcode(instruction.opcode))
		return ValidateTypedHelperInstruction(instruction);

	const bool width8_to_64 = instruction.width == 1 || instruction.width == 2 ||
	                          instruction.width == 4 || instruction.width == 8;
	const bool width32_or_64 = instruction.width == 4 || instruction.width == 8;
	const bool no_registers = instruction.destination == IR_NO_REGISTER && instruction.source == IR_NO_REGISTER &&
	                          instruction.source2 == IR_NO_REGISTER;
	const bool default_address = IsDefaultAddress(instruction.address);
	const bool default_condition = instruction.condition == 0;
	const bool default_immediate = instruction.immediate == 0;
	const bool default_target = instruction.target == 0;
	const bool default_source_width = instruction.source_width == 0;
	const bool immediate_fits_width = instruction.width == 8 ||
	                                  (instruction.width == 4 && instruction.immediate <= UINT32_MAX) ||
	                                  (instruction.width == 2 && instruction.immediate <= UINT16_MAX) ||
	                                  (instruction.width == 1 && instruction.immediate <= UINT8_MAX);
	const uint64_t immediate_low32 = instruction.immediate & UINT32_MAX;
	const uint64_t sign_extended_immediate32 = (immediate_low32 & UINT64_C(0x80000000)) != 0
	                                               ? immediate_low32 | UINT64_C(0xffffffff00000000)
	                                               : immediate_low32;
	const bool store_immediate_is_normalized =
	    (instruction.width == 1 && instruction.immediate <= UINT8_MAX) ||
	    (instruction.width == 2 && instruction.immediate <= UINT16_MAX) ||
	    (instruction.width == 4 && instruction.immediate <= UINT32_MAX) ||
	    (instruction.width == 8 && instruction.immediate == sign_extended_immediate32);
	const bool vector_opcode = instruction.opcode == IrOpcode::VectorMoveRegister ||
	                           instruction.opcode == IrOpcode::VectorMoveScalarRegister ||
	                           instruction.opcode == IrOpcode::VectorLoad ||
	                           instruction.opcode == IrOpcode::VectorStore ||
	                           instruction.opcode == IrOpcode::VectorLoadScalar ||
	                           instruction.opcode == IrOpcode::VectorStoreScalar ||
	                           instruction.opcode == IrOpcode::VectorXorRegister ||
	                           instruction.opcode == IrOpcode::VectorXorMemory ||
	                           instruction.opcode == IrOpcode::VectorPermuteDwordsImmediate;
	const bool source2_opcode = vector_opcode || instruction.opcode == IrOpcode::BitTestRegister ||
	                            instruction.opcode == IrOpcode::TestRegister ||
	                            instruction.opcode == IrOpcode::BitExtractRegister ||
	                            instruction.opcode == IrOpcode::AndNotRegister ||
	                            instruction.opcode == IrOpcode::SignedMultiplyRegister;
	const uint8_t vector_flag_mask = IrVectorFlagMask(IrVectorFlag::ZeroUpper) |
	                                 IrVectorFlagMask(IrVectorFlag::RequireAlignment);
	if ((!source2_opcode && instruction.source2 != IR_NO_REGISTER) ||
	    (!vector_opcode && instruction.vector_flags != 0) ||
	    (instruction.vector_flags & ~vector_flag_mask) != 0)
	{
		return false;
	}

	switch (instruction.opcode)
	{
		case IrOpcode::Nop:
			return instruction.width == 0 && no_registers && default_address && default_condition && default_immediate &&
			       default_target && default_source_width;
		case IrOpcode::MoveRegister:
			return width8_to_64 && IsRegister(instruction.destination) && IsRegister(instruction.source) && default_address &&
			       default_condition && default_immediate && default_target && default_source_width;
		case IrOpcode::MoveImmediate:
			return width8_to_64 && IsRegister(instruction.destination) && instruction.source == IR_NO_REGISTER &&
			       default_address && default_condition && default_target && default_source_width &&
			       immediate_fits_width;
		case IrOpcode::Load:
			return width8_to_64 && IsRegister(instruction.destination) && instruction.source == IR_NO_REGISTER &&
			       IsValidAddress(instruction.address, true) && default_condition && default_immediate && default_target &&
			       default_source_width;
		case IrOpcode::AddRegisterMemory:
		case IrOpcode::SubRegisterMemory:
		case IrOpcode::CompareRegisterMemory:
			return width32_or_64 && IsRegister(instruction.destination) && instruction.source == IR_NO_REGISTER &&
			       IsValidAddress(instruction.address, true) && default_condition && default_immediate && default_target &&
			       default_source_width;
		case IrOpcode::OrRegisterMemory:
			return (instruction.width == 1 || instruction.width == 2) && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && IsValidAddress(instruction.address, true) &&
			       default_condition && default_immediate && default_target && default_source_width;
		case IrOpcode::AndRegisterMemory:
			return (instruction.width == 1 || width32_or_64) && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && IsValidAddress(instruction.address, true) &&
			       default_condition && default_immediate && default_target && default_source_width;
		case IrOpcode::XorRegisterMemory:
			return width32_or_64 && IsRegister(instruction.destination) && instruction.source == IR_NO_REGISTER &&
			       IsValidAddress(instruction.address, true) && default_condition && default_immediate && default_target &&
			       default_source_width;
		case IrOpcode::CompareMemoryRegister:
			return width32_or_64 && instruction.destination == IR_NO_REGISTER && IsRegister(instruction.source) &&
			       IsValidAddress(instruction.address, true) && default_condition && default_immediate && default_target &&
			       default_source_width;
		case IrOpcode::CompareMemoryImmediate:
			return width8_to_64 && no_registers && IsValidAddress(instruction.address, true) &&
			       default_condition && IsNormalizedImmediateForWidth(instruction.width, instruction.immediate) &&
			       default_target && default_source_width;
		case IrOpcode::TestMemoryImmediate:
			return instruction.width == 1 && no_registers && IsValidAddress(instruction.address, true) &&
			       default_condition && instruction.immediate <= UINT8_MAX && default_target && default_source_width;
		case IrOpcode::SignedMultiplyRegisterMemory:
			return width32_or_64 && IsRegister(instruction.destination) &&
			       instruction.source == instruction.destination && IsValidAddress(instruction.address, true) &&
			       default_condition && default_immediate && default_target && default_source_width;
		case IrOpcode::SignedMultiplyMemoryImmediate:
			return width32_or_64 && IsRegister(instruction.destination) && instruction.source == IR_NO_REGISTER &&
			       IsValidAddress(instruction.address, true) && default_condition &&
			       IsNormalizedImmediateForWidth(instruction.width, instruction.immediate) && default_target &&
			       default_source_width;
		case IrOpcode::AddMemoryRegister:
		case IrOpcode::SubtractMemoryRegister:
		case IrOpcode::AndMemoryRegister:
			return width32_or_64 && instruction.destination == IR_NO_REGISTER &&
			       IsRegister(instruction.source) && IsValidAddress(instruction.address, true) &&
			       default_condition && default_immediate && default_target && default_source_width;
		case IrOpcode::AndMemoryImmediate:
			return instruction.width == 1 && no_registers &&
			       IsValidAddress(instruction.address, true) && default_condition &&
			       instruction.immediate <= UINT8_MAX && default_target && default_source_width;
		case IrOpcode::IncrementMemory:
			return width32_or_64 && no_registers && IsValidAddress(instruction.address, true) &&
			       default_condition && default_immediate && default_target && default_source_width;
		case IrOpcode::SetConditionMemory:
			return instruction.width == 1 && no_registers &&
			       IsValidAddress(instruction.address, true) && instruction.condition <= 15 &&
			       default_immediate && default_target && default_source_width;
		case IrOpcode::Store:
		{
			const bool register_source = IsRegister(instruction.source) && default_immediate &&
			                             IsValidAddress(instruction.address, false);
			const bool immediate_source = instruction.source == IR_NO_REGISTER && store_immediate_is_normalized &&
			                              IsValidAddress(instruction.address, true);
			return width8_to_64 && instruction.destination == IR_NO_REGISTER &&
			       (register_source || immediate_source) && default_condition && default_target && default_source_width;
		}
		case IrOpcode::SubImmediate:
			return width32_or_64 && IsRegister(instruction.destination) && instruction.source == IR_NO_REGISTER &&
			       default_address && default_condition && default_target && default_source_width;
		case IrOpcode::AddImmediate:
			return (instruction.width == 1 || width32_or_64) && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && default_address && default_condition && default_target &&
			       default_source_width &&
			       (instruction.width != 1 || instruction.immediate <= UINT8_MAX);
		case IrOpcode::CompareImmediate:
			return (instruction.width == 2 || width32_or_64) && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && default_address && default_condition && default_target &&
			       default_source_width &&
			       (instruction.width != 2 || instruction.immediate <= UINT16_MAX);
		case IrOpcode::AddRegister:
		case IrOpcode::SubRegister:
		case IrOpcode::CompareRegister:
			return width32_or_64 && IsRegister(instruction.destination) && IsRegister(instruction.source) &&
			       default_address && default_condition && default_immediate && default_target && default_source_width;
		case IrOpcode::ShiftRightLogicalImmediate:
		{
			const bool valid_width = instruction.width == 1 || width32_or_64;
			const uint64_t maximum_count = instruction.width == 8 ? 63u : 31u;
			return valid_width && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && default_address && default_condition &&
			       instruction.immediate <= maximum_count && default_target && default_source_width;
		}
		case IrOpcode::ShiftLeftImmediate:
		{
			const uint64_t maximum_count = instruction.width == 8 ? 63u : 31u;
			const bool valid_condition =
			    default_condition ||
			    instruction.condition == IR_SHIFT_IMMEDIATE_ROTATE_LEFT;
			return width32_or_64 && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && default_address && valid_condition &&
			       instruction.immediate <= maximum_count && default_target && default_source_width;
		}
		case IrOpcode::ShiftRightArithmeticImmediate:
		{
			const uint64_t maximum_count = instruction.width == 8 ? 63u : 31u;
			return width32_or_64 && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && default_address && default_condition &&
			       instruction.immediate <= maximum_count && default_target && default_source_width;
		}
		case IrOpcode::ShiftLeftCl:
		case IrOpcode::ShiftRightLogicalCl:
			return width32_or_64 && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && default_address && default_condition &&
			       default_immediate && default_target && default_source_width;
		case IrOpcode::AndImmediate:
			return width8_to_64 && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && default_address && default_condition &&
			       IsNormalizedImmediateForWidth(instruction.width, instruction.immediate) &&
			       default_target && default_source_width;
		case IrOpcode::OrImmediate:
			return (instruction.width == 1 || width32_or_64) &&
			       IsRegister(instruction.destination) && instruction.source == IR_NO_REGISTER &&
			       default_address && default_condition &&
			       IsNormalizedImmediateForWidth(instruction.width, instruction.immediate) &&
			       default_target && default_source_width;
		case IrOpcode::XorImmediate:
			return width32_or_64 && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && default_address && default_condition &&
			       IsNormalizedImmediateForWidth(instruction.width, instruction.immediate) &&
			       default_target && default_source_width;
		case IrOpcode::BitTestRegister:
			return width32_or_64 && instruction.destination == IR_NO_REGISTER &&
			       IsRegister(instruction.source) && IsRegister(instruction.source2) &&
			       default_address && default_condition && default_immediate && default_target &&
			       default_source_width;
		case IrOpcode::BitTestImmediate:
			return width32_or_64 && instruction.destination == IR_NO_REGISTER &&
			       IsRegister(instruction.source) && default_address && default_condition &&
			       instruction.immediate <= (instruction.width == 8 ? 63u : 31u) &&
			       default_target && default_source_width;
		case IrOpcode::Increment:
		case IrOpcode::Decrement:
			return width32_or_64 && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && default_address && default_condition &&
			       default_immediate && default_target && default_source_width;
		case IrOpcode::SetCondition:
			return instruction.width == 1 && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && default_address &&
			       instruction.condition <= 0x0f && default_immediate && default_target &&
			       default_source_width;
		case IrOpcode::ConditionalMove:
			return width32_or_64 && IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) && default_address &&
			       instruction.condition <= 0x0f && default_immediate && default_target &&
			       default_source_width;
		case IrOpcode::ClearDirectionFlag:
			return instruction.width == 0 && no_registers && default_address &&
			       default_condition && default_immediate && default_target &&
			       default_source_width;
		case IrOpcode::PushRegister:
			return instruction.width == 8 && instruction.destination == IR_NO_REGISTER && IsRegister(instruction.source) &&
			       default_address && default_condition && default_immediate && default_target && default_source_width;
		case IrOpcode::PopRegister:
			return instruction.width == 8 && IsRegister(instruction.destination) && instruction.source == IR_NO_REGISTER &&
			       default_address && default_condition && default_immediate && default_target && default_source_width;
		case IrOpcode::BranchCondition:
			return instruction.width == 0 && no_registers && default_address && instruction.condition <= 0x0f &&
			       default_immediate && default_source_width;
		case IrOpcode::JumpImmediate:
		case IrOpcode::CallImmediate:
			return instruction.width == 0 && no_registers && default_address && default_condition && default_immediate &&
			       default_source_width;
		case IrOpcode::Return:
			// Return.immediate is the architectural RET imm16 stack adjustment. C3
			// naturally lowers with zero; C2 retains its unsigned 16-bit operand.
			return instruction.width == 8 && no_registers && default_address && default_condition &&
			       instruction.immediate <= UINT16_MAX &&
			       default_target && default_source_width;
		case IrOpcode::VectorMoveGpr32ToXmm:
			return instruction.width == 4 && IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) && instruction.source2 == IR_NO_REGISTER &&
			       instruction.vector_flags == 0 &&
			       default_address && default_condition && default_immediate && default_target &&
			       default_source_width;
		case IrOpcode::VectorPermuteDwordsImmediate:
			return instruction.width == 16 && IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) && instruction.source2 == IR_NO_REGISTER &&
			       instruction.vector_flags == IrVectorFlagMask(IrVectorFlag::ZeroUpper) &&
			       default_address && default_condition && instruction.immediate <= UINT8_MAX &&
			       default_target && default_source_width;
		case IrOpcode::VectorMoveRegister:
			return (instruction.width == 16 || instruction.width == 32) &&
			       IsRegister(instruction.destination) && IsRegister(instruction.source) &&
			       instruction.source2 == IR_NO_REGISTER && default_address && default_condition && default_immediate &&
			       default_target && default_source_width &&
			       (instruction.vector_flags & IrVectorFlagMask(IrVectorFlag::RequireAlignment)) == 0 &&
			       (instruction.width != 32 || instruction.vector_flags == 0);
		case IrOpcode::VectorMoveScalarRegister:
			return (instruction.width == 4 || instruction.width == 8) &&
			       IsRegister(instruction.destination) && IsRegister(instruction.source) &&
			       IsRegister(instruction.source2) && default_address && default_condition &&
			       default_immediate && default_target && default_source_width &&
			       instruction.vector_flags == IrVectorFlagMask(IrVectorFlag::ZeroUpper);
		case IrOpcode::VectorLoadScalar:
			return (instruction.width == 4 || instruction.width == 8) &&
			       IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER &&
			       instruction.source2 == IR_NO_REGISTER &&
			       IsValidAddress(instruction.address, true) && default_condition &&
			       default_immediate && default_target && default_source_width &&
			       instruction.vector_flags == 0;
		case IrOpcode::VectorStoreScalar:
			return (instruction.width == 4 || instruction.width == 8) &&
			       instruction.destination == IR_NO_REGISTER &&
			       IsRegister(instruction.source) &&
			       instruction.source2 == IR_NO_REGISTER &&
			       IsValidAddress(instruction.address, true) && default_condition &&
			       default_immediate && default_target && default_source_width &&
			       instruction.vector_flags == 0;
		case IrOpcode::VectorLoad:
			return (instruction.width == 16 || instruction.width == 32) &&
			       IsRegister(instruction.destination) && instruction.source == IR_NO_REGISTER &&
			       instruction.source2 == IR_NO_REGISTER && IsValidAddress(instruction.address, true) && default_condition &&
			       default_immediate && default_target && default_source_width &&
			       (instruction.width != 32 ||
			        (instruction.vector_flags & IrVectorFlagMask(IrVectorFlag::ZeroUpper)) == 0);
		case IrOpcode::VectorStore:
			return (instruction.width == 16 || instruction.width == 32) &&
			       instruction.destination == IR_NO_REGISTER && IsRegister(instruction.source) &&
			       instruction.source2 == IR_NO_REGISTER && IsValidAddress(instruction.address, true) && default_condition &&
			       default_immediate && default_target && default_source_width &&
			       (instruction.vector_flags & IrVectorFlagMask(IrVectorFlag::ZeroUpper)) == 0;
		case IrOpcode::VectorXorRegister:
			return (instruction.width == 16 || instruction.width == 32) &&
			       IsRegister(instruction.destination) && IsRegister(instruction.source) &&
			       IsRegister(instruction.source2) && default_address && default_condition && default_immediate &&
			       default_target && default_source_width &&
			       (instruction.vector_flags & IrVectorFlagMask(IrVectorFlag::RequireAlignment)) == 0 &&
			       (instruction.width != 32 || instruction.vector_flags == 0);
		case IrOpcode::VectorXorMemory:
			return (instruction.width == 16 || instruction.width == 32) &&
			       IsRegister(instruction.destination) && IsRegister(instruction.source) &&
			       instruction.source2 == IR_NO_REGISTER && IsValidAddress(instruction.address, true) && default_condition &&
			       default_immediate && default_target && default_source_width &&
			       (instruction.vector_flags & IrVectorFlagMask(IrVectorFlag::RequireAlignment)) == 0 &&
			       (instruction.width != 32 || instruction.vector_flags == 0);
		case IrOpcode::ZeroExtendRegister:
		case IrOpcode::SignExtendRegister:
		{
			const bool narrow_source = instruction.source_width == 1 || instruction.source_width == 2;
			const bool movsxd = instruction.opcode == IrOpcode::SignExtendRegister &&
			                    instruction.source_width == 4 && instruction.width == 8;
			return width32_or_64 && (narrow_source || movsxd) &&
			       IsRegister(instruction.destination) && IsRegister(instruction.source) &&
			       default_address && default_condition && default_immediate && default_target;
		}
		case IrOpcode::ZeroExtendMemory:
		case IrOpcode::SignExtendMemory:
		{
			const bool narrow_source = instruction.source_width == 1 || instruction.source_width == 2;
			const bool movsxd = instruction.opcode == IrOpcode::SignExtendMemory &&
			                    instruction.source_width == 4 && instruction.width == 8;
			return width32_or_64 && (narrow_source || movsxd) &&
			       IsRegister(instruction.destination) && instruction.source == IR_NO_REGISTER &&
			       IsValidAddress(instruction.address, true) && default_condition && default_immediate &&
			       default_target;
		}
		case IrOpcode::EffectiveAddress:
			return width32_or_64 && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && IsValidAddress(instruction.address, true) &&
			       default_condition && default_immediate && default_target && default_source_width;
		case IrOpcode::ByteSwapRegister:
			return width32_or_64 && IsRegister(instruction.destination) &&
			       instruction.source == instruction.destination && default_address && default_condition &&
			       default_immediate && default_target && default_source_width;
		case IrOpcode::BitwiseNot:
			return width32_or_64 && IsRegister(instruction.destination) &&
			       instruction.source == instruction.destination && default_address && default_condition &&
			       default_immediate && default_target && default_source_width;
		case IrOpcode::Negate:
			return (instruction.width == 2 || width32_or_64) && IsRegister(instruction.destination) &&
			       instruction.source == instruction.destination && default_address && default_condition &&
			       default_immediate && default_target && default_source_width;
		case IrOpcode::TestImmediate:
			return width8_to_64 && instruction.destination == IR_NO_REGISTER &&
			       IsRegister(instruction.source) && default_address &&
			       (instruction.condition == 0 ||
			        (instruction.condition == IR_TEST_IMMEDIATE_HIGH_BYTE &&
			         instruction.width == 1 && instruction.source < 4)) &&
			       default_target && default_source_width &&
			       IsNormalizedImmediateForWidth(instruction.width, instruction.immediate);
		case IrOpcode::AndRegister:
			return (instruction.width == 2 || instruction.width == 4 || instruction.width == 8) &&
			       IsRegister(instruction.destination) && IsRegister(instruction.source) &&
			       default_address && default_condition && default_immediate && default_target &&
			       default_source_width;
		case IrOpcode::OrRegister:
			return (instruction.width == 1 || instruction.width == 4 || instruction.width == 8) &&
			       IsRegister(instruction.destination) && IsRegister(instruction.source) &&
			       default_address && default_condition && default_immediate && default_target &&
			       default_source_width;
		case IrOpcode::XorRegister:
			return width32_or_64 && IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) && default_address && default_condition &&
			       default_immediate && default_target && default_source_width;
		case IrOpcode::TestRegister:
			return width8_to_64 && instruction.destination == IR_NO_REGISTER &&
			       IsRegister(instruction.source) && IsRegister(instruction.source2) &&
			       default_address && default_condition && default_immediate && default_target &&
			       default_source_width;
		case IrOpcode::SubtractWithBorrowRegister:
			return (instruction.width == 1 || instruction.width == 2) &&
			       IsRegister(instruction.destination) && IsRegister(instruction.source) &&
			       default_address && default_condition && default_immediate && default_target &&
			       default_source_width;
		case IrOpcode::BitExtractRegister:
			return instruction.width == 4 && IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) && IsRegister(instruction.source2) &&
			       default_address && default_condition && default_immediate && default_target &&
			       default_source_width;
		case IrOpcode::AndNotRegister:
			return width32_or_64 && IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) && IsRegister(instruction.source2) &&
			       default_address && default_condition && default_immediate && default_target &&
			       default_source_width;
		case IrOpcode::SignedMultiplyRegister:
			return width32_or_64 && IsRegister(instruction.destination) &&
			       instruction.source == instruction.destination && IsRegister(instruction.source2) &&
			       default_address && default_condition && default_immediate && default_target &&
			       default_source_width;
		case IrOpcode::SignedMultiplyImmediate:
			return width32_or_64 && IsRegister(instruction.destination) && IsRegister(instruction.source) &&
			       instruction.source2 == IR_NO_REGISTER && default_address && default_condition &&
			       IsNormalizedImmediateForWidth(instruction.width, instruction.immediate) &&
			       default_target && default_source_width;
		case IrOpcode::Count: return false;
		default: return false;
	}
	return false;
}

bool ValidateInstructionBranchLocalCore(const IrInstruction& instruction)
{
	// Keep the proof-wide checks before opcode dispatch. Everything below this
	// point is intentionally evaluated only by the opcode that owns the field.
	if (instruction.length == 0 || instruction.length > 15 ||
	    instruction.step_cost != 1 ||
	    instruction.rip > UINT64_MAX - instruction.length ||
	    instruction.effects != ExpectedEffects(instruction.opcode))
	{
		return false;
	}
	if (IsTypedHelperOpcode(instruction.opcode))
		return ValidateTypedHelperInstruction(instruction);

	const auto width8_to_64 = [&instruction]()
	{
		return instruction.width == 1 || instruction.width == 2 ||
		       instruction.width == 4 || instruction.width == 8;
	};
	const auto width32_or_64 = [&instruction]()
	{
		return instruction.width == 4 || instruction.width == 8;
	};
	const auto no_auxiliary = [&instruction]()
	{
		return instruction.source2 == IR_NO_REGISTER &&
		       instruction.vector_flags == 0;
	};
	const auto default_address = [&instruction]()
	{
		return IsDefaultAddress(instruction.address);
	};
	const auto only_vector_flags = [&instruction](uint8_t allowed)
	{
		return (instruction.vector_flags &
		        static_cast<uint8_t>(~allowed)) == 0;
	};

	switch (instruction.opcode)
	{
		case IrOpcode::Nop:
			return instruction.width == 0 &&
			       instruction.destination == IR_NO_REGISTER &&
			       instruction.source == IR_NO_REGISTER &&
			       instruction.source2 == IR_NO_REGISTER &&
			       instruction.vector_flags == 0 && default_address() &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::MoveRegister:
			return width8_to_64() && IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) && no_auxiliary() &&
			       default_address() && instruction.condition == 0 &&
			       instruction.immediate == 0 && instruction.target == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::MoveImmediate:
		{
			const bool immediate_fits_width =
			    instruction.width == 8 ||
			    (instruction.width == 4 && instruction.immediate <= UINT32_MAX) ||
			    (instruction.width == 2 && instruction.immediate <= UINT16_MAX) ||
			    (instruction.width == 1 && instruction.immediate <= UINT8_MAX);
			return width8_to_64() && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       default_address() && instruction.condition == 0 &&
			       instruction.target == 0 && instruction.source_width == 0 &&
			       immediate_fits_width;
		}
		case IrOpcode::Load:
			return width8_to_64() && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       IsValidAddress(instruction.address, true) &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::AddRegisterMemory:
		case IrOpcode::SubRegisterMemory:
		case IrOpcode::CompareRegisterMemory:
			return width32_or_64() && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       IsValidAddress(instruction.address, true) &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::OrRegisterMemory:
			return (instruction.width == 1 || instruction.width == 2) &&
			       IsRegister(instruction.destination) && instruction.source == IR_NO_REGISTER &&
			       no_auxiliary() && IsValidAddress(instruction.address, true) &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::AndRegisterMemory:
			return (instruction.width == 1 || width32_or_64()) &&
			       IsRegister(instruction.destination) && instruction.source == IR_NO_REGISTER &&
			       no_auxiliary() && IsValidAddress(instruction.address, true) &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::XorRegisterMemory:
			return width32_or_64() && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       IsValidAddress(instruction.address, true) &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::CompareMemoryRegister:
			return width32_or_64() && instruction.destination == IR_NO_REGISTER &&
			       IsRegister(instruction.source) && no_auxiliary() &&
			       IsValidAddress(instruction.address, true) &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::CompareMemoryImmediate:
			return width8_to_64() && instruction.destination == IR_NO_REGISTER &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       IsValidAddress(instruction.address, true) && instruction.condition == 0 &&
			       IsNormalizedImmediateForWidth(instruction.width, instruction.immediate) &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::TestMemoryImmediate:
			return instruction.width == 1 && instruction.destination == IR_NO_REGISTER &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       IsValidAddress(instruction.address, true) && instruction.condition == 0 &&
			       instruction.immediate <= UINT8_MAX && instruction.target == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::SignedMultiplyRegisterMemory:
			return width32_or_64() && IsRegister(instruction.destination) &&
			       instruction.source == instruction.destination && no_auxiliary() &&
			       IsValidAddress(instruction.address, true) && instruction.condition == 0 &&
			       instruction.immediate == 0 && instruction.target == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::SignedMultiplyMemoryImmediate:
			return width32_or_64() && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       IsValidAddress(instruction.address, true) && instruction.condition == 0 &&
			       IsNormalizedImmediateForWidth(instruction.width, instruction.immediate) &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::AddMemoryRegister:
		case IrOpcode::SubtractMemoryRegister:
		case IrOpcode::AndMemoryRegister:
			return width32_or_64() && instruction.destination == IR_NO_REGISTER &&
			       IsRegister(instruction.source) && no_auxiliary() &&
			       IsValidAddress(instruction.address, true) && instruction.condition == 0 &&
			       instruction.immediate == 0 && instruction.target == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::AndMemoryImmediate:
			return instruction.width == 1 && instruction.destination == IR_NO_REGISTER &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       IsValidAddress(instruction.address, true) && instruction.condition == 0 &&
			       instruction.immediate <= UINT8_MAX && instruction.target == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::IncrementMemory:
			return width32_or_64() && instruction.destination == IR_NO_REGISTER &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       IsValidAddress(instruction.address, true) && instruction.condition == 0 &&
			       instruction.immediate == 0 && instruction.target == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::SetConditionMemory:
			return instruction.width == 1 && instruction.destination == IR_NO_REGISTER &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       IsValidAddress(instruction.address, true) && instruction.condition <= 15 &&
			       instruction.immediate == 0 && instruction.target == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::Store:
		{
			const uint64_t immediate_low32 = instruction.immediate & UINT32_MAX;
			const uint64_t sign_extended_immediate32 =
			    (immediate_low32 & UINT64_C(0x80000000)) != 0
			        ? immediate_low32 | UINT64_C(0xffffffff00000000)
			        : immediate_low32;
			const bool store_immediate_is_normalized =
			    (instruction.width == 1 && instruction.immediate <= UINT8_MAX) ||
			    (instruction.width == 2 && instruction.immediate <= UINT16_MAX) ||
			    (instruction.width == 4 && instruction.immediate <= UINT32_MAX) ||
			    (instruction.width == 8 &&
			     instruction.immediate == sign_extended_immediate32);
			const bool register_source =
			    IsRegister(instruction.source) && instruction.immediate == 0 &&
			    IsValidAddress(instruction.address, false);
			const bool immediate_source =
			    instruction.source == IR_NO_REGISTER &&
			    store_immediate_is_normalized &&
			    IsValidAddress(instruction.address, true);
			return width8_to_64() &&
			       instruction.destination == IR_NO_REGISTER && no_auxiliary() &&
			       (register_source || immediate_source) &&
			       instruction.condition == 0 && instruction.target == 0 &&
			       instruction.source_width == 0;
		}
		case IrOpcode::SubImmediate:
			return width32_or_64() && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       default_address() && instruction.condition == 0 &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::AddImmediate:
			return (instruction.width == 1 || width32_or_64()) &&
			       IsRegister(instruction.destination) && instruction.source == IR_NO_REGISTER &&
			       no_auxiliary() && default_address() && instruction.condition == 0 &&
			       (instruction.width != 1 || instruction.immediate <= UINT8_MAX) &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::CompareImmediate:
			return (instruction.width == 2 || width32_or_64()) &&
			       IsRegister(instruction.destination) && instruction.source == IR_NO_REGISTER &&
			       no_auxiliary() && default_address() && instruction.condition == 0 &&
			       (instruction.width != 2 || instruction.immediate <= UINT16_MAX) &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::AddRegister:
		case IrOpcode::SubRegister:
		case IrOpcode::CompareRegister:
			return width32_or_64() && IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) && no_auxiliary() &&
			       default_address() && instruction.condition == 0 &&
			       instruction.immediate == 0 && instruction.target == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::ShiftRightLogicalImmediate:
		{
			const bool valid_width = instruction.width == 1 || width32_or_64();
			const uint64_t maximum_count = instruction.width == 8 ? 63u : 31u;
			return valid_width && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       default_address() && instruction.condition == 0 &&
			       instruction.immediate <= maximum_count &&
			       instruction.target == 0 && instruction.source_width == 0;
		}
		case IrOpcode::ShiftLeftImmediate:
		{
			const uint64_t maximum_count = instruction.width == 8 ? 63u : 31u;
			const bool valid_condition =
			    instruction.condition == 0 ||
			    instruction.condition == IR_SHIFT_IMMEDIATE_ROTATE_LEFT;
			return width32_or_64() && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       default_address() && valid_condition &&
			       instruction.immediate <= maximum_count &&
			       instruction.target == 0 && instruction.source_width == 0;
		}
		case IrOpcode::ShiftRightArithmeticImmediate:
		{
			const uint64_t maximum_count = instruction.width == 8 ? 63u : 31u;
			return width32_or_64() && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       default_address() && instruction.condition == 0 &&
			       instruction.immediate <= maximum_count &&
			       instruction.target == 0 && instruction.source_width == 0;
		}
		case IrOpcode::ShiftLeftCl:
		case IrOpcode::ShiftRightLogicalCl:
			return width32_or_64() && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       default_address() && instruction.condition == 0 &&
			       instruction.immediate == 0 && instruction.target == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::AndImmediate:
			return width8_to_64() && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       default_address() && instruction.condition == 0 &&
			       IsNormalizedImmediateForWidth(instruction.width, instruction.immediate) &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::OrImmediate:
			return (instruction.width == 1 || width32_or_64()) &&
			       IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       default_address() && instruction.condition == 0 &&
			       IsNormalizedImmediateForWidth(instruction.width, instruction.immediate) &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::XorImmediate:
			return width32_or_64() && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       default_address() && instruction.condition == 0 &&
			       IsNormalizedImmediateForWidth(instruction.width, instruction.immediate) &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::BitTestRegister:
			return width32_or_64() &&
			       instruction.destination == IR_NO_REGISTER &&
			       IsRegister(instruction.source) &&
			       IsRegister(instruction.source2) &&
			       instruction.vector_flags == 0 && default_address() &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::BitTestImmediate:
			return width32_or_64() &&
			       instruction.destination == IR_NO_REGISTER &&
			       IsRegister(instruction.source) && no_auxiliary() &&
			       default_address() && instruction.condition == 0 &&
			       instruction.immediate <= (instruction.width == 8 ? 63u : 31u) &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::Increment:
		case IrOpcode::Decrement:
			return width32_or_64() && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       default_address() && instruction.condition == 0 &&
			       instruction.immediate == 0 && instruction.target == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::SetCondition:
			return instruction.width == 1 &&
			       IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       default_address() && instruction.condition <= 0x0f &&
			       instruction.immediate == 0 && instruction.target == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::ConditionalMove:
			return width32_or_64() && IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) && no_auxiliary() && default_address() &&
			       instruction.condition <= 0x0f && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::ClearDirectionFlag:
			return instruction.width == 0 &&
			       instruction.destination == IR_NO_REGISTER &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       default_address() && instruction.condition == 0 &&
			       instruction.immediate == 0 && instruction.target == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::PushRegister:
			return instruction.width == 8 &&
			       instruction.destination == IR_NO_REGISTER &&
			       IsRegister(instruction.source) && no_auxiliary() &&
			       default_address() && instruction.condition == 0 &&
			       instruction.immediate == 0 && instruction.target == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::PopRegister:
			return instruction.width == 8 &&
			       IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       default_address() && instruction.condition == 0 &&
			       instruction.immediate == 0 && instruction.target == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::BranchCondition:
			return instruction.width == 0 &&
			       instruction.destination == IR_NO_REGISTER &&
			       instruction.source == IR_NO_REGISTER &&
			       instruction.source2 == IR_NO_REGISTER &&
			       instruction.vector_flags == 0 && default_address() &&
			       instruction.condition <= 0x0f &&
			       instruction.immediate == 0 && instruction.source_width == 0;
		case IrOpcode::JumpImmediate:
		case IrOpcode::CallImmediate:
			return instruction.width == 0 &&
			       instruction.destination == IR_NO_REGISTER &&
			       instruction.source == IR_NO_REGISTER &&
			       instruction.source2 == IR_NO_REGISTER &&
			       instruction.vector_flags == 0 && default_address() &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::Return:
			return instruction.width == 8 &&
			       instruction.destination == IR_NO_REGISTER &&
			       instruction.source == IR_NO_REGISTER &&
			       instruction.source2 == IR_NO_REGISTER &&
			       instruction.vector_flags == 0 && default_address() &&
			       instruction.condition == 0 &&
			       instruction.immediate <= UINT16_MAX &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::VectorMoveGpr32ToXmm:
			return instruction.width == 4 &&
			       IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) &&
			       instruction.source2 == IR_NO_REGISTER &&
			       instruction.vector_flags == 0 && default_address() &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::VectorPermuteDwordsImmediate:
			return instruction.width == 16 &&
			       IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) &&
			       instruction.source2 == IR_NO_REGISTER &&
			       instruction.vector_flags ==
			           IrVectorFlagMask(IrVectorFlag::ZeroUpper) &&
			       default_address() && instruction.condition == 0 &&
			       instruction.immediate <= UINT8_MAX &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::VectorMoveRegister:
			return (instruction.width == 16 || instruction.width == 32) &&
			       IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) &&
			       instruction.source2 == IR_NO_REGISTER && default_address() &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0 &&
			       only_vector_flags(
			           instruction.width == 16
			               ? IrVectorFlagMask(IrVectorFlag::ZeroUpper)
			               : 0);
		case IrOpcode::VectorMoveScalarRegister:
			return (instruction.width == 4 || instruction.width == 8) &&
			       IsRegister(instruction.destination) && IsRegister(instruction.source) &&
			       IsRegister(instruction.source2) && default_address() &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0 &&
			       instruction.vector_flags == IrVectorFlagMask(IrVectorFlag::ZeroUpper);
		case IrOpcode::VectorLoadScalar:
			return (instruction.width == 4 || instruction.width == 8) &&
			       IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER &&
			       instruction.source2 == IR_NO_REGISTER &&
			       instruction.vector_flags == 0 &&
			       IsValidAddress(instruction.address, true) &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::VectorStoreScalar:
			return (instruction.width == 4 || instruction.width == 8) &&
			       instruction.destination == IR_NO_REGISTER &&
			       IsRegister(instruction.source) &&
			       instruction.source2 == IR_NO_REGISTER &&
			       instruction.vector_flags == 0 &&
			       IsValidAddress(instruction.address, true) &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::VectorLoad:
			return (instruction.width == 16 || instruction.width == 32) &&
			       IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER &&
			       instruction.source2 == IR_NO_REGISTER &&
			       IsValidAddress(instruction.address, true) &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0 &&
			       only_vector_flags(
			           (instruction.width == 16
			                ? IrVectorFlagMask(IrVectorFlag::ZeroUpper)
			                : 0) |
			           IrVectorFlagMask(IrVectorFlag::RequireAlignment));
		case IrOpcode::VectorStore:
			return (instruction.width == 16 || instruction.width == 32) &&
			       instruction.destination == IR_NO_REGISTER &&
			       IsRegister(instruction.source) &&
			       instruction.source2 == IR_NO_REGISTER &&
			       IsValidAddress(instruction.address, true) &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0 &&
			       only_vector_flags(
			           IrVectorFlagMask(IrVectorFlag::RequireAlignment));
		case IrOpcode::VectorXorRegister:
			return (instruction.width == 16 || instruction.width == 32) &&
			       IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) &&
			       IsRegister(instruction.source2) && default_address() &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0 &&
			       only_vector_flags(
			           instruction.width == 16
			               ? IrVectorFlagMask(IrVectorFlag::ZeroUpper)
			               : 0);
		case IrOpcode::VectorXorMemory:
			return (instruction.width == 16 || instruction.width == 32) &&
			       IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) &&
			       instruction.source2 == IR_NO_REGISTER &&
			       IsValidAddress(instruction.address, true) &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0 &&
			       only_vector_flags(
			           instruction.width == 16
			               ? IrVectorFlagMask(IrVectorFlag::ZeroUpper)
			               : 0);
		case IrOpcode::ZeroExtendRegister:
		case IrOpcode::SignExtendRegister:
		{
			const bool narrow_source =
			    instruction.source_width == 1 || instruction.source_width == 2;
			const bool movsxd =
			    instruction.opcode == IrOpcode::SignExtendRegister &&
			    instruction.source_width == 4 && instruction.width == 8;
			return width32_or_64() && (narrow_source || movsxd) &&
			       IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) && no_auxiliary() &&
			       default_address() && instruction.condition == 0 &&
			       instruction.immediate == 0 && instruction.target == 0;
		}
		case IrOpcode::ZeroExtendMemory:
		case IrOpcode::SignExtendMemory:
		{
			const bool narrow_source =
			    instruction.source_width == 1 || instruction.source_width == 2;
			const bool movsxd =
			    instruction.opcode == IrOpcode::SignExtendMemory &&
			    instruction.source_width == 4 && instruction.width == 8;
			return width32_or_64() && (narrow_source || movsxd) &&
			       IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       IsValidAddress(instruction.address, true) &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0;
		}
		case IrOpcode::EffectiveAddress:
			return width32_or_64() && IsRegister(instruction.destination) &&
			       instruction.source == IR_NO_REGISTER && no_auxiliary() &&
			       IsValidAddress(instruction.address, true) &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::ByteSwapRegister:
		case IrOpcode::BitwiseNot:
			return width32_or_64() && IsRegister(instruction.destination) &&
			       instruction.source == instruction.destination &&
			       no_auxiliary() && default_address() &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::TestImmediate:
			return width8_to_64() &&
			       instruction.destination == IR_NO_REGISTER &&
			       IsRegister(instruction.source) && no_auxiliary() &&
			       default_address() &&
			       (instruction.condition == 0 ||
			        (instruction.condition == IR_TEST_IMMEDIATE_HIGH_BYTE &&
			         instruction.width == 1 && instruction.source < 4)) &&
			       instruction.target == 0 && instruction.source_width == 0 &&
			       IsNormalizedImmediateForWidth(instruction.width, instruction.immediate);
		case IrOpcode::AndRegister:
			return (instruction.width == 2 || instruction.width == 4 ||
			        instruction.width == 8) &&
			       IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) && no_auxiliary() &&
			       default_address() && instruction.condition == 0 &&
			       instruction.immediate == 0 && instruction.target == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::Negate:
			return (instruction.width == 2 || width32_or_64()) &&
			       IsRegister(instruction.destination) &&
			       instruction.source == instruction.destination && no_auxiliary() &&
			       default_address() && instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::OrRegister:
			return (instruction.width == 1 || instruction.width == 4 ||
			        instruction.width == 8) &&
			       IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) && no_auxiliary() &&
			       default_address() && instruction.condition == 0 &&
			       instruction.immediate == 0 && instruction.target == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::XorRegister:
			return width32_or_64() &&
			       IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) && no_auxiliary() &&
			       default_address() && instruction.condition == 0 &&
			       instruction.immediate == 0 && instruction.target == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::TestRegister:
			return width8_to_64() &&
			       instruction.destination == IR_NO_REGISTER &&
			       IsRegister(instruction.source) &&
			       IsRegister(instruction.source2) &&
			       instruction.vector_flags == 0 && default_address() &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::SubtractWithBorrowRegister:
			return (instruction.width == 1 || instruction.width == 2) &&
			       IsRegister(instruction.destination) && IsRegister(instruction.source) &&
			       no_auxiliary() && default_address() && instruction.condition == 0 &&
			       instruction.immediate == 0 && instruction.target == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::BitExtractRegister:
			return instruction.width == 4 && IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) && IsRegister(instruction.source2) &&
			       instruction.vector_flags == 0 && default_address() &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::AndNotRegister:
			return width32_or_64() && IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) && IsRegister(instruction.source2) &&
			       instruction.vector_flags == 0 && default_address() &&
			       instruction.condition == 0 && instruction.immediate == 0 &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::SignedMultiplyRegister:
			return width32_or_64() && IsRegister(instruction.destination) &&
			       instruction.source == instruction.destination && IsRegister(instruction.source2) &&
			       instruction.vector_flags == 0 && default_address() && instruction.condition == 0 &&
			       instruction.immediate == 0 && instruction.target == 0 &&
			       instruction.source_width == 0;
		case IrOpcode::SignedMultiplyImmediate:
			return width32_or_64() && IsRegister(instruction.destination) &&
			       IsRegister(instruction.source) && no_auxiliary() && default_address() &&
			       instruction.condition == 0 &&
			       IsNormalizedImmediateForWidth(instruction.width, instruction.immediate) &&
			       instruction.target == 0 && instruction.source_width == 0;
		case IrOpcode::Count: return false;
		default: return false;
	}
	return false;
}

void CountInstructionValidationForTest()
{
#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || defined(LUMEN_CPU_X64_TEST)
	g_ir_verification_test_counters.instruction_validations++;
#endif
}

bool ValidateInstruction(const IrInstruction& instruction)
{
	CountInstructionValidationForTest();
	return ValidateInstructionEagerCore(instruction);
}

template <bool BranchLocal, bool CountForTest = true>
bool ValidateLoweredInstruction(const IrInstruction& instruction)
{
	if constexpr (CountForTest) CountInstructionValidationForTest();
	if constexpr (BranchLocal)
		return ValidateInstructionBranchLocalCore(instruction);
	return ValidateInstructionEagerCore(instruction);
}

bool IsDirectFallthroughOpcode(IrOpcode opcode)
{
	switch (opcode)
	{
		case IrOpcode::Nop:
		case IrOpcode::MoveRegister:
		case IrOpcode::MoveImmediate:
		case IrOpcode::AddImmediate:
		case IrOpcode::SubImmediate:
		case IrOpcode::CompareImmediate:
		case IrOpcode::AddRegister:
		case IrOpcode::SubRegister:
		case IrOpcode::CompareRegister:
		case IrOpcode::ShiftLeftImmediate:
		case IrOpcode::ShiftLeftCl:
		case IrOpcode::ShiftRightLogicalImmediate:
		case IrOpcode::ShiftRightLogicalCl:
		case IrOpcode::ShiftRightArithmeticImmediate:
		case IrOpcode::AndImmediate:
		case IrOpcode::OrImmediate:
		case IrOpcode::XorImmediate:
		case IrOpcode::BitTestRegister:
		case IrOpcode::BitTestImmediate:
		case IrOpcode::Increment:
		case IrOpcode::Decrement:
		case IrOpcode::SetCondition:
		case IrOpcode::ConditionalMove:
		case IrOpcode::ClearDirectionFlag:
		case IrOpcode::BitwiseNot:
		case IrOpcode::Negate:
		case IrOpcode::TestImmediate:
		case IrOpcode::AndRegister:
		case IrOpcode::OrRegister:
		case IrOpcode::XorRegister:
		case IrOpcode::TestRegister:
		case IrOpcode::SubtractWithBorrowRegister:
		case IrOpcode::BitExtractRegister:
		case IrOpcode::AndNotRegister:
		case IrOpcode::SignedMultiplyRegister:
		case IrOpcode::SignedMultiplyImmediate:
		case IrOpcode::VectorMoveGpr32ToXmm:
		case IrOpcode::VectorPermuteDwordsImmediate:
		case IrOpcode::VectorMoveRegister:
		case IrOpcode::VectorMoveScalarRegister:
		case IrOpcode::VectorXorRegister:
		case IrOpcode::ZeroExtendRegister:
		case IrOpcode::SignExtendRegister:
		case IrOpcode::EffectiveAddress:
		case IrOpcode::ByteSwapRegister: return true;
		default: return false;
	}
}

bool HasExistingEligibility(const IrBlock& block)
{
	if (!block.has_terminator) return false;
	const uint32_t required = SemanticMask(SemanticClass::GuestMemory) |
	                          SemanticMask(SemanticClass::StackControl);
	return (block.semantic_mask & required) == required;
}

bool HasDirectFallthroughEligibility(const IrBlock& block)
{
	if (block.has_terminator) return false;
	constexpr uint8_t forbidden_effects =
	    IrEffectMask(IrEffect::ReadGuestMemory) | IrEffectMask(IrEffect::WriteGuestMemory) |
	    IrEffectMask(IrEffect::Stack) | IrEffectMask(IrEffect::Terminator) |
	    IrEffectMask(IrEffect::MayFault) | IrEffectMask(IrEffect::HostCall);
	constexpr uint32_t allowed_semantics =
	    SemanticMask(SemanticClass::Integer) | SemanticMask(SemanticClass::Flags) |
	    SemanticMask(SemanticClass::SimdFloatingPoint);
	if ((block.semantic_mask & ~allowed_semantics) != 0) return false;
	for (size_t index = 0; index < block.instruction_count; index++)
	{
		const auto& instruction = block.instructions[index];
		if ((instruction.effects & forbidden_effects) != 0 ||
		    !IsDirectFallthroughOpcode(instruction.opcode))
		{
			return false;
		}
	}
	return true;
}

bool HasDirectTerminalEligibility(const IrBlock& block)
{
	if (!block.has_terminator || block.instruction_count == 0) return false;
	constexpr uint8_t forbidden_linear_effects =
	    IrEffectMask(IrEffect::ReadGuestMemory) | IrEffectMask(IrEffect::WriteGuestMemory) |
	    IrEffectMask(IrEffect::Stack) | IrEffectMask(IrEffect::Terminator) |
	    IrEffectMask(IrEffect::MayFault) | IrEffectMask(IrEffect::HostCall);
	constexpr uint32_t allowed_semantics =
	    SemanticMask(SemanticClass::Integer) | SemanticMask(SemanticClass::Flags) |
	    SemanticMask(SemanticClass::StackControl) |
	    SemanticMask(SemanticClass::SimdFloatingPoint) |
	    SemanticMask(SemanticClass::PreciseExit);
	if ((block.semantic_mask & ~allowed_semantics) != 0) return false;

	for (size_t index = 0; index + 1 < block.instruction_count; index++)
	{
		const auto& instruction = block.instructions[index];
		if ((instruction.effects & forbidden_linear_effects) != 0 ||
		    !IsDirectFallthroughOpcode(instruction.opcode))
		{
			return false;
		}
	}
	const IrOpcode terminator = block.instructions[block.instruction_count - 1u].opcode;
	return terminator == IrOpcode::BranchCondition || terminator == IrOpcode::JumpImmediate;
}

bool HasDirectBasicBlockEligibility(const IrBlock& block)
{
	return HasDirectFallthroughEligibility(block) || HasDirectTerminalEligibility(block);
}

bool IsProductionScalarDataMemoryOpcode(IrOpcode opcode)
{
	switch (opcode)
	{
		case IrOpcode::Load:
		case IrOpcode::Store:
		case IrOpcode::ZeroExtendMemory:
		case IrOpcode::SignExtendMemory:
		case IrOpcode::AddRegisterMemory:
		case IrOpcode::SubRegisterMemory:
		case IrOpcode::OrRegisterMemory:
		case IrOpcode::AndRegisterMemory:
		case IrOpcode::XorRegisterMemory:
		case IrOpcode::CompareRegisterMemory:
		case IrOpcode::CompareMemoryRegister:
		case IrOpcode::CompareMemoryImmediate:
		case IrOpcode::TestMemoryImmediate:
		case IrOpcode::SignedMultiplyRegisterMemory:
		case IrOpcode::SignedMultiplyMemoryImmediate:
		case IrOpcode::AddMemoryRegister:
		case IrOpcode::SubtractMemoryRegister:
		case IrOpcode::AndMemoryRegister:
		case IrOpcode::AndMemoryImmediate:
		case IrOpcode::IncrementMemory:
		case IrOpcode::SetConditionMemory:
		case IrOpcode::VectorLoadScalar:
		case IrOpcode::VectorStoreScalar: return true;
		default: return false;
	}
}

bool HasProductionScalarMemoryFallthroughEligibility(const IrBlock& block)
{
	if (block.instruction_count == 0 ||
	    block.instruction_count > MAX_IR_INSTRUCTIONS || block.has_terminator)
	{
		return false;
	}
	constexpr uint32_t allowed_semantics =
	    SemanticMask(SemanticClass::Integer) |
	    SemanticMask(SemanticClass::Flags) |
	    SemanticMask(SemanticClass::GuestMemory) |
	    SemanticMask(SemanticClass::SimdFloatingPoint) |
	    SemanticMask(SemanticClass::PreciseExit) |
	    SemanticMask(SemanticClass::SelfModifyingCode);
	if ((block.semantic_mask & SemanticMask(SemanticClass::GuestMemory)) == 0 ||
	    (block.semantic_mask & ~allowed_semantics) != 0)
	{
		return false;
	}

	constexpr uint8_t memory_effects =
	    IrEffectMask(IrEffect::ReadGuestMemory) |
	    IrEffectMask(IrEffect::WriteGuestMemory);
	constexpr uint8_t forbidden_memory_effects =
	    IrEffectMask(IrEffect::Stack) |
	    IrEffectMask(IrEffect::Terminator) |
	    IrEffectMask(IrEffect::HostCall);
	constexpr uint8_t forbidden_register_effects =
	    memory_effects | forbidden_memory_effects |
	    IrEffectMask(IrEffect::MayFault);
	bool has_scalar_memory = false;
	uint32_t semantic_mask = 0;
	for (size_t index = 0; index < block.instruction_count; index++)
	{
		const auto& instruction = block.instructions[index];
		semantic_mask |= InstructionSemanticMask(instruction.opcode);
		if (IsProductionScalarDataMemoryOpcode(instruction.opcode))
		{
			if (instruction.effects != ExpectedEffects(instruction.opcode) ||
			    (instruction.effects & memory_effects) == 0 ||
			    (instruction.effects & forbidden_memory_effects) != 0)
			{
				return false;
			}
			has_scalar_memory = true;
			continue;
		}
		if (!IsDirectFallthroughOpcode(instruction.opcode) ||
		    instruction.effects != ExpectedEffects(instruction.opcode) ||
		    (instruction.effects & forbidden_register_effects) != 0)
		{
			return false;
		}
	}
	return has_scalar_memory && block.semantic_mask == semantic_mask;
}

bool IsDirectBasicBlockOpcode(IrOpcode opcode);

bool HasVerifiedHelperEligibility(const IrBlock& block)
{
	if (block.instruction_count == 0) return false;
	bool has_helper = false;
	for (size_t index = 0; index < block.instruction_count; index++)
	{
		const auto& instruction = block.instructions[index];
		const bool helper = IsTypedHelperOpcode(instruction.opcode);
		if (!helper && !IsDirectBasicBlockOpcode(instruction.opcode)) return false;
		if (HasEffect(instruction, IrEffect::Terminator) && index + 1 != block.instruction_count)
			return false;
		has_helper |= helper;
	}
	const bool terminal_tail = HasEffect(
	    block.instructions[block.instruction_count - 1u], IrEffect::Terminator);
	return has_helper && terminal_tail == block.has_terminator;
}

constexpr uint8_t VERIFIED_IR_EXISTING = 1u << 0u;
constexpr uint8_t VERIFIED_IR_DIRECT_FALLTHROUGH = 1u << 1u;
constexpr uint8_t VERIFIED_IR_DIRECT_BASIC_BLOCK = 1u << 2u;
constexpr uint8_t VERIFIED_IR_HELPER_BASIC_BLOCK = 1u << 3u;
constexpr uint8_t VERIFIED_IR_CONTAINS_TYPED_HELPER = 1u << 4u;
constexpr uint8_t VERIFIED_IR_PRODUCTION_SCALAR_MEMORY_FALLTHROUGH = 1u << 5u;

bool ContainsTypedHelper(const IrBlock& block)
{
	for (size_t index = 0; index < block.instruction_count; index++)
	{
		if (IsTypedHelperOpcode(block.instructions[index].opcode)) return true;
	}
	return false;
}

bool ContainsPacked256Instruction(const IrBlock& block)
{
	for (size_t index = 0; index < block.instruction_count; index++)
	{
		const auto& instruction = block.instructions[index];
		if (instruction.width != 32) continue;
		switch (instruction.opcode)
		{
			case IrOpcode::VectorMoveRegister:
			case IrOpcode::VectorLoad:
			case IrOpcode::VectorStore:
			case IrOpcode::VectorXorRegister:
			case IrOpcode::VectorXorMemory: return true;
			default: break;
		}
	}
	return false;
}

uint8_t ClassifyVerifiedIrEligibility(const IrBlock& block)
{
	uint8_t result = 0;
	if (HasExistingEligibility(block)) result |= VERIFIED_IR_EXISTING;
	const bool direct_fallthrough = HasDirectFallthroughEligibility(block);
	if (direct_fallthrough) result |= VERIFIED_IR_DIRECT_FALLTHROUGH;
	if (direct_fallthrough || HasDirectTerminalEligibility(block))
		result |= VERIFIED_IR_DIRECT_BASIC_BLOCK;
	if (HasVerifiedHelperEligibility(block)) result |= VERIFIED_IR_HELPER_BASIC_BLOCK;
	if (ContainsTypedHelper(block)) result |= VERIFIED_IR_CONTAINS_TYPED_HELPER;
	if (HasProductionScalarMemoryFallthroughEligibility(block))
		result |= VERIFIED_IR_PRODUCTION_SCALAR_MEMORY_FALLTHROUGH;
	return result;
}

Status VerifyLoweredIrStructure(const IrBlock& block)
{
#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || defined(LUMEN_CPU_X64_TEST)
	g_ir_verification_test_counters.structural_block_verifications++;
#endif
	if (block.instruction_count == 0 ||
	    block.instruction_count > MAX_IR_INSTRUCTIONS ||
	    block.rip != block.instructions[0].rip)
	{
		return Status::InvalidIr;
	}

	uint64_t expected_rip = block.rip;
	uint32_t semantic_mask = 0;
	for (size_t index = 0; index < block.instruction_count; index++)
	{
		const auto& instruction = block.instructions[index];
		if (instruction.rip != expected_rip) return Status::InvalidIr;
		const bool terminator = HasEffect(instruction, IrEffect::Terminator);
		if (terminator !=
		    (block.has_terminator && index + 1 == block.instruction_count))
		{
			return Status::InvalidIr;
		}
		expected_rip = instruction.rip + instruction.length;
		semantic_mask |= InstructionSemanticMask(instruction.opcode);
	}
	return block.fallthrough_rip == expected_rip &&
	               block.semantic_mask == semantic_mask
	           ? Status::Ok
	           : Status::InvalidIr;
}

bool IsDirectBasicBlockOpcode(IrOpcode opcode)
{
	return IsDirectFallthroughOpcode(opcode) || opcode == IrOpcode::BranchCondition ||
	       opcode == IrOpcode::JumpImmediate;
}

uint32_t ClassifyVerifiedInstructionForDiagnostics(
	const IrInstruction& instruction)
{
	uint32_t flags = 0;
	const auto set = [&flags](IrInstructionDiagnosticFlag flag)
	{
		flags |= IrInstructionDiagnosticMask(flag);
	};
	const bool reads_memory = HasEffect(instruction, IrEffect::ReadGuestMemory);
	const bool writes_memory = HasEffect(instruction, IrEffect::WriteGuestMemory);
	const bool stack = HasEffect(instruction, IrEffect::Stack);
	const bool terminator = HasEffect(instruction, IrEffect::Terminator);
	const bool typed_helper = IsTypedHelperOpcode(instruction.opcode);
	const bool direct_terminator =
	    instruction.opcode == IrOpcode::BranchCondition ||
	    instruction.opcode == IrOpcode::JumpImmediate;

	if (IsDirectBasicBlockOpcode(instruction.opcode))
		set(IrInstructionDiagnosticFlag::ProductionDirect);
	if (HasEffect(instruction, IrEffect::ReadFlags))
		set(IrInstructionDiagnosticFlag::ReadsRflags);
	if (HasEffect(instruction, IrEffect::WriteFlags))
		set(IrInstructionDiagnosticFlag::WritesRflags);
	if (reads_memory) set(IrInstructionDiagnosticFlag::ReadsGuestMemory);
	if (writes_memory) set(IrInstructionDiagnosticFlag::WritesGuestMemory);
	if (reads_memory || writes_memory)
		set(IrInstructionDiagnosticFlag::GuestMemory);
	if (terminator) set(IrInstructionDiagnosticFlag::Terminator);
	if (direct_terminator)
		set(IrInstructionDiagnosticFlag::DirectTerminator);
	if (HasEffect(instruction, IrEffect::MayFault))
		set(IrInstructionDiagnosticFlag::MayFault);
	if (HasEffect(instruction, IrEffect::HostCall))
		set(IrInstructionDiagnosticFlag::HostCall);
	if (stack) set(IrInstructionDiagnosticFlag::Stack);

	if (!typed_helper) return flags;
	set(IrInstructionDiagnosticFlag::TypedHelper);
	constexpr uint8_t register_only_forbidden_effects =
	    IrEffectMask(IrEffect::ReadGuestMemory) |
	    IrEffectMask(IrEffect::WriteGuestMemory) |
	    IrEffectMask(IrEffect::Stack) |
	    IrEffectMask(IrEffect::Terminator) |
	    IrEffectMask(IrEffect::MayFault);
	if (IsTypedHelperRegisterOnlyOpcode(instruction.opcode) &&
	    (instruction.effects & register_only_forbidden_effects) == 0)
	{
		set(IrInstructionDiagnosticFlag::TypedHelperRegisterOnly);
	}
	if (reads_memory || writes_memory)
		set(IrInstructionDiagnosticFlag::TypedHelperMemory);
	if (stack) set(IrInstructionDiagnosticFlag::TypedHelperStack);
	if (instruction.opcode == IrOpcode::AtomicCompareExchangeMemory)
		set(IrInstructionDiagnosticFlag::TypedHelperAtomic);
	if (instruction.opcode == IrOpcode::RepeatMove ||
	    instruction.opcode == IrOpcode::RepeatStore)
	{
		set(IrInstructionDiagnosticFlag::TypedHelperRepeat);
	}
	if (instruction.opcode == IrOpcode::CallIndirectRegister ||
	    instruction.opcode == IrOpcode::CallIndirectMemory ||
	    instruction.opcode == IrOpcode::JumpIndirectRegister ||
	    instruction.opcode == IrOpcode::JumpIndirectMemory)
	{
		set(IrInstructionDiagnosticFlag::TypedHelperIndirectControl);
	}
	return flags;
}

enum class BlockGprOperandField : uint8_t
{
	Destination,
	Source,
	Source2,
	AddressBase,
	AddressIndex,
	ImplicitRcx,
};

struct BlockGprOperandModel
{
	uint8_t guest = IR_NO_REGISTER;
	BlockGprOperandField field = BlockGprOperandField::Destination;
	uint8_t width = 0;
	uint8_t baseline_loads = 0;
	uint8_t baseline_stores = 0;
	bool reads = false;
	bool writes = false;
	bool conditional_write = false;
};

struct BlockGprInstructionModel
{
	std::array<BlockGprOperandModel, 3> operands {};
	uint8_t operand_count = 0;
	bool shift_cl = false;
};

bool AddBlockGprOperand(
	BlockGprInstructionModel* model, uint8_t guest, BlockGprOperandField field,
	uint8_t width, uint8_t baseline_loads, uint8_t baseline_stores,
	bool reads, bool writes, bool conditional_write = false)
{
	if (model == nullptr || !IsRegister(guest) || model->operand_count >= model->operands.size() ||
	    (width != 1 && width != 2 && width != 4 && width != 8))
	{
		return false;
	}
	model->operands[model->operand_count++] = {
	    guest, field, width, baseline_loads, baseline_stores,
	    reads, writes, conditional_write};
	return true;
}

bool BuildBlockGprInstructionModel(
	const IrInstruction& instruction, BlockGprInstructionModel* model)
{
	if (model == nullptr) return false;
	*model = {};
	const auto add = [model](uint8_t guest, BlockGprOperandField field,
	                         uint8_t width, uint8_t loads, uint8_t stores,
	                         bool reads, bool writes,
	                         bool conditional_write = false) {
		return AddBlockGprOperand(
		    model, guest, field, width, loads, stores, reads, writes,
		    conditional_write);
	};

	switch (instruction.opcode)
	{
		case IrOpcode::Nop:
		case IrOpcode::ClearDirectionFlag:
		case IrOpcode::BranchCondition:
		case IrOpcode::JumpImmediate:
		case IrOpcode::VectorPermuteDwordsImmediate:
		case IrOpcode::VectorMoveRegister:
		case IrOpcode::VectorMoveScalarRegister:
		case IrOpcode::VectorXorRegister: return true;
		case IrOpcode::MoveRegister:
		{
			const bool partial = instruction.width == 1 || instruction.width == 2;
			return add(instruction.source, BlockGprOperandField::Source,
			           instruction.width, 1, 0, true, false) &&
		       add(instruction.destination, BlockGprOperandField::Destination,
			           instruction.width, partial ? 1 : 0, 1, partial, true);
		}
		case IrOpcode::MoveImmediate:
		{
			const bool partial = instruction.width == 1 || instruction.width == 2;
			return add(instruction.destination, BlockGprOperandField::Destination,
			           instruction.width, partial ? 1 : 0, 1, partial, true);
		}
		case IrOpcode::ZeroExtendRegister:
		case IrOpcode::SignExtendRegister:
			return add(instruction.source, BlockGprOperandField::Source,
			           instruction.source_width, 1, 0, true, false) &&
		       add(instruction.destination, BlockGprOperandField::Destination,
			           instruction.width, 0, 1, false, true);
		case IrOpcode::EffectiveAddress:
			if (instruction.address.base != IR_NO_REGISTER &&
			    !add(instruction.address.base, BlockGprOperandField::AddressBase,
			         8, 1, 0, true, false))
			{
				return false;
			}
			if (instruction.address.index != IR_NO_REGISTER &&
			    !add(instruction.address.index, BlockGprOperandField::AddressIndex,
			         8, 1, 0, true, false))
			{
				return false;
			}
			return add(instruction.destination, BlockGprOperandField::Destination,
			           instruction.width, 0, 1, false, true);
		case IrOpcode::ByteSwapRegister:
		case IrOpcode::BitwiseNot:
			return add(instruction.destination, BlockGprOperandField::Destination,
			           instruction.width, 1, 1, true, true);
		case IrOpcode::Negate:
			return add(instruction.destination, BlockGprOperandField::Destination,
			           instruction.width, instruction.width == 2 ? 2 : 1, 1,
			           true, true);
		case IrOpcode::AddImmediate:
		case IrOpcode::SubImmediate:
		case IrOpcode::CompareImmediate:
		{
			const bool writes = instruction.opcode != IrOpcode::CompareImmediate;
			const uint8_t loads = instruction.width == 1 ? 2 : 1;
			return add(instruction.destination, BlockGprOperandField::Destination,
			           instruction.width, loads, writes ? 1 : 0, true, writes);
		}
		case IrOpcode::AddRegister:
		case IrOpcode::SubRegister:
		case IrOpcode::CompareRegister:
		{
			const bool writes = instruction.opcode != IrOpcode::CompareRegister;
			return add(instruction.destination, BlockGprOperandField::Destination,
			           instruction.width, 1, writes ? 1 : 0, true, writes) &&
		       add(instruction.source, BlockGprOperandField::Source,
			           instruction.width, 1, 0, true, false);
		}
		case IrOpcode::ShiftLeftImmediate:
		case IrOpcode::ShiftRightLogicalImmediate:
		case IrOpcode::ShiftRightArithmeticImmediate:
			if (instruction.immediate == 0) return true;
			return add(instruction.destination, BlockGprOperandField::Destination,
			           instruction.width, 1, 1, true, true);
		case IrOpcode::ShiftLeftCl:
		case IrOpcode::ShiftRightLogicalCl:
			model->shift_cl = true;
			return add(1, BlockGprOperandField::ImplicitRcx, 4, 1, 0, true, false) &&
		       add(instruction.destination, BlockGprOperandField::Destination,
			           instruction.width, 1, 1, true, true, true);
		case IrOpcode::AndImmediate:
		case IrOpcode::OrImmediate:
		case IrOpcode::XorImmediate:
			return add(instruction.destination, BlockGprOperandField::Destination,
			           instruction.width, 1, 1, true, true);
		case IrOpcode::BitTestRegister:
			return add(instruction.source, BlockGprOperandField::Source,
			           instruction.width, 1, 0, true, false) &&
		       add(instruction.source2, BlockGprOperandField::Source2,
			           instruction.width, 1, 0, true, false);
		case IrOpcode::BitTestImmediate:
			return add(instruction.source, BlockGprOperandField::Source,
			           instruction.width, 1, 0, true, false);
		case IrOpcode::Increment:
		case IrOpcode::Decrement:
			return add(instruction.destination, BlockGprOperandField::Destination,
			           instruction.width, 1, 1, true, true);
		case IrOpcode::SetCondition:
			return add(instruction.destination, BlockGprOperandField::Destination,
			           1, 1, 1, true, true);
		case IrOpcode::ConditionalMove:
			return add(instruction.destination, BlockGprOperandField::Destination,
			           instruction.width, 1, 1, true, true) &&
		       add(instruction.source, BlockGprOperandField::Source,
			           instruction.width, 1, 0, true, false);
		case IrOpcode::TestImmediate:
			if (instruction.condition != 0 &&
			    (instruction.condition != IR_TEST_IMMEDIATE_HIGH_BYTE ||
			     instruction.width != 1 || instruction.source >= 4))
			{
				return false;
			}
			return add(instruction.source, BlockGprOperandField::Source,
			           instruction.width, 1, 0, true, false);
		case IrOpcode::AndRegister:
		case IrOpcode::OrRegister:
		case IrOpcode::XorRegister:
			return add(instruction.destination, BlockGprOperandField::Destination,
			           instruction.width, 1, 1, true, true) &&
		       add(instruction.source, BlockGprOperandField::Source,
			           instruction.width, 1, 0, true, false);
		case IrOpcode::TestRegister:
			return add(instruction.source, BlockGprOperandField::Source,
			           instruction.width, 1, 0, true, false) &&
		       add(instruction.source2, BlockGprOperandField::Source2,
			           instruction.width, 1, 0, true, false);
		case IrOpcode::SubtractWithBorrowRegister:
			return add(instruction.destination, BlockGprOperandField::Destination,
			           instruction.width, 2, 1, true, true) &&
		       add(instruction.source, BlockGprOperandField::Source,
			           instruction.width, 1, 0, true, false);
		case IrOpcode::BitExtractRegister:
			return add(instruction.source, BlockGprOperandField::Source,
			           4, 1, 0, true, false) &&
		       add(instruction.source2, BlockGprOperandField::Source2,
			           4, 1, 0, true, false) &&
		       add(instruction.destination, BlockGprOperandField::Destination,
			           4, 0, 1, false, true);
		case IrOpcode::AndNotRegister:
			return add(instruction.source, BlockGprOperandField::Source,
			           instruction.width, 1, 0, true, false) &&
		       add(instruction.source2, BlockGprOperandField::Source2,
			           instruction.width, 1, 0, true, false) &&
		       add(instruction.destination, BlockGprOperandField::Destination,
			           instruction.width, 0, 1, false, true);
		case IrOpcode::SignedMultiplyRegister:
			return add(instruction.source, BlockGprOperandField::Source,
			           instruction.width, 1, 0, true, false) &&
		       add(instruction.source2, BlockGprOperandField::Source2,
			           instruction.width, 1, 0, true, false) &&
		       add(instruction.destination, BlockGprOperandField::Destination,
			           instruction.width, 0, 1, false, true);
		case IrOpcode::SignedMultiplyImmediate:
			return add(instruction.source, BlockGprOperandField::Source,
			           instruction.width, 1, 0, true, false) &&
		       add(instruction.destination, BlockGprOperandField::Destination,
			           instruction.width, 0, 1, false, true);
		case IrOpcode::VectorMoveGpr32ToXmm:
			return add(instruction.source, BlockGprOperandField::Source,
			           4, 1, 0, true, false);
		default: return false;
	}
}

uint16_t BlockGprCandidatePrepareWords(
	const IrInstruction& instruction, uint8_t guest)
{
	switch (instruction.opcode)
	{
		case IrOpcode::EffectiveAddress:
			return instruction.address.base == guest ? 1 : 0;
		case IrOpcode::MoveRegister:
			return instruction.width == 4 && instruction.source == guest &&
		               instruction.destination != guest
		           ? 1
		           : 0;
		case IrOpcode::ConditionalMove:
			return instruction.width == 4 &&
		               (instruction.destination == guest || instruction.source == guest)
		           ? 1
		           : 0;
		case IrOpcode::VectorMoveGpr32ToXmm:
			return instruction.source == guest ? 1 : 0;
		default: return 0;
	}
}

uint16_t BlockGprCandidateCommitWords(
	const IrInstruction& instruction, uint8_t guest)
{
	if ((instruction.opcode == IrOpcode::ShiftLeftImmediate ||
	     instruction.opcode == IrOpcode::ShiftRightLogicalImmediate ||
	     instruction.opcode == IrOpcode::ShiftRightArithmeticImmediate) &&
	    instruction.immediate == 0)
	{
		return 0;
	}
	if (instruction.destination != guest ||
	    (instruction.width != 4 && instruction.width != 8))
	{
		return 0;
	}
	switch (instruction.opcode)
	{
		case IrOpcode::MoveRegister:
			return instruction.width == 8 && instruction.source == guest ? 0 : 1;
		case IrOpcode::MoveImmediate:
		case IrOpcode::ZeroExtendRegister:
		case IrOpcode::SignExtendRegister:
		case IrOpcode::EffectiveAddress:
		case IrOpcode::ByteSwapRegister:
		case IrOpcode::BitwiseNot:
		case IrOpcode::AndImmediate:
		case IrOpcode::OrImmediate:
		case IrOpcode::XorImmediate:
		case IrOpcode::AndRegister:
		case IrOpcode::OrRegister:
		case IrOpcode::XorRegister:
		case IrOpcode::ConditionalMove:
		case IrOpcode::AndNotRegister: return 0;
		case IrOpcode::AddImmediate:
		case IrOpcode::SubImmediate:
		case IrOpcode::AddRegister:
		case IrOpcode::SubRegister:
		case IrOpcode::Negate:
		case IrOpcode::ShiftLeftImmediate:
		case IrOpcode::ShiftRightLogicalImmediate:
		case IrOpcode::ShiftRightArithmeticImmediate:
		case IrOpcode::Increment:
		case IrOpcode::Decrement:
		case IrOpcode::BitExtractRegister:
		case IrOpcode::SignedMultiplyRegister:
		case IrOpcode::SignedMultiplyImmediate: return 1;
		default: return 0;
	}
}

bool BetterBlockGprCachePlan(
	const BlockGprCachePlan& candidate, const BlockGprCachePlan& best)
{
	if (!best.eligible) return true;
	if (candidate.eliminated_access_floor != best.eliminated_access_floor)
		return candidate.eliminated_access_floor > best.eliminated_access_floor;
	if (candidate.eliminated_word_floor != best.eliminated_word_floor)
		return candidate.eliminated_word_floor > best.eliminated_word_floor;
	if (candidate.selected_conditional_write != best.selected_conditional_write)
		return !candidate.selected_conditional_write;
	if (candidate.selected_alias != best.selected_alias)
		return !candidate.selected_alias;
	return candidate.guest_register < best.guest_register;
}

bool AnalyzeVerifiedBlockGprCachePlan(
	const IrBlock& block, BlockGprCachePlan* output)
{
	if (output == nullptr) return false;
	std::array<BlockGprInstructionModel, MAX_IR_INSTRUCTIONS> models {};
	bool has_gpr_access = false;
	bool block_has_partial_hazard = false;
	bool block_has_shift_cl_hazard = false;
	for (size_t index = 0; index < block.instruction_count; index++)
	{
		if (!BuildBlockGprInstructionModel(block.instructions[index], &models[index]))
			return false;
		block_has_shift_cl_hazard |= models[index].shift_cl;
		for (size_t operand = 0; operand < models[index].operand_count; operand++)
		{
			has_gpr_access = true;
			const uint8_t width = models[index].operands[operand].width;
			block_has_partial_hazard |= width == 1 || width == 2;
		}
	}

	BlockGprCachePlan best {};
	best.has_gpr_access = has_gpr_access;
	best.block_has_partial_hazard = block_has_partial_hazard;
	best.block_has_shift_cl_hazard = block_has_shift_cl_hazard;
	for (uint8_t guest = 0; guest < 16; guest++)
	{
		BlockGprCachePlan candidate {};
		candidate.has_gpr_access = has_gpr_access;
		candidate.block_has_partial_hazard = block_has_partial_hazard;
		candidate.block_has_shift_cl_hazard = block_has_shift_cl_hazard;
		candidate.guest_register = guest;
		bool available = false;
		bool selected_read = false;
		bool selected_write = false;
		for (size_t index = 0; index < block.instruction_count; index++)
		{
			const auto& instruction = block.instructions[index];
			const auto& model = models[index];
			bool instruction_read = false;
			bool instruction_write = false;
			bool instruction_full_unconditional_write = false;
			uint8_t matching_fields = 0;
			for (size_t operand = 0; operand < model.operand_count; operand++)
			{
				const auto& role = model.operands[operand];
				if (role.guest != guest) continue;
				matching_fields++;
				candidate.baseline_loads += role.baseline_loads;
				candidate.baseline_stores += role.baseline_stores;
				instruction_read |= role.reads;
				instruction_write |= role.writes;
				instruction_full_unconditional_write |=
				    role.writes && !role.conditional_write && role.width >= 4;
				candidate.selected_partial_width |= role.width == 1 || role.width == 2;
				candidate.selected_conditional_write |= role.conditional_write;
			}
			candidate.selected_alias |= matching_fields >= 2;
			candidate.selected_shift_cl |=
			    model.shift_cl &&
			    (guest == 1 || instruction.destination == guest);
			if (instruction_read && !available)
			{
				candidate.candidate_initial_loads = 1;
				available = true;
			}
			if (instruction_full_unconditional_write) available = true;
			selected_read |= instruction_read;
			selected_write |= instruction_write;
			candidate.candidate_prepare_words +=
			    BlockGprCandidatePrepareWords(instruction, guest);
			candidate.candidate_commit_words +=
			    BlockGprCandidateCommitWords(instruction, guest);
		}

		candidate.baseline_accesses =
		    candidate.baseline_loads + candidate.baseline_stores;
		candidate.candidate_final_stores = selected_write ? 1 : 0;
		candidate.candidate_accesses =
		    candidate.candidate_initial_loads + candidate.candidate_final_stores;
		candidate.candidate_cache_words =
		    candidate.candidate_accesses + candidate.candidate_prepare_words +
		    candidate.candidate_commit_words;
		candidate.selected_read_only = selected_read && !selected_write;
		candidate.selected_write_only = !selected_read && selected_write;
		candidate.selected_readwrite = selected_read && selected_write;
		candidate.selected_lazy_init =
		    selected_write && candidate.candidate_initial_loads == 0;
		const bool touched = selected_read || selected_write;
		candidate.eligible = touched && !candidate.selected_partial_width &&
		                     !candidate.selected_shift_cl &&
		                     candidate.baseline_accesses > candidate.candidate_accesses &&
		                     candidate.baseline_accesses > candidate.candidate_cache_words;
		if (!candidate.eligible) continue;
		candidate.eliminated_access_floor =
		    candidate.baseline_accesses - candidate.candidate_accesses;
		candidate.eliminated_word_floor =
		    candidate.baseline_accesses - candidate.candidate_cache_words;
		if (BetterBlockGprCachePlan(candidate, best)) best = candidate;
	}
	*output = best;
	return true;
}

struct BlockRflagsCachePlan
{
	bool eligible = false;
	bool possibly_writes = false;
};

bool AnalyzeVerifiedBlockRflagsCachePlan(
	const IrBlock& block, BlockRflagsCachePlan* output)
{
	if (output == nullptr || block.instruction_count == 0 ||
	    block.instruction_count > MAX_IR_INSTRUCTIONS)
	{
		return false;
	}
	BlockRflagsCachePlan plan {};
	uint16_t baseline_accesses = 0;
	bool touches = false;
	constexpr uint8_t read_flags = IrEffectMask(IrEffect::ReadFlags);
	constexpr uint8_t write_flags = IrEffectMask(IrEffect::WriteFlags);
	constexpr uint8_t terminator = IrEffectMask(IrEffect::Terminator);
	auto effects_are = [](const IrInstruction& instruction, uint8_t expected)
	{
		return instruction.effects == expected;
	};
	auto record_writer = [&](uint16_t accesses)
	{
		touches = true;
		plan.possibly_writes = true;
		baseline_accesses = static_cast<uint16_t>(baseline_accesses + accesses);
	};
	for (size_t index = 0; index < block.instruction_count; index++)
	{
		const auto& instruction = block.instructions[index];
		switch (instruction.opcode)
		{
			case IrOpcode::Nop:
			case IrOpcode::MoveRegister:
			case IrOpcode::MoveImmediate:
			case IrOpcode::VectorMoveRegister:
			case IrOpcode::VectorXorRegister:
			case IrOpcode::ZeroExtendRegister:
			case IrOpcode::SignExtendRegister:
			case IrOpcode::EffectiveAddress:
			case IrOpcode::ByteSwapRegister:
			case IrOpcode::BitwiseNot:
			case IrOpcode::VectorMoveGpr32ToXmm:
			case IrOpcode::VectorPermuteDwordsImmediate:
			case IrOpcode::VectorMoveScalarRegister:
				if (!effects_are(instruction, 0)) return false;
				break;
			case IrOpcode::JumpImmediate:
				if (!effects_are(instruction, terminator)) return false;
				break;
			case IrOpcode::AddImmediate:
			case IrOpcode::SubImmediate:
			case IrOpcode::CompareImmediate:
			case IrOpcode::AddRegister:
			case IrOpcode::SubRegister:
			case IrOpcode::CompareRegister:
			case IrOpcode::Negate:
			case IrOpcode::TestImmediate:
			case IrOpcode::AndRegister:
			case IrOpcode::OrRegister:
			case IrOpcode::XorRegister:
			case IrOpcode::TestRegister:
				if (!effects_are(instruction, write_flags)) return false;
				record_writer(2);
				break;
			case IrOpcode::AndImmediate:
			case IrOpcode::OrImmediate:
			case IrOpcode::XorImmediate:
			case IrOpcode::BitTestRegister:
			case IrOpcode::BitTestImmediate:
			case IrOpcode::Increment:
			case IrOpcode::Decrement:
			case IrOpcode::BitExtractRegister:
			case IrOpcode::AndNotRegister:
			case IrOpcode::SignedMultiplyRegister:
			case IrOpcode::SignedMultiplyImmediate:
			case IrOpcode::ClearDirectionFlag:
				if (!effects_are(instruction, read_flags | write_flags)) return false;
				record_writer(2);
				break;
			case IrOpcode::SubtractWithBorrowRegister:
				if (!effects_are(instruction, read_flags | write_flags)) return false;
				record_writer(3);
				break;
			case IrOpcode::ShiftLeftImmediate:
			case IrOpcode::ShiftRightLogicalImmediate:
			case IrOpcode::ShiftRightArithmeticImmediate:
				if (!effects_are(instruction, read_flags | write_flags)) return false;
				if (static_cast<uint8_t>(instruction.immediate) != 0) record_writer(2);
				break;
			case IrOpcode::ShiftLeftCl:
			case IrOpcode::ShiftRightLogicalCl:
				if (!effects_are(instruction, read_flags | write_flags)) return false;
				// Runtime count zero skips the update, but a selected cache still
				// flushes its unchanged architectural value at the segment boundary.
				record_writer(0);
				break;
			case IrOpcode::SetCondition:
			case IrOpcode::ConditionalMove:
				if (!effects_are(instruction, read_flags)) return false;
				touches = true;
				baseline_accesses++;
				break;
			case IrOpcode::BranchCondition:
				if (!effects_are(instruction, read_flags | terminator)) return false;
				touches = true;
				baseline_accesses++;
				break;
			default: return false;
		}
	}
	const uint16_t candidate_accesses =
	    static_cast<uint16_t>(touches ? 1u + (plan.possibly_writes ? 1u : 0u) : 0u);
	plan.eligible = touches && baseline_accesses > candidate_accesses;
	*output = plan;
	return true;
}

bool NormalizeAddress(const X64Fast::FastOp& operation, bool allow_absolute, IrAddress* output)
{
	if (output == nullptr || operation.scale > 3) return false;
	IrAddress address {};
	if (operation.base == IR_NO_REGISTER)
	{
		if (!allow_absolute || operation.index != IR_NO_REGISTER || operation.scale != 0 || operation.displacement != 0)
		{
			return false;
		}
		address.absolute = operation.target;
	} else
	{
		if (!IsRegister(operation.base) ||
		    (operation.index != IR_NO_REGISTER && !IsRegister(operation.index)) || operation.target != 0)
		{
			return false;
		}
		address.base         = operation.base;
		address.index        = operation.index;
		address.scale        = operation.scale;
		address.displacement = operation.displacement;
		if (address.index == IR_NO_REGISTER && address.scale != 0) return false;
	}
	*output = address;
	return true;
}

template <bool BranchLocal, bool CountForTest = true>
Status LowerInstructionImpl(
	const X64Fast::FastOp& operation, IrInstruction* output,
	bool packed256_enabled)
{
	if (operation.kind == X64Fast::FastOpKind::CompareNoFlags ||
	    operation.kind == X64Fast::FastOpKind::TestNoFlags)
	{
		const auto original =
		    static_cast<X64Fast::FastOpKind>(operation.condition);
		const bool valid_compare =
		    operation.kind == X64Fast::FastOpKind::CompareNoFlags &&
		    (original == X64Fast::FastOpKind::CmpReg32 ||
		     original == X64Fast::FastOpKind::CmpReg64 ||
		     original == X64Fast::FastOpKind::CmpReg16Imm ||
		     original == X64Fast::FastOpKind::CmpReg32Imm ||
		     original == X64Fast::FastOpKind::CmpReg64Imm);
		const bool valid_test =
		    operation.kind == X64Fast::FastOpKind::TestNoFlags &&
		    (original == X64Fast::FastOpKind::TestReg16 ||
		     original == X64Fast::FastOpKind::TestReg8 ||
		     original == X64Fast::FastOpKind::TestReg32 ||
		     original == X64Fast::FastOpKind::TestReg64 ||
		     original == X64Fast::FastOpKind::TestReg8Imm ||
		     original == X64Fast::FastOpKind::TestReg16Imm ||
		     original == X64Fast::FastOpKind::TestReg32Imm ||
		     original == X64Fast::FastOpKind::TestReg64Imm);
		if (!valid_compare && !valid_test) return Status::InvalidArgument;
		X64Fast::FastOp normalized = operation;
		normalized.kind = original;
		normalized.condition = 0;
		return LowerInstructionImpl<BranchLocal, CountForTest>(
		    normalized, output, packed256_enabled);
	}
	const X64Fast::FastOpKind normalized_kind =
	    X64Fast::NormalizeDeadRflagsFastOpKind(operation.kind);
	if (normalized_kind != operation.kind)
	{
		X64Fast::FastOp normalized = operation;
		normalized.kind = normalized_kind;
		return LowerInstructionImpl<BranchLocal, CountForTest>(
		    normalized, output, packed256_enabled);
	}
	if (output == nullptr || operation.length == 0 || operation.length > 15 ||
	    operation.rip > UINT64_MAX - operation.length)
	{
		return Status::InvalidArgument;
	}

	IrInstruction instruction {};
	instruction.rip    = operation.rip;
	instruction.length = operation.length;
	switch (operation.kind)
	{
		case X64Fast::FastOpKind::Nop: instruction.opcode = IrOpcode::Nop; break;
		case X64Fast::FastOpKind::MovReg8:
		case X64Fast::FastOpKind::MovReg16:
		case X64Fast::FastOpKind::MovReg32:
		case X64Fast::FastOpKind::MovReg64:
			instruction.opcode      = IrOpcode::MoveRegister;
			instruction.width       = operation.kind == X64Fast::FastOpKind::MovReg8 ? 1 :
			                          (operation.kind == X64Fast::FastOpKind::MovReg16 ? 2 :
			                           (operation.kind == X64Fast::FastOpKind::MovReg64 ? 8 : 4));
			instruction.destination = operation.src;
			instruction.source      = operation.dst;
			break;
		case X64Fast::FastOpKind::MovImmReg8:
		case X64Fast::FastOpKind::MovImmReg16:
		case X64Fast::FastOpKind::MovImmReg32:
		case X64Fast::FastOpKind::MovImmReg64:
			instruction.opcode      = IrOpcode::MoveImmediate;
			instruction.width       = operation.kind == X64Fast::FastOpKind::MovImmReg8 ? 1 :
			                          (operation.kind == X64Fast::FastOpKind::MovImmReg16 ? 2 :
			                           (operation.kind == X64Fast::FastOpKind::MovImmReg64 ? 8 : 4));
			instruction.destination = operation.dst;
			instruction.immediate = instruction.width == 8 ? operation.target :
			                        (instruction.width == 4 ? static_cast<uint32_t>(operation.immediate) :
			                         (instruction.width == 2 ? static_cast<uint16_t>(operation.immediate) :
			                          static_cast<uint8_t>(operation.immediate)));
			break;
		case X64Fast::FastOpKind::MovzxReg32Reg8:
		case X64Fast::FastOpKind::MovzxReg32Reg16:
			instruction.opcode       = IrOpcode::ZeroExtendRegister;
			instruction.width        = 4;
			instruction.source_width = operation.kind == X64Fast::FastOpKind::MovzxReg32Reg8 ? 1 : 2;
			instruction.destination  = operation.dst;
			instruction.source       = operation.src;
			break;
		case X64Fast::FastOpKind::MovsxReg32Reg8:
		case X64Fast::FastOpKind::MovsxReg32Reg16:
		case X64Fast::FastOpKind::MovsxReg64Reg8:
		case X64Fast::FastOpKind::MovsxReg64Reg16:
		case X64Fast::FastOpKind::MovsxdReg64Reg32:
			instruction.opcode       = IrOpcode::SignExtendRegister;
			instruction.width        = operation.kind == X64Fast::FastOpKind::MovsxReg32Reg8 ||
			                           operation.kind == X64Fast::FastOpKind::MovsxReg32Reg16 ? 4 : 8;
			instruction.source_width = operation.kind == X64Fast::FastOpKind::MovsxdReg64Reg32 ? 4 :
			                           (operation.kind == X64Fast::FastOpKind::MovsxReg32Reg8 ||
			                            operation.kind == X64Fast::FastOpKind::MovsxReg64Reg8 ? 1 : 2);
			instruction.destination = operation.dst;
			instruction.source      = operation.src;
			break;
		case X64Fast::FastOpKind::MovzxReg32Mem8:
		case X64Fast::FastOpKind::MovzxReg64Mem8:
		case X64Fast::FastOpKind::MovzxReg32Mem16:
		case X64Fast::FastOpKind::MovzxReg64Mem16:
			instruction.opcode = IrOpcode::ZeroExtendMemory;
			instruction.width = operation.kind == X64Fast::FastOpKind::MovzxReg64Mem8 ||
			                    operation.kind == X64Fast::FastOpKind::MovzxReg64Mem16 ? 8 : 4;
			instruction.source_width = operation.kind == X64Fast::FastOpKind::MovzxReg32Mem8 ||
			                           operation.kind == X64Fast::FastOpKind::MovzxReg64Mem8 ? 1 : 2;
			instruction.destination = operation.dst;
			if (!NormalizeAddress(operation, true, &instruction.address)) return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::MovsxdReg64Mem32:
		case X64Fast::FastOpKind::MovsxReg32Mem8:
		case X64Fast::FastOpKind::MovsxReg32Mem16:
		case X64Fast::FastOpKind::MovsxReg64Mem8:
		case X64Fast::FastOpKind::MovsxReg64Mem16:
			instruction.opcode = IrOpcode::SignExtendMemory;
			instruction.width = operation.kind == X64Fast::FastOpKind::MovsxReg32Mem8 ||
			                    operation.kind == X64Fast::FastOpKind::MovsxReg32Mem16 ? 4 : 8;
			instruction.source_width = operation.kind == X64Fast::FastOpKind::MovsxdReg64Mem32 ? 4 :
			                           (operation.kind == X64Fast::FastOpKind::MovsxReg32Mem8 ||
			                            operation.kind == X64Fast::FastOpKind::MovsxReg64Mem8 ? 1 : 2);
			instruction.destination = operation.dst;
			if (!NormalizeAddress(operation, true, &instruction.address)) return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::LeaReg32:
		case X64Fast::FastOpKind::LeaReg64:
			instruction.opcode      = IrOpcode::EffectiveAddress;
			instruction.width       = operation.kind == X64Fast::FastOpKind::LeaReg64 ? 8 : 4;
			instruction.destination = operation.dst;
			if (!NormalizeAddress(operation, true, &instruction.address)) return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::BswapReg32:
		case X64Fast::FastOpKind::BswapReg64:
			instruction.opcode      = IrOpcode::ByteSwapRegister;
			instruction.width       = operation.kind == X64Fast::FastOpKind::BswapReg64 ? 8 : 4;
			instruction.destination = operation.dst;
			instruction.source      = operation.dst;
			break;
		case X64Fast::FastOpKind::MovReg8Mem:
		case X64Fast::FastOpKind::MovReg16Mem:
		case X64Fast::FastOpKind::MovReg32Mem:
		case X64Fast::FastOpKind::MovReg64Mem:
			instruction.opcode      = IrOpcode::Load;
			instruction.width       = operation.kind == X64Fast::FastOpKind::MovReg8Mem ? 1 :
			                          (operation.kind == X64Fast::FastOpKind::MovReg16Mem ? 2 :
			                           (operation.kind == X64Fast::FastOpKind::MovReg64Mem ? 8 : 4));
			instruction.destination = operation.dst;
			if (!NormalizeAddress(operation, true, &instruction.address)) return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::StoreReg8:
		case X64Fast::FastOpKind::StoreReg16:
		case X64Fast::FastOpKind::StoreReg32:
		case X64Fast::FastOpKind::StoreReg64:
			instruction.opcode = IrOpcode::Store;
			instruction.width  = operation.kind == X64Fast::FastOpKind::StoreReg8 ? 1 :
			                     (operation.kind == X64Fast::FastOpKind::StoreReg16 ? 2 :
			                      (operation.kind == X64Fast::FastOpKind::StoreReg64 ? 8 : 4));
			instruction.source = operation.src;
			if (!NormalizeAddress(operation, false, &instruction.address)) return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::MovImmMem8:
		case X64Fast::FastOpKind::MovImmMem16:
		case X64Fast::FastOpKind::MovImmMem32:
		case X64Fast::FastOpKind::MovImmMem64:
			instruction.opcode = IrOpcode::Store;
			instruction.width  = operation.kind == X64Fast::FastOpKind::MovImmMem8 ? 1 :
			                     (operation.kind == X64Fast::FastOpKind::MovImmMem16 ? 2 :
			                      (operation.kind == X64Fast::FastOpKind::MovImmMem64 ? 8 : 4));
			instruction.immediate = instruction.width == 8
			                            ? static_cast<uint64_t>(static_cast<int64_t>(operation.immediate))
			                            : (instruction.width == 4 ? static_cast<uint32_t>(operation.immediate) :
			                               (instruction.width == 2 ? static_cast<uint16_t>(operation.immediate) :
			                                                         static_cast<uint8_t>(operation.immediate)));
			if (!NormalizeAddress(operation, true, &instruction.address)) return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::AddReg32Mem:
		case X64Fast::FastOpKind::AddReg64Mem:
		case X64Fast::FastOpKind::SubReg32Mem:
		case X64Fast::FastOpKind::SubReg64Mem:
		{
			const bool add = operation.kind == X64Fast::FastOpKind::AddReg32Mem ||
			                 operation.kind == X64Fast::FastOpKind::AddReg64Mem;
			instruction.opcode = add ? IrOpcode::AddRegisterMemory : IrOpcode::SubRegisterMemory;
			instruction.width = operation.kind == X64Fast::FastOpKind::AddReg64Mem ||
			                            operation.kind == X64Fast::FastOpKind::SubReg64Mem
			                        ? 8
			                        : 4;
			instruction.destination = operation.dst;
			if (!NormalizeAddress(operation, true, &instruction.address)) return Status::InvalidArgument;
			break;
		}
		case X64Fast::FastOpKind::OrReg8Mem:
		case X64Fast::FastOpKind::OrReg16Mem:
			instruction.opcode      = IrOpcode::OrRegisterMemory;
			instruction.width       = operation.kind == X64Fast::FastOpKind::OrReg8Mem ? 1 : 2;
			instruction.destination = operation.dst;
			if (!NormalizeAddress(operation, true, &instruction.address)) return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::AndReg8Mem:
		case X64Fast::FastOpKind::AndReg32Mem:
		case X64Fast::FastOpKind::AndReg64Mem:
			instruction.opcode = IrOpcode::AndRegisterMemory;
			instruction.width = operation.kind == X64Fast::FastOpKind::AndReg8Mem
			                        ? 1
			                        : (operation.kind == X64Fast::FastOpKind::AndReg64Mem ? 8 : 4);
			instruction.destination = operation.dst;
			if (!NormalizeAddress(operation, true, &instruction.address)) return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::XorReg32Mem:
		case X64Fast::FastOpKind::XorReg64Mem:
			instruction.opcode      = IrOpcode::XorRegisterMemory;
			instruction.width       = operation.kind == X64Fast::FastOpKind::XorReg64Mem ? 8 : 4;
			instruction.destination = operation.dst;
			if (!NormalizeAddress(operation, true, &instruction.address)) return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::CmpReg32Mem:
		case X64Fast::FastOpKind::CmpReg64Mem:
			instruction.opcode      = IrOpcode::CompareRegisterMemory;
			instruction.width       = operation.kind == X64Fast::FastOpKind::CmpReg64Mem ? 8 : 4;
			instruction.destination = operation.dst;
			if (!NormalizeAddress(operation, true, &instruction.address)) return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::CmpMem32Reg:
		case X64Fast::FastOpKind::CmpMem64Reg:
			instruction.opcode = IrOpcode::CompareMemoryRegister;
			instruction.width  = operation.kind == X64Fast::FastOpKind::CmpMem64Reg ? 8 : 4;
			instruction.source = operation.src;
			if (!NormalizeAddress(operation, true, &instruction.address)) return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::CmpMem8Imm:
		case X64Fast::FastOpKind::CmpMem16Imm:
		case X64Fast::FastOpKind::CmpMem32Imm:
		case X64Fast::FastOpKind::CmpMem64Imm:
			instruction.opcode = IrOpcode::CompareMemoryImmediate;
			instruction.width = operation.kind == X64Fast::FastOpKind::CmpMem8Imm
			                        ? 1
			                    : operation.kind == X64Fast::FastOpKind::CmpMem16Imm
			                        ? 2
			                    : operation.kind == X64Fast::FastOpKind::CmpMem64Imm ? 8 : 4;
			instruction.immediate = instruction.width == 8
			                            ? static_cast<uint64_t>(static_cast<int64_t>(operation.immediate))
			                            : (instruction.width == 4
			                                   ? static_cast<uint32_t>(operation.immediate)
			                                   : (instruction.width == 2
			                                          ? static_cast<uint16_t>(operation.immediate)
			                                          : static_cast<uint8_t>(operation.immediate)));
			if (!NormalizeAddress(operation, true, &instruction.address)) return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::TestMem8Imm:
			instruction.opcode    = IrOpcode::TestMemoryImmediate;
			instruction.width     = 1;
			instruction.immediate = static_cast<uint8_t>(operation.immediate);
			if (!NormalizeAddress(operation, true, &instruction.address)) return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::ImulReg32Mem:
		case X64Fast::FastOpKind::ImulReg64Mem:
			instruction.opcode = IrOpcode::SignedMultiplyRegisterMemory;
			instruction.width  = operation.kind == X64Fast::FastOpKind::ImulReg64Mem ? 8 : 4;
			instruction.destination = operation.dst;
			instruction.source      = operation.dst;
			if (!NormalizeAddress(operation, true, &instruction.address)) return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::ImulReg32MemImm:
		case X64Fast::FastOpKind::ImulReg64MemImm:
			instruction.opcode = IrOpcode::SignedMultiplyMemoryImmediate;
			instruction.width  = operation.kind == X64Fast::FastOpKind::ImulReg64MemImm ? 8 : 4;
			instruction.destination = operation.dst;
			instruction.immediate = instruction.width == 8
			                              ? static_cast<uint64_t>(static_cast<int64_t>(operation.immediate))
			                              : static_cast<uint32_t>(operation.immediate);
			if (!NormalizeAddress(operation, true, &instruction.address)) return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::AddReg8Imm:
		case X64Fast::FastOpKind::AddReg32Imm:
		case X64Fast::FastOpKind::AddReg64Imm:
		case X64Fast::FastOpKind::SubReg32Imm:
		case X64Fast::FastOpKind::SubReg64Imm:
		case X64Fast::FastOpKind::CmpReg16Imm:
		case X64Fast::FastOpKind::CmpReg32Imm:
		case X64Fast::FastOpKind::CmpReg64Imm:
			if (operation.kind == X64Fast::FastOpKind::AddReg8Imm ||
			    operation.kind == X64Fast::FastOpKind::AddReg32Imm ||
			    operation.kind == X64Fast::FastOpKind::AddReg64Imm)
			{
				instruction.opcode = IrOpcode::AddImmediate;
			} else if (operation.kind == X64Fast::FastOpKind::SubReg32Imm || operation.kind == X64Fast::FastOpKind::SubReg64Imm)
			{
				instruction.opcode = IrOpcode::SubImmediate;
			} else
			{
				instruction.opcode = IrOpcode::CompareImmediate;
			}
			instruction.width = operation.kind == X64Fast::FastOpKind::AddReg8Imm ? 1 :
			                    (operation.kind == X64Fast::FastOpKind::CmpReg16Imm ? 2 :
			                    (operation.kind == X64Fast::FastOpKind::AddReg64Imm ||
			                            operation.kind == X64Fast::FastOpKind::SubReg64Imm ||
			                            operation.kind == X64Fast::FastOpKind::CmpReg64Imm ? 8 : 4));
			instruction.destination = operation.dst;
			instruction.immediate = instruction.width == 8
			                            ? static_cast<uint64_t>(static_cast<int64_t>(operation.immediate))
			                            : (instruction.width == 4
			                                   ? static_cast<uint32_t>(operation.immediate)
			                                   : (instruction.width == 2
			                                          ? static_cast<uint16_t>(operation.immediate)
			                                          : static_cast<uint8_t>(operation.immediate)));
			break;
		case X64Fast::FastOpKind::AddReg32:
		case X64Fast::FastOpKind::AddReg64:
		case X64Fast::FastOpKind::SubReg32:
		case X64Fast::FastOpKind::SubReg64:
		case X64Fast::FastOpKind::CmpReg32:
		case X64Fast::FastOpKind::CmpReg64:
		{
			const bool add = operation.kind == X64Fast::FastOpKind::AddReg32 ||
			                 operation.kind == X64Fast::FastOpKind::AddReg64;
			const bool sub = operation.kind == X64Fast::FastOpKind::SubReg32 ||
			                 operation.kind == X64Fast::FastOpKind::SubReg64;
			instruction.opcode = add ? IrOpcode::AddRegister :
			                     (sub ? IrOpcode::SubRegister : IrOpcode::CompareRegister);
			instruction.width = operation.kind == X64Fast::FastOpKind::AddReg64 ||
			                    operation.kind == X64Fast::FastOpKind::SubReg64 ||
			                    operation.kind == X64Fast::FastOpKind::CmpReg64 ? 8 : 4;
			// FastOp retains decoder-specific field order. Normalize it here so
			// destination is always the architectural lhs (and possible store)
			// while source is the rhs for all three non-commutative semantics.
			instruction.destination = sub ? operation.dst : operation.src;
			instruction.source      = sub ? operation.src : operation.dst;
			break;
		}
		case X64Fast::FastOpKind::ShrReg8Imm:
		case X64Fast::FastOpKind::ShlReg32Imm:
		case X64Fast::FastOpKind::ShlReg64Imm:
		case X64Fast::FastOpKind::ShlReg32Cl:
		case X64Fast::FastOpKind::ShlReg64Cl:
		case X64Fast::FastOpKind::ShrReg32Cl:
		case X64Fast::FastOpKind::ShrReg64Cl:
		case X64Fast::FastOpKind::ShrReg32Imm:
		case X64Fast::FastOpKind::ShrReg64Imm:
		case X64Fast::FastOpKind::SarReg32Imm:
		case X64Fast::FastOpKind::SarReg64Imm:
		{
			const bool immediate_shift_left =
			    operation.kind == X64Fast::FastOpKind::ShlReg32Imm ||
			    operation.kind == X64Fast::FastOpKind::ShlReg64Imm;
			const bool rotate_left =
			    immediate_shift_left &&
			    operation.condition == X64Fast::FAST_OP_SHIFT_IMMEDIATE_ROTATE_LEFT;
			if (operation.condition != 0 && !rotate_left)
				return Status::InvalidArgument;
			const bool shift_left = operation.kind == X64Fast::FastOpKind::ShlReg32Imm ||
			                        operation.kind == X64Fast::FastOpKind::ShlReg64Imm ||
			                        operation.kind == X64Fast::FastOpKind::ShlReg32Cl ||
			                        operation.kind == X64Fast::FastOpKind::ShlReg64Cl;
			const bool from_cl = operation.kind == X64Fast::FastOpKind::ShlReg32Cl ||
			                     operation.kind == X64Fast::FastOpKind::ShlReg64Cl ||
			                     operation.kind == X64Fast::FastOpKind::ShrReg32Cl ||
			                     operation.kind == X64Fast::FastOpKind::ShrReg64Cl;
			const bool arithmetic = operation.kind == X64Fast::FastOpKind::SarReg32Imm ||
			                        operation.kind == X64Fast::FastOpKind::SarReg64Imm;
			const bool width64 = operation.kind == X64Fast::FastOpKind::ShlReg64Imm ||
			                     operation.kind == X64Fast::FastOpKind::ShlReg64Cl ||
			                     operation.kind == X64Fast::FastOpKind::ShrReg64Cl ||
			                     operation.kind == X64Fast::FastOpKind::ShrReg64Imm ||
			                     operation.kind == X64Fast::FastOpKind::SarReg64Imm;

			instruction.opcode = shift_left ? (from_cl ? IrOpcode::ShiftLeftCl
			                                                 : IrOpcode::ShiftLeftImmediate)
			                     : (from_cl ? IrOpcode::ShiftRightLogicalCl
			                                : (arithmetic ? IrOpcode::ShiftRightArithmeticImmediate
			                                              : IrOpcode::ShiftRightLogicalImmediate));
			instruction.width = operation.kind == X64Fast::FastOpKind::ShrReg8Imm ? 1 :
			                    (width64 ? 8 : 4);
			instruction.destination = operation.dst;
			if (!from_cl)
			{
				const uint8_t count_mask = instruction.width == 8 ? 63u : 31u;
				instruction.immediate = static_cast<uint8_t>(operation.immediate) & count_mask;
			}
			if (rotate_left)
				instruction.condition = IR_SHIFT_IMMEDIATE_ROTATE_LEFT;
			break;
		}
		case X64Fast::FastOpKind::AndReg8Imm:
		case X64Fast::FastOpKind::OrReg8Imm:
		case X64Fast::FastOpKind::AndReg16Imm:
		case X64Fast::FastOpKind::AndReg32Imm:
		case X64Fast::FastOpKind::AndReg64Imm:
		case X64Fast::FastOpKind::OrReg32Imm:
		case X64Fast::FastOpKind::OrReg64Imm:
		case X64Fast::FastOpKind::XorReg32Imm:
		case X64Fast::FastOpKind::XorReg64Imm:
		{
			const bool bitwise_or = operation.kind == X64Fast::FastOpKind::OrReg8Imm ||
			                        operation.kind == X64Fast::FastOpKind::OrReg32Imm ||
			                        operation.kind == X64Fast::FastOpKind::OrReg64Imm;
			const bool bitwise_xor = operation.kind == X64Fast::FastOpKind::XorReg32Imm ||
			                         operation.kind == X64Fast::FastOpKind::XorReg64Imm;
			instruction.opcode = bitwise_or ? IrOpcode::OrImmediate :
			                     (bitwise_xor ? IrOpcode::XorImmediate : IrOpcode::AndImmediate);
			instruction.width = operation.kind == X64Fast::FastOpKind::AndReg8Imm ||
			                            operation.kind == X64Fast::FastOpKind::OrReg8Imm
			                        ? 1
			                    : operation.kind == X64Fast::FastOpKind::AndReg16Imm
			                        ? 2
			                    : operation.kind == X64Fast::FastOpKind::AndReg64Imm ||
			                              operation.kind == X64Fast::FastOpKind::OrReg64Imm ||
			                              operation.kind == X64Fast::FastOpKind::XorReg64Imm
			                        ? 8
			                        : 4;
			instruction.destination = operation.dst;
			instruction.immediate = instruction.width == 8
			                            ? static_cast<uint64_t>(static_cast<int64_t>(operation.immediate))
			                            : (instruction.width == 4
			                                   ? static_cast<uint32_t>(operation.immediate)
			                                   : (instruction.width == 2
			                                          ? static_cast<uint16_t>(operation.immediate)
			                                          : static_cast<uint8_t>(operation.immediate)));
			break;
		}
		case X64Fast::FastOpKind::BtReg32:
		case X64Fast::FastOpKind::BtReg64:
			instruction.opcode  = IrOpcode::BitTestRegister;
			instruction.width   = operation.kind == X64Fast::FastOpKind::BtReg64 ? 8 : 4;
			instruction.source  = operation.src;
			instruction.source2 = operation.dst;
			break;
		case X64Fast::FastOpKind::BtReg32Imm:
		case X64Fast::FastOpKind::BtReg64Imm:
			instruction.opcode = IrOpcode::BitTestImmediate;
			instruction.width = operation.kind == X64Fast::FastOpKind::BtReg64Imm ? 8 : 4;
			instruction.source = operation.src;
			instruction.immediate = static_cast<uint8_t>(operation.immediate) &
			                        (instruction.width == 8 ? 63u : 31u);
			break;
		case X64Fast::FastOpKind::IncReg32:
		case X64Fast::FastOpKind::IncReg64:
		case X64Fast::FastOpKind::DecReg32:
		case X64Fast::FastOpKind::DecReg64:
			instruction.opcode = operation.kind == X64Fast::FastOpKind::IncReg32 ||
			                             operation.kind == X64Fast::FastOpKind::IncReg64
			                         ? IrOpcode::Increment
			                         : IrOpcode::Decrement;
			instruction.width = operation.kind == X64Fast::FastOpKind::IncReg64 ||
			                            operation.kind == X64Fast::FastOpKind::DecReg64
			                        ? 8
			                        : 4;
			instruction.destination = operation.dst;
			break;
		case X64Fast::FastOpKind::IncMem32:
		case X64Fast::FastOpKind::IncMem64:
			instruction.opcode = IrOpcode::IncrementMemory;
			instruction.width = operation.kind == X64Fast::FastOpKind::IncMem64 ? 8 : 4;
			if (!NormalizeAddress(operation, true, &instruction.address))
				return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::NotReg32:
		case X64Fast::FastOpKind::NotReg64:
			instruction.opcode      = IrOpcode::BitwiseNot;
			instruction.width       = operation.kind == X64Fast::FastOpKind::NotReg64 ? 8 : 4;
			instruction.destination = operation.dst;
			instruction.source      = operation.dst;
			break;
		case X64Fast::FastOpKind::NegReg16:
		case X64Fast::FastOpKind::NegReg32:
		case X64Fast::FastOpKind::NegReg64:
			instruction.opcode      = IrOpcode::Negate;
			instruction.width       = operation.kind == X64Fast::FastOpKind::NegReg16 ? 2 :
			                          (operation.kind == X64Fast::FastOpKind::NegReg64 ? 8 : 4);
			instruction.destination = operation.dst;
			instruction.source      = operation.dst;
			break;
		case X64Fast::FastOpKind::TestReg8Imm:
		case X64Fast::FastOpKind::TestReg16Imm:
		case X64Fast::FastOpKind::TestReg32Imm:
		case X64Fast::FastOpKind::TestReg64Imm:
			if (operation.kind == X64Fast::FastOpKind::TestReg8Imm &&
			    (operation.condition != 0 ||
			     (operation.base != 0 &&
			      operation.base != X64Fast::FAST_OP_TEST_REG8_IMM_HIGH_BYTE) ||
			     (operation.base == X64Fast::FAST_OP_TEST_REG8_IMM_HIGH_BYTE &&
			      operation.dst >= 4)))
			{
				return Status::InvalidArgument;
			}
			instruction.opcode = IrOpcode::TestImmediate;
			instruction.width = operation.kind == X64Fast::FastOpKind::TestReg8Imm ? 1 :
			                    (operation.kind == X64Fast::FastOpKind::TestReg16Imm ? 2 :
			                     (operation.kind == X64Fast::FastOpKind::TestReg64Imm ? 8 : 4));
			instruction.source = operation.dst;
			if (operation.kind == X64Fast::FastOpKind::TestReg8Imm &&
			    operation.base == X64Fast::FAST_OP_TEST_REG8_IMM_HIGH_BYTE)
			{
				instruction.condition = IR_TEST_IMMEDIATE_HIGH_BYTE;
			}
			instruction.immediate = instruction.width == 8
			                            ? static_cast<uint64_t>(static_cast<int64_t>(operation.immediate))
			                            : (instruction.width == 4
			                                   ? static_cast<uint32_t>(operation.immediate)
			                                   : (instruction.width == 2
			                                          ? static_cast<uint16_t>(operation.immediate)
			                                          : static_cast<uint8_t>(operation.immediate)));
			break;
		case X64Fast::FastOpKind::TestReg8:
		case X64Fast::FastOpKind::TestReg16:
		case X64Fast::FastOpKind::TestReg32:
		case X64Fast::FastOpKind::TestReg64:
			instruction.opcode = IrOpcode::TestRegister;
			instruction.width = operation.kind == X64Fast::FastOpKind::TestReg8 ? 1 :
			                    (operation.kind == X64Fast::FastOpKind::TestReg16 ? 2 :
			                     (operation.kind == X64Fast::FastOpKind::TestReg64 ? 8 : 4));
			instruction.source  = operation.src;
			instruction.source2 = operation.dst;
			break;
		case X64Fast::FastOpKind::SbbReg8:
		case X64Fast::FastOpKind::SbbReg16:
			instruction.opcode      = IrOpcode::SubtractWithBorrowRegister;
			instruction.width       = operation.kind == X64Fast::FastOpKind::SbbReg8 ? 1 : 2;
			instruction.destination = operation.src;
			instruction.source      = operation.dst;
			break;
		case X64Fast::FastOpKind::BextrReg32:
			instruction.opcode      = IrOpcode::BitExtractRegister;
			instruction.width       = 4;
			instruction.destination = operation.dst;
			instruction.source      = operation.src;
			instruction.source2     = operation.base;
			break;
		case X64Fast::FastOpKind::AndnReg32:
		case X64Fast::FastOpKind::AndnReg64:
			instruction.opcode      = IrOpcode::AndNotRegister;
			instruction.width       = operation.kind == X64Fast::FastOpKind::AndnReg64 ? 8 : 4;
			instruction.destination = operation.dst;
			instruction.source      = operation.src;
			instruction.source2     = operation.base;
			break;
		case X64Fast::FastOpKind::ImulReg32:
		case X64Fast::FastOpKind::ImulReg64:
			instruction.opcode      = IrOpcode::SignedMultiplyRegister;
			instruction.width       = operation.kind == X64Fast::FastOpKind::ImulReg64 ? 8 : 4;
			instruction.destination = operation.dst;
			instruction.source      = operation.dst;
			instruction.source2     = operation.src;
			break;
		case X64Fast::FastOpKind::ImulReg32Imm:
		case X64Fast::FastOpKind::ImulReg64Imm:
			instruction.opcode      = IrOpcode::SignedMultiplyImmediate;
			instruction.width       = operation.kind == X64Fast::FastOpKind::ImulReg64Imm ? 8 : 4;
			instruction.destination = operation.dst;
			instruction.source      = operation.src;
			instruction.immediate   = instruction.width == 8
			                              ? static_cast<uint64_t>(static_cast<int64_t>(operation.immediate))
			                              : static_cast<uint32_t>(operation.immediate);
			break;
		case X64Fast::FastOpKind::AndReg16:
		case X64Fast::FastOpKind::AndReg32:
		case X64Fast::FastOpKind::AndReg64:
			instruction.opcode = IrOpcode::AndRegister;
			instruction.width = operation.kind == X64Fast::FastOpKind::AndReg16 ? 2 :
			                    (operation.kind == X64Fast::FastOpKind::AndReg64 ? 8 : 4);
			instruction.destination = operation.dst;
			instruction.source      = operation.src;
			break;
		case X64Fast::FastOpKind::OrReg8:
		case X64Fast::FastOpKind::OrReg32:
		case X64Fast::FastOpKind::OrReg64:
			instruction.opcode = IrOpcode::OrRegister;
			instruction.width = operation.kind == X64Fast::FastOpKind::OrReg8 ? 1 :
			                    (operation.kind == X64Fast::FastOpKind::OrReg64 ? 8 : 4);
			instruction.destination = operation.src;
			instruction.source      = operation.dst;
			break;
		case X64Fast::FastOpKind::XorReg32:
		case X64Fast::FastOpKind::XorReg64:
			instruction.opcode      = IrOpcode::XorRegister;
			instruction.width       = operation.kind == X64Fast::FastOpKind::XorReg64 ? 8 : 4;
			instruction.destination = operation.src;
			instruction.source      = operation.dst;
			break;
		case X64Fast::FastOpKind::AddMem32Reg:
		case X64Fast::FastOpKind::AddMem64Reg:
		case X64Fast::FastOpKind::SubMem32Reg:
		case X64Fast::FastOpKind::SubMem64Reg:
		case X64Fast::FastOpKind::AndMem32Reg:
		case X64Fast::FastOpKind::AndMem64Reg:
			instruction.opcode = operation.kind == X64Fast::FastOpKind::AddMem32Reg ||
			                             operation.kind == X64Fast::FastOpKind::AddMem64Reg
			                         ? IrOpcode::AddMemoryRegister
			                     : operation.kind == X64Fast::FastOpKind::SubMem32Reg ||
			                               operation.kind == X64Fast::FastOpKind::SubMem64Reg
			                         ? IrOpcode::SubtractMemoryRegister
			                         : IrOpcode::AndMemoryRegister;
			instruction.width = operation.kind == X64Fast::FastOpKind::AddMem64Reg ||
			                            operation.kind == X64Fast::FastOpKind::SubMem64Reg ||
			                            operation.kind == X64Fast::FastOpKind::AndMem64Reg
			                        ? 8
			                        : 4;
			instruction.source = operation.src;
			if (!NormalizeAddress(operation, true, &instruction.address))
				return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::AndMem8Imm:
			instruction.opcode = IrOpcode::AndMemoryImmediate;
			instruction.width = 1;
			instruction.immediate = static_cast<uint8_t>(operation.immediate);
			if (!NormalizeAddress(operation, true, &instruction.address))
				return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::SetccReg8:
			instruction.opcode      = IrOpcode::SetCondition;
			instruction.width       = 1;
			instruction.destination = operation.dst;
			instruction.condition   = operation.condition;
			break;
		case X64Fast::FastOpKind::SetccMem8:
			instruction.opcode = IrOpcode::SetConditionMemory;
			instruction.width = 1;
			instruction.condition = operation.condition;
			if (!NormalizeAddress(operation, true, &instruction.address))
				return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::SetbReg8:
			instruction.opcode      = IrOpcode::SetCondition;
			instruction.width       = 1;
			instruction.destination = operation.dst;
			instruction.condition   = 2;
			break;
		case X64Fast::FastOpKind::CmovccReg32:
		case X64Fast::FastOpKind::CmovccReg64:
			instruction.opcode      = IrOpcode::ConditionalMove;
			instruction.width       = operation.kind == X64Fast::FastOpKind::CmovccReg64 ? 8 : 4;
			instruction.destination = operation.dst;
			instruction.source      = operation.src;
			instruction.condition   = operation.condition;
			break;
		case X64Fast::FastOpKind::Cld:
			instruction.opcode = IrOpcode::ClearDirectionFlag;
			break;
		case X64Fast::FastOpKind::PushReg64:
			instruction.opcode = IrOpcode::PushRegister;
			instruction.width  = 8;
			instruction.source = operation.src;
			break;
		case X64Fast::FastOpKind::PopReg64:
			instruction.opcode      = IrOpcode::PopRegister;
			instruction.width       = 8;
			instruction.destination = operation.dst;
			break;
		case X64Fast::FastOpKind::Jcc:
			instruction.opcode    = IrOpcode::BranchCondition;
			instruction.condition = operation.condition;
			instruction.target    = operation.target;
			break;
		case X64Fast::FastOpKind::Jmp:
			instruction.opcode = IrOpcode::JumpImmediate;
			instruction.target = operation.target;
			break;
		case X64Fast::FastOpKind::Call:
			instruction.opcode = IrOpcode::CallImmediate;
			instruction.target = operation.target;
			break;
		case X64Fast::FastOpKind::CallReg64:
			instruction.opcode = IrOpcode::CallIndirectRegister;
			instruction.width = 8;
			instruction.source = operation.src;
			break;
		case X64Fast::FastOpKind::CallMem64:
			instruction.opcode = IrOpcode::CallIndirectMemory;
			instruction.width = 8;
			if (!NormalizeAddress(operation, true, &instruction.address))
				return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::JmpReg64:
			instruction.opcode = IrOpcode::JumpIndirectRegister;
			instruction.width = 8;
			instruction.source = operation.src;
			break;
		case X64Fast::FastOpKind::JmpMem64:
			instruction.opcode = IrOpcode::JumpIndirectMemory;
			instruction.width = 8;
			if (!NormalizeAddress(operation, true, &instruction.address))
				return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::RepMovs8:
		case X64Fast::FastOpKind::RepMovs64:
			instruction.opcode = IrOpcode::RepeatMove;
			instruction.width = operation.kind == X64Fast::FastOpKind::RepMovs64 ? 8 : 1;
			break;
		case X64Fast::FastOpKind::RepStos8:
		case X64Fast::FastOpKind::RepStos64:
			instruction.opcode = IrOpcode::RepeatStore;
			instruction.width = operation.kind == X64Fast::FastOpKind::RepStos64 ? 8 : 1;
			break;
		case X64Fast::FastOpKind::LockCmpxchgMem64Reg:
			instruction.opcode = IrOpcode::AtomicCompareExchangeMemory;
			instruction.width = 8;
			instruction.source = operation.src;
			if (!NormalizeAddress(operation, true, &instruction.address))
				return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::Ret:
			if (operation.immediate < 0 || operation.immediate > static_cast<int32_t>(UINT16_MAX))
			{
				return Status::InvalidArgument;
			}
			instruction.opcode = IrOpcode::Return;
			instruction.width  = 8;
			instruction.immediate = static_cast<uint16_t>(operation.immediate);
			break;
		case X64Fast::FastOpKind::Vcvttss2siReg32Reg:
		case X64Fast::FastOpKind::Vcvttss2siReg64Reg:
			instruction.opcode = IrOpcode::VectorConvertFloatToIntegerRegister;
			instruction.width = operation.kind == X64Fast::FastOpKind::Vcvttss2siReg64Reg ? 8 : 4;
			instruction.source_width = 4;
			instruction.destination = operation.dst;
			instruction.source = operation.base;
			break;
		case X64Fast::FastOpKind::Vcvttss2siReg32Mem:
		case X64Fast::FastOpKind::Vcvttss2siReg64Mem:
			instruction.opcode = IrOpcode::VectorConvertFloatToIntegerMemory;
			instruction.width = operation.kind == X64Fast::FastOpKind::Vcvttss2siReg64Mem ? 8 : 4;
			instruction.source_width = 4;
			instruction.destination = operation.dst;
			if (!NormalizeAddress(operation, true, &instruction.address))
				return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::Vcvtsi2ssReg32Reg:
		case X64Fast::FastOpKind::Vcvtsi2ssReg64Reg:
			instruction.opcode = IrOpcode::VectorConvertIntegerToFloatRegister;
			instruction.width = 4;
			instruction.source_width = operation.kind == X64Fast::FastOpKind::Vcvtsi2ssReg64Reg ? 8 : 4;
			instruction.destination = operation.dst;
			instruction.source = operation.src;
			instruction.source2 = operation.base;
			instruction.vector_flags = IrVectorFlagMask(IrVectorFlag::ZeroUpper);
			break;
		case X64Fast::FastOpKind::Vcvtsi2ssReg32Mem:
		case X64Fast::FastOpKind::Vcvtsi2ssReg64Mem:
			instruction.opcode = IrOpcode::VectorConvertIntegerToFloatMemory;
			instruction.width = 4;
			instruction.source_width = operation.kind == X64Fast::FastOpKind::Vcvtsi2ssReg64Mem ? 8 : 4;
			instruction.destination = operation.dst;
			instruction.source = operation.src;
			instruction.vector_flags = IrVectorFlagMask(IrVectorFlag::ZeroUpper);
			if (!NormalizeAddress(operation, true, &instruction.address))
				return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::VaddssReg:
		case X64Fast::FastOpKind::VmulssReg:
		case X64Fast::FastOpKind::VsubssReg:
		case X64Fast::FastOpKind::VdivssReg:
		case X64Fast::FastOpKind::VaddsdReg:
		case X64Fast::FastOpKind::VmulsdReg:
		case X64Fast::FastOpKind::VsubsdReg:
		case X64Fast::FastOpKind::VdivsdReg:
		case X64Fast::FastOpKind::VaddpsReg:
		case X64Fast::FastOpKind::VmulpsReg:
		case X64Fast::FastOpKind::VsubpsReg:
		case X64Fast::FastOpKind::VaddssMem:
		case X64Fast::FastOpKind::VmulssMem:
		case X64Fast::FastOpKind::VsubssMem:
		case X64Fast::FastOpKind::VdivssMem:
		case X64Fast::FastOpKind::VaddsdMem:
		case X64Fast::FastOpKind::VmulsdMem:
		case X64Fast::FastOpKind::VsubsdMem:
		case X64Fast::FastOpKind::VdivsdMem:
		case X64Fast::FastOpKind::VaddpsMem:
		case X64Fast::FastOpKind::VmulpsMem:
		case X64Fast::FastOpKind::VsubpsMem:
		case X64Fast::FastOpKind::VsubpdMem128:
		{
			const bool memory = operation.kind == X64Fast::FastOpKind::VaddssMem ||
			                    operation.kind == X64Fast::FastOpKind::VmulssMem ||
			                    operation.kind == X64Fast::FastOpKind::VsubssMem ||
			                    operation.kind == X64Fast::FastOpKind::VdivssMem ||
			                    operation.kind == X64Fast::FastOpKind::VaddsdMem ||
			                    operation.kind == X64Fast::FastOpKind::VmulsdMem ||
			                    operation.kind == X64Fast::FastOpKind::VsubsdMem ||
			                    operation.kind == X64Fast::FastOpKind::VdivsdMem ||
			                    operation.kind == X64Fast::FastOpKind::VaddpsMem ||
			                    operation.kind == X64Fast::FastOpKind::VmulpsMem ||
			                    operation.kind == X64Fast::FastOpKind::VsubpsMem ||
			                    operation.kind == X64Fast::FastOpKind::VsubpdMem128;
			const bool scalar_single = operation.kind == X64Fast::FastOpKind::VaddssReg ||
			                           operation.kind == X64Fast::FastOpKind::VmulssReg ||
			                           operation.kind == X64Fast::FastOpKind::VsubssReg ||
			                           operation.kind == X64Fast::FastOpKind::VdivssReg ||
			                           operation.kind == X64Fast::FastOpKind::VaddssMem ||
			                           operation.kind == X64Fast::FastOpKind::VmulssMem ||
			                           operation.kind == X64Fast::FastOpKind::VsubssMem ||
			                           operation.kind == X64Fast::FastOpKind::VdivssMem;
			const bool scalar_double = operation.kind == X64Fast::FastOpKind::VaddsdReg ||
			                           operation.kind == X64Fast::FastOpKind::VmulsdReg ||
			                           operation.kind == X64Fast::FastOpKind::VsubsdReg ||
			                           operation.kind == X64Fast::FastOpKind::VdivsdReg ||
			                           operation.kind == X64Fast::FastOpKind::VaddsdMem ||
			                           operation.kind == X64Fast::FastOpKind::VmulsdMem ||
			                           operation.kind == X64Fast::FastOpKind::VsubsdMem ||
			                           operation.kind == X64Fast::FastOpKind::VdivsdMem;
			const bool packed_double = operation.kind == X64Fast::FastOpKind::VsubpdMem128;
			const bool multiply = operation.kind == X64Fast::FastOpKind::VmulssReg ||
			                      operation.kind == X64Fast::FastOpKind::VmulssMem ||
			                      operation.kind == X64Fast::FastOpKind::VmulsdReg ||
			                      operation.kind == X64Fast::FastOpKind::VmulsdMem ||
			                      operation.kind == X64Fast::FastOpKind::VmulpsReg ||
			                      operation.kind == X64Fast::FastOpKind::VmulpsMem;
			const bool subtract = operation.kind == X64Fast::FastOpKind::VsubssReg ||
			                      operation.kind == X64Fast::FastOpKind::VsubssMem ||
			                      operation.kind == X64Fast::FastOpKind::VsubsdReg ||
			                      operation.kind == X64Fast::FastOpKind::VsubsdMem ||
			                      operation.kind == X64Fast::FastOpKind::VsubpsReg ||
			                      operation.kind == X64Fast::FastOpKind::VsubpsMem || packed_double;
			const bool divide = operation.kind == X64Fast::FastOpKind::VdivssReg ||
			                    operation.kind == X64Fast::FastOpKind::VdivssMem ||
			                    operation.kind == X64Fast::FastOpKind::VdivsdReg ||
			                    operation.kind == X64Fast::FastOpKind::VdivsdMem;

			instruction.opcode = memory ? IrOpcode::VectorBinaryMemory : IrOpcode::VectorBinaryRegister;
			instruction.destination = operation.dst;
			instruction.source = operation.src;
			instruction.condition = static_cast<uint8_t>(
			    divide ? IrVectorBinaryOperation::Divide :
			    subtract ? IrVectorBinaryOperation::Subtract :
			    multiply ? IrVectorBinaryOperation::Multiply : IrVectorBinaryOperation::Add);
			if (scalar_single || scalar_double)
			{
				instruction.width = scalar_single ? 4 : 8;
				instruction.vector_flags = IrVectorFlagMask(IrVectorFlag::ZeroUpper);
			} else if (packed_double)
			{
				instruction.width = 16;
				instruction.source_width = 8;
				instruction.vector_flags = IrVectorFlagMask(IrVectorFlag::ZeroUpper);
			} else
			{
				if (operation.immediate != 2 && operation.immediate != 4)
					return Status::InvalidArgument;
				instruction.width = static_cast<uint8_t>(operation.immediate * sizeof(uint64_t));
				instruction.source_width = 4;
				if (instruction.width == 16)
					instruction.vector_flags = IrVectorFlagMask(IrVectorFlag::ZeroUpper);
			}
			if (memory)
			{
				if (!NormalizeAddress(operation, true, &instruction.address))
					return Status::InvalidArgument;
			} else
			{
				instruction.source2 = operation.base;
			}
			break;
		}
		case X64Fast::FastOpKind::VhaddpdReg128:
			instruction.opcode = IrOpcode::VectorHorizontalAddRegister;
			instruction.width = 16;
			instruction.source_width = 8;
			instruction.destination = operation.dst;
			instruction.source = operation.src;
			instruction.source2 = operation.base;
			instruction.vector_flags = IrVectorFlagMask(IrVectorFlag::ZeroUpper);
			break;
		case X64Fast::FastOpKind::VpunpckldqMem128:
			instruction.opcode = IrOpcode::VectorUnpackLowDwordsMemory;
			instruction.width = 16;
			instruction.source_width = 4;
			instruction.destination = operation.dst;
			instruction.source = operation.src;
			instruction.vector_flags = IrVectorFlagMask(IrVectorFlag::ZeroUpper);
			if (!NormalizeAddress(operation, true, &instruction.address))
				return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::VmovqReg64ToXmm:
			instruction.opcode = IrOpcode::VectorMoveGpr64ToXmm;
			instruction.width = 8;
			instruction.destination = operation.dst;
			instruction.source = operation.base;
			break;
		case X64Fast::FastOpKind::VucomissReg:
		case X64Fast::FastOpKind::VucomisdReg:
			instruction.opcode = IrOpcode::VectorCompareRegister;
			instruction.width = operation.kind == X64Fast::FastOpKind::VucomisdReg ? 8 : 4;
			instruction.destination = operation.dst;
			instruction.source = operation.base;
			break;
		case X64Fast::FastOpKind::VucomissMem:
		case X64Fast::FastOpKind::VucomisdMem:
			instruction.opcode = IrOpcode::VectorCompareMemory;
			instruction.width = operation.kind == X64Fast::FastOpKind::VucomisdMem ? 8 : 4;
			instruction.destination = operation.dst;
			if (!NormalizeAddress(operation, true, &instruction.address))
				return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::VmovdReg32ToXmm:
			instruction.opcode      = IrOpcode::VectorMoveGpr32ToXmm;
			instruction.width       = 4;
			instruction.destination = operation.dst;
			instruction.source      = operation.base;
			break;
		case X64Fast::FastOpKind::VectorPermuteDwordsImmediate:
			if (operation.immediate < 0 || operation.immediate > static_cast<int32_t>(UINT8_MAX) ||
			    operation.src != 0 || operation.index != IR_NO_REGISTER || operation.scale != 0 ||
			    operation.displacement != 0 || operation.target != 0 ||
			    operation.condition != X64Fast::FAST_OP_VECTOR_ZERO_UPPER)
			{
				return Status::InvalidArgument;
			}
			instruction.opcode       = IrOpcode::VectorPermuteDwordsImmediate;
			instruction.width        = 16;
			instruction.destination  = operation.dst;
			instruction.source       = operation.base;
			instruction.immediate    = static_cast<uint8_t>(operation.immediate);
			instruction.vector_flags = IrVectorFlagMask(IrVectorFlag::ZeroUpper);
			break;
		case X64Fast::FastOpKind::VmovPackedMem:
			if (operation.immediate != 2 &&
			    !(packed256_enabled && operation.immediate == 4))
				return Status::UnsupportedOperation;
			instruction.opcode       = IrOpcode::VectorLoad;
			instruction.width        = static_cast<uint8_t>(operation.immediate * sizeof(uint64_t));
			instruction.destination  = operation.dst;
			instruction.vector_flags = operation.condition;
			if (!NormalizeAddress(operation, true, &instruction.address)) return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::VmovPackedReg:
			if (operation.immediate != 2 &&
			    !(packed256_enabled && operation.immediate == 4))
				return Status::UnsupportedOperation;
			instruction.opcode       = IrOpcode::VectorMoveRegister;
			instruction.width        = static_cast<uint8_t>(operation.immediate * sizeof(uint64_t));
			instruction.destination  = operation.dst;
			instruction.source       = operation.base;
			instruction.vector_flags = operation.condition;
			break;
		case X64Fast::FastOpKind::VmovssReg:
		case X64Fast::FastOpKind::VmovsdReg:
			instruction.opcode       = IrOpcode::VectorMoveScalarRegister;
			instruction.width        = operation.kind == X64Fast::FastOpKind::VmovssReg ? 4 : 8;
			instruction.destination  = operation.dst;
			instruction.source       = operation.src;
			instruction.source2      = operation.base;
			instruction.vector_flags = IrVectorFlagMask(IrVectorFlag::ZeroUpper);
			break;
		case X64Fast::FastOpKind::VmovssMem:
		case X64Fast::FastOpKind::VmovsdMem:
			if (operation.condition != 0 || operation.immediate != 0)
				return Status::InvalidArgument;
			instruction.opcode = IrOpcode::VectorLoadScalar;
			instruction.width = operation.kind == X64Fast::FastOpKind::VmovssMem ? 4 : 8;
			instruction.destination = operation.dst;
			if (!NormalizeAddress(operation, true, &instruction.address))
				return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::VmovssStoreMem:
		case X64Fast::FastOpKind::VmovsdStoreMem:
			if (operation.condition != 0 || operation.immediate != 0)
				return Status::InvalidArgument;
			instruction.opcode = IrOpcode::VectorStoreScalar;
			instruction.width = operation.kind == X64Fast::FastOpKind::VmovssStoreMem ? 4 : 8;
			instruction.source = operation.dst;
			if (!NormalizeAddress(operation, true, &instruction.address))
				return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::VmovPackedStoreMem:
			if (operation.immediate != 2 &&
			    !(packed256_enabled && operation.immediate == 4))
				return Status::UnsupportedOperation;
			instruction.opcode       = IrOpcode::VectorStore;
			instruction.width        = static_cast<uint8_t>(operation.immediate * sizeof(uint64_t));
			instruction.source       = operation.dst;
			instruction.vector_flags = operation.condition;
			if (!NormalizeAddress(operation, true, &instruction.address)) return Status::InvalidArgument;
			break;
		case X64Fast::FastOpKind::VxorPackedReg:
			if (operation.immediate != 2 &&
			    !(packed256_enabled && operation.immediate == 4))
				return Status::UnsupportedOperation;
			instruction.opcode       = IrOpcode::VectorXorRegister;
			instruction.width        = static_cast<uint8_t>(operation.immediate * sizeof(uint64_t));
			instruction.destination  = operation.dst;
			instruction.source       = operation.src;
			instruction.source2      = operation.base;
			instruction.vector_flags = operation.condition;
			break;
		case X64Fast::FastOpKind::VxorPackedMem:
			if (operation.immediate != 2 &&
			    !(packed256_enabled && operation.immediate == 4))
				return Status::UnsupportedOperation;
			instruction.opcode       = IrOpcode::VectorXorMemory;
			instruction.width        = static_cast<uint8_t>(operation.immediate * sizeof(uint64_t));
			instruction.destination  = operation.dst;
			instruction.source       = operation.src;
			instruction.vector_flags = operation.condition;
			if (!NormalizeAddress(operation, true, &instruction.address)) return Status::InvalidArgument;
			break;
		default: return Status::UnsupportedOperation;
	}
	instruction.effects = ExpectedEffects(instruction.opcode);
	if (!ValidateLoweredInstruction<BranchLocal, CountForTest>(instruction))
		return Status::InvalidArgument;
	*output = instruction;
	return Status::Ok;
}

Status LowerInstruction(
	const X64Fast::FastOp& operation, IrInstruction* output,
	bool packed256_enabled)
{
	return LowerInstructionImpl<false>(operation, output, packed256_enabled);
}

FastOpExecutionSnapshotAnalysis AnalyzeFastOpExecutionSnapshotImpl(
	const X64Fast::FastOp& operation, bool packed256_enabled)
{
	FastOpExecutionSnapshotAnalysis analysis {};
	IrInstruction instruction {};
	// This is an execution-safety probe, not an IR verification hand-off. Keep
	// it side-effect-free so decoding FastBlocks cannot perturb verifier
	// telemetry or exact unit-test counters.
	if (LowerInstructionImpl<false, false>(
	        operation, &instruction, packed256_enabled) != Status::Ok)
		return analysis;
	constexpr uint8_t guest_memory_effects =
	    IrEffectMask(IrEffect::ReadGuestMemory) |
	    IrEffectMask(IrEffect::WriteGuestMemory);
	analysis.known = true;
	analysis.guest_memory =
	    (instruction.effects & guest_memory_effects) != 0;
	analysis.host_call =
	    (instruction.effects & IrEffectMask(IrEffect::HostCall)) != 0;
	analysis.unknown = false;
	analysis.snapshot_avoidance_candidate =
	    analysis.guest_memory && !analysis.host_call;
	return analysis;
}

FastBlockOperationSnapshotAnalysis AnalyzeFastBlockOperationSnapshotImpl(
	const X64Fast::FastOp* operations, size_t operation_count,
	bool packed256_enabled)
{
	FastBlockOperationSnapshotAnalysis analysis {};
	analysis.operation_count = operation_count;
	if (operation_count == 0) return analysis;
	if (operations == nullptr || operation_count > MAX_IR_INSTRUCTIONS)
	{
		analysis.unknown_operation_count = operation_count;
		return analysis;
	}

	for (size_t index = 0; index < operation_count; index++)
	{
		const auto operation_analysis = AnalyzeFastOpExecutionSnapshotImpl(
		    operations[index], packed256_enabled);
		if (operation_analysis.known) analysis.known_operation_count++;
		if (operation_analysis.guest_memory)
			analysis.guest_memory_operation_count++;
		if (operation_analysis.host_call) analysis.host_call_operation_count++;
		if (operation_analysis.unknown) analysis.unknown_operation_count++;
	}
	analysis.snapshot_avoidance_candidate =
	    analysis.known_operation_count == operation_count &&
	    analysis.guest_memory_operation_count != 0 &&
	    analysis.host_call_operation_count == 0 &&
	    analysis.unknown_operation_count == 0;
	return analysis;
}

bool FastOpRequiresExecutionSnapshotImpl(
	const X64Fast::FastOp& operation, bool packed256_enabled)
{
	const auto analysis =
	    AnalyzeFastOpExecutionSnapshotImpl(operation, packed256_enabled);
	return analysis.unknown || analysis.guest_memory || analysis.host_call;
}

bool HasRuntimeServices(const EntryContext* context)
{
	if (context == nullptr || context->runtime_context == nullptr) return false;
	const auto* services = static_cast<const RuntimeServices*>(context->runtime_context);
	return services->execute_step != nullptr;
}

uint8_t ScalarMemoryAccessWidth(const IrInstruction& instruction);
bool IsScalarMemoryReadModifyWriteOpcode(IrOpcode opcode);

void DiscardScalarMemoryTranslations(EntryContext* context)
{
	if (context == nullptr) return;
	context->scalar_memory_regions = {};
	context->scalar_memory_host_address = 0;
	context->scalar_memory_read_host_address = 0;
	context->scalar_memory_next = 0;
}

void SanitizeScalarMemoryTranslations(EntryContext* context)
{
	if (context == nullptr) return;
	constexpr uint32_t permission_mask =
	    ScalarMemoryPermissionMask(ScalarMemoryPermission::Read) |
	    ScalarMemoryPermissionMask(ScalarMemoryPermission::Write);
	for (auto& region: context->scalar_memory_regions)
	{
		const uint64_t span = region.guest_end > region.guest_begin
		                          ? region.guest_end - region.guest_begin
		                          : 0;
		if (span == 0 || region.guest_begin < UINT64_C(0x100000000) ||
		    region.host_begin == 0 || region.host_begin > UINT64_MAX - (span - 1u) ||
		    region.permissions == 0 || (region.permissions & ~permission_mask) != 0)
		{
			region = {};
		} else
		{
			region.reserved = 0;
		}
	}
	context->scalar_memory_next %= static_cast<uint32_t>(SCALAR_MEMORY_CACHE_ENTRIES);
}

class ScalarMemoryInvocationGuard final
{
public:
	explicit ScalarMemoryInvocationGuard(EntryContext* context): m_context(context) {}
	~ScalarMemoryInvocationGuard() { DiscardScalarMemoryTranslations(m_context); }

	ScalarMemoryInvocationGuard(const ScalarMemoryInvocationGuard&) = delete;
	ScalarMemoryInvocationGuard& operator=(const ScalarMemoryInvocationGuard&) = delete;

private:
	EntryContext* m_context = nullptr;
};

StepAction DispatchIrStep(EntryContext* context, const IrInstruction* instruction) noexcept
{
	if (context == nullptr || instruction == nullptr)
	{
		return StepAction::Exit;
	}
	if (instruction->step_cost == 0 || context->executed_steps > context->remaining_steps ||
	    instruction->step_cost > context->remaining_steps - context->executed_steps)
	{
		context->exit_reason = ExitReason::StepLimit;
		return StepAction::Exit;
	}

	auto* services = static_cast<RuntimeServices*>(context->runtime_context);
	if (services == nullptr || services->execute_step == nullptr)
	{
		context->exit_reason = ExitReason::Bailout;
		return StepAction::Exit;
	}
	// Once the semantic callback is entered the current op is counted and may
	// commit. Keep the sequential post-op boundary ready for an unexpected
	// bailout; architectural fault/branch helpers publish their own RIP.
	context->resume_rip = instruction->rip + instruction->length;
	context->executed_steps += instruction->step_cost;
	const StepAction action = services->execute_step(services->opaque, context, instruction);
	// A generic semantic helper may invoke a re-entrant address translator or a
	// host service. Never carry a host interval across that callback boundary.
	DiscardScalarMemoryTranslations(context);
	if (action != StepAction::Continue && action != StepAction::Exit)
	{
		context->exit_reason = ExitReason::Bailout;
		return StepAction::Exit;
	}
	if (action == StepAction::Continue && HasEffect(*instruction, IrEffect::Terminator))
	{
		context->exit_reason = ExitReason::Bailout;
		return StepAction::Exit;
	}
	return action;
}

ScalarMemoryResolveResult DispatchScalarMemoryResolve(EntryContext* context,
	                                                   const IrInstruction* instruction,
	                                                   uint64_t guest_address) noexcept
{
	if (context == nullptr || context->state == nullptr || instruction == nullptr ||
	    context->scalar_memory_enabled == 0)
	{
		return ScalarMemoryResolveResult::UseGeneric;
	}
	auto* services = static_cast<RuntimeServices*>(context->runtime_context);
	if (services == nullptr || services->resolve_scalar_memory == nullptr)
	{
		return ScalarMemoryResolveResult::UseGeneric;
	}
	// A callback must publish a fresh exact pointer for this miss. Never allow a
	// nominal Resolved result to inherit either channel from a prior access.
	context->scalar_memory_host_address = 0;
	context->scalar_memory_read_host_address = 0;
	context->scalar_memory_post_action = 0;
	const ScalarMemoryResolveResult result =
	    services->resolve_scalar_memory(services->opaque, context, instruction, guest_address);
	if (result != ScalarMemoryResolveResult::Resolved &&
	    result != ScalarMemoryResolveResult::MemoryFault &&
	    result != ScalarMemoryResolveResult::UseGeneric)
	{
		context->exit_reason = ExitReason::Bailout;
		DiscardScalarMemoryTranslations(context);
		return ScalarMemoryResolveResult::MemoryFault;
	}
	if (result != ScalarMemoryResolveResult::Resolved)
	{
		DiscardScalarMemoryTranslations(context);
		context->scalar_memory_post_action = 0;
		return result;
	}
	SanitizeScalarMemoryTranslations(context);
	const uint8_t width = ScalarMemoryAccessWidth(*instruction);
	if ((width != 1 && width != 2 && width != 4 && width != 8) ||
	    context->scalar_memory_host_address == 0 ||
	    context->scalar_memory_host_address > UINT64_MAX - (width - 1u) ||
	    (IsScalarMemoryReadModifyWriteOpcode(instruction->opcode) &&
	     (context->scalar_memory_read_host_address == 0 ||
	      context->scalar_memory_read_host_address > UINT64_MAX - (width - 1u))))
	{
		context->exit_reason = ExitReason::Bailout;
		DiscardScalarMemoryTranslations(context);
		return ScalarMemoryResolveResult::MemoryFault;
	}
	if (IsScalarMemoryReadModifyWriteOpcode(instruction->opcode) &&
	    context->scalar_memory_read_host_address !=
	        context->scalar_memory_host_address)
	{
		// A single ScalarMemoryRegion has only one host base.  Therefore a record
		// carrying both permissions cannot represent an interval whose authoritative
		// read and write translations use different aliases.  A resolver is allowed
		// to publish acceleration records independently of its exact result, so
		// invalidate the two bounded combined records before a later RMW can
		// incorrectly reuse a write alias as its read side.
		const uint32_t read_write =
		    ScalarMemoryPermissionMask(ScalarMemoryPermission::Read) |
		    ScalarMemoryPermissionMask(ScalarMemoryPermission::Write);
		for (auto& region: context->scalar_memory_regions)
		{
			if ((region.permissions & read_write) == read_write) region = {};
		}
	}
	if (context->scalar_memory_post_action != 0)
	{
		context->scalar_memory_post_action = static_cast<uint32_t>(StepAction::Exit);
		context->state->rip = instruction->rip + instruction->length;
	}
	return result;
}

StepAction DispatchScalarStoreNotify(EntryContext* context, const IrInstruction* instruction,
	                                 uint64_t guest_address, uint64_t host_address,
	                                 uint64_t value) noexcept
{
	if (context == nullptr || instruction == nullptr)
	{
		return StepAction::Exit;
	}
	auto* services = static_cast<RuntimeServices*>(context->runtime_context);
	if (services == nullptr || services->notify_scalar_store == nullptr)
	{
		context->exit_reason = ExitReason::Bailout;
		context->scalar_memory_post_action = static_cast<uint32_t>(StepAction::Exit);
		if (context->state != nullptr) context->state->rip = instruction->rip + instruction->length;
		return StepAction::Exit;
	}
	StepAction action = services->notify_scalar_store(
	    services->opaque, context, instruction, guest_address, host_address, value);
	if (!services->notify_scalar_store_preserves_translations)
	{
		DiscardScalarMemoryTranslations(context);
	}
	if (action != StepAction::Continue && action != StepAction::Exit)
	{
		context->exit_reason = ExitReason::Bailout;
		action = StepAction::Exit;
	}
	if (context->scalar_memory_post_action != 0) action = StepAction::Exit;
	if (action == StepAction::Exit)
	{
		context->scalar_memory_post_action = static_cast<uint32_t>(StepAction::Exit);
		if (context->state != nullptr) context->state->rip = instruction->rip + instruction->length;
	}
	return action;
}

void RestoreDirectCallSnapshot(X64State* state,
	                           const X64CallStackTransaction& transaction) noexcept
{
	if (state == nullptr) return;
	if (transaction.active && transaction.slot_host_address != 0)
	{
		std::memcpy(reinterpret_cast<void*>(transaction.slot_host_address),
		            &transaction.original_slot_value,
		            sizeof(transaction.original_slot_value));
		state->gpr[4] = transaction.original_rsp;
	}
	state->host_call_stack_transaction = {};
}

void PublishDirectCallRollback(RuntimeServices* services, EntryContext* context,
	                           const IrInstruction* instruction,
	                           const X64CallStackTransaction& transaction) noexcept
{
	if (services == nullptr || services->notify_scalar_store == nullptr ||
	    context == nullptr || instruction == nullptr || !transaction.active ||
	    transaction.slot_host_address == 0)
	{
		return;
	}
	// Mirror X64RollbackHostCallStack's second RecordMemoryWrite. The result is
	// deliberately ignored because the malformed completion already requires a
	// fail-closed exit, but publishing the restoration prevents transient CALL
	// bytes decoded by re-entrant work from surviving in an SMC cache.
	services->notify_scalar_store(
	    services->opaque, context, instruction, transaction.slot_address,
	    transaction.slot_host_address, transaction.original_slot_value);
}

StepAction DispatchCompleteDirectCall(EntryContext* context,
	                                  const IrInstruction* instruction) noexcept
{
	if (context == nullptr || context->state == nullptr || instruction == nullptr ||
	    instruction->opcode != IrOpcode::CallImmediate)
	{
		return StepAction::Exit;
	}
	auto* services = static_cast<RuntimeServices*>(context->runtime_context);
	if (services == nullptr || services->complete_direct_call == nullptr)
	{
		// The native entry checks this before committing the push. Keep this guard
		// atomic as well if a malformed provider mutates its service table while a
		// callback is in flight.
		const X64CallStackTransaction transaction =
		    context->state->host_call_stack_transaction;
		RestoreDirectCallSnapshot(context->state, transaction);
		PublishDirectCallRollback(services, context, instruction, transaction);
		context->state->rip = instruction->rip;
		context->exit_reason = ExitReason::Bailout;
		context->scalar_memory_post_action = static_cast<uint32_t>(StepAction::Exit);
		DiscardScalarMemoryTranslations(context);
		return StepAction::Exit;
	}

	// A terminating CALL must complete its control transfer even when the stack
	// write invalidated the block. The dedicated service owns the final RIP,
	// transaction commit/rollback and exit reason.
	const X64CallStackTransaction transaction =
	    context->state->host_call_stack_transaction;
	context->scalar_memory_post_action = 0;
	const StepAction action =
	    services->complete_direct_call(services->opaque, context, instruction);
	if (action != StepAction::Exit ||
	    context->state->host_call_stack_transaction.active)
	{
		// A malformed provider must not strand a tentative push or poison the
		// next CALL. Restore the immutable pre-callback snapshot and stop without
		// replay; conforming providers always consume it and return Exit.
		RestoreDirectCallSnapshot(context->state, transaction);
		PublishDirectCallRollback(services, context, instruction, transaction);
		context->state->rip = instruction->rip;
		context->exit_reason = ExitReason::Bailout;
		context->scalar_memory_post_action =
		    static_cast<uint32_t>(StepAction::Exit);
	}
	DiscardScalarMemoryTranslations(context);
	return StepAction::Exit;
}

uint64_t DirectReferenceResultFlags(uint64_t value, uint8_t width)
{
	const uint64_t mask = width == 1 ? 0xffULL :
	                      (width == 2 ? 0xffffULL :
	                       (width == 4 ? 0xffffffffULL : ~0ULL));
	const uint64_t sign = width == 1 ? 0x80ULL :
	                      (width == 2 ? 0x8000ULL :
	                       (width == 4 ? 0x80000000ULL : 0x8000000000000000ULL));
	value &= mask;
	const uint64_t parity = (0x9669u >> (((value >> 4u) ^ value) & 0x0fu)) & 1u;
	return (value == 0 ? X64_FLAG_ZF : 0) | ((value & sign) != 0 ? X64_FLAG_SF : 0) |
	       (parity != 0 ? X64_FLAG_PF : 0);
}

bool DirectReferenceCondition(uint8_t condition, uint64_t flags)
{
	if (condition > 0x0f) return false;
	const uint32_t cf = (flags & X64_FLAG_CF) != 0;
	const uint32_t pf = (flags & X64_FLAG_PF) != 0;
	const uint32_t zf = (flags & X64_FLAG_ZF) != 0;
	const uint32_t sf = (flags & X64_FLAG_SF) != 0;
	const uint32_t of = (flags & X64_FLAG_OF) != 0;
	const uint32_t sl = sf ^ of;
	const uint32_t predicates = of | (cf << 1u) | (zf << 2u) |
	                            ((cf | zf) << 3u) | (sf << 4u) |
	                            (pf << 5u) | (sl << 6u) |
	                            ((zf | sl) << 7u);
	return ((((predicates >> (condition >> 1u)) ^ condition) & 1u) != 0);
}

uint64_t ExecuteDirectReferenceArithmetic(X64State* state, uint64_t lhs, uint64_t rhs,
	                                      uint8_t width, bool subtract)
{
	const uint64_t mask = width == 1 ? 0xffULL :
	                      (width == 2 ? 0xffffULL :
	                       (width == 4 ? 0xffffffffULL : ~0ULL));
	const uint64_t sign = width == 1 ? 0x80ULL :
	                      (width == 2 ? 0x8000ULL :
	                       (width == 4 ? 0x80000000ULL : 0x8000000000000000ULL));
	lhs &= mask;
	rhs &= mask;
	const uint64_t result = subtract ? (lhs - rhs) & mask : (lhs + rhs) & mask;
	uint64_t flags = DirectReferenceResultFlags(result, width);
	if (subtract ? lhs < rhs : result < lhs) flags |= X64_FLAG_CF;
	if (((lhs ^ rhs ^ result) & 0x10u) != 0) flags |= X64_FLAG_AF;
	if (subtract ? (((lhs ^ rhs) & (lhs ^ result) & sign) != 0)
	             : (((~(lhs ^ rhs)) & (lhs ^ result) & sign) != 0))
	{
		flags |= X64_FLAG_OF;
	}
	state->rflags = (state->rflags & ~X64_ARITHMETIC_FLAGS) | flags;
	return result;
}

uint64_t ExecuteDirectReferenceSubtractWithBorrow(
	X64State* state, uint64_t lhs, uint64_t rhs, uint8_t width)
{
	const uint64_t mask = width == 1 ? UINT64_C(0xff) : UINT64_C(0xffff);
	const uint64_t sign = width == 1 ? UINT64_C(0x80) : UINT64_C(0x8000);
	lhs &= mask;
	rhs &= mask;
	const uint64_t borrow = (state->rflags & X64_FLAG_CF) != 0 ? 1u : 0u;
	const uint64_t subtrahend = rhs + borrow;
	const uint64_t result = (lhs - subtrahend) & mask;
	uint64_t flags = DirectReferenceResultFlags(result, width);
	if (lhs < subtrahend) flags |= X64_FLAG_CF;
	if (((lhs ^ rhs ^ result) & 0x10u) != 0) flags |= X64_FLAG_AF;
	if (((lhs ^ rhs) & (lhs ^ result) & sign) != 0) flags |= X64_FLAG_OF;
	state->rflags = (state->rflags & ~X64_ARITHMETIC_FLAGS) | flags;
	return result;
}

void SetDirectReferenceMultiplyFlags(
	X64State* state, __int128 product, uint64_t result, uint8_t width)
{
	const __int128 truncated = width == 4
	                               ? static_cast<__int128>(
	                                     static_cast<int32_t>(static_cast<uint32_t>(result)))
	                               : static_cast<__int128>(static_cast<int64_t>(result));
	state->rflags &= ~(X64_FLAG_CF | X64_FLAG_OF);
	if (product != truncated) state->rflags |= X64_FLAG_CF | X64_FLAG_OF;
}

void WriteDirectRegister(X64State* state, uint8_t destination, uint8_t width, uint64_t value)
{
	if (width == 8)
	{
		state->gpr[destination] = value;
	} else if (width == 4)
	{
		state->gpr[destination] = static_cast<uint32_t>(value);
	} else
	{
		const uint64_t mask = width == 2 ? 0xffffULL : 0xffULL;
		state->gpr[destination] = (state->gpr[destination] & ~mask) | (value & mask);
	}
}

bool ExecuteDirectReferenceInstruction(X64State* state, const IrInstruction& instruction)
{
	if (state == nullptr || !IsDirectBasicBlockOpcode(instruction.opcode)) return false;
	switch (instruction.opcode)
	{
		case IrOpcode::Nop: return true;
		case IrOpcode::MoveRegister:
		{
			const uint64_t value = state->gpr[instruction.source];
			WriteDirectRegister(state, instruction.destination, instruction.width, value);
			return true;
		}
		case IrOpcode::MoveImmediate:
			WriteDirectRegister(state, instruction.destination, instruction.width,
			                    instruction.immediate);
			return true;
		case IrOpcode::ZeroExtendRegister:
		case IrOpcode::SignExtendRegister:
		{
			const uint8_t source_bits = instruction.source_width * 8u;
			const uint64_t source_mask = (uint64_t {1} << source_bits) - 1u;
			uint64_t value = state->gpr[instruction.source] & source_mask;
			if (instruction.opcode == IrOpcode::SignExtendRegister &&
			    (value & (uint64_t {1} << (source_bits - 1u))) != 0)
			{
				value |= ~source_mask;
			}
			state->gpr[instruction.destination] = instruction.width == 4
			                                               ? static_cast<uint32_t>(value)
			                                               : value;
			return true;
		}
		case IrOpcode::EffectiveAddress:
		{
			uint64_t value = instruction.address.absolute;
			if (instruction.address.base != IR_NO_REGISTER)
			{
				value = state->gpr[instruction.address.base];
				if (instruction.address.index != IR_NO_REGISTER)
				{
					value += state->gpr[instruction.address.index] << instruction.address.scale;
				}
				value += static_cast<uint64_t>(static_cast<int64_t>(instruction.address.displacement));
			}
			WriteDirectRegister(state, instruction.destination, instruction.width, value);
			return true;
		}
		case IrOpcode::ByteSwapRegister:
		{
			const uint64_t value = state->gpr[instruction.source];
			const uint64_t result = instruction.width == 4
			                            ? static_cast<uint64_t>(__builtin_bswap32(static_cast<uint32_t>(value)))
			                            : __builtin_bswap64(value);
			WriteDirectRegister(state, instruction.destination, instruction.width, result);
			return true;
		}
		case IrOpcode::BitwiseNot:
			WriteDirectRegister(state, instruction.destination, instruction.width,
			                    ~state->gpr[instruction.source]);
			return true;
		case IrOpcode::Negate:
		{
			const uint64_t result = ExecuteDirectReferenceArithmetic(
			    state, 0, state->gpr[instruction.source], instruction.width, true);
			WriteDirectRegister(state, instruction.destination, instruction.width, result);
			return true;
		}
		case IrOpcode::TestImmediate:
		{
			const uint64_t value =
			    instruction.condition == IR_TEST_IMMEDIATE_HIGH_BYTE
			        ? (state->gpr[instruction.source] >> 8u) & UINT64_C(0xff)
			        : state->gpr[instruction.source];
			const uint64_t result = value & instruction.immediate;
			constexpr uint64_t changed = X64_FLAG_CF | X64_FLAG_PF | X64_FLAG_ZF |
			                             X64_FLAG_SF | X64_FLAG_OF;
			state->rflags = (state->rflags & ~changed) |
			                DirectReferenceResultFlags(result, instruction.width);
			return true;
		}
		case IrOpcode::AndRegister:
		case IrOpcode::OrRegister:
		case IrOpcode::XorRegister:
		case IrOpcode::TestRegister:
		{
			const bool test = instruction.opcode == IrOpcode::TestRegister;
			const uint64_t lhs = state->gpr[test ? instruction.source : instruction.destination];
			const uint64_t rhs = state->gpr[test ? instruction.source2 : instruction.source];
			const uint64_t result = instruction.opcode == IrOpcode::OrRegister ? lhs | rhs :
			                        (instruction.opcode == IrOpcode::XorRegister ? lhs ^ rhs : lhs & rhs);
			if (!test)
			{
				WriteDirectRegister(state, instruction.destination, instruction.width, result);
			}
			constexpr uint64_t changed = X64_FLAG_CF | X64_FLAG_PF | X64_FLAG_ZF |
			                             X64_FLAG_SF | X64_FLAG_OF;
			state->rflags = (state->rflags & ~changed) |
			                DirectReferenceResultFlags(result, instruction.width);
			return true;
		}
		case IrOpcode::AddImmediate:
		case IrOpcode::SubImmediate:
		case IrOpcode::CompareImmediate:
		{
			const bool subtract = instruction.opcode != IrOpcode::AddImmediate;
			const uint64_t result = ExecuteDirectReferenceArithmetic(
			    state, state->gpr[instruction.destination], instruction.immediate,
			    instruction.width, subtract);
			if (instruction.opcode != IrOpcode::CompareImmediate)
			{
				WriteDirectRegister(state, instruction.destination, instruction.width, result);
			}
			return true;
		}
		case IrOpcode::SubtractWithBorrowRegister:
		{
			const uint64_t lhs = state->gpr[instruction.destination];
			const uint64_t rhs = state->gpr[instruction.source];
			const uint64_t result = ExecuteDirectReferenceSubtractWithBorrow(
			    state, lhs, rhs, instruction.width);
			WriteDirectRegister(state, instruction.destination, instruction.width, result);
			return true;
		}
		case IrOpcode::BitExtractRegister:
		{
			const uint32_t value = static_cast<uint32_t>(state->gpr[instruction.source]);
			const uint32_t control = static_cast<uint32_t>(state->gpr[instruction.source2]);
			const uint32_t start = control & 0xffu;
			const uint32_t length = (control >> 8u) & 0xffu;
			uint32_t result = 0;
			if (start < 32u && length != 0)
			{
				const uint32_t used = std::min(length, 32u - start);
				const uint32_t mask = used == 32u ? UINT32_MAX : (uint32_t {1} << used) - 1u;
				result = (value >> start) & mask;
			}
			WriteDirectRegister(state, instruction.destination, 4, result);
			state->rflags &= ~(X64_FLAG_CF | X64_FLAG_OF | X64_FLAG_SF | X64_FLAG_ZF);
			if (result == 0) state->rflags |= X64_FLAG_ZF;
			return true;
		}
		case IrOpcode::AndNotRegister:
		{
			const uint64_t result = ~state->gpr[instruction.source] &
			                        state->gpr[instruction.source2];
			WriteDirectRegister(state, instruction.destination, instruction.width, result);
			const uint64_t sign = instruction.width == 8
			                          ? UINT64_C(0x8000000000000000)
			                          : UINT64_C(0x80000000);
			state->rflags &= ~(X64_FLAG_CF | X64_FLAG_OF | X64_FLAG_SF | X64_FLAG_ZF);
			if ((result & (instruction.width == 4 ? UINT64_C(0xffffffff) : UINT64_MAX)) == 0)
				state->rflags |= X64_FLAG_ZF;
			if ((result & sign) != 0) state->rflags |= X64_FLAG_SF;
			return true;
		}
		case IrOpcode::SignedMultiplyRegister:
		case IrOpcode::SignedMultiplyImmediate:
		{
			const bool width64 = instruction.width == 8;
			const uint64_t lhs_bits = state->gpr[instruction.source];
			const int64_t lhs = width64 ? static_cast<int64_t>(lhs_bits)
			                            : static_cast<int32_t>(lhs_bits);
			int64_t rhs = 0;
			if (instruction.opcode == IrOpcode::SignedMultiplyRegister)
			{
				const uint64_t rhs_bits = state->gpr[instruction.source2];
				rhs = width64 ? static_cast<int64_t>(rhs_bits) : static_cast<int32_t>(rhs_bits);
			} else
			{
				rhs = width64 ? static_cast<int64_t>(instruction.immediate)
				              : static_cast<int32_t>(instruction.immediate);
			}
			const __int128 product = static_cast<__int128>(lhs) * rhs;
			const uint64_t result = static_cast<uint64_t>(product);
			WriteDirectRegister(state, instruction.destination, instruction.width, result);
			SetDirectReferenceMultiplyFlags(state, product, result, instruction.width);
			return true;
		}
		case IrOpcode::AddRegister:
		case IrOpcode::SubRegister:
		case IrOpcode::CompareRegister:
		{
			const bool subtract = instruction.opcode != IrOpcode::AddRegister;
			const uint64_t lhs = state->gpr[instruction.destination];
			const uint64_t rhs = state->gpr[instruction.source];
			const uint64_t result = ExecuteDirectReferenceArithmetic(
			    state, lhs, rhs, instruction.width, subtract);
			if (instruction.opcode != IrOpcode::CompareRegister)
			{
				WriteDirectRegister(state, instruction.destination, instruction.width, result);
			}
			return true;
		}
		case IrOpcode::ShiftLeftImmediate:
		case IrOpcode::ShiftLeftCl:
		case IrOpcode::ShiftRightLogicalImmediate:
		case IrOpcode::ShiftRightLogicalCl:
		case IrOpcode::ShiftRightArithmeticImmediate:
		{
			const bool rotate_left =
			    instruction.opcode == IrOpcode::ShiftLeftImmediate &&
			    instruction.condition == IR_SHIFT_IMMEDIATE_ROTATE_LEFT;
			if ((instruction.condition != 0 && !rotate_left) ||
			    (rotate_left && instruction.width != 4 && instruction.width != 8))
			{
				return false;
			}
			const bool shift_left = instruction.opcode == IrOpcode::ShiftLeftImmediate ||
			                        instruction.opcode == IrOpcode::ShiftLeftCl;
			const bool arithmetic = instruction.opcode == IrOpcode::ShiftRightArithmeticImmediate;
			const bool from_cl = instruction.opcode == IrOpcode::ShiftLeftCl ||
			                     instruction.opcode == IrOpcode::ShiftRightLogicalCl;
			const uint8_t count_mask = instruction.width == 8 ? 63u : 31u;
			// CL is architecturally sampled before a possible write to RCX itself.
			const uint8_t count = static_cast<uint8_t>(
			    from_cl ? state->gpr[1] : instruction.immediate) & count_mask;
			if (count == 0) return true;

			const uint8_t bits = instruction.width * 8u;
			const uint64_t width_mask = instruction.width == 1 ? UINT64_C(0xff) :
			                            (instruction.width == 4 ? UINT64_C(0xffffffff) : UINT64_MAX);
			const uint64_t value = state->gpr[instruction.destination] & width_mask;
			uint64_t result = 0;
			if (rotate_left)
			{
				result = ((value << count) | (value >> (bits - count))) & width_mask;
			} else if (shift_left)
			{
				result = (value << count) & width_mask;
			} else if (arithmetic && instruction.width == 8)
			{
				result = static_cast<uint64_t>(static_cast<int64_t>(value) >> count);
			} else if (arithmetic)
			{
				result = static_cast<uint32_t>(static_cast<int32_t>(value) >> count);
			} else
			{
				result = value >> count;
			}
			const bool carry = shift_left ? ((value >> (bits - count)) & 1u) != 0
			                              : ((value >> (count - 1u)) & 1u) != 0;
			if (rotate_left)
			{
				state->rflags = (state->rflags & ~X64_FLAG_CF) |
				                (carry ? X64_FLAG_CF : 0);
			} else
			{
				state->rflags = (state->rflags &
				                 ~(X64_FLAG_CF | X64_FLAG_PF | X64_FLAG_ZF | X64_FLAG_SF)) |
				                DirectReferenceResultFlags(result, instruction.width) |
				                (carry ? X64_FLAG_CF : 0);
			}
			if (count == 1)
			{
				const bool overflow = shift_left
				                          ? (((result >> (bits - 1u)) & 1u) !=
				                             static_cast<uint64_t>(carry))
				                          : (!arithmetic && ((value >> (bits - 1u)) & 1u) != 0);
				if (overflow) state->rflags |= X64_FLAG_OF;
				else state->rflags &= ~X64_FLAG_OF;
			}
			WriteDirectRegister(state, instruction.destination, instruction.width, result);
			return true;
		}
		case IrOpcode::AndImmediate:
		case IrOpcode::OrImmediate:
		case IrOpcode::XorImmediate:
		{
			const uint64_t lhs = state->gpr[instruction.destination];
			const uint64_t result = instruction.opcode == IrOpcode::OrImmediate
			                            ? lhs | instruction.immediate
			                        : instruction.opcode == IrOpcode::XorImmediate
			                            ? lhs ^ instruction.immediate
			                            : lhs & instruction.immediate;
			WriteDirectRegister(state, instruction.destination, instruction.width, result);
			constexpr uint64_t changed = X64_FLAG_CF | X64_FLAG_PF | X64_FLAG_ZF |
			                             X64_FLAG_SF | X64_FLAG_OF;
			state->rflags = (state->rflags & ~changed) |
			                DirectReferenceResultFlags(result, instruction.width);
			return true;
		}
		case IrOpcode::BitTestRegister:
		case IrOpcode::BitTestImmediate:
		{
			const uint64_t value = state->gpr[instruction.source];
			const uint32_t bit = instruction.opcode == IrOpcode::BitTestImmediate
			                         ? static_cast<uint32_t>(instruction.immediate)
			                         : static_cast<uint32_t>(state->gpr[instruction.source2]) &
			                               (instruction.width == 8 ? 63u : 31u);
			if (((value >> bit) & 1u) != 0) state->rflags |= X64_FLAG_CF;
			else state->rflags &= ~X64_FLAG_CF;
			return true;
		}
		case IrOpcode::Increment:
		case IrOpcode::Decrement:
		{
			const uint64_t old_cf = state->rflags & X64_FLAG_CF;
			const bool decrement = instruction.opcode == IrOpcode::Decrement;
			const uint64_t result = ExecuteDirectReferenceArithmetic(
			    state, state->gpr[instruction.destination], 1, instruction.width,
			    decrement);
			WriteDirectRegister(state, instruction.destination, instruction.width, result);
			state->rflags = (state->rflags & ~X64_FLAG_CF) | old_cf;
			return true;
		}
		case IrOpcode::SetCondition:
			WriteDirectRegister(state, instruction.destination, 1,
			                    DirectReferenceCondition(instruction.condition, state->rflags) ? 1 : 0);
			return true;
		case IrOpcode::ConditionalMove:
		{
			const bool take = DirectReferenceCondition(instruction.condition, state->rflags);
			if (take || instruction.width == 4)
			{
				const uint64_t value = take ? state->gpr[instruction.source]
				                            : state->gpr[instruction.destination];
				WriteDirectRegister(state, instruction.destination, instruction.width, value);
			}
			return true;
		}
		case IrOpcode::ClearDirectionFlag:
			state->rflags &= ~X64_FLAG_DF;
			return true;
		case IrOpcode::BranchCondition:
			state->rip = DirectReferenceCondition(instruction.condition, state->rflags)
			                 ? instruction.target
			                 : instruction.rip + instruction.length;
			return true;
		case IrOpcode::JumpImmediate:
			state->rip = instruction.target;
			return true;
		case IrOpcode::VectorMoveGpr32ToXmm:
			state->xmm[instruction.destination][0] =
			    static_cast<uint32_t>(state->gpr[instruction.source]);
			state->xmm[instruction.destination][1] = 0;
			state->xmm[instruction.destination][2] = 0;
			state->xmm[instruction.destination][3] = 0;
			return true;
		case IrOpcode::VectorPermuteDwordsImmediate:
		{
			uint32_t source[4] {};
			uint32_t result[4] {};
			std::memcpy(source, state->xmm[instruction.source], sizeof(source));
			const uint8_t selector = static_cast<uint8_t>(instruction.immediate);
			for (uint8_t element = 0; element < 4; element++)
			{
				result[element] = source[(selector >> (element * 2u)) & 3u];
			}
			std::memcpy(state->xmm[instruction.destination], result, sizeof(result));
			state->xmm[instruction.destination][2] = 0;
			state->xmm[instruction.destination][3] = 0;
			return true;
		}
		case IrOpcode::VectorMoveRegister:
		{
			const size_t qword_count = instruction.width / sizeof(uint64_t);
			uint64_t value[4] {};
			// Snapshot every source lane before the first destination write so the
			// four-lane path remains correct for dst == src.
			std::memcpy(value, state->xmm[instruction.source],
			            qword_count * sizeof(uint64_t));
			std::memcpy(state->xmm[instruction.destination], value,
			            qword_count * sizeof(uint64_t));
			if (HasVectorFlag(instruction, IrVectorFlag::ZeroUpper))
			{
				state->xmm[instruction.destination][2] = 0;
				state->xmm[instruction.destination][3] = 0;
			}
			return true;
		}
		case IrOpcode::VectorMoveScalarRegister:
		{
			const uint64_t merge_low = state->xmm[instruction.source][0];
			const uint64_t merge_high = state->xmm[instruction.source][1];
			const uint64_t scalar = state->xmm[instruction.source2][0];
			state->xmm[instruction.destination][0] = instruction.width == 4
			                                                  ? (merge_low & UINT64_C(0xffffffff00000000)) |
			                                                        (scalar & UINT64_C(0xffffffff))
			                                                  : scalar;
			state->xmm[instruction.destination][1] = merge_high;
			state->xmm[instruction.destination][2] = 0;
			state->xmm[instruction.destination][3] = 0;
			return true;
		}
		case IrOpcode::VectorXorRegister:
		{
			const size_t qword_count = instruction.width / sizeof(uint64_t);
			uint64_t lhs[4] {};
			uint64_t rhs[4] {};
			uint64_t result[4] {};
			// Preload both complete guest operands: destination may alias either.
			std::memcpy(lhs, state->xmm[instruction.source],
			            qword_count * sizeof(uint64_t));
			std::memcpy(rhs, state->xmm[instruction.source2],
			            qword_count * sizeof(uint64_t));
			for (size_t lane = 0; lane < qword_count; lane++)
				result[lane] = lhs[lane] ^ rhs[lane];
			std::memcpy(state->xmm[instruction.destination], result,
			            qword_count * sizeof(uint64_t));
			if (HasVectorFlag(instruction, IrVectorFlag::ZeroUpper))
			{
				state->xmm[instruction.destination][2] = 0;
				state->xmm[instruction.destination][3] = 0;
			}
			return true;
		}
		default: return false;
	}
}

template <class Function>
uint64_t FunctionAddress(Function function)
{
	static_assert(sizeof(function) <= sizeof(uint64_t), "unsupported function-pointer representation");
	uint64_t result = 0;
	std::memcpy(&result, &function, sizeof(function));
	return result;
}

enum class GuestEntryMode : uint8_t
{
	FramedContext,
	LeafContext,
	DenseContextless,
};

static_assert(IR_NO_REGISTER > 30,
	          "contextless EntryContext sentinel must fail A64 register validation");

class GuestCodeWriter final
{
public:
	GuestCodeWriter(void* storage, size_t capacity, bool compact_immediates = false)
	    : m_storage(static_cast<uint8_t*>(storage)), m_capacity(capacity), m_compact_immediates(compact_immediates)
	{
	}

	bool Emit(uint32_t word)
	{
		if (m_storage == nullptr || m_size > m_capacity || A64_WORD_SIZE > m_capacity - m_size) return false;
		m_storage[m_size + 0] = static_cast<uint8_t>(word);
		m_storage[m_size + 1] = static_cast<uint8_t>(word >> 8u);
		m_storage[m_size + 2] = static_cast<uint8_t>(word >> 16u);
		m_storage[m_size + 3] = static_cast<uint8_t>(word >> 24u);
		m_size += A64_WORD_SIZE;
		return true;
	}

	bool EmitLoad64(uint8_t destination, uint8_t base, uint32_t byte_offset)
	{
		if (base == StateRegister() && m_block_rflags_cache &&
		    byte_offset == static_cast<uint32_t>(offsetof(X64State, rflags)))
		{
			return EmitOrr64(destination, 31, 5);
		}
		if (base == StateRegister() && m_block_gpr_cache_guest < 16 &&
		    byte_offset == static_cast<uint32_t>(offsetof(X64State, gpr)) +
		                       m_block_gpr_cache_guest * sizeof(uint64_t))
		{
			return EmitOrr64(destination, 31, 6);
		}
		return EmitLoad64Uncached(destination, base, byte_offset);
	}

	bool EmitLoad64Uncached(uint8_t destination, uint8_t base, uint32_t byte_offset)
	{
		return destination <= 30 && base <= 30 && (byte_offset % 8u) == 0 && byte_offset / 8u <= 4095u &&
		       Emit(0xf9400000u | ((byte_offset / 8u) << 10u) | (static_cast<uint32_t>(base) << 5u) | destination);
	}

	bool EmitLoad32(uint8_t destination, uint8_t base, uint32_t byte_offset)
	{
		if (base == StateRegister() && m_block_gpr_cache_guest < 16 &&
		    byte_offset == static_cast<uint32_t>(offsetof(X64State, gpr)) +
		                       m_block_gpr_cache_guest * sizeof(uint64_t))
		{
			return EmitMoveRegister32(destination, 6);
		}
		return destination <= 30 && base <= 30 && (byte_offset % 4u) == 0 && byte_offset / 4u <= 4095u &&
		       Emit(A64_LDR_W | ((byte_offset / 4u) << 10u) | (static_cast<uint32_t>(base) << 5u) | destination);
	}

	bool EmitLoadScalar(uint8_t destination, uint8_t base, uint8_t width)
	{
		if (destination > 30 || base > 30) return false;
		const uint32_t opcode = width == 1 ? 0x39400000u :
		                        (width == 2 ? 0x79400000u :
		                         (width == 4 ? A64_LDR_W :
		                          (width == 8 ? 0xf9400000u : 0u)));
		return opcode != 0 && Emit(opcode | (static_cast<uint32_t>(base) << 5u) | destination);
	}

	bool EmitLoadVector128(uint8_t destination, uint8_t base, uint32_t byte_offset)
	{
		return destination <= 31 && base <= 30 && (byte_offset % 16u) == 0 && byte_offset / 16u <= 4095u &&
		       Emit(0x3dc00000u | ((byte_offset / 16u) << 10u) |
		            (static_cast<uint32_t>(base) << 5u) | destination);
	}

	bool EmitStore64(uint8_t source, uint8_t base, uint32_t byte_offset)
	{
		if (base == StateRegister() && m_block_rflags_cache &&
		    byte_offset == static_cast<uint32_t>(offsetof(X64State, rflags)))
		{
			return EmitOrr64(5, 31, source);
		}
		if (base == StateRegister() && m_block_gpr_cache_guest < 16 &&
		    byte_offset == static_cast<uint32_t>(offsetof(X64State, gpr)) +
		                       m_block_gpr_cache_guest * sizeof(uint64_t))
		{
			return EmitOrr64(6, 31, source);
		}
		return EmitStore64Uncached(source, base, byte_offset);
	}

	bool EmitStore64Uncached(uint8_t source, uint8_t base, uint32_t byte_offset)
	{
		return source <= 31 && base <= 30 && (byte_offset % 8u) == 0 && byte_offset / 8u <= 4095u &&
		       Emit(0xf9000000u | ((byte_offset / 8u) << 10u) | (static_cast<uint32_t>(base) << 5u) | source);
	}

	bool EmitStore32(uint8_t source, uint8_t base, uint32_t byte_offset)
	{
		return source <= 31 && base <= 30 && (byte_offset % 4u) == 0 && byte_offset / 4u <= 4095u &&
		       Emit(A64_STR_W | ((byte_offset / 4u) << 10u) | (static_cast<uint32_t>(base) << 5u) | source);
	}

	bool EmitStoreScalar(uint8_t source, uint8_t base, uint8_t width)
	{
		if (source > 31 || base > 30) return false;
		const uint32_t opcode = width == 1 ? 0x39000000u :
		                        (width == 2 ? 0x79000000u :
		                         (width == 4 ? A64_STR_W :
		                          (width == 8 ? 0xf9000000u : 0u)));
		return opcode != 0 && Emit(opcode | (static_cast<uint32_t>(base) << 5u) | source);
	}

	bool EmitStoreVector128(uint8_t source, uint8_t base, uint32_t byte_offset)
	{
		return source <= 31 && base <= 30 && (byte_offset % 16u) == 0 && byte_offset / 16u <= 4095u &&
		       Emit(0x3d800000u | ((byte_offset / 16u) << 10u) |
		            (static_cast<uint32_t>(base) << 5u) | source);
	}

	bool EmitXorVector128(uint8_t destination, uint8_t lhs, uint8_t rhs)
	{
		return destination <= 31 && lhs <= 31 && rhs <= 31 &&
		       Emit(0x6e201c00u | (static_cast<uint32_t>(rhs) << 16u) |
		            (static_cast<uint32_t>(lhs) << 5u) | destination);
	}

	bool EmitDuplicateVectorDword(uint8_t destination, uint8_t source,
	                              uint8_t source_lane)
	{
		return destination <= 31 && source <= 31 && source_lane < 4 &&
		       Emit(0x4e040400u | (static_cast<uint32_t>(source_lane) << 19u) |
		            (static_cast<uint32_t>(source) << 5u) | destination);
	}

	bool EmitInsertVectorDword(uint8_t destination, uint8_t destination_lane,
	                           uint8_t source, uint8_t source_lane)
	{
		if (destination > 31 || source > 31 || destination_lane >= 4 || source_lane >= 4)
		{
			return false;
		}
		const uint32_t destination_imm5 = 4u | (static_cast<uint32_t>(destination_lane) << 3u);
		const uint32_t source_imm4 = static_cast<uint32_t>(source_lane) << 2u;
		return Emit(0x6e000400u | (destination_imm5 << 16u) | (source_imm4 << 11u) |
		            (static_cast<uint32_t>(source) << 5u) | destination);
	}

	bool EmitAddImmediate(uint8_t destination, uint8_t source, uint16_t immediate, bool width64)
	{
		if (destination > 30 || source > 30 || immediate > 4095) return false;
		return Emit((width64 ? 0x91000000u : 0x11000000u) |
		            (static_cast<uint32_t>(immediate) << 10u) |
		            (static_cast<uint32_t>(source) << 5u) | destination);
	}

	bool EmitAddImmediateFlags64(uint8_t destination, uint8_t source, uint16_t immediate)
	{
		return destination <= 30 && source <= 30 && immediate <= 4095 &&
		       Emit(0xb1000000u | (static_cast<uint32_t>(immediate) << 10u) |
		            (static_cast<uint32_t>(source) << 5u) | destination);
	}

	bool EmitAddSubFlags(uint8_t destination, uint8_t lhs, uint8_t rhs, bool subtract, bool width64)
	{
		if (destination > 30 || lhs > 30 || rhs > 30) return false;
		const uint32_t base = subtract ? (width64 ? 0xeb000000u : 0x6b000000u)
		                               : (width64 ? 0xab000000u : 0x2b000000u);
		return Emit(base | (static_cast<uint32_t>(rhs) << 16u) |
		            (static_cast<uint32_t>(lhs) << 5u) | destination);
	}

	bool EmitSubtractWithCarryFlags(uint8_t destination, uint8_t lhs, uint8_t rhs,
	                                bool width64)
	{
		return destination <= 30 && lhs <= 30 && rhs <= 30 &&
		       Emit((width64 ? 0xfa000000u : 0x7a000000u) |
		            (static_cast<uint32_t>(rhs) << 16u) |
		            (static_cast<uint32_t>(lhs) << 5u) | destination);
	}

	bool EmitSetCarryToInverseBit32(uint8_t source)
	{
		// SUBS WZR, WZR, Wsource publishes C=1 exactly when source is zero.
		return source <= 30 &&
		       Emit(0x6b0003ffu | (static_cast<uint32_t>(source) << 16u));
	}

	bool EmitAddShifted64(uint8_t destination, uint8_t lhs, uint8_t rhs, uint8_t shift)
	{
		if (destination > 30 || lhs > 30 || rhs > 30 || shift > 63) return false;
		return Emit(0x8b000000u | (static_cast<uint32_t>(rhs) << 16u) |
		            (static_cast<uint32_t>(shift) << 10u) |
		            (static_cast<uint32_t>(lhs) << 5u) | destination);
	}

	bool EmitSubtract64(uint8_t destination, uint8_t lhs, uint8_t rhs)
	{
		return destination <= 30 && lhs <= 30 && rhs <= 30 &&
		       Emit(0xcb000000u | (static_cast<uint32_t>(rhs) << 16u) |
		            (static_cast<uint32_t>(lhs) << 5u) | destination);
	}

	bool EmitCompare64(uint8_t lhs, uint8_t rhs)
	{
		return lhs <= 30 && rhs <= 30 &&
		       Emit(0xeb00001fu | (static_cast<uint32_t>(rhs) << 16u) |
		            (static_cast<uint32_t>(lhs) << 5u));
	}

#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
	bool EmitCompare32(uint8_t lhs, uint8_t rhs)
	{
		return lhs <= 30 && rhs <= 30 &&
		       Emit(0x6b00001fu | (static_cast<uint32_t>(rhs) << 16u) |
		            (static_cast<uint32_t>(lhs) << 5u));
	}
#endif

	bool EmitCompareImmediate(uint8_t source, uint16_t immediate, bool width64)
	{
		return source <= 30 && immediate <= 4095 &&
		       Emit((width64 ? 0xf100001fu : 0x7100001fu) |
		            (static_cast<uint32_t>(immediate) << 10u) |
		            (static_cast<uint32_t>(source) << 5u));
	}

	bool EmitLogicalShiftRight64(uint8_t destination, uint8_t source, uint8_t shift)
	{
		return destination <= 30 && source <= 30 && shift < 64 &&
		       Emit(0xd340fc00u | (static_cast<uint32_t>(shift) << 16u) |
		            (static_cast<uint32_t>(source) << 5u) | destination);
	}

	bool EmitLogicalShiftRight32(uint8_t destination, uint8_t source, uint8_t shift)
	{
		return destination <= 30 && source <= 30 && shift < 32 &&
		       Emit(0x53007c00u | (static_cast<uint32_t>(shift) << 16u) |
		            (static_cast<uint32_t>(source) << 5u) | destination);
	}

	bool EmitLogicalShiftLeft32(uint8_t destination, uint8_t source, uint8_t shift)
	{
		if (destination > 30 || source > 30 || shift >= 32) return false;
		const uint32_t immr = (32u - shift) & 31u;
		const uint32_t imms = 31u - shift;
		return Emit(0x53000000u | (immr << 16u) | (imms << 10u) |
		            (static_cast<uint32_t>(source) << 5u) | destination);
	}

	bool EmitLogicalShiftLeft64(uint8_t destination, uint8_t source, uint8_t shift)
	{
		if (destination > 30 || source > 30 || shift >= 64) return false;
		const uint32_t immr = (64u - shift) & 63u;
		const uint32_t imms = 63u - shift;
		return Emit(0xd3400000u | (immr << 16u) | (imms << 10u) |
		            (static_cast<uint32_t>(source) << 5u) | destination);
	}

	bool EmitArithmeticShiftRight32(uint8_t destination, uint8_t source, uint8_t shift)
	{
		return destination <= 30 && source <= 30 && shift < 32 &&
		       Emit(0x13007c00u | (static_cast<uint32_t>(shift) << 16u) |
		            (static_cast<uint32_t>(source) << 5u) | destination);
	}

	bool EmitArithmeticShiftRight64(uint8_t destination, uint8_t source, uint8_t shift)
	{
		return destination <= 30 && source <= 30 && shift < 64 &&
		       Emit(0x9340fc00u | (static_cast<uint32_t>(shift) << 16u) |
		            (static_cast<uint32_t>(source) << 5u) | destination);
	}

	bool EmitLogicalShiftLeftVariable32(uint8_t destination, uint8_t source, uint8_t shift_register)
	{
		return destination <= 30 && source <= 30 && shift_register <= 30 &&
		       Emit(0x1ac02000u | (static_cast<uint32_t>(shift_register) << 16u) |
		            (static_cast<uint32_t>(source) << 5u) | destination);
	}

	bool EmitLogicalShiftLeftVariable64(uint8_t destination, uint8_t source, uint8_t shift_register)
	{
		return destination <= 30 && source <= 30 && shift_register <= 30 &&
		       Emit(0x9ac02000u | (static_cast<uint32_t>(shift_register) << 16u) |
		            (static_cast<uint32_t>(source) << 5u) | destination);
	}

	bool EmitLogicalShiftRightVariable32(uint8_t destination, uint8_t source, uint8_t shift_register)
	{
		return destination <= 30 && source <= 30 && shift_register <= 30 &&
		       Emit(0x1ac02400u | (static_cast<uint32_t>(shift_register) << 16u) |
		            (static_cast<uint32_t>(source) << 5u) | destination);
	}

	bool EmitLogicalShiftRightVariable64(uint8_t destination, uint8_t source, uint8_t shift_register)
	{
		return destination <= 30 && source <= 30 && shift_register <= 30 &&
		       Emit(0x9ac02400u | (static_cast<uint32_t>(shift_register) << 16u) |
		            (static_cast<uint32_t>(source) << 5u) | destination);
	}

	bool EmitShifted64(uint32_t base, uint8_t destination, uint8_t lhs, uint8_t rhs,
	                   uint8_t shift, uint8_t amount)
	{
		if (destination > 30 || lhs > 31 || rhs > 30 || shift > 1 || amount > 63) return false;
		return Emit(base | (static_cast<uint32_t>(shift) << 22u) |
		            (static_cast<uint32_t>(rhs) << 16u) |
		            (static_cast<uint32_t>(amount) << 10u) |
		            (static_cast<uint32_t>(lhs) << 5u) | destination);
	}

	bool EmitEor64(uint8_t destination, uint8_t lhs, uint8_t rhs, uint8_t shift = 0, uint8_t amount = 0)
	{
		return EmitShifted64(0xca000000u, destination, lhs, rhs, shift, amount);
	}

	bool EmitEor32(uint8_t destination, uint8_t lhs, uint8_t rhs)
	{
		return destination <= 30 && lhs <= 31 && rhs <= 30 &&
		       Emit(0x4a000000u | (static_cast<uint32_t>(rhs) << 16u) |
		            (static_cast<uint32_t>(lhs) << 5u) | destination);
	}

	bool EmitAnd32(uint8_t destination, uint8_t lhs, uint8_t rhs)
	{
		return destination <= 30 && lhs <= 31 && rhs <= 30 &&
		       Emit(0x0a000000u | (static_cast<uint32_t>(rhs) << 16u) |
		            (static_cast<uint32_t>(lhs) << 5u) | destination);
	}

	bool EmitAnd64(uint8_t destination, uint8_t lhs, uint8_t rhs)
	{
		return destination <= 30 && lhs <= 31 && rhs <= 30 &&
		       Emit(0x8a000000u | (static_cast<uint32_t>(rhs) << 16u) |
		            (static_cast<uint32_t>(lhs) << 5u) | destination);
	}

	bool EmitOrr64(uint8_t destination, uint8_t lhs, uint8_t rhs, uint8_t shift = 0, uint8_t amount = 0)
	{
		return EmitShifted64(0xaa000000u, destination, lhs, rhs, shift, amount);
	}

	bool EmitOrr32(uint8_t destination, uint8_t lhs, uint8_t rhs)
	{
		return destination <= 30 && lhs <= 31 && rhs <= 30 &&
		       Emit(0x2a000000u | (static_cast<uint32_t>(rhs) << 16u) |
		            (static_cast<uint32_t>(lhs) << 5u) | destination);
	}

	bool EmitBic64(uint8_t destination, uint8_t lhs, uint8_t rhs)
	{
		return EmitShifted64(0x8a200000u, destination, lhs, rhs, 0, 0);
	}

	bool EmitBic32(uint8_t destination, uint8_t lhs, uint8_t rhs)
	{
		return destination <= 30 && lhs <= 31 && rhs <= 30 &&
		       Emit(0x0a200000u | (static_cast<uint32_t>(rhs) << 16u) |
		            (static_cast<uint32_t>(lhs) << 5u) | destination);
	}

	bool EmitMvn64(uint8_t destination, uint8_t source)
	{
		return destination <= 30 && source <= 30 &&
		       Emit(0xaa2003e0u | (static_cast<uint32_t>(source) << 16u) | destination);
	}

	bool EmitMvn32(uint8_t destination, uint8_t source)
	{
		return destination <= 30 && source <= 30 &&
		       Emit(0x2a2003e0u | (static_cast<uint32_t>(source) << 16u) | destination);
	}

	bool EmitUbfx64(uint8_t destination, uint8_t source, uint8_t lsb, uint8_t width)
	{
		if (destination > 30 || source > 30 || width == 0 || lsb >= 64 || width > 64 - lsb) return false;
		return Emit(0xd3400000u | (static_cast<uint32_t>(lsb) << 16u) |
		            (static_cast<uint32_t>(lsb + width - 1u) << 10u) |
		            (static_cast<uint32_t>(source) << 5u) | destination);
	}

	bool EmitExtendRegister(uint8_t destination, uint8_t source, uint8_t source_width,
	                        uint8_t destination_width, bool sign_extend)
	{
		if (destination > 30 || source > 30 ||
		    (source_width != 1 && source_width != 2 && source_width != 4) ||
		    (destination_width != 4 && destination_width != 8) || source_width >= destination_width)
		{
			return false;
		}
		const uint32_t base = sign_extend ? (destination_width == 8 ? 0x93400000u : 0x13000000u)
		                                  : (destination_width == 8 ? 0xd3400000u : 0x53000000u);
		const uint32_t imms = static_cast<uint32_t>(source_width * 8u - 1u);
		return Emit(base | (imms << 10u) | (static_cast<uint32_t>(source) << 5u) | destination);
	}

	bool EmitInsertLow64(uint8_t destination, uint8_t source, uint8_t width)
	{
		if (destination > 30 || source > 30 ||
		    (width != 1 && width != 2 && width != 4))
		{
			return false;
		}
		const uint32_t imms = static_cast<uint32_t>(width * 8u - 1u);
		// BFM with immr=0 is the BFXIL alias: replace only the low field of
		// destination and preserve every other guest-register bit.
		return Emit(0xb3400000u | (imms << 10u) |
		            (static_cast<uint32_t>(source) << 5u) | destination);
	}

	bool EmitByteSwap(uint8_t destination, uint8_t source, uint8_t width)
	{
		if (destination > 30 || source > 30 || (width != 4 && width != 8)) return false;
		const uint32_t base = width == 8 ? 0xdac00c00u : 0x5ac00800u;
		return Emit(base | (static_cast<uint32_t>(source) << 5u) | destination);
	}

	bool EmitMrsNzcv(uint8_t destination)
	{
		return destination <= 30 && Emit(0xd53b4200u | destination);
	}

	bool EmitCompareZero64(uint8_t source)
	{
		return source <= 30 && Emit(0xf100001fu | (static_cast<uint32_t>(source) << 5u));
	}

	bool EmitConditionalSelect64(uint8_t destination, uint8_t if_true, uint8_t if_false, uint8_t condition)
	{
		return destination <= 30 && if_true <= 31 && if_false <= 31 && condition <= 15 &&
		       Emit(0x9a800000u | (static_cast<uint32_t>(if_false) << 16u) |
		            (static_cast<uint32_t>(condition) << 12u) |
		            (static_cast<uint32_t>(if_true) << 5u) | destination);
	}

	bool EmitMoveRegister32(uint8_t destination, uint8_t source)
	{
		return destination <= 30 && source <= 30 &&
		       Emit(0x2a0003e0u | (static_cast<uint32_t>(source) << 16u) | destination);
	}

#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
	bool EmitLoadAcquire32(uint8_t destination, uint8_t base)
	{
		return destination <= 30 && base <= 30 &&
		       Emit(0x88dffc00u | (static_cast<uint32_t>(base) << 5u) |
		            destination);
	}

	bool EmitStoreRelease32(uint8_t source, uint8_t base)
	{
		return source <= 31 && base <= 30 &&
		       Emit(0x889ffc00u | (static_cast<uint32_t>(base) << 5u) |
		            source);
	}
#endif

	bool EmitSignedMultiplyLong32(uint8_t destination, uint8_t lhs, uint8_t rhs)
	{
		return destination <= 30 && lhs <= 30 && rhs <= 30 &&
		       Emit(0x9b207c00u | (static_cast<uint32_t>(rhs) << 16u) |
		            (static_cast<uint32_t>(lhs) << 5u) | destination);
	}

	bool EmitMultiply64(uint8_t destination, uint8_t lhs, uint8_t rhs)
	{
		return destination <= 30 && lhs <= 30 && rhs <= 30 &&
		       Emit(0x9b007c00u | (static_cast<uint32_t>(rhs) << 16u) |
		            (static_cast<uint32_t>(lhs) << 5u) | destination);
	}

	bool EmitSignedMultiplyHigh64(uint8_t destination, uint8_t lhs, uint8_t rhs)
	{
		return destination <= 30 && lhs <= 30 && rhs <= 30 &&
		       Emit(0x9b407c00u | (static_cast<uint32_t>(rhs) << 16u) |
		            (static_cast<uint32_t>(lhs) << 5u) | destination);
	}

	bool EmitImmediate64(uint8_t destination, uint64_t value)
	{
		if (destination > 30) return false;
		if (!m_compact_immediates)
		{
			if (!Emit(A64_MOVZ_64 | (static_cast<uint32_t>(value & 0xffffu) << 5u) | destination))
			{
				return false;
			}
			for (uint32_t halfword = 1; halfword < 4; halfword++)
			{
				const uint32_t immediate = static_cast<uint32_t>((value >> (halfword * 16u)) & 0xffffu);
				if (!Emit(A64_MOVK_64 | (halfword << 21u) | (immediate << 5u) | destination))
				{
					return false;
				}
			}
			return true;
		}

		std::array<uint16_t, 4> halfwords {};
		uint32_t                movz_words = 0;
		uint32_t                movn_words = 0;
		for (uint32_t halfword = 0; halfword < halfwords.size(); halfword++)
		{
			halfwords[halfword] = static_cast<uint16_t>((value >> (halfword * 16u)) & 0xffffu);
			movz_words += halfwords[halfword] != 0 ? 1u : 0u;
			movn_words += halfwords[halfword] != UINT16_MAX ? 1u : 0u;
		}
		movz_words                   = (std::max)(movz_words, 1u);
		movn_words                   = (std::max)(movn_words, 1u);
		const bool     use_movn      = movn_words < movz_words;
		const uint16_t fill          = use_movn ? UINT16_MAX : uint16_t {0};
		uint32_t       seed_halfword = 0;
		while (seed_halfword < halfwords.size() && halfwords[seed_halfword] == fill)
		{
			seed_halfword++;
		}
		if (seed_halfword == halfwords.size()) seed_halfword = 0;
		const uint32_t seed_immediate = use_movn ? static_cast<uint32_t>(static_cast<uint16_t>(~halfwords[seed_halfword]))
		                                         : static_cast<uint32_t>(halfwords[seed_halfword]);
		if (!Emit((use_movn ? A64_MOVN_64 : A64_MOVZ_64) | (seed_halfword << 21u) | (seed_immediate << 5u) | destination))
		{
			return false;
		}
		for (uint32_t halfword = 0; halfword < halfwords.size(); halfword++)
		{
			if (halfword == seed_halfword || halfwords[halfword] == fill) continue;
			if (!Emit(A64_MOVK_64 | (halfword << 21u) | (static_cast<uint32_t>(halfwords[halfword]) << 5u) | destination))
			{
				return false;
			}
		}
		return true;
	}

	bool PatchCbnzW(size_t byte_offset, size_t target_byte_offset, uint8_t source)
	{
		if (source > 30 || m_storage == nullptr || byte_offset >= m_size ||
		    (byte_offset % A64_WORD_SIZE) != 0 ||
		    (target_byte_offset % A64_WORD_SIZE) != 0)
		{
			return false;
		}
		const int64_t delta = static_cast<int64_t>(target_byte_offset) - static_cast<int64_t>(byte_offset);
		if ((delta % static_cast<int64_t>(A64_WORD_SIZE)) != 0) return false;
		const int64_t words = delta / static_cast<int64_t>(A64_WORD_SIZE);
		if (words < -(int64_t {1} << 18) || words >= (int64_t {1} << 18)) return false;
		const uint32_t encoded = 0x35000000u | ((static_cast<uint32_t>(words) & 0x7ffffu) << 5u) | source;
		m_storage[byte_offset + 0] = static_cast<uint8_t>(encoded);
		m_storage[byte_offset + 1] = static_cast<uint8_t>(encoded >> 8u);
		m_storage[byte_offset + 2] = static_cast<uint8_t>(encoded >> 16u);
		m_storage[byte_offset + 3] = static_cast<uint8_t>(encoded >> 24u);
		return true;
	}

	bool PatchBranchCondition(size_t byte_offset, size_t target_byte_offset, uint8_t condition)
	{
		if (condition > 15 || !PatchBranchOffset(byte_offset, target_byte_offset, 19, 5,
		                                           0x54000000u | condition))
		{
			return false;
		}
		return true;
	}

	bool PatchBranch(size_t byte_offset, size_t target_byte_offset)
	{
		return PatchBranchOffset(byte_offset, target_byte_offset, 26, 0, 0x14000000u);
	}

	void SetBatchDirectRetirement(bool enabled) { m_batch_direct_retirement = enabled; }
	bool BatchDirectRetirement() const { return m_batch_direct_retirement; }
	void SetEntryMode(GuestEntryMode mode) { m_entry_mode = mode; }
	void SetBlockStateCache(bool rflags, uint8_t gpr_guest)
	{
		m_block_rflags_cache = rflags;
		m_block_gpr_cache_guest = gpr_guest < 16 ? gpr_guest : IR_NO_REGISTER;
	}
	bool BlockRflagsCache() const { return m_block_rflags_cache; }
	uint8_t BlockGprCacheGuest() const { return m_block_gpr_cache_guest; }
	bool DirectLeafAbi() const
	{
		return m_entry_mode != GuestEntryMode::FramedContext;
	}
	bool ContextlessDirect() const
	{
		return m_entry_mode == GuestEntryMode::DenseContextless;
	}
	uint8_t ContextRegister() const
	{
		// Every encoder validates base <= 30. Returning the IR sentinel makes an
		// accidental EntryContext access fail compilation in contextless mode.
		if (ContextlessDirect()) return IR_NO_REGISTER;
		return DirectLeafAbi() ? 1u : 19u;
	}
	uint8_t StateRegister() const
	{
		return ContextlessDirect() ? 0u : (DirectLeafAbi() ? 2u : 20u);
	}
	uint8_t ResumeRegister() const
	{
		return ContextlessDirect() ? 1u : (DirectLeafAbi() ? 3u : 21u);
	}
	uint8_t ExecutedRegister() const
	{
		return ContextlessDirect() ? IR_NO_REGISTER
		                           : (DirectLeafAbi() ? 4u : 22u);
	}
	size_t Size() const { return m_size; }

private:
	bool PatchBranchOffset(size_t byte_offset, size_t target_byte_offset, uint8_t bits,
	                       uint8_t shift, uint32_t opcode)
	{
		if (m_storage == nullptr || byte_offset >= m_size ||
		    (byte_offset % A64_WORD_SIZE) != 0 ||
		    (target_byte_offset % A64_WORD_SIZE) != 0 || bits == 0 || bits > 26)
		{
			return false;
		}
		const int64_t delta = static_cast<int64_t>(target_byte_offset) -
		                      static_cast<int64_t>(byte_offset);
		if ((delta % static_cast<int64_t>(A64_WORD_SIZE)) != 0) return false;
		const int64_t words = delta / static_cast<int64_t>(A64_WORD_SIZE);
		const int64_t limit = int64_t {1} << (bits - 1u);
		if (words < -limit || words >= limit) return false;
		const uint32_t mask = (uint32_t {1} << bits) - 1u;
		const uint32_t encoded = opcode | ((static_cast<uint32_t>(words) & mask) << shift);
		m_storage[byte_offset + 0] = static_cast<uint8_t>(encoded);
		m_storage[byte_offset + 1] = static_cast<uint8_t>(encoded >> 8u);
		m_storage[byte_offset + 2] = static_cast<uint8_t>(encoded >> 16u);
		m_storage[byte_offset + 3] = static_cast<uint8_t>(encoded >> 24u);
		return true;
	}

	uint8_t* m_storage = nullptr;
	size_t m_capacity = 0;
	size_t m_size = 0;
	bool m_batch_direct_retirement = false;
	GuestEntryMode m_entry_mode = GuestEntryMode::FramedContext;
	bool m_compact_immediates = false;
	bool m_block_rflags_cache = false;
	uint8_t m_block_gpr_cache_guest = IR_NO_REGISTER;
};

bool EmitEffectiveAddress(GuestCodeWriter* writer, const IrInstruction& instruction)
{
	if (writer == nullptr || instruction.opcode != IrOpcode::EffectiveAddress) return false;
	const auto& address = instruction.address;
	if (address.base == IR_NO_REGISTER)
	{
		if (!writer->EmitImmediate64(9, address.absolute)) return false;
	} else
	{
		const uint32_t base_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
		                             address.base * sizeof(uint64_t);
		if (!writer->EmitLoad64(9, writer->StateRegister(), base_offset)) return false;
		if (address.index != IR_NO_REGISTER)
		{
			const uint32_t index_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
			                              address.index * sizeof(uint64_t);
			if (!writer->EmitLoad64(10, writer->StateRegister(), index_offset) ||
			    !writer->EmitAddShifted64(9, 9, 10, address.scale))
			{
				return false;
			}
		}
		if (address.displacement != 0)
		{
			const uint64_t displacement =
			    static_cast<uint64_t>(static_cast<int64_t>(address.displacement));
			if (!writer->EmitImmediate64(10, displacement) ||
			    !writer->EmitAddShifted64(9, 9, 10, 0))
			{
				return false;
			}
		}
	}
	return instruction.width == 8 || writer->EmitMoveRegister32(9, 9);
}

bool IsScalarMemoryAluOpcode(IrOpcode opcode)
{
	return opcode == IrOpcode::AddRegisterMemory ||
	       opcode == IrOpcode::SubRegisterMemory ||
	       opcode == IrOpcode::OrRegisterMemory ||
	       opcode == IrOpcode::AndRegisterMemory ||
	       opcode == IrOpcode::XorRegisterMemory ||
	       opcode == IrOpcode::CompareRegisterMemory ||
	       opcode == IrOpcode::CompareMemoryRegister ||
	       opcode == IrOpcode::CompareMemoryImmediate ||
	       opcode == IrOpcode::TestMemoryImmediate ||
	       opcode == IrOpcode::SignedMultiplyRegisterMemory ||
	       opcode == IrOpcode::SignedMultiplyMemoryImmediate ||
	       IsScalarMemoryReadModifyWriteOpcode(opcode);
}

bool IsScalarMemoryReadModifyWriteOpcode(IrOpcode opcode)
{
	return opcode == IrOpcode::AddMemoryRegister ||
	       opcode == IrOpcode::SubtractMemoryRegister ||
	       opcode == IrOpcode::AndMemoryRegister ||
	       opcode == IrOpcode::AndMemoryImmediate ||
	       opcode == IrOpcode::IncrementMemory;
}

bool IsScalarMemoryOpcode(IrOpcode opcode)
{
	return opcode == IrOpcode::Load || opcode == IrOpcode::Store ||
	       opcode == IrOpcode::VectorLoadScalar ||
	       opcode == IrOpcode::VectorStoreScalar ||
	       opcode == IrOpcode::ZeroExtendMemory || opcode == IrOpcode::SignExtendMemory ||
	       IsScalarMemoryAluOpcode(opcode) ||
	       opcode == IrOpcode::SetConditionMemory ||
	       opcode == IrOpcode::PushRegister || opcode == IrOpcode::PopRegister ||
	       opcode == IrOpcode::CallImmediate || opcode == IrOpcode::Return;
}

bool IsStackMemoryOpcode(IrOpcode opcode)
{
	return opcode == IrOpcode::PushRegister || opcode == IrOpcode::PopRegister ||
	       opcode == IrOpcode::CallImmediate || opcode == IrOpcode::Return;
}

bool IsScalarMemoryWrite(const IrInstruction& instruction)
{
	return instruction.opcode == IrOpcode::Store ||
	       instruction.opcode == IrOpcode::VectorStoreScalar ||
	       IsScalarMemoryReadModifyWriteOpcode(instruction.opcode) ||
	       instruction.opcode == IrOpcode::SetConditionMemory ||
	       instruction.opcode == IrOpcode::PushRegister ||
	       instruction.opcode == IrOpcode::CallImmediate;
}

uint8_t ScalarMemoryAccessWidth(const IrInstruction& instruction)
{
	if (IsStackMemoryOpcode(instruction.opcode)) return 8;
	return instruction.opcode == IrOpcode::ZeroExtendMemory ||
	               instruction.opcode == IrOpcode::SignExtendMemory
	           ? instruction.source_width
	           : instruction.width;
}

bool EmitMemoryAddress(GuestCodeWriter* writer, const IrInstruction& instruction,
	                   uint8_t destination, uint8_t temporary)
{
	if (writer == nullptr || !IsScalarMemoryOpcode(instruction.opcode) ||
	    destination > 30 || temporary > 30 || destination == temporary)
	{
		return false;
	}
	if (IsStackMemoryOpcode(instruction.opcode))
	{
		constexpr uint32_t rsp_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
		                                4u * sizeof(uint64_t);
		if (!writer->EmitLoad64(destination, writer->StateRegister(), rsp_offset)) return false;
		if (instruction.opcode == IrOpcode::PushRegister ||
		    instruction.opcode == IrOpcode::CallImmediate)
		{
			return writer->EmitImmediate64(temporary, sizeof(uint64_t)) &&
			       writer->EmitSubtract64(destination, destination, temporary);
		}
		return true;
	}
	const auto& address = instruction.address;
	if (address.base == IR_NO_REGISTER)
	{
		return writer->EmitImmediate64(destination, address.absolute);
	}
	const uint32_t base_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
	                             address.base * sizeof(uint64_t);
	if (!writer->EmitLoad64(destination, writer->StateRegister(), base_offset)) return false;
	if (address.index != IR_NO_REGISTER)
	{
		const uint32_t index_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
		                              address.index * sizeof(uint64_t);
		if (!writer->EmitLoad64(temporary, writer->StateRegister(), index_offset) ||
		    !writer->EmitAddShifted64(destination, destination, temporary, address.scale))
		{
			return false;
		}
	}
	if (address.displacement != 0)
	{
		const uint64_t displacement =
		    static_cast<uint64_t>(static_cast<int64_t>(address.displacement));
		if (!writer->EmitImmediate64(temporary, displacement) ||
		    !writer->EmitAddShifted64(destination, destination, temporary, 0))
		{
			return false;
		}
	}
	return true;
}

bool EmitDirectRetirement(GuestCodeWriter* writer, const IrInstruction& instruction)
{
	constexpr uint32_t direct_offset = static_cast<uint32_t>(offsetof(EntryContext, direct_steps));
	if (writer == nullptr) return false;
	if (writer->BatchDirectRetirement()) return true;
	return
	       writer->EmitAddImmediate(writer->ResumeRegister(), writer->ResumeRegister(), instruction.length, true) &&
	       writer->EmitAddImmediate(writer->ExecutedRegister(), writer->ExecutedRegister(), instruction.step_cost, false) &&
	       writer->EmitLoad32(9, writer->ContextRegister(), direct_offset) &&
	       writer->EmitAddImmediate(9, 9, instruction.step_cost, false) &&
	       writer->EmitStore32(9, writer->ContextRegister(), direct_offset);
}

// Every scalar-memory node can publish direct, fault and generic exits. A
// terminating direct CALL has one additional transaction-conflict exit after a
// re-entrant resolver, so reserve that single fail-closed edge as well.
constexpr size_t MAX_GUEST_EXIT_PATCHES = MAX_IR_INSTRUCTIONS * 3u + 1u;

struct GuestExitPatches
{
	std::array<size_t, MAX_GUEST_EXIT_PATCHES> conditional {};
	std::array<size_t, MAX_GUEST_EXIT_PATCHES> unconditional {};
	size_t conditional_count = 0;
	size_t unconditional_count = 0;

	bool EmitConditional(GuestCodeWriter* writer)
	{
		if (writer == nullptr || conditional_count >= conditional.size()) return false;
		const size_t offset = writer->Size();
		if (!writer->Emit(0x35000000u)) return false;
		conditional[conditional_count++] = offset;
		return true;
	}

	bool EmitUnconditional(GuestCodeWriter* writer)
	{
		if (writer == nullptr || unconditional_count >= unconditional.size()) return false;
		const size_t offset = writer->Size();
		if (!writer->Emit(0x14000000u)) return false;
		unconditional[unconditional_count++] = offset;
		return true;
	}
};

bool EmitGenericIrDispatch(GuestCodeWriter* writer, uintptr_t instruction_address,
	                       GuestExitPatches* exits)
{
	constexpr uint32_t resume_offset = static_cast<uint32_t>(offsetof(EntryContext, resume_rip));
	constexpr uint32_t executed_offset = static_cast<uint32_t>(offsetof(EntryContext, executed_steps));
	return writer != nullptr && exits != nullptr &&
	       writer->EmitStore64(writer->ResumeRegister(), writer->ContextRegister(), resume_offset) &&
	       writer->EmitStore32(writer->ExecutedRegister(), writer->ContextRegister(), executed_offset) &&
	       writer->Emit(0xaa1303e0u) &&
	       writer->EmitImmediate64(1, static_cast<uint64_t>(instruction_address)) &&
	       writer->EmitImmediate64(16, FunctionAddress(&DispatchIrStep)) &&
	       writer->Emit(0xd63f0200u) &&
	       writer->EmitLoad64(writer->ResumeRegister(), writer->ContextRegister(), resume_offset) &&
	       writer->EmitLoad32(writer->ExecutedRegister(), writer->ContextRegister(), executed_offset) &&
	       exits->EmitConditional(writer);
}

bool EmitPrepareResultFlags(GuestCodeWriter* writer, uint8_t width,
	                        uint64_t clear_mask,
	                        uint8_t saved_flags = IR_NO_REGISTER);
bool EmitArithmeticFlags(GuestCodeWriter* writer, bool subtract,
	                     bool preserve_carry = false,
	                     uint8_t saved_flags = IR_NO_REGISTER);
bool EmitNarrowArithmeticFlags(GuestCodeWriter* writer, uint8_t width,
	                           bool subtract,
	                           uint8_t saved_flags = IR_NO_REGISTER);
bool EmitMultiplyFlags(GuestCodeWriter* writer,
	                   uint8_t saved_flags = IR_NO_REGISTER);
bool EmitBranchPredicate(GuestCodeWriter* writer, uint8_t condition);

Status EmitScalarMemoryInstruction(GuestCodeWriter* writer, const IrInstruction& instruction,
	                               uintptr_t instruction_address, GuestExitPatches* exits)
{
	if (writer == nullptr || exits == nullptr || !IsScalarMemoryOpcode(instruction.opcode))
	{
		return Status::InvalidArgument;
	}
	constexpr uint32_t regions_offset =
	    static_cast<uint32_t>(offsetof(EntryContext, scalar_memory_regions));
	constexpr uint32_t host_address_offset =
	    static_cast<uint32_t>(offsetof(EntryContext, scalar_memory_host_address));
	constexpr uint32_t read_host_address_offset =
	    static_cast<uint32_t>(offsetof(EntryContext, scalar_memory_read_host_address));
	constexpr uint32_t post_action_offset =
	    static_cast<uint32_t>(offsetof(EntryContext, scalar_memory_post_action));
	constexpr uint32_t enabled_offset =
	    static_cast<uint32_t>(offsetof(EntryContext, scalar_memory_enabled));
	constexpr uint32_t resume_offset = static_cast<uint32_t>(offsetof(EntryContext, resume_rip));
	constexpr uint32_t executed_offset = static_cast<uint32_t>(offsetof(EntryContext, executed_steps));

	const uint8_t access_width = ScalarMemoryAccessWidth(instruction);
	if (access_width != 1 && access_width != 2 && access_width != 4 && access_width != 8)
	{
		return Status::InvalidIr;
	}
	const bool stack_memory = IsStackMemoryOpcode(instruction.opcode);
	const bool write_access = IsScalarMemoryWrite(instruction);
	const bool direct_call = instruction.opcode == IrOpcode::CallImmediate;
	const bool memory_alu = IsScalarMemoryAluOpcode(instruction.opcode);
	const bool memory_rmw = IsScalarMemoryReadModifyWriteOpcode(instruction.opcode);

	// x23, x24 and x27 are callee-saved snapshots. Translation callbacks are
	// allowed to re-enter the runtime, so neither the effective address, a GPR
	// operand nor the input flags may be reconstructed from guest state after the
	// callback. x28 additionally preserves upper GPR bits for byte/word writes.
	if (!EmitMemoryAddress(writer, instruction, 23, 9)) return Status::CapacityExceeded;
	if (instruction.opcode == IrOpcode::VectorStoreScalar)
	{
		const uint32_t source_offset = static_cast<uint32_t>(offsetof(X64State, xmm)) +
		                               instruction.source * sizeof(uint64_t) * 4u;
		if (!(instruction.width == 4 ? writer->EmitLoad32(24, writer->StateRegister(), source_offset)
		                              : writer->EmitLoad64(24, writer->StateRegister(), source_offset)))
		{
			return Status::CapacityExceeded;
		}
	} else if (instruction.opcode == IrOpcode::Store)
	{
		if (instruction.source != IR_NO_REGISTER)
		{
			const uint32_t source_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
			                               instruction.source * sizeof(uint64_t);
			if (!(instruction.width == 4 ? writer->EmitLoad32(24, writer->StateRegister(), source_offset)
			                              : writer->EmitLoad64(24, writer->StateRegister(), source_offset)))
			{
				return Status::CapacityExceeded;
			}
			if ((instruction.width == 1 || instruction.width == 2) &&
			    !writer->EmitUbfx64(24, 24, 0, instruction.width * 8u))
			{
				return Status::CapacityExceeded;
			}
		} else if (!writer->EmitImmediate64(24, instruction.immediate))
		{
			return Status::CapacityExceeded;
		}
	} else if (instruction.opcode == IrOpcode::PushRegister)
	{
		const uint32_t source_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
		                               instruction.source * sizeof(uint64_t);
		if (!writer->EmitLoad64(24, writer->StateRegister(), source_offset)) return Status::CapacityExceeded;
	} else if (direct_call &&
	           !writer->EmitImmediate64(24, instruction.rip + instruction.length))
	{
		return Status::CapacityExceeded;
	} else if (instruction.opcode == IrOpcode::SetConditionMemory)
	{
		if (!EmitBranchPredicate(writer, instruction.condition) ||
		    !writer->EmitOrr64(24, 31, 10))
		{
			return Status::CapacityExceeded;
		}
	}
	if (memory_rmw)
	{
		constexpr uint32_t rflags_offset = static_cast<uint32_t>(offsetof(X64State, rflags));
		if (!writer->EmitLoad64(27, writer->StateRegister(), rflags_offset)) return Status::CapacityExceeded;
		if (instruction.opcode == IrOpcode::AddMemoryRegister ||
		    instruction.opcode == IrOpcode::SubtractMemoryRegister ||
		    instruction.opcode == IrOpcode::AndMemoryRegister)
		{
			const uint32_t operand_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
			                                instruction.source * sizeof(uint64_t);
			if (!writer->EmitLoad64(24, writer->StateRegister(), operand_offset)) return Status::CapacityExceeded;
		} else if (instruction.opcode == IrOpcode::AndMemoryImmediate &&
		           !writer->EmitImmediate64(24, instruction.immediate))
		{
			return Status::CapacityExceeded;
		}
	} else if (memory_alu)
	{
		constexpr uint32_t rflags_offset = static_cast<uint32_t>(offsetof(X64State, rflags));
		if (!writer->EmitLoad64(27, writer->StateRegister(), rflags_offset)) return Status::CapacityExceeded;
		uint8_t operand = IR_NO_REGISTER;
		if (instruction.opcode == IrOpcode::CompareMemoryRegister)
		{
			operand = instruction.source;
		} else if (instruction.opcode != IrOpcode::CompareMemoryImmediate &&
		           instruction.opcode != IrOpcode::TestMemoryImmediate &&
		           instruction.opcode != IrOpcode::SignedMultiplyMemoryImmediate)
		{
			operand = instruction.destination;
		}
		if (operand != IR_NO_REGISTER)
		{
			const uint32_t operand_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
			                                operand * sizeof(uint64_t);
			if (!writer->EmitLoad64(24, writer->StateRegister(), operand_offset)) return Status::CapacityExceeded;
			if ((instruction.width == 1 || instruction.width == 2) &&
			    !writer->EmitUbfx64(24, 24, 0, instruction.width * 8u))
			{
				return Status::CapacityExceeded;
			}
			if ((instruction.width == 1 || instruction.width == 2) &&
			    instruction.opcode != IrOpcode::CompareMemoryRegister &&
			    !writer->EmitLoad64(28, writer->StateRegister(), operand_offset))
			{
				return Status::CapacityExceeded;
			}
		}
	}

	struct GenericPatch
	{
		size_t offset = 0;
		uint8_t condition = 0xff;
	};
	std::array<GenericPatch, 10> to_generic {};
	size_t to_generic_count = 0;
	std::array<size_t, 3> to_transaction_conflict {};
	size_t to_transaction_conflict_count = 0;
	std::array<size_t, SCALAR_MEMORY_CACHE_ENTRIES> to_access {};
	size_t to_access_count = 0;
	auto emit_conditional_to_generic = [&](uint8_t condition) {
		if (condition > 15 || to_generic_count >= to_generic.size()) return false;
		const size_t offset = writer->Size();
		if (!writer->Emit(0x54000000u | condition)) return false;
		to_generic[to_generic_count++] = {offset, condition};
		return true;
	};
	auto emit_unconditional_to_generic = [&]() {
		if (to_generic_count >= to_generic.size()) return false;
		const size_t offset = writer->Size();
		if (!writer->Emit(0x14000000u)) return false;
		to_generic[to_generic_count++] = {offset, 0xff};
		return true;
	};
	auto emit_transaction_conflict = [&]() {
		if (to_transaction_conflict_count >=
		    to_transaction_conflict.size())
		{
			return false;
		}
		const size_t offset = writer->Size();
		if (!writer->Emit(0x54000001u)) return false;
		to_transaction_conflict[to_transaction_conflict_count++] = offset;
		return true;
	};

	if (direct_call)
	{
		constexpr uint32_t runtime_offset =
		    static_cast<uint32_t>(offsetof(EntryContext, runtime_context));
		constexpr uint32_t notify_offset =
		    static_cast<uint32_t>(offsetof(RuntimeServices, notify_scalar_store));
		constexpr uint32_t complete_offset =
		    static_cast<uint32_t>(offsetof(RuntimeServices, complete_direct_call));
		constexpr uint32_t preserves_offset = static_cast<uint32_t>(
		    offsetof(RuntimeServices, notify_scalar_store_preserves_translations));
		constexpr uint32_t transaction_active_offset =
		    static_cast<uint32_t>(offsetof(X64State, host_call_stack_transaction) +
		                          offsetof(X64CallStackTransaction, active));
		// A missing completion service cannot be discovered after the tentative
		// push. Likewise, an already-active transaction must keep the generic
		// PushCall refusal semantics without overwriting the earlier snapshot.
		if (!writer->EmitLoad64(9, writer->ContextRegister(), runtime_offset) || !writer->EmitCompareZero64(9) ||
		    !emit_conditional_to_generic(0) ||
		    !writer->EmitLoad64(10, 9, notify_offset) || !writer->EmitCompareZero64(10) ||
		    !emit_conditional_to_generic(0) ||
		    !writer->EmitLoad64(10, 9, complete_offset) || !writer->EmitCompareZero64(10) ||
		    !emit_conditional_to_generic(0) ||
		    !writer->EmitAddImmediate(10, 9, preserves_offset, true) ||
		    !writer->EmitLoadScalar(10, 10, 1) || !writer->EmitCompareImmediate(10, 0, false) ||
		    !emit_conditional_to_generic(0) ||
		    !writer->EmitImmediate64(9, transaction_active_offset) ||
		    !writer->EmitAddShifted64(9, writer->StateRegister(), 9, 0) ||
		    !writer->EmitLoadScalar(9, 9, 1) || !writer->EmitCompareImmediate(9, 0, false) ||
		    !emit_transaction_conflict())
		{
			return Status::CapacityExceeded;
		}
	}

	// Any wrap or 4 KiB guest-page crossing is deliberately delegated to the
	// atomic generic memory helper. Host page size is not guest semantics.
	if (!writer->EmitLoad32(9, writer->ContextRegister(), enabled_offset) ||
	    !writer->EmitCompareImmediate(9, 0, false) || !emit_conditional_to_generic(0) ||
	    !writer->EmitAddImmediateFlags64(26, 23, static_cast<uint16_t>(access_width - 1u)) ||
	    !emit_conditional_to_generic(2) ||
	    !writer->EmitLogicalShiftRight64(9, 23, 12) ||
	    !writer->EmitLogicalShiftRight64(10, 26, 12) ||
	    !writer->EmitCompare64(9, 10) || !emit_conditional_to_generic(1))
	{
		return Status::CapacityExceeded;
	}

	const uint32_t required_permissions = memory_rmw
	                                          ? ScalarMemoryPermissionMask(
	                                                ScalarMemoryPermission::Read) |
	                                                ScalarMemoryPermissionMask(
	                                                    ScalarMemoryPermission::Write)
	                                          : ScalarMemoryPermissionMask(
	                                                write_access
	                                                    ? ScalarMemoryPermission::Write
	                                                    : ScalarMemoryPermission::Read);
	for (size_t index = 0; index < SCALAR_MEMORY_CACHE_ENTRIES; index++)
	{
		const uint32_t region_offset = regions_offset +
		    static_cast<uint32_t>(index * sizeof(ScalarMemoryRegion));
		std::array<size_t, 3> next_region {};
		auto emit_next_region = [&](uint8_t condition, size_t patch_index) {
			next_region[patch_index] = writer->Size();
			return writer->Emit(0x54000000u | condition);
		};
		if (!writer->EmitLoad64(9, writer->ContextRegister(), region_offset + offsetof(ScalarMemoryRegion, guest_begin)) ||
		    !writer->EmitCompare64(23, 9) || !emit_next_region(3, 0) ||
		    !writer->EmitLoad64(10, writer->ContextRegister(), region_offset + offsetof(ScalarMemoryRegion, guest_end)) ||
		    !writer->EmitCompare64(26, 10) || !emit_next_region(2, 1) ||
		    !writer->EmitLoad32(11, writer->ContextRegister(), region_offset + offsetof(ScalarMemoryRegion, permissions)) ||
		    !writer->EmitImmediate64(12, required_permissions) ||
		    !writer->EmitAnd32(11, 11, 12) ||
		    !writer->EmitCompareImmediate(11, static_cast<uint16_t>(required_permissions), false) ||
		    !emit_next_region(1, 2) ||
		    !writer->EmitSubtract64(9, 23, 9) ||
		    !writer->EmitLoad64(25, writer->ContextRegister(), region_offset + offsetof(ScalarMemoryRegion, host_begin)) ||
		    !writer->EmitAddShifted64(25, 25, 9, 0) ||
		    (memory_rmw && !writer->EmitOrr64(26, 31, 25)))
		{
			return Status::CapacityExceeded;
		}
		to_access[to_access_count] = writer->Size();
		if (!writer->Emit(0x14000000u)) return Status::CapacityExceeded;
		to_access_count++;
		const size_t next = writer->Size();
		constexpr std::array<uint8_t, 3> conditions {3, 2, 1};
		for (size_t patch_index = 0; patch_index < next_region.size(); patch_index++)
		{
			if (!writer->PatchBranchCondition(next_region[patch_index], next,
			                                  conditions[patch_index]))
				return Status::PlatformFailure;
		}
	}

	// Cold miss: the callback performs the exact authoritative translation and
	// may refill the entry-local records. A successful translation still comes
	// back here for the native LDR/STR and retirement.
	if (!writer->EmitStore64(writer->ResumeRegister(), writer->ContextRegister(), resume_offset) ||
	    !writer->EmitStore32(writer->ExecutedRegister(), writer->ContextRegister(), executed_offset) ||
	    !writer->Emit(0xaa1303e0u) ||
	    !writer->EmitImmediate64(1, static_cast<uint64_t>(instruction_address)) ||
	    !writer->Emit(0xaa1703e2u) ||
	    !writer->EmitImmediate64(16, FunctionAddress(&DispatchScalarMemoryResolve)) ||
	    !writer->Emit(0xd63f0200u))
	{
		return Status::CapacityExceeded;
	}
	if (direct_call)
	{
		constexpr uint32_t transaction_active_offset =
		    static_cast<uint32_t>(offsetof(X64State, host_call_stack_transaction) +
		                          offsetof(X64CallStackTransaction, active));
		// A resolver can arm a newer CALL transaction and still report a memory
		// fault. Ownership conflict takes precedence over that resolver result,
		// matching PushCall before any RIP or retirement is committed.
		if (!writer->EmitImmediate64(9, transaction_active_offset) ||
		    !writer->EmitAddShifted64(9, writer->StateRegister(), 9, 0) ||
		    !writer->EmitLoadScalar(9, 9, 1) ||
		    !writer->EmitCompareImmediate(9, 0, false) ||
		    !emit_transaction_conflict())
		{
			return Status::CapacityExceeded;
		}
	}
	if (!writer->EmitCompareImmediate(
	        0, static_cast<uint16_t>(ScalarMemoryResolveResult::Resolved), false))
	{
		return Status::CapacityExceeded;
	}
	const size_t to_resolved = writer->Size();
	if (!writer->Emit(0x54000000u) ||
	    !writer->EmitCompareImmediate(0, static_cast<uint16_t>(ScalarMemoryResolveResult::MemoryFault), false))
	{
		return Status::CapacityExceeded;
	}
	const size_t to_fault = writer->Size();
	if (!writer->Emit(0x54000000u) || !emit_unconditional_to_generic())
	{
		return Status::CapacityExceeded;
	}

	const size_t resolved = writer->Size();
	if (!writer->PatchBranchCondition(to_resolved, resolved, 0)) return Status::PlatformFailure;
	if (!writer->EmitLoad64(25, writer->ContextRegister(), host_address_offset) ||
	    (memory_rmw && !writer->EmitLoad64(26, writer->ContextRegister(), read_host_address_offset)))
	{
		return Status::CapacityExceeded;
	}
	const size_t access = writer->Size();
	for (size_t index = 0; index < to_access_count; index++)
	{
		if (!writer->PatchBranch(to_access[index], access)) return Status::PlatformFailure;
	}
	if (direct_call)
	{
		constexpr uint32_t transaction_active_offset =
		    static_cast<uint32_t>(offsetof(X64State, host_call_stack_transaction) +
		                          offsetof(X64CallStackTransaction, active));
		// A cold resolver may re-enter the runtime after the first guard and arm a
		// CALL transaction on the same state. Recheck at the commit boundary so
		// this entry can never overwrite that newer rollback snapshot.
		if (!writer->EmitImmediate64(9, transaction_active_offset) ||
		    !writer->EmitAddShifted64(9, writer->StateRegister(), 9, 0) ||
		    !writer->EmitLoadScalar(9, 9, 1) ||
		    !writer->EmitCompareImmediate(9, 0, false) ||
		    !emit_transaction_conflict())
		{
			return Status::CapacityExceeded;
		}
	}

	auto emit_store_notification = [&]() {
		return writer->EmitStore64(writer->ResumeRegister(), writer->ContextRegister(), resume_offset) &&
		       writer->EmitStore32(writer->ExecutedRegister(), writer->ContextRegister(), executed_offset) &&
		       writer->Emit(0xaa1303e0u) &&
		       writer->EmitImmediate64(1, static_cast<uint64_t>(instruction_address)) &&
		       writer->Emit(0xaa1703e2u) && writer->Emit(0xaa1903e3u) &&
		       writer->Emit(0xaa1803e4u) &&
		       writer->EmitImmediate64(16, FunctionAddress(&DispatchScalarStoreNotify)) &&
		       writer->Emit(0xd63f0200u);
	};

	if (instruction.opcode == IrOpcode::VectorStoreScalar)
	{
		if (!writer->EmitStoreScalar(24, 25, instruction.width) ||
		    !emit_store_notification() || !EmitDirectRetirement(writer, instruction) ||
		    !exits->EmitConditional(writer))
		{
			return Status::CapacityExceeded;
		}
	} else if (instruction.opcode == IrOpcode::VectorLoadScalar)
	{
		const uint32_t destination_offset =
		    static_cast<uint32_t>(offsetof(X64State, xmm)) +
		    instruction.destination * sizeof(uint64_t) * 4u;
		if (!writer->EmitLoadScalar(9, 25, instruction.width) ||
		    !writer->EmitStore64(9, writer->StateRegister(), destination_offset) ||
		    !writer->EmitStore64(31, writer->StateRegister(), destination_offset + sizeof(uint64_t)) ||
		    !writer->EmitStore64(31, writer->StateRegister(), destination_offset + 2u * sizeof(uint64_t)) ||
		    !writer->EmitStore64(31, writer->StateRegister(), destination_offset + 3u * sizeof(uint64_t)) ||
		    !EmitDirectRetirement(writer, instruction) ||
		    !writer->EmitLoad32(0, writer->ContextRegister(), post_action_offset) ||
		    !exits->EmitConditional(writer))
		{
			return Status::CapacityExceeded;
		}
	} else if (instruction.opcode == IrOpcode::Store)
	{
		if (!writer->EmitStoreScalar(24, 25, instruction.width) ||
		    !emit_store_notification() || !EmitDirectRetirement(writer, instruction) ||
		    !exits->EmitConditional(writer))
		{
			return Status::CapacityExceeded;
		}
	} else if (instruction.opcode == IrOpcode::PushRegister)
	{
		constexpr uint32_t rsp_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
		                                4u * sizeof(uint64_t);
		if (!writer->EmitStoreScalar(24, 25, 8) ||
		    !emit_store_notification() ||
		    // Push updates RSP after Store/RecordMemoryWrite returns. Preserve that
		    // ordering for a notifier that performs re-entrant work.
		    !writer->EmitStore64(23, writer->StateRegister(), rsp_offset) ||
		    !EmitDirectRetirement(writer, instruction) ||
		    !exits->EmitConditional(writer))
		{
			return Status::CapacityExceeded;
		}
	} else if (direct_call)
	{
		constexpr uint32_t rsp_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
		                                4u * sizeof(uint64_t);
		constexpr uint32_t rip_offset = static_cast<uint32_t>(offsetof(X64State, rip));
		constexpr uint32_t transaction_offset =
		    static_cast<uint32_t>(offsetof(X64State, host_call_stack_transaction));
		constexpr uint32_t original_rsp_offset = transaction_offset +
		    static_cast<uint32_t>(offsetof(X64CallStackTransaction, original_rsp));
		constexpr uint32_t slot_address_offset = transaction_offset +
		    static_cast<uint32_t>(offsetof(X64CallStackTransaction, slot_address));
		constexpr uint32_t slot_host_address_offset = transaction_offset +
		    static_cast<uint32_t>(offsetof(X64CallStackTransaction, slot_host_address));
		constexpr uint32_t original_slot_value_offset = transaction_offset +
		    static_cast<uint32_t>(offsetof(X64CallStackTransaction, original_slot_value));
		constexpr uint32_t active_offset = transaction_offset +
		    static_cast<uint32_t>(offsetof(X64CallStackTransaction, active));

		// Snapshot through the already-authorized write mapping. This deliberately
		// does not introduce guest-read permission, and it makes the tentative CALL
		// push recoverable if HLE preflight faults.
		if (!writer->EmitLoadScalar(26, 25, 8) ||
		    !writer->EmitStoreScalar(24, 25, 8) ||
		    !writer->EmitStore64(23, writer->StateRegister(), rsp_offset) ||
		    !writer->EmitAddImmediate(9, 23, sizeof(uint64_t), true) ||
		    !writer->EmitStore64(9, writer->StateRegister(), original_rsp_offset) ||
		    !writer->EmitStore64(23, writer->StateRegister(), slot_address_offset) ||
		    !writer->EmitStore64(25, writer->StateRegister(), slot_host_address_offset) ||
		    !writer->EmitStore64(26, writer->StateRegister(), original_slot_value_offset) ||
		    !writer->EmitImmediate64(10, active_offset) ||
		    !writer->EmitAddShifted64(10, writer->StateRegister(), 10, 0) ||
		    !writer->EmitImmediate64(9, 1) || !writer->EmitStoreScalar(9, 10, 1) ||
		    !emit_store_notification() ||
		    // Host callbacks observe the architectural post-decode CALL boundary.
		    !writer->EmitStore64(24, writer->StateRegister(), rip_offset) ||
		    !EmitDirectRetirement(writer, instruction) ||
		    !writer->EmitStore64(writer->ResumeRegister(), writer->ContextRegister(), resume_offset) ||
		    !writer->EmitStore32(writer->ExecutedRegister(), writer->ContextRegister(), executed_offset) ||
		    !writer->Emit(0xaa1303e0u) ||
		    !writer->EmitImmediate64(1, static_cast<uint64_t>(instruction_address)) ||
		    !writer->EmitImmediate64(16, FunctionAddress(&DispatchCompleteDirectCall)) ||
		    !writer->Emit(0xd63f0200u) || !exits->EmitUnconditional(writer))
		{
			return Status::CapacityExceeded;
		}
	} else if (memory_rmw)
	{
		constexpr uint32_t rflags_offset =
		    static_cast<uint32_t>(offsetof(X64State, rflags));
		const bool width64 = instruction.width == 8;
		if (!writer->EmitLoadScalar(9, 26, instruction.width))
			return Status::CapacityExceeded;
		if (instruction.opcode == IrOpcode::AddMemoryRegister ||
		    instruction.opcode == IrOpcode::SubtractMemoryRegister)
		{
			const bool subtract = instruction.opcode == IrOpcode::SubtractMemoryRegister;
			if (!writer->EmitOrr64(10, 31, 24) ||
			    !writer->EmitAddSubFlags(11, 9, 10, subtract, width64) ||
			    !EmitArithmeticFlags(writer, subtract, false, 27))
			{
				return Status::CapacityExceeded;
			}
		} else if (instruction.opcode == IrOpcode::IncrementMemory)
		{
			if (!writer->EmitImmediate64(10, 1) ||
			    !writer->EmitAddSubFlags(11, 9, 10, false, width64) ||
			    !EmitArithmeticFlags(writer, false, true, 27))
			{
				return Status::CapacityExceeded;
			}
		} else
		{
			const bool logic = width64 ? writer->EmitAnd64(11, 9, 24)
			                           : writer->EmitAnd32(11, 9, 24);
			constexpr uint64_t clear_mask = X64_FLAG_CF | X64_FLAG_PF |
			                                X64_FLAG_ZF | X64_FLAG_SF |
			                                X64_FLAG_OF;
			if (!logic || !EmitPrepareResultFlags(
			                  writer, instruction.width, clear_mask, 27) ||
			    !writer->EmitStore64(12, writer->StateRegister(), rflags_offset))
			{
				return Status::CapacityExceeded;
			}
		}
		if (!writer->EmitOrr64(24, 31, 11) ||
		    !writer->EmitStoreScalar(24, 25, instruction.width) ||
		    !emit_store_notification() ||
		    !EmitDirectRetirement(writer, instruction) ||
		    !exits->EmitConditional(writer))
		{
			return Status::CapacityExceeded;
		}
	} else if (instruction.opcode == IrOpcode::SetConditionMemory)
	{
		if (!writer->EmitStoreScalar(24, 25, 1) ||
		    !emit_store_notification() ||
		    !EmitDirectRetirement(writer, instruction) ||
		    !exits->EmitConditional(writer))
		{
			return Status::CapacityExceeded;
		}
	} else if (instruction.opcode == IrOpcode::PopRegister ||
	           instruction.opcode == IrOpcode::Return)
	{
		constexpr uint32_t rsp_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
		                                4u * sizeof(uint64_t);
		if (!writer->EmitLoadScalar(24, 25, 8)) return Status::CapacityExceeded;
		const uint64_t stack_adjust = sizeof(uint64_t) +
		    (instruction.opcode == IrOpcode::Return ? instruction.immediate : 0);
		if (!writer->EmitImmediate64(9, stack_adjust) ||
		    !writer->EmitAddShifted64(26, 23, 9, 0) ||
		    !writer->EmitStore64(26, writer->StateRegister(), rsp_offset))
		{
			return Status::CapacityExceeded;
		}
		if (instruction.opcode == IrOpcode::PopRegister)
		{
			const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
			                                    instruction.destination * sizeof(uint64_t);
			// POP RSP first performs the increment, then the destination write; the
			// loaded qword therefore wins when destination is RSP.
			if (!writer->EmitStore64(24, writer->StateRegister(), destination_offset) ||
			    !EmitDirectRetirement(writer, instruction) ||
			    !writer->EmitLoad32(0, writer->ContextRegister(), post_action_offset) ||
			    !exits->EmitConditional(writer))
			{
				return Status::CapacityExceeded;
			}
		} else
		{
			constexpr uint32_t rip_offset = static_cast<uint32_t>(offsetof(X64State, rip));
			constexpr uint32_t reason_offset =
			    static_cast<uint32_t>(offsetof(EntryContext, exit_reason));
			if (!writer->EmitStore64(24, writer->StateRegister(), rip_offset) ||
			    !EmitDirectRetirement(writer, instruction) ||
			    !writer->EmitLoad32(9, writer->ContextRegister(), post_action_offset) ||
			    !writer->EmitCompareImmediate(9, 0, false))
			{
				return Status::CapacityExceeded;
			}
			const size_t preserve_reason = writer->Size();
			if (!writer->Emit(0x54000001u) ||
			    !writer->EmitStore32(31, writer->ContextRegister(), reason_offset))
			{
				return Status::CapacityExceeded;
			}
			if (!writer->PatchBranchCondition(preserve_reason, writer->Size(), 1))
			{
				return Status::PlatformFailure;
			}
			if (!exits->EmitUnconditional(writer)) return Status::CapacityExceeded;
		}
	} else if (!stack_memory)
	{
		if (!writer->EmitLoadScalar(9, 25, access_width)) return Status::CapacityExceeded;
		if (memory_alu)
		{
			constexpr uint32_t rflags_offset =
			    static_cast<uint32_t>(offsetof(X64State, rflags));
			const bool add = instruction.opcode == IrOpcode::AddRegisterMemory;
			const bool sub = instruction.opcode == IrOpcode::SubRegisterMemory;
			const bool compare_register =
			    instruction.opcode == IrOpcode::CompareRegisterMemory;
			const bool compare_memory =
			    instruction.opcode == IrOpcode::CompareMemoryRegister ||
			    instruction.opcode == IrOpcode::CompareMemoryImmediate;
			const bool test = instruction.opcode == IrOpcode::TestMemoryImmediate;
			const bool multiply =
			    instruction.opcode == IrOpcode::SignedMultiplyRegisterMemory ||
			    instruction.opcode == IrOpcode::SignedMultiplyMemoryImmediate;
			const bool width64 = instruction.width == 8;
			const bool write_result = !compare_register && !compare_memory && !test;
			const uint32_t destination_offset = write_result
			                                        ? static_cast<uint32_t>(offsetof(X64State, gpr)) +
			                                              instruction.destination * sizeof(uint64_t)
			                                        : 0;
			if (multiply)
			{
				if (instruction.opcode == IrOpcode::SignedMultiplyRegisterMemory)
				{
					if (!writer->EmitOrr64(10, 31, 9) || !writer->EmitOrr64(9, 31, 24))
						return Status::CapacityExceeded;
				} else
				{
					if (!writer->EmitOrr64(10, 31, 9) ||
					    !writer->EmitImmediate64(9, instruction.immediate))
						return Status::CapacityExceeded;
				}
				if (width64)
				{
					if (!writer->EmitMultiply64(11, 9, 10) ||
					    !writer->EmitSignedMultiplyHigh64(13, 9, 10) ||
					    !writer->EmitArithmeticShiftRight64(15, 11, 63) ||
					    !writer->EmitCompare64(13, 15))
						return Status::CapacityExceeded;
				} else if (!writer->EmitSignedMultiplyLong32(11, 9, 10) ||
				           !writer->EmitExtendRegister(15, 11, 4, 8, true) ||
				           !writer->EmitCompare64(11, 15))
				{
					return Status::CapacityExceeded;
				}
				if (!writer->EmitImmediate64(14, 1) ||
				    !writer->EmitConditionalSelect64(14, 14, 31, 1) ||
				    (!width64 && !writer->EmitMoveRegister32(11, 11)) ||
				    !EmitMultiplyFlags(writer, 27))
				{
					return Status::CapacityExceeded;
				}
			} else if (add || sub || compare_register || compare_memory)
			{
				if (instruction.opcode == IrOpcode::CompareMemoryImmediate)
				{
					if (!writer->EmitImmediate64(10, instruction.immediate))
						return Status::CapacityExceeded;
				} else if (compare_memory)
				{
					if (!writer->EmitOrr64(10, 31, 24)) return Status::CapacityExceeded;
				} else
				{
					if (!writer->EmitOrr64(10, 31, 9) || !writer->EmitOrr64(9, 31, 24))
						return Status::CapacityExceeded;
				}
				if (!writer->EmitAddSubFlags(11, 9, 10, !add, width64))
				{
					return Status::CapacityExceeded;
				}
				if (instruction.width == 1 || instruction.width == 2)
				{
					if (!writer->EmitUbfx64(11, 11, 0, instruction.width * 8u) ||
					    !EmitNarrowArithmeticFlags(writer, instruction.width, true, 27))
					{
						return Status::CapacityExceeded;
					}
				} else if (!EmitArithmeticFlags(writer, !add, false, 27))
				{
					return Status::CapacityExceeded;
				}
			} else
			{
				if (test)
				{
					if (!writer->EmitImmediate64(10, instruction.immediate))
						return Status::CapacityExceeded;
				} else
				{
					if (!writer->EmitOrr64(10, 31, 24)) return Status::CapacityExceeded;
				}
				const bool logic =
				    instruction.opcode == IrOpcode::OrRegisterMemory
				        ? (width64 ? writer->EmitOrr64(11, 10, 9)
				                   : writer->EmitOrr32(11, 10, 9))
				    : instruction.opcode == IrOpcode::XorRegisterMemory
				        ? (width64 ? writer->EmitEor64(11, 10, 9)
				                   : writer->EmitEor32(11, 10, 9))
				        : (width64 ? writer->EmitAnd64(11, 10, 9)
				                   : writer->EmitAnd32(11, 10, 9));
				constexpr uint64_t clear_mask = X64_FLAG_CF | X64_FLAG_PF |
				                                X64_FLAG_ZF | X64_FLAG_SF |
				                                X64_FLAG_OF;
				if (!logic || !EmitPrepareResultFlags(
				                  writer, instruction.width, clear_mask, 27) ||
				    !writer->EmitStore64(12, writer->StateRegister(), rflags_offset))
				{
					return Status::CapacityExceeded;
				}
			}
			if (write_result)
			{
				if (instruction.width == 1 || instruction.width == 2)
				{
					if (!writer->EmitInsertLow64(28, 11, instruction.width) ||
					    !writer->EmitStore64(28, writer->StateRegister(), destination_offset))
					{
						return Status::CapacityExceeded;
					}
				} else if (!writer->EmitStore64(11, writer->StateRegister(), destination_offset))
				{
					return Status::CapacityExceeded;
				}
			}
		} else
		{
			const uint32_t destination_offset =
			    static_cast<uint32_t>(offsetof(X64State, gpr)) +
			    instruction.destination * sizeof(uint64_t);
			if (instruction.opcode == IrOpcode::Load)
			{
				if (instruction.width == 1 || instruction.width == 2)
				{
					if (!writer->EmitLoad64(10, writer->StateRegister(), destination_offset) ||
					    !writer->EmitInsertLow64(10, 9, instruction.width) ||
					    !writer->EmitStore64(10, writer->StateRegister(), destination_offset))
					{
						return Status::CapacityExceeded;
					}
				} else if (!writer->EmitStore64(9, writer->StateRegister(), destination_offset))
				{
					return Status::CapacityExceeded;
				}
			} else if (!writer->EmitExtendRegister(
			               10, 9, instruction.source_width, instruction.width,
			               instruction.opcode == IrOpcode::SignExtendMemory) ||
			           !writer->EmitStore64(10, writer->StateRegister(), destination_offset))
			{
				return Status::CapacityExceeded;
			}
		}
		if (!EmitDirectRetirement(writer, instruction) ||
		    !writer->EmitLoad32(0, writer->ContextRegister(), post_action_offset) ||
		    !exits->EmitConditional(writer))
		{
			return Status::CapacityExceeded;
		}
	} else
	{
		return Status::InvalidIr;
	}
	const size_t to_done = writer->Size();
	if (!writer->Emit(0x14000000u)) return Status::CapacityExceeded;

	const size_t fault = writer->Size();
	if (!writer->PatchBranchCondition(to_fault, fault, 0)) return Status::PlatformFailure;
	if (memory_rmw &&
	    !writer->EmitStore64(
	        27, writer->StateRegister(), static_cast<uint32_t>(offsetof(X64State, rflags))))
	{
		return Status::CapacityExceeded;
	}
	if (!writer->EmitAddImmediate(writer->ResumeRegister(), writer->ResumeRegister(), instruction.length, true) ||
	    !writer->EmitAddImmediate(writer->ExecutedRegister(), writer->ExecutedRegister(), instruction.step_cost, false) ||
	    !exits->EmitUnconditional(writer)) return Status::CapacityExceeded;

	if (direct_call)
	{
		const size_t transaction_conflict = writer->Size();
		for (size_t index = 0; index < to_transaction_conflict_count; index++)
		{
			if (!writer->PatchBranchCondition(
			        to_transaction_conflict[index], transaction_conflict, 1))
			{
				return Status::PlatformFailure;
			}
		}
		constexpr uint32_t rip_offset = static_cast<uint32_t>(offsetof(X64State, rip));
		constexpr uint32_t reason_offset =
		    static_cast<uint32_t>(offsetof(EntryContext, exit_reason));
		constexpr uint32_t transaction_conflict_offset = static_cast<uint32_t>(
		    offsetof(EntryContext, call_stack_transaction_conflict));
		// An active transaction is an ownership conflict, not an executable outer
		// CALL. Publish an explicit non-poisoning exit and leave x22/direct_steps at
		// the accounting accumulated before this instruction, while preserving the
		// newer transaction byte-for-byte for its owner.
		if (!writer->EmitImmediate64(9, instruction.rip) ||
		    !writer->EmitStore64(9, writer->StateRegister(), rip_offset) ||
		    !writer->EmitImmediate64(writer->ResumeRegister(), instruction.rip) ||
		    !writer->EmitImmediate64(
		        9, static_cast<uint32_t>(ExitReason::Bailout)) ||
		    !writer->EmitStore32(9, writer->ContextRegister(), reason_offset) ||
		    !writer->EmitImmediate64(9, 1) ||
		    !writer->EmitStore32(9, writer->ContextRegister(), transaction_conflict_offset) ||
		    !exits->EmitUnconditional(writer))
		{
			return Status::CapacityExceeded;
		}
	}

	const size_t generic = writer->Size();
	for (size_t index = 0; index < to_generic_count; index++)
	{
		const GenericPatch patch = to_generic[index];
		if (patch.condition == 0xff)
		{
			if (!writer->PatchBranch(patch.offset, generic)) return Status::PlatformFailure;
		} else
		{
			if (!writer->PatchBranchCondition(patch.offset, generic, patch.condition))
			{
				return Status::PlatformFailure;
			}
		}
	}
	if (!EmitGenericIrDispatch(writer, instruction_address, exits))
	{
		return Status::CapacityExceeded;
	}
	const size_t done = writer->Size();
	if (!writer->PatchBranch(to_done, done)) return Status::PlatformFailure;
	return Status::Ok;
}

bool EmitVectorUpperPolicy(GuestCodeWriter* writer, const IrInstruction& instruction, uint32_t destination_offset)
{
	if (writer == nullptr) return false;
	if (!HasVectorFlag(instruction, IrVectorFlag::ZeroUpper)) return true;
	return writer->EmitStore64(31, writer->StateRegister(), destination_offset + 16u) &&
	       writer->EmitStore64(31, writer->StateRegister(), destination_offset + 24u);
}

bool EmitPrepareResultFlags(GuestCodeWriter* writer, uint8_t width, uint64_t clear_mask,
	                        uint8_t saved_flags)
{
	constexpr uint32_t rflags_offset = static_cast<uint32_t>(offsetof(X64State, rflags));
	if (writer == nullptr || (width != 1 && width != 2 && width != 4 && width != 8) ||
	    (saved_flags != IR_NO_REGISTER && saved_flags > 30))
	{
		return false;
	}
	if (!(saved_flags == IR_NO_REGISTER
	          ? writer->EmitLoad64(12, writer->StateRegister(), rflags_offset)
	          : writer->EmitOrr64(12, 31, saved_flags)) ||
	    !writer->EmitImmediate64(15, clear_mask) || !writer->EmitBic64(12, 12, 15) ||
	    !writer->EmitCompareZero64(11) || !writer->EmitMrsNzcv(13))
	{
		return false;
	}
	// ZF comes from the compare; SF uses the guest operand width rather than
	// AArch64's 64-bit sign bit.
	if (!writer->EmitUbfx64(14, 13, 30, 1) || !writer->EmitOrr64(12, 12, 14, 0, 6) ||
	    !writer->EmitUbfx64(14, 11, static_cast<uint8_t>(width * 8u - 1u), 1) ||
	    !writer->EmitOrr64(12, 12, 14, 0, 7))
	{
		return false;
	}
	// PF is even parity of the low result byte.
	return writer->EmitEor64(14, 11, 11, 1, 4) &&
	       writer->EmitEor64(14, 14, 14, 1, 2) &&
	       writer->EmitEor64(14, 14, 14, 1, 1) && writer->EmitMvn64(14, 14) &&
	       writer->EmitUbfx64(14, 14, 0, 1) && writer->EmitOrr64(12, 12, 14, 0, 2);
}

bool EmitArithmeticFlags(GuestCodeWriter* writer, bool subtract, bool preserve_carry,
	                     uint8_t saved_flags)
{
	constexpr uint32_t rflags_offset = static_cast<uint32_t>(offsetof(X64State, rflags));
	if (writer == nullptr || (saved_flags != IR_NO_REGISTER && saved_flags > 30) ||
	    !writer->EmitMrsNzcv(13) ||
	    !(saved_flags == IR_NO_REGISTER
	          ? writer->EmitLoad64(12, writer->StateRegister(), rflags_offset)
	          : writer->EmitOrr64(12, 31, saved_flags)) ||
	    !writer->EmitImmediate64(15, preserve_carry ? 0x8d4u : 0x8d5u) ||
	    !writer->EmitBic64(12, 12, 15))
	{
		return false;
	}

	// CF: AArch64 C is carry for ADD and "no borrow" for SUB/CMP.
	if (!preserve_carry)
	{
		if (!writer->EmitUbfx64(14, 13, 29, 1)) return false;
		if (subtract && (!writer->EmitMvn64(14, 14) || !writer->EmitUbfx64(14, 14, 0, 1))) return false;
		if (!writer->EmitOrr64(12, 12, 14)) return false;
	}
	// ZF, SF and OF come directly from NZCV.
	if (!writer->EmitUbfx64(14, 13, 30, 1) || !writer->EmitOrr64(12, 12, 14, 0, 6) ||
	    !writer->EmitUbfx64(14, 13, 31, 1) || !writer->EmitOrr64(12, 12, 14, 0, 7) ||
	    !writer->EmitUbfx64(14, 13, 28, 1) || !writer->EmitOrr64(12, 12, 14, 0, 11))
	{
		return false;
	}
	// AF = bit 4 of lhs ^ rhs ^ result.
	if (!writer->EmitEor64(14, 9, 10) || !writer->EmitEor64(14, 14, 11) ||
	    !writer->EmitUbfx64(14, 14, 4, 1) || !writer->EmitOrr64(12, 12, 14, 0, 4)) return false;
	// PF is even parity of the low result byte.
	if (!writer->EmitEor64(14, 11, 11, 1, 4) || !writer->EmitEor64(14, 14, 14, 1, 2) ||
	    !writer->EmitEor64(14, 14, 14, 1, 1) || !writer->EmitMvn64(14, 14) ||
	    !writer->EmitUbfx64(14, 14, 0, 1) || !writer->EmitOrr64(12, 12, 14, 0, 2) ||
	    !writer->EmitStore64(12, writer->StateRegister(), rflags_offset)) return false;
	return true;
}

bool EmitNarrowArithmeticFlags(GuestCodeWriter* writer, uint8_t width, bool subtract,
	                           uint8_t saved_flags)
{
	constexpr uint32_t rflags_offset = static_cast<uint32_t>(offsetof(X64State, rflags));
	if (writer == nullptr || (width != 1 && width != 2) ||
	    (saved_flags != IR_NO_REGISTER && saved_flags > 30) ||
	    !writer->EmitMrsNzcv(13) ||
	    !(saved_flags == IR_NO_REGISTER
	          ? writer->EmitLoad64(12, writer->StateRegister(), rflags_offset)
	          : writer->EmitOrr64(12, 31, saved_flags)) ||
	    !writer->EmitImmediate64(15, X64_ARITHMETIC_FLAGS) ||
	    !writer->EmitBic64(12, 12, 15) || !writer->EmitUbfx64(14, 13, 29, 1))
	{
		return false;
	}
	// C is carry for ADD and no-borrow for SUB/SBB.
	if (subtract &&
	    (!writer->EmitMvn64(14, 14) || !writer->EmitUbfx64(14, 14, 0, 1)))
	{
		return false;
	}
	if (!writer->EmitOrr64(12, 12, 14) || !writer->EmitCompareZero64(11) ||
	    !writer->EmitMrsNzcv(13) || !writer->EmitUbfx64(14, 13, 30, 1) ||
	    !writer->EmitOrr64(12, 12, 14, 0, 6) ||
	    !writer->EmitUbfx64(14, 11, static_cast<uint8_t>(width * 8u - 1u), 1) ||
	    !writer->EmitOrr64(12, 12, 14, 0, 7))
	{
		return false;
	}
	// OF = sign bit of ((lhs^rhs)&(lhs^result)) for subtraction and
	// (~(lhs^rhs)&(lhs^result)) for addition.
	if (!writer->EmitEor64(14, 9, 10) ||
	    (!subtract && !writer->EmitMvn64(14, 14)) ||
	    !writer->EmitEor64(15, 9, 11) || !writer->EmitAnd64(14, 14, 15) ||
	    !writer->EmitUbfx64(14, 14, static_cast<uint8_t>(width * 8u - 1u), 1) ||
	    !writer->EmitOrr64(12, 12, 14, 0, 11) ||
	    !writer->EmitEor64(14, 9, 10) || !writer->EmitEor64(14, 14, 11) ||
	    !writer->EmitUbfx64(14, 14, 4, 1) || !writer->EmitOrr64(12, 12, 14, 0, 4))
	{
		return false;
	}
	return writer->EmitEor64(14, 11, 11, 1, 4) &&
	       writer->EmitEor64(14, 14, 14, 1, 2) &&
	       writer->EmitEor64(14, 14, 14, 1, 1) && writer->EmitMvn64(14, 14) &&
	       writer->EmitUbfx64(14, 14, 0, 1) &&
	       writer->EmitOrr64(12, 12, 14, 0, 2) &&
	       writer->EmitStore64(12, writer->StateRegister(), rflags_offset);
}

bool EmitZeroSignFlagsPreservingParityAux(
	GuestCodeWriter* writer, uint8_t width, bool include_sign = true)
{
	constexpr uint32_t rflags_offset = static_cast<uint32_t>(offsetof(X64State, rflags));
	constexpr uint64_t changed = X64_FLAG_CF | X64_FLAG_ZF | X64_FLAG_SF | X64_FLAG_OF;
	if (writer == nullptr || (width != 4 && width != 8) ||
	    !writer->EmitLoad64(12, writer->StateRegister(), rflags_offset) ||
	    !writer->EmitImmediate64(15, changed) || !writer->EmitBic64(12, 12, 15) ||
	    !writer->EmitCompareZero64(11) || !writer->EmitMrsNzcv(13) ||
	    !writer->EmitUbfx64(14, 13, 30, 1) || !writer->EmitOrr64(12, 12, 14, 0, 6))
	{
		return false;
	}
	if (include_sign &&
	    (!writer->EmitUbfx64(14, 11, static_cast<uint8_t>(width * 8u - 1u), 1) ||
	     !writer->EmitOrr64(12, 12, 14, 0, 7)))
	{
		return false;
	}
	return writer->EmitStore64(12, writer->StateRegister(), rflags_offset);
}

bool EmitMultiplyFlags(GuestCodeWriter* writer, uint8_t saved_flags)
{
	constexpr uint32_t rflags_offset = static_cast<uint32_t>(offsetof(X64State, rflags));
	constexpr uint64_t changed = X64_FLAG_CF | X64_FLAG_OF;
	return writer != nullptr && (saved_flags == IR_NO_REGISTER || saved_flags <= 30) &&
	       (saved_flags == IR_NO_REGISTER
	            ? writer->EmitLoad64(12, writer->StateRegister(), rflags_offset)
	            : writer->EmitOrr64(12, 31, saved_flags)) &&
	       writer->EmitImmediate64(15, changed) && writer->EmitBic64(12, 12, 15) &&
	       writer->EmitOrr64(12, 12, 14) && writer->EmitOrr64(12, 12, 14, 0, 11) &&
	       writer->EmitStore64(12, writer->StateRegister(), rflags_offset);
}

bool EmitBranchPredicate(GuestCodeWriter* writer, uint8_t condition)
{
	constexpr uint32_t rflags_offset = static_cast<uint32_t>(offsetof(X64State, rflags));
	if (writer == nullptr || condition > 0x0f || !writer->EmitLoad64(9, writer->StateRegister(), rflags_offset)) return false;
	const uint8_t pair = condition >> 1u;
	switch (pair)
	{
		case 0: if (!writer->EmitUbfx64(10, 9, 11, 1)) return false; break; // O
		case 1: if (!writer->EmitUbfx64(10, 9, 0, 1)) return false; break;  // B
		case 2: if (!writer->EmitUbfx64(10, 9, 6, 1)) return false; break;  // E
		case 3: // BE
			if (!writer->EmitUbfx64(10, 9, 0, 1) || !writer->EmitUbfx64(11, 9, 6, 1) ||
			    !writer->EmitOrr64(10, 10, 11)) return false;
			break;
		case 4: if (!writer->EmitUbfx64(10, 9, 7, 1)) return false; break;  // S
		case 5: if (!writer->EmitUbfx64(10, 9, 2, 1)) return false; break;  // P
		case 6: // L
			if (!writer->EmitUbfx64(10, 9, 7, 1) || !writer->EmitUbfx64(11, 9, 11, 1) ||
			    !writer->EmitEor64(10, 10, 11)) return false;
			break;
		case 7: // LE
			if (!writer->EmitUbfx64(10, 9, 7, 1) || !writer->EmitUbfx64(11, 9, 11, 1) ||
			    !writer->EmitEor64(10, 10, 11) || !writer->EmitUbfx64(11, 9, 6, 1) ||
			    !writer->EmitOrr64(10, 10, 11)) return false;
			break;
	}
	if ((condition & 1u) != 0 &&
	    (!writer->EmitMvn64(10, 10) || !writer->EmitUbfx64(10, 10, 0, 1))) return false;
	return true;
}

Status EmitGuestBlock(const IrBlock& block, GuestCodeWriter* writer, uint8_t* direct_instruction_count,
	                  bool direct_fallthrough, bool direct_basic_block,
	                  bool batch_direct_retirement, bool direct_leaf_abi,
	                  const MultiSegmentArtifactSpec* multi_segment = nullptr,
	                  bool dense_contextless_entry = false)
{
	if (writer == nullptr || direct_instruction_count == nullptr) return Status::InvalidArgument;
	*direct_instruction_count = 0;
	const bool scalar_memory_fallthrough =
	    HasProductionScalarMemoryFallthroughEligibility(block);
	const bool multi_segment_enabled = multi_segment != nullptr;
	const size_t segment_count = multi_segment_enabled
	                                 ? multi_segment->segment_count
	                                 : 1u;
	if (segment_count == 0 || segment_count > MULTI_SEGMENT_MAX_SEGMENTS ||
	    (multi_segment_enabled && segment_count < 2))
	{
		return Status::InvalidIr;
	}
	// The 6CQ class is deliberately ordinary/framed. Do not let a future caller
	// accidentally reinterpret its verifier bit as any direct or fused ABI.
	if (scalar_memory_fallthrough &&
	    (direct_fallthrough || direct_basic_block || multi_segment_enabled ||
	     dense_contextless_entry))
	{
		return Status::InvalidIr;
	}
	batch_direct_retirement = batch_direct_retirement && direct_basic_block;
	// A signature side exit must expose the exact completed prefix. The first
	// production bundle therefore retains precise retirement within its single
	// native entry even when the process-wide 6BL selector is enabled.
	if (multi_segment_enabled) batch_direct_retirement = false;
	direct_leaf_abi = direct_leaf_abi && direct_basic_block;
	if (dense_contextless_entry &&
	    (!direct_basic_block || multi_segment_enabled ||
	     !batch_direct_retirement || !direct_leaf_abi))
	{
		return Status::InvalidIr;
	}
	uint32_t batched_length = 0;
	uint32_t batched_steps = 0;
	if (batch_direct_retirement)
	{
		for (size_t index = 0; index < block.instruction_count; index++)
		{
			const auto& instruction = block.instructions[index];
			if ((dense_contextless_entry && instruction.step_cost != 1) ||
			    batched_length > 4095u - instruction.length ||
			    batched_steps > 4095u - instruction.step_cost)
			{
				return Status::InvalidIr;
			}
			batched_length += instruction.length;
			batched_steps += instruction.step_cost;
		}
	}
	writer->SetBatchDirectRetirement(batch_direct_retirement);
	writer->SetEntryMode(
	    dense_contextless_entry
	        ? GuestEntryMode::DenseContextless
	        : (direct_leaf_abi ? GuestEntryMode::LeafContext
	                           : GuestEntryMode::FramedContext));
	std::array<BlockRflagsCachePlan, MULTI_SEGMENT_MAX_SEGMENTS> rflags_caches {};
	std::array<BlockGprCachePlan, MULTI_SEGMENT_MAX_SEGMENTS> gpr_caches {};
	std::array<size_t, MULTI_SEGMENT_MAX_SEGMENTS> segment_ends {};
	size_t merged_instructions = 0;
	for (size_t segment = 0; segment < segment_count; segment++)
	{
		const IrBlock& segment_block = multi_segment_enabled
		                                   ? multi_segment->segments[segment].block
		                                   : block;
		if (segment_block.instruction_count == 0 ||
		    merged_instructions > MAX_IR_INSTRUCTIONS -
		                              segment_block.instruction_count)
		{
			return Status::InvalidIr;
		}
		merged_instructions += segment_block.instruction_count;
		segment_ends[segment] = merged_instructions;
		if (direct_basic_block && BlockRflagsCacheRequested() &&
		    !AnalyzeVerifiedBlockRflagsCachePlan(
		        segment_block, &rflags_caches[segment]))
		{
			return Status::InvalidIr;
		}
		if (direct_basic_block && BlockGprCacheRequested() &&
		    !AnalyzeVerifiedBlockGprCachePlan(segment_block, &gpr_caches[segment]))
		{
			return Status::InvalidIr;
		}
	}
	if (merged_instructions != block.instruction_count) return Status::InvalidIr;
	writer->SetBlockStateCache(false, IR_NO_REGISTER);
	constexpr uint32_t state_offset = static_cast<uint32_t>(offsetof(EntryContext, state));
	constexpr uint32_t resume_offset = static_cast<uint32_t>(offsetof(EntryContext, resume_rip));
	constexpr uint32_t executed_offset = static_cast<uint32_t>(offsetof(EntryContext, executed_steps));
	constexpr uint32_t reason_offset = static_cast<uint32_t>(offsetof(EntryContext, exit_reason));
	constexpr uint32_t rip_offset = static_cast<uint32_t>(offsetof(X64State, rip));
	const bool has_scalar_memory = std::any_of(
	    block.instructions.begin(), block.instructions.begin() + block.instruction_count,
	    [](const IrInstruction& instruction) { return IsScalarMemoryOpcode(instruction.opcode); });
	const bool has_scalar_memory_alu = std::any_of(
	    block.instructions.begin(), block.instructions.begin() + block.instruction_count,
	    [](const IrInstruction& instruction)
	    { return IsScalarMemoryAluOpcode(instruction.opcode); });
	// Helper-capable entries retain the standard callee-saved frame. Proven
	// direct basic blocks are true leaves: their logical context/state and
	// accounting roles map to caller-saved x1--x4 and never adjust SP. The dense
	// contextless ABI arrives with x0=state and x1=sequential end, so it needs no
	// EntryContext setup at all.
	// Clang may tail-call the compact contextless ABI through BR instead of
	// BLR. Its landing pad must therefore admit both guarded jump and call
	// branch types; the historical EntryContext ABI remains call-only.
	if (!writer->Emit(dense_contextless_entry ? A64_BTI_JUMP_CALL
	                                          : A64_BTI_CALL))
	{
		return Status::CapacityExceeded;
	}
	if (!dense_contextless_entry &&
	    ((!direct_leaf_abi &&
	      (!writer->Emit(0xa9bf7bfdu) || !writer->Emit(0x910003fdu) ||
	       !writer->Emit(0xa9bf53f3u) || !writer->Emit(0xa9bf5bf5u) ||
	       (has_scalar_memory &&
	        (!writer->Emit(0xa9bf63f7u) || !writer->Emit(0xa9bf6bf9u))) ||
	       (has_scalar_memory_alu && !writer->Emit(0xa9bf73fbu)))) ||
	     (direct_leaf_abi
	          ? !writer->EmitOrr64(writer->ContextRegister(), 31, 0)
	          : !writer->Emit(0xaa0003f3u)) ||
	     !writer->EmitLoad64(writer->StateRegister(), writer->ContextRegister(), state_offset) ||
	     !writer->EmitLoad64(writer->ResumeRegister(), writer->ContextRegister(), resume_offset) ||
	     (batch_direct_retirement &&
	      !writer->EmitAddImmediate(writer->ResumeRegister(), writer->ResumeRegister(), static_cast<uint16_t>(batched_length), true)) ||
	     !writer->EmitLoad32(writer->ExecutedRegister(), writer->ContextRegister(), executed_offset)))
	{
		return Status::CapacityExceeded;
	}
	constexpr uint32_t rflags_offset =
	    static_cast<uint32_t>(offsetof(X64State, rflags));
	auto begin_segment_cache = [&](size_t segment)
	{
		const auto& rflags_cache = rflags_caches[segment];
		const auto& gpr_cache = gpr_caches[segment];
		writer->SetBlockStateCache(
		    rflags_cache.eligible,
		    gpr_cache.eligible ? gpr_cache.guest_register : IR_NO_REGISTER);
		const uint32_t cached_gpr_offset =
		    static_cast<uint32_t>(offsetof(X64State, gpr)) +
		    static_cast<uint32_t>(gpr_cache.guest_register) * sizeof(uint64_t);
		return (!rflags_cache.eligible ||
		        writer->EmitLoad64Uncached(
		            5, writer->StateRegister(), rflags_offset)) &&
		       (!gpr_cache.eligible || gpr_cache.candidate_initial_loads == 0 ||
		        writer->EmitLoad64Uncached(
		            6, writer->StateRegister(), cached_gpr_offset));
	};
	auto end_segment_cache = [&](size_t segment)
	{
		const auto& rflags_cache = rflags_caches[segment];
		const auto& gpr_cache = gpr_caches[segment];
		const uint32_t cached_gpr_offset =
		    static_cast<uint32_t>(offsetof(X64State, gpr)) +
		    static_cast<uint32_t>(gpr_cache.guest_register) * sizeof(uint64_t);
		const bool emitted =
		    (!rflags_cache.eligible || !rflags_cache.possibly_writes ||
		     writer->EmitStore64Uncached(
		         5, writer->StateRegister(), rflags_offset)) &&
		    (!gpr_cache.eligible || gpr_cache.candidate_final_stores == 0 ||
		     writer->EmitStore64Uncached(
		         6, writer->StateRegister(), cached_gpr_offset));
		writer->SetBlockStateCache(false, IR_NO_REGISTER);
		return emitted;
	};

	constexpr size_t max_signature_patches = 96;
	std::array<size_t, max_signature_patches> resume_mismatch_patches {};
	std::array<size_t, max_signature_patches> preserve_mismatch_patches {};
	std::array<size_t, MULTI_SEGMENT_MAX_SEGMENTS * 2u> clean_prefix_patches {};
	size_t resume_mismatch_count = 0;
	size_t preserve_mismatch_count = 0;
	size_t clean_prefix_count = 0;
	size_t null_mapping_patch = (std::numeric_limits<size_t>::max)();
	auto add_mismatch_patch = [&](size_t patch, bool preserve_rip)
	{
		auto& count = preserve_rip ? preserve_mismatch_count
		                           : resume_mismatch_count;
		auto& patches = preserve_rip ? preserve_mismatch_patches
		                             : resume_mismatch_patches;
		if (count >= patches.size()) return false;
		patches[count++] = patch;
		return true;
	};
	auto emit_signature_check = [&](const MultiSegmentArtifactSegment& segment,
	                                uint32_t stage, bool preserve_rip)
	{
		if (!writer->EmitImmediate64(11, stage)) return false;
		size_t signature_offset = 0;
		while (signature_offset < segment.byte_count)
		{
			const size_t remaining = segment.byte_count - signature_offset;
			const uint8_t width = remaining >= sizeof(uint64_t)
			                          ? uint8_t {8}
			                          : (remaining >= sizeof(uint32_t)
			                                 ? uint8_t {4}
			                                 : (remaining >= sizeof(uint16_t)
			                                        ? uint8_t {2}
			                                        : uint8_t {1}));
			uint64_t expected = 0;
			for (uint8_t byte = 0; byte < width; byte++)
				expected |= static_cast<uint64_t>(
				                segment.expected[signature_offset + byte])
				            << (byte * 8u);
			const uint32_t mapping_offset = static_cast<uint32_t>(
			    segment.page_offset + signature_offset);
			if (!writer->EmitAddImmediate(
			        8, 7, static_cast<uint16_t>(mapping_offset), true) ||
			    !writer->EmitLoadScalar(9, 8, width) ||
			    !writer->EmitImmediate64(10, expected) ||
			    !writer->EmitCompare64(9, 10))
			{
				return false;
			}
			const size_t mismatch = writer->Size();
			if (!writer->Emit(0x54000001u) ||
			    !add_mismatch_patch(mismatch, preserve_rip))
			{
				return false;
			}
			signature_offset += width;
		}
		return true;
	};
	auto add_clean_prefix_patch = [&](size_t patch)
	{
		if (clean_prefix_count >= clean_prefix_patches.size()) return false;
		clean_prefix_patches[clean_prefix_count++] = patch;
		return true;
	};
	if (multi_segment_enabled)
	{
		constexpr uint32_t page_mapping_offset = static_cast<uint32_t>(
		    offsetof(EntryContext, instruction_page_host_begin));
		if (!writer->EmitLoad64(
		        7, writer->ContextRegister(), page_mapping_offset) ||
		    !writer->EmitImmediate64(11, UINT32_MAX) ||
		    !writer->EmitCompareZero64(7))
		{
			return Status::CapacityExceeded;
		}
		null_mapping_patch = writer->Size();
		if (!writer->Emit(0x54000000u) ||
		    !emit_signature_check(multi_segment->segments[0], 1u, false))
		{
			return Status::CapacityExceeded;
		}
	}
	if (!begin_segment_cache(0)) return Status::CapacityExceeded;

	GuestExitPatches exits {};
	size_t active_segment = 0;
	for (size_t index = 0; index < block.instruction_count; index++)
	{
		const auto& instruction = block.instructions[index];
		const auto instruction_address = reinterpret_cast<uintptr_t>(&block.instructions[index]);
		bool direct = true;
		switch (instruction.opcode)
		{
			case IrOpcode::Nop:
				direct = writer->Emit(0xd503201fu) && EmitDirectRetirement(writer, instruction);
				break;
			case IrOpcode::MoveRegister:
			{
				const uint32_t source_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                               instruction.source * sizeof(uint64_t);
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                                    instruction.destination * sizeof(uint64_t);
				if (instruction.width == 1 || instruction.width == 2)
				{
					direct = writer->EmitLoad64(9, writer->StateRegister(), source_offset) &&
					         writer->EmitLoad64(10, writer->StateRegister(), destination_offset) &&
					         writer->EmitInsertLow64(10, 9, instruction.width) &&
					         writer->EmitStore64(10, writer->StateRegister(), destination_offset) &&
					         EmitDirectRetirement(writer, instruction);
				} else
				{
					direct = (instruction.width == 8 ? writer->EmitLoad64(9, writer->StateRegister(), source_offset)
					                                  : writer->EmitLoad32(9, writer->StateRegister(), source_offset)) &&
					         writer->EmitStore64(9, writer->StateRegister(), destination_offset) &&
					         EmitDirectRetirement(writer, instruction);
				}
				break;
			}
			case IrOpcode::MoveImmediate:
			{
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                                    instruction.destination * sizeof(uint64_t);
				if (instruction.width == 1 || instruction.width == 2)
				{
					direct = writer->EmitImmediate64(9, instruction.immediate) &&
					         writer->EmitLoad64(10, writer->StateRegister(), destination_offset) &&
					         writer->EmitInsertLow64(10, 9, instruction.width) &&
					         writer->EmitStore64(10, writer->StateRegister(), destination_offset) &&
					         EmitDirectRetirement(writer, instruction);
				} else
				{
					const uint64_t value = instruction.width == 4
					                           ? static_cast<uint32_t>(instruction.immediate)
					                           : instruction.immediate;
					direct = writer->EmitImmediate64(9, value) &&
					         writer->EmitStore64(9, writer->StateRegister(), destination_offset) &&
					         EmitDirectRetirement(writer, instruction);
				}
				break;
			}
			case IrOpcode::ZeroExtendRegister:
			case IrOpcode::SignExtendRegister:
			{
				const uint32_t source_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                               instruction.source * sizeof(uint64_t);
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                                    instruction.destination * sizeof(uint64_t);
				direct = writer->EmitLoad32(9, writer->StateRegister(), source_offset) &&
				         writer->EmitExtendRegister(10, 9, instruction.source_width, instruction.width,
				                                    instruction.opcode == IrOpcode::SignExtendRegister) &&
				         writer->EmitStore64(10, writer->StateRegister(), destination_offset) &&
				         EmitDirectRetirement(writer, instruction);
				break;
			}
			case IrOpcode::EffectiveAddress:
			{
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                                    instruction.destination * sizeof(uint64_t);
				direct = EmitEffectiveAddress(writer, instruction) &&
				         writer->EmitStore64(9, writer->StateRegister(), destination_offset) &&
				         EmitDirectRetirement(writer, instruction);
				break;
			}
			case IrOpcode::ByteSwapRegister:
			{
				const uint32_t register_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                                 instruction.source * sizeof(uint64_t);
				direct = (instruction.width == 8 ? writer->EmitLoad64(9, writer->StateRegister(), register_offset)
				                                  : writer->EmitLoad32(9, writer->StateRegister(), register_offset)) &&
				         writer->EmitByteSwap(10, 9, instruction.width) &&
				         writer->EmitStore64(10, writer->StateRegister(), register_offset) &&
				         EmitDirectRetirement(writer, instruction);
				break;
			}
			case IrOpcode::BitwiseNot:
			{
				const uint32_t register_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                                 instruction.source * sizeof(uint64_t);
				const bool width64 = instruction.width == 8;
				direct = (width64 ? writer->EmitLoad64(9, writer->StateRegister(), register_offset)
				                  : writer->EmitLoad32(9, writer->StateRegister(), register_offset)) &&
				         (width64 ? writer->EmitMvn64(10, 9) : writer->EmitMvn32(10, 9)) &&
				         writer->EmitStore64(10, writer->StateRegister(), register_offset) &&
				         EmitDirectRetirement(writer, instruction);
				break;
			}
			case IrOpcode::Negate:
			{
				const uint32_t register_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                                 instruction.source * sizeof(uint64_t);
				const bool width64 = instruction.width == 8;
				if (instruction.width == 2)
				{
					direct = writer->EmitLoad32(10, writer->StateRegister(), register_offset) &&
					         writer->EmitUbfx64(10, 10, 0, 16) &&
					         writer->EmitImmediate64(9, 0) &&
					         writer->EmitAddSubFlags(11, 9, 10, true, false) &&
					         writer->EmitUbfx64(11, 11, 0, 16) &&
					         EmitNarrowArithmeticFlags(writer, 2, true) &&
					         writer->EmitLoad64(9, writer->StateRegister(), register_offset) &&
					         writer->EmitInsertLow64(9, 11, 2) &&
					         writer->EmitStore64(9, writer->StateRegister(), register_offset) &&
					         EmitDirectRetirement(writer, instruction);
				} else
				{
					direct = (width64 ? writer->EmitLoad64(10, writer->StateRegister(), register_offset)
					                  : writer->EmitLoad32(10, writer->StateRegister(), register_offset)) &&
					         writer->EmitImmediate64(9, 0) &&
					         writer->EmitAddSubFlags(11, 9, 10, true, width64) &&
					         writer->EmitStore64(11, writer->StateRegister(), register_offset) &&
					         EmitArithmeticFlags(writer, true) &&
					         EmitDirectRetirement(writer, instruction);
				}
				break;
			}
			case IrOpcode::TestImmediate:
			{
				constexpr uint32_t rflags_offset = static_cast<uint32_t>(offsetof(X64State, rflags));
				const uint32_t source_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                               instruction.source * sizeof(uint64_t);
				const bool width64 = instruction.width == 8;
				const bool high_byte =
				    instruction.condition == IR_TEST_IMMEDIATE_HIGH_BYTE;
				constexpr uint64_t clear_mask = X64_FLAG_CF | X64_FLAG_PF | X64_FLAG_ZF |
				                                X64_FLAG_SF | X64_FLAG_OF;
				direct = (width64 || high_byte
				              ? writer->EmitLoad64(9, writer->StateRegister(), source_offset)
				              : writer->EmitLoad32(9, writer->StateRegister(), source_offset)) &&
				         (!high_byte || writer->EmitUbfx64(9, 9, 8, 8)) &&
				         writer->EmitImmediate64(10, instruction.immediate) &&
				         (width64 ? writer->EmitAnd64(11, 9, 10)
				                  : writer->EmitAnd32(11, 9, 10)) &&
				         EmitPrepareResultFlags(writer, instruction.width, clear_mask) &&
				         writer->EmitStore64(12, writer->StateRegister(), rflags_offset) &&
				         EmitDirectRetirement(writer, instruction);
				break;
			}
			case IrOpcode::AndRegister:
			case IrOpcode::OrRegister:
			case IrOpcode::XorRegister:
			case IrOpcode::TestRegister:
			{
				constexpr uint32_t rflags_offset = static_cast<uint32_t>(offsetof(X64State, rflags));
				constexpr uint64_t clear_mask = X64_FLAG_CF | X64_FLAG_PF | X64_FLAG_ZF |
				                                X64_FLAG_SF | X64_FLAG_OF;
				const bool test = instruction.opcode == IrOpcode::TestRegister;
				const bool width64 = instruction.width == 8;
				const bool narrow = instruction.width == 1 || instruction.width == 2;
				const uint8_t lhs_register = test ? instruction.source : instruction.destination;
				const uint8_t rhs_register = test ? instruction.source2 : instruction.source;
				const uint32_t lhs_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                            lhs_register * sizeof(uint64_t);
				const uint32_t rhs_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                            rhs_register * sizeof(uint64_t);

				direct = (width64 || narrow ? writer->EmitLoad64(9, writer->StateRegister(), lhs_offset)
				                              : writer->EmitLoad32(9, writer->StateRegister(), lhs_offset)) &&
				         (width64 ? writer->EmitLoad64(10, writer->StateRegister(), rhs_offset)
				                  : writer->EmitLoad32(10, writer->StateRegister(), rhs_offset));
				if (direct)
				{
					switch (instruction.opcode)
					{
						case IrOpcode::OrRegister:
							direct = width64 ? writer->EmitOrr64(11, 9, 10)
							                 : writer->EmitOrr32(11, 9, 10);
							break;
						case IrOpcode::XorRegister:
							direct = width64 ? writer->EmitEor64(11, 9, 10)
							                 : writer->EmitEor32(11, 9, 10);
							break;
						default:
							direct = width64 ? writer->EmitAnd64(11, 9, 10)
							                 : writer->EmitAnd32(11, 9, 10);
							break;
					}
				}
				if (direct && narrow)
				{
					direct = writer->EmitUbfx64(11, 11, 0,
					                           static_cast<uint8_t>(instruction.width * 8u));
				}
				if (direct && !test)
				{
					direct = narrow ? writer->EmitInsertLow64(9, 11, instruction.width) &&
					                      writer->EmitStore64(9, writer->StateRegister(), lhs_offset)
					                : writer->EmitStore64(11, writer->StateRegister(), lhs_offset);
				}
				if (direct)
				{
					direct = EmitPrepareResultFlags(writer, instruction.width, clear_mask) &&
					         writer->EmitStore64(12, writer->StateRegister(), rflags_offset) &&
					         EmitDirectRetirement(writer, instruction);
				}
				break;
			}
			case IrOpcode::VectorMoveGpr32ToXmm:
			{
				const uint32_t source_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                               instruction.source * sizeof(uint64_t);
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, xmm)) +
				                                    instruction.destination * sizeof(uint64_t) * 4u;
				direct = writer->EmitLoad32(9, writer->StateRegister(), source_offset) &&
				         writer->EmitStore64(9, writer->StateRegister(), destination_offset) &&
				         writer->EmitStore64(31, writer->StateRegister(), destination_offset + 8u) &&
				         writer->EmitStore64(31, writer->StateRegister(), destination_offset + 16u) &&
				         writer->EmitStore64(31, writer->StateRegister(), destination_offset + 24u) &&
				         EmitDirectRetirement(writer, instruction);
				break;
			}
			case IrOpcode::VectorPermuteDwordsImmediate:
			{
				const uint32_t source_offset = static_cast<uint32_t>(offsetof(X64State, xmm)) +
				                               instruction.source * sizeof(uint64_t) * 4u;
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, xmm)) +
				                                    instruction.destination * sizeof(uint64_t) * 4u;
				const uint8_t selector = static_cast<uint8_t>(instruction.immediate);
				// Keep the guest source in V0 until all four selectors have been
				// materialized in V1; this remains correct when dst aliases src.
				direct = writer->EmitLoadVector128(0, writer->StateRegister(), source_offset) &&
				         writer->EmitDuplicateVectorDword(1, 0, (selector >> 0u) & 3u) &&
				         writer->EmitInsertVectorDword(1, 1, 0, (selector >> 2u) & 3u) &&
				         writer->EmitInsertVectorDword(1, 2, 0, (selector >> 4u) & 3u) &&
				         writer->EmitInsertVectorDword(1, 3, 0, (selector >> 6u) & 3u) &&
				         writer->EmitStoreVector128(1, writer->StateRegister(), destination_offset) &&
				         writer->EmitStore64(31, writer->StateRegister(), destination_offset + 16u) &&
				         writer->EmitStore64(31, writer->StateRegister(), destination_offset + 24u) &&
				         EmitDirectRetirement(writer, instruction);
				break;
			}
			case IrOpcode::VectorMoveRegister:
			{
				const uint32_t source_offset = static_cast<uint32_t>(offsetof(X64State, xmm)) +
				                               instruction.source * sizeof(uint64_t) * 4u;
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, xmm)) +
				                                    instruction.destination * sizeof(uint64_t) * 4u;
				if (instruction.width == 32)
				{
					// Both halves must be resident before dst is touched: guest aliases
					// are legal and the state layout stores each YMM as two adjacent Qs.
					direct = writer->EmitLoadVector128(0, writer->StateRegister(), source_offset) &&
					         writer->EmitLoadVector128(1, writer->StateRegister(), source_offset + 16u) &&
					         writer->EmitStoreVector128(0, writer->StateRegister(), destination_offset) &&
					         writer->EmitStoreVector128(1, writer->StateRegister(), destination_offset + 16u) &&
					         EmitDirectRetirement(writer, instruction);
				} else
				{
					direct = writer->EmitLoadVector128(0, writer->StateRegister(), source_offset) &&
					         writer->EmitStoreVector128(0, writer->StateRegister(), destination_offset) &&
					         EmitVectorUpperPolicy(writer, instruction, destination_offset) &&
					         EmitDirectRetirement(writer, instruction);
				}
				break;
			}
			case IrOpcode::VectorMoveScalarRegister:
			{
				const uint32_t merge_offset = static_cast<uint32_t>(offsetof(X64State, xmm)) +
				                              instruction.source * sizeof(uint64_t) * 4u;
				const uint32_t scalar_offset = static_cast<uint32_t>(offsetof(X64State, xmm)) +
				                               instruction.source2 * sizeof(uint64_t) * 4u;
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, xmm)) +
				                                    instruction.destination * sizeof(uint64_t) * 4u;
				// Snapshot both merge lanes and the scalar before any destination write;
				// all three guest operands are allowed to alias.
				direct = writer->EmitLoad64(9, writer->StateRegister(), merge_offset) &&
				         writer->EmitLoad64(11, writer->StateRegister(), merge_offset + 8u) &&
				         writer->EmitLoad64(10, writer->StateRegister(), scalar_offset) &&
				         (instruction.width == 8 || writer->EmitInsertLow64(9, 10, 4)) &&
				         writer->EmitStore64(instruction.width == 8 ? 10 : 9, writer->StateRegister(),
				                             destination_offset) &&
				         writer->EmitStore64(11, writer->StateRegister(), destination_offset + 8u) &&
				         EmitVectorUpperPolicy(writer, instruction, destination_offset) &&
				         EmitDirectRetirement(writer, instruction);
				break;
			}
			case IrOpcode::VectorXorRegister:
			{
				const uint32_t lhs_offset = static_cast<uint32_t>(offsetof(X64State, xmm)) +
				                            instruction.source * sizeof(uint64_t) * 4u;
				const uint32_t rhs_offset = static_cast<uint32_t>(offsetof(X64State, xmm)) +
				                            instruction.source2 * sizeof(uint64_t) * 4u;
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, xmm)) +
				                                    instruction.destination * sizeof(uint64_t) * 4u;
				if (instruction.width == 32)
				{
					// Preload all four source Q registers before either destination Q
					// is stored. This covers dst == lhs, dst == rhs and lhs == rhs.
					direct = writer->EmitLoadVector128(0, writer->StateRegister(), lhs_offset) &&
					         writer->EmitLoadVector128(1, writer->StateRegister(), lhs_offset + 16u) &&
					         writer->EmitLoadVector128(2, writer->StateRegister(), rhs_offset) &&
					         writer->EmitLoadVector128(3, writer->StateRegister(), rhs_offset + 16u) &&
					         writer->EmitXorVector128(0, 0, 2) &&
					         writer->EmitXorVector128(1, 1, 3) &&
					         writer->EmitStoreVector128(0, writer->StateRegister(), destination_offset) &&
					         writer->EmitStoreVector128(1, writer->StateRegister(), destination_offset + 16u) &&
					         EmitDirectRetirement(writer, instruction);
				} else
				{
					direct = writer->EmitLoadVector128(0, writer->StateRegister(), lhs_offset) &&
					         writer->EmitLoadVector128(1, writer->StateRegister(), rhs_offset) &&
					         writer->EmitXorVector128(0, 0, 1) &&
					         writer->EmitStoreVector128(0, writer->StateRegister(), destination_offset) &&
					         EmitVectorUpperPolicy(writer, instruction, destination_offset) &&
					         EmitDirectRetirement(writer, instruction);
				}
				break;
			}
			case IrOpcode::AddImmediate:
			case IrOpcode::SubImmediate:
			case IrOpcode::CompareImmediate:
			{
				const bool subtract = instruction.opcode != IrOpcode::AddImmediate;
				const bool write_result = instruction.opcode != IrOpcode::CompareImmediate;
				const bool width64 = instruction.width == 8;
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                                    instruction.destination * sizeof(uint64_t);
				if (instruction.width == 1)
				{
					// Shift both byte operands to bit 31 so ADDS publishes the guest
					// carry. Restore their unshifted values before AF/OF/PF synthesis.
					direct = writer->EmitLoad32(9, writer->StateRegister(), destination_offset) &&
					         writer->EmitUbfx64(9, 9, 0, 8) &&
					         writer->EmitImmediate64(10, instruction.immediate) &&
					         writer->EmitMoveRegister32(16, 9) &&
					         writer->EmitMoveRegister32(17, 10) &&
					         writer->EmitLogicalShiftLeft32(9, 9, 24) &&
					         writer->EmitLogicalShiftLeft32(10, 10, 24) &&
					         writer->EmitAddSubFlags(11, 9, 10, false, false) &&
					         writer->EmitLogicalShiftRight32(11, 11, 24) &&
					         writer->EmitMoveRegister32(9, 16) &&
					         writer->EmitMoveRegister32(10, 17) &&
					         EmitNarrowArithmeticFlags(writer, 1, false) &&
					         writer->EmitLoad64(9, writer->StateRegister(), destination_offset) &&
					         writer->EmitInsertLow64(9, 11, 1) &&
					         writer->EmitStore64(9, writer->StateRegister(), destination_offset) &&
					         EmitDirectRetirement(writer, instruction);
				} else if (instruction.width == 2)
				{
					direct = writer->EmitLoad32(9, writer->StateRegister(), destination_offset) &&
					         writer->EmitUbfx64(9, 9, 0, 16) &&
					         writer->EmitImmediate64(10, instruction.immediate) &&
					         writer->EmitAddSubFlags(11, 9, 10, true, false) &&
					         writer->EmitUbfx64(11, 11, 0, 16) &&
					         EmitNarrowArithmeticFlags(writer, 2, true) &&
					         EmitDirectRetirement(writer, instruction);
				} else
				{
					direct = (width64 ? writer->EmitLoad64(9, writer->StateRegister(), destination_offset)
					                  : writer->EmitLoad32(9, writer->StateRegister(), destination_offset)) &&
					         writer->EmitImmediate64(10, instruction.immediate) &&
					         writer->EmitAddSubFlags(11, 9, 10, subtract, width64) &&
					         (!write_result || writer->EmitStore64(11, writer->StateRegister(), destination_offset)) &&
					         EmitArithmeticFlags(writer, subtract) &&
					         EmitDirectRetirement(writer, instruction);
				}
				break;
			}
			case IrOpcode::AddRegister:
			case IrOpcode::SubRegister:
			case IrOpcode::CompareRegister:
			{
				const bool subtract = instruction.opcode != IrOpcode::AddRegister;
				const bool write_result = instruction.opcode != IrOpcode::CompareRegister;
				const bool width64 = instruction.width == 8;
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                                    instruction.destination * sizeof(uint64_t);
				const uint32_t source_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                               instruction.source * sizeof(uint64_t);
				direct = (width64 ? writer->EmitLoad64(9, writer->StateRegister(), destination_offset)
				                  : writer->EmitLoad32(9, writer->StateRegister(), destination_offset)) &&
				         (width64 ? writer->EmitLoad64(10, writer->StateRegister(), source_offset)
				                  : writer->EmitLoad32(10, writer->StateRegister(), source_offset)) &&
				         writer->EmitAddSubFlags(11, 9, 10, subtract, width64) &&
				         (!write_result || writer->EmitStore64(11, writer->StateRegister(), destination_offset)) &&
				         EmitArithmeticFlags(writer, subtract) && EmitDirectRetirement(writer, instruction);
				break;
			}
			case IrOpcode::SubtractWithBorrowRegister:
			{
				constexpr uint32_t rflags_offset = static_cast<uint32_t>(offsetof(X64State, rflags));
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                                    instruction.destination * sizeof(uint64_t);
				const uint32_t source_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                               instruction.source * sizeof(uint64_t);
				const uint8_t bits = static_cast<uint8_t>(instruction.width * 8u);
				direct = writer->EmitLoad32(9, writer->StateRegister(), destination_offset) &&
				         writer->EmitLoad32(10, writer->StateRegister(), source_offset) &&
				         writer->EmitUbfx64(9, 9, 0, bits) &&
				         writer->EmitUbfx64(10, 10, 0, bits) &&
				         writer->EmitLoad64(12, writer->StateRegister(), rflags_offset) &&
				         writer->EmitUbfx64(14, 12, 0, 1) &&
				         writer->EmitSetCarryToInverseBit32(14) &&
				         writer->EmitSubtractWithCarryFlags(11, 9, 10, false) &&
				         writer->EmitUbfx64(11, 11, 0, bits) &&
				         EmitNarrowArithmeticFlags(writer, instruction.width, true) &&
				         writer->EmitLoad64(9, writer->StateRegister(), destination_offset) &&
				         writer->EmitInsertLow64(9, 11, instruction.width) &&
				         writer->EmitStore64(9, writer->StateRegister(), destination_offset) &&
				         EmitDirectRetirement(writer, instruction);
				break;
			}
			case IrOpcode::BitExtractRegister:
			{
				const uint32_t source_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                               instruction.source * sizeof(uint64_t);
				const uint32_t control_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                                instruction.source2 * sizeof(uint64_t);
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                                    instruction.destination * sizeof(uint64_t);
				std::array<size_t, 3> to_done {};
				direct = writer->EmitLoad32(9, writer->StateRegister(), source_offset) &&
				         writer->EmitLoad32(10, writer->StateRegister(), control_offset) &&
				         writer->EmitUbfx64(14, 10, 0, 8) &&
				         writer->EmitUbfx64(15, 10, 8, 8) &&
				         writer->EmitImmediate64(11, 0) &&
				         writer->EmitCompareImmediate(14, 32, false);
				if (direct)
				{
					to_done[0] = writer->Size();
					direct = writer->Emit(0x54000002u) &&
					         writer->EmitCompareImmediate(15, 0, false);
				}
				if (direct)
				{
					to_done[1] = writer->Size();
					direct = writer->Emit(0x54000000u) &&
					         writer->EmitLogicalShiftRightVariable32(11, 9, 14) &&
					         writer->EmitImmediate64(16, 32) &&
					         writer->EmitSubtract64(16, 16, 14) &&
					         writer->EmitCompare64(15, 16);
				}
				if (direct)
				{
					to_done[2] = writer->Size();
					direct = writer->Emit(0x54000002u) &&
					         writer->EmitImmediate64(17, UINT32_MAX) &&
					         writer->EmitLogicalShiftLeftVariable32(17, 17, 15) &&
					         writer->EmitMvn32(17, 17) &&
					         writer->EmitAnd32(11, 11, 17);
				}
				if (direct)
				{
					const size_t done = writer->Size();
					direct = writer->PatchBranchCondition(to_done[0], done, 2) &&
					         writer->PatchBranchCondition(to_done[1], done, 0) &&
					         writer->PatchBranchCondition(to_done[2], done, 2) &&
					         writer->EmitStore64(11, writer->StateRegister(), destination_offset) &&
					         EmitZeroSignFlagsPreservingParityAux(writer, 4, false) &&
					         EmitDirectRetirement(writer, instruction);
				}
				break;
			}
			case IrOpcode::AndNotRegister:
			{
				const bool width64 = instruction.width == 8;
				const uint32_t lhs_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                            instruction.source * sizeof(uint64_t);
				const uint32_t rhs_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                            instruction.source2 * sizeof(uint64_t);
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                                    instruction.destination * sizeof(uint64_t);
				direct = (width64 ? writer->EmitLoad64(9, writer->StateRegister(), lhs_offset)
				                  : writer->EmitLoad32(9, writer->StateRegister(), lhs_offset)) &&
				         (width64 ? writer->EmitLoad64(10, writer->StateRegister(), rhs_offset)
				                  : writer->EmitLoad32(10, writer->StateRegister(), rhs_offset)) &&
				         (width64 ? writer->EmitBic64(11, 10, 9)
				                  : writer->EmitBic32(11, 10, 9)) &&
				         writer->EmitStore64(11, writer->StateRegister(), destination_offset) &&
				         EmitZeroSignFlagsPreservingParityAux(writer, instruction.width) &&
				         EmitDirectRetirement(writer, instruction);
				break;
			}
			case IrOpcode::SignedMultiplyRegister:
			case IrOpcode::SignedMultiplyImmediate:
			{
				const bool width64 = instruction.width == 8;
				const uint32_t lhs_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                            instruction.source * sizeof(uint64_t);
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                                    instruction.destination * sizeof(uint64_t);
				direct = (width64 ? writer->EmitLoad64(9, writer->StateRegister(), lhs_offset)
				                  : writer->EmitLoad32(9, writer->StateRegister(), lhs_offset));
				if (direct && instruction.opcode == IrOpcode::SignedMultiplyRegister)
				{
					const uint32_t rhs_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
					                            instruction.source2 * sizeof(uint64_t);
					direct = width64 ? writer->EmitLoad64(10, writer->StateRegister(), rhs_offset)
					                 : writer->EmitLoad32(10, writer->StateRegister(), rhs_offset);
				} else if (direct)
				{
					direct = writer->EmitImmediate64(10, instruction.immediate);
				}
				if (direct && width64)
				{
					direct = writer->EmitMultiply64(11, 9, 10) &&
					         writer->EmitSignedMultiplyHigh64(13, 9, 10) &&
					         writer->EmitArithmeticShiftRight64(15, 11, 63) &&
					         writer->EmitCompare64(13, 15);
				} else if (direct)
				{
					direct = writer->EmitSignedMultiplyLong32(11, 9, 10) &&
					         writer->EmitExtendRegister(15, 11, 4, 8, true) &&
					         writer->EmitCompare64(11, 15);
				}
				if (direct)
				{
					direct = writer->EmitImmediate64(14, 1) &&
					         writer->EmitConditionalSelect64(14, 14, 31, 1) &&
					         (width64 || writer->EmitMoveRegister32(11, 11)) &&
					         writer->EmitStore64(11, writer->StateRegister(), destination_offset) &&
					         EmitMultiplyFlags(writer) &&
					         EmitDirectRetirement(writer, instruction);
				}
				break;
			}
			case IrOpcode::ShiftLeftImmediate:
			case IrOpcode::ShiftLeftCl:
			case IrOpcode::ShiftRightLogicalImmediate:
			case IrOpcode::ShiftRightLogicalCl:
			case IrOpcode::ShiftRightArithmeticImmediate:
			{
				constexpr uint32_t rflags_offset = static_cast<uint32_t>(offsetof(X64State, rflags));
				const bool rotate_left =
				    instruction.opcode == IrOpcode::ShiftLeftImmediate &&
				    instruction.condition == IR_SHIFT_IMMEDIATE_ROTATE_LEFT;
				if ((instruction.condition != 0 && !rotate_left) ||
				    (rotate_left && instruction.width != 4 && instruction.width != 8))
				{
					direct = false;
					break;
				}
				const bool shift_left = instruction.opcode == IrOpcode::ShiftLeftImmediate ||
				                        instruction.opcode == IrOpcode::ShiftLeftCl;
				const bool arithmetic = instruction.opcode == IrOpcode::ShiftRightArithmeticImmediate;
				const bool from_cl = instruction.opcode == IrOpcode::ShiftLeftCl ||
				                     instruction.opcode == IrOpcode::ShiftRightLogicalCl;
				const bool width64 = instruction.width == 8;
				const uint8_t bits = instruction.width * 8u;
				const uint8_t count = static_cast<uint8_t>(instruction.immediate);
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                                    instruction.destination * sizeof(uint64_t);
				size_t nonzero_count = 0;
				size_t count_done = 0;

				if (from_cl)
				{
					// Snapshot and normalize CL before loading a destination that may itself be RCX.
					const uint32_t count_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
					                              sizeof(uint64_t);
					direct = writer->EmitLoad32(10, writer->StateRegister(), count_offset) &&
					         writer->EmitUbfx64(10, 10, 0, width64 ? 6 : 5);
					if (direct)
					{
						nonzero_count = writer->Size();
						direct = writer->Emit(0x35000000u) &&
						         EmitDirectRetirement(writer, instruction);
					}
					if (direct)
					{
						count_done = writer->Size();
						direct = writer->Emit(0x14000000u) &&
						         writer->PatchCbnzW(nonzero_count, writer->Size(), 10);
					}
				} else if (count == 0)
				{
					direct = EmitDirectRetirement(writer, instruction);
					break;
				}

				if (direct)
				{
					direct = (instruction.width == 1 || width64
					              ? writer->EmitLoad64(9, writer->StateRegister(), destination_offset)
					              : writer->EmitLoad32(9, writer->StateRegister(), destination_offset));
				}
				if (direct && instruction.width == 1)
				{
					// A byte count is masked to five bits, but bits above bit 7 are never input data.
					direct = writer->EmitUbfx64(11, 9, 0, 8) &&
					         writer->EmitLogicalShiftRight32(11, 11, count);
				} else if (direct && rotate_left)
				{
					direct = (width64
					              ? writer->EmitLogicalShiftLeft64(11, 9, count) &&
					                    writer->EmitLogicalShiftRight64(15, 9, bits - count) &&
					                    writer->EmitOrr64(11, 11, 15)
					              : writer->EmitLogicalShiftLeft32(11, 9, count) &&
					                    writer->EmitLogicalShiftRight32(15, 9, bits - count) &&
					                    writer->EmitOrr32(11, 11, 15));
				} else if (direct && shift_left)
				{
					direct = from_cl
					             ? (width64 ? writer->EmitLogicalShiftLeftVariable64(11, 9, 10)
					                        : writer->EmitLogicalShiftLeftVariable32(11, 9, 10))
					             : (width64 ? writer->EmitLogicalShiftLeft64(11, 9, count)
					                        : writer->EmitLogicalShiftLeft32(11, 9, count));
				} else if (direct && arithmetic)
				{
					direct = width64 ? writer->EmitArithmeticShiftRight64(11, 9, count)
					                 : writer->EmitArithmeticShiftRight32(11, 9, count);
				} else if (direct)
				{
					direct = from_cl
					             ? (width64 ? writer->EmitLogicalShiftRightVariable64(11, 9, 10)
					                        : writer->EmitLogicalShiftRightVariable32(11, 9, 10))
					             : (width64 ? writer->EmitLogicalShiftRight64(11, 9, count)
					                        : writer->EmitLogicalShiftRight32(11, 9, count));
				}

				// Undefined AF and OF for counts other than one remain bit-for-bit preserved.
				const uint64_t clear_mask = rotate_left
				                                ? X64_FLAG_CF |
				                                      (count == 1 ? X64_FLAG_OF : 0)
				                                : X64_FLAG_CF | X64_FLAG_PF | X64_FLAG_ZF |
				                                      X64_FLAG_SF |
				                                      (!from_cl && count == 1 ? X64_FLAG_OF : 0);
				if (direct && rotate_left)
				{
					direct = writer->EmitLoad64(12, writer->StateRegister(), rflags_offset) &&
					         writer->EmitImmediate64(15, clear_mask) &&
					         writer->EmitBic64(12, 12, 15);
				} else if (direct)
				{
					direct = EmitPrepareResultFlags(writer, instruction.width, clear_mask);
				}
				if (direct && !(instruction.width == 1 && count > 8))
				{
					if (from_cl)
					{
						direct = writer->EmitImmediate64(14, shift_left ? bits : 1) &&
						         writer->EmitSubtract64(14, shift_left ? 14 : 10,
						                                shift_left ? 10 : 14) &&
						         (width64 ? writer->EmitLogicalShiftRightVariable64(14, 9, 14)
						                  : writer->EmitLogicalShiftRightVariable32(14, 9, 14)) &&
						         writer->EmitUbfx64(14, 14, 0, 1);
					} else
					{
						const uint8_t carry_bit = shift_left ? static_cast<uint8_t>(bits - count)
						                                     : static_cast<uint8_t>(count - 1u);
						direct = writer->EmitUbfx64(14, 9, carry_bit, 1);
					}
					if (direct) direct = writer->EmitOrr64(12, 12, 14);
				}

				if (direct && from_cl)
				{
					// Only count==1 defines OF; every other non-zero count preserves it.
					direct = writer->EmitCompareImmediate(10, 1, false);
					const size_t skip_overflow = writer->Size();
					if (direct) direct = writer->Emit(0x54000001u) &&
					                     writer->EmitImmediate64(15, X64_FLAG_OF) &&
					                     writer->EmitBic64(12, 12, 15);
					if (direct && shift_left)
					{
						direct = writer->EmitUbfx64(15, 11, static_cast<uint8_t>(bits - 1u), 1) &&
						         writer->EmitEor64(14, 15, 14) &&
						         writer->EmitOrr64(12, 12, 14, 0, 11);
					} else if (direct)
					{
						direct = writer->EmitUbfx64(15, 9, static_cast<uint8_t>(bits - 1u), 1) &&
						         writer->EmitOrr64(12, 12, 15, 0, 11);
					}
					if (direct) direct = writer->PatchBranchCondition(skip_overflow, writer->Size(), 1);
				} else if (direct && count == 1 && shift_left)
				{
					direct = writer->EmitUbfx64(15, 11, static_cast<uint8_t>(bits - 1u), 1) &&
					         writer->EmitEor64(14, 15, 14) &&
					         writer->EmitOrr64(12, 12, 14, 0, 11);
				} else if (direct && count == 1 && !arithmetic)
				{
					direct = writer->EmitUbfx64(14, 9, static_cast<uint8_t>(bits - 1u), 1) &&
					         writer->EmitOrr64(12, 12, 14, 0, 11);
				}

				if (direct && instruction.width == 1)
				{
					direct = writer->EmitInsertLow64(9, 11, 1) &&
					         writer->EmitStore64(9, writer->StateRegister(), destination_offset);
				} else if (direct)
				{
					direct = writer->EmitStore64(11, writer->StateRegister(), destination_offset);
				}
				if (direct)
				{
					direct = writer->EmitStore64(12, writer->StateRegister(), rflags_offset) &&
					         EmitDirectRetirement(writer, instruction);
				}
				if (direct && from_cl)
				{
					direct = writer->PatchBranch(count_done, writer->Size());
				}
				break;
			}
			case IrOpcode::AndImmediate:
			case IrOpcode::OrImmediate:
			case IrOpcode::XorImmediate:
			{
				constexpr uint32_t rflags_offset = static_cast<uint32_t>(offsetof(X64State, rflags));
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                                    instruction.destination * sizeof(uint64_t);
				constexpr uint64_t clear_mask = X64_FLAG_CF | X64_FLAG_PF | X64_FLAG_ZF |
				                                X64_FLAG_SF | X64_FLAG_OF;
				const bool width64 = instruction.width == 8;
				const bool narrow = instruction.width == 1 || instruction.width == 2;
				direct = (width64 || narrow ? writer->EmitLoad64(9, writer->StateRegister(), destination_offset)
				                            : writer->EmitLoad32(9, writer->StateRegister(), destination_offset)) &&
				         writer->EmitImmediate64(10, instruction.immediate);
				if (direct)
				{
					switch (instruction.opcode)
					{
						case IrOpcode::OrImmediate:
							direct = width64 ? writer->EmitOrr64(11, 9, 10)
							                 : writer->EmitOrr32(11, 9, 10);
							break;
						case IrOpcode::XorImmediate:
							direct = width64 ? writer->EmitEor64(11, 9, 10)
							                 : writer->EmitEor32(11, 9, 10);
							break;
						default:
							direct = width64 ? writer->EmitAnd64(11, 9, 10)
							                 : writer->EmitAnd32(11, 9, 10);
							break;
					}
				}
				if (direct && narrow)
				{
					direct = writer->EmitUbfx64(11, 11, 0,
					                           static_cast<uint8_t>(instruction.width * 8u)) &&
					         writer->EmitInsertLow64(9, 11, instruction.width) &&
					         writer->EmitStore64(9, writer->StateRegister(), destination_offset);
				} else if (direct)
				{
					direct = writer->EmitStore64(11, writer->StateRegister(), destination_offset);
				}
				if (direct)
				{
					direct = EmitPrepareResultFlags(writer, instruction.width, clear_mask) &&
					         writer->EmitStore64(12, writer->StateRegister(), rflags_offset) &&
					         EmitDirectRetirement(writer, instruction);
				}
				break;
			}
			case IrOpcode::BitTestRegister:
			case IrOpcode::BitTestImmediate:
			{
				constexpr uint32_t rflags_offset = static_cast<uint32_t>(offsetof(X64State, rflags));
				const uint32_t value_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                              instruction.source * sizeof(uint64_t);
				const bool width64 = instruction.width == 8;
				const bool immediate = instruction.opcode == IrOpcode::BitTestImmediate;
				direct = (width64 ? writer->EmitLoad64(9, writer->StateRegister(), value_offset)
				                  : writer->EmitLoad32(9, writer->StateRegister(), value_offset));
				if (direct && immediate)
				{
					direct = width64
					             ? writer->EmitLogicalShiftRight64(
					                   11, 9, static_cast<uint8_t>(instruction.immediate))
					             : writer->EmitLogicalShiftRight32(
					                   11, 9, static_cast<uint8_t>(instruction.immediate));
				} else if (direct)
				{
					const uint32_t bit_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
					                            instruction.source2 * sizeof(uint64_t);
					direct = (width64 ? writer->EmitLoad64(10, writer->StateRegister(), bit_offset)
					                  : writer->EmitLoad32(10, writer->StateRegister(), bit_offset)) &&
					         (width64 ? writer->EmitLogicalShiftRightVariable64(11, 9, 10)
					                  : writer->EmitLogicalShiftRightVariable32(11, 9, 10));
				}
				if (direct)
				{
					direct = writer->EmitLoad64(12, writer->StateRegister(), rflags_offset) &&
				         writer->EmitImmediate64(15, X64_FLAG_CF) &&
				         writer->EmitBic64(12, 12, 15) && writer->EmitUbfx64(14, 11, 0, 1) &&
				         writer->EmitOrr64(12, 12, 14) &&
				         writer->EmitStore64(12, writer->StateRegister(), rflags_offset) &&
				         EmitDirectRetirement(writer, instruction);
				}
				break;
			}
			case IrOpcode::Increment:
			case IrOpcode::Decrement:
			{
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                                    instruction.destination * sizeof(uint64_t);
				const bool decrement = instruction.opcode == IrOpcode::Decrement;
				const bool width64 = instruction.width == 8;
				direct = (width64 ? writer->EmitLoad64(9, writer->StateRegister(), destination_offset)
				                  : writer->EmitLoad32(9, writer->StateRegister(), destination_offset)) &&
				         writer->EmitImmediate64(10, 1) &&
				         writer->EmitAddSubFlags(11, 9, 10, decrement, width64) &&
				         writer->EmitStore64(11, writer->StateRegister(), destination_offset) &&
				         EmitArithmeticFlags(writer, decrement, true) &&
				         EmitDirectRetirement(writer, instruction);
				break;
			}
			case IrOpcode::SetCondition:
			{
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                                    instruction.destination * sizeof(uint64_t);
				direct = EmitBranchPredicate(writer, instruction.condition) &&
				         writer->EmitLoad64(9, writer->StateRegister(), destination_offset) &&
				         writer->EmitInsertLow64(9, 10, 1) &&
				         writer->EmitStore64(9, writer->StateRegister(), destination_offset) &&
				         EmitDirectRetirement(writer, instruction);
				break;
			}
			case IrOpcode::ConditionalMove:
			{
				const uint32_t destination_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                                    instruction.destination * sizeof(uint64_t);
				const uint32_t source_offset = static_cast<uint32_t>(offsetof(X64State, gpr)) +
				                               instruction.source * sizeof(uint64_t);
				const bool width64 = instruction.width == 8;
				direct = EmitBranchPredicate(writer, instruction.condition) &&
				         (width64 ? writer->EmitLoad64(9, writer->StateRegister(), destination_offset)
				                  : writer->EmitLoad32(9, writer->StateRegister(), destination_offset)) &&
				         (width64 ? writer->EmitLoad64(11, writer->StateRegister(), source_offset)
				                  : writer->EmitLoad32(11, writer->StateRegister(), source_offset)) &&
				         writer->EmitCompareZero64(10) &&
				         writer->EmitConditionalSelect64(12, 11, 9, 1) &&
				         writer->EmitStore64(12, writer->StateRegister(), destination_offset) &&
				         EmitDirectRetirement(writer, instruction);
				break;
			}
			case IrOpcode::ClearDirectionFlag:
			{
				constexpr uint32_t rflags_offset = static_cast<uint32_t>(offsetof(X64State, rflags));
				direct = writer->EmitLoad64(12, writer->StateRegister(), rflags_offset) &&
				         writer->EmitImmediate64(15, X64_FLAG_DF) &&
				         writer->EmitBic64(12, 12, 15) &&
				         writer->EmitStore64(12, writer->StateRegister(), rflags_offset) &&
				         EmitDirectRetirement(writer, instruction);
				break;
			}
			case IrOpcode::BranchCondition:
				direct = EmitBranchPredicate(writer, instruction.condition) &&
				         EmitDirectRetirement(writer, instruction) &&
				         writer->EmitImmediate64(11, instruction.target) &&
				         writer->EmitCompareZero64(10) &&
				         writer->EmitConditionalSelect64(9, 11, writer->ResumeRegister(), 1) &&
				         writer->EmitStore64(9, writer->StateRegister(), rip_offset) &&
				         (dense_contextless_entry ||
				          writer->EmitStore32(
				              31, writer->ContextRegister(), reason_offset));
				break;
			case IrOpcode::JumpImmediate:
				direct = EmitDirectRetirement(writer, instruction) &&
				         writer->EmitImmediate64(9, instruction.target) &&
				         writer->EmitStore64(9, writer->StateRegister(), rip_offset) &&
				         (dense_contextless_entry ||
				          writer->EmitStore32(
				              31, writer->ContextRegister(), reason_offset));
				break;
			case IrOpcode::Load:
			case IrOpcode::Store:
			case IrOpcode::ZeroExtendMemory:
			case IrOpcode::SignExtendMemory:
			case IrOpcode::AddRegisterMemory:
			case IrOpcode::SubRegisterMemory:
			case IrOpcode::OrRegisterMemory:
			case IrOpcode::AndRegisterMemory:
			case IrOpcode::XorRegisterMemory:
			case IrOpcode::CompareRegisterMemory:
			case IrOpcode::CompareMemoryRegister:
			case IrOpcode::CompareMemoryImmediate:
			case IrOpcode::TestMemoryImmediate:
			case IrOpcode::SignedMultiplyRegisterMemory:
			case IrOpcode::SignedMultiplyMemoryImmediate:
			case IrOpcode::AddMemoryRegister:
			case IrOpcode::SubtractMemoryRegister:
			case IrOpcode::AndMemoryRegister:
			case IrOpcode::AndMemoryImmediate:
			case IrOpcode::IncrementMemory:
			case IrOpcode::SetConditionMemory:
			case IrOpcode::VectorLoadScalar:
			case IrOpcode::VectorStoreScalar:
			case IrOpcode::PushRegister:
			case IrOpcode::PopRegister:
			case IrOpcode::CallImmediate:
			case IrOpcode::Return:
			{
				const Status status = EmitScalarMemoryInstruction(
				    writer, instruction, instruction_address, &exits);
				if (status != Status::Ok) return status;
				break;
			}
			default: direct = false; break;
		}
		if (direct)
		{
			(*direct_instruction_count)++;
			if (direct_basic_block && index + 1u == segment_ends[active_segment])
			{
				if (!end_segment_cache(active_segment))
					return Status::CapacityExceeded;
				if (multi_segment_enabled)
				{
					const bool final_segment = active_segment + 1u == segment_count;
					const bool preserve_final_rip =
					    final_segment && block.has_terminator;
					if ((!final_segment || !block.has_terminator) &&
					    !writer->EmitStore64(
					        writer->ResumeRegister(), writer->StateRegister(), rip_offset))
					{
						return Status::CapacityExceeded;
					}
					if (!final_segment)
					{
						const uint64_t successor_rip =
						    multi_segment->segments[active_segment + 1u].block.rip;
						if (!writer->EmitImmediate64(9, successor_rip) ||
						    !writer->EmitCompare64(writer->ResumeRegister(), 9))
						{
							return Status::CapacityExceeded;
						}
						const size_t unexpected_edge = writer->Size();
						if (!writer->Emit(0x54000001u) ||
						    !add_clean_prefix_patch(unexpected_edge))
						{
							return Status::CapacityExceeded;
						}
					}
					if (!emit_signature_check(
					        multi_segment->segments[active_segment],
					        static_cast<uint32_t>(2u + active_segment * 2u),
					        preserve_final_rip))
					{
						return Status::CapacityExceeded;
					}
					if (!final_segment)
					{
						constexpr uint32_t cancel_offset = static_cast<uint32_t>(
						    offsetof(X64State, execution_cancel_requested));
						if (!writer->EmitImmediate64(8, cancel_offset) ||
						    !writer->EmitAddShifted64(
						        8, writer->StateRegister(), 8, 0) ||
						    !writer->EmitLoadScalar(8, 8, 1) ||
						    !writer->EmitCompareZero64(8))
						{
							return Status::CapacityExceeded;
						}
						const size_t cancelled = writer->Size();
						if (!writer->Emit(0x54000001u) ||
						    !add_clean_prefix_patch(cancelled) ||
						    !emit_signature_check(
						        multi_segment->segments[active_segment + 1u],
						        static_cast<uint32_t>(3u + active_segment * 2u),
						        false) ||
						    !begin_segment_cache(active_segment + 1u))
						{
							return Status::CapacityExceeded;
						}
					}
				}
				active_segment++;
			}
			continue;
		}
		if (instruction.opcode == IrOpcode::Nop || instruction.opcode == IrOpcode::MoveRegister ||
		    instruction.opcode == IrOpcode::MoveImmediate || instruction.opcode == IrOpcode::AddImmediate ||
		    instruction.opcode == IrOpcode::SubImmediate || instruction.opcode == IrOpcode::CompareImmediate ||
		    instruction.opcode == IrOpcode::AddRegister || instruction.opcode == IrOpcode::SubRegister ||
		    instruction.opcode == IrOpcode::CompareRegister ||
		    instruction.opcode == IrOpcode::ShiftLeftImmediate ||
		    instruction.opcode == IrOpcode::ShiftLeftCl ||
		    instruction.opcode == IrOpcode::ShiftRightLogicalImmediate ||
		    instruction.opcode == IrOpcode::ShiftRightLogicalCl ||
		    instruction.opcode == IrOpcode::ShiftRightArithmeticImmediate ||
		    instruction.opcode == IrOpcode::AndImmediate ||
		    instruction.opcode == IrOpcode::OrImmediate ||
		    instruction.opcode == IrOpcode::XorImmediate ||
		    instruction.opcode == IrOpcode::BitTestRegister ||
		    instruction.opcode == IrOpcode::BitTestImmediate ||
		    instruction.opcode == IrOpcode::Increment ||
		    instruction.opcode == IrOpcode::Decrement ||
		    instruction.opcode == IrOpcode::SetCondition ||
		    instruction.opcode == IrOpcode::ConditionalMove ||
		    instruction.opcode == IrOpcode::ClearDirectionFlag ||
		    instruction.opcode == IrOpcode::BitwiseNot ||
		    instruction.opcode == IrOpcode::Negate ||
		    instruction.opcode == IrOpcode::TestImmediate ||
		    instruction.opcode == IrOpcode::AndRegister ||
		    instruction.opcode == IrOpcode::OrRegister ||
		    instruction.opcode == IrOpcode::XorRegister ||
		    instruction.opcode == IrOpcode::TestRegister ||
		    instruction.opcode == IrOpcode::SubtractWithBorrowRegister ||
		    instruction.opcode == IrOpcode::BitExtractRegister ||
		    instruction.opcode == IrOpcode::AndNotRegister ||
		    instruction.opcode == IrOpcode::SignedMultiplyRegister ||
		    instruction.opcode == IrOpcode::SignedMultiplyImmediate ||
		    instruction.opcode == IrOpcode::BranchCondition || instruction.opcode == IrOpcode::JumpImmediate ||
		    instruction.opcode == IrOpcode::VectorMoveGpr32ToXmm ||
		    instruction.opcode == IrOpcode::VectorPermuteDwordsImmediate ||
		    instruction.opcode == IrOpcode::VectorMoveRegister ||
		    instruction.opcode == IrOpcode::VectorMoveScalarRegister ||
		    instruction.opcode == IrOpcode::VectorXorRegister ||
		    instruction.opcode == IrOpcode::ZeroExtendRegister ||
		    instruction.opcode == IrOpcode::SignExtendRegister ||
		    instruction.opcode == IrOpcode::EffectiveAddress ||
		    instruction.opcode == IrOpcode::ByteSwapRegister)
		{
			return Status::CapacityExceeded;
		}

		if (!EmitGenericIrDispatch(writer, instruction_address, &exits))
		{
			return Status::CapacityExceeded;
		}
	}
	if (direct_basic_block &&
	    (exits.conditional_count != 0 || exits.unconditional_count != 0 ||
	     *direct_instruction_count != block.instruction_count))
	{
		return Status::InvalidIr;
	}
	if (!direct_basic_block && !end_segment_cache(0))
		return Status::CapacityExceeded;
	if (batch_direct_retirement && !dense_contextless_entry)
	{
		constexpr uint32_t direct_offset =
		    static_cast<uint32_t>(offsetof(EntryContext, direct_steps));
		if (!writer->EmitAddImmediate(writer->ExecutedRegister(), writer->ExecutedRegister(), static_cast<uint16_t>(batched_steps), false) ||
		    !writer->EmitLoad32(9, writer->ContextRegister(), direct_offset) ||
		    !writer->EmitAddImmediate(9, 9, static_cast<uint16_t>(batched_steps), false) ||
		    !writer->EmitStore32(9, writer->ContextRegister(), direct_offset))
		{
			return Status::CapacityExceeded;
		}
	}

	const bool helper_fallthrough = !block.has_terminator &&
	                                HasVerifiedHelperEligibility(block);
	if (((direct_fallthrough || scalar_memory_fallthrough) &&
	     (!writer->EmitStore64(writer->ResumeRegister(), writer->StateRegister(), rip_offset) ||
	      (!dense_contextless_entry &&
	       !writer->EmitStore32(
	           31, writer->ContextRegister(), reason_offset)))) ||
	    (helper_fallthrough &&
	     (!writer->EmitImmediate64(9, block.fallthrough_rip) ||
	      !writer->EmitStore64(9, writer->StateRegister(), rip_offset) ||
	      !writer->EmitStore32(31, writer->ContextRegister(), reason_offset))))
	{
		return Status::CapacityExceeded;
	}
	size_t normal_to_epilogue = (std::numeric_limits<size_t>::max)();
	size_t clean_prefix = (std::numeric_limits<size_t>::max)();
	size_t clean_to_epilogue = (std::numeric_limits<size_t>::max)();
	size_t resume_mismatch = (std::numeric_limits<size_t>::max)();
	size_t preserve_mismatch = (std::numeric_limits<size_t>::max)();
	if (multi_segment_enabled)
	{
		normal_to_epilogue = writer->Size();
		if (!writer->Emit(0x14000000u)) return Status::CapacityExceeded;
		clean_prefix = writer->Size();
		constexpr uint32_t mismatch_stage_offset = static_cast<uint32_t>(
		    offsetof(EntryContext, multi_segment_mismatch_stage));
		if (!writer->EmitStore64(
		        writer->ResumeRegister(), writer->StateRegister(), rip_offset) ||
		    !writer->EmitStore32(
		        31, writer->ContextRegister(), mismatch_stage_offset) ||
		    !writer->EmitStore32(31, writer->ContextRegister(), reason_offset))
		{
			return Status::CapacityExceeded;
		}
		clean_to_epilogue = writer->Size();
		if (!writer->Emit(0x14000000u)) return Status::CapacityExceeded;
		resume_mismatch = writer->Size();
		if (!writer->EmitStore64(
		        writer->ResumeRegister(), writer->StateRegister(), rip_offset))
		{
			return Status::CapacityExceeded;
		}
		preserve_mismatch = writer->Size();
		if (!writer->EmitStore32(
		        11, writer->ContextRegister(), mismatch_stage_offset) ||
		    !writer->EmitImmediate64(
		        9, static_cast<uint32_t>(ExitReason::SignatureMismatch)) ||
		    !writer->EmitStore32(9, writer->ContextRegister(), reason_offset))
		{
			return Status::CapacityExceeded;
		}
	}
	const size_t epilogue = writer->Size();
	if (dense_contextless_entry)
	{
		const uint32_t cookie = DenseContextlessCompletionCookie(
		    block.instruction_count);
		if (cookie > UINT16_MAX ||
		    !writer->Emit(
		        A64_MOVZ_64 | (cookie << 5u)) ||
		    !writer->Emit(A64_RET | (30u << 5u)))
		{
			return Status::CapacityExceeded;
		}
	} else if (
	    !writer->EmitStore64(writer->ResumeRegister(), writer->ContextRegister(), resume_offset) ||
	    !writer->EmitStore32(writer->ExecutedRegister(), writer->ContextRegister(), executed_offset) ||
	    !writer->EmitMoveRegister32(0, writer->ExecutedRegister()) ||
	    (!direct_leaf_abi &&
	     ((has_scalar_memory_alu && !writer->Emit(0xa8c173fbu)) ||
	      (has_scalar_memory &&
	       (!writer->Emit(0xa8c16bf9u) || !writer->Emit(0xa8c163f7u))) ||
	      !writer->Emit(0xa8c15bf5u) || !writer->Emit(0xa8c153f3u) ||
	      !writer->Emit(0xa8c17bfdu))) ||
	    !writer->Emit(A64_RET | (30u << 5u)))
	{
		return Status::CapacityExceeded;
	}
	for (size_t index = 0; index < exits.conditional_count; index++)
	{
		if (!writer->PatchCbnzW(exits.conditional[index], epilogue, 0)) return Status::PlatformFailure;
	}
	for (size_t index = 0; index < exits.unconditional_count; index++)
	{
		if (!writer->PatchBranch(exits.unconditional[index], epilogue)) return Status::PlatformFailure;
	}
	if (multi_segment_enabled)
	{
		if (!writer->PatchBranch(normal_to_epilogue, epilogue) ||
		    !writer->PatchBranch(clean_to_epilogue, epilogue) ||
		    null_mapping_patch == (std::numeric_limits<size_t>::max)() ||
		    !writer->PatchBranchCondition(
		        null_mapping_patch, resume_mismatch, 0))
		{
			return Status::PlatformFailure;
		}
		for (size_t index = 0; index < resume_mismatch_count; index++)
		{
			if (!writer->PatchBranchCondition(
			        resume_mismatch_patches[index], resume_mismatch, 1))
				return Status::PlatformFailure;
		}
		for (size_t index = 0; index < preserve_mismatch_count; index++)
		{
			if (!writer->PatchBranchCondition(
			        preserve_mismatch_patches[index], preserve_mismatch, 1))
				return Status::PlatformFailure;
		}
		for (size_t index = 0; index < clean_prefix_count; index++)
		{
			if (!writer->PatchBranchCondition(
			        clean_prefix_patches[index], clean_prefix, 1))
				return Status::PlatformFailure;
		}
	}
	return Status::Ok;
}

} // namespace

FastOpExecutionSnapshotAnalysis AnalyzeFastOpExecutionSnapshot(
	const X64Fast::FastOp& operation) noexcept
{
	return AnalyzeFastOpExecutionSnapshotImpl(
	    operation, Packed256Requested());
}

FastBlockOperationSnapshotAnalysis AnalyzeFastBlockOperationSnapshot(
	const X64Fast::FastOp* operations, size_t operation_count) noexcept
{
	return AnalyzeFastBlockOperationSnapshotImpl(
	    operations, operation_count, Packed256Requested());
}

bool FastOpRequiresExecutionSnapshot(
	const X64Fast::FastOp& operation)
{
	return FastOpRequiresExecutionSnapshotImpl(
	    operation, Packed256Requested());
}

bool IsJitPageRecycleEnabled()
{
	return JitPageRecycleRequested();
}

bool IsBatchDirectRetirementEnabled()
{
	return BatchDirectRetirementRequested();
}

bool IsDirectLeafAbiEnabled()
{
	return DirectLeafAbiRequested();
}

bool IsDirectContextReuseEnabled()
{
	return DirectContextReuseRequested();
}

bool IsCompactImmediateEncodingEnabled()
{
	return CompactImmediateRequested();
}

bool IsPacked256Enabled()
{
	return Packed256Requested();
}

bool IsBlockRflagsCacheEnabled()
{
	return BlockRflagsCacheRequested();
}

bool IsBlockGprCacheEnabled()
{
	return BlockGprCacheRequested();
}

bool IsMultiSegmentArtifactEnabled()
{
	return MultiSegmentArtifactRequested();
}

bool IsDenseResidentCacheEnabled()
{
	return DenseResidentCacheRequested();
}

uint64_t DenseResidentReleaseGeneration()
{
#if LUMEN_A64_DYNAREC_MACOS
	auto* arena = DenseResidentArenaGlobal();
	return arena == nullptr
	           ? 0
	           : arena->release_generation.load(std::memory_order_relaxed);
#else
	return 0;
#endif
}

#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
bool DenseResidentCacheEnvironmentValueEnabledForTest(const char* value)
{
	return DenseResidentCacheEnvironmentValueEnabled(value);
}
#endif

#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_CPU_X64_TEST)
DenseResidentArenaTestSnapshot GetDenseResidentArenaTestSnapshot()
{
	DenseResidentArenaTestSnapshot snapshot {};
	snapshot.executable_budget = DENSE_RESIDENT_EXECUTABLE_BUDGET;
#if LUMEN_A64_DYNAREC_MACOS
	snapshot.page_size = JitPageRecyclePageSize();
	auto* arena = DenseResidentArenaGlobal();
	if (arena == nullptr) return snapshot;
	std::lock_guard<std::mutex> lock(arena->mutex);
	snapshot.mapped_bytes = arena->mapped_bytes;
	snapshot.page_count = arena->page_count;
	snapshot.acquire_hits = arena->acquire_hits;
	snapshot.acquire_misses = arena->acquire_misses;
	snapshot.releases = arena->releases;
	snapshot.live_slices = arena->live_slices;
#endif
	return snapshot;
}
#endif

#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_CPU_X64_TEST) || \
    defined(LUMEN_JIT_PAGE_RECYCLE_BENCHMARK)
bool JitPageRecycleEnvironmentValueEnabledForTest(const char* value)
{
	return JitPageRecycleEnvironmentValueEnabled(value);
}

JitPageRecycleTestSnapshot GetJitPageRecycleTestSnapshot()
{
	JitPageRecycleTestSnapshot snapshot {};
#if LUMEN_A64_DYNAREC_MACOS
	snapshot.slot_capacity = JIT_PAGE_RECYCLE_SLOT_CAPACITY;
	snapshot.page_size = JitPageRecyclePageSize();
	auto* pool = JitPageRecycleGlobal();
	if (pool == nullptr) return snapshot;
	std::lock_guard<std::mutex> lock(pool->mutex);
	snapshot.resident = pool->resident;
	snapshot.acquire_hits = pool->acquire_hits;
	snapshot.acquire_misses = pool->acquire_misses;
	snapshot.retire_attempts = pool->retire_attempts;
	snapshot.retire_stored = pool->retire_stored;
	snapshot.retire_overflows = pool->retire_overflows;
	snapshot.retire_rejected = pool->retire_rejected;
#endif
	return snapshot;
}

void ResetJitPageRecyclePoolForTest()
{
#if LUMEN_A64_DYNAREC_MACOS
	auto* pool = JitPageRecycleGlobal();
	if (pool == nullptr) return;
	std::array<void*, JIT_PAGE_RECYCLE_SLOT_CAPACITY> pages {};
	size_t resident = 0;
	{
		std::lock_guard<std::mutex> lock(pool->mutex);
		resident = pool->resident;
		for (size_t index = 0; index < resident; index++)
		{
			pages[index] = pool->pages[index];
			pool->pages[index] = nullptr;
		}
		pool->resident = 0;
		pool->acquire_hits = 0;
		pool->acquire_misses = 0;
		pool->retire_attempts = 0;
		pool->retire_stored = 0;
		pool->retire_overflows = 0;
		pool->retire_rejected = 0;
	}
	for (size_t index = 0; index < resident; index++)
	{
		if (pages[index] != nullptr)
			munmap(pages[index], JitPageRecyclePageSize());
	}
#endif
}
#endif

#if defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE)
bool HostPcProfileEnvironmentRequested()
{
#if LUMEN_A64_DYNAREC_MACOS
#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
	const int32_t override_value =
	    g_host_pc_profile_test_environment.load(std::memory_order_acquire);
	if (override_value >= 0) return override_value == 1;
#endif
	static const bool requested = HostPcProfileEnvironmentValueEnabled(
	    std::getenv("LUMEN_X64_A64_DYNAREC_HOST_PC_PROFILE"));
	return requested;
#else
	return false;
#endif
}

bool PrepareHostPcProfile()
{
#if LUMEN_A64_DYNAREC_MACOS
	if (!HostPcProfileEnvironmentRequested()) return false;
	for (;;)
	{
		uint64_t control =
		    g_host_pc_profile_control.load(std::memory_order_acquire);
		const HostPcProfilePhase phase =
		    HostPcProfileControlPhase(control);
		if (phase == HostPcProfilePhase::Recording) return true;
		if (phase == HostPcProfilePhase::Preparing)
		{
			std::this_thread::yield();
			continue;
		}
		if (phase != HostPcProfilePhase::Dormant ||
		    HostPcProfileControlWriters(control) != 0)
		{
			return false;
		}
		if (!g_host_pc_profile_control.compare_exchange_weak(
		        control,
		        HostPcProfileControl(HostPcProfilePhase::Preparing, 0),
		        std::memory_order_acq_rel, std::memory_order_acquire))
		{
			continue;
		}

		const uint64_t capacity = HostPcProfileEffectiveCapacity();
		const uint64_t generation =
		    g_host_pc_profile_next_generation.fetch_add(
		        1, std::memory_order_relaxed) + 1;
		if (capacity == 0 ||
		    capacity > HOST_PC_PROFILE_RECORD_CAPACITY || generation == 0 ||
		    generation > UINT32_MAX)
		{
			HostPcProfileMarkInvalid();
			g_host_pc_profile_control.store(
			    HostPcProfileControl(HostPcProfilePhase::Frozen, 0),
			    std::memory_order_release);
			return false;
		}
		g_host_pc_profile_generation.store(generation,
		                                   std::memory_order_relaxed);
		g_host_pc_profile_pid.store(static_cast<uint64_t>(getpid()),
		                            std::memory_order_relaxed);
		g_host_pc_profile_begin_ns.store(0, std::memory_order_relaxed);
		g_host_pc_profile_cutoff_ns.store(0, std::memory_order_relaxed);
		g_host_pc_profile_next_record.store(0, std::memory_order_relaxed);
		g_host_pc_profile_retired.store(0, std::memory_order_relaxed);
		g_host_pc_profile_overflow.store(0, std::memory_order_relaxed);
		g_host_pc_profile_frozen_snapshot = {};
		g_host_pc_profile_control.store(
		    HostPcProfileControl(HostPcProfilePhase::Recording, 0),
		    std::memory_order_release);
		return true;
	}
#else
	return false;
#endif
}

bool BeginHostPcProfile(uint64_t begin_ns)
{
#if LUMEN_A64_DYNAREC_MACOS
	if (begin_ns == 0)
	{
		if (HostPcProfileEnvironmentRequested()) HostPcProfileMarkInvalid();
		return false;
	}
	if (!PrepareHostPcProfile() ||
	    HostPcProfileControlPhase(g_host_pc_profile_control.load(
	        std::memory_order_acquire)) !=
	        HostPcProfilePhase::Recording)
	{
		return false;
	}
	uint64_t expected = 0;
	return g_host_pc_profile_begin_ns.compare_exchange_strong(
	    expected, begin_ns, std::memory_order_release,
	    std::memory_order_relaxed);
#else
	(void)begin_ns;
	return false;
#endif
}

bool ClaimCloseHostPcProfile(uint64_t cutoff_ns)
{
#if LUMEN_A64_DYNAREC_MACOS
	const uint64_t begin_ns =
	    g_host_pc_profile_begin_ns.load(std::memory_order_acquire);
	if (cutoff_ns == 0 || begin_ns == 0 || cutoff_ns <= begin_ns)
	{
		if (HostPcProfileEnvironmentRequested()) HostPcProfileMarkInvalid();
		return false;
	}
	uint64_t control =
	    g_host_pc_profile_control.load(std::memory_order_acquire);
	for (;;)
	{
		if (HostPcProfileControlPhase(control) !=
		    HostPcProfilePhase::Recording)
		{
			return false;
		}
		const uint64_t desired = HostPcProfileControl(
		    HostPcProfilePhase::ClosingClaimed,
		    HostPcProfileControlWriters(control));
		if (g_host_pc_profile_control.compare_exchange_weak(
		        control, desired, std::memory_order_acq_rel,
		        std::memory_order_acquire))
		{
			control = desired;
			break;
		}
	}
	g_host_pc_profile_cutoff_ns.store(cutoff_ns,
	                                  std::memory_order_release);
	for (;;)
	{
		if (HostPcProfileControlPhase(control) !=
		    HostPcProfilePhase::ClosingClaimed)
		{
			HostPcProfileMarkInvalid();
			return false;
		}
		const uint64_t desired = HostPcProfileControl(
		    HostPcProfilePhase::Closing,
		    HostPcProfileControlWriters(control));
		if (g_host_pc_profile_control.compare_exchange_weak(
		        control, desired, std::memory_order_acq_rel,
		        std::memory_order_acquire))
		{
			break;
		}
	}
	return true;
#else
	(void)cutoff_ns;
	return false;
#endif
}

bool FinishHostPcProfileSnapshot(HostPcProfileSnapshot* snapshot)
{
#if LUMEN_A64_DYNAREC_MACOS
	if (snapshot == nullptr)
	{
		const auto phase = HostPcProfileControlPhase(
		    g_host_pc_profile_control.load(std::memory_order_acquire));
		if (phase != HostPcProfilePhase::Frozen) HostPcProfileMarkInvalid();
		return false;
	}
	for (;;)
	{
		uint64_t control =
		    g_host_pc_profile_control.load(std::memory_order_acquire);
		const HostPcProfilePhase phase =
		    HostPcProfileControlPhase(control);
		if (phase == HostPcProfilePhase::Frozen)
		{
			*snapshot = g_host_pc_profile_frozen_snapshot;
			return snapshot->complete == 1;
		}
		if (phase == HostPcProfilePhase::ClosingClaimed ||
		    phase == HostPcProfilePhase::Finishing)
		{
			std::this_thread::yield();
			continue;
		}
		if (phase != HostPcProfilePhase::Closing) return false;
		if (HostPcProfileControlWriters(control) != 0)
		{
#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
			g_host_pc_profile_test_finish_waiting.store(
			    true, std::memory_order_release);
#endif
			std::this_thread::yield();
			continue;
		}
		if (!g_host_pc_profile_control.compare_exchange_weak(
		        control,
		        HostPcProfileControl(HostPcProfilePhase::Finishing, 0),
		        std::memory_order_acq_rel, std::memory_order_acquire))
		{
			continue;
		}

		HostPcProfileSnapshot result {};
		result.schema_version = HOST_PC_PROFILE_SCHEMA_VERSION;
		result.complete       = 1;
		result.generation =
		    g_host_pc_profile_generation.load(std::memory_order_acquire);
		result.pid = g_host_pc_profile_pid.load(std::memory_order_acquire);
		result.begin_ns =
		    g_host_pc_profile_begin_ns.load(std::memory_order_acquire);
		result.cutoff_ns =
		    g_host_pc_profile_cutoff_ns.load(std::memory_order_acquire);
		result.capacity = HostPcProfileEffectiveCapacity();
		result.record_count =
		    g_host_pc_profile_next_record.load(std::memory_order_acquire);
		if (result.record_count > result.capacity ||
		    result.capacity > HOST_PC_PROFILE_RECORD_CAPACITY)
		{
			HostPcProfileMarkInvalid();
			result.record_count = (std::min)(
			    result.record_count, HOST_PC_PROFILE_RECORD_CAPACITY);
		}
		uint64_t observed_retired = 0;
		for (uint64_t index = 0; index < result.record_count; index++)
		{
			const auto& slot = g_host_pc_profile_records[
			    static_cast<size_t>(index)];
			const uint64_t ready = slot.ready_generation.load(
			    std::memory_order_acquire);
			const uint64_t retire_ns =
			    slot.retire_ns.load(std::memory_order_acquire);
			if (ready != result.generation || slot.base == 0 ||
			    slot.published_size == 0 || slot.capacity == 0 ||
			    slot.published_size > slot.capacity || slot.epoch == 0 ||
			    slot.publish_ns == 0 ||
			    slot.base > UINT64_MAX - slot.published_size ||
			    slot.base > UINT64_MAX - slot.capacity ||
			    (retire_ns != 0 && retire_ns <= slot.publish_ns))
			{
				HostPcProfileMarkInvalid();
			}
			if (retire_ns != 0) observed_retired++;
		}
		const uint64_t retired_counter =
		    g_host_pc_profile_retired.load(std::memory_order_acquire);
		if (retired_counter != observed_retired) HostPcProfileMarkInvalid();
		result.retired_count = observed_retired;
		result.overflow_count =
		    g_host_pc_profile_overflow.load(std::memory_order_acquire);
		result.invalid_count =
		    g_host_pc_profile_invalid.load(std::memory_order_acquire);
		if (result.generation == 0 || result.pid == 0 ||
		    result.begin_ns == 0 || result.cutoff_ns <= result.begin_ns)
		{
			HostPcProfileMarkInvalid();
			result.invalid_count =
			    g_host_pc_profile_invalid.load(std::memory_order_acquire);
		}
		result.checksum = HostPcProfileChecksum(result);
		g_host_pc_profile_frozen_snapshot = result;
		g_host_pc_profile_control.store(
		    HostPcProfileControl(HostPcProfilePhase::Frozen, 0),
		    std::memory_order_release);
		*snapshot = result;
		return true;
	}
#else
	(void)snapshot;
	return false;
#endif
}

bool ReadHostPcProfileRecord(const HostPcProfileSnapshot& snapshot,
	                          uint64_t index, HostPcProfileRecord* record)
{
#if LUMEN_A64_DYNAREC_MACOS
	if (record == nullptr ||
	    HostPcProfileControlPhase(g_host_pc_profile_control.load(
	        std::memory_order_acquire)) !=
	        HostPcProfilePhase::Frozen ||
	    !HostPcProfileSnapshotsMatch(
	        snapshot, g_host_pc_profile_frozen_snapshot) ||
	    index >= snapshot.record_count)
	{
		return false;
	}
	const auto& slot =
	    g_host_pc_profile_records[static_cast<size_t>(index)];
	if (slot.ready_generation.load(std::memory_order_acquire) !=
	    snapshot.generation)
	{
		return false;
	}
	record->base           = slot.base;
	record->published_size = slot.published_size;
	record->capacity       = slot.capacity;
	record->epoch          = slot.epoch;
	record->publish_ns     = slot.publish_ns;
	record->retire_ns =
	    slot.retire_ns.load(std::memory_order_acquire);
	return true;
#else
	(void)snapshot;
	(void)index;
	(void)record;
	return false;
#endif
}

#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
bool ResetHostPcProfileForTest(uint64_t effective_capacity,
	                           bool environment_requested)
{
#if LUMEN_A64_DYNAREC_MACOS
	const uint64_t control =
	    g_host_pc_profile_control.load(std::memory_order_acquire);
	if (effective_capacity == 0 ||
	    effective_capacity > HOST_PC_PROFILE_RECORD_CAPACITY ||
	    HostPcProfileControlWriters(control) != 0)
	{
		return false;
	}
	const auto phase = HostPcProfileControlPhase(control);
	if (phase == HostPcProfilePhase::Preparing ||
	    phase == HostPcProfilePhase::ClosingClaimed ||
	    phase == HostPcProfilePhase::Finishing)
	{
		return false;
	}
	const uint64_t previous_count = (std::min)(
	    g_host_pc_profile_next_record.load(std::memory_order_acquire),
	    HOST_PC_PROFILE_RECORD_CAPACITY);
	for (uint64_t index = 0; index < previous_count; index++)
	{
		auto& slot =
		    g_host_pc_profile_records[static_cast<size_t>(index)];
		slot.ready_generation.store(0, std::memory_order_relaxed);
		slot.retire_ns.store(0, std::memory_order_relaxed);
		slot.base = 0;
		slot.published_size = 0;
		slot.capacity = 0;
		slot.epoch = 0;
		slot.publish_ns = 0;
	}
	g_host_pc_profile_generation.store(0, std::memory_order_relaxed);
	g_host_pc_profile_pid.store(0, std::memory_order_relaxed);
	g_host_pc_profile_begin_ns.store(0, std::memory_order_relaxed);
	g_host_pc_profile_cutoff_ns.store(0, std::memory_order_relaxed);
	g_host_pc_profile_next_record.store(0, std::memory_order_relaxed);
	g_host_pc_profile_retired.store(0, std::memory_order_relaxed);
	g_host_pc_profile_overflow.store(0, std::memory_order_relaxed);
	g_host_pc_profile_invalid.store(0, std::memory_order_relaxed);
	g_host_pc_profile_frozen_snapshot = {};
	g_host_pc_profile_test_capacity.store(effective_capacity,
	                                      std::memory_order_release);
	g_host_pc_profile_test_environment.store(
	    environment_requested ? 1 : 0, std::memory_order_release);
	g_host_pc_profile_test_clock_enabled.store(false,
	                                           std::memory_order_relaxed);
	g_host_pc_profile_test_pause_writer.store(false,
	                                          std::memory_order_relaxed);
	g_host_pc_profile_test_writer_paused.store(false,
	                                           std::memory_order_relaxed);
	g_host_pc_profile_test_release_writer.store(true,
	                                            std::memory_order_relaxed);
	g_host_pc_profile_test_finish_waiting.store(false,
	                                            std::memory_order_relaxed);
	g_host_pc_profile_test_pause_pre_admission.store(
	    false, std::memory_order_relaxed);
	g_host_pc_profile_test_pre_admission_paused.store(
	    false, std::memory_order_relaxed);
	g_host_pc_profile_test_release_pre_admission.store(
	    true, std::memory_order_relaxed);
	g_host_pc_profile_control.store(
	    HostPcProfileControl(HostPcProfilePhase::Dormant, 0),
	    std::memory_order_release);
	return true;
#else
	(void)effective_capacity;
	(void)environment_requested;
	return false;
#endif
}

void SetHostPcProfileClockForTest(uint64_t now_ns)
{
#if LUMEN_A64_DYNAREC_MACOS
	g_host_pc_profile_test_clock_ns.store(now_ns,
	                                      std::memory_order_release);
	g_host_pc_profile_test_clock_enabled.store(true,
	                                           std::memory_order_release);
#else
	(void)now_ns;
#endif
}

void ConfigureHostPcProfilePreAdmissionPauseForTest(bool pause)
{
#if LUMEN_A64_DYNAREC_MACOS
	g_host_pc_profile_test_pre_admission_paused.store(
	    false, std::memory_order_relaxed);
	g_host_pc_profile_test_release_pre_admission.store(
	    !pause, std::memory_order_release);
	g_host_pc_profile_test_pause_pre_admission.store(
	    pause, std::memory_order_release);
#else
	(void)pause;
#endif
}

bool HostPcProfilePreAdmissionPausedForTest()
{
#if LUMEN_A64_DYNAREC_MACOS
	return g_host_pc_profile_test_pre_admission_paused.load(
	    std::memory_order_acquire);
#else
	return false;
#endif
}

void ReleaseHostPcProfilePreAdmissionForTest()
{
#if LUMEN_A64_DYNAREC_MACOS
	g_host_pc_profile_test_release_pre_admission.store(
	    true, std::memory_order_release);
#endif
}

void ConfigureHostPcProfileWriterPauseForTest(bool pause)
{
#if LUMEN_A64_DYNAREC_MACOS
	g_host_pc_profile_test_writer_paused.store(false,
	                                           std::memory_order_relaxed);
	g_host_pc_profile_test_release_writer.store(!pause,
	                                            std::memory_order_release);
	g_host_pc_profile_test_pause_writer.store(pause,
	                                          std::memory_order_release);
#else
	(void)pause;
#endif
}

bool HostPcProfileWriterPausedForTest()
{
#if LUMEN_A64_DYNAREC_MACOS
	return g_host_pc_profile_test_writer_paused.load(
	    std::memory_order_acquire);
#else
	return false;
#endif
}

bool HostPcProfileFinishWaitingForTest()
{
#if LUMEN_A64_DYNAREC_MACOS
	return g_host_pc_profile_test_finish_waiting.load(
	    std::memory_order_acquire);
#else
	return false;
#endif
}

void ReleaseHostPcProfileWriterForTest()
{
#if LUMEN_A64_DYNAREC_MACOS
	g_host_pc_profile_test_release_writer.store(true,
	                                            std::memory_order_release);
#endif
}

uint64_t RegisterHostPcProfileMappingForTest(
	uint64_t base, uint64_t published_size, uint64_t capacity,
	uint64_t epoch)
{
#if LUMEN_A64_DYNAREC_MACOS
	return RegisterHostPcProfileMapping(
	    base, published_size, capacity, epoch);
#else
	(void)base;
	(void)published_size;
	(void)capacity;
	(void)epoch;
	return 0;
#endif
}

bool RetireHostPcProfileMappingForTest(uint64_t token)
{
#if LUMEN_A64_DYNAREC_MACOS
	return RetireHostPcProfileMapping(token);
#else
	(void)token;
	return false;
#endif
}

bool HostPcProfileEnvironmentValueEnabledForTest(const char* value)
{
#if LUMEN_A64_DYNAREC_MACOS
	return HostPcProfileEnvironmentValueEnabled(value);
#else
	return value != nullptr && std::strcmp(value, "1") == 0;
#endif
}
#endif
#endif

const char* StatusName(Status status)
{
	switch (status)
	{
		case Status::Ok: return "Ok";
		case Status::UnsupportedHost: return "UnsupportedHost";
		case Status::InvalidArgument: return "InvalidArgument";
		case Status::AllocationFailed: return "AllocationFailed";
		case Status::NotAllocated: return "NotAllocated";
		case Status::CapacityExceeded: return "CapacityExceeded";
		case Status::AlreadyAllocated: return "AlreadyAllocated";
		case Status::AlreadyPublished: return "AlreadyPublished";
		case Status::UnsupportedOperation: return "UnsupportedOperation";
		case Status::InvalidIr: return "InvalidIr";
		case Status::PlatformFailure: return "PlatformFailure";
	}
	return "Unknown";
}

VerifiedIrBlock::VerifiedIrBlock(IrBlock&& block, uint8_t eligibility_mask)
    : m_block(std::move(block)), m_eligibility_mask(eligibility_mask)
{
}

VerifiedIrBlock::VerifiedIrBlock() = default;

VerifiedIrBlock::VerifiedIrBlock(LoweringStorageTag): VerifiedIrBlock() {}

const IrBlock& VerifiedIrBlock::Block() const noexcept
{
	return m_block;
}

bool VerifiedIrBlock::Supports(EligibilityPolicy policy) const noexcept
{
	switch (policy)
	{
		case EligibilityPolicy::Existing:
			return (m_eligibility_mask & VERIFIED_IR_EXISTING) != 0;
		case EligibilityPolicy::ExistingOrDirectFallthrough:
			return (m_eligibility_mask &
			        (VERIFIED_IR_EXISTING | VERIFIED_IR_DIRECT_FALLTHROUGH)) != 0;
		case EligibilityPolicy::ExistingOrDirectBasicBlock:
			return (m_eligibility_mask &
			        (VERIFIED_IR_EXISTING | VERIFIED_IR_DIRECT_BASIC_BLOCK)) != 0;
		case EligibilityPolicy::ExistingOrProductionBasicBlock:
			return (m_eligibility_mask & VERIFIED_IR_CONTAINS_TYPED_HELPER) == 0 &&
			       (m_eligibility_mask &
			        (VERIFIED_IR_EXISTING | VERIFIED_IR_DIRECT_BASIC_BLOCK)) != 0;
		case EligibilityPolicy::ExistingOrProductionScalarMemoryFallthrough:
			return (m_eligibility_mask & VERIFIED_IR_CONTAINS_TYPED_HELPER) == 0 &&
			       (m_eligibility_mask &
			        (VERIFIED_IR_EXISTING | VERIFIED_IR_DIRECT_BASIC_BLOCK |
			         VERIFIED_IR_PRODUCTION_SCALAR_MEMORY_FALLTHROUGH)) != 0;
		case EligibilityPolicy::ExistingOrVerifiedBasicBlock:
			return (m_eligibility_mask &
			        (VERIFIED_IR_EXISTING | VERIFIED_IR_DIRECT_BASIC_BLOCK |
			         VERIFIED_IR_HELPER_BASIC_BLOCK)) != 0;
		case EligibilityPolicy::DirectBasicBlockOnly:
			return (m_eligibility_mask & VERIFIED_IR_DIRECT_BASIC_BLOCK) != 0;
	}
	return false;
}

bool VerifiedIrBlock::IsDirectFallthrough() const noexcept
{
	return (m_eligibility_mask & VERIFIED_IR_DIRECT_FALLTHROUGH) != 0;
}

bool VerifiedIrBlock::IsDirectBasicBlock() const noexcept
{
	return (m_eligibility_mask & VERIFIED_IR_DIRECT_BASIC_BLOCK) != 0;
}

bool VerifiedIrBlock::IsProductionScalarMemoryFallthrough() const noexcept
{
	return (m_eligibility_mask &
	        VERIFIED_IR_PRODUCTION_SCALAR_MEMORY_FALLTHROUGH) != 0;
}

const char* SingleRegisterHelperMixedBlockReasonName(
	SingleRegisterHelperMixedBlockReason reason) noexcept
{
	switch (reason)
	{
		case SingleRegisterHelperMixedBlockReason::Eligible:
			return "Eligible";
		case SingleRegisterHelperMixedBlockReason::TypedHelperCountMismatch:
			return "TypedHelperCountMismatch";
		case SingleRegisterHelperMixedBlockReason::TypedHelperNotRegisterOnly:
			return "TypedHelperNotRegisterOnly";
		case SingleRegisterHelperMixedBlockReason::NonProductionInstruction:
			return "NonProductionInstruction";
		case SingleRegisterHelperMixedBlockReason::InvalidTerminator:
			return "InvalidTerminator";
		case SingleRegisterHelperMixedBlockReason::InsufficientProductionDirect:
			return "InsufficientProductionDirect";
	}
	return "Unknown";
}

SingleRegisterHelperMixedBlockAnalysis AnalyzeSingleRegisterHelperMixedBlock(
	const VerifiedIrBlock& verified) noexcept
{
	SingleRegisterHelperMixedBlockAnalysis result {};
	const IrBlock& block = verified.Block();
	result.instruction_count = block.instruction_count;
	bool direct_terminator_not_tail = false;

	for (size_t index = 0; index < block.instruction_count; index++)
	{
		const IrInstruction& instruction = block.instructions[index];
		const uint32_t flags =
		    ClassifyVerifiedInstructionForDiagnostics(instruction);
		result.instruction_flags[index] = flags;
		const auto has = [flags](IrInstructionDiagnosticFlag flag)
		{
			return (flags & IrInstructionDiagnosticMask(flag)) != 0;
		};

		const bool production_direct =
		    has(IrInstructionDiagnosticFlag::ProductionDirect);
		const bool typed_helper = has(IrInstructionDiagnosticFlag::TypedHelper);
		const bool direct_terminator =
		    has(IrInstructionDiagnosticFlag::DirectTerminator);
		const bool terminator = has(IrInstructionDiagnosticFlag::Terminator);
		if (production_direct)
		{
			result.production_direct_count++;
			if (!direct_terminator)
				result.production_direct_linear_count++;
		}
		if (typed_helper)
		{
			if (result.typed_helper_count == 0)
			{
				result.helper_instruction_index = static_cast<uint8_t>(index);
				result.helper_opcode = instruction.opcode;
			}
			result.typed_helper_count++;
		}
		if (!production_direct && !typed_helper)
			result.non_production_instruction_count++;
		if (has(IrInstructionDiagnosticFlag::TypedHelperRegisterOnly))
			result.typed_helper_register_only_count++;
		if (has(IrInstructionDiagnosticFlag::TypedHelperMemory))
			result.typed_helper_memory_count++;
		if (has(IrInstructionDiagnosticFlag::TypedHelperStack))
			result.typed_helper_stack_count++;
		if (has(IrInstructionDiagnosticFlag::TypedHelperAtomic))
			result.typed_helper_atomic_count++;
		if (has(IrInstructionDiagnosticFlag::TypedHelperRepeat))
			result.typed_helper_repeat_count++;
		if (has(IrInstructionDiagnosticFlag::TypedHelperIndirectControl))
			result.typed_helper_indirect_control_count++;
		if (has(IrInstructionDiagnosticFlag::ReadsRflags) ||
		    has(IrInstructionDiagnosticFlag::WritesRflags))
		{
			result.rflags_instruction_count++;
		}
		if (has(IrInstructionDiagnosticFlag::GuestMemory))
			result.guest_memory_instruction_count++;
		if (has(IrInstructionDiagnosticFlag::HostCall))
			result.host_call_instruction_count++;
		if (has(IrInstructionDiagnosticFlag::MayFault))
			result.may_fault_instruction_count++;
		if (terminator) result.terminator_count++;
		if (terminator && direct_terminator)
		{
			result.direct_terminator_count++;
			if (index + 1u != block.instruction_count)
				direct_terminator_not_tail = true;
		}
	}

	const auto reject = [&result](SingleRegisterHelperMixedBlockRejection flag)
	{
		result.rejection_flags |=
		    SingleRegisterHelperMixedBlockRejectionMask(flag);
	};
	if (result.typed_helper_count != 1)
		reject(SingleRegisterHelperMixedBlockRejection::TypedHelperCountMismatch);
	if (result.typed_helper_count !=
	    result.typed_helper_register_only_count)
	{
		reject(SingleRegisterHelperMixedBlockRejection::TypedHelperNotRegisterOnly);
	}
	if (result.production_direct_count <
	    SINGLE_REGISTER_HELPER_MIN_PRODUCTION_DIRECT)
	{
		reject(SingleRegisterHelperMixedBlockRejection::InsufficientProductionDirect);
	}
	if (result.non_production_instruction_count != 0)
		reject(SingleRegisterHelperMixedBlockRejection::NonProductionInstruction);
	if (result.terminator_count != result.direct_terminator_count)
		reject(SingleRegisterHelperMixedBlockRejection::NonDirectTerminator);
	if (direct_terminator_not_tail)
		reject(SingleRegisterHelperMixedBlockRejection::DirectTerminatorNotTail);

	if ((result.rejection_flags & SingleRegisterHelperMixedBlockRejectionMask(
	                                  SingleRegisterHelperMixedBlockRejection::TypedHelperCountMismatch)) != 0)
	{
		result.reason =
		    SingleRegisterHelperMixedBlockReason::TypedHelperCountMismatch;
	} else if ((result.rejection_flags &
	            SingleRegisterHelperMixedBlockRejectionMask(
	                SingleRegisterHelperMixedBlockRejection::TypedHelperNotRegisterOnly)) != 0)
	{
		result.reason =
		    SingleRegisterHelperMixedBlockReason::TypedHelperNotRegisterOnly;
	} else if ((result.rejection_flags &
	            SingleRegisterHelperMixedBlockRejectionMask(
	                SingleRegisterHelperMixedBlockRejection::NonProductionInstruction)) != 0)
	{
		result.reason =
		    SingleRegisterHelperMixedBlockReason::NonProductionInstruction;
	} else if ((result.rejection_flags &
	            (SingleRegisterHelperMixedBlockRejectionMask(
	                 SingleRegisterHelperMixedBlockRejection::NonDirectTerminator) |
	             SingleRegisterHelperMixedBlockRejectionMask(
	                 SingleRegisterHelperMixedBlockRejection::DirectTerminatorNotTail))) != 0)
	{
		result.reason = SingleRegisterHelperMixedBlockReason::InvalidTerminator;
	} else if ((result.rejection_flags &
	            SingleRegisterHelperMixedBlockRejectionMask(
	                SingleRegisterHelperMixedBlockRejection::InsufficientProductionDirect)) != 0)
	{
		result.reason = SingleRegisterHelperMixedBlockReason::InsufficientProductionDirect;
	} else
	{
		result.eligible = true;
		result.reason = SingleRegisterHelperMixedBlockReason::Eligible;
	}
	return result;
}

namespace {

Status LowerFastOpsWithPacked256Policy(
	const X64Fast::FastOp* operations, size_t operation_count,
	IrBlock* output, bool packed256_enabled)
{
	if (operations == nullptr || output == nullptr || operation_count == 0 || operation_count > MAX_IR_INSTRUCTIONS)
	{
		return Status::InvalidArgument;
	}

	IrBlock block {};
	block.rip = operations[0].rip;
	for (size_t index = 0; index < operation_count; index++)
	{
		const Status status = LowerInstruction(
		    operations[index], &block.instructions[index], packed256_enabled);
		if (status != Status::Ok) return status;
		if (index != 0 && operations[index].rip != block.fallthrough_rip) return Status::InvalidArgument;
		const auto& instruction = block.instructions[index];
		if (HasEffect(instruction, IrEffect::Terminator) && index + 1 != operation_count) return Status::InvalidArgument;
		block.fallthrough_rip = instruction.rip + instruction.length;
		block.semantic_mask |= InstructionSemanticMask(instruction.opcode);
	}
	block.instruction_count = static_cast<uint8_t>(operation_count);
	block.has_terminator = HasEffect(block.instructions[operation_count - 1], IrEffect::Terminator);
	const Status status = VerifyIrBlock(block);
	if (status != Status::Ok) return status;
	*output = block;
	return Status::Ok;
}

} // namespace

Status LowerFastOps(const X64Fast::FastOp* operations, size_t operation_count, IrBlock* output)
{
	return LowerFastOpsWithPacked256Policy(
	    operations, operation_count, output, Packed256Requested());
}

template <bool BranchLocal>
Status LowerFastOpsVerifiedBlock(
	const X64Fast::FastOp* operations, size_t operation_count, IrBlock* output,
	bool packed256_enabled)
{
	if (output == nullptr) return Status::InvalidArgument;
	// Build directly in the caller-owned result. Keeping a second full IrBlock
	// in this frame exceeds the emulator's 48 KiB guest-thread contract under
	// ASan once the verifier and dispatcher redzones are included.
	static_assert(std::is_trivially_copyable<IrBlock>::value,
	              "verified IR must support in-place zero initialization");
	std::memset(output, 0, sizeof(*output));
	IrBlock& block = *output;
	block.rip = operations[0].rip;
	for (size_t index = 0; index < operation_count; index++)
	{
		const Status status =
		    LowerInstructionImpl<BranchLocal>(
		        operations[index], &block.instructions[index], packed256_enabled);
		if (status != Status::Ok) return status;
		if (index != 0 && operations[index].rip != block.fallthrough_rip)
			return Status::InvalidArgument;
		const auto& instruction = block.instructions[index];
		if (HasEffect(instruction, IrEffect::Terminator) &&
		    index + 1 != operation_count)
		{
			return Status::InvalidArgument;
		}
		block.fallthrough_rip = instruction.rip + instruction.length;
		block.semantic_mask |= InstructionSemanticMask(instruction.opcode);
	}
	block.instruction_count = static_cast<uint8_t>(operation_count);
	block.has_terminator = HasEffect(
	    block.instructions[operation_count - 1], IrEffect::Terminator);
	const Status status = VerifyLoweredIrStructure(block);
	if (status != Status::Ok) return status;
	return Status::Ok;
}

VerifiedIrResult LowerFastOpsVerified(
	const X64Fast::FastOp* operations, size_t operation_count)
{
	if (operations == nullptr || operation_count == 0 ||
	    operation_count > MAX_IR_INSTRUCTIONS)
	{
		return {Status::InvalidArgument, std::nullopt};
	}

	IrBlock block {};
	const bool packed256_enabled = Packed256Requested();
	const Status status = BranchLocalIrValidationRequested()
	                          ? LowerFastOpsVerifiedBlock<true>(
	                                operations, operation_count, &block,
	                                packed256_enabled)
	                          : LowerFastOpsVerifiedBlock<false>(
	                                operations, operation_count, &block,
	                                packed256_enabled);
	if (status != Status::Ok) return {status, std::nullopt};

	const uint8_t eligibility_mask = ClassifyVerifiedIrEligibility(block);
	VerifiedIrBlock verified(std::move(block), eligibility_mask);
	return {Status::Ok,
	        std::optional<VerifiedIrBlock>(std::move(verified))};
}

Status LowerFastOpsVerified(
	const X64Fast::FastOp* operations, size_t operation_count,
	VerifiedIrBlock* output)
{
	// Reusing storage must revoke the previous proof before any operation can
	// fail and leave a partial block behind, including argument validation.
	if (output == nullptr) return Status::InvalidArgument;
	output->m_eligibility_mask = 0;
	if (operations == nullptr || operation_count == 0 ||
	    operation_count > MAX_IR_INSTRUCTIONS)
	{
		return Status::InvalidArgument;
	}
	const bool packed256_enabled = Packed256Requested();
	const Status status = BranchLocalIrValidationRequested()
	                          ? LowerFastOpsVerifiedBlock<true>(
	                                operations, operation_count, &output->m_block,
	                                packed256_enabled)
	                          : LowerFastOpsVerifiedBlock<false>(
	                                operations, operation_count, &output->m_block,
	                                packed256_enabled);
	if (status != Status::Ok) return status;
	output->m_eligibility_mask =
	    ClassifyVerifiedIrEligibility(output->m_block);
	return Status::Ok;
}

Status VerifyIrBlock(const IrBlock& block)
{
#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || defined(LUMEN_CPU_X64_TEST)
	g_ir_verification_test_counters.full_block_verifications++;
#endif
	if (block.instruction_count == 0 || block.instruction_count > MAX_IR_INSTRUCTIONS) return Status::InvalidIr;
	if (block.rip != block.instructions[0].rip) return Status::InvalidIr;

	uint64_t expected_rip = block.rip;
	uint32_t semantic_mask = 0;
	for (size_t index = 0; index < block.instruction_count; index++)
	{
		const auto& instruction = block.instructions[index];
		if (instruction.rip != expected_rip || !ValidateInstruction(instruction)) return Status::InvalidIr;
		const bool terminator = HasEffect(instruction, IrEffect::Terminator);
		if (terminator != (block.has_terminator && index + 1 == block.instruction_count)) return Status::InvalidIr;
		expected_rip = instruction.rip + instruction.length;
		semantic_mask |= InstructionSemanticMask(instruction.opcode);
	}
	if (block.fallthrough_rip != expected_rip || block.semantic_mask != semantic_mask) return Status::InvalidIr;
	return Status::Ok;
}

Status AnalyzeBlockGprCachePlan(
	const IrBlock& block, BlockGprCachePlan* output)
{
	if (output == nullptr) return Status::InvalidArgument;
	*output = {};
	const Status verification = VerifyIrBlock(block);
	if (verification != Status::Ok) return verification;
	if (!HasDirectBasicBlockEligibility(block))
		return Status::UnsupportedOperation;
	return AnalyzeVerifiedBlockGprCachePlan(block, output)
	           ? Status::Ok
	           : Status::InvalidIr;
}

bool IsRuntimeEligible(const IrBlock& block)
{
	return IsRuntimeEligible(block, EligibilityPolicy::Existing);
}

bool IsRuntimeEligible(const IrBlock& block, EligibilityPolicy policy)
{
	if (VerifyIrBlock(block) != Status::Ok) return false;
	const bool existing = HasExistingEligibility(block);
	switch (policy)
	{
		case EligibilityPolicy::Existing: return existing;
		case EligibilityPolicy::ExistingOrDirectFallthrough:
			return existing || HasDirectFallthroughEligibility(block);
		case EligibilityPolicy::ExistingOrDirectBasicBlock:
			return existing || HasDirectBasicBlockEligibility(block);
		case EligibilityPolicy::ExistingOrProductionBasicBlock:
			return !ContainsTypedHelper(block) &&
			       (existing || HasDirectBasicBlockEligibility(block));
		case EligibilityPolicy::ExistingOrProductionScalarMemoryFallthrough:
			return !ContainsTypedHelper(block) &&
			       (existing || HasDirectBasicBlockEligibility(block) ||
			        HasProductionScalarMemoryFallthroughEligibility(block));
		case EligibilityPolicy::ExistingOrVerifiedBasicBlock:
			return existing || HasDirectBasicBlockEligibility(block) ||
			       HasVerifiedHelperEligibility(block);
		case EligibilityPolicy::DirectBasicBlockOnly:
			return HasDirectBasicBlockEligibility(block);
	}
	return false;
}

bool IsRuntimeEligible(
	const VerifiedIrBlock& block, EligibilityPolicy policy)
{
	return block.Supports(policy);
}

uint32_t ExecuteIrReference(const IrBlock& block, EntryContext* context)
{
	return ExecuteIrReference(block, context, EligibilityPolicy::Existing);
}

uint32_t ExecuteIrReference(const IrBlock& block, EntryContext* context, EligibilityPolicy policy)
{
	if (context == nullptr) return 0;
	context->executed_steps = 0;
	context->direct_steps   = 0;
	context->resume_rip     = block.rip;
	context->exit_reason    = ExitReason::Bailout;
	if (!IsRuntimeEligible(block, policy) || context->state == nullptr)
	{
		return 0;
	}
	const bool direct_fallthrough = HasDirectFallthroughEligibility(block);
	const bool direct_basic_block = HasDirectBasicBlockEligibility(block) &&
	                                (policy == EligibilityPolicy::DirectBasicBlockOnly ||
	                                 policy == EligibilityPolicy::ExistingOrDirectBasicBlock ||
	                                 policy == EligibilityPolicy::ExistingOrProductionBasicBlock ||
	                                 policy == EligibilityPolicy::ExistingOrProductionScalarMemoryFallthrough ||
	                                 policy == EligibilityPolicy::ExistingOrVerifiedBasicBlock);
	const bool direct_execution = direct_fallthrough || direct_basic_block;
	if (!direct_execution && !HasRuntimeServices(context)) return 0;
	if (block.instruction_count > context->remaining_steps)
	{
		context->exit_reason = ExitReason::StepLimit;
		return 0;
	}
	if (direct_execution)
	{
		for (size_t index = 0; index < block.instruction_count; index++)
		{
			const auto& instruction = block.instructions[index];
			if (!ExecuteDirectReferenceInstruction(context->state, instruction))
			{
				context->exit_reason = ExitReason::Bailout;
				return context->executed_steps;
			}
			context->resume_rip = instruction.rip + instruction.length;
			context->executed_steps += instruction.step_cost;
			context->direct_steps += instruction.step_cost;
		}
		if (direct_fallthrough) context->state->rip = block.fallthrough_rip;
		context->exit_reason = ExitReason::Completed;
		return context->executed_steps;
	}
	for (size_t index = 0; index < block.instruction_count; index++)
	{
		if (DispatchIrStep(context, &block.instructions[index]) == StepAction::Exit) return context->executed_steps;
	}
	const bool completed_helper_fallthrough =
	    policy == EligibilityPolicy::ExistingOrVerifiedBasicBlock &&
	    HasVerifiedHelperEligibility(block) && !block.has_terminator;
	const bool completed_scalar_memory_fallthrough =
	    policy ==
	        EligibilityPolicy::ExistingOrProductionScalarMemoryFallthrough &&
	    HasProductionScalarMemoryFallthroughEligibility(block);
	if (completed_helper_fallthrough || completed_scalar_memory_fallthrough)
	{
		context->state->rip = block.fallthrough_rip;
		context->resume_rip = block.fallthrough_rip;
		context->exit_reason = ExitReason::Completed;
		return context->executed_steps;
	}
	context->exit_reason = ExitReason::Bailout;
	return context->executed_steps;
}

Status EncodeIrBlock(const IrBlock& block, void* storage, size_t capacity_bytes,
                     size_t* encoded_size, uint8_t* direct_instruction_count)
{
	return EncodeIrBlock(block, storage, capacity_bytes, encoded_size, direct_instruction_count,
	                     EligibilityPolicy::Existing);
}

Status EncodeIrBlock(const IrBlock& block, void* storage, size_t capacity_bytes,
	                 size_t* encoded_size, uint8_t* direct_instruction_count,
	                 EligibilityPolicy policy)
{
	if (encoded_size == nullptr || direct_instruction_count == nullptr) return Status::InvalidArgument;
	*encoded_size = 0;
	*direct_instruction_count = 0;
	if (storage == nullptr || capacity_bytes == 0) return Status::InvalidArgument;
	if (!IsRuntimeEligible(block, policy)) return Status::InvalidIr;
	const bool packed256_enabled = Packed256Requested();
	if (ContainsPacked256Instruction(block) && !packed256_enabled)
		return Status::InvalidIr;
	const bool direct_fallthrough = HasDirectFallthroughEligibility(block);
	const bool direct_basic_block = HasDirectBasicBlockEligibility(block);
	GuestCodeWriter writer(storage, capacity_bytes, CompactImmediateRequested());
	const Status status = EmitGuestBlock(block, &writer, direct_instruction_count,
	                                     direct_fallthrough, direct_basic_block,
	                                     BatchDirectRetirementRequested(),
	                                     DirectLeafAbiRequested());
	if (status != Status::Ok)
	{
		*direct_instruction_count = 0;
		return status;
	}
	*encoded_size = writer.Size();
	return Status::Ok;
}

#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
Status EncodeIrBlockWithBatchDirectRetirementForTest(
	const IrBlock& block, void* storage, size_t capacity_bytes,
	size_t* encoded_size, uint8_t* direct_instruction_count,
	EligibilityPolicy policy, bool batch_direct_retirement)
{
	if (encoded_size == nullptr || direct_instruction_count == nullptr)
		return Status::InvalidArgument;
	*encoded_size = 0;
	*direct_instruction_count = 0;
	if (storage == nullptr || capacity_bytes == 0) return Status::InvalidArgument;
	if (!IsRuntimeEligible(block, policy)) return Status::InvalidIr;
	if (ContainsPacked256Instruction(block) && !Packed256Requested())
		return Status::InvalidIr;

	GuestCodeWriter writer(storage, capacity_bytes);
	const Status status = EmitGuestBlock(
	    block, &writer, direct_instruction_count,
	    HasDirectFallthroughEligibility(block), HasDirectBasicBlockEligibility(block),
	    batch_direct_retirement, false);
	if (status != Status::Ok)
	{
		*direct_instruction_count = 0;
		return status;
	}
	*encoded_size = writer.Size();
	return Status::Ok;
}

Status EncodeIrBlockWithDirectLeafAbiForTest(
	const IrBlock& block, void* storage, size_t capacity_bytes,
	size_t* encoded_size, uint8_t* direct_instruction_count,
	EligibilityPolicy policy, bool batch_direct_retirement,
	bool direct_leaf_abi)
{
	if (encoded_size == nullptr || direct_instruction_count == nullptr)
		return Status::InvalidArgument;
	*encoded_size = 0;
	*direct_instruction_count = 0;
	if (storage == nullptr || capacity_bytes == 0) return Status::InvalidArgument;
	if (!IsRuntimeEligible(block, policy)) return Status::InvalidIr;
	if (ContainsPacked256Instruction(block) && !Packed256Requested())
		return Status::InvalidIr;

	GuestCodeWriter writer(storage, capacity_bytes);
	const Status status = EmitGuestBlock(
	    block, &writer, direct_instruction_count,
	    HasDirectFallthroughEligibility(block), HasDirectBasicBlockEligibility(block),
	    batch_direct_retirement, direct_leaf_abi);
	if (status != Status::Ok)
	{
		*direct_instruction_count = 0;
		return status;
	}
	*encoded_size = writer.Size();
	return Status::Ok;
}

bool BatchDirectRetirementEnvironmentValueEnabledForTest(const char* value)
{
	return BatchDirectRetirementEnvironmentValueEnabled(value);
}

bool DirectLeafAbiEnvironmentValueEnabledForTest(const char* value)
{
	return DirectLeafAbiEnvironmentValueEnabled(value);
}

bool DirectContextReuseEnvironmentValueEnabledForTest(const char* value)
{
	return DirectContextReuseEnvironmentValueEnabled(value);
}

bool CompactImmediateEnvironmentValueEnabledForTest(const char* value)
{
	return CompactImmediateEnvironmentValueEnabled(value);
}

bool BlockRflagsCacheEnvironmentValueEnabledForTest(const char* value)
{
	return BlockRflagsCacheEnvironmentValueEnabled(value);
}

bool BlockGprCacheEnvironmentValueEnabledForTest(const char* value)
{
	return BlockGprCacheEnvironmentValueEnabled(value);
}

bool MultiSegmentArtifactEnvironmentValueEnabledForTest(const char* value)
{
	return MultiSegmentArtifactEnvironmentValueEnabled(value);
}

Status LowerFastOpsWithPacked256ForTest(
	const X64Fast::FastOp* operations, size_t operation_count,
	IrBlock* output, bool packed256_enabled)
{
	return LowerFastOpsWithPacked256Policy(
	    operations, operation_count, output, packed256_enabled);
}

Status EncodeIrBlockWithPacked256ForTest(
	const IrBlock& block, void* storage, size_t capacity_bytes,
	size_t* encoded_size, uint8_t* direct_instruction_count,
	EligibilityPolicy policy, bool packed256_enabled)
{
	if (encoded_size == nullptr || direct_instruction_count == nullptr)
		return Status::InvalidArgument;
	*encoded_size = 0;
	*direct_instruction_count = 0;
	if (storage == nullptr || capacity_bytes == 0) return Status::InvalidArgument;
	if (!IsRuntimeEligible(block, policy)) return Status::InvalidIr;
	if (ContainsPacked256Instruction(block) && !packed256_enabled)
		return Status::InvalidIr;

	GuestCodeWriter writer(storage, capacity_bytes, CompactImmediateRequested());
	const Status status = EmitGuestBlock(
	    block, &writer, direct_instruction_count,
	    HasDirectFallthroughEligibility(block), HasDirectBasicBlockEligibility(block),
	    BatchDirectRetirementRequested(), DirectLeafAbiRequested());
	if (status != Status::Ok)
	{
		*direct_instruction_count = 0;
		return status;
	}
	*encoded_size = writer.Size();
	return Status::Ok;
}

bool Packed256EnvironmentValueEnabledForTest(const char* value)
{
	return Packed256EnvironmentValueEnabled(value);
}

Status EncodeImmediate64ForTest(uint8_t destination, uint64_t value, bool compact_immediates, void* storage, size_t capacity_bytes,
                                size_t* encoded_size)
{
	if (encoded_size == nullptr) return Status::InvalidArgument;
	*encoded_size = 0;
	if (destination > 30 || storage == nullptr || capacity_bytes == 0) return Status::InvalidArgument;
	GuestCodeWriter writer(storage, capacity_bytes, compact_immediates);
	if (!writer.EmitImmediate64(destination, value)) return Status::CapacityExceeded;
	*encoded_size = writer.Size();
	return Status::Ok;
}

Status EncodeIrBlockWithCompactImmediatesForTest(const IrBlock& block, void* storage, size_t capacity_bytes, size_t* encoded_size,
                                                 uint8_t* direct_instruction_count, EligibilityPolicy policy, bool batch_direct_retirement,
                                                 bool direct_leaf_abi, bool compact_immediates)
{
	if (encoded_size == nullptr || direct_instruction_count == nullptr) return Status::InvalidArgument;
	*encoded_size             = 0;
	*direct_instruction_count = 0;
	if (storage == nullptr || capacity_bytes == 0) return Status::InvalidArgument;
	if (!IsRuntimeEligible(block, policy)) return Status::InvalidIr;
	if (ContainsPacked256Instruction(block) && !Packed256Requested())
		return Status::InvalidIr;

	GuestCodeWriter writer(storage, capacity_bytes, compact_immediates);
	const Status    status = EmitGuestBlock(block, &writer, direct_instruction_count, HasDirectFallthroughEligibility(block),
	                                        HasDirectBasicBlockEligibility(block), batch_direct_retirement, direct_leaf_abi);
	if (status != Status::Ok)
	{
		*direct_instruction_count = 0;
		return status;
	}
	*encoded_size = writer.Size();
	return Status::Ok;
}
#endif

#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_VERIFIED_IR_BENCHMARK) || \
    defined(LUMEN_JIT_PUBLICATION_BENCHMARK) || \
    defined(LUMEN_JIT_PAGE_RECYCLE_BENCHMARK)
Status EncodeVerifiedIrBlockForTest(
	const VerifiedIrBlock& block, void* storage, size_t capacity_bytes,
	size_t* encoded_size, uint8_t* direct_instruction_count,
	EligibilityPolicy policy)
{
	if (encoded_size == nullptr || direct_instruction_count == nullptr)
		return Status::InvalidArgument;
	*encoded_size = 0;
	*direct_instruction_count = 0;
	if (storage == nullptr || capacity_bytes == 0)
		return Status::InvalidArgument;
	if (!block.Supports(policy)) return Status::InvalidIr;
	if (ContainsPacked256Instruction(block.Block()) && !Packed256Requested())
		return Status::InvalidIr;

	GuestCodeWriter writer(storage, capacity_bytes, CompactImmediateRequested());
	const Status status = EmitGuestBlock(
	    block.Block(), &writer, direct_instruction_count,
	    block.IsDirectFallthrough(), block.IsDirectBasicBlock(),
	    BatchDirectRetirementRequested(), DirectLeafAbiRequested());
	if (status != Status::Ok)
	{
		*direct_instruction_count = 0;
		return status;
	}
	*encoded_size = writer.Size();
	return Status::Ok;
}
#endif

#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || defined(LUMEN_CPU_X64_TEST)
void ResetIrVerificationTestCounters()
{
	g_ir_verification_test_counters = {};
}

IrVerificationTestCounters GetIrVerificationTestCounters()
{
	return g_ir_verification_test_counters;
}
#endif

#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
IrInstructionValidationPairForTest CompareIrInstructionValidatorsForTest(
	const IrInstruction& instruction)
{
	return {ValidateInstructionEagerCore(instruction),
	        ValidateInstructionBranchLocalCore(instruction)};
}

bool BranchLocalIrValidationEnvironmentValueEnabledForTest(const char* value)
{
	return BranchLocalIrValidationEnvironmentValueEnabled(value);
}

bool IsBranchLocalIrValidationEnabledForTest()
{
	return BranchLocalIrValidationRequested();
}

Status EncodePhase6ByBoundaryProofForTest(
	const Phase6ByBoundarySegmentForTest* segments, size_t segment_count,
	void* storage, size_t capacity_bytes, size_t* encoded_size)
{
	if (encoded_size == nullptr) return Status::InvalidArgument;
	*encoded_size = 0;
	if (segments == nullptr || storage == nullptr || capacity_bytes == 0 ||
	    (capacity_bytes % A64_WORD_SIZE) != 0 || segment_count < 2 ||
	    segment_count > PHASE6BY_BOUNDARY_MAX_SEGMENTS)
	{
		return Status::InvalidArgument;
	}
	for (size_t index = 0; index < segment_count; index++)
	{
		const auto& segment = segments[index];
		if (segment.byte_count == 0 ||
		    segment.byte_count > PHASE6BY_BOUNDARY_MAX_SIGNATURE_BYTES ||
		    segment.page_offset >= PHASE6BY_BOUNDARY_PAGE_SIZE ||
		    segment.byte_count >
		        PHASE6BY_BOUNDARY_PAGE_SIZE - segment.page_offset)
		{
			return Status::InvalidArgument;
		}
	}

	static_assert(
	    std::is_standard_layout<Phase6ByBoundaryInvocationForTest>::value,
	    "phase-6BY proof invocation must have a fixed standard layout");
	static_assert(std::atomic<uint32_t>::is_always_lock_free,
	              "phase-6BY proof handshake requires lock-free 32-bit atomics");
	static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t),
	              "phase-6BY proof atomics must occupy one native word");
	static_assert(sizeof(void*) == sizeof(uint64_t),
	              "phase-6BY proof requires a 64-bit invocation pointer ABI");
	constexpr uint32_t page_offset = static_cast<uint32_t>(
	    offsetof(Phase6ByBoundaryInvocationForTest, page_host_begin));
	constexpr uint32_t completed_offset = static_cast<uint32_t>(
	    offsetof(Phase6ByBoundaryInvocationForTest, completed_segments));
	constexpr uint32_t release_offset = static_cast<uint32_t>(
	    offsetof(Phase6ByBoundaryInvocationForTest, checkpoint_release));
	constexpr uint32_t mismatch_offset = static_cast<uint32_t>(
	    offsetof(Phase6ByBoundaryInvocationForTest, mismatch_stage));
	static_assert((page_offset % sizeof(uint64_t)) == 0,
	              "unaligned phase-6BY page mapping");
	static_assert((completed_offset % sizeof(uint32_t)) == 0 &&
	                  (release_offset % sizeof(uint32_t)) == 0 &&
	                  (mismatch_offset % sizeof(uint32_t)) == 0,
	              "unaligned phase-6BY handshake word");
	static_assert(mismatch_offset <= 4095u,
	              "phase-6BY proof invocation exceeds ADD-immediate range");

	GuestCodeWriter writer(storage, capacity_bytes, false);
	// 63 bytes is the worst decomposition: 7x8 followed by 4, 2 and 1.
	constexpr size_t max_signature_chunks =
	    PHASE6BY_BOUNDARY_MAX_SIGNATURE_BYTES / sizeof(uint64_t) + 2u;
	constexpr size_t max_exit_patches =
	    2u * PHASE6BY_BOUNDARY_MAX_SEGMENTS * max_signature_chunks;
	std::array<size_t, max_exit_patches> exit_patches {};
	size_t exit_patch_count = 0;
	const auto add_exit_patch = [&](size_t patch) {
		if (exit_patch_count >= exit_patches.size()) return false;
		exit_patches[exit_patch_count++] = patch;
		return true;
	};
	const auto emit_context_address = [&](uint8_t destination,
	                                      uint32_t offset) {
		return writer.EmitAddImmediate(destination, 0, static_cast<uint16_t>(offset),
		                               true);
	};
	const auto emit_signature_check = [&](const Phase6ByBoundarySegmentForTest& segment,
	                                      uint32_t stage) {
		if (!writer.EmitImmediate64(13, stage)) return false;
		size_t signature_offset = 0;
		while (signature_offset < segment.byte_count)
		{
			const size_t remaining = segment.byte_count - signature_offset;
			const uint8_t width = remaining >= sizeof(uint64_t)
			                          ? uint8_t {8}
			                          : (remaining >= sizeof(uint32_t)
			                                 ? uint8_t {4}
			                                 : (remaining >= sizeof(uint16_t)
			                                        ? uint8_t {2}
			                                        : uint8_t {1}));
			uint64_t expected = 0;
			for (uint8_t byte = 0; byte < width; byte++)
			{
				expected |= static_cast<uint64_t>(
				                segment.expected[signature_offset + byte])
				            << (byte * 8u);
			}
			const uint32_t mapping_offset = static_cast<uint32_t>(
			    segment.page_offset + signature_offset);
			if (!writer.EmitAddImmediate(15, 9,
			                             static_cast<uint16_t>(mapping_offset), true) ||
			    !writer.EmitLoadScalar(10, 15, width) ||
			    !writer.EmitImmediate64(11, expected) ||
			    !writer.EmitCompare64(10, 11))
			{
				return false;
			}
			const size_t mismatch = writer.Size();
			if (!writer.Emit(0x54000001u) || !add_exit_patch(mismatch))
				return false;
			signature_offset += width;
		}
		return true;
	};

	// x0 remains the invocation pointer until the common exit. x9 owns only
	// this invocation's borrowed host mapping; it is never stored or embedded.
	if (!writer.Emit(A64_BTI_CALL) ||
	    !writer.EmitLoad64(9, 0, page_offset) ||
	    !writer.EmitImmediate64(14, 0) ||
	    !emit_context_address(15, completed_offset) ||
	    !writer.EmitStoreRelease32(14, 15) ||
	    !writer.EmitImmediate64(13, PHASE6BY_BOUNDARY_INVALID_MAPPING_STAGE) ||
	    !writer.EmitCompareZero64(9))
	{
		return Status::CapacityExceeded;
	}
	const size_t null_mapping = writer.Size();
	if (!writer.Emit(0x54000000u) ||
	    !emit_signature_check(segments[0],
	                          Phase6ByInitialSourceStageForTest()))
	{
		return Status::CapacityExceeded;
	}

	for (size_t index = 0; index < segment_count; index++)
	{
		const uint32_t completed = static_cast<uint32_t>(index + 1u);
		if (!writer.EmitImmediate64(14, completed) ||
		    !emit_context_address(15, completed_offset) ||
		    !writer.EmitStoreRelease32(14, 15) ||
		    !emit_context_address(15, release_offset))
		{
			return Status::CapacityExceeded;
		}
		const size_t wait = writer.Size();
		if (!writer.EmitLoadAcquire32(12, 15) ||
		    !writer.EmitCompare32(12, 14))
		{
			return Status::CapacityExceeded;
		}
		const size_t wait_more = writer.Size();
		if (!writer.Emit(0x54000003u) ||
		    !writer.PatchBranchCondition(wait_more, wait, 3) ||
		    !emit_signature_check(
		        segments[index], Phase6BySourcePostStageForTest(index)))
		{
			return Status::CapacityExceeded;
		}
		if (index + 1u < segment_count &&
		    !emit_signature_check(
		        segments[index + 1u],
		        Phase6BySuccessorPreStageForTest(index)))
		{
			return Status::CapacityExceeded;
		}
	}

	if (!writer.EmitImmediate64(13, 0)) return Status::CapacityExceeded;
	const size_t exit = writer.Size();
	if (!emit_context_address(15, mismatch_offset) ||
	    !writer.EmitStoreRelease32(13, 15) ||
	    !writer.EmitMoveRegister32(0, 14) ||
	    !writer.Emit(A64_RET | (30u << 5u)))
	{
		return Status::CapacityExceeded;
	}
	if (!writer.PatchBranchCondition(null_mapping, exit, 0))
		return Status::PlatformFailure;
	for (size_t index = 0; index < exit_patch_count; index++)
	{
		if (!writer.PatchBranchCondition(exit_patches[index], exit, 1))
			return Status::PlatformFailure;
	}
	*encoded_size = writer.Size();
	return Status::Ok;
}
#endif

#if defined(LUMEN_IR_VALIDATOR_BENCHMARK)
bool ValidateIrInstructionForBenchmark(
	const IrInstruction& instruction, bool branch_local)
{
	return branch_local ? ValidateInstructionBranchLocalCore(instruction)
	                    : ValidateInstructionEagerCore(instruction);
}
#endif

A64CodeBuffer::A64CodeBuffer(void* storage, size_t capacity_bytes)
    : m_storage(static_cast<uint8_t*>(storage)), m_capacity_bytes(capacity_bytes)
{
}

Status A64CodeBuffer::Reserve(size_t word_count) const
{
	if (m_storage == nullptr || m_capacity_bytes == 0 || (m_capacity_bytes % A64_WORD_SIZE) != 0 || (m_size_bytes % A64_WORD_SIZE) != 0)
	{
		return Status::InvalidArgument;
	}
	if (m_size_bytes > m_capacity_bytes || word_count > (m_capacity_bytes - m_size_bytes) / A64_WORD_SIZE)
	{
		return Status::CapacityExceeded;
	}
	return Status::Ok;
}

void A64CodeBuffer::EmitUnchecked(uint32_t word)
{
	// AArch64 code is serialized explicitly as little-endian bytes so encoding
	// tests remain deterministic even when they run on a non-AArch64 host.
	m_storage[m_size_bytes + 0] = static_cast<uint8_t>(word);
	m_storage[m_size_bytes + 1] = static_cast<uint8_t>(word >> 8u);
	m_storage[m_size_bytes + 2] = static_cast<uint8_t>(word >> 16u);
	m_storage[m_size_bytes + 3] = static_cast<uint8_t>(word >> 24u);
	m_size_bytes += A64_WORD_SIZE;
}

Status A64CodeBuffer::EmitBtiCall()
{
	const Status status = Reserve(1);
	if (status != Status::Ok) return status;
	EmitUnchecked(A64_BTI_CALL);
	return Status::Ok;
}

Status A64CodeBuffer::EmitMoveZero64(uint8_t destination, uint16_t immediate, uint8_t shift)
{
	if (destination >= 31 || shift > 48 || (shift % 16) != 0) return Status::InvalidArgument;
	const Status status = Reserve(1);
	if (status != Status::Ok) return status;
	const uint32_t halfword = static_cast<uint32_t>(shift / 16);
	EmitUnchecked(A64_MOVZ_64 | (halfword << 21u) | (static_cast<uint32_t>(immediate) << 5u) | destination);
	return Status::Ok;
}

Status A64CodeBuffer::EmitMoveKeep64(uint8_t destination, uint16_t immediate, uint8_t shift)
{
	if (destination >= 31 || shift > 48 || (shift % 16) != 0) return Status::InvalidArgument;
	const Status status = Reserve(1);
	if (status != Status::Ok) return status;
	const uint32_t halfword = static_cast<uint32_t>(shift / 16);
	EmitUnchecked(A64_MOVK_64 | (halfword << 21u) | (static_cast<uint32_t>(immediate) << 5u) | destination);
	return Status::Ok;
}

Status A64CodeBuffer::EmitMoveImmediate64(uint8_t destination, uint64_t value)
{
	if (destination >= 31) return Status::InvalidArgument;
	const Status status = Reserve(4);
	if (status != Status::Ok) return status;

	EmitUnchecked(A64_MOVZ_64 | (static_cast<uint32_t>(value & 0xffffu) << 5u) | destination);
	for (uint32_t halfword = 1; halfword < 4; halfword++)
	{
		const uint32_t immediate = static_cast<uint32_t>((value >> (halfword * 16u)) & 0xffffu);
		EmitUnchecked(A64_MOVK_64 | (halfword << 21u) | (immediate << 5u) | destination);
	}
	return Status::Ok;
}

Status A64CodeBuffer::EmitReturn(uint8_t register_index)
{
	if (register_index >= 31) return Status::InvalidArgument;
	const Status status = Reserve(1);
	if (status != Status::Ok) return status;
	EmitUnchecked(A64_RET | (static_cast<uint32_t>(register_index) << 5u));
	return Status::Ok;
}

Status A64CodeBuffer::EmitReturnConstant(uint64_t value)
{
	const Status status = Reserve(6);
	if (status != Status::Ok) return status;

	EmitUnchecked(A64_BTI_CALL);
	EmitUnchecked(A64_MOVZ_64 | (static_cast<uint32_t>(value & 0xffffu) << 5u));
	for (uint32_t halfword = 1; halfword < 4; halfword++)
	{
		const uint32_t immediate = static_cast<uint32_t>((value >> (halfword * 16u)) & 0xffffu);
		EmitUnchecked(A64_MOVK_64 | (halfword << 21u) | (immediate << 5u));
	}
	EmitUnchecked(A64_RET | (30u << 5u));
	return Status::Ok;
}

Status A64CodeBuffer::EmitEntryContextSmoke()
{
	const Status status = Reserve(6);
	if (status != Status::Ok) return status;

	constexpr uint32_t remaining_offset = static_cast<uint32_t>(offsetof(EntryContext, remaining_steps));
	constexpr uint32_t executed_offset  = static_cast<uint32_t>(offsetof(EntryContext, executed_steps));
	constexpr uint32_t reason_offset    = static_cast<uint32_t>(offsetof(EntryContext, exit_reason));
	constexpr uint32_t context_register = 9;

	EmitUnchecked(A64_BTI_CALL);
	EmitUnchecked(A64_MOV_X9_X0);
	EmitUnchecked(A64_LDR_W | ((remaining_offset / 4u) << 10u) | (context_register << 5u));
	EmitUnchecked(A64_STR_W | ((executed_offset / 4u) << 10u) | (context_register << 5u));
	EmitUnchecked(A64_STR_W | ((reason_offset / 4u) << 10u) | (context_register << 5u) | 31u);
	EmitUnchecked(A64_RET | (30u << 5u));
	return Status::Ok;
}

const void* A64CodeBuffer::Data() const
{
	return m_storage;
}

uint32_t A64CodeBuffer::WordAt(size_t index) const
{
	if (m_storage == nullptr || index >= WordCount()) return 0;
	const size_t offset = index * A64_WORD_SIZE;
	return static_cast<uint32_t>(m_storage[offset + 0]) | (static_cast<uint32_t>(m_storage[offset + 1]) << 8u) |
	       (static_cast<uint32_t>(m_storage[offset + 2]) << 16u) | (static_cast<uint32_t>(m_storage[offset + 3]) << 24u);
}

size_t A64CodeBuffer::WordCount() const
{
	return m_size_bytes / A64_WORD_SIZE;
}

size_t A64CodeBuffer::SizeBytes() const
{
	return m_size_bytes;
}

size_t A64CodeBuffer::CapacityBytes() const
{
	return m_capacity_bytes;
}

ExecutableCodeMemory::~ExecutableCodeMemory()
{
	Reset();
}

Status CompiledBlock::Compile(const IrBlock& block)
{
	return Compile(block, EligibilityPolicy::Existing);
}

Status CompiledBlock::Compile(const IrBlock& block, EligibilityPolicy policy)
{
	return CompileWithMinimumCapacity(block, policy, 0);
}

Status CompiledBlock::CompileVerified(
	const VerifiedIrBlock& block, EligibilityPolicy policy)
{
	if (m_code.IsPublished()) return Status::AlreadyPublished;
	if (!block.Supports(policy)) return Status::InvalidIr;
	const bool packed256_enabled = Packed256Requested();
	if (ContainsPacked256Instruction(block.Block()) && !packed256_enabled)
		return Status::InvalidIr;
	if (!IsHostSupported()) return Status::UnsupportedHost;

	m_ir = block.Block();
	// Keep the verified path allocation/publication identical to the raw
	// compiler. Only duplicate semantic scans are removed.
	constexpr size_t staging_capacity = 16384;
	auto staging = std::unique_ptr<uint8_t[]>(
	    new (std::nothrow) uint8_t[staging_capacity]);
	if (!staging) return Status::AllocationFailed;
	size_t encoded_size = 0;
	uint8_t direct_instruction_count = 0;
	GuestCodeWriter writer(staging.get(), staging_capacity, CompactImmediateRequested());
	Status status = EmitGuestBlock(
	    m_ir, &writer, &direct_instruction_count,
	    block.IsDirectFallthrough(), block.IsDirectBasicBlock(),
	    BatchDirectRetirementRequested(), DirectLeafAbiRequested());
	if (status != Status::Ok) return status;
	encoded_size = writer.Size();
	status = m_code.Allocate(encoded_size);
	if (status != Status::Ok) return status;
	status = m_code.Publish(staging.get(), encoded_size);
	if (status != Status::Ok)
	{
		m_code.Reset();
		return status;
	}
	m_direct_instruction_count = direct_instruction_count;
	m_direct_fallthrough = block.IsDirectFallthrough();
	m_direct_basic_block = block.IsDirectBasicBlock();
	m_production_scalar_memory_fallthrough =
	    block.IsProductionScalarMemoryFallthrough();
	return Status::Ok;
}

Status DenseResidentBlock::CompileVerified(
	const VerifiedIrBlock& block, EligibilityPolicy policy)
{
#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
	return CompileVerified(block, policy, false, nullptr, nullptr, nullptr);
#else
	return CompileVerified(block, policy, false, nullptr, nullptr);
#endif
}

#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
Status DenseResidentBlock::CompileVerified(
	const VerifiedIrBlock& block, EligibilityPolicy policy,
	bool contextless_direct, DenseResidentCapacityReclaimer reclaimer,
	void* reclaimer_context, DenseResidentCompileTiming* timing)
#else
Status DenseResidentBlock::CompileVerified(
	const VerifiedIrBlock& block, EligibilityPolicy policy,
	bool contextless_direct, DenseResidentCapacityReclaimer reclaimer,
	void* reclaimer_context)
#endif
{
#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
	if (timing != nullptr) *timing = {};
#endif
	if (m_code.IsPublished()) return Status::AlreadyPublished;
	if (!DenseResidentCacheRequested() || !block.Supports(policy) ||
	    !block.IsDirectBasicBlock())
	{
		return Status::InvalidIr;
	}
	if (ContainsPacked256Instruction(block.Block()) && !Packed256Requested())
		return Status::InvalidIr;
	if (!IsHostSupported()) return Status::UnsupportedHost;

#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
	const auto staging_start = timing != nullptr
	                               ? std::chrono::steady_clock::now()
	                               : std::chrono::steady_clock::time_point {};
#endif
	constexpr size_t staging_capacity = 16384;
	auto staging = std::unique_ptr<uint8_t[]>(
	    new (std::nothrow) uint8_t[staging_capacity]);
	if (!staging) return Status::AllocationFailed;
#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
	if (timing != nullptr)
	{
		timing->staging_ns = static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(
		        std::chrono::steady_clock::now() - staging_start)
		        .count());
	}
#endif
	GuestCodeWriter writer(
	    staging.get(), staging_capacity, CompactImmediateRequested());
	uint8_t direct_instruction_count = 0;
#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
	const auto emit_start = timing != nullptr
	                            ? std::chrono::steady_clock::now()
	                            : std::chrono::steady_clock::time_point {};
#endif
	const Status emit = EmitGuestBlock(
	    block.Block(), &writer, &direct_instruction_count,
	    block.IsDirectFallthrough(), true,
	    BatchDirectRetirementRequested(), DirectLeafAbiRequested(), nullptr,
	    contextless_direct);
#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
	if (timing != nullptr)
	{
		timing->emit_ns = static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(
		        std::chrono::steady_clock::now() - emit_start)
		        .count());
	}
#endif
	if (emit != Status::Ok) return emit;
#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
	const auto allocate_start = timing != nullptr
	                                ? std::chrono::steady_clock::now()
	                                : std::chrono::steady_clock::time_point {};
#endif
	Status status = m_code.AllocateDenseResident(writer.Size());
	if (status != Status::Ok)
	{
		if (status == Status::CapacityExceeded && reclaimer != nullptr)
		{
#if LUMEN_A64_DYNAREC_MACOS
			// Keep the emitted staging buffer alive while phase-6CF releases one
			// compatible archived slice. A successful callback retries only arena
			// allocation/publication; it never lowers or emits the block twice.
			const size_t required_code_capacity = DenseResidentSlotSize(
			    writer.Size(), JitPageRecyclePageSize());
			if (required_code_capacity != 0 &&
			    reclaimer(reclaimer_context, required_code_capacity))
			{
				status = m_code.AllocateDenseResident(writer.Size());
			}
#else
			(void)reclaimer_context;
#endif
		}
		if (status != Status::Ok)
		{
#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
			if (timing != nullptr)
			{
				timing->allocate_ns = static_cast<uint64_t>(
				    std::chrono::duration_cast<std::chrono::nanoseconds>(
				        std::chrono::steady_clock::now() - allocate_start)
				        .count());
			}
#endif
			return status;
		}
	}
#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
	if (timing != nullptr)
	{
		timing->allocate_ns = static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(
		        std::chrono::steady_clock::now() - allocate_start)
		        .count());
	}
	const auto publish_start = timing != nullptr
	                               ? std::chrono::steady_clock::now()
	                               : std::chrono::steady_clock::time_point {};
#endif
	status = m_code.Publish(staging.get(), writer.Size());
#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
	if (timing != nullptr)
	{
		timing->publish_ns = static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(
		        std::chrono::steady_clock::now() - publish_start)
		        .count());
	}
#endif
	if (status != Status::Ok)
	{
		m_code.Reset();
		return status;
	}
	m_rip = block.Block().rip;
	m_fallthrough_rip = block.Block().fallthrough_rip;
	m_instruction_count = block.Block().instruction_count;
	m_direct_fallthrough = block.IsDirectFallthrough();
	m_contextless_direct = contextless_direct;
#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
	if (timing != nullptr)
	{
		timing->code_size = m_code.PublishedSize();
		timing->code_capacity = m_code.Capacity();
	}
#endif
	return Status::Ok;
}

uint32_t DenseResidentBlock::ExecutePreparedDirect(
	EntryContext* context) const
{
	if (context == nullptr || m_contextless_direct || !m_code.IsPublished() ||
	    context->state == nullptr ||
	    context->runtime_context != nullptr || context->resume_rip != m_rip ||
	    context->executed_steps != 0 || context->direct_steps != 0 ||
	    context->exit_reason != ExitReason::Bailout ||
	    context->call_stack_transaction_conflict != 0)
	{
		return 0;
	}
	if (context->cache_epoch != m_code.Epoch())
	{
		context->exit_reason = ExitReason::Invalidated;
		return 0;
	}
	if (m_instruction_count > context->remaining_steps)
	{
		context->exit_reason = ExitReason::StepLimit;
		return 0;
	}
	const auto entry = reinterpret_cast<EntryPoint>(
	    const_cast<void*>(m_code.EntryAddress()));
	if (entry == nullptr) return 0;
	const uint32_t returned_steps = entry(context);
	if (returned_steps != context->executed_steps)
	{
		context->exit_reason = ExitReason::Bailout;
		return (std::max)(returned_steps, context->executed_steps);
	}
	return context->executed_steps;
}

uint32_t DenseResidentBlock::ExecutePreparedDirectKnownValid(
	EntryContext* context) const
{
	if (context == nullptr || m_contextless_direct) return 0;
	const auto entry = reinterpret_cast<EntryPoint>(
	    const_cast<void*>(m_code.EntryAddress()));
	if (entry == nullptr) return 0;
	const uint32_t returned_steps = entry(context);
	if (returned_steps != context->executed_steps)
	{
		context->exit_reason = ExitReason::Bailout;
		return (std::max)(returned_steps, context->executed_steps);
	}
	return context->executed_steps;
}

uint32_t DenseResidentBlock::ExecuteContextlessDirect(
	X64State* state, uint64_t block_end) const
{
	if (!m_contextless_direct || state == nullptr || !m_code.IsPublished() ||
	    state->rip != m_rip || block_end != m_fallthrough_rip ||
	    m_instruction_count == 0)
	{
		return 0;
	}
	const auto entry = reinterpret_cast<DenseContextlessEntryPoint>(
	    const_cast<void*>(m_code.EntryAddress()));
	return entry != nullptr ? entry(state, block_end) : 0;
}

uint64_t DenseResidentBlock::Epoch() const
{
	return m_code.Epoch();
}

const void* DenseResidentBlock::CodeAddress() const
{
	return m_code.EntryAddress();
}

size_t DenseResidentBlock::CodeSize() const
{
	return m_code.PublishedSize();
}

size_t DenseResidentBlock::CodeCapacity() const
{
	return m_code.Capacity();
}

bool DenseResidentBlock::IsDirectFallthrough() const
{
	return m_direct_fallthrough;
}

bool DenseResidentBlock::IsContextlessDirect() const
{
	return m_contextless_direct;
}

bool DenseResidentBlock::IsCompiled() const
{
	return m_code.IsPublished();
}

bool DenseResidentBlock::MatchesMetadata(
	uint64_t rip, uint64_t fallthrough_rip, uint8_t instruction_count,
	bool direct_fallthrough, bool contextless_direct) const
{
	return IsCompiled() && m_rip == rip &&
	       m_fallthrough_rip == fallthrough_rip &&
	       m_instruction_count == instruction_count &&
	       m_direct_fallthrough == direct_fallthrough &&
	       m_contextless_direct == contextless_direct;
}

Status CompiledBlock::CompileMultiSegment(const MultiSegmentArtifactSpec& spec)
{
	if (m_code.IsPublished()) return Status::AlreadyPublished;
	if (!MultiSegmentArtifactRequested() || spec.segment_count < 2 ||
	    spec.segment_count > MULTI_SEGMENT_MAX_SEGMENTS || !IsHostSupported())
	{
		return Status::UnsupportedOperation;
	}
	IrBlock merged {};
	size_t merged_count = 0;
	uint64_t page_guest_begin = 0;
	for (size_t segment = 0; segment < spec.segment_count; segment++)
	{
		const auto& source = spec.segments[segment];
		if (source.byte_count == 0 ||
		    source.byte_count > MULTI_SEGMENT_MAX_SIGNATURE_BYTES ||
		    source.page_offset >= MULTI_SEGMENT_PAGE_SIZE ||
		    source.byte_count > MULTI_SEGMENT_PAGE_SIZE - source.page_offset ||
		    VerifyIrBlock(source.block) != Status::Ok ||
		    (segment + 1u < spec.segment_count
		         ? !HasDirectFallthroughEligibility(source.block)
		         : !HasDirectBasicBlockEligibility(source.block)) ||
		    source.block.instruction_count == 0 ||
		    merged_count > MAX_IR_INSTRUCTIONS - source.block.instruction_count)
		{
			return Status::InvalidIr;
		}
		const uint64_t source_page = source.block.rip - source.page_offset;
		if ((source.block.rip & (MULTI_SEGMENT_PAGE_SIZE - 1u)) !=
		        source.page_offset ||
		    (segment != 0 && source_page != page_guest_begin) ||
		    (segment != 0 &&
		     spec.segments[segment - 1u].block.fallthrough_rip !=
		         source.block.rip))
		{
			return Status::InvalidIr;
		}
		if (segment == 0)
		{
			page_guest_begin = source_page;
			merged.rip = source.block.rip;
		}
		for (size_t instruction = 0;
		     instruction < source.block.instruction_count; instruction++)
		{
			merged.instructions[merged_count++] =
			    source.block.instructions[instruction];
		}
		merged.semantic_mask |= source.block.semantic_mask;
	}
	const auto& final = spec.segments[spec.segment_count - 1u].block;
	merged.instruction_count = static_cast<uint8_t>(merged_count);
	merged.fallthrough_rip = final.fallthrough_rip;
	merged.has_terminator = final.has_terminator;
	if (VerifyIrBlock(merged) != Status::Ok ||
	    !HasDirectBasicBlockEligibility(merged))
	{
		return Status::InvalidIr;
	}

	constexpr size_t staging_capacity = 16384;
	auto staging = std::unique_ptr<uint8_t[]>(
	    new (std::nothrow) uint8_t[staging_capacity]);
	if (!staging) return Status::AllocationFailed;
	GuestCodeWriter writer(
	    staging.get(), staging_capacity, CompactImmediateRequested());
	uint8_t direct_instruction_count = 0;
	const Status emit = EmitGuestBlock(
	    merged, &writer, &direct_instruction_count,
	    HasDirectFallthroughEligibility(final), true,
	    BatchDirectRetirementRequested(), DirectLeafAbiRequested(), &spec);
	if (emit != Status::Ok) return emit;
	Status status = m_code.Allocate(writer.Size());
	if (status != Status::Ok) return status;
	status = m_code.Publish(staging.get(), writer.Size());
	if (status != Status::Ok)
	{
		m_code.Reset();
		return status;
	}
	m_ir = merged;
	m_direct_instruction_count = direct_instruction_count;
	m_direct_fallthrough = HasDirectFallthroughEligibility(final);
	m_direct_basic_block = true;
	m_production_scalar_memory_fallthrough = false;
	m_segment_count = spec.segment_count;
	m_segment_instruction_counts = {};
	m_segment_byte_counts = {};
	for (size_t segment = 0; segment < spec.segment_count; segment++)
	{
		m_segment_instruction_counts[segment] =
		    spec.segments[segment].block.instruction_count;
		m_segment_byte_counts[segment] = spec.segments[segment].byte_count;
	}
	return Status::Ok;
}

#if defined(LUMEN_CPU_X64_TEST)
Status CompiledBlock::CompileWithMinimumCapacityForTest(
	const IrBlock& block, EligibilityPolicy policy,
	size_t minimum_capacity)
{
	return CompileWithMinimumCapacity(block, policy, minimum_capacity);
}
#endif

Status CompiledBlock::CompileWithMinimumCapacity(
	const IrBlock& block, EligibilityPolicy policy,
	size_t minimum_capacity)
{
	if (m_code.IsPublished()) return Status::AlreadyPublished;
	if (!IsRuntimeEligible(block, policy)) return Status::InvalidIr;
	if (!IsHostSupported()) return Status::UnsupportedHost;

	m_ir = block;
	// Sixteen scalar-memory nodes include two bounded region probes plus cold
	// resolver and generic fallback islands. Keep their staging independent of
	// both the host page size and the deliberately small guest-thread stack.
	constexpr size_t staging_capacity = 16384;
	auto staging = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[staging_capacity]);
	if (!staging) return Status::AllocationFailed;
	size_t encoded_size = 0;
	uint8_t direct_instruction_count = 0;
	Status status = EncodeIrBlock(m_ir, staging.get(), staging_capacity, &encoded_size,
	                              &direct_instruction_count, policy);
	if (status != Status::Ok) return status;
	status = m_code.Allocate((std::max)(encoded_size, minimum_capacity));
	if (status != Status::Ok) return status;
	status = m_code.Publish(staging.get(), encoded_size);
	if (status != Status::Ok)
	{
		m_code.Reset();
		return status;
	}
	m_direct_instruction_count = direct_instruction_count;
	m_direct_fallthrough = HasDirectFallthroughEligibility(m_ir);
	m_direct_basic_block = HasDirectBasicBlockEligibility(m_ir);
	m_production_scalar_memory_fallthrough =
	    HasProductionScalarMemoryFallthroughEligibility(m_ir);
	return Status::Ok;
}

#if defined(__ANDROID__) && defined(__aarch64__) && \
    defined(LUMEN_ANDROID_DYNAREC_POLICY_SELECTIVE_NODA)
namespace {

constexpr int ANDROID_NODA_TIMEOUT_SIGNAL = SIGURG;
constexpr uint64_t ANDROID_NODA_ENTRY_TIMEOUT_NS = UINT64_C(10000000);
constexpr uint64_t ANDROID_NODA_SELF_TEST_TIMEOUT_NS = UINT64_C(20000000);
constexpr uint64_t ANDROID_NODA_SELF_TEST_BUSY_BOUND_NS =
    UINT64_C(250000000);
constexpr uint64_t ANDROID_NODA_CONCURRENT_TIMEOUT_NS = UINT64_C(100000000);
constexpr size_t ANDROID_NODA_ALT_STACK_SIZE = 64u * 1024u;
constexpr size_t ANDROID_NODA_SIGNAL_SLOT_COUNT = 128;
constexpr std::array<int, 5> ANDROID_NODA_SIGNALS = {
	SIGILL, SIGSEGV, SIGBUS, SIGFPE, ANDROID_NODA_TIMEOUT_SIGNAL};

using AndroidNodaIsolatedCallback = void (*)(void* opaque);

// This record is deliberately static BSS, not C++ TLS. Android API 26 lowers
// thread_local access through __emutls_get_address, which is not signal-safe.
// The handler performs only lock-free loads after finding its pre-published TID.
struct AndroidNodaSignalSlot
{
	std::atomic<int32_t> thread_id {0};
	sigjmp_buf jump {};
	std::atomic<int32_t> active {0};
	std::atomic<int32_t> signal_number {0};
	std::atomic<int32_t> allow_test_signal {0};
	std::atomic<uintptr_t> code_begin {0};
	std::atomic<uintptr_t> code_end {0};
};

static_assert(std::atomic<int32_t>::is_always_lock_free);
static_assert(std::atomic<uintptr_t>::is_always_lock_free);
static_assert(sizeof(pid_t) <= sizeof(int32_t));

struct AndroidNodaThreadState
{
	~AndroidNodaThreadState();

	AndroidNodaSignalSlot* signal_slot = nullptr;
	timer_t timer {};
	bool timer_created = false;
	bool alternate_stack_ready = false;
	bool installed_alternate_stack = false;
	void* alternate_stack_pointer = nullptr;
	size_t alternate_stack_size = 0;
	stack_t previous_alternate_stack {};
	alignas(16) std::array<uint8_t, ANDROID_NODA_ALT_STACK_SIZE>
	    alternate_stack {};
};

std::mutex g_android_noda_isolation_mutex;
std::atomic<bool> g_android_noda_isolation_initialized {false};
std::atomic<bool> g_android_noda_isolation_validated {false};
std::atomic<bool> g_android_noda_isolation_healthy {false};
bool g_android_noda_initialization_attempted = false;
bool g_android_noda_validation_attempted = false;
std::array<struct sigaction, ANDROID_NODA_SIGNALS.size()>
    g_android_noda_previous_actions {};
std::array<AndroidNodaSignalSlot, ANDROID_NODA_SIGNAL_SLOT_COUNT>
    g_android_noda_signal_slots {};
thread_local AndroidNodaThreadState g_android_noda_thread_state {};

bool AndroidNodaPreparationFailure(const char* stage, int error_code,
	                               int stack_flags = 0,
	                               size_t stack_size = 0)
{
	__android_log_print(
	    ANDROID_LOG_ERROR, "LumenAndroidRuntime",
	    "LUMEN_ANDROID_NODA_PREPARE_V1 FAIL stage=%s error=%d "
	    "stackFlags=%d stackSize=%zu",
	    stage, error_code, stack_flags, stack_size);
	return false;
}

bool AndroidNodaInitializationFailure(const char* stage, int signal_number,
	                                  int error_code,
	                                  const struct sigaction* action = nullptr)
{
	int disposition = 0;
	int flags = 0;
	if (action != nullptr)
	{
		flags = action->sa_flags;
		if (action->sa_handler == SIG_IGN)
			disposition = 1;
		else if (action->sa_handler != SIG_DFL)
			disposition = (action->sa_flags & SA_SIGINFO) != 0 ? 2 : 3;
	}
	__android_log_print(
	    ANDROID_LOG_ERROR, "LumenAndroidRuntime",
	    "LUMEN_ANDROID_NODA_INIT_V1 FAIL stage=%s signal=%d error=%d "
	    "disposition=%d flags=%d",
	    stage, signal_number, error_code, disposition, flags);
	return false;
}

bool AndroidNodaSelfTestFailure(const char* stage,
	                            AndroidNodaExecutionStatus status,
	                            int64_t detail_a = 0,
	                            int64_t detail_b = 0)
{
	__android_log_print(
	    ANDROID_LOG_ERROR, "LumenAndroidRuntime",
	    "LUMEN_ANDROID_NODA_SELF_TEST_V1 FAIL stage=%s status=%u "
	    "detailA=%lld detailB=%lld",
	    stage, static_cast<unsigned>(status),
	    static_cast<long long>(detail_a), static_cast<long long>(detail_b));
	return false;
}

bool AndroidNodaSetRawSignalMask(
	const sigset_t* requested_mask, sigset_t* previous_mask)
{
	static_assert(sizeof(sigset_t) == sizeof(uint64_t),
	              "Android arm64 kernel signal mask must be 64-bit");
	sigset_t filtered_mask {};
	const sigset_t* kernel_mask = requested_mask;
	if (requested_mask != nullptr)
	{
		filtered_mask = *requested_mask;
		// Mirror Bionic filter_reserved_signals(SIG_SETMASK): its POSIX timer
		// signal 32 must stay blocked, while the other runtime signals 33..41
		// must stay unblocked.
		sigaddset(&filtered_mask, 32);
		for (int reserved_signal = 33; reserved_signal <= 41;
		     reserved_signal++)
		{
			sigdelset(&filtered_mask, reserved_signal);
		}
		kernel_mask = &filtered_mask;
	}
	return syscall(SYS_rt_sigprocmask, SIG_SETMASK, kernel_mask,
	               previous_mask, sizeof(uint64_t)) == 0;
}

[[noreturn]] void AndroidNodaTerminalTeardownFailure(
	const char* stage, int error_code)
{
	__android_log_print(
	    ANDROID_LOG_FATAL, "LumenAndroidRuntime",
	    "LUMEN_ANDROID_NODA_TEARDOWN_V1 FATAL stage=%s error=%d",
	    stage, error_code);
	_exit(127);
}

void RestoreAndroidNodaRawSignalMaskOrExit(
	const sigset_t& previous_mask, const char* stage)
{
	if (!AndroidNodaSetRawSignalMask(&previous_mask, nullptr))
	{
		const int error_code = errno;
		AndroidNodaTerminalTeardownFailure(stage, error_code);
	}
}

void AndroidNodaSignalHandler(
	int signal_number, siginfo_t* signal_info, void* raw_context)
{
	const int interrupted_errno = errno;
	const int32_t thread_id = static_cast<int32_t>(syscall(SYS_gettid));
	AndroidNodaSignalSlot* slot = nullptr;
	for (auto& candidate : g_android_noda_signal_slots)
	{
		if (candidate.thread_id.load(std::memory_order_acquire) == thread_id)
		{
			slot = &candidate;
			break;
		}
	}
	const bool active =
	    slot != nullptr &&
	    slot->active.load(std::memory_order_acquire) != 0;
	bool contained = signal_number == ANDROID_NODA_TIMEOUT_SIGNAL;
	if (active)
	{
		contained = contained ||
		            slot->allow_test_signal.load(std::memory_order_relaxed) != 0;
	}
	const uintptr_t code_begin =
	    slot != nullptr
	        ? slot->code_begin.load(std::memory_order_relaxed)
	        : 0;
	const uintptr_t code_end =
	    slot != nullptr ? slot->code_end.load(std::memory_order_relaxed) : 0;
	if (!contained && active && raw_context != nullptr && code_begin != 0 &&
	    code_end > code_begin)
	{
		const auto* context = static_cast<const ucontext_t*>(raw_context);
		const uintptr_t pc = static_cast<uintptr_t>(context->uc_mcontext.pc);
		contained = pc >= code_begin && pc < code_end;
	}
	if (active && contained)
	{
		slot->signal_number.store(signal_number, std::memory_order_relaxed);
		errno = interrupted_errno;
		siglongjmp(slot->jump, 1);
	}

	// SIGURG is owned solely as the per-thread timeout interrupt. A queued
	// timeout after disarm is harmless when no isolated entry is active.
	if (signal_number == ANDROID_NODA_TIMEOUT_SIGNAL)
	{
		errno = interrupted_errno;
		return;
	}

	// Android installs its own SA_SIGINFO crash reporter for the synchronous
	// fatal set. Direct-chain its immutable initialization snapshot and the
	// original siginfo/ucontext outside our exact
	// containment window so the platform retains the real fault address, code
	// and PC. This path never writes a signal disposition, hence it cannot
	// overwrite a later owner while forwarding an already in-flight signal.
	const struct sigaction* previous_action = nullptr;
	for (size_t index = 0; index < ANDROID_NODA_SIGNALS.size(); index++)
	{
		if (ANDROID_NODA_SIGNALS[index] == signal_number)
		{
			previous_action = &g_android_noda_previous_actions[index];
			break;
		}
	}
	struct sigaction target_action {};
	if (previous_action != nullptr) target_action = *previous_action;
	if (target_action.sa_handler == SIG_IGN)
	{
		errno = interrupted_errno;
		return;
	}
	if (target_action.sa_handler == SIG_DFL ||
	    target_action.sa_sigaction == AndroidNodaSignalHandler)
	{
		_exit(128 + signal_number);
	}

	// Recreate the target action's mask rather than calling it under the wider
	// containment mask. uc_sigmask is the mask from before this delivery.
	sigset_t target_mask {};
	if (raw_context != nullptr)
		target_mask = static_cast<const ucontext_t*>(raw_context)->uc_sigmask;
	else if (!AndroidNodaSetRawSignalMask(nullptr, &target_mask))
		_exit(128 + signal_number);
	for (int candidate = 1; candidate < NSIG; candidate++)
	{
		if (sigismember(&target_action.sa_mask, candidate) == 1)
			sigaddset(&target_mask, candidate);
	}
	if ((target_action.sa_flags & SA_NODEFER) == 0)
		sigaddset(&target_mask, signal_number);
	sigset_t containment_mask {};
	// ART's libsigchain interposes sigprocmask() in the app process. Use the
	// kernel operation directly so the chained handler receives its saved
	// sa_mask and implicit current-signal blocking without changing
	// dispositions.
	if (!AndroidNodaSetRawSignalMask(&target_mask, &containment_mask))
		_exit(128 + signal_number);
	if ((target_action.sa_flags & SA_SIGINFO) != 0)
		target_action.sa_sigaction(signal_number, signal_info, raw_context);
	else
		target_action.sa_handler(signal_number);
	if (!AndroidNodaSetRawSignalMask(&containment_mask, nullptr))
		_exit(128 + signal_number);
	errno = interrupted_errno;
}

bool IsAndroidNodaSignalAction(const struct sigaction& action)
{
	if ((action.sa_flags & (SA_SIGINFO | SA_ONSTACK)) !=
	    (SA_SIGINFO | SA_ONSTACK) ||
	    action.sa_sigaction != AndroidNodaSignalHandler)
	{
		return false;
	}
	for (const int signal_number : ANDROID_NODA_SIGNALS)
	{
		if (sigismember(&action.sa_mask, signal_number) != 1) return false;
	}
	return true;
}

bool VerifyAndroidNodaSignalOwnershipInternal()
{
	if (!g_android_noda_isolation_initialized.load(
	        std::memory_order_acquire) ||
	    !g_android_noda_isolation_healthy.load(std::memory_order_acquire))
	{
		return false;
	}
	for (const int signal_number : ANDROID_NODA_SIGNALS)
	{
		struct sigaction current {};
		if (sigaction(signal_number, nullptr, &current) != 0 ||
		    !IsAndroidNodaSignalAction(current))
		{
			g_android_noda_isolation_healthy.store(
			    false, std::memory_order_release);
			g_android_noda_isolation_validated.store(
			    false, std::memory_order_release);
			return false;
		}
	}
	return true;
}

bool VerifyAndroidNodaThreadStack()
{
	auto& thread = g_android_noda_thread_state;
	const size_t minimum_stack_size = thread.installed_alternate_stack
	                                      ? ANDROID_NODA_ALT_STACK_SIZE
	                                      : static_cast<size_t>(MINSIGSTKSZ);
	if (!thread.alternate_stack_ready ||
	    thread.alternate_stack_pointer == nullptr ||
	    thread.alternate_stack_size < minimum_stack_size)
	{
		return false;
	}
	stack_t current {};
	int unsafe_flags = SS_DISABLE | SS_ONSTACK;
#if defined(SS_AUTODISARM)
	unsafe_flags |= SS_AUTODISARM;
#endif
	return sigaltstack(nullptr, &current) == 0 &&
	       (current.ss_flags & unsafe_flags) == 0 &&
	       current.ss_sp == thread.alternate_stack_pointer &&
	       current.ss_size == thread.alternate_stack_size;
}

bool DrainAndroidNodaTimeoutSignal()
{
	sigset_t timeout_set;
	sigemptyset(&timeout_set);
	sigaddset(&timeout_set, ANDROID_NODA_TIMEOUT_SIGNAL);
	const struct timespec no_wait {0, 0};
	for (;;)
	{
		if (sigtimedwait(&timeout_set, nullptr, &no_wait) >= 0) continue;
		if (errno == EINTR) continue;
		return errno == EAGAIN;
	}
}

bool PrepareAndroidNodaSignalSlot()
{
	auto& thread = g_android_noda_thread_state;
	const int32_t thread_id = static_cast<int32_t>(syscall(SYS_gettid));
	if (thread_id <= 0) return false;
	if (thread.signal_slot != nullptr)
	{
		return thread.signal_slot->thread_id.load(std::memory_order_acquire) ==
		       thread_id;
	}
	for (auto& candidate : g_android_noda_signal_slots)
	{
		if (candidate.thread_id.load(std::memory_order_acquire) == thread_id)
		{
			thread.signal_slot = &candidate;
			return true;
		}
	}
	for (auto& candidate : g_android_noda_signal_slots)
	{
		int32_t expected = 0;
		if (!candidate.thread_id.compare_exchange_strong(
		        expected, thread_id, std::memory_order_acq_rel,
		        std::memory_order_acquire))
		{
			continue;
		}
		thread.signal_slot = &candidate;
		return true;
	}
	return false;
}

AndroidNodaThreadState::~AndroidNodaThreadState()
{
	// Publish a closed window before touching timer or alt-stack lifetime. The
	// handler may still observe this slot until thread_id is released last.
	if (signal_slot != nullptr)
	{
		signal_slot->active.store(0, std::memory_order_release);
		signal_slot->signal_number.store(0, std::memory_order_relaxed);
		signal_slot->allow_test_signal.store(0, std::memory_order_relaxed);
		signal_slot->code_begin.store(0, std::memory_order_relaxed);
		signal_slot->code_end.store(0, std::memory_order_relaxed);
	}

	sigset_t isolation_mask {};
	sigset_t previous_mask {};
	bool blocked = AndroidNodaSetRawSignalMask(nullptr, &previous_mask);
	if (blocked)
	{
		isolation_mask = previous_mask;
		for (const int signal_number : ANDROID_NODA_SIGNALS)
		{
			if (sigaddset(&isolation_mask, signal_number) != 0)
			{
				blocked = false;
				break;
			}
		}
	}
	if (blocked)
		blocked = AndroidNodaSetRawSignalMask(&isolation_mask, nullptr);
	if (!blocked) _exit(127);
	if (timer_created)
	{
		const struct itimerspec disarmed {};
		if (timer_settime(timer, 0, &disarmed, nullptr) != 0 ||
		    timer_delete(timer) != 0)
		{
			_exit(127);
		}
		timer_created = false;
		if (!DrainAndroidNodaTimeoutSignal()) _exit(127);
	}
	if (installed_alternate_stack)
	{
		stack_t current {};
		int unsafe_flags = SS_DISABLE | SS_ONSTACK;
#if defined(SS_AUTODISARM)
		unsafe_flags |= SS_AUTODISARM;
#endif
		if (sigaltstack(nullptr, &current) != 0 ||
		    (current.ss_flags & unsafe_flags) != 0 ||
		    current.ss_sp != alternate_stack_pointer ||
		    current.ss_size != alternate_stack_size ||
		    sigaltstack(&previous_alternate_stack, nullptr) != 0)
		{
			_exit(127);
		}
		installed_alternate_stack = false;
	}
	if (signal_slot != nullptr)
	{
		signal_slot->thread_id.store(0, std::memory_order_release);
		signal_slot = nullptr;
	}
	if (!AndroidNodaSetRawSignalMask(&previous_mask, nullptr)) _exit(127);
}

bool PrepareAndroidNodaThread()
{
	auto& thread = g_android_noda_thread_state;
	if (!PrepareAndroidNodaSignalSlot())
		return AndroidNodaPreparationFailure("signal-slot", 0);
	if (!thread.alternate_stack_ready)
	{
		stack_t current {};
		if (sigaltstack(nullptr, &current) != 0)
			return AndroidNodaPreparationFailure("altstack-query", errno);
		int unsafe_flags = SS_ONSTACK;
#if defined(SS_AUTODISARM)
		unsafe_flags |= SS_AUTODISARM;
#endif
		if ((current.ss_flags & unsafe_flags) != 0)
			return AndroidNodaPreparationFailure(
			    "altstack-unsafe", 0, current.ss_flags, current.ss_size);
		if ((current.ss_flags & SS_DISABLE) != 0)
		{
			thread.previous_alternate_stack = current;
			stack_t replacement {};
			replacement.ss_sp = thread.alternate_stack.data();
			replacement.ss_size = thread.alternate_stack.size();
			if (sigaltstack(&replacement, nullptr) != 0)
				return AndroidNodaPreparationFailure(
				    "altstack-install", errno, current.ss_flags,
				    current.ss_size);
			thread.installed_alternate_stack = true;
		}
		int active_unsafe_flags = SS_DISABLE | SS_ONSTACK;
#if defined(SS_AUTODISARM)
		active_unsafe_flags |= SS_AUTODISARM;
#endif
		if (sigaltstack(nullptr, &current) != 0 ||
		    (current.ss_flags & active_unsafe_flags) != 0 ||
		    current.ss_sp == nullptr ||
		    current.ss_size <
		        (thread.installed_alternate_stack
		             ? ANDROID_NODA_ALT_STACK_SIZE
		             : static_cast<size_t>(MINSIGSTKSZ)))
		{
			return AndroidNodaPreparationFailure(
			    "altstack-active", errno, current.ss_flags,
			    current.ss_size);
		}
		thread.alternate_stack_pointer = current.ss_sp;
		thread.alternate_stack_size = current.ss_size;
		thread.alternate_stack_ready = true;
	}
	if (!VerifyAndroidNodaThreadStack())
		return AndroidNodaPreparationFailure(
		    "altstack-verify", 0, 0, thread.alternate_stack_size);

	if (!thread.timer_created)
	{
		clockid_t thread_clock {};
		const int clock_result =
		    pthread_getcpuclockid(pthread_self(), &thread_clock);
		if (clock_result != 0)
			return AndroidNodaPreparationFailure(
			    "thread-cpu-clock", clock_result);
		struct sigevent event {};
		event.sigev_notify = SIGEV_THREAD_ID;
		event.sigev_signo = ANDROID_NODA_TIMEOUT_SIGNAL;
		event.sigev_notify_thread_id =
		    static_cast<pid_t>(syscall(SYS_gettid));
		if (timer_create(thread_clock, &event, &thread.timer) != 0)
			return AndroidNodaPreparationFailure("timer-create", errno);
		thread.timer_created = true;
	}
	return true;
}

bool ReleaseAndroidNodaThreadTimer()
{
	auto& thread = g_android_noda_thread_state;
	if (!thread.timer_created) return true;
	if (thread.signal_slot != nullptr &&
	    thread.signal_slot->active.load(std::memory_order_acquire) != 0)
	{
		g_android_noda_isolation_healthy.store(false,
		                                        std::memory_order_release);
		return AndroidNodaPreparationFailure("timer-release-active", 0);
	}

	sigset_t previous_mask {};
	sigset_t isolation_mask {};
	if (!AndroidNodaSetRawSignalMask(nullptr, &previous_mask))
	{
		g_android_noda_isolation_healthy.store(false,
		                                        std::memory_order_release);
		return AndroidNodaPreparationFailure("timer-release-mask-query",
		                                     errno);
	}
	isolation_mask = previous_mask;
	for (const int signal_number : ANDROID_NODA_SIGNALS)
	{
		if (sigaddset(&isolation_mask, signal_number) != 0)
		{
			g_android_noda_isolation_healthy.store(
			    false, std::memory_order_release);
			return AndroidNodaPreparationFailure(
			    "timer-release-mask-build", errno);
		}
	}
	if (!AndroidNodaSetRawSignalMask(&isolation_mask, nullptr))
	{
		g_android_noda_isolation_healthy.store(false,
		                                        std::memory_order_release);
		return AndroidNodaPreparationFailure("timer-release-mask-block",
		                                     errno);
	}

	const struct itimerspec disarmed {};
	const bool timer_disarmed = timer_settime(
	                                thread.timer, 0, &disarmed, nullptr) == 0;
	const int disarm_error = timer_disarmed ? 0 : errno;
	const bool timer_deleted = timer_delete(thread.timer) == 0;
	const int delete_error = timer_deleted ? 0 : errno;
	if (timer_deleted)
	{
		thread.timer = timer_t {};
		thread.timer_created = false;
	}
	const bool timeout_drained = DrainAndroidNodaTimeoutSignal();
	const int drain_error = timeout_drained ? 0 : errno;
	RestoreAndroidNodaRawSignalMaskOrExit(
	    previous_mask, "timer-release-mask-restore");

	if (!timer_disarmed || !timer_deleted || !timeout_drained)
	{
		g_android_noda_isolation_healthy.store(false,
		                                        std::memory_order_release);
		if (!timer_disarmed)
			return AndroidNodaPreparationFailure(
			    "timer-release-disarm", disarm_error);
		if (!timer_deleted)
			return AndroidNodaPreparationFailure(
			    "timer-release-delete", delete_error);
		return AndroidNodaPreparationFailure(
		    "timer-release-drain", drain_error);
	}
	return true;
}

bool ArmAndroidNodaTimer(uint64_t timeout_ns)
{
	struct itimerspec timeout {};
	timeout.it_value.tv_sec = static_cast<time_t>(
	    timeout_ns / UINT64_C(1000000000));
	timeout.it_value.tv_nsec = static_cast<long>(
	    timeout_ns % UINT64_C(1000000000));
	return timer_settime(g_android_noda_thread_state.timer, 0, &timeout,
	                     nullptr) == 0;
}

bool DisarmAndroidNodaTimer()
{
	const struct itimerspec disarmed {};
	return timer_settime(g_android_noda_thread_state.timer, 0, &disarmed,
	                     nullptr) == 0;
}

void ClearAndroidNodaSignalWindow(AndroidNodaSignalSlot* slot)
{
	if (slot == nullptr) return;
	slot->signal_number.store(0, std::memory_order_relaxed);
	slot->allow_test_signal.store(0, std::memory_order_relaxed);
	slot->code_begin.store(0, std::memory_order_relaxed);
	slot->code_end.store(0, std::memory_order_relaxed);
}

AndroidNodaExecutionStatus RunAndroidNodaIsolated(
	AndroidNodaIsolatedCallback callback, void* opaque, uint64_t timeout_ns,
	int32_t* signal_number, uintptr_t code_begin, size_t code_size,
	bool test_window)
{
	if (signal_number != nullptr) *signal_number = 0;
	const bool valid_code_range =
	    code_begin != 0 && code_size != 0 &&
	    code_begin <= UINTPTR_MAX - code_size;
	if (callback == nullptr || timeout_ns == 0 ||
	    (!test_window && !valid_code_range) ||
	    !g_android_noda_isolation_initialized.load(std::memory_order_acquire) ||
	    !g_android_noda_isolation_healthy.load(std::memory_order_acquire) ||
	    !PrepareAndroidNodaThread() ||
	    !VerifyAndroidNodaSignalOwnershipInternal())
	{
		return AndroidNodaExecutionStatus::Unavailable;
	}

	auto& thread = g_android_noda_thread_state;
	auto* slot = thread.signal_slot;
	if (slot == nullptr ||
	    slot->active.load(std::memory_order_acquire) != 0)
	{
		return AndroidNodaExecutionStatus::Unavailable;
	}

	sigset_t previous_mask {};
	if (!AndroidNodaSetRawSignalMask(nullptr, &previous_mask))
		return AndroidNodaExecutionStatus::Unavailable;
	constexpr std::array<int, 4> fatal_signals = {
	    SIGILL, SIGSEGV, SIGBUS, SIGFPE};
	for (const int fatal_signal : fatal_signals)
	{
		if (sigismember(&previous_mask, fatal_signal) != 0)
			return AndroidNodaExecutionStatus::Unavailable;
	}

	sigset_t blocked_mask = previous_mask;
	sigset_t run_mask = previous_mask;
	if (sigaddset(&blocked_mask, ANDROID_NODA_TIMEOUT_SIGNAL) != 0 ||
	    sigdelset(&run_mask, ANDROID_NODA_TIMEOUT_SIGNAL) != 0 ||
	    !AndroidNodaSetRawSignalMask(&blocked_mask, nullptr))
	{
		return AndroidNodaExecutionStatus::Unavailable;
	}
	if (!DrainAndroidNodaTimeoutSignal())
	{
		RestoreAndroidNodaRawSignalMaskOrExit(
		    previous_mask, "entry-drain-mask-restore");
		return AndroidNodaExecutionStatus::Unavailable;
	}
	slot->signal_number.store(0, std::memory_order_relaxed);
	slot->allow_test_signal.store(test_window ? 1 : 0,
	                              std::memory_order_relaxed);
	slot->code_begin.store(valid_code_range ? code_begin : 0,
	                       std::memory_order_relaxed);
	slot->code_end.store(valid_code_range ? code_begin + code_size : 0,
	                     std::memory_order_relaxed);

	if (sigsetjmp(slot->jump, 1) == 0)
	{
		slot->active.store(1, std::memory_order_release);
		if (!ArmAndroidNodaTimer(timeout_ns))
		{
			slot->active.store(0, std::memory_order_release);
			ClearAndroidNodaSignalWindow(slot);
			RestoreAndroidNodaRawSignalMaskOrExit(
			    previous_mask, "entry-arm-mask-restore");
			return AndroidNodaExecutionStatus::Unavailable;
		}
		if (!AndroidNodaSetRawSignalMask(&run_mask, nullptr))
		{
			slot->active.store(0, std::memory_order_release);
			const bool disarmed = DisarmAndroidNodaTimer();
			ClearAndroidNodaSignalWindow(slot);
			RestoreAndroidNodaRawSignalMaskOrExit(
			    previous_mask, "entry-unblock-mask-restore");
			if (!disarmed)
				g_android_noda_isolation_healthy.store(
				    false, std::memory_order_release);
			return AndroidNodaExecutionStatus::Unavailable;
		}
		callback(opaque);
		if (!AndroidNodaSetRawSignalMask(&blocked_mask, nullptr))
		{
			slot->active.store(0, std::memory_order_release);
			const bool disarmed = DisarmAndroidNodaTimer();
			ClearAndroidNodaSignalWindow(slot);
			RestoreAndroidNodaRawSignalMaskOrExit(
			    previous_mask, "entry-reblock-mask-restore");
			if (!disarmed)
				g_android_noda_isolation_healthy.store(
				    false, std::memory_order_release);
			return AndroidNodaExecutionStatus::Unavailable;
		}
		const bool disarmed = DisarmAndroidNodaTimer();
		slot->active.store(0, std::memory_order_release);
		ClearAndroidNodaSignalWindow(slot);
		const bool drained = DrainAndroidNodaTimeoutSignal();
		RestoreAndroidNodaRawSignalMaskOrExit(
		    previous_mask, "entry-complete-mask-restore");
		if (!disarmed || !drained)
			g_android_noda_isolation_healthy.store(
			    false, std::memory_order_release);
		return disarmed && drained
		           ? AndroidNodaExecutionStatus::Completed
		           : AndroidNodaExecutionStatus::Unavailable;
	}

	// sigsetjmp saved the mask while SIGURG was blocked, so both timer disarm
	// and pending-signal drain are race-free after a fatal or timeout jump.
	const bool disarmed = DisarmAndroidNodaTimer();
	slot->active.store(0, std::memory_order_release);
	const int received =
	    slot->signal_number.load(std::memory_order_relaxed);
	ClearAndroidNodaSignalWindow(slot);
	const bool drained = DrainAndroidNodaTimeoutSignal();
	RestoreAndroidNodaRawSignalMaskOrExit(
	    previous_mask, "entry-fault-mask-restore");
	if (signal_number != nullptr) *signal_number = received;
	if (!disarmed || !drained)
	{
		g_android_noda_isolation_healthy.store(false,
		                                        std::memory_order_release);
		return AndroidNodaExecutionStatus::Unavailable;
	}
	return received == ANDROID_NODA_TIMEOUT_SIGNAL
	           ? AndroidNodaExecutionStatus::TimedOut
	           : AndroidNodaExecutionStatus::Faulted;
}

struct AndroidNodaSuccessPayload
{
	uint32_t calls = 0;
};

void AndroidNodaSuccessCallback(void* opaque)
{
	auto* payload = static_cast<AndroidNodaSuccessPayload*>(opaque);
	if (payload != nullptr) payload->calls++;
}

void AndroidNodaSignalCallback(void* opaque)
{
	const auto* signal_number = static_cast<const int*>(opaque);
	if (signal_number != nullptr) (void)raise(*signal_number);
}

void AndroidNodaBusyCallback(void*)
{
	struct timespec started {};
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &started) != 0) return;
	volatile uint64_t value = UINT64_C(0x9e3779b97f4a7c15);
	uint64_t rounds = 0;
	for (;; rounds++)
	{
		value ^= value << 7u;
		value ^= value >> 9u;
		if ((rounds & UINT64_C(0x3fff)) != 0) continue;
		struct timespec current {};
		if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &current) != 0) return;
		const int64_t elapsed_ns =
		    static_cast<int64_t>(current.tv_sec - started.tv_sec) *
		        INT64_C(1000000000) +
		    static_cast<int64_t>(current.tv_nsec - started.tv_nsec);
		if (elapsed_ns >=
		    static_cast<int64_t>(ANDROID_NODA_SELF_TEST_BUSY_BOUND_NS))
		{
			return;
		}
	}
}

struct AndroidNodaNoCommitPayload
{
	X64State* shadow = nullptr;
	uint32_t* shadow_steps = nullptr;
};

void AndroidNodaNoCommitCallback(void* opaque)
{
	auto* payload = static_cast<AndroidNodaNoCommitPayload*>(opaque);
	if (payload != nullptr && payload->shadow != nullptr &&
	    payload->shadow_steps != nullptr)
	{
		payload->shadow->gpr[0] = UINT64_C(0xbad0c0debad0c0de);
		payload->shadow->xmm[0][0] = UINT64_C(0xfeedfacecafebeef);
		payload->shadow->rip = UINT64_C(0x1111222233334444);
		payload->shadow->rflags = UINT64_C(0x246);
		*payload->shadow_steps = 1;
	}
	(void)raise(SIGILL);
}

struct AndroidNodaConcurrentShared
{
	std::atomic<uint32_t> arrivals {0};
	std::atomic<bool> release {false};
};

struct AndroidNodaConcurrentPayload
{
	AndroidNodaConcurrentShared* shared = nullptr;
	bool inject_fault = false;
	bool callback_completed = false;
	bool passed = false;
};

void AndroidNodaConcurrentCallback(void* opaque)
{
	auto* payload = static_cast<AndroidNodaConcurrentPayload*>(opaque);
	if (payload == nullptr || payload->shared == nullptr) return;
	payload->shared->arrivals.fetch_add(1, std::memory_order_acq_rel);
	while (!payload->shared->release.load(std::memory_order_acquire))
		(void)sched_yield();
	if (payload->inject_fault) (void)raise(SIGILL);
	payload->callback_completed = true;
}

void* AndroidNodaConcurrentWorker(void* opaque)
{
	auto* payload = static_cast<AndroidNodaConcurrentPayload*>(opaque);
	int32_t received = 0;
	const AndroidNodaExecutionStatus status = RunAndroidNodaIsolated(
	    AndroidNodaConcurrentCallback, payload,
	    ANDROID_NODA_CONCURRENT_TIMEOUT_NS, &received, 0, 0, true);
	const bool timer_released = ReleaseAndroidNodaThreadTimer();
	if (payload != nullptr)
		payload->passed =
		    timer_released &&
		    (payload->inject_fault
		         ? status == AndroidNodaExecutionStatus::Faulted &&
		               received == SIGILL && !payload->callback_completed
		         : status == AndroidNodaExecutionStatus::Completed &&
		               payload->callback_completed);
	return nullptr;
}

bool RunAndroidNodaIsolationSelfTest()
{
	sigset_t original_mask {};
	sigset_t blocked_mask {};
	sigset_t observed_mask {};
	sigset_t restored_mask {};
	const bool queried =
	    AndroidNodaSetRawSignalMask(nullptr, &original_mask);
	if (queried) blocked_mask = original_mask;
	bool prepared = queried && sigaddset(&blocked_mask, SIGILL) == 0 &&
	                sigdelset(&blocked_mask, 32) == 0;
	for (int reserved_signal = 33; prepared && reserved_signal <= 41;
	     reserved_signal++)
	{
		prepared = sigaddset(&blocked_mask, reserved_signal) == 0;
	}
	const bool applied =
	    prepared && AndroidNodaSetRawSignalMask(&blocked_mask, nullptr);
	bool observed =
	    applied && AndroidNodaSetRawSignalMask(nullptr, &observed_mask) &&
	    sigismember(&observed_mask, SIGILL) == 1 &&
	    sigismember(&observed_mask, 32) == 1;
	for (int reserved_signal = 33; observed && reserved_signal <= 41;
	     reserved_signal++)
	{
		observed = sigismember(&observed_mask, reserved_signal) == 0;
	}
	if (queried)
		RestoreAndroidNodaRawSignalMaskOrExit(
		    original_mask, "self-test-mask-restore");
	const bool restored =
	    queried &&
	    AndroidNodaSetRawSignalMask(nullptr, &restored_mask) &&
	    std::memcmp(&restored_mask, &original_mask, sizeof(original_mask)) == 0;
	if (!queried || !prepared || !applied || !observed || !restored)
	{
		const int64_t detail =
		    (queried ? 1 : 0) | (prepared ? 2 : 0) |
		    (applied ? 4 : 0) | (observed ? 8 : 0) |
		    (restored ? 16 : 0);
		return AndroidNodaSelfTestFailure(
		    "raw-signal-mask", AndroidNodaExecutionStatus::Unavailable,
		    detail);
	}

	AndroidNodaSuccessPayload success {};
	const AndroidNodaExecutionStatus success_status = RunAndroidNodaIsolated(
	    AndroidNodaSuccessCallback, &success,
	    ANDROID_NODA_ENTRY_TIMEOUT_NS, nullptr, 0, 0, true);
	if (success_status != AndroidNodaExecutionStatus::Completed ||
	    success.calls != 1)
	{
		return AndroidNodaSelfTestFailure(
		    "success", success_status, success.calls);
	}

	constexpr std::array<int, 4> fatal_signals = {
	    SIGILL, SIGSEGV, SIGBUS, SIGFPE};
	for (const int signal_number : fatal_signals)
	{
		int injected_signal = signal_number;
		int32_t received = 0;
		const AndroidNodaExecutionStatus fatal_status = RunAndroidNodaIsolated(
		    AndroidNodaSignalCallback, &injected_signal,
		    ANDROID_NODA_ENTRY_TIMEOUT_NS, &received, 0, 0, true);
		if (fatal_status != AndroidNodaExecutionStatus::Faulted ||
		    received != signal_number)
		{
			return AndroidNodaSelfTestFailure(
			    "fatal", fatal_status, signal_number, received);
		}
	}

	const uintptr_t pc_trap_begin = reinterpret_cast<uintptr_t>(
	    &LumenAndroidNodaPcRangeTrap);
	const uintptr_t pc_trap_end = reinterpret_cast<uintptr_t>(
	    LumenAndroidNodaPcRangeTrapEnd);
	int32_t pc_trap_signal = 0;
	const AndroidNodaExecutionStatus pc_trap_status =
	    pc_trap_begin != 0 && pc_trap_end > pc_trap_begin
	        ? RunAndroidNodaIsolated(
	              LumenAndroidNodaPcRangeTrap, nullptr,
	              ANDROID_NODA_ENTRY_TIMEOUT_NS, &pc_trap_signal,
	              pc_trap_begin,
	              static_cast<size_t>(pc_trap_end - pc_trap_begin), false)
	        : AndroidNodaExecutionStatus::Unavailable;
	if (pc_trap_status != AndroidNodaExecutionStatus::Faulted ||
	    pc_trap_signal != SIGILL)
	{
		return AndroidNodaSelfTestFailure(
		    "pc-range", pc_trap_status, pc_trap_signal,
		    static_cast<int64_t>(pc_trap_end - pc_trap_begin));
	}

	const AndroidNodaExecutionStatus timeout_status = RunAndroidNodaIsolated(
	    AndroidNodaBusyCallback, nullptr,
	    ANDROID_NODA_SELF_TEST_TIMEOUT_NS, nullptr, 0, 0, true);
	if (timeout_status != AndroidNodaExecutionStatus::TimedOut)
	{
		return AndroidNodaSelfTestFailure("timeout", timeout_status);
	}

	X64State live {};
	live.gpr[0] = UINT64_C(0x123456789abcdef0);
	live.xmm[0][0] = UINT64_C(0x0f1e2d3c4b5a6978);
	live.rip = UINT64_C(0x5555666677778888);
	live.rflags = UINT64_C(0x202);
	X64State baseline {};
	X64State shadow {};
	CopyAndroidNodaArchitectedState(&baseline, live);
	CopyAndroidNodaArchitectedState(&shadow, live);
	uint32_t live_steps = 0;
	uint32_t shadow_steps = 0;
	AndroidNodaNoCommitPayload no_commit {&shadow, &shadow_steps};
	const AndroidNodaExecutionStatus no_commit_status = RunAndroidNodaIsolated(
	    AndroidNodaNoCommitCallback, &no_commit,
	    ANDROID_NODA_ENTRY_TIMEOUT_NS, nullptr, 0, 0, true);
	const bool committed = CommitAndroidNodaArchitectedState(
	    &live, shadow,
	    no_commit_status == AndroidNodaExecutionStatus::Completed);
	if (committed) live_steps = shadow_steps;
	if (no_commit_status != AndroidNodaExecutionStatus::Faulted || committed ||
	    live_steps != 0 || shadow_steps != 1 ||
	    !EqualAndroidNodaArchitectedState(live, baseline) ||
	    EqualAndroidNodaArchitectedState(shadow, baseline))
	{
		const int64_t detail =
		    (committed ? 1 : 0) | (static_cast<int64_t>(live_steps) << 8) |
		    (static_cast<int64_t>(shadow_steps) << 24);
		return AndroidNodaSelfTestFailure(
		    "no-commit", no_commit_status, detail);
	}

	AndroidNodaConcurrentShared concurrent {};
	std::array<AndroidNodaConcurrentPayload, 2> workers {};
	workers[0].shared = &concurrent;
	workers[1].shared = &concurrent;
	workers[1].inject_fault = true;
	std::array<pthread_t, 2> threads {};
	const int first_create = pthread_create(
	    &threads[0], nullptr, AndroidNodaConcurrentWorker, &workers[0]);
	if (first_create != 0)
	{
		return AndroidNodaSelfTestFailure(
		    "concurrency-create-first", AndroidNodaExecutionStatus::Unavailable,
		    first_create);
	}
	const int second_create = pthread_create(
	    &threads[1], nullptr, AndroidNodaConcurrentWorker, &workers[1]);
	if (second_create != 0)
	{
		concurrent.release.store(true, std::memory_order_release);
		(void)pthread_join(threads[0], nullptr);
		return AndroidNodaSelfTestFailure(
		    "concurrency-create-second", AndroidNodaExecutionStatus::Unavailable,
		    second_create);
	}
	struct timespec deadline {};
	const bool clock_ready = clock_gettime(CLOCK_MONOTONIC, &deadline) == 0;
	deadline.tv_sec += 1;
	while (clock_ready &&
	       concurrent.arrivals.load(std::memory_order_acquire) != 2)
	{
		struct timespec now {};
		if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
		    now.tv_sec > deadline.tv_sec ||
		    (now.tv_sec == deadline.tv_sec &&
		     now.tv_nsec >= deadline.tv_nsec))
		{
			break;
		}
		(void)sched_yield();
	}
	const bool overlapped = clock_ready &&
	    concurrent.arrivals.load(std::memory_order_acquire) == 2;
	concurrent.release.store(true, std::memory_order_release);
	const bool joined_first = pthread_join(threads[0], nullptr) == 0;
	const bool joined_second = pthread_join(threads[1], nullptr) == 0;
	const bool passed = overlapped && joined_first && joined_second &&
	                    workers[0].passed && workers[1].passed;
	if (!passed)
	{
		const int64_t detail =
		    (overlapped ? 1 : 0) | (joined_first ? 2 : 0) |
		    (joined_second ? 4 : 0) | (workers[0].passed ? 8 : 0) |
		    (workers[1].passed ? 16 : 0);
		return AndroidNodaSelfTestFailure(
		    "concurrency", AndroidNodaExecutionStatus::Unavailable, detail,
		    concurrent.arrivals.load(std::memory_order_acquire));
	}
	return true;
}

struct AndroidNodaNativeInvocation
{
	EntryPoint entry = nullptr;
	EntryContext* context = nullptr;
	uint32_t returned_steps = 0;
};

void AndroidNodaNativeEntryCallback(void* opaque)
{
	auto* invocation = static_cast<AndroidNodaNativeInvocation*>(opaque);
	if (invocation != nullptr && invocation->entry != nullptr)
		invocation->returned_steps = invocation->entry(invocation->context);
}

bool RestoreAndroidNodaSignalActionsPrefixIfOwned(size_t count)
{
	g_android_noda_isolation_validated.store(false,
	                                          std::memory_order_release);
	g_android_noda_isolation_healthy.store(false, std::memory_order_release);
	g_android_noda_isolation_initialized.store(false,
	                                            std::memory_order_release);
	bool restored = true;
	const size_t restore_count =
	    (std::min)(count, ANDROID_NODA_SIGNALS.size());
	for (size_t index = 0; index < restore_count; index++)
	{
		struct sigaction current {};
		const int signal_number = ANDROID_NODA_SIGNALS[index];
		if (sigaction(signal_number, nullptr, &current) != 0)
		{
			const int error_code = errno;
			(void)AndroidNodaInitializationFailure(
			    "restore-query", signal_number, error_code);
			restored = false;
			continue;
		}
		// Ownership may have changed after installation. Never overwrite a
		// later owner while rolling this tier back.
		if (!IsAndroidNodaSignalAction(current)) continue;
		if (sigaction(signal_number, &g_android_noda_previous_actions[index],
		              nullptr) == 0)
		{
			continue;
		}
		const int error_code = errno;
		(void)AndroidNodaInitializationFailure(
		    "restore-install", signal_number, error_code, &current);
		restored = false;
	}
	return restored;
}

bool RestoreAndroidNodaSignalActionsIfOwned()
{
	return RestoreAndroidNodaSignalActionsPrefixIfOwned(
	    ANDROID_NODA_SIGNALS.size());
}

} // namespace

void CopyAndroidNodaArchitectedState(X64State* destination,
	                                  const X64State& source)
{
	if (destination == nullptr) return;
	std::memcpy(destination->gpr, source.gpr, sizeof(source.gpr));
	std::memcpy(destination->xmm, source.xmm, sizeof(source.xmm));
	destination->rip = source.rip;
	destination->rflags = source.rflags;
}

bool EqualAndroidNodaArchitectedState(const X64State& left,
	                                   const X64State& right)
{
	return std::memcmp(left.gpr, right.gpr, sizeof(left.gpr)) == 0 &&
	       std::memcmp(left.xmm, right.xmm, sizeof(left.xmm)) == 0 &&
	       left.rip == right.rip && left.rflags == right.rflags;
}

bool CommitAndroidNodaArchitectedState(X64State* live,
	                                   const X64State& shadow,
	                                   bool validation_passed)
{
	if (live == nullptr || !validation_passed) return false;
	CopyAndroidNodaArchitectedState(live, shadow);
	return true;
}

bool InitializeAndroidNodaIsolation()
{
	std::lock_guard<std::mutex> lock(g_android_noda_isolation_mutex);
	if (g_android_noda_initialization_attempted)
		return g_android_noda_isolation_initialized.load(
		    std::memory_order_acquire) &&
		       g_android_noda_isolation_healthy.load(std::memory_order_acquire);
	g_android_noda_initialization_attempted = true;

	for (size_t index = 0; index < ANDROID_NODA_SIGNALS.size(); index++)
	{
		const int signal_number = ANDROID_NODA_SIGNALS[index];
		struct sigaction previous {};
		if (sigaction(signal_number, nullptr, &previous) != 0)
		{
			const int error_code = errno;
			(void)AndroidNodaInitializationFailure(
			    "query", signal_number, error_code);
			if (!RestoreAndroidNodaSignalActionsPrefixIfOwned(index))
				return false;
			return false;
		}
		const bool timeout_signal =
		    signal_number == ANDROID_NODA_TIMEOUT_SIGNAL;
		const bool fatal_chain_ready =
		    !timeout_signal && previous.sa_handler != SIG_DFL &&
		    previous.sa_handler != SIG_IGN &&
		    (previous.sa_flags & SA_SIGINFO) != 0 &&
		    (previous.sa_flags & SA_RESETHAND) == 0;
		if ((timeout_signal && previous.sa_handler != SIG_DFL) ||
		    (!timeout_signal && !fatal_chain_ready))
		{
			(void)AndroidNodaInitializationFailure(
			    "occupied", signal_number, 0, &previous);
			if (!RestoreAndroidNodaSignalActionsPrefixIfOwned(index))
				return false;
			return false;
		}
		g_android_noda_previous_actions[index] = previous;

		struct sigaction replacement {};
		replacement.sa_sigaction = AndroidNodaSignalHandler;
		replacement.sa_flags = SA_SIGINFO | SA_ONSTACK;
		replacement.sa_flags |= previous.sa_flags & SA_RESTART;
#if defined(SA_EXPOSE_TAGBITS)
		replacement.sa_flags |= previous.sa_flags & SA_EXPOSE_TAGBITS;
#endif
		bool mask_ready = sigemptyset(&replacement.sa_mask) == 0;
		for (const int contained_signal : ANDROID_NODA_SIGNALS)
		{
			if (mask_ready &&
			    sigaddset(&replacement.sa_mask, contained_signal) != 0)
			{
				mask_ready = false;
			}
		}
		if (!mask_ready)
		{
			const int error_code = errno;
			(void)AndroidNodaInitializationFailure(
			    "mask-build", signal_number, error_code);
			if (!RestoreAndroidNodaSignalActionsPrefixIfOwned(index))
				return false;
			return false;
		}
		if (sigaction(signal_number, &replacement, nullptr) != 0)
		{
			const int error_code = errno;
			(void)AndroidNodaInitializationFailure(
			    "install", signal_number, error_code);
			if (!RestoreAndroidNodaSignalActionsPrefixIfOwned(index))
				return false;
			return false;
		}
	}

	g_android_noda_isolation_healthy.store(true, std::memory_order_release);
	g_android_noda_isolation_initialized.store(true,
	                                           std::memory_order_release);
	return true;
}

bool ValidateAndroidNodaIsolation()
{
	if (!InitializeAndroidNodaIsolation()) return false;
	std::lock_guard<std::mutex> lock(g_android_noda_isolation_mutex);
	if (g_android_noda_validation_attempted)
		return g_android_noda_isolation_validated.load(
		           std::memory_order_acquire) &&
		       g_android_noda_isolation_healthy.load(std::memory_order_acquire);
	g_android_noda_validation_attempted = true;
	const bool self_test_passed = RunAndroidNodaIsolationSelfTest();
	const bool ownership_passed =
	    self_test_passed && VerifyAndroidNodaSignalOwnershipInternal();
	const bool stack_passed = ownership_passed && VerifyAndroidNodaThreadStack();
	const bool isolation_passed =
	    self_test_passed && ownership_passed && stack_passed;
	if (self_test_passed && !ownership_passed)
		(void)AndroidNodaSelfTestFailure(
		    "post-ownership", AndroidNodaExecutionStatus::Unavailable);
	else if (ownership_passed && !stack_passed)
		(void)AndroidNodaSelfTestFailure(
		    "post-altstack", AndroidNodaExecutionStatus::Unavailable);
	const bool timer_released = ReleaseAndroidNodaThreadTimer();
	if (isolation_passed && !timer_released)
		(void)AndroidNodaSelfTestFailure(
		    "post-timer-release", AndroidNodaExecutionStatus::Unavailable);
	const bool passed = isolation_passed && timer_released;
	g_android_noda_isolation_validated.store(passed,
	                                         std::memory_order_release);
	if (!passed)
	{
		const bool signal_actions_restored =
		    RestoreAndroidNodaSignalActionsIfOwned();
		if (!signal_actions_restored)
			(void)AndroidNodaSelfTestFailure(
			    "post-signal-restore",
			    AndroidNodaExecutionStatus::Unavailable);
	}
	return passed;
}

bool IsAndroidNodaIsolationReady()
{
	return g_android_noda_isolation_initialized.load(
	           std::memory_order_acquire) &&
	       g_android_noda_isolation_validated.load(std::memory_order_acquire) &&
	       g_android_noda_isolation_healthy.load(std::memory_order_acquire);
}

bool VerifyAndroidNodaIsolationOwnership()
{
	return IsAndroidNodaIsolationReady() &&
	       VerifyAndroidNodaSignalOwnershipInternal();
}

void InvalidateAndroidNodaIsolation()
{
	g_android_noda_isolation_healthy.store(false, std::memory_order_release);
	g_android_noda_isolation_validated.store(false, std::memory_order_release);
}

AndroidNodaExecutionResult CompiledBlock::ExecuteAndroidNodaIsolated(
	EntryContext* context) const
{
	AndroidNodaExecutionResult result {};
	if (!IsAndroidNodaIsolationReady() || context == nullptr ||
	    !m_direct_basic_block || !m_code.IsPublished() ||
	    context->state == nullptr || context->runtime_context != nullptr ||
	    context->resume_rip != m_ir.rip || context->executed_steps != 0 ||
	    context->direct_steps != 0 ||
	    context->exit_reason != ExitReason::Bailout ||
	    context->call_stack_transaction_conflict != 0 ||
	    context->scalar_memory_host_address != 0 ||
	    context->scalar_memory_read_host_address != 0 ||
	    context->scalar_memory_next != 0 ||
	    context->scalar_memory_post_action != 0 ||
	    context->scalar_memory_enabled != 0 ||
	    context->instruction_page_host_begin != nullptr ||
	    context->multi_segment_mismatch_stage != 0 ||
	    context->multi_segment_reserved != 0 ||
	    !std::all_of(
	        context->scalar_memory_regions.begin(),
	        context->scalar_memory_regions.end(),
	        [](const ScalarMemoryRegion& region)
	        {
		        return region.guest_begin == 0 && region.guest_end == 0 &&
		               region.host_begin == 0 && region.permissions == 0 &&
		               region.reserved == 0;
	        }) ||
	    context->cache_epoch != m_code.Epoch() ||
	    m_ir.instruction_count > context->remaining_steps)
	{
		return result;
	}
	const auto entry = reinterpret_cast<EntryPoint>(
	    const_cast<void*>(m_code.EntryAddress()));
	const uintptr_t code_begin = reinterpret_cast<uintptr_t>(
	    m_code.EntryAddress());
	const size_t code_size = m_code.PublishedSize();
	if (entry == nullptr || code_begin == 0 || code_size == 0 ||
	    code_begin > UINTPTR_MAX - code_size)
	{
		return result;
	}

	AndroidNodaNativeInvocation invocation {entry, context, 0};
	result.status = RunAndroidNodaIsolated(
	    AndroidNodaNativeEntryCallback, &invocation,
	    ANDROID_NODA_ENTRY_TIMEOUT_NS, &result.signal, code_begin, code_size,
	    false);
	if (result.status == AndroidNodaExecutionStatus::Completed)
		result.returned_steps = invocation.returned_steps;
	return result;
}
#endif

uint32_t CompiledBlock::Execute(EntryContext* context) const
{
	if (context == nullptr) return 0;
	ScalarMemoryInvocationGuard scalar_memory_guard(context);
	context->executed_steps = 0;
	context->direct_steps   = 0;
	context->resume_rip     = m_ir.rip;
	context->exit_reason    = ExitReason::Bailout;
	context->scalar_memory_host_address = 0;
	context->scalar_memory_read_host_address = 0;
	context->scalar_memory_post_action = 0;
	context->call_stack_transaction_conflict = 0;
	SanitizeScalarMemoryTranslations(context);
	if (!m_code.IsPublished() || context->state == nullptr ||
	    (!m_direct_basic_block && !HasRuntimeServices(context)))
	{
		return 0;
	}
	if (context->cache_epoch != m_code.Epoch())
	{
		context->exit_reason = ExitReason::Invalidated;
		return 0;
	}
	if (m_ir.instruction_count > context->remaining_steps)
	{
		context->exit_reason = ExitReason::StepLimit;
		return 0;
	}
	const auto entry = reinterpret_cast<EntryPoint>(const_cast<void*>(m_code.EntryAddress()));
	if (entry == nullptr) return 0;
	const uint32_t returned_steps = entry(context);
	if (returned_steps != context->executed_steps)
	{
		context->exit_reason = ExitReason::Bailout;
		// Either side may already reflect committed architected effects. Return a
		// non-zero count whenever either ABI channel says work happened so the
		// dispatcher cannot execute the C++ block a second time.
		return (std::max)(returned_steps, context->executed_steps);
	}
	return context->executed_steps;
}

uint32_t CompiledBlock::ExecutePreparedDirect(EntryContext* context) const
{
	// This is intentionally not a relaxed version of Execute(). The caller must
	// publish a fresh segment boundary, and only a verified helper-free direct
	// block may bypass runtime-service and scalar-translation preparation.
	if (context == nullptr || !m_direct_basic_block || !m_code.IsPublished() ||
	    context->state == nullptr || context->runtime_context != nullptr ||
	    context->resume_rip != m_ir.rip || context->executed_steps != 0 ||
	    context->direct_steps != 0 || context->exit_reason != ExitReason::Bailout ||
	    context->call_stack_transaction_conflict != 0)
	{
		return 0;
	}
	if (context->cache_epoch != m_code.Epoch())
	{
		context->exit_reason = ExitReason::Invalidated;
		return 0;
	}
	if (m_ir.instruction_count > context->remaining_steps)
	{
		context->exit_reason = ExitReason::StepLimit;
		return 0;
	}
	const auto entry = reinterpret_cast<EntryPoint>(
	    const_cast<void*>(m_code.EntryAddress()));
	if (entry == nullptr) return 0;
	const uint32_t returned_steps = entry(context);
	if (returned_steps != context->executed_steps)
	{
		context->exit_reason = ExitReason::Bailout;
		return (std::max)(returned_steps, context->executed_steps);
	}
	return context->executed_steps;
}

const IrBlock& CompiledBlock::Ir() const
{
	return m_ir;
}

uint64_t CompiledBlock::Epoch() const
{
	return m_code.Epoch();
}

const void* CompiledBlock::CodeAddress() const
{
	return m_code.EntryAddress();
}

size_t CompiledBlock::CodeSize() const
{
	return m_code.PublishedSize();
}

size_t CompiledBlock::CodeCapacity() const
{
	return m_code.Capacity();
}

uint8_t CompiledBlock::DirectInstructionCount() const
{
	return m_direct_instruction_count;
}

bool CompiledBlock::IsDirectFallthrough() const
{
	return m_direct_fallthrough;
}

bool CompiledBlock::IsDirectBasicBlock() const
{
	return m_direct_basic_block;
}

bool CompiledBlock::IsProductionScalarMemoryFallthrough() const
{
	return m_production_scalar_memory_fallthrough;
}

bool CompiledBlock::IsCompiled() const
{
	return m_code.IsPublished();
}

#if defined(__ANDROID__) && \
    defined(LUMEN_ANDROID_DYNAREC_PROVIDER_AARCH64_WX)
bool CompiledBlock::ResetCodeForAndroidProbe()
{
	return m_code.ResetForAndroidProbe();
}
#endif

void CompiledBlock::RetireCodeForReuseAfterQuiescence()
{
	m_code.RetireForReuseAfterQuiescence();
	m_production_scalar_memory_fallthrough = false;
}

#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
#if LUMEN_A64_DYNAREC_MACOS
struct JitCopyTestLease
{
	JitCopyTestLease(void* destination, size_t capacity, const void* source, size_t size):
	    request(destination, capacity, source, size)
	{
	}

	RegisteredJitCopy request;
};
#else
struct JitCopyTestLease
{
};
#endif

void ConfigureJitCopyConcurrencyTest(uint32_t expected_callbacks)
{
#if LUMEN_A64_DYNAREC_MACOS
	g_jit_copy_test_active.store(0, std::memory_order_relaxed);
	g_jit_copy_test_arrivals.store(0, std::memory_order_relaxed);
	g_jit_copy_test_max_concurrent.store(0, std::memory_order_relaxed);
	g_jit_copy_test_release.store(expected_callbacks == 0, std::memory_order_relaxed);
	g_jit_copy_test_expected.store(expected_callbacks, std::memory_order_release);
#else
	(void)expected_callbacks;
#endif
}

void ReleaseJitCopyConcurrencyTest()
{
#if LUMEN_A64_DYNAREC_MACOS
	g_jit_copy_test_release.store(true, std::memory_order_release);
#endif
}

JitCopyTestSnapshot GetJitCopyTestSnapshot()
{
	JitCopyTestSnapshot snapshot {};
#if LUMEN_A64_DYNAREC_MACOS
	snapshot.arrivals = g_jit_copy_test_arrivals.load(std::memory_order_acquire);
	snapshot.max_concurrent =
	    g_jit_copy_test_max_concurrent.load(std::memory_order_acquire);
	snapshot.last_token = g_jit_copy_test_last_token.load(std::memory_order_acquire);
#endif
	return snapshot;
}

int InvokeJitCopyCallbackForTest(uintptr_t token)
{
#if LUMEN_A64_DYNAREC_MACOS
	return CopyJitCode(reinterpret_cast<void*>(token));
#else
	(void)token;
	return EINVAL;
#endif
}

JitCopyTestLease* CreateJitCopyLeaseForTest(void* destination, size_t capacity,
                                            const void* source, size_t size)
{
#if LUMEN_A64_DYNAREC_MACOS
	auto* lease = new (std::nothrow) JitCopyTestLease(destination, capacity, source, size);
	if (lease != nullptr && !lease->request.IsRegistered())
	{
		delete lease;
		lease = nullptr;
	}
	return lease;
#else
	(void)destination;
	(void)capacity;
	(void)source;
	(void)size;
	return nullptr;
#endif
}

uintptr_t JitCopyLeaseTokenForTest(const JitCopyTestLease* lease)
{
#if LUMEN_A64_DYNAREC_MACOS
	return lease == nullptr ? 0 : reinterpret_cast<uintptr_t>(lease->request.Opaque());
#else
	(void)lease;
	return 0;
#endif
}

void DestroyJitCopyLeaseForTest(JitCopyTestLease* lease)
{
	delete lease;
}
#endif

bool IsHostSupported()
{
#if defined(__AARCH64EB__) || defined(__arm64e__)
	return false;
#elif LUMEN_A64_DYNAREC_MACOS
	return pthread_jit_write_protect_supported_np() != 0;
#elif LUMEN_A64_DYNAREC_LINUX || LUMEN_A64_DYNAREC_WINDOWS
	return true;
#else
	return false;
#endif
}

Status ExecutableCodeMemory::Allocate(size_t minimum_capacity)
{
	std::lock_guard<std::mutex> lock(m_publish_mutex);
	if (m_address != nullptr) return Status::AlreadyAllocated;
	if (minimum_capacity == 0) return Status::InvalidArgument;
	if (!IsHostSupported()) return Status::UnsupportedHost;

	const size_t page_size = HostPageSize();
	size_t       capacity  = 0;
	if (page_size == 0 || !RoundUp(minimum_capacity, page_size, &capacity)) return Status::CapacityExceeded;

	void* address = nullptr;
#if LUMEN_A64_DYNAREC_MACOS
	address = AcquireRecycledJitPage(capacity);
	const bool recycled_lease = address != nullptr;
	if (address == nullptr)
	{
		address = mmap(nullptr, capacity, PROT_READ | PROT_WRITE | PROT_EXEC,
		               MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
	}
	if (address == MAP_FAILED) address = nullptr;
#elif LUMEN_A64_DYNAREC_LINUX
	address = mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (address == MAP_FAILED) address = nullptr;
#elif LUMEN_A64_DYNAREC_WINDOWS
	address = VirtualAlloc(nullptr, capacity, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#endif
	if (address == nullptr) return Status::AllocationFailed;

	m_address  = address;
	m_capacity = capacity;
	m_dense_resident_slice = false;
	m_dense_resident_page = UINT32_MAX;
	m_dense_resident_slot = UINT8_MAX;
#if LUMEN_A64_DYNAREC_MACOS
	m_recycled_lease = recycled_lease;
#else
	m_recycled_lease = false;
#endif
	m_epoch    = NextEpoch();
	m_publish_started = false;
	return Status::Ok;
}

Status ExecutableCodeMemory::AllocateDenseResident(size_t minimum_capacity)
{
	std::lock_guard<std::mutex> lock(m_publish_mutex);
	if (m_address != nullptr) return Status::AlreadyAllocated;
	if (minimum_capacity == 0) return Status::InvalidArgument;
	if (!IsHostSupported()) return Status::UnsupportedHost;
#if LUMEN_A64_DYNAREC_MACOS
	const size_t page_size = JitPageRecyclePageSize();
	if (page_size == 0 ||
	    DenseResidentSlotSize(minimum_capacity, page_size) == 0)
	{
		// This artifact can never fit an arena class. Keep it distinct from a
		// temporarily full compatible class so releases do not trigger retries.
		return Status::UnsupportedOperation;
	}
	void* address = nullptr;
	size_t capacity = 0;
	uint32_t page_index = UINT32_MAX;
	uint8_t slot_index = UINT8_MAX;
	if (!AcquireDenseResidentSlice(minimum_capacity, &address, &capacity,
	                              &page_index, &slot_index))
	{
		return Status::CapacityExceeded;
	}
	m_address = address;
	m_capacity = capacity;
	m_dense_resident_slice = true;
	m_dense_resident_page = page_index;
	m_dense_resident_slot = slot_index;
	// A recycled arena page or a reused slot can contain a longer old entry.
	// Publication always writes one zero instruction immediately after the new
	// artifact so accidental fallthrough fails closed.
	m_recycled_lease = true;
	m_epoch = NextEpoch();
	m_publish_started = false;
	return Status::Ok;
#else
	return Status::UnsupportedHost;
#endif
}

Status ExecutableCodeMemory::Publish(const void* staging_code, size_t size)
{
	std::lock_guard<std::mutex> lock(m_publish_mutex);
	if (m_address == nullptr) return Status::NotAllocated;
	if (staging_code == nullptr || size == 0 || (size % A64_WORD_SIZE) != 0) return Status::InvalidArgument;
	if (size > m_capacity) return Status::CapacityExceeded;
	if (m_publish_started)
	{
		return Status::AlreadyPublished;
	}
	m_publish_started = true;

	// Snapshot before opening the Apple JIT write window. The allowlisted
	// callback never follows a caller-owned pointer that another thread can
	// mutate while executable mappings are writable.
	auto staging_snapshot = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[size]);
	if (!staging_snapshot)
	{
		ResetUnlocked();
		return Status::AllocationFailed;
	}
	std::memcpy(staging_snapshot.get(), staging_code, size);

#if LUMEN_A64_DYNAREC_MACOS
	RegisteredJitCopy request(m_address, m_capacity, staging_snapshot.get(),
	                          size, m_recycled_lease);
	if (!request.IsRegistered())
	{
		ResetUnlocked();
		return Status::AllocationFailed;
	}
	const int copy_result = pthread_jit_write_with_callback_np(CopyJitCode, request.Opaque());
	// pthread_jit_write_with_callback_np is synchronous. Retire the trusted
	// descriptor before either unmapping the destination on failure or
	// destroying its private source snapshot on return.
	request.Release();
	if (copy_result != 0)
	{
		ResetUnlocked();
		return Status::PlatformFailure;
	}
	const size_t invalidated_size =
	    m_recycled_lease && m_capacity - size >= A64_WORD_SIZE
	        ? size + A64_WORD_SIZE
	        : size;
	sys_icache_invalidate(m_address, invalidated_size);
#elif LUMEN_A64_DYNAREC_LINUX
	std::memcpy(m_address, staging_snapshot.get(), size);
	__builtin___clear_cache(static_cast<char*>(m_address), static_cast<char*>(m_address) + size);
	if (mprotect(m_address, m_capacity, PROT_READ | PROT_EXEC) != 0)
	{
		ResetUnlocked();
		return Status::PlatformFailure;
	}
#elif LUMEN_A64_DYNAREC_WINDOWS
	std::memcpy(m_address, staging_snapshot.get(), size);
	DWORD old_protection = 0;
	if (VirtualProtect(m_address, m_capacity, PAGE_EXECUTE_READ, &old_protection) == 0 ||
	    FlushInstructionCache(GetCurrentProcess(), m_address, size) == 0)
	{
		ResetUnlocked();
		return Status::PlatformFailure;
	}
#else
	return Status::UnsupportedHost;
#endif

#if defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE) && LUMEN_A64_DYNAREC_MACOS
	// Publish the sidecar record before making the native entry observable.
	// Prepare is intentionally lazy as well as explicitly callable: this keeps
	// module-init mappings that appear after the pre-StartAllModules arm point.
	if (HostPcProfileEnvironmentRequested() && PrepareHostPcProfile())
	{
		m_host_pc_profile_token = RegisterHostPcProfileMapping(
		    reinterpret_cast<uintptr_t>(m_address), size, m_capacity,
		    m_epoch);
	}
#endif
	m_published_size.store(size, std::memory_order_release);
	return Status::Ok;
}

void ExecutableCodeMemory::Reset()
{
	std::lock_guard<std::mutex> lock(m_publish_mutex);
	(void)ResetUnlocked();
}

#if defined(__ANDROID__) && \
    defined(LUMEN_ANDROID_DYNAREC_PROVIDER_AARCH64_WX)
bool ExecutableCodeMemory::ResetForAndroidProbe()
{
	std::lock_guard<std::mutex> lock(m_publish_mutex);
	return ResetUnlocked();
}
#endif

void ExecutableCodeMemory::RetireForReuseAfterQuiescence()
{
#if LUMEN_A64_DYNAREC_MACOS
	void* address = nullptr;
	size_t capacity = 0;
	{
		std::lock_guard<std::mutex> lock(m_publish_mutex);
		if (m_address == nullptr) return;
		if (m_dense_resident_slice || !JitPageRecycleRequested() ||
		    m_capacity != JitPageRecyclePageSize() ||
		    !m_publish_started ||
		    m_published_size.load(std::memory_order_acquire) == 0)
		{
			ResetUnlocked();
			return;
		}

		address = m_address;
		capacity = m_capacity;
#if defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE)
		// Close the old generation while its address still denotes this owner.
		// A later Publish may legitimately register the same base with a new
		// epoch, and the profiler must never join the two lifetimes.
		const uint64_t host_pc_profile_token = m_host_pc_profile_token;
		m_host_pc_profile_token = 0;
		if (host_pc_profile_token != 0)
		{
			(void)RetireHostPcProfileMapping(host_pc_profile_token);
		}
#endif
		m_published_size.store(0, std::memory_order_release);
		m_address = nullptr;
		m_capacity = 0;
		m_recycled_lease = false;
		m_epoch = NextEpoch();
		m_publish_started = false;
	}

	// Never hold an artifact mutex while contending on the bounded global
	// stack, and never unmap while holding the pool mutex.
	if (!StoreRecycledJitPage(address, capacity))
		munmap(address, capacity);
#else
	Reset();
#endif
}

#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_CPU_X64_TEST) || \
    defined(LUMEN_JIT_PAGE_RECYCLE_BENCHMARK)
void ExecutableCodeMemory::RetireForReuseAfterQuiescenceForTest()
{
	RetireForReuseAfterQuiescence();
}
#endif

bool ExecutableCodeMemory::ResetUnlocked()
{
	if (m_address == nullptr) return true;

	void*  address  = m_address;
	size_t capacity = m_capacity;
#if LUMEN_A64_DYNAREC_MACOS
	const bool dense_resident_slice = m_dense_resident_slice;
	const uint32_t dense_resident_page = m_dense_resident_page;
	const uint8_t dense_resident_slot = m_dense_resident_slot;
#endif
#if defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE) && LUMEN_A64_DYNAREC_MACOS
	// The retirement timestamp must be committed while the mapping still
	// exists. Once close has been claimed the frozen zero value correctly
	// means that the mapping was live at the profile cutoff.
	const uint64_t host_pc_profile_token = m_host_pc_profile_token;
	m_host_pc_profile_token = 0;
	if (host_pc_profile_token != 0)
	{
		(void)RetireHostPcProfileMapping(host_pc_profile_token);
	}
#endif
	m_published_size.store(0, std::memory_order_release);
	m_address  = nullptr;
	m_capacity = 0;
	m_recycled_lease = false;
	m_dense_resident_slice = false;
	m_dense_resident_page = UINT32_MAX;
	m_dense_resident_slot = UINT8_MAX;
	m_epoch    = NextEpoch();
	m_publish_started = false;

#if LUMEN_A64_DYNAREC_MACOS || LUMEN_A64_DYNAREC_LINUX
#if LUMEN_A64_DYNAREC_MACOS
	if (dense_resident_slice)
	{
		ReleaseDenseResidentSlice(dense_resident_page, dense_resident_slot);
		return true;
	}
	else
#endif
		return munmap(address, capacity) == 0;
#elif LUMEN_A64_DYNAREC_WINDOWS
	return VirtualFree(address, 0, MEM_RELEASE) != 0;
#else
	(void)address;
	(void)capacity;
	return true;
#endif
}

const void* ExecutableCodeMemory::EntryAddress() const
{
	return m_published_size.load(std::memory_order_acquire) != 0 ? m_address : nullptr;
}

size_t ExecutableCodeMemory::Capacity() const
{
	return m_capacity;
}

size_t ExecutableCodeMemory::PublishedSize() const
{
	return m_published_size.load(std::memory_order_acquire);
}

uint64_t ExecutableCodeMemory::Epoch() const
{
	return m_epoch;
}

bool ExecutableCodeMemory::IsPublished() const
{
	return m_published_size.load(std::memory_order_acquire) != 0;
}

} // namespace Lumen::Cpu::A64Dynarec
