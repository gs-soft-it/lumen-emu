#include "Emulator/Graphics/GraphicsRender.h"

#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/File.h"
#include "Lumen/Core/Hash.h"
#include "Lumen/Core/Hashmap.h"
#include "Lumen/Core/MagicEnum.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Vector.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/DepthSurfaceLayout.h"
#include "Emulator/Graphics/DepthSurfaceImport.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/GuestWriteDestination.h"
#include "Emulator/Graphics/HardwareColorMetadata.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/HtileCpuObservation.h"
#include "Emulator/Graphics/Objects/DepthStencilBuffer.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/IndexBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/RenderTexture.h"
#include "Emulator/Graphics/Objects/StorageBuffer.h"
#include "Emulator/Graphics/Objects/StorageTexture.h"
#include "Emulator/Graphics/Objects/Texture.h"
#include "Emulator/Graphics/Objects/VertexBuffer.h"
#include "Emulator/Graphics/NullSampledImageContract.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/RenderTargetVulkanContract.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderColorExportContract.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Graphics/TextureThin1dReinterpretCopy.h"
#include "Emulator/Graphics/TextureBc7Contract.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/UploadStagingArena.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Graphics/VideoOutBufferVulkanContract.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Kernel/EventQueue.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Profiler.h"
#include "../../../../android/app/src/main/cpp/CriticalPathTrace.h"
#include "../../../cpu/runtime/MemoryHandoff.h"
#include "../../../cpu/runtime/MemoryHandoffPageCache.h"

#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && defined(LUMEN_ANDROID_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME))
#include "../../../../android/app/src/main/cpp/AndroidFexCoreRuntime.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <tuple>
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
#include <thread>
#endif
#include <utility>
#include <vector>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

constexpr int GRAPHICS_EVENT_EOP = 0x40;

struct Label;
struct RenderDepthInfo;
static void AbortGfxResourceCaptureForSessionReset();

struct VulkanDescriptor
{
	VkDescriptorSet descriptor_set = nullptr;
};

// Pack structs to guarantee the uniquess of object representation
#pragma pack(push, 1)

struct PipelineStencilStaticState
{
	VkStencilOp failOp      = VK_STENCIL_OP_KEEP;
	VkStencilOp passOp      = VK_STENCIL_OP_KEEP;
	VkStencilOp depthFailOp = VK_STENCIL_OP_KEEP;
	VkCompareOp compareOp   = VK_COMPARE_OP_NEVER;
};

struct PipelineStencilDynamicState
{
	uint32_t compareMask = 0;
	uint32_t writeMask   = 0;
	uint32_t reference   = 0;
};

enum class PipelineDepthBiasRepresentation : uint8_t
{
	FormatRelative,
	AbsoluteWindowDepth,
};

struct PipelineDepthBias
{
	bool                            enabled        = false;
	PipelineDepthBiasRepresentation representation = PipelineDepthBiasRepresentation::FormatRelative;
	float                           constant       = 0.0f;
	float                           slope          = 0.0f;
	float                           clamp          = 0.0f;
};

struct PipelineStaticParameters
{
	float                      viewport_scale[3]        = {};
	float                      viewport_offset[3]       = {};
	VkPrimitiveTopology        topology                 = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	bool                       with_depth               = false;
	bool                       depth_test_enable        = false;
	bool                       depth_write_enable       = false;
	VkCompareOp                depth_compare_op         = VK_COMPARE_OP_NEVER;
	bool                       depth_bounds_test_enable = false;
	float                      depth_min_bounds         = 0.0f;
	float                      depth_max_bounds         = 0.0f;
	bool                       stencil_test_enable      = false;
	PipelineStencilStaticState stencil_front;
	PipelineStencilStaticState stencil_back;
	uint32_t                   color_mask           = 0;
	bool                       cull_front           = false;
	bool                       cull_back            = false;
	bool                       face                 = false;
	PipelineDepthBias          depth_bias;
	uint8_t                    color_srcblend       = 0;
	uint8_t                    color_comb_fcn       = 0;
	uint8_t                    color_destblend      = 0;
	uint8_t                    alpha_srcblend       = 0;
	uint8_t                    alpha_comb_fcn       = 0;
	uint8_t                    alpha_destblend      = 0;
	bool                       separate_alpha_blend = false;
	bool                       blend_enable         = false;
	float                      blend_color_red      = 0.0f;
	float                      blend_color_green    = 0.0f;
	float                      blend_color_blue     = 0.0f;
	float                      blend_color_alpha    = 0.0f;

	bool operator==(const PipelineStaticParameters& other) const;
};

struct PipelineDynamicParameters
{
	bool vk_dynamic_state_scissor                = false;
	bool vk_dynamic_state_line_width             = false;
	bool vk_dynamic_state_stencil_compare_mask   = false;
	bool vk_dynamic_state_stencil_write_mask     = false;
	bool vk_dynamic_state_stencil_reference      = false;
	bool vk_dynamic_state_color_write_enable_ext = false;

	float line_width         = 1.0f;
	bool  color_write_enable = true;
	int   scissor_ltrb[4]    = {0};

	PipelineStencilDynamicState stencil_front;
	PipelineStencilDynamicState stencil_back;

	bool operator==(const PipelineDynamicParameters& other) const;
};
#pragma pack(pop)

static bool BuildScissorRect(const int (&ltrb)[4], VkRect2D* rectangle);

// The existing framebuffer/export profile represents MRT0 only. Preserve the
// raw mask as pipeline state and reject every component of another target.
static bool ResolveRt0ColorWriteMask(uint32_t target_mask, bool enabled, VkColorComponentFlags* result)
{
	if (result == nullptr) return false;
	*result = 0;
	if ((target_mask & ~0xFu) != 0) return false;
	if (!enabled) return true;
	if ((target_mask & 0x1u) != 0) *result |= VK_COLOR_COMPONENT_R_BIT;
	if ((target_mask & 0x2u) != 0) *result |= VK_COLOR_COMPONENT_G_BIT;
	if ((target_mask & 0x4u) != 0) *result |= VK_COLOR_COMPONENT_B_BIT;
	if ((target_mask & 0x8u) != 0) *result |= VK_COLOR_COMPONENT_A_BIT;
	return true;
}

static constexpr bool AbsoluteDepthBiasTopologySupported(const PipelineDepthBias& bias,
	                                                      VkPrimitiveTopology topology)
{
	if (!bias.enabled || bias.representation != PipelineDepthBiasRepresentation::AbsoluteWindowDepth)
	{
		return true;
	}
	return topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
	       topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
	       topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
}

static bool ApplyAbsoluteDepthBiasViewport(const PipelineDepthBias& bias, VkViewport* viewport,
	                                       const char** reason = nullptr)
{
	if (reason != nullptr) *reason = nullptr;
	const auto reject = [&](const char* value) {
		if (reason != nullptr) *reason = value;
		return false;
	};
	if (viewport == nullptr) return reject("missing viewport");
	if (!bias.enabled || bias.representation != PipelineDepthBiasRepresentation::AbsoluteWindowDepth ||
	    bias.constant == 0.0f)
		return true;

	const float original_min = viewport->minDepth;
	const float original_max = viewport->maxDepth;
	const float shifted_min = original_min + bias.constant;
	const float shifted_max = original_max + bias.constant;
	if (!std::isfinite(shifted_min) || !std::isfinite(shifted_max) || shifted_min < 0.0f || shifted_min > 1.0f ||
	    shifted_max < 0.0f || shifted_max > 1.0f)
		return reject("absolute bias moves viewport depth outside [0,1]");
	if (shifted_min - original_min != bias.constant || shifted_max - original_max != bias.constant ||
	    shifted_max - shifted_min != original_max - original_min)
		return reject("absolute bias viewport translation is not exactly representable");

	viewport->minDepth = shifted_min;
	viewport->maxDepth = shifted_max;
	return true;
}

// Original decoder of the Gen4 register contract. Only visible, enabled faces
// contribute; reset/disabled zero becomes one canonical pipeline state.
static bool ResolvePolygonOffset(const HW::Context& ctx, VkFormat depth_format, bool has_depth,
                                 PipelineDepthBias* result, const char** reason = nullptr)
{
	if (reason != nullptr) *reason = nullptr;
	const auto reject = [&](const char* value) {
		if (reason != nullptr) *reason = value;
		return false;
	};
	if (result == nullptr) return reject("missing bias output");
	*result = {};
	const auto& mode = ctx.GetModeControl();
	if (!mode.poly_offset_front_enable && !mode.poly_offset_back_enable) return true;
	if (mode.cull_front && mode.cull_back) return reject("both faces culled with offset requested");
	const bool front_active = !mode.cull_front && mode.poly_offset_front_enable;
	const bool back_active = !mode.cull_back && mode.poly_offset_back_enable;
	if (!front_active && !back_active) return true;
	const auto& words = ctx.GetPolygonOffsetRegisters().values;
	const auto decode = [&](uint32_t index) {
		float value;
		static_assert(sizeof(value) == sizeof(words[index]));
		std::memcpy(&value, &words[index], sizeof(value));
		return value;
	};
	const float clamp = decode(1);
	if (!std::isfinite(clamp)) return reject("nonfinite active clamp");
	const float front_scale = front_active ? decode(2) : 0.0f;
	const float front_offset = front_active ? decode(3) : 0.0f;
	const float back_scale = back_active ? decode(4) : 0.0f;
	const float back_offset = back_active ? decode(5) : 0.0f;
	if (!std::isfinite(front_scale) || !std::isfinite(front_offset) ||
	    !std::isfinite(back_scale) || !std::isfinite(back_offset)) return reject("nonfinite active face coefficient");
	if (!mode.cull_front && !mode.cull_back && (front_scale != back_scale || front_offset != back_offset))
		return reject("visible faces require different bias");
	const float scale = mode.cull_front ? back_scale : front_scale;
	const float offset = mode.cull_front ? back_offset : front_offset;
	if (scale == 0.0f && offset == 0.0f) return true;
	if (clamp != 0.0f) return reject("nonzero bias clamp is unsupported");
	if (!has_depth || (depth_format != VK_FORMAT_D32_SFLOAT && depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT))
		return reject("nonzero bias requires a D32Float depth plane");
	PipelineDepthBiasRepresentation representation;
	if (words[0] == 0x1e9u)
		representation = PipelineDepthBiasRepresentation::FormatRelative;
	else if (words[0] == 0u)
		representation = PipelineDepthBiasRepresentation::AbsoluteWindowDepth;
	else
		return reject("unsupported polygon offset depth representation");
	const float slope = scale / 16.0f;
	if (!std::isfinite(slope) || (scale != 0.0f && (slope == 0.0f || slope * 16.0f != scale)))
		return reject("slope conversion is not exactly representable");
	PipelineDepthBias resolved;
	resolved.enabled = true;
	resolved.representation = representation;
	resolved.constant = offset == 0.0f ? 0.0f : offset;
	resolved.slope = slope == 0.0f ? 0.0f : slope;
	if (representation == PipelineDepthBiasRepresentation::AbsoluteWindowDepth)
	{
		const auto& viewport_state = ctx.GetScreenViewport().viewports[0];
		VkViewport viewport {};
		viewport.minDepth = viewport_state.zoffset;
		viewport.maxDepth = viewport_state.zscale + viewport_state.zoffset;
		if (!ApplyAbsoluteDepthBiasViewport(resolved, &viewport, reason)) return false;
	}
	*result = resolved;
	return true;
}

struct VulkanPipeline
{
	VkPipelineLayout                pipeline_layout = nullptr;
	VkPipeline                      pipeline        = nullptr;
	const PipelineStaticParameters* static_params   = nullptr;
	PipelineDynamicParameters*      dynamic_params  = nullptr;
	bool                            ps_has_image_load = false;
	bool                            ps_depth_stencil_export_free = false;
	ShaderStorageWriteAuthority     vs_storage_write_authority {};
	ShaderStorageWriteAuthority     ps_storage_write_authority {};
	ShaderStorageWriteAuthority     cs_storage_write_authority {};
	bool                            cs_requires_post_image_store_barrier = false;
	ShaderComputeImageMutationClass cs_image_mutation = ShaderComputeImageMutationClass::None;
};

static bool PixelShaderDepthStencilExportFree(const ShaderCode& code)
{
	if (code.GetType() != ShaderType::Pixel) return false;
	// The sole supported embedded module writes only MRT0. Unknown modules do
	// not inherit this fact, even when the raw export registers are disabled.
	if (code.IsPsEmbedded()) return code.GetPsEmbeddedId() == 0;
	bool color_export = false;
	for (const auto& instruction: code.GetInstructions())
	{
		if (instruction.type != ShaderInstructionType::Exp) continue;
		if (instruction.export_target != 0 && instruction.export_target != 0xff) return false;
		switch (instruction.format)
		{
			case ShaderInstructionFormat::Mrt0OffOffComprVmDone:
			case ShaderInstructionFormat::Mrt0Vsrc0OffComprVmDone:
			case ShaderInstructionFormat::Mrt0Vsrc0Vsrc1ComprVmDone:
			case ShaderInstructionFormat::Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone: color_export = true; break;
			default: return false;
		}
	}
	return color_export;
}

// Shared opt-in metadata gate, implemented by the GPU-memory diagnostics.
bool GpuMemoryEopResourceTraceEnabled();

static bool TakeStorageNoStoreDiagnosticRecord()
{
	if (!GpuMemoryEopResourceTraceEnabled()) return false;
	static std::atomic<uint32_t> records {0};
	auto record = records.load(std::memory_order_relaxed);
	do
	{
		if (record >= 16) return false;
	} while (!records.compare_exchange_weak(record, record + 1u, std::memory_order_relaxed));
	return true;
}

static void TraceShaderStorageNoStoreFailure(const ShaderCode& code, ShaderType stage,
                                            const ShaderBindResources& bind, const VulkanPipeline* pipeline, bool qualified)
{
	if (qualified || bind.storage_buffers.buffers_num == 0 || !TakeStorageNoStoreDiagnosticRecord()) return;
	const char* reason = code.GetType() != stage ? "mismatched-stage" :
	                     code.IsVsEmbedded() || code.IsPsEmbedded() ? "embedded-module" :
	                     code.GetInstructions().IsEmpty() ? "empty-ir" :
	                     ShaderCodeHasStorageBufferWrites(code) ? "storage-store-ir" : "unsupported-stage";
	std::array<uint32_t, 3> counts {}, first_pcs {UINT32_MAX, UINT32_MAX, UINT32_MAX};
	std::array<int, 3> first_registers {-1, -1, -1};
	std::array<uint32_t, 3> first_types {};
	for (const auto& inst: code.GetInstructions())
	{
		int index = -1;
		if (inst.type == ShaderInstructionType::BufferStoreDword) index = 0;
		if (inst.type == ShaderInstructionType::BufferStoreFormatX) index = 1;
		if (inst.type == ShaderInstructionType::BufferStoreFormatXy) index = 2;
		if (index >= 0 && counts[index]++ == 0)
		{
			first_pcs[index] = inst.pc;
			first_registers[index] = inst.src[1].register_id;
			first_types[index] = static_cast<uint32_t>(inst.src[1].type);
		}
	}
	std::fprintf(stderr, "[GFX_STORAGE_NO_STORE_SHADER] pipeline=%p expected_stage=%u code_type=%u reason=%s"
	                     " shader=%08" PRIx32 ":%08" PRIx32 " embedded_vs=%u embedded_ps=%u embedded_vs_id=%u embedded_ps_id=%u"
	                     " instructions=%u storage_bindings=%d writes=%u dword_count=%u dword_first_pc=%08" PRIx32
	                     " dword_target_type=%u dword_target_register=%d formatx_count=%u formatx_first_pc=%08" PRIx32
	                     " formatx_target_type=%u formatx_target_register=%d formatxy_count=%u formatxy_first_pc=%08" PRIx32
	                     " formatxy_target_type=%u formatxy_target_register=%d metadata_only=1 admission=unchanged\n",
	             static_cast<const void*>(pipeline), static_cast<unsigned>(stage), static_cast<unsigned>(code.GetType()), reason,
	             code.GetHash0(), code.GetCrc32(), code.IsVsEmbedded() ? 1u : 0u, code.IsPsEmbedded() ? 1u : 0u,
	             code.GetVsEmbeddedId(), code.GetPsEmbeddedId(), code.GetInstructions().Size(), bind.storage_buffers.buffers_num,
	             ShaderCodeHasStorageBufferWrites(code) ? 1u : 0u, counts[0], first_pcs[0], first_types[0], first_registers[0],
	             counts[1], first_pcs[1], first_types[1], first_registers[1], counts[2], first_pcs[2], first_types[2], first_registers[2]);
}

static bool ShaderStorageNoStoreProof(const ShaderCode& code, ShaderType stage)
{
	// Embedded modules do not populate this IR. No proof may be inferred
	// from their empty parser object or from an unknown/mismatched stage.
	return (stage == ShaderType::Vertex || stage == ShaderType::Pixel || stage == ShaderType::Compute) &&
	       code.GetType() == stage && !code.IsVsEmbedded() && !code.IsPsEmbedded() && !code.GetInstructions().IsEmpty() &&
	       !ShaderCodeHasStorageBufferWrites(code);
}

static void TraceShaderStorageWriteAuthority(ShaderType stage, const ShaderStorageWriteAuthority& authority)
{
	if (!authority.complete || authority.write_mask == 0 || !TakeStorageNoStoreDiagnosticRecord()) return;
	std::fprintf(stderr, "[GFX_STORAGE_WRITE_AUTHORITY] stage=%u no_store_mask=%04" PRIx32
	                     " write_mask=%04" PRIx32 " complete=1 policy=immutable-direct-store-index\n",
	             static_cast<unsigned>(stage), authority.no_store_mask, authority.write_mask);
}

static bool ShaderStorageBindingNoStoreProof(const ShaderStorageWriteAuthority& authority, int binding)
{
	return authority.complete && (authority.no_store_mask & authority.write_mask) == 0 && binding >= 0 && binding < ShaderStorageResources::BUFFERS_MAX &&
	       (authority.no_store_mask & (1u << static_cast<unsigned>(binding))) != 0;
}

static bool ShaderStorageBindingWriteProof(const ShaderStorageWriteAuthority& authority, int binding)
{
	return authority.complete && (authority.no_store_mask & authority.write_mask) == 0 && binding >= 0 &&
	       binding < ShaderStorageResources::BUFFERS_MAX && (authority.write_mask & (1u << static_cast<unsigned>(binding))) != 0;
}

class PipelineCache
{
public:
	PipelineCache() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~PipelineCache() { LUMEN_NOT_IMPLEMENTED; }
	LUMEN_CLASS_NO_COPY(PipelineCache);

	VulkanPipeline* CreatePipeline(VulkanFramebuffer* framebuffer, RenderColorInfo* color, RenderDepthInfo* depth,
	                               ShaderVertexInputInfo* vs_input_info, HW::Context* ctx, HW::Shader* sh_ctx,
	                               ShaderPixelInputInfo* ps_input_info, VkPrimitiveTopology topology);
	VulkanPipeline* CreatePipeline(ShaderComputeInputInfo* input_info, const HW::ComputeShaderInfo* cs_regs,
	                               const HW::ShaderRegisters* sh_regs);
	void            DeletePipeline(VulkanPipeline* pipeline);
	void            DeletePipelines(VulkanFramebuffer* framebuffer);
	void            DeleteAllPipelines();

private:
#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
	friend bool GraphicsRenderPipelineCacheTombstoneContractTest();
#endif
#if defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	friend bool GraphicsRenderDynamicScissorContractTest();
	friend bool GraphicsRenderStencilClearContractTest();
	friend bool GraphicsRenderPipelineCacheGrowthContractTest();
	friend bool GraphicsRenderRt0ComponentMaskContractTest();
	friend bool GraphicsRenderPolygonOffsetContractTest();
	friend bool GraphicsRenderBlendConstantContractTest();
#endif

	// Older builds stopped at this count even though Vector storage and the
	// heap-owned VulkanPipeline objects can grow independently. Keep the value
	// only as a one-shot diagnostic threshold, not as a runtime capacity.
	static constexpr uint32_t LEGACY_PIPELINE_LIMIT = 128;

	struct Pipeline
	{
		uint64_t                   render_pass_id = 0;
		ShaderId                   vs_shader_id;
		ShaderId                   ps_shader_id;
		ShaderId                   cs_shader_id;
		VulkanPipeline*            pipeline       = nullptr;
		PipelineStaticParameters*  static_params  = nullptr;
		PipelineDynamicParameters* dynamic_params = nullptr;
	};

	[[nodiscard]] VulkanPipeline* Find(const Pipeline& p) const;
	[[nodiscard]] static VulkanPipeline* FindIn(const Vector<Pipeline>& pipelines, const Pipeline& p);
	[[nodiscard]] static uint32_t StoreIn(Vector<Pipeline>* pipelines, const Pipeline& p);

	void DeletePipelineInternal(uint32_t id);

	void DumpToFile(Core::File* f, const Pipeline& p);
	void DumpPipeline(const char* action, uint32_t id);

	Vector<Pipeline> m_pipelines;
	Core::Mutex      m_mutex;
};

struct VulkanDescriptorSet
{
	VkDescriptorSet       set     = nullptr;
	VkDescriptorSetLayout layout  = nullptr;
	int                   pool_id = -1;
};

class DescriptorCache;

struct DescriptorRecordingBatch
{
	struct ConstantPool { VkDescriptorPool pool = nullptr; std::vector<VkDescriptorSet> sets; };
	struct ConstantBuffer
	{
		VulkanBuffer* buffer = nullptr;
		void* mapped = nullptr;
		VkDeviceSize used = 0;
	};
	DescriptorCache* cache = nullptr;
	CommandBuffer* command = nullptr;
	std::vector<int> slots;
	// Native constants are immutable per draw and owned by this recording.
	std::vector<ConstantPool> constant_pools;
	std::vector<ConstantBuffer> constant_buffers;
	bool active = false;
};

constexpr VkDeviceSize CONSTANT_ARENA_BYTES = 64u * 1024u;

static bool ConstantArenaOffset(VkDeviceSize used, VkDeviceSize bytes, VkDeviceSize alignment, VkDeviceSize* offset)
{
	if (offset == nullptr || alignment == 0 || bytes == 0 || bytes > CONSTANT_ARENA_BYTES ||
	    used > UINT64_MAX - (alignment - 1)) return false;
	const auto next = ((used + alignment - 1) / alignment) * alignment;
	if (next > CONSTANT_ARENA_BYTES - bytes) return false;
	*offset = next;
	return true;
}

template <typename ReleasePool, typename ReleaseBuffer>
static void ReleaseConstantResources(DescriptorRecordingBatch* batch, ReleasePool&& release_pool, ReleaseBuffer&& release_buffer)
{
	for (auto& pool: batch->constant_pools) release_pool(pool);
	for (auto& arena: batch->constant_buffers) release_buffer(arena);
	batch->constant_pools.clear();
	batch->constant_buffers.clear();
}

class DescriptorCache
{
public:
	enum class Stage
	{
		Unknown,
		Vertex,
		Pixel,
		Compute
	};

	static constexpr int BUFFERS_MAX          = ShaderStorageResources::BUFFERS_MAX;
	static constexpr int TEXTURES_SAMPLED_MAX = ShaderTextureResources::RES_MAX;
	static constexpr int TEXTURES_STORAGE_MAX = ShaderTextureResources::RES_MAX;
	static constexpr int SAMPLERS_MAX         = ShaderSamplerResources::RES_MAX;
	static constexpr int PUSH_CONSTANTS_MAX   = 16 * 4;
	static constexpr int GDS_BUFFER_MAX       = 1;

	DescriptorCache() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~DescriptorCache() { LUMEN_NOT_IMPLEMENTED; }
	LUMEN_CLASS_NO_COPY(DescriptorCache);

	VkDescriptorSetLayout GetDescriptorSetLayout(Stage stage, const ShaderBindResources& bind);
	VkDescriptorSetLayout GetConstantDescriptorSetLayout(VkShaderStageFlags stages);
	VkDescriptorSetLayout GetEmptyDescriptorSetLayout();
	VkDescriptorSet GetConstantDescriptor(DescriptorRecordingBatch* batch, VkShaderStageFlags stages,
	                                      const std::vector<uint32_t>& words, VkDescriptorBufferInfo* captured = nullptr);

	VulkanDescriptorSet* Allocate(Stage stage, int storage_buffers_num, int textures2d_sampled_num, int textures2d_storage_num,
	                              int samplers_num, int gds_buffers_num);
	void                 Free(VulkanDescriptorSet* set);

	DescriptorRecordingBatch* CreateBatch(CommandBuffer* command);
	void ReleaseBatch(DescriptorRecordingBatch* batch);
	VulkanDescriptorSet* GetDescriptor(DescriptorRecordingBatch* batch, Stage stage, VulkanBuffer** storage_buffers,
	                                   VulkanImage** textures2d_sampled,
	                                   const int* textures2d_sampled_view, VulkanImage** textures2d_storage, uint64_t* samplers,
	                                   VulkanBuffer** gds_buffers, const ShaderBindResources& bind);
	void                 FreeDescriptor(VulkanBuffer* buffer);
	void                 FreeDescriptor(VulkanImage* image);
	void                 ResetForNextSession();

private:
	struct Set
	{
		VulkanDescriptorSet* set                                         = nullptr;
		int                  next_free_set                               = -1;
		uint32_t             hash                                        = 0;
		Stage                stage                                       = Stage::Unknown;
		int                  storage_buffers_num                         = 0;
		uint64_t             storage_buffers_id[BUFFERS_MAX]             = {};
		int                  textures2d_sampled_num                      = 0;
		uint64_t             textures2d_sampled_id[TEXTURES_SAMPLED_MAX] = {};
		int                  textures2d_sampled_view[TEXTURES_SAMPLED_MAX] = {};
		int                  textures2d_storage_num                      = 0;
		uint64_t             textures2d_storage_id[TEXTURES_STORAGE_MAX] = {};
		int                  samplers_num                                = 0;
		uint64_t             samplers_id[SAMPLERS_MAX]                   = {};
		int                  gds_buffers_num                             = 0;
		uint64_t             gds_buffers_id[GDS_BUFFER_MAX]              = {};
		DescriptorRecordingBatch* owner = nullptr;
		bool cached = false;
	};

	// Invalidated entries retain their slot and Vulkan handle until the owning
	// recording retires. This prevents both premature frees and slot ABA reuse.
	class SetTable
	{
	public:
		VulkanDescriptorSet* FindSet(const Set& s) const;
		void BeginBatch(DescriptorRecordingBatch* batch)
		{
			EXIT_IF(batch == nullptr || batch->active || !batch->slots.empty());
			batch->active = true;
			++active_batches;
		}
		int Add(Set set)
		{
			EXIT_IF(set.set == nullptr || set.owner == nullptr || !set.owner->active);
			set.cached = true;
			int index = m_first_free_set;
			if (index != -1)
			{
				EXIT_IF(m_sets.At(index).set != nullptr);
				m_first_free_set = m_sets.At(index).next_free_set;
				m_sets[index] = set;
			} else
			{
				index = static_cast<int>(m_sets.Size());
				m_sets.Add(set);
			}
			m_sets_map[set.hash].Add(index);
			set.owner->slots.push_back(index);
			++active_sets;
			peak_active_sets = std::max(peak_active_sets, active_sets);
			return index;
		}
		void InvalidateBuffer(uint64_t id);
		void InvalidateImage(uint64_t id);
		template <typename Release>
		void ReleaseBatch(DescriptorRecordingBatch* batch, Release release)
		{
			EXIT_IF(batch == nullptr || !batch->active || active_batches == 0);
			for (int index: batch->slots)
			{
				EXIT_IF(!m_sets.IndexValid(index));
				auto& set = m_sets[index];
				EXIT_IF(set.set == nullptr || set.owner != batch || active_sets == 0);
				InvalidateSet(index);
				release(set.set);
				set.set = nullptr;
				set.owner = nullptr;
				set.next_free_set = m_first_free_set;
				m_first_free_set = index;
				--active_sets;
				++released_total;
			}
			batch->slots.clear();
			batch->active = false;
			--active_batches;
			++completed_batches;
		}
		bool Clear()
		{
			if (active_batches != 0 || active_sets != 0) { return false; }
			m_sets.Clear();
			m_sets_map.Clear();
			m_first_free_set = -1;
			return true;
		}
		uint32_t HistoricalSlots() const { return m_sets.Size(); }
		uint32_t Capacity() const { return m_sets.Capacity(); }
		uint32_t active_sets = 0;
		uint32_t peak_active_sets = 0;
		uint32_t active_batches = 0;
		uint64_t completed_batches = 0;
		uint64_t released_total = 0;
	private:
		void InvalidateSet(int index)
		{
			auto& set = m_sets[index];
			if (!set.cached) { return; }
			EXIT_IF(set.set == nullptr || !m_sets_map.Contains(set.hash));
			auto& ids = m_sets_map[set.hash];
			ids.Remove(index);
			if (ids.IsEmpty()) { m_sets_map.Remove(set.hash); }
			set.cached = false;
		}
		Vector<Set> m_sets;
		int m_first_free_set = -1;
		Core::Hashmap<uint32_t, Vector<int>> m_sets_map;
	};

	struct Pool
	{
		VkDescriptorPool pool           = nullptr;
		int              next_free_pool = -1;
		bool             free           = true;
	};
	static void MarkPoolAvailable(Pool& pool, int pool_id, int& first_free_pool)
	{
		if (!pool.free)
		{
			pool.free = true;
			pool.next_free_pool = first_free_pool;
			first_free_pool = pool_id;
		}
	}

	void Init();
	void CreatePool();
	VkDescriptorSetLayout GetOrCreateDescriptorSetLayout(Stage stage, int storage_buffers_num, int textures2d_sampled_num,
	                                                     int textures2d_storage_num, int samplers_num, int gds_buffers_num);

	static uint32_t CalcHash(const Set& s);

	Core::Mutex  m_mutex;
	Vector<Pool> m_pools;
	SetTable m_sets;
	int          m_first_free_pool = -1;
#if defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	friend bool GraphicsRenderDescriptorRecordingContractTest();
#endif

	VkDescriptorSetLayout m_descriptor_set_layout_vertex[BUFFERS_MAX + 1][TEXTURES_SAMPLED_MAX + 1][TEXTURES_STORAGE_MAX + 1]
	                                                    [SAMPLERS_MAX + 1][GDS_BUFFER_MAX + 1] = {};
	VkDescriptorSetLayout m_descriptor_set_layout_pixel[BUFFERS_MAX + 1][TEXTURES_SAMPLED_MAX + 1][TEXTURES_STORAGE_MAX + 1]
	                                                   [SAMPLERS_MAX + 1][GDS_BUFFER_MAX + 1] = {};
	VkDescriptorSetLayout m_descriptor_set_layout_compute[BUFFERS_MAX + 1][TEXTURES_SAMPLED_MAX + 1][TEXTURES_STORAGE_MAX + 1]
	                                                     [SAMPLERS_MAX + 1][GDS_BUFFER_MAX + 1] = {};
	bool m_initialized                                                                          = false;
	VkDescriptorSetLayout m_constant_graphics_layout = nullptr;
	VkDescriptorSetLayout m_constant_compute_layout = nullptr;
	VkDescriptorSetLayout m_empty_layout = nullptr;
	std::vector<DescriptorRecordingBatch::ConstantPool> m_free_constant_pools;
	std::vector<DescriptorRecordingBatch::ConstantBuffer> m_free_constant_buffers;
};

class SamplerCache
{
public:
	SamplerCache() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~SamplerCache() { LUMEN_NOT_IMPLEMENTED; }
	LUMEN_CLASS_NO_COPY(SamplerCache);

	VkSampler GetSampler(uint64_t id);
	uint64_t  GetSamplerId(const ShaderSamplerResource& r);

private:
	struct Sampler
	{
		ShaderSamplerResource r;
		VkSampler             vk = nullptr;
	};

	Core::Mutex     m_mutex;
	Vector<Sampler> m_samplers;
};

struct VulkanFramebuffer
{
	VkRenderPass  render_pass    = nullptr;
	uint64_t      render_pass_id = 0;
	VkFramebuffer framebuffer    = nullptr;
};

// Native Vulkan can retain compatible LOAD/STORE passes just as the Metal
// adapter retains its encoder. Keep a separate logical state: transfers are
// legal after EndRenderPass and close the retained physical pass explicitly.
[[maybe_unused]] static bool NativeRenderPassReuseOverride(const char* value)
{
	return value == nullptr || std::strcmp(value, "1") == 0;
}

static bool NativeRenderPassReuseEnabled()
{
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && LUMEN_ABI == LUMEN_ABI_ARM64_V8A)
	static const bool enabled = NativeRenderPassReuseOverride(std::getenv("LUMEN_VULKAN_RENDER_PASS_REUSE"));
	return enabled;
#else
	return false; // NativeMetal already has its own physical encoder retention.
#endif
}

static bool RenderPassReuseTraceEnabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_VULKAN_RENDER_PASS_REUSE_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

struct RenderPassReuseCounters
{
	uint64_t logical = 0, physical = 0, merged = 0, closed = 0, ineligible = 0;
};
static thread_local RenderPassReuseCounters g_render_pass_reuse_counters;

static void RecordRenderPassBegin(bool reused, bool eligible)
{
	if (!RenderPassReuseTraceEnabled()) return;
	auto& counts = g_render_pass_reuse_counters;
	++counts.logical;
	if (reused) ++counts.merged; else ++counts.physical;
	if (!eligible) ++counts.ineligible;
	if (counts.logical == 1 || counts.logical % 4096 == 0)
		std::fprintf(stderr, "[GFX_RENDER_PASS_REUSE] enabled=%u logical=%" PRIu64
		                     " physical=%" PRIu64 " merged=%" PRIu64 " closed=%" PRIu64
		                     " ineligible=%" PRIu64 " scope=recording-thread\n",
		             NativeRenderPassReuseEnabled() ? 1u : 0u, counts.logical, counts.physical,
		             counts.merged, counts.closed, counts.ineligible);
}

// Preserve the existing attachment dependency at each physical pass boundary.
// Compatible draws inside one subpass use primitive order for fixed-function
// color/depth/stencil attachment operations. This is not a general ordering
// guarantee for shader memory: reuse separately excludes its writes and aliases.
static VkSubpassDependency RenderPassAttachmentDependency()
{
	VkSubpassDependency dependency {};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
	                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	dependency.dstStageMask = dependency.srcStageMask;
	dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
	                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
	return dependency;
}

struct RenderPassBatchState
{
	uint64_t framebuffer_id = 0;
	uint32_t width = 0, height = 0;
	bool physical = false, logical = false, reusable = false;

	bool CanReuse(uint64_t id, uint32_t w, uint32_t h, bool eligible) const
	{
		return eligible && id != 0 && w != 0 && h != 0 && physical && !logical && reusable &&
		       framebuffer_id == id && width == w && height == h;
	}

	template <typename Close>
	bool Outside(Close&& close)
	{
		if (logical) return false;
		if (physical) close();
		Reset();
		return true;
	}

	template <typename Close, typename Open>
	bool Begin(uint64_t id, uint32_t w, uint32_t h, bool eligible, Close&& close, Open&& open)
	{
		if (logical || id == 0 || w == 0 || h == 0) return false;
		const bool reuse = CanReuse(id, w, h, eligible);
		if (!reuse)
		{
			if (!Outside(close)) return false;
			open();
		}
		framebuffer_id = id;
		width = w;
		height = h;
		physical = logical = true;
		reusable = eligible;
		return true;
	}

	template <typename Close>
	bool End(Close&& close)
	{
		if (!logical || !physical) return false;
		logical = false;
		return reusable || Outside(close);
	}

	void Reset()
	{
		framebuffer_id = 0;
		width = height = 0;
		physical = logical = reusable = false;
	}
};


class FramebufferCache
{
public:
	FramebufferCache() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~FramebufferCache() { LUMEN_NOT_IMPLEMENTED; }
	LUMEN_CLASS_NO_COPY(FramebufferCache);

	VulkanFramebuffer* CreateFramebuffer(RenderColorInfo* color, RenderDepthInfo* depth);
	void               FreeFramebufferByColor(VulkanImage* image);
	void               FreeFramebufferByDepth(DepthStencilVulkanImage* image);
	void               ResetForNextSession();

private:
#if defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	friend bool GraphicsRenderFramebufferCacheReleaseAllContractTest();
#endif

	VideoOutVulkanImage* CreateDummyBuffer(VkFormat format, uint32_t width, uint32_t height);

	struct Framebuffer
	{
		VulkanFramebuffer* framebuffer          = nullptr;
		uint64_t           image_id             = 0;
		uint64_t           depth_id             = 0;
		bool               depth_clear_enable   = false;
		bool               stencil_clear_enable = false;
	};

	template <typename Predicate, typename Release>
	static uint32_t ReleaseMatching(Vector<Framebuffer>* framebuffers, Predicate predicate, Release release)
	{
		EXIT_IF(framebuffers == nullptr);
		uint32_t released = 0;
		for (auto& entry: *framebuffers)
		{
			if (entry.framebuffer == nullptr || !predicate(entry))
			{
				continue;
			}

			auto* framebuffer = entry.framebuffer;
			release(framebuffer);
			entry.framebuffer = nullptr;
			++released;
		}
		return released;
	}

	void FreeFramebuffersMatchingLocked(uint64_t image_id, bool match_depth);

	Core::Mutex                  m_mutex;
	Vector<Framebuffer>          m_framebuffers;
	Vector<VideoOutVulkanImage*> m_dummy_buffers;
};

class GdsBuffer
{
public:
	GdsBuffer() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~GdsBuffer() { LUMEN_NOT_IMPLEMENTED; }
	LUMEN_CLASS_NO_COPY(GdsBuffer);

	void Clear(GraphicContext* ctx, uint64_t dw_offset, uint32_t dw_num, uint32_t clear_value);
	void Read(GraphicContext* ctx, uint32_t* dst, uint32_t dw_offset, uint32_t dw_size);
	void ResetForNextSession(GraphicContext* ctx);

	VulkanBuffer* GetBuffer(GraphicContext* ctx);

static constexpr uint64_t DW_SIZE = LabelGpuObject::MAX_WRITE_BYTES / sizeof(uint32_t);

private:

	void Init(GraphicContext* ctx);

	Core::Mutex   m_mutex;
	VulkanBuffer* m_buffer = nullptr;
};

constexpr uint32_t QUAD_LIST_VERTICES_PER_QUAD = 4;
constexpr uint32_t QUAD_LIST_INDICES_PER_QUAD  = 6;
constexpr uint32_t QUAD_LIST_INDEX_PATTERN[QUAD_LIST_INDICES_PER_QUAD] = {0, 1, 2, 0, 2, 3};

enum class AutoIndexPrimitiveMode
{
	Direct,
	RectList,
	QuadList,
};

struct AutoIndexPrimitiveProfile
{
	VkPrimitiveTopology    topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	AutoIndexPrimitiveMode mode     = AutoIndexPrimitiveMode::Direct;
};

static bool ResolveAutoIndexPrimitive(uint32_t primitive, AutoIndexPrimitiveProfile* profile)
{
	if (profile == nullptr)
	{
		return false;
	}
	switch (primitive)
	{
		case 2: *profile = {VK_PRIMITIVE_TOPOLOGY_LINE_LIST, AutoIndexPrimitiveMode::Direct}; return true;
		case 3: *profile = {VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, AutoIndexPrimitiveMode::Direct}; return true;
		case 4: *profile = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, AutoIndexPrimitiveMode::Direct}; return true;
		case 5: *profile = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, AutoIndexPrimitiveMode::Direct}; return true;
		case 6: *profile = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, AutoIndexPrimitiveMode::Direct}; return true;
		case 17: *profile = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, AutoIndexPrimitiveMode::RectList}; return true;
		case 19: *profile = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, AutoIndexPrimitiveMode::QuadList}; return true;
		default: return false;
	}
}

#if defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
bool GraphicsRenderAutoIndexPrimitiveContractTest()
{
	AutoIndexPrimitiveProfile profile;
	if (ResolveAutoIndexPrimitive(2, &profile) == false || profile.topology != VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
	    profile.mode != AutoIndexPrimitiveMode::Direct || ResolveAutoIndexPrimitive(3, &profile) == false ||
	    profile.topology != VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
	    profile.mode != AutoIndexPrimitiveMode::Direct || ResolveAutoIndexPrimitive(4, &profile) == false ||
	    profile.topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
	    profile.mode != AutoIndexPrimitiveMode::Direct || ResolveAutoIndexPrimitive(5, &profile) == false ||
	    profile.topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN || profile.mode != AutoIndexPrimitiveMode::Direct ||
	    ResolveAutoIndexPrimitive(6, &profile) == false || profile.topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
	    profile.mode != AutoIndexPrimitiveMode::Direct ||
	    ResolveAutoIndexPrimitive(17, &profile) == false || profile.topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
	    profile.mode != AutoIndexPrimitiveMode::RectList || ResolveAutoIndexPrimitive(19, &profile) == false ||
	    profile.topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || profile.mode != AutoIndexPrimitiveMode::QuadList)
	{
		return false;
	}
	const auto unchanged = profile;
	return !ResolveAutoIndexPrimitive(2, nullptr) && !ResolveAutoIndexPrimitive(3, nullptr) &&
	       !ResolveAutoIndexPrimitive(5, nullptr) && !ResolveAutoIndexPrimitive(6, nullptr) &&
	       !ResolveAutoIndexPrimitive(0, &profile) && profile.topology == unchanged.topology &&
	       profile.mode == unchanged.mode && !ResolveAutoIndexPrimitive(7, &profile) && profile.topology == unchanged.topology &&
	       profile.mode == unchanged.mode && !ResolveAutoIndexPrimitive(63, &profile) && profile.topology == unchanged.topology &&
	       profile.mode == unchanged.mode;
}
#endif

constexpr uint64_t GetQuadListIndexValue(uint64_t index)
{
	return (index / QUAD_LIST_INDICES_PER_QUAD) * QUAD_LIST_VERTICES_PER_QUAD +
	       QUAD_LIST_INDEX_PATTERN[index % QUAD_LIST_INDICES_PER_QUAD];
}

constexpr uint64_t GetQuadListIndexCount(uint32_t vertex_count)
{
	return (static_cast<uint64_t>(vertex_count) / QUAD_LIST_VERTICES_PER_QUAD) * QUAD_LIST_INDICES_PER_QUAD;
}

static_assert(GetQuadListIndexCount(4) == 6);
static_assert(GetQuadListIndexCount(8) == 12);
static_assert(GetQuadListIndexValue(0) == 0 && GetQuadListIndexValue(1) == 1 && GetQuadListIndexValue(2) == 2);
static_assert(GetQuadListIndexValue(3) == 0 && GetQuadListIndexValue(4) == 2 && GetQuadListIndexValue(5) == 3);
static_assert(GetQuadListIndexValue(6) == 4 && GetQuadListIndexValue(7) == 5 && GetQuadListIndexValue(8) == 6);
static_assert(GetQuadListIndexValue(9) == 4 && GetQuadListIndexValue(10) == 6 && GetQuadListIndexValue(11) == 7);
static_assert(GetQuadListIndexValue(QUAD_LIST_INDICES_PER_QUAD * 16384ULL - 1) == std::numeric_limits<uint16_t>::max());

class QuadListIndexBufferCache
{
public:
	QuadListIndexBufferCache() = default;
	virtual ~QuadListIndexBufferCache() { LUMEN_NOT_IMPLEMENTED; }
	LUMEN_CLASS_NO_COPY(QuadListIndexBufferCache);

	VulkanBuffer* GetBuffer(GraphicContext* ctx, uint32_t vertex_count, VkIndexType* index_type, uint32_t* draw_index_count);

private:
	struct Slot
	{
		VulkanBuffer* buffer        = nullptr;
		uint32_t      quad_capacity = 0;
	};

	static uint32_t CalcCapacity(uint32_t current, uint32_t required);

	template <typename IndexT>
	static VulkanBuffer* CreateBuffer(GraphicContext* ctx, uint32_t quad_capacity);

	Core::Mutex          m_mutex;
	Slot                 m_uint16;
	Slot                 m_uint32;
	// A per-queue reset cannot prove that another queue has finished with an
	// older generation, so all geometrically bounded generations live until process exit.
	Vector<VulkanBuffer*> m_buffers;
};

class RenderContext
{
public:
	RenderContext()
	    : m_pipeline_cache(new PipelineCache), m_descriptor_cache(new DescriptorCache), m_framebuffer_cache(new FramebufferCache),
	      m_sampler_cache(new SamplerCache), m_gds_buffer(new GdsBuffer), m_quad_list_index_buffer_cache(new QuadListIndexBufferCache)
	{
		EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());
	}
	virtual ~RenderContext() { LUMEN_NOT_IMPLEMENTED; }
	LUMEN_CLASS_NO_COPY(RenderContext);

	void SetGraphicCtx(GraphicContext* ctx)
	{
		EXIT_IF(ctx == nullptr || ctx->physical_device == nullptr);
		m_graphic_ctx = ctx;
		VkPhysicalDeviceProperties properties {};
		vkGetPhysicalDeviceProperties(ctx->physical_device, &properties);
		m_device_limits = properties.limits;
	}
	GraphicContext* GetGraphicCtx() { return m_graphic_ctx; }
	const VkPhysicalDeviceLimits& GetDeviceLimits() const { return m_device_limits; }

	Core::Mutex&      GetMutex() { return m_mutex; }
	PipelineCache*    GetPipelineCache() { return m_pipeline_cache; }
	DescriptorCache*  GetDescriptorCache() { return m_descriptor_cache; }
	FramebufferCache* GetFramebufferCache() { return m_framebuffer_cache; }
	SamplerCache*     GetSamplerCache() { return m_sampler_cache; }
	GdsBuffer*        GetGdsBuffer() { return m_gds_buffer; }
	QuadListIndexBufferCache* GetQuadListIndexBufferCache() { return m_quad_list_index_buffer_cache; }
	TextureVulkanImage* GetNullSampledImage();
	bool                HasNullSampledImage();

	void AddEqEvent(const LibKernel::EventQueue::KernelEqueueIdentity& identity, int id);
	void DeleteEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id, uint64_t generation = 0);
	void TriggerEvent(int id, uint64_t data);
	void TriggerEopEvent();
	bool ResetForNextSession();

private:
	Core::Mutex       m_mutex;
	PipelineCache*    m_pipeline_cache    = nullptr;
	DescriptorCache*  m_descriptor_cache  = nullptr;
	FramebufferCache* m_framebuffer_cache = nullptr;
	SamplerCache*     m_sampler_cache     = nullptr;
	GraphicContext*   m_graphic_ctx       = nullptr;
	VkPhysicalDeviceLimits m_device_limits {};
	GdsBuffer*        m_gds_buffer        = nullptr;
	QuadListIndexBufferCache* m_quad_list_index_buffer_cache = nullptr;
	Core::Mutex         m_null_sampled_image_mutex;
	TextureVulkanImage* m_null_sampled_image = nullptr;

	struct EventRegistration
	{
		LibKernel::EventQueue::KernelEqueueIdentity identity;
		int id = 0;
	};
	Core::Mutex m_event_mutex;
	std::vector<EventRegistration> m_event_registrations;
};

struct RenderDepthInfo
{
	VkFormat                    format                   = VK_FORMAT_UNDEFINED;
	uint32_t                    width                    = 0;
	uint32_t                    height                   = 0;
	bool                        htile                    = false;
	bool                        neo                      = false;
	uint64_t                    depth_buffer_size        = 0;
	uint64_t                    depth_buffer_vaddr       = 0;
	uint64_t                    depth_tile_swizzle       = 0;
	uint64_t                    stencil_buffer_size      = 0;
	uint64_t                    stencil_buffer_vaddr     = 0;
	uint64_t                    stencil_tile_swizzle     = 0;
	uint64_t                    htile_buffer_size        = 0;
	uint64_t                    htile_buffer_vaddr       = 0;
	uint64_t                    htile_tile_swizzle       = 0;
	bool                        depth_clear_enable       = false;
	float                       depth_clear_value        = 0.0f;
	bool                        depth_test_enable        = false;
	bool                        depth_write_enable       = false;
	VkCompareOp                 depth_compare_op         = VK_COMPARE_OP_NEVER;
	bool                        depth_bounds_test_enable = false;
	float                       depth_min_bounds         = 0.0f;
	float                       depth_max_bounds         = 0.0f;
	bool                        stencil_clear_enable     = false;
	uint8_t                     stencil_clear_value      = 0;
	bool                        stencil_test_enable      = false;
	bool                        uniform_depth_read_only  = false;
	PipelineStencilStaticState  stencil_static_front;
	PipelineStencilStaticState  stencil_static_back;
	PipelineStencilDynamicState stencil_dynamic_front;
	PipelineStencilDynamicState stencil_dynamic_back;
	DepthStencilVulkanImage*    vulkan_buffer = nullptr;
	uint64_t                    vaddr[3]      = {};
	uint64_t                    size[3]       = {};
	int                         vaddr_num     = 0;
};

static bool ResolveRenderStencilState(const HW::DepthControl& control, const HW::StencilControl& stencil,
                                     const HW::StencilMask& mask, RenderDepthInfo* depth);
static bool UniformDepthZeroReadOnlyDraw(const RenderDepthInfo& depth, const VulkanPipeline* pipeline);

static void SetPipelineStencilDynamicState(PipelineDynamicParameters* parameters, const RenderDepthInfo& depth)
{
	parameters->stencil_front = depth.stencil_dynamic_front;
	parameters->stencil_back  = depth.stencil_dynamic_back;
	// The attachment load operation already performs the requested clear.
	// Preserve it through the triggering draw, as for depth_write_enable.
	if (depth.stencil_clear_enable)
	{
		parameters->stencil_front.writeMask = 0;
		parameters->stencil_back.writeMask  = 0;
	}
}

enum class RenderColorType
{
	NoColorOutput,
	DisplayBuffer,
	// OffscreenBuffer,
	RenderTexture,
};

struct RenderColorInfo
{
	RenderColorType       type          = RenderColorType::NoColorOutput;
	VulkanImage*          vulkan_buffer = nullptr;
	GpuMemoryObjectHandle tracked_handle;
	uint64_t              base_addr     = 0;
	uint64_t              buffer_size   = 0;
};

static bool claim_command_pool_slot(VulkanCommandPool* pool, uint32_t* index)
{
	if (pool == nullptr || index == nullptr) return false;
	Core::LockGuard lock(pool->mutex);
	if (pool->busy == nullptr) return false;
	for (uint32_t slot = 0; slot < pool->buffers_count; ++slot)
	{
		if (!pool->busy[slot])
		{
			pool->busy[slot] = true;
			*index = slot;
			return true;
		}
	}
	return false;
}

template <typename Create>
static VulkanCommandPool* get_command_pool_with_slot(std::vector<VulkanCommandPool*>* chunks, uint32_t* index, Create&& create)
{
	EXIT_IF(chunks == nullptr || index == nullptr);
	*index = UINT32_MAX;
	for (auto* pool: *chunks)
		if (claim_command_pool_slot(pool, index)) return pool;
	// A processor can occupy every persistent slot while a handoff export
	// needs a transient buffer on the same native queue. Allocate another stable
	// chunk, preserving every existing pool pointer, handle array and slot index.
	auto* pool = create();
	EXIT_IF(pool == nullptr || !claim_command_pool_slot(pool, index));
	chunks->push_back(pool);
	return pool;
}

class CommandPool
{
public:
	CommandPool() = default;
	~CommandPool() // NOLINT
	{
		// TODO(): check if destructor is called from std::_Exit()
		// DeleteAll();
	}

	LUMEN_CLASS_NO_COPY(CommandPool);

	VulkanCommandPool* GetPool(int id, uint32_t* index)
	{
		EXIT_IF(id < 0 || id >= GraphicContext::QUEUES_NUM);
		return get_command_pool_with_slot(&m_pool[id], index, [&] { return Create(id); });
	}

private:
	VulkanCommandPool* Create(int id);
	void DeleteAll();

	std::vector<VulkanCommandPool*> m_pool[GraphicContext::QUEUES_NUM];
};

static RenderContext*           g_render_ctx = nullptr;
static thread_local CommandPool g_command_pool;
static std::atomic_uint64_t      g_null_sampled_image_prepared = 0;
static bool                      g_null_sampled_image_enabled  = false;

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
static bool command_pool_chunks_contract_test()
{
	constexpr uint32_t slots_per_pool = VulkanCommandPool::PROCESSOR_BUFFER_COUNT;
	struct Chunk
	{
		VulkanCommandPool pool;
		std::array<VkCommandBuffer, slots_per_pool> buffers;
		std::array<VkFence, slots_per_pool> fences;
		std::array<VkSemaphore, slots_per_pool> semaphores;
		std::array<bool, slots_per_pool> busy {};
	};
	std::vector<std::unique_ptr<Chunk>> owned;
	std::vector<VulkanCommandPool*> graphics, compute;
	const auto create = [&]() {
		auto chunk = std::make_unique<Chunk>();
		const uintptr_t base = 0x1000 + owned.size() * 0x100;
		for (uintptr_t slot = 0; slot < slots_per_pool; ++slot)
		{
			chunk->buffers[slot] = reinterpret_cast<VkCommandBuffer>(base + slot + 1);
			chunk->fences[slot] = reinterpret_cast<VkFence>(base + slot + slots_per_pool + 1);
			chunk->semaphores[slot] = reinterpret_cast<VkSemaphore>(base + slot + slots_per_pool * 2 + 1);
		}
		chunk->pool.pool = reinterpret_cast<VkCommandPool>(base);
		chunk->pool.buffers_count = slots_per_pool; chunk->pool.buffers = chunk->buffers.data();
		chunk->pool.fences = chunk->fences.data(); chunk->pool.semaphores = chunk->semaphores.data();
		chunk->pool.busy = chunk->busy.data();
		auto* pool = &chunk->pool;
		owned.push_back(std::move(chunk));
		return pool;
	};
	std::array<uint32_t, slots_per_pool> processor_slots {};
	VulkanCommandPool* processor = nullptr;
	for (uint32_t slot = 0; slot < slots_per_pool; ++slot)
	{
		auto* selected = get_command_pool_with_slot(&graphics, &processor_slots[slot], create);
		if (slot == 0) processor = selected;
		if (selected != processor || processor_slots[slot] != slot || graphics.size() != 1) return false;
	}
	const auto original_buffers = owned[0]->buffers;
	const auto original_fences = owned[0]->fences;
	const auto original_semaphores = owned[0]->semaphores;
	auto* original_busy = processor->busy;
	uint32_t utility_slot = UINT32_MAX;
	auto* utility = get_command_pool_with_slot(&graphics, &utility_slot, create);
	if (utility == processor || utility_slot != 0 || graphics.size() != 2 || owned.size() != 2) return false;
	{
		Core::LockGuard lock(utility->mutex);
		utility->busy[utility_slot] = false;
	}
	if (get_command_pool_with_slot(&graphics, &utility_slot, create) != utility || utility_slot != 0 || owned.size() != 2)
		return false;
	{
		Core::LockGuard lock(processor->mutex);
		processor->busy[slots_per_pool - 1] = false;
	}
	uint32_t reused = UINT32_MAX;
	if (get_command_pool_with_slot(&graphics, &reused, create) != processor || reused != slots_per_pool - 1 || owned.size() != 2)
		return false;
	// Grow beyond the old fixed capacity without moving any native storage or
	// changing a processor's identity. Every later claim is atomically reserved.
	for (uint32_t borrow = 0; borrow < slots_per_pool * 5; ++borrow)
	{
		uint32_t slot = UINT32_MAX;
		auto* selected = get_command_pool_with_slot(&graphics, &slot, create);
		if (slot >= selected->buffers_count || !selected->busy[slot] ||
		    selected->buffers[slot] == nullptr || selected->fences[slot] == nullptr) return false;
	}
	if (graphics.size() != 7 || processor->buffers != owned[0]->buffers.data() || processor->busy != original_busy ||
	    owned[0]->buffers != original_buffers || owned[0]->fences != original_fences ||
	    owned[0]->semaphores != original_semaphores) return false;
	uint32_t independent_slot = UINT32_MAX;
	auto* independent = get_command_pool_with_slot(&compute, &independent_slot, create);
	if (independent == processor || independent == utility || independent_slot != 0 || compute.size() != 1 || graphics.size() != 7)
		return false;
	uint32_t unchanged = 77;
	if (claim_command_pool_slot(processor, &unchanged) || unchanged != 77 ||
	    claim_command_pool_slot(nullptr, &unchanged) || claim_command_pool_slot(independent, nullptr)) return false;
	// Slot retirement may happen on another native thread. The same pool mutex
	// protects every claim and release, including simultaneous claims.
	{
		Core::LockGuard lock(independent->mutex);
		for (uint32_t slot = 0; slot < slots_per_pool; ++slot) independent->busy[slot] = false;
	}
	std::array<uint32_t, slots_per_pool * 2> claimed;
	claimed.fill(UINT32_MAX);
	std::array<std::thread, slots_per_pool * 2> workers;
	for (size_t worker = 0; worker < workers.size(); ++worker)
		workers[worker] = std::thread([&, worker] { (void)claim_command_pool_slot(independent, &claimed[worker]); });
	for (auto& worker: workers) worker.join();
	std::array<uint32_t, slots_per_pool> claims {};
	for (const auto slot: claimed) if (slot != UINT32_MAX) { if (slot >= slots_per_pool) return false; ++claims[slot]; }
	for (const auto count: claims) if (count != 1) return false;
	std::fprintf(stderr, "[GFX_COMMAND_POOL_CHUNKS_TEST] processor-slots=%u utility-overflow=accepted reuse=existing-slot"
	    " growth=7-stable-chunks queue-isolation=checked handles/arrays/indices=unchanged concurrent-claims=unique"
	    " native-allocation=modeled mutex=real\n", slots_per_pool);
	return true;
}
#endif

static void uc_print(const char* func, const HW::UserConfig& uc)
{
	printf("%s\n", func);

	const auto& ge_cntl = uc.GetGeControl();
	const auto& user_en = uc.GetGeUserVgprEn();

	printf("\t GetPrimType()         = 0x%08" PRIx32 "\n", uc.GetPrimType());
	printf("\t primitive_group_size  = 0x%04" PRIx16 "\n", ge_cntl.primitive_group_size);
	printf("\t vertex_group_size     = 0x%04" PRIx16 "\n", ge_cntl.vertex_group_size);
	printf("\t en_user_vgpr1         = %s\n", user_en.vgpr1 ? "true" : "false");
	printf("\t en_user_vgpr2         = %s\n", user_en.vgpr2 ? "true" : "false");
	printf("\t en_user_vgpr3         = %s\n", user_en.vgpr3 ? "true" : "false");
}

static void uc_check(const HW::UserConfig& uc)
{
	const auto& ge_cntl = uc.GetGeControl();
	const auto& user_en = uc.GetGeUserVgprEn();

	EXIT_NOT_IMPLEMENTED(ge_cntl.primitive_group_size != 0x0000 && ge_cntl.primitive_group_size != 0x0040);
	EXIT_NOT_IMPLEMENTED(ge_cntl.vertex_group_size != 0x0000 && ge_cntl.vertex_group_size != 0x0040);
	EXIT_NOT_IMPLEMENTED(user_en.vgpr1 != false);
	EXIT_NOT_IMPLEMENTED(user_en.vgpr2 != false);
	EXIT_NOT_IMPLEMENTED(user_en.vgpr3 != false);
}

static void sh_print(const char* func, const HW::Shader& /*uc*/)
{
	printf("%s\n", func);
}

static void sh_check(const HW::Shader& /*uc*/) {}

static Core::StringList rt_print(const char* func, const HW::RenderTarget& rt)
{
	Core::StringList dst;

	dst.Add(String::FromPrintf("%s\n", func));

	dst.Add(String::FromPrintf("\t base.addr                       = 0x%016" PRIx64 "\n", rt.base.addr));
	dst.Add(String::FromPrintf("\t pitch.pitch_div8_minus1         = 0x%08" PRIx32 "\n", rt.pitch.pitch_div8_minus1));
	dst.Add(String::FromPrintf("\t pitch.fmask_pitch_div8_minus1   = 0x%08" PRIx32 "\n", rt.pitch.fmask_pitch_div8_minus1));
	dst.Add(String::FromPrintf("\t slice.slice_div64_minus1        = 0x%08" PRIx32 "\n", rt.slice.slice_div64_minus1));
	dst.Add(String::FromPrintf("\t view.base_array_slice_index     = 0x%08" PRIx32 "\n", rt.view.base_array_slice_index));
	dst.Add(String::FromPrintf("\t view.last_array_slice_index     = 0x%08" PRIx32 "\n", rt.view.last_array_slice_index));
	dst.Add(String::FromPrintf("\t view.current_mip_level          = 0x%08" PRIx32 "\n", rt.view.current_mip_level));
	dst.Add(String::FromPrintf("\t info.fmask_compression_enable   = %s\n", rt.info.fmask_compression_enable ? "true" : "false"));

	// dst.Add(String::FromPrintf("\t info.fmask_compression_mode     = 0x%08" PRIx32 "\n", rt.info.fmask_compression_mode));
	dst.Add(String::FromPrintf("\t info.fmask_data_compression_disable = %s\n", rt.info.fmask_data_compression_disable ? "true" : "false"));
	dst.Add(String::FromPrintf("\t info.fmask_one_frag_mode        = %s\n", rt.info.fmask_one_frag_mode ? "true" : "false"));

	dst.Add(String::FromPrintf("\t info.cmask_fast_clear_enable    = %s\n", rt.info.cmask_fast_clear_enable ? "true" : "false"));
	dst.Add(String::FromPrintf("\t info.dcc_compression_enable     = %s\n", rt.info.dcc_compression_enable ? "true" : "false"));
	dst.Add(String::FromPrintf("\t info.neo_mode                   = %s\n", rt.info.neo_mode ? "true" : "false"));
	dst.Add(String::FromPrintf("\t info.cmask_tile_mode            = 0x%08" PRIx32 "\n", rt.info.cmask_tile_mode));
	dst.Add(String::FromPrintf("\t info.cmask_tile_mode_neo        = 0x%08" PRIx32 "\n", rt.info.cmask_tile_mode_neo));
	dst.Add(String::FromPrintf("\t info.format                     = 0x%08" PRIx32 "\n", rt.info.format));
	dst.Add(String::FromPrintf("\t info.channel_type               = 0x%08" PRIx32 "\n", rt.info.channel_type));
	dst.Add(String::FromPrintf("\t info.channel_order              = 0x%08" PRIx32 "\n", rt.info.channel_order));
	dst.Add(String::FromPrintf("\t info.blend_bypa                 = %s\n", rt.info.blend_bypass ? "true" : "false"));
	dst.Add(String::FromPrintf("\t info.blend_clamp                = %s\n", rt.info.blend_clamp ? "true" : "false"));
	dst.Add(String::FromPrintf("\t info.round_mode                 = %s\n", rt.info.round_mode ? "true" : "false"));
	dst.Add(String::FromPrintf("\t attrib.force_dest_alpha_to_one  = %s\n", rt.attrib.force_dest_alpha_to_one ? "true" : "false"));
	dst.Add(String::FromPrintf("\t attrib.tile_mode                = 0x%08" PRIx32 "\n", rt.attrib.tile_mode));
	dst.Add(String::FromPrintf("\t attrib.fmask_tile_mode          = 0x%08" PRIx32 "\n", rt.attrib.fmask_tile_mode));
	dst.Add(String::FromPrintf("\t attrib.num_samples              = 0x%08" PRIx32 "\n", rt.attrib.num_samples));
	dst.Add(String::FromPrintf("\t attrib.num_fragments            = 0x%08" PRIx32 "\n", rt.attrib.num_fragments));
	dst.Add(String::FromPrintf("\t attrib2.width                   = 0x%08" PRIx32 "\n", rt.attrib2.width));
	dst.Add(String::FromPrintf("\t attrib2.height                  = 0x%08" PRIx32 "\n", rt.attrib2.height));
	dst.Add(String::FromPrintf("\t attrib2.num_mip_levels          = 0x%08" PRIx32 "\n", rt.attrib2.num_mip_levels));
	dst.Add(String::FromPrintf("\t attrib3.depth                   = 0x%08" PRIx32 "\n", rt.attrib3.depth));
	dst.Add(String::FromPrintf("\t attrib3.tile_mode               = 0x%08" PRIx32 "\n", rt.attrib3.tile_mode));
	dst.Add(String::FromPrintf("\t attrib3.dimension               = 0x%08" PRIx32 "\n", rt.attrib3.dimension));
	dst.Add(String::FromPrintf("\t attrib3.cmask_pipe_aligned      = %s\n", rt.attrib3.cmask_pipe_aligned ? "true" : "false"));
	dst.Add(String::FromPrintf("\t attrib3.dcc_pipe_aligned        = %s\n", rt.attrib3.dcc_pipe_aligned ? "true" : "false"));
	dst.Add(String::FromPrintf("\t dcc.max_uncompressed_block_size = 0x%08" PRIx32 "\n", rt.dcc.max_uncompressed_block_size));
	dst.Add(String::FromPrintf("\t dcc.max_compressed_block_size   = 0x%08" PRIx32 "\n", rt.dcc.max_compressed_block_size));
	dst.Add(String::FromPrintf("\t dcc.min_compressed_block_size   = 0x%08" PRIx32 "\n", rt.dcc.min_compressed_block_size));
	dst.Add(String::FromPrintf("\t dcc.color_transform             = 0x%08" PRIx32 "\n", rt.dcc.color_transform));
	dst.Add(String::FromPrintf("\t dcc.overwrite_combiner_disable  = %s\n", rt.dcc.overwrite_combiner_disable ? "true" : "false"));
	dst.Add(String::FromPrintf("\t dcc.independent_64b_blocks      = %s\n", rt.dcc.independent_64b_blocks ? "true" : "false"));
	dst.Add(String::FromPrintf("\t dcc.independent_128b_blocks     = %s\n", rt.dcc.independent_128b_blocks ? "true" : "false"));
	dst.Add(String::FromPrintf("\t data_write_on_dcc_clear_to_reg  = %s\n", rt.dcc.data_write_on_dcc_clear_to_reg ? "true" : "false"));
	dst.Add(String::FromPrintf("\t dcc.dcc_clear_key_enable        = %s\n", rt.dcc.dcc_clear_key_enable ? "true" : "false"));
	dst.Add(String::FromPrintf("\t cmask.addr                      = 0x%016" PRIx64 "\n", rt.cmask.addr));
	dst.Add(String::FromPrintf("\t cmask_slice.slice_minus1        = 0x%08" PRIx32 "\n", rt.cmask_slice.slice_minus1));
	dst.Add(String::FromPrintf("\t fmask.addr                      = 0x%016" PRIx64 "\n", rt.fmask.addr));
	dst.Add(String::FromPrintf("\t fmask_slice.slice_minus1        = 0x%08" PRIx32 "\n", rt.fmask_slice.slice_minus1));
	dst.Add(String::FromPrintf("\t clear_word0.word0               = 0x%08" PRIx32 "\n", rt.clear_word0.word0));
	dst.Add(String::FromPrintf("\t clear_word1.word1               = 0x%08" PRIx32 "\n", rt.clear_word1.word1));
	dst.Add(String::FromPrintf("\t dcc_addr.addr                   = 0x%016" PRIx64 "\n", rt.dcc_addr.addr));
	dst.Add(String::FromPrintf("\t size.width                      = 0x%08" PRIx32 "\n", rt.size.width));
	dst.Add(String::FromPrintf("\t size.height                     = 0x%08" PRIx32 "\n", rt.size.height));

	return dst;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void rt_check(const HW::RenderTarget& rt)
{
	if (rt.base.addr != 0)
	{
		// The admitted single-sample CMASK fast-clear profile (CMASK surface,
		// FAST_CLEAR, clear words, unused FMASK alias) does not change how the
		// uncompressed renderer stores the surface. Every other metadata profile
		// stays fail-closed through the shared classifier.
		const auto metadata = HW::ClassifyColorMetadataSlot(rt);
		if (metadata.profile == HW::ColorMetadataProfile::Unsupported)
		{
			EXIT("unsupported color metadata on render target: %s\n%s", metadata.cause,
			     rt_print("RenderTarget metadata rejection", rt).Concat(U"").C_Str());
		}
		EXIT_NOT_IMPLEMENTED(rt.view.base_array_slice_index != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.view.last_array_slice_index != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.view.current_mip_level != 0x00000000);
		EXIT_NOT_IMPLEMENTED(!(rt.attrib.tile_mode == 0x0d) && rt.info.neo_mode != Config::IsNeo());
		EXIT_NOT_IMPLEMENTED(rt.info.blend_bypass != false);
		// EXIT_NOT_IMPLEMENTED(rt.info.blend_clamp != false);
		EXIT_NOT_IMPLEMENTED(rt.info.round_mode != false);
		//		 EXIT_NOT_IMPLEMENTED(rt.format != 0x0000000a);
		// EXIT_NOT_IMPLEMENTED(rt.channel_type != 0x00000006);
		// EXIT_NOT_IMPLEMENTED(rt.channel_order != 0x00000001);
		EXIT_NOT_IMPLEMENTED(rt.attrib.force_dest_alpha_to_one != false);
		// EXIT_NOT_IMPLEMENTED(rt.tile_mode != 0x0000000a);
		// EXIT_NOT_IMPLEMENTED(rt.fmask_tile_mode != 0x0000000a);
		EXIT_NOT_IMPLEMENTED(rt.attrib2.width != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.attrib2.height != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.attrib2.num_mip_levels != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.attrib3.depth != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.attrib3.tile_mode != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.attrib3.dimension != 0x00000000);
	}
}

static void z_print(const char* func, const HW::DepthRenderTarget& z)
{
	printf("%s\n", func);

	printf("\t z_info.format                         = 0x%08" PRIx32 "\n", z.z_info.format);
	printf("\t z_info.tile_mode_index                = 0x%08" PRIx32 "\n", z.z_info.tile_mode_index);
	printf("\t z_info.num_samples                    = 0x%08" PRIx32 "\n", z.z_info.num_samples);
	printf("\t z_info.tile_surface_enable            = %s\n", z.z_info.tile_surface_enable ? "true" : "false");
	printf("\t z_info.expclear_enabled               = %s\n", z.z_info.expclear_enabled ? "true" : "false");
	printf("\t z_info.zrange_precision               = 0x%08" PRIx32 "\n", z.z_info.zrange_precision);
	printf("\t z_info.embedded_sample_locations      = %s\n", z.z_info.embedded_sample_locations ? "true" : "false");
	printf("\t z_info.partially_resident             = %s\n", z.z_info.partially_resident ? "true" : "false");
	printf("\t z_info.num_mip_levels                 = 0x%02" PRIx8 "\n", z.z_info.num_mip_levels);
	printf("\t z_info.plane_compression              = 0x%02" PRIx8 "\n", z.z_info.plane_compression);
	printf("\t stencil_info.format                   = 0x%08" PRIx32 "\n", z.stencil_info.format);
	printf("\t stencil_info.tile_stencil_disable     = %s\n", z.stencil_info.tile_stencil_disable ? "true" : "false");
	printf("\t stencil_info.expclear_enabled         = %s\n", z.stencil_info.expclear_enabled ? "true" : "false");
	printf("\t stencil_info.tile_mode_index          = 0x%08" PRIx32 "\n", z.stencil_info.tile_mode_index);
	printf("\t stencil_info.tile_split               = 0x%08" PRIx32 "\n", z.stencil_info.tile_split);
	printf("\t stencil_info.texture_compatible_stencil = %s\n", z.stencil_info.texture_compatible_stencil ? "true" : "false");
	printf("\t stencil_info.partially_resident       = %s\n", z.stencil_info.partially_resident ? "true" : "false");
	printf("\t depth_info.addr5_swizzle_mask         = 0x%08" PRIx32 "\n", z.depth_info.addr5_swizzle_mask);
	printf("\t depth_info.array_mode                 = 0x%08" PRIx32 "\n", z.depth_info.array_mode);
	printf("\t depth_info.pipe_config                = 0x%08" PRIx32 "\n", z.depth_info.pipe_config);
	printf("\t depth_info.bank_width                 = 0x%08" PRIx32 "\n", z.depth_info.bank_width);
	printf("\t depth_info.bank_height                = 0x%08" PRIx32 "\n", z.depth_info.bank_height);
	printf("\t depth_info.macro_tile_aspect          = 0x%08" PRIx32 "\n", z.depth_info.macro_tile_aspect);
	printf("\t depth_info.num_banks                  = 0x%08" PRIx32 "\n", z.depth_info.num_banks);
	printf("\t depth_view.slice_start                = 0x%08" PRIx32 "\n", z.depth_view.slice_start);
	printf("\t depth_view.slice_max                  = 0x%08" PRIx32 "\n", z.depth_view.slice_max);
	printf("\t depth_view.current_mip_level          = 0x%02" PRIx8 "\n", z.depth_view.current_mip_level);
	printf("\t depth_view.depth_write_disable        = %s\n", z.depth_view.depth_write_disable ? "true" : "false");
	printf("\t depth_view.stencil_write_disable      = %s\n", z.depth_view.stencil_write_disable ? "true" : "false");
	printf("\t htile_surface.raw                     = 0x%08" PRIx32 "\n", z.htile_surface.raw);
	printf("\t htile_surface.linear                  = 0x%08" PRIx32 "\n", z.htile_surface.linear);
	printf("\t htile_surface.full_cache              = 0x%08" PRIx32 "\n", z.htile_surface.full_cache);
	printf("\t htile_surface.htile_uses_preload_win  = 0x%08" PRIx32 "\n", z.htile_surface.htile_uses_preload_win);
	printf("\t htile_surface.preload                 = 0x%08" PRIx32 "\n", z.htile_surface.preload);
	printf("\t htile_surface.prefetch_width          = 0x%08" PRIx32 "\n", z.htile_surface.prefetch_width);
	printf("\t htile_surface.prefetch_height         = 0x%08" PRIx32 "\n", z.htile_surface.prefetch_height);
	printf("\t htile_surface.dst_outside_zero_to_one = 0x%08" PRIx32 "\n", z.htile_surface.dst_outside_zero_to_one);
	printf("\t z_read_base_addr                      = 0x%016" PRIx64 "\n", z.z_read_base_addr);
	printf("\t stencil_read_base_addr                = 0x%016" PRIx64 "\n", z.stencil_read_base_addr);
	printf("\t z_write_base_addr                     = 0x%016" PRIx64 "\n", z.z_write_base_addr);
	printf("\t stencil_write_base_addr               = 0x%016" PRIx64 "\n", z.stencil_write_base_addr);
	printf("\t pitch_div8_minus1                     = 0x%08" PRIx32 "\n", z.pitch_div8_minus1);
	printf("\t height_div8_minus1                    = 0x%08" PRIx32 "\n", z.height_div8_minus1);
	printf("\t slice_div64_minus1                    = 0x%08" PRIx32 "\n", z.slice_div64_minus1);
	printf("\t htile_data_base_addr                  = 0x%016" PRIx64 "\n", z.htile_data_base_addr);
	printf("\t width                                 = 0x%08" PRIx32 "\n", z.width);
	printf("\t height                                = 0x%08" PRIx32 "\n", z.height);
	printf("\t size.x_max                            = 0x%04" PRIx16 "\n", z.size.x_max);
	printf("\t size.y_max                            = 0x%04" PRIx16 "\n", z.size.y_max);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void z_check(const HW::DepthRenderTarget& z)
{
	if (z.z_info.format == 0)
	{
		EXIT_NOT_IMPLEMENTED(z.z_info.format != 0);
		EXIT_NOT_IMPLEMENTED(z.z_info.tile_mode_index != 0);
		EXIT_NOT_IMPLEMENTED(z.z_info.num_samples != 0);
		EXIT_NOT_IMPLEMENTED(z.z_info.tile_surface_enable != false);
		EXIT_NOT_IMPLEMENTED(z.z_info.expclear_enabled != false);
		EXIT_NOT_IMPLEMENTED(z.z_info.zrange_precision != 0);
		EXIT_NOT_IMPLEMENTED(z.z_info.embedded_sample_locations != false);
		EXIT_NOT_IMPLEMENTED(z.z_info.partially_resident != false);
		EXIT_NOT_IMPLEMENTED(z.z_info.num_mip_levels != 0);
		EXIT_NOT_IMPLEMENTED(z.z_info.plane_compression != 0);
	} else
	{
		EXIT_NOT_IMPLEMENTED(z.z_info.format != 0x00000003);
		// EXIT_NOT_IMPLEMENTED(z.z_info.tile_mode_index != (Config::IsNeo() ? 0x00000002 : 0));
		EXIT_NOT_IMPLEMENTED(z.z_info.num_samples != 0x00000000);
		// EXIT_NOT_IMPLEMENTED(z.z_info.tile_surface_enable != true);
		EXIT_NOT_IMPLEMENTED(z.z_info.expclear_enabled != false);
		EXIT_NOT_IMPLEMENTED(z.z_info.zrange_precision != 0x00000001);
		EXIT_NOT_IMPLEMENTED(z.z_info.embedded_sample_locations != false);
		EXIT_NOT_IMPLEMENTED(z.z_info.partially_resident != false);
		EXIT_NOT_IMPLEMENTED(z.z_info.num_mip_levels != 0);
		EXIT_NOT_IMPLEMENTED(z.z_info.plane_compression != 0);
	}

	if (z.stencil_info.format == 0)
	{
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.format != 0);
		//  EXIT_NOT_IMPLEMENTED(z.stencil_info.tile_stencil_disable != false);
		EXIT_NOT_IMPLEMENTED(z.stencil_info.expclear_enabled != false);
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.tile_mode_index != 0);
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.tile_split != 0);
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.texture_compatible_stencil != true);
		EXIT_NOT_IMPLEMENTED(z.stencil_info.partially_resident != false);
	} else
	{
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.format != 0x00000001);
		EXIT_NOT_IMPLEMENTED(z.stencil_info.tile_stencil_disable != true);
		EXIT_NOT_IMPLEMENTED(z.stencil_info.expclear_enabled != false);
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.tile_mode_index != (Config::IsNeo() ? 0x00000002 : 0));
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.tile_split != (Config::IsNeo() ? 0x00000002 : 0));
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.texture_compatible_stencil != true);
		EXIT_NOT_IMPLEMENTED(z.stencil_info.partially_resident != false);
	}

	if (z.z_info.format != 0 || z.stencil_info.format != 0)
	{
		const bool uniform_addr5_zero_candidate = z.depth_info.addr5_swizzle_mask == 0 &&
		    DepthSurfaceUniformZeroImportEnabled() && z.z_info.format == 3 && z.stencil_info.format == 1;
		if (z.depth_info.addr5_swizzle_mask != 0x00000001 && !uniform_addr5_zero_candidate)
		{
			z_print("DepthRenderTraget:", z);
			EXIT("unsupported depth addr5 swizzle mask: 0x%08" PRIx32 "\n", z.depth_info.addr5_swizzle_mask);
		}
		EXIT_NOT_IMPLEMENTED(z.depth_info.array_mode != 0x00000004);
		EXIT_NOT_IMPLEMENTED(z.depth_info.pipe_config != (Config::IsNeo() ? 0x00000012 : 0x0c));
		EXIT_NOT_IMPLEMENTED(z.depth_info.bank_width != 0x00000000);
		// EXIT_NOT_IMPLEMENTED(z.depth_info.bank_height != (Config::IsNeo() ? 0x00000001 : 2));
		// EXIT_NOT_IMPLEMENTED(z.depth_info.macro_tile_aspect != (Config::IsNeo() ? 0x00000000 : 2));
		// EXIT_NOT_IMPLEMENTED(z.depth_info.num_banks != (Config::IsNeo() ? 0x00000002 : 3));
		EXIT_NOT_IMPLEMENTED(z.htile_surface.linear != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.full_cache != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.htile_uses_preload_win != 0x00000000);
		EXIT_NOT_IMPLEMENTED(!z.IsHtilePreloadStateSupported());
		EXIT_NOT_IMPLEMENTED(z.htile_surface.prefetch_width != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.prefetch_height != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.dst_outside_zero_to_one != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.depth_view.slice_start != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.depth_view.slice_max != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.depth_view.current_mip_level != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.depth_view.depth_write_disable != false);
		EXIT_NOT_IMPLEMENTED(z.depth_view.stencil_write_disable != false);
		EXIT_NOT_IMPLEMENTED(z.z_read_base_addr != z.z_write_base_addr);
		EXIT_NOT_IMPLEMENTED(z.stencil_read_base_addr != z.stencil_write_base_addr);
		EXIT_NOT_IMPLEMENTED(z.z_write_base_addr == 0);
		EXIT_NOT_IMPLEMENTED(z.stencil_info.format != 0 && z.stencil_write_base_addr == 0);
		// EXIT_NOT_IMPLEMENTED(z.pitch_div8_minus1 != 0x000000ff);
		// EXIT_NOT_IMPLEMENTED(z.height_div8_minus1 != 0x0000008f);
		// EXIT_NOT_IMPLEMENTED(z.slice_div64_minus1 != 0x00008fff);
		// EXIT_NOT_IMPLEMENTED(z.htile_data_base_addr == 0);
		// EXIT_NOT_IMPLEMENTED(z.width != 0x00000780);
		// EXIT_NOT_IMPLEMENTED(z.height != 0x00000438);
	}
}

static void clip_print(const char* func, const HW::ClipControl& c)
{
	printf("%s\n", func);

	printf("\t user_clip_planes                    = 0x%02" PRIx8 "\n", c.user_clip_planes);
	printf("\t user_clip_plane_mode                = 0x%02" PRIx8 "\n", c.user_clip_plane_mode);
	printf("\t dx_clip_space                       = %s\n", c.dx_clip_space ? "true" : "false");
	printf("\t vertex_kill_any                     = %s\n", c.vertex_kill_any ? "true" : "false");
	printf("\t min_z_clip_disable                  = %s\n", c.min_z_clip_disable ? "true" : "false");
	printf("\t max_z_clip_disable                  = %s\n", c.max_z_clip_disable ? "true" : "false");
	printf("\t user_clip_plane_negate_y            = %s\n", c.user_clip_plane_negate_y ? "true" : "false");
	printf("\t clip_disable                        = %s\n", c.clip_disable ? "true" : "false");
	printf("\t user_clip_plane_cull_only           = %s\n", c.user_clip_plane_cull_only ? "true" : "false");
	printf("\t cull_on_clipping_error_disable      = %s\n", c.cull_on_clipping_error_disable ? "true" : "false");
	printf("\t linear_attribute_clip_enable        = %s\n", c.linear_attribute_clip_enable ? "true" : "false");
	printf("\t force_viewport_index_from_vs_enable = %s\n", c.force_viewport_index_from_vs_enable ? "true" : "false");
}

static void clip_check(const HW::ClipControl& c)
{
	EXIT_NOT_IMPLEMENTED(!c.IsSupportedForDraw());
}

static void rc_print(const char* func, const HW::RenderControl& c)
{
	printf("%s\n", func);

	printf("\t depth_clear_enable       = %s\n", c.depth_clear_enable ? "true" : "false");
	printf("\t stencil_clear_enable     = %s\n", c.stencil_clear_enable ? "true" : "false");
	printf("\t resummarize_enable       = %s\n", c.resummarize_enable ? "true" : "false");
	printf("\t stencil_compress_disable = %s\n", c.stencil_compress_disable ? "true" : "false");
	printf("\t depth_compress_disable   = %s\n", c.depth_compress_disable ? "true" : "false");
	printf("\t copy_centroid            = %s\n", c.copy_centroid ? "true" : "false");
	printf("\t copy_sample              = %" PRIu8 "\n", c.copy_sample);
}

static void rc_check(const HW::RenderControl& c)
{
	// EXIT_NOT_IMPLEMENTED(c.depth_clear_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.stencil_clear_enable != false);
	EXIT_NOT_IMPLEMENTED(c.resummarize_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.stencil_compress_disable != false);
	// EXIT_NOT_IMPLEMENTED(c.depth_compress_disable != false);
	EXIT_NOT_IMPLEMENTED(c.copy_centroid != false);
	EXIT_NOT_IMPLEMENTED(c.copy_sample != 0);
}

static void mc_print(const char* func, const HW::ModeControl& c)
{
	printf("%s\n", func);

	printf("\t cull_front               = %s\n", c.cull_front ? "true" : "false");
	printf("\t cull_back                = %s\n", c.cull_back ? "true" : "false");
	printf("\t face                     = %s\n", c.face ? "true" : "false");
	printf("\t poly_mode                = %" PRIu8 "\n", c.poly_mode);
	printf("\t polymode_front_ptype     = %" PRIu8 "\n", c.polymode_front_ptype);
	printf("\t polymode_back_ptype      = %" PRIu8 "\n", c.polymode_back_ptype);
	printf("\t poly_offset_front_enable = %s\n", c.poly_offset_front_enable ? "true" : "false");
	printf("\t poly_offset_back_enable  = %s\n", c.poly_offset_back_enable ? "true" : "false");
	printf("\t vtx_window_offset_enable = %s\n", c.vtx_window_offset_enable ? "true" : "false");
	printf("\t provoking_vtx_last       = %s\n", c.provoking_vtx_last ? "true" : "false");
	printf("\t persp_corr_dis           = %s\n", c.persp_corr_dis ? "true" : "false");
}

static void mc_check(const HW::ModeControl& c)
{
	// EXIT_NOT_IMPLEMENTED(c.cull_front != false);
	// EXIT_NOT_IMPLEMENTED(c.cull_back != false);
	// EXIT_NOT_IMPLEMENTED(c.face != false);
	EXIT_NOT_IMPLEMENTED(c.poly_mode != 0);
	EXIT_NOT_IMPLEMENTED(c.polymode_front_ptype != 0 && c.polymode_front_ptype != 2);
	EXIT_NOT_IMPLEMENTED(c.polymode_back_ptype != 0 && c.polymode_back_ptype != 2);
	// Polygon-offset admission uses the actual depth attachment before lookup.
	EXIT_NOT_IMPLEMENTED(c.vtx_window_offset_enable != false);
	EXIT_NOT_IMPLEMENTED(c.provoking_vtx_last != false);
	EXIT_NOT_IMPLEMENTED(c.persp_corr_dis != false);
}

static void bc_print(const char* func, const HW::BlendControl& c, const HW::BlendColor& color, const HW::ColorControl& cc)
{
	printf("%s\n", func);

	printf("\t color_srcblend       = %" PRIu8 "\n", c.color_srcblend);
	printf("\t color_comb_fcn       = %" PRIu8 "\n", c.color_comb_fcn);
	printf("\t color_destblend      = %" PRIu8 "\n", c.color_destblend);
	printf("\t alpha_srcblend       = %" PRIu8 "\n", c.alpha_srcblend);
	printf("\t alpha_comb_fcn       = %" PRIu8 "\n", c.alpha_comb_fcn);
	printf("\t alpha_destblend      = %" PRIu8 "\n", c.alpha_destblend);
	printf("\t separate_alpha_blend = %s\n", c.separate_alpha_blend ? "true" : "false");
	printf("\t enable               = %s\n", c.enable ? "true" : "false");
	printf("\t red                  = %f\n", color.red);
	printf("\t green                = %f\n", color.green);
	printf("\t blue                 = %f\n", color.blue);
	printf("\t alpha                = %f\n", color.alpha);
	printf("\t cc.mode              = %" PRIu8 "\n", cc.mode);
	printf("\t cc.op                = %" PRIu8 "\n", cc.op);
}

static bool BlendConstantsSupported(const HW::BlendColor& color)
{
	for (float value: {color.red, color.green, color.blue, color.alpha})
		if (!std::isfinite(value) || value < 0.0f || value > 1.0f) return false;
	return true;
}

static void SetPipelineBlendConstants(PipelineStaticParameters* parameters, const HW::BlendColor& color)
{
	parameters->blend_color_red = color.red;
	parameters->blend_color_green = color.green;
	parameters->blend_color_blue = color.blue;
	parameters->blend_color_alpha = color.alpha;
}

static void bc_check(const HW::BlendControl& c, const HW::BlendColor& color, const HW::ColorControl& cc)
{
	if (!BlendConstantsSupported(color))
	{
		const float values[] = {color.red, color.green, color.blue, color.alpha};
		uint32_t raw[4];
		std::memcpy(raw, values, sizeof(raw));
		std::fprintf(stderr, "unsupported blend constants: expected finite RGBA in [0,1]; "
		                     "raw=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32
		                     " values=%.9g,%.9g,%.9g,%.9g enable=%u separate_alpha=%u "
		                     "color_src=%u color_dst=%u color_op=%u alpha_src=%u alpha_dst=%u alpha_op=%u "
		                     "cc_mode=%u cc_op=%u\n",
		             raw[0], raw[1], raw[2], raw[3], values[0], values[1], values[2], values[3],
		             unsigned(c.enable), unsigned(c.separate_alpha_blend), unsigned(c.color_srcblend),
		             unsigned(c.color_destblend), unsigned(c.color_comb_fcn), unsigned(c.alpha_srcblend),
		             unsigned(c.alpha_destblend), unsigned(c.alpha_comb_fcn), unsigned(cc.mode), unsigned(cc.op));
		std::fflush(stderr);
		EXIT("unsupported blend constants\n");
	}
	// Mode 2 is CB_ELIMINATE_FAST_CLEAR. The renderer already stores every
	// color surface uncompressed and materialized, so the elimination pass has
	// no expand work to do and runs with color writes disabled.
	EXIT_NOT_IMPLEMENTED(cc.mode != 1 && cc.mode != 0 && cc.mode != 2);
	EXIT_NOT_IMPLEMENTED(cc.op != 0xCC);
}

static void d_print(const char* func, const HW::DepthControl& c, const HW::StencilControl& s, const HW::StencilMask& sm)
{
	printf("%s\n", func);

	printf("\t stencil_enable       = %s\n", c.stencil_enable ? "true" : "false");
	printf("\t z_enable             = %s\n", c.z_enable ? "true" : "false");
	printf("\t z_write_enable       = %s\n", c.z_write_enable ? "true" : "false");
	printf("\t depth_bounds_enable  = %s\n", c.depth_bounds_enable ? "true" : "false");
	printf("\t zfunc                = %" PRIu8 "\n", c.zfunc);
	printf("\t backface_enable      = %s\n", c.backface_enable ? "true" : "false");
	printf("\t stencilfunc          = %" PRIu8 "\n", c.stencilfunc);
	printf("\t stencilfunc_bf       = %" PRIu8 "\n", c.stencilfunc_bf);
	printf("\t color_writes_on_depth_fail_enable  = %s\n", c.color_writes_on_depth_fail_enable ? "true" : "false");
	printf("\t color_writes_on_depth_pass_disable = %s\n", c.color_writes_on_depth_pass_disable ? "true" : "false");
	printf("\t stencil_fail         = %" PRIu8 "\n", s.stencil_fail);
	printf("\t stencil_zpass        = %" PRIu8 "\n", s.stencil_zpass);
	printf("\t stencil_zfail        = %" PRIu8 "\n", s.stencil_zfail);
	printf("\t stencil_fail_bf      = %" PRIu8 "\n", s.stencil_fail_bf);
	printf("\t stencil_zpass_bf     = %" PRIu8 "\n", s.stencil_zpass_bf);
	printf("\t stencil_zfail_bf     = %" PRIu8 "\n", s.stencil_zfail_bf);
	printf("\t stencil_testval      = %" PRIu8 "\n", sm.stencil_testval);
	printf("\t stencil_mask         = %" PRIu8 "\n", sm.stencil_mask);
	printf("\t stencil_writemask    = %" PRIu8 "\n", sm.stencil_writemask);
	printf("\t stencil_opval        = %" PRIu8 "\n", sm.stencil_opval);
	printf("\t stencil_testval_bf   = %" PRIu8 "\n", sm.stencil_testval_bf);
	printf("\t stencil_mask_bf      = %" PRIu8 "\n", sm.stencil_mask_bf);
	printf("\t stencil_writemask_bf = %" PRIu8 "\n", sm.stencil_writemask_bf);
	printf("\t stencil_opval_bf     = %" PRIu8 "\n", sm.stencil_opval_bf);
}

static bool IsRenderStencilFaceStateSupported(const HW::DepthControl& c, const HW::StencilControl& s, const HW::StencilMask& sm)
{
	if (!c.backface_enable)
		return s.stencil_fail_bf == 0 && s.stencil_zpass_bf == 0 && s.stencil_zfail_bf == 0;
	// The legacy decoder chooses its Vulkan reference from the write operation,
	// so non-Always comparisons need separate reference qualification. Retained
	// inactive state must not conceal an unsupported comparison or unequal input.
	if (c.stencilfunc != 7 || c.stencilfunc != c.stencilfunc_bf || s.stencil_fail != s.stencil_fail_bf ||
	    s.stencil_zpass != s.stencil_zpass_bf || s.stencil_zfail != s.stencil_zfail_bf ||
	    sm.stencil_testval != sm.stencil_testval_bf || sm.stencil_mask != sm.stencil_mask_bf ||
	    sm.stencil_writemask != sm.stencil_writemask_bf || sm.stencil_opval != sm.stencil_opval_bf) return false;
	const auto arithmetic = [](uint8_t op) { return op == 5 || op == 6 || op == 8 || op == 9; };
	// Vulkan increment/decrement changes the old value by one, while the guest
	// arithmetic uses OPVAL. Only that operand is represented by this decoder.
	return !(arithmetic(s.stencil_fail) || arithmetic(s.stencil_zpass) || arithmetic(s.stencil_zfail)) ||
	       sm.stencil_opval == 1;
}

static void d_check(const HW::DepthControl& c, const HW::StencilControl& s, const HW::StencilMask& sm)
{
	// EXIT_NOT_IMPLEMENTED(c.stencil_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.z_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.z_write_enable != false);
	EXIT_NOT_IMPLEMENTED(c.depth_bounds_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.zfunc != 0);
	EXIT_NOT_IMPLEMENTED(!IsRenderStencilFaceStateSupported(c, s, sm));
	// EXIT_NOT_IMPLEMENTED(c.stencilfunc != 0);
	// EXIT_NOT_IMPLEMENTED(c.stencilfunc_bf != 0);
	EXIT_NOT_IMPLEMENTED(c.color_writes_on_depth_fail_enable != false);
	EXIT_NOT_IMPLEMENTED(c.color_writes_on_depth_pass_disable != false);
	// EXIT_NOT_IMPLEMENTED(s.stencil_fail != 0);
	// EXIT_NOT_IMPLEMENTED(s.stencil_zpass != 0);
	// EXIT_NOT_IMPLEMENTED(s.stencil_zfail != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_testval != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_mask != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_writemask != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_opval != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_testval_bf != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_mask_bf != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_writemask_bf != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_opval_bf != 0);
}

// This is admission of retained, unused register state, not D16 content or
// attachment support. Re-evaluate all consumers on each draw, including cache
// hits. Any activation returns to the unchanged active descriptor gate.
static bool IsInactiveD16DepthDescriptorSupported(const HW::Context& hw, bool neo, const char** reason = nullptr)
{
	if (reason != nullptr) *reason = nullptr;
	const auto reject = [&](const char* value) { if (reason != nullptr) *reason = value; return false; };
	const auto& z = hw.GetDepthRenderTarget();
	const auto& d = z.z_info;
	const auto& s = z.stencil_info;
	const auto& t = z.depth_info;
	const auto& h = z.htile_surface;
	const auto& v = z.depth_view;
	const auto& dc = hw.GetDepthControl();
	const auto& rc = hw.GetRenderControl();
	if (d.format != 1 || s.format != 0 || d.tile_mode_index != 0 || d.num_samples != 0 ||
	    d.zrange_precision > 1 || d.expclear_enabled || d.embedded_sample_locations || d.partially_resident ||
	    d.num_mip_levels != 0 || d.plane_compression != 0 || s.tile_mode_index != 0 || s.tile_split != 0 ||
	    s.expclear_enabled || s.texture_compatible_stencil || s.partially_resident ||
	    d.unsupported_bits != 0 || s.unsupported_bits != 0 || t.unsupported_bits != 0 ||
	    z.depth_size_unsupported_bits != 0 || z.depth_slice_unsupported_bits != 0 ||
	    v.slice_start != 0 || v.slice_max != 0 || v.current_mip_level != 0 || v.depth_write_disable || v.stencil_write_disable)
		return reject("format-plane-sample-mip-view-or-unknown-metadata");
	if (t.addr5_swizzle_mask > 1 || t.array_mode != 4 || t.pipe_config != (neo ? 0x12u : 0xcu) ||
	    t.bank_width != 0 || t.bank_height != (neo ? 1u : 2u) || t.macro_tile_aspect != (neo ? 0u : 2u) ||
	    t.num_banks != (neo ? 2u : 3u) || (h.raw != 0 && h.raw != 8) || h.linear != 0 || h.full_cache != 0 ||
	    h.htile_uses_preload_win != 0 || h.preload != (h.raw >> 3u) || h.prefetch_width != 0 || h.prefetch_height != 0 ||
	    h.dst_outside_zero_to_one != 0 || !z.IsHtilePreloadStateSupported())
		return reject("tile-bank-or-htile-metadata");
	if (z.z_read_base_addr == 0 || z.z_read_base_addr != z.z_write_base_addr ||
	    z.stencil_read_base_addr != 0 || z.stencil_write_base_addr != 0 ||
	    (d.tile_surface_enable && z.htile_data_base_addr == 0) ||
	    (!d.tile_surface_enable && z.htile_data_base_addr != 0))
		return reject("base-or-plane-presence");
	// Check only the declared geometry. No allocation lookup, address mapping,
	// guest lease, import or native object is authorized by this predicate.
	const uint64_t pitch = (static_cast<uint64_t>(z.pitch_div8_minus1) + 1u) * 8u;
	const uint64_t height = (static_cast<uint64_t>(z.height_div8_minus1) + 1u) * 8u;
	const uint64_t pixels = (static_cast<uint64_t>(z.slice_div64_minus1) + 1u) * 64u;
	if (z.width == 0 || z.height == 0 || pitch > std::numeric_limits<uint32_t>::max() ||
	    height > std::numeric_limits<uint32_t>::max() || z.width > pitch || z.height > height ||
	    pixels % pitch != 0 || pixels / pitch != height)
		return reject("declared-geometry");
	if (dc.z_enable || dc.z_write_enable || dc.stencil_enable || dc.depth_bounds_enable || dc.backface_enable ||
	    dc.color_writes_on_depth_fail_enable || dc.color_writes_on_depth_pass_disable || dc.unsupported_bits != 0 ||
	    dc.zfunc > 7 || dc.stencilfunc > 7 || dc.stencilfunc_bf > 7 ||
	    rc.raw != 0 || rc.depth_clear_enable || rc.stencil_clear_enable || rc.resummarize_enable ||
	    rc.depth_compress_disable || rc.stencil_compress_disable || rc.copy_centroid || rc.copy_sample != 0)
		return reject("depth-render-consumer-or-opaque-control");
	// Inactive comparison values may remain programmed, but the complete raw
	// control must agree and cannot hide an opaque or enabled control bit.
	const uint32_t inert_control =
	    (static_cast<uint32_t>(dc.zfunc) << Pm4::DB_DEPTH_CONTROL_ZFUNC_SHIFT) |
	    (static_cast<uint32_t>(dc.stencilfunc) << Pm4::DB_DEPTH_CONTROL_STENCILFUNC_SHIFT) |
	    (static_cast<uint32_t>(dc.stencilfunc_bf) << Pm4::DB_DEPTH_CONTROL_STENCILFUNC_BF_SHIFT);
	if (dc.raw != inert_control) return reject("raw-depth-control-disagreement");
	if (!DepthSurfaceUniformZeroShaderProfile(hw.GetShaderRegisters())) return reject("shader-depth-interface");
	if (!IsRenderStencilFaceStateSupported(dc, hw.GetStencilControl(), hw.GetStencilMask())) return reject("stencil-cross-check");
	PipelineDepthBias bias;
	if (!ResolvePolygonOffset(hw, VK_FORMAT_UNDEFINED, false, &bias) || bias.enabled) return reject("polygon-offset-consumer");
	return true;
}

static void eqaa_print(const char* func, const HW::EqaaControl& c)
{
	printf("%s\n", func);

	printf("\t max_anchor_samples         = %" PRIu8 "\n", c.max_anchor_samples);
	printf("\t ps_iter_samples            = %" PRIu8 "\n", c.ps_iter_samples);
	printf("\t mask_export_num_samples    = %" PRIu8 "\n", c.mask_export_num_samples);
	printf("\t alpha_to_mask_num_samples  = %" PRIu8 "\n", c.alpha_to_mask_num_samples);
	printf("\t high_quality_intersections = %s\n", c.high_quality_intersections ? "true" : "false");
	printf("\t incoherent_eqaa_reads      = %s\n", c.incoherent_eqaa_reads ? "true" : "false");
	printf("\t interpolate_comp_z         = %s\n", c.interpolate_comp_z ? "true" : "false");
	printf("\t static_anchor_associations = %s\n", c.static_anchor_associations ? "true" : "false");
}

static void eqaa_check(const HW::EqaaControl& c)
{
	EXIT_NOT_IMPLEMENTED(c.max_anchor_samples != 0);
	EXIT_NOT_IMPLEMENTED(c.ps_iter_samples != 0);
	EXIT_NOT_IMPLEMENTED(c.mask_export_num_samples != 0);
	EXIT_NOT_IMPLEMENTED(c.alpha_to_mask_num_samples != 0);
	EXIT_NOT_IMPLEMENTED(c.high_quality_intersections != false);
	EXIT_NOT_IMPLEMENTED(c.incoherent_eqaa_reads != false);
	EXIT_NOT_IMPLEMENTED(c.interpolate_comp_z != false);
	// EXIT_NOT_IMPLEMENTED(c.static_anchor_associations != false);
}

static void aa_print(const char* func, const HW::AaSampleControl& c, const HW::AaConfig& cf)
{
	printf("%s\n", func);

	printf("\t centroid_priority = %016" PRIx64 "\n", c.centroid_priority);
	for (int i = 0; i < 16; i++)
	{
		printf("\t locations[%d] = %08" PRIx32 "\n", i, c.locations[i]);
	}
	printf("\t msaa_num_samples      = %" PRIu8 "\n", cf.msaa_num_samples);
	printf("\t aa_mask_centroid_dtmn = %s\n", cf.aa_mask_centroid_dtmn ? "true" : "false");
	printf("\t max_sample_dist       = %" PRIu8 "\n", cf.max_sample_dist);
	printf("\t msaa_exposed_samples  = %" PRIu8 "\n", cf.msaa_exposed_samples);
}

static void aa_check(const HW::AaSampleControl& c, const HW::AaConfig& cf)
{
	EXIT_NOT_IMPLEMENTED(c.centroid_priority != 0);
	for (uint32_t l: c.locations)
	{
		EXIT_NOT_IMPLEMENTED(l != 0);
	}
	EXIT_NOT_IMPLEMENTED(cf.msaa_num_samples != 0);
	EXIT_NOT_IMPLEMENTED(cf.aa_mask_centroid_dtmn != false);
	EXIT_NOT_IMPLEMENTED(cf.max_sample_dist != 0);
	EXIT_NOT_IMPLEMENTED(cf.msaa_exposed_samples != 0);
}

static void vp_print(const char* func, const HW::ScreenViewport& vp, const HW::ScanModeControl& smc)
{
	printf("%s\n", func);

	printf("\t msaa_enable                    = %s\n", smc.msaa_enable ? "true" : "false");
	printf("\t vport_scissor_enable           = %s\n", smc.vport_scissor_enable ? "true" : "false");
	printf("\t line_stipple_enable            = %s\n", smc.line_stipple_enable ? "true" : "false");
	printf("\t vp[0].zmin                     = %f\n", vp.viewports[0].zmin);
	printf("\t vp[0].zmax                     = %f\n", vp.viewports[0].zmax);
	printf("\t vp[0].xscale                   = %f\n", vp.viewports[0].xscale);
	printf("\t vp[0].xoffset                  = %f\n", vp.viewports[0].xoffset);
	printf("\t vp[0].yscale                   = %f\n", vp.viewports[0].yscale);
	printf("\t vp[0].yoffset                  = %f\n", vp.viewports[0].yoffset);
	printf("\t vp[0].zscale                   = %f\n", vp.viewports[0].zscale);
	printf("\t vp[0].zoffset                  = %f\n", vp.viewports[0].zoffset);
	printf("\t vp[0].viewport_scissor_left    = %d\n", vp.viewports[0].viewport_scissor_left);
	printf("\t vp[0].viewport_scissor_top     = %d\n", vp.viewports[0].viewport_scissor_top);
	printf("\t vp[0].viewport_scissor_right   = %d\n", vp.viewports[0].viewport_scissor_right);
	printf("\t vp[0].viewport_scissor_bottom  = %d\n", vp.viewports[0].viewport_scissor_bottom);
	printf("\t transform_control              = 0x%08" PRIx32 "\n", vp.transform_control);
	printf("\t screen_scissor_left            = %d\n", vp.screen_scissor_left);
	printf("\t screen_scissor_top             = %d\n", vp.screen_scissor_top);
	printf("\t screen_scissor_right           = %d\n", vp.screen_scissor_right);
	printf("\t screen_scissor_bottom          = %d\n", vp.screen_scissor_bottom);
	printf("\t generic_scissor_left           = %d\n", vp.generic_scissor_left);
	printf("\t generic_scissor_top            = %d\n", vp.generic_scissor_top);
	printf("\t generic_scissor_right          = %d\n", vp.generic_scissor_right);
	printf("\t generic_scissor_bottom         = %d\n", vp.generic_scissor_bottom);
	printf("\t hw_offset_x                    = %u\n", vp.hw_offset_x);
	printf("\t hw_offset_y                    = %u\n", vp.hw_offset_y);
	printf("\t guard_band_horz_clip           = %f\n", vp.guard_band_horz_clip);
	printf("\t guard_band_vert_clip           = %f\n", vp.guard_band_vert_clip);
	printf("\t guard_band_horz_discard        = %f\n", vp.guard_band_horz_discard);
	printf("\t guard_band_vert_discard        = %f\n", vp.guard_band_vert_discard);
	printf("\t generic_scissor_window_offset_enable               = %s\n", vp.generic_scissor_window_offset_enable ? "true" : "false");
	printf("\t viewports[0].viewport_scissor_window_offset_enable = %s\n",
	       vp.viewports[0].viewport_scissor_window_offset_enable ? "true" : "false");
}

static bool IsViewportScissorActive(const HW::ScreenViewport& vp, const HW::ScanModeControl& smc)
{
	// Preserve the existing local presence policy; the enable bit controls
	// whether retained viewport coordinates participate in a draw at all.
	return smc.vport_scissor_enable &&
	       (vp.viewports[0].viewport_scissor_left != 0 || vp.viewports[0].viewport_scissor_top != 0 ||
	        vp.viewports[0].viewport_scissor_right != 0 || vp.viewports[0].viewport_scissor_bottom != 0);
}

static bool SelectGraphicsScissor(const HW::ScreenViewport& vp, const HW::ScanModeControl& smc, int (&ltrb)[4])
{
	const bool viewport_scissor = IsViewportScissorActive(vp, smc);
	const bool generic_scissor =
	    vp.generic_scissor_left != 0 || vp.generic_scissor_top != 0 || vp.generic_scissor_right != 0 || vp.generic_scissor_bottom != 0;
	const bool screen_scissor =
	    vp.screen_scissor_left != 0 || vp.screen_scissor_top != 0 || vp.screen_scissor_right != 0 || vp.screen_scissor_bottom != 0;
	// Composition is qualified only in screen coordinates. Preserve the legacy
	// standalone viewport route, without inventing a PA_SC_WINDOW_OFFSET value.
	if (vp.generic_scissor_window_offset_enable ||
	    (viewport_scissor && (generic_scissor || screen_scissor) && vp.viewports[0].viewport_scissor_window_offset_enable))
		return false;
	int bounds[3][4] {};
	uint32_t count = 0;
	if (viewport_scissor)
	{
		const auto& viewport = vp.viewports[0];
		bounds[count][0] = viewport.viewport_scissor_left;
		bounds[count][1] = viewport.viewport_scissor_top;
		bounds[count][2] = viewport.viewport_scissor_right;
		bounds[count++][3] = viewport.viewport_scissor_bottom;
	}
	if (generic_scissor)
	{
		bounds[count][0] = vp.generic_scissor_left;
		bounds[count][1] = vp.generic_scissor_top;
		bounds[count][2] = vp.generic_scissor_right;
		bounds[count++][3] = vp.generic_scissor_bottom;
	}
	if (screen_scissor || count == 0)
	{
		bounds[count][0] = vp.screen_scissor_left;
		bounds[count][1] = vp.screen_scissor_top;
		bounds[count][2] = vp.screen_scissor_right;
		bounds[count++][3] = vp.screen_scissor_bottom;
	}
	// Validate every participating rectangle before selecting a contained one;
	// a narrow rectangle must not conceal malformed bounds in another scissor.
	VkRect2D validated {};
	for (uint32_t index = 0; index < count; ++index)
		if (!BuildScissorRect(bounds[index], &validated) ||
		    (viewport_scissor && count > 1 && (validated.extent.width == 0 || validated.extent.height == 0))) return false;
	for (uint32_t candidate = 0; candidate < count; ++candidate)
	{
		bool contained = true;
		for (uint32_t other = 0; other < count; ++other)
			contained = contained && bounds[candidate][0] >= bounds[other][0] && bounds[candidate][1] >= bounds[other][1] &&
			            bounds[candidate][2] <= bounds[other][2] && bounds[candidate][3] <= bounds[other][3];
		if (contained)
		{
			std::copy(std::begin(bounds[candidate]), std::end(bounds[candidate]), std::begin(ltrb));
			return true;
		}
	}
	// Crossing or disjoint scissors need a separately qualified composition.
	return false;
}

static void vp_check(const HW::ScreenViewport& vp, const HW::ScanModeControl& smc)
{
	EXIT_NOT_IMPLEMENTED(smc.msaa_enable);
	// EXIT_NOT_IMPLEMENTED(smc.vport_scissor_enable);
	EXIT_NOT_IMPLEMENTED(smc.line_stipple_enable);

	int scissor_ltrb[4] {};
	EXIT_NOT_IMPLEMENTED(!SelectGraphicsScissor(vp, smc, scissor_ltrb));

	EXIT_NOT_IMPLEMENTED(vp.viewports[0].zmin > 0.000000);
	EXIT_NOT_IMPLEMENTED(vp.viewports[0].zmax != 1.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].xscale != 960.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].xoffset != 960.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].yscale != -540.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].yoffset != 540.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].zscale != 0.500000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].zoffset != 0.500000);
	EXIT_NOT_IMPLEMENTED(vp.transform_control != 1087);
	// EXIT_NOT_IMPLEMENTED(vp.hw_offset_x != 60);
	// EXIT_NOT_IMPLEMENTED(vp.hw_offset_y != 32);
	// EXIT_NOT_IMPLEMENTED(fabsf(vp.guard_band_horz_clip - 33.133327f) > 0.001f);
	// EXIT_NOT_IMPLEMENTED(fabsf(vp.guard_band_vert_clip - 59.629623f) > 0.001f);
	EXIT_NOT_IMPLEMENTED(vp.guard_band_horz_discard != 1.000000);
	EXIT_NOT_IMPLEMENTED(vp.guard_band_vert_discard != 1.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].viewport_scissor_left != 0);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].viewport_scissor_top != 0);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].viewport_scissor_right != 0);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].viewport_scissor_bottom != 0);
}

static void hw_check(const HW::Context& hw)
{
	const auto& rt   = hw.GetRenderTarget(0);
	const auto& bc   = hw.GetBlendControl(0);
	const auto& bclr = hw.GetBlendColor();
	const auto& vp   = hw.GetScreenViewport();
	const auto& z    = hw.GetDepthRenderTarget();
	const auto& c    = hw.GetClipControl();
	const auto& rc   = hw.GetRenderControl();
	const auto& d    = hw.GetDepthControl();
	const auto& s    = hw.GetStencilControl();
	const auto& sm   = hw.GetStencilMask();
	const auto& mc   = hw.GetModeControl();
	const auto& eqaa = hw.GetEqaaControl();
	const auto& cc   = hw.GetColorControl();
	const auto& smc  = hw.GetScanModeControl();
	const auto& aa   = hw.GetAaSampleControl();
	const auto& ac   = hw.GetAaConfig();

	rt_check(rt);
	vp_check(vp, smc);
	const char* inactive_d16_rejection = nullptr;
	const bool inactive_d16 = IsInactiveD16DepthDescriptorSupported(hw, Config::IsNeo(), &inactive_d16_rejection);
	const bool unsupported_depth_metadata =
	    ((z.z_info.format != 0 || z.stencil_info.format != 0) && z.depth_info.addr5_swizzle_mask != 1) ||
	    (z.z_info.format != 0 && z.z_info.format != 3);
	if (!inactive_d16 && unsupported_depth_metadata)
	{
		std::fprintf(stderr,
		             "[GFX_DEPTH_ADDR5_STATE] addr5_swizzle=%u z_enable=%u z_write=%u stencil_enable=%u"
		             " depth_clear=%u stencil_clear=%u resummarize=%u depth_compress_disable=%u"
		             " stencil_compress_disable=%u copy_centroid=%u copy_sample=%u htile=%u color_mode=%u\n",
		             z.depth_info.addr5_swizzle_mask, d.z_enable ? 1u : 0u, d.z_write_enable ? 1u : 0u,
		             d.stencil_enable ? 1u : 0u, rc.depth_clear_enable ? 1u : 0u, rc.stencil_clear_enable ? 1u : 0u,
		             rc.resummarize_enable ? 1u : 0u, rc.depth_compress_disable ? 1u : 0u,
		             rc.stencil_compress_disable ? 1u : 0u, rc.copy_centroid ? 1u : 0u,
		             static_cast<unsigned>(rc.copy_sample), z.z_info.tile_surface_enable ? 1u : 0u, cc.mode);
		std::fprintf(stderr,
		             "[GFX_DEPTH_LAYOUT_STATE] width=%u height=%u pitch_field=%u slice_field=%u depth_format=%u"
		             " stencil_format=%u depth_tile=%u stencil_tile=%u tile_split=%u array_mode=%u pipe=%u"
		             " bank_width=%u bank_height=%u macro_aspect=%u banks=%u htile_raw=%08x"
		             " depth=%016" PRIx64 " stencil=%016" PRIx64 " htile=%016" PRIx64 "\n",
		             z.width, z.height, z.pitch_div8_minus1, z.slice_div64_minus1, z.z_info.format, z.stencil_info.format,
		             z.z_info.tile_mode_index, z.stencil_info.tile_mode_index, z.stencil_info.tile_split,
		             z.depth_info.array_mode, z.depth_info.pipe_config, z.depth_info.bank_width, z.depth_info.bank_height,
		             z.depth_info.macro_tile_aspect, z.depth_info.num_banks, z.htile_surface.raw,
		             z.z_read_base_addr, z.stencil_read_base_addr, z.htile_data_base_addr);
	}
	if (!inactive_d16 && z.z_info.format != 0 && z.z_info.format != 3)
		std::fprintf(stderr,
		    "[GFX_DEPTH_FORMAT_STATE] format=%u samples=%u zrange=%u expclear=%u embedded=%u resident=%u mips=%u"
		    " compression=%u z_unknown=%08x stencil_unknown=%08x depth_unknown=%08x size_unknown=%08x slice_unknown=%08x"
		    " view_start=%u view_max=%u view_mip=%u depth_write_disable=%u stencil_write_disable=%u"
		    " height_field=%u stencil_clear_explicit=%u stencil_resident=%u stencil_compatible=%u stencil_tile_disable=%u"
		    " render_control_raw=%08x depth_control_raw=%08x depth_control_unknown=%08x metadata_only=1 admission=unchanged\n",
		    z.z_info.format, z.z_info.num_samples, z.z_info.zrange_precision, z.z_info.expclear_enabled ? 1u : 0u,
		    z.z_info.embedded_sample_locations ? 1u : 0u, z.z_info.partially_resident ? 1u : 0u,
		    static_cast<unsigned>(z.z_info.num_mip_levels), static_cast<unsigned>(z.z_info.plane_compression),
		    z.z_info.unsupported_bits, z.stencil_info.unsupported_bits, z.depth_info.unsupported_bits,
		    z.depth_size_unsupported_bits, z.depth_slice_unsupported_bits,
		    z.depth_view.slice_start, z.depth_view.slice_max, static_cast<unsigned>(z.depth_view.current_mip_level),
		    z.depth_view.depth_write_disable ? 1u : 0u, z.depth_view.stencil_write_disable ? 1u : 0u,
		    z.height_div8_minus1, z.stencil_info.expclear_enabled ? 1u : 0u,
		    z.stencil_info.partially_resident ? 1u : 0u, z.stencil_info.texture_compatible_stencil ? 1u : 0u,
		    z.stencil_info.tile_stencil_disable ? 1u : 0u, rc.raw, d.raw, d.unsupported_bits);
	if (!inactive_d16 && z.z_info.format == 1)
	{
		const auto& shader = hw.GetShaderRegisters();
		const auto& depth_shader = shader.db_shader_control;
		std::fprintf(stderr,
		    "[GFX_DEPTH_D16_CONSUMER_STATE] bounds=%u backface=%u color_on_fail=%u color_pass_disable=%u"
		    " shader_z_format=%08x z_export=%u conservative=%u z_behavior=%u shader_unknown=%08x"
		    " poly_front=%u poly_back=%u depth_write_base=%016" PRIx64 " stencil_write_base=%016" PRIx64
		    " inactive_domain_rejection=%s metadata_only=1 admission=unchanged\n",
		    d.depth_bounds_enable ? 1u : 0u, d.backface_enable ? 1u : 0u,
		    d.color_writes_on_depth_fail_enable ? 1u : 0u, d.color_writes_on_depth_pass_disable ? 1u : 0u,
		    shader.shader_z_format, depth_shader.shader_z_export_enable ? 1u : 0u,
		    static_cast<unsigned>(depth_shader.conservative_z_export_value),
		    static_cast<unsigned>(depth_shader.shader_z_behavior), depth_shader.other_bits,
		    mc.poly_offset_front_enable ? 1u : 0u, mc.poly_offset_back_enable ? 1u : 0u,
		    z.z_write_base_addr, z.stencil_write_base_addr,
		    inactive_d16_rejection != nullptr ? inactive_d16_rejection : "none");
	}
	if (inactive_d16)
	{
		static std::atomic_uint32_t logged {0};
		if (logged.load(std::memory_order_relaxed) < 8u && logged.fetch_add(1, std::memory_order_relaxed) < 8u)
			std::fprintf(stderr, "[GFX_DEPTH_D16_INACTIVE] format=1 stencil=0 consumers=off shader-depth=off"
			                     " unknown-bits=0 admission=descriptor-only depth-attachment=none"
			                     " guest-access=0 native-allocation=0 content-qualification=none\n");
	} else
	{
		z_check(z);
	}
	clip_check(c);
	rc_check(rc);
	d_check(d, s, sm);
	mc_check(mc);
	bc_check(bc, bclr, cc);
	eqaa_check(eqaa);
	aa_check(aa, ac);

	const uint32_t render_target_mask = hw.GetRenderTargetMask();
	VkColorComponentFlags color_write_mask = 0;
	if (!ResolveRt0ColorWriteMask(render_target_mask, cc.mode == 1, &color_write_mask))
	{
		const auto& shader = hw.GetShaderRegisters();
		EXIT("Unsupported render target mask: raw=%08" PRIx32 " rt0_components=%x upper_target_bits=%08" PRIx32
		     " color_mode=%u rt0_format=%u rt0_channel_type=%u rt0_channel_order=%u"
		     " blend_enable=%u separate_alpha_blend=%u shader_mask=%08" PRIx32 " output_mode0=%u\n",
		     render_target_mask, render_target_mask & 0xFu, render_target_mask & ~0xFu,
		     static_cast<unsigned>(cc.mode), static_cast<unsigned>(rt.info.format),
		     static_cast<unsigned>(rt.info.channel_type), static_cast<unsigned>(rt.info.channel_order),
		     bc.enable ? 1u : 0u, bc.separate_alpha_blend ? 1u : 0u, shader.m_cbShaderMask,
		     static_cast<unsigned>(shader.target_output_mode[0]));
	}
	EXIT_NOT_IMPLEMENTED(hw.GetDepthClearValue() != 0.0f && hw.GetDepthClearValue() != 1.0f);
	// EXIT_NOT_IMPLEMENTED(hw.GetStencilClearValue() != 0);
}

static void hw_print(const HW::Context& hw)
{
	const auto& rt   = hw.GetRenderTarget(0);
	const auto& bc   = hw.GetBlendControl(0);
	const auto& bclr = hw.GetBlendColor();
	const auto& vp   = hw.GetScreenViewport();
	const auto& z    = hw.GetDepthRenderTarget();
	const auto& c    = hw.GetClipControl();
	const auto& rc   = hw.GetRenderControl();
	const auto& d    = hw.GetDepthControl();
	const auto& s    = hw.GetStencilControl();
	const auto& sm   = hw.GetStencilMask();
	const auto& mc   = hw.GetModeControl();
	const auto& eqaa = hw.GetEqaaControl();
	const auto& cc   = hw.GetColorControl();
	const auto& smc  = hw.GetScanModeControl();
	const auto& aa   = hw.GetAaSampleControl();
	const auto& ac   = hw.GetAaConfig();

	if (Lumen::Log::GetDirection() != Lumen::Log::Direction::Silent)
	{
		printf("Context\n");
		printf("\t GetAlphaToMask()        = 0x%08" PRIx32 "\n", hw.GetAlphaToMask());
		printf("\t GetMultiVgtParam()      = 0x%08" PRIx32 "\n", hw.GetMultiVgtParam());
		printf("\t GetIndexOffset()        = 0x%08" PRIx32 "\n", hw.GetIndexOffset());
		printf("\t GetGsMode()             = 0x%08" PRIx32 " (configured=%u)\n", hw.GetGsMode(),
		       hw.IsGsModeConfigured() ? 1u : 0u);
		printf("\t GetStreamoutConfig()    = 0x%08" PRIx32 " (configured=%u)\n", hw.GetStreamoutConfig(),
		       hw.IsStreamoutConfigConfigured() ? 1u : 0u);
		printf("\t GetRenderTargetMask()   = 0x%08" PRIx32 "\n", hw.GetRenderTargetMask());
		printf("\t GetDepthClearValue()    = %f\n", hw.GetDepthClearValue());
		printf("\t GetStencilClearValue()  = %" PRIu8 "\n", hw.GetStencilClearValue());
		printf("\t GetLineWidth()          = %f\n", hw.GetLineWidth());

		printf("%s", rt_print("RenderTraget:", rt).Concat(U"").C_Str());

		z_print("DepthRenderTraget:", z);
		vp_print("ScreenViewport:", vp, smc);
		clip_print("ClipControl:", c);
		rc_print("RenderControl:", rc);
		d_print("DepthStencilControlMask:", d, s, sm);
		mc_print("ModeControl:", mc);
		bc_print("BlendColorControl:", bc, bclr, cc);
		eqaa_print("EqaaControl:", eqaa);
		aa_print("AaSampleControl:", aa, ac);
	}
}

void GraphicsRenderInit()
{
	EXIT_IF(g_render_ctx != nullptr);

	g_null_sampled_image_enabled = ShaderNullSampledImageEnabled();
	g_render_ctx = new RenderContext;
	dbg_printf("GFX descriptor configuration: null_sampled_image=%u null_sampled_scope=zero8_readonly_ps_gen4"
	           " null_sampled_backend=shared_zero_rgba8\n",
	           g_null_sampled_image_enabled ? 1u : 0u);
}

void GraphicsRenderCreateContext()
{
	EXIT_IF(g_render_ctx == nullptr);

	g_render_ctx->SetGraphicCtx(WindowGetGraphicContext());
}

TextureVulkanImage* RenderContext::GetNullSampledImage()
{
	Core::LockGuard lock(m_null_sampled_image_mutex);
	if (m_null_sampled_image != nullptr)
	{
		return m_null_sampled_image;
	}

	EXIT_IF(m_graphic_ctx == nullptr || m_graphic_ctx->device == nullptr);

	auto* image          = new TextureVulkanImage;
	image->extent.width  = NullSampledImageContract::width;
	image->extent.height = NullSampledImageContract::height;
	image->format        = NullSampledImageContract::format;
	image->layout        = VK_IMAGE_LAYOUT_UNDEFINED;

	VkImageCreateInfo image_info {};
	image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType     = VK_IMAGE_TYPE_2D;
	image_info.extent        = {NullSampledImageContract::width, NullSampledImageContract::height, 1};
	image_info.mipLevels     = 1;
	image_info.arrayLayers   = 1;
	image_info.format        = image->format;
	image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
	image_info.initialLayout = image->layout;
	image_info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	image_info.samples       = VK_SAMPLE_COUNT_1_BIT;

	const auto image_result = vkCreateImage(m_graphic_ctx->device, &image_info, nullptr, &image->image);
	EXIT_NOT_IMPLEMENTED(image_result != VK_SUCCESS || image->image == nullptr);

	VulkanMemory memory {};
	vkGetImageMemoryRequirements(m_graphic_ctx->device, image->image, &memory.requirements);
	memory.property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	EXIT_NOT_IMPLEMENTED(!VulkanAllocate(m_graphic_ctx, &memory));
	VulkanBindImageMemory(m_graphic_ctx, image, &memory);
	image->memory = memory;

	VkImageViewCreateInfo view_info {};
	view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image                           = image->image;
	view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format                          = image->format;
	view_info.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	view_info.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	view_info.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	view_info.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.baseMipLevel   = 0;
	view_info.subresourceRange.levelCount     = 1;
	view_info.subresourceRange.baseArrayLayer = 0;
	view_info.subresourceRange.layerCount     = 1;

	const auto view_result =
	    vkCreateImageView(m_graphic_ctx->device, &view_info, nullptr, &image->image_view[VulkanImage::VIEW_DEFAULT]);
	EXIT_NOT_IMPLEMENTED(view_result != VK_SUCCESS || image->image_view[VulkanImage::VIEW_DEFAULT] == nullptr);

	uint32_t zero_pixels[NullSampledImageContract::upload_pitch_texels] = {};
	CommandBuffer upload(GraphicContext::QUEUE_GFX);
	upload.Begin();
	UtilUploadImageForShaderStage(m_graphic_ctx, &upload, image, zero_pixels, sizeof(zero_pixels),
	                              NullSampledImageContract::upload_pitch_texels,
	                              static_cast<uint64_t>(NullSampledImageContract::final_layout),
	                              static_cast<uint64_t>(NullSampledImageContract::final_stage));
	upload.End();
	upload.Execute();
	upload.WaitForFence();
	EXIT_IF(image->layout != NullSampledImageContract::final_layout);

	m_null_sampled_image = image;
	return m_null_sampled_image;
}

bool RenderContext::HasNullSampledImage()
{
	Core::LockGuard lock(m_null_sampled_image_mutex);
	return m_null_sampled_image != nullptr;
}

bool GraphicsRenderReportNullSampledImageFinal()
{
	const bool enabled      = g_null_sampled_image_enabled;
	const uint64_t prepared = g_null_sampled_image_prepared.load(std::memory_order_acquire);
	const bool created       = g_render_ctx != nullptr && g_render_ctx->HasNullSampledImage();
	const bool clean         = (!enabled && prepared == 0 && !created) || (enabled && (prepared == 0 || created));
	std::fprintf(stderr,
	             "[GFX_NULL_SAMPLED_FINAL] enabled=%u prepared=%" PRIu64
	             " dummy_created=%u backend=shared_zero_rgba8 status=%s\n",
	             enabled ? 1u : 0u, prepared, created ? 1u : 0u, clean ? "clean" : "unhealthy");
	std::fflush(stderr);
	return clean;
}

bool GraphicsRenderResetForNextSession()
{
	return g_render_ctx != nullptr && g_render_ctx->ResetForNextSession();
}

uint32_t QuadListIndexBufferCache::CalcCapacity(uint32_t current, uint32_t required)
{
	constexpr uint64_t max_quad_count = std::numeric_limits<uint32_t>::max() / QUAD_LIST_INDICES_PER_QUAD;

	EXIT_IF(required == 0);
	EXIT_IF(required > max_quad_count);
	EXIT_IF(current > max_quad_count);

	uint64_t capacity = (current == 0 ? 1 : current);
	while (capacity < required)
	{
		if (capacity > max_quad_count / 2)
		{
			capacity = required;
			break;
		}
		capacity *= 2;
	}

	EXIT_IF(capacity < required || capacity > max_quad_count);
	return static_cast<uint32_t>(capacity);
}

template <typename IndexT>
VulkanBuffer* QuadListIndexBufferCache::CreateBuffer(GraphicContext* ctx, uint32_t quad_capacity)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(quad_capacity == 0);

	const uint64_t index_capacity = static_cast<uint64_t>(quad_capacity) * QUAD_LIST_INDICES_PER_QUAD;
	EXIT_IF(index_capacity == 0 || index_capacity > std::numeric_limits<uint32_t>::max());
	EXIT_IF(index_capacity > std::numeric_limits<size_t>::max() / sizeof(IndexT));
	EXIT_IF(GetQuadListIndexValue(index_capacity - 1) > std::numeric_limits<IndexT>::max());

	const uint64_t buffer_size = index_capacity * sizeof(IndexT);

	auto* buffer = new VulkanBuffer;

	buffer->usage           = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	buffer->memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	buffer->buffer          = nullptr;

	VulkanCreateBuffer(ctx, buffer_size, buffer);
	EXIT_NOT_IMPLEMENTED(buffer->buffer == nullptr);

	void* data = nullptr;
	VulkanMapMemory(ctx, &buffer->memory, &data);
	EXIT_IF(data == nullptr);

	auto* indices = static_cast<IndexT*>(data);
	for (uint64_t index = 0; index < index_capacity; index++)
	{
		indices[index] = static_cast<IndexT>(GetQuadListIndexValue(index));
	}

	VulkanUnmapMemory(ctx, &buffer->memory);

	return buffer;
}

VulkanBuffer* QuadListIndexBufferCache::GetBuffer(GraphicContext* ctx, uint32_t vertex_count, VkIndexType* index_type,
                                                  uint32_t* draw_index_count)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(index_type == nullptr || draw_index_count == nullptr);
	EXIT_IF(vertex_count == 0 || (vertex_count % QUAD_LIST_VERTICES_PER_QUAD) != 0);

	const uint32_t quad_count       = vertex_count / QUAD_LIST_VERTICES_PER_QUAD;
	const uint64_t index_count      = GetQuadListIndexCount(vertex_count);
	const bool     use_uint16       = vertex_count <= static_cast<uint64_t>(std::numeric_limits<uint16_t>::max()) + 1;
	const auto     selected_type    = use_uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

	EXIT_IF(index_count == 0 || index_count > std::numeric_limits<uint32_t>::max());
	EXIT_IF(GetQuadListIndexValue(index_count - 1) >= vertex_count);

	Core::LockGuard lock(m_mutex);

	auto& slot = (use_uint16 ? m_uint16 : m_uint32);
	if (slot.buffer == nullptr || slot.quad_capacity < quad_count)
	{
		const uint32_t new_capacity = CalcCapacity(slot.quad_capacity, quad_count);
		auto*          new_buffer =
		    (use_uint16 ? CreateBuffer<uint16_t>(ctx, new_capacity) : CreateBuffer<uint32_t>(ctx, new_capacity));

		EXIT_IF(new_buffer == nullptr || new_buffer->buffer == nullptr);

		m_buffers.Add(new_buffer);
		slot.buffer        = new_buffer;
		slot.quad_capacity = new_capacity;
	}

	EXIT_IF(slot.buffer == nullptr || slot.buffer->buffer == nullptr || slot.quad_capacity < quad_count);

	*index_type       = selected_type;
	*draw_index_count = static_cast<uint32_t>(index_count);
	return slot.buffer;
}

void RenderContext::AddEqEvent(const LibKernel::EventQueue::KernelEqueueIdentity& identity, int id)
{
	Core::LockGuard lock(m_event_mutex);
	const auto found = std::find_if(m_event_registrations.begin(), m_event_registrations.end(),
	    [&](const auto& entry) { return entry.identity.queue == identity.queue && entry.id == id; });
	if (found != m_event_registrations.end()) found->identity = identity;
	else m_event_registrations.push_back({identity, id});
}

void RenderContext::DeleteEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id, uint64_t generation)
{
	Core::LockGuard lock(m_event_mutex);
	const auto found = std::find_if(m_event_registrations.begin(), m_event_registrations.end(),
	    [&](const auto& entry) {
		    return entry.identity.queue == eq && entry.id == id &&
		           (generation == 0 || entry.identity.generation == generation);
	    });
	if (found != m_event_registrations.end()) m_event_registrations.erase(found);
}

void RenderContext::TriggerEvent(int id, uint64_t data)
{
	std::vector<EventRegistration> registrations;
	{
		Core::LockGuard lock(m_event_mutex);
		registrations = m_event_registrations;
	}
	// Queue deletion invokes our callback under the queue lock. Never retain
	// the renderer lock while entering the registry or the queue.
	for (const auto& registration: registrations)
	{
		if (registration.id != id) continue;
		const auto result = LibKernel::EventQueue::TriggerEventIfRegistered(registration.identity, id,
		    LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS, reinterpret_cast<void*>(data));
		if (result == LibKernel::KERNEL_ERROR_EBADF)
			DeleteEqEvent(registration.identity.queue, id, registration.identity.generation);
		else EXIT_NOT_IMPLEMENTED(result != OK && result != LibKernel::KERNEL_ERROR_ENOENT);
	}
}

void RenderContext::TriggerEopEvent()
{
	TriggerEvent(GRAPHICS_EVENT_EOP, LibKernel::KernelReadTsc());
}

bool RenderContext::ResetForNextSession()
{
	Core::LockGuard lock(m_mutex);
	AbortGfxResourceCaptureForSessionReset();

	std::vector<EventRegistration> registrations;
	{
		Core::LockGuard event_lock(m_event_mutex);
		registrations = m_event_registrations;
	}
	for (const auto& registration: registrations)
	{
		const int result = LibKernel::EventQueue::DeleteEventIfRegistered(registration.identity, registration.id,
		    LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS);
		if (result != OK && result != LibKernel::KERNEL_ERROR_EBADF && result != LibKernel::KERNEL_ERROR_ENOENT)
		{
			return false;
		}
	}
	{
		Core::LockGuard event_lock(m_event_mutex);
		m_event_registrations.clear();
	}

	if (m_graphic_ctx == nullptr)
	{
		// No command submission or video-buffer registration reached the
		// renderer. There are no device-backed guest caches to tear down.
		return true;
	}
	if (m_graphic_ctx->device == nullptr)
	{
		return true;
	}

	// Pipelines depend on render passes, while descriptor sets and
	// framebuffers refer to Vulkan objects derived from guest mappings.
	// The session boundary has already drained workers and the device. Retire
	// the idle readback/vertex allocations before the context can be reused.
	if (!UtilResetReadbackStaging(m_graphic_ctx) || !VertexBufferResetReplacementCache(m_graphic_ctx) ||
	    !UtilResetUploadStaging(m_graphic_ctx))
	{
		return false;
	}
	m_pipeline_cache->DeleteAllPipelines();
	m_descriptor_cache->ResetForNextSession();
	m_framebuffer_cache->ResetForNextSession();
	m_gds_buffer->ResetForNextSession(m_graphic_ctx);
	return true;
}

void GdsBuffer::Init(GraphicContext* ctx)
{
	if (m_buffer == nullptr)
	{
		m_buffer = new VulkanBuffer;

		m_buffer->usage           = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		m_buffer->memory.property = static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
		                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
		m_buffer->buffer = nullptr;

		VulkanCreateBuffer(ctx, DW_SIZE * 4, m_buffer);
		EXIT_NOT_IMPLEMENTED(m_buffer->buffer == nullptr);
	}
}

void GdsBuffer::Clear(GraphicContext* ctx, uint64_t dw_offset, uint32_t dw_num, uint32_t clear_value)
{
	EXIT_IF(ctx == nullptr);

	Core::LockGuard lock(m_mutex);

	Init(ctx);

	EXIT_NOT_IMPLEMENTED(dw_offset >= DW_SIZE);
	EXIT_NOT_IMPLEMENTED(dw_offset + dw_num > DW_SIZE);

	EXIT_IF(m_buffer == nullptr);

	void* data = nullptr;
	VulkanMapMemory(ctx, &m_buffer->memory, &data);

	EXIT_IF(data == nullptr);

	for (uint32_t i = 0; i < dw_num; i++)
	{
		static_cast<uint32_t*>(data)[dw_offset + i] = clear_value;
	}

	VulkanUnmapMemory(ctx, &m_buffer->memory);
}

void GdsBuffer::Read(GraphicContext* ctx, uint32_t* dst, uint32_t dw_offset, uint32_t dw_size)
{
	EXIT_IF(dst == nullptr);

	Core::LockGuard lock(m_mutex);

	Init(ctx);

	EXIT_NOT_IMPLEMENTED(dw_offset >= DW_SIZE);
	EXIT_NOT_IMPLEMENTED(dw_size > DW_SIZE - dw_offset);

	EXIT_IF(m_buffer == nullptr);

	void* data = nullptr;
	VulkanMapMemory(ctx, &m_buffer->memory, &data);

	EXIT_IF(data == nullptr);

	for (uint32_t i = 0; i < dw_size; i++)
	{
		dst[i] = static_cast<uint32_t*>(data)[dw_offset + i];
	}

	VulkanUnmapMemory(ctx, &m_buffer->memory);
}

void GdsBuffer::ResetForNextSession(GraphicContext* ctx)
{
	EXIT_IF(ctx == nullptr);
	Core::LockGuard lock(m_mutex);
	if (m_buffer == nullptr)
	{
		return;
	}

	void* data = nullptr;
	VulkanMapMemory(ctx, &m_buffer->memory, &data);
	EXIT_IF(data == nullptr);
	for (uint32_t index = 0; index < DW_SIZE; index++)
	{
		static_cast<uint32_t*>(data)[index] = 0;
	}
	VulkanUnmapMemory(ctx, &m_buffer->memory);
}

VulkanBuffer* GdsBuffer::GetBuffer(GraphicContext* ctx)
{
	Core::LockGuard lock(m_mutex);

	Init(ctx);

	return m_buffer;
}

VulkanCommandPool* CommandPool::Create(int id)
{
	auto* ctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(id < 0 || id >= GraphicContext::QUEUES_NUM);

	auto* pool = new VulkanCommandPool;

	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->device == nullptr);
	EXIT_IF(ctx->queues[id].family == static_cast<uint32_t>(-1));
	EXIT_IF(pool->pool != nullptr);
	EXIT_IF(pool->buffers != nullptr);
	EXIT_IF(pool->fences != nullptr);
	EXIT_IF(pool->semaphores != nullptr);
	EXIT_IF(pool->buffers_count != 0);

	VkCommandPoolCreateInfo pool_info {};
	pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.pNext            = nullptr;
	pool_info.queueFamilyIndex = ctx->queues[id].family;
	pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

	vkCreateCommandPool(ctx->device, &pool_info, nullptr, &pool->pool);

	EXIT_NOT_IMPLEMENTED(pool->pool == nullptr);

	pool->buffers_count = VulkanCommandPool::PROCESSOR_BUFFER_COUNT;
	pool->buffers       = new VkCommandBuffer[pool->buffers_count];
	pool->fences        = new VkFence[pool->buffers_count];
	pool->semaphores    = new VkSemaphore[pool->buffers_count];
	pool->busy          = new bool[pool->buffers_count];

	VkCommandBufferAllocateInfo alloc_info {};
	alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	alloc_info.commandPool        = pool->pool;
	alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc_info.commandBufferCount = pool->buffers_count;

	if (vkAllocateCommandBuffers(ctx->device, &alloc_info, pool->buffers) != VK_SUCCESS)
	{
		EXIT("Can't allocate command buffers");
	}

	for (uint32_t i = 0; i < pool->buffers_count; i++)
	{
		pool->busy[i] = false;

		VkFenceCreateInfo fence_info {};
		fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fence_info.pNext = nullptr;
		fence_info.flags = 0;

		if (vkCreateFence(ctx->device, &fence_info, nullptr, &pool->fences[i]) != VK_SUCCESS)
		{
			EXIT("Can't create fence");
		}

		VkSemaphoreCreateInfo semaphore_info {};
		semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		semaphore_info.pNext = nullptr;
		semaphore_info.flags = 0;

		if (vkCreateSemaphore(ctx->device, &semaphore_info, nullptr, &pool->semaphores[i]) != VK_SUCCESS)
		{
			EXIT("Can't create semaphore");
		}

		EXIT_IF(pool->buffers[i] == nullptr);
		EXIT_IF(pool->fences[i] == nullptr);
		EXIT_IF(pool->semaphores[i] == nullptr);
	}
	return pool;
}

void CommandPool::DeleteAll()
{
	auto* ctx = g_render_ctx->GetGraphicCtx();

	for (auto& chunks: m_pool)
	{
		for (auto* pool: chunks)
		{
			EXIT_IF(pool == nullptr);
			EXIT_IF(ctx == nullptr);
			EXIT_IF(ctx->device == nullptr);

			for (uint32_t i = 0; i < pool->buffers_count; i++)
			{
				vkDestroySemaphore(ctx->device, pool->semaphores[i], nullptr);
				vkDestroyFence(ctx->device, pool->fences[i], nullptr);
			}

			vkFreeCommandBuffers(ctx->device, pool->pool, pool->buffers_count, pool->buffers);

			vkDestroyCommandPool(ctx->device, pool->pool, nullptr);

			delete[] pool->semaphores;
			delete[] pool->fences;
			delete[] pool->buffers;
			delete[] pool->busy;

			delete pool;
		}
		chunks.clear();
	}
}

VulkanFramebuffer* FramebufferCache::CreateFramebuffer(RenderColorInfo* color, RenderDepthInfo* depth)
{
	LUMEN_PROFILER_FUNCTION();

	static std::atomic<uint64_t> seq = 0;

	Core::LockGuard lock(m_mutex);

	EXIT_IF(color == nullptr);
	EXIT_IF(depth == nullptr);
	EXIT_IF(g_render_ctx == nullptr);

	bool with_depth = (depth->format != VK_FORMAT_UNDEFINED && depth->vulkan_buffer != nullptr);
	bool with_color = (color->vulkan_buffer != nullptr);

	for (auto& f: m_framebuffers)
	{
		if (f.framebuffer != nullptr && f.image_id == (with_color ? color->vulkan_buffer->memory.unique_id : 0) &&
		    f.depth_id == (with_depth ? depth->vulkan_buffer->memory.unique_id : 0) && f.depth_clear_enable == depth->depth_clear_enable &&
		    f.stencil_clear_enable == depth->stencil_clear_enable)
		{
			return f.framebuffer;
		}
	}

	auto* framebuffer        = new VulkanFramebuffer;
	framebuffer->render_pass = nullptr;
	framebuffer->framebuffer = nullptr;

	auto* gctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(gctx == nullptr);

	EXIT_NOT_IMPLEMENTED(!with_depth && !with_color);
	EXIT_NOT_IMPLEMENTED(with_depth && with_color &&
	                     (color->vulkan_buffer->extent.width != depth->vulkan_buffer->extent.width ||
	                      color->vulkan_buffer->extent.height != depth->vulkan_buffer->extent.height));

	VulkanImage* vulkan_buffer =
	    (with_color ? color->vulkan_buffer
	                : CreateDummyBuffer(VK_FORMAT_B8G8R8A8_SRGB, depth->vulkan_buffer->extent.width, depth->vulkan_buffer->extent.height));

	VkAttachmentDescription attachments[2];
	attachments[0].flags          = 0;
	attachments[0].format         = vulkan_buffer->format;
	attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachments[0].finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	attachments[1].flags          = 0;
	attachments[1].format         = depth->format;
	attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp         = (depth->depth_clear_enable ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);
	attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[1].stencilLoadOp  = (depth->stencil_clear_enable ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[1].initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference color_attachment_ref {};
	color_attachment_ref.attachment = (with_color ? 0 : VK_ATTACHMENT_UNUSED);
	color_attachment_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference depth_attachment_ref {};
	depth_attachment_ref.attachment = 1;
	depth_attachment_ref.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass {};
	subpass.flags                   = 0;
	subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.inputAttachmentCount    = 0;
	subpass.pInputAttachments       = nullptr;
	subpass.colorAttachmentCount    = 1;
	subpass.pColorAttachments       = &color_attachment_ref;
	subpass.pResolveAttachments     = nullptr;
	subpass.pDepthStencilAttachment = (with_depth ? &depth_attachment_ref : nullptr);
	subpass.preserveAttachmentCount = 0;
	subpass.pPreserveAttachments    = nullptr;

	const VkSubpassDependency dependency = RenderPassAttachmentDependency();

	VkRenderPassCreateInfo render_pass_info {};
	render_pass_info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	render_pass_info.pNext           = nullptr;
	render_pass_info.flags           = 0;
	render_pass_info.attachmentCount = (with_depth ? 2 : 1);
	render_pass_info.pAttachments    = attachments;
	render_pass_info.subpassCount    = 1;
	render_pass_info.pSubpasses      = &subpass;
	render_pass_info.dependencyCount = 1;
	render_pass_info.pDependencies   = &dependency;

	vkCreateRenderPass(gctx->device, &render_pass_info, nullptr, &framebuffer->render_pass);

	framebuffer->render_pass_id = ++seq;

	EXIT_NOT_IMPLEMENTED(framebuffer->render_pass == nullptr);

	VkImageView views[] = {vulkan_buffer->image_view[VulkanImage::VIEW_DEFAULT],
	                       (with_depth ? depth->vulkan_buffer->image_view[VulkanImage::VIEW_DEFAULT] : nullptr)};

	VkFramebufferCreateInfo framebuffer_info {};
	framebuffer_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer_info.pNext           = nullptr;
	framebuffer_info.flags           = 0;
	framebuffer_info.renderPass      = framebuffer->render_pass;
	framebuffer_info.attachmentCount = with_depth ? 2 : 1;
	framebuffer_info.pAttachments    = views;
	framebuffer_info.width           = vulkan_buffer->extent.width;
	framebuffer_info.height          = vulkan_buffer->extent.height;
	framebuffer_info.layers          = 1;

	vkCreateFramebuffer(gctx->device, &framebuffer_info, nullptr, &framebuffer->framebuffer);

	EXIT_NOT_IMPLEMENTED(framebuffer->framebuffer == nullptr);

	Framebuffer fnew;
	fnew.framebuffer          = framebuffer;
	fnew.image_id             = (with_color ? color->vulkan_buffer->memory.unique_id : 0);
	fnew.depth_id             = (with_depth ? depth->vulkan_buffer->memory.unique_id : 0);
	fnew.depth_clear_enable   = depth->depth_clear_enable;
	fnew.stencil_clear_enable = depth->stencil_clear_enable;

	bool updated = false;

	for (auto& f: m_framebuffers)
	{
		if (f.framebuffer == nullptr)
		{
			f       = fnew;
			updated = true;
			break;
		}
	}

	if (!updated)
	{
		m_framebuffers.Add(fnew);
	}

	return framebuffer;
}

void FramebufferCache::FreeFramebufferByColor(VulkanImage* image)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(image == nullptr);

	Core::LockGuard lock(m_mutex);
	FreeFramebuffersMatchingLocked(image->memory.unique_id, false);
}

void FramebufferCache::FreeFramebufferByDepth(DepthStencilVulkanImage* image)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(image == nullptr);

	Core::LockGuard lock(m_mutex);
	FreeFramebuffersMatchingLocked(image->memory.unique_id, true);
}

void FramebufferCache::FreeFramebuffersMatchingLocked(uint64_t image_id, bool match_depth)
{
	auto* gctx           = g_render_ctx->GetGraphicCtx();
	auto* pipeline_cache = g_render_ctx->GetPipelineCache();
	EXIT_IF(gctx == nullptr || gctx->device == nullptr || pipeline_cache == nullptr);

	ReleaseMatching(
	    &m_framebuffers,
	    [image_id, match_depth](const Framebuffer& entry) {
		    return (match_depth ? entry.depth_id : entry.image_id) == image_id;
	    },
	    [gctx, pipeline_cache](VulkanFramebuffer* framebuffer) {
		    pipeline_cache->DeletePipelines(framebuffer);
		    vkDestroyFramebuffer(gctx->device, framebuffer->framebuffer, nullptr);
		    vkDestroyRenderPass(gctx->device, framebuffer->render_pass, nullptr);
		    delete framebuffer;
	    });
}

#if defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
bool GraphicsRenderFramebufferCacheReleaseAllContractTest()
{
	VulkanFramebuffer live[6] {};
	Vector<FramebufferCache::Framebuffer> entries;
	const uint64_t color_id       = 11;
	const uint64_t depth_id       = 21;
	const uint64_t other_color_id = 12;
	const uint64_t other_depth_id = 22;
	const uint64_t third_color_id = 13;

	auto add = [&entries](VulkanFramebuffer* framebuffer, uint64_t image_id, uint64_t entry_depth_id) {
		FramebufferCache::Framebuffer entry {};
		entry.framebuffer = framebuffer;
		entry.image_id    = image_id;
		entry.depth_id    = entry_depth_id;
		entries.Add(entry);
	};
	add(&live[0], color_id, depth_id);
	add(&live[1], color_id, other_depth_id);
	add(&live[2], other_color_id, depth_id);
	add(&live[3], color_id, depth_id);
	add(&live[4], third_color_id, depth_id);
	add(nullptr, color_id, depth_id);

	uint32_t release_calls          = 0;
	bool     callback_saw_live_entry = true;
	auto release = [&entries, &release_calls, &callback_saw_live_entry](VulkanFramebuffer* framebuffer) {
		bool found_live = false;
		for (const auto& entry: entries)
		{
			found_live = found_live || entry.framebuffer == framebuffer;
		}
		callback_saw_live_entry = callback_saw_live_entry && found_live;
		++release_calls;
	};

	const uint32_t color_released = FramebufferCache::ReleaseMatching(
	    &entries, [](const FramebufferCache::Framebuffer& entry) { return entry.image_id == color_id; }, release);
	if (color_released != 3 || release_calls != 3 || !callback_saw_live_entry ||
	    entries[0].framebuffer != nullptr || entries[1].framebuffer != nullptr ||
	    entries[2].framebuffer != &live[2] || entries[3].framebuffer != nullptr ||
	    entries[4].framebuffer != &live[4] || entries[5].framebuffer != nullptr)
	{
		return false;
	}

	const uint32_t depth_released = FramebufferCache::ReleaseMatching(
	    &entries, [](const FramebufferCache::Framebuffer& entry) { return entry.depth_id == depth_id; }, release);
	return depth_released == 2 && release_calls == 5 && callback_saw_live_entry &&
	       entries[2].framebuffer == nullptr && entries[4].framebuffer == nullptr;
}
#endif

void FramebufferCache::ResetForNextSession()
{
	Core::LockGuard lock(m_mutex);
	auto*           gctx = g_render_ctx != nullptr ? g_render_ctx->GetGraphicCtx() : nullptr;
	EXIT_IF(gctx == nullptr || gctx->device == nullptr);

	for (auto& framebuffer: m_framebuffers)
	{
		if (framebuffer.framebuffer == nullptr)
		{
			continue;
		}
		vkDestroyFramebuffer(gctx->device, framebuffer.framebuffer->framebuffer, nullptr);
		vkDestroyRenderPass(gctx->device, framebuffer.framebuffer->render_pass, nullptr);
		delete framebuffer.framebuffer;
		framebuffer.framebuffer = nullptr;
	}
	m_framebuffers.Clear();
}

VideoOutVulkanImage* FramebufferCache::CreateDummyBuffer(VkFormat format, uint32_t width, uint32_t height)
{
	for (auto* b: m_dummy_buffers)
	{
		if (b->extent.width == width && b->extent.height == height && b->format == format)
		{
			return b;
		}
	}

	auto* ctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(ctx == nullptr);

	auto* vk_obj = new VideoOutVulkanImage;

	vk_obj->extent.width  = width;
	vk_obj->extent.height = height;
	vk_obj->format        = format;
	vk_obj->image         = nullptr;
	vk_obj->layout        = VK_IMAGE_LAYOUT_UNDEFINED;

	for (auto& view: vk_obj->image_view)
	{
		view = nullptr;
	}

	VkImageCreateInfo image_info {};
	image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.pNext         = nullptr;
	image_info.flags         = 0;
	image_info.imageType     = VK_IMAGE_TYPE_2D;
	image_info.extent.width  = vk_obj->extent.width;
	image_info.extent.height = vk_obj->extent.height;
	image_info.extent.depth  = 1;
	image_info.mipLevels     = 1;
	image_info.arrayLayers   = 1;
	image_info.format        = vk_obj->format;
	image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
	image_info.initialLayout = vk_obj->layout;
	image_info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	image_info.samples       = VK_SAMPLE_COUNT_1_BIT;

	vkCreateImage(ctx->device, &image_info, nullptr, &vk_obj->image);

	EXIT_NOT_IMPLEMENTED(vk_obj->image == nullptr);

	VulkanMemory mem;

	vkGetImageMemoryRequirements(ctx->device, vk_obj->image, &mem.requirements);

	mem.property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	bool allocated = VulkanAllocate(ctx, &mem);

	EXIT_NOT_IMPLEMENTED(!allocated);

	VulkanBindImageMemory(ctx, vk_obj, &mem);

	vk_obj->memory = mem;

	VkImageViewCreateInfo create_info {};
	create_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create_info.pNext                           = nullptr;
	create_info.flags                           = 0;
	create_info.image                           = vk_obj->image;
	create_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
	create_info.format                          = vk_obj->format;
	create_info.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	create_info.subresourceRange.baseArrayLayer = 0;
	create_info.subresourceRange.baseMipLevel   = 0;
	create_info.subresourceRange.layerCount     = 1;
	create_info.subresourceRange.levelCount     = 1;

	vkCreateImageView(ctx->device, &create_info, nullptr, &vk_obj->image_view[VulkanImage::VIEW_DEFAULT]);

	EXIT_NOT_IMPLEMENTED(vk_obj->image_view[VulkanImage::VIEW_DEFAULT] == nullptr);

	UtilSetImageLayoutOptimal(vk_obj);

	m_dummy_buffers.Add(vk_obj);

	return vk_obj;
}

VkSampler SamplerCache::GetSampler(uint64_t id)
{
	Core::LockGuard lock(m_mutex);
	if (id < m_samplers.Size())
	{
		return m_samplers.At(id).vk;
	}
	return nullptr;
}

static bool SamplerDescriptorsEqual(const ShaderSamplerResource& a, const ShaderSamplerResource& b)
{
	return a.fields[0] == b.fields[0] && a.fields[1] == b.fields[1] && a.fields[2] == b.fields[2] && a.fields[3] == b.fields[3];
}

static void SetOrdinarySamplerComparison(const ShaderSamplerResource& r, VkSamplerCreateInfo* sampler_info)
{
	// Supported IMAGE_SAMPLE instructions have no z-comp operand and lower to non-Dref sampling.
	// DepthCompareFunc is dormant for those instructions; Vulkan comparison must remain disabled.
	// Comparison opcodes remain rejected by the parser. A future Dref path needs its own sampler use identity.
	sampler_info->compareEnable = VK_FALSE;
	sampler_info->compareOp     = static_cast<VkCompareOp>(r.DepthCompareFunc());
}

static ShaderType ShaderTypeForDescriptorStage(DescriptorCache::Stage stage)
{
	return stage == DescriptorCache::Stage::Vertex ? ShaderType::Vertex
	       : stage == DescriptorCache::Stage::Pixel ? ShaderType::Pixel
	       : stage == DescriptorCache::Stage::Compute ? ShaderType::Compute
	                                                 : ShaderType::Unknown;
}

static bool SamplerImagesHaveNoZCoordinate(const ShaderBindResources& bind, DescriptorCache::Stage stage)
{
	const auto shader_type = ShaderTypeForDescriptorStage(stage);
	const auto& textures = bind.textures2D;
	if (shader_type == ShaderType::Unknown || bind.samplers.samplers_num <= 0 ||
	    bind.samplers.samplers_num > ShaderSamplerResources::RES_MAX || textures.textures_num <= 0 ||
	    textures.textures_num > ShaderTextureResources::RES_MAX || textures.textures2d_sampled_num != textures.textures_num ||
	    textures.textures2d_storage_num != 0) return false;
	// All possible images paired with these samplers must have no Z axis.
	// Keep mixed image-load/storage declarations outside this added profile;
	// it does not infer a sampler/image pairing from slots or guest values.
	for (int index = 0; index < textures.textures_num; ++index)
	{
		const auto& desc = textures.desc[index];
		const auto& texture = desc.texture;
		if (desc.usage != ShaderTextureUsage::ReadOnly || desc.textures2d_without_sampler || desc.IsNullSampledImage() ||
		    !ShaderTextureDescriptorDimensionSupported(desc, shader_type) || texture.Depth() != 0 ||
		    texture.BaseArray() != 0 || texture.LastArray() != 0) return false;
		if (texture.Type() != ShaderTextureResource::TYPE_2D &&
		    !(shader_type == ShaderType::Pixel && texture.Type() == ShaderTextureResource::TYPE_8_COMPAT &&
		      ShaderTextureType8PixelSampleCompatible(texture))) return false;
	}
	return true;
}

static bool SamplerZFilterSupported(const ShaderSamplerResource& sampler, bool images_have_no_z_coordinate)
{
	// Preserve the legacy value. Sea Islands defines 0=NONE, 1=POINT, 2=LINEAR;
	// add 0/2 only for the proven no-Z profile. Value 3 remains unqualified.
	return sampler.ZFilter() == 1 || (sampler.ZFilter() < 3 && images_have_no_z_coordinate);
}

#if defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
bool GraphicsRenderOrdinarySamplerComparisonContractTest()
{
	ShaderSamplerResource descriptors[24] {};
	for (uint32_t index = 0; index < 24; ++index)
	{
		const uint32_t function = index & 7u, z_filter = index >> 3u;
		auto& descriptor = descriptors[index];
		descriptor.fields[0] = function << 12u;
		descriptor.fields[1] = 0x00fff000u;
		descriptor.fields[2] = (1u << 20u) | (1u << 22u) | (z_filter << 24u);
		const auto original = descriptor;
		VkSamplerCreateInfo info {};
		info.compareEnable = VK_TRUE;
		info.minLod = 1.5f;
		info.maxLod = 7.25f;
		SetOrdinarySamplerComparison(descriptor, &info);
		if (info.compareEnable != VK_FALSE || info.compareOp != static_cast<VkCompareOp>(function) ||
		    info.minLod != 1.5f || info.maxLod != 7.25f || !SamplerDescriptorsEqual(descriptor, original)) return false;
		for (uint32_t other = 0; other <= index; ++other)
			if (SamplerDescriptorsEqual(descriptor, descriptors[other]) != (index == other)) return false;
		for (uint32_t word = 0; word < 4; ++word)
		{
			auto changed = descriptor;
			changed.fields[word] ^= 1u;
			if (SamplerDescriptorsEqual(descriptor, changed)) return false;
		}
	}
	std::fprintf(stderr, "[GFX_ORDINARY_SAMPLER_COMPARISON_TEST] functions=8 z_filter=0/1/2 comparison=disabled descriptor=retained cache=raw4-separated\n");
	return true;
}

bool GraphicsRenderSamplerZFilterDimensionContractTest()
{
	for (uint32_t filter = 0; filter < 4; ++filter)
	{
		ShaderSamplerResource sampler {};
		sampler.fields[2] = filter << 24u;
		if (SamplerZFilterSupported(sampler, true) != (filter < 3) ||
		    SamplerZFilterSupported(sampler, false) != (filter == 1)) return false;
	}
	ShaderBindResources bind {};
	bind.samplers.samplers_num = 1;
	bind.textures2D.textures_num = 1;
	bind.textures2D.textures2d_sampled_num = 1;
	bind.textures2D.desc[0].usage = ShaderTextureUsage::ReadOnly;
	bind.textures2D.desc[0].texture.fields[3] = ShaderTextureResource::TYPE_2D << 28u;
	for (const auto stage: {DescriptorCache::Stage::Vertex, DescriptorCache::Stage::Pixel, DescriptorCache::Stage::Compute})
		if (!SamplerImagesHaveNoZCoordinate(bind, stage)) return false;
	if (SamplerImagesHaveNoZCoordinate(bind, DescriptorCache::Stage::Unknown)) return false;
	for (uint32_t type = 0; type < 16; ++type)
	{
		auto other = bind;
		other.textures2D.desc[0].texture.fields[3] = type << 28u;
		if (SamplerImagesHaveNoZCoordinate(other, DescriptorCache::Stage::Pixel) != (type == ShaderTextureResource::TYPE_2D)) return false;
	}
	for (uint32_t mutation = 0; mutation < 10; ++mutation)
	{
		auto other = bind;
		auto& desc = other.textures2D.desc[0];
		if (mutation == 0) desc.texture.fields[4] |= 1u;
		if (mutation == 1) desc.texture.fields[5] |= 1u;
		if (mutation == 2) desc.texture.fields[5] |= 1u << 13u;
		if (mutation == 3) desc.textures2d_without_sampler = true;
		if (mutation == 4) desc.usage = ShaderTextureUsage::Unknown;
		if (mutation == 5) desc.usage = ShaderTextureUsage::ReadWrite;
		if (mutation == 6) other.textures2D.textures_num = other.textures2D.textures2d_sampled_num = 0;
		if (mutation == 7) other.textures2D.textures2d_sampled_num = 0;
		if (mutation == 8) other.textures2D.textures2d_storage_num = 1;
		if (mutation == 9) other.samplers.samplers_num = 0;
		if (SamplerImagesHaveNoZCoordinate(other, DescriptorCache::Stage::Pixel)) return false;
	}
	auto bridge = bind;
	auto& texture = bridge.textures2D.desc[0].texture;
	texture.fields[1] = 10u << 20u;
	texture.fields[3] = (ShaderTextureResource::TYPE_8_COMPAT << 28u) | (1u << 26u) | (8u << 20u) | 0xfacu;
	if (!ShaderTextureType8PixelSampleCompatible(texture) || !SamplerImagesHaveNoZCoordinate(bridge, DescriptorCache::Stage::Pixel) ||
	    SamplerImagesHaveNoZCoordinate(bridge, DescriptorCache::Stage::Vertex) ||
	    SamplerImagesHaveNoZCoordinate(bridge, DescriptorCache::Stage::Compute)) return false;
	texture.fields[2] |= 1u << 14u;
	if (SamplerImagesHaveNoZCoordinate(bridge, DescriptorCache::Stage::Pixel)) return false;
	// The same sampler cache key must not substitute for current image proof.
	// Check a valid-invalid-valid bind with a second image declaration.
	auto mixed = bind;
	mixed.textures2D.textures_num = mixed.textures2D.textures2d_sampled_num = 2;
	mixed.textures2D.desc[1] = mixed.textures2D.desc[0];
	mixed.textures2D.desc[1].start_register = 8;
	mixed.textures2D.desc[1].slot = 1;
	for (const uint32_t filter: {0u, 2u})
	{
		ShaderSamplerResource sampler {};
		sampler.fields[2] = filter << 24u;
		const auto original_sampler = sampler;
		for (const uint32_t depth: {0u, 1u, 0u})
		{
			mixed.textures2D.desc[1].texture.fields[4] = depth;
			if (!SamplerDescriptorsEqual(sampler, original_sampler) ||
			    SamplerZFilterSupported(sampler, SamplerImagesHaveNoZCoordinate(mixed, DescriptorCache::Stage::Pixel)) != (depth == 0))
				return false;
		}
	}
	mixed.textures2D.desc[1].texture.fields[3] = 11u << 28u;
	if (SamplerImagesHaveNoZCoordinate(mixed, DescriptorCache::Stage::Pixel)) return false;
	std::fprintf(stderr, "[GFX_SAMPLER_Z_FILTER_DIMENSION_TEST] zero-two=dimension-bound one=legacy three=reject"
	                     " two-dimensional=admitted one-dimensional=exact-pixel-bridge"
	                     " volume-cube-array-ms=reject unknown-or-unpaired=reject dimension-proof=current-bind"
	                     " all-images=checked cache-key-is-not-dimension-proof\n");
	return true;
}
#endif

uint64_t SamplerCache::GetSamplerId(const ShaderSamplerResource& r)
{
	Core::LockGuard lock(m_mutex);
	uint32_t        m_samplers_size = m_samplers.Size();
	for (uint32_t i = 0; i < m_samplers_size; i++)
	{
		const auto& s = m_samplers.At(i);
		if (SamplerDescriptorsEqual(s.r, r))
		{
			return i;
		}
	}
	Sampler s;
	s.r  = r;
	s.vk = nullptr;

	bool  aniso       = false;
	float aniso_ratio = 1.0f;
	auto  mag_filter  = r.XyMagFilter();
	auto  min_filter  = r.XyMinFilter();

	if (mag_filter == 2 || mag_filter == 3 || min_filter == 2 || min_filter == 3)
	{
		aniso = true;
		switch (r.MaxAnisoRatio())
		{
			case 0: aniso_ratio = 1.0f; break;
			case 1: aniso_ratio = 2.0f; break;
			case 2: aniso_ratio = 4.0f; break;
			case 3: aniso_ratio = 8.0f; break;
			case 4: aniso_ratio = 16.0f; break;
			default: EXIT("unknown ratio: %d\n", static_cast<int>(r.MaxAnisoRatio()));
		}
	}

	auto  mip_filter = r.MipFilter();
	float min_lod    = 0.0f;
	float max_lod    = 0.0f;
	if (mip_filter != 0)
	{
		min_lod = static_cast<float>(r.MinLod()) / 256.0f;
		max_lod = static_cast<float>(r.MaxLod()) / 256.0f;
	}

	VkSamplerCreateInfo sampler_info {};

	auto get_warp = [](uint8_t clamp)
	{
		switch (clamp)
		{
			case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
			case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
			case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
			case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; break;
			default: EXIT("unknown clamp: %u\n", clamp);
		}
		return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	};

	// Sampled textures are exposed to SPIR-V as floating-point images. Integer
	// border colors with those image views produce undefined sampling results.
	VkBorderColor border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	switch (r.BorderColorType())
	{
		case 0: border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK; break;
		case 1: border = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK; break;
		case 2: border = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; break;
		default: EXIT("unknown border color: %d", static_cast<int>(r.BorderColorType()));
	}

	sampler_info.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.pNext                   = nullptr;
	sampler_info.flags                   = 0;
	sampler_info.magFilter               = (mag_filter == 0 || mag_filter == 2 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
	sampler_info.minFilter               = (min_filter == 0 || min_filter == 2 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
	sampler_info.mipmapMode              = (mip_filter == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST);
	sampler_info.addressModeU            = get_warp(r.ClampX());
	sampler_info.addressModeV            = get_warp(r.ClampY());
	sampler_info.addressModeW            = get_warp(r.ClampZ());
	sampler_info.mipLodBias              = static_cast<float>(static_cast<int16_t>((r.LodBias() ^ 0x2000u) - 0x2000u)) / 256.0f;
	sampler_info.anisotropyEnable        = (aniso ? VK_TRUE : VK_FALSE);
	sampler_info.maxAnisotropy           = aniso_ratio;
	SetOrdinarySamplerComparison(r, &sampler_info);
	sampler_info.minLod                  = min_lod;
	sampler_info.maxLod                  = max_lod;
	sampler_info.borderColor             = border;
	sampler_info.unnormalizedCoordinates = (r.ForceUnormCoords() ? VK_TRUE : VK_FALSE);

	vkCreateSampler(g_render_ctx->GetGraphicCtx()->device, &sampler_info, nullptr, &s.vk);
	EXIT_NOT_IMPLEMENTED(s.vk == nullptr);

	m_samplers.Add(s);
	return m_samplers_size;
}

static bool TryGetInputFormat(const ShaderBufferResource& res, const ShaderVertexDestination& destination, VkFormat* format, uint32_t* size)
{
	if (format == nullptr || size == nullptr) return false;
	auto nfmt = res.Nfmt();
	auto dfmt = res.Dfmt();
	if (nfmt == 7 && dfmt == 14)
	{
		*format = VK_FORMAT_R32G32B32A32_SFLOAT;
		*size   = 4;
	} else if (nfmt == 7 && dfmt == 13)
	{
		*format = VK_FORMAT_R32G32B32_SFLOAT;
		*size   = 3;
	} else if (nfmt == 7 && dfmt == 11)
	{
		*format = VK_FORMAT_R32G32_SFLOAT;
		*size   = 2;
	} else if (nfmt == 0 && dfmt == 10)
	{
		*format = VK_FORMAT_R8G8B8A8_UNORM;
		*size   = 4;
	} else if (ShaderVertexInputSnorm8x4XyzCompatible(res, destination.registers_num))
	{
		*format = VK_FORMAT_R8G8B8A8_SNORM;
		*size   = 4;
	} else if (ShaderVertexInputSint16x2Compatible(res, destination.registers_num))
	{
		*format = VK_FORMAT_R16G16_SINT;
		*size   = 2;
	} else if (ShaderVertexInputSint16x4Compatible(res, destination.registers_num))
	{
		*format = VK_FORMAT_R16G16B16A16_SINT;
		*size   = 4;
	} else
	{
		return false;
	}
	return true;
}

static void get_input_format(const ShaderBufferResource& res, const ShaderVertexDestination& destination, int resource_index,
                             VkFormat* format, uint32_t* size)
{
	EXIT_IF(format == nullptr);
	EXIT_IF(size == nullptr);
	if (!TryGetInputFormat(res, destination, format, size))
	{
		EXIT("unknown vertex input format: index=%d registers=%d nfmt=%u dfmt=%u stride=%u records=%u dst_sel=%03x add_tid=%u "
		     "swizzle=%u memory=%02x raw=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 "\n",
		     resource_index, destination.registers_num, unsigned(res.Nfmt()), unsigned(res.Dfmt()), unsigned(res.Stride()),
		     res.NumRecords(), res.DstSelXYZW(), unsigned(res.AddTid()), unsigned(res.SwizzleEnabled()),
		     unsigned(res.MemoryType()), res.fields[0], res.fields[1], res.fields[2], res.fields[3]);
	}
}

static VkBlendFactor get_blend_factor(uint32_t factor)
{
	switch (factor)
	{
		case /* Zero */ 0x00000000: return VK_BLEND_FACTOR_ZERO;
		case /* One */ 0x00000001: return VK_BLEND_FACTOR_ONE;
		case /* SrcColor */ 0x00000002: return VK_BLEND_FACTOR_SRC_COLOR;
		case /* OneMinusSrcColor */ 0x00000003: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		case /* SrcAlpha */ 0x00000004: return VK_BLEND_FACTOR_SRC_ALPHA;
		case /* OneMinusSrcAlpha */ 0x00000005: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case /* DestAlpha */ 0x00000006: return VK_BLEND_FACTOR_DST_ALPHA;
		case /* OneMinusDestAlpha */ 0x00000007: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		case /* DestColor */ 0x00000008: return VK_BLEND_FACTOR_DST_COLOR;
		case /* OneMinusDestColor */ 0x00000009: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
		case /* SrcAlphaSaturate */ 0x0000000a: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
		case /* ConstantColor */ 0x0000000d: return VK_BLEND_FACTOR_CONSTANT_COLOR;
		case /* OneMinusConstantColor */ 0x0000000e: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
		case /* Src1Color */ 0x0000000f: return VK_BLEND_FACTOR_SRC1_COLOR;
		case /* InverseSrc1Color */ 0x00000010: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
		case /* Src1Alpha */ 0x00000011: return VK_BLEND_FACTOR_SRC1_ALPHA;
		case /* InverseSrc1Alpha */ 0x00000012: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
		case /* ConstantAlpha */ 0x00000013: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
		case /* OneMinusConstantAlpha */ 0x00000014: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
		default: EXIT("unknown factor: %u\n", factor);
	}
	return VK_BLEND_FACTOR_ZERO;
}

static VkBlendOp get_blend_op(uint32_t op)
{
	switch (op)
	{
		case /* Add */ 0x00000000: return VK_BLEND_OP_ADD;
		case /* Subtract */ 0x00000001: return VK_BLEND_OP_SUBTRACT;
		case /* Min */ 0x00000002: return VK_BLEND_OP_MIN;
		case /* Max */ 0x00000003: return VK_BLEND_OP_MAX;
		case /* ReverseSubtract */ 0x00000004: return VK_BLEND_OP_REVERSE_SUBTRACT;
		default: EXIT("unknown op: %u\n", op);
	}
	return VK_BLEND_OP_ADD;
}

static void SelectRuntimeConstantStorage(ShaderBindResources* first, ShaderBindResources* second)
{
	const auto& limits = g_render_ctx->GetDeviceLimits();
	EXIT_NOT_IMPLEMENTED(!ShaderSelectConstantStorage(first, second, limits.maxPushConstantsSize,
	    limits.maxUniformBufferRange, limits.maxBoundDescriptorSets));
	if (first->constant_storage == ShaderConstantStorage::UniformBuffer)
		EXIT_NOT_IMPLEMENTED(limits.maxPerStageDescriptorUniformBuffers == 0 || limits.maxDescriptorSetUniformBuffers == 0);
}

static void AppendConstantLayout(VkDescriptorSetLayout* layouts, uint32_t* count, VkShaderStageFlags stages)
{
	EXIT_IF(*count > ShaderBindResources::UNIFORM_DESCRIPTOR_SET);
	auto* cache = g_render_ctx->GetDescriptorCache();
	while (*count < ShaderBindResources::UNIFORM_DESCRIPTOR_SET)
		layouts[(*count)++] = cache->GetEmptyDescriptorSetLayout();
	layouts[(*count)++] = cache->GetConstantDescriptorSetLayout(stages);
}

static void CreateLayout(VkDescriptorSetLayout* set_layouts, uint32_t* set_layouts_num, VkPushConstantRange* push_constant_info,
                         uint32_t* push_constant_info_num, const ShaderBindResources& bind, VkShaderStageFlags vk_stage,
                         DescriptorCache::Stage stage)
{
	EXIT_IF(set_layouts == nullptr);
	EXIT_IF(set_layouts_num == nullptr);
	EXIT_IF(push_constant_info == nullptr);
	EXIT_IF(push_constant_info_num == nullptr);

	bool need_descriptor = (bind.storage_buffers.buffers_num > 0 || bind.textures2D.textures_num > 0 || bind.samplers.samplers_num > 0 ||
	                        bind.gds_pointers.pointers_num > 0);

	EXIT_IF(need_descriptor && bind.push_constant_size == 0);

	if (bind.push_constant_size != 0 && bind.constant_storage == ShaderConstantStorage::PushConstants)
	{
		auto index = *push_constant_info_num;

		push_constant_info[index].stageFlags = vk_stage;
		push_constant_info[index].offset     = bind.push_constant_offset;
		push_constant_info[index].size       = bind.push_constant_size;
		(*push_constant_info_num)++;
	}

	if (need_descriptor)
	{
		EXIT_IF(bind.descriptor_set_slot != *set_layouts_num);

		set_layouts[*set_layouts_num] = g_render_ctx->GetDescriptorCache()->GetDescriptorSetLayout(stage, bind /*, bind_params*/);
		(*set_layouts_num)++;
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static VulkanPipeline* CreatePipelineInternal(VkRenderPass render_pass, const ShaderVertexInputInfo* vs_input_info,
                                              const Vector<uint32_t>& vs_shader, const ShaderPixelInputInfo* ps_input_info,
                                              const Vector<uint32_t>& ps_shader, const PipelineStaticParameters* static_params,
                                              PipelineDynamicParameters* dynamic_params)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(render_pass == nullptr);
	EXIT_IF(static_params == nullptr);
	EXIT_IF(dynamic_params == nullptr);

	auto* pipeline           = new VulkanPipeline;
	pipeline->static_params  = static_params;
	pipeline->dynamic_params = dynamic_params;

	auto* gctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(gctx == nullptr);

	VkShaderModule vert_shader_module = nullptr;
	VkShaderModule frag_shader_module = nullptr;

	VkShaderModuleCreateInfo create_info {};

	create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	create_info.pNext = nullptr;
	create_info.flags = 0;

	create_info.codeSize = static_cast<size_t>(vs_shader.Size()) * 4;
	create_info.pCode    = vs_shader.GetDataConst();
	const VkResult vert_module_result =
	    vkCreateShaderModule(gctx->device, &create_info, nullptr, &vert_shader_module);
	if (vert_module_result != VK_SUCCESS || vert_shader_module == nullptr)
	{
		if (vert_shader_module != nullptr)
			vkDestroyShaderModule(gctx->device, vert_shader_module, nullptr);
		delete pipeline;
		EXIT("vkCreateShaderModule(vertex) failed: %d", static_cast<int>(vert_module_result));
		return nullptr;
	}

	create_info.codeSize = static_cast<size_t>(ps_shader.Size()) * 4;
	create_info.pCode    = ps_shader.GetDataConst();
	const VkResult frag_module_result =
	    vkCreateShaderModule(gctx->device, &create_info, nullptr, &frag_shader_module);
	if (frag_module_result != VK_SUCCESS || frag_shader_module == nullptr)
	{
		if (frag_shader_module != nullptr)
			vkDestroyShaderModule(gctx->device, frag_shader_module, nullptr);
		vkDestroyShaderModule(gctx->device, vert_shader_module, nullptr);
		delete pipeline;
		EXIT("vkCreateShaderModule(fragment) failed: %d", static_cast<int>(frag_module_result));
		return nullptr;
	}

	VkPipelineShaderStageCreateInfo vert_shader_stage_info {};
	vert_shader_stage_info.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	vert_shader_stage_info.pNext               = nullptr;
	vert_shader_stage_info.flags               = 0;
	vert_shader_stage_info.stage               = VK_SHADER_STAGE_VERTEX_BIT;
	vert_shader_stage_info.module              = vert_shader_module;
	vert_shader_stage_info.pName               = "main";
	vert_shader_stage_info.pSpecializationInfo = nullptr;

	VkPipelineShaderStageCreateInfo frag_shader_stage_info {};
	frag_shader_stage_info.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	frag_shader_stage_info.pNext               = nullptr;
	frag_shader_stage_info.flags               = 0;
	frag_shader_stage_info.stage               = VK_SHADER_STAGE_FRAGMENT_BIT;
	frag_shader_stage_info.module              = frag_shader_module;
	frag_shader_stage_info.pName               = "main";
	frag_shader_stage_info.pSpecializationInfo = nullptr;

	VkPipelineShaderStageCreateInfo shader_stages[] = {vert_shader_stage_info, frag_shader_stage_info};

	VkVertexInputAttributeDescription input_attr[ShaderVertexInputInfo::RES_MAX];
	VkVertexInputBindingDescription   input_desc[ShaderVertexInputInfo::RES_MAX];

	for (int bi = 0; bi < vs_input_info->buffers_num; bi++)
	{
		const auto& b            = vs_input_info->buffers[bi];
		input_desc[bi].binding   = bi;
		input_desc[bi].stride    = b.stride;
		input_desc[bi].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

		for (int ai = 0; ai < b.attr_num; ai++)
		{
			auto index                 = b.attr_indices[ai];
			input_attr[index].binding  = bi;
			input_attr[index].location = index;
			input_attr[index].offset   = b.attr_offsets[ai];

			uint32_t attr_size = 4;
			get_input_format(vs_input_info->resources[index], vs_input_info->resources_dst[index], index,
			                 &input_attr[index].format, &attr_size);

			auto registers_num = vs_input_info->resources_dst[index].registers_num;

			EXIT_NOT_IMPLEMENTED(vs_input_info->resources[index].AddTid());
			EXIT_NOT_IMPLEMENTED(vs_input_info->resources[index].SwizzleEnabled());

			switch (registers_num)
			{
				case 1:
				{
					auto swizzle = vs_input_info->resources[index].DstSelX();
					EXIT_NOT_IMPLEMENTED(swizzle != DstSel(4));
					break;
				}
				case 2:
				{
					auto swizzle = vs_input_info->resources[index].DstSelXY();
					EXIT_NOT_IMPLEMENTED(attr_size == 1 && swizzle != DstSel(4, 0));
					EXIT_NOT_IMPLEMENTED(attr_size >= 2 && swizzle != DstSel(4, 5));
					break;
				}
				case 3:
				{
					auto swizzle = vs_input_info->resources[index].DstSelXYZ();
					EXIT_NOT_IMPLEMENTED(attr_size == 1 && swizzle != DstSel(4, 0, 0));
					EXIT_NOT_IMPLEMENTED(attr_size == 2 && swizzle != DstSel(4, 5, 0));
					EXIT_NOT_IMPLEMENTED(attr_size >= 3 && swizzle != DstSel(4, 5, 6));
					break;
				}
				case 4:
				{
					auto swizzle = vs_input_info->resources[index].DstSelXYZW();
					EXIT_NOT_IMPLEMENTED(attr_size == 1 && swizzle != DstSel(4, 0, 0, 1));
					EXIT_NOT_IMPLEMENTED(attr_size == 2 && swizzle != DstSel(4, 5, 0, 1));
					EXIT_NOT_IMPLEMENTED(attr_size == 3 && swizzle != DstSel(4, 5, 6, 1));
					if (attr_size == 4 && swizzle != DstSel(4, 5, 6, 7) &&
					    !(vs_input_info->fetch_external && ShaderVertexInputFloat4SwizzleCompatible(
					        vs_input_info->resources[index], registers_num)))
					{
						const auto& resource = vs_input_info->resources[index];
						EXIT("unsupported vertex swizzle: registers=%d nfmt=%u dfmt=%u dst_sel=%03x external=%u\n",
						     registers_num, unsigned(resource.Nfmt()), unsigned(resource.Dfmt()), swizzle,
						     unsigned(vs_input_info->fetch_external));
					}
					break;
				}
				default: EXIT("invalid registers_num");
			}
		}
	}

	VkPipelineVertexInputStateCreateInfo vertex_input_info {};
	vertex_input_info.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input_info.pNext                           = nullptr;
	vertex_input_info.flags                           = 0;
	vertex_input_info.vertexBindingDescriptionCount   = vs_input_info->buffers_num;
	vertex_input_info.pVertexBindingDescriptions      = input_desc;
	vertex_input_info.vertexAttributeDescriptionCount = vs_input_info->resources_num;
	vertex_input_info.pVertexAttributeDescriptions    = input_attr;

	VkPipelineInputAssemblyStateCreateInfo input_assembly {};
	input_assembly.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	input_assembly.pNext                  = nullptr;
	input_assembly.flags                  = 0;
	input_assembly.topology               = static_params->topology;
	input_assembly.primitiveRestartEnable = VK_FALSE;

	VkViewport viewport {};
	viewport.x        = static_params->viewport_offset[0] - static_params->viewport_scale[0];
	viewport.y        = static_params->viewport_offset[1] - static_params->viewport_scale[1];
	viewport.width    = static_params->viewport_scale[0] * 2.0f;
	viewport.height   = static_params->viewport_scale[1] * 2.0f;
	viewport.minDepth = static_params->viewport_offset[2];
	viewport.maxDepth = static_params->viewport_scale[2] + static_params->viewport_offset[2];
	const char* absolute_bias_reason = nullptr;
	if (!ApplyAbsoluteDepthBiasViewport(static_params->depth_bias, &viewport, &absolute_bias_reason))
	{
		EXIT("unsupported absolute polygon offset viewport: %s\n", absolute_bias_reason);
	}

	VkRect2D scissor {};
	if (!dynamic_params->vk_dynamic_state_scissor)
	{
		EXIT_NOT_IMPLEMENTED(!BuildScissorRect(dynamic_params->scissor_ltrb, &scissor));
	}

	VkPipelineViewportStateCreateInfo viewport_state {};
	viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport_state.pNext         = nullptr;
	viewport_state.flags         = 0;
	viewport_state.viewportCount = 1;
	viewport_state.pViewports    = &viewport;
	viewport_state.scissorCount  = 1;
	viewport_state.pScissors     = &scissor;

	VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;
	cull_mode |= (static_params->cull_back ? VK_CULL_MODE_BACK_BIT : 0u);
	cull_mode |= (static_params->cull_front ? VK_CULL_MODE_FRONT_BIT : 0u);

	VkFrontFace front_face = (static_params->face ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);

	VkPipelineRasterizationDepthClipStateCreateInfoEXT clip_ext {};
	clip_ext.sType           = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT;
	clip_ext.pNext           = nullptr;
	clip_ext.flags           = 0;
	clip_ext.depthClipEnable = VK_FALSE;

	VkPipelineRasterizationStateCreateInfo rasterizer {};
	rasterizer.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	// Apple graphics backends and the Android portability path do not expose
	// VK_EXT_depth_clip_enable. The native Metal adapter maps core depth clamp
	// directly to MTLDepthClipModeClamp.
	rasterizer.pNext                   = nullptr;
	rasterizer.depthClampEnable        = VK_TRUE;
#else
	rasterizer.pNext                   = &clip_ext;
	rasterizer.depthClampEnable        = VK_FALSE;
#endif
	rasterizer.flags                   = 0;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode             = VK_POLYGON_MODE_FILL;
	rasterizer.cullMode                = cull_mode;
	rasterizer.frontFace               = front_face;
	rasterizer.depthBiasEnable         = static_params->depth_bias.enabled ? VK_TRUE : VK_FALSE;
	rasterizer.depthBiasConstantFactor =
	    static_params->depth_bias.representation == PipelineDepthBiasRepresentation::AbsoluteWindowDepth
	        ? 0.0f
	        : static_params->depth_bias.constant;
	rasterizer.depthBiasClamp          = static_params->depth_bias.clamp;
	rasterizer.depthBiasSlopeFactor    = static_params->depth_bias.slope;
	rasterizer.lineWidth               = dynamic_params->line_width;

	VkPipelineMultisampleStateCreateInfo multisampling {};
	multisampling.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.pNext                 = nullptr;
	multisampling.flags                 = 0;
	multisampling.sampleShadingEnable   = VK_FALSE;
	multisampling.rasterizationSamples  = VK_SAMPLE_COUNT_1_BIT;
	multisampling.minSampleShading      = 1.0f;
	multisampling.pSampleMask           = nullptr;
	multisampling.alphaToCoverageEnable = VK_FALSE;
	multisampling.alphaToOneEnable      = VK_FALSE;

	VkColorComponentFlags color_write_mask = 0;
	// VK_EXT_color_write_enable is not available on every portability driver.
	// Raw components and non-dynamic enable are both already cache-key state;
	// disabling color writes produces an empty effective component mask.
	EXIT_NOT_IMPLEMENTED(!ResolveRt0ColorWriteMask(static_params->color_mask,
	    pipeline->dynamic_params->color_write_enable, &color_write_mask));

	VkPipelineColorBlendAttachmentState color_blend_attachment {};
	color_blend_attachment.colorWriteMask      = color_write_mask;
	color_blend_attachment.blendEnable         = static_params->blend_enable ? VK_TRUE : VK_FALSE;
	color_blend_attachment.srcColorBlendFactor = get_blend_factor(static_params->color_srcblend);
	color_blend_attachment.dstColorBlendFactor = get_blend_factor(static_params->color_destblend);
	color_blend_attachment.colorBlendOp        = get_blend_op(static_params->color_comb_fcn);
	color_blend_attachment.srcAlphaBlendFactor = (static_params->separate_alpha_blend ? get_blend_factor(static_params->alpha_srcblend)
	                                                                                  : color_blend_attachment.srcColorBlendFactor);
	color_blend_attachment.dstAlphaBlendFactor = (static_params->separate_alpha_blend ? get_blend_factor(static_params->alpha_destblend)
	                                                                                  : color_blend_attachment.dstColorBlendFactor);
	color_blend_attachment.alphaBlendOp =
	    (static_params->separate_alpha_blend ? get_blend_op(static_params->alpha_comb_fcn) : color_blend_attachment.colorBlendOp);

	VkPipelineColorBlendStateCreateInfo color_blending {};
	color_blending.sType             = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	color_blending.pNext             = nullptr;
	color_blending.flags             = 0;
	color_blending.logicOpEnable     = VK_FALSE;
	color_blending.logicOp           = VK_LOGIC_OP_COPY;
	color_blending.attachmentCount   = 1;
	color_blending.pAttachments      = &color_blend_attachment;
	color_blending.blendConstants[0] = static_params->blend_color_red;
	color_blending.blendConstants[1] = static_params->blend_color_green;
	color_blending.blendConstants[2] = static_params->blend_color_blue;
	color_blending.blendConstants[3] = static_params->blend_color_alpha;

	VkDescriptorSetLayout set_layouts[3]  = {};
	uint32_t              set_layouts_num = 0;

	VkPushConstantRange push_constant_info[2];
	uint32_t            push_constant_info_num = 0;

	CreateLayout(set_layouts, &set_layouts_num, push_constant_info, &push_constant_info_num, vs_input_info->bind,
	             /*additional_params->vs_bind,*/ VK_SHADER_STAGE_VERTEX_BIT, DescriptorCache::Stage::Vertex);
	CreateLayout(set_layouts, &set_layouts_num, push_constant_info, &push_constant_info_num, ps_input_info->bind,
	             /*additional_params->ps_bind,*/ VK_SHADER_STAGE_FRAGMENT_BIT, DescriptorCache::Stage::Pixel);
	if (vs_input_info->bind.constant_storage == ShaderConstantStorage::UniformBuffer)
		AppendConstantLayout(set_layouts, &set_layouts_num, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

	VkPipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout_info.pNext                  = nullptr;
	pipeline_layout_info.flags                  = 0;
	pipeline_layout_info.setLayoutCount         = set_layouts_num;
	pipeline_layout_info.pSetLayouts            = (set_layouts_num > 0 ? set_layouts : nullptr);
	pipeline_layout_info.pushConstantRangeCount = push_constant_info_num;
	pipeline_layout_info.pPushConstantRanges    = push_constant_info_num > 0 ? push_constant_info : nullptr;

	EXIT_IF(pipeline->pipeline_layout != nullptr);

	const VkResult layout_result =
	    vkCreatePipelineLayout(gctx->device, &pipeline_layout_info, nullptr, &pipeline->pipeline_layout);
	if (layout_result != VK_SUCCESS || pipeline->pipeline_layout == nullptr)
	{
		if (pipeline->pipeline_layout != nullptr)
			vkDestroyPipelineLayout(gctx->device, pipeline->pipeline_layout, nullptr);
		vkDestroyShaderModule(gctx->device, frag_shader_module, nullptr);
		vkDestroyShaderModule(gctx->device, vert_shader_module, nullptr);
		delete pipeline;
		EXIT("vkCreatePipelineLayout(graphics) failed: %d", static_cast<int>(layout_result));
		return nullptr;
	}

	VkPipelineDepthStencilStateCreateInfo depth_stencil_info {};
	depth_stencil_info.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depth_stencil_info.pNext                 = nullptr;
	depth_stencil_info.flags                 = 0;
	depth_stencil_info.depthTestEnable       = (static_params->depth_test_enable ? VK_TRUE : VK_FALSE);
	depth_stencil_info.depthWriteEnable      = (static_params->depth_write_enable ? VK_TRUE : VK_FALSE);
	depth_stencil_info.depthCompareOp        = static_params->depth_compare_op;
	depth_stencil_info.depthBoundsTestEnable = (static_params->depth_bounds_test_enable ? VK_TRUE : VK_FALSE);
	depth_stencil_info.stencilTestEnable     = (static_params->stencil_test_enable ? VK_TRUE : VK_FALSE);
	depth_stencil_info.front.failOp          = static_params->stencil_front.failOp;
	depth_stencil_info.front.passOp          = static_params->stencil_front.passOp;
	depth_stencil_info.front.depthFailOp     = static_params->stencil_front.depthFailOp;
	depth_stencil_info.front.compareOp       = static_params->stencil_front.compareOp;
	depth_stencil_info.front.compareMask     = dynamic_params->stencil_front.compareMask;
	depth_stencil_info.front.writeMask       = dynamic_params->stencil_front.writeMask;
	depth_stencil_info.front.reference       = dynamic_params->stencil_front.reference;
	depth_stencil_info.back.failOp           = static_params->stencil_back.failOp;
	depth_stencil_info.back.passOp           = static_params->stencil_back.passOp;
	depth_stencil_info.back.depthFailOp      = static_params->stencil_back.depthFailOp;
	depth_stencil_info.back.compareOp        = static_params->stencil_back.compareOp;
	depth_stencil_info.back.compareMask      = dynamic_params->stencil_back.compareMask;
	depth_stencil_info.back.writeMask        = dynamic_params->stencil_back.writeMask;
	depth_stencil_info.back.reference        = dynamic_params->stencil_back.reference;
	depth_stencil_info.minDepthBounds        = static_params->depth_min_bounds;
	depth_stencil_info.maxDepthBounds        = static_params->depth_max_bounds;

	VkDynamicState dynamic_states[8]    = {};
	uint32_t       dynamic_states_count = 0;
	if (dynamic_params->vk_dynamic_state_scissor)
	{
		dynamic_states[dynamic_states_count++] = VK_DYNAMIC_STATE_SCISSOR;
	}
	if (dynamic_params->vk_dynamic_state_line_width)
	{
		dynamic_states[dynamic_states_count++] = VK_DYNAMIC_STATE_LINE_WIDTH;
	}
	if (dynamic_params->vk_dynamic_state_stencil_compare_mask)
	{
		dynamic_states[dynamic_states_count++] = VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
	}
	if (dynamic_params->vk_dynamic_state_stencil_reference)
	{
		dynamic_states[dynamic_states_count++] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
	}
	if (dynamic_params->vk_dynamic_state_stencil_write_mask)
	{
		dynamic_states[dynamic_states_count++] = VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
	}
	if (dynamic_params->vk_dynamic_state_color_write_enable_ext)
	{
		dynamic_states[dynamic_states_count++] = VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT;
	}

	VkPipelineDynamicStateCreateInfo dynamic_state {};
	dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic_state.pNext             = nullptr;
	dynamic_state.flags             = 0;
	dynamic_state.dynamicStateCount = dynamic_states_count;
	dynamic_state.pDynamicStates    = dynamic_states;

	VkGraphicsPipelineCreateInfo pipeline_info {};
	pipeline_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline_info.pNext               = nullptr;
	pipeline_info.flags               = 0;
	pipeline_info.stageCount          = 2;
	pipeline_info.pStages             = shader_stages;
	pipeline_info.pVertexInputState   = &vertex_input_info;
	pipeline_info.pInputAssemblyState = &input_assembly;
	pipeline_info.pTessellationState  = nullptr;
	pipeline_info.pViewportState      = &viewport_state;
	pipeline_info.pRasterizationState = &rasterizer;
	pipeline_info.pMultisampleState   = &multisampling;
	pipeline_info.pDepthStencilState  = (static_params->with_depth ? &depth_stencil_info : nullptr);
	pipeline_info.pColorBlendState    = &color_blending;
	pipeline_info.pDynamicState       = &dynamic_state;
	pipeline_info.layout              = pipeline->pipeline_layout;
	pipeline_info.renderPass          = render_pass;
	pipeline_info.subpass             = 0;
	pipeline_info.basePipelineHandle  = nullptr;
	pipeline_info.basePipelineIndex   = -1;

	EXIT_IF(pipeline->pipeline != nullptr);

	const VkResult pipeline_result =
	    vkCreateGraphicsPipelines(gctx->device, nullptr, 1, &pipeline_info, nullptr, &pipeline->pipeline);
	if (pipeline_result != VK_SUCCESS || pipeline->pipeline == nullptr)
	{
		if (pipeline->pipeline != nullptr)
			vkDestroyPipeline(gctx->device, pipeline->pipeline, nullptr);
		vkDestroyShaderModule(gctx->device, frag_shader_module, nullptr);
		vkDestroyShaderModule(gctx->device, vert_shader_module, nullptr);
		vkDestroyPipelineLayout(gctx->device, pipeline->pipeline_layout, nullptr);
		delete pipeline;
		EXIT("vkCreateGraphicsPipelines failed: %d", static_cast<int>(pipeline_result));
		return nullptr;
	}

	vkDestroyShaderModule(gctx->device, frag_shader_module, nullptr);
	vkDestroyShaderModule(gctx->device, vert_shader_module, nullptr);

	return pipeline;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static VulkanPipeline* CreatePipelineInternal(const ShaderComputeInputInfo* input_info, const Vector<uint32_t>& cs_shader,
                                              const PipelineStaticParameters* static_params, PipelineDynamicParameters* dynamic_params)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(static_params == nullptr);
	EXIT_IF(dynamic_params == nullptr);

	auto* pipeline           = new VulkanPipeline;
	pipeline->static_params  = static_params;
	pipeline->dynamic_params = dynamic_params;

	auto* gctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(gctx == nullptr);

	VkShaderModule comp_shader_module = nullptr;

	VkShaderModuleCreateInfo create_info {};

	create_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	create_info.pNext    = nullptr;
	create_info.flags    = 0;
	create_info.codeSize = static_cast<size_t>(cs_shader.Size()) * 4;
	create_info.pCode    = cs_shader.GetDataConst();
	const VkResult comp_module_result =
	    vkCreateShaderModule(gctx->device, &create_info, nullptr, &comp_shader_module);
	if (comp_module_result != VK_SUCCESS || comp_shader_module == nullptr)
	{
		if (comp_shader_module != nullptr)
			vkDestroyShaderModule(gctx->device, comp_shader_module, nullptr);
		delete pipeline;
		EXIT("vkCreateShaderModule(compute) failed: %d", static_cast<int>(comp_module_result));
		return nullptr;
	}

	VkPipelineShaderStageCreateInfo comp_shader_stage_info {};
	comp_shader_stage_info.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	comp_shader_stage_info.pNext               = nullptr;
	comp_shader_stage_info.flags               = 0;
	comp_shader_stage_info.stage               = VK_SHADER_STAGE_COMPUTE_BIT;
	comp_shader_stage_info.module              = comp_shader_module;
	comp_shader_stage_info.pName               = "main";
	comp_shader_stage_info.pSpecializationInfo = nullptr;

	VkDescriptorSetLayout set_layouts[3]  = {};
	uint32_t              set_layouts_num = 0;

	VkPushConstantRange push_constant_info[1];
	uint32_t            push_constant_info_num = 0;

	CreateLayout(set_layouts, &set_layouts_num, push_constant_info, &push_constant_info_num,
	             input_info->bind, /*additional_params->cs_bind,*/
	             VK_SHADER_STAGE_COMPUTE_BIT, DescriptorCache::Stage::Compute);
	if (input_info->bind.constant_storage == ShaderConstantStorage::UniformBuffer)
		AppendConstantLayout(set_layouts, &set_layouts_num, VK_SHADER_STAGE_COMPUTE_BIT);

	VkPipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout_info.pNext                  = nullptr;
	pipeline_layout_info.flags                  = 0;
	pipeline_layout_info.setLayoutCount         = set_layouts_num;
	pipeline_layout_info.pSetLayouts            = (set_layouts_num > 0 ? set_layouts : nullptr);
	pipeline_layout_info.pushConstantRangeCount = push_constant_info_num;
	pipeline_layout_info.pPushConstantRanges    = push_constant_info_num > 0 ? push_constant_info : nullptr;

	EXIT_IF(pipeline->pipeline_layout != nullptr);

	const VkResult layout_result =
	    vkCreatePipelineLayout(gctx->device, &pipeline_layout_info, nullptr, &pipeline->pipeline_layout);
	if (layout_result != VK_SUCCESS || pipeline->pipeline_layout == nullptr)
	{
		if (pipeline->pipeline_layout != nullptr)
			vkDestroyPipelineLayout(gctx->device, pipeline->pipeline_layout, nullptr);
		vkDestroyShaderModule(gctx->device, comp_shader_module, nullptr);
		delete pipeline;
		EXIT("vkCreatePipelineLayout(compute) failed: %d", static_cast<int>(layout_result));
		return nullptr;
	}

	VkComputePipelineCreateInfo info {};
	info.sType              = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.pNext              = nullptr;
	info.flags              = 0;
	info.stage              = comp_shader_stage_info;
	info.layout             = pipeline->pipeline_layout;
	info.basePipelineHandle = nullptr;
	info.basePipelineIndex  = -1;

	EXIT_IF(pipeline->pipeline != nullptr);

	const VkResult pipeline_result =
	    vkCreateComputePipelines(gctx->device, nullptr, 1, &info, nullptr, &pipeline->pipeline);
	if (pipeline_result != VK_SUCCESS || pipeline->pipeline == nullptr)
	{
		if (pipeline->pipeline != nullptr)
			vkDestroyPipeline(gctx->device, pipeline->pipeline, nullptr);
		vkDestroyShaderModule(gctx->device, comp_shader_module, nullptr);
		vkDestroyPipelineLayout(gctx->device, pipeline->pipeline_layout, nullptr);
		delete pipeline;
		EXIT("vkCreateComputePipelines failed: %d", static_cast<int>(pipeline_result));
		return nullptr;
	}

	vkDestroyShaderModule(gctx->device, comp_shader_module, nullptr);

	return pipeline;
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const
{
	// NOLINTNEXTLINE(bugprone-suspicious-memory-comparison,cert-exp42-c,cert-flp37-c)
	return (0 == memcmp(this, &other, sizeof(struct PipelineStaticParameters)));
}

static bool BuildScissorRect(const int (&ltrb)[4], VkRect2D* rectangle)
{
	const int64_t width = static_cast<int64_t>(ltrb[2]) - ltrb[0];
	const int64_t height = static_cast<int64_t>(ltrb[3]) - ltrb[1];
	if (rectangle == nullptr || ltrb[0] < 0 || ltrb[1] < 0 || width < 0 || height < 0 ||
	    static_cast<int64_t>(ltrb[0]) + width > INT32_MAX || static_cast<int64_t>(ltrb[1]) + height > INT32_MAX)
	{
		return false;
	}
	*rectangle = {{ltrb[0], ltrb[1]}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}};
	return true;
}

bool PipelineDynamicParameters::operator==(const PipelineDynamicParameters& other) const
{
	if (vk_dynamic_state_scissor != other.vk_dynamic_state_scissor ||
	    (!vk_dynamic_state_scissor && memcmp(scissor_ltrb, other.scissor_ltrb, sizeof(scissor_ltrb)) != 0))
	{
		return false;
	}
	EXIT_IF(vk_dynamic_state_line_width != other.vk_dynamic_state_line_width ||
	        vk_dynamic_state_stencil_compare_mask != other.vk_dynamic_state_stencil_compare_mask ||
	        vk_dynamic_state_stencil_write_mask != other.vk_dynamic_state_stencil_write_mask ||
	        vk_dynamic_state_stencil_reference != other.vk_dynamic_state_stencil_reference ||
	        vk_dynamic_state_color_write_enable_ext != other.vk_dynamic_state_color_write_enable_ext);

	if (!vk_dynamic_state_line_width)
	{
		if (line_width != other.line_width)
		{
			return false;
		}
	}
	if (!vk_dynamic_state_stencil_compare_mask)
	{
		if (stencil_front.compareMask != other.stencil_front.compareMask || stencil_back.compareMask != other.stencil_back.compareMask)
		{
			return false;
		}
	}
	if (!vk_dynamic_state_stencil_write_mask)
	{
		if (stencil_front.writeMask != other.stencil_front.writeMask || stencil_back.writeMask != other.stencil_back.writeMask)
		{
			return false;
		}
	}
	if (!vk_dynamic_state_stencil_reference)
	{
		if (stencil_front.reference != other.stencil_front.reference || stencil_back.reference != other.stencil_back.reference)
		{
			return false;
		}
	}
	if (!vk_dynamic_state_color_write_enable_ext)
	{
		if (color_write_enable != other.color_write_enable)
		{
			return false;
		}
	}
	return true;
}

void PipelineCache::DeletePipelineInternal(uint32_t id)
{
	EXIT_NOT_IMPLEMENTED(!m_pipelines.IndexValid(id));

	Pipeline& p = m_pipelines[id];

	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(p.pipeline == nullptr);
	EXIT_IF(p.pipeline->pipeline == nullptr);
	EXIT_IF(p.pipeline->pipeline_layout == nullptr);
	EXIT_IF(p.static_params == nullptr);
	EXIT_IF(p.dynamic_params == nullptr);

	EXIT_IF(p.pipeline->static_params != p.static_params);
	EXIT_IF(p.pipeline->dynamic_params != p.dynamic_params);

	DumpPipeline("delete", id);

	delete p.static_params;
	delete p.dynamic_params;

	p.static_params  = nullptr;
	p.dynamic_params = nullptr;

	auto* gctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(gctx == nullptr);

	vkDestroyPipeline(gctx->device, p.pipeline->pipeline, nullptr);
	vkDestroyPipelineLayout(gctx->device, p.pipeline->pipeline_layout, nullptr);

	delete p.pipeline;

	p.pipeline = nullptr;
}

VulkanPipeline* PipelineCache::Find(const Pipeline& p) const
{
	return FindIn(m_pipelines, p);
}

VulkanPipeline* PipelineCache::FindIn(const Vector<Pipeline>& pipelines, const Pipeline& p)
{
	for (const auto& pn: pipelines)
	{
		if (pn.pipeline != nullptr && p.render_pass_id == pn.render_pass_id && p.vs_shader_id == pn.vs_shader_id &&
		    p.ps_shader_id == pn.ps_shader_id && p.cs_shader_id == pn.cs_shader_id && *p.static_params == *pn.static_params &&
		    *p.dynamic_params == *pn.dynamic_params)
		{
			*pn.pipeline->dynamic_params = *p.dynamic_params;
			return pn.pipeline;
		}
	}
	return nullptr;
}

uint32_t PipelineCache::StoreIn(Vector<Pipeline>* pipelines, const Pipeline& p)
{
	EXIT_IF(pipelines == nullptr || p.pipeline == nullptr);
	uint32_t index = 0;
	for (auto& existing: *pipelines)
	{
		if (existing.pipeline == nullptr)
		{
			existing = p;
			return index;
		}
		++index;
	}
	EXIT_IF(index != pipelines->Size());
	pipelines->Add(p);
	return index;
}

VulkanPipeline* PipelineCache::CreatePipeline(VulkanFramebuffer* framebuffer, RenderColorInfo* color, RenderDepthInfo* depth,
                                              ShaderVertexInputInfo* vs_input_info, HW::Context* ctx, HW::Shader* sh_ctx,
	                                              ShaderPixelInputInfo* ps_input_info, VkPrimitiveTopology topology)
{
	LUMEN_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(framebuffer == nullptr);
	EXIT_IF(depth == nullptr);
	EXIT_IF(color == nullptr);

	Core::LockGuard lock(m_mutex);

	const auto& vs_regs = sh_ctx->GetVs();
	const auto& ps_regs = sh_ctx->GetPs();
	const auto& sh_regs = ctx->GetShaderRegisters();
	SelectRuntimeConstantStorage(&vs_input_info->bind, &ps_input_info->bind);
	EXIT_NOT_IMPLEMENTED(!ShaderGraphicsFp32StateSupported(vs_regs, ps_regs));
	EXIT_NOT_IMPLEMENTED(!ShaderGraphicsDx10ClampStateSupported(vs_regs, ps_regs));
	EXIT_NOT_IMPLEMENTED(vs_input_info->rect_list && !ShaderRectListInputSupported(*vs_input_info));
	vs_input_info->host_parameter_interface_valid = false;
	vs_input_info->host_vertex_output_components = 0;
	if (vs_input_info->export_count > 5)
	{
		// Embedded modules have separate fixed interfaces. Keep both them and
		// the legacy <=5 path outside this added admission and property query.
		EXIT_NOT_IMPLEMENTED(vs_regs.vs_embedded || ps_regs.ps_embedded);
		VkPhysicalDeviceProperties properties {};
		vkGetPhysicalDeviceProperties(g_render_ctx->GetGraphicCtx()->physical_device, &properties);
		const char* interface_rejection = nullptr;
		const bool supported = ShaderVertexPixelInterfaceSupported(*vs_input_info, *ps_input_info,
		    properties.limits.maxVertexOutputComponents, properties.limits.maxFragmentInputComponents, &interface_rejection);
		if (!supported)
			EXIT("unsupported vertex/pixel interface before pipeline cache: exports=%d inputs=%u producer_mask=%08x"
			     " max_vertex_components=%u max_fragment_components=%u reason=%s\n", vs_input_info->export_count, ps_input_info->input_num,
			     vs_input_info->parameter_export_mask, properties.limits.maxVertexOutputComponents, properties.limits.maxFragmentInputComponents,
			     interface_rejection != nullptr ? interface_rejection : "unknown");
		vs_input_info->host_vertex_output_components = properties.limits.maxVertexOutputComponents;
		vs_input_info->host_parameter_interface_valid = true;
		if (vs_input_info->export_count > 5)
		{
			static std::atomic<uint32_t> reports {0};
			auto report = reports.load(std::memory_order_relaxed);
			while (report < 16 && !reports.compare_exchange_strong(report, report + 1, std::memory_order_relaxed)) {}
			if (report < 16)
			{
				uint32_t locations = 0;
				for (uint32_t i = 0; i < ps_input_info->input_num; ++i) locations |= uint32_t{1} << (ps_input_info->interpolator_settings[i] & 0x1fu);
				std::fprintf(stderr, "[GFX_PARAMETER_INTERFACE] exports=%d producer_mask=%08x ps_inputs=%u ps_locations=%08x"
				                     " vs_components=%u ps_components=%u max_vertex_components=%u max_fragment_components=%u"
				                     " producer_proof=syntactic host_interface=admitted\n", vs_input_info->export_count,
				             vs_input_info->parameter_export_mask, ps_input_info->input_num, locations,
				             4u * static_cast<uint32_t>(vs_input_info->export_count) + 7u,
				             4u * ps_input_info->input_num + (ps_input_info->ps_pos_xy ? 4u : 0u),
				             properties.limits.maxVertexOutputComponents, properties.limits.maxFragmentInputComponents);
			}
		}
	}
	if (!ShaderTextureDescriptorProfileSupportedPreCache(vs_input_info->bind.textures2D, ShaderType::Vertex) ||
	    !ShaderTextureDescriptorProfileSupportedPreCache(ps_input_info->bind.textures2D, ShaderType::Pixel))
	{
		EXIT("unsupported nonzero storage mip descriptor profile before graphics pipeline cache lookup\n");
	}
	EXIT_NOT_IMPLEMENTED(!IsPixelColorExportStateSupported(ps_regs.ps_embedded, sh_regs.target_output_mode[0], sh_regs.m_cbShaderMask));
	const HW::BlendColor&     bclr       = ctx->GetBlendColor();
	const HW::ScreenViewport& vp         = ctx->GetScreenViewport();
	const auto&               bc         = ctx->GetBlendControl(0);
	uint32_t                  color_mask = ctx->GetRenderTargetMask();
	const HW::ModeControl&    mc         = ctx->GetModeControl();
	const auto&               cc         = ctx->GetColorControl();
	VkColorComponentFlags validated_color_write_mask = 0;
	EXIT_NOT_IMPLEMENTED(!ResolveRt0ColorWriteMask(color_mask, cc.mode == 1, &validated_color_write_mask));

	if (Config::GetPrintfDirection() != Log::Direction::Silent)
	{
		ShaderDbgDumpInputInfo(vs_input_info);
		ShaderDbgDumpInputInfo(ps_input_info);
	}

	auto vs_id = ShaderGetIdVS(&vs_regs, vs_input_info);
	auto ps_id = ShaderGetIdPS(&ps_regs, ps_input_info);

	PipelineStaticParameters static_params;
	PipelineDynamicParameters dynamic_params;
	Pipeline p {};
	p.render_pass_id = framebuffer->render_pass_id;
	p.ps_shader_id   = ps_id;
	p.vs_shader_id   = vs_id;
	p.static_params  = &static_params;
	p.dynamic_params = &dynamic_params;

	p.dynamic_params->vk_dynamic_state_scissor              = true;
	p.dynamic_params->vk_dynamic_state_line_width           = true;
	p.dynamic_params->vk_dynamic_state_stencil_compare_mask = true;
	p.dynamic_params->vk_dynamic_state_stencil_reference    = true;
	p.dynamic_params->vk_dynamic_state_stencil_write_mask   = true;
	p.dynamic_params->color_write_enable                    = true;

	EXIT_NOT_IMPLEMENTED(depth->depth_test_enable && ps_input_info->ps_execute_on_noop);

	p.static_params->viewport_scale[0]  = vp.viewports[0].xscale;
	p.static_params->viewport_scale[1]  = vp.viewports[0].yscale;
	p.static_params->viewport_scale[2]  = vp.viewports[0].zscale;
	p.static_params->viewport_offset[0] = vp.viewports[0].xoffset;
	p.static_params->viewport_offset[1] = vp.viewports[0].yoffset;
	p.static_params->viewport_offset[2] = vp.viewports[0].zoffset;
	EXIT_NOT_IMPLEMENTED(!SelectGraphicsScissor(vp, ctx->GetScanModeControl(), p.dynamic_params->scissor_ltrb));
	p.static_params->topology                 = topology;
	p.static_params->with_depth               = (depth->format != VK_FORMAT_UNDEFINED && depth->vulkan_buffer != nullptr);
	p.static_params->depth_test_enable        = depth->depth_test_enable;
	p.static_params->depth_write_enable       = (depth->depth_write_enable && !depth->depth_clear_enable);
	p.static_params->depth_compare_op         = depth->depth_compare_op;
	p.static_params->depth_bounds_test_enable = depth->depth_bounds_test_enable;
	p.static_params->depth_min_bounds         = depth->depth_min_bounds;
	p.static_params->depth_max_bounds         = depth->depth_max_bounds;
	p.static_params->stencil_test_enable      = depth->stencil_test_enable;
	p.static_params->stencil_front            = depth->stencil_static_front;
	p.static_params->stencil_back             = depth->stencil_static_back;
	p.static_params->color_mask               = color_mask;
	p.static_params->cull_back                = mc.cull_back;
	p.static_params->cull_front               = mc.cull_front;
	p.static_params->face                     = mc.face;
	const char* bias_reason = nullptr;
	if (!ResolvePolygonOffset(*ctx, depth->format, p.static_params->with_depth, &p.static_params->depth_bias, &bias_reason))
	{
		EXIT("unsupported polygon offset before pipeline cache lookup: %s\n", bias_reason);
	}
	if (!AbsoluteDepthBiasTopologySupported(p.static_params->depth_bias, topology))
	{
		EXIT("unsupported absolute polygon offset topology before pipeline cache lookup: %u\n",
		     static_cast<unsigned>(topology));
	}
	p.static_params->color_srcblend           = bc.color_srcblend;
	p.static_params->color_comb_fcn           = bc.color_comb_fcn;
	p.static_params->color_destblend          = bc.color_destblend;
	p.static_params->alpha_srcblend           = bc.alpha_srcblend;
	p.static_params->alpha_comb_fcn           = bc.alpha_comb_fcn;
	p.static_params->alpha_destblend          = bc.alpha_destblend;
	p.static_params->separate_alpha_blend     = bc.separate_alpha_blend;
	p.static_params->blend_enable             = bc.enable;
	SetPipelineBlendConstants(p.static_params, bclr);

	p.dynamic_params->line_width         = ctx->GetLineWidth();
	SetPipelineStencilDynamicState(p.dynamic_params, *depth);
	p.dynamic_params->color_write_enable = (cc.mode == 1);
	VkRect2D validated_scissor {};
	if (!BuildScissorRect(p.dynamic_params->scissor_ltrb, &validated_scissor))
	{
		EXIT("unsupported graphics scissor: %d,%d,%d,%d\n", p.dynamic_params->scissor_ltrb[0],
		     p.dynamic_params->scissor_ltrb[1], p.dynamic_params->scissor_ltrb[2], p.dynamic_params->scissor_ltrb[3]);
	}

	auto* found = Find(p);

	if (found != nullptr)
	{
		return found;
	}

	// Only a miss publishes parameter pointers into the native pipeline/cache.
	p.static_params  = new PipelineStaticParameters(static_params);
	p.dynamic_params = new PipelineDynamicParameters(dynamic_params);

	auto vs_code = ShaderParseVS(&vs_regs, &sh_regs);
	auto ps_code = ShaderParsePS(&ps_regs, &sh_regs);
	const bool ps_has_image_load = g_null_sampled_image_enabled && ShaderCodeHasImageLoad(ps_code);

	Vector<uint32_t> vs_shader, ps_shader;
	ShaderRecompileGraphicsPair(vs_code, vs_input_info, ps_code, ps_input_info, &vs_shader, &ps_shader);

	EXIT_IF(vs_shader.IsEmpty());
	EXIT_IF(ps_shader.IsEmpty());

	p.pipeline = CreatePipelineInternal(framebuffer->render_pass, vs_input_info, vs_shader, ps_input_info, ps_shader, p.static_params,
	                                    p.dynamic_params);
	EXIT_NOT_IMPLEMENTED(p.pipeline == nullptr);
	p.pipeline->ps_has_image_load = ps_has_image_load;
	p.pipeline->ps_depth_stencil_export_free = PixelShaderDepthStencilExportFree(ps_code);
	// Cache hits retain these facts about the actual bound native modules;
	// no current descriptor usage or fresh guest bytes can replace them.
	p.pipeline->vs_storage_write_authority = ShaderClassifyStorageBufferWrites(vs_code, ShaderType::Vertex, vs_input_info->bind);
	TraceShaderStorageWriteAuthority(ShaderType::Vertex, p.pipeline->vs_storage_write_authority);
	p.pipeline->ps_storage_write_authority = ShaderClassifyStorageBufferWrites(ps_code, ShaderType::Pixel, ps_input_info->bind);
	TraceShaderStorageWriteAuthority(ShaderType::Pixel, p.pipeline->ps_storage_write_authority);
	TraceShaderStorageNoStoreFailure(vs_code, ShaderType::Vertex, vs_input_info->bind, p.pipeline,
	                                ShaderStorageNoStoreProof(vs_code, ShaderType::Vertex));
	TraceShaderStorageNoStoreFailure(ps_code, ShaderType::Pixel, ps_input_info->bind, p.pipeline,
	                                ShaderStorageNoStoreProof(ps_code, ShaderType::Pixel));

	const uint32_t previous_size = m_pipelines.Size();
	const uint32_t index         = StoreIn(&m_pipelines, p);
	if (previous_size == LEGACY_PIPELINE_LIMIT && index == previous_size)
	{
		std::fprintf(stderr,
		             "[GFX_PIPELINE_CACHE_GROWTH] previous=%u next=%u kind=graphics policy=grow eviction=none\n",
		             previous_size, m_pipelines.Size());
		std::fflush(stderr);
	}
	DumpPipeline("create", index);
	return p.pipeline;
}

VulkanPipeline* PipelineCache::CreatePipeline(ShaderComputeInputInfo* input_info, const HW::ComputeShaderInfo* cs_regs,
                                              const HW::ShaderRegisters* sh_regs)
{
	LUMEN_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(cs_regs == nullptr);
	EXIT_IF(sh_regs == nullptr);
	EXIT_IF(input_info == nullptr);
	SelectRuntimeConstantStorage(&input_info->bind, nullptr);
	if (!ShaderTextureDescriptorProfileSupportedPreCache(input_info->bind.textures2D, ShaderType::Compute))
	{
		ShaderDbgDumpInputInfo(input_info);
		EXIT("unsupported nonzero storage mip descriptor profile before compute pipeline cache lookup\n");
	}

	Core::LockGuard lock(m_mutex);

	ShaderDbgDumpInputInfo(input_info);

	auto cs_id = ShaderGetIdCS(cs_regs, input_info);

	PipelineStaticParameters static_params;
	PipelineDynamicParameters dynamic_params;
	Pipeline p {};
	p.cs_shader_id   = cs_id;
	p.static_params  = &static_params;
	p.dynamic_params = &dynamic_params;

	p.dynamic_params->vk_dynamic_state_line_width           = true;
	p.dynamic_params->vk_dynamic_state_stencil_compare_mask = true;
	p.dynamic_params->vk_dynamic_state_stencil_reference    = true;
	p.dynamic_params->vk_dynamic_state_stencil_write_mask   = true;
	p.dynamic_params->color_write_enable                    = true;

	auto* found = Find(p);

	if (found != nullptr)
	{
		return found;
	}

	// Only a miss publishes parameter pointers into the native pipeline/cache.
	p.static_params  = new PipelineStaticParameters(static_params);
	p.dynamic_params = new PipelineDynamicParameters(dynamic_params);

	auto cs_code = ShaderParseCS(cs_regs, sh_regs);
	const bool cs_requires_post_image_store_barrier = ShaderComputeRequiresPostDispatchImageStoreBarrier(cs_code);
	const auto cs_image_mutation = ShaderClassifyComputeImageMutation(cs_code, input_info->bind);

	auto cs_shader = ShaderRecompileCS(cs_code, input_info);
	EXIT_IF(cs_shader.IsEmpty());

	p.pipeline = CreatePipelineInternal(input_info, cs_shader, p.static_params, p.dynamic_params /*, params2*/);
	EXIT_NOT_IMPLEMENTED(p.pipeline == nullptr);
	p.pipeline->cs_requires_post_image_store_barrier = cs_requires_post_image_store_barrier;
	p.pipeline->cs_image_mutation = cs_image_mutation;
	p.pipeline->cs_storage_write_authority = ShaderClassifyStorageBufferWrites(cs_code, ShaderType::Compute, input_info->bind);
	TraceShaderStorageWriteAuthority(ShaderType::Compute, p.pipeline->cs_storage_write_authority);
	TraceShaderStorageNoStoreFailure(cs_code, ShaderType::Compute, input_info->bind, p.pipeline,
	                                ShaderStorageNoStoreProof(cs_code, ShaderType::Compute));

	const uint32_t previous_size = m_pipelines.Size();
	const uint32_t index         = StoreIn(&m_pipelines, p);
	if (previous_size == LEGACY_PIPELINE_LIMIT && index == previous_size)
	{
		std::fprintf(stderr,
		             "[GFX_PIPELINE_CACHE_GROWTH] previous=%u next=%u kind=compute policy=grow eviction=none\n",
		             previous_size, m_pipelines.Size());
		std::fflush(stderr);
	}

	return p.pipeline;
}

#if defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
bool GraphicsRenderRt0ComponentMaskContractTest()
{
	constexpr VkColorComponentFlags r = VK_COLOR_COMPONENT_R_BIT, g = VK_COLOR_COMPONENT_G_BIT;
	constexpr VkColorComponentFlags b = VK_COLOR_COMPONENT_B_BIT, a = VK_COLOR_COMPONENT_A_BIT;
	// Independent channel table: single components, every combination and the
	// historical none/full cases. Disabled color output preserves the raw key.
	constexpr VkColorComponentFlags expected[] = {
	    0, r, g, r | g, b, r | b, g | b, r | g | b,
	    a, r | a, g | a, r | g | a, b | a, r | b | a, g | b | a, r | g | b | a};
	for (uint32_t mask = 0; mask < std::size(expected); ++mask)
	{
		VkColorComponentFlags enabled = UINT32_MAX, disabled = UINT32_MAX;
		if (!ResolveRt0ColorWriteMask(mask, true, &enabled) || enabled != expected[mask] ||
		    !ResolveRt0ColorWriteMask(mask, false, &disabled) || disabled != 0) return false;
	}
	for (uint32_t upper_bit = 4; upper_bit < 32; ++upper_bit)
	for (uint32_t mask = 0; mask < std::size(expected); ++mask)
	for (const bool enabled: {false, true})
	{
		VkColorComponentFlags rejected = UINT32_MAX;
		if (ResolveRt0ColorWriteMask((uint32_t {1} << upper_bit) | mask, enabled, &rejected) || rejected != 0) return false;
	}
	VkColorComponentFlags rejected = UINT32_MAX;
	if (ResolveRt0ColorWriteMask(UINT32_MAX, true, &rejected) || rejected != 0 ||
	    ResolveRt0ColorWriteMask(0, true, nullptr)) return false;

	// Exercise the production cache lookup with exactly the same render pass
	// and shader IDs. Raw component masks remain distinct even when all their
	// effective writes are disabled; enable is a separate non-dynamic key.
	constexpr uint32_t states = 32;
	std::array<PipelineStaticParameters, states> static_params {};
	std::array<PipelineDynamicParameters, states> dynamic_params {};
	std::array<VulkanPipeline, states> pipelines {};
	Vector<PipelineCache::Pipeline> entries;
	for (uint32_t index = 0; index < states; ++index)
	{
		static_params[index].color_mask = index % 16;
		dynamic_params[index].color_write_enable = index >= 16;
		pipelines[index].static_params = &static_params[index];
		pipelines[index].dynamic_params = &dynamic_params[index];
		PipelineCache::Pipeline entry {};
		entry.render_pass_id = 7;
		entry.vs_shader_id.ids.Add(11);
		entry.ps_shader_id.ids.Add(17);
		entry.static_params = &static_params[index];
		entry.dynamic_params = &dynamic_params[index];
		entry.pipeline = &pipelines[index];
		if (PipelineCache::StoreIn(&entries, entry) != index) return false;
	}
	auto requested_static = static_params[0];
	auto requested_dynamic = dynamic_params[0];
	auto requested = entries[0];
	requested.static_params = &requested_static;
	requested.dynamic_params = &requested_dynamic;
	const auto find = [&](uint32_t index) {
		requested_static.color_mask = index % 16;
		requested_dynamic.color_write_enable = index >= 16;
		auto* found = PipelineCache::FindIn(entries, requested);
		if (found != &pipelines[index] || entries.Size() != states) return false;
		VkColorComponentFlags effective = UINT32_MAX;
		return ResolveRt0ColorWriteMask(found->static_params->color_mask, found->dynamic_params->color_write_enable, &effective) &&
		       effective == (index >= 16 ? expected[index % 16] : 0);
	};
	for (uint32_t index = 0; index < states; ++index) if (!find(index)) return false;
	// RGB/full/RGB and enabled/disabled/enabled must return their own original
	// entries; neither cache reuse nor a false enable can expand the write mask.
	for (const uint32_t index: {23u, 31u, 23u, 7u, 23u}) if (!find(index)) return false;
	requested_static.color_mask = 0x17;
	if (PipelineCache::FindIn(entries, requested) != nullptr) return false;
	std::fprintf(stderr, "[GFX_RT0_COMPONENT_MASK_TEST] masks=16 explicit-RGBA=pass disabled=zero"
	    " upper-bits=28 all-low4-neighbors=reject raw-mask/enable-cache-keys=32 distinct-hits=pass A/B/A=pass"
	    " mrt-count=unchanged shader-export-authority=unchanged blending=unchanged whole-image-tracking=unchanged"
	    " native-pixels=unqualified\n");
	return true;
}

bool GraphicsRenderPipelineCacheGrowthContractTest()
{
	if (!GraphicsRenderRt0ComponentMaskContractTest()) return false;
	VulkanPipeline live[PipelineCache::LEGACY_PIPELINE_LIMIT + 2] {};
	Vector<PipelineCache::Pipeline> entries;
	for (uint32_t index = 0; index < PipelineCache::LEGACY_PIPELINE_LIMIT + 1; ++index)
	{
		PipelineCache::Pipeline entry {};
		entry.pipeline = &live[index];
		if (PipelineCache::StoreIn(&entries, entry) != index || entries.Size() != index + 1)
		{
			return false;
		}
	}
	entries[37].pipeline = nullptr;
	PipelineCache::Pipeline replacement {};
	replacement.pipeline = &live[PipelineCache::LEGACY_PIPELINE_LIMIT + 1];
	return PipelineCache::StoreIn(&entries, replacement) == 37 &&
	       entries.Size() == PipelineCache::LEGACY_PIPELINE_LIMIT + 1 &&
	       entries[37].pipeline == replacement.pipeline;
}
#endif

void PipelineCache::DeletePipeline(VulkanPipeline* pipeline)
{
	Core::LockGuard lock(m_mutex);

	auto index = m_pipelines.Find(pipeline, [](auto r, auto p) { return p == r.pipeline; });

	EXIT_IF(!m_pipelines.IndexValid(index));

	if (m_pipelines.IndexValid(index))
	{
		DeletePipelineInternal(index);
	}
}

void PipelineCache::DeletePipelines(VulkanFramebuffer* framebuffer)
{
	EXIT_IF(framebuffer == nullptr);

	Core::LockGuard lock(m_mutex);

	int index = 0;
	for (auto& p: m_pipelines)
	{
		if (p.pipeline != nullptr && p.render_pass_id == framebuffer->render_pass_id)
		{
			DeletePipelineInternal(index);
		}
		index++;
	}
}

void PipelineCache::DeleteAllPipelines()
{
	Core::LockGuard lock(m_mutex);

	for (uint32_t index = 0; index < m_pipelines.Size(); index++)
	{
		if (m_pipelines[index].pipeline != nullptr)
		{
			DeletePipelineInternal(index);
		}
	}
	m_pipelines.Clear();
}

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool GraphicsRenderPipelineCacheTombstoneContractTest()
{
	if (g_render_ctx == nullptr)
	{
		GraphicsRenderInit();
	}

	auto* cache = g_render_ctx->GetPipelineCache();
	if (cache == nullptr)
	{
		return false;
	}

	{
		Core::LockGuard lock(cache->m_mutex);
		if (cache->m_pipelines.Size() != 0)
		{
			return false;
		}

		PipelineCache::Pipeline tombstone {};
		cache->m_pipelines.Add(tombstone);
		cache->m_pipelines.Add(tombstone);
	}

	cache->DeleteAllPipelines();
	cache->DeleteAllPipelines();

	Core::LockGuard lock(cache->m_mutex);
	return cache->m_pipelines.Size() == 0;
}
#endif

#if defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
bool GraphicsRenderVertexInputFormatContractTest()
{
	ShaderBufferResource snorm_xyz {};
	snorm_xyz.fields[1] = 32u << 16u;
	snorm_xyz.fields[2] = 4;
	snorm_xyz.fields[3] = DstSel(4, 5, 6, 7) | (1u << 12u) | (10u << 15u);
	ShaderVertexDestination destination_xyz {12, 3};
	VkFormat               format = VK_FORMAT_UNDEFINED;
	uint32_t               size   = 0;
	if (snorm_xyz.DstSelXYZW() != DstSel(4, 5, 6, 7) || snorm_xyz.Nfmt() != 1 || snorm_xyz.Dfmt() != 10 ||
	    (snorm_xyz.fields[3] & 0x7ffffu) != 0x51facu ||
	    !ShaderVertexInputSnorm8x4XyzCompatible(snorm_xyz, destination_xyz.registers_num) ||
	    !TryGetInputFormat(snorm_xyz, destination_xyz, &format, &size) ||
	    format != VK_FORMAT_R8G8B8A8_SNORM || size != 4)
	{
		return false;
	}
	for (const int registers_num: {1, 2, 4})
	{
		auto wrong_destination = destination_xyz;
		wrong_destination.registers_num = registers_num;
		format = VK_FORMAT_UNDEFINED;
		size = 0;
		if (ShaderVertexInputSnorm8x4XyzCompatible(snorm_xyz, registers_num) ||
		    TryGetInputFormat(snorm_xyz, wrong_destination, &format, &size) ||
		    format != VK_FORMAT_UNDEFINED || size != 0)
		{
			return false;
		}
	}
	for (uint32_t channel = 0; channel < 4; ++channel)
	{
		auto wrong_selector = snorm_xyz;
		wrong_selector.fields[3] ^= 1u << (channel * 3u);
		format = VK_FORMAT_UNDEFINED;
		size = 0;
		if (ShaderVertexInputSnorm8x4XyzCompatible(wrong_selector, 3) ||
		    TryGetInputFormat(wrong_selector, destination_xyz, &format, &size) ||
		    format != VK_FORMAT_UNDEFINED || size != 0)
		{
			return false;
		}
	}
	for (const uint32_t format_bit: {1u << 13u, 1u << 15u})
	{
		auto wrong_format = snorm_xyz;
		wrong_format.fields[3] ^= format_bit;
		format = VK_FORMAT_UNDEFINED;
		size = 0;
		if (ShaderVertexInputSnorm8x4XyzCompatible(wrong_format, 3) ||
		    TryGetInputFormat(wrong_format, destination_xyz, &format, &size) ||
		    format != VK_FORMAT_UNDEFINED || size != 0)
		{
			return false;
		}
	}

	ShaderBufferResource signed_xy {};
	signed_xy.fields[1] = 32u << 16u;
	signed_xy.fields[2] = 4;
	signed_xy.fields[3] = DstSel(4, 5) | (5u << 12u) | (5u << 15u);
	ShaderVertexDestination destination {16, 2};
	format = VK_FORMAT_UNDEFINED;
	size   = 0;
	if (!ShaderVertexInputSint16x2Compatible(signed_xy, destination.registers_num) ||
	    !TryGetInputFormat(signed_xy, destination, &format, &size) || format != VK_FORMAT_R16G16_SINT || size != 2)
	{
		return false;
	}
	ShaderBufferResource signed_xyzw {};
	signed_xyzw.fields[1] = 16u << 16u;
	signed_xyzw.fields[2] = 4;
	signed_xyzw.fields[3] = DstSel(4, 5, 6, 7) | (5u << 12u) | (12u << 15u);
	ShaderVertexDestination destination_xyzw {20, 4};
	if (!ShaderVertexInputSint16x4Compatible(signed_xyzw, destination_xyzw.registers_num) ||
	    !TryGetInputFormat(signed_xyzw, destination_xyzw, &format, &size) || format != VK_FORMAT_R16G16B16A16_SINT || size != 4)
	{
		return false;
	}
	for (const int registers_num: {1, 2, 3})
	{
		auto wrong_destination          = destination_xyzw;
		wrong_destination.registers_num = registers_num;
		format                          = VK_FORMAT_UNDEFINED;
		size                            = 0;
		if (ShaderVertexInputSint16x4Compatible(signed_xyzw, registers_num) ||
		    TryGetInputFormat(signed_xyzw, wrong_destination, &format, &size) || format != VK_FORMAT_UNDEFINED || size != 0)
		{
			return false;
		}
	}

	auto wrong_nfmt = signed_xy;
	wrong_nfmt.fields[3] ^= 1u << 12u;
	auto wrong_dfmt = signed_xy;
	wrong_dfmt.fields[3] ^= 1u << 15u;
	for (const auto& unsupported: {wrong_nfmt, wrong_dfmt})
	{
		format = VK_FORMAT_UNDEFINED;
		size   = 0;
		if (ShaderVertexInputSint16x2Compatible(unsupported, destination.registers_num) ||
		    TryGetInputFormat(unsupported, destination, &format, &size) || format != VK_FORMAT_UNDEFINED || size != 0)
		{
			return false;
		}
	}
	auto wrong_xyzw_nfmt = signed_xyzw;
	wrong_xyzw_nfmt.fields[3] ^= 1u << 12u;
	auto wrong_xyzw_dfmt = signed_xyzw;
	wrong_xyzw_dfmt.fields[3] ^= 1u << 15u;
	for (const auto& unsupported: {wrong_xyzw_nfmt, wrong_xyzw_dfmt})
	{
		format = VK_FORMAT_UNDEFINED;
		size   = 0;
		if (ShaderVertexInputSint16x4Compatible(unsupported, destination_xyzw.registers_num) ||
		    TryGetInputFormat(unsupported, destination_xyzw, &format, &size) || format != VK_FORMAT_UNDEFINED || size != 0)
		{
			return false;
		}
	}
	for (const int registers_num: {1, 3, 4})
	{
		auto wrong_destination          = destination;
		wrong_destination.registers_num = registers_num;
		if (ShaderVertexInputSint16x2Compatible(signed_xy, registers_num) ||
		    TryGetInputFormat(signed_xy, wrong_destination, &format, &size))
		{
			return false;
		}
	}

	ShaderBufferResource float_xy {};
	float_xy.fields[3] = DstSel(4, 5) | (7u << 12u) | (11u << 15u);
	return TryGetInputFormat(float_xy, destination, &format, &size) && format == VK_FORMAT_R32G32_SFLOAT && size == 2 &&
	       !TryGetInputFormat(signed_xy, destination, nullptr, &size) && !TryGetInputFormat(signed_xy, destination, &format, nullptr);
}

bool GraphicsRenderBlendConstantContractTest()
{
	constexpr float HW::BlendColor::* components[] = {
	    &HW::BlendColor::red, &HW::BlendColor::green, &HW::BlendColor::blue, &HW::BlendColor::alpha};
	for (auto component: components)
	{
		HW::BlendColor color;
		for (float accepted: {0.0f, -0.0f, std::numeric_limits<float>::denorm_min(), 0.25f, 0.5f, 1.0f})
		{
			color.*component = accepted;
			if (!BlendConstantsSupported(color)) return false;
		}
		for (float rejected: {-std::numeric_limits<float>::denorm_min(), -1.0f, std::nextafter(1.0f, 2.0f),
		                      std::numeric_limits<float>::max(), std::numeric_limits<float>::quiet_NaN(),
		                      std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()})
		{
			color.*component = rejected;
			if (BlendConstantsSupported(color)) return false;
		}
	}
	if (get_blend_factor(0x0d) != VK_BLEND_FACTOR_CONSTANT_COLOR ||
	    get_blend_factor(0x0e) != VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR ||
	    get_blend_factor(0x13) != VK_BLEND_FACTOR_CONSTANT_ALPHA ||
	    get_blend_factor(0x14) != VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA) return false;

	// Use the production state copy and actual FindIn lookup. Each component
	// must distinguish a pipeline, coexist with the others, and remain reusable.
	PipelineStaticParameters static_parameters[5];
	PipelineDynamicParameters dynamic_parameters;
	VulkanPipeline pipelines[5];
	Vector<PipelineCache::Pipeline> entries;
	PipelineCache::Pipeline key;
	key.render_pass_id = 7; key.vs_shader_id.ids.Add(11); key.ps_shader_id.ids.Add(17);
	key.dynamic_params = &dynamic_parameters;
	for (uint32_t index = 0; index < 5; ++index)
	{
		HW::BlendColor color;
		if (index != 0) color.*components[index - 1] = static_cast<float>(index) * 0.125f;
		SetPipelineBlendConstants(&static_parameters[index], color);
		const auto& value = static_parameters[index];
		if (value.blend_color_red != color.red || value.blend_color_green != color.green ||
		    value.blend_color_blue != color.blue || value.blend_color_alpha != color.alpha) return false;
		key.static_params = &static_parameters[index];
		if (PipelineCache::FindIn(entries, key) != nullptr) return false;
		pipelines[index].static_params = &static_parameters[index];
		pipelines[index].dynamic_params = &dynamic_parameters;
		key.pipeline = &pipelines[index];
		entries.Add(key);
		if (PipelineCache::FindIn(entries, key) != &pipelines[index]) return false;
	}
	for (uint32_t index = 0; index < 5; ++index)
	{
		// Copy the values into different storage, as for a later draw.
		auto requested = static_parameters[index];
		key.static_params = &requested;
		if (PipelineCache::FindIn(entries, key) != &pipelines[index]) return false;
	}
	HW::BlendColor signed_zero;
	signed_zero.red = -0.0f;
	PipelineStaticParameters copied;
	SetPipelineBlendConstants(&copied, signed_zero);
	return std::signbit(copied.blend_color_red) && entries.Size() == 5;
}

bool GraphicsRenderStencilClearMasksForTest(bool stencil_clear, bool depth_clear, uint32_t front_write_mask,
                                           uint32_t back_write_mask, uint32_t* front_out, uint32_t* back_out)
{
	if (front_out == nullptr || back_out == nullptr) return false;
	RenderDepthInfo depth;
	depth.stencil_clear_enable = stencil_clear;
	depth.depth_clear_enable = depth_clear;
	depth.stencil_dynamic_front.writeMask = front_write_mask;
	depth.stencil_dynamic_back.writeMask = back_write_mask;
	PipelineDynamicParameters parameters;
	SetPipelineStencilDynamicState(&parameters, depth);
	*front_out = parameters.stencil_front.writeMask;
	*back_out = parameters.stencil_back.writeMask;
	return true;
}

bool GraphicsRenderStencilClearContractTest()
{
	const auto same_static = [](const PipelineStencilStaticState& a, const PipelineStencilStaticState& b) {
		return a.failOp == b.failOp && a.passOp == b.passOp && a.depthFailOp == b.depthFailOp && a.compareOp == b.compareOp;
	};
	const auto same_dynamic = [](const PipelineStencilDynamicState& a, const PipelineStencilDynamicState& b) {
		return a.compareMask == b.compareMask && a.writeMask == b.writeMask && a.reference == b.reference;
	};
	const auto same_stencil = [&](const RenderDepthInfo& a, const RenderDepthInfo& b) {
		return a.stencil_test_enable == b.stencil_test_enable && same_static(a.stencil_static_front, b.stencil_static_front) &&
		       same_static(a.stencil_static_back, b.stencil_static_back) &&
		       same_dynamic(a.stencil_dynamic_front, b.stencil_dynamic_front) &&
		       same_dynamic(a.stencil_dynamic_back, b.stencil_dynamic_back);
	};
	HW::DepthControl control;
	control.stencil_enable = true;
	control.backface_enable = true;
	control.stencilfunc = control.stencilfunc_bf = 7;
	HW::StencilControl stencil;
	stencil.stencil_zpass = stencil.stencil_zpass_bf = 1;
	HW::StencilMask mask;
	mask.stencil_mask = mask.stencil_mask_bf = 0xff;
	mask.stencil_writemask = mask.stencil_writemask_bf = 0xff;
	mask.stencil_opval = mask.stencil_opval_bf = 1;
	RenderDepthInfo equivalent;
	if (!ResolveRenderStencilState(control, stencil, mask, &equivalent)) return false;
	d_check(control, stencil, mask);
	const PipelineStencilStaticState expected_static = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_KEEP,
	                                                    VK_COMPARE_OP_ALWAYS};
	const PipelineStencilDynamicState expected_dynamic = {0xff, 0xff, 0};
	if (!equivalent.stencil_test_enable || !same_static(equivalent.stencil_static_front, expected_static) ||
	    !same_static(equivalent.stencil_static_back, expected_static) ||
	    !same_dynamic(equivalent.stencil_dynamic_front, expected_dynamic) ||
	    !same_dynamic(equivalent.stencil_dynamic_back, expected_dynamic)) return false;

	// Every operation already understood by the front decoder produces the same
	// pipeline state through the equivalent-face route and the legacy route.
	for (uint8_t op = 0; op <= 9; ++op)
	{
		auto equal_stencil = stencil;
		equal_stencil.stencil_zpass = equal_stencil.stencil_zpass_bf = op;
		auto equal_mask = mask;
		equal_mask.stencil_testval = equal_mask.stencil_testval_bf = 0x29;
		equal_mask.stencil_opval = equal_mask.stencil_opval_bf = (op == 5 || op == 6 || op == 8 || op == 9) ? 1 : 0x29;
		RenderDepthInfo both, legacy;
		if (!ResolveRenderStencilState(control, equal_stencil, equal_mask, &both)) return false;
		auto legacy_control = control;
		legacy_control.backface_enable = false;
		equal_stencil.stencil_zpass_bf = 0;
		if (!ResolveRenderStencilState(legacy_control, equal_stencil, equal_mask, &legacy) || !same_stencil(both, legacy)) return false;
	}
	const auto rejects_without_writes = [&](const HW::DepthControl& c, const HW::StencilControl& s, const HW::StencilMask& m) {
		auto output = equivalent;
		return !ResolveRenderStencilState(c, s, m, &output) && same_stencil(output, equivalent);
	};
	for (const bool enabled: {false, true})
	{
		for (uint8_t op: {5, 6, 8, 9})
			for (uint8_t operand: {0, 2, 255})
				for (int outcome = 0; outcome < 3; ++outcome)
				{
					auto arithmetic_control = control;
					arithmetic_control.stencil_enable = enabled;
					auto arithmetic_stencil = stencil;
					if (outcome == 0) arithmetic_stencil.stencil_fail = arithmetic_stencil.stencil_fail_bf = op;
					if (outcome == 1) arithmetic_stencil.stencil_zpass = arithmetic_stencil.stencil_zpass_bf = op;
					if (outcome == 2) arithmetic_stencil.stencil_zfail = arithmetic_stencil.stencil_zfail_bf = op;
					auto arithmetic_mask = mask;
					arithmetic_mask.stencil_opval = arithmetic_mask.stencil_opval_bf = operand;
					if (!rejects_without_writes(arithmetic_control, arithmetic_stencil, arithmetic_mask)) return false;
				}
		// Raw equality alone does not qualify comparison references selected by
		// the legacy decoder. Non-Always states remain refused even while idle.
		for (uint8_t function = 0; function < 7; ++function)
		{
			auto non_always = control;
			non_always.stencil_enable = enabled;
			non_always.stencilfunc = non_always.stencilfunc_bf = function;
			if (!rejects_without_writes(non_always, stencil, mask)) return false;
		}
		auto unequal_control = control;
		unequal_control.stencil_enable = enabled;
		++unequal_control.stencilfunc_bf;
		if (!rejects_without_writes(unequal_control, stencil, mask)) return false;
		unequal_control.stencilfunc_bf = control.stencilfunc_bf;
		for (auto field: {&HW::StencilControl::stencil_fail_bf, &HW::StencilControl::stencil_zpass_bf,
		                  &HW::StencilControl::stencil_zfail_bf})
		{
			auto unequal_stencil = stencil;
			++(unequal_stencil.*field);
			if (!rejects_without_writes(unequal_control, unequal_stencil, mask)) return false;
		}
		for (auto field: {&HW::StencilMask::stencil_testval_bf, &HW::StencilMask::stencil_mask_bf,
		                  &HW::StencilMask::stencil_writemask_bf, &HW::StencilMask::stencil_opval_bf})
		{
			auto unequal_mask = mask;
			++(unequal_mask.*field);
			if (!rejects_without_writes(unequal_control, stencil, unequal_mask)) return false;
		}
		// The inactive legacy route still requires all three BF operations zero.
		unequal_control.backface_enable = false;
		for (auto field: {&HW::StencilControl::stencil_fail_bf, &HW::StencilControl::stencil_zpass_bf,
		                  &HW::StencilControl::stencil_zfail_bf})
		{
			auto legacy_stencil = stencil;
			legacy_stencil.stencil_zpass_bf = 0;
			legacy_stencil.*field = 1;
			if (!rejects_without_writes(unequal_control, legacy_stencil, mask)) return false;
		}
	}
	auto disabled_control = control;
	disabled_control.stencil_enable = false;
	RenderDepthInfo disabled;
	RenderDepthInfo reset;
	if (!ResolveRenderStencilState(disabled_control, stencil, mask, &disabled) || !same_stencil(disabled, reset) ||
	    ResolveRenderStencilState(control, stencil, mask, nullptr)) return false;

	PipelineStaticParameters cached_static;
	cached_static.stencil_test_enable = true;
	cached_static.stencil_front = {VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INVERT, VK_COMPARE_OP_EQUAL};
	cached_static.stencil_back  = {VK_STENCIL_OP_INVERT, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_NOT_EQUAL};
	PipelineDynamicParameters cached_dynamic;
	cached_dynamic.vk_dynamic_state_stencil_compare_mask = true;
	cached_dynamic.vk_dynamic_state_stencil_write_mask = true;
	cached_dynamic.vk_dynamic_state_stencil_reference = true;
	VulkanPipeline pipeline;
	pipeline.static_params = &cached_static;
	pipeline.dynamic_params = &cached_dynamic;
	PipelineCache::Pipeline entry;
	entry.pipeline = &pipeline;
	entry.static_params = &cached_static;
	entry.dynamic_params = &cached_dynamic;
	Vector<PipelineCache::Pipeline> entries;
	entries.Add(entry);
	auto requested_static = cached_static;
	auto requested_dynamic = cached_dynamic;
	auto requested = entry;
	requested.static_params = &requested_static;
	requested.dynamic_params = &requested_dynamic;
	RenderDepthInfo depth;
	depth.stencil_dynamic_front = {0x7fu, 0x35u, 0x19u};
	depth.stencil_dynamic_back  = {0xe3u, 0xcau, 0x82u};
	for (const bool depth_clear: {false, true})
	{
		depth.depth_clear_enable = depth_clear;
		for (const bool stencil_clear: {false, true, false})
		{
			depth.stencil_clear_enable = stencil_clear;
			SetPipelineStencilDynamicState(&requested_dynamic, depth);
			auto* found = PipelineCache::FindIn(entries, requested);
			if (found != &pipeline || entries.Size() != 1 || !(*found->static_params == requested_static)) return false;
			const auto& front = found->dynamic_params->stencil_front;
			const auto& back  = found->dynamic_params->stencil_back;
			if (front.writeMask != (stencil_clear ? 0u : 0x35u) || back.writeMask != (stencil_clear ? 0u : 0xcau) ||
			    front.compareMask != 0x7fu || back.compareMask != 0xe3u || front.reference != 0x19u || back.reference != 0x82u ||
			    depth.stencil_dynamic_front.writeMask != 0x35u || depth.stencil_dynamic_back.writeMask != 0xcau)
			{
				return false;
			}
		}
	}
	return true;
}

bool GraphicsRenderPolygonOffsetContractTest()
{
	const auto set_float = [](HW::Context& ctx, uint32_t index, float value) {
		uint32_t word;
		std::memcpy(&word, &value, sizeof(word));
		ctx.SetPolygonOffsetRegister(index, word);
	};
	const auto both_faces = [&](float scale, float offset) {
		HW::Context ctx;
		HW::ModeControl mode;
		mode.poly_offset_front_enable = true;
		mode.poly_offset_back_enable = true;
		ctx.SetModeControl(mode);
		ctx.SetPolygonOffsetRegister(0, 0x1e9u);
		set_float(ctx, 2, scale); set_float(ctx, 3, offset);
		set_float(ctx, 4, scale); set_float(ctx, 5, offset);
		return ctx;
	};
	const auto off = [](const PipelineDepthBias& bias) {
		return !bias.enabled && bias.representation == PipelineDepthBiasRepresentation::FormatRelative &&
		       bias.constant == 0.0f && !std::signbit(bias.constant) &&
		       bias.slope == 0.0f && !std::signbit(bias.slope) && bias.clamp == 0.0f && !std::signbit(bias.clamp);
	};
	PipelineDepthBias bias;
	const char* reason = nullptr;
	HW::Context zero;
	HW::ModeControl mode;
	mode.poly_offset_front_enable = true;
	zero.SetModeControl(mode);
	// Reset-zero is effective zero even with one enabled face and D32/S8.
	if (!ResolvePolygonOffset(zero, VK_FORMAT_D32_SFLOAT_S8_UINT, true, &bias, &reason) || !off(bias) || reason != nullptr) return false;
	set_float(zero, 1, -7.0f);
	zero.SetPolygonOffsetRegister(0, 0xffffffffu);
	if (!ResolvePolygonOffset(zero, VK_FORMAT_UNDEFINED, false, &bias) || !off(bias)) return false;
	set_float(zero, 2, -0.0f); set_float(zero, 3, -0.0f);
	if (!ResolvePolygonOffset(zero, VK_FORMAT_D16_UNORM, true, &bias) || !off(bias)) return false;
	set_float(zero, 1, std::numeric_limits<float>::infinity());
	if (ResolvePolygonOffset(zero, VK_FORMAT_D32_SFLOAT, true, &bias)) return false;
	mode.poly_offset_front_enable = false;
	mode.cull_front = true; mode.cull_back = true;
	zero.SetModeControl(mode);
	set_float(zero, 2, std::numeric_limits<float>::quiet_NaN());
	if (!ResolvePolygonOffset(zero, VK_FORMAT_UNDEFINED, false, &bias) || !off(bias)) return false;
	mode.cull_front = false; mode.cull_back = false;
	zero.SetModeControl(mode);

	const auto active = both_faces(32.0f, -2.0f);
	if (!ResolvePolygonOffset(active, VK_FORMAT_D32_SFLOAT, true, &bias) || !bias.enabled ||
	    bias.representation != PipelineDepthBiasRepresentation::FormatRelative || bias.constant != -2.0f ||
	    bias.slope != 2.0f || bias.clamp != 0.0f) return false;
	if (!ResolvePolygonOffset(active, VK_FORMAT_D32_SFLOAT_S8_UINT, true, &bias) || !bias.enabled ||
	    bias.representation != PipelineDepthBiasRepresentation::FormatRelative || bias.constant != -2.0f ||
	    bias.slope != 2.0f || bias.clamp != 0.0f) return false;
	for (int mutation = 0; mutation < 15; ++mutation)
	{
		auto bad = active;
		auto changed_mode = bad.GetModeControl();
		switch (mutation)
		{
			case 0: changed_mode.poly_offset_back_enable = false; bad.SetModeControl(changed_mode); break;
			case 1: set_float(bad, 4, 16.0f); break;
			case 2: set_float(bad, 5, -1.0f); break;
			case 3: set_float(bad, 1, 1.0f); break;
			case 4: set_float(bad, 1, -1.0f); break;
			case 5: set_float(bad, 1, std::numeric_limits<float>::quiet_NaN()); break;
			case 6: set_float(bad, 2, std::numeric_limits<float>::infinity()); break;
			case 7: set_float(bad, 3, -std::numeric_limits<float>::infinity()); break;
			case 8: set_float(bad, 4, std::numeric_limits<float>::quiet_NaN()); break;
			case 9: set_float(bad, 5, std::numeric_limits<float>::quiet_NaN()); break;
			case 10: bad.SetPolygonOffsetRegister(0, 0); break;
			case 11: bad.SetPolygonOffsetRegister(0, 0x1e8u); break;
			case 12: bad.SetPolygonOffsetRegister(0, 0x2e9u); break;
			case 13: bad.SetPolygonOffsetRegister(2, 1); bad.SetPolygonOffsetRegister(4, 1); break;
			case 14: bad.SetPolygonOffsetRegister(2, 17); bad.SetPolygonOffsetRegister(4, 17); break;
		}
		if (ResolvePolygonOffset(bad, VK_FORMAT_D32_SFLOAT, true, &bias, &reason) || reason == nullptr || !off(bias)) return false;
	}
	for (const auto format: {VK_FORMAT_UNDEFINED, VK_FORMAT_D16_UNORM, VK_FORMAT_D24_UNORM_S8_UINT})
		if (ResolvePolygonOffset(active, format, true, &bias)) return false;
	if (ResolvePolygonOffset(active, VK_FORMAT_D32_SFLOAT, false, &bias)) return false;
	for (bool front_visible: {false, true})
	{
		auto single = active;
		auto single_mode = single.GetModeControl();
		single_mode.cull_front = !front_visible;
		single_mode.cull_back = front_visible;
		single.SetModeControl(single_mode);
		set_float(single, front_visible ? 4 : 2, std::numeric_limits<float>::quiet_NaN());
		if (!ResolvePolygonOffset(single, VK_FORMAT_D32_SFLOAT, true, &bias) || !bias.enabled ||
		    bias.constant != -2.0f || bias.slope != 2.0f) return false;
		if (front_visible) single_mode.poly_offset_front_enable = false;
		else single_mode.poly_offset_back_enable = false;
		single.SetModeControl(single_mode);
		set_float(single, 1, std::numeric_limits<float>::quiet_NaN());
		if (!ResolvePolygonOffset(single, VK_FORMAT_UNDEFINED, false, &bias) || !off(bias)) return false;
		single_mode.cull_front = true; single_mode.cull_back = true;
		single.SetModeControl(single_mode);
		if (ResolvePolygonOffset(single, VK_FORMAT_D32_SFLOAT, true, &bias)) return false;
	}
	const auto negative_slope = both_faces(-16.0f, 3.0f);
	if (!ResolvePolygonOffset(negative_slope, VK_FORMAT_D32_SFLOAT, true, &bias) || bias.slope != -1.0f || bias.constant != 3.0f) return false;
	auto signed_zero = both_faces(16.0f, -0.0f);
	set_float(signed_zero, 1, -0.0f);
	if (!ResolvePolygonOffset(signed_zero, VK_FORMAT_D32_SFLOAT, true, &bias) || !bias.enabled ||
	    bias.slope != 1.0f || bias.constant != 0.0f || std::signbit(bias.constant) || std::signbit(bias.clamp)) return false;
	auto small = both_faces(0.0f, 0.0f);
	small.SetPolygonOffsetRegister(2, 0x02800000u); small.SetPolygonOffsetRegister(4, 0x02800000u);
	if (!ResolvePolygonOffset(small, VK_FORMAT_D32_SFLOAT, true, &bias) || bias.slope != std::numeric_limits<float>::min()) return false;
	auto maximum = both_faces(std::numeric_limits<float>::max(), 0.0f);
	if (!ResolvePolygonOffset(maximum, VK_FORMAT_D32_SFLOAT, true, &bias) || !bias.enabled || !std::isfinite(bias.slope)) return false;

	auto absolute = both_faces(-4.8f, -0.3f);
	absolute.SetPolygonOffsetRegister(0, 0u);
	absolute.SetViewportZScale(0, 0.5f);
	absolute.SetViewportZOffset(0, 0.5f);
	if (!ResolvePolygonOffset(absolute, VK_FORMAT_D32_SFLOAT_S8_UINT, true, &bias) || !bias.enabled ||
	    bias.representation != PipelineDepthBiasRepresentation::AbsoluteWindowDepth || bias.constant != -0.3f ||
	    bias.slope != -0.3f || bias.clamp != 0.0f) return false;
	VkViewport translated {};
	translated.minDepth = 0.5f;
	translated.maxDepth = 1.0f;
	if (!ApplyAbsoluteDepthBiasViewport(bias, &translated) || translated.minDepth != 0.5f + -0.3f ||
	    translated.maxDepth != 1.0f + -0.3f || translated.maxDepth - translated.minDepth != 0.5f) return false;
	for (const auto topology: {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
	                          VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN})
		if (!AbsoluteDepthBiasTopologySupported(bias, topology)) return false;
	for (const auto topology: {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
	                          VK_PRIMITIVE_TOPOLOGY_LINE_STRIP})
		if (AbsoluteDepthBiasTopologySupported(bias, topology)) return false;
	PipelineDepthBias relative_topology = bias;
	relative_topology.representation = PipelineDepthBiasRepresentation::FormatRelative;
	if (!AbsoluteDepthBiasTopologySupported(relative_topology, VK_PRIMITIVE_TOPOLOGY_POINT_LIST) ||
	    !AbsoluteDepthBiasTopologySupported(PipelineDepthBias {}, VK_PRIMITIVE_TOPOLOGY_LINE_LIST)) return false;
	auto out_of_range = absolute;
	out_of_range.SetViewportZOffset(0, 0.0f);
	if (ResolvePolygonOffset(out_of_range, VK_FORMAT_D32_SFLOAT_S8_UINT, true, &bias, &reason) ||
	    reason == nullptr || !off(bias)) return false;

	// Exercise the actual cache lookup: canonical OFF reuses, effective bias
	// changes miss, ON can coexist, and returning to OFF selects its old entry.
	PipelineStaticParameters cached_static;
	PipelineDynamicParameters cached_dynamic;
	cached_static.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VulkanPipeline pipeline;
	pipeline.static_params = &cached_static; pipeline.dynamic_params = &cached_dynamic;
	PipelineCache::Pipeline entry;
	entry.render_pass_id = 7; entry.vs_shader_id.ids.Add(11); entry.ps_shader_id.ids.Add(17);
	entry.pipeline = &pipeline; entry.static_params = &cached_static; entry.dynamic_params = &cached_dynamic;
	Vector<PipelineCache::Pipeline> entries;
	entries.Add(entry);
	auto requested_static = cached_static;
	auto requested_dynamic = cached_dynamic;
	auto requested = entry;
	requested.static_params = &requested_static; requested.dynamic_params = &requested_dynamic;
	HW::Context reset_front;
	HW::ModeControl reset_front_mode;
	reset_front_mode.poly_offset_front_enable = true;
	reset_front.SetModeControl(reset_front_mode);
	if (!ResolvePolygonOffset(reset_front, VK_FORMAT_D32_SFLOAT_S8_UINT, true, &requested_static.depth_bias) ||
	    PipelineCache::FindIn(entries, requested) != &pipeline) return false;
	for (uint32_t index = 0; index < 6; ++index) reset_front.SetPolygonOffsetRegister(index, 0);
	if (!ResolvePolygonOffset(reset_front, VK_FORMAT_D32_SFLOAT_S8_UINT, true, &requested_static.depth_bias) ||
	    PipelineCache::FindIn(entries, requested) != &pipeline) return false;
	if (!ResolvePolygonOffset(zero, VK_FORMAT_UNDEFINED, false, &requested_static.depth_bias) ||
	    PipelineCache::FindIn(entries, requested) != &pipeline) return false;
	if (!ResolvePolygonOffset(active, VK_FORMAT_D32_SFLOAT, true, &requested_static.depth_bias) ||
	    PipelineCache::FindIn(entries, requested) != nullptr) return false;
	if (!ResolvePolygonOffset(absolute, VK_FORMAT_D32_SFLOAT_S8_UINT, true, &requested_static.depth_bias) ||
	    PipelineCache::FindIn(entries, requested) != nullptr) return false;
	if (!ResolvePolygonOffset(active, VK_FORMAT_D32_SFLOAT, true, &requested_static.depth_bias)) return false;
	auto on_static = requested_static;
	auto on_dynamic = cached_dynamic;
	VulkanPipeline on_pipeline;
	on_pipeline.static_params = &on_static; on_pipeline.dynamic_params = &on_dynamic;
	auto on_entry = entry;
	on_entry.pipeline = &on_pipeline; on_entry.static_params = &on_static; on_entry.dynamic_params = &on_dynamic;
	entries.Add(on_entry);
	if (PipelineCache::FindIn(entries, requested) != &on_pipeline) return false;
	for (int change = 0; change < 4; ++change)
	{
		requested_static = on_static;
		switch (change)
		{
			case 0: requested_static.depth_bias.enabled = false; break;
			case 1: requested_static.depth_bias.constant = 1.0f; break;
			case 2: requested_static.depth_bias.slope = 1.0f; break;
			case 3: requested_static.depth_bias.clamp = 1.0f; break;
		}
		if (PipelineCache::FindIn(entries, requested) != nullptr) return false;
	}
	requested_static = cached_static;
	return PipelineCache::FindIn(entries, requested) == &pipeline && entries.Size() == 2;
}

bool GraphicsRenderDynamicScissorContractTest()
{
	HW::ScreenViewport viewport;
	viewport.viewports[0].zmax = 1.0f;
	viewport.guard_band_horz_discard = 1.0f;
	viewport.guard_band_vert_discard = 1.0f;
	viewport.screen_scissor_right = viewport.generic_scissor_right = 1920;
	viewport.screen_scissor_bottom = viewport.generic_scissor_bottom = 1080;
	viewport.viewports[0].viewport_scissor_right = 4095;
	viewport.viewports[0].viewport_scissor_bottom = 4095;
	HW::ScanModeControl mode;
	mode.vport_scissor_enable = false;
	int selected[4] = {123, 456, 789, 1011};
	auto matches = [&](int left, int top, int right, int bottom)
	{
		return selected[0] == left && selected[1] == top && selected[2] == right && selected[3] == bottom;
	};
	if (IsViewportScissorActive(viewport, mode) || !SelectGraphicsScissor(viewport, mode, selected) ||
	    !matches(0, 0, 1920, 1080)) return false;
	vp_check(viewport, mode);
	// Stale disabled bounds and their window flag cannot change the active
	// generic/screen restriction, including a rectangle the host would reject.
	viewport.viewports[0].viewport_scissor_left = INT32_MAX;
	viewport.viewports[0].viewport_scissor_top = -7;
	viewport.viewports[0].viewport_scissor_right = -11;
	viewport.viewports[0].viewport_scissor_bottom = INT32_MIN;
	viewport.viewports[0].viewport_scissor_window_offset_enable = true;
	if (!SelectGraphicsScissor(viewport, mode, selected) || !matches(0, 0, 1920, 1080)) return false;
	vp_check(viewport, mode);
	viewport.generic_scissor_left = 10;
	viewport.generic_scissor_top = 20;
	viewport.generic_scissor_right = 1010;
	viewport.generic_scissor_bottom = 720;
	if (!SelectGraphicsScissor(viewport, mode, selected) || !matches(10, 20, 1010, 720)) return false;
	vp_check(viewport, mode);
	// Existing generic/screen containment also permits zero host coverage.
	viewport.generic_scissor_right = 10;
	if (!SelectGraphicsScissor(viewport, mode, selected) || !matches(10, 20, 10, 720)) return false;
	vp_check(viewport, mode);
	viewport.generic_scissor_right = 1010;
	if (!SelectGraphicsScissor(viewport, mode, selected) || !matches(10, 20, 1010, 720)) return false;

	// Re-enabling malformed retained viewport bounds must reject without writes.
	mode.vport_scissor_enable = true;
	if (SelectGraphicsScissor(viewport, mode, selected) || !matches(10, 20, 1010, 720)) return false;
	viewport.viewports[0].viewport_scissor_window_offset_enable = false;
	viewport.viewports[0].viewport_scissor_left = viewport.viewports[0].viewport_scissor_top = 0;
	viewport.viewports[0].viewport_scissor_right = 1920;
	viewport.viewports[0].viewport_scissor_bottom = 1080;
	viewport.generic_scissor_left = viewport.generic_scissor_top = 0;
	viewport.generic_scissor_right = 1920;
	viewport.generic_scissor_bottom = 1080;
	if (!SelectGraphicsScissor(viewport, mode, selected) || !matches(0, 0, 1920, 1080)) return false;
	vp_check(viewport, mode);

	// Any scissor can be the tightest rectangle. Exercise all assignments of
	// three nested rectangles, independently of the order of the registers.
	const int nested[3][4] = {{0, 0, 1920, 1080}, {10, 20, 1010, 720}, {30, 40, 130, 240}};
	auto set_bounds = [](HW::ScreenViewport& state, const int (&v)[4], const int (&g)[4], const int (&s)[4])
	{
		state.viewports[0].viewport_scissor_left = v[0]; state.viewports[0].viewport_scissor_top = v[1];
		state.viewports[0].viewport_scissor_right = v[2]; state.viewports[0].viewport_scissor_bottom = v[3];
		state.generic_scissor_left = g[0]; state.generic_scissor_top = g[1];
		state.generic_scissor_right = g[2]; state.generic_scissor_bottom = g[3];
		state.screen_scissor_left = s[0]; state.screen_scissor_top = s[1];
		state.screen_scissor_right = s[2]; state.screen_scissor_bottom = s[3];
	};
	for (uint32_t v = 0; v < 3; ++v)
		for (uint32_t g = 0; g < 3; ++g)
			for (uint32_t s = 0; s < 3; ++s)
			{
				if (v == g || v == s || g == s) continue;
				set_bounds(viewport, nested[v], nested[g], nested[s]);
				if (!SelectGraphicsScissor(viewport, mode, selected) || !matches(30, 40, 130, 240)) return false;
				vp_check(viewport, mode);
			}
	const int unsupported[][4] = {
	    {0, 100, 100, 300}, {140, 40, 240, 240}, // crossing and disjoint
	    {50, 0, 100, 300},                      // narrower X, wider Y
	    {-1, 0, 1920, 1080}, {0, -1, 1920, 1080},
	    {100, 0, 99, 1080}, {0, 100, 1920, 99},
	    {30, 40, 30, 240}, {30, 40, 130, 40},   // composition of empty rectangles
	};
	for (const auto& bounds: unsupported)
		for (uint32_t invalid = 0; invalid < 3; ++invalid)
		{
			set_bounds(viewport, invalid == 0 ? bounds : nested[2], invalid == 1 ? bounds : nested[2],
			           invalid == 2 ? bounds : nested[2]);
			if (SelectGraphicsScissor(viewport, mode, selected) || !matches(30, 40, 130, 240)) return false;
		}
	set_bounds(viewport, nested[2], nested[1], nested[0]);
	for (const bool generic_offset: {false, true})
	{
		viewport.generic_scissor_window_offset_enable = generic_offset;
		viewport.viewports[0].viewport_scissor_window_offset_enable = !generic_offset;
		if (SelectGraphicsScissor(viewport, mode, selected) || !matches(30, 40, 130, 240)) return false;
	}
	viewport.generic_scissor_window_offset_enable = false;
	viewport.screen_scissor_right = viewport.screen_scissor_bottom = 0;
	viewport.screen_scissor_left = viewport.screen_scissor_top = 0;
	viewport.generic_scissor_left = viewport.generic_scissor_top = 0;
	viewport.generic_scissor_right = viewport.generic_scissor_bottom = 0;
	viewport.viewports[0].viewport_scissor_left = 30;
	viewport.viewports[0].viewport_scissor_top = 40;
	viewport.viewports[0].viewport_scissor_right = 130;
	viewport.viewports[0].viewport_scissor_bottom = 240;
	if (!SelectGraphicsScissor(viewport, mode, selected) || !matches(30, 40, 130, 240)) return false;
	vp_check(viewport, mode);
	// The existing standalone route and the offset-disabled route both survive.
	viewport.viewports[0].viewport_scissor_window_offset_enable = true;
	if (!SelectGraphicsScissor(viewport, mode, selected) || !matches(30, 40, 130, 240)) return false;
	mode.vport_scissor_enable = false;
	viewport.screen_scissor_right = 1280;
	viewport.screen_scissor_bottom = 720;
	if (!SelectGraphicsScissor(viewport, mode, selected) || !matches(0, 0, 1280, 720)) return false;
	// All-zero viewport presence keeps its prior local policy in this change.
	viewport.viewports[0].viewport_scissor_left = viewport.viewports[0].viewport_scissor_top = 0;
	viewport.viewports[0].viewport_scissor_right = viewport.viewports[0].viewport_scissor_bottom = 0;
	mode.vport_scissor_enable = true;
	if (!SelectGraphicsScissor(viewport, mode, selected) || !matches(0, 0, 1280, 720)) return false;

	PipelineStaticParameters cached_static;
	PipelineDynamicParameters cached_dynamic;
	cached_static.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	cached_dynamic.vk_dynamic_state_scissor = true;
	VulkanPipeline pipeline;
	pipeline.static_params = &cached_static;
	pipeline.dynamic_params = &cached_dynamic;
	PipelineCache::Pipeline entry;
	entry.render_pass_id = 7;
	entry.vs_shader_id.ids.Add(11);
	entry.ps_shader_id.ids.Add(17);
	entry.pipeline = &pipeline;
	entry.static_params = &cached_static;
	entry.dynamic_params = &cached_dynamic;
	Vector<PipelineCache::Pipeline> entries;
	entries.Add(entry);
	auto requested_static = cached_static;
	auto requested_dynamic = cached_dynamic;
	auto requested = entry;
	requested.static_params = &requested_static;
	requested.dynamic_params = &requested_dynamic;
	VkRect2D snapshots[3] {};
	// 257 distinct scissors, then back to A, must all find the one real cache
	// entry and refresh its per-draw value. This exercises the production lookup
	// without creating Vulkan handles or evicting any in-flight object.
	for (int draw = 0; draw <= 512; ++draw)
	{
		const int y = draw <= 256 ? draw : 512 - draw;
		requested_dynamic.scissor_ltrb[0] = 10;
		requested_dynamic.scissor_ltrb[1] = y;
		requested_dynamic.scissor_ltrb[2] = 70;
		requested_dynamic.scissor_ltrb[3] = y + 20;
		auto* found = PipelineCache::FindIn(entries, requested);
		VkRect2D rectangle {};
		if (found != &pipeline || entries.Size() != 1 || !BuildScissorRect(found->dynamic_params->scissor_ltrb, &rectangle) ||
		    rectangle.offset.x != 10 || rectangle.offset.y != y || rectangle.extent.width != 60 || rectangle.extent.height != 20)
		{
			return false;
		}
		if (draw == 0) snapshots[0] = rectangle;
		if (draw == 256) snapshots[1] = rectangle;
		if (draw == 512) snapshots[2] = rectangle;
	}
	if (snapshots[0].offset.y != 0 || snapshots[1].offset.y != 256 || snapshots[2].offset.y != 0) return false;

	// A lookup key may live only until FindIn returns. Cache hits copy its
	// dynamic values into the existing object and never adopt either pointer.
	const auto saved_dynamic = cached_dynamic;
	const auto ephemeral_lookup = [&] {
		auto local_static = cached_static;
		auto local_dynamic = cached_dynamic;
		local_dynamic.scissor_ltrb[1] = 123;
		local_dynamic.scissor_ltrb[3] = 143;
		auto local = entry;
		local.static_params = &local_static;
		local.dynamic_params = &local_dynamic;
		auto* found = PipelineCache::FindIn(entries, local);
		if (found != &pipeline || found->static_params != &cached_static || found->dynamic_params != &cached_dynamic ||
		    entries[0].static_params != &cached_static || entries[0].dynamic_params != &cached_dynamic) return false;
		local_static.color_mask = UINT32_MAX;
		local_dynamic.scissor_ltrb[1] = -1;
		return cached_static.color_mask == 0 && cached_dynamic.scissor_ltrb[1] == 123;
	};
	VkRect2D after_lookup {};
	if (!ephemeral_lookup() || !BuildScissorRect(pipeline.dynamic_params->scissor_ltrb, &after_lookup) ||
	    after_lookup.offset.y != 123 || after_lookup.extent.height != 20) return false;
	cached_dynamic = saved_dynamic;
	for (int mutation = 0; mutation < 8; ++mutation)
	{
		requested_static = cached_static;
		switch (mutation)
		{
			case 0: requested_static.viewport_offset[0] = 1.0f; break;
			case 1: requested_static.viewport_scale[1] = -1.0f; break;
			case 2: requested_static.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
			case 3: requested_static.blend_color_red = 0.25f; break;
			case 4: requested_static.blend_enable = true; break;
			case 5: requested_static.depth_test_enable = true; break;
			case 6: requested_static.cull_front = true; break;
			case 7: requested_static.color_mask = 15; break;
		}
		if (PipelineCache::FindIn(entries, requested) != nullptr) return false;
	}
	requested_static = cached_static;
	requested.render_pass_id++;
	if (PipelineCache::FindIn(entries, requested) != nullptr) return false;
	requested.render_pass_id = entry.render_pass_id;
	requested.vs_shader_id.ids.Add(19);
	if (PipelineCache::FindIn(entries, requested) != nullptr) return false;
	requested.vs_shader_id = entry.vs_shader_id;
	requested_dynamic.vk_dynamic_state_scissor = false;
	if (PipelineCache::FindIn(entries, requested) != nullptr) return false;
	cached_dynamic.vk_dynamic_state_scissor = false;
	if (PipelineCache::FindIn(entries, requested) != &pipeline) return false;
	requested_dynamic.scissor_ltrb[1]++;
	if (PipelineCache::FindIn(entries, requested) != nullptr) return false;
	const PipelineDynamicParameters compute_defaults;
	if (compute_defaults.vk_dynamic_state_scissor) return false;
	struct GeometryCase { int ltrb[4]; bool supported; };
	const GeometryCase cases[] = {
	    {{10, 20, 40, 70}, true}, {{10, 20, 10, 70}, true}, {{10, 20, 40, 20}, true},
	    {{0, 0, INT32_MAX, INT32_MAX}, true}, {{INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX}, true},
	    {{-1, 0, 10, 10}, false}, {{0, -1, 10, 10}, false}, {{10, 0, 9, 10}, false},
	    {{0, 10, 10, 9}, false}, {{INT32_MIN, 0, INT32_MAX, 10}, false},
	    {{0, INT32_MIN, 10, INT32_MAX}, false}, {{INT32_MAX, 0, INT32_MIN, 10}, false},
	};
	for (const auto& geometry: cases)
	{
		VkRect2D rectangle {{123, 456}, {789, 1011}};
		if (BuildScissorRect(geometry.ltrb, &rectangle) != geometry.supported) return false;
		if (!geometry.supported && (rectangle.offset.x != 123 || rectangle.offset.y != 456 ||
		                           rectangle.extent.width != 789 || rectangle.extent.height != 1011)) return false;
	}
	return true;
}
#endif

void PipelineCache::DumpToFile(Core::File* f, const Pipeline& p) {}

void PipelineCache::DumpPipeline(const char* action, uint32_t id)
{
	EXIT_IF(!m_pipelines.IndexValid(id));
	EXIT_IF(action == nullptr);

	static std::atomic_int dump_id = 0;

	if (Config::PipelineDumpEnabled())
	{
		Core::File f;
		String     file_name = Config::GetPipelineDumpFolder().FixDirectorySlash() +
		                   String::FromPrintf("%04d_%04d_pipeline_%u_%s.log", GraphicsRunGetFrameNum(), dump_id++, id, action);
		Core::File::CreateDirectories(file_name.DirectoryWithoutFilename());
		f.Create(file_name);
		if (f.IsInvalid())
		{
			printf(FG_BRIGHT_RED "Can't create file: %s\n" FG_DEFAULT, file_name.C_Str());
			return;
		}
		Pipeline& p = m_pipelines[id];
		DumpToFile(&f, p);
		f.Close();
	}
}

static void create_layout(GraphicContext* gctx, int storage_buffers_num, int textures2d_sampled_num, int textures2d_storage_num,
                          int samplers_num, int gds_buffers_num, VkShaderStageFlags stage, VkDescriptorSetLayout* dst)
{
	uint32_t binding_num = 0;

	ShaderBindResources tmp {};
	tmp.storage_buffers.buffers_num = storage_buffers_num;
	tmp.textures2D.textures_num     = textures2d_sampled_num + textures2d_storage_num;
	tmp.samplers.samplers_num       = samplers_num;
	tmp.gds_pointers.pointers_num   = gds_buffers_num;

	ShaderCalcBindingIndices(&tmp);

	constexpr uint32_t B_MAX = 5;

	VkDescriptorSetLayoutBinding ubo_layout_binding[B_MAX] = {};

	if (storage_buffers_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.storage_buffers.binding_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		ubo_layout_binding[binding_num].descriptorCount    = storage_buffers_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (textures2d_sampled_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.textures2D.binding_sampled_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		ubo_layout_binding[binding_num].descriptorCount    = textures2d_sampled_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (textures2d_storage_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.textures2D.binding_storage_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		ubo_layout_binding[binding_num].descriptorCount    = textures2d_storage_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (samplers_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.samplers.binding_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLER;
		ubo_layout_binding[binding_num].descriptorCount    = samplers_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (gds_buffers_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.gds_pointers.binding_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		ubo_layout_binding[binding_num].descriptorCount    = gds_buffers_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (binding_num > 0)
	{
		VkDescriptorSetLayoutCreateInfo layout_info {};
		layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layout_info.pNext        = nullptr;
		layout_info.flags        = 0;
		layout_info.bindingCount = binding_num;
		layout_info.pBindings    = ubo_layout_binding;

		EXIT_IF(*dst != nullptr);

		vkCreateDescriptorSetLayout(gctx->device, &layout_info, nullptr, dst);

		EXIT_NOT_IMPLEMENTED(*dst == nullptr);
	} else
	{
		*dst = nullptr;
	}
};

void DescriptorCache::Init()
{
	if (m_initialized)
	{
		return;
	}

	auto* gctx = g_render_ctx->GetGraphicCtx();
	EXIT_IF(gctx == nullptr);

	EXIT_IF(m_descriptor_set_layout_vertex[0][0][0][0][0] != nullptr);
	EXIT_IF(m_descriptor_set_layout_pixel[0][0][0][0][0] != nullptr);
	EXIT_IF(m_descriptor_set_layout_compute[0][0][0][0][0] != nullptr);

	m_initialized = true;
}

VkDescriptorSetLayout DescriptorCache::GetOrCreateDescriptorSetLayout(Stage stage, int storage_buffers_num, int textures2d_sampled_num,
                                                                      int textures2d_storage_num, int samplers_num, int gds_buffers_num)
{
	Init();

	VkDescriptorSetLayout* layout = nullptr;
	VkShaderStageFlags     vk_stage {};

	switch (stage)
	{
		case Stage::Vertex:
			layout   = &m_descriptor_set_layout_vertex[storage_buffers_num][textures2d_sampled_num][textures2d_storage_num][samplers_num]
			                                           [gds_buffers_num];
			vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
			break;
		case Stage::Pixel:
			layout   = &m_descriptor_set_layout_pixel[storage_buffers_num][textures2d_sampled_num][textures2d_storage_num][samplers_num]
			                                          [gds_buffers_num];
			vk_stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			break;
		case Stage::Compute:
			layout   = &m_descriptor_set_layout_compute[storage_buffers_num][textures2d_sampled_num][textures2d_storage_num][samplers_num]
			                                            [gds_buffers_num];
			vk_stage = VK_SHADER_STAGE_COMPUTE_BIT;
			break;
		default: EXIT("unknown stage\n");
	}

	if (*layout == nullptr)
	{
		auto* gctx = g_render_ctx->GetGraphicCtx();
		EXIT_IF(gctx == nullptr);

		create_layout(gctx, storage_buffers_num, textures2d_sampled_num, textures2d_storage_num, samplers_num, gds_buffers_num, vk_stage, layout);
	}

	return *layout;
}

void DescriptorCache::CreatePool()
{
	LUMEN_PROFILER_BLOCK("DescriptorCache::CreatePool");

	auto* gctx = g_render_ctx->GetGraphicCtx();
	EXIT_IF(gctx == nullptr);

	static const uint32_t max_sets = 512;

	VkDescriptorPoolSize pool_size[4];
	pool_size[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	pool_size[0].descriptorCount = max_sets * (BUFFERS_MAX + GDS_BUFFER_MAX);
	pool_size[1].type            = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	pool_size[1].descriptorCount = max_sets * TEXTURES_SAMPLED_MAX;
	pool_size[2].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	pool_size[2].descriptorCount = max_sets * TEXTURES_STORAGE_MAX;
	pool_size[3].type            = VK_DESCRIPTOR_TYPE_SAMPLER;
	pool_size[3].descriptorCount = max_sets * SAMPLERS_MAX;

	VkDescriptorPoolCreateInfo pool_info {};
	pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.pNext         = nullptr;
	pool_info.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool_info.poolSizeCount = 4;
	pool_info.pPoolSizes    = pool_size;
	pool_info.maxSets       = max_sets;

	Pool pool {};

	vkCreateDescriptorPool(gctx->device, &pool_info, nullptr, &pool.pool);

	EXIT_NOT_IMPLEMENTED(pool.pool == nullptr);

	pool.free           = true;
	pool.next_free_pool = m_first_free_pool;
	m_first_free_pool   = static_cast<int>(m_pools.Size());

	m_pools.Add(pool);
}

VulkanDescriptorSet* DescriptorCache::Allocate(Stage stage, int storage_buffers_num, int textures2d_sampled_num, int textures2d_storage_num,
                                               int samplers_num, int gds_buffers_num)
{
	LUMEN_PROFILER_BLOCK("DescriptorCache::Allocate");

	EXIT_IF(storage_buffers_num < 0 || storage_buffers_num > BUFFERS_MAX);
	EXIT_IF(textures2d_sampled_num < 0 || textures2d_sampled_num > TEXTURES_SAMPLED_MAX);
	EXIT_IF(textures2d_storage_num < 0 || textures2d_storage_num > TEXTURES_STORAGE_MAX);
	EXIT_IF(samplers_num < 0 || samplers_num > SAMPLERS_MAX);
	EXIT_IF(gds_buffers_num < 0 || gds_buffers_num > GDS_BUFFER_MAX);

	Core::LockGuard lock(m_mutex);

	auto* gctx = g_render_ctx->GetGraphicCtx();
	EXIT_IF(gctx == nullptr);

	auto descriptor_set_layout =
	    GetOrCreateDescriptorSetLayout(stage, storage_buffers_num, textures2d_sampled_num, textures2d_storage_num, samplers_num, gds_buffers_num);
	EXIT_IF(descriptor_set_layout == nullptr);

	auto* ret = new VulkanDescriptorSet;

	ret->set = nullptr;
	ret->layout = descriptor_set_layout;

	for (int try_num = 0; try_num < 2; try_num++)
	{
		int pool_id = m_first_free_pool;
		while (pool_id != -1)
		{
			auto& pool = m_pools[pool_id];

			VkDescriptorSetAllocateInfo alloc_info {};
			alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			alloc_info.pNext              = nullptr;
			alloc_info.descriptorPool     = pool.pool;
			alloc_info.descriptorSetCount = 1;
			alloc_info.pSetLayouts        = &descriptor_set_layout;

			ret->pool_id = pool_id;

			EXIT_IF(!pool.free);

			{
				LUMEN_PROFILER_BLOCK("vkAllocateDescriptorSets");

				auto result = vkAllocateDescriptorSets(gctx->device, &alloc_info, &ret->set);

				if (result == VK_SUCCESS)
				{
					return ret;
				}
			}

			pool.free         = false;
			m_first_free_pool = pool.next_free_pool;
			pool_id           = pool.next_free_pool;
		}

		CreatePool();
	}

	delete ret;
	return nullptr;
}

void DescriptorCache::Free(VulkanDescriptorSet* set)
{
	EXIT_IF(set == nullptr);

	Core::LockGuard lock(m_mutex);

	auto* gctx = g_render_ctx->GetGraphicCtx();
	EXIT_IF(gctx == nullptr);
	EXIT_IF(!m_pools.IndexValid(set->pool_id));

	auto& pool = m_pools[set->pool_id];

	vkFreeDescriptorSets(gctx->device, pool.pool, 1, &set->set);
	MarkPoolAvailable(pool, set->pool_id, m_first_free_pool);

	delete set;
}

void DescriptorCache::ResetForNextSession()
{
	Core::LockGuard lock(m_mutex);
	auto*           gctx = g_render_ctx != nullptr ? g_render_ctx->GetGraphicCtx() : nullptr;
	EXIT_IF(gctx == nullptr || gctx->device == nullptr);
	// Every recording must have completed or been discarded and drained before
	// pool destruction. Device idle alone does not discard unsubmitted commands.
	EXIT_IF(!m_sets.Clear());

	// Destroying a descriptor pool releases all sets allocated from it. Keep
	// the immutable layouts because the VkDevice is process-lifetime on iOS.
	for (auto& pool: m_pools)
	{
		if (pool.pool != nullptr)
		{
			vkDestroyDescriptorPool(gctx->device, pool.pool, nullptr);
			pool.pool = nullptr;
		}
	}
	m_pools.Clear();
	m_first_free_pool = -1;
	for (const auto& pool: m_free_constant_pools) vkDestroyDescriptorPool(gctx->device, pool.pool, nullptr);
	for (const auto& arena: m_free_constant_buffers)
	{
		VulkanUnmapMemory(gctx, &arena.buffer->memory);
		VulkanDeleteBuffer(gctx, arena.buffer);
		delete arena.buffer;
	}
	m_free_constant_pools.clear();
	m_free_constant_buffers.clear();
}

DescriptorRecordingBatch* DescriptorCache::CreateBatch(CommandBuffer* command)
{
	EXIT_IF(command == nullptr);
	Core::LockGuard lock(m_mutex);
	auto* batch = new DescriptorRecordingBatch;
	batch->cache = this;
	batch->command = command;
	m_sets.BeginBatch(batch);
	return batch;
}

void DescriptorCache::ReleaseBatch(DescriptorRecordingBatch* batch)
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(batch == nullptr || batch->cache != this);
	m_sets.ReleaseBatch(batch, [this](VulkanDescriptorSet* set) { Free(set); });
	// This callback has the same completion/discard gate as ordinary sets.
	// No constant buffer or descriptor pool is reused by another recording.
	auto* ctx = g_render_ctx->GetGraphicCtx();
	constexpr size_t CACHED_CONSTANT_RESOURCES_MAX = 32;
	ReleaseConstantResources(batch, [&](auto& pool) {
		EXIT_NOT_IMPLEMENTED(vkFreeDescriptorSets(ctx->device, pool.pool, static_cast<uint32_t>(pool.sets.size()),
		                                       pool.sets.data()) != VK_SUCCESS);
		pool.sets.clear();
		if (m_free_constant_pools.size() < CACHED_CONSTANT_RESOURCES_MAX)
			m_free_constant_pools.push_back(std::move(pool));
		else vkDestroyDescriptorPool(ctx->device, pool.pool, nullptr);
	}, [&](auto& arena) {
		arena.used = 0;
		if (m_free_constant_buffers.size() < CACHED_CONSTANT_RESOURCES_MAX)
			m_free_constant_buffers.push_back(arena);
		else
		{
			VulkanUnmapMemory(ctx, &arena.buffer->memory);
			VulkanDeleteBuffer(ctx, arena.buffer);
			delete arena.buffer;
		}
	});
	// Logarithmically bounded receipts show continued reuse during long runs.
	const uint64_t completed = m_sets.completed_batches;
	if ((completed & (completed - 1)) == 0)
	{
		std::fprintf(stderr,
		             "[GFX_DESCRIPTOR_RECORDING] completed_batches=%" PRIu64 " active_sets=%u peak_active_sets=%u "
		             "high_water_slots=%u capacity_slots=%u active_batches=%u released_total=%" PRIu64
		             " descriptor_bytes_per_slot=%zu\n",
		             completed, m_sets.active_sets, m_sets.peak_active_sets, m_sets.HistoricalSlots(), m_sets.Capacity(),
		             m_sets.active_batches, m_sets.released_total, sizeof(Set));
	}
	delete batch;
}

uint32_t DescriptorCache::CalcHash(const Set& s)
{
	uint32_t hash = Core::hash64(reinterpret_cast<uintptr_t>(s.owner));
	hash += Core::hash8(static_cast<uint8_t>(s.stage));
	hash ^= Core::hash8(static_cast<uint8_t>(s.storage_buffers_num));
	hash += Core::hash8(static_cast<uint8_t>(s.textures2d_sampled_num));
	hash ^= Core::hash8(static_cast<uint8_t>(s.textures2d_storage_num));
	hash += Core::hash8(static_cast<uint8_t>(s.samplers_num));
	hash ^= Core::hash8(static_cast<uint8_t>(s.gds_buffers_num));
	for (int i = 0; i < s.storage_buffers_num; i++)
	{
		hash += Core::hash64(s.storage_buffers_id[i]);
	}
	for (int i = 0; i < s.textures2d_sampled_num; i++)
	{
		hash ^= Core::hash64(s.textures2d_sampled_id[i]);
		hash += Core::hash32(static_cast<uint32_t>(s.textures2d_sampled_view[i]));
	}
	for (int i = 0; i < s.textures2d_storage_num; i++)
	{
		hash += Core::hash64(s.textures2d_storage_id[i]);
	}
	for (int i = 0; i < s.samplers_num; i++)
	{
		hash ^= Core::hash64(s.samplers_id[i]);
	}
	for (int i = 0; i < s.gds_buffers_num; i++)
	{
		hash += Core::hash64(s.gds_buffers_id[i]);
	}
	return hash;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
VulkanDescriptorSet* DescriptorCache::SetTable::FindSet(const Set& s) const
{
	const auto* list = m_sets_map.Find(s.hash);
	if (list != nullptr)
	{
		for (int index: *list)
		{
			const auto& set = m_sets.At(index);

			if (set.cached && set.set != nullptr && set.owner == s.owner && set.stage == s.stage &&
			    set.storage_buffers_num == s.storage_buffers_num &&
			    set.textures2d_sampled_num == s.textures2d_sampled_num && set.textures2d_storage_num == s.textures2d_storage_num &&
			    set.samplers_num == s.samplers_num && set.gds_buffers_num == s.gds_buffers_num)
			{
				bool match = true;
				for (int i = 0; i < s.storage_buffers_num; i++)
				{
					if (match && s.storage_buffers_id[i] != set.storage_buffers_id[i])
					{
						match = false;
						break;
					}
				}
				if (match)
				{
					for (int i = 0; i < s.textures2d_sampled_num; i++)
					{
						if (s.textures2d_sampled_id[i] != set.textures2d_sampled_id[i] ||
						    s.textures2d_sampled_view[i] != set.textures2d_sampled_view[i])
						{
							match = false;
							break;
						}
					}
				}
				if (match)
				{
					for (int i = 0; i < s.textures2d_storage_num; i++)
					{
						if (s.textures2d_storage_id[i] != set.textures2d_storage_id[i])
						{
							match = false;
							break;
						}
					}
				}
				if (match)
				{
					for (int i = 0; i < s.samplers_num; i++)
					{
						if (s.samplers_id[i] != set.samplers_id[i])
						{
							match = false;
							break;
						}
					}
				}
				if (match)
				{
					for (int i = 0; i < s.gds_buffers_num; i++)
					{
						if (s.gds_buffers_id[i] != set.gds_buffers_id[i])
						{
							match = false;
							break;
						}
					}
				}
				if (match)
				{
					return set.set;
				}
			}
		}
	}
	return nullptr;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
VulkanDescriptorSet* DescriptorCache::GetDescriptor(DescriptorRecordingBatch* batch, Stage stage, VulkanBuffer** storage_buffers,
                                                    VulkanImage** textures2d_sampled,
                                                    const int* textures2d_sampled_view, VulkanImage** textures2d_storage,
                                                    uint64_t* samplers, VulkanBuffer** gds_buffers, const ShaderBindResources& bind)
{
	LUMEN_PROFILER_BLOCK("DescriptorCache::GetDescriptor::search");

	int storage_buffers_num    = bind.storage_buffers.buffers_num;
	int textures2d_sampled_num = bind.textures2D.textures2d_sampled_num;
	int textures2d_storage_num = bind.textures2D.textures2d_storage_num;
	int samplers_num           = bind.samplers.samplers_num;
	int gds_buffers_num        = bind.gds_pointers.pointers_num;

	EXIT_IF(storage_buffers_num < 0 || storage_buffers_num > BUFFERS_MAX);
	EXIT_IF(textures2d_sampled_num < 0 || textures2d_sampled_num > TEXTURES_SAMPLED_MAX);
	EXIT_IF(textures2d_storage_num < 0 || textures2d_storage_num > TEXTURES_STORAGE_MAX);
	EXIT_IF(samplers_num < 0 || samplers_num > SAMPLERS_MAX);
	EXIT_IF(storage_buffers == nullptr);

	Core::LockGuard lock(m_mutex);
	EXIT_IF(batch == nullptr || batch->cache != this || !batch->active);

	auto* gctx = g_render_ctx->GetGraphicCtx();
	EXIT_IF(gctx == nullptr);

	Set nset;
	nset.owner                  = batch;
	nset.set                    = nullptr;
	nset.storage_buffers_num    = storage_buffers_num;
	nset.textures2d_sampled_num = textures2d_sampled_num;
	nset.textures2d_storage_num = textures2d_storage_num;
	nset.samplers_num           = samplers_num;
	nset.gds_buffers_num        = gds_buffers_num;
	nset.stage                  = stage;
	for (int i = 0; i < storage_buffers_num; i++)
	{
		EXIT_IF(storage_buffers[i] == nullptr || (storage_buffers[i]->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) == 0);
		nset.storage_buffers_id[i] = storage_buffers[i]->memory.unique_id;
	}
	for (int i = 0; i < textures2d_sampled_num; i++)
	{
		nset.textures2d_sampled_id[i]   = textures2d_sampled[i]->memory.unique_id;
		nset.textures2d_sampled_view[i] = textures2d_sampled_view[i];
	}
	for (int i = 0; i < textures2d_storage_num; i++)
	{
		nset.textures2d_storage_id[i] = textures2d_storage[i]->memory.unique_id;
	}
	for (int i = 0; i < samplers_num; i++)
	{
		nset.samplers_id[i] = samplers[i];
	}
	for (int i = 0; i < gds_buffers_num; i++)
	{
		EXIT_IF(gds_buffers[i] == nullptr || (gds_buffers[i]->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) == 0);
		nset.gds_buffers_id[i] = gds_buffers[i]->memory.unique_id;
	}
	nset.hash = CalcHash(nset);

	if (auto* f = m_sets.FindSet(nset); f != nullptr)
	{
		return f;
	}

	LUMEN_PROFILER_END_BLOCK;

	LUMEN_PROFILER_BLOCK("DescriptorCache::GetDescriptor::create");

	auto* new_set = Allocate(stage, storage_buffers_num, textures2d_sampled_num, textures2d_storage_num, samplers_num, gds_buffers_num);
	EXIT_NOT_IMPLEMENTED(new_set == nullptr);

	VkDescriptorBufferInfo buffer_info[BUFFERS_MAX] {};
	for (int i = 0; i < storage_buffers_num; i++)
	{
		buffer_info[i].buffer = storage_buffers[i]->buffer;
		buffer_info[i].offset = 0;
		buffer_info[i].range  = VK_WHOLE_SIZE;
	}

	VkDescriptorImageInfo texture2d_sampled_info[TEXTURES_SAMPLED_MAX] {};
	for (int i = 0; i < textures2d_sampled_num; i++)
	{
		texture2d_sampled_info[i].sampler   = nullptr;
		texture2d_sampled_info[i].imageView = textures2d_sampled[i]->image_view[textures2d_sampled_view[i]];
		texture2d_sampled_info[i].imageLayout =
		    (textures2d_sampled[i]->type == VulkanImageType::DepthStencil ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
		     : textures2d_sampled[i]->type == VulkanImageType::StorageTexture ? VK_IMAGE_LAYOUT_GENERAL
		                                                                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}

	VkDescriptorImageInfo texture2d_storage_info[TEXTURES_STORAGE_MAX] {};
	for (int i = 0; i < textures2d_storage_num; i++)
	{
		texture2d_storage_info[i].sampler     = nullptr;
		texture2d_storage_info[i].imageView   = textures2d_storage[i]->image_view[VulkanImage::VIEW_DEFAULT];
		texture2d_storage_info[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	}

	VkDescriptorImageInfo sampler_info[SAMPLERS_MAX] {};
	for (int i = 0; i < samplers_num; i++)
	{
		sampler_info[i].sampler     = g_render_ctx->GetSamplerCache()->GetSampler(samplers[i]);
		sampler_info[i].imageView   = nullptr;
		sampler_info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	VkDescriptorBufferInfo gds_buffer_info[GDS_BUFFER_MAX] {};
	for (int i = 0; i < gds_buffers_num; i++)
	{
		gds_buffer_info[i].buffer = gds_buffers[i]->buffer;
		gds_buffer_info[i].offset = 0;
		gds_buffer_info[i].range  = VK_WHOLE_SIZE;
	}

	uint32_t binding_num = 0;

	constexpr uint32_t B_MAX = 5;

	VkWriteDescriptorSet descriptor_write[B_MAX] = {};

	if (storage_buffers_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.storage_buffers.binding_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		descriptor_write[binding_num].descriptorCount  = storage_buffers_num;
		descriptor_write[binding_num].pBufferInfo      = buffer_info;
		descriptor_write[binding_num].pImageInfo       = nullptr;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	if (textures2d_sampled_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.textures2D.binding_sampled_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		descriptor_write[binding_num].descriptorCount  = textures2d_sampled_num;
		descriptor_write[binding_num].pBufferInfo      = nullptr;
		descriptor_write[binding_num].pImageInfo       = texture2d_sampled_info;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	if (textures2d_storage_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.textures2D.binding_storage_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		descriptor_write[binding_num].descriptorCount  = textures2d_storage_num;
		descriptor_write[binding_num].pBufferInfo      = nullptr;
		descriptor_write[binding_num].pImageInfo       = texture2d_storage_info;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	if (samplers_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.samplers.binding_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLER;
		descriptor_write[binding_num].descriptorCount  = samplers_num;
		descriptor_write[binding_num].pBufferInfo      = nullptr;
		descriptor_write[binding_num].pImageInfo       = sampler_info;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	if (gds_buffers_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.gds_pointers.binding_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		descriptor_write[binding_num].descriptorCount  = gds_buffers_num;
		descriptor_write[binding_num].pBufferInfo      = gds_buffer_info;
		descriptor_write[binding_num].pImageInfo       = nullptr;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	vkUpdateDescriptorSets(gctx->device, binding_num, descriptor_write, 0, nullptr);

	nset.set = new_set;
	m_sets.Add(nset);
	return new_set;
}

void DescriptorCache::SetTable::InvalidateBuffer(uint64_t id)
{
	for (uint32_t index = 0; index < m_sets.Size(); ++index)
	{
		const auto& set = m_sets.At(index);
		if (!set.cached) { continue; }
		for (int i = 0; i < set.storage_buffers_num; ++i)
		{
			if (set.storage_buffers_id[i] == id)
			{
				InvalidateSet(static_cast<int>(index));
				break;
			}
		}
	}
}

void DescriptorCache::SetTable::InvalidateImage(uint64_t id)
{
	for (uint32_t index = 0; index < m_sets.Size(); ++index)
	{
		const auto& set = m_sets.At(index);
		if (!set.cached) { continue; }
		bool references_image = false;
		for (int i = 0; i < set.textures2d_sampled_num; ++i)
		{
			if (set.textures2d_sampled_id[i] == id) { references_image = true; break; }
		}
		for (int i = 0; !references_image && i < set.textures2d_storage_num; ++i)
		{
			if (set.textures2d_storage_id[i] == id) { references_image = true; }
		}
		if (references_image) { InvalidateSet(static_cast<int>(index)); }
	}
}

void DescriptorCache::FreeDescriptor(VulkanBuffer* buffer)
{
	LUMEN_PROFILER_FUNCTION();
	EXIT_IF(buffer == nullptr);
	// Keep the established storage-buffer filter, including its GDS exclusion.
	if ((buffer->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) == 0) { return; }
	Core::LockGuard lock(m_mutex);
	m_sets.InvalidateBuffer(buffer->memory.unique_id);
}

void DescriptorCache::FreeDescriptor(VulkanImage* image)
{
	LUMEN_PROFILER_FUNCTION();
	EXIT_IF(image == nullptr);
	Core::LockGuard lock(m_mutex);
	m_sets.InvalidateImage(image->memory.unique_id);
}

VkDescriptorSetLayout DescriptorCache::GetDescriptorSetLayout(Stage stage, const ShaderBindResources& bind)
{
	int storage_buffers_num    = bind.storage_buffers.buffers_num;
	int textures2d_sampled_num = bind.textures2D.textures2d_sampled_num;
	int textures2d_storage_num = bind.textures2D.textures2d_storage_num;
	int samplers_num           = bind.samplers.samplers_num;
	int gds_buffers_num        = bind.gds_pointers.pointers_num;

	EXIT_IF(storage_buffers_num < 0 || storage_buffers_num > BUFFERS_MAX);
	EXIT_IF(textures2d_sampled_num < 0 || textures2d_sampled_num > TEXTURES_SAMPLED_MAX);
	EXIT_IF(textures2d_storage_num < 0 || textures2d_storage_num > TEXTURES_STORAGE_MAX);
	EXIT_IF(samplers_num < 0 || samplers_num > SAMPLERS_MAX);
	EXIT_IF(gds_buffers_num < 0 || gds_buffers_num > GDS_BUFFER_MAX);

	// Vertex shader admission validates read-only image operations before the
	// pipeline cache lookup. Its descriptor layout can use the same sampled
	// image and sampler bindings as the other stages, but not writable images.
	EXIT_NOT_IMPLEMENTED(stage == Stage::Vertex && (textures2d_storage_num > 0 || gds_buffers_num > 0));
	EXIT_NOT_IMPLEMENTED(stage != Stage::Vertex && stage != Stage::Pixel && stage != Stage::Compute);

	Core::LockGuard lock(m_mutex);
	return GetOrCreateDescriptorSetLayout(stage, storage_buffers_num, textures2d_sampled_num, textures2d_storage_num, samplers_num,
	                                      gds_buffers_num);
}

VkDescriptorSetLayout DescriptorCache::GetEmptyDescriptorSetLayout()
{
	Core::LockGuard lock(m_mutex);
	if (m_empty_layout == nullptr)
	{
		VkDescriptorSetLayoutCreateInfo info {};
		info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		EXIT_NOT_IMPLEMENTED(vkCreateDescriptorSetLayout(g_render_ctx->GetGraphicCtx()->device, &info, nullptr,
		                                               &m_empty_layout) != VK_SUCCESS);
	}
	return m_empty_layout;
}

VkDescriptorSetLayout DescriptorCache::GetConstantDescriptorSetLayout(VkShaderStageFlags stages)
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(stages != VK_SHADER_STAGE_COMPUTE_BIT && stages != (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT));
	auto& layout = stages == VK_SHADER_STAGE_COMPUTE_BIT ? m_constant_compute_layout : m_constant_graphics_layout;
	if (layout == nullptr)
	{
		VkDescriptorSetLayoutBinding binding {};
		binding.binding = ShaderBindResources::UNIFORM_BINDING;
		binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		binding.descriptorCount = 1;
		binding.stageFlags = stages;
		VkDescriptorSetLayoutCreateInfo info {};
		info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		info.bindingCount = 1;
		info.pBindings = &binding;
		EXIT_NOT_IMPLEMENTED(vkCreateDescriptorSetLayout(g_render_ctx->GetGraphicCtx()->device, &info, nullptr, &layout) != VK_SUCCESS);
	}
	return layout;
}

VkDescriptorSet DescriptorCache::GetConstantDescriptor(DescriptorRecordingBatch* batch, VkShaderStageFlags stages,
                                                       const std::vector<uint32_t>& words, VkDescriptorBufferInfo* captured)
{
	const auto layout = GetConstantDescriptorSetLayout(stages);
	Core::LockGuard lock(m_mutex);
	EXIT_IF(batch == nullptr || batch->cache != this || !batch->active || words.empty());
	auto* ctx = g_render_ctx->GetGraphicCtx();
	const uint64_t bytes = words.size() * sizeof(uint32_t);
	EXIT_IF(bytes > g_render_ctx->GetDeviceLimits().maxUniformBufferRange);
	const auto alignment = std::max<VkDeviceSize>(16, g_render_ctx->GetDeviceLimits().minUniformBufferOffsetAlignment);
	VkDeviceSize offset = 0;
	if (batch->constant_buffers.empty() || !ConstantArenaOffset(batch->constant_buffers.back().used, bytes, alignment, &offset))
	{
		DescriptorRecordingBatch::ConstantBuffer arena;
		if (!m_free_constant_buffers.empty())
		{
			arena = m_free_constant_buffers.back();
			m_free_constant_buffers.pop_back();
		} else
		{
			arena.buffer = new VulkanBuffer;
			arena.buffer->usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
			arena.buffer->memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
			VulkanCreateBuffer(ctx, CONSTANT_ARENA_BYTES, arena.buffer);
			EXIT_NOT_IMPLEMENTED(arena.buffer->buffer == nullptr);
			VulkanMapMemory(ctx, &arena.buffer->memory, &arena.mapped);
			EXIT_IF(arena.mapped == nullptr);
		}
		batch->constant_buffers.push_back(arena);
	}
	auto& arena = batch->constant_buffers.back();
	EXIT_IF(!ConstantArenaOffset(arena.used, bytes, alignment, &offset));
	std::memcpy(static_cast<uint8_t*>(arena.mapped) + offset, words.data(), bytes);
	arena.used = offset + bytes;

	constexpr uint32_t SETS_PER_POOL = 128;
	if (batch->constant_pools.empty() || batch->constant_pools.back().sets.size() == SETS_PER_POOL)
	{
		DescriptorRecordingBatch::ConstantPool pool;
		if (!m_free_constant_pools.empty())
		{
			pool = std::move(m_free_constant_pools.back());
			m_free_constant_pools.pop_back();
		} else
		{
			VkDescriptorPoolSize size {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SETS_PER_POOL};
			VkDescriptorPoolCreateInfo info {};
			info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
			info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
			info.maxSets = SETS_PER_POOL;
			info.poolSizeCount = 1;
			info.pPoolSizes = &size;
			EXIT_NOT_IMPLEMENTED(vkCreateDescriptorPool(ctx->device, &info, nullptr, &pool.pool) != VK_SUCCESS);
			pool.sets.reserve(SETS_PER_POOL);
		}
		batch->constant_pools.push_back(std::move(pool));
	}
	auto& pool = batch->constant_pools.back();
	VkDescriptorSetAllocateInfo allocate {};
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = pool.pool;
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts = &layout;
	VkDescriptorSet set = nullptr;
	EXIT_NOT_IMPLEMENTED(vkAllocateDescriptorSets(ctx->device, &allocate, &set) != VK_SUCCESS || set == nullptr);
	pool.sets.push_back(set);
	VkDescriptorBufferInfo buffer_info {arena.buffer->buffer, offset, bytes};
	if (captured != nullptr) *captured = buffer_info;
	VkWriteDescriptorSet write {};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = set;
	write.dstBinding = ShaderBindResources::UNIFORM_BINDING;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	write.pBufferInfo = &buffer_info;
	vkUpdateDescriptorSets(ctx->device, 1, &write, 0, nullptr);
	return set;
}

void DeleteFramebuffer(VideoOutVulkanImage* image)
{
	g_render_ctx->GetFramebufferCache()->FreeFramebufferByColor(image);
}

void DeleteFramebuffer(RenderTextureVulkanImage* image)
{
	g_render_ctx->GetFramebufferCache()->FreeFramebufferByColor(image);
}

void DeleteFramebuffer(DepthStencilVulkanImage* image)
{
	g_render_ctx->GetFramebufferCache()->FreeFramebufferByDepth(image);
}

void DeleteDescriptor(VulkanBuffer* buffer)
{
	g_render_ctx->GetDescriptorCache()->FreeDescriptor(buffer);
}

void DeleteDescriptor(VideoOutVulkanImage* image)
{
	g_render_ctx->GetDescriptorCache()->FreeDescriptor(image);
}

void DeleteDescriptor(TextureVulkanImage* image)
{
	g_render_ctx->GetDescriptorCache()->FreeDescriptor(image);
}

void DeleteDescriptor(StorageTextureVulkanImage* image)
{
	g_render_ctx->GetDescriptorCache()->FreeDescriptor(image);
}

void DeleteDescriptor(RenderTextureVulkanImage* image)
{
	g_render_ctx->GetDescriptorCache()->FreeDescriptor(image);
}

static bool get_stencil_op(VkStencilOp* f, uint32_t* ref, uint8_t op, uint8_t testval, uint8_t opval)
{
	VkStencilOp vk      = VK_STENCIL_OP_KEEP;
	uint32_t    r       = opval;
	bool        use_ref = true;

	switch (op)
	{
		case 0x00:
			vk      = VK_STENCIL_OP_KEEP;
			use_ref = false;
			break; /* Keep           */
		case 0x01:
			vk      = VK_STENCIL_OP_ZERO;
			use_ref = false;
			break; /* Zero           */
		case 0x02:
			vk = VK_STENCIL_OP_REPLACE;
			r  = 0xFF;
			break; /* Ones           */
		case 0x03:
			vk = VK_STENCIL_OP_REPLACE;
			r  = testval;
			break;                                                /* ReplaceTest    */
		case 0x04: vk = VK_STENCIL_OP_REPLACE; break;             /* ReplaceOp      */
		case 0x05: vk = VK_STENCIL_OP_INCREMENT_AND_CLAMP; break; /* AddClamp       */
		case 0x06: vk = VK_STENCIL_OP_DECREMENT_AND_CLAMP; break; /* SubClamp       */
		case 0x07: vk = VK_STENCIL_OP_INVERT; break;              /* Invert         */
		case 0x08: vk = VK_STENCIL_OP_INCREMENT_AND_WRAP; break;  /* AddWrap        */
		case 0x09: vk = VK_STENCIL_OP_DECREMENT_AND_WRAP; break;  /* SubWrap        */
		case 0x0a:                                                /* And            */
		case 0x0b:                                                /* Or             */
		case 0x0c:                                                /* Xor            */
		case 0x0d:                                                /* Nand           */
		case 0x0e:                                                /* Nor            */
		case 0x0f:                                                /* Xnor           */
		default: EXIT("invalid op");
	}
	*f   = vk;
	*ref = r;
	return use_ref;
}

static void get_stencil_state(PipelineStencilStaticState* s, PipelineStencilDynamicState* d, uint8_t func, uint8_t fail, uint8_t zpass,
                              uint8_t zfail, uint8_t testval, uint8_t mask, uint8_t writemask, uint8_t opval)
{
	EXIT_IF(s == nullptr || d == nullptr);

	uint32_t ref[3]     = {};
	bool     use_ref[3] = {};

	use_ref[0]     = get_stencil_op(&s->failOp, ref + 0, fail, testval, opval);
	use_ref[1]     = get_stencil_op(&s->passOp, ref + 1, zpass, testval, opval);
	use_ref[2]     = get_stencil_op(&s->depthFailOp, ref + 2, zfail, testval, opval);
	s->compareOp   = static_cast<VkCompareOp>(func);
	d->compareMask = mask;
	d->writeMask   = writemask;

	if (use_ref[0])
	{
		EXIT_NOT_IMPLEMENTED((ref[0] != ref[1] && use_ref[1]) || (ref[0] != ref[2] && use_ref[2]));
		d->reference = ref[0];
	} else if (use_ref[1])
	{
		EXIT_NOT_IMPLEMENTED(ref[1] != ref[2] && use_ref[2]);
		d->reference = ref[1];
	} else if (use_ref[2])
	{
		d->reference = ref[2];
	} else
	{
		d->reference = 0;
	}
}

static bool ResolveRenderStencilState(const HW::DepthControl& control, const HW::StencilControl& stencil,
                                     const HW::StencilMask& mask, RenderDepthInfo* depth)
{
	if (depth == nullptr || !IsRenderStencilFaceStateSupported(control, stencil, mask)) return false;
	PipelineStencilStaticState front, back;
	PipelineStencilDynamicState dynamic_front, dynamic_back;
	if (control.stencil_enable)
	{
		get_stencil_state(&front, &dynamic_front, control.stencilfunc, stencil.stencil_fail, stencil.stencil_zpass,
		                  stencil.stencil_zfail, mask.stencil_testval, mask.stencil_mask, mask.stencil_writemask, mask.stencil_opval);
		if (control.backface_enable)
		{
			get_stencil_state(&back, &dynamic_back, control.stencilfunc_bf, stencil.stencil_fail_bf, stencil.stencil_zpass_bf,
			                  stencil.stencil_zfail_bf, mask.stencil_testval_bf, mask.stencil_mask_bf, mask.stencil_writemask_bf,
			                  mask.stencil_opval_bf);
		} else
		{
			back = front;
			dynamic_back = dynamic_front;
		}
	}
	depth->stencil_test_enable = control.stencil_enable;
	depth->stencil_static_front = front;
	depth->stencil_static_back = back;
	depth->stencil_dynamic_front = dynamic_front;
	depth->stencil_dynamic_back = dynamic_back;
	return true;
}

static Vector<RenderTextureVulkanImage*> FindRenderTexture(uint64_t vaddr, uint64_t size, bool exact, const char* diagnostic_caller)
{
	LUMEN_PROFILER_FUNCTION();

	Vector<RenderTextureVulkanImage*> ret;

	auto objects = GpuMemoryFindObjects(vaddr, size, GpuMemoryObjectType::RenderTexture, exact, false, diagnostic_caller);

	for (const auto& obj: objects)
	{
		if (obj.type == GpuMemoryObjectType::RenderTexture)
		{
			ret.Add(static_cast<RenderTextureVulkanImage*>(obj.obj));
		}
	}

	return ret;
}

static Vector<DepthStencilVulkanImage*> FindDepthStencil(uint64_t vaddr, uint64_t size, bool exact, const char* diagnostic_caller)
{
	LUMEN_PROFILER_FUNCTION();

	Vector<DepthStencilVulkanImage*> ret;

	auto objects = GpuMemoryFindObjects(vaddr, size, GpuMemoryObjectType::DepthStencilBuffer, exact, true, diagnostic_caller);

	for (const auto& obj: objects)
	{
		if (obj.type == GpuMemoryObjectType::DepthStencilBuffer)
		{
			ret.Add(static_cast<DepthStencilVulkanImage*>(obj.obj));
		}
	}

	return ret;
}

static void RecordGlobalMemoryBarrier(CommandBuffer* buffer, VkPipelineStageFlags stages, VkAccessFlags destination_access)
{
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	auto* vk_buffer = buffer->GetBufferOutsideRenderPass();

	VkMemoryBarrier mem_barrier {};
	mem_barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	mem_barrier.pNext         = nullptr;
	mem_barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	mem_barrier.dstAccessMask = destination_access;

	vkCmdPipelineBarrier(vk_buffer, stages, stages, 0, 1, &mem_barrier, 0, nullptr, 0, nullptr);
}

void GraphicsRenderMemoryBarrier(CommandBuffer* buffer)
{
	RecordGlobalMemoryBarrier(buffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                          VK_ACCESS_MEMORY_READ_BIT);
}

void GraphicsRenderFullMemoryBarrier(CommandBuffer* buffer)
{
	RecordGlobalMemoryBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}

GpuMemoryDbCacheProofResult GraphicsRenderDbCacheAlreadySynchronizedBarrier(
    CommandBuffer* buffer, bool record, GpuMemoryDbCachePreflightTicket* ticket)
{
	// CP -> renderer -> tracker is the same order as the protected depth
	// transition. Never acquire renderer for the first time under tracker.
	Core::LockGuard lock(g_render_ctx->GetMutex());
	return GpuMemoryProveAlreadySynchronizedDepth(buffer, record ? GraphicsRenderFullMemoryBarrier : nullptr, ticket);
}

struct ComputeImageStoreBarrierPolicy
{
	VkPipelineStageFlags src_stage_mask;
	VkPipelineStageFlags dst_stage_mask;
	VkAccessFlags        src_access_mask;
	VkAccessFlags        dst_access_mask;
};

static constexpr ComputeImageStoreBarrierPolicy GetComputeImageStoreBarrierPolicy()
{
	return {
	    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	    VK_ACCESS_SHADER_WRITE_BIT,
	    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
	};
}

static void GraphicsRenderComputeImageStoreBarrier(VkCommandBuffer buffer)
{
	const auto policy = GetComputeImageStoreBarrierPolicy();

	VkMemoryBarrier mem_barrier {};
	mem_barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	mem_barrier.pNext         = nullptr;
	mem_barrier.srcAccessMask = policy.src_access_mask;
	mem_barrier.dstAccessMask = policy.dst_access_mask;

	vkCmdPipelineBarrier(buffer, policy.src_stage_mask, policy.dst_stage_mask, 0, 1, &mem_barrier, 0, nullptr, 0, nullptr);
}

bool GraphicsRenderComputeImageStoreBarrierContractTest()
{
	const auto policy = GetComputeImageStoreBarrierPolicy();
	return policy.src_stage_mask == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT &&
	       policy.dst_stage_mask == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT &&
	       policy.src_access_mask == VK_ACCESS_SHADER_WRITE_BIT &&
	       policy.dst_access_mask == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}

static void GraphicsRenderRenderTextureBarrier(VkCommandBuffer vk_buffer, VulkanImage* image)
{
	EXIT_IF(image == nullptr);

	if (image->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
	{
		VkImageMemoryBarrier image_memory_barrier {};
		image_memory_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		image_memory_barrier.pNext                           = nullptr;
		image_memory_barrier.srcAccessMask                   = VK_ACCESS_MEMORY_WRITE_BIT;
		image_memory_barrier.dstAccessMask                   = VK_ACCESS_MEMORY_READ_BIT;
		image_memory_barrier.oldLayout                       = image->layout;
		image_memory_barrier.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.image                           = image->image;
		image_memory_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		image_memory_barrier.subresourceRange.baseMipLevel   = 0;
		image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
		image_memory_barrier.subresourceRange.baseArrayLayer = 0;
		image_memory_barrier.subresourceRange.layerCount     = 1;

		vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                     VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		                     &image_memory_barrier);

		image->layout = image_memory_barrier.newLayout;
	}
}

static constexpr bool GuestRangesOverlapFailClosed(uint64_t first_addr, uint64_t first_size,
	                                                uint64_t second_addr, uint64_t second_size)
{
	if (first_addr == 0 || first_size == 0 || second_addr == 0 || second_size == 0 ||
	    first_size > UINT64_MAX - first_addr || second_size > UINT64_MAX - second_addr)
	{
		return true;
	}
	return first_addr < second_addr + second_size && second_addr < first_addr + first_size;
}

struct VideoOutSampledAliasBarrierPolicy
{
	VkImageLayout         old_layout;
	VkImageLayout         new_layout;
	VkPipelineStageFlags  src_stage_mask;
	VkPipelineStageFlags  dst_stage_mask;
	VkAccessFlags         src_access_mask;
	VkAccessFlags         dst_access_mask;
};

static constexpr VideoOutSampledAliasBarrierPolicy GetVideoOutSampledAliasBarrierPolicy(bool restore)
{
	if (!restore)
	{
		return {
		    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
		    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
		    VK_ACCESS_SHADER_READ_BIT,
		};
	}
	return {
	    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
	    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	    VK_ACCESS_SHADER_READ_BIT,
	    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
	};
}

static void GraphicsRenderVideoOutSampledAliasBarrier(VkCommandBuffer vk_buffer,
	                                                    VideoOutVulkanImage* const* images,
	                                                    uint32_t images_num, bool restore)
{
	if (images_num == 0)
	{
		return;
	}
	EXIT_IF(vk_buffer == VK_NULL_HANDLE || images == nullptr ||
	        images_num > ShaderTextureResources::RES_MAX * 2u);
	const auto policy = GetVideoOutSampledAliasBarrierPolicy(restore);
	VkImageMemoryBarrier barriers[ShaderTextureResources::RES_MAX * 2u] {};
	for (uint32_t index = 0; index < images_num; ++index)
	{
		EXIT_IF(images[index] == nullptr || images[index]->image == VK_NULL_HANDLE);
		auto& barrier                            = barriers[index];
		barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask                   = policy.src_access_mask;
		barrier.dstAccessMask                   = policy.dst_access_mask;
		barrier.oldLayout                       = policy.old_layout;
		barrier.newLayout                       = policy.new_layout;
		barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		barrier.image                           = images[index]->image;
		barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel   = 0;
		barrier.subresourceRange.levelCount     = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount     = 1;
	}
	vkCmdPipelineBarrier(vk_buffer, policy.src_stage_mask, policy.dst_stage_mask, 0, 0, nullptr, 0, nullptr,
	                     images_num, barriers);
}

static void GraphicsRenderDepthStencilBarrier(VkCommandBuffer vk_buffer, VulkanImage* image)
{
	EXIT_IF(image == nullptr);

	if (image->layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
	{
		VkImageMemoryBarrier image_memory_barrier {};
		image_memory_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		image_memory_barrier.pNext                           = nullptr;
		image_memory_barrier.srcAccessMask                   = VK_ACCESS_MEMORY_WRITE_BIT;
		image_memory_barrier.dstAccessMask                   = VK_ACCESS_MEMORY_READ_BIT;
		image_memory_barrier.oldLayout                       = image->layout;
		image_memory_barrier.newLayout                       = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.image                           = image->image;
		image_memory_barrier.subresourceRange.aspectMask     = UtilGetDepthStencilAspectMask(image->format);
		image_memory_barrier.subresourceRange.baseMipLevel   = 0;
		image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
		image_memory_barrier.subresourceRange.baseArrayLayer = 0;
		image_memory_barrier.subresourceRange.layerCount     = 1;

		vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                     VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		                     &image_memory_barrier);

		image->layout = image_memory_barrier.newLayout;
	}
}

static std::chrono::steady_clock::time_point GetDiagnosticTimestamp(bool diagnostics_enabled)
{
	if (diagnostics_enabled)
	{
		return std::chrono::steady_clock::now();
	}
	return {};
}

void GraphicsRenderRenderTextureBarrier(CommandBuffer* buffer, uint64_t vaddr, uint64_t size)
{
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	const bool diagnostics_enabled = Config::GraphicsTimingEnabled();

	const auto diagnostic_start = GetDiagnosticTimestamp(diagnostics_enabled);
	Core::LockGuard lock(g_render_ctx->GetMutex());
	const auto diagnostic_locked = GetDiagnosticTimestamp(diagnostics_enabled);

	auto* vk_buffer = buffer->GetBufferOutsideRenderPass();

	auto       images           = FindRenderTexture(vaddr, size, false, "render-texture-barrier");
	const auto diagnostic_found = GetDiagnosticTimestamp(diagnostics_enabled);

	for (auto* image: images)
	{
		GraphicsRenderRenderTextureBarrier(vk_buffer, image);
	}

	if (diagnostics_enabled)
	{
		static thread_local uint64_t count = 0, lock_us = 0, find_us = 0, record_us = 0, images_num = 0;
		lock_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(diagnostic_locked - diagnostic_start).count());
		find_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(diagnostic_found - diagnostic_locked).count());
		record_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - diagnostic_found).count());
		images_num += images.Size();
		if (++count == 2040)
		{
			dbg_printf("[PERF] rt barriers count=%" PRIu64 " lock=%.3f us find=%.3f us record=%.3f us images=%.3f\n", count,
			           static_cast<double>(lock_us) / count, static_cast<double>(find_us) / count,
			           static_cast<double>(record_us) / count, static_cast<double>(images_num) / count);
		}
	}
}

void GraphicsRenderDepthStencilBarrier(CommandBuffer* buffer, uint64_t vaddr, uint64_t size)
{
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	const bool diagnostics_enabled = Config::GraphicsTimingEnabled();

	const auto diagnostic_start = GetDiagnosticTimestamp(diagnostics_enabled);
	Core::LockGuard lock(g_render_ctx->GetMutex());
	const auto diagnostic_locked = GetDiagnosticTimestamp(diagnostics_enabled);

	// Protected uniform-zero resources use a tracker-owned operation. The native
	// pointer and its complete guest-plane proof never escape to the generic finder.
	const auto protected_barrier =
	    GpuMemoryRecordUniformDepthZeroReadOnlyBarrier(g_render_ctx->GetGraphicCtx(), buffer, vaddr, size);
	if (protected_barrier.status == GpuMemoryDepthStencilBarrierStatus::Complete) return;
	if (protected_barrier.status == GpuMemoryDepthStencilBarrierStatus::Rejected)
	{
		EXIT("Uniform depth/stencil barrier rejected: %s\n",
		     protected_barrier.reason != nullptr ? protected_barrier.reason : "unknown");
	}

	auto* vk_buffer = buffer->GetBufferOutsideRenderPass();

	auto       images           = FindDepthStencil(vaddr, size, false, "depth-stencil-barrier");
	const auto diagnostic_found = GetDiagnosticTimestamp(diagnostics_enabled);

	for (auto* image: images)
	{
		GraphicsRenderDepthStencilBarrier(vk_buffer, image);
	}

	if (diagnostics_enabled)
	{
		static thread_local uint64_t count = 0, lock_us = 0, find_us = 0, record_us = 0, images_num = 0;
		lock_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(diagnostic_locked - diagnostic_start).count());
		find_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(diagnostic_found - diagnostic_locked).count());
		record_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - diagnostic_found).count());
		images_num += images.Size();
		if (++count == 840)
		{
			dbg_printf("[PERF] depth barriers count=%" PRIu64 " lock=%.3f us find=%.3f us record=%.3f us images=%.3f\n", count,
			           static_cast<double>(lock_us) / count, static_cast<double>(find_us) / count,
			           static_cast<double>(record_us) / count, static_cast<double>(images_num) / count);
		}
	}
}

struct UniformDepthZeroByteScan
{
	bool operator()(int, const uint8_t* bytes, uint64_t size) const
	{
		return std::all_of(bytes, bytes + size, [](uint8_t value) { return value == 0; });
	}
};

struct UniformDepthZeroLeaseAcquire
{
	Core::VirtualMemory::GuestDataLease* parent = nullptr;

	bool operator()(uint64_t begin, uint64_t bytes, Core::VirtualMemory::Mode mode,
	                Core::VirtualMemory::GuestDataLease* output)
	{
		// The first plane never waits for a mutation beneath graphics locks.
		// Once acquired, its exact managed lease already excludes that writer;
		// prove ownership/continuity before extending the reader to S and HTile.
		// This current-token proof grants only pin acquisition. The complete set
		// still validates every plane at the caller's original token before scan.
		if (parent != nullptr && parent->IsActive())
			return TryAcquireGraphicsGuestContinuityLeaseUnderParent(begin, bytes, mode,
			    parent->MutationGeneration(), *parent, output);
		if (!GraphicsGuestLeaseAcquire {}(begin, bytes, mode, output)) return false;
		parent = output;
		return true;
	}
};

// Retry only the complete local acquisition set. No byte scan, GPU operation,
// callback, sleep, yield or caller-lock release belongs to this loop.
template <typename Acquire, typename Scan>
static bool DepthSurfaceUniformZeroBytesWithAcquisition(const uint64_t* addresses, const uint64_t* sizes, int count,
                                                        uint64_t expected_generation, const char** rejection,
	                                                    int* rejected_plane, Acquire&& acquire, Scan&& scan,
	                                                    DepthSurfaceUniformZeroLeaseSet* retained_set = nullptr)
{
	using namespace Core::VirtualMemory;
	if (rejection != nullptr) *rejection = nullptr;
	if (rejected_plane != nullptr) *rejected_plane = -1;
	const auto refuse = [rejection, rejected_plane](const char* reason, int plane = -1) {
		if (rejection != nullptr) *rejection = reason;
		if (rejected_plane != nullptr) *rejected_plane = plane;
		return false;
	};
	if (retained_set != nullptr)
		for (const auto& lease: retained_set->planes)
			if (lease.IsActive()) return refuse("retained-lease-set-not-empty");
	if (addresses == nullptr || sizes == nullptr || count != 3) return refuse("plane-count-or-input");
	for (int plane = 0; plane < 3; ++plane)
	{
		const uint64_t address = addresses[plane];
		const uint64_t size = sizes[plane];
		const uint64_t cap = plane == 2 ? HTILE_CPU_OBSERVATION_MAX_BYTES : CPU_OBSERVATION_HARD_MAX_BYTES;
		if (address == 0 || size == 0 || size > cap || size % 4u != 0 || address > UINTPTR_MAX ||
		    size > UINTPTR_MAX - address) return refuse("plane-range", plane);
		for (int prior = 0; prior < plane; ++prior)
			if (address < addresses[prior] + sizes[prior] && addresses[prior] < address + size)
				return refuse("plane-overlap", plane);
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	if (expected_generation == UINT64_MAX) return refuse("mapping-generation-unavailable");
	if (expected_generation > GetMutationGeneration()) return refuse("mapping-generation");
	DepthSurfaceUniformZeroLeaseSet local_set;
	auto& leases = retained_set != nullptr ? retained_set->planes : local_set.planes;
	struct FailureCleanup
	{
		GuestDataLease* leases;
		bool keep = false;
		~FailureCleanup()
		{
			if (!keep)
				for (int plane = 0; plane != 3; ++plane) ReleaseGuestDataLease(&leases[plane]);
		}
	} cleanup {leases};
	constexpr uint32_t max_attempts = 16;
	for (uint32_t attempt = 1; attempt <= max_attempts; ++attempt)
	{
		int failed_plane = -1;
		for (int plane = 0; plane < 3; ++plane)
		{
			if (!acquire(addresses[plane], sizes[plane], Mode::Read, &leases[plane]))
			{
				failed_plane = plane;
				break;
			}
			if (!leases[plane].IsActive() || leases[plane].HostBegin() != addresses[plane] ||
			    leases[plane].ByteCount() != sizes[plane]) return refuse("lease-range", plane);
		}
		if (failed_plane < 0) break;
		// A test acquisition may return false with its real lease still active.
		// Release every local plane, including that output, before considering
		// another attempt. Never release or extend an outer caller's borrower.
		for (auto& lease: leases) ReleaseGuestDataLease(&lease);
		if (CurrentThreadHasMemoryLease()) return refuse("retry-memory-lease-held", failed_plane);
		// A false VM result is ambiguous. Reject a known logical denial using
		// only the optional immutable snapshot; never use its registry fallback
		// without a lease, and never treat a snapshot as admission authority.
		uint64_t cursor = addresses[failed_plane];
		const uint64_t end = cursor + sizes[failed_plane];
		while (cursor < end)
		{
			ManagedRegionState state = ManagedRegionState::Unmanaged;
			ManagedRegion region {};
			if (!TryQueryManagedRegionSnapshot(cursor, &state, &region)) break;
			if (state != ManagedRegionState::Visible || region.begin > cursor || region.end <= cursor ||
			    IsExecute(region.mode) ||
			    (static_cast<uint32_t>(region.mode) & static_cast<uint32_t>(Mode::Read)) == 0)
				return refuse("managed-readability", failed_plane);
			cursor = std::min(end, region.end);
		}
		if (attempt == max_attempts) return refuse("lease-set-retry-exhausted", failed_plane);
	}
	// Validate complete managed coverage and continuity from the original token
	// only after the entire set is pinned. Unrelated mappings may have changed
	// the global generation, but every depth/stencil/HTile mapping must retain
	// its identity and protection history through this exact lease set.
	// No successful guest content scan can be repeated by acquisition retry.
	for (int plane = 0; plane < 3; ++plane)
	{
		uint64_t cursor = addresses[plane];
		const uint64_t end = cursor + sizes[plane];
		while (cursor < end)
		{
			ManagedRegion region {};
			if (QueryManagedRegion(cursor, &region) != ManagedRegionState::Visible ||
			    region.begin > cursor || region.end <= cursor || IsExecute(region.mode) ||
			    (static_cast<uint32_t>(region.mode) & static_cast<uint32_t>(Mode::Read)) == 0)
				return refuse("managed-readability", plane);
			cursor = std::min(end, region.end);
		}
		if (!ValidateGuestDataLeaseRangeContinuity(leases[plane], addresses[plane], sizes[plane], expected_generation))
			return refuse("mapping-generation", plane);
	}
	// HTile fast-clear +0 defines logical depth without reading depth samples.
	// Separate uncompressed S8 must be byte-zero over its complete allocation.
	// Mapping leases exclude replacement, not unsynchronized ordinary stores.
	for (int plane: {1, 2})
		if (!scan(plane, reinterpret_cast<const uint8_t*>(leases[plane].HostBegin()), sizes[plane]))
			return refuse("nonzero-content", plane);
	cleanup.keep = retained_set != nullptr;
	return true;
#else
	(void)expected_generation; (void)acquire; (void)scan;
	return refuse("mapping-lease-capability-unavailable");
#endif
}

bool DepthSurfaceUniformZeroBytes(const uint64_t* addresses, const uint64_t* sizes, int count,
	                             uint64_t expected_generation, const char** rejection, int* rejected_plane,
	                             DepthSurfaceUniformZeroLeaseSet* retained_set)
{
	return DepthSurfaceUniformZeroBytesWithAcquisition(addresses, sizes, count, expected_generation,
	    rejection, rejected_plane, UniformDepthZeroLeaseAcquire {}, UniformDepthZeroByteScan {}, retained_set);
}

bool DepthSurfaceUniformZeroBytesUnderParent(const uint64_t* addresses, const uint64_t* sizes, int count,
    uint64_t original_generation, const Core::VirtualMemory::GuestDataLease& parent,
    const char** rejection, DepthSurfaceUniformZeroLeaseSet* retained_set, int* rejected_plane)
{
	return DepthSurfaceUniformZeroBytesWithAcquisition(addresses, sizes, count, original_generation,
	    rejection, rejected_plane, [&parent, original_generation](uint64_t begin, uint64_t bytes, Core::VirtualMemory::Mode mode,
	                                  Core::VirtualMemory::GuestDataLease* output) {
		    return TryAcquireGraphicsGuestContinuityLeaseUnderParent(begin, bytes, mode,
		        original_generation, parent, output);
	    }, UniformDepthZeroByteScan {}, retained_set);
}

bool GraphicsRenderUniformDepthZeroReusePause(CommandProcessor* cp, CommandBuffer* buffer)
{
	// Native range execution owns an additional outer renderer lock. This
	// retry protocol requires exactly renderer1/CP1 and cannot drop that owner.
	if (GuestCpu::CurrentMemoryHandoffNativeClaimOwner() != 0) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	if (g_render_ctx == nullptr || Core::VirtualMemory::CurrentThreadHasMemoryLease() ||
	    !GraphicsRunCommandProcessorExclusiveRecordingValid(cp, buffer)) return false;
	// The caller has unwound its unsuccessful admission and released tracker1.
	// RunLock stays owned; release the remaining renderer1/CP1 in reverse order
	// and reacquire CP1/renderer1 before it resumes the tracker proof.
	g_render_ctx->GetMutex().Unlock();
	GraphicsRunCommandProcessorUnlock(cp);
	Core::Thread::SleepMicro(1000);
	GraphicsRunCommandProcessorLock(cp);
	g_render_ctx->GetMutex().Lock();
	return true;
#else
	(void)cp; (void)buffer;
	return false;
#endif
}

static thread_local uint32_t g_videoout_renderer_admission_depth = 0;
struct RetainedRendererRecordingWitness
{
	CommandBuffer* buffer = nullptr;
	uint32_t depth = 0;
};
static thread_local RetainedRendererRecordingWitness g_retained_renderer_recording;
struct RetainedRendererRecordingScope
{
	RetainedRendererRecordingWitness previous = g_retained_renderer_recording;
	explicit RetainedRendererRecordingScope(CommandBuffer* buffer)
	{
		g_retained_renderer_recording = {buffer, previous.depth + 1u};
	}
	~RetainedRendererRecordingScope() { g_retained_renderer_recording = previous; }
};

bool GraphicsRenderRetainedRecordingScopeValid(CommandBuffer* buffer)
{
	return buffer != nullptr && g_retained_renderer_recording.buffer == buffer &&
	    g_retained_renderer_recording.depth == 1;
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool GraphicsRenderRetainedRecordingScopeContractTest()
{
	auto* buffer = reinterpret_cast<CommandBuffer*>(uintptr_t {0x1000});
	auto* other = reinterpret_cast<CommandBuffer*>(uintptr_t {0x2000});
	if (GraphicsRenderRetainedRecordingScopeValid(buffer)) return false;
	Core::Mutex renderer;
	{
		Core::LockGuard lock(renderer);
		RetainedRendererRecordingScope scope(buffer);
		if (!GraphicsRenderRetainedRecordingScopeValid(buffer) ||
		    GraphicsRenderRetainedRecordingScopeValid(other) || GraphicsRenderRetainedRecordingScopeValid(nullptr)) return false;
		bool contender_blocked = false, contender_unqualified = false;
		std::thread contender([&] {
			contender_unqualified = !GraphicsRenderRetainedRecordingScopeValid(buffer);
			contender_blocked = !renderer.TryLock();
			if (!contender_blocked) renderer.Unlock();
		});
		contender.join();
		if (!contender_blocked || !contender_unqualified) return false;
		{
			RetainedRendererRecordingScope nested(buffer);
			if (GraphicsRenderRetainedRecordingScopeValid(buffer)) return false;
		}
		if (!GraphicsRenderRetainedRecordingScopeValid(buffer)) return false;
	}
	if (GraphicsRenderRetainedRecordingScopeValid(buffer)) return false;
	std::fprintf(stderr, "[GFX_RETAINED_RENDERER_RECORDING_TEST] scope=exact nested/foreign/absent=reject"
	    " other-thread=unqualified renderer-contention=real scope-exit=revokes\n");
	return true;
}
#endif

struct VideoOutRendererAdmissionScope
{
	VideoOutRendererAdmissionScope() { ++g_videoout_renderer_admission_depth; }
	~VideoOutRendererAdmissionScope() { --g_videoout_renderer_admission_depth; }
};
struct VideoOutPinAdmissionWitness
{
	CommandProcessor* cp = nullptr;
	CommandBuffer* buffer = nullptr;
	uint32_t depth = 0;
};
static thread_local VideoOutPinAdmissionWitness g_videoout_pin_admission_witness;
[[maybe_unused]] static bool VideoOutPinAdmissionWitnessValid(const VideoOutPinAdmissionWitness& witness,
    uint32_t renderer_depth, CommandProcessor* cp, CommandBuffer* buffer)
{
	return renderer_depth == 1 && witness.depth == 1 && cp != nullptr && buffer != nullptr &&
	    witness.cp == cp && witness.buffer == buffer;
}
static bool PinVideoOutForRendererCommand(CommandBuffer* buffer, const GpuMemoryObjectHandle& handle,
    uint64_t address, uint64_t bytes, void** output)
{
	struct Scope
	{
		VideoOutPinAdmissionWitness previous = g_videoout_pin_admission_witness;
		~Scope() { g_videoout_pin_admission_witness = previous; }
	} scope;
	g_videoout_pin_admission_witness = {buffer != nullptr ? buffer->GetParent() : nullptr,
	    buffer, scope.previous.depth + 1u};
	return GpuMemoryPinVideoOutBufferForCommand(buffer, handle, address, bytes, output);
}

bool GraphicsRenderVideoOutPinAdmissionPause(CommandProcessor* cp, CommandBuffer* buffer)
{
	if (GuestCpu::CurrentMemoryHandoffNativeClaimOwner() != 0) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	if (g_render_ctx == nullptr || !VideoOutPinAdmissionWitnessValid(g_videoout_pin_admission_witness,
	        g_videoout_renderer_admission_depth, cp, buffer) || Core::VirtualMemory::CurrentThreadHasMemoryLease() ||
	    !GraphicsRunCommandProcessorExclusiveRecordingValid(cp, buffer)) return false;
	g_render_ctx->GetMutex().Unlock();
	GraphicsRunCommandProcessorUnlock(cp);
	Core::Thread::SleepMicro(1000);
	GraphicsRunCommandProcessorLock(cp);
	g_render_ctx->GetMutex().Lock();
	return true;
#else
	(void)cp; (void)buffer;
	return false;
#endif
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool DepthSurfaceUniformZeroLeaseSetContractTest()
{
	extern bool RenderFullCpuOuterLocksContractTest();
	extern bool CommandBufferStoppedSubmissionIdentityContractTest();
	if (!RenderFullCpuOuterLocksContractTest() || !CommandBufferStoppedSubmissionIdentityContractTest()) return false;
	{
		auto* cp = reinterpret_cast<CommandProcessor*>(uintptr_t {0x1000});
		auto* buffer = reinterpret_cast<CommandBuffer*>(uintptr_t {0x2000});
		const VideoOutPinAdmissionWitness witness {cp, buffer, 1};
		if (!VideoOutPinAdmissionWitnessValid(witness, 1, cp, buffer)) return false;
		for (uint32_t depth: {0u, 2u})
			if (VideoOutPinAdmissionWitnessValid(witness, depth, cp, buffer)) return false;
		for (uint32_t depth: {0u, 2u})
			if (VideoOutPinAdmissionWitnessValid({cp, buffer, depth}, 1, cp, buffer)) return false;
		if (VideoOutPinAdmissionWitnessValid(witness, 1, nullptr, buffer) ||
		    VideoOutPinAdmissionWitnessValid(witness, 1, cp, nullptr) ||
		    VideoOutPinAdmissionWitnessValid(witness, 1, reinterpret_cast<CommandProcessor*>(uintptr_t {0x3000}), buffer) ||
		    VideoOutPinAdmissionWitnessValid(witness, 1, cp, reinterpret_cast<CommandBuffer*>(uintptr_t {0x4000}))) return false;
		std::fprintf(stderr, "[GFX_VIDEOOUT_PIN_PAUSE_SCOPE_TEST] renderer1/current-pin1=required outside/nested/wrong-owner=reject"
		                     " modeled-identities=1 unlocks=0\n");
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	Init();
	const uint64_t address = Alloc(0, 65536, Mode::ReadWrite);
	if (address == 0) return false;
	struct Owner { uint64_t address; ~Owner() { Free(address); } } owner {address};
	const uint64_t addresses[] = {address, address + 16384, address + 32768};
	const uint64_t sizes[] = {64, 64, 64};
	std::memset(reinterpret_cast<void*>(address), 0, 65536);
	// Fast-clear metadata, rather than sample bytes, defines depth +0.
	std::memset(reinterpret_cast<void*>(address), 0xa5, sizes[0]);
	Core::Mutex processor_mutex, tracker_mutex;
	uint32_t acquisitions = 0;
	std::array<uint32_t, 3> scans {};
	const char* rejection = nullptr;
	int rejected_plane = -1;
	const auto scan = [&](int plane, const uint8_t* bytes, uint64_t size) {
		++scans[plane];
		return CurrentThreadHasMemoryLease() && bytes == reinterpret_cast<const uint8_t*>(addresses[plane]) &&
		       size == sizes[plane] && UniformDepthZeroByteScan {}(plane, bytes, size);
	};
	const auto acquire = [&](uint64_t begin, uint64_t size, Mode mode, GuestDataLease* lease) {
		++acquisitions;
		return TryAcquireGuestDataLease(begin, size, mode, lease, false);
	};
	const auto check = [&](const uint64_t* begins, const uint64_t* bytes, int count, uint64_t generation, auto&& borrow,
	                       DepthSurfaceUniformZeroLeaseSet* retained = nullptr) {
		acquisitions = 0;
		scans = {};
		return DepthSurfaceUniformZeroBytesWithAcquisition(begins, bytes, count, generation,
		    &rejection, &rejected_plane, borrow, scan, retained);
	};
	if (!check(addresses, sizes, 3, GetMutationGeneration(), acquire) || acquisitions != 3 ||
	    scans != std::array<uint32_t, 3> {0, 1, 1} || rejection != nullptr || rejected_plane != -1 ||
	    CurrentThreadHasMemoryLease()) return false;
	for (int failed_plane = 0; failed_plane < 3; ++failed_plane)
	{
		uint32_t rounds = 0;
		bool scope_valid = true;
		const auto recover = [&](uint64_t begin, uint64_t size, Mode mode, GuestDataLease* lease) {
			++acquisitions;
			const int plane = begin == addresses[0] ? 0 : begin == addresses[1] ? 1 : 2;
			if (plane == 0)
			{
				++rounds;
				scope_valid = scope_valid && !CurrentThreadHasMemoryLease();
				bool processor_available = true, tracker_available = true;
				std::thread observer([&] {
					processor_available = processor_mutex.TryLock();
					if (processor_available) processor_mutex.Unlock();
					tracker_available = tracker_mutex.TryLock();
					if (tracker_available) tracker_mutex.Unlock();
				});
				observer.join();
				scope_valid = scope_valid && !processor_available && !tracker_available;
			}
			else scope_valid = scope_valid && CurrentThreadHasMemoryLease();
			// Keep a real validated lease active even on injected false: cleanup
			// must include both earlier planes and the failed acquisition output.
			if (!TryAcquireGuestDataLease(begin, size, mode, lease, false)) { scope_valid = false; return false; }
			return rounds != 1 || plane != failed_plane;
		};
		Core::LockGuard processor_lock(processor_mutex);
		Core::LockGuard tracker_lock(tracker_mutex);
		if (!check(addresses, sizes, 3, GetMutationGeneration(), recover) || !scope_valid || rounds != 2 ||
		    acquisitions != static_cast<uint32_t>(failed_plane + 4) || scans != std::array<uint32_t, 3> {0, 1, 1} ||
		    CurrentThreadHasMemoryLease()) return false;
	}
	// The original plane set survives unrelated allocation activity, including
	// activity between failed acquisition rounds. Its token is never recaptured.
	for (const bool during_retry: {false, true})
	{
		const uint64_t original_generation = GetMutationGeneration();
		uint64_t unrelated = during_retry ? 0 : Alloc(0, 65536, Mode::ReadWrite);
		if (!during_retry && unrelated == 0) return false;
		const auto independent = [&](uint64_t begin, uint64_t size, Mode mode, GuestDataLease* lease) {
			++acquisitions;
			if (during_retry && acquisitions == 1)
			{
				if (CurrentThreadHasMemoryLease()) return false;
				unrelated = Alloc(0, 65536, Mode::ReadWrite);
				return false;
			}
			return TryAcquireGuestDataLease(begin, size, mode, lease, false);
		};
		const bool accepted = check(addresses, sizes, 3, original_generation, independent);
		if (unrelated == 0 || !Free(unrelated) || !accepted || acquisitions != (during_retry ? 4u : 3u) ||
		    scans != std::array<uint32_t, 3> {0, 1, 1} || CurrentThreadHasMemoryLease()) return false;
	}
	const auto deny_htile = [&](uint64_t begin, uint64_t size, Mode mode, GuestDataLease* lease) {
		++acquisitions;
		if (!TryAcquireGuestDataLease(begin, size, mode, lease, false)) return false;
		return begin != addresses[2];
	};
	if (check(addresses, sizes, 3, GetMutationGeneration(), deny_htile) || acquisitions != 48 ||
	    scans != std::array<uint32_t, 3> {} || CurrentThreadHasMemoryLease() || rejected_plane != 2 ||
	    std::strcmp(rejection, "lease-set-retry-exhausted") != 0) return false;
	GuestDataLease outer;
	if (!TryAcquireGuestDataLease(address + 49152, 64, Mode::Read, &outer, false) ||
	    !check(addresses, sizes, 3, GetMutationGeneration(), acquire) || acquisitions != 3 || !outer.IsActive() ||
	    scans != std::array<uint32_t, 3> {0, 1, 1}) return false;
	if (check(addresses, sizes, 3, GetMutationGeneration(), deny_htile) || acquisitions != 3 ||
	    scans != std::array<uint32_t, 3> {} || !outer.IsActive() ||
	    std::strcmp(rejection, "retry-memory-lease-held") != 0) return false;
	ReleaseGuestDataLease(&outer);
	if (CurrentThreadHasMemoryLease()) return false;
	for (const uint64_t generation: {UINT64_MAX, GetMutationGeneration() + 1u})
		if (check(addresses, sizes, 3, generation, acquire) || acquisitions != 0 ||
		    scans != std::array<uint32_t, 3> {} || CurrentThreadHasMemoryLease()) return false;
	bool changed_generation_valid = true;
	const uint64_t original_generation = GetMutationGeneration();
	const auto change_generation = [&](uint64_t begin, uint64_t size, Mode mode, GuestDataLease* lease) {
		++acquisitions;
		if (acquisitions == 1)
			changed_generation_valid = !CurrentThreadHasMemoryLease() && Protect(address, 65536, Mode::ReadWrite);
		return changed_generation_valid && TryAcquireGuestDataLease(begin, size, mode, lease, false);
	};
	if (check(addresses, sizes, 3, original_generation, change_generation) || !changed_generation_valid ||
	    acquisitions != 3 || scans != std::array<uint32_t, 3> {} || CurrentThreadHasMemoryLease() ||
	    std::strcmp(rejection, "mapping-generation") != 0) return false;
	const uint64_t denial_generation = GetMutationGeneration();
	const auto change_then_deny = [&](uint64_t begin, uint64_t size, Mode mode, GuestDataLease* lease) {
		++acquisitions;
		if (acquisitions == 1)
		{
			changed_generation_valid = !CurrentThreadHasMemoryLease() && Protect(address, 65536, Mode::ReadWrite);
			return false;
		}
		return changed_generation_valid && TryAcquireGuestDataLease(begin, size, mode, lease, false);
	};
	if (check(addresses, sizes, 3, denial_generation, change_then_deny) || !changed_generation_valid ||
	    acquisitions != 4 || scans != std::array<uint32_t, 3> {} || CurrentThreadHasMemoryLease() ||
	    std::strcmp(rejection, "mapping-generation") != 0) return false;
	// Restoring rights or recreating the same address with identical bytes does
	// not restore the original mapping/protection proof.
	for (const bool remap: {false, true})
	{
		const uint64_t baseline = GetMutationGeneration();
		if (remap)
		{
			if (!Free(address) || !AllocFixed(address, 65536, Mode::ReadWrite)) return false;
			std::memset(reinterpret_cast<void*>(address), 0, 65536);
		} else if (!Protect(address, 65536, Mode::Read) || !Protect(address, 65536, Mode::ReadWrite)) return false;
		if (check(addresses, sizes, 3, baseline, acquire) || acquisitions != 3 || scans != std::array<uint32_t, 3> {} ||
		    CurrentThreadHasMemoryLease() || std::strcmp(rejection, "mapping-generation") != 0) return false;
	}
	const auto wrong_scope = [&](uint64_t begin, uint64_t size, Mode mode, GuestDataLease* lease) {
		++acquisitions;
		return TryAcquireGuestDataLease(begin + 128, size, mode, lease, false);
	};
	if (check(addresses, sizes, 3, GetMutationGeneration(), wrong_scope) || acquisitions != 1 ||
	    scans != std::array<uint32_t, 3> {} || CurrentThreadHasMemoryLease() ||
	    std::strcmp(rejection, "lease-range") != 0) return false;
	for (Mode mode: {Mode::NoAccess, Mode::ExecuteReadWrite})
	{
		if (!Protect(addresses[1], 16384, mode) ||
		    check(addresses, sizes, 3, GetMutationGeneration(), acquire) || acquisitions == 0 || acquisitions > 48 ||
		    scans != std::array<uint32_t, 3> {} || CurrentThreadHasMemoryLease() ||
		    !Protect(addresses[1], 16384, Mode::ReadWrite)) return false;
	}
	// Complete coverage is required, including a final inaccessible fragment.
	uint64_t crossing_sizes[] = {sizes[0], sizes[1], 16388};
	if (!Protect(address + 49152, 16384, Mode::NoAccess) ||
	    check(addresses, crossing_sizes, 3, GetMutationGeneration(), acquire) || acquisitions == 0 ||
	    acquisitions > 48 || scans != std::array<uint32_t, 3> {} || CurrentThreadHasMemoryLease() ||
	    !Protect(address + 49152, 16384, Mode::ReadWrite)) return false;
	std::array<uint8_t, 64> native {};
	const uint64_t unowned[] = {addresses[0], addresses[1], reinterpret_cast<uint64_t>(native.data())};
	if (check(unowned, sizes, 3, GetMutationGeneration(), acquire) || acquisitions != 3 ||
	    scans != std::array<uint32_t, 3> {} || CurrentThreadHasMemoryLease() ||
	    std::strcmp(rejection, "managed-readability") != 0) return false;
	for (int plane: {1, 2})
	{
		auto* bytes = reinterpret_cast<uint8_t*>(addresses[plane]);
		bytes[sizes[plane] - 1u] = 1;
		const bool accepted = check(addresses, sizes, 3, GetMutationGeneration(), acquire);
		bytes[sizes[plane] - 1u] = 0;
		if (accepted || acquisitions != 3 || scans[0] != 0 || scans[1] != 1 || scans[2] != (plane == 2 ? 1u : 0u) ||
		    CurrentThreadHasMemoryLease() || rejected_plane != plane || std::strcmp(rejection, "nonzero-content") != 0) return false;
	}
	for (const int count: {0, 2, 4})
		if (check(addresses, sizes, count, GetMutationGeneration(), acquire) || acquisitions != 0 ||
		    scans != std::array<uint32_t, 3> {}) return false;
	uint64_t overlap[] = {addresses[0], addresses[0] + 4, addresses[2]};
	if (check(overlap, sizes, 3, GetMutationGeneration(), acquire) || acquisitions != 0 ||
	    scans != std::array<uint32_t, 3> {}) return false;
	for (int plane = 0; plane < 3; ++plane)
	{
		uint64_t bad_addresses[] = {addresses[0], addresses[1], addresses[2]};
		uint64_t bad_sizes[] = {sizes[0], sizes[1], sizes[2]};
		bad_sizes[plane] = 0;
		if (check(bad_addresses, bad_sizes, 3, GetMutationGeneration(), acquire) || acquisitions != 0 ||
		    scans != std::array<uint32_t, 3> {}) return false;
		bad_sizes[plane] = sizes[plane];
		bad_addresses[plane] = UINT64_MAX - sizes[plane] + 1u;
		if (check(bad_addresses, bad_sizes, 3, GetMutationGeneration(), acquire) || acquisitions != 0 ||
		    scans != std::array<uint32_t, 3> {}) return false;
	}
	if (check(nullptr, sizes, 3, GetMutationGeneration(), acquire) || acquisitions != 0 ||
	    check(addresses, nullptr, 3, GetMutationGeneration(), acquire) || acquisitions != 0 ||
	    scans != std::array<uint32_t, 3> {}) return false;
	// A real active VM transaction remains denied on all sixteen attempts.
	MutationTransaction transaction;
	ReservationHandle reservation;
	if (!BeginReserveAligned(0, 65536, 65536, &reservation, &transaction)) return false;
	const bool accepted = check(addresses, sizes, 3, GetMutationGeneration(), acquire);
	if (!transaction.Abort() || accepted || acquisitions != 16 || scans != std::array<uint32_t, 3> {} ||
	    CurrentThreadHasMemoryLease() || std::strcmp(rejection, "lease-set-retry-exhausted") != 0) return false;
	// Successful retention keeps the exact scan borrowers through the caller's
	// binding decision. Rejected sets leave no partial borrower in the output.
	for (int failed_plane = 0; failed_plane != 3; ++failed_plane)
	{
		uint32_t rounds = 0;
		const auto recover_retained = [&](uint64_t begin, uint64_t size, Mode mode, GuestDataLease* lease) {
			++acquisitions;
			const int plane = begin == addresses[0] ? 0 : begin == addresses[1] ? 1 : 2;
			if (plane == 0) ++rounds;
			if (!TryAcquireGuestDataLease(begin, size, mode, lease, false)) return false;
			return rounds != 1 || plane != failed_plane;
		};
		{
			DepthSurfaceUniformZeroLeaseSet retained;
			const uint64_t original_generation = GetMutationGeneration();
			if (!check(addresses, sizes, 3, original_generation, recover_retained, &retained) || rounds != 2 ||
			    acquisitions != static_cast<uint32_t>(failed_plane + 4) ||
			    scans != std::array<uint32_t, 3> {0, 1, 1} || !CurrentThreadHasMemoryLease()) return false;
			for (int plane = 0; plane != 3; ++plane)
				if (!retained.planes[plane].IsActive() || retained.planes[plane].HostBegin() != addresses[plane] ||
				    retained.planes[plane].ByteCount() != sizes[plane] ||
				    !ValidateGuestDataLeaseRangeContinuity(retained.planes[plane], addresses[plane], sizes[plane], original_generation)) return false;
		}
		if (CurrentThreadHasMemoryLease()) return false;
		const auto deny_retained = [&](uint64_t begin, uint64_t size, Mode mode, GuestDataLease* lease) {
			++acquisitions;
			const int plane = begin == addresses[0] ? 0 : begin == addresses[1] ? 1 : 2;
			if (!TryAcquireGuestDataLease(begin, size, mode, lease, false)) return false;
			return plane != failed_plane;
		};
		DepthSurfaceUniformZeroLeaseSet rejected;
		if (check(addresses, sizes, 3, GetMutationGeneration(), deny_retained, &rejected) ||
		    acquisitions != 16u * static_cast<uint32_t>(failed_plane + 1) || scans != std::array<uint32_t, 3> {} ||
		    CurrentThreadHasMemoryLease() || rejected_plane != failed_plane ||
		    std::strcmp(rejection, "lease-set-retry-exhausted") != 0) return false;
		for (const auto& lease: rejected.planes) if (lease.IsActive()) return false;
	}
	{
		DepthSurfaceUniformZeroLeaseSet incoming;
		if (!TryAcquireGuestDataLease(addresses[1], sizes[1], Mode::Read, &incoming.planes[1], false)) return false;
		const uint64_t inherited_generation = incoming.planes[1].MutationGeneration();
		if (check(addresses, sizes, 3, GetMutationGeneration(), acquire, &incoming) || acquisitions != 0 ||
		    scans != std::array<uint32_t, 3> {} || std::strcmp(rejection, "retained-lease-set-not-empty") != 0 ||
		    incoming.planes[0].IsActive() || !incoming.planes[1].IsActive() || incoming.planes[2].IsActive() ||
		    incoming.planes[1].MutationGeneration() != inherited_generation) return false;
	}
	if (CurrentThreadHasMemoryLease()) return false;
	for (const int nonzero_plane: {1, 2})
	{
		DepthSurfaceUniformZeroLeaseSet rejected;
		auto* bytes = reinterpret_cast<uint8_t*>(addresses[nonzero_plane]);
		bytes[sizes[nonzero_plane] - 1] = 1;
		const bool success = check(addresses, sizes, 3, GetMutationGeneration(), acquire, &rejected);
		bytes[sizes[nonzero_plane] - 1] = 0;
		if (success || acquisitions != 3 || CurrentThreadHasMemoryLease() || rejected_plane != nonzero_plane ||
		    scans != std::array<uint32_t, 3> {0, 1, nonzero_plane == 2 ? 1u : 0u} ||
		    std::strcmp(rejection, "nonzero-content") != 0) return false;
		for (const auto& lease: rejected.planes) if (lease.IsActive()) return false;
	}
	// Start a real VM writer after the first production borrow, before S/HTile
	// acquisition. Fresh nonblocking borrows are refused; the production adapter
	// extends the proven parent reader and keeps the scan pins through binding.
	// Join only after the complete retained set has left scope.
	std::atomic<bool> writer_ready {false}, writer_requested {false}, writer_cancelled {false};
	std::atomic<bool> writer_started {false}, writer_completed {false}, writer_ok {false};
	std::thread writer([&] {
		writer_ready.store(true, std::memory_order_release);
		while (!writer_requested.load(std::memory_order_acquire) && !writer_cancelled.load(std::memory_order_acquire))
			std::this_thread::yield();
		if (writer_cancelled.load(std::memory_order_acquire)) return;
		writer_started.store(true, std::memory_order_release);
		MutationTransaction pending;
		ReservationHandle reservation;
		const bool begun = BeginReserveAligned(0, 65536, 65536, &reservation, &pending);
		writer_ok.store(begun && pending.Abort(), std::memory_order_release);
		writer_completed.store(true, std::memory_order_release);
	});
	// The worker is prepared before the first borrower exists. No caller sleep,
	// yield, thread join or mutex wait is added while the scan set is retained.
	while (!writer_ready.load(std::memory_order_acquire)) std::this_thread::yield();
	bool pending_writer_valid = true;
	uint32_t binding_updates = 0;
	{
		DepthSurfaceUniformZeroLeaseSet retained;
		const uint64_t original_generation = GetMutationGeneration();
		UniformDepthZeroLeaseAcquire production_acquire;
		const auto acquire_with_writer = [&](uint64_t begin, uint64_t bytes, Mode mode, GuestDataLease* output) {
			++acquisitions;
			if (!production_acquire(begin, bytes, mode, output)) return false;
			if (begin == addresses[0])
			{
				writer_requested.store(true, std::memory_order_release);
				bool fresh_denied = false;
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
				while (!fresh_denied && std::chrono::steady_clock::now() < deadline)
				{
					if (!writer_started.load(std::memory_order_acquire)) continue;
					GuestDataLease fresh;
					fresh_denied = !TryAcquireGuestDataLease(address + 49152, 64, Mode::Read, &fresh, false);
				}
				pending_writer_valid = fresh_denied && !writer_completed.load(std::memory_order_acquire);
			}
			return pending_writer_valid;
		};
		acquisitions = 0; scans = {};
		const bool success = DepthSurfaceUniformZeroBytesWithAcquisition(addresses, sizes, 3, original_generation,
		    &rejection, &rejected_plane, acquire_with_writer, scan, &retained);
		pending_writer_valid = pending_writer_valid && success && acquisitions == 3 &&
		    scans == std::array<uint32_t, 3> {0, 1, 1} && CurrentThreadHasMemoryLease() &&
		    !writer_completed.load(std::memory_order_acquire);
		for (int plane = 0; plane != 3; ++plane)
			pending_writer_valid = pending_writer_valid && retained.planes[plane].IsActive() &&
			    retained.planes[plane].HostBegin() == addresses[plane] && retained.planes[plane].ByteCount() == sizes[plane] &&
			    ValidateGuestDataLeaseRangeContinuity(retained.planes[plane], addresses[plane], sizes[plane], original_generation);
		if (pending_writer_valid) ++binding_updates;
	}
	writer_cancelled.store(true, std::memory_order_release);
	if (writer.joinable()) writer.join();
	if (!pending_writer_valid || binding_updates != 1 || !writer_ok.load(std::memory_order_acquire) ||
	    !writer_completed.load(std::memory_order_acquire) || CurrentThreadHasMemoryLease()) return false;
	std::fprintf(stderr, "[GFX_DEPTH_RETAINED_LEASE_SET_TEST] scan-pins=three-through-binding postscan-reborrow=0"
	                     " recover-plane0/1/2=2 exhaust-rounds=16 full-failure-cleanup=pass incoming-active=preserved"
	                     " S/HTile-scans=once pending-real-writer=after-plane0 fresh-borrow=denied"
	                     " production-parent-child-borrows=two retained-binding=once release-before-join=pass\n");
	std::fprintf(stderr, "[GFX_DEPTH_LEASE_SET_TEST] immediate-round-bound=16 recover-plane0/1/2=2"
	                     " partial-set=fully-released failed-output=released outer-success=preserved outer-retry=reject"
	                     " caller-locks=retained original-token=retained unrelated-mapping/retry-mutation=accepted"
	                     " protect/restore/remap/scope/coverage/permissions/execute/unowned=reject"
	                     " scan-D=0 scan-S/H=once-after-full-set exhausted-scans=0 real-mutation-denial=16\n");
#endif
	return true;
}
#endif

static void ObserveHtileStorageWriteback(uint64_t submit_id, CommandBuffer* buffer, const HW::Context& hw,
	                                   const DepthSurfaceLayout& layout)
{
	if (!HtileCpuObservationEnabled() || layout.htile.allocation.size == 0) return;
	static std::atomic<uint32_t> emitted {0};
	auto event = emitted.load(std::memory_order_relaxed);
	do
	{
		if (event >= HTILE_CPU_OBSERVATION_MAX_EVENTS) return;
	} while (!emitted.compare_exchange_weak(event, event + 1u, std::memory_order_relaxed));

	HtileStorageWritebackSnapshot snapshot {};
	const bool available = GetHtileStorageWritebackHistory().Find(layout.htile.address, layout.htile.allocation.size, &snapshot);
	const auto& z = hw.GetDepthRenderTarget();
	const auto& rc = hw.GetRenderControl();
	const auto& dc = hw.GetDepthControl();
	uint32_t depth_clear_raw = 0;
	const float depth_clear = hw.GetDepthClearValue();
	static_assert(sizeof(depth_clear_raw) == sizeof(depth_clear));
	std::memcpy(&depth_clear_raw, &depth_clear, sizeof(depth_clear_raw));
	std::fprintf(stderr,
	             "[GFX_HTILE_CPU_OBSERVATION_V1] event=%u submit=%" PRIu64 " recording=%" PRIu64
	             " address=0x%016" PRIx64 " size=%u available=%u source=storage-writeback"
	             " snapshot-not-current=1 guest-current=unverified completion=not-attested-by-diagnostic atomic=0"
	             " width=%u height=%u pitch_div8_minus1=%u slice_div64_minus1=%u depth_format=%u stencil_format=%u"
	             " htile_enable=%u htile_raw=0x%08" PRIx32 " preload=%u tile_stencil_disable=%u"
	             " depth_test=%u depth_write=%u depth_clear=%u depth_clear_raw=0x%08" PRIx32
	             " stencil_test=%u stencil_clear=%u stencil_clear_raw=0x%02x"
	             " depth_compress_disable=%u stencil_compress_disable=%u resummarize=%u\n",
	             event + 1u, submit_id, buffer != nullptr ? buffer->GetRecordingId() : 0,
	             layout.htile.address, layout.htile.allocation.size, available ? 1u : 0u,
	             z.width, z.height, z.pitch_div8_minus1, z.slice_div64_minus1, z.z_info.format, z.stencil_info.format,
	             z.z_info.tile_surface_enable ? 1u : 0u, z.htile_surface.raw, z.htile_surface.preload,
	             z.stencil_info.tile_stencil_disable ? 1u : 0u, dc.z_enable ? 1u : 0u, dc.z_write_enable ? 1u : 0u,
	             rc.depth_clear_enable ? 1u : 0u, depth_clear_raw, dc.stencil_enable ? 1u : 0u,
	             rc.stencil_clear_enable ? 1u : 0u, static_cast<unsigned>(hw.GetStencilClearValue()),
	             rc.depth_compress_disable ? 1u : 0u, rc.stencil_compress_disable ? 1u : 0u, rc.resummarize_enable ? 1u : 0u);
	if (available)
	{
		const auto& summary = snapshot.summary;
		std::fprintf(stderr,
		             "[GFX_HTILE_CPU_SUMMARY_V1] event=%u snapshot=%" PRIu64 " source_memory=%" PRIu64
		             " bytes=%u words=%u uniform=%u byte_uniform=%u first_byte=0x%02x fnv1a64=0x%016" PRIx64
		             " bins=%u other_words=%u bin_policy=first-distinct\n",
		             event + 1u, snapshot.sequence, snapshot.source_memory_id, summary.byte_count, summary.word_count,
		             summary.IsUniform() ? 1u : 0u, summary.byte_uniform ? 1u : 0u, static_cast<unsigned>(summary.first_byte),
		             summary.fnv1a64, summary.bin_count, summary.other_words);
		for (uint32_t bin = 0; bin < summary.bin_count; ++bin)
		{
			std::fprintf(stderr, "[GFX_HTILE_CPU_BIN_V1] event=%u value=0x%08" PRIx32 " words=%u\n",
			             event + 1u, summary.bins[bin].value, summary.bins[bin].count);
		}
	}
	std::fflush(stderr);
}

static void ObserveStencilCpuBytes(uint64_t submit_id, CommandBuffer* buffer, const HW::Context& hw,
	                              const DepthSurfaceLayout& layout)
{
	static const bool enabled = HtileCpuObservationSelectorEnabled(std::getenv("LUMEN_GFX_STENCIL_CPU_OBSERVATION"));
	if (!enabled || layout.stencil.allocation.size == 0) return;
	static std::atomic<uint32_t> emitted {0};
	auto event = emitted.load(std::memory_order_relaxed);
	do
	{
		if (event >= HTILE_CPU_OBSERVATION_MAX_EVENTS) return;
	} while (!emitted.compare_exchange_weak(event, event + 1u, std::memory_order_relaxed));

	const uint64_t address = layout.stencil.address;
	const uint64_t size = layout.stencil.allocation.size;
	const char* status = "unsupported-backend";
	HtileCpuSummary summary {};
	uint64_t mapping_generation = 0;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	if (address == 0 || size > CPU_OBSERVATION_HARD_MAX_BYTES || size % 4u != 0 || address > UINT64_MAX - size)
	{
		status = "invalid-range-or-cap";
	} else
	{
		// Allocate before borrowing VM data. No allocation, log, GPU call, or
		// VM mutation occurs while the reader gate pins the source mappings.
		std::unique_ptr<uint8_t[]> copy(new (std::nothrow) uint8_t[static_cast<size_t>(size)]);
		status = "host-allocation-failed";
		if (copy != nullptr)
		{
			bool copied = false;
			{
				Core::VirtualMemory::GuestDataLease lease;
				status = "lease-denied";
				if (Core::VirtualMemory::TryAcquireGuestDataLease(address, size, Core::VirtualMemory::Mode::Read, &lease))
				{
					// The general lease also accepts native buffers. This diagnostic
					// requires every byte to belong to readable, non-executable Core
					// guest mappings, checked while the lease excludes replacement.
					bool managed = lease.HostBegin() == address && lease.ByteCount() == size;
					uint64_t cursor = address;
					while (managed && cursor < address + size)
					{
						Core::VirtualMemory::ManagedRegion region {};
						const auto state = Core::VirtualMemory::QueryManagedRegion(cursor, &region);
						managed = state == Core::VirtualMemory::ManagedRegionState::Visible &&
						          region.begin <= cursor && region.end > cursor &&
						          (static_cast<uint32_t>(region.mode) & static_cast<uint32_t>(Core::VirtualMemory::Mode::Read)) != 0 &&
						          !Core::VirtualMemory::IsExecute(region.mode);
						if (managed) cursor = std::min(address + size, region.end);
					}
					status = "not-managed-readable-data";
					if (managed)
					{
						mapping_generation = lease.MutationGeneration();
						std::memcpy(copy.get(), reinterpret_cast<const void*>(lease.HostBegin()), static_cast<size_t>(size));
						copied = true;
					}
				}
			}
			// The lease pins addresses and rights, not ordinary CPU/GPU writes.
			// Only the owned copy is inspected after releasing it; uniform bytes
			// do not establish guest/GPU resource authority or import semantics.
			if (copied)
			{
				status = SummarizeHtileCpuBytes(copy.get(), size, &summary, CPU_OBSERVATION_HARD_MAX_BYTES)
				             ? "sampled" : "summary-rejected";
			}
		}
	}
#endif
	const auto& z = hw.GetDepthRenderTarget();
	const auto& rc = hw.GetRenderControl();
	const auto& dc = hw.GetDepthControl();
	std::fprintf(stderr,
	             "[GFX_STENCIL_CPU_OBSERVATION_V1] event=%u submit=%" PRIu64 " recording=%" PRIu64
	             " address=0x%016" PRIx64 " size=%" PRIu64 " raw_address=0x%016" PRIx64
	             " swizzle=0x%016" PRIx64 " status=%s current=%s source=guest-cpu-copy atomic=0"
	             " gpu-completion=unverified authority=unverified mapping_generation=%" PRIu64
	             " stencil_format=%u stencil_tile_mode=%u tile_stencil_disable=%u texture_compatible=%u"
	             " stencil_test=%u stencil_clear=%u stencil_clear_raw=0x%02x stencil_write_disable=%u"
	             " stencil_compress_disable=%u depth_test=%u depth_write=%u depth_clear=%u htile_enable=%u htile_raw=0x%08" PRIx32 "\n",
	             event + 1u, submit_id, buffer != nullptr ? buffer->GetRecordingId() : 0,
	             address, size, z.stencil_read_base_addr, layout.stencil.swizzle, status,
	             summary.byte_count != 0 ? "sampled" : "unavailable", mapping_generation,
	             z.stencil_info.format, z.stencil_info.tile_mode_index, z.stencil_info.tile_stencil_disable ? 1u : 0u,
	             z.stencil_info.texture_compatible_stencil ? 1u : 0u, dc.stencil_enable ? 1u : 0u,
	             rc.stencil_clear_enable ? 1u : 0u, static_cast<unsigned>(hw.GetStencilClearValue()),
	             z.depth_view.stencil_write_disable ? 1u : 0u, rc.stencil_compress_disable ? 1u : 0u,
	             dc.z_enable ? 1u : 0u, dc.z_write_enable ? 1u : 0u, rc.depth_clear_enable ? 1u : 0u,
	             z.z_info.tile_surface_enable ? 1u : 0u, z.htile_surface.raw);
	if (summary.byte_count != 0)
	{
		std::fprintf(stderr,
		             "[GFX_STENCIL_CPU_SUMMARY_V1] event=%u bytes=%u words=%u word_uniform=%u byte_uniform=%u"
		             " first_byte=0x%02x fnv1a64=0x%016" PRIx64 " bins=%u other_words=%u bin_policy=first-distinct\n",
		             event + 1u, summary.byte_count, summary.word_count, summary.IsUniform() ? 1u : 0u,
		             summary.byte_uniform ? 1u : 0u, static_cast<unsigned>(summary.first_byte), summary.fnv1a64,
		             summary.bin_count, summary.other_words);
		for (uint32_t bin = 0; bin < summary.bin_count; ++bin)
		{
			std::fprintf(stderr, "[GFX_STENCIL_CPU_BIN_V1] event=%u value=0x%08" PRIx32 " words=%u\n",
			             event + 1u, summary.bins[bin].value, summary.bins[bin].count);
		}
	}
	std::fflush(stderr);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void FindRenderDepthInfo(uint64_t submit_id, CommandBuffer* buffer, const HW::Context& hw, RenderDepthInfo* r)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(r == nullptr);

	const auto& z  = hw.GetDepthRenderTarget();
	const auto& rc = hw.GetRenderControl();
	const auto& dc = hw.GetDepthControl();
	const auto& sc = hw.GetStencilControl();
	const auto& sm = hw.GetStencilMask();
	const auto& cc = hw.GetColorControl();

	DepthSurfaceLayout layout {};

	bool neo        = Config::IsNeo();
	bool htile      = z.z_info.tile_surface_enable;
	bool decompress = htile && (rc.depth_compress_disable || rc.stencil_compress_disable);

	if (dc.z_enable || dc.z_write_enable || dc.stencil_enable || decompress)
	{
		switch (z.z_info.format * 2 + z.stencil_info.format)
		{
			case 0: r->format = VK_FORMAT_UNDEFINED; break;
			case 1: /*r->format = VK_FORMAT_S8_UINT*/ EXIT("Unsupported format: VK_FORMAT_S8_UINT\n"); break;
			case 2: r->format = VK_FORMAT_D16_UNORM; break;
			case 3:
				// Preserve the established format on existing platforms. Windows may
				// use D32/S8 when its selected Vulkan driver lacks D24/S8.
			#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
				EXIT_IF(g_render_ctx == nullptr || g_render_ctx->GetGraphicCtx() == nullptr);
				r->format = g_render_ctx->GetGraphicCtx()->guest_d24s8_format;
			#elif LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
				r->format = VK_FORMAT_D32_SFLOAT_S8_UINT;
			#else
				r->format = VK_FORMAT_D24_UNORM_S8_UINT;
			#endif
				break;
			case 6: r->format = VK_FORMAT_D32_SFLOAT; break;
			case 7: r->format = VK_FORMAT_D32_SFLOAT_S8_UINT; break;
			default: EXIT("unknown z and stencil format: %u, %u\n", z.z_info.format, z.stencil_info.format);
		}
	}

	if (r->format != VK_FORMAT_UNDEFINED)
	{
		EXIT_NOT_IMPLEMENTED(!ResolveDepthSurfaceLayout(z, neo, &layout));
	}

	r->htile                = htile;
	r->neo                  = neo;
	r->depth_buffer_size    = layout.depth.allocation.size;
	r->depth_buffer_vaddr   = layout.depth.address;
	r->depth_tile_swizzle   = layout.depth.swizzle;
	r->stencil_buffer_size  = layout.stencil.allocation.size;
	r->stencil_buffer_vaddr = layout.stencil.address;
	r->stencil_tile_swizzle = layout.stencil.swizzle;
	r->htile_buffer_size    = layout.htile.allocation.size;
	r->htile_buffer_vaddr   = layout.htile.address;
	r->htile_tile_swizzle   = layout.htile.swizzle;
	r->width                = z.width;
	r->height               = z.height;
	r->depth_clear_enable   = rc.depth_clear_enable;
	r->depth_clear_value    = hw.GetDepthClearValue();
	r->depth_test_enable    = dc.z_enable;
	r->depth_write_enable   = dc.z_write_enable;
	r->depth_compare_op     = static_cast<VkCompareOp>(dc.zfunc);

	r->depth_bounds_test_enable = dc.depth_bounds_enable;
	r->depth_min_bounds         = 0.0f;
	r->depth_max_bounds         = 0.0f;
	EXIT_NOT_IMPLEMENTED(r->depth_bounds_test_enable);

	r->stencil_clear_enable = rc.stencil_clear_enable;
	r->stencil_clear_value  = hw.GetStencilClearValue();
	// EXIT_NOT_IMPLEMENTED(r->stencil_clear_enable);

	EXIT_NOT_IMPLEMENTED(!ResolveRenderStencilState(dc, sc, sm, r));
	// EXIT_NOT_IMPLEMENTED(r->stencil_test_enable);

	r->vulkan_buffer = nullptr;

	if (r->format != VK_FORMAT_UNDEFINED)
	{
		bool sampled = ((z.stencil_info.format == 0 && z.z_info.tile_mode_index != 0) || z.stencil_info.texture_compatible_stencil);

		uint32_t depth_clear_raw = 0;
		static_assert(sizeof(depth_clear_raw) == sizeof(r->depth_clear_value));
		std::memcpy(&depth_clear_raw, &r->depth_clear_value, sizeof(depth_clear_raw));
		const bool uniform_shader = DepthSurfaceUniformZeroShaderProfile(hw.GetShaderRegisters());
		const bool uniform_zero_import = DepthSurfaceUniformZeroImportEnabled() && uniform_shader &&
		    DepthSurfaceUniformZeroProfile(z, neo, layout, rc, dc, sc, sm, depth_clear_raw, r->stencil_clear_value);
		const bool uniform_zero_reuse = uniform_zero_import ||
		    (DepthSurfaceUniformZeroImportEnabled() && uniform_shader &&
		     DepthSurfaceUniformZeroReuseProfile(z, neo, layout, rc, dc, sc, sm, depth_clear_raw, r->stencil_clear_value));
		r->uniform_depth_read_only = uniform_zero_reuse &&
		    DepthSurfaceUniformZeroReadOnlyProfile(z, neo, layout, rc, dc, sc, sm, hw.GetShaderRegisters(),
		        depth_clear_raw, r->stencil_clear_value);
		// ADDR5=0 changes the 32-byte selection inside a 64-byte word. It is
		// admitted only by the exact uniform-zero contract, rechecked on every
		// bind before the tracker or a cached native attachment can be used.
		if (z.depth_info.addr5_swizzle_mask == 0 && !uniform_zero_reuse)
		{
			EXIT("unsupported nonuniform depth addr5=0 profile\n");
		}
		DepthStencilBufferObject vulkan_buffer_info(r->format, r->width, r->height, htile, neo, sampled, uniform_zero_import,
		                                          uniform_zero_reuse, z.pitch_div8_minus1, z.slice_div64_minus1,
		                                          z.depth_info.addr5_swizzle_mask == 0);

		EXIT_NOT_IMPLEMENTED(z.z_info.tile_mode_index != 0 && r->depth_tile_swizzle != 0);
		EXIT_NOT_IMPLEMENTED(r->stencil_tile_swizzle != 0);
		EXIT_NOT_IMPLEMENTED(r->htile_tile_swizzle != 0);

		r->vaddr_num = 0;

		if (r->depth_buffer_size > 0)
		{
			r->vaddr[r->vaddr_num] = r->depth_buffer_vaddr;
			r->size[r->vaddr_num]  = r->depth_buffer_size;
			r->vaddr_num++;
		}

		if (r->stencil_buffer_size > 0)
		{
			r->vaddr[r->vaddr_num] = r->stencil_buffer_vaddr;
			r->size[r->vaddr_num]  = r->stencil_buffer_size;
			r->vaddr_num++;
		}

		if (r->htile_buffer_size > 0)
		{
			r->vaddr[r->vaddr_num] = r->htile_buffer_vaddr;
			r->size[r->vaddr_num]  = r->htile_buffer_size;
			r->vaddr_num++;
		}

		EXIT_NOT_IMPLEMENTED(r->vaddr_num == 0);

		ObserveHtileStorageWriteback(submit_id, buffer, hw, layout);
		ObserveStencilCpuBytes(submit_id, buffer, hw, layout);
		r->vulkan_buffer = static_cast<DepthStencilVulkanImage*>(
		    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), buffer, r->vaddr, r->size, r->vaddr_num, vulkan_buffer_info));

		EXIT_NOT_IMPLEMENTED(r->vulkan_buffer == nullptr);

		if ((cc.mode == 0 && cc.op == 0xCC) || (dc.z_enable || dc.z_write_enable))
		{
			r->vulkan_buffer->compressed = htile && !decompress;
		}
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
static bool UniformDepthZeroReadOnlyDrawContractTest()
{
	uint32_t admitted = 0, rejected = 0;
	for (const bool neo: {false, true})
	for (const uint32_t addr5: {0u, 1u})
	{
		HW::DepthRenderTarget z;
		z.z_info.format = 3; z.z_info.tile_mode_index = neo ? 2u : 0u;
		z.z_info.zrange_precision = 1; z.z_info.tile_surface_enable = true;
		z.stencil_info.format = 1; z.stencil_info.tile_mode_index = neo ? 2u : 0u;
		z.stencil_info.tile_split = neo ? 2u : 0u; z.stencil_info.tile_stencil_disable = true;
		z.depth_info.addr5_swizzle_mask = addr5; z.depth_info.array_mode = 4;
		z.depth_info.pipe_config = neo ? 0x12u : 0xcu; z.depth_info.bank_height = neo ? 1u : 2u;
		z.depth_info.macro_tile_aspect = neo ? 0u : 2u; z.depth_info.num_banks = neo ? 2u : 3u;
		z.htile_surface.raw = 8; z.htile_surface.preload = 1;
		z.z_read_base_addr = z.z_write_base_addr = 0x1000000u;
		z.stencil_read_base_addr = z.stencil_write_base_addr = 0x2000000u;
		z.htile_data_base_addr = 0x3000000u;
		// This complete footprint is table-qualified on both base and Neo.
		// The generic 960x540 allocation resolver is intentionally base-only.
		z.width = 1920; z.height = 1080;
		z.pitch_div8_minus1 = 255; z.height_div8_minus1 = 143; z.slice_div64_minus1 = 36863;
		DepthSurfaceLayout layout;
		if (!ResolveDepthSurfaceLayout(z, neo, &layout)) return false;
		HW::RenderControl rc;
		HW::DepthControl dc; dc.z_enable = true; dc.zfunc = 6; dc.raw = 0x62;
		HW::StencilControl sc;
		HW::StencilMask sm;
		HW::ShaderRegisters shader;
		const auto profile = [&](const HW::DepthRenderTarget& target, const HW::RenderControl& render,
		                         const HW::DepthControl& control, const HW::ShaderRegisters& registers) {
			return DepthSurfaceUniformZeroReadOnlyProfile(target, neo, layout, render, control, sc, sm, registers, 0, 0);
		};
		if (!profile(z, rc, dc, shader)) return false;
		++admitted;
		for (int mutation = 0; mutation != 13; ++mutation)
		{
			auto target = z; auto render = rc; auto control = dc; auto registers = shader;
			switch (mutation)
			{
				case 0: control.z_write_enable = true; control.raw |= 4u; break;
				case 1: control.stencil_enable = true; control.raw |= 1u; break;
				case 2: render.depth_clear_enable = true; render.raw = 1u; break;
				case 3: render.stencil_clear_enable = true; render.raw = 2u; break;
				case 4: registers.db_shader_control.shader_z_export_enable = true; break;
				case 5: registers.shader_z_format = 1; break;
				case 6: registers.db_shader_control.other_bits = 0x80000000u; break;
				case 7: control.unsupported_bits = 0x800u; control.raw |= 0x800u; break;
				case 8: control.raw |= 4u; break; // raw/decoded disagreement
				case 9: target.depth_info.unsupported_bits = 1; break;
				case 10: control.raw |= 0x80u; break; // raw/decoded backface disagreement
				case 11: render.raw = 0x80000000u; break;
				case 12: registers.db_shader_control.conservative_z_export_value = 1; break;
			}
			if (profile(target, render, control, registers)) return false;
			++rejected;
		}
		RenderDepthInfo depth; depth.format = VK_FORMAT_D32_SFLOAT_S8_UINT; depth.uniform_depth_read_only = true;
		PipelineStaticParameters parameters; parameters.with_depth = true;
		VulkanPipeline pipeline; pipeline.static_params = &parameters; pipeline.ps_depth_stencil_export_free = true;
		if (!UniformDepthZeroReadOnlyDraw(depth, &pipeline) || !UniformDepthZeroReadOnlyDraw(depth, &pipeline)) return false;
		++admitted;
		for (int mutation = 0; mutation != 9; ++mutation)
		{
			auto requested = depth; auto native = parameters; auto bound = pipeline; bound.static_params = &native;
			switch (mutation)
			{
				case 0: requested.uniform_depth_read_only = false; break;
				case 1: requested.depth_write_enable = true; break;
				case 2: requested.stencil_test_enable = true; break;
				case 3: requested.depth_clear_enable = true; break;
				case 4: requested.stencil_clear_enable = true; break;
				case 5: bound.ps_depth_stencil_export_free = false; break;
				case 6: native.depth_write_enable = true; break;
				case 7: native.stencil_test_enable = true; break;
				case 8: bound.static_params = nullptr; break;
			}
			if (UniformDepthZeroReadOnlyDraw(requested, &bound)) return false;
			++rejected;
		}
		if (!profile(z, rc, dc, shader) || !UniformDepthZeroReadOnlyDraw(depth, &pipeline)) return false;
		for (const bool backface: {false, true})
		{
			auto active = dc;
			active.stencil_enable = true; active.stencilfunc = 7; active.backface_enable = backface;
			active.stencilfunc_bf = 7; // retained BF comparison also when BACKFACE_ENABLE is off
			active.raw |= 1u | (7u << Pm4::DB_DEPTH_CONTROL_STENCILFUNC_SHIFT) |
			    (7u << Pm4::DB_DEPTH_CONTROL_STENCILFUNC_BF_SHIFT);
			if (backface) active.raw |= 0x80u;
			sm.stencil_mask = sm.stencil_mask_bf = 0xff;
			sm.stencil_writemask = sm.stencil_writemask_bf = 0xff;
			RenderDepthInfo stencil_depth = depth;
			PipelineStaticParameters stencil_native = parameters;
			PipelineDynamicParameters stencil_dynamic;
			stencil_dynamic.vk_dynamic_state_stencil_write_mask = true;
			VulkanPipeline bound = pipeline; bound.static_params = &stencil_native; bound.dynamic_params = &stencil_dynamic;
			const auto resolve = [&] {
				if (!ResolveRenderStencilState(active, sc, sm, &stencil_depth)) return false;
				stencil_native.stencil_test_enable = stencil_depth.stencil_test_enable;
				stencil_native.stencil_front = stencil_depth.stencil_static_front;
				stencil_native.stencil_back = stencil_depth.stencil_static_back;
				SetPipelineStencilDynamicState(&stencil_dynamic, stencil_depth);
				return true;
			};
			if (!profile(z, rc, active, shader) || !resolve() || !UniformDepthZeroReadOnlyDraw(stencil_depth, &bound)) return false;
			++admitted;
			// Native KEEP proves no writes independently of a dynamic write mask.
			bound.dynamic_params = nullptr;
			if (!UniformDepthZeroReadOnlyDraw(stencil_depth, &bound)) return false;
			bound.dynamic_params = &stencil_dynamic;
			for (int outcome = 0; outcome != 3; ++outcome)
			{
				if (outcome == 0) { sc.stencil_fail = 1; sc.stencil_fail_bf = backface ? 1 : 0; } // ZERO still writes
				if (outcome == 1) { sc.stencil_zpass = 3; sc.stencil_zpass_bf = backface ? 3 : 0; } // REPLACE zero still writes
				if (outcome == 2) { sc.stencil_zfail = 4; sc.stencil_zfail_bf = backface ? 4 : 0; }
				if (profile(z, rc, active, shader) || !resolve() || UniformDepthZeroReadOnlyDraw(stencil_depth, &bound))
				{
					std::fprintf(stderr, "[GFX_DEPTH_READ_ONLY_DRAW_TEST_FAILURE] line=%d stage=write-negative neo=%u addr5=%u backface=%u outcome=%d\n",
					    __LINE__, neo ? 1u : 0u, addr5, backface ? 1u : 0u, outcome);
					return false;
				}
				++rejected;
				sc = {};
			}
			// The same supported REPLACE operations are literal reads only when
			// both effective native masks are zero and were emitted for this draw.
			sc.stencil_zpass = 3; sc.stencil_zpass_bf = backface ? 3 : 0;
			sm.stencil_writemask = sm.stencil_writemask_bf = 0;
			if (!profile(z, rc, active, shader) || !resolve() || !UniformDepthZeroReadOnlyDraw(stencil_depth, &bound))
			{
				std::fprintf(stderr, "[GFX_DEPTH_READ_ONLY_DRAW_TEST_FAILURE] line=%d stage=masked-positive neo=%u addr5=%u backface=%u\n",
				    __LINE__, neo ? 1u : 0u, addr5, backface ? 1u : 0u);
				return false;
			}
			++admitted;
			for (int mutation = 0; mutation != 7; ++mutation)
			{
				auto current_depth = stencil_depth; auto native = stencil_native; auto dynamic = stencil_dynamic; auto current = bound;
				current.static_params = &native; current.dynamic_params = &dynamic;
				switch (mutation)
				{
					case 0: current.dynamic_params = nullptr; break;
					case 1: dynamic.vk_dynamic_state_stencil_write_mask = false; break;
					case 2: dynamic.stencil_front.writeMask = 0xff; break; // cached mask differs from current request
					case 3: dynamic.stencil_back.writeMask = 0xff; break;
					case 4: dynamic.stencil_front.writeMask = current_depth.stencil_dynamic_front.writeMask = 0xff; break;
					case 5: dynamic.stencil_back.reference++; break;
					case 6: native.stencil_back.passOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP; break;
				}
				if (UniformDepthZeroReadOnlyDraw(current_depth, &current))
				{
					std::fprintf(stderr, "[GFX_DEPTH_READ_ONLY_DRAW_TEST_FAILURE] line=%d stage=dynamic-negative neo=%u addr5=%u backface=%u mutation=%d\n",
					    __LINE__, neo ? 1u : 0u, addr5, backface ? 1u : 0u, mutation);
					return false;
				}
				++rejected;
			}
			sc = {}; sm = {};
		}
	}
	ShaderCode pixel; pixel.SetType(ShaderType::Pixel);
	ShaderInstruction export_instruction; export_instruction.type = ShaderInstructionType::Exp;
	export_instruction.format = ShaderInstructionFormat::Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone;
	export_instruction.export_target = 0;
	pixel.GetInstructions().Add(export_instruction);
	if (!PixelShaderDepthStencilExportFree(pixel)) return false;
	pixel.GetInstructions()[0].export_target = 8;
	if (PixelShaderDepthStencilExportFree(pixel)) return false;
	pixel.GetInstructions()[0].export_target = 0;
	pixel.GetInstructions()[0].format = ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done;
	if (PixelShaderDepthStencilExportFree(pixel)) return false;
	pixel.SetPsEmbedded(true); pixel.SetPsEmbeddedId(0);
	if (!PixelShaderDepthStencilExportFree(pixel)) return false;
	pixel.SetPsEmbeddedId(1);
	if (PixelShaderDepthStencilExportFree(pixel)) return false;
	pixel.SetPsEmbeddedId(0); pixel.SetType(ShaderType::Vertex);
	if (PixelShaderDepthStencilExportFree(pixel)) return false;
	std::fprintf(stderr, "[GFX_DEPTH_READ_ONLY_DRAW_TEST] admitted=%u rejected=%u base/Neo/ADDR5=checked"
	    " raw/resolved/native-writes/clears/exports/unknown=reject immutable-pixel-module=checked"
	    " cached-pipeline-does-not-replace-current-bind-proof=checked gpu-hardware=unqualified\n", admitted, rejected);
	return true;
}

bool GraphicsRenderInactiveD16DescriptorContractTest()
{
	if (!UniformDepthZeroReadOnlyDrawContractTest()) return false;
	uint32_t admitted = 0;
	uint32_t rejected = 0;
	for (const bool neo: {false, true})
	{
		HW::Context hw;
		HW::DepthRenderTarget z;
		z.z_info.format = 1;
		z.z_info.tile_surface_enable = true;
		z.depth_info.array_mode = 4;
		z.depth_info.pipe_config = neo ? 0x12u : 0xcu;
		z.depth_info.bank_height = neo ? 1u : 2u;
		z.depth_info.macro_tile_aspect = neo ? 0u : 2u;
		z.depth_info.num_banks = neo ? 2u : 3u;
		z.htile_surface.raw = 8;
		z.htile_surface.preload = 1;
		// Deliberately unmapped addresses: inactive DB state must never reach
		// mapping/lease, object creation, GPU commands or guest content access.
		z.z_read_base_addr = z.z_write_base_addr = 0x1000u;
		z.htile_data_base_addr = 0x3000u;
		z.width = 960; z.height = 540;
		z.pitch_div8_minus1 = 127; z.height_div8_minus1 = 79; z.slice_div64_minus1 = 10239;
		hw.SetDepthRenderTarget(z);
		const auto accept = [&](const HW::Context& test)
		{
			if (!IsInactiveD16DepthDescriptorSupported(test, neo)) return false;
			RenderDepthInfo depth;
			FindRenderDepthInfo(0, nullptr, test, &depth);
			if (depth.format != VK_FORMAT_UNDEFINED || depth.vulkan_buffer != nullptr || depth.vaddr_num != 0 ||
			    depth.depth_buffer_size != 0 || depth.depth_buffer_vaddr != 0 || depth.depth_tile_swizzle != 0 ||
			    depth.stencil_buffer_size != 0 || depth.stencil_buffer_vaddr != 0 || depth.stencil_tile_swizzle != 0 ||
			    depth.htile_buffer_size != 0 || depth.htile_buffer_vaddr != 0 || depth.htile_tile_swizzle != 0 ||
			    depth.depth_test_enable || depth.depth_write_enable || depth.depth_bounds_test_enable ||
			    depth.stencil_test_enable || depth.depth_clear_enable || depth.stencil_clear_enable)
				return false;
			for (uint32_t i = 0; i < 3; ++i) if (depth.vaddr[i] != 0 || depth.size[i] != 0) return false;
			++admitted;
			return true;
		};
		const auto reject = [&](const HW::Context& test)
		{
			if (IsInactiveD16DepthDescriptorSupported(test, neo)) return false;
			++rejected;
			return true;
		};
		if (!accept(hw)) return false;
		for (const uint32_t addr5: {0u, 1u})
		for (const bool htile: {false, true})
		for (const uint32_t z_behavior: {0u, 1u})
		for (const uint32_t zrange: {0u, 1u})
		for (const bool stencil_tile_disable: {false, true})
		{
			auto test = hw;
			auto target = z;
			target.depth_info.addr5_swizzle_mask = addr5;
			target.z_info.zrange_precision = zrange;
			target.stencil_info.tile_stencil_disable = stencil_tile_disable;
			target.z_info.tile_surface_enable = htile;
			target.htile_data_base_addr = htile ? 0x3000u : 0u;
			target.htile_surface.raw = htile ? 8u : 0u;
			target.htile_surface.preload = htile ? 1u : 0u;
			test.SetDepthRenderTarget(target);
			auto shader = test.GetShaderRegisters().db_shader_control;
			shader.shader_z_behavior = static_cast<uint8_t>(z_behavior);
			test.SetDepthShaderControl(shader);
			if (!accept(test)) return false;
		}
		for (const auto& shape: {std::array<uint32_t, 4> {13, 7, 16, 8}, std::array<uint32_t, 4> {128, 127, 256, 128}})
		{
			auto test = hw; auto target = z;
			target.width = shape[0]; target.height = shape[1];
			target.pitch_div8_minus1 = shape[2] / 8u - 1u;
			target.height_div8_minus1 = shape[3] / 8u - 1u;
			target.slice_div64_minus1 = shape[2] * shape[3] / 64u - 1u;
			test.SetDepthRenderTarget(target);
			if (!accept(test)) return false;
		}
		for (const uint8_t comparison: {uint8_t {0}, uint8_t {7}})
		{
			auto test = hw; auto control = test.GetDepthControl();
			control.zfunc = control.stencilfunc = control.stencilfunc_bf = comparison;
			control.raw = (static_cast<uint32_t>(comparison) << 4u) |
			              (static_cast<uint32_t>(comparison) << 8u) | (static_cast<uint32_t>(comparison) << 20u);
			test.SetDepthControl(control);
			if (!accept(test)) return false;
		}
		using DepthMutation = void (*)(HW::DepthRenderTarget&);
		const DepthMutation metadata_mutations[] = {
		    [](auto& t) { t.z_info.format = 0; }, [](auto& t) { t.z_info.format = 2; },
		    [](auto& t) { t.z_info.format = 3; }, [](auto& t) { t.stencil_info.format = 1; },
		    [](auto& t) { t.z_info.tile_mode_index = 1; }, [](auto& t) { t.z_info.num_samples = 1; },
		    [](auto& t) { t.z_info.zrange_precision = 2; }, [](auto& t) { t.z_info.expclear_enabled = true; },
		    [](auto& t) { t.z_info.embedded_sample_locations = true; }, [](auto& t) { t.z_info.partially_resident = true; },
		    [](auto& t) { t.z_info.num_mip_levels = 1; }, [](auto& t) { t.z_info.plane_compression = 1; },
		    [](auto& t) { t.stencil_info.tile_mode_index = 1; }, [](auto& t) { t.stencil_info.tile_split = 1; },
		    [](auto& t) { t.stencil_info.expclear_enabled = true; },
		    [](auto& t) { t.stencil_info.texture_compatible_stencil = true; }, [](auto& t) { t.stencil_info.partially_resident = true; },
		    [](auto& t) { t.depth_view.slice_start = 1; }, [](auto& t) { t.depth_view.slice_max = 1; },
		    [](auto& t) { t.depth_view.current_mip_level = 1; }, [](auto& t) { t.depth_view.depth_write_disable = true; },
		    [](auto& t) { t.depth_view.stencil_write_disable = true; }, [](auto& t) { t.depth_info.addr5_swizzle_mask = 2; },
		    [](auto& t) { t.depth_info.array_mode = 0; }, [](auto& t) { t.depth_info.pipe_config = 0; },
		    [](auto& t) { t.depth_info.bank_width = 1; }, [](auto& t) { t.depth_info.bank_height = 3; },
		    [](auto& t) { t.depth_info.macro_tile_aspect = 3; }, [](auto& t) { t.depth_info.num_banks = 0; },
		    [](auto& t) { t.htile_surface.raw = 0x80000008u; }, [](auto& t) { t.htile_surface.linear = 1; },
		    [](auto& t) { t.htile_surface.full_cache = 1; }, [](auto& t) { t.htile_surface.htile_uses_preload_win = 1; },
		    [](auto& t) { t.htile_surface.preload = 0; }, [](auto& t) { t.htile_surface.prefetch_width = 1; },
		    [](auto& t) { t.htile_surface.prefetch_height = 1; }, [](auto& t) { t.htile_surface.dst_outside_zero_to_one = 1; },
		    [](auto& t) { t.z_read_base_addr = 0; t.z_write_base_addr = 0; }, [](auto& t) { t.z_write_base_addr += 256; },
		    [](auto& t) { t.stencil_read_base_addr = 0x4000u; }, [](auto& t) { t.stencil_write_base_addr = 0x4000u; },
		    [](auto& t) { t.htile_data_base_addr = 0; }, [](auto& t) { t.z_info.tile_surface_enable = false; },
		    [](auto& t) { t.width = 0; }, [](auto& t) { t.height = 0; },
		    [](auto& t) { t.width = 1025; }, [](auto& t) { t.height = 641; },
		    [](auto& t) { t.pitch_div8_minus1 = UINT32_MAX; }, [](auto& t) { t.height_div8_minus1 = UINT32_MAX; },
		    [](auto& t) { ++t.slice_div64_minus1; }, [](auto& t) { t.slice_div64_minus1 = UINT32_MAX; },
		};
		for (const auto mutate: metadata_mutations)
		{
			auto test = hw; auto target = z; mutate(target); test.SetDepthRenderTarget(target);
			if (!reject(test)) return false;
		}
		for (uint32_t bit = 0; bit < 32; ++bit)
		{
			const auto omitted = uint32_t {1} << bit;
			for (uint32_t field = 0; field < 5; ++field)
			{
				auto test = hw; auto target = z;
				switch (field)
				{
					case 0: target.z_info.unsupported_bits = omitted; break;
					case 1: target.stencil_info.unsupported_bits = omitted; break;
					case 2: target.depth_info.unsupported_bits = omitted; break;
					case 3: target.depth_size_unsupported_bits = omitted; break;
					case 4: target.depth_slice_unsupported_bits = omitted; break;
				}
				test.SetDepthRenderTarget(target);
				if (!reject(test)) return false;
			}
			auto test = hw; auto control = test.GetDepthControl();
			control.unsupported_bits = omitted; test.SetDepthControl(control);
			if (!reject(test)) return false;
			control.unsupported_bits = 0; control.raw = omitted; test.SetDepthControl(control);
			if (!reject(test)) return false;
			auto shader = test.GetShaderRegisters().db_shader_control;
			shader.other_bits = omitted; test = hw; test.SetDepthShaderControl(shader);
			if (!reject(test)) return false;
		}
		bool HW::DepthControl::* const depth_consumers[] = {
		    &HW::DepthControl::z_enable, &HW::DepthControl::z_write_enable, &HW::DepthControl::stencil_enable,
		    &HW::DepthControl::depth_bounds_enable, &HW::DepthControl::backface_enable,
		    &HW::DepthControl::color_writes_on_depth_fail_enable, &HW::DepthControl::color_writes_on_depth_pass_disable,
		};
		for (const auto field: depth_consumers)
		{
			auto test = hw; auto control = test.GetDepthControl(); control.*field = true; test.SetDepthControl(control);
			if (!reject(test)) return false;
		}
		bool HW::RenderControl::* const render_consumers[] = {
		    &HW::RenderControl::depth_clear_enable, &HW::RenderControl::stencil_clear_enable, &HW::RenderControl::resummarize_enable,
		    &HW::RenderControl::depth_compress_disable, &HW::RenderControl::stencil_compress_disable, &HW::RenderControl::copy_centroid,
		};
		for (const auto field: render_consumers)
		{
			auto test = hw; auto control = test.GetRenderControl(); control.*field = true; test.SetRenderControl(control);
			if (!reject(test)) return false;
		}
		for (const uint32_t raw: {1u, 0x80000000u})
		{
			auto test = hw; auto control = test.GetRenderControl(); control.raw = raw; test.SetRenderControl(control);
			if (!reject(test)) return false;
		}
		{
			auto test = hw; auto control = test.GetRenderControl(); control.copy_sample = 1; test.SetRenderControl(control);
			if (!reject(test)) return false;
		}
		{
			auto test = hw; test.SetShaderZFormat(1); if (!reject(test)) return false;
			auto shader = hw.GetShaderRegisters().db_shader_control;
			shader.shader_z_export_enable = true; test = hw; test.SetDepthShaderControl(shader); if (!reject(test)) return false;
			shader = hw.GetShaderRegisters().db_shader_control;
			shader.conservative_z_export_value = 1; test = hw; test.SetDepthShaderControl(shader); if (!reject(test)) return false;
			shader = hw.GetShaderRegisters().db_shader_control;
			shader.shader_z_behavior = 2; test = hw; test.SetDepthShaderControl(shader); if (!reject(test)) return false;
		}
		{
			auto test = hw; auto stencil = test.GetStencilControl(); stencil.stencil_fail_bf = 1; test.SetStencilControl(stencil);
			if (!reject(test)) return false;
			test = hw; auto mode = test.GetModeControl(); mode.cull_back = true; mode.poly_offset_front_enable = true;
			test.SetModeControl(mode); test.SetPolygonOffsetRegister(2, 0x41800000u); // active slope 16.0
			if (!reject(test)) return false;
			test.SetPolygonOffsetRegister(2, 0u); if (!accept(test)) return false;
		}
		{
			// The retained combination can appear after an earlier stencil pass.
			// Replay its fully known inert control, then reactivate each consumer
			// with matching raw/decoded bits; the expanded metadata domain must
			// not admit either that activation or newly opaque state.
			auto observed = hw; auto target = z;
			target.z_info.zrange_precision = 1;
			target.stencil_info.tile_stencil_disable = true;
			observed.SetDepthRenderTarget(target);
			auto control = observed.GetDepthControl(); control.zfunc = 7; control.raw = 0x70u;
			observed.SetDepthControl(control);
			auto shader = observed.GetShaderRegisters().db_shader_control; shader.shader_z_behavior = 1;
			observed.SetDepthShaderControl(shader);
			if (!accept(observed)) return false;
			const uint32_t depth_bits[] = {2u, 4u, 1u, 8u, 0x80u, 0x40000000u, 0x80000000u};
			for (uint32_t i = 0; i < std::size(depth_consumers); ++i)
			{
				auto test = observed; auto active = control;
				active.*depth_consumers[i] = true; active.raw |= depth_bits[i]; test.SetDepthControl(active);
				if (!reject(test)) return false;
			}
			const uint32_t render_bits[] = {1u, 2u, 16u, 64u, 32u, 128u};
			for (uint32_t i = 0; i < std::size(render_consumers); ++i)
			{
				auto test = observed; auto active = test.GetRenderControl();
				active.*render_consumers[i] = true; active.raw = render_bits[i]; test.SetRenderControl(active);
				if (!reject(test)) return false;
			}
			auto test = observed; auto render = test.GetRenderControl(); render.copy_sample = 1; render.raw = 0x100u;
			test.SetRenderControl(render); if (!reject(test)) return false;
			test = observed; auto opaque = control; opaque.raw |= 0x800u; opaque.unsupported_bits = 0x800u;
			test.SetDepthControl(opaque); if (!reject(test)) return false;
			test = observed; render = test.GetRenderControl(); render.raw = 0x80000000u;
			test.SetRenderControl(render); if (!reject(test)) return false;
			test = observed; auto exporting = shader; exporting.shader_z_export_enable = true;
			test.SetDepthShaderControl(exporting); if (!reject(test)) return false;
			test = observed; test.SetShaderZFormat(1); if (!reject(test)) return false;
			test = observed; exporting = shader; exporting.other_bits = 0x80000000u;
			test.SetDepthShaderControl(exporting); if (!reject(test)) return false;
			test = observed; auto with_stencil = target; with_stencil.stencil_info.format = 1;
			test.SetDepthRenderTarget(with_stencil); if (!reject(test)) return false;
			test = observed; auto opaque_target = target; opaque_target.z_info.unsupported_bits = 1;
			test.SetDepthRenderTarget(opaque_target); if (!reject(test)) return false;
		}
		if (!accept(hw)) return false; // rejected neighbors cannot mutate the admitted original
	}
	std::fprintf(stderr, "[GFX_DEPTH_D16_INACTIVE_CONTRACT_TEST] base/Neo=checked admitted=%u rejected=%u"
	                     " attachment/ranges=none unmapped-addresses=not-accessed consumers/shader/unknown=fail-closed"
	                     " content/native-D16/hardware-PS4=unqualified\n", admitted, rejected);
	return true;
}
#endif

static void FindRenderColorInfo(uint64_t submit_id, CommandBuffer* buffer, const HW::Context& hw, RenderColorInfo* r)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(r == nullptr);

	const auto& rt   = hw.GetRenderTarget(0);
	auto        mask = hw.GetRenderTargetMask();

	if (rt.base.addr == 0 || mask == 0)
	{
		// No color output
		r->type          = RenderColorType::NoColorOutput;
		r->base_addr     = 0;
		r->vulkan_buffer = nullptr;
		r->buffer_size   = 0;
		return;
	}

	const auto ps4_descriptor = BuildPs4ColorRenderTargetDescriptor(rt.info.format, rt.info.channel_type, rt.info.channel_order,
	                                                               rt.slice.slice_div64_minus1);
	const auto format_info = ps4_descriptor.format;
	if (!format_info.IsSupported())
	{
		EXIT("%s\n unknown format\n", rt_print("RenderTarget", rt).Concat(U"").C_Str());
	}

	uint32_t width      = 0;
	uint32_t height     = 0;
	uint32_t pitch      = 0;
	uint64_t size       = 0;
	bool     tile       = false;
	bool     write_back = false;

	switch (rt.attrib.tile_mode)
	{
		case 0x8:
			tile       = false;
			write_back = false;
			break;

		case 0x1f:
			tile       = false;
			write_back = true;
			break;

		case 0xa:
		case 0xd:
		case 0xe:
			tile       = true;
			write_back = false;
			break;

		default: EXIT("unknown tile mode: %u\n", rt.attrib.tile_mode);
	}

	width  = rt.size.width;
	height = rt.size.height;
	pitch  = (rt.pitch.pitch_div8_minus1 + 1) * 8;
	size   = ps4_descriptor.byte_size;
	EXIT_NOT_IMPLEMENTED(size == 0);
	if (rt.attrib.tile_mode == 10)
	{
		write_back = RenderTextureObject::SupportsDisplay2dReadback(format_info.format, width, height, pitch,
		                                                          rt.info.neo_mode, rt.base.addr, size);
	}

	VideoOut::VideoOutImageLookupIdentity videoout_lookup {};
	const bool trace_videoout = VideoOut::VideoOutLifecycleTraceEnabled();
	if (trace_videoout)
	{
		videoout_lookup.kind             = VideoOut::VideoOutImageLookupKind::RenderTarget;
		videoout_lookup.command_buffer   = buffer;
		videoout_lookup.submit_id        = submit_id;
		videoout_lookup.requested_size   = size;
		videoout_lookup.requested_pitch  = pitch;
		videoout_lookup.requested_width  = width;
		videoout_lookup.requested_height = height;
		if (buffer != nullptr)
		{
			videoout_lookup.recording_id                   = buffer->GetRecordingId();
			videoout_lookup.command_ordinal                = buffer->GetTrackedCommandOrdinal();
			videoout_lookup.submission_session_epoch       = buffer->GetSubmissionSessionEpoch();
			videoout_lookup.recording_submit_serial        = buffer->GetRecordingSubmitSerial();
			videoout_lookup.begin_submit_serial_cutoff     = buffer->GetBeginSubmitSerialCutoff();
			videoout_lookup.begin_completed_serial_cutoff  = buffer->GetBeginCompletedSerialCutoff();
			videoout_lookup.queue                          = buffer->GetQueue();
		}
	}
	auto video_image       = VideoOut::VideoOutGetImage(rt.base.addr, trace_videoout ? &videoout_lookup : nullptr);
	bool render_to_texture = (video_image.image == nullptr);
	VideoOutVulkanImage* pinned_video_image = nullptr;
	if (!render_to_texture)
	{
		EXIT_NOT_IMPLEMENTED(video_image.buffer_size != size);
		EXIT_NOT_IMPLEMENTED(video_image.buffer_pitch != pitch);
		void* pinned_host = nullptr;
		const bool pinned = PinVideoOutForRendererCommand(
		    buffer, video_image.tracked_handle, rt.base.addr, video_image.buffer_size, &pinned_host);
		if (!pinned || pinned_host == nullptr || pinned_host != video_image.image)
		{
			VideoOut::VideoOutDumpLifecycleTrace("render-target-lifetime-unavailable");
			EXIT("VideoOut render target lifetime unavailable\n");
		}
		pinned_video_image = static_cast<VideoOutVulkanImage*>(pinned_host);
	}

	if (render_to_texture)
	{
		// Render to texture
		RenderTextureObject vulkan_buffer_info(format_info.format, width, height, tile, rt.info.neo_mode, pitch, write_back,
		                                               rt.attrib.tile_mode);
		auto*               buffer_vulkan = static_cast<Graphics::RenderTextureVulkanImage*>(
		    Graphics::GpuMemoryCreateObjectTracked(submit_id, g_render_ctx->GetGraphicCtx(), buffer, rt.base.addr, size,
		                                           vulkan_buffer_info, &r->tracked_handle));
		EXIT_NOT_IMPLEMENTED(buffer_vulkan == nullptr);
		r->type          = RenderColorType::RenderTexture;
		r->base_addr     = rt.base.addr;
		r->vulkan_buffer = buffer_vulkan;
		r->buffer_size   = size;
	} else
	{
		EXIT_NOT_IMPLEMENTED(!(rt.info.format == 0xa && (rt.info.channel_type == 0x6 || rt.info.channel_type == 0x0) &&
		                       (rt.info.channel_order == 0x0 || rt.info.channel_order == 0x1)));

		// Display buffer
		r->type           = RenderColorType::DisplayBuffer;
		r->base_addr      = rt.base.addr;
		r->vulkan_buffer  = pinned_video_image;
		r->tracked_handle = video_image.tracked_handle;
		r->buffer_size    = video_image.buffer_size;
	}
}

static void InvalidateMemoryObject(CommandBuffer* buffer, const RenderColorInfo& r, bool draw_recorded)
{
	bool with_color = (r.vulkan_buffer != nullptr);

	if (with_color)
	{
		if (r.type == RenderColorType::RenderTexture)
		{
			const bool recorded = GpuMemoryRecordRenderTexturePass(buffer, r.tracked_handle, r.base_addr, r.buffer_size,
			                                                       draw_recorded);
			EXIT_NOT_IMPLEMENTED(!recorded);
		} else if (r.type == RenderColorType::DisplayBuffer /*|| r.type == RenderColorType::OffscreenBuffer*/)
		{
			EXIT_NOT_IMPLEMENTED(!GpuMemoryRecordVideoOutBufferPass(buffer, r.tracked_handle, r.base_addr,
			                                                       r.buffer_size, draw_recorded));
		}
	}
}

static bool UniformDepthZeroReadOnlyDraw(const RenderDepthInfo& depth, const VulkanPipeline* pipeline)
{
	if (!depth.uniform_depth_read_only || depth.format != VK_FORMAT_D32_SFLOAT_S8_UINT ||
	    depth.depth_write_enable || depth.depth_bounds_test_enable || depth.depth_clear_enable || depth.stencil_clear_enable ||
	    pipeline == nullptr || !pipeline->ps_depth_stencil_export_free || pipeline->static_params == nullptr ||
	    !pipeline->static_params->with_depth || pipeline->static_params->depth_write_enable ||
	    pipeline->static_params->depth_bounds_test_enable ||
	    pipeline->static_params->stencil_test_enable != depth.stencil_test_enable) return false;
	if (!depth.stencil_test_enable) return true;
	const auto same_static = [](const PipelineStencilStaticState& a, const PipelineStencilStaticState& b) {
		return a.failOp == b.failOp && a.passOp == b.passOp && a.depthFailOp == b.depthFailOp && a.compareOp == b.compareOp;
	};
	if (!same_static(depth.stencil_static_front, pipeline->static_params->stencil_front) ||
	    !same_static(depth.stencil_static_back, pipeline->static_params->stencil_back)) return false;
	const auto all_keep = [](const PipelineStencilStaticState& face) {
		return face.failOp == VK_STENCIL_OP_KEEP && face.passOp == VK_STENCIL_OP_KEEP && face.depthFailOp == VK_STENCIL_OP_KEEP;
	};
	const bool front_keep = all_keep(pipeline->static_params->stencil_front);
	const bool back_keep = all_keep(pipeline->static_params->stencil_back);
	if (front_keep && back_keep) return true;
	// A zero current write mask is effective only when SetDynamicParams emitted
	// it for this draw. Cache hits copy the current dynamic states before that
	// call; neither a zero requested mask nor an old cached mask is sufficient.
	if (pipeline->dynamic_params == nullptr || !pipeline->dynamic_params->vk_dynamic_state_stencil_write_mask) return false;
	const auto same_dynamic = [](const PipelineStencilDynamicState& a, const PipelineStencilDynamicState& b) {
		return a.writeMask == b.writeMask && a.compareMask == b.compareMask && a.reference == b.reference;
	};
	return same_dynamic(depth.stencil_dynamic_front, pipeline->dynamic_params->stencil_front) &&
	       same_dynamic(depth.stencil_dynamic_back, pipeline->dynamic_params->stencil_back) &&
	       (front_keep || pipeline->dynamic_params->stencil_front.writeMask == 0) &&
	       (back_keep || pipeline->dynamic_params->stencil_back.writeMask == 0);
}

static void InvalidateMemoryObject(CommandBuffer* buffer, uint64_t submit_id, const RenderDepthInfo& r,
                                  const VulkanPipeline* pipeline, bool draw_recorded)
{
	bool with_depth = (r.format != VK_FORMAT_UNDEFINED && r.vulkan_buffer != nullptr);

	if (with_depth)
	{
		const bool read_only = UniformDepthZeroReadOnlyDraw(r, pipeline);
		// Attachment load clears write even when the following draw has no
		// vertices. Record the actual image and its current planes before
		// invalidating their guest hashes.
		const bool potential_write = (draw_recorded && !read_only) || r.depth_clear_enable || r.stencil_clear_enable;
		EXIT_NOT_IMPLEMENTED(!GpuMemoryRecordDepthStencilPass(buffer, r.vulkan_buffer, r.vaddr, r.size,
		                                                     r.vaddr_num, potential_write, read_only));
		GpuMemoryResetHash(r.vaddr, r.size, r.vaddr_num, GpuMemoryObjectType::DepthStencilBuffer, buffer, submit_id,
		                   read_only);
	}
}

bool GraphicsRenderGpuR8CaptureEnabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_GFX_RESOURCE_CAPTURE_GPU_R8");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

bool GraphicsRenderGpuRgba8MipsCaptureEnabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_GFX_RESOURCE_CAPTURE_GPU_RGBA8_MIPS");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

static constexpr uint64_t GPU_READBACK_IMAGE_CAP = 1024 * 1024;
static constexpr uint64_t GPU_READBACK_TOTAL_CAP = 8 * GPU_READBACK_IMAGE_CAP;
static constexpr uint32_t GPU_READBACK_JOB_CAP = 8;
static constexpr uint32_t GPU_RGBA8_MIP_CAP = 15;

struct GpuR8CaptureShape
{
	bool recording = false;
	bool pixel_stage = false;
	bool sampled_read_only = false;
	uint32_t format = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t descriptor_width = 0;
	uint32_t descriptor_height = 0;
	uint32_t type = 0;
	uint32_t base_level = 0;
	uint32_t last_level = 0;
	uint32_t host_levels = 0;
	uint32_t depth = 0;
	uint32_t base_array = 0;
	uint32_t last_array = 0;
	uint32_t layout = 0;
	bool transfer_source = false;
};

static const char* GpuR8CaptureRejection(const GpuR8CaptureShape& shape)
{
	if (!shape.recording) { return "command_state"; }
	if (!shape.pixel_stage || !shape.sampled_read_only) { return "stage_or_access"; }
	if (shape.format != VK_FORMAT_R8_UNORM || !shape.transfer_source) { return "format_or_usage"; }
	if (shape.type != ShaderTextureResource::TYPE_2D || shape.depth != 0 || shape.base_array != 0 || shape.last_array != 0)
	{
		return "array_or_dimension";
	}
	if (shape.base_level != 0 || shape.last_level != 0 || shape.host_levels != 1) { return "mip_window"; }
	if (shape.layout != VK_IMAGE_LAYOUT_GENERAL && shape.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	{
		return "layout";
	}
	if (shape.width == 0 || shape.height == 0 || shape.width != shape.descriptor_width || shape.height != shape.descriptor_height ||
	    uint64_t(shape.width) * shape.height > GPU_READBACK_IMAGE_CAP)
	{
		return "extent";
	}
	return nullptr;
}

struct GpuRgba8MipLayout
{
	uint32_t levels = 0;
	uint64_t total_bytes = 0;
	uint64_t offsets[GPU_RGBA8_MIP_CAP] = {};
	uint32_t widths[GPU_RGBA8_MIP_CAP] = {};
	uint32_t heights[GPU_RGBA8_MIP_CAP] = {};
	uint32_t bytes[GPU_RGBA8_MIP_CAP] = {};
};

static bool BuildGpuRgba8MipLayout(uint32_t width, uint32_t height, uint32_t levels, GpuRgba8MipLayout* output)
{
	if (output == nullptr || width == 0 || height == 0 || levels < 2 || levels > GPU_RGBA8_MIP_CAP) { return false; }
	uint32_t max_levels = 1;
	for (uint32_t dimension = std::max(width, height); dimension > 1; dimension >>= 1u) { ++max_levels; }
	if (levels > max_levels) { return false; }
	GpuRgba8MipLayout result {};
	result.levels = levels;
	for (uint32_t level = 0; level < levels; ++level)
	{
		const uint32_t mip_width = std::max(width >> level, 1u);
		const uint32_t mip_height = std::max(height >> level, 1u);
		const uint64_t mip_pixels = uint64_t(mip_width) * mip_height;
		if (mip_pixels == 0 || mip_pixels > GPU_READBACK_IMAGE_CAP / 4u) { return false; }
		const uint64_t mip_bytes = mip_pixels * 4u;
		if (mip_bytes > UINT32_MAX || result.total_bytes > GPU_READBACK_IMAGE_CAP ||
		    mip_bytes > GPU_READBACK_IMAGE_CAP - result.total_bytes)
		{
			return false;
		}
		result.offsets[level] = result.total_bytes;
		result.widths[level] = mip_width;
		result.heights[level] = mip_height;
		result.bytes[level] = static_cast<uint32_t>(mip_bytes);
		result.total_bytes += mip_bytes;
	}
	*output = result;
	return true;
}

static const char* GpuRgba8MipsCaptureRejection(const GpuR8CaptureShape& shape, GpuRgba8MipLayout* layout)
{
	if (!shape.recording) { return "command_state"; }
	if (!shape.pixel_stage || !shape.sampled_read_only) { return "stage_or_access"; }
	if (shape.format != VK_FORMAT_R8G8B8A8_UNORM || !shape.transfer_source) { return "format_or_usage"; }
	if (shape.type != ShaderTextureResource::TYPE_2D || shape.depth != 0 || shape.base_array != 0 || shape.last_array != 0)
	{
		return "array_or_dimension";
	}
	if (shape.base_level != 0 || shape.host_levels < 2 || shape.host_levels > GPU_RGBA8_MIP_CAP ||
	    shape.last_level != shape.host_levels - 1)
	{
		return "mip_window";
	}
	if (shape.layout != VK_IMAGE_LAYOUT_GENERAL && shape.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	{
		return "layout";
	}
	if (shape.width == 0 || shape.height == 0 || shape.width != shape.descriptor_width ||
	    shape.height != shape.descriptor_height || !BuildGpuRgba8MipLayout(shape.width, shape.height, shape.host_levels, layout))
	{
		return "extent";
	}
	return nullptr;
}

static bool GpuReadbackBudgetAllowed(uint32_t jobs, uint64_t consumed, uint64_t bytes)
{
	return jobs < GPU_READBACK_JOB_CAP && consumed <= GPU_READBACK_TOTAL_CAP && bytes != 0 &&
	       bytes <= GPU_READBACK_IMAGE_CAP && bytes <= GPU_READBACK_TOTAL_CAP - consumed;
}

struct GpuR8Readback
{
	VkBuffer buffer = nullptr;
	VkDeviceMemory memory = nullptr;
	uint32_t job = 0;
	uint32_t event = 0;
	int stage = 0;
	int slot = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint64_t bytes = 0;
	uint64_t image_id = 0;
	uint64_t recording_id = 0;
	uint64_t submit_serial = 0;
	VkImageLayout original_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	uint32_t descriptor[8] = {};
	String binary_path;
	String status_path;
};

static void WriteGpuR8ReadbackStatus(const GpuR8Readback& job, const char* status, bool completed, uint32_t written, uint32_t hash)
{
	Core::File receipt;
	if (!receipt.Create(job.status_path)) { return; }
	receipt.Printf("gpu_r8 version=1 job=%u event=%u stage=%d slot=%d state=%s completed=%u"
	               " encoding=r8_unorm_linear underlying_components=R001 snapshot=first_use"
	               " image_unique_id=%" PRIu64 " width=%u height=%u expected_bytes=%" PRIu64
	               " written_bytes=%u hash=%08x recording_id=%" PRIu64 " submit_serial=%" PRIu64 " original_layout=%u\n",
	               job.job, job.event, job.stage, job.slot, status, completed ? 1u : 0u, job.image_id, job.width, job.height,
	               job.bytes, written, hash, job.recording_id, job.submit_serial, static_cast<unsigned>(job.original_layout));
	receipt.Printf("descriptor");
	for (const auto field: job.descriptor) { receipt.Printf(" %08x", field); }
	receipt.Printf("\n");
	receipt.Close();
}

static void DeleteGpuR8Readback(GraphicContext* ctx, GpuR8Readback* job)
{
	if (job->buffer != nullptr) { vkDestroyBuffer(ctx->device, job->buffer, nullptr); }
	if (job->memory != nullptr) { vkFreeMemory(ctx->device, job->memory, nullptr); }
	delete job;
}

static void CompleteGpuR8Readback(GraphicContext* ctx, void* object, bool completed)
{
	auto* job = static_cast<GpuR8Readback*>(object);
	if (!completed)
	{
		WriteGpuR8ReadbackStatus(*job, "canceled", false, 0, 0);
		DeleteGpuR8Readback(ctx, job);
		return;
	}
	void* mapped = nullptr;
	uint32_t written = 0;
	uint32_t hash = 0;
	const char* status = "map_failed";
	if (vkMapMemory(ctx->device, job->memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS && mapped != nullptr)
	{
		VkMappedMemoryRange range {};
		range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		range.memory = job->memory;
		range.size = VK_WHOLE_SIZE;
		status = "invalidate_failed";
		if (vkInvalidateMappedMemoryRanges(ctx->device, 1, &range) == VK_SUCCESS)
		{
			Core::File binary;
			status = "write_failed";
			if (binary.Create(job->binary_path))
			{
				binary.Write(mapped, static_cast<uint32_t>(job->bytes), &written);
				binary.Close();
				if (written == job->bytes)
				{
					hash = Core::hash(mapped, static_cast<uint32_t>(job->bytes));
					status = "ready";
				}
			}
		}
		vkUnmapMemory(ctx->device, job->memory);
	}
	WriteGpuR8ReadbackStatus(*job, status, true, written, hash);
	DeleteGpuR8Readback(ctx, job);
}

static bool AllocateGpuReadback(GraphicContext* ctx, uint64_t bytes, VkBuffer* buffer_out, VkDeviceMemory* memory_out)
{
	if (ctx == nullptr || buffer_out == nullptr || memory_out == nullptr) { return false; }
	VkBufferCreateInfo buffer {};
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.size = bytes;
	buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(ctx->device, &buffer, nullptr, buffer_out) != VK_SUCCESS || *buffer_out == nullptr) { return false; }
	VkMemoryRequirements requirements {};
	vkGetBufferMemoryRequirements(ctx->device, *buffer_out, &requirements);
	if (requirements.size == 0 || requirements.size > GPU_READBACK_IMAGE_CAP) { return false; }
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &properties);
	uint32_t index = properties.memoryTypeCount;
	for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
	{
		constexpr VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		if ((requirements.memoryTypeBits & (1u << i)) != 0 && (properties.memoryTypes[i].propertyFlags & required) == required)
		{
			index = i;
			break;
		}
	}
	if (index == properties.memoryTypeCount) { return false; }
	VkMemoryAllocateInfo allocate {};
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = index;
	return vkAllocateMemory(ctx->device, &allocate, nullptr, memory_out) == VK_SUCCESS && *memory_out != nullptr &&
	       vkBindBufferMemory(ctx->device, *buffer_out, *memory_out, 0) == VK_SUCCESS;
}

static void RecordGpuR8Readback(VkCommandBuffer command, VkImage image, VkImageLayout original_layout, uint32_t width,
                              uint32_t height, VkBuffer destination)
{
	VkImageMemoryBarrier before {};
	before.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	before.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	before.oldLayout = original_layout;
	before.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	before.srcQueueFamilyIndex = before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.image = image;
	before.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
	                     1, &before);
	VkBufferImageCopy copy {};
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {width, height, 1};
	vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination, 1, &copy);
	VkBufferMemoryBarrier host {};
	host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host.buffer = destination;
	host.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
	VkImageMemoryBarrier after = before;
	after.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	after.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	after.newLayout = original_layout;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
	                     1, &after);
}

struct GpuRgba8MipsReadback
{
	VkBuffer buffer = nullptr;
	VkDeviceMemory memory = nullptr;
	uint32_t job = 0;
	uint32_t event = 0;
	int stage = 0;
	int slot = 0;
	uint64_t image_id = 0;
	uint64_t recording_id = 0;
	uint64_t submit_serial = 0;
	VkImageLayout original_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	uint32_t descriptor[8] = {};
	GpuRgba8MipLayout layout;
	String binary_path;
	String status_path;
};

static void WriteGpuRgba8MipsReadbackStatus(const GpuRgba8MipsReadback& job, const char* status, bool completed,
	                                         uint32_t written, const void* mapped)
{
	Core::File receipt;
	if (!receipt.Create(job.status_path)) { return; }
	const bool ready = completed && mapped != nullptr && written == job.layout.total_bytes;
	const uint32_t hash = ready ? Core::hash(mapped, static_cast<uint32_t>(job.layout.total_bytes)) : 0;
	receipt.Printf("gpu_rgba8_mips version=1 job=%u event=%u stage=%d slot=%d state=%s completed=%u"
	               " encoding=rgba8_unorm_linear underlying_components=RGBA snapshot=first_use"
	               " image_unique_id=%" PRIu64 " width=%u height=%u mip_levels=%u expected_bytes=%" PRIu64
	               " written_bytes=%u hash=%08x recording_id=%" PRIu64 " submit_serial=%" PRIu64 " original_layout=%u\n",
	               job.job, job.event, job.stage, job.slot, status, completed ? 1u : 0u, job.image_id,
	               job.layout.widths[0], job.layout.heights[0], job.layout.levels, job.layout.total_bytes, written, hash,
	               job.recording_id, job.submit_serial, static_cast<unsigned>(job.original_layout));
	const auto* bytes = static_cast<const uint8_t*>(mapped);
	for (uint32_t level = 0; level < job.layout.levels; ++level)
	{
		const uint32_t mip_hash = ready ? Core::hash(bytes + job.layout.offsets[level], job.layout.bytes[level]) : 0;
		receipt.Printf("mip level=%u offset=%" PRIu64 " width=%u height=%u bytes=%u hash=%08x\n", level,
		               job.layout.offsets[level], job.layout.widths[level], job.layout.heights[level],
		               job.layout.bytes[level], mip_hash);
	}
	receipt.Printf("descriptor");
	for (const auto field: job.descriptor) { receipt.Printf(" %08x", field); }
	receipt.Printf("\n");
	receipt.Close();
}

static void DeleteGpuRgba8MipsReadback(GraphicContext* ctx, GpuRgba8MipsReadback* job)
{
	if (job->buffer != nullptr) { vkDestroyBuffer(ctx->device, job->buffer, nullptr); }
	if (job->memory != nullptr) { vkFreeMemory(ctx->device, job->memory, nullptr); }
	delete job;
}

static void CompleteGpuRgba8MipsReadback(GraphicContext* ctx, void* object, bool completed)
{
	auto* job = static_cast<GpuRgba8MipsReadback*>(object);
	if (!completed)
	{
		WriteGpuRgba8MipsReadbackStatus(*job, "canceled", false, 0, nullptr);
		DeleteGpuRgba8MipsReadback(ctx, job);
		return;
	}
	void* mapped = nullptr;
	uint32_t written = 0;
	const char* status = "map_failed";
	if (vkMapMemory(ctx->device, job->memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS && mapped != nullptr)
	{
		VkMappedMemoryRange range {};
		range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		range.memory = job->memory;
		range.size = VK_WHOLE_SIZE;
		status = "invalidate_failed";
		if (vkInvalidateMappedMemoryRanges(ctx->device, 1, &range) == VK_SUCCESS)
		{
			Core::File binary;
			status = "write_failed";
			if (binary.Create(job->binary_path))
			{
				binary.Write(mapped, static_cast<uint32_t>(job->layout.total_bytes), &written);
				binary.Close();
				if (written == job->layout.total_bytes) { status = "ready"; }
			}
		}
		WriteGpuRgba8MipsReadbackStatus(*job, status, true, written, mapped);
		vkUnmapMemory(ctx->device, job->memory);
	} else
	{
		WriteGpuRgba8MipsReadbackStatus(*job, status, true, 0, nullptr);
	}
	DeleteGpuRgba8MipsReadback(ctx, job);
}

static void RecordGpuRgba8MipsReadback(VkCommandBuffer command, VkImage image, VkImageLayout original_layout,
	                                    const GpuRgba8MipLayout& layout, VkBuffer destination)
{
	VkImageMemoryBarrier before {};
	before.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	before.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	before.oldLayout = original_layout;
	before.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	before.srcQueueFamilyIndex = before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.image = image;
	before.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layout.levels, 0, 1};
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
	                     1, &before);
	VkBufferImageCopy copies[GPU_RGBA8_MIP_CAP] {};
	for (uint32_t level = 0; level < layout.levels; ++level)
	{
		copies[level].bufferOffset = layout.offsets[level];
		copies[level].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
		copies[level].imageExtent = {layout.widths[level], layout.heights[level], 1};
	}
	vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination, layout.levels, copies);
	VkBufferMemoryBarrier host {};
	host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host.buffer = destination;
	host.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
	VkImageMemoryBarrier after = before;
	after.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	after.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	after.newLayout = original_layout;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
	                     1, &after);
}

struct GfxResourceCaptureLimits
{
	uint32_t max_events = 512;
	uint64_t max_read_bytes = 64 * 1024 * 1024;
	bool extended = false;
};

static GfxResourceCaptureLimits ResourceCaptureLimitsForSelector(const char* value)
{
	if (value != nullptr && std::strcmp(value, "1") == 0) { return {4096, 256 * 1024 * 1024, true}; }
	return {};
}

class GfxResourceCapture
{
public:
	~GfxResourceCapture()
	{
		// Core::File requires explicit close, including an interrupted capture.
		if (!file.IsInvalid()) { file.Close(); }
	}
	void AbortForSessionReset()
	{
		if (!file.IsInvalid()) { Stop(false, "session_reset"); }
	}

	// Called only under the existing render mutex. CPU ranges remain independent
	// from opt-in GPU sidecars, whose readiness requires a separate completion.
	void Begin(uint64_t submit_id, CommandBuffer* buffer, const char* kind)
	{
		active = false;
		static const bool enabled = [] {
			const char* value = std::getenv("LUMEN_GFX_RESOURCE_CAPTURE");
			return value != nullptr && std::strcmp(value, "1") == 0;
		}();
		if (!enabled || m_phase == Phase::Done)
		{
			return;
		}
		static const auto limits = ResourceCaptureLimitsForSelector(std::getenv("LUMEN_GFX_RESOURCE_CAPTURE_EXTENDED"));
		m_limits = limits;
		if (m_phase == Phase::Waiting)
		{
			// An asynchronous compute CP can keep submit_id=0 indefinitely. Arm
			// from graphics, then include dispatches belonging to that same CP.
			if (std::strcmp(kind, "dispatch") == 0) { return; }
			const auto now = std::chrono::steady_clock::now();
			if (now < m_next_check) { return; }
			m_next_check = now + std::chrono::seconds(1);
			if (!Core::File::IsFileExisting(U"_GpuCapture/request")) { return; }
			// Consume once, even when opening the output fails; never retry every draw.
			m_phase = Phase::Done;
			if (!Core::File::DeleteFile(U"_GpuCapture/request") || !file.Create(U"_GpuCapture/capture.log")) { return; }
			m_cp = buffer->GetParent();
			m_submit = submit_id;
			m_phase = Phase::Armed;
			const bool gpu_r8 = GraphicsRenderGpuR8CaptureEnabled();
			const bool gpu_rgba8_mips = GraphicsRenderGpuRgba8MipsCaptureEnabled();
			file.Printf("capture version=1 scope=one_cp_submit cp=%p armed_submit=%" PRIu64
			            " max_events=%u max_read_bytes=%" PRIu64 " extended=%u hash=Core::hash guest_bytes_only=%u"
			            " dedup=first_use_range raw_metadata=host_abi_hex completion_requires_end_record=1"
			            " gpu_r8_readback=%u gpu_rgba8_mips_readback=%u gpu_readback_job_cap=8"
			            " gpu_readback_byte_cap=8388608 gpu_readback_budget=shared gpu_completion=separate_sidecar\n",
			            static_cast<void*>(m_cp), submit_id, m_limits.max_events, m_limits.max_read_bytes,
			            m_limits.extended ? 1u : 0u, gpu_r8 || gpu_rgba8_mips ? 0u : 1u, gpu_r8 ? 1u : 0u,
			            gpu_rgba8_mips ? 1u : 0u);
			file.Flush();
			return;
		}
		if (buffer->GetParent() != m_cp)
		{
			if (m_phase == Phase::Capturing) { ++m_other_cp_events; }
			return;
		}
		if (m_phase == Phase::Armed)
		{
			if (submit_id == m_submit) { return; }
			m_submit = submit_id;
			m_phase = Phase::Capturing;
			file.Printf("start submit=%" PRIu64 "\n", submit_id);
		} else if (submit_id != m_submit)
		{
			Stop(true, "next_submit");
			return;
		}
		active = true;
		stage = -1;
		++m_events;
		file.Printf("event id=%u kind=%s submit=%" PRIu64 " command_buffer=%p\n", m_events, kind, submit_id,
		            static_cast<void*>(buffer));
	}

	void End()
	{
		if (!active) { return; }
		file.Printf("event_end id=%u\n", m_events);
		active = false;
		if (m_events >= m_limits.max_events) { Stop(false, "event_cap"); }
		else if (m_read_bytes >= m_limits.max_read_bytes) { Stop(false, "byte_cap"); }
		else { file.Flush(); }
	}

	void Metadata(const char* name, const void* data, size_t size)
	{
		if (!active) { return; }
		if (size > 16384 || size > m_limits.max_read_bytes - m_read_bytes) { m_partial = true; return; }
		m_read_bytes += size;
		static constexpr char hex[] = "0123456789abcdef";
		std::string text(size * 2, '0');
		const auto* bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < size; ++i)
		{
			text[2 * i] = hex[bytes[i] >> 4u];
			text[2 * i + 1] = hex[bytes[i] & 15u];
		}
		file.Printf("metadata event=%u stage=%d name=%s bytes=%zu hex=%s\n", m_events, stage, name, size, text.c_str());
	}

	void Range(const char* name, int slot, uint64_t addr, uint64_t size, uint64_t prefix_cap = 1024 * 1024)
	{
		if (!active) { return; }
		for (const auto& previous: m_ranges)
		{
			if (previous.addr == addr && previous.size == size)
			{
				file.Printf("range_ref event=%u stage=%d name=%s slot=%d resource=%u snapshot=first_use\n",
				            m_events, stage, name, slot, previous.id);
				return;
			}
		}
		const uint32_t id = ++m_resource_id;
		const auto count = static_cast<uint32_t>(std::min({size, prefix_cap, m_limits.max_read_bytes - m_read_bytes}));
		std::vector<uint8_t> bytes(count);
		if (size == 0 || addr > UINT64_MAX - size || !Read(addr, bytes.data(), count))
		{
			m_partial = true;
			file.Printf("range event=%u stage=%d name=%s slot=%d resource=%u addr=%016" PRIx64
			            " size=%" PRIu64 " available=0\n", m_events, stage, name, slot, id, addr, size);
			return;
		}
		uint32_t nonzero = 0;
		for (const auto byte: bytes) { nonzero += (byte != 0); }
		const auto path = String::FromPrintf("_GpuCapture/resource_%05u.bin", id);
		Core::File binary;
		uint32_t written = 0;
		if (binary.Create(path))
		{
			binary.Write(bytes.data(), count, &written);
			binary.Close();
		}
		const bool complete = (count == size && written == count);
		m_partial |= !complete;
		file.Printf("range event=%u stage=%d name=%s slot=%d resource=%u addr=%016" PRIx64
		            " size=%" PRIu64 " captured=%u complete=%u hash=%08" PRIx32 " nonzero=%u written=%u\n",
		            m_events, stage, name, slot, id, addr, size, count, complete ? 1u : 0u,
		            Core::hash(bytes.data(), count), nonzero, written);
		// Bound lookup work independently from the byte budget (many tiny buffers).
		if (m_ranges.size() < 4096) { m_ranges.push_back({addr, size, id}); }
	}

	void Shader(const char* name, uint64_t addr, bool embedded = false, uint32_t embedded_id = 0)
	{
		if (!active) { return; }
		file.Printf("shader event=%u name=%s addr=%016" PRIx64 " embedded=%u embedded_id=%u\n",
		            m_events, name, addr, embedded ? 1u : 0u, embedded_id);
		if (embedded) { return; }
		for (const auto previous: m_shaders) { if (previous == addr) { return; } }
		if (m_shaders.size() >= 64) { m_partial = true; return; }
		m_shaders.push_back(addr);
		uint32_t prefix[2] {};
		if (!Read(addr, prefix, sizeof(prefix)) || prefix[0] != 0xbeeb03ff) { m_partial = true; return; }
		// Same binary-info location as Shader.cpp:GetBinaryInfo, with checked 64-bit
		// arithmetic. Saving through this header also preserves the preceding code
		// and usage table, without reparsing or enabling global shader logging.
		const uint64_t offset = (static_cast<uint64_t>(prefix[1]) + 1) * 8;
		uint32_t header[7] {};
		if (offset > 1024 * 1024 - sizeof(header) || addr > UINT64_MAX - offset ||
		    !Read(addr + offset, header, sizeof(header))) { m_partial = true; return; }
		file.Printf("shader_binary name=%s addr=%016" PRIx64 " header_offset=%" PRIu64
		            " declared_length=%u hash0=%08" PRIx32 " hash1=%08" PRIx32 " crc32=%08" PRIx32 "\n",
		            name, addr, offset, header[2] >> 8u, header[4], header[5], header[6]);
		Range("shader_binary", -1, addr, offset + sizeof(header));
	}

	void Image(const char* name, int slot, const VulkanImage* image)
	{
		if (!active || image == nullptr) { return; }
		file.Printf("image event=%u stage=%d name=%s slot=%d image=%p unique_id=%" PRIu64
		            " type=%u format=%u width=%u height=%u layout=%u\n", m_events, stage, name, slot,
		            reinterpret_cast<void*>(image->image), image->memory.unique_id, static_cast<unsigned>(image->type),
		            static_cast<unsigned>(image->format), image->extent.width, image->extent.height,
		            static_cast<unsigned>(image->layout));
	}


	void GpuR8(CommandBuffer* command, const ShaderTextureDescriptor& descriptor, VulkanImage* image, int slot,
	           const GpuMemoryObjectHandle& tracked_handle)
	{
		if (!active || !GraphicsRenderGpuR8CaptureEnabled() || image == nullptr || image->image == nullptr) { return; }
		// Other stages/formats remain the existing CPU-only capture path.
		if (stage != static_cast<int>(ShaderType::Pixel) || descriptor.usage != ShaderTextureUsage::ReadOnly ||
		    descriptor.textures2d_without_sampler || image->format != VK_FORMAT_R8_UNORM) { return; }
		const auto& texture = descriptor.texture;
		GpuR8CaptureShape shape {command->CanRecordTextureTransfer(), true, true, static_cast<uint32_t>(image->format),
		                         image->extent.width, image->extent.height, uint32_t(texture.Width4()) + 1,
		                         uint32_t(texture.Height4()) + 1, texture.Type(), texture.BaseLevel(), texture.LastLevel(),
		                         image->mip_levels, texture.Depth(), texture.BaseArray(), texture.LastArray(),
		                         static_cast<uint32_t>(image->layout), (image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0};
		const char* rejection = GpuR8CaptureRejection(shape);
		if (rejection != nullptr || image->memory.unique_id == 0)
		{
			file.Printf("gpu_r8_skip event=%u stage=%d slot=%d reason=%s\n", m_events, stage, slot,
			            rejection == nullptr ? "image_identity" : rejection);
			return;
		}
		for (const auto& previous: m_gpu_images)
		{
			if (previous.image_id == image->memory.unique_id)
			{
				file.Printf("gpu_r8_ref event=%u stage=%d slot=%d job=%u snapshot=first_use\n",
				            m_events, stage, slot, previous.job);
				return;
			}
		}
		const uint64_t bytes = uint64_t(shape.width) * shape.height;
		if (!GpuReadbackBudgetAllowed(m_gpu_jobs, m_gpu_bytes, bytes))
		{
			file.Printf("gpu_r8_skip event=%u stage=%d slot=%d reason=budget\n", m_events, stage, slot);
			return;
		}
		auto* job = new GpuR8Readback;
		job->job = ++m_gpu_jobs;
		job->event = m_events;
		job->stage = stage;
		job->slot = slot;
		job->width = shape.width;
		job->height = shape.height;
		job->bytes = bytes;
		job->image_id = image->memory.unique_id;
		job->recording_id = command->GetRecordingId();
		job->submit_serial = command->GetRecordingSubmitSerial();
		job->original_layout = image->layout;
		std::memcpy(job->descriptor, texture.fields, sizeof(job->descriptor));
		job->binary_path = String::FromPrintf("_GpuCapture/gpu_r8_%04u.bin", job->job);
		job->status_path = String::FromPrintf("_GpuCapture/gpu_r8_%04u.status", job->job);
		m_gpu_bytes += bytes;
		// Sidecars can complete after capture.log closes. No callback holds this
		// logger or a source-image pointer, and no existing output is overwritten.
		if (Core::File::IsFileExisting(job->binary_path) || Core::File::IsFileExisting(job->status_path))
		{
			file.Printf("gpu_r8_skip event=%u stage=%d slot=%d job=%u reason=output_exists\n", m_events, stage, slot, job->job);
			delete job;
			return;
		}
		m_gpu_images.push_back({job->image_id, job->job});
		auto* ctx = g_render_ctx->GetGraphicCtx();
		if (!AllocateGpuReadback(ctx, job->bytes, &job->buffer, &job->memory))
		{
			file.Printf("gpu_r8_skip event=%u stage=%d slot=%d job=%u reason=allocation_failed\n", m_events, stage, slot, job->job);
			WriteGpuR8ReadbackStatus(*job, "allocation_failed", false, 0, 0);
			DeleteGpuR8Readback(ctx, job);
			return;
		}
		file.Printf("gpu_r8_pending event=%u stage=%d slot=%d job=%u image_unique_id=%" PRIu64
		            " width=%u height=%u bytes=%" PRIu64 " encoding=r8_unorm_linear snapshot=first_use"
		            " completion=separate_sidecar\n", m_events, stage, slot, job->job, job->image_id, job->width, job->height, job->bytes);
		// One read-only query per accepted first-use job, within the existing
		// eight-image budget. This does not advance the command/access ledger.
		const auto lineage = GpuMemoryQueryR8Lineage(command, tracked_handle);
		file.Printf("gpu_r8_lineage event=%u stage=%d slot=%d job=%u image_unique_id=%" PRIu64
		            " available=%u reason=%s producer_handle_matches=%u copied_epoch_matches=%u"
		            " copied_producer_epoch=%" PRIu64 " copied_producer_generation=%" PRIu64
		            " current_identity_valid=%u current_recording=%" PRIu64 " current_ordinal=%" PRIu64
		            " current_submit=%" PRIu64 " current_session=%" PRIu64 " current_queue=%d"
		            " submitted_cutoff=%" PRIu64 " completed_cutoff=%" PRIu64 " policy=observe_only\n",
		            m_events, stage, slot, job->job, job->image_id, unsigned(lineage.available), lineage.reason,
		            unsigned(lineage.producer_handle_matches), unsigned(lineage.copied_epoch_matches),
		            lineage.copied_producer_epoch, lineage.copied_producer_generation, unsigned(lineage.current_identity_valid),
		            lineage.current_recording_id, lineage.current_command_ordinal, lineage.current_submit_serial,
		            lineage.current_session_epoch, lineage.current_queue, lineage.submitted_cutoff, lineage.completed_cutoff);
		const GpuMemoryR8LineagePoint* points[] = {&lineage.sampled, &lineage.producer};
		const char* roles[] = {"sampled", "producer"};
		for (unsigned index = 0; index < 2; ++index)
		{
			const auto& point = *points[index];
			file.Printf("gpu_r8_lineage_point event=%u job=%u role=%s image_unique_id=%" PRIu64
			            " generation=%" PRIu64 " content_epoch=%" PRIu64 " kind=%u"
			            " recording=%" PRIu64 " ordinal=%" PRIu64 " submit=%" PRIu64 " session=%" PRIu64 " queue=%d"
			            " identity_valid=%u access_precedes=%u mutation_precedes=%u mutation_completed_before_recording=%u"
			            " access_ambiguous=%u pending_storage_bind=%u\n",
			            m_events, job->job, roles[index], point.image_unique_id, point.lifetime_generation,
			            point.content_epoch, unsigned(point.kind), point.recording_id, point.command_ordinal,
			            point.submit_serial, point.session_epoch, point.queue, unsigned(point.identity_valid),
			            unsigned(point.access_precedes), unsigned(point.mutation_precedes),
			            unsigned(point.mutation_completed_before_recording), unsigned(point.access_ambiguous),
			            unsigned(point.pending_storage_bind));
		}
		RecordGpuR8Readback(command->GetBufferOutsideRenderPass(), image->image, image->layout,
		                    job->width, job->height, job->buffer);
		command->Retire(job, CompleteGpuR8Readback);
	}

	void GpuRgba8Mips(CommandBuffer* command, const ShaderTextureDescriptor& descriptor, VulkanImage* image, int slot)
	{
		if (!active || !GraphicsRenderGpuRgba8MipsCaptureEnabled() || image == nullptr || image->image == nullptr) { return; }
		// The multi-mip selector deliberately excludes single-level RGBA8 images,
		// keeping the diagnostic budget focused without a title or address filter.
		if (stage != static_cast<int>(ShaderType::Pixel) || descriptor.usage != ShaderTextureUsage::ReadOnly ||
		    descriptor.textures2d_without_sampler || image->format != VK_FORMAT_R8G8B8A8_UNORM || image->mip_levels < 2)
		{
			return;
		}
		const auto& texture = descriptor.texture;
		GpuR8CaptureShape shape {command->CanRecordTextureTransfer(), true, true, static_cast<uint32_t>(image->format),
		                         image->extent.width, image->extent.height, uint32_t(texture.Width4()) + 1,
		                         uint32_t(texture.Height4()) + 1, texture.Type(), texture.BaseLevel(), texture.LastLevel(),
		                         image->mip_levels, texture.Depth(), texture.BaseArray(), texture.LastArray(),
		                         static_cast<uint32_t>(image->layout), (image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0};
		GpuRgba8MipLayout layout {};
		const char* rejection = GpuRgba8MipsCaptureRejection(shape, &layout);
		if (rejection != nullptr || image->memory.unique_id == 0)
		{
			file.Printf("gpu_rgba8_mips_skip event=%u stage=%d slot=%d reason=%s\n", m_events, stage, slot,
			            rejection == nullptr ? "image_identity" : rejection);
			return;
		}
		for (const auto& previous: m_gpu_images)
		{
			if (previous.image_id == image->memory.unique_id)
			{
				file.Printf("gpu_rgba8_mips_ref event=%u stage=%d slot=%d job=%u snapshot=first_use\n",
				            m_events, stage, slot, previous.job);
				return;
			}
		}
		if (!GpuReadbackBudgetAllowed(m_gpu_jobs, m_gpu_bytes, layout.total_bytes))
		{
			file.Printf("gpu_rgba8_mips_skip event=%u stage=%d slot=%d reason=budget\n", m_events, stage, slot);
			return;
		}
		auto* job = new GpuRgba8MipsReadback;
		job->job = ++m_gpu_jobs;
		job->event = m_events;
		job->stage = stage;
		job->slot = slot;
		job->image_id = image->memory.unique_id;
		job->recording_id = command->GetRecordingId();
		job->submit_serial = command->GetRecordingSubmitSerial();
		job->original_layout = image->layout;
		job->layout = layout;
		std::memcpy(job->descriptor, texture.fields, sizeof(job->descriptor));
		job->binary_path = String::FromPrintf("_GpuCapture/gpu_rgba8_mips_%04u.bin", job->job);
		job->status_path = String::FromPrintf("_GpuCapture/gpu_rgba8_mips_%04u.status", job->job);
		m_gpu_bytes += layout.total_bytes;
		if (Core::File::IsFileExisting(job->binary_path) || Core::File::IsFileExisting(job->status_path))
		{
			file.Printf("gpu_rgba8_mips_skip event=%u stage=%d slot=%d job=%u reason=output_exists\n",
			            m_events, stage, slot, job->job);
			delete job;
			return;
		}
		m_gpu_images.push_back({job->image_id, job->job});
		auto* ctx = g_render_ctx->GetGraphicCtx();
		if (!AllocateGpuReadback(ctx, job->layout.total_bytes, &job->buffer, &job->memory))
		{
			file.Printf("gpu_rgba8_mips_skip event=%u stage=%d slot=%d job=%u reason=allocation_failed\n",
			            m_events, stage, slot, job->job);
			WriteGpuRgba8MipsReadbackStatus(*job, "allocation_failed", false, 0, nullptr);
			DeleteGpuRgba8MipsReadback(ctx, job);
			return;
		}
		file.Printf("gpu_rgba8_mips_pending event=%u stage=%d slot=%d job=%u image_unique_id=%" PRIu64
		            " width=%u height=%u mip_levels=%u bytes=%" PRIu64
		            " encoding=rgba8_unorm_linear snapshot=first_use completion=separate_sidecar\n",
		            m_events, stage, slot, job->job, job->image_id, layout.widths[0], layout.heights[0], layout.levels,
		            layout.total_bytes);
		for (uint32_t level = 0; level < layout.levels; ++level)
		{
			file.Printf("gpu_rgba8_mips_layout event=%u job=%u level=%u offset=%" PRIu64
			            " width=%u height=%u bytes=%u\n", m_events, job->job, level, layout.offsets[level],
			            layout.widths[level], layout.heights[level], layout.bytes[level]);
		}
		RecordGpuRgba8MipsReadback(command->GetBufferOutsideRenderPass(), image->image, image->layout,
		                            job->layout, job->buffer);
		command->Retire(job, CompleteGpuRgba8MipsReadback);
	}

	void Buffer(const char* name, int slot, const VulkanBuffer* buffer)
	{
		if (!active || buffer == nullptr) { return; }
		file.Printf("buffer event=%u stage=%d name=%s slot=%d buffer=%p unique_id=%" PRIu64 "\n",
		            m_events, stage, name, slot, reinterpret_cast<void*>(buffer->buffer), buffer->memory.unique_id);
	}

	void DrawState(HW::Context* ctx, HW::Shader* sh, VulkanPipeline* pipeline, const ShaderVertexInputInfo& vs,
	               const ShaderPixelInputInfo& ps, const RenderColorInfo& color, const RenderDepthInfo& depth)
	{
		if (!active) { return; }
		Metadata("vs_input", &vs, sizeof(vs));
		Metadata("ps_input", &ps, sizeof(ps));
		Metadata("vs_registers", &sh->GetVs(), sizeof(sh->GetVs()));
		Metadata("ps_registers", &sh->GetPs(), sizeof(sh->GetPs()));
		Metadata("shader_registers", &ctx->GetShaderRegisters(), sizeof(ctx->GetShaderRegisters()));
		Metadata("render_target_0", &ctx->GetRenderTarget(0), sizeof(ctx->GetRenderTarget(0)));
		Metadata("depth_control", &ctx->GetDepthControl(), sizeof(ctx->GetDepthControl()));
		Metadata("depth_render_target", &ctx->GetDepthRenderTarget(), sizeof(ctx->GetDepthRenderTarget()));
		Metadata("viewport", &ctx->GetScreenViewport(), sizeof(ctx->GetScreenViewport()));
		Metadata("clip", &ctx->GetClipControl(), sizeof(ctx->GetClipControl()));
		Metadata("mode_control", &ctx->GetModeControl(), sizeof(ctx->GetModeControl()));
		Metadata("polygon_offset_registers", &ctx->GetPolygonOffsetRegisters(), sizeof(ctx->GetPolygonOffsetRegisters()));
		Metadata("pipeline_static", pipeline->static_params, sizeof(*pipeline->static_params));
		Metadata("pipeline_dynamic", pipeline->dynamic_params, sizeof(*pipeline->dynamic_params));
		const auto& p = *pipeline->static_params;
		const auto& d = *pipeline->dynamic_params;
		file.Printf("pipeline event=%u topology=%u viewport_scale=%g,%g,%g viewport_offset=%g,%g,%g"
		            " scissor=%d,%d,%d,%d scissor_dynamic=%u cull_front=%u cull_back=%u face=%u color_mask=%08x blend=%u"
		            " depth_test=%u depth_write=%u depth_compare=%u stencil_test=%u"
		            " depth_bias=%u bias_constant=%g bias_slope=%g bias_clamp=%g\n", m_events,
		            static_cast<unsigned>(p.topology), p.viewport_scale[0], p.viewport_scale[1], p.viewport_scale[2],
		            p.viewport_offset[0], p.viewport_offset[1], p.viewport_offset[2], d.scissor_ltrb[0], d.scissor_ltrb[1],
		            d.scissor_ltrb[2], d.scissor_ltrb[3], d.vk_dynamic_state_scissor ? 1u : 0u,
		            p.cull_front ? 1u : 0u, p.cull_back ? 1u : 0u, p.face ? 1u : 0u,
		            p.color_mask, p.blend_enable ? 1u : 0u, p.depth_test_enable ? 1u : 0u, p.depth_write_enable ? 1u : 0u,
		            static_cast<unsigned>(p.depth_compare_op), p.stencil_test_enable ? 1u : 0u,
		            p.depth_bias.enabled ? 1u : 0u, p.depth_bias.constant, p.depth_bias.slope, p.depth_bias.clamp);
		file.Printf("targets event=%u color_addr=%016" PRIx64 " color_size=%" PRIu64
		            " depth_addr=%016" PRIx64 " depth_size=%" PRIu64 " depth_clear=%u depth_clear_value=%g"
		            " stencil_addr=%016" PRIx64 " stencil_size=%" PRIu64 " stencil_clear=%u stencil_clear_value=%u\n",
		            m_events, color.base_addr, color.buffer_size, depth.depth_buffer_vaddr, depth.depth_buffer_size,
		            depth.depth_clear_enable ? 1u : 0u, depth.depth_clear_value, depth.stencil_buffer_vaddr,
		            depth.stencil_buffer_size, depth.stencil_clear_enable ? 1u : 0u, depth.stencil_clear_value);
		Image("color_target", 0, color.vulkan_buffer);
		Image("depth_target", 0, depth.vulkan_buffer);
	}

	bool active = false;
	int stage = -1;
	Core::File file;

private:
	bool Read(uint64_t addr, void* dst, uint32_t size)
	{
		if (addr == 0 || size == 0 || addr > UINT64_MAX - size || size > m_limits.max_read_bytes - m_read_bytes) { return false; }
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		// Fail closed for unmanaged/reserved/unreadable spans. These are the same
		// live ranges already used by resource creation, additionally capped here.
		uint64_t cursor = addr;
		for (unsigned segments = 0; cursor < addr + size; ++segments)
		{
			Core::VirtualMemory::ManagedRegion region {};
			if (segments == 256 || Core::VirtualMemory::QueryManagedRegion(cursor, &region) !=
			                           Core::VirtualMemory::ManagedRegionState::Visible ||
			    (static_cast<uint32_t>(region.mode) & 1u) == 0 || region.end <= cursor) { return false; }
			cursor = region.end;
		}
		std::memcpy(dst, reinterpret_cast<const void*>(addr), size);
		m_read_bytes += size;
		return true;
#else
		return false;
#endif
	}

	void Stop(bool complete, const char* reason)
	{
		file.Printf("end complete_submit=%u resources_complete=%u reason=%s events=%u read_bytes=%" PRIu64
		            " other_cp_events=%u global_frame_complete=0\n", complete ? 1u : 0u, m_partial ? 0u : 1u,
		            reason, m_events, m_read_bytes, m_other_cp_events);
		file.Flush();
		file.Close();
		active = false;
		m_phase = Phase::Done;
	}

	GfxResourceCaptureLimits m_limits;
	enum class Phase { Waiting, Armed, Capturing, Done };
	struct RangeInfo { uint64_t addr; uint64_t size; uint32_t id; };
	Phase m_phase = Phase::Waiting;
	CommandProcessor* m_cp = nullptr;
	uint64_t m_submit = 0;
	uint64_t m_read_bytes = 0;
	uint32_t m_events = 0;
	uint32_t m_other_cp_events = 0;
	uint32_t m_resource_id = 0;
	bool m_partial = false;
	std::chrono::steady_clock::time_point m_next_check {};
	std::vector<RangeInfo> m_ranges;
	std::vector<uint64_t> m_shaders;
	struct GpuImageInfo { uint64_t image_id; uint32_t job; };
	std::vector<GpuImageInfo> m_gpu_images;
	uint32_t m_gpu_jobs = 0;
	uint64_t m_gpu_bytes = 0;
};

static GfxResourceCapture g_resource_capture;

static void AbortGfxResourceCaptureForSessionReset()
{
	g_resource_capture.AbortForSessionReset();
}

struct GfxResourceCaptureEvent
{
	GfxResourceCaptureEvent(uint64_t submit_id, CommandBuffer* buffer, const char* kind) { g_resource_capture.Begin(submit_id, buffer, kind); }
	~GfxResourceCaptureEvent() { g_resource_capture.End(); }
};

static GpuMemoryAccessKind StorageBufferDeclaredAccess(ShaderStorageUsage usage)
{
	switch (usage)
	{
		case ShaderStorageUsage::Constant:
		case ShaderStorageUsage::ReadOnly: return GpuMemoryAccessKind::StorageRead;
		case ShaderStorageUsage::ReadWrite: return GpuMemoryAccessKind::StorageReadWrite;
		default: return GpuMemoryAccessKind::Unknown;
	}
}

static bool DrawAccessPlanEnabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_GPU_DRAW_ACCESS_PLAN");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

// This inventory is synchronous: renderer caches and recording batches remain
// owned under the existing processor/renderer locks and exclusive epoch. It
// describes all native bindings, but is not an asynchronous resource pin.
struct DrawNativeInventory
{
	GpuMemoryDrawAccessPlan accesses;
	std::array<const void*, 40> renderer_objects {};
	std::array<VkSampler, 32> samplers {};
	std::array<VkDescriptorSet, 3> descriptors {};
	VkDescriptorBufferInfo uniform {};
	uint32_t renderer_count = 0, sampler_count = 0, descriptor_count = 0;
	bool complete = true;

	void Add(void* object, GpuMemoryObjectType type, const GpuMemoryObjectHandle& handle = {},
	         uint64_t address = 0, uint64_t size = 0)
	{
		if (object == nullptr || accesses.native_input_count == accesses.native_inputs.size())
		{
			complete = false;
			return;
		}
		auto& input = accesses.native_inputs[accesses.native_input_count++];
		input.native_object = object;
		input.type = type;
		input.handle = handle;
		if (address != 0 && size != 0)
		{
			input.vaddr[0] = address;
			input.size[0] = size;
			input.ranges = 1;
		}
	}
	void AddRendererObject(const void* object)
	{
		if (object == nullptr || renderer_count == renderer_objects.size()) complete = false;
		else renderer_objects[renderer_count++] = object;
	}
	void AddSampler(VkSampler sampler)
	{
		if (sampler == VK_NULL_HANDLE || sampler_count == samplers.size()) complete = false;
		else samplers[sampler_count++] = sampler;
	}
	void AddDescriptor(VkDescriptorSet descriptor)
	{
		if (descriptor == VK_NULL_HANDLE || descriptor_count == descriptors.size()) complete = false;
		else descriptors[descriptor_count++] = descriptor;
	}
};

static GpuMemoryObjectType DrawImageObjectType(VulkanImageType type)
{
	switch (type)
	{
		case VulkanImageType::VideoOut: return GpuMemoryObjectType::VideoOutBuffer;
		case VulkanImageType::DepthStencil: return GpuMemoryObjectType::DepthStencilBuffer;
		case VulkanImageType::Texture: return GpuMemoryObjectType::Texture;
		case VulkanImageType::StorageTexture: return GpuMemoryObjectType::StorageTexture;
		case VulkanImageType::RenderTexture: return GpuMemoryObjectType::RenderTexture;
		default: return GpuMemoryObjectType::Invalid;
	}
}

struct GpuMemoryStorageAccessBatch
{
	// One graphics command binds at most a full VS and PS resource set.
	static constexpr uint32_t MAX = ShaderStorageResources::BUFFERS_MAX * 2u;
	bool Add(const GpuMemoryObjectHandle& handle, GpuMemoryAccessKind kind, bool shader_no_storage_stores = false,
	         bool compute_writer = false)
	{
		if (handle.host_object == nullptr || handle.lifetime_generation == 0 || handle.heap_id < 0 ||
		    handle.object_id < 0 || count >= MAX) return false;
		// Do not collapse stage uses: a later read cannot erase an earlier
		// potential write or unknown access to the same generation.
		accesses[count++] = {handle, kind, kind == GpuMemoryAccessKind::StorageRead && shader_no_storage_stores, compute_writer};
		return true;
	}
	GpuMemoryCommandAccess accesses[MAX];
	uint32_t count = 0;
	DrawNativeInventory* native_inputs = nullptr;
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void PrepareStorageBuffers(uint64_t submit_id, CommandBuffer* buffer, const ShaderStorageResources& storage_buffers,
	                              VulkanBuffer** buffers, uint32_t** sgprs, GpuMemoryStorageAccessBatch* command_accesses,
	                              const ShaderStorageWriteAuthority& shader_storage_writes, const VulkanPipeline* diagnostic_pipeline,
	                              DescriptorCache::Stage diagnostic_stage)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(buffers == nullptr);
	EXIT_IF(sgprs == nullptr);
	EXIT_IF(*sgprs == nullptr);

	for (int i = 0; i < storage_buffers.buffers_num; i++)
	{
		auto r = storage_buffers.buffers[i];

		EXIT_NOT_IMPLEMENTED(r.AddTid());
		EXIT_NOT_IMPLEMENTED(r.SwizzleEnabled());

		EXIT_NOT_IMPLEMENTED(!ShaderStorageBufferFormatUsageSupported(r, storage_buffers.usages[i]));
		EXIT_NOT_IMPLEMENTED(!(r.MemoryType() == 0x00 || r.MemoryType() == 0x10 || r.MemoryType() == 0x6d));
		if (ShaderStorageBufferXyFloatStride8Compatible(r))
		{
			static std::atomic_bool receipt {false};
			if (!receipt.exchange(true, std::memory_order_relaxed))
			{
				std::fprintf(stderr,
				             "[GFX_MUBUF_XY_FLOAT_STRIDE8] dfmt=32_32 nfmt=float dst=xy01 stride=8 memory=0x10 usage=readonly policy=exact-profile\n");
				std::fflush(stderr);
			}
		}
		if (ShaderStorageBufferUnorm8x4Constant0011Compatible(r))
		{
			static std::atomic_bool receipt {false};
			if (!receipt.exchange(true, std::memory_order_relaxed))
			{
				std::fprintf(stderr,
				             "[GFX_MUBUF_UNORM8_CONSTANT_0011] dfmt=8_8_8_8 nfmt=unorm dst=0011 stride=8 memory=0x10 usage=readonly policy=exact-profile\n");
				std::fflush(stderr);
			}
		}
		if (ShaderStorageBufferSscaled16XyzCompatible(r))
		{
			static std::atomic_bool receipt {false};
			if (!receipt.exchange(true, std::memory_order_relaxed))
			{
				std::fprintf(stderr,
				             "[GFX_MUBUF_SSCALED16_XYZ] dfmt=16_16_16_16 nfmt=sscaled dst=xyz0 stride=16 memory=0x10 policy=exact-profile\n");
				std::fflush(stderr);
			}
		}

		auto addr        = r.Base44();
		auto stride      = r.Stride();
		auto num_records = r.NumRecords();
		auto size        = stride * num_records;
		EXIT_NOT_IMPLEMENTED(size == 0);
		EXIT_NOT_IMPLEMENTED((size & 0x3u) != 0);

		bool read_only = (r.MemoryType() == 0x10);

		EXIT_NOT_IMPLEMENTED(read_only && !(storage_buffers.usages[i] == ShaderStorageUsage::ReadOnly ||
		                                    storage_buffers.usages[i] == ShaderStorageUsage::Constant));

		StorageBufferGpuObject buf_info(stride, num_records, read_only);
		const bool track_access = command_accesses != nullptr && GpuMemoryAccessTrackingEnabled();
		const bool shader_no_storage_stores = ShaderStorageBindingNoStoreProof(shader_storage_writes, i);
		const GpuMemoryStorageBindingAuthority binding_authority {
		    diagnostic_stage == DescriptorCache::Stage::Compute,
		    shader_storage_writes.complete && (shader_storage_writes.no_store_mask & shader_storage_writes.write_mask) == 0,
		    ShaderStorageBindingWriteProof(shader_storage_writes, i)};
		if (GpuMemoryEopResourceTraceEnabled() && GpuMemoryHasUniformDepthZeroOverlap(addr, size))
		{
			static std::atomic<uint32_t> records {0};
			const uint32_t record = records.fetch_add(1, std::memory_order_relaxed);
			if (record < 16)
				std::fprintf(stderr, "[GFX_UNIFORM_DEPTH_ZERO_STORAGE_REQUEST] submit=%" PRIu64
				                     " recording=%" PRIu64 " prior_ordinal=%" PRIu64 " descriptor_stage=%u"
				                     " index=%d start_register=%d slot=%d extended=%u address=0x%016" PRIx64
				                     " bytes=%u stride=%u records=%u memory_type=%u read_only=%u declared_usage=%u"
				                     " access_kind=%u actual_no_store=%u authority_complete=%u write_mask=%04" PRIx32
				                     " binding_writer=%u tracked=%u metadata_only=1\n",
				    submit_id, buffer->GetRecordingId(), buffer->GetTrackedCommandOrdinal(), static_cast<unsigned>(diagnostic_stage),
				    i, storage_buffers.start_register[i], storage_buffers.slots[i], storage_buffers.extended[i] ? 1u : 0u,
				    addr, size, stride, num_records, r.MemoryType(), read_only ? 1u : 0u,
				    static_cast<unsigned>(storage_buffers.usages[i]),
				    static_cast<unsigned>(StorageBufferDeclaredAccess(storage_buffers.usages[i])),
				    shader_no_storage_stores ? 1u : 0u, binding_authority.complete ? 1u : 0u,
				    shader_storage_writes.write_mask, binding_authority.writes ? 1u : 0u, track_access ? 1u : 0u);
		}
		GpuMemoryObjectHandle handle;
		// Descriptor memory type retains its admission/writeback meaning. The
		// independent declared usage supplies only conservative command access.
		if (track_access) buf_info.access_kind = StorageBufferDeclaredAccess(storage_buffers.usages[i]);
		auto* buf = static_cast<StorageVulkanBuffer*>(track_access
		    ? GpuMemoryCreateStorageObjectTracked(submit_id, g_render_ctx->GetGraphicCtx(), buffer, addr, size,
		        buf_info, binding_authority, &handle)
		    : GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), buffer, addr, size, buf_info));

		EXIT_NOT_IMPLEMENTED(buf == nullptr);
		if (command_accesses != nullptr && command_accesses->native_inputs != nullptr)
			command_accesses->native_inputs->Add(buf, GpuMemoryObjectType::StorageBuffer, handle, addr, size);
		if (track_access) EXIT_NOT_IMPLEMENTED(!command_accesses->Add(handle, buf_info.access_kind, shader_no_storage_stores,
		    binding_authority.compute && binding_authority.complete && binding_authority.writes));
		if (track_access && !shader_no_storage_stores && buf_info.access_kind == GpuMemoryAccessKind::StorageRead &&
		    TakeStorageNoStoreDiagnosticRecord())
			std::fprintf(stderr, "[GFX_STORAGE_NO_STORE_BIND] pipeline=%p descriptor_stage=%u submit=%" PRIu64
			                     " recording=%" PRIu64 " prior_ordinal=%" PRIu64 " heap=%d object=%d lifetime=%" PRIu64
			                     " index=%d start_register=%d slot=%d extended=%u usage=%u memory_type=%u stride=%u records=%u"
			                     " bytes=%u actual_no_store=0 metadata_only=1 admission=unchanged\n",
			             static_cast<const void*>(diagnostic_pipeline), static_cast<unsigned>(diagnostic_stage), submit_id,
			             buffer->GetRecordingId(), buffer->GetTrackedCommandOrdinal(), handle.heap_id, handle.object_id,
			             handle.lifetime_generation, i, storage_buffers.start_register[i], storage_buffers.slots[i],
			             storage_buffers.extended[i] ? 1u : 0u, static_cast<unsigned>(storage_buffers.usages[i]), r.MemoryType(),
			             stride, num_records, size);

		StorageBufferSet(buffer, buf);
		g_resource_capture.Buffer("storage", i, buf);
		g_resource_capture.Range("storage_guest", i, addr, size);

		buffers[i] = buf;

		r.UpdateAddress44(i);

		EXIT_NOT_IMPLEMENTED((r.Base44() >> 32u) != 0);

		(*sgprs)[0] = r.fields[0];
		(*sgprs)[1] = r.fields[1];
		(*sgprs)[2] = r.fields[2];
		(*sgprs)[3] = r.fields[3];

		(*sgprs) += 4;
	}
}

static void DumpPreparedTextureMismatch(const char* reason, DescriptorCache::Stage stage, int index,
	                                    const ShaderTextureDescriptor& desc)
{
	const auto& r = desc.texture;
	dbg_printf("[GFX_DESCRIPTOR_MISMATCH] phase=prepare reason=%s stage=%u index=%d usage=%u start=%d slot=%d extended=%u\n", reason,
	           static_cast<unsigned>(stage), index, static_cast<unsigned>(desc.usage), desc.start_register, desc.slot,
	           desc.extended ? 1u : 0u);
	dbg_printf("[GFX_DESCRIPTOR_MISMATCH] raw=%08" PRIx32 ":%08" PRIx32 ":%08" PRIx32 ":%08" PRIx32 ":%08" PRIx32
	           ":%08" PRIx32 ":%08" PRIx32 ":%08" PRIx32
	           " type=%u base=0x%016" PRIx64 " dfmt=%u nfmt=%u width=%u height=%u pitch=%u tile=%u memory=0x%x\n",
	           r.fields[0], r.fields[1], r.fields[2], r.fields[3], r.fields[4], r.fields[5], r.fields[6], r.fields[7], r.Type(),
	           r.Base38(), r.Dfmt(), r.Nfmt(), static_cast<unsigned>(r.Width4() + 1u), static_cast<unsigned>(r.Height4() + 1u),
	           static_cast<unsigned>(r.Pitch() + 1u), r.TileMode(), r.MemoryType());
}

struct GpuMemoryImageAccessBatch
{
	static constexpr uint32_t MAX = ShaderTextureResources::RES_MAX * 2u;
	DrawNativeInventory* native_inputs = nullptr;

	void SetRenderAttachments(const RenderColorInfo& color, const RenderDepthInfo& depth)
	{
		color_memory = color.vulkan_buffer != nullptr ? color.vulkan_buffer->memory.memory : VK_NULL_HANDLE;
		depth_memory = depth.vulkan_buffer != nullptr ? depth.vulkan_buffer->memory.memory : VK_NULL_HANDLE;
	}

	void ObserveRenderResource(VkDeviceMemory memory)
	{
		// Reject whole-allocation aliases, even if views, image handles or buffer
		// offsets differ. Unknown allocations are conservatively ineligible.
		render_pass_read_only &= memory != VK_NULL_HANDLE && memory != color_memory && memory != depth_memory;
	}

	void Add(const GpuMemoryObjectHandle& handle, GpuMemoryAccessKind kind)
	{
		if (handle.host_object == nullptr || handle.lifetime_generation == 0)
		{
			return;
		}
		EXIT_IF(count >= MAX);
		accesses[count++] = {handle, kind};
	}

	[[nodiscard]] VideoOutVulkanImage* FindSampledVideoOut(const GpuMemoryObjectHandle& handle) const
	{
		for (uint32_t index = 0; index < sampled_videoout_count; ++index)
		{
			const auto& existing = sampled_videoout_handles[index];
			if (existing.host_object == handle.host_object &&
			    existing.lifetime_generation == handle.lifetime_generation &&
			    existing.heap_id == handle.heap_id && existing.object_id == handle.object_id)
			{
				return sampled_videoout[index];
			}
		}
		return nullptr;
	}

	void AddSampledVideoOut(VideoOutVulkanImage* image, const GpuMemoryObjectHandle& handle)
	{
		EXIT_IF(image == nullptr || handle.host_object != image || handle.lifetime_generation == 0 ||
		        FindSampledVideoOut(handle) != nullptr);
		EXIT_IF(sampled_videoout_count >= MAX);
		sampled_videoout[sampled_videoout_count]         = image;
		sampled_videoout_handles[sampled_videoout_count] = handle;
		++sampled_videoout_count;
	}

	GpuMemoryCommandAccess accesses[MAX];
	uint32_t               count = 0;
	VideoOutVulkanImage*   sampled_videoout[MAX] = {};
	GpuMemoryObjectHandle  sampled_videoout_handles[MAX] = {};
	uint32_t               sampled_videoout_count = 0;
	VkDeviceMemory         color_memory = VK_NULL_HANDLE;
	VkDeviceMemory         depth_memory = VK_NULL_HANDLE;
	bool                   render_pass_read_only = true;
};

static void RecordShaderResourceCommandAccesses(CommandBuffer* buffer, uint32_t x, uint32_t y, uint32_t z,
	                                            const GpuMemoryImageAccessBatch& images,
	                                            const GpuMemoryStorageAccessBatch& storage,
	                                            GpuMemoryCommandMutationPolicy image_mutation_policy)
{
	// Called only after the Vulkan command has been recorded. Zero work keeps
	// storage binds pending. VideoOut uses are authoritative even when the
	// optional general access ledger is disabled.
	if (x == 0 || y == 0 || z == 0) return;
	for (uint32_t i = 0; i != images.sampled_videoout_count; ++i)
		EXIT_NOT_IMPLEMENTED(!GpuMemoryRecordVideoOutBufferRead(buffer, images.sampled_videoout_handles[i]));
	if (!GpuMemoryAccessTrackingEnabled() ||
	    (images.count == 0 && storage.count == 0)) return;
	GpuMemoryRecordCommandAccessGroups(buffer, images.accesses, images.count, image_mutation_policy,
	                                  storage.accesses, storage.count);
}

static void BuildDrawAttachmentPlan(GpuMemoryDrawAccessPlan* plan, uint64_t submit_id, bool draw_recorded,
	                               const RenderColorInfo& color, const RenderDepthInfo& depth, const VulkanPipeline* pipeline)
{
	plan->submit_id = submit_id;
	plan->draw_recorded = draw_recorded;
	if (color.vulkan_buffer != nullptr)
	{
		plan->color.kind = color.type == RenderColorType::RenderTexture ? GpuMemoryDrawColorKind::RenderTexture :
		                   color.type == RenderColorType::DisplayBuffer ? GpuMemoryDrawColorKind::VideoOut : GpuMemoryDrawColorKind::None;
		plan->color.handle = color.tracked_handle;
		plan->color.address = color.base_addr;
		plan->color.size = color.buffer_size;
	}
	if (depth.format != VK_FORMAT_UNDEFINED && depth.vulkan_buffer != nullptr)
	{
		plan->depth.image = depth.vulkan_buffer;
		plan->depth.planes = depth.vaddr_num;
		std::copy_n(depth.vaddr, 3, plan->depth.address);
		std::copy_n(depth.size, 3, plan->depth.size);
		plan->depth.read_only = UniformDepthZeroReadOnlyDraw(depth, pipeline);
		plan->depth.depth_clear = depth.depth_clear_enable;
		plan->depth.stencil_clear = depth.stencil_clear_enable;
		plan->depth.potential_write = (draw_recorded && !plan->depth.read_only) || depth.depth_clear_enable || depth.stencil_clear_enable;
	}
}

enum class DrawAccessEffect { VideoOutRead, ShaderGroups, ColorRenderTexture, ColorVideoOut, Depth, ResetDepthHash };

template <typename Visitor>
static void VisitDrawShaderEffects(const GpuMemoryDrawAccessPlan& plan, bool access_tracking, Visitor&& visit)
{
	if (!plan.draw_recorded) return;
	for (uint32_t i = 0; i < plan.sampled_videoout_count; ++i) visit(DrawAccessEffect::VideoOutRead, i);
	if (access_tracking && (plan.image_count != 0 || plan.storage_count != 0)) visit(DrawAccessEffect::ShaderGroups, 0);
}

template <typename Visitor>
static void VisitDrawAttachmentEffects(const GpuMemoryDrawAccessPlan& plan, Visitor&& visit)
{
	if (plan.color.kind == GpuMemoryDrawColorKind::RenderTexture) visit(DrawAccessEffect::ColorRenderTexture, 0);
	else if (plan.color.kind == GpuMemoryDrawColorKind::VideoOut) visit(DrawAccessEffect::ColorVideoOut, 0);
	if (plan.depth.image != nullptr)
	{
		visit(DrawAccessEffect::Depth, 0);
		visit(DrawAccessEffect::ResetDepthHash, 0);
	}
}

class DrawAccessTransaction final
{
public:
	DrawNativeInventory inventory;
	~DrawAccessTransaction() { EXIT_IF(m_token != 0 && !m_finished); }

	void Prepare(CommandBuffer* buffer, uint64_t submit_id, bool draw_recorded,
	             const RenderColorInfo& color, const RenderDepthInfo& depth, const VulkanPipeline* pipeline,
	             const VulkanFramebuffer* framebuffer, const GpuMemoryImageAccessBatch& images,
	             const GpuMemoryStorageAccessBatch& storage)
	{
		EXIT_IF(m_buffer != nullptr);
		m_buffer = buffer;
		auto& plan = inventory.accesses;
		BuildDrawAttachmentPlan(&plan, submit_id, draw_recorded, color, depth, pipeline);
		plan.image_count = images.count;
		plan.storage_count = storage.count;
		plan.sampled_videoout_count = images.sampled_videoout_count;
		static_assert(GpuMemoryImageAccessBatch::MAX <= GpuMemoryDrawAccessPlan::MAX_SHADER);
		static_assert(GpuMemoryStorageAccessBatch::MAX <= GpuMemoryDrawAccessPlan::MAX_SHADER);
		std::copy_n(images.accesses, images.count, plan.images.begin());
		std::copy_n(storage.accesses, storage.count, plan.storage.begin());
		std::copy_n(images.sampled_videoout_handles, images.sampled_videoout_count, plan.sampled_videoout.begin());
		inventory.AddRendererObject(pipeline);
		inventory.AddRendererObject(framebuffer);
		m_descriptors = buffer->PeekDescriptorBatch();
		m_uploads = buffer->PeekUploadRecordingBatch();
		const char* reason = "native-inventory-capacity";
		if (!inventory.complete || !GpuMemoryPrepareDrawAccesses(buffer, plan, &m_proof, &reason))
		{
			static std::atomic<uint32_t> fallbacks {0};
			if (fallbacks.fetch_add(1, std::memory_order_relaxed) < 8)
				std::fprintf(stderr, "[GFX_DRAW_ACCESS_PLAN] fallback=%s epoch=exclusive\n", reason != nullptr ? reason : "unknown");
			return;
		}
		EXIT_IF(!RecordingResourcesValid());
		m_token = buffer->ReserveDrawAccesses(m_proof.ExpectedCommandOrdinals());
	}

	bool Reserved() const { return m_token != 0; }
	void CommitShader()
	{
		EXIT_IF(!Reserved() || m_shader_committed || m_finished || !RecordingResourcesValid());
		const char* reason = nullptr;
		if (!GpuMemoryValidateDrawAccesses(m_buffer, inventory.accesses, m_proof, &reason))
			EXIT("draw access plan changed after reservation: %s\n", reason != nullptr ? reason : "unknown");
		m_buffer->BeginDrawAccessCommit(m_token);
		const auto& plan = inventory.accesses;
		VisitDrawShaderEffects(plan, GpuMemoryAccessTrackingEnabled(), [&](DrawAccessEffect effect, uint32_t index) {
			if (effect == DrawAccessEffect::VideoOutRead)
				EXIT_NOT_IMPLEMENTED(!GpuMemoryRecordVideoOutBufferRead(m_buffer, plan.sampled_videoout[index]));
			else
			{
				EXIT_IF(effect != DrawAccessEffect::ShaderGroups);
				GpuMemoryRecordCommandAccessGroups(m_buffer, plan.images.data(), plan.image_count,
				    GpuMemoryCommandMutationPolicy::PotentialStorageWrite, plan.storage.data(), plan.storage_count);
			}
		});
		m_shader_committed = true;
	}
	void CommitAttachments()
	{
		EXIT_IF(!Reserved() || !m_shader_committed || m_finished || !RecordingResourcesValid());
		const auto& plan = inventory.accesses;
		const auto& color = plan.color;
		const auto& depth = plan.depth;
		VisitDrawAttachmentEffects(plan, [&](DrawAccessEffect effect, uint32_t) {
			switch (effect)
			{
				case DrawAccessEffect::ColorRenderTexture:
					EXIT_NOT_IMPLEMENTED(!GpuMemoryRecordRenderTexturePass(m_buffer, color.handle, color.address, color.size, plan.draw_recorded)); break;
				case DrawAccessEffect::ColorVideoOut:
					EXIT_NOT_IMPLEMENTED(!GpuMemoryRecordVideoOutBufferPass(m_buffer, color.handle, color.address, color.size, plan.draw_recorded)); break;
				case DrawAccessEffect::Depth:
					EXIT_NOT_IMPLEMENTED(!GpuMemoryRecordDepthStencilPass(m_buffer, depth.image, depth.address, depth.size,
					    depth.planes, depth.potential_write, depth.read_only)); break;
				case DrawAccessEffect::ResetDepthHash:
					GpuMemoryResetHash(depth.address, depth.size, depth.planes, GpuMemoryObjectType::DepthStencilBuffer,
					    m_buffer, plan.submit_id, depth.read_only); break;
				default: EXIT("invalid attachment effect\n");
			}
		});
		m_buffer->FinishDrawAccessCommit(m_token);
		m_finished = true;
		static std::atomic<uint64_t> commits {0};
		const auto count = commits.fetch_add(1, std::memory_order_relaxed) + 1;
		if (count == 1 || count == 1024 || count == 65536)
			std::fprintf(stderr, "[GFX_DRAW_ACCESS_PLAN] committed=%" PRIu64 " native=%u shader=%u ordinals=%u epoch=exclusive\n",
			    count, plan.native_input_count, plan.image_count + plan.storage_count, m_proof.ExpectedCommandOrdinals());
	}
private:
	bool RecordingResourcesValid() const
	{
		if (!GraphicsRenderRetainedRecordingScopeValid(m_buffer) ||
		    !GraphicsRunDrawAccessReservationScopeValid(m_buffer->GetParent(), m_buffer) ||
		    m_buffer->PeekDescriptorBatch() != m_descriptors || m_buffer->PeekUploadRecordingBatch() != m_uploads) return false;
		if (m_descriptors != nullptr && (!m_descriptors->active || m_descriptors->command != m_buffer)) return false;
		if (inventory.uniform.buffer != VK_NULL_HANDLE)
		{
			if (m_descriptors == nullptr || inventory.uniform.range == 0) return false;
			bool owned = false;
			for (const auto& arena: m_descriptors->constant_buffers)
				owned |= arena.buffer != nullptr && arena.buffer->buffer == inventory.uniform.buffer &&
				    inventory.uniform.offset <= arena.used && inventory.uniform.range <= arena.used - inventory.uniform.offset;
			if (!owned) return false;
		}
		return true;
	}
	GpuMemoryDrawAccessProof m_proof;
	CommandBuffer* m_buffer = nullptr;
	DescriptorRecordingBatch* m_descriptors = nullptr;
	UploadRecordingBatch* m_uploads = nullptr;
	uint64_t m_token = 0;
	bool m_shader_committed = false, m_finished = false;
};

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
static bool GraphicsRenderDrawDepthPlanContractTest()
{
	RenderDepthInfo depth;
	depth.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
	depth.vulkan_buffer = reinterpret_cast<DepthStencilVulkanImage*>(uintptr_t {0x1000});
	depth.vaddr_num = 3;
	depth.vaddr[0] = 0x10000; depth.vaddr[1] = 0x20000; depth.vaddr[2] = 0x30000;
	depth.size[0] = 4096; depth.size[1] = 2048; depth.size[2] = 1024;
	depth.uniform_depth_read_only = true;
	PipelineStaticParameters parameters;
	parameters.with_depth = true;
	PipelineDynamicParameters dynamic;
	dynamic.vk_dynamic_state_stencil_write_mask = true;
	VulkanPipeline pipeline;
	pipeline.static_params = &parameters;
	pipeline.dynamic_params = &dynamic;
	pipeline.ps_depth_stencil_export_free = true;
	const RenderColorInfo no_color;
	const auto agrees = [&](const GpuMemoryDrawAccessPlan& plan, const RenderDepthInfo& source,
	                        bool draw, bool read_only, bool write) {
		if (plan.submit_id != 17 || plan.draw_recorded != draw || plan.depth.image != source.vulkan_buffer ||
		    plan.depth.planes != 3 || plan.depth.read_only != read_only || plan.depth.potential_write != write ||
		    plan.depth.depth_clear != source.depth_clear_enable || plan.depth.stencil_clear != source.stencil_clear_enable) return false;
		for (uint32_t plane = 0; plane < 3; ++plane)
			if (plan.depth.address[plane] != source.vaddr[plane] || plan.depth.size[plane] != source.size[plane]) return false;
		return true;
	};
	// No stencil, effective KEEP, and effective zero write masks are distinct
	// native read-only proofs. Every clear must still write on a zero-count draw.
	for (uint32_t mode = 0; mode < 3; ++mode)
	{
		depth.stencil_test_enable = parameters.stencil_test_enable = mode != 0;
		depth.stencil_static_front.passOp = parameters.stencil_front.passOp =
		    mode == 2 ? VK_STENCIL_OP_REPLACE : VK_STENCIL_OP_KEEP;
		depth.stencil_static_back.passOp = parameters.stencil_back.passOp = parameters.stencil_front.passOp;
		pipeline.dynamic_params = mode == 2 ? &dynamic : nullptr;
		for (uint32_t mask = 0; mask < 8; ++mask)
		{
			auto current = depth;
			const bool draw = (mask & 1) != 0;
			current.depth_clear_enable = (mask & 2) != 0;
			current.stencil_clear_enable = (mask & 4) != 0;
			const bool clear = (mask & 6) != 0;
			GpuMemoryDrawAccessPlan plan;
			BuildDrawAttachmentPlan(&plan, 17, draw, no_color, current, &pipeline);
			if (!agrees(plan, current, draw, !clear, clear)) return false;
		}
	}
	// Start from actual REPLACE operations with emitted zero masks. Mutating
	// the bound pipeline must defeat a stale requested read-only classification.
	for (uint32_t mutation = 0; mutation < 19; ++mutation)
	{
		for (const bool draw: {false, true})
		{
			auto current = depth;
			auto native = parameters;
			auto emitted = dynamic;
			auto bound = pipeline;
			bound.static_params = &native;
			bound.dynamic_params = &emitted;
			switch (mutation)
			{
				case 0: break; // No effective pipeline at all.
				case 1: bound.static_params = nullptr; break;
				case 2: bound.ps_depth_stencil_export_free = false; break;
				case 3: native.depth_write_enable = true; break;
				case 4: native.depth_bounds_test_enable = true; break;
				case 5: native.with_depth = false; break;
				case 6: native.stencil_test_enable = false; break;
				case 7: bound.dynamic_params = nullptr; break;
				case 8: emitted.vk_dynamic_state_stencil_write_mask = false; break;
				case 9: emitted.stencil_front.writeMask = 0xff; break;
				case 10: emitted.stencil_front.writeMask = current.stencil_dynamic_front.writeMask = 0xff; break;
				case 11: ++emitted.stencil_back.reference; break;
				case 12: native.stencil_back.passOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP; break;
				case 13: current.uniform_depth_read_only = false; break;
				case 14: current.depth_write_enable = true; break;
				case 15: current.depth_bounds_test_enable = true; break;
				case 16: current.format = VK_FORMAT_D32_SFLOAT; break;
				case 17: ++emitted.stencil_front.compareMask; break;
				case 18: emitted.stencil_back.writeMask = current.stencil_dynamic_back.writeMask = 0xff; break;
			}
			GpuMemoryDrawAccessPlan plan;
			BuildDrawAttachmentPlan(&plan, 17, draw, no_color, current, mutation == 0 ? nullptr : &bound);
			if (!agrees(plan, current, draw, false, draw)) return false;
		}
	}
	for (const bool absent_image: {false, true})
	{
		auto absent = depth;
		absent.depth_clear_enable = absent.stencil_clear_enable = true;
		if (absent_image) absent.vulkan_buffer = nullptr;
		else absent.format = VK_FORMAT_UNDEFINED;
		GpuMemoryDrawAccessPlan plan;
		BuildDrawAttachmentPlan(&plan, 17, true, no_color, absent, &pipeline);
		if (plan.depth.image != nullptr || plan.depth.planes != 0 || plan.depth.potential_write ||
		    plan.depth.read_only || plan.depth.depth_clear || plan.depth.stencil_clear) return false;
	}
	for (const auto type: {RenderColorType::NoColorOutput, RenderColorType::DisplayBuffer, RenderColorType::RenderTexture})
		for (const bool present: {false, true})
			for (const bool draw: {false, true})
			{
				RenderColorInfo color;
				color.type = type;
				color.vulkan_buffer = present ? reinterpret_cast<VulkanImage*>(uintptr_t {0x2000}) : nullptr;
				color.tracked_handle = {color.vulkan_buffer, 5, 2, 3};
				color.base_addr = 0x40000;
				color.buffer_size = 4096;
				GpuMemoryDrawAccessPlan plan;
				BuildDrawAttachmentPlan(&plan, 17, draw, color, {}, nullptr);
				const auto expected = !present || type == RenderColorType::NoColorOutput ? GpuMemoryDrawColorKind::None :
				    type == RenderColorType::DisplayBuffer ? GpuMemoryDrawColorKind::VideoOut : GpuMemoryDrawColorKind::RenderTexture;
				if (plan.color.kind != expected || plan.draw_recorded != draw || plan.depth.image != nullptr) return false;
				if (present && (plan.color.handle.host_object != color.tracked_handle.host_object ||
				    plan.color.handle.lifetime_generation != color.tracked_handle.lifetime_generation ||
				    plan.color.handle.heap_id != color.tracked_handle.heap_id || plan.color.handle.object_id != color.tracked_handle.object_id ||
				    plan.color.address != color.base_addr || plan.color.size != color.buffer_size)) return false;
				if (!present && (plan.color.handle.host_object != nullptr || plan.color.handle.lifetime_generation != 0 ||
				    plan.color.address != 0 || plan.color.size != 0)) return false;
			}
	return true;
}

static bool GraphicsRenderDrawEffectSequenceContractTest()
{
	// Literal legacy schedules: sampled reads precede the single shader group;
	// attachment publication is a later phase and remains present at zero count.
	constexpr const char* shader_schedule[2][2] = {{"", "S"}, {"01", "01S"}};
	constexpr const char* attachment_schedule[3][2] = {{"", "DH"}, {"R", "RDH"}, {"V", "VDH"}};
	const GpuMemoryDrawColorKind colors[] = {GpuMemoryDrawColorKind::None,
	    GpuMemoryDrawColorKind::RenderTexture, GpuMemoryDrawColorKind::VideoOut};
	for (uint32_t scenario = 0; scenario < 192; ++scenario)
	{
		uint32_t fields = scenario;
		const bool draw = (fields % 2) != 0; fields /= 2;
		const bool tracking = (fields % 2) != 0; fields /= 2;
		const uint32_t sampled = fields % 2; fields /= 2;
		const uint32_t resources = fields % 4; fields /= 4;
		const uint32_t color = fields % 3; fields /= 3;
		const uint32_t depth = fields % 2;
		GpuMemoryDrawAccessPlan plan;
		plan.draw_recorded = draw;
		plan.sampled_videoout_count = sampled != 0 ? 2 : 0;
		plan.image_count = (resources & 1) != 0 ? 2 : 0;
		plan.storage_count = (resources & 2) != 0 ? 3 : 0;
		plan.color.kind = colors[color];
		if (depth != 0) plan.depth.image = reinterpret_cast<VulkanImage*>(uintptr_t {0x1000});
		std::array<char, 8> effects {};
		uint32_t effect_count = 0;
		bool valid = true;
		const auto record = [&](DrawAccessEffect effect, uint32_t index) {
			if (effect_count == effects.size()) { valid = false; return; }
			char code = '?';
			switch (effect)
			{
				case DrawAccessEffect::VideoOutRead:
					if (index > 1) valid = false;
					code = index == 0 ? '0' : '1';
					break;
				case DrawAccessEffect::ShaderGroups: code = 'S'; break;
				case DrawAccessEffect::ColorRenderTexture: code = 'R'; break;
				case DrawAccessEffect::ColorVideoOut: code = 'V'; break;
				case DrawAccessEffect::Depth: code = 'D'; break;
				case DrawAccessEffect::ResetDepthHash: code = 'H'; break;
			}
			if (effect != DrawAccessEffect::VideoOutRead && index != 0) valid = false;
			effects[effect_count++] = code;
		};
		VisitDrawShaderEffects(plan, tracking, record);
		const char* expected_shader = draw ? shader_schedule[sampled][tracking && resources != 0 ? 1 : 0] : "";
		const size_t shader_count = std::strlen(expected_shader);
		if (!valid || effect_count != shader_count || std::memcmp(effects.data(), expected_shader, shader_count) != 0) return false;
		VisitDrawAttachmentEffects(plan, record);
		const char* expected_attachment = attachment_schedule[color][depth];
		const size_t attachment_count = std::strlen(expected_attachment);
		if (!valid || effect_count != shader_count + attachment_count ||
		    std::memcmp(effects.data(), expected_shader, shader_count) != 0 ||
		    std::memcmp(effects.data() + shader_count, expected_attachment, attachment_count) != 0) return false;
	}
	return true;
}

bool GraphicsRenderDrawAccessPlanContractTest()
{
	if (!GraphicsRenderDrawDepthPlanContractTest() || !GraphicsRenderDrawEffectSequenceContractTest()) return false;
	RenderColorInfo color;
	RenderDepthInfo depth;
	depth.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
	depth.vulkan_buffer = reinterpret_cast<DepthStencilVulkanImage*>(uintptr_t {0x1000});
	depth.vaddr_num = 3;
	depth.vaddr[0] = 0x10000; depth.vaddr[1] = 0x20000; depth.vaddr[2] = 0x30000;
	depth.size[0] = 4096; depth.size[1] = 2048; depth.size[2] = 1024;
	for (unsigned mask = 0; mask < 8; ++mask)
	{
		GpuMemoryDrawAccessPlan plan;
		depth.depth_clear_enable = (mask & 2) != 0;
		depth.stencil_clear_enable = (mask & 4) != 0;
		BuildDrawAttachmentPlan(&plan, 17, (mask & 1) != 0, color, depth, nullptr);
		if (plan.depth.potential_write != (mask != 0) || plan.depth.read_only || plan.depth.planes != 3 ||
		    plan.depth.image != depth.vulkan_buffer || plan.depth.address[2] != depth.vaddr[2] ||
		    plan.depth.size[1] != depth.size[1] || plan.submit_id != 17 || plan.draw_recorded != ((mask & 1) != 0)) return false;
	}
	DrawNativeInventory inventory;
	for (uint32_t i = 0; i < GpuMemoryDrawAccessPlan::MAX_NATIVE; ++i)
		inventory.Add(reinterpret_cast<void*>(uintptr_t {0x1000}), GpuMemoryObjectType::VertexBuffer, {}, 0x10000, 64);
	if (!inventory.complete || inventory.accesses.native_input_count != GpuMemoryDrawAccessPlan::MAX_NATIVE) return false;
	inventory.Add(reinterpret_cast<void*>(uintptr_t {0x1000}), GpuMemoryObjectType::IndexBuffer);
	return !inventory.complete && inventory.accesses.native_input_count == GpuMemoryDrawAccessPlan::MAX_NATIVE &&
	       DrawImageObjectType(VulkanImageType::DepthStencil) == GpuMemoryObjectType::DepthStencilBuffer;
}

bool GraphicsRenderVideoOutSampledAliasBarrierContractTest()
{
	const auto begin   = GetVideoOutSampledAliasBarrierPolicy(false);
	const auto restore = GetVideoOutSampledAliasBarrierPolicy(true);
	GpuMemoryImageAccessBatch batch;
	auto* first  = reinterpret_cast<VideoOutVulkanImage*>(uintptr_t {0x1000});
	auto* second = reinterpret_cast<VideoOutVulkanImage*>(uintptr_t {0x2000});
	const GpuMemoryObjectHandle first_handle {first, 1, 0, 0};
	const GpuMemoryObjectHandle second_handle {second, 2, 0, 1};
	batch.AddSampledVideoOut(first, first_handle);
	batch.AddSampledVideoOut(second, second_handle);
	return batch.sampled_videoout_count == 2 && batch.sampled_videoout[0] == first &&
	       batch.sampled_videoout[1] == second && batch.FindSampledVideoOut(first_handle) == first &&
	       batch.FindSampledVideoOut(second_handle) == second &&
	       begin.old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
	       begin.new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
	       begin.src_stage_mask == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT &&
	       begin.dst_stage_mask == VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT &&
	       begin.src_access_mask == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT) &&
	       begin.dst_access_mask == VK_ACCESS_SHADER_READ_BIT &&
	       restore.old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
	       restore.new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
	       restore.src_stage_mask == VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT &&
	       restore.dst_stage_mask == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT &&
	       restore.src_access_mask == VK_ACCESS_SHADER_READ_BIT &&
	       restore.dst_access_mask == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT) &&
	       GuestRangesOverlapFailClosed(0x1000, 0x100, 0x1080, 0x100) &&
	       !GuestRangesOverlapFailClosed(0x1000, 0x100, 0x1100, 0x100) &&
	       GuestRangesOverlapFailClosed(UINT64_MAX - 0x10, 0x20, 0x1000, 0x100);
}
#endif

struct TextureThin1dPrefixPreparationProfile
{
	uint64_t base = 0;
	uint64_t size = 0;
	uint8_t  format = 0;
	bool     padded_destination = false;
};

static bool GetTextureThin1dPrefixPreparationProfile(const ShaderTextureDescriptor& descriptor,
	                                                  TextureThin1dPrefixPreparationProfile* profile)
{
	if (profile == nullptr)
	{
		return false;
	}
	const auto& texture = descriptor.texture;
	const uint32_t width  = static_cast<uint32_t>(texture.Width4()) + 1u;
	const uint32_t height = static_cast<uint32_t>(texture.Height4()) + 1u;
	const uint32_t pitch  = static_cast<uint32_t>(texture.Pitch()) + 1u;
	const uint32_t bytes_per_element = texture.Dfmt() == 1 ? 1u : texture.Dfmt() == 10 ? 4u : 0u;
	if (texture.Type() != ShaderTextureResource::TYPE_2D || bytes_per_element == 0 || texture.Nfmt() != 0 ||
	    texture.TileMode() != 13 || texture.BaseLevel() != 0 || texture.LastLevel() != 0 ||
	    texture.Interlaced() || texture.Depth() != 0 || texture.BaseArray() != 0 || texture.LastArray() != 0 ||
	    texture.Base38() == 0)
	{
		return false;
	}
	if (texture.Pow2Pad() || pitch != width || (width & 7u) != 0 || (height & 7u) != 0)
	{
		uint64_t size = 0;
		if (descriptor.usage != ShaderTextureUsage::ReadWrite || !descriptor.textures2d_without_sampler ||
		    texture.Dfmt() != 10 || texture.DstSelXYZW() != DstSel(4, 5, 6, 7) ||
		    !TextureThin1dSingleLevelRgba8DestinationSize(width, height, pitch, texture.Pow2Pad(), &size))
		{
			return false;
		}
		*profile = {texture.Base38(), size, texture.Dfmt(), true};
		return true;
	}
	if (pitch != width || (width & 7u) != 0 || (height & 7u) != 0)
	{
		return false;
	}
	const uint64_t elements = static_cast<uint64_t>(pitch) * height;
	if (elements == 0 || elements > UINT64_MAX / bytes_per_element)
	{
		return false;
	}
	*profile = {texture.Base38(), elements * bytes_per_element, texture.Dfmt(), false};
	return profile->size != 0;
}

static bool TextureThin1dPrefixMustPrecede(const ShaderTextureDescriptor& candidate,
	                                        const ShaderTextureDescriptor& current)
{
	TextureThin1dPrefixPreparationProfile candidate_profile {};
	TextureThin1dPrefixPreparationProfile current_profile {};
	return GetTextureThin1dPrefixPreparationProfile(candidate, &candidate_profile) &&
	       GetTextureThin1dPrefixPreparationProfile(current, &current_profile) &&
	       candidate_profile.base == current_profile.base &&
	       (candidate_profile.format != current_profile.format || candidate_profile.format == 10) &&
	       !candidate_profile.padded_destination &&
	       candidate_profile.size > current_profile.size;
}

// Swap only the endpoints of an exact alias dependency. Descriptors which do
// not participate retain their processing positions, including descriptors
// between a reversed source/destination pair.
static void ApplyTextureThin1dPrefixPreparationOrder(const ShaderTextureResources& textures, int* processing_order)
{
	EXIT_IF(processing_order == nullptr);
	const int count = textures.textures_num;
	for (int pass = 0; pass < ShaderTextureResources::RES_MAX * ShaderTextureResources::RES_MAX; ++pass)
	{
		bool changed = false;
		for (int current = 0; current < count; ++current)
		{
			for (int candidate = current + 1; candidate < count; ++candidate)
			{
				if (TextureThin1dPrefixMustPrecede(textures.desc[processing_order[candidate]],
				                                      textures.desc[processing_order[current]]))
				{
					std::swap(processing_order[current], processing_order[candidate]);
					changed = true;
				}
			}
		}
		if (!changed)
		{
			break;
		}
	}
}

static void BuildTexturePreparationOrder(const ShaderTextureResources& textures, bool adjacent_storage_mip_profile,
	                                      int* processing_order)
{
	EXIT_IF(processing_order == nullptr);
	for (int index = 0; index < textures.textures_num; ++index)
	{
		processing_order[index] = index;
	}
	if (adjacent_storage_mip_profile)
	{
		int sampled_index = -1;
		for (int index = 0; index < textures.textures_num; ++index)
		{
			if (textures.desc[index].usage == ShaderTextureUsage::ReadOnly &&
			    !textures.desc[index].textures2d_without_sampler)
			{
				sampled_index = index;
				break;
			}
		}
		EXIT_IF(sampled_index < 0);
		std::swap(processing_order[0], processing_order[sampled_index]);
	}
	ApplyTextureThin1dPrefixPreparationOrder(textures, processing_order);
}

#if defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
bool GraphicsRenderTexturePreparationOrderContractTest()
{
	const auto make_descriptor = [](uint64_t base, uint32_t format, uint32_t width, uint32_t height,
	                                uint32_t pitch = 0, bool pow2_pad = false)
	{
		if (pitch == 0)
		{
			pitch = width;
		}
		ShaderTextureDescriptor descriptor {};
		descriptor.texture.fields[1] = format << 20u;
		descriptor.texture.fields[2] = (width - 1u) | ((height - 1u) << 14u);
		descriptor.texture.fields[3] = (ShaderTextureResource::TYPE_2D << 28u) | (13u << 20u) |
		                               (pow2_pad ? 1u << 25u : 0u) | DstSel(4, 5, 6, 7);
		descriptor.texture.fields[4] = (pitch - 1u) << 13u;
		descriptor.texture.UpdateAddress38(base);
		descriptor.usage                      = ShaderTextureUsage::ReadWrite;
		descriptor.textures2d_without_sampler = true;
		return descriptor;
	};
	const auto order_is = [](const ShaderTextureResources& textures, int first, int second)
	{
		int order[ShaderTextureResources::RES_MAX] {};
		BuildTexturePreparationOrder(textures, false, order);
		return order[0] == first && order[1] == second;
	};
	constexpr uint64_t base = 0x400000u;

	const auto large_r8    = make_descriptor(base, 1, 512, 512);
	const auto small_rgba8 = make_descriptor(base, 10, 48, 48);
	const auto large_rgba8 = make_descriptor(base, 10, 1920, 1080);
	const auto small_r8    = make_descriptor(base, 1, 512, 512);
	const auto padded_rgba8 = make_descriptor(base, 10, 182, 5, 256, true);
	const auto reshaped_rgba8 = make_descriptor(base, 10, 1503, 46, 2048, true);
	const auto partial_rgba8 = make_descriptor(base, 10, 74, 68, 80);

	ShaderTextureResources forward_destination_first {};
	forward_destination_first.textures_num = 2;
	forward_destination_first.desc[0]       = small_rgba8;
	forward_destination_first.desc[1]       = large_r8;
	if (!order_is(forward_destination_first, 1, 0))
	{
		return false;
	}
	ShaderTextureResources forward_source_first = forward_destination_first;
	std::swap(forward_source_first.desc[0], forward_source_first.desc[1]);
	if (!order_is(forward_source_first, 0, 1))
	{
		return false;
	}
	ShaderTextureResources padded_forward_destination_first {};
	padded_forward_destination_first.textures_num = 2;
	padded_forward_destination_first.desc[0]       = padded_rgba8;
	padded_forward_destination_first.desc[1]       = large_r8;
	if (!order_is(padded_forward_destination_first, 1, 0))
	{
		return false;
	}
	ShaderTextureResources partial_destination_first {};
	partial_destination_first.textures_num = 2;
	partial_destination_first.desc[0] = partial_rgba8;
	partial_destination_first.desc[1] = large_r8;
	if (!order_is(partial_destination_first, 1, 0)) return false;
	std::swap(partial_destination_first.desc[0], partial_destination_first.desc[1]);
	if (!order_is(partial_destination_first, 0, 1)) return false;
	auto partial_same_format = partial_destination_first;
	partial_same_format.desc[0] = partial_rgba8;
	partial_same_format.desc[1] = large_rgba8;
	if (!order_is(partial_same_format, 1, 0)) return false;
	for (unsigned fault = 0; fault < 4; ++fault)
	{
		auto unsupported_partial = partial_destination_first;
		std::swap(unsupported_partial.desc[0], unsupported_partial.desc[1]);
		auto& destination = unsupported_partial.desc[0];
		switch (fault)
		{
			case 0: destination.usage = ShaderTextureUsage::ReadOnly; break;
			case 1: destination.textures2d_without_sampler = false; break;
			case 2: destination.texture.fields[4] = (88u - 1u) << 13u; break;
			case 3: destination.texture.fields[3] ^= 1u << 20u; break;
		}
		if (!order_is(unsupported_partial, 0, 1)) return false;
	}
	// A partial destination must never become a larger authoritative source.
	ShaderTextureResources partial_source {};
	partial_source.textures_num = 2;
	partial_source.desc[0] = small_rgba8;
	partial_source.desc[1] = partial_rgba8;
	if (!order_is(partial_source, 0, 1)) return false;
	ShaderTextureResources padded_source {};
	padded_source.textures_num = 2;
	padded_source.desc[0]       = make_descriptor(base, 1, 64, 64);
	padded_source.desc[1]       = padded_rgba8;
	if (!order_is(padded_source, 0, 1))
	{
		return false;
	}

	ShaderTextureResources reverse_destination_first {};
	reverse_destination_first.textures_num = 2;
	reverse_destination_first.desc[0]       = small_r8;
	reverse_destination_first.desc[1]       = large_rgba8;
	if (!order_is(reverse_destination_first, 1, 0))
	{
		return false;
	}
	ShaderTextureResources reverse_source_first = reverse_destination_first;
	std::swap(reverse_source_first.desc[0], reverse_source_first.desc[1]);
	if (!order_is(reverse_source_first, 0, 1))
	{
		return false;
	}

	// Same-format Thin1D images can address the same byte prefix with a
	// different pitch. Prepare every larger consumer before the bridge retires
	// its tracker entry; its host image remains alive through the current fence.
	ShaderTextureResources same_format_destination_first {};
	same_format_destination_first.textures_num = 2;
	same_format_destination_first.desc[0]       = reshaped_rgba8;
	same_format_destination_first.desc[1]       = large_rgba8;
	if (!order_is(same_format_destination_first, 1, 0))
	{
		return false;
	}
	auto same_format_source_first = same_format_destination_first;
	std::swap(same_format_source_first.desc[0], same_format_source_first.desc[1]);
	if (!order_is(same_format_source_first, 0, 1))
	{
		return false;
	}
	auto same_format_strict = same_format_destination_first;
	same_format_strict.desc[0] = small_rgba8;
	if (!order_is(same_format_strict, 1, 0))
	{
		return false;
	}

	ShaderTextureResources unrelated_between = reverse_destination_first;
	unrelated_between.textures_num = 3;
	unrelated_between.desc[2]       = unrelated_between.desc[1];
	unrelated_between.desc[1]       = make_descriptor(base + 0x1000000u, 1, 64, 64);
	int unrelated_order[ShaderTextureResources::RES_MAX] {};
	BuildTexturePreparationOrder(unrelated_between, false, unrelated_order);
	if (unrelated_order[0] != 2 || unrelated_order[1] != 1 || unrelated_order[2] != 0)
	{
		return false;
	}
	unrelated_between.desc[0] = reshaped_rgba8;
	BuildTexturePreparationOrder(unrelated_between, false, unrelated_order);
	if (unrelated_order[0] != 2 || unrelated_order[1] != 1 || unrelated_order[2] != 0)
	{
		return false;
	}

	auto same_format_r8 = same_format_destination_first;
	same_format_r8.desc[0] = make_descriptor(base, 1, 64, 64);
	same_format_r8.desc[1] = large_r8;
	auto same_format_padded_source = same_format_destination_first;
	same_format_padded_source.desc[0] = small_rgba8;
	same_format_padded_source.desc[1] = reshaped_rgba8;
	auto same_format_equal_size = same_format_destination_first;
	same_format_equal_size.desc[0] = make_descriptor(base, 10, 16, 8);
	same_format_equal_size.desc[1] = make_descriptor(base, 10, 8, 16);
	auto same_format_different_base = same_format_destination_first;
	same_format_different_base.desc[1].texture.UpdateAddress38(base + 0x1000000u);
	if (!order_is(same_format_r8, 0, 1) || !order_is(same_format_padded_source, 0, 1) ||
	    !order_is(same_format_equal_size, 0, 1) || !order_is(same_format_different_base, 0, 1))
	{
		return false;
	}
	for (unsigned fault = 0; fault < 4; ++fault)
	{
		auto unsupported_same_format = same_format_destination_first;
		auto& destination = unsupported_same_format.desc[0];
		switch (fault)
		{
			case 0: destination.usage = ShaderTextureUsage::ReadOnly; break;
			case 1: destination.textures2d_without_sampler = false; break;
			case 2: destination.texture.fields[4] = (4096u - 1u) << 13u; break;
			case 3: destination.texture.fields[3] ^= 1u << 20u; break;
		}
		if (!order_is(unsupported_same_format, 0, 1))
		{
			return false;
		}
	}

	ShaderTextureResources equal_size {};
	equal_size.textures_num = 2;
	equal_size.desc[0]       = make_descriptor(base, 1, 16, 32);
	equal_size.desc[1]       = make_descriptor(base, 10, 8, 16);
	if (!order_is(equal_size, 0, 1))
	{
		return false;
	}
	auto unsupported = reverse_destination_first;
	unsupported.desc[1].texture.fields[3] ^= 1u << 20u;
	if (!order_is(unsupported, 0, 1))
	{
		return false;
	}
	auto different_base = reverse_destination_first;
	different_base.desc[1].texture.UpdateAddress38(base + 0x1000000u);
	return order_is(different_base, 0, 1);
}
#endif

static bool VideoOutSampleAliasSelfFeedback(const RenderColorInfo* current_color,
	                                         const VulkanImage* sampled_image, uint64_t sampled_addr,
	                                         uint64_t sampled_size)
{
	if (current_color == nullptr || current_color->vulkan_buffer == nullptr)
	{
		return false;
	}
	return current_color->vulkan_buffer == sampled_image ||
	       GuestRangesOverlapFailClosed(current_color->base_addr, current_color->buffer_size,
	                                    sampled_addr, sampled_size);
}

static bool Bc7SampledTextureDescriptorAllowed(const ShaderTextureDescriptor& descriptor)
{
	const auto& r = descriptor.texture;
	return descriptor.usage == ShaderTextureUsage::ReadOnly && !descriptor.textures2d_without_sampler &&
	       r.MemoryType() == 0x10 && r.Type() == ShaderTextureResource::TYPE_2D && r.Depth() == 0 &&
	       r.BaseArray() == 0 && r.LastArray() == 0 &&
	       ResolveBc7Thin1dLayout(r.Dfmt(), r.Nfmt(), r.Width4() + 1u, r.Height4() + 1u, r.Pitch() + 1u,
	                             r.BaseLevel(), r.LastLevel() + 1u, r.TileMode(), r.Pow2Pad());
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool GraphicsRenderBc7TextureProfileContractTest()
{
	ShaderTextureDescriptor descriptor {};
	descriptor.usage = ShaderTextureUsage::ReadOnly;
	descriptor.texture.fields[0] = 1;
	descriptor.texture.fields[1] = 41u << 20u;
	descriptor.texture.fields[2] = 31u | (31u << 14u);
	descriptor.texture.fields[3] = (ShaderTextureResource::TYPE_2D << 28u) | (1u << 26u) | (13u << 20u) | DstSel(4, 5, 6, 7);
	descriptor.texture.fields[4] = 31u << 13u;
	if (!Bc7SampledTextureDescriptorAllowed(descriptor)) return false;
	for (int variation = 0; variation < 16; ++variation)
	{
		auto changed = descriptor;
		switch (variation)
		{
			case 0: changed.usage = ShaderTextureUsage::ReadWrite; break;
			case 1: changed.textures2d_without_sampler = true; break;
			case 2: changed.texture.fields[1] = 40u << 20u; break;
			case 3: changed.texture.fields[1] |= 9u << 26u; break;
			case 4: changed.texture.fields[3] = (changed.texture.fields[3] & 0x0fffffffu) | (8u << 28u); break;
			case 5: changed.texture.fields[4] |= 1u; break;
			case 6: changed.texture.fields[5] = 1u; break;
			case 7: changed.texture.fields[5] = 1u << 13u; break;
			case 8: changed.texture.fields[3] |= 1u << 12u; break;
			case 9: changed.texture.fields[3] |= 1u << 16u; break;
			case 10: changed.texture.fields[3] = (changed.texture.fields[3] & ~(0x1fu << 20u)) | (8u << 20u); break;
			case 11: changed.texture.fields[3] |= 1u << 25u; break;
			case 12: changed.texture.fields[3] &= ~(1u << 26u); break;
			case 13: changed.texture.fields[2] = 30u | (31u << 14u); break;
			case 14: changed.texture.fields[2] = 31u | (30u << 14u); break;
			case 15: changed.texture.fields[4] = 27u << 13u; break;
		}
		if (Bc7SampledTextureDescriptorAllowed(changed)) return false;
	}
	for (const auto shape: {std::array<uint32_t, 3> {48, 20, 64}, {900, 736, 928}, {4, 4, 4}})
	{
		auto changed = descriptor;
		changed.texture.fields[2] = (shape[0] - 1u) | ((shape[1] - 1u) << 14u);
		changed.texture.fields[4] = (shape[2] - 1u) << 13u;
		if (!Bc7SampledTextureDescriptorAllowed(changed)) return false;
	}
	std::fprintf(stderr, "[GFX_BC7_DESCRIPTOR_TEST] readonly-sampled-2D=pass negatives=16"
	                     " storage/type/depth/array/mip/srgb/pow2/memory/shape=reject\n");
	return true;
}
#endif

static uint32_t TextureNativeViewSwizzle(const ShaderTextureDescriptor& descriptor)
{
	return descriptor.usage == ShaderTextureUsage::ReadWrite && ShaderTextureR8UnormStorageCompatible(descriptor.texture) ?
	    ShaderTextureStorageViewSwizzle(descriptor.texture) : descriptor.texture.DstSelXYZW();
}

static TextureObject SampledTextureBackingRequest(const ShaderTextureDescriptor& descriptor, bool neo)
{
	const auto& texture = descriptor.texture;
	TextureObject object(texture.Dfmt(), texture.Nfmt(), uint32_t(texture.Width4()) + 1,
	    uint32_t(texture.Height4()) + 1, uint32_t(texture.Pitch()) + 1, texture.BaseLevel(),
	    texture.LastLevel() + 1, texture.TileMode(), neo, TextureNativeViewSwizzle(descriptor), texture.Pow2Pad());
	if (CanUseRenderTextureAsSampledAlias(descriptor.textures2d_without_sampler,
	                                    descriptor.usage == ShaderTextureUsage::ReadOnly))
		object.access_kind = GpuMemoryAccessKind::SampledRead;
	return object;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void PrepareTextures(uint64_t submit_id, CommandBuffer* buffer, const ShaderTextureResources& textures, VulkanImage** images_sampled,
	                        VulkanImage** images_storage, int* images_sampled_view, uint32_t** sgprs, DescriptorCache::Stage stage,
	                        GpuMemoryImageAccessBatch* command_accesses, const RenderColorInfo* current_color)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(images_sampled == nullptr);
	EXIT_IF(images_storage == nullptr);
	EXIT_IF(sgprs == nullptr);
	EXIT_IF(*sgprs == nullptr);
	const ShaderType shader_type = ShaderTypeForDescriptorStage(stage);
	EXIT_NOT_IMPLEMENTED(shader_type == ShaderType::Unknown ||
	                     !ShaderTextureDescriptorProfileSupportedPreCache(textures, shader_type));
	const bool adjacent_storage_mip_profile =
	    ShaderTextureStorageMipProfileId(textures) == SHADER_TEXTURE_STORAGE_MIP_PROFILE_ADJACENT_SINGLE_WINDOW;

	// The adjacent-mip profile carries exactly one sampled source and one storage
	// destination. Resolve the source first even when the guest descriptor records
	// are physically reversed: the destination must never be admitted from a stale
	// guest snapshot before the GPU-produced source has been proven live. Descriptor
	// words are buffered below and still copied to SGPRs in their original order.
	int processing_order[ShaderTextureResources::RES_MAX] {};
	BuildTexturePreparationOrder(textures, adjacent_storage_mip_profile, processing_order);
	uint32_t prepared_descriptor_fields[ShaderTextureResources::RES_MAX][8] {};
	bool     descriptor_prepared[ShaderTextureResources::RES_MAX] {};

	int index_sampled = 0;
	int index_storage = 0;

	for (int processing_index = 0; processing_index < textures.textures_num; ++processing_index)
	{
		const int i = processing_order[processing_index];
		auto r = textures.desc[i].texture;
		if (g_null_sampled_image_enabled && stage == DescriptorCache::Stage::Pixel &&
		    textures.desc[i].IsNullSampledImage())
		{
			EXIT_IF(textures.desc[i].usage != ShaderTextureUsage::ReadOnly || textures.desc[i].textures2d_without_sampler);
			auto* null_image = g_render_ctx->GetNullSampledImage();
			EXIT_IF(null_image == nullptr || null_image->image_view[VulkanImage::VIEW_DEFAULT] == nullptr);
			if (command_accesses != nullptr && command_accesses->native_inputs != nullptr)
				command_accesses->native_inputs->AddRendererObject(null_image);

			images_sampled[index_sampled]      = null_image;
			images_sampled_view[index_sampled] = VulkanImage::VIEW_DEFAULT;
			r.UpdateAddress38(index_sampled);
			index_sampled++;

			std::copy(std::begin(r.fields), std::end(r.fields), prepared_descriptor_fields[i]);
			descriptor_prepared[i] = true;
			g_null_sampled_image_prepared.fetch_add(1, std::memory_order_relaxed);
			continue;
		}

		const bool bc7_profile = r.Dfmt() == 41 && Bc7SampledTextureDescriptorAllowed(textures.desc[i]);
		if (r.Dfmt() == 41 && !bc7_profile) DumpPreparedTextureMismatch("bc7-profile", stage, i, textures.desc[i]);
		EXIT_NOT_IMPLEMENTED(r.Dfmt() == 41 && !bc7_profile);
		const bool tile_supported = r.TileMode() == 8 || r.TileMode() == 13 || r.TileMode() == 14 || r.TileMode() == 2 ||
		                            r.TileMode() == 10 || r.TileMode() == 31;
		const bool format_supported = r.Dfmt() == 1 || r.Dfmt() == 10 || r.Dfmt() == 37 || r.Dfmt() == 4 || r.Dfmt() == 35 ||
		                              r.Dfmt() == 3 || r.Dfmt() == 36 || bc7_profile;
		const bool numeric_format_supported = r.Nfmt() == 9 || r.Nfmt() == 0 || r.Nfmt() == 7;
		const bool perf_supported           = r.PerfMod() == 7 || r.PerfMod() == 0;
		const bool memory_supported         = r.MemoryType() == 0x10 || r.MemoryType() == 0x6d;
		const char* field_mismatch = !format_supported          ? "dfmt"
		                             : !numeric_format_supported ? "nfmt"
		                             : !perf_supported           ? "perf-mod"
		                             : r.Interlaced()             ? "interlaced"
		                             : !tile_supported            ? "tile-mode"
		                             : r.BaseArray() != 0          ? "base-array"
		                             : r.LastArray() != 0          ? "last-array"
		                             : r.MinLodWarn() != 0         ? "min-lod-warn"
		                             : r.CounterBankId() != 0      ? "counter-bank"
		                             : r.LodHdwCntEn()             ? "lod-hdw-counter"
		                             : !memory_supported           ? "memory-type"
		                                                           : nullptr;
		if (field_mismatch != nullptr)
		{
			DumpPreparedTextureMismatch(field_mismatch, stage, i, textures.desc[i]);
		}
		EXIT_NOT_IMPLEMENTED(r.Dfmt() != 1 && r.Dfmt() != 10 && r.Dfmt() != 37 && r.Dfmt() != 4 && r.Dfmt() != 35 && r.Dfmt() != 3 &&
		                     r.Dfmt() != 36 && !bc7_profile);
		EXIT_NOT_IMPLEMENTED(r.Nfmt() != 9 && r.Nfmt() != 0 && r.Nfmt() != 7);
		EXIT_NOT_IMPLEMENTED(r.PerfMod() != 7 && r.PerfMod() != 0);
		EXIT_NOT_IMPLEMENTED(r.Interlaced() != false);
		EXIT_NOT_IMPLEMENTED(!tile_supported);
		EXIT_NOT_IMPLEMENTED(r.BaseArray() != 0);
		EXIT_NOT_IMPLEMENTED(r.LastArray() != 0);
		EXIT_NOT_IMPLEMENTED(r.MinLodWarn() != 0);
		EXIT_NOT_IMPLEMENTED(r.CounterBankId() != 0);
		EXIT_NOT_IMPLEMENTED(r.LodHdwCntEn() != false);
		EXIT_NOT_IMPLEMENTED(r.MemoryType() != 0x10 && r.MemoryType() != 0x6d);
		if (r.Base38() == 0)
		{
			DumpPreparedTextureMismatch("zero-base", stage, i, textures.desc[i]);
		}
		EXIT_NOT_IMPLEMENTED(r.Base38() == 0);
		EXIT_NOT_IMPLEMENTED(r.MinLod() != 0);
		const bool texture_dimension_supported = ShaderTextureDescriptorDimensionSupported(textures.desc[i], shader_type);
		if (!texture_dimension_supported)
		{
			DumpPreparedTextureMismatch("texture-type", stage, i, textures.desc[i]);
		}
		EXIT_NOT_IMPLEMENTED(!texture_dimension_supported);
		if (stage == DescriptorCache::Stage::Pixel && r.Type() == ShaderTextureResource::TYPE_8_COMPAT)
		{
			static std::atomic_bool receipt {false};
			if (!receipt.exchange(true, std::memory_order_relaxed))
			{
				std::fprintf(stderr, "[GFX_TYPE8_PIXEL_SAMPLE] guest=1d host=2d-height1 sample_y=0.5 policy=exact-profile\n");
				std::fflush(stderr);
			}
		}
		EXIT_NOT_IMPLEMENTED(r.Depth() != 0);

		bool read_only = (r.MemoryType() == 0x10);

		EXIT_NOT_IMPLEMENTED(read_only && !(textures.desc[i].usage == ShaderTextureUsage::ReadOnly));

		TileSizeAlign size {};
		auto          addr       = r.Base38();
		bool          neo        = Config::IsNeo();
		auto          width      = r.Width4() + 1;
		auto          height     = r.Height4() + 1;
		auto          pitch      = r.Pitch() + 1;
		auto          base_level = r.BaseLevel();
		auto          levels     = r.LastLevel() + 1;
		auto          tile       = r.TileMode();
		auto          dfmt       = r.Dfmt();
		auto          nfmt       = r.Nfmt();
		uint32_t      swizzle    = TextureNativeViewSwizzle(textures.desc[i]);
		if (textures.desc[i].usage == ShaderTextureUsage::ReadWrite && ShaderTextureR8UnormStorageCompatible(r))
		{
			// Vulkan storage views require identity component mapping. The guest
			// XXXX receive selection remains in the descriptor passed to the shader;
			// for an R8 image the write consumes R and ignores unavailable channels.
			static std::atomic_bool receipt {false};
			if (!receipt.exchange(true, std::memory_order_relaxed))
			{
				std::fprintf(stderr,
				             "[GFX_STORAGE_R8_UNORM] guest=dfmt1-nfmt0 host=VK_FORMAT_R8_UNORM spirv=R8 swizzle=identity "
				             "policy=exact-write-profile\n");
				std::fflush(stderr);
			}
		}

		const bool adjacent_nonzero_mip_window = adjacent_storage_mip_profile && base_level != 0;
		bool check_depth_texture = (tile == 2);

		TileGetTextureSize(dfmt, nfmt, width, height, pitch, levels, tile, neo, &size, nullptr, nullptr, r.Pow2Pad());

		EXIT_NOT_IMPLEMENTED(size.size == 0);
		EXIT_NOT_IMPLEMENTED((addr & (static_cast<uint64_t>(size.align) - 1u)) != 0);

		VulkanImage* tex            = nullptr;
		bool         render_texture = false;
		bool         depth_texture  = false;
		bool         videoout_texture = false;
		int          view_type      = VulkanImage::VIEW_DEFAULT;
		GpuMemoryObjectHandle tracked_handle {};
		const GpuMemoryAccessKind tracked_access = textures.desc[i].usage == ShaderTextureUsage::ReadWrite
		                                                    ? GpuMemoryAccessKind::StorageReadWrite
		                                                    : GpuMemoryAccessKind::SampledRead;

		VideoOut::VideoOutBufferImageInfo video_image;
		if (tile == 10)
		{
			VideoOut::VideoOutImageLookupIdentity videoout_lookup {};
			const bool trace_videoout = VideoOut::VideoOutLifecycleTraceEnabled();
			if (trace_videoout)
			{
				videoout_lookup.kind                         = VideoOut::VideoOutImageLookupKind::SampledTexture;
				videoout_lookup.command_buffer               = buffer;
				videoout_lookup.submit_id                    = submit_id;
				videoout_lookup.requested_size               = size.size;
				videoout_lookup.requested_pitch              = pitch;
				videoout_lookup.requested_width              = width;
				videoout_lookup.requested_height             = height;
				videoout_lookup.requested_data_format        = dfmt;
				videoout_lookup.requested_number_format      = nfmt;
				videoout_lookup.requested_tile_mode          = tile;
				videoout_lookup.requested_swizzle            = swizzle;
				videoout_lookup.requested_base_level         = base_level;
				videoout_lookup.requested_levels             = levels;
				videoout_lookup.requested_read_only          = read_only;
				videoout_lookup.requested_storage_descriptor = textures.desc[i].textures2d_without_sampler;
				if (buffer != nullptr)
				{
					videoout_lookup.recording_id                  = buffer->GetRecordingId();
					videoout_lookup.command_ordinal               = buffer->GetTrackedCommandOrdinal();
					videoout_lookup.submission_session_epoch      = buffer->GetSubmissionSessionEpoch();
					videoout_lookup.recording_submit_serial       = buffer->GetRecordingSubmitSerial();
					videoout_lookup.begin_submit_serial_cutoff    = buffer->GetBeginSubmitSerialCutoff();
					videoout_lookup.begin_completed_serial_cutoff = buffer->GetBeginCompletedSerialCutoff();
					videoout_lookup.queue                         = buffer->GetQueue();
				}
			}
			video_image = VideoOut::VideoOutGetImage(addr, trace_videoout ? &videoout_lookup : nullptr);
		}

		if (video_image.image != nullptr)
		{
			EXIT_IF(command_accesses == nullptr);
			VideoOutVulkanImage* pinned_image = command_accesses->FindSampledVideoOut(video_image.tracked_handle);
			const bool already_pinned = pinned_image != nullptr;
			void* pinned_host = pinned_image;
			bool lifetime_valid = already_pinned;
			if (!already_pinned)
			{
				lifetime_valid = PinVideoOutForRendererCommand(
				    buffer, video_image.tracked_handle, addr, size.size, &pinned_host);
				pinned_image = static_cast<VideoOutVulkanImage*>(pinned_host);
			}
			// The registry pointer is only an identity token until the tracker has
			// pinned and returned that same live host object.
			lifetime_valid = lifetime_valid && pinned_image != nullptr && pinned_image == video_image.image;
			if (!lifetime_valid)
			{
				pinned_image = nullptr;
			}
			uint32_t available_view_mask = 0;
			for (int view = 0; pinned_image != nullptr && view < VulkanImage::VIEW_MAX; ++view)
			{
				if (pinned_image->image_view[view] != VK_NULL_HANDLE)
				{
					available_view_mask |= 1u << static_cast<uint32_t>(view);
				}
			}
			VideoOutSampleAliasRequest request {};
			request.graphics_queue       = buffer != nullptr && buffer->GetQueue() == GraphicContext::QUEUE_GFX &&
			                               stage != DescriptorCache::Stage::Compute;
			request.two_dimensional      = r.Type() == ShaderTextureResource::TYPE_2D;
			request.storage_descriptor   = textures.desc[i].textures2d_without_sampler;
			request.read_only_descriptor = textures.desc[i].usage == ShaderTextureUsage::ReadOnly;
			request.read_only_memory     = read_only;
			request.registered_tiled     = video_image.registered_tiled;
			request.registered_neo       = video_image.registered_neo;
			request.requested_neo        = neo;
			request.pow2_pad              = r.Pow2Pad();
			request.lifetime_valid        = lifetime_valid;
			request.self_feedback         = VideoOutSampleAliasSelfFeedback(
			    current_color, pinned_image, addr, size.size);
			request.registered_size       = video_image.buffer_size;
			request.registered_pitch      = video_image.buffer_pitch;
			request.requested_size        = size.size;
			request.requested_pitch       = pitch;
			request.image_width           = pinned_image != nullptr ? pinned_image->extent.width : 0;
			request.image_height          = pinned_image != nullptr ? pinned_image->extent.height : 0;
			request.requested_width       = width;
			request.requested_height      = height;
			request.data_format           = dfmt;
			request.number_format         = nfmt;
			request.tile_mode             = tile;
			request.component_swizzle     = swizzle;
			request.base_level            = base_level;
			request.levels                = levels;
			request.image_format          = pinned_image != nullptr ? pinned_image->format : VK_FORMAT_UNDEFINED;
			request.image_usage           = pinned_image != nullptr ? pinned_image->usage : 0;
			request.image_flags           = pinned_image != nullptr ? pinned_image->flags : 0;
			request.image_mip_levels      = pinned_image != nullptr ? pinned_image->mip_levels : 0;
			request.available_view_mask   = available_view_mask;
			const auto decision = ResolveVideoOutSampleAlias(request);
			if (!decision.IsSupported())
			{
				std::fprintf(stderr,
				             "[GFX_VIDEOOUT_SAMPLED_ALIAS_REJECT] reason=%s address=0x%016" PRIx64
				             " size=%" PRIu64 " pitch=%u extent=%ux%u dfmt=%u nfmt=%u tile=%u swizzle=0x%x"
				             " base=%u levels=%u storage=%u read_only=%u slot=%u set=%d generation=%" PRIu64 "\n",
			             VideoOutSampleAliasRejectionName(decision.rejection), addr,
			             static_cast<uint64_t>(size.size), pitch, width, height,
				             dfmt, nfmt, tile, swizzle, base_level, levels,
				             textures.desc[i].textures2d_without_sampler ? 1u : 0u,
				             textures.desc[i].usage == ShaderTextureUsage::ReadOnly ? 1u : 0u,
				             video_image.slot, video_image.set_id, video_image.generation);
				std::fflush(stderr);
				VideoOut::VideoOutDumpLifecycleTrace("sampled_alias_rejected");
			}
			EXIT_NOT_IMPLEMENTED(!decision.IsSupported());
			view_type = GetVideoOutSampleViewIndex(decision.sample_view.view);
			EXIT_IF(view_type < 0 || view_type >= VulkanImage::VIEW_MAX);
			EXIT_IF(pinned_image == nullptr);
			tex              = pinned_image;
			videoout_texture = true;
			tracked_handle   = video_image.tracked_handle;
			if (!already_pinned)
			{
				command_accesses->AddSampledVideoOut(pinned_image, video_image.tracked_handle);
			}
			static std::atomic_bool reported {false};
			if (!reported.exchange(true, std::memory_order_relaxed))
			{
				std::fprintf(stderr,
				             "[GFX_VIDEOOUT_SAMPLED_ALIAS] native_format=%d sample_format=%d view=%d"
				             " size=%" PRIu64 " pitch=%u extent=%ux%u dfmt=%u nfmt=%u tile=%u swizzle=0x%x"
				             " slot=%u set=%d generation=%" PRIu64 " policy=same-image-exact\n",
			             static_cast<int>(pinned_image->format), static_cast<int>(decision.sample_view.format),
			             view_type, static_cast<uint64_t>(size.size), pitch, width, height, dfmt, nfmt, tile, swizzle,
				             video_image.slot, video_image.set_id, video_image.generation);
				std::fflush(stderr);
			}
		}

		if (!videoout_texture && check_depth_texture && !adjacent_nonzero_mip_window)
		{
			auto dtex     = FindDepthStencil(addr, size.size, true, "sampled-depth");
			depth_texture = !dtex.IsEmpty();
			if (depth_texture)
			{
				EXIT_NOT_IMPLEMENTED(dtex.Size() > 1);
				EXIT_NOT_IMPLEMENTED(swizzle != DstSel(4, 4, 4, 4));
				EXIT_NOT_IMPLEMENTED(dtex.At(0)->compressed);
				tex = dtex.At(0);
			}
		} else if (!videoout_texture && !adjacent_nonzero_mip_window)
		{
			void* render_host = nullptr;
			render_texture = GpuMemoryPinRenderTextureForCommand(buffer, addr, size.size, &tracked_handle, &render_host);
			if (render_texture)
			{
				EXIT_IF(command_accesses == nullptr);
				auto* rtex = static_cast<RenderTextureVulkanImage*>(render_host);
				EXIT_IF(rtex == nullptr || tracked_handle.host_object != rtex);
				EXIT_NOT_IMPLEMENTED(VideoOutSampleAliasSelfFeedback(current_color, rtex, addr, size.size));
				EXIT_NOT_IMPLEMENTED(!CanUseRenderTextureAsSampledAlias(
				    textures.desc[i].textures2d_without_sampler, textures.desc[i].usage == ShaderTextureUsage::ReadOnly));
				EXIT_NOT_IMPLEMENTED(!AreRenderTextureExtentsCompatible(rtex->extent.width, rtex->extent.height,
				                                                          width, height));
				const auto sampled_format = DecodeSampledRenderTextureFormat(dfmt, nfmt);
				const auto render_format  = GetRenderTextureFormatFromVulkan(rtex->format);
				const auto sample_view = SelectRenderTextureSampleView(render_format, sampled_format.format, swizzle);
				EXIT_NOT_IMPLEMENTED(!sampled_format.IsSupported() || sample_view == RenderTextureSampleView::Unsupported);
				tex = rtex;
				if (sample_view == RenderTextureSampleView::Bgra)
				{
					view_type = VulkanImage::VIEW_BGRA;
				}
			}
		}

		if (!videoout_texture && !render_texture && !depth_texture)
		{
			if (textures.desc[i].textures2d_without_sampler || adjacent_nonzero_mip_window)
			{
				EXIT_NOT_IMPLEMENTED(textures.desc[i].textures2d_without_sampler
				                         ? textures.desc[i].usage != ShaderTextureUsage::ReadWrite
				                         : textures.desc[i].usage != ShaderTextureUsage::ReadOnly);

				StorageTextureObject vulkan_texture_info(dfmt, nfmt, width, height, pitch, base_level, levels, tile, neo, swizzle,
				                                                r.Pow2Pad());
				vulkan_texture_info.access_kind = tracked_access;
				tex = static_cast<StorageTextureVulkanImage*>(GpuMemoryCreateObjectTracked(
				    submit_id, g_render_ctx->GetGraphicCtx(), buffer, addr, size.size, vulkan_texture_info, &tracked_handle));
			} else
			{
				EXIT_NOT_IMPLEMENTED(textures.desc[i].usage != ShaderTextureUsage::ReadOnly);

				if (tile == 10)
				{
					RenderTextureFormat rt_format = RenderTextureFormat::Unknown;
					if (dfmt == 10 && nfmt == 0)
					{
						rt_format = RenderTextureFormat::R8G8B8A8Unorm;
					}

					RenderTextureObject vulkan_buffer_info(rt_format, width, height, true, neo, pitch, false, tile);
					vulkan_buffer_info.access_kind = tracked_access;
					tex = static_cast<Graphics::RenderTextureVulkanImage*>(Graphics::GpuMemoryCreateObjectTracked(
					    submit_id, g_render_ctx->GetGraphicCtx(), buffer, addr, size.size, vulkan_buffer_info, &tracked_handle));
				} else
				{
					TextureObject vulkan_texture_info(dfmt, nfmt, width, height, pitch, base_level, levels, tile, neo, swizzle,
					                                  r.Pow2Pad());
					vulkan_texture_info.access_kind = tracked_access;
					tex = static_cast<TextureVulkanImage*>(GpuMemoryCreateObjectTracked(
					    submit_id, g_render_ctx->GetGraphicCtx(), buffer, addr, size.size, vulkan_texture_info, &tracked_handle));
				}
			}
		}

		EXIT_NOT_IMPLEMENTED(tex == nullptr);
		if (adjacent_nonzero_mip_window)
		{
			EXIT_NOT_IMPLEMENTED(tex->type != VulkanImageType::StorageTexture);
			const auto* window = static_cast<const StorageTextureVulkanImage*>(tex);
			const uint32_t expected_width  = std::max(static_cast<uint32_t>(width >> base_level), 1u);
			const uint32_t expected_height = std::max(static_cast<uint32_t>(height >> base_level), 1u);
			EXIT_NOT_IMPLEMENTED(window->descriptor_base_level != base_level || window->descriptor_levels != levels ||
			                     tex->extent.width != expected_width || tex->extent.height != expected_height ||
			                     window->physical_extent.width != expected_width ||
			                     window->physical_extent.height != expected_height || tex->layout != VK_IMAGE_LAYOUT_GENERAL);
		}

		g_resource_capture.Image("texture", i, tex);
		g_resource_capture.GpuR8(buffer, textures.desc[i], tex, i, tracked_handle);
		g_resource_capture.GpuRgba8Mips(buffer, textures.desc[i], tex, i);
		g_resource_capture.Metadata("texture_descriptor", textures.desc[i].texture.fields, sizeof(textures.desc[i].texture.fields));
		g_resource_capture.Range("texture_guest", i, addr, size.size);

		if (textures.desc[i].textures2d_without_sampler)
		{
			images_storage[index_storage] = tex;
			r.UpdateAddress38(index_storage);
			index_storage++;
		} else
		{
			images_sampled[index_sampled]      = tex;
			images_sampled_view[index_sampled] = (depth_texture ? VulkanImage::VIEW_DEPTH_TEXTURE : view_type);
			r.UpdateAddress38(index_sampled);
			index_sampled++;
		}
		if (command_accesses != nullptr)
		{
			command_accesses->Add(tracked_handle, tracked_access);
			if (command_accesses->native_inputs != nullptr)
				command_accesses->native_inputs->Add(tex, DrawImageObjectType(tex->type), tracked_handle);
		}

		EXIT_NOT_IMPLEMENTED((r.Base38() >> 32u) != 0);

		std::copy(std::begin(r.fields), std::end(r.fields), prepared_descriptor_fields[i]);
		descriptor_prepared[i] = true;
	}

	for (int i = 0; i < textures.textures_num; ++i)
	{
		EXIT_IF(!descriptor_prepared[i]);
		std::copy(std::begin(prepared_descriptor_fields[i]), std::end(prepared_descriptor_fields[i]), *sgprs);
		(*sgprs) += 8;
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void PrepareSamplers(const ShaderBindResources& bind, uint64_t* sampler_ids, uint32_t** sgprs,
                            DescriptorCache::Stage stage)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(sampler_ids == nullptr);
	EXIT_IF(sgprs == nullptr);
	EXIT_IF(*sgprs == nullptr);
	const auto& samplers = bind.samplers;
	const bool images_have_no_z_coordinate = SamplerImagesHaveNoZCoordinate(bind, stage);

	for (int i = 0; i < samplers.samplers_num; i++)
	{
		auto r = samplers.samplers[i];

		// EXIT_NOT_IMPLEMENTED(r.ClampX() != 0);
		// EXIT_NOT_IMPLEMENTED(r.ClampY() != 0);
		// EXIT_NOT_IMPLEMENTED(r.ClampZ() != 0);
		// EXIT_NOT_IMPLEMENTED(r.MaxAnisoRatio() != 0);
		EXIT_NOT_IMPLEMENTED(r.ForceUnormCoords() != false);
		EXIT_NOT_IMPLEMENTED(r.AnisoThreshold() != 0);
		EXIT_NOT_IMPLEMENTED(r.McCoordTrunc() != false);
		EXIT_NOT_IMPLEMENTED(r.ForceDegamma() != false);
		EXIT_NOT_IMPLEMENTED(r.AnisoBias() != 0);
		EXIT_NOT_IMPLEMENTED(r.TruncCoord() != false);
		EXIT_NOT_IMPLEMENTED(r.DisableCubeWrap() != false);
		EXIT_NOT_IMPLEMENTED(r.FilterMode() != 0);
		// EXIT_NOT_IMPLEMENTED(r.MinLod() != 0);
		// EXIT_NOT_IMPLEMENTED(r.MaxLod() != 4095);
		EXIT_NOT_IMPLEMENTED(r.PerfMip() != 0);
		EXIT_NOT_IMPLEMENTED(r.PerfZ() != 0);
		// EXIT_NOT_IMPLEMENTED(r.LodBias() != 0);
		EXIT_NOT_IMPLEMENTED(r.LodBiasSec() != 0);
		// EXIT_NOT_IMPLEMENTED(r.XyMagFilter() != 1);
		// EXIT_NOT_IMPLEMENTED(r.XyMinFilter() != 1);
		if (r.ZFilter() != 1)
		{
			static std::atomic<uint32_t> reports {0};
			auto report = reports.load(std::memory_order_relaxed);
			while (report < 16 && !reports.compare_exchange_strong(report, report + 1, std::memory_order_relaxed)) {}
			if (report < 16)
			{
				uint32_t types = 0, without_sampler = 0;
				for (int index = 0; index < std::min(bind.textures2D.textures_num, ShaderTextureResources::RES_MAX); ++index)
				{
					types |= 1u << bind.textures2D.desc[index].texture.Type();
					without_sampler += bind.textures2D.desc[index].textures2d_without_sampler ? 1u : 0u;
				}
				std::fprintf(stderr, "[GFX_SAMPLER_Z_FILTER] stage=%u index=%d z_filter=%u xy_filter=%u:%u mip_filter=%u"
				                     " depth_compare=%u textures=%d sampled=%d storage=%d types_mask=%04x without_sampler=%u"
				                     " dimension_proved=%u z_filter_admitted=%u policy=zero-two-require-no-z\n",
				             static_cast<unsigned>(stage), i, static_cast<unsigned>(r.ZFilter()), static_cast<unsigned>(r.XyMagFilter()),
				             static_cast<unsigned>(r.XyMinFilter()), static_cast<unsigned>(r.MipFilter()), static_cast<unsigned>(r.DepthCompareFunc()),
				             bind.textures2D.textures_num, bind.textures2D.textures2d_sampled_num, bind.textures2D.textures2d_storage_num,
				             types, without_sampler, images_have_no_z_coordinate ? 1u : 0u,
				             SamplerZFilterSupported(r, images_have_no_z_coordinate) ? 1u : 0u);
			}
		}
		EXIT_NOT_IMPLEMENTED(!SamplerZFilterSupported(r, images_have_no_z_coordinate));
		// EXIT_NOT_IMPLEMENTED(r.MipFilter() != 0 && r.MipFilter() != 2);
		EXIT_NOT_IMPLEMENTED(r.BorderColorPtr() != 0);
		EXIT_NOT_IMPLEMENTED(r.BorderColorType() != 0);

		if (r.DepthCompareFunc() != 0)
		{
			static std::atomic<uint32_t> reports {0};
			auto report = reports.load(std::memory_order_relaxed);
			while (report < 16 && !reports.compare_exchange_strong(report, report + 1, std::memory_order_relaxed)) {}
			if (report < 16)
				std::fprintf(stderr, "[GFX_SAMPLER_COMPARE_DORMANT] stage=%u index=%d sgpr=%d extended=%u depth_compare=%u"
				                     " clamp=%u:%u:%u xy_filter=%u:%u z_filter=%u mip_filter=%u min_lod=%u max_lod=%u"
				                     " unnormalized=%u border_type=%u comparison_disabled=1 non_dref_model=1\n",
				             static_cast<unsigned>(stage), i, samplers.start_register[i], samplers.extended[i] ? 1u : 0u,
				             static_cast<unsigned>(r.DepthCompareFunc()), static_cast<unsigned>(r.ClampX()),
				             static_cast<unsigned>(r.ClampY()), static_cast<unsigned>(r.ClampZ()),
				             static_cast<unsigned>(r.XyMagFilter()), static_cast<unsigned>(r.XyMinFilter()),
				             static_cast<unsigned>(r.ZFilter()), static_cast<unsigned>(r.MipFilter()),
				             static_cast<unsigned>(r.MinLod()), static_cast<unsigned>(r.MaxLod()),
				             r.ForceUnormCoords() ? 1u : 0u, static_cast<unsigned>(r.BorderColorType()));
		}

		sampler_ids[i] = g_render_ctx->GetSamplerCache()->GetSamplerId(r);

		r.UpdateIndex(i);

		(*sgprs)[0] = r.fields[0];
		(*sgprs)[1] = r.fields[1];
		(*sgprs)[2] = r.fields[2];
		(*sgprs)[3] = r.fields[3];

		(*sgprs) += 4;
	}
}

static void PrepareGdsPointers(const ShaderGdsResources& gds_pointers, uint32_t** sgprs)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(sgprs == nullptr);
	EXIT_IF(*sgprs == nullptr);

	for (int i = 0; i < gds_pointers.pointers_num; i++)
	{
		auto r = gds_pointers.pointers[i];

		EXIT_NOT_IMPLEMENTED(r.Size() != 4);

		(*sgprs)[i] = r.field;
	}

	if (gds_pointers.pointers_num > 0)
	{
		(*sgprs) += static_cast<ptrdiff_t>(4 * ((gds_pointers.pointers_num - 1) / 4 + 1));
	}
}

static void PrepareDirectSgprs(const ShaderDirectSgprsResources& direct_sgprs, uint32_t** sgprs)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(sgprs == nullptr);
	EXIT_IF(*sgprs == nullptr);

	for (int i = 0; i < direct_sgprs.sgprs_num; i++)
	{
		auto r = direct_sgprs.sgprs[i];

		(*sgprs)[i] = r.field;
	}

	if (direct_sgprs.sgprs_num > 0)
	{
		(*sgprs) += static_cast<ptrdiff_t>(4 * ((direct_sgprs.sgprs_num - 1) / 4 + 1));
	}
}

struct DescriptorTimingStats
{
	uint64_t count = 0;
	uint64_t us[7] = {};
};

static void RecordDescriptorTiming(DescriptorTimingStats* stats, DescriptorCache::Stage stage,
                                   const std::chrono::steady_clock::time_point (&points)[8])
{
	if (!Config::GraphicsTimingEnabled())
	{
		return;
	}

	for (int i = 0; i < 7; i++)
	{
		stats->us[i] += static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::microseconds>(points[i + 1] - points[i]).count());
	}

	if (++stats->count == 5000)
	{
		const char* name = (stage == DescriptorCache::Stage::Vertex   ? "descriptor_vs"
		                    : stage == DescriptorCache::Stage::Pixel ? "descriptor_ps"
		                                                           : "descriptor_cs");
		dbg_printf("[PERF] %s storage=%.3f textures=%.3f samplers=%.3f constants=%.3f barriers=%.3f cache=%.3f commands=%.3f us\n",
		           name, static_cast<double>(stats->us[0]) / stats->count, static_cast<double>(stats->us[1]) / stats->count,
		           static_cast<double>(stats->us[2]) / stats->count, static_cast<double>(stats->us[3]) / stats->count,
		           static_cast<double>(stats->us[4]) / stats->count, static_cast<double>(stats->us[5]) / stats->count,
		           static_cast<double>(stats->us[6]) / stats->count);
		*stats = {};
	}
}

static bool ShaderBindingsAllowRenderPassReuse(const ShaderBindResources& bind)
{
	if (bind.gds_pointers.pointers_num != 0 || bind.textures2D.textures2d_storage_num != 0) return false;
	for (int index = 0; index < bind.storage_buffers.buffers_num; ++index)
	{
		const auto usage = bind.storage_buffers.usages[index];
		if (usage != ShaderStorageUsage::ReadOnly && usage != ShaderStorageUsage::Constant) return false;
	}
	for (int index = 0; index < bind.textures2D.textures_num; ++index)
		if (bind.textures2D.desc[index].usage != ShaderTextureUsage::ReadOnly ||
		    bind.textures2D.desc[index].textures2d_without_sampler) return false;
	return true;
}

static bool PackShaderConstantWords(std::vector<uint32_t>* destination, const ShaderBindResources& bind, const uint32_t* words)
{
	if (destination == nullptr || words == nullptr || (bind.push_constant_offset % sizeof(uint32_t)) != 0 ||
	    uint64_t{bind.push_constant_offset} + bind.push_constant_size > destination->size() * sizeof(uint32_t)) return false;
	std::memcpy(destination->data() + bind.push_constant_offset / sizeof(uint32_t), words, bind.push_constant_size);
	return true;
}

class ShaderConstantUpload
{
public:
	ShaderConstantUpload(const ShaderBindResources& first, const ShaderBindResources* second = nullptr)
	{
		m_uniform = first.constant_storage == ShaderConstantStorage::UniformBuffer;
		if (second != nullptr) EXIT_IF(second->constant_storage != first.constant_storage);
		if (m_uniform)
		{
			uint64_t span = first.push_constant_size == 0 ? 0 : uint64_t{first.push_constant_offset} + first.push_constant_size;
			if (second != nullptr && second->push_constant_size != 0)
				span = std::max(span, uint64_t{second->push_constant_offset} + second->push_constant_size);
			EXIT_IF(span == 0 || span > g_render_ctx->GetDeviceLimits().maxUniformBufferRange);
			m_words.resize(static_cast<size_t>(span / sizeof(uint32_t)), 0);
		}
	}
	void Capture(const ShaderBindResources& bind, const uint32_t* words)
	{
		EXIT_IF(!m_uniform || bind.constant_storage != ShaderConstantStorage::UniformBuffer);
		EXIT_IF(!PackShaderConstantWords(&m_words, bind, words));
	}
	void Bind(CommandBuffer* command, VkPipelineBindPoint bind_point, VkPipelineLayout layout, DrawNativeInventory* inventory = nullptr)
	{
		if (!m_uniform) return;
		const VkShaderStageFlags stages = bind_point == VK_PIPELINE_BIND_POINT_COMPUTE ? VK_SHADER_STAGE_COMPUTE_BIT :
		    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		const auto set = g_render_ctx->GetDescriptorCache()->GetConstantDescriptor(command->GetDescriptorBatch(), stages, m_words,
		    inventory != nullptr ? &inventory->uniform : nullptr);
		if (inventory != nullptr) inventory->AddDescriptor(set);
		vkCmdBindDescriptorSets(command->GetBufferForDraw(), bind_point, layout, ShaderBindResources::UNIFORM_DESCRIPTOR_SET,
		                        1, &set, 0, nullptr);
		static std::atomic<bool> reported {false};
		if (!reported.exchange(true, std::memory_order_relaxed))
			std::fprintf(stderr, "[GFX_CONSTANT_STORAGE] mode=uniform_buffer push_limit=%u span=%zu arena_bytes=65536 owner=recording\n",
			             g_render_ctx->GetDeviceLimits().maxPushConstantsSize, m_words.size() * sizeof(uint32_t));
	}
private:
	bool m_uniform = false;
	std::vector<uint32_t> m_words;
};

static void BindDescriptors(uint64_t submit_id, CommandBuffer* buffer, VkPipelineBindPoint pipeline_bind_point, VkPipelineLayout layout,
	                        const ShaderBindResources& bind, bool shader_has_image_load, const ShaderStorageWriteAuthority& shader_storage_writes,
	                        VkShaderStageFlags vk_stage,
	                        DescriptorCache::Stage stage, GpuMemoryImageAccessBatch* command_accesses,
	                        const RenderColorInfo* current_color, GpuMemoryStorageAccessBatch* storage_accesses,
	                        const VulkanPipeline* diagnostic_pipeline, ShaderConstantUpload* constant_upload)
{
	LUMEN_PROFILER_FUNCTION();
	if (command_accesses != nullptr)
		command_accesses->render_pass_read_only &= ShaderBindingsAllowRenderPassReuse(bind);
	if (g_resource_capture.active)
	{
		g_resource_capture.stage = static_cast<int>(stage);
		g_resource_capture.Metadata("bind", &bind, sizeof(bind));
	}

	const bool unsupported_null_image_load =
	    g_null_sampled_image_enabled && ShaderNullSampledImageLoadUnsupported(
	                                        bind.textures2D, stage == DescriptorCache::Stage::Pixel, shader_has_image_load);
	if (unsupported_null_image_load)
	{
		dbg_printf("[GFX_DESCRIPTOR_MISMATCH] phase=bind reason=null-sampled-image-load stage=%u\n",
		           static_cast<unsigned>(stage));
	}
	EXIT_NOT_IMPLEMENTED(unsupported_null_image_load);

	if (bind.push_constant_size > 0)
	{
		const bool diagnostics_enabled = Config::GraphicsTimingEnabled();

		std::chrono::steady_clock::time_point diagnostic_points[8] {};
		diagnostic_points[0] = GetDiagnosticTimestamp(diagnostics_enabled);

		EXIT_NOT_IMPLEMENTED(bind.push_constant_size > ShaderBindResources::CONSTANT_DATA_MAX);
		if (bind.constant_storage == ShaderConstantStorage::PushConstants)
		{
			const auto limit = g_render_ctx->GetDeviceLimits().maxPushConstantsSize;
			EXIT_IF(bind.push_constant_offset >= limit || bind.push_constant_size > limit - bind.push_constant_offset);
		}
		EXIT_NOT_IMPLEMENTED(bind.storage_buffers.buffers_num > DescriptorCache::BUFFERS_MAX);
		EXIT_NOT_IMPLEMENTED((bind.textures2D.textures2d_storage_num > DescriptorCache::TEXTURES_STORAGE_MAX) ||
		                     (bind.textures2D.textures2d_sampled_num > DescriptorCache::TEXTURES_SAMPLED_MAX));
		EXIT_NOT_IMPLEMENTED(bind.textures2D.textures2d_storage_num + bind.textures2D.textures2d_sampled_num !=
		                     bind.textures2D.textures_num);
		EXIT_NOT_IMPLEMENTED(bind.samplers.samplers_num > DescriptorCache::SAMPLERS_MAX);

		bool need_descriptor = false;

		VulkanBuffer* storage_buffers[DescriptorCache::BUFFERS_MAX];
		VulkanImage*  textures2d_sampled[DescriptorCache::TEXTURES_SAMPLED_MAX];
		int           textures2d_sampled_view[DescriptorCache::TEXTURES_SAMPLED_MAX];
		VulkanImage*  textures2d_storage[DescriptorCache::TEXTURES_STORAGE_MAX];
		uint64_t      samplers[DescriptorCache::SAMPLERS_MAX];
		uint32_t      sgprs[ShaderBindResources::CONSTANT_DATA_MAX / sizeof(uint32_t)];
		std::fill_n(sgprs, bind.push_constant_size / sizeof(uint32_t), 0u);

		uint32_t* sgprs_ptr = sgprs;

		VulkanBuffer* gds_buffer = nullptr;

		if (bind.storage_buffers.buffers_num > 0)
		{
			PrepareStorageBuffers(submit_id, buffer, bind.storage_buffers, storage_buffers, &sgprs_ptr, storage_accesses,
			                      shader_storage_writes, diagnostic_pipeline, stage);
			need_descriptor = true;
		}
		diagnostic_points[1] = GetDiagnosticTimestamp(diagnostics_enabled);
		if (bind.textures2D.textures_num > 0)
		{
			PrepareTextures(submit_id, buffer, bind.textures2D, textures2d_sampled, textures2d_storage, textures2d_sampled_view,
			                &sgprs_ptr, stage, command_accesses, current_color);
			need_descriptor = true;
		}
		diagnostic_points[2] = GetDiagnosticTimestamp(diagnostics_enabled);
		if (bind.samplers.samplers_num > 0)
		{
			PrepareSamplers(bind, samplers, &sgprs_ptr, stage);
			if (command_accesses != nullptr && command_accesses->native_inputs != nullptr)
				for (int i = 0; i < bind.samplers.samplers_num; ++i)
					command_accesses->native_inputs->AddSampler(g_render_ctx->GetSamplerCache()->GetSampler(samplers[i]));
			need_descriptor = true;
		}
		diagnostic_points[3] = GetDiagnosticTimestamp(diagnostics_enabled);
		if (bind.gds_pointers.pointers_num > 0)
		{
			PrepareGdsPointers(bind.gds_pointers, &sgprs_ptr);
			gds_buffer      = g_render_ctx->GetGdsBuffer()->GetBuffer(g_render_ctx->GetGraphicCtx());
			if (command_accesses != nullptr && command_accesses->native_inputs != nullptr)
				command_accesses->native_inputs->AddRendererObject(gds_buffer);
			need_descriptor = true;
		}
		if (bind.direct_sgprs.sgprs_num > 0)
		{
			PrepareDirectSgprs(bind.direct_sgprs, &sgprs_ptr);
		}
		diagnostic_points[4] = GetDiagnosticTimestamp(diagnostics_enabled);

		EXIT_IF(bind.push_constant_size != (sgprs_ptr - sgprs) * 4);
		g_resource_capture.Metadata("push_constants", sgprs, bind.push_constant_size);

		auto* vk_buffer = buffer->GetBufferForDraw();
		if (command_accesses != nullptr)
		{
			for (int index = 0; index < bind.storage_buffers.buffers_num; ++index)
				command_accesses->ObserveRenderResource(storage_buffers[index]->memory.memory);
			for (int index = 0; index < bind.textures2D.textures2d_sampled_num; ++index)
				command_accesses->ObserveRenderResource(textures2d_sampled[index]->memory.memory);
		}

		if (bind.textures2D.textures_num > 0)
		{
			for (int i = 0; i < bind.textures2D.textures2d_sampled_num; i++)
			{
				if (textures2d_sampled[i]->type == VulkanImageType::DepthStencil &&
				    textures2d_sampled[i]->layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
				{
					GraphicsRenderDepthStencilBarrier(buffer->GetBufferOutsideRenderPass(), textures2d_sampled[i]);
				} else if (textures2d_sampled[i]->type == VulkanImageType::RenderTexture &&
				           textures2d_sampled[i]->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
				{
					GraphicsRenderRenderTextureBarrier(buffer->GetBufferOutsideRenderPass(), textures2d_sampled[i]);
				}
			}
		}
		diagnostic_points[5] = GetDiagnosticTimestamp(diagnostics_enabled);

		if (need_descriptor)
		{
			auto* descriptor_set = g_render_ctx->GetDescriptorCache()->GetDescriptor(
			    buffer->GetDescriptorBatch(), stage, storage_buffers, textures2d_sampled, textures2d_sampled_view,
			    textures2d_storage, samplers, &gds_buffer, bind);

			EXIT_IF(descriptor_set == nullptr);
			if (command_accesses != nullptr && command_accesses->native_inputs != nullptr)
				command_accesses->native_inputs->AddDescriptor(descriptor_set->set);
			diagnostic_points[6] = GetDiagnosticTimestamp(diagnostics_enabled);

			vkCmdBindDescriptorSets(vk_buffer, pipeline_bind_point, layout, bind.descriptor_set_slot, 1, &descriptor_set->set, 0, nullptr);
		} else
		{
			diagnostic_points[6] = GetDiagnosticTimestamp(diagnostics_enabled);
		}
		if (bind.constant_storage == ShaderConstantStorage::UniformBuffer)
		{
			EXIT_IF(constant_upload == nullptr);
			constant_upload->Capture(bind, sgprs);
		} else
		{
			vkCmdPushConstants(vk_buffer, layout, vk_stage, bind.push_constant_offset, bind.push_constant_size, sgprs);
		}
		diagnostic_points[7] = GetDiagnosticTimestamp(diagnostics_enabled);
		if (diagnostics_enabled)
		{
			static thread_local DescriptorTimingStats diagnostic_stats[4];
			RecordDescriptorTiming(&diagnostic_stats[static_cast<int>(stage)], stage, diagnostic_points);
		}
	}
}

static void VulkanCmdSetColorWriteEnableEXT(GraphicContext* ctx, VkCommandBuffer command_buffer, uint32_t attachment_count,
                                            const VkBool32* p_color_write_enables)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->instance == nullptr);

	static auto func =
	    reinterpret_cast<PFN_vkCmdSetColorWriteEnableEXT>(vkGetInstanceProcAddr(ctx->instance, "vkCmdSetColorWriteEnableEXT"));

	if (func != nullptr)
	{
		func(command_buffer, attachment_count, p_color_write_enables);
	} else
	{
		EXIT("vkCmdSetColorWriteEnableEXT not present\n");
	}
}

static void SetDynamicParams(VkCommandBuffer vk_buffer, VulkanPipeline* pipeline)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(pipeline == nullptr);
	EXIT_IF(pipeline->static_params == nullptr);
	EXIT_IF(pipeline->dynamic_params == nullptr);

	if (pipeline->dynamic_params->vk_dynamic_state_scissor)
	{
		VkRect2D scissor {};
		EXIT_NOT_IMPLEMENTED(!BuildScissorRect(pipeline->dynamic_params->scissor_ltrb, &scissor));
		vkCmdSetScissor(vk_buffer, 0, 1, &scissor);
	}

	if (pipeline->dynamic_params->vk_dynamic_state_line_width)
	{
		vkCmdSetLineWidth(vk_buffer, pipeline->dynamic_params->line_width);
	}

	if (pipeline->static_params->stencil_test_enable)
	{
		if (pipeline->dynamic_params->vk_dynamic_state_stencil_compare_mask)
		{
			vkCmdSetStencilCompareMask(vk_buffer, VK_STENCIL_FACE_FRONT_BIT, pipeline->dynamic_params->stencil_front.compareMask);
			vkCmdSetStencilCompareMask(vk_buffer, VK_STENCIL_FACE_BACK_BIT, pipeline->dynamic_params->stencil_back.compareMask);
		}
		if (pipeline->dynamic_params->vk_dynamic_state_stencil_write_mask)
		{
			vkCmdSetStencilWriteMask(vk_buffer, VK_STENCIL_FACE_FRONT_BIT, pipeline->dynamic_params->stencil_front.writeMask);
			vkCmdSetStencilWriteMask(vk_buffer, VK_STENCIL_FACE_BACK_BIT, pipeline->dynamic_params->stencil_back.writeMask);
		}
		if (pipeline->dynamic_params->vk_dynamic_state_stencil_reference)
		{
			vkCmdSetStencilReference(vk_buffer, VK_STENCIL_FACE_FRONT_BIT, pipeline->dynamic_params->stencil_front.reference);
			vkCmdSetStencilReference(vk_buffer, VK_STENCIL_FACE_BACK_BIT, pipeline->dynamic_params->stencil_back.reference);
		}
	}

	if (pipeline->dynamic_params->vk_dynamic_state_color_write_enable_ext)
	{
		VkBool32 enable = (pipeline->dynamic_params->color_write_enable ? VK_TRUE : VK_FALSE);
		VulkanCmdSetColorWriteEnableEXT(g_render_ctx->GetGraphicCtx(), vk_buffer, 1, &enable);
	}
}

static bool shader_is_disabled(HW::Shader* sh_ctx)
{
	if (const auto& vs = sh_ctx->GetVs();
	    !vs.vs_embedded && vs.vs_regs.data_addr != 0 && ShaderIsDisabled(vs.vs_regs.data_addr, true))
	{
		return true;
	}

	if (const auto& ps = sh_ctx->GetPs();
	    !ps.ps_embedded && ps.ps_regs.data_addr != 0 && ShaderIsDisabled(ps.ps_regs.data_addr))
	{
		return true;
	}

	return false;
}

struct DrawTimingStats
{
	uint64_t count = 0;
	uint64_t us[11] = {};
};

static void RecordDrawTiming(DrawTimingStats* stats, const char* name,
                             const std::chrono::steady_clock::time_point (&points)[12])
{
	if (!Config::GraphicsTimingEnabled())
	{
		return;
	}
	for (int i = 0; i < 11; i++)
	{
		stats->us[i] += static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::microseconds>(points[i + 1] - points[i]).count());
	}
	if (++stats->count == 5000)
	{
		dbg_printf("[PERF] %s lock=%.3f validate=%.3f targets=%.3f framebuffer=%.3f shader=%.3f pipeline=%.3f "
		           "vertex=%.3f desc_vs=%.3f desc_ps=%.3f index=%.3f record=%.3f us\n",
		           name, static_cast<double>(stats->us[0]) / stats->count, static_cast<double>(stats->us[1]) / stats->count,
		           static_cast<double>(stats->us[2]) / stats->count, static_cast<double>(stats->us[3]) / stats->count,
		           static_cast<double>(stats->us[4]) / stats->count, static_cast<double>(stats->us[5]) / stats->count,
		           static_cast<double>(stats->us[6]) / stats->count, static_cast<double>(stats->us[7]) / stats->count,
		           static_cast<double>(stats->us[8]) / stats->count, static_cast<double>(stats->us[9]) / stats->count,
		           static_cast<double>(stats->us[10]) / stats->count);
		*stats = {};
	}
}

static void DumpUnsupportedPolygonOffsetDraw(const char* kind, uint64_t submit_id, CommandBuffer* buffer,
                                            const HW::Context& ctx, const HW::UserConfig& ucfg, const HW::Shader& sh,
                                            uint32_t count, uint32_t flags, const RenderDepthInfo& resolved_depth,
                                            const char* reason)
{
	const auto& mode = ctx.GetModeControl();
	if (!mode.poly_offset_front_enable && !mode.poly_offset_back_enable) return;
	const auto& observed = ctx.GetPolygonOffsetRegisters();
	const auto& depth = ctx.GetDepthRenderTarget();
	const auto& control = ctx.GetDepthControl();
	const auto& render = ctx.GetRenderControl();
	const auto& viewport = ctx.GetScreenViewport().viewports[0];
	const auto& clip = ctx.GetClipControl();
	const auto& shader = ctx.GetShaderRegisters();
	const auto& depth_shader = shader.db_shader_control;
	const auto& vs = sh.GetVs();
	const auto& ps = sh.GetPs();
	const bool depth_path_requested = control.z_enable || control.z_write_enable || control.stencil_enable ||
	                                  (depth.z_info.tile_surface_enable &&
	                                   (render.depth_compress_disable || render.stencil_compress_disable));
	fprintf(stderr, "[GFX_POLYGON_OFFSET_UNSUPPORTED] kind=%s submit=%" PRIu64 " cp=%p pool=%p queue=%d cb=%u"
	                " recording=%" PRIu64 " session=%" PRIu64 " count=%u flags=%08" PRIx32
	                " primitive=%u pipeline_created=0 depth_attachment_resolved=1 host_depth_format=%u attachment=%u reason=%s\n",
	        kind, submit_id, static_cast<void*>(buffer->GetParent()), static_cast<void*>(buffer->GetPool()), buffer->GetQueue(),
	        buffer->GetIndex(), buffer->GetRecordingId(), buffer->GetSubmissionSessionEpoch(), count, flags, ucfg.GetPrimType(),
	        static_cast<unsigned>(resolved_depth.format), resolved_depth.vulkan_buffer != nullptr ? 1u : 0u, reason);
	fprintf(stderr, "[GFX_POLYGON_OFFSET_MODE] raw=%08" PRIx32 " written=%u front=%u back=%u cull_front=%u cull_back=%u"
	                " face=%u poly_mode=%u front_ptype=%u back_ptype=%u\n",
	        observed.mode_control_raw, observed.mode_control_written ? 1u : 0u,
	        mode.poly_offset_front_enable ? 1u : 0u, mode.poly_offset_back_enable ? 1u : 0u,
	        mode.cull_front ? 1u : 0u, mode.cull_back ? 1u : 0u, mode.face ? 1u : 0u,
	        static_cast<unsigned>(mode.poly_mode), static_cast<unsigned>(mode.polymode_front_ptype),
	        static_cast<unsigned>(mode.polymode_back_ptype));
	fprintf(stderr, "[GFX_POLYGON_OFFSET_RAW] written_mask=%02x unwritten_uses_gen4_clearstate_zero=1"
	                " db_fmt_cntl=%08" PRIx32 " clamp=%08" PRIx32 " front_scale=%08" PRIx32
	                " front_offset=%08" PRIx32 " back_scale=%08" PRIx32 " back_offset=%08" PRIx32 "\n",
	        static_cast<unsigned>(observed.written_mask), observed.values[0], observed.values[1], observed.values[2],
	        observed.values[3], observed.values[4], observed.values[5]);
	fprintf(stderr, "[GFX_POLYGON_OFFSET_DEPTH] path_requested=%u test=%u write=%u compare=%u stencil=%u"
	                " format=%u stencil_format=%u zrange_precision=%u samples=%u htile=%u width=%u height=%u"
	                " read_addr=%016" PRIx64 " write_addr=%016" PRIx64 " view_write_disable=%u"
	                " clear=%u decompress=%u stencil_decompress=%u color_target_mask=%08" PRIx32 "\n",
	        depth_path_requested ? 1u : 0u, control.z_enable ? 1u : 0u, control.z_write_enable ? 1u : 0u,
	        static_cast<unsigned>(control.zfunc), control.stencil_enable ? 1u : 0u, depth.z_info.format,
	        depth.stencil_info.format, depth.z_info.zrange_precision, depth.z_info.num_samples,
	        depth.z_info.tile_surface_enable ? 1u : 0u, depth.width, depth.height, depth.z_read_base_addr,
	        depth.z_write_base_addr, depth.depth_view.depth_write_disable ? 1u : 0u, render.depth_clear_enable ? 1u : 0u,
	        render.depth_compress_disable ? 1u : 0u, render.stencil_compress_disable ? 1u : 0u, ctx.GetRenderTargetMask());
	fprintf(stderr, "[GFX_POLYGON_OFFSET_VIEWPORT] zmin=%g zmax=%g zscale=%g zoffset=%g yscale=%g yoffset=%g"
	                " dx_clip_space=%u min_z_clip_disable=%u max_z_clip_disable=%u\n",
	        viewport.zmin, viewport.zmax, viewport.zscale, viewport.zoffset, viewport.yscale, viewport.yoffset,
	        clip.dx_clip_space ? 1u : 0u, clip.min_z_clip_disable ? 1u : 0u, clip.max_z_clip_disable ? 1u : 0u);
	fprintf(stderr, "[GFX_POLYGON_OFFSET_SHADER] stages=%08" PRIx32 " vs_addr=%016" PRIx64 " vs_embedded=%u"
	                " ps_addr=%016" PRIx64 " ps_embedded=%u ps_chksum=%016" PRIx64
	                " ps_input_ena=%08" PRIx32 " ps_input_addr=%08" PRIx32 " shader_z_format=%08" PRIx32
	                " z_export=%u z_behavior=%u conservative_z=%u kill=%u execute_on_noop=%u other_bits=%08" PRIx32 "\n",
	        ctx.GetShaderStages(), vs.vs_regs.data_addr, vs.vs_embedded ? 1u : 0u, ps.ps_regs.data_addr,
	        ps.ps_embedded ? 1u : 0u, ps.ps_regs.chksum, shader.ps_input_ena, shader.ps_input_addr, shader.shader_z_format,
	        depth_shader.shader_z_export_enable ? 1u : 0u, static_cast<unsigned>(depth_shader.shader_z_behavior),
	        static_cast<unsigned>(depth_shader.conservative_z_export_value), depth_shader.shader_kill_enable ? 1u : 0u,
	        depth_shader.shader_execute_on_noop ? 1u : 0u, depth_shader.other_bits);
	fflush(stderr);
	// The regular DrawState capture runs after pipeline creation and is
	// unreachable at this fatal. Save only already available state when armed.
	if (g_resource_capture.active)
	{
		g_resource_capture.Metadata("polygon_offset_registers", &observed, sizeof(observed));
		g_resource_capture.Metadata("mode_control", &mode, sizeof(mode));
		g_resource_capture.Metadata("depth_target", &depth, sizeof(depth));
		g_resource_capture.Metadata("depth_control", &control, sizeof(control));
		g_resource_capture.Metadata("render_control", &render, sizeof(render));
		g_resource_capture.Metadata("viewport", &ctx.GetScreenViewport(), sizeof(ctx.GetScreenViewport()));
		g_resource_capture.Metadata("clip", &clip, sizeof(clip));
		g_resource_capture.Metadata("shader_registers", &shader, sizeof(shader));
		g_resource_capture.Metadata("vs_registers", &vs, sizeof(vs));
		g_resource_capture.Metadata("ps_registers", &ps, sizeof(ps));
		g_resource_capture.file.Flush();
	}
}

static void RequireSupportedPolygonOffsetDraw(const char* kind, uint64_t submit_id, CommandBuffer* buffer,
                                             const HW::Context& ctx, const HW::UserConfig& ucfg, const HW::Shader& sh,
                                             uint32_t count, uint32_t flags, const RenderDepthInfo& depth)
{
	PipelineDepthBias bias;
	const char* reason = nullptr;
	if (!ResolvePolygonOffset(ctx, depth.format, depth.vulkan_buffer != nullptr, &bias, &reason))
	{
		DumpUnsupportedPolygonOffsetDraw(kind, submit_id, buffer, ctx, ucfg, sh, count, flags, depth, reason);
		EXIT("unsupported polygon offset: %s\n", reason);
	}
}

static void RequireSupportedDrawControl(const HW::Context& ctx)
{
	if (!ctx.IsAlphaToMaskSupportedForDraw())
	{
		EXIT("unsupported DB_ALPHA_TO_MASK for draw: %08" PRIx32 "\n", ctx.GetAlphaToMask());
	}
	if (!ctx.IsMultiVgtParamSupportedForDraw())
	{
		EXIT("unsupported IA_MULTI_VGT_PARAM for draw: %08" PRIx32 "\n", ctx.GetMultiVgtParam());
	}
	if (!ctx.IsScanModeControl1SupportedForDraw())
	{
		EXIT("unsupported PA_SC_MODE_CNTL_1 for draw: %08" PRIx32 "\n", ctx.GetScanModeControl1());
	}
	if (!ctx.IsAaMaskSupportedForDraw())
	{
		EXIT("unsupported PA_SC_AA_MASK for draw: X0Y0_X1Y0=%08" PRIx32 " X0Y1_X1Y1=%08" PRIx32 "\n",
		     ctx.GetAaMask(0), ctx.GetAaMask(1));
	}
}

struct GraphicsRenderPreparedInputAccess
{
	static const void* ThreadIdentity()
	{
		static thread_local const uint8_t identity = 0;
		return &identity;
	}
	static void Reset(GraphicsRenderPreparedInputs& prepared)
	{
		prepared.m_draw.reset();
		prepared.m_compute.reset();
		prepared.m_ready = false;
		prepared.m_consumed = false;
		prepared.m_context = nullptr;
		prepared.m_shader = nullptr;
		prepared.m_thread = nullptr;
	}
	static void Begin(GraphicsRenderPreparedInputs& prepared, const HW::Context* context, const HW::Shader* shader, bool compute)
	{
		Reset(prepared);
		prepared.m_context = context;
		prepared.m_shader = shader;
		prepared.m_thread = ThreadIdentity();
		prepared.m_is_compute = compute;
		std::memcpy(prepared.m_context_bytes.data(), reinterpret_cast<const uint8_t*>(context), sizeof(*context));
		std::memcpy(prepared.m_shader_bytes.data(), reinterpret_cast<const uint8_t*>(shader), sizeof(*shader));
	}
	static bool Matches(const GraphicsRenderPreparedInputs& prepared, const HW::Context* context,
	    const HW::Shader* shader, bool compute)
	{
		return context != nullptr && shader != nullptr && prepared.m_context == context && prepared.m_shader == shader &&
		    prepared.m_thread == ThreadIdentity() && prepared.m_is_compute == compute &&
		    std::memcmp(prepared.m_context_bytes.data(), reinterpret_cast<const uint8_t*>(context), sizeof(*context)) == 0 &&
		    std::memcmp(prepared.m_shader_bytes.data(), reinterpret_cast<const uint8_t*>(shader), sizeof(*shader)) == 0;
	}
	static void Commit(GraphicsRenderPreparedInputs& prepared)
	{
		// Producer completion may rotate the native recording, but must not
		// change the CP's draw/dispatch state while this transaction is owned.
		EXIT_IF(!Matches(prepared, prepared.m_context, prepared.m_shader, prepared.m_is_compute));
		prepared.m_ready = true;
	}
	static bool TryConsume(const GraphicsRenderPreparedInputs& prepared, const HW::Context* context,
	    const HW::Shader* shader, bool compute)
	{
		if (!prepared.m_ready || prepared.m_consumed || !Matches(prepared, context, shader, compute)) return false;
		prepared.m_consumed = true;
		return true;
	}
	static bool Consume(const GraphicsRenderPreparedInputs* prepared, const HW::Context* context,
	    const HW::Shader* shader, bool compute)
	{
		if (prepared == nullptr) return false;
		if (!prepared->m_ready && !GpuMemoryCpuHandoffEnabled()) return false;
		EXIT_IF(!TryConsume(*prepared, context, shader, compute));
		return true;
	}
	static auto& AnalyzeDraw(GraphicsRenderPreparedInputs& prepared, const GraphicsRenderOwnedPreparation* owned = nullptr)
	{
		auto& draw = prepared.m_draw.emplace();
		if (owned != nullptr)
		{
			EXIT_IF(!owned->Consume(prepared.m_context, prepared.m_shader, &draw.vertex, &draw.pixel));
			return draw;
		}
		ShaderGetInputInfoVS(&prepared.m_shader->GetVs(), &prepared.m_context->GetShaderRegisters(), &draw.vertex);
		ShaderGetInputInfoPS(&prepared.m_shader->GetPs(), &prepared.m_context->GetShaderRegisters(), &draw.vertex, &draw.pixel);
		return draw;
	}
	static auto& AnalyzeCompute(GraphicsRenderPreparedInputs& prepared)
	{
		auto& compute = prepared.m_compute.emplace();
		ShaderGetInputInfoCS(&prepared.m_shader->GetCs(), &prepared.m_context->GetShaderRegisters(), &compute);
		return compute;
	}
	static void CopyDraw(const GraphicsRenderPreparedInputs& prepared, ShaderVertexInputInfo* vertex, ShaderPixelInputInfo* pixel)
	{
		EXIT_IF(!prepared.m_consumed || !prepared.m_draw.has_value());
		*vertex = prepared.m_draw->vertex;
		*pixel = prepared.m_draw->pixel;
	}
	static void CopyCompute(const GraphicsRenderPreparedInputs& prepared, ShaderComputeInputInfo* compute)
	{
		EXIT_IF(!prepared.m_consumed || !prepared.m_compute.has_value());
		*compute = *prepared.m_compute;
	}
};

struct GraphicsRenderOwnedPreparation::Impl
{
	ShaderOwnedDrawInputs source;
	ShaderVertexInputInfo vertex;
	ShaderPixelInputInfo pixel;
	std::array<uint8_t, sizeof(HW::Context)> context_bytes {};
	std::array<uint8_t, sizeof(HW::Shader)> shader_bytes {};
	const HW::Context* context = nullptr;
	const HW::Shader* shader = nullptr;
	const void* thread = nullptr;
	enum class State { Empty, Captured, Analyzed, Validated, Consumed };
	mutable State state = State::Empty;
	bool Matches(const HW::Context* current_context, const HW::Shader* current_shader) const
	{
		return current_context != nullptr && current_shader != nullptr && context == current_context && shader == current_shader &&
		    thread == GraphicsRenderPreparedInputAccess::ThreadIdentity() &&
		    std::memcmp(context_bytes.data(), reinterpret_cast<const uint8_t*>(current_context), sizeof(*current_context)) == 0 &&
		    std::memcmp(shader_bytes.data(), reinterpret_cast<const uint8_t*>(current_shader), sizeof(*current_shader)) == 0;
	}
};

GraphicsRenderOwnedPreparation::GraphicsRenderOwnedPreparation() = default;
GraphicsRenderOwnedPreparation::~GraphicsRenderOwnedPreparation() = default;

bool GraphicsRenderOwnedPreparation::Capture(HW::Context* context, HW::Shader* shader)
{
	m_impl.reset();
	if (!GpuMemoryCpuHandoffEnabled() || context == nullptr || shader == nullptr || g_render_ctx == nullptr) return false;
	Core::LockGuard renderer_lock(g_render_ctx->GetMutex());
	if (shader_is_disabled(shader)) return false;
	m_impl = std::make_unique<Impl>();
	auto& owned = *m_impl;
	owned.context = context;
	owned.shader = shader;
	owned.thread = GraphicsRenderPreparedInputAccess::ThreadIdentity();
	std::memcpy(owned.context_bytes.data(), reinterpret_cast<const uint8_t*>(context), sizeof(*context));
	std::memcpy(owned.shader_bytes.data(), reinterpret_cast<const uint8_t*>(shader), sizeof(*shader));
	if (!owned.source.Capture(&shader->GetVs(), &shader->GetPs(), &context->GetShaderRegisters()) ||
	    !owned.Matches(context, shader)) return false;
	owned.state = Impl::State::Captured;
	return true;
}

void GraphicsRenderOwnedPreparation::Analyze()
{
	EXIT_IF(m_impl == nullptr);
	auto& owned = *m_impl;
	EXIT_IF(owned.state != Impl::State::Captured || owned.thread != GraphicsRenderPreparedInputAccess::ThreadIdentity());
	// No CP state, guest pointer, mapping pin or native resource is consulted.
	owned.state = owned.source.Analyze(&owned.vertex, &owned.pixel) ? Impl::State::Analyzed : Impl::State::Empty;
}

bool GraphicsRenderOwnedPreparation::Validate(HW::Context* context, HW::Shader* shader)
{
	if (m_impl == nullptr) return false;
	auto& owned = *m_impl;
	if (owned.state != Impl::State::Analyzed || !owned.Matches(context, shader)) return false;
	Core::LockGuard renderer_lock(g_render_ctx->GetMutex());
	if (!owned.source.Validate() || !owned.Matches(context, shader))
	{
		owned.state = Impl::State::Empty;
		return false;
	}
	owned.state = Impl::State::Validated;
	return true;
}

bool GraphicsRenderOwnedPreparation::IsValidated() const
{
	return m_impl != nullptr && m_impl->state == Impl::State::Validated;
}

const ShaderVertexSourceSnapshot* GraphicsRenderOwnedPreparation::VertexSource() const
{
	return IsValidated() ? m_impl->source.VertexSource() : nullptr;
}

bool GraphicsRenderOwnedPreparation::Consume(const HW::Context* context, const HW::Shader* shader,
    ShaderVertexInputInfo* vertex, ShaderPixelInputInfo* pixel) const
{
	if (vertex == nullptr || pixel == nullptr || !IsValidated()) return false;
	auto& owned = *m_impl;
	if (!owned.Matches(context, shader)) return false;
	*vertex = owned.vertex;
	*pixel = owned.pixel;
	owned.state = Impl::State::Consumed;
	return true;
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool GraphicsRenderPreparedInputsContractTest()
{
	ShaderTextureDescriptor storage;
	storage.usage = ShaderTextureUsage::ReadWrite;
	storage.textures2d_without_sampler = true;
	storage.texture.fields[0] = 0x02000000u;
	storage.texture.fields[1] = (3u << 30u) | (1u << 20u) | (1u << 6u);
	storage.texture.fields[2] = 127u | (63u << 14u);
	storage.texture.fields[3] = (ShaderTextureResource::TYPE_2D << 28u) | (13u << 20u) | DstSel(4, 4, 4, 4);
	storage.texture.fields[4] = 127u << 13u;
	const auto original_storage = storage.texture;
	const StorageTextureObject native_storage(1, 0, 128, 64, 128, 0, 1, 13, false, DstSel(4, 5, 6, 7), false);
	const StorageTextureObject preflight_storage(1, 0, 128, 64, 128, 0, 1, 13, false, TextureNativeViewSwizzle(storage), false);
	if (!ShaderTextureR8UnormStorageCompatible(storage.texture) || !preflight_storage.Equal(native_storage.params) ||
	    std::memcmp(storage.texture.fields, original_storage.fields, sizeof(original_storage.fields)) != 0) return false;
	// Only the already-supported R8 writable view is normalized. Sampled,
	// unknown-use and incompatible descriptors retain their exact selection.
	for (unsigned mutation = 0; mutation < 6; ++mutation)
	{
		auto unchanged = storage;
		switch (mutation)
		{
			case 0: unchanged.usage = ShaderTextureUsage::ReadOnly; break;
			case 1: unchanged.usage = ShaderTextureUsage::Unknown; break;
			case 2: unchanged.texture.fields[1] ^= 1u << 26u; break;
			case 3: unchanged.texture.fields[3] ^= 1u << 20u; break;
			case 4: unchanged.texture.fields[3] ^= 1u; break;
			case 5: unchanged.texture.fields[4] = 126u << 13u; break;
		}
		if (TextureNativeViewSwizzle(unchanged) != unchanged.texture.DstSelXYZW()) return false;
	}
	std::fprintf(stderr, "[GFX_PREFLIGHT_VIEW_DESCRIPTION_TEST] R8-writable-XXXX=native-identity"
	    " exact-object-equality=pass guest-selection=retained sampled/unknown/incompatible=unchanged\n");
	ShaderTextureDescriptor display;
	display.usage = ShaderTextureUsage::ReadOnly;
	display.texture.fields[1] = 10u << 20u;
	display.texture.fields[2] = 63u | (31u << 14u);
	display.texture.fields[3] = (ShaderTextureResource::TYPE_2D << 28u) | (10u << 20u) | DstSel(6, 5, 4, 7);
	display.texture.fields[4] = 127u << 13u;
	for (unsigned mutation = 0; mutation < 8; ++mutation)
	{
		auto descriptor = display;
		switch (mutation)
		{
			case 1: descriptor.texture.fields[1] |= 9u << 26u; break; // SRGB stays explicit, not silently UNORM.
			case 2: descriptor.texture.fields[3] |= 1u << 25u; break;
			case 3: descriptor.texture.fields[3] |= (1u << 12u) | (2u << 16u); break;
			case 4: descriptor.texture.fields[3] ^= 2u; break;
			case 5: descriptor.usage = ShaderTextureUsage::Unknown; break;
			case 6: descriptor.usage = ShaderTextureUsage::ReadWrite; break;
			case 7: descriptor.textures2d_without_sampler = true; break;
		}
		const auto before = descriptor.texture;
		const auto request = SampledTextureBackingRequest(descriptor, false);
		const auto& t = descriptor.texture;
		if (request.type != GpuMemoryObjectType::Texture || request.GetWriteBackFunc() != nullptr ||
		    request.params[TextureObject::PARAM_FORMAT] != ((uint64_t(t.Dfmt()) << 8u) | t.Nfmt()) ||
		    request.params[TextureObject::PARAM_WIDTH_HEIGHT] != ((uint64_t(t.Width4() + 1) << 32u) | (t.Height4() + 1)) ||
		    request.params[TextureObject::PARAM_PITCH] != t.Pitch() + 1u ||
		    request.params[TextureObject::PARAM_LEVELS] != ((uint64_t(t.BaseLevel()) << 32u) | (t.LastLevel() + 1u)) ||
		    request.params[TextureObject::PARAM_TILE] != 10 || request.params[TextureObject::PARAM_NEO] != 0 ||
		    request.params[TextureObject::PARAM_SWIZZLE] != t.DstSelXYZW() ||
		    request.params[TextureObject::PARAM_POW2_PAD] != t.Pow2Pad() ||
		    (request.access_kind == GpuMemoryAccessKind::SampledRead) != (mutation < 5) ||
		    std::memcmp(before.fields, descriptor.texture.fields, sizeof(before.fields)) != 0) return false;
	}
	if (SampledTextureBackingRequest(display, true).params[TextureObject::PARAM_NEO] != 1) return false;
	std::fprintf(stderr, "[GFX_PREFLIGHT_DISPLAY_SAMPLE_TEST] native-sampled-request=pass"
	    " format/swizzle/extent/pitch/mip/pow2/neo=preserved non-sampled-access=denied guest-descriptor=unchanged\n");
	// Use the real shader-input decoder, including direct SGPR packing. The
	// envelope only retains its result; it must not weaken these admissions.
	// Four code words followed by zero-usage binary metadata; the decoder
	// consumes the header at the offset encoded in word 1.
	std::array<uint32_t, 16> fixture {0xbeeb03ffu, 1u, 0xbf810000u, 0};
	HW::Context context;
	HW::Shader shader;
	shader.SetVsShaderBase(reinterpret_cast<uint64_t>(fixture.data()));
	HW::VsShaderResource2 resources {};
	resources.user_sgpr = 2;
	shader.SetVsShaderResource2(resources);
	shader.SetVsUserSgpr(0, 0x12345678u, HW::UserSgprType::Region);
	shader.SetVsUserSgpr(1, 0xabcdef01u, HW::UserSgprType::Region);
	shader.SetPsEmbedded(0);
	GraphicsRenderPreparedInputs prepared;
	using Access = GraphicsRenderPreparedInputAccess;
	if (Access::TryConsume(prepared, &context, &shader, false)) return false;
	Access::Begin(prepared, &context, &shader, false);
	Access::AnalyzeDraw(prepared);
	if (Access::TryConsume(prepared, &context, &shader, false)) return false;
	Access::Commit(prepared);
	if (Access::TryConsume(prepared, &context, &shader, true)) return false;
	HW::Context other_context = context;
	HW::Shader other_shader = shader;
	if (Access::TryConsume(prepared, &other_context, &shader, false) ||
	    Access::TryConsume(prepared, &context, &other_shader, false)) return false;
	bool other_thread_accepted = true;
	std::thread other_thread([&] { other_thread_accepted = Access::TryConsume(prepared, &context, &shader, false); });
	other_thread.join();
	if (other_thread_accepted) return false;
	context.SetVsOutConfig(2);
	if (Access::TryConsume(prepared, &context, &shader, false)) return false;
	context.SetVsOutConfig(0);
	shader.SetVsUserSgpr(0, 9, HW::UserSgprType::Region);
	if (Access::TryConsume(prepared, &context, &shader, false)) return false;
	shader.SetVsUserSgpr(0, 0x12345678u, HW::UserSgprType::Region);
	if (!Access::TryConsume(prepared, &context, &shader, false) ||
	    Access::TryConsume(prepared, &context, &shader, false)) return false;
	ShaderVertexInputInfo vertex;
	ShaderPixelInputInfo pixel;
	Access::CopyDraw(prepared, &vertex, &pixel);
	if (vertex.bind.direct_sgprs.sgprs_num != 2 || vertex.bind.push_constant_size != 16 ||
	    vertex.bind.direct_sgprs.sgprs[0].field != 0x12345678u || vertex.bind.direct_sgprs.sgprs[1].field != 0xabcdef01u)
		return false;
	// Pipeline capability selection and rect-list lowering are draw-local.
	// Mutating those working copies cannot contaminate the prepared analysis.
	vertex.rect_list = true;
	vertex.bind.push_constant_size = 272;
	if (!ShaderSelectConstantStorage(&vertex.bind, &pixel.bind, 256, 65536, 4) ||
	    vertex.bind.constant_storage != ShaderConstantStorage::UniformBuffer) return false;
	Access::CopyDraw(prepared, &vertex, &pixel);
	if (vertex.rect_list || vertex.bind.constant_storage != ShaderConstantStorage::PushConstants ||
	    vertex.bind.push_constant_size != 16) return false;

	HW::CsStageRegisters compute {};
	compute.data_addr = reinterpret_cast<uint64_t>(fixture.data());
	compute.user_sgpr = 2;
	compute.num_thread_x = 8; compute.num_thread_y = 4; compute.num_thread_z = 2;
	compute.tgid_x_en = 1; compute.tgid_z_en = 1; compute.tidig_comp_cnt = 2;
	shader.SetCsShader(compute, 0);
	shader.SetCsUserSgpr(0, 0x13579bdfu, HW::UserSgprType::Region);
	shader.SetCsUserSgpr(1, 0x2468ace0u, HW::UserSgprType::Region);
	Access::Begin(prepared, &context, &shader, true);
	Access::AnalyzeCompute(prepared);
	Access::Commit(prepared);
	if (Access::TryConsume(prepared, &context, &shader, false) ||
	    !Access::TryConsume(prepared, &context, &shader, true)) return false;
	ShaderComputeInputInfo output;
	Access::CopyCompute(prepared, &output);
	if (output.threads_num[0] != 8 || output.threads_num[1] != 4 || output.threads_num[2] != 2 ||
	    !output.group_id[0] || output.group_id[1] || !output.group_id[2] || output.thread_ids_num != 3 ||
	    output.bind.direct_sgprs.sgprs_num != 2 || output.bind.direct_sgprs.sgprs[0].field != 0x13579bdfu ||
	    output.bind.direct_sgprs.sgprs[1].field != 0x2468ace0u) return false;
	Access::Reset(prepared);
	if (Access::TryConsume(prepared, &context, &shader, true)) return false;
	std::fprintf(stderr, "[GFX_PREPARED_INPUTS_TEST] real-vs/cs-decoder=pass sgpr-packing=pass working-copy-isolation=pass"
	    " wrong-stage/owner/thread/state/replay=reject unfinished/reset=reject lifetime=one-synchronous-command\n");
	return true;
}
#endif

// Resolve upload/replacement dependencies before either retained recording
// scope captures a command buffer. Completing an older producer may rotate it.
static void PrepareResourceBackingImpl(HW::Context* hw, HW::Shader* shader, bool compute,
    uint64_t index_address, uint64_t index_bytes, GraphicsRenderPreparedInputs* prepared,
    const GraphicsRenderOwnedPreparation* owned, std::vector<GpuMemoryClaimRange>* claim_ranges)
{
	Lumen::CriticalPath::Scope preflight(Lumen::CriticalPath::Reason::GpuResourcePreflight,
	    reinterpret_cast<uintptr_t>(hw), compute ? 1 : 0);
	if (!GpuMemoryCpuHandoffEnabled())
	{
		if (prepared != nullptr) GraphicsRenderPreparedInputAccess::Reset(*prepared);
		return;
	}
	EXIT_IF(hw == nullptr || shader == nullptr || g_render_ctx == nullptr);
	Core::LockGuard renderer_lock(g_render_ctx->GetMutex());
	GraphicsRenderPreparedInputs local;
	auto& inputs = prepared != nullptr ? *prepared : local;
	GraphicsRenderPreparedInputAccess::Begin(inputs, hw, shader, compute);
	auto* graphic = g_render_ctx->GetGraphicCtx();
	const auto prepare = [&](uint64_t address, uint64_t bytes, const GpuObject& object) {
		if (claim_ranges != nullptr) claim_ranges->push_back({address, bytes});
		if (!GpuMemoryPrepareResourceBacking(graphic, &address, &bytes, 1, object))
			EXIT("[GPU_MEMORY_HANDOFF_PREFLIGHT_DENIED] type=%s address=0x%" PRIx64 " bytes=0x%" PRIx64
			     " compute=%u params=%" PRIx64 ",%" PRIx64 ",%" PRIx64 ",%" PRIx64 ",%" PRIx64
			     ",%" PRIx64 ",%" PRIx64 ",%" PRIx64 "\n", Core::EnumName(object.type).C_Str(), address, bytes,
			     compute ? 1u : 0u, object.params[0], object.params[1], object.params[2], object.params[3],
			     object.params[4], object.params[5], object.params[6], object.params[7]);
	};
	const auto bindings = [&](const ShaderBindResources& bind) {
		for (int i = 0; i < bind.storage_buffers.buffers_num; ++i)
		{
			const auto& b = bind.storage_buffers.buffers[i];
			prepare(b.Base44(), uint64_t(b.Stride()) * b.NumRecords(),
			    StorageBufferGpuObject(b.Stride(), b.NumRecords(), b.MemoryType() == 0x10));
		}
		for (int i = 0; i < bind.textures2D.textures_num; ++i)
		{
			const auto& descriptor = bind.textures2D.desc[i];
			const auto& t = descriptor.texture;
			if (descriptor.IsNullSampledImage()) continue;
			const auto width = uint32_t(t.Width4()) + 1, height = uint32_t(t.Height4()) + 1, pitch = uint32_t(t.Pitch()) + 1;
			TileSizeAlign size {};
			TileGetTextureSize(t.Dfmt(), t.Nfmt(), width, height, pitch, t.LastLevel() + 1, t.TileMode(), Config::IsNeo(),
			    &size, nullptr, nullptr, t.Pow2Pad());
			if (descriptor.textures2d_without_sampler || t.BaseLevel() != 0)
				prepare(t.Base38(), size.size, StorageTextureObject(t.Dfmt(), t.Nfmt(), width, height, pitch, t.BaseLevel(),
				    t.LastLevel() + 1, t.TileMode(), Config::IsNeo(), TextureNativeViewSwizzle(descriptor), t.Pow2Pad()));
			else
			{
				// Binding first selects an exact native render image and its sampled
				// view, including Display2D/BGRA. Describe that read before considering
				// the fallback upload; it is not a new RGBA color attachment.
				const auto object = SampledTextureBackingRequest(descriptor, Config::IsNeo());
				prepare(t.Base38(), size.size, object);
			}
		}
	};
	if (compute)
	{
		if (ShaderIsDisabled(shader->GetCs().cs_regs.data_addr))
		{
			GraphicsRenderPreparedInputAccess::Commit(inputs);
			return;
		}
		const auto& input = GraphicsRenderPreparedInputAccess::AnalyzeCompute(inputs);
		bindings(input.bind);
		GraphicsRenderPreparedInputAccess::Commit(inputs);
		return;
	}
	if (shader_is_disabled(shader))
	{
		GraphicsRenderPreparedInputAccess::Commit(inputs);
		return;
	}
	const auto& draw = GraphicsRenderPreparedInputAccess::AnalyzeDraw(inputs, owned);
	const auto& vertex = draw.vertex;
	const auto& pixel = draw.pixel;
	for (int i = 0; i < vertex.buffers_num; ++i)
		prepare(vertex.buffers[i].addr, uint64_t(vertex.buffers[i].stride) * vertex.buffers[i].num_records, VertexBufferGpuObject());
	bindings(vertex.bind);
	bindings(pixel.bind);
	if (index_bytes != 0) prepare(index_address, index_bytes, IndexBufferGpuObject());
	const auto& color = hw->GetRenderTarget(0);
	if (color.base.addr != 0 && hw->GetRenderTargetMask() != 0)
	{
		const auto descriptor = BuildPs4ColorRenderTargetDescriptor(color.info.format, color.info.channel_type,
		    color.info.channel_order, color.slice.slice_div64_minus1);
		const auto pitch = (color.pitch.pitch_div8_minus1 + 1) * 8;
		const bool writeback = color.attrib.tile_mode == 31 || (color.attrib.tile_mode == 10 &&
		    RenderTextureObject::SupportsDisplay2dReadback(descriptor.format.format, color.size.width, color.size.height, pitch,
		        color.info.neo_mode, color.base.addr, descriptor.byte_size));
		prepare(color.base.addr, descriptor.byte_size, RenderTextureObject(descriptor.format.format, color.size.width,
		    color.size.height, color.attrib.tile_mode != 8 && color.attrib.tile_mode != 31, color.info.neo_mode,
		    pitch, writeback, color.attrib.tile_mode));
	}
	const auto& depth = hw->GetDepthRenderTarget();
	const auto& depth_control = hw->GetDepthControl();
	const auto& render_control = hw->GetRenderControl();
	if (depth_control.z_enable || depth_control.z_write_enable || depth_control.stencil_enable ||
	    (depth.z_info.tile_surface_enable && (render_control.depth_compress_disable || render_control.stencil_compress_disable)))
	{
		DepthSurfaceLayout layout {};
		EXIT_IF(!ResolveDepthSurfaceLayout(depth, Config::IsNeo(), &layout));
		uint64_t addresses[3] {}, sizes[3] {}; int count = 0;
		for (const auto& plane: {layout.depth, layout.stencil, layout.htile})
			if (plane.allocation.size != 0)
			{
				addresses[count] = plane.address;
				sizes[count++] = plane.allocation.size;
				if (claim_ranges != nullptr) claim_ranges->push_back({plane.address, plane.allocation.size});
			}
		// The native attachment keeps its exact backing independent of color or
		// shader upload peers. Parameter shape is checked by ordinary binding.
		DepthStencilBufferObject object(VK_FORMAT_D32_SFLOAT_S8_UINT, depth.width, depth.height,
		    depth.z_info.tile_surface_enable, Config::IsNeo(), false, false, false,
		    depth.pitch_div8_minus1, depth.slice_div64_minus1, depth.depth_info.addr5_swizzle_mask == 0);
		EXIT_IF(count != 0 && !GpuMemoryPrepareResourceBacking(graphic, addresses, sizes, count, object));
	}
	GraphicsRenderPreparedInputAccess::Commit(inputs);
}

void GraphicsRenderPrepareResourceBacking(HW::Context* hw, HW::Shader* shader, bool compute,
    uint64_t index_address, uint64_t index_bytes, GraphicsRenderPreparedInputs* prepared,
    const GraphicsRenderOwnedPreparation* owned)
{
	PrepareResourceBackingImpl(hw, shader, compute, index_address, index_bytes, prepared, owned, nullptr);
}

struct GraphicsRenderRangePreparation::Impl
{
	GraphicsRenderOwnedPreparation sources;
	GpuMemoryRangeClaimPlan plan;
	LabelScalarPublicationCensusBank completed_labels;
	const ShaderVertexSourceSnapshot* vertex_source = nullptr;
};

GraphicsRenderRangePreparation::GraphicsRenderRangePreparation() = default;
GraphicsRenderRangePreparation::~GraphicsRenderRangePreparation() = default;

bool GraphicsRenderRangePreparation::Capture(HW::Context* context, HW::Shader* shader,
    uint64_t index_address, uint64_t index_bytes, GraphicsRenderPreparedInputs* prepared,
    const std::vector<GpuMemoryClaimRange>& additional_ranges)
{
	struct Counts
	{
		uint64_t attempts = 0, success = 0, owner = 0, sources = 0, source_validation = 0, inventory = 0;
		uint64_t post_validation = 0, census = 0, pending_labels = 0, closure = 0, census_validation = 0;
	};
	static thread_local Counts counts;
	struct Receipt
	{
		Counts& counts;
		size_t ranges = 0;
		~Receipt()
		{
			if ((counts.attempts & (counts.attempts - 1)) == 0)
				std::fprintf(stderr, "[GPU_RANGE_PREPARATION_COUNTS] attempts=%" PRIu64 " success=%" PRIu64
				    " owner=%" PRIu64 " sources=%" PRIu64 " source_validation=%" PRIu64 " inventory=%" PRIu64
				    " post_validation=%" PRIu64 " census=%" PRIu64 " pending_labels=%" PRIu64 " closure=%" PRIu64
				    " census_validation=%" PRIu64 " last_seed_ranges=%zu per-thread=1\n", counts.attempts, counts.success,
				    counts.owner, counts.sources, counts.source_validation, counts.inventory, counts.post_validation,
				    counts.census, counts.pending_labels, counts.closure, counts.census_validation, ranges);
		}
	} receipt {counts};
	++counts.attempts;
	const auto reject = [](uint64_t& counter) { ++counter; return false; };
	Reset();
	if (prepared == nullptr || Core::VirtualMemory::GetOwnedGuestMemoryMutationToken() == 0 ||
	    additional_ranges.size() > 1024) return reject(counts.owner);
	auto captured = std::make_unique<Impl>();
	if (!captured->sources.Capture(context, shader)) return reject(counts.sources);
	captured->sources.Analyze();
	if (!captured->sources.Validate(context, shader)) return reject(counts.source_validation);
	captured->vertex_source = captured->sources.VertexSource();
	std::vector<GpuMemoryClaimRange> ranges = additional_ranges;
	if (!captured->sources.m_impl->source.VisitRanges(+[](void* opaque, uint64_t address, uint64_t bytes) {
		    auto& output = *static_cast<std::vector<GpuMemoryClaimRange>*>(opaque);
		    if (output.size() >= 1024) return false;
		    output.push_back({address, bytes});
		    return true;
	    }, &ranges)) return reject(counts.inventory);
	// Use the exact production resource visitor, including all attachment
	// planes. A separately maintained descriptor inventory could miss a new
	// format, mip window or resource routing path added to normal rendering.
	PrepareResourceBackingImpl(context, shader, false, index_address, index_bytes, prepared, &captured->sources, &ranges);
	receipt.ranges = ranges.size();
	// A resource preflight may complete an older GPU producer. Recheck the
	// original shader/descriptor bytes after those writebacks, before any native
	// draw effects can use the retained analysis outside exclusive admission.
	if (!captured->sources.m_impl->source.Validate()) return reject(counts.post_validation);
	// Completing a producer can dispatch a Label callback whose writeback
	// reaches beyond this draw's resources. The whole publication census also
	// includes detached worker deliveries; only already completed enrollments
	// are eligible. The native GPU admission permit prevents new enrollment.
	if (!LabelCaptureScalarPublicationCensusBank(&captured->completed_labels)) return reject(counts.census);
	if (captured->completed_labels.ScalarCount() != 0) return reject(counts.pending_labels);
	if (!captured->plan.Capture(ranges)) return reject(counts.closure);
	if (!LabelValidateScalarPublicationCensusBank(captured->completed_labels)) return reject(counts.census_validation);
	m_impl = std::move(captured);
	++counts.success;
	return true;
}

const GpuMemoryRangeClaimPlan* GraphicsRenderRangePreparation::RangePlan() const
{
	return m_impl != nullptr ? &m_impl->plan : nullptr;
}

uint64_t GraphicsRenderRangePreparation::Owner() const
{
	return m_impl != nullptr ? m_impl->plan.Owner() : 0;
}

const ShaderVertexSourceSnapshot* GraphicsRenderRangePreparation::VertexSource() const
{
	return m_impl != nullptr ? m_impl->vertex_source : nullptr;
}

void GraphicsRenderRangePreparation::Reset() { m_impl.reset(); }

void GraphicsRenderDrawIndex(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::UserConfig* ucfg, HW::Shader* sh_ctx,
                             uint32_t index_type_and_size, uint32_t index_count, const void* index_addr, uint32_t flags, uint32_t type,
                             const GraphicsRenderPreparedInputs* prepared)
{
	LUMEN_PROFILER_FUNCTION();
	const bool diagnostics_enabled = Config::GraphicsTimingEnabled();

	std::chrono::steady_clock::time_point diagnostic_points[12] {};
	diagnostic_points[0] = GetDiagnosticTimestamp(diagnostics_enabled);

	EXIT_IF(ctx == nullptr);
	RequireSupportedDrawControl(*ctx);
	if (!ctx->IsIndexOffsetSupportedForDraw())
	{
		EXIT("unsupported VGT_INDX_OFFSET for indexed draw: %08" PRIx32 "\n", ctx->GetIndexOffset());
	}
	if (!ctx->IsGeometryStreamoutSupportedForDraw())
	{
		EXIT("unsupported geometry/stream-out state for indexed draw: VGT_GS_MODE=%08" PRIx32
		     " configured=%u VGT_STRMOUT_CONFIG=%08" PRIx32 " configured=%u\n",
		     ctx->GetGsMode(), ctx->IsGsModeConfigured() ? 1u : 0u, ctx->GetStreamoutConfig(),
		     ctx->IsStreamoutConfigConfigured() ? 1u : 0u);
	}
	EXIT_IF(ucfg == nullptr);
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());
	const bool inputs_prepared = GraphicsRenderPreparedInputAccess::Consume(prepared, ctx, sh_ctx, false);
	VideoOutRendererAdmissionScope videoout_admission_scope;
	RetainedRendererRecordingScope retained_recording_scope(buffer);
	GfxResourceCaptureEvent capture_event(submit_id, buffer, "draw_index");
	if (g_resource_capture.active)
	{
		g_resource_capture.file.Printf("draw count=%u index_type=%u flags=%u type=%u primitive=%u\n",
		                              index_count, index_type_and_size, flags, type, ucfg->GetPrimType());
		g_resource_capture.Shader("vs", sh_ctx->GetVs().vs_regs.data_addr, sh_ctx->GetVs().vs_embedded, sh_ctx->GetVs().vs_embedded_id);
		g_resource_capture.Shader("ps", sh_ctx->GetPs().ps_regs.data_addr, sh_ctx->GetPs().ps_embedded, sh_ctx->GetPs().ps_embedded_id);
	}
	diagnostic_points[1] = GetDiagnosticTimestamp(diagnostics_enabled);

	if (shader_is_disabled(sh_ctx))
	{
		if (g_resource_capture.active) { g_resource_capture.file.Printf("skip reason=disabled_shader\n"); }
		return;
	}

	if (Lumen::Log::GetDirection() != Lumen::Log::Direction::Silent)
	{
		sh_print("GraphicsRenderDrawIndex():Shader:", *sh_ctx);
		uc_print("GraphicsRenderDrawIndex():UserConfig:", *ucfg);
		hw_print(*ctx);
	}
	sh_check(*sh_ctx);

	uc_check(*ucfg);

	hw_check(*ctx);

	EXIT_NOT_IMPLEMENTED(ctx->GetShaderStages() != 0 && ctx->GetShaderStages() != 0x02002000);

	const auto primitive_type = ucfg->GetPrimType();
	VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

	switch (primitive_type)
	{
		case 4: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;  // kPrimitiveTypeTriList
		case 5: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;   // kPrimitiveTypeTriFan
		case 6: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break; // kPrimitiveTypeTriStrip
		case 19: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;  // kPrimitiveTypeQuadList
		default: EXIT("unknown primitive type: %u\n", primitive_type);
	}

	if (Lumen::Log::GetDirection() != Lumen::Log::Direction::Silent)
	{
		printf("GraphicsRenderDrawIndex():Parameters:\n");
		printf("\t index_type_and_size = 0x%08" PRIx32 "\n", index_type_and_size);
		printf("\t index_count         = 0x%08" PRIx32 "\n", index_count);
		printf("\t index_addr          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(index_addr));
		printf("\t flags               = 0x%08" PRIx32 "\n", flags);
		printf("\t type                = 0x%08" PRIx32 "\n", type);
	}

	VkIndexType index_type = VK_INDEX_TYPE_UINT16;
	uint64_t    index_size = 0;

	switch (index_type_and_size)
	{
		case 0:
			index_type = VK_INDEX_TYPE_UINT16;
			index_size = 2 * static_cast<uint64_t>(index_count);
			break;
		case 1:
			index_type = VK_INDEX_TYPE_UINT32;
			index_size = 4 * static_cast<uint64_t>(index_count);
			break;
		default: EXIT("unknown index_type_and_size: %u\n", index_type_and_size);
	}

	EXIT_NOT_IMPLEMENTED(flags != 0);
	EXIT_NOT_IMPLEMENTED(type != 1);
	diagnostic_points[2] = GetDiagnosticTimestamp(diagnostics_enabled);

	RenderDepthInfo depth_info;
	FindRenderDepthInfo(submit_id, buffer, *ctx, &depth_info);
	RequireSupportedPolygonOffsetDraw("draw_index", submit_id, buffer, *ctx, *ucfg, *sh_ctx, index_count, flags, depth_info);

	RenderColorInfo color_info;
	FindRenderColorInfo(submit_id, buffer, *ctx, &color_info);
	diagnostic_points[3] = GetDiagnosticTimestamp(diagnostics_enabled);

	auto* framebuffer = g_render_ctx->GetFramebufferCache()->CreateFramebuffer(&color_info, &depth_info);
	diagnostic_points[4] = GetDiagnosticTimestamp(diagnostics_enabled);

	EXIT_NOT_IMPLEMENTED(framebuffer == nullptr);
	EXIT_NOT_IMPLEMENTED(framebuffer->render_pass == nullptr);

	auto* vk_buffer = buffer->GetBufferForDraw();

	ShaderVertexInputInfo vs_input_info;
	ShaderPixelInputInfo ps_input_info;
	if (inputs_prepared) GraphicsRenderPreparedInputAccess::CopyDraw(*prepared, &vs_input_info, &ps_input_info);
	else
	{
		ShaderGetInputInfoVS(&sh_ctx->GetVs(), &ctx->GetShaderRegisters(), &vs_input_info);
		ShaderGetInputInfoPS(&sh_ctx->GetPs(), &ctx->GetShaderRegisters(), &vs_input_info, &ps_input_info);
	}
	diagnostic_points[5] = GetDiagnosticTimestamp(diagnostics_enabled);

	auto* pipeline = g_render_ctx->GetPipelineCache()->CreatePipeline(framebuffer, &color_info, &depth_info, &vs_input_info, ctx, sh_ctx,
	                                                                  &ps_input_info, topology);
	diagnostic_points[6] = GetDiagnosticTimestamp(diagnostics_enabled);

	// EXIT_NOT_IMPLEMENTED(vs_input_info.buffers_num > 1);

	vkCmdBindPipeline(vk_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);

	SetDynamicParams(vk_buffer, pipeline);
	g_resource_capture.DrawState(ctx, sh_ctx, pipeline, vs_input_info, ps_input_info, color_info, depth_info);

	std::optional<DrawAccessTransaction> draw_access;
	if (DrawAccessPlanEnabled()) draw_access.emplace();
	GpuMemoryImageAccessBatch image_accesses;
	GpuMemoryStorageAccessBatch storage_accesses;
	if (draw_access)
	{
		image_accesses.native_inputs = &draw_access->inventory;
		storage_accesses.native_inputs = &draw_access->inventory;
	}
	image_accesses.SetRenderAttachments(color_info, depth_info);
	for (int i = 0; i < vs_input_info.buffers_num; i++)
	{
		const auto& b    = vs_input_info.buffers[i];
		uint64_t    addr = b.addr;
		uint64_t    size = static_cast<uint64_t>(b.stride) * b.num_records;

		auto* vertices = static_cast<VulkanBuffer*>(
		    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), buffer, addr, size, VertexBufferGpuObject()));
		g_resource_capture.Buffer("vertex", i, vertices);
		g_resource_capture.Range("vertex_guest", i, addr, size);

		if (draw_access) draw_access->inventory.Add(vertices, GpuMemoryObjectType::VertexBuffer, {}, addr, size);
		image_accesses.ObserveRenderResource(vertices->memory.memory);
		VkDeviceSize offset = 0;

		vkCmdBindVertexBuffers(vk_buffer, i, 1, &vertices->buffer, &offset);
	}
	diagnostic_points[7] = GetDiagnosticTimestamp(diagnostics_enabled);


	ShaderConstantUpload constant_upload(vs_input_info.bind, &ps_input_info.bind);
	BindDescriptors(submit_id, buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout, vs_input_info.bind,
	                false, pipeline->vs_storage_write_authority, VK_SHADER_STAGE_VERTEX_BIT, DescriptorCache::Stage::Vertex, &image_accesses,
	                &color_info, &storage_accesses, pipeline, &constant_upload);
	diagnostic_points[8] = GetDiagnosticTimestamp(diagnostics_enabled);

	BindDescriptors(submit_id, buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout, ps_input_info.bind,
	                pipeline->ps_has_image_load, pipeline->ps_storage_write_authority, VK_SHADER_STAGE_FRAGMENT_BIT,
	                DescriptorCache::Stage::Pixel, &image_accesses,
	                &color_info, &storage_accesses, pipeline, &constant_upload);
	constant_upload.Bind(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout,
	    draw_access ? &draw_access->inventory : nullptr);
	diagnostic_points[9] = GetDiagnosticTimestamp(diagnostics_enabled);

	VulkanBuffer* indices = static_cast<VulkanBuffer*>(GpuMemoryCreateObject(
	    submit_id, g_render_ctx->GetGraphicCtx(), buffer, reinterpret_cast<uint64_t>(index_addr), index_size, IndexBufferGpuObject()));

	EXIT_NOT_IMPLEMENTED(indices == nullptr);
	g_resource_capture.stage = -1;
	g_resource_capture.Buffer("index", 0, indices);
	g_resource_capture.Range("index_guest", 0, reinterpret_cast<uint64_t>(index_addr), index_size);

	if (draw_access) draw_access->inventory.Add(indices, GpuMemoryObjectType::IndexBuffer, {},
	    reinterpret_cast<uint64_t>(index_addr), index_size);
	image_accesses.ObserveRenderResource(indices->memory.memory);
	vkCmdBindIndexBuffer(vk_buffer, indices->buffer, 0, index_type);
	diagnostic_points[10] = GetDiagnosticTimestamp(diagnostics_enabled);

	if (draw_access) draw_access->Prepare(buffer, submit_id, index_count != 0, color_info, depth_info, pipeline,
	    framebuffer, image_accesses, storage_accesses);

	if (index_count != 0 && image_accesses.sampled_videoout_count != 0)
	{
		GraphicsRenderVideoOutSampledAliasBarrier(buffer->GetBufferOutsideRenderPass(), image_accesses.sampled_videoout,
		                                           image_accesses.sampled_videoout_count, false);
	}
	buffer->BeginRenderPass(framebuffer, &color_info, &depth_info, image_accesses.render_pass_read_only);

	switch (primitive_type)
	{
		case 4:
		case 5:
		case 6: vkCmdDrawIndexed(vk_buffer, index_count, 1, 0, 0, 0); break;
		case 19:
			EXIT_NOT_IMPLEMENTED((index_count & 0x3u) != 0);
			for (uint32_t i = 0; i < index_count; i += 4)
			{
				vkCmdDrawIndexed(vk_buffer, 4, 1, i, 0, 0);
			}
			break;
		default: EXIT("unknown primitive type: %u\n", primitive_type);
	}
	if (draw_access && draw_access->Reserved()) draw_access->CommitShader();
	else RecordShaderResourceCommandAccesses(buffer, index_count, 1, 1, image_accesses, storage_accesses,
	                                   GpuMemoryCommandMutationPolicy::PotentialStorageWrite);

	buffer->EndRenderPass();
	if (index_count != 0 && image_accesses.sampled_videoout_count != 0)
	{
		GraphicsRenderVideoOutSampledAliasBarrier(buffer->GetBufferOutsideRenderPass(), image_accesses.sampled_videoout,
		                                           image_accesses.sampled_videoout_count, true);
	}

	if (draw_access && draw_access->Reserved()) draw_access->CommitAttachments();
	else
	{
		InvalidateMemoryObject(buffer, color_info, index_count != 0);
		InvalidateMemoryObject(buffer, submit_id, depth_info, pipeline, index_count != 0);
	}
	diagnostic_points[11] = GetDiagnosticTimestamp(diagnostics_enabled);
	if (diagnostics_enabled)
	{
		static thread_local DrawTimingStats diagnostic_stats;
		RecordDrawTiming(&diagnostic_stats, "draw_index", diagnostic_points);
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void GraphicsRenderDrawIndexAuto(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::UserConfig* ucfg, HW::Shader* sh_ctx,
                                 uint32_t index_count, uint32_t flags, const GraphicsRenderPreparedInputs* prepared)
{
	LUMEN_PROFILER_FUNCTION();
	const bool diagnostics_enabled = Config::GraphicsTimingEnabled();

	std::chrono::steady_clock::time_point diagnostic_points[12] {};
	diagnostic_points[0] = GetDiagnosticTimestamp(diagnostics_enabled);

	EXIT_IF(ctx == nullptr || ucfg == nullptr);
	RequireSupportedDrawControl(*ctx);
	if (!ctx->IsIndexOffsetSupportedForDraw())
	{
		EXIT("unsupported VGT_INDX_OFFSET for auto-index draw: %08" PRIx32 "\n", ctx->GetIndexOffset());
	}
	if (!ctx->IsGeometryStreamoutSupportedForDraw())
	{
		EXIT("unsupported geometry/stream-out state for auto-index draw: VGT_GS_MODE=%08" PRIx32
		     " configured=%u VGT_STRMOUT_CONFIG=%08" PRIx32 " configured=%u\n",
		     ctx->GetGsMode(), ctx->IsGsModeConfigured() ? 1u : 0u, ctx->GetStreamoutConfig(),
		     ctx->IsStreamoutConfigConfigured() ? 1u : 0u);
	}
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr || buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());
	const bool inputs_prepared = GraphicsRenderPreparedInputAccess::Consume(prepared, ctx, sh_ctx, false);
	VideoOutRendererAdmissionScope videoout_admission_scope;
	RetainedRendererRecordingScope retained_recording_scope(buffer);
	GfxResourceCaptureEvent capture_event(submit_id, buffer, "draw_auto");
	if (g_resource_capture.active)
	{
		g_resource_capture.file.Printf("draw count=%u flags=%u primitive=%u\n", index_count, flags, ucfg->GetPrimType());
		g_resource_capture.Shader("vs", sh_ctx->GetVs().vs_regs.data_addr, sh_ctx->GetVs().vs_embedded, sh_ctx->GetVs().vs_embedded_id);
		g_resource_capture.Shader("ps", sh_ctx->GetPs().ps_regs.data_addr, sh_ctx->GetPs().ps_embedded, sh_ctx->GetPs().ps_embedded_id);
	}
	diagnostic_points[1] = GetDiagnosticTimestamp(diagnostics_enabled);

	if (shader_is_disabled(sh_ctx))
	{
		if (g_resource_capture.active) { g_resource_capture.file.Printf("skip reason=disabled_shader\n"); }
		return;
	}

	if (Lumen::Log::GetDirection() != Lumen::Log::Direction::Silent)
	{
		sh_print("GraphicsRenderDrawIndexAuto():Shader:", *sh_ctx);
		uc_print("GraphicsRenderDrawIndexAuto():UserConfig:", *ucfg);
		hw_print(*ctx);
	}
	sh_check(*sh_ctx);

	uc_check(*ucfg);

	hw_check(*ctx);

	if (Lumen::Log::GetDirection() != Lumen::Log::Direction::Silent)
	{
		printf("GraphicsRenderDrawIndexAuto():Parameters:\n");
		printf("\t index_count         = 0x%08" PRIx32 "\n", index_count);
		printf("\t flags               = 0x%08" PRIx32 "\n", flags);
	}

	EXIT_NOT_IMPLEMENTED(flags != 0);
	EXIT_NOT_IMPLEMENTED(ctx->GetShaderStages() != 0 && ctx->GetShaderStages() != 0x02002000);
	diagnostic_points[2] = GetDiagnosticTimestamp(diagnostics_enabled);

	RenderDepthInfo depth_info;
	FindRenderDepthInfo(submit_id, buffer, *ctx, &depth_info);
	RequireSupportedPolygonOffsetDraw("draw_auto", submit_id, buffer, *ctx, *ucfg, *sh_ctx, index_count, flags, depth_info);

	RenderColorInfo color_info;
	FindRenderColorInfo(submit_id, buffer, *ctx, &color_info);
	diagnostic_points[3] = GetDiagnosticTimestamp(diagnostics_enabled);

	auto* framebuffer = g_render_ctx->GetFramebufferCache()->CreateFramebuffer(&color_info, &depth_info);
	diagnostic_points[4] = GetDiagnosticTimestamp(diagnostics_enabled);

	EXIT_NOT_IMPLEMENTED(framebuffer == nullptr);
	EXIT_NOT_IMPLEMENTED(framebuffer->render_pass == nullptr);

	auto* vk_buffer = buffer->GetBufferForDraw();

	AutoIndexPrimitiveProfile primitive;
	if (!ResolveAutoIndexPrimitive(ucfg->GetPrimType(), &primitive))
	{
		EXIT("unknown primitive type: %u\n", ucfg->GetPrimType());
	}
	if (primitive.mode == AutoIndexPrimitiveMode::QuadList)
	{
		EXIT_NOT_IMPLEMENTED((index_count % QUAD_LIST_VERTICES_PER_QUAD) != 0);
	}

	const auto& vertex_shader_info = sh_ctx->GetVs();
	const auto& pixel_shader_info  = sh_ctx->GetPs();
	const auto& shader_regs        = ctx->GetShaderRegisters();

	ShaderVertexInputInfo vs_input_info;
	ShaderPixelInputInfo ps_input_info;
	if (inputs_prepared) GraphicsRenderPreparedInputAccess::CopyDraw(*prepared, &vs_input_info, &ps_input_info);
	else ShaderGetInputInfoVS(&vertex_shader_info, &shader_regs, &vs_input_info);
	vs_input_info.rect_list = primitive.mode == AutoIndexPrimitiveMode::RectList && !vertex_shader_info.vs_embedded &&
	                          ShaderRectListExperimentEnabled();

	if (!inputs_prepared) ShaderGetInputInfoPS(&pixel_shader_info, &shader_regs, &vs_input_info, &ps_input_info);
	diagnostic_points[5] = GetDiagnosticTimestamp(diagnostics_enabled);

	auto* pipeline = g_render_ctx->GetPipelineCache()->CreatePipeline(framebuffer, &color_info, &depth_info, &vs_input_info, ctx, sh_ctx,
	                                                                  &ps_input_info, primitive.topology);
	diagnostic_points[6] = GetDiagnosticTimestamp(diagnostics_enabled);

	// EXIT_NOT_IMPLEMENTED(vs_input_info.buffers_num > 1);

	vkCmdBindPipeline(vk_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);

	SetDynamicParams(vk_buffer, pipeline);
	g_resource_capture.DrawState(ctx, sh_ctx, pipeline, vs_input_info, ps_input_info, color_info, depth_info);

	std::optional<DrawAccessTransaction> draw_access;
	if (DrawAccessPlanEnabled()) draw_access.emplace();
	GpuMemoryImageAccessBatch image_accesses;
	GpuMemoryStorageAccessBatch storage_accesses;
	if (draw_access)
	{
		image_accesses.native_inputs = &draw_access->inventory;
		storage_accesses.native_inputs = &draw_access->inventory;
	}
	image_accesses.SetRenderAttachments(color_info, depth_info);
	for (int i = 0; i < vs_input_info.buffers_num; i++)
	{
		const auto& b    = vs_input_info.buffers[i];
		uint64_t    addr = b.addr;
		uint64_t    size = static_cast<uint64_t>(b.stride) * b.num_records;

		auto* vertices = static_cast<VulkanBuffer*>(
		    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), buffer, addr, size, VertexBufferGpuObject()));
		g_resource_capture.Buffer("vertex", i, vertices);
		g_resource_capture.Range("vertex_guest", i, addr, size);

		if (draw_access) draw_access->inventory.Add(vertices, GpuMemoryObjectType::VertexBuffer, {}, addr, size);
		image_accesses.ObserveRenderResource(vertices->memory.memory);
		VkDeviceSize offset = 0;

		vkCmdBindVertexBuffers(vk_buffer, i, 1, &vertices->buffer, &offset);
	}
	diagnostic_points[7] = GetDiagnosticTimestamp(diagnostics_enabled);


	ShaderConstantUpload constant_upload(vs_input_info.bind, &ps_input_info.bind);
	BindDescriptors(submit_id, buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout, vs_input_info.bind,
	                false, pipeline->vs_storage_write_authority, VK_SHADER_STAGE_VERTEX_BIT, DescriptorCache::Stage::Vertex, &image_accesses,
	                &color_info, &storage_accesses, pipeline, &constant_upload);
	diagnostic_points[8] = GetDiagnosticTimestamp(diagnostics_enabled);

	BindDescriptors(submit_id, buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout, ps_input_info.bind,
	                pipeline->ps_has_image_load, pipeline->ps_storage_write_authority, VK_SHADER_STAGE_FRAGMENT_BIT,
	                DescriptorCache::Stage::Pixel, &image_accesses,
	                &color_info, &storage_accesses, pipeline, &constant_upload);
	constant_upload.Bind(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout,
	    draw_access ? &draw_access->inventory : nullptr);
	diagnostic_points[9]  = GetDiagnosticTimestamp(diagnostics_enabled);
	diagnostic_points[10] = diagnostic_points[9];

	uint32_t      quad_list_draw_index_count = 0;
	VkIndexType   quad_list_index_type       = VK_INDEX_TYPE_UINT16;
	VulkanBuffer* quad_list_index_buffer     = nullptr;

	if (primitive.mode == AutoIndexPrimitiveMode::QuadList && index_count != 0)
	{
		quad_list_index_buffer = g_render_ctx->GetQuadListIndexBufferCache()->GetBuffer(
		    g_render_ctx->GetGraphicCtx(), index_count, &quad_list_index_type, &quad_list_draw_index_count);
		EXIT_IF(quad_list_index_buffer == nullptr || quad_list_index_buffer->buffer == nullptr || quad_list_draw_index_count == 0);

		if (draw_access) draw_access->inventory.AddRendererObject(quad_list_index_buffer);
		image_accesses.ObserveRenderResource(quad_list_index_buffer->memory.memory);
		vkCmdBindIndexBuffer(vk_buffer, quad_list_index_buffer->buffer, 0, quad_list_index_type);
	}

	if (draw_access) draw_access->Prepare(buffer, submit_id, index_count != 0, color_info, depth_info, pipeline,
	    framebuffer, image_accesses, storage_accesses);

	if (index_count != 0 && image_accesses.sampled_videoout_count != 0)
	{
		GraphicsRenderVideoOutSampledAliasBarrier(buffer->GetBufferOutsideRenderPass(), image_accesses.sampled_videoout,
		                                           image_accesses.sampled_videoout_count, false);
	}
	buffer->BeginRenderPass(framebuffer, &color_info, &depth_info, image_accesses.render_pass_read_only);

	switch (primitive.mode)
	{
		case AutoIndexPrimitiveMode::Direct: vkCmdDraw(vk_buffer, index_count, 1, 0, 0); break;
		case AutoIndexPrimitiveMode::RectList:
			EXIT_NOT_IMPLEMENTED(!(index_count == 3 && vs_input_info.buffers_num == 0));
			vkCmdDraw(vk_buffer, 4, 1, 0, 0);
			break;
		case AutoIndexPrimitiveMode::QuadList:
			if (index_count != 0)
			{
				vkCmdDrawIndexed(vk_buffer, quad_list_draw_index_count, 1, 0, 0, 0);
			}
			break;
	}
	if (draw_access && draw_access->Reserved()) draw_access->CommitShader();
	else RecordShaderResourceCommandAccesses(buffer, index_count, 1, 1, image_accesses, storage_accesses,
	                                   GpuMemoryCommandMutationPolicy::PotentialStorageWrite);

	buffer->EndRenderPass();
	if (index_count != 0 && image_accesses.sampled_videoout_count != 0)
	{
		GraphicsRenderVideoOutSampledAliasBarrier(buffer->GetBufferOutsideRenderPass(), image_accesses.sampled_videoout,
		                                           image_accesses.sampled_videoout_count, true);
	}

	if (draw_access && draw_access->Reserved()) draw_access->CommitAttachments();
	else
	{
		InvalidateMemoryObject(buffer, color_info, index_count != 0);
		InvalidateMemoryObject(buffer, submit_id, depth_info, pipeline, index_count != 0);
	}
	diagnostic_points[11] = GetDiagnosticTimestamp(diagnostics_enabled);
	if (diagnostics_enabled)
	{
		static thread_local DrawTimingStats diagnostic_stats;
		RecordDrawTiming(&diagnostic_stats, "draw_index_auto", diagnostic_points);
	}
}

void GraphicsRenderDispatchDirect(uint64_t submit_id, CommandBuffer* buffer, HW::Context* ctx, HW::Shader* sh_ctx, uint32_t thread_group_x,
                                  uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode,
                                  const GraphicsRenderPreparedInputs* prepared)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());
	const bool inputs_prepared = GraphicsRenderPreparedInputAccess::Consume(prepared, ctx, sh_ctx, true);
	VideoOutRendererAdmissionScope videoout_admission_scope;
	RetainedRendererRecordingScope retained_recording_scope(buffer);
	GfxResourceCaptureEvent capture_event(submit_id, buffer, "dispatch");
	if (g_resource_capture.active)
	{
		g_resource_capture.file.Printf("dispatch groups=%u,%u,%u mode=%u\n", thread_group_x, thread_group_y, thread_group_z, mode);
		g_resource_capture.Shader("cs", sh_ctx->GetCs().cs_regs.data_addr);
		g_resource_capture.Metadata("cs_registers", &sh_ctx->GetCs(), sizeof(sh_ctx->GetCs()));
		g_resource_capture.Metadata("shader_registers", &ctx->GetShaderRegisters(), sizeof(ctx->GetShaderRegisters()));
	}

	if (ShaderIsDisabled(sh_ctx->GetCs().cs_regs.data_addr))
	{
		if (g_resource_capture.active) { g_resource_capture.file.Printf("skip reason=disabled_shader\n"); }
		return;
	}

	EXIT_NOT_IMPLEMENTED(mode != 0);

	const auto& cs_regs = sh_ctx->GetCs();
	const auto& sh_regs = ctx->GetShaderRegisters();

	ShaderComputeInputInfo input_info;
	if (inputs_prepared) GraphicsRenderPreparedInputAccess::CopyCompute(*prepared, &input_info);
	else ShaderGetInputInfoCS(&cs_regs, &sh_regs, &input_info);
	g_resource_capture.Metadata("cs_input", &input_info, sizeof(input_info));

	auto* vk_buffer = buffer->GetBufferOutsideRenderPass();

	auto* pipeline = g_render_ctx->GetPipelineCache()->CreatePipeline(&input_info, &sh_ctx->GetCs(), &ctx->GetShaderRegisters());

	vkCmdBindPipeline(vk_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);

	SetDynamicParams(vk_buffer, pipeline);

	GpuMemoryImageAccessBatch image_accesses;
	GpuMemoryStorageAccessBatch storage_accesses;
	ShaderConstantUpload constant_upload(input_info.bind);
	BindDescriptors(submit_id, buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline_layout, input_info.bind,
	                false, pipeline->cs_storage_write_authority, VK_SHADER_STAGE_COMPUTE_BIT, DescriptorCache::Stage::Compute,
	                &image_accesses, nullptr, &storage_accesses, pipeline, &constant_upload);
	constant_upload.Bind(buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline_layout);

	// Resource creation performed by BindDescriptors may record a private
	// compute utility. Restore the guest pipeline before its dispatch; bound
	// descriptor sets and push constants already belong to the guest layout.
	vkCmdBindPipeline(vk_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);
	EXIT_NOT_IMPLEMENTED(!GpuMemoryValidateUniformDepthZeroStorageDispatch(buffer,
	    image_accesses.accesses, image_accesses.count, storage_accesses.accesses, storage_accesses.count,
	    thread_group_x != 0 && thread_group_y != 0 && thread_group_z != 0));
	vkCmdDispatch(vk_buffer, thread_group_x, thread_group_y, thread_group_z);
	if (thread_group_x != 0 && thread_group_y != 0 && thread_group_z != 0 &&
	    (image_accesses.count != 0 || storage_accesses.count != 0))
	{
		GpuMemoryCommandMutationPolicy mutation_policy = GpuMemoryCommandMutationPolicy::AccessOnly;
		switch (pipeline->cs_image_mutation)
		{
			case ShaderComputeImageMutationClass::UniqueStorageTarget:
				mutation_policy = GpuMemoryCommandMutationPolicy::UniqueComputeStorageWrite;
				break;
			case ShaderComputeImageMutationClass::AdjacentSampledToStorage:
				mutation_policy = GpuMemoryCommandMutationPolicy::AdjacentComputeMipWrite;
				break;
			case ShaderComputeImageMutationClass::None:
				mutation_policy = GpuMemoryCommandMutationPolicy::PotentialStorageWrite;
				break;
		}
		RecordShaderResourceCommandAccesses(buffer, thread_group_x, thread_group_y, thread_group_z,
		                                   image_accesses, storage_accesses, mutation_policy);
	}
	if (pipeline->cs_requires_post_image_store_barrier)
	{
		GraphicsRenderComputeImageStoreBarrier(vk_buffer);
	}
	// This is the last encoding boundary: reconciliation may flush this buffer
	// and begin its successor. It never returns with an unreconciled writer.
	EXIT_NOT_IMPLEMENTED(!GpuMemoryReconcileUniformDepthZeroStorageDispatch(g_render_ctx->GetGraphicCtx(), buffer,
	    storage_accesses.accesses, storage_accesses.count));
}

static LabelDeferredCaptureTicket CaptureNaturalLabelBeforeRenderer(CommandBuffer* buffer, uint64_t address,
                                                                     uint64_t bytes, const LabelGpuObject& description)
{
	LabelDeferredCaptureTicket ticket;
	const char* reason = nullptr;
	if (!LabelCaptureDeferredDestination(buffer, address, bytes, description, &ticket, &reason))
		EXIT("Natural Label initial destination rejected before enrollment: %s\n", reason != nullptr ? reason : "unknown");
	return ticket;
}

void GraphicsRenderWriteAtEndOfPipe32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t value,
                                    const LabelScalarCaptureTicket* scalar_capture)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Graphics::LabelGpuObject label_info(value, nullptr, nullptr);
	const auto capture = scalar_capture != nullptr ? LabelDeferredCaptureTicket {} :
	    CaptureNaturalLabelBeforeRenderer(buffer, reinterpret_cast<uint64_t>(dst_gpu_addr), 4, label_info);
	Core::LockGuard lock(g_render_ctx->GetMutex());

	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 4, label_info));

	LabelSet(buffer, label, scalar_capture, capture.IsCaptured() ? &capture : nullptr);
}

void GraphicsRenderWriteAtEndOfPipeGds32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t dw_offset,
                                         uint32_t dw_num)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	EXIT_NOT_IMPLEMENTED(dw_num == 0 || dw_offset >= GdsBuffer::DW_SIZE ||
	                     dw_num > GdsBuffer::DW_SIZE - dw_offset);

	uint64_t args[LABEL_ARGS_MAX] = {static_cast<uint64_t>(dw_offset), static_cast<uint64_t>(dw_num)};

	Graphics::LabelGpuObject label_info(
	    0,
	    [](const uint64_t* args, void* output, uint64_t output_size)
	    {
		    auto  dw_offset    = static_cast<uint32_t>(args[0]);
		    auto  dw_num       = static_cast<uint32_t>(args[1]);
		    EXIT_IF(output == nullptr || output_size != static_cast<uint64_t>(dw_num) * sizeof(uint32_t));
		    g_render_ctx->GetGdsBuffer()->Read(g_render_ctx->GetGraphicCtx(), static_cast<uint32_t*>(output), dw_offset, dw_num);
		    return true;
	    },
	    nullptr, args, static_cast<uint64_t>(dw_num) * sizeof(uint32_t));

	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr),
	                          static_cast<uint64_t>(dw_num) * sizeof(uint32_t), label_info));

	LabelSet(buffer, label);
}

void GraphicsRenderWriteAtEndOfPipe64(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr, uint64_t value,
                                    const LabelScalarCaptureTicket* scalar_capture)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Graphics::LabelGpuObject label_info(value, nullptr, nullptr);
	const auto capture = scalar_capture != nullptr ? LabelDeferredCaptureTicket {} :
	    CaptureNaturalLabelBeforeRenderer(buffer, reinterpret_cast<uint64_t>(dst_gpu_addr), 8, label_info);
	Core::LockGuard lock(g_render_ctx->GetMutex());

	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 8, label_info));

	LabelSet(buffer, label, scalar_capture, capture.IsCaptured() ? &capture : nullptr);
}

struct FlipInterruptArguments
{
	int handle;
	int index;
	int flip_mode;
	int64_t flip_arg;
	uint32_t event_id;
	CommandProcessor* cp;
};

static std::array<uint64_t, LABEL_ARGS_MAX> PackFlipInterruptArguments(const FlipInterruptArguments& values)
{
	// Pair the 32-bit fields so Label retains a dedicated footprint parameter.
	// All flip-argument bits and the processor pointer remain independent.
	return {(static_cast<uint64_t>(static_cast<uint32_t>(values.handle)) << 32u) | static_cast<uint32_t>(values.index),
	    (static_cast<uint64_t>(values.event_id) << 32u) | static_cast<uint32_t>(values.flip_mode),
	    static_cast<uint64_t>(values.flip_arg), reinterpret_cast<uint64_t>(values.cp)};
}

static FlipInterruptArguments UnpackFlipInterruptArguments(const uint64_t* args)
{
	return {static_cast<int32_t>(static_cast<uint32_t>(args[0] >> 32u)), static_cast<int32_t>(static_cast<uint32_t>(args[0])),
	    static_cast<int32_t>(static_cast<uint32_t>(args[1])), static_cast<int64_t>(args[2]),
	    static_cast<uint32_t>(args[1] >> 32u), reinterpret_cast<CommandProcessor*>(args[3])};
}

template <typename Submit>
static bool SubmitLegacyFlipArguments(const uint64_t* args, Submit&& submit)
{
	int64_t flip_arg = 0;
	static_assert(sizeof(flip_arg) == sizeof(args[3]));
	std::memcpy(&flip_arg, &args[3], sizeof(flip_arg));
	submit(static_cast<int>(args[0]), static_cast<int>(args[1]), static_cast<int>(args[2]), flip_arg);
	return true;
}

bool GraphicsRenderFlipArgumentsContractTest()
{
	const std::array<int64_t, 7> values {0, INT64_C(0x0000000180000001), INT64_C(0x12345678abcdef01),
	    -INT64_C(0x12345678abcdef01), INT64_C(0x0000ffffffffffff), INT64_MIN, INT64_MAX};
	for (const int64_t value: values)
	{
		const std::array<uint64_t, LABEL_ARGS_MAX> args {7, 3, 1, static_cast<uint64_t>(value)};
		const auto original = args;
		unsigned calls = 0;
		bool matches = false;
		const bool completed = SubmitLegacyFlipArguments(args.data(), [&](int handle, int index, int mode, int64_t argument) {
			++calls;
			matches = handle == 7 && index == 3 && mode == 1 && argument == value;
			return 0;
		});
		if (!completed || calls != 1 || !matches || args != original) return false;
	}
	return true;
}

static constexpr uint64_t GPU_CLOCK_PAYLOAD_MASK = UINT64_C(0x0000ffffffffffff);

static uint64_t GpuClockTicks()
{
	// Lumen's virtual GPU clock retains the counter already used by ordinary
	// GPU clock writes. No physical PS4 frequency, epoch or interrupt delay is inferred.
	return LibKernel::KernelReadTsc();
}

static constexpr uint64_t EncodeComputeInterruptPayload(uint64_t ticks, uint32_t event_id)
{
	return ((ticks & GPU_CLOCK_PAYLOAD_MASK) << 16u) | event_id;
}

static constexpr bool IsQualifiedInterruptSource(uint32_t event_id)
{
	return event_id <= 6 || event_id == 64;
}

static void TriggerCompletionInterrupt(uint32_t event_id)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_NOT_IMPLEMENTED(!IsQualifiedInterruptSource(event_id));
	const uint64_t ticks = GpuClockTicks();
	// Graphics EOP retains its existing raw counter payload and event fields.
	g_render_ctx->TriggerEvent(static_cast<int>(event_id), event_id <= 6 ? EncodeComputeInterruptPayload(ticks, event_id) : ticks);
}

static bool TriggerLabelInterrupt(const uint64_t* args, void* /*output*/, uint64_t /*output_size*/)
{
	TriggerCompletionInterrupt(static_cast<uint32_t>(args[1]));
	return true;
}

// Diagnostic source tags; these are categories, not producer identities.
enum class CriticalReadbackSource : uint64_t
{
	LabelClock = 1, LabelInterrupt, LabelValue64, LabelFlip, Range, RenderFull,
};

static LabelGpuObject MakeGpuClockLabel(uint64_t* destination, CommandProcessor* writeback_cp, uint32_t event_id, bool interrupt)
{
	EXIT_IF(destination == nullptr);
	EXIT_NOT_IMPLEMENTED(interrupt && !IsQualifiedInterruptSource(event_id));
	const uint64_t args[LABEL_ARGS_MAX] = {reinterpret_cast<uint64_t>(writeback_cp), event_id};
	return LabelGpuObject(0,
	    +[](const uint64_t* args, void* output, uint64_t output_size)
	    {
		    auto* cp = reinterpret_cast<CommandProcessor*>(args[0]);
		    if (cp != nullptr)
		    {
			    EXIT_IF(g_render_ctx == nullptr);
			    Lumen::CriticalPath::Scope readback(Lumen::CriticalPath::Reason::GpuReadback,
			        static_cast<uint64_t>(CriticalReadbackSource::LabelClock));
			    GpuMemoryWriteBack(g_render_ctx->GetGraphicCtx(), cp, "label-clock");
		    }
		    EXIT_IF(output == nullptr || output_size != sizeof(uint64_t));
		    const uint64_t ticks = GpuClockTicks();
		    std::memcpy(output, &ticks, sizeof(ticks));
		    return true;
	    }, interrupt ? TriggerLabelInterrupt : nullptr, args, 0,
	    writeback_cp == nullptr ? LabelGpuObject::PreparationRole::Ordinary : LabelGpuObject::PreparationRole::LockFreeWriteBack);
}

static LabelGpuObject MakeInterruptLabel(uint64_t value, CommandProcessor* writeback_cp, uint32_t event_id)
{
	EXIT_NOT_IMPLEMENTED(!IsQualifiedInterruptSource(event_id));
	// LabelGpuObject and the worker snapshot both copy these values. Deferred
	// notification never reads the command processor to recover its identity.
	const uint64_t args[LABEL_ARGS_MAX] = {reinterpret_cast<uint64_t>(writeback_cp), event_id};
	const auto writeback = +[](const uint64_t* args, void* /*output*/, uint64_t /*output_size*/)
	{
		EXIT_IF(g_render_ctx == nullptr);
		Lumen::CriticalPath::Scope readback(Lumen::CriticalPath::Reason::GpuReadback,
		    static_cast<uint64_t>(CriticalReadbackSource::LabelInterrupt));
		GpuMemoryWriteBack(g_render_ctx->GetGraphicCtx(), reinterpret_cast<CommandProcessor*>(args[0]), "label-interrupt");
		return true;
	};
	return LabelGpuObject(value, writeback_cp == nullptr ? nullptr : writeback, TriggerLabelInterrupt, args, 0,
	    writeback_cp == nullptr ? LabelGpuObject::PreparationRole::Ordinary : LabelGpuObject::PreparationRole::LockFreeWriteBack);
}

bool GraphicsRenderInterruptCompletionContractTest()
{
	namespace Eq = LibKernel::EventQueue;
	// The virtual GPU counter and its public frequency query share the kernel
	// timebase. Equal consecutive samples are valid at the host's resolution.
	const uint64_t frequency = Gen4::GraphicsGetGpuCoreClockFrequency();
	if (frequency == 0 || frequency != LibKernel::KernelGetTscFrequency()) return false;
	uint64_t previous_ticks = GpuClockTicks();
	for (unsigned sample = 0; sample < 32; ++sample)
	{
		const uint64_t before = LibKernel::KernelReadTsc();
		const uint64_t ticks = GpuClockTicks();
		const uint64_t after = LibKernel::KernelReadTsc();
		if (before < previous_ticks || ticks < before || ticks > after) return false;
		previous_ticks = after;
	}
	enum class Write { Value32, Clock64, ClockOnly };
	struct State
	{
		Eq::KernelEqueue queue = nullptr;
		uint32_t id = 0;
		void* user = nullptr;
		Write write = Write::Value32;
		bool notify = true;
		uint64_t before = 0;
		uint32_t words32[3] = {0x12345678, 0xaabbccdd, 0x87654321};
		uint64_t words64[3] = {UINT64_C(0x12345678abcdef01), UINT64_C(0xfedcba9876543210), UINT64_C(0x87654321abcdef01)};
	};
	static_assert(EncodeComputeInterruptPayload(0, 6) == 6);
	static_assert(EncodeComputeInterruptPayload(GPU_CLOCK_PAYLOAD_MASK, 6) == UINT64_C(0xffffffffffff0006));
	static_assert(EncodeComputeInterruptPayload(UINT64_C(1) << 48u, 2) == 2);
	static_assert(EncodeComputeInterruptPayload(UINT64_MAX, 0) == UINT64_C(0xffffffffffff0000));
	if (!IsQualifiedInterruptSource(0) || !IsQualifiedInterruptSource(6) || !IsQualifiedInterruptSource(64)) return false;
	for (uint32_t id = 7; id < 64; ++id) if (IsQualifiedInterruptSource(id)) return false;
	// Exercise the same packing and decoding used by the deferred flip callback.
	for (const int mode: {-1, 0, 1})
	{
		for (const int64_t argument: {INT64_C(0x12345678abcdef01), INT64_C(-0x12345678abcdef01)})
		{
			const FlipInterruptArguments expected {-7, 3, mode, argument, 64, reinterpret_cast<CommandProcessor*>(uintptr_t {0x1234})};
			const auto args = PackFlipInterruptArguments(expected);
			const auto actual = UnpackFlipInterruptArguments(args.data());
			if (actual.handle != expected.handle || actual.index != expected.index || actual.flip_mode != mode ||
			    actual.flip_arg != argument || actual.event_id != 64 || actual.cp != expected.cp) return false;
		}
	}
	State state;
	if (Eq::KernelCreateEqueue(&state.queue, "graphics-completion-contract") != 0) return false;
	state.user = reinterpret_cast<void*>(uintptr_t {0x1357});
	bool result = true;
	for (int id: {0, 1, 2, 3, 4, 5, 6, 64}) result = GraphicsRenderAddEqEvent(state.queue, id, state.user) == 0 && result;
	const auto observe = +[](void* opaque, bool completed)
	{
		auto& state = *static_cast<State*>(opaque);
		Eq::KernelEvent event {};
		int count = -1;
		const LibKernel::KernelUseconds timeout = 0;
		const int status = Eq::KernelWaitEqueue(state.queue, &event, 1, &count, &timeout);
		const uint64_t after = GpuClockTicks();
		if (after < state.before) return false;
		const uint64_t elapsed = after - state.before;
		if (state.words32[0] != 0x12345678 || state.words32[2] != 0x87654321 ||
		    state.words64[0] != UINT64_C(0x12345678abcdef01) || state.words64[2] != UINT64_C(0x87654321abcdef01)) return false;
		if (!completed)
			return status == LibKernel::KERNEL_ERROR_ETIMEDOUT && count == 0 && state.words32[1] == 0xaabbccdd &&
			    state.words64[1] == UINT64_C(0xfedcba9876543210);
		uint64_t written_offset = 0;
		if (state.write == Write::Value32)
		{
			if (state.words32[1] != 0x11223344 || state.words64[1] != UINT64_C(0xfedcba9876543210)) return false;
		} else
		{
			// Guest clock writes retain all 64 bits; only compute event payloads
			// use the separate 48-bit packing below.
			if (state.words32[1] != 0xaabbccdd || state.words64[1] < state.before || state.words64[1] > after) return false;
			written_offset = state.words64[1] - state.before;
		}
		if (!state.notify) return status == LibKernel::KERNEL_ERROR_ETIMEDOUT && count == 0;
		if (status != 0 || count != 1 || event.ident != state.id || event.filter != Eq::KERNEL_EVFILT_GRAPHICS ||
		    event.udata != state.user) return false;
		const auto payload = static_cast<uint64_t>(event.data);
		uint64_t event_ticks = payload;
		if (state.id <= 6)
		{
			if (event.flags != 0x20 || event.fflags != 0 || (payload & 0xffffu) != state.id) return false;
			event_ticks = payload >> 16u;
		} else
		{
			if (event.flags != 1 || event.fflags != 1 || event_ticks < state.before || event_ticks > after) return false;
			return written_offset <= event_ticks - state.before; // Existing full-width graphics EOP profile.
		}
		const auto event_offset = (event_ticks - (state.before & GPU_CLOCK_PAYLOAD_MASK)) & GPU_CLOCK_PAYLOAD_MASK;
		return written_offset <= event_offset && event_offset <= elapsed;
	};
	const auto empty = [&]()
	{
		Eq::KernelEvent event {};
		int count = -1;
		const LibKernel::KernelUseconds timeout = 0;
		return Eq::KernelWaitEqueue(state.queue, &event, 1, &count, &timeout) == LibKernel::KERNEL_ERROR_ETIMEDOUT && count == 0;
	};
	const auto complete = [&](Write write)
	{
		state.write = write;
		state.words32[1] = 0xaabbccdd;
		state.words64[1] = UINT64_C(0xfedcba9876543210);
		state.before = GpuClockTicks();
		if (write == Write::Value32)
		{
			const auto description = MakeInterruptLabel(0x11223344, nullptr, state.id);
			const auto different = MakeInterruptLabel(0x11223344, nullptr, state.id == 64 ? 1 : 64);
			return !description.Equal(different.params) &&
			    LabelCompletionContractTest(description, &state.words32[1], observe, &state) && empty();
		}
		const auto description = MakeGpuClockLabel(&state.words64[1], nullptr, state.id, write != Write::ClockOnly);
		return LabelCompletionContractTest64(description, &state.words64[1], observe, &state) && empty();
	};
	for (const uint32_t id: {64u, 0u, 1u, 2u, 3u, 4u, 5u, 6u})
	{
		state.id = id;
		for (unsigned repeat = 0; repeat < 2; ++repeat)
		{
			state.notify = false;
			result = complete(Write::ClockOnly) && result;
			state.notify = true;
			result = complete(Write::Value32) && complete(Write::Clock64) && result;
		}
	}
	// Reset removes all registrations without resetting the shared monotonic clock.
	const uint64_t before_reset = GpuClockTicks();
	result = GraphicsRenderResetForNextSession() && result;
	state.id = 1;
	state.notify = false;
	result = complete(Write::Clock64) && result;
	state.user = reinterpret_cast<void*>(uintptr_t {0x2468});
	result = GraphicsRenderAddEqEvent(state.queue, 2, state.user) == 0 && result;
	result = complete(Write::Clock64) && result; // id1 must not notify newly registered id2.
	state.id = 2;
	state.notify = true;
	result = complete(Write::Clock64) && GpuClockTicks() >= before_reset &&
	    Gen4::GraphicsGetGpuCoreClockFrequency() == frequency && LibKernel::KernelGetTscFrequency() == frequency && result;
	return Eq::KernelDeleteEqueue(state.queue) == 0 && result;
}

void GraphicsRenderQueueEndOfPipeInterrupt(CommandBuffer* buffer, uint32_t event_id)
{
	EXIT_NOT_IMPLEMENTED(!IsQualifiedInterruptSource(event_id));
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	const uint64_t args[LABEL_ARGS_MAX] = {0, event_id};
	auto* label = LabelCreate32(g_render_ctx->GetGraphicCtx(), nullptr, 0, nullptr, TriggerLabelInterrupt, args);

	// Deletion only marks this active label for retirement. The worker observes
	// the BOTTOM_OF_PIPE event before firing the callback; retirement may delay
	// delivery, but the interrupt is never emitted while parsing the packet.
	LabelSet(buffer, label);
	LabelDelete(label);
}

void GraphicsRenderWriteAtEndOfPipeClockCounter(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	const auto label_info = MakeGpuClockLabel(dst_gpu_addr, nullptr, 64, false);
	const auto capture = CaptureNaturalLabelBeforeRenderer(buffer, reinterpret_cast<uint64_t>(dst_gpu_addr), 8, label_info);
	Core::LockGuard lock(g_render_ctx->GetMutex());

	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 8, label_info));

	LabelSet(buffer, label, nullptr, capture.IsCaptured() ? &capture : nullptr);
}

void GraphicsRenderWriteAtEndOfPipeClockCounterWithInterruptWriteBack(uint64_t submit_id, CommandBuffer* buffer,
                                                                   uint64_t* dst_gpu_addr, uint32_t event_id)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr || buffer->IsInvalid());
	const auto label_info = MakeGpuClockLabel(dst_gpu_addr, buffer->GetParent(), event_id, true);
	const auto capture = CaptureNaturalLabelBeforeRenderer(buffer, reinterpret_cast<uint64_t>(dst_gpu_addr), 8, label_info);
	Core::LockGuard lock(g_render_ctx->GetMutex());
	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 8, label_info));
	LabelSet(buffer, label, nullptr, capture.IsCaptured() ? &capture : nullptr);
}

void GraphicsRenderWriteAtEndOfPipeWithWriteBack64(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr, uint64_t value)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	uint64_t args[LABEL_ARGS_MAX] = {reinterpret_cast<uint64_t>(buffer->GetParent())};

	Graphics::LabelGpuObject label_info(
	    value,
	    [](const uint64_t* args, void* /*output*/, uint64_t /*output_size*/)
	    {
		    EXIT_IF(g_render_ctx == nullptr);

		    auto* cp = reinterpret_cast<CommandProcessor*>(args[0]);

		    Lumen::CriticalPath::Scope readback(Lumen::CriticalPath::Reason::GpuReadback,
		        static_cast<uint64_t>(CriticalReadbackSource::LabelValue64));
		    GpuMemoryWriteBack(g_render_ctx->GetGraphicCtx(), cp, "label-value64");
		    return true;
	    },
	    nullptr, args, 0, LabelGpuObject::PreparationRole::LockFreeWriteBack);
	const auto capture = CaptureNaturalLabelBeforeRenderer(buffer, reinterpret_cast<uint64_t>(dst_gpu_addr), 8, label_info);
	Core::LockGuard lock(g_render_ctx->GetMutex());

	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 8, label_info));

	LabelSet(buffer, label, nullptr, capture.IsCaptured() ? &capture : nullptr);
}

void GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBack64(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr,
                                                            uint64_t value, uint32_t event_id)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	const auto label_info = MakeInterruptLabel(value, buffer->GetParent(), event_id);
	const auto capture = CaptureNaturalLabelBeforeRenderer(buffer, reinterpret_cast<uint64_t>(dst_gpu_addr), 8, label_info);
	Core::LockGuard lock(g_render_ctx->GetMutex());

	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 8, label_info));

	LabelSet(buffer, label, nullptr, capture.IsCaptured() ? &capture : nullptr);
}

void GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBack32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr,
                                                            uint32_t value, uint32_t event_id)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	const auto label_info = MakeInterruptLabel(value, buffer->GetParent(), event_id);
	const auto capture = CaptureNaturalLabelBeforeRenderer(buffer, reinterpret_cast<uint64_t>(dst_gpu_addr), 4, label_info);
	Core::LockGuard lock(g_render_ctx->GetMutex());
	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 4, label_info));
	LabelSet(buffer, label, nullptr, capture.IsCaptured() ? &capture : nullptr);
}

void GraphicsRenderWriteAtEndOfPipeWithInterrupt64(uint64_t submit_id, CommandBuffer* buffer, uint64_t* dst_gpu_addr, uint64_t value, uint32_t event_id)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	const auto label_info = MakeInterruptLabel(value, nullptr, event_id);
	const auto capture = CaptureNaturalLabelBeforeRenderer(buffer, reinterpret_cast<uint64_t>(dst_gpu_addr), 8, label_info);
	Core::LockGuard lock(g_render_ctx->GetMutex());

	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 8, label_info));

	LabelSet(buffer, label, nullptr, capture.IsCaptured() ? &capture : nullptr);
}

void GraphicsRenderWriteAtEndOfPipeWithInterrupt32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t value, uint32_t event_id)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	const auto label_info = MakeInterruptLabel(value, nullptr, event_id);
	const auto capture = CaptureNaturalLabelBeforeRenderer(buffer, reinterpret_cast<uint64_t>(dst_gpu_addr), 4, label_info);
	Core::LockGuard lock(g_render_ctx->GetMutex());

	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 4, label_info));

	LabelSet(buffer, label, nullptr, capture.IsCaptured() ? &capture : nullptr);
}

void GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBackFlip32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr,
                                                                uint32_t value, int handle, int index, int flip_mode, int64_t flip_arg, uint32_t event_id)
{
	(void)VideoOut::VideoOutPrepareEventPayload(static_cast<uint64_t>(flip_arg));
	EXIT_NOT_IMPLEMENTED(!IsQualifiedInterruptSource(event_id));
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	const auto args = PackFlipInterruptArguments({handle, index, flip_mode, flip_arg, event_id, buffer->GetParent()});

	Graphics::LabelGpuObject label_info(
	    value,
	    [](const uint64_t* args, void* /*output*/, uint64_t /*output_size*/)
	    {
		    EXIT_IF(g_render_ctx == nullptr);

		    const auto values = UnpackFlipInterruptArguments(args);
		    Lumen::CriticalPath::Scope readback(Lumen::CriticalPath::Reason::GpuReadback,
		        static_cast<uint64_t>(CriticalReadbackSource::LabelFlip));
		    GpuMemoryWriteBack(g_render_ctx->GetGraphicCtx(), values.cp, "label-flip");
		    readback.End();
		    VideoOut::VideoOutSubmitFlip(values.handle, values.index, values.flip_mode, values.flip_arg);
		    return true;
	    },
	    [](const uint64_t* args, void* /*output*/, uint64_t /*output_size*/)
	    {
		    EXIT_IF(g_render_ctx == nullptr);
		    TriggerCompletionInterrupt(UnpackFlipInterruptArguments(args).event_id);
		    return true;
	    },
	    args.data(), 0, LabelGpuObject::PreparationRole::LockFreeWriteBack);
	const auto capture = CaptureNaturalLabelBeforeRenderer(buffer, reinterpret_cast<uint64_t>(dst_gpu_addr), 4, label_info);
	Core::LockGuard lock(g_render_ctx->GetMutex());

	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 4, label_info));

	LabelSet(buffer, label, nullptr, capture.IsCaptured() ? &capture : nullptr);
}

void GraphicsRenderWriteAtEndOfPipeWithFlip32(uint64_t submit_id, CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t value, int handle,
                                              int index, int flip_mode, int64_t flip_arg)
{
	(void)VideoOut::VideoOutPrepareEventPayload(static_cast<uint64_t>(flip_arg));
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	uint64_t args[LABEL_ARGS_MAX] = {static_cast<uint64_t>(handle), static_cast<uint64_t>(index), static_cast<uint64_t>(flip_mode),
	                                 static_cast<uint64_t>(flip_arg)};

	Graphics::LabelGpuObject label_info(
	    value,
	    [](const uint64_t* args, void* /*output*/, uint64_t /*output_size*/)
	    {
		    return SubmitLegacyFlipArguments(args, VideoOut::VideoOutSubmitFlip);
	    },
	    nullptr, args);
	const auto capture = CaptureNaturalLabelBeforeRenderer(buffer, reinterpret_cast<uint64_t>(dst_gpu_addr), 4, label_info);
	Core::LockGuard lock(g_render_ctx->GetMutex());

	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(submit_id, g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 4, label_info));

	LabelSet(buffer, label, nullptr, capture.IsCaptured() ? &capture : nullptr);
}

void GraphicsRenderWriteAtEndOfPipeOnlyFlip(uint64_t /*submit_id*/, CommandBuffer* buffer, int handle, int index, int flip_mode,
                                            int64_t flip_arg)
{
	(void)VideoOut::VideoOutPrepareEventPayload(static_cast<uint64_t>(flip_arg));
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	uint64_t args[LABEL_ARGS_MAX] = {static_cast<uint64_t>(handle), static_cast<uint64_t>(index), static_cast<uint64_t>(flip_mode),
	                                 static_cast<uint64_t>(flip_arg)};

	auto* label = LabelCreate32(
	    g_render_ctx->GetGraphicCtx(), nullptr, 0,
	    [](const uint64_t* args, void* /*output*/, uint64_t /*output_size*/)
	    {
		    return SubmitLegacyFlipArguments(args, VideoOut::VideoOutSubmitFlip);
	    },
	    nullptr, args);

	LabelSet(buffer, label);

	LabelDelete(label);
}

struct RenderFullCpuWitness
{
	CommandProcessor* cp = nullptr;
	uint32_t depth = 0;
	bool exclusive = false;
};
static thread_local RenderFullCpuWitness g_render_full_cpu_witness;

template <typename UnlockProcessor, typename LockProcessor, typename Backoff>
static void WithRenderFullCpuOuterLocksReleased(Core::Mutex& renderer, UnlockProcessor unlock_processor,
    LockProcessor lock_processor, Backoff backoff)
{
	struct Restore
	{
		Core::Mutex& renderer;
		LockProcessor& lock_processor;
		~Restore() { lock_processor(); lock_processor(); renderer.Lock(); }
	};
	renderer.Unlock();
	unlock_processor();
	unlock_processor();
	Restore restore {renderer, lock_processor};
	backoff();
}

bool GraphicsRenderRenderFullCpuScopeValid(CommandProcessor* cp)
{
	const auto& witness = g_render_full_cpu_witness;
	return cp != nullptr && witness.cp == cp && witness.depth == 1 && witness.exclusive;
}

bool GraphicsRenderRenderFullCpuPause(CommandProcessor* cp, CommandBuffer* buffer)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	if (g_render_ctx == nullptr || !GraphicsRenderRenderFullCpuScopeValid(cp) ||
	    Core::VirtualMemory::CurrentThreadHasMemoryLease() ||
	    !GraphicsRunCommandProcessorExclusiveRecordingValid(cp, buffer)) return false;
	WithRenderFullCpuOuterLocksReleased(g_render_ctx->GetMutex(),
	    [cp] { GraphicsRunCommandProcessorUnlock(cp); }, [cp] { GraphicsRunCommandProcessorLock(cp); },
	    [] { Core::Thread::SleepMicro(1000); });
	return true;
#else
	(void)cp; (void)buffer;
	return false;
#endif
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool RenderFullCpuOuterLocksContractTest()
{
	auto* cp = reinterpret_cast<CommandProcessor*>(uintptr_t {0x1000});
	auto* foreign = reinterpret_cast<CommandProcessor*>(uintptr_t {0x2000});
	struct RestoreWitness
	{
		RenderFullCpuWitness previous = g_render_full_cpu_witness;
		~RestoreWitness() { g_render_full_cpu_witness = previous; }
	} restore;
	for (const uint32_t depth: {0u, 1u, 2u})
	for (const bool exclusive: {false, true})
	{
		g_render_full_cpu_witness = {cp, depth, exclusive};
		if (GraphicsRenderRenderFullCpuScopeValid(cp) != (depth == 1 && exclusive) ||
		    GraphicsRenderRenderFullCpuScopeValid(nullptr) || GraphicsRenderRenderFullCpuScopeValid(foreign)) return false;
	}
	Core::Mutex processor, renderer, run;
	bool valid = true;
	{
		Core::LockGuard run_lock(run), cp1(processor), cp2(processor), render1(renderer);
		const auto observe = [&](bool released) {
			std::thread peer([&] {
				const bool p = processor.TryLock(), r = renderer.TryLock(), stream = run.TryLock();
				valid = valid && p == released && r == released && !stream;
				if (p) processor.Unlock(); if (r) renderer.Unlock(); if (stream) run.Unlock();
			});
			peer.join();
		};
		observe(false);
		WithRenderFullCpuOuterLocksReleased(renderer, [&] { processor.Unlock(); }, [&] { processor.Lock(); },
		    [&] { observe(true); });
		observe(false);
	}
	std::fprintf(stderr, "[GFX_RENDER_FULL_DEPTH_PAUSE_SCOPE_TEST] private-owner1/exclusive=required"
	                     " outside/nested/foreign/null=reject CP2/renderer1=fully-released/restored RunLock=retained"
	                     " witness-identities=modeled mutexes=real\n");
	return valid;
}
#endif

GpuMemoryRenderFullWriteBackResult GraphicsRenderTryWriteBackRenderFull(CommandProcessor* cp,
    GpuMemoryRenderFullPreflightTicket* ticket, bool exclusive_stream)
{
	Lumen::CriticalPath::Scope readback(Lumen::CriticalPath::Reason::GpuReadback,
	    static_cast<uint64_t>(CriticalReadbackSource::RenderFull));
	EXIT_IF(g_render_ctx == nullptr);
	struct Scope
	{
		RenderFullCpuWitness previous = g_render_full_cpu_witness;
		~Scope() { g_render_full_cpu_witness = previous; }
	} scope;
	g_render_full_cpu_witness = {cp, scope.previous.depth + 1u, exclusive_stream};
	Core::LockGuard lock(g_render_ctx->GetMutex());
	return GpuMemoryTryWriteBackRenderFull(g_render_ctx->GetGraphicCtx(), cp, ticket, exclusive_stream);
}

void GraphicsRenderWriteBack(CommandProcessor* cp, uint64_t vaddr, uint64_t size)
{
	Lumen::CriticalPath::Scope readback(Lumen::CriticalPath::Reason::GpuReadback,
	    static_cast<uint64_t>(CriticalReadbackSource::Range));
	Core::LockGuard lock(g_render_ctx->GetMutex());

	EXIT_IF(g_render_ctx == nullptr);

	GpuMemoryWriteBack(g_render_ctx->GetGraphicCtx(), cp, vaddr, size);
}

Core::Mutex* GraphicsRenderMemoryMutex() noexcept
{
	return g_render_ctx == nullptr ? nullptr : &g_render_ctx->GetMutex();
}

bool GraphicsRenderPrepareCpuMemoryAccess(uint64_t address, uint64_t bytes, GuestCpu::MemoryAccessIntent intent) noexcept
{
	if (!GuestCpu::WaitForMemoryHandoffNativeClaim(address, bytes)) return false;
	if (g_render_ctx == nullptr || !GuestCpu::NeedsHandoff(address, bytes)) return true;
	// Admission remains live through the caller's actual load, so no new GPU
	// writer can appear between this conservative clean proof and that access.
	if (intent == GuestCpu::MemoryAccessIntent::Read && !GpuMemoryHasPendingGpuDirty()) return true;
	GuestCpu::MemoryHandoffBypass bypass;
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && defined(LUMEN_ANDROID_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME))
	// A counted CPU reader excludes uploads and resource enrollment through
	// the actual access. Recheck the physical candidates under the tracker;
	// repeated stores whose upload is already revoked need no renderer work.
	// Native workers without this admission retain the renderer transaction.
	static thread_local GuestCpu::MemoryHandoffPageCache page_cache;
	const uint64_t execution_epoch = LumenAndroidFexCoreGuestExecutionReadAdmissionEpochV1();
	if (page_cache.Try(execution_epoch, address, bytes, intent, GpuMemoryCpuMemoryAccessNeedsNoRenderer)) return true;
	// A first store to a clean private snapshot changes only the tracker. Keep
	// the counted execution reader through the store, excluding GPU uploads and
	// every VM mutation while the complete physical alias cohort is revoked.
	if (execution_epoch != 0 && GuestCpu::WritesMemory(intent) &&
	    GpuMemoryPrepareCleanCpuWriteAccess(address, bytes, intent)) return true;
	if (LumenAndroidFexCoreOwnsGuestMemoryReadLeaseV1() == 1 &&
	    GpuMemoryCpuMemoryAccessNeedsNoRenderer(address, bytes, intent)) return true;
#endif
	Core::LockGuard lock(g_render_ctx->GetMutex());
	return GpuMemoryPrepareCpuMemoryAccess(g_render_ctx->GetGraphicCtx(), address, bytes, intent);
}

bool GraphicsRenderCopyDmaData(CommandProcessor* cp, uint64_t source_address, uint64_t destination_address,
	                           uint32_t byte_count)
{
	if (g_render_ctx == nullptr || cp == nullptr || source_address == 0 || destination_address == 0 || byte_count == 0)
	{
		return false;
	}

	// Keep destination preservation, source publication, the guest memcpy and
	// the tracker flush in one renderer transaction. No other CP can recreate or
	// bind a detached snapshot between those steps.
	Core::LockGuard lock(g_render_ctx->GetMutex());
	auto*           ctx = g_render_ctx->GetGraphicCtx();
	if (ctx == nullptr ||
	    !GpuMemoryWriteBackForCpuCopy(ctx, cp, destination_address, byte_count, GpuMemoryCpuCopyRole::Destination) ||
	    !GpuMemoryWriteBackForCpuCopy(ctx, cp, source_address, byte_count, GpuMemoryCpuCopyRole::Source))
	{
		return false;
	}

	CommandBuffer* buffer = GraphicsRunCommandProcessorGetCurrentBuffer(cp);
	if (buffer == nullptr || buffer->IsInvalid())
	{
		return false;
	}
	std::memcpy(reinterpret_cast<void*>(destination_address), reinterpret_cast<const void*>(source_address), byte_count);
	GpuMemoryFlush(ctx, buffer, destination_address, byte_count);
	return true;
}

static void graphics_event_reset_func(LibKernel::EventQueue::KernelEqueueEvent* event)
{
	EXIT_IF(event == nullptr);
	event->triggered    = false;
	event->event.fflags = 0;
	event->event.data   = 0;
}

static void graphics_event_delete_func(LibKernel::EventQueue::KernelEqueue eq, LibKernel::EventQueue::KernelEqueueEvent* event)
{
	EXIT_IF(event == nullptr);
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_NOT_IMPLEMENTED(event->event.filter != LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS);
	g_render_ctx->DeleteEqEvent(eq, static_cast<int>(event->event.ident));
}

static void graphics_event_trigger_func(LibKernel::EventQueue::KernelEqueueEvent* event, void* trigger_data)
{
	EXIT_IF(event == nullptr);
	event->triggered = true;
	if (event->event.ident <= 6)
	{
		event->event.flags = 0x20;
		event->event.fflags = 0;
	} else
	{
		event->event.fflags++;
	}
	event->event.data = reinterpret_cast<intptr_t>(trigger_data);
}

int GraphicsRenderAddEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id, void* udata)
{
	if (eq == nullptr) return LibKernel::KERNEL_ERROR_EBADF;
	if (id == 65) return LibKernel::KERNEL_ERROR_EPERM;
	if (id == -1 || id == std::numeric_limits<int>::max()) return LibKernel::KERNEL_ERROR_EINVAL;
	if (id < 0 || id > 64)
	{
		EXIT("Unqualified graphics event registration: id=0x%08x", static_cast<unsigned>(id));
	}
	EXIT_IF(g_render_ctx == nullptr);

	LibKernel::EventQueue::KernelEqueueIdentity identity;
	if (!LibKernel::EventQueue::GetEqueueIdentity(eq, &identity)) return LibKernel::KERNEL_ERROR_EBADF;

	LibKernel::EventQueue::KernelEqueueEvent event {};
	event.triggered                = false;
	event.event.ident              = static_cast<uintptr_t>(static_cast<intptr_t>(id));
	event.event.filter             = LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS;
	event.event.flags              = 1; // EV_ADD, established by the native wrapper.
	event.event.udata              = udata;
	event.event.fflags             = 0;
	event.event.data               = id;
	event.filter.delete_event_func = graphics_event_delete_func;
	event.filter.reset_func        = graphics_event_reset_func;
	event.filter.trigger_func      = graphics_event_trigger_func;
	event.filter.data              = nullptr;

	return LibKernel::EventQueue::AddEventIfRegistered(identity, event,
	    [](const auto& registered_identity, const auto& registered_event) {
		    g_render_ctx->AddEqEvent(registered_identity, static_cast<int>(registered_event.event.ident));
	    });
}

int GraphicsRenderDeleteEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id)
{
	if (eq == nullptr) return LibKernel::KERNEL_ERROR_EBADF;
	if (id < 0 || id > 64)
	{
		EXIT("Unqualified graphics event deletion: id=0x%08x", static_cast<unsigned>(id));
	}
	EXIT_IF(g_render_ctx == nullptr);

	LibKernel::EventQueue::KernelEqueueIdentity identity;
	if (!LibKernel::EventQueue::GetEqueueIdentity(eq, &identity)) return LibKernel::KERNEL_ERROR_EBADF;
	return LibKernel::EventQueue::DeleteEventIfRegistered(identity, static_cast<uintptr_t>(id),
	    LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS);
}

void GraphicsRenderTriggerEvent(int id, uint64_t data)
{
	EXIT_IF(g_render_ctx == nullptr);
	g_render_ctx->TriggerEvent(id, data);
}

void GraphicsRenderTriggerEopEvent()
{
	EXIT_IF(g_render_ctx == nullptr);
	g_render_ctx->TriggerEopEvent();
}

void GraphicsRenderClearGds(uint64_t dw_offset, uint32_t dw_num, uint32_t clear_value)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(g_render_ctx->GetGdsBuffer() == nullptr);

	g_render_ctx->GetGdsBuffer()->Clear(g_render_ctx->GetGraphicCtx(), dw_offset, dw_num, clear_value);
}

bool GraphicsRenderGdsReadRangeIsValid(uint64_t destination_address, uint64_t source_byte_offset, uint32_t byte_count)
{
	constexpr uint64_t capacity = GdsBuffer::DW_SIZE * sizeof(uint32_t);
	constexpr uint64_t pointer_max = std::numeric_limits<uintptr_t>::max();
	return destination_address != 0 && destination_address <= pointer_max &&
	       destination_address % sizeof(uint32_t) == 0 && source_byte_offset % sizeof(uint32_t) == 0 &&
	       byte_count % sizeof(uint32_t) == 0 && source_byte_offset < capacity &&
	       byte_count <= capacity - source_byte_offset && byte_count <= pointer_max - destination_address;
}

template <typename Read>
static bool StageAndPublishGdsRead(uint64_t destination_address, uint64_t source_byte_offset, uint32_t byte_count, Read&& read)
{
	if (!GraphicsRenderGdsReadRangeIsValid(destination_address, source_byte_offset, byte_count)) return false;
	if (byte_count == 0) return true; // Preserve the existing empty-copy behavior without touching the destination.
	GuestWriteDestination destination;
	if (!destination.Capture(destination_address, byte_count, sizeof(uint32_t))) return false;
	// Capture at CP consumption before GDS locking/map/read/unmap, not at HLE
	// submission. Unlike Label, this operation has no retained worker snapshot.
	// Its bounded staging and short publication lease do not add a GPU fence.
	std::vector<uint32_t> staging(byte_count / sizeof(uint32_t));
	if (!read(staging.data(), static_cast<uint32_t>(source_byte_offset / sizeof(uint32_t)),
	          static_cast<uint32_t>(staging.size()))) return false;
	return destination.Publish(staging.data());
}

bool GraphicsRenderReadGds(uint64_t destination_address, uint64_t source_byte_offset, uint32_t byte_count)
{
	if (g_render_ctx == nullptr || g_render_ctx->GetGdsBuffer() == nullptr) return false;
	return StageAndPublishGdsRead(destination_address, source_byte_offset, byte_count,
	    [](uint32_t* output, uint32_t offset, uint32_t count)
	    {
		    g_render_ctx->GetGdsBuffer()->Read(g_render_ctx->GetGraphicCtx(), output, offset, count);
		    return true;
	    });
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool GraphicsRenderGdsReadDestinationContractTest()
{
	using namespace Core::VirtualMemory;
	struct Allocation
	{
		Allocation()
		{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
			Init();
			address = Alloc(0, 65536, Mode::ReadWrite);
#else
			address = reinterpret_cast<uint64_t>(native.data());
#endif
		}
		~Allocation()
		{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
			if (address != 0) Free(address);
#endif
		}
		uint64_t address = 0;
#if LUMEN_PLATFORM != LUMEN_PLATFORM_OSX
		std::array<uint32_t, 16384> native {};
#endif
	} owned;
	if (owned.address == 0) return false;
	GuestWriteDestination ticket;
	uint64_t payload = 0x123456789abcdef0u;
	if (ticket.Validate() || ticket.Publish(&payload) || ticket.Capture(owned.address, 8, 0) ||
	    ticket.Capture(owned.address, 0) || !ticket.Capture(owned.address, 8, 4) || !ticket.Validate() ||
	    ticket.Address() != owned.address || ticket.Size() != 8 || ticket.Publish(nullptr) ||
	    ticket.Capture(UINT64_MAX - 3, 8, 4) || ticket.Validate() || ticket.Publish(&payload)) return false;
	const auto pattern = [](uint32_t index) { return (index * 0x31415927u) ^ 0x13579bdfu; };
	uint32_t prepares = 0;
	bool private_staging = true;
	auto fill = [&](uint32_t* output, uint32_t offset, uint32_t count)
	{
		++prepares;
		private_staging = private_staging && (reinterpret_cast<uint64_t>(output) < owned.address ||
		                                      reinterpret_cast<uint64_t>(output) >= owned.address + 65536);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		private_staging = private_staging && !CurrentThreadHasMemoryLease();
#endif
		for (uint32_t i = 0; i != count; ++i) output[i] = pattern(offset + i);
		return true;
	};
	const auto reset = [&]() { std::memset(reinterpret_cast<void*>(owned.address), 0xa5, 65536); prepares = 0; };
	const auto intact = [&](uint8_t expected)
	{
		const auto* bytes = reinterpret_cast<const uint8_t*>(owned.address);
		for (uint32_t i = 0; i != 65536; ++i) if (bytes[i] != expected) return false;
		return true;
	};
	for (uint32_t size: {4u, 8u, 64u, 49152u})
	{
		reset();
		const uint32_t destination_offset = size == 49152 ? 0 : 16380;
		const uint32_t source_offset = 49152 - size;
		if (!StageAndPublishGdsRead(owned.address + destination_offset, source_offset, size, fill) ||
		    prepares != 1 || !private_staging) return false;
		const auto* words = reinterpret_cast<const uint32_t*>(owned.address);
		for (uint32_t i = 0; i != 16384; ++i)
		{
			const bool inside = i >= destination_offset / 4 && i < (destination_offset + size) / 4;
			if (words[i] != (inside ? pattern((source_offset - destination_offset) / 4 + i) : 0xa5a5a5a5u)) return false;
		}
	}
	reset();
	if (!StageAndPublishGdsRead(owned.address, 0, 0, fill) || prepares != 0 || !intact(0xa5)) return false;
	if (StageAndPublishGdsRead(owned.address, 0, 8, [&](uint32_t* output, uint32_t offset, uint32_t count)
	    { fill(output, offset, count); return false; }) || prepares != 1 || !intact(0xa5)) return false;
	reset();
	for (uint64_t source: {UINT64_C(1), UINT64_C(49148), UINT64_C(49152), UINT64_C(0x400000000), UINT64_MAX - 3})
		if (StageAndPublishGdsRead(owned.address, source, 8, fill) || prepares != 0 || !intact(0xa5)) return false;
	for (uint64_t destination: {UINT64_C(0), owned.address + 1, owned.address + 2, owned.address + 3, UINT64_MAX - 3})
		if (StageAndPublishGdsRead(destination, 0, 8, fill) || prepares != 0 || !intact(0xa5)) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	for (Mode mode: {Mode::Read, Mode::ExecuteReadWrite})
	{
		if (!Protect(owned.address, 65536, mode) || StageAndPublishGdsRead(owned.address, 0, 8, fill) ||
		    prepares != 0 || !intact(0xa5) || !Protect(owned.address, 65536, Mode::ReadWrite)) return false;
	}
	uint64_t unmanaged = 0;
	if (StageAndPublishGdsRead(reinterpret_cast<uint64_t>(&unmanaged), 0, 8, fill) || prepares != 0 || unmanaged != 0 ||
	    StageAndPublishGdsRead(owned.address + 65532, 0, 8, fill) || prepares != 0) return false;
	if (!Protect(owned.address + 16384, 16384, Mode::Read) ||
	    StageAndPublishGdsRead(owned.address + 16380, 0, 8, fill) || prepares != 0 ||
	    !Protect(owned.address + 16384, 16384, Mode::ReadWrite)) return false;
	for (int mutation: {0, 1, 2, 3})
	{
		reset();
		bool mutated = false;
		bool no_early_store = true;
		const bool copied = StageAndPublishGdsRead(owned.address + 16380, 0, 64,
		    [&](uint32_t* output, uint32_t offset, uint32_t count)
		    {
			    fill(output, offset, count);
			    no_early_store = intact(0xa5);
			    if (mutation == 0) mutated = Protect(owned.address, 65536, Mode::Read);
			    if (mutation == 1)
			    {
				    mutated = Free(owned.address) && AllocFixed(owned.address, 65536, Mode::ReadWrite);
				    if (mutated) std::memset(reinterpret_cast<void*>(owned.address), 0x97, 65536);
			    }
			    if (mutation == 2)
			    {
				    const uint64_t other = Alloc(0, 65536, Mode::ReadWrite);
				    mutated = other != 0 && Free(other);
			    }
			    if (mutation == 3)
				    mutated = Protect(owned.address, 65536, Mode::Read) && Protect(owned.address, 65536, Mode::ReadWrite);
			    return mutated;
		    });
		if (!mutated || !private_staging || !no_early_store || prepares != 1 || copied != (mutation == 2)) return false;
		if (mutation == 2)
		{
			// An unrelated allocation does not renew or revoke the destination's
			// original token. Publish the staged words once, preserving both guards.
			const auto* words = reinterpret_cast<const uint32_t*>(owned.address);
			for (uint32_t i = 0; i < 16384; ++i)
				if (words[i] != (i >= 4095 && i < 4111 ? pattern(i - 4095) : 0xa5a5a5a5u)) return false;
		} else if (!intact(mutation == 1 ? 0x97 : 0xa5)) return false;
		if (mutation == 0 && !Protect(owned.address, 65536, Mode::ReadWrite)) return false;
	}
	// Exercise the production borrower across two independently owned adjacent
	// mappings, including a rejected second region without a partial write.
	if (!Free(owned.address) || !AllocFixed(owned.address, 32768, Mode::ReadWrite)) return false;
	const uint64_t second = owned.address + 32768;
	if (!AllocFixed(second, 32768, Mode::ReadWrite)) return false;
	reset();
	bool adjacent_ok = StageAndPublishGdsRead(second - 4, 0, 8, fill) && prepares == 1 &&
	    *reinterpret_cast<uint32_t*>(second - 8) == 0xa5a5a5a5u &&
	    *reinterpret_cast<uint32_t*>(second - 4) == pattern(0) && *reinterpret_cast<uint32_t*>(second) == pattern(1) &&
	    *reinterpret_cast<uint32_t*>(second + 4) == 0xa5a5a5a5u;
	reset();
	adjacent_ok = Protect(second, 32768, Mode::Read) && adjacent_ok;
	adjacent_ok = !StageAndPublishGdsRead(second - 4, 0, 8, fill) && prepares == 0 && intact(0xa5) && adjacent_ok;
	adjacent_ok = Free(second) && adjacent_ok;
	if (!adjacent_ok) return false;
#endif
	std::fprintf(stderr, "[GDS_DESTINATION_TEST] bounded-staging=pass range=pass guard=pass preparation-failure=reject\n");
	return true;
}
#endif

void GraphicsRenderMemoryFree(CommandBuffer* buffer, uint64_t vaddr, uint64_t size)
{
	EXIT_IF(g_render_ctx == nullptr);
	Core::LockGuard lock(g_render_ctx->GetMutex());
	GpuMemoryFree(g_render_ctx->GetGraphicCtx(), buffer, vaddr, size, false);
}

void GraphicsRenderDeleteIndexBuffers()
{
	IndexBufferDeleteAll(g_render_ctx->GetGraphicCtx());
}

void GraphicsRenderMemoryFlush(CommandBuffer* buffer, uint64_t vaddr, uint64_t size)
{
	EXIT_IF(g_render_ctx == nullptr);
	Core::LockGuard lock(g_render_ctx->GetMutex());
	GpuMemoryFlush(g_render_ctx->GetGraphicCtx(), buffer, vaddr, size);
}

// Command buffers belong to a bounded pool and are reused continuously. Keep the
// driver's recording allocations instead of releasing them on every reset.
static constexpr VkCommandBufferResetFlags command_buffer_reuse_reset_flags = 0;
static std::atomic_uint64_t                 command_recording_sequence {0};

// This router is shared by normal fence completion, device-idle reset and
// discarded recordings. Completion observers cannot mistake deletion for GPU
// execution; legacy deletion callbacks still run for both completed and canceled
// batches. A failed fence/reset leaves the batch owned by the command buffer.
class CommandRetirementCallbacks
{
	public:
	struct Entry
	{
		void*         obj         = nullptr;
		CommandBuffer::retire_func_t delete_func = nullptr;
		CommandBuffer::completion_func_t completion_func = nullptr;
		bool queue_ordered = false;
	};
	void Add(Entry entry)
	{
		EXIT_IF(entry.obj == nullptr || (entry.delete_func == nullptr) == (entry.completion_func == nullptr));
		Core::LockGuard lock(m_mutex);
		EXIT_IF(m_ready);
		m_entries.Add(entry);
	}
	bool FenceResult(VkResult result)
	{
		if (result != VK_SUCCESS) { return false; }
		Core::LockGuard lock(m_mutex);
		m_ready = true;
		m_completed = true;
		return true;
	}
	bool ResetResult(bool was_submitted, VkResult result)
	{
		if (result != VK_SUCCESS) { return false; }
		Core::LockGuard lock(m_mutex);
		m_ready = true;
		// A repeated reset/Free before the deferred drain must not turn an
		// already completed submission into a canceled recording.
		m_completed = m_completed || was_submitted;
		return true;
	}
	bool Drain(GraphicContext* ctx, bool after_device_idle = false)
	{
		Vector<Entry> entries;
		bool completed = false;
		{
			Core::LockGuard lock(m_mutex);
			if (!m_ready) { return true; }
			if (!m_completed && !after_device_idle)
			{
				Vector<Entry> queue_ordered;
				entries.Expand(m_entries.Size());
				queue_ordered.Expand(m_entries.Size());
				for (const auto& entry: m_entries)
				{
					// Resetting an unsubmitted recording proves only that its own
					// commands were discarded. Older work on the same queue may
					// still reference a queue-ordered resource. Keep just those
					// entries for a later fence on this persistent command buffer.
					if (entry.queue_ordered) { queue_ordered.Add(entry); }
					else { entries.Add(entry); }
				}
				m_entries = std::move(queue_ordered);
			} else
			{
				entries = std::move(m_entries);
				m_entries = Vector<Entry>();
			}
			completed = m_completed;
			m_ready = false;
			m_completed = false;
		}
		for (const auto& entry: entries)
		{
			if (entry.completion_func != nullptr) { entry.completion_func(ctx, entry.obj, completed); }
			else { entry.delete_func(ctx, entry.obj); }
		}
		return true;
	}
	bool HasEntries()
	{
		Core::LockGuard lock(m_mutex);
		return !m_entries.IsEmpty();
	}
	private:
	Core::Mutex m_mutex;
	Vector<Entry> m_entries;
	bool m_ready = false;
	bool m_completed = false;
};

#if defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
bool GraphicsRenderConstantUploadContractTest()
{
	ShaderBindResources vs {}, ps {};
	vs.push_constant_size = 160;
	ps.push_constant_offset = 160;
	ps.push_constant_size = 112;
	std::vector<uint32_t> vertex(40), pixel(28), first(68), second(68);
	for (size_t index = 0; index < vertex.size(); ++index) vertex[index] = 0x1000u + static_cast<uint32_t>(index);
	for (size_t index = 0; index < pixel.size(); ++index) pixel[index] = 0x2000u + static_cast<uint32_t>(index);
	if (!PackShaderConstantWords(&first, vs, vertex.data()) || !PackShaderConstantWords(&first, ps, pixel.data()) ||
	    !std::equal(vertex.begin(), vertex.end(), first.begin()) || !std::equal(pixel.begin(), pixel.end(), first.begin() + 40)) return false;
	for (auto& word: vertex) word += 0x4000u;
	for (auto& word: pixel) word += 0x4000u;
	if (!PackShaderConstantWords(&second, vs, vertex.data()) || !PackShaderConstantWords(&second, ps, pixel.data())) return false;
	auto bad = ps;
	bad.push_constant_offset = 161;
	if (PackShaderConstantWords(&second, bad, pixel.data())) return false;
	bad.push_constant_offset = UINT32_MAX - 3;
	if (PackShaderConstantWords(&second, bad, pixel.data())) return false;
	VkDeviceSize first_offset = UINT64_MAX, second_offset = UINT64_MAX;
	if (!ConstantArenaOffset(0, 272, 256, &first_offset) || first_offset != 0 ||
	    !ConstantArenaOffset(272, 272, 256, &second_offset) || second_offset != 512 ||
	    ConstantArenaOffset(CONSTANT_ARENA_BYTES - 256, 272, 256, &second_offset) ||
	    ConstantArenaOffset(UINT64_MAX, 272, 256, &second_offset) || ConstantArenaOffset(0, 272, 0, &second_offset)) return false;
	std::vector<uint8_t> arena_bytes(CONSTANT_ARENA_BYTES, 0xcd);
	std::memcpy(arena_bytes.data() + first_offset, first.data(), 272);
	std::memcpy(arena_bytes.data() + second_offset, second.data(), 272);
	if (std::memcmp(arena_bytes.data(), first.data(), 272) != 0 ||
	    std::memcmp(arena_bytes.data() + 512, second.data(), 272) != 0 || first == second) return false;

	VulkanBuffer backing[2];
	DescriptorRecordingBatch a, b;
	a.active = b.active = true;
	a.constant_buffers.push_back({&backing[0], arena_bytes.data(), 784});
	b.constant_buffers.push_back({&backing[1], nullptr, 272});
	a.constant_pools.push_back({});
	b.constant_pools.push_back({});
	std::vector<DescriptorRecordingBatch::ConstantBuffer> free_buffers;
	size_t released_pools = 0;
	struct Pending { DescriptorRecordingBatch* batch; decltype(free_buffers)* buffers; size_t* pools; };
	const auto retire = +[](GraphicContext*, void* obj) {
		auto* pending = static_cast<Pending*>(obj);
		ReleaseConstantResources(pending->batch, [&](auto&) { ++*pending->pools; }, [&](auto& arena) {
			arena.used = 0;
			pending->buffers->push_back(arena);
		});
		pending->batch->active = false;
	};
	Pending pa {&a, &free_buffers, &released_pools}, pb {&b, &free_buffers, &released_pools};
	CommandRetirementCallbacks callbacks_a, callbacks_b;
	callbacks_a.Add({&pa, retire, nullptr, false});
	callbacks_b.Add({&pb, retire, nullptr, false});
	if (!callbacks_a.Drain(nullptr) || callbacks_a.FenceResult(VK_TIMEOUT) ||
	    callbacks_a.ResetResult(false, VK_ERROR_DEVICE_LOST) || !callbacks_a.Drain(nullptr) ||
	    !free_buffers.empty() || !a.active || !b.active) return false;
	if (!callbacks_a.FenceResult(VK_SUCCESS) || !callbacks_a.Drain(nullptr) || !callbacks_a.Drain(nullptr) ||
	    free_buffers.size() != 1 || free_buffers[0].buffer != &backing[0] || free_buffers[0].used != 0 ||
	    !b.active || b.constant_buffers[0].buffer == free_buffers[0].buffer || released_pools != 1) return false;
	if (!callbacks_b.ResetResult(false, VK_SUCCESS) || !callbacks_b.Drain(nullptr) || !callbacks_b.Drain(nullptr) ||
	    free_buffers.size() != 2 || released_pools != 2 || b.active) return false;
	std::fprintf(stderr, "[GFX_CONSTANT_UPLOAD_TEST] span272=exact two_draws=immutable alignment256=pass arena_spill=pass"
	                     " two_recordings=isolated failed_wait=retained fence_discard=recycle_once\n");
	return true;
}

bool GraphicsRenderDescriptorRecordingContractTest()
{
	// Use the production table and completion router; only Vulkan destruction is
	// observed with a callback, so no device or mirrored cache implementation is needed.
	DescriptorCache::SetTable table;
	DescriptorRecordingBatch a, b, c, d;
	VulkanDescriptorSet handles[7];
	for (int i = 0; i < 7; ++i) { handles[i].pool_id = i % 2; }
	std::vector<VulkanDescriptorSet*> released;
	auto make_set = [&](DescriptorRecordingBatch* owner, int handle, uint64_t buffer, uint64_t sampled) {
		DescriptorCache::Set set;
		set.owner = owner;
		set.set = &handles[handle];
		set.stage = DescriptorCache::Stage::Vertex;
		set.storage_buffers_num = 2;
		set.storage_buffers_id[0] = buffer;
		set.storage_buffers_id[1] = buffer; // duplicate bindings must invalidate only once
		set.textures2d_sampled_num = 1;
		set.textures2d_sampled_id[0] = sampled;
		set.hash = DescriptorCache::CalcHash(set);
		return set;
	};
	table.BeginBatch(&a);
	table.BeginBatch(&b);
	table.BeginBatch(&c);
	auto sa = make_set(&a, 0, 11, 21);
	auto sb = make_set(&b, 1, 11, 21);
	auto sa2 = make_set(&a, 2, 12, 22);
	// Deliberate collisions exercise exact owner and resource equality.
	sa.hash = sb.hash = sa2.hash = 7;
	const int ia = table.Add(sa);
	const int ib = table.Add(sb);
	const int ia2 = table.Add(sa2);
	if (table.FindSet(sa) != sa.set || table.FindSet(sb) != sb.set || table.FindSet(sa2) != sa2.set ||
	    table.active_sets != 3 || table.Clear()) { return false; }
	auto wrong_owner = sa;
	wrong_owner.owner = &c;
	if (table.FindSet(wrong_owner) != nullptr) { return false; }
	table.InvalidateBuffer(11);
	table.InvalidateBuffer(11);
	if (table.FindSet(sa) != nullptr || table.FindSet(sb) != nullptr || table.FindSet(sa2) != sa2.set ||
	    table.active_sets != 3 || !released.empty()) { return false; }
	auto replacement = sa;
	replacement.set = &handles[3];
	const int replacement_index = table.Add(replacement);
	if (replacement_index == ia || replacement_index == ib || replacement_index == ia2 ||
	    table.FindSet(sa) != replacement.set) { return false; }
	table.InvalidateImage(21);
	table.InvalidateImage(21);
	if (table.FindSet(sa) != nullptr || table.active_sets != 4 || !released.empty()) { return false; }
	// Preserve the existing distinction between ordinary storage and GDS bindings.
	auto gds = make_set(&c, 4, 0, 0);
	gds.storage_buffers_num = 0;
	gds.textures2d_sampled_num = 0;
	gds.gds_buffers_num = 1;
	gds.gds_buffers_id[0] = 11;
	gds.textures2d_storage_num = 1;
	gds.textures2d_storage_id[0] = 23;
	gds.hash = DescriptorCache::CalcHash(gds);
	table.Add(gds);
	table.InvalidateBuffer(11);
	if (table.FindSet(gds) != gds.set) { return false; }
	table.InvalidateImage(23);
	if (table.FindSet(gds) != nullptr || table.active_sets != 5) { return false; }

	struct Pending
	{
		DescriptorCache::SetTable* table;
		DescriptorRecordingBatch* batch;
		std::vector<VulkanDescriptorSet*>* released;
	};
	const auto retire = +[](GraphicContext*, void* obj) {
		auto* pending = static_cast<Pending*>(obj);
		pending->table->ReleaseBatch(pending->batch, [&](VulkanDescriptorSet* handle) { pending->released->push_back(handle); });
	};
	Pending pending_a {&table, &a, &released};
	Pending pending_b {&table, &b, &released};
	CommandRetirementCallbacks callbacks_a, callbacks_b;
	callbacks_a.Add({&pending_a, retire, nullptr, false});
	callbacks_b.Add({&pending_b, retire, nullptr, false});
	// Recording/End without submission grants no release; failures retain ownership.
	if (!callbacks_a.Drain(nullptr) || callbacks_a.FenceResult(VK_TIMEOUT) ||
	    callbacks_a.ResetResult(false, VK_ERROR_DEVICE_LOST) || !callbacks_a.Drain(nullptr) ||
	    !a.active || !b.active || table.active_sets != 5 || !released.empty()) { return false; }
	if (!callbacks_a.FenceResult(VK_SUCCESS) || !callbacks_a.Drain(nullptr) || !callbacks_a.Drain(nullptr) ||
	    a.active || !b.active || released.size() != 3 || released[0] != sa.set || released[1] != sa2.set ||
	    released[2] != replacement.set || table.active_sets != 2) { return false; }
	table.BeginBatch(&d);
	auto sd = make_set(&d, 5, 13, 24);
	const int id = table.Add(sd);
	if (id != replacement_index || id == ib || table.HistoricalSlots() != 5) { return false; }
	// A proven discard can release B, even while another batch remains pending.
	if (!callbacks_b.ResetResult(false, VK_SUCCESS) || !callbacks_b.Drain(nullptr) ||
	    !callbacks_b.Drain(nullptr) || b.active || released.size() != 4 || released[3] != sb.set || table.Clear()) { return false; }
	Pending pending_c {&table, &c, &released};
	Pending pending_d {&table, &d, &released};
	CommandRetirementCallbacks callbacks_c, callbacks_d;
	callbacks_c.Add({&pending_c, retire, nullptr, false});
	callbacks_d.Add({&pending_d, retire, nullptr, false});
	// Device idle still requires a successful reset for submitted and unsubmitted
	// recordings. Reset and drain are separate lifecycle phases.
	if (callbacks_c.ResetResult(true, VK_ERROR_DEVICE_LOST) || !callbacks_c.Drain(nullptr, true) ||
	    !c.active || !callbacks_c.ResetResult(true, VK_SUCCESS) || !callbacks_d.ResetResult(false, VK_SUCCESS) ||
	    !c.active || !d.active || released.size() != 4 || table.Clear()) { return false; }
	if (!callbacks_c.Drain(nullptr, true) || !callbacks_d.Drain(nullptr, true) ||
	    !callbacks_c.Drain(nullptr, true) || !callbacks_d.Drain(nullptr, true)) { return false; }
	if (released.size() != 6 || table.active_sets != 0 || table.active_batches != 0 ||
	    table.released_total != 6 || table.completed_batches != 4 || !table.Clear() || !table.Clear() ||
	    table.HistoricalSlots() != 0) { return false; }

	// Thousands of distinct combinations across completed recordings reuse the
	// same slot high-water mark instead of growing with process lifetime.
	for (uint64_t recording = 0; recording < 128; ++recording)
	{
		DescriptorRecordingBatch batch;
		table.BeginBatch(&batch);
		for (uint64_t draw = 0; draw < 64; ++draw)
		{
			auto set = make_set(&batch, 6, recording * 64 + draw + 100, draw);
			table.Add(set);
			if (table.FindSet(set) != set.set) { return false; }
		}
		table.ReleaseBatch(&batch, [](VulkanDescriptorSet*) {});
		if (table.active_sets != 0 || table.active_batches != 0 || table.HistoricalSlots() != 64) { return false; }
	}
	// Actual free-pool bookkeeping permits reuse once and cannot self-link on
	// repeated releases from an already available Vulkan descriptor pool.
	DescriptorCache::Pool pool;
	pool.free = false;
	int first_pool = 2;
	DescriptorCache::MarkPoolAvailable(pool, 7, first_pool);
	if (!pool.free || first_pool != 7 || pool.next_free_pool != 2) { return false; }
	DescriptorCache::MarkPoolAvailable(pool, 7, first_pool);
	return first_pool == 7 && pool.next_free_pool == 2 && table.peak_active_sets == 64 && table.Clear();
}
#endif

#if defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
bool GraphicsRenderPassReuseContractTest()
{
	if (!ShaderStorageWriteAuthorityContractTest()) return false;
	if (!NativeRenderPassReuseOverride(nullptr) || !NativeRenderPassReuseOverride("1") ||
	    NativeRenderPassReuseOverride("0") || NativeRenderPassReuseOverride("") ||
	    NativeRenderPassReuseOverride("01") || NativeRenderPassReuseOverride("true") ||
	    NativeRenderPassReuseOverride("1 ")) return false;
	constexpr const char* selector = "LUMEN_VULKAN_RENDER_PASS_REUSE";
	const char* original_value = std::getenv(selector);
	const bool had_value = original_value != nullptr;
	const std::string original = had_value ? original_value : "";
	const char* expected = std::getenv("LUMEN_EXPECT_VULKAN_RENDER_PASS_REUSE");
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && LUMEN_ABI == LUMEN_ABI_ARM64_V8A)
	if (expected == nullptr && !had_value) expected = "1";
#else
	if (expected == nullptr) expected = "0";
#endif
	const bool enabled = NativeRenderPassReuseEnabled();
	if (expected != nullptr && ((std::strcmp(expected, "0") != 0 && std::strcmp(expected, "1") != 0) ||
	                           enabled != (std::strcmp(expected, "1") == 0))) return false;
#if defined(_WIN32)
	const int changed = _putenv_s(selector, enabled ? "0" : "1");
#else
	const int changed = setenv(selector, enabled ? "0" : "1", 1);
#endif
	const bool latched = changed == 0 && NativeRenderPassReuseEnabled() == enabled;
#if defined(_WIN32)
	const int restored = _putenv_s(selector, original.c_str());
#else
	const int restored = had_value ? setenv(selector, original.c_str(), 1) : unsetenv(selector);
#endif
	if (!latched || restored != 0) return false;
	std::fprintf(stderr, "[GFX_RENDER_PASS_REUSE_SELECTOR_TEST] enabled=%u latched=1 parser=pass\n", enabled ? 1u : 0u);

	RenderPassBatchState state;
	std::string events;
	const auto close = [&] { events += 'E'; };
	const auto open = [&] { events += 'B'; };
	const auto begin = [&](uint64_t id, uint32_t width, bool eligible) {
		return state.Begin(id, width, 64, eligible, close, open);
	};
	// Same attachment pass keeps the physical instance. Outside commands must
	// close it exactly once; an active logical pass cannot be interrupted.
	if (!begin(1, 64, true) || state.Outside(close) || begin(1, 64, true) ||
	    !state.End(close) || !begin(1, 64, true) || !state.End(close) ||
	    !state.Outside(close) || !state.Outside(close) || events != "BE" ||
	    state.End(close) || state.physical || state.logical) return false;
	// Changed extent/identity and an unsafe next draw cannot use the old pass.
	events.clear();
	if (!begin(1, 64, true) || !state.End(close) || !begin(1, 128, true) || !state.End(close) ||
	    !begin(2, 128, true) || !state.End(close) || !begin(2, 128, false) || !state.End(close) ||
	    !begin(2, 128, true) || !state.End(close) || !state.Outside(close) || events != "BEBEBEBEBE") return false;
	// A successful command-buffer reset discards a pending pass without emitting
	// commands, and a new recording can never inherit its reuse eligibility.
	events.clear();
	if (!begin(7, 64, true) || !state.End(close)) return false;
	state.Reset();
	if (!begin(7, 64, true) || !state.End(close) || !state.Outside(close) || events != "BBE" ||
	    begin(0, 64, true) || begin(7, 0, true)) return false;

	ShaderBindResources bind {};
	if (!ShaderBindingsAllowRenderPassReuse(bind)) return false;
	bind.storage_buffers.buffers_num = 1;
	if (ShaderBindingsAllowRenderPassReuse(bind)) return false; // Unknown.
	for (const auto usage: {ShaderStorageUsage::ReadOnly, ShaderStorageUsage::Constant})
	{
		bind.storage_buffers.usages[0] = usage;
		if (!ShaderBindingsAllowRenderPassReuse(bind)) return false;
	}
	bind.storage_buffers.usages[0] = ShaderStorageUsage::ReadWrite;
	if (ShaderBindingsAllowRenderPassReuse(bind)) return false;
	bind = {};
	bind.textures2D.textures_num = bind.textures2D.textures2d_sampled_num = 1;
	bind.textures2D.desc[0].usage = ShaderTextureUsage::ReadOnly;
	if (!ShaderBindingsAllowRenderPassReuse(bind)) return false;
	bind.textures2D.desc[0].usage = ShaderTextureUsage::ReadWrite;
	if (ShaderBindingsAllowRenderPassReuse(bind)) return false;
	bind.textures2D.desc[0].usage = ShaderTextureUsage::ReadOnly;
	bind.textures2D.desc[0].textures2d_without_sampler = true;
	if (ShaderBindingsAllowRenderPassReuse(bind)) return false;
	bind = {};
	bind.gds_pointers.pointers_num = 1;
	if (ShaderBindingsAllowRenderPassReuse(bind)) return false;
	bind = {};
	bind.textures2D.textures2d_storage_num = 1;
	if (ShaderBindingsAllowRenderPassReuse(bind)) return false;

	const auto memory = [](uintptr_t value) { return reinterpret_cast<VkDeviceMemory>(value); };
	GpuMemoryImageAccessBatch batch;
	batch.color_memory = memory(1);
	batch.depth_memory = memory(2);
	batch.ObserveRenderResource(memory(3));
	if (!batch.render_pass_read_only) return false;
	const VkDeviceMemory unsafe_memories[] = {VK_NULL_HANDLE, memory(1), memory(2)};
	for (const auto unsafe: unsafe_memories)
	{
		batch.render_pass_read_only = true;
		batch.ObserveRenderResource(unsafe);
		batch.ObserveRenderResource(memory(3));
		if (batch.render_pass_read_only) return false; // Safe later bind cannot erase an alias.
	}
	if (StorageBufferDeclaredAccess(ShaderStorageUsage::Constant) != GpuMemoryAccessKind::StorageRead ||
	    StorageBufferDeclaredAccess(ShaderStorageUsage::ReadOnly) != GpuMemoryAccessKind::StorageRead ||
	    StorageBufferDeclaredAccess(ShaderStorageUsage::ReadWrite) != GpuMemoryAccessKind::StorageReadWrite ||
	    StorageBufferDeclaredAccess(ShaderStorageUsage::Unknown) != GpuMemoryAccessKind::Unknown ||
	    StorageBufferDeclaredAccess(static_cast<ShaderStorageUsage>(255)) != GpuMemoryAccessKind::Unknown) return false;
	GpuMemoryStorageAccessBatch storage;
	const GpuMemoryObjectHandle storage_handle {reinterpret_cast<void*>(uintptr_t {0x3000}), 1, 0, 0};
	const GpuMemoryAccessKind mixed_uses[] = {GpuMemoryAccessKind::StorageReadWrite, GpuMemoryAccessKind::StorageRead,
	                                        GpuMemoryAccessKind::Unknown};
	const VulkanPipeline unclassified_pipeline {};
	const GpuMemoryCommandAccess unclassified_access {};
	if (unclassified_pipeline.vs_storage_write_authority.complete || unclassified_pipeline.ps_storage_write_authority.complete ||
	    unclassified_pipeline.cs_storage_write_authority.complete || unclassified_access.storage_no_shader_stores) return false;
	for (const auto stage: {ShaderType::Vertex, ShaderType::Pixel, ShaderType::Compute})
	{
		// Original bounded ISA: BUFFER_LOAD_DWORD v1,v0,s[0:3],0 idxen;
		// S_ENDPGM. The write cases change the local decoded opcode only.
		uint32_t words[7] {0xe0000000u | (0x0cu << 18u) | (1u << 13u), 0x80000100u, 0xbf810000u};
		ShaderCode read_code;
		read_code.SetType(stage);
		if (ShaderTryParseBounded(words, 7, &read_code) != 3 || read_code.GetInstructions().Size() != 2 ||
		    read_code.GetInstructions().At(0).type != ShaderInstructionType::BufferLoadDword ||
		    !ShaderStorageNoStoreProof(read_code, stage) || ShaderStorageNoStoreProof(read_code, ShaderType::Unknown)) return false;
		for (const auto other: {ShaderType::Vertex, ShaderType::Pixel, ShaderType::Compute})
			if (other != stage && ShaderStorageNoStoreProof(read_code, other)) return false;
		constexpr ShaderInstructionType stores[] {ShaderInstructionType::BufferStoreFormatX,
		    ShaderInstructionType::BufferStoreFormatXy, ShaderInstructionType::BufferStoreDword};
		constexpr uint32_t store_opcodes[] {0x04u, 0x05u, 0x1cu};
		for (uint32_t index = 0; index < 3; ++index)
		{
			words[0] = 0xe0000000u | (store_opcodes[index] << 18u) | (1u << 13u);
			ShaderCode write_code;
			write_code.SetType(stage);
			if (ShaderTryParseBounded(words, 7, &write_code) != 3 || write_code.GetInstructions().Size() != 2 ||
			    write_code.GetInstructions().At(0).type != stores[index] || !ShaderCodeHasStorageBufferWrites(write_code) ||
			    ShaderStorageNoStoreProof(write_code, stage)) return false;
		}
		ShaderCode empty_code;
		empty_code.SetType(stage);
		if (ShaderStorageNoStoreProof(empty_code, stage)) return false;
		for (const bool vertex_embedded: {false, true})
		{
			auto embedded = read_code;
			if (vertex_embedded) embedded.SetVsEmbedded(true); else embedded.SetPsEmbedded(true);
			if (ShaderStorageNoStoreProof(embedded, stage)) return false;
		}
	}
	// Per-binding facts survive pipeline caching and repeated stage uses;
	// an unknown stage or a later writer never acquires the positive fact.
	ShaderStorageWriteAuthority selective {};
	selective.complete = true;
	selective.no_store_mask = 0x5u;
	selective.write_mask = 0x2u;
	if (!ShaderStorageBindingWriteProof(selective, 1) || ShaderStorageBindingWriteProof(selective, 0) ||
	    ShaderStorageBindingWriteProof(selective, 2) || ShaderStorageBindingWriteProof(selective, -1) ||
	    ShaderStorageBindingWriteProof(selective, ShaderStorageResources::BUFFERS_MAX) ||
	    ShaderStorageBindingWriteProof(unclassified_pipeline.cs_storage_write_authority, 1)) return false;
	if (!ShaderStorageBindingNoStoreProof(selective, 0) || ShaderStorageBindingNoStoreProof(selective, 1) ||
	    !ShaderStorageBindingNoStoreProof(selective, 2) || ShaderStorageBindingNoStoreProof(selective, -1) ||
	    ShaderStorageBindingNoStoreProof(selective, ShaderStorageResources::BUFFERS_MAX) ||
	    ShaderStorageBindingNoStoreProof(unclassified_pipeline.cs_storage_write_authority, 0)) return false;
	auto cached = unclassified_pipeline;
	cached.cs_storage_write_authority = selective;
	if (!ShaderStorageBindingNoStoreProof(cached.cs_storage_write_authority, 0) ||
	    ShaderStorageBindingNoStoreProof(cached.cs_storage_write_authority, 1)) return false;
	selective.write_mask |= 1u;
	if (ShaderStorageBindingNoStoreProof(selective, 0) || ShaderStorageBindingWriteProof(selective, 1)) return false;
	selective.write_mask &= ~1u;
	selective.complete = false;
	if (ShaderStorageBindingNoStoreProof(selective, 0) || ShaderStorageBindingWriteProof(selective, 1)) return false;
	GpuMemoryStorageAccessBatch proofs;
	if (!proofs.Add(storage_handle, GpuMemoryAccessKind::StorageRead, true) ||
	    !proofs.Add(storage_handle, GpuMemoryAccessKind::StorageRead, false) ||
	    !proofs.Add(storage_handle, GpuMemoryAccessKind::StorageRead, true) || proofs.count != 3 ||
	    !proofs.accesses[0].storage_no_shader_stores || proofs.accesses[1].storage_no_shader_stores ||
	    !proofs.accesses[2].storage_no_shader_stores) return false;
	for (const auto kind: {GpuMemoryAccessKind::StorageReadWrite, GpuMemoryAccessKind::Unknown})
	{
		if (!proofs.Add(storage_handle, kind, true) || proofs.accesses[proofs.count - 1].storage_no_shader_stores) return false;
	}
	for (uint32_t index = 0; index < storage.MAX; ++index)
	{
		if (!storage.Add(storage_handle, mixed_uses[index % 3]) ||
		    storage.accesses[index].kind != mixed_uses[index % 3] || storage.accesses[index].storage_no_shader_stores) return false;
	}
	if (storage.Add(storage_handle, GpuMemoryAccessKind::StorageRead) || storage.count != storage.MAX) return false;
	GpuMemoryStorageAccessBatch invalid;
	for (unsigned field = 0; field != 4; ++field)
	{
		auto bad = storage_handle;
		if (field == 0) bad.host_object = nullptr;
		if (field == 1) bad.lifetime_generation = 0;
		if (field == 2) bad.heap_id = -1;
		if (field == 3) bad.object_id = -1;
		if (invalid.Add(bad, GpuMemoryAccessKind::Unknown) || invalid.count != 0) return false;
	}
	// Null buffer/global tracker would fail if any zero-work path attempted
	// command registration. Nonempty tracking-OFF must also leave it alone.
	for (unsigned dimension = 0; dimension != 3; ++dimension)
	{
		const uint32_t counts[] = {dimension == 0 ? 0u : 1u, dimension == 1 ? 0u : 1u, dimension == 2 ? 0u : 1u};
		RecordShaderResourceCommandAccesses(nullptr, counts[0], counts[1], counts[2], batch, storage,
		                                   GpuMemoryCommandMutationPolicy::UniqueComputeStorageWrite);
	}
	if (!GpuMemoryAccessTrackingEnabled())
		RecordShaderResourceCommandAccesses(nullptr, 1, 1, 1, batch, storage,
		                                   GpuMemoryCommandMutationPolicy::UniqueComputeStorageWrite);
	std::fprintf(stderr, "[GFX_STORAGE_ACCESS_BATCH_TEST] declared-usage=pass bounds=32 mixed-stage-uses=pass"
	                     " invalid-handles=pass zero-command=pass tracking-off=preserved\n");
	std::fprintf(stderr, "[GFX_STORAGE_NO_STORE_STAGE_TEST] actual-ir-vs/ps/cs=pass supported-stores=reject"
	                     " empty/embedded/mismatched=reject defaults=unproven mixed-stage-proof=preserved zero-command=unrecorded\n");
	std::fprintf(stderr, "[GFX_RENDER_PASS_REUSE_TEST] adjacent=pass outside-close=pass unsafe-boundary=pass"
	                     " identity-extent=pass reset=pass writable-resource=pass allocation-alias=pass\n");
	return true;
}
#endif

struct CommandBuffer::RetiredResources
{
	CommandRetirementCallbacks callbacks;
	RenderPassBatchState render_pass;
	uint64_t      submission_session_epoch        = 0;
	uint64_t      recording_submit_serial         = 0;
	uint64_t      begin_submit_serial_cutoff      = 0;
	uint64_t      begin_completed_serial_cutoff   = 0;
	uint64_t      successful_submit_serial        = 0;
};

CommandBuffer::CommandBuffer(int queue): m_queue(queue), m_retired(new RetiredResources)
{
	Allocate();
}

CommandBuffer::~CommandBuffer()
{
	Free();
	// A queue-ordered entry may outlive a canceled recording only while this
	// persistent buffer can provide a later fence or a device-idle drain.
	EXIT_IF(m_retired->callbacks.HasEntries());
	EXIT_IF(m_descriptor_batch != nullptr);
	EXIT_IF(m_upload_batch != nullptr);
	delete m_retired;
}

DescriptorRecordingBatch* CommandBuffer::GetDescriptorBatch()
{
	EXIT_IF(!m_recording || m_execute || IsInvalid());
	if (m_descriptor_batch == nullptr)
	{
		m_descriptor_batch = g_render_ctx->GetDescriptorCache()->CreateBatch(this);
		Retire(m_descriptor_batch, ReleaseDescriptorBatch);
	}
	return m_descriptor_batch;
}

void CommandBuffer::ReleaseDescriptorBatch(GraphicContext*, void* obj)
{
	auto* batch = static_cast<DescriptorRecordingBatch*>(obj);
	EXIT_IF(batch == nullptr || batch->command == nullptr || batch->command->m_descriptor_batch != batch);
	// Clear only when the completion router actually releases this batch. End,
	// failed waits/resets and deferred device-idle drains preserve ownership.
	batch->command->m_descriptor_batch = nullptr;
	batch->cache->ReleaseBatch(batch);
}

struct UploadRecordingRetirement
{
	CommandBuffer* command;
	UploadRecordingBatch* batch;
	uint64_t recording;
};

UploadRecordingBatch* CommandBuffer::GetUploadRecordingBatch()
{
	EXIT_IF(!m_recording || m_execute || IsInvalid());
	if (m_upload_batch == nullptr)
	{
		auto* ctx = g_render_ctx->GetGraphicCtx();
		EXIT_IF(m_queue < 0 || m_queue >= GraphicContext::QUEUES_NUM);
		m_upload_batch = UtilCreateUploadRecordingBatch(ctx, ctx->queues[m_queue].family);
		m_upload_recording_id = m_recording_id;
		Retire(new UploadRecordingRetirement {this, m_upload_batch, m_upload_recording_id}, ReleaseUploadRecordingBatch);
	}
	EXIT_IF(m_upload_recording_id != m_recording_id);
	return m_upload_batch;
}

void CommandBuffer::ReleaseUploadRecordingBatch(GraphicContext* ctx, void* obj)
{
	auto* retired = static_cast<UploadRecordingRetirement*>(obj);
	EXIT_IF(retired == nullptr || retired->command == nullptr);
	auto* command = retired->command;
	EXIT_IF(command->m_upload_batch != retired->batch || command->m_upload_recording_id != retired->recording);
	command->m_upload_batch = nullptr;
	command->m_upload_recording_id = 0;
	UtilReleaseUploadRecordingBatch(ctx, retired->batch);
	delete retired;
}

void CommandBuffer::Retire(void* obj, retire_func_t delete_func)
{
	EXIT_IF(obj == nullptr);
	EXIT_IF(delete_func == nullptr);
	EXIT_IF(m_retired == nullptr);

	m_retired->callbacks.Add({obj, delete_func, nullptr, false});
}

void CommandBuffer::Retire(void* obj, completion_func_t completion_func)
{
	EXIT_IF(m_retired == nullptr);
	m_retired->callbacks.Add({obj, nullptr, completion_func, false});
}

void CommandBuffer::RetireAfterQueueIdle(void* obj, retire_func_t delete_func)
{
	EXIT_IF(obj == nullptr);
	EXIT_IF(delete_func == nullptr);
	EXIT_IF(m_retired == nullptr);
	EXIT_IF(m_parent == nullptr || !m_recording || m_execute || IsInvalid());

	m_retired->callbacks.Add({obj, delete_func, nullptr, true});
}

void CommandBuffer::ReleaseRetired(bool after_device_idle)
{
	EXIT_IF(m_draw_access_reservation.BlocksSubmission());
	EXIT_IF(m_retired == nullptr);
	EXIT_IF(g_render_ctx == nullptr);

	EXIT_IF(!m_retired->callbacks.Drain(g_render_ctx->GetGraphicCtx(), after_device_idle));
}

bool CommandBuffer::IsInvalid() const
{
	EXIT_IF(g_render_ctx == nullptr);

	if (m_pool != nullptr)
	{
		Core::LockGuard lock(m_pool->mutex);

		return (m_index == static_cast<uint32_t>(-1) || m_index >= m_pool->buffers_count);
	}

	return true;
}

bool CommandBuffer::CanRecordTextureTransfer() const
{
	return m_recording && !m_execute && !m_render_pass_active && m_parent != nullptr &&
	       m_queue == GraphicContext::QUEUE_GFX && !IsInvalid() && m_pool->buffers != nullptr &&
	       m_pool->buffers[m_index] != nullptr;
}

static bool StoppedCommandPoolSlotValid(VulkanCommandPool* pool, uint32_t index)
{
	if (pool == nullptr) return false;
	Core::LockGuard lock(pool->mutex);
	return pool->pool != nullptr && index < pool->buffers_count && pool->buffers != nullptr &&
	       pool->fences != nullptr && pool->busy != nullptr && pool->buffers[index] != nullptr &&
	       pool->fences[index] != nullptr && pool->busy[index];
}

static bool RecordingRequiresSubmission(bool valid, bool recording, bool executing, bool native_commands,
    uint64_t command_ordinal, bool descriptors, bool render_pass_active, const RenderPassBatchState& render_pass,
    CommandRetirementCallbacks& callbacks)
{
	return !valid || !recording || executing || native_commands || command_ordinal != 0 || descriptors ||
	    render_pass_active || render_pass.physical || render_pass.logical || callbacks.HasEntries();
}

bool CommandBuffer::HasPendingSubmissionWork() const
{
	if (m_retired == nullptr) return true;
	return RecordingRequiresSubmission(StoppedCommandPoolSlotValid(m_pool, m_index), m_recording, m_execute,
	    m_native_commands, m_tracked_command_ordinal, m_descriptor_batch != nullptr, m_render_pass_active,
	    m_retired->render_pass, m_retired->callbacks);
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool CommandBufferPendingSubmissionWorkContractTest()
{
	CommandRetirementCallbacks callbacks;
	RenderPassBatchState pass;
	const auto pending = [&](bool valid = true, bool recording = true, bool executing = false,
	    bool native = false, uint64_t ordinal = 0, bool descriptors = false, bool active = false) {
		return RecordingRequiresSubmission(valid, recording, executing, native, ordinal, descriptors, active, pass, callbacks);
	};
	if (pending() || !pending(false) || !pending(true, false) || !pending(true, true, true) ||
	    !pending(true, true, false, true) || !pending(true, true, false, false, 1) ||
	    !pending(true, true, false, false, 0, true) || !pending(true, true, false, false, 0, false, true)) return false;
	unsigned opens = 0, closes = 0;
	if (!pass.Begin(1, 64, 64, true, [&] { ++closes; }, [&] { ++opens; }) || !pending() ||
	    !pass.End([&] { ++closes; }) || !pending() || !pass.Outside([&] { ++closes; }) || pending() ||
	    opens != 1 || closes != 1) return false;
	unsigned retired = 0;
	callbacks.Add({&retired, [](GraphicContext*, void* value) { ++*static_cast<unsigned*>(value); }, nullptr, true});
	if (!pending() || callbacks.FenceResult(VK_TIMEOUT) || !pending() || retired != 0) return false;
	// A discarded recording cannot release a callback protecting older queue
	// work. Even its otherwise empty successor still needs a fence submission.
	if (!callbacks.ResetResult(false, VK_SUCCESS) || !callbacks.Drain(nullptr) || !pending() || retired != 0 ||
	    !callbacks.FenceResult(VK_SUCCESS) || !callbacks.Drain(nullptr) || pending() || retired != 1) return false;
	std::fprintf(stderr, "[GFX_PENDING_SUBMISSION_WORK_TEST] empty-recording=skippable native/ordinal/descriptors=submit"
	    " active/retained-pass=submit callback-only/timeout/discard=retained invalid/nonrecording=conservative\n");
	return true;
}
#endif

static bool StoppedSubmissionMetadataMatches(const GpuMemoryRecordingIdentity& retained,
    const GpuMemoryRecordingIdentity& original, bool recording, bool execute, bool render_pass_active, bool pool_slot_valid)
{
	if (recording || execute || render_pass_active || !pool_slot_valid || original.cp == nullptr || original.pool == nullptr ||
	    original.queue < 0 || original.queue >= GraphicContext::QUEUES_NUM || original.recording_id == 0 ||
	    original.submission_session_epoch == 0 || original.recording_submit_serial == 0 ||
	    original.begin_submit_serial_cutoff == UINT64_MAX ||
	    original.recording_submit_serial != original.begin_submit_serial_cutoff + 1u ||
	    original.begin_completed_serial_cutoff > original.begin_submit_serial_cutoff) return false;
	return std::tie(retained.cp, retained.pool, retained.queue, retained.recording_id, retained.command_ordinal,
	                retained.submission_session_epoch, retained.recording_submit_serial,
	                retained.begin_submit_serial_cutoff, retained.begin_completed_serial_cutoff) ==
	       std::tie(original.cp, original.pool, original.queue, original.recording_id, original.command_ordinal,
	                original.submission_session_epoch, original.recording_submit_serial,
	                original.begin_submit_serial_cutoff, original.begin_completed_serial_cutoff);
}

bool CommandBuffer::MatchesStoppedSubmissionIdentity(const GpuMemoryRecordingIdentity& original) const
{
	// Generic recording getters deliberately return zero after End. Read only
	// this owned buffer's retained fields, without granting submission/fence
	// authority or exposing the successor recording's newer Begin cutoffs.
	if (m_retired == nullptr || m_pool != original.pool || m_parent != original.cp) return false;
	const GpuMemoryRecordingIdentity retained {m_parent, m_pool, m_recording_id, m_tracked_command_ordinal,
	    m_retired->submission_session_epoch, m_retired->recording_submit_serial,
	    m_retired->begin_submit_serial_cutoff, m_retired->begin_completed_serial_cutoff, m_queue};
	return StoppedSubmissionMetadataMatches(retained, original, m_recording, m_execute, m_render_pass_active,
	    StoppedCommandPoolSlotValid(m_pool, m_index));
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool CommandBufferStoppedSubmissionIdentityContractTest()
{
	VulkanCommandPool pool;
	VkCommandBuffer buffers[2] {reinterpret_cast<VkCommandBuffer>(uintptr_t {1}), reinterpret_cast<VkCommandBuffer>(uintptr_t {2})};
	VkFence fences[2] {reinterpret_cast<VkFence>(uintptr_t {3}), reinterpret_cast<VkFence>(uintptr_t {4})};
	bool busy[2] {true, true};
	pool.pool = reinterpret_cast<VkCommandPool>(uintptr_t {5});
	pool.buffers = buffers; pool.fences = fences; pool.busy = busy; pool.buffers_count = 2;
	const GpuMemoryRecordingIdentity original {reinterpret_cast<CommandProcessor*>(uintptr_t {6}), &pool,
	    17, 3, 8, 22, 21, 19, GraphicContext::QUEUE_GFX};
	uint32_t cases = 0;
	const auto match = [&](const GpuMemoryRecordingIdentity& retained, const GpuMemoryRecordingIdentity& expected,
	    bool recording = false, bool execute = false, bool render_pass = false, uint32_t index = 1) {
		++cases;
		return StoppedSubmissionMetadataMatches(retained, expected, recording, execute, render_pass,
		    StoppedCommandPoolSlotValid(&pool, index));
	};
	if (!match(original, original)) return false;
	for (uint32_t field = 0; field != 9; ++field)
	{
		auto changed = original;
		switch (field)
		{
			case 0: changed.cp = reinterpret_cast<CommandProcessor*>(uintptr_t {7}); break;
			case 1: changed.pool = reinterpret_cast<VulkanCommandPool*>(uintptr_t {8}); break;
			case 2: ++changed.queue; break;
			case 3: ++changed.recording_id; break;
			case 4: ++changed.command_ordinal; break;
			case 5: ++changed.submission_session_epoch; break;
			case 6: ++changed.recording_submit_serial; break;
			case 7: ++changed.begin_submit_serial_cutoff; break;
			case 8: ++changed.begin_completed_serial_cutoff; break;
		}
		if (match(changed, original)) return false;
	}
	if (match(original, original, true) || match(original, original, false, true) ||
	    match(original, original, false, false, true) || match(original, original, false, false, false, UINT32_MAX) ||
	    match(original, original, false, false, false, 2)) return false;
	for (uint32_t field = 0; field != 10; ++field)
	{
		auto invalid = original;
		switch (field)
		{
			case 0: invalid.cp = nullptr; break;
			case 1: invalid.pool = nullptr; break;
			case 2: invalid.queue = -1; break;
			case 3: invalid.queue = GraphicContext::QUEUES_NUM; break;
			case 4: invalid.recording_id = 0; break;
			case 5: invalid.submission_session_epoch = 0; break;
			case 6: invalid.recording_submit_serial = 0; break;
			case 7: invalid.begin_submit_serial_cutoff = UINT64_MAX; break;
			case 8: invalid.begin_submit_serial_cutoff = invalid.recording_submit_serial; break;
			case 9: invalid.begin_completed_serial_cutoff = invalid.begin_submit_serial_cutoff + 1; break;
		}
		if (match(invalid, invalid)) return false;
	}
	for (uint32_t field = 0; field != 7; ++field)
	{
		const auto native_pool = pool.pool;
		switch (field)
		{
			case 0: pool.pool = nullptr; break;
			case 1: pool.buffers = nullptr; break;
			case 2: pool.fences = nullptr; break;
			case 3: pool.busy = nullptr; break;
			case 4: buffers[1] = nullptr; break;
			case 5: fences[1] = nullptr; break;
			case 6: busy[1] = false; break;
		}
		const bool accepted = match(original, original);
		pool.pool = native_pool; pool.buffers = buffers; pool.fences = fences; pool.busy = busy;
		buffers[1] = reinterpret_cast<VkCommandBuffer>(uintptr_t {2});
		fences[1] = reinterpret_cast<VkFence>(uintptr_t {4}); busy[1] = true;
		if (accepted) return false;
	}
	if (StoppedCommandPoolSlotValid(nullptr, 0)) return false;
	++cases;
	auto ordinal_zero = original;
	ordinal_zero.command_ordinal = 0;
	if (!match(ordinal_zero, ordinal_zero)) return false;
	std::fprintf(stderr, "[GFX_STOPPED_SUBMISSION_IDENTITY_TEST] cases=%u retained-nine-fields=exact stopped-phase=required"
	    " pool/slot/busy/buffer/fence=checked pool-mutex=real getters=unchanged native-submit/fence=modeled"
	    " native-queries=0 waits=0 authority=metadata-only\n", cases);
	return true;
}
#endif

uint64_t CommandBuffer::GetSubmissionSessionEpoch() const
{
	return m_recording && m_retired != nullptr ? m_retired->submission_session_epoch : 0;
}

uint64_t CommandBuffer::GetRecordingSubmitSerial() const
{
	return m_recording && m_retired != nullptr ? m_retired->recording_submit_serial : 0;
}

uint64_t CommandBuffer::GetPendingSubmissionSessionEpoch() const
{
	return m_execute && m_retired != nullptr ? m_retired->submission_session_epoch : 0;
}

uint64_t CommandBuffer::GetPendingSubmitSerial() const
{
	return m_execute && m_retired != nullptr ? m_retired->successful_submit_serial : 0;
}

uint64_t CommandBuffer::GetBeginSubmitSerialCutoff() const
{
	return m_recording && m_retired != nullptr ? m_retired->begin_submit_serial_cutoff : 0;
}

uint64_t CommandBuffer::GetBeginCompletedSerialCutoff() const
{
	return m_recording && m_retired != nullptr ? m_retired->begin_completed_serial_cutoff : 0;
}

void CommandBuffer::Allocate()
{
	EXIT_IF(m_draw_access_reservation.BlocksSubmission());
	EXIT_IF(!IsInvalid());
	EXIT_IF(m_descriptor_batch != nullptr);

	m_pool = g_command_pool.GetPool(m_queue, &m_index);

	Core::LockGuard lock(m_pool->mutex);

	EXIT_NOT_IMPLEMENTED(IsInvalid());
	EXIT_IF(!m_pool->busy[m_index]);
	EXIT_NOT_IMPLEMENTED(vkResetCommandBuffer(m_pool->buffers[m_index], command_buffer_reuse_reset_flags) != VK_SUCCESS);
	m_recording                        = false;
	m_render_pass_active               = false;
	m_retired->render_pass.Reset();
	m_recording_id                     = 0;
	m_tracked_command_ordinal          = 0;
	m_retired->submission_session_epoch      = 0;
	m_retired->recording_submit_serial       = 0;
	m_retired->begin_submit_serial_cutoff    = 0;
	m_retired->begin_completed_serial_cutoff = 0;
	m_retired->successful_submit_serial      = 0;
}

void CommandBuffer::Free()
{
	EXIT_IF(m_draw_access_reservation.BlocksSubmission());
	EXIT_IF(IsInvalid());

	WaitForFence();
	m_native_wait_pins.BeforeResetOrReuse();

	{
		Core::LockGuard lock(m_pool->mutex);
		const auto reset_result = vkResetCommandBuffer(m_pool->buffers[m_index], command_buffer_reuse_reset_flags);
		if (!m_retired->callbacks.ResetResult(false, reset_result))
		{
			EXIT_NOT_IMPLEMENTED(reset_result != VK_SUCCESS);
			return;
		}
	}
	// An unsubmitted recording can reference staging resources too. Reset must
	// discard those commands before callbacks free their storage.
	ReleaseRetired();
	{
		Core::LockGuard lock(m_pool->mutex);
		m_pool->busy[m_index] = false;
		m_index = static_cast<uint32_t>(-1);
	}
	m_recording                       = false;
	m_render_pass_active              = false;
	m_retired->render_pass.Reset();
	m_recording_id                    = 0;
	m_tracked_command_ordinal         = 0;
	m_retired->submission_session_epoch      = 0;
	m_retired->recording_submit_serial       = 0;
	m_retired->begin_submit_serial_cutoff    = 0;
	m_retired->begin_completed_serial_cutoff = 0;
	m_retired->successful_submit_serial      = 0;

	EXIT_NOT_IMPLEMENTED(!IsInvalid());
}

void CommandBuffer::Begin() const
{
	EXIT_IF(m_draw_access_reservation.BlocksSubmission());
	EXIT_IF(IsInvalid());
	EXIT_IF(m_recording);
	EXIT_IF(m_render_pass_active);
	EXIT_IF(m_descriptor_batch != nullptr || m_execute);

	auto* buffer = m_pool->buffers[m_index];

	VkCommandBufferBeginInfo begin_info {};
	begin_info.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.pNext            = nullptr;
	begin_info.flags            = 0;
	begin_info.pInheritanceInfo = nullptr;

	auto result = vkBeginCommandBuffer(buffer, &begin_info);

	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
	m_recording_id                     = 0;
	m_retired->submission_session_epoch      = 0;
	m_retired->recording_submit_serial       = 0;
	m_retired->begin_submit_serial_cutoff    = 0;
	m_retired->begin_completed_serial_cutoff = 0;
	m_retired->successful_submit_serial      = 0;
	// VideoOut ownership is authoritative even when the optional access ledger
	// is disabled. Every accepted recording needs its own identity and timeline.
	const uint64_t recording_id = command_recording_sequence.fetch_add(1, std::memory_order_relaxed) + 1u;
	EXIT_IF(recording_id == 0);
	m_recording_id = recording_id;
	if (m_parent != nullptr)
	{
		const bool timeline_valid = GraphicsRunBeginTrackedCommandBufferSubmission(
		    m_parent, m_queue, &m_retired->submission_session_epoch, &m_retired->recording_submit_serial,
		    &m_retired->begin_submit_serial_cutoff, &m_retired->begin_completed_serial_cutoff);
		if (!timeline_valid)
		{
			m_retired->submission_session_epoch      = 0;
			m_retired->recording_submit_serial       = 0;
			m_retired->begin_submit_serial_cutoff    = 0;
			m_retired->begin_completed_serial_cutoff = 0;
		}
	}
	m_tracked_command_ordinal = 0;
	m_native_commands         = false;
	m_recording               = true;
}

void CommandBuffer::End() const
{
	EXIT_IF(m_draw_access_reservation.BlocksSubmission());
	EXIT_IF(IsInvalid());
	EXIT_IF(!m_recording);
	EXIT_IF(m_render_pass_active);
	EndRetainedRenderPass();

	auto* buffer = m_pool->buffers[m_index];

	auto result = vkEndCommandBuffer(buffer);

	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
	m_recording = false;
}

GpuMemoryRecordingIdentity CommandBuffer::GetDrawRecordingIdentity() const
{
	return {m_parent, m_pool, GetRecordingId(), GetTrackedCommandOrdinal(), GetSubmissionSessionEpoch(),
	        GetRecordingSubmitSerial(), GetBeginSubmitSerialCutoff(), GetBeginCompletedSerialCutoff(), m_queue};
}

bool CommandBuffer::CanPauseOwnedPreparation() const
{
	return !IsInvalid() && m_recording && !m_execute && !m_draw_access_reservation.BlocksSubmission();
}

static const void* DrawAccessThreadIdentity()
{
	static thread_local const char identity = 0;
	return &identity;
}

uint64_t CommandBuffer::ReserveDrawAccesses(uint64_t expected_ordinals)
{
	EXIT_IF(!m_recording || m_execute || IsInvalid() || m_queue < 0 || m_queue >= GraphicContext::QUEUES_NUM);
	EXIT_IF(!GraphicsRenderRetainedRecordingScopeValid(this) || !GraphicsRunDrawAccessReservationScopeValid(m_parent, this));
	uint64_t token = 0;
	EXIT_IF(!m_draw_access_reservation.Reserve(GetDrawRecordingIdentity(), m_index, DrawAccessThreadIdentity(), expected_ordinals, &token));
	return token;
}

void CommandBuffer::BeginDrawAccessCommit(uint64_t token)
{
	EXIT_IF(!GraphicsRenderRetainedRecordingScopeValid(this) || !GraphicsRunDrawAccessReservationScopeValid(m_parent, this));
	EXIT_IF(!m_draw_access_reservation.BeginCommit(token, GetDrawRecordingIdentity(), m_index, DrawAccessThreadIdentity()));
}

void CommandBuffer::FinishDrawAccessCommit(uint64_t token)
{
	EXIT_IF(!GraphicsRenderRetainedRecordingScopeValid(this) || !GraphicsRunDrawAccessReservationScopeValid(m_parent, this));
	EXIT_IF(!m_draw_access_reservation.Finish(token, GetDrawRecordingIdentity(), m_index, DrawAccessThreadIdentity()));
}

uint64_t CommandBuffer::NextTrackedCommandOrdinal()
{
	EXIT_IF(m_draw_access_reservation.GetState() == DrawAccessReservation::State::PendingNative ||
	        m_draw_access_reservation.Failed());
	EXIT_IF(!m_recording || m_recording_id == 0);
	EXIT_IF(m_tracked_command_ordinal == UINT64_MAX);
	return ++m_tracked_command_ordinal;
}

void CommandBuffer::Execute()
{
	EXIT_IF(m_draw_access_reservation.BlocksSubmission());
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];
	auto* fence  = m_pool->fences[m_index];

	VkSubmitInfo submit_info {};
	submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.pNext                = nullptr;
	submit_info.waitSemaphoreCount   = 0;
	submit_info.pWaitSemaphores      = nullptr;
	submit_info.pWaitDstStageMask    = nullptr;
	submit_info.commandBufferCount   = 1;
	submit_info.pCommandBuffers      = &buffer;
	submit_info.signalSemaphoreCount = 0;
	submit_info.pSignalSemaphores    = nullptr;

	EXIT_IF(m_queue < 0 || m_queue >= GraphicContext::QUEUES_NUM);

	const auto& queue = g_render_ctx->GetGraphicCtx()->queues[m_queue];
	Lumen::CriticalPath::Scope submission(Lumen::CriticalPath::Reason::VulkanSubmit, static_cast<uint64_t>(m_queue + 1));

	if (queue.mutex != nullptr)
	{
		queue.mutex->Lock();
	}

	auto result = vkQueueSubmit(queue.vk_queue, 1, &submit_info, fence);
	if (result == VK_SUCCESS && m_recording_id != 0 && m_retired->submission_session_epoch != 0 &&
	    m_retired->recording_submit_serial != 0 &&
	    m_parent != nullptr && GraphicsRunRecordTrackedCommandBufferSubmission(
	                               m_parent, m_queue, m_retired->submission_session_epoch,
	                               m_retired->recording_submit_serial))
	{
		m_retired->successful_submit_serial = m_retired->recording_submit_serial;
	}

	if (queue.mutex != nullptr)
	{
		queue.mutex->Unlock();
	}
	submission.End();

	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
	if (result == VK_SUCCESS)
	{
		m_execute = true;
		// Begin assigns a process-unique recording ID. Emit a producer only
		// after accepted submission; a matching fence wait proves completion.
		Lumen::CriticalPath::Event(Lumen::CriticalPath::Reason::GpuSubmissionAccepted, m_recording_id, 0,
		                          static_cast<uint64_t>(m_queue + 1));
	}
}

void CommandBuffer::ExecuteWithSemaphores(VkSemaphore wait_semaphore, VkPipelineStageFlags wait_stage, VkSemaphore signal_semaphore)
{
	EXIT_IF(m_draw_access_reservation.BlocksSubmission());
	EXIT_IF(IsInvalid());
	EXIT_IF(wait_semaphore == nullptr);
	EXIT_IF(wait_stage == 0);
	EXIT_IF(signal_semaphore == nullptr);

	auto* buffer = m_pool->buffers[m_index];
	auto* fence  = m_pool->fences[m_index];

	VkSubmitInfo submit_info {};
	submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.pNext                = nullptr;
	submit_info.waitSemaphoreCount   = 1;
	submit_info.pWaitSemaphores      = &wait_semaphore;
	submit_info.pWaitDstStageMask    = &wait_stage;
	submit_info.commandBufferCount   = 1;
	submit_info.pCommandBuffers      = &buffer;
	submit_info.signalSemaphoreCount = 1;
	submit_info.pSignalSemaphores    = &signal_semaphore;

	EXIT_IF(m_queue < 0 || m_queue >= GraphicContext::QUEUES_NUM);

	const auto& queue = g_render_ctx->GetGraphicCtx()->queues[m_queue];
	Lumen::CriticalPath::Scope submission(Lumen::CriticalPath::Reason::VulkanSubmit, static_cast<uint64_t>(m_queue + 1));

	if (queue.mutex != nullptr)
	{
		queue.mutex->Lock();
	}

	auto result = vkQueueSubmit(queue.vk_queue, 1, &submit_info, fence);
	if (result == VK_SUCCESS && m_recording_id != 0 && m_retired->submission_session_epoch != 0 &&
	    m_retired->recording_submit_serial != 0 &&
	    m_parent != nullptr && GraphicsRunRecordTrackedCommandBufferSubmission(
	                               m_parent, m_queue, m_retired->submission_session_epoch,
	                               m_retired->recording_submit_serial))
	{
		m_retired->successful_submit_serial = m_retired->recording_submit_serial;
	}

	if (queue.mutex != nullptr)
	{
		queue.mutex->Unlock();
	}
	submission.End();

	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
	if (result == VK_SUCCESS)
	{
		m_execute = true;
		Lumen::CriticalPath::Event(Lumen::CriticalPath::Reason::GpuSubmissionAccepted, m_recording_id, 0,
		                          static_cast<uint64_t>(m_queue + 1));
	}
}

template <typename Wait, typename ResetFence, typename ResetCommand, typename Complete, typename Drain>
static VkResult CompleteCommandBufferFenceWait(bool reset_command, bool& executing, uint64_t& successful_serial,
    CommandRetirementCallbacks& callbacks, Wait wait, ResetFence reset_fence, ResetCommand reset_buffer,
    Complete complete, Drain drain)
{
	auto result = wait();
	if (result != VK_SUCCESS) return result;
	result = reset_fence();
	if (result != VK_SUCCESS) return result;
	if (reset_command)
	{
		result = reset_buffer();
		if (result != VK_SUCCESS) return result;
	}
	// Do every native slot mutation before authorizing callbacks. A retirement
	// callback can reenter, begin and even submit a replacement recording in the
	// same persistent slot; nothing below Drain may reset or clear that owner.
	EXIT_IF(!callbacks.FenceResult(VK_SUCCESS));
	complete();
	executing = false;
	successful_serial = 0;
	drain();
	return VK_SUCCESS;
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
static bool command_buffer_fence_wait_contract_test()
{
	struct Fixture
	{
		CommandRetirementCallbacks callbacks;
		bool executing = true, valid = true, restart = false, nested = false;
		uint64_t serial = 7, generation = 1;
		uint32_t waits = 0, fence_resets = 0, buffer_resets = 0, completions = 0, retired = 0;
		VkResult wait_result = VK_SUCCESS, fence_result = VK_SUCCESS, buffer_result = VK_SUCCESS;
		std::vector<uint64_t> reset_generations;
		static VkResult Complete(Fixture* fixture, bool reset)
		{
			return CompleteCommandBufferFenceWait(reset, fixture->executing, fixture->serial, fixture->callbacks,
			    [&] { ++fixture->waits; return fixture->wait_result; },
			    [&] { ++fixture->fence_resets; return fixture->fence_result; },
			    [&] {
				    ++fixture->buffer_resets;
				    fixture->reset_generations.push_back(fixture->generation);
				    return fixture->buffer_result;
			    }, [&] {
				    fixture->valid &= fixture->executing && fixture->serial != 0;
				    ++fixture->completions;
			    }, [&] { fixture->valid &= fixture->callbacks.Drain(nullptr); });
		}
		static void Retired(GraphicContext*, void* object, bool completed)
		{
			auto* fixture = static_cast<Fixture*>(object);
			fixture->valid &= completed && !fixture->executing && fixture->serial == 0;
			++fixture->retired;
			if (!fixture->restart) return;
			fixture->restart = false;
			++fixture->generation;
			fixture->executing = true;
			fixture->serial = 8;
			fixture->callbacks.Add({fixture, nullptr, Retired, false});
			// The real router detaches its completed batch before invoking this
			// callback. A nested completion belongs to the replacement alone.
			if (fixture->nested) fixture->valid &= Complete(fixture, true) == VK_SUCCESS;
		}
		void Add() { callbacks.Add({this, nullptr, Retired, false}); }
	};
	for (unsigned failure = 0; failure != 4; ++failure)
	{
		Fixture fixture;
		fixture.Add();
		if (failure == 0) fixture.wait_result = VK_TIMEOUT;
		if (failure == 1) fixture.wait_result = VK_ERROR_DEVICE_LOST;
		if (failure == 2) fixture.fence_result = VK_ERROR_DEVICE_LOST;
		if (failure == 3) fixture.buffer_result = VK_ERROR_DEVICE_LOST;
		if (Fixture::Complete(&fixture, true) == VK_SUCCESS || !fixture.callbacks.Drain(nullptr) || !fixture.valid ||
		    !fixture.executing || fixture.serial != 7 || !fixture.callbacks.HasEntries() || fixture.completions != 0 ||
		    fixture.retired != 0 || fixture.waits != 1 || fixture.fence_resets != (failure < 2 ? 0u : 1u) ||
		    fixture.buffer_resets != (failure == 3 ? 1u : 0u)) return false;
	}
	for (const bool reset: {false, true})
		for (const bool nested: {false, true})
		{
			Fixture fixture;
			fixture.restart = true;
			fixture.nested = nested;
			fixture.Add();
			if (Fixture::Complete(&fixture, reset) != VK_SUCCESS || !fixture.valid || fixture.generation != 2 ||
			    fixture.completions != (nested ? 2u : 1u) || fixture.retired != (nested ? 2u : 1u) ||
			    fixture.executing != !nested || fixture.serial != (nested ? 0u : 8u) ||
			    fixture.callbacks.HasEntries() != !nested || fixture.reset_generations.size() != unsigned(reset) + unsigned(nested))
				return false;
			// Generation one must be reset before the callback creates generation
			// two. In the nested case, each completion resets its own generation.
			if ((reset && fixture.reset_generations[0] != 1) ||
			    (nested && fixture.reset_generations.back() != 2)) return false;
			if (!nested && (Fixture::Complete(&fixture, reset) != VK_SUCCESS || !fixture.valid ||
			    fixture.executing || fixture.serial != 0 || fixture.completions != 2 || fixture.retired != 2 ||
			    fixture.callbacks.HasEntries())) return false;
			if (!fixture.callbacks.Drain(nullptr) || fixture.retired != 2) return false;
		}
	std::fprintf(stderr, "[GFX_FENCE_WAIT_RETIREMENT_TEST] wait-timeout/device-loss/reset-failures=retained"
	    " native-reset-before-retirement=pass slot-ABA/nested-completion=pass completed-publication=once native-fences=modeled\n");
	return true;
}
#endif

VkResult CommandBuffer::NativeFenceWait::Wait()
{
	if (!pin.IsActive() || device == VK_NULL_HANDLE || fence == VK_NULL_HANDLE)
	{
		pin.Release();
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	VkResult result;
	{
		// This observer retains no queue/recording identity. Zero preserves the
		// trace's unknown-identity contract; completion remains with the owner.
		Lumen::CriticalPath::Scope wait(Lumen::CriticalPath::Reason::GpuFenceWait);
		result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
	}
	// This release needs neither a command-processor lock nor an execution
	// lease, including timeout/device loss. A peer may now reset the old fence.
	pin.Release();
	return result;
}

bool CommandBuffer::CaptureNativeFenceWait(NativeFenceWait* wait)
{
	if (wait == nullptr || wait->IsActive() || IsInvalid() || !m_execute ||
	    m_retired->successful_submit_serial == 0 || m_retired->submission_session_epoch == 0) return false;
	wait->device = g_render_ctx->GetGraphicCtx()->device;
	wait->fence = m_pool->fences[m_index];
	if (wait->device == VK_NULL_HANDLE || wait->fence == VK_NULL_HANDLE) return false;
	wait->pin = m_native_wait_pins.Capture();
	return true;
}

void CommandBuffer::WaitForFenceInternal(bool reset_command)
{
	EXIT_IF(m_draw_access_reservation.BlocksSubmission());
	EXIT_IF(IsInvalid());

	if (m_execute)
	{
		auto* device = g_render_ctx->GetGraphicCtx()->device;

		const auto result = CompleteCommandBufferFenceWait(reset_command, m_execute, m_retired->successful_submit_serial,
		    m_retired->callbacks, [&] {
			    Lumen::CriticalPath::Scope wait(Lumen::CriticalPath::Reason::GpuFenceWait,
			        static_cast<uint64_t>(m_queue + 1), m_recording_id);
			    return vkWaitForFences(device, 1, &m_pool->fences[m_index], VK_TRUE, UINT64_MAX);
		    }, [&] {
			    m_native_wait_pins.BeforeResetOrReuse();
			    return vkResetFences(device, 1, &m_pool->fences[m_index]);
		    }, [&] {
			    return vkResetCommandBuffer(m_pool->buffers[m_index], command_buffer_reuse_reset_flags);
		    }, [&] {
			    Lumen::CriticalPath::Event(Lumen::CriticalPath::Reason::GpuFenceCompleted, m_recording_id, 0,
			        static_cast<uint64_t>(m_queue + 1));
			    if (m_retired->successful_submit_serial != 0 && m_parent != nullptr)
				    GraphicsRunRecordTrackedCommandBufferCompletion(m_parent, m_queue, m_retired->submission_session_epoch,
				        m_retired->successful_submit_serial);
		    }, [&] { ReleaseRetired(); });
		// Failed waits or either failed reset preserve execution metadata and the
		// retirement batch. No completion/publication callback has been admitted.
		EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
	}
}

void CommandBuffer::WaitForFence()
{
	WaitForFenceInternal(false);
}

void CommandBuffer::WaitForFenceAndReset()
{
	WaitForFenceInternal(true);
}

bool CommandBuffer::ResetAfterDeviceIdle()
{
	EXIT_IF(m_draw_access_reservation.BlocksSubmission());
	if (IsInvalid())
	{
		return false;
	}

	auto* device = g_render_ctx->GetGraphicCtx()->device;
	if (device == nullptr)
	{
		return false;
	}
	m_native_wait_pins.BeforeResetOrReuse();
	const bool was_submitted = m_execute;
	if (m_execute)
	{
		// WindowFinishIOSRuntimeSession has already completed a successful
		// vkDeviceWaitIdle. Waiting on the fence again would turn reset into an
		// unbounded operation and adds no ordering guarantee.
		if (vkResetFences(device, 1, &m_pool->fences[m_index]) != VK_SUCCESS)
		{
			return false;
		}
		if (m_retired->successful_submit_serial != 0 && m_parent != nullptr)
		{
			GraphicsRunRecordTrackedCommandBufferCompletion(m_parent, m_queue, m_retired->submission_session_epoch,
			                                                m_retired->successful_submit_serial);
		}
	}

	// Reset first: an unsubmitted recording buffer can still reference objects
	// queued in m_retired even though the device itself is idle.
	const auto reset_result = vkResetCommandBuffer(m_pool->buffers[m_index], command_buffer_reuse_reset_flags);
	if (!m_retired->callbacks.ResetResult(was_submitted, reset_result))
	{
		return false;
	}
	m_execute                        = false;
	m_recording                      = false;
	m_render_pass_active             = false;
	m_retired->render_pass.Reset();
	m_recording_id                   = 0;
	m_tracked_command_ordinal        = 0;
	m_retired->submission_session_epoch      = 0;
	m_retired->recording_submit_serial       = 0;
	m_retired->begin_submit_serial_cutoff    = 0;
	m_retired->begin_completed_serial_cutoff = 0;
	m_retired->successful_submit_serial      = 0;
	return true;
}

void CommandBuffer::ReleaseRetiredAfterDeviceIdle()
{
	ReleaseRetired(true);
}


#if defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
void GraphicsRenderRecordGpuR8ReadbackForTest(VkCommandBuffer command, VkImage image, VkImageLayout original_layout,
                                            uint32_t width, uint32_t height, VkBuffer destination)
{
	RecordGpuR8Readback(command, image, original_layout, width, height, destination);
}

bool GraphicsRenderResourceCaptureLimitsContractTest()
{
	struct Case { const char* value; bool extended; };
	const Case cases[] = {{nullptr, false}, {"", false}, {"0", false}, {"1", true},
	                      {"01", false}, {"true", false}, {" 1", false}, {"1 ", false}};
	for (const auto& test: cases)
	{
		const auto limits = ResourceCaptureLimitsForSelector(test.value);
		const bool extended = test.extended;
		if (limits.extended != extended || limits.max_events != (extended ? 4096u : 512u) ||
		    limits.max_read_bytes != (extended ? 268435456u : 67108864u)) { return false; }
	}
	return true;
}

bool GraphicsRenderGpuR8CaptureContractTest()
{
	if (!command_pool_chunks_contract_test()) return false;
	if (!command_buffer_fence_wait_contract_test()) return false;
	GpuR8CaptureShape shape {true, true, true, VK_FORMAT_R8_UNORM, 512, 512, 512, 512,
	                         ShaderTextureResource::TYPE_2D, 0, 0, 1, 0, 0, 0, VK_IMAGE_LAYOUT_GENERAL, true};
	if (GpuR8CaptureRejection(shape) != nullptr) { return false; }
	shape.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (GpuR8CaptureRejection(shape) != nullptr) { return false; }
	auto rejected = [&](auto mutate) { auto invalid = shape; mutate(invalid); return GpuR8CaptureRejection(invalid) != nullptr; };
	if (!rejected([](auto& s) { s.recording = false; }) ||
	    !rejected([](auto& s) { s.pixel_stage = false; }) ||
	    !rejected([](auto& s) { s.sampled_read_only = false; }) ||
	    !rejected([](auto& s) { s.format = VK_FORMAT_R8G8B8A8_UNORM; }) ||
	    !rejected([](auto& s) { s.transfer_source = false; }) ||
	    !rejected([](auto& s) { s.type = ShaderTextureResource::TYPE_8_COMPAT; }) ||
	    !rejected([](auto& s) { s.depth = 1; }) ||
	    !rejected([](auto& s) { s.base_array = 1; }) ||
	    !rejected([](auto& s) { s.last_array = 1; }) ||
	    !rejected([](auto& s) { s.base_level = 1; }) ||
	    !rejected([](auto& s) { s.last_level = 1; }) ||
	    !rejected([](auto& s) { s.host_levels = 2; }) ||
	    !rejected([](auto& s) { s.layout = VK_IMAGE_LAYOUT_UNDEFINED; }) ||
	    !rejected([](auto& s) { s.width = 0; }) ||
	    !rejected([](auto& s) { s.descriptor_height = 513; }) ||
	    !rejected([](auto& s) { s.width = s.descriptor_width = UINT32_MAX; s.height = s.descriptor_height = UINT32_MAX; }))
	{
		return false;
	}
	shape.width = shape.height = shape.descriptor_width = shape.descriptor_height = 1024;
	if (GpuR8CaptureRejection(shape) != nullptr ||
	    !GpuReadbackBudgetAllowed(7, 7 * GPU_READBACK_IMAGE_CAP, GPU_READBACK_IMAGE_CAP) ||
	    GpuReadbackBudgetAllowed(8, 0, 1) || GpuReadbackBudgetAllowed(0, 0, 0) ||
	    GpuReadbackBudgetAllowed(0, 0, GPU_READBACK_IMAGE_CAP + 1) ||
	    GpuReadbackBudgetAllowed(0, GPU_READBACK_TOTAL_CAP + 1, 1) ||
	    GpuReadbackBudgetAllowed(0, GPU_READBACK_TOTAL_CAP, 1) ||
	    GpuReadbackBudgetAllowed(0, GPU_READBACK_TOTAL_CAP - 1, 2))
	{
		return false;
	}

	GpuR8CaptureShape rgba_shape {true, true, true, VK_FORMAT_R8G8B8A8_UNORM, 256, 256, 256, 256,
	                              ShaderTextureResource::TYPE_2D, 0, 4, 5, 0, 0, 0,
	                              VK_IMAGE_LAYOUT_GENERAL, true};
	GpuRgba8MipLayout rgba_layout {};
	if (GpuRgba8MipsCaptureRejection(rgba_shape, &rgba_layout) != nullptr || rgba_layout.levels != 5 ||
	    rgba_layout.total_bytes != 349184 || rgba_layout.offsets[0] != 0 || rgba_layout.offsets[1] != 262144 ||
	    rgba_layout.offsets[2] != 327680 || rgba_layout.offsets[3] != 344064 || rgba_layout.offsets[4] != 348160 ||
	    rgba_layout.widths[4] != 16 || rgba_layout.heights[4] != 16 || rgba_layout.bytes[4] != 1024)
	{
		return false;
	}
	rgba_shape.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (GpuRgba8MipsCaptureRejection(rgba_shape, &rgba_layout) != nullptr) { return false; }
	auto rgba_rejected = [&](auto mutate) {
		auto invalid = rgba_shape;
		mutate(invalid);
		GpuRgba8MipLayout ignored {};
		return GpuRgba8MipsCaptureRejection(invalid, &ignored) != nullptr;
	};
	if (!rgba_rejected([](auto& s) { s.recording = false; }) ||
	    !rgba_rejected([](auto& s) { s.pixel_stage = false; }) ||
	    !rgba_rejected([](auto& s) { s.sampled_read_only = false; }) ||
	    !rgba_rejected([](auto& s) { s.format = VK_FORMAT_R8_UNORM; }) ||
	    !rgba_rejected([](auto& s) { s.transfer_source = false; }) ||
	    !rgba_rejected([](auto& s) { s.type = ShaderTextureResource::TYPE_8_COMPAT; }) ||
	    !rgba_rejected([](auto& s) { s.depth = 1; }) || !rgba_rejected([](auto& s) { s.base_array = 1; }) ||
	    !rgba_rejected([](auto& s) { s.last_array = 1; }) || !rgba_rejected([](auto& s) { s.base_level = 1; }) ||
	    !rgba_rejected([](auto& s) { s.last_level = 3; }) || !rgba_rejected([](auto& s) { s.host_levels = 1; }) ||
	    !rgba_rejected([](auto& s) { s.host_levels = 16; s.last_level = 15; }) ||
	    !rgba_rejected([](auto& s) { s.layout = VK_IMAGE_LAYOUT_UNDEFINED; }) ||
	    !rgba_rejected([](auto& s) { s.descriptor_width = 255; }) ||
	    !rgba_rejected([](auto& s) { s.width = s.descriptor_width = 1024; s.height = s.descriptor_height = 1024; }))
	{
		return false;
	}

	// Exercise the actual production completion router without a Vulkan device.
	// Neither failed fences nor failed resets authorize its Drain operation.
	struct Observed { uint32_t legacy = 0; uint32_t completed = 0; uint32_t canceled = 0; uint32_t queue_ordered = 0; } observed;
	const auto legacy = +[](GraphicContext*, void* obj) { ++static_cast<Observed*>(obj)->legacy; };
	const auto queue_ordered = +[](GraphicContext*, void* obj) { ++static_cast<Observed*>(obj)->queue_ordered; };
	const auto completion = +[](GraphicContext*, void* obj, bool completed) {
		auto* value = static_cast<Observed*>(obj);
		if (completed) { ++value->completed; } else { ++value->canceled; }
	};
	CommandRetirementCallbacks callbacks;
	auto add_pair = [&] { callbacks.Add({&observed, legacy, nullptr, false}); callbacks.Add({&observed, nullptr, completion, false}); };
	auto counts = [&](uint32_t old, uint32_t done, uint32_t cancel, uint32_t ordered = 0) {
		return observed.legacy == old && observed.completed == done && observed.canceled == cancel &&
		       observed.queue_ordered == ordered;
	};
	add_pair();
	if (!callbacks.Drain(nullptr)) { return false; }
	if (!counts(0, 0, 0) || callbacks.FenceResult(VK_TIMEOUT) || callbacks.FenceResult(VK_ERROR_DEVICE_LOST)) { return false; }
	if (!callbacks.Drain(nullptr)) { return false; }
	if (!counts(0, 0, 0) || !callbacks.FenceResult(VK_SUCCESS) || !counts(0, 0, 0)) { return false; }
	if (!callbacks.Drain(nullptr) || !callbacks.Drain(nullptr)) { return false; }
	if (!counts(1, 1, 0)) { return false; }

	add_pair();
	if (callbacks.ResetResult(false, VK_ERROR_DEVICE_LOST)) { return false; }
	if (!callbacks.Drain(nullptr)) { return false; }
	if (!counts(1, 1, 0) || !callbacks.ResetResult(false, VK_SUCCESS) || !counts(1, 1, 0)) { return false; }
	if (!callbacks.Drain(nullptr) || !callbacks.Drain(nullptr)) { return false; }
	if (!counts(2, 1, 1)) { return false; }

	add_pair();
	if (callbacks.ResetResult(true, VK_ERROR_DEVICE_LOST)) { return false; }
	if (!callbacks.Drain(nullptr)) { return false; }
	if (!counts(2, 1, 1) || !callbacks.ResetResult(true, VK_SUCCESS) || !callbacks.ResetResult(false, VK_SUCCESS) ||
	    !counts(2, 1, 1)) { return false; }
	if (!callbacks.Drain(nullptr) || !callbacks.Drain(nullptr)) { return false; }
	if (!counts(3, 2, 1)) { return false; }

	add_pair();
	if (!callbacks.ResetResult(false, VK_SUCCESS) || !callbacks.FenceResult(VK_SUCCESS)) { return false; }
	if (!callbacks.Drain(nullptr)) { return false; }
	if (!counts(4, 3, 1)) { return false; }
	add_pair();
	if (!callbacks.ResetResult(false, VK_SUCCESS)) { return false; }
	if (!callbacks.Drain(nullptr) || !counts(5, 3, 2)) { return false; }

	add_pair();
	callbacks.Add({&observed, queue_ordered, nullptr, true});
	if (!callbacks.ResetResult(false, VK_SUCCESS) || !callbacks.Drain(nullptr) ||
	    !counts(6, 3, 3) || !callbacks.HasEntries() || !callbacks.FenceResult(VK_SUCCESS) ||
	    !callbacks.Drain(nullptr) || callbacks.HasEntries() || !counts(6, 3, 3, 1))
	{
		return false;
	}
	callbacks.Add({&observed, queue_ordered, nullptr, true});
	if (!callbacks.ResetResult(false, VK_SUCCESS) || !callbacks.Drain(nullptr, true) || !counts(6, 3, 3, 2)) return false;

	// Handoff replacements retain a complete cohort on the later queue fence.
	// Exercise the production router beyond the 64-object all-queue quarantine
	// bound across ordinary completed recordings, with one owning callback per
	// cohort and a distinct observation for every native object.
	constexpr uint32_t HANDOFF_BATCHES = 8;
	constexpr uint32_t HANDOFF_OBJECTS = 12;
	struct HandoffObject { uint32_t deleted = 0; };
	struct HandoffBatch
	{
		std::array<HandoffObject*, HANDOFF_OBJECTS> objects {};
		uint32_t* deleted_batches = nullptr;
	};
	std::array<std::array<HandoffObject, HANDOFF_OBJECTS>, HANDOFF_BATCHES> handoff_objects {};
	uint32_t deleted_batches = 0;
	const auto delete_handoff_batch = +[](GraphicContext*, void* object)
	{
		std::unique_ptr<HandoffBatch> batch(static_cast<HandoffBatch*>(object));
		for (auto* native: batch->objects)
		{
			EXIT_IF(native == nullptr);
			++native->deleted;
		}
		++*batch->deleted_batches;
	};
	const auto retain_batch = [&](CommandRetirementCallbacks* owner, auto& objects)
	{
		auto batch = std::make_unique<HandoffBatch>();
		for (uint32_t index = 0; index < HANDOFF_OBJECTS; ++index) batch->objects[index] = &objects[index];
		batch->deleted_batches = &deleted_batches;
		owner->Add({batch.get(), delete_handoff_batch, nullptr, true});
		batch.release();
	};
	const auto deleted_exactly = [](const auto& objects, uint32_t count)
	{
		return std::all_of(objects.begin(), objects.end(), [count](const auto& object) { return object.deleted == count; });
	};
	CommandRetirementCallbacks handoff_callbacks;
	for (uint32_t recording = 0; recording < HANDOFF_BATCHES; ++recording)
	{
		auto& objects = handoff_objects[recording];
		retain_batch(&handoff_callbacks, objects);
		if (!handoff_callbacks.HasEntries() || !handoff_callbacks.Drain(nullptr) ||
		    handoff_callbacks.FenceResult(VK_TIMEOUT) || handoff_callbacks.FenceResult(VK_ERROR_DEVICE_LOST) ||
		    !handoff_callbacks.Drain(nullptr) || !deleted_exactly(objects, 0) || deleted_batches != recording ||
		    !handoff_callbacks.FenceResult(VK_SUCCESS) || !deleted_exactly(objects, 0) ||
		    !handoff_callbacks.Drain(nullptr) || !handoff_callbacks.Drain(nullptr) || handoff_callbacks.HasEntries() ||
		    !deleted_exactly(objects, 1) || deleted_batches != recording + 1u) return false;
	}

	// Discarding the later, unsubmitted recording cannot prove completion of
	// earlier same-queue consumers. Failed reset/fence results and a plain drain
	// preserve the whole cohort; successful device-idle reset releases it once.
	std::array<HandoffObject, HANDOFF_OBJECTS> discarded_objects {};
	retain_batch(&handoff_callbacks, discarded_objects);
	if (handoff_callbacks.ResetResult(false, VK_ERROR_DEVICE_LOST) || !handoff_callbacks.Drain(nullptr, true) ||
	    !handoff_callbacks.HasEntries() || !deleted_exactly(discarded_objects, 0) ||
	    !handoff_callbacks.ResetResult(false, VK_SUCCESS) || !handoff_callbacks.Drain(nullptr) ||
	    !handoff_callbacks.Drain(nullptr) || handoff_callbacks.FenceResult(VK_TIMEOUT) ||
	    !handoff_callbacks.Drain(nullptr) || !handoff_callbacks.HasEntries() ||
	    !deleted_exactly(discarded_objects, 0) || deleted_batches != HANDOFF_BATCHES ||
	    !handoff_callbacks.ResetResult(false, VK_SUCCESS) || !handoff_callbacks.Drain(nullptr, true) ||
	    !handoff_callbacks.Drain(nullptr, true) || handoff_callbacks.HasEntries() ||
	    !deleted_exactly(discarded_objects, 1) || deleted_batches != HANDOFF_BATCHES + 1u) return false;
	std::fprintf(stderr, "[GFX_HANDOFF_RETIREMENT_ROUTER_TEST] completed_batches=%u completed_objects=%u"
	    " batch_owner=actual-router failed_fence/reset=retained unsubmitted_discard=retained"
	    " device_idle_reset=single-release native_idle=modeled\n", HANDOFF_BATCHES, HANDOFF_BATCHES * HANDOFF_OBJECTS);
	// Upload slabs are exclusive to one recording, so a proved unsubmitted
	// discard may release them. Timeouts and failed resets cannot recycle them.
	UploadStagingPoolPolicy upload_pool;
	struct UploadPending { UploadStagingPoolPolicy* pool; UploadStagingPoolPolicy::Lease lease; uint32_t releases = 0; };
	UploadPending upload {&upload_pool, upload_pool.Reserve(UploadStagingPoolPolicy::SlabBytes)};
	const auto release_upload = +[](GraphicContext*, void* value) {
		auto* pending = static_cast<UploadPending*>(value);
		EXIT_IF(!pending->pool->Complete(pending->lease));
		++pending->releases;
	};
	CommandRetirementCallbacks upload_callbacks;
	upload_callbacks.Add({&upload, release_upload, nullptr, false});
	if (!upload_callbacks.Drain(nullptr) || upload_callbacks.FenceResult(VK_TIMEOUT) ||
	    upload_callbacks.ResetResult(false, VK_ERROR_DEVICE_LOST) || !upload_callbacks.Drain(nullptr) ||
	    !upload_pool.Busy(upload.lease) || upload_pool.Reset() || upload.releases != 0 ||
	    !upload_callbacks.FenceResult(VK_SUCCESS) || !upload_callbacks.Drain(nullptr) ||
	    !upload_callbacks.Drain(nullptr) || upload.releases != 1) return false;
	upload.lease = upload_pool.Acquire(upload.lease.slot);
	upload_callbacks.Add({&upload, release_upload, nullptr, false});
	if (!upload.lease.Valid() || !upload_callbacks.ResetResult(false, VK_SUCCESS) ||
	    !upload_callbacks.Drain(nullptr) || !upload_callbacks.Drain(nullptr) || upload.releases != 2 ||
	    !upload_pool.Reset()) return false;
	std::fprintf(stderr, "[GFX_UPLOAD_RETIREMENT_ROUTER_TEST] timeout/failed-reset=retained fence/discard=single-release ABA=guarded\n");
	return true;
}
#endif

VkCommandBuffer CommandBuffer::GetBufferForDraw() const
{
	EXIT_IF(IsInvalid() || !m_recording);
	m_native_commands = true;
	return m_pool->buffers[m_index];
}

void CommandBuffer::EndRetainedRenderPass() const
{
	EXIT_IF(m_render_pass_active);
	EXIT_IF(!m_retired->render_pass.Outside([&] {
		vkCmdEndRenderPass(m_pool->buffers[m_index]);
		if (RenderPassReuseTraceEnabled()) ++g_render_pass_reuse_counters.closed;
	}));
}

VkCommandBuffer CommandBuffer::GetBufferOutsideRenderPass() const
{
	EXIT_IF(IsInvalid() || !m_recording);
	m_native_commands = true;
	EndRetainedRenderPass();
	return m_pool->buffers[m_index];
}

void CommandBuffer::BeginRenderPass(VulkanFramebuffer* framebuffer, RenderColorInfo* color, RenderDepthInfo* depth,
                                  bool resources_read_only) const
{
	EXIT_IF(IsInvalid());
	EXIT_IF(!m_recording || m_render_pass_active);
	m_native_commands = true;

	auto* buffer = m_pool->buffers[m_index];

	EXIT_IF(framebuffer == nullptr);

	bool with_depth = (depth->format != VK_FORMAT_UNDEFINED && depth->vulkan_buffer != nullptr);
	bool with_color = (color->vulkan_buffer != nullptr);

	EXIT_NOT_IMPLEMENTED(!with_depth && !with_color);

	VkClearValue clears[2];
	clears[0].color        = {{0.0f, 0.0f, 0.0f, 1.0f}};
	clears[1].depthStencil = {depth->depth_clear_value, depth->stencil_clear_value};

	VkExtent2D extent = (with_color ? color->vulkan_buffer->extent : depth->vulkan_buffer->extent);
	const bool eligible = NativeRenderPassReuseEnabled() && resources_read_only && with_color &&
	                      !depth->depth_clear_enable && !depth->stencil_clear_enable &&
	                      color->vulkan_buffer->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
	                      (!with_depth || depth->vulkan_buffer->layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	const bool reuse = m_retired->render_pass.CanReuse(framebuffer->render_pass_id, extent.width, extent.height, eligible);
	if (!reuse) EndRetainedRenderPass();

	VkRenderPassBeginInfo render_pass_info {};
	render_pass_info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	render_pass_info.pNext             = nullptr;
	render_pass_info.renderPass        = framebuffer->render_pass;
	render_pass_info.framebuffer       = framebuffer->framebuffer;
	render_pass_info.renderArea.offset = {0, 0};
	render_pass_info.renderArea.extent = extent;
	render_pass_info.clearValueCount   = with_depth ? 2 : 1;
	render_pass_info.pClearValues      = clears;

	if (with_color && color->type == RenderColorType::DisplayBuffer)
	{
		// VideoOut command batches maintain a literal CA -> operation -> CA
		// invariant. Its layout field is intentionally not shared between the
		// render and presentation recording threads.
		EXIT_IF(color->vulkan_buffer->type != VulkanImageType::VideoOut);
	} else if (with_color && color->vulkan_buffer->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
	{
		VkImageMemoryBarrier image_memory_barrier {};
		image_memory_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		image_memory_barrier.pNext                           = nullptr;
		image_memory_barrier.srcAccessMask                   = VK_ACCESS_MEMORY_READ_BIT;
		image_memory_barrier.dstAccessMask                   = VK_ACCESS_MEMORY_WRITE_BIT;
		image_memory_barrier.oldLayout                       = color->vulkan_buffer->layout;
		image_memory_barrier.newLayout                       = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.image                           = color->vulkan_buffer->image;
		image_memory_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		image_memory_barrier.subresourceRange.baseMipLevel   = 0;
		image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
		image_memory_barrier.subresourceRange.baseArrayLayer = 0;
		image_memory_barrier.subresourceRange.layerCount     = 1;

		vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                     VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		                     &image_memory_barrier);

		color->vulkan_buffer->layout = image_memory_barrier.newLayout;
	}

	if (with_depth && depth->vulkan_buffer->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
	{
		VkImageMemoryBarrier image_memory_barrier {};
		image_memory_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		image_memory_barrier.pNext                           = nullptr;
		image_memory_barrier.srcAccessMask                   = VK_ACCESS_MEMORY_READ_BIT;
		image_memory_barrier.dstAccessMask                   = VK_ACCESS_MEMORY_WRITE_BIT;
		image_memory_barrier.oldLayout                       = depth->vulkan_buffer->layout;
		image_memory_barrier.newLayout                       = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.image                           = depth->vulkan_buffer->image;
		image_memory_barrier.subresourceRange.aspectMask     = UtilGetDepthStencilAspectMask(depth->vulkan_buffer->format);
		image_memory_barrier.subresourceRange.baseMipLevel   = 0;
		image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
		image_memory_barrier.subresourceRange.baseArrayLayer = 0;
		image_memory_barrier.subresourceRange.layerCount     = 1;

		vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                     VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		                     &image_memory_barrier);

		depth->vulkan_buffer->layout = image_memory_barrier.newLayout;
	}

	EXIT_IF(!m_retired->render_pass.Begin(framebuffer->render_pass_id, extent.width, extent.height, eligible,
	    [&] { vkCmdEndRenderPass(buffer); },
	    [&] { vkCmdBeginRenderPass(buffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE); }));
	RecordRenderPassBegin(reuse, eligible);
	m_render_pass_active = true;
}

void CommandBuffer::EndRenderPass() const
{
	EXIT_IF(IsInvalid());
	EXIT_IF(!m_recording || !m_render_pass_active);

	auto* buffer = m_pool->buffers[m_index];

	EXIT_IF(!m_retired->render_pass.End([&] {
		vkCmdEndRenderPass(buffer);
		if (RenderPassReuseTraceEnabled()) ++g_render_pass_reuse_counters.closed;
	}));
	m_render_pass_active = false;
}

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED
