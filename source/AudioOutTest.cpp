#include "Emulator/AudioOutputBackend.h"
#include "Emulator/AudioOutputHostWait.h"

#if defined(LUMEN_AUDIO_OUT_TEST_HOOK)
#define LUMEN_ANDROID_FEXCORE_BARRIER_TESTING 1
#include "../android/app/src/main/cpp/AndroidFexCoreMutationBarrier.h"
#endif

#include "SDL.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace Host = Lumen::Libs::Audio::Host;

constexpr uint32_t SAMPLE_RATE     = 48000;
constexpr uint32_t TEST_FRAMES     = 3;
constexpr int      FULL_VOLUME     = 32768;
constexpr float    FLOAT_TOLERANCE = 1.0e-6f;
constexpr float    AUDIBLE_PEAK    = 1.0e-6f;

struct FormatShape
{
	Host::SampleFormat sample_format;
	uint32_t           channels;
	bool               standard_8ch;
};

constexpr std::array<FormatShape, 8> FORMAT_SHAPES {{
    {Host::SampleFormat::Signed16, 1, false},
    {Host::SampleFormat::Signed16, 2, false},
    {Host::SampleFormat::Signed16, 8, false},
    {Host::SampleFormat::Signed16, 8, true},
    {Host::SampleFormat::Float32, 1, false},
    {Host::SampleFormat::Float32, 2, false},
    {Host::SampleFormat::Float32, 8, false},
    {Host::SampleFormat::Float32, 8, true},
}};

Host::StreamSpec MakeSpec(const FormatShape& shape, uint32_t frames = TEST_FRAMES)
{
	Host::StreamSpec spec {};
	spec.sample_rate      = SAMPLE_RATE;
	spec.frames_per_block = frames;
	spec.channels         = shape.channels;
	spec.sample_format    = shape.sample_format;
	spec.standard_8ch     = shape.standard_8ch;
	return spec;
}

bool Near(float actual, float expected)
{
	return std::fabs(actual - expected) <= FLOAT_TOLERANCE;
}

bool Fail(const char* message)
{
	std::fprintf(stderr, "audio-output-test failed: %s\n", message);
	return false;
}

uint32_t SourceChannel(const FormatShape& shape, uint32_t output_channel)
{
	if (!shape.standard_8ch || output_channel < 4)
	{
		return output_channel;
	}
	return output_channel < 6 ? output_channel + 2 : output_channel - 2;
}

bool TestFormatShapes()
{
	std::array<int, 8> volumes {};
	volumes.fill(FULL_VOLUME);

	for (const auto& shape: FORMAT_SHAPES)
	{
		const Host::StreamSpec spec           = MakeSpec(shape);
		const size_t           sample_size    = shape.sample_format == Host::SampleFormat::Signed16 ? sizeof(int16_t) : sizeof(float);
		const size_t           expected_bytes = static_cast<size_t>(TEST_FRAMES) * shape.channels * sample_size;
		const size_t           output_samples = static_cast<size_t>(TEST_FRAMES) * shape.channels;
		std::vector<float>     output(output_samples, -9.0f);

		if (Host::InputBlockBytes(spec) != expected_bytes)
		{
			return Fail("wrong input byte count for a PS4 format shape");
		}

		if (shape.sample_format == Host::SampleFormat::Signed16)
		{
			std::vector<int16_t> input(output_samples);
			for (uint32_t frame = 0; frame < TEST_FRAMES; frame++)
			{
				for (uint32_t channel = 0; channel < shape.channels; channel++)
				{
					const uint32_t unit                                          = frame * 8 + channel + 1;
					input[static_cast<size_t>(frame) * shape.channels + channel] = static_cast<int16_t>(unit * 512);
				}
			}
			if (!Host::ConvertBlockToFloat(spec, input.data(), volumes.data(), output.data(), output.size()))
			{
				return Fail("signed 16-bit format conversion was rejected");
			}
		} else
		{
			std::vector<float> input(output_samples);
			for (uint32_t frame = 0; frame < TEST_FRAMES; frame++)
			{
				for (uint32_t channel = 0; channel < shape.channels; channel++)
				{
					const uint32_t unit                                          = frame * 8 + channel + 1;
					input[static_cast<size_t>(frame) * shape.channels + channel] = static_cast<float>(unit) / 64.0f;
				}
			}
			if (!Host::ConvertBlockToFloat(spec, input.data(), volumes.data(), output.data(), output.size()))
			{
				return Fail("32-bit float format conversion was rejected");
			}
		}

		for (uint32_t frame = 0; frame < TEST_FRAMES; frame++)
		{
			for (uint32_t channel = 0; channel < shape.channels; channel++)
			{
				const uint32_t source_channel = SourceChannel(shape, channel);
				const float    expected       = static_cast<float>(frame * 8 + source_channel + 1) / 64.0f;
				if (!Near(output[static_cast<size_t>(frame) * shape.channels + channel], expected))
				{
					return Fail("converted channel value or standard 8-channel order is wrong");
				}
			}
		}
	}

	return true;
}

bool TestInvalidSpecsAndDestination()
{
	const FormatShape stereo_shape {Host::SampleFormat::Signed16, 2, false};
	Host::StreamSpec  spec = MakeSpec(stereo_shape, 2);

	auto invalid        = spec;
	invalid.sample_rate = 0;
	if (Host::InputBlockBytes(invalid) != 0) return Fail("zero sample rate was accepted");
	invalid                  = spec;
	invalid.frames_per_block = 0;
	if (Host::InputBlockBytes(invalid) != 0) return Fail("zero frame count was accepted");
	invalid          = spec;
	invalid.channels = 3;
	if (Host::InputBlockBytes(invalid) != 0) return Fail("unsupported channel count was accepted");
	invalid              = spec;
	invalid.standard_8ch = true;
	if (Host::InputBlockBytes(invalid) != 0) return Fail("standard 8-channel flag on stereo was accepted");
	invalid               = spec;
	invalid.sample_format = static_cast<Host::SampleFormat>((std::numeric_limits<uint32_t>::max)());
	if (Host::InputBlockBytes(invalid) != 0) return Fail("unknown sample format was accepted");
	invalid                  = spec;
	invalid.frames_per_block = (std::numeric_limits<uint32_t>::max)();
	if (Host::InputBlockBytes(invalid) != 0) return Fail("overflow-shaped frame count was accepted");
	invalid             = spec;
	invalid.sample_rate = (std::numeric_limits<uint32_t>::max)();
	if (Host::InputBlockBytes(invalid) != 0) return Fail("overflow-shaped sample rate was accepted");
	invalid                  = spec;
	invalid.frames_per_block = (std::numeric_limits<uint32_t>::max)();
	invalid.channels         = (std::numeric_limits<uint32_t>::max)();
	invalid.sample_format    = Host::SampleFormat::Float32;
	if (Host::InputBlockBytes(invalid) != 0) return Fail("overflow-shaped stream specification was accepted");

	std::array<int16_t, 4> input {{1, 2, 3, 4}};
	std::array<int, 8>     volumes {};
	std::array<float, 4>   output {{7.0f, 7.0f, 7.0f, 7.0f}};
	volumes.fill(FULL_VOLUME);
	if (Host::ConvertBlockToFloat(spec, input.data(), volumes.data(), nullptr, output.size()))
	{
		return Fail("null conversion destination was accepted");
	}
	if (Host::ConvertBlockToFloat(spec, input.data(), volumes.data(), output.data(), output.size() - 1))
	{
		return Fail("short conversion destination was accepted");
	}
	for (float value: output)
	{
		if (value != 7.0f) return Fail("rejected conversion changed its destination");
	}

	return true;
}

bool TestSigned16Extremes()
{
	const FormatShape            mono_shape {Host::SampleFormat::Signed16, 1, false};
	const auto                   spec = MakeSpec(mono_shape, 5);
	const std::array<int16_t, 5> input {{(std::numeric_limits<int16_t>::min)(), -16384, 0, 16384, (std::numeric_limits<int16_t>::max)()}};
	const std::array<float, 5>   expected {{-1.0f, -0.5f, 0.0f, 0.5f, 32767.0f / 32768.0f}};
	std::array<int, 8>           volumes {};
	std::array<float, 5>         output {};
	volumes.fill(FULL_VOLUME);

	if (!Host::ConvertBlockToFloat(spec, input.data(), volumes.data(), output.data(), output.size()))
	{
		return Fail("signed 16-bit extremes conversion was rejected");
	}
	for (size_t i = 0; i < output.size(); i++)
	{
		if (!Near(output[i], expected[i])) return Fail("signed 16-bit extreme converted incorrectly");
	}
	return true;
}

