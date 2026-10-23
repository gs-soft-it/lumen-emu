#include "emulator/src/ControllerDigitalHistory.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

using History = Lumen::Libs::Controller::Detail::DigitalButtonHistory;
using Sample  = History::Sample;

constexpr uint32_t CROSS  = 0x00004000;
constexpr uint32_t CIRCLE = 0x00002000;

bool PopEquals(History& history, uint32_t buttons, uint64_t time, uint64_t sequence)
{
	Sample sample;
	return history.Pop(&sample) && sample.buttons == buttons && sample.time == time && sample.sequence == sequence;
}

bool TestTapAndHold()
{
	History history;
	history.Reset();
	if (!history.Record(CROSS, 10) || history.Record(CROSS, 11) || !history.Record(0, 20) ||
	    history.Size() != 2 || !PopEquals(history, CROSS, 10, 1) || !PopEquals(history, 0, 20, 2))
	{
		return false;
	}

	Sample sample;
	return !history.Pop(&sample) && history.CurrentButtons() == 0 && history.Sequence() == 2;
}

bool TestDoubleTap()
{
	History history;
	history.Reset();
	const std::array<uint32_t, 4> buttons {CROSS, 0, CROSS, 0};
	for (std::size_t index = 0; index < buttons.size(); ++index)
	{
		if (!history.Record(buttons[index], 100 + index))
		{
			return false;
		}
	}
	for (std::size_t index = 0; index < buttons.size(); ++index)
	{
		if (!PopEquals(history, buttons[index], 100 + index, index + 1))
		{
			return false;
		}
	}
	return history.Size() == 0;
}

bool TestChordAndMixedConsume()
{
	History history;
	history.Reset();
	const std::array<uint32_t, 4> buttons {CROSS, CROSS | CIRCLE, CIRCLE, 0};
	for (std::size_t index = 0; index < buttons.size(); ++index)
	{
		if (!history.Record(buttons[index], 200 + index))
		{
			return false;
		}
	}

	// A buffered PadRead through sequence two has already exposed the first
	// two masks, so latest-state reads must start at the next transition.
	history.ConsumeThrough(2);
	return history.Size() == 2 && PopEquals(history, CIRCLE, 202, 3) && PopEquals(history, 0, 203, 4);
}

bool TestResetAndOverflow()
{
	History history;
	history.Reset(CIRCLE);
	if (history.Record(CIRCLE, 1) || !history.Record(0, 2))
	{
		return false;
	}
	history.Reset();
	if (history.Size() != 0 || history.Sequence() != 0 || history.CurrentButtons() != 0 || history.TakeOverflow())
	{
		return false;
	}

	uint32_t buttons = 0;
	for (std::size_t index = 0; index < History::CAPACITY; ++index)
	{
		buttons = buttons == 0 ? CROSS : 0;
		if (!history.Record(buttons, 300 + index))
		{
			return false;
		}
	}
	buttons = buttons == 0 ? CROSS : 0;
	if (!history.Record(buttons, 999) || history.Size() != 0 || history.Pop(nullptr) || !history.TakeOverflow() ||
	    history.TakeOverflow() || history.CurrentButtons() != buttons)
	{
		return false;
	}

	// Overflow resynchronizes to latest. Only a later transition may become
	// pending again.
	const uint32_t next = buttons == 0 ? CROSS : 0;
	return history.Record(next, 1000) && PopEquals(history, next, 1000, History::CAPACITY + 2);
}

} // namespace

int main()
{
	const bool tap_hold_ok = TestTapAndHold();
	const bool double_ok   = TestDoubleTap();
	const bool chord_ok    = TestChordAndMixedConsume();
	const bool overflow_ok = TestResetAndOverflow();

	std::printf("controller-digital-history tap-hold=%s double=%s chord-mixed=%s overflow=%s\n",
	            tap_hold_ok ? "ok" : "fail", double_ok ? "ok" : "fail", chord_ok ? "ok" : "fail",
	            overflow_ok ? "ok" : "fail");
	return tap_hold_ok && double_ok && chord_ok && overflow_ok ? 0 : 1;
}
