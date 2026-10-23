#pragma once

#include <cstdint>
#include <optional>

namespace Lumen::Libs::LibKernel::Detail {

// Native sleep only: copied duration and independently live host metadata may
// be used between slices. Pending must never enter guest code or acquire a VM
// lease. HostWait destruction restores admission before delivering exceptions.
template <typename HostWait, typename Sleep, typename Pending>
void SleepWithHostWaitSlices(uint64_t nanos, bool retain_wait, Sleep&& sleep, Pending&& pending)
{
	constexpr uint64_t slice_nanos = 10 * 1000 * 1000;
	if (!retain_wait)
	{
		while (nanos > 0)
		{
			const uint64_t slice = nanos > slice_nanos ? slice_nanos : nanos;
			{
				HostWait host_wait;
				sleep(slice);
			}
			nanos -= slice;
		}
		return;
	}

	std::optional<HostWait> host_wait;
	while (nanos > 0)
	{
		const uint64_t slice = nanos > slice_nanos ? slice_nanos : nanos;
		if (!host_wait) host_wait.emplace();
		sleep(slice);
		nanos -= slice;
		// An ineligible/nested wait retains the original per-slice boundary.
		// A request racing the pending check is observed after the next slice
		// (at most 10 ms of requested native sleep), or by the final destructor.
		if (nanos == 0 || !host_wait->IsActive() || pending()) host_wait.reset();
	}
}

} // namespace Lumen::Libs::LibKernel::Detail
