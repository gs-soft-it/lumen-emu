#include "Lumen/Core/Threads.h"
#include "Emulator/Config.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Log.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {
namespace Kernel = Lumen::Libs::LibKernel;

class Watchdog
{
public:
	Watchdog(): m_thread([this] {
		std::unique_lock<std::mutex> lock(m_mutex);
		if (!m_changed.wait_for(lock, std::chrono::seconds(10), [this] { return m_done; }))
		{
			std::fputs("PthreadConditionTest: timed out\n", stderr);
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

bool Check(bool result, const char* description)
{
	if (!result) std::fprintf(stderr, "PthreadConditionTest: %s\n", description);
	return result;
}

bool UninitializedCondition()
{
	Kernel::PthreadCond condition = nullptr;
	Kernel::PthreadMutex mutex = nullptr;
	if (!Check(Kernel::PthreadCondWait(&condition, &mutex) == Kernel::KERNEL_ERROR_EINVAL &&
	           condition == nullptr && mutex == nullptr,
	           "invalid condition wait touched an uninitialized mutex")) return false;
	if (!Check(Kernel::PthreadMutexInit(&mutex, nullptr, "condition-test-mutex") == 0, "mutex init failed")) return false;
	const auto saved = mutex;
	// No guest module owns this host stack slot. It is intentionally unsupported
	// implicit initialization, tested as a recoverable error rather than abort.
	const bool passed = Check(Kernel::PthreadCondBroadcast(&condition) == Kernel::KERNEL_ERROR_EINVAL &&
	                          Kernel::PthreadCondBroadcast(nullptr) == Kernel::KERNEL_ERROR_EINVAL &&
	                          Kernel::PthreadCondWait(&condition, &mutex) == Kernel::KERNEL_ERROR_EINVAL &&
	                          condition == nullptr && mutex == saved,
	                          "uninitialized condition did not fail without changing handles") &&
	                    Check(Kernel::PthreadMutexLock(&mutex) == 0 && Kernel::PthreadMutexUnlock(&mutex) == 0,
	                          "failed condition wait changed the mutex");
	return Check(Kernel::PthreadMutexDestroy(&mutex) == 0, "mutex cleanup failed") && passed;
}

bool WakeAndDestroy()
{
	for (unsigned round = 0; round < 32; ++round)
	{
		Kernel::PthreadCond condition = nullptr;
		Kernel::PthreadMutex mutex = nullptr;
		if (!Check(Kernel::PthreadMutexInit(&mutex, nullptr, "condition-wake-mutex") == 0 &&
		           Kernel::PthreadCondInit(&condition, nullptr, "condition-wake-name") == 0,
		           "wake fixture init failed")) return false;
		std::atomic<unsigned> arrived {0};
		std::atomic<unsigned> completed {0};
		std::atomic<bool> operations_ok {true};
		bool release = false;
		std::array<std::thread, 4> waiters;
		for (auto& waiter: waiters)
			waiter = std::thread([&] {
				if (Kernel::PthreadMutexLock(&mutex) != 0) { operations_ok = false; return; }
				arrived.fetch_add(1, std::memory_order_release);
				while (!release)
					if (Kernel::PthreadCondWait(&condition, &mutex) != 0) { operations_ok = false; break; }
				if (release) ++completed;
				if (Kernel::PthreadMutexUnlock(&mutex) != 0) operations_ok = false;
			});
		while (arrived.load(std::memory_order_acquire) != waiters.size()) std::this_thread::yield();
		if (Kernel::PthreadMutexLock(&mutex) != 0) std::_Exit(3);
		release = true;
		const int broadcast = Kernel::PthreadCondBroadcast(&condition);
		if (Kernel::PthreadMutexUnlock(&mutex) != 0) std::_Exit(3);
		for (auto& waiter: waiters) waiter.join();
		if (!Check(broadcast == 0 && operations_ok && completed == waiters.size(), "broadcast did not release every waiter") ||
		    !Check(Kernel::PthreadCondDestroy(&condition) == 0 && condition == nullptr, "condition cleanup failed") ||
		    !Check(Kernel::PthreadCondBroadcast(&condition) == Kernel::KERNEL_ERROR_EINVAL && condition == nullptr,
		           "broadcast after explicit destruction aborted or resurrected the stack condition") ||
		    !Check(Kernel::PthreadCondInit(&condition, nullptr, "condition-reinitialized") == 0 &&
		           Kernel::PthreadCondSignal(&condition) == 0 && Kernel::PthreadCondBroadcast(&condition) == 0 &&
		           Kernel::PthreadCondDestroy(&condition) == 0 && Kernel::PthreadMutexDestroy(&mutex) == 0,
		           "explicit condition reinitialization or cleanup failed")) return false;
	}
	return true;
}

#if defined(LUMEN_PTHREAD_CONDITION_TEST_HOOK)
struct GenerationWakeState
{
	bool between_slices = false;
	unsigned injections = 0;
	int signal_result = Kernel::KERNEL_ERROR_EINVAL;
};
thread_local GenerationWakeState g_generation_wake;

void InjectGenerationWake(Kernel::PthreadCond condition, bool completed_slice)
{
	if (completed_slice != g_generation_wake.between_slices) return;
	Kernel::PthreadSetConditionWaitTestHook(nullptr);
	++g_generation_wake.injections;
	// Use the real signal wrapper after the wait captured its generation. The
	// caller still owns every native mutex level in both targeted windows.
	auto slot = condition;
	g_generation_wake.signal_result = Kernel::PthreadCondSignal(&slot);
}

bool ForeignAcquisition(Kernel::PthreadMutex* mutex, bool expected)
{
	int result = Kernel::KERNEL_ERROR_EINVAL;
	int released = 0;
	std::thread foreign([&] {
		result = Kernel::PthreadMutexTrylock(mutex);
		if (result == 0) released = Kernel::PthreadMutexUnlock(mutex);
	});
	foreign.join();
	return Check(result == (expected ? 0 : Kernel::KERNEL_ERROR_EBUSY) && released == 0,
	             "generation wake changed native mutex ownership or recursive depth");
}

bool GenerationWakeOwnership()
{
	for (const int type: {1, 2})
		for (const bool between_slices: {false, true})
			for (const bool timed: {false, true})
			{
				Kernel::PthreadMutexattr attribute = nullptr;
				Kernel::PthreadMutex mutex = nullptr;
				Kernel::PthreadCond condition = nullptr;
				if (!Check(Kernel::PthreadMutexattrInit(&attribute) == 0 &&
				           Kernel::PthreadMutexattrSettype(&attribute, type) == 0 &&
				           Kernel::PthreadMutexInit(&mutex, &attribute, nullptr) == 0 &&
				           Kernel::PthreadMutexattrDestroy(&attribute) == 0 &&
				           Kernel::PthreadCondInit(&condition, nullptr, nullptr) == 0,
				           "generation wake fixture init failed")) return false;
				const unsigned depth = type == 2 ? 3 : 1;
				for (unsigned level = 0; level < depth; ++level)
					if (!Check(Kernel::PthreadMutexLock(&mutex) == 0, "generation wake initial lock failed")) return false;
				g_generation_wake = {between_slices, 0, Kernel::KERNEL_ERROR_EINVAL};
				Kernel::PthreadSetConditionWaitTestHook(InjectGenerationWake);
				const int result = timed ? Kernel::PthreadCondTimedwait(&condition, &mutex, 100000) :
				                           Kernel::PthreadCondWait(&condition, &mutex);
				Kernel::PthreadSetConditionWaitTestHook(nullptr);
				if (!Check(result == 0 && g_generation_wake.injections == 1 && g_generation_wake.signal_result == 0,
				           "generation wake did not return after exactly one injected signal") ||
				    !ForeignAcquisition(&mutex, false)) return false;
				if (type == 1 && !Check(Kernel::PthreadMutexTimedlock(&mutex, 0) == Kernel::KERNEL_ERROR_EDEADLK,
				                       "generation wake did not restore ERRORCHECK owner metadata")) return false;
				for (unsigned level = 0; level < depth; ++level)
					if (!Check(Kernel::PthreadMutexUnlock(&mutex) == 0, "generation wake owner unlock failed") ||
					    !ForeignAcquisition(&mutex, level + 1 == depth)) return false;
				if (!Check(Kernel::PthreadCondDestroy(&condition) == 0 && Kernel::PthreadMutexDestroy(&mutex) == 0,
				           "generation wake fixture cleanup failed")) return false;
			}
	std::puts("PthreadConditionTest: generation windows and exact mutex depth PASS (8 cases)");
	return true;
}
#endif
} // namespace

int main()
{
	Watchdog watchdog;
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Kernel::PthreadSubsystem::Instance()->Init(nullptr);
	if (!UninitializedCondition() || !WakeAndDestroy()) return 1;
#if defined(LUMEN_PTHREAD_CONDITION_TEST_HOOK)
	if (!GenerationWakeOwnership()) return 1;
#endif
	std::puts("PthreadConditionTest: PASS");
	return 0;
}
