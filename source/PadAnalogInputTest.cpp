#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Controller.h"
#include "Emulator/Loader/Timer.h"
#include "Emulator/Log.h"

#include "SDL_events.h"
#include "3rdparty/sdl2/sdl2/src/joystick/iphoneos/SDL_mfidigitalqueue.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

namespace Pad = Lumen::Libs::Controller;
constexpr int INPUT_ID = Pad::VIRTUAL_CONTROLLER_ID;
constexpr uint8_t CANARY = 0xa7;

// These coordinates describe only the existing local Controller.cpp PadData.
// The record remains opaque: no external header or console layout is used.
constexpr std::size_t RECORD_BYTES = 120;
constexpr std::size_t TIMESTAMP_OFFSET = 80;
constexpr std::size_t GUARD_BYTES = 64;
struct Prefix
{
	uint32_t buttons = 0;
	uint8_t lx = 128;
	uint8_t ly = 128;
	uint8_t rx = 128;
	uint8_t ry = 128;
	uint8_t l2 = 0;
	uint8_t r2 = 0;
};
static_assert(offsetof(Prefix, lx) == 4 && offsetof(Prefix, r2) == 9);

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "PadAnalogInputTest: %s\n", message);
	return condition;
}

struct Storage
{
	alignas(std::max_align_t) std::array<uint8_t, GUARD_BYTES + 64 * RECORD_BYTES + GUARD_BYTES> bytes;

	Storage() { bytes.fill(CANARY); }
	Pad::PadData* Data() { return reinterpret_cast<Pad::PadData*>(bytes.data() + GUARD_BYTES); }
	Prefix Sample(std::size_t index = 0) const
	{
		Prefix prefix;
		std::memcpy(&prefix, bytes.data() + GUARD_BYTES + index * RECORD_BYTES, sizeof(prefix));
		return prefix;
	}
	uint64_t Timestamp(std::size_t index = 0) const
	{
		uint64_t timestamp = 0;
		std::memcpy(&timestamp, bytes.data() + GUARD_BYTES + index * RECORD_BYTES + TIMESTAMP_OFFSET, sizeof(timestamp));
		return timestamp;
	}
	bool Guarded(int returned) const
	{
		const auto untouched = [](uint8_t value) { return value == CANARY; };
		return returned >= 0 && returned <= 64 &&
		       std::all_of(bytes.begin(), bytes.begin() + GUARD_BYTES, untouched) &&
		       std::all_of(bytes.begin() + GUARD_BYTES + returned * RECORD_BYTES, bytes.end(), untouched);
	}
};

bool Equal(const Prefix& sample, const Prefix& expected)
{
	if (sample.buttons == expected.buttons && sample.lx == expected.lx && sample.ly == expected.ly &&
	    sample.rx == expected.rx && sample.ry == expected.ry && sample.l2 == expected.l2 && sample.r2 == expected.r2) return true;
	std::fprintf(stderr,
	             "PadAnalogInputTest: got buttons=%08x axes=[%u,%u,%u,%u,%u,%u], expected %08x [%u,%u,%u,%u,%u,%u]\n",
	             sample.buttons, sample.lx, sample.ly, sample.rx, sample.ry, sample.l2, sample.r2,
	             expected.buttons, expected.lx, expected.ly, expected.rx, expected.ry, expected.l2, expected.r2);
	return false;
}

bool Read(Storage& output, int requested, int expected_count)
{
	const int count = Pad::PadRead(1, output.Data(), requested);
	if (count != expected_count)
	{
		std::fprintf(stderr, "PadAnalogInputTest: Read(%d) returned %d instead of %d\n", requested, count, expected_count);
		return false;
	}
	return Check(output.Guarded(count), "Read wrote outside its returned records");
}

bool ReadOne(const Prefix& expected, int requested = 1)
{
	Storage output;
	return Read(output, requested, 1) && Equal(output.Sample(), expected);
}

bool Empty()
{
	Storage output;
	return Read(output, 64, 0);
}

bool Latest(const Prefix& expected, Storage& output)
{
	return Check(Pad::PadReadState(1, output.Data()) == 0, "ReadState failed") &&
	       Check(output.Guarded(1), "ReadState wrote beyond its record") && Equal(output.Sample(), expected);
}

void AxisBurst(Pad::Axis axis, int final_value)
{
	for (int value = 1; value <= 100; ++value) Pad::ControllerAxis(INPUT_ID, axis, value);
	Pad::ControllerAxis(INPUT_ID, axis, final_value);
}

