#include "Emulator/AvPlayerAndroid.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace Lumen::Libs::Audio::AvPlayer::Android {

namespace {

using Clock = std::chrono::steady_clock;

// Android exposes several MediaFormat key variables only from API 28 even
// though get/setInt32 and the corresponding key strings work on API 21. Keep
// minSdk 26 free of those link-time symbol references.
constexpr char KEY_MIME[]          = "mime";
constexpr char KEY_WIDTH[]         = "width";
constexpr char KEY_HEIGHT[]        = "height";
constexpr char KEY_FRAME_RATE[]    = "frame-rate";
constexpr char KEY_COLOR_FORMAT[]  = "color-format";
constexpr char KEY_COLOR_RANGE[]   = "color-range";
constexpr char KEY_STRIDE[]        = "stride";
constexpr char KEY_SLICE_HEIGHT[]  = "slice-height";
constexpr char KEY_CROP_LEFT[]     = "crop-left";
constexpr char KEY_CROP_RIGHT[]    = "crop-right";
constexpr char KEY_CROP_TOP[]      = "crop-top";
constexpr char KEY_CROP_BOTTOM[]   = "crop-bottom";
constexpr char KEY_CHANNEL_COUNT[] = "channel-count";
constexpr char KEY_SAMPLE_RATE[]   = "sample-rate";
constexpr char KEY_PCM_ENCODING[]  = "pcm-encoding";

constexpr int32_t COLOR_FORMAT_YUV420_SEMIPLANAR = 21;
constexpr int32_t COLOR_RANGE_FULL                = 1;
constexpr int32_t PCM_ENCODING_SIGNED_16_BIT      = 2;
constexpr size_t  MAX_DEQUEUE_STEPS               = 32;
constexpr size_t  MAX_VIDEO_CATCH_UP_DROPS        = 64;

struct Track
{
	AMediaExtractor* extractor = nullptr;
	AMediaCodec*     codec     = nullptr;
	size_t            index     = 0;
	bool              present   = false;
	bool              started   = false;
	bool              input_eof = false;
	bool              output_eof = false;
	ssize_t           pending_output = -1;
	AMediaCodecBufferInfo pending_info {};
	size_t            pending_offset = 0;
};

struct VideoLayout
{
	uint32_t coded_width  = 0;
	uint32_t coded_height = 0;
	uint32_t stride       = 0;
	uint32_t slice_height = 0;
	uint32_t crop_left    = 0;
	uint32_t crop_top     = 0;
	uint32_t crop_width   = 0;
	uint32_t crop_height  = 0;
	bool     full_range   = false;
	bool     validated    = false;
};

bool HasPrefix(const char* value, const char* prefix)
{
	return value != nullptr &&
	       std::strncmp(value, prefix, std::strlen(prefix)) == 0;
}

bool GetPositiveInt32(AMediaFormat* format, const char* key, int32_t* value)
{
	return format != nullptr && value != nullptr &&
	       AMediaFormat_getInt32(format, key, value) && *value > 0;
}

bool ReadCrop(AMediaFormat* format, int32_t coded_width, int32_t coded_height,
	          int32_t* left, int32_t* top, int32_t* width, int32_t* height)
{
	if (format == nullptr || left == nullptr || top == nullptr || width == nullptr ||
	    height == nullptr || coded_width <= 0 || coded_height <= 0)
	{
		return false;
	}

	int32_t crop_left   = 0;
	int32_t crop_top    = 0;
	int32_t crop_right  = coded_width - 1;
	int32_t crop_bottom = coded_height - 1;
	const bool has_left   = AMediaFormat_getInt32(format, KEY_CROP_LEFT, &crop_left);
	const bool has_right  = AMediaFormat_getInt32(format, KEY_CROP_RIGHT, &crop_right);
	const bool has_top    = AMediaFormat_getInt32(format, KEY_CROP_TOP, &crop_top);
	const bool has_bottom = AMediaFormat_getInt32(format, KEY_CROP_BOTTOM, &crop_bottom);
	const bool any_crop   = has_left || has_right || has_top || has_bottom;
	const bool all_crop   = has_left && has_right && has_top && has_bottom;
	if (any_crop && !all_crop)
	{
		return false;
	}
	if (crop_left < 0 || crop_top < 0 || crop_right < crop_left ||
	    crop_bottom < crop_top || crop_right >= coded_width ||
	    crop_bottom >= coded_height)
	{
		return false;
	}
	const int64_t crop_width  = static_cast<int64_t>(crop_right) - crop_left + 1;
	const int64_t crop_height = static_cast<int64_t>(crop_bottom) - crop_top + 1;
	if (crop_width <= 0 || crop_height <= 0 ||
	    crop_width > std::numeric_limits<int32_t>::max() ||
	    crop_height > std::numeric_limits<int32_t>::max())
	{
		return false;
	}
	*left   = crop_left;
	*top    = crop_top;
	*width  = static_cast<int32_t>(crop_width);
	*height = static_cast<int32_t>(crop_height);
	return true;
}

bool ComputeNv12Size(uint32_t width, uint32_t height, size_t* size)
{
	if (size == nullptr || width == 0 || height == 0 || (width & 1u) != 0 ||
	    (height & 1u) != 0 ||
	    static_cast<size_t>(height) > std::numeric_limits<size_t>::max() / width)
	{
		return false;
	}
	const size_t luma = static_cast<size_t>(width) * height;
	if (luma > std::numeric_limits<size_t>::max() - luma / 2)
	{
		return false;
	}
	*size = luma + luma / 2;
	return true;
}

uint64_t GetElapsedUs(bool started, Clock::time_point start)
{
	if (!started)
	{
		return 0;
	}
	const auto elapsed =
	    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
	return elapsed <= 0 ? 0 : static_cast<uint64_t>(elapsed);
}

void ReleasePendingOutput(Track* track)
{
	if (track != nullptr && track->codec != nullptr && track->pending_output >= 0)
	{
		AMediaCodec_releaseOutputBuffer(
		    track->codec, static_cast<size_t>(track->pending_output), false);
	}
	if (track != nullptr)
	{
		track->pending_output = -1;
		track->pending_info   = {};
		track->pending_offset = 0;
	}
}

void ReleaseTrack(Track* track)
{
	if (track == nullptr)
	{
		return;
	}
	ReleasePendingOutput(track);
	if (track->codec != nullptr)
	{
		if (track->started)
		{
			AMediaCodec_stop(track->codec);
		}
		AMediaCodec_delete(track->codec);
	}
	if (track->extractor != nullptr)
	{
		AMediaExtractor_delete(track->extractor);
	}
	*track = {};
}

} // namespace

