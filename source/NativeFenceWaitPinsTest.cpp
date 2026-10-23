// Lumen, 2026. SPDX-License-Identifier: GPL-2.0-only
#include "Emulator/Graphics/NativeFenceWaitPins.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>
using Lumen::Libs::Graphics::NativeFenceWaitPins;
using namespace std::chrono_literals;

int main()
{
	// Model a peer holding the processor lock while it retires/resets the old
	// fence. Native observers must drop pins without that lock or readmission.
	NativeFenceWaitPins pins;
	std::mutex processor;
	auto first = pins.Capture();
	auto second = pins.Capture();
	auto moved = std::move(first);
	if (first.IsActive() || !moved.IsActive()) return 1;
	std::promise<void> entered;
	std::atomic<bool> reset {false};
	auto peer = std::async(std::launch::async, [&] {
		std::lock_guard lock(processor);
		entered.set_value();
		pins.BeforeResetOrReuse();
		reset.store(true, std::memory_order_release);
	});
	entered.get_future().wait();
	if (peer.wait_for(20ms) != std::future_status::timeout || reset.load()) return 2;
	moved.Release();
	moved.Release(); // Cancellation/destruction after explicit completion is safe.
	if (peer.wait_for(20ms) != std::future_status::timeout || reset.load()) return 3;
	second = {}; // Failed/abandoned capture releases the final observer.
	if (peer.wait_for(2s) != std::future_status::ready || !reset.load()) return 4;
	peer.get();
	{
		std::lock_guard lock(processor);
		auto next_generation = pins.Capture();
	}
	pins.BeforeResetOrReuse();
	// The counter storage outlives an abandoned owner until its last pin drops.
	NativeFenceWaitPins::Lease orphan;
	{ NativeFenceWaitPins owner; orphan = owner.Capture(); }
	orphan.Release();
	std::puts("NativeFenceWaitPinsTest: PASS multiple-observers move cancellation reuse peer-lock-independent-release");
	return 0;
}
