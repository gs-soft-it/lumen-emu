#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Singleton.h"
#include "Lumen/Core/VirtualMemory.h"
#include "Emulator/Config.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Log.h"
#include "AndroidFexCoreRuntime.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <mach-o/dyld.h>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <type_traits>
#include <unistd.h>

extern char** environ;

namespace {
namespace Kernel = Lumen::Libs::LibKernel;
namespace Loader = Lumen::Loader;
namespace Memory = Lumen::Core::VirtualMemory;

using CleanupPushFunction = void (LUMEN_SYSV_ABI *)(Kernel::pthread_cleanup_func_t, void*,
                                                     Kernel::PthreadCleanupInfo*);
using CleanupPopFunction = void (LUMEN_SYSV_ABI *)(int);
static_assert(std::is_same_v<decltype(&Kernel::PthreadCleanupPush), CleanupPushFunction>);
static_assert(std::is_same_v<decltype(&Kernel::PthreadCleanupPop), CleanupPopFunction>);
static_assert(sizeof(Kernel::PthreadCleanupInfo) == 64 && alignof(Kernel::PthreadCleanupInfo) == 8);

bool Check(bool result, const char* description)
{
	if (!result) std::fprintf(stderr, "PthreadExitTest: %s\n", description);
	return result;
}

class Watchdog
{
public:
	explicit Watchdog(unsigned seconds): m_thread([this, seconds] {
		std::unique_lock<std::mutex> lock(m_mutex);
		if (!m_changed.wait_for(lock, std::chrono::seconds(seconds), [this] { return m_done; }))
		{
			std::fputs("PthreadExitTest: timed out\n", stderr);
			std::_Exit(2);
		}
	}) {}
	~Watchdog()
	{
		{ std::lock_guard<std::mutex> lock(m_mutex); m_done = true; }
		m_changed.notify_one();
		m_thread.join();
	}
private:
	std::mutex m_mutex;
	std::condition_variable m_changed;
	bool m_done = false;
	std::thread m_thread;
};

class Mapping
{
public:
	explicit Mapping(size_t size): m_size(size), m_address(Memory::Alloc(0, size, Memory::Mode::ReadWrite)) {}
	~Mapping() { if (m_address != 0) Memory::Free(m_address); }
	Mapping(const Mapping&) = delete;
	Mapping& operator=(const Mapping&) = delete;

