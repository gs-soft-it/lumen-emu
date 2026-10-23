#include "Emulator/AvPlayerMac.h"

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>

namespace Lumen::Libs::Audio::AvPlayer::Mac {

#if defined(LUMEN_AV_SYNC_TELEMETRY)
using AvSyncEvent      = Internal::AvSyncTelemetryEventForTest;
using AvSyncEventKind  = Internal::AvSyncTelemetryEventKind;
using AvSyncResult     = Internal::AvSyncTelemetryResult;
using AvSyncEndReason  = Internal::AvSyncTelemetryEndReason;

constexpr uint64_t AV_SYNC_CLOCK_FREQUENCY = UINT64_C(1000000000);
constexpr uint64_t AV_SYNC_MAX_DURATION_NS  = UINT64_C(120) * AV_SYNC_CLOCK_FREQUENCY;

struct AvSyncTelemetryState
{
	std::array<AvSyncEvent, Internal::AV_SYNC_TELEMETRY_EVENT_CAPACITY> events;
	size_t          event_count            = 0;
	uint64_t        next_sequence          = 0;
	uint64_t        event_overflow_dropped = 0;
	uint64_t        session                = 1;
	uint64_t        capture_start_tick_ns  = 0;
	uint64_t        playback_clock_tick_ns = 0;
	int64_t         playback_start_pts_ms  = 0;
	uint64_t        video_read_calls       = 0;
	uint64_t        video_delivered        = 0;
	uint64_t        video_not_due          = 0;
	uint64_t        video_catch_up_dropped = 0;
	uint64_t        audio_read_calls       = 0;
	uint64_t        audio_delivered        = 0;
	uint64_t        audio_not_due          = 0;
	uint32_t        nominal_video_fps_milli = 0;
	uint32_t        audio_sample_rate       = 0;
	uint16_t        audio_channel_count     = 0;
	bool            playback_clock_started = false;
	bool            ended                  = false;
	bool            flushed                = false;
	AvSyncEndReason end_reason              = AvSyncEndReason::None;
};

static std::atomic_bool gAvSyncFirstSessionClaimed {false};
#endif

// Keep 100 ms of jitter tolerance and at least one nominal source period for
// low-frame-rate media. The per-read cap bounds decoder work while covering
// more than four seconds of backlog at 59.94 fps.
constexpr uint64_t VIDEO_CATCH_UP_MIN_LAG_MS         = 100;
constexpr uint32_t VIDEO_CATCH_UP_MAX_DROPS_PER_READ = 256;

struct Decoder
{
	mutable std::mutex mutex;
	std::string        filename;
	std::string        last_error;

	AVURLAsset*               asset         = nil;
	AVAssetReader*            reader        = nil;
	AVAssetReaderTrackOutput* video_output  = nil;
	AVAssetReaderTrackOutput* audio_output  = nil;
	CMSampleBufferRef         pending_video = nullptr;
	CMSampleBufferRef         pending_audio = nullptr;
	size_t                    pending_audio_offset_frames = 0;

	VideoFormat video_format {};
	AudioFormat audio_format {};

	std::chrono::steady_clock::time_point playback_start {};
	int64_t                   playback_start_timestamp = 0;
	uint64_t                  video_frame_index         = 0;
	uint64_t                  video_read_calls          = 0;
	uint64_t                  video_not_due_reads       = 0;
	uint64_t                  video_catch_up_dropped    = 0;
	int64_t                   video_last_timestamp      = 0;
	uint64_t                  audio_frame_index         = 0;
	uint64_t                  audio_sample_frames       = 0;
	uint64_t                  audio_read_calls          = 0;
	uint64_t                  audio_not_due_reads       = 0;
	int64_t                   audio_last_timestamp      = 0;
	bool                      audio_timestamp_anchor_valid = false;
	int64_t                   audio_timestamp_anchor       = 0;
	uint64_t                  audio_timestamp_anchor_frames = 0;
	bool                      delivered_audio_end_valid = false;
	uint64_t                  delivered_audio_end_ms    = 0;
	bool                      playback_clock_started    = false;
	bool                      audio_requested           = false;
	bool                      has_audio                 = false;
	bool                      video_eof                 = true;
	bool                      audio_eof                 = true;
	bool                      loop                      = false;
	bool                      active                    = false;
	bool                      eof                       = true;
	bool                      stopped                   = true;
#if defined(LUMEN_AV_SYNC_TELEMETRY)
	std::unique_ptr<AvSyncTelemetryState> av_sync_telemetry;
#endif
};

static bool TraceEnabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_AVPLAYER_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	return enabled;
}

