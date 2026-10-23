#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOSTPOLLING_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOSTPOLLING_H_

#include "Lumen/Core/Threads.h"
#include <cstdio>

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
// Do not introduce the Windows min/max macros into graphics translation units.
#pragma push_macro("NOMINMAX")
#pragma push_macro("WIN32_LEAN_AND_MEAN")
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#pragma pop_macro("WIN32_LEAN_AND_MEAN")
#pragma pop_macro("NOMINMAX")
#endif

namespace Lumen::Libs::Graphics {

[[nodiscard]] inline bool GraphicsHostPollPreciseWaitEnabled()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
	static const bool enabled = [] {
		char value[2] {};
		SetLastError(ERROR_SUCCESS);
		const DWORD length = GetEnvironmentVariableA("LUMEN_WINDOWS_PRECISE_GRAPHICS_WAIT", value, sizeof(value));
		if (length == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) return true;
		return length == 1 && value[0] == '1';
	}();
	return enabled;
#else
	return false;
#endif
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
namespace HostPollingDetail {

class Timer final
{
public:
	Timer(): m_timer(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
	                                     TIMER_MODIFY_STATE | SYNCHRONIZE))
	{
		std::fprintf(stderr, "[GFX_HOST_POLL_TIMER] created=%u error=%lu scope=thread fallback=sleep_micro\n",
		             m_timer != nullptr ? 1u : 0u, m_timer != nullptr ? 0UL : GetLastError());
	}

	~Timer() { Close(); }

	Timer(const Timer&)            = delete;
	Timer& operator=(const Timer&) = delete;
	Timer(Timer&&)                 = delete;
	Timer& operator=(Timer&&)      = delete;

	[[nodiscard]] bool Wait(uint32_t micros)
	{
		if (m_timer == nullptr) return false;
		LARGE_INTEGER due {};
		due.QuadPart = -static_cast<LONGLONG>(micros) * 10;
		if (SetWaitableTimerEx(m_timer, &due, 0, nullptr, nullptr, nullptr, 0) != 0 &&
		    WaitForSingleObject(m_timer, INFINITE) == WAIT_OBJECT_0)
		{
			return true;
		}
		// A failed timer stays disabled for this thread; ordinary polling remains
		// available without repeatedly creating kernel objects after a failure.
		std::fprintf(stderr, "[GFX_HOST_POLL_TIMER] wait_failed=1 error=%lu fallback=sleep_micro\n", GetLastError());
		Close();
		return false;
	}

private:
	void Close()
	{
		if (m_timer != nullptr)
		{
			CloseHandle(m_timer);
			m_timer = nullptr;
		}
	}

	HANDLE m_timer = nullptr;
};

} // namespace HostPollingDetail
#endif

inline void GraphicsHostPollSleep(uint32_t micros)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
	if (micros != 0 && GraphicsHostPollPreciseWaitEnabled())
	{
		// Auto-reset, unnamed, and owned by the polling thread. This does not
		// change process/system timer resolution or add an active spin wait.
		thread_local HostPollingDetail::Timer timer;
		if (timer.Wait(micros)) return;
	}
#endif
	Core::Thread::SleepMicro(micros);
}

} // namespace Lumen::Libs::Graphics

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOSTPOLLING_H_
