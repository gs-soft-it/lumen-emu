#include "Emulator/AvPlayerMac.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace Mac = Lumen::Libs::Audio::AvPlayer::Mac;
using Clock   = std::chrono::steady_clock;

constexpr auto     WAIT_TIMEOUT         = std::chrono::seconds(5);
constexpr auto     POLL_INTERVAL        = std::chrono::milliseconds(1);
constexpr uint32_t VIDEO_WIDTH          = 16;
constexpr uint32_t VIDEO_HEIGHT         = 16;
constexpr uint32_t VIDEO_PITCH          = 16;
constexpr size_t   VIDEO_BUFFER_SIZE    = 384;
constexpr size_t   VIDEO_FRAME_COUNT    = 3;
constexpr uint16_t AUDIO_CHANNELS       = 2;
constexpr uint32_t AUDIO_SAMPLE_RATE    = 48000;
constexpr size_t   AUDIO_SAMPLE_FRAMES  = 19200;
constexpr size_t   AUDIO_FRAME_BYTES    = 4;
constexpr size_t   AUDIO_MAX_SLICE_BYTES = Mac::AUDIO_FRAME_MAX_SAMPLE_FRAMES * AUDIO_FRAME_BYTES;
constexpr size_t   AUDIO_TOTAL_BYTES    = AUDIO_SAMPLE_FRAMES * AUDIO_FRAME_BYTES;
#if defined(LUMEN_AV_SYNC_TELEMETRY)
constexpr size_t   AUDIO_SLICE_COUNT    =
    (AUDIO_SAMPLE_FRAMES + Mac::AUDIO_FRAME_MAX_SAMPLE_FRAMES - 1) / Mac::AUDIO_FRAME_MAX_SAMPLE_FRAMES;
#endif
constexpr uint64_t AUDIO_FNV1A64        = UINT64_C(0x4c39dbe379c9d186);
constexpr size_t   AV_MOV_BYTES         = 79596;
constexpr size_t   VIDEO_ONLY_MOV_BYTES = 2303;
constexpr size_t   GUARD_SIZE           = 16;

bool Error(const char* message)
{
	std::fprintf(stderr, "%s\n", message);
	return false;
}

int Base64Value(char value)
{
	if (value >= 'A' && value <= 'Z') return value - 'A';
	if (value >= 'a' && value <= 'z') return value - 'a' + 26;
	if (value >= '0' && value <= '9') return value - '0' + 52;
	if (value == '+') return 62;
	if (value == '/') return 63;
	return -1;
}

