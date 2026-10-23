// Lumen, 2026. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <chrono>

namespace Lumen::Libs::Graphics {

// Limits completed presentation cycles, independently of display refresh or
// the renderer backend. Own one instance per window/session. The caller waits
// after releasing graphics locks; this is not a minimum interval guarantee
// between asynchronous native presents within cycles of different lengths.
class PresentationPacer final
{
public:
	using Clock = std::chrono::steady_clock;
	using TimePoint = Clock::time_point;
	static constexpr auto Period()
	{
		// Round upwards in clock ticks, never to 16 milliseconds (62.5 Hz).
		return std::chrono::ceil<Clock::duration>(std::chrono::duration<int, std::ratio<1, 60>> {1});
	}
	static constexpr auto ServiceSlice() { return std::chrono::milliseconds(8); }

	void Reset() noexcept { m_has_completion = false; }

	// Injectable clock/wait/service make the timing and cancellation contract
	// testable without scheduler-dependent real-time thresholds or busy waits.
	template <typename Now, typename SleepUntil, typename Service>
	bool CompleteFrame(Now&& now, SleepUntil&& sleep_until, Service&& service)
	{
		if (!service()) { Reset(); return false; }
		auto current = now();
		if (m_has_completion)
		{
			const auto deadline = m_last_completion + Period();
			while (current < deadline)
			{
				sleep_until(std::min(deadline, current + ServiceSlice()));
				if (!service()) { Reset(); return false; }
				current = now();
			}
		}
		// Include actual oversleep, slow rendering and request waits. Never
		// carry overdue deadlines into a burst of catch-up presentations.
		m_last_completion = current;
		m_has_completion = true;
		return true;
	}

private:
	TimePoint m_last_completion {};
	bool m_has_completion = false;
};

} // namespace Lumen::Libs::Graphics
