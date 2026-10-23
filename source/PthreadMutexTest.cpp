#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#include "cpu/runtime/GuestCpuRuntime.h"
#endif
#if defined(__APPLE__) && defined(LUMEN_FEXCORE_RUNTIME)
#include "AndroidFexCoreRuntime.h"
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID) && defined(__aarch64__)
#include <unistd.h>
#endif

// Private fixture hooks, compiled only into the existing pthread test build.
namespace Lumen::Libs::LibKernel {
void PthreadSetMutexWaitTestHook(void (*hook)());
void PthreadSetMutexReadmissionTestHook(void (*hook)());
void PthreadSetMutexWakeTestHook(void (*hook)(bool));
void PthreadSetMutexWaitArmedTestHook(void (*hook)());
void PthreadSetMutexBeforeEnrollTestHook(void (*hook)());
void PthreadSetMutexWaitSliceForTest(uint32_t usec);
bool PthreadMutexQueuedAcquisitionForTest();
bool PthreadMutexHeadWakeForTest();
}

namespace {

namespace Kernel = Lumen::Libs::LibKernel;
namespace Posix = Lumen::Libs::Posix;
namespace Memory = Lumen::Core::VirtualMemory;
using TimedlockFunction = decltype(&Kernel::PthreadMutexTimedlock);
TimedlockFunction g_timedlock = &Kernel::PthreadMutexTimedlock;
using PosixCondTimedwaitFunction = int (LUMEN_SYSV_ABI*)(
    Kernel::PthreadCond*, Kernel::PthreadMutex*, const Kernel::KernelTimespec*);
PosixCondTimedwaitFunction g_posix_cond_timedwait = nullptr;
using PthreadRenameFunction = int (LUMEN_SYSV_ABI*)(Kernel::Pthread, const char*);
PthreadRenameFunction g_pthread_rename = nullptr;
constexpr int k_errno_sentinel = 193;

// These synthetic tests exercise Lumen's local relative-microsecond contract.
// They do not qualify the independently unverified guest adapter behavior.
static_assert(sizeof(Kernel::PthreadMutex) == 8 && sizeof(int) == 4 && sizeof(uint64_t) == 8);

// A regression to a host NORMAL mutex can block the owning thread forever.
// Keep the standalone executable bounded even when it is run outside CTest.
class Watchdog final
{
public:
	Watchdog(): m_thread([this] {
		std::unique_lock<std::mutex> lock(m_mutex);
		if (!m_changed.wait_for(lock, std::chrono::seconds(5), [this] { return m_done; }))
		{
			std::fprintf(stderr, "PthreadMutexTest: FAIL: mutex operation timed out\n");
			std::fflush(stderr);
			std::_Exit(2);
		}
	}) {}

	~Watchdog()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_done = true;
		}
		m_changed.notify_one();
		m_thread.join();
	}

	Watchdog(const Watchdog&)            = delete;
	Watchdog& operator=(const Watchdog&) = delete;

private:
	std::mutex              m_mutex;
	std::condition_variable m_changed;
	bool                    m_done = false;
	std::thread             m_thread;
};

bool Expect(int actual, int expected, int type, const char* operation)
{
	if (actual == expected)
	{
		return true;
	}
	std::fprintf(stderr, "PthreadMutexTest: type=%d %s returned 0x%08x, expected 0x%08x\n",
	             type, operation, static_cast<unsigned>(actual), static_cast<unsigned>(expected));
	return false;
}

bool Check(bool condition, const char* description)
{
	if (!condition) std::fprintf(stderr, "PthreadMutexTest: %s\n", description);
	return condition;
}

bool TimedCall(Kernel::PthreadMutex* mutex, uint64_t usec, int expected, int type, const char* operation)
{
	*Posix::GetErrorAddr() = k_errno_sentinel;
	const int result = g_timedlock(mutex, usec);
	return Expect(result, expected, type, operation) &&
	       Check(*Posix::GetErrorAddr() == k_errno_sentinel, "kernel timedlock changed guest errno");
}

bool TestTimedlockBinding()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!Lumen::Libs::Init(U"libkernel_1", &symbols))
	{
		std::fputs("PthreadMutexTest: libkernel registration failed\n", stderr);
		return false;
	}
	Lumen::Loader::SymbolResolve exact {};
	exact.name = "IafI2PxcPnQ";
	exact.library = "libkernel";
	exact.library_version = 1;
	exact.module = "libkernel";
	exact.module_version_major = 1;
	exact.module_version_minor = 1;
	exact.type = Lumen::Loader::SymbolType::Func;
	const auto* symbol = symbols.Find(exact);
	if (symbol == nullptr || symbol->vaddr != reinterpret_cast<uint64_t>(&Kernel::PthreadMutexTimedlock))
	{
		std::fputs("PthreadMutexTest: timedlock export is missing\n", stderr);
		return false;
	}
	auto posix = exact;
	posix.name = "27bAgiJmOh0";
	posix.library = "Posix";
	const auto* posix_symbol = symbols.Find(posix);
	if (posix_symbol == nullptr)
	{
		std::fputs("PthreadMutexTest: POSIX condition timedwait export is missing\n", stderr);
		return false;
	}
	g_posix_cond_timedwait =
	    reinterpret_cast<PosixCondTimedwaitFunction>(posix_symbol->vaddr);
	auto rename = exact;
	rename.name = "GBUY7ywdULE";
	const auto* rename_symbol = symbols.Find(rename);
	const auto* rename_scalar = rename_symbol == nullptr ? nullptr :
	    symbols.HostCallScalarMetadataFor(rename_symbol->vaddr);
	if (rename_symbol == nullptr || rename_scalar == nullptr || rename_scalar->arity != 2)
	{
		std::fputs("PthreadMutexTest: pthread rename export or scalar ABI is missing\n", stderr);
		return false;
	}
	g_pthread_rename = reinterpret_cast<PthreadRenameFunction>(rename_symbol->vaddr);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && defined(__aarch64__)
	auto exit_query = exact;
	exit_query.name = "3kg7rT0NQIs";
	const auto* exit_symbol = symbols.Find(exit_query);
	if (exit_symbol == nullptr ||
	    exit_symbol->vaddr != reinterpret_cast<uint64_t>(&Kernel::PthreadExit))
	{
		std::fputs("PthreadMutexTest: Android FEX pthread-exit export is missing\n",
		           stderr);
		return false;
	}
#endif
	for (unsigned mismatch = 0; mismatch < 6; ++mismatch)
	{
		auto wrong = exact;
		switch (mismatch)
		{
			case 0: wrong.library = "unrelated_mutex_library"; break;
			case 1: wrong.module = "unrelated_kernel_module"; break;
			case 2: wrong.library_version = 2; break;
			case 3: wrong.module_version_major = 2; break;
			case 4: wrong.module_version_minor = 0; break;
			case 5: wrong.type = Lumen::Loader::SymbolType::Object; break;
		}
		if (symbols.Find(wrong) != nullptr)
		{
			std::fputs("PthreadMutexTest: timedlock export matched an unsupported identity\n", stderr);
			return false;
		}
	}
	g_timedlock = reinterpret_cast<TimedlockFunction>(symbol->vaddr);
	return true;
}

bool TestThreadRename()
{
	constexpr size_t page = 16384;
	const uint64_t mapping = Memory::Alloc(0, page, Memory::Mode::ReadWrite);
	if (!Check(mapping != 0, "pthread rename guest mapping failed")) return false;
	struct MappingOwner
	{
		uint64_t address;
		~MappingOwner() { (void)Memory::Free(address); }
	} owner {mapping};

	Kernel::PthreadInitSelfForMainThread();
	Kernel::Pthread self = Kernel::PthreadSelf();
	auto* input = reinterpret_cast<char*>(mapping + 64);
	auto* output = reinterpret_cast<char*>(mapping + 256);
	std::memcpy(input, "LumenWorker", sizeof("LumenWorker"));
	std::memset(output, 0xa5, 32);
	bool ok = Expect(g_pthread_rename(self, input), 0, 0, "pthread rename") &&
	          Expect(Kernel::PthreadGetname(self, output), 0, 0, "pthread renamed getname") &&
	          Check(std::strcmp(output, "LumenWorker") == 0, "pthread rename was not guest-visible");

	std::memset(input, 'x', 32);
	input[32] = '\0';
	ok = Expect(g_pthread_rename(self, input), Kernel::KERNEL_ERROR_ENAMETOOLONG,
	            0, "pthread overlong rename") && ok;
	std::memset(output, 0xa5, 32);
	ok = Expect(Kernel::PthreadGetname(self, output), 0, 0, "pthread name after rejected rename") &&
	     Check(std::strcmp(output, "LumenWorker") == 0, "rejected rename changed the logical name") && ok;
	ok = Expect(g_pthread_rename(nullptr, input), Kernel::KERNEL_ERROR_ESRCH,
	            0, "pthread rename null thread") &&
	     Expect(g_pthread_rename(self, nullptr), Kernel::KERNEL_ERROR_EINVAL,
	            0, "pthread rename null name") && ok;

	if (!Memory::Protect(mapping, page, Memory::Mode::NoAccess))
		return Check(false, "pthread rename guard setup failed");
	const int inaccessible = g_pthread_rename(self, input);
	if (!Memory::Protect(mapping, page, Memory::Mode::ReadWrite))
		return Check(false, "pthread rename guard restore failed");
	ok = Expect(inaccessible, Kernel::KERNEL_ERROR_EFAULT,
	            0, "pthread rename inaccessible name") && ok;

	std::memcpy(input, "MainThread", sizeof("MainThread"));
	return Expect(g_pthread_rename(self, input), 0, 0, "pthread restore name") && ok;
}

