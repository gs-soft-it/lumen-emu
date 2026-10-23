#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Kernel/HostWaitSleep.h"
#include "Emulator/Kernel/ReadMostlyObjectDirectory.h"

#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/Singleton.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Timer.h"
#include "Lumen/Core/Vector.h"
#include "Lumen/Core/VirtualMemory.h"
#include "../../../cpu/runtime/MemoryHandoff.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/RuntimeLinker.h"
#if defined(LUMEN_FEXCORE_RUNTIME)
#include "GuestCpuRuntime.h"
#endif
#include "Emulator/Loader/SystemContent.h"
#include "Emulator/Loader/Timer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#include <android/log.h>
#endif

#ifdef LUMEN_EMU_ENABLED

#if LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <pthread.h>

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
#include <pthread_time.h>
#endif

#if defined(LUMEN_FEXCORE_RUNTIME)
#include "AndroidFexCoreRuntime.h"
#endif
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
#include "AndroidFexCoreMutationBarrier.h"
#include <thread>
#endif

namespace Lumen::Libs {

namespace LibKernel {

LIB_NAME("libkernel", "libkernel");

constexpr int KEYS_MAX              = 256;
constexpr int DESTRUCTOR_ITERATIONS = 4;

constexpr int GUEST_PTHREAD_DEFAULT_PRIORITY = 700;

#if defined(LUMEN_POSIX_SEMAPHORE_TEST_HOOK) || defined(LUMEN_PTHREAD_ATTRIBUTE_TEST_HOOK)
static thread_local uint64_t g_argument_lease_acquisitions = 0;
uint64_t PthreadArgumentLeaseAcquisitionsForTest() { return g_argument_lease_acquisitions; }
#endif

// Acquire guest data intent before dereferencing HLE arguments. Attribute
// handles name private native objects, but the handle slots and result bytes
// can themselves alias a GPU resource. Keep these leases out of object locks
// and never retain one across a guest host wait.
class PthreadArgumentLease
{
public:
	int Acquire(const void* address, uint64_t bytes, Core::VirtualMemory::Mode mode)
	{
#if defined(LUMEN_POSIX_SEMAPHORE_TEST_HOOK) || defined(LUMEN_PTHREAD_ATTRIBUTE_TEST_HOOK)
		++g_argument_lease_acquisitions;
#endif
		if (address == nullptr) return KERNEL_ERROR_EINVAL;
#if defined(LUMEN_FEXCORE_RUNTIME)
		if (!Core::VirtualMemory::TryAcquireGuestDataLease(
		    reinterpret_cast<uint64_t>(address), bytes, mode, &m_lease))
			return KERNEL_ERROR_EFAULT;
#else
		(void)bytes;
		(void)mode;
#endif
		return OK;
	}

private:
#if defined(LUMEN_FEXCORE_RUNTIME)
	Core::VirtualMemory::GuestDataLease m_lease;
#endif
};

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && LUMEN_ABI == LUMEN_ABI_ARM64_V8A && defined(LUMEN_FEXCORE_RUNTIME)
#define LUMEN_MACOS_FEXCORE_SEPARATE_PTHREAD_STACK 1
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_MACOS_FEXCORE_SEPARATE_PTHREAD_STACK)
#define LUMEN_PTHREAD_MUTEX_QUEUED_ACQUISITION 1
#endif

struct PthreadMutexGuestPrefix
{
	uint8_t reserved[256];
};
static_assert(sizeof(PthreadMutexGuestPrefix) == 256);

struct MutexWaiter
{
	MutexWaiter* next = nullptr;
	bool queued = false;
	int thread_id = 0;
	uint64_t wake_generation = 0;
#if defined(LUMEN_PTHREAD_MUTEX_QUEUED_ACQUISITION)
	// Construct native wait state only when this ticket actually joins a queue.
	std::optional<std::condition_variable> changed;
#endif
};

struct PthreadMutexPrivate
{
	PthreadMutexGuestPrefix* guest_prefix = nullptr;
	String          name;
	pthread_mutex_t p;
	// The native mutex remains authoritative. Only its owner accesses depth;
	// timed acquisition observes owner solely to distinguish self-deadlock.
	std::atomic<int> owner {0};
	uint64_t         depth = 0;
	size_t           operation_pins = 0; // Protected by g_mutex_handles_mutex.
	ReadMostlyObjectDirectory<PthreadMutexPrivate>::Record* directory_record = nullptr;
#if defined(LUMEN_PTHREAD_MUTEX_QUEUED_ACQUISITION)
	std::mutex       acquisition_mutex;
	std::condition_variable acquisition_changed;
	uint64_t         wake_generation = 0;
	MutexWaiter*     first_waiter = nullptr;
	MutexWaiter*     last_waiter = nullptr;
#endif
};

enum class MutexErrorStage : uint8_t
{
	Resolve,
	SlotSnapshot,
	StaticNativeInit,
	RetiredPrefixClear,
	BackingAllocation,
	NativeTrylock,
	SelfDeadlock,
	Count,
};

// Diagnostics consume only scalar values already observed by the operation.
// The outer scope emits after every resolver lease and mutex-operation pin is
// gone; it never reads a guest slot or waits while holding a subsystem lock.
class MutexErrorTrace;
static thread_local MutexErrorTrace* g_mutex_error_trace = nullptr;

class MutexErrorTrace final
{
public:
	explicit MutexErrorTrace(const void* slot): m_slot(reinterpret_cast<uint64_t>(slot))
	{
		static const bool enabled = [] {
			const char* value = std::getenv("LUMEN_PTHREAD_MUTEX_ERROR_TRACE");
			return value != nullptr && std::strcmp(value, "1") == 0;
		}();
		if (!enabled) return;
		m_enabled = true;
		m_previous = g_mutex_error_trace;
		g_mutex_error_trace = this;
	}
	~MutexErrorTrace()
	{
		if (!m_enabled) return;
		g_mutex_error_trace = m_previous;
		if (m_result == OK) return;
		static std::array<std::atomic<uint32_t>, static_cast<size_t>(MutexErrorStage::Count)> seen {};
		static std::atomic<uint32_t> emitted {0};
		const auto occurrence = seen[static_cast<size_t>(m_stage)].fetch_add(1, std::memory_order_relaxed) + 1;
		if (occurrence > 4 && (occurrence & (occurrence - 1)) != 0) return;
		auto index = emitted.load(std::memory_order_relaxed);
		while (index < 256 && !emitted.compare_exchange_weak(index, index + 1, std::memory_order_relaxed)) {}
		if (index >= 256) return;
		static constexpr const char* names[] = {"resolve", "slot-snapshot", "static-native-init", "retired-prefix-clear",
		    "backing-allocation", "native-trylock", "self-deadlock"};
		const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		    std::chrono::steady_clock::now().time_since_epoch()).count();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		__android_log_print(ANDROID_LOG_INFO, "LumenPthread",
#else
		std::fprintf(stderr,
#endif
		    "[PTHREAD_MUTEX_ERROR_TRACE_V1] index=%u occurrence=%u monotonicMs=%lld thread=%d"
		    " slot=0x%016" PRIx64 " stage=%s guest_result=0x%08x"
		    " slot_snapshot=0x%016" PRIx64 " snapshot_valid=%u native_result=%d native_result_valid=%u"
		    " native_owner=%d owner_observed=%u depth=%" PRIu64 " depth_observed=%u"
		    " lease_reason=%u allocation_failure=%u retryable=%u\n",
		    index, occurrence, static_cast<long long>(now_ms), Core::Thread::GetThreadIdUnique(), m_slot,
		    names[static_cast<size_t>(m_stage)], static_cast<unsigned>(m_result), m_snapshot, m_snapshot_valid ? 1u : 0u,
		    m_native_result, m_native_result_valid ? 1u : 0u, m_owner, m_owner_observed ? 1u : 0u,
		    m_depth, m_depth_observed ? 1u : 0u, m_lease_reason, m_allocation_failure, m_retryable ? 1u : 0u);
	}
	int Result(int result) { m_result = result; return result; }
	void Stage(MutexErrorStage stage) { m_stage = stage; }
	void Snapshot(void* snapshot)
	{
		m_snapshot = reinterpret_cast<uint64_t>(snapshot);
		m_snapshot_valid = true;
		m_stage = MutexErrorStage::SlotSnapshot;
	}
	void NativeResult(int result)
	{
		m_native_result = result;
		m_native_result_valid = true;
	}
	void Ownership(PthreadMutex object)
	{
		m_owner = object->owner.load(std::memory_order_relaxed);
		m_owner_observed = true;
		// Only the native owner may access the non-atomic recursion depth.
		if (m_owner == Core::Thread::GetThreadIdUnique())
		{
			m_depth = object->depth;
			m_depth_observed = true;
		}
	}
	void LeaseDenial(const Core::VirtualMemory::GuestDataLeaseDenialMetadata& denial)
	{
		m_lease_reason = static_cast<unsigned>(denial.reason);
		m_retryable = denial.retryable;
	}
	void AllocationDenial(const Core::VirtualMemory::AllocationDenialMetadata& denial)
	{
		m_stage = MutexErrorStage::BackingAllocation;
		m_allocation_failure = static_cast<unsigned>(denial.reason);
		m_retryable = denial.retryable;
	}
	MutexErrorTrace(const MutexErrorTrace&) = delete;
	MutexErrorTrace& operator=(const MutexErrorTrace&) = delete;

private:
	MutexErrorTrace* m_previous = nullptr;
	uint64_t m_slot;
	uint64_t m_snapshot = 0;
	uint64_t m_depth = 0;
	int m_result = OK;
	int m_native_result = 0;
	int m_owner = 0;
	unsigned m_lease_reason = 0;
	unsigned m_allocation_failure = 0;
	MutexErrorStage m_stage = MutexErrorStage::Resolve;
	bool m_enabled = false;
	bool m_snapshot_valid = false;
	bool m_native_result_valid = false;
	bool m_owner_observed = false;
	bool m_depth_observed = false;
	bool m_retryable = false;
};

// Readmission can deliver a nested guest exception. Its unrelated HLE must
// not overwrite the interrupted lock's resolver observations.
class ScopedMutexErrorTracePause final
{
public:
	ScopedMutexErrorTracePause(): m_previous(g_mutex_error_trace) { g_mutex_error_trace = nullptr; }
	~ScopedMutexErrorTracePause() { g_mutex_error_trace = m_previous; }
private:
	MutexErrorTrace* m_previous;
};

static std::mutex g_mutex_handles_mutex;
static std::unordered_map<PthreadMutex, PthreadMutex> g_mutex_handles;
static std::vector<PthreadMutexGuestPrefix*> g_retired_mutex_prefixes;
using NativeMutexDirectory = ReadMostlyObjectDirectory<PthreadMutexPrivate>;
static NativeMutexDirectory g_mutex_directory;

static bool mutex_directory_enabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_PTHREAD_MUTEX_DIRECTORY");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

static PthreadMutex mutex_guest_handle(PthreadMutex object)
{
	return reinterpret_cast<PthreadMutex>(object->guest_prefix);
}

static PthreadMutex resolve_mutex_handle(PthreadMutex handle, bool pin = false)
{
	if (handle == nullptr) return nullptr;
	if (pin && mutex_directory_enabled())
	{
		const auto key = reinterpret_cast<uintptr_t>(handle);
		auto found = g_mutex_directory.FindAndPin(key);
		if (found.status == NativeMutexDirectory::Lookup::Pinned) return found.record->value;
		// A destroy may have closed enrollment before native destroy reports
		// EBUSY. Wait for that cold decision, then retry the same fresh handle.
		std::lock_guard lock(g_mutex_handles_mutex);
		found = g_mutex_directory.FindAndPin(key);
		return found.status == NativeMutexDirectory::Lookup::Pinned ? found.record->value : nullptr;
	}
	std::lock_guard lock(g_mutex_handles_mutex);
	const auto found = g_mutex_handles.find(handle);
	if (found == g_mutex_handles.end()) return nullptr;
	if (pin) ++found->second->operation_pins;
	return found->second;
}

class ScopedMutexOperation final
{
public:
	explicit ScopedMutexOperation(PthreadMutex pinned): m_mutex(pinned) {}
	~ScopedMutexOperation()
	{
		if (mutex_directory_enabled())
		{
			NativeMutexDirectory::Release(m_mutex->directory_record);
			return;
		}
		std::lock_guard lock(g_mutex_handles_mutex);
		EXIT_IF(m_mutex->operation_pins == 0);
		--m_mutex->operation_pins;
	}
	ScopedMutexOperation(const ScopedMutexOperation&) = delete;
	ScopedMutexOperation& operator=(const ScopedMutexOperation&) = delete;
private:
	PthreadMutex m_mutex;
};

// Caller holds g_mutex_handles_mutex until the output slot is written and the
// record opened. A stale alias cannot enroll in an unpublished native object.
static void register_mutex_handle_locked(PthreadMutex object, NativeMutexDirectory::Garbage& garbage)
{
	const auto handle = mutex_guest_handle(object);
	EXIT_IF(!g_mutex_handles.emplace(handle, object).second);
	if (mutex_directory_enabled())
	{
		auto* record = g_mutex_directory.Insert(reinterpret_cast<uintptr_t>(handle), object, garbage,
		    [](NativeMutexDirectory::Record* entry, void* opaque) {
			    static_cast<PthreadMutex>(opaque)->directory_record = entry;
			    entry->state.store(0, std::memory_order_relaxed);
		    }, object);
		EXIT_IF(record == nullptr);
	}
}

static bool clear_mutex_guest_prefix(PthreadMutexGuestPrefix* prefix,
    Core::VirtualMemory::GuestDataLeaseDenialMetadata* denied = nullptr)
{
	EXIT_IF(prefix == nullptr);
#if defined(__aarch64__)
	Core::VirtualMemory::GuestDataLease lease;
	Core::VirtualMemory::GuestDataLeaseDenialMetadata denial;
	if (!Core::VirtualMemory::TryAcquireGuestDataLease(
	    reinterpret_cast<uint64_t>(prefix), sizeof(*prefix), Core::VirtualMemory::Mode::Write,
	    &lease, false, &denial))
	{
		if (denied != nullptr) *denied = denial;
		using Reason = Core::VirtualMemory::GuestDataLeaseDenialReason;
		if (denial.reason == Reason::MappingLeaseCapabilityUnavailable ||
		    denial.reason == Reason::GateTrylockUnavailable ||
		    denial.reason == Reason::GateMutationActive ||
		    denial.reason == Reason::GateWriterPending) return false;
		EXIT("Pthread mutex guest prefix lease denied: %s\n",
		     Core::VirtualMemory::GuestDataLeaseDenialReasonName(denial.reason));
	}
#endif
	(void)denied;
	std::memset(prefix, 0, sizeof(*prefix));
	return true;
}

static PthreadMutexGuestPrefix* take_retired_mutex_prefix()
{
	std::lock_guard lock(g_mutex_handles_mutex);
	if (g_retired_mutex_prefixes.empty()) return nullptr;
	auto* prefix = g_retired_mutex_prefixes.back();
	g_retired_mutex_prefixes.pop_back();
	return prefix;
}

static void retire_mutex_guest_prefix(PthreadMutexGuestPrefix* prefix)
{
	// Destruction can run in a nested guest finalizer or behind another module's
	// HLE readers. Retain mapping ownership instead of requiring an exclusive
	// Free there. Every reuse must clear the prefix before publishing its alias.
	(void)clear_mutex_guest_prefix(prefix);
	std::lock_guard lock(g_mutex_handles_mutex);
	g_retired_mutex_prefixes.push_back(prefix);
}

static void destroy_unpublished_mutex(PthreadMutexPrivate* object)
{
	if (object == nullptr) return;
	if (object->guest_prefix != nullptr) retire_mutex_guest_prefix(object->guest_prefix);
	(void)pthread_mutex_destroy(&object->p);
	delete object;
}

using PreparedPthreadMutex = std::unique_ptr<PthreadMutexPrivate, decltype(&destroy_unpublished_mutex)>;

[[maybe_unused]] static void unregister_unpublished_mutex_locked(PreparedPthreadMutex& prepared,
    NativeMutexDirectory::Garbage& garbage)
{
	auto* object = prepared.get();
	g_mutex_handles.erase(mutex_guest_handle(object));
	if (mutex_directory_enabled())
	{
		// Published buckets may have readers, but the provisional record never
		// accepted a native operation. Snapshot retirement protects its record.
		prepared.release();
		EXIT_IF(!g_mutex_directory.Remove(object->directory_record, destroy_unpublished_mutex, garbage));
	}
}

static bool read_pthread_name(const char* name, std::string* copied)
{
	copied->clear();
	if (name == nullptr) return true;
	const auto address = reinterpret_cast<uint64_t>(name);
	for (uint64_t offset = 0; ; ++offset)
	{
		char byte = 0;
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
		if (address > UINT64_MAX - offset ||
		    !Loader::ReadGuestMemory(address + offset, &byte, sizeof(byte))) return false;
#else
		byte = name[offset];
#endif
		if (byte == '\0') return true;
		copied->push_back(byte);
	}
}

struct PthreadMutexattrPrivate
{
	uint8_t             reserved[64];
	pthread_mutexattr_t p;
	int                 pprotocol;
};

struct PthreadAttrPrivate
{
	uint8_t        reserved[64];
	std::atomic<KernelCpumask> affinity;
	size_t         guard_size;
	int            policy;
	int            inherit_sched = PTHREAD_INHERIT_SCHED;
	int            guest_sched_priority = GUEST_PTHREAD_DEFAULT_PRIORITY;
	bool           detached;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || defined(LUMEN_MACOS_FEXCORE_SEPARATE_PTHREAD_STACK)
	void*          guest_stack_addr = nullptr;
	size_t         guest_stack_size = 0;
#endif
	pthread_attr_t p;
};

struct PthreadCleanupHandler
{
	pthread_cleanup_func_t routine = nullptr;
	void*                  argument = nullptr;
	uint64_t               info_address = 0;
};

struct PthreadGuestPrefix
{
	uint8_t reserved[4096];
};
static_assert(sizeof(PthreadGuestPrefix) == 4096);
constexpr uint64_t GUEST_PTHREAD_BACKING_SIZE = 16 * 1024;

struct PthreadPrivate
{
	PthreadGuestPrefix*  guest_prefix = nullptr;
	String               name;
	std::mutex           name_mutex;
	pthread_t            p;
	PthreadAttr          attr;
	// Keep the guest policy/priority snapshot and native setter serialized.
	std::mutex           scheduling_mutex;
	int                  effective_guest_policy = 1;
	pthread_entry_func_t entry;
	void*                arg;
	int                  unique_id;
	// One pending PS4 exception request (SIGUSR1 stop-the-world). Written by
	// the raising thread through the resolved handle and consumed by this
	// thread at its next generated-code boundary, HLE boundary or host-wait
	// slice. Standard signals coalesce, so a second request while one is
	// pending keeps this slot set.
	std::atomic<uint32_t> pending_exception_signal {0};
	// One-shot token of the boundary interrupt armed by the raising thread
	// for this thread's current FEX run (0 = none). The raising thread stores
	// it after queueing the signal; this thread clears it when the request is
	// consumed or cancelled.
	std::atomic<uint64_t> boundary_interrupt_token {0};
	std::atomic_bool     started;
	std::atomic_bool     detached;
	std::atomic_bool     host_detached;
	std::atomic_bool     join_claimed {false};
	std::atomic_bool     almost_done;
	std::atomic_bool     free;
	int                  guest_sched_priority = GUEST_PTHREAD_DEFAULT_PRIORITY;
	std::vector<PthreadCleanupHandler> cleanup_handlers;
};

static Pthread pthread_guest_handle(Pthread thread)
{
	return thread == nullptr ? nullptr : reinterpret_cast<Pthread>(thread->guest_prefix);
}

static bool clear_pthread_guest_prefix(Pthread thread)
{
	EXIT_IF(thread == nullptr || thread->guest_prefix == nullptr);
#if defined(__aarch64__)
	Core::VirtualMemory::GuestDataLease lease;
	Core::VirtualMemory::GuestDataLeaseDenialMetadata denial;
	if (!Core::VirtualMemory::TryAcquireGuestDataLease(
	    reinterpret_cast<uint64_t>(thread->guest_prefix), sizeof(PthreadGuestPrefix),
	    Core::VirtualMemory::Mode::Write, &lease, false, &denial))
	{
		using Reason = Core::VirtualMemory::GuestDataLeaseDenialReason;
		// Native reapers have no HLE reader. Mutation contention grants no
		// pointer access; leave this inactive prefix for mandatory reset at reuse.
		if (denial.reason == Reason::MappingLeaseCapabilityUnavailable ||
		    denial.reason == Reason::GateTrylockUnavailable ||
		    denial.reason == Reason::GateMutationActive ||
		    denial.reason == Reason::GateWriterPending) return false;
		EXIT("Pthread guest prefix lease denied: %s\n",
		     Core::VirtualMemory::GuestDataLeaseDenialReasonName(denial.reason));
	}
#endif
	std::memset(thread->guest_prefix, 0, sizeof(PthreadGuestPrefix));
	return true;
}

struct PthreadBackingAllocation
{
	uint64_t address = 0;
	int error = KERNEL_ERROR_EAGAIN;
	uint32_t attempts = 0;
};

template <typename Allocate, typename Pause, typename Now>
static PthreadBackingAllocation allocate_pthread_guest_backing(uint64_t bytes, bool allow_retry, Allocate allocate, Pause pause, Now now)
{
	using Failure = Core::VirtualMemory::AllocationFailure;
	// A writer has a two-second safepoint deadline. Allow one further second
	// for readmission, without retrying allocator or permanent admission errors.
	const auto deadline = now() + std::chrono::seconds(3);
	constexpr uint32_t max_attempts = 4096;
	for (uint32_t attempt = 1; attempt <= max_attempts; ++attempt)
	{
		Core::VirtualMemory::AllocationDenialMetadata denial;
		const auto address = allocate(bytes, &denial);
		if (address != 0) return {address, OK, attempt};
		if (g_mutex_error_trace != nullptr) g_mutex_error_trace->AllocationDenial(denial);
		if (denial.reason == Failure::HostAllocationFailed) return {0, KERNEL_ERROR_ENOMEM, attempt};
		if (denial.reason == Failure::InvalidArgument) return {0, KERNEL_ERROR_EINVAL, attempt};
		if (!allow_retry || denial.reason != Failure::AdmissionDenied || !denial.retryable || attempt == max_attempts || now() >= deadline)
			return {0, KERNEL_ERROR_EAGAIN, attempt};
		// The caller has copied guest inputs and retains no local lock or lease.
		// Restore admission completely before a fresh allocation attempt; even
		// this additive mapping cannot be created inside a quiescent host wait.
		if (!pause() || now() >= deadline) return {0, KERNEL_ERROR_EAGAIN, attempt};
	}
	return {};
}

static PthreadBackingAllocation allocate_pthread_guest_backing(uint64_t bytes, bool allow_retry = false)
{
	const auto started = std::chrono::steady_clock::now();
	const auto result = allocate_pthread_guest_backing(bytes, allow_retry,
	    [](uint64_t size, Core::VirtualMemory::AllocationDenialMetadata* denial) {
		    return Core::VirtualMemory::Alloc(0, size, Core::VirtualMemory::Mode::ReadWrite, denial);
	    }, [] {
		    ScopedMutexErrorTracePause pause_trace;
		    Loader::ScopedGuestHostWait host_wait;
		    return host_wait.IsActive();
	    }, [] { return std::chrono::steady_clock::now(); });
	if (result.attempts > 1 || result.error != OK)
	{
		static std::atomic<uint32_t> reports {0};
		if (reports.fetch_add(1, std::memory_order_relaxed) < 16)
		{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
			__android_log_print(ANDROID_LOG_INFO, "LumenPthread",
#else
			std::fprintf(stderr,
#endif
			    "[PTHREAD_BACKING_ALLOCATION] bytes=%" PRIu64 " attempts=%u elapsed_ms=%lld result=0x%08x allocated=%u\n",
			    bytes, result.attempts, static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
			        std::chrono::steady_clock::now() - started).count()), static_cast<unsigned>(result.error), result.address != 0 ? 1u : 0u);
		}
	}
	return result;
}

static int prepare_pthread_guest_prefix(Pthread thread)
{
	EXIT_IF(thread == nullptr || thread->guest_prefix != nullptr);
	// Prepare the guest object before this wrapper can start a root or publish
	// TLS. The native wrapper and all native synchronization state stay private.
	// Core owns additive mutation admission and publication.
	// No pool lock, data lease or quiescent host wait surrounds this mutator.
	const auto allocated = allocate_pthread_guest_backing(GUEST_PTHREAD_BACKING_SIZE, true);
	if (allocated.error != OK) return allocated.error;
	thread->guest_prefix = reinterpret_cast<PthreadGuestPrefix*>(allocated.address);
	// Alloc supplies a zero-filled prefix and padding. No guest root has seen
	// this allocation, so initial publication needs no separate native store.
	return OK;
}

#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
static thread_local int g_mutex_preparation_test_error = OK;
static thread_local void (*g_explicit_mutex_preparation_test_hook)(void*) = nullptr;
static thread_local void* g_explicit_mutex_preparation_test_context = nullptr;
#endif

static int pthread_mutex_init_impl(PthreadMutex* mutex, const PthreadMutexattr* attr, const char* name, bool allow_retry);

static int prepare_mutex_guest_backing(PthreadMutexattr source_attr, const std::string& copied_name,
                                      bool allow_retry, PreparedPthreadMutex* prepared)
{
	if (source_attr == nullptr || prepared == nullptr || *prepared != nullptr) return KERNEL_ERROR_EINVAL;
	auto* object = new PthreadMutexPrivate {};
	object->name = copied_name.c_str();
	if (g_mutex_error_trace != nullptr) g_mutex_error_trace->Stage(MutexErrorStage::StaticNativeInit);
	const int result = pthread_mutex_init(&object->p, &source_attr->p);
	if (g_mutex_error_trace != nullptr) g_mutex_error_trace->NativeResult(result);
	if (result != 0)
	{
		delete object;
		return result == EAGAIN ? KERNEL_ERROR_EAGAIN : result == ENOMEM ? KERNEL_ERROR_ENOMEM : KERNEL_ERROR_EINVAL;
	}
	PreparedPthreadMutex candidate(object, destroy_unpublished_mutex);
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
	if (g_mutex_preparation_test_error != OK) return g_mutex_preparation_test_error;
#endif
	auto* prefix = take_retired_mutex_prefix();
	if (prefix != nullptr)
	{
		Core::VirtualMemory::GuestDataLeaseDenialMetadata denial;
		if (g_mutex_error_trace != nullptr) g_mutex_error_trace->Stage(MutexErrorStage::RetiredPrefixClear);
		if (!clear_mutex_guest_prefix(prefix, &denial))
		{
			if (g_mutex_error_trace != nullptr) g_mutex_error_trace->LeaseDenial(denial);
			std::lock_guard lock(g_mutex_handles_mutex);
			g_retired_mutex_prefixes.push_back(prefix);
			return KERNEL_ERROR_EAGAIN;
		}
	} else
	{
		if (g_mutex_error_trace != nullptr) g_mutex_error_trace->Stage(MutexErrorStage::BackingAllocation);
		const auto allocated = allocate_pthread_guest_backing(16 * 1024, allow_retry);
		if (allocated.error != OK) return allocated.error;
		prefix = reinterpret_cast<PthreadMutexGuestPrefix*>(allocated.address);
	}
	object->guest_prefix = prefix;
	*prepared = std::move(candidate);
	return OK;
}

static String pthread_name_snapshot(Pthread thread)
{
	std::lock_guard lock(thread->name_mutex);
	return thread->name;
}

static int claim_pthread_join(Pthread thread)
{
	if (thread == nullptr) return KERNEL_ERROR_EINVAL;
	if (thread->free) return KERNEL_ERROR_ESRCH;
	bool expected = false;
	return thread->join_claimed.compare_exchange_strong(expected, true) ? OK : KERNEL_ERROR_EINVAL;
}

struct PthreadJoinClaim
{
	Pthread thread;
	bool release = true;
	~PthreadJoinClaim() { if (release) thread->join_claimed = false; }
};

static int join_claimed_pthread(Pthread thread, void** value);

enum class PthreadOnceState : uint8_t
{
	Ready,
	Running,
	Complete,
};

struct PthreadOnceRecord
{
	uint64_t                address = 0;
	Loader::Program*        program = nullptr;
	std::mutex              mutex;
	std::condition_variable changed;
	PthreadOnceState        state = PthreadOnceState::Ready;
	int                     owner_thread = -1;
};

static std::mutex                                      g_pthread_once_registry_mutex;
static std::vector<std::shared_ptr<PthreadOnceRecord>> g_pthread_once_records;

static std::shared_ptr<PthreadOnceRecord> find_or_create_pthread_once_record(uint64_t address)
{
	// RuntimeLinker unload invokes PthreadDeleteStaticObjects while holding its
	// own mutex. Resolve ownership before taking the once registry mutex so the
	// two lock orders cannot form a cycle.
	auto* program = Core::Singleton<Loader::RuntimeLinker>::Instance()->FindProgramByAddr(address);
	std::lock_guard<std::mutex> lock(g_pthread_once_registry_mutex);
	for (const auto& record: g_pthread_once_records)
	{
		if (record->address == address) return record;
	}
	auto record     = std::make_shared<PthreadOnceRecord>();
	record->address = address;
	record->program = program;
	g_pthread_once_records.push_back(record);
	return record;
}

static void delete_pthread_once_records(Loader::Program* program)
{
	if (program == nullptr) return;
	std::lock_guard<std::mutex> lock(g_pthread_once_registry_mutex);
	g_pthread_once_records.erase(
	    std::remove_if(g_pthread_once_records.begin(), g_pthread_once_records.end(),
	                   [program](const auto& record) { return record->program == program; }),
	    g_pthread_once_records.end());
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
static bool reset_pthread_once_records()
{
	std::lock_guard<std::mutex> lock(g_pthread_once_registry_mutex);
	for (const auto& record: g_pthread_once_records)
	{
		std::lock_guard<std::mutex> record_lock(record->mutex);
		if (record->state == PthreadOnceState::Running) return false;
	}
	g_pthread_once_records.clear();
	return true;
}
#endif

struct PthreadRwlockPrivate
{
	uint8_t          reserved[256];
	String           name;
	pthread_rwlock_t p;
};

struct PthreadRwlockattrPrivate
{
	uint8_t              reserved[64];
	int                  type;
	pthread_rwlockattr_t p;
};

struct PthreadCondattrPrivate
{
	uint8_t            reserved[64];
	pthread_condattr_t p;
};

// Platforms whose condition wait is served by the bounded wakeup-generation
// protocol instead of a single native block. A broadcast or signal bumps the
// generation so a waiter can never lose a wakeup while it is between slices.
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME))
#define LUMEN_PTHREAD_COND_WAKEUP_GENERATION 1
#endif

struct PthreadCondPrivate
{
	uint8_t        reserved[256];
	String         name;
	pthread_cond_t p;
#if defined(LUMEN_PTHREAD_COND_WAKEUP_GENERATION)
	std::atomic_uint64_t signal_generation = 0;
#endif
};

enum class ConditionTraceKind
{
	Lifecycle,
	Wake,
	Rejected
};