bool TestLatestAxesAndTimestamp()
{
	Pad::ControllerResetForNextSession();
	AxisBurst(Pad::Axis::LeftX, 241);
	AxisBurst(Pad::Axis::LeftY, 17);
	AxisBurst(Pad::Axis::RightX, 33);
	AxisBurst(Pad::Axis::RightY, 219);
	const Prefix expected {0, 241, 17, 33, 219};
	Storage latest;
	Storage buffered;
	// Without digital edges ReadState gives the exact latest analog timestamp.
	// Comparing the two APIs avoids timing assumptions and sleeps.
	return Latest(expected, latest) && Read(buffered, 1, 1) && Equal(buffered.Sample(), expected) &&
	       Check(buffered.Timestamp() == latest.Timestamp(), "coalesced sample kept an older timestamp") && Empty();
}

bool TestReturnToCenter()
{
	Pad::ControllerResetForNextSession();
	AxisBurst(Pad::Axis::LeftY, 255);
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::LeftY, 128);
	return ReadOne({}) && Empty();
}

bool TestPairedLeftStick()
{
	Pad::ControllerResetForNextSession();
	for (int value = 1; value <= 100; ++value) Pad::ControllerSetLeftStick(INPUT_ID, value, 255 - value);
	Pad::ControllerSetLeftStick(INPUT_ID, 43, 211);
	if (!ReadOne({0, 43, 211}, 64) || !Empty()) return false;
	Pad::ControllerSetLeftStick(INPUT_ID, 128, 128);
	return ReadOne({}) && Empty();
}

bool TestDigitalBarriersSurviveAnalogBursts()
{
	Pad::ControllerResetForNextSession();
	Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_CROSS, true);
	AxisBurst(Pad::Axis::LeftX, 231);
	Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_CROSS, false);
	AxisBurst(Pad::Axis::LeftY, 29);
	Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_CIRCLE, true);
	AxisBurst(Pad::Axis::RightX, 213);
	Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_CIRCLE, false);
	AxisBurst(Pad::Axis::RightY, 47);
	return ReadOne({Pad::PAD_BUTTON_CROSS, 231}) && ReadOne({0, 231, 29}) &&
	       ReadOne({Pad::PAD_BUTTON_CIRCLE, 231, 29, 213}) && ReadOne({0, 231, 29, 213, 47}) && Empty();
}

bool TestTriggerEdges()
{
	Pad::ControllerResetForNextSession();
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 10);
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 200);
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerRight, 17);
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerRight, 222);
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 0);
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerRight, 0);
	return ReadOne({Pad::PAD_BUTTON_L2, 128, 128, 128, 128, 200}) &&
	       ReadOne({Pad::PAD_BUTTON_L2 | Pad::PAD_BUTTON_R2, 128, 128, 128, 128, 200, 222}) &&
	       ReadOne({Pad::PAD_BUTTON_R2, 128, 128, 128, 128, 0, 222}) && ReadOne({}) && Empty();
}

bool TestOptionsTapsBetweenReads()
{
	Pad::ControllerResetForNextSession();
	// Both physical edges arrive before the next guest read. Large analog
	// bursts around them must not erase either edge or require repeated taps.
	Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_OPTIONS, true);
	AxisBurst(Pad::Axis::LeftX, 211);
	Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_OPTIONS, false);
	AxisBurst(Pad::Axis::LeftX, 128);
	if (!ReadOne({Pad::PAD_BUTTON_OPTIONS, 211}) || !ReadOne({}) || !Empty()) return false;

	Pad::ControllerResetForNextSession();
	for (int tap = 0; tap < 2; ++tap)
	{
		Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_OPTIONS, true);
		Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_OPTIONS, false);
	}
	Storage batch;
	if (!Read(batch, 64, 4)) return false;
	for (int index = 0; index < 4; ++index)
	{
		if (!Equal(batch.Sample(index), {index % 2 == 0 ? Pad::PAD_BUTTON_OPTIONS : 0})) return false;
	}
	if (!Empty()) return false;

	Pad::ControllerResetForNextSession();
	Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_OPTIONS, true);
	Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_OPTIONS, false);
	AxisBurst(Pad::Axis::LeftX, 128);
	Storage latest_down;
	Storage latest_up;
	Storage stable_up;
	return Latest({Pad::PAD_BUTTON_OPTIONS}, latest_down) && Latest({}, latest_up) && Latest({}, stable_up);
}