bool CheckOtherThreadExcluded(Kernel::PthreadMutex* mutex, int type)
{
	int try_before = 0;
	int unlock     = 0;
	int try_after  = 0;
	std::thread worker([&] {
		try_before = Kernel::PthreadMutexTrylock(mutex);
		unlock     = Kernel::PthreadMutexUnlock(mutex);
		try_after  = Kernel::PthreadMutexTrylock(mutex);
	});
	worker.join();
	return Expect(try_before, Kernel::KERNEL_ERROR_EBUSY, type, "other-thread trylock") &&
	       Expect(unlock, Kernel::KERNEL_ERROR_EPERM, type, "foreign-owner unlock") &&
	       Expect(try_after, Kernel::KERNEL_ERROR_EBUSY, type, "trylock after rejected unlock");
}

bool TestType(int type)
{
	Kernel::PthreadMutexattr attr  = nullptr;
	Kernel::PthreadMutex     mutex = nullptr;
	// Type zero exercises the default attributes owned by the subsystem.
	if (type != 0)
	{
		if (!Expect(Kernel::PthreadMutexattrInit(&attr), 0, type, "attribute init") ||
		    !Expect(Kernel::PthreadMutexattrSettype(&attr, type), 0, type, "attribute type"))
		{
			return false;
		}
	}
	if (!Expect(Kernel::PthreadMutexInit(&mutex, type == 0 ? nullptr : &attr, nullptr), 0, type, "mutex init"))
	{
		return false;
	}
	if (attr != nullptr && !Expect(Kernel::PthreadMutexattrDestroy(&attr), 0, type, "attribute destroy"))
	{
		return false;
	}

	const bool recursive = type == 2;
	if (!Expect(Kernel::PthreadMutexLock(&mutex), 0, type, "first lock") ||
	    !Expect(Kernel::PthreadMutexLock(&mutex), recursive ? 0 : Kernel::KERNEL_ERROR_EDEADLK, type, "self lock") ||
	    !Expect(Kernel::PthreadMutexTrylock(&mutex), recursive ? 0 : Kernel::KERNEL_ERROR_EBUSY, type, "self trylock"))
	{
		return false;
	}

	// A recursive lock and trylock each add one level. Failed non-recursive
	// operations must leave exactly the original level owned by this thread.
	const int depth = recursive ? 3 : 1;
	for (int remaining = depth; remaining > 0; --remaining)
	{
		if (!CheckOtherThreadExcluded(&mutex, type) ||
		    !Expect(Kernel::PthreadMutexUnlock(&mutex), 0, type, "owner unlock"))
		{
			return false;
		}
	}

	int acquired = -1;
	int released = -1;
	std::thread worker([&] {
		acquired = Kernel::PthreadMutexLock(&mutex);
		if (acquired == 0)
		{
			released = Kernel::PthreadMutexUnlock(&mutex);
		}
	});
	worker.join();
	if (!Expect(acquired, 0, type, "other-thread lock after final release") ||
	    !Expect(released, 0, type, "other-thread unlock") ||
	    !Expect(Kernel::PthreadMutexUnlock(&mutex), Kernel::KERNEL_ERROR_EPERM, type, "unlock while unowned") ||
	    !Expect(Kernel::PthreadMutexTrylock(&mutex), 0, type, "trylock after rejected unowned unlock") ||
	    !Expect(Kernel::PthreadMutexUnlock(&mutex), 0, type, "final unlock") ||
	    !Expect(Kernel::PthreadMutexDestroy(&mutex), 0, type, "mutex destroy"))
	{
		return false;
	}
	return true;
}

bool InitMutex(Kernel::PthreadMutex* mutex, int type)
{
	Kernel::PthreadMutexattr attr = nullptr;
	if (type != 0 &&
	    (!Expect(Kernel::PthreadMutexattrInit(&attr), 0, type, "timed attribute init") ||
	     !Expect(Kernel::PthreadMutexattrSettype(&attr, type), 0, type, "timed attribute type"))) return false;
	const bool initialized = Expect(Kernel::PthreadMutexInit(mutex, type == 0 ? nullptr : &attr, nullptr),
	                                0, type, "timed mutex init");
	const bool destroyed = attr == nullptr || Expect(Kernel::PthreadMutexattrDestroy(&attr), 0, type, "timed attribute destroy");
	return initialized && destroyed;
}

bool TestTimedType(int type)
{
	Kernel::PthreadMutex mutex = nullptr;
	if (!InitMutex(&mutex, type) || !TimedCall(&mutex, 0, 0, type, "unowned zero-time lock")) return false;
	const bool recursive = type == 2;
	if (!TimedCall(&mutex, std::numeric_limits<uint64_t>::max(), recursive ? 0 : Kernel::KERNEL_ERROR_EDEADLK,
	               type, "self timedlock with maximum timeout") ||
	    !TimedCall(&mutex, 0, recursive ? 0 : Kernel::KERNEL_ERROR_EDEADLK, type, "self zero-time lock")) return false;
	for (int depth = recursive ? 3 : 1; depth > 0; --depth)
	{
		if (!CheckOtherThreadExcluded(&mutex, type) ||
		    !Expect(Kernel::PthreadMutexUnlock(&mutex), 0, type, "timed owner unlock")) return false;
	}
	return Expect(Kernel::PthreadMutexUnlock(&mutex), Kernel::KERNEL_ERROR_EPERM, type, "timed unowned unlock") &&
	       TimedCall(&mutex, 0, 0, type, "timed lock after rejected unlock") &&
	       Expect(Kernel::PthreadMutexUnlock(&mutex), 0, type, "timed final unlock") &&
	       Expect(Kernel::PthreadMutexDestroy(&mutex), 0, type, "timed mutex destroy");
}

bool TestContendedTimeout(int type)
{
	Kernel::PthreadMutex mutex = nullptr;
	if (!InitMutex(&mutex, type) || !Expect(Kernel::PthreadMutexLock(&mutex), 0, type, "timeout owner lock")) return false;
	bool worker_ok = false;
	std::thread worker([&] {
		worker_ok = TimedCall(&mutex, 0, Kernel::KERNEL_ERROR_ETIMEDOUT, type, "contended zero-time lock");
		const auto start = std::chrono::steady_clock::now();
		worker_ok = TimedCall(&mutex, 30000, Kernel::KERNEL_ERROR_ETIMEDOUT, type, "contended relative timeout") && worker_ok;
		const auto elapsed = std::chrono::steady_clock::now() - start;
		worker_ok = Check(elapsed >= std::chrono::milliseconds(20), "relative timeout expired too early") && worker_ok;
		worker_ok = Expect(Kernel::PthreadMutexTrylock(&mutex), Kernel::KERNEL_ERROR_EBUSY, type,
		                   "trylock after timeout") && worker_ok;
		worker_ok = Expect(Kernel::PthreadMutexUnlock(&mutex), Kernel::KERNEL_ERROR_EPERM, type,
		                   "foreign unlock after timeout") && worker_ok;
	});
	worker.join();
	const bool still_owned = CheckOtherThreadExcluded(&mutex, type);
	const bool cleanup = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, type, "timeout owner release") &&
	                     TimedCall(&mutex, 0, 0, type, "timeout did not consume later ownership") &&
	                     Expect(Kernel::PthreadMutexUnlock(&mutex), 0, type, "timeout final release") &&
	                     Expect(Kernel::PthreadMutexDestroy(&mutex), 0, type, "timeout mutex destroy");
	return worker_ok && still_owned && cleanup;
}