static void trace_condition(ConditionTraceKind kind, const char* operation, const char* branch,
	                        uint64_t slot, uint64_t handle, const char* result_kind, int result)
{
	static const bool enabled = [] {
		const char* flag = std::getenv("LUMEN_X64_HOST_ERROR_TRACE");
		return flag != nullptr && flag[0] == '1' && flag[1] == '\0';
	}();
	if (!enabled) return;

	// Successful wakes must not consume the lifecycle or rejection evidence.
	static std::atomic<uint32_t> lifecycle_remaining {4096};
	static std::atomic<uint32_t> wake_remaining {256};
	static std::atomic<uint32_t> rejected_remaining {512};
	auto& remaining = kind == ConditionTraceKind::Rejected ? rejected_remaining :
	                  kind == ConditionTraceKind::Wake ? wake_remaining : lifecycle_remaining;
	uint32_t available = remaining.load(std::memory_order_relaxed);
	while (available != 0 && !remaining.compare_exchange_weak(available, available - 1, std::memory_order_relaxed)) {}
	if (available == 0) return;

	static std::atomic<uint32_t> sequence {0};
	const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count();
	// Only numeric snapshots are used here; a wake or destruction may already
	// have ended the handle's lifetime before this diagnostic runs.
	std::fprintf(stderr,
	             "[PTHREAD_CONDITION_TRACE] index=%u monotonicMs=%lld thread=%d op=%s branch=%s "
	             "slot=0x%016" PRIx64 " handle=0x%016" PRIx64 " resultKind=%s result=0x%08x\n",
	             sequence.fetch_add(1, std::memory_order_relaxed) + 1,
	             static_cast<long long>(milliseconds), Core::Thread::GetThreadIdUnique(), operation, branch,
	             slot, handle, result_kind, static_cast<unsigned int>(result));
}

struct PthreadStaticObject
{
	enum class Type
	{
		Mutex,
		Cond,
		Rwlock
	};

	Type             type;
	uint64_t         vaddr;
	Loader::Program* program;
};

class PthreadStaticObjects
{
public:
	PthreadStaticObjects() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~PthreadStaticObjects() { LUMEN_NOT_IMPLEMENTED; }

	LUMEN_CLASS_NO_COPY(PthreadStaticObjects);

	void* CreateObject(void* addr, PthreadStaticObject::Type type);
	int   ResolveCondition(PthreadCond* slot, PthreadCond* object);
	int   ResolveMutex(PthreadMutex* slot, PthreadMutex* object, bool pin = false);
	void  DeleteObjects(Loader::Program* program);
	bool  ResetForNextSession();
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
	bool  MutexInitializationContractTest();
#endif

private:
	void* CreateObjectLocked(void* addr, PthreadStaticObject::Type type, int* error = nullptr);
	int   ResolveObject(void* slot, PthreadStaticObject::Type type, void** object);
	Loader::Program* FindSlotProgram(uint64_t address);

	Vector<PthreadStaticObject*> m_objects;
	Core::Mutex                  m_mutex;
	uint64_t                     m_retirement_generation = 0;
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
	Loader::Program* (*m_program_test)(void*, uint64_t) = nullptr;
	void (*m_preparation_test)(void*, void*) = nullptr;
	void* m_initialization_test_context = nullptr;
#endif
};

class PthreadKeys
{
public:
	PthreadKeys() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~PthreadKeys() { LUMEN_NOT_IMPLEMENTED; }

	LUMEN_CLASS_NO_COPY(PthreadKeys);

	bool Create(int* key, pthread_key_destructor_func_t destructor);
	bool Delete(int key);
	void Destruct(int thread_id, bool execute_callbacks);
	bool Set(int key, void* data);
	bool Get(int key, void** data);
	void ResetForNextSession();

private:
	struct SpecificValue
	{
		uint64_t generation = 0;
		void* data = nullptr;
	};

	struct Key
	{
		// Odd generations are live. Delete/reset invalidate every thread's
		// value without touching storage owned by another native thread.
		std::atomic<uint64_t>          generation {0};
		pthread_key_destructor_func_t destructor = nullptr;
	};

	static_assert(std::atomic<uint64_t>::is_always_lock_free);
	static thread_local std::array<SpecificValue, KEYS_MAX> s_specific_values;
	Core::Mutex m_mutex;
	Key         m_keys[KEYS_MAX];
};

thread_local std::array<PthreadKeys::SpecificValue, KEYS_MAX> PthreadKeys::s_specific_values {};

class PthreadPool
{
public:
	PthreadPool() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~PthreadPool() { LUMEN_NOT_IMPLEMENTED; }

	LUMEN_CLASS_NO_COPY(PthreadPool);

	Pthread Create(int* error = nullptr);
	// Completed joinable wrappers still resolve for lifecycle APIs. Exception
	// delivery requires a running wrapper. Free or unknown handles never resolve.
	Pthread Resolve(uint64_t handle, bool allow_completed);

	void FreeDetachedThreads();
	bool ResetForNextSession();

private:
	Vector<Pthread> m_threads;
	Core::Mutex     m_mutex;
};

class PThreadContext
{
public:
	PThreadContext() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~PThreadContext() { LUMEN_NOT_IMPLEMENTED; }

	LUMEN_CLASS_NO_COPY(PThreadContext);

	PthreadAttr*          GetDefaultAttr() { return &m_default_attr; }
	void                  SetDefaultAttr(PthreadAttr attr) { m_default_attr = attr; }
	PthreadCondattr*      GetDefaultCondattr() { return &m_default_condattr; }
	void                  SetDefaultCondattr(PthreadCondattr attr) { m_default_condattr = attr; }
	PthreadMutexattr*     GetDefaultMutexattr() { return &m_default_mutexattr; }
	void                  SetDefaultMutexattr(PthreadMutexattr attr) { m_default_mutexattr = attr; }
	PthreadRwlockattr*    GetDefaultRwlockattr() { return &m_default_rwlockattr; }
	void                  SetDefaultRwlockattr(PthreadRwlockattr attr) { m_default_rwlockattr = attr; }
	PthreadPool*          GetPthreadPool() { return m_pthread_pool; }
	void                  SetPthreadPool(PthreadPool* pool) { m_pthread_pool = pool; }
	PthreadStaticObjects* GetPthreadStaticObjects() { return m_pthread_static_objects; }
	void                  SetPthreadStaticObjects(PthreadStaticObjects* objs) { m_pthread_static_objects = objs; }
	PthreadKeys*          GetPthreadKeys() { return m_pthread_keys; }
	void                  SetPthreadKeys(PthreadKeys* keys) { m_pthread_keys = keys; }

	[[nodiscard]] thread_dtors_func_t GetThreadDtors() const { return m_thread_dtors; }
	void                              SetThreadDtors(thread_dtors_func_t dtors) { m_thread_dtors = dtors; }

private:
	// Core::Mutex           m_mutex;
	PthreadMutexattr      m_default_mutexattr      = nullptr;
	PthreadRwlockattr     m_default_rwlockattr     = nullptr;
	PthreadCondattr       m_default_condattr       = nullptr;
	PthreadAttr           m_default_attr           = nullptr;
	PthreadPool*          m_pthread_pool           = nullptr;
	PthreadStaticObjects* m_pthread_static_objects = nullptr;
	PthreadKeys*          m_pthread_keys           = nullptr;

	std::atomic<thread_dtors_func_t> m_thread_dtors = nullptr;
};

thread_local Pthread g_pthread_self    = nullptr;
static thread_local bool g_pthread_cleanup_active = false;
PThreadContext*      g_pthread_context = nullptr;

// Records published by PthreadInitSelfForMainThread: the runtime Execute
// thread and, in host tests, additional threads that own a Pthread context.
// Handle resolution runs on the raising thread, so the published set is
// mutex-guarded and never dereferences an unresolved value.
static std::mutex           g_published_threads_mutex;
static std::vector<Pthread> g_published_threads;

static Pthread resolve_thread_handle(uint64_t handle, bool allow_completed = false)
{
	if (handle == 0) return nullptr;
	{
		std::lock_guard lock(g_published_threads_mutex);
		for (auto* thread: g_published_threads)
		{
			if (reinterpret_cast<uint64_t>(pthread_guest_handle(thread)) != handle) continue;
			return !thread->free && (allow_completed || !thread->almost_done) ? thread : nullptr;
		}
	}
	if (g_pthread_context == nullptr) return nullptr;
	return g_pthread_context->GetPthreadPool()->Resolve(handle, allow_completed);
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
// --- Local capture diagnostics: published guest-thread boundary states --------
// One bounded lock-free slot per live guest thread. Each thread seals its most
// recent boundary state (16 x86-64 GP words, RIP, the HLE import it entered and
// the private x86-64 stack allocation that owns RSP) into its own slot at run
// entry, HLE boundaries and exception delivery, so the deliver/handled points
// of the exception delivery can report every guest thread without taking the
// pthread pool lock. Selector LUMEN_MACOS_VM_ROOT_SNAPSHOT gates claim and
// publication; with the selector off every path here is a no-op. The guest
// handle is published for the record attribution regardless of that selector.
namespace
{
constexpr size_t kGuestRootSlotCount = 64;

struct GuestRootSlot
{
	std::atomic<uint32_t> sequence {0};
	std::atomic<uint64_t> handle {0};
	std::atomic<uint32_t> uniqueId {0};
	std::atomic<uint32_t> valid {0};
	std::atomic<uint64_t> rip {0};
	std::atomic<uint64_t> hostCall {0};
	std::atomic<uint64_t> stackBase {0};
	std::atomic<uint64_t> stackSize {0};
	std::array<std::atomic<uint64_t>, 16> gprs {};
};

std::array<GuestRootSlot, kGuestRootSlotCount> g_guest_root_slots {};
thread_local GuestRootSlot* g_guest_root_slot = nullptr;

GuestRootSlot* guest_root_slot_for_current_thread()
{
	if (g_guest_root_slot != nullptr) return g_guest_root_slot;
	if (!Core::VirtualMemory::GuestRootSnapshotEnabled() || g_pthread_self == nullptr) return nullptr;
	const uint64_t handle = reinterpret_cast<uint64_t>(pthread_guest_handle(g_pthread_self));
	for (auto& slot: g_guest_root_slots)
	{
		uint64_t expected = 0;
		if (!slot.handle.compare_exchange_strong(expected, handle, std::memory_order_acq_rel)) continue;
		slot.uniqueId.store(static_cast<uint32_t>(g_pthread_self->unique_id), std::memory_order_relaxed);
		slot.valid.store(0, std::memory_order_relaxed);
		g_guest_root_slot = &slot;
		return &slot;
	}
	return nullptr;
}

// End-of-thread invalidation under the slot's own sequence; the slot stays
// claimed until the wrapper is recycled so a reader never sees a dead state.
void invalidate_guest_root_slot(Pthread thread)
{
	if (thread == nullptr) return;
	const uint64_t handle = reinterpret_cast<uint64_t>(pthread_guest_handle(thread));
	for (auto& slot: g_guest_root_slots)
	{
		if (slot.handle.load(std::memory_order_acquire) != handle) continue;
		const uint32_t sequence = slot.sequence.load(std::memory_order_relaxed);
		slot.sequence.store(sequence + 1, std::memory_order_relaxed);
		slot.valid.store(0, std::memory_order_relaxed);
		slot.sequence.store(sequence + 2, std::memory_order_release);
		return;
	}
}

void release_guest_root_slot(Pthread thread)
{
	if (thread == nullptr) return;
	const uint64_t handle = reinterpret_cast<uint64_t>(pthread_guest_handle(thread));
	for (auto& slot: g_guest_root_slots)
	{
		if (slot.handle.load(std::memory_order_acquire) != handle) continue;
		invalidate_guest_root_slot(thread);
		slot.handle.store(0, std::memory_order_release);
		return;
	}
}

// Thread attach: bind the guest handle used by the record attribution and
// claim this thread's slot when the root snapshot selector is on.
void attach_guest_root_diagnostics()
{
	if (g_pthread_self == nullptr) return;
	Core::VirtualMemory::SetCaptureDiagnosticGuestThread(reinterpret_cast<uint64_t>(pthread_guest_handle(g_pthread_self)));
	(void)guest_root_slot_for_current_thread();
}

} // namespace

void KernelGuestThreadPublishBoundaryState(const uint64_t gpr[16], uint64_t rip,
                                           uint64_t hostCall, uint64_t stackBase,
                                           uint64_t stackSize)
{
	if (gpr == nullptr || !Core::VirtualMemory::GuestRootSnapshotEnabled()) return;
	GuestRootSlot* slot = guest_root_slot_for_current_thread();
	if (slot == nullptr) return;
	const uint32_t sequence = slot->sequence.load(std::memory_order_relaxed);
	slot->sequence.store(sequence + 1, std::memory_order_relaxed);
	for (size_t index = 0; index < slot->gprs.size(); ++index)
		slot->gprs[index].store(gpr[index], std::memory_order_relaxed);
	slot->rip.store(rip, std::memory_order_relaxed);
	slot->hostCall.store(hostCall, std::memory_order_relaxed);
	slot->stackBase.store(stackBase, std::memory_order_relaxed);
	slot->stackSize.store(stackSize, std::memory_order_relaxed);
	slot->valid.store(1, std::memory_order_relaxed);
	slot->sequence.store(sequence + 2, std::memory_order_release);
}

namespace
{
// Deliver/handled point: report every claimed slot's most recent published
// state. A slot being written, or already invalidated, is skipped.
void snapshot_guest_thread_roots(const char* tag)
{
	if (tag == nullptr || !Core::VirtualMemory::GuestRootSnapshotEnabled()) return;
	size_t claimed = 0;
	size_t reported = 0;
	for (auto& slot: g_guest_root_slots)
	{
		if (slot.handle.load(std::memory_order_acquire) == 0) continue;
		++claimed;
		const uint32_t sequence = slot.sequence.load(std::memory_order_acquire);
		if ((sequence & 1u) != 0) continue;
		if (slot.valid.load(std::memory_order_relaxed) == 0) continue;
		uint64_t gpr[16] {};
		for (size_t index = 0; index < 16; ++index)
			gpr[index] = slot.gprs[index].load(std::memory_order_relaxed);
		const uint64_t rip = slot.rip.load(std::memory_order_relaxed);
		const uint64_t hostCall = slot.hostCall.load(std::memory_order_relaxed);
		const uint64_t stackBase = slot.stackBase.load(std::memory_order_relaxed);
		const uint64_t stackSize = slot.stackSize.load(std::memory_order_relaxed);
		const uint64_t handle = slot.handle.load(std::memory_order_relaxed);
		const uint32_t uniqueId = slot.uniqueId.load(std::memory_order_relaxed);
		if (slot.sequence.load(std::memory_order_acquire) != sequence) continue;
		++reported;
		Core::VirtualMemory::ReportGuestThreadRoots(tag, uniqueId, handle, hostCall,
		                                            stackBase, stackSize, gpr, rip);
	}
	Core::VirtualMemory::ReportGuestRootSnapshotSummary(tag, claimed, reported);
}
} // namespace
#endif

#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
static std::atomic<PthreadExitTestHook> g_after_cleanup_test_hook {nullptr};
static std::atomic<PthreadExitTestHook> g_before_native_join_test_hook {nullptr};

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
uint64_t LUMEN_SYSV_ABI PthreadBackingAllocationFexContractTest(uint64_t scenario)
{
	// Entered by original guest code through the real HLE bridge. Unlike the
	// policy fixture below, neither allocation nor denial metadata is modeled.
	using namespace Core::VirtualMemory;
	if (scenario > 2 || Loader::TryGetActiveHostCallGuestGprs() == nullptr) return 0;
	const bool allow_retry = scenario != 1;
	const bool retain_lease = scenario == 2;
	const auto lease_backing = Alloc(0, GUEST_PTHREAD_BACKING_SIZE, Mode::ReadWrite);
	if (lease_backing == 0) return 0;
	std::atomic<bool> writer_published {false};
	bool writer_closed = false;
	std::thread writer([&] {
		const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV3(
		    LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2, nullptr);
		if (token != 0)
			writer_closed = LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0,
			    [](void* opaque) noexcept { static_cast<std::atomic<bool>*>(opaque)->store(true, std::memory_order_release); },
			    &writer_published) == 1;
	});
	bool conflict = false, valid = true;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (std::chrono::steady_clock::now() < deadline)
	{
		uint32_t retryable = 0;
		const auto probe = LumenAndroidFexCoreBeginGuestMemoryMutationV3(
		    LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_ADDITIVE_MAPPING_V2, &retryable);
		if (probe == 0)
		{
			conflict = retryable == 1;
			break;
		}
		valid = LumenAndroidFexCoreEndGuestMemoryMutationV2(probe, 0, 0, 0, nullptr, nullptr) == 1 && valid;
		std::this_thread::yield();
	}
	PthreadBackingAllocation result;
	uint64_t generation = GetMutationGeneration();
	{
		GuestDataLease lease;
		valid = (!retain_lease || TryAcquireGuestDataLease(lease_backing, 8, Mode::Read, &lease, false)) && valid;
		if (conflict && valid)
		{
			AllocationDenialMetadata denial {AllocationFailure::HostAllocationFailed, true};
			const auto denied = Alloc(0, GUEST_PTHREAD_BACKING_SIZE, Mode::ReadWrite, &denial);
			valid = denied == 0 && denial.reason == AllocationFailure::AdmissionDenied &&
			    denial.retryable == !retain_lease && GetMutationGeneration() == generation &&
			    !writer_published.load(std::memory_order_acquire) && valid;
			if (denied != 0) result.address = denied;
			else result = allocate_pthread_guest_backing(GUEST_PTHREAD_BACKING_SIZE, allow_retry);
			const bool succeeds = allow_retry && !retain_lease;
			valid = result.error == (succeeds ? OK : KERNEL_ERROR_EAGAIN) &&
			    result.attempts == (succeeds ? 2u : 1u) && (result.address != 0) == succeeds &&
			    GetMutationGeneration() == generation + (succeeds ? 1u : 0u) &&
			    writer_published.load(std::memory_order_acquire) == succeeds && valid;
		}
	}
	// Also balance a failed assertion: no lease is carried into donation, and
	// the peer must finish before any native join or backing cleanup.
	{
		Loader::ScopedGuestHostWait wait;
		valid = wait.IsActive() && valid;
		writer.join();
	}
	valid = conflict && writer_closed && writer_published.load(std::memory_order_acquire) &&
	    !CurrentThreadHasMemoryLease() && valid;
	if (result.address != 0)
	{
		{
			GuestDataLease lease;
			const bool readable = TryAcquireGuestDataLease(result.address, GUEST_PTHREAD_BACKING_SIZE, Mode::Read, &lease);
			valid = readable && valid;
			if (readable)
			{
				const auto* bytes = reinterpret_cast<const uint8_t*>(result.address);
				for (uint64_t i = 0; i < GUEST_PTHREAD_BACKING_SIZE; ++i) valid = bytes[i] == 0 && valid;
			}
		}
		valid = Free(result.address) && valid;
	}
	valid = Free(lease_backing) && valid;
	std::fprintf(stderr, "[PTHREAD_BACKING_ALLOCATION_FEX_TEST] case=%" PRIu64
	    " conflict=%u attempts=%u real-core-alloc=1 real-hle=1 reader-restored=%u pass=%u\n",
	    scenario, conflict ? 1u : 0u, result.attempts, writer_closed ? 1u : 0u, valid ? 1u : 0u);
	return valid ? 1 : 0;
}
#endif

bool PthreadBackingAllocationContractTest()
{
	using Failure = Core::VirtualMemory::AllocationFailure;
	using Denial = Core::VirtualMemory::AllocationDenialMetadata;
	enum class Case { Immediate, Contention, PermanentAdmission, HostFailure, Invalid, Unknown, InactiveWait, Deadline, AttemptLimit, RetryDisabled };
	uint32_t cases = 0;
	for (const auto scenario: {Case::Immediate, Case::Contention, Case::PermanentAdmission, Case::HostFailure,
	    Case::Invalid, Case::Unknown, Case::InactiveWait, Case::Deadline, Case::AttemptLimit, Case::RetryDisabled})
	{
		uint32_t attempts = 0, waits = 0;
		auto now = std::chrono::steady_clock::time_point {};
		const auto result = allocate_pthread_guest_backing(GUEST_PTHREAD_BACKING_SIZE, scenario != Case::RetryDisabled,
		    [&](uint64_t bytes, Denial* denial) -> uint64_t {
			    ++attempts;
			    if (scenario == Case::Immediate || (scenario == Case::Contention && attempts == 3))
				    return Core::VirtualMemory::Alloc(0, bytes, Core::VirtualMemory::Mode::ReadWrite, denial);
			    denial->reason = scenario == Case::HostFailure ? Failure::HostAllocationFailed :
			        scenario == Case::Invalid ? Failure::InvalidArgument :
			        scenario == Case::Unknown ? Failure::None : Failure::AdmissionDenied;
			    // A stray flag cannot make invalid/host/unknown failures retryable.
			    denial->retryable = scenario != Case::PermanentAdmission;
			    return 0;
		    }, [&] {
			    ++waits;
			    if (scenario == Case::Deadline) now += std::chrono::seconds(3);
			    return scenario != Case::InactiveWait;
		    }, [&] { return now; });
		const bool succeeds = scenario == Case::Immediate || scenario == Case::Contention;
		const uint32_t expected_attempts = scenario == Case::Contention ? 3u : scenario == Case::AttemptLimit ? 4096u : 1u;
		const uint32_t expected_waits = scenario == Case::Contention ? 2u : scenario == Case::AttemptLimit ? 4095u :
		    scenario == Case::InactiveWait || scenario == Case::Deadline ? 1u : 0u;
		const int expected_error = succeeds ? OK : scenario == Case::HostFailure ? KERNEL_ERROR_ENOMEM :
		    scenario == Case::Invalid ? KERNEL_ERROR_EINVAL : KERNEL_ERROR_EAGAIN;
		bool valid = (result.address != 0) == succeeds && result.error == expected_error &&
		    result.attempts == expected_attempts && attempts == expected_attempts && waits == expected_waits;
		if (result.address != 0)
		{
			// Successful attempts use the real Core allocation and zero-filled
			// backing. The surrounding cases exercise only scheduling/failure input.
			const auto* bytes = reinterpret_cast<const uint8_t*>(result.address);
			for (uint64_t i = 0; i < GUEST_PTHREAD_BACKING_SIZE; ++i) valid = valid && bytes[i] == 0;
			valid = Core::VirtualMemory::Free(result.address) && valid;
		}
		if (!valid)
		{
			std::fprintf(stderr, "[PTHREAD_BACKING_ALLOCATION_TEST_FAIL] case=%u attempts=%u waits=%u error=0x%08x\n",
			    static_cast<unsigned>(scenario), attempts, waits, static_cast<unsigned>(result.error));
			return false;
		}
		++cases;
	}
	// The real barrier refuses the first additive request while an exclusive
	// writer needs this HLE reader. The retry helper must donate that reader,
	// readmit it, and only then acquire a fresh additive mutation.
	using Barrier = Android::FexCoreLab::GuestMemoryMutationBarrier;
	using Scope = Android::FexCoreLab::GuestMemoryMutationScope;
	Barrier barrier;
	if (!barrier.EnterExecution() || !barrier.SuspendExecutionForHostCall()) return false;
	bool writer_completed = false;
	std::atomic_bool writer_armed = false;
	std::thread writer([&] {
		const auto token = barrier.BeginMutation(Scope::Exclusive, [&](uint64_t) {
			writer_armed.store(true, std::memory_order_release);
			return true;
		}, [](uint64_t) {},
		    std::chrono::seconds(2));
		writer_completed = token != 0 && barrier.EndMutation(token);
	});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (!writer_armed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	const bool pending = writer_armed.load(std::memory_order_acquire);
	uint32_t waits = 0;
	bool admitted_before_allocate = true;
	const auto result = allocate_pthread_guest_backing(GUEST_PTHREAD_BACKING_SIZE, true,
	    [&](uint64_t, Denial* denial) -> uint64_t {
		    admitted_before_allocate = admitted_before_allocate && barrier.CurrentThreadOwnsHostMemoryReadLease();
		    Barrier::MutationDenialDiagnostic observed {};
		    const auto token = barrier.BeginMutation(Scope::AdditiveMapping, [](uint64_t) { return true; }, [](uint64_t) {},
		        std::chrono::seconds(1), &observed);
		    if (token == 0)
		    {
			    denial->reason = Failure::AdmissionDenied;
			    denial->retryable = observed.reason == Barrier::MutationDenialReason::CompetingExclusiveMutation;
			    return 0;
		    }
		    return barrier.EndMutation(token) ? UINT64_C(0x1000) : 0;
	    }, [&] {
		    ++waits;
		    Android::FexCoreLab::QuiescentHostWaitLease host_wait(barrier);
		    return host_wait.Acquired();
	    }, [] { return std::chrono::steady_clock::now(); });
	const bool resumed = barrier.ResumeExecutionAfterHostCall();
	barrier.LeaveExecution();
	writer.join();
	if (!pending || !writer_completed || !resumed || !admitted_before_allocate || result.error != OK ||
	    result.attempts != 2 || waits != 1 || result.address != 0x1000 || barrier.CurrentThreadOwnsHostMemoryReadLease())
		return false;
	std::fprintf(stderr, "[PTHREAD_BACKING_ALLOCATION_TEST] cases=%u typed-helper=checked real-allocation/zeroing=checked"
	    " permanent-errors=not-retried bounds=deadline/attempts inactive-wait=denied real-writer/readmission=checked\n", cases + 1);
	return true;
}

void PthreadSetExitTestHooks(PthreadExitTestHook after_cleanup, PthreadExitTestHook before_native_join)
{
	g_after_cleanup_test_hook = after_cleanup;
	g_before_native_join_test_hook = before_native_join;
}

void PthreadReapDetachedThreadsForTest()
{
	EXIT_IF(g_pthread_context == nullptr);
	g_pthread_context->GetPthreadPool()->FreeDetachedThreads();
}
#endif

static void FreeDetachedThreads(void* /*arg*/)
{
	PRINT_NAME_ENABLE(false);

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_pool = g_pthread_context->GetPthreadPool();

	EXIT_IF(pthread_pool == nullptr);

	while (true)
	{
		Core::Thread::Sleep(10000);
		pthread_pool->FreeDetachedThreads();
	}
}

void PthreadDeleteStaticObjects(Loader::Program* program)
{
	delete_pthread_once_records(program);
	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	pthread_static_objects->DeleteObjects(program);
}

void PthreadInitSelfForMainThread()
{
	EXIT_IF(g_pthread_self != nullptr);

	g_pthread_self = new PthreadPrivate {};
	const int prefix_error = prepare_pthread_guest_prefix(g_pthread_self);
	if (prefix_error != OK) EXIT("Main pthread guest backing allocation failed: 0x%08x\n", static_cast<unsigned>(prefix_error));
	PthreadAttrInit(&g_pthread_self->attr);
	g_pthread_self->p           = pthread_self();
	g_pthread_self->effective_guest_policy = g_pthread_self->attr->policy;
	g_pthread_self->guest_sched_priority = g_pthread_self->attr->guest_sched_priority;
	{
		std::lock_guard lock(g_pthread_self->name_mutex);
		g_pthread_self->name = "MainThread";
	}
	g_pthread_self->unique_id   = Core::Thread::GetThreadIdUnique();
	g_pthread_self->free        = false;
	g_pthread_self->detached    = false;
	g_pthread_self->host_detached = false;
	g_pthread_self->almost_done = false;
	g_pthread_self->entry       = nullptr;
	g_pthread_self->arg         = nullptr;
	{
		std::lock_guard lock(g_published_threads_mutex);
		g_published_threads.push_back(g_pthread_self);
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	attach_guest_root_diagnostics();
#endif
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
void PthreadFinishSelfForMainThread()
{
	if (g_pthread_self == nullptr)
	{
		return;
	}
	if (g_pthread_self->attr != nullptr)
	{
		PthreadAttrDestroy(&g_pthread_self->attr);
	}
	{
		std::lock_guard lock(g_published_threads_mutex);
		g_published_threads.erase(
		    std::remove(g_published_threads.begin(), g_published_threads.end(), g_pthread_self),
		    g_published_threads.end());
	}
	g_pthread_self->cleanup_handlers.clear();
	// Execute and its guest callbacks have returned. Remove every TCB that
	// references this handle before releasing the main-thread guest backing.
	Core::Singleton<Loader::RuntimeLinker>::Instance()->DeleteTlss(g_pthread_self->unique_id);
	EXIT_IF(!Core::VirtualMemory::Free(reinterpret_cast<uint64_t>(g_pthread_self->guest_prefix)));
	delete g_pthread_self;
	g_pthread_self = nullptr;
}
#endif

LUMEN_SUBSYSTEM_INIT(Pthread)
{
	PRINT_NAME_ENABLE(false);

	EXIT_IF(g_pthread_context != nullptr);

	g_pthread_context = new PThreadContext;

	g_pthread_context->SetPthreadStaticObjects(new PthreadStaticObjects);
	g_pthread_context->SetPthreadPool(new PthreadPool);
	g_pthread_context->SetPthreadKeys(new PthreadKeys);

	PthreadMutexattr  default_mutexattr  = nullptr;
	PthreadRwlockattr default_rwlockattr = nullptr;
	PthreadCondattr   default_condattr   = nullptr;
	PthreadAttr       default_attr       = nullptr;

	PthreadAttrInit(&default_attr);
	PthreadMutexattrInit(&default_mutexattr);
	PthreadRwlockattrInit(&default_rwlockattr);
	PthreadCondattrInit(&default_condattr);

	g_pthread_context->SetDefaultMutexattr(default_mutexattr);
	g_pthread_context->SetDefaultRwlockattr(default_rwlockattr);
	g_pthread_context->SetDefaultCondattr(default_condattr);
	g_pthread_context->SetDefaultAttr(default_attr);

	PRINT_NAME_ENABLE(true);

	Core::Thread thread(FreeDetachedThreads, nullptr);
	thread.Detach();
}

LUMEN_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Pthread) {}

LUMEN_SUBSYSTEM_DESTROY(Pthread) {}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
bool PthreadResetForNextSession()
{
	if (g_pthread_context == nullptr || !Loader::GuestExecutionIsIdle())
	{
		return false;
	}

	auto* pool = g_pthread_context->GetPthreadPool();
	auto* keys = g_pthread_context->GetPthreadKeys();
	auto* static_objects = g_pthread_context->GetPthreadStaticObjects();
	if (pool == nullptr || keys == nullptr || static_objects == nullptr || !pool->ResetForNextSession() ||
	    !static_objects->ResetForNextSession() || !reset_pthread_once_records())
	{
		return false;
	}

	keys->ResetForNextSession();
	g_pthread_context->SetThreadDtors(nullptr);
	return Loader::GuestExecutionIsIdle();
}
#endif

// These helpers receive an already resolved private native object. The caller
// owns the output storage; public entry points retain guest slot/output admission.
static int PthreadAttrGetaffinityHost(const PthreadAttrPrivate& attr, KernelCpumask& mask)
{
	mask = attr.affinity.load(std::memory_order_relaxed);
	return OK;
}

static int PthreadAttrGetdetachstateHost(const PthreadAttrPrivate& attr, int& state)
{
	state = attr.detached ? 1 : 0;
	return OK;
}

static int PthreadAttrGetguardsizeHost(const PthreadAttrPrivate& attr, size_t& guard_size)
{
	guard_size = attr.guard_size;
	return OK;
}

static int PthreadAttrGetinheritschedHost(const PthreadAttrPrivate& attr, int& inherit_sched)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	// Bionic's accessors require API 28; preserve the guest metadata on API 26.
	inherit_sched = attr.inherit_sched;
	int result = 0;
#else
	int result = pthread_attr_getinheritsched(&attr.p, &inherit_sched);
#endif
	switch (inherit_sched)
	{
		case PTHREAD_EXPLICIT_SCHED: inherit_sched = 0; break;
		case PTHREAD_INHERIT_SCHED: inherit_sched = 4; break;
		default: EXIT("unknown inherit_sched: %d\n", inherit_sched);
	}
	return result == 0 ? OK : KERNEL_ERROR_EINVAL;
}

static int PthreadAttrGetschedparamHost(const PthreadAttrPrivate& attr, KernelSchedParam& param)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	int result = 0;
#else
	sched_param native_param {};
	int result = pthread_attr_getschedparam(&attr.p, &native_param);
#endif
	if (result != 0) return KERNEL_ERROR_EINVAL;
	// Native scheduling may quantize priorities; report the exact guest value.
	param.sched_priority = attr.guest_sched_priority;
	return OK;
}