bool TestQueuedStartTapToPadRead()
{
	Pad::ControllerResetForNextSession();
	SDL_MFIDigitalQueue queue {};
	SDL_MFIDigitalQueueReset(&queue);
	// Model a complete START tap captured while the main event pump waits for
	// presentation. The producer is released again before the queue is drained.
	if (!Check(SDL_MFIDigitalQueueEnqueue(&queue, SDL_MFI_DIGITAL_EVENT_BUTTON, SDL_CONTROLLER_BUTTON_START, SDL_PRESSED) &&
	           SDL_MFIDigitalQueueEnqueue(&queue, SDL_MFI_DIGITAL_EVENT_BUTTON, SDL_CONTROLLER_BUTTON_START, SDL_RELEASED),
	           "START callback queue rejected a short tap")) return false;
	SDL_MFIDigitalEvent events[2] {};
	if (!Check(SDL_MFIDigitalQueueDrain(&queue, events, 2) == 2, "START tap was lost before the event pump")) return false;
	for (const auto& event : events)
	{
		if (!Check(event.type == SDL_MFI_DIGITAL_EVENT_BUTTON && event.index == SDL_CONTROLLER_BUTTON_START,
		           "callback queue changed the START event")) return false;
		// The existing Window mapping translates SDL START to PS4 Options.
		Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_OPTIONS, event.value == SDL_PRESSED);
	}
	Storage output;
	return Read(output, 64, 2) && Equal(output.Sample(0), {Pad::PAD_BUTTON_OPTIONS}) &&
	       Equal(output.Sample(1), {}) && Empty();
}

bool TestQuantizedAxisNoOp()
{
	struct AxisCase { Pad::Axis axis; Prefix expected; };
	const std::array<AxisCase, 6> axes {{{Pad::Axis::LeftX, {0, 91}}, {Pad::Axis::LeftY, {0, 128, 91}},
	                                  {Pad::Axis::RightX, {0, 128, 128, 91}}, {Pad::Axis::RightY, {0, 128, 128, 128, 91}},
	                                  {Pad::Axis::TriggerLeft, {Pad::PAD_BUTTON_L2, 128, 128, 128, 128, 91}},
	                                  {Pad::Axis::TriggerRight, {Pad::PAD_BUTTON_R2, 128, 128, 128, 128, 0, 91}}}};
	for (const auto& test : axes)
	{
		Pad::ControllerResetForNextSession();
		Pad::ControllerAxis(INPUT_ID, test.axis, 91);
		if (!ReadOne(test.expected)) return false;
		Storage before;
		Storage after;
		if (!Latest(test.expected, before)) return false;
		for (int repeat = 0; repeat < 12; ++repeat) Pad::ControllerAxis(INPUT_ID, test.axis, 91);
		if (!Empty() || !Latest(test.expected, after) ||
		    !Check(before.Timestamp() == after.Timestamp(), "unchanged axis refreshed its timestamp")) return false;
	}
	// An identical analog value is not a no-op when a separate button input
	// left the synthesized trigger bit inconsistent with that value.
	Pad::ControllerResetForNextSession();
	Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_L2, true);
	if (!ReadOne({Pad::PAD_BUTTON_L2})) return false;
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 0);
	return ReadOne({}) && Empty();
}

bool TestReleaseSupersedesDeliveredHeld()
{
	for (const auto axis : {Pad::Axis::TriggerLeft, Pad::Axis::TriggerRight})
	{
		Pad::ControllerResetForNextSession();
		const Prefix pressed = axis == Pad::Axis::TriggerLeft ? Prefix {Pad::PAD_BUTTON_L2, 128, 128, 128, 128, 10} :
		                                                      Prefix {Pad::PAD_BUTTON_R2, 128, 128, 128, 128, 0, 10};
		Pad::ControllerAxis(INPUT_ID, axis, 10);
		if (!ReadOne(pressed)) return false;
		Pad::ControllerAxis(INPUT_ID, axis, 200);
		Pad::ControllerAxis(INPUT_ID, axis, 0);
		if (!ReadOne({}) || !Empty()) return false;
	}
	return true;
}

bool TestUnreadTriggerTapPreserved()
{
	Pad::ControllerResetForNextSession();
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 10);
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 200);
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 0);
	return ReadOne({Pad::PAD_BUTTON_L2, 128, 128, 128, 128, 200}) && ReadOne({}) && Empty();
}