bool TestFloatSaturation()
{
	const FormatShape          mono_shape {Host::SampleFormat::Float32, 1, false};
	const auto                 spec = MakeSpec(mono_shape, 6);
	const std::array<float, 6> input {{-2.0f, -1.0f, -0.25f, 0.25f, 1.0f, 2.0f}};
	const std::array<float, 6> expected {{-1.0f, -1.0f, -0.25f, 0.25f, 1.0f, 1.0f}};
	std::array<int, 8>         volumes {};
	std::array<float, 6>       output {};
	volumes.fill(FULL_VOLUME);

	if (!Host::ConvertBlockToFloat(spec, input.data(), volumes.data(), output.data(), output.size()))
	{
		return Fail("float saturation conversion was rejected");
	}
	for (size_t i = 0; i < output.size(); i++)
	{
		if (!Near(output[i], expected[i])) return Fail("float input was not saturated to the host range");
	}
	return true;
}

bool TestVolumeClamping()
{
	const FormatShape      eight_channel_shape {Host::SampleFormat::Signed16, 8, false};
	const auto             spec = MakeSpec(eight_channel_shape, 1);
	std::array<int16_t, 8> input {};
	input.fill(16384);
	const std::array<int, 8> volumes {
	    {-1, 0, 8192, 16384, 32768, 40000, (std::numeric_limits<int>::min)(), (std::numeric_limits<int>::max)()}};
	const std::array<float, 8> expected {{0.0f, 0.0f, 0.125f, 0.25f, 0.5f, 0.5f, 0.0f, 0.5f}};
	std::array<float, 8>       output {};

	if (!Host::ConvertBlockToFloat(spec, input.data(), volumes.data(), output.data(), output.size()))
	{
		return Fail("per-channel volume conversion was rejected");
	}
	for (size_t i = 0; i < output.size(); i++)
	{
		if (!Near(output[i], expected[i])) return Fail("per-channel volume was not independently clamped or applied");
	}
	return true;
}

bool TestDownmixToStereo()
{
	const FormatShape mono_shape {Host::SampleFormat::Float32, 1, false};
	const auto        invalid_spec = MakeSpec(mono_shape, 2);
	const std::array<float, 2> invalid_input {{0.25f, -0.5f}};
	std::array<float, 4>       untouched {{9.0f, 9.0f, 9.0f, 9.0f}};

	if (Host::DownmixBlockToStereo(invalid_spec, nullptr, invalid_input.size(), untouched.data(), untouched.size()) ||
	    Host::DownmixBlockToStereo(invalid_spec, invalid_input.data(), invalid_input.size(), nullptr, untouched.size()) ||
	    Host::DownmixBlockToStereo(invalid_spec, invalid_input.data(), invalid_input.size() - 1, untouched.data(), untouched.size()) ||
	    Host::DownmixBlockToStereo(invalid_spec, invalid_input.data(), invalid_input.size(), untouched.data(), untouched.size() - 1))
	{
		return Fail("invalid downmix pointers or buffer shapes were accepted");
	}
	auto unsupported_spec = invalid_spec;
	unsupported_spec.channels = 3;
	if (Host::DownmixBlockToStereo(unsupported_spec, invalid_input.data(), invalid_input.size(), untouched.data(), untouched.size()))
	{
		return Fail("unsupported downmix channel shape was accepted");
	}
	for (float sample: untouched)
	{
		if (sample != 9.0f) return Fail("rejected downmix changed its destination");
	}

	const auto mono_spec = MakeSpec(mono_shape, 3);
	const std::array<float, 3> mono_input {{-0.75f, 0.25f, 1.5f}};
	const std::array<float, 6> mono_expected {{-0.75f, -0.75f, 0.25f, 0.25f, 1.5f, 1.5f}};
	std::array<float, 6>       mono_output {};
	if (!Host::DownmixBlockToStereo(mono_spec, mono_input.data(), mono_input.size(), mono_output.data(), mono_output.size()))
	{
		return Fail("mono-to-stereo downmix was rejected");
	}
	for (size_t i = 0; i < mono_output.size(); i++)
	{
		if (!Near(mono_output[i], mono_expected[i])) return Fail("mono sample was not duplicated to both host channels");
	}

	const FormatShape stereo_shape {Host::SampleFormat::Float32, 2, false};
	const auto        stereo_spec = MakeSpec(stereo_shape, 2);
	const std::array<float, 4> stereo_input {{-0.5f, 0.25f, 1.25f, -1.5f}};
	std::array<float, 4>       stereo_output {};
	if (!Host::DownmixBlockToStereo(stereo_spec, stereo_input.data(), stereo_input.size(), stereo_output.data(),
	                                stereo_output.size()))
	{
		return Fail("stereo passthrough downmix was rejected");
	}
	for (size_t i = 0; i < stereo_output.size(); i++)
	{
		if (!Near(stereo_output[i], stereo_input[i])) return Fail("stereo downmix changed channel order or amplitude");
	}

	constexpr float CENTER_GAIN = 0.7071067811865475f;
	const FormatShape standard_8ch_shape {Host::SampleFormat::Float32, 8, true};
	const auto        standard_8ch_spec = MakeSpec(standard_8ch_shape, 9);
	std::array<float, 72> standard_8ch_input {};
	for (size_t channel = 0; channel < 8; channel++)
	{
		standard_8ch_input[channel * 8 + channel] = 1.0f;
	}
	for (size_t channel = 0; channel < 8; channel++)
	{
		standard_8ch_input[8 * 8 + channel] = 1.0f;
	}
	const std::array<float, 18> standard_8ch_expected {{
	    1.0f, 0.0f, 0.0f, 1.0f, CENTER_GAIN, CENTER_GAIN, 0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 0.5f, 0.5f, 0.0f,
	    0.0f, 0.5f, 2.5f + CENTER_GAIN, 2.5f + CENTER_GAIN,
	}};
	std::array<float, 18> standard_8ch_output {};
	if (!Host::DownmixBlockToStereo(standard_8ch_spec, standard_8ch_input.data(), standard_8ch_input.size(),
	                                standard_8ch_output.data(), standard_8ch_output.size()))
	{
		return Fail("canonical standard 8-channel downmix was rejected");
	}
	for (size_t i = 0; i < standard_8ch_output.size(); i++)
	{
		if (!Near(standard_8ch_output[i], standard_8ch_expected[i]))
		{
			return Fail("canonical 8-channel impulse used the wrong stereo downmix coefficient");
		}
	}

	return true;
}

bool TestPacingDeadline()
{
	constexpr uint32_t FRAMES = 256;
	constexpr uint32_t RATE   = 48000;
	constexpr uint64_t START  = 1000;

	Host::PacingState fractional {};
	if (Host::AdvancePacingDeadline(&fractional, START, FRAMES, RATE) != START || fractional.deadline_us != START ||
	    fractional.remainder != 0 || fractional.sample_rate != RATE)
	{
		return Fail("fractional pacing did not initialize at the current time");
	}
	for (uint32_t period = 0; period < 3; period++)
	{
		static_cast<void>(Host::AdvancePacingDeadline(&fractional, START, FRAMES, RATE));
	}
	if (fractional.deadline_us != START + 16000 || fractional.remainder != 0)
	{
		return Fail("three fractional 256-frame periods did not total exactly 16000 us");
	}

	constexpr uint64_t LONG_PERIODS    = 100000;
	constexpr uint64_t BLOCK_NUMERATOR = static_cast<uint64_t>(FRAMES) * 1000000;
	Host::PacingState long_sequence {};
	static_cast<void>(Host::AdvancePacingDeadline(&long_sequence, START, FRAMES, RATE));
	for (uint64_t period = 0; period < LONG_PERIODS; period++)
	{
		static_cast<void>(Host::AdvancePacingDeadline(&long_sequence, START, FRAMES, RATE));
	}
	const uint64_t total_numerator = BLOCK_NUMERATOR * LONG_PERIODS;
	if (long_sequence.deadline_us != START + total_numerator / RATE || long_sequence.remainder != total_numerator % RATE)
	{
		return Fail("long fractional pacing sequence accumulated drift");
	}

	Host::PacingState late {};
	static_cast<void>(Host::AdvancePacingDeadline(&late, START, FRAMES, RATE));
	static_cast<void>(Host::AdvancePacingDeadline(&late, START, FRAMES, RATE));
	if (Host::AdvancePacingDeadline(&late, 20000, FRAMES, RATE) != 20000 || late.remainder != 0 || late.sample_rate != RATE)
	{
		return Fail("late fractional pacing did not resynchronize");
	}

	Host::PacingState rate_change {};
	static_cast<void>(Host::AdvancePacingDeadline(&rate_change, START, FRAMES, RATE));
	static_cast<void>(Host::AdvancePacingDeadline(&rate_change, START, FRAMES, RATE));
	if (Host::AdvancePacingDeadline(&rate_change, 2000, FRAMES, 44100) != 2000 || rate_change.remainder != 0 ||
	    rate_change.sample_rate != 44100)
	{
		return Fail("fractional pacing rate change did not reset its remainder");
	}

	Host::PacingState overflow {};
	overflow.deadline_us = (std::numeric_limits<uint64_t>::max)() - 100;
	overflow.sample_rate = RATE;
	if (Host::AdvancePacingDeadline(&overflow, 42, FRAMES, RATE) != 42 || overflow.deadline_us != 42 || overflow.remainder != 0)
	{
		return Fail("overflowing fractional pacing deadline was accepted");
	}
	if (Host::AdvancePacingDeadline(nullptr, 77, FRAMES, RATE) != 77)
	{
		return Fail("null fractional pacing state did not fail closed");
	}

	if (Host::AdvancePacingDeadline(0, 1000, 500) != 1000) return Fail("first pacing deadline did not start immediately");
	if (Host::AdvancePacingDeadline(1000, 1100, 500) != 1500) return Fail("pacing deadline did not advance by one period");
	if (Host::AdvancePacingDeadline(1500, 1600, 500) != 2000) return Fail("pacing accumulated caller overhead");
	if (Host::AdvancePacingDeadline(2000, 3000, 500) != 3000) return Fail("late pacing deadline did not resynchronize");
	if (Host::AdvancePacingDeadline((std::numeric_limits<uint64_t>::max)() - 100, 42, 200) != 42)
		return Fail("overflowing pacing deadline was accepted");
	if (Host::AdvancePacingDeadline(1000, 1500, 0) != 1500) return Fail("zero-period pacing deadline was accepted");
	return true;
}

