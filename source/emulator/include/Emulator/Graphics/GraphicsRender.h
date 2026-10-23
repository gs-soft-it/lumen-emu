#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/DrawAccessReservation.h"
#include "Emulator/Graphics/NativeFenceWaitPins.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Kernel/EventQueue.h"

#include <array>
#include <memory>
#include <optional>
#include <vulkan/vulkan_core.h>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::GuestCpu { enum class MemoryAccessIntent : uint32_t; }

namespace Lumen::Core { class Mutex; }

namespace Lumen::Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
class Shader;
} // namespace HW

class CommandProcessor;
class LabelScalarCaptureTicket;
enum class GpuMemoryCpuCopyRole;
struct GraphicContext;
struct VideoOutVulkanImage;
struct DepthStencilVulkanImage;
struct TextureVulkanImage;
struct StorageTextureVulkanImage;
struct RenderTextureVulkanImage;
struct VulkanCommandPool;
struct VulkanBuffer;
struct VulkanFramebuffer;
struct RenderDepthInfo;
struct RenderColorInfo;
struct DescriptorRecordingBatch;
struct UploadRecordingBatch;
struct GpuMemoryDbCacheProofResult;
struct GpuMemoryRecordingIdentity;
class GpuMemoryDbCachePreflightTicket;
class GpuMemoryRangeClaimPlan;

// One synchronous draw/dispatch's owned input analysis. The caller retains
// its processor and memory-handoff admission from preparation through render.
// This is neither a cross-draw cache nor a guest-memory/lifetime authority.
// Native command-buffer rotation during preflight does not invalidate it.
class GraphicsRenderPreparedInputs
{
public:
	GraphicsRenderPreparedInputs() = default;
	LUMEN_CLASS_NO_COPY(GraphicsRenderPreparedInputs);

private:
	friend struct GraphicsRenderPreparedInputAccess;
	struct DrawInputs { ShaderVertexInputInfo vertex; ShaderPixelInputInfo pixel; };
	std::optional<DrawInputs> m_draw;
	std::optional<ShaderComputeInputInfo> m_compute;
	std::array<uint8_t, sizeof(HW::Context)> m_context_bytes;
	std::array<uint8_t, sizeof(HW::Shader)> m_shader_bytes;
	const HW::Context* m_context = nullptr;
	const HW::Shader* m_shader = nullptr;
	const void* m_thread = nullptr;
	bool m_is_compute = false;
	bool m_ready = false;
	mutable bool m_consumed = false;
};

// Owns every shader-analysis input before leaving guest-memory admission.
// Analyze performs private CPU work only. A fresh validation is mandatory
// before the result can be consumed by resource preflight. This owns no native
// resource and grants no permission to record commands outside admission.
class GraphicsRenderOwnedPreparation
{
public:
	GraphicsRenderOwnedPreparation();
	~GraphicsRenderOwnedPreparation();
	LUMEN_CLASS_NO_COPY(GraphicsRenderOwnedPreparation);
	[[nodiscard]] bool Capture(HW::Context* context, HW::Shader* shader);
	void Analyze();
	[[nodiscard]] bool Validate(HW::Context* context, HW::Shader* shader);
	[[nodiscard]] bool IsValidated() const;
	[[nodiscard]] const ShaderVertexSourceSnapshot* VertexSource() const;
private:
	friend struct GraphicsRenderPreparedInputAccess;
	friend class GraphicsRenderRangePreparation;
	[[nodiscard]] bool Consume(const HW::Context* context, const HW::Shader* shader,
	    ShaderVertexInputInfo* vertex, ShaderPixelInputInfo* pixel) const;
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

// One draw's shader sources, resource preflight and guarded physical closure.
// Capture is exclusive; executing with this plan requires the separate runtime
// active claim, mapping pin and native renderer ownership protocol.
class GraphicsRenderRangePreparation
{
public:
	GraphicsRenderRangePreparation();
	~GraphicsRenderRangePreparation();
	LUMEN_CLASS_NO_COPY(GraphicsRenderRangePreparation);
	bool Capture(HW::Context* context, HW::Shader* shader, uint64_t index_address, uint64_t index_bytes,
	             GraphicsRenderPreparedInputs* prepared, const std::vector<GpuMemoryClaimRange>& additional_ranges = {});
	const GpuMemoryRangeClaimPlan* RangePlan() const;
	uint64_t Owner() const;
	const ShaderVertexSourceSnapshot* VertexSource() const;
	void Reset();
private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

class CommandBuffer
{
public:
	using retire_func_t = void (*)(GraphicContext* ctx, void* obj);
	using completion_func_t = void (*)(GraphicContext* ctx, void* obj, bool completed);

