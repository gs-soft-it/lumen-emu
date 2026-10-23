#include "Lumen/Core/VirtualMemory.h"
#include "Lumen/Core/MemoryMetadataScope.h"
#include "../../../cpu/runtime/MemoryHandoff.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemoryImmutableSnapshot.h"

#if defined(LUMEN_FEXCORE_RUNTIME)
#include "AndroidFexCoreRuntime.h"
#endif

#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS) && !defined(LUMEN_APPLE_VM_STANDALONE)
#include "Lumen/Sys/SysVirtual.h"
#endif

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mach/mach.h>
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
#include <mach/mach_vm.h>
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && LUMEN_ABI == LUMEN_ABI_X86_64
#include <pthread.h>
#endif

#if LUMEN_ABI == LUMEN_ABI_ARM64_V8A
#include <libkern/OSCacheControl.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <vector>

namespace Lumen::Core {

#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS) && !defined(LUMEN_APPLE_VM_STANDALONE)
SystemInfo GetSystemInfo()
{
	SystemInfo ret {};
	sys_get_system_info(&ret);
	return ret;
}
#endif

namespace VirtualMemory {

namespace {
std::atomic<GuestBackingOwnerQuery> g_backing_owner_query {};
std::atomic<GuestBackingOwnerMark> g_backing_owner_mark {};
std::atomic<GuestBackingOwnerMarkOwned> g_backing_owner_mark_owned {};
std::atomic<GuestBackingOwnerPresence> g_backing_owner_presence {};
std::atomic<GuestBackingOwnerGpuCopy> g_backing_owner_gpu_copy {};
}

// Optional game-loop diagnostic hook used by the emulator's POSIX stubs.
volatile uint64_t g_usleep_state_addr = 0;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
void TraceVMMutationV1(const char* operation, uint64_t requested_address,
                      uint64_t size, Mode mode, uint64_t result_address,
                      uint32_t result_mode, uint64_t caller,
                      const char* phase = "result", uint64_t identity = 0,
                      uint64_t parent_identity = 0,
                      uint64_t generation_gauge = UINT64_MAX) noexcept;
#endif

namespace {

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
// Attribution is host-owned TLS; tracing never reads guest memory.
thread_local uint64_t g_capture_thread_handle = 0;
thread_local uint64_t g_capture_host_call = 0;

class VMMutationTraceScope final
{
public:
	VMMutationTraceScope(const char* operation, uint64_t address, uint64_t size,
	                     Mode mode, uint64_t caller) noexcept
	    : m_operation(operation), m_address(address), m_size(size), m_mode(mode), m_caller(caller)
	{
		TraceVMMutationV1(m_operation, m_address, m_size, m_mode, 0, 0,
		                   m_caller, "attempt");
	}
	~VMMutationTraceScope()
	{
		TraceVMMutationV1(m_operation, m_address, m_size, m_mode, m_result, m_result_mode,
		                   m_caller, m_phase, m_identity, m_parent_identity, m_generation);
	}
	void Observe(uint64_t identity, uint64_t parent_identity, uint64_t size,
	             uint64_t generation) noexcept
	{
		m_identity = identity;
		m_parent_identity = parent_identity;
		m_size = size;
		m_generation = generation;
	}
	void Complete(const char* phase, uint64_t result, uint32_t result_mode,
	              uint64_t generation) noexcept
	{
		m_phase = phase;
		m_result = result;
		m_result_mode = result_mode;
		m_generation = generation;
	}
private:
	const char* m_operation;
	uint64_t m_address;
	uint64_t m_size;
	Mode m_mode;
	uint64_t m_caller;
	const char* m_phase = "failure";
	uint64_t m_result = 0;
	uint32_t m_result_mode = 0;
	uint64_t m_identity = 0;
	uint64_t m_parent_identity = 0;
	uint64_t m_generation = UINT64_MAX;
};
#endif

struct Mapping
{
	uint64_t raw_base      = 0;
	uint64_t raw_size      = 0;
	uint64_t visible_base  = 0;
	uint64_t visible_size  = 0;
	Mode*    page_modes    = nullptr;
	uint64_t page_mode_num = 0;
	uint64_t identity      = 0;
	uint64_t continuity_generation = 0;
	uint64_t reservation_identity = 0;
	uint64_t validated_code_host_pages[8] {};
	uint8_t  validated_code_host_page_next = 0;
};

struct Reservation
{
	uint64_t base     = 0;
	uint64_t size     = 0;
	uint64_t identity = 0;
};

struct MappingRegistry
{
	std::mutex              mutation_mutex {};
	std::condition_variable mutation_ready {};
	uint64_t                code_readers       = 0;
	uint64_t                waiting_mutations  = 0;
	bool                    mutation_active    = false;
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
	uint64_t                waiting_guest_data_readers = 0;
#endif
	std::mutex              mutex {};
	Mapping*                mappings           = nullptr;
	size_t                  mapping_num        = 0;
	size_t                  mapping_capacity   = 0;
	Reservation*            reservations       = nullptr;
	size_t                  reservation_num    = 0;
	size_t                  reservation_capacity = 0;
	uint64_t                next_identity      = 1;
	uint64_t                next_reservation_identity = 1;
	std::atomic<uint64_t> mutation_generation {0};
	uint64_t uncertain_continuity_generation = 0;
	ImmutableSnapshot::Publisher immutable_snapshot {};

	~MappingRegistry()
	{
		for (size_t index = 0; index < mapping_num; index++)
		{
			std::free(mappings[index].page_modes);
		}
		std::free(mappings);
		std::free(reservations);
	}
};

MappingRegistry& Registry()
{
	static MappingRegistry registry;
	return registry;
}

bool NeedsPermissionSnapshot()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	// FEX's signal permission proof is a correctness requirement, independent
	// of the user-selectable snapshot acceleration for ordinary VM queries.
	return true;
#else
	return ImmutableSnapshot::Requested();
#endif
}

class ImmutableSnapshotMutation final
{
public:
	explicit ImmutableSnapshotMutation(MappingRegistry& registry)
	    : m_publisher(NeedsPermissionSnapshot()
	                      ? &registry.immutable_snapshot
	                      : nullptr)
	{
		if (m_publisher != nullptr) m_publisher->BeginMutation();
	}

	~ImmutableSnapshotMutation()
	{
		if (m_publisher != nullptr) m_publisher->EndMutation();
	}

	ImmutableSnapshotMutation(const ImmutableSnapshotMutation&) = delete;
	ImmutableSnapshotMutation& operator=(const ImmutableSnapshotMutation&) = delete;

private:
	ImmutableSnapshot::Publisher* m_publisher = nullptr;
};

thread_local uint64_t g_vm_readers = 0;
thread_local MappingRegistry* g_vm_mutation_owner = nullptr;
// A verified external FEX producer retains a shared local mapping pin.
// It permits native completions while FEX excludes guest execution/mutators;
// ordinary VM transactions retain their exclusive gate and denial policy.
thread_local uint64_t g_vm_external_mutation_token = 0;
thread_local uint64_t g_vm_external_native_token = 0;
thread_local uint64_t g_vm_metadata_scope = 0;

bool OwnsExternalMutationOwner(const MappingRegistry& registry) noexcept
{
#if defined(LUMEN_FEXCORE_RUNTIME)
	return g_vm_mutation_owner == &registry && g_vm_external_mutation_token != 0 &&
	    LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(g_vm_external_mutation_token) == 1;
#else
	(void)registry;
	return false;
#endif
}

bool OwnsExternalNativeOwner() noexcept
{
#if defined(LUMEN_FEXCORE_RUNTIME)
	return g_vm_external_native_token != 0 &&
	    LumenAndroidFexCoreOwnsNativeGuestDataLeaseV1(g_vm_external_native_token) == 1;
#else
	return false;
#endif
}
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
thread_local void (*g_guest_data_physical_validation_hook)() = nullptr;
thread_local void (*g_memory_mutation_local_release_hook)() = nullptr;
#endif

void NotifyMemoryMutationLocalReleaseForTest() noexcept
{
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
	if (g_memory_mutation_local_release_hook != nullptr)
		g_memory_mutation_local_release_hook();
#endif
}

struct CodeReadLeaseDenialDiagnostic
{
	uint64_t address = 0;
	uint64_t size = 0;
	const char* reason = nullptr;
	bool gate_observed = false;
	bool mutation_active = false;
	uint64_t waiting_mutations = 0;
	uint64_t code_readers = 0;
	uint64_t physical_begin = 0;
	uint64_t physical_end = 0;

	~CodeReadLeaseDenialDiagnostic()
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		if (reason == nullptr) return;
		static const bool enabled = []
		{
			const char* value = std::getenv("LUMEN_MACOS_FEXCORE_FAULT_CONTEXT");
			return value != nullptr && std::strcmp(value, "1") == 0;
		}();
		if (!enabled) return;
		static std::atomic<uint32_t> emitted {0};
		auto count = emitted.load(std::memory_order_relaxed);
		do
		{
			if (count >= 16) return;
		} while (!emitted.compare_exchange_weak(count, count + 1, std::memory_order_relaxed));
		// This object outlives the registry lock in TryAcquireCodeReadLease.
		// The gate fields are a snapshot of the denial, not a later re-query.
		std::fprintf(stderr,
		             "[MACOS_CODE_READ_LEASE_DENIED_V1] address=0x%llx size=0x%llx"
		             " reason=%s gateObserved=%u mutationActive=%u"
		             " waitingMutations=%llu codeReaders=%llu"
		             " physicalBegin=0x%llx physicalEnd=0x%llx\n",
		             static_cast<unsigned long long>(address),
		             static_cast<unsigned long long>(size), reason,
		             gate_observed ? 1u : 0u, mutation_active ? 1u : 0u,
		             static_cast<unsigned long long>(waiting_mutations),
		             static_cast<unsigned long long>(code_readers),
		             static_cast<unsigned long long>(physical_begin),
		             static_cast<unsigned long long>(physical_end));
#endif
	}

	bool Deny(const char* value)
	{
		reason = value;
		return false;
	}
};

bool GuestDataLeaseDenialTraceEnabled()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_MACOS_GUEST_DATA_LEASE_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
#else
	return false;
#endif
}

struct GuestDataLeaseDenialDiagnostic : GuestDataLeaseDenialMetadata
{
	GuestDataLeaseDenialDiagnostic(uint64_t begin, uint64_t size, Mode mode,
	                               GuestDataLeaseDenialMetadata* output_metadata, bool trace_enabled)
	    : output(output_metadata), trace(trace_enabled)
	{
		address = begin; bytes = size; required = mode;
		acquisition_thread = reinterpret_cast<uintptr_t>(&g_vm_readers);
	}
	GuestDataLeaseDenialMetadata* output = nullptr;
	bool trace = false;

	void ObserveGate(const MappingRegistry& registry)
	{
		// Caller holds mutation_mutex. An unavailable try_lock never reaches
		// this observation: its non-atomic gate fields remain unknown.
		gate_observed = true;
		mutation_active = registry.mutation_active;
		waiting_mutations = registry.waiting_mutations;
		code_readers = registry.code_readers;
		generation = registry.mutation_generation.load(std::memory_order_relaxed);
	}

	bool Deny(GuestDataLeaseDenialReason cause)
	{
		reason = cause;
		// Only a fresh caller with no retained local admission can retry a
		// transient gate observation. Range/rights/handoff/context failures
		// remain terminal and grant no pointer or read authority.
		retryable = g_vm_readers == 0 && g_vm_mutation_owner == nullptr && g_vm_external_native_token == 0 &&
		    (cause == GuestDataLeaseDenialReason::GateTrylockUnavailable ||
		     cause == GuestDataLeaseDenialReason::GateMutationActive ||
		     cause == GuestDataLeaseDenialReason::GateWriterPending);
		return false;
	}

	~GuestDataLeaseDenialDiagnostic()
	{
		// One caller-owned output belongs only to this acquisition. Copy after
		// every local gate/registry lock ends, independently of trace budgets.
		if (output != nullptr && reason != GuestDataLeaseDenialReason::None) *output = *this;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		// All local gate/registry locks and this acquisition's reader have
		// ended before destruction. A caller's retained lease or mutation
		// must not acquire stdio's lock either: suppress that observation.
		if (!trace || reason == GuestDataLeaseDenialReason::None || g_vm_readers != 0 || g_vm_mutation_owner != nullptr) return;
		static std::atomic<uint32_t> emitted {0};
		auto count = emitted.load(std::memory_order_relaxed);
		while (count < 64 && !emitted.compare_exchange_strong(count, count + 1, std::memory_order_relaxed)) {}
		if (count >= 64) return;
		std::fprintf(stderr,
		             "[MACOS_GUEST_DATA_LEASE_DENIED_V1] index=%u phase=%s reason=%s"
		             " attempt_scope=single-vm-acquisition required_mode=%u generation_gauge=%llu"
		             " gate_observed=%u mutation_active=%u waiting_mutations=%llu code_readers=%llu"
		             " physical_observed=%u physical_result=%d physical_protection=%u\n",
		             count, GuestDataLeaseDenialPhaseName(reason), GuestDataLeaseDenialReasonName(reason), static_cast<uint32_t>(required),
		             static_cast<unsigned long long>(generation), gate_observed ? 1u : 0u,
		             mutation_active ? 1u : 0u, static_cast<unsigned long long>(waiting_mutations),
		             static_cast<unsigned long long>(code_readers), physical_observed ? 1u : 0u,
		             physical_result, static_cast<uint32_t>(physical_protection));
#endif
	}
};

bool BeginCodeReadGate(MappingRegistry& registry,
                      CodeReadLeaseDenialDiagnostic* diagnostic)
{
	std::lock_guard<std::mutex> lock(registry.mutation_mutex);
	if (registry.mutation_active || registry.waiting_mutations != 0 ||
	    registry.code_readers == (std::numeric_limits<uint64_t>::max)())
	{
		if (diagnostic != nullptr)
		{
			diagnostic->gate_observed = true;
			diagnostic->mutation_active = registry.mutation_active;
			diagnostic->waiting_mutations = registry.waiting_mutations;
			diagnostic->code_readers = registry.code_readers;
		}
		return false;
	}
	registry.code_readers++;
	++g_vm_readers;
	return true;
}

void EndCodeReadGate(MappingRegistry& registry)
{
	if (g_vm_readers == 0) std::abort();
	--g_vm_readers;
	bool notify = false;
	{
		std::lock_guard<std::mutex> lock(registry.mutation_mutex);
		if (registry.code_readers == 0) return;
		registry.code_readers--;
		notify = registry.code_readers == 0;
	}
	if (notify) registry.mutation_ready.notify_all();
}

bool BeginGuestDataGate(MappingRegistry& registry, bool wait_for_mutation,
                       GuestDataLeaseDenialDiagnostic* diagnostic)
{
	std::unique_lock<std::mutex> lock(registry.mutation_mutex, std::defer_lock);
	if (wait_for_mutation) lock.lock();
	else if (!lock.try_lock())
	{
		if (diagnostic != nullptr) diagnostic->Deny(GuestDataLeaseDenialReason::GateTrylockUnavailable);
		return false;
	}
	const bool owns_external_mutation = OwnsExternalMutationOwner(registry);
	const bool owns_external_native = OwnsExternalNativeOwner();
	if ((g_vm_external_mutation_token != 0 && !owns_external_mutation) ||
	    (g_vm_external_native_token != 0 && !owns_external_native))
	{
		if (diagnostic != nullptr) diagnostic->Deny(GuestDataLeaseDenialReason::GateMutationActive);
		return false;
	}
	// Beneath subsystem locks, only the exact external producer owner may
	// borrow its own closed gate. Other nested readers and ordinary VM
	// transaction owners keep the existing nonblocking denial policy.
	if (!wait_for_mutation && !owns_external_mutation && !owns_external_native &&
	    (registry.mutation_active || registry.waiting_mutations != 0))
	{
		if (diagnostic != nullptr)
		{
			diagnostic->ObserveGate(registry);
			diagnostic->Deny(registry.mutation_active ? GuestDataLeaseDenialReason::GateMutationActive
			    : GuestDataLeaseDenialReason::GateWriterPending);
		}
		return false;
	}
	// A transaction may publish a checked HLE output before Commit. Its owner
	// already excludes other VM mutations and must not wait on its own gate.
	// Keep the ordinary reader count and all logical/physical access checks.
	const bool owns_mutation = g_vm_mutation_owner == &registry;
	if (g_vm_readers == 0 && !owns_mutation && !owns_external_native)
	{
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
		++registry.waiting_guest_data_readers;
#endif
		registry.mutation_ready.wait(lock, [&registry]
		{
			return !registry.mutation_active && registry.waiting_mutations == 0;
		});
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
		--registry.waiting_guest_data_readers;
#endif
	}
	if (diagnostic != nullptr) diagnostic->ObserveGate(registry);
	if ((registry.mutation_active && !owns_mutation) ||
	    registry.code_readers == (std::numeric_limits<uint64_t>::max)())
	{
		if (diagnostic != nullptr)
			diagnostic->Deny(registry.mutation_active && !owns_mutation
			    ? GuestDataLeaseDenialReason::GateMutationActive : GuestDataLeaseDenialReason::GateReaderOverflow);
		return false;
	}
	++registry.code_readers;
	++g_vm_readers;
	return true;
}

void EndGuestDataGate(MappingRegistry& registry)
{
	EndCodeReadGate(registry);
}

class MutationGate final
{
public:
	explicit MutationGate(MappingRegistry& registry): m_registry(&registry)
	{
		std::unique_lock<std::mutex> lock(registry.mutation_mutex);
		registry.waiting_mutations++;
		registry.mutation_ready.wait(lock, [&registry]
		{
			return !registry.mutation_active && registry.code_readers == 0;
		});
		registry.waiting_mutations--;
		registry.mutation_active = true;
		g_vm_mutation_owner = &registry;
	}

	~MutationGate()
	{
		if (m_registry == nullptr) return;
		RequireNoGuestDataBorrows();
		{
			std::lock_guard<std::mutex> lock(m_registry->mutation_mutex);
			g_vm_mutation_owner = nullptr;
			m_registry->mutation_active = false;
		}
		m_registry->mutation_ready.notify_all();
	}

	MutationGate(const MutationGate&) = delete;
	MutationGate& operator=(const MutationGate&) = delete;

	void RequireNoGuestDataBorrows() const
	{
		// Closing (especially Abort) can invalidate transaction-owned mappings.
		// Owner borrows therefore end before either terminal operation begins.
		if (g_vm_mutation_owner != m_registry || g_vm_readers != 0) std::abort();
	}

#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
	bool IsOwnedForTest() const
	{
		if (m_registry == nullptr) return false;
		std::lock_guard<std::mutex> lock(m_registry->mutation_mutex);
		return m_registry->mutation_active;
	}
#endif

private:
	MappingRegistry* m_registry = nullptr;
};

enum class FexGuestMemoryMutationKind : uint32_t
{
	Exclusive       = 0,
	AdditiveMapping = 1,
};

using LocalMutationRelease = void (*)(void*) noexcept;

#if defined(LUMEN_FEXCORE_RUNTIME)
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
std::uint32_t QueryFexDataAccess(std::uint64_t address, std::uint64_t size,
	                              std::uint32_t write) noexcept;
