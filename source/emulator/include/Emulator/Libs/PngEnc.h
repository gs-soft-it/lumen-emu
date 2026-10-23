#ifndef EMULATOR_LIBS_PNG_ENC_H_
#define EMULATOR_LIBS_PNG_ENC_H_

#include "Emulator/Common.h"

#include <cstddef>
#include <cstdint>

// Independently expressed Gen4 guest ABI.
namespace Lumen::Libs::PngEnc {

using Handle = void*;

struct CreateInput
{
	uint32_t size;
	uint32_t attributes;
	uint32_t max_width;
	uint32_t max_filters;
};

struct EncodeInput
{
	const void* image;
	void*       png;
	uint32_t    image_size;
	uint32_t    png_size;
	uint32_t    width;
	uint32_t    height;
	uint32_t    pitch;
	uint16_t    format;
	uint16_t    color;
	uint16_t    depth;
	uint16_t    clut_count;
	uint16_t    filter;
	uint16_t    compression;
};

struct OutputInfo
{
	uint32_t data_size;
	uint32_t processed_height;
};

static_assert(sizeof(CreateInput) == 16 && alignof(CreateInput) == 4 && offsetof(CreateInput, max_filters) == 12);
static_assert(sizeof(EncodeInput) == 48 && alignof(EncodeInput) == 8 && offsetof(EncodeInput, image_size) == 16 &&
              offsetof(EncodeInput, width) == 24 && offsetof(EncodeInput, format) == 36 &&
              offsetof(EncodeInput, compression) == 46);
static_assert(sizeof(OutputInfo) == 8 && alignof(OutputInfo) == 4 && offsetof(OutputInfo, processed_height) == 4);
static_assert(sizeof(Handle) == 8);

constexpr int32_t InvalidAddress = static_cast<int32_t>(0x80690101u);
constexpr int32_t InvalidSize    = static_cast<int32_t>(0x80690102u);
constexpr int32_t InvalidParam   = static_cast<int32_t>(0x80690103u);
constexpr int32_t InvalidHandle  = static_cast<int32_t>(0x80690104u);
constexpr int32_t DataOverflow  = static_cast<int32_t>(0x80690110u);
constexpr int32_t Fatal          = static_cast<int32_t>(0x80690120u);

int32_t LUMEN_SYSV_ABI QueryMemorySize(const CreateInput* input);
int32_t LUMEN_SYSV_ABI Create(const CreateInput* input, void* workspace,
                              uint32_t workspace_size, Handle* handle);
int32_t LUMEN_SYSV_ABI Encode(Handle handle, const EncodeInput* input, OutputInfo* info);
int32_t LUMEN_SYSV_ABI Delete(Handle handle);
// The aggregate HLE lifecycle calls this only after all guest callbacks have
// stopped and joined; it is not a concurrent guest operation.
void ResetSessionState();

#if defined(LUMEN_PNG_ENC_TEST_HOOK)
enum class EncodePausePoint: uint32_t
{
	AfterAdmission = 1,
	AfterSlotLock = 2,
	AfterMemoryBorrow = 3,
};

void ArmEncodePauseForTest(EncodePausePoint point);
bool EncodePausedForTest();
void ResumeEncodeForTest();
bool IsHandleClosingForTest(Handle handle);
void SetParallelEncodeBarrierForTest(uint32_t participants);
uint32_t MaxParallelEncodesForTest();
void FailNextAllocationForTest();
#endif

} // namespace Lumen::Libs::PngEnc

#endif