bool TestBufferedPacingContinuity()
{
	constexpr uint32_t FRAMES = 512;
	constexpr uint32_t CALLBACK_FRAMES = 256;
	constexpr uint32_t RATE = 48000;
	const size_t target = Host::BufferedQueueTargetFrames(FRAMES, RATE);
	const size_t capacity_samples = Host::Internal::StereoRingCapacitySamples(FRAMES, target + FRAMES);
	if (target != 4096 || Host::BufferedQueueTargetFrames(256, RATE) != 3840 ||
	    Host::BufferedQueueTargetFrames(2048, RATE) != 4096 || capacity_samples != 4608 * 2)
	{
		return Fail("buffer target was not bounded in frames across block sizes");
	}

	const std::array<size_t, 2> targets {{target, target}};
	std::array<size_t, 2> queued {{FRAMES, FRAMES}};
	if (!Host::ShouldRefillBufferedBatch(queued.data(), targets.data(), 2, 2) ||
	    Host::ShouldRefillBufferedBatch(queued.data(), targets.data(), 1, 2) ||
	    Host::ShouldRefillBufferedBatch(queued.data(), targets.data(), 0, 0) ||
	    Host::ShouldRefillBufferedBatch(nullptr, targets.data(), 2, 2))
	{
		return Fail("partial or invalid batch changed buffered pacing");
	}
	queued[1] = target;
	if (Host::ShouldRefillBufferedBatch(queued.data(), targets.data(), 2, 2))
	{
		return Fail("one full batch stream did not block refill of its peers");
	}

	Host::PacingState fallback {};
	if (Host::AdvanceBufferedPacingDeadline(&fallback, 1000, FRAMES, RATE, 0, false) != 1000 ||
	    Host::AdvanceBufferedPacingDeadline(&fallback, 1000, FRAMES, RATE, 0, false) != 11666 || fallback.refill_pending)
	{
		return Fail("fallback or null-data output lost nominal pacing");
	}
	Host::PacingState nominal {};
	static_cast<void>(Host::AdvanceBufferedPacingDeadline(&nominal, 1000, FRAMES, RATE, target, true));
	const uint64_t nominal_deadline = Host::AdvancePacingDeadline(&nominal, 1000, FRAMES, RATE);
	if (nominal_deadline != 11666 ||
	    Host::ApplyBufferedPacingDeadline(&nominal, 1100, FRAMES, RATE, FRAMES, true) != nominal_deadline ||
	    nominal.refill_pending || nominal.remainder != 32000)
	{
		return Fail("a nominal queue dip advanced the deadline twice or discarded its fraction");
	}

	Host::PacingState pacing {};
	std::deque<uint64_t> pending;
	uint64_t now_us = 1000000;
	uint64_t callback_index = 1;
	uint64_t next_callback_us = (std::numeric_limits<uint64_t>::max)();
	uint64_t submitted_frames = 0;
	uint64_t consumed_frames = 0;
	uint64_t underrun_frames = 0;
	size_t read_index_samples = 0;
	const auto next_callback_time = [&]() {
		return 1000000 + callback_index * CALLBACK_FRAMES * 1000000 / RATE;
	};
	const auto advance_to = [&](uint64_t target_us) {
		while (next_callback_us <= target_us)
		{
			for (uint32_t frame = 0; frame < CALLBACK_FRAMES; frame++)
			{
				if (pending.empty())
				{
					underrun_frames++;
				} else
				{
					if (pending.front() != consumed_frames) return false;
					pending.pop_front();
					consumed_frames++;
					read_index_samples = (read_index_samples + 2) % capacity_samples;
				}
			}
				callback_index++;
				next_callback_us = next_callback_time();
			}
			now_us = target_us;
			return true;
	};
	for (uint32_t block = 0; block < 100; block++)
	{
		const auto plan = Host::Internal::PlanRingEnqueue(capacity_samples, read_index_samples, pending.size() * 2,
		                                                    FRAMES * 2, 2);
		if (!plan.valid || plan.dropped_samples != 0)
		{
			return Fail("buffered pacing overflowed the host ring");
		}
		for (uint32_t frame = 0; frame < FRAMES; frame++) pending.push_back(submitted_frames++);
		read_index_samples = plan.read_index;
		if (pending.size() * 2 != plan.queued_samples)
		{
			return Fail("buffered pacing lost queued frame accounting");
		}
		if (next_callback_us == (std::numeric_limits<uint64_t>::max)())
		{
			next_callback_us = next_callback_time();
		}
		const uint64_t deadline = Host::AdvanceBufferedPacingDeadline(&pacing, now_us, FRAMES, RATE,
		                                                               pending.size(), true);
		if (!advance_to(std::max(now_us, deadline)))
		{
			return Fail("buffered callbacks changed PCM order");
		}
		if (block == 30)
		{
			if (pending.size() < 7 * FRAMES || !advance_to(now_us + 65000))
			{
				return Fail("buffered callbacks did not preserve a 65 ms reserve");
			}
		}
	}
	while (!pending.empty())
	{
		if (!advance_to(next_callback_us)) return Fail("final buffered drain changed PCM order");
	}
	if (underrun_frames != 0 || consumed_frames != submitted_frames)
	{
		return Fail("buffered pacing underrun or PCM loss after warmup and a 65 ms stall");
	}
	return true;
}

bool TestBufferedPacingBurstyConsumer()
{
	constexpr uint32_t FRAMES = 512;
	constexpr uint32_t RATE = 48000;
	constexpr uint32_t BURST_FRAMES = 15 * 256;
	constexpr uint64_t BURST_PERIOD_US = 80000;
	constexpr uint64_t START = 1000000;
	constexpr uint32_t BLOCKS = 1000;
	const size_t target = Host::BufferedQueueTargetFrames(FRAMES, RATE);
	const size_t capacity_samples = Host::Internal::StereoRingCapacitySamples(FRAMES, target + FRAMES);
	Host::PacingState pacing {};
	std::deque<uint64_t> pending;
	uint64_t now_us = START;
	uint64_t next_callback_us = (std::numeric_limits<uint64_t>::max)();
	uint64_t submitted_frames = 0, consumed_frames = 0, nominal_periods = 0;
	size_t read_index_samples = 0;
	bool steady = false;
	for (uint32_t block = 0; block < BLOCKS; ++block)
	{
		const auto plan = Host::Internal::PlanRingEnqueue(capacity_samples, read_index_samples, pending.size() * 2,
		                                                    FRAMES * 2, 2);
		if (!plan.valid || plan.dropped_samples != 0) return Fail("bursty consumer caused a PCM overflow");
		for (uint32_t frame = 0; frame < FRAMES; ++frame) pending.push_back(submitted_frames++);
		read_index_samples = plan.read_index;
		if (pending.size() * 2 != plan.queued_samples) return Fail("bursty consumer lost queued frame accounting");
		if (!steady && pending.size() >= target)
		{
			// The capacity includes a guard block before the first notification;
			// successive callbacks arrive in 3840-frame groups every 80 ms.
			steady = true;
			next_callback_us = now_us + static_cast<uint64_t>(FRAMES) * 1000000 / RATE + 1;
		}
		const uint64_t deadline = Host::AdvanceBufferedPacingDeadline(&pacing, now_us, FRAMES, RATE,
		                                                               pending.size(), true);
		if (steady)
		{
			if (pacing.refill_pending || deadline <= now_us)
				return Fail("a consumer burst reopened refill without a producer stall");
			++nominal_periods;
		}
		while (next_callback_us <= deadline)
		{
			if (pending.size() < BURST_FRAMES) return Fail("nominal pacing starved a burst consumer");
			for (uint32_t frame = 0; frame < BURST_FRAMES; ++frame)
			{
				if (pending.front() != consumed_frames) return Fail("bursty callbacks changed PCM order");
				pending.pop_front();
				++consumed_frames;
				read_index_samples = (read_index_samples + 2) % capacity_samples;
			}
			next_callback_us += BURST_PERIOD_US;
		}
		now_us = deadline;
	}
	const uint64_t total_numerator = nominal_periods * FRAMES * 1000000;
	if (!steady || now_us != START + total_numerator / RATE || pacing.remainder != total_numerator % RATE ||
	    submitted_frames != consumed_frames + pending.size())
	{
		return Fail("bursty consumer changed the nominal sample clock or PCM conservation");
	}
	for (const uint64_t frame: pending)
	{
		if (frame != consumed_frames++) return Fail("bursty consumer changed retained PCM order");
	}
	const uint64_t stalled_now = now_us + 65000;
	if (Host::AdvanceBufferedPacingDeadline(&pacing, stalled_now, FRAMES, RATE, FRAMES, true) != stalled_now ||
	    !pacing.refill_pending ||
	    Host::AdvanceBufferedPacingDeadline(&pacing, stalled_now, FRAMES, RATE, target, true) != stalled_now + 10666 ||
	    pacing.refill_pending || pacing.remainder != 32000)
	{
		return Fail("a real producer stall did not refill once and resume fractional pacing");
	}
	return true;
}