static int PthreadAttrGetschedpolicyHost(const PthreadAttrPrivate& attr, int& policy)
{
	int result = pthread_attr_getschedpolicy(&attr.p, &policy);
	switch (policy)
	{
		case SCHED_OTHER: policy = attr.policy; break;
		case SCHED_FIFO: policy = 1; break;
		case SCHED_RR: policy = 3; break;
		default: EXIT("unknown policy: %d\n", policy);
	}
	return result == 0 ? OK : KERNEL_ERROR_EINVAL;
}

static int PthreadAttrGetstackaddrHost(const PthreadAttrPrivate& attr, void*& stack_addr)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || defined(LUMEN_MACOS_FEXCORE_SEPARATE_PTHREAD_STACK)
	stack_addr = attr.guest_stack_addr;
	return OK;
#else
	return pthread_attr_getstackaddr(&attr.p, &stack_addr) == 0 ? OK : KERNEL_ERROR_EINVAL;
#endif
}

static int PthreadAttrGetstacksizeHost(const PthreadAttrPrivate& attr, size_t& stack_size)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || defined(LUMEN_MACOS_FEXCORE_SEPARATE_PTHREAD_STACK)
	stack_size = attr.guest_stack_size;
	return OK;
#else
	return pthread_attr_getstacksize(&attr.p, &stack_size) == 0 ? OK : KERNEL_ERROR_EINVAL;
#endif
}

static int pthread_attr_copy(PthreadAttr* dst, const PthreadAttr* src)
{
	if (dst == nullptr || *dst == nullptr || src == nullptr || *src == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	KernelCpumask    mask          = 0;
	int              state         = 0;
	size_t           guard_size    = 0;
	int              inherit_sched = 0;
	KernelSchedParam param         = {};
	int              policy        = 0;
	void*            stack_addr    = nullptr;
	size_t           stack_size    = 0;

	int result = 0;

	result = (result == 0 ? PthreadAttrGetaffinityHost(**src, mask) : result);
	result = (result == 0 ? PthreadAttrGetdetachstateHost(**src, state) : result);
	result = (result == 0 ? PthreadAttrGetguardsizeHost(**src, guard_size) : result);
	result = (result == 0 ? PthreadAttrGetinheritschedHost(**src, inherit_sched) : result);
	result = (result == 0 ? PthreadAttrGetschedparamHost(**src, param) : result);
	result = (result == 0 ? PthreadAttrGetschedpolicyHost(**src, policy) : result);
	result = (result == 0 ? PthreadAttrGetstackaddrHost(**src, stack_addr) : result);
	result = (result == 0 ? PthreadAttrGetstacksizeHost(**src, stack_size) : result);

	result = (result == 0 ? PthreadAttrSetaffinity(dst, mask) : result);
	result = (result == 0 ? PthreadAttrSetdetachstate(dst, state) : result);
	result = (result == 0 ? PthreadAttrSetguardsize(dst, guard_size) : result);
	result = (result == 0 ? PthreadAttrSetinheritsched(dst, inherit_sched) : result);
	result = (result == 0 ? PthreadAttrSetschedparam(dst, &param) : result);
	result = (result == 0 ? PthreadAttrSetschedpolicy(dst, policy) : result);
	if (stack_addr != nullptr)
	{
		result = (result == 0 ? PthreadAttrSetstackaddr(dst, stack_addr) : result);
	}
	if (stack_size != 0)
	{
		result = (result == 0 ? PthreadAttrSetstacksize(dst, stack_size) : result);
	}

	return result;
}

static void pthread_attr_dbg_print(const PthreadAttr* src)
{
	KernelCpumask    mask          = 0;
	int              state         = 0;
	size_t           guard_size    = 0;
	int              inherit_sched = 0;
	KernelSchedParam param         = {};
	int              policy        = 0;
	void*            stack_addr    = nullptr;
	size_t           stack_size    = 0;

	PthreadAttrGetaffinityHost(**src, mask);
	PthreadAttrGetdetachstateHost(**src, state);
	PthreadAttrGetguardsizeHost(**src, guard_size);
	PthreadAttrGetinheritschedHost(**src, inherit_sched);
	PthreadAttrGetschedparamHost(**src, param);
	PthreadAttrGetschedpolicyHost(**src, policy);
	PthreadAttrGetstackaddrHost(**src, stack_addr);
	PthreadAttrGetstacksizeHost(**src, stack_size);

	printf("\tcpu_mask       = 0x%" PRIx64 "\n", mask);
	printf("\tdetach_state   = %d\n", state);
	printf("\tguard_size     = %" PRIu64 "\n", guard_size);
	printf("\tinherit_sched  = %d\n", inherit_sched);
	printf("\tsched_priority = %d\n", param.sched_priority);
	printf("\tpolicy         = %d\n", policy);
	printf("\tstack_addr     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(stack_addr));
	printf("\tstack_size    = %" PRIu64 "\n", static_cast<uint64_t>(stack_size));
}

#if defined(LUMEN_PTHREAD_ATTRIBUTE_TEST_HOOK)
bool PthreadAttributeHostOutputContractTest()
{
	struct Attributes
	{
		PthreadAttr source = nullptr;
		PthreadAttr copy = nullptr;
		~Attributes()
		{
			if (source != nullptr) PthreadAttrDestroy(&source);
			if (copy != nullptr) PthreadAttrDestroy(&copy);
		}
	} attrs;
	if (PthreadAttrInit(&attrs.source) != OK || PthreadAttrInit(&attrs.copy) != OK) return false;
	constexpr KernelCpumask mask = UINT64_C(0x8040201008040201);
	constexpr size_t guard_size = 0x6000, stack_size = 0x100000;
	std::array<uint8_t, 16> stack_marker {};
	void* stack_addr = stack_marker.data();
	const KernelSchedParam priority {650};
	if (PthreadAttrSetaffinity(&attrs.source, mask) != OK ||
	    PthreadAttrSetdetachstate(&attrs.source, 1) != OK ||
	    PthreadAttrSetguardsize(&attrs.source, guard_size) != OK ||
	    PthreadAttrSetinheritsched(&attrs.source, 0) != OK ||
	    PthreadAttrSetschedparam(&attrs.source, &priority) != OK ||
	    PthreadAttrSetschedpolicy(&attrs.source, 3) != OK ||
	    PthreadAttrSetstackaddr(&attrs.source, stack_addr) != OK ||
	    PthreadAttrSetstacksize(&attrs.source, stack_size) != OK) return false;
	const auto check = [&](auto host_get, auto guest_get, auto expected, uint64_t guest_leases) {
		using T = decltype(expected);
		struct Output { uint64_t before; T value; uint64_t after; };
		Output host {0x123456789abcdef0u, {}, 0xfedcba9876543210u};
		Output guest = host;
		const auto admissions = g_argument_lease_acquisitions;
		if (host_get(*attrs.source, host.value) != OK || g_argument_lease_acquisitions != admissions ||
		    guest_get(&attrs.source, &guest.value) != OK ||
		    g_argument_lease_acquisitions != admissions + guest_leases ||
		    std::memcmp(&host.value, &expected, sizeof(T)) != 0 ||
		    std::memcmp(&guest.value, &expected, sizeof(T)) != 0 ||
		    host.before != guest.before || host.after != guest.after ||
		    host.before != 0x123456789abcdef0u || host.after != 0xfedcba9876543210u) return false;
		PthreadAttr missing = nullptr;
		return guest_get(nullptr, &guest.value) == KERNEL_ERROR_EINVAL &&
		    guest_get(&missing, &guest.value) == KERNEL_ERROR_EINVAL &&
		    guest_get(&attrs.source, nullptr) == KERNEL_ERROR_EINVAL &&
		    std::memcmp(&guest.value, &expected, sizeof(T)) == 0 &&
		    guest.before == host.before && guest.after == host.after;
	};
	if (!check(PthreadAttrGetaffinityHost, PthreadAttrGetaffinity, mask, 2) ||
	    !check(PthreadAttrGetdetachstateHost, PthreadAttrGetdetachstate, 1, 2) ||
	    !check(PthreadAttrGetguardsizeHost, PthreadAttrGetguardsize, guard_size, 2) ||
	    !check(PthreadAttrGetinheritschedHost, PthreadAttrGetinheritsched, 0, 2) ||
	    !check(PthreadAttrGetschedparamHost, PthreadAttrGetschedparam, priority, 0) ||
	    !check(PthreadAttrGetschedpolicyHost, PthreadAttrGetschedpolicy, 3, 2) ||
	    !check(PthreadAttrGetstackaddrHost, PthreadAttrGetstackaddr, stack_addr, 2) ||
	    !check(PthreadAttrGetstacksizeHost, PthreadAttrGetstacksize, stack_size, 2)) return false;
	const auto admissions = g_argument_lease_acquisitions;
	pthread_attr_dbg_print(&attrs.source);
	if (g_argument_lease_acquisitions != admissions || pthread_attr_copy(&attrs.copy, &attrs.source) != OK ||
	    g_argument_lease_acquisitions != admissions + 9) return false; // Existing setter admissions only.
	std::swap(attrs.source, attrs.copy);
	if (!check(PthreadAttrGetaffinityHost, PthreadAttrGetaffinity, mask, 2) ||
	    !check(PthreadAttrGetdetachstateHost, PthreadAttrGetdetachstate, 1, 2) ||
	    !check(PthreadAttrGetguardsizeHost, PthreadAttrGetguardsize, guard_size, 2) ||
	    !check(PthreadAttrGetinheritschedHost, PthreadAttrGetinheritsched, 0, 2) ||
	    !check(PthreadAttrGetschedparamHost, PthreadAttrGetschedparam, priority, 0) ||
	    !check(PthreadAttrGetschedpolicyHost, PthreadAttrGetschedpolicy, 3, 2) ||
	    !check(PthreadAttrGetstackaddrHost, PthreadAttrGetstackaddr, stack_addr, 2) ||
	    !check(PthreadAttrGetstacksizeHost, PthreadAttrGetstacksize, stack_size, 2)) return false;
	std::fprintf(stderr, "[PTHREAD_ATTRIBUTE_HOST_OUTPUT_TEST] getters=8 native-admission=0 debug-admission=0"
	    " copy-getter-admission=0 public-slot/output-admission=retained values/canaries/null-errors=pass\n");
	return true;
}
#endif

#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID && LUMEN_PLATFORM != LUMEN_PLATFORM_IOS
static void usec_to_timespec(struct timespec* ts, KernelUseconds usec)
{
	ts->tv_sec  = usec / 1000000;
	ts->tv_nsec = static_cast<decltype(ts->tv_nsec)>((usec % 1000000) * 1000);
}
#endif

static void sec_to_timespec(KernelTimespec* ts, double sec)
{
	ts->tv_sec  = static_cast<int64_t>(sec);
	ts->tv_nsec = static_cast<int64_t>((sec - static_cast<double>(ts->tv_sec)) * 1000000000.0);
}

static void mutex_acquired(PthreadMutex mutex)
{
	++mutex->depth;
	mutex->owner.store(Core::Thread::GetThreadIdUnique(), std::memory_order_relaxed);
}

#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
static thread_local void (*g_mutex_wake_test_hook)(bool) = nullptr;
static thread_local void (*g_mutex_wait_armed_test_hook)() = nullptr;
static thread_local void (*g_mutex_before_enroll_test_hook)() = nullptr;
static thread_local uint32_t g_mutex_wait_slice_test_usec = 0;
void PthreadSetMutexWakeTestHook(void (*hook)(bool)) { g_mutex_wake_test_hook = hook; }
void PthreadSetMutexWaitArmedTestHook(void (*hook)()) { g_mutex_wait_armed_test_hook = hook; }
void PthreadSetMutexBeforeEnrollTestHook(void (*hook)()) { g_mutex_before_enroll_test_hook = hook; }
void PthreadSetMutexWaitSliceForTest(uint32_t usec) { g_mutex_wait_slice_test_usec = usec; }
#endif

static bool mutex_head_wake_enabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_PTHREAD_MUTEX_HEAD_WAKE");
		const bool selected = value != nullptr && std::strcmp(value, "1") == 0;
		if (selected) std::fputs("[PTHREAD_MUTEX_HEAD_WAKE] selected=1 eligible-thread=deepest-ticket\n", stderr);
		return selected;
	}();
	return enabled;
}

// Called with acquisition_mutex held, while the operation still pins the
// wrapper. The generation also covers releases before a waiter reaches wait().
static void mutex_notify_waiters_locked(PthreadMutex mutex)
{
#if defined(LUMEN_PTHREAD_MUTEX_QUEUED_ACQUISITION)
	++mutex->wake_generation;
	// A failed trylock can precede enrollment, so always advance the
	// generation. Only enrolled tickets can be sleeping on either condition;
	// notifying an empty queue cannot wake a waiter.
	if (mutex->first_waiter != nullptr)
	{
		if (mutex_head_wake_enabled())
		{
			auto* target = mutex->first_waiter;
			// Exception delivery may re-enter the reserved head's thread and
			// block in a nested call. Its last queued ticket is the active call;
			// waking the suspended outer ticket would rely on fallback polling.
			for (auto* entry = target->next; entry != nullptr; entry = entry->next)
				if (entry->thread_id == target->thread_id) target = entry;
			EXIT_IF(!target->changed.has_value());
			target->changed->notify_one();
		} else mutex->acquisition_changed.notify_all();
	}
#else
	(void)mutex;
#endif
}

// A reservation contains only host state. In particular, the head waiter can
// leave its HLE reader and readmit while the native mutex remains unlocked.
class MutexWaitTicket final
{
	enum class RemovalReason { Abandoned, Acquired };
public:
	explicit MutexWaitTicket(PthreadMutex mutex): m_mutex(mutex) {}
	~MutexWaitTicket()
	{
#if defined(LUMEN_PTHREAD_MUTEX_QUEUED_ACQUISITION)
		if (!m_waiter.queued) return;
		std::lock_guard lock(m_mutex->acquisition_mutex);
		Remove(m_mutex, &m_waiter, RemovalReason::Abandoned);
#endif
	}
	void Enroll()
	{
#if defined(LUMEN_PTHREAD_MUTEX_QUEUED_ACQUISITION)
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
		if (auto hook = g_mutex_before_enroll_test_hook) hook();
#endif
		std::lock_guard lock(m_mutex->acquisition_mutex);
		if (m_waiter.queued) return;
		if (mutex_head_wake_enabled()) m_waiter.changed.emplace();
		m_waiter.thread_id = Core::Thread::GetThreadIdUnique();
		m_waiter.queued = true;
		if (m_mutex->last_waiter != nullptr) m_mutex->last_waiter->next = &m_waiter;
		else m_mutex->first_waiter = &m_waiter;
		m_mutex->last_waiter = &m_waiter;
#endif
	}
	void WaitForWake(uint32_t usec = 1000)
	{
#if defined(LUMEN_PTHREAD_MUTEX_QUEUED_ACQUISITION)
		bool changed = false;
		{
			std::unique_lock lock(m_mutex->acquisition_mutex);
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
			if (auto hook = g_mutex_wait_armed_test_hook) hook();
			if (g_mutex_wait_slice_test_usec != 0) usec = g_mutex_wait_slice_test_usec;
#endif
			// A wake only permits another trylock after readmission. In
			// particular, owner == 0 is not proof of native availability when
			// a condition wait temporarily owns the native mutex without metadata.
			auto& condition = mutex_head_wake_enabled() ? *m_waiter.changed : m_mutex->acquisition_changed;
			changed = condition.wait_for(lock, std::chrono::microseconds(usec), [&] {
				return m_mutex->wake_generation != m_waiter.wake_generation;
			});
		}
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
		if (auto hook = g_mutex_wake_test_hook) hook(changed);
#else
		(void)changed;
#endif
#else
		Core::Thread::SleepMicro(usec);
#endif
	}
	MutexWaiter* Waiter() { return &m_waiter; }
	MutexWaitTicket(const MutexWaitTicket&) = delete;
	MutexWaitTicket& operator=(const MutexWaitTicket&) = delete;
	static void RemoveAfterAcquire(PthreadMutex mutex, MutexWaiter* waiter)
	{
		Remove(mutex, waiter, RemovalReason::Acquired);
	}
private:
	static void Remove(PthreadMutex mutex, MutexWaiter* waiter, RemovalReason reason)
	{
#if defined(LUMEN_PTHREAD_MUTEX_QUEUED_ACQUISITION)
		if (!waiter->queued) return;
		MutexWaiter* previous = nullptr;
		for (auto* entry = mutex->first_waiter; entry != nullptr; entry = entry->next)
		{
			if (entry != waiter) { previous = entry; continue; }
			if (previous != nullptr) previous->next = entry->next;
			else mutex->first_waiter = entry->next;
			if (mutex->last_waiter == entry) mutex->last_waiter = previous;
			entry->next = nullptr;
			entry->queued = false;
			// Taking the native mutex makes it unavailable to the new head.
			// Only abandoning a reservation can expose an already free mutex;
			// successful acquisition leaves notification to the actual release.
			if (previous == nullptr && reason == RemovalReason::Abandoned) mutex_notify_waiters_locked(mutex);
			return;
		}
		EXIT("queued mutex waiter is missing\n");
#else
		(void)mutex;
		(void)waiter;
		(void)reason;
#endif
	}
	PthreadMutex m_mutex;
	MutexWaiter m_waiter;
};

#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
static thread_local void (*g_mutex_wait_test_hook)() = nullptr;
static thread_local void (*g_mutex_readmission_test_hook)() = nullptr;
void PthreadSetMutexWaitTestHook(void (*hook)()) { g_mutex_wait_test_hook = hook; }
void PthreadSetMutexReadmissionTestHook(void (*hook)()) { g_mutex_readmission_test_hook = hook; }
bool PthreadMutexQueuedAcquisitionForTest()
{
#if defined(LUMEN_PTHREAD_MUTEX_QUEUED_ACQUISITION)
	return true;
#else
	return false;
#endif
}
bool PthreadMutexHeadWakeForTest() { return mutex_head_wake_enabled(); }
bool PthreadMutexDirectoryForTest() { return mutex_directory_enabled(); }
#endif

static int mutex_trylock(PthreadMutex mutex, MutexWaiter* waiter = nullptr, bool record_ownership = true)
{
#if defined(LUMEN_PTHREAD_MUTEX_QUEUED_ACQUISITION)
	std::lock_guard lock(mutex->acquisition_mutex);
	if (waiter != nullptr) waiter->wake_generation = mutex->wake_generation;
	// The current native owner must still be able to recurse (or receive the
	// native self-lock error), regardless of pending foreign reservations.
	const int caller = Core::Thread::GetThreadIdUnique();
	if (mutex->owner.load(std::memory_order_relaxed) != caller && mutex->first_waiter != nullptr &&
	    mutex->first_waiter != waiter && mutex->first_waiter->thread_id != caller) return EBUSY;
	// Exception delivery can re-enter this same queued thread after readmission.
	// Its nested call may use the head reservation but cannot consume the outer
	// ticket; native ownership and recursion checks remain authoritative.
#else
	(void)waiter;
#endif
	const int result = pthread_mutex_trylock(&mutex->p);
	if (result == 0)
	{
		if (record_ownership) mutex_acquired(mutex);
		if (waiter != nullptr) MutexWaitTicket::RemoveAfterAcquire(mutex, waiter);
	}
	return result;
}

static int mutex_unlock(PthreadMutex mutex)
{
#if defined(LUMEN_PTHREAD_MUTEX_QUEUED_ACQUISITION)
	std::lock_guard lock(mutex->acquisition_mutex);
#endif
	const int caller = Core::Thread::GetThreadIdUnique();
	if (mutex->owner.load(std::memory_order_relaxed) != caller)
	{
		// Let the host report foreign/unowned unlock without changing metadata.
		return pthread_mutex_unlock(&mutex->p);
	}
	const uint64_t depth = mutex->depth;
	if (--mutex->depth == 0) mutex->owner.store(0, std::memory_order_relaxed);
	const int result = pthread_mutex_unlock(&mutex->p);
	if (result != 0)
	{
		mutex->depth = depth;
		mutex->owner.store(caller, std::memory_order_relaxed);
	} else if (depth == 1)
	{
		mutex_notify_waiters_locked(mutex);
	}
	// The operation pin covers notification; the queue lock excludes a native
	// successor until metadata and the release generation have been published.
	return result;
}

#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID && LUMEN_PLATFORM != LUMEN_PLATFORM_IOS
class MutexConditionOwnership final
{
public:
	explicit MutexConditionOwnership(PthreadMutex mutex): m_mutex(mutex), m_owner(Core::Thread::GetThreadIdUnique())
	{
		if (m_mutex->owner.load(std::memory_order_relaxed) == m_owner)
		{
			m_depth = m_mutex->depth;
			m_mutex->depth = 0;
			m_mutex->owner.store(0, std::memory_order_relaxed);
		}
	}
	~MutexConditionOwnership()
	{
		if (m_depth != 0)
		{
			m_mutex->depth = m_depth;
			m_mutex->owner.store(m_owner, std::memory_order_relaxed);
		}
	}
	MutexConditionOwnership(const MutexConditionOwnership&) = delete;
	MutexConditionOwnership& operator=(const MutexConditionOwnership&) = delete;
	[[nodiscard]] uint64_t Depth() const { return m_depth; }

private:
	PthreadMutex m_mutex;
	int          m_owner;
	uint64_t     m_depth = 0;
};

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
#if defined(LUMEN_PTHREAD_CONDITION_TEST_HOOK)
static thread_local PthreadConditionWaitTestHook g_condition_wait_test_hook = nullptr;

void PthreadSetConditionWaitTestHook(PthreadConditionWaitTestHook hook)
{
	g_condition_wait_test_hook = hook;
}
#endif

static int macos_fex_cond_wait(PthreadCond cond, PthreadMutex mutex, const timespec* relative_timeout)
{
	EXIT_IF(cond == nullptr || mutex == nullptr);

	// Both handles are host-owned snapshots. No guest slot or VM lease is
	// touched while ScopedGuestHostWait yields the top-level HLE reader.
	MutexConditionOwnership ownership(mutex);
	const uint64_t          ownership_depth = ownership.Depth();
	bool                    native_levels_owned = ownership_depth != 0;

	// The condition wait restores every recursive native lock level. Release
	// all levels before EndQuiescentHostWait: readmission can itself wait for
	// a mutation whose draining HLE reader still needs this mutex. Ownership
	// depth, rather than the condition result, tells whether this caller held
	// the native mutex on entry (including fail-fast error paths).
	const auto release_levels = [&]
	{
		if (!native_levels_owned) return;
#if defined(LUMEN_PTHREAD_MUTEX_QUEUED_ACQUISITION)
		std::lock_guard lock(mutex->acquisition_mutex);
#endif
		for (uint64_t level = 0; level < ownership_depth; ++level)
		{
			const int unlock_result = pthread_mutex_unlock(&mutex->p);
			EXIT_NOT_IMPLEMENTED(unlock_result != 0);
		}
		native_levels_owned = false;
		mutex_notify_waiters_locked(mutex);
	};
	const auto reacquire_levels = [&]
	{
		// A generation wake can exit before the first native wait or between
		// slices, when all saved levels are already owned by this caller.
		if (native_levels_owned || ownership_depth == 0) return;
		MutexWaitTicket ticket(mutex);
		int lock_result = EBUSY;
		while ((lock_result = mutex_trylock(mutex, ticket.Waiter(), false)) == EBUSY)
		{
			ticket.Enroll();
			Loader::ScopedGuestHostWait host_wait;
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
			if (auto hook = g_mutex_wait_test_hook) hook();
#endif
			ticket.WaitForWake();
		}
		EXIT_NOT_IMPLEMENTED(lock_result != 0);
		for (uint64_t level = 1; level < ownership_depth; ++level)
		{
			// Only a recursive mutex can have saved depth greater than one, so
			// restoring the remaining levels must never block.
			lock_result = pthread_mutex_trylock(&mutex->p);
			EXIT_NOT_IMPLEMENTED(lock_result != 0);
		}
		native_levels_owned = true;
	};

	// Bounded slices keep a pending guest exception deliverable while this
	// thread is blocked here, and the shared wakeup generation closes the window
	// in which a signal could otherwise be lost between slices. The guest only
	// observes a real wakeup, a native ownership/argument error or its own
	// deadline; an already elapsed caller deadline passes the caller's original
	// timeout to the final native wait so those native results are preserved.
	const uint64_t generation = cond->signal_generation.load(std::memory_order_acquire);
	constexpr uint64_t slice_nanos = 10 * 1000 * 1000;
	const auto deadline = relative_timeout == nullptr
	    ? std::chrono::steady_clock::time_point::max()
	    : std::chrono::steady_clock::now() +
	          std::chrono::seconds(relative_timeout->tv_sec) +
	          std::chrono::nanoseconds(relative_timeout->tv_nsec);
	int result = 0;
#if defined(LUMEN_PTHREAD_CONDITION_TEST_HOOK)
	bool completed_slice = false;
#endif
	for (;;)
	{
#if defined(LUMEN_PTHREAD_CONDITION_TEST_HOOK)
		if (auto hook = g_condition_wait_test_hook) hook(cond, completed_slice);
#endif
		if (cond->signal_generation.load(std::memory_order_acquire) != generation)
		{
			// A signal or broadcast completed while this waiter was between
			// slices and could not have observed it natively.
			result = 0;
			break;
		}
		timespec slice {};
		if (relative_timeout == nullptr)
		{
			slice.tv_nsec = static_cast<decltype(slice.tv_nsec)>(slice_nanos);
		} else if (std::chrono::steady_clock::now() >= deadline)
		{
			slice = *relative_timeout;
		} else
		{
			uint64_t wait_nanos = slice_nanos;
			const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - std::chrono::steady_clock::now()).count();
			if (left > 0 && static_cast<uint64_t>(left) < wait_nanos) wait_nanos = static_cast<uint64_t>(left);
			if (wait_nanos == 0) wait_nanos = 1;
			slice.tv_sec  = static_cast<decltype(slice.tv_sec)>(wait_nanos / 1000000000);
			slice.tv_nsec = static_cast<decltype(slice.tv_nsec)>(wait_nanos % 1000000000);
		}
		{
			Loader::ScopedGuestHostWait host_wait;
			result = pthread_cond_timedwait_relative_np(&cond->p, &mutex->p, &slice);
			release_levels();
		}
		if (result != ETIMEDOUT) break;
		if (relative_timeout != nullptr && std::chrono::steady_clock::now() >= deadline) break;
		// The condition wait reacquired the mutex before returning the slice
		// timeout; release left it unlocked, so restore it before continuing.
		reacquire_levels();
#if defined(LUMEN_PTHREAD_CONDITION_TEST_HOOK)
		completed_slice = true;
#endif
	}
	reacquire_levels();

	return result;
}
#endif
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
static bool runtime_sleep_nanos(uint64_t nanos)
{
	constexpr uint64_t stop_poll_nanos = 10000000;
	while (nanos > 0 && !Loader::GuestExecutionStopRequested())
	{
		const uint64_t slice = nanos > stop_poll_nanos ? stop_poll_nanos : nanos;
		{
			Loader::ScopedGuestHostWait host_wait;
			Core::Thread::SleepNano(slice);
		}
		nanos -= slice;
	}
	return !Loader::GuestExecutionStopRequested();
}

static int runtime_cond_wait(PthreadCond cond, PthreadMutex mutex, KernelUseconds timeout, bool infinitely)
{
	const uint64_t generation = cond->signal_generation.load(std::memory_order_acquire);
	int result = mutex_unlock(mutex);
	if (result != 0)
	{
		return result;
	}

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout);
	bool timed_out = false;
	{
		// The condition is a host-owned snapshot, and this segment only reads its
		// generation, the host stop flag and the clock. Keep the HLE reader yielded
		// across polling sleeps; no guest slot, bytes or mutex are accessed here.
		// Enter lazily so a zero timeout does not introduce a new readmission wait.
		std::optional<Loader::ScopedGuestHostWait> host_wait;
		while (cond->signal_generation.load(std::memory_order_acquire) == generation &&
		       !Loader::GuestExecutionStopRequested())
		{
			if (!infinitely)
			{
				const auto now = std::chrono::steady_clock::now();
				if (now >= deadline)
				{
					timed_out = true;
					break;
				}
				const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
				if (!host_wait) host_wait.emplace();
				Core::Thread::SleepMicro(static_cast<uint32_t>(remaining > 1000 ? 1000 : remaining));
			} else
			{
				if (!host_wait) host_wait.emplace();
				Core::Thread::SleepMicro(1000);
			}
		}
	}
	// Readmit before any attempt to own the mutex: holding it during
	// readmission could block a peer HLE needed to drain an exclusive mutation.

	// During the one-shot stop the guest will not resume, so deliberately leave
	// the mutex unlocked. This avoids the POSIX cond-wait reacquire deadlock when
	// another stopped guest thread was its last owner.
	if (Loader::GuestExecutionStopRequested())
	{
		return ECANCELED;
	}

	MutexWaitTicket ticket(mutex);
	result = EBUSY;
	while (result == EBUSY && !Loader::GuestExecutionStopRequested())
	{
		result = mutex_trylock(mutex, ticket.Waiter());
		if (result == EBUSY)
		{
			ticket.Enroll();
			Loader::ScopedGuestHostWait host_wait;
			ticket.WaitForWake();
		}
	}
	if (Loader::GuestExecutionStopRequested())
	{
		if (result == 0)
		{
			mutex_unlock(mutex);
		}
		return ECANCELED;
	}
	if (result != 0)
	{
		return result;
	}
	return timed_out ? ETIMEDOUT : 0;
}
#endif

void* PthreadStaticObjects::CreateObject(void* addr, PthreadStaticObject::Type type)
{
	if (type == PthreadStaticObject::Type::Mutex)
	{
		void* object = nullptr;
		return ResolveObject(addr, type, &object) == OK ? addr : nullptr;
	}
	// Complete the slot's read outside the registry. Existing handles can live
	// in a read-only page; only implicit initialization needs write intention.
#if defined(LUMEN_FEXCORE_RUNTIME)
	Core::VirtualMemory::GuestDataLease read_lease, write_lease;
	if (addr == nullptr || !Core::VirtualMemory::TryAcquireGuestDataLease(
	        reinterpret_cast<uint64_t>(addr), sizeof(void*), Core::VirtualMemory::Mode::Read, &read_lease)) return nullptr;
	bool writable = false;
	for (;;)
	{
		{
			Core::LockGuard lock(m_mutex);
			if (*static_cast<void**>(addr) != nullptr || writable)
			{
				GuestCpu::MemoryHandoffBypass prepared;
				return CreateObjectLocked(addr, type);
			}
		}
		if (!Core::VirtualMemory::TryAcquireGuestDataLease(
		        reinterpret_cast<uint64_t>(addr), sizeof(void*), Core::VirtualMemory::Mode::Write, &write_lease)) return nullptr;
		writable = true;
	}
#else
	Core::LockGuard lock(m_mutex);
	return CreateObjectLocked(addr, type);
#endif
}

void* PthreadStaticObjects::CreateObjectLocked(void* addr, PthreadStaticObject::Type type, int* error)
{
	if (error != nullptr) *error = KERNEL_ERROR_EINVAL;
	if (addr == nullptr || *static_cast<void**>(addr) != nullptr)
	{
		return addr;
	}

	auto* rt      = Core::Singleton<Loader::RuntimeLinker>::Instance();
	auto  vaddr   = reinterpret_cast<uint64_t>(addr);
	auto* program = rt->FindProgramByAddr(vaddr);

	// A null condition outside a module cannot use the tracked static-object
	// initializer. Let callers report EINVAL without allocating an unowned
	// object or terminating the emulator. This does not identify its lifetime.
	if (program == nullptr && type == PthreadStaticObject::Type::Cond)
	{
		trace_condition(ConditionTraceKind::Rejected, "implicit-init", "outside-module-null-slot",
		                vaddr, 0, "kernel", KERNEL_ERROR_EINVAL);
		return nullptr;
	}

	EXIT_NOT_IMPLEMENTED(program == nullptr);

	auto* obj    = new PthreadStaticObject;
	obj->program = program;
	obj->type    = type;
	obj->vaddr   = vaddr;

	String name = String::FromPrintf("Static%016" PRIx64, vaddr);

	int result = OK;
	switch (type)
	{
		case PthreadStaticObject::Type::Mutex:
			result = pthread_mutex_init_impl(static_cast<PthreadMutex*>(addr), nullptr, name.C_Str(), false);
			break;
		case PthreadStaticObject::Type::Cond: result = PthreadCondInit(static_cast<PthreadCond*>(addr), nullptr, name.C_Str()); break;
		case PthreadStaticObject::Type::Rwlock: result = PthreadRwlockInit(static_cast<PthreadRwlock*>(addr), nullptr, name.C_Str()); break;
		default: EXIT("unknown type: %d\n", static_cast<int>(type));
	}

	if (result != OK && type == PthreadStaticObject::Type::Mutex)
	{
		// Mutex backing cannot yield while this registry lock is held. Keep the
		// null guest slot unpublished and propagate the typed allocation error.
		delete obj;
		if (error != nullptr) *error = result;
		return nullptr;
	}
	EXIT_NOT_IMPLEMENTED(result != OK);

	auto index = m_objects.Find(nullptr);

	if (m_objects.IndexValid(index))
	{
		m_objects[index] = obj;
	} else
	{
		m_objects.Add(obj);
	}

	return addr;
}

Loader::Program* PthreadStaticObjects::FindSlotProgram(uint64_t address)
{
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
	if (m_program_test != nullptr) return m_program_test(m_initialization_test_context, address);
#endif
	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();
	return rt != nullptr ? rt->FindProgramByAddr(address) : nullptr;
}

int PthreadStaticObjects::ResolveObject(void* slot, PthreadStaticObject::Type type, void** object)
{
	if (slot == nullptr || object == nullptr) return KERNEL_ERROR_EINVAL;
	*object = nullptr;
	NativeMutexDirectory::Garbage directory_garbage;
	// A rejected or racing candidate is retired only after the slot leases and
	// registry lock have ended. No handle is registered during preparation.
	PreparedPthreadMutex prepared_mutex(nullptr, destroy_unpublished_mutex);
	uint64_t prepared_retirement_generation = 0;
	int32_t prepared_program_id = -1;
#if defined(LUMEN_FEXCORE_RUNTIME)
	Core::VirtualMemory::GuestDataRangeSeal slot_seal;
	Core::VirtualMemory::GuestDataLease read_lease, write_lease;
	// A native caller can meet an exclusive GPU/VM epoch without owning an HLE
	// reader. Wait only for that typed transient denial, before taking any
	// registry lock or reading the slot. Every attempt proves lifetime and
	// rights anew; permanent context/range/permission failures stay failures.
	const auto acquire_read = [&] {
		Core::VirtualMemory::GuestDataLeaseDenialMetadata denial;
		auto read_deadline = std::chrono::steady_clock::time_point {};
		constexpr uint32_t max_read_attempts = 4096;
		uint32_t read_attempts = 0;
		while (!Core::VirtualMemory::TryAcquireGuestDataLease(reinterpret_cast<uint64_t>(slot), sizeof(void*),
		        Core::VirtualMemory::Mode::Read, &read_lease, true, &denial))
		{
			using Reason = Core::VirtualMemory::GuestDataLeaseDenialReason;
			if (denial.reason == Reason::GateWriterPending && denial.retryable)
			{
				const auto now = std::chrono::steady_clock::now();
				if (read_attempts == 0) read_deadline = now + std::chrono::seconds(3);
				if (++read_attempts < max_read_attempts && now < read_deadline)
				{
					// No data pin was granted. Native admission remains outside HLE,
					// so there is no execution reader to donate or fabricate.
					Core::Thread::Sleep(1);
					continue;
				}
			}
			return false;
		}
		return true;
	};
	if (!acquire_read()) return KERNEL_ERROR_EFAULT;
#endif
	const auto read_snapshot = [&](void** snapshot) {
#if defined(LUMEN_FEXCORE_RUNTIME)
		// The exact slot is already admitted through the final read below. Use
		// that lease's tagged host pointer instead of querying/admitting the same
		// eight bytes again while the static-object registry is locked.
		std::memcpy(snapshot, reinterpret_cast<const void*>(read_lease.HostBegin()), sizeof(*snapshot));
		return true;
#elif defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
		return Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(slot), snapshot, sizeof(*snapshot));
#else
		std::memcpy(snapshot, slot, sizeof(*snapshot));
		return true;
#endif
	};
	// Resolving an existing handle does not use the static-initializer registry.
	// The current read lease supplies the slot bytes and keeps their lifetime;
	// the mutex handle registry validates the native object independently below.
	// Only a null slot needs serialization with another implicit initializer.
	void* current = nullptr;
	if (!read_snapshot(&current)) return KERNEL_ERROR_EFAULT;
	if (g_mutex_error_trace != nullptr) g_mutex_error_trace->Snapshot(current);
	if (current != nullptr)
	{
		*object = current;
		return OK;
	}
	bool writable = false;
	for (;;)
	{
		bool prepare_mutex = false;
		{
			// A static initializer publishes its slot while holding this same lock.
			// The preflight above stabilizes GPU ownership before this lock is taken.
			Core::LockGuard lock(m_mutex);
			GuestCpu::MemoryHandoffBypass prepared;
			const auto address = reinterpret_cast<uint64_t>(slot);
			if (prepared_mutex != nullptr)
			{
				if (m_retirement_generation != prepared_retirement_generation) return KERNEL_ERROR_EINVAL;
				const auto* program = FindSlotProgram(address);
				if (program == nullptr || program->unique_id != prepared_program_id) return KERNEL_ERROR_EINVAL;
			}
			void* snapshot = nullptr;
			if (!read_snapshot(&snapshot)) return KERNEL_ERROR_EFAULT;
			if (g_mutex_error_trace != nullptr) g_mutex_error_trace->Snapshot(snapshot);
			if (snapshot != nullptr)
			{
				*object = snapshot;
				return OK;
			}

			auto* program = FindSlotProgram(address);
			if (address % alignof(void*) != 0 || program == nullptr)
				return KERNEL_ERROR_EINVAL;
			if (type == PthreadStaticObject::Type::Mutex && prepared_mutex == nullptr)
			{
#if defined(LUMEN_FEXCORE_RUNTIME)
				if (!Core::VirtualMemory::CaptureGuestDataRangeSeal(read_lease, &slot_seal)) return KERNEL_ERROR_EFAULT;
#endif
				prepared_retirement_generation = m_retirement_generation;
				prepared_program_id = program->unique_id;
				prepare_mutex = true;
			} else if (writable && type == PthreadStaticObject::Type::Mutex)
			{
				// Keep one registry record when a tracked slot is destroyed/reused.
				// The record owns this slot's current object, not an older handle.
				PthreadStaticObject* record = nullptr;
				for (auto* current: m_objects)
					if (current != nullptr && current->vaddr == address)
					{
						if (current->program != program || current->type != type) return KERNEL_ERROR_EINVAL;
						record = current;
					}
				std::unique_ptr<PthreadStaticObject> new_record;
				if (record == nullptr) new_record.reset(new PthreadStaticObject {type, address, program});
				auto* native = prepared_mutex.get();
				const auto handle = mutex_guest_handle(native);
				{
					std::lock_guard handles_lock(g_mutex_handles_mutex);
					register_mutex_handle_locked(native, directory_garbage);
#if defined(LUMEN_FEXCORE_RUNTIME)
					std::memcpy(reinterpret_cast<void*>(write_lease.HostBegin()), &handle, sizeof(handle));
#elif defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
					// Non-FEX WriteGuestMemory only checks native/VM mapping
					// metadata and copies; it never performs HLE admission/handoff.
					if (!Loader::WriteGuestMemory(address, &handle, sizeof(handle)))
					{
						unregister_unpublished_mutex_locked(prepared_mutex, directory_garbage);
						return KERNEL_ERROR_EFAULT;
					}
#else
					std::memcpy(slot, &handle, sizeof(handle));
#endif
					prepared_mutex.release();
					if (mutex_directory_enabled()) NativeMutexDirectory::Reopen(native->directory_record);
				}
				if (new_record != nullptr)
				{
					const auto index = m_objects.Find(nullptr);
					if (m_objects.IndexValid(index)) m_objects[index] = new_record.release();
					else m_objects.Add(new_record.release());
				}
				*object = handle;
				return OK;
			} else if (writable)
			{
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
				if (!Loader::ValidateGuestMemory(address, sizeof(snapshot), true)) return KERNEL_ERROR_EFAULT;
#endif
				int creation_error = KERNEL_ERROR_EINVAL;
				if (CreateObjectLocked(slot, type, &creation_error) == nullptr) return creation_error;
				if (!read_snapshot(&snapshot)) return KERNEL_ERROR_EFAULT;
				if (snapshot == nullptr) return KERNEL_ERROR_EINVAL;
				*object = snapshot;
				return OK;
			}
		}
		if (prepare_mutex)
		{
#if defined(LUMEN_FEXCORE_RUNTIME)
			Core::VirtualMemory::ReleaseGuestDataLease(&write_lease);
			Core::VirtualMemory::ReleaseGuestDataLease(&read_lease);
#endif
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
			if (m_preparation_test != nullptr) m_preparation_test(m_initialization_test_context, slot);
#endif
			// Only scalar module identity and a revocable range seal survive a
			// possible host wait. The default native attribute is consumed before
			// allocating; no module pointer or guest argument is retained here.
			const std::string name = String::FromPrintf("Static%016" PRIx64, reinterpret_cast<uint64_t>(slot)).C_Str();
			const int result = prepare_mutex_guest_backing(*g_pthread_context->GetDefaultMutexattr(), name,
#if defined(LUMEN_FEXCORE_RUNTIME)
			    true,
#else
			    false,
#endif
			    &prepared_mutex);
			if (result != OK) return result;
#if defined(LUMEN_FEXCORE_RUNTIME)
			if (!acquire_read() || !Core::VirtualMemory::ValidateGuestDataRangeSeal(read_lease, slot_seal))
				return KERNEL_ERROR_EFAULT;
#endif
			continue;
		}
		// Drop the registry before requesting the write. An exclusive GPU epoch
		// cannot start while the read lease survives, and the next iteration
		// rechecks the slot in case a peer initialized it in the meantime.
#if defined(LUMEN_FEXCORE_RUNTIME)
		if (!Core::VirtualMemory::TryAcquireGuestDataLease(reinterpret_cast<uint64_t>(slot), sizeof(void*),
		        Core::VirtualMemory::Mode::Write, &write_lease)) return KERNEL_ERROR_EFAULT;
#else
		if (!Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(slot), sizeof(void*), true)) return KERNEL_ERROR_EFAULT;
#endif
		writable = true;
	}
}

int PthreadStaticObjects::ResolveCondition(PthreadCond* slot, PthreadCond* object)
{
	if (object == nullptr) return KERNEL_ERROR_EINVAL;
	void* resolved = nullptr;
	const int result = ResolveObject(slot, PthreadStaticObject::Type::Cond, &resolved);
	if (result == OK) *object = static_cast<PthreadCond>(resolved);
	return result;
}

int PthreadStaticObjects::ResolveMutex(PthreadMutex* slot, PthreadMutex* object, bool pin)
{
	if (object == nullptr) return KERNEL_ERROR_EINVAL;
	void* resolved = nullptr;
	const int result = ResolveObject(slot, PthreadStaticObject::Type::Mutex, &resolved);
	if (result != OK) return result;
	*object = resolve_mutex_handle(static_cast<PthreadMutex>(resolved), pin);
	return *object != nullptr ? OK : KERNEL_ERROR_EINVAL;
}

static int resolve_condition_wait_objects(PthreadCond* condition_slot, PthreadMutex* mutex_slot,
	                                       PthreadCond* condition, PthreadMutex* mutex)
{
	if (condition_slot == nullptr || mutex_slot == nullptr || condition == nullptr || mutex == nullptr ||
	    g_pthread_context == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
	auto* objects = g_pthread_context->GetPthreadStaticObjects();
	if (objects == nullptr) return KERNEL_ERROR_EINVAL;
	const int condition_result = objects->ResolveCondition(condition_slot, condition);
	if (condition_result != OK) return condition_result;
	return objects->ResolveMutex(mutex_slot, mutex, true);
}

void PthreadStaticObjects::DeleteObjects(Loader::Program* program)
{
	Vector<PthreadStaticObject*> retired;
	{
		Core::LockGuard lock(m_mutex);
		EXIT_IF(m_retirement_generation == UINT64_MAX);
		++m_retirement_generation;
		for (auto& obj: m_objects)
		{
			if (obj != nullptr && obj->program == program)
			{
				retired.Add(obj);
				obj = nullptr;
			}
		}
	}
	// Another module's HLE reader may need this static-object lock to finish.
	// Retire native objects and acquire prefix-reset leases outside that lock.
	for (auto* obj: retired)
	{
		int result = OK;
		switch (obj->type)
		{
			case PthreadStaticObject::Type::Mutex: result = PthreadMutexDestroy(reinterpret_cast<PthreadMutex*>(obj->vaddr)); break;
			case PthreadStaticObject::Type::Cond: result = PthreadCondDestroy(reinterpret_cast<PthreadCond*>(obj->vaddr)); break;
			case PthreadStaticObject::Type::Rwlock: result = PthreadRwlockDestroy(reinterpret_cast<PthreadRwlock*>(obj->vaddr)); break;
			default: EXIT("unknown type: %d\n", static_cast<int>(obj->type));
		}

		EXIT_NOT_IMPLEMENTED(result != OK);

		delete obj;
	}
}

bool PthreadStaticObjects::ResetForNextSession()
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(m_retirement_generation == UINT64_MAX);
	++m_retirement_generation;
	for (auto*& obj: m_objects)
	{
		if (obj == nullptr)
		{
			continue;
		}

		int result = OK;
		switch (obj->type)
		{
			case PthreadStaticObject::Type::Mutex: result = PthreadMutexDestroy(reinterpret_cast<PthreadMutex*>(obj->vaddr)); break;
			case PthreadStaticObject::Type::Cond: result = PthreadCondDestroy(reinterpret_cast<PthreadCond*>(obj->vaddr)); break;
			case PthreadStaticObject::Type::Rwlock: result = PthreadRwlockDestroy(reinterpret_cast<PthreadRwlock*>(obj->vaddr)); break;
		}
		if (result != OK)
		{
			return false;
		}
		delete obj;
		obj = nullptr;
	}
	m_objects.Clear();
	return true;
}

#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
bool PthreadStaticObjects::MutexInitializationContractTest()
{
#if !defined(LUMEN_FEXCORE_RUNTIME)
	std::fprintf(stderr, "[PTHREAD_STATIC_MUTEX_INITIALIZATION_TEST] lease-transaction=requires-FEX-runtime\n");
	return true;
#else
	using namespace Core::VirtualMemory;
	enum class Case { Immediate, Race, Reuse, InitializedRegistryBusy, Unload, Reset, Remap, ProtectRevert, ModuleChange, AllocationError };
	struct Context
	{
		PthreadStaticObjects* registry;
		Loader::Program program;
		uint64_t mapping;
		Case scenario;
		std::atomic<uint32_t> arrivals {0};
		std::atomic<bool> valid {true};
	};
	for (auto* record: m_objects) if (record != nullptr) return false;
	uint32_t cases = 0;
	for (const auto scenario: {Case::Immediate, Case::Race, Case::Reuse, Case::InitializedRegistryBusy, Case::Unload, Case::Reset,
	    Case::Remap, Case::ProtectRevert, Case::ModuleChange, Case::AllocationError})
	{
		constexpr uint64_t page_size = 16 * 1024;
		const auto mapping = Alloc(0, page_size, Mode::ReadWrite);
		if (mapping == 0) return false;
		Context context {this, {}, mapping, scenario};
		context.program.unique_id = 117;
		auto* slot = reinterpret_cast<PthreadMutex*>(mapping + 64);
		struct Cleanup
		{
			Context* context;
			~Cleanup()
			{
				g_mutex_preparation_test_error = OK;
				context->registry->m_preparation_test = nullptr;
				context->registry->DeleteObjects(&context->program);
				context->registry->m_program_test = nullptr;
				context->registry->m_initialization_test_context = nullptr;
				if (context->mapping != 0) (void)Free(context->mapping);
			}
		} cleanup {&context};
		m_initialization_test_context = &context;
		m_program_test = [](void* opaque, uint64_t address) -> Loader::Program* {
			auto& test = *static_cast<Context*>(opaque);
			return address >= test.mapping && address - test.mapping < page_size ? &test.program : nullptr;
		};
		m_preparation_test = [](void* opaque, void*) {
			auto& test = *static_cast<Context*>(opaque);
			// This boundary is the real resolver's allocation boundary. Check
			// its lease state and lock availability before any injected schedule.
			if (CurrentThreadHasMemoryLease()) test.valid = false;
			const auto lock_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
			bool registry_available = false;
			while (!(registry_available = test.registry->m_mutex.TryLock()) && std::chrono::steady_clock::now() < lock_deadline)
				std::this_thread::yield();
			if (registry_available) test.registry->m_mutex.Unlock();
			else test.valid = false;
			switch (test.scenario)
			{
				case Case::Race:
				{
					test.arrivals.fetch_add(1, std::memory_order_release);
					const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
					while (test.arrivals.load(std::memory_order_acquire) < 2 && std::chrono::steady_clock::now() < deadline)
						std::this_thread::yield();
					if (test.arrivals.load(std::memory_order_acquire) != 2) test.valid = false;
					break;
				}
				case Case::Unload: test.registry->DeleteObjects(&test.program); break;
				case Case::Reset: if (!test.registry->ResetForNextSession()) test.valid = false; break;
				case Case::Remap:
				{
					const auto old_address = test.mapping;
					if (!Free(old_address)) { test.valid = false; break; }
					test.mapping = Alloc(old_address, page_size, Mode::ReadWrite);
					if (test.mapping != old_address) test.valid = false;
					break;
				}
				case Case::ProtectRevert:
					if (!Protect(test.mapping, page_size, Mode::Read) || !Protect(test.mapping, page_size, Mode::ReadWrite))
						test.valid = false;
					break;
				case Case::ModuleChange: ++test.program.unique_id; break;
				case Case::AllocationError: g_mutex_preparation_test_error = KERNEL_ERROR_ENOMEM; break;
				default: break;
			}
		};
		size_t handles_before = 0;
		{
			std::lock_guard lock(g_mutex_handles_mutex);
			handles_before = g_mutex_handles.size();
		}
		const bool succeeds = scenario == Case::Immediate || scenario == Case::Race || scenario == Case::Reuse ||
		    scenario == Case::InitializedRegistryBusy;
		PthreadMutex native = nullptr;
		int result = KERNEL_ERROR_EINVAL;
		if (scenario == Case::Race)
		{
			std::array<PthreadMutex, 2> resolved {};
			std::array<int, 2> results {};
			std::array<std::thread, 2> threads;
			for (unsigned index = 0; index < threads.size(); ++index)
				threads[index] = std::thread([&, index] { results[index] = ResolveMutex(slot, &resolved[index]); });
			for (auto& thread: threads) thread.join();
			result = results[0]; native = resolved[0];
			if (results[1] != OK || resolved[1] != native || context.arrivals != 2) context.valid = false;
		} else
		{
			result = ResolveMutex(slot, &native);
		}
		g_mutex_preparation_test_error = OK;
		if (scenario == Case::Reuse && result == OK)
		{
			if (PthreadMutexDestroy(slot) != OK || *slot != nullptr) context.valid = false;
			result = ResolveMutex(slot, &native);
		}
		if (scenario == Case::InitializedRegistryBusy && result == OK)
		{
			// Hold the real static-initializer registry on a peer. A live slot
			// still requires a fresh guest lease and native handle validation,
			// but must resolve without waiting for unrelated initialization.
			std::atomic<bool> registry_locked {false}, release_registry {false}, resolved {false};
			PthreadMutex repeated = nullptr;
			int repeated_result = KERNEL_ERROR_EINVAL;
			std::thread owner([&] {
				Core::LockGuard lock(m_mutex);
				registry_locked.store(true, std::memory_order_release);
				while (!release_registry.load(std::memory_order_acquire)) std::this_thread::yield();
			});
			while (!registry_locked.load(std::memory_order_acquire)) std::this_thread::yield();
			std::thread reader([&] {
				repeated_result = ResolveMutex(slot, &repeated);
				resolved.store(true, std::memory_order_release);
			});
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
			while (!resolved.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
				std::this_thread::yield();
			const bool resolved_while_locked = resolved.load(std::memory_order_acquire);
			release_registry.store(true, std::memory_order_release);
			owner.join();
			reader.join();
			if (!resolved_while_locked || repeated_result != OK || repeated != native) context.valid = false;
		}
		const int expected_error = succeeds ? OK : scenario == Case::Remap || scenario == Case::ProtectRevert ? KERNEL_ERROR_EFAULT :
		    scenario == Case::AllocationError ? KERNEL_ERROR_ENOMEM : KERNEL_ERROR_EINVAL;
		uint32_t records = 0;
		for (auto* record: m_objects) if (record != nullptr && record->vaddr == reinterpret_cast<uint64_t>(slot)) ++records;
		bool handles_valid = false;
		{
			std::lock_guard lock(g_mutex_handles_mutex);
			handles_valid = g_mutex_handles.size() == handles_before + (succeeds ? 1u : 0u);
			for (auto* prefix: g_retired_mutex_prefixes)
				if (g_mutex_handles.count(reinterpret_cast<PthreadMutex>(prefix)) != 0) handles_valid = false;
		}
		if (result != expected_error || !context.valid || !handles_valid || records != (succeeds ? 1u : 0u) ||
		    CurrentThreadHasMemoryLease() || (succeeds ? *slot == nullptr || native == nullptr : *slot != nullptr || native != nullptr))
		{
			std::fprintf(stderr, "[PTHREAD_STATIC_MUTEX_INITIALIZATION_TEST_FAIL] case=%u result=0x%08x expected=0x%08x"
			    " records=%u handles=%u boundary=%u\n", static_cast<unsigned>(scenario), result, expected_error,
			    records, handles_valid ? 1u : 0u, context.valid ? 1u : 0u);
			return false;
		}
		if (succeeds && (PthreadMutexLock(slot) != OK || PthreadMutexUnlock(slot) != OK)) return false;
		++cases;
	}
	std::fprintf(stderr, "[PTHREAD_STATIC_MUTEX_INITIALIZATION_TEST] cases=%u allocation-boundary=no-lease/no-registry"
	    " initialized=independent-registry race=one-winner reuse=one-record retirement=epoch remap/protect-revert=sealed error=unpublished"
	    " native-mutex/backing/leases/seal=real module-lookup=lifetime-model\n", cases);
	return true;
#endif
}

bool PthreadStaticMutexInitializationContractTest()
{
	return g_pthread_context != nullptr && g_pthread_context->GetPthreadStaticObjects()->MutexInitializationContractTest();
}
#endif

Pthread PthreadPool::Create(int* error)
{
	if (error != nullptr) *error = KERNEL_ERROR_EAGAIN;
	Pthread recycled = nullptr;
	{
		Core::LockGuard lock(m_mutex);
		for (auto* p: m_threads)
		{
			if (p->free)
			{
				// The previous native thread has joined and its TLS roots are gone.
				// Retain handle identity while clearing only its guest ABI prefix.
				// Clear the old generation's reaper eligibility before publishing the
				// reserved slot. PthreadCreate fills its remaining fields afterwards.
				p->detached = false;
				p->almost_done = false;
				p->join_claimed = false;
				// A recycled wrapper starts a new generation: never deliver an
				// exception queued for the previous one, and never cancel a
				// boundary interrupt armed for a later generation.
				p->pending_exception_signal.store(0, std::memory_order_release);
				p->boundary_interrupt_token.store(0, std::memory_order_release);
				p->cleanup_handlers.clear();
				p->free = false;
				recycled = p;
				break;
			}
		}
	}
	if (recycled != nullptr)
	{
		// Reserve the wrapper under the pool lock, then acquire mapping lifetime
		// without that lock. A denied reset never publishes or starts a generation.
		if (clear_pthread_guest_prefix(recycled))
		{
			if (error != nullptr) *error = OK;
			return recycled;
		}
		Core::LockGuard lock(m_mutex);
		recycled->free = true;
		return nullptr;
	}

	auto owned = std::make_unique<PthreadPrivate>();
	auto* ret = owned.get();
	const int prefix_error = prepare_pthread_guest_prefix(ret);
	if (prefix_error != OK)
	{
		if (error != nullptr) *error = prefix_error;
		return nullptr;
	}

	ret->free        = false;
	ret->detached    = false;
	ret->host_detached = false;
	ret->almost_done = false;
	ret->attr        = nullptr;

	{
		Core::LockGuard lock(m_mutex);
		m_threads.Add(ret);
	}

	if (error != nullptr) *error = OK;
	return owned.release();
}

Pthread PthreadPool::Resolve(uint64_t handle, bool allow_completed)
{
	if (handle == 0) return nullptr;

	Core::LockGuard lock(m_mutex);

	for (auto* p: m_threads)
	{
		if (reinterpret_cast<uint64_t>(pthread_guest_handle(p)) != handle) continue;
		return !p->free && (allow_completed || !p->almost_done) ? p : nullptr;
	}

	return nullptr;
}

void PthreadPool::FreeDetachedThreads()
{
	Vector<Pthread> claimed;
	{
		Core::LockGuard lock(m_mutex);
		for (auto* p: m_threads)
		{
			if (p->detached && p->almost_done && claim_pthread_join(p) == OK) claimed.Add(p);
		}
	}
	// Native exit and TLS/key cleanup may block or reenter HLE. A claim pins
	// each wrapper without keeping Create or another joiner behind the pool lock.
	for (auto* p: claimed) join_claimed_pthread(p, nullptr);
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
bool PthreadPool::ResetForNextSession()
{
	Core::LockGuard lock(m_mutex);

	// Validate the complete pool before joining anything so a still-running
	// guest thread leaves the current session fail-closed and inspectable.
	for (auto* thread: m_threads)
	{
		if (thread != nullptr && !thread->free && thread->started && !thread->almost_done)
		{
			return false;
		}
	}

	for (auto* thread: m_threads)
	{
		if (thread == nullptr || thread->free)
		{
			continue;
		}

		if (thread->started && !thread->host_detached)
		{
			const int result = pthread_join(thread->p, nullptr);
			if (result != 0)
			{
				return false;
			}
		}

		// Do not call guest TLS/key destructors while the cooperative stop flag
		// is set. RuntimeLinker removes every Program TLS allocation later in
		// the ordered reset, after all guest callbacks have been purged.
		{
			std::lock_guard name_lock(thread->name_mutex);
			thread->name.Clear();
		}
		thread->entry       = nullptr;
		thread->arg         = nullptr;
		thread->unique_id   = -1;
		thread->started     = false;
		thread->detached    = false;
		thread->host_detached = false;
		thread->almost_done = false;
		thread->cleanup_handlers.clear();
		(void)clear_pthread_guest_prefix(thread);
		thread->free        = true;
		if (thread->attr != nullptr)
		{
			PthreadAttrDestroy(&thread->attr);
		}
	}
	return true;
}
#endif

bool PthreadKeys::Create(int* key, pthread_key_destructor_func_t destructor)
{
	EXIT_IF(key == nullptr);

	Core::LockGuard lock(m_mutex);

	for (int index = 0; index < KEYS_MAX; index++)
	{
		auto& entry = m_keys[index];
		const auto generation = entry.generation.load(std::memory_order_relaxed);
		// Retire a saturated slot permanently instead of reusing a generation
		// which could make an old thread-local value visible again.
		if ((generation & 1u) == 0 && generation != UINT64_MAX - 1)
		{
			entry.destructor = destructor;
			entry.generation.store(generation + 1, std::memory_order_release);
			*key = index;
			return true;
		}
	}

	return false;
}

bool PthreadKeys::Delete(int key)
{
	if (key < 0 || key >= KEYS_MAX) return false;
	Core::LockGuard lock(m_mutex);

	auto& entry = m_keys[key];
	const auto generation = entry.generation.load(std::memory_order_relaxed);
	if ((generation & 1u) == 0) return false;

	entry.generation.store(generation + 1, std::memory_order_release);
	entry.destructor = nullptr;

	return true;
}

void PthreadKeys::ResetForNextSession()
{
	Core::LockGuard lock(m_mutex);
	for (auto& key: m_keys)
	{
		const auto generation = key.generation.load(std::memory_order_relaxed);
		if ((generation & 1u) != 0)
			key.generation.store(generation + 1, std::memory_order_release);
		key.destructor = nullptr;
	}
	s_specific_values = {};
}

void PthreadKeys::Destruct(int thread_id, bool execute_callbacks)
{
	// cleanup_thread runs on the exiting native pthread before publishing
	// almost_done. Joining/reaping peers never consumes another thread's TLS.
	EXIT_IF(thread_id != Core::Thread::GetThreadIdUnique());
	struct CallInfo
	{
		pthread_key_destructor_func_t destructor = nullptr;
		void*                         data = nullptr;
	};

	for (int iter = 0; execute_callbacks && iter < DESTRUCTOR_ITERATIONS; iter++)
	{
		bool invoked = false;
		for (int index = 0; index < KEYS_MAX; index++)
		{
			CallInfo call;
			{
				Core::LockGuard lock(m_mutex);
				auto& key = m_keys[index];
				auto& value = s_specific_values[index];
				const auto generation = key.generation.load(std::memory_order_relaxed);
				if ((generation & 1u) != 0 && generation == value.generation &&
				    key.destructor != nullptr && value.data != nullptr)
				{
					call = {key.destructor, value.data};
					// POSIX exposes null to this callback, but later keys stay
					// visible until their own turn. A callback can replace them.
					value.data = nullptr;
				}
			}
			if (call.destructor == nullptr) continue;
			invoked = true;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
			if (Loader::GuestExecutionStopRequested())
			{
				execute_callbacks = false;
				break;
			}
#endif
			// Guest callbacks may use pthread_{get,set}specific or delete a key. Do
			// not retain the registry lock across arbitrary guest/HLE execution, and
			// clear only this live binding immediately before its callback so an
			// earlier destructor can replace a later key in the same iteration.
#if defined(__aarch64__)
			if (Loader::IsNativeHostCallTarget(reinterpret_cast<uint64_t>(call.destructor)))
			{
				call.destructor(call.data);
			} else
			{
				Loader::RunGuestX64(reinterpret_cast<uint64_t>(call.destructor), reinterpret_cast<uint64_t>(call.data));
			}
#else
			call.destructor(call.data);
#endif
		}
		if (!invoked) break;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		if (!execute_callbacks || Loader::GuestExecutionStopRequested()) break;
#endif
	}

	// Thread-specific values have no lifetime after thread exit, including
	// keys without destructors and a value reinstalled by the final permitted
	// destructor iteration. Purge those records without invoking more guest
	// code; this is also the cooperative-stop path on persistent targets.
	s_specific_values = {};
}

bool PthreadKeys::Set(int key, void* data)
{
	if (key < 0 || key >= KEYS_MAX) return false;
	const auto generation = m_keys[key].generation.load(std::memory_order_acquire);
	if ((generation & 1u) == 0) return false;
	// A concurrent delete may linearize after the load. Its new generation
	// makes this value invisible even if this thread stores it afterwards.
	s_specific_values[key] = {generation, data};
	return true;
}

bool PthreadKeys::Get(int key, void** data)
{
	EXIT_IF(data == nullptr);
	if (key < 0 || key >= KEYS_MAX) return false;
	const auto generation = m_keys[key].generation.load(std::memory_order_acquire);
	if ((generation & 1u) == 0) return false;
	const auto& value = s_specific_values[key];
	*data = generation == value.generation ? value.data : nullptr;
	return true;
}

int LUMEN_SYSV_ABI PthreadMutexattrInit(PthreadMutexattr* attr)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Write); error != OK)
		return error;

	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attr == nullptr);

	*attr = new PthreadMutexattrPrivate {};

	int result = pthread_mutexattr_init(&(*attr)->p);

	result = (result == 0 ? PthreadMutexattrSettype(attr, 1) : result);
	result = (result == 0 ? PthreadMutexattrSetprotocol(attr, 0) : result);

	switch (result)
	{
		case 0: return OK;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadMutexattrDestroy(PthreadMutexattr* attr)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::ReadWrite); error != OK)
		return error;

	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attr == nullptr || *attr == nullptr);

	int result = pthread_mutexattr_destroy(&(*attr)->p);

	delete *attr;
	*attr = nullptr;

	switch (result)
	{
		case 0: return OK;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadMutexattrSettype(PthreadMutexattr* attr, int type)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;

	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attr == nullptr || *attr == nullptr);

	int ptype = PTHREAD_MUTEX_DEFAULT;
	switch (type)
	{
		// Use the non-recursive, error-reporting compatibility mapping documented
		// in MACOS_GAME_LIBRARY_COMPATIBILITY_REPORT.md. Host NORMAL can hang on
		// self-lock; the desktop lock path translates ERRORCHECK's EDEADLK.
		case 1:
		case 3:
		case 4: ptype = PTHREAD_MUTEX_ERRORCHECK; break;
		case 2: ptype = PTHREAD_MUTEX_RECURSIVE; break;
		default: EXIT("invalid type: %d\n", type);
	}

	int result = pthread_mutexattr_settype(&(*attr)->p, ptype);

	if (result == 0)
	{
		return OK;
	}

	return KERNEL_ERROR_EINVAL;
}

