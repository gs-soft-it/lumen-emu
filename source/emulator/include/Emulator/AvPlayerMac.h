#ifndef EMULATOR_INCLUDE_EMULATOR_AVPLAYERMAC_H_
#define EMULATOR_INCLUDE_EMULATOR_AVPLAYERMAC_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace Lumen::Libs::Audio::AvPlayer::Mac {

struct Decoder;

// The guest AvPlayer contract consumes codec-sized PCM frames. AVFoundation
// may coalesce several of them into one CMSampleBuffer, so expose bounded
// slices instead of leaking the host-specific buffer granularity.
constexpr size_t AUDIO_FRAME_MAX_SAMPLE_FRAMES = 1024;

struct VideoFormat
{
	uint32_t width       = 0;
	uint32_t height      = 0;
	uint32_t pitch       = 0;
	size_t   buffer_size = 0;
	float    frame_rate  = 0.0f;
	bool     full_range  = false;
};

struct VideoFrame
{
	// Milliseconds from the common playback origin shared with audio.
	uint64_t time_stamp = 0;
	bool     full_range = false;
};

struct AudioFormat
{
	// Both fields remain zero when the asset has no decodable audio track.
	uint16_t channel_count = 0;
	uint32_t sample_rate   = 0;
};

struct AudioFrame
{
	// Milliseconds from the common playback origin shared with video.
	uint64_t time_stamp = 0;
	size_t   data_size  = 0;
};

Decoder* Create();
void     Destroy(Decoder* decoder);

// A null audio_format keeps the legacy video-only path and does not open an audio output.
// When audio is requested, callers must drain both tracks so coordinated EOF/loop can advance.
bool Open(Decoder* decoder, const char* filename, VideoFormat* format, AudioFormat* audio_format = nullptr);
bool ReadFrame(Decoder* decoder, void* destination, size_t destination_size, VideoFrame* frame);

// Returns the capacity for the next <= AUDIO_FRAME_MAX_SAMPLE_FRAMES slice
// without consuming it, even before its due time.
size_t GetPendingAudioFrameSize(Decoder* decoder);
bool   ReadAudioFrame(Decoder* decoder, void* destination, size_t destination_size, AudioFrame* frame);

void SetLooping(Decoder* decoder, bool loop);
void Stop(Decoder* decoder);

bool        IsActive(const Decoder* decoder);
std::string GetLastError(const Decoder* decoder);

namespace Internal {

// Deterministic validation surface for the timestamp fallback. Production
// playback uses the same pure resolver before committing any decoder state.
struct AudioTimestampResolutionForTest
{
	int64_t  time_stamp          = 0;
	bool     update_anchor       = false;
	int64_t  anchor_time_stamp   = 0;
	uint64_t anchor_sample_frame = 0;
};

AudioTimestampResolutionForTest ResolveAudioTimestampForTest(bool source_time_stamp_valid,
                                                              int64_t source_time_stamp, bool anchor_valid,
                                                              int64_t anchor_time_stamp,
                                                              uint64_t anchor_sample_frame,
                                                              uint64_t current_sample_frame, uint32_t sample_rate,
                                                              bool previous_time_stamp_valid,
                                                              int64_t previous_time_stamp);

// Pure validation surface for the video media-master policy. A known gap
// before the next PCM slice may follow the wall clock up to that slice's PTS;
// an unknown gap remains fail-closed at the last delivered PCM boundary.
uint64_t ResolveVideoMasterPositionForTest(bool has_audio, bool audio_eof,
                                           uint64_t wall_clock_position_ms,
                                           bool delivered_audio_end_valid,
                                           uint64_t delivered_audio_end_ms,
                                           bool pending_audio_time_stamp_valid,
                                           uint64_t pending_audio_time_stamp_ms);

// Read-only validation state used to prove that an undersized destination is
// atomic with respect to PCM position, the fallback anchor and the A/V clock.
struct AudioReadStateForTest
{
	size_t   pending_offset_frames    = 0;
	uint64_t consumed_sample_frames   = 0;
	bool     time_stamp_anchor_valid  = false;
	int64_t  time_stamp_anchor        = 0;
	uint64_t time_stamp_anchor_frames = 0;
	bool     playback_clock_started   = false;
	// End position, in milliseconds from the common playback origin, of the
	// latest PCM slice successfully delivered to the guest.
	bool     delivered_audio_end_valid = false;
	uint64_t delivered_audio_end_ms    = 0;
};

bool GetAudioReadStateForTest(const Decoder* decoder, AudioReadStateForTest* state);

#if defined(LUMEN_AV_SYNC_TELEMETRY)
// The diagnostic build records only the first A/V session in the process. This
// scope covers AvPlayer PTS and read outcomes; it does not provide AudioOut
// token/callback linkage. The fixed event budget is allocated before playback,
// and recording performs no allocation, I/O or additional locking in either
// read path.
constexpr size_t AV_SYNC_TELEMETRY_EVENT_CAPACITY = 65536;

enum class AvSyncTelemetryEventKind : uint32_t
{
	Open = 1,
	ClockStart,
	VideoRead,
	VideoDrop,
	AudioRead,
	End,
};

enum class AvSyncTelemetryResult : uint32_t
{
	Ok = 1,
	Delivered,
	NotDue,
	Inactive,
	EndOfStream,
	InvalidDestination,
	DecodeError,
	CatchUp,
	CatchUpPending,
};

enum class AvSyncTelemetryEndReason : uint32_t
{
	None = 0,
	EndOfStream,
	Stop,
	Reopen,
	Destroy,
	DurationLimit,
};

struct AvSyncTelemetryEventForTest
{
	uint64_t                 sequence      = 0;
	uint64_t                 host_tick_ns  = 0;
	uint64_t                 duration_ns   = 0;
	uint64_t                 pts_ms        = 0;
	uint64_t                 media_index   = 0;
	uint64_t                 sample_frames = 0;
	uint64_t                 data_size     = 0;
	AvSyncTelemetryEventKind kind          = AvSyncTelemetryEventKind::Open;
	AvSyncTelemetryResult    result        = AvSyncTelemetryResult::Ok;
};

struct AvSyncTelemetrySummaryForTest
{
	uint64_t                 session                 = 0;
	uint64_t                 capture_start_tick_ns   = 0;
	uint64_t                 playback_clock_tick_ns  = 0;
	int64_t                  playback_start_pts_ms   = 0;
	uint64_t                 event_count             = 0;
	uint64_t                 event_overflow_dropped  = 0;
	uint64_t                 video_read_calls        = 0;
	uint64_t                 video_delivered         = 0;
	uint64_t                 video_not_due           = 0;
	uint64_t                 video_catch_up_dropped  = 0;
	uint64_t                 audio_read_calls        = 0;
	uint64_t                 audio_delivered         = 0;
	uint64_t                 audio_not_due           = 0;
	uint32_t                 nominal_video_fps_milli = 0;
	uint32_t                 audio_sample_rate       = 0;
	uint16_t                 audio_channel_count     = 0;
	bool                     playback_clock_started  = false;
	bool                     ended                   = false;
	AvSyncTelemetryEndReason end_reason              = AvSyncTelemetryEndReason::None;
};

bool GetAvSyncTelemetrySummaryForTest(const Decoder* decoder, AvSyncTelemetrySummaryForTest* summary);
bool GetAvSyncTelemetryEventForTest(const Decoder* decoder, size_t index, AvSyncTelemetryEventForTest* event);
#endif

} // namespace Internal

} // namespace Lumen::Libs::Audio::AvPlayer::Mac

#endif /* EMULATOR_INCLUDE_EMULATOR_AVPLAYERMAC_H_ */
