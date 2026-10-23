#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/PngDec.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__APPLE__)
#include <unistd.h>
#endif

namespace {

namespace Png = Lumen::Libs::PngDec;

// Original tiny fixtures: filter zero on each row and stored DEFLATE blocks.
// Generated from the pixel values below with Python's standard struct/zlib
// modules; no external image, test fixture, or implementation was copied.
constexpr std::array<uint8_t, 86> RGBA_PNG {{
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00,
    0x1d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x01, 0x01, 0x12, 0x00, 0xed, 0xff,
    0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0x80, 0x00, 0x00, 0x00,
    0xff, 0x40, 0xff, 0xff, 0xff, 0x00, 0x40, 0x18, 0x07, 0xba, 0x2a, 0x86,
    0xf3, 0x71, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42,
    0x60, 0x82,
}};

constexpr std::array<uint8_t, 82> RGB_PNG {{
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x02, 0x00, 0x00, 0x00, 0xfd, 0xd4, 0x9a, 0x73, 0x00, 0x00, 0x00,
    0x19, 0x49, 0x44, 0x41, 0x54, 0x78, 0x01, 0x01, 0x0e, 0x00, 0xf1, 0xff,
    0x00, 0xfe, 0x11, 0x03, 0x04, 0xfd, 0x13, 0x00, 0x07, 0x1f, 0xfc, 0xff,
    0xff, 0xff, 0x22, 0x35, 0x06, 0x46, 0x00, 0x75, 0x10, 0x27, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
}};

constexpr std::array<uint8_t, 102> RGBA16_PNG {{
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x10, 0x06, 0x00, 0x00, 0x00, 0x22, 0x26, 0xd1, 0x67, 0x00, 0x00, 0x00,
    0x2d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x01, 0x01, 0x22, 0x00, 0xdd, 0xff,
    0x00, 0x12, 0x34, 0xab, 0xcd, 0xfe, 0xdc, 0x98, 0x76, 0xff, 0x00, 0x00,
    0x80, 0x45, 0xab, 0x80, 0xff, 0x00, 0x01, 0x02, 0x23, 0x00, 0x67, 0x01,
    0xff, 0xff, 0xff, 0xff, 0xfe, 0xfe, 0x00, 0x01, 0x00, 0x00, 0x0c, 0xb4,
    0x0f, 0x1c, 0xe9, 0xcf, 0x0c, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
    0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
}};

constexpr std::array<uint8_t, 106> PALETTE_PNG {{
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x03, 0x00, 0x00, 0x00, 0x45, 0x68, 0xfd, 0x16, 0x00, 0x00, 0x00,
    0x06, 0x50, 0x4c, 0x54, 0x45, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0xd2,
    0x87, 0xef, 0x71, 0x00, 0x00, 0x00, 0x02, 0x74, 0x52, 0x4e, 0x53, 0x7f,
    0xff, 0xf3, 0x36, 0x47, 0x8c, 0x00, 0x00, 0x00, 0x11, 0x49, 0x44, 0x41,
    0x54, 0x78, 0x01, 0x01, 0x06, 0x00, 0xf9, 0xff, 0x00, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x00, 0x0c, 0x00, 0x03, 0x48, 0xbb, 0x62, 0x3b, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
}};

constexpr std::array<uint8_t, 77> RECTANGULAR_PNG {{
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0xf4, 0x22, 0x7f, 0x8a, 0x00, 0x00, 0x00,
    0x14, 0x49, 0x44, 0x41, 0x54, 0x78, 0x01, 0x01, 0x09, 0x00, 0xf6, 0xff,
    0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0x80, 0x10, 0x79, 0x03,
    0x7e, 0x36, 0xa0, 0x68, 0x60, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e,
    0x44, 0xae, 0x42, 0x60, 0x82,
}};

constexpr std::array<uint8_t, 16> RGBA_PIXELS {{
    255, 0, 0, 255, 0, 255, 0, 128,
    0, 0, 255, 64, 255, 255, 255, 0,
}};
constexpr std::array<uint8_t, 16> BGRA_PIXELS {{
    0, 0, 255, 255, 0, 255, 0, 128,
    255, 0, 0, 64, 255, 255, 255, 0,
}};
constexpr std::array<uint8_t, 16> RGBA16_AS_8_PIXELS {{
    0x12, 0xab, 0xfe, 0x98, 0xff, 0x00, 0x45, 0x80,
    0x01, 0x23, 0x67, 0xff, 0xff, 0xfe, 0x00, 0x00,
}};
constexpr std::array<uint8_t, 16> PALETTE_PIXELS {{
    255, 0, 0, 127, 0, 255, 0, 255,
    0, 255, 0, 255, 255, 0, 0, 127,
}};

constexpr uint8_t OUTPUT_CANARY = 0xa5;
constexpr size_t  GUARD_SIZE    = 16;
using Output = std::array<uint8_t, GUARD_SIZE + 24 + GUARD_SIZE>;

class Workspace final
{
public:
	explicit Workspace(uint32_t size): m_size(size), m_words((size + 7) / 8 + 2, CANARY) {}