int LUMEN_SYSV_ABI PthreadMutexattrSetprotocol([[maybe_unused]] PthreadMutexattr* attr, int protocol)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;

	// PRINT_NAME();

	if (attr == nullptr || *attr == nullptr) return KERNEL_ERROR_EINVAL;

	[[maybe_unused]] int pprotocol = PTHREAD_PRIO_NONE;
	switch (protocol)
	{
		case 0: pprotocol = PTHREAD_PRIO_NONE; break;
		case 1: pprotocol = PTHREAD_PRIO_INHERIT; break;
		case 2:
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
			// Bionic does not expose the optional priority-ceiling protocol.
			// This wrapper already stores protocol metadata without applying it
			// to the host mutex, so preserve the guest value directly.
			pprotocol = 2;
#else
			pprotocol = PTHREAD_PRIO_PROTECT;
#endif
			break;
		default: return KERNEL_ERROR_EINVAL;
	}

	// protocol doesn't work in winpthreads
	int result         = 0; // pthread_mutexattr_setprotocol(&(*attr)->p, pprotocol);
	(*attr)->pprotocol = pprotocol;

	if (result == 0)
	{
		return OK;
	}

	return KERNEL_ERROR_EINVAL;
}

static int pthread_mutex_init_impl(PthreadMutex* mutex, const PthreadMutexattr* attr, const char* name, bool allow_retry)
{
	if (name != nullptr) PRINT_NAME();
	if (mutex == nullptr) return KERNEL_ERROR_EINVAL;
	NativeMutexDirectory::Garbage directory_garbage;
	bool retry_preparation = false;
#if defined(LUMEN_FEXCORE_RUNTIME)
	Core::VirtualMemory::GuestDataRangeSeal output_seal;
	{
		Core::VirtualMemory::GuestDataLease initial_output;
		if (!Core::VirtualMemory::TryAcquireGuestDataLease(reinterpret_cast<uint64_t>(mutex), sizeof(*mutex),
		        Core::VirtualMemory::Mode::Write, &initial_output)) return KERNEL_ERROR_EFAULT;
		// An unmanaged output keeps the ordinary no-retry path. A captured seal
		// permits donation without carrying a lease or trusting a reused address.
		if (allow_retry) retry_preparation = Core::VirtualMemory::CaptureGuestDataRangeSeal(initial_output, &output_seal);
	}
#else
	(void)allow_retry;
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	if (!Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(mutex), sizeof(*mutex), true))
		return KERNEL_ERROR_EFAULT;
#endif
#endif
	PthreadMutexattr source_attr = nullptr;
	if (attr == nullptr)
	{
		EXIT_IF(g_pthread_context == nullptr);
		source_attr = *g_pthread_context->GetDefaultMutexattr();
	} else
	{
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
		if (!Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(attr), &source_attr, sizeof(source_attr)))
			return KERNEL_ERROR_EFAULT;
#else
		source_attr = *attr;
#endif
	}
	if (source_attr == nullptr) return KERNEL_ERROR_EINVAL;
	std::string copied_name;
	if (!read_pthread_name(name, &copied_name)) return KERNEL_ERROR_EFAULT;

	// Consume all guest inputs before Core publishes the new mapping. Native
	// pthread state stays in this private wrapper; the guest sees only its ABI
	// prefix and zero padding, never a String, native mutex or owner counter.
	PreparedPthreadMutex private_object(nullptr, destroy_unpublished_mutex);
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
	if (g_explicit_mutex_preparation_test_hook != nullptr)
		g_explicit_mutex_preparation_test_hook(g_explicit_mutex_preparation_test_context);
#endif
	// prepare consumes the native attribute before its allocation can donate
	// the reader; no guest argument or registry lock survives that wait.
	const int result = prepare_mutex_guest_backing(source_attr, copied_name, retry_preparation, &private_object);
	if (result != OK) return result;
#if defined(LUMEN_FEXCORE_RUNTIME)
	Core::VirtualMemory::GuestDataLease final_output;
	if (!Core::VirtualMemory::TryAcquireGuestDataLease(reinterpret_cast<uint64_t>(mutex), sizeof(*mutex),
	        Core::VirtualMemory::Mode::Write, &final_output) ||
	    (output_seal.IsActive() && !Core::VirtualMemory::ValidateGuestDataRangeSeal(final_output, output_seal)))
		return KERNEL_ERROR_EFAULT;
#endif
	auto* object = private_object.get();
	const auto handle = mutex_guest_handle(object);
	{
		std::lock_guard lock(g_mutex_handles_mutex);
		register_mutex_handle_locked(object, directory_garbage);
#if defined(LUMEN_FEXCORE_RUNTIME)
		std::memcpy(reinterpret_cast<void*>(final_output.HostBegin()), &handle, sizeof(handle));
#elif defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
		// The non-FEX implementation performs native mapping checks/copy only.
		// FEX admission is completed by final_output before this registry lock.
		if (!Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(mutex), &handle, sizeof(handle)))
		{
			unregister_unpublished_mutex_locked(private_object, directory_garbage);
			return KERNEL_ERROR_EFAULT;
		}
#else
		*mutex = handle;
#endif
		private_object.release();
		if (mutex_directory_enabled()) NativeMutexDirectory::Reopen(object->directory_record);
	}
	if (name != nullptr) printf("\tmutex init: %s, %d\n", copied_name.c_str(), result);
	return OK;
}

int LUMEN_SYSV_ABI PthreadMutexInit(PthreadMutex* mutex, const PthreadMutexattr* attr, const char* name)
{
	return pthread_mutex_init_impl(mutex, attr, name, true);
}

#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK) && LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
uint64_t LUMEN_SYSV_ABI PthreadMutexInitializationFexContractTest(uint64_t scenario)
{
	using namespace Core::VirtualMemory;
	// 0: copied inputs; 1: unmapped output; 2: same-VA replacement;
	// 3: protection changed and restored; 4: explicitly disabled retry.
	if (scenario > 4 || Loader::TryGetActiveHostCallGuestGprs() == nullptr) return 0;
	constexpr uint64_t bytes = 16 * 1024;
	const uint64_t output = Alloc(0, bytes, Mode::ReadWrite);
	const uint64_t inputs = Alloc(0, bytes, Mode::ReadWrite);
	if (output == 0 || inputs == 0)
	{
		if (output != 0) Free(output);
		if (inputs != 0) Free(inputs);
		return 0;
	}
	PthreadMutexattr attribute = nullptr;
	if (PthreadMutexattrInit(&attribute) != OK || PthreadMutexattrSettype(&attribute, 2) != OK)
	{
		if (attribute != nullptr) PthreadMutexattrDestroy(&attribute);
		Free(inputs); Free(output);
		return 0;
	}
	constexpr char name[] = "explicit-mutex-retry";
	bool valid = Loader::WriteGuestMemory(inputs, &attribute, sizeof(attribute)) &&
	    Loader::WriteGuestMemory(inputs + 64, name, sizeof(name));
	struct FreshBacking
	{
		std::vector<PthreadMutexGuestPrefix*> saved;
		FreshBacking() { std::lock_guard lock(g_mutex_handles_mutex); saved.swap(g_retired_mutex_prefixes); }
		~FreshBacking()
		{
			std::lock_guard lock(g_mutex_handles_mutex);
			g_retired_mutex_prefixes.insert(g_retired_mutex_prefixes.end(), saved.begin(), saved.end());
		}
	} fresh_backing;
	struct Preparation
	{
		std::atomic<bool> entered {false};
		bool conflict = false, valid = true;
	} preparation;
	size_t handles_before = 0;
	{ std::lock_guard lock(g_mutex_handles_mutex); handles_before = g_mutex_handles.size(); }
	std::atomic<bool> published {false};
	bool writer_closed = false, writer_valid = false, output_live = true, inputs_live = true;
	std::thread writer([&] {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (!preparation.entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		if (!preparation.entered.load(std::memory_order_acquire)) return;
		const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV3(
		    LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2, nullptr);
		if (token == 0) return;
		{ std::lock_guard lock(g_mutex_handles_mutex); writer_valid = g_mutex_handles.size() == handles_before; }
		// The initializer must already have consumed both the guest inputs and
		// the native attribute before its backing allocation releases the reader.
		writer_valid = PthreadMutexattrDestroy(&attribute) == OK && writer_valid;
		inputs_live = !Free(inputs);
		writer_valid = !inputs_live && writer_valid;
		if (scenario == 1 || scenario == 2)
		{
			output_live = !Free(output);
			writer_valid = !output_live && writer_valid;
			if (scenario == 2 && !output_live)
			{
				output_live = AllocFixed(output, bytes, Mode::ReadWrite);
				writer_valid = output_live && writer_valid;
			}
		} else if (scenario == 3)
		{
			writer_valid = Protect(output, bytes, Mode::NoAccess) &&
			    Protect(output, bytes, Mode::ReadWrite) && writer_valid;
		}
		writer_closed = LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0,
		    [](void* opaque) noexcept { static_cast<std::atomic<bool>*>(opaque)->store(true, std::memory_order_release); },
		    &published) == 1;
	});
	g_explicit_mutex_preparation_test_context = &preparation;
	g_explicit_mutex_preparation_test_hook = [](void* opaque) {
		auto& state = *static_cast<Preparation*>(opaque);
		state.valid = !CurrentThreadHasMemoryLease();
		state.entered.store(true, std::memory_order_release);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (std::chrono::steady_clock::now() < deadline)
		{
			uint32_t retryable = 0;
			const auto probe = LumenAndroidFexCoreBeginGuestMemoryMutationV3(
			    LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_ADDITIVE_MAPPING_V2, &retryable);
			if (probe == 0) { state.conflict = retryable == 1; break; }
			state.valid = LumenAndroidFexCoreEndGuestMemoryMutationV2(probe, 0, 0, 0, nullptr, nullptr) == 1 && state.valid;
			std::this_thread::yield();
		}
	};
	const int result = scenario == 4
	    ? pthread_mutex_init_impl(reinterpret_cast<PthreadMutex*>(output), reinterpret_cast<const PthreadMutexattr*>(inputs),
	        reinterpret_cast<const char*>(inputs + 64), false)
	    : PthreadMutexInit(reinterpret_cast<PthreadMutex*>(output), reinterpret_cast<const PthreadMutexattr*>(inputs),
	        reinterpret_cast<const char*>(inputs + 64));
	g_explicit_mutex_preparation_test_hook = nullptr;
	g_explicit_mutex_preparation_test_context = nullptr;
	const bool published_before_return = published.load(std::memory_order_acquire);
	{
		Loader::ScopedGuestHostWait wait;
		valid = wait.IsActive() && valid;
		writer.join();
	}
	const int expected = scenario == 0 ? OK : scenario == 4 ? KERNEL_ERROR_EAGAIN : KERNEL_ERROR_EFAULT;
	valid = preparation.valid && preparation.conflict && writer_closed && writer_valid && result == expected &&
	    published_before_return == (scenario != 4) && !CurrentThreadHasMemoryLease() && valid;
	{ std::lock_guard lock(g_mutex_handles_mutex); valid = g_mutex_handles.size() == handles_before + (scenario == 0 ? 1u : 0u) && valid; }
	PthreadMutex handle = nullptr;
	if (output_live)
	{
		valid = Loader::ReadGuestMemory(output, &handle, sizeof(handle)) && valid;
		valid = (handle != nullptr) == (scenario == 0) && valid;
	}
	if (handle != nullptr)
	{
		const auto* native = resolve_mutex_handle(handle);
		valid = native != nullptr && std::strcmp(native->name.C_Str(), name) == 0 && valid;
		auto* slot = reinterpret_cast<PthreadMutex*>(output);
		const bool first = PthreadMutexLock(slot) == OK;
		const bool second = first && PthreadMutexLock(slot) == OK;
		if (second) valid = PthreadMutexUnlock(slot) == OK && valid;
		if (first) valid = PthreadMutexUnlock(slot) == OK && valid;
		valid = first && second && PthreadMutexDestroy(slot) == OK && valid;
	}
	if (attribute != nullptr) valid = PthreadMutexattrDestroy(&attribute) == OK && valid;
	if (inputs_live) valid = Free(inputs) && valid;
	if (output_live) valid = Free(output) && valid;
	bool handles_balanced = false;
	{ std::lock_guard lock(g_mutex_handles_mutex); handles_balanced = g_mutex_handles.size() == handles_before; }
	valid = handles_balanced && valid;
	std::fprintf(stderr, "[PTHREAD_MUTEX_INIT_FEX_TEST] case=%" PRIu64 " conflict=%u result=0x%08x"
	    " real-hle/core/writer=1 inputs-retired=1 native-handles-balanced=%u pass=%u\n",
	    scenario, preparation.conflict ? 1u : 0u, static_cast<unsigned>(result), handles_balanced ? 1u : 0u, valid ? 1u : 0u);
	return valid ? 1 : 0;
}
#endif

int LUMEN_SYSV_ABI PthreadMutexDestroy(PthreadMutex* mutex)
{
	PRINT_NAME();
	if (mutex == nullptr) return KERNEL_ERROR_EINVAL;
	NativeMutexDirectory::Garbage directory_garbage;
	const bool use_directory = mutex_directory_enabled();
	PthreadMutex handle = nullptr;
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	if (!Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(mutex), &handle, sizeof(handle)) ||
	    !Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(mutex), sizeof(*mutex), true))
		return KERNEL_ERROR_EFAULT;
#else
	handle = *mutex;
#endif
	PthreadMutex object = nullptr;
	PthreadMutexGuestPrefix* prefix = nullptr;
	{
		// Enrollment and the whole operation are pinned under this registry.
		// A queued/readmitting waiter must keep the wrapper alive even when its
		// native mutex is momentarily unowned, as with a native pthread waiter.
		std::lock_guard lock(g_mutex_handles_mutex);
		const auto found = g_mutex_handles.find(handle);
		if (found == g_mutex_handles.end()) return KERNEL_ERROR_EINVAL;
		object = found->second;
		if (use_directory)
		{
			if (!NativeMutexDirectory::TryClose(object->directory_record)) return KERNEL_ERROR_EBUSY;
		} else if (object->operation_pins != 0) return KERNEL_ERROR_EBUSY;
		const int result = pthread_mutex_destroy(&object->p);
		printf("\tmutex destroy: %s, %d\n", object->name.C_Str(), result);
		if (result != 0)
		{
			if (use_directory) NativeMutexDirectory::Reopen(object->directory_record);
			return result == EBUSY ? KERNEL_ERROR_EBUSY : KERNEL_ERROR_EINVAL;
		}
		prefix = object->guest_prefix;
		g_mutex_handles.erase(found);
		if (use_directory)
			EXIT_IF(!g_mutex_directory.Remove(object->directory_record,
			    [](PthreadMutexPrivate* retired) { delete retired; }, directory_garbage));
	}
	retire_mutex_guest_prefix(prefix);
	if (!use_directory) delete object;
	const PthreadMutex empty = nullptr;
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	if (!Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(mutex), &empty, sizeof(empty)))
		return KERNEL_ERROR_EFAULT;
#else
	*mutex = empty;
#endif
	return OK;
}

#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
bool PthreadMutexDirectoryContractTest()
{
	if (!mutex_directory_enabled()) return true;
	PthreadMutex mutex = nullptr;
	if (PthreadMutexInit(&mutex, nullptr, nullptr) != OK) return false;
	std::mutex gate;
	std::condition_variable changed;
	bool warm = false, start = false, done = false, valid = true;
	std::thread worker([&] {
		bool local = PthreadMutexLock(&mutex) == OK;
		if (local) local = PthreadMutexUnlock(&mutex) == OK;
		{
			std::unique_lock lock(gate);
			warm = true;
			changed.notify_all();
			changed.wait(lock, [&] { return start; });
		}
		const bool acquired = PthreadMutexLock(&mutex) == OK;
		local = acquired && local;
		if (acquired) local = PthreadMutexUnlock(&mutex) == OK && local;
		{
			std::lock_guard lock(gate);
			valid = local;
			done = true;
		}
		changed.notify_all();
	});
	bool progressed = false;
	{
		std::unique_lock lock(gate);
		changed.wait(lock, [&] { return warm; });
		// Exercise the real fresh-slot resolver, native lock, unlock and pins
		// while the directory's cold create/destroy registry is unavailable.
		std::lock_guard registry_lock(g_mutex_handles_mutex);
		start = true;
		changed.notify_all();
		progressed = changed.wait_for(lock, std::chrono::seconds(10), [&] { return done; });
	}
	worker.join();
	const bool destroyed = PthreadMutexDestroy(&mutex) == OK;
	std::fprintf(stderr, "[PTHREAD_MUTEX_DIRECTORY_PROGRESS_TEST] registry-held=1 fresh-slot/native-lock/unlock=%s\n",
	    progressed && valid && destroyed ? "pass" : "fail");
	return progressed && valid && destroyed;
}
#endif

int LUMEN_SYSV_ABI PthreadMutexLock(PthreadMutex* mutex)
{
	// PRINT_NAME();
	MutexErrorTrace trace(mutex);

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	PthreadMutex object = nullptr;
	const int resolve_result = pthread_static_objects->ResolveMutex(mutex, &object, true);
	if (resolve_result != OK) return trace.Result(resolve_result);
	ScopedMutexOperation operation(object);
	trace.Stage(MutexErrorStage::NativeTrylock);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_MACOS_FEXCORE_SEPARATE_PTHREAD_STACK)
#if defined(LUMEN_PTHREAD_MUTEX_QUEUED_ACQUISITION)
	const int caller = Core::Thread::GetThreadIdUnique();
#endif
	MutexWaitTicket ticket(object);
	int result = EBUSY;
	while (result == EBUSY
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	       && !Loader::GuestExecutionStopRequested()
#endif
	)
	{
		result = mutex_trylock(object, ticket.Waiter());
		if (g_mutex_error_trace != nullptr) trace.NativeResult(result);
		if (result == EBUSY)
		{
#if defined(LUMEN_PTHREAD_MUTEX_QUEUED_ACQUISITION)
			// trylock reports EBUSY for an ERRORCHECK mutex already owned by
			// the caller. Match the blocking lock contract instead of polling
			// forever; recursive mutexes have already succeeded above.
			if (object->owner.load(std::memory_order_relaxed) == caller)
			{
				trace.Stage(MutexErrorStage::SelfDeadlock);
				result = EDEADLK;
				break;
			}
#endif
			ticket.Enroll();
			{
				ScopedMutexErrorTracePause pause_trace;
				Loader::ScopedGuestHostWait host_wait;
				// Keep only the resolved host object during contention. Readmit
				// before trylock so no native mutex is held across a VM mutation.
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
				if (auto hook = g_mutex_wait_test_hook) hook();
#endif
				ticket.WaitForWake();
			}
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
			if (auto hook = g_mutex_readmission_test_hook) hook();
#endif
		}
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (result == EBUSY && Loader::GuestExecutionStopRequested())
	{
		return trace.Result(KERNEL_ERROR_ECANCELED);
	}
#endif
#else
	int result = pthread_mutex_lock(&object->p);
	if (result == 0) mutex_acquired(object);
	if (g_mutex_error_trace != nullptr) trace.NativeResult(result);
#endif
	if (g_mutex_error_trace != nullptr && result != 0) trace.Ownership(object);

	// printf("\tmutex lock: %s, %d\n", (*mutex)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return trace.Result(KERNEL_ERROR_EAGAIN);
		case EINVAL: return trace.Result(KERNEL_ERROR_EINVAL);
		case EDEADLK: return trace.Result(KERNEL_ERROR_EDEADLK);
		default: return trace.Result(KERNEL_ERROR_EINVAL);
	}
}

int LUMEN_SYSV_ABI PthreadMutexTrylock(PthreadMutex* mutex)
{
	// PRINT_NAME();

	if (mutex == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	// A static initializer leaves the slot zeroed until first use, exactly
	// like PthreadMutexLock and PthreadMutexTimedlock, so trylock must
	// materialise the same static object instead of failing closed.
	PthreadMutex object = nullptr;
	const int resolve_result = pthread_static_objects->ResolveMutex(mutex, &object, true);
	if (resolve_result != OK) return resolve_result;
	ScopedMutexOperation operation(object);

	int result = mutex_trylock(object);

	// printf("\tmutex trylock: %s, %d\n", (*mutex)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EBUSY: return KERNEL_ERROR_EBUSY;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadMutexTimedlock(PthreadMutex* mutex, uint64_t usec)
{
	PRINT_NAME();
	static_assert(sizeof(PthreadMutex) == 8 && sizeof(int) == 4);
	if (mutex == nullptr) return KERNEL_ERROR_EINVAL;
	EXIT_IF(g_pthread_context == nullptr);
	auto* objects = g_pthread_context->GetPthreadStaticObjects();
	EXIT_IF(objects == nullptr);
	PthreadMutex object = nullptr;
	const int resolve_result = objects->ResolveMutex(mutex, &object, true);
	if (resolve_result != OK) return resolve_result;
	ScopedMutexOperation operation(object);
	MutexWaitTicket ticket(object);

	const int caller = Core::Thread::GetThreadIdUnique();
	const auto start = std::chrono::steady_clock::now();
	const auto elapsed_usec = [&] {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		    std::chrono::steady_clock::now() - start).count());
	};
	// Immediate acquisition has priority even for a zero-length interval.
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (Loader::GuestExecutionStopRequested()) return KERNEL_ERROR_ECANCELED;
#endif
	int result = mutex_trylock(object, ticket.Waiter());
	while (result == EBUSY)
	{
		if (object->owner.load(std::memory_order_relaxed) == caller) return KERNEL_ERROR_EDEADLK;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		if (Loader::GuestExecutionStopRequested()) return KERNEL_ERROR_ECANCELED;
#endif
		const uint64_t elapsed = elapsed_usec();
		if (elapsed >= usec) return KERNEL_ERROR_ETIMEDOUT;
		const uint64_t remaining = usec - elapsed;
		ticket.Enroll();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_MACOS_FEXCORE_SEPARATE_PTHREAD_STACK)
		{
			Loader::ScopedGuestHostWait host_wait;
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
			if (auto hook = g_mutex_wait_test_hook) hook();
#endif
			ticket.WaitForWake(static_cast<uint32_t>(remaining > 1000 ? 1000 : remaining));
		}
#else
		Core::Thread::SleepMicro(static_cast<uint32_t>(remaining > 1000 ? 1000 : remaining));
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		if (Loader::GuestExecutionStopRequested()) return KERNEL_ERROR_ECANCELED;
#endif
		if (elapsed_usec() >= usec) return KERNEL_ERROR_ETIMEDOUT;
		result = mutex_trylock(object, ticket.Waiter());
	}
	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EDEADLK: return KERNEL_ERROR_EDEADLK;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadMutexUnlock(PthreadMutex* mutex)
{
	// PRINT_NAME();

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	PthreadMutex object = nullptr;
	const int resolve_result = pthread_static_objects->ResolveMutex(mutex, &object, true);
	if (resolve_result != OK) return resolve_result;
	ScopedMutexOperation operation(object);

	int result = mutex_unlock(object);

	// printf("\tmutex unlock: %s, %d\n", (*mutex)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;

		case EINVAL: return KERNEL_ERROR_EINVAL;
		case EPERM: return KERNEL_ERROR_EPERM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadAttrInit(PthreadAttr* attr)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Write); error != OK)
		return error;

	PRINT_NAME();

	*attr = new PthreadAttrPrivate {};

	int result = pthread_attr_init(&(*attr)->p);

	(*attr)->affinity   = 0x7f;
	(*attr)->guard_size = 0x1000;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	// A PS4 stack address describes guest memory. It must never become the
	// backing stack of the ARM64 interpreter thread. Keep guest metadata in the
	// wrapper and reserve a separate, comfortably sized Bionic host stack.
	result = (result == 0 ? pthread_attr_getstacksize(&(*attr)->p, &(*attr)->guest_stack_size) : result);
	constexpr size_t android_host_stack_size = 2u * 1024u * 1024u;
	result = (result == 0 ? pthread_attr_setstacksize(&(*attr)->p, android_host_stack_size) : result);
#elif defined(LUMEN_MACOS_FEXCORE_SEPARATE_PTHREAD_STACK)
	// The x86-64 stack is owned by RunGuestX64. Guest stack attributes must not
	// shrink or replace the Darwin stack which runs FEX and native HLE frames.
	result = (result == 0 ? pthread_attr_getstacksize(&(*attr)->p, &(*attr)->guest_stack_size) : result);
	constexpr size_t macos_fexcore_host_stack_size = 2u * 1024u * 1024u;
	result = (result == 0 ? pthread_attr_setstacksize(&(*attr)->p, macos_fexcore_host_stack_size) : result);
#endif

	KernelSchedParam param;
	param.sched_priority = 700;

	result = (result == 0 ? PthreadAttrSetinheritsched(attr, 4) : result);
	result = (result == 0 ? PthreadAttrSetschedparam(attr, &param) : result);
	result = (result == 0 ? PthreadAttrSetschedpolicy(attr, 1) : result);
	result = (result == 0 ? PthreadAttrSetdetachstate(attr, 0) : result);

	if (PRINT_NAME_ENABLED)
	{
		pthread_attr_dbg_print(attr);
	}

	switch (result)
	{
		case 0: return OK;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadAttrDestroy(PthreadAttr* attr)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::ReadWrite); error != OK)
		return error;

	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attr == nullptr || *attr == nullptr);

	int result = pthread_attr_destroy(&(*attr)->p);

	delete *attr;
	*attr = nullptr;

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int LUMEN_SYSV_ABI PthreadAttrGet(Pthread thread, PthreadAttr* attr)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;

	PRINT_NAME();
	thread = resolve_thread_handle(reinterpret_cast<uint64_t>(thread), true);

	if (thread == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	const int result = pthread_attr_copy(attr, &thread->attr);

#if defined(LUMEN_MACOS_FEXCORE_SEPARATE_PTHREAD_STACK)
	// A running guest thread executes on the private x86-64 stack owned by
	// RunGuestX64, which a creation attribute does not describe when the guest
	// never installed an explicit stack address. The console reports the actual
	// stack bounds of the queried thread here, and callers derive the stack top
	// as `base + size`: the stop-the-world capture of Hollow Knight fills its
	// thread records with that top and scans `[captured rsp, top)` from them, so
	// a zero base collapses the scan to nothing and the collector frees objects
	// that are still referenced from live stacks. Fill the live bounds for the
	// calling thread; other threads and explicitly addressed stacks keep the
	// copied guest metadata.
	if (result == 0 && thread == g_pthread_self && (*attr)->guest_stack_addr == nullptr)
	{
		uint64_t stack_base = 0;
		uint64_t stack_size = 0;
		if (Loader::CurrentGuestRunStackBounds(&stack_base, &stack_size))
		{
			(*attr)->guest_stack_addr = reinterpret_cast<void*>(static_cast<uintptr_t>(stack_base));
			(*attr)->guest_stack_size = stack_size;
		}
	}
#endif

	return result;
}

int LUMEN_SYSV_ABI PthreadAttrGetaffinity(const PthreadAttr* attr, KernelCpumask* mask)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;
	PthreadArgumentLease mask_lease;
	if (const int error = mask_lease.Acquire(mask, sizeof(*mask), Core::VirtualMemory::Mode::Write); error != OK)
		return error;

	PRINT_NAME();

	if (mask == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	return PthreadAttrGetaffinityHost(**attr, *mask);
}

int LUMEN_SYSV_ABI PthreadAttrGetdetachstate(const PthreadAttr* attr, int* state)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;
	PthreadArgumentLease state_lease;
	if (const int error = state_lease.Acquire(state, sizeof(*state), Core::VirtualMemory::Mode::Write); error != OK)
		return error;

	PRINT_NAME();

	if (state == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	return PthreadAttrGetdetachstateHost(**attr, *state);
}

int LUMEN_SYSV_ABI PthreadAttrGetguardsize(const PthreadAttr* attr, size_t* guard_size)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;
	PthreadArgumentLease guard_size_lease;
	if (const int error = guard_size_lease.Acquire(guard_size, sizeof(*guard_size), Core::VirtualMemory::Mode::Write); error != OK)
		return error;

	PRINT_NAME();

	if (guard_size == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	return PthreadAttrGetguardsizeHost(**attr, *guard_size);
}

int LUMEN_SYSV_ABI PthreadAttrGetinheritsched(const PthreadAttr* attr, int* inherit_sched)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;
	PthreadArgumentLease inherit_sched_lease;
	if (const int error = inherit_sched_lease.Acquire(inherit_sched, sizeof(*inherit_sched), Core::VirtualMemory::Mode::Write); error != OK)
		return error;

	PRINT_NAME();

	if (inherit_sched == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	return PthreadAttrGetinheritschedHost(**attr, *inherit_sched);
}

