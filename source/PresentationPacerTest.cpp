// Lumen, 2026. SPDX-License-Identifier: GPL-2.0-only
#include "Emulator/Graphics/PresentationPacer.h"

#include <chrono>
#include <cstdio>
#include <vector>

using Lumen::Libs::Graphics::PresentationPacer;
using namespace std::chrono_literals;

struct Fixture
{
	PresentationPacer pacer;
	PresentationPacer::TimePoint now {};
	PresentationPacer::Clock::duration oversleep {};
	std::vector<PresentationPacer::Clock::duration> sleeps;
	unsigned services = 0;
	unsigned cancel_at_service = 0;
	bool slices_valid = true;

	bool Complete()
	{
		return pacer.CompleteFrame([&] { return now; }, [&](auto deadline) {
			const auto duration = deadline - now;
			slices_valid &= duration > 0ns && duration <= PresentationPacer::ServiceSlice();
			sleeps.push_back(duration);
			now = deadline + oversleep;
		}, [&] { return ++services != cancel_at_service; });
	}
	void Work(PresentationPacer::Clock::duration duration) { now += duration; }
};

int main()
{
	unsigned failures = 0;
	auto check = [&](bool result, const char* name) {
		if (!result) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
	};
	const auto period = PresentationPacer::Period();
	check(period >= std::chrono::duration<double>(1.0 / 60.0) && period < 16667us,
	      "60 Hz precision rather than a 16 ms approximation");
	Fixture fast;
	check(fast.Complete() && fast.sleeps.empty(), "first completed frame is immediate");
	fast.Work(1ms);
	check(fast.Complete() && fast.now.time_since_epoch() == period && fast.slices_valid,
	      "fast frame waits only the remainder in bounded slices");
	Fixture almost;
	check(almost.Complete(), "prime nearly full frame");
	almost.Work(16ms);
	check(almost.Complete() && almost.sleeps.size() == 1 && almost.sleeps[0] == period - 16ms,
	      "16 ms rendering does not receive another full period");
	Fixture slow;
	check(slow.Complete(), "prime slow frame");
	slow.Work(20ms);
	check(slow.Complete() && slow.sleeps.empty() && slow.now.time_since_epoch() == 20ms,
	      "sub-60 FPS rendering has zero pacing delay");
	slow.Work(200ms);
	check(slow.Complete() && slow.sleeps.empty(), "stall adds no delay");
	const auto stalled = slow.now;
	slow.Work(1ms);
	check(slow.Complete() && slow.now - stalled == period, "stall has no accumulated catch-up debt");
	Fixture overslept;
	check(overslept.Complete(), "prime oversleep");
	overslept.oversleep = 200ms;
	check(overslept.Complete() && overslept.sleeps.size() == 1, "oversleep finishes without polling");
	const auto actual = overslept.now;
	overslept.oversleep = 0ns;
	overslept.Work(1ms);
	check(overslept.Complete() && overslept.now - actual == period, "next cycle anchors to actual completion");
	Fixture exact;
	check(exact.Complete(), "prime exact period");
	exact.Work(period);
	check(exact.Complete() && exact.sleeps.empty(), "exact-period frame does not sleep");
	for (unsigned cancellation = 1; cancellation <= 3; ++cancellation)
	{
		Fixture interrupted;
		check(interrupted.Complete(), "prime cancellation");
		interrupted.services = 0;
		interrupted.cancel_at_service = cancellation;
		check(!interrupted.Complete() && interrupted.slices_valid &&
		      interrupted.sleeps.size() == cancellation - 1, "service cancels before/after a bounded sleep");
		const auto at_cancel = interrupted.now;
		interrupted.cancel_at_service = 0;
		check(interrupted.Complete() && interrupted.now == at_cancel, "resume after cancellation is immediate");
	}
	Fixture reset;
	check(reset.Complete(), "prime reset");
	reset.Work(1ms);
	reset.pacer.Reset(); // A false flip/pause abandons the old completion deadline.
	check(reset.Complete() && reset.sleeps.empty(), "failed flip or session reset discards deadline");
	Fixture independent;
	check(independent.Complete() && independent.sleeps.empty(), "new session has no inherited deadline");
	Fixture rate;
	check(rate.Complete(), "prime rate");
	for (unsigned frame = 0; frame != 600; ++frame)
	{
		rate.Work(1ms);
		check(rate.Complete(), "complete fast stream");
	}
	check(rate.now.time_since_epoch() == period * 600 && rate.now.time_since_epoch() >= 10s && rate.slices_valid,
	      "600 completed intervals cannot run faster than 60 Hz");
	if (failures == 0)
		std::puts("PresentationPacerTest: PASS 60Hz first/fast/16ms/slow/stall/oversleep/cancel/reset/session/rate; slices<=8ms");
	return failures != 0;
}