	uint64_t Address() const { return m_address; }
	bool Protect(Memory::Mode mode) const { return m_address != 0 && Memory::Protect(m_address, m_size, mode); }
	bool Free()
	{
		if (m_address == 0 || !Memory::Free(m_address)) return false;
		m_address = 0;
		return true;
	}
private:
	size_t m_size;
	uint64_t m_address;
};

// Original, deliberately small x86-64 fixtures. They execute in managed RX
// mappings through RunGuestX64; none of these bytes comes from a game or FEX.
class GuestCode
{
public:
	void Bytes(std::initializer_list<uint8_t> bytes)
	{
		for (uint8_t byte: bytes)
		{
			if (m_size == m_bytes.size()) std::_Exit(3);
			m_bytes[m_size++] = byte;
		}
	}
	void Word64(uint64_t value)
	{
		for (unsigned shift = 0; shift != 64; shift += 8) Bytes({static_cast<uint8_t>(value >> shift)});
	}
	void Call(uint64_t target)
	{
		Bytes({0x48, 0xb8}); Word64(target); // movabs rax, target
		Bytes({0xff, 0xd0});                // call rax
	}
	void Begin() { Bytes({0x48, 0x83, 0xec, 0x08}); } // sub rsp, 8: align a SysV call
	void Return() { Bytes({0x48, 0x83, 0xc4, 0x08, 0xc3}); }
	void ForbiddenContinuation(uint64_t sentinel)
	{
		Bytes({0x48, 0xb8}); Word64(sentinel);
		Bytes({0xc7, 0x00, 0x01, 0x00, 0x00, 0x00}); // mov dword [rax], 1
		Bytes({0x0f, 0x0b});                         // ud2 if exit unexpectedly returned
	}
	bool Publish(const Mapping& mapping) const
	{
		if (mapping.Address() == 0) return false;
		std::memcpy(reinterpret_cast<void*>(mapping.Address()), m_bytes.data(), m_size);
		return mapping.Protect(Memory::Mode::ExecuteRead) &&
		       Memory::FlushInstructionCache(mapping.Address(), m_size) &&
		       !Loader::IsNativeHostCallTarget(mapping.Address());
	}
private:
	std::array<uint8_t, 128> m_bytes {};
	size_t m_size = 0;
};

struct ThreadRecord
{
	uint64_t value = 0;
	uint32_t expected_policy = 1;
	int expected_priority = 700;
	pthread_t host {};
	size_t host_stack_size = 0;
	std::atomic<unsigned> entries {0};
	std::atomic<unsigned> cleanups {0};
	std::atomic<bool> valid {true};
};

constexpr size_t SMALL_GUEST_STACK_SIZE = 32u * 1024u;
constexpr size_t STACK_ATTRIBUTE_SEED_SIZE = 64u * 1024u;
constexpr size_t STACK_ATTRIBUTE_STORAGE_SIZE = 2u * STACK_ATTRIBUTE_SEED_SIZE;
constexpr size_t MACOS_FEXCORE_HOST_STACK_FLOOR = 2u * 1024u * 1024u;

std::array<ThreadRecord, 3> g_records;
thread_local ThreadRecord* g_current_record = nullptr;
uint64_t g_token = 0;

enum class KeyDestructorMode : uint64_t
{
	NoReset,
	ResetOnce,
	ResetAlways,
};

struct KeyDestructorRecord
{
	Kernel::PthreadKey key = -1;
	KeyDestructorMode mode = KeyDestructorMode::NoReset;
	pthread_t expected_host {};
	Kernel::Pthread expected_self = nullptr;
	uint64_t expected_value = 0;
	unsigned calls = 0;
	bool valid = true;
	bool purged = false;
} g_key_destructor;

constexpr uint64_t KEY_DESTRUCTOR_VALUE_BASE = 0x4b455900;
constexpr uint64_t KEY_DESTRUCTOR_RESULT_BASE = 0x54534400;
constexpr uint64_t CROSS_KEY_VALUE_A = 0x4b455941;
constexpr uint64_t CROSS_KEY_VALUE_B_OLD = 0x4b455942;
constexpr uint64_t CROSS_KEY_VALUE_B_NEW = 0x4b455943;
constexpr uint64_t CROSS_KEY_RESULT = 0x54534441;

struct CrossKeyDestructorRecord
{
	Kernel::PthreadKey key_a = -1;
	Kernel::PthreadKey key_b = -1;
	pthread_t expected_host {};
	Kernel::Pthread expected_self = nullptr;
	unsigned calls_a = 0;
	unsigned calls_b = 0;
	bool valid = true;
} g_cross_key_destructor;

struct SpecificReuseFixture
{
	std::mutex mutex;
	std::condition_variable changed;
	Kernel::PthreadKey key = -1;
	unsigned reached = 0;
	unsigned phase = 0;
	unsigned cleanups = 0;
	bool purged = true;
} g_specific_reuse;

constexpr uint64_t SPECIFIC_MAIN_OLD = 0x5350454301;
constexpr uint64_t SPECIFIC_MAIN_NEW = 0x5350454302;
constexpr uint64_t SPECIFIC_WORKER_OLD = 0x5350454303;
constexpr uint64_t SPECIFIC_WORKER_NEW = 0x5350454304;
constexpr uint64_t SPECIFIC_REUSE_RESULT = 0x5350454349464943;

void ObserveSpecificPurge(Kernel::Pthread)
{
	++g_specific_reuse.cleanups;
	g_specific_reuse.purged = Kernel::PthreadGetspecific(g_specific_reuse.key) == nullptr && g_specific_reuse.purged;
}

void ObserveDestructorPurge(Kernel::Pthread)
{
	g_key_destructor.purged = Kernel::PthreadGetspecific(g_key_destructor.key) == nullptr;
}

uint64_t LUMEN_SYSV_ABI ObserveSpecificReuse(uint64_t fresh_thread)
{
	const auto key = g_specific_reuse.key;
	bool passed = Kernel::PthreadGetspecific(key) == nullptr;
	if (fresh_thread != 0) return passed ? SPECIFIC_REUSE_RESULT : 0;
	passed = Kernel::PthreadSetspecific(key, reinterpret_cast<void*>(SPECIFIC_WORKER_OLD)) == 0 && passed;
	passed = reinterpret_cast<uint64_t>(Kernel::PthreadGetspecific(key)) == SPECIFIC_WORKER_OLD && passed;
	const auto rendezvous = [](unsigned phase) {
		Loader::ScopedGuestHostWait host_wait;
		std::unique_lock lock(g_specific_reuse.mutex);
		g_specific_reuse.reached = phase;
		g_specific_reuse.changed.notify_all();
		return g_specific_reuse.changed.wait_for(lock, std::chrono::seconds(2),
		    [phase] { return g_specific_reuse.phase >= phase; });
	};
	if (!rendezvous(1)) return 0;
	// The parent deleted and recreated this exact slot on another host thread.
	// Its new value must not revive our cached value from the prior generation.
	passed = Kernel::PthreadGetspecific(key) == nullptr && passed;
	passed = Kernel::PthreadSetspecific(key, reinterpret_cast<void*>(SPECIFIC_WORKER_NEW)) == 0 && passed;
	passed = reinterpret_cast<uint64_t>(Kernel::PthreadGetspecific(key)) == SPECIFIC_WORKER_NEW && passed;
	if (!rendezvous(2)) return 0;
	passed = reinterpret_cast<uint64_t>(Kernel::PthreadGetspecific(key)) == SPECIFIC_WORKER_NEW && passed;
	passed = Kernel::PthreadSetspecific(key, nullptr) == 0 && passed;
	passed = Kernel::PthreadGetspecific(key) == nullptr && passed;
	// Leave a value on a key without a destructor for the real thread-exit
	// cleanup path; the next managed thread still has to begin empty.
	passed = Kernel::PthreadSetspecific(key, reinterpret_cast<void*>(SPECIFIC_WORKER_NEW)) == 0 && passed;
	return passed ? SPECIFIC_REUSE_RESULT : 0;
}

uint64_t LUMEN_SYSV_ABI PrepareKeyDestructor(uint64_t raw_mode)
{
	if (raw_mode > static_cast<uint64_t>(KeyDestructorMode::ResetAlways)) std::_Exit(3);
	g_key_destructor.mode = static_cast<KeyDestructorMode>(raw_mode);
	g_key_destructor.expected_host = pthread_self();
	g_key_destructor.expected_self = Kernel::PthreadSelf();
	g_key_destructor.expected_value = KEY_DESTRUCTOR_VALUE_BASE + raw_mode * 0x10;
	g_key_destructor.calls = 0;
	g_key_destructor.purged = false;
	g_key_destructor.valid = Kernel::PthreadSetspecific(
	    g_key_destructor.key,
	    reinterpret_cast<void*>(g_key_destructor.expected_value)) == 0;
	return KEY_DESTRUCTOR_RESULT_BASE + raw_mode;
}

void LUMEN_SYSV_ABI ObserveKeyDestructor(void* data)
{
	if (!pthread_equal(g_key_destructor.expected_host, pthread_self()) ||
	    Kernel::PthreadSelf() != g_key_destructor.expected_self ||
	    Kernel::PthreadGetspecific(g_key_destructor.key) != nullptr ||
	    reinterpret_cast<uint64_t>(data) != g_key_destructor.expected_value)
	{
		g_key_destructor.valid = false;
	}

	++g_key_destructor.calls;
	const bool reset = g_key_destructor.mode == KeyDestructorMode::ResetAlways ||
	                   (g_key_destructor.mode == KeyDestructorMode::ResetOnce &&
	                    g_key_destructor.calls == 1);
	if (reset)
	{
		++g_key_destructor.expected_value;
		if (Kernel::PthreadSetspecific(
		        g_key_destructor.key,
		        reinterpret_cast<void*>(g_key_destructor.expected_value)) != 0)
		{
			g_key_destructor.valid = false;
		}
	}
}

uint64_t LUMEN_SYSV_ABI PrepareCrossKeyDestructors()
{
	g_cross_key_destructor.expected_host = pthread_self();
	g_cross_key_destructor.expected_self = Kernel::PthreadSelf();
	g_cross_key_destructor.calls_a = 0;
	g_cross_key_destructor.calls_b = 0;
	g_cross_key_destructor.valid =
	    Kernel::PthreadSetspecific(g_cross_key_destructor.key_a,
	                               reinterpret_cast<void*>(CROSS_KEY_VALUE_A)) == 0 &&
	    Kernel::PthreadSetspecific(g_cross_key_destructor.key_b,
	                               reinterpret_cast<void*>(CROSS_KEY_VALUE_B_OLD)) == 0;
	return CROSS_KEY_RESULT;
}

void LUMEN_SYSV_ABI ObserveCrossKeyDestructor(void* data)
{
	if (!pthread_equal(g_cross_key_destructor.expected_host, pthread_self()) ||
	    Kernel::PthreadSelf() != g_cross_key_destructor.expected_self)
	{
		g_cross_key_destructor.valid = false;
	}

	const uint64_t value = reinterpret_cast<uint64_t>(data);
	if (value == CROSS_KEY_VALUE_A)
	{
		++g_cross_key_destructor.calls_a;
		if (Kernel::PthreadGetspecific(g_cross_key_destructor.key_a) != nullptr ||
		    Kernel::PthreadSetspecific(g_cross_key_destructor.key_b,
		                               reinterpret_cast<void*>(CROSS_KEY_VALUE_B_NEW)) != 0)
		{
			g_cross_key_destructor.valid = false;
		}
	} else if (value == CROSS_KEY_VALUE_B_NEW)
	{
		++g_cross_key_destructor.calls_b;
		if (Kernel::PthreadGetspecific(g_cross_key_destructor.key_b) != nullptr)
		{
			g_cross_key_destructor.valid = false;
		}
	} else
	{
		g_cross_key_destructor.valid = false;
	}
}

std::mutex g_cleanup_mutex;
std::array<uint64_t, 8> g_cleanup_order {};
unsigned g_cleanup_count = 0;
Kernel::PthreadCleanupInfo* g_reentrant_cleanup_info = nullptr;
std::atomic<bool> g_cleanup_cancel_ready {false};

void RecordCleanupValue(uint64_t value)
{
	std::lock_guard lock(g_cleanup_mutex);
	if (g_cleanup_count >= g_cleanup_order.size()) std::_Exit(3);
	g_cleanup_order[g_cleanup_count++] = value;
}

void ResetCleanupValues()
{
	std::lock_guard lock(g_cleanup_mutex);
	g_cleanup_order.fill(0);
	g_cleanup_count = 0;
}

void LUMEN_SYSV_ABI ObservePthreadCleanup(void* argument)
{
	RecordCleanupValue(reinterpret_cast<uint64_t>(argument));
}

void LUMEN_SYSV_ABI ObserveReentrantPthreadCleanup(void* argument)
{
	RecordCleanupValue(reinterpret_cast<uint64_t>(argument));
	Kernel::PthreadCleanupPush(&ObservePthreadCleanup, reinterpret_cast<void*>(5),
	                           g_reentrant_cleanup_info);
	Kernel::PthreadCleanupPop(1);
}

void* LUMEN_SYSV_ABI WaitForPthreadCleanupCancellation(void* storage)
{
	auto* info = static_cast<Kernel::PthreadCleanupInfo*>(storage);
	Kernel::PthreadCleanupPush(&ObservePthreadCleanup, reinterpret_cast<void*>(1), &info[6]);
	Kernel::PthreadCleanupPush(&ObservePthreadCleanup, reinterpret_cast<void*>(2), &info[7]);
	g_cleanup_cancel_ready.store(true, std::memory_order_release);
	for (;;)
	{
		Kernel::PthreadTestcancel();
		std::this_thread::yield();
	}
}

struct JoinFixture
{
	std::mutex mutex;
	std::condition_variable changed;
	Kernel::Pthread target = nullptr;
	int self_join_result = 0;
	unsigned native_joins = 0;
	unsigned cleanups = 0;
	bool entry_ready = false;
	bool release_entry = false;
	bool cleanup_ready = false;
	bool release_cleanup = false;
	bool other_finished = false;
	bool other_passed = false;
} g_join_fixture;

constexpr uint64_t JOIN_FIXTURE_RESULT = 0x13579;

uint64_t LUMEN_SYSV_ABI ObserveJoinFixture(uint64_t self_join)
{
	const auto self = Kernel::PthreadSelf();
	const int result = self_join != 0 ? Kernel::PthreadJoin(self, nullptr) : 0;
	{
		Loader::ScopedGuestHostWait host_wait;
		std::unique_lock lock(g_join_fixture.mutex);
		g_join_fixture.target = self;
		g_join_fixture.self_join_result = result;
		g_join_fixture.entry_ready = true;
		g_join_fixture.changed.notify_all();
		g_join_fixture.changed.wait(lock, [] { return g_join_fixture.release_entry; });
	}
	return JOIN_FIXTURE_RESULT;
}

void HoldAfterCleanup(Kernel::Pthread thread)
{
	Loader::ScopedGuestHostWait host_wait;
	std::unique_lock lock(g_join_fixture.mutex);
	if (thread != g_join_fixture.target) return;
	++g_join_fixture.cleanups;
	g_join_fixture.cleanup_ready = true;
	g_join_fixture.changed.notify_all();
	g_join_fixture.changed.wait(lock, [] { return g_join_fixture.release_cleanup; });
}

void ObserveNativeJoin(Kernel::Pthread thread)
{
	std::lock_guard lock(g_join_fixture.mutex);
	if (thread == g_join_fixture.target)
	{
		++g_join_fixture.native_joins;
		g_join_fixture.changed.notify_all();
	}
}

void ResetJoinFixture()
{
	std::lock_guard lock(g_join_fixture.mutex);
	g_join_fixture.target = nullptr;
	g_join_fixture.self_join_result = 0;
	g_join_fixture.native_joins = 0;
	g_join_fixture.cleanups = 0;
	g_join_fixture.entry_ready = false;
	g_join_fixture.release_entry = false;
	g_join_fixture.cleanup_ready = false;
	g_join_fixture.release_cleanup = false;
	g_join_fixture.other_finished = false;
	g_join_fixture.other_passed = false;
}

template <typename Predicate>
bool WaitForJoinFixture(Predicate ready, const char* description)
{
	std::unique_lock lock(g_join_fixture.mutex);
	return Check(g_join_fixture.changed.wait_for(lock, std::chrono::seconds(2), ready), description);
}

void ReleaseJoinFixture(bool cleanup)
{
	std::lock_guard lock(g_join_fixture.mutex);
	(cleanup ? g_join_fixture.release_cleanup : g_join_fixture.release_entry) = true;
	g_join_fixture.changed.notify_all();
}

bool SchedulingMatches(Kernel::Pthread thread, uint32_t expected_policy, int expected_priority)
{
	uint32_t policy = ~uint32_t {0};
	Kernel::KernelSchedParam param {};
	param.sched_priority = -1;
	int priority = -1;
	return Kernel::PthreadGetschedparam(thread, &policy, &param) == 0 &&
	       Kernel::PthreadGetprio(thread, &priority) == 0 && policy == expected_policy &&
	       param.sched_priority == expected_priority && priority == expected_priority;
}

uint64_t LUMEN_SYSV_ABI ObserveEntry(uint64_t index)
{
	if (index >= g_records.size()) std::_Exit(3);
	auto& record = g_records[index];
	g_current_record = &record;
	record.host = pthread_self();
	record.host_stack_size = pthread_get_stacksize_np(record.host);
	++record.entries;
	if (!Kernel::PthreadHasManagedExitBoundary()) record.valid = false;
	if (!SchedulingMatches(Kernel::PthreadSelf(), record.expected_policy, record.expected_priority)) record.valid = false;
	if (index == 1 && (Kernel::PthreadSetprio(Kernel::PthreadSelf(), 650) != 0 ||
	                  !SchedulingMatches(Kernel::PthreadSelf(), 3, 650))) record.valid = false;
	if (index == 1)
	{
		const Kernel::KernelSchedParam requested {680};
		if (Kernel::PthreadSetschedparam(Kernel::PthreadSelf(), 1, &requested) != 0 ||
		    !SchedulingMatches(Kernel::PthreadSelf(), 1, 680) ||
		    Kernel::PthreadSetprio(Kernel::PthreadSelf(), 690) != 0 ||
		    !SchedulingMatches(Kernel::PthreadSelf(), 1, 690)) record.valid = false;
	}
	return record.value;
}

uint64_t LUMEN_SYSV_ABI ObserveCleanup()
{
	auto* record = g_current_record;
	if (record == nullptr) std::_Exit(3);
	if (Kernel::PthreadHasManagedExitBoundary() || !pthread_equal(record->host, pthread_self())) record->valid = false;
	++record->cleanups;
	return 0;
}

uint64_t LUMEN_SYSV_ABI EnterNestedGuest(uint64_t entry)
{
	return Loader::RunGuestX64(entry, 0x1234);
}

struct TlsRetirementFixture
{
	Loader::RuntimeLinker* linker = nullptr;
	std::atomic<int> target_id {0};
	std::atomic<uint64_t> original_storage {0}, added_storage {0};
	std::atomic<bool> start_writer {false}, writer_published {false}, writer_ended {false};
	std::atomic<bool> pending_verified {false}, registry_retired {false}, passed {true};
	std::atomic<unsigned> retries {0}, readmissions {0};
} g_tls_retirement;

uint64_t LUMEN_SYSV_ABI ObserveTlsRetirementTarget()
{
	// This Program is owned by this fixture's linker until final Clear(). No
	// borrowed module pointer is carried by DeleteTlss or across its donation.
	auto* program = g_tls_retirement.linker->FindProgramByName("pthread-exit-synthetic-tls");
	if (program == nullptr) { g_tls_retirement.passed.store(false); return 0; }
	const auto storage = reinterpret_cast<uint64_t>(Loader::RuntimeLinker::TlsGetAddr(program));
	g_tls_retirement.original_storage.store(storage, std::memory_order_release);
	g_tls_retirement.target_id.store(Lumen::Core::Thread::GetThreadIdUnique(), std::memory_order_release);
	return JOIN_FIXTURE_RESULT;
}

void ObserveTlsRetirementAdmission(uint32_t phase, int thread_id, uint32_t)
{
	if (thread_id != g_tls_retirement.target_id.load(std::memory_order_acquire)) return;
	if (phase == 0)
	{
		g_tls_retirement.start_writer.store(true, std::memory_order_release);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		// The real writer must be waiting on this real HLE reader. A successful
		// probe closes an empty epoch and retries; no mapping is changed here.
		while (std::chrono::steady_clock::now() < deadline)
		{
			uint32_t retryable = 0;
			const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV3(
			    LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2, &retryable);
			if (token != 0)
			{
				if (LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 0)
					g_tls_retirement.passed.store(false, std::memory_order_release);
			} else if (retryable == 1)
			{
				g_tls_retirement.pending_verified.store(true, std::memory_order_release);
				break;
			} else
			{
				g_tls_retirement.passed.store(false, std::memory_order_release);
				break;
			}
			std::this_thread::yield();
		}
	} else if (phase == 1)
	{
		g_tls_retirement.retries.fetch_add(1, std::memory_order_relaxed);
		if (!g_tls_retirement.pending_verified.load(std::memory_order_acquire) ||
		    g_tls_retirement.writer_published.load(std::memory_order_acquire) || Memory::CurrentThreadHasMemoryLease())
			g_tls_retirement.passed.store(false, std::memory_order_release);
	} else if (phase == 2)
	{
		g_tls_retirement.readmissions.fetch_add(1, std::memory_order_relaxed);
		if (!g_tls_retirement.writer_published.load(std::memory_order_acquire))
			g_tls_retirement.passed.store(false, std::memory_order_release);
	} else if (phase == 3)
	{
		g_tls_retirement.registry_retired.store(!g_tls_retirement.linker->HasTlsForRetirementTest(thread_id),
		    std::memory_order_release);
	}
}

bool Initialize(Loader::RuntimeLinker& linker, uint64_t* exit_target)
{
	auto* symbols = linker.Symbols();
	if (!Check(symbols != nullptr &&
	           symbols->RegisterHostCallTarget(reinterpret_cast<uint64_t>(&ObserveEntry)) &&
	           symbols->RegisterHostCallTarget(reinterpret_cast<uint64_t>(&ObserveCleanup)) &&
	           symbols->RegisterHostCallTarget(reinterpret_cast<uint64_t>(&PrepareKeyDestructor)) &&
	           symbols->RegisterHostCallTarget(reinterpret_cast<uint64_t>(&ObserveKeyDestructor)) &&
	           symbols->RegisterHostCallTarget(reinterpret_cast<uint64_t>(&PrepareCrossKeyDestructors)) &&
	           symbols->RegisterHostCallTarget(reinterpret_cast<uint64_t>(&ObserveCrossKeyDestructor)) &&
	           symbols->RegisterHostCallTarget(reinterpret_cast<uint64_t>(&ObserveSpecificReuse)) &&
	           symbols->RegisterHostCallTarget(reinterpret_cast<uint64_t>(&ObservePthreadCleanup)) &&
	           symbols->RegisterHostCallTarget(reinterpret_cast<uint64_t>(&ObserveReentrantPthreadCleanup)) &&
	           symbols->RegisterHostCallTarget(reinterpret_cast<uint64_t>(&WaitForPthreadCleanupCancellation)) &&
	           symbols->RegisterHostCallTarget(reinterpret_cast<uint64_t>(&ObserveJoinFixture)) &&
	           symbols->RegisterHostCallTarget(reinterpret_cast<uint64_t>(&EnterNestedGuest)) &&
	           symbols->RegisterHostCallTarget(reinterpret_cast<uint64_t>(&ObserveTlsRetirementTarget)) &&
	           symbols->RegisterHostCallTarget(reinterpret_cast<uint64_t>(&Kernel::PthreadBackingAllocationFexContractTest)) &&
	           symbols->RegisterHostCallTarget(reinterpret_cast<uint64_t>(&Kernel::PthreadMutexInitializationFexContractTest)) &&
	           Lumen::Libs::Init(U"libkernel_1", symbols), "host-call registration failed")) return false;
	symbols->SealHostCallRegistry();
	Loader::SymbolResolve query {};
	query.name = "3kg7rT0NQIs";
	query.library = "libkernel";
	query.library_version = 1;
	query.module = "libkernel";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type = Loader::SymbolType::Func;
	const auto* record = symbols->Find(query);
	if (!Check(symbols->HostCallRegistrySealed() && record != nullptr &&
	           record->vaddr == reinterpret_cast<uint64_t>(&Kernel::PthreadExit) &&
	           Loader::IsNativeHostCallTarget(record->vaddr), "exact pthread-exit export missing")) return false;
	const auto cleanup_symbol = [&](const char* nid) {
		Loader::SymbolResolve cleanup_query = query;
		cleanup_query.name = nid;
		return symbols->Find(cleanup_query);
	};
	const auto* push = cleanup_symbol("1xvtUVx1-Sg");
	const auto* pop = cleanup_symbol("iWsFlYMf3Kw");
	const auto* push_metadata = push != nullptr ? symbols->HostCallScalarMetadataFor(push->vaddr) : nullptr;
	const auto* pop_metadata = pop != nullptr ? symbols->HostCallScalarMetadataFor(pop->vaddr) : nullptr;
	using ScalarType = Loader::HostCallScalarType;
	if (!Check(push != nullptr && push->vaddr == reinterpret_cast<uint64_t>(&Kernel::PthreadCleanupPush) &&
	           pop != nullptr && pop->vaddr == reinterpret_cast<uint64_t>(&Kernel::PthreadCleanupPop) &&
	           push_metadata != nullptr && push_metadata->arity == 3 &&
	           push_metadata->arguments[0] == ScalarType::Gp &&
	           push_metadata->arguments[1] == ScalarType::Gp &&
	           push_metadata->arguments[2] == ScalarType::Gp &&
	           push_metadata->result == ScalarType::Void && push_metadata->invoker != nullptr &&
	           pop_metadata != nullptr && pop_metadata->arity == 1 &&
	           pop_metadata->arguments[0] == ScalarType::Gp &&
	           pop_metadata->result == ScalarType::Void && pop_metadata->invoker != nullptr,
	           "pthread cleanup bindings or scalar metadata differ")) return false;
	*exit_target = record->vaddr;
	// This fixture only supplies a TLS owner. Guest execution, host calls,
	// stacks, kernel thread cleanup and join use their production paths.
	return Check(linker.SetupSyntheticTlsForPthreadExitTest(), "synthetic TLS owner setup failed");
}

bool ContendedBackingAllocation(size_t page_size)
{
	Mapping code(page_size);
	GuestCode call;
	call.Begin();
	call.Call(reinterpret_cast<uint64_t>(&Kernel::PthreadBackingAllocationFexContractTest));
	call.Return();
	if (!Check(call.Publish(code), "backing allocation guest fixture could not be published")) return false;
	for (uint64_t scenario = 0; scenario != 3; ++scenario)
		if (!Check(Loader::RunGuestX64(code.Address(), scenario) == 1,
		           "real Core backing allocation lost retry metadata, bypassed a lease or failed readmission")) return false;
	return true;
}

bool ContendedMutexInitialization(size_t page_size)
{
	Mapping code(page_size);
	GuestCode call;
	call.Begin();
	call.Call(reinterpret_cast<uint64_t>(&Kernel::PthreadMutexInitializationFexContractTest));
	call.Return();
	if (!Check(call.Publish(code), "mutex init guest fixture could not be published")) return false;
	for (uint64_t scenario = 0; scenario != 5; ++scenario)
		if (!Check(Loader::RunGuestX64(code.Address(), scenario) == 1,
		           "contended explicit mutex init failed retry, input snapshot or output lifetime validation")) return false;
	return true;
}

class SchedulingAttributes
{
public:
	~SchedulingAttributes() { if (m_attr != nullptr) Kernel::PthreadAttrDestroy(&m_attr); }
	bool Configure(int inherit)
	{
		Kernel::KernelSchedParam requested {}, observed {};
		requested.sched_priority = 600;
		return Check(Kernel::PthreadAttrInit(&m_attr) == 0 &&
		             Kernel::PthreadAttrSetschedpolicy(&m_attr, 3) == 0 &&
		             Kernel::PthreadAttrSetschedparam(&m_attr, &requested) == 0 &&
		             Kernel::PthreadAttrSetinheritsched(&m_attr, inherit) == 0 &&
		             Kernel::PthreadAttrGetschedparam(&m_attr, &observed) == 0 && observed.sched_priority == 600,
		             "child attributes lost the requested scheduling priority");
	}
	bool ConfigureStack(void* guest_stack_addr)
	{
		void* observed_addr = nullptr;
		size_t observed_size = 0;
		void* observed_addr_only = nullptr;
		size_t observed_size_only = 0;
		const auto seed_addr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(guest_stack_addr) + STACK_ATTRIBUTE_SEED_SIZE);
		return Check(Kernel::PthreadAttrSetstack(&m_attr, seed_addr, STACK_ATTRIBUTE_SEED_SIZE) == 0 &&
		             Kernel::PthreadAttrSetstacksize(&m_attr, SMALL_GUEST_STACK_SIZE) == 0 &&
		             Kernel::PthreadAttrSetstackaddr(&m_attr, guest_stack_addr) == 0 &&
		             Kernel::PthreadAttrGetstack(&m_attr, &observed_addr, &observed_size) == 0 &&
		             Kernel::PthreadAttrGetstackaddr(&m_attr, &observed_addr_only) == 0 &&
		             Kernel::PthreadAttrGetstacksize(&m_attr, &observed_size_only) == 0 &&
		             observed_addr == guest_stack_addr && observed_addr_only == guest_stack_addr &&
		             observed_size == SMALL_GUEST_STACK_SIZE && observed_size_only == SMALL_GUEST_STACK_SIZE,
		             "guest stack attributes did not round-trip independently of the host stack");
	}
	const Kernel::PthreadAttr* Address() const { return &m_attr; }
private:
	Kernel::PthreadAttr m_attr = nullptr;
};

