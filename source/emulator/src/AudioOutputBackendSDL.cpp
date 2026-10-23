#include "Emulator/AudioOutputBackend.h"

#include "SDL.h"
#include "SDL_audio.h"
#include "SDL_error.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Audio::Host {

namespace {

constexpr size_t MAX_STREAMS          = 32;
constexpr size_t MIN_QUEUED_BLOCKS    = 4;
constexpr size_t HOST_CHANNELS        = 2;
#if defined(__ANDROID__)
constexpr size_t MIN_QUEUED_FRAMES    = 4096;
#else
constexpr size_t MIN_QUEUED_FRAMES    = 0;
#endif
constexpr Uint16 HOST_CALLBACK_FRAMES = 256;
constexpr uint64_t BUFFERED_QUEUE_TARGET_US = 80000;
constexpr size_t   MAX_BUFFERED_TARGET_FRAMES = 8192;
constexpr int    MAX_VOLUME          = 32768;
constexpr float  CENTER_GAIN         = 0.7071067811865475f;
constexpr float  LOW_FREQUENCY_GAIN  = 0.5f;
constexpr float  SURROUND_GAIN       = 0.5f;
constexpr float  AUDIBLE_PEAK         = 1.0e-6f;

void ClearError(std::string* error)
{
	if (error != nullptr)
	{
		error->clear();
	}
}

void SetError(std::string* error, std::string message)
{
	if (error != nullptr)
	{
		*error = std::move(message);
	}
}

void SetSdlError(std::string* error, const char* operation)
{
	if (error == nullptr)
	{
		return;
	}

	*error             = operation;
	const char* detail = SDL_GetError();
	if (detail != nullptr && detail[0] != '\0')
	{
		error->append(": ");
		error->append(detail);
	}
}

bool CheckedMultiply(size_t left, size_t right, size_t* result)
{
	if (result == nullptr || (right != 0 && left > std::numeric_limits<size_t>::max() / right))
	{
		return false;
	}

	*result = left * right;
	return true;
}

bool GetBytesPerSample(SampleFormat format, size_t* bytes_per_sample)
{
	if (bytes_per_sample == nullptr)
	{
		return false;
	}

	switch (format)
	{
		case SampleFormat::Signed16: *bytes_per_sample = sizeof(int16_t); return true;
		case SampleFormat::Float32: *bytes_per_sample = sizeof(float); return true;
	}

	return false;
}

bool GetSampleCount(const StreamSpec& spec, size_t* sample_count)
{
	if (sample_count == nullptr || spec.sample_rate == 0 || spec.sample_rate > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
	    spec.frames_per_block == 0 || spec.frames_per_block > std::numeric_limits<uint16_t>::max())
	{
		return false;
	}

	if (spec.channels != 1 && spec.channels != 2 && spec.channels != 8)
	{
		return false;
	}

	if (spec.standard_8ch && spec.channels != 8)
	{
		return false;
	}

	size_t bytes_per_sample = 0;
	if (!GetBytesPerSample(spec.sample_format, &bytes_per_sample))
	{
		return false;
	}

	return CheckedMultiply(static_cast<size_t>(spec.frames_per_block), static_cast<size_t>(spec.channels), sample_count);
}

size_t SourceChannel(const StreamSpec& spec, size_t output_channel)
{
	static constexpr size_t STD_8CH_TO_CANONICAL[8] = {0, 1, 2, 3, 6, 7, 4, 5};

	return spec.standard_8ch ? STD_8CH_TO_CANONICAL[output_channel] : output_channel;
}

float ReadSigned16(const uint8_t* input)
{
	const uint32_t bits         = static_cast<uint32_t>(input[0]) | (static_cast<uint32_t>(input[1]) << 8u);
	const int32_t  signed_value = (bits < 0x8000u ? static_cast<int32_t>(bits) : static_cast<int32_t>(bits) - 0x10000);

	return static_cast<float>(signed_value) / 32768.0f;
}

float ReadFloat32(const uint8_t* input)
{
	const uint32_t bits = static_cast<uint32_t>(input[0]) | (static_cast<uint32_t>(input[1]) << 8u) |
	                      (static_cast<uint32_t>(input[2]) << 16u) | (static_cast<uint32_t>(input[3]) << 24u);
	float value = 0.0f;
	static_assert(sizeof(value) == sizeof(bits), "32-bit float samples are required");
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

float ClampSample(float sample)
{
	if (std::isnan(sample))
	{
		return 0.0f;
	}
	if (sample < -1.0f)
	{
		return -1.0f;
	}
	if (sample > 1.0f)
	{
		return 1.0f;
	}
	return sample;
}

bool DownmixToStereoImpl(const StreamSpec& spec, const float* canonical, size_t canonical_samples, float* stereo,
                         size_t stereo_samples)
{
	if (canonical == nullptr || stereo == nullptr)
	{
		return false;
	}

	size_t expected_canonical = 0;
	size_t expected_stereo    = 0;
	if (!GetSampleCount(spec, &expected_canonical) ||
	    !CheckedMultiply(static_cast<size_t>(spec.frames_per_block), HOST_CHANNELS, &expected_stereo) ||
	    canonical_samples != expected_canonical || stereo_samples != expected_stereo)
	{
		return false;
	}

	for (size_t frame = 0; frame < static_cast<size_t>(spec.frames_per_block); frame++)
	{
		const size_t stereo_offset = frame * HOST_CHANNELS;
		if (spec.channels == 1)
		{
			const float sample       = canonical[frame];
			stereo[stereo_offset]     = sample;
			stereo[stereo_offset + 1] = sample;
		} else if (spec.channels == 2)
		{
			const size_t source       = frame * 2;
			stereo[stereo_offset]     = canonical[source];
			stereo[stereo_offset + 1] = canonical[source + 1];
		} else
		{
			// Canonical order is FL, FR, FC, LFE, BL, BR, SL, SR. Keep the
			// headroom decision at the final mixer clamp so multiple ports sum once.
			const size_t source = frame * 8;
			stereo[stereo_offset] = canonical[source] + CENTER_GAIN * canonical[source + 2] +
			                        LOW_FREQUENCY_GAIN * canonical[source + 3] + SURROUND_GAIN * canonical[source + 4] +
			                        SURROUND_GAIN * canonical[source + 6];
			stereo[stereo_offset + 1] = canonical[source + 1] + CENTER_GAIN * canonical[source + 2] +
			                            LOW_FREQUENCY_GAIN * canonical[source + 3] + SURROUND_GAIN * canonical[source + 5] +
			                            SURROUND_GAIN * canonical[source + 7];
		}
	}

	return true;
}

struct MixerStreamState
{
	MixerStreamState(StreamSpec stream_spec, size_t stream_input_bytes, size_t stream_canonical_samples, size_t stream_stereo_samples,
	                 size_t ring_samples)
	    : spec(stream_spec), input_bytes(stream_input_bytes), stereo_block_samples(stream_stereo_samples),
	      stereo_block_bytes(stream_stereo_samples * sizeof(float)), ring(ring_samples), canonical(stream_canonical_samples),
	      stereo(stream_stereo_samples)
	{
	}

	StreamSpec         spec;
	size_t             input_bytes          = 0;
	size_t             stereo_block_samples = 0;
	size_t             stereo_block_bytes   = 0;
	std::vector<float> ring;
	std::vector<float> canonical;
	std::vector<float> stereo;
	std::mutex         submit_mutex;
	OutputStream*      owner          = nullptr;
	size_t             read_index     = 0;
	size_t             queued_samples = 0;
	StreamStats        stats;
	bool               registered = false;
	bool               closing_requested = false;
};

bool PrepareStreamBlock(const std::shared_ptr<MixerStreamState>& state, const void* data, const int canonical_volumes[8])
{
	return state != nullptr &&
	       ConvertBlockToFloat(state->spec, data, canonical_volumes, state->canonical.data(), state->canonical.size()) &&
	       DownmixBlockToStereo(state->spec, state->canonical.data(), state->canonical.size(), state->stereo.data(),
	                            state->stereo.size());
}

class SdlMixer final
{
public:
	SdlMixer() { m_streams.reserve(MAX_STREAMS); }

	~SdlMixer() { Shutdown(); }

	SdlMixer(const SdlMixer&)            = delete;
	SdlMixer& operator=(const SdlMixer&) = delete;

	void Activate()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_accepting_streams = true;
	}

	[[nodiscard]] bool Register(const std::shared_ptr<MixerStreamState>& state, std::string* error)
	{
		if (state == nullptr || state->owner == nullptr)
		{
			SetError(error, "invalid audio output stream state");
			return false;
		}

		std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mutex);
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_accepting_streams)
			{
				SetError(error, "audio output backend is shutting down");
				return false;
			}
			if (state->registered || m_streams.size() >= MAX_STREAMS)
			{
				SetError(error, "audio output stream limit reached");
				return false;
			}
			if (m_device != 0 && m_device_rate != state->spec.sample_rate)
			{
				SetError(error, "audio output stream rate differs from the shared device rate");
				return false;
			}
		}

		if (!OpenDeviceIfNeeded(state->spec, error))
		{
			return false;
		}

		std::lock_guard<std::mutex> lock(m_mutex);
		m_streams.push_back(state);
		state->registered = true;
		return true;
	}

	[[nodiscard]] bool ResolveBatch(const OutputBatchItem* items, size_t count,
	                                std::vector<std::shared_ptr<MixerStreamState>>* states, std::string* error) const
	{
		if (items == nullptr || states == nullptr || count == 0 || count > MAX_STREAMS || states->capacity() < count)
		{
			SetError(error, "invalid audio output batch");
			return false;
		}

		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_accepting_streams)
		{
			SetError(error, "audio output backend is shutting down");
			return false;
		}

		for (size_t item_index = 0; item_index < count; item_index++)
		{
			const auto& item = items[item_index];
			if (item.stream == nullptr || item.data == nullptr || item.canonical_volumes == nullptr)
			{
				SetError(error, "invalid audio output batch item");
				return false;
			}

			const auto position = std::find_if(m_streams.begin(), m_streams.end(), [&item](const auto& state) {
				return state->registered && state->owner == item.stream;
			});
			if (position == m_streams.end())
			{
				SetError(error, "audio output batch stream is not owned by this backend");
				return false;
			}
			if (std::find(states->begin(), states->end(), *position) != states->end())
			{
				SetError(error, "audio output batch contains a duplicate stream");
				return false;
			}
			states->push_back(*position);
		}

		return true;
	}

	void Unregister(const std::shared_ptr<MixerStreamState>& state)
	{
		if (state == nullptr)
		{
			return;
		}

		std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mutex);
		SDL_AudioDeviceID           device_to_close = 0;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!state->registered)
			{
				return;
			}

			const auto position = std::find(m_streams.begin(), m_streams.end(), state);
			if (position != m_streams.end())
			{
				m_streams.erase(position);
			}
			state->registered               = false;
			state->owner                    = nullptr;
			state->read_index               = 0;
			state->queued_samples           = 0;
			state->stats.queued_output_bytes = 0;

			if (m_streams.empty() && m_device != 0)
			{
				device_to_close = m_device;
				m_device        = 0;
				m_device_rate   = 0;
				m_device_paused = true;
			}
		}
		m_queue_space.notify_all();

		CloseDevice(device_to_close);
	}

	void CancelPendingSubmissions(const std::shared_ptr<MixerStreamState>& state)
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			state->closing_requested = true;
		}
		m_queue_space.notify_all();
	}

	[[nodiscard]] bool WaitForQueueSpace(const std::shared_ptr<MixerStreamState>* states, size_t count, std::string* error)
	{
		using Clock = std::chrono::steady_clock;
		const auto start = Clock::now();
		std::unique_lock<std::mutex> lock(m_mutex);
		uint64_t max_ring_us = 0;
		for (size_t i = 0; i < count; ++i)
		{
			const auto& state = states[i];
			if (state == nullptr || state->spec.sample_rate == 0)
			{
				SetError(error, "invalid audio output capacity wait");
				return false;
			}
			max_ring_us = std::max(max_ring_us, static_cast<uint64_t>(state->ring.size() / HOST_CHANNELS) * 1000000 /
			                                      state->spec.sample_rate);
		}
		// Broken-stream watchdog, independent of normal pacing. A timeout is a
		// failed submission: no PCM is committed, repeated, or silently dropped.
		const uint64_t budget_us = std::clamp(max_ring_us * 4, uint64_t {250000}, uint64_t {2000000});
		const auto deadline = start + std::chrono::microseconds(budget_us);
		bool waited = false;
		const auto finish_wait = [&](bool cancelled, bool timed_out) {
			if (!waited) return;
			const uint64_t elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			    Clock::now() - start).count());
			for (size_t i = 0; i < count; ++i)
			{
				states[i]->stats.capacity_wait_us += elapsed;
				states[i]->stats.capacity_wait_cancels += static_cast<uint64_t>(cancelled);
				states[i]->stats.capacity_wait_timeouts += static_cast<uint64_t>(timed_out);
			}
		};
		for (;;)
		{
			bool ready = true;
			for (size_t i = 0; i < count; ++i)
			{
				const auto& state = states[i];
				if (!m_accepting_streams || m_device == 0 || !state->registered || state->owner == nullptr ||
				    state->closing_requested)
				{
					finish_wait(true, false);
					SetError(error, "audio output capacity wait cancelled by close");
					return false;
				}
				if (state->queued_samples > state->ring.size() || state->stereo_block_samples > state->ring.size())
				{
					finish_wait(false, false);
					SetError(error, "invalid audio output ring capacity");
					return false;
				}
				ready = ready && state->stereo_block_samples <= state->ring.size() - state->queued_samples;
			}
			if (ready)
			{
				finish_wait(false, false);
				return true;
			}
			if (!waited)
			{
				waited = true;
				for (size_t i = 0; i < count; ++i) ++states[i]->stats.capacity_wait_events;
			}
			if (Clock::now() >= deadline)
			{
				finish_wait(false, true);
				SetError(error, "audio output capacity wait timed out without sufficient space");
				return false;
			}
			m_queue_space.wait_until(lock, deadline);
		}
	}

	[[nodiscard]] bool EnqueueBatch(const std::shared_ptr<MixerStreamState>* states, size_t count, std::string* error,
	                               bool preserve_queued_pcm = false)
	{
		if (states == nullptr || count == 0 || count > MAX_STREAMS)
		{
			SetError(error, "invalid prepared audio output batch");
			return false;
		}

		std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mutex);
		SDL_AudioDeviceID           device_to_resume = 0;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			std::array<Internal::RingEnqueuePlan, MAX_STREAMS> plans {};
			if (!m_accepting_streams || m_device == 0)
			{
				SetError(error, "audio output backend is closed");
				return false;
			}

			for (size_t state_index = 0; state_index < count; state_index++)
			{
				const auto& state = states[state_index];
				if (state == nullptr || !state->registered || state->owner == nullptr || state->closing_requested ||
				    state->stereo.size() != state->stereo_block_samples || state->stereo_block_samples > state->ring.size() ||
				    state->queued_samples > state->ring.size())
				{
					SetError(error, "audio output batch contains a closed or invalid stream");
					return false;
				}
				for (size_t previous = 0; previous < state_index; previous++)
				{
					if (states[previous] == state)
					{
						SetError(error, "audio output batch contains a duplicate stream");
						return false;
					}
				}
				plans[state_index] =
				    Internal::PlanRingEnqueue(state->ring.size(), state->read_index, state->queued_samples,
				                              state->stereo_block_samples, HOST_CHANNELS);
				if (!plans[state_index].valid)
				{
					SetError(error, "audio output batch contains invalid ring state");
					return false;
				}
				if (preserve_queued_pcm && plans[state_index].dropped_samples != 0)
				{
					SetError(error, "audio output blocking batch lost its reserved capacity");
					return false;
				}
			}

			for (size_t state_index = 0; state_index < count; state_index++)
			{
				const auto& state = states[state_index];
				float       submitted_peak = 0.0f;
				for (const float sample: state->stereo)
				{
					if (std::isfinite(sample))
					{
						submitted_peak = std::max(submitted_peak, std::fabs(sample));
					}
				}
				const auto& plan = plans[state_index];
				state->read_index     = plan.read_index;
				state->queued_samples = plan.retained_samples;
				if (plan.dropped_samples != 0)
				{
					state->stats.queue_overflow_events++;
					state->stats.dropped_overrun_frames += plan.dropped_samples / HOST_CHANNELS;
				}

				std::copy_n(state->stereo.data(), plan.first_copy_samples,
				            state->ring.begin() + static_cast<std::ptrdiff_t>(plan.write_index));
				std::copy_n(state->stereo.data() + plan.first_copy_samples,
				            state->stereo_block_samples - plan.first_copy_samples, state->ring.begin());
				state->queued_samples = plan.queued_samples;
				state->stats.submitted_blocks++;
				if (submitted_peak <= AUDIBLE_PEAK)
				{
					state->stats.inaudible_blocks++;
				}
				if (submitted_peak == 0.0f)
				{
					state->stats.exact_silent_blocks++;
				}
				state->stats.input_bytes += state->input_bytes;
				state->stats.output_bytes += state->stereo_block_bytes;
				state->stats.queued_output_bytes = state->queued_samples * sizeof(float);
				state->stats.max_submitted_peak = std::max(state->stats.max_submitted_peak, submitted_peak);
			}

			if (m_device_paused)
			{
				m_device_paused = false;
#if defined(LUMEN_AUDIO_OUT_TEST_HOOK)
				if (!m_test_manual_callbacks)
#endif
				device_to_resume = m_device;
			}
		}

		if (device_to_resume != 0)
		{
			SDL_PauseAudioDevice(device_to_resume, 0);
		}
		return true;
	}

	void RecordSubmitError(const std::shared_ptr<MixerStreamState>& state)
	{
		if (state != nullptr)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			state->stats.submit_errors++;
		}
	}

	[[nodiscard]] StreamStats GetStats(const std::shared_ptr<MixerStreamState>& state) const
	{
		if (state == nullptr)
		{
			return {};
		}

		std::lock_guard<std::mutex> lock(m_mutex);
		state->stats.queued_output_bytes = state->queued_samples * sizeof(float);
		return state->stats;
	}

	void Shutdown()
	{
		std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mutex);
		SDL_AudioDeviceID           device_to_close = 0;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_accepting_streams = false;
			for (const auto& state: m_streams)
			{
				state->registered                = false;
				state->owner                     = nullptr;
				state->read_index                = 0;
				state->queued_samples            = 0;
				state->stats.queued_output_bytes = 0;
			}
			m_streams.clear();
			device_to_close = m_device;
			m_device        = 0;
			m_device_rate   = 0;
			m_device_paused = true;