	explicit CommandBuffer(int queue);
	virtual ~CommandBuffer();

	void              SetParent(CommandProcessor* parent) { m_parent = parent; }
	CommandProcessor* GetParent() { return m_parent; }

	LUMEN_CLASS_NO_COPY(CommandBuffer);

	[[nodiscard]] bool IsInvalid() const;
	[[nodiscard]] bool CanRecordTextureTransfer() const;
	// Only a definitely empty live recording can skip an optional boundary
	// submission. This does not cancel an explicit barrier, fence or serial.
	[[nodiscard]] bool HasPendingSubmissionWork() const;
	[[nodiscard]] bool CanPauseOwnedPreparation() const;

	void Allocate();
	void Free();
	void Begin() const;
	void End() const;
	void Execute();
	void ExecuteWithSemaphores(VkSemaphore wait_semaphore, VkPipelineStageFlags wait_stage, VkSemaphore signal_semaphore);
	void BeginRenderPass(VulkanFramebuffer* framebuffer, RenderColorInfo* color, RenderDepthInfo* depth,
	                     bool resources_read_only = false) const;
	void EndRenderPass() const;
	// All transfer, event, compute and outside-pass barrier recording uses this
	// accessor. It materializes the end of a retained physical render pass.
	VkCommandBuffer GetBufferOutsideRenderPass() const;
	// Only graphics binds, dynamic state and draws may preserve a retained pass.
	VkCommandBuffer GetBufferForDraw() const;
	void WaitForFence();
	void WaitForFenceAndReset();
	// A native-only observer: no completion, reset, callback or guest access.
	// Capture under the processor lock; Wait releases its pin before returning,
	// so guest-memory readmission must happen after Wait, without that lock.
	class NativeFenceWait final
	{
		friend class CommandBuffer;
	public:
		[[nodiscard]] VkResult Wait();
		bool IsActive() const { return pin.IsActive(); }
	private:
		NativeFenceWaitPins::Lease pin;
		VkDevice device = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
	};
	[[nodiscard]] bool CaptureNativeFenceWait(NativeFenceWait* wait);
	// Called only after vkDeviceWaitIdle succeeds. Discards recording or
	// executable commands without waiting on a fence. Retired resources are
	// drained separately after every command-processor lock has been released.
	[[nodiscard]] bool ResetAfterDeviceIdle();
	void               ReleaseRetiredAfterDeviceIdle();
	void               Retire(void* obj, retire_func_t delete_func);
	void               Retire(void* obj, completion_func_t completion_func);
	// Queue-ordered resources may be released after this command buffer's
	// successful fence, or after an independently proven device-idle reset.
	// A discarded recording without either proof remains fail-closed.
	void               RetireAfterQueueIdle(void* obj, retire_func_t delete_func);
	// Descriptor sets are shared only within this recording and retired by its
	// successful fence or proven discard, independently of access tracking.
	DescriptorRecordingBatch* GetDescriptorBatch();
	UploadRecordingBatch* GetUploadRecordingBatch();
	DescriptorRecordingBatch* PeekDescriptorBatch() const { return m_descriptor_batch; }
	UploadRecordingBatch* PeekUploadRecordingBatch() const { return m_upload_batch; }
	[[nodiscard]] GpuMemoryRecordingIdentity GetDrawRecordingIdentity() const;
	uint64_t ReserveDrawAccesses(uint64_t expected_ordinals);
	void BeginDrawAccessCommit(uint64_t token);
	void FinishDrawAccessCommit(uint64_t token);

	[[nodiscard]] uint32_t GetIndex() const { return m_index; }
	VulkanCommandPool*     GetPool() { return m_pool; }
	[[nodiscard]] bool     IsExecute() const { return m_execute; }
	[[nodiscard]] int      GetQueue() const { return m_queue; }
	[[nodiscard]] uint64_t GetRecordingId() const { return m_recording ? m_recording_id : 0; }
	[[nodiscard]] uint64_t GetTrackedCommandOrdinal() const { return m_recording ? m_tracked_command_ordinal : 0; }
	[[nodiscard]] uint64_t GetSubmissionSessionEpoch() const;
	[[nodiscard]] uint64_t GetRecordingSubmitSerial() const;
	// Successful, still-pending native submission. Recording metadata alone
	// never supplies a fence dependency; these become zero after completion.
	[[nodiscard]] uint64_t GetPendingSubmissionSessionEpoch() const;
	[[nodiscard]] uint64_t GetPendingSubmitSerial() const;
	[[nodiscard]] uint64_t GetBeginSubmitSerialCutoff() const;
	[[nodiscard]] uint64_t GetBeginCompletedSerialCutoff() const;
	// The caller serializes this buffer through its owning processor. This
	// compares retained metadata after End/fence reset; native completion must
	// be proved separately on the original processor/queue/session/serial.
	[[nodiscard]] bool MatchesStoppedSubmissionIdentity(const GpuMemoryRecordingIdentity& original) const;
	uint64_t               NextTrackedCommandOrdinal();

private:
	struct RetiredResources;