bool ThreadStackAttributesMatch(Kernel::Pthread thread, void* guest_stack_addr)
{
	Kernel::PthreadAttr snapshot = nullptr;
	bool passed = Check(Kernel::PthreadAttrInit(&snapshot) == 0, "thread stack snapshot initialization failed");
	if (passed)
	{
		const auto seed_addr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(guest_stack_addr) + SMALL_GUEST_STACK_SIZE);
		passed = Check(Kernel::PthreadAttrSetstack(&snapshot, seed_addr, SMALL_GUEST_STACK_SIZE) == 0 &&
		               Kernel::PthreadAttrGet(thread, &snapshot) == 0,
		               "thread stack attribute copy failed");
	}
	if (passed)
	{
		void* observed_addr = nullptr;
		size_t observed_size = 0;
		passed = Check(Kernel::PthreadAttrGetstack(&snapshot, &observed_addr, &observed_size) == 0 &&
		               observed_addr == guest_stack_addr && observed_size == SMALL_GUEST_STACK_SIZE,
		               "thread stack attribute copy lost guest metadata");
	}
	if (snapshot != nullptr)
	{
		passed = Check(Kernel::PthreadAttrDestroy(&snapshot) == 0, "thread stack snapshot destruction failed") && passed;
	}
	return passed;
}