#if defined(LUMEN_AUDIO_OUT_TEST_HOOK)
			m_test_manual_callbacks = false;
#endif
		}
		m_queue_space.notify_all();

		CloseDevice(device_to_close);
	}

#if defined(LUMEN_AUDIO_OUT_TEST_HOOK)
	[[nodiscard]] bool TestSetManualCallbacks(bool manual)
	{
		std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mutex);
		const char* driver = SDL_GetCurrentAudioDriver();
		if (driver == nullptr || std::strcmp(driver, "dummy") != 0) return false;
		SDL_AudioDeviceID device = 0;
		bool paused = true;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_accepting_streams || m_device == 0) return false;
			m_test_manual_callbacks = manual;
			device = m_device;
			paused = manual || m_device_paused;
		}
		SDL_PauseAudioDevice(device, paused ? 1 : 0);
		return true;
	}

	[[nodiscard]] bool TestConsumeFrames(size_t frames, std::vector<float>* output)
	{
		std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mutex);
		if (output == nullptr || frames == 0 || frames > HOST_CALLBACK_FRAMES * 64) return false;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_test_manual_callbacks || !m_accepting_streams || m_device == 0) return false;
		}
		output->assign(frames * HOST_CHANNELS, 0.0f);
		Mix(reinterpret_cast<Uint8*>(output->data()), static_cast<int>(output->size() * sizeof(float)));
		return true;
	}

	[[nodiscard]] bool TestShutdownMixer()
	{
		const char* driver = SDL_GetCurrentAudioDriver();
		if (driver == nullptr || std::strcmp(driver, "dummy") != 0) return false;
		Shutdown();
		return true;
	}
