#include "Emulator/Graphics/HostPolling.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "GraphicsHostPollingTest: FAIL: %s\n", message);
	return condition;
}

bool CheckMinimumWait(uint32_t micros)
{
	const auto begin = std::chrono::steady_clock::now();
	Lumen::Libs::Graphics::GraphicsHostPollSleep(micros);
	const auto elapsed = std::chrono::steady_clock::now() - begin;
	return Check(elapsed >= std::chrono::microseconds(micros), "poll sleep returned before its requested duration");
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
bool HandleCount(DWORD* count)
{
	return Check(GetProcessHandleCount(GetCurrentProcess(), count) != 0, "cannot query process handle count");
}

bool HighResolutionTimerSupported()
{
	HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
	                                    TIMER_MODIFY_STATE | SYNCHRONIZE);
	if (timer == nullptr)
	{
		std::printf("GraphicsHostPollingTest: high-resolution timer unavailable error=%lu; checking fallback\n", GetLastError());
		return false;
	}
	LARGE_INTEGER due {};
	due.QuadPart = -100;
	const bool supported = SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0) != 0 &&
	                       WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0;
	if (!supported)
	{
		std::printf("GraphicsHostPollingTest: high-resolution wait unavailable error=%lu; checking fallback\n", GetLastError());
	}
	CloseHandle(timer);
	return supported;
}

bool CheckTimerLifecycle(bool enabled)
{
	const bool timer_available = HighResolutionTimerSupported();
	const bool uses_timer = enabled && timer_available;
	// Warm the native thread runtime before using handle counts as the oracle.
	std::thread([] {}).join();
	DWORD before_thread = 0;
	if (!HandleCount(&before_thread)) return false;
	bool ok = true;
	std::thread worker([&] {
		DWORD before = 0;
		DWORD after_zero = 0;
		DWORD after_first = 0;
		DWORD after_repeated = 0;
		ok = HandleCount(&before) && CheckMinimumWait(0) && HandleCount(&after_zero);
		ok = Check(after_zero == before, "zero duration created a timer handle") && ok;
		ok = CheckMinimumWait(10) && HandleCount(&after_first) && ok;
		for (int i = 0; i < 24; ++i) ok = CheckMinimumWait(100) && ok;
		ok = HandleCount(&after_repeated) && ok;
		ok = Check(after_first == before + (uses_timer ? 1u : 0u), "unexpected live timer count for selector/capability") && ok;
		ok = Check(after_repeated == after_first, "repeated polling accumulated timer handles") && ok;
	});
	worker.join();
	DWORD after_thread = 0;
	ok = HandleCount(&after_thread) && ok;
	ok = Check(after_thread == before_thread, "thread exit did not release its timer handle") && ok;

	for (int i = 0; i < 16; ++i)
	{
		std::thread another([] { Lumen::Libs::Graphics::GraphicsHostPollSleep(100); });
		another.join();
	}
	DWORD after_many_threads = 0;
	ok = HandleCount(&after_many_threads) && ok;
	return Check(after_many_threads == before_thread, "repeated thread lifecycles leaked handles") && ok;
}
#endif

} // namespace

int main(int argc, char** argv)
{
	const char* selector = argc > 1 ? argv[1] : "unset";
	bool expected = false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
	if (!Check(SetEnvironmentVariableA("LUMEN_WINDOWS_PRECISE_GRAPHICS_WAIT",
	                                  std::strcmp(selector, "unset") == 0 ? nullptr : selector) != 0,
	           "cannot set selector for this fresh test process")) return 1;
	expected = std::strcmp(selector, "unset") == 0 || std::strcmp(selector, "1") == 0;
#endif
	bool ok = Check(Lumen::Libs::Graphics::GraphicsHostPollPreciseWaitEnabled() == expected,
	                "selector defaults on for Windows x64, accepts exact 1, and is disabled on other hosts");
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
	ok = Check(SetEnvironmentVariableA("LUMEN_WINDOWS_PRECISE_GRAPHICS_WAIT", expected ? "0" : "1") != 0,
	           "cannot change environment after selector latch") && ok;
	ok = Check(Lumen::Libs::Graphics::GraphicsHostPollPreciseWaitEnabled() == expected,
	           "selector changed after process latch") && ok;
	ok = CheckTimerLifecycle(expected) && ok;
#else
	constexpr uint32_t durations[] = {0u, 10u, 100u};
	for (uint32_t micros: durations) ok = CheckMinimumWait(micros) && ok;
#endif
	std::printf("GraphicsHostPollingTest: %s selector=%s enabled=%d\n", ok ? "PASS" : "FAIL", selector, expected ? 1 : 0);
	return ok ? 0 : 1;
}