#endif
static_assert(static_cast<uint32_t>(FexGuestMemoryMutationKind::Exclusive) ==
              LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
static_assert(static_cast<uint32_t>(FexGuestMemoryMutationKind::AdditiveMapping) ==
              LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_ADDITIVE_MAPPING_V2);

class FexGuestMemoryMutation final
{
public:
	explicit FexGuestMemoryMutation(FexGuestMemoryMutationKind kind)
	{
		// An adopted producer pins existing mappings; it cannot turn that pin
		// into a nested Core mutation and wait forever on its own reader.
		if (g_vm_external_mutation_token != 0 || g_vm_external_native_token != 0) return;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		if (LumenMacOSFexCoreRegisterDataAccessQueryV1(QueryFexDataAccess) == 0) return;
#endif
		uint32_t may_retry = 0;
		m_token = LumenAndroidFexCoreBeginGuestMemoryMutationV3(static_cast<uint32_t>(kind), &may_retry);
		// The provider classifies only effect-free conflicts in a top-level
		// HLE reader. Local Core borrows/owners must also be absent before a
		// caller may release that reader and make a fresh attempt.
		m_retryable = m_token == 0 && may_retry == 1 && g_vm_readers == 0 &&
		    g_vm_mutation_owner == nullptr && g_vm_external_mutation_token == 0 && g_vm_external_native_token == 0;
	}

	~FexGuestMemoryMutation()
	{
		Close();
	}

	FexGuestMemoryMutation(const FexGuestMemoryMutation&) = delete;
	FexGuestMemoryMutation& operator=(const FexGuestMemoryMutation&) = delete;

	bool IsAdmitted() const { return m_token != 0; }
	bool IsRetryable() const { return m_retryable; }

	void Commit(uint64_t range_base, uint64_t range_size)
	{
		if (m_token == 0) return;
		LumenAndroidFexCoreNotifyFreshGuestCallbackMappingChangeV1(range_base, range_size);
		m_range_base = range_base;
		m_range_size = range_size;
		m_committed  = true;
	}

	void Close(LocalMutationRelease release_local = nullptr, void* opaque = nullptr)
	{
		if (m_token == 0) return;
		if (LumenAndroidFexCoreEndGuestMemoryMutationV2(
		        m_token, m_range_base, m_range_size, m_committed ? 1 : 0,
		        release_local, opaque) == 0) std::abort();
		m_token = 0;
	}

private:
	uint64_t m_token      = 0;
	uint64_t m_range_base = 0;
	uint64_t m_range_size = 0;
	bool     m_committed  = false;
	bool     m_retryable  = false;
};
#else
class FexGuestMemoryMutation final
{
public:
	explicit FexGuestMemoryMutation(FexGuestMemoryMutationKind) {}
	bool IsAdmitted() const { return true; }
	bool IsRetryable() const { return false; }
	void Commit(uint64_t, uint64_t) {}
	void Close(LocalMutationRelease release_local = nullptr, void* opaque = nullptr)
	{
		if (release_local != nullptr) release_local(opaque);
	}
};
#endif

class GatedFexGuestMemoryMutation final
{
public:
	explicit GatedFexGuestMemoryMutation(
	    MappingRegistry& registry, FexGuestMemoryMutationKind kind)
	    : m_fex(kind)
	{
		if (m_fex.IsAdmitted()) m_gate.emplace(registry);
	}

	~GatedFexGuestMemoryMutation()
	{
		// Keep the local gate through code invalidation, but release it before
		// FEX readmits guest threads. Otherwise a resumed code query can mistake
		// the still-closed local gate for an executable-permission failure.
		if (m_gate.has_value()) m_fex.Close(&ReleaseLocalGate, this);
		else m_fex.Close();
	}

	GatedFexGuestMemoryMutation(const GatedFexGuestMemoryMutation&) = delete;
	GatedFexGuestMemoryMutation& operator=(
	    const GatedFexGuestMemoryMutation&) = delete;

	bool IsAdmitted() const
	{
		return m_fex.IsAdmitted() && m_gate.has_value();
	}
	bool IsRetryable() const
	{
		return !m_fex.IsAdmitted() && !m_gate.has_value() && m_fex.IsRetryable();
	}

	void Commit(uint64_t range_base, uint64_t range_size)
	{
		m_fex.Commit(range_base, range_size);
	}

private:
	static void ReleaseLocalGate(void* opaque) noexcept
	{
		auto* owner = static_cast<GatedFexGuestMemoryMutation*>(opaque);
		owner->m_gate.reset();
		NotifyMemoryMutationLocalReleaseForTest();
	}

	FexGuestMemoryMutation      m_fex;
	std::optional<MutationGate> m_gate;
};

enum class TransactionKind : uint32_t
{
	Reservation,
	ReservedMapping,
};

#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
std::atomic<uint64_t> g_virtual_range_transaction_begins {0};
std::atomic<uint64_t> g_virtual_range_transaction_closes {0};
std::atomic<uint64_t> g_virtual_range_transaction_gate_closes {0};
std::atomic<uint64_t> g_virtual_range_transaction_active {0};
#endif

struct MutationTransactionState
{
	explicit MutationTransactionState(MappingRegistry& registry)
	    : fex(FexGuestMemoryMutationKind::AdditiveMapping)
	{
		if (fex.IsAdmitted())
		{
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
			g_virtual_range_transaction_begins.fetch_add(
			    1, std::memory_order_relaxed);
			g_virtual_range_transaction_active.fetch_add(
			    1, std::memory_order_relaxed);
			counted_for_test = true;
#endif
			gate = new (std::nothrow) MutationGate(registry);
		}
	}
	~MutationTransactionState()
	{
		// Publish while the local gate is owned, then release it inside the
		// publication callback, before FEX can readmit guest execution.
		bool gate_owned = false;
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
		gate_owned = gate != nullptr && gate->IsOwnedForTest();
#endif
		if (gate != nullptr) fex.Close(&ReleaseLocalGate, this);
		else fex.Close();
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
		if (counted_for_test)
		{
			g_virtual_range_transaction_closes.fetch_add(
			    1, std::memory_order_relaxed);
			g_virtual_range_transaction_active.fetch_sub(
			    1, std::memory_order_relaxed);
			if (gate_owned)
				g_virtual_range_transaction_gate_closes.fetch_add(
				    1, std::memory_order_relaxed);
		}
#else
		(void)gate_owned;
#endif
	}

	static void ReleaseLocalGate(void* opaque) noexcept
	{
		auto* owner = static_cast<MutationTransactionState*>(opaque);
		delete owner->gate;
		owner->gate = nullptr;
		NotifyMemoryMutationLocalReleaseForTest();
	}

	bool IsAdmitted() const { return fex.IsAdmitted() && gate != nullptr; }

	FexGuestMemoryMutation fex;
	MutationGate*          gate = nullptr;
	TransactionKind        kind = TransactionKind::Reservation;
	ReservationHandle      reservation {};
	uint64_t               mapping_identity = 0;
	uint64_t               range_base = 0;
	uint64_t               range_size = 0;
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
	bool                   counted_for_test = false;
#endif
};

constexpr uint64_t GUEST_PAGE_SIZE            = 4096;
constexpr uint32_t MANAGED_ADDRESS_CACHE_SIZE = 32;

enum class VirtualRangeTestFault : uint32_t
{
	None,
	CoreCapacity,
	SnapshotPublish,
	FexAdditiveAdmission,
	FexExclusiveAdmission,
	ActivateHost,
	DecommitHost,
	ReleaseHost,
	AbortHost,
	ProtectAfterHost,
};

#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
std::atomic<uint32_t> g_virtual_range_test_fault {
	static_cast<uint32_t>(VirtualRangeTestFault::None)};

bool ConsumeVirtualRangeTestFault(VirtualRangeTestFault expected)
{
	uint32_t value = static_cast<uint32_t>(expected);
	return g_virtual_range_test_fault.compare_exchange_strong(
	    value, static_cast<uint32_t>(VirtualRangeTestFault::None),
	    std::memory_order_acq_rel, std::memory_order_acquire);
}
#else
bool ConsumeVirtualRangeTestFault(VirtualRangeTestFault)
{
	return false;
}
#endif

struct ManagedAddressRange
{
	uint64_t begin = 0;
	uint64_t end   = 0;
};

struct ManagedAddressCache
{
	ManagedAddressRange ranges[MANAGED_ADDRESS_CACHE_SIZE] {};
	uint32_t            next       = 0;
	uint32_t            mru        = 0;
	uint64_t            generation = 0;
};

thread_local ManagedAddressCache g_managed_address_cache {};

void SynchronizeManagedAddressCache(ManagedAddressCache& cache, uint64_t generation)
{
	if (cache.generation == generation) return;
	cache            = {};
	cache.generation = generation;
}

uint64_t HostPageSize()
{
	static const uint64_t page_size = []
	{
		const long value = sysconf(_SC_PAGESIZE);
		return value > 0 ? static_cast<uint64_t>(value) : uint64_t {4096};
	}();
	return page_size;
}

bool IsPowerOfTwo(uint64_t value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

bool AddWithoutOverflow(uint64_t left, uint64_t right, uint64_t* result)
{
	if (result == nullptr || left > (std::numeric_limits<uint64_t>::max)() - right)
	{
		return false;
	}
	*result = left + right;
	return true;
}

bool RoundUp(uint64_t value, uint64_t alignment, uint64_t* result)
{
	uint64_t sum = 0;
	if (!IsPowerOfTwo(alignment) || !AddWithoutOverflow(value, alignment - 1, &sum))
	{
		return false;
	}
	*result = sum & ~(alignment - 1);
	return true;
}

int HostProtection(Mode mode)
{
	const auto bits   = static_cast<uint32_t>(mode);
	int        result = PROT_NONE;

	if ((bits & static_cast<uint32_t>(Mode::Read)) != 0)
	{
		result |= PROT_READ;
	}
	if ((bits & static_cast<uint32_t>(Mode::Write)) != 0)
	{
		// Darwin does not provide a useful write-only mapping for this runtime.
		result |= PROT_READ | PROT_WRITE;
	}

#if LUMEN_ABI == LUMEN_ABI_ARM64_V8A
	// The ARM64 backend interprets guest x86-64 bytes. Logical execute access
	// therefore needs host read access, never an executable host page.
	if ((bits & static_cast<uint32_t>(Mode::Execute)) != 0)
	{
		result |= PROT_READ;
	}
#else
	if ((bits & static_cast<uint32_t>(Mode::Execute)) != 0)
	{
		result |= PROT_EXEC;
	}
#endif

	return result;
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
std::uint32_t QueryFexDataAccess(std::uint64_t address, std::uint64_t size,
	                              std::uint32_t write) noexcept
{
	static_assert(std::atomic<uint64_t>::is_always_lock_free);
	static_assert(std::atomic<void*>::is_always_lock_free);
	static_assert(std::atomic<bool>::is_always_lock_free);
	std::uint64_t end = 0;
	if (address == 0 || size == 0 || write > 1 || !AddWithoutOverflow(address, size, &end)) return 0;
	// This entry is invoked only from a translated guest while the FEX execution
	// barrier pins existing mappings. No lock or snapshot diagnostic is allowed
	// in a signal handler; missing/incomplete publication denies emulation.
	auto& publisher = Registry().immutable_snapshot;
	const int required = PROT_READ | (write != 0 ? PROT_WRITE : 0);
	while (address < end)
	{
		ManagedRegionState state = ManagedRegionState::Unmanaged;
		ManagedRegion region {};
		if (!publisher.Lookup(address, &state, &region, false, true) ||
		    state != ManagedRegionState::Visible || region.end <= address ||
		    (HostProtection(region.mode) & required) != required) return 0;
		address = region.end < end ? region.end : end;
	}
	return 1;
}
#endif

int MappingFlags(Mode mode)
{
	int flags = MAP_PRIVATE | MAP_ANON;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && LUMEN_ABI == LUMEN_ABI_X86_64 && defined(MAP_JIT)
	if (IsExecute(mode))
	{
		flags |= MAP_JIT;
	}
#else
	(void)mode;
#endif
	return flags;
}

void EnableIntelJitWrites(Mode mode)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && LUMEN_ABI == LUMEN_ABI_X86_64
	if ((static_cast<uint32_t>(mode) & static_cast<uint32_t>(Mode::Write)) != 0)
	{
		pthread_jit_write_protect_np(0);
	}
#else
	(void)mode;
#endif
}

bool ValidateMappingSize(uint64_t size)
{
	return size != 0 && size <= static_cast<uint64_t>((std::numeric_limits<size_t>::max)());
}

bool ValidateProtectionMode(Mode mode)
{
	// Do not silently discard unknown guest permission bits while publishing
	// their original value to the managed registry and immutable snapshot.
	return (static_cast<uint32_t>(mode) &
	        ~static_cast<uint32_t>(Mode::ExecuteReadWrite)) == 0;
}

void RecordMutation(MappingRegistry& registry)
{
	auto generation = registry.mutation_generation.load(std::memory_order_relaxed);
	while (generation != (std::numeric_limits<uint64_t>::max)() &&
	       !registry.mutation_generation.compare_exchange_weak(generation, generation + 1, std::memory_order_release,
	                                                           std::memory_order_relaxed))
	{
	}
}

// These watermarks are authoritative only under registry.mutex and the VM
// writer gate. A failed operation may have touched then restored host rights
// without advancing the public generation. current+1 revokes earlier captures
// conservatively until a later public generation permits a fresh capture; it
// never changes the public generation semantics or revives an old baseline.
uint64_t ContinuityWatermark(const MappingRegistry& registry, bool uncertain)
{
	const auto current = registry.mutation_generation.load(std::memory_order_relaxed);
	return uncertain && current != UINT64_MAX ? current + 1 : current;
}

void MarkMappingContinuity(MappingRegistry& registry, Mapping& mapping, bool uncertain = false)
{
	mapping.continuity_generation = std::max(mapping.continuity_generation,
	                                       ContinuityWatermark(registry, uncertain));
}

void MarkRangeContinuity(MappingRegistry& registry, uint64_t address, uint64_t size, bool uncertain = false)
{
	const auto watermark = ContinuityWatermark(registry, uncertain);
	uint64_t end = 0;
	if (!AddWithoutOverflow(address, size, &end))
	{
		registry.uncertain_continuity_generation = std::max(registry.uncertain_continuity_generation, watermark);
		return;
	}
	if (size == 0) return;
	for (size_t index = 0; index < registry.mapping_num; ++index)
	{
		auto& mapping = registry.mappings[index];
		uint64_t raw_end = 0;
		if (!AddWithoutOverflow(mapping.raw_base, mapping.raw_size, &raw_end))
		{
			registry.uncertain_continuity_generation = std::max(registry.uncertain_continuity_generation, watermark);
			continue;
		}
		if (mapping.raw_base < end && address < raw_end)
			mapping.continuity_generation = std::max(mapping.continuity_generation, watermark);
	}
}

bool EnsureRegistryCapacity(MappingRegistry& registry)
{
	if (registry.mapping_num < registry.mapping_capacity)
	{
		return true;
	}

	constexpr size_t initial_capacity = 16;
	const size_t     max_capacity     = (std::numeric_limits<size_t>::max)() / sizeof(Mapping);
	if (registry.mapping_capacity >= max_capacity)
	{
		return false;
	}

	size_t new_capacity = initial_capacity;
	if (registry.mapping_capacity != 0)
	{
		new_capacity = registry.mapping_capacity > max_capacity / 2 ? max_capacity : registry.mapping_capacity * 2;
	}
	if (new_capacity <= registry.mapping_capacity)
	{
		return false;
	}

	void* resized = std::realloc(registry.mappings, new_capacity * sizeof(Mapping));
	if (resized == nullptr)
	{
		return false;
	}
	registry.mappings         = static_cast<Mapping*>(resized);
	registry.mapping_capacity = new_capacity;
	return true;
}

bool EnsureReservationCapacity(MappingRegistry& registry)
{
	if (registry.reservation_num < registry.reservation_capacity)
	{
		return true;
	}

	constexpr size_t initial_capacity = 8;
	const size_t max_capacity =
	    (std::numeric_limits<size_t>::max)() / sizeof(Reservation);
	if (registry.reservation_capacity >= max_capacity) return false;

	size_t new_capacity = initial_capacity;
	if (registry.reservation_capacity != 0)
	{
		new_capacity = registry.reservation_capacity > max_capacity / 2
		                   ? max_capacity
		                   : registry.reservation_capacity * 2;
	}
	if (new_capacity <= registry.reservation_capacity) return false;

	void* resized = std::realloc(
	    registry.reservations, new_capacity * sizeof(Reservation));
	if (resized == nullptr) return false;
	registry.reservations = static_cast<Reservation*>(resized);
	registry.reservation_capacity = new_capacity;
	return true;
}

size_t LowerBoundMapping(const MappingRegistry& registry, uint64_t visible_base)
{
	size_t begin = 0;
	size_t end   = registry.mapping_num;
	while (begin < end)
	{
		const size_t middle = begin + (end - begin) / 2;
		if (registry.mappings[middle].visible_base < visible_base)
		{
			begin = middle + 1;
		} else
		{
			end = middle;
		}
	}
	return begin;
}

size_t UpperBoundMapping(const MappingRegistry& registry, uint64_t visible_base)
{
	size_t begin = 0;
	size_t end   = registry.mapping_num;
	while (begin < end)
	{
		const size_t middle = begin + (end - begin) / 2;
		if (registry.mappings[middle].visible_base <= visible_base)
		{
			begin = middle + 1;
		} else
		{
			end = middle;
		}
	}
	return begin;
}

size_t LowerBoundReservation(const MappingRegistry& registry, uint64_t base)
{
	size_t begin = 0;
	size_t end = registry.reservation_num;
	while (begin < end)
	{
		const size_t middle = begin + (end - begin) / 2;
		if (registry.reservations[middle].base < base)
		{
			begin = middle + 1;
		} else
		{
			end = middle;
		}
	}
	return begin;
}

bool RangesOverlap(uint64_t left_begin, uint64_t left_size,
	               uint64_t right_begin, uint64_t right_size)
{
	uint64_t left_end = 0;
	uint64_t right_end = 0;
	return AddWithoutOverflow(left_begin, left_size, &left_end) &&
	       AddWithoutOverflow(right_begin, right_size, &right_end) &&
	       left_begin < right_end && right_begin < left_end;
}

Reservation* FindContainingReservation(MappingRegistry& registry,
	                                    uint64_t address)
{
	const size_t insertion = LowerBoundReservation(registry, address);
	if (insertion < registry.reservation_num &&
	    registry.reservations[insertion].base == address)
	{
		return &registry.reservations[insertion];
	}
	if (insertion == 0) return nullptr;
	Reservation* reservation = &registry.reservations[insertion - 1];
	uint64_t end = 0;
	return AddWithoutOverflow(reservation->base, reservation->size, &end) &&
	               address >= reservation->base && address < end
	           ? reservation
	           : nullptr;
}

uint64_t MappingSnapshotOwner(const Mapping& mapping)
{
	return mapping.identity <= (std::numeric_limits<uint64_t>::max)() / 2
	           ? mapping.identity * 2
	           : 0;
}

uint64_t ReservationSnapshotOwner(const Reservation& reservation)
{
	return reservation.identity < (std::numeric_limits<uint64_t>::max)() / 2
	           ? reservation.identity * 2 + 1
	           : 0;
}

bool AppendMappingSnapshot(const Mapping& mapping,
	                       ImmutableSnapshot::Builder& builder)
{
	const uint64_t owner = MappingSnapshotOwner(mapping);
	uint64_t raw_end = 0;
	uint64_t visible_end = 0;
	if (mapping.raw_base == 0 || owner == 0 ||
	    !AddWithoutOverflow(mapping.raw_base, mapping.raw_size, &raw_end) ||
	    !AddWithoutOverflow(mapping.visible_base, mapping.visible_size,
	                        &visible_end) ||
	    mapping.visible_base < mapping.raw_base || visible_end > raw_end)
	{
		return false;
	}

	if (mapping.raw_base < mapping.visible_base &&
	    !builder.Append({mapping.raw_base, mapping.visible_base,
	                     owner, ManagedRegionState::Reserved,
	                     Mode::NoAccess}))
	{
		return false;
	}

	uint64_t page = 0;
	while (page < mapping.page_mode_num)
	{
		const uint64_t run_begin = mapping.visible_base + page * GUEST_PAGE_SIZE;
		if (run_begin >= visible_end) break;
		const Mode mode = mapping.page_modes[static_cast<size_t>(page)];
		uint64_t run_page_end = page + 1;
		while (run_page_end < mapping.page_mode_num &&
		       mapping.page_modes[static_cast<size_t>(run_page_end)] == mode)
		{
			run_page_end++;
		}
		const uint64_t raw_run_end =
		    mapping.visible_base + run_page_end * GUEST_PAGE_SIZE;
		const uint64_t run_end = raw_run_end < visible_end ? raw_run_end : visible_end;
		if (!builder.Append({run_begin, run_end, owner,
		                     ManagedRegionState::Visible, mode}))
		{
			return false;
		}
		page = run_page_end;
	}

	return visible_end >= raw_end ||
	       builder.Append({visible_end, raw_end, owner,
	                       ManagedRegionState::Reserved, Mode::NoAccess});
}

bool AppendReservationSnapshot(const MappingRegistry& registry,
	                           const Reservation& reservation,
	                           ImmutableSnapshot::Builder& builder)
{
	uint64_t reservation_end = 0;
	const uint64_t owner = ReservationSnapshotOwner(reservation);
	if (owner == 0 ||
	    !AddWithoutOverflow(reservation.base, reservation.size,
	                        &reservation_end))
	{
		return false;
	}

	uint64_t cursor = reservation.base;
	for (size_t index = 0; index < registry.mapping_num; index++)
	{
		const Mapping& mapping = registry.mappings[index];
		if (mapping.reservation_identity != reservation.identity) continue;
		uint64_t mapping_end = 0;
		if (!AddWithoutOverflow(mapping.visible_base, mapping.visible_size,
		                        &mapping_end) ||
		    mapping.visible_base < cursor || mapping_end > reservation_end)
		{
			return false;
		}
		if (cursor < mapping.visible_base &&
		    !builder.Append({cursor, mapping.visible_base, owner,
		                     ManagedRegionState::Reserved, Mode::NoAccess}))
		{
			return false;
		}
		if (!AppendMappingSnapshot(mapping, builder)) return false;
		cursor = mapping_end;
	}

	return cursor >= reservation_end ||
	       builder.Append({cursor, reservation_end, owner,
	                       ManagedRegionState::Reserved, Mode::NoAccess});
}

bool PublishFullSnapshot(MappingRegistry& registry)
{
	if (!NeedsPermissionSnapshot()) return true;
	ImmutableSnapshot::Builder builder;
	for (size_t index = 0; index < registry.mapping_num; index++)
	{
		if (registry.mappings[index].reservation_identity == 0 &&
		    !AppendMappingSnapshot(registry.mappings[index], builder))
		{
			registry.immutable_snapshot.Invalidate();
			return false;
		}
	}
	for (size_t index = 0; index < registry.reservation_num; index++)
	{
		if (!AppendReservationSnapshot(
		        registry, registry.reservations[index], builder))
		{
			registry.immutable_snapshot.Invalidate();
			return false;
		}
	}
	return registry.immutable_snapshot.Publish(builder);
}

bool PublishVirtualRangeSnapshot(MappingRegistry& registry)
{
	if (ConsumeVirtualRangeTestFault(
	        VirtualRangeTestFault::SnapshotPublish))
	{
		registry.immutable_snapshot.Invalidate();
		return false;
	}
	return PublishFullSnapshot(registry);
}

bool PublishRegisteredMapping(MappingRegistry& registry,
	                          const Mapping& mapping)
{
	if (!NeedsPermissionSnapshot()) return true;
	size_t current_count = 0;
	const auto* current =
	    registry.immutable_snapshot.CurrentEntriesForWriter(&current_count);
	if (current == nullptr) return PublishFullSnapshot(registry);

	if (current_count > (std::numeric_limits<size_t>::max)() - 3)
	{
		registry.immutable_snapshot.Invalidate();
		return false;
	}
	ImmutableSnapshot::Builder builder;
	if (!builder.Reserve(current_count + 3) ||
	    !builder.AppendAll(current, current_count) ||
	    !AppendMappingSnapshot(mapping, builder))
	{
		registry.immutable_snapshot.Invalidate();
		return false;
	}
	return registry.immutable_snapshot.Publish(builder);
}

bool PublishFreedMapping(MappingRegistry& registry, uint64_t owner)
{
	if (!NeedsPermissionSnapshot()) return true;
	size_t current_count = 0;
	const auto* current =
	    registry.immutable_snapshot.CurrentEntriesForWriter(&current_count);
	if (current == nullptr) return PublishFullSnapshot(registry);

	ImmutableSnapshot::Builder builder;
	if (!builder.Reserve(current_count))
	{
		registry.immutable_snapshot.Invalidate();
		return false;
	}
	bool removed = false;
	for (size_t index = 0; index < current_count; index++)
	{
		if (current[index].owner == owner)
		{
			removed = true;
			continue;
		}
		if (!builder.Append(current[index]))
		{
			registry.immutable_snapshot.Invalidate();
			return false;
		}
	}
	return removed ? registry.immutable_snapshot.Publish(builder)
	               : PublishFullSnapshot(registry);
}

bool RegisterMapping(uint64_t raw_base, uint64_t raw_size, uint64_t visible_base, uint64_t visible_size, Mode mode)
{
	const uint64_t host_page_size       = HostPageSize();
	uint64_t       rounded_raw_size     = 0;
	uint64_t       rounded_visible_size = 0;
	uint64_t       visible_end          = 0;
	uint64_t       raw_end              = 0;
	uint64_t       physical_visible_end = 0;
	if (host_page_size < GUEST_PAGE_SIZE || host_page_size % GUEST_PAGE_SIZE != 0 ||
	    !RoundUp(raw_size, host_page_size, &rounded_raw_size) || !ValidateMappingSize(rounded_raw_size) ||
	    !RoundUp(visible_size, GUEST_PAGE_SIZE, &rounded_visible_size) ||
	    !AddWithoutOverflow(visible_base, visible_size, &visible_end) ||
	    !RoundUp(visible_end, host_page_size, &physical_visible_end) ||
	    !AddWithoutOverflow(raw_base, rounded_raw_size, &raw_end) || visible_base < raw_base || visible_end > raw_end ||
	    physical_visible_end > raw_end || (raw_base & (host_page_size - 1)) != 0 ||
	    (visible_base & (host_page_size - 1)) != 0)
	{
		return false;
	}

	const uint64_t page_count = rounded_visible_size / GUEST_PAGE_SIZE;
	if (page_count == 0 || page_count > static_cast<uint64_t>((std::numeric_limits<size_t>::max)() / sizeof(Mode)))
	{
		return false;
	}

	auto* page_modes = static_cast<Mode*>(std::malloc(static_cast<size_t>(page_count) * sizeof(Mode)));
	if (page_modes == nullptr)
	{
		return false;
	}
	for (uint64_t page = 0; page < page_count; page++)
	{
		page_modes[static_cast<size_t>(page)] = mode;
	}

	auto&                       registry = Registry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	const size_t                insertion = LowerBoundMapping(registry, visible_base);
	if ((insertion < registry.mapping_num && registry.mappings[insertion].visible_base == visible_base) ||
	    registry.next_identity == 0 ||
	    registry.next_identity > (std::numeric_limits<uint64_t>::max)() / 2 ||
	    !EnsureRegistryCapacity(registry))
	{
		std::free(page_modes);
		return false;
	}

	ImmutableSnapshotMutation snapshot_mutation(registry);
	if (insertion < registry.mapping_num)
	{
		std::memmove(&registry.mappings[insertion + 1], &registry.mappings[insertion],
		             (registry.mapping_num - insertion) * sizeof(Mapping));
	}
	Mapping mapping {};
	mapping.raw_base = raw_base;
	mapping.raw_size = rounded_raw_size;
	mapping.visible_base = visible_base;
	mapping.visible_size = visible_size;
	mapping.page_modes = page_modes;
	mapping.page_mode_num = page_count;
	mapping.identity = registry.next_identity++;
	registry.mappings[insertion] = mapping;
	registry.mapping_num++;
	RecordMutation(registry);
	MarkMappingContinuity(registry, registry.mappings[insertion]);
	(void)PublishRegisteredMapping(registry, registry.mappings[insertion]);
	return true;
}

uint64_t AllocateRaw(uint64_t address, uint64_t size, Mode mode)
{
	if (!ValidateMappingSize(size))
	{
		return 0;
	}

	EnableIntelJitWrites(mode);
	void* result = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(address)), static_cast<size_t>(size), HostProtection(mode),
	                    MappingFlags(mode), -1, 0);
	if (result == MAP_FAILED)
	{
		std::fprintf(stderr, "[VirtualMemoryApple] mmap failed: address=0x%llx size=0x%llx mode=%u errno=%d (%s)\n",
		             static_cast<unsigned long long>(address), static_cast<unsigned long long>(size), static_cast<unsigned>(mode), errno,
		             std::strerror(errno));
		return 0;
	}
	return reinterpret_cast<uint64_t>(result);
}