bool CheckRingConservation(size_t queued_before, size_t incoming, const Host::Internal::RingEnqueuePlan& plan)
{
	return plan.valid && queued_before + incoming == plan.dropped_samples + plan.queued_samples &&
	       plan.retained_samples + plan.dropped_samples == queued_before;
}

bool TestRingEnqueuePlanning()
{
	const auto exact_fit = Host::Internal::PlanRingEnqueue(16, 4, 12, 4, 2);
	if (!CheckRingConservation(12, 4, exact_fit) || exact_fit.read_index != 4 || exact_fit.retained_samples != 12 ||
	    exact_fit.write_index != 0 || exact_fit.first_copy_samples != 4 || exact_fit.queued_samples != 16 ||
	    exact_fit.dropped_samples != 0)
	{
		return Fail("exact-fit ring enqueue plan is incorrect");
	}

	const auto partial_overflow = Host::Internal::PlanRingEnqueue(16, 4, 12, 8, 2);
	if (!CheckRingConservation(12, 8, partial_overflow) || partial_overflow.read_index != 8 ||
	    partial_overflow.retained_samples != 8 || partial_overflow.write_index != 0 ||
	    partial_overflow.first_copy_samples != 8 || partial_overflow.queued_samples != 16 ||
	    partial_overflow.dropped_samples != 4)
	{
		return Fail("partial-overflow ring enqueue plan did not discard the minimum oldest samples");
	}

	const auto wrapping_copy = Host::Internal::PlanRingEnqueue(12, 6, 4, 6, 2);
	if (!CheckRingConservation(4, 6, wrapping_copy) || wrapping_copy.read_index != 6 || wrapping_copy.retained_samples != 4 ||
	    wrapping_copy.write_index != 10 || wrapping_copy.first_copy_samples != 2 || wrapping_copy.queued_samples != 10 ||
	    wrapping_copy.dropped_samples != 0)
	{
		return Fail("wrapping ring enqueue plan is incorrect");
	}

	std::array<int, 8> ring {{3, 3, 4, 4, 1, 1, 2, 2}};
	const std::array<int, 2> incoming {{5, 5}};
	const auto full_overflow = Host::Internal::PlanRingEnqueue(ring.size(), 4, ring.size(), incoming.size(), 2);
	if (!CheckRingConservation(ring.size(), incoming.size(), full_overflow) || full_overflow.dropped_samples != 2 ||
	    full_overflow.read_index != 6 || full_overflow.retained_samples != 6 || full_overflow.write_index != 4 ||
	    full_overflow.queued_samples != ring.size())
	{
		return Fail("full ring enqueue plan is incorrect");
	}
	std::copy_n(incoming.data(), full_overflow.first_copy_samples,
	            ring.begin() + static_cast<std::ptrdiff_t>(full_overflow.write_index));
	std::copy_n(incoming.data() + full_overflow.first_copy_samples,
	            incoming.size() - full_overflow.first_copy_samples, ring.begin());
	std::array<int, 8> logical {};
	for (size_t sample = 0; sample < logical.size(); sample++)
	{
		logical[sample] = ring[(full_overflow.read_index + sample) % ring.size()];
	}
	if (logical != std::array<int, 8> {{2, 2, 3, 3, 4, 4, 5, 5}})
	{
		return Fail("ring overflow did not preserve the newest queued frames before the new block");
	}

	std::array<int, 8> split_ring {{1, 1, 2, 2, 3, 3, 0, 0}};
	const std::array<int, 4> split_incoming {{4, 4, 5, 5}};
	const auto split_plan = Host::Internal::PlanRingEnqueue(split_ring.size(), 0, 6, split_incoming.size(), 2);
	std::copy_n(split_incoming.data(), split_plan.first_copy_samples,
	            split_ring.begin() + static_cast<std::ptrdiff_t>(split_plan.write_index));
	std::copy_n(split_incoming.data() + split_plan.first_copy_samples,
	            split_incoming.size() - split_plan.first_copy_samples, split_ring.begin());
	for (size_t sample = 0; sample < logical.size(); sample++)
	{
		logical[sample] = split_ring[(split_plan.read_index + sample) % split_ring.size()];
	}
	if (!CheckRingConservation(6, split_incoming.size(), split_plan) || split_plan.first_copy_samples != 2 ||
	    logical != std::array<int, 8> {{2, 2, 3, 3, 4, 4, 5, 5}})
	{
		return Fail("minimum-drop ring enqueue did not preserve order across a split copy");
	}

	if (Host::Internal::PlanRingEnqueue(0, 0, 0, 2, 2).valid ||
	    Host::Internal::PlanRingEnqueue(8, 8, 0, 2, 2).valid ||
	    Host::Internal::PlanRingEnqueue(8, 0, 10, 2, 2).valid ||
	    Host::Internal::PlanRingEnqueue(8, 0, 0, 0, 2).valid ||
	    Host::Internal::PlanRingEnqueue(8, 0, 0, 10, 2).valid ||
	    Host::Internal::PlanRingEnqueue(8, 0, 0, 2, 0).valid ||
	    Host::Internal::PlanRingEnqueue(7, 0, 0, 2, 2).valid ||
	    Host::Internal::PlanRingEnqueue(8, 1, 0, 2, 2).valid ||
	    Host::Internal::PlanRingEnqueue(8, 0, 3, 2, 2).valid ||
	    Host::Internal::PlanRingEnqueue(8, 0, 0, 3, 2).valid)
	{
		return Fail("invalid or non-frame-aligned ring state was accepted");
	}
	return true;
}

bool TestRingCapacityPlanning()
{
	constexpr size_t ANDROID_MINIMUM_FRAMES = 4096;
	if (Host::Internal::StereoRingCapacitySamples(256, ANDROID_MINIMUM_FRAMES) != 4096 * 2 ||
	    Host::Internal::StereoRingCapacitySamples(512, ANDROID_MINIMUM_FRAMES) != 4096 * 2 ||
	    Host::Internal::StereoRingCapacitySamples(768, ANDROID_MINIMUM_FRAMES) != 4096 * 2 ||
	    Host::Internal::StereoRingCapacitySamples(1024, ANDROID_MINIMUM_FRAMES) != 4096 * 2 ||
	    Host::Internal::StereoRingCapacitySamples(1280, ANDROID_MINIMUM_FRAMES) != 5120 * 2 ||
	    Host::Internal::StereoRingCapacitySamples(2048, ANDROID_MINIMUM_FRAMES) != 8192 * 2 ||
	    Host::Internal::StereoRingCapacitySamples(256, 0) != 1024 * 2 ||
	    Host::Internal::StereoRingCapacitySamples(0, ANDROID_MINIMUM_FRAMES) != 0 ||
	    Host::Internal::StereoRingCapacitySamples((std::numeric_limits<size_t>::max)(), ANDROID_MINIMUM_FRAMES) != 0 ||
	    Host::Internal::StereoRingCapacitySamples(1, (std::numeric_limits<size_t>::max)()) != 0)
	{
		return Fail("stereo ring capacity does not preserve four blocks and the Android frame floor");
	}

	const size_t capacity       = Host::Internal::StereoRingCapacitySamples(256, ANDROID_MINIMUM_FRAMES);
	size_t       read_index     = 0;
	size_t       queued_samples = 0;
	for (size_t block = 0; block < 16; block++)
	{
		const auto plan = Host::Internal::PlanRingEnqueue(capacity, read_index, queued_samples, 256 * 2, 2);
		if (!plan.valid || plan.dropped_samples != 0)
		{
			return Fail("Android ring floor dropped a frame inside a sixteen-block burst");
		}
		read_index     = plan.read_index;
		queued_samples = plan.queued_samples;
	}
	const auto overflow = Host::Internal::PlanRingEnqueue(capacity, read_index, queued_samples, 256 * 2, 2);
	if (!overflow.valid || overflow.dropped_samples != 256 * 2 || overflow.queued_samples != capacity)
	{
		return Fail("Android ring floor did not apply minimum-drop at the seventeenth 256-frame block");
	}
	return true;
}