bool TestRepeatedTriggerGenerationPreserved()
{
	Pad::ControllerResetForNextSession();
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 10);
	if (!ReadOne({Pad::PAD_BUTTON_L2, 128, 128, 128, 128, 10})) return false;
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 0);
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 10);
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 200);
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 0);
	// The second press has the same mask as the delivered first press but a
	// different digital sequence. Its release must not replace that press.
	return ReadOne({}) && ReadOne({Pad::PAD_BUTTON_L2, 128, 128, 128, 128, 200}) && ReadOne({}) && Empty();
}

bool TestReadStateDoesNotAdvanceBufferedCursor()
{
	Pad::ControllerResetForNextSession();
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 10);
	Storage latest;
	if (!Latest({Pad::PAD_BUTTON_L2, 128, 128, 128, 128, 10}, latest)) return false;
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 200);
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 0);
	return ReadOne({Pad::PAD_BUTTON_L2, 128, 128, 128, 128, 200}) && ReadOne({}) && Empty();
}

bool TestPendingChordPreserved()
{
	Pad::ControllerResetForNextSession();
	Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_CROSS, true);
	if (!ReadOne({Pad::PAD_BUTTON_CROSS})) return false;
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::LeftX, 17);
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 20);
	Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_CROSS, false);
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 0);
	return ReadOne({Pad::PAD_BUTTON_CROSS | Pad::PAD_BUTTON_L2, 17, 128, 128, 128, 20}) &&
	       ReadOne({Pad::PAD_BUTTON_L2, 17, 128, 128, 128, 20}) && ReadOne({0, 17}) && Empty();
}

bool TestBufferedCursorInvalidation()
{
	Pad::ControllerResetForNextSession();
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 10);
	if (!ReadOne({Pad::PAD_BUTTON_L2, 128, 128, 128, 128, 10})) return false;
	// The reset restarts the sequence numbering. A stale cursor would now
	// mistake a new, unread press for the old delivered press.
	if (!TestUnreadTriggerTapPreserved()) return false;
	Pad::ControllerResetForNextSession();
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 10);
	if (!ReadOne({Pad::PAD_BUTTON_L2, 128, 128, 128, 128, 10})) return false;
	constexpr int physical = 71234;
	Pad::ControllerConnect(physical);
	Pad::ControllerAxis(physical, Pad::Axis::TriggerLeft, 10);
	Pad::ControllerAxis(physical, Pad::Axis::TriggerLeft, 200);
	Pad::ControllerAxis(physical, Pad::Axis::TriggerLeft, 0);
	bool valid = ReadOne({Pad::PAD_BUTTON_L2, 128, 128, 128, 128, 200}) && ReadOne({}) && Empty();
	Pad::ControllerResetForNextSession();
	Pad::ControllerAxis(physical, Pad::Axis::TriggerLeft, 10);
	valid = ReadOne({Pad::PAD_BUTTON_L2, 128, 128, 128, 128, 10}) && valid;
	Pad::ControllerDisconnect(physical);
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 10);
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 200);
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerLeft, 0);
	return ReadOne({Pad::PAD_BUTTON_L2, 128, 128, 128, 128, 200}) && ReadOne({}) && Empty() && valid;
}

bool TestUpdatesAfterRead()
{
	Pad::ControllerResetForNextSession();
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::LeftX, 31);
	if (!ReadOne({0, 31}) || !Empty()) return false;
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::LeftX, 73);
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::LeftX, 215);
	if (!ReadOne({0, 215}) || !Empty()) return false;
	Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_CROSS, true);
	if (!ReadOne({Pad::PAD_BUTTON_CROSS, 215})) return false;
	AxisBurst(Pad::Axis::LeftX, 49);
	return ReadOne({Pad::PAD_BUTTON_CROSS, 49}) && Empty();
}

bool TestReadStateAndBufferedConsumption()
{
	Pad::ControllerResetForNextSession();
	Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_CROSS, true);
	AxisBurst(Pad::Axis::LeftX, 211);
	Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_CROSS, false);
	AxisBurst(Pad::Axis::LeftX, 19);
	Storage latest_down;
	if (!Latest({Pad::PAD_BUTTON_CROSS, 19}, latest_down)) return false;
	AxisBurst(Pad::Axis::LeftX, 61);
	if (!ReadOne({Pad::PAD_BUTTON_CROSS, 211}) || !ReadOne({0, 61}) || !Empty()) return false;
	Storage latest_up;
	Storage stable_up;
	return Latest({0, 61}, latest_up) && Latest({0, 61}, stable_up) && Empty();
}