#endif

private:
	[[nodiscard]] bool OpenDeviceIfNeeded(const StreamSpec& spec, std::string* error)
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_device != 0)
			{
				return true;
			}
		}

		SDL_AudioSpec desired {};
		desired.freq     = static_cast<int>(spec.sample_rate);
		desired.format   = AUDIO_F32SYS;
		desired.channels = static_cast<Uint8>(HOST_CHANNELS);
		desired.samples  = HOST_CALLBACK_FRAMES;
		desired.callback = AudioCallback;
		desired.userdata = this;

		SDL_AudioSpec           obtained {};
		const SDL_AudioDeviceID device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
		if (device == 0)
		{
			SetSdlError(error, "SDL_OpenAudioDevice failed");
			return false;
		}

		if (obtained.freq != desired.freq || obtained.format != desired.format || obtained.channels != desired.channels)
		{
			SDL_CloseAudioDevice(device);
			SetError(error, "SDL audio device did not provide the requested stereo format");
			return false;
		}

		SDL_PauseAudioDevice(device, 1);
		std::lock_guard<std::mutex> lock(m_mutex);
		m_device        = device;
		m_device_rate   = spec.sample_rate;
		m_device_paused = true;
		return true;
	}

	static void CloseDevice(SDL_AudioDeviceID device)
	{
		if (device != 0)
		{
			SDL_PauseAudioDevice(device, 1);
			SDL_CloseAudioDevice(device);
		}
	}

	static void SDLCALL AudioCallback(void* userdata, Uint8* output, int length)
	{
		if (userdata != nullptr)
		{
			static_cast<SdlMixer*>(userdata)->Mix(output, length);
		}
	}

	void Mix(Uint8* output_bytes, int length)
	{
		if (output_bytes == nullptr || length <= 0)
		{
			return;
		}

		SDL_memset(output_bytes, 0, static_cast<size_t>(length));
		const size_t output_samples = static_cast<size_t>(length) / sizeof(float);
		const size_t mix_samples    = (output_samples / HOST_CHANNELS) * HOST_CHANNELS;
		const size_t mix_frames     = mix_samples / HOST_CHANNELS;
		auto*        output         = reinterpret_cast<float*>(output_bytes);

		{
			std::lock_guard<std::mutex> lock(m_mutex);
			for (const auto& state: m_streams)
			{
				if (!state->registered)
				{
					continue;
				}
				state->stats.callback_calls++;
				state->stats.callback_frames += mix_frames;
				if (state->queued_samples == 0)
				{
					state->stats.underrun_frames += mix_frames;
					continue;
				}

				const size_t consume = std::min(mix_samples, state->queued_samples);
				size_t       remaining = consume;
				size_t       output_offset = 0;
				float        callback_peak = 0.0f;
				while (remaining != 0)
				{
					const size_t part = std::min(remaining, state->ring.size() - state->read_index);
					for (size_t sample = 0; sample < part; sample++)
					{
						const float value = state->ring[state->read_index + sample];
						output[output_offset + sample] += value;
						if (std::isfinite(value))
						{
							callback_peak = std::max(callback_peak, std::fabs(value));
						}
					}
					state->read_index = (state->read_index + part) % state->ring.size();
					output_offset += part;
					remaining -= part;
				}

				state->queued_samples -= consume;
				if (state->queued_samples == 0)
				{
					state->read_index = 0;
				}
				state->stats.consumed_output_bytes += consume * sizeof(float);
				state->stats.underrun_frames += (mix_samples - consume) / HOST_CHANNELS;
				if (callback_peak > AUDIBLE_PEAK)
				{
					state->stats.audible_callback_calls++;
				}
				state->stats.max_callback_peak = std::max(state->stats.max_callback_peak, callback_peak);
				state->stats.queued_output_bytes = state->queued_samples * sizeof(float);
			}
		}
		m_queue_space.notify_all();

		for (size_t sample = 0; sample < mix_samples; sample++)
		{
			output[sample] = ClampSample(output[sample]);
		}
	}

	mutable std::mutex                             m_mutex;
	std::condition_variable                       m_queue_space;
	std::mutex                                     m_lifecycle_mutex;
	std::vector<std::shared_ptr<MixerStreamState>> m_streams;
	SDL_AudioDeviceID                              m_device           = 0;
	uint32_t                                       m_device_rate      = 0;
	bool                                           m_device_paused    = true;
	bool                                           m_accepting_streams = false;