bool CheckStats(const Host::StreamStats& stats, uint64_t expected_blocks, size_t input_block_bytes, size_t output_block_bytes)
{
	return stats.submitted_blocks == expected_blocks && stats.input_bytes == expected_blocks * input_block_bytes &&
	       stats.output_bytes == expected_blocks * output_block_bytes &&
	       stats.consumed_output_bytes == expected_blocks * output_block_bytes && stats.queued_output_bytes == 0 &&
	       stats.callback_calls > 0 && stats.callback_frames > 0 && stats.audible_callback_calls > 0 &&
	       stats.inaudible_blocks == 0 && stats.exact_silent_blocks == 0 && stats.max_submitted_peak > AUDIBLE_PEAK &&
	       stats.max_callback_peak > AUDIBLE_PEAK && stats.queue_resets == 0 && stats.queue_overflow_events == 0 &&
	       stats.dropped_overrun_frames == 0 && stats.submit_errors == 0;
}

bool SameStats(const Host::StreamStats& left, const Host::StreamStats& right)
{
	return left.submitted_blocks == right.submitted_blocks && left.inaudible_blocks == right.inaudible_blocks &&
	       left.exact_silent_blocks == right.exact_silent_blocks && left.input_bytes == right.input_bytes &&
	       left.output_bytes == right.output_bytes && left.consumed_output_bytes == right.consumed_output_bytes &&
	       left.queued_output_bytes == right.queued_output_bytes && left.callback_calls == right.callback_calls &&
	       left.callback_frames == right.callback_frames && left.audible_callback_calls == right.audible_callback_calls &&
	       left.underrun_frames == right.underrun_frames && left.queue_resets == right.queue_resets &&
	       left.queue_overflow_events == right.queue_overflow_events &&
	       left.dropped_overrun_frames == right.dropped_overrun_frames && left.submit_errors == right.submit_errors &&
	       left.capacity_wait_events == right.capacity_wait_events && left.capacity_wait_us == right.capacity_wait_us &&
	       left.capacity_wait_timeouts == right.capacity_wait_timeouts && left.capacity_wait_cancels == right.capacity_wait_cancels &&
	       left.max_submitted_peak == right.max_submitted_peak &&
	       left.max_callback_peak == right.max_callback_peak;
}

#if defined(LUMEN_AUDIO_OUT_TEST_HOOK)
bool TestOutputHostWait()
{
	using Lumen::Android::FexCoreLab::GuestMemoryMutationBarrier;
	using Lumen::Android::FexCoreLab::GuestMemoryMutationScope;
	using Lumen::Android::FexCoreLab::QuiescentHostWaitLease;
	using namespace std::chrono_literals;
	const auto wait_until = [](auto predicate) {
		const auto deadline = std::chrono::steady_clock::now() + 3s;
		while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
		return predicate();
	};
	for (const bool contending_reader: {false, true})
	{
		for (const bool error_return: {false, true})
		{
			GuestMemoryMutationBarrier barrier;
			std::mutex submit_mutex;
			std::atomic<bool> audio_waiting {false}, finish_audio {false}, audio_done {false};
			std::atomic<bool> peer_ready {false}, start_peer {false}, peer_acquired {false};
			std::atomic<bool> writer_acquired {false}, finish_writer {false}, writer_done {false};
			std::atomic<bool> transitions_ok {true};
			const auto enter_host_call = [&] {
				if (!barrier.EnterExecution() || !barrier.SuspendExecutionForHostCall()) transitions_ok.store(false);
			};
			const auto leave_host_call = [&] {
				if (!barrier.ResumeExecutionAfterHostCall()) transitions_ok.store(false);
				barrier.LeaveExecution();
			};
			std::thread audio([&] {
				enter_host_call();
				const auto submit_and_pace = [&] {
					// Same production scope: the port remains locked throughout
					// the native wait, then is released before reader readmission.
					Host::OutputHostWait<QuiescentHostWaitLease, std::unique_lock<std::mutex>> work(barrier);
					work.Locks() = std::unique_lock(submit_mutex);
					audio_waiting.store(true);
					while (!finish_audio.load()) std::this_thread::yield();
					if (error_return) return -1;
					return 2048;
				};
				if (submit_and_pace() != (error_return ? -1 : 2048)) transitions_ok.store(false);
				leave_host_call();
				audio_done.store(true);
			});
			bool ok = wait_until([&] { return audio_waiting.load(); });
			std::thread peer;
			if (contending_reader)
			{
				peer = std::thread([&] {
					enter_host_call();
					peer_ready.store(true);
					while (!start_peer.load()) std::this_thread::yield();
					{
						// Models close/reset or another already admitted HLE:
						// this reader cannot leave until the port becomes free.
						std::lock_guard lock(submit_mutex);
						peer_acquired.store(true);
					}
					leave_host_call();
				});
				ok = wait_until([&] { return peer_ready.load(); }) && ok;
			}
			std::thread writer([&] {
				const auto token = barrier.BeginMutation(GuestMemoryMutationScope::Exclusive,
				    [](uint64_t) noexcept { return true; }, [](uint64_t) noexcept {}, 2s);
				if (token != 0)
				{
					writer_acquired.store(true);
					while (!finish_writer.load()) std::this_thread::yield();
					if (!barrier.EndMutation(token)) transitions_ok.store(false);
				}
				writer_done.store(true);
			});
			if (contending_reader)
			{
				// Exclusive is pending on the peer's reader, while audio owns
				// the port. Releasing the port before readmission breaks the
				// potential port -> peer reader -> writer -> audio cycle.
				ok = wait_until([&] {
					const auto state = barrier.SnapshotForTesting();
					return state.requestEpoch != 0 && !state.writer && state.hostCallReaders == 1;
				}) && ok;
				start_peer.store(true);
				finish_audio.store(true);
				ok = wait_until([&] { return writer_acquired.load() || writer_done.load(); }) && ok;
				ok = writer_acquired.load() && peer_acquired.load() && ok;
			} else
			{
				// Exclusive must acquire while pacing is still blocked, not
				// only after the audio operation returns to guest execution.
				ok = wait_until([&] { return writer_acquired.load() || writer_done.load(); }) && ok;
				ok = writer_acquired.load() && !audio_done.load() && ok;
				finish_audio.store(true);
			}
			if (writer_acquired.load())
			{
				ok = wait_until([&] { return barrier.SnapshotForTesting().waitingEntrants != 0; }) && ok;
				const bool port_released = submit_mutex.try_lock();
				if (port_released) submit_mutex.unlock();
				ok = port_released && !audio_done.load() && ok;
			}
			// Always release the handshakes, including after a failed assertion.
			start_peer.store(true);
			finish_audio.store(true);
			finish_writer.store(true);
			writer.join();
			audio.join();
			if (peer.joinable()) peer.join();
			const auto state = barrier.SnapshotForTesting();
			if (!ok || !transitions_ok.load() || state.writer || state.requestEpoch != 0 ||
			    state.readers != 0 || state.hostCallReaders != 0 || state.waitingEntrants != 0)
				return Fail("AudioOut host wait blocked Exclusive progress or readmitted while holding a port lock");
		}
	}
	return true;
}