	void* Data() { return m_words.data() + 1; }
	uint32_t Size() const { return m_size; }
	bool GuardsIntact() const { return m_words.front() == CANARY && m_words.back() == CANARY; }

private:
	static constexpr uint64_t CANARY = 0x2e3ba971c40d5f86ULL;
	uint32_t                 m_size;
	std::vector<uint64_t>     m_words;
};

bool Check(bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "PngDecTest: %s\n", message);
	}
	return condition;
}

bool TestParseMetadata()
{
	constexpr uint64_t canary = 0xa13cb47df0169e52ULL;
	struct GuardedInfo
	{
		uint64_t       before = canary;
		Png::ImageInfo info {};
		uint64_t       after = canary;
	};
	GuardedInfo output;
	Png::ParseInput input {RGBA_PNG.data(), static_cast<uint32_t>(RGBA_PNG.size()), 0};
	if (!Check(Png::ParseHeader(&input, &output.info) == 0, "RGBA header was rejected") ||
	    !Check(output.info.width == 2 && output.info.height == 2 && output.info.color == 19 &&
	           output.info.depth == 8 && output.info.flags == 0, "RGBA metadata differs from the encoded header") ||
	    !Check(output.before == canary && output.after == canary, "parse wrote outside image information"))
	{
		return false;
	}
	input = {RGB_PNG.data(), static_cast<uint32_t>(RGB_PNG.size()), 0};
	return Check(Png::ParseHeader(&input, &output.info) == 0, "RGB header was rejected") &&
	       Check(output.info.width == 2 && output.info.height == 2 && output.info.color == 3 &&
	             output.info.depth == 8 && output.info.flags == 0, "RGB metadata differs from the encoded header") &&
	       Check(output.before == canary && output.after == canary, "RGB parse wrote outside image information");
}

bool TestMalformedHeaders()
{
	Png::ImageInfo info {};
	for (uint32_t size = 0; size < 29; ++size)
	{
		Png::ParseInput input {RGBA_PNG.data(), size, 0};
		if (!Check(Png::ParseHeader(&input, &info) < 0, "incomplete PNG signature or IHDR was accepted"))
		{
			return false;
		}
	}
	auto invalid = RGBA_PNG;
	invalid[0] = 0;
	Png::ParseInput input {invalid.data(), static_cast<uint32_t>(invalid.size()), 0};
	if (!Check(Png::ParseHeader(&input, &info) < 0, "invalid PNG signature was accepted"))
	{
		return false;
	}
	input.data = RGBA_PNG.data();
	Png::ParseInput absent {nullptr, UINT32_MAX, 0};
	return Check(Png::ParseHeader(nullptr, &info) == Png::InvalidParam, "null parse input was not rejected") &&
	       Check(Png::ParseHeader(&input, nullptr) == Png::InvalidAddress, "null image information was not rejected") &&
	       Check(Png::ParseHeader(&absent, &info) == Png::InvalidAddress, "null data must precede oversized PNG data");
}

bool CheckPixels(const Output& output, uint32_t pitch, const std::array<uint8_t, 16>& pixels, uint32_t height = 2)
{
	for (size_t index = 0; index < output.size(); ++index)
	{
		uint8_t expected = OUTPUT_CANARY;
		for (size_t row = 0; row < height; ++row)
		{
			const size_t begin = GUARD_SIZE + row * pitch;
			if (index >= begin && index < begin + 8)
			{
				expected = pixels[row * 8 + index - begin];
			}
		}
		if (output[index] != expected)
		{
			std::fprintf(stderr, "PngDecTest: output byte %zu is %u, expected %u (pitch %u)\n",
			             index, output[index], expected, pitch);
			return false;
		}
	}
	return true;
}