int ReservationMappingFlags()
{
	int flags = MAP_PRIVATE | MAP_ANON;
#if defined(MAP_NORESERVE)
	flags |= MAP_NORESERVE;
#endif
	return flags;
}

uint64_t AllocateReservationRaw(uint64_t address, uint64_t size,
	                            uint64_t alignment)
{
	if (!ValidateMappingSize(size) || !IsPowerOfTwo(alignment) ||
	    alignment < HostPageSize() || alignment % HostPageSize() != 0 ||
	    size % HostPageSize() != 0)
	{
		return 0;
	}

	if (address != 0)
	{
		if (address % alignment != 0) return 0;
		void* result = mmap(
		    reinterpret_cast<void*>(static_cast<uintptr_t>(address)),
		    static_cast<size_t>(size), PROT_NONE, ReservationMappingFlags(), -1,
		    0);
		if (result == MAP_FAILED) return 0;
		const uint64_t mapped = reinterpret_cast<uint64_t>(result);
		if (mapped != address)
		{
			(void)munmap(result, static_cast<size_t>(size));
			return 0;
		}
		return mapped;
	}

	uint64_t raw_size = 0;
	if (!AddWithoutOverflow(size, alignment, &raw_size) ||
	    !ValidateMappingSize(raw_size))
	{
		return 0;
	}
	void* raw_result = mmap(nullptr, static_cast<size_t>(raw_size), PROT_NONE,
	                        ReservationMappingFlags(), -1, 0);
	if (raw_result == MAP_FAILED) return 0;

	const uint64_t raw = reinterpret_cast<uint64_t>(raw_result);
	uint64_t aligned = 0;
	uint64_t raw_end = 0;
	uint64_t aligned_end = 0;
	if (!RoundUp(raw, alignment, &aligned) ||
	    !AddWithoutOverflow(raw, raw_size, &raw_end) ||
	    !AddWithoutOverflow(aligned, size, &aligned_end) ||
	    aligned_end > raw_end)
	{
		(void)munmap(raw_result, static_cast<size_t>(raw_size));
		return 0;
	}

	const uint64_t prefix = aligned - raw;
	const uint64_t suffix = raw_end - aligned_end;
	if (prefix != 0 &&
	    munmap(raw_result, static_cast<size_t>(prefix)) != 0)
	{
		(void)munmap(raw_result, static_cast<size_t>(raw_size));
		return 0;
	}
	if (suffix != 0 &&
	    munmap(reinterpret_cast<void*>(static_cast<uintptr_t>(aligned_end)),
	           static_cast<size_t>(suffix)) != 0)
	{
		(void)munmap(reinterpret_cast<void*>(static_cast<uintptr_t>(aligned)),
		             static_cast<size_t>(raw_end - aligned));
		return 0;
	}
	return aligned;
}

bool ReplaceReservationRange(uint64_t address, uint64_t size, Mode mode)
{
	if (!ValidateMappingSize(size) || address == 0 ||
	    address % HostPageSize() != 0 || size % HostPageSize() != 0)
	{
		return false;
	}
	EnableIntelJitWrites(mode);
	void* result = mmap(
	    reinterpret_cast<void*>(static_cast<uintptr_t>(address)),
	    static_cast<size_t>(size), HostProtection(mode),
	    MappingFlags(mode) | MAP_FIXED, -1, 0);
	return result != MAP_FAILED && reinterpret_cast<uint64_t>(result) == address;
}

Mapping* FindContainingMapping(MappingRegistry& registry, uint64_t address)
{
	const size_t upper = UpperBoundMapping(registry, address);
	if (upper == 0)
	{
		return nullptr;
	}

	Mapping* mapping     = &registry.mappings[upper - 1];
	uint64_t visible_end = 0;
	if (!AddWithoutOverflow(mapping->visible_base, mapping->visible_size, &visible_end) || address < mapping->visible_base ||
	    address >= visible_end)
	{
		return nullptr;
	}
	return mapping;
}

Mapping* FindRawContainingMapping(MappingRegistry& registry, uint64_t address)
{
	const size_t insertion = LowerBoundMapping(registry, address);
	if (insertion < registry.mapping_num)
	{
		Mapping* mapping = &registry.mappings[insertion];
		uint64_t raw_end = 0;
		if (AddWithoutOverflow(mapping->raw_base, mapping->raw_size, &raw_end) && address >= mapping->raw_base && address < raw_end)
		{
			return mapping;
		}
	}
	if (insertion != 0)
	{
		Mapping* mapping = &registry.mappings[insertion - 1];
		uint64_t raw_end = 0;
		if (AddWithoutOverflow(mapping->raw_base, mapping->raw_size, &raw_end) && address >= mapping->raw_base && address < raw_end)
		{
			return mapping;
		}
	}
	return nullptr;
}

bool AnyMappingOverlap(const MappingRegistry& registry, uint64_t address,
	                   uint64_t size)
{
	for (size_t index = 0; index < registry.mapping_num; index++)
	{
		const Mapping& mapping = registry.mappings[index];
		if (RangesOverlap(address, size, mapping.raw_base, mapping.raw_size))
		{
			return true;
		}
	}
	return false;
}

bool AnyReservationOverlap(const MappingRegistry& registry, uint64_t address,
	                       uint64_t size)
{
	for (size_t index = 0; index < registry.reservation_num; index++)
	{
		const Reservation& reservation = registry.reservations[index];
		if (RangesOverlap(address, size, reservation.base, reservation.size))
		{
			return true;
		}
	}
	return false;
}

size_t FindMappingIdentity(const MappingRegistry& registry, uint64_t identity)
{
	for (size_t index = 0; index < registry.mapping_num; index++)
	{
		if (registry.mappings[index].identity == identity) return index;
	}
	return registry.mapping_num;
}

size_t FindReservationHandle(const MappingRegistry& registry,
	                         const ReservationHandle& handle)
{
	const size_t index = LowerBoundReservation(registry, handle.base);
	if (index >= registry.reservation_num) return registry.reservation_num;
	const Reservation& reservation = registry.reservations[index];
	return reservation.base == handle.base && reservation.size == handle.size &&
	               reservation.identity == handle.identity
	           ? index
	           : registry.reservation_num;
}

void EraseMapping(MappingRegistry& registry, size_t index)
{
	if (index >= registry.mapping_num) return;
	std::free(registry.mappings[index].page_modes);
	if (index + 1 < registry.mapping_num)
	{
		std::memmove(&registry.mappings[index], &registry.mappings[index + 1],
		             (registry.mapping_num - index - 1) * sizeof(Mapping));
	}
	registry.mapping_num--;
}

void EraseReservation(MappingRegistry& registry, size_t index)
{
	if (index >= registry.reservation_num) return;
	if (index + 1 < registry.reservation_num)
	{
		std::memmove(
		    &registry.reservations[index], &registry.reservations[index + 1],
		    (registry.reservation_num - index - 1) * sizeof(Reservation));
	}
	registry.reservation_num--;
}

struct ProtectionRange
{
	Mapping* mapping    = nullptr;
	uint64_t begin      = 0;
	uint64_t end        = 0;
	uint64_t first_page = 0;
	uint64_t page_num   = 0;
};

bool ResolveProtectionRange(MappingRegistry& registry, uint64_t address, uint64_t size, ProtectionRange* range)
{
	uint64_t requested_end = 0;
	if (range == nullptr || address == 0 || size == 0 || !AddWithoutOverflow(address, size, &requested_end))
	{
		return false;
	}

	Mapping* mapping = FindContainingMapping(registry, address);
	if (mapping == nullptr)
	{
		return false;
	}

	uint64_t visible_end = 0;
	if (!AddWithoutOverflow(mapping->visible_base, mapping->visible_size, &visible_end) || requested_end > visible_end)
	{
		return false;
	}

	const uint64_t begin = address & ~(GUEST_PAGE_SIZE - 1);
	uint64_t       end   = 0;
	if (!RoundUp(requested_end, GUEST_PAGE_SIZE, &end) || end <= begin || begin < mapping->visible_base)
	{
		return false;
	}

	const uint64_t first_page = (begin - mapping->visible_base) / GUEST_PAGE_SIZE;
	const uint64_t page_num   = (end - begin) / GUEST_PAGE_SIZE;
	if (first_page >= mapping->page_mode_num || page_num > mapping->page_mode_num - first_page)
	{
		return false;
	}

	*range = ProtectionRange {mapping, begin, end, first_page, page_num};
	return true;
}

Mode HostEnvelope(const Mapping& mapping, uint64_t host_page_address, const ProtectionRange* replacement, Mode replacement_mode)
{
	if (host_page_address < mapping.visible_base)
	{
		return Mode::NoAccess;
	}
	const uint64_t first_page     = (host_page_address - mapping.visible_base) / GUEST_PAGE_SIZE;
	if (first_page >= mapping.page_mode_num)
	{
		return Mode::NoAccess;
	}
	const uint64_t pages_per_host = HostPageSize() / GUEST_PAGE_SIZE;
	const uint64_t remaining      = mapping.page_mode_num - first_page;
	const uint64_t page_num       = remaining < pages_per_host ? remaining : pages_per_host;
	uint32_t       mode_bits      = 0;

	for (uint64_t page = 0; page < page_num; page++)
	{
		const uint64_t guest_page = first_page + page;
		Mode           guest_mode = mapping.page_modes[static_cast<size_t>(guest_page)];
		if (replacement != nullptr && guest_page >= replacement->first_page &&
		    guest_page - replacement->first_page < replacement->page_num)
		{
			guest_mode = replacement_mode;
		}
		mode_bits |= static_cast<uint32_t>(guest_mode);
	}
	return static_cast<Mode>(mode_bits);
}

bool PhysicalRange(const ProtectionRange& range, uint64_t* begin, uint64_t* end)
{
	if (begin == nullptr || end == nullptr)
	{
		return false;
	}

	const uint64_t host_page_size = HostPageSize();
	*begin                        = range.begin & ~(host_page_size - 1);
	return RoundUp(range.end, host_page_size, end) && *end > *begin;
}

bool HasRepresentableDataProtection(const ProtectionRange& range, Mode mode)
{
	if (!RequiresUniformHostPageDataProtection()) return true;
	uint64_t begin = 0;
	uint64_t end = 0;
	if (!PhysicalRange(range, &begin, &end)) return false;
	const Mapping& mapping = *range.mapping;
	const uint64_t pages_per_host = HostPageSize() / GUEST_PAGE_SIZE;
	for (uint64_t host = begin; host < end; host += HostPageSize())
	{
		const uint64_t first = (host - mapping.visible_base) / GUEST_PAGE_SIZE;
		const uint64_t count = std::min(pages_per_host, mapping.page_mode_num - first);
		int expected = -1;
		for (uint64_t page = first; page < first + count; page++)
		{
			const Mode desired = page >= range.first_page &&
			                             page - range.first_page < range.page_num
			                         ? mode
			                         : mapping.page_modes[static_cast<size_t>(page)];
			const int protection = HostProtection(desired) & (PROT_READ | PROT_WRITE);
			if (expected != -1 && expected != protection) return false;
			expected = protection;
		}
	}
	return true;
}

bool RestoreHostEnvelopes(const Mapping& mapping, uint64_t begin, uint64_t end)
{
	const uint64_t host_page_size = HostPageSize();
	bool           restored       = true;
	uint64_t       cursor         = begin;
	while (cursor < end)
	{
		const Mode envelope   = HostEnvelope(mapping, cursor, nullptr, Mode::NoAccess);
		const int  protection = HostProtection(envelope);
		uint64_t   run_end     = cursor + host_page_size;
		while (run_end < end && HostProtection(HostEnvelope(mapping, run_end, nullptr, Mode::NoAccess)) == protection)
		{
			run_end += host_page_size;
		}

		EnableIntelJitWrites(envelope);
		if (mprotect(reinterpret_cast<void*>(static_cast<uintptr_t>(cursor)), static_cast<size_t>(run_end - cursor), protection) != 0)
		{
			restored = false;
		}
		cursor = run_end;
	}
	return restored;
}

bool ApplyHostEnvelopes(const ProtectionRange& range, Mode mode)
{
	uint64_t physical_begin = 0;
	uint64_t physical_end   = 0;
	if (!PhysicalRange(range, &physical_begin, &physical_end))
	{
		return false;
	}

	const uint64_t host_page_size = HostPageSize();
	uint64_t       cursor         = physical_begin;
	while (cursor < physical_end)
	{
		const Mode desired_mode       = HostEnvelope(*range.mapping, cursor, &range, mode);
		const int  desired_protection = HostProtection(desired_mode);
		bool     physical_change =
		    HostProtection(HostEnvelope(*range.mapping, cursor, nullptr, Mode::NoAccess)) != desired_protection;
		uint64_t run_end = cursor + host_page_size;
		while (run_end < physical_end &&
		       HostProtection(HostEnvelope(*range.mapping, run_end, &range, mode)) == desired_protection)
		{
			physical_change = physical_change ||
			                  HostProtection(HostEnvelope(*range.mapping, run_end, nullptr, Mode::NoAccess)) != desired_protection;
			run_end += host_page_size;
		}

		if (physical_change)
		{
			EnableIntelJitWrites(desired_mode);
			if (mprotect(reinterpret_cast<void*>(static_cast<uintptr_t>(cursor)), static_cast<size_t>(run_end - cursor),
			             desired_protection) != 0)
			{
				(void)RestoreHostEnvelopes(*range.mapping, physical_begin, run_end);
				return false;
			}
		}
		cursor = run_end;
	}
	return true;
}