bool TestBlockingBackend()
{
	enum class Action { Consume, Cancel, Close, Shutdown, Timeout, NonBlocking };
	constexpr uint32_t FRAMES = 512;
	constexpr size_t FULL_BLOCKS = 9;
	const auto sample = [](size_t frame, size_t channel, float base) {
		return base + static_cast<float>(frame) / 65536.0f + static_cast<float>(channel) / 32.0f;
	};
	for (const auto action: {Action::Consume, Action::Cancel, Action::Close, Action::Shutdown, Action::Timeout,
	                         Action::NonBlocking})
	{
		Host::OutputBackend backend;
		std::string error;
		if (!backend.Initialize(&error) || backend.DriverName() != "dummy") return Fail("blocking test requires dummy audio");
		const auto spec = MakeSpec({Host::SampleFormat::Float32, 2, false}, FRAMES);
		auto left = backend.Open(spec, &error);
		auto right = backend.Open(spec, &error);
		if (left == nullptr || right == nullptr || !backend.TestSetManualCallbacks(true))
			return Fail("blocking test could not establish manual dummy callbacks");
		std::array<int, 8> volumes {};
		volumes.fill(FULL_VOLUME);
		std::vector<float> left_input(FRAMES * 2), right_input(FRAMES * 2);
		const auto fill = [&](std::vector<float>* input, size_t block, float base) {
			for (size_t frame = 0; frame < FRAMES; ++frame)
				for (size_t channel = 0; channel < 2; ++channel)
					(*input)[frame * 2 + channel] = sample(block * FRAMES + frame, channel, base);
		};
		for (size_t block = 0; block < FULL_BLOCKS; ++block)
		{
			fill(&left_input, block, 0.0625f);
			if (!left->Submit(left_input.data(), volumes.data(), &error)) return Fail("blocking test could not fill left ring");
			if (block + 1 < FULL_BLOCKS)
			{
				fill(&right_input, block, 0.125f);
				if (!right->Submit(right_input.data(), volumes.data(), &error)) return Fail("blocking test could not fill right ring");
			}
		}
		const auto left_before = left->GetStats();
		const auto right_before = right->GetStats();
		if (left_before.queued_output_bytes != FULL_BLOCKS * FRAMES * 8 ||
		    right_before.queued_output_bytes != (FULL_BLOCKS - 1) * FRAMES * 8)
			return Fail("blocking test ring capacity differs from the shared target plus guard block");
		fill(&left_input, FULL_BLOCKS, 0.0625f);
		fill(&right_input, FULL_BLOCKS - 1, 0.125f);
		const std::array<Host::OutputBatchItem, 2> batch {{
		    {left.get(), left_input.data(), volumes.data()}, {right.get(), right_input.data(), volumes.data()},
		}};
		if (action == Action::NonBlocking)
		{
			if (!backend.SubmitBatch(batch.data(), batch.size(), &error) ||
			    left->GetStats().dropped_overrun_frames != FRAMES || right->GetStats().dropped_overrun_frames != 0 ||
			    left->GetStats().capacity_wait_events != 0)
				return Fail("default nonblocking enqueue did not retain its bounded DropOldest behavior");
			continue;
		}
		std::atomic<bool> done {false};
		bool submitted = false;
		std::thread producer([&] {
			submitted = backend.SubmitBlockingBatch(batch.data(), batch.size(), &error);
			done.store(true);
		});
		const auto handshake_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (!done.load() && left->GetStats().capacity_wait_events == 0 &&
		       std::chrono::steady_clock::now() < handshake_deadline) std::this_thread::yield();
		if (done.load() || left->GetStats().capacity_wait_events != 1)
		{
			left->CancelPendingSubmissions();
			right->CancelPendingSubmissions();
			producer.join();
			return Fail("blocking producer did not enter an observable capacity wait");
		}
		if (left->GetStats().submitted_blocks != left_before.submitted_blocks ||
		    right->GetStats().submitted_blocks != right_before.submitted_blocks ||
		    left->GetStats().queued_output_bytes != left_before.queued_output_bytes ||
		    right->GetStats().queued_output_bytes != right_before.queued_output_bytes)
		{
			left->CancelPendingSubmissions();
			right->CancelPendingSubmissions();
			producer.join();
			return Fail("a blocked batch committed a peer before every ring had capacity");
		}
		// The wait handshake occurs after preparation. Mutating these caller buffers
		// now proves the pending submission owns its PCM and never rereads the guest.
		std::fill(left_input.begin(), left_input.end(), -0.9f);
		std::fill(right_input.begin(), right_input.end(), -0.9f);
		std::vector<float> output;
		bool callback_ok = true;
		const auto action_start = std::chrono::steady_clock::now();
		if (action == Action::Consume) callback_ok = backend.TestConsumeFrames(FRAMES, &output);
		if (action == Action::Cancel) left->CancelPendingSubmissions();
		if (action == Action::Close) left.reset();
		if (action == Action::Shutdown) callback_ok = backend.TestShutdownMixer();
		producer.join();
		if (!callback_ok) return Fail("manual callback or shutdown failed");
		const auto right_after = right->GetStats();
		if (action != Action::Consume)
		{
			const bool timeout = action == Action::Timeout;
			if (submitted || error.empty() || right_after.submitted_blocks != right_before.submitted_blocks ||
			    right_after.dropped_overrun_frames != 0 || right_after.queue_overflow_events != 0 ||
			    right_after.submit_errors != 1 || right_after.capacity_wait_timeouts != static_cast<uint64_t>(timeout) ||
			    right_after.capacity_wait_cancels != static_cast<uint64_t>(!timeout) ||
			    right_after.queued_output_bytes != (action == Action::Shutdown ? 0 : right_before.queued_output_bytes))
				return Fail("close, cancellation, or timeout acknowledged or mutated an uncommitted batch");
			const auto elapsed = std::chrono::steady_clock::now() - action_start;
			if (timeout && (elapsed < std::chrono::milliseconds(250) || elapsed > std::chrono::milliseconds(2500)))
				return Fail("broken-stream capacity wait did not fail within its bounded watchdog");
			continue;
		}
		if (!submitted || !error.empty()) return Fail("available capacity did not wake the complete prepared batch");
		const auto check_pcm = [&](size_t block) {
			for (size_t frame = 0; frame < FRAMES; ++frame)
				for (size_t channel = 0; channel < 2; ++channel)
					if (!Near(output[frame * 2 + channel], sample(block * FRAMES + frame, channel, 0.0625f) +
					                                            sample(block * FRAMES + frame, channel, 0.125f))) return false;
			return true;
		};
		if (!check_pcm(0)) return Fail("capacity release changed the first queued PCM block");
		fill(&right_input, FULL_BLOCKS, 0.125f);
		if (!right->Submit(right_input.data(), volumes.data(), &error)) return Fail("balanced callback drain could not submit its tail");
		for (size_t block = 1; block <= FULL_BLOCKS; ++block)
		{
			if (!backend.TestConsumeFrames(FRAMES, &output) || !check_pcm(block))
				return Fail("blocking batch lost PCM order or reread a caller buffer during the wait");
		}
		for (const auto* stream: {left.get(), right.get()})
		{
			const auto stats = stream->GetStats();
			if (stats.submitted_blocks != FULL_BLOCKS + 1 || stats.consumed_output_bytes != (FULL_BLOCKS + 1) * FRAMES * 8 ||
			    stats.queued_output_bytes != 0 || stats.underrun_frames != 0 || stats.dropped_overrun_frames != 0 ||
			    stats.queue_overflow_events != 0 || stats.submit_errors != 0 || stats.capacity_wait_events != 1 ||
			    stats.capacity_wait_us == 0 || stats.capacity_wait_timeouts != 0 || stats.capacity_wait_cancels != 0)
				return Fail("blocking batch did not conserve every queued and consumed PCM frame");
		}
		left.reset();
		right.reset();
		auto reopened = backend.Open(spec, &error);
		if (reopened == nullptr || !SameStats(reopened->GetStats(), Host::StreamStats {}))
			return Fail("closing and reopening retained capacity/cancellation state");
	}
	return true;
}
#endif

bool WaitForConsumption(Host::OutputStream* stream, uint64_t expected_output_bytes, Host::StreamStats* result)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	do
	{
		*result = stream->GetStats();
		if (result->consumed_output_bytes == expected_output_bytes && result->queued_output_bytes == 0 &&
		    result->callback_calls > 0)
		{
			return true;
		}
		SDL_Delay(1);
	} while (std::chrono::steady_clock::now() < deadline);

	return false;
}

bool CaptureDummyStats(Host::OutputStream* stream, const Host::StreamSpec& spec, const char* port_name, uint64_t expected_blocks,
	                   Host::StreamStats* result)
{
	const size_t host_output_bytes = static_cast<size_t>(spec.frames_per_block) * 2 * sizeof(float);
	if (!WaitForConsumption(stream, expected_blocks * host_output_bytes, result))
	{
		std::fprintf(stderr, "audio-output-test failed: dummy %s callback did not consume the submitted PCM\n", port_name);
		return false;
	}
	if (!CheckStats(*result, expected_blocks, Host::InputBlockBytes(spec), host_output_bytes))
	{
		std::fprintf(stderr, "audio-output-test failed: dummy %s per-port counters are inconsistent after submit\n", port_name);
		return false;
	}
	return true;
}

bool OpenDummyStream(Host::OutputBackend* backend, const Host::StreamSpec& spec, const char* port_name,
	                 std::unique_ptr<Host::OutputStream>* stream, std::string* error)
{
	if (backend->DriverName() != "dummy") return Fail("SDL driver changed before opening a stream");

	*stream = backend->Open(spec, error);
	if (*stream == nullptr)
	{
		std::fprintf(stderr, "audio-output-test failed: simultaneous dummy %s stream open: %s\n", port_name, error->c_str());
		return false;
	}
	return true;
}

bool SubmitDummyBlock(Host::OutputStream* stream, const Host::StreamSpec& spec, const char* port_name, uint64_t expected_blocks,
	                  Host::StreamStats* result, std::string* error)
{
	std::array<int, 8> volumes {};
	volumes.fill(FULL_VOLUME);
	const size_t samples   = static_cast<size_t>(spec.frames_per_block) * spec.channels;
	bool         submit_ok = false;
	if (spec.sample_format == Host::SampleFormat::Signed16)
	{
		std::vector<int16_t> input(samples);
		for (size_t i = 0; i < input.size(); i++)
			input[i] = static_cast<int16_t>(i % 127 + 1);
		submit_ok = stream->Submit(input.data(), volumes.data(), error);
	} else
	{
		std::vector<float> input(samples);
		for (size_t i = 0; i < input.size(); i++)
			input[i] = static_cast<float>(i % 31 + 1) / 32.0f;
		submit_ok = stream->Submit(input.data(), volumes.data(), error);
	}
	if (!submit_ok)
	{
		std::fprintf(stderr, "audio-output-test failed: dummy %s stream submit: %s\n", port_name, error->c_str());
		return false;
	}

	return CaptureDummyStats(stream, spec, port_name, expected_blocks, result);
}