template<size_t Size>
bool DecodeMatches(Png::Handle handle, const std::array<uint8_t, Size>& image,
                   uint16_t format, uint16_t alpha, uint32_t requested_pitch,
                   const std::array<uint8_t, 16>& pixels, uint16_t color, uint16_t depth, uint32_t flags, uint32_t height = 2)
{
	Output output;
	output.fill(OUTPUT_CANARY);
	const uint32_t pitch = requested_pitch == 0 ? 8 : requested_pitch;
	Png::DecodeInput input {image.data(), output.data() + GUARD_SIZE,
	                        static_cast<uint32_t>(image.size()), pitch * height, format, alpha, requested_pitch};
	Png::ImageInfo info {};
	return Check(Png::Decode(handle, &input, &info) == static_cast<int32_t>(0x00020000 | height), "decode did not return packed dimensions") &&
	       Check(info.width == 2 && info.height == height && info.color == color && info.depth == depth && info.flags == flags,
	             "decode image information differs from the source") && CheckPixels(output, pitch, pixels, height);
}

bool TestDecodePixels(Png::Handle handle)
{
	for (uint16_t format = 0; format < 2; ++format)
	{
		for (uint32_t requested_pitch: {0u, 12u})
		{
			if (!DecodeMatches(handle, RGBA_PNG, format, 0xffff, requested_pitch,
			                   format == 0 ? RGBA_PIXELS : BGRA_PIXELS, 19, 8, 0))
			{
				return false;
			}
		}
	}
	return DecodeMatches(handle, RECTANGULAR_PNG, 0, 255, 0, RGBA_PIXELS, 19, 8, 0, 1);
}

bool TestDecodeAliasedInput(Png::Handle handle)
{
	constexpr size_t output_offset = GUARD_SIZE + offsetof(Png::DecodeInput, format);
	constexpr size_t arena_size = output_offset + RGBA_PIXELS.size() + GUARD_SIZE;
	static_assert(GUARD_SIZE % alignof(Png::DecodeInput) == 0);

	for (uint16_t format = 0; format < 2; ++format)
	{
		alignas(Png::DecodeInput) unsigned char arena[arena_size];
		std::fill_n(arena, arena_size, OUTPUT_CANARY);
		auto* input = ::new (static_cast<void*>(arena + GUARD_SIZE)) Png::DecodeInput {
		    RGBA_PNG.data(), arena + output_offset, static_cast<uint32_t>(RGBA_PNG.size()),
		    static_cast<uint32_t>(RGBA_PIXELS.size()), format, 255, 0};
		std::array<uint8_t, arena_size> expected {};
		std::copy_n(arena, expected.size(), expected.begin());
		const auto& pixels = format == 0 ? RGBA_PIXELS : BGRA_PIXELS;
		std::copy(pixels.begin(), pixels.end(), expected.begin() + output_offset);

		// The aligned byte arena provides storage for a live DecodeInput. The first
		// output row replaces its format/alpha/pitch bytes; subsequent rows must
		// still use the settings supplied when Decode was entered.
		Png::ImageInfo info {};
		if (!Check(Png::Decode(handle, input, &info) == 0x00020002,
		           "decode rejected output overlapping its input settings") ||
		    !Check(info.width == 2 && info.height == 2 && info.color == 19 && info.depth == 8 && info.flags == 0,
		           "aliased decode returned incorrect source metadata"))
		{
			return false;
		}
		for (size_t index = 0; index < expected.size(); ++index)
		{
			if (arena[index] != expected[index])
			{
				std::fprintf(stderr, "PngDecTest: aliased format %u arena byte %zu is %u, expected %u\n",
				             format, index, arena[index], expected[index]);
				return false;
			}
		}
	}
	return true;
}

