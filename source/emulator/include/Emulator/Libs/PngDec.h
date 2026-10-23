#ifndef EMULATOR_LIBS_PNG_DEC_H_
#define EMULATOR_LIBS_PNG_DEC_H_

#include "Emulator/Common.h"

#include <cstddef>
#include <cstdint>

// Independently expressed guest ABI. No external header is included.
namespace Lumen::Libs::PngDec {

using Handle = void*;

struct ParseInput
{
	const void* data;
	uint32_t    size;
	uint32_t    reserved;
};

struct ImageInfo
{
	uint32_t width;
	uint32_t height;
	uint16_t color;
	uint16_t depth;
	uint32_t flags;
};

struct CreateInput
{
	uint32_t size;
	uint32_t attributes;
	uint32_t max_width;
};

struct DecodeInput
{
	const void* data;
	void*       output;
	uint32_t    size;
	uint32_t    output_size;
	uint16_t    format;
	uint16_t    alpha;
	uint32_t    pitch;
};

static_assert(sizeof(ParseInput) == 16 && offsetof(ParseInput, reserved) == 12);
static_assert(sizeof(ImageInfo) == 16 && offsetof(ImageInfo, depth) == 10 && offsetof(ImageInfo, flags) == 12);
static_assert(sizeof(CreateInput) == 12 && offsetof(CreateInput, max_width) == 8);
static_assert(sizeof(DecodeInput) == 32 && offsetof(DecodeInput, size) == 16 && offsetof(DecodeInput, pitch) == 28);

constexpr int32_t InvalidAddress = static_cast<int32_t>(0x80690001u);
constexpr int32_t InvalidSize    = static_cast<int32_t>(0x80690002u);
constexpr int32_t InvalidParam   = static_cast<int32_t>(0x80690003u);
constexpr int32_t InvalidHandle  = static_cast<int32_t>(0x80690004u);
constexpr int32_t InvalidWork    = static_cast<int32_t>(0x80690005u);
constexpr int32_t InvalidData    = static_cast<int32_t>(0x80690010u);
constexpr int32_t Unsupported    = static_cast<int32_t>(0x80690011u);
constexpr int32_t DecodeFailure  = static_cast<int32_t>(0x80690012u);
constexpr int32_t Fatal          = static_cast<int32_t>(0x80690020u);

int32_t LUMEN_SYSV_ABI ParseHeader(const ParseInput* input, ImageInfo* info);
int32_t LUMEN_SYSV_ABI QueryMemorySize(const CreateInput* input);
int32_t LUMEN_SYSV_ABI Create(const CreateInput* input, void* workspace, uint32_t workspace_size, Handle* handle);
int32_t LUMEN_SYSV_ABI Decode(Handle handle, const DecodeInput* input, ImageInfo* info);
int32_t LUMEN_SYSV_ABI Delete(Handle handle);
void ResetSessionState();

} // namespace Lumen::Libs::PngDec

#endif