struct Decoder
{
	mutable std::mutex mutex;
	int                descriptor = -1;
	off64_t            file_size  = 0;
	Track              video;
	Track              audio;
	VideoFormat        video_format {};
	AudioFormat        audio_format {};
	VideoLayout        video_layout {};
	std::string        filename;
	std::string        last_error;
	int64_t            stream_origin_us = 0;
	bool               stream_origin_valid = false;
	Clock::time_point  playback_start {};
	bool               playback_clock_started = false;
	bool               audio_requested = false;
	bool               has_audio = false;
	bool               audio_output_validated = false;
	bool               delivered_audio_end_valid = false;
	uint64_t           delivered_audio_end_us = 0;
	bool               loop = false;
	bool               active = false;
	bool               stopped = true;
	bool               failed = false;
};

namespace {

void SetError(Decoder* decoder, const std::string& message)
{
	if (decoder == nullptr)
	{
		return;
	}
	decoder->last_error = message;
	decoder->failed     = true;
	decoder->active     = false;
}

void SetStatusError(Decoder* decoder, const char* operation, media_status_t status)
{
	SetError(decoder, std::string(operation) + " failed with media status " +
	                      std::to_string(static_cast<int>(status)));
}

void ReleaseSource(Decoder* decoder)
{
	if (decoder == nullptr)
	{
		return;
	}
	ReleaseTrack(&decoder->video);
	ReleaseTrack(&decoder->audio);
	if (decoder->descriptor >= 0)
	{
		close(decoder->descriptor);
	}
	decoder->descriptor = -1;
	decoder->file_size  = 0;
	decoder->video_format = {};
	decoder->audio_format = {};
	decoder->video_layout = {};
	decoder->stream_origin_us = 0;
	decoder->stream_origin_valid = false;
	decoder->playback_clock_started = false;
	decoder->audio_requested = false;
	decoder->has_audio = false;
	decoder->audio_output_validated = false;
	decoder->delivered_audio_end_valid = false;
	decoder->delivered_audio_end_us = 0;
	decoder->active  = false;
	decoder->stopped = true;
}

void IncludeStreamOrigin(Decoder* decoder, int64_t value)
{
	if (decoder == nullptr || value < 0)
	{
		return;
	}
	if (!decoder->stream_origin_valid || value < decoder->stream_origin_us)
	{
		decoder->stream_origin_us    = value;
		decoder->stream_origin_valid = true;
	}
}

AMediaFormat* SelectTrack(Decoder* decoder, Track* track, const char* prefix,
	                      bool required)
{
	track->extractor = AMediaExtractor_new();
	if (track->extractor == nullptr)
	{
		SetError(decoder, "AMediaExtractor_new returned null");
		return nullptr;
	}
	const media_status_t source_status = AMediaExtractor_setDataSourceFd(
	    track->extractor, decoder->descriptor, 0, decoder->file_size);
	if (source_status != AMEDIA_OK)
	{
		SetStatusError(decoder, "AMediaExtractor_setDataSourceFd", source_status);
		return nullptr;
	}

	const size_t count = AMediaExtractor_getTrackCount(track->extractor);
	for (size_t index = 0; index < count; index++)
	{
		AMediaFormat* format =
		    AMediaExtractor_getTrackFormat(track->extractor, index);
		if (format == nullptr)
		{
			continue;
		}
		const char* mime = nullptr;
		if (AMediaFormat_getString(format, KEY_MIME, &mime) &&
		    HasPrefix(mime, prefix))
		{
			const media_status_t select_status =
			    AMediaExtractor_selectTrack(track->extractor, index);
			if (select_status != AMEDIA_OK)
			{
				AMediaFormat_delete(format);
				SetStatusError(decoder, "AMediaExtractor_selectTrack", select_status);
				return nullptr;
			}
			track->index   = index;
			track->present = true;
			IncludeStreamOrigin(decoder,
			                    AMediaExtractor_getSampleTime(track->extractor));
			return format;
		}
		AMediaFormat_delete(format);
	}

	if (required)
	{
		SetError(decoder, std::string("media asset contains no ") + prefix + " track");
	} else
	{
		AMediaExtractor_delete(track->extractor);
		track->extractor = nullptr;
	}
	return nullptr;
}

bool ConfigureCodec(Decoder* decoder, Track* track, AMediaFormat* format,
	                const char* mime)
{
	track->codec = AMediaCodec_createDecoderByType(mime);
	if (track->codec == nullptr)
	{
		SetError(decoder, std::string("no Android decoder for ") + mime);
		return false;
	}
	media_status_t status =
	    AMediaCodec_configure(track->codec, format, nullptr, nullptr, 0);
	if (status != AMEDIA_OK)
	{
		SetStatusError(decoder, "AMediaCodec_configure", status);
		return false;
	}
	status = AMediaCodec_start(track->codec);
	if (status != AMEDIA_OK)
	{
		SetStatusError(decoder, "AMediaCodec_start", status);
		return false;
	}
	track->started = true;
	return true;
}

bool ConfigureVideo(Decoder* decoder, AMediaFormat* format)
{
	int32_t coded_width  = 0;
	int32_t coded_height = 0;
	if (!GetPositiveInt32(format, KEY_WIDTH, &coded_width) ||
	    !GetPositiveInt32(format, KEY_HEIGHT, &coded_height))
	{
		SetError(decoder, "video track has invalid dimensions");
		return false;
	}
	int32_t crop_left = 0;
	int32_t crop_top = 0;
	int32_t crop_width = 0;
	int32_t crop_height = 0;
	if (!ReadCrop(format, coded_width, coded_height, &crop_left, &crop_top,
	              &crop_width, &crop_height) || (crop_left & 1) != 0 ||
	    (crop_top & 1) != 0 || (crop_width & 1) != 0 ||
	    (crop_height & 1) != 0)
	{
		SetError(decoder, "video track has an unsupported YUV420 crop");
		return false;
	}

	size_t frame_size = 0;
	if (!ComputeNv12Size(static_cast<uint32_t>(crop_width),
	                     static_cast<uint32_t>(crop_height), &frame_size))
	{
		SetError(decoder, "video track NV12 size overflows");
		return false;
	}
	float frame_rate = 0.0f;
	if (!AMediaFormat_getFloat(format, KEY_FRAME_RATE, &frame_rate))
	{
		int32_t integral_rate = 0;
		if (AMediaFormat_getInt32(format, KEY_FRAME_RATE, &integral_rate))
		{
			frame_rate = static_cast<float>(integral_rate);
		}
	}
	if (!std::isfinite(frame_rate) || frame_rate < 0.0f)
	{
		frame_rate = 0.0f;
	}

	// OMX COLOR_FormatYUV420SemiPlanar is the explicit linear NV12 byte-buffer
	// contract. A decoder that substitutes flexible/vendor-tiled output is
	// rejected when its output format is observed.
	AMediaFormat_setInt32(format, KEY_COLOR_FORMAT,
	                      COLOR_FORMAT_YUV420_SEMIPLANAR);
	const char* mime = nullptr;
	if (!AMediaFormat_getString(format, KEY_MIME, &mime) || mime == nullptr ||
	    !ConfigureCodec(decoder, &decoder->video, format, mime))
	{
		if (!decoder->failed)
		{
			SetError(decoder, "video track MIME is missing");
		}
		return false;
	}

	decoder->video_format.width       = static_cast<uint32_t>(crop_width);
	decoder->video_format.height      = static_cast<uint32_t>(crop_height);
	decoder->video_format.pitch       = static_cast<uint32_t>(crop_width);
	decoder->video_format.buffer_size = frame_size;
	decoder->video_format.frame_rate  = frame_rate;
	decoder->video_format.full_range  = false;
	return true;
}

bool ConfigureAudio(Decoder* decoder, AMediaFormat* format)
{
	int32_t channel_count = 0;
	int32_t sample_rate   = 0;
	if (!GetPositiveInt32(format, KEY_CHANNEL_COUNT, &channel_count) ||
	    !GetPositiveInt32(format, KEY_SAMPLE_RATE, &sample_rate) ||
	    channel_count > std::numeric_limits<uint16_t>::max())
	{
		SetError(decoder, "audio track has an invalid PCM format");
		return false;
	}
	AMediaFormat_setInt32(format, KEY_PCM_ENCODING,
	                      PCM_ENCODING_SIGNED_16_BIT);
	const char* mime = nullptr;
	if (!AMediaFormat_getString(format, KEY_MIME, &mime) || mime == nullptr ||
	    !ConfigureCodec(decoder, &decoder->audio, format, mime))
	{
		if (!decoder->failed)
		{
			SetError(decoder, "audio track MIME is missing");
		}
		return false;
	}
	decoder->audio_format.channel_count =
	    static_cast<uint16_t>(channel_count);
	decoder->audio_format.sample_rate = static_cast<uint32_t>(sample_rate);
	decoder->has_audio = true;
	return true;
}

bool FeedInput(Decoder* decoder, Track* track)
{
	if (track->input_eof)
	{
		return true;
	}
	for (size_t step = 0; step < MAX_DEQUEUE_STEPS; step++)
	{
		const ssize_t index = AMediaCodec_dequeueInputBuffer(track->codec, 0);
		if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
		{
			return true;
		}
		if (index < 0)
		{
			SetError(decoder, "AMediaCodec_dequeueInputBuffer returned an invalid index");
			return false;
		}

		size_t capacity = 0;
		uint8_t* buffer = AMediaCodec_getInputBuffer(
		    track->codec, static_cast<size_t>(index), &capacity);
		if (buffer == nullptr || capacity == 0)
		{
			SetError(decoder, "AMediaCodec_getInputBuffer returned no storage");
			return false;
		}

		const int sample_track =
		    AMediaExtractor_getSampleTrackIndex(track->extractor);
		const int64_t sample_time =
		    AMediaExtractor_getSampleTime(track->extractor);
		if (sample_track < 0 || sample_time < 0)
		{
			const media_status_t status = AMediaCodec_queueInputBuffer(
			    track->codec, static_cast<size_t>(index), 0, 0, 0,
			    AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
			if (status != AMEDIA_OK)
			{
				SetStatusError(decoder, "AMediaCodec_queueInputBuffer(EOS)", status);
				return false;
			}
			track->input_eof = true;
			return true;
		}
		if (static_cast<size_t>(sample_track) != track->index)
		{
			SetError(decoder, "extractor returned a sample from an unselected track");
			return false;
		}
		if ((AMediaExtractor_getSampleFlags(track->extractor) &
		     AMEDIAEXTRACTOR_SAMPLE_FLAG_ENCRYPTED) != 0)
		{
			SetError(decoder, "encrypted media samples are unsupported");
			return false;
		}

		const ssize_t bytes = AMediaExtractor_readSampleData(
		    track->extractor, buffer, capacity);
		if (bytes <= 0 || static_cast<size_t>(bytes) > capacity)
		{
			SetError(decoder, "extractor sample does not fit the codec input buffer");
			return false;
		}
		const media_status_t status = AMediaCodec_queueInputBuffer(
		    track->codec, static_cast<size_t>(index), 0,
		    static_cast<size_t>(bytes), static_cast<uint64_t>(sample_time), 0);
		if (status != AMEDIA_OK)
		{
			SetStatusError(decoder, "AMediaCodec_queueInputBuffer", status);
			return false;
		}
		AMediaExtractor_advance(track->extractor);
	}
	return true;
}

bool UpdateVideoOutputFormat(Decoder* decoder, AMediaFormat* format)
{
	int32_t coded_width  = 0;
	int32_t coded_height = 0;
	int32_t color_format = 0;
	if (!GetPositiveInt32(format, KEY_WIDTH, &coded_width) ||
	    !GetPositiveInt32(format, KEY_HEIGHT, &coded_height) ||
	    !AMediaFormat_getInt32(format, KEY_COLOR_FORMAT, &color_format) ||
	    color_format != COLOR_FORMAT_YUV420_SEMIPLANAR)
	{
		SetError(decoder, "MediaCodec did not honor linear NV12 output format 21");
		return false;
	}
	int32_t stride       = coded_width;
	int32_t slice_height = coded_height;
	if (AMediaFormat_getInt32(format, KEY_STRIDE, &stride) && stride <= 0)
	{
		SetError(decoder, "MediaCodec returned an invalid video stride");
		return false;
	}
	if (AMediaFormat_getInt32(format, KEY_SLICE_HEIGHT, &slice_height) &&
	    slice_height <= 0)
	{
		SetError(decoder, "MediaCodec returned an invalid video slice height");
		return false;
	}
	int32_t crop_left = 0;
	int32_t crop_top = 0;
	int32_t crop_width = 0;
	int32_t crop_height = 0;
	if (!ReadCrop(format, coded_width, coded_height, &crop_left, &crop_top,
	              &crop_width, &crop_height) || crop_left < 0 || crop_top < 0 ||
	    (crop_left & 1) != 0 || (crop_top & 1) != 0 ||
	    (crop_width & 1) != 0 || (crop_height & 1) != 0 ||
	    stride < coded_width || slice_height < coded_height ||
	    static_cast<uint32_t>(crop_width) != decoder->video_format.width ||
	    static_cast<uint32_t>(crop_height) != decoder->video_format.height)
	{
		SetError(decoder, "MediaCodec returned an incompatible NV12 crop/layout");
		return false;
	}

	int32_t color_range = 0;
	const bool full_range =
	    AMediaFormat_getInt32(format, KEY_COLOR_RANGE, &color_range) &&
	    color_range == COLOR_RANGE_FULL;
	decoder->video_layout.coded_width  = static_cast<uint32_t>(coded_width);
	decoder->video_layout.coded_height = static_cast<uint32_t>(coded_height);
	decoder->video_layout.stride       = static_cast<uint32_t>(stride);
	decoder->video_layout.slice_height = static_cast<uint32_t>(slice_height);
	decoder->video_layout.crop_left    = static_cast<uint32_t>(crop_left);
	decoder->video_layout.crop_top     = static_cast<uint32_t>(crop_top);
	decoder->video_layout.crop_width   = static_cast<uint32_t>(crop_width);
	decoder->video_layout.crop_height  = static_cast<uint32_t>(crop_height);
	decoder->video_layout.full_range   = full_range;
	decoder->video_layout.validated    = true;
	decoder->video_format.full_range   = full_range;
	return true;
}

bool UpdateAudioOutputFormat(Decoder* decoder, AMediaFormat* format)
{
	int32_t channel_count = 0;
	int32_t sample_rate   = 0;
	int32_t pcm_encoding  = PCM_ENCODING_SIGNED_16_BIT;
	if (!GetPositiveInt32(format, KEY_CHANNEL_COUNT, &channel_count) ||
	    !GetPositiveInt32(format, KEY_SAMPLE_RATE, &sample_rate) ||
	    channel_count > std::numeric_limits<uint16_t>::max() ||
	    (AMediaFormat_getInt32(format, KEY_PCM_ENCODING, &pcm_encoding) &&
	     pcm_encoding != PCM_ENCODING_SIGNED_16_BIT) ||
	    static_cast<uint16_t>(channel_count) != decoder->audio_format.channel_count ||
	    static_cast<uint32_t>(sample_rate) != decoder->audio_format.sample_rate)
	{
		SetError(decoder, "MediaCodec returned an incompatible signed PCM16 format");
		return false;
	}
	decoder->audio_output_validated = true;
	return true;
}

bool EnsurePendingOutput(Decoder* decoder, Track* track, bool video)
{
	if (track->pending_output >= 0)
	{
		return true;
	}
	if (track->output_eof || !decoder->active)
	{
		return false;
	}

	for (size_t step = 0; step < MAX_DEQUEUE_STEPS; step++)
	{
		if (!FeedInput(decoder, track))
		{
			return false;
		}
		AMediaCodecBufferInfo info {};
		const ssize_t index =
		    AMediaCodec_dequeueOutputBuffer(track->codec, &info, 2000);
		if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
		{
			return false;
		}
		if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
		{
			AMediaFormat* output = AMediaCodec_getOutputFormat(track->codec);
			if (output == nullptr)
			{
				SetError(decoder, "AMediaCodec_getOutputFormat returned null");
				return false;
			}
			const bool ok = video ? UpdateVideoOutputFormat(decoder, output)
			                      : UpdateAudioOutputFormat(decoder, output);
			AMediaFormat_delete(output);
			if (!ok)
			{
				return false;
			}
			continue;
		}
		if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
		{
			continue;
		}
		if (index < 0)
		{
			SetError(decoder, "AMediaCodec_dequeueOutputBuffer returned an invalid status");
			return false;
		}
		if (info.offset < 0 || info.size < 0 || info.presentationTimeUs < 0)
		{
			AMediaCodec_releaseOutputBuffer(track->codec,
			                               static_cast<size_t>(index), false);
			SetError(decoder, "MediaCodec returned invalid output buffer metadata");
			return false;
		}
		const bool eos =
		    (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
		const bool codec_config =
		    (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0;
		if (info.size == 0 || codec_config)
		{
			AMediaCodec_releaseOutputBuffer(track->codec,
			                               static_cast<size_t>(index), false);
			if (eos)
			{
				track->output_eof = true;
				return false;
			}
			continue;
		}
		track->pending_output = index;
		track->pending_info   = info;
		track->pending_offset = 0;
		return true;
	}
	return false;
}

void StartPlaybackClock(Decoder* decoder)
{
	if (!decoder->playback_clock_started)
	{
		decoder->playback_start         = Clock::now();
		decoder->playback_clock_started = true;
	}
}

bool ResetTrackForLoop(Decoder* decoder, Track* track)
{
	ReleasePendingOutput(track);
	media_status_t status = AMediaCodec_flush(track->codec);
	if (status != AMEDIA_OK)
	{
		SetStatusError(decoder, "AMediaCodec_flush", status);
		return false;
	}
	status = AMediaExtractor_seekTo(track->extractor, 0,
	                                AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
	if (status != AMEDIA_OK)
	{
		SetStatusError(decoder, "AMediaExtractor_seekTo", status);
		return false;
	}
	track->input_eof  = false;
	track->output_eof = false;
	return true;
}

void FinishOrLoop(Decoder* decoder)
{
	const bool video_done = decoder->video.output_eof &&
	                        decoder->video.pending_output < 0;
	const bool audio_done = !decoder->has_audio ||
	                        (decoder->audio.output_eof &&
	                         decoder->audio.pending_output < 0);
	if (!video_done || !audio_done)
	{
		return;
	}
	if (!decoder->loop)
	{
		decoder->active = false;
		return;
	}
	if (!ResetTrackForLoop(decoder, &decoder->video) ||
	    (decoder->has_audio && !ResetTrackForLoop(decoder, &decoder->audio)))
	{
		return;
	}
	decoder->delivered_audio_end_valid = false;
	decoder->delivered_audio_end_us    = 0;
	decoder->playback_clock_started = false;
	decoder->active = true;
}

bool ReleaseCurrentOutput(Decoder* decoder, Track* track)
{
	const bool eos =
	    (track->pending_info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
	const media_status_t status = AMediaCodec_releaseOutputBuffer(
	    track->codec, static_cast<size_t>(track->pending_output), false);
	track->pending_output = -1;
	track->pending_info   = {};
	track->pending_offset = 0;
	if (status != AMEDIA_OK)
	{
		SetStatusError(decoder, "AMediaCodec_releaseOutputBuffer", status);
		return false;
	}
	if (eos)
	{
		track->output_eof = true;
	}
	FinishOrLoop(decoder);
	return !decoder->failed;
}

bool CopyVideoOutput(Decoder* decoder, void* destination,
	                 size_t destination_size)
{
	Track& track = decoder->video;
	if (!decoder->video_layout.validated)
	{
		AMediaFormat* format = AMediaCodec_getOutputFormat(track.codec);
		if (format == nullptr)
		{
			SetError(decoder, "video output format is unavailable");
			return false;
		}
		const bool valid = UpdateVideoOutputFormat(decoder, format);
		AMediaFormat_delete(format);
		if (!valid)
		{
			return false;
		}
	}
	if (destination == nullptr ||
	    destination_size < decoder->video_format.buffer_size)
	{
		SetError(decoder, "guest NV12 destination buffer is too small");
		return false;
	}

	size_t capacity = 0;
	uint8_t* buffer = AMediaCodec_getOutputBuffer(
	    track.codec, static_cast<size_t>(track.pending_output), &capacity);
	if (buffer == nullptr)
	{
		SetError(decoder, "MediaCodec video output is not CPU accessible");
		return false;
	}
	const size_t offset = static_cast<size_t>(track.pending_info.offset);
	const size_t bytes  = static_cast<size_t>(track.pending_info.size);
	if (offset > capacity || bytes > capacity - offset)
	{
		SetError(decoder, "MediaCodec video output range exceeds its buffer");
		return false;
	}

	const VideoLayout& layout = decoder->video_layout;
	const uint64_t luma_plane_size =
	    static_cast<uint64_t>(layout.stride) * layout.slice_height;
	const uint64_t last_y =
	    static_cast<uint64_t>(layout.crop_top + layout.crop_height - 1) * layout.stride +
	    layout.crop_left + layout.crop_width;
	const uint64_t last_uv = luma_plane_size +
	    static_cast<uint64_t>(layout.crop_top / 2 + layout.crop_height / 2 - 1) * layout.stride +
	    layout.crop_left + layout.crop_width;
	if (last_y > bytes || last_uv > bytes || last_uv > capacity - offset)
	{
		SetError(decoder, "MediaCodec NV12 planes exceed the output buffer");
		return false;
	}

	const uint8_t* source = buffer + offset;
	auto* target = static_cast<uint8_t*>(destination);
	for (uint32_t row = 0; row < layout.crop_height; row++)
	{
		std::memcpy(target + static_cast<size_t>(row) * layout.crop_width,
		            source + static_cast<size_t>(layout.crop_top + row) * layout.stride +
		                layout.crop_left,
		            layout.crop_width);
	}
	const size_t target_luma =
	    static_cast<size_t>(layout.crop_width) * layout.crop_height;
	const uint8_t* source_uv = source + static_cast<size_t>(luma_plane_size);
	uint8_t* target_uv = target + target_luma;
	for (uint32_t row = 0; row < layout.crop_height / 2; row++)
	{
		std::memcpy(target_uv + static_cast<size_t>(row) * layout.crop_width,
		            source_uv +
		                static_cast<size_t>(layout.crop_top / 2 + row) * layout.stride +
		                layout.crop_left,
		            layout.crop_width);
	}
	return true;
}

bool GetAudioSlice(Decoder* decoder, size_t* source_offset, size_t* size,
	               size_t* sample_frames, int64_t* presentation_time_us)
{
	if (source_offset == nullptr || size == nullptr || sample_frames == nullptr ||
	    presentation_time_us == nullptr)
	{
		return false;
	}
	Track& track = decoder->audio;
	if (!decoder->audio_output_validated)
	{
		AMediaFormat* format = AMediaCodec_getOutputFormat(track.codec);
		if (format == nullptr)
		{
			SetError(decoder, "audio output format is unavailable");
			return false;
		}
		const bool valid = UpdateAudioOutputFormat(decoder, format);
		AMediaFormat_delete(format);
		if (!valid)
		{
			return false;
		}
	}
	const size_t frame_bytes =
	    static_cast<size_t>(decoder->audio_format.channel_count) * sizeof(int16_t);
	const size_t total_size = static_cast<size_t>(track.pending_info.size);
	if (frame_bytes == 0 || total_size == 0 || total_size % frame_bytes != 0 ||
	    track.pending_offset >= total_size ||
	    (track.pending_offset % frame_bytes) != 0)
	{
		SetError(decoder, "MediaCodec returned a malformed interleaved PCM16 buffer");
		return false;
	}
	const size_t total_frames = total_size / frame_bytes;
	const size_t consumed_frames = track.pending_offset / frame_bytes;
	const size_t frames = std::min(total_frames - consumed_frames,
	                               AUDIO_FRAME_MAX_SAMPLE_FRAMES);
	if (frames == 0 || frames > std::numeric_limits<size_t>::max() / frame_bytes)
	{
		SetError(decoder, "MediaCodec PCM slice size overflows");
		return false;
	}
	const uint64_t delta_us =
	    static_cast<uint64_t>(consumed_frames) * 1000000 /
	    decoder->audio_format.sample_rate;
	if (delta_us > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
	    track.pending_info.presentationTimeUs >
	        std::numeric_limits<int64_t>::max() - static_cast<int64_t>(delta_us))
	{
		SetError(decoder, "MediaCodec PCM timestamp overflows");
		return false;
	}
	*source_offset = static_cast<size_t>(track.pending_info.offset) +
	                 track.pending_offset;
	*size = frames * frame_bytes;
	*sample_frames = frames;
	*presentation_time_us = track.pending_info.presentationTimeUs +
	                        static_cast<int64_t>(delta_us);
	return true;
}

uint64_t NormalizePresentationTimeUs(int64_t presentation_time_us,
	                                 int64_t stream_origin_us)
{
	return presentation_time_us <= stream_origin_us
	           ? 0
	           : static_cast<uint64_t>(presentation_time_us -
	                                   stream_origin_us);
}

bool GetVideoMasterPositionUs(Decoder* decoder, uint64_t* master_position_us)
{
	if (decoder == nullptr || master_position_us == nullptr)
	{
		return false;
	}

	if (decoder->has_audio && !decoder->audio.output_eof &&
	    decoder->audio.pending_output < 0)
	{
		// Prime one audio output so the video clock can distinguish a known
		// PCM-free lead-in/gap from PCM that the guest has not consumed.
		(void)EnsurePendingOutput(decoder, &decoder->audio, false);
		if (decoder->failed)
		{
			return false;
		}
	}
	const uint64_t wall_clock_position_us = GetElapsedUs(
	    decoder->playback_clock_started, decoder->playback_start);
	bool     pending_audio_time_stamp_valid = false;
	uint64_t pending_audio_time_stamp_us    = 0;
	if (decoder->has_audio && !decoder->audio.output_eof &&
	    decoder->audio.pending_output >= 0)
	{
		size_t  source_offset = 0;
		size_t  size          = 0;
		size_t  sample_frames = 0;
		int64_t presentation_time_us = 0;
		if (!GetAudioSlice(decoder, &source_offset, &size, &sample_frames,
		                   &presentation_time_us))
		{
			return false;
		}
		pending_audio_time_stamp_us = NormalizePresentationTimeUs(
		    presentation_time_us, decoder->stream_origin_us);
		pending_audio_time_stamp_valid = true;
	}

	*master_position_us = Internal::ResolveVideoMasterPosition(
	    decoder->has_audio, decoder->audio.output_eof,
	    wall_clock_position_us, decoder->delivered_audio_end_valid,
	    decoder->delivered_audio_end_us, pending_audio_time_stamp_valid,
	    pending_audio_time_stamp_us);
	return true;
}

void RecordDeliveredAudioEnd(Decoder* decoder, int64_t presentation_time_us,
	                         size_t sample_frames)
{
	if (decoder == nullptr || decoder->audio_format.sample_rate == 0)
	{
		return;
	}

	const uint64_t start_us = NormalizePresentationTimeUs(
	    presentation_time_us, decoder->stream_origin_us);
	const uint64_t numerator = static_cast<uint64_t>(sample_frames) * 1000000;
	const uint64_t duration_us =
	    (numerator + decoder->audio_format.sample_rate - 1) /
	    decoder->audio_format.sample_rate;
	const uint64_t end_us =
	    duration_us > std::numeric_limits<uint64_t>::max() - start_us
	        ? std::numeric_limits<uint64_t>::max()
	        : start_us + duration_us;
	decoder->delivered_audio_end_us = decoder->delivered_audio_end_valid
	                                      ? std::max(
	                                            decoder->delivered_audio_end_us,
	                                            end_us)
	                                      : end_us;
	decoder->delivered_audio_end_valid = true;
}

} // namespace

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
	{
		std::lock_guard lock(decoder->mutex);
		ReleaseSource(decoder);
	}
	delete decoder;
}

bool Open(Decoder* decoder, const char* filename, VideoFormat* format,
	      AudioFormat* audio_format)
{
	if (decoder == nullptr || filename == nullptr || format == nullptr)
	{
		return false;
	}
	std::lock_guard lock(decoder->mutex);
	ReleaseSource(decoder);
	decoder->last_error.clear();
	decoder->failed          = false;
	decoder->filename        = filename;
	decoder->audio_requested = audio_format != nullptr;

	decoder->descriptor = open(filename, O_RDONLY | O_CLOEXEC);
	if (decoder->descriptor < 0)
	{
		SetError(decoder, "unable to open Android media file");
		return false;
	}
	struct stat file_stat {};
	if (fstat(decoder->descriptor, &file_stat) != 0 || file_stat.st_size <= 0)
	{
		SetError(decoder, "Android media file has an invalid size");
		ReleaseSource(decoder);
		return false;
	}
	decoder->file_size = static_cast<off64_t>(file_stat.st_size);

	AMediaFormat* video_format =
	    SelectTrack(decoder, &decoder->video, "video/", true);
	if (video_format == nullptr || !ConfigureVideo(decoder, video_format))
	{
		if (video_format != nullptr)
		{
			AMediaFormat_delete(video_format);
		}
		const std::string error = decoder->last_error;
		ReleaseSource(decoder);
		decoder->last_error = error;
		decoder->failed = true;
		return false;
	}
	AMediaFormat_delete(video_format);

	if (decoder->audio_requested)
	{
		AMediaFormat* selected_audio =
		    SelectTrack(decoder, &decoder->audio, "audio/", false);
		if (selected_audio == nullptr && decoder->failed)
		{
			const std::string error = decoder->last_error;
			ReleaseSource(decoder);
			decoder->last_error = error;
			decoder->failed = true;
			return false;
		}
		if (selected_audio != nullptr)
		{
			if (!ConfigureAudio(decoder, selected_audio))
			{
				AMediaFormat_delete(selected_audio);
				const std::string error = decoder->last_error;
				ReleaseSource(decoder);
				decoder->last_error = error;
				decoder->failed = true;
				return false;
			}
			AMediaFormat_delete(selected_audio);
		}
	}
	if (!decoder->stream_origin_valid)
	{
		decoder->stream_origin_us    = 0;
		decoder->stream_origin_valid = true;
	}
	decoder->active  = true;
	decoder->stopped = false;
	*format = decoder->video_format;
	if (audio_format != nullptr)
	{
		*audio_format = decoder->audio_format;
	}
	return true;
}

bool ReadFrame(Decoder* decoder, void* destination, size_t destination_size,
	           VideoFrame* frame)
{
	if (decoder == nullptr || frame == nullptr)
	{
		return false;
	}
	std::lock_guard lock(decoder->mutex);
	if (!decoder->active || decoder->stopped || decoder->failed)
	{
		return false;
	}
	StartPlaybackClock(decoder);
	if (!EnsurePendingOutput(decoder, &decoder->video, true))
	{
		FinishOrLoop(decoder);
		return false;
	}
	uint64_t master_position_us = 0;
	if (!GetVideoMasterPositionUs(decoder, &master_position_us))
	{
		return false;
	}

	for (size_t dropped = 0; dropped <= MAX_VIDEO_CATCH_UP_DROPS; dropped++)
	{
		const int64_t pts = decoder->video.pending_info.presentationTimeUs;
		if (!Internal::IsPresentationDue(pts, decoder->stream_origin_us,
		                                 master_position_us))
		{
			return false;
		}
		const uint64_t frame_interval =
		    decoder->video_format.frame_rate > 0.0f
		        ? static_cast<uint64_t>(std::llround(
		              1000000.0 / decoder->video_format.frame_rate))
		        : 33333;
		if (Internal::ShouldDropVideoFrame(pts, decoder->stream_origin_us,
		                                   master_position_us, frame_interval))
		{
			if (dropped == MAX_VIDEO_CATCH_UP_DROPS)
			{
				// Yield the decoder mutex before continuing catch-up so audio
				// reads cannot be starved by one unbounded video call.
				return false;
			}
			if (!ReleaseCurrentOutput(decoder, &decoder->video))
			{
				return false;
			}
			if (!decoder->playback_clock_started)
			{
				// The EOS release completed a loop transition. Re-enter with a
				// fresh clock and master instead of comparing the new loop to the
				// previous loop's frozen master position.
				return false;
			}
			if (!EnsurePendingOutput(decoder, &decoder->video, true))
			{
				FinishOrLoop(decoder);
				return false;
			}
			continue;
		}
		if (!CopyVideoOutput(decoder, destination, destination_size))
		{
			return false;
		}
		frame->time_stamp = Internal::NormalizePresentationTimeMs(
		    pts, decoder->stream_origin_us);
		frame->full_range = decoder->video_layout.full_range;
		return ReleaseCurrentOutput(decoder, &decoder->video);
	}
	return false;
}

size_t GetPendingAudioFrameSize(Decoder* decoder)
{
	if (decoder == nullptr)
	{
		return 0;
	}
	std::lock_guard lock(decoder->mutex);
	if (!decoder->active || decoder->stopped || decoder->failed ||
	    !decoder->has_audio ||
	    !EnsurePendingOutput(decoder, &decoder->audio, false))
	{
		FinishOrLoop(decoder);
		return 0;
	}
	size_t source_offset = 0;
	size_t size = 0;
	size_t sample_frames = 0;
	int64_t pts = 0;
	return GetAudioSlice(decoder, &source_offset, &size, &sample_frames, &pts)
	           ? size
	           : 0;
}

bool ReadAudioFrame(Decoder* decoder, void* destination,
	                size_t destination_size, AudioFrame* frame)
{
	if (decoder == nullptr || frame == nullptr)
	{
		return false;
	}
	std::lock_guard lock(decoder->mutex);
	if (!decoder->active || decoder->stopped || decoder->failed ||
	    !decoder->has_audio ||
	    !EnsurePendingOutput(decoder, &decoder->audio, false))
	{
		FinishOrLoop(decoder);
		return false;
	}
	StartPlaybackClock(decoder);

	size_t source_offset = 0;
	size_t size = 0;
	size_t sample_frames = 0;
	int64_t pts = 0;
	if (!GetAudioSlice(decoder, &source_offset, &size, &sample_frames, &pts) ||
	    destination == nullptr || destination_size < size)
	{
		if (!decoder->failed)
		{
			SetError(decoder, "guest PCM16 destination buffer is too small");
		}
		return false;
	}
	const uint64_t elapsed = GetElapsedUs(
	    decoder->playback_clock_started, decoder->playback_start);
	if (!Internal::IsPresentationDue(pts, decoder->stream_origin_us, elapsed))
	{
		return false;
	}

	size_t capacity = 0;
	uint8_t* buffer = AMediaCodec_getOutputBuffer(
	    decoder->audio.codec,
	    static_cast<size_t>(decoder->audio.pending_output), &capacity);
	if (buffer == nullptr || source_offset > capacity ||
	    size > capacity - source_offset)
	{
		SetError(decoder, "MediaCodec PCM16 output range exceeds its buffer");
		return false;
	}
	std::memcpy(destination, buffer + source_offset, size);
	frame->time_stamp = Internal::NormalizePresentationTimeMs(
	    pts, decoder->stream_origin_us);
	frame->data_size = size;
	RecordDeliveredAudioEnd(decoder, pts, sample_frames);
	decoder->audio.pending_offset += size;
	if (decoder->audio.pending_offset ==
	    static_cast<size_t>(decoder->audio.pending_info.size))
	{
		if (!ReleaseCurrentOutput(decoder, &decoder->audio))
		{
			return false;
		}
	}
	return true;
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
	std::lock_guard lock(decoder->mutex);
	ReleaseSource(decoder);
}

bool IsActive(const Decoder* decoder)
{
	if (decoder == nullptr)
	{
		return false;
	}
	std::lock_guard lock(decoder->mutex);
	return decoder->active && !decoder->stopped && !decoder->failed;
}

std::string GetLastError(const Decoder* decoder)
{
	if (decoder == nullptr)
	{
		return "Android MediaCodec decoder is null";
	}
	std::lock_guard lock(decoder->mutex);
	return decoder->last_error;
}

} // namespace Lumen::Libs::Audio::AvPlayer::Android