bool TestAlphaAndDepth(Png::Handle handle)
{
	for (uint8_t alpha: {0u, 37u, 255u})
	{
		const std::array<uint8_t, 16> pixels {{
		    254, 17, 3, alpha, 4, 253, 19, alpha,
		    7, 31, 252, alpha, 255, 255, 255, alpha,
		}};
		if (!DecodeMatches(handle, RGB_PNG, 0, alpha, 12, pixels, 3, 8, 0))
		{
			return false;
		}
	}
	constexpr std::array<uint8_t, 16> rgb_as_bgra {{
	    3, 17, 254, 255, 19, 253, 4, 255,
	    252, 31, 7, 255, 255, 255, 255, 255,
	}};
	Png::ImageInfo info {};
	Png::ParseInput palette {PALETTE_PNG.data(), static_cast<uint32_t>(PALETTE_PNG.size()), 0};
	return DecodeMatches(handle, RGB_PNG, 1, 255, 0, rgb_as_bgra, 3, 8, 0) &&
	       Check(Png::ParseHeader(&palette, &info) == 0 && info.color == 4 && info.depth == 8 && info.flags == 2,
	             "palette transparency metadata was not reported") &&
	       DecodeMatches(handle, PALETTE_PNG, 0, 0xffff, 12, PALETTE_PIXELS, 4, 8, 2) &&
	       DecodeMatches(handle, RGBA16_PNG, 0, 0xffff, 12, RGBA16_AS_8_PIXELS, 19, 16, 0);
}

bool CheckRejected(int32_t actual, int32_t expected, const Output& output, const char* message)
{
	return Check(expected == 0 ? actual < 0 : actual == expected, message) &&
	       Check(std::all_of(output.begin(), output.end(), [](uint8_t value) { return value == OUTPUT_CANARY; }),
	             "rejected decode changed destination bytes");
}

bool TestDecodeErrors(Png::Handle handle)
{
	Output output;
	output.fill(OUTPUT_CANARY);
	Png::ImageInfo info {};
	Png::DecodeInput input {RGBA_PNG.data(), output.data() + GUARD_SIZE,
	                        static_cast<uint32_t>(RGBA_PNG.size()), 16, 0, 255, 0};
	input.output_size = 15;
	if (!CheckRejected(Png::Decode(handle, &input, &info), Png::InvalidSize, output, "short output buffer was not rejected"))
	{
		return false;
	}
	input.output_size = 16;
	input.pitch = 7;
	if (!CheckRejected(Png::Decode(handle, &input, &info), Png::InvalidSize, output, "short output pitch was not rejected"))
	{
		return false;
	}
	input.pitch = 12;
	input.output_size = 19;
	if (!CheckRejected(Png::Decode(handle, &input, &info), Png::InvalidSize, output, "pitched destination was too small but accepted"))
	{
		return false;
	}
	input.pitch = UINT32_MAX;
	input.output_size = 16;
	if (!CheckRejected(Png::Decode(handle, &input, &info), Png::InvalidSize, output, "overflowing output geometry was accepted"))
	{
		return false;
	}
	input.pitch = 0;
	input.format = 2;
	if (!CheckRejected(Png::Decode(handle, &input, &info), Png::InvalidParam, output, "unknown pixel format was not rejected"))
	{
		return false;
	}
	input.format = 0;
	if (!CheckRejected(Png::Decode(nullptr, &input, &info), Png::InvalidHandle, output, "null handle was not rejected") ||
	    !CheckRejected(Png::Decode(reinterpret_cast<Png::Handle>(uintptr_t {1}), &input, &info), Png::InvalidHandle,
	                   output, "unregistered handle was not rejected") ||
	    !CheckRejected(Png::Decode(handle, nullptr, &info), Png::InvalidParam, output, "null decode input was not rejected"))
	{
		return false;
	}
	input.data = nullptr;
	input.size = UINT32_MAX;
	if (!CheckRejected(Png::Decode(handle, &input, &info), Png::InvalidAddress, output, "null encoded data was not rejected"))
	{
		return false;
	}
	input.data = RGBA_PNG.data();
	input.size = static_cast<uint32_t>(RGBA_PNG.size());
	input.output = nullptr;
	if (!CheckRejected(Png::Decode(handle, &input, &info), Png::InvalidAddress, output, "null destination was not rejected"))
	{
		return false;
	}
	input.output = output.data() + GUARD_SIZE;
	input.data = RGB_PNG.data();
	input.size = static_cast<uint32_t>(RGB_PNG.size());
	input.alpha = 256;
	if (!CheckRejected(Png::Decode(handle, &input, &info), Png::Unsupported, output, "unverified wide fill alpha was accepted"))
	{
		return false;
	}
	input.alpha = 255;
	input.size = static_cast<uint32_t>(RGBA_PNG.size());
	auto corrupt = RGBA_PNG;
	corrupt[47] ^= 1; // Invalid stored-DEFLATE length complement.
	input.data = corrupt.data();
	if (!CheckRejected(Png::Decode(handle, &input, &info), 0, output, "corrupt compressed data was accepted"))
	{
		return false;
	}
	input.data = RGBA_PNG.data();
	input.size = 32;
	if (!CheckRejected(Png::Decode(handle, &input, &info), 0, output, "truncated image was accepted"))
	{
		return false;
	}
	input.size = static_cast<uint32_t>(RGBA_PNG.size());
	return Check(Png::Decode(handle, &input, nullptr) == 0x00020002, "decode required optional image information") &&
	       CheckPixels(output, 8, RGBA_PIXELS);
}