bool PublishProtectedMapping(MappingRegistry& registry,
	                         const ProtectionRange& range, Mode mode)
{
	if (!NeedsPermissionSnapshot()) return true;
	if (range.mapping->reservation_identity != 0)
	{
		return PublishVirtualRangeSnapshot(registry);
	}
	const uint64_t mapping_owner = MappingSnapshotOwner(*range.mapping);
	if (mapping_owner == 0)
	{
		registry.immutable_snapshot.Invalidate();
		return false;
	}
	size_t current_count = 0;
	const auto* current =
	    registry.immutable_snapshot.CurrentEntriesForWriter(&current_count);
	if (current == nullptr) return PublishFullSnapshot(registry);
	if (current_count > (std::numeric_limits<size_t>::max)() - 2)
	{
		registry.immutable_snapshot.Invalidate();
		return false;
	}

	uint64_t visible_end = 0;
	if (!AddWithoutOverflow(range.mapping->visible_base,
	                        range.mapping->visible_size, &visible_end))
	{
		registry.immutable_snapshot.Invalidate();
		return false;
	}
	const uint64_t replacement_begin = range.begin;
	const uint64_t replacement_end = range.end < visible_end ? range.end : visible_end;

	ImmutableSnapshot::Builder builder;
	if (!builder.Reserve(current_count + 2))
	{
		registry.immutable_snapshot.Invalidate();
		return false;
	}
	bool replaced = false;
	for (size_t index = 0; index < current_count; index++)
	{
		const auto& entry = current[index];
		if (entry.owner != mapping_owner ||
		    entry.state != ManagedRegionState::Visible ||
		    entry.end <= replacement_begin || entry.begin >= replacement_end)
		{
			if (!builder.Append(entry))
			{
				registry.immutable_snapshot.Invalidate();
				return false;
			}
			continue;
		}

		replaced = true;
		if (entry.begin < replacement_begin &&
		    !builder.Append({entry.begin, replacement_begin, entry.owner,
		                     entry.state, entry.mode}))
		{
			registry.immutable_snapshot.Invalidate();
			return false;
		}
		const uint64_t overlap_begin =
		    entry.begin > replacement_begin ? entry.begin : replacement_begin;
		const uint64_t overlap_end =
		    entry.end < replacement_end ? entry.end : replacement_end;
		if (!builder.Append({overlap_begin, overlap_end, entry.owner,
		                     ManagedRegionState::Visible, mode}))
		{
			registry.immutable_snapshot.Invalidate();
			return false;
		}
		if (entry.end > replacement_end &&
		    !builder.Append({replacement_end, entry.end, entry.owner,
		                     entry.state, entry.mode}))
		{
			registry.immutable_snapshot.Invalidate();
			return false;
		}
	}

	return replaced ? registry.immutable_snapshot.Publish(builder)
	                : PublishFullSnapshot(registry);
}

bool ApplyProtection(MappingRegistry& registry, const ProtectionRange& range, Mode mode, Mode* old_mode)
{
	// Check every affected host page before changing any physical permission or
	// logical metadata. OR-ing mixed guest permissions is safe for the software
	// checked interpreter, but lets direct FEX stores bypass read-only/guard
	// subpages. RX and R remain compatible because execution has its own gate.
	if (!HasRepresentableDataProtection(range, mode)) return false;
	const Mode previous_mode = range.mapping->page_modes[static_cast<size_t>(range.first_page)];
	std::memset(range.mapping->validated_code_host_pages, 0,
	            sizeof(range.mapping->validated_code_host_pages));
	range.mapping->validated_code_host_page_next = 0;
	if (!ApplyHostEnvelopes(range, mode))
	{
		MarkMappingContinuity(registry, *range.mapping, true);
		return false;
	}
	if (ConsumeVirtualRangeTestFault(VirtualRangeTestFault::ProtectAfterHost))
	{
		uint64_t begin = 0, end = 0;
		if (PhysicalRange(range, &begin, &end)) (void)RestoreHostEnvelopes(*range.mapping, begin, end);
		MarkMappingContinuity(registry, *range.mapping, true);
		return false;
	}

	for (uint64_t page = 0; page < range.page_num; page++)
	{
		range.mapping->page_modes[static_cast<size_t>(range.first_page + page)] = mode;
	}
	if (old_mode != nullptr)
	{
		*old_mode = previous_mode;
	}
	RecordMutation(registry);
	MarkMappingContinuity(registry, *range.mapping);
	(void)PublishProtectedMapping(registry, range, mode);
	return true;
}

bool CodeReadRangeEligible(const ProtectionRange& range)
{
	if (range.mapping == nullptr) return false;
	for (uint64_t page = 0; page < range.page_num; page++)
	{
		const Mode mode = range.mapping->page_modes[
		    static_cast<size_t>(range.first_page + page)];
		const int protection = HostProtection(mode);
		if (!IsExecute(mode) || (static_cast<uint32_t>(mode) &
		     static_cast<uint32_t>(Mode::Write)) != 0 ||
		    (protection & PROT_READ) == 0 || (protection & PROT_WRITE) != 0)
		{
			return false;
		}
	}

	// Darwin may merge four logical 4 KiB guest pages into one 16 KiB physical
	// permission envelope. Refuse a lease when a writable sibling keeps that
	// physical page writable: a previously translated guest Store must never be
	// able to race a code reader silently through the wider host permission.
	uint64_t physical_begin = 0;
	uint64_t physical_end = 0;
	if (!PhysicalRange(range, &physical_begin, &physical_end)) return false;
	for (uint64_t cursor = physical_begin; cursor < physical_end;
	     cursor += HostPageSize())
	{
		const int protection = HostProtection(
		    HostEnvelope(*range.mapping, cursor, nullptr, Mode::NoAccess));
		if ((protection & PROT_READ) == 0 || (protection & PROT_WRITE) != 0)
			return false;
	}
	return true;
}

bool GuestDataSnapshotPreflightEnabled()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	// FEX maintains its permission snapshot even when ordinary snapshot queries
	// are disabled. The accelerated logical preflight is the Mac FEX default;
	// any explicit value other than exact "1" selects the registry rollback.
	static const bool enabled = []
	{
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_GUEST_DATA_SNAPSHOT_PREFLIGHT");
		return value == nullptr || std::strcmp(value, "1") == 0;
	}();
	return enabled;
#else
	return false;
#endif
}

enum class GuestDataPreflight
{
	Unavailable,
	Permitted,
	Denied
};

GuestDataPreflight PreflightGuestDataSnapshot(MappingRegistry& registry,
                                             uint64_t address, uint64_t end, int permission)
{
	// The caller already holds the VM reader gate, so all lookups see stable
	// Core mappings. An owner publishing a transaction output must instead use
	// its current registry state, which may not have a usable snapshot yet.
	if (!GuestDataSnapshotPreflightEnabled() || g_vm_mutation_owner == &registry)
		return GuestDataPreflight::Unavailable;
	while (address < end)
	{
		ManagedRegionState state = ManagedRegionState::Unmanaged;
		ManagedRegion region {};
		uint64_t next_region = 0;
		if (!registry.immutable_snapshot.Lookup(address, &state, &region,
		                                        false, false, &next_region))
			return GuestDataPreflight::Unavailable;
		if (state == ManagedRegionState::Visible)
		{
			if (region.end <= address || (HostProtection(region.mode) & permission) != permission)
				return GuestDataPreflight::Denied;
			address = std::min(end, region.end);
		} else if (state == ManagedRegionState::Unmanaged && next_region > address)
		{
			// A native interval ends at the next managed entry. Its Mach region
			// can also contain a logical guard or reservation that must be checked.
			address = std::min(end, next_region);
		} else
		{
			return GuestDataPreflight::Denied;
		}
	}
	return GuestDataPreflight::Permitted;
}

bool PhysicalRangeHasProtection(uint64_t begin, uint64_t end,
                                vm_prot_t required, vm_prot_t forbidden,
                                GuestDataLeaseDenialDiagnostic* diagnostic = nullptr)
{
	if (begin == 0 || begin >= end) return false;
	uint64_t cursor = begin;
	while (cursor < end)
	{
	#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		mach_vm_address_t region_address = cursor;
		mach_vm_size_t region_size = 0;
	#else
		vm_address_t region_address = static_cast<vm_address_t>(cursor);
		vm_size_t region_size = 0;
	#endif
		vm_region_basic_info_data_64_t info {};
		mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t object_name = MACH_PORT_NULL;
	#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		const kern_return_t result = mach_vm_region(
		    mach_task_self(), &region_address, &region_size,
		    VM_REGION_BASIC_INFO_64,
		    reinterpret_cast<vm_region_info_t>(&info), &info_count,
		    &object_name);
	#else
		const kern_return_t result = vm_region_64(
		    mach_task_self(), &region_address, &region_size,
		    VM_REGION_BASIC_INFO_64,
		    reinterpret_cast<vm_region_info_t>(&info), &info_count,
		    &object_name);
	#endif
		if (object_name != MACH_PORT_NULL)
			mach_port_deallocate(mach_task_self(), object_name);
		uint64_t region_end = 0;
		if (result != KERN_SUCCESS || region_address > cursor ||
		    region_size == 0 ||
		    !AddWithoutOverflow(static_cast<uint64_t>(region_address),
		                        static_cast<uint64_t>(region_size),
		                        &region_end) || region_end <= cursor ||
		    (info.protection & required) != required ||
		    (info.protection & forbidden) != 0)
		{
			if (diagnostic != nullptr)
			{
				diagnostic->physical_observed = result == KERN_SUCCESS;
				diagnostic->physical_result = result;
				diagnostic->physical_protection = result == KERN_SUCCESS ? info.protection : VM_PROT_NONE;
				diagnostic->Deny(GuestDataLeaseDenialReason::PhysicalQueryOrRightsDenied);
			}
			return false;
		}
		cursor = region_end < end ? region_end : end;
	}
	return true;
}

bool PhysicalRangeCurrentlyReadOnly(uint64_t begin, uint64_t end)
{
	return PhysicalRangeHasProtection(begin, end, VM_PROT_READ, VM_PROT_WRITE);
}

bool IsCodeHostPageValidated(const Mapping& mapping, uint64_t physical_begin)
{
	for (uint64_t validated: mapping.validated_code_host_pages)
	{
		if (validated == physical_begin) return true;
	}
	return false;
}

void MarkCodeHostPageValidated(Mapping& mapping, uint64_t physical_begin)
{
	const uint8_t slot = static_cast<uint8_t>(
	    mapping.validated_code_host_page_next %
	    (sizeof(mapping.validated_code_host_pages) /
	     sizeof(mapping.validated_code_host_pages[0])));
	mapping.validated_code_host_pages[slot] = physical_begin;
	mapping.validated_code_host_page_next = static_cast<uint8_t>(slot + 1u);
}

bool FlushInstructionCacheRaw(uint64_t address, uint64_t size)
{
	uint64_t end = 0;
	if (!AddWithoutOverflow(address, size, &end) ||
	    size > static_cast<uint64_t>((std::numeric_limits<size_t>::max)()))
	{
		return false;
	}
#if LUMEN_ABI == LUMEN_ABI_ARM64_V8A
	(void)end;
	sys_icache_invalidate(reinterpret_cast<void*>(static_cast<uintptr_t>(address)),
	                      static_cast<size_t>(size));
#else
	__builtin___clear_cache(
	    reinterpret_cast<char*>(static_cast<uintptr_t>(address)),
	    reinterpret_cast<char*>(static_cast<uintptr_t>(end)));
#endif
	return true;
}

bool AbortTransactionState(MutationTransactionState& transaction)
{
	auto& registry = Registry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	ImmutableSnapshotMutation snapshot_mutation(registry);

	if (transaction.kind == TransactionKind::Reservation)
	{
		const size_t reservation_index =
		    FindReservationHandle(registry, transaction.reservation);
		if (reservation_index >= registry.reservation_num) return false;
		if (ConsumeVirtualRangeTestFault(VirtualRangeTestFault::AbortHost) ||
		    munmap(reinterpret_cast<void*>(static_cast<uintptr_t>(
		                 transaction.reservation.base)),
		           static_cast<size_t>(transaction.reservation.size)) != 0)
		{
			MarkRangeContinuity(registry, transaction.range_base, transaction.range_size, true);
			transaction.fex.Commit(transaction.range_base,
			                       transaction.range_size);
			return false;
		}

		for (size_t index = registry.mapping_num; index != 0; index--)
		{
			if (registry.mappings[index - 1].reservation_identity ==
			    transaction.reservation.identity)
			{
				EraseMapping(registry, index - 1);
			}
		}
		EraseReservation(registry, reservation_index);
		RecordMutation(registry);
		(void)PublishVirtualRangeSnapshot(registry);
		return true;
	}

	const size_t mapping_index =
	    FindMappingIdentity(registry, transaction.mapping_identity);
	if (mapping_index >= registry.mapping_num) return false;
	if (ConsumeVirtualRangeTestFault(VirtualRangeTestFault::AbortHost) ||
	    !ReplaceReservationRange(transaction.range_base,
	                             transaction.range_size, Mode::NoAccess))
	{
		MarkMappingContinuity(registry, registry.mappings[mapping_index], true);
		transaction.fex.Commit(transaction.range_base,
		                       transaction.range_size);
		return false;
	}
	EraseMapping(registry, mapping_index);
	RecordMutation(registry);
	(void)PublishVirtualRangeSnapshot(registry);
	return true;
}

} // namespace

void Init()
{
	static std::once_flag init_once;
	std::call_once(init_once,
	               []
	               {
		               auto& registry = Registry();
		               if (NeedsPermissionSnapshot())
		               {
			               std::lock_guard<std::mutex> lock(registry.mutex);
			               (void)PublishFullSnapshot(registry);
		               }
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS) && !defined(LUMEN_APPLE_VM_STANDALONE)
		               sys_virtual_init();
#endif
	               });
}

bool IsImmutableRegionSnapshotEnabled()
{
	return ImmutableSnapshot::Requested();
}

bool RequiresUniformHostPageDataProtection()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	return true;
#else
	return false;
#endif
}

bool RegisterGuestBackingOwner(GuestBackingOwnerQuery query, GuestBackingOwnerMark mark,
    GuestBackingOwnerPresence presence, GuestBackingOwnerGpuCopy copy, GuestBackingOwnerMarkOwned mark_owned) noexcept
{
	if (query == nullptr || mark == nullptr) return false;
	static std::mutex registration_mutex;
	std::lock_guard lock(registration_mutex);
	const auto current = g_backing_owner_query.load(std::memory_order_acquire);
	if (current != nullptr)
		return current == query && g_backing_owner_mark.load(std::memory_order_acquire) == mark &&
		    g_backing_owner_mark_owned.load(std::memory_order_acquire) == mark_owned &&
		    g_backing_owner_presence.load(std::memory_order_acquire) == presence &&
		    g_backing_owner_gpu_copy.load(std::memory_order_acquire) == copy;
	g_backing_owner_mark.store(mark, std::memory_order_release);
	g_backing_owner_mark_owned.store(mark_owned, std::memory_order_release);
	g_backing_owner_presence.store(presence, std::memory_order_release);
	g_backing_owner_gpu_copy.store(copy, std::memory_order_release);
	g_backing_owner_query.store(query, std::memory_order_release);
	return true;
}

uint64_t GetMutationGeneration()
{
	return Registry().mutation_generation.load(std::memory_order_acquire);
}

#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
bool GuestDataSnapshotPreflightEnabledForTest()
{
	return GuestDataSnapshotPreflightEnabled();
}

void FailNextVirtualRangeOperationForTest(VirtualRangeFaultForTest fault)
{
	static_assert(static_cast<uint32_t>(VirtualRangeFaultForTest::ProtectAfterHost) ==
	              static_cast<uint32_t>(VirtualRangeTestFault::ProtectAfterHost));
	g_virtual_range_test_fault.store(static_cast<uint32_t>(fault),
	                                 std::memory_order_release);
}

void SetGuestDataPhysicalValidationHookForTest(void (*hook)())
{
	g_guest_data_physical_validation_hook = hook;
}

void SetMemoryMutationLocalReleaseHookForTest(void (*hook)())
{
	g_memory_mutation_local_release_hook = hook;
}

void WithGuestDataGateLockedForTest(void (*callback)(void*), void* context)
{
	if (callback == nullptr) return;
	std::lock_guard<std::mutex> lock(Registry().mutation_mutex);
	callback(context);
}

size_t ReservationCountForTest()
{
	auto& registry = Registry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	return registry.reservation_num;
}

VirtualRangeTransactionStateForTest
GetVirtualRangeTransactionStateForTest()
{
	auto& registry = Registry();
	std::lock_guard<std::mutex> lock(registry.mutation_mutex);
	return VirtualRangeTransactionStateForTest {
	    g_virtual_range_transaction_begins.load(std::memory_order_relaxed),
	    g_virtual_range_transaction_closes.load(std::memory_order_relaxed),
	    g_virtual_range_transaction_gate_closes.load(
	        std::memory_order_relaxed),
	    g_virtual_range_transaction_active.load(std::memory_order_relaxed),
	    registry.waiting_guest_data_readers, registry.waiting_mutations};
}
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
bool CurrentThreadHasMemoryLease() noexcept
{
	return g_vm_readers != 0;
}
#endif

bool TryAcquireCodeReadLease(uint64_t address, uint64_t size,
	                         CodeReadLease* lease)
{
	CodeReadLeaseDenialDiagnostic diagnostic {address, size};
	if ((g_vm_external_native_token != 0 || GuestCpu::CurrentMemoryHandoffNativeClaimOwner() != 0) &&
	    !CurrentNativeClaimCoversGuestRange(address, size)) return diagnostic.Deny("native-claim");
	if (lease == nullptr || lease->mapping_identity != 0)
		return diagnostic.Deny("lease-state");
	lease->mapping_identity = 0;
	lease->host_begin = 0;
	lease->byte_count = 0;
	lease->mutation_generation = 0;
	if (size == 0 || size > static_cast<uint64_t>(
	                         (std::numeric_limits<size_t>::max)()))
	{
		return diagnostic.Deny("size");
	}

	auto& registry = Registry();
	if (!BeginCodeReadGate(registry, &diagnostic))
		return diagnostic.Deny("gate-busy");
	std::lock_guard<std::mutex> lock(registry.mutex);
	uint64_t requested_end = 0;
	ProtectionRange range {};
	if (!AddWithoutOverflow(address, size, &requested_end) ||
	    ((address ^ (requested_end - 1u)) & ~(GUEST_PAGE_SIZE - 1u)) != 0)
	{
		EndCodeReadGate(registry);
		return diagnostic.Deny("guest-page-range");
	}
	if (!ResolveProtectionRange(registry, address, size, &range) ||
	    !CodeReadRangeEligible(range) || range.mapping->identity == 0)
	{
		EndCodeReadGate(registry);
		return diagnostic.Deny("logical-range-or-host-envelope");
	}
	uint64_t physical_begin = 0;
	uint64_t physical_end = 0;
	if (!PhysicalRange(range, &physical_begin, &physical_end))
	{
		EndCodeReadGate(registry);
		return diagnostic.Deny("physical-range");
	}
	diagnostic.physical_begin = physical_begin;
	diagnostic.physical_end = physical_end;
	if (!IsCodeHostPageValidated(*range.mapping, physical_begin) &&
	    !PhysicalRangeCurrentlyReadOnly(physical_begin, physical_end))
	{
		EndCodeReadGate(registry);
		return diagnostic.Deny("physical-protection");
	}
	if (!IsCodeHostPageValidated(*range.mapping, physical_begin))
		MarkCodeHostPageValidated(*range.mapping, physical_begin);

	lease->mapping_identity = range.mapping->identity;
	lease->host_begin = address;
	lease->byte_count = size;
	lease->mutation_generation =
	    registry.mutation_generation.load(std::memory_order_relaxed);
	return true;
}

void ReleaseCodeReadLease(CodeReadLease* lease)
{
	if (lease == nullptr || lease->mapping_identity == 0) return;

	auto& registry = Registry();
	lease->mapping_identity = 0;
	lease->host_begin = 0;
	lease->byte_count = 0;
	lease->mutation_generation = 0;
	EndCodeReadGate(registry);
}