#if defined(LUMEN_AUDIO_OUT_TEST_HOOK)
	bool                                           m_test_manual_callbacks = false;
#endif
};

class SdlOutputStream final: public OutputStream
{
public:
	SdlOutputStream(std::shared_ptr<SdlMixer> mixer, std::shared_ptr<MixerStreamState> state)
	    : m_mixer(std::move(mixer)), m_state(std::move(state))
	{
		m_state->owner = this;
	}

	~SdlOutputStream() override { m_mixer->Unregister(m_state); }

	[[nodiscard]] bool Submit(const void* data, const int canonical_volumes[8], std::string* error) override
	{
		std::lock_guard<std::mutex> lock(m_state->submit_mutex);
		ClearError(error);
		if (!PrepareStreamBlock(m_state, data, canonical_volumes))
		{
			m_mixer->RecordSubmitError(m_state);
			SetError(error, "invalid audio output block");
			return false;
		}

		if (!m_mixer->EnqueueBatch(&m_state, 1, error))
		{
			m_mixer->RecordSubmitError(m_state);
			return false;
		}
		return true;
	}

	[[nodiscard]] StreamStats GetStats() const override { return m_mixer->GetStats(m_state); }
	void CancelPendingSubmissions() override { m_mixer->CancelPendingSubmissions(m_state); }

private:
	std::shared_ptr<SdlMixer>            m_mixer;
	std::shared_ptr<MixerStreamState>    m_state;
};

} // namespace