bool TestCreateAndLifecycle()
{
	Png::ResetSessionState();
	Png::CreateInput create {sizeof(Png::CreateInput), 0, 2};
	const int32_t bytes = Png::QueryMemorySize(&create);
	if (!Check(bytes > 0, "valid creation parameters have no workspace size") ||
	    !Check(Png::QueryMemorySize(nullptr) == Png::InvalidParam, "null memory-query input was not rejected"))
	{
		return false;
	}
	auto invalid = create;
	invalid.size = 0;
	if (!Check(Png::QueryMemorySize(&invalid) == Png::InvalidParam, "invalid creation size marker was accepted"))
	{
		return false;
	}
	invalid = create;
	invalid.attributes = 2;
	if (!Check(Png::QueryMemorySize(&invalid) == Png::InvalidParam, "unknown creation attribute was accepted"))
	{
		return false;
	}
	invalid = create;
	invalid.max_width = 0;
	if (!Check(Png::QueryMemorySize(&invalid) == Png::InvalidSize, "zero maximum width was accepted"))
	{
		return false;
	}
	Workspace workspace(static_cast<uint32_t>(bytes));
	Png::Handle handle = nullptr;
	if (!Check(Png::Create(nullptr, workspace.Data(), workspace.Size(), &handle) == Png::InvalidParam,
	           "null creation input was not rejected") ||
	    !Check(Png::Create(&create, nullptr, workspace.Size(), &handle) == Png::InvalidAddress,
	           "null workspace was not rejected") ||
	    !Check(Png::Create(&create, workspace.Data(), workspace.Size(), nullptr) == Png::InvalidAddress,
	           "null handle slot was not rejected") ||
	    !Check(Png::Create(&create, workspace.Data(), workspace.Size() - 1, &handle) == Png::InvalidWork,
	           "workspace smaller than the queried requirement was accepted") ||
	    !Check(Png::Create(&create, static_cast<uint8_t*>(workspace.Data()) + 1, workspace.Size(), &handle) == Png::InvalidWork,
	           "unaligned workspace was accepted"))
	{
		return false;
	}
	if (!Check(Png::Create(&create, workspace.Data(), workspace.Size(), &handle) == 0 && handle != nullptr,
	           "create did not provide a handle with the queried workspace"))
	{
		return false;
	}
	Png::Handle duplicate = nullptr;
	if (!Check(Png::Create(&create, workspace.Data(), workspace.Size(), &duplicate) == Png::InvalidWork,
	           "workspace used by a live decoder was accepted again") ||
	    !TestDecodePixels(handle) || !TestDecodeAliasedInput(handle) || !TestAlphaAndDepth(handle) || !TestDecodeErrors(handle) ||
	    !Check(workspace.GuardsIntact(), "decoder wrote outside caller workspace") ||
	    !Check(Png::Delete(handle) == 0, "delete failed") ||
	    !Check(Png::Delete(handle) == Png::InvalidHandle, "repeated delete was accepted"))
	{
		return false;
	}

	Output output;
	output.fill(OUTPUT_CANARY);
	Png::ImageInfo info {};
	Png::DecodeInput decode {RGBA_PNG.data(), output.data() + GUARD_SIZE,
	                         static_cast<uint32_t>(RGBA_PNG.size()), 16, 0, 255, 0};
	if (!CheckRejected(Png::Decode(handle, &decode, &info), Png::InvalidHandle, output, "deleted handle was accepted"))
	{
		return false;
	}
	Workspace other_workspace(static_cast<uint32_t>(bytes));
	Png::Handle other = nullptr;
	if (!Check(Png::Create(&create, workspace.Data(), workspace.Size(), &handle) == 0,
	           "workspace could not be reused after delete") ||
	    !Check(Png::Create(&create, other_workspace.Data(), other_workspace.Size(), &other) == 0,
	           "second independent decoder could not be created"))
	{
		return false;
	}
	Png::ResetSessionState();
	if (!Check(Png::Delete(handle) == Png::InvalidHandle && Png::Delete(other) == Png::InvalidHandle,
	           "session reset retained a decoder handle") ||
	    !CheckRejected(Png::Decode(handle, &decode, &info), Png::InvalidHandle, output, "reset handle was accepted") ||
	    !Check(Png::Create(&create, workspace.Data(), workspace.Size(), &handle) == 0,
	           "decoder could not be recreated after session reset") ||
	    !TestDecodePixels(handle) || !Check(Png::Delete(handle) == 0, "recreated decoder could not be deleted"))
	{
		return false;
	}
	return Check(workspace.GuardsIntact() && other_workspace.GuardsIntact(), "lifecycle wrote outside workspace");
}