int LUMEN_SYSV_ABI PthreadAttrGetschedparam(const PthreadAttr* attr, KernelSchedParam* param)
{
	PRINT_NAME();

	static_assert(sizeof(PthreadAttr) == 8 && sizeof(KernelSchedParam) == 4);
	if (param == nullptr || attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
	PthreadAttr object = nullptr;
#if defined(__aarch64__)
	if (!Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(attr), &object, sizeof(object)))
		return KERNEL_ERROR_EFAULT;
#else
	std::memcpy(&object, attr, sizeof(object));
#endif
	if (object == nullptr) return KERNEL_ERROR_EINVAL;
#if defined(__aarch64__)
	if (!Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(param), sizeof(*param), true))
		return KERNEL_ERROR_EFAULT;
#endif

	KernelSchedParam output {};
	const int result = PthreadAttrGetschedparamHost(*object, output);
	if (result != OK) return result;
#if defined(__aarch64__)
	if (!Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(param), &output, sizeof(output)))
		return KERNEL_ERROR_EFAULT;
#else
	std::memcpy(param, &output, sizeof(output));
#endif
	return OK;
}

int LUMEN_SYSV_ABI PthreadAttrGetschedpolicy(const PthreadAttr* attr, int* policy)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;
	PthreadArgumentLease policy_lease;
	if (const int error = policy_lease.Acquire(policy, sizeof(*policy), Core::VirtualMemory::Mode::Write); error != OK)
		return error;

	PRINT_NAME();

	if (policy == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	return PthreadAttrGetschedpolicyHost(**attr, *policy);
}