bool TestSilentDummyCounters(Host::OutputBackend* backend, Host::StreamStats* result, std::string* error)
{
	Host::StreamSpec spec {};
	spec.sample_rate      = SAMPLE_RATE;
	spec.frames_per_block = 256;
	spec.channels         = 2;
	spec.sample_format    = Host::SampleFormat::Float32;

	std::unique_ptr<Host::OutputStream> stream;
	if (!OpenDummyStream(backend, spec, "silent-float-stereo", &stream, error))
	{
		return false;
	}

	std::array<int, 8> volumes {};
	volumes.fill(FULL_VOLUME);
	std::vector<float> silence(static_cast<size_t>(spec.frames_per_block) * spec.channels);
	if (!stream->Submit(silence.data(), volumes.data(), error))
	{
		std::fprintf(stderr, "audio-output-test failed: silent dummy submit: %s\n", error->c_str());
		return false;
	}

	const uint64_t expected_output_bytes = static_cast<uint64_t>(spec.frames_per_block) * 2 * sizeof(float);
	if (!WaitForConsumption(stream.get(), expected_output_bytes, result))
	{
		return Fail("silent dummy block was not consumed by the callback");
	}
	if (result->submitted_blocks != 1 || result->inaudible_blocks != 1 || result->exact_silent_blocks != 1 ||
	    result->consumed_output_bytes != expected_output_bytes || result->max_submitted_peak != 0.0f ||
	    result->max_callback_peak != 0.0f || result->audible_callback_calls != 0 || result->queue_resets != 0 ||
	    result->queue_overflow_events != 0 || result->dropped_overrun_frames != 0 || result->submit_errors != 0)
	{
		return Fail("silent/inaudible callback counters are inconsistent");
	}

	SDL_Delay(20);
	*result = stream->GetStats();
	if (result->underrun_frames == 0)
	{
		return Fail("empty dummy ring did not report callback underrun frames");
	}
	stream.reset();
	return true;
}

bool TestDummyBackend(std::string* driver, Host::StreamStats* signed_stats, Host::StreamStats* mono_stats,
	                  Host::StreamStats* standard_8ch_stats, Host::StreamStats* reopened_stats,
	                  Host::StreamStats* silent_stats)
{
	if (SDL_WasInit(SDL_INIT_AUDIO) != 0) return Fail("SDL audio was initialized before the fail-closed dummy test");

	Host::OutputBackend backend;
	std::string         error;
	if (!backend.Initialize(&error))
	{
		std::fprintf(stderr, "audio-output-test failed: SDL dummy initialization: %s\n", error.c_str());
		return false;
	}
	if (!backend.IsInitialized()) return Fail("backend did not report initialized state");

	*driver = backend.DriverName();
	if (*driver != "dummy") return Fail("SDL audio driver is not exactly dummy; refusing to open any stream");

	Host::StreamSpec signed_spec {};
	signed_spec.sample_rate      = SAMPLE_RATE;
	signed_spec.frames_per_block = 1024;
	signed_spec.channels         = 2;
	signed_spec.sample_format    = Host::SampleFormat::Signed16;

	Host::StreamSpec mono_spec {};
	mono_spec.sample_rate      = SAMPLE_RATE;
	mono_spec.frames_per_block = 256;
	mono_spec.channels         = 1;
	mono_spec.sample_format    = Host::SampleFormat::Float32;

	Host::StreamSpec standard_8ch_spec {};
	standard_8ch_spec.sample_rate      = SAMPLE_RATE;
	standard_8ch_spec.frames_per_block = 512;
	standard_8ch_spec.channels         = 8;
	standard_8ch_spec.sample_format    = Host::SampleFormat::Float32;
	standard_8ch_spec.standard_8ch     = true;

	std::unique_ptr<Host::OutputStream> signed_stream;
	std::unique_ptr<Host::OutputStream> mono_stream;
	std::unique_ptr<Host::OutputStream> standard_8ch_stream;
	if (!OpenDummyStream(&backend, signed_spec, "signed-stereo", &signed_stream, &error) ||
	    !OpenDummyStream(&backend, mono_spec, "float-mono", &mono_stream, &error) ||
	    !OpenDummyStream(&backend, standard_8ch_spec, "float-standard-8ch", &standard_8ch_stream, &error))
	{
		return false;
	}

	std::array<int, 8> batch_volumes {};
	batch_volumes.fill(FULL_VOLUME);
	std::vector<int16_t> signed_input(static_cast<size_t>(signed_spec.frames_per_block) * signed_spec.channels);
	std::vector<float>   mono_input(static_cast<size_t>(mono_spec.frames_per_block) * mono_spec.channels);
	std::vector<float>   standard_8ch_input(static_cast<size_t>(standard_8ch_spec.frames_per_block) * standard_8ch_spec.channels);
	for (size_t i = 0; i < signed_input.size(); i++) signed_input[i] = static_cast<int16_t>(i % 127 + 1);
	for (size_t i = 0; i < mono_input.size(); i++) mono_input[i] = static_cast<float>(i % 31 + 1) / 32.0f;
	for (size_t i = 0; i < standard_8ch_input.size(); i++)
		standard_8ch_input[i] = static_cast<float>(i % 31 + 1) / 32.0f;

	const std::array<Host::OutputBatchItem, 3> batch_items {{
	    {signed_stream.get(), signed_input.data(), batch_volumes.data()},
	    {mono_stream.get(), mono_input.data(), batch_volumes.data()},
	    {standard_8ch_stream.get(), standard_8ch_input.data(), batch_volumes.data()},
	}};
	const std::array<Host::StreamStats, 3> before_reject {{
	    signed_stream->GetStats(),
	    mono_stream->GetStats(),
	    standard_8ch_stream->GetStats(),
	}};
	const auto reject_left_stats_unchanged = [&]() {
		return SameStats(signed_stream->GetStats(), before_reject[0]) && SameStats(mono_stream->GetStats(), before_reject[1]) &&
		       SameStats(standard_8ch_stream->GetStats(), before_reject[2]);
	};

	const std::array<Host::OutputBatchItem, 2> duplicate_items {{batch_items[0], batch_items[0]}};
	if (backend.SubmitBatch(duplicate_items.data(), duplicate_items.size(), &error) || !reject_left_stats_unchanged())
	{
		return Fail("duplicate dummy batch was not rejected atomically");
	}
	auto invalid_items    = batch_items;
	invalid_items[1].data = nullptr;
	if (backend.SubmitBatch(invalid_items.data(), invalid_items.size(), &error) || !reject_left_stats_unchanged())
	{
		return Fail("invalid dummy batch item was not rejected atomically");
	}

	if (!backend.SubmitBatch(batch_items.data(), batch_items.size(), &error))
	{
		std::fprintf(stderr, "audio-output-test failed: heterogeneous dummy batch submit: %s\n", error.c_str());
		return false;
	}
	if (!CaptureDummyStats(signed_stream.get(), signed_spec, "signed-stereo", 1, signed_stats) ||
	    !CaptureDummyStats(mono_stream.get(), mono_spec, "float-mono", 1, mono_stats) ||
	    !CaptureDummyStats(standard_8ch_stream.get(), standard_8ch_spec, "float-standard-8ch", 1, standard_8ch_stats))
	{
		return false;
	}

	// Close the middle-opened stream first, then the first-opened stream. The
	// last stream must remain usable and must retain its own counters.
	mono_stream.reset();
	signed_stream.reset();
	if (backend.DriverName() != "dummy") return Fail("SDL driver changed while simultaneous streams were alive");
	if (!SubmitDummyBlock(standard_8ch_stream.get(), standard_8ch_spec, "float-standard-8ch", 2, standard_8ch_stats, &error))
	{
		return false;
	}
	standard_8ch_stream.reset();

	std::unique_ptr<Host::OutputStream> reopened_stream;
	if (!OpenDummyStream(&backend, signed_spec, "reopened-signed-stereo", &reopened_stream, &error) ||
	    !SubmitDummyBlock(reopened_stream.get(), signed_spec, "reopened-signed-stereo", 1, reopened_stats, &error))
	{
		return false;
	}
	reopened_stream.reset();
	return TestSilentDummyCounters(&backend, silent_stats, &error);
}