bool Start(Kernel::Pthread* thread, uint64_t entry, uint64_t argument, const char* name,
           const Kernel::PthreadAttr* attr = nullptr)
{
	return Check(Kernel::PthreadCreate(thread, attr, reinterpret_cast<Kernel::pthread_entry_func_t>(entry),
	                                   reinterpret_cast<void*>(argument), name) == 0, "pthread create failed");
}

bool Join(Kernel::Pthread thread, uint64_t expected)
{
	void* value = reinterpret_cast<void*>(~uintptr_t {0});
	return Check(Kernel::PthreadJoin(thread, &value) == 0 && reinterpret_cast<uint64_t>(value) == expected,
	             "join did not return the exact exit value");
}

bool ContendedJoinRetiresFreshTlsRegistry(Loader::RuntimeLinker& linker, size_t page_size)
{
	Mapping target_code(page_size), join_code(page_size);
	GuestCode target;
	target.Begin(); target.Call(reinterpret_cast<uint64_t>(&ObserveTlsRetirementTarget)); target.Return();
	if (!Check(target.Publish(target_code), "TLS retirement target publication failed")) return false;
	g_tls_retirement.linker = &linker;
	Kernel::Pthread child = nullptr;
	if (!Start(&child, target_code.Address(), 0, "tls-retirement-target")) return false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (g_tls_retirement.target_id.load(std::memory_order_acquire) == 0 &&
	       std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
	if (!Check(g_tls_retirement.target_id.load(std::memory_order_acquire) != 0 &&
	           g_tls_retirement.original_storage.load(std::memory_order_acquire) != 0, "TLS retirement target did not initialize")) return false;
	GuestCode join;
	join.Begin();
	join.Bytes({0x48, 0xbf}); join.Word64(reinterpret_cast<uint64_t>(child)); // movabs rdi, guest handle
	join.Bytes({0x31, 0xf6}); // xor esi, esi: null result pointer
	join.Call(reinterpret_cast<uint64_t>(&Kernel::PthreadJoin)); join.Return();
	if (!Check(join.Publish(join_code), "TLS retirement join publication failed")) return false;
	std::thread writer([&] {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!g_tls_retirement.start_writer.load(std::memory_order_acquire) &&
		       std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
		if (!g_tls_retirement.start_writer.load(std::memory_order_acquire)) return;
		const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
		    LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
		if (token == 0) return;
		// This second TLS owner did not exist when cleanup entered DeleteTlss.
		const auto storage = linker.AddSyntheticTlsForRetirementTest(g_tls_retirement.target_id.load(), token);
		g_tls_retirement.added_storage.store(storage, std::memory_order_release);
		if (storage == 0 || !linker.HasTlsForRetirementTest(g_tls_retirement.target_id.load()))
			g_tls_retirement.passed.store(false, std::memory_order_release);
		g_tls_retirement.writer_ended.store(LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0,
		    [](void*) noexcept { g_tls_retirement.writer_published.store(true, std::memory_order_release); }, nullptr) != 0,
		    std::memory_order_release);
	});
	Loader::RuntimeLinker::SetTlsRetirementAdmissionTestHook(&ObserveTlsRetirementAdmission);
	const auto result = Loader::RunGuestX64(join_code.Address());
	Loader::RuntimeLinker::SetTlsRetirementAdmissionTestHook(nullptr);
	writer.join();
	Memory::ManagedRegion original {}, added {};
	const bool storage_retired = Memory::QueryManagedRegion(g_tls_retirement.original_storage.load(), &original) == Memory::ManagedRegionState::Unmanaged &&
	    g_tls_retirement.added_storage.load() != 0 &&
	    Memory::QueryManagedRegion(g_tls_retirement.added_storage.load(), &added) == Memory::ManagedRegionState::Unmanaged;
	const bool passed = result == 0 && g_tls_retirement.passed.load() && g_tls_retirement.pending_verified.load() &&
	    g_tls_retirement.retries.load() == 1 && g_tls_retirement.readmissions.load() == 1 &&
	    g_tls_retirement.writer_published.load() && g_tls_retirement.writer_ended.load() &&
	    g_tls_retirement.registry_retired.load() && storage_retired &&
	    Kernel::PthreadJoin(child, nullptr) == Kernel::KERNEL_ERROR_ESRCH;
	std::printf("PTHREAD_TLS_RETIREMENT actual_join=1 typed_denial=%u retries=%u readmissions=%u writer_published=%u writer_ended=%u fresh_registry_retired=%u both_mappings_retired=%u result=%s\n",
	    g_tls_retirement.pending_verified.load() ? 1u : 0u, g_tls_retirement.retries.load(), g_tls_retirement.readmissions.load(),
	    g_tls_retirement.writer_published.load() ? 1u : 0u, g_tls_retirement.writer_ended.load() ? 1u : 0u,
	    g_tls_retirement.registry_retired.load() ? 1u : 0u, storage_retired ? 1u : 0u, passed ? "PASS" : "FAIL");
	return Check(passed, "contended join did not retire the fresh TLS registry after reader readmission");
}