bool TestReleaseBeforeTimeout(int type, uint64_t timeout)
{
	Kernel::PthreadMutex mutex = nullptr;
	if (!InitMutex(&mutex, type) || !Expect(Kernel::PthreadMutexLock(&mutex), 0, type, "delayed-release owner lock")) return false;
	std::atomic<bool> started {false};
	std::atomic<bool> finished {false};
	bool worker_ok = false;
	std::thread worker([&] {
		started.store(true, std::memory_order_release);
		worker_ok = TimedCall(&mutex, timeout, 0, type, "lock acquired before timeout");
		if (worker_ok)
		{
			worker_ok = TimedCall(&mutex, 0, type == 2 ? 0 : Kernel::KERNEL_ERROR_EDEADLK,
			                     type, "delayed acquirer owns mutex");
			if (type == 2) worker_ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, type, "delayed recursive release") && worker_ok;
			worker_ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, type, "delayed acquirer release") && worker_ok;
		}
		finished.store(true, std::memory_order_release);
	});
	while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	const bool waited = Check(!finished.load(std::memory_order_acquire), "wide timeout narrowed or overflowed before release");
	const bool released = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, type, "delayed owner release");
	worker.join();
	return Expect(Kernel::PthreadMutexDestroy(&mutex), 0, type, "delayed mutex destroy") && waited && released && worker_ok;
}

bool TestConditionInterop(int type)
{
	Kernel::PthreadMutex mutex = nullptr;
	Kernel::PthreadCond condition = nullptr;
	if (!InitMutex(&mutex, type) || !Expect(Kernel::PthreadCondInit(&condition, nullptr, nullptr), 0, type, "interop condition init") ||
	    !TimedCall(&mutex, 0, 0, type, "interop timed lock")) return false;
	const bool recursive = type == 2;
	if (recursive && !Expect(Kernel::PthreadMutexLock(&mutex), 0, type, "interop second recursive level")) return false;
	if (!Expect(Kernel::PthreadCondTimedwait(&condition, &mutex, 2000), Kernel::KERNEL_ERROR_ETIMEDOUT,
	            type, "condition timeout") ||
	    !TimedCall(&mutex, 0, recursive ? 0 : Kernel::KERNEL_ERROR_EDEADLK, type, "ownership after condition timeout")) return false;
	for (int depth = recursive ? 3 : 1; depth > 0; --depth)
	{
		if (!CheckOtherThreadExcluded(&mutex, type) ||
		    !Expect(Kernel::PthreadMutexUnlock(&mutex), 0, type, "condition-timeout owner release")) return false;
	}
	if (!Expect(Kernel::PthreadMutexLock(&mutex), 0, type, "condition-wake owner lock")) return false;
	if (!recursive)
	{
		bool foreign_ok = false;
		std::thread foreign([&] {
			foreign_ok = Expect(Kernel::PthreadCondTimedwait(&condition, &mutex, 0), Kernel::KERNEL_ERROR_EPERM,
			                    type, "foreign condition timeout") &&
			             TimedCall(&mutex, 0, Kernel::KERNEL_ERROR_ETIMEDOUT, type, "foreign condition left owner intact");
		});
		foreign.join();
		if (!foreign_ok || !TimedCall(&mutex, 0, Kernel::KERNEL_ERROR_EDEADLK, type, "owner after rejected condition wait")) return false;
	}
	bool release = false;
	bool signaler_ok = false;
	std::thread signaler([&] {
		signaler_ok = TimedCall(&mutex, 500000, 0, type, "condition signaler timed acquisition");
		if (signaler_ok)
		{
			release = true;
			signaler_ok = Expect(Kernel::PthreadCondSignal(&condition), 0, type, "interop condition signal");
			signaler_ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, type, "condition signaler release") && signaler_ok;
		}
	});
	bool waiter_ok = true;
	while (!release)
	{
		if (!Expect(Kernel::PthreadCondWait(&condition, &mutex), 0, type, "interop condition wait"))
		{
			waiter_ok = false;
			break;
		}
	}
	signaler.join();
	const bool owned = TimedCall(&mutex, 0, recursive ? 0 : Kernel::KERNEL_ERROR_EDEADLK, type, "ownership after condition wake") &&
	                   CheckOtherThreadExcluded(&mutex, type);
	if (recursive && !Expect(Kernel::PthreadMutexUnlock(&mutex), 0, type, "condition-wake recursive release")) return false;
	const bool cleanup = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, type, "condition-wake owner release") &&
	                     Expect(Kernel::PthreadCondDestroy(&condition), 0, type, "interop condition destroy") &&
	                     Expect(Kernel::PthreadMutexDestroy(&mutex), 0, type, "interop mutex destroy");
	return waiter_ok && signaler_ok && owned && cleanup;
}

bool TestMixedHandoffs(int type)
{
	Kernel::PthreadMutex mutex = nullptr;
	if (!InitMutex(&mutex, type)) return false;
	constexpr unsigned rounds = 100;
	std::atomic<bool> operations_ok {true};
	unsigned count = 0;
	std::array<std::thread, 3> workers;
	for (unsigned worker = 0; worker < workers.size(); ++worker)
	{
		workers[worker] = std::thread([&, worker] {
			for (unsigned round = 0; round < rounds && operations_ok.load(std::memory_order_relaxed); ++round)
			{
				int result = 0;
				switch ((worker + round) % 3)
				{
					case 0: result = Kernel::PthreadMutexLock(&mutex); break;
					case 1: result = g_timedlock(&mutex, 500000); break;
					case 2:
						do
						{
							result = Kernel::PthreadMutexTrylock(&mutex);
							if (result == Kernel::KERNEL_ERROR_EBUSY) std::this_thread::yield();
						} while (result == Kernel::KERNEL_ERROR_EBUSY);
						break;
				}
				if (!Expect(result, 0, type, "mixed handoff acquisition")) { operations_ok = false; break; }
				++count;
				bool owned = TimedCall(&mutex, 0, type == 2 ? 0 : Kernel::KERNEL_ERROR_EDEADLK, type, "mixed handoff owner");
				if (type == 2) owned = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, type, "mixed recursive release") && owned;
				owned = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, type, "mixed handoff release") && owned;
				if (!owned) { operations_ok = false; break; }
			}
		});
	}
	for (auto& worker: workers) worker.join();
	return Expect(Kernel::PthreadMutexDestroy(&mutex), 0, type, "mixed mutex destroy") && operations_ok &&
	       Check(count == workers.size() * rounds, "mixed handoffs lost protected updates");
}

bool TestTimedErrors()
{
	Kernel::PthreadMutex absent = nullptr;
	return TimedCall(nullptr, 0, Kernel::KERNEL_ERROR_EINVAL, 0, "null pointer slot") &&
	       TimedCall(&absent, 0, Kernel::KERNEL_ERROR_EINVAL, 0, "untracked null handle") &&
	       Check(absent == nullptr, "rejected null handle allocated an untracked mutex");
}

struct PausedMutexWaiter
{
	std::mutex mutex;
	std::condition_variable condition;
	bool entered = false, released = false;
	bool verify_donated_reader = false, donated_reader = false;
	Kernel::PthreadMutex* modelled_reentry_slot = nullptr;
	bool modelled_reentry_succeeded = false;
	bool WaitUntilEntered()
	{
		std::unique_lock lock(mutex);
		return condition.wait_for(lock, std::chrono::seconds(2), [&] { return entered; });
	}
	void Release()
	{
		std::lock_guard lock(mutex);
		released = true;
		condition.notify_all();
	}
};
thread_local PausedMutexWaiter* g_paused_mutex_waiter = nullptr;
thread_local std::atomic<bool>* g_condition_head_finished = nullptr;

void PauseAtMutexGate(PausedMutexWaiter& gate)
{
	std::unique_lock lock(gate.mutex);
	gate.entered = true;
	gate.condition.notify_all();
	gate.condition.wait(lock, [&] { return gate.released; });
}

void ModelledMutexReadmissionReentry()
{
	Kernel::PthreadSetMutexReadmissionTestHook(nullptr);
	auto& waiter = *g_paused_mutex_waiter;
	waiter.modelled_reentry_succeeded = Kernel::PthreadMutexTrylock(waiter.modelled_reentry_slot) == 0 &&
	                                   Kernel::PthreadMutexUnlock(waiter.modelled_reentry_slot) == 0;
}