int LUMEN_SYSV_ABI PthreadAttrGetstack(const PthreadAttr* __restrict attr, void** __restrict stack_addr, size_t* __restrict stack_size)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;
	PthreadArgumentLease stack_addr_lease;
	if (const int error = stack_addr_lease.Acquire(stack_addr, sizeof(*stack_addr), Core::VirtualMemory::Mode::Write); error != OK)
		return error;
	PthreadArgumentLease stack_size_lease;
	if (const int error = stack_size_lease.Acquire(stack_size, sizeof(*stack_size), Core::VirtualMemory::Mode::Write); error != OK)
		return error;

	PRINT_NAME();

	if (stack_size == nullptr || stack_addr == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || defined(LUMEN_MACOS_FEXCORE_SEPARATE_PTHREAD_STACK)
	*stack_addr = (*attr)->guest_stack_addr;
	*stack_size = (*attr)->guest_stack_size;
	int result1 = 0;
	int result2 = 0;
#else
	int result1 = pthread_attr_getstackaddr(&(*attr)->p, stack_addr);
	int result2 = pthread_attr_getstacksize(&(*attr)->p, stack_size);
#endif

	if (result1 == 0 && result2 == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int LUMEN_SYSV_ABI PthreadAttrGetstackaddr(const PthreadAttr* attr, void** stack_addr)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;
	PthreadArgumentLease stack_addr_lease;
	if (const int error = stack_addr_lease.Acquire(stack_addr, sizeof(*stack_addr), Core::VirtualMemory::Mode::Write); error != OK)
		return error;

	PRINT_NAME();

	if (stack_addr == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	return PthreadAttrGetstackaddrHost(**attr, *stack_addr);
}

int LUMEN_SYSV_ABI PthreadAttrGetstacksize(const PthreadAttr* attr, size_t* stack_size)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;
	PthreadArgumentLease stack_size_lease;
	if (const int error = stack_size_lease.Acquire(stack_size, sizeof(*stack_size), Core::VirtualMemory::Mode::Write); error != OK)
		return error;

	PRINT_NAME();

	if (stack_size == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	return PthreadAttrGetstacksizeHost(**attr, *stack_size);
}

int LUMEN_SYSV_ABI PthreadAttrSetaffinity(PthreadAttr* attr, KernelCpumask mask)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;

	PRINT_NAME();

	if (attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	(*attr)->affinity.store(mask, std::memory_order_relaxed);

	return OK;
}

int LUMEN_SYSV_ABI PthreadAttrSetdetachstate(PthreadAttr* attr, int state)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;

	PRINT_NAME();

	if (attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	int pstate = PTHREAD_CREATE_JOINABLE;
	switch (state)
	{
		case 0: pstate = PTHREAD_CREATE_JOINABLE; break;
		case 1: pstate = PTHREAD_CREATE_DETACHED; break;
		default: EXIT("unknown state: %d\n", state);
	}

	// int result = pthread_attr_setdetachstate(&(*attr)->p, pstate);
	int result = 0;

	(*attr)->detached = (pstate == PTHREAD_CREATE_DETACHED);

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int LUMEN_SYSV_ABI PthreadAttrSetguardsize(PthreadAttr* attr, size_t guard_size)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;

	PRINT_NAME();

	if (attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	(*attr)->guard_size = guard_size;

	return OK;
}

int LUMEN_SYSV_ABI PthreadAttrSetinheritsched(PthreadAttr* attr, int inherit_sched)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;

	PRINT_NAME();

	if (attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	[[maybe_unused]] int pinherit_sched = PTHREAD_INHERIT_SCHED;
	switch (inherit_sched)
	{
		case 0: pinherit_sched = PTHREAD_EXPLICIT_SCHED; break;
		case 4: pinherit_sched = PTHREAD_INHERIT_SCHED; break;
		default: EXIT("unknown inherit_sched: %d\n", inherit_sched);
	}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	(*attr)->inherit_sched = pinherit_sched;
	int result = 0;
#else
	int result = pthread_attr_setinheritsched(&(*attr)->p, pinherit_sched);
#endif

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int LUMEN_SYSV_ABI PthreadAttrSetschedparam(PthreadAttr* attr, const KernelSchedParam* param)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;
	PthreadArgumentLease param_lease;
	if (const int error = param_lease.Acquire(param, sizeof(*param), Core::VirtualMemory::Mode::Read); error != OK)
		return error;

	PRINT_NAME();

	if (param == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	// Bionic documents priority 0 as the only usable value for an app's
	// SCHED_OTHER threads. Preserve the exact Orbis priority as metadata and
	// leave the native attribute at its supported scheduler default.
	int result = 0;
#else
	sched_param pparam {};
	if (param->sched_priority <= 478)
	{
		pparam.sched_priority = +2;
	} else if (param->sched_priority >= 733)
	{
		pparam.sched_priority = -2;
	} else
	{
		pparam.sched_priority = 0;
	}

	int result = pthread_attr_setschedparam(&(*attr)->p, &pparam);
#endif

	if (result == 0)
	{
		(*attr)->guest_sched_priority = param->sched_priority;
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int LUMEN_SYSV_ABI PthreadAttrSetschedpolicy(PthreadAttr* attr, int policy)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;

	PRINT_NAME();

	if (attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	// winpthreads supports only SCHED_OTHER policy
	int ppolicy = SCHED_OTHER;

	(*attr)->policy = policy;

	int result = pthread_attr_setschedpolicy(&(*attr)->p, ppolicy);

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int LUMEN_SYSV_ABI PthreadAttrSetstack(PthreadAttr* attr, void* addr, size_t size)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;

	PRINT_NAME();

	if (addr == nullptr || size == 0 || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || defined(LUMEN_MACOS_FEXCORE_SEPARATE_PTHREAD_STACK)
	(*attr)->guest_stack_addr = addr;
	(*attr)->guest_stack_size = size;
	int result1 = 0;
	int result2 = 0;
#else
	int result1 = pthread_attr_setstackaddr(&(*attr)->p, addr);
	int result2 = pthread_attr_setstacksize(&(*attr)->p, size);
#endif

	if (result1 == 0 && result2 == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int LUMEN_SYSV_ABI PthreadAttrSetstackaddr(PthreadAttr* attr, void* addr)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;

	PRINT_NAME();

	if (addr == nullptr || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || defined(LUMEN_MACOS_FEXCORE_SEPARATE_PTHREAD_STACK)
	(*attr)->guest_stack_addr = addr;
	int result1 = 0;
#else
	int result1 = pthread_attr_setstackaddr(&(*attr)->p, addr);
#endif

	if (result1 == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int LUMEN_SYSV_ABI PthreadAttrSetstacksize(PthreadAttr* attr, size_t stack_size)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;

	PRINT_NAME();

	if (stack_size == 0 || attr == nullptr || *attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || defined(LUMEN_MACOS_FEXCORE_SEPARATE_PTHREAD_STACK)
	(*attr)->guest_stack_size = stack_size;
	int result2 = 0;
#else
	int result2 = pthread_attr_setstacksize(&(*attr)->p, stack_size);
#endif

	if (result2 == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int LUMEN_SYSV_ABI PthreadRwlockDestroy(PthreadRwlock* rwlock)
{
	PRINT_NAME();

	if (rwlock == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
#if defined(LUMEN_FEXCORE_RUNTIME)
	if (!Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(rwlock), sizeof(*rwlock), true)) return KERNEL_ERROR_EFAULT;
#endif

	EXIT_NOT_IMPLEMENTED(*rwlock == nullptr);

	int result = pthread_rwlock_destroy(&(*rwlock)->p);

	printf("\trwlock destroy: %s, %d\n", (*rwlock)->name.C_Str(), result);

	delete *rwlock;
	*rwlock = nullptr;

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int LUMEN_SYSV_ABI PthreadRwlockInit(PthreadRwlock* rwlock, const PthreadRwlockattr* attr, const char* name)
{
	PRINT_NAME();

	if (rwlock == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
	PthreadArgumentLease slot_lease;
	if (const int error = slot_lease.Acquire(rwlock, sizeof(*rwlock), Core::VirtualMemory::Mode::Write); error != OK)
		return error;
	PthreadArgumentLease attr_lease;
	if (attr != nullptr)
		if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
			return error;
	std::string copied_name;
	if (!read_pthread_name(name, &copied_name)) return KERNEL_ERROR_EFAULT;

	if (attr == nullptr)
	{
		EXIT_IF(g_pthread_context == nullptr);

		attr = g_pthread_context->GetDefaultRwlockattr();
	}

	*rwlock = new PthreadRwlockPrivate {};

	(*rwlock)->name = copied_name.c_str();

	int result = pthread_rwlock_init(&(*rwlock)->p, &(*attr)->p);

	printf("\trwlock init: %s, %d\n", (*rwlock)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EINVAL: return KERNEL_ERROR_EINVAL;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadRwlockRdlock(PthreadRwlock* rwlock)
{
	PRINT_NAME();

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	rwlock = static_cast<PthreadRwlock*>(pthread_static_objects->CreateObject(rwlock, PthreadStaticObject::Type::Rwlock));

	if (rwlock == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*rwlock == nullptr);
	PthreadRwlock const object = *rwlock;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	int result = EBUSY;
	while (result == EBUSY && !Loader::GuestExecutionStopRequested())
	{
		result = pthread_rwlock_tryrdlock(&object->p);
		if (result == EBUSY)
		{
			Loader::ScopedGuestHostWait host_wait;
			Core::Thread::SleepMicro(1000);
		}
	}
	if (result == EBUSY && Loader::GuestExecutionStopRequested())
	{
		return KERNEL_ERROR_ECANCELED;
	}
#else
	int result = pthread_rwlock_rdlock(&object->p);
#endif

	// printf("\trwlock rdlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadRwlockTimedrdlock(PthreadRwlock* rwlock, KernelUseconds usec)
{
	PRINT_NAME();

	if (rwlock == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*rwlock == nullptr);
	PthreadRwlock const object = *rwlock;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || \
    LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	auto end_time = std::chrono::steady_clock::now() + std::chrono::microseconds(usec);

	int result = 0;
	do
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		if (Loader::GuestExecutionStopRequested())
		{
			return KERNEL_ERROR_ECANCELED;
		}
#endif
		result = pthread_rwlock_tryrdlock(&object->p);
		if (result != EBUSY)
		{
			break;
		}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		{
			Loader::ScopedGuestHostWait host_wait;
			Core::Thread::SleepMicro(100);
		}
#else
		Core::Thread::SleepMicro(100);
#endif
	} while (std::chrono::steady_clock::now() < end_time);

	if (result == EBUSY)
	{
		result = ETIMEDOUT;
	}
#else
	timespec t {};
	usec_to_timespec(&t, usec);

	int result = pthread_rwlock_timedrdlock(&object->p, &t);
#endif

	// printf("\trwlock timedrdlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case ETIMEDOUT: return KERNEL_ERROR_ETIMEDOUT;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadRwlockTimedwrlock(PthreadRwlock* rwlock, KernelUseconds usec)
{
	PRINT_NAME();

	if (rwlock == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*rwlock == nullptr);
	PthreadRwlock const object = *rwlock;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || \
    LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	auto end_time = std::chrono::steady_clock::now() + std::chrono::microseconds(usec);

	int result = 0;
	do
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		if (Loader::GuestExecutionStopRequested())
		{
			return KERNEL_ERROR_ECANCELED;
		}
#endif
		result = pthread_rwlock_trywrlock(&object->p);
		if (result != EBUSY)
		{
			break;
		}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		{
			Loader::ScopedGuestHostWait host_wait;
			Core::Thread::SleepMicro(100);
		}
#else
		Core::Thread::SleepMicro(100);
#endif
	} while (std::chrono::steady_clock::now() < end_time);

	if (result == EBUSY)
	{
		result = ETIMEDOUT;
	}
#else
	timespec t {};
	usec_to_timespec(&t, usec);

	int result = pthread_rwlock_timedwrlock(&object->p, &t);
#endif

	// printf("\trwlock timedwrlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case ETIMEDOUT: return KERNEL_ERROR_ETIMEDOUT;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadRwlockTryrdlock(PthreadRwlock* rwlock)
{
	PRINT_NAME();

	if (rwlock == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*rwlock == nullptr);

	int result = pthread_rwlock_tryrdlock(&(*rwlock)->p);

	// printf("\trwlock tryrdlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EBUSY: return KERNEL_ERROR_EBUSY;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadRwlockTrywrlock(PthreadRwlock* rwlock)
{
	PRINT_NAME();

	if (rwlock == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*rwlock == nullptr);

	int result = pthread_rwlock_trywrlock(&(*rwlock)->p);

	// printf("\trwlock trywrlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EBUSY: return KERNEL_ERROR_EBUSY;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadRwlockUnlock(PthreadRwlock* rwlock)
{
	// PRINT_NAME();

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	rwlock = static_cast<PthreadRwlock*>(pthread_static_objects->CreateObject(rwlock, PthreadStaticObject::Type::Rwlock));

	if (rwlock == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*rwlock == nullptr);

	int result = pthread_rwlock_unlock(&(*rwlock)->p);

	// printf("\trwlock unlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;

		case EINVAL: return KERNEL_ERROR_EINVAL;
		case EPERM: return KERNEL_ERROR_EPERM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadRwlockWrlock(PthreadRwlock* rwlock)
{
	// PRINT_NAME();

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	rwlock = static_cast<PthreadRwlock*>(pthread_static_objects->CreateObject(rwlock, PthreadStaticObject::Type::Rwlock));

	if (rwlock == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*rwlock == nullptr);
	PthreadRwlock const object = *rwlock;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	int result = EBUSY;
	while (result == EBUSY && !Loader::GuestExecutionStopRequested())
	{
		result = pthread_rwlock_trywrlock(&object->p);
		if (result == EBUSY)
		{
			Loader::ScopedGuestHostWait host_wait;
			Core::Thread::SleepMicro(1000);
		}
	}
	if (result == EBUSY && Loader::GuestExecutionStopRequested())
	{
		return KERNEL_ERROR_ECANCELED;
	}
#else
	int result = pthread_rwlock_wrlock(&object->p);
#endif

	// printf("\trwlock wrlock: %s, %d\n", (*rwlock)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadRwlockattrDestroy(PthreadRwlockattr* attr)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::ReadWrite); error != OK)
		return error;

	PRINT_NAME();

	if (attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*attr == nullptr);

	int result = pthread_rwlockattr_destroy(&(*attr)->p);

	delete *attr;
	*attr = nullptr;

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int LUMEN_SYSV_ABI PthreadRwlockattrInit(PthreadRwlockattr* attr)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Write); error != OK)
		return error;

	PRINT_NAME();

	*attr = new PthreadRwlockattrPrivate {};

	int result = pthread_rwlockattr_init(&(*attr)->p);

	result = (result == 0 ? PthreadRwlockattrSettype(attr, 1) : result);

	switch (result)
	{
		case 0: return OK;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadRwlockattrGettype(PthreadRwlockattr* attr, int* type)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;
	PthreadArgumentLease type_lease;
	if (const int error = type_lease.Acquire(type, sizeof(*type), Core::VirtualMemory::Mode::Write); error != OK)
		return error;

	PRINT_NAME();

	if (type == nullptr || attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	*type = (*attr)->type;

	return OK;
}

int LUMEN_SYSV_ABI PthreadRwlockattrSettype(PthreadRwlockattr* attr, int type)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
		return error;

	PRINT_NAME();

	if (attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	(*attr)->type = type;

	return OK;
}

int LUMEN_SYSV_ABI PthreadCondattrDestroy(PthreadCondattr* attr)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::ReadWrite); error != OK)
		return error;

	PRINT_NAME();

	if (attr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*attr == nullptr);

	int result = pthread_condattr_destroy(&(*attr)->p);

	delete *attr;
	*attr = nullptr;

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int LUMEN_SYSV_ABI PthreadCondattrInit(PthreadCondattr* attr)
{
	PthreadArgumentLease attr_lease;
	if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Write); error != OK)
		return error;

	PRINT_NAME();

	*attr = new PthreadCondattrPrivate {};

	int result = pthread_condattr_init(&(*attr)->p);

	switch (result)
	{
		case 0: return OK;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadCondBroadcast(PthreadCond* cond)
{
	PRINT_NAME();
	const uint64_t original_slot = reinterpret_cast<uint64_t>(cond);

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	cond = static_cast<PthreadCond*>(pthread_static_objects->CreateObject(cond, PthreadStaticObject::Type::Cond));

	if (cond == nullptr)
	{
		trace_condition(ConditionTraceKind::Rejected, "broadcast", "resolve-rejected",
		                original_slot, 0, "kernel", KERNEL_ERROR_EINVAL);
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*cond == nullptr);

	PthreadCond const object = *cond;
	const uint64_t handle = reinterpret_cast<uint64_t>(object);
	int result = pthread_cond_broadcast(&object->p);
#if defined(LUMEN_PTHREAD_COND_WAKEUP_GENERATION)
	if (result == 0)
	{
		(*cond)->signal_generation.fetch_add(1, std::memory_order_release);
	}
#endif

	// A woken thread can finish using and destroy the condition before this
	// wrapper returns. Logging must not dereference its handle after the wake.
	printf("\tcond broadcast: slot=%p, %d\n", static_cast<void*>(cond), result);
	trace_condition(result == 0 ? ConditionTraceKind::Wake : ConditionTraceKind::Rejected,
	                "broadcast", "host-result", original_slot, handle, "host", result);

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int LUMEN_SYSV_ABI PthreadCondDestroy(PthreadCond* cond)
{
	PRINT_NAME();

	if (cond == nullptr)
	{
		trace_condition(ConditionTraceKind::Rejected, "destroy", "null-slot", 0, 0, "kernel", KERNEL_ERROR_EINVAL);
		return KERNEL_ERROR_EINVAL;
	}
#if defined(LUMEN_FEXCORE_RUNTIME)
	if (!Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(cond), sizeof(*cond), true)) return KERNEL_ERROR_EFAULT;
#endif

	EXIT_NOT_IMPLEMENTED(*cond == nullptr);

	PthreadCond const object = *cond;
	const uint64_t slot = reinterpret_cast<uint64_t>(cond);
	const uint64_t handle = reinterpret_cast<uint64_t>(object);
	int result = pthread_cond_destroy(&object->p);

	printf("\tcond destroy: %s, %d\n", (*cond)->name.C_Str(), result);

	delete *cond;
	*cond = nullptr;
	trace_condition(result == 0 ? ConditionTraceKind::Lifecycle : ConditionTraceKind::Rejected,
	                "destroy", "host-result-slot-cleared", slot, handle, "host", result);

	switch (result)
	{
		case 0: return OK;
		case EINVAL: return KERNEL_ERROR_EINVAL;
		case EBUSY: return KERNEL_ERROR_EBUSY;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadCondInit(PthreadCond* cond, const PthreadCondattr* attr, const char* name)
{
	PRINT_NAME();

	if (cond == nullptr)
	{
		trace_condition(ConditionTraceKind::Rejected, "init", "null-slot", 0, 0, "kernel", KERNEL_ERROR_EINVAL);
		return KERNEL_ERROR_EINVAL;
	}
	PthreadArgumentLease slot_lease;
	if (const int error = slot_lease.Acquire(cond, sizeof(*cond), Core::VirtualMemory::Mode::Write); error != OK)
		return error;
	PthreadArgumentLease attr_lease;
	if (attr != nullptr)
		if (const int error = attr_lease.Acquire(attr, sizeof(*attr), Core::VirtualMemory::Mode::Read); error != OK)
			return error;
	std::string copied_name;
	if (!read_pthread_name(name, &copied_name)) return KERNEL_ERROR_EFAULT;

	if (attr == nullptr)
	{
		EXIT_IF(g_pthread_context == nullptr);

		attr = g_pthread_context->GetDefaultCondattr();
	}

	*cond = new PthreadCondPrivate {};

	(*cond)->name = copied_name.c_str();

	PthreadCond const object = *cond;
	const uint64_t handle = reinterpret_cast<uint64_t>(object);
	int result = pthread_cond_init(&object->p, &(*attr)->p);

	printf("\tcond init: %s, %d\n", (*cond)->name.C_Str(), result);
	trace_condition(result == 0 ? ConditionTraceKind::Lifecycle : ConditionTraceKind::Rejected,
	                "init", "host-result-slot-published", reinterpret_cast<uint64_t>(cond), handle, "host", result);

	switch (result)
	{
		case 0: return OK;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EINVAL: return KERNEL_ERROR_EINVAL;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadCondSignal(PthreadCond* cond)
{
	PRINT_NAME();
	const uint64_t original_slot = reinterpret_cast<uint64_t>(cond);

	EXIT_IF(g_pthread_context == nullptr);

	auto* pthread_static_objects = g_pthread_context->GetPthreadStaticObjects();

	EXIT_IF(pthread_static_objects == nullptr);

	if (cond == nullptr)
	{
		trace_condition(ConditionTraceKind::Rejected, "signal", "null-slot", 0, 0, "kernel", KERNEL_ERROR_EINVAL);
		return KERNEL_ERROR_EINVAL;
	}

	// Mirror PthreadCondBroadcast: a static condition slot materialises on
	// first use instead of failing closed here.
	cond = static_cast<PthreadCond*>(pthread_static_objects->CreateObject(cond, PthreadStaticObject::Type::Cond));

	if (cond == nullptr)
	{
		trace_condition(ConditionTraceKind::Rejected, "signal", "resolve-rejected",
		                original_slot, 0, "kernel", KERNEL_ERROR_EINVAL);
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*cond == nullptr);

	PthreadCond const object = *cond;
	const uint64_t slot = reinterpret_cast<uint64_t>(cond);
	const uint64_t handle = reinterpret_cast<uint64_t>(object);
	int result = pthread_cond_signal(&object->p);
#if defined(LUMEN_PTHREAD_COND_WAKEUP_GENERATION)
	if (result == 0)
	{
		// Waking more than one polling waiter is an allowed spurious wakeup;
		// guest predicates still decide which thread can make progress.
		(*cond)->signal_generation.fetch_add(1, std::memory_order_release);
	}
#endif

	// printf("\tcond signal: %s, %d\n", (*cond)->name.C_Str(), result);
	trace_condition(result == 0 ? ConditionTraceKind::Wake : ConditionTraceKind::Rejected,
	                "signal", "host-result", slot, handle, "host", result);

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int LUMEN_SYSV_ABI PthreadCondSignalto(PthreadCond* cond, Pthread thread)
{
	PthreadArgumentLease cond_lease;
	if (const int error = cond_lease.Acquire(cond, sizeof(*cond), Core::VirtualMemory::Mode::Read); error != OK)
		return error;

	PRINT_NAME();
	thread = resolve_thread_handle(reinterpret_cast<uint64_t>(thread), true);

	if (cond == nullptr || thread == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(*cond == nullptr);

	int result = 0;

	LUMEN_NOT_IMPLEMENTED;

	// printf("\tcond signalto: %s, %d\n", (*cond)->name.C_Str(), result);

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int LUMEN_SYSV_ABI PthreadCondTimedwait(PthreadCond* cond, PthreadMutex* mutex, KernelUseconds usec)
{
	PRINT_NAME();

	PthreadCond  condition_object = nullptr;
	PthreadMutex mutex_object     = nullptr;
	const int resolve_result = resolve_condition_wait_objects(
	    cond, mutex, &condition_object, &mutex_object);
	if (resolve_result != OK) return resolve_result;
	ScopedMutexOperation operation(mutex_object);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	int result = runtime_cond_wait(condition_object, mutex_object, usec, false);
#else
	timespec t {};
	usec_to_timespec(&t, usec);
	int result = 0;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	result = macos_fex_cond_wait(condition_object, mutex_object, &t);
#else
	{
		MutexConditionOwnership ownership(mutex_object);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
		// The Orbis API timeout is relative. Darwin's POSIX
		// pthread_cond_timedwait() takes an absolute CLOCK_REALTIME deadline;
		// Darwin and winpthreads expose a relative extension directly.
		result = pthread_cond_timedwait_relative_np(&condition_object->p, &mutex_object->p, &t);
#else
		timespec absolute {};
		if (clock_gettime(CLOCK_REALTIME, &absolute) != 0)
		{
			result = EINVAL;
		}
		else
		{
			absolute.tv_sec += t.tv_sec;
			absolute.tv_nsec += t.tv_nsec;
			if (absolute.tv_nsec >= 1000000000)
			{
				++absolute.tv_sec;
				absolute.tv_nsec -= 1000000000;
			}
			result = pthread_cond_timedwait(&condition_object->p, &mutex_object->p, &absolute);
		}
#endif
	}
#endif
#endif

	// printf("\tcond timedwait: %s, %d\n", (*cond)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case ECANCELED: return KERNEL_ERROR_ECANCELED;
		case ETIMEDOUT: return KERNEL_ERROR_ETIMEDOUT;
		case EPERM: return KERNEL_ERROR_EPERM;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadCondTimedwaitAbsolute(PthreadCond* cond, PthreadMutex* mutex,
                                                const KernelTimespec* deadline_ptr)
{
	PRINT_NAME();

	if (cond == nullptr || mutex == nullptr || deadline_ptr == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	KernelTimespec deadline {};
	if (!Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(deadline_ptr),
	                             &deadline, sizeof(deadline)))
	{
		return KERNEL_ERROR_EFAULT;
	}
	if (deadline.tv_nsec < 0 || deadline.tv_nsec >= 1000000000)
	{
		return KERNEL_ERROR_EINVAL;
	}

	for (;;)
	{
		timespec now {};
		if (clock_gettime(CLOCK_REALTIME, &now) != 0)
		{
			return KERNEL_ERROR_EINVAL;
		}

		const bool expired = deadline.tv_sec < static_cast<int64_t>(now.tv_sec) ||
		    (deadline.tv_sec == static_cast<int64_t>(now.tv_sec) &&
		     deadline.tv_nsec <= static_cast<int64_t>(now.tv_nsec));
		KernelUseconds relative = 0;
		if (!expired)
		{
			// Re-sample realtime at most once per second.  This bounds host waits
			// and preserves an absolute deadline across realtime clock changes.
			const uint64_t seconds = static_cast<uint64_t>(deadline.tv_sec) -
			                         static_cast<uint64_t>(now.tv_sec);
			if (seconds >= 2)
			{
				relative = 1000000;
			}
			else
			{
				const int64_t nanos = static_cast<int64_t>(seconds) * 1000000000 +
				                      deadline.tv_nsec - static_cast<int64_t>(now.tv_nsec);
				const int64_t usecs = (nanos + 999) / 1000;
				relative = static_cast<KernelUseconds>(usecs > 1000000 ? 1000000 : usecs);
			}
		}
		const int result = PthreadCondTimedwait(
		    cond, mutex, relative);
		if (result != KERNEL_ERROR_ETIMEDOUT)
		{
			return result;
		}
		if (expired) return KERNEL_ERROR_ETIMEDOUT;
	}
}

int LUMEN_SYSV_ABI PthreadCondWait(PthreadCond* cond, PthreadMutex* mutex)
{
	PRINT_NAME();

	PthreadCond  condition_object = nullptr;
	PthreadMutex mutex_object     = nullptr;
	const int resolve_result = resolve_condition_wait_objects(
	    cond, mutex, &condition_object, &mutex_object);
	if (resolve_result != OK) return resolve_result;
	ScopedMutexOperation operation(mutex_object);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	int result = runtime_cond_wait(condition_object, mutex_object, 0, true);
#else
	int result = 0;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	result = macos_fex_cond_wait(condition_object, mutex_object, nullptr);
#else
	{
		MutexConditionOwnership ownership(mutex_object);
		result = pthread_cond_wait(&condition_object->p, &mutex_object->p);
	}
#endif
#endif

	// printf("\tcond wait: %s, %d\n", (*cond)->name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case ECANCELED: return KERNEL_ERROR_ECANCELED;
		case EPERM: return KERNEL_ERROR_EPERM;
		case EINVAL:
		default: return KERNEL_ERROR_EINVAL;
	}
}

Pthread LUMEN_SYSV_ABI PthreadSelf()
{
	// PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(g_pthread_self == nullptr);

	return pthread_guest_handle(g_pthread_self);
}

uint64_t PthreadGuestSelfHandle()
{
	return reinterpret_cast<uint64_t>(pthread_guest_handle(g_pthread_self));
}

class ScopedPthreadCancellationDisable
{
public:
	ScopedPthreadCancellationDisable()
	{
#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID
		m_valid = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &m_previous) == 0;
#endif
	}
	~ScopedPthreadCancellationDisable()
	{
#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID
		if (m_valid) (void)pthread_setcancelstate(m_previous, nullptr);
#endif
	}
	[[nodiscard]] bool Valid() const { return m_valid; }

private:
	#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID
	int  m_previous = PTHREAD_CANCEL_DISABLE;
	#endif
	bool m_valid = true;
};

static void invoke_pthread_cleanup_handler(const PthreadCleanupHandler& handler)
{
	if (handler.routine == nullptr) return;
#if defined(__aarch64__)
	if (Loader::IsNativeHostCallTarget(reinterpret_cast<uint64_t>(handler.routine)))
	{
		handler.routine(handler.argument);
	} else
	{
		(void)Loader::RunGuestX64(reinterpret_cast<uint64_t>(handler.routine),
		                          reinterpret_cast<uint64_t>(handler.argument));
	}
#else
	handler.routine(handler.argument);
#endif
}

static void drain_pthread_cleanup_handlers(Pthread thread, bool execute)
{
	while (!thread->cleanup_handlers.empty())
	{
		const auto handler = thread->cleanup_handlers.back();
		thread->cleanup_handlers.pop_back();
		if (execute) invoke_pthread_cleanup_handler(handler);
	}
}

void LUMEN_SYSV_ABI PthreadCleanupPush(pthread_cleanup_func_t routine, void* argument,
                                       PthreadCleanupInfo* info)
{
	PRINT_NAME();
	const uint64_t info_address = reinterpret_cast<uint64_t>(info);
	if (g_pthread_self == nullptr || routine == nullptr || info == nullptr ||
	    info_address % alignof(PthreadCleanupInfo) != 0)
	{
		EXIT("Invalid pthread cleanup push arguments");
		return;
	}
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	if (!Loader::ValidateGuestMemory(info_address, sizeof(*info), true))
	{
		EXIT("Invalid pthread cleanup storage");
		return;
	}
#endif
	ScopedPthreadCancellationDisable cancellation;
	if (!cancellation.Valid())
	{
		EXIT("Unable to protect pthread cleanup push from cancellation");
		return;
	}
	g_pthread_self->cleanup_handlers.push_back({routine, argument, info_address});
}

void LUMEN_SYSV_ABI PthreadCleanupPop(int execute)
{
	PRINT_NAME();
	if (g_pthread_self == nullptr)
	{
		EXIT("Pthread cleanup pop without a current thread");
		return;
	}
	PthreadCleanupHandler handler {};
	{
		ScopedPthreadCancellationDisable cancellation;
		if (!cancellation.Valid())
		{
			EXIT("Unable to protect pthread cleanup pop from cancellation");
			return;
		}
		if (g_pthread_self->cleanup_handlers.empty()) return;
		handler = g_pthread_self->cleanup_handlers.back();
		g_pthread_self->cleanup_handlers.pop_back();
	}
	if (execute != 0) invoke_pthread_cleanup_handler(handler);
}

static void cleanup_thread(void* arg)
{
	g_pthread_cleanup_active = false;
	auto* thread = static_cast<Pthread>(arg);

	EXIT_IF(g_pthread_context == nullptr);
	bool execute_cleanup = true;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (Loader::GuestExecutionStopRequested()) execute_cleanup = false;
#endif
	drain_pthread_cleanup_handlers(thread, execute_cleanup);

	auto thread_dtors = g_pthread_context->GetThreadDtors();

	if (thread_dtors != nullptr
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	    && !Loader::GuestExecutionStopRequested()
#endif
	)
	{
#if defined(__aarch64__)
		if (Loader::IsNativeHostCallTarget(reinterpret_cast<uint64_t>(thread_dtors)))
		{
			thread_dtors();
		} else
		{
			Loader::RunGuestX64(reinterpret_cast<uint64_t>(thread_dtors));
		}
#else
		thread_dtors();
#endif
	}

	bool execute_key_destructors = execute_cleanup;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	// A stop may have been requested while the earlier cleanup callbacks ran.
	// Discard key state but do not enter new guest code once shutdown starts.
	execute_key_destructors = execute_key_destructors && !Loader::GuestExecutionStopRequested();
#endif
	g_pthread_context->GetPthreadKeys()->Destruct(thread->unique_id, execute_key_destructors);

	thread->almost_done = true;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	invalidate_guest_root_slot(thread);
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	Loader::GuestExecutionActivityEndAllForCurrentThread();
#endif
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
	if (auto hook = g_after_cleanup_test_hook.load()) hook(pthread_guest_handle(thread));
#endif
}

static void* run_thread(void* arg)
{
	auto* thread = static_cast<Pthread>(arg);
	void* ret    = nullptr;

	thread->unique_id = Core::Thread::GetThreadIdUnique();

	g_pthread_self = thread;
	thread->cleanup_handlers.clear();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	attach_guest_root_diagnostics();
#endif

	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
	pthread_cleanup_push(cleanup_thread, thread);
	g_pthread_cleanup_active = true;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	Loader::GuestExecutionActivityBegin();
#endif

	thread->started = true;

#if defined(__aarch64__)
	if (
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	    !Loader::GuestExecutionStopRequested() &&
#endif
	    Loader::IsNativeHostCallTarget(reinterpret_cast<uint64_t>(thread->entry)))
	{
		ret = thread->entry(thread->arg);
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	else if (!Loader::GuestExecutionStopRequested())
#else
	else
#endif
	{
		ret = reinterpret_cast<void*>(Loader::RunGuestX64(reinterpret_cast<uint64_t>(thread->entry),
		                                                  reinterpret_cast<uint64_t>(thread->arg)));
	}
#else
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (!Loader::GuestExecutionStopRequested())
	{
		ret = thread->entry(thread->arg);
	}
#else
	ret = thread->entry(thread->arg);
#endif
#endif

	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
	pthread_cleanup_pop(1);
	return ret;
}

int LUMEN_SYSV_ABI PthreadCreate(Pthread* thread, const PthreadAttr* attr, pthread_entry_func_t entry, void* arg, const char* name)
{
	PRINT_NAME();
	if (thread == nullptr) return KERNEL_ERROR_EINVAL;
	EXIT_IF(g_pthread_context == nullptr);
	auto* pthread_pool = g_pthread_context->GetPthreadPool();
	EXIT_IF(pthread_pool == nullptr);

	// Pool creation may release the HLE reader while preparing VM ownership.
	// Snapshot the guest attribute cell and name before that wait, then clone
	// the independently live native attributes into private creation state.
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	if (!Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(thread), sizeof(*thread), true))
		return KERNEL_ERROR_EFAULT;
#endif
	PthreadAttr source_attr = nullptr;
	if (attr == nullptr)
	{
		source_attr = *g_pthread_context->GetDefaultAttr();
	} else
	{
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
		if (!Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(attr), &source_attr, sizeof(source_attr)))
			return KERNEL_ERROR_EFAULT;
#else
		source_attr = *attr;
#endif
	}
	if (source_attr == nullptr) return KERNEL_ERROR_EINVAL;

	std::string copied_name;
	if (name != nullptr)
	{
		const auto address = reinterpret_cast<uint64_t>(name);
		for (uint64_t offset = 0; ; ++offset)
		{
			char byte = 0;
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
			if (address > UINT64_MAX - offset ||
			    !Loader::ReadGuestMemory(address + offset, &byte, sizeof(byte)))
				return KERNEL_ERROR_EFAULT;
#else
			byte = name[offset];
#endif
			if (byte == '\0') break;
			copied_name.push_back(byte);
		}
	}

	struct CreationAttributes
	{
		PthreadAttr value = nullptr;
		~CreationAttributes() { if (value != nullptr) PthreadAttrDestroy(&value); }
	} copied_attr;
	PthreadAttrInit(&copied_attr.value);
	int result = pthread_attr_copy(&copied_attr.value, &source_attr);
	int inherit_sched = 0;
	if (result == 0) result = PthreadAttrGetinheritschedHost(*copied_attr.value, inherit_sched);
	if (result != 0) return KERNEL_ERROR_EINVAL;
	int effective_policy = copied_attr.value->policy;
	int guest_priority = copied_attr.value->guest_sched_priority;
	if (inherit_sched == 4 && g_pthread_self != nullptr)
	{
		std::lock_guard lock(g_pthread_self->scheduling_mutex);
		effective_policy = g_pthread_self->effective_guest_policy;
		guest_priority = g_pthread_self->guest_sched_priority;
	}

	int allocation_error = KERNEL_ERROR_EAGAIN;
	auto* native_thread = pthread_pool->Create(&allocation_error);
	if (native_thread == nullptr) return allocation_error;
	if (native_thread->attr != nullptr) PthreadAttrDestroy(&native_thread->attr);
	native_thread->attr = copied_attr.value;
	copied_attr.value = nullptr;
	{
		std::lock_guard name_lock(native_thread->name_mutex);
		native_thread->name = copied_name.c_str();
	}
	native_thread->entry = entry;
	native_thread->arg = arg;
	native_thread->almost_done = false;
	native_thread->detached = native_thread->attr->detached;
	// Native threads remain joinable so the reaper proves exit before recycle.
	native_thread->host_detached = false;
	native_thread->started = false;
	native_thread->unique_id = -1;
	native_thread->pending_exception_signal.store(0, std::memory_order_release);
	native_thread->boundary_interrupt_token.store(0, std::memory_order_release);
	native_thread->effective_guest_policy = effective_policy;
	native_thread->guest_sched_priority = guest_priority;

	// Revalidate and write the guest output only after reader readmission.
	const auto guest_handle = pthread_guest_handle(native_thread);
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	if (!Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(thread), &guest_handle, sizeof(guest_handle)))
	{
		native_thread->detached = false;
		native_thread->free = true;
		return KERNEL_ERROR_EFAULT;
	}
#else
	*thread = guest_handle;
#endif

	PRINT_NAME_ENABLE(false);
	result = pthread_create(&native_thread->p, &native_thread->attr->p, run_thread, native_thread);
	if (result == 0)
	{
		while (!native_thread->started) Core::Thread::SleepMicro(1000);
	} else
	{
		native_thread->detached = false;
	}
	const String created_name = pthread_name_snapshot(native_thread);
	printf("\tthread create: %s, id = %d, %d\n", created_name.C_Str(), native_thread->unique_id, result);
	pthread_attr_dbg_print(&native_thread->attr);
	PRINT_NAME_ENABLE(true);
	// Failure diagnostics still read this wrapper and its attribute snapshot.
	// Release it for reuse only after the last native access by this caller.
	if (result != 0) native_thread->free = true;

	switch (result)
	{
		case 0: return OK;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case EDEADLK: return KERNEL_ERROR_EDEADLK;
		case EPERM: return KERNEL_ERROR_EPERM;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadDetach(Pthread thread)
{
	PRINT_NAME();
	if (thread == nullptr) return KERNEL_ERROR_EINVAL;
	thread = resolve_thread_handle(reinterpret_cast<uint64_t>(thread), true);
	if (thread == nullptr) return KERNEL_ERROR_ESRCH;

	const int claimed = claim_pthread_join(thread);
	if (claimed != OK) return claimed;
	PthreadJoinClaim claim {thread};

	const String detached_name = pthread_name_snapshot(thread);
	printf("\tthread detach: %s, %d\n", detached_name.C_Str(), 0);

	// Detach changes guest ownership only. Every pool-owned native pthread
	// stays joinable so the reaper can prove native exit before recycling it.
	thread->detached = true;

	return OK;
}

int LUMEN_SYSV_ABI PthreadJoin(Pthread thread, void** value)
{
	PRINT_NAME();
	if (thread == nullptr) return KERNEL_ERROR_EINVAL;
	thread = resolve_thread_handle(reinterpret_cast<uint64_t>(thread), true);
	if (thread == nullptr) return KERNEL_ERROR_ESRCH;

	const int claimed = claim_pthread_join(thread);
	if (claimed != OK) return claimed;
	if (thread->detached)
	{
		PthreadJoinClaim claim {thread};
		return KERNEL_ERROR_EINVAL;
	}
	return join_claimed_pthread(thread, value);
}

static int join_claimed_pthread(Pthread thread, void** value)
{
	PthreadJoinClaim claim {thread};
	const pthread_t native_thread = thread->p;
	if (pthread_equal(native_thread, pthread_self())) return KERNEL_ERROR_EDEADLK;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	while (!thread->almost_done && !thread->free && !Loader::GuestExecutionStopRequested())
	{
		Loader::ScopedGuestHostWait host_wait;
		Core::Thread::SleepMicro(1000);
	}
	if (thread->free)
	{
		return KERNEL_ERROR_ESRCH;
	}
	if (Loader::GuestExecutionStopRequested() && !thread->almost_done)
	{
		return KERNEL_ERROR_ECANCELED;
	}
#endif
#if defined(__aarch64__)
	if (value != nullptr && !Loader::ValidateGuestMemory(
	        reinterpret_cast<uint64_t>(value), sizeof(*value), true))
	{
		return KERNEL_ERROR_EFAULT;
	}
#endif
	void* joined_value = nullptr;
	int result = 0;
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_MACOS_FEXCORE_SEPARATE_PTHREAD_STACK)
		Loader::ScopedGuestHostWait host_wait;
#endif
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
		if (auto hook = g_before_native_join_test_hook.load()) hook(pthread_guest_handle(thread));
#endif
		result = pthread_join(native_thread, &joined_value);
		// Successful join consumes the handle before host-wait readmission.
		if (result == 0) claim.release = false;
	}

	if (PRINT_NAME_ENABLED)
	{
		const String joined_name = pthread_name_snapshot(thread);
		printf("\tthread join: %s, %d\n", joined_name.C_Str(), result);
	}

	if (result == 0)
	{
		// The native handle has been consumed. Keep the claim even if cleanup
		// unwinds; no second caller may join that handle again.
		bool output_ok = true;
		if (value != nullptr)
		{
#if defined(__aarch64__)
			// Revalidate after host-wait readmission; no guest pointer or lease was
			// retained by the native join.
			output_ok = Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(value), &joined_value, sizeof(joined_value));
#else
			*value = joined_value;
#endif
		}
		const int id = thread->unique_id;
		auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();
		rt->DeleteTlss(id);
		// A native reaper may find admission temporarily closed by a mutation.
		// No old root can access the prefix now; Create must clear it before reuse.
		(void)clear_pthread_guest_prefix(thread);
		thread->almost_done = false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		release_guest_root_slot(thread);
#endif
		// Publish reaper/recycle eligibility only after the previous generation's
		// guest prefix, TLS and diagnostic root identity have all been retired.
		thread->free = true;
		return output_ok ? OK : KERNEL_ERROR_EFAULT;
	}

	switch (result)
	{
		case ESRCH: return KERNEL_ERROR_ESRCH;
		case EDEADLK: return KERNEL_ERROR_EDEADLK;
		case EOPNOTSUPP: return KERNEL_ERROR_EOPNOTSUPP;
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI PthreadCancel(Pthread thread)
{
	PRINT_NAME();

	if (thread == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
	thread = resolve_thread_handle(reinterpret_cast<uint64_t>(thread), true);
	if (thread == nullptr) return KERNEL_ERROR_ESRCH;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	// Bionic deliberately omits POSIX thread cancellation. Returning success
	// without integrating every blocking cancellation point could strand a
	// target thread and deadlock a later join, so keep this fail-closed.
	if (thread->free || thread->almost_done)
	{
		return KERNEL_ERROR_ESRCH;
	}
	return KERNEL_ERROR_EOPNOTSUPP;
#else
	int result = pthread_cancel(thread->p);

	const String cancelled_name = pthread_name_snapshot(thread);
	printf("\tthread cancel: %s, %d\n", cancelled_name.C_Str(), result);

	switch (result)
	{
		case 0: return OK;
		case ESRCH: return KERNEL_ERROR_ESRCH;
		default: return KERNEL_ERROR_EINVAL;
	}
#endif
}

int LUMEN_SYSV_ABI PthreadGetaffinity(Pthread thread, KernelCpumask* mask)
{
	PRINT_NAME();
	thread = resolve_thread_handle(reinterpret_cast<uint64_t>(thread), true);

	if (thread == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	// Guest affinity is logical state, shared with Setaffinity and AttrGet.
	// Host scheduling does not define the guest CPU mask.
	return PthreadAttrGetaffinity(&thread->attr, mask);
}

int32_t AppCpuModeFromAttributes(uint32_t attributes)
{
	// ATTRIBUTE bits 15 and 16 encode the application's CPU mode. They do
	// not describe the host core count or the current thread's affinity mask.
	switch ((attributes >> 15U) & 3U)
	{
		case 2: return 5;
		case 3: return 2;
		default: return 0;
	}
}

int32_t LUMEN_SYSV_ABI KernelGetCpumode()
{
	PRINT_NAME();
	int32_t attributes = 0;
	// Query current session metadata on each call; never retain a preceding
	// application's mode. Missing ATTRIBUTE uses the local base mode zero.
	if (!Loader::SystemContentParamSfoGetInt("ATTRIBUTE", &attributes)) return 0;
	return AppCpuModeFromAttributes(static_cast<uint32_t>(attributes));
}

int LUMEN_SYSV_ABI PthreadSetaffinity(Pthread thread, KernelCpumask mask)
{
	PRINT_NAME();
	thread = resolve_thread_handle(reinterpret_cast<uint64_t>(thread), true);

	if (thread == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	auto result = PthreadAttrSetaffinity(&thread->attr, mask);

	return result;
}

int LUMEN_SYSV_ABI PthreadSetcancelstate(int state, int* old_state)
{
	PRINT_NAME();

	if (state != 0 && state != 1)
	{
		return KERNEL_ERROR_EINVAL;
	}
	if (old_state != nullptr &&
	    !Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(old_state),
	                                 sizeof(*old_state), true))
	{
		return KERNEL_ERROR_EFAULT;
	}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	// Bionic has no native cancellation state to change.  Keep the call
	// fail-closed instead of claiming a state that pthread_cancel cannot honor.
	return KERNEL_ERROR_EOPNOTSUPP;
#else
	const int pstate = state == 0 ? PTHREAD_CANCEL_ENABLE : PTHREAD_CANCEL_DISABLE;
	int previous_host_state = PTHREAD_CANCEL_DISABLE;
	int result = pthread_setcancelstate(pstate,
	                                    old_state == nullptr ? nullptr : &previous_host_state);

	printf("\tthread setcancelstate: %d\n", result);
	if (result != 0)
	{
		return KERNEL_ERROR_EINVAL;
	}
	if (old_state != nullptr)
	{
		int previous_guest_state = 0;
		if (previous_host_state == PTHREAD_CANCEL_ENABLE)
		{
			previous_guest_state = 0;
		}
		else if (previous_host_state == PTHREAD_CANCEL_DISABLE)
		{
			previous_guest_state = 1;
		}
		else
		{
			(void)pthread_setcancelstate(previous_host_state, nullptr);
			return KERNEL_ERROR_EINVAL;
		}
		if (!Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(old_state),
		                              &previous_guest_state,
		                              sizeof(previous_guest_state)))
		{
			(void)pthread_setcancelstate(previous_host_state, nullptr);
			return KERNEL_ERROR_EFAULT;
		}
	}
	return OK;
#endif
}

int LUMEN_SYSV_ABI PthreadSetcanceltype(int type, int* old_type)
{
	PRINT_NAME();

	if (type != 0 && type != 2)
	{
		return KERNEL_ERROR_EINVAL;
	}
	if (old_type != nullptr &&
	    !Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(old_type), sizeof(*old_type), true))
	{
		return KERNEL_ERROR_EFAULT;
	}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	return KERNEL_ERROR_EOPNOTSUPP;
#else
	const int ptype = type == 0 ? PTHREAD_CANCEL_DEFERRED : PTHREAD_CANCEL_ASYNCHRONOUS;
	int previous_host_type = PTHREAD_CANCEL_DEFERRED;
	const int result = pthread_setcanceltype(ptype, old_type == nullptr ? nullptr : &previous_host_type);

	printf("\tthread setcanceltype: %d\n", result);
	if (result != 0)
	{
		return KERNEL_ERROR_EINVAL;
	}
	if (old_type != nullptr)
	{
		int previous_guest_type = 0;
		if (previous_host_type == PTHREAD_CANCEL_DEFERRED)
		{
			previous_guest_type = 0;
		}
		else if (previous_host_type == PTHREAD_CANCEL_ASYNCHRONOUS)
		{
			previous_guest_type = 2;
		}
		else
		{
			(void)pthread_setcanceltype(previous_host_type, nullptr);
			return KERNEL_ERROR_EINVAL;
		}
		if (!Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(old_type), &previous_guest_type, sizeof(previous_guest_type)))
		{
			(void)pthread_setcanceltype(previous_host_type, nullptr);
			return KERNEL_ERROR_EFAULT;
		}
	}
	return OK;
#endif
}

static int read_pthread_scheduling(Pthread thread, int* policy, int* prio)
{
	std::lock_guard lock(thread->scheduling_mutex);
	sched_param param {};
	int         pol = 0;

	int result = pthread_getschedparam(thread->p, &pol, &param);

	if (result == 0)
	{
		*prio = thread->guest_sched_priority;

		if (policy != nullptr) *policy = thread->effective_guest_policy;
		return OK;
	}

	return KERNEL_ERROR_EINVAL;
}

int LUMEN_SYSV_ABI PthreadGetschedparam(Pthread thread, uint32_t* policy, KernelSchedParam* param)
{
	PRINT_NAME();
	thread = resolve_thread_handle(reinterpret_cast<uint64_t>(thread), true);
	static_assert(sizeof(int) == 4 && sizeof(uint32_t) == 4 && sizeof(KernelSchedParam) == 4);
	static_assert(offsetof(KernelSchedParam, sched_priority) == 0);
	if (thread == nullptr) return KERNEL_ERROR_ESRCH;
	if (policy == nullptr || param == nullptr) return KERNEL_ERROR_EINVAL;
#if defined(__aarch64__)
	if (!Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(policy), sizeof(*policy), true) ||
	    !Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(param), sizeof(*param), true))
		return KERNEL_ERROR_EFAULT;
#endif
	int guest_policy = 0;
	KernelSchedParam guest_param {};
	const int result = read_pthread_scheduling(thread, &guest_policy, &guest_param.sched_priority);
	if (result != OK) return result;
	const auto guest_policy_value = static_cast<uint32_t>(guest_policy);
#if defined(__aarch64__)
	if (!Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(policy), &guest_policy_value, sizeof(guest_policy_value)) ||
	    !Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(param), &guest_param, sizeof(guest_param)))
		return KERNEL_ERROR_EFAULT;
#else
	std::memcpy(policy, &guest_policy_value, sizeof(guest_policy_value));
	std::memcpy(param, &guest_param, sizeof(guest_param));
#endif
	printf("\t PthreadGetschedparam: %d, policy = %u, priority = %d\n",
	       thread->unique_id, guest_policy_value, guest_param.sched_priority);
	return OK;
}

int LUMEN_SYSV_ABI PthreadGetprio(Pthread thread, int* prio)
{
	PRINT_NAME();
	thread = resolve_thread_handle(reinterpret_cast<uint64_t>(thread), true);
	if (thread == nullptr) return KERNEL_ERROR_ESRCH;
	PthreadArgumentLease prio_lease;
	if (const int error = prio_lease.Acquire(prio, sizeof(*prio), Core::VirtualMemory::Mode::Write); error != OK)
		return error;
	int output = 0;
	const int result = read_pthread_scheduling(thread, nullptr, &output);
	if (result == OK)
	{
		*prio = output;
		printf("\t PthreadGetprio: %d, %d\n", thread->unique_id, output);
	}
	return result;
}

static int write_pthread_scheduling(Pthread thread, int priority, const uint32_t* guest_policy)
{
	std::lock_guard lock(thread->scheduling_mutex);
	sched_param native_param {};
	int native_policy = 0;
	if (pthread_getschedparam(thread->p, &native_policy, &native_param) != 0) return KERNEL_ERROR_EINVAL;
#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID
	if (priority <= 478)
	{
		native_param.sched_priority = +2;
	} else if (priority >= 733)
	{
		native_param.sched_priority = -2;
	} else
	{
		native_param.sched_priority = 0;
	}
	// Match the existing attribute policy model; guest FIFO/RR remain logical.
	if (guest_policy != nullptr) native_policy = SCHED_OTHER;
	if (pthread_setschedparam(thread->p, native_policy, &native_param) != 0) return KERNEL_ERROR_EINVAL;
#endif
	thread->guest_sched_priority = priority;
	if (guest_policy != nullptr) thread->effective_guest_policy = static_cast<int>(*guest_policy);
	return OK;
}

int LUMEN_SYSV_ABI PthreadSetschedparam(Pthread thread, uint32_t policy, const KernelSchedParam* param)
{
	PRINT_NAME();
	thread = resolve_thread_handle(reinterpret_cast<uint64_t>(thread), true);
	if (thread == nullptr) return KERNEL_ERROR_ESRCH;
	if (param == nullptr || policy < 1 || policy > 3) return KERNEL_ERROR_EINVAL;
	KernelSchedParam requested {};
#if defined(__aarch64__)
	if (!Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(param), &requested, sizeof(requested)))
		return KERNEL_ERROR_EFAULT;
#else
	std::memcpy(&requested, param, sizeof(requested));
#endif
	const int result = write_pthread_scheduling(thread, requested.sched_priority, &policy);
	if (result == OK) printf("\t PthreadSetschedparam: %d, policy = %u, priority = %d\n",
	                         thread->unique_id, policy, requested.sched_priority);
	return result;
}

int LUMEN_SYSV_ABI PthreadSetprio(Pthread thread, int prio)
{
	PRINT_NAME();
	thread = resolve_thread_handle(reinterpret_cast<uint64_t>(thread), true);
	if (thread == nullptr) return KERNEL_ERROR_ESRCH;
	const int result = write_pthread_scheduling(thread, prio, nullptr);
	if (result == OK) printf("\t PthreadSetprio: %d, %d\n", thread->unique_id, prio);
	return result;
}

void LUMEN_SYSV_ABI PthreadTestcancel()
{
	PRINT_NAME();

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	// No pending cancellation can exist because PthreadCancel fails closed.
#else
	pthread_testcancel();
#endif
}

void LUMEN_SYSV_ABI PthreadExit(void* value)
{
	PRINT_NAME();

	pthread_exit(value);
}

bool PthreadHasManagedExitBoundary()
{
	return g_pthread_cleanup_active;
}

int LUMEN_SYSV_ABI PthreadEqual(Pthread thread1, Pthread thread2)
{
	// PRINT_NAME();
	const auto native1 = resolve_thread_handle(reinterpret_cast<uint64_t>(thread1), true);
	const auto native2 = resolve_thread_handle(reinterpret_cast<uint64_t>(thread2), true);
	return (native1 != nullptr && native1 == native2 ? 1 : 0);
}

int LUMEN_SYSV_ABI PthreadGetname(Pthread thread, char* name)
{
	PRINT_NAME();
	thread = resolve_thread_handle(reinterpret_cast<uint64_t>(thread), true);

	if (thread == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	if (name == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	std::array<char, 32> output {};
	{
		std::lock_guard lock(thread->name_mutex);
		std::strncpy(output.data(), thread->name.C_Str(), output.size() - 1);
	}
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	if (!Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(name), output.data(), output.size()))
		return KERNEL_ERROR_EFAULT;
#else
	std::memcpy(name, output.data(), output.size());
#endif

	return OK;
}

int LUMEN_SYSV_ABI PthreadRename(Pthread thread, const char* name)
{
	PRINT_NAME();
	thread = resolve_thread_handle(reinterpret_cast<uint64_t>(thread), true);

	if (thread == nullptr || thread->free) return KERNEL_ERROR_ESRCH;
	if (name == nullptr) return KERNEL_ERROR_EINVAL;

	constexpr size_t capacity = 32;
	std::array<char, capacity> input {};
	size_t length = 0;
	for (; length < input.size(); ++length)
	{
		char value = 0;
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
		const uint64_t address = reinterpret_cast<uint64_t>(name);
		if (address > std::numeric_limits<uint64_t>::max() - length ||
		    !Loader::ReadGuestMemory(address + length, &value, sizeof(value)))
		{
			return KERNEL_ERROR_EFAULT;
		}
#else
		value = name[length];
#endif
		if (value == '\0') break;
		input[length] = value;
	}
	if (length == input.size()) return KERNEL_ERROR_ENAMETOOLONG;

	{
		std::lock_guard lock(thread->name_mutex);
		thread->name = String::FromUtf8(input.data());
	}
	return OK;
}

void LUMEN_SYSV_ABI PthreadYield()
{
	PRINT_NAME();

	sched_yield();
}

int LUMEN_SYSV_ABI PthreadGetthreadid()
{
	PRINT_NAME();

	return Core::Thread::GetThreadIdUnique();
}

// PS4 kernel clock ids (`ORBIS_CLOCK_*`, from public reverse engineering of
// the Orbis kernel): 0 realtime, 4 monotonic, 5 uptime, 7 uptime precise, 8
// uptime fast, 9 realtime precise, 10 realtime fast, 11 monotonic precise, 12
// monotonic fast, 13 second, 14 thread cputime id, 15 proctime. The host
// provides the realtime and monotonic families, so uptime and the
// precise/fast variants collapse onto the closest one; proctime (15) keeps
// the microsecond process grid of `sceKernelGetProcessTime`. The CPU-time and
// extended network clock ids stay fail-closed: they have no faithful host
// counterpart and no product need yet.
int LUMEN_SYSV_ABI KernelClockGetres(KernelClockid clock_id, KernelTimespec* tp)
{
	PRINT_NAME();

	if (tp == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}
	PthreadArgumentLease output_lease;
	if (output_lease.Acquire(tp, sizeof(*tp), Core::VirtualMemory::Mode::Write) != OK)
		return KERNEL_ERROR_EFAULT;

	if (clock_id == 15)
	{
		tp->tv_sec  = 0;
		tp->tv_nsec = 1000;
		return OK;
	}

	clockid_t pclock_id = CLOCK_REALTIME;
	switch (clock_id)
	{
		case 0:
		case 9:
		case 10: pclock_id = CLOCK_REALTIME; break;
		case 4:
		case 5:
		case 7:
		case 8:
		case 11:
		case 12:
		case 13: pclock_id = CLOCK_MONOTONIC; break;
		default: EXIT("unknown clock_id: %d", clock_id);
	}

	timespec t {};

	int result = clock_getres(pclock_id, &t);

	tp->tv_sec  = t.tv_sec;
	tp->tv_nsec = t.tv_nsec;

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int LUMEN_SYSV_ABI KernelClockGettime(KernelClockid clock_id, KernelTimespec* tp)
{
	PRINT_NAME();

	if (tp == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}
	PthreadArgumentLease output_lease;
	if (output_lease.Acquire(tp, sizeof(*tp), Core::VirtualMemory::Mode::Write) != OK)
		return KERNEL_ERROR_EFAULT;

	return KernelClockGettimeHost(clock_id, *tp);
}

int KernelClockGettimeHost(KernelClockid clock_id, KernelTimespec& value)
{
	if (clock_id == 15)
	{
		const uint64_t us = KernelGetProcessTime();
		value.tv_sec     = static_cast<int64_t>(us / 1000000);
		value.tv_nsec    = static_cast<int64_t>((us % 1000000) * 1000);
		return OK;
	}

	clockid_t pclock_id = CLOCK_REALTIME;
	switch (clock_id)
	{
		case 0:
		case 9:
		case 10: pclock_id = CLOCK_REALTIME; break;
		case 4:
		case 5:
		case 7:
		case 8:
		case 11:
		case 12:
		case 13: pclock_id = CLOCK_MONOTONIC; break;
		default:
		{
			// Local numeric record on the fatal path: the clock id and the guest
			// caller, so an unknown id can be traced back to its module offset.
			const uint64_t* gprs = Loader::TryGetActiveHostCallGuestGprs();
			uint64_t        guest_ret = 0;
			if (gprs != nullptr)
			{
				(void)Loader::ReadGuestMemory(gprs[4], &guest_ret, sizeof(guest_ret));
			}
			fprintf(stderr, "clock_id=%d guest_rsp=0x%016" PRIx64 " guest_ret=0x%016" PRIx64 "\n", clock_id,
			        gprs != nullptr ? gprs[4] : 0, guest_ret);
			std::fflush(stderr);
			EXIT("unknown clock_id: %d", clock_id);
		}
	}

	timespec t {};

	int result = clock_gettime(pclock_id, &t);

	value.tv_sec  = t.tv_sec;
	value.tv_nsec = t.tv_nsec;

	if (result == 0)
	{
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

int LUMEN_SYSV_ABI KernelGettimeofday(KernelTimeval* tp)
{
	PRINT_NAME();

	if (tp == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	timespec t {};
	if (clock_gettime(CLOCK_REALTIME, &t) == 0)
	{
		const KernelTimeval value {
		    .tv_sec  = static_cast<int64_t>(t.tv_sec),
		    .tv_usec = static_cast<int64_t>(t.tv_nsec / 1000),
		};
		if (!Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(tp), &value,
		                              sizeof(value)))
		{
			return KERNEL_ERROR_EFAULT;
		}
		return OK;
	}
	return KERNEL_ERROR_EINVAL;
}

uint64_t LUMEN_SYSV_ABI KernelGetTscFrequency()
{
#if defined(LUMEN_FEXCORE_RUNTIME)
	LumenAndroidFexCoreGuestTscV1 sample;
	const uint32_t code = LumenAndroidFexCoreQueryGuestTscV1(&sample);
	EXIT_IF(code != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1);
	return sample.frequency;
#else
	return Core::Timer::QueryPerformanceFrequency();
#endif
}

uint64_t LUMEN_SYSV_ABI KernelReadTsc()
{
#if defined(LUMEN_FEXCORE_RUNTIME)
	LumenAndroidFexCoreGuestTscV1 sample;
	const uint32_t code = LumenAndroidFexCoreQueryGuestTscV1(&sample);
	EXIT_IF(code != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1);
	return sample.counter;
#else
	return Core::Timer::QueryPerformanceCounter();
#endif
}

uint64_t LUMEN_SYSV_ABI KernelGetProcessTime()
{
	return static_cast<uint64_t>(Loader::Timer::GetTimeMs() * 1000.0);
}

uint64_t LUMEN_SYSV_ABI KernelGetProcessTimeCounter()
{
	return Loader::Timer::GetCounter();
}

uint64_t LUMEN_SYSV_ABI KernelGetProcessTimeCounterFrequency()
{
	return Loader::Timer::GetFrequency();
}

void LUMEN_SYSV_ABI KernelSetThreadDtors(thread_dtors_func_t dtors)
{
	PRINT_NAME();

	EXIT_IF(g_pthread_context == nullptr);

	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());
	EXIT_NOT_IMPLEMENTED(g_pthread_context->GetThreadDtors() != nullptr);

	g_pthread_context->SetThreadDtors(dtors);
	// g_thread_dtors = dtors;
}

// --- PS4 exception delivery (libkernel_unity) --------------------------------
//
// KernelRaiseException resolves the target through the live pthread pool and
// queues one request on the wrapper. A target executing translated code is
// interrupted at its next generated-code boundary: the raiser arms the FEX
// interrupt page of the thread's current run (the same doorbell the VM
// mutation barrier uses) and the target consumes the request in its pause
// path with the complete boundary state. A target already inside an HLE
// import or host wait consumes it at its next HLE boundary (published by
// RuntimeLinker) or between blocking host-wait slices (ScopedGuestHostWait).
// Both routes run the installed handler as a nested guest call with the
// ucontext reconstructed from the interrupted GP state, and then resume the
// interrupted code, import or wait.

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
static thread_local PthreadSleepSliceTestHook g_sleep_slice_test_hook = nullptr;
void PthreadSetSleepSliceTestHook(PthreadSleepSliceTestHook hook)
{
	g_sleep_slice_test_hook = hook;
}
#endif
// Check exceptions after bounded native slices. The opt-in path can keep an
// active reader donation across slices without a request. Delivery and final
// return use the ordinary readmission; total requested duration is unchanged.
static void sleep_host_with_exception_delivery(uint64_t nanos)
{
	static const bool retain_wait = [] {
		const char* value = std::getenv("LUMEN_FEX_SLEEP_RETAIN_HOST_WAIT");
		const char* ledger = std::getenv("LUMEN_MACOS_VM_LEDGER");
		// The diagnostic ledger uses the ordinary slice destructor as its tick.
		const bool enabled = value != nullptr && std::strcmp(value, "1") == 0 &&
		    !(ledger != nullptr && std::strcmp(ledger, "1") == 0);
		if (enabled) std::fputs("[FEX_SLEEP_WAIT] retain_host_wait=1 scope=native-sleep-only\n", stderr);
		return enabled;
	}();
	Detail::SleepWithHostWaitSlices<Loader::ScopedGuestHostWait>(nanos, retain_wait,
	    [](uint64_t slice) {
		    Core::Thread::SleepNano(slice);
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
		    if (auto hook = g_sleep_slice_test_hook) hook(slice);
#endif
	    },
	    [] {
		    // The current thread owns its host wrapper for its entire lifetime.
		    // Do not inspect guest state or consume the request while quiescent:
		    // the ordinary wait destructor readmits and performs delivery.
		    return g_pthread_self != nullptr &&
		        g_pthread_self->pending_exception_signal.load(std::memory_order_acquire) != 0;
	    });
}
#endif

void PthreadBuildExceptionContext(const uint64_t gpr[16], uint64_t resume_rip, uint8_t* context)
{
	if (context == nullptr) return;
	std::memset(context, 0, kExceptionContextSize);
	const auto store = [context](size_t offset, uint64_t value)
	{
		std::memcpy(context + offset, &value, sizeof(value));
	};
	// mcontext+0x08..: the interrupted general-purpose registers in the order
	// the guest handler reads them. The mc_onstack slot at mcontext+0x00 stays
	// zero: this is not an altstack signal frame.
	store(kExceptionMcRdiOffset, gpr[7]);
	store(kExceptionMcRsiOffset, gpr[6]);
	store(kExceptionMcRdxOffset, gpr[2]);
	store(kExceptionMcRcxOffset, gpr[1]);
	store(kExceptionMcR8Offset, gpr[8]);
	store(kExceptionMcR9Offset, gpr[9]);
	store(kExceptionMcRaxOffset, gpr[0]);
	store(kExceptionMcRbxOffset, gpr[3]);
	store(kExceptionMcRbpOffset, gpr[5]);
	store(kExceptionMcR10Offset, gpr[10]);
	store(kExceptionMcR11Offset, gpr[11]);
	store(kExceptionMcR12Offset, gpr[12]);
	store(kExceptionMcR13Offset, gpr[13]);
	store(kExceptionMcR14Offset, gpr[14]);
	store(kExceptionMcR15Offset, gpr[15]);
	// mc_rip is the resume address of the interrupted HLE call and mc_rsp the
	// interrupted stack pointer (pointing at that call's return-address slot).
	store(kExceptionMcRipOffset, resume_rip);
	store(kExceptionMcRspOffset, gpr[4]);
}

#if defined(__aarch64__)
namespace
{
// The generated-code boundary route exists only where the FEX adapter owns the
// interrupt page and the guest-call path supports nested handler calls.
constexpr bool g_fex_boundary_interrupt_available =
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__aarch64__) && \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
    true;
#else
    false;
#endif

// Both delivery routes share one guard: a queued request must not enter a
// second handler while the first one is still running on this thread.
thread_local bool g_exception_delivery_active = false;

struct ExceptionDeliveryGuard final
{
	explicit ExceptionDeliveryGuard(bool& flag): active(flag) { active = true; }
	~ExceptionDeliveryGuard() { active = false; }
	bool& active;
};

// Host identity of a guest thread as the FEX adapter captured it: the
// wrapper stores the host pthread object, which is an opaque pointer on Darwin
// and an integer on Android.
uint64_t host_thread_identity(pthread_t value)
{
#if defined(__APPLE__)
	return reinterpret_cast<uint64_t>(value);
#else
	return static_cast<uint64_t>(value);
#endif
}

// Arms the FEX interrupt page of the target's current run, if any. A failure
// or an inactive run leaves the ordinary boundary routes in place.
void arm_boundary_exception_interrupt(Pthread thread)
{
	if (!g_fex_boundary_interrupt_available || thread == nullptr) return;
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__aarch64__) && \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	// The wrapper holds the host pthread object created for this thread; the
	// FEX adapter captured the same value from pthread_self at run admission.
	const uint64_t host_thread = host_thread_identity(thread->p);
	const uint64_t token = LumenAndroidFexCoreRequestGuestBoundaryInterruptV1(host_thread);
	thread->boundary_interrupt_token.store(token, std::memory_order_release);
#endif
}

void cancel_boundary_exception_interrupt(Pthread thread)
{
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__aarch64__) && \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	if (thread == nullptr) return;
	const uint64_t token = thread->boundary_interrupt_token.exchange(0, std::memory_order_acq_rel);
	if (token != 0)
	{
		const int32_t result = LumenAndroidFexCoreCancelGuestBoundaryInterruptV1(token);
		// Bounded numeric record: token in the target slot, host identity in
		// the handler slot, so a replay shows which route consumed the request.
		KernelTraceExceptionEvent("cancel", 0, token, host_thread_identity(thread->p), result);
	}
#else
	(void)thread;
#endif
}

// Runs the installed handler as exactly one nested guest call with the given
// interrupted state. The caller consumed the pending request first. `boundary`
// selects the generated-code operation names so a replay distinguishes the
// block-boundary route from the HLE boundary and host-wait slice route.
bool run_exception_handler(uint32_t signum, uint64_t handler, const uint64_t* gprs, uint64_t resume_rip,
                           bool boundary)
{
	const char* const op_begin = boundary ? "deliver-boundary-begin" : "deliver-begin";
	const char* const op_state = boundary ? "deliver-boundary-state" : "deliver-state";
	const char* const op_end = boundary ? "deliver-boundary-end" : "deliver-end";
	alignas(16) static thread_local uint8_t context[kExceptionContextSize];
	PthreadBuildExceptionContext(gprs, resume_rip, context);
	const uint64_t target = reinterpret_cast<uint64_t>(pthread_guest_handle(g_pthread_self));
	KernelTraceExceptionEvent(op_begin, static_cast<int32_t>(signum), target, handler, OK);
	// The interrupted state stays numeric: RSP in the target slot and the
	// resume RIP in the handler slot of this record.
	KernelTraceExceptionEvent(op_state, static_cast<int32_t>(signum), gprs[4], resume_rip, OK);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	// Local diagnostic: repeat the bounded managed-region snapshots when the
	// handler starts (the capture point) and when the nested handler returns
	// (after the stop-the-world collection the title's handler waits for), so a
	// later fault can be bisected against the raise and delivery points. Both
	// snapshots share the same point number. Gated and bounded by
	// LUMEN_MACOS_VM_REGION_SNAPSHOT; a no-op when it is disabled.
	static std::atomic<uint32_t> exception_snapshot_points {0};
	const uint32_t snapshot_point =
	    exception_snapshot_points.fetch_add(1, std::memory_order_relaxed) + 1;
	std::array<char, 16> snapshot_tag {};
	std::snprintf(snapshot_tag.data(), snapshot_tag.size(), "deliver%u", snapshot_point);
	// Local diagnostic: seal the delivered state as this thread's published
	// boundary state and repeat the bounded guest-root snapshot, so the capture
	// point can be compared offline with the freed block the later fault names.
	// Gated and bounded by LUMEN_MACOS_VM_ROOT_SNAPSHOT; a no-op when disabled.
	{
		uint64_t guest_stack_base = 0;
		uint64_t guest_stack_size = 0;
		(void)Loader::CurrentGuestRunStackBounds(&guest_stack_base, &guest_stack_size);
		KernelGuestThreadPublishBoundaryState(
		    gprs, resume_rip, Core::VirtualMemory::CaptureDiagnosticHostCall(),
		    guest_stack_base, guest_stack_size);
		snapshot_guest_thread_roots(snapshot_tag.data());
	}
	(void)Core::VirtualMemory::SnapshotRecordedRegions(snapshot_tag.data());
#endif
	Loader::RunGuestX64(handler, static_cast<uint64_t>(static_cast<int64_t>(signum)), reinterpret_cast<uint64_t>(context));
	KernelTraceExceptionEvent(op_end, static_cast<int32_t>(signum), target, handler, OK);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	std::snprintf(snapshot_tag.data(), snapshot_tag.size(), "handled%u", snapshot_point);
	// The roots capture runs before the region dumps: the fault that ends the
	// run can arrive a few milliseconds after the handler returns, and the
	// bounded roots records must not wait behind the file writes.
	snapshot_guest_thread_roots(snapshot_tag.data());
	(void)Core::VirtualMemory::SnapshotRecordedRegions(snapshot_tag.data());
#endif
	return true;
}
} // namespace
#endif

int32_t PthreadQueueExceptionForHandle(uint64_t handle, int32_t signum)
{
	Pthread thread = resolve_thread_handle(handle);
	if (thread == nullptr) return KERNEL_ERROR_ESRCH;
	thread->pending_exception_signal.store(static_cast<uint32_t>(signum), std::memory_order_release);
#if defined(__aarch64__)
	// Make the request ready at the target's next generated-code boundary; a
	// target that stays in host code or an HLE wait keeps the ordinary route.
	arm_boundary_exception_interrupt(thread);
	// Bounded numeric record: token in the target slot (zero records that no
	// FEX run was active) and the host thread identity in the handler slot.
	KernelTraceExceptionEvent("arm", signum,
	                          thread->boundary_interrupt_token.load(std::memory_order_acquire),
	                          host_thread_identity(thread->p), OK);
#endif
	return OK;
}

bool PthreadDeliverPendingException(const uint64_t* interrupted_gprs)
{
#if defined(__aarch64__)
	Pthread thread = g_pthread_self;
	if (thread == nullptr || g_exception_delivery_active) return false;
	const uint32_t signum = thread->pending_exception_signal.load(std::memory_order_acquire);
	if (signum == 0) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (Loader::GuestExecutionStopRequested()) return false;
#endif
	const uint64_t handler = KernelExceptionHandlerForSignal(static_cast<int32_t>(signum));
	if (handler == 0)
	{
		// The session reset the handler registry while the request was queued:
		// nothing can run, and the request must not survive the session.
		thread->pending_exception_signal.store(0, std::memory_order_release);
		cancel_boundary_exception_interrupt(thread);
		return false;
	}
	const uint64_t* gprs = interrupted_gprs != nullptr ? interrupted_gprs : Loader::TryGetActiveHostCallGuestGprs();
	if (gprs == nullptr)
	{
		// No interrupted state to describe yet: keep the request queued for the
		// next boundary or host-wait slice of this thread.
		return false;
	}
	// Consume before entering the handler so the handler's own HLE boundaries
	// and waits cannot re-deliver the same request.
#if defined(LUMEN_FEXCORE_RUNTIME)
	// A short service may have kept its execution reader. Delivery itself has
	// memory/callback effects; promote at the actual delivery point so a signal
	// queued after effect classification cannot enter a nested call unadmitted.
	if (!GuestCpu::EnsureOrdinaryHostCallAdmission()) return false;
#endif
	thread->pending_exception_signal.store(0, std::memory_order_release);
	// This boundary state is the delivered one; an armed generated-code
	// boundary for the same request is no longer needed.
	cancel_boundary_exception_interrupt(thread);

	uint64_t resume_rip = 0;
	// Best-effort resume address: the return-address slot of the interrupted
	// HLE call. An unreadable slot publishes a zero rip rather than dropping
	// the capture; the stack pointer and the registers still describe the
	// interrupted thread.
	(void)Loader::ReadGuestMemory(gprs[4], &resume_rip, sizeof(resume_rip));

	ExceptionDeliveryGuard guard(g_exception_delivery_active);
	return run_exception_handler(signum, handler, gprs, resume_rip, false);
#else
	(void)interrupted_gprs;
	return false;
#endif
}

bool PthreadDeliverPendingExceptionAtBoundary(const uint64_t gpr[16], uint64_t resume_rip)
{
#if defined(__aarch64__)
	Pthread thread = g_pthread_self;
	if (thread == nullptr || gpr == nullptr || resume_rip == 0) return false;
	// The FEX adapter consumed the one-shot arm before this call: one delivery
	// attempt per armed boundary, even when nothing runs.
	thread->boundary_interrupt_token.store(0, std::memory_order_release);
	if (g_exception_delivery_active) return false;
	const uint32_t signum = thread->pending_exception_signal.load(std::memory_order_acquire);
	if (signum == 0) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (Loader::GuestExecutionStopRequested()) return false;
#endif
	const uint64_t handler = KernelExceptionHandlerForSignal(static_cast<int32_t>(signum));
	if (handler == 0)
	{
		thread->pending_exception_signal.store(0, std::memory_order_release);
		return false;
	}
	thread->pending_exception_signal.store(0, std::memory_order_release);

	ExceptionDeliveryGuard guard(g_exception_delivery_active);
	return run_exception_handler(signum, handler, gpr, resume_rip, true);
#else
	(void)gpr;
	(void)resume_rip;
	return false;
#endif
}

int LUMEN_SYSV_ABI KernelUsleep(KernelUseconds microseconds)
{
	PRINT_NAME();
	printf("\tusleep: %u\n", microseconds);
	Core::Timer t;
	t.Start();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	const bool completed = runtime_sleep_nanos(static_cast<uint64_t>(microseconds) * 1000);
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	sleep_host_with_exception_delivery(static_cast<uint64_t>(microseconds) * 1000);
#else
	Core::Thread::SleepMicro(microseconds);
#endif
	double ts = t.GetTimeS();
	printf("\tactual: %g microseconds\n", ts * 1000000.0);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (!completed)
	{
		return KERNEL_ERROR_ECANCELED;
	}
#endif
	return OK;
}

#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID && LUMEN_PLATFORM != LUMEN_PLATFORM_IOS && \
    !(LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME))
static void sleep_seconds_on_host(unsigned int seconds)
{
	// Core::Thread::Sleep takes milliseconds and SDL_Delay narrows to Uint32.
	// Chunk to preserve the full guest seconds range without truncation.
	constexpr unsigned int max_chunk_seconds = UINT32_MAX / 1000;
	while (seconds > 0)
	{
		const unsigned int chunk = seconds > max_chunk_seconds ? max_chunk_seconds : seconds;
		Core::Thread::Sleep(chunk * 1000);
		seconds -= chunk;
	}
}
#endif

unsigned int LUMEN_SYSV_ABI KernelSleep(unsigned int seconds)
{
	PRINT_NAME();
	printf("\tsleep: %u\n", seconds);
	Core::Timer t;
	t.Start();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	const bool completed = runtime_sleep_nanos(static_cast<uint64_t>(seconds) * 1000000000);
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	sleep_host_with_exception_delivery(static_cast<uint64_t>(seconds) * 1000000000);
#else
	sleep_seconds_on_host(seconds);
#endif
	double ts = t.GetTimeS();
	printf("\tactual: %g seconds\n", ts);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (!completed)
	{
		return seconds;
	}
#endif
	return OK;
}

int LUMEN_SYSV_ABI KernelNanosleep(const KernelTimespec* rqtp, KernelTimespec* rmtp)
{
	PRINT_NAME();

	if (rqtp == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX) && defined(LUMEN_FEXCORE_RUNTIME)
	KernelTimespec requested {};
	if (!Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(rqtp), &requested, sizeof(requested)))
	{
		return KERNEL_ERROR_EFAULT;
	}
#else
	const KernelTimespec& requested = *rqtp;
#endif

	if (requested.tv_sec < 0 || requested.tv_nsec < 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	uint64_t nanos = requested.tv_sec * 1000000000 + requested.tv_nsec;

	printf("\tnanosleep: %" PRIu64 "\n", nanos);

	Core::Timer t;
	t.Start();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	const bool completed = runtime_sleep_nanos(nanos);
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	sleep_host_with_exception_delivery(nanos);
#else
	Core::Thread::SleepNano(nanos);
#endif
	double ts = t.GetTimeS();
	printf("\tactual: %g nanoseconds\n", ts * 1000000000.0);

	if (rmtp != nullptr)
	{
		KernelTimespec remaining {};
		sec_to_timespec(&remaining, ts);
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX) && defined(LUMEN_FEXCORE_RUNTIME)
		if (!Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(rmtp), sizeof(remaining), true) ||
		    !Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(rmtp), &remaining, sizeof(remaining)))
		{
			return KERNEL_ERROR_EFAULT;
		}
#else
		*rmtp = remaining;
#endif
	}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (!completed)
	{
		return KERNEL_ERROR_ECANCELED;
	}
#endif

	return OK;
}

int LUMEN_SYSV_ABI PthreadKeyCreate(PthreadKey* key, pthread_key_destructor_func_t destructor)
{
	PRINT_NAME();

	if (key == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
	PthreadArgumentLease key_lease;
	if (const int error = key_lease.Acquire(key, sizeof(*key), Core::VirtualMemory::Mode::Write); error != OK)
		return error;

	EXIT_IF(g_pthread_context == nullptr || g_pthread_context->GetPthreadKeys() == nullptr);

	PthreadKey output = 0;
	if (!g_pthread_context->GetPthreadKeys()->Create(&output, destructor))
	{
		return KERNEL_ERROR_EAGAIN;
	}
	*key = output;

	printf("\t destructor = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(destructor));
	printf("\t key        = %d\n", output);

	return OK;
}

int LUMEN_SYSV_ABI PthreadKeyDelete(PthreadKey key)
{
	PRINT_NAME();

	printf("\t key = %d\n", key);

	EXIT_IF(g_pthread_context == nullptr || g_pthread_context->GetPthreadKeys() == nullptr);

	if (!g_pthread_context->GetPthreadKeys()->Delete(key))
	{
		return KERNEL_ERROR_EINVAL;
	}

	return OK;
}

int LUMEN_SYSV_ABI PthreadOnce(const void* once_control, pthread_once_init_func_t init_routine)
{
	PRINT_NAME();

	if (once_control == nullptr || init_routine == nullptr) return KERNEL_ERROR_EINVAL;
	const uint64_t address = reinterpret_cast<uint64_t>(once_control);
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	// The guest control object is opaque to Lumen; its stable address is the
	// identity. Validate one readable byte without assuming a host pthread
	// layout or writing private state into guest memory.
	if (!Loader::ValidateGuestMemory(address, 1, false)) return KERNEL_ERROR_EFAULT;
#endif

	auto record = find_or_create_pthread_once_record(address);
	const int caller = Core::Thread::GetThreadIdUnique();
	for (;;)
	{
		std::unique_lock<std::mutex> lock(record->mutex);
		if (record->state == PthreadOnceState::Complete) return OK;
		if (record->state == PthreadOnceState::Ready)
		{
			record->state        = PthreadOnceState::Running;
			record->owner_thread = caller;
			break;
		}
		// The console behavior for recursive use is not publicly documented.
		// Refuse the local cycle instead of hanging the whole guest indefinitely.
		if (record->owner_thread == caller) return KERNEL_ERROR_EDEADLK;
		// Waiting while retaining a FEX host-call reader can block unrelated VM
		// lifecycle progress. The control address has already been copied and no
		// guest pointer is touched while the reader is ceded.
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#if defined(LUMEN_FEXCORE_RUNTIME)
		{
			Loader::ScopedGuestHostWait host_wait;
			(void)record->changed.wait_for(lock, std::chrono::milliseconds(10),
			                               [&record] { return record->state != PthreadOnceState::Running; });
			lock.unlock();
		}
		lock.lock();
#else
		(void)record->changed.wait_for(lock, std::chrono::milliseconds(10),
		                               [&record] { return record->state != PthreadOnceState::Running; });
#endif
		if (Loader::GuestExecutionStopRequested()) return KERNEL_ERROR_ECANCELED;
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
		{
			Loader::ScopedGuestHostWait host_wait;
			// Bounded slices keep a pending guest exception deliverable while
			// this thread is blocked on the one-shot control; the predicate makes
			// each expiry a re-check, so only the real transition completes it.
			(void)record->changed.wait_for(lock, std::chrono::milliseconds(10),
			                               [&record] { return record->state != PthreadOnceState::Running; });
			// wait_for() reacquires the once mutex. Drop it before host_wait
			// restores the guest reader so another HLE can drain through this
			// control.
			lock.unlock();
		}
		lock.lock();
#else
		record->changed.wait(lock, [&record] { return record->state != PthreadOnceState::Running; });
#endif
	}

	struct CompletionGuard
	{
		std::shared_ptr<PthreadOnceRecord> record;
		bool                               complete = false;
		~CompletionGuard()
		{
			{
				std::lock_guard<std::mutex> lock(record->mutex);
				record->state        = complete ? PthreadOnceState::Complete : PthreadOnceState::Ready;
				record->owner_thread = -1;
			}
			record->changed.notify_all();
		}
	} completion {record};

#if defined(__aarch64__)
	if (Loader::IsNativeHostCallTarget(reinterpret_cast<uint64_t>(init_routine)))
	{
		init_routine();
	} else
	{
		(void)Loader::RunGuestX64(reinterpret_cast<uint64_t>(init_routine));
	}
#else
	init_routine();
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (Loader::GuestExecutionStopRequested()) return KERNEL_ERROR_ECANCELED;
#endif
	completion.complete = true;
	return OK;
}

int LUMEN_SYSV_ABI PthreadSetspecific(PthreadKey key, void* value)
{
	PRINT_NAME();

	int thread_id = Core::Thread::GetThreadIdUnique();

	printf("\t key       = %d\n", key);
	printf("\t thread_id = %d\n", thread_id);
	printf("\t value     = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(value));

	EXIT_IF(g_pthread_context == nullptr || g_pthread_context->GetPthreadKeys() == nullptr);

	if (!g_pthread_context->GetPthreadKeys()->Set(key, value))
	{
		return KERNEL_ERROR_EINVAL;
	}

	return OK;
}

void* LUMEN_SYSV_ABI PthreadGetspecific(PthreadKey key)
{
	PRINT_NAME();

	int thread_id = Core::Thread::GetThreadIdUnique();

	printf("\t key       = %d\n", key);
	printf("\t thread_id = %d\n", thread_id);

	EXIT_IF(g_pthread_context == nullptr || g_pthread_context->GetPthreadKeys() == nullptr);

	void* value = nullptr;

	if (!g_pthread_context->GetPthreadKeys()->Get(key, &value))
	{
		return nullptr;
	}

	printf("\t value     = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(value));

	return value;
}

} // namespace LibKernel

namespace Posix {

LIB_NAME("Posix", "libkernel");

int LUMEN_SYSV_ABI pthread_attr_destroy(LibKernel::PthreadAttr* attr)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrDestroy(attr));
}

int LUMEN_SYSV_ABI pthread_attr_init(LibKernel::PthreadAttr* attr)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrInit(attr));
}

int LUMEN_SYSV_ABI pthread_attr_setstacksize(LibKernel::PthreadAttr* attr, size_t stack_size)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadAttrSetstacksize(attr, stack_size));
}

int LUMEN_SYSV_ABI pthread_cancel(LibKernel::Pthread thread)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadCancel(thread));
}

int LUMEN_SYSV_ABI pthread_detach(LibKernel::Pthread thread)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadDetach(thread));
}

int LUMEN_SYSV_ABI pthread_equal(LibKernel::Pthread left, LibKernel::Pthread right)
{
	return LibKernel::PthreadEqual(left, right);
}

void LUMEN_SYSV_ABI pthread_exit(void* value)
{
	LibKernel::PthreadExit(value);
}

int LUMEN_SYSV_ABI pthread_getschedparam(LibKernel::Pthread thread, uint32_t* policy, LibKernel::KernelSchedParam* param)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadGetschedparam(thread, policy, param));
}

LibKernel::Pthread LUMEN_SYSV_ABI pthread_self()
{
	return LibKernel::PthreadSelf();
}

int LUMEN_SYSV_ABI pthread_create(LibKernel::Pthread* thread, const LibKernel::PthreadAttr* attr, LibKernel::pthread_entry_func_t entry,
                                 void* arg)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCreate(thread, attr, entry, arg, ""));
}

int LUMEN_SYSV_ABI pthread_join(LibKernel::Pthread thread, void** value)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadJoin(thread, value));
}

int LUMEN_SYSV_ABI pthread_setschedparam(LibKernel::Pthread thread, uint32_t policy, const LibKernel::KernelSchedParam* param)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadSetschedparam(thread, policy, param));
}