bool DownmixBlockToStereo(const StreamSpec& spec, const float* canonical_input, size_t input_samples, float* output,
                          size_t output_samples)
{
	return DownmixToStereoImpl(spec, canonical_input, input_samples, output, output_samples);
}

size_t InputBlockBytes(const StreamSpec& spec)
{
	size_t sample_count     = 0;
	size_t bytes_per_sample = 0;
	size_t input_bytes      = 0;

	if (!GetSampleCount(spec, &sample_count) || !GetBytesPerSample(spec.sample_format, &bytes_per_sample) ||
	    !CheckedMultiply(sample_count, bytes_per_sample, &input_bytes))
	{
		return 0;
	}

	return input_bytes;
}

bool ConvertBlockToFloat(const StreamSpec& spec, const void* input, const int canonical_volumes[8], float* output, size_t output_samples)
{
	size_t sample_count     = 0;
	size_t bytes_per_sample = 0;
	if (input == nullptr || canonical_volumes == nullptr || output == nullptr || !GetSampleCount(spec, &sample_count) ||
	    !GetBytesPerSample(spec.sample_format, &bytes_per_sample) || output_samples != sample_count || InputBlockBytes(spec) == 0)
	{
		return false;
	}

	const auto*  bytes    = static_cast<const uint8_t*>(input);
	const size_t channels = static_cast<size_t>(spec.channels);

	for (size_t frame = 0; frame < static_cast<size_t>(spec.frames_per_block); frame++)
	{
		for (size_t output_channel = 0; output_channel < channels; output_channel++)
		{
			const size_t   source_channel = SourceChannel(spec, output_channel);
			const size_t   source_sample  = frame * channels + source_channel;
			const size_t   output_sample  = frame * channels + output_channel;
			const uint8_t* source         = bytes + source_sample * bytes_per_sample;

			float     sample = (spec.sample_format == SampleFormat::Signed16 ? ReadSigned16(source) : ReadFloat32(source));
			const int volume = std::clamp(canonical_volumes[output_channel], 0, MAX_VOLUME);
			sample *= static_cast<float>(volume) / static_cast<float>(MAX_VOLUME);
			output[output_sample] = ClampSample(sample);
		}
	}

	return true;
}