void PauseEnrolledMutexWaiter()
{
	Kernel::PthreadSetMutexWaitTestHook(nullptr);
	auto& waiter = *g_paused_mutex_waiter;
	if (waiter.modelled_reentry_slot != nullptr)
		Kernel::PthreadSetMutexReadmissionTestHook(ModelledMutexReadmissionReentry);
	std::unique_lock lock(waiter.mutex);
#if defined(__APPLE__) && defined(LUMEN_FEXCORE_RUNTIME)
	if (waiter.verify_donated_reader)
		waiter.donated_reader = LumenAndroidFexCoreBeginQuiescentHostWaitV1() == 0;
#endif
	waiter.entered = true;
	waiter.condition.notify_all();
	waiter.condition.wait(lock, [&] { return waiter.released; });
}

struct MutexWakeObservation
{
	std::mutex mutex;
	std::condition_variable condition;
	bool armed = false, observed = false, notified = false;
	bool Wait(bool for_wake)
	{
		std::unique_lock lock(mutex);
		return condition.wait_for(lock, std::chrono::milliseconds(for_wake ? 250 : 2000), [&] {
			return for_wake ? observed : armed;
		});
	}
};
thread_local MutexWakeObservation* g_mutex_wake_observation = nullptr;

void PauseBeforeMutexEnrollment()
{
	Kernel::PthreadSetMutexBeforeEnrollTestHook(nullptr);
	PauseEnrolledMutexWaiter();
}

void ObserveMutexWaitArmed()
{
	Kernel::PthreadSetMutexWaitArmedTestHook(nullptr);
	auto& observation = *g_mutex_wake_observation;
	std::lock_guard lock(observation.mutex);
	observation.armed = true;
	observation.condition.notify_all();
}

void ObserveMutexWake(bool notified)
{
	Kernel::PthreadSetMutexWakeTestHook(nullptr);
	auto& observation = *g_mutex_wake_observation;
	std::lock_guard lock(observation.mutex);
	observation.notified = notified;
	observation.observed = true;
	observation.condition.notify_all();
}

void ConfigureMutexWakeObservation(MutexWakeObservation* observation)
{
	g_mutex_wake_observation = observation;
	Kernel::PthreadSetMutexWaitArmedTestHook(ObserveMutexWaitArmed);
	Kernel::PthreadSetMutexWakeTestHook(ObserveMutexWake);
	// Make a missing notification observably different from fallback polling,
	// without requiring sub-millisecond scheduling from the test host. The
	// production slice remains 1 ms; only this fixture thread uses one second.
	Kernel::PthreadSetMutexWaitSliceForTest(1000000);
}

bool TestQueuedWakeNotification()
{
	if (!Kernel::PthreadMutexQueuedAcquisitionForTest()) return true;
	// Cover unlock during the CV wait, after enrollment but before wait, and
	// after failed trylock while the queue is still empty. The last case must
	// retain the generation change even though no CV notification is issued.
	for (const unsigned release_window: {0u, 1u, 2u})
	{
		const bool release_before_wait = release_window != 0;
		Kernel::PthreadMutex mutex = nullptr;
		if (!InitMutex(&mutex, 4) || !Expect(Kernel::PthreadMutexLock(&mutex), 0, 4, "wake owner lock")) return false;
		PausedMutexWaiter paused;
		MutexWakeObservation observation;
		bool worker_ok = true;
		std::thread worker([&] {
			ConfigureMutexWakeObservation(&observation);
			if (release_before_wait)
			{
				g_paused_mutex_waiter = &paused;
				if (release_window == 2)
					Kernel::PthreadSetMutexBeforeEnrollTestHook(PauseBeforeMutexEnrollment);
				else
					Kernel::PthreadSetMutexWaitTestHook(PauseEnrolledMutexWaiter);
			}
			worker_ok = Expect(Kernel::PthreadMutexLock(&mutex), 0, 4, "notified waiter lock");
			if (worker_ok) worker_ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, 4, "notified waiter release");
		});
		bool ok = Check(release_before_wait ? paused.WaitUntilEntered() : observation.Wait(false),
		                "notified waiter did not reach the selected release window");
		ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, 4, "notify final native release") && ok;
		paused.Release();
		ok = Check(observation.Wait(true), "mutex release was serviced only by the fallback timeout") && ok;
		worker.join();
		ok = Check(worker_ok && observation.notified,
		           "release notification was lost before or during the CV wait") && ok;
		if (!Expect(Kernel::PthreadMutexDestroy(&mutex), 0, 4, "wake mutex destruction") || !ok) return false;
	}
	return true;
}

bool TestQueuedHeadTimeoutNotification()
{
	if (!Kernel::PthreadMutexQueuedAcquisitionForTest()) return true;
	Kernel::PthreadMutex mutex = nullptr;
	if (!InitMutex(&mutex, 4) || !Expect(Kernel::PthreadMutexLock(&mutex), 0, 4, "head timeout owner lock")) return false;
	PausedMutexWaiter head;
	MutexWakeObservation successor;
	bool head_ok = true, successor_ok = true;
	std::thread first([&] {
		g_paused_mutex_waiter = &head;
		Kernel::PthreadSetMutexWaitTestHook(PauseEnrolledMutexWaiter);
		head_ok = TimedCall(&mutex, 2000, Kernel::KERNEL_ERROR_ETIMEDOUT, 4, "reserved head timeout");
	});
	bool ok = Check(head.WaitUntilEntered(), "timed head did not enroll");
	ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, 4, "head timeout owner release") && ok;
	// Enroll the successor after release so it cannot consume that earlier
	// generation. Only removing the expired head can now notify this waiter.
	std::thread second([&] {
		ConfigureMutexWakeObservation(&successor);
		successor_ok = Expect(Kernel::PthreadMutexLock(&mutex), 0, 4, "head timeout successor lock");
		if (successor_ok) successor_ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, 4, "head timeout successor release");
	});
	ok = Check(successor.Wait(false), "head timeout successor did not arm its native wait") && ok;
	// The head is deliberately paused beyond its relative deadline; this sleep
	// controls the timeout condition, not a notification latency assertion.
	std::this_thread::sleep_for(std::chrono::milliseconds(3));
	head.Release();
	ok = Check(successor.Wait(true), "expired FIFO head did not notify its successor") && ok;
	first.join();
	second.join();
	ok = Check(head_ok && successor_ok && successor.notified, "head timeout lost notification or successor ownership") && ok;
	return Expect(Kernel::PthreadMutexDestroy(&mutex), 0, 4, "head timeout mutex destruction") && ok;
}

bool TestQueuedAcquisitionDoesNotWakeSuccessors()
{
	if (!Kernel::PthreadMutexQueuedAcquisitionForTest()) return true;
	Kernel::PthreadMutex mutex = nullptr;
	if (!InitMutex(&mutex, 4) || !Expect(Kernel::PthreadMutexLock(&mutex), 0, 4, "acquisition wake owner lock")) return false;
	PausedMutexWaiter head, acquired;
	std::array<MutexWakeObservation, 2> successors;
	std::array<std::thread, 2> workers;
	std::atomic<bool> workers_ok {true};
	std::thread first([&] {
		g_paused_mutex_waiter = &head;
		Kernel::PthreadSetMutexWaitTestHook(PauseEnrolledMutexWaiter);
		const bool locked = Expect(Kernel::PthreadMutexLock(&mutex), 0, 4, "acquisition wake head lock");
		if (!locked) workers_ok = false;
		{
			std::unique_lock lock(acquired.mutex);
			acquired.entered = true;
			acquired.condition.notify_all();
			acquired.condition.wait(lock, [&] { return acquired.released; });
		}
		if (locked && !Expect(Kernel::PthreadMutexUnlock(&mutex), 0, 4, "acquisition wake head release")) workers_ok = false;
	});
	bool ok = Check(head.WaitUntilEntered(), "acquisition wake head did not enroll");
	ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, 4, "acquisition wake original release") && ok;
	// The native mutex is free but reserved for the paused head. Successors
	// enroll after the original release, so that generation cannot wake them.
	for (size_t i = 0; i < workers.size(); ++i)
	{
		workers[i] = std::thread([&, i] {
			ConfigureMutexWakeObservation(&successors[i]);
			const bool locked = Expect(Kernel::PthreadMutexLock(&mutex), 0, 4, "acquisition wake successor lock");
			if (!locked || !Expect(Kernel::PthreadMutexUnlock(&mutex), 0, 4, "acquisition wake successor release")) workers_ok = false;
		});
		ok = Check(successors[i].Wait(false), "acquisition wake successor did not arm") && ok;
	}
	head.Release();
	ok = Check(acquired.WaitUntilEntered(), "acquisition wake head did not acquire") && ok;
	// Both 250 ms observations fit inside the fixture's one-second wait slice.
	// A wake here is caused by acquisition, not fallback polling or release.
	for (auto& successor: successors)
		ok = Check(!successor.Wait(true), "successful acquisition woke a successor while mutex remained owned") && ok;
	acquired.Release();
	for (auto& successor: successors)
		ok = Check(successor.Wait(true), "actual head release did not wake its successors") && ok;
	first.join();
	for (auto& worker: workers) worker.join();
	for (auto& successor: successors)
		ok = Check(successor.notified, "successor acquired only after fallback polling") && ok;
	ok = Check(workers_ok, "acquisition wake workers lost mutex ownership") && ok;
	return Expect(Kernel::PthreadMutexDestroy(&mutex), 0, 4, "acquisition wake mutex destruction") && ok;
}