bool JoinOwnership(size_t page_size)
{
	// Reuse the production FEX exit/return and pool paths with two original
	// fixtures. No native pthread handle reuse or reaper timer is assumed.
	Mapping held_code(page_size), short_code(page_size);
	GuestCode held;
	held.Begin(); held.Call(reinterpret_cast<uint64_t>(&ObserveJoinFixture)); held.Return();
	GuestCode short_return;
	short_return.Begin(); short_return.Bytes({0x48, 0xb8}); short_return.Word64(JOIN_FIXTURE_RESULT); short_return.Return();
	if (!Check(held.Publish(held_code) && short_return.Publish(short_code), "join fixture publication failed")) return false;

	ResetJoinFixture();
	Kernel::Pthread self_joiner = nullptr;
	if (!Start(&self_joiner, held_code.Address(), 1, "self-join-rejected")) return false;
	bool passed = WaitForJoinFixture([] { return g_join_fixture.entry_ready; }, "self-join did not return");
	if (passed)
	{
		passed = Check(g_join_fixture.self_join_result == Kernel::KERNEL_ERROR_EDEADLK,
		               "self-join did not return EDEADLK");
		Kernel::Pthread other = nullptr;
		if (Start(&other, short_code.Address(), 0, "after-rejected-join"))
		{
			passed = Check(other != self_joiner, "failed join recycled a running thread wrapper") && passed;
			passed = Join(other, JOIN_FIXTURE_RESULT) && passed;
		} else passed = false;
	}
	ReleaseJoinFixture(false);
	passed = Join(self_joiner, JOIN_FIXTURE_RESULT) && passed;
	if (!passed) return false;

	for (const bool detached: {false, true})
	{
		ResetJoinFixture();
		Kernel::PthreadSetExitTestHooks(HoldAfterCleanup, ObserveNativeJoin);
		Kernel::Pthread target = nullptr;
		if (!Start(&target, held_code.Address(), 0, detached ? "detached-reaper-held" : "joinable-owner-held"))
		{
			Kernel::PthreadSetExitTestHooks(nullptr, nullptr);
			return false;
		}
		passed = WaitForJoinFixture([] { return g_join_fixture.entry_ready; }, "join fixture did not enter");
		if (detached)
		{
			passed = Check(Kernel::PthreadDetach(target) == 0, "guest detach failed") && passed;
			passed = Check(Kernel::PthreadJoin(target, nullptr) == Kernel::KERNEL_ERROR_EINVAL,
			               "public join accepted a guest-detached thread") && passed;
		}
		ReleaseJoinFixture(false);
		passed = WaitForJoinFixture([] { return g_join_fixture.cleanup_ready; }, "cleanup did not become eligible") && passed;
		bool owner_passed = true;
		std::thread owner([&] {
			if (detached) Kernel::PthreadReapDetachedThreadsForTest();
			else owner_passed = Join(target, JOIN_FIXTURE_RESULT);
		});
		passed = WaitForJoinFixture([] { return g_join_fixture.native_joins != 0; }, "owner did not claim native join") && passed;
		// The joinable case proves exclusion against a public join owner. The
		// detached case separately exercises the process-owned reaper path.
		passed = Check(Kernel::PthreadJoin(target, nullptr) == Kernel::KERNEL_ERROR_EINVAL,
		               "second joiner acquired an owned native handle") && passed;
		std::thread creator([&] {
			Kernel::Pthread other = nullptr;
			bool ok = Start(&other, short_code.Address(), 0, "create-during-join");
			if (ok)
			{
				ok = Check(other != target, "join recycled a host thread before native exit");
				ok = Join(other, JOIN_FIXTURE_RESULT) && ok;
			}
			std::lock_guard lock(g_join_fixture.mutex);
			g_join_fixture.other_passed = ok;
			g_join_fixture.other_finished = true;
			g_join_fixture.changed.notify_all();
		});
		passed = WaitForJoinFixture([] { return g_join_fixture.other_finished; }, "Create stalled behind a native join") && passed;
		ReleaseJoinFixture(true);
		owner.join();
		creator.join();
		// The periodic reaper may have won the detached claim before the explicit
		// pass. EINVAL means that owner still pins the slot; ESRCH proves release.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		int reaped = Kernel::KERNEL_ERROR_EINVAL;
		while (reaped == Kernel::KERNEL_ERROR_EINVAL && std::chrono::steady_clock::now() < deadline)
		{
			reaped = Kernel::PthreadJoin(target, nullptr);
			if (reaped == Kernel::KERNEL_ERROR_EINVAL) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		Kernel::PthreadReapDetachedThreadsForTest();
		Kernel::PthreadSetExitTestHooks(nullptr, nullptr);
		passed = Check(reaped == Kernel::KERNEL_ERROR_ESRCH && g_join_fixture.native_joins == 1 &&
		               g_join_fixture.cleanups == 1 && g_join_fixture.other_passed && owner_passed,
		               "join/cleanup did not finish exactly once or preserve concurrent Create") && passed;
		if (!passed) return false;
	}
	return true;
}

bool SchedulingOutputs(size_t page_size)
{
	static_assert(sizeof(uint32_t) == 4 && sizeof(Kernel::KernelSchedParam) == 4);
	static_assert(offsetof(Kernel::KernelSchedParam, sched_priority) == 0);
	Mapping output(page_size);
	if (!Check(output.Address() != 0, "scheduling output allocation failed")) return false;
	constexpr uint32_t marker = 0x12345678;
	const std::array<uint32_t, 5> initial {marker, marker, marker, marker, marker};
	std::memcpy(reinterpret_cast<void*>(output.Address()), initial.data(), sizeof(initial));
	auto* mapped_policy = reinterpret_cast<uint32_t*>(output.Address() + 4);
	auto* mapped_param = reinterpret_cast<Kernel::KernelSchedParam*>(output.Address() + 12);
	const auto self = Kernel::PthreadSelf();
	*Lumen::Libs::Posix::GetErrorAddr() = 123;
	if (!Check(Kernel::PthreadGetschedparam(self, mapped_policy, mapped_param) == 0 &&
	           *Lumen::Libs::Posix::GetErrorAddr() == 123, "scheduling query failed or changed errno")) return false;
	auto expected = initial;
	expected[1] = 1;
	expected[3] = 700;
	if (!Check(std::memcmp(reinterpret_cast<void*>(output.Address()), expected.data(), sizeof(expected)) == 0 &&
	           SchedulingMatches(self, 1, 700), "main scheduling snapshot or four-byte output bounds were wrong")) return false;
	std::memcpy(reinterpret_cast<void*>(output.Address()), initial.data(), sizeof(initial));
	if (!Check(output.Protect(Memory::Mode::Read), "scheduling output protection failed")) return false;
	uint32_t policy = marker;
	Kernel::KernelSchedParam param {};
	param.sched_priority = static_cast<int>(marker);
	const auto rejected = [&](Kernel::Pthread thread, uint32_t* policy_output,
	                          Kernel::KernelSchedParam* param_output, int error) {
		*Lumen::Libs::Posix::GetErrorAddr() = 123;
		return Check(Kernel::PthreadGetschedparam(thread, policy_output, param_output) == error &&
		             *Lumen::Libs::Posix::GetErrorAddr() == 123 &&
		             policy == marker && param.sched_priority == static_cast<int>(marker) &&
		             std::memcmp(reinterpret_cast<void*>(output.Address()), initial.data(), sizeof(initial)) == 0,
		             "invalid scheduling output returned the wrong error or changed errno or an output");
	};
	return rejected(nullptr, &policy, &param, Kernel::KERNEL_ERROR_ESRCH) &&
	       rejected(nullptr, mapped_policy, nullptr, Kernel::KERNEL_ERROR_ESRCH) &&
	       rejected(self, nullptr, mapped_param, Kernel::KERNEL_ERROR_EINVAL) &&
	       rejected(self, &policy, nullptr, Kernel::KERNEL_ERROR_EINVAL) &&
	       rejected(self, mapped_policy, &param, Kernel::KERNEL_ERROR_EFAULT) &&
	       rejected(self, &policy, mapped_param, Kernel::KERNEL_ERROR_EFAULT);
}

bool SchedulingInputs(size_t page_size)
{
	Mapping input(page_size * 2);
	if (!Check(input.Address() != 0, "scheduling input allocation failed")) return false;
	const Kernel::KernelSchedParam aligned {640}, unaligned {650};
	std::memcpy(reinterpret_cast<void*>(input.Address() + 1), &unaligned, sizeof(unaligned));
	std::memcpy(reinterpret_cast<void*>(input.Address() + page_size - 4), &aligned, sizeof(aligned));
	if (!Check(Memory::Protect(input.Address(), page_size, Memory::Mode::Read) &&
	           Memory::Protect(input.Address() + page_size, page_size, Memory::Mode::NoAccess),
	           "scheduling input protection failed")) return false;
	const auto self = Kernel::PthreadSelf();
	auto* tail = reinterpret_cast<const Kernel::KernelSchedParam*>(input.Address() + page_size - 4);
	for (uint32_t policy: {1U, 2U, 3U})
	{
		*Lumen::Libs::Posix::GetErrorAddr() = 123;
		if (!Check(Kernel::PthreadSetschedparam(self, policy, tail) == 0 &&
		           *Lumen::Libs::Posix::GetErrorAddr() == 123 && SchedulingMatches(self, policy, 640),
		           "setter rejected readable four-byte input, changed errno or lost state")) return false;
	}
	if (!Check(Kernel::PthreadSetschedparam(self, 3, reinterpret_cast<const Kernel::KernelSchedParam*>(input.Address() + 1)) == 0 &&
	           SchedulingMatches(self, 3, 650), "setter rejected unaligned input")) return false;
	const auto rejected = [&](Kernel::Pthread thread, uint32_t policy, const Kernel::KernelSchedParam* param, int expected) {
		*Lumen::Libs::Posix::GetErrorAddr() = 123;
		return Check(Kernel::PthreadSetschedparam(thread, policy, param) == expected &&
		             *Lumen::Libs::Posix::GetErrorAddr() == 123 && SchedulingMatches(self, 3, 650),
		             "invalid setter input changed errno or scheduling state");
	};
	if (!rejected(nullptr, 0, nullptr, Kernel::KERNEL_ERROR_ESRCH) ||
	    !rejected(self, 1, nullptr, Kernel::KERNEL_ERROR_EINVAL) ||
	    !rejected(self, 0, tail, Kernel::KERNEL_ERROR_EINVAL) ||
	    !rejected(self, UINT32_MAX, tail, Kernel::KERNEL_ERROR_EINVAL) ||
	    !rejected(self, 1, reinterpret_cast<const Kernel::KernelSchedParam*>(input.Address() + page_size - 2), Kernel::KERNEL_ERROR_EFAULT) ||
	    !rejected(self, 1, reinterpret_cast<const Kernel::KernelSchedParam*>(input.Address() + page_size), Kernel::KERNEL_ERROR_EFAULT) ||
	    !rejected(self, 1, reinterpret_cast<const Kernel::KernelSchedParam*>(UINTPTR_MAX - 1), Kernel::KERNEL_ERROR_EFAULT)) return false;
	const Kernel::KernelSchedParam restored {700};
	*Lumen::Libs::Posix::GetErrorAddr() = 123;
	if (!Check(Lumen::Libs::Posix::pthread_setschedparam(nullptr, 1, &restored) == Lumen::Libs::Posix::POSIX_ESRCH &&
	           Lumen::Libs::Posix::pthread_setschedparam(self, 0, &restored) == Lumen::Libs::Posix::POSIX_EINVAL &&
	           Lumen::Libs::Posix::pthread_setschedparam(self, 2, &restored) == 0 &&
	           *Lumen::Libs::Posix::GetErrorAddr() == 123 && SchedulingMatches(self, 2, 700),
	           "POSIX scheduling adapter did not preserve errno or return direct error numbers")) return false;
	return Check(Kernel::PthreadSetschedparam(self, 1, &restored) == 0 && SchedulingMatches(self, 1, 700),
	             "failed to restore main scheduling state");
}

bool CleanupValues(std::initializer_list<uint64_t> expected, const char* description)
{
	std::lock_guard lock(g_cleanup_mutex);
	if (g_cleanup_count != expected.size()) return Check(false, description);
	unsigned index = 0;
	for (const uint64_t value: expected)
		if (g_cleanup_order[index++] != value) return Check(false, description);
	return true;
}

bool CleanupHandlers(size_t page_size)
{
	Mapping storage(page_size), callback_code(page_size), pop_code(page_size), exit_code(page_size),
	        sentinel(page_size);
	if (!Check(storage.Address() != 0 && sentinel.Address() != 0,
	           "pthread cleanup storage allocation failed")) return false;
	auto* info = reinterpret_cast<Kernel::PthreadCleanupInfo*>(storage.Address());

	ResetCleanupValues();
	Kernel::PthreadCleanupPop(1);
	Kernel::PthreadCleanupPush(&ObservePthreadCleanup, reinterpret_cast<void*>(1), &info[0]);
	Kernel::PthreadCleanupPush(&ObservePthreadCleanup, reinterpret_cast<void*>(2), &info[1]);
	Kernel::PthreadCleanupPop(0);
	Kernel::PthreadCleanupPop(1);
	if (!CleanupValues({1}, "pthread cleanup pop execute/discard behavior differs")) return false;

	ResetCleanupValues();
	Kernel::PthreadCleanupPush(&ObservePthreadCleanup, reinterpret_cast<void*>(1), &info[0]);
	Kernel::PthreadCleanupPush(&ObservePthreadCleanup, reinterpret_cast<void*>(2), &info[1]);
	Kernel::PthreadCleanupPop(1);
	Kernel::PthreadCleanupPop(7);
	if (!CleanupValues({2, 1}, "pthread cleanup nesting is not LIFO")) return false;

	ResetCleanupValues();
	g_reentrant_cleanup_info = &info[3];
	Kernel::PthreadCleanupPush(&ObserveReentrantPthreadCleanup, reinterpret_cast<void*>(4), &info[2]);
	Kernel::PthreadCleanupPop(1);
	g_reentrant_cleanup_info = nullptr;
	if (!CleanupValues({4, 5}, "pthread cleanup callback could not reenter after removal")) return false;

	GuestCode callback;
	callback.Begin();
	callback.Call(reinterpret_cast<uint64_t>(&ObservePthreadCleanup));
	callback.Return();

	GuestCode pop;
	pop.Begin();
	pop.Bytes({0x48, 0xba}); pop.Word64(reinterpret_cast<uint64_t>(&info[4])); // movabs rdx, info
	pop.Bytes({0x48, 0xbe}); pop.Word64(6);                                   // movabs rsi, argument
	pop.Bytes({0x48, 0xbf}); pop.Word64(callback_code.Address());             // movabs rdi, callback
	pop.Call(reinterpret_cast<uint64_t>(&Kernel::PthreadCleanupPush));
	pop.Bytes({0xbf, 0x01, 0x00, 0x00, 0x00});                                // mov edi, 1
	pop.Call(reinterpret_cast<uint64_t>(&Kernel::PthreadCleanupPop));
	pop.Bytes({0x48, 0xb8}); pop.Word64(7);                                    // movabs rax, result
	pop.Return();

	GuestCode exit;
	exit.Begin();
	exit.Bytes({0x48, 0x89, 0xfb});                                            // mov rbx, rdi: info
	exit.Bytes({0x48, 0x89, 0xda});                                            // mov rdx, rbx
	exit.Bytes({0x48, 0xbe}); exit.Word64(1);                                  // movabs rsi, argument
	exit.Bytes({0x48, 0xbf}); exit.Word64(callback_code.Address());            // movabs rdi, callback
	exit.Call(reinterpret_cast<uint64_t>(&Kernel::PthreadCleanupPush));
	exit.Bytes({0x48, 0x8d, 0x53, 0x40});                                      // lea rdx, [rbx + 64]
	exit.Bytes({0x48, 0xbe}); exit.Word64(2);                                  // movabs rsi, argument
	exit.Bytes({0x48, 0xbf}); exit.Word64(callback_code.Address());            // movabs rdi, callback
	exit.Call(reinterpret_cast<uint64_t>(&Kernel::PthreadCleanupPush));
	exit.Bytes({0x48, 0xbf}); exit.Word64(0x5ca1ab1e);                         // movabs rdi, exit value
	exit.Call(reinterpret_cast<uint64_t>(&Kernel::PthreadExit));
	exit.ForbiddenContinuation(sentinel.Address());

	if (!Check(callback.Publish(callback_code) && pop.Publish(pop_code) && exit.Publish(exit_code),
	           "pthread cleanup guest fixtures could not be published")) return false;
	ResetCleanupValues();
	if (!Check(Loader::RunGuestX64(pop_code.Address(), 0) == 7,
	           "guest pthread cleanup push/pop did not return")) return false;
	if (!CleanupValues({6}, "guest pthread cleanup callback did not reenter FEX")) return false;

	ResetCleanupValues();
	Kernel::Pthread thread = nullptr;
	if (!Start(&thread, exit_code.Address(), storage.Address(),
	           "cleanup-exit") || !Join(thread, 0x5ca1ab1e)) return false;
	if (!Check(*reinterpret_cast<uint32_t*>(sentinel.Address()) == 0,
	           "guest continued after pthread exit with cleanup handlers") ||
	    !CleanupValues({2, 1}, "pthread exit did not run guest cleanup handlers in LIFO order")) return false;

	ResetCleanupValues();
	g_cleanup_cancel_ready.store(false, std::memory_order_relaxed);
	thread = nullptr;
	if (!Start(&thread, reinterpret_cast<uint64_t>(&WaitForPthreadCleanupCancellation),
	           storage.Address(), "cleanup-cancel")) return false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!g_cleanup_cancel_ready.load(std::memory_order_acquire) &&
	       std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::yield();
	}
	if (!Check(g_cleanup_cancel_ready.load(std::memory_order_acquire),
	           "pthread cleanup cancellation worker did not become ready") ||
	    !Check(Kernel::PthreadCancel(thread) == 0, "pthread cleanup cancellation request failed") ||
	    !Join(thread, reinterpret_cast<uint64_t>(PTHREAD_CANCELED))) return false;
	return CleanupValues({2, 1}, "pthread cancellation did not drain cleanup handlers once in LIFO order");
}