	void ReleaseRetired(bool after_device_idle = false);
	void WaitForFenceInternal(bool reset_command);
	void EndRetainedRenderPass() const;
	static void ReleaseDescriptorBatch(GraphicContext* ctx, void* obj);
	static void ReleaseUploadRecordingBatch(GraphicContext* ctx, void* obj);

	VulkanCommandPool* m_pool    = nullptr;
	uint32_t           m_index   = static_cast<uint32_t>(-1);
	int                m_queue   = -1;
	bool               m_execute = false;
	CommandProcessor*  m_parent  = nullptr;
	RetiredResources*  m_retired = nullptr;
	NativeFenceWaitPins m_native_wait_pins;
	DrawAccessReservation m_draw_access_reservation;
	DescriptorRecordingBatch* m_descriptor_batch = nullptr;
	UploadRecordingBatch* m_upload_batch = nullptr;
	uint64_t m_upload_recording_id = 0;
	mutable uint64_t   m_recording_id             = 0;
	mutable uint64_t   m_tracked_command_ordinal  = 0;
	mutable bool       m_recording                = false;
	mutable bool       m_render_pass_active       = false;
	mutable bool       m_native_commands          = false;
};

// Lock-free bridge to the owning command processor's diagnostic timeline.
// Callers must already obey the command processor's normal serialization. The
// bridge fails closed and never changes command submission behavior.
[[nodiscard]] bool GraphicsRunBeginTrackedCommandBufferSubmission(CommandProcessor* cp, int queue, uint64_t* session_epoch,
	                                                               uint64_t* recording_submit_serial,
	                                                               uint64_t* begin_submit_serial_cutoff,
	                                                               uint64_t* begin_completed_serial_cutoff);
[[nodiscard]] bool GraphicsRunRecordTrackedCommandBufferSubmission(CommandProcessor* cp, int queue, uint64_t session_epoch,
	                                                                uint64_t recording_submit_serial);
void GraphicsRunRecordTrackedCommandBufferCompletion(CommandProcessor* cp, int queue, uint64_t session_epoch,
	                                                   uint64_t recording_submit_serial);

void GraphicsRenderInit();
void GraphicsRenderCreateContext();
bool GraphicsRenderResetForNextSession();
bool GraphicsRenderReportNullSampledImageFinal();
[[nodiscard]] bool GraphicsRenderGpuR8CaptureEnabled();
[[nodiscard]] bool GraphicsRenderGpuRgba8MipsCaptureEnabled();
#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool GraphicsRenderPipelineCacheTombstoneContractTest();
#endif
#if defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
bool GraphicsRenderComputeImageStoreBarrierContractTest();
bool GraphicsRenderVideoOutSampledAliasBarrierContractTest();
bool GraphicsRenderDrawAccessPlanContractTest();
bool GraphicsRenderAutoIndexPrimitiveContractTest();
bool GraphicsRenderPipelineCacheGrowthContractTest();
bool GraphicsRenderRt0ComponentMaskContractTest();
bool GraphicsRenderFramebufferCacheReleaseAllContractTest();
bool GraphicsRenderDynamicScissorContractTest();
bool GraphicsRenderStencilClearContractTest();
bool GraphicsRenderInactiveD16DescriptorContractTest();
bool GraphicsRenderStencilClearMasksForTest(bool stencil_clear, bool depth_clear, uint32_t front_write_mask,
                                           uint32_t back_write_mask, uint32_t* front_out, uint32_t* back_out);
bool GraphicsRenderPolygonOffsetContractTest();
bool GraphicsRenderBlendConstantContractTest();
bool GraphicsRenderVertexInputFormatContractTest();
bool GraphicsRenderOrdinarySamplerComparisonContractTest();
bool GraphicsRenderSamplerZFilterDimensionContractTest();
bool GraphicsRenderBc7TextureProfileContractTest();
bool GraphicsRenderTexturePreparationOrderContractTest();
bool GraphicsRenderRetainedRecordingScopeContractTest();
bool GraphicsRenderGpuR8CaptureContractTest();
bool GraphicsRenderResourceCaptureLimitsContractTest();
bool GraphicsRenderDescriptorRecordingContractTest();
bool GraphicsRenderConstantUploadContractTest();
bool GraphicsRenderPreparedInputsContractTest();
bool CommandBufferPendingSubmissionWorkContractTest();
bool GraphicsRenderPassReuseContractTest();
void GraphicsRenderRecordGpuR8ReadbackForTest(VkCommandBuffer command, VkImage image, VkImageLayout original_layout,
                                            uint32_t width, uint32_t height, VkBuffer destination);
#endif

void GraphicsRenderDrawIndex(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::UserConfig* ucfg, HW::Shader* sh_ctx,
                             uint32_t index_type_and_size, uint32_t index_count, const void* index_addr, uint32_t flags, uint32_t type,
                             const GraphicsRenderPreparedInputs* prepared = nullptr);
void GraphicsRenderDrawIndexAuto(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::UserConfig* ucfg, HW::Shader* sh_ctx,
                                 uint32_t index_count, uint32_t flags, const GraphicsRenderPreparedInputs* prepared = nullptr);
void GraphicsRenderWriteAtEndOfPipe64(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr, uint64_t value,
                                    const LabelScalarCaptureTicket* scalar_capture = nullptr);
void GraphicsRenderQueueEndOfPipeInterrupt(CommandBuffer* buffer, uint32_t event_id);
void GraphicsRenderWriteAtEndOfPipeClockCounter(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr);
void GraphicsRenderWriteAtEndOfPipeClockCounterWithInterruptWriteBack(uint64_t submit_id, CommandBuffer* buffer,
                                                                   uint64_t* dst_gpu_addr, uint32_t event_id);
void GraphicsRenderWriteAtEndOfPipe32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t value,
                                    const LabelScalarCaptureTicket* scalar_capture = nullptr);
void GraphicsRenderWriteAtEndOfPipeGds32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t dw_offset,
                                         uint32_t dw_num);
void GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBackFlip32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr,
                                                                uint32_t value, int handle, int index, int flip_mode, int64_t flip_arg, uint32_t event_id);
void GraphicsRenderWriteAtEndOfPipeWithFlip32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t value, int handle,
                                              int index, int flip_mode, int64_t flip_arg);
void GraphicsRenderWriteAtEndOfPipeOnlyFlip(uint64_t submit_id, CommandBuffer* buffer, int handle, int index, int flip_mode,
                                            int64_t flip_arg);
void GraphicsRenderWriteAtEndOfPipeWithWriteBack64(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr, uint64_t value);
void GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBack64(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr,
                                                            uint64_t value, uint32_t event_id);
void GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBack32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr,
                                                            uint32_t value, uint32_t event_id);
void GraphicsRenderWriteAtEndOfPipeWithInterrupt64(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr, uint64_t value, uint32_t event_id);
void GraphicsRenderWriteAtEndOfPipeWithInterrupt32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t value, uint32_t event_id);
class GpuMemoryRenderFullPreflightTicket;
struct GpuMemoryRenderFullWriteBackResult;
GpuMemoryRenderFullWriteBackResult GraphicsRenderTryWriteBackRenderFull(CommandProcessor* cp,
    GpuMemoryRenderFullPreflightTicket* ticket, bool exclusive_stream);
// Private entry witness for a CPU-only depth/VideoOut proof in exclusive render-full
// WriteBack. Its caller releases tracker2 before this exact renderer1/CP2 pause;
// completed native work and RunLock remain retained on the same stack.
[[nodiscard]] bool GraphicsRenderRenderFullCpuScopeValid(CommandProcessor* cp);
[[nodiscard]] bool GraphicsRenderRenderFullCpuPause(CommandProcessor* cp, CommandBuffer* buffer);
// Only the pure protected-depth attachment admission may use this pause.
// Its caller has released tracker1 while retaining renderer1 and CP1, with
// RunLock keeping the exclusive recording owned throughout the pause.
[[nodiscard]] bool GraphicsRenderUniformDepthZeroReusePause(CommandProcessor* cp, CommandBuffer* buffer);
// Only a pure VideoOut pin-equivalence admission, before lifetime/binding
// effects, may release renderer1/CP1 here. The caller released tracker1;
// exclusive RunLock ownership and the exact current recording stay retained.
[[nodiscard]] bool GraphicsRenderVideoOutPinAdmissionPause(CommandProcessor* cp, CommandBuffer* buffer);
// Draw/dispatch entry witness while its renderer mutex remains held. This
// grants no submission/completion proof and must be paired with CP ownership.
[[nodiscard]] bool GraphicsRenderRetainedRecordingScopeValid(CommandBuffer* buffer);
void GraphicsRenderWriteBack(CommandProcessor* cp, uint64_t vaddr, uint64_t size);
Core::Mutex* GraphicsRenderMemoryMutex() noexcept;
bool GraphicsRenderPrepareCpuMemoryAccess(uint64_t address, uint64_t bytes, GuestCpu::MemoryAccessIntent intent) noexcept;
void GraphicsRenderPrepareResourceBacking(HW::Context* ctx, HW::Shader* shader, bool compute, uint64_t index_address = 0,
    uint64_t index_bytes = 0, GraphicsRenderPreparedInputs* prepared = nullptr,
    const GraphicsRenderOwnedPreparation* owned = nullptr);