GuestMemoryMutationOwnerScope::GuestMemoryMutationOwnerScope(uint64_t owned_token)
{
#if defined(LUMEN_FEXCORE_RUNTIME)
	if (owned_token == 0 || g_vm_readers != 0 || g_vm_mutation_owner != nullptr ||
	    g_vm_external_mutation_token != 0 || g_vm_external_native_token != 0 ||
	    LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(owned_token) != 1) return;
	auto& registry = Registry();
	std::unique_lock lock(registry.mutation_mutex);
	registry.mutation_ready.wait(lock, [&registry]
	{
		return !registry.mutation_active && registry.waiting_mutations == 0;
	});
	if (registry.code_readers == UINT64_MAX) return;
	// FEX already excludes guest executors and Core mutations (which acquire
	// FEX before the local gate). Keep one independent local lifetime pin,
	// allowing checked native completion borrows on other threads. This pin
	// is not a data borrow: g_vm_readers remains zero until an actual lease.
	++registry.code_readers;
	g_vm_mutation_owner = &registry;
	g_vm_external_mutation_token = owned_token;
	g_vm_metadata_scope = Detail::NewMemoryMetadataScope();
	token = owned_token;
	acquiring_thread = reinterpret_cast<uintptr_t>(&g_vm_readers);
#else
	(void)owned_token;
#endif
}

GuestMemoryMutationOwnerScope::~GuestMemoryMutationOwnerScope()
{
	if (token == 0) return;
#if defined(LUMEN_FEXCORE_RUNTIME)
	auto& registry = Registry();
	if (acquiring_thread != reinterpret_cast<uintptr_t>(&g_vm_readers) ||
	    g_vm_readers != 0 || g_vm_mutation_owner != &registry ||
	    g_vm_external_mutation_token != token ||
	    LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(token) != 1) std::abort();
	{
		std::lock_guard lock(registry.mutation_mutex);
		if (registry.mutation_active || registry.code_readers == 0) std::abort();
		--registry.code_readers; // Remove only our pin; native completions may remain.
		g_vm_external_mutation_token = 0;
		g_vm_metadata_scope = 0;
		g_vm_mutation_owner = nullptr;
	}
	registry.mutation_ready.notify_all();
	token = 0;
#endif
}

GuestDataLease::~GuestDataLease()
{
	ReleaseGuestDataLease(this);
}

GuestNativeMemoryOwnerScope::GuestNativeMemoryOwnerScope(uint64_t native_token)
{
#if defined(LUMEN_FEXCORE_RUNTIME)
	if (native_token == 0 || g_vm_readers != 0 || g_vm_mutation_owner != nullptr ||
	    g_vm_external_mutation_token != 0 || g_vm_external_native_token != 0 ||
	    LumenAndroidFexCoreOwnsNativeGuestDataLeaseV1(native_token) != 1) return;
	auto& registry = Registry();
	std::unique_lock lock(registry.mutation_mutex, std::try_to_lock);
	if (!lock.owns_lock() || registry.mutation_active || registry.waiting_mutations != 0 ||
	    registry.code_readers == UINT64_MAX) return;
	// The backend pin excludes destructive mapping changes. This additional
	// local pin also keeps additive Core transactions out of these exact views.
	// It is not a data borrow or a mutation owner; nested accesses remain checked.
	++registry.code_readers;
	g_vm_external_native_token = native_token;
	token = native_token;
	acquiring_thread = reinterpret_cast<uintptr_t>(&g_vm_readers);
#else
	(void)native_token;
#endif
}

GuestNativeMemoryOwnerScope::~GuestNativeMemoryOwnerScope()
{
	if (token == 0) return;
#if defined(LUMEN_FEXCORE_RUNTIME)
	auto& registry = Registry();
	if (acquiring_thread != reinterpret_cast<uintptr_t>(&g_vm_readers) || g_vm_readers != 0 ||
	    g_vm_mutation_owner != nullptr || g_vm_external_native_token != token ||
	    !OwnsExternalNativeOwner()) std::abort();
	{
		std::lock_guard lock(registry.mutation_mutex);
		if (registry.mutation_active || registry.code_readers == 0) std::abort();
		--registry.code_readers;
		g_vm_external_native_token = 0;
	}
	registry.mutation_ready.notify_all();
	token = 0;
#endif
}

bool CurrentNativeClaimCoversGuestRange(uint64_t address, uint64_t bytes) noexcept
{
	if (address == 0 || bytes == 0 || address > UINT64_MAX - bytes ||
	    GuestCpu::CurrentMemoryHandoffNativeClaimOwner() == 0 ||
	    (g_vm_external_native_token != 0 && !OwnsExternalNativeOwner())) return false;
	const auto end = address + bytes;
	while (address < end)
	{
		GuestBackingSpan span;
		if (!QueryGuestBackingSpan(address, end - address, &span) || span.identity == 0 ||
		    span.address != address || span.size == 0 || span.size > end - address ||
		    span.offset > UINT64_MAX - span.size ||
		    !GuestCpu::NativeClaimCoversPhysical({span.identity, span.offset, span.size})) return false;
		address += span.size;
	}
	return true;
}

uint64_t GetOwnedGuestMemoryMutationToken() noexcept
{
	return OwnsExternalMutationOwner(Registry()) ? g_vm_metadata_scope : 0;
}

bool CheckGuestDataAccessUnderFexExecutionBarrier(uint64_t address, uint64_t size,
                                                bool write) noexcept
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	return QueryFexDataAccess(address, size, write ? 1 : 0) != 0;
#else
	(void)address;
	(void)size;
	(void)write;
	return false;
#endif
}

bool CheckOwnedStackAccessUnderFexExecutionBarrier(uint64_t address, uint64_t size,
	bool write, uint64_t owned_begin, uint64_t owned_size) noexcept
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	uint64_t end = 0;
	uint64_t owned_end = 0;
	if (owned_begin == 0 || owned_size == 0 || size == 0 ||
	    !AddWithoutOverflow(owned_begin, owned_size, &owned_end) ||
	    !AddWithoutOverflow(address, size, &end) ||
	    address < owned_begin || end > owned_end) return false;
	auto& publisher = Registry().immutable_snapshot;
	const int required = PROT_READ | (write ? PROT_WRITE : 0);
	while (address < end)
	{
		ManagedRegionState state = ManagedRegionState::Unmanaged;
		ManagedRegion region {};
		uint64_t next_region = 0;
		if (!publisher.Lookup(address, &state, &region, false, true, &next_region)) return false;
		if (state == ManagedRegionState::Visible)
		{
			if (region.end <= address || (HostProtection(region.mode) & required) != required) return false;
			address = std::min(end, region.end);
		} else if (state == ManagedRegionState::Unmanaged && next_region > address)
		{
			// Stop at the next registry entry, even if it lies inside the
			// owner's interval. An ownership claim cannot bypass logical rights.
			address = std::min(end, next_region);
		} else
		{
			return false;
		}
	}
	return true;
#else
	(void)address; (void)size; (void)write; (void)owned_begin; (void)owned_size;
	return false;
#endif
}

bool TryAcquireGuestDataLease(uint64_t address, uint64_t size,
                              Mode required, GuestDataLease* lease, bool wait_for_mutation,
                              GuestDataLeaseDenialMetadata* denial)
{
	if (denial != nullptr) *denial = {};
	std::optional<GuestDataLeaseDenialDiagnostic> diagnostic;
	const bool trace = GuestDataLeaseDenialTraceEnabled();
	if (trace || denial != nullptr) diagnostic.emplace(address, size, required, denial, trace);
	uint64_t end = 0;
	if (lease == nullptr || lease->IsActive() || address == 0 ||
	    !ValidateMappingSize(size) || !AddWithoutOverflow(address, size, &end) ||
	    (required != Mode::Read && required != Mode::Write &&
	     required != Mode::ReadWrite))
	{
		if (diagnostic.has_value()) diagnostic->Deny(GuestDataLeaseDenialReason::LeaseOrRangeInvalid);
		return false;
	}
	if ((g_vm_external_native_token != 0 || GuestCpu::CurrentMemoryHandoffNativeClaimOwner() != 0) &&
	    !CurrentNativeClaimCoversGuestRange(address, size))
	{
		if (diagnostic.has_value()) diagnostic->Deny(GuestDataLeaseDenialReason::DataHandoffDenied);
		return false;
	}

	auto& registry = Registry();
	if (diagnostic.has_value()) diagnostic->generation = registry.mutation_generation.load(std::memory_order_relaxed);
	if (!BeginGuestDataGate(registry, wait_for_mutation, diagnostic.has_value() ? &*diagnostic : nullptr)) return false;
	const int permission = HostProtection(required);
	const auto snapshot_preflight = PreflightGuestDataSnapshot(registry, address, end, permission);
	bool permitted = snapshot_preflight != GuestDataPreflight::Denied;
	if (!permitted && diagnostic.has_value()) diagnostic->Deny(GuestDataLeaseDenialReason::LogicalSnapshotDenied);
	if (snapshot_preflight == GuestDataPreflight::Unavailable)
	{
		std::lock_guard<std::mutex> lock(registry.mutex);
		uint64_t cursor = address;
		while (permitted && cursor < end)
		{
			const Mapping* mapping = FindRawContainingMapping(registry, cursor);
			if (mapping != nullptr)
			{
				uint64_t visible_end = 0;
				if (!AddWithoutOverflow(mapping->visible_base, mapping->visible_size,
				                        &visible_end) || cursor < mapping->visible_base ||
				    cursor >= visible_end)
				{
					permitted = false;
					break;
				}
				const uint64_t page = (cursor - mapping->visible_base) / GUEST_PAGE_SIZE;
				if (page >= mapping->page_mode_num ||
				    (HostProtection(mapping->page_modes[page]) & permission) != permission)
				{
					permitted = false;
					break;
				}
				cursor = std::min(end, std::min(visible_end,
				    mapping->visible_base + (page + 1) * GUEST_PAGE_SIZE));
				continue;
			}
			if (FindContainingReservation(registry, cursor) != nullptr)
			{
				permitted = false;
				break;
			}
			// The native interval must stop before any managed mapping/reservation.
			// A single Mach region can contain both, so physical checks alone are
			// insufficient even when its endpoints are native addresses.
			uint64_t next = end;
			for (size_t index = 0; index < registry.mapping_num; ++index)
			{
				const auto begin = registry.mappings[index].raw_base;
				if (begin > cursor) next = std::min(next, begin);
			}
			for (size_t index = 0; index < registry.reservation_num; ++index)
			{
				const auto begin = registry.reservations[index].base;
				if (begin > cursor) next = std::min(next, begin);
			}
			cursor = next;
		}
	}
	if (!permitted && snapshot_preflight == GuestDataPreflight::Unavailable && diagnostic.has_value())
		diagnostic->Deny(GuestDataLeaseDenialReason::LogicalRegistryDenied);
	// The reader gate still pins logical rights and host mappings. Neither
	// preflight keeps the registry lock across Mach, so independent HLE readers
	// can validate concurrently. No registry pointer escapes either preflight.
	if (permitted)
	{
#if defined(LUMEN_VIRTUAL_RANGE_TEST_HOOK)
		if (g_guest_data_physical_validation_hook != nullptr)
			g_guest_data_physical_validation_hook();
#endif
		const vm_prot_t physical = VM_PROT_READ |
		    ((permission & PROT_WRITE) != 0 ? VM_PROT_WRITE : 0);
		permitted = PhysicalRangeHasProtection(address, end, physical, 0, diagnostic.has_value() ? &*diagnostic : nullptr);
	}
	if (permitted && !GuestCpu::PrepareCpuMemoryAccess(address, size,
	    static_cast<GuestCpu::MemoryAccessIntent>(static_cast<uint32_t>(required))))
	{
		permitted = false;
		if (diagnostic.has_value()) diagnostic->Deny(GuestDataLeaseDenialReason::DataHandoffDenied);
	}
	if (permitted)
	{
		lease->host_begin = address;
		lease->byte_count = size;
		lease->mutation_generation =
		    registry.mutation_generation.load(std::memory_order_relaxed);
		lease->acquiring_thread = reinterpret_cast<uintptr_t>(&g_vm_readers);
	}
	if (!permitted) EndGuestDataGate(registry);
	return permitted;
}

void ReleaseGuestDataLease(GuestDataLease* lease)
{
	if (lease == nullptr || !lease->IsActive()) return;
	lease->host_begin = 0;
	lease->byte_count = 0;
	lease->mutation_generation = 0;
	lease->acquiring_thread = 0;
	EndGuestDataGate(Registry());
}

bool ValidateGuestDataLeaseRangeContinuity(const GuestDataLease& lease,
    uint64_t address, uint64_t size, uint64_t original_generation)
{
	uint64_t end = 0;
	// A VM transaction may still abort or replace its owned mappings, so its
	// output borrows cannot certify deferred continuity. An adopted GPU epoch
	// owns no such transaction: it only pins existing mappings and admits
	// checked producer borrows. Accept that exact live token while retaining
	// every mapping watermark, identity, extent and acquiring-thread check.
	const bool external_owner = OwnsExternalMutationOwner(Registry());
	if (!lease.IsActive() || lease.HostBegin() != address || lease.ByteCount() != size ||
	    address == 0 || !ValidateMappingSize(size) || !AddWithoutOverflow(address, size, &end) ||
	    lease.acquiring_thread != reinterpret_cast<uintptr_t>(&g_vm_readers) || g_vm_readers == 0 ||
	    (g_vm_mutation_owner != nullptr && !external_owner) || original_generation == UINT64_MAX ||
	    original_generation > lease.MutationGeneration() || lease.MutationGeneration() == UINT64_MAX)
		return false;
	auto& registry = Registry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	if (registry.uncertain_continuity_generation > original_generation) return false;
	for (uint64_t cursor = address; cursor < end;)
	{
		const Mapping* mapping = FindContainingMapping(registry, cursor);
		uint64_t visible_end = 0;
		if (mapping == nullptr || mapping->identity == 0 || mapping->continuity_generation == 0 ||
		    mapping->continuity_generation > original_generation ||
		    !AddWithoutOverflow(mapping->visible_base, mapping->visible_size, &visible_end) || visible_end <= cursor)
			return false;
		cursor = std::min(end, visible_end);
	}
	return true;
}

bool CaptureGuestDataRangeSeal(const GuestDataLease& lease, GuestDataRangeSeal* seal)
{
	if (seal == nullptr || seal->IsActive() ||
	    !ValidateGuestDataLeaseRangeContinuity(lease, lease.HostBegin(), lease.ByteCount(), lease.MutationGeneration()))
		return false;
	seal->ticket = 1;
	seal->host_begin = lease.HostBegin();
	seal->byte_count = lease.ByteCount();
	seal->original_generation = lease.MutationGeneration();
	return true;
}

bool ValidateGuestDataRangeSeal(const GuestDataLease& lease, const GuestDataRangeSeal& seal)
{
	return seal.IsActive() && ValidateGuestDataLeaseRangeContinuity(
	    lease, seal.host_begin, seal.byte_count, seal.original_generation);
}

void ReleaseGuestDataRangeSeal(GuestDataRangeSeal* seal)
{
	if (seal == nullptr) return;
	seal->ticket = seal->host_begin = seal->byte_count = seal->original_generation = 0;
}

MutationTransaction::~MutationTransaction()
{
	(void)Abort();
}

void MutationTransaction::Commit()
{
	auto* transaction = static_cast<MutationTransactionState*>(state);
	if (transaction == nullptr) return;
	transaction->gate->RequireNoGuestDataBorrows();
	transaction->fex.Commit(transaction->range_base, transaction->range_size);
	state = nullptr;
	delete transaction;
}

bool MutationTransaction::Abort()
{
	auto* transaction = static_cast<MutationTransactionState*>(state);
	if (transaction == nullptr) return true;
	transaction->gate->RequireNoGuestDataBorrows();
	const bool result = AbortTransactionState(*transaction);
	state = nullptr;
	delete transaction;
	return result;
}

bool BeginReserveAligned(uint64_t address, uint64_t size, uint64_t alignment,
	                     ReservationHandle* handle,
	                     MutationTransaction* transaction,
	                     ReservationPlacement placement,
	                     AllocationDenialMetadata* denial)
{
	if (denial != nullptr) *denial = {};
	const auto fail = [denial](AllocationFailure reason, bool retryable = false) {
		if (denial != nullptr) *denial = {reason, retryable};
		return false;
	};
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	VMMutationTraceScope trace {"reserve", address, size, Mode::NoAccess,
	                           reinterpret_cast<uint64_t>(__builtin_return_address(0))};
#endif
	if (handle != nullptr) *handle = {};
	const uint64_t host_page_size = HostPageSize();
	uint64_t requested_end = 0;
	if (handle == nullptr || transaction == nullptr || transaction->IsActive() ||
	    !ValidateMappingSize(size) ||
	    !IsPowerOfTwo(alignment) || alignment < host_page_size ||
	    alignment % host_page_size != 0 || size % host_page_size != 0 ||
	    (placement != ReservationPlacement::Exact && placement != ReservationPlacement::PreferAddress) ||
	    (address != 0 && (address % alignment != 0 ||
	                      !AddWithoutOverflow(address, size, &requested_end))))
	{
		return fail(AllocationFailure::InvalidArgument);
	}

	if (ConsumeVirtualRangeTestFault(
	        VirtualRangeTestFault::FexAdditiveAdmission))
	{
		return fail(AllocationFailure::AdmissionDenied);
	}

	auto& registry = Registry();
	auto* pending = new (std::nothrow) MutationTransactionState(registry);
	if (pending == nullptr) return fail(AllocationFailure::HostAllocationFailed);
	if (!pending->IsAdmitted())
	{
		const bool fex_denied = !pending->fex.IsAdmitted();
		const bool retryable = fex_denied && pending->fex.IsRetryable();
		delete pending;
		return fail(fex_denied ? AllocationFailure::AdmissionDenied : AllocationFailure::HostAllocationFailed, retryable);
	}

	uint64_t allocation_address = address;
	{
		std::lock_guard<std::mutex> lock(registry.mutex);
		const bool preferred_address_overlaps =
		    address != 0 &&
		    (AnyMappingOverlap(registry, address, size) ||
		     AnyReservationOverlap(registry, address, size));
		if (ConsumeVirtualRangeTestFault(
		        VirtualRangeTestFault::CoreCapacity) ||
		    !EnsureReservationCapacity(registry) ||
		    registry.next_reservation_identity == 0 ||
		    registry.next_reservation_identity >=
		        (std::numeric_limits<uint64_t>::max)() / 2 ||
		    (preferred_address_overlaps &&
		     placement == ReservationPlacement::Exact))
		{
			delete pending;
			return fail(AllocationFailure::HostAllocationFailed);
		}
		if (preferred_address_overlaps)
		{
			allocation_address = 0;
		}
	}

	uint64_t base = AllocateReservationRaw(allocation_address, size, alignment);
	if (base == 0 && allocation_address != 0 &&
	    placement == ReservationPlacement::PreferAddress)
	{
		base = AllocateReservationRaw(0, size, alignment);
	}
	if (base == 0)
	{
		delete pending;
		return fail(AllocationFailure::HostAllocationFailed);
	}

	{
		std::lock_guard<std::mutex> lock(registry.mutex);
		if (AnyMappingOverlap(registry, base, size) ||
		    AnyReservationOverlap(registry, base, size))
		{
			(void)munmap(reinterpret_cast<void*>(static_cast<uintptr_t>(base)),
			             static_cast<size_t>(size));
			delete pending;
			return fail(AllocationFailure::HostAllocationFailed);
		}

		ImmutableSnapshotMutation snapshot_mutation(registry);
		const size_t insertion = LowerBoundReservation(registry, base);
		if (insertion < registry.reservation_num)
		{
			std::memmove(
			    &registry.reservations[insertion + 1],
			    &registry.reservations[insertion],
			    (registry.reservation_num - insertion) * sizeof(Reservation));
		}
		const Reservation reservation {
		    base, size, registry.next_reservation_identity++};
		registry.reservations[insertion] = reservation;
		registry.reservation_num++;
		RecordMutation(registry);
		(void)PublishVirtualRangeSnapshot(registry);

		pending->kind = TransactionKind::Reservation;
		pending->reservation = ReservationHandle {
		    reservation.base, reservation.size, reservation.identity};
		pending->range_base = reservation.base;
		pending->range_size = reservation.size;
		*handle = pending->reservation;
	}

	transaction->state = pending;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	trace.Observe(handle->identity, 0, size,
	              registry.mutation_generation.load(std::memory_order_relaxed));
	trace.Complete("begun", handle->base, 0,
	               registry.mutation_generation.load(std::memory_order_relaxed));
#endif
	return true;
}

ReservedFixedResult BeginAllocFixedInReservation(
	uint64_t address, uint64_t size, Mode mode,
	MutationTransaction* transaction)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	VMMutationTraceScope trace {"activate-reservation", address, size, mode,
	                           reinterpret_cast<uint64_t>(__builtin_return_address(0))};