bool RunPhysicalCoreAudioProbe()
{
#if !defined(__APPLE__)
	return Fail("the physical CoreAudio probe is available only on Apple hosts");
#else
	if (SDL_WasInit(SDL_INIT_AUDIO) != 0)
	{
		return Fail("SDL audio was initialized before the physical probe");
	}

	Host::OutputBackend backend;
	std::string         error;
	if (!backend.Initialize(&error))
	{
		std::fprintf(stderr, "audio-output-test failed: physical SDL initialization: %s\n", error.c_str());
		return false;
	}
	const std::string driver = backend.DriverName();
	if (driver != "coreaudio")
	{
		std::fprintf(stderr, "audio-output-test failed: physical probe requires driver=coreaudio, got driver=%s\n",
		             driver.empty() ? "<none>" : driver.c_str());
		return false;
	}

	const int device_count = SDL_GetNumAudioDevices(0);
	std::printf("physical-coreaudio-probe devices=%d\n", device_count);
	for (int device = 0; device < device_count; device++)
	{
		const char* name = SDL_GetAudioDeviceName(device, 0);
		std::printf("physical-coreaudio-probe device[%d]=%s\n", device, name != nullptr ? name : "<unknown>");
	}

	Host::StreamSpec spec {};
	spec.sample_rate      = SAMPLE_RATE;
	spec.frames_per_block = 256;
	spec.channels         = 2;
	spec.sample_format    = Host::SampleFormat::Float32;
	auto stream           = backend.Open(spec, &error);
	if (stream == nullptr)
	{
		std::fprintf(stderr, "audio-output-test failed: physical CoreAudio stream open: %s\n", error.c_str());
		return false;
	}

	constexpr double PI               = 3.14159265358979323846;
	constexpr double FREQUENCY        = 440.0;
	constexpr float  AMPLITUDE        = 0.08f;
	constexpr uint64_t DURATION_FRAMES = 3u * SAMPLE_RATE;
	constexpr uint64_t FADE_FRAMES     = SAMPLE_RATE / 20u;
	const uint64_t     block_count = (DURATION_FRAMES + spec.frames_per_block - 1u) / spec.frames_per_block;
	const uint64_t     generated_frames = block_count * spec.frames_per_block;
	std::vector<float> samples(static_cast<size_t>(spec.frames_per_block) * spec.channels);
	std::array<int, 8> volumes {};
	volumes.fill(FULL_VOLUME);
	double phase = 0.0;
	const double phase_step = 2.0 * PI * FREQUENCY / SAMPLE_RATE;
	const auto   start      = std::chrono::steady_clock::now();

	std::printf("physical-coreaudio-probe tone=440Hz amplitude=0.08 duration=3s\n");
	for (uint64_t block = 0; block < block_count; block++)
	{
		for (uint32_t frame = 0; frame < spec.frames_per_block; frame++)
		{
			const uint64_t absolute_frame = block * spec.frames_per_block + frame;
			const uint64_t fade_out_frames = generated_frames - absolute_frame - 1u;
			const float fade = static_cast<float>(std::min<uint64_t>({absolute_frame, fade_out_frames, FADE_FRAMES})) /
			                   static_cast<float>(FADE_FRAMES);
			const float value = AMPLITUDE * fade * static_cast<float>(std::sin(phase));
			samples[static_cast<size_t>(frame) * 2]     = value;
			samples[static_cast<size_t>(frame) * 2 + 1] = value;
			phase += phase_step;
			if (phase >= 2.0 * PI)
			{
				phase -= 2.0 * PI;
			}
		}
		if (!stream->Submit(samples.data(), volumes.data(), &error))
		{
			std::fprintf(stderr, "audio-output-test failed: physical CoreAudio submit: %s\n", error.c_str());
			return false;
		}
		const uint64_t elapsed_us = (block + 1u) * spec.frames_per_block * 1000000u / SAMPLE_RATE;
		std::this_thread::sleep_until(start + std::chrono::microseconds(elapsed_us));
	}

	Host::StreamStats stats {};
	const uint64_t expected_output_bytes = block_count * spec.frames_per_block * 2u * sizeof(float);
	if (!WaitForConsumption(stream.get(), expected_output_bytes, &stats))
	{
		return Fail("physical CoreAudio callback did not drain the generated tone");
	}
	if (stats.callback_calls == 0 || stats.consumed_output_bytes != expected_output_bytes ||
	    stats.audible_callback_calls == 0 || stats.max_submitted_peak < 0.07f || stats.max_callback_peak < 0.07f ||
	    stats.queue_resets != 0 || stats.queue_overflow_events != 0 || stats.dropped_overrun_frames != 0 ||
	    stats.submit_errors != 0)
	{
		return Fail("physical CoreAudio counters did not prove audible PCM delivery");
	}

	std::printf("physical-coreaudio-probe=PASS driver=%s callbacks=%" PRIu64 " consumed_bytes=%" PRIu64
	            " submitted_peak=%.6f callback_peak=%.6f underrun_frames=%" PRIu64 "\n",
	            driver.c_str(), stats.callback_calls, stats.consumed_output_bytes, stats.max_submitted_peak,
	            stats.max_callback_peak, stats.underrun_frames);
	return true;
#endif
}

} // namespace

int main(int argc, char* argv[])
{
	if (argc == 3 && std::strcmp(argv[1], "--physical-coreaudio-probe") == 0 &&
	    std::strcmp(argv[2], "--allow-physical-audio") == 0)
	{
		return RunPhysicalCoreAudioProbe() ? 0 : 1;
	}
	if (argc != 1)
	{
		std::fprintf(stderr,
		             "usage: lumen_audio_out_test [--physical-coreaudio-probe --allow-physical-audio]\n");
		return 2;
	}

	if (SDL_setenv("SDL_AUDIODRIVER", "dummy", 1) != 0)
	{
		std::fprintf(stderr, "audio-output-test failed: unable to force SDL_AUDIODRIVER=dummy: %s\n", SDL_GetError());
		return 1;
	}
	if (SDL_WasInit(SDL_INIT_AUDIO) != 0)
	{
		std::fprintf(stderr, "audio-output-test failed: SDL audio was already initialized; refusing physical-driver fallback\n");
		return 1;
	}

	if (!TestFormatShapes() || !TestInvalidSpecsAndDestination() || !TestSigned16Extremes() || !TestFloatSaturation() ||
	    !TestVolumeClamping() || !TestDownmixToStereo() || !TestPacingDeadline() || !TestBufferedPacingContinuity() ||
	    !TestBufferedPacingBurstyConsumer() || !TestRingEnqueuePlanning() ||
	    !TestRingCapacityPlanning())
	{
		return 1;
	}

#if defined(LUMEN_AUDIO_OUT_TEST_HOOK)
	if (!TestOutputHostWait() || !TestBlockingBackend()) return 1;
#endif

	std::string       driver;
	Host::StreamStats signed_stats {};
	Host::StreamStats mono_stats {};
	Host::StreamStats standard_8ch_stats {};
	Host::StreamStats reopened_stats {};
	Host::StreamStats silent_stats {};
	if (!TestDummyBackend(&driver, &signed_stats, &mono_stats, &standard_8ch_stats, &reopened_stats, &silent_stats)) return 1;
	if (SDL_WasInit(SDL_INIT_AUDIO) != 0)
	{
		Fail("backend destruction did not release the SDL audio subsystem");
		return 1;
	}

	std::printf("audio-output-test=PASS driver=%s blocks=%" PRIu64 " input_bytes=%" PRIu64 " output_bytes=%" PRIu64
	            " consumed_bytes=%" PRIu64 " callbacks=%" PRIu64 " underrun_frames=%" PRIu64 " resets=%" PRIu64
	            " overflows=%" PRIu64 " dropped_overrun_frames=%" PRIu64 " errors=%" PRIu64 "\n",
	            driver.c_str(),
	            signed_stats.submitted_blocks + mono_stats.submitted_blocks + standard_8ch_stats.submitted_blocks +
	                reopened_stats.submitted_blocks + silent_stats.submitted_blocks,
	            signed_stats.input_bytes + mono_stats.input_bytes + standard_8ch_stats.input_bytes + reopened_stats.input_bytes +
	                silent_stats.input_bytes,
	            signed_stats.output_bytes + mono_stats.output_bytes + standard_8ch_stats.output_bytes + reopened_stats.output_bytes +
	                silent_stats.output_bytes,
	            signed_stats.consumed_output_bytes + mono_stats.consumed_output_bytes + standard_8ch_stats.consumed_output_bytes +
	                reopened_stats.consumed_output_bytes + silent_stats.consumed_output_bytes,
	            signed_stats.callback_calls + mono_stats.callback_calls + standard_8ch_stats.callback_calls +
	                reopened_stats.callback_calls + silent_stats.callback_calls,
	            signed_stats.underrun_frames + mono_stats.underrun_frames + standard_8ch_stats.underrun_frames +
	                reopened_stats.underrun_frames + silent_stats.underrun_frames,
	            signed_stats.queue_resets + mono_stats.queue_resets + standard_8ch_stats.queue_resets + reopened_stats.queue_resets +
	                silent_stats.queue_resets,
	            signed_stats.queue_overflow_events + mono_stats.queue_overflow_events + standard_8ch_stats.queue_overflow_events +
	                reopened_stats.queue_overflow_events + silent_stats.queue_overflow_events,
	            signed_stats.dropped_overrun_frames + mono_stats.dropped_overrun_frames +
	                standard_8ch_stats.dropped_overrun_frames + reopened_stats.dropped_overrun_frames +
	                silent_stats.dropped_overrun_frames,
	            signed_stats.submit_errors + mono_stats.submit_errors + standard_8ch_stats.submit_errors + reopened_stats.submit_errors +
	                silent_stats.submit_errors);
	return 0;
}