// Called only by CommandProcessor::CopyDmaData while that CP lock is held;
// preserves the global CP -> renderer -> tracker lock order.
[[nodiscard]] bool GraphicsRenderCopyDmaData(CommandProcessor* cp, uint64_t source_address,
                                             uint64_t destination_address, uint32_t byte_count);
void GraphicsRenderDispatchDirect(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::Shader* sh_ctx, uint32_t thread_group_x,
                                  uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode,
                                  const GraphicsRenderPreparedInputs* prepared = nullptr);
void GraphicsRenderMemoryBarrier(CommandBuffer* buffer);
void GraphicsRenderFullMemoryBarrier(CommandBuffer* buffer);
GpuMemoryDbCacheProofResult GraphicsRenderDbCacheAlreadySynchronizedBarrier(
    CommandBuffer* buffer, bool record, GpuMemoryDbCachePreflightTicket* ticket = nullptr);
void GraphicsRenderRenderTextureBarrier(CommandBuffer* buffer, uint64_t vaddr, uint64_t size);
void GraphicsRenderDepthStencilBarrier(CommandBuffer* buffer, uint64_t vaddr, uint64_t size);
void GraphicsRenderMemoryFree(CommandBuffer* buffer, uint64_t vaddr, uint64_t size);
void GraphicsRenderDeleteIndexBuffers();
void GraphicsRenderMemoryFlush(CommandBuffer* buffer, uint64_t vaddr, uint64_t size);

void DeleteFramebuffer(VideoOutVulkanImage* image);
void DeleteFramebuffer(DepthStencilVulkanImage* image);
void DeleteFramebuffer(RenderTextureVulkanImage* image);
void DeleteDescriptor(VulkanBuffer* buffer);
void DeleteDescriptor(VideoOutVulkanImage* image);
void DeleteDescriptor(TextureVulkanImage* image);
void DeleteDescriptor(StorageTextureVulkanImage* image);
void DeleteDescriptor(RenderTextureVulkanImage* image);

int GraphicsRenderAddEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id, void* udata);
int GraphicsRenderDeleteEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id);

void GraphicsRenderClearGds(uint64_t dw_offset, uint32_t dw_num, uint32_t clear_value);
// Keep byte offsets wide until the complete local GDS range is validated.
bool GraphicsRenderGdsReadRangeIsValid(uint64_t destination_address, uint64_t source_byte_offset, uint32_t byte_count);
// CP-consumed DMA readback: stage before publishing, with mapping protection
// on VM backends that provide guest-data leases. Does not add GPU ordering.
[[nodiscard]] bool GraphicsRenderReadGds(uint64_t destination_address, uint64_t source_byte_offset, uint32_t byte_count);
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool GraphicsRenderGdsReadDestinationContractTest();
#endif

// Returns the last RenderTexture used as color target with actual color writes
// Used as fallback when VideoOut buffer is not rendered to directly
RenderTextureVulkanImage* GraphicsRenderGetLastColorRenderTexture();
void GraphicsRenderResetLastColorRenderTexture();

uint32_t GraphicsRenderGetFrameDrawCalls();
uint32_t GraphicsRenderGetFrameRenderPasses();

void GraphicsRenderTriggerEopEvent();
bool GraphicsRenderInterruptCompletionContractTest();
bool GraphicsRenderFlipArgumentsContractTest();
// Notify only the specified graphics identifier. The caller supplies its
// established event payload; this does not map a GPU source to an identifier.
void GraphicsRenderTriggerEvent(int id, uint64_t data);

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_ */