#endif
	if (transaction == nullptr || transaction->IsActive() || !ValidateProtectionMode(mode))
	{
		return ReservedFixedResult::Failed;
	}

	auto& registry = Registry();
	{
		std::lock_guard<std::mutex> lock(registry.mutex);
		if (address == 0 || size == 0 ||
		    !AnyReservationOverlap(registry, address, size))
		{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
			trace.Complete("not-reserved", 0, 0,
			               registry.mutation_generation.load(std::memory_order_relaxed));
#endif
			return ReservedFixedResult::NotReserved;
		}
	}

	uint64_t requested_end = 0;
	const uint64_t host_page_size = HostPageSize();
	if (!ValidateMappingSize(size) ||
	    !AddWithoutOverflow(address, size, &requested_end) ||
	    address % host_page_size != 0 || size % host_page_size != 0)
	{
		return ReservedFixedResult::Failed;
	}

	const uint64_t page_count = size / GUEST_PAGE_SIZE;
	if (page_count == 0 ||
	    page_count > static_cast<uint64_t>(
	                     (std::numeric_limits<size_t>::max)() / sizeof(Mode)))
	{
		return ReservedFixedResult::Failed;
	}
	auto* page_modes = static_cast<Mode*>(
	    std::malloc(static_cast<size_t>(page_count) * sizeof(Mode)));
	if (page_modes == nullptr) return ReservedFixedResult::Failed;
	for (uint64_t page = 0; page < page_count; page++)
	{
		page_modes[static_cast<size_t>(page)] = mode;
	}
	if (ConsumeVirtualRangeTestFault(
	        VirtualRangeTestFault::FexAdditiveAdmission))
	{
		std::free(page_modes);
		return ReservedFixedResult::Failed;
	}

	auto* pending = new (std::nothrow) MutationTransactionState(registry);
	if (pending == nullptr)
	{
		std::free(page_modes);
		return ReservedFixedResult::Failed;
	}
	if (!pending->IsAdmitted())
	{
		delete pending;
		std::free(page_modes);
		return ReservedFixedResult::Failed;
	}

	{
		std::lock_guard<std::mutex> lock(registry.mutex);
		Reservation* reservation = FindContainingReservation(registry, address);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		trace.Observe(0, reservation == nullptr ? 0 : reservation->identity, size,
		              registry.mutation_generation.load(std::memory_order_relaxed));
#endif
		uint64_t reservation_end = 0;
		if (reservation == nullptr ||
		    !AddWithoutOverflow(reservation->base, reservation->size,
		                        &reservation_end) ||
		    requested_end > reservation_end ||
		    AnyMappingOverlap(registry, address, size) ||
		    !EnsureRegistryCapacity(registry) || registry.next_identity == 0 ||
		    registry.next_identity >
		        (std::numeric_limits<uint64_t>::max)() / 2)
		{
			delete pending;
			std::free(page_modes);
			return ReservedFixedResult::Failed;
		}

		ImmutableSnapshotMutation snapshot_mutation(registry);
		if (ConsumeVirtualRangeTestFault(
		        VirtualRangeTestFault::ActivateHost) ||
		    !ReplaceReservationRange(address, size, mode))
		{
			delete pending;
			std::free(page_modes);
			return ReservedFixedResult::Failed;
		}

		const size_t insertion = LowerBoundMapping(registry, address);
		if (insertion < registry.mapping_num)
		{
			std::memmove(&registry.mappings[insertion + 1],
			             &registry.mappings[insertion],
			             (registry.mapping_num - insertion) * sizeof(Mapping));
		}
		Mapping mapping {};
		mapping.raw_base = address;
		mapping.raw_size = size;
		mapping.visible_base = address;
		mapping.visible_size = size;
		mapping.page_modes = page_modes;
		mapping.page_mode_num = page_count;
		mapping.identity = registry.next_identity++;
		mapping.reservation_identity = reservation->identity;
		registry.mappings[insertion] = mapping;
		registry.mapping_num++;
		RecordMutation(registry);
		MarkMappingContinuity(registry, registry.mappings[insertion]);
		(void)PublishVirtualRangeSnapshot(registry);

		pending->kind = TransactionKind::ReservedMapping;
		pending->mapping_identity = mapping.identity;
		pending->range_base = address;
		pending->range_size = size;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		trace.Observe(mapping.identity, mapping.reservation_identity, size,
		              registry.mutation_generation.load(std::memory_order_relaxed));
#endif
	}

	transaction->state = pending;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	trace.Complete("begun", address, static_cast<uint32_t>(mode),
	               registry.mutation_generation.load(std::memory_order_relaxed));
#endif
	return ReservedFixedResult::Begun;
}

bool HasReservationOverlap(uint64_t address, uint64_t size)
{
	auto& registry = Registry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	return AnyReservationOverlap(registry, address, size);
}

bool ReleaseReservation(const ReservationHandle& handle)
{
	if (!handle.IsValid()) return false;
	if (ConsumeVirtualRangeTestFault(
	        VirtualRangeTestFault::FexExclusiveAdmission))
	{
		return false;
	}
	auto& registry = Registry();
	GatedFexGuestMemoryMutation mutation {
	    registry, FexGuestMemoryMutationKind::Exclusive};
	if (!mutation.IsAdmitted()) return false;
	std::lock_guard<std::mutex> lock(registry.mutex);
	const size_t reservation_index = FindReservationHandle(registry, handle);
	if (reservation_index >= registry.reservation_num) return false;

	ImmutableSnapshotMutation snapshot_mutation(registry);
	if (ConsumeVirtualRangeTestFault(VirtualRangeTestFault::ReleaseHost) ||
	    munmap(reinterpret_cast<void*>(static_cast<uintptr_t>(handle.base)),
	           static_cast<size_t>(handle.size)) != 0)
	{
		MarkRangeContinuity(registry, handle.base, handle.size, true);
		return false;
	}
	for (size_t index = registry.mapping_num; index != 0; index--)
	{
		if (registry.mappings[index - 1].reservation_identity == handle.identity)
		{
			EraseMapping(registry, index - 1);
		}
	}
	EraseReservation(registry, reservation_index);
	RecordMutation(registry);
	(void)PublishVirtualRangeSnapshot(registry);
	mutation.Commit(handle.base, handle.size);
	return true;
}

ManagedRegionState QueryManagedRegion(uint64_t address, ManagedRegion* region)
{
	if (region != nullptr)
	{
		*region = ManagedRegion {0, 0, Mode::NoAccess};
	}

	auto& registry = Registry();
	if (ImmutableSnapshot::Requested())
	{
		ManagedRegionState snapshot_state = ManagedRegionState::Unmanaged;
		if (registry.immutable_snapshot.Lookup(address, &snapshot_state, region,
		                                       true))
		{
			return snapshot_state;
		}
	}
	if (address == 0)
	{
		return ManagedRegionState::Unmanaged;
	}

	std::lock_guard<std::mutex> lock(registry.mutex);
	const Mapping*              mapping = FindRawContainingMapping(registry, address);
	if (mapping == nullptr)
	{
		const Reservation* reservation =
		    FindContainingReservation(registry, address);
		if (reservation == nullptr) return ManagedRegionState::Unmanaged;

		uint64_t hole_begin = reservation->base;
		uint64_t hole_end = 0;
		if (!AddWithoutOverflow(reservation->base, reservation->size,
		                        &hole_end))
		{
			return ManagedRegionState::Unmanaged;
		}
		for (size_t index = 0; index < registry.mapping_num; index++)
		{
			const Mapping& child = registry.mappings[index];
			if (child.reservation_identity != reservation->identity) continue;
			uint64_t child_end = 0;
			if (!AddWithoutOverflow(child.visible_base, child.visible_size,
			                        &child_end))
			{
				return ManagedRegionState::Unmanaged;
			}
			if (child_end <= address)
			{
				hole_begin = child_end;
				continue;
			}
			if (child.visible_base > address) hole_end = child.visible_base;
			break;
		}
		if (region != nullptr)
		{
			*region = ManagedRegion {hole_begin, hole_end, Mode::NoAccess};
		}
		return ManagedRegionState::Reserved;
	}

	uint64_t visible_end = 0;
	uint64_t raw_end     = 0;
	if (!AddWithoutOverflow(mapping->visible_base, mapping->visible_size, &visible_end) ||
	    !AddWithoutOverflow(mapping->raw_base, mapping->raw_size, &raw_end))
	{
		return ManagedRegionState::Unmanaged;
	}

	if (address < mapping->visible_base || address >= visible_end)
	{
		if (region != nullptr)
		{
			*region = address < mapping->visible_base ? ManagedRegion {mapping->raw_base, mapping->visible_base, Mode::NoAccess}
			                                         : ManagedRegion {visible_end, raw_end, Mode::NoAccess};
		}
		return ManagedRegionState::Reserved;
	}

	const uint64_t page = (address - mapping->visible_base) / GUEST_PAGE_SIZE;
	if (page >= mapping->page_mode_num)
	{
		return ManagedRegionState::Unmanaged;
	}
	const Mode mode = mapping->page_modes[static_cast<size_t>(page)];
	uint64_t   first_page = page;
	while (first_page != 0 && mapping->page_modes[static_cast<size_t>(first_page - 1)] == mode)
	{
		first_page--;
	}
	uint64_t last_page = page + 1;
	while (last_page < mapping->page_mode_num && mapping->page_modes[static_cast<size_t>(last_page)] == mode)
	{
		last_page++;
	}

	if (region != nullptr)
	{
		const uint64_t run_begin   = mapping->visible_base + first_page * GUEST_PAGE_SIZE;
		const uint64_t raw_run_end = mapping->visible_base + last_page * GUEST_PAGE_SIZE;
		*region = ManagedRegion {run_begin, raw_run_end < visible_end ? raw_run_end : visible_end, mode};
	}
	return ManagedRegionState::Visible;
}

bool QueryGuestBackingSpan(uint64_t address, uint64_t maximum_bytes, GuestBackingSpan* span) noexcept
{
	if (span != nullptr) *span = {};
	if (span == nullptr || address == 0 || maximum_bytes == 0 || address > UINT64_MAX - maximum_bytes) return false;
	if (const auto query = g_backing_owner_query.load(std::memory_order_acquire);
	    query != nullptr && query(address, maximum_bytes, span)) return span->identity != 0 && span->size != 0;
	auto& registry = Registry();
	std::lock_guard lock(registry.mutex);
	const auto* mapping = FindRawContainingMapping(registry, address);
	if (mapping == nullptr || mapping->identity == 0 || address < mapping->visible_base ||
	    address - mapping->visible_base >= mapping->visible_size) return false;
	const auto offset = address - mapping->visible_base;
	*span = {mapping->identity, offset, address, std::min(maximum_bytes, mapping->visible_size - offset),
	    registry.mutation_generation.load(std::memory_order_acquire)};
	return true;
}

uint64_t CreateGuestBackingHandoffOwner() noexcept
{
	return GuestCpu::CreateMemoryHandoffOwner();
}

bool ReleaseGuestBackingHandoffOwner(uint64_t owner) noexcept
{
	return GuestCpu::ReleaseMemoryHandoffOwner(owner);
}

bool MarkGuestBackingHandoffRangeOwned(uint64_t owner, uint64_t address, uint64_t bytes, uint32_t watch_intents) noexcept
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	address &= UINT64_C(0x00ffffffffffffff);
#endif
	if (owner == 0 || address == 0 || bytes == 0 || address > UINT64_MAX - bytes ||
	    watch_intents < 1u || watch_intents > 3u) return false;
	if ((g_vm_external_native_token != 0 || GuestCpu::CurrentMemoryHandoffNativeClaimOwner() != 0) &&
	    !CurrentNativeClaimCoversGuestRange(address, bytes)) return false;
	const auto owner_query = g_backing_owner_query.load(std::memory_order_acquire);
	const auto owner_mark = owner_query != nullptr ? g_backing_owner_mark_owned.load(std::memory_order_acquire) : nullptr;
	if (owner_mark != nullptr && owner_mark(owner, address, bytes, watch_intents)) return true;
	const auto end = address + bytes;
	while (address < end)
	{
		GuestBackingSpan span;
		if (!QueryGuestBackingSpan(address, end - address, &span) || span.identity == 0 ||
		    span.address != address || span.size == 0 || span.size > end - address ||
		    span.offset > UINT64_MAX - span.size) return false;
		GuestBackingSpan physical;
		if (owner_query != nullptr && owner_query(address, span.size, &physical))
		{
			// A physical owner must enumerate all aliases and remember future
			// remaps. Never silently turn an unavailable owned protocol into a
			// private-VA guard or a legacy sticky enrollment.
			if (owner_mark == nullptr || !owner_mark(owner, address, span.size, watch_intents)) return false;
		} else
		{
			// Core-private allocations have one accepted VA per identity. A
			// future allocation has a different identity even at the same VA.
			const GuestCpu::PhysicalMemoryRange range {span.identity, span.offset, span.size};
			if (!GuestCpu::EnrollMemoryHandoffPhysicalRange(owner, range,
			        static_cast<GuestCpu::MemoryAccessIntent>(watch_intents)) ||
			    !GuestCpu::MapMemoryHandoffOwnerPhysicalAlias(owner, range, span.address)) return false;
		}
		address += span.size;
	}
	return true;
}

bool CopyGuestBackingDataForGpu(uint64_t address, void* buffer, uint64_t bytes, bool write) noexcept
{
	if (address == 0 || buffer == nullptr || bytes == 0 || bytes > SIZE_MAX || address > UINT64_MAX - bytes) return false;
	if ((g_vm_external_native_token != 0 || GuestCpu::CurrentMemoryHandoffNativeClaimOwner() != 0) &&
	    !CurrentNativeClaimCoversGuestRange(address, bytes)) return false;
	// Registration publishes the query last; never consume a partial owner.
	if (g_backing_owner_query.load(std::memory_order_acquire) != nullptr)
	{
		const auto copy = g_backing_owner_gpu_copy.load(std::memory_order_acquire);
		if (copy != nullptr && copy(address, buffer, bytes, write)) return true;
	}
	GuestCpu::MemoryHandoffBypass producer;
	GuestDataLease lease;
	if (!TryAcquireGuestDataLease(address, bytes, write ? Mode::Write : Mode::Read, &lease, false)) return false;
	auto* guest = reinterpret_cast<void*>(static_cast<uintptr_t>(lease.HostBegin()));
	if (write) std::memcpy(guest, buffer, static_cast<size_t>(bytes));
	else std::memcpy(buffer, guest, static_cast<size_t>(bytes));
	return true;
}

GuestBackingPresence QueryGuestBackingPresence(uint64_t identity, uint64_t offset, uint64_t bytes) noexcept
{
	if (identity == 0 || bytes == 0 || offset > UINT64_MAX - bytes) return GuestBackingPresence::Unknown;
	if (g_backing_owner_query.load(std::memory_order_acquire) != nullptr)
	{
		const auto presence = g_backing_owner_presence.load(std::memory_order_acquire);
		if (presence == nullptr) return GuestBackingPresence::Unknown;
		const auto result = presence(identity, offset, bytes);
		if (result != GuestBackingPresence::Unknown) return result;
	}
	auto& registry = Registry();
	std::lock_guard lock(registry.mutex);
	if (identity >= registry.next_identity) return GuestBackingPresence::Unknown;
	for (size_t index = 0; index != registry.mapping_num; ++index)
	{
		const auto& mapping = registry.mappings[index];
		if (mapping.identity == identity && offset < mapping.visible_size) return GuestBackingPresence::Live;
	}
	return GuestBackingPresence::Retired;
}