bool ThreadSpecificDestructors(size_t page_size)
{
	Mapping entry_code(page_size), destructor_code(page_size);
	GuestCode entry;
	entry.Begin(); entry.Call(reinterpret_cast<uint64_t>(&PrepareKeyDestructor)); entry.Return();
	GuestCode destructor;
	destructor.Begin(); destructor.Call(reinterpret_cast<uint64_t>(&ObserveKeyDestructor)); destructor.Return();
	if (!Check(entry.Publish(entry_code) && destructor.Publish(destructor_code),
	           "pthread key destructor guest fixtures could not be published")) return false;

	if (!Check(Kernel::PthreadKeyCreate(
	               &g_key_destructor.key,
	               reinterpret_cast<Kernel::pthread_key_destructor_func_t>(destructor_code.Address())) == 0,
	           "pthread key creation failed")) return false;

	bool passed = true;
	Kernel::PthreadSetExitTestHooks(ObserveDestructorPurge, nullptr);
	struct RestoreHooks { ~RestoreHooks() { Kernel::PthreadSetExitTestHooks(nullptr, nullptr); } } restore_hooks;
	const std::array<unsigned, 3> expected_calls {1, 2, 4};
	for (uint64_t raw_mode = 0; raw_mode < expected_calls.size(); ++raw_mode)
	{
		Kernel::Pthread thread = nullptr;
		if (!Start(&thread, entry_code.Address(), raw_mode, "key-destructor-normal-return") ||
		    !Join(thread, KEY_DESTRUCTOR_RESULT_BASE + raw_mode))
		{
			passed = false;
			break;
		}
		passed = Check(g_key_destructor.valid && g_key_destructor.purged &&
		               g_key_destructor.calls == expected_calls[raw_mode],
		               "pthread key destructor count, final purge, clearing or exiting-thread identity differed") && passed;
	}

	passed = Check(Kernel::PthreadKeyDelete(g_key_destructor.key) == 0,
	               "pthread key deletion failed") && passed;
	g_key_destructor.key = -1;
	return passed;
}

bool ThreadSpecificIsolationAndReuse(size_t page_size)
{
	Mapping entry_code(page_size);
	GuestCode entry;
	entry.Begin(); entry.Call(reinterpret_cast<uint64_t>(&ObserveSpecificReuse)); entry.Return();
	if (!Check(entry.Publish(entry_code), "pthread specific reuse guest fixture publication failed")) return false;
	Kernel::PthreadKey key = -1;
	if (!Check(Kernel::PthreadKeyCreate(&key, nullptr) == 0, "pthread specific reuse key creation failed")) return false;
	struct Cleanup
	{
		Kernel::PthreadKey& key;
		~Cleanup() { if (key >= 0) (void)Kernel::PthreadKeyDelete(key); }
	} cleanup {key};
	g_specific_reuse.key = key;
	g_specific_reuse.reached = 0;
	g_specific_reuse.phase = 0;
	g_specific_reuse.cleanups = 0;
	g_specific_reuse.purged = true;
	if (!Check(Kernel::PthreadGetspecific(key) == nullptr &&
	           Kernel::PthreadSetspecific(key, reinterpret_cast<void*>(SPECIFIC_MAIN_OLD)) == 0,
	           "fresh pthread specific key was not empty or set failed")) return false;
	Kernel::PthreadSetExitTestHooks(ObserveSpecificPurge, nullptr);
	struct RestoreHooks { ~RestoreHooks() { Kernel::PthreadSetExitTestHooks(nullptr, nullptr); } } restore_hooks;
	Kernel::Pthread thread = nullptr;
	if (!Start(&thread, entry_code.Address(), 0, "specific-reuse")) return false;
	const auto wait = [](unsigned phase) {
		std::unique_lock lock(g_specific_reuse.mutex);
		return g_specific_reuse.changed.wait_for(lock, std::chrono::seconds(2),
		    [phase] { return g_specific_reuse.reached >= phase; });
	};
	const auto advance = [](unsigned phase) {
		std::lock_guard lock(g_specific_reuse.mutex);
		g_specific_reuse.phase = phase;
		g_specific_reuse.changed.notify_all();
	};
	bool passed = Check(wait(1), "pthread specific worker did not install its value");
	if (passed)
	{
		passed = Check(reinterpret_cast<uint64_t>(Kernel::PthreadGetspecific(key)) == SPECIFIC_MAIN_OLD &&
		               Kernel::PthreadSetspecific(key, nullptr) == 0 && Kernel::PthreadGetspecific(key) == nullptr &&
		               Kernel::PthreadSetspecific(key, reinterpret_cast<void*>(SPECIFIC_MAIN_OLD)) == 0,
		               "pthread specific thread isolation or set-null differed") && passed;
		const auto retired = key;
		const bool deleted = Kernel::PthreadKeyDelete(key) == 0;
		if (deleted) key = -1;
		passed = Check(deleted && Kernel::PthreadGetspecific(retired) == nullptr &&
		               Kernel::PthreadSetspecific(retired, reinterpret_cast<void*>(SPECIFIC_MAIN_OLD)) ==
		                   Kernel::KERNEL_ERROR_EINVAL &&
		               Kernel::PthreadKeyDelete(retired) == Kernel::KERNEL_ERROR_EINVAL,
		               "deleted pthread key remained usable") && passed;
		if (deleted)
		{
			passed = Check(Kernel::PthreadKeyCreate(&key, nullptr) == 0 && key == retired &&
			               Kernel::PthreadGetspecific(key) == nullptr &&
			               Kernel::PthreadSetspecific(key, reinterpret_cast<void*>(SPECIFIC_MAIN_NEW)) == 0,
			               "recreated pthread key revived a prior value or changed slot") && passed;
		}
	}
	advance(1);
	passed = Check(wait(2), "pthread specific worker did not observe key reuse") && passed;
	if (key >= 0)
		passed = Check(reinterpret_cast<uint64_t>(Kernel::PthreadGetspecific(key)) == SPECIFIC_MAIN_NEW,
		               "worker overwrote the parent's recreated pthread specific value") && passed;
	advance(2);
	passed = Join(thread, SPECIFIC_REUSE_RESULT) && passed;
	if (!passed) return false;
	// A subsequent managed guest thread must start without the previous
	// worker's value even if the host recycles its native thread allocation.
	thread = nullptr;
	return Start(&thread, entry_code.Address(), 1, "specific-fresh-thread") && Join(thread, SPECIFIC_REUSE_RESULT) &&
	       Check(g_specific_reuse.cleanups == 2 && g_specific_reuse.purged,
	             "exiting native thread retained pthread specific state after cleanup");
}

bool CrossKeyThreadSpecificDestructors(size_t page_size)
{
	Mapping entry_code(page_size), destructor_code(page_size);
	GuestCode entry;
	entry.Begin(); entry.Call(reinterpret_cast<uint64_t>(&PrepareCrossKeyDestructors)); entry.Return();
	GuestCode destructor;
	destructor.Begin(); destructor.Call(reinterpret_cast<uint64_t>(&ObserveCrossKeyDestructor)); destructor.Return();
	if (!Check(entry.Publish(entry_code) && destructor.Publish(destructor_code),
	           "cross-key destructor guest fixtures could not be published")) return false;

	const auto guest_destructor =
	    reinterpret_cast<Kernel::pthread_key_destructor_func_t>(destructor_code.Address());
	if (!Check(Kernel::PthreadKeyCreate(&g_cross_key_destructor.key_a, guest_destructor) == 0 &&
	           Kernel::PthreadKeyCreate(&g_cross_key_destructor.key_b, guest_destructor) == 0 &&
	           g_cross_key_destructor.key_a < g_cross_key_destructor.key_b,
	           "cross-key destructor keys were not created in scan order")) return false;

	Kernel::Pthread thread = nullptr;
	const bool ran = Start(&thread, entry_code.Address(), 0, "key-destructor-cross-key") &&
	                 Join(thread, CROSS_KEY_RESULT);
	const bool observed = Check(ran && g_cross_key_destructor.valid &&
	                            g_cross_key_destructor.calls_a == 1 &&
	                            g_cross_key_destructor.calls_b == 1,
	                            "cross-key destructor did not replace and clear the later binding once");
	const bool deleted = Check(
	    Kernel::PthreadKeyDelete(g_cross_key_destructor.key_b) == 0 &&
	    Kernel::PthreadKeyDelete(g_cross_key_destructor.key_a) == 0,
	    "cross-key destructor key deletion failed");
	g_cross_key_destructor.key_a = -1;
	g_cross_key_destructor.key_b = -1;
	return observed && deleted;
}