bool TestCapacityAndRingWrap()
{
	Pad::ControllerResetForNextSession();
	// Pure digital overflow retains its existing latest-64 policy. Analog
	// compression must not collapse distinct masks or extend the guest count.
	for (int index = 0; index < 70; ++index) Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_CROSS, index % 2 == 0);
	Storage full;
	if (!Read(full, 64, 64)) return false;
	for (int index = 0; index < 64; ++index)
	{
		if (!Equal(full.Sample(index), {index % 2 == 0 ? Pad::PAD_BUTTON_CROSS : 0})) return false;
	}
	if (!Empty()) return false;
	for (int index = 0; index < 80; ++index)
	{
		const uint8_t value = static_cast<uint8_t>(index + 7);
		Pad::ControllerAxis(INPUT_ID, Pad::Axis::LeftX, value);
		if (!ReadOne({0, value})) return false;
	}
	Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_CROSS, true);
	AxisBurst(Pad::Axis::LeftY, 201);
	Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_CROSS, false);
	AxisBurst(Pad::Axis::LeftY, 128);
	Storage wrapped;
	return Read(wrapped, 64, 2) && Equal(wrapped.Sample(0), {Pad::PAD_BUTTON_CROSS, 86, 201}) &&
	       Equal(wrapped.Sample(1), {0, 86, 128}) && Empty();
}

bool TestResetDiscardsPendingInput()
{
	Pad::ControllerResetForNextSession();
	Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_CROSS, true);
	AxisBurst(Pad::Axis::LeftY, 255);
	Pad::ControllerButton(INPUT_ID, Pad::PAD_BUTTON_CROSS, false);
	Pad::ControllerAxis(INPUT_ID, Pad::Axis::TriggerRight, 222);
	Pad::ControllerResetForNextSession();
	Storage neutral;
	if (!Read(neutral, 64, 1) || !Equal(neutral.Sample(), {}) ||
	    !Check(neutral.Timestamp() == 0, "reset retained a pending timestamp")) return false;
	Storage latest;
	if (!Latest({}, latest)) return false;
	Pad::ControllerSetLeftStick(INPUT_ID, 203, 23);
	return ReadOne({0, 203, 23}) && Empty();
}

} // namespace

int main()
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Lumen::Core::VirtualMemory::Init();
	Lumen::Loader::Timer::TimerSubsystem::Instance()->Init(nullptr);
	Pad::ControllerSubsystem::Instance()->Init(nullptr);
	if (!Check(Pad::PadInit() == 0 && Pad::PadOpen(1, 0, 0, nullptr) == 1, "Pad fixture initialization failed")) return 1;

	// Synthetic input reaches the public shared Pad API. This does not measure
	// Bluetooth, host event scheduling, hardware mapping, guest FPS or latency.
	struct Case { const char* name; bool (*run)(); };
	const std::array<Case, 18> cases = {{{"latest axes and timestamp", TestLatestAxesAndTimestamp},
	                                {"return to center", TestReturnToCenter},
	                                {"paired left stick", TestPairedLeftStick},
	                                {"digital barriers", TestDigitalBarriersSurviveAnalogBursts},
	                                {"trigger edges", TestTriggerEdges},
	                                {"Options taps between reads", TestOptionsTapsBetweenReads},
	                                {"queued START tap to PadRead", TestQueuedStartTapToPadRead},
	                                {"quantized axis no-op", TestQuantizedAxisNoOp},
	                                {"release supersedes delivered held", TestReleaseSupersedesDeliveredHeld},
	                                {"unread trigger tap", TestUnreadTriggerTapPreserved},
	                                {"repeated trigger generation", TestRepeatedTriggerGenerationPreserved},
	                                {"ReadState keeps buffered cursor independent", TestReadStateDoesNotAdvanceBufferedCursor},
	                                {"pending chord", TestPendingChordPreserved},
	                                {"buffered cursor invalidation", TestBufferedCursorInvalidation},
	                                {"updates after read", TestUpdatesAfterRead},
	                                {"ReadState and buffered reads", TestReadStateAndBufferedConsumption},
	                                {"capacity and ring wrap", TestCapacityAndRingWrap},
	                                {"reset", TestResetDiscardsPendingInput}}};
	bool passed = true;
	for (const auto& test : cases)
	{
		const bool result = test.run();
		std::printf("PadAnalogInputTest: %s: %s\n", test.name, result ? "PASS" : "FAIL");
		passed = result && passed;
	}
	if (!passed) return 1;
	std::puts("PadAnalogInputTest: PASS");
	return 0;
}