bool MarkGuestBackingHandoffRange(uint64_t address, uint64_t bytes, uint32_t watch_intents) noexcept
{
	if (address == 0 || bytes == 0 || address > UINT64_MAX - bytes || watch_intents < 1u || watch_intents > 3u) return false;
	if ((g_vm_external_native_token != 0 || GuestCpu::CurrentMemoryHandoffNativeClaimOwner() != 0) &&
	    !CurrentNativeClaimCoversGuestRange(address, bytes)) return false;
	const auto mark = g_backing_owner_mark.load(std::memory_order_acquire);
	if (mark != nullptr && mark(address, bytes, watch_intents)) return true;
	const auto end = address + bytes;
	while (address < end)
	{
		GuestBackingSpan span;
		if (!QueryGuestBackingSpan(address, end - address, &span) || span.size == 0) return false;
		if (mark == nullptr || !mark(span.address, span.size, watch_intents))
			GuestCpu::MarkHandoffRange(span.address, span.size, static_cast<GuestCpu::MemoryAccessIntent>(watch_intents));
		address += span.size;
	}
	return true;
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
bool TryQueryManagedRegionSnapshot(uint64_t address,
	                               ManagedRegionState* state,
	                               ManagedRegion* region)
{
	if (state != nullptr) *state = ManagedRegionState::Unmanaged;
	if (region != nullptr)
	{
		*region = ManagedRegion {0, 0, Mode::NoAccess};
	}
	if (!ImmutableSnapshot::Requested()) return false;
	ManagedRegionState snapshot_state = ManagedRegionState::Unmanaged;
	ManagedRegion      snapshot_region {0, 0, Mode::NoAccess};
	if (!Registry().immutable_snapshot.Lookup(
	        address, &snapshot_state, &snapshot_region, false))
	{
		return false;
	}
	if (state != nullptr) *state = snapshot_state;
	if (region != nullptr) *region = snapshot_region;
	return true;
}
#endif

bool IsManagedAddress(uint64_t address)
{
	if (address == 0) return false;

	auto& registry = Registry();
	const bool immutable_snapshot_requested = ImmutableSnapshot::Requested();
	if (immutable_snapshot_requested)
	{
		ManagedRegionState snapshot_state = ManagedRegionState::Unmanaged;
		if (registry.immutable_snapshot.Lookup(address, &snapshot_state, nullptr,
		                                       false))
		{
			return snapshot_state == ManagedRegionState::Visible;
		}
	}

	auto& cache      = g_managed_address_cache;
	const auto match = [address](const ManagedAddressRange& range)
	{
		return address >= range.begin && address < range.end;
	};

	uint64_t generation = 0;
	if (!immutable_snapshot_requested)
	{
		generation = registry.mutation_generation.load(std::memory_order_acquire);
		SynchronizeManagedAddressCache(cache, generation);
		if (match(cache.ranges[cache.mru])) return true;
		for (uint32_t index = 0; index < MANAGED_ADDRESS_CACHE_SIZE; index++)
		{
			if (index != cache.mru && match(cache.ranges[index]))
			{
				cache.mru = index;
				return true;
			}
		}
	}

	// A failed snapshot lookup means either publication is unavailable or a VM
	// mutation is active. In the latter case the generation is not advanced until
	// after the physical mutation, so a pre-lock TLS cache hit could be stale.
	// Bypass that cache whenever the experiment is requested and serialize with
	// the authoritative registry instead.
	std::lock_guard<std::mutex> lock(registry.mutex);
	generation = registry.mutation_generation.load(std::memory_order_relaxed);
	SynchronizeManagedAddressCache(cache, generation);
	const Mapping* mapping = FindContainingMapping(registry, address);
	if (mapping == nullptr) return false;

	uint64_t end = 0;
	if (!AddWithoutOverflow(mapping->visible_base, mapping->visible_size, &end)) return false;
	const uint32_t insert = cache.next;
	cache.ranges[insert]  = {mapping->visible_base, end};
	cache.mru             = insert;
	cache.next            = (insert + 1) % MANAGED_ADDRESS_CACHE_SIZE;
	return true;
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
// Optional bounded observation of the virtual-memory lifecycle, used to
// diagnose a guest execution denied on a range whose recorded mode is
// writable. Numeric only: the caller address is resolved offline from the
// unstripped
// runtime, so no title, module or path identity is captured here.
void TraceVMMutationV1(const char* operation, uint64_t requested_address,
                       uint64_t size, Mode mode, uint64_t result_address,
                       uint32_t result_mode, uint64_t caller,
                       const char* phase, uint64_t identity,
                       uint64_t parent_identity, uint64_t generation_gauge) noexcept
{
	static const bool enabled = [] {
		const char* text = std::getenv("LUMEN_MACOS_VM_MUTATION_TRACE");
		return text != nullptr && text[0] == '1' && text[1] == '\0';
	}();
	if (!enabled) return;
	static std::atomic<uint32_t> records {0};
	const uint32_t index = records.fetch_add(1, std::memory_order_relaxed);
	if (index >= 16384) return;
	std::array<char, 640> record {};
	const int length = std::snprintf(record.data(), record.size(),
	             "[VM_MUTATION_TRACE] index=%u operation=%s requested=0x%016" PRIx64
	             " size=0x%016" PRIx64 " mode=%u result=0x%016" PRIx64
	             " resultMode=%u caller=0x%016" PRIx64 " phase=%s identity=%" PRIu64
	             " parent_identity=%" PRIu64 " generation_gauge=%" PRIu64
	             " thread=%d handle=0x%016" PRIx64 " hle=0x%016" PRIx64 "\n",
	             index, operation, requested_address, size,
	             static_cast<unsigned>(mode), result_address, result_mode, caller,
	             phase, identity, parent_identity, generation_gauge,
	             Thread::GetThreadIdUnique(), g_capture_thread_handle, g_capture_host_call);
	if (length > 0 && static_cast<size_t>(length) < record.size())
	{
		(void)write(STDERR_FILENO, record.data(), static_cast<size_t>(length));
	}
}
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
// Optional bounded region snapshots used to compare a small managed region
// between its creation and a later guest fault or kernel exception. The
// selector is latched once per process, the recorded count and the per-region
// size are bounded, and the files live next to the other local run outputs.
namespace
{
constexpr size_t   kRecordedRegionLimit = 64;
constexpr uint64_t kRecordedRegionMaximumSize = 4 * 1024 * 1024;

struct RecordedRegion
{
	uint64_t base;
	uint64_t size;
};

std::array<RecordedRegion, kRecordedRegionLimit> g_recorded_regions {};
std::atomic<size_t> g_recorded_region_count {0};

bool RegionSnapshotsEnabled()
{
	static const bool enabled = [] {
		const char* text = std::getenv("LUMEN_MACOS_VM_REGION_SNAPSHOT");
		return text != nullptr && text[0] == '1' && text[1] == '\0';
	}();
	return enabled;
}

void WriteRegionSnapshot(const char* tag, uint64_t base, uint64_t size)
{
	std::array<char, 64> dump_path {};
	std::snprintf(dump_path.data(), dump_path.size(),
	              "_vmregion_%s_%016" PRIx64 ".bin", tag, base);
	if (FILE* file = std::fopen(dump_path.data(), "wb"))
	{
		std::fwrite(reinterpret_cast<const void*>(static_cast<uintptr_t>(base)), 1,
		            static_cast<size_t>(size), file);
		std::fclose(file);
	}
}

// --- Optional managed-region change ledger (local diagnostic) ----------------
// The ledger keeps one bounded in-memory copy of every recorded small managed
// region and reports, per trigger, the numeric byte spans that changed since
// the previous observation plus every eight-byte word that falls inside an
// operator-supplied address range (selector LUMEN_MACOS_VM_LEDGER_REFERENCE).
// It replaces "keep a full dump per point" with a timeline, so the window
// between two title collections can be dated without unbounded files. All
// output is numeric, bounded in records and bytes, and never touches guest
// memory state.
constexpr uint64_t kLedgerRegionMaximumSize = 256 * 1024;
constexpr size_t   kLedgerMaximumRecords    = 4096;
constexpr size_t   kLedgerMaximumSpans      = 8;
constexpr size_t   kLedgerMaximumReferences = 32;

std::array<std::vector<uint8_t>, kRecordedRegionLimit> g_ledger_cache {};
std::atomic<uint32_t> g_ledger_records {0};
std::atomic<uint64_t> g_ledger_last_tick_ns {0};

// One attribution snapshot per record: the guest thread that produced it and
// its most recent HLE import. Resolved once per trigger.
struct RegionLedgerThread
{
	int      thread = 0;
	uint64_t handle = 0;
	uint64_t hostCall = 0;
};

RegionLedgerThread CurrentRegionLedgerThread()
{
	return RegionLedgerThread {Thread::GetThreadIdUnique(), g_capture_thread_handle,
	                           g_capture_host_call};
}

bool RegionLedgerEnabled()
{
	static const bool enabled = [] {
		const char* text = std::getenv("LUMEN_MACOS_VM_LEDGER");
		return text != nullptr && text[0] == '1' && text[1] == '\0';
	}();
	return enabled;
}

uint64_t RegionLedgerIntervalNs()
{
	static const uint64_t interval_ns = [] {
		const char* text = std::getenv("LUMEN_MACOS_VM_LEDGER_INTERVAL_MS");
		long parsed = 20;
		if (text != nullptr)
		{
			char* end = nullptr;
			const long value = std::strtol(text, &end, 10);
			if (end != text && *end == '\0' && value >= 0 && value <= 10000) parsed = value;
		}
		return static_cast<uint64_t>(parsed) * 1000ULL * 1000ULL;
	}();
	return interval_ns;
}

// Reference scan request: either an absolute range "0x<begin>-0x<end>" or a
// size-relative range "size:0x<size>+0x<offset>:0x<length>", which watches the
// same sub-range in every recorded region of that size and therefore survives
// address-space layout changes between replays.
struct RegionLedgerReference
{
	bool     configured = false;
	bool     by_size    = false;
	uint64_t size       = 0;
	uint64_t offset     = 0;
	uint64_t length     = 0;
	uint64_t begin      = 0;
	uint64_t end        = 0;
};

uint64_t LedgerParseHex(const char*& cursor)
{
	if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) cursor += 2;
	uint64_t value = 0;
	for (; *cursor != '\0'; ++cursor)
	{
		const char c = *cursor;
		const int digit = (c >= '0' && c <= '9') ? c - '0' :
		                  (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
		                  (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
		if (digit < 0) break;
		value = (value << 4) | static_cast<uint64_t>(digit);
	}
	return value;
}

const RegionLedgerReference& RegionLedgerReferenceConfig()
{
	static const RegionLedgerReference config = [] {
		RegionLedgerReference parsed {};
		const char* const text = std::getenv("LUMEN_MACOS_VM_LEDGER_REFERENCE");
		if (text == nullptr || text[0] == '\0') return parsed;
		const char* cursor = text;
		if (std::strncmp(cursor, "size:", 5) == 0)
		{
			cursor += 5;
			parsed.size = LedgerParseHex(cursor);
			if (*cursor != '+') return RegionLedgerReference {};
			++cursor;
			parsed.offset = LedgerParseHex(cursor);
			if (*cursor == ':') parsed.length = LedgerParseHex(++cursor);
			if (*cursor != '\0' || parsed.size == 0) return RegionLedgerReference {};
			if (parsed.length == 0 || parsed.length > parsed.size ||
			    parsed.offset >= parsed.size || parsed.offset > parsed.size - parsed.length)
			{
				return RegionLedgerReference {};
			}
			parsed.by_size    = true;
			parsed.configured = true;
			return parsed;
		}
		parsed.begin = LedgerParseHex(cursor);
		if (*cursor != '-' && *cursor != ':') return RegionLedgerReference {};
		++cursor;
		parsed.end = LedgerParseHex(cursor);
		if (*cursor != '\0' || parsed.begin == 0 || parsed.end <= parsed.begin)
		{
			return RegionLedgerReference {};
		}
		parsed.configured = true;
		return parsed;
	}();
	return config;
}

uint64_t LedgerClockNs()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

bool LedgerRecordBudgetAvailable()
{
	return g_ledger_records.fetch_add(1, std::memory_order_relaxed) < kLedgerMaximumRecords;
}

// The recorded regions are small managed mappings. Query the immutable region
// snapshot instead of trusting the registry lock, and skip a region that is no
// longer visible or readable instead of dereferencing it.
bool LedgerRegionReadable(uint64_t base, uint64_t size)
{
	if (size == 0 || base > UINT64_MAX - size) return false;
	ManagedRegionState state = ManagedRegionState::Unmanaged;
	ManagedRegion      region {};
	if (!TryQueryManagedRegionSnapshot(base, &state, &region) ||
	    state != ManagedRegionState::Visible ||
	    (static_cast<uint32_t>(region.mode) & static_cast<uint32_t>(Mode::Read)) == 0)
	{
		return false;
	}
	if (!TryQueryManagedRegionSnapshot(base + size - 1, &state, &region) ||
	    state != ManagedRegionState::Visible)
	{
		return false;
	}
	return true;
}
} // namespace

void RecordRegionSnapshot(uint64_t base, uint64_t size)
{
	if (!RegionSnapshotsEnabled() || base == 0 || size == 0 ||
	    size > kRecordedRegionMaximumSize)
	{
		return;
	}
	const size_t index = g_recorded_region_count.fetch_add(1, std::memory_order_relaxed);
	if (index >= g_recorded_regions.size()) return;
	g_recorded_regions[index] = {base, size};
	WriteRegionSnapshot("map", base, size);
}

void TickRecordedRegionLedger(const char* tag, bool force)
{
	if (!RegionLedgerEnabled() || tag == nullptr || tag[0] == '\0') return;
	const RegionLedgerThread thread = CurrentRegionLedgerThread();
	const uint64_t now_ns = LedgerClockNs();
	if (!force)
	{
		const uint64_t interval_ns = RegionLedgerIntervalNs();
		const uint64_t last_ns = g_ledger_last_tick_ns.load(std::memory_order_relaxed);
		if (last_ns != 0 && now_ns - last_ns < interval_ns) return;
	}
	g_ledger_last_tick_ns.store(now_ns, std::memory_order_relaxed);

	const RegionLedgerReference& reference = RegionLedgerReferenceConfig();

	const size_t count = g_recorded_region_count.load(std::memory_order_relaxed);
	const size_t bounded = count < g_recorded_regions.size() ? count : g_recorded_regions.size();
	for (size_t index = 0; index < bounded; ++index)
	{
		const RecordedRegion& region = g_recorded_regions[index];
		if (region.size == 0 || region.size > kLedgerRegionMaximumSize) continue;
		if (!LedgerRegionReadable(region.base, region.size)) continue;
		const auto* live = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(region.base));
		std::vector<uint8_t>& cache = g_ledger_cache[index];
		if (cache.size() != region.size)
		{
			cache.assign(live, live + region.size);
			if (LedgerRecordBudgetAvailable())
			{
				std::fprintf(stderr,
				             "[VM_LEDGER] ns=%" PRIu64 " tag=%s state=initial index=%zu base=0x%016" PRIx64
				             " size=0x%016" PRIx64 " thread=%d handle=0x%016" PRIx64 " hle=0x%016" PRIx64 "\n",
				             now_ns, tag, index, region.base, region.size, thread.thread,
				             thread.handle, thread.hostCall);
			}
			continue;
		}

		const uint64_t watch_begin = reference.configured
		    ? (reference.by_size ? region.base + reference.offset : reference.begin)
		    : 0;
		const uint64_t watch_end = reference.configured
		    ? (reference.by_size ? watch_begin + reference.length : reference.end)
		    : 0;
		size_t changed = 0;
		size_t span_count = 0;
		std::array<uint64_t, kLedgerMaximumSpans> span_offset {};
		std::array<uint64_t, kLedgerMaximumSpans> span_bytes {};
		// Two span lists: the first spans in offset order (the general view of
		// the region) and every span that overlaps the watched window, which is
		// reported even when it sits past the general budget.
		std::array<uint64_t, 4> window_offset {};
		std::array<uint64_t, 4> window_bytes {};
		size_t window_span_count = 0;
		const auto flush_span = [&](size_t start, size_t bytes)
		{
			if (bytes == 0) return;
			if (span_count < kLedgerMaximumSpans)
			{
				span_offset[span_count] = start;
				span_bytes[span_count]  = bytes;
				++span_count;
			}
			if (reference.configured && window_span_count < window_offset.size())
			{
				const uint64_t span_begin = region.base + start;
				const uint64_t span_end   = span_begin + bytes;
				if (span_begin < watch_end && watch_begin < span_end)
				{
					window_offset[window_span_count] = start;
					window_bytes[window_span_count]  = bytes;
					++window_span_count;
				}
			}
		};
		if (std::memcmp(cache.data(), live, cache.size()) != 0)
		{
			// The common case is an unchanged region: one memcmp decides it and
			// the byte-accurate span scan runs only when something moved.
			size_t current_start = 0;
			size_t current_bytes = 0;
			for (size_t offset = 0; offset < cache.size(); ++offset)
			{
				if (cache[offset] == live[offset])
				{
					if (current_bytes != 0)
					{
						flush_span(current_start, current_bytes);
						current_bytes = 0;
					}
					continue;
				}
				++changed;
				if (current_bytes == 0) current_start = offset;
				++current_bytes;
			}
			if (current_bytes != 0) flush_span(current_start, current_bytes);
		}
		if (changed != 0)
		{
			if (LedgerRecordBudgetAvailable())
			{
				std::fprintf(stderr,
				             "[VM_LEDGER] ns=%" PRIu64 " tag=%s state=changed index=%zu base=0x%016" PRIx64
				             " size=0x%016" PRIx64 " bytes=%zu spans=%zu thread=%d handle=0x%016" PRIx64
				             " hle=0x%016" PRIx64 "\n",
				             now_ns, tag, index, region.base, region.size, changed, span_count,
				             thread.thread, thread.handle, thread.hostCall);
				for (size_t span = 0; span < span_count; ++span)
				{
					std::fprintf(stderr,
					             "[VM_LEDGER_SPAN] ns=%" PRIu64 " tag=%s index=%zu offset=0x%08" PRIx64
					             " bytes=%" PRIu64 " window=0 thread=%d handle=0x%016" PRIx64
					             " hle=0x%016" PRIx64 "\n",
					             now_ns, tag, index, span_offset[span], span_bytes[span], thread.thread,
					             thread.handle, thread.hostCall);
				}
				for (size_t span = 0; span < window_span_count; ++span)
				{
					std::fprintf(stderr,
					             "[VM_LEDGER_SPAN] ns=%" PRIu64 " tag=%s index=%zu offset=0x%08" PRIx64
					             " bytes=%" PRIu64 " window=1 thread=%d handle=0x%016" PRIx64
					             " hle=0x%016" PRIx64 "\n",
					             now_ns, tag, index, window_offset[span], window_bytes[span], thread.thread,
					             thread.handle, thread.hostCall);
				}
			}
			std::memcpy(cache.data(), live, cache.size());
		}
		if (reference.configured && LedgerRecordBudgetAvailable())
		{
			if (!reference.by_size || region.size == reference.size)
			{
				size_t reported = 0;
				for (size_t offset = 0;
				     offset + sizeof(uint64_t) <= cache.size() && reported < kLedgerMaximumReferences;
				     offset += sizeof(uint64_t))
				{
					uint64_t word = 0;
					std::memcpy(&word, live + offset, sizeof(word));
					if (word < watch_begin || word >= watch_end) continue;
					++reported;
					std::fprintf(stderr,
					             "[VM_LEDGER_REF] ns=%" PRIu64 " tag=%s index=%zu base=0x%016" PRIx64
					             " offset=0x%08zx value=0x%016" PRIx64 " thread=%d handle=0x%016" PRIx64
					             " hle=0x%016" PRIx64 "\n",
					             now_ns, tag, index, region.base, offset, word, thread.thread,
					             thread.handle, thread.hostCall);
				}
			}
		}
	}
}

bool SnapshotRecordedRegions(const char* tag)
{
	// The ledger has its own selector and runs before the file snapshots so a
	// trigger records the changed spans even when the dumps themselves are off.
	TickRecordedRegionLedger(tag, true);
	if (!RegionSnapshotsEnabled() || tag == nullptr || tag[0] == '\0') return false;
	const size_t count = g_recorded_region_count.load(std::memory_order_relaxed);
	const size_t bounded = count < g_recorded_regions.size() ? count : g_recorded_regions.size();
	for (size_t index = 0; index < bounded; ++index)
	{
		const RecordedRegion& region = g_recorded_regions[index];
		if (region.size == 0) continue;
		WriteRegionSnapshot(tag, region.base, region.size);
	}
	return bounded != 0;
}

// --- Optional guest-thread root snapshot (local diagnostic) ------------------
// Reports one guest thread's published boundary state at explicit triggers (the
// deliver/handled points of the exception delivery) so the stop-the-world
// capture question can be answered offline: the fifteen GP words the title's
// collector scans, in the delivered-ucontext order, plus RSP and RIP, and every
// eight-byte word of the stack window [RSP, RSP+1MiB) that falls inside the
// watched reference window. The window is clamped to the private x86-64 stack
// allocation that owns RSP and read through Mach, so no host allocation outside
// a guest stack is ever dereferenced; a size-relative reference is resolved
// against the recorded regions of that size. Selector
// LUMEN_MACOS_VM_ROOT_SNAPSHOT, process-latched and off by default; records are
// numeric and bounded and no guest state is written.
namespace
{
constexpr uint64_t kRootsStackWindowSize = 1024 * 1024;
constexpr size_t   kRootsMaximumRecords = 4096;
constexpr size_t   kRootsMaximumReferencesPerThread = 16;
constexpr size_t   kRootsMaximumReferences = 256;
constexpr size_t   kRootsReadChunkSize = 16 * 1024;
constexpr size_t   kRootsMaximumWatchWindows = 8;

std::atomic<uint32_t> g_roots_records {0};
std::atomic<uint32_t> g_roots_references {0};

struct RootsWatchWindow
{
	uint64_t begin;
	uint64_t end;
};

// Resolve the operator reference into absolute watch windows. A size-relative
// reference names the same subrange inside every recorded region of that size,
// so it stays valid when the address-space layout changes between replays.
size_t ResolveRootsWatchWindows(const RegionLedgerReference& reference,
                                std::array<RootsWatchWindow, kRootsMaximumWatchWindows>* windows)
{
	if (!reference.configured || windows == nullptr) return 0;
	if (!reference.by_size)
	{
		(*windows)[0] = {reference.begin, reference.end};
		return 1;
	}
	size_t count = 0;
	const size_t recorded = g_recorded_region_count.load(std::memory_order_relaxed);
	const size_t bounded = recorded < g_recorded_regions.size() ? recorded : g_recorded_regions.size();
	for (size_t index = 0; index < bounded && count < windows->size(); ++index)
	{
		const RecordedRegion& region = g_recorded_regions[index];
		if (region.size != reference.size) continue;
		(*windows)[count].begin = region.base + reference.offset;
		(*windows)[count].end   = region.base + reference.offset + reference.length;
		++count;
	}
	return count;
}

bool RootsWordInWatchWindows(uint64_t word, const RootsWatchWindow* windows, size_t count)
{
	for (size_t index = 0; index < count; ++index)
	{
		if (word >= windows[index].begin && word < windows[index].end) return true;
	}
	return false;
}

bool RootSnapshotRecordAvailable()
{
	return g_roots_records.fetch_add(1, std::memory_order_relaxed) < kRootsMaximumRecords;
}
} // namespace

void SetCaptureDiagnosticGuestThread(uint64_t guestThreadHandle)
{
	g_capture_thread_handle = guestThreadHandle;
}

void NoteCaptureDiagnosticHostCall(uint64_t hostCallTarget)
{
	g_capture_host_call = hostCallTarget;
}

uint64_t CaptureDiagnosticHostCall()
{
	return g_capture_host_call;
}

bool GuestRootSnapshotEnabled()
{
	static const bool enabled = [] {
		const char* text = std::getenv("LUMEN_MACOS_VM_ROOT_SNAPSHOT");
		return text != nullptr && text[0] == '1' && text[1] == '\0';
	}();
	return enabled;
}

void ReportGuestRootSnapshotSummary(const char* tag, size_t claimed, size_t reported)
{
	if (!GuestRootSnapshotEnabled() || tag == nullptr) return;
	if (!RootSnapshotRecordAvailable()) return;
	std::fprintf(stderr,
	             "[VM_ROOTS_END] ns=%" PRIu64 " tag=%s claimed=%zu reported=%zu\n",
	             LedgerClockNs(), tag, claimed, reported);
}

void ReportGuestThreadRoots(const char* tag, uint32_t threadId, uint64_t handle,
                            uint64_t hostCall, uint64_t stackBase,
                            uint64_t stackSize, const uint64_t gpr[16],
                            uint64_t rip)
{
	if (!GuestRootSnapshotEnabled() || tag == nullptr || gpr == nullptr) return;
	if (!RootSnapshotRecordAvailable()) return;
	const uint64_t now_ns = LedgerClockNs();
	const uint64_t rsp = gpr[4];
	// The fifteen GP words in the order the title's collector reads them from
	// the delivered ucontext (rdi, rsi, rdx, rcx, r8, r9, rax, rbx, rbp, r10,
	// r11, r12, r13, r14, r15); RSP and RIP follow separately.
	std::fprintf(stderr,
	             "[VM_ROOTS] ns=%" PRIu64 " tag=%s thread=%u handle=0x%016" PRIx64
	             " hle=0x%016" PRIx64 " rip=0x%016" PRIx64 " rsp=0x%016" PRIx64
	             " gp=0x%016" PRIx64 ",0x%016" PRIx64 ",0x%016" PRIx64 ",0x%016" PRIx64
	             ",0x%016" PRIx64 ",0x%016" PRIx64 ",0x%016" PRIx64 ",0x%016" PRIx64
	             ",0x%016" PRIx64 ",0x%016" PRIx64 ",0x%016" PRIx64 ",0x%016" PRIx64
	             ",0x%016" PRIx64 ",0x%016" PRIx64 ",0x%016" PRIx64 "\n",
	             now_ns, tag, threadId, handle, hostCall, rip, rsp,
	             gpr[7], gpr[6], gpr[2], gpr[1], gpr[8], gpr[9], gpr[0], gpr[3],
	             gpr[5], gpr[10], gpr[11], gpr[12], gpr[13], gpr[14], gpr[15]);

	// The stack window is bounded by the private x86-64 stack allocation that
	// owns RSP: [max(rsp, stackBase), min(rsp+1MiB, stackBase+stackSize)).
	const bool bounded = stackBase != 0 && stackSize != 0 && stackBase <= UINT64_MAX - stackSize;
	uint64_t window_begin = rsp;
	uint64_t window_end = rsp > UINT64_MAX - kRootsStackWindowSize ? UINT64_MAX
	                                                                : rsp + kRootsStackWindowSize;
	if (bounded)
	{
		const uint64_t allocation_end = stackBase + stackSize;
		if (window_begin < stackBase) window_begin = stackBase;
		if (window_end > allocation_end) window_end = allocation_end;
	}
	const RegionLedgerReference& reference = RegionLedgerReferenceConfig();
	std::array<RootsWatchWindow, kRootsMaximumWatchWindows> windows {};
	const size_t window_count = ResolveRootsWatchWindows(reference, &windows);
	size_t scanned = 0;
	size_t skipped = 0;
	size_t hits = 0;
	if (window_count != 0 && window_begin < window_end)
	{
		std::array<uint8_t, kRootsReadChunkSize> buffer {};
		for (uint64_t cursor = window_begin; cursor < window_end;)
		{
			const uint64_t remaining = window_end - cursor;
			const size_t chunk = remaining < buffer.size() ? static_cast<size_t>(remaining)
			                                               : buffer.size();
			mach_vm_size_t copied = 0;
			const kern_return_t result = mach_vm_read_overwrite(
			    mach_task_self(), cursor, chunk,
			    reinterpret_cast<mach_vm_address_t>(buffer.data()), &copied);
			if (result != KERN_SUCCESS || copied != chunk)
			{
				skipped += chunk;
				cursor += chunk;
				continue;
			}
			scanned += chunk;
			for (size_t offset = 0; offset + sizeof(uint64_t) <= chunk; offset += sizeof(uint64_t))
			{
				uint64_t word = 0;
				std::memcpy(&word, buffer.data() + offset, sizeof(word));
				if (!RootsWordInWatchWindows(word, windows.data(), window_count)) continue;
				++hits;
				if (hits <= kRootsMaximumReferencesPerThread &&
				    g_roots_references.fetch_add(1, std::memory_order_relaxed) < kRootsMaximumReferences &&
				    RootSnapshotRecordAvailable())
				{
					std::fprintf(stderr,
					             "[VM_ROOTS_REF] ns=%" PRIu64 " tag=%s thread=%u handle=0x%016" PRIx64
					             " offset=0x%08" PRIx64 " value=0x%016" PRIx64 "\n",
					             now_ns, tag, threadId, handle, cursor + offset - rsp, word);
				}
			}
			cursor += chunk;
		}
	}
	if (RootSnapshotRecordAvailable())
	{
		const uint64_t window_size = window_end > window_begin ? window_end - window_begin : 0;
		std::fprintf(stderr,
		             "[VM_ROOTS_SCAN] ns=%" PRIu64 " tag=%s thread=%u rsp=0x%016" PRIx64
		             " window=0x%016" PRIx64 "+0x%016" PRIx64 " scanned=%zu skipped=%zu"
		             " hits=%zu reference=%zu bounded=%u\n",
		             now_ns, tag, threadId, rsp, window_begin, window_size, scanned, skipped,
		             hits, window_count, bounded ? 1U : 0U);
	}
}
#endif

uint64_t Alloc(uint64_t address, uint64_t size, Mode mode, AllocationDenialMetadata* denial)
{
	if (denial != nullptr) *denial = {};
	if (!ValidateProtectionMode(mode) || !ValidateMappingSize(size) || size > UINT64_MAX - address)
	{
		if (denial != nullptr) denial->reason = AllocationFailure::InvalidArgument;
		return 0;
	}
	auto& registry = Registry();
	GatedFexGuestMemoryMutation mutation {
	    registry, FexGuestMemoryMutationKind::AdditiveMapping};
	if (!mutation.IsAdmitted())
	{
		// This is still before the local gate, allocation and publication. Only
		// an explicit metadata caller may act on the provider's checked conflict;
		// Alloc itself never donates a reader or retries the operation.
		if (denial != nullptr) *denial = {AllocationFailure::AdmissionDenied, mutation.IsRetryable()};
		return 0;
	}
	const uint64_t result = AllocateRaw(address, size, mode);
	if (result == 0)
	{
		if (denial != nullptr) denial->reason = AllocationFailure::HostAllocationFailed;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		TraceVMMutationV1("alloc", address, size, mode, 0, 0,
		                  reinterpret_cast<uint64_t>(__builtin_return_address(0)));
#endif
		return 0;
	}
	if (!RegisterMapping(result, size, result, size, mode))
	{
		munmap(reinterpret_cast<void*>(static_cast<uintptr_t>(result)), static_cast<size_t>(size));
		if (denial != nullptr) denial->reason = AllocationFailure::HostAllocationFailed;
		return 0;
	}
	mutation.Commit(result, size);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	TraceVMMutationV1("alloc", address, size, mode, result,
	                  static_cast<uint32_t>(mode),
	                  reinterpret_cast<uint64_t>(__builtin_return_address(0)));
	RecordRegionSnapshot(result, size);
#endif
	return result;
}

uint64_t AllocAligned(uint64_t address, uint64_t size, Mode mode, uint64_t alignment)
{
	if (!ValidateMappingSize(size) || !IsPowerOfTwo(alignment) || !ValidateProtectionMode(mode))
	{
		return 0;
	}

	auto& registry = Registry();
	GatedFexGuestMemoryMutation mutation {
	    registry, FexGuestMemoryMutationKind::AdditiveMapping};
	if (!mutation.IsAdmitted()) return 0;
	uint64_t raw_size = 0;
	if (!AddWithoutOverflow(size, alignment - 1, &raw_size) || !ValidateMappingSize(raw_size))
	{
		return 0;
	}

	const uint64_t raw = AllocateRaw(address, raw_size, mode);
	if (raw == 0)
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		TraceVMMutationV1("alloc-aligned", address, size, mode, 0, 0,
		                  reinterpret_cast<uint64_t>(__builtin_return_address(0)));
#endif
		return 0;
	}

	uint64_t aligned = 0;
	if (!RoundUp(raw, alignment, &aligned) || !RegisterMapping(raw, raw_size, aligned, size, mode))
	{
		munmap(reinterpret_cast<void*>(static_cast<uintptr_t>(raw)), static_cast<size_t>(raw_size));
		return 0;
	}
	mutation.Commit(aligned, size);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	TraceVMMutationV1("alloc-aligned", address, size, mode, aligned,
	                  static_cast<uint32_t>(mode),
	                  reinterpret_cast<uint64_t>(__builtin_return_address(0)));
	RecordRegionSnapshot(aligned, size);
#endif
	return aligned;
}

bool AllocFixed(uint64_t address, uint64_t size, Mode mode)
{
	if (!ValidateProtectionMode(mode)) return false;
	MutationTransaction reserved_transaction;
	const ReservedFixedResult reserved_result =
	    BeginAllocFixedInReservation(address, size, mode,
	                                  &reserved_transaction);
	if (reserved_result == ReservedFixedResult::Begun)
	{
		reserved_transaction.Commit();
		return true;
	}
	if (reserved_result == ReservedFixedResult::Failed) return false;

	// AllocateRaw deliberately does not use MAP_FIXED, so a successful exact
	// address result only adds an unused mapping and cannot replace one.
	if (address == 0 || !ValidateMappingSize(size) || (address & (HostPageSize() - 1)) != 0)
	{
		return false;
	}

	auto& registry = Registry();
	GatedFexGuestMemoryMutation mutation {
	    registry, FexGuestMemoryMutationKind::AdditiveMapping};
	if (!mutation.IsAdmitted()) return false;
	const uint64_t result = AllocateRaw(address, size, mode);
	if (result == 0)
	{
		return false;
	}
	if (result != address)
	{
		munmap(reinterpret_cast<void*>(static_cast<uintptr_t>(result)), static_cast<size_t>(size));
		return false;
	}
	if (!RegisterMapping(result, size, result, size, mode))
	{
		munmap(reinterpret_cast<void*>(static_cast<uintptr_t>(result)), static_cast<size_t>(size));
		return false;
	}
	mutation.Commit(address, size);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	TraceVMMutationV1("alloc-fixed", address, size, mode, result,
	                  static_cast<uint32_t>(mode),
	                  reinterpret_cast<uint64_t>(__builtin_return_address(0)));
	RecordRegionSnapshot(result, size);
#endif
	return true;
}

bool BindFreshGuestMappingToSharedBacking(uint64_t address, uint64_t bytes, int fd,
    uint64_t backing_offset, Mode mode) noexcept
{
	const uint64_t host_page = HostPageSize();
	if (fd < 0 || address == 0 || bytes == 0 || bytes > SIZE_MAX ||
	    address > UINT64_MAX - bytes || backing_offset > UINT64_MAX - bytes ||
	    backing_offset > static_cast<uint64_t>((std::numeric_limits<off_t>::max)()) ||
	    address % host_page != 0 || bytes % host_page != 0 || backing_offset % host_page != 0 ||
	    !ValidateProtectionMode(mode) || g_vm_readers != 0 || g_vm_external_mutation_token != 0 ||
	    g_vm_external_native_token != 0) return false;
	struct stat file {};
	if (fstat(fd, &file) != 0 || file.st_size < 0 || backing_offset + bytes > static_cast<uint64_t>(file.st_size)) return false;
	auto& registry = Registry();
	std::optional<GatedFexGuestMemoryMutation> mutation;
	// A reservation transaction already owns both gates and publishes the
	// final extent at Commit. Reacquiring the local gate would self-deadlock.
	if (g_vm_mutation_owner != &registry)
	{
		mutation.emplace(registry, FexGuestMemoryMutationKind::Exclusive);
		if (!mutation->IsAdmitted()) return false;
	}
	std::lock_guard lock(registry.mutex);
	const size_t index = LowerBoundMapping(registry, address);
	if (index >= registry.mapping_num) return false;
	auto& mapping = registry.mappings[index];
	if (mapping.visible_base != address || mapping.visible_size != bytes) return false;
	for (uint64_t page = 0; page != mapping.page_mode_num; ++page)
		if (mapping.page_modes[static_cast<size_t>(page)] != mode) return false;
	ImmutableSnapshotMutation snapshot_mutation(registry);
	void* const requested = reinterpret_cast<void*>(static_cast<uintptr_t>(address));
	void* const mapped = mmap(requested, static_cast<size_t>(bytes), HostProtection(mode),
	    MAP_SHARED | MAP_FIXED, fd, static_cast<off_t>(backing_offset));
	if (mapped != requested)
	{
		if (mapped != MAP_FAILED) munmap(mapped, static_cast<size_t>(bytes));
		// Only a fresh unpublished extent may call this API. Restore exactly
		// its initial anonymous storage/rights before the owner aborts it.
		if (!ReplaceReservationRange(address, bytes, mode)) std::abort();
		MarkMappingContinuity(registry, mapping, true);
		return false;
	}
	std::memset(mapping.validated_code_host_pages, 0, sizeof(mapping.validated_code_host_pages));
	mapping.validated_code_host_page_next = 0;
	RecordMutation(registry);
	MarkMappingContinuity(registry, mapping);
	(void)PublishFullSnapshot(registry);
	if (mutation.has_value()) mutation->Commit(address, bytes);
	return true;
}

bool Free(uint64_t address)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	VMMutationTraceScope trace {"free", address, 0, Mode::NoAccess,
	                           reinterpret_cast<uint64_t>(__builtin_return_address(0))};
#endif
	if (ConsumeVirtualRangeTestFault(
	        VirtualRangeTestFault::FexExclusiveAdmission))
	{
		return false;
	}
	if (address == 0)
	{
		return false;
	}

	auto& registry = Registry();
	GatedFexGuestMemoryMutation mutation {
	    registry, FexGuestMemoryMutationKind::Exclusive};
	if (!mutation.IsAdmitted()) return false;
	std::lock_guard<std::mutex> lock(registry.mutex);
	const size_t index = LowerBoundMapping(registry, address);
	if (index >= registry.mapping_num || registry.mappings[index].visible_base != address)
	{
		return false;
	}
	ImmutableSnapshotMutation snapshot_mutation(registry);
	const Mapping mapping = registry.mappings[index];
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	trace.Observe(mapping.identity, mapping.reservation_identity, mapping.visible_size,
	              registry.mutation_generation.load(std::memory_order_relaxed));
#endif
	const bool released = mapping.reservation_identity != 0
	                          ? (!ConsumeVirtualRangeTestFault(
	                                 VirtualRangeTestFault::DecommitHost) &&
	                             ReplaceReservationRange(
	                                mapping.raw_base, mapping.raw_size,
	                                Mode::NoAccess))
	                          : munmap(reinterpret_cast<void*>(
	                                       static_cast<uintptr_t>(mapping.raw_base)),
	                                   static_cast<size_t>(mapping.raw_size)) == 0;
	if (!released)
	{
		MarkMappingContinuity(registry, registry.mappings[index], true);
		return false;
	}
	EraseMapping(registry, index);
	RecordMutation(registry);
	if (mapping.reservation_identity != 0)
	{
		(void)PublishVirtualRangeSnapshot(registry);
	} else
	{
		(void)PublishFreedMapping(registry, MappingSnapshotOwner(mapping));
	}
	mutation.Commit(mapping.visible_base, mapping.visible_size);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	trace.Complete("success", address, 0,
	               registry.mutation_generation.load(std::memory_order_relaxed));
#endif
	return true;
}