bool TestDepthAttributeAndWidth()
{
	Png::CreateInput create {sizeof(Png::CreateInput), 1, 2};
	int32_t bytes = Png::QueryMemorySize(&create);
	if (!Check(bytes > 0, "16-bit input attribute was rejected"))
	{
		return false;
	}
	Workspace workspace(static_cast<uint32_t>(bytes));
	Png::Handle handle = nullptr;
	if (!Check(Png::Create(&create, workspace.Data(), workspace.Size(), &handle) == 0, "create with attribute one failed") ||
	    !DecodeMatches(handle, RGBA16_PNG, 0, 0xffff, 12, RGBA16_AS_8_PIXELS, 19, 16, 0) ||
	    !Check(Png::Delete(handle) == 0, "16-bit decoder delete failed") ||
	    !Check(workspace.GuardsIntact(), "16-bit decoder wrote outside workspace"))
	{
		return false;
	}
	create.max_width = 1;
	bytes = Png::QueryMemorySize(&create);
	if (!Check(bytes > 0, "positive maximum width was rejected"))
	{
		return false;
	}
	Workspace narrow_workspace(static_cast<uint32_t>(bytes));
	if (!Check(Png::Create(&create, narrow_workspace.Data(), narrow_workspace.Size(), &handle) == 0,
	           "narrow decoder creation failed"))
	{
		return false;
	}
	Output output;
	output.fill(OUTPUT_CANARY);
	Png::ImageInfo info {};
	Png::DecodeInput input {RGBA_PNG.data(), output.data() + GUARD_SIZE,
	                        static_cast<uint32_t>(RGBA_PNG.size()), 16, 0, 255, 0};
	return CheckRejected(Png::Decode(handle, &input, &info), Png::InvalidSize, output, "maximum decoder width was ignored") &&
	       Check(Png::Delete(handle) == 0, "narrow decoder delete failed") &&
	       Check(narrow_workspace.GuardsIntact(), "narrow decoder wrote outside workspace");
}

