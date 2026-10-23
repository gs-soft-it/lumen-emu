#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_UTILS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_UTILS_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"

#include <utility>
#include <vector>
#include <vulkan/vulkan_core.h>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen {
template <typename T>
class Vector;
} // namespace Lumen

namespace Lumen::Libs::Graphics {

class CommandBuffer;
struct GraphicContext;
struct VulkanBuffer;
struct VulkanImage;
struct VideoOutVulkanImage;
struct RenderTextureVulkanImage;
struct DepthStencilVulkanImage;
struct VulkanSwapchain;
struct GraphicsGuestLeaseResult;
struct EopCpuProofResult;
struct UploadRecordingBatch;

// CommandBuffer owns each batch until its existing retirement router proves
// completion/discard. Never release from End, submission failure or timeout.
UploadRecordingBatch* UtilCreateUploadRecordingBatch(GraphicContext* ctx, uint32_t queue_family);
void UtilReleaseUploadRecordingBatch(GraphicContext* ctx, UploadRecordingBatch* batch);
bool UtilResetUploadStaging(GraphicContext* ctx);

// Synchronous continuation supplied by qualified EOP or exclusive render-full
// WriteBack scopes, each retaining its own exact locks and original tickets.
// A proof may retain its successful destination lease; the continuation must
// return immediately on success. Validation/yield are allowed only after a
// failed acquisition has released every lease. Ordinary callers omit it.
struct VideoOutReadbackContinuation
{
	using Proof = EopCpuProofResult (*)(void*);
	void* context = nullptr;
	EopCpuProofResult (*resume)(void*, const char*, Proof, void*) = nullptr;
};
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool UtilUploadStagingCopyForTest(GraphicContext* ctx, UploadRecordingBatch* batch, const void* src, uint64_t bytes,
                                  uint64_t alignment, VkBuffer* buffer, uint64_t* offset);
enum class VideoOutPreparedContractFault { Entry, Publication, Both };
struct VideoOutPreparedContractObservation
{
	bool published = false;
	uint32_t reads = 0, entry_acquisitions = 0, publication_acquisitions = 0;
	uint64_t published_hash = 0;
	const char* failure_phase = nullptr;
	const char* reason = nullptr;
};
// Executes the production prepared publisher with modeled GPU bytes and real
// destination leases/store/hash. The continuation is the caller's real scope.
bool UtilVideoOutPreparedRenderFullContractTest(GraphicContext* ctx, VideoOutVulkanImage* image,
    uint64_t address, uint64_t size, uint64_t original_generation, VideoOutPreparedContractFault fault, bool exhaust,
    const VideoOutReadbackContinuation* continuation, VideoOutPreparedContractObservation* observation);
#endif

struct BufferImageCopy
{
	uint32_t offset;
	uint32_t pitch;
	uint32_t dst_level;
	uint32_t width;
	uint32_t height;
	int      dst_x;
	int      dst_y;
};

struct ImageImageCopy
{
	VulkanImage* src_image;
	uint32_t     src_level;
	uint32_t     dst_level;
	uint32_t     width;
	uint32_t     height;
	int          src_x;
	int          src_y;
	int          dst_x;
	int          dst_y;
};

struct BlitDestinationRect
{
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

void UtilBufferToImage(CommandBuffer* buffer, VulkanBuffer* src_buffer, uint32_t src_pitch, VulkanImage* dst_image, uint64_t dst_layout);
void UtilBufferToImage(CommandBuffer* buffer, VulkanBuffer* src_buffer, VulkanImage* dst_image, const Vector<BufferImageCopy>& regions,
                       uint64_t dst_layout);
void UtilImageToBuffer(CommandBuffer* buffer, VulkanImage* src_image, VulkanBuffer* dst_buffer, uint32_t dst_pitch, uint64_t src_layout);
void UtilImageToImage(CommandBuffer* buffer, const Vector<ImageImageCopy>& regions, VulkanImage* dst_image, uint64_t dst_layout);
BlitDestinationRect UtilComputeBlitDestinationRect(uint32_t source_width, uint32_t source_height,
                                                   uint32_t destination_width, uint32_t destination_height,
                                                   bool preserve_aspect_ratio);
void UtilBlitImage(CommandBuffer* buffer, VulkanImage* src_image, VulkanSwapchain* dst_swapchain,
                   bool preserve_aspect_ratio);

// Debug/diagnostic helper: reads back a single pixel into a host-visible VulkanBuffer.
// The destination buffer must have VK_BUFFER_USAGE_TRANSFER_DST_BIT and be host-visible.
void UtilReadbackPixel(CommandBuffer* buffer, VulkanImage* src_image, VulkanBuffer* dst_buffer, uint32_t x, uint32_t y,
	                   uint64_t dst_offset);
void UtilFillImage(GraphicContext* ctx, VulkanImage* dst_image, const void* src_data, uint64_t size, uint32_t src_pitch,
                   uint64_t dst_layout);
void UtilFillImage(GraphicContext* ctx, VulkanImage* dst_image, const void* src_data, uint64_t size, const Vector<BufferImageCopy>& regions,
                   uint64_t dst_layout);
void UtilUploadImage(GraphicContext* ctx, CommandBuffer* buffer, VulkanImage* dst_image, const void* src_data, uint64_t size,
                     uint32_t src_pitch, uint64_t dst_layout);
void UtilUploadImageForShaderStage(GraphicContext* ctx, CommandBuffer* buffer, VulkanImage* dst_image, const void* src_data,
                                   uint64_t size, uint32_t src_pitch, uint64_t dst_layout, uint64_t dst_stage);
void UtilUploadImage(GraphicContext* ctx, CommandBuffer* buffer, VulkanImage* dst_image, const void* src_data, uint64_t size,
                     const Vector<BufferImageCopy>& regions, uint64_t dst_layout);
void UtilFillImage(GraphicContext* ctx, const Vector<ImageImageCopy>& regions, VulkanImage* dst_image, uint64_t dst_layout);
enum class ImageReadbackLayout { Linear, Display2dThin, Color2dThin };
void UtilFillBuffer(GraphicContext* ctx, void* dst_data, uint64_t size, uint32_t dst_pitch, VulkanImage* src_image, uint64_t src_layout,
                    ImageReadbackLayout output_layout = ImageReadbackLayout::Linear, bool neo = false, int producer_queue = -1);
// Host-only RGBA8/Thin1D/POW2_PAD mip export, with the exact TileGetTextureSize
// offsets and per-mip pitches expected by TileConvertLinearToTiled. Preflight
// performs no Vulkan operation. The caller retains the native lifetime and
// exclusive image/layout authority, proves completed producers, and releases
// guest leases and the tracker lock before the synchronous wait. Its utility
// command buffer has no parent processor, guest semaphores or callbacks; the
// queue mutex is released before waiting. The caller may retain renderer and
// processor exclusion to preserve that authority throughout the readback.
// Only logical texels are copied; output padding is zero and is not guest data.
// Failure leaves output unchanged. No guest address or mapping token is used.
bool UtilCanReadbackImageRegionsToHost(const VulkanImage* image, const Vector<BufferImageCopy>& regions,
                                     uint64_t bytes, int producer_queue = -1);
bool UtilReadbackImageRegionsToHost(GraphicContext* ctx, const VulkanImage* image, const Vector<BufferImageCopy>& regions,
    uint64_t bytes, std::vector<uint8_t>* output, int producer_queue = -1);
bool UtilCanReadbackTextureMipsToHost(const VulkanImage* image, uint32_t pitch, bool pow2_pad, uint64_t bytes);
bool UtilReadbackTextureMipsToHost(GraphicContext* ctx, const VulkanImage* image, uint32_t pitch, bool pow2_pad,
    uint64_t bytes, std::vector<uint8_t>* output, bool admitted_handoff = false, int producer_queue = -1);
// Candidate VideoOut readback: no VM lease is held while GPU work is waiting.
// The generation captured before the caller's producer wait is mandatory.
bool UtilCanReadbackVideoOutToGuest(GraphicContext* ctx, VideoOutVulkanImage* image, uint64_t address, uint64_t size,
                                  uint32_t pitch, ImageReadbackLayout layout, bool neo, uint64_t* generation,
                                  uint64_t expected_generation = UINT64_MAX, GraphicsGuestLeaseResult* result = nullptr);
// Optional host hash output covers the complete published range, including
// preserved padding, under the final ReadWrite lease; zero on failure.
bool UtilReadbackVideoOutToGuest(GraphicContext* ctx, VideoOutVulkanImage* image, uint64_t address, uint64_t size,
                               uint32_t pitch, ImageReadbackLayout layout, bool neo, uint64_t expected_generation,
	                               uint64_t* published_hash = nullptr,
	                               const VideoOutReadbackContinuation* continuation = nullptr);
// Typed EOP color export: single-mip RGBA8/tile14, exact physical guest
// footprint. The native image remains a RenderTexture; ordinary callbacks
// keep their original admission. Padding is preserved and included in hash.
// Only a caller which proved the complete RenderTargetSnapshot pair may pass
// snapshot_pair=true to admit SHADER_READ_ONLY_OPTIMAL. The actual original
// layout is validated and restored; ordinary callers remain attachment-only.
bool UtilCanReadbackRenderTextureToGuest(GraphicContext* ctx, RenderTextureVulkanImage* image, uint64_t address, uint64_t size,
                                       uint32_t pitch, bool neo, uint64_t* generation, uint64_t expected_generation,
                                       GraphicsGuestLeaseResult* result = nullptr, bool snapshot_pair = false);
bool UtilReadbackRenderTextureToGuest(GraphicContext* ctx, RenderTextureVulkanImage* image, uint64_t address, uint64_t size,
                                    uint32_t pitch, bool neo, uint64_t expected_generation, uint64_t* published_hash,
                                    const VideoOutReadbackContinuation* continuation = nullptr, bool snapshot_pair = false);
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
// A raw-device probe supplies its real GPU transfer/fence. Destination proof,
// typed profile, staging ownership, retile and hash use the production writer.
bool UtilRenderTexturePublicationContractTest(GraphicContext* ctx, RenderTextureVulkanImage* image, uint64_t address,
    uint64_t size, uint32_t pitch, bool neo, uint64_t original_generation, uint64_t* published_hash,
    bool (*read)(void* context, void* staging, uint64_t size), void* read_context, bool snapshot_pair = false);
#endif
// Release retained readback staging while the device is alive and graphics
// workers are quiescent. Refuses reset while any readback lease is active.
bool UtilResetReadbackStaging(GraphicContext* ctx);
// Copies only active texels from an already synchronized linear readback.
// Invalid format, footprint, capacity or overlapping buffers leave dst intact.
bool UtilTryGetImageReadbackSpan(VkFormat format, uint32_t width, uint32_t height, uint32_t pitch,
                                uint64_t* row_bytes, uint64_t* span);
bool UtilCopyReadbackRows(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                          VkFormat format, uint32_t width, uint32_t height, uint32_t pitch);
// Overlay owned linear native texels onto a guest-layout snapshot. Padding is
// preserved; this performs no guest access, GPU work or synchronization.
bool UtilCopyReadbackImage(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                           VkFormat format, uint32_t width, uint32_t height, uint32_t pitch,
                           ImageReadbackLayout layout, bool neo);
// Initial upload to a new vertex/index buffer, recorded before its first draw
// on the same graphics command buffer. Staging is retained until completion.
void UtilUploadVertexInputBuffer(GraphicContext* ctx, CommandBuffer* buffer, VulkanBuffer* dst_buffer,
                                 const void* src_data, uint64_t size);
void UtilCopyBuffer(VulkanBuffer* src_buffer, VulkanBuffer* dst_buffer, uint64_t size);
VkImageAspectFlags UtilGetDepthStencilAspectMask(VkFormat format);
void UtilSetDepthLayoutOptimal(DepthStencilVulkanImage* image);
// Initialize a new D32S8 attachment to depth +0/stencil 0, with GPU completion
// before returning. This function never inspects or modifies guest memory.
void UtilInitializeDepthStencilZero(GraphicContext* ctx, DepthStencilVulkanImage* image);
// Host-only integral +0/S0 verification. The caller serializes and completes
// the latest producer on queue, retains sole image ownership and holds no guest
// mapping lease while this helper waits. No guest bytes or hashes are written.
// Queue must be the original graphics queue. The explicit expected layout must
// match the image and is restored; existing callers retain attachment-only proof.
bool UtilVerifyDepthStencilZero(GraphicContext* ctx, DepthStencilVulkanImage* image, int queue,
                                const char** rejection = nullptr,
                                VkImageLayout expected_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
void UtilSetImageLayoutOptimal(VulkanImage* image);

void VulkanCreateBuffer(GraphicContext* gctx, uint64_t size, VulkanBuffer* buffer);
void VulkanDeleteBuffer(GraphicContext* gctx, VulkanBuffer* buffer);
bool VulkanReadbackMemoryContractTest();

inline std::pair<int, int> UtilCalcMipmapOffset(uint32_t lod, uint32_t width, uint32_t height)
{
	uint32_t mip_width  = width;
	uint32_t mip_height = height;
	int      mip_x      = 0;
	int      mip_y      = 0;

	for (uint32_t i = 0; i < 16; i++)
	{
		if (i == lod)
		{
			return {mip_x, mip_y};
		}

		bool odd = ((i & 1u) != 0);
		mip_x += static_cast<int>(odd ? mip_width : 0);
		mip_y += static_cast<int>(odd ? 0 : mip_height);

		mip_width >>= (mip_width > 1 ? 1u : 0u);
		mip_height >>= (mip_height > 1 ? 1u : 0u);
	}

	return {mip_x, mip_y};
}

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_UTILS_H_ */
