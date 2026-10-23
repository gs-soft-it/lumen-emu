#include "Emulator/AvPlayerAndroid.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace Lumen::Libs::Audio::AvPlayer::Android::Internal {

namespace {

bool PlaneContains(const YuvPlane& plane, uint32_t left, uint32_t top,
                   uint32_t width, uint32_t height)
{
	if (plane.data == nullptr || plane.row_stride == 0 ||
	    plane.pixel_stride == 0 || width == 0 || height == 0)
	{
		return false;
	}

	const uint64_t last_row = static_cast<uint64_t>(top) + height - 1;
	const uint64_t last_col = static_cast<uint64_t>(left) + width - 1;
	const uint64_t offset = last_row * plane.row_stride +
	                        last_col * plane.pixel_stride;
	return offset < plane.data_size;
}

} // namespace

bool ConvertYuv420ToNv12(const Yuv420Image& image, void* destination,
                         size_t destination_size)
{
	if (destination == nullptr || image.width == 0 || image.height == 0 ||
	    image.crop_width == 0 || image.crop_height == 0 ||
	    (image.crop_left & 1u) != 0 || (image.crop_top & 1u) != 0 ||
	    (image.crop_width & 1u) != 0 || (image.crop_height & 1u) != 0 ||
	    image.crop_left > image.width || image.crop_top > image.height ||
	    image.crop_width > image.width - image.crop_left ||
	    image.crop_height > image.height - image.crop_top)
	{
		return false;
	}

	const size_t width  = image.crop_width;
	const size_t height = image.crop_height;
	if (height > std::numeric_limits<size_t>::max() / width)
	{
		return false;
	}
	const size_t luma_size = width * height;
	if (luma_size > std::numeric_limits<size_t>::max() - luma_size / 2 ||
	    destination_size < luma_size + luma_size / 2)
	{
		return false;
	}

	const uint32_t chroma_left   = image.crop_left / 2;
	const uint32_t chroma_top    = image.crop_top / 2;
	const uint32_t chroma_width  = image.crop_width / 2;
	const uint32_t chroma_height = image.crop_height / 2;
	if (!PlaneContains(image.y, image.crop_left, image.crop_top,
	                   image.crop_width, image.crop_height) ||
	    !PlaneContains(image.u, chroma_left, chroma_top, chroma_width,
	                   chroma_height) ||
	    !PlaneContains(image.v, chroma_left, chroma_top, chroma_width,
	                   chroma_height))
	{
		return false;
	}

	auto* output = static_cast<uint8_t*>(destination);
	for (uint32_t row = 0; row < image.crop_height; row++)
	{
		const uint8_t* source = image.y.data +
		                        static_cast<size_t>(image.crop_top + row) * image.y.row_stride +
		                        static_cast<size_t>(image.crop_left) * image.y.pixel_stride;
		uint8_t* target = output + static_cast<size_t>(row) * image.crop_width;
		if (image.y.pixel_stride == 1)
		{
			std::memcpy(target, source, image.crop_width);
		} else
		{
			for (uint32_t column = 0; column < image.crop_width; column++)
			{
				target[column] = source[static_cast<size_t>(column) * image.y.pixel_stride];
			}
		}
	}

	uint8_t* chroma_output = output + luma_size;
	for (uint32_t row = 0; row < chroma_height; row++)
	{
		const uint8_t* source_u = image.u.data +
		                          static_cast<size_t>(chroma_top + row) * image.u.row_stride +
		                          static_cast<size_t>(chroma_left) * image.u.pixel_stride;
		const uint8_t* source_v = image.v.data +
		                          static_cast<size_t>(chroma_top + row) * image.v.row_stride +
		                          static_cast<size_t>(chroma_left) * image.v.pixel_stride;
		uint8_t* target = chroma_output + static_cast<size_t>(row) * image.crop_width;
		for (uint32_t column = 0; column < chroma_width; column++)
		{
			target[column * 2] =
			    source_u[static_cast<size_t>(column) * image.u.pixel_stride];
			target[column * 2 + 1] =
			    source_v[static_cast<size_t>(column) * image.v.pixel_stride];
		}
	}
	return true;
}

bool IsPresentationDue(int64_t presentation_time_us, int64_t stream_origin_us,
                       uint64_t elapsed_us)
{
	if (presentation_time_us <= stream_origin_us)
	{
		return true;
	}
	const uint64_t distance =
	    static_cast<uint64_t>(presentation_time_us - stream_origin_us);
	return distance <= elapsed_us;
}

uint64_t NormalizePresentationTimeMs(int64_t presentation_time_us,
	                                 int64_t stream_origin_us)
{
	if (presentation_time_us <= stream_origin_us)
	{
		return 0;
	}
	return static_cast<uint64_t>(presentation_time_us - stream_origin_us) / 1000;
}

uint64_t ResolveVideoMasterPosition(bool has_audio, bool audio_eof,
	                                uint64_t wall_clock_position_us,
	                                bool delivered_audio_end_valid,
	                                uint64_t delivered_audio_end_us,
	                                bool pending_audio_time_stamp_valid,
	                                uint64_t pending_audio_time_stamp_us)
{
	if (!has_audio || audio_eof)
	{
		return wall_clock_position_us;
	}

	const uint64_t delivered_position =
	    delivered_audio_end_valid ? delivered_audio_end_us : 0;
	if (!pending_audio_time_stamp_valid ||
	    pending_audio_time_stamp_us <= delivered_position)
	{
		return delivered_position;
	}

	return std::max(delivered_position,
	                std::min(wall_clock_position_us,
	                         pending_audio_time_stamp_us));
}

bool ShouldDropVideoFrame(int64_t presentation_time_us, int64_t stream_origin_us,
	                      uint64_t master_position_us,
	                      uint64_t frame_interval_us)
{
	if (!IsPresentationDue(presentation_time_us, stream_origin_us,
	                       master_position_us))
	{
		return false;
	}
	const uint64_t target = presentation_time_us <= stream_origin_us
	                            ? 0
	                            : static_cast<uint64_t>(presentation_time_us - stream_origin_us);
	constexpr uint64_t MINIMUM_CATCH_UP_LAG_US = 100000;
	const uint64_t tolerance =
	    std::max(MINIMUM_CATCH_UP_LAG_US, frame_interval_us);
	return master_position_us > target &&
	       master_position_us - target > tolerance;
}

} // namespace Lumen::Libs::Audio::AvPlayer::Android::Internal
