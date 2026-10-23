#ifndef EMULATOR_INCLUDE_EMULATOR_AVPLAYERANDROID_H_
#define EMULATOR_INCLUDE_EMULATOR_AVPLAYERANDROID_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace Lumen::Libs::Audio::AvPlayer::Android {

struct Decoder;

// Keep guest PCM buffers bounded even when MediaCodec coalesces several AAC
// access units into one output buffer.
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
	uint64_t time_stamp = 0;
	bool     full_range = false;
};

struct AudioFormat
{
	uint16_t channel_count = 0;
	uint32_t sample_rate   = 0;
};

struct AudioFrame
{
	uint64_t time_stamp = 0;
	size_t   data_size  = 0;
};

Decoder* Create();
void     Destroy(Decoder* decoder);

// The backend consumes an ordinary host file. The caller resolves the guest
// mount before opening it. Android uses an fd-based extractor data source and
// keeps that descriptor alive until Stop/Destroy.
bool Open(Decoder* decoder, const char* filename, VideoFormat* format,
          AudioFormat* audio_format = nullptr);
bool ReadFrame(Decoder* decoder, void* destination, size_t destination_size,
               VideoFrame* frame);

size_t GetPendingAudioFrameSize(Decoder* decoder);
bool   ReadAudioFrame(Decoder* decoder, void* destination,
                      size_t destination_size, AudioFrame* frame);

void SetLooping(Decoder* decoder, bool loop);
void Stop(Decoder* decoder);

bool        IsActive(const Decoder* decoder);
std::string GetLastError(const Decoder* decoder);

namespace Internal {

// Platform-independent description of one YUV_420_888 plane. AImage exposes
// the same row/pixel-stride contract; keeping conversion independent makes the
// bounds and NV12 ordering testable without MediaCodec or a device.
struct YuvPlane
{
	const uint8_t* data         = nullptr;
	size_t         data_size    = 0;
	uint32_t       row_stride   = 0;
	uint32_t       pixel_stride = 0;
};

struct Yuv420Image
{
	uint32_t width       = 0;
	uint32_t height      = 0;
	uint32_t crop_left   = 0;
	uint32_t crop_top    = 0;
	uint32_t crop_width  = 0;
	uint32_t crop_height = 0;
	YuvPlane y;
	YuvPlane u;
	YuvPlane v;
};

// Writes tightly packed NV12: crop_width bytes of Y per row followed by
// interleaved U/V rows. Odd or out-of-bounds 4:2:0 crops fail without writing.
bool ConvertYuv420ToNv12(const Yuv420Image& image, void* destination,
                         size_t destination_size);

bool     IsPresentationDue(int64_t presentation_time_us,
                           int64_t stream_origin_us,
                           uint64_t elapsed_us);
uint64_t NormalizePresentationTimeMs(int64_t presentation_time_us,
                                     int64_t stream_origin_us);

// Video follows PCM that the guest has actually accepted. A known lead-in or
// gap may advance with wall time up to the next PCM timestamp; an unknown gap
// remains at the last delivered PCM boundary. Video-only playback and the
// tail after audio EOF continue to use wall time.
uint64_t ResolveVideoMasterPosition(bool has_audio, bool audio_eof,
                                    uint64_t wall_clock_position_us,
                                    bool delivered_audio_end_valid,
                                    uint64_t delivered_audio_end_us,
                                    bool pending_audio_time_stamp_valid,
                                    uint64_t pending_audio_time_stamp_us);
bool     ShouldDropVideoFrame(int64_t presentation_time_us,
                              int64_t stream_origin_us,
                              uint64_t master_position_us,
                              uint64_t frame_interval_us);

} // namespace Internal

} // namespace Lumen::Libs::Audio::AvPlayer::Android

#endif // EMULATOR_INCLUDE_EMULATOR_AVPLAYERANDROID_H_