bool Protect(uint64_t address, uint64_t size, Mode mode, Mode* old_mode)
{
	if (!ValidateProtectionMode(mode)) return false;
	auto& registry = Registry();
	GatedFexGuestMemoryMutation mutation {
	    registry, FexGuestMemoryMutationKind::Exclusive};
	if (!mutation.IsAdmitted()) return false;
	if (size == 0)
	{
		std::lock_guard<std::mutex> lock(registry.mutex);
		RecordMutation(registry);
		mutation.Commit(address, 0);
		return true;
	}

	std::lock_guard<std::mutex> lock(registry.mutex);
	ImmutableSnapshotMutation snapshot_mutation(registry);
	ProtectionRange range {};
	Mode previous_mode = Mode::NoAccess;
	const bool resolved =
	    ResolveProtectionRange(registry, address, size, &range);
	if (resolved)
		previous_mode = range.mapping->page_modes[static_cast<size_t>(range.first_page)];
	const bool result = resolved &&
	                    ApplyProtection(registry, range, mode, old_mode);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	TraceVMMutationV1("protect", address, size, mode, result ? address : 0,
	                  static_cast<uint32_t>(previous_mode),
	                  reinterpret_cast<uint64_t>(__builtin_return_address(0)));
#endif
	if (result) mutation.Commit(address, size);
	return result;
}

bool FlushInstructionCache(uint64_t address, uint64_t size)
{
	auto& registry = Registry();
	GatedFexGuestMemoryMutation mutation {
	    registry, FexGuestMemoryMutationKind::Exclusive};
	if (!mutation.IsAdmitted()) return false;
	const bool result = FlushInstructionCacheRaw(address, size);
	// This is the publication point for bytes written through a writable guest
	// mapping. Invalidate conservatively even if the cache operation was denied:
	// the store may already have happened before this API was called.
	{
		std::lock_guard<std::mutex> lock(registry.mutex);
		RecordMutation(registry);
		MarkRangeContinuity(registry, address, size);
	}
	mutation.Commit(address, size);
	return result;
}

bool PatchReplace(uint64_t vaddr, uint64_t value)
{
	auto& registry = Registry();
	GatedFexGuestMemoryMutation mutation {
	    registry, FexGuestMemoryMutationKind::Exclusive};
	if (!mutation.IsAdmitted()) return false;
	bool bytes_changed = false;
	bool footprint_resolved = false;
	const bool result = [&]
	{
		std::lock_guard<std::mutex> lock(registry.mutex);
		ProtectionRange range {};
		if (!ResolveProtectionRange(registry, vaddr, sizeof(value), &range) ||
		    range.page_num > 2)
		{
			return false;
		}
		footprint_resolved = true;
		// The legacy query path is serialized by registry.mutex. Keep the
		// immutable readers behind the same boundary while host permissions are
		// temporarily widened and restored, even though the logical page modes do
		// not change.
		ImmutableSnapshotMutation snapshot_mutation(registry);

		bool flush_required = false;
		for (uint64_t page = 0; page < range.page_num; page++)
		{
			flush_required = flush_required ||
			                 IsExecute(range.mapping->page_modes[
			                     static_cast<size_t>(range.first_page + page)]);
		}

		uint64_t physical_begin = 0;
		uint64_t physical_end   = 0;
		if (!PhysicalRange(range, &physical_begin, &physical_end))
		{
			return false;
		}
		std::memset(range.mapping->validated_code_host_pages, 0,
		            sizeof(range.mapping->validated_code_host_pages));
		range.mapping->validated_code_host_page_next = 0;

		EnableIntelJitWrites(Mode::ReadWrite);
		if (mprotect(reinterpret_cast<void*>(static_cast<uintptr_t>(physical_begin)),
		             static_cast<size_t>(physical_end - physical_begin),
		             HostProtection(Mode::ReadWrite)) != 0)
		{
			(void)RestoreHostEnvelopes(*range.mapping, physical_begin,
			                           physical_end);
			return false;
		}

		uint64_t previous = 0;
		auto* ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(vaddr));
		std::memcpy(&previous, ptr, sizeof(previous));
		bytes_changed = previous != value;
		if (bytes_changed) std::memcpy(ptr, &value, sizeof(value));

		// Cache maintenance itself accesses the addressed host pages on ARM64.
		// A patch may span a logical guard and executable page, so flush while
		// the temporary writable window is still open, then restore the guard.
		const bool flushed = !bytes_changed || !flush_required ||
		                     FlushInstructionCacheRaw(vaddr, sizeof(value));
		const bool restored = RestoreHostEnvelopes(
		    *range.mapping, physical_begin, physical_end);
		return restored && flushed && bytes_changed;
	}();

	// Preserve the legacy NODA/iOS mutation-generation contract: a no-op or a
	// failure before the store does not publish a VM mutation, while a changed
	// value remains visible even if restoring permissions later fails.
#if defined(LUMEN_FEXCORE_RUNTIME)
	// FEX also treats an unchanged value and every partial platform failure as a
	// conservative invalidation boundary, so no translated block can survive an
	// operation whose final protection state is uncertain.
	RecordMutation(registry);
#else
	if (bytes_changed) RecordMutation(registry);
#endif
	{
		std::lock_guard<std::mutex> lock(registry.mutex);
		const bool no_public_advance = !bytes_changed;
#if defined(LUMEN_FEXCORE_RUNTIME)
		constexpr bool uncertain = false;
#else
		const bool uncertain = no_public_advance;
#endif
		(void)no_public_advance;
		if (footprint_resolved) MarkRangeContinuity(registry, vaddr, sizeof(value), uncertain);
		else registry.uncertain_continuity_generation = std::max(registry.uncertain_continuity_generation,
		                                                      ContinuityWatermark(registry, uncertain));
	}
	mutation.Commit(
	    result ? vaddr : 0,
	    result ? sizeof(value) : (std::numeric_limits<uint64_t>::max)());
	return result;
}

class ExceptionHandlerPrivate
{
};

ExceptionHandler::ExceptionHandler(): m_p(new ExceptionHandlerPrivate) {}
ExceptionHandler::~ExceptionHandler()
{
	delete m_p;
}
uint64_t ExceptionHandler::GetSize()
{
	return 0;
}
bool ExceptionHandler::Install(uint64_t, uint64_t, uint64_t, handler_func_t)
{
	return false;
}
bool ExceptionHandler::Uninstall()
{
	return true;
}

} // namespace VirtualMemory
} // namespace Lumen::Core