bool TestQueuedHeadWakeSelection()
{
	if (!Kernel::PthreadMutexQueuedAcquisitionForTest()) return true;
	const bool targeted = Kernel::PthreadMutexHeadWakeForTest();
	Kernel::PthreadMutex mutex = nullptr;
	if (!InitMutex(&mutex, 4) || !Expect(Kernel::PthreadMutexLock(&mutex), 0, 4, "head wake owner lock")) return false;
	std::array<MutexWakeObservation, 3> observations;
	std::array<std::thread, 3> workers;
	PausedMutexWaiter acquired;
	std::atomic<bool> workers_ok {true};
	std::atomic<unsigned> order {0};
	bool ok = true;
	for (size_t i = 0; i < workers.size(); ++i)
	{
		workers[i] = std::thread([&, i] {
			ConfigureMutexWakeObservation(&observations[i]);
			const bool locked = Expect(Kernel::PthreadMutexLock(&mutex), 0, 4, "head wake worker lock");
			if (!locked || order.fetch_add(1) != i) workers_ok = false;
			if (i == 0) PauseAtMutexGate(acquired);
			if (locked && !Expect(Kernel::PthreadMutexUnlock(&mutex), 0, 4, "head wake worker release")) workers_ok = false;
		});
		ok = Check(observations[i].Wait(false), "head wake worker did not arm in FIFO order") && ok;
	}
	ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, 4, "head wake original release") && ok;
	ok = Check(observations[0].Wait(true) && acquired.WaitUntilEntered(), "selected head woke only by fallback polling") && ok;
	// All three native CV waits were armed before release. Keep the selected
	// owner parked so later observations distinguish one recipient from a
	// broadcast, instead of succeeding through a rapid chain of real unlocks.
	for (size_t i = 1; i < observations.size(); ++i)
		ok = Check(observations[i].Wait(true) == !targeted, "mutex release selected the wrong waiter set") && ok;
	acquired.Release();
	for (auto& observation: observations)
		ok = Check(observation.Wait(true), "head wake chain relied on fallback polling") && ok;
	for (auto& worker: workers) worker.join();
	for (auto& observation: observations) ok = Check(observation.notified, "head wake chain lost a notification") && ok;
	ok = Check(workers_ok && order == workers.size(), "head wake changed FIFO acquisition order") && ok;
	return Expect(Kernel::PthreadMutexDestroy(&mutex), 0, 4, "head wake mutex destruction") && ok;
}

struct NestedMutexReadmission
{
	Kernel::PthreadMutex* slot;
	PausedMutexWaiter start;
	MutexWakeObservation wake;
	std::atomic<unsigned>* order;
	bool inner_ok = false;
};
thread_local NestedMutexReadmission* g_nested_mutex_readmission = nullptr;

void NestedMutexReadmissionWait()
{
	Kernel::PthreadSetMutexReadmissionTestHook(nullptr);
	auto& nested = *g_nested_mutex_readmission;
	PauseAtMutexGate(nested.start);
	ConfigureMutexWakeObservation(&nested.wake);
	const bool locked = Expect(Kernel::PthreadMutexLock(nested.slot), 0, 4, "nested head inner acquisition");
	nested.inner_ok = locked && nested.order->fetch_add(1) == 0;
	if (locked) nested.inner_ok = Expect(Kernel::PthreadMutexUnlock(nested.slot), 0, 4, "nested head inner release") && nested.inner_ok;
}

bool TestQueuedNestedHeadWake()
{
	if (!Kernel::PthreadMutexQueuedAcquisitionForTest()) return true;
	const bool targeted = Kernel::PthreadMutexHeadWakeForTest();
	Kernel::PthreadMutex mutex = nullptr;
	if (!InitMutex(&mutex, 4) || !Expect(Kernel::PthreadMutexLock(&mutex), 0, 4, "nested head owner lock")) return false;
	std::atomic<unsigned> order {0};
	NestedMutexReadmission nested {&mutex, {}, {}, &order};
	PausedMutexWaiter acquired;
	MutexWakeObservation foreign_wake;
	bool outer_ok = true, foreign_ok = true;
	std::thread outer([&] {
		g_nested_mutex_readmission = &nested;
		Kernel::PthreadSetMutexReadmissionTestHook(NestedMutexReadmissionWait);
		const bool locked = Expect(Kernel::PthreadMutexLock(&mutex), 0, 4, "nested head outer acquisition");
		outer_ok = locked && order.fetch_add(1) == 1;
		PauseAtMutexGate(acquired);
		if (locked) outer_ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, 4, "nested head outer release") && outer_ok;
	});
	bool ok = Check(nested.start.WaitUntilEntered(), "outer head did not reach modelled exception readmission");
	// The foreign ticket deliberately sits between the outer reservation and
	// its nested call. Only the head's thread is eligible to own the mutex.
	std::thread foreign([&] {
		ConfigureMutexWakeObservation(&foreign_wake);
		const bool locked = Expect(Kernel::PthreadMutexLock(&mutex), 0, 4, "nested head foreign acquisition");
		foreign_ok = locked && order.fetch_add(1) == 2;
		if (locked) foreign_ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, 4, "nested head foreign release") && foreign_ok;
	});
	ok = Check(foreign_wake.Wait(false), "foreign waiter did not enroll behind outer head") && ok;
	nested.start.Release();
	ok = Check(nested.wake.Wait(false), "nested head did not arm its own CV") && ok;
	ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, 4, "nested head original release") && ok;
	ok = Check(nested.wake.Wait(true), "nested head relied on polling of its suspended outer reservation") && ok;
	ok = Check(acquired.WaitUntilEntered(), "outer head did not resume after nested release") && ok;
	ok = Check(foreign_wake.Wait(true) == !targeted, "nested head release notified a foreign reservation") && ok;
	acquired.Release();
	ok = Check(foreign_wake.Wait(true), "outer head release did not advance to foreign reservation") && ok;
	outer.join();
	foreign.join();
	ok = Check(outer_ok && foreign_ok && nested.inner_ok && nested.wake.notified && foreign_wake.notified && order == 3,
	           "nested head changed acquisition order or lost notification") && ok;
	return Expect(Kernel::PthreadMutexDestroy(&mutex), 0, 4, "nested head mutex destruction") && ok;
}