bool ManagedExit(size_t page_size, uint64_t exit_target)
{
	// Only the positive parent gets a main-thread context; --unmanaged owns its
	// separate initialization and must retain the original rejection path.
	Kernel::PthreadInitSelfForMainThread();
	if (!CleanupHandlers(page_size) || !SchedulingOutputs(page_size) || !SchedulingInputs(page_size) ||
	    !ThreadSpecificIsolationAndReuse(page_size) || !ThreadSpecificDestructors(page_size) ||
	    !CrossKeyThreadSpecificDestructors(page_size)) return false;
	const Kernel::KernelSchedParam parent_priority {620};
	if (!Check(Kernel::PthreadSetschedparam(Kernel::PthreadSelf(), 2, &parent_priority) == 0,
	           "failed to update creator scheduling before inheritance")) return false;
	std::array<SchedulingAttributes, 2> attributes;
	if (!attributes[0].Configure(4) || !attributes[1].Configure(0)) return false;
	Mapping guest_stack(STACK_ATTRIBUTE_STORAGE_SIZE), exit_code(page_size), cleanup_code(page_size), normal_code(page_size),
	        sentinel(page_size);
	if (!Check(sentinel.Address() != 0, "sentinel allocation failed")) return false;
	auto* const guest_stack_addr = reinterpret_cast<void*>(guest_stack.Address());
	if (!Check(guest_stack_addr != nullptr, "guest stack metadata allocation failed") ||
	    !attributes[0].ConfigureStack(guest_stack_addr)) return false;
	auto* forbidden = reinterpret_cast<uint32_t*>(sentinel.Address());
	*forbidden = 0;
	GuestCode exit;
	exit.Begin();
	exit.Call(reinterpret_cast<uint64_t>(&ObserveEntry));
	exit.Bytes({0x48, 0x89, 0xc7}); // mov rdi, rax: use the observer's token as the exit value
	exit.Call(exit_target);
	exit.ForbiddenContinuation(sentinel.Address());
	GuestCode cleanup;
	cleanup.Begin(); cleanup.Call(reinterpret_cast<uint64_t>(&ObserveCleanup)); cleanup.Return();
	GuestCode normal;
	normal.Begin(); normal.Call(reinterpret_cast<uint64_t>(&ObserveEntry)); normal.Return();
	if (!Check(exit.Publish(exit_code) && cleanup.Publish(cleanup_code) && normal.Publish(normal_code),
	           "guest fixture publication failed")) return false;

	g_records[0].value = 0;
	g_records[0].expected_policy = 2;
	g_records[0].expected_priority = 620;
	g_records[1].value = reinterpret_cast<uint64_t>(&g_token);
	g_records[1].expected_policy = 3;
	g_records[1].expected_priority = 600;
	g_records[2].value = 0x12345678;
	g_records[2].expected_policy = 2;
	g_records[2].expected_priority = 620;
	// This is the existing guest thread_dtors callback, not PthreadKeys::Destruct
	// in the joiner. Running guest code here also exercises FEX again on the
	// exiting host thread after the previous run has released its state.
	Kernel::KernelSetThreadDtors(reinterpret_cast<Kernel::thread_dtors_func_t>(cleanup_code.Address()));
	std::array<Kernel::Pthread, 2> threads {};
	unsigned started = 0;
	for (; started < threads.size(); ++started)
		if (!Start(&threads[started], exit_code.Address(), started, "exit-synthetic", attributes[started].Address())) break;
	bool passed = started == threads.size();
	if (started != 0) passed = ThreadStackAttributesMatch(threads[0], guest_stack_addr) && passed;
	for (unsigned index = 0; index < started; ++index)
		passed = Join(threads[index], g_records[index].value) && passed;
	if (!passed) return false;
	if (!Check(g_records[0].host_stack_size >= MACOS_FEXCORE_HOST_STACK_FLOOR,
	           "small guest stack request shrank the native FEX/HLE pthread stack")) return false;
	if (!Check(*forbidden == 0, "guest continued after pthread exit")) return false;

	const uint64_t generation = Memory::GetMutationGeneration();
	if (!Check(exit_code.Protect(Memory::Mode::ReadWrite) && exit_code.Free() &&
	           Memory::GetMutationGeneration() != generation, "managed mutation after exit failed or stalled")) return false;
	Kernel::Pthread normal_thread = nullptr;
	if (!Start(&normal_thread, normal_code.Address(), 2, "return-after-exit") ||
	    !Join(normal_thread, g_records[2].value)) return false;
	if (!Check(Kernel::PthreadJoin(normal_thread, nullptr) == Kernel::KERNEL_ERROR_ESRCH,
	           "joining an already-reaped thread did not fail without waiting")) return false;
	for (auto& record: g_records)
		if (!Check(record.entries == 1 && record.cleanups == 1 && record.valid,
		           "entry/guest cleanup count, thread identity, managed boundary or scheduling snapshot was wrong")) return false;
	return Check(*forbidden == 0 && !Kernel::PthreadHasManagedExitBoundary(),
	             "exit altered the caller's boundary or executed the forbidden continuation") &&
	       Check(guest_stack.Free() && normal_code.Free() && cleanup_code.Free() && sentinel.Free(), "final managed mapping cleanup failed");
}

int RejectedExit(const char* mode, size_t page_size, uint64_t exit_target)
{
	Mapping inner(page_size), outer(page_size), sentinel(page_size);
	if (sentinel.Address() == 0) return 1;
	GuestCode exit;
	exit.Begin(); exit.Call(exit_target); exit.ForbiddenContinuation(sentinel.Address());
	if (!exit.Publish(inner)) return 1;
	if (std::strcmp(mode, "--unmanaged") == 0)
	{
		Kernel::PthreadInitSelfForMainThread();
		if (!Check(!Kernel::PthreadHasManagedExitBoundary(), "main thread unexpectedly has a managed boundary")) return 1;
		Loader::RunGuestX64(inner.Address(), 0x1234);
	} else
	{
		GuestCode nested;
		nested.Begin(); nested.Call(reinterpret_cast<uint64_t>(&EnterNestedGuest));
		nested.ForbiddenContinuation(sentinel.Address());
		if (!nested.Publish(outer)) return 1;
		Kernel::Pthread thread = nullptr;
		if (!Start(&thread, outer.Address(), inner.Address(), "nested-exit")) return 1;
		void* result = nullptr;
		Kernel::PthreadJoin(thread, &result);
	}
	std::fputs("PthreadExitTest: unsupported guest exit returned\n", stderr);
	return 3;
}

bool RejectedSubprocess(const char* executable, const char* mode, const char* expected)
{
	int descriptors[2];
	if (!Check(pipe(descriptors) == 0, "stderr pipe failed")) return false;
	posix_spawn_file_actions_t actions;
	int error = posix_spawn_file_actions_init(&actions);
	if (error != 0) { close(descriptors[0]); close(descriptors[1]); return Check(false, "spawn actions failed"); }
	error = posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO);
	if (error == 0) error = posix_spawn_file_actions_addclose(&actions, descriptors[0]);
	if (error == 0) error = posix_spawn_file_actions_addclose(&actions, descriptors[1]);
	if (error == 0) error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
	pid_t child = -1;
	char* arguments[] = {const_cast<char*>(executable), const_cast<char*>(mode), nullptr};
	if (error == 0) error = posix_spawn(&child, executable, &actions, nullptr, arguments, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(descriptors[1]);
	if (error != 0) { close(descriptors[0]); return Check(false, "child spawn failed"); }
	const int flags = fcntl(descriptors[0], F_GETFL, 0);
	if (flags < 0 || fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) < 0)
	{
		kill(child, SIGKILL); waitpid(child, nullptr, 0); close(descriptors[0]);
		return Check(false, "nonblocking stderr failed");
	}
	std::string output;
	auto drain = [&] {
		char buffer[1024];
		for (;;)
		{
			const ssize_t count = read(descriptors[0], buffer, sizeof(buffer));
			if (count < 0 && errno == EINTR) continue;
			if (count <= 0) break;
			output.append(buffer, static_cast<size_t>(count));
			if (output.size() > 8192) output.erase(0, output.size() - 8192);
		}
	};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
	int status = 0;
	bool finished = false;
	while (std::chrono::steady_clock::now() < deadline)
	{
		drain();
		const pid_t waited = waitpid(child, &status, WNOHANG);
		if (waited == child) { finished = true; break; }
		if (waited < 0 && errno != EINTR) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	if (!finished) { kill(child, SIGKILL); while (waitpid(child, &status, 0) < 0 && errno == EINTR) {} }
	drain();
	close(descriptors[0]);
	const bool passed = finished && WIFEXITED(status) && WEXITSTATUS(status) == 74 &&
	                    output.find(expected) != std::string::npos &&
	                    output.find("[MACOS_FEXCORE_FAIL_CLOSED_V1] code=8 exit=74") != std::string::npos;
	if (!passed) std::fprintf(stderr, "PthreadExitTest: %s rejected incorrectly (status=%d)\n%s", mode, status, output.c_str());
	return passed;
}
} // namespace

int main(int argc, char** argv)
{
	const bool child = argc == 2 && (std::strcmp(argv[1], "--nested") == 0 || std::strcmp(argv[1], "--unmanaged") == 0);
	if (argc != 1 && !child) return 2;
	Watchdog watchdog(child ? 10 : 35);
	if (setenv("LUMEN_MACOS_FEXCORE_RUN", "1", 1) != 0) return 1;
	if (!child)
	{
		std::array<char, 4096> executable {};
		uint32_t size = executable.size();
		if (!Check(_NSGetExecutablePath(executable.data(), &size) == 0, "executable path unavailable") ||
		    !RejectedSubprocess(executable.data(), "--nested", "FEX pthread exit unsupported: depth=2 managed=1") ||
		    !RejectedSubprocess(executable.data(), "--unmanaged", "FEX pthread exit unsupported: depth=1 managed=0")) return 1;
	}
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Memory::Init();
	Kernel::PthreadSubsystem::Instance()->Init(nullptr);
	LumenAndroidFexCoreRuntimeStatusV1 status {};
	if (!Check(LumenAndroidFexCoreRuntimeProbeV1(&status) == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
	           status.backendRouteReady != 0, "real FEX runtime unavailable")) return 1;
	const long page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) return 1;
	// PthreadJoin obtains this same production singleton to release thread TLS.
	auto* linker = Lumen::Core::Singleton<Loader::RuntimeLinker>::Instance();
	uint64_t exit_target = 0;
	if (!Initialize(*linker, &exit_target)) { linker->Clear(); return 1; }
	const int result = child ? RejectedExit(argv[1], static_cast<size_t>(page_size), exit_target)
	                         // Run the pool fixtures before ManagedExit registers
	                         // its guest cleanup callback through the one-shot API.
	                         : (Kernel::PthreadBackingAllocationContractTest() &&
	                            ContendedBackingAllocation(static_cast<size_t>(page_size)) &&
	                            ContendedMutexInitialization(static_cast<size_t>(page_size)) &&
	                            JoinOwnership(static_cast<size_t>(page_size)) &&
	                            ContendedJoinRetiresFreshTlsRegistry(*linker, static_cast<size_t>(page_size)) &&
	                            ManagedExit(static_cast<size_t>(page_size), exit_target) ? 0 : 1);
	linker->Clear();
	if (result != 0 || child) return result;
	std::puts("PthreadExitTest: PASS");
	return 0;
}