uint64_t AdvancePacingDeadline(uint64_t previous_deadline_us, uint64_t now_us, uint64_t period_us)
{
	if (period_us == 0 || previous_deadline_us == 0 || previous_deadline_us > std::numeric_limits<uint64_t>::max() - period_us)
	{
		return now_us;
	}

	const uint64_t next_deadline = previous_deadline_us + period_us;
	return (next_deadline < now_us ? now_us : next_deadline);
}

uint64_t AdvancePacingDeadline(PacingState* state, uint64_t now_us, uint32_t frames, uint32_t sample_rate)
{
	if (state == nullptr)
	{
		return now_us;
	}

	const auto resync = [state, now_us, sample_rate]() {
		state->deadline_us    = now_us;
		state->remainder      = 0;
		state->sample_rate    = sample_rate;
		state->refill_pending = true;
		return now_us;
	};

	if (frames == 0 || sample_rate == 0)
	{
		state->deadline_us    = now_us;
		state->remainder      = 0;
		state->sample_rate    = 0;
		state->refill_pending = false;
		return now_us;
	}

	if (state->sample_rate == 0 || state->sample_rate != sample_rate || state->remainder >= sample_rate)
	{
		return resync();
	}

	constexpr uint64_t MICROSECONDS_PER_SECOND = 1000000;
	if (static_cast<uint64_t>(frames) > std::numeric_limits<uint64_t>::max() / MICROSECONDS_PER_SECOND)
	{
		return resync();
	}

	const uint64_t numerator          = static_cast<uint64_t>(frames) * MICROSECONDS_PER_SECOND;
	const uint64_t whole_microseconds = numerator / sample_rate;
	const uint64_t block_remainder    = numerator % sample_rate;
	const uint64_t combined_remainder = state->remainder + block_remainder;
	const uint64_t remainder_carry    = combined_remainder / sample_rate;
	const uint64_t next_remainder     = combined_remainder % sample_rate;
	if (whole_microseconds > std::numeric_limits<uint64_t>::max() - remainder_carry)
	{
		return resync();
	}

	const uint64_t increment = whole_microseconds + remainder_carry;
	if (state->deadline_us > std::numeric_limits<uint64_t>::max() - increment)
	{
		return resync();
	}

	const uint64_t next_deadline = state->deadline_us + increment;
	if (next_deadline < now_us)
	{
		return resync();
	}

	state->deadline_us = next_deadline;
	state->remainder   = next_remainder;
	return next_deadline;
}

size_t BufferedQueueTargetFrames(uint32_t frames_per_block, uint32_t sample_rate)
{
	if (frames_per_block == 0 || sample_rate == 0)
	{
		return 0;
	}

	const uint64_t duration_frames =
	    (static_cast<uint64_t>(sample_rate) * BUFFERED_QUEUE_TARGET_US + 999999u) / 1000000u;
	const size_t target_frames =
	    std::max(static_cast<size_t>(frames_per_block),
	             static_cast<size_t>(std::min<uint64_t>(duration_frames, MAX_BUFFERED_TARGET_FRAMES)));
	const size_t blocks = (target_frames - 1) / frames_per_block + 1;
	return blocks * frames_per_block;
}