bool TestQueuedHandoff(int type)
{
	if (!Kernel::PthreadMutexQueuedAcquisitionForTest()) return true;
	Kernel::PthreadMutex mutex = nullptr;
	if (!InitMutex(&mutex, type) || !Expect(Kernel::PthreadMutexLock(&mutex), 0, type, "FIFO initial owner")) return false;
	const auto handle = mutex;
	PausedMutexWaiter head, second;
	head.modelled_reentry_slot = &mutex;
	std::atomic<unsigned> order {0};
	std::atomic<bool> workers_ok {true};
	const auto run_waiter = [&](PausedMutexWaiter& parked, unsigned expected_order)
	{
		g_paused_mutex_waiter = &parked;
		Kernel::PthreadSetMutexWaitTestHook(PauseEnrolledMutexWaiter);
		const int result = Kernel::PthreadMutexLock(&mutex);
		if (result != 0) { workers_ok = false; return; }
		if (order.fetch_add(1) != expected_order) workers_ok = false;
		if (Kernel::PthreadMutexUnlock(&mutex) != 0) workers_ok = false;
	};
	std::thread first_thread([&] { run_waiter(head, 0); });
	bool ok = Check(head.WaitUntilEntered(), "FIFO head did not enroll before its host wait");
	// A pending foreign reservation cannot prevent recursive ownership or
	// change ERRORCHECK self-lock disposition while the original owner holds it.
	ok = Expect(Kernel::PthreadMutexLock(&mutex), type == 2 ? 0 : Kernel::KERNEL_ERROR_EDEADLK,
	            type, "FIFO queued self-lock") && ok;
	if (type == 2) ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, type, "FIFO recursive release") && ok;
	ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, type, "FIFO original owner release") && ok;
	for (unsigned attempt = 0; attempt < 100; ++attempt)
		ok = Expect(Kernel::PthreadMutexTrylock(&mutex), Kernel::KERNEL_ERROR_EBUSY, type,
		            "FIFO rejected former-owner barging") && ok;
	ok = TimedCall(&mutex, 0, Kernel::KERNEL_ERROR_ETIMEDOUT, type, "FIFO immediate timed barger") && ok;
	ok = Expect(Kernel::PthreadMutexUnlock(&mutex), Kernel::KERNEL_ERROR_EPERM, type, "FIFO unowned unlock") && ok;
	ok = Expect(Kernel::PthreadMutexDestroy(&mutex), Kernel::KERNEL_ERROR_EBUSY, type, "FIFO pinned destroy") &&
	     Check(mutex == handle, "busy destroy changed the reserved handle") && ok;
	// Expiration removes a middle reservation and must not leave a queue hole.
	std::thread timed_thread([&] {
		if (!TimedCall(&mutex, 2000, Kernel::KERNEL_ERROR_ETIMEDOUT, type, "FIFO middle timeout")) workers_ok = false;
	});
	timed_thread.join();
	std::thread second_thread([&] { run_waiter(second, 1); });
	ok = Check(second.WaitUntilEntered(), "FIFO second waiter did not enroll") && ok;
	head.Release();
	first_thread.join();
	ok = Check(order == 1 && head.modelled_reentry_succeeded,
	           "FIFO head or modelled same-thread readmission reentry lost its reservation") && ok;
	ok = Expect(Kernel::PthreadMutexTrylock(&mutex), Kernel::KERNEL_ERROR_EBUSY, type,
	            "FIFO second reservation survived predecessor release") && ok;
	second.Release();
	second_thread.join();
	ok = Check(workers_ok && order == 2, "FIFO waiters lost order or ownership") && ok;
	ok = TimedCall(&mutex, 0, 0, type, "FIFO completed queue immediate acquisition") && ok;
	ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, type, "FIFO final release") && ok;
	return Expect(Kernel::PthreadMutexDestroy(&mutex), 0, type, "FIFO unpinned destroy") && ok;
}

void ResumeConditionHead()
{
	Kernel::PthreadSetMutexWaitTestHook(nullptr);
	g_paused_mutex_waiter->Release();
	while (!g_condition_head_finished->load(std::memory_order_acquire)) std::this_thread::yield();
}

bool TestQueuedConditionRestore()
{
#if !defined(__APPLE__)
	return true;
#else
	if (!Kernel::PthreadMutexQueuedAcquisitionForTest()) return true;
	Kernel::PthreadMutex mutex = nullptr;
	Kernel::PthreadCond condition = nullptr;
	if (!InitMutex(&mutex, 2) || !Expect(Kernel::PthreadCondInit(&condition, nullptr, nullptr), 0, 2, "FIFO condition init") ||
	    !Expect(Kernel::PthreadMutexLock(&mutex), 0, 2, "FIFO condition first depth") ||
	    !Expect(Kernel::PthreadMutexLock(&mutex), 0, 2, "FIFO condition second depth")) return false;
	PausedMutexWaiter head;
	std::atomic<bool> finished {false}, worker_ok {true};
	std::thread waiter([&] {
		g_paused_mutex_waiter = &head;
		Kernel::PthreadSetMutexWaitTestHook(PauseEnrolledMutexWaiter);
		const int result = Kernel::PthreadMutexLock(&mutex);
		if (result != 0) worker_ok = false;
		else
		{
			if (Kernel::PthreadCondSignal(&condition) != 0 || Kernel::PthreadMutexUnlock(&mutex) != 0) worker_ok = false;
		}
		finished.store(true, std::memory_order_release);
	});
	bool ok = Check(head.WaitUntilEntered(), "FIFO condition foreign head did not enroll");
	g_paused_mutex_waiter = &head;
	g_condition_head_finished = &finished;
	Kernel::PthreadSetMutexWaitTestHook(ResumeConditionHead);
	ok = Expect(Kernel::PthreadCondTimedwait(&condition, &mutex, 0), Kernel::KERNEL_ERROR_ETIMEDOUT,
	            2, "FIFO condition expired wait restored behind head") && ok;
	Kernel::PthreadSetMutexWaitTestHook(nullptr);
	// Release even on a failure where the condition never entered its restore
	// loop, so the executable's cleanup does not leave a parked host waiter.
	head.Release();
	ok = Check(finished && worker_ok, "FIFO condition reacquired before its reserved foreign head") && ok;
	ok = TimedCall(&mutex, 0, 0, 2, "FIFO condition restored recursive ownership") && ok;
	ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, 2, "FIFO condition recursive probe release") && ok;
	ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, 2, "FIFO condition saved second-level release") && ok;
	ok = CheckOtherThreadExcluded(&mutex, 2) && ok;
	ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, 2, "FIFO condition saved final-level release") && ok;
	waiter.join();
	ok = Expect(Kernel::PthreadCondDestroy(&condition), 0, 2, "FIFO condition destruction") && ok;
	return Expect(Kernel::PthreadMutexDestroy(&mutex), 0, 2, "FIFO condition mutex destruction") && ok;
#endif
}

#if defined(__APPLE__) && defined(LUMEN_FEXCORE_RUNTIME)
constexpr uint64_t kFexMutexPage = 16384;
struct FexMutexMapping final
{
	FexMutexMapping(): address(Memory::Alloc(0, kFexMutexPage, Memory::Mode::ReadWrite)) {}
	~FexMutexMapping() { if (address != 0 && !Memory::Free(address)) std::abort(); }
	const uint64_t address;
};
struct FexMutexContext
{
	uint64_t code = 0;
	Kernel::PthreadMutex* slot = nullptr;
	PausedMutexWaiter* parked = nullptr;
	bool called = false, owned = false;
};
void FexMutexTarget() {}

uint32_t FexMutexQuery(void* opaque, uint64_t address, uint64_t* base, uint64_t* size, uint32_t* writable) noexcept
{
	const auto& context = *static_cast<FexMutexContext*>(opaque);
	if (address < context.code || address - context.code >= kFexMutexPage) return 0;
	Memory::CodeReadLease lease;
	const uint64_t page = address & ~uint64_t {4095};
	if (!Memory::TryAcquireCodeReadLease(page, 4096, &lease)) return 0;
	*base = page;
	*size = 4096;
	*writable = 0;
	Memory::ReleaseCodeReadLease(&lease);
	return 1;
}

uint32_t FexMutexCallback(void* opaque, uint64_t target, uint64_t* gprs, uint64_t* next_rip) noexcept
{
	auto& context = *static_cast<FexMutexContext*>(opaque);
	if (target != reinterpret_cast<uint64_t>(&FexMutexTarget)) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	context.called = true;
	g_paused_mutex_waiter = context.parked;
	Kernel::PthreadSetMutexWaitTestHook(PauseEnrolledMutexWaiter);
	const int result = Kernel::PthreadMutexLock(context.slot);
	if (result == 0)
	{
		context.owned = Kernel::PthreadMutexLock(context.slot) == 0 &&
		                Kernel::PthreadMutexUnlock(context.slot) == 0 &&
		                Kernel::PthreadMutexUnlock(context.slot) == 0;
	}
	gprs[0] = static_cast<uint64_t>(static_cast<int64_t>(result));
	std::memcpy(next_rip, reinterpret_cast<const void*>(gprs[4]), sizeof(*next_rip));
	gprs[4] += sizeof(*next_rip);
	return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
}

