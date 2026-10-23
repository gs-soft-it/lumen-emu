#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUT_H_

#include "Lumen/Core/Common.h"
//#include "Lumen/Core/Subsystems.h"

#include "Emulator/Common.h"
#include "Emulator/Kernel/EventQueue.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"

#include <array>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {
// struct VulkanSwapchain;
struct VideoOutVulkanImage;
} // namespace Lumen::Libs::Graphics

namespace Lumen::Libs::VideoOut {

struct VideoOutResolutionStatus;
struct VideoOutBufferAttribute;
struct VideoOutFlipStatus;
struct VideoOutVblankStatus;

struct VideoOutBufferImageInfo
{
	Graphics::VideoOutVulkanImage* image          = nullptr;
	Graphics::GpuMemoryObjectHandle tracked_handle;
	uint32_t                       index          = static_cast<uint32_t>(-1);
	uint32_t                       slot           = static_cast<uint32_t>(-1);
	uint64_t                       buffer_size    = 0;
	uint64_t                       buffer_pitch   = 0;
	uint64_t                       generation     = 0;
	int                            set_id         = -1;
	bool                           registered_tiled = false;
	bool                           registered_neo   = false;
	bool                           lifecycle_trace_dump = false;
};

enum class VideoOutImageLookupKind : uint8_t
{
	RenderTarget,
	SampledTexture,
};

// Snapshot of the incoming render command at a VideoOut lookup. It does not
// prove completion or establish shared contents between host images.
struct VideoOutImageLookupIdentity
{
	VideoOutImageLookupKind kind                     = VideoOutImageLookupKind::RenderTarget;
	const void* command_buffer                 = nullptr;
	uint64_t    submit_id                      = 0;
	uint64_t    recording_id                   = 0;
	uint64_t    command_ordinal                = 0;
	uint64_t    submission_session_epoch       = 0;
	uint64_t    recording_submit_serial        = 0;
	uint64_t    begin_submit_serial_cutoff     = 0;
	uint64_t    begin_completed_serial_cutoff  = 0;
	uint64_t    requested_size                 = 0;
	uint64_t    requested_pitch                = 0;
	uint32_t    requested_width                = 0;
	uint32_t    requested_height               = 0;
	uint32_t    requested_data_format          = 0;
	uint32_t    requested_number_format        = 0;
	uint32_t    requested_tile_mode            = 0;
	uint32_t    requested_swizzle              = 0;
	uint32_t    requested_base_level           = 0;
	uint32_t    requested_levels               = 0;
	bool        requested_read_only            = false;
	bool        requested_storage_descriptor   = false;
	int         queue                          = -1;
};

void                    VideoOutInit(uint32_t width, uint32_t height);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
enum class VideoOutIOSFlipCancelResult
{
	Failed,
	Busy,
	Idle,
	Cancelled,
};

[[nodiscard]] VideoOutIOSFlipCancelResult VideoOutCancelIOSRuntimeFlips();
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
bool                    VideoOutResetIOSRuntimeSession();
#endif
VideoOutBufferImageInfo VideoOutGetImage(uint64_t addr, const VideoOutImageLookupIdentity* identity);
[[nodiscard]] bool      VideoOutLifecycleTraceEnabled();
[[nodiscard]] bool      VideoOutModeInitEnabled();
[[nodiscard]] bool      VideoOutConfigureAnySdrEnabled();
[[nodiscard]] bool      VideoOutEventDataEnabled();
enum class VideoOutEventPayloadResult { LegacyRaw, Encoded, OutOfRange };
[[nodiscard]] VideoOutEventPayloadResult VideoOutEncodeEventPayload(uint64_t payload, uint64_t& encoded);
// Prepare a local event representation before admitting work. Unsupported
// payloads in the experimental profile fail closed without truncation.
uint64_t VideoOutPrepareEventPayload(uint64_t payload);
bool VideoOutEventDataProducerContractTest();
[[nodiscard]] bool      VideoOutMatchesAnySdrMode(const std::array<uint8_t, 32>& host_record);
// Local bounded initializer helper; false means that nothing was published.
[[nodiscard]] bool      VideoOutTryInitializeMode(void* output, uint32_t size);
void                    VideoOutDumpLifecycleTrace(const char* reason);
void                    VideoOutWaitFlipDone(int handle, int index);

LUMEN_SYSV_ABI int  VideoOutOpen(int user_id, int bus_type, int index, const void* param);
LUMEN_SYSV_ABI void VideoOutModeSetAny(void* output, uint32_t size);
LUMEN_SYSV_ABI int  VideoOutClose(int handle);
LUMEN_SYSV_ABI int  VideoOutGetDeviceCapabilityInfo(int handle, void* info, size_t size_of_info);
LUMEN_SYSV_ABI int  VideoOutIsFlipPending(int handle);
LUMEN_SYSV_ABI int  VideoOutConfigureOutputMode(int handle, uint32_t reserved, const void* mode, const void* options,
                                               uint32_t mode_size, uint32_t options_size);
LUMEN_SYSV_ABI int  VideoOutGetResolutionStatus(int handle, VideoOutResolutionStatus* status);
LUMEN_SYSV_ABI void VideoOutSetBufferAttribute(VideoOutBufferAttribute* attribute, uint32_t pixel_format, uint32_t tiling_mode,
                                              uint32_t aspect_ratio, uint32_t width, uint32_t height, uint32_t pitch_in_pixel);
LUMEN_SYSV_ABI int  VideoOutSetFlipRate(int handle, int rate);
LUMEN_SYSV_ABI int  VideoOutAddFlipEvent(LibKernel::EventQueue::KernelEqueue eq, int handle, void* udata);
LUMEN_SYSV_ABI int  VideoOutAddVblankEvent(LibKernel::EventQueue::KernelEqueue eq, int handle, void* udata);
LUMEN_SYSV_ABI int  VideoOutGetEventData(const LibKernel::EventQueue::KernelEvent* event, int64_t* data);
LUMEN_SYSV_ABI int  VideoOutRegisterBuffers(int handle, int start_index, void* const* addresses, int buffer_num,
                                           const VideoOutBufferAttribute* attribute);
LUMEN_SYSV_ABI int  VideoOutSubmitFlip(int handle, int index, int flip_mode, int64_t flip_arg);
LUMEN_SYSV_ABI int  VideoOutGetFlipStatus(int handle, VideoOutFlipStatus* status);
LUMEN_SYSV_ABI int  VideoOutGetVblankStatus(int handle, VideoOutVblankStatus* status);
LUMEN_SYSV_ABI int  VideoOutSetWindowModeMargins(int handle, int top, int bottom);
LUMEN_SYSV_ABI int  VideoOutColorSettingsSetGamma(void* settings, float gamma);
LUMEN_SYSV_ABI int  VideoOutAdjustColor(int handle, const void* settings);

void VideoOutBeginVblank();
void VideoOutEndVblank();
// Called synchronously on the presentation caller, outside the flip-queue lock
// and before claiming a request. Returning false leaves every request queued.
// Neither callback nor userdata is retained after VideoOutFlipWindow returns.
using VideoOutWaitService = bool (*)(void* userdata);
bool VideoOutFlipWindow(uint32_t micros, VideoOutWaitService service = nullptr, void* userdata = nullptr);

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
[[nodiscard]] bool VideoOutIOSFlipCancelContractTest();
[[nodiscard]] bool VideoOutFramePresentLimitContractTest();
[[nodiscard]] bool VideoOutFramePresentCheckpointContractTest();
[[nodiscard]] bool VideoOutBufferPublicationContractTest();
[[nodiscard]] bool VideoOutRegistrationHandoffContractTest();
[[nodiscard]] bool VideoOutWaitServiceContractTest();
#endif

} // namespace Lumen::Libs::VideoOut

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUT_H_ */