bool ShouldRefillBufferedBatch(const size_t* queued_frames, const size_t* target_frames, size_t submitted_count,
                               size_t total_count)
{
	if (queued_frames == nullptr || target_frames == nullptr || submitted_count == 0 || submitted_count != total_count)
	{
		return false;
	}
	for (size_t i = 0; i < total_count; i++)
	{
		if (target_frames[i] == 0 || queued_frames[i] >= target_frames[i])
		{
			return false;
		}
	}
	return true;
}

uint64_t ApplyBufferedPacingDeadline(PacingState* state, uint64_t now_us, uint32_t frames, uint32_t sample_rate,
                                     size_t queued_frames, bool allow_refill)
{
	if (state == nullptr)
	{
		return now_us;
	}
	const size_t target = BufferedQueueTargetFrames(frames, sample_rate);
	if (!allow_refill || target == 0 || queued_frames >= target)
	{
		state->refill_pending = false;
	}
	// A low queue alone is not a producer stall: a host may consume callbacks
	// in bursts. Only an initialization or late deadline opens this refill phase.
	if (state->refill_pending)
	{
		state->deadline_us = now_us;
		state->remainder   = 0;
		state->sample_rate = sample_rate;
		return now_us;
	}
	return state->deadline_us;
}

uint64_t AdvanceBufferedPacingDeadline(PacingState* state, uint64_t now_us, uint32_t frames, uint32_t sample_rate,
                                       size_t queued_frames, bool allow_refill)
{
	static_cast<void>(AdvancePacingDeadline(state, now_us, frames, sample_rate));
	return ApplyBufferedPacingDeadline(state, now_us, frames, sample_rate, queued_frames, allow_refill);
}

Internal::RingEnqueuePlan Internal::PlanRingEnqueue(size_t capacity_samples, size_t read_index, size_t queued_samples,
                                                    size_t incoming_samples, size_t sample_alignment)
{
	RingEnqueuePlan plan {};
	if (capacity_samples == 0 || sample_alignment == 0 || incoming_samples == 0 || incoming_samples > capacity_samples ||
	    read_index >= capacity_samples || queued_samples > capacity_samples || capacity_samples % sample_alignment != 0 ||
	    read_index % sample_alignment != 0 || queued_samples % sample_alignment != 0 ||
	    incoming_samples % sample_alignment != 0)
	{
		return plan;
	}

	const size_t free_samples = capacity_samples - queued_samples;
	plan.dropped_samples      = (incoming_samples > free_samples ? incoming_samples - free_samples : 0);
	const auto advance = [capacity_samples](size_t index, size_t offset) {
		const size_t until_wrap = capacity_samples - index;
		return offset >= until_wrap ? offset - until_wrap : index + offset;
	};
	plan.read_index         = advance(read_index, plan.dropped_samples);
	plan.retained_samples   = queued_samples - plan.dropped_samples;
	plan.write_index        = advance(plan.read_index, plan.retained_samples);
	plan.first_copy_samples = std::min(incoming_samples, capacity_samples - plan.write_index);
	plan.queued_samples     = plan.retained_samples + incoming_samples;
	plan.valid              = true;
	return plan;
}

size_t Internal::StereoRingCapacitySamples(size_t frames_per_block, size_t minimum_queued_frames)
{
	if (frames_per_block == 0)
	{
		return 0;
	}

	size_t capacity_frames  = 0;
	size_t capacity_samples = 0;
	if (!CheckedMultiply(frames_per_block, MIN_QUEUED_BLOCKS, &capacity_frames))
	{
		return 0;
	}
	capacity_frames = std::max(capacity_frames, minimum_queued_frames);
	if (!CheckedMultiply(capacity_frames, HOST_CHANNELS, &capacity_samples))
	{
		return 0;
	}
	return capacity_samples;
}

class OutputBackend::Impl
{
public:
	Impl(): mixer(std::make_shared<SdlMixer>()) {}

	mutable std::mutex       mutex;
	std::shared_ptr<SdlMixer> mixer;
	bool                      initialized = false;
};

OutputBackend::OutputBackend(): m_impl(std::make_unique<Impl>()) {}

OutputBackend::~OutputBackend()
{
	if (m_impl == nullptr)
	{
		return;
	}

	std::shared_ptr<SdlMixer> mixer;
	bool                      quit_audio = false;
	{
		std::lock_guard<std::mutex> lock(m_impl->mutex);
		if (m_impl->initialized)
		{
			m_impl->initialized = false;
			mixer               = m_impl->mixer;
			quit_audio          = true;
		}
	}

	if (quit_audio)
	{
		mixer->Shutdown();
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	}
}

bool OutputBackend::Initialize(std::string* error)
{
	ClearError(error);
	std::lock_guard<std::mutex> lock(m_impl->mutex);
	if (m_impl->initialized)
	{
		return true;
	}

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
	{
		SetSdlError(error, "SDL audio initialization failed");
		return false;
	}

	m_impl->mixer->Activate();
	m_impl->initialized = true;
	return true;
}

