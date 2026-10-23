// Lumen, 2026. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace Lumen::Libs::Graphics {

// Capture is serialized with fence reset/reuse by the owning processor. A
// waiter releases its pin without that processor lock, before readmission.
// Consequently a completion holding the processor lock can safely wait for
// native observers without waiting for their guest-memory admission.
class NativeFenceWaitPins final
{
	struct State
	{
		std::mutex mutex;
		std::condition_variable changed;
		uint64_t readers = 0;
	};
public:
	class Lease final
	{
		friend class NativeFenceWaitPins;
	public:
		Lease() = default;
		~Lease() { Release(); }
		Lease(const Lease&) = delete;
		Lease& operator=(const Lease&) = delete;
		Lease(Lease&& other) noexcept: state(std::move(other.state)) {}
		Lease& operator=(Lease&& other) noexcept
		{
			if (this != &other) { Release(); state = std::move(other.state); }
			return *this;
		}
		bool IsActive() const { return state != nullptr; }
		void Release()
		{
			if (auto held = std::move(state))
			{
				std::lock_guard lock(held->mutex);
				--held->readers;
				if (held->readers == 0) held->changed.notify_all();
			}
		}
	private:
		std::shared_ptr<State> state;
	};

	Lease Capture()
	{
		Lease result;
		std::lock_guard lock(state->mutex);
		++state->readers;
		result.state = state;
		return result;
	}
	void BeforeResetOrReuse()
	{
		std::unique_lock lock(state->mutex);
		state->changed.wait(lock, [&] { return state->readers == 0; });
	}
private:
	std::shared_ptr<State> state = std::make_shared<State>();
};
} // namespace Lumen::Libs::Graphics