bool DecodeBase64File(const char* filename, size_t expected_size, std::vector<uint8_t>* decoded)
{
	std::ifstream input(filename, std::ios::binary);
	if (!input)
	{
		std::fprintf(stderr, "cannot open Base64 fixture: %s\n", filename);
		return false;
	}
	std::string encoded((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	encoded.erase(std::remove_if(encoded.begin(), encoded.end(), [](unsigned char value) {
	                  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
	              }),
	              encoded.end());
	if (encoded.empty() || (encoded.size() % 4) != 0)
	{
		return Error("invalid Base64 fixture length");
	}

	decoded->clear();
	decoded->reserve((encoded.size() / 4) * 3);
	for (size_t offset = 0; offset < encoded.size(); offset += 4)
	{
		const bool last     = offset + 4 == encoded.size();
		const bool pad_two  = encoded[offset + 2] == '=';
		const bool pad_three = encoded[offset + 3] == '=';
		const int  a        = Base64Value(encoded[offset]);
		const int  b        = Base64Value(encoded[offset + 1]);
		const int  c        = pad_two ? 0 : Base64Value(encoded[offset + 2]);
		const int  d        = pad_three ? 0 : Base64Value(encoded[offset + 3]);
		if (a < 0 || b < 0 || c < 0 || d < 0 || (pad_two && !pad_three) || (!last && (pad_two || pad_three)) ||
		    (pad_two && (b & 0x0f) != 0) || (!pad_two && pad_three && (c & 0x03) != 0))
		{
			return Error("invalid or non-canonical Base64 fixture");
		}
		decoded->push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
		if (!pad_two) decoded->push_back(static_cast<uint8_t>((b << 4) | (c >> 2)));
		if (!pad_three) decoded->push_back(static_cast<uint8_t>((c << 6) | d));
	}
	if (decoded->size() != expected_size)
	{
		std::fprintf(stderr, "decoded fixture size mismatch: actual=%zu expected=%zu\n", decoded->size(), expected_size);
		return false;
	}
	return true;
}

bool WriteFile(const std::string& filename, const std::vector<uint8_t>& bytes)
{
	const int descriptor = open(filename.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (descriptor < 0)
	{
		std::fprintf(stderr, "cannot create temporary movie: %s\n", std::strerror(errno));
		return false;
	}

	size_t offset = 0;
	while (offset < bytes.size())
	{
		const ssize_t written = write(descriptor, bytes.data() + offset, bytes.size() - offset);
		if (written < 0 && errno == EINTR) continue;
		if (written <= 0)
		{
			std::fprintf(stderr, "cannot write temporary movie: %s\n", std::strerror(errno));
			close(descriptor);
			unlink(filename.c_str());
			return false;
		}
		offset += static_cast<size_t>(written);
	}
	if (close(descriptor) != 0)
	{
		std::fprintf(stderr, "cannot close temporary movie: %s\n", std::strerror(errno));
		unlink(filename.c_str());
		return false;
	}
	return true;
}

class TemporaryMovies
{
public:
	~TemporaryMovies()
	{
		if (!m_av.empty()) unlink(m_av.c_str());
		if (!m_video_only.empty()) unlink(m_video_only.c_str());
		if (!m_directory.empty()) rmdir(m_directory.c_str());
	}

	bool Create(const char* av_base64, const char* video_only_base64)
	{
		char directory[] = "/tmp/lumen-avplayer-XXXXXX";
		char* created = mkdtemp(directory);
		if (created == nullptr)
		{
			std::fprintf(stderr, "cannot create temporary directory: %s\n", std::strerror(errno));
			return false;
		}
		m_directory  = created;
		m_av         = m_directory + "/synthetic-av.mov";
		m_video_only = m_directory + "/synthetic-video-only.mov";

		std::vector<uint8_t> bytes;
		return DecodeBase64File(av_base64, AV_MOV_BYTES, &bytes) && WriteFile(m_av, bytes) &&
		       DecodeBase64File(video_only_base64, VIDEO_ONLY_MOV_BYTES, &bytes) && WriteFile(m_video_only, bytes);
	}

	const char* Av() const { return m_av.c_str(); }
	const char* VideoOnly() const { return m_video_only.c_str(); }

private:
	std::string m_directory;
	std::string m_av;
	std::string m_video_only;
};

uint64_t Fnv1a64(const uint8_t* bytes, size_t size)
{
	uint64_t hash = UINT64_C(14695981039346656037);
	for (size_t index = 0; index < size; index++)
	{
		hash ^= bytes[index];
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

bool IsFilled(const std::vector<uint8_t>& bytes, uint8_t value)
{
	return std::all_of(bytes.begin(), bytes.end(), [value](uint8_t current) { return current == value; });
}

bool CheckVideoFormat(const Mac::VideoFormat& format)
{
	if (format.width != VIDEO_WIDTH || format.height != VIDEO_HEIGHT || format.pitch != VIDEO_PITCH ||
	    format.buffer_size != VIDEO_BUFFER_SIZE || !std::isfinite(format.frame_rate) ||
	    std::fabs(format.frame_rate - 10.0f) > 0.01f || format.full_range)
	{
		std::fprintf(stderr, "unexpected video format: %ux%u pitch=%u bytes=%zu fps=%.3f full=%d\n", format.width,
		             format.height, format.pitch, format.buffer_size, static_cast<double>(format.frame_rate),
		             format.full_range ? 1 : 0);
		return false;
	}
	return true;
}

bool CheckAudioFormat(const Mac::AudioFormat& format, bool expected)
{
	if (expected && format.channel_count == AUDIO_CHANNELS && format.sample_rate == AUDIO_SAMPLE_RATE) return true;
	if (!expected && format.channel_count == 0 && format.sample_rate == 0) return true;
	std::fprintf(stderr, "unexpected audio format: channels=%u rate=%u expected=%d\n",
	             static_cast<unsigned>(format.channel_count), format.sample_rate, expected ? 1 : 0);
	return false;
}

bool CheckVideoPayload(const std::vector<uint8_t>& frame, size_t frame_index)
{
	static constexpr std::array<uint8_t, VIDEO_FRAME_COUNT> LUMA = {32, 112, 208};
	if (frame.size() != VIDEO_BUFFER_SIZE || frame_index >= LUMA.size()) return false;
	for (size_t index = 0; index < VIDEO_WIDTH * VIDEO_HEIGHT; index++)
	{
		if (frame[index] != LUMA[frame_index]) return false;
	}
	for (size_t index = VIDEO_WIDTH * VIDEO_HEIGHT; index < frame.size(); index++)
	{
		if (frame[index] != 128) return false;
	}
	return true;
}

bool CheckPcm(const std::vector<uint8_t>& pcm, bool complete)
{
	if ((pcm.size() % AUDIO_FRAME_BYTES) != 0 || (complete && pcm.size() != AUDIO_TOTAL_BYTES) ||
	    pcm.size() > AUDIO_TOTAL_BYTES)
	{
		return Error("invalid concatenated PCM size");
	}
	for (size_t sample = 0; sample < pcm.size() / AUDIO_FRAME_BYTES; sample++)
	{
		const uint16_t left = static_cast<uint16_t>((sample * 257u + 0x1234u) & 0xffffu);
		const uint16_t right = static_cast<uint16_t>((sample * 521u + 0x4321u) & 0xffffu);
		const size_t   offset = sample * AUDIO_FRAME_BYTES;
		if (pcm[offset] != static_cast<uint8_t>(left) || pcm[offset + 1] != static_cast<uint8_t>(left >> 8u) ||
		    pcm[offset + 2] != static_cast<uint8_t>(right) || pcm[offset + 3] != static_cast<uint8_t>(right >> 8u))
		{
			std::fprintf(stderr, "PCM oracle mismatch at sample %zu\n", sample);
			return false;
		}
	}
	if (complete && Fnv1a64(pcm.data(), pcm.size()) != AUDIO_FNV1A64)
	{
		return Error("PCM FNV-1a oracle mismatch");
	}
	return true;
}

bool TestAudioTimestampResolver()
{
	static constexpr std::array<uint64_t, 3> FRAMES   = {0, 1024, 2048};
	static constexpr std::array<int64_t, 3>  EXPECTED = {0, 21, 43};
	for (size_t index = 0; index < FRAMES.size(); index++)
	{
		const auto result = Mac::Internal::ResolveAudioTimestampForTest(
		    false, 0, true, 0, 0, FRAMES[index], AUDIO_SAMPLE_RATE, false, 0);
		if (result.time_stamp != EXPECTED[index] || result.update_anchor)
		{
			std::fprintf(stderr, "48 kHz fallback mismatch: frames=%" PRIu64 " actual=%" PRId64
			                     " expected=%" PRId64 "\n",
			             FRAMES[index], result.time_stamp, EXPECTED[index]);
			return false;
		}
	}

	const auto half_away = Mac::Internal::ResolveAudioTimestampForTest(
	    false, 0, true, 0, 0, 24, AUDIO_SAMPLE_RATE, false, 0);
	const auto positive_anchor = Mac::Internal::ResolveAudioTimestampForTest(
	    false, 0, true, 5000, 100, 1124, AUDIO_SAMPLE_RATE, false, 0);
	const auto negative_anchor = Mac::Internal::ResolveAudioTimestampForTest(
	    false, 0, true, -40, 500, 2548, AUDIO_SAMPLE_RATE, false, 0);
	const auto saturated = Mac::Internal::ResolveAudioTimestampForTest(
	    false, 0, true, std::numeric_limits<int64_t>::max() - 10, 0, 1024, AUDIO_SAMPLE_RATE, false, 0);
	const auto retrograde = Mac::Internal::ResolveAudioTimestampForTest(
	    true, 25, true, 100, 2048, 4096, AUDIO_SAMPLE_RATE, true, 50);
	if (half_away.time_stamp != 1 || positive_anchor.time_stamp != 5021 || negative_anchor.time_stamp != 3 ||
	    saturated.time_stamp != std::numeric_limits<int64_t>::max() || retrograde.time_stamp != 50 ||
	    !retrograde.update_anchor || retrograde.anchor_time_stamp != 50 ||
	    retrograde.anchor_sample_frame != 4096)
	{
		return Error("anchored, half-away, saturating or monotonic audio PTS fallback failed");
	}
	return true;
}

bool TestVideoMasterPositionResolver()
{
	struct TestCase
	{
		const char* name;
		bool        has_audio;
		bool        audio_eof;
		uint64_t    wall_clock_position_ms;
		bool        delivered_audio_end_valid;
		uint64_t    delivered_audio_end_ms;
		bool        pending_audio_time_stamp_valid;
		uint64_t    pending_audio_time_stamp_ms;
		uint64_t    expected_position_ms;
	};

	static constexpr std::array<TestCase, 11> CASES = {{
	    {"video-only", false, false, 500, false, 0, false, 0, 500},
	    {"audio-eof", true, true, 500, true, 200, false, 0, 500},
	    {"co-origin-no-pcm", true, false, 240, false, 0, true, 0, 0},
	    {"delayed-audio-before-boundary", true, false, 240, false, 0, true, 1000, 240},
	    {"delayed-audio-at-boundary", true, false, 1200, false, 0, true, 1000, 1000},
	    {"contiguous-pcm-stalled-guest", true, false, 500, true, 200, true, 200, 200},
	    {"known-gap-before-boundary", true, false, 500, true, 200, true, 800, 500},
	    {"known-gap-at-boundary", true, false, 900, true, 200, true, 800, 800},
	    {"unknown-initial-gap-fail-closed", true, false, 500, false, 0, false, 0, 0},
	    {"unknown-later-gap-fail-closed", true, false, 500, true, 200, false, 0, 200},
	    {"overlapping-pending-pcm", true, false, 500, true, 200, true, 100, 200},
	}};

	for (const auto& test : CASES)
	{
		const uint64_t actual = Mac::Internal::ResolveVideoMasterPositionForTest(
		    test.has_audio, test.audio_eof, test.wall_clock_position_ms,
		    test.delivered_audio_end_valid, test.delivered_audio_end_ms,
		    test.pending_audio_time_stamp_valid, test.pending_audio_time_stamp_ms);
		if (actual != test.expected_position_ms)
		{
			std::fprintf(stderr, "video master mismatch: case=%s actual=%" PRIu64 " expected=%" PRIu64 "\n",
			             test.name, actual, test.expected_position_ms);
			return false;
		}
	}
	return true;
}

enum class AudioRetryProbe
{
	None,
	FirstSlice,
	NonzeroOffset,
	FinalPartial,
};

bool SameAudioReadState(const Mac::Internal::AudioReadStateForTest& left,
                        const Mac::Internal::AudioReadStateForTest& right)
{
	return left.pending_offset_frames == right.pending_offset_frames &&
	       left.consumed_sample_frames == right.consumed_sample_frames &&
	       left.time_stamp_anchor_valid == right.time_stamp_anchor_valid &&
	       left.time_stamp_anchor == right.time_stamp_anchor &&
	       left.time_stamp_anchor_frames == right.time_stamp_anchor_frames &&
	       left.playback_clock_started == right.playback_clock_started &&
	       left.delivered_audio_end_valid == right.delivered_audio_end_valid &&
	       left.delivered_audio_end_ms == right.delivered_audio_end_ms;
}

bool WaitVideoFrame(Mac::Decoder* decoder, std::vector<uint8_t>* destination, Mac::VideoFrame* frame)
{
	const auto deadline = Clock::now() + WAIT_TIMEOUT;
	while (Clock::now() < deadline)
	{
		if (Mac::ReadFrame(decoder, destination->data(), destination->size(), frame)) return true;
		if (!Mac::IsActive(decoder)) return false;
		std::this_thread::sleep_for(POLL_INTERVAL);
	}
	return Error("timed out waiting for video frame");
}

bool ReadAudioFrame(Mac::Decoder* decoder, std::vector<uint8_t>* pcm, uint64_t* previous_pts, bool* have_pts,
	                AudioRetryProbe retry_probe)
{
	const auto deadline = Clock::now() + WAIT_TIMEOUT;
	size_t     required = 0;
	while (Clock::now() < deadline && Mac::IsActive(decoder))
	{
		required = Mac::GetPendingAudioFrameSize(decoder);
		if (required != 0) break;
		std::this_thread::sleep_for(POLL_INTERVAL);
	}
	if (required == 0) return Error("timed out waiting for audio capacity");
	if (required > AUDIO_MAX_SLICE_BYTES || (required % AUDIO_FRAME_BYTES) != 0)
	{
		std::fprintf(stderr, "invalid bounded audio slice capacity: %zu\n", required);
		return false;
	}

	std::vector<uint8_t> guarded(required + GUARD_SIZE * 2, 0xa5);
	auto*                destination = guarded.data() + GUARD_SIZE;
	if (retry_probe != AudioRetryProbe::None)
	{
		Mac::Internal::AudioReadStateForTest before {};
		Mac::Internal::AudioReadStateForTest after {};
		if (!Mac::Internal::GetAudioReadStateForTest(decoder, &before))
		{
			return Error("unable to snapshot audio retry state");
		}
		const bool expected_position =
		    (retry_probe == AudioRetryProbe::FirstSlice && before.pending_offset_frames == 0 &&
		     before.consumed_sample_frames == 0 && !before.time_stamp_anchor_valid &&
		     !before.playback_clock_started && !before.delivered_audio_end_valid) ||
		    (retry_probe == AudioRetryProbe::NonzeroOffset && before.pending_offset_frames != 0 &&
		     before.consumed_sample_frames != 0 && before.time_stamp_anchor_valid &&
		     before.playback_clock_started && before.delivered_audio_end_valid) ||
		    (retry_probe == AudioRetryProbe::FinalPartial && before.pending_offset_frames != 0 &&
		     before.consumed_sample_frames != 0 && required < AUDIO_MAX_SLICE_BYTES &&
		     before.time_stamp_anchor_valid && before.playback_clock_started && before.delivered_audio_end_valid);
		if (!expected_position)
		{
			return Error("audio undersized retry reached an unexpected slice position");
		}
		Mac::AudioFrame probe {UINT64_MAX, SIZE_MAX};
		if (Mac::ReadAudioFrame(decoder, destination, required - 1, &probe) || probe.time_stamp != UINT64_MAX ||
		    probe.data_size != SIZE_MAX || !IsFilled(guarded, 0xa5) ||
		    !Mac::Internal::GetAudioReadStateForTest(decoder, &after) || !SameAudioReadState(before, after) ||
		    Mac::GetPendingAudioFrameSize(decoder) != required)
		{
			return Error("small audio buffer consumed data or changed clock, anchor, position or outputs");
		}
	}

	Mac::AudioFrame frame {UINT64_MAX, SIZE_MAX};
	while (Clock::now() < deadline)
	{
		if (Mac::ReadAudioFrame(decoder, destination, required, &frame)) break;
		if (frame.time_stamp != UINT64_MAX || frame.data_size != SIZE_MAX || !IsFilled(guarded, 0xa5) ||
		    !Mac::IsActive(decoder))
		{
			return Error("failed audio poll changed state or output");
		}
		std::this_thread::sleep_for(POLL_INTERVAL);
	}
	if (frame.data_size == SIZE_MAX) return Error("timed out waiting for paced audio frame");
	if (frame.data_size != required || (frame.data_size % AUDIO_FRAME_BYTES) != 0 ||
	    !std::all_of(guarded.begin(), guarded.begin() + static_cast<std::ptrdiff_t>(GUARD_SIZE),
	                 [](uint8_t value) { return value == 0xa5; }) ||
	    !std::all_of(guarded.end() - static_cast<std::ptrdiff_t>(GUARD_SIZE), guarded.end(),
	                 [](uint8_t value) { return value == 0xa5; }))
	{
		return Error("audio copy size or canary mismatch");
	}
	const size_t frame_offset = pcm->size() / AUDIO_FRAME_BYTES;
	const uint64_t expected_pts =
	    (static_cast<uint64_t>(frame_offset) * 1000u + AUDIO_SAMPLE_RATE / 2u) / AUDIO_SAMPLE_RATE;
	if (frame.time_stamp != expected_pts)
	{
		std::fprintf(stderr, "audio slice PTS mismatch: offset=%zu actual=%" PRIu64 " expected=%" PRIu64 "\n",
		             frame_offset, frame.time_stamp, expected_pts);
		return false;
	}
	if (*have_pts && frame.time_stamp < *previous_pts) return Error("audio PTS is not monotonic");
	*previous_pts = frame.time_stamp;
	*have_pts     = true;
	pcm->insert(pcm->end(), destination, destination + frame.data_size);
	return true;
}

bool DrainAudioThrough(Mac::Decoder* decoder, std::vector<uint8_t>* pcm, uint64_t* previous_pts, bool* have_pts,
	                   uint64_t target_end_ms, AudioRetryProbe first_retry_probe = AudioRetryProbe::None)
{
	bool retry_probe_used = first_retry_probe == AudioRetryProbe::None;
	while (pcm->size() < AUDIO_TOTAL_BYTES)
	{
		Mac::Internal::AudioReadStateForTest state {};
		if (!Mac::Internal::GetAudioReadStateForTest(decoder, &state))
		{
			return Error("unable to snapshot delivered audio media position");
		}
		if (state.delivered_audio_end_valid && state.delivered_audio_end_ms >= target_end_ms)
		{
			return true;
		}

		const AudioRetryProbe retry_probe = retry_probe_used ? AudioRetryProbe::None : first_retry_probe;
		if (!ReadAudioFrame(decoder, pcm, previous_pts, have_pts, retry_probe))
		{
			return false;
		}
		retry_probe_used = true;
	}
	return Error("audio ended before reaching the requested media position");
}

bool ReadAndCheckVideo(Mac::Decoder* decoder, const Mac::VideoFormat& format, size_t expected_index,
	                   Mac::VideoFrame* result = nullptr)
{
	static constexpr std::array<uint64_t, VIDEO_FRAME_COUNT> PTS = {0, 100, 200};
	std::vector<uint8_t> frame_bytes(format.buffer_size, 0);
	Mac::VideoFrame      frame {};
	if (!WaitVideoFrame(decoder, &frame_bytes, &frame)) return false;
	if (expected_index >= PTS.size() || frame.time_stamp != PTS[expected_index] || !CheckVideoPayload(frame_bytes, expected_index))
	{
		std::fprintf(stderr, "video oracle mismatch: index=%zu pts=%" PRIu64 "\n", expected_index, frame.time_stamp);
		return false;
	}
	if (result != nullptr) *result = frame;
	return true;
}

bool MarkVideoEof(Mac::Decoder* decoder, const Mac::VideoFormat& format)
{
	std::vector<uint8_t> canary(format.buffer_size, 0x5a);
	Mac::VideoFrame      frame {UINT64_MAX, true};
	return !Mac::ReadFrame(decoder, canary.data(), canary.size(), &frame) && IsFilled(canary, 0x5a) &&
	       frame.time_stamp == UINT64_MAX && frame.full_range;
}

bool CheckStopped(Mac::Decoder* decoder, const Mac::VideoFormat& format)
{
	std::vector<uint8_t> video(format.buffer_size, 0x33);
	std::array<uint8_t, 8> audio {};
	audio.fill(0x44);
	Mac::VideoFrame stopped_video {UINT64_MAX, true};
	Mac::AudioFrame stopped_audio {UINT64_MAX, SIZE_MAX};
	return !Mac::IsActive(decoder) && Mac::GetPendingAudioFrameSize(decoder) == 0 &&
	       !Mac::ReadFrame(decoder, video.data(), video.size(), &stopped_video) && IsFilled(video, 0x33) &&
	       stopped_video.time_stamp == UINT64_MAX && stopped_video.full_range &&
	       !Mac::ReadAudioFrame(decoder, audio.data(), audio.size(), &stopped_audio) &&
	       std::all_of(audio.begin(), audio.end(), [](uint8_t value) { return value == 0x44; }) &&
	       stopped_audio.time_stamp == UINT64_MAX && stopped_audio.data_size == SIZE_MAX;
}

bool TestVideoCatchUp(Mac::Decoder* decoder, const char* filename)
{
	Mac::SetLooping(decoder, false);
	Mac::VideoFormat video_format {};
	Mac::AudioFormat audio_format {};
	if (!Mac::Open(decoder, filename, &video_format, &audio_format) || !CheckVideoFormat(video_format) ||
	    !CheckAudioFormat(audio_format, true))
	{
		return Error("A/V catch-up open failed");
	}

	Mac::Internal::AudioReadStateForTest initial_audio_state {};
	if (!Mac::Internal::GetAudioReadStateForTest(decoder, &initial_audio_state) ||
	    initial_audio_state.delivered_audio_end_valid)
	{
		return Error("A/V catch-up opened with an advanced audio media position");
	}
	if (!ReadAndCheckVideo(decoder, video_format, 0))
	{
		return false;
	}
	std::vector<uint8_t> not_due_bytes(video_format.buffer_size, 0x6d);
	Mac::VideoFrame      not_due_frame {UINT64_MAX, true};
	if (Mac::ReadFrame(decoder, not_due_bytes.data(), not_due_bytes.size(), &not_due_frame) || !IsFilled(not_due_bytes, 0x6d) ||
	    not_due_frame.time_stamp != UINT64_MAX || !not_due_frame.full_range)
	{
		return Error("catch-up probe did not preserve the not-due video read");
	}

	// Wall time alone must not move an A/V stream past its origin before the
	// guest has accepted any PCM. This prevents a stalled guest from receiving
	// current video beside old audio.
	std::this_thread::sleep_for(std::chrono::milliseconds(240));
	if (Mac::ReadFrame(decoder, not_due_bytes.data(), not_due_bytes.size(), &not_due_frame) ||
	    !IsFilled(not_due_bytes, 0x6d) || not_due_frame.time_stamp != UINT64_MAX || !not_due_frame.full_range)
	{
		return Error("wall time bypassed the delivered-audio video master");
	}

	std::vector<uint8_t> pcm;
	uint64_t             audio_pts      = 0;
	bool                 have_audio_pts = false;
	if (!DrainAudioThrough(decoder, &pcm, &audio_pts, &have_audio_pts, 201))
	{
		return false;
	}
	Mac::Internal::AudioReadStateForTest catch_up_audio_state {};
	if (!Mac::Internal::GetAudioReadStateForTest(decoder, &catch_up_audio_state) ||
	    !catch_up_audio_state.delivered_audio_end_valid || catch_up_audio_state.delivered_audio_end_ms <= 200 ||
	    catch_up_audio_state.delivered_audio_end_ms > 250)
	{
		return Error("audio media master did not advance through the catch-up boundary");
	}

	// At 10 fps the catch-up tolerance is 100 ms: an audio end position just
	// past 200 ms makes PTS 100 stale while PTS 200 remains useful.
	if (!ReadAndCheckVideo(decoder, video_format, 2))
	{
		return false;
	}
	if (!MarkVideoEof(decoder, video_format) || !Mac::IsActive(decoder))
	{
		return Error("video catch-up did not preserve the pending audio tail");
	}

	while (pcm.size() < AUDIO_TOTAL_BYTES)
	{
		if (!ReadAudioFrame(decoder, &pcm, &audio_pts, &have_audio_pts, AudioRetryProbe::None)) return false;
	}
	if (!CheckPcm(pcm, true) || Mac::GetPendingAudioFrameSize(decoder) != 0 || Mac::IsActive(decoder))
	{
		return Error("catch-up probe did not preserve PCM or coordinated EOF");
	}
	Mac::Stop(decoder);

#if defined(LUMEN_AV_SYNC_TELEMETRY)
	Mac::Internal::AvSyncTelemetrySummaryForTest summary {};
	if (!Mac::Internal::GetAvSyncTelemetrySummaryForTest(decoder, &summary) || summary.session != 1 ||
	    summary.nominal_video_fps_milli != 10000 || summary.audio_sample_rate != AUDIO_SAMPLE_RATE ||
	    summary.audio_channel_count != AUDIO_CHANNELS || !summary.playback_clock_started ||
	    summary.playback_clock_tick_ns < summary.capture_start_tick_ns || summary.playback_start_pts_ms != 0 ||
	    summary.video_read_calls != 5 || summary.video_delivered != 2 || summary.video_not_due != 2 ||
	    summary.video_catch_up_dropped != 1 || summary.audio_read_calls < AUDIO_SLICE_COUNT + 1 ||
	    summary.audio_delivered != AUDIO_SLICE_COUNT || summary.event_overflow_dropped != 0 || !summary.ended ||
	    summary.end_reason != Mac::Internal::AvSyncTelemetryEndReason::EndOfStream ||
	    summary.event_count > Mac::Internal::AV_SYNC_TELEMETRY_EVENT_CAPACITY)
	{
		return Error("A/V telemetry summary did not preserve audio-master catch-up and natural EOF");
	}

	std::array<Mac::Internal::AvSyncTelemetryEventForTest, 2> delivered_video {};
	size_t                                                      delivered_count       = 0;
	size_t                                                      video_not_due_count   = 0;
	size_t                                                      audio_delivered_count = 0;
	uint64_t                                                    audio_sample_frames   = 0;
	uint64_t                                                    audio_data_size       = 0;
	bool                                                        saw_video_drop        = false;
	bool                                                        saw_video_eof         = false;
	bool                                                        saw_audio_eof         = false;
	for (size_t index = 0; index < summary.event_count; index++)
	{
		Mac::Internal::AvSyncTelemetryEventForTest event {};
		if (!Mac::Internal::GetAvSyncTelemetryEventForTest(decoder, index, &event) || event.sequence != index)
		{
			return Error("A/V telemetry event sequence is incomplete");
		}
		if (event.kind == Mac::Internal::AvSyncTelemetryEventKind::VideoRead &&
		    event.result == Mac::Internal::AvSyncTelemetryResult::Delivered)
		{
			if (delivered_count == delivered_video.size()) return Error("too many delivered video telemetry events");
			delivered_video[delivered_count++] = event;
		} else if (event.kind == Mac::Internal::AvSyncTelemetryEventKind::VideoRead &&
		           event.result == Mac::Internal::AvSyncTelemetryResult::NotDue)
		{
			if (event.pts_ms != 100) return Error("unexpected not-due video PTS in A/V telemetry");
			video_not_due_count++;
		} else if (event.kind == Mac::Internal::AvSyncTelemetryEventKind::VideoDrop &&
		           event.result == Mac::Internal::AvSyncTelemetryResult::CatchUp)
		{
			saw_video_drop = event.pts_ms == 100 && event.media_index == 1;
		} else if (event.kind == Mac::Internal::AvSyncTelemetryEventKind::VideoRead &&
		           event.result == Mac::Internal::AvSyncTelemetryResult::EndOfStream)
		{
			saw_video_eof = true;
		} else if (event.kind == Mac::Internal::AvSyncTelemetryEventKind::AudioRead &&
		           event.result == Mac::Internal::AvSyncTelemetryResult::Delivered)
		{
			audio_delivered_count++;
			audio_sample_frames += event.sample_frames;
			audio_data_size += event.data_size;
		} else if (event.kind == Mac::Internal::AvSyncTelemetryEventKind::AudioRead &&
		           event.result == Mac::Internal::AvSyncTelemetryResult::EndOfStream)
		{
			saw_audio_eof = true;
		}
	}
	if (delivered_count != delivered_video.size() || video_not_due_count != 2 || !saw_video_drop || !saw_video_eof ||
	    !saw_audio_eof || audio_delivered_count != AUDIO_SLICE_COUNT || audio_sample_frames != AUDIO_SAMPLE_FRAMES ||
	    audio_data_size != AUDIO_TOTAL_BYTES)
	{
		return Error("A/V telemetry omitted catch-up, complete PCM or EOF outcomes");
	}
	static constexpr std::array<uint64_t, 2> EXPECTED_PTS   = {0, 200};
	static constexpr std::array<uint64_t, 2> EXPECTED_INDEX = {0, 2};
	for (size_t index = 0; index < delivered_video.size(); index++)
	{
		if (delivered_video[index].pts_ms != EXPECTED_PTS[index] ||
		    delivered_video[index].media_index != EXPECTED_INDEX[index])
		{
			return Error("A/V telemetry changed catch-up PTS or source index");
		}
	}
	if (delivered_video[1].host_tick_ns < summary.playback_clock_tick_ns ||
	    delivered_video[1].host_tick_ns - summary.playback_clock_tick_ns < UINT64_C(200000000))
	{
		return Error("A/V telemetry did not preserve the overdue catch-up timing");
	}
#endif
	return CheckStopped(decoder, video_format);
}

bool TestAudiovisualEof(Mac::Decoder* decoder, const char* filename)
{
	Mac::SetLooping(decoder, false);
	Mac::VideoFormat video_format {};
	Mac::AudioFormat audio_format {};
	if (!Mac::Open(decoder, filename, &video_format, &audio_format) || !CheckVideoFormat(video_format) ||
	    !CheckAudioFormat(audio_format, true))
	{
		std::fprintf(stderr, "A/V open failed: %s\n", Mac::GetLastError(decoder).c_str());
		return false;
	}

	const auto playback_start = Clock::now();
	std::vector<uint8_t> pcm;
	uint64_t             audio_pts      = 0;
	bool                 have_audio_pts = false;
	if (!ReadAudioFrame(decoder, &pcm, &audio_pts, &have_audio_pts, AudioRetryProbe::FirstSlice))
	{
		return false;
	}
	std::vector<uint8_t> small_video(video_format.buffer_size - 1, 0x4d);
	Mac::VideoFrame      small_video_frame {UINT64_MAX, true};
	if (Mac::ReadFrame(decoder, small_video.data(), small_video.size(), &small_video_frame) ||
	    !IsFilled(small_video, 0x4d) || small_video_frame.time_stamp != UINT64_MAX || !small_video_frame.full_range ||
	    !Mac::IsActive(decoder))
	{
		return Error("small video buffer consumed data or changed outputs");
	}
	if (!ReadAndCheckVideo(decoder, video_format, 0)) return false;
	std::vector<uint8_t> pacing_canary(video_format.buffer_size, 0x6b);
	Mac::VideoFrame      pacing_frame {UINT64_MAX, true};
	if (Mac::ReadFrame(decoder, pacing_canary.data(), pacing_canary.size(), &pacing_frame) ||
	    !IsFilled(pacing_canary, 0x6b) || pacing_frame.time_stamp != UINT64_MAX || !pacing_frame.full_range)
	{
		return Error("future video frame bypassed pacing or changed output");
	}

	if (!DrainAudioThrough(decoder, &pcm, &audio_pts, &have_audio_pts, 100, AudioRetryProbe::NonzeroOffset) ||
	    !ReadAndCheckVideo(decoder, video_format, 1) ||
	    !DrainAudioThrough(decoder, &pcm, &audio_pts, &have_audio_pts, 200) ||
	    !ReadAndCheckVideo(decoder, video_format, 2) || !MarkVideoEof(decoder, video_format) ||
	    !Mac::IsActive(decoder))
	{
		return Error("audio-master video pacing or coordinated video EOF failed");
	}

	bool probed_final_partial = false;
	while (pcm.size() < AUDIO_TOTAL_BYTES)
	{
		AudioRetryProbe retry_probe = AudioRetryProbe::None;
		if (AUDIO_TOTAL_BYTES - pcm.size() < AUDIO_MAX_SLICE_BYTES)
		{
			retry_probe          = AudioRetryProbe::FinalPartial;
			probed_final_partial = true;
		}
		if (!ReadAudioFrame(decoder, &pcm, &audio_pts, &have_audio_pts, retry_probe)) return false;
	}
	if (!probed_final_partial || !CheckPcm(pcm, true)) return false;
	if (Mac::GetPendingAudioFrameSize(decoder) != 0 || Mac::IsActive(decoder))
	{
		return Error("A/V decoder did not reach coordinated EOF");
	}
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - playback_start).count();
	if (elapsed < 380) return Error("audio tail completed without per-slice real-time pacing");
	return CheckStopped(decoder, video_format);
}

bool TestAudiovisualLoopAndReopen(Mac::Decoder* decoder, const char* filename)
{
	Mac::SetLooping(decoder, true);
	Mac::VideoFormat video_format {};
	Mac::AudioFormat audio_format {};
	if (!Mac::Open(decoder, filename, &video_format, &audio_format) || !CheckVideoFormat(video_format) ||
	    !CheckAudioFormat(audio_format, true))
	{
		return Error("looping A/V reopen failed");
	}
	std::vector<uint8_t> pcm;
	uint64_t             audio_pts      = 0;
	bool                 have_audio_pts = false;
	if (!ReadAndCheckVideo(decoder, video_format, 0) ||
	    !DrainAudioThrough(decoder, &pcm, &audio_pts, &have_audio_pts, 100) ||
	    !ReadAndCheckVideo(decoder, video_format, 1) ||
	    !DrainAudioThrough(decoder, &pcm, &audio_pts, &have_audio_pts, 200) ||
	    !ReadAndCheckVideo(decoder, video_format, 2))
	{
		return Error("looping A/V audio-master pacing failed");
	}
	if (!MarkVideoEof(decoder, video_format) || !Mac::IsActive(decoder)) return Error("video track EOF was not isolated");

	const auto no_early_loop_deadline = Clock::now() + std::chrono::milliseconds(25);
	while (Clock::now() < no_early_loop_deadline)
	{
		std::vector<uint8_t> canary(video_format.buffer_size, 0x77);
		Mac::VideoFrame      frame {UINT64_MAX, true};
		if (Mac::ReadFrame(decoder, canary.data(), canary.size(), &frame) || !IsFilled(canary, 0x77) ||
		    frame.time_stamp != UINT64_MAX || !frame.full_range)
		{
			return Error("loop restarted before the audio track was drained");
		}
		std::this_thread::sleep_for(POLL_INTERVAL);
	}

	while (pcm.size() < AUDIO_TOTAL_BYTES)
	{
		if (!ReadAudioFrame(decoder, &pcm, &audio_pts, &have_audio_pts, AudioRetryProbe::None)) return false;
	}
	if (!CheckPcm(pcm, true)) return false;

	const auto restart_deadline = Clock::now() + WAIT_TIMEOUT;
	while (Mac::GetPendingAudioFrameSize(decoder) == 0 && Clock::now() < restart_deadline)
	{
		if (!Mac::IsActive(decoder)) return Error("coordinated loop restart failed");
		std::this_thread::sleep_for(POLL_INTERVAL);
	}
	Mac::VideoFrame second_video {};
	if (!ReadAndCheckVideo(decoder, video_format, 0, &second_video)) return false;
	std::vector<uint8_t> second_pcm;
	uint64_t             second_audio_pts = 0;
	bool                 have_second_audio_pts = false;
	if (!ReadAudioFrame(decoder, &second_pcm, &second_audio_pts, &have_second_audio_pts, AudioRetryProbe::None) ||
	    second_video.time_stamp != 0 || second_audio_pts != 0 || !CheckPcm(second_pcm, false))
	{
		return Error("loop did not reset the common A/V clock and PCM stream");
	}

	Mac::Stop(decoder);
	if (!CheckStopped(decoder, video_format)) return Error("Stop did not close both tracks");
	Mac::SetLooping(decoder, false);
	Mac::VideoFormat reopened_video {};
	Mac::AudioFormat reopened_audio {};
	if (!Mac::Open(decoder, filename, &reopened_video, &reopened_audio) || !CheckVideoFormat(reopened_video) ||
	    !CheckAudioFormat(reopened_audio, true))
	{
		return Error("clean reopen after Stop failed");
	}
	Mac::Stop(decoder);
	return CheckStopped(decoder, reopened_video);
}

bool TestVideoOnlyAndOptionalAudio(Mac::Decoder* decoder, const char* video_only, const char* av_file)
{
	Mac::SetLooping(decoder, false);
	Mac::VideoFormat video_format {};
	Mac::AudioFormat audio_format {UINT16_MAX, UINT32_MAX};
	if (!Mac::Open(decoder, video_only, &video_format, &audio_format) || !CheckVideoFormat(video_format) ||
	    !CheckAudioFormat(audio_format, false))
	{
		return Error("video-only open or format failed");
	}
	std::array<uint8_t, 8> audio_canary {};
	audio_canary.fill(0x9c);
	Mac::AudioFrame absent_audio {UINT64_MAX, SIZE_MAX};
	if (Mac::GetPendingAudioFrameSize(decoder) != 0 ||
	    Mac::ReadAudioFrame(decoder, audio_canary.data(), audio_canary.size(), &absent_audio) ||
	    !std::all_of(audio_canary.begin(), audio_canary.end(), [](uint8_t value) { return value == 0x9c; }) ||
	    absent_audio.time_stamp != UINT64_MAX || absent_audio.data_size != SIZE_MAX)
	{
		return Error("video-only asset exposed or mutated audio");
	}
	for (size_t index = 0; index < VIDEO_FRAME_COUNT; index++)
	{
		if (!ReadAndCheckVideo(decoder, video_format, index)) return false;
	}
	if (!MarkVideoEof(decoder, video_format) || Mac::IsActive(decoder)) return Error("video-only EOF failed");

	Mac::SetLooping(decoder, true);
	if (!Mac::Open(decoder, video_only, &video_format, &audio_format)) return Error("video-only loop open failed");
	for (size_t index = 0; index < VIDEO_FRAME_COUNT; index++)
	{
		if (!ReadAndCheckVideo(decoder, video_format, index)) return false;
	}
	if (!ReadAndCheckVideo(decoder, video_format, 0)) return Error("video-only loop did not reset");
	Mac::Stop(decoder);

	// The legacy video-only host API must ignore an audio track when the caller
	// omits AudioFormat, so EOF cannot be held open by unrequested audio.
	Mac::SetLooping(decoder, false);
	if (!Mac::Open(decoder, av_file, &video_format) || !CheckVideoFormat(video_format))
	{
		return Error("optional-audio A/V open failed");
	}
	for (size_t index = 0; index < VIDEO_FRAME_COUNT; index++)
	{
		if (!ReadAndCheckVideo(decoder, video_format, index)) return false;
	}
	if (!MarkVideoEof(decoder, video_format) || Mac::IsActive(decoder))
	{
		return Error("unrequested audio blocked video-only EOF");
	}
	return true;
}

} // namespace

int main(int argc, char* argv[])
{
	if (argc != 3)
	{
		std::fprintf(stderr, "usage: lumen_avplayer_mac_test <av.mov.base64> <video-only.mov.base64>\n");
		return 2;
	}
	if (!TestAudioTimestampResolver()) return 1;
	if (!TestVideoMasterPositionResolver()) return 1;

	TemporaryMovies movies;
	if (!movies.Create(argv[1], argv[2])) return 1;
	Mac::Decoder* decoder = Mac::Create();
	if (decoder == nullptr)
	{
		Error("unable to create AVFoundation decoder");
		return 1;
	}

	const bool catch_up_ok   = TestVideoCatchUp(decoder, movies.Av());
	const bool eof_ok        = catch_up_ok && TestAudiovisualEof(decoder, movies.Av());
	const bool loop_ok       = eof_ok && TestAudiovisualLoopAndReopen(decoder, movies.Av());
	const bool video_only_ok = loop_ok && TestVideoOnlyAndOptionalAudio(decoder, movies.VideoOnly(), movies.Av());
	std::printf("AVFoundation-AvPlayer catch-up=%s video=%s pcm=%s eof=%s loop-stop-reopen=%s video-only=%s\n",
	            catch_up_ok ? "ok" : "failed", eof_ok ? "ok" : "failed", eof_ok ? "ok" : "failed",
	            eof_ok ? "ok" : "failed", loop_ok ? "ok" : "failed", video_only_ok ? "ok" : "failed");
	Mac::Destroy(decoder);
	return catch_up_ok && eof_ok && loop_ok && video_only_ok ? 0 : 1;
}