bool OutputBackend::IsInitialized() const
{
	std::lock_guard<std::mutex> lock(m_impl->mutex);
	return m_impl->initialized;
}

std::string OutputBackend::DriverName() const
{
	std::lock_guard<std::mutex> lock(m_impl->mutex);
	if (!m_impl->initialized)
	{
		return {};
	}

	const char* driver = SDL_GetCurrentAudioDriver();
	return (driver != nullptr ? std::string(driver) : std::string {});
}

std::unique_ptr<OutputStream> OutputBackend::Open(const StreamSpec& spec, std::string* error)
{
	ClearError(error);
	std::shared_ptr<SdlMixer> mixer;
	{
		std::lock_guard<std::mutex> lock(m_impl->mutex);
		if (!m_impl->initialized)
		{
			SetError(error, "audio output backend is not initialized");
			return {};
		}
		mixer = m_impl->mixer;
	}

	const size_t input_bytes = InputBlockBytes(spec);
	size_t       canonical_samples = 0;
	size_t       stereo_samples    = 0;
	size_t       stereo_bytes      = 0;
	size_t       ring_samples      = 0;
	const size_t target_frames = BufferedQueueTargetFrames(spec.frames_per_block, spec.sample_rate);
	size_t       buffered_capacity_floor = 0;
	if (input_bytes == 0 || !GetSampleCount(spec, &canonical_samples) ||
	    !CheckedMultiply(static_cast<size_t>(spec.frames_per_block), HOST_CHANNELS, &stereo_samples) ||
	    !CheckedMultiply(stereo_samples, sizeof(float), &stereo_bytes) ||
	    target_frames == 0 || target_frames > std::numeric_limits<size_t>::max() - spec.frames_per_block ||
	    (buffered_capacity_floor = std::max(MIN_QUEUED_FRAMES, target_frames + spec.frames_per_block)) == 0 ||
	    (ring_samples = Internal::StereoRingCapacitySamples(spec.frames_per_block, buffered_capacity_floor)) == 0 ||
	    stereo_bytes > std::numeric_limits<Uint32>::max())
	{
		SetError(error, "invalid audio output stream specification");
		return {};
	}

	auto state = std::make_shared<MixerStreamState>(spec, input_bytes, canonical_samples, stereo_samples, ring_samples);
	std::unique_ptr<OutputStream> stream(new SdlOutputStream(mixer, state));
	if (!mixer->Register(state, error))
	{
		return {};
	}

	return stream;
}

bool OutputBackend::SubmitBatch(const OutputBatchItem* items, size_t count, std::string* error)
{
	return SubmitBatchWithPolicy(items, count, false, error);
}

bool OutputBackend::SubmitBlockingBatch(const OutputBatchItem* items, size_t count, std::string* error)
{
	return SubmitBatchWithPolicy(items, count, true, error);
}

#if defined(LUMEN_AUDIO_OUT_TEST_HOOK)
bool OutputBackend::TestSetManualCallbacks(bool manual)
{
	return m_impl->mixer->TestSetManualCallbacks(manual);
}

bool OutputBackend::TestConsumeFrames(size_t frames, std::vector<float>* output)
{
	return m_impl->mixer->TestConsumeFrames(frames, output);
}

bool OutputBackend::TestShutdownMixer()
{
	return m_impl->mixer->TestShutdownMixer();
}
#endif

bool OutputBackend::SubmitBatchWithPolicy(const OutputBatchItem* items, size_t count, bool wait_for_capacity, std::string* error)
{
	ClearError(error);
	if (items == nullptr || count == 0 || count > MAX_STREAMS)
	{
		SetError(error, "invalid audio output batch");
		return false;
	}

	std::shared_ptr<SdlMixer> mixer;
	{
		std::lock_guard<std::mutex> lock(m_impl->mutex);
		if (!m_impl->initialized)
		{
			SetError(error, "audio output backend is not initialized");
			return false;
		}
		mixer = m_impl->mixer;
	}

	std::vector<std::shared_ptr<MixerStreamState>> states;
	states.reserve(count);
	if (!mixer->ResolveBatch(items, count, &states, error))
	{
		return false;
	}

	std::vector<MixerStreamState*> lock_order;
	lock_order.reserve(count);
	for (const auto& state: states)
	{
		lock_order.push_back(state.get());
	}
	std::sort(lock_order.begin(), lock_order.end(), std::less<MixerStreamState*> {});

	std::vector<std::unique_lock<std::mutex>> submit_locks;
	submit_locks.reserve(count);
	for (auto* state: lock_order)
	{
		submit_locks.emplace_back(state->submit_mutex);
	}

	for (size_t item_index = 0; item_index < count; item_index++)
	{
		if (!PrepareStreamBlock(states[item_index], items[item_index].data, items[item_index].canonical_volumes))
		{
			mixer->RecordSubmitError(states[item_index]);
			SetError(error, "invalid audio output batch item data");
			return false;
		}
	}

	if ((wait_for_capacity && !mixer->WaitForQueueSpace(states.data(), states.size(), error)) ||
	    !mixer->EnqueueBatch(states.data(), states.size(), error, wait_for_capacity))
	{
		for (const auto& state: states)
		{
			mixer->RecordSubmitError(state);
		}
		return false;
	}
	return true;
}

} // namespace Lumen::Libs::Audio::Host

#endif // LUMEN_EMU_ENABLED