bool TestFexQueuedHandoffYieldsForMutation()
{
	FexMutexMapping code, stack, slot_mapping, mutation;
	if (!Check(code.address && stack.address && slot_mapping.address && mutation.address,
	           "FEX FIFO fixture allocation failed")) return false;
	const auto target = reinterpret_cast<uint64_t>(&FexMutexTarget);
	std::array<uint8_t, 13> program {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0, 0xc3};
	std::memcpy(program.data() + 2, &target, sizeof(target));
	std::memcpy(reinterpret_cast<void*>(code.address), program.data(), program.size());
	if (!Check(Memory::Protect(code.address, kFexMutexPage, Memory::Mode::ExecuteRead) &&
	           LumenAndroidFexCoreRegisterHostCallTargetV1(target) == 1, "FEX FIFO code publication failed")) return false;
	auto* slot = reinterpret_cast<Kernel::PthreadMutex*>(slot_mapping.address);
	if (!InitMutex(slot, 2) || !Expect(Kernel::PthreadMutexLock(slot), 0, 2, "FEX FIFO initial recursive owner")) return false;
	PausedMutexWaiter parked;
	parked.verify_donated_reader = true;
	FexMutexContext context {code.address, slot, &parked};
	uint32_t run_result = UINT32_MAX;
	std::thread waiter([&] {
		LumenAndroidFexCoreGuestStateV1 state {};
		state.structSize = sizeof(state);
		state.abiVersion = 1;
		state.rip = code.address;
		state.gprs[4] = stack.address + kFexMutexPage - 8;
		const uint64_t root_return = LumenAndroidFexCoreRootReturnAddressV1();
		std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &root_return, sizeof(root_return));
		run_result = LumenAndroidFexCoreRunGuestV1(&state, FexMutexCallback, FexMutexQuery, &context);
	});
	bool ok = Check(parked.WaitUntilEntered() && parked.donated_reader,
	                "FEX FIFO head did not donate its actual HLE reader");
	// This writer is a peer to the actual FEX Run. It must pass while the head
	// reservation is parked and keeps no native mutex or guest data lease.
	ok = Check(Memory::Protect(mutation.address, kFexMutexPage, Memory::Mode::Read),
	           "FEX FIFO reserved waiter prevented Exclusive mapping mutation") && ok;
	ok = Expect(Kernel::PthreadMutexLock(slot), 0, 2, "FEX FIFO recursive owner with pending head") && ok;
	ok = Expect(Kernel::PthreadMutexUnlock(slot), 0, 2, "FEX FIFO recursive owner release") && ok;
	ok = Expect(Kernel::PthreadMutexUnlock(slot), 0, 2, "FEX FIFO native final release") && ok;
	const auto handle = *slot;
	ok = Expect(Kernel::PthreadMutexTrylock(slot), Kernel::KERNEL_ERROR_EBUSY, 2, "FEX FIFO native-free barger") && ok;
	ok = Expect(Kernel::PthreadMutexDestroy(slot), Kernel::KERNEL_ERROR_EBUSY, 2, "FEX FIFO native-free pinned destroy") &&
	     Check(*slot == handle, "FEX FIFO busy destruction changed the handle") && ok;
	parked.Release();
	waiter.join();
	ok = Check(context.called && context.owned && run_result == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1,
	           "FEX FIFO head failed to readmit, own recursively, release and return") && ok;
	ok = Check(Memory::Protect(mutation.address, kFexMutexPage, Memory::Mode::ReadWrite),
	           "FEX FIFO completion retained an active HLE reader") && ok;
	return Expect(Kernel::PthreadMutexDestroy(slot), 0, 2, "FEX FIFO final destruction") && ok;
}
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
bool TestNativeMutationContention()
{
	Kernel::PthreadMutex mutex = nullptr;
	if (!InitMutex(&mutex, 0)) return false;
	// Prepare the test thread's errno storage before a writer owns its epoch.
	// This test directly calls native APIs; it has no fabricated HLE reader.
	Posix::PrepareCurrentThreadError();
	bool ok = true;
	for (unsigned operation = 0; operation < 3 && ok; ++operation)
	{
		if (operation == 2 && !Expect(Kernel::PthreadMutexLock(&mutex), 0, 0, "pending-writer initial owner"))
		{
			ok = false;
			break;
		}
		std::atomic<bool> ready {false}, release {false}, acquired {false}, completed {false};
		std::thread writer([&] {
			const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
			    LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
			acquired.store(token != 0, std::memory_order_relaxed);
			ready.store(true, std::memory_order_release);
			if (token == 0) return;
			while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
			// Keep the epoch pending through the native caller's first attempt.
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			completed.store(LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) != 0,
			    std::memory_order_release);
		});
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (!ready.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		bool pending = ready.load(std::memory_order_acquire) && acquired.load(std::memory_order_relaxed);
		if (pending)
		{
			Memory::GuestDataLease lease;
			Memory::GuestDataLeaseDenialMetadata denial;
			pending = !Memory::TryAcquireGuestDataLease(reinterpret_cast<uint64_t>(&mutex), sizeof(mutex),
			    Memory::Mode::Read, &lease, false, &denial) &&
			    denial.reason == Memory::GuestDataLeaseDenialReason::GateWriterPending && denial.retryable &&
			    !denial.physical_observed && !lease.IsActive();
		}
		release.store(true, std::memory_order_release);
		if (pending)
		{
			const int result = operation == 0 ? Kernel::PthreadMutexLock(&mutex) :
			    operation == 1 ? g_timedlock(&mutex, 500000) : Kernel::PthreadMutexUnlock(&mutex);
			ok = Expect(result, 0, 0, "native slot readmission after exclusive epoch");
			if (ok && operation != 2) ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, 0, "readmitted owner release");
		}
		writer.join();
		ok = Check(pending, "native epoch did not deny the initial slot lease") &&
		     Check(completed.load(std::memory_order_acquire), "native exclusive epoch did not close") && ok;
	}
	return Expect(Kernel::PthreadMutexDestroy(&mutex), 0, 0, "native mutation mutex destroy") && ok;
}
#endif

bool TestAbsoluteConditionWait()
{
	constexpr size_t page = 16384;
	const uint64_t mapping = Memory::Alloc(0, page, Memory::Mode::ReadWrite);
	if (!Check(mapping != 0, "absolute condition deadline allocation failed")) return false;
	struct MappingOwner
	{
		uint64_t address;
		~MappingOwner() { Memory::Free(address); }
	} owner {mapping};
	auto* deadline = reinterpret_cast<Kernel::KernelTimespec*>(mapping);

	Kernel::PthreadMutex mutex = nullptr;
	Kernel::PthreadCond condition = nullptr;
	if (!InitMutex(&mutex, 0) ||
	    !Expect(Kernel::PthreadCondInit(&condition, nullptr, nullptr), 0, 0,
	            "absolute condition init") ||
	    !Expect(Kernel::PthreadMutexLock(&mutex), 0, 0,
	            "absolute condition owner lock")) return false;

	const auto now = std::chrono::system_clock::now().time_since_epoch();
	const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
	const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds);
	deadline->tv_sec = seconds.count();
	deadline->tv_nsec = nanos.count() + 20000000;
	if (deadline->tv_nsec >= 1000000000)
	{
		++deadline->tv_sec;
		deadline->tv_nsec -= 1000000000;
	}
	*Posix::GetErrorAddr() = k_errno_sentinel;
	const auto start = std::chrono::steady_clock::now();
	const int timeout = g_posix_cond_timedwait(&condition, &mutex, deadline);
	const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now() - start).count();
	bool ok = Expect(timeout, Posix::POSIX_ETIMEDOUT, 0,
	                 "absolute condition timeout") &&
	          Check(waited_ms >= 5 && waited_ms < 1000,
	                "absolute condition deadline was not honored") &&
	          Check(*Posix::GetErrorAddr() == k_errno_sentinel,
	                "POSIX condition timedwait changed errno");

	deadline->tv_sec = -1;
	deadline->tv_nsec = 0;
	ok = Expect(g_posix_cond_timedwait(&condition, &mutex, deadline),
	            Posix::POSIX_ETIMEDOUT, 0, "past absolute condition deadline") && ok;
	Kernel::PthreadCond implicit_condition = nullptr;
	ok = Expect(g_posix_cond_timedwait(&implicit_condition, &mutex, deadline),
	            Posix::POSIX_EINVAL, 0, "untracked implicit condition slot") && ok;
	ok = Check(implicit_condition == nullptr,
	           "rejected implicit condition slot was materialized") && ok;
	Kernel::PthreadMutex implicit_mutex = nullptr;
	ok = Expect(g_posix_cond_timedwait(&condition, &implicit_mutex, deadline),
	            Posix::POSIX_EINVAL, 0, "untracked implicit mutex slot") && ok;
	ok = Check(implicit_mutex == nullptr,
	           "rejected implicit mutex slot was materialized") && ok;
	deadline->tv_sec = 0;
	deadline->tv_nsec = 1000000000;
	ok = Expect(g_posix_cond_timedwait(&condition, &mutex, deadline),
	            Posix::POSIX_EINVAL, 0, "invalid absolute condition nanoseconds") && ok;
	ok = Expect(g_posix_cond_timedwait(&condition, &mutex, nullptr),
	            Posix::POSIX_EINVAL, 0, "null absolute condition deadline") && ok;

	if (!Memory::Protect(mapping, page, Memory::Mode::NoAccess))
	{
		return Check(false, "absolute condition guard protection failed");
	}
	ok = Expect(g_posix_cond_timedwait(&condition, &mutex, deadline),
	            Posix::POSIX_EFAULT, 0, "inaccessible absolute condition deadline") && ok;
	if (!Memory::Protect(mapping, page, Memory::Mode::ReadWrite))
	{
		return Check(false, "absolute condition guard restoration failed");
	}

	ok = TimedCall(&mutex, 0, Kernel::KERNEL_ERROR_EDEADLK, 0,
	               "condition timeout did not reacquire owner mutex") && ok;
	ok = Expect(Kernel::PthreadMutexUnlock(&mutex), 0, 0,
	            "absolute condition owner unlock") && ok;
	ok = Expect(Kernel::PthreadCondDestroy(&condition), 0, 0,
	            "absolute condition destroy") && ok;
	ok = Expect(Kernel::PthreadMutexDestroy(&mutex), 0, 0,
	            "absolute condition mutex destroy") && ok;
	return ok;
}