int LUMEN_SYSV_ABI pthread_cond_broadcast(LibKernel::PthreadCond* cond)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondBroadcast(cond));
}

int LUMEN_SYSV_ABI pthread_cond_destroy(LibKernel::PthreadCond* cond)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondDestroy(cond));
}

int LUMEN_SYSV_ABI pthread_cond_init(LibKernel::PthreadCond* cond, const LibKernel::PthreadCondattr* attr)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondInit(cond, attr, nullptr));
}

int LUMEN_SYSV_ABI pthread_cond_signal(LibKernel::PthreadCond* cond)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondSignal(cond));
}

int LUMEN_SYSV_ABI pthread_cond_wait(LibKernel::PthreadCond* cond, LibKernel::PthreadMutex* mutex)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadCondWait(cond, mutex));
}

int LUMEN_SYSV_ABI pthread_mutex_lock(LibKernel::PthreadMutex* mutex)
{
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexLock(mutex));
}

int LUMEN_SYSV_ABI pthread_mutex_trylock(LibKernel::PthreadMutex* mutex)
{
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexTrylock(mutex));
}

int LUMEN_SYSV_ABI pthread_mutex_unlock(LibKernel::PthreadMutex* mutex)
{
	// PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexUnlock(mutex));
}

int LUMEN_SYSV_ABI pthread_rwlock_rdlock(LibKernel::PthreadRwlock* rwlock)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockRdlock(rwlock));
}

int LUMEN_SYSV_ABI pthread_rwlock_unlock(LibKernel::PthreadRwlock* rwlock)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockUnlock(rwlock));
}

int LUMEN_SYSV_ABI pthread_rwlock_wrlock(LibKernel::PthreadRwlock* rwlock)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockWrlock(rwlock));
}

int LUMEN_SYSV_ABI pthread_key_create(LibKernel::PthreadKey* key, LibKernel::pthread_key_destructor_func_t destructor)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadKeyCreate(key, destructor));
}

int LUMEN_SYSV_ABI pthread_key_delete(LibKernel::PthreadKey key)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadKeyDelete(key));
}

int LUMEN_SYSV_ABI pthread_setspecific(LibKernel::PthreadKey key, void* value)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadSetspecific(key, value));
}

void* LUMEN_SYSV_ABI pthread_getspecific(LibKernel::PthreadKey key)
{
	PRINT_NAME();

	return (LibKernel::PthreadGetspecific(key));
}

int LUMEN_SYSV_ABI pthread_mutex_destroy(LibKernel::PthreadMutex* mutex)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexDestroy(mutex));
}

int LUMEN_SYSV_ABI pthread_mutex_init(LibKernel::PthreadMutex* mutex, const LibKernel::PthreadMutexattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexInit(mutex, attr, nullptr));
}

int LUMEN_SYSV_ABI pthread_mutexattr_init(LibKernel::PthreadMutexattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexattrInit(attr));
}

int LUMEN_SYSV_ABI pthread_mutexattr_settype(LibKernel::PthreadMutexattr* attr, int type)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexattrSettype(attr, type));
}

int LUMEN_SYSV_ABI pthread_mutexattr_setprotocol(LibKernel::PthreadMutexattr* attr, int protocol)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexattrSetprotocol(attr, protocol));
}

int LUMEN_SYSV_ABI pthread_mutexattr_destroy(LibKernel::PthreadMutexattr* attr)
{
	PRINT_NAME();

	return POSIX_PTHREAD_CALL(LibKernel::PthreadMutexattrDestroy(attr));
}

int LUMEN_SYSV_ABI pthread_rwlock_destroy(LibKernel::PthreadRwlock* rwlock)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockDestroy(rwlock));
}

int LUMEN_SYSV_ABI pthread_rwlock_init(LibKernel::PthreadRwlock* rwlock, const LibKernel::PthreadRwlockattr* attr)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockInit(rwlock, attr, nullptr));
}

int LUMEN_SYSV_ABI pthread_rwlock_tryrdlock(LibKernel::PthreadRwlock* rwlock)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockTryrdlock(rwlock));
}

int LUMEN_SYSV_ABI pthread_rwlock_trywrlock(LibKernel::PthreadRwlock* rwlock)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockTrywrlock(rwlock));
}

int LUMEN_SYSV_ABI pthread_rwlockattr_destroy(LibKernel::PthreadRwlockattr* attr)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockattrDestroy(attr));
}

int LUMEN_SYSV_ABI pthread_rwlockattr_init(LibKernel::PthreadRwlockattr* attr)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(LibKernel::PthreadRwlockattrInit(attr));
}

} // namespace Posix

} // namespace Lumen::Libs

#if LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX
#pragma GCC diagnostic pop
#endif

#endif // LUMEN_EMU_ENABLED
