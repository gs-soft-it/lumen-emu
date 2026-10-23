#include "Emulator/AvPlayerAndroid.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>

namespace {

namespace Android = Lumen::Libs::Audio::AvPlayer::Android;

bool TestPlanarToNv12()
{
	std::array<uint8_t, 24> y {};
	std::array<uint8_t, 6> u {};
	std::array<uint8_t, 6> v {};
	for (size_t row = 0; row < 4; row++)
	{
		for (size_t column = 0; column < 4; column++)
		{
			y[row * 6 + column] = static_cast<uint8_t>(row * 10 + column);
		}
	}
	for (size_t row = 0; row < 2; row++)
	{
		for (size_t column = 0; column < 2; column++)
		{
			u[row * 3 + column] = static_cast<uint8_t>(100 + row * 10 + column);
			v[row * 3 + column] = static_cast<uint8_t>(150 + row * 10 + column);
		}
	}

	Android::Internal::Yuv420Image image {};
	image.width       = 4;
	image.height      = 4;
	image.crop_width  = 4;
	image.crop_height = 4;
	image.y = {y.data(), y.size(), 6, 1};
	image.u = {u.data(), u.size(), 3, 1};
	image.v = {v.data(), v.size(), 3, 1};
	std::array<uint8_t, 24> output {};
	const std::array<uint8_t, 24> expected = {
	    0, 1, 2, 3, 10, 11, 12, 13, 20, 21, 22, 23, 30, 31, 32, 33,
	    100, 150, 101, 151, 110, 160, 111, 161};
	return Android::Internal::ConvertYuv420ToNv12(
	           image, output.data(), output.size()) && output == expected;
}

bool TestInterleavedVuToNv12()
{
	std::array<uint8_t, 16> y {};
	for (size_t index = 0; index < y.size(); index++)
	{
		y[index] = static_cast<uint8_t>(index);
	}
	// The producer exposes V and U as separate pixel-stride-2 views over one
	// NV21-like allocation. Conversion must still emit canonical U,V NV12.
	std::array<uint8_t, 12> vu = {
	    150, 100, 151, 101, 0, 0,
	    160, 110, 161, 111, 0, 0};
	Android::Internal::Yuv420Image image {};
	image.width       = 4;
	image.height      = 4;
	image.crop_width  = 4;
	image.crop_height = 4;
	image.y = {y.data(), y.size(), 4, 1};
	image.u = {vu.data() + 1, vu.size() - 1, 6, 2};
	image.v = {vu.data(), vu.size(), 6, 2};
	std::array<uint8_t, 24> output {};
	const std::array<uint8_t, 8> expected_chroma = {
	    100, 150, 101, 151, 110, 160, 111, 161};
	return Android::Internal::ConvertYuv420ToNv12(
	           image, output.data(), output.size()) &&
	       std::equal(y.begin(), y.end(), output.begin()) &&
	       std::equal(expected_chroma.begin(), expected_chroma.end(),
	                  output.begin() + y.size());
}

bool TestCropAndAtomicFailure()
{
	std::array<uint8_t, 60> y {};
	std::array<uint8_t, 18> u {};
	std::array<uint8_t, 18> v {};
	for (size_t row = 0; row < 6; row++)
	{
		for (size_t column = 0; column < 8; column++)
		{
			y[row * 10 + column] = static_cast<uint8_t>(row * 10 + column);
		}
	}
	for (size_t row = 0; row < 3; row++)
	{
		for (size_t column = 0; column < 4; column++)
		{
			u[row * 6 + column] = static_cast<uint8_t>(100 + row * 10 + column);
			v[row * 6 + column] = static_cast<uint8_t>(150 + row * 10 + column);
		}
	}
	Android::Internal::Yuv420Image image {};
	image.width       = 8;
	image.height      = 6;
	image.crop_left   = 2;
	image.crop_top    = 2;
	image.crop_width  = 4;
	image.crop_height = 4;
	image.y = {y.data(), y.size(), 10, 1};
	image.u = {u.data(), u.size(), 6, 1};
	image.v = {v.data(), v.size(), 6, 1};
	std::array<uint8_t, 24> output {};
	const std::array<uint8_t, 24> expected = {
	    22, 23, 24, 25, 32, 33, 34, 35, 42, 43, 44, 45, 52, 53, 54, 55,
	    111, 161, 112, 162, 121, 171, 122, 172};
	if (!Android::Internal::ConvertYuv420ToNv12(
	        image, output.data(), output.size()) || output != expected)
	{
		return false;
	}

	output.fill(0x5a);
	image.u.data_size = 1;
	return !Android::Internal::ConvertYuv420ToNv12(
	            image, output.data(), output.size()) &&
	       std::all_of(output.begin(), output.end(),
	                   [](uint8_t value) { return value == 0x5a; });
}

bool TestPacing()
{
	using Android::Internal::IsPresentationDue;
	using Android::Internal::NormalizePresentationTimeMs;
	using Android::Internal::ResolveVideoMasterPosition;
	using Android::Internal::ShouldDropVideoFrame;
	if (!IsPresentationDue(1000, 1000, 0) ||
	       IsPresentationDue(2000, 1000, 999) ||
	       !IsPresentationDue(2000, 1000, 1000) ||
	       NormalizePresentationTimeMs(500, 1000) != 0 ||
	       NormalizePresentationTimeMs(2501000, 1000) != 2500 ||
	       ShouldDropVideoFrame(34000, 1000, 133000, 33333) ||
	       !ShouldDropVideoFrame(34000, 1000, 133001, 33333) ||
	       ShouldDropVideoFrame(200000, 1000, 100000, 33333))
	{
		return false;
	}

	struct MasterCase
	{
		bool     has_audio;
		bool     audio_eof;
		uint64_t wall_us;
		bool     delivered_valid;
		uint64_t delivered_us;
		bool     pending_valid;
		uint64_t pending_us;
		uint64_t expected_us;
	};
	static constexpr std::array<MasterCase, 11> CASES = {{
	    {false, false, 500000, false, 0, false, 0, 500000},
	    {true, true, 500000, true, 200000, false, 0, 500000},
	    {true, false, 240000, false, 0, true, 0, 0},
	    {true, false, 240000, false, 0, true, 1000000, 240000},
	    {true, false, 1200000, false, 0, true, 1000000, 1000000},
	    {true, false, 500000, true, 200000, true, 200000, 200000},
	    {true, false, 500000, true, 200000, true, 800000, 500000},
	    {true, false, 900000, true, 200000, true, 800000, 800000},
	    {true, false, 500000, false, 0, false, 0, 0},
	    {true, false, 500000, true, 200000, false, 0, 200000},
	    {true, false, 500000, true, 200000, true, 100000, 200000},
	}};
	for (const auto& test: CASES)
	{
		if (ResolveVideoMasterPosition(
		        test.has_audio, test.audio_eof, test.wall_us,
		        test.delivered_valid, test.delivered_us, test.pending_valid,
		        test.pending_us) != test.expected_us)
		{
			return false;
		}
	}
	return true;
}

} // namespace

int main()
{
	if (!TestPlanarToNv12() || !TestInterleavedVuToNv12() ||
	    !TestCropAndAtomicFailure() || !TestPacing())
	{
		std::fprintf(stderr, "AvPlayerAndroidLayoutTest: FAIL\n");
		return 1;
	}
	std::printf("AvPlayerAndroidLayoutTest: PASS\n");
	return 0;
}