#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID) && defined(__aarch64__)
bool TestTimedSlotMemory()
{
	const long page_size = sysconf(_SC_PAGESIZE);
	if (!Check(page_size > 0, "host page size unavailable")) return false;
	const auto page = static_cast<size_t>(page_size);
	const uint64_t mapping = Memory::Alloc(0, page * 3, Memory::Mode::ReadWrite);
	if (!Check(mapping != 0, "slot test allocation failed")) return false;
	struct MappingOwner
	{
		uint64_t address;
		~MappingOwner() { Memory::Free(address); }
	} mapping_owner {mapping};
	auto* bytes = reinterpret_cast<uint8_t*>(mapping);
	std::memset(bytes, 0x6d, page * 3);
	Kernel::PthreadMutex mutex = nullptr;
	if (!InitMutex(&mutex, 0)) return false;
	const std::array<size_t, 4> offsets {page + 16, page + 33, page - sizeof(mutex), page * 3 - sizeof(mutex)};
	for (size_t offset: offsets) std::memcpy(bytes + offset, &mutex, sizeof(mutex));
	const Kernel::PthreadMutex absent = nullptr;
	std::memcpy(bytes + page + 64, &absent, sizeof(absent));
	const std::vector<uint8_t> before(bytes, bytes + page * 3);
	if (!Check(Memory::Protect(mapping + page, page, Memory::Mode::Read), "slot read-only protection failed")) return false;
	bool read_only = TimedCall(reinterpret_cast<Kernel::PthreadMutex*>(bytes + page + 64), 0,
	                           Kernel::KERNEL_ERROR_EINVAL, 0, "read-only untracked null handle");
	for (size_t offset: offsets)
	{
		auto* slot = reinterpret_cast<Kernel::PthreadMutex*>(bytes + offset);
		const bool locked = TimedCall(slot, 0, 0, 0, "readable slot acquisition");
		read_only = locked && read_only;
		if (locked) read_only = Expect(Kernel::PthreadMutexUnlock(slot), 0, 0, "read-only slot release") && read_only;
	}
	if (!Check(Memory::Protect(mapping + page, page, Memory::Mode::NoAccess), "slot guard protection failed")) return false;
	const bool guarded =
	    TimedCall(reinterpret_cast<Kernel::PthreadMutex*>(bytes + page + 16), 0, Kernel::KERNEL_ERROR_EFAULT, 0, "inaccessible slot") &&
	    TimedCall(reinterpret_cast<Kernel::PthreadMutex*>(bytes + page - 4), 0, Kernel::KERNEL_ERROR_EFAULT, 0, "partly readable slot") &&
	    TimedCall(reinterpret_cast<Kernel::PthreadMutex*>(std::numeric_limits<uintptr_t>::max() - 3), 0,
	              Kernel::KERNEL_ERROR_EFAULT, 0, "overflowing slot range");
	auto* tail = reinterpret_cast<Kernel::PthreadMutex*>(bytes + page - sizeof(mutex));
	const bool tail_locked = TimedCall(tail, 0, 0, 0, "exact eight-byte readable tail");
	bool exact_tail = tail_locked;
	if (tail_locked)
	{
		// Revoked or partly readable slots must fail before touching the host
		// mutex. Its original owner must still be able to release through a
		// fully readable alias; restoring rights then admits the revoked alias.
		exact_tail = Expect(Kernel::PthreadMutexUnlock(reinterpret_cast<Kernel::PthreadMutex*>(bytes + page + 16)),
		                    Kernel::KERNEL_ERROR_EFAULT, 0, "revoked slot unlock") &&
		             Expect(Kernel::PthreadMutexUnlock(reinterpret_cast<Kernel::PthreadMutex*>(bytes + page - 4)),
		                    Kernel::KERNEL_ERROR_EFAULT, 0, "partly readable slot unlock") &&
		             TimedCall(&mutex, 0, Kernel::KERNEL_ERROR_EDEADLK, 0, "denied unlock retained ownership") &&
		             CheckOtherThreadExcluded(&mutex, 0);
		exact_tail = Expect(Kernel::PthreadMutexUnlock(tail), 0, 0, "tail acquisition release") && exact_tail;
	}
	if (!Check(Memory::Protect(mapping + page, page, Memory::Mode::ReadWrite), "slot guard restoration failed")) return false;
	auto* restored = reinterpret_cast<Kernel::PthreadMutex*>(bytes + page + 16);
	const bool restored_locked = TimedCall(restored, 0, 0, 0, "restored slot acquisition");
	bool restored_ok = restored_locked;
	if (restored_locked) restored_ok = Expect(Kernel::PthreadMutexUnlock(restored), 0, 0, "restored slot release");
	const bool unchanged = Check(std::memcmp(bytes, before.data(), before.size()) == 0, "timedlock changed input slot or canaries");
	return Expect(Kernel::PthreadMutexDestroy(&mutex), 0, 0, "slot mutex destroy") && read_only && guarded && exact_tail && restored_ok && unchanged;
}
#endif

} // namespace

int main()
{
	Watchdog watchdog;
	const char* head_wake_value = std::getenv("LUMEN_PTHREAD_MUTEX_HEAD_WAKE");
	const bool head_wake_selected = head_wake_value != nullptr && std::strcmp(head_wake_value, "1") == 0;
	if (!Check(Kernel::PthreadMutexHeadWakeForTest() == head_wake_selected, "head wake selector does not match exact environment")) return 1;
	const char* directory_value = std::getenv("LUMEN_PTHREAD_MUTEX_DIRECTORY");
	const bool directory_selected = directory_value != nullptr && std::strcmp(directory_value, "1") == 0;
	if (!Check(Kernel::PthreadMutexDirectoryForTest() == directory_selected, "mutex directory selector does not match exact environment")) return 1;
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Memory::Init();
	Kernel::PthreadSubsystem::Instance()->Init(nullptr);
	if (!Kernel::PthreadStaticMutexInitializationContractTest()) return 1;
	if (!Kernel::PthreadMutexDirectoryContractTest()) return 1;

	if (!TestTimedlockBinding())
	{
		return 1;
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (!TestNativeMutationContention()) return 1;
#endif
#if defined(__APPLE__) && defined(LUMEN_FEXCORE_RUNTIME)
	if (!TestFexQueuedHandoffYieldsForMutation()) return 1;
#endif
	for (int type = 0; type <= 4; ++type)
	{
		if (!TestType(type) || !TestTimedType(type) || !TestContendedTimeout(type) ||
		    !TestReleaseBeforeTimeout(type, 500000) || !TestConditionInterop(type) || !TestMixedHandoffs(type) ||
		    !TestQueuedHandoff(type))
		{
			return 1;
		}
	}
	if (!TestQueuedWakeNotification() || !TestQueuedHeadTimeoutNotification() ||
	    !TestQueuedAcquisitionDoesNotWakeSuccessors() || !TestQueuedHeadWakeSelection() ||
	    !TestQueuedNestedHeadWake() || !TestQueuedConditionRestore()) return 1;
	if (!TestTimedErrors() || !TestAbsoluteConditionWait() ||
	    !TestReleaseBeforeTimeout(0, UINT64_C(1) << 32) ||
	    !TestReleaseBeforeTimeout(0, std::numeric_limits<uint64_t>::max()) ||
	    !TestThreadRename()) return 1;
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID) && defined(__aarch64__)
	if (!TestTimedSlotMemory()) return 1;
#endif
	std::fprintf(stderr, "[PTHREAD_MUTEX_HEAD_WAKE_TEST] selected=%u waiters=3 nested-readmission=deepest-ticket"
	    " timeout-head/empty-enrollment/FIFO/condition-release=pass production-poll=unchanged\n", head_wake_selected ? 1u : 0u);
	std::puts("PthreadMutexTest: PASS");
	std::fprintf(stderr, "[PTHREAD_MUTEX_DIRECTORY_TEST] selected=%u fresh-slot/FIFO/recursive/condition/destroy=pass\n",
	    directory_selected ? 1u : 0u);
	return 0;
}