static int64_t TraceTimeMilliseconds()
{
	static const auto start = std::chrono::steady_clock::now();
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

static void Trace(const char* format, ...)
{
	if (!TraceEnabled())
	{
		return;
	}
	std::fprintf(stderr, "[AVPLAYER_MAC] elapsed=%" PRId64 " ", TraceTimeMilliseconds());
	va_list args;
	va_start(args, format);
	std::vfprintf(stderr, format, args);
	va_end(args);
	std::fputc('\n', stderr);
}

#if defined(LUMEN_AV_SYNC_TELEMETRY)
static uint64_t AvSyncTick(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
{
	const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
	return ticks > 0 ? static_cast<uint64_t>(ticks) : 0;
}

static const char* AvSyncKindName(AvSyncEventKind kind)
{
	switch (kind)
	{
		case AvSyncEventKind::Open: return "open";
		case AvSyncEventKind::ClockStart: return "clock_start";
		case AvSyncEventKind::VideoRead: return "video_read";
		case AvSyncEventKind::VideoDrop: return "video_drop";
		case AvSyncEventKind::AudioRead: return "audio_read";
		case AvSyncEventKind::End: return "end";
	}
	return "unknown";
}

static const char* AvSyncResultName(AvSyncResult result)
{
	switch (result)
	{
		case AvSyncResult::Ok: return "ok";
		case AvSyncResult::Delivered: return "delivered";
		case AvSyncResult::NotDue: return "not_due";
		case AvSyncResult::Inactive: return "inactive";
		case AvSyncResult::EndOfStream: return "eof";
		case AvSyncResult::InvalidDestination: return "invalid_destination";
		case AvSyncResult::DecodeError: return "decode_error";
		case AvSyncResult::CatchUp: return "catch_up";
		case AvSyncResult::CatchUpPending: return "catch_up_pending";
	}
	return "unknown";
}

static const char* AvSyncEndReasonName(AvSyncEndReason reason)
{
	switch (reason)
	{
		case AvSyncEndReason::None: return "none";
		case AvSyncEndReason::EndOfStream: return "eof";
		case AvSyncEndReason::Stop: return "stop";
		case AvSyncEndReason::Reopen: return "reopen";
		case AvSyncEndReason::Destroy: return "destroy";
		case AvSyncEndReason::DurationLimit: return "duration_limit";
	}
	return "unknown";
}

static void AvSyncAppendRaw(AvSyncTelemetryState* telemetry, AvSyncEventKind kind, AvSyncResult result,
	                        uint64_t host_tick_ns, uint64_t duration_ns, uint64_t pts_ms,
	                        uint64_t media_index, uint64_t sample_frames, uint64_t data_size)
{
	if (telemetry == nullptr)
	{
		return;
	}
	AvSyncEvent event {};
	event.sequence      = telemetry->next_sequence++;
	event.host_tick_ns  = host_tick_ns;
	event.duration_ns   = duration_ns;
	event.pts_ms        = pts_ms;
	event.media_index   = media_index;
	event.sample_frames = sample_frames;
	event.data_size     = data_size;
	event.kind          = kind;
	event.result        = result;
	if (telemetry->event_count == telemetry->events.size())
	{
		telemetry->event_overflow_dropped++;
		return;
	}
	telemetry->events[telemetry->event_count++] = event;
}

static void AvSyncEnd(Decoder* decoder, AvSyncEndReason reason, uint64_t host_tick_ns = 0)
{
	auto* telemetry = decoder != nullptr ? decoder->av_sync_telemetry.get() : nullptr;
	if (telemetry == nullptr || telemetry->ended)
	{
		return;
	}
	const uint64_t tick = host_tick_ns != 0 ? host_tick_ns : AvSyncTick();
	telemetry->ended     = true;
	telemetry->end_reason = reason;
	AvSyncAppendRaw(telemetry, AvSyncEventKind::End, AvSyncResult::Ok, tick, 0, 0, 0, 0, 0);
}

static bool AvSyncCanRecord(Decoder* decoder, uint64_t host_tick_ns)
{
	auto* telemetry = decoder != nullptr ? decoder->av_sync_telemetry.get() : nullptr;
	if (telemetry == nullptr || telemetry->ended)
	{
		return false;
	}
	if (host_tick_ns >= telemetry->capture_start_tick_ns &&
	    host_tick_ns - telemetry->capture_start_tick_ns >= AV_SYNC_MAX_DURATION_NS)
	{
		AvSyncEnd(decoder, AvSyncEndReason::DurationLimit, host_tick_ns);
		return false;
	}
	return true;
}

static uint64_t AvSyncBeginRead(const Decoder* decoder)
{
	return decoder != nullptr && decoder->av_sync_telemetry != nullptr && !decoder->av_sync_telemetry->ended
	           ? AvSyncTick()
	           : 0;
}

static void AvSyncRecordClockStart(Decoder* decoder, std::chrono::steady_clock::time_point now,
	                               int64_t playback_start_pts_ms)
{
	const uint64_t tick = AvSyncTick(now);
	if (!AvSyncCanRecord(decoder, tick))
	{
		return;
	}
	auto* telemetry                    = decoder->av_sync_telemetry.get();
	telemetry->playback_clock_started = true;
	telemetry->playback_clock_tick_ns = tick;
	telemetry->playback_start_pts_ms  = playback_start_pts_ms;
	AvSyncAppendRaw(telemetry, AvSyncEventKind::ClockStart, AvSyncResult::Ok, tick, 0, 0, 0, 0, 0);
}

static void AvSyncRecordVideoRead(Decoder* decoder, uint64_t begin_tick_ns, AvSyncResult result,
	                              uint64_t pts_ms, uint64_t frame_index, uint64_t data_size)
{
	if (begin_tick_ns == 0)
	{
		return;
	}
	const uint64_t tick = AvSyncTick();
	if (!AvSyncCanRecord(decoder, tick))
	{
		return;
	}
	auto* telemetry = decoder->av_sync_telemetry.get();
	telemetry->video_read_calls++;
	telemetry->video_delivered += result == AvSyncResult::Delivered ? 1 : 0;
	telemetry->video_not_due += result == AvSyncResult::NotDue ? 1 : 0;
	AvSyncAppendRaw(telemetry, AvSyncEventKind::VideoRead, result, tick,
	                tick >= begin_tick_ns ? tick - begin_tick_ns : 0, pts_ms, frame_index, 0, data_size);
}

static void AvSyncRecordVideoDrop(Decoder* decoder, uint64_t pts_ms, uint64_t frame_index)
{
	auto* telemetry = decoder != nullptr ? decoder->av_sync_telemetry.get() : nullptr;
	const uint64_t tick = telemetry != nullptr && !telemetry->ended ? AvSyncTick() : 0;
	if (tick == 0 || !AvSyncCanRecord(decoder, tick))
	{
		return;
	}
	telemetry->video_catch_up_dropped++;
	AvSyncAppendRaw(telemetry, AvSyncEventKind::VideoDrop, AvSyncResult::CatchUp, tick, 0, pts_ms,
	                frame_index, 0, 0);
}

static void AvSyncRecordAudioRead(Decoder* decoder, uint64_t begin_tick_ns, AvSyncResult result,
	                              uint64_t pts_ms, uint64_t frame_index, uint64_t sample_frames,
	                              uint64_t data_size)
{
	if (begin_tick_ns == 0)
	{
		return;
	}
	const uint64_t tick = AvSyncTick();
	if (!AvSyncCanRecord(decoder, tick))
	{
		return;
	}
	auto* telemetry = decoder->av_sync_telemetry.get();
	telemetry->audio_read_calls++;
	telemetry->audio_delivered += result == AvSyncResult::Delivered ? 1 : 0;
	telemetry->audio_not_due += result == AvSyncResult::NotDue ? 1 : 0;
	AvSyncAppendRaw(telemetry, AvSyncEventKind::AudioRead, result, tick,
	                tick >= begin_tick_ns ? tick - begin_tick_ns : 0, pts_ms, frame_index, sample_frames, data_size);
}

static void AvSyncStart(Decoder* decoder)
{
	if (decoder == nullptr || !decoder->has_audio || decoder->av_sync_telemetry != nullptr)
	{
		return;
	}
	bool expected = false;
	if (!gAvSyncFirstSessionClaimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
	{
		return;
	}
	std::unique_ptr<AvSyncTelemetryState> telemetry(new (std::nothrow) AvSyncTelemetryState);
	if (telemetry == nullptr)
	{
		gAvSyncFirstSessionClaimed.store(false, std::memory_order_release);
		return;
	}
	telemetry->capture_start_tick_ns = AvSyncTick();
	if (std::isfinite(decoder->video_format.frame_rate) && decoder->video_format.frame_rate > 0.0f)
	{
		const double milli = std::round(static_cast<double>(decoder->video_format.frame_rate) * 1000.0);
		telemetry->nominal_video_fps_milli =
		    milli <= static_cast<double>(UINT32_MAX) ? static_cast<uint32_t>(milli) : UINT32_MAX;
	}
	telemetry->audio_sample_rate   = decoder->audio_format.sample_rate;
	telemetry->audio_channel_count = decoder->audio_format.channel_count;
	decoder->av_sync_telemetry     = std::move(telemetry);
	AvSyncAppendRaw(decoder->av_sync_telemetry.get(), AvSyncEventKind::Open, AvSyncResult::Ok,
	                decoder->av_sync_telemetry->capture_start_tick_ns, 0, 0, 0, 0, 0);
}

static void AvSyncFlush(Decoder* decoder)
{
	auto* telemetry = decoder != nullptr ? decoder->av_sync_telemetry.get() : nullptr;
	if (telemetry == nullptr || telemetry->flushed)
	{
		return;
	}
	telemetry->flushed = true;
	std::fprintf(stderr,
	             "[AV_SYNC_HEADER] version=1 session=%" PRIu64 " clock=steady_nanoseconds frequency=%" PRIu64
	             " capacity=%zu capture_start_tick=%" PRIu64 " clock_started=%u clock_start_tick=%" PRIu64
	             " clock_start_pts_ms=%" PRId64 " nominal_video_fps_milli=%u audio_rate=%u audio_channels=%u\n",
	             telemetry->session, AV_SYNC_CLOCK_FREQUENCY, telemetry->events.size(), telemetry->capture_start_tick_ns,
	             telemetry->playback_clock_started ? 1u : 0u, telemetry->playback_clock_tick_ns,
	             telemetry->playback_start_pts_ms, telemetry->nominal_video_fps_milli, telemetry->audio_sample_rate,
	             static_cast<unsigned>(telemetry->audio_channel_count));
	for (size_t index = 0; index < telemetry->event_count; index++)
	{
		const auto& event = telemetry->events[index];
		std::fprintf(stderr,
		             "[AV_SYNC_EVENT] session=%" PRIu64 " sequence=%" PRIu64 " kind=%s result=%s host_tick=%" PRIu64
		             " duration_ns=%" PRIu64 " pts_ms=%" PRIu64 " media_index=%" PRIu64
		             " sample_frames=%" PRIu64 " bytes=%" PRIu64 "\n",
		             telemetry->session, event.sequence, AvSyncKindName(event.kind), AvSyncResultName(event.result),
		             event.host_tick_ns, event.duration_ns, event.pts_ms, event.media_index, event.sample_frames,
		             event.data_size);
	}
	std::fprintf(stderr,
	             "[AV_SYNC_END] version=1 session=%" PRIu64 " reason=%s events=%zu event_overflow_dropped=%" PRIu64
	             " video_reads=%" PRIu64 " video_delivered=%" PRIu64 " video_not_due=%" PRIu64
	             " video_catch_up_dropped=%" PRIu64
	             " audio_reads=%" PRIu64 " audio_delivered=%" PRIu64 " audio_not_due=%" PRIu64 "\n",
	             telemetry->session, AvSyncEndReasonName(telemetry->end_reason), telemetry->event_count,
	             telemetry->event_overflow_dropped, telemetry->video_read_calls, telemetry->video_delivered,
	             telemetry->video_not_due, telemetry->video_catch_up_dropped, telemetry->audio_read_calls, telemetry->audio_delivered,
	             telemetry->audio_not_due);
	std::fflush(stderr);
}
#endif

static void SetError(Decoder* decoder, const char* message, NSError* error = nil)
{
	decoder->last_error = message != nullptr ? message : "unknown AVFoundation error";
	if (error != nil)
	{
		const char* description = [[error localizedDescription] UTF8String];
		if (description != nullptr && description[0] != '\0')
		{
			decoder->last_error += ": ";
			decoder->last_error += description;
		}
	}
}

static void ReleaseSample(CMSampleBufferRef* sample)
{
	if (*sample != nullptr)
	{
		CFRelease(*sample);
		*sample = nullptr;
	}
}

static void ReleaseReader(Decoder* decoder)
{
	ReleaseSample(&decoder->pending_video);
	ReleaseSample(&decoder->pending_audio);
	decoder->pending_audio_offset_frames = 0;
	if (decoder->reader != nil)
	{
		[decoder->reader cancelReading];
	}
	[decoder->video_output release];
	decoder->video_output = nil;
	[decoder->audio_output release];
	decoder->audio_output = nil;
	[decoder->reader release];
	decoder->reader = nil;
}

static void ReleaseSource(Decoder* decoder)
{
	ReleaseReader(decoder);
	[decoder->asset release];
	decoder->asset = nil;
	decoder->video_format = {};
	decoder->audio_format = {};
	decoder->playback_start_timestamp = 0;
	decoder->video_frame_index         = 0;
	decoder->video_read_calls          = 0;
	decoder->video_not_due_reads       = 0;
	decoder->video_catch_up_dropped    = 0;
	decoder->video_last_timestamp      = 0;
	decoder->audio_frame_index         = 0;
	decoder->audio_sample_frames       = 0;
	decoder->audio_read_calls          = 0;
	decoder->audio_not_due_reads       = 0;
	decoder->audio_last_timestamp      = 0;
	decoder->audio_timestamp_anchor_valid = false;
	decoder->audio_timestamp_anchor       = 0;
	decoder->audio_timestamp_anchor_frames = 0;
	decoder->delivered_audio_end_valid = false;
	decoder->delivered_audio_end_ms    = 0;
	decoder->pending_audio_offset_frames = 0;
	decoder->playback_clock_started   = false;
	decoder->audio_requested          = false;
	decoder->has_audio                = false;
	decoder->video_eof                = true;
	decoder->audio_eof                = true;
	decoder->active                   = false;
	decoder->eof                      = true;
	decoder->stopped                  = true;
}

static CMSampleBufferRef CopyNextSample(AVAssetReaderTrackOutput* output)
{
	if (output == nil)
	{
		return nullptr;
	}

	while (true)
	{
		CMSampleBufferRef sample = [output copyNextSampleBuffer];
		if (sample == nullptr || CMSampleBufferGetNumSamples(sample) != 0)
		{
			return sample;
		}
		CFRelease(sample);
	}
}

static bool InspectVideoSample(Decoder* decoder, CMSampleBufferRef sample, AVAssetTrack* track)
{
	CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
	if (image == nullptr)
	{
		SetError(decoder, "AVFoundation returned a video sample without a pixel buffer");
		return false;
	}

	auto* pixel_buffer = static_cast<CVPixelBufferRef>(image);
	const OSType pixel_format = CVPixelBufferGetPixelFormatType(pixel_buffer);
	if (pixel_format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
	    pixel_format != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
	{
		SetError(decoder, "AVFoundation did not produce 8-bit bi-planar NV12");
		return false;
	}
	if (CVPixelBufferGetPlaneCount(pixel_buffer) != 2)
	{
		SetError(decoder, "AVFoundation returned an invalid NV12 plane layout");
		return false;
	}

	const size_t width         = CVPixelBufferGetWidth(pixel_buffer);
	const size_t height        = CVPixelBufferGetHeight(pixel_buffer);
	const size_t chroma_height = CVPixelBufferGetHeightOfPlane(pixel_buffer, 1);
	if (width == 0 || height == 0 || (width & 1u) != 0 || (height & 1u) != 0 || chroma_height != height / 2 ||
	    width > std::numeric_limits<uint32_t>::max() ||
	    height > std::numeric_limits<uint32_t>::max() || height > std::numeric_limits<size_t>::max() - chroma_height ||
	    width > std::numeric_limits<size_t>::max() / (height + chroma_height))
	{
		SetError(decoder, "AVFoundation returned invalid video dimensions");
		return false;
	}

	decoder->video_format.width       = static_cast<uint32_t>(width);
	decoder->video_format.height      = static_cast<uint32_t>(height);
	decoder->video_format.pitch       = static_cast<uint32_t>(width);
	decoder->video_format.buffer_size = width * (height + chroma_height);
	const float nominal_frame_rate    = track != nil ? [track nominalFrameRate] : 0.0f;
	decoder->video_format.frame_rate  =
	    std::isfinite(nominal_frame_rate) && nominal_frame_rate > 0.0f ? nominal_frame_rate : 0.0f;
	decoder->video_format.full_range  = pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
	return true;
}

static bool InspectAudioSample(Decoder* decoder, CMSampleBufferRef sample, AudioFormat* format)
{
	if (sample == nullptr || format == nullptr)
	{
		SetError(decoder, "AVFoundation returned an invalid audio sample");
		return false;
	}

	CMAudioFormatDescriptionRef description = CMSampleBufferGetFormatDescription(sample);
	const AudioStreamBasicDescription* stream =
	    description != nullptr ? CMAudioFormatDescriptionGetStreamBasicDescription(description) : nullptr;
	if (stream == nullptr || stream->mFormatID != kAudioFormatLinearPCM)
	{
		SetError(decoder, "AVFoundation did not produce linear PCM audio");
		return false;
	}

	constexpr AudioFormatFlags forbidden_flags =
	    kAudioFormatFlagIsFloat | kAudioFormatFlagIsBigEndian | kAudioFormatFlagIsNonInterleaved;
	const bool signed_packed = (stream->mFormatFlags & kAudioFormatFlagIsSignedInteger) != 0 &&
	                           (stream->mFormatFlags & kAudioFormatFlagIsPacked) != 0;
	if (stream->mBitsPerChannel != 16 || !signed_packed || (stream->mFormatFlags & forbidden_flags) != 0)
	{
		SetError(decoder, "AVFoundation did not produce signed 16-bit little-endian interleaved PCM");
		return false;
	}
	if (stream->mChannelsPerFrame == 0 || stream->mChannelsPerFrame > std::numeric_limits<uint16_t>::max())
	{
		SetError(decoder, "AVFoundation returned an invalid audio channel count");
		return false;
	}

	const double rounded_sample_rate = std::round(stream->mSampleRate);
	if (!std::isfinite(stream->mSampleRate) || rounded_sample_rate <= 0.0 ||
	    rounded_sample_rate > static_cast<double>(std::numeric_limits<uint32_t>::max()) ||
	    std::fabs(stream->mSampleRate - rounded_sample_rate) > 0.001)
	{
		SetError(decoder, "AVFoundation returned an invalid audio sample rate");
		return false;
	}

	const uint32_t bytes_per_frame = stream->mChannelsPerFrame * sizeof(int16_t);
	if (stream->mBytesPerFrame != bytes_per_frame || stream->mFramesPerPacket != 1 ||
	    stream->mBytesPerPacket != bytes_per_frame)
	{
		SetError(decoder, "AVFoundation returned an invalid interleaved PCM frame layout");
		return false;
	}

	format->channel_count = static_cast<uint16_t>(stream->mChannelsPerFrame);
	format->sample_rate   = static_cast<uint32_t>(rounded_sample_rate);
	return true;
}

static bool GetAudioSampleSize(Decoder* decoder, CMSampleBufferRef sample, size_t* size)
{
	AudioFormat sample_format {};
	if (size == nullptr || !InspectAudioSample(decoder, sample, &sample_format))
	{
		return false;
	}
	if (sample_format.channel_count != decoder->audio_format.channel_count ||
	    sample_format.sample_rate != decoder->audio_format.sample_rate)
	{
		SetError(decoder, "AVFoundation changed audio format during playback");
		return false;
	}

	const CMItemCount sample_count    = CMSampleBufferGetNumSamples(sample);
	const size_t      bytes_per_frame = static_cast<size_t>(sample_format.channel_count) * sizeof(int16_t);
	if (sample_count <= 0 || static_cast<uint64_t>(sample_count) > std::numeric_limits<size_t>::max() / bytes_per_frame)
	{
		SetError(decoder, "AVFoundation returned an invalid PCM sample count");
		return false;
	}

	const size_t expected_size = static_cast<size_t>(sample_count) * bytes_per_frame;
	const size_t sample_size   = CMSampleBufferGetTotalSampleSize(sample);
	if (expected_size > std::numeric_limits<uint32_t>::max() || sample_size != expected_size)
	{
		SetError(decoder, "AVFoundation returned an invalid packed PCM buffer size");
		return false;
	}

	*size = expected_size;
	return true;
}

static bool GetPendingAudioSliceLayout(Decoder* decoder, CMSampleBufferRef sample, size_t* sample_frames,
                                       size_t* data_size)
{
	if (sample_frames == nullptr || data_size == nullptr)
	{
		SetError(decoder, "AVFoundation audio slice output is null");
		return false;
	}

	size_t full_size = 0;
	if (!GetAudioSampleSize(decoder, sample, &full_size))
	{
		return false;
	}

	const CMItemCount total_frames = CMSampleBufferGetNumSamples(sample);
	if (total_frames <= 0 || decoder->pending_audio_offset_frames >= static_cast<uint64_t>(total_frames))
	{
		SetError(decoder, "AVFoundation audio slice offset is invalid");
		return false;
	}

	const size_t total       = static_cast<size_t>(total_frames);
	const size_t remaining   = total - decoder->pending_audio_offset_frames;
	const size_t frames      = std::min(remaining, AUDIO_FRAME_MAX_SAMPLE_FRAMES);
	const size_t frame_bytes = static_cast<size_t>(decoder->audio_format.channel_count) * sizeof(int16_t);
	if (frames == 0 || frame_bytes == 0 || frames > std::numeric_limits<size_t>::max() / frame_bytes)
	{
		SetError(decoder, "AVFoundation audio slice size is invalid");
		return false;
	}

	const size_t bytes = frames * frame_bytes;
	if (decoder->pending_audio_offset_frames > std::numeric_limits<size_t>::max() / frame_bytes ||
	    decoder->pending_audio_offset_frames * frame_bytes > full_size ||
	    bytes > full_size - decoder->pending_audio_offset_frames * frame_bytes)
	{
		SetError(decoder, "AVFoundation audio slice exceeds the pending PCM buffer");
		return false;
	}

	*sample_frames = frames;
	*data_size     = bytes;
	return true;
}

static bool StartReader(Decoder* decoder)
{
	ReleaseReader(decoder);
	decoder->video_format              = {};
	decoder->audio_format              = {};
	decoder->playback_start_timestamp  = 0;
	decoder->video_frame_index         = 0;
	decoder->video_read_calls          = 0;
	decoder->video_not_due_reads       = 0;
	decoder->video_catch_up_dropped    = 0;
	decoder->video_last_timestamp      = 0;
	decoder->audio_frame_index         = 0;
	decoder->audio_sample_frames       = 0;
	decoder->audio_read_calls          = 0;
	decoder->audio_not_due_reads       = 0;
	decoder->audio_last_timestamp      = 0;
	decoder->audio_timestamp_anchor_valid = false;
	decoder->audio_timestamp_anchor       = 0;
	decoder->audio_timestamp_anchor_frames = 0;
	decoder->delivered_audio_end_valid = false;
	decoder->delivered_audio_end_ms    = 0;
	decoder->pending_audio_offset_frames = 0;
	decoder->playback_clock_started    = false;
	decoder->has_audio                 = false;
	decoder->video_eof                 = true;
	decoder->audio_eof                 = true;
	decoder->active                    = false;
	decoder->eof                       = true;

	if (decoder->asset == nil)
	{
		SetError(decoder, "AVFoundation media asset is not open");
		return false;
	}

	NSArray<AVAssetTrack*>* video_tracks = [decoder->asset tracksWithMediaType:AVMediaTypeVideo];
	AVAssetTrack* video_track = [video_tracks firstObject];
	if (video_track == nil)
	{
		SetError(decoder, "AVFoundation asset has no video track");
		return false;
	}
	AVAssetTrack* audio_track = nil;
	if (decoder->audio_requested)
	{
		NSArray<AVAssetTrack*>* audio_tracks = [decoder->asset tracksWithMediaType:AVMediaTypeAudio];
		audio_track = [audio_tracks firstObject];
	}

	NSError* error = nil;
	decoder->reader = [[AVAssetReader alloc] initWithAsset:decoder->asset error:&error];
	if (decoder->reader == nil)
	{
		SetError(decoder, "Unable to create AVFoundation asset reader", error);
		return false;
	}

	NSDictionary* video_settings = @{
		(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
	};
	decoder->video_output = [[AVAssetReaderTrackOutput alloc] initWithTrack:video_track outputSettings:video_settings];
	if (decoder->video_output == nil || ![decoder->reader canAddOutput:decoder->video_output])
	{
		SetError(decoder, "Unable to configure AVFoundation NV12 output");
		ReleaseReader(decoder);
		return false;
	}
	decoder->video_output.alwaysCopiesSampleData = NO;
	[decoder->reader addOutput:decoder->video_output];

	if (audio_track != nil)
	{
		NSDictionary* audio_settings = @{
			AVFormatIDKey: @(kAudioFormatLinearPCM),
			AVLinearPCMBitDepthKey: @16,
			AVLinearPCMIsBigEndianKey: @NO,
			AVLinearPCMIsFloatKey: @NO,
			AVLinearPCMIsNonInterleaved: @NO
		};
		decoder->audio_output = [[AVAssetReaderTrackOutput alloc] initWithTrack:audio_track outputSettings:audio_settings];
		if (decoder->audio_output == nil || ![decoder->reader canAddOutput:decoder->audio_output])
		{
			SetError(decoder, "Unable to configure AVFoundation signed 16-bit PCM output");
			ReleaseReader(decoder);
			return false;
		}
		decoder->audio_output.alwaysCopiesSampleData = NO;
		[decoder->reader addOutput:decoder->audio_output];
	}

	if (![decoder->reader startReading])
	{
		SetError(decoder, "Unable to start AVFoundation asset reader", [decoder->reader error]);
		ReleaseReader(decoder);
		return false;
	}

	decoder->pending_video = CopyNextSample(decoder->video_output);
	if (decoder->pending_video == nullptr)
	{
		SetError(decoder, "AVFoundation asset contains no decodable video frames", [decoder->reader error]);
		ReleaseReader(decoder);
		return false;
	}
	if (!InspectVideoSample(decoder, decoder->pending_video, video_track))
	{
		ReleaseReader(decoder);
		return false;
	}

	decoder->video_eof = false;
	if (decoder->audio_output != nil)
	{
		decoder->pending_audio = CopyNextSample(decoder->audio_output);
		decoder->pending_audio_offset_frames = 0;
		if (decoder->pending_audio != nullptr)
		{
			size_t first_audio_size = 0;
			if (!InspectAudioSample(decoder, decoder->pending_audio, &decoder->audio_format) ||
			    !GetAudioSampleSize(decoder, decoder->pending_audio, &first_audio_size))
			{
				ReleaseReader(decoder);
				return false;
			}
			decoder->has_audio = true;
			decoder->audio_eof = false;
		} else
		{
			const AVAssetReaderStatus status = [decoder->reader status];
			if (status == AVAssetReaderStatusFailed || status == AVAssetReaderStatusCancelled ||
			    status == AVAssetReaderStatusUnknown)
			{
				SetError(decoder, status == AVAssetReaderStatusFailed
				                      ? "AVFoundation failed while opening the audio track"
				                      : "AVFoundation asset reader stopped while opening the audio track",
				         [decoder->reader error]);
				ReleaseReader(decoder);
				return false;
			}
		}
	}

	decoder->active  = true;
	decoder->eof     = false;
	decoder->stopped = false;
	return true;
}

static bool RestartLoop(Decoder* decoder)
{
	if (!decoder->loop || decoder->stopped)
	{
		return false;
	}
	return StartReader(decoder);
}

static bool GetPresentationTimestamp(CMSampleBufferRef sample, int64_t* time_stamp)
{
	if (sample == nullptr || time_stamp == nullptr)
	{
		return false;
	}
	const CMTime presentation_time = CMSampleBufferGetPresentationTimeStamp(sample);
	if (CMTIME_IS_NUMERIC(presentation_time) && presentation_time.timescale > 0)
	{
		const CMTime milliseconds = CMTimeConvertScale(presentation_time, 1000, kCMTimeRoundingMethod_RoundHalfAwayFromZero);
		if (CMTIME_IS_NUMERIC(milliseconds))
		{
			*time_stamp = milliseconds.value;
			return true;
		}
	}
	return false;
}

static int64_t GetVideoSampleTimestamp(const Decoder* decoder, CMSampleBufferRef sample)
{
	int64_t     time_stamp = 0;
	const float frame_rate = decoder->video_format.frame_rate;
	if (!GetPresentationTimestamp(sample, &time_stamp) && std::isfinite(frame_rate) && frame_rate > 0.0f)
	{
		const double milliseconds =
		    (static_cast<double>(decoder->video_frame_index) * 1000.0) / static_cast<double>(frame_rate);
		if (!std::isfinite(milliseconds) ||
		    milliseconds >= static_cast<double>(std::numeric_limits<int64_t>::max()))
		{
			time_stamp = std::numeric_limits<int64_t>::max();
		} else
		{
			time_stamp = static_cast<int64_t>(milliseconds);
		}
	}
	return decoder->video_frame_index != 0 ? std::max(time_stamp, decoder->video_last_timestamp) : time_stamp;
}

static bool GetAudioSlicePresentationTimestamp(CMSampleBufferRef sample, size_t offset_frames, uint32_t sample_rate,
                                               int64_t* time_stamp)
{
	if (sample == nullptr || time_stamp == nullptr || sample_rate == 0 ||
	    sample_rate > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
	    offset_frames > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
	{
		return false;
	}

	const CMTime presentation_time = CMSampleBufferGetPresentationTimeStamp(sample);
	if (!CMTIME_IS_NUMERIC(presentation_time) || presentation_time.timescale <= 0)
	{
		return false;
	}

	const CMTime offset = CMTimeMake(static_cast<int64_t>(offset_frames), static_cast<int32_t>(sample_rate));
	const CMTime slice_time = CMTimeAdd(presentation_time, offset);
	if (!CMTIME_IS_NUMERIC(slice_time))
	{
		return false;
	}

	const CMTime milliseconds = CMTimeConvertScale(slice_time, 1000, kCMTimeRoundingMethod_RoundHalfAwayFromZero);
	if (!CMTIME_IS_NUMERIC(milliseconds))
	{
		return false;
	}

	*time_stamp = milliseconds.value;
	return true;
}

static uint64_t GetRoundedFrameDeltaMilliseconds(uint64_t frame_delta, uint32_t sample_rate)
{
	if (sample_rate == 0)
	{
		return 0;
	}

	const uint64_t rate                    = sample_rate;
	const uint64_t whole_seconds           = frame_delta / rate;
	const uint64_t remainder_frames        = frame_delta % rate;
	const uint64_t fractional_numerator    = remainder_frames * 1000u;
	uint64_t       fractional_milliseconds = fractional_numerator / rate;
	if ((fractional_numerator % rate) * 2u >= rate)
	{
		fractional_milliseconds++;
	}
	if (whole_seconds > std::numeric_limits<uint64_t>::max() / 1000u)
	{
		return std::numeric_limits<uint64_t>::max();
	}

	const uint64_t whole_milliseconds = whole_seconds * 1000u;
	return fractional_milliseconds > std::numeric_limits<uint64_t>::max() - whole_milliseconds
	           ? std::numeric_limits<uint64_t>::max()
	           : whole_milliseconds + fractional_milliseconds;
}

static int64_t AddPositiveMillisecondsSaturating(int64_t time_stamp, uint64_t delta)
{
	if (time_stamp >= 0)
	{
		const uint64_t headroom = static_cast<uint64_t>(std::numeric_limits<int64_t>::max() - time_stamp);
		return delta > headroom ? std::numeric_limits<int64_t>::max()
		                        : time_stamp + static_cast<int64_t>(delta);
	}

	const uint64_t magnitude = static_cast<uint64_t>(-(time_stamp + 1)) + 1u;
	if (delta < magnitude)
	{
		const uint64_t remaining = magnitude - delta;
		return remaining == (uint64_t {1} << 63u) ? std::numeric_limits<int64_t>::min()
		                                             : -static_cast<int64_t>(remaining);
	}
	const uint64_t positive = delta - magnitude;
	return positive > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
	           ? std::numeric_limits<int64_t>::max()
	           : static_cast<int64_t>(positive);
}

static Internal::AudioTimestampResolutionForTest ResolveAudioTimestamp(
    bool source_time_stamp_valid, int64_t source_time_stamp, bool anchor_valid, int64_t anchor_time_stamp,
    uint64_t anchor_sample_frame, uint64_t current_sample_frame, uint32_t sample_rate,
    bool previous_time_stamp_valid, int64_t previous_time_stamp)
{
	Internal::AudioTimestampResolutionForTest result {};
	result.anchor_time_stamp   = anchor_time_stamp;
	result.anchor_sample_frame = anchor_sample_frame;
	if (source_time_stamp_valid)
	{
		result.time_stamp          = source_time_stamp;
		result.update_anchor       = true;
		result.anchor_sample_frame = current_sample_frame;
	} else
	{
		const int64_t  fallback_anchor_time   = anchor_valid ? anchor_time_stamp : 0;
		const uint64_t fallback_anchor_frames = anchor_valid ? anchor_sample_frame : 0;
		const uint64_t frame_delta =
		    current_sample_frame >= fallback_anchor_frames ? current_sample_frame - fallback_anchor_frames : 0;
		result.time_stamp = AddPositiveMillisecondsSaturating(
		    fallback_anchor_time, GetRoundedFrameDeltaMilliseconds(frame_delta, sample_rate));
	}

	if (previous_time_stamp_valid)
	{
		result.time_stamp = std::max(result.time_stamp, previous_time_stamp);
	}
	if (result.update_anchor)
	{
		result.anchor_time_stamp = result.time_stamp;
	}
	return result;
}

static uint64_t ResolveVideoMasterPosition(bool has_audio, bool audio_eof,
                                           uint64_t wall_clock_position_ms,
                                           bool delivered_audio_end_valid,
                                           uint64_t delivered_audio_end_ms,
                                           bool pending_audio_time_stamp_valid,
                                           uint64_t pending_audio_time_stamp_ms)
{
	if (!has_audio || audio_eof)
	{
		return wall_clock_position_ms;
	}

	const uint64_t delivered_position = delivered_audio_end_valid ? delivered_audio_end_ms : 0;
	if (!pending_audio_time_stamp_valid || pending_audio_time_stamp_ms <= delivered_position)
	{
		return delivered_position;
	}

	// No PCM exists between the delivered boundary and the next known slice.
	// Let video traverse that lead-in/gap with wall time, but never cross into
	// PCM that the guest has not accepted.
	return std::max(delivered_position, std::min(wall_clock_position_ms, pending_audio_time_stamp_ms));
}

static Internal::AudioTimestampResolutionForTest GetAudioSampleTimestamp(const Decoder* decoder,
                                                                          CMSampleBufferRef sample)
{
	int64_t source_time_stamp = 0;
	const bool source_time_stamp_valid = GetAudioSlicePresentationTimestamp(
	    sample, decoder->pending_audio_offset_frames, decoder->audio_format.sample_rate, &source_time_stamp);
	return ResolveAudioTimestamp(source_time_stamp_valid, source_time_stamp,
	                             decoder->audio_timestamp_anchor_valid, decoder->audio_timestamp_anchor,
	                             decoder->audio_timestamp_anchor_frames, decoder->audio_sample_frames,
	                             decoder->audio_format.sample_rate, decoder->audio_frame_index != 0,
	                             decoder->audio_last_timestamp);
}

namespace Internal {

AudioTimestampResolutionForTest ResolveAudioTimestampForTest(
    bool source_time_stamp_valid, int64_t source_time_stamp, bool anchor_valid, int64_t anchor_time_stamp,
    uint64_t anchor_sample_frame, uint64_t current_sample_frame, uint32_t sample_rate,
    bool previous_time_stamp_valid, int64_t previous_time_stamp)
{
	return ResolveAudioTimestamp(source_time_stamp_valid, source_time_stamp, anchor_valid, anchor_time_stamp,
	                             anchor_sample_frame, current_sample_frame, sample_rate,
	                             previous_time_stamp_valid, previous_time_stamp);
}

uint64_t ResolveVideoMasterPositionForTest(bool has_audio, bool audio_eof,
                                           uint64_t wall_clock_position_ms,
                                           bool delivered_audio_end_valid,
                                           uint64_t delivered_audio_end_ms,
                                           bool pending_audio_time_stamp_valid,
                                           uint64_t pending_audio_time_stamp_ms)
{
	return ResolveVideoMasterPosition(has_audio, audio_eof, wall_clock_position_ms,
	                                  delivered_audio_end_valid, delivered_audio_end_ms,
	                                  pending_audio_time_stamp_valid, pending_audio_time_stamp_ms);
}

bool GetAudioReadStateForTest(const Decoder* decoder, AudioReadStateForTest* state)
{
	if (decoder == nullptr || state == nullptr)
	{
		return false;
	}
	std::lock_guard lock(decoder->mutex);
	state->pending_offset_frames    = decoder->pending_audio_offset_frames;
	state->consumed_sample_frames   = decoder->audio_sample_frames;
	state->time_stamp_anchor_valid  = decoder->audio_timestamp_anchor_valid;
	state->time_stamp_anchor        = decoder->audio_timestamp_anchor;
	state->time_stamp_anchor_frames = decoder->audio_timestamp_anchor_frames;
	state->playback_clock_started   = decoder->playback_clock_started;
	state->delivered_audio_end_valid = decoder->delivered_audio_end_valid;
	state->delivered_audio_end_ms    = decoder->delivered_audio_end_ms;
	return true;
}

#if defined(LUMEN_AV_SYNC_TELEMETRY)
bool GetAvSyncTelemetrySummaryForTest(const Decoder* decoder, AvSyncTelemetrySummaryForTest* summary)
{
	if (decoder == nullptr || summary == nullptr)
	{
		return false;
	}
	std::lock_guard lock(decoder->mutex);
	const auto* telemetry = decoder->av_sync_telemetry.get();
	if (telemetry == nullptr)
	{
		return false;
	}
	summary->session                 = telemetry->session;
	summary->capture_start_tick_ns   = telemetry->capture_start_tick_ns;
	summary->playback_clock_tick_ns  = telemetry->playback_clock_tick_ns;
	summary->playback_start_pts_ms   = telemetry->playback_start_pts_ms;
	summary->event_count             = telemetry->event_count;
	summary->event_overflow_dropped  = telemetry->event_overflow_dropped;
	summary->video_read_calls        = telemetry->video_read_calls;
	summary->video_delivered         = telemetry->video_delivered;
	summary->video_not_due           = telemetry->video_not_due;
	summary->video_catch_up_dropped  = telemetry->video_catch_up_dropped;
	summary->audio_read_calls        = telemetry->audio_read_calls;
	summary->audio_delivered         = telemetry->audio_delivered;
	summary->audio_not_due           = telemetry->audio_not_due;
	summary->nominal_video_fps_milli = telemetry->nominal_video_fps_milli;
	summary->audio_sample_rate       = telemetry->audio_sample_rate;
	summary->audio_channel_count     = telemetry->audio_channel_count;
	summary->playback_clock_started  = telemetry->playback_clock_started;
	summary->ended                   = telemetry->ended;
	summary->end_reason              = telemetry->end_reason;
	return true;
}

bool GetAvSyncTelemetryEventForTest(const Decoder* decoder, size_t index, AvSyncTelemetryEventForTest* event)
{
	if (decoder == nullptr || event == nullptr)
	{
		return false;
	}
	std::lock_guard lock(decoder->mutex);
	const auto* telemetry = decoder->av_sync_telemetry.get();
	if (telemetry == nullptr || index >= telemetry->event_count)
	{
		return false;
	}
	*event = telemetry->events[index];
	return true;
}
#endif

} // namespace Internal

static int64_t GetPlaybackStartTimestamp(const Decoder* decoder)
{
	int64_t start = 0;
	bool    found = false;
	if (decoder->pending_video != nullptr)
	{
		start = GetVideoSampleTimestamp(decoder, decoder->pending_video);
		found = true;
	}
	if (decoder->pending_audio != nullptr)
	{
		const int64_t audio_start = GetAudioSampleTimestamp(decoder, decoder->pending_audio).time_stamp;
		start = found ? std::min(start, audio_start) : audio_start;
		found = true;
	}
	return found ? start : 0;
}

static uint64_t GetTimestampDistance(int64_t time_stamp, int64_t origin)
{
	if (time_stamp <= origin)
	{
		return 0;
	}
	constexpr uint64_t sign_bit       = uint64_t {1} << 63u;
	const uint64_t     ordered_time   = static_cast<uint64_t>(time_stamp) ^ sign_bit;
	const uint64_t     ordered_origin = static_cast<uint64_t>(origin) ^ sign_bit;
	return ordered_time - ordered_origin;
}

static uint64_t NormalizeTimestamp(const Decoder* decoder, int64_t time_stamp)
{
	return GetTimestampDistance(time_stamp, decoder->playback_start_timestamp);
}

static std::chrono::steady_clock::time_point EnsurePlaybackClockStarted(Decoder* decoder)
{
	const auto now = std::chrono::steady_clock::now();
	if (!decoder->playback_clock_started)
	{
		decoder->playback_start           = now;
		decoder->playback_start_timestamp = GetPlaybackStartTimestamp(decoder);
		decoder->playback_clock_started   = true;
#if defined(LUMEN_AV_SYNC_TELEMETRY)
		AvSyncRecordClockStart(decoder, now, decoder->playback_start_timestamp);
#endif
	}
	return now;
}

static uint64_t GetPlaybackElapsedMilliseconds(const Decoder* decoder,
                                               std::chrono::steady_clock::time_point now)
{
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - decoder->playback_start).count();
	return elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0;
}

static bool IsSampleDue(Decoder* decoder, int64_t time_stamp)
{
	const auto now = EnsurePlaybackClockStarted(decoder);

	const uint64_t target_time = GetTimestampDistance(time_stamp, decoder->playback_start_timestamp);
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - decoder->playback_start).count();
	return elapsed >= 0 && static_cast<uint64_t>(elapsed) >= target_time;
}

static uint64_t GetVideoMasterPositionMilliseconds(Decoder* decoder)
{
	const auto     now = EnsurePlaybackClockStarted(decoder);
	const uint64_t wall_clock_position = GetPlaybackElapsedMilliseconds(decoder, now);
	bool           pending_audio_time_stamp_valid = false;
	uint64_t       pending_audio_time_stamp        = 0;
	if (decoder->has_audio && !decoder->audio_eof && decoder->pending_audio != nullptr)
	{
		pending_audio_time_stamp = NormalizeTimestamp(
		    decoder, GetAudioSampleTimestamp(decoder, decoder->pending_audio).time_stamp);
		pending_audio_time_stamp_valid = true;
	}

	return ResolveVideoMasterPosition(
	    decoder->has_audio, decoder->audio_eof, wall_clock_position,
	    decoder->delivered_audio_end_valid, decoder->delivered_audio_end_ms,
	    pending_audio_time_stamp_valid, pending_audio_time_stamp);
}

static bool IsVideoSampleDue(const Decoder* decoder, int64_t time_stamp, uint64_t master_position)
{
	const uint64_t target_time = GetTimestampDistance(time_stamp, decoder->playback_start_timestamp);
	return target_time <= master_position;
}

static bool CopyVideoSample(Decoder* decoder, CMSampleBufferRef sample, uint64_t time_stamp, void* destination,
                            size_t destination_size, VideoFrame* frame)
{
	CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
	if (image == nullptr)
	{
		SetError(decoder, "AVFoundation returned a video sample without a pixel buffer");
		return false;
	}

	auto* pixel_buffer = static_cast<CVPixelBufferRef>(image);
	const OSType pixel_format = CVPixelBufferGetPixelFormatType(pixel_buffer);
	if (pixel_format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
	    pixel_format != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
	{
		SetError(decoder, "AVFoundation changed to an unsupported pixel format");
		return false;
	}
	if (CVPixelBufferGetPlaneCount(pixel_buffer) != 2)
	{
		SetError(decoder, "AVFoundation returned an invalid NV12 plane layout");
		return false;
	}

	const size_t width         = CVPixelBufferGetWidth(pixel_buffer);
	const size_t height        = CVPixelBufferGetHeight(pixel_buffer);
	const size_t chroma_height = CVPixelBufferGetHeightOfPlane(pixel_buffer, 1);
	if (width != decoder->video_format.width || height != decoder->video_format.height ||
	    chroma_height != height / 2)
	{
		SetError(decoder, "AVFoundation changed video dimensions during playback");
		return false;
	}
	const size_t required_size = decoder->video_format.buffer_size;
	if (destination == nullptr || destination_size < required_size)
	{
		SetError(decoder, "AVFoundation destination buffer is too small");
		return false;
	}

	const CVReturn lock_result = CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
	if (lock_result != kCVReturnSuccess)
	{
		SetError(decoder, "Unable to lock AVFoundation pixel buffer");
		return false;
	}

	bool ok = true;
	auto* dst = static_cast<uint8_t*>(destination);
	for (size_t plane = 0; plane < 2 && ok; plane++)
	{
		auto* src = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, plane));
		const size_t src_pitch   = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, plane);
		const size_t plane_rows  = CVPixelBufferGetHeightOfPlane(pixel_buffer, plane);
		const size_t dst_offset  = plane == 0 ? 0 : width * height;
		if (src == nullptr || src_pitch < width || plane_rows != (plane == 0 ? height : chroma_height))
		{
			ok = false;
			break;
		}
		for (size_t row = 0; row < plane_rows; row++)
		{
			std::memcpy(dst + dst_offset + row * width, src + row * src_pitch, width);
		}
	}

	CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
	if (!ok)
	{
		SetError(decoder, "AVFoundation returned an invalid NV12 row layout");
		return false;
	}

	frame->time_stamp = time_stamp;
	frame->full_range = pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
	decoder->video_frame_index++;
	return true;
}

static bool CopyAudioSlice(Decoder* decoder, CMSampleBufferRef sample, uint64_t time_stamp, void* destination,
                           size_t destination_size, AudioFrame* frame)
{
	size_t full_size    = 0;
	size_t slice_frames = 0;
	size_t slice_size   = 0;
	if (!GetAudioSampleSize(decoder, sample, &full_size) ||
	    !GetPendingAudioSliceLayout(decoder, sample, &slice_frames, &slice_size))
	{
		return false;
	}
	if (destination == nullptr || destination_size < slice_size)
	{
		SetError(decoder, "AVFoundation audio destination buffer is too small");
		return false;
	}

	if (static_cast<uint64_t>(slice_frames) >
	    std::numeric_limits<uint64_t>::max() - decoder->audio_sample_frames)
	{
		SetError(decoder, "AVFoundation audio sample counter overflow");
		return false;
	}
	const size_t frame_bytes = static_cast<size_t>(decoder->audio_format.channel_count) * sizeof(int16_t);
	const size_t source_offset = decoder->pending_audio_offset_frames * frame_bytes;

	AudioBufferList  buffer_list {};
	CMBlockBufferRef retained_block     = nullptr;
	size_t           required_list_size = 0;
	const OSStatus status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
	    sample, &required_list_size, &buffer_list, sizeof(buffer_list), kCFAllocatorDefault, kCFAllocatorDefault,
	    kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &retained_block);
	const bool valid_buffer = status == noErr && buffer_list.mNumberBuffers == 1 &&
	                          buffer_list.mBuffers[0].mNumberChannels == decoder->audio_format.channel_count &&
	                          buffer_list.mBuffers[0].mDataByteSize == full_size &&
	                          buffer_list.mBuffers[0].mData != nullptr;
	if (valid_buffer)
	{
		const auto* source = static_cast<const uint8_t*>(buffer_list.mBuffers[0].mData);
		std::memcpy(destination, source + source_offset, slice_size);
	}
	if (retained_block != nullptr)
	{
		CFRelease(retained_block);
	}
	if (!valid_buffer)
	{
		SetError(decoder, "AVFoundation returned an invalid interleaved PCM buffer");
		return false;
	}

	frame->time_stamp = time_stamp;
	frame->data_size  = slice_size;
	decoder->audio_sample_frames += static_cast<uint64_t>(slice_frames);
	decoder->pending_audio_offset_frames += slice_frames;
	decoder->audio_frame_index++;
	return true;
}

static void AbortPlayback(Decoder* decoder)
{
	ReleaseReader(decoder);
	decoder->active    = false;
	decoder->eof       = true;
	decoder->video_eof = true;
	decoder->audio_eof = true;
}

enum class MediaTrack
{
	Video,
	Audio,
};

static bool HandleTrackEnd(Decoder* decoder, MediaTrack track)
{
	if (track == MediaTrack::Video)
	{
		decoder->video_eof = true;
	} else
	{
		decoder->audio_eof = true;
	}

	if (decoder->reader == nil)
	{
		SetError(decoder, "AVFoundation asset reader is not available");
		AbortPlayback(decoder);
		return false;
	}

	const AVAssetReaderStatus status = [decoder->reader status];
	if (status == AVAssetReaderStatusFailed)
	{
		SetError(decoder, track == MediaTrack::Video ? "AVFoundation failed while decoding video"
		                                             : "AVFoundation failed while decoding audio",
		         [decoder->reader error]);
		AbortPlayback(decoder);
		return false;
	}
	if (status == AVAssetReaderStatusUnknown || status == AVAssetReaderStatusCancelled)
	{
		SetError(decoder, "AVFoundation asset reader stopped unexpectedly", [decoder->reader error]);
		AbortPlayback(decoder);
		return false;
	}
	if (!decoder->video_eof || !decoder->audio_eof)
	{
		return false;
	}

	Trace("eof decoder=%p video_reads=%" PRIu64 " video_frames=%" PRIu64 " video_catch_up_dropped=%" PRIu64
	      " audio_reads=%" PRIu64
	      " audio_frames=%" PRIu64 " status=%ld",
	      static_cast<void*>(decoder), decoder->video_read_calls, decoder->video_frame_index,
	      decoder->video_catch_up_dropped, decoder->audio_read_calls, decoder->audio_frame_index,
	      static_cast<long>(status));
	if (RestartLoop(decoder))
	{
		Trace("loop decoder=%p", static_cast<void*>(decoder));
		return true;
	}
	if (decoder->loop && !decoder->stopped)
	{
		decoder->active = false;
		decoder->eof    = true;
		return false;
	}

	ReleaseReader(decoder);
	decoder->active = false;
	decoder->eof    = true;
	return false;
}

static bool EnsurePendingVideo(Decoder* decoder)
{
	if (decoder->video_eof || decoder->video_output == nil)
	{
		return false;
	}
	if (decoder->pending_video != nullptr)
	{
		return true;
	}

	decoder->pending_video = CopyNextSample(decoder->video_output);
	if (decoder->pending_video != nullptr)
	{
		return true;
	}
	return HandleTrackEnd(decoder, MediaTrack::Video) && decoder->active && !decoder->video_eof &&
	       decoder->pending_video != nullptr;
}

static uint64_t GetVideoCatchUpLagMilliseconds(const Decoder* decoder)
{
	const double frame_rate = static_cast<double>(decoder->video_format.frame_rate);
	if (!std::isfinite(frame_rate) || frame_rate <= 0.0)
	{
		return VIDEO_CATCH_UP_MIN_LAG_MS;
	}

	const double frame_period = std::ceil(1000.0 / frame_rate);
	if (!std::isfinite(frame_period) || frame_period >= static_cast<double>(UINT64_MAX))
	{
		return UINT64_MAX;
	}
	return std::max(VIDEO_CATCH_UP_MIN_LAG_MS, static_cast<uint64_t>(frame_period));
}

static bool IsVideoSampleTooLate(const Decoder* decoder, int64_t sample_timestamp,
                                 uint64_t master_position)
{
	const uint64_t target = GetTimestampDistance(sample_timestamp, decoder->playback_start_timestamp);
	return master_position > target && master_position - target > GetVideoCatchUpLagMilliseconds(decoder);
}

static bool CatchUpVideo(Decoder* decoder, uint64_t master_position)
{
	uint32_t dropped_this_read = 0;
	while (decoder->pending_video != nullptr)
	{
		const int64_t sample_timestamp = GetVideoSampleTimestamp(decoder, decoder->pending_video);
		if (!IsVideoSampleTooLate(decoder, sample_timestamp, master_position))
		{
			return true;
		}
		if (dropped_this_read == VIDEO_CATCH_UP_MAX_DROPS_PER_READ)
		{
			return false;
		}

		CMSampleBufferRef successor = CopyNextSample(decoder->video_output);
		if (successor == nullptr)
		{
			// Keep the last available frame even when it is late; there is no newer
			// image that could improve synchronization.
			return true;
		}

#if defined(LUMEN_AV_SYNC_TELEMETRY)
		const uint64_t dropped_timestamp = NormalizeTimestamp(decoder, sample_timestamp);
		const uint64_t dropped_index     = decoder->video_frame_index;
#endif
		ReleaseSample(&decoder->pending_video);
		decoder->pending_video          = successor;
		decoder->video_last_timestamp   = sample_timestamp;
		decoder->video_frame_index++;
		decoder->video_catch_up_dropped++;
		dropped_this_read++;
#if defined(LUMEN_AV_SYNC_TELEMETRY)
		AvSyncRecordVideoDrop(decoder, dropped_timestamp, dropped_index);
#endif
	}
	return true;
}

static bool EnsurePendingAudio(Decoder* decoder)
{
	if (!decoder->has_audio || decoder->audio_eof || decoder->audio_output == nil)
	{
		return false;
	}
	if (decoder->pending_audio != nullptr)
	{
		return true;
	}

	decoder->pending_audio = CopyNextSample(decoder->audio_output);
	decoder->pending_audio_offset_frames = 0;
	if (decoder->pending_audio != nullptr)
	{
		return true;
	}
	return HandleTrackEnd(decoder, MediaTrack::Audio) && decoder->active && decoder->has_audio &&
	       !decoder->audio_eof && decoder->pending_audio != nullptr;
}

Decoder* Create()
{
	return new Decoder;
}

void Destroy(Decoder* decoder)
{
	if (decoder == nullptr)
	{
		return;
	}
	@autoreleasepool
	{
		std::lock_guard lock(decoder->mutex);
#if defined(LUMEN_AV_SYNC_TELEMETRY)
		AvSyncEnd(decoder, AvSyncEndReason::Destroy);
		AvSyncFlush(decoder);
#endif
		ReleaseSource(decoder);
	}
	delete decoder;
}

bool Open(Decoder* decoder, const char* filename, VideoFormat* format, AudioFormat* audio_format)
{
	if (decoder == nullptr || filename == nullptr || format == nullptr)
	{
		return false;
	}

	@autoreleasepool
	{
		std::lock_guard lock(decoder->mutex);
#if defined(LUMEN_AV_SYNC_TELEMETRY)
		AvSyncEnd(decoder, AvSyncEndReason::Reopen);
		AvSyncFlush(decoder);
#endif
		ReleaseSource(decoder);
		decoder->last_error.clear();
		decoder->filename        = filename;
		decoder->audio_requested = audio_format != nullptr;

		NSString* path = [NSString stringWithUTF8String:filename];
		if (path == nil)
		{
			SetError(decoder, "Media path is not valid UTF-8");
			return false;
		}
		NSURL* url = [NSURL fileURLWithPath:path];
		decoder->asset = [[AVURLAsset alloc] initWithURL:url options:nil];
		if (decoder->asset == nil)
		{
			SetError(decoder, "Unable to open AVFoundation media asset");
			return false;
		}
		if (!StartReader(decoder))
		{
			[decoder->asset release];
			decoder->asset = nil;
			return false;
		}

#if defined(LUMEN_AV_SYNC_TELEMETRY)
		AvSyncStart(decoder);
#endif

		*format = decoder->video_format;
		if (audio_format != nullptr)
		{
			*audio_format = decoder->audio_format;
		}
		Trace("open decoder=%p file=%s size=%ux%u pitch=%u fps=%.3f audio_channels=%u audio_rate=%u",
		      static_cast<void*>(decoder), filename, format->width, format->height, format->pitch,
		      static_cast<double>(format->frame_rate), static_cast<unsigned>(decoder->audio_format.channel_count),
		      decoder->audio_format.sample_rate);
		return true;
	}
}

bool ReadFrame(Decoder* decoder, void* destination, size_t destination_size, VideoFrame* frame)
{
	if (decoder == nullptr || frame == nullptr)
	{
		return false;
	}

	@autoreleasepool
	{
		std::lock_guard lock(decoder->mutex);
#if defined(LUMEN_AV_SYNC_TELEMETRY)
		const uint64_t av_sync_begin = AvSyncBeginRead(decoder);
#endif
		if (!decoder->active || decoder->stopped || decoder->video_eof)
		{
#if defined(LUMEN_AV_SYNC_TELEMETRY)
			AvSyncRecordVideoRead(decoder, av_sync_begin, AvSyncResult::Inactive, UINT64_MAX,
			                          decoder->video_frame_index, 0);
#endif
			return false;
		}
		decoder->video_read_calls++;

		if (!EnsurePendingVideo(decoder))
		{
#if defined(LUMEN_AV_SYNC_TELEMETRY)
			AvSyncRecordVideoRead(decoder, av_sync_begin,
			                          decoder->video_eof ? AvSyncResult::EndOfStream : AvSyncResult::DecodeError,
			                          UINT64_MAX, decoder->video_frame_index, 0);
			if (decoder->eof)
			{
				AvSyncEnd(decoder, AvSyncEndReason::EndOfStream);
			}
#endif
			return false;
		}

		const uint64_t master_position = GetVideoMasterPositionMilliseconds(decoder);
		int64_t        sample_timestamp = GetVideoSampleTimestamp(decoder, decoder->pending_video);
		bool           sample_due       = IsVideoSampleDue(decoder, sample_timestamp, master_position);
		uint64_t       time_stamp       = NormalizeTimestamp(decoder, sample_timestamp);
		if (!sample_due)
		{
			decoder->video_not_due_reads++;
			if (decoder->video_not_due_reads == 1 || (decoder->video_not_due_reads % 1000000) == 0)
			{
				Trace("video_wait decoder=%p reads=%" PRIu64 " frames=%" PRIu64 " pts=%" PRIu64
				      " not_due=%" PRIu64,
				      static_cast<void*>(decoder), decoder->video_read_calls, decoder->video_frame_index, time_stamp,
				      decoder->video_not_due_reads);
			}
#if defined(LUMEN_AV_SYNC_TELEMETRY)
			AvSyncRecordVideoRead(decoder, av_sync_begin, AvSyncResult::NotDue, time_stamp,
			                          decoder->video_frame_index, 0);
#endif
			return false;
		}
		if (destination == nullptr || destination_size < decoder->video_format.buffer_size)
		{
			SetError(decoder, "AVFoundation video destination buffer is too small");
#if defined(LUMEN_AV_SYNC_TELEMETRY)
			AvSyncRecordVideoRead(decoder, av_sync_begin, AvSyncResult::InvalidDestination, time_stamp,
			                          decoder->video_frame_index, destination_size);
#endif
			return false;
		}
		if (!CatchUpVideo(decoder, master_position))
		{
			sample_timestamp = GetVideoSampleTimestamp(decoder, decoder->pending_video);
			time_stamp       = NormalizeTimestamp(decoder, sample_timestamp);
#if defined(LUMEN_AV_SYNC_TELEMETRY)
			AvSyncRecordVideoRead(decoder, av_sync_begin, AvSyncResult::CatchUpPending, time_stamp,
			                          decoder->video_frame_index, 0);
#endif
			return false;
		}

		sample_timestamp = GetVideoSampleTimestamp(decoder, decoder->pending_video);
		sample_due       = IsVideoSampleDue(decoder, sample_timestamp, master_position);
		time_stamp       = NormalizeTimestamp(decoder, sample_timestamp);
		if (!sample_due)
		{
			decoder->video_not_due_reads++;
#if defined(LUMEN_AV_SYNC_TELEMETRY)
			AvSyncRecordVideoRead(decoder, av_sync_begin, AvSyncResult::NotDue, time_stamp,
			                          decoder->video_frame_index, 0);
#endif
			return false;
		}

		const bool ok =
		    CopyVideoSample(decoder, decoder->pending_video, time_stamp, destination, destination_size, frame);
		if (!ok)
		{
			AbortPlayback(decoder);
#if defined(LUMEN_AV_SYNC_TELEMETRY)
			AvSyncRecordVideoRead(decoder, av_sync_begin, AvSyncResult::DecodeError, time_stamp,
			                          decoder->video_frame_index, destination_size);
#endif
			return false;
		}
		decoder->video_last_timestamp = sample_timestamp;
		ReleaseSample(&decoder->pending_video);
		if (TraceEnabled() && (decoder->video_frame_index == 1 || (decoder->video_frame_index % 60) == 0))
		{
			const auto* bytes = static_cast<const uint8_t*>(destination);
			uint8_t     minimum = std::numeric_limits<uint8_t>::max();
			uint8_t     maximum = 0;
			for (size_t index = 0; index < decoder->video_format.buffer_size; index++)
			{
				minimum = std::min(minimum, bytes[index]);
				maximum = std::max(maximum, bytes[index]);
			}
			Trace("video_frame decoder=%p reads=%" PRIu64 " frame=%" PRIu64 " pts=%" PRIu64
			      " dst=%p range=%u-%u",
			      static_cast<void*>(decoder), decoder->video_read_calls, decoder->video_frame_index,
			      frame->time_stamp, destination, static_cast<unsigned>(minimum), static_cast<unsigned>(maximum));
		}
#if defined(LUMEN_AV_SYNC_TELEMETRY)
		AvSyncRecordVideoRead(decoder, av_sync_begin, AvSyncResult::Delivered, frame->time_stamp,
		                          decoder->video_frame_index - 1, decoder->video_format.buffer_size);
#endif
		return true;
	}
}

size_t GetPendingAudioFrameSize(Decoder* decoder)
{
	if (decoder == nullptr)
	{
		return 0;
	}

	@autoreleasepool
	{
		std::lock_guard lock(decoder->mutex);
		if (!decoder->active || decoder->stopped || !decoder->has_audio || decoder->audio_eof)
		{
			return 0;
		}
#if defined(LUMEN_AV_SYNC_TELEMETRY)
		const uint64_t av_sync_begin = AvSyncBeginRead(decoder);
#endif
		if (!EnsurePendingAudio(decoder))
		{
#if defined(LUMEN_AV_SYNC_TELEMETRY)
			AvSyncRecordAudioRead(decoder, av_sync_begin,
			                          decoder->audio_eof ? AvSyncResult::EndOfStream : AvSyncResult::DecodeError,
			                          UINT64_MAX, decoder->audio_frame_index, 0, 0);
			if (decoder->eof)
			{
				AvSyncEnd(decoder, AvSyncEndReason::EndOfStream);
			}
#endif
			return 0;
		}

		size_t slice_frames = 0;
		size_t slice_size   = 0;
		if (!GetPendingAudioSliceLayout(decoder, decoder->pending_audio, &slice_frames, &slice_size))
		{
			AbortPlayback(decoder);
			return 0;
		}
		return slice_size;
	}
}

bool ReadAudioFrame(Decoder* decoder, void* destination, size_t destination_size, AudioFrame* frame)
{
	if (decoder == nullptr || frame == nullptr)
	{
		return false;
	}

	@autoreleasepool
	{
		std::lock_guard lock(decoder->mutex);
#if defined(LUMEN_AV_SYNC_TELEMETRY)
		const uint64_t av_sync_begin = AvSyncBeginRead(decoder);
#endif
		if (!decoder->active || decoder->stopped || !decoder->has_audio || decoder->audio_eof)
		{
#if defined(LUMEN_AV_SYNC_TELEMETRY)
			AvSyncRecordAudioRead(decoder, av_sync_begin, AvSyncResult::Inactive, UINT64_MAX,
			                          decoder->audio_frame_index, 0, 0);
#endif
			return false;
		}
		decoder->audio_read_calls++;

		if (!EnsurePendingAudio(decoder))
		{
#if defined(LUMEN_AV_SYNC_TELEMETRY)
			AvSyncRecordAudioRead(decoder, av_sync_begin,
			                          decoder->audio_eof ? AvSyncResult::EndOfStream : AvSyncResult::DecodeError,
			                          UINT64_MAX, decoder->audio_frame_index, 0, 0);
			if (decoder->eof)
			{
				AvSyncEnd(decoder, AvSyncEndReason::EndOfStream);
			}
#endif
			return false;
		}

		size_t slice_frames = 0;
		size_t required_size = 0;
		if (!GetPendingAudioSliceLayout(decoder, decoder->pending_audio, &slice_frames, &required_size))
		{
			AbortPlayback(decoder);
#if defined(LUMEN_AV_SYNC_TELEMETRY)
			AvSyncRecordAudioRead(decoder, av_sync_begin, AvSyncResult::DecodeError, UINT64_MAX,
			                          decoder->audio_frame_index, 0, 0);
#endif
			return false;
		}
		const size_t source_offset_frames = decoder->pending_audio_offset_frames;
		const CMItemCount total_frames = CMSampleBufferGetNumSamples(decoder->pending_audio);
		if (total_frames <= 0 || source_offset_frames > static_cast<uint64_t>(total_frames) ||
		    slice_frames > static_cast<uint64_t>(total_frames) - source_offset_frames)
		{
			SetError(decoder, "AVFoundation audio slice exceeds the pending PCM frame count");
			AbortPlayback(decoder);
#if defined(LUMEN_AV_SYNC_TELEMETRY)
			AvSyncRecordAudioRead(decoder, av_sync_begin, AvSyncResult::DecodeError, UINT64_MAX,
			                          decoder->audio_frame_index, slice_frames, required_size);
#endif
			return false;
		}
		const bool completes_sample =
		    source_offset_frames + slice_frames == static_cast<size_t>(total_frames);
		if (destination == nullptr || destination_size < required_size)
		{
			SetError(decoder, "AVFoundation audio destination buffer is too small");
#if defined(LUMEN_AV_SYNC_TELEMETRY)
			AvSyncRecordAudioRead(decoder, av_sync_begin, AvSyncResult::InvalidDestination, UINT64_MAX,
			                          decoder->audio_frame_index, slice_frames, destination_size);
#endif
			return false;
		}
		const Internal::AudioTimestampResolutionForTest timestamp =
		    GetAudioSampleTimestamp(decoder, decoder->pending_audio);
		const bool     sample_due = IsSampleDue(decoder, timestamp.time_stamp);
		const uint64_t time_stamp = NormalizeTimestamp(decoder, timestamp.time_stamp);
		if (!sample_due)
		{
			decoder->audio_not_due_reads++;
			if (decoder->audio_not_due_reads == 1 || (decoder->audio_not_due_reads % 1000000) == 0)
			{
				Trace("audio_wait decoder=%p reads=%" PRIu64 " frames=%" PRIu64 " pts=%" PRIu64
				      " not_due=%" PRIu64,
				      static_cast<void*>(decoder), decoder->audio_read_calls, decoder->audio_frame_index, time_stamp,
				      decoder->audio_not_due_reads);
			}
#if defined(LUMEN_AV_SYNC_TELEMETRY)
			AvSyncRecordAudioRead(decoder, av_sync_begin, AvSyncResult::NotDue, time_stamp,
			                          decoder->audio_frame_index, slice_frames, required_size);
#endif
			return false;
		}
		if (!CopyAudioSlice(decoder, decoder->pending_audio, time_stamp, destination, destination_size, frame))
		{
			AbortPlayback(decoder);
#if defined(LUMEN_AV_SYNC_TELEMETRY)
			AvSyncRecordAudioRead(decoder, av_sync_begin, AvSyncResult::DecodeError, time_stamp,
			                          decoder->audio_frame_index, slice_frames, required_size);
#endif
			return false;
		}
		const uint64_t slice_duration_ms =
		    GetRoundedFrameDeltaMilliseconds(slice_frames, decoder->audio_format.sample_rate);
		const uint64_t delivered_end = slice_duration_ms > std::numeric_limits<uint64_t>::max() - frame->time_stamp
		                                   ? std::numeric_limits<uint64_t>::max()
		                                   : frame->time_stamp + slice_duration_ms;
		decoder->delivered_audio_end_ms = decoder->delivered_audio_end_valid
		                                      ? std::max(decoder->delivered_audio_end_ms, delivered_end)
		                                      : delivered_end;
		decoder->delivered_audio_end_valid = true;

		decoder->audio_last_timestamp = timestamp.time_stamp;
		if (timestamp.update_anchor)
		{
			decoder->audio_timestamp_anchor_valid  = true;
			decoder->audio_timestamp_anchor        = timestamp.anchor_time_stamp;
			decoder->audio_timestamp_anchor_frames = timestamp.anchor_sample_frame;
		}
		if (completes_sample)
		{
			ReleaseSample(&decoder->pending_audio);
			decoder->pending_audio_offset_frames = 0;
		}
		if (TraceEnabled() && (decoder->audio_frame_index == 1 || (decoder->audio_frame_index % 100) == 0))
		{
			Trace("audio_frame decoder=%p reads=%" PRIu64 " frame=%" PRIu64 " pts=%" PRIu64
			      " bytes=%zu source_offset_frames=%zu dst=%p",
			      static_cast<void*>(decoder), decoder->audio_read_calls, decoder->audio_frame_index,
			      frame->time_stamp, frame->data_size, source_offset_frames, destination);
		}
#if defined(LUMEN_AV_SYNC_TELEMETRY)
		AvSyncRecordAudioRead(decoder, av_sync_begin, AvSyncResult::Delivered, frame->time_stamp,
		                          decoder->audio_frame_index - 1, slice_frames, frame->data_size);
#endif
		return true;
	}
}

void SetLooping(Decoder* decoder, bool loop)
{
	if (decoder == nullptr)
	{
		return;
	}
	std::lock_guard lock(decoder->mutex);
	decoder->loop = loop;
}

void Stop(Decoder* decoder)
{
	if (decoder == nullptr)
	{
		return;
	}
	@autoreleasepool
	{
		std::lock_guard lock(decoder->mutex);
#if defined(LUMEN_AV_SYNC_TELEMETRY)
		AvSyncEnd(decoder, AvSyncEndReason::Stop);
#endif
		Trace("stop decoder=%p video_reads=%" PRIu64 " video_frames=%" PRIu64 " video_catch_up_dropped=%" PRIu64
		      " audio_reads=%" PRIu64
		      " audio_frames=%" PRIu64 " active=%d",
		      static_cast<void*>(decoder), decoder->video_read_calls, decoder->video_frame_index,
		      decoder->video_catch_up_dropped, decoder->audio_read_calls, decoder->audio_frame_index,
		      decoder->active ? 1 : 0);
		ReleaseReader(decoder);
		decoder->active    = false;
		decoder->eof       = true;
		decoder->stopped   = true;
		decoder->video_eof = true;
		decoder->audio_eof = true;
#if defined(LUMEN_AV_SYNC_TELEMETRY)
		AvSyncFlush(decoder);
#endif
	}
}

bool IsActive(const Decoder* decoder)
{
	if (decoder == nullptr)
	{
		return false;
	}
	std::lock_guard lock(decoder->mutex);
	return decoder->active && !decoder->eof && !decoder->stopped;
}

std::string GetLastError(const Decoder* decoder)
{
	if (decoder == nullptr)
	{
		return "AVFoundation decoder is null";
	}
	std::lock_guard lock(decoder->mutex);
	return decoder->last_error;
}

} // namespace Lumen::Libs::Audio::AvPlayer::Mac