#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__APPLE__)
bool TestGuestMemoryBorrows()
{
	namespace VM = Lumen::Core::VirtualMemory;
	Png::ResetSessionState();
	const long host_page = sysconf(_SC_PAGESIZE);
	if (!Check(host_page > 0, "borrow host page size")) return false;
	const uint64_t page = static_cast<uint64_t>(host_page);
	const uint64_t mapping = VM::Alloc(0, page * 3, VM::Mode::ReadWrite);
	const uint64_t tiny = VM::Alloc(0, 1, VM::Mode::ReadWrite);
	if (!Check(mapping != 0 && tiny != 0, "borrow mappings")) return false;
	struct Cleanup
	{
		uint64_t mapping;
		uint64_t tiny;
		~Cleanup() { VM::Free(mapping); VM::Free(tiny); }
	} cleanup {mapping, tiny};
	auto* bytes = reinterpret_cast<uint8_t*>(mapping);
	std::memset(bytes, OUTPUT_CANARY, static_cast<size_t>(page * 3));
	std::memcpy(bytes + page, RGBA_PNG.data(), RGBA_PNG.size());
	if (!Check(VM::Protect(mapping + page, page, VM::Mode::Read) &&
	           VM::Protect(mapping + page * 2, page, VM::Mode::NoAccess), "borrow protections")) return false;
	Png::CreateInput create {sizeof(create), 0, 2};
	Workspace workspace(16);
	Png::Handle handle {};
	if (!Check(Png::Create(&create, workspace.Data(), workspace.Size(), &handle) == 0, "borrow decoder creation")) return false;
	Output output;
	output.fill(OUTPUT_CANARY);
	Png::ImageInfo info {};
	Png::DecodeInput input {bytes + page, output.data() + GUARD_SIZE,
	                        static_cast<uint32_t>(RGBA_PNG.size()), 16, 0, 255, 0};
	bool ok = Check(Png::Decode(handle, &input, &info) == 0x00020002 && CheckPixels(output, 8, RGBA_PIXELS),
	                "read-only guest PNG decode");
	output.fill(OUTPUT_CANARY);
	info = {};
	const auto unchanged = [&]
	{
		return std::all_of(output.begin(), output.end(), [](uint8_t value) { return value == OUTPUT_CANARY; }) &&
		       info.width == 0 && info.height == 0 && info.color == 0 && info.depth == 0 && info.flags == 0;
	};
	Png::ParseInput parse {RGBA_PNG.data(), static_cast<uint32_t>(RGBA_PNG.size()), 0};
	for (const void* invalid: {static_cast<void*>(bytes + page * 2), static_cast<void*>(bytes + page * 2 - 4),
	                          reinterpret_cast<void*>(UINTPTR_MAX - 3), reinterpret_cast<void*>(tiny)})
	{
		input.data = invalid;
		parse.data = invalid;
		ok = Check(Png::Decode(handle, &input, &info) == Png::InvalidAddress && unchanged() &&
		           Png::ParseHeader(&parse, &info) == Png::InvalidAddress && unchanged(),
		           "denied PNG source preserves pixels and info") && ok;
		ok = Check(Png::Decode(handle, static_cast<const Png::DecodeInput*>(invalid), &info) == Png::InvalidAddress &&
		           Png::ParseHeader(static_cast<const Png::ParseInput*>(invalid), &info) == Png::InvalidAddress &&
		           Png::QueryMemorySize(static_cast<const Png::CreateInput*>(invalid)) == Png::InvalidAddress && unchanged(),
		           "denied descriptor is never dereferenced") && ok;
	}
	input.data = RGBA_PNG.data();
	parse.data = RGBA_PNG.data();
	for (void* invalid: {static_cast<void*>(bytes + page), static_cast<void*>(bytes + page * 2),
	                    static_cast<void*>(bytes + page - 4), reinterpret_cast<void*>(UINTPTR_MAX - 3),
	                    reinterpret_cast<void*>(tiny)})
	{
		input.output = invalid;
		ok = Check(Png::Decode(handle, &input, &info) == Png::InvalidAddress && unchanged(),
		           "denied pixels publish no metadata") && ok;
		input.output = output.data() + GUARD_SIZE;
		ok = Check(Png::Decode(handle, &input, static_cast<Png::ImageInfo*>(invalid)) == Png::InvalidAddress && unchanged() &&
		           Png::ParseHeader(&parse, static_cast<Png::ImageInfo*>(invalid)) == Png::InvalidAddress,
		           "denied image info publishes no pixels") && ok;
	}
	Png::Handle unpublished = nullptr;
	Workspace new_workspace(16);
	std::array<uint8_t, 16> before {};
	std::memcpy(before.data(), new_workspace.Data(), before.size());
	ok = Check(Png::Create(&create, bytes + page, 16, &unpublished) == Png::InvalidAddress && unpublished == nullptr &&
	           Png::Create(&create, new_workspace.Data(), 16, reinterpret_cast<Png::Handle*>(bytes + page)) == Png::InvalidAddress &&
	           std::memcmp(before.data(), new_workspace.Data(), before.size()) == 0 &&
	           Png::Create(&create, new_workspace.Data(), 16, &unpublished) == 0,
	           "create preflights both outputs before writing or registering workspace") && ok;
	ok = Check(std::all_of(bytes + page - 128, bytes + page, [](uint8_t value) { return value == OUTPUT_CANARY; }),
	           "cross-page failures preserve writable prefix") && ok;
	return Check(Png::Delete(handle) == 0 && Png::Delete(unpublished) == 0, "borrow decoder deletion") && ok;
}
#endif

} // namespace

int main()
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	if (!TestParseMetadata() || !TestMalformedHeaders() || !TestCreateAndLifecycle() || !TestDepthAttributeAndWidth())
	{
		return 1;
	}
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__APPLE__)
	if (!TestGuestMemoryBorrows()) return 1;
#endif
	std::puts("PngDecTest: PASS");
	return 0;
}
