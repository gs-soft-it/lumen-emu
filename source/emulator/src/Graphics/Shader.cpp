#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/CompiledProgramCache.h"

#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/File.h"
#include "Lumen/Core/MagicEnum.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/String8.h"
#include "Lumen/Core/Vector.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/ShaderColorExportContract.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Graphics/ShaderSourceRetry.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Profiler.h"
#include "../../../cpu/runtime/MemoryHandoff.h"

#include "spirv-tools/libspirv.h"
#include "spirv-tools/libspirv.hpp"
#include "spirv-tools/optimizer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <vector>
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX
#include <pthread.h>
#endif
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
#include "Lumen/Core/Threads.h"
#include <thread>
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#include <unistd.h>
#endif
#endif

//#define SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS
//#include "spirv_cross/spirv_glsl.hpp"

#ifdef LUMEN_EMU_ENABLED

LUMEN_ENUM_RANGE(Lumen::Libs::Graphics::ShaderInstructionType, 0, static_cast<int>(Lumen::Libs::Graphics::ShaderInstructionType::ZMax));

namespace Lumen::Libs::Graphics {

namespace {
struct OwnedShaderReadRange
{
	uint64_t guest = 0;
	const uint32_t* data = nullptr;
	uint64_t bytes = 0;
};
struct OwnedShaderReadContext
{
	std::vector<OwnedShaderReadRange> ranges;
	const ShaderCode* fetch = nullptr;
	uint64_t fetch_address = 0;
	mutable bool refused = false;
	const uint32_t* Resolve(uint64_t address, uint64_t bytes, bool host_only = false) const
	{
		if (address != 0 && bytes != 0 && address <= UINT64_MAX - bytes)
			for (const auto& range: ranges)
			{
				const auto host = reinterpret_cast<uint64_t>(range.data);
				for (const auto base: {host, host_only ? host : range.guest})
					if (base != 0 && address >= base && address - base <= range.bytes && bytes <= range.bytes - (address - base))
						return reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(range.data) + address - base);
			}
		refused = true;
		return nullptr;
	}
};
thread_local const OwnedShaderReadContext* g_owned_shader_reads = nullptr;
thread_local const ScopedShaderPrivateRead* g_private_shader_read = nullptr;
struct ScopedOwnedShaderReads
{
	const OwnedShaderReadContext* previous;
	explicit ScopedOwnedShaderReads(const OwnedShaderReadContext& reads): previous(g_owned_shader_reads)
	{
		g_owned_shader_reads = &reads;
	}
	~ScopedOwnedShaderReads() { g_owned_shader_reads = previous; }
};
static const uint32_t* ResolveOwnedShaderWords(const uint32_t* source, uint64_t bytes)
{
	return g_owned_shader_reads == nullptr ? source :
	    g_owned_shader_reads->Resolve(reinterpret_cast<uint64_t>(source), bytes);
}
}

ScopedShaderPrivateRead::ScopedShaderPrivateRead(const uint32_t* words, uint64_t bytes)
    : m_address(reinterpret_cast<uint64_t>(words)), m_bytes(bytes), m_previous(g_private_shader_read)
{
	g_private_shader_read = this;
}
ScopedShaderPrivateRead::~ScopedShaderPrivateRead()
{
	EXIT_IF(g_private_shader_read != this);
	g_private_shader_read = m_previous;
}

bool ShaderPrepareMemoryRead(uint64_t address, uint64_t bytes) noexcept
{
	if (address == 0 || bytes == 0 || address > UINT64_MAX - bytes) return false;
	// Strict owned analysis never falls through to a live guest read, renderer
	// callback, mapping query, or an admission acquired after the capture.
	if (g_owned_shader_reads != nullptr)
	{
		for (auto* range = g_private_shader_read; range != nullptr; range = range->m_previous)
			if (address >= range->m_address && address - range->m_address <= range->m_bytes &&
			    bytes <= range->m_bytes - (address - range->m_address)) return true;
		return g_owned_shader_reads->Resolve(address, bytes, true) != nullptr;
	}
	if (!GuestCpu::MemoryHandoffEnabled() || !GuestCpu::NeedsHandoff(address, bytes)) return true;
	return GraphicsRenderPrepareCpuMemoryAccess(address, bytes, GuestCpu::MemoryAccessIntent::Read);
}

struct ShaderBinaryInfo
{
	uint8_t  signature[7];
	uint8_t  version;
	uint32_t pssl_or_cg  : 1;
	uint32_t cached      : 1;
	uint32_t type        : 4;
	uint32_t source_type : 2;
	uint32_t length      : 24;
	uint8_t  chunk_usage_base_offset_dw;
	uint8_t  num_input_usage_slots;
	uint8_t  is_srt                 : 1;
	uint8_t  is_srt_used_info_valid : 1;
	uint8_t  is_extended_usage_info : 1;
	uint8_t  reserved2              : 5;
	uint8_t  reserved3;
	uint32_t hash0;
	uint32_t hash1;
	uint32_t crc32;
};

struct ShaderUsageSlot
{
	uint8_t type;
	uint8_t slot;
	uint8_t start_register;
	uint8_t flags;
};

struct ShaderUsageInfo
{
	const uint32_t*        usage_masks = nullptr;
	const ShaderUsageSlot* slots       = nullptr;
	int                    slots_num   = 0;
	bool                   valid       = false;
};

static thread_local const ShaderVertexSourceSnapshot* g_vertex_source_snapshot = nullptr;
static std::atomic<uint64_t> g_vertex_parse_cache_generation {0};

static const ShaderVertexSourceSnapshot* ShaderOwnedVertexSource(uint64_t address)
{
	return g_vertex_source_snapshot != nullptr && g_vertex_source_snapshot->Address() == address
	    ? g_vertex_source_snapshot : nullptr;
}

static const uint32_t* ShaderResolveVertexSource(const uint32_t* source)
{
	const auto* owned = ShaderOwnedVertexSource(reinterpret_cast<uint64_t>(source));
	return owned != nullptr ? owned->Data() : source;
}

ScopedShaderVertexSource::ScopedShaderVertexSource(const ShaderVertexSourceSnapshot* source)
	: m_source(source), m_previous(g_vertex_source_snapshot)
{
	EXIT_IF(source != nullptr && (source->Address() == 0 || source->Data() == nullptr));
	g_vertex_source_snapshot = source;
}

ScopedShaderVertexSource::~ScopedShaderVertexSource()
{
	EXIT_IF(g_vertex_source_snapshot != m_source);
	g_vertex_source_snapshot = m_previous;
}

bool ShaderVertexSourceSnapshot::Capture(uint64_t address, bool* retryable)
{
	if (retryable != nullptr) *retryable = false;
	m_address = 0;
	m_code_words = 0;
	m_bytes = 0;
	m_words = {};
	using namespace Core::VirtualMemory;
	GuestDataLease prefix_lease;
	GuestDataLeaseDenialMetadata denial;
	if (!ShaderPrepareMemoryRead(address, 8)) return false;
	if (!TryAcquireGuestDataLease(address, 8, Mode::Read, &prefix_lease, false, &denial))
	{
		if (retryable != nullptr) *retryable = denial.retryable;
		return false;
	}
	uint32_t prefix[2];
	std::memcpy(prefix, reinterpret_cast<const void*>(prefix_lease.HostBegin()), sizeof(prefix));
	static_assert(sizeof(ShaderBinaryInfo) % sizeof(uint32_t) == 0);
	const uint64_t code_bytes = (static_cast<uint64_t>(prefix[1]) + 1u) * 8u;
	// The cache's 64 KiB budget is not a shader-format limit. Admit the
	// encoded span when it fits the decoder's uint32 byte PCs and native size types.
	const uint64_t bytes = code_bytes + sizeof(ShaderBinaryInfo);
	if (prefix[0] != 0xbeeb03ffu || code_bytes > UINT32_MAX ||
	    bytes > SIZE_MAX || address > UINT64_MAX - bytes) return false;
	GuestDataLease full_lease;
	if (!ShaderPrepareMemoryRead(address, bytes)) return false;
	// Retaining the prefix lease keeps the same native admission across both
	// proofs. Neither source authority nor guest contents survive this capture.
	const uint32_t* current = nullptr;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	static const bool kernel_copy = [] {
		const char* value = std::getenv("LUMEN_SHADER_SNAPSHOT_KERNEL_COPY");
		const bool selected = value != nullptr && std::strcmp(value, "1") == 0;
		std::fprintf(stderr, "[GFX_SHADER_SNAPSHOT_KERNEL_COPY_CONFIG] selected=%u private_cap=%zu fallback=tag-mte-size\n",
		    selected ? 1u : 0u, PRIVATE_GUEST_DATA_COPY_LIMIT);
		return selected;
	}();
	if (kernel_copy && bytes <= PRIVATE_GUEST_DATA_COPY_LIMIT)
	{
		struct PrivateScratch
		{
			std::unique_ptr<uint32_t[]> words;
			size_t capacity = 0;
			uint32_t* Prepare(size_t count)
			{
				if (count > capacity)
				{
					// Drop the previous allocation before growth, retaining at most
					// 2 MiB scratch per worker even during replacement.
					words.reset();
					capacity = 0;
					words.reset(new (std::nothrow) uint32_t[count]);
					if (words) capacity = count;
				}
				return words.get();
			}
		};
		thread_local PrivateScratch scratch;
		if (auto* output = scratch.Prepare(static_cast<size_t>(bytes) / sizeof(uint32_t)))
		{
			const auto copied = CopyGuestDataToPrivateScratch(address, output, bytes, &denial);
			// On denial the scratch may contain old or partially copied bytes.
			// Never inspect it, publish it, cache it or retry with a raw pointer.
			if (copied == PrivateGuestDataCopyResult::Denied) return false;
			if (copied == PrivateGuestDataCopyResult::Copied)
			{
				current = output;
				thread_local uint64_t copies = 0;
				if (copies != UINT64_MAX) ++copies;
				if ((copies & (copies - 1)) == 0)
					std::fprintf(stderr, "[GFX_SHADER_SNAPSHOT_KERNEL_COPY] worker_copies=%" PRIu64 " bytes=%" PRIu64 "\n",
					    copies, bytes);
			}
		}
	}
#endif
	if (current == nullptr)
	{
		// Unsupported tag/MTE/size cases and scratch allocation failure retain
		// the ordinary full-range proof and its native tag-check semantics.
		if (!TryAcquireGuestDataLease(address, bytes, Mode::Read, &full_lease, false)) return false;
		current = reinterpret_cast<const uint32_t*>(full_lease.HostBegin());
	}
	thread_local ShaderSourceSnapshotCache cache;
	auto candidate = cache.CaptureWithPrefix(address, current, static_cast<size_t>(bytes), prefix,
	    g_vertex_parse_cache_generation.load(std::memory_order_relaxed));
	if (!candidate) return false;
	m_words = std::move(candidate);
	m_bytes = static_cast<size_t>(bytes);
	m_code_words = static_cast<uint32_t>(code_bytes / sizeof(uint32_t));
	m_address = address;
	return true;
}

struct ShaderParsedUsage
{
	bool fetch                     = false;
	int  fetch_reg                 = 0;
	bool vertex_buffer             = false;
	int  vertex_buffer_reg         = 0;
	int  storage_buffers_readwrite = 0;
	int  storage_buffers_readonly  = 0;
	int  storage_buffers_constant  = 0;
	int  textures2D_readonly       = 0;
	int  textures2D_readwrite      = 0;
	bool extended_buffer           = false;
	int  samplers                  = 0;
	int  gds_pointers              = 0;
	int  direct_sgprs              = 0;
};

struct ShaderDebugPrintfCmds
{
	uint64_t                  id = 0;
	Vector<ShaderDebugPrintf> cmds;
};

static Vector<uint64_t>*              g_disabled_shaders = nullptr;
static Vector<ShaderDebugPrintfCmds>* g_debug_printfs    = nullptr;
static std::atomic_uint               g_dx10_clamp_zero_receipts {0};

static bool ShaderTextureDescriptorSupported(const char* stage, const ShaderTextureDescriptor& desc);

static constexpr bool ShaderSamplerClampSupported(uint8_t clamp)
{
	return clamp == 0 || clamp == 1 || clamp == 2 || clamp == 6;
}

static bool ShaderSamplerUnknownProvenanceDescriptorSupported(const ShaderSamplerResource& sampler)
{
	const bool anisotropic = sampler.XyMagFilter() >= 2 || sampler.XyMinFilter() >= 2;
	return (sampler.fields[0] & 0x80000000u) == 0 && (sampler.fields[2] & 0xf0000000u) == 0 && sampler.fields[3] == 0 &&
	       ShaderSamplerClampSupported(sampler.ClampX()) && ShaderSamplerClampSupported(sampler.ClampY()) &&
	       ShaderSamplerClampSupported(sampler.ClampZ()) && (!anisotropic || sampler.MaxAnisoRatio() <= 4) &&
	       sampler.DepthCompareFunc() == 0 && !sampler.ForceUnormCoords() && sampler.AnisoThreshold() == 0 &&
	       !sampler.McCoordTrunc() && !sampler.ForceDegamma() && sampler.AnisoBias() == 0 && !sampler.TruncCoord() &&
	       !sampler.DisableCubeWrap() && sampler.FilterMode() == 0 && sampler.MinLod() <= sampler.MaxLod() && sampler.LodBias() == 0 &&
	       sampler.PerfMip() == 0 && sampler.PerfZ() == 0 && sampler.LodBiasSec() == 0 && sampler.ZFilter() == 1 &&
	       (sampler.MipFilter() == 0 || sampler.MipFilter() == 2) && sampler.BorderColorPtr() == 0 && sampler.BorderColorType() == 0;
}

static bool ShaderSamplerDirectSgprsSupported(const HW::UserSgprInfo& user_sgpr, int start_index,
	                                           int user_sgpr_num, const ShaderSamplerResource& sampler, bool* unknown_fallback)
{
	if (unknown_fallback != nullptr)
	{
		*unknown_fallback = false;
	}
	if (start_index < 0 || start_index > HW::UserSgprInfo::SGPRS_MAX - 4)
	{
		return false;
	}
	bool marked      = true;
	bool all_unknown = true;
	for (int index = 0; index < 4; ++index)
	{
		const auto type = user_sgpr.type[start_index + index];
		marked &= type == HW::UserSgprType::Vsharp || type == HW::UserSgprType::Region;
		all_unknown &= type == HW::UserSgprType::Unknown;
	}
	// Region/Vsharp is the established path. The all-Unknown fallback is new and
	// must not consume values outside either the hardware-declared or observed
	// SGPR range.
	const bool fallback_range = user_sgpr_num >= 0 && user_sgpr_num <= HW::UserSgprInfo::SGPRS_MAX &&
	                            user_sgpr.count <= HW::UserSgprInfo::SGPRS_MAX && start_index <= user_sgpr_num - 4 &&
	                            static_cast<uint32_t>(start_index + 4) <= user_sgpr.count;
	const bool fallback = all_unknown && fallback_range && ShaderSamplerUnknownProvenanceDescriptorSupported(sampler);
	if (unknown_fallback != nullptr)
	{
		*unknown_fallback = !marked && fallback;
	}
	return marked || fallback;
}

static CompiledProgramCache& ShaderCompiledPrograms()
{
	static CompiledProgramCache cache;
	return cache;
}

static bool ShaderCompiledProgramsEnabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_GPU_COMPILED_PROGRAMS");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

static bool ShaderParallelCompilationEnabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_GPU_PARALLEL_COMPILATION");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

void ShaderInit() {}

bool ShaderResetForNextSession()
{
	ShaderCompiledPrograms().Reset();
	g_vertex_parse_cache_generation.fetch_add(1, std::memory_order_relaxed);
	// Disable and debug-injection policies are supplied by the per-title script
	// and must not leak into the next one.
	if (g_disabled_shaders != nullptr)
	{
		g_disabled_shaders->Clear();
	}
	if (g_debug_printfs != nullptr)
	{
		g_debug_printfs->Clear();
	}
	return true;
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool ShaderSessionResetContractTest()
{
	ShaderDisable(0x12345678u);
	ShaderInjectDebugPrintf(0x12345678u, ShaderDebugPrintf {});
	if (g_disabled_shaders == nullptr || g_disabled_shaders->IsEmpty() || g_debug_printfs == nullptr || g_debug_printfs->IsEmpty())
	{
		return false;
	}
	return ShaderResetForNextSession() && g_disabled_shaders->IsEmpty() && g_debug_printfs->IsEmpty();
}

bool ShaderNullSampledImageContractTest()
{
	ShaderTextureDescriptor null_sampled {};
	null_sampled.usage = ShaderTextureUsage::ReadOnly;

	ShaderTextureDescriptor ordinary {};
	ordinary.usage             = ShaderTextureUsage::ReadOnly;
	ordinary.texture.fields[3] = 9u << 28u;
	ShaderTextureDescriptor type8           = ordinary;
	type8.texture.fields[1]                 = (3u << 30u) | (10u << 20u) | (1u << 6u);
	type8.texture.fields[2]                 = 1u;
	type8.texture.fields[3]                 = (ShaderTextureResource::TYPE_8_COMPAT << 28u) | (8u << 20u) | 0xfacu;
	type8.texture.fields[4]                 = 1u << 13u;
	ShaderTextureDescriptor pixel_type8     = type8;
	pixel_type8.texture.fields[1]            = 10u << 20u;
	pixel_type8.texture.fields[3] |= 1u << 26u;
	ShaderTextureDescriptor malformed_type8 = pixel_type8;
	malformed_type8.texture.fields[2]       = 1u << 14u;
	ShaderTextureDescriptor reserved_type8  = pixel_type8;
	reserved_type8.texture.fields[7]        = 1u;
	ShaderTextureDescriptor storage_type8   = pixel_type8;
	storage_type8.usage                     = ShaderTextureUsage::ReadWrite;
	ShaderTextureDescriptor samplerless_type8 = pixel_type8;
	samplerless_type8.textures2d_without_sampler = true;

	ShaderTextureDescriptor storage = null_sampled;
	storage.usage                    = ShaderTextureUsage::ReadWrite;
	ShaderTextureDescriptor unknown = null_sampled;
	unknown.usage                    = ShaderTextureUsage::Unknown;

	ShaderTextureDescriptor sampled_without_sampler = null_sampled;
	sampled_without_sampler.textures2d_without_sampler = true;

	ShaderTextureResources resources {};
	resources.textures_num = 1;
	resources.desc[0]      = null_sampled;
	if (!null_sampled.IsNullSampledImage() || !resources.HasNullSampledImage() || ordinary.IsNullSampledImage() ||
	    storage.IsNullSampledImage() || unknown.IsNullSampledImage() || sampled_without_sampler.IsNullSampledImage())
	{
		return false;
	}

	for (auto& field: resources.desc[0].texture.fields)
	{
		field = 1;
		if (resources.desc[0].IsNullSampledImage() || resources.HasNullSampledImage())
		{
			return false;
		}
		field = 0;
	}

	const bool enabled = ShaderNullSampledImageEnabled();
	if (!ShaderTextureDescriptorSupported("vs", ordinary) || !ShaderTextureDescriptorSupported("ps", ordinary) ||
	    !ShaderTextureDescriptorSupported("cs", type8) || ShaderTextureDescriptorSupported("vs", type8) ||
	    ShaderTextureDescriptorSupported("ps", type8) || ShaderTextureDescriptorSupported("cs", pixel_type8) ||
	    !ShaderTextureDescriptorSupported("ps", pixel_type8) || ShaderTextureDescriptorSupported("cs", malformed_type8) ||
	    ShaderTextureDescriptorSupported("ps", malformed_type8) || ShaderTextureDescriptorSupported("cs", reserved_type8) ||
	    ShaderTextureDescriptorSupported("ps", reserved_type8) || ShaderTextureDescriptorSupported("ps", storage_type8) ||
	    ShaderTextureDescriptorSupported("ps", samplerless_type8) || ShaderTextureDescriptorSupported("vs", null_sampled) ||
	    ShaderTextureDescriptorSupported("cs", null_sampled) || ShaderTextureDescriptorSupported("ps", null_sampled) != enabled)
	{
		return false;
	}

	ShaderCode sample_code;
	ShaderInstruction sample {};
	sample.type = ShaderInstructionType::ImageSample;
	sample_code.GetInstructions().Add(sample);
	if (ShaderCodeHasImageLoad(sample_code) || ShaderNullSampledImageLoadUnsupported(resources, true, false))
	{
		return false;
	}
	ShaderInstruction load {};
	load.type = ShaderInstructionType::ImageLoad;
	sample_code.GetInstructions().Add(load);
	if (!ShaderCodeHasImageLoad(sample_code) || ShaderNullSampledImageLoadUnsupported(resources, true, true) != enabled ||
	    ShaderNullSampledImageLoadUnsupported(resources, false, true))
	{
		return false;
	}
	ShaderCode mip_load_code;
	ShaderInstruction mip_load {};
	mip_load.type = ShaderInstructionType::ImageLoadMip;
	mip_load_code.GetInstructions().Add(mip_load);
	if (!ShaderCodeHasImageLoad(mip_load_code))
	{
		return false;
	}
	return true;
}
#endif

bool ShaderNullSampledImageEnabled()
{
	static const bool enabled = []
	{
		const char* value = std::getenv("LUMEN_GFX_NULL_SAMPLED_IMAGE");
		if (value == nullptr)
		{
#if LUMEN_GFX_NULL_SAMPLED_IMAGE_DEFAULT
			return true;
#else
			return false;
#endif
		}
		return value[0] == '1' && value[1] == '\0';
	}();
	return enabled;
}

bool ShaderDx10ClampZeroExperimentEnabled()
{
	static const bool enabled = []
	{
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_GFX_DX10_CLAMP_ZERO");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	return enabled;
}

bool ShaderImplicitSampleLodZeroExperimentEnabled()
{
	static const bool enabled = []
	{
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_GFX_IMPLICIT_SAMPLE_LOD_ZERO");
		const bool selected = value != nullptr && value[0] == '1' && value[1] == '\0';
		if (selected)
		{
			std::fprintf(stderr,
			             "[GFX_IMPLICIT_SAMPLE_LOD_ZERO] selector=1 implicit=explicit_lod0 explicit=unchanged "
			             "scope=process status=diagnostic\n");
			std::fflush(stderr);
		}
		return selected;
	}();
	return enabled;
}

static void ShaderDx10ClampZeroReceipt(const char* stage, uint32_t stage_bit)
{
	const uint32_t previous = g_dx10_clamp_zero_receipts.fetch_or(stage_bit, std::memory_order_relaxed);
	if ((previous & stage_bit) == 0)
	{
		std::fprintf(stderr, "[DX10_CLAMP_V3] selector=1 stage=%s dx10_clamp=0 action=pass-through optimizer=none\n", stage);
		std::fflush(stderr);
	}
}

bool ShaderCodeHasImageLoad(const ShaderCode& code)
{
	return code.HasAnyOf({ShaderInstructionType::ImageLoad, ShaderInstructionType::ImageLoadMip});
}

bool ShaderCodeHasStorageBufferWrites(const ShaderCode& code)
{
	return code.HasAnyOf({ShaderInstructionType::BufferStoreDword, ShaderInstructionType::BufferStoreFormatX,
	                      ShaderInstructionType::BufferStoreFormatXy});
}

static bool StorageAuthorityPlain(const ShaderOperand& operand)
{
	return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp;
}

static bool StorageAuthorityRegister(const ShaderOperand& operand, int width, bool vector = false)
{
	if (!StorageAuthorityPlain(operand) || operand.size != width || operand.constant.u != 0) return false;
	if (operand.type == ShaderOperandType::Vgpr)
		return vector && operand.register_id >= 0 && operand.register_id <= 256 - width;
	if (vector) return false;
	if (operand.type == ShaderOperandType::Sgpr)
		return operand.register_id >= 0 && operand.register_id <= 104 - width;
	return operand.register_id == 0 && ((width == 1 && (operand.type == ShaderOperandType::VccLo ||
	    operand.type == ShaderOperandType::VccHi || operand.type == ShaderOperandType::ExecLo ||
	    operand.type == ShaderOperandType::ExecHi || operand.type == ShaderOperandType::M0)) ||
	    (width == 2 && (operand.type == ShaderOperandType::VccLo || operand.type == ShaderOperandType::ExecLo)));
}

static bool StorageAuthorityConstant(const ShaderOperand& operand)
{
	return StorageAuthorityPlain(operand) && operand.size == 0 && operand.register_id == 0 &&
	       (operand.type == ShaderOperandType::IntegerInlineConstant || operand.type == ShaderOperandType::LiteralConstant);
}

static bool StorageAuthorityValue(const ShaderOperand& operand)
{
	return StorageAuthorityConstant(operand) || StorageAuthorityRegister(operand, 1) || StorageAuthorityRegister(operand, 1, true);
}

static bool StorageAuthorityUnused(const ShaderOperand& operand)
{
	return StorageAuthorityPlain(operand) && operand.type == ShaderOperandType::Unknown &&
	       operand.constant.u == 0 && operand.register_id == 0 && operand.size == 0;
}

static bool StorageAuthorityBranch(ShaderInstructionType type)
{
	return type == ShaderInstructionType::SBranch || type == ShaderInstructionType::SCbranchExecz ||
	       type == ShaderInstructionType::SCbranchScc0 || type == ShaderInstructionType::SCbranchScc1 ||
	       type == ShaderInstructionType::SCbranchVccz || type == ShaderInstructionType::SCbranchVccnz;
}

// Deliberately bounded effect model. Every admitted lowerer either writes its
// explicit destinations or changes only SCC/EXEC; none calls external code or
// creates another storage-buffer write. New instruction classes need an audit.
static bool StorageAuthorityInstruction(const ShaderInstruction& inst)
{
	if (inst.src_num < 0 || inst.src_num > 4 || inst.globally_coherent || inst.unnormalized_coordinates ||
	    inst.export_valid_mask || inst.export_done || inst.export_target != 0xff) return false;
	// SOPP decoding retains its canonical zero SIMM operand for S_ENDPGM,
	// although the terminal's public source count is zero.
	const bool terminal = inst.type == ShaderInstructionType::SEndpgm;
	// VOP3 decoding retains the encoded zero third source as plain s0 even
	// for these two-source forms. Their audited native lowerers never read it.
	// Permit only that exact residue, never an arbitrary ignored operand.
	const bool two_source_vop3 = inst.src_num == 2 && inst.src[2].type == ShaderOperandType::Sgpr &&
	    inst.src[2].register_id == 0 && StorageAuthorityRegister(inst.src[2], 1) &&
	    (((inst.type == ShaderInstructionType::VCmpxGtU32 || inst.type == ShaderInstructionType::VCmpxGeU32) &&
	       inst.format == ShaderInstructionFormat::SmaskVsrc0Vsrc1) ||
	     ((inst.type == ShaderInstructionType::VAddI32 || inst.type == ShaderInstructionType::VSubI32 ||
	       inst.type == ShaderInstructionType::VSubrevI32) && inst.format == ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1) ||
	     (inst.type == ShaderInstructionType::VAndB32 && inst.format == ShaderInstructionFormat::SVdstSVsrc0SVsrc1));
	for (int i = terminal ? 1 : inst.src_num; i < 4; ++i)
		if (!(i == 2 && two_source_vop3) && !StorageAuthorityUnused(inst.src[i])) return false;
	if (StorageAuthorityBranch(inst.type))
		return inst.format == ShaderInstructionFormat::Label && inst.src_num == 1 &&
		       inst.src[0].type == ShaderOperandType::LiteralConstant && StorageAuthorityConstant(inst.src[0]) &&
		       StorageAuthorityUnused(inst.dst) && StorageAuthorityUnused(inst.dst2);
	if (inst.type == ShaderInstructionType::SEndpgm)
		return inst.format == ShaderInstructionFormat::Empty && inst.src_num == 0 &&
		       (StorageAuthorityUnused(inst.src[0]) || (inst.src[0].type == ShaderOperandType::LiteralConstant &&
		       StorageAuthorityConstant(inst.src[0]) && inst.src[0].constant.u == 0)) &&
		       StorageAuthorityUnused(inst.dst) && StorageAuthorityUnused(inst.dst2);
	if (inst.type == ShaderInstructionType::SWaitcnt)
		return inst.format == ShaderInstructionFormat::Imm && inst.src_num == 1 &&
		       inst.src[0].type == ShaderOperandType::LiteralConstant && StorageAuthorityConstant(inst.src[0]) &&
		       StorageAuthorityUnused(inst.dst) && StorageAuthorityUnused(inst.dst2);
	if (inst.type == ShaderInstructionType::SMovB32)
		return inst.format == ShaderInstructionFormat::SVdstSVsrc0 && inst.src_num == 1 &&
		       StorageAuthorityRegister(inst.dst, 1) && StorageAuthorityValue(inst.src[0]) && StorageAuthorityUnused(inst.dst2);
	if (inst.type == ShaderInstructionType::SMovB64)
		return inst.format == ShaderInstructionFormat::Sdst2Ssrc02 && inst.src_num == 1 &&
		       StorageAuthorityRegister(inst.dst, 2) && (StorageAuthorityRegister(inst.src[0], 2) ||
		       StorageAuthorityConstant(inst.src[0])) && StorageAuthorityUnused(inst.dst2);
	if (inst.type == ShaderInstructionType::SLshlB32)
		return inst.format == ShaderInstructionFormat::SVdstSVsrc0SVsrc1 && inst.src_num == 2 &&
		       StorageAuthorityRegister(inst.dst, 1) && StorageAuthorityValue(inst.src[0]) &&
		       StorageAuthorityValue(inst.src[1]) && StorageAuthorityUnused(inst.dst2);
	if (inst.type == ShaderInstructionType::VAndB32)
		return inst.format == ShaderInstructionFormat::SVdstSVsrc0SVsrc1 && inst.src_num == 2 &&
		       StorageAuthorityRegister(inst.dst, 1, true) && StorageAuthorityValue(inst.src[0]) &&
		       StorageAuthorityValue(inst.src[1]) && StorageAuthorityUnused(inst.dst2);
	if (inst.type == ShaderInstructionType::VAddI32 || inst.type == ShaderInstructionType::VSubI32 ||
	    inst.type == ShaderInstructionType::VSubrevI32)
		return inst.format == ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1 && inst.src_num == 2 &&
		       StorageAuthorityRegister(inst.dst, 1, true) && StorageAuthorityRegister(inst.dst2, 2) &&
		       inst.dst2.type != ShaderOperandType::ExecLo && StorageAuthorityValue(inst.src[0]) && StorageAuthorityValue(inst.src[1]);
	if (inst.type == ShaderInstructionType::VCmpxGtU32 || inst.type == ShaderInstructionType::VCmpxGeU32)
		return inst.format == ShaderInstructionFormat::SmaskVsrc0Vsrc1 && inst.src_num == 2 &&
		       StorageAuthorityRegister(inst.dst, 2) && inst.dst.type != ShaderOperandType::ExecLo &&
		       StorageAuthorityValue(inst.src[0]) && StorageAuthorityValue(inst.src[1]) && StorageAuthorityUnused(inst.dst2);
	int width = 0;
	ShaderInstructionFormat::Format format = ShaderInstructionFormat::Unknown;
	switch (inst.type)
	{
		case ShaderInstructionType::SBufferLoadDword: width = 1; format = ShaderInstructionFormat::SdstSvSoffset; break;
		case ShaderInstructionType::SBufferLoadDwordx2: width = 2; format = ShaderInstructionFormat::Sdst2SvSoffset; break;
		case ShaderInstructionType::SBufferLoadDwordx4: width = 4; format = ShaderInstructionFormat::Sdst4SvSoffset; break;
		case ShaderInstructionType::SBufferLoadDwordx8: width = 8; format = ShaderInstructionFormat::Sdst8SvSoffset; break;
		case ShaderInstructionType::SBufferLoadDwordx16: width = 16; format = ShaderInstructionFormat::Sdst16SvSoffset; break;
		default: break;
	}
	if (width != 0)
		return inst.format == format && inst.src_num == 2 && inst.dst.type == ShaderOperandType::Sgpr &&
		       StorageAuthorityRegister(inst.dst, width) && inst.src[0].type == ShaderOperandType::Sgpr &&
		       StorageAuthorityRegister(inst.src[0], 4) && StorageAuthorityConstant(inst.src[1]) && StorageAuthorityUnused(inst.dst2);
	switch (inst.type)
	{
		case ShaderInstructionType::BufferLoadDword: case ShaderInstructionType::BufferLoadFormatX:
		case ShaderInstructionType::BufferStoreDword: case ShaderInstructionType::BufferStoreFormatX:
			width = 1; format = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen; break;
		case ShaderInstructionType::BufferStoreFormatXy:
			width = 2; format = ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen; break;
		case ShaderInstructionType::BufferLoadFormatXyzw:
			width = 4; format = ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen; break;
		default: return false;
	}
	return inst.format == format && inst.src_num == 3 && StorageAuthorityRegister(inst.dst, width, true) &&
	       StorageAuthorityRegister(inst.src[0], 1, true) && inst.src[1].type == ShaderOperandType::Sgpr &&
	       StorageAuthorityRegister(inst.src[1], 4) && StorageAuthorityConstant(inst.src[2]) && StorageAuthorityUnused(inst.dst2);
}

ShaderStorageWriteAuthority ShaderClassifyStorageBufferWrites(const ShaderCode& code, ShaderType expected_stage,
                                                               const ShaderBindResources& bind)
{
	const auto& storage = bind.storage_buffers;
	const auto& insts = code.GetInstructions();
	const auto reject = [&](const char* reason, uint32_t index = UINT32_MAX) -> ShaderStorageWriteAuthority {
		static const bool trace = [] {
			const char* value = std::getenv("LUMEN_GFX_EOP_RESOURCE_TRACE");
			return value != nullptr && value[0] == '1' && value[1] == '\0';
		}();
		static std::atomic<uint32_t> records {0};
		auto record = records.load(std::memory_order_relaxed);
		if (trace)
			while (record < 16 && !records.compare_exchange_weak(record, record + 1u, std::memory_order_relaxed)) {}
		if (trace && record < 16)
		{
			std::fprintf(stderr, "[GFX_STORAGE_WRITE_AUTHORITY_REJECT] reason=%s stage=%u code_stage=%u shader=%08" PRIx32 ":%08" PRIx32
			                     " instructions=%u index=%u storage=%d textures=%d samplers=%d direct=%d gds=%d extended=%u push_bytes=%u"
			                     " labels=%u indirect_labels=%u metadata_only=1 admission=unchanged\n",
			             reason, static_cast<unsigned>(expected_stage), static_cast<unsigned>(code.GetType()), code.GetHash0(), code.GetCrc32(),
			             insts.Size(), index, storage.buffers_num, bind.textures2D.textures_num, bind.samplers.samplers_num,
			             bind.direct_sgprs.sgprs_num, bind.gds_pointers.pointers_num, bind.extended.used ? 1u : 0u, bind.push_constant_size,
			             code.GetLabels().Size(), code.GetIndirectLabels().Size());
			if (index < insts.Size())
			{
				const auto& inst = insts.At(index);
				std::fprintf(stderr, "[GFX_STORAGE_WRITE_AUTHORITY_IR] index=%u pc=%08" PRIx32 " opcode=%u format=%016" PRIx64
				                     " sources=%d coherent=%u unnormalized=%u export_valid=%u export_done=%u export_target=%u",
				             index, inst.pc, static_cast<unsigned>(inst.type), static_cast<uint64_t>(inst.format), inst.src_num,
				             inst.globally_coherent ? 1u : 0u, inst.unnormalized_coordinates ? 1u : 0u,
				             inst.export_valid_mask ? 1u : 0u, inst.export_done ? 1u : 0u, inst.export_target);
				const ShaderOperand* operands[] = {&inst.dst, &inst.dst2, &inst.src[0], &inst.src[1], &inst.src[2], &inst.src[3]};
				for (unsigned op = 0; op < 6; ++op)
				{
					const auto& operand = *operands[op];
					std::fprintf(stderr, " op%u={type=%u,reg=%d,size=%d,constant_nonzero=%u,abs=%u,neg=%u,clamp=%u,multiplier=%g}",
					             op, static_cast<unsigned>(operand.type), operand.register_id, operand.size, operand.constant.u != 0 ? 1u : 0u,
					             operand.absolute ? 1u : 0u, operand.negate ? 1u : 0u, operand.clamp ? 1u : 0u,
					             static_cast<double>(operand.multiplier));
				}
				std::fprintf(stderr, " metadata_only=1 admission=unchanged\n");
			}
		}
		return {};
	};
	if ((expected_stage != ShaderType::Vertex && expected_stage != ShaderType::Pixel && expected_stage != ShaderType::Compute) ||
	    code.GetType() != expected_stage || code.IsVsEmbedded() || code.IsPsEmbedded() || insts.IsEmpty() ||
	    storage.buffers_num < 0 || storage.buffers_num > ShaderStorageResources::BUFFERS_MAX) return reject("stage-or-ir");
	const uint32_t all = (1u << static_cast<unsigned>(storage.buffers_num)) - 1u;
	if (!ShaderCodeHasStorageBufferWrites(code)) return {all, 0, true};
	if (expected_stage != ShaderType::Compute || insts.Size() > 16384 || bind.extended.used ||
	    bind.gds_pointers.pointers_num != 0 || bind.textures2D.textures_num < 0 ||
	    bind.textures2D.textures_num > ShaderTextureResources::RES_MAX || bind.samplers.samplers_num < 0 ||
	    bind.samplers.samplers_num > ShaderSamplerResources::RES_MAX || bind.direct_sgprs.sgprs_num < 0 ||
	    bind.direct_sgprs.sgprs_num > ShaderDirectSgprsResources::SGPRS_MAX) return reject("mixed-profile-layout");
	// Match the native prologue: workgroup initialization precedes resources;
	// storage, textures, samplers and direct scalars then write these lanes.
	// Reject declaration collisions instead of guessing which value wins.
	bool initialized[16] = {};
	const auto declare = [&](int first, int width, bool extended) {
		if (extended || first < 0 || first > 16 - width) return false;
		for (int lane = 0; lane < width; ++lane) if (initialized[first + lane]) return false;
		for (int lane = 0; lane < width; ++lane) initialized[first + lane] = true;
		return true;
	};
	for (int i = 0; i < storage.buffers_num; ++i)
		if (!declare(storage.start_register[i], 4, storage.extended[i])) return reject("storage-prologue");
	for (int i = 0; i < bind.textures2D.textures_num; ++i)
		if (!declare(bind.textures2D.desc[i].start_register, 8, bind.textures2D.desc[i].extended)) return reject("texture-prologue");
	for (int i = 0; i < bind.samplers.samplers_num; ++i)
		if (!declare(bind.samplers.start_register[i], 4, bind.samplers.extended[i])) return reject("sampler-prologue");
	for (int i = 0; i < bind.direct_sgprs.sgprs_num; ++i)
		if (!declare(bind.direct_sgprs.start_register[i], 1, false)) return reject("scalar-prologue");
	const uint64_t prologue_size = uint64_t{16} * (storage.buffers_num + 2 * bind.textures2D.textures_num +
	    bind.samplers.samplers_num + (bind.direct_sgprs.sgprs_num + 3) / 4);
	if (bind.push_constant_size != prologue_size) return reject("prologue-size");
	bool written_sgprs[104] = {};
	uint32_t writes = 0;
	const auto instruction_index = [&](int64_t pc) {
		for (uint32_t i = 0; i < insts.Size(); ++i) if (insts.At(i).pc == pc) return i;
		return UINT32_MAX;
	};
	for (uint32_t i = 0; i < insts.Size(); ++i)
	{
		const auto& inst = insts.At(i);
		if ((inst.pc & 3u) != 0 || (i == 0 && inst.pc != 0) ||
		    (i > 0 && inst.pc <= insts.At(i - 1).pc)) return reject("instruction-order", i);
		if (!StorageAuthorityInstruction(inst)) return reject("instruction-profile", i);
		for (const auto* dst: {&inst.dst, &inst.dst2})
			if (dst->type == ShaderOperandType::Sgpr)
				for (int lane = 0; lane < dst->size; ++lane) written_sgprs[dst->register_id + lane] = true;
		if (StorageAuthorityBranch(inst.type))
		{
			const int64_t target = int64_t{inst.pc} + 4 + inst.src[0].constant.i;
			if (instruction_index(target) == UINT32_MAX || i + 1 == insts.Size()) return reject("branch-target", i);
			bool declared_target = false;
			for (const auto& label: code.GetLabels())
				declared_target |= !label.IsDisabled() && label.GetSrc() == inst.pc && label.GetDst() == target;
			if (!declared_target) return reject("branch-metadata", i);
			if (inst.type != ShaderInstructionType::SBranch)
			{
				bool declared_fallthrough = false;
				for (const auto& label: code.GetIndirectLabels())
					declared_fallthrough |= !label.IsDisabled() && label.GetSrc() == inst.pc && label.GetDst() == insts.At(i + 1).pc;
				if (!declared_fallthrough) return reject("branch-fallthrough", i);
			}
		}
		if (inst.type == ShaderInstructionType::SEndpgm && i + 1 != insts.Size()) return reject("early-terminal", i);
		if (inst.type != ShaderInstructionType::BufferStoreDword && inst.type != ShaderInstructionType::BufferStoreFormatX &&
		    inst.type != ShaderInstructionType::BufferStoreFormatXy) continue;
		int target = -1;
		for (int b = 0; b < storage.buffers_num; ++b)
			if (storage.start_register[b] == inst.src[1].register_id) target = b;
		if (target < 0) return reject("store-target-uninitialized", i);
		writes |= 1u << static_cast<unsigned>(target);
	}
	if (insts.At(insts.Size() - 1).type != ShaderInstructionType::SEndpgm) return reject("missing-terminal");
	for (const auto& label: code.GetLabels())
	{
		const auto index = instruction_index(label.GetSrc());
		if (label.IsDisabled() || index == UINT32_MAX || !StorageAuthorityBranch(insts.At(index).type) ||
		    int64_t{insts.At(index).pc} + 4 + insts.At(index).src[0].constant.i != label.GetDst()) return reject("invalid-label", index);
	}
	for (const auto& label: code.GetIndirectLabels())
	{
		const auto index = instruction_index(label.GetSrc());
		if (label.IsDisabled() || index == UINT32_MAX || !StorageAuthorityBranch(insts.At(index).type) ||
		    insts.At(index).type == ShaderInstructionType::SBranch || index + 1 == insts.Size() ||
		    label.GetDst() != insts.At(index + 1).pc || label.GetDst() != label.GetSrc() + 4) return reject("invalid-indirect-label", index);
	}
	// Inspect the whole module, including writes after a store and loop bodies.
	// An immutable target tuple needs no path-dependent reaching-definition proof.
	for (int b = 0; b < storage.buffers_num; ++b)
		if ((writes & (1u << static_cast<unsigned>(b))) != 0)
			for (int lane = 0; lane < 4; ++lane) if (written_sgprs[storage.start_register[b] + lane]) return reject("store-tuple-mutated");
	return {all & ~writes, writes, true};
}

bool ShaderStorageBufferXyFloatStride8Compatible(const ShaderBufferResource& resource)
{
	return resource.Stride() == 8 && resource.DstSelXYZW() == DstSel(4, 5, 0, 1) && resource.Dfmt() == 11 &&
	       resource.Nfmt() == 7 && resource.MemoryType() == 0x10;
}

bool ShaderStorageBufferUnorm8x4Constant0011Compatible(const ShaderBufferResource& resource)
{
	return resource.Stride() == 8 && resource.DstSelXYZW() == DstSel(0, 0, 1, 1) && resource.Dfmt() == 10 &&
	       resource.Nfmt() == 0 && resource.MemoryType() == 0x10;
}

bool ShaderStorageBufferSscaled16XyzCompatible(const ShaderBufferResource& resource)
{
	return resource.Stride() == 16 && resource.DstSelXYZW() == DstSel(4, 5, 6, 0) && resource.Dfmt() == 12 &&
	       resource.Nfmt() == 3 && resource.MemoryType() == 0x10;
}

bool ShaderVertexInputSnorm8x4XyzCompatible(const ShaderBufferResource& resource, int registers_num)
{
	return registers_num == 3 && resource.Dfmt() == 10 && resource.Nfmt() == 1 &&
	       resource.DstSelXYZW() == DstSel(4, 5, 6, 7);
}

// Four physical float-valued components already supplied by vertex fetch.
// DST_SEL maps those components or the existing typed zero/one constants.
bool ShaderVertexInputFloat4SwizzleCompatible(const ShaderBufferResource& resource, int registers_num)
{
	if (registers_num != 4 || resource.AddTid() || resource.SwizzleEnabled() ||
	    !((resource.Nfmt() == 0 && resource.Dfmt() == 10) || (resource.Nfmt() == 7 && resource.Dfmt() == 14))) return false;
	for (const auto selector: {resource.DstSelX(), resource.DstSelY(), resource.DstSelZ(), resource.DstSelW()})
		if (selector != 0 && selector != 1 && (selector < 4 || selector > 7)) return false;
	return true;
}

bool ShaderVertexInputSint16x2Compatible(const ShaderBufferResource& resource, int registers_num)
{
	return registers_num == 2 && resource.Dfmt() == 5 && resource.Nfmt() == 5;
}

bool ShaderVertexInputSint16x4Compatible(const ShaderBufferResource& resource, int registers_num)
{
	return registers_num == 4 && resource.Dfmt() == 12 && resource.Nfmt() == 5;
}

bool ShaderStorageBufferFormatSupported(const ShaderBufferResource& resource)
{
	return (resource.Stride() == 4 && resource.DstSelXYZW() == DstSel(4, 0, 0, 0) && resource.Dfmt() == 4 &&
	        resource.Nfmt() == 4) ||
	       (resource.Stride() == 4 && resource.DstSelXYZW() == DstSel(4, 0, 0, 1) && resource.Dfmt() == 4 &&
	        resource.Nfmt() == 7) ||
	       (resource.Stride() == 8 && resource.DstSelXYZW() == DstSel(4, 5, 0, 0) && resource.Dfmt() == 11 &&
	        resource.Nfmt() == 4) ||
	       (resource.Stride() == 16 && resource.DstSelXYZW() == DstSel(4, 5, 0, 1) && resource.Dfmt() == 11 &&
	        resource.Nfmt() == 7) ||
	       (resource.Stride() == 16 && resource.DstSelXYZW() == DstSel(4, 5, 6, 7) && resource.Dfmt() == 14 &&
	        resource.Nfmt() == 7) ||
	       ShaderStorageBufferXyFloatStride8Compatible(resource) ||
	       ShaderStorageBufferUnorm8x4Constant0011Compatible(resource) ||
	       ShaderStorageBufferSscaled16XyzCompatible(resource);
}

bool ShaderStorageBufferFormatUsageSupported(const ShaderBufferResource& resource, ShaderStorageUsage usage)
{
	return ShaderStorageBufferFormatSupported(resource) &&
	       (!ShaderStorageBufferXyFloatStride8Compatible(resource) || usage == ShaderStorageUsage::ReadOnly) &&
	       (!ShaderStorageBufferUnorm8x4Constant0011Compatible(resource) || usage == ShaderStorageUsage::ReadOnly) &&
	       (!ShaderStorageBufferSscaled16XyzCompatible(resource) || usage == ShaderStorageUsage::ReadOnly);
}

static bool ShaderVertexReadOnlyStorageHasWriteConflict(const ShaderCode& code, const ShaderBindResources& bind)
{
	if (!ShaderCodeHasStorageBufferWrites(code))
	{
		return false;
	}
	for (int i = 0; i < bind.storage_buffers.buffers_num; ++i)
	{
		if (bind.storage_buffers.usages[i] == ShaderStorageUsage::ReadOnly)
		{
			return true;
		}
	}
	return false;
}

bool ShaderNullSampledImageLoadUnsupported(const ShaderTextureResources& textures, bool pixel_stage, bool shader_has_image_load)
{
	return ShaderNullSampledImageEnabled() && pixel_stage && shader_has_image_load && textures.HasNullSampledImage();
}

static String8 operand_to_str(ShaderOperand op)
{
	String8 ret = "???";
	switch (op.type)
	{
		case ShaderOperandType::LiteralConstant:
			EXIT_IF(op.size != 0);
			EXIT_IF(op.negate || op.absolute);
			return String8::FromPrintf("%f (%u)", op.constant.f, op.constant.u);
			break;
		case ShaderOperandType::IntegerInlineConstant:
			EXIT_IF(op.size != 0);
			EXIT_IF(op.negate || op.absolute);
			return String8::FromPrintf("%d", op.constant.i);
			break;
		case ShaderOperandType::FloatInlineConstant:
			EXIT_IF(op.size != 0);
			EXIT_IF(op.negate || op.absolute);
			return String8::FromPrintf("%f", op.constant.f);
			break;
		default: break;
	}

	EXIT_IF(op.size != 1);

	switch (op.type)
	{
		case ShaderOperandType::VccHi: ret = "vcc_hi"; break;
		case ShaderOperandType::VccLo: ret = "vcc_lo"; break;
		case ShaderOperandType::ExecHi: ret = "exec_hi"; break;
		case ShaderOperandType::ExecLo: ret = "exec_lo"; break;
		case ShaderOperandType::ExecZ: ret = "execz"; break;
		case ShaderOperandType::Scc: ret = "scc"; break;
		case ShaderOperandType::M0: ret = "m0"; break;
		case ShaderOperandType::Vgpr: ret = String8::FromPrintf("v%d", op.register_id); break;
		case ShaderOperandType::Sgpr: ret = String8::FromPrintf("s%d", op.register_id); break;
		case ShaderOperandType::Null: ret = "null"; break;
		default: break;
	}

	if (op.absolute)
	{
		ret = "abs(" + ret + ")";
	}

	if (op.negate)
	{
		return "-" + ret;
	}

	return ret;
}

static String8 operand_array_to_str(ShaderOperand op, int n)
{
	String8 ret = "???";

	EXIT_IF(op.size != n);

	switch (op.type)
	{
		case ShaderOperandType::VccLo:
			if (n == 2)
			{
				ret = "vcc";
			}
			break;
		case ShaderOperandType::ExecLo:
			if (n == 2)
			{
				ret = "exec";
			}
			break;
		case ShaderOperandType::Sgpr: ret = String8::FromPrintf("s[%d:%d]", op.register_id, op.register_id + n - 1); break;
		case ShaderOperandType::Vgpr: ret = String8::FromPrintf("v[%d:%d]", op.register_id, op.register_id + n - 1); break;
		case ShaderOperandType::LiteralConstant:
			if (n == 2)
			{
				ret = String8::FromPrintf("%f (%u)", op.constant.f, op.constant.u);
			}
			break;
		case ShaderOperandType::IntegerInlineConstant:
			if (n == 2)
			{
				ret = String8::FromPrintf("%d", op.constant.i);
			}
			break;
		default: break;
	}

	EXIT_IF(ret == "???");

	if (op.absolute)
	{
		ret = "abs(" + ret + ")";
	}

	if (op.negate)
	{
		return "-" + ret;
	}

	return ret;
}

int ShaderGetParameterExportIndex(const ShaderInstruction& inst)
{
	if (inst.type != ShaderInstructionType::Exp) return -1;
	int index = -1;
	switch (inst.format)
	{
		case ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3: index = 0; break;
		case ShaderInstructionFormat::Param1Vsrc0Vsrc1Vsrc2Vsrc3: index = 1; break;
		case ShaderInstructionFormat::Param2Vsrc0Vsrc1Vsrc2Vsrc3: index = 2; break;
		case ShaderInstructionFormat::Param3Vsrc0Vsrc1Vsrc2Vsrc3: index = 3; break;
		case ShaderInstructionFormat::Param4Vsrc0Vsrc1Vsrc2Vsrc3: index = 4; break;
		case ShaderInstructionFormat::ParamVsrc0Vsrc1Vsrc2Vsrc3:
			return inst.export_target >= 0x25 && inst.export_target <= 0x3f ? inst.export_target - 0x20 : -1;
		default: return -1;
	}
	return inst.export_target == 0xff || inst.export_target == 0x20 + index ? index : -1;
}

bool ShaderVertexPixelInterfaceSupported(const ShaderVertexInputInfo& vertex, const ShaderPixelInputInfo& pixel,
                                        uint32_t max_vertex_output_components, uint32_t max_fragment_input_components,
                                        const char** rejection_reason)
{
	if (rejection_reason != nullptr) *rejection_reason = nullptr;
	const auto reject = [&](const char* reason) {
		if (rejection_reason != nullptr) *rejection_reason = reason;
		return false;
	};
	if (vertex.export_count < 0 || vertex.export_count > 32 || pixel.input_num > 32) return reject("count-range");
	if (max_vertex_output_components == 0 || max_fragment_input_components == 0) return reject("host-capacity-unavailable");
	if (vertex.export_count > 5 && !vertex.parameter_exports_valid) return reject("producer-proof-unavailable");
	// The current templates declare Position(4), PointSize(1), ClipDistance[1]
	// and CullDistance[1], plus FragCoord(4) when present. Count even unwritten
	// declared members conservatively; do not substitute a guessed host limit.
	if (uint64_t{4} * static_cast<uint32_t>(vertex.export_count) + 7 > max_vertex_output_components)
		return reject("vertex-components-exceeded");
	if (uint64_t{4} * pixel.input_num + (pixel.ps_pos_xy ? 4u : 0u) > max_fragment_input_components)
		return reject("fragment-components-exceeded");
	uint32_t locations = 0;
	for (uint32_t index = 0; index < pixel.input_num; ++index)
	{
		const auto setting = pixel.interpolator_settings[index];
		// OFFSET[5] default/passthrough is a separate unimplemented profile.
		if ((setting & ~uint32_t{0x41f}) != 0) return reject("pixel-offset-mode");
		const auto location = setting & 0x1fu;
		if (location >= static_cast<uint32_t>(vertex.export_count)) return reject("pixel-location-undeclared");
		if (location >= max_fragment_input_components / 4u) return reject("pixel-location-exceeds-host-capacity");
		if ((locations & (uint32_t{1} << location)) != 0) return reject("duplicate-pixel-location");
		if (vertex.export_count > 5 && (vertex.parameter_export_mask & (uint32_t{1} << location)) == 0)
			return reject("pixel-location-without-producer");
		locations |= uint32_t{1} << location;
	}
	return true;
}

static String8 dbg_fmt_to_str(const ShaderInstruction& inst)
{
	switch (inst.format)
	{
		case ShaderInstructionFormat::Unknown: return "Unknown"; break;
		case ShaderInstructionFormat::Empty: return "Empty"; break;
		case ShaderInstructionFormat::Imm: return "Imm"; break;
		case ShaderInstructionFormat::Mrt0OffOffComprVmDone: return "Mrt0OffOffComprVmDone"; break;
		case ShaderInstructionFormat::Mrt0Vsrc0OffComprVmDone: return "Mrt0Vsrc0OffComprVmDone"; break;
		case ShaderInstructionFormat::Mrt0Vsrc0Vsrc1ComprVmDone: return "Mrt0Vsrc0Vsrc1ComprVmDone"; break;
		case ShaderInstructionFormat::Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone: return "Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone"; break;
		case ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3: return "Param0Vsrc0Vsrc1Vsrc2Vsrc3"; break;
		case ShaderInstructionFormat::Param1Vsrc0Vsrc1Vsrc2Vsrc3: return "Param1Vsrc0Vsrc1Vsrc2Vsrc3"; break;
		case ShaderInstructionFormat::Param2Vsrc0Vsrc1Vsrc2Vsrc3: return "Param2Vsrc0Vsrc1Vsrc2Vsrc3"; break;
		case ShaderInstructionFormat::Param3Vsrc0Vsrc1Vsrc2Vsrc3: return "Param3Vsrc0Vsrc1Vsrc2Vsrc3"; break;
		case ShaderInstructionFormat::Param4Vsrc0Vsrc1Vsrc2Vsrc3: return "Param4Vsrc0Vsrc1Vsrc2Vsrc3"; break;
		case ShaderInstructionFormat::ParamVsrc0Vsrc1Vsrc2Vsrc3: return "ParamVsrc0Vsrc1Vsrc2Vsrc3"; break;
		case ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done: return "Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done"; break;
		case ShaderInstructionFormat::Saddr: return "Saddr"; break;
		case ShaderInstructionFormat::SdstSbaseSoffset: return "SdstSbaseSoffset"; break;
		case ShaderInstructionFormat::Sdst4SbaseSoffset: return "Sdst4SbaseSoffset"; break;
		case ShaderInstructionFormat::Sdst8SbaseSoffset: return "Sdst8SbaseSoffset"; break;
		case ShaderInstructionFormat::SdstSvSoffset: return "SdstSvSoffset"; break;
		case ShaderInstructionFormat::Sdst2SvSoffset: return "Sdst2SvSoffset"; break;
		case ShaderInstructionFormat::Sdst4SvSoffset: return "Sdst4SvSoffset"; break;
		case ShaderInstructionFormat::Sdst8SvSoffset: return "Sdst8SvSoffset"; break;
		case ShaderInstructionFormat::Sdst16SvSoffset: return "Sdst16SvSoffset"; break;
		case ShaderInstructionFormat::SVdstSVsrc0: return "SVdstSVsrc0"; break;
		case ShaderInstructionFormat::Sdst2Ssrc02: return "Sdst2Ssrc02"; break;
		case ShaderInstructionFormat::Sdst2Ssrc02Ssrc1: return "Sdst2Ssrc02Ssrc1"; break;
		case ShaderInstructionFormat::Sdst2Ssrc02Ssrc12: return "Sdst2Ssrc02Ssrc12"; break;
		case ShaderInstructionFormat::SmaskVsrc0Vsrc1: return "SmaskVsrc0Vsrc1"; break;
		case ShaderInstructionFormat::Ssrc0Ssrc1: return "Ssrc0Ssrc1"; break;
		case ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen: return "Vdata1VaddrSvSoffsIdxen"; break;
		case ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxenFloat1: return "Vdata1VaddrSvSoffsIdxenFloat1"; break;
		case ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen: return "Vdata2VaddrSvSoffsIdxen"; break;
		case ShaderInstructionFormat::Vdata3VaddrSvSoffsIdxen: return "Vdata3VaddrSvSoffsIdxen"; break;
		case ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen: return "Vdata4VaddrSvSoffsIdxen"; break;
		case ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxenFloat4: return "Vdata4VaddrSvSoffsIdxenFloat4"; break;
		case ShaderInstructionFormat::Vdata4Vaddr2SvSoffsOffenIdxenFloat4: return "Vdata4Vaddr2SvSoffsOffenIdxenFloat4"; break;
		case ShaderInstructionFormat::Vdata1Vaddr3StSsDmask1: return "Vdata1Vaddr3StSsDmask1"; break;
		case ShaderInstructionFormat::Vdata1Vaddr3StSsDmask2: return "Vdata1Vaddr3StSsDmask2"; break;
		case ShaderInstructionFormat::Vdata1Vaddr3StSsDmask4: return "Vdata1Vaddr3StSsDmask4"; break;
		case ShaderInstructionFormat::Vdata1Vaddr3StSsDmask8: return "Vdata1Vaddr3StSsDmask8"; break;
		case ShaderInstructionFormat::Vdata2Vaddr3StSsDmask3: return "Vdata2Vaddr3StSsDmask3"; break;
		case ShaderInstructionFormat::Vdata2Vaddr3StSsDmask5: return "Vdata2Vaddr3StSsDmask5"; break;
		case ShaderInstructionFormat::Vdata2Vaddr3StSsDmask9: return "Vdata2Vaddr3StSsDmask9"; break;
		case ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7: return "Vdata3Vaddr3StSsDmask7"; break;
		case ShaderInstructionFormat::Vdata3Vaddr4StSsDmask7: return "Vdata3Vaddr4StSsDmask7"; break;
		case ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF: return "Vdata4Vaddr3StSsDmaskF"; break;
		case ShaderInstructionFormat::Vdata4Vaddr3StDmaskF: return "Vdata4Vaddr3StDmaskF"; break;
		case ShaderInstructionFormat::Vdata4Vaddr4StDmaskF: return "Vdata4Vaddr4StDmaskF"; break;
		case ShaderInstructionFormat::SVdstSVsrc0SVsrc1: return "SVdstSVsrc0SVsrc1"; break;
		case ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2: return "VdstVsrc0Vsrc1Smask2"; break;
		case ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2: return "VdstVsrc0Vsrc1Vsrc2"; break;
		case ShaderInstructionFormat::VdstVsrcAttrChan: return "VdstVsrcAttrChan"; break;
		case ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1: return "VdstSdst2Vsrc0Vsrc1"; break;
		case ShaderInstructionFormat::VdstGds: return "VdstGds"; break;
		case ShaderInstructionFormat::Label: return "Label"; break;
		default: return "????"; break;
	}
}

static String8 dbg_fmt_print(const ShaderInstruction& inst)
{
	uint64_t f = inst.format;
	EXIT_IF(f == ShaderInstructionFormat::Unknown);
	String8 str;
	if (f == ShaderInstructionFormat::Empty)
	{
		return str;
	}
	int src_num = 0;
	for (;;)
	{
		String8 s;
		auto    fu = f & 0xffu;
		if (fu == 0)
		{
			break;
		}
		switch (fu)
		{
			case ShaderInstructionFormat::D: s = operand_to_str(inst.dst); break;
			case ShaderInstructionFormat::D2: s = operand_to_str(inst.dst2); break;
			case ShaderInstructionFormat::S0: s = operand_to_str(inst.src[0]); break;
			case ShaderInstructionFormat::S1: s = operand_to_str(inst.src[1]); break;
			case ShaderInstructionFormat::S2: s = operand_to_str(inst.src[2]); break;
			case ShaderInstructionFormat::S3: s = operand_to_str(inst.src[3]); break;
			case ShaderInstructionFormat::DA2: s = operand_array_to_str(inst.dst, 2); break;
			case ShaderInstructionFormat::DA3: s = operand_array_to_str(inst.dst, 3); break;
			case ShaderInstructionFormat::DA4: s = operand_array_to_str(inst.dst, 4); break;
			case ShaderInstructionFormat::DA8: s = operand_array_to_str(inst.dst, 8); break;
			case ShaderInstructionFormat::DA16: s = operand_array_to_str(inst.dst, 16); break;
			case ShaderInstructionFormat::D2A2: s = operand_array_to_str(inst.dst2, 2); break;
			case ShaderInstructionFormat::D2A3: s = operand_array_to_str(inst.dst2, 3); break;
			case ShaderInstructionFormat::D2A4: s = operand_array_to_str(inst.dst2, 4); break;
			case ShaderInstructionFormat::S0A2: s = operand_array_to_str(inst.src[0], 2); break;
			case ShaderInstructionFormat::S0A3: s = operand_array_to_str(inst.src[0], 3); break;
			case ShaderInstructionFormat::S0A4: s = operand_array_to_str(inst.src[0], 4); break;
			case ShaderInstructionFormat::S1A2: s = operand_array_to_str(inst.src[1], 2); break;
			case ShaderInstructionFormat::S1A3: s = operand_array_to_str(inst.src[1], 3); break;
			case ShaderInstructionFormat::S1A4: s = operand_array_to_str(inst.src[1], 4); break;
			case ShaderInstructionFormat::S1A8: s = operand_array_to_str(inst.src[1], 8); break;
			case ShaderInstructionFormat::S2A2: s = operand_array_to_str(inst.src[2], 2); break;
			case ShaderInstructionFormat::S2A3: s = operand_array_to_str(inst.src[2], 3); break;
			case ShaderInstructionFormat::S2A4: s = operand_array_to_str(inst.src[2], 4); break;
			case ShaderInstructionFormat::Attr: s = String8::FromPrintf("attr%u.%u", inst.src[1].constant.u, inst.src[2].constant.u); break;
			case ShaderInstructionFormat::Idxen: s = "idxen"; break;
			case ShaderInstructionFormat::Offen: s = "offen"; break;
			case ShaderInstructionFormat::Float1: s = "format:float1"; break;
			case ShaderInstructionFormat::Float4: s = "format:float4"; break;
			case ShaderInstructionFormat::Pos0: s = "pos0"; break;
			case ShaderInstructionFormat::Done: s = "done"; break;
			case ShaderInstructionFormat::Param0: s = "param0"; break;
			case ShaderInstructionFormat::Param1: s = "param1"; break;
			case ShaderInstructionFormat::Param2: s = "param2"; break;
			case ShaderInstructionFormat::Param3: s = "param3"; break;
			case ShaderInstructionFormat::Param4: s = "param4"; break;
			case ShaderInstructionFormat::Param: s = String8::FromPrintf("param%d", ShaderGetParameterExportIndex(inst)); break;
			case ShaderInstructionFormat::Mrt0: s = "mrt_color0"; break;
			case ShaderInstructionFormat::Off: s = "off"; break;
			case ShaderInstructionFormat::Compr: s = "compr"; break;
			case ShaderInstructionFormat::Vm: s = "vm"; break;
			case ShaderInstructionFormat::L: s = String8::FromPrintf("label_%04" PRIx32, inst.pc + 4 + inst.src[0].constant.i); break;
			case ShaderInstructionFormat::Dmask1: s = "dmask:0x1"; break;
			case ShaderInstructionFormat::Dmask2: s = "dmask:0x2"; break;
			case ShaderInstructionFormat::Dmask4: s = "dmask:0x4"; break;
			case ShaderInstructionFormat::Dmask8: s = "dmask:0x8"; break;
			case ShaderInstructionFormat::Dmask3: s = "dmask:0x3"; break;
			case ShaderInstructionFormat::Dmask5: s = "dmask:0x5"; break;
			case ShaderInstructionFormat::Dmask7: s = "dmask:0x7"; break;
			case ShaderInstructionFormat::Dmask9: s = "dmask:0x9"; break;
			case ShaderInstructionFormat::DmaskF: s = "dmask:0xf"; break;
			case ShaderInstructionFormat::Gds: s = "gds"; break;
			default: EXIT("unknown code: %u\n", static_cast<uint32_t>(fu));
		}
		switch (fu)
		{
			case ShaderInstructionFormat::L:
			case ShaderInstructionFormat::S0:
			case ShaderInstructionFormat::S0A2:
			case ShaderInstructionFormat::S0A3:
			case ShaderInstructionFormat::S0A4: src_num = std::max(src_num, 1); break;
			case ShaderInstructionFormat::S1:
			case ShaderInstructionFormat::S1A2:
			case ShaderInstructionFormat::S1A3:
			case ShaderInstructionFormat::S1A4:
			case ShaderInstructionFormat::S1A8: src_num = std::max(src_num, 2); break;
			case ShaderInstructionFormat::S2:
			case ShaderInstructionFormat::S2A2:
			case ShaderInstructionFormat::S2A3:
			case ShaderInstructionFormat::S2A4:
			case ShaderInstructionFormat::Attr: src_num = std::max(src_num, 3); break;
			case ShaderInstructionFormat::S3: src_num = std::max(src_num, 4); break;
			default: break;
		}
		str = s + (str.IsEmpty() ? "" : ", " + str);
		f >>= 8u;
	}
	EXIT_IF(src_num != inst.src_num);
	if (inst.dst.multiplier == 2.0f)
	{
		str += " mul:2";
	}
	if (inst.dst.multiplier == 4.0f)
	{
		str += " mul:4";
	}
	if (inst.dst.multiplier == 0.5f)
	{
		str += " div:2";
	}
	if (inst.dst.clamp)
	{
		str += " clamp";
	}
	return str;
}

String8 ShaderCode::DbgInstructionToStr(const ShaderInstruction& inst)
{
	String8 ret;

	String8 name   = Core::EnumName8(inst.type);
	String8 format = dbg_fmt_to_str(inst);

	ret += String8::FromPrintf("%-20s [%-30s] ", name.c_str(), format.c_str());
	ret += dbg_fmt_print(inst);

	return ret;
}

String8 ShaderCode::DbgDump() const
{
	String8 ret;
	for (const auto& inst: m_instructions)
	{
		if (m_labels.Contains(inst.pc, [](auto label, auto pc) { return (!label.IsDisabled() && label.GetDst() == pc); }))
		{
			ret += String8::FromPrintf("\nlabel_%04" PRIx32 ":\n", inst.pc);
		}
		if (m_indirect_labels.Contains(inst.pc, [](auto label, auto pc) { return (!label.IsDisabled() && label.GetDst() == pc); }))
		{
			ret += "\n";
		}
		ret += String8::FromPrintf("  %s\n", DbgInstructionToStr(inst).c_str());
	}
	return ret;
}

static bool IsDiscardInstruction(const Vector<ShaderInstruction>& code, uint32_t index)
{
	if (!(index == 0 || index + 1 >= code.Size()))
	{
		const auto& prev_inst = code.At(index - 1);
		const auto& inst      = code.At(index);
		const auto& next_inst = code.At(index + 1);

		return (inst.type == ShaderInstructionType::Exp && inst.format == ShaderInstructionFormat::Mrt0OffOffComprVmDone &&
		        prev_inst.type == ShaderInstructionType::SMovB64 && prev_inst.format == ShaderInstructionFormat::Sdst2Ssrc02 &&
		        prev_inst.dst.type == ShaderOperandType::ExecLo && prev_inst.src[0].type == ShaderOperandType::IntegerInlineConstant &&
		        prev_inst.src[0].constant.i == 0 && next_inst.type == ShaderInstructionType::SEndpgm);
	}
	return false;
}

// bool ShaderCode::IsDiscardBlock(uint32_t pc) const
//{
//	auto inst_count = m_instructions.Size();
//	for (uint32_t index = 0; index < inst_count; index++)
//	{
//		const auto& inst = m_instructions.At(index);
//		if (inst.pc == pc)
//		{
//			for (uint32_t i = index; i < inst_count; i++)
//			{
//				const auto& inst = m_instructions.At(i);
//
//				if (inst.type == ShaderInstructionType::SEndpgm || inst.type == ShaderInstructionType::SCbranchExecz ||
//				    inst.type == ShaderInstructionType::SCbranchScc0 || inst.type == ShaderInstructionType::SCbranchScc1 ||
//				    inst.type == ShaderInstructionType::SCbranchVccz)
//				{
//					return false;
//				}
//
//				if (IsDiscardInstruction(i))
//				{
//					return true;
//				}
//			}
//			return false;
//		}
//	}
//	return false;
// }

ShaderControlFlowBlock ShaderCode::ReadBlock(uint32_t pc) const
{
	ShaderControlFlowBlock ret;
	auto                   inst_count = m_instructions.Size();
	for (uint32_t index = 0; index < inst_count; index++)
	{
		const auto& inst = m_instructions.At(index);
		if (inst.pc == pc)
		{
			ret.pc       = pc;
			ret.is_valid = true;
			for (uint32_t i = index; i < inst_count; i++)
			{
				const auto& inst = m_instructions.At(i);

				if (inst.type == ShaderInstructionType::SEndpgm || inst.type == ShaderInstructionType::SCbranchExecz ||
				    inst.type == ShaderInstructionType::SCbranchScc0 || inst.type == ShaderInstructionType::SCbranchScc1 ||
				    inst.type == ShaderInstructionType::SCbranchVccz || inst.type == ShaderInstructionType::SCbranchVccnz ||
				    inst.type == ShaderInstructionType::SBranch)
				{
					ret.last = inst;
					break;
				}

				if (IsDiscardInstruction(m_instructions, i))
				{
					ret.is_discard = true;
				}
			}
			break;
		}
	}
	return ret;
}

Vector<ShaderInstruction> ShaderCode::ReadIntructions(const ShaderControlFlowBlock& block) const
{
	Vector<ShaderInstruction> ret;

	auto inst_count = m_instructions.Size();
	for (uint32_t index = 0; index < inst_count; index++)
	{
		const auto& inst = m_instructions.At(index);
		if (inst.pc == block.pc)
		{
			for (uint32_t i = index; i < inst_count; i++)
			{
				const auto& inst = m_instructions.At(i);

				ret.Add(inst);

				if (inst.pc == block.last.pc)
				{
					break;
				}
			}
			break;
		}
	}

	return ret;
}

static void vs_print(const char* func, const HW::VertexShaderInfo& vs, const HW::ShaderRegisters& sh)
{
	printf("%s\n", func);

	printf("\t vs.data_addr                 = 0x%016" PRIx64 "\n", vs.vs_regs.data_addr);
	printf("\t es.data_addr                 = 0x%016" PRIx64 "\n", vs.es_regs.data_addr);
	printf("\t gs.data_addr                 = 0x%016" PRIx64 "\n", vs.gs_regs.data_addr);

	if (vs.vs_regs.data_addr != 0)
	{
		printf("\t vs.vgprs                     = 0x%02" PRIx8 "\n", vs.vs_regs.rsrc1.vgprs);
		printf("\t vs.sgprs                     = 0x%02" PRIx8 "\n", vs.vs_regs.rsrc1.sgprs);
		printf("\t vs.priority                  = 0x%02" PRIx8 "\n", vs.vs_regs.rsrc1.priority);
		printf("\t vs.float_mode                = 0x%02" PRIx8 "\n", vs.vs_regs.rsrc1.float_mode);
		printf("\t vs.dx10_clamp                = %s\n", vs.vs_regs.rsrc1.dx10_clamp ? "true" : "false");
		printf("\t vs.ieee_mode                 = %s\n", vs.vs_regs.rsrc1.ieee_mode ? "true" : "false");
		printf("\t vs.vgpr_component_count      = 0x%02" PRIx8 "\n", vs.vs_regs.rsrc1.vgpr_component_count);
		printf("\t vs.cu_group_enable           = %s\n", vs.vs_regs.rsrc1.cu_group_enable ? "true" : "false");
		printf("\t vs.require_forward_progress  = %s\n", vs.vs_regs.rsrc1.require_forward_progress ? "true" : "false");
		printf("\t vs.fp16_overflow             = %s\n", vs.vs_regs.rsrc1.fp16_overflow ? "true" : "false");
		printf("\t vs.scratch_en                = %s\n", vs.vs_regs.rsrc2.scratch_en ? "true" : "false");
		printf("\t vs.user_sgpr                 = 0x%02" PRIx8 "\n", vs.vs_regs.rsrc2.user_sgpr);
		printf("\t vs.offchip_lds               = %s\n", vs.vs_regs.rsrc2.offchip_lds ? "true" : "false");
		printf("\t vs.streamout_enabled         = %s\n", vs.vs_regs.rsrc2.streamout_enabled ? "true" : "false");
		printf("\t vs.shared_vgprs              = 0x%02" PRIx8 "\n", vs.vs_regs.rsrc2.shared_vgprs);
	}

	if (vs.gs_regs.data_addr != 0 || vs.es_regs.data_addr != 0)
	{
		printf("\t gs.vgprs                     = 0x%02" PRIx8 "\n", vs.gs_regs.rsrc1.vgprs);
		printf("\t gs.sgprs                     = 0x%02" PRIx8 "\n", vs.gs_regs.rsrc1.sgprs);
		printf("\t gs.priority                  = 0x%02" PRIx8 "\n", vs.gs_regs.rsrc1.priority);
		printf("\t gs.float_mode                = 0x%02" PRIx8 "\n", vs.gs_regs.rsrc1.float_mode);
		printf("\t gs.dx10_clamp                = %s\n", vs.gs_regs.rsrc1.dx10_clamp ? "true" : "false");
		printf("\t gs.ieee_mode                 = %s\n", vs.gs_regs.rsrc1.ieee_mode ? "true" : "false");
		printf("\t gs.debug_mode                = %s\n", vs.gs_regs.rsrc1.debug_mode ? "true" : "false");
		printf("\t gs.lds_configuration         = %s\n", vs.gs_regs.rsrc1.lds_configuration ? "true" : "false");
		printf("\t gs.cu_group_enable           = %s\n", vs.gs_regs.rsrc1.cu_group_enable ? "true" : "false");
		printf("\t gs.require_forward_progress  = %s\n", vs.gs_regs.rsrc1.require_forward_progress ? "true" : "false");
		printf("\t gs.fp16_overflow             = %s\n", vs.gs_regs.rsrc1.fp16_overflow ? "true" : "false");
		printf("\t gs.gs_vgpr_component_count   = 0x%02" PRIx8 "\n", vs.gs_regs.rsrc1.gs_vgpr_component_count);
		printf("\t gs.scratch_en                = %s\n", vs.gs_regs.rsrc2.scratch_en ? "true" : "false");
		printf("\t gs.user_sgpr                 = 0x%02" PRIx8 "\n", vs.gs_regs.rsrc2.user_sgpr);
		printf("\t gs.offchip_lds               = %s\n", vs.gs_regs.rsrc2.offchip_lds ? "true" : "false");
		printf("\t gs.shared_vgprs              = 0x%02" PRIx8 "\n", vs.gs_regs.rsrc2.shared_vgprs);
		printf("\t gs.es_vgpr_component_count   = 0x%02" PRIx8 "\n", vs.gs_regs.rsrc2.es_vgpr_component_count);
		printf("\t gs.lds_size                  = 0x%02" PRIx8 "\n", vs.gs_regs.rsrc2.lds_size);
	}

	printf("\t m_spiVsOutConfig          = 0x%08" PRIx32 "\n", sh.m_spiVsOutConfig);
	printf("\t m_spiShaderPosFormat      = 0x%08" PRIx32 "\n", sh.m_spiShaderPosFormat);
	printf("\t m_paClVsOutCntl           = 0x%08" PRIx32 "\n", sh.m_paClVsOutCntl);
	printf("\t m_spiShaderIdxFormat      = 0x%08" PRIx32 "\n", sh.m_spiShaderIdxFormat);
	printf("\t m_geNggSubgrpCntl         = 0x%08" PRIx32 "\n", sh.m_geNggSubgrpCntl);
	printf("\t m_vgtGsInstanceCnt        = 0x%08" PRIx32 "\n", sh.m_vgtGsInstanceCnt);
	printf("\t GetEsVertsPerSubgrp()     = 0x%08" PRIx32 "\n", sh.GetEsVertsPerSubgrp());
	printf("\t GetGsPrimsPerSubgrp()     = 0x%08" PRIx32 "\n", sh.GetGsPrimsPerSubgrp());
	printf("\t GetGsInstPrimsInSubgrp()  = 0x%08" PRIx32 "\n", sh.GetGsInstPrimsInSubgrp());
	printf("\t m_geMaxOutputPerSubgroup  = 0x%08" PRIx32 "\n", sh.m_geMaxOutputPerSubgroup);
	printf("\t m_vgtEsgsRingItemsize     = 0x%08" PRIx32 "\n", sh.m_vgtEsgsRingItemsize);
	printf("\t m_vgtGsMaxVertOut         = 0x%08" PRIx32 "\n", sh.m_vgtGsMaxVertOut);
	printf("\t m_vgtGsOutPrimType        = 0x%08" PRIx32 "\n", sh.m_vgtGsOutPrimType);
}

static void ps_print(const char* func, const HW::PsStageRegisters& ps, const HW::ShaderRegisters& sh)
{
	printf("%s\n", func);

	printf("\t data_addr                   = 0x%016" PRIx64 "\n", ps.data_addr);
	printf("\t conservative_z_export_value = 0x%08" PRIx32 "\n", sh.db_shader_control.conservative_z_export_value);
	printf("\t shader_z_behavior           = 0x%08" PRIx32 "\n", sh.db_shader_control.shader_z_behavior);
	printf("\t shader_kill_enable          = %s\n", sh.db_shader_control.shader_kill_enable ? "true" : "false");
	printf("\t shader_z_export_enable      = %s\n", sh.db_shader_control.shader_z_export_enable ? "true" : "false");
	printf("\t shader_execute_on_noop      = %s\n", sh.db_shader_control.shader_execute_on_noop ? "true" : "false");
	printf("\t vgprs                       = 0x%02" PRIx8 "\n", ps.rsrc1.vgprs);
	printf("\t sgprs                       = 0x%02" PRIx8 "\n", ps.rsrc1.sgprs);
	printf("\t priority                    = 0x%02" PRIx8 "\n", ps.rsrc1.priority);
	printf("\t float_mode                  = 0x%02" PRIx8 "\n", ps.rsrc1.float_mode);
	printf("\t dx10_clamp                  = %s\n", ps.rsrc1.dx10_clamp ? "true" : "false");
	printf("\t debug_mode                  = %s\n", ps.rsrc1.debug_mode ? "true" : "false");
	printf("\t ieee_mode                   = %s\n", ps.rsrc1.ieee_mode ? "true" : "false");
	printf("\t cu_group_disable            = %s\n", ps.rsrc1.cu_group_disable ? "true" : "false");
	printf("\t require_forward_progress    = %s\n", ps.rsrc1.require_forward_progress ? "true" : "false");
	printf("\t fp16_overflow               = %s\n", ps.rsrc1.fp16_overflow ? "true" : "false");
	printf("\t scratch_en                  = %s\n", ps.rsrc2.scratch_en ? "true" : "false");
	printf("\t user_sgpr                   = 0x%02" PRIx8 "\n", ps.rsrc2.user_sgpr);
	printf("\t wave_cnt_en                 = %s\n", ps.rsrc2.wave_cnt_en ? "true" : "false");
	printf("\t extra_lds_size              = 0x%02" PRIx8 "\n", ps.rsrc2.extra_lds_size);
	printf("\t raster_ordered_shading      = %s\n", ps.rsrc2.raster_ordered_shading ? "true" : "false");
	printf("\t shared_vgprs                = 0x%02" PRIx8 "\n", ps.rsrc2.shared_vgprs);

	printf("\t shader_z_format             = 0x%08" PRIx32 "\n", sh.shader_z_format);
	printf("\t target_output_mode[0]       = 0x%02" PRIx8 "\n", sh.target_output_mode[0]);
	printf("\t ps_input_ena                = 0x%08" PRIx32 "\n", sh.ps_input_ena);
	printf("\t ps_input_addr               = 0x%08" PRIx32 "\n", sh.ps_input_addr);
	printf("\t ps_in_control               = 0x%08" PRIx32 "\n", sh.ps_in_control);
	printf("\t baryc_cntl                  = 0x%08" PRIx32 "\n", sh.baryc_cntl);
	printf("\t m_cbShaderMask              = 0x%08" PRIx32 "\n", sh.m_cbShaderMask);

	printf("\t m_paScShaderControl         = 0x%08" PRIx32 "\n", sh.m_paScShaderControl);
}

static void cs_print(const char* func, const HW::CsStageRegisters& cs, const HW::ShaderRegisters& /*sh*/)
{
	printf("%s\n", func);

	//	printf("\t GetGpuAddress()        = 0x%016" PRIx64 "\n", cs.GetGpuAddress());
	//	printf("\t m_computePgmLo         = 0x%08" PRIx32 "\n", cs.m_computePgmLo);
	//	printf("\t m_computePgmHi         = 0x%08" PRIx32 "\n", cs.m_computePgmHi);
	//	printf("\t m_computePgmRsrc1      = 0x%08" PRIx32 "\n", cs.m_computePgmRsrc1);
	//	printf("\t m_computePgmRsrc2      = 0x%08" PRIx32 "\n", cs.m_computePgmRsrc2);
	//	printf("\t m_computeNumThreadX    = 0x%08" PRIx32 "\n", cs.m_computeNumThreadX);
	//	printf("\t m_computeNumThreadY    = 0x%08" PRIx32 "\n", cs.m_computeNumThreadY);
	//	printf("\t m_computeNumThreadZ    = 0x%08" PRIx32 "\n", cs.m_computeNumThreadZ);
	printf("\t data_addr      = 0x%016" PRIx64 "\n", cs.data_addr);
	printf("\t num_thread_x   = 0x%08" PRIx32 "\n", cs.num_thread_x);
	printf("\t num_thread_y   = 0x%08" PRIx32 "\n", cs.num_thread_y);
	printf("\t num_thread_z   = 0x%08" PRIx32 "\n", cs.num_thread_z);
	printf("\t vgprs          = 0x%02" PRIx8 "\n", cs.vgprs);
	printf("\t sgprs          = 0x%02" PRIx8 "\n", cs.sgprs);
	printf("\t bulky          = 0x%02" PRIx8 "\n", cs.bulky);
	printf("\t scratch_en     = 0x%02" PRIx8 "\n", cs.scratch_en);
	printf("\t user_sgpr      = 0x%02" PRIx8 "\n", cs.user_sgpr);
	printf("\t tgid_x_en      = 0x%02" PRIx8 "\n", cs.tgid_x_en);
	printf("\t tgid_y_en      = 0x%02" PRIx8 "\n", cs.tgid_y_en);
	printf("\t tgid_z_en      = 0x%02" PRIx8 "\n", cs.tgid_z_en);
	printf("\t tg_size_en     = 0x%02" PRIx8 "\n", cs.tg_size_en);
	printf("\t tidig_comp_cnt = 0x%02" PRIx8 "\n", cs.tidig_comp_cnt);
	printf("\t lds_size       = 0x%02" PRIx8 "\n", cs.lds_size);
}

static void bi_print(const char* func, const ShaderBinaryInfo& bi)
{
	printf("%s\n", func);

	printf("\t signature                  = %.7s\n", bi.signature);
	printf("\t version                    = 0x%02" PRIx8 "\n", bi.version);
	printf("\t pssl_or_cg                 = 0x%08" PRIx32 "\n", static_cast<uint32_t>(bi.pssl_or_cg));
	printf("\t cached                     = 0x%08" PRIx32 "\n", static_cast<uint32_t>(bi.cached));
	printf("\t type                       = 0x%08" PRIx32 "\n", static_cast<uint32_t>(bi.type));
	printf("\t source_type                = 0x%08" PRIx32 "\n", static_cast<uint32_t>(bi.source_type));
	printf("\t length                     = 0x%08" PRIx32 "\n", static_cast<uint32_t>(bi.length));
	printf("\t chunk_usage_base_offset_dw = 0x%02" PRIx8 "\n", bi.chunk_usage_base_offset_dw);
	printf("\t num_input_usage_slots      = 0x%02" PRIx8 "\n", bi.num_input_usage_slots);
	printf("\t is_srt                     = 0x%02" PRIx8 "\n", bi.is_srt);
	printf("\t is_srt_used_info_valid     = 0x%02" PRIx8 "\n", bi.is_srt_used_info_valid);
	printf("\t is_extended_usage_info     = 0x%02" PRIx8 "\n", bi.is_extended_usage_info);
	printf("\t reserved2                  = 0x%02" PRIx8 "\n", bi.reserved2);
	printf("\t reserved3                  = 0x%02" PRIx8 "\n", bi.reserved3);
	printf("\t hash0                      = 0x%08" PRIx32 "\n", bi.hash0);
	printf("\t hash1                      = 0x%08" PRIx32 "\n", bi.hash1);
	printf("\t crc32                      = 0x%08" PRIx32 "\n", bi.crc32);
}

bool ShaderGraphicsFp32StateSupported(const HW::VertexShaderInfo& vs, const HW::PixelShaderInfo& ps)
{
	if (!vs.vs_embedded)
	{
		if (vs.vs_regs.data_addr != 0 && (vs.vs_regs.rsrc1.float_mode != 192 || vs.vs_regs.rsrc1.ieee_mode))
		{
			return false;
		}
		if ((vs.es_regs.data_addr != 0 || vs.gs_regs.data_addr != 0) && (vs.gs_regs.rsrc1.float_mode != 192 || vs.gs_regs.rsrc1.ieee_mode))
		{
			return false;
		}
	}
	return ps.ps_embedded || (ps.ps_regs.rsrc1.float_mode == 192 && !ps.ps_regs.rsrc1.ieee_mode);
}

bool ShaderGraphicsDx10ClampStateSupported(const HW::VertexShaderInfo& vs, const HW::PixelShaderInfo& ps)
{
	const bool zero_experiment = ShaderDx10ClampZeroExperimentEnabled();
	if (!vs.vs_embedded)
	{
		if (vs.vs_regs.data_addr != 0 && !vs.vs_regs.rsrc1.dx10_clamp && !zero_experiment)
		{
			return false;
		}
		if ((vs.es_regs.data_addr != 0 || vs.gs_regs.data_addr != 0) && !vs.gs_regs.rsrc1.dx10_clamp)
		{
			return false;
		}
	}
	return ps.ps_embedded || ps.ps_regs.rsrc1.dx10_clamp || zero_experiment;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void vs_check(const HW::VertexShaderInfo& vs, const HW::ShaderRegisters& sh)
{
	if (vs.vs_regs.data_addr != 0)
	{
		EXIT_NOT_IMPLEMENTED(vs.vs_regs.rsrc1.priority != 0);
		EXIT_NOT_IMPLEMENTED(vs.vs_regs.rsrc1.float_mode != 192);
		EXIT_NOT_IMPLEMENTED(!vs.vs_regs.rsrc1.dx10_clamp && !ShaderDx10ClampZeroExperimentEnabled());
		EXIT_NOT_IMPLEMENTED(vs.vs_regs.rsrc1.ieee_mode != false);
		EXIT_NOT_IMPLEMENTED(vs.vs_regs.rsrc1.cu_group_enable != false);
		EXIT_NOT_IMPLEMENTED(vs.vs_regs.rsrc1.require_forward_progress != false);
		EXIT_NOT_IMPLEMENTED(vs.vs_regs.rsrc1.fp16_overflow != false);
		EXIT_NOT_IMPLEMENTED(vs.vs_regs.rsrc2.scratch_en != false);
		EXIT_NOT_IMPLEMENTED(vs.vs_regs.rsrc2.offchip_lds != false);
		EXIT_NOT_IMPLEMENTED(vs.vs_regs.rsrc2.streamout_enabled != false);
		EXIT_NOT_IMPLEMENTED(vs.vs_regs.rsrc2.shared_vgprs != 0);
	}

	if (vs.es_regs.data_addr != 0 || vs.gs_regs.data_addr != 0)
	{
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.priority != 0);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.float_mode != 192);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.dx10_clamp != true);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.debug_mode != false);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.ieee_mode != false);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.cu_group_enable != false);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.require_forward_progress != false);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.lds_configuration != false);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.gs_vgpr_component_count != 3);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.fp16_overflow != false);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc2.scratch_en != false);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc2.offchip_lds != false);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc2.es_vgpr_component_count != 3);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc2.lds_size != 0);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc2.shared_vgprs != 0);
	}

	EXIT_NOT_IMPLEMENTED(sh.m_spiShaderPosFormat != 0x00000004);
	EXIT_NOT_IMPLEMENTED(sh.m_paClVsOutCntl != 0x00000000);

	EXIT_NOT_IMPLEMENTED(sh.m_spiShaderIdxFormat != 0x00000000 && sh.m_spiShaderIdxFormat != 0x00000001);
	EXIT_NOT_IMPLEMENTED(sh.m_geNggSubgrpCntl != 0x00000000 && sh.m_geNggSubgrpCntl != 0x00000001);
	EXIT_NOT_IMPLEMENTED(sh.m_vgtGsInstanceCnt != 0x00000000);
	EXIT_NOT_IMPLEMENTED(sh.GetEsVertsPerSubgrp() != 0x00000000 && sh.GetEsVertsPerSubgrp() != 0x00000040);
	EXIT_NOT_IMPLEMENTED(sh.GetGsPrimsPerSubgrp() != 0x00000000 && sh.GetGsPrimsPerSubgrp() != 0x00000040);
	EXIT_NOT_IMPLEMENTED(sh.GetGsInstPrimsInSubgrp() != 0x00000000 && sh.GetGsInstPrimsInSubgrp() != 0x00000040);
	EXIT_NOT_IMPLEMENTED(sh.m_geMaxOutputPerSubgroup != 0x00000000 && sh.m_geMaxOutputPerSubgroup != 0x00000040);
	EXIT_NOT_IMPLEMENTED(sh.m_vgtEsgsRingItemsize != 0x00000000 && sh.m_vgtEsgsRingItemsize != 0x00000004);
	EXIT_NOT_IMPLEMENTED(sh.m_vgtGsMaxVertOut != 0x00000000);
	EXIT_NOT_IMPLEMENTED(sh.m_vgtGsOutPrimType != 0x00000000);
}

static void ps_check(const HW::PsStageRegisters& ps, const HW::ShaderRegisters& sh)
{
	EXIT_NOT_IMPLEMENTED(!IsPixelColorExportSupported(DecodePixelColorExportEncoding(sh.target_output_mode[0], sh.m_cbShaderMask)));
	EXIT_NOT_IMPLEMENTED(sh.db_shader_control.conservative_z_export_value != 0x00000000);
	EXIT_NOT_IMPLEMENTED(sh.db_shader_control.shader_z_behavior != 0x00000001 && sh.db_shader_control.shader_z_behavior != 0x00000000);
	// EXIT_NOT_IMPLEMENTED(ps.shader_kill_enable != false);
	EXIT_NOT_IMPLEMENTED(sh.db_shader_control.shader_z_export_enable != false);
	// EXIT_NOT_IMPLEMENTED(ps.shader_execute_on_noop != false);
	// EXIT_NOT_IMPLEMENTED(ps.m_spiShaderPgmRsrc1Ps != 0x002c0000);
	// EXIT_NOT_IMPLEMENTED(ps.m_spiShaderPgmRsrc2Ps != 0x00000000);
	// EXIT_NOT_IMPLEMENTED(ps.vgprs != 0x00 && ps.vgprs != 0x01);
	// EXIT_NOT_IMPLEMENTED(ps.sgprs != 0x00 && ps.sgprs != 0x01);
	EXIT_NOT_IMPLEMENTED(ps.rsrc1.priority != 0);
	EXIT_NOT_IMPLEMENTED(ps.rsrc1.float_mode != 192);
	EXIT_NOT_IMPLEMENTED(!ps.rsrc1.dx10_clamp && !ShaderDx10ClampZeroExperimentEnabled());
	EXIT_NOT_IMPLEMENTED(ps.rsrc1.debug_mode != false);
	EXIT_NOT_IMPLEMENTED(ps.rsrc1.ieee_mode != false);
	EXIT_NOT_IMPLEMENTED(ps.rsrc1.cu_group_disable != false);
	EXIT_NOT_IMPLEMENTED(ps.rsrc1.require_forward_progress != false);
	EXIT_NOT_IMPLEMENTED(ps.rsrc1.fp16_overflow != false);
	EXIT_NOT_IMPLEMENTED(ps.rsrc2.scratch_en != false);
	// EXIT_NOT_IMPLEMENTED(ps.user_sgpr != 0 && ps.user_sgpr != 4 && ps.user_sgpr != 12);
	EXIT_NOT_IMPLEMENTED(ps.rsrc2.wave_cnt_en != false);
	EXIT_NOT_IMPLEMENTED(ps.rsrc2.extra_lds_size != 0);
	EXIT_NOT_IMPLEMENTED(ps.rsrc2.raster_ordered_shading != false);
	EXIT_NOT_IMPLEMENTED(ps.rsrc2.shared_vgprs != 0);

	EXIT_NOT_IMPLEMENTED(sh.shader_z_format != 0x00000000);
	EXIT_NOT_IMPLEMENTED(sh.ps_input_ena != 0x00000002 && sh.ps_input_ena != 0x00000302);
	EXIT_NOT_IMPLEMENTED(sh.ps_input_addr != 0x00000002 && sh.ps_input_addr != 0x00000302);
	// EXIT_NOT_IMPLEMENTED(ps.m_spiPsInControl != 0x00000000);
	EXIT_NOT_IMPLEMENTED(sh.baryc_cntl != 0x00000000 && sh.baryc_cntl != 0x01000000);
	EXIT_NOT_IMPLEMENTED(sh.db_shader_control.other_bits != 0x00000000);
	EXIT_NOT_IMPLEMENTED(sh.m_paScShaderControl != 0x00000000);
}

bool ShaderIsPixelColorExportCompatible(const ShaderCode& code, uint32_t target_output_mode, uint32_t shader_mask)
{
	if (code.IsPsEmbedded())
	{
		return true;
	}

	const auto register_encoding = DecodePixelColorExportEncoding(target_output_mode, shader_mask);
	if (!IsPixelColorExportSupported(register_encoding))
	{
		return false;
	}

	bool saw_mrt0_export = false;
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.type != ShaderInstructionType::Exp)
		{
			continue;
		}
		saw_mrt0_export                               = true;
		PixelColorExportEncoding instruction_encoding = PixelColorExportEncoding::Unsupported;
		switch (inst.format)
		{
			case ShaderInstructionFormat::Mrt0OffOffComprVmDone: continue;
			case ShaderInstructionFormat::Mrt0Vsrc0OffComprVmDone:
				instruction_encoding = PixelColorExportEncoding::Mrt0Rg16FloatCompressed;
				break;
			case ShaderInstructionFormat::Mrt0Vsrc0Vsrc1ComprVmDone:
				instruction_encoding = PixelColorExportEncoding::Mrt0Rgba16FloatCompressed;
				break;
			case ShaderInstructionFormat::Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone:
				instruction_encoding = PixelColorExportEncoding::Mrt0Rgba32Float;
				break;
			default: return false;
		}
		if (!ArePixelColorExportEncodingsCompatible(register_encoding, instruction_encoding))
		{
			return false;
		}
	}
	return saw_mrt0_export;
}

static void cs_check(const HW::CsStageRegisters& cs, const HW::ShaderRegisters& /*sh*/)
{
	// EXIT_NOT_IMPLEMENTED(cs.num_thread_x != 0x00000040);
	// EXIT_NOT_IMPLEMENTED(cs.num_thread_y != 0x00000001);
	// EXIT_NOT_IMPLEMENTED(cs.num_thread_z != 0x00000001);
	// EXIT_NOT_IMPLEMENTED(cs.vgprs != 0x00 && cs.vgprs != 0x01);
	// EXIT_NOT_IMPLEMENTED(cs.sgprs != 0x01 && cs.sgprs != 0x02);
	EXIT_NOT_IMPLEMENTED(cs.bulky != 0x00);
	EXIT_NOT_IMPLEMENTED(cs.scratch_en != 0x00);
	// EXIT_NOT_IMPLEMENTED(cs.user_sgpr != 0x0c);
	EXIT_NOT_IMPLEMENTED(cs.tgid_x_en != 0x01);
	// EXIT_NOT_IMPLEMENTED(cs.tgid_y_en != 0x00);
	// EXIT_NOT_IMPLEMENTED(cs.tgid_z_en != 0x00);
	EXIT_NOT_IMPLEMENTED(cs.tg_size_en != 0x00);
	EXIT_NOT_IMPLEMENTED(cs.tidig_comp_cnt > 2);
	EXIT_NOT_IMPLEMENTED(cs.lds_size != 0x00);

	//	EXIT_NOT_IMPLEMENTED(cs.m_computePgmRsrc1 != 0x002c0040);
	//	EXIT_NOT_IMPLEMENTED(cs.m_computePgmRsrc2 != 0x00000098);
	//	EXIT_NOT_IMPLEMENTED(cs.m_computeNumThreadX != 0x00000040);
	//	EXIT_NOT_IMPLEMENTED(cs.m_computeNumThreadY != 0x00000001);
	//	EXIT_NOT_IMPLEMENTED(cs.m_computeNumThreadZ != 0x00000001);
}

// Vulkan 1.1 guarantees SPIR-V 1.3 in core. Keeping the generated shaders on
// that baseline avoids requiring the optional SPIR-V 1.4 dependency pair on
// otherwise compatible Vulkan 1.1 devices.
constexpr spv_target_env SPIRV_TARGET_ENV = SPV_ENV_VULKAN_1_1;

static bool SpirvDisassemble(const uint32_t* src_binary, size_t src_binary_size, String8* dst_disassembly)
{
	if (dst_disassembly != nullptr)
	{
		spvtools::SpirvTools core(SPIRV_TARGET_ENV);

		std::string disassembly;
		if (!core.Disassemble(src_binary, src_binary_size, &disassembly,
		                      static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_NO_HEADER) |
		                          static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES) |
		                          static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COMMENT) |
		                          static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_INDENT) |
		                          static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COLOR)))
		{
			*dst_disassembly = disassembly.c_str();

			printf("Disassemble failed\n");
			return false;
		}

		*dst_disassembly = disassembly.c_str();
	}
	return true;
}

static bool SpirvToGlsl(const uint32_t* /*src_binary*/, size_t /*src_binary_size*/, String8* /*dst_code*/)
{
	//	if (dst_code != nullptr)
	//	{
	//		spirv_cross::CompilerGLSL glsl(src_binary, src_binary_size);
	//
	//		std::string source = glsl.compile();
	//
	//		*dst_code = source.c_str();
	//	}
	return true;
}

static Config::ShaderOptimizationType ShaderEffectiveOptimization(const ShaderCode& code, Config::ShaderOptimizationType configured)
{
	if (code.HasAnyOf(
	        {ShaderInstructionType::VMulLegacyF32, ShaderInstructionType::VMacLegacyF32, ShaderInstructionType::VRsqClampF32}))
	{
		return Config::ShaderOptimizationType::None;
	}
	const bool graphics_stage = code.GetType() == ShaderType::Vertex || code.GetType() == ShaderType::Pixel;
	return graphics_stage && code.GetDx10ClampMode() == ShaderDx10ClampMode::Disabled ? Config::ShaderOptimizationType::None : configured;
}

[[maybe_unused]] static bool ShaderAndroidFexStructuralProfileSelected(const ShaderCode& code, bool android_fex_runtime,
	                                                                   bool compatibility_profile_enabled,
	                                                                   bool spirv_debug_printf_enabled)
{
	return android_fex_runtime && compatibility_profile_enabled && !spirv_debug_printf_enabled &&
	       (code.GetType() == ShaderType::Vertex || code.GetType() == ShaderType::Pixel) &&
	       code.GetDebugPrintfs().IsEmpty();
}

static bool ShaderAndroidFexStructuralProfileExperimentEnabled()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && LUMEN_ABI == LUMEN_ABI_ARM64_V8A && \
    defined(LUMEN_ANDROID_FEXCORE_RUNTIME)
	static const bool enabled = []()
	{
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_GFX_ANDROID_FEX_SPIRV_SIZE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
#else
	return false;
#endif
}

static bool ShaderAndroidFexStructuralProfileRequired(const ShaderCode& code)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && LUMEN_ABI == LUMEN_ABI_ARM64_V8A && \
    defined(LUMEN_ANDROID_FEXCORE_RUNTIME)
	return ShaderAndroidFexStructuralProfileSelected(
	    code, true, ShaderAndroidFexStructuralProfileExperimentEnabled(), Config::SpirvDebugPrintfEnabled());
#else
	(void)code;
	return false;
#endif
}

static bool SpirvRun(const String8& src, Vector<uint32_t>* dst, String8* err_msg,
                     Config::ShaderOptimizationType optimization, bool android_fex_structural_profile = false,
                     bool validate = Config::ShaderValidationEnabled())
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(err_msg == nullptr);

	spvtools::SpirvTools core(SPIRV_TARGET_ENV);
	spvtools::Optimizer  opt(SPIRV_TARGET_ENV);

	spv_position_t error_position {};
	String8        error_msg;

	auto print_msg_to_stderr = [&error_position, &error_msg](spv_message_level_t /* level */, const char* /*source*/,
	                                                         [[maybe_unused]] const spv_position_t& position, const char* m)
	{
		// printf("%s\n", source);
		error_msg = String8::FromPrintf("%d: %d (%d) %s", static_cast<int>(position.line), static_cast<int>(position.column),
		                                static_cast<int>(position.index), m);
		printf(FG_BRIGHT_RED "error: %s\n" FG_DEFAULT, error_msg.c_str());
		error_position = position;
	};
	core.SetMessageConsumer(print_msg_to_stderr);
	opt.SetMessageConsumer(print_msg_to_stderr);

	dst->Clear();

	std::vector<uint32_t> spirv;
	if (!core.Assemble(src.GetDataConst(), src.Size(), &spirv))
	{
		printf("Assemble failed at:\n%s\n", src.Mid(src.FindIndex('\n', error_position.index - 100), 200).c_str());
		*err_msg = String8::FromPrintf("Assemble failed at:\n%s\n", src.Mid(src.FindIndex('\n', error_position.index - 100), 200).c_str());
		return false;
	}

	if (validate && !core.Validate(spirv))
	{
		String8 disassembly;
		SpirvDisassemble(spirv.data(), spirv.size(), &disassembly);
		printf("%s\n", disassembly.c_str());
		printf("Validate failed\n");
		*err_msg = String8::FromPrintf("%s\n\nValidate failed:\n%s\n", Log::RemoveColors(String::FromUtf8(disassembly.c_str())).C_Str(),
		                               error_msg.c_str());
		return false;
	}

	bool optimize = true;
	if (android_fex_structural_profile)
	{
		// Adreno rejects some unoptimized helper/function shapes emitted by the
		// graphics lowerer. Use SPIRV-Tools' complete, validated size profile:
		// the smaller hand-picked subset leaves shader shapes which the driver can
		// still reject at graphics-pipeline creation time. The selector confines
		// this to explicitly opted-in Android ARM64/FEX graphics shaders and excludes
		// DebugPrintf; other platforms and the ordinary Android profile are unchanged.
		opt.RegisterSizePasses();
	} else switch (optimization)
	{
		case Config::ShaderOptimizationType::Performance: opt.RegisterPerformancePasses(); break;
		case Config::ShaderOptimizationType::Size: opt.RegisterSizePasses(); break;
		default: optimize = false; break;
	}

	if (optimize && !opt.Run(spirv.data(), spirv.size(), &spirv))
	{
		printf("Optimize failed\n");
		*err_msg = String8::FromPrintf("Optimize failed\n");
		return false;
	}
	if (android_fex_structural_profile && !core.Validate(spirv))
	{
		*err_msg = String8::FromPrintf("Android FEX structural profile validation failed:\n%s\n", error_msg.c_str());
		return false;
	}

	dst->Add(spirv.data(), spirv.size());

	return true;
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool ShaderDx10ClampOptimizationContractTest()
{
	using Optimization                  = Config::ShaderOptimizationType;
	constexpr Optimization configured[] = {Optimization::None, Optimization::Size, Optimization::Performance};

	ShaderCode enabled;
	enabled.SetType(ShaderType::Vertex);
	ShaderCode disabled;
	disabled.SetType(ShaderType::Pixel);
	disabled.SetDx10ClampMode(ShaderDx10ClampMode::Disabled);
	ShaderCode compute;
	compute.SetType(ShaderType::Compute);
	compute.SetDx10ClampMode(ShaderDx10ClampMode::Disabled);

	for (const auto profile: configured)
	{
		if (ShaderEffectiveOptimization(enabled, profile) != profile ||
		    ShaderEffectiveOptimization(disabled, profile) != Optimization::None ||
		    ShaderEffectiveOptimization(compute, profile) != profile)
		{
			return false;
		}
	}
	return true;
}

bool ShaderVMulLegacyF32OptimizationContractTest()
{
	using Optimization                  = Config::ShaderOptimizationType;
	constexpr Optimization configured[] = {Optimization::None, Optimization::Size, Optimization::Performance};

	ShaderCode legacy;
	legacy.SetType(ShaderType::Vertex);
	ShaderInstruction legacy_mul;
	legacy_mul.type = ShaderInstructionType::VMulLegacyF32;
	legacy.GetInstructions().Add(legacy_mul);

	ShaderCode ordinary;
	ordinary.SetType(ShaderType::Vertex);
	ShaderInstruction ordinary_mul;
	ordinary_mul.type = ShaderInstructionType::VMulF32;
	ordinary.GetInstructions().Add(ordinary_mul);

	for (const auto profile: configured)
	{
		if (ShaderEffectiveOptimization(legacy, profile) != Optimization::None || ShaderEffectiveOptimization(ordinary, profile) != profile)
		{
			return false;
		}
	}
	return true;
}

bool ShaderVMacLegacyF32OptimizationContractTest()
{
	using Optimization                  = Config::ShaderOptimizationType;
	constexpr Optimization configured[] = {Optimization::None, Optimization::Size, Optimization::Performance};

	ShaderCode legacy;
	legacy.SetType(ShaderType::Vertex);
	ShaderInstruction legacy_mac;
	legacy_mac.type = ShaderInstructionType::VMacLegacyF32;
	legacy.GetInstructions().Add(legacy_mac);

	ShaderCode ordinary;
	ordinary.SetType(ShaderType::Vertex);
	ShaderInstruction ordinary_mac;
	ordinary_mac.type = ShaderInstructionType::VMacF32;
	ordinary.GetInstructions().Add(ordinary_mac);

	for (const auto profile: configured)
	{
		if (ShaderEffectiveOptimization(legacy, profile) != Optimization::None || ShaderEffectiveOptimization(ordinary, profile) != profile)
		{
			return false;
		}
	}
	return true;
}

bool ShaderVRsqClampF32OptimizationContractTest()
{
	using Optimization                  = Config::ShaderOptimizationType;
	constexpr Optimization configured[] = {Optimization::None, Optimization::Size, Optimization::Performance};

	ShaderCode diagnostic;
	diagnostic.SetType(ShaderType::Vertex);
	ShaderInstruction clamp_rsq;
	clamp_rsq.type = ShaderInstructionType::VRsqClampF32;
	diagnostic.GetInstructions().Add(clamp_rsq);

	ShaderCode ordinary;
	ordinary.SetType(ShaderType::Vertex);
	ShaderInstruction ordinary_rsq;
	ordinary_rsq.type = ShaderInstructionType::VRsqF32;
	ordinary.GetInstructions().Add(ordinary_rsq);

	for (const auto profile: configured)
	{
		if (ShaderEffectiveOptimization(diagnostic, profile) != Optimization::None ||
		    ShaderEffectiveOptimization(ordinary, profile) != profile)
		{
			return false;
		}
	}
	return true;
}

bool ShaderAndroidFexStructuralProfileSelectorContractTest()
{
	ShaderCode vmac;
	vmac.SetType(ShaderType::Vertex);
	ShaderInstruction vmac_instruction;
	vmac_instruction.type = ShaderInstructionType::VMacLegacyF32;
	vmac.GetInstructions().Add(vmac_instruction);

	ShaderCode vrsq;
	vrsq.SetType(ShaderType::Vertex);
	ShaderInstruction vrsq_instruction;
	vrsq_instruction.type = ShaderInstructionType::VRsqClampF32;
	vrsq.GetInstructions().Add(vrsq_instruction);

	if (!ShaderAndroidFexStructuralProfileSelected(vmac, true, true, false) ||
	    !ShaderAndroidFexStructuralProfileSelected(vrsq, true, true, false) ||
	    ShaderAndroidFexStructuralProfileSelected(vmac, false, true, false) ||
	    ShaderAndroidFexStructuralProfileSelected(vmac, true, false, false) ||
	    ShaderAndroidFexStructuralProfileSelected(vmac, true, true, true))
	{
		return false;
	}

	ShaderCode debug_vmac = vmac;
	debug_vmac.GetDebugPrintfs().Add(ShaderDebugPrintf {});
	if (ShaderAndroidFexStructuralProfileSelected(debug_vmac, true, true, false)) return false;

	ShaderCode pixel_vmac = vmac;
	pixel_vmac.SetType(ShaderType::Pixel);
	ShaderCode compute_vrsq = vrsq;
	compute_vrsq.SetType(ShaderType::Compute);
	if (!ShaderAndroidFexStructuralProfileSelected(pixel_vmac, true, true, false) ||
	    ShaderAndroidFexStructuralProfileSelected(compute_vrsq, true, true, false))
	{
		return false;
	}

	ShaderCode legacy_mul;
	legacy_mul.SetType(ShaderType::Vertex);
	legacy_mul.SetDx10ClampMode(ShaderDx10ClampMode::Disabled);
	ShaderInstruction legacy_mul_instruction;
	legacy_mul_instruction.type = ShaderInstructionType::VMulLegacyF32;
	legacy_mul.GetInstructions().Add(legacy_mul_instruction);

	ShaderCode ordinary;
	ordinary.SetType(ShaderType::Vertex);
	ordinary.SetDx10ClampMode(ShaderDx10ClampMode::Disabled);
	ShaderInstruction ordinary_instruction;
	ordinary_instruction.type = ShaderInstructionType::VMulF32;
	ordinary.GetInstructions().Add(ordinary_instruction);
	return ShaderAndroidFexStructuralProfileSelected(legacy_mul, true, true, false) &&
	       ShaderAndroidFexStructuralProfileSelected(ordinary, true, true, false);
}

bool ShaderStorageWriteAuthorityContractTest()
{
	const auto rejected = [](const ShaderCode& code, const ShaderBindResources& bind, ShaderType stage = ShaderType::Compute) {
		const auto proof = ShaderClassifyStorageBufferWrites(code, stage, bind);
		return !proof.complete && proof.no_store_mask == 0 && proof.write_mask == 0;
	};
	const auto fixture = [](uint32_t opcode, int target, bool permutation, bool branch,
	                        ShaderCode* code, ShaderComputeInputInfo* input) {
		input->threads_num[0] = input->threads_num[1] = input->threads_num[2] = 1;
		input->thread_ids_num = 1;
		input->bind.storage_buffers.buffers_num = 3;
		for (int i = 0; i < 3; ++i)
		{
			input->bind.storage_buffers.start_register[i] = 4 * (permutation ? (i + 2) % 3 : i);
			input->bind.storage_buffers.slots[i] = i;
			input->bind.storage_buffers.usages[i] = i == target ? ShaderStorageUsage::ReadWrite : ShaderStorageUsage::ReadOnly;
		}
		ShaderCalcBindingIndices(&input->bind);
		const int store = input->bind.storage_buffers.start_register[target];
		const int before = input->bind.storage_buffers.start_register[(target + 1) % 3];
		const int after = input->bind.storage_buffers.start_register[(target + 2) % 3];
		// Original ISA: load float4, overwrite another descriptor, optionally
		// branch over a store, store one/two lanes, overwrite the last descriptor.
		// These are independent fixtures, not guest shader bytes.
		uint32_t words[16] {};
		uint32_t count = 0;
		words[count++] = 0xe0000000u | (3u << 18u) | (1u << 13u);
		words[count++] = 0x80000100u | (static_cast<uint32_t>(after / 4) << 16u);
		words[count++] = 0xbe800382u | (static_cast<uint32_t>(before) << 16u);
		if (branch) words[count++] = 0xbf880003u;
		words[count++] = 0xe0000000u | (opcode << 18u) | (1u << 13u);
		words[count++] = 0x80000100u | (static_cast<uint32_t>(store / 4) << 16u);
		words[count++] = 0xbe800380u | (static_cast<uint32_t>(after) << 16u);
		words[count++] = 0xbf810000u;
		code->SetType(ShaderType::Compute);
		return ShaderTryParseBounded(words, count + 4, code) == count && code->GetInstructions().Size() == (branch ? 6u : 5u);
	};
	uint32_t native_modules = 0;
	for (const uint32_t opcode: {0x04u, 0x05u, 0x1cu})
		for (int target = 0; target < 3; ++target)
			for (const bool permutation: {false, true})
				for (const bool branch: {false, true})
				{
					ShaderCode code;
					ShaderComputeInputInfo input {};
					if (!fixture(opcode, target, permutation, branch, &code, &input))
					{
						std::fprintf(stderr, "storage write authority original ISA fixture decode failed\n");
						return false;
					}
					const auto proof = ShaderClassifyStorageBufferWrites(code, ShaderType::Compute, input.bind);
					const uint32_t written = 1u << static_cast<unsigned>(target);
					if (!proof.complete || proof.write_mask != written || proof.no_store_mask != (7u & ~written))
					{
						std::fprintf(stderr, "storage write authority classification failed opcode=%u target=%d permutation=%u branch=%u"
						                     " complete=%u writes=%u no_stores=%u\n", opcode, target, permutation ? 1u : 0u,
						             branch ? 1u : 0u, proof.complete ? 1u : 0u, proof.write_mask, proof.no_store_mask);
						return false;
					}
					const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
					const auto reg = input.bind.storage_buffers.start_register[target];
					if (!source.ContainsStr(String8::FromPrintf("%%vsharp_s%d = OpAccessChain %%_ptr_PushConstant_uint %%vsharp %%int_0 %%int_%d %%int_0", reg, target)) ||
					    !source.ContainsStr(String8::FromPrintf("OpStore %%s%d %%vsharp_value_s%d", reg, reg)) ||
					    !source.ContainsStr(String8::FromPrintf("OpLoad %%uint %%s%d", reg)) ||
					    source.ContainsStr("OpDecorate %buf NonWritable"))
					{
						std::fprintf(stderr, "storage write authority native prologue/index/writable annotation failed\n");
						return false;
					}
					Vector<uint32_t> module;
					String8 error;
					if (!SpirvRun(source, &module, &error, Config::ShaderOptimizationType::None))
					{
						std::fprintf(stderr, "storage write authority assembly failed: %s\n", error.c_str());
						return false;
					}
					spvtools::SpirvTools validator(SPIRV_TARGET_ENV);
					validator.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& pos, const char* message) {
						std::fprintf(stderr, "storage write authority SPIR-V %zu: %s\n", pos.index, message);
					});
					if (!validator.Validate(module.GetDataConst(), module.Size())) return false;
					++native_modules;
				}
	ShaderCode baseline;
	ShaderComputeInputInfo input {};
	if (!fixture(0x04u, 1, false, true, &baseline, &input)) return false;
	uint32_t negative_cases = 0;
	for (int scenario = 0; scenario < 43; ++scenario)
	{
		auto code = baseline;
		auto bind = input.bind;
		auto& insts = code.GetInstructions();
		auto& store = insts[3];
		switch (scenario)
		{
			case 0: store.src[1].size = 3; break;
			case 1: store.src[1].size = -1; break;
			case 2: store.src[1].register_id = -1; break;
			case 3: store.src[1].register_id = INT32_MAX; break;
			case 4: store.src[1].register_id = 12; break;
			case 5: store.src[1].absolute = true; break;
			case 6: store.src[1].negate = true; break;
			case 7: store.src[1].multiplier = 2.0f; break;
			case 8: store.src[1].type = ShaderOperandType::Vgpr; break;
			case 9: store.src[1].constant.u = 1; break;
			case 10: store.type = ShaderInstructionType::Unknown; break;
			case 11: insts[1].type = ShaderInstructionType::Unknown; break;
			case 12: store.format = ShaderInstructionFormat::Unknown; break;
			case 13: store.globally_coherent = true; break;
			case 14: store.src[3].type = ShaderOperandType::Sgpr; break;
			case 15: store.src_num = 5; break;
			case 16: insts[1].dst.register_id = 4; break; // Before store.
			case 17: insts[4].dst.register_id = 7; break; // After store; full tuple.
			case 18: insts[4].dst.size = INT32_MAX; break;
			case 19: insts[4].dst2 = store.src[1]; break;
			case 20: insts[1].type = ShaderInstructionType::SSwappcB64; break;
			case 21: insts[1].type = ShaderInstructionType::SSetpcB64; break;
			case 22: code.GetLabels().Clear(); break;
			case 23: code.GetIndirectLabels().Clear(); break;
			case 24: insts[2].src[0].constant.i = 8; break; // Target metadata mismatch.
			case 25: code.GetIndirectLabels().Add(ShaderLabel(0, 0)); break;
			case 26: insts[4].pc += 1; break;
			case 27: insts[4].pc = insts[3].pc; break;
			case 28: insts[5] = insts[4]; insts[5].pc = 28; break;
			case 29: bind.storage_buffers.start_register[0] = 4; break;
			case 30: bind.storage_buffers.start_register[0] = 2; break;
			case 31: bind.storage_buffers.start_register[1] = 13; break;
			case 32: bind.storage_buffers.extended[1] = true; break;
			case 33: bind.extended.used = true; break;
			case 34: bind.gds_pointers.pointers_num = 1; break;
			case 35: bind.textures2D.textures_num = 1; bind.textures2D.desc[0].start_register = 4; break;
			case 36: bind.samplers.samplers_num = 1; bind.samplers.start_register[0] = 4; break;
			case 37: bind.direct_sgprs.sgprs_num = 1; bind.direct_sgprs.start_register[0] = 4; break;
			case 38: bind.storage_buffers.buffers_num = ShaderStorageResources::BUFFERS_MAX + 1; break;
			case 39: bind.push_constant_size -= 16; break;
			case 40: code.SetVsEmbedded(true); break;
			case 41: code.SetType(ShaderType::Pixel); break;
			case 42: // Back edge plus descriptor write is rejected regardless of path.
				insts[2].src[0].constant.i = -16;
				code.GetLabels().Clear(); code.GetLabels().Add(ShaderLabel(insts[2]));
				insts[4].dst.register_id = 4;
				break;
		}
		// Replacing the only store deliberately enters the preserved no-store
		// profile, so scenario 10 needs a second known store to test unknown IR.
		if (scenario == 10) { insts[4] = baseline.GetInstructions().At(3); insts[4].pc = 24; }
		if (!rejected(code, bind))
		{
			std::fprintf(stderr, "storage write authority negative scenario %d admitted\n", scenario);
			return false;
		}
		++negative_cases;
	}
	// Original VOP3 words use the encoded zero third source retained by the
	// decoder. CMPX writes s[0:1], reads s6/v0 and leaves s[4:7] immutable;
	// the carry forms exercise the same ordinary-SGPR effect through dst2.
	for (const uint32_t opcode: {0xd4u, 0xd6u, 0x125u, 0x126u, 0x127u, 0x11bu})
	{
		const bool compare = opcode < 0x100u;
		const bool carry_form = opcode >= 0x125u;
		const uint32_t alu_dst = compare ? 0u : 2u;
		const uint32_t words[11] {
			0xe0000000u | (3u << 18u) | (1u << 13u), 0x80000100u,
			0xd0000000u | (opcode << 17u) | alu_dst, 6u | (256u << 9u),
			0xe0000000u | (4u << 18u) | (1u << 13u), 0x80010100u, 0xbf810000u,
		};
		ShaderCode code;
		code.SetType(ShaderType::Compute);
		if (ShaderTryParseBounded(words, 11, &code) != 7 || code.GetInstructions().Size() != 4) return false;
		const auto& alu = code.GetInstructions().At(1);
		if (alu.src_num != 2 || alu.src[2].type != ShaderOperandType::Sgpr || alu.src[2].register_id != 0 || alu.src[2].size != 1 ||
		    ((compare || carry_form) && (compare ? alu.dst : alu.dst2).type != ShaderOperandType::Sgpr)) return false;
		const auto proof = ShaderClassifyStorageBufferWrites(code, ShaderType::Compute, input.bind);
		if (!proof.complete || proof.write_mask != 2 || proof.no_store_mask != 5)
		{
			std::fprintf(stderr, "storage write authority canonical VOP3 opcode %u rejected\n", opcode);
			return false;
		}
		const auto source = SpirvGenerateSource(code, nullptr, nullptr, &input);
		Vector<uint32_t> module;
		String8 error;
		spvtools::SpirvTools validator(SPIRV_TARGET_ENV);
		if (source.ContainsStr("OpDecorate %buf NonWritable") || !source.ContainsStr("OpLoad %uint %s4") ||
		    !SpirvRun(source, &module, &error, Config::ShaderOptimizationType::None) ||
		    !validator.Validate(module.GetDataConst(), module.Size())) return false;
		++native_modules;
		for (int variant = 0; variant < 12; ++variant)
		{
			auto malformed = code;
			auto& instruction = malformed.GetInstructions()[1];
			auto& ignored = instruction.src[2];
			switch (variant)
			{
				case 0: ignored.register_id = 1; break;
				case 1: ignored.register_id = -1; break;
				case 2: ignored.register_id = INT32_MAX; break;
				case 3: ignored.size = 0; break;
				case 4: ignored.size = 2; break;
				case 5: ignored.absolute = true; break;
				case 6: ignored.negate = true; break;
				case 7: ignored.clamp = true; break;
				case 8: ignored.multiplier = 2.0f; break;
				case 9: ignored.constant.u = 1; break;
				case 10: ignored.type = ShaderOperandType::Vgpr; break;
				case 11: instruction.src[3] = ignored; break;
			}
			if (!rejected(malformed, input.bind)) return false;
			++negative_cases;
		}
		if (compare || carry_form)
		{
			auto mutated = code;
			(compare ? mutated.GetInstructions()[1].dst : mutated.GetInstructions()[1].dst2).register_id = 4;
			if (!rejected(mutated, input.bind)) return false;
			++negative_cases;
		}
	}
	// Destination two can hold a comparison/carry mask in ordinary SGPRs.
	auto carry = baseline;
	ShaderInstruction add {};
	add.pc = 8; add.type = ShaderInstructionType::VAddI32; add.format = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
	add.src_num = 2; add.dst = baseline.GetInstructions().At(0).src[0];
	add.dst2 = baseline.GetInstructions().At(3).src[1]; add.dst2.size = 2;
	add.src[0] = add.src[1] = add.dst;
	carry.GetInstructions()[1] = add;
	if (!rejected(carry, input.bind) || !rejected(baseline, input.bind, ShaderType::Vertex) ||
	    !rejected(baseline, input.bind, ShaderType::Unknown)) return false;
	for (const auto stage: {ShaderType::Vertex, ShaderType::Pixel, ShaderType::Compute})
	{
		auto read = baseline; read.SetType(stage);
		read.GetInstructions()[3].type = ShaderInstructionType::BufferLoadFormatX;
		const auto proof = ShaderClassifyStorageBufferWrites(read, stage, input.bind);
		if (!proof.complete || proof.no_store_mask != 7 || proof.write_mask != 0) return false;
	}
	std::fprintf(stderr, "[GFX_STORAGE_WRITE_AUTHORITY_TEST] native_modules=%u negative_cases=%u"
	                     " stores=3 permutations=2 static_branch=1 immutable_tuple=1 dst2_revocation=1 stage_match=1 canonical_vop3=6\n",
	             native_modules, negative_cases + 3);
	return true;
}

bool ShaderDx10ClampSpirvPassThroughContractTest()
{
	const String8 source = R"(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %runtime_array ArrayStride 4
OpMemberDecorate %block 0 Offset 0
OpDecorate %block BufferBlock
OpDecorate %data DescriptorSet 0
OpDecorate %data Binding 0
%void = OpTypeVoid
%function = OpTypeFunction %void
%float = OpTypeFloat 32
%int = OpTypeInt 32 1
%int_0 = OpConstant %int 0
%int_1 = OpConstant %int 1
%int_2 = OpConstant %int 2
%float_0 = OpConstant %float 0
%runtime_array = OpTypeRuntimeArray %float
%block = OpTypeStruct %runtime_array
%ptr_uniform_block = OpTypePointer Uniform %block
%ptr_uniform_float = OpTypePointer Uniform %float
%data = OpVariable %ptr_uniform_block Uniform
%main = OpFunction %void None %function
%entry = OpLabel
%input_ptr = OpAccessChain %ptr_uniform_float %data %int_0 %int_0
%input = OpLoad %float %input_ptr
%multiply = OpFMul %float %input %float_0
%divide = OpFDiv %float %float_0 %input
%multiply_ptr = OpAccessChain %ptr_uniform_float %data %int_0 %int_1
%divide_ptr = OpAccessChain %ptr_uniform_float %data %int_0 %int_2
OpStore %multiply_ptr %multiply
OpStore %divide_ptr %divide
OpReturn
OpFunctionEnd
)";

	Vector<uint32_t> module;
	String8          error;
	if (!SpirvRun(source, &module, &error, Config::ShaderOptimizationType::None))
	{
		std::fprintf(stderr, "DX10 clamp SPIR-V assembly failed: %s\n", error.c_str());
		return false;
	}

	spvtools::SpirvTools validator(SPIRV_TARGET_ENV);
	validator.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                             { std::fprintf(stderr, "DX10 clamp SPIR-V %zu: %s\n", position.index, message); });
	if (!validator.Validate(module.GetDataConst(), module.Size()))
	{
		std::fprintf(stderr, "DX10 clamp SPIR-V validation failed\n");
		return false;
	}

	String8 disassembly;
	if (!SpirvDisassemble(module.GetDataConst(), module.Size(), &disassembly))
	{
		std::fprintf(stderr, "DX10 clamp SPIR-V disassembly failed\n");
		return false;
	}
	const std::string_view text(disassembly.c_str(), disassembly.Size());
	const bool operations_preserved = text.find("OpFMul") != std::string_view::npos && text.find("OpFDiv") != std::string_view::npos;
	if (!operations_preserved)
	{
		std::fprintf(stderr, "DX10 clamp SPIR-V arithmetic instructions were removed\n");
	}
	return operations_preserved;
}
#endif

static const ShaderBinaryInfo* GetBinaryInfo(const uint32_t* code)
{
	code = ResolveOwnedShaderWords(code, 2 * sizeof(uint32_t));
	EXIT_IF(code == nullptr);
	const auto address = reinterpret_cast<uint64_t>(code);
	EXIT_IF(!ShaderPrepareMemoryRead(address, 2 * sizeof(uint32_t)));

	if (code[0] == 0xBEEB03FF)
	{
		const uint64_t code_bytes = (static_cast<uint64_t>(code[1]) + 1u) * 8u;
		EXIT_IF(code_bytes > UINT32_MAX || address > UINT64_MAX - code_bytes - sizeof(ShaderBinaryInfo));
		// Early input decoding publishes the complete code/usage/header span
		// before a CP retains its recording buffer for later pipeline parsing.
		EXIT_IF(!ShaderPrepareMemoryRead(address, code_bytes + sizeof(ShaderBinaryInfo)));
		return reinterpret_cast<const ShaderBinaryInfo*>(address + code_bytes);
	}

	return nullptr;
}

static ShaderUsageInfo GetUsageSlots(const uint32_t* code)
{
	EXIT_IF(code == nullptr);

	const auto* binary_info = GetBinaryInfo(code);

	ShaderUsageInfo ret;

	if (binary_info != nullptr)
	{
		const auto code_address   = reinterpret_cast<uintptr_t>(code);
		const auto binary_address = reinterpret_cast<uintptr_t>(binary_info);
		if (binary_address < code_address || ((binary_address - code_address) % sizeof(uint32_t)) != 0)
		{
			return ret;
		}
		const size_t preceding_dwords = (binary_address - code_address) / sizeof(uint32_t);
		const size_t usage_offset     = binary_info->chunk_usage_base_offset_dw;
		const size_t slots_num        = binary_info->num_input_usage_slots;
		if (usage_offset > preceding_dwords || slots_num > preceding_dwords - usage_offset)
		{
			return ret;
		}

		// An offset of zero stores the compact usage slots directly before the
		// binary info. Nonzero offsets preserve the slots -> masks -> info layout.
		const auto* binary_words = reinterpret_cast<const uint32_t*>(binary_info);
		ret.usage_masks          = usage_offset == 0 ? nullptr : binary_words - usage_offset;
		ret.slots_num   = binary_info->num_input_usage_slots;
		const auto* slots_end    = usage_offset == 0 ? reinterpret_cast<const ShaderUsageSlot*>(binary_info)
		                                             : reinterpret_cast<const ShaderUsageSlot*>(ret.usage_masks);
		ret.slots                = ret.slots_num == 0 ? nullptr : slots_end - ret.slots_num;
		ret.valid       = true;
	}

	return ret;
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool ShaderUsageLayoutContractTest()
{
	static_assert(sizeof(ShaderUsageSlot) == sizeof(uint32_t));
	struct Fixture
	{
		uint32_t         code[16] = {};
		ShaderBinaryInfo info {};
	};
	static_assert(offsetof(Fixture, info) == 16 * sizeof(uint32_t));

	Fixture fixture;
	fixture.code[0]                      = 0xbeeb03ffu;
	fixture.code[1]                      = 7;
	const ShaderUsageSlot slot_fixture[] = {
	    {0x1b, 1, 2, 0},
	    {0x00, 0, 4, 3},
	    {0x02, 0, 12, 0},
	    {0x04, 0, 16, 3},
	};
	std::memcpy(fixture.code + 12, slot_fixture, sizeof(slot_fixture));
	const auto* inline_slots                = reinterpret_cast<const ShaderUsageSlot*>(fixture.code + 12);
	fixture.info.chunk_usage_base_offset_dw = 0;
	fixture.info.num_input_usage_slots      = 4;
	auto usage                              = GetUsageSlots(fixture.code);
	if (!usage.valid || usage.usage_masks != nullptr || usage.slots != inline_slots || usage.slots_num != 4 ||
	    usage.slots[0].type != 0x1b || usage.slots[0].slot != 1 || usage.slots[0].start_register != 2 || usage.slots[3].type != 0x04 ||
	    usage.slots[3].start_register != 16 || usage.slots[3].flags != 3)
	{
		return false;
	}

	fixture.info.num_input_usage_slots = 0;
	usage                              = GetUsageSlots(fixture.code);
	if (!usage.valid || usage.usage_masks != nullptr || usage.slots != nullptr || usage.slots_num != 0)
	{
		return false;
	}

	fixture.info.chunk_usage_base_offset_dw = 2;
	fixture.info.num_input_usage_slots      = 2;
	usage                                   = GetUsageSlots(fixture.code);
	if (!usage.valid || usage.usage_masks != fixture.code + 14 || usage.slots != inline_slots || usage.slots_num != 2)
	{
		return false;
	}

	fixture.info.chunk_usage_base_offset_dw = 17;
	if (GetUsageSlots(fixture.code).valid)
	{
		return false;
	}
	fixture.info.chunk_usage_base_offset_dw = 0;
	fixture.info.num_input_usage_slots      = 17;
	return !GetUsageSlots(fixture.code).valid;
}
#endif

static void ShaderDetectBuffers(ShaderVertexInputInfo* info)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(info == nullptr);

	info->buffers_num = 0;

	for (int ri = 0; ri < info->resources_num; ri++)
	{
		const auto& r = info->resources[ri];

		bool merged = false;
		for (int bi = 0; bi < info->buffers_num; bi++)
		{
			auto& b = info->buffers[bi];

			uint64_t stride = b.stride;

			if (stride == r.Stride())
			{
					uint64_t rbase   = r.Base44();
				uint64_t base    = std::min(rbase, b.addr);
				uint64_t offset1 = rbase - base;
				uint64_t offset2 = b.addr - base;

				if (offset1 < stride && offset2 < stride)
				{
					EXIT_NOT_IMPLEMENTED(b.num_records != r.NumRecords());
					b.addr = base;
					EXIT_NOT_IMPLEMENTED(b.attr_num >= ShaderVertexInputBuffer::ATTR_MAX);
					b.attr_indices[b.attr_num++] = ri;
					merged                       = true;
					break;
				}
			}
		}

		if (!merged)
		{
			EXIT_NOT_IMPLEMENTED(info->buffers_num >= ShaderVertexInputInfo::RES_MAX);
			int bi                            = info->buffers_num++;
				info->buffers[bi].addr            = r.Base44();
			info->buffers[bi].stride          = r.Stride();
			info->buffers[bi].num_records     = r.NumRecords();
			info->buffers[bi].attr_num        = 1;
			info->buffers[bi].attr_indices[0] = ri;
		}
	}

	for (int bi = 0; bi < info->buffers_num; bi++)
	{
		auto& b = info->buffers[bi];
		for (int ri = 0; ri < b.attr_num; ri++)
		{
				b.attr_offsets[ri] = info->resources[b.attr_indices[ri]].Base44() - b.addr;
		}
	}
}

static void ShaderParseFetch(ShaderVertexInputInfo* info, const uint32_t* fetch, const uint32_t* buffer, int table_register)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(info == nullptr || fetch == nullptr || buffer == nullptr);

	LUMEN_PROFILER_BLOCK("ShaderParseFetch::parse_code");

	ShaderCode code;
	code.SetType(ShaderType::Fetch);
	// The admitted runtime has immutable TTRACE_EN=0. Register controls that
	// could enable tracing remain unsupported; this is not a hardware default.
	code.SetThreadTraceMode(ShaderThreadTraceMode::Disabled);
	// shader_parse(0, fetch, nullptr, &code);
	if (g_owned_shader_reads != nullptr)
	{
		EXIT_IF(g_owned_shader_reads->fetch == nullptr ||
		    g_owned_shader_reads->fetch_address != reinterpret_cast<uint64_t>(fetch));
		code = *g_owned_shader_reads->fetch;
	} else ShaderParse(fetch, &code);
	const char* rejection = nullptr;
	// Record the omitted scalar effects for every external fetch. Legacy
	// non-resource calls keep their admission; a moved entry must prove these
	// effects dead before cache lookup and again in the compiler.
	info->fetch_effects_valid = ShaderVertexFetchEffectsSupported(code, info->fetch_shader_reg, table_register,
	                                                             info->fetch_written_sgprs, &rejection);
	if (ShaderVertexResourceProfileRequired(*info) && !info->fetch_effects_valid) EXIT("Vertex fetch rejected: %s\n", rejection);

	LUMEN_PROFILER_END_BLOCK;

	// printf("%s", code.DbgDump().c_str());

	LUMEN_PROFILER_BLOCK("ShaderParseFetch::check_insts");

	const auto& insts = code.GetInstructions();
	uint32_t    size  = insts.Size();
	// int         temp_register = 0;
	uint32_t temp_value[104] = {0};
	int      s_num           = 0;
	int      v_num           = 0;

	for (uint32_t i = 0; i < size; i++)
	{
		const auto& inst = insts.At(i);

		if (inst.type == ShaderInstructionType::SLoadDwordx4)
		{
			EXIT_NOT_IMPLEMENTED(inst.src[1].type != ShaderOperandType::LiteralConstant || (inst.src[1].constant.u & 3u) != 0);
			EXIT_NOT_IMPLEMENTED(inst.src[0].type != ShaderOperandType::Sgpr || inst.src[0].register_id != 2);
			EXIT_NOT_IMPLEMENTED(inst.dst.type != ShaderOperandType::Sgpr);
			EXIT_NOT_IMPLEMENTED(inst.dst.register_id < 0 || inst.dst.register_id > 100);

			uint32_t index    = inst.src[1].constant.u >> 2u;
			int      t        = inst.dst.register_id;
			const auto table_address = reinterpret_cast<uint64_t>(buffer);
			const uint64_t offset = static_cast<uint64_t>(index) * sizeof(uint32_t);
			EXIT_IF(table_address > UINT64_MAX - offset);
			const auto* values = ResolveOwnedShaderWords(reinterpret_cast<const uint32_t*>(table_address + offset), 4 * sizeof(uint32_t));
			EXIT_IF(values == nullptr || !ShaderPrepareMemoryRead(reinterpret_cast<uint64_t>(values), 4 * sizeof(uint32_t)));
			temp_value[t + 0] = values[0];
			temp_value[t + 1] = values[1];
			temp_value[t + 2] = values[2];
			temp_value[t + 3] = values[3];

			s_num++;
		}

		bool load_inst     = true;
		int  registers_num = 0;
		switch (inst.type)
		{
			case ShaderInstructionType::BufferLoadFormatX: registers_num = 1; break;
			case ShaderInstructionType::BufferLoadFormatXy: registers_num = 2; break;
			case ShaderInstructionType::BufferLoadFormatXyz: registers_num = 3; break;
			case ShaderInstructionType::BufferLoadFormatXyzw: registers_num = 4; break;
			default: load_inst = false;
		}

		if (load_inst && registers_num > 0)
		{
			// EXIT_NOT_IMPLEMENTED(!(i >= 2 && insts.At(i - 1).type == ShaderInstructionType::SWaitcnt &&
			//                       insts.At(i - 2).type == ShaderInstructionType::SLoadDwordx4));
			EXIT_NOT_IMPLEMENTED(inst.dst.type != ShaderOperandType::Vgpr);
			EXIT_NOT_IMPLEMENTED(inst.src[0].type != ShaderOperandType::Vgpr || inst.src[0].register_id != 0);
			EXIT_NOT_IMPLEMENTED(inst.src[1].type != ShaderOperandType::Sgpr);
			EXIT_NOT_IMPLEMENTED(inst.src[2].type != ShaderOperandType::IntegerInlineConstant || inst.src[2].constant.i != 0);

			EXIT_NOT_IMPLEMENTED(info->resources_num >= ShaderVertexInputInfo::RES_MAX);

			int t = inst.src[1].register_id;
			EXIT_NOT_IMPLEMENTED(t < 0 || t > 100);

			auto& r           = info->resources[info->resources_num];
			auto& rd          = info->resources_dst[info->resources_num];
			rd.register_start = inst.dst.register_id;
			rd.registers_num  = registers_num;
			r.fields[0]       = temp_value[t + 0];
			r.fields[1]       = temp_value[t + 1];
			r.fields[2]       = temp_value[t + 2];
			r.fields[3]       = temp_value[t + 3];

			info->resources_num++;

			v_num++;
		}
	}

	LUMEN_PROFILER_END_BLOCK;

	EXIT_NOT_IMPLEMENTED(s_num != v_num);
}

static bool ShaderVertexFp32RegisterStateSupported(const HW::VsShaderResource1& state)
{
	return state.float_mode == 0xc0 && !state.ieee_mode;
}

static ShaderCode ShaderVertexResourceProfileCode(const HW::VsShaderResource1& state)
{
	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	// Cache entries share the same immutable runtime trace contract.
	code.SetThreadTraceMode(ShaderThreadTraceMode::Disabled);
	if (!state.dx10_clamp)
	{
		code.SetDx10ClampMode(ShaderDx10ClampMode::Disabled);
	}
	if (ShaderVertexFp32RegisterStateSupported(state))
	{
		code.SetFp32Mode(ShaderFp32Mode::RneFlushInOut);
	}
	return code;
}

static ShaderCode ShaderParseVertexResourceProfile(const uint32_t* source, const HW::VsShaderResource1& state)
{
	auto code = ShaderVertexResourceProfileCode(state);
	if (const auto* owned = ShaderOwnedVertexSource(reinterpret_cast<uint64_t>(source)))
	{
		EXIT_IF(ShaderTryParseBounded(owned->Data(), owned->CodeWords(), &code) == 0);
	} else
	{
		ShaderParse(source, &code);
	}
	return code;
}

static bool ShaderVertexParseCacheEnabled()
{
	static const bool enabled = [] {
#if ((LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID) && \
     LUMEN_ABI == LUMEN_ABI_ARM64_V8A) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_GFX_VERTEX_PARSE_CACHE");
		// Active ARM64 configurations require FEX. Use public platform/ABI
		// macros so library and contract-test translation units agree.
		// Product default on the three active targets; every explicit value
		// other than exact 1 retains the ordinary parser as rollback.
		return value == nullptr || std::strcmp(value, "1") == 0;
#else
		return false;
#endif
	}();
	return enabled;
}

// Only decoder output is retained. The caller must prove current resource
// declarations, SGPR provenance and FETCH effects again on every draw.
class ShaderVertexParseCache
{
public:
	static constexpr size_t CAPACITY = 16;
	static constexpr size_t BYTE_CAP = 8 * 1024 * 1024;
	static constexpr uint32_t MAX_SOURCE_DWORDS = 16384;
	static constexpr bool CHECKED_COPY = LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS;
	explicit ShaderVertexParseCache(bool report = false, size_t byte_cap = BYTE_CAP, bool checked_copy = CHECKED_COPY)
	    : m_byte_cap(byte_cap), m_report(report), m_checked_copy(checked_copy) {}
	~ShaderVertexParseCache() { Report(); }

	const ShaderCode* Get(const uint32_t* source, const HW::VsShaderResource1& state, bool* hit = nullptr,
	                      bool* access_refused = nullptr)
	{
		const auto generation = g_vertex_parse_cache_generation.load(std::memory_order_relaxed);
		if (m_generation != generation)
		{
			Report();
			for (auto& entry: m_entries) entry.reset();
			m_bytes = m_size = m_next = 0;
			m_recent = CAPACITY;
			m_checks = m_hits = m_misses = m_fallbacks = m_refusals = 0;
			m_source_reads = 0;
			m_generation = generation;
		}
		bool found = false;
		bool refused = false;
		const auto* code = m_checked_copy ? GetImpl<true>(source, state, &found, &refused)
		                                  : GetImpl<false>(source, state, &found, &refused);
		++m_checks;
		if (found) ++m_hits;
		else if (code != nullptr) ++m_misses;
		else if (refused) ++m_refusals;
		else ++m_fallbacks;
		if (hit != nullptr) *hit = found;
		if (access_refused != nullptr) *access_refused = refused;
		if ((m_checks & (m_checks - 1)) == 0) Report();
		return code;
	}
	[[nodiscard]] size_t Size() const { return m_size; }
	[[nodiscard]] size_t Bytes() const { return m_bytes; }
	[[nodiscard]] uint64_t SourceReads() const { return m_source_reads; }

private:
	template <bool Copy>
	struct SourceRead
	{
		Core::VirtualMemory::GuestDataLease lease;
		// Default initialization deliberately leaves scratch bytes untouched.
		// A successful checked copy initializes exactly the requested range.
		std::array<uint32_t, Copy ? MAX_SOURCE_DWORDS : 0> scratch;
		const uint32_t* data = nullptr;
		bool Acquire(uint64_t address, uint64_t size)
		{
			if (const auto* owned = ShaderOwnedVertexSource(address))
			{
				if (size > owned->Bytes()) return false;
				data = owned->Data();
				return true;
			}
			if (!ShaderPrepareMemoryRead(address, size)) return false;
			if constexpr (Copy)
			{
				// Android/Windows use their existing checked synchronous Loader
				// reader. It does not claim Apple's mapping pinning; all later
				// comparison and decoding here consume only this owned snapshot.
				if (size > sizeof(scratch) || !Loader::ReadGuestMemory(address, scratch.data(), static_cast<size_t>(size)))
					return false;
				data = scratch.data();
			} else
			{
				if (!Core::VirtualMemory::TryAcquireGuestDataLease(address, size, Core::VirtualMemory::Mode::Read, &lease))
					return false;
				data = reinterpret_cast<const uint32_t*>(lease.HostBegin());
			}
			return true;
		}
		void Release()
		{
			Core::VirtualMemory::ReleaseGuestDataLease(&lease);
			data = nullptr;
		}
	};
	struct Entry
	{
		std::vector<uint32_t> source;
		ShaderSourceWords owned_source;
		size_t owned_code_words = 0;
		uint64_t guest_address = 0; // Lookup hint only; never a retained guest pointer.
		std::array<uint32_t, 5> metadata {};
		ShaderCode code;
		size_t bytes = 0;
		const uint32_t* Source() const { return owned_source ? owned_source.Data() : source.data(); }
		size_t SourceWords() const { return owned_source ? owned_code_words : source.size(); }
	};
	template <typename T>
	static size_t VectorBytes(const Vector<T>& values)
	{
		return sizeof(typename Vector<T>::DataType) + values.Capacity() * sizeof(T);
	}
	static size_t EntryBytes(const Entry& entry)
	{
		return sizeof(Entry) + entry.source.capacity() * sizeof(uint32_t) + entry.owned_source.Bytes() +
		       VectorBytes(entry.code.GetInstructions()) + VectorBytes(entry.code.GetLabels()) +
		       VectorBytes(entry.code.GetIndirectLabels()) + VectorBytes(entry.code.GetDebugPrintfs());
	}
	template <bool Copy>
	bool Acquire(uint64_t address, uint64_t size, SourceRead<Copy>* read)
	{
		++m_source_reads;
		return read->Acquire(address, size);
	}
	template <bool Copy>
	const ShaderCode* GetImpl(const uint32_t* source, const HW::VsShaderResource1& state, bool* hit, bool* access_refused)
	{
		if (source == nullptr) return nullptr;
		uint32_t prefix[2];
		SourceRead<Copy> read;
		const auto address = reinterpret_cast<uint64_t>(source);
		const std::array<uint32_t, 5> metadata = {state.float_mode, state.ieee_mode, state.dx10_clamp,
		    ShaderVMacLegacyF32ExperimentEnabled(), ShaderVRsqClampF32ExperimentEnabled()};
		const auto matches_address = [&](size_t slot) {
			return slot < CAPACITY && m_entries[slot] != nullptr && m_entries[slot]->guest_address == address &&
			       m_entries[slot]->metadata == metadata;
		};
		size_t hint = m_recent;
		if (!matches_address(hint))
		{
			hint = CAPACITY;
			for (size_t offset = 0; offset < CAPACITY; ++offset)
			{
				const size_t slot = (m_next + CAPACITY - 1 - offset) % CAPACITY;
				if (matches_address(slot)) { hint = slot; break; }
			}
		}
		if (hint < CAPACITY)
		{
			const auto& entry = m_entries[hint];
			// Capture already compared every byte under today's complete lease.
			// Both sides retain this immutable host allocation, so identity cannot
			// be recycled. This only reuses decoded code, never the draw's resource
			// or FETCH proof, and a raw guest address cannot grant this shortcut.
			const auto* owned = ShaderOwnedVertexSource(address);
			if (owned != nullptr && entry->owned_source.SameStorage(owned->OwnedWords()) &&
			    entry->SourceWords() == owned->CodeWords())
			{ m_recent = hint; *hit = true; return &entry->code; }
			// A prior extent saves the separate prefix read only after its
			// current readability and every byte (including the offset) agree.
			if (Acquire(address, entry->SourceWords() * sizeof(uint32_t), &read))
			{
				if (std::memcmp(entry->Source(), read.data, entry->SourceWords() * sizeof(uint32_t)) == 0)
				{ m_recent = hint; *hit = true; return &entry->code; }
				read.Release();
			} else
			{
				// A mapping can shrink at the same address. Recover only through
				// a newly checked prefix and span; no later unchecked fallback.
				*access_refused = true;
			}
		}
		if (!Acquire(address, sizeof(prefix), &read))
		{ *access_refused = true; return nullptr; }
		std::memcpy(prefix, read.data, sizeof(prefix));
		read.Release();
		if (prefix[0] != 0xbeeb03ffu) return nullptr;
		// Existing binary-info offset, widened before arithmetic. Neither its
		// declared shader length nor its unverified hashes establish identity.
		const uint64_t source_dwords = (static_cast<uint64_t>(prefix[1]) + 1u) * 2u;
		if (source_dwords < 2 || source_dwords > MAX_SOURCE_DWORDS) return nullptr;
		if (!Acquire(address, source_dwords * sizeof(uint32_t), &read))
		{ *access_refused = true; return nullptr; }
		for (size_t slot = 0; slot < CAPACITY; ++slot)
		{
			auto& entry = m_entries[slot];
			if (entry != nullptr && entry->SourceWords() == source_dwords && entry->metadata == metadata &&
			    std::memcmp(entry->Source(), read.data, source_dwords * sizeof(uint32_t)) == 0)
			{
				entry->guest_address = address;
				m_recent = slot;
				*hit = true;
				*access_refused = false;
				return &entry->code;
			}
		}
		auto candidate = std::make_unique<Entry>();
		if (const auto* owned = ShaderOwnedVertexSource(address); owned != nullptr &&
		    owned->Data() == read.data && owned->CodeWords() == source_dwords)
		{
			candidate->owned_source = owned->OwnedWords();
			candidate->owned_code_words = static_cast<size_t>(source_dwords);
		} else candidate->source.assign(read.data, read.data + source_dwords);
		read.Release();
		if (candidate->Source()[0] != prefix[0] || candidate->Source()[1] != prefix[1]) return nullptr;
		candidate->code = ShaderVertexResourceProfileCode(state);
		if (ShaderTryParseBounded(candidate->Source(), static_cast<uint32_t>(source_dwords), &candidate->code) == 0)
			return nullptr;
		candidate->metadata = metadata;
		candidate->guest_address = address;
		candidate->bytes = EntryBytes(*candidate);
		if (candidate->bytes > m_byte_cap) return nullptr;
		// Bound retained source and decoded-array capacities, not merely entry
		// count. One temporary miss is additionally bounded by the source cap.
		while (m_size == CAPACITY || m_bytes > m_byte_cap - candidate->bytes)
		{
			auto& victim = m_entries[m_next];
			if (victim != nullptr) { m_bytes -= victim->bytes; victim.reset(); --m_size; }
			m_next = (m_next + 1) % CAPACITY;
		}
		while (m_entries[m_next] != nullptr) m_next = (m_next + 1) % CAPACITY;
		const auto* result = &candidate->code;
		m_bytes += candidate->bytes;
		++m_size;
		m_entries[m_next] = std::move(candidate);
		m_recent = m_next;
		m_next = (m_next + 1) % CAPACITY;
		*access_refused = false;
		return result;
	}
	void Report() const
	{
		if (!m_report || m_checks == 0) return;
		std::fprintf(stderr, "[GFX_VERTEX_PARSE_CACHE] generation=%" PRIu64 " checks=%" PRIu64 " hits=%" PRIu64
		                     " misses=%" PRIu64 " fallbacks=%" PRIu64 " refusals=%" PRIu64
		                     " lease_attempts=%" PRIu64 " source_reads=%" PRIu64 " reader=%s entries=%zu capacity=%zu owned_bytes=%zu byte_cap=%zu"
		                     " policy=parse-only-current-draw-proof\n",
		             m_generation, m_checks, m_hits, m_misses, m_fallbacks, m_refusals, m_checked_copy ? 0 : m_source_reads,
		             m_source_reads, m_checked_copy ? "checked-copy" : "guest-lease", m_size, CAPACITY, m_bytes, m_byte_cap);
		std::fflush(stderr);
	}
	std::array<std::unique_ptr<Entry>, CAPACITY> m_entries;
	size_t m_bytes = 0;
	size_t m_size = 0;
	size_t m_next = 0;
	size_t m_recent = CAPACITY;
	size_t m_byte_cap = BYTE_CAP;
	uint64_t m_generation = 0;
	uint64_t m_checks = 0;
	uint64_t m_hits = 0;
	uint64_t m_misses = 0;
	uint64_t m_fallbacks = 0;
	uint64_t m_refusals = 0;
	uint64_t m_source_reads = 0;
	bool m_report = false;
	bool m_checked_copy = CHECKED_COPY;
};

static bool ShaderCheckVertexResourceProfile(const uint32_t* source, const HW::VsShaderResource1& state,
	                                         ShaderVertexInputInfo& input, const char** rejection, uint32_t* rejection_pc)
{
	input.parameter_exports_valid = false;
	input.parameter_export_mask = 0;
	const auto check = [&](const ShaderCode& code) {
		input.parameter_exports_valid = ShaderVertexResourceProfileSupported(code, input, rejection, rejection_pc) &&
		    ShaderVertexParameterExportsSupported(code, input, rejection, rejection_pc, &input.parameter_export_mask);
		return input.parameter_exports_valid;
	};
	if (ShaderVertexParseCacheEnabled())
	{
		thread_local ShaderVertexParseCache cache(true);
		bool access_refused = false;
		if (const auto* code = cache.Get(source, state, nullptr, &access_refused))
			return check(*code);
		// A failed checked read must never become an unchecked pointer fallback.
		// The cache requires the complete prefix, including padding, readable.
		if (access_refused) EXIT("Vertex parse snapshot guest access refused\n");
	}
	const auto code = ShaderParseVertexResourceProfile(source, state);
	return check(code);
}

static bool ShaderResolveResourceAddressing(int start_index, int direct_dwords, const uint32_t* extended_buffer, bool* extended)
{
	if (extended == nullptr || start_index < 0 || direct_dwords <= 0 || direct_dwords > HW::UserSgprInfo::SGPRS_MAX)
	{
		return false;
	}
	*extended = start_index >= HW::UserSgprInfo::SGPRS_MAX;
	return *extended ? extended_buffer != nullptr &&
	                       start_index <= HW::UserSgprInfo::SGPRS_MAX + ShaderExtendedResources::DATA_DWORDS_MAX - direct_dwords
	                 : start_index <= HW::UserSgprInfo::SGPRS_MAX - direct_dwords;
}

static const uint32_t* ShaderExtendedWords(const uint32_t* extended_buffer, int start_index, uint32_t count)
{
	const uint64_t base = reinterpret_cast<uint64_t>(extended_buffer);
	const uint64_t offset = uint64_t(start_index - HW::UserSgprInfo::SGPRS_MAX) * sizeof(uint32_t);
	EXIT_IF(base > UINT64_MAX - offset);
	const auto* words = ResolveOwnedShaderWords(reinterpret_cast<const uint32_t*>(base + offset), uint64_t(count) * sizeof(uint32_t));
	EXIT_IF(words == nullptr || !ShaderPrepareMemoryRead(reinterpret_cast<uint64_t>(words), uint64_t(count) * sizeof(uint32_t)));
	return words;
}

static void ShaderGetStorageBuffer(ShaderStorageResources* info, bool* direct_sgprs, int start_index, int slot, ShaderStorageUsage usage,
                                   const HW::UserSgprInfo& user_sgpr, const uint32_t* extended_buffer)
{
	EXIT_IF(info == nullptr);

	EXIT_NOT_IMPLEMENTED(info->buffers_num < 0 || info->buffers_num >= ShaderStorageResources::BUFFERS_MAX);

	int  index = info->buffers_num;
	bool extended;
	EXIT_NOT_IMPLEMENTED(!ShaderResolveResourceAddressing(start_index, 4, extended_buffer, &extended));
	const auto* extended_values = extended ? ShaderExtendedWords(extended_buffer, start_index, 4) : nullptr;

	info->start_register[index] = start_index;
	info->slots[index]          = slot;
	info->usages[index]         = usage;
	info->extended[index]       = extended;
	// info->extended_index[index] = extended_index;

	if (!extended)
	{
		for (int j = 0; j < 4; j++)
		{
			auto type = user_sgpr.type[start_index + j];
			EXIT_NOT_IMPLEMENTED(type != HW::UserSgprType::Vsharp && type != HW::UserSgprType::Region);

			direct_sgprs[start_index + j] = false;
		}
	}

	info->buffers[index].fields[0] = (extended ? extended_values[0] : user_sgpr.value[start_index + 0]);
	info->buffers[index].fields[1] = (extended ? extended_values[1] : user_sgpr.value[start_index + 1]);
	info->buffers[index].fields[2] = (extended ? extended_values[2] : user_sgpr.value[start_index + 2]);
	info->buffers[index].fields[3] = (extended ? extended_values[3] : user_sgpr.value[start_index + 3]);

	info->buffers_num++;
}

static void ShaderGetTextureBuffer(ShaderTextureResources* info, bool* direct_sgprs, int start_index, int slot, ShaderTextureUsage usage,
                                   const HW::UserSgprInfo& user_sgpr, const uint32_t* extended_buffer)
{
	EXIT_IF(info == nullptr);

	EXIT_NOT_IMPLEMENTED(info->textures_num < 0 || info->textures_num >= ShaderTextureResources::RES_MAX);
	// EXIT_NOT_IMPLEMENTED(info->textures_num != slot);

	int  index = info->textures_num;
	bool extended;
	EXIT_NOT_IMPLEMENTED(!ShaderResolveResourceAddressing(start_index, 8, extended_buffer, &extended));
	const auto* extended_values = extended ? ShaderExtendedWords(extended_buffer, start_index, 8) : nullptr;

	info->desc[index].start_register = start_index;
	info->desc[index].extended       = extended;
	info->desc[index].slot           = slot;
	info->desc[index].usage          = usage;

	EXIT_IF(usage == ShaderTextureUsage::Unknown);

	if (usage == ShaderTextureUsage::ReadWrite)
	{
		info->textures2d_storage_num++;
		info->desc[index].textures2d_without_sampler = true;
	} else
	{
		info->textures2d_sampled_num++;
		info->desc[index].textures2d_without_sampler = false;
	}

	if (!extended)
	{
		for (int j = 0; j < 8; j++)
		{
			auto type = user_sgpr.type[start_index + j];
			EXIT_NOT_IMPLEMENTED(type != HW::UserSgprType::Vsharp && type != HW::UserSgprType::Region);

			direct_sgprs[start_index + j] = false;
		}
	}

	for (uint32_t word = 0; word < 8; ++word)
		info->desc[index].texture.fields[word] = extended ? extended_values[word] : user_sgpr.value[start_index + word];

	info->textures_num++;
}

static void ShaderDumpTextureTypeMismatch(const char* stage, uint64_t source_addr, int usage_type, int usage_flags, int user_sgpr_num,
	                                      const HW::UserSgprInfo& user_sgpr, const ShaderTextureDescriptor& desc)
{
	// Diagnostics inspect CP history and may decode live guest bytes. A failed
	// owned analysis retains the normal rejection, without leaving its snapshot.
	if (g_owned_shader_reads != nullptr) return;
	const auto& r = desc.texture;
	dbg_printf("[GFX_DESCRIPTOR_MISMATCH] phase=shader-usage source=0x%016" PRIx64
	           " stage=%s usage_type=0x%x usage_flags=0x%x usage=%u start=%d slot=%d extended=%u sgprs=%d current_root_owned=%u\n",
	           source_addr, stage, usage_type, usage_flags, static_cast<unsigned>(desc.usage), desc.start_register, desc.slot,
	           desc.extended ? 1u : 0u, user_sgpr_num, GraphicsRunCurrentCommandRootOwned() ? 1u : 0u);
	dbg_printf("[GFX_DESCRIPTOR_MISMATCH] raw=%08" PRIx32 ":%08" PRIx32 ":%08" PRIx32 ":%08" PRIx32 ":%08" PRIx32
	           ":%08" PRIx32 ":%08" PRIx32 ":%08" PRIx32
		           " type=%u base=0x%016" PRIx64 " dfmt=%u nfmt=%u width=%u height=%u pitch=%u tile=%u memory=0x%x\n",
		           r.fields[0], r.fields[1], r.fields[2], r.fields[3], r.fields[4], r.fields[5], r.fields[6], r.fields[7], r.Type(),
		           r.Base38(), r.Dfmt(), r.Nfmt(), static_cast<unsigned>(r.Width4() + 1u), static_cast<unsigned>(r.Height4() + 1u),
		           static_cast<unsigned>(r.Pitch() + 1u), r.TileMode(), r.MemoryType());
	if (!desc.extended && desc.start_register >= 0 && desc.start_register <= HW::UserSgprInfo::SGPRS_MAX - 8)
	{
		const auto start = static_cast<uint32_t>(desc.start_register);
		dbg_printf("[GFX_DESCRIPTOR_MISMATCH] sgpr_types=%u:%u:%u:%u:%u:%u:%u:%u\n",
		           static_cast<unsigned>(user_sgpr.type[start + 0]), static_cast<unsigned>(user_sgpr.type[start + 1]),
		           static_cast<unsigned>(user_sgpr.type[start + 2]), static_cast<unsigned>(user_sgpr.type[start + 3]),
		           static_cast<unsigned>(user_sgpr.type[start + 4]), static_cast<unsigned>(user_sgpr.type[start + 5]),
		           static_cast<unsigned>(user_sgpr.type[start + 6]), static_cast<unsigned>(user_sgpr.type[start + 7]));
		GraphicsRunDumpDescriptorProvenance(stage, start, 8);
	}
	if (stage != nullptr && stage[0] == 'p' && stage[1] == 's' && stage[2] == '\0' && source_addr != 0 &&
	    r.Type() == ShaderTextureResource::TYPE_8_COMPAT)
	{
		ShaderCode code;
		code.SetType(ShaderType::Pixel);
		code.SetThreadTraceMode(ShaderThreadTraceMode::Disabled);
		ShaderParse(reinterpret_cast<const uint32_t*>(source_addr), &code);
		uint32_t image_ops = 0;
		auto writes_sgpr_window = [](const ShaderOperand& operand, int start, int count)
		{
			if (operand.type != ShaderOperandType::Sgpr)
			{
				return false;
			}
			const int operand_count = operand.size > 0 ? operand.size : 1;
			return operand.register_id < start + count && start < operand.register_id + operand_count;
		};
		for (const auto& inst: code.GetInstructions())
		{
			const bool provenance_writer = writes_sgpr_window(inst.dst, 2, 2) || writes_sgpr_window(inst.dst2, 2, 2) ||
			                               writes_sgpr_window(inst.dst, 16, 16) || writes_sgpr_window(inst.dst2, 16, 16);
			const bool control_flow = inst.type == ShaderInstructionType::SBranch || inst.type == ShaderInstructionType::SCbranchExecz ||
			                          inst.type == ShaderInstructionType::SCbranchScc0 ||
			                          inst.type == ShaderInstructionType::SCbranchScc1 ||
			                          inst.type == ShaderInstructionType::SCbranchVccz ||
			                          inst.type == ShaderInstructionType::SCbranchVccnz || inst.type == ShaderInstructionType::SSetpcB64 ||
			                          inst.type == ShaderInstructionType::SSwappcB64;
			if (control_flow)
			{
				std::fprintf(stderr, "[GFX_DESCRIPTOR_MISMATCH_TRACE] stage=ps pc=%" PRIu32 " type=%u format=%u op=%s\n", inst.pc,
				             static_cast<unsigned>(inst.type), static_cast<unsigned>(inst.format),
				             ShaderCode::DbgInstructionToStr(inst).c_str());
			}
			switch (inst.type)
			{
				case ShaderInstructionType::SLoadDwordx4:
				case ShaderInstructionType::SLoadDwordx8:
				case ShaderInstructionType::ImageGetResinfo:
				case ShaderInstructionType::ImageLoad:
				case ShaderInstructionType::ImageLoadMip:
				case ShaderInstructionType::ImageSample:
				case ShaderInstructionType::ImageSampleLz:
				case ShaderInstructionType::ImageSampleLzO:
				case ShaderInstructionType::ImageStore:
				case ShaderInstructionType::ImageStoreMip:
					std::fprintf(stderr,
					             "[GFX_DESCRIPTOR_MISMATCH_OP] stage=ps pc=%" PRIu32 " type=%u format=%u unrm=%u glc=%u op=%s\n",
					             inst.pc, static_cast<unsigned>(inst.type), static_cast<unsigned>(inst.format),
					             inst.unnormalized_coordinates ? 1u : 0u, inst.globally_coherent ? 1u : 0u,
					             ShaderCode::DbgInstructionToStr(inst).c_str());
					if (inst.type != ShaderInstructionType::SLoadDwordx4 && inst.type != ShaderInstructionType::SLoadDwordx8)
					{
						image_ops++;
					}
					break;
				default:
					if (provenance_writer)
					{
						std::fprintf(stderr,
						             "[GFX_DESCRIPTOR_MISMATCH_TRACE] stage=ps pc=%" PRIu32 " type=%u format=%u op=%s\n", inst.pc,
						             static_cast<unsigned>(inst.type), static_cast<unsigned>(inst.format),
						             ShaderCode::DbgInstructionToStr(inst).c_str());
					}
					break;
			}
		}
		std::fprintf(stderr, "[GFX_DESCRIPTOR_MISMATCH_OP] stage=ps instructions=%u image_ops=%u\n", code.GetInstructions().Size(), image_ops);
		std::fflush(stderr);
	}
}

bool ShaderTextureType8SingleLevelCompatible(const ShaderTextureResource& texture)
{
	constexpr uint32_t field1_fixed = (3u << 30u) | (10u << 20u) | (1u << 6u);
	constexpr uint32_t field3_exact = (ShaderTextureResource::TYPE_8_COMPAT << 28u) | (8u << 20u) | 0xfacu;
	return (texture.fields[1] & ~0x3fu) == field1_fixed && (texture.fields[2] & ~0x3fffu) == 0 && texture.fields[3] == field3_exact &&
	       (texture.fields[4] & ~0x07ffe000u) == 0 && texture.fields[5] == 0 && texture.fields[6] == 0 && texture.fields[7] == 0 &&
	       texture.Pitch() >= texture.Width4();
}

bool ShaderTextureType8PixelSampleCompatible(const ShaderTextureResource& texture)
{
	constexpr uint32_t field1_fixed = 10u << 20u;
	constexpr uint32_t field3_exact =
	    (ShaderTextureResource::TYPE_8_COMPAT << 28u) | (1u << 26u) | (8u << 20u) | 0xfacu;
	return (texture.fields[1] & ~0x3fu) == field1_fixed && (texture.fields[2] & ~0x3fffu) == 0 &&
	       texture.fields[3] == field3_exact && (texture.fields[4] & ~0x07ffe000u) == 0 && texture.fields[5] == 0 &&
	       texture.fields[6] == 0 && texture.fields[7] == 0 && texture.MemoryType() == 0x10 &&
	       texture.Pitch() >= texture.Width4();
}

bool ShaderTextureR8UnormSampledCompatible(const ShaderTextureResource& texture)
{
	constexpr uint32_t field1_fixed = 1u << 20u;
	constexpr uint32_t field3_exact =
	    (ShaderTextureResource::TYPE_2D << 28u) | (1u << 26u) | (8u << 20u) | DstSel(4, 0, 0, 1);
	return (texture.fields[1] & ~0x3fu) == field1_fixed && (texture.fields[2] & ~0x0fffffffu) == 0 &&
	       texture.fields[3] == field3_exact && (texture.fields[4] & ~0x07ffe000u) == 0 && texture.fields[5] == 0 &&
	       texture.fields[6] == 0 && texture.fields[7] == 0 && texture.MemoryType() == 0x10 &&
	       texture.Pitch() >= texture.Width4();
}

bool ShaderTextureR8UnormStorageCompatible(const ShaderTextureResource& texture)
{
	constexpr uint32_t field1_fixed = (3u << 30u) | (1u << 20u) | (1u << 6u);
	constexpr uint32_t field3_exact =
	    (ShaderTextureResource::TYPE_2D << 28u) | (13u << 20u) | DstSel(4, 4, 4, 4);
	return (texture.fields[1] & ~0x3fu) == field1_fixed && (texture.fields[2] & ~0x0fffffffu) == 0 &&
	       texture.fields[3] == field3_exact && (texture.fields[4] & ~0x07ffe000u) == 0 && texture.fields[5] == 0 &&
	       texture.fields[6] == 0 && texture.fields[7] == 0 && texture.MemoryType() == 0x6d &&
	       texture.Pitch() >= texture.Width4();
}

uint32_t ShaderTextureStorageViewSwizzle(const ShaderTextureResource& texture)
{
	return ShaderTextureR8UnormStorageCompatible(texture) ? DstSel(4, 5, 6, 7) : texture.DstSelXYZW();
}

ShaderStorageImageFormat ShaderGetUniformStorageImageFormat(const ShaderTextureResources& textures)
{
	ShaderStorageImageFormat format = ShaderStorageImageFormat::Invalid;
	bool                     seen   = false;
	for (int index = 0; index < textures.textures_num; ++index)
	{
		const auto& desc = textures.desc[index];
		if (desc.usage != ShaderTextureUsage::ReadWrite || desc.IsNullSampledImage())
		{
			continue;
		}
		const auto current = desc.texture.Dfmt() == 1
		                         ? (desc.texture.Nfmt() == 0 && ShaderTextureR8UnormStorageCompatible(desc.texture)
		                                ? ShaderStorageImageFormat::R8Unorm
		                                : ShaderStorageImageFormat::Invalid)
		                         : ShaderStorageImageFormat::Rgba8;
		if (current == ShaderStorageImageFormat::Invalid || (seen && current != format))
		{
			return ShaderStorageImageFormat::Invalid;
		}
		format = current;
		seen   = true;
	}
	return seen ? format : ShaderStorageImageFormat::Invalid;
}

bool ShaderTextureDescriptorDimensionSupported(const ShaderTextureDescriptor& desc, ShaderType shader_type)
{
	if (shader_type != ShaderType::Vertex && shader_type != ShaderType::Pixel && shader_type != ShaderType::Compute) return false;
	const bool pixel_stage = shader_type == ShaderType::Pixel;
	const bool compute_stage = shader_type == ShaderType::Compute;
	const bool pixel_type8_sampled = pixel_stage && desc.usage == ShaderTextureUsage::ReadOnly &&
	                                 !desc.textures2d_without_sampler && ShaderTextureType8PixelSampleCompatible(desc.texture);
	const bool dimension_supported = desc.texture.Type() == ShaderTextureResource::TYPE_2D ||
	                                 (compute_stage && ShaderTextureType8SingleLevelCompatible(desc.texture)) ||
	                                 pixel_type8_sampled;
	return dimension_supported || (ShaderNullSampledImageEnabled() && pixel_stage && desc.IsNullSampledImage());
}

static bool ShaderTextureDescriptorSupported(const char* stage, const ShaderTextureDescriptor& desc)
{
	const ShaderType shader_type = stage != nullptr && std::strcmp(stage, "vs") == 0 ? ShaderType::Vertex
	                               : stage != nullptr && std::strcmp(stage, "ps") == 0 ? ShaderType::Pixel
	                               : stage != nullptr && std::strcmp(stage, "cs") == 0 ? ShaderType::Compute
	                                                                                 : ShaderType::Unknown;
	return ShaderTextureDescriptorDimensionSupported(desc, shader_type);
}

uint8_t ShaderGetUniformTextureType(const ShaderTextureResources& textures, ShaderTextureUsage usage)
{
	uint8_t type = 0;
	for (int i = 0; i < textures.textures_num; i++)
	{
		const auto& desc = textures.desc[i];
		if (desc.usage != usage || desc.IsNullSampledImage())
		{
			continue;
		}
		const auto current = desc.texture.Type();
		if (type != 0 && type != current)
		{
			return 0;
		}
		type = current;
	}
	// The opt-in null sampled image uses the existing physical 2D fallback.
	return type == 0 && usage == ShaderTextureUsage::ReadOnly ? ShaderTextureResource::TYPE_2D : type;
}

static void ShaderGetSampler(const char* stage, uint64_t shader_address, ShaderSamplerResources* info, bool* direct_sgprs,
	                         int start_index, int slot, int user_sgpr_num, const HW::UserSgprInfo& user_sgpr,
	                         const uint32_t* extended_buffer)
{
	EXIT_IF(info == nullptr);

	EXIT_NOT_IMPLEMENTED(info->samplers_num < 0 || info->samplers_num >= ShaderSamplerResources::RES_MAX);
	// EXIT_NOT_IMPLEMENTED(info->samplers_num != slot);

	int  index = info->samplers_num;
	bool extended;
	EXIT_NOT_IMPLEMENTED(!ShaderResolveResourceAddressing(start_index, 4, extended_buffer, &extended));
	const auto* extended_values = extended ? ShaderExtendedWords(extended_buffer, start_index, 4) : nullptr;

	info->start_register[index] = start_index;
	info->extended[index]       = extended;
	info->slots[index]          = slot;
	for (uint32_t word = 0; word < 4; ++word)
		info->samplers[index].fields[word] = extended ? extended_values[word] : user_sgpr.value[start_index + word];

	if (!extended)
	{
		bool unknown_fallback = false;
		const bool supported_provenance =
		    ShaderSamplerDirectSgprsSupported(user_sgpr, start_index, user_sgpr_num, info->samplers[index], &unknown_fallback);
		if (!supported_provenance)
		{
			std::fprintf(stderr,
			             "[GFX_SAMPLER_SGPR_UNSUPPORTED] stage=%s shader=0x%016" PRIx64
			             " start=%d slot=%d types=%u:%u:%u:%u values=%08" PRIx32 ":%08" PRIx32 ":%08" PRIx32 ":%08" PRIx32
			             "\n",
			             stage == nullptr ? "?" : stage, shader_address, start_index, slot,
			             static_cast<unsigned>(user_sgpr.type[start_index + 0]),
			             static_cast<unsigned>(user_sgpr.type[start_index + 1]),
			             static_cast<unsigned>(user_sgpr.type[start_index + 2]),
			             static_cast<unsigned>(user_sgpr.type[start_index + 3]), user_sgpr.value[start_index + 0],
			             user_sgpr.value[start_index + 1], user_sgpr.value[start_index + 2], user_sgpr.value[start_index + 3]);
			if (g_owned_shader_reads == nullptr) GraphicsRunDumpDescriptorProvenance(stage, start_index, 4);
		}
		EXIT_NOT_IMPLEMENTED(!supported_provenance);
		if (unknown_fallback)
		{
			static std::atomic_uint receipt_stages {0};
			const uint32_t stage_bit = stage != nullptr && stage[0] == 'p' ? 1u : (stage != nullptr && stage[0] == 'v' ? 2u : 4u);
			if ((receipt_stages.fetch_or(stage_bit, std::memory_order_relaxed) & stage_bit) == 0)
			{
				std::fprintf(stderr,
				             "[GFX_SAMPLER_SGPR_FALLBACK] stage=%s policy=usage-plus-amd-layout provenance=all-unknown action=sampler\n",
				             stage == nullptr ? "?" : stage);
			}
		}
		for (int j = 0; j < 4; j++)
		{
			direct_sgprs[start_index + j] = false;
		}
	}

	info->samplers_num++;
}

static void ShaderGetGdsPointer(ShaderGdsResources* info, bool* direct_sgprs, int start_index, int slot, const HW::UserSgprInfo& user_sgpr,
                                const uint32_t* extended_buffer)
{
	EXIT_IF(info == nullptr);

	EXIT_NOT_IMPLEMENTED(info->pointers_num < 0 || info->pointers_num >= ShaderGdsResources::POINTERS_MAX);
	// EXIT_NOT_IMPLEMENTED(info->pointers_num != slot);

	int  index = info->pointers_num;
	bool extended;
	EXIT_NOT_IMPLEMENTED(!ShaderResolveResourceAddressing(start_index, 1, extended_buffer, &extended));
	const auto* extended_values = extended ? ShaderExtendedWords(extended_buffer, start_index, 1) : nullptr;

	info->start_register[index] = start_index;
	info->extended[index]       = extended;
	info->slots[index]          = slot;

	if (!extended)
	{
		auto type = user_sgpr.type[start_index];
		EXIT_NOT_IMPLEMENTED(type != HW::UserSgprType::Unknown);

		direct_sgprs[start_index] = false;
	}

	info->pointers[index].field = (extended ? extended_values[0] : user_sgpr.value[start_index]);

	info->pointers_num++;
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool ShaderResourceRoutingContractTest()
{
	uint32_t extended_words[8] = {
	    0x10111213u, 0x20212223u, 0x30313233u, 0x40414243u, 0x50515253u, 0x60616263u, 0x70717273u, 0x80818283u,
	};
	bool extended = true;
	if (!ShaderResolveResourceAddressing(4, 8, extended_words, &extended) || extended ||
	    !ShaderResolveResourceAddressing(16, 8, extended_words, &extended) || !extended ||
	    !ShaderResolveResourceAddressing(72, 8, extended_words, &extended) || !extended ||
	    ShaderResolveResourceAddressing(73, 8, extended_words, &extended) ||
	    !ShaderResolveResourceAddressing(76, 4, extended_words, &extended) || !extended ||
	    ShaderResolveResourceAddressing(77, 4, extended_words, &extended) ||
	    !ShaderResolveResourceAddressing(79, 1, extended_words, &extended) || !extended ||
	    ShaderResolveResourceAddressing(80, 1, extended_words, &extended) || ShaderResolveResourceAddressing(16, 8, nullptr, &extended) ||
	    ShaderResolveResourceAddressing(9, 8, extended_words, &extended) ||
	    ShaderResolveResourceAddressing(-1, 4, extended_words, &extended))
	{
		return false;
	}

	HW::UserSgprInfo user_sgpr;
	for (uint32_t i = 0; i < HW::UserSgprInfo::SGPRS_MAX; ++i)
	{
		user_sgpr.value[i] = 0xa0000000u + i;
		user_sgpr.type[i]  = HW::UserSgprType::Region;
	}
	bool direct_sgprs[HW::UserSgprInfo::SGPRS_MAX];
	std::fill(std::begin(direct_sgprs), std::end(direct_sgprs), true);
	ShaderStorageResources resources;
	ShaderGetStorageBuffer(&resources, direct_sgprs, 4, 0, ShaderStorageUsage::ReadOnly, user_sgpr, extended_words);
	ShaderGetStorageBuffer(&resources, direct_sgprs, 16, 1, ShaderStorageUsage::ReadWrite, user_sgpr, extended_words);
	if (resources.buffers_num != 2 || resources.extended[0] || !resources.extended[1])
	{
		return false;
	}
	for (uint32_t i = 0; i < 4; ++i)
	{
		if (resources.buffers[0].fields[i] != user_sgpr.value[4 + i] || resources.buffers[1].fields[i] != extended_words[i] ||
		    direct_sgprs[4 + i])
		{
			return false;
		}
	}

	std::fill(std::begin(direct_sgprs), std::end(direct_sgprs), true);
	ShaderTextureResources textures;
	ShaderGetTextureBuffer(&textures, direct_sgprs, 4, 2, ShaderTextureUsage::ReadOnly, user_sgpr, extended_words);
	ShaderGetTextureBuffer(&textures, direct_sgprs, 16, 3, ShaderTextureUsage::ReadWrite, user_sgpr, extended_words);
	if (textures.textures_num != 2 || textures.textures2d_sampled_num != 1 || textures.textures2d_storage_num != 1 ||
	    textures.desc[0].extended || !textures.desc[1].extended || textures.desc[0].start_register != 4 ||
	    textures.desc[1].start_register != 16 || textures.desc[0].slot != 2 || textures.desc[1].slot != 3 ||
	    textures.desc[0].textures2d_without_sampler || !textures.desc[1].textures2d_without_sampler)
	{
		return false;
	}
	for (uint32_t i = 0; i < 8; ++i)
	{
		if (textures.desc[0].texture.fields[i] != user_sgpr.value[4 + i] || textures.desc[1].texture.fields[i] != extended_words[i] ||
		    direct_sgprs[4 + i])
		{
			return false;
		}
	}

	HW::UserSgprInfo unknown_sampler_sgprs;
	unknown_sampler_sgprs.value[12] = 0x00000092u;
	unknown_sampler_sgprs.value[13] = 0x00fff000u;
	unknown_sampler_sgprs.value[14] = 0x09500000u;
	unknown_sampler_sgprs.value[15] = 0x00000000u;
	ShaderSamplerResource observed_sampler {{0x00000092u, 0x00fff000u, 0x09500000u, 0x00000000u}};
	bool                  unknown_fallback = false;
	unknown_sampler_sgprs.count = 16;
	if (!ShaderSamplerDirectSgprsSupported(unknown_sampler_sgprs, 12, 16, observed_sampler, &unknown_fallback) || !unknown_fallback)
	{
		return false;
	}
	if (ShaderSamplerDirectSgprsSupported(unknown_sampler_sgprs, 12, 15, observed_sampler, &unknown_fallback) || unknown_fallback)
	{
		return false;
	}
	HW::UserSgprInfo short_sampler_sgprs = unknown_sampler_sgprs;
	short_sampler_sgprs.count            = 15;
	if (ShaderSamplerDirectSgprsSupported(short_sampler_sgprs, 12, 16, observed_sampler, &unknown_fallback) || unknown_fallback)
	{
		return false;
	}
	ShaderSamplerResource valid_variant = observed_sampler;
	valid_variant.fields[0] &= ~0x7u;
	valid_variant.fields[2] &= ~(0x3u << 26u);
	if (!ShaderSamplerUnknownProvenanceDescriptorSupported(valid_variant))
	{
		return false;
	}
	for (uint32_t field = 0; field < 6; ++field)
	{
		ShaderSamplerResource rejected = observed_sampler;
		switch (field)
		{
			case 0: rejected.fields[0] |= 0x80000000u; break;
			case 1: rejected.fields[2] |= 0x10000000u; break;
			case 2: rejected.fields[0] = (rejected.fields[0] & ~0x7u) | 3u; break;
			case 3:
				rejected.fields[0] = (rejected.fields[0] & ~(0x7u << 9u)) | (5u << 9u);
				rejected.fields[2] = (rejected.fields[2] & ~(0x3u << 20u)) | (2u << 20u);
				break;
			case 4: rejected.fields[1] = 0x00000fffu; break;
			case 5: rejected.fields[2] |= 1u; break;
			default: return false;
		}
		if (ShaderSamplerUnknownProvenanceDescriptorSupported(rejected))
		{
			return false;
		}
	}
	HW::UserSgprInfo mixed_sampler_sgprs = unknown_sampler_sgprs;
	mixed_sampler_sgprs.type[12]         = HW::UserSgprType::Region;
	if (ShaderSamplerDirectSgprsSupported(mixed_sampler_sgprs, 12, 16, observed_sampler, &unknown_fallback))
	{
		return false;
	}
	HW::UserSgprInfo marked_sampler_sgprs = unknown_sampler_sgprs;
	for (int index = 12; index < 16; ++index)
	{
		marked_sampler_sgprs.type[index] = HW::UserSgprType::Region;
	}
	ShaderSamplerResource marked_sampler = observed_sampler;
	marked_sampler.fields[0] |= 0x80000000u;
	if (!ShaderSamplerDirectSgprsSupported(marked_sampler_sgprs, 12, 16, marked_sampler, &unknown_fallback) || unknown_fallback)
	{
		return false;
	}
	std::fill(std::begin(direct_sgprs), std::end(direct_sgprs), true);
	ShaderSamplerResources samplers;
	ShaderGetSampler("ps", 0, &samplers, direct_sgprs, 12, 0, 16, unknown_sampler_sgprs, extended_words);
	if (samplers.samplers_num != 1 || samplers.start_register[0] != 12 || samplers.slots[0] != 0 || samplers.extended[0] ||
	    std::memcmp(samplers.samplers[0].fields, observed_sampler.fields, sizeof(observed_sampler.fields)) != 0)
	{
		return false;
	}
	for (int index = 12; index < 16; ++index)
	{
		if (direct_sgprs[index])
		{
			return false;
		}
	}
	return true;
}
#endif

static bool ShaderDirectSgprTypeSupported(HW::UserSgprType type)
{
	// Region annotates every DWORD in the following SET_SH_REG packet. The
	// shader usage table, not that packet-wide annotation, classifies individual
	// lanes, so one region may contain a direct-scalar prefix and a resource
	// tuple. Vsharp remains resource-only until an equally bounded contract is
	// established for it.
	return type == HW::UserSgprType::Unknown || type == HW::UserSgprType::Region;
}

static void ShaderGetDirectSgpr(ShaderDirectSgprsResources* info, int start_index, const HW::UserSgprInfo& user_sgpr)
{
	EXIT_IF(info == nullptr);

	EXIT_NOT_IMPLEMENTED(info->sgprs_num < 0 || info->sgprs_num >= ShaderDirectSgprsResources::SGPRS_MAX);

	int index = info->sgprs_num;

	EXIT_NOT_IMPLEMENTED(start_index < 0 || start_index >= HW::UserSgprInfo::SGPRS_MAX);

	info->start_register[index] = start_index;

	auto type = user_sgpr.type[start_index];
	EXIT_NOT_IMPLEMENTED(!ShaderDirectSgprTypeSupported(type));

	info->sgprs[index].field = user_sgpr.value[start_index];

	info->sgprs_num++;
}

static void ShaderDumpDirectSgprTypeMismatch(const char* stage, uint64_t source_addr, const ShaderUsageInfo& usages,
	                                          int user_sgpr_num, const HW::UserSgprInfo& user_sgpr, int start_index)
{
	if (g_owned_shader_reads != nullptr) return;
	const auto* source = reinterpret_cast<const uint32_t*>(source_addr);
	if (stage != nullptr && std::strcmp(stage, "vs") == 0) source = ShaderResolveVertexSource(source);
	const auto* binary_info = GetBinaryInfo(source);
	dbg_printf("[GFX_DIRECT_SGPR_MISMATCH] stage=%s source=0x%016" PRIx64
	           " declared=%d high_water=%u index=%d type=%u value=%08" PRIx32 " slots=%d",
	           stage == nullptr ? "unknown" : stage, source_addr, user_sgpr_num, user_sgpr.count, start_index,
	           static_cast<unsigned>(user_sgpr.type[start_index]), user_sgpr.value[start_index], usages.slots_num);
	if (binary_info != nullptr)
	{
		dbg_printf(" hash0=%08" PRIx32 " hash1=%08" PRIx32 " crc32=%08" PRIx32,
		           binary_info->hash0, binary_info->hash1, binary_info->crc32);
	}
	dbg_printf(" current_root_owned=%u\n", GraphicsRunCurrentCommandRootOwned() ? 1u : 0u);

	for (int i = 0; i < usages.slots_num; ++i)
	{
		const auto& usage = usages.slots[i];
		dbg_printf("[GFX_DIRECT_SGPR_MISMATCH] usage=%d type=0x%02" PRIx8 " slot=%u start=%u flags=0x%02" PRIx8 "\n",
		           i, usage.type, usage.slot, usage.start_register, usage.flags);
	}
	for (int i = 0; i < HW::UserSgprInfo::SGPRS_MAX; ++i)
	{
		dbg_printf("[GFX_DIRECT_SGPR_MISMATCH] sgpr=%d declared=%u observed=%u type=%u value=%08" PRIx32 "\n",
		           i, i < user_sgpr_num ? 1u : 0u, static_cast<uint32_t>(i) < user_sgpr.count ? 1u : 0u,
		           static_cast<unsigned>(user_sgpr.type[i]), user_sgpr.value[i]);
	}
	GraphicsRunDumpDescriptorProvenance(stage, static_cast<uint32_t>(start_index), 1);
}

void ShaderCalcBindingIndices(ShaderBindResources* bind)
{
	LUMEN_PROFILER_FUNCTION();

	int binding_index = 0;

	bind->push_constant_size = 0;

	if (bind->storage_buffers.buffers_num > 0)
	{
		bind->storage_buffers.binding_index = binding_index++;
		bind->push_constant_size += bind->storage_buffers.buffers_num * 16;
	}

	if (bind->textures2D.textures_num > 0)
	{
		bind->textures2D.binding_sampled_index = binding_index++;
		bind->textures2D.binding_storage_index = binding_index++;

		bind->push_constant_size += bind->textures2D.textures_num * 32;
	}

	if (bind->samplers.samplers_num > 0)
	{
		bind->samplers.binding_index = binding_index++;
		bind->push_constant_size += bind->samplers.samplers_num * 16;
	}

	if (bind->gds_pointers.pointers_num > 0)
	{
		bind->gds_pointers.binding_index = binding_index++;
		bind->push_constant_size += (((bind->gds_pointers.pointers_num - 1) / 4) + 1) * 16;
	}

	if (bind->direct_sgprs.sgprs_num > 0)
	{
		bind->push_constant_size += (((bind->direct_sgprs.sgprs_num - 1) / 4) + 1) * 16;
	}

	EXIT_IF((bind->push_constant_size % 16) != 0);
}

bool ShaderSelectConstantStorage(ShaderBindResources* first, ShaderBindResources* second, uint32_t max_push_constants,
                                 uint32_t max_uniform_range, uint32_t max_descriptor_sets)
{
	if (first == nullptr) return false;
	uint64_t span = 0;
	for (const auto* bind: {first, second})
	{
		if (bind == nullptr) continue;
		const auto count_fits = [](int count, int maximum) { return count >= 0 && count <= maximum; };
		if (!count_fits(bind->storage_buffers.buffers_num, ShaderStorageResources::BUFFERS_MAX) ||
		    !count_fits(bind->textures2D.textures_num, ShaderTextureResources::RES_MAX) ||
		    !count_fits(bind->samplers.samplers_num, ShaderSamplerResources::RES_MAX) ||
		    !count_fits(bind->gds_pointers.pointers_num, ShaderGdsResources::POINTERS_MAX) ||
		    !count_fits(bind->direct_sgprs.sgprs_num, ShaderDirectSgprsResources::SGPRS_MAX)) return false;
		if ((bind->push_constant_offset % 16) != 0 || (bind->push_constant_size % 16) != 0 ||
		    bind->push_constant_size > ShaderBindResources::CONSTANT_DATA_MAX) return false;
		if (bind->push_constant_size != 0)
			span = std::max(span, uint64_t{bind->push_constant_offset} + bind->push_constant_size);
	}
	if (second != nullptr && first->push_constant_size != 0 && second->push_constant_size != 0 &&
	    uint64_t{second->push_constant_offset} < uint64_t{first->push_constant_offset} + first->push_constant_size) return false;
	const bool uniform = span > max_push_constants;
	if (uniform && (span > max_uniform_range || max_descriptor_sets <= ShaderBindResources::UNIFORM_DESCRIPTOR_SET)) return false;
	first->constant_storage = uniform ? ShaderConstantStorage::UniformBuffer : ShaderConstantStorage::PushConstants;
	if (second != nullptr) second->constant_storage = first->constant_storage;
	return true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static bool ShaderVertexUserSgprStateSupported(int declared, int high_water)
{
	return declared >= 0 && declared <= HW::UserSgprInfo::SGPRS_MAX && high_water >= 0 &&
	       high_water <= HW::UserSgprInfo::SGPRS_MAX;
}

static bool ShaderVertexUsageTupleFits(const ShaderUsageSlot& usage, int declared, int available)
{
	int width = 0;
	bool pointer = false;
	switch (usage.type)
	{
		case 0x00: case 0x04: width = usage.flags == 3 ? 8 : 4; break;
		case 0x01: case 0x02: width = 4; break;
		case 0x12: case 0x17: case 0x1b: width = 2; pointer = true; break;
		default: return true; // Other kinds retain their existing parser veto.
	}
	if (!ShaderVertexUserSgprStateSupported(declared, available)) return false;
	if (usage.start_register >= HW::UserSgprInfo::SGPRS_MAX) return !pointer;
	return usage.start_register + width <= declared && usage.start_register + width <= available;
}

void ShaderParseUsage(const char* stage, uint64_t addr, ShaderParsedUsage* info, ShaderBindResources* bind,
	                  const HW::UserSgprInfo& user_sgpr, int user_sgpr_num)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(bind == nullptr);
	EXIT_IF(info == nullptr);

	const auto* src = reinterpret_cast<const uint32_t*>(addr);
	if (std::strcmp(stage, "vs") == 0) src = ShaderResolveVertexSource(src);
	src = ResolveOwnedShaderWords(src, 2 * sizeof(uint32_t));
	EXIT_IF(src == nullptr);

	auto usages = GetUsageSlots(src);

	EXIT_NOT_IMPLEMENTED(!usages.valid);
	if (std::strcmp(stage, "vs") == 0)
		for (int i = 0; i < usages.slots_num; ++i)
			EXIT_NOT_IMPLEMENTED(!ShaderVertexUsageTupleFits(usages.slots[i], user_sgpr_num, user_sgpr.count));

	info->fetch                     = false;
	info->fetch_reg                 = 0;
	info->vertex_buffer             = false;
	info->vertex_buffer_reg         = 0;
	info->storage_buffers_readonly  = 0;
	info->storage_buffers_constant  = 0;
	info->storage_buffers_readwrite = 0;
	info->textures2D_readonly       = 0;
	info->textures2D_readwrite      = 0;
	info->extended_buffer           = false;
	info->samplers                  = 0;
	info->gds_pointers              = 0;
	info->direct_sgprs              = 0;

	uint32_t* extended_buffer = nullptr;

	bool direct_sgprs[HW::UserSgprInfo::SGPRS_MAX];
	for (int i = 0; i < HW::UserSgprInfo::SGPRS_MAX; i++)
	{
		direct_sgprs[i] = (i < user_sgpr_num);
	}

	// Resolve the extension table before consuming resource records. Usage slots
	// describe a set, so resource routing must not depend on where the pointer
	// record happens to appear in that set.
	for (int i = 0; i < usages.slots_num; i++)
	{
		const auto& usage = usages.slots[i];
		if (usage.type != 0x1b)
		{
			continue;
		}
		EXIT_NOT_IMPLEMENTED(usage.flags != 0);
		EXIT_NOT_IMPLEMENTED(usage.slot != 1);
		EXIT_NOT_IMPLEMENTED(bind->extended.used);
		EXIT_NOT_IMPLEMENTED(usage.start_register + 1 >= HW::UserSgprInfo::SGPRS_MAX);
		EXIT_NOT_IMPLEMENTED(std::strcmp(stage, "vs") == 0 &&
		                     (usage.start_register + 1 >= user_sgpr_num || usage.start_register + 1 >= user_sgpr.count));
		bind->extended.used                    = true;
		bind->extended.slot                    = usage.slot;
		bind->extended.start_register          = usage.start_register;
		bind->extended.data.fields[0]          = user_sgpr.value[usage.start_register];
		bind->extended.data.fields[1]          = user_sgpr.value[usage.start_register + 1];
		extended_buffer                        = reinterpret_cast<uint32_t*>(bind->extended.data.Base());
		info->extended_buffer                  = true;
		direct_sgprs[usage.start_register]     = false;
		direct_sgprs[usage.start_register + 1] = false;
	}

	for (int i = 0; i < usages.slots_num; i++)
	{
		const auto& usage = usages.slots[i];
		switch (usage.type)
		{
			case 0x00:
				EXIT_NOT_IMPLEMENTED(usage.flags != 0 && usage.flags != 3);
				if (usage.flags == 0)
				{
					ShaderGetStorageBuffer(&bind->storage_buffers, direct_sgprs, usage.start_register, usage.slot,
					                       ShaderStorageUsage::ReadOnly, user_sgpr, extended_buffer);
					info->storage_buffers_readonly++;
				} else if (usage.flags == 3)
				{
					ShaderGetTextureBuffer(&bind->textures2D, direct_sgprs, usage.start_register, usage.slot, ShaderTextureUsage::ReadOnly,
					                       user_sgpr, extended_buffer);
					info->textures2D_readonly++;
					if (!ShaderTextureDescriptorSupported(stage, bind->textures2D.desc[bind->textures2D.textures_num - 1]))
					{
						ShaderDumpTextureTypeMismatch(stage, addr, usage.type, usage.flags, user_sgpr_num, user_sgpr,
						                              bind->textures2D.desc[bind->textures2D.textures_num - 1]);
					}
					EXIT_NOT_IMPLEMENTED(!ShaderTextureDescriptorSupported(stage,
					                                                               bind->textures2D.desc[bind->textures2D.textures_num - 1]));
				}
				break;

			case 0x01:
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				ShaderGetSampler(stage, addr, &bind->samplers, direct_sgprs, usage.start_register, usage.slot, user_sgpr_num, user_sgpr,
				                 extended_buffer);
				info->samplers++;
				break;

			case 0x02:
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				ShaderGetStorageBuffer(&bind->storage_buffers, direct_sgprs, usage.start_register, usage.slot, ShaderStorageUsage::Constant,
				                       user_sgpr, extended_buffer);
				info->storage_buffers_constant++;
				break;

			case 0x04:
				EXIT_NOT_IMPLEMENTED(usage.flags != 0 && usage.flags != 3);
				if (usage.flags == 0)
				{
					ShaderGetStorageBuffer(&bind->storage_buffers, direct_sgprs, usage.start_register, usage.slot,
					                       ShaderStorageUsage::ReadWrite, user_sgpr, extended_buffer);
					info->storage_buffers_readwrite++;
				} else if (usage.flags == 3)
				{
					ShaderGetTextureBuffer(&bind->textures2D, direct_sgprs, usage.start_register, usage.slot, ShaderTextureUsage::ReadWrite,
					                       user_sgpr, extended_buffer);
					info->textures2D_readwrite++;
					if (!ShaderTextureDescriptorSupported(stage, bind->textures2D.desc[bind->textures2D.textures_num - 1]))
					{
						ShaderDumpTextureTypeMismatch(stage, addr, usage.type, usage.flags, user_sgpr_num, user_sgpr,
						                              bind->textures2D.desc[bind->textures2D.textures_num - 1]);
					}
					EXIT_NOT_IMPLEMENTED(!ShaderTextureDescriptorSupported(stage,
					                                                               bind->textures2D.desc[bind->textures2D.textures_num - 1]));
				}
				break;

			case 0x07:
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				ShaderGetGdsPointer(&bind->gds_pointers, direct_sgprs, usage.start_register, usage.slot, user_sgpr, extended_buffer);
				info->gds_pointers++;
				break;

			case 0x12:
				EXIT_NOT_IMPLEMENTED(usage.slot != 0);
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				EXIT_NOT_IMPLEMENTED(usage.start_register + 1 >= HW::UserSgprInfo::SGPRS_MAX);
				info->fetch                            = true;
				info->fetch_reg                        = usage.start_register;
				direct_sgprs[usage.start_register]     = false;
				direct_sgprs[usage.start_register + 1] = false;
				break;

			case 0x17:
				EXIT_NOT_IMPLEMENTED(usage.slot != 0);
				EXIT_NOT_IMPLEMENTED(usage.flags != 0);
				EXIT_NOT_IMPLEMENTED(usage.start_register + 1 >= HW::UserSgprInfo::SGPRS_MAX);
				info->vertex_buffer                    = true;
				info->vertex_buffer_reg                = usage.start_register;
				direct_sgprs[usage.start_register]     = false;
				direct_sgprs[usage.start_register + 1] = false;
				break;

			case 0x1b:
				// Already validated and resolved by the order-independent pre-pass.
				break;

			default: EXIT("unknown usage type: 0x%02" PRIx8 "\n", usage.type);
		}
	}

	for (int i = 0; i < HW::UserSgprInfo::SGPRS_MAX; i++)
	{
		if (direct_sgprs[i])
		{
			if (!ShaderDirectSgprTypeSupported(user_sgpr.type[i]))
			{
				ShaderDumpDirectSgprTypeMismatch(stage, addr, usages, user_sgpr_num, user_sgpr, i);
			}
			ShaderGetDirectSgpr(&bind->direct_sgprs, i, user_sgpr);
			info->direct_sgprs++;
		}
	}

	// Sampled 1D and 2D descriptors share the existing physical host-2D image
	// representation. Per-instruction validation/lowering resolves their guest
	// dimensionality; storage images retain the established uniform-type gate.
	EXIT_NOT_IMPLEMENTED(bind->textures2D.textures2d_storage_num > 0 &&
	                     ShaderGetUniformTextureType(bind->textures2D, ShaderTextureUsage::ReadWrite) == 0);
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool ShaderUsageOrderContractTest()
{
	struct Fixture
	{
		uint32_t         code[16] = {};
		ShaderBinaryInfo info {};
	};
	static_assert(offsetof(Fixture, info) == 16 * sizeof(uint32_t));

	auto initialize_fixture = [](Fixture* fixture, bool extension_first)
	{
		fixture->code[0]                         = 0xbeeb03ffu;
		fixture->code[1]                         = 7;
		fixture->info.chunk_usage_base_offset_dw = 0;
		fixture->info.num_input_usage_slots      = 2;
		const ShaderUsageSlot extension          = {0x1b, 1, 2, 0};
		const ShaderUsageSlot resource           = {0x02, 0, 16, 0};
		const ShaderUsageSlot slots[2]           = {extension_first ? extension : resource, extension_first ? resource : extension};
		std::memcpy(fixture->code + 14, slots, sizeof(slots));
	};

	Fixture extension_first;
	Fixture resource_first;
	initialize_fixture(&extension_first, true);
	initialize_fixture(&resource_first, false);

	uint32_t         extended_words[4] = {0x10111213u, 0x20212223u, 0x30313233u, 0x40414243u};
	const auto       extended_address  = reinterpret_cast<uint64_t>(extended_words);
	HW::UserSgprInfo user_sgpr;
	user_sgpr.value[0] = 0xa0a0a0a0u;
	user_sgpr.value[1] = 0xb1b1b1b1u;
	user_sgpr.value[2] = static_cast<uint32_t>(extended_address);
	user_sgpr.value[3] = static_cast<uint32_t>(extended_address >> 32u);

	ShaderParsedUsage   extension_first_usage;
	ShaderParsedUsage   resource_first_usage;
	ShaderBindResources extension_first_bind;
	ShaderBindResources resource_first_bind;
	ShaderParseUsage("cs", reinterpret_cast<uint64_t>(extension_first.code), &extension_first_usage, &extension_first_bind, user_sgpr, 4);
	ShaderParseUsage("cs", reinterpret_cast<uint64_t>(resource_first.code), &resource_first_usage, &resource_first_bind, user_sgpr, 4);

	auto valid_result = [extended_address, &extended_words](const ShaderParsedUsage& usage, const ShaderBindResources& bind)
	{
		if (!usage.extended_buffer || usage.storage_buffers_constant != 1 || usage.direct_sgprs != 2 || !bind.extended.used ||
		    bind.extended.start_register != 2 || bind.extended.data.Base() != extended_address || bind.storage_buffers.buffers_num != 1 ||
		    bind.storage_buffers.start_register[0] != 16 || !bind.storage_buffers.extended[0] || bind.direct_sgprs.sgprs_num != 2 ||
		    bind.direct_sgprs.start_register[0] != 0 || bind.direct_sgprs.start_register[1] != 1)
		{
			return false;
		}
		for (int i = 0; i < 4; i++)
		{
			if (bind.storage_buffers.buffers[0].fields[i] != extended_words[i])
			{
				return false;
			}
		}
		return true;
	};

	if (!valid_result(extension_first_usage, extension_first_bind) || !valid_result(resource_first_usage, resource_first_bind))
	{
		return false;
	}
	for (int i = 0; i < 4; i++)
	{
		if (extension_first_bind.storage_buffers.buffers[0].fields[i] != resource_first_bind.storage_buffers.buffers[0].fields[i])
		{
			return false;
		}
	}
	return true;
}
#endif

static bool ShaderReadDiagnosticBytes(uint64_t address, void* destination, uint32_t size)
{
	if (address == 0 || destination == nullptr || size == 0 || size > 65536 || address > UINT64_MAX - size) return false;
	if (!ShaderPrepareMemoryRead(address, size)) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	uint64_t cursor = address;
	for (unsigned segments = 0; cursor < address + size; ++segments)
	{
		Core::VirtualMemory::ManagedRegion region {};
		if (segments == 256 || Core::VirtualMemory::QueryManagedRegion(cursor, &region) !=
		                           Core::VirtualMemory::ManagedRegionState::Visible ||
		    (static_cast<uint32_t>(region.mode) & 1u) == 0 || region.end <= cursor) return false;
		cursor = region.end;
	}
	std::memcpy(destination, reinterpret_cast<const void*>(address), size);
	return true;
#else
	return false;
#endif
}

static void ShaderDumpDiagnosticBinary(uint64_t address)
{
	if (g_owned_shader_reads != nullptr) return;
	uint32_t prefix[2] {};
	if (!ShaderReadDiagnosticBytes(address, prefix, sizeof(prefix)) || prefix[0] != 0xbeeb03ffu)
	{
		std::fprintf(stderr, "[GFX_VS_EXTENDED_BINARY] available=0 reason=prefix_unavailable_or_invalid cap=65536\n");
		return;
	}
	// Preserve code, usage slots and binary info together, as in the existing
	// resource capture. Checked 64-bit arithmetic precedes the bounded read.
	const uint64_t header_offset = (static_cast<uint64_t>(prefix[1]) + 1u) * 8u;
	if (header_offset > 65536u - sizeof(ShaderBinaryInfo) || address > UINT64_MAX - header_offset - sizeof(ShaderBinaryInfo))
	{
		std::fprintf(stderr, "[GFX_VS_EXTENDED_BINARY] available=0 reason=header_offset_cap_or_overflow header_offset=%" PRIu64
		                     " cap=65536\n", header_offset);
		return;
	}
	const auto size = static_cast<uint32_t>(header_offset + sizeof(ShaderBinaryInfo));
	std::vector<uint8_t> bytes(size);
	if (!ShaderReadDiagnosticBytes(address, bytes.data(), size))
	{
		std::fprintf(stderr, "[GFX_VS_EXTENDED_BINARY] available=0 reason=span_unreadable requested=%u cap=65536\n", size);
		return;
	}
	uint32_t captured_prefix[2];
	std::memcpy(captured_prefix, bytes.data(), sizeof(captured_prefix));
	if (captured_prefix[0] != prefix[0] || captured_prefix[1] != prefix[1])
	{
		std::fprintf(stderr, "[GFX_VS_EXTENDED_BINARY] available=0 reason=prefix_changed cap=65536\n");
		return;
	}
	ShaderBinaryInfo header;
	std::memcpy(&header, bytes.data() + header_offset, sizeof(header));
	std::fprintf(stderr, "[GFX_VS_EXTENDED_BINARY] available=1 captured=%u header_offset=%" PRIu64
	                     " declared_length=%u hash0=%08" PRIx32 " crc32=%08" PRIx32
	                     " encoding=byte_hex cap=65536 atomic_snapshot=0\n",
	             size, header_offset, unsigned(header.length), header.hash0, header.crc32);
	for (uint32_t offset = 0; offset < size; offset += 32)
	{
		std::fprintf(stderr, "[GFX_VS_EXTENDED_BYTES] offset=%u hex=", offset);
		for (uint32_t index = offset; index < size && index < offset + 32; ++index)
			std::fprintf(stderr, "%02x", static_cast<unsigned>(bytes[index]));
		std::fputc('\n', stderr);
	}
	std::fprintf(stderr, "[GFX_VS_EXTENDED_BINARY_END] captured=%u\n", size);
}

static void ShaderDumpDiagnosticFetch(const HW::VertexShaderInfo& regs, const ShaderParsedUsage& usage)
{
	if (g_owned_shader_reads != nullptr) return;
	if (!usage.fetch || !usage.vertex_buffer || usage.fetch_reg < 0 || usage.fetch_reg > 14 ||
	    usage.vertex_buffer_reg < 0 || usage.vertex_buffer_reg > 14 ||
	    unsigned(usage.fetch_reg + 2) > regs.vs_user_sgpr.count || unsigned(usage.vertex_buffer_reg + 2) > regs.vs_user_sgpr.count ||
	    unsigned(usage.fetch_reg + 2) > regs.vs_regs.rsrc2.user_sgpr || unsigned(usage.vertex_buffer_reg + 2) > regs.vs_regs.rsrc2.user_sgpr)
	{
		std::fprintf(stderr, "[GFX_VS_FETCH_END] complete_known_walk=0 reason=missing_or_invalid_pointer_pair\n");
		return;
	}
	const auto address = [&](int start) {
		return uint64_t(regs.vs_user_sgpr.value[start]) | (uint64_t(regs.vs_user_sgpr.value[start + 1]) << 32u);
	};
	const uint64_t fetch = address(usage.fetch_reg);
	const uint64_t table = address(usage.vertex_buffer_reg);
	std::fprintf(stderr, "[GFX_VS_FETCH] source=%016" PRIx64 " table=%016" PRIx64
	                     " fetch_pair=%d vertex_pair=%d byte_cap=4096 instruction_cap=256 descriptor_cap=16"
	                     " descriptor_window_cap=4096 atomic_snapshot=0 admission=unchanged\n",
	             fetch, table, usage.fetch_reg, usage.vertex_buffer_reg);
	uint32_t pc = 0, instructions = 0, descriptors = 0;
	uint32_t descriptor_offsets[16] {};
	bool vertex_pair_unchanged = true;
	bool complete = false;
	const char* reason = "instruction_or_byte_cap";
	const auto word_at = [&](uint32_t offset, uint32_t* word) {
		return (fetch & 3u) == 0 && offset <= 4096 - 4 && fetch <= UINT64_MAX - offset &&
		       ShaderReadDiagnosticBytes(fetch + offset, word, sizeof(*word));
	};
	while (pc < 4096 && instructions < 256)
	{
		uint32_t words[2] {};
		if (!word_at(pc, &words[0])) { reason = "fetch_word_unreadable"; break; }
		uint32_t count = 1;
		bool descriptor_load = false;
		uint32_t descriptor_offset = 0, destination = 0, base_register = 0;
		const char* form = "unknown";
		if (words[0] == 0xbe802000u)
		{
			form = "return";
		} else if ((words[0] & 0xffff0000u) == 0xbf8c0000u)
		{
			form = "waitcnt";
		} else if ((words[0] & 0xf8000000u) == 0xc0000000u && ((words[0] >> 22u) & 0x1fu) == 2)
		{
			// The same descriptor-load fields used by Lumen's SMRD parser and
			// ShaderParseFetch. This recognizer never replaces either parser.
			const bool immediate = ((words[0] >> 8u) & 1u) != 0;
			if (immediate)
			{
				descriptor_offset = (words[0] & 0xffu) * 4;
			} else if ((words[0] & 0xffu) == 0xffu && word_at(pc + 4, &words[1]))
			{
				count = 2;
				descriptor_offset = words[1];
			} else
			{
				reason = "dynamic_or_unreadable_smrd_offset";
			}
			if (immediate || count == 2)
			{
				form = "descriptor_load_x4";
				descriptor_load = true;
				base_register = ((words[0] >> 9u) & 0x3fu) * 2;
				destination = (words[0] >> 15u) & 0x7fu;
			}
		} else if ((words[0] >> 26u) == 0x38 && ((words[0] >> 18u) & 0x1fu) <= 3 &&
		           (words[0] & 0x17fffu) == 0x2000u && word_at(pc + 4, &words[1]))
		{
			count = 2;
			if ((words[1] >> 24u) == 128 && (words[1] & 0x00c000ffu) == 0)
				form = "vertex_load_format";
		}
		std::fprintf(stderr, "[GFX_VS_FETCH_WORDS] pc=%u count=%u form=%s raw=%08" PRIx32, pc, count, form, words[0]);
		if (count == 2) std::fprintf(stderr, ",%08" PRIx32, words[1]);
		std::fputc('\n', stderr);
		pc += count * 4;
		++instructions;
		if (std::strcmp(form, "unknown") == 0)
		{
			if (std::strcmp(reason, "instruction_or_byte_cap") == 0) reason = "unknown_fetch_form";
			break;
		}
		if (descriptor_load)
		{
			if (base_register != unsigned(usage.vertex_buffer_reg) || !vertex_pair_unchanged || destination > 100)
			{
				reason = "descriptor_pointer_or_destination_unresolved";
				break;
			}
			if ((descriptor_offset & 3u) != 0 || descriptor_offset > 4096 - 16 || table > UINT64_MAX - descriptor_offset)
			{
				reason = "descriptor_offset_out_of_window";
				break;
			}
			bool seen = false;
			for (uint32_t index = 0; index < descriptors; ++index) seen |= descriptor_offsets[index] == descriptor_offset;
			if (!seen)
			{
				if (descriptors == 16) { reason = "descriptor_count_cap"; break; }
				uint32_t descriptor[4] {};
				if (!ShaderReadDiagnosticBytes(table + descriptor_offset, descriptor, sizeof(descriptor)))
				{
					reason = "descriptor_span_unreadable";
					break;
				}
				std::fprintf(stderr, "[GFX_VS_FETCH_DESCRIPTOR] offset=%u bytes=16 raw=%08" PRIx32 ",%08" PRIx32
				                     ",%08" PRIx32 ",%08" PRIx32 "\n",
				             descriptor_offset, descriptor[0], descriptor[1], descriptor[2], descriptor[3]);
				descriptor_offsets[descriptors++] = descriptor_offset;
			}
			vertex_pair_unchanged &= !(destination < unsigned(usage.vertex_buffer_reg + 2) &&
			                          unsigned(usage.vertex_buffer_reg) < destination + 4);
		}
		if (std::strcmp(form, "return") == 0) { complete = true; reason = "known_return"; break; }
	}
	std::fprintf(stderr, "[GFX_VS_FETCH_END] complete_known_walk=%u reason=%s instructions=%u captured=%u descriptors=%u\n",
	             unsigned(complete), reason, instructions, pc, descriptors);
}

static void ShaderDumpVsExtendedUsage(const HW::VertexShaderInfo& regs, const HW::ShaderRegisters& shader_registers,
                                      int export_count, const ShaderParsedUsage& usage, const ShaderBindResources& bind)
{
	if (g_owned_shader_reads != nullptr) return;
	// Terminal diagnostics only. Resource words below are the descriptor copies
	// already resolved by ShaderParseUsage; never reread the extension table.
	const auto* source = ShaderResolveVertexSource(reinterpret_cast<const uint32_t*>(regs.vs_regs.data_addr));
	const auto* header = GetBinaryInfo(source);
	const auto slots = GetUsageSlots(source);
	const char* capture = std::getenv("LUMEN_GFX_RESOURCE_CAPTURE");
	const bool raw_enabled = capture != nullptr && std::strcmp(capture, "1") == 0;
	std::fprintf(stderr, "[GFX_VS_EXTENDED] action=terminal_unsupported source=%016" PRIx64
	                     " hash0=%08" PRIx32 " crc32=%08" PRIx32 " declared_length=%u usage_slots=%d"
	                     " sgprs_declared=%u sgprs_count=%u pair_start=%d pair_slot=%d pair_raw=%08" PRIx32 ",%08" PRIx32
	                     " table_base=%016" PRIx64 " fetch=%u fetch_reg=%d vertex_buffer=%u vertex_buffer_reg=%d"
	                     " storage_ro=%d storage_constant=%d storage_rw=%d texture_ro=%d texture_rw=%d samplers=%d gds=%d"
	                     " direct=%d export_count=%d raw_enabled=%u root_owned=%u\n",
	             regs.vs_regs.data_addr, header != nullptr ? header->hash0 : 0u, header != nullptr ? header->crc32 : 0u,
	             header != nullptr ? header->length : 0u, slots.valid ? slots.slots_num : -1,
	             unsigned(regs.vs_regs.rsrc2.user_sgpr), regs.vs_user_sgpr.count, bind.extended.start_register, bind.extended.slot,
	             bind.extended.data.fields[0], bind.extended.data.fields[1], bind.extended.data.Base(),
	             unsigned(usage.fetch), usage.fetch_reg, unsigned(usage.vertex_buffer), usage.vertex_buffer_reg,
	             usage.storage_buffers_readonly, usage.storage_buffers_constant, usage.storage_buffers_readwrite,
	             usage.textures2D_readonly, usage.textures2D_readwrite, usage.samplers, usage.gds_pointers, usage.direct_sgprs, export_count,
	             unsigned(raw_enabled), unsigned(GraphicsRunCurrentCommandRootOwned()));
	if (slots.valid)
	{
		for (int index = 0; index < slots.slots_num && index < 255; ++index)
		{
			const auto& slot = slots.slots[index];
			std::fprintf(stderr, "[GFX_VS_EXTENDED_USAGE] index=%d type=%02x slot=%u start=%u flags=%02x\n",
			             index, unsigned(slot.type), unsigned(slot.slot), unsigned(slot.start_register), unsigned(slot.flags));
		}
	}
	for (unsigned index = 0; index < HW::UserSgprInfo::SGPRS_MAX; ++index)
		std::fprintf(stderr, "[GFX_VS_EXTENDED_SGPR] index=%u value=%08" PRIx32 " type=%u\n",
		             index, regs.vs_user_sgpr.value[index], unsigned(regs.vs_user_sgpr.type[index]));
	const auto resource = [](const char* kind, int index, unsigned resource_usage, int slot, int start, bool extended,
	                         const uint32_t* words, unsigned count) {
		std::fprintf(stderr, "[GFX_VS_EXTENDED_RESOURCE] kind=%s index=%d usage=%u slot=%d start=%d extended=%u raw=",
		             kind, index, resource_usage, slot, start, unsigned(extended));
		for (unsigned word = 0; word < count; ++word) std::fprintf(stderr, "%s%08" PRIx32, word == 0 ? "" : ",", words[word]);
		std::fputc('\n', stderr);
	};
	for (int index = 0; index < bind.storage_buffers.buffers_num && index < ShaderStorageResources::BUFFERS_MAX; ++index)
		resource("storage", index, unsigned(bind.storage_buffers.usages[index]), bind.storage_buffers.slots[index],
		         bind.storage_buffers.start_register[index], bind.storage_buffers.extended[index], bind.storage_buffers.buffers[index].fields, 4);
	for (int index = 0; index < bind.textures2D.textures_num && index < ShaderTextureResources::RES_MAX; ++index)
	{
		const auto& desc = bind.textures2D.desc[index];
		resource("texture", index, unsigned(desc.usage), desc.slot, desc.start_register, desc.extended, desc.texture.fields, 8);
	}
	for (int index = 0; index < bind.samplers.samplers_num && index < ShaderSamplerResources::RES_MAX; ++index)
		resource("sampler", index, 0, bind.samplers.slots[index], bind.samplers.start_register[index],
		         bind.samplers.extended[index], bind.samplers.samplers[index].fields, 4);
	for (int index = 0; index < bind.gds_pointers.pointers_num && index < ShaderGdsResources::POINTERS_MAX; ++index)
		resource("gds", index, 0, bind.gds_pointers.slots[index], bind.gds_pointers.start_register[index],
		         bind.gds_pointers.extended[index], &bind.gds_pointers.pointers[index].field, 1);
	for (int index = 0; index < bind.direct_sgprs.sgprs_num && index < ShaderDirectSgprsResources::SGPRS_MAX; ++index)
		resource("direct", index, 0, 0, bind.direct_sgprs.start_register[index], false, &bind.direct_sgprs.sgprs[index].field, 1);
	std::fflush(stderr);
	if (raw_enabled)
	{
		const uint16_t byte_order = 1;
		const char* endian = *reinterpret_cast<const uint8_t*>(&byte_order) == 1 ? "little" : "big";
#if defined(__aarch64__) || defined(__arm64__)
		const char* architecture = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
		const char* architecture = "x86_64";
#else
		const char* architecture = "other";
#endif
		std::fprintf(stderr, "[GFX_VS_EXTENDED_METADATA_BEGIN] version=1 encoding=host_abi_byte_hex arch=%s endian=%s"
		                     " pointer_bytes=%zu bool_bytes=%zu same_build_layout_required=1 export_count=%d"
		                     " vs_modifier=%08" PRIx32 " embedded=%u embedded_id=%u\n",
		             architecture, endian, sizeof(void*), sizeof(bool), export_count, regs.vs_shader_modifier,
		             unsigned(regs.vs_embedded), regs.vs_embedded_id);
		const auto metadata = [](const char* name, const void* value, size_t size, size_t alignment) {
			if (size > 512)
			{
				std::fprintf(stderr, "[GFX_VS_EXTENDED_METADATA] name=%s available=0 bytes=%zu reason=metadata_cap\n", name, size);
				return;
			}
			std::fprintf(stderr, "[GFX_VS_EXTENDED_METADATA] name=%s available=1 bytes=%zu align=%zu hex=", name, size, alignment);
			const auto* bytes = reinterpret_cast<const uint8_t*>(value);
			for (size_t index = 0; index < size; ++index) std::fprintf(stderr, "%02x", unsigned(bytes[index]));
			std::fputc('\n', stderr);
		};
		// Small stage/state records only; UserSGPR values/types are already named
		// above. No Context, pipeline, guest table or resource payload is read here.
		metadata("HW::VsStageRegisters", &regs.vs_regs, sizeof(regs.vs_regs), alignof(HW::VsStageRegisters));
		metadata("HW::EsStageRegisters", &regs.es_regs, sizeof(regs.es_regs), alignof(HW::EsStageRegisters));
		metadata("HW::GsStageRegisters", &regs.gs_regs, sizeof(regs.gs_regs), alignof(HW::GsStageRegisters));
		metadata("HW::ShaderRegisters", &shader_registers, sizeof(shader_registers), alignof(HW::ShaderRegisters));
		std::fprintf(stderr, "[GFX_VS_EXTENDED_METADATA_END] records=4\n");
		ShaderDumpDiagnosticBinary(regs.vs_regs.data_addr);
		ShaderDumpDiagnosticFetch(regs, usage);
	}
	std::fflush(stderr);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void ShaderGetInputInfoVS(const HW::VertexShaderInfo* regs, const HW::ShaderRegisters* sh, ShaderVertexInputInfo* info)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(info == nullptr || regs == nullptr);

	info->export_count              = static_cast<int>(sh->GetExportCount());
	info->bind.push_constant_offset = 0;
	info->bind.push_constant_size   = 0;
	info->bind.descriptor_set_slot  = 0;

	if (regs->vs_embedded)
	{
		return;
	}

	ShaderParsedUsage usage;

	const uint64_t          shader_addr   = regs->vs_regs.data_addr;
	const HW::UserSgprInfo& user_sgpr     = regs->vs_user_sgpr;
	const auto              user_sgpr_num = regs->vs_regs.rsrc2.user_sgpr;
	EXIT_NOT_IMPLEMENTED(!ShaderVertexUserSgprStateSupported(user_sgpr_num, user_sgpr.count));

	ShaderParseUsage("vs", shader_addr, &usage, &info->bind, user_sgpr, user_sgpr_num);

	EXIT_NOT_IMPLEMENTED(usage.gds_pointers > 0);
	EXIT_NOT_IMPLEMENTED(usage.storage_buffers_readwrite > 0 || usage.textures2D_readwrite > 0);
	EXIT_NOT_IMPLEMENTED((usage.fetch && !usage.vertex_buffer) || (!usage.fetch && usage.vertex_buffer));

	if (usage.fetch && usage.vertex_buffer)
	{
		info->fetch_external   = true;
		info->fetch_inline     = false;
		info->fetch_shader_reg = usage.fetch_reg;

		EXIT_NOT_IMPLEMENTED(usage.fetch_reg + 1 >= HW::UserSgprInfo::SGPRS_MAX);
		EXIT_NOT_IMPLEMENTED(usage.vertex_buffer_reg + 1 >= HW::UserSgprInfo::SGPRS_MAX);
		EXIT_NOT_IMPLEMENTED(usage.fetch_reg + 1 >= user_sgpr_num || usage.vertex_buffer_reg + 1 >= user_sgpr_num);

		const auto* fetch = reinterpret_cast<const uint32_t*>(static_cast<uint64_t>(user_sgpr.value[usage.fetch_reg]) |
		                                                      (static_cast<uint64_t>(user_sgpr.value[usage.fetch_reg + 1]) << 32u));
		const auto* buffer =
		    reinterpret_cast<const uint32_t*>(static_cast<uint64_t>(user_sgpr.value[usage.vertex_buffer_reg]) |
		                                      (static_cast<uint64_t>(user_sgpr.value[usage.vertex_buffer_reg + 1]) << 32u));

		EXIT_NOT_IMPLEMENTED(fetch == nullptr || buffer == nullptr);

		ShaderParseFetch(info, fetch, buffer, usage.vertex_buffer_reg);
		ShaderDetectBuffers(info);
	}

	ShaderCalcBindingIndices(&info->bind);
	if (ShaderVertexResourceProfileRequired(*info) || info->fetch_external || info->export_count > 5)
	{
		// Validate every draw before module/pipeline cache lookup. The opt-in
		// decoder cache cannot replace current SGPR/FETCH provenance checks.
		const char* rejection = nullptr;
		uint32_t rejection_pc = UINT32_MAX;
		if (!ShaderCheckVertexResourceProfile(reinterpret_cast<const uint32_t*>(shader_addr), regs->vs_regs.rsrc1,
		                                     *info, &rejection, &rejection_pc))
		{
			ShaderDumpVsExtendedUsage(*regs, *sh, info->export_count, usage, info->bind);
			EXIT("Vertex resource profile rejected before cache: %s, pc=%08" PRIx32 "\n", rejection, rejection_pc);
		}
	}
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
static bool ShaderBoundedTailContractTest()
{
	HW::VsShaderResource1 state {};
	state.float_mode = 0xc0;
	const std::vector<std::vector<uint32_t>> complete {
	    {0xbe860300u, 0xbf810000u}, // One-word MOV followed immediately by END.
	    {0xf800026fu, 0x03020100u, 0xbf810000u}, // Two-word EXP plus END.
	    {0x060000f9u, 0x06060600u, 0xbf810000u}, // Complete SDWA control word plus END.
	    {0xbe8603ffu, 0xbf810000u, 0xbf810000u}, // A terminal-looking literal is data.
	    {0xbf820001u, 0xbf810000u, 0xbe860300u, 0xbf810000u}, // Retain original PCs/labels.
	    {0xbeeb03ffu, 1u, 0xbe860300u, 0xbf810000u}, // Minimal complete binary prefix.
	    {0xd2820000u, 255u | (255u << 9u) | (255u << 18u), 1u, 2u, 3u, 0xbf810000u}
	};
	const std::vector<std::vector<uint32_t>> truncated {
	    {0xbe8603ffu}, // Missing literal.
	    {0xf800026fu}, // Missing mandatory second EXP word.
	    {0x060000f9u}, // Missing SDWA control word; do not validate invented selectors.
	    {0x7c0000f9u}, // Same missing control word through VOPC.
	    {0xd2820000u, 255u | (255u << 9u) | (255u << 18u), 1u, 2u}, // Third literal absent.
	    {0xbe860300u, 0xf800026fu} // A complete prefix survives; incomplete EXP does not publish.
	};
	const auto check = [&](const uint32_t* source, const std::vector<uint32_t>& words, bool valid) {
		auto bounded = ShaderVertexResourceProfileCode(state);
		const auto consumed = ShaderTryParseBounded(source, static_cast<uint32_t>(words.size()), &bounded);
		if (!valid)
		{
			const uint32_t complete_prefix = words.size() == 2 ? 1 : 0;
			return consumed == 0 && bounded.GetInstructions().Size() == complete_prefix &&
			       bounded.GetLabels().IsEmpty() && bounded.GetIndirectLabels().IsEmpty();
		}
		auto ordinary = ShaderVertexResourceProfileCode(state);
		ShaderParse(words.data(), &ordinary);
		return consumed == words.size() && bounded.DbgDump() == ordinary.DbgDump() &&
		       bounded.GetLabels().Size() == ordinary.GetLabels().Size() &&
		       bounded.GetIndirectLabels().Size() == ordinary.GetIndirectLabels().Size();
	};
	for (const auto& words: complete) if (!check(words.data(), words, true)) return false;
	for (const auto& words: truncated) if (!check(words.data(), words, false)) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	const uint64_t allocation = Alloc(0, 32768, Mode::ReadWrite);
	if (allocation == 0) return false;
	struct Allocation { uint64_t address; ~Allocation() { Free(address); } } cleanup {allocation};
	if (!Protect(allocation + 16384, 16384, Mode::NoAccess)) return false;
	for (const bool valid: {false, true})
		for (const auto& words: valid ? complete : truncated)
		{
			auto* source = reinterpret_cast<uint32_t*>(allocation + 16384 - words.size() * sizeof(uint32_t));
			std::memcpy(source, words.data(), words.size() * sizeof(uint32_t));
			if (!check(source, words, valid)) return false;
		}
#endif
	std::fprintf(stderr, "[GFX_SHADER_BOUNDED_TAIL_TEST] complete=7 truncated=6 AST-publish=after-length-check"
	    " original-pc/labels=preserved guard-page=mac\n");
	return true;
}

static bool ShaderVertexSourceSnapshotContractTest()
{
	const auto okay = []() -> const char* { return nullptr; };
	unsigned captures = 0, pauses = 0;
	const auto pending = [&](bool* retryable) { ++captures; *retryable = true; return false; };
	for (unsigned invalid = 0; invalid < 4; ++invalid)
	{
		captures = pauses = 0;
		const auto context = [&]() { return ShaderSourceRetryContextRejection(
		    invalid == 0, invalid != 1, invalid != 2, invalid != 3); };
		const auto result = CaptureShaderSourceWithRetry(pending, context, [&] { ++pauses; }, okay);
		if (result.captured || result.reason == nullptr || captures != 0 || pauses != 0) return false;
	}
	captures = pauses = 0;
	auto result = CaptureShaderSourceWithRetry([&](bool*) { ++captures; return false; }, okay, [&] { ++pauses; }, okay);
	if (result.captured || std::strcmp(result.reason, "source-access-refused") != 0 || captures != 1 || pauses != 0) return false;
	captures = pauses = 0;
	result = CaptureShaderSourceWithRetry(pending, okay, [&] { ++pauses; }, okay, 3);
	if (result.captured || std::strcmp(result.reason, "source-admission-timeout") != 0 || captures != 3 || pauses != 2) return false;
	captures = pauses = 0;
	result = CaptureShaderSourceWithRetry(pending, okay, [&] { ++pauses; }, okay, 4096, std::chrono::milliseconds(0));
	if (result.captured || captures != 1 || pauses != 0) return false;
	// A changed recording or native draw state must stop before a second read.
	for (const char* changed : {"source-recording-changed", "source-draw-state-changed", "source-index-state-changed"})
	{
		captures = pauses = 0;
		bool identity_changed = false;
		result = CaptureShaderSourceWithRetry(pending, okay, [&] { ++pauses; identity_changed = true; },
		    [&]() -> const char* { return identity_changed ? changed : nullptr; });
		if (result.captured || result.reason != changed || captures != 1 || pauses != 1) return false;
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	// Exercise real guest-memory capture and all downstream VS consumers. Once
	// captured, the original mapping becomes inaccessible: an accidental raw
	// read or a second lease here fails instead of passing on identical bytes.
	using namespace Core::VirtualMemory;
	const uint64_t allocation = Alloc(0, 65536, Mode::ReadWrite);
	if (allocation == 0) return false;
	struct Allocation { uint64_t address; ~Allocation() { Free(address); } } cleanup {allocation};
	struct Fixture { uint32_t code[16] = {0xbeeb03ffu, 7u, 0xbe860300u, 0xbf810000u}; ShaderBinaryInfo header {}; } fixture;
	static_assert(offsetof(Fixture, header) == 64);
	fixture.header.hash0 = 0x12345678u;
	fixture.header.crc32 = 0x90abcdefu;
	fixture.header.length = 16;
	std::memcpy(reinterpret_cast<void*>(allocation), &fixture, sizeof(fixture));
	ShaderVertexSourceSnapshot snapshot;
	Core::Mutex processor;
	processor.Lock();
	bool peer_owned = false;
	captures = pauses = 0;
	result = CaptureShaderSourceWithRetry([&](bool* retryable) {
		if (++captures == 1) { *retryable = true; return false; }
		return snapshot.Capture(allocation, retryable);
	}, [&]() { return ShaderSourceRetryContextRejection(CurrentThreadHasMemoryLease(), true, true, true); }, [&] {
		++pauses;
		processor.Unlock();
		std::thread peer([&] { peer_owned = processor.TryLock(); if (peer_owned) processor.Unlock(); });
		peer.join();
		processor.Lock();
	}, okay);
	processor.Unlock();
	if (!result.captured || !peer_owned || captures != 2 || pauses != 1 || snapshot.Address() != allocation ||
	    snapshot.Bytes() != sizeof(fixture) || snapshot.CodeWords() != 16 || CurrentThreadHasMemoryLease()) return false;
	ShaderVertexSourceSnapshot repeated;
	bool repeated_retryable = true;
	if (!repeated.Capture(allocation, &repeated_retryable) || repeated_retryable ||
	    !repeated.OwnedWords().SameStorage(snapshot.OwnedWords()) || CurrentThreadHasMemoryLease()) return false;
	// The address and encoded length stay identical. Interior padding and the
	// binary-info tail are part of the equality proof, not only executable words.
	for (const size_t changed_word: {size_t {8}, sizeof(fixture) / sizeof(uint32_t) - 1})
	{
		auto* guest = reinterpret_cast<uint32_t*>(allocation);
		guest[changed_word] ^= 1u;
		ShaderVertexSourceSnapshot changed;
		if (!changed.Capture(allocation, &repeated_retryable) || repeated_retryable ||
		    changed.OwnedWords().SameStorage(snapshot.OwnedWords()) ||
		    changed.Data()[changed_word] != guest[changed_word] ||
		    snapshot.Data()[changed_word] == guest[changed_word]) return false;
		guest[changed_word] ^= 1u;
	}
	// Restore the original draw's snapshot to the latest cache slot. Its first
	// owned copy has remained unchanged throughout both replacements.
	if (std::memcmp(snapshot.Data(), &fixture, sizeof(fixture)) != 0 ||
	    !snapshot.Capture(allocation, &repeated_retryable) || repeated_retryable) return false;
	if (!Protect(allocation, 65536, Mode::NoAccess)) return false;
	HW::VertexShaderInfo regs {};
	regs.vs_regs.data_addr = allocation;
	regs.vs_regs.rsrc1.float_mode = 192;
	regs.vs_regs.rsrc1.dx10_clamp = true;
	HW::ShaderRegisters shader_regs {};
	shader_regs.m_spiShaderPosFormat = 4;
	ShaderVertexInputInfo input;
	{
		ScopedShaderVertexSource scope(&snapshot);
		ShaderParsedUsage usage;
		ShaderParseUsage("vs", allocation, &usage, &input.bind, regs.vs_user_sgpr, 0);
		ShaderVertexParseCache cache(false, ShaderVertexParseCache::BYTE_CAP, true);
		bool hit = false, refused = false;
		const auto* parsed = cache.Get(reinterpret_cast<const uint32_t*>(allocation), regs.vs_regs.rsrc1, &hit, &refused);
		if (parsed == nullptr || hit || refused || parsed->GetInstructions().Size() != 3) return false;
		const auto initial_reads = cache.SourceReads();
		if (cache.Get(reinterpret_cast<const uint32_t*>(allocation), regs.vs_regs.rsrc1, &hit, &refused) == nullptr ||
		    !hit || refused || cache.SourceReads() != initial_reads) return false;
		const auto id = ShaderGetIdVS(&regs, &input);
		const auto code = ShaderParseVS(&regs, &shader_regs);
		if (id.hash0 != fixture.header.hash0 || id.crc32 != fixture.header.crc32 || code.GetHash0() != id.hash0 ||
		    code.GetCrc32() != id.crc32 || code.GetInstructions().Size() != 3 || ShaderIsDisabled(allocation, true)) return false;
	}
	ShaderVertexSourceSnapshot denied;
	bool retryable = true;
	if (denied.Capture(allocation, &retryable) || retryable || denied.Data() != nullptr || CurrentThreadHasMemoryLease()) return false;
	if (!Protect(allocation, 65536, Mode::ReadWrite)) return false;
	fixture.header.hash0 ^= 1;
	std::memcpy(reinterpret_cast<void*>(allocation), &fixture, sizeof(fixture));
	if (ShaderGetIdVS(&regs, &input).hash0 != fixture.header.hash0) return false;
	for (unsigned invalid = 0; invalid < 3; ++invalid)
	{
		auto bad = fixture;
		if (invalid == 0) bad.code[0] = 0; else bad.code[1] = invalid == 1 ? UINT32_MAX : 0x20000000u;
		std::memcpy(reinterpret_cast<void*>(allocation), &bad, sizeof(bad));
		retryable = true;
		if (denied.Capture(allocation, &retryable) || retryable || denied.Data() != nullptr || CurrentThreadHasMemoryLease()) return false;
	}
	// Reject an unbacked encoded extent before allocating its owned copy.
	fixture.code[1] = 0x08000000u;
	std::memcpy(reinterpret_cast<void*>(allocation), &fixture, sizeof(fixture));
	retryable = true;
	if (denied.Capture(allocation, &retryable) || retryable || denied.Data() != nullptr ||
	    denied.Capture(UINT64_MAX - 3, &retryable) || retryable || CurrentThreadHasMemoryLease()) return false;
	const uint64_t large_allocation = Alloc(0, 131072, Mode::ReadWrite);
	if (large_allocation == 0) return false;
	Allocation large_cleanup {large_allocation};
	constexpr uint32_t large_code_bytes = 65544;
	fixture.code[1] = large_code_bytes / 8 - 1;
	std::memset(reinterpret_cast<void*>(large_allocation), 0, 131072);
	std::memcpy(reinterpret_cast<void*>(large_allocation), fixture.code, sizeof(fixture.code));
	std::memcpy(reinterpret_cast<void*>(large_allocation + large_code_bytes), &fixture.header, sizeof(fixture.header));
	ShaderVertexSourceSnapshot large;
	if (!large.Capture(large_allocation, &retryable) || retryable ||
	    large.Bytes() != large_code_bytes + sizeof(ShaderBinaryInfo) ||
	    !Protect(large_allocation, 131072, Mode::NoAccess)) return false;
	{
		ScopedShaderVertexSource scope(&large);
		regs.vs_regs.data_addr = large_allocation;
		ShaderVertexParseCache cache(false, ShaderVertexParseCache::BYTE_CAP, true);
		bool hit = false, refused = false;
		// Too large for the cache, but still accepted by the same owned parser.
		if (cache.Get(reinterpret_cast<const uint32_t*>(large_allocation), regs.vs_regs.rsrc1, &hit, &refused) != nullptr ||
		    hit || refused || ShaderParseVertexResourceProfile(reinterpret_cast<const uint32_t*>(large_allocation),
		        regs.vs_regs.rsrc1).GetInstructions().Size() != 3 ||
		    ShaderParseVS(&regs, &shader_regs).GetInstructions().Size() != 3 ||
		    ShaderGetIdVS(&regs, &input).hash0 != fixture.header.hash0) return false;
	}
#endif
	std::fprintf(stderr, "[GFX_VERTEX_SOURCE_SNAPSHOT_TEST] immutable-owned-reuse=full-byte-compare"
	    " interior/header-mutation=refreshed permission-denial=preserved parse-owned-identity=retained\n");
	return true;
}

bool ShaderVertexParseCacheContractTest()
{
	if (!ShaderBoundedTailContractTest() || !ShaderVertexSourceSnapshotContractTest()) return false;
	const bool selected = ShaderVertexParseCacheEnabled();
	if (const char* expected = std::getenv("LUMEN_EXPECT_VERTEX_PARSE_CACHE"))
	{
		if ((std::strcmp(expected, "0") != 0 && std::strcmp(expected, "1") != 0) ||
		    selected != (std::strcmp(expected, "1") == 0)) return false;
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	const char* old_value = std::getenv("LUMEN_EXPERIMENTAL_GFX_VERTEX_PARSE_CACHE");
	const bool had_value = old_value != nullptr;
	const std::string saved = had_value ? old_value : "";
	if (setenv("LUMEN_EXPERIMENTAL_GFX_VERTEX_PARSE_CACHE", selected ? "0" : "1", 1) != 0) return false;
	const bool latched = ShaderVertexParseCacheEnabled() == selected;
	const int restored = had_value ? setenv("LUMEN_EXPERIMENTAL_GFX_VERTEX_PARSE_CACHE", saved.c_str(), 1)
	                               : unsetenv("LUMEN_EXPERIMENTAL_GFX_VERTEX_PARSE_CACHE");
	if (!latched || restored != 0) return false;
#endif
	HW::VsShaderResource1 state {};
	state.float_mode = 0xc0;
	const uint32_t terminal = 0xbf810000u;
	const uint32_t truncated_literal = 0xbe8603ffu;
	auto bounded = ShaderVertexResourceProfileCode(state);
	if (ShaderTryParseBounded(&terminal, 1, &bounded) != 1 ||
	    ShaderTryParseBounded(&terminal, 0, &bounded) != 0 ||
	    ShaderTryParseBounded(&truncated_literal, 1, &bounded) != 0) return false;
	// A terminal-looking literal is data, and a label at the next PC makes an
	// interior END nonterminal for the existing parser.
	const uint32_t literal_end[8] = {0xbeeb03ffu, 3u, 0xbe8603ffu, 0xbf810000u, 0xbf810000u};
	if (ShaderTryParseBounded(literal_end, 8, &bounded) != 5 || bounded.GetInstructions().Size() != 3) return false;
	const uint32_t continued_end[12] = {0xbeeb03ffu, 5u, 0xbf820001u, 0xbf810000u, 0xbe860300u, 0xbf810000u};
	if (ShaderTryParseBounded(continued_end, 12, &bounded) != 6 || bounded.GetInstructions().Size() != 5 ||
	    ShaderTryParseBounded(continued_end, 4, &bounded) != 0) return false;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	// Exercise the checked-copy reader on Mac as well, using its existing
	// Loader helper; Android/Windows never invoke the unsupported lease stub.
	constexpr unsigned reader_variants = LUMEN_PLATFORM == LUMEN_PLATFORM_OSX ? 2 : 1;
	for (unsigned reader_variant = 0; reader_variant < reader_variants; ++reader_variant)
	{
		const bool checked_copy = LUMEN_PLATFORM != LUMEN_PLATFORM_OSX || reader_variant == 1;
		struct Fixture { uint32_t code[16] = {0xbeeb03ffu, 7u, 0xbe860300u, 0xbf810000u}; ShaderBinaryInfo header {}; } fixture;
		static_assert(offsetof(Fixture, header) == 16 * sizeof(uint32_t));
		fixture.header.hash0 = 0x12345678u;
		fixture.header.crc32 = 0x90abcdefu;
		ShaderVertexInputInfo input;
		input.bind.extended.used = true;
		input.bind.extended.start_register = 4;
		input.bind.direct_sgprs.sgprs_num = 1;
		ShaderVertexParseCache cache(false, ShaderVertexParseCache::BYTE_CAP, checked_copy);
		bool hit = false;
		bool refused = false;
		const auto check = [&](const uint32_t* source, const HW::VsShaderResource1& fp, const ShaderVertexInputInfo& current,
		                       bool expected_hit, bool expected_supported) {
			const auto* code = cache.Get(source, fp, &hit, &refused);
			if (code == nullptr || refused || hit != expected_hit) return false;
			const auto ordinary = ShaderParseVertexResourceProfile(source, fp);
			return code->DbgDump() == ordinary.DbgDump() && code->GetFp32Mode() == ordinary.GetFp32Mode() &&
			       code->GetDx10ClampMode() == ordinary.GetDx10ClampMode() &&
			       (ShaderVertexResourceProfileSupported(*code, current) && ShaderVertexParameterExportsSupported(*code, current)) == expected_supported &&
			       (ShaderVertexResourceProfileSupported(ordinary, current) && ShaderVertexParameterExportsSupported(ordinary, current)) == expected_supported;
		};
		if (!check(fixture.code, state, input, false, true) || cache.SourceReads() != 2) return false;
		const auto before_stable_hit = cache.SourceReads();
		if (!check(fixture.code, state, input, true, true) || cache.SourceReads() != before_stable_hit + 1) return false;
		const auto initial_bytes = cache.Bytes();
		const auto original = fixture;
		auto relocated = fixture;
		const auto before_relocation = cache.SourceReads();
		if (!check(relocated.code, state, input, true, true) || cache.SourceReads() != before_relocation + 2 ||
		    !check(relocated.code, state, input, true, true) || cache.SourceReads() != before_relocation + 3) return false;
		// Identical unverified header hashes cannot hide altered instructions.
		fixture.code[2] = 0xbe860301u; // Read undeclared s1 instead of s0.
		if (!check(fixture.code, state, input, false, false) || !check(fixture.code, state, input, true, false)) return false;
		fixture = original;
		fixture.code[15] = 1;
		if (!check(fixture.code, state, input, false, true)) return false;
		fixture = original;
		for (unsigned variant = 0; variant < 3; ++variant)
		{
			auto fp = state;
			if (variant == 0) fp.float_mode = 0;
			if (variant == 1) fp.ieee_mode = true;
			if (variant == 2) fp.dx10_clamp = true;
			if (!check(fixture.code, fp, input, false, true) || !check(fixture.code, fp, input, true, true)) return false;
		}
		// Same AST hit, different current input: provenance is checked every time.
		auto missing_scalar = input;
		missing_scalar.bind.direct_sgprs.start_register[0] = 1;
		if (!check(fixture.code, state, missing_scalar, true, false)) return false;
		fixture.code[2] = 0xbe802100u;
		fixture.code[3] = 0xbe880306u;
		fixture.code[4] = 0xbf810000u;
		auto fetch = input;
		fetch.bind.direct_sgprs.start_register[0] = 6;
		fetch.fetch_external = true;
		fetch.fetch_effects_valid = true;
		if (!check(fixture.code, state, fetch, false, true)) return false;
		fetch.fetch_written_sgprs[0] = uint64_t{1} << 6u;
		if (!check(fixture.code, state, fetch, true, false)) return false;
		fetch.fetch_written_sgprs[0] = 0;
		fetch.fetch_effects_valid = false;
		if (!check(fixture.code, state, fetch, true, false)) return false;
		// The moved entry proof remains current-input dependent on AST cache
		// hits even with no sampled/extended resources requesting the old gate.
		fixture = original;
		fixture.code[2] = 0xbefc03ffu; fixture.code[3] = 0x2468ace0u;
		fixture.code[4] = 0xbf960000u; fixture.code[5] = 0xbe802100u;
		fixture.code[6] = 0xbe880306u; fixture.code[7] = 0xbf810000u;
		ShaderVertexInputInfo moved;
		moved.fetch_external = true; moved.fetch_effects_valid = true;
		moved.fetch_written_sgprs[0] = uint64_t{15} << 16;
		moved.bind.direct_sgprs.sgprs_num = 1; moved.bind.direct_sgprs.start_register[0] = 6;
		if (ShaderVertexResourceProfileRequired(moved) || !check(fixture.code, state, moved, false, true) ||
		    !check(fixture.code, state, moved, true, true)) return false;
		moved.fetch_written_sgprs[0] |= uint64_t{1} << 6;
		if (!check(fixture.code, state, moved, true, false)) return false;
		moved.fetch_written_sgprs[0] = uint64_t{15} << 16; moved.fetch_effects_valid = false;
		if (!check(fixture.code, state, moved, true, false)) return false;
		// Same immutable AST, changed current declaration: no positive parameter
		// admission is cached. Only the statically produced PARAM6 is required.
		fixture = original;
		fixture.code[2] = 0xf800026fu; fixture.code[3] = 0x03020100u;
		fixture.code[4] = 0xbf810000u;
		ShaderVertexInputInfo parameters;
		parameters.export_count = 7;
		if (!check(fixture.code, state, parameters, false, true) || !check(fixture.code, state, parameters, true, true)) return false;
		parameters.export_count = 6;
		if (!check(fixture.code, state, parameters, true, false)) return false;
		parameters.export_count = 7;
		if (!check(fixture.code, state, parameters, true, true)) return false;
		// Exercise the actual pre-cache adapter too: failure revokes captured
		// validity/mask before the same AST becomes admissible again.
		if (!ShaderCheckVertexResourceProfile(fixture.code, state, parameters, nullptr, nullptr) ||
		    !parameters.parameter_exports_valid || parameters.parameter_export_mask != (uint32_t{1} << 6)) return false;
		parameters.export_count = 6;
		if (ShaderCheckVertexResourceProfile(fixture.code, state, parameters, nullptr, nullptr) ||
		    parameters.parameter_exports_valid || parameters.parameter_export_mask != 0) return false;
		parameters.export_count = 7;
		if (!ShaderCheckVertexResourceProfile(fixture.code, state, parameters, nullptr, nullptr) ||
		    !parameters.parameter_exports_valid || parameters.parameter_export_mask != (uint32_t{1} << 6)) return false;
		fixture = original;
		const auto* immutable = cache.Get(fixture.code, state);
		if (immutable == nullptr) return false;
		ShaderCode copy = *immutable;
		copy.GetInstructions().Clear();
		if (!check(fixture.code, state, input, true, true)) return false;

		ShaderVertexParseCache limited(false, initial_bytes * 2 - 1, checked_copy);
		if (limited.Get(fixture.code, state) == nullptr) return false;
		fixture.code[15] = 1;
		if (limited.Get(fixture.code, state) == nullptr || limited.Size() != 1 || limited.Bytes() > initial_bytes * 2 - 1) return false;
		ShaderVertexParseCache too_small(false, initial_bytes - 1, checked_copy);
		if (too_small.Get(fixture.code, state, &hit, &refused) != nullptr || hit || refused || too_small.Bytes() != 0) return false;
		ShaderVertexParseCache count_limited(false, ShaderVertexParseCache::BYTE_CAP, checked_copy);
		for (size_t index = 0; index <= ShaderVertexParseCache::CAPACITY; ++index)
		{
			fixture.code[15] = static_cast<uint32_t>(index);
			if (count_limited.Get(fixture.code, state, &hit) == nullptr || hit ||
			    count_limited.Size() > ShaderVertexParseCache::CAPACITY || count_limited.Bytes() > ShaderVertexParseCache::BYTE_CAP) return false;
		}
		fixture.code[15] = 0;
		if (count_limited.Get(fixture.code, state, &hit) == nullptr || hit ||
		    count_limited.Get(fixture.code, state, &hit) == nullptr || !hit) return false;
		if (!ShaderResetForNextSession() || count_limited.Get(fixture.code, state, &hit) == nullptr || hit || count_limited.Size() != 1)
			return false;
		fixture.code[1] = UINT32_MAX;
		if (cache.Get(fixture.code, state, &hit, &refused) != nullptr || hit || refused) return false;
		fixture.code[1] = 0;
		if (cache.Get(fixture.code, state, &hit, &refused) != nullptr || hit || refused) return false;
		fixture = original;
		std::array<uint32_t, 32> resized_source {};
		std::memcpy(resized_source.data(), fixture.code, sizeof(fixture.code));
		ShaderVertexParseCache resized(false, ShaderVertexParseCache::BYTE_CAP, checked_copy);
		if (resized.Get(resized_source.data(), state, &hit) == nullptr || hit) return false;
		resized_source[1] = 15; // Same address, grow the declared prefix from 16 to 32 DWORDs.
		if (resized.Get(resized_source.data(), state, &hit, &refused) == nullptr || hit || refused) return false;
		resized_source[1] = 7;
		if (resized.Get(resized_source.data(), state, &hit, &refused) == nullptr || !hit || refused) return false;
		const auto resized_attempts = resized.SourceReads();
		if (resized.Get(resized_source.data(), state, &hit) == nullptr || !hit || resized.SourceReads() != resized_attempts + 1)
			return false;

		const uint64_t allocation = Core::VirtualMemory::Alloc(0, 65536, Core::VirtualMemory::Mode::ReadWrite);
		if (allocation == 0) return false;
		auto* sparse = reinterpret_cast<uint32_t*>(allocation);
		std::memcpy(sparse, fixture.code, sizeof(fixture.code));
		sparse[1] = 8191;
		ShaderVertexParseCache stale_extent(false, ShaderVertexParseCache::BYTE_CAP, checked_copy);
		const bool initial_extent = stale_extent.Get(sparse, state, &hit, &refused) != nullptr && !hit && !refused;
		const bool protected_tail = Core::VirtualMemory::Protect(allocation + 32768, 16384, Core::VirtualMemory::Mode::NoAccess);
		// A refused stale extent followed by an ineligible prefix must not reach
		// the unbounded fallback, even when those first bytes remain readable.
		sparse[0] = 0xbf810000u;
		const bool ineligible_refused = protected_tail && stale_extent.Get(sparse, state, &hit, &refused) == nullptr && !hit && refused;
		sparse[0] = fixture.code[0];
		sparse[1] = 7;
		const auto stale_attempts = stale_extent.SourceReads();
		const bool shorter_checked = protected_tail && stale_extent.Get(sparse, state, &hit, &refused) != nullptr && !hit && !refused &&
		    stale_extent.SourceReads() == stale_attempts + 3;
		sparse[1] = 8191;
		const bool tail_refused = protected_tail && stale_extent.Get(sparse, state, &hit, &refused) == nullptr && !hit && refused;
		const bool prefix_refused = protected_tail && cache.Get(sparse + 8192, state, &hit, &refused) == nullptr && !hit && refused;
		const bool freed = Core::VirtualMemory::Free(allocation);
		if (!initial_extent || !ineligible_refused || !shorter_checked || !tail_refused || !prefix_refused || !freed) return false;
	}
#endif
	return true;
}

bool ShaderVertexResourcePreCacheFp32ContractTest()
{
	constexpr uint32_t program[] = {0xbf8c0000u, 0x0e040300u, 0xbf810000u};
	constexpr uint32_t vmac_program[] = {0xbf8c0000u, 0x0c04170bu, 0xbf810000u};
	constexpr uint32_t rsq_clamp_program[] = {0xbf8c0000u, 0x7e045902u, 0xbf810000u};
	HW::VsShaderResource1 state {};
	state.float_mode = 0xc0;
	if (!ShaderVertexFp32RegisterStateSupported(state)) return false;
	const auto code = ShaderParseVertexResourceProfile(program, state);
	if (code.GetFp32Mode() != ShaderFp32Mode::RneFlushInOut || code.GetInstructions().Size() != 3 ||
	    code.GetInstructions().At(1).type != ShaderInstructionType::VMulLegacyF32)
	{
		return false;
	}
	if (ShaderVMacLegacyF32FormSupported(code, 267u, 267u, 258u, false, false, 0, 0, 0, false))
	{
		const auto vmac = ShaderParseVertexResourceProfile(vmac_program, state);
		if (vmac.GetDx10ClampMode() != ShaderDx10ClampMode::Disabled || vmac.GetInstructions().Size() != 3 ||
		    vmac.GetInstructions().At(1).type != ShaderInstructionType::VMacLegacyF32)
		{
			return false;
		}
	}
	if (ShaderVRsqClampF32FormSupported(code, 258u, 258u))
	{
		const auto rsq_clamp = ShaderParseVertexResourceProfile(rsq_clamp_program, state);
		if (rsq_clamp.GetFp32Mode() != ShaderFp32Mode::RneFlushInOut ||
		    rsq_clamp.GetDx10ClampMode() != ShaderDx10ClampMode::Disabled || rsq_clamp.GetInstructions().Size() != 3 ||
		    rsq_clamp.GetInstructions().At(1).type != ShaderInstructionType::VRsqClampF32)
		{
			return false;
		}
	}
	state.float_mode = 0;
	if (ShaderVertexFp32RegisterStateSupported(state)) return false;
	state.float_mode = 0xc0;
	state.ieee_mode = true;
	if (ShaderVertexFp32RegisterStateSupported(state)) return false;

	ShaderCode fetch;
	fetch.SetType(ShaderType::Fetch);
	fetch.SetFp32Mode(ShaderFp32Mode::RneFlushInOut);
	return !ShaderVMulLegacyF32FormSupported(fetch, false, false, 0, 0, 0, false, 0);
}

bool ShaderVertexUserSgprDeclarationContractTest()
{
	if (!ShaderVertexUserSgprStateSupported(0, 0) || !ShaderVertexUserSgprStateSupported(2, 0) ||
	    !ShaderVertexUserSgprStateSupported(HW::UserSgprInfo::SGPRS_MAX, HW::UserSgprInfo::SGPRS_MAX) ||
	    ShaderVertexUserSgprStateSupported(-1, 0) ||
	    ShaderVertexUserSgprStateSupported(HW::UserSgprInfo::SGPRS_MAX + 1, 0) ||
	    ShaderVertexUserSgprStateSupported(2, HW::UserSgprInfo::SGPRS_MAX + 1))
	{
		return false;
	}

	const ShaderUsageSlot storage_at_zero {0x00, 0, 0, 0};
	const ShaderUsageSlot storage_at_four {0x00, 0, 4, 0};
	const ShaderUsageSlot fetch_at_zero {0x12, 0, 0, 0};
	if (!ShaderVertexUsageTupleFits(storage_at_zero, 8, 4) || ShaderVertexUsageTupleFits(storage_at_four, 8, 4) ||
	    ShaderVertexUsageTupleFits(storage_at_zero, 4, 0) || ShaderVertexUsageTupleFits(fetch_at_zero, 2, 0) ||
	    !ShaderVertexUsageTupleFits(fetch_at_zero, 2, 2))
	{
		return false;
	}

	struct Fixture
	{
		uint32_t         code[4] = {};
		ShaderBinaryInfo info {};
	};
	static_assert(offsetof(Fixture, info) == 4 * sizeof(uint32_t));

	Fixture fixture;
	fixture.code[0]                         = 0xbeeb03ffu;
	fixture.code[1]                         = 1;
	fixture.code[2]                         = 0xbf810000u;
	fixture.info.chunk_usage_base_offset_dw = 0;
	fixture.info.num_input_usage_slots      = 0;

	HW::VertexShaderInfo regs {};
	regs.vs_regs.data_addr       = reinterpret_cast<uint64_t>(fixture.code);
	regs.vs_regs.rsrc2.user_sgpr = 2;
	// count is a write high-water mark, not the declared window. The reset bank
	// is intentionally zero/Unknown and remains valid for direct SGPRs.
	if (regs.vs_user_sgpr.count != 0 || regs.vs_user_sgpr.value[0] != 0 || regs.vs_user_sgpr.value[1] != 0 ||
	    regs.vs_user_sgpr.type[0] != HW::UserSgprType::Unknown || regs.vs_user_sgpr.type[1] != HW::UserSgprType::Unknown)
	{
		return false;
	}

	HW::ShaderRegisters   shader_regs {};
	ShaderVertexInputInfo input {};
	ShaderGetInputInfoVS(&regs, &shader_regs, &input);
	return input.bind.storage_buffers.buffers_num == 0 && input.bind.textures2D.textures_num == 0 &&
	       input.bind.samplers.samplers_num == 0 && input.bind.gds_pointers.pointers_num == 0 &&
	       input.bind.direct_sgprs.sgprs_num == 2 && input.bind.direct_sgprs.start_register[0] == 0 &&
	       input.bind.direct_sgprs.start_register[1] == 1 && input.bind.direct_sgprs.sgprs[0].field == 0 &&
	       input.bind.direct_sgprs.sgprs[1].field == 0 && input.bind.push_constant_offset == 0 &&
	       input.bind.push_constant_size == 16 && input.bind.descriptor_set_slot == 0;
}

bool ShaderVertexReadOnlyStorageContractTest()
{
	if (!ShaderVertexUsageTupleFits({0x00, 0, 8, 3}, 16, 16) || ShaderVertexUsageTupleFits({0x00, 0, 8, 3}, 14, 16) ||
	    ShaderVertexUsageTupleFits({0x1b, 1, 15, 0}, 16, 16) || ShaderVertexUsageTupleFits({0x01, 0, 12, 0}, 16, 14)) return false;
	struct Fixture
	{
		uint32_t         code[16] = {};
		ShaderBinaryInfo info {};
	};
	static_assert(offsetof(Fixture, info) == 16 * sizeof(uint32_t));

	Fixture fixture;
	fixture.code[0]                         = 0xbeeb03ffu;
	fixture.code[1]                         = 7;
	fixture.info.chunk_usage_base_offset_dw = 0;
	fixture.info.num_input_usage_slots      = 2;
	const ShaderUsageSlot slots[]           = {{0x00, 0, 0, 0}, {0x00, 1, 4, 0}};
	std::memcpy(fixture.code + 14, slots, sizeof(slots));

	HW::VertexShaderInfo regs {};
	regs.vs_regs.data_addr       = reinterpret_cast<uint64_t>(fixture.code);
	regs.vs_regs.rsrc2.user_sgpr = 8;
	regs.vs_user_sgpr.count      = 8;
	for (uint32_t i = 0; i < 8; ++i)
	{
		regs.vs_user_sgpr.value[i] = 0x10101010u * (i + 1u);
		regs.vs_user_sgpr.type[i]  = HW::UserSgprType::Region;
	}

	HW::ShaderRegisters   shader_regs {};
	ShaderVertexInputInfo input {};
	ShaderGetInputInfoVS(&regs, &shader_regs, &input);
	if (input.bind.storage_buffers.buffers_num != 2 || input.bind.storage_buffers.binding_index != 0 ||
	    input.bind.storage_buffers.usages[0] != ShaderStorageUsage::ReadOnly ||
	    input.bind.storage_buffers.usages[1] != ShaderStorageUsage::ReadOnly || input.bind.storage_buffers.slots[0] != 0 ||
	    input.bind.storage_buffers.slots[1] != 1 || input.bind.storage_buffers.start_register[0] != 0 ||
	    input.bind.storage_buffers.start_register[1] != 4 || input.bind.storage_buffers.extended[0] ||
	    input.bind.storage_buffers.extended[1] || input.bind.push_constant_size != 32 || input.bind.descriptor_set_slot != 0 ||
	    input.bind.direct_sgprs.sgprs_num != 0)
	{
		return false;
	}

	ShaderCode read_only_code;
	if (ShaderCodeHasStorageBufferWrites(read_only_code) || ShaderVertexReadOnlyStorageHasWriteConflict(read_only_code, input.bind))
	{
		return false;
	}
	constexpr ShaderInstructionType store_types[] = {
	    ShaderInstructionType::BufferStoreDword,
	    ShaderInstructionType::BufferStoreFormatX,
	    ShaderInstructionType::BufferStoreFormatXy,
	};
	for (const auto store_type: store_types)
	{
		ShaderCode        store_code;
		ShaderInstruction store {};
		store.type = store_type;
		store_code.GetInstructions().Add(store);
		if (!ShaderCodeHasStorageBufferWrites(store_code) || !ShaderVertexReadOnlyStorageHasWriteConflict(store_code, input.bind))
		{
			return false;
		}
	}

	ShaderInstruction store {};
	store.type = ShaderInstructionType::BufferStoreDword;
	read_only_code.GetInstructions().Add(store);
	input.bind.storage_buffers.usages[0] = ShaderStorageUsage::Constant;
	if (!ShaderVertexReadOnlyStorageHasWriteConflict(read_only_code, input.bind))
	{
		return false;
	}
	input.bind.storage_buffers.usages[1] = ShaderStorageUsage::Constant;
	if (ShaderVertexReadOnlyStorageHasWriteConflict(read_only_code, input.bind))
	{
		return false;
	}
	return true;
}
#endif

void ShaderGetInputInfoPS(const HW::PixelShaderInfo* regs, const HW::ShaderRegisters* sh, const ShaderVertexInputInfo* vs_info,
                          ShaderPixelInputInfo* ps_info)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(vs_info == nullptr);
	EXIT_IF(ps_info == nullptr);
	EXIT_IF(regs == nullptr);
	EXIT_IF(sh == nullptr);

	if (regs->ps_embedded)
	{
		return;
	}

	ps_info->input_num            = sh->ps_in_control;
	// Validate before copying the fixed 32-entry interpolator register array.
	EXIT_NOT_IMPLEMENTED(ps_info->input_num > 32);
	ps_info->ps_pos_xy            = (sh->ps_input_ena == 0x00000302 && sh->ps_input_addr == 0x00000302);
	ps_info->ps_pixel_kill_enable = sh->db_shader_control.shader_kill_enable;
	ps_info->ps_early_z           = (sh->db_shader_control.shader_z_behavior == 1);
	ps_info->ps_execute_on_noop   = sh->db_shader_control.shader_execute_on_noop;

	for (uint32_t i = 0; i < ps_info->input_num; i++)
	{
		ps_info->interpolator_settings[i] = sh->ps_interpolator_settings[i];
	}

	ps_info->bind.descriptor_set_slot  = (vs_info->bind.storage_buffers.buffers_num > 0 || vs_info->bind.textures2D.textures_num > 0 ||
	                                      vs_info->bind.samplers.samplers_num > 0 || vs_info->bind.gds_pointers.pointers_num > 0 ? 1 : 0);
	ps_info->bind.push_constant_offset = vs_info->bind.push_constant_offset + vs_info->bind.push_constant_size;
	ps_info->bind.push_constant_size   = 0;

	for (int i = 0; i < 8; i++)
	{
		ps_info->target_output_mode[i] = sh->target_output_mode[i];
	}
	ps_info->shader_mask = sh->m_cbShaderMask;

	ShaderParsedUsage usage;

	ShaderParseUsage("ps", regs->ps_regs.data_addr, &usage, &ps_info->bind, regs->ps_user_sgpr, regs->ps_regs.rsrc2.user_sgpr);

	EXIT_NOT_IMPLEMENTED(usage.fetch || usage.vertex_buffer);
	EXIT_NOT_IMPLEMENTED(usage.storage_buffers_readwrite > 0);
	EXIT_NOT_IMPLEMENTED(usage.gds_pointers > 0);

	ShaderCalcBindingIndices(&ps_info->bind);
}

struct ShaderOwnedDrawInputs::Impl
{
	using Backing = Core::VirtualMemory::GuestBackingSpan;
	struct Range
	{
		uint64_t address = 0, bytes = 0;
		std::vector<uint32_t> words;
		ShaderSourceWords shared;
		std::vector<Backing> backing;
		std::unique_ptr<Core::VirtualMemory::GuestDataRangeSeal> seal;
		const uint32_t* Data() const { return shared ? shared.Data() : words.data(); }
	};
	HW::VertexShaderInfo vertex {};
	HW::PixelShaderInfo pixel {};
	HW::ShaderRegisters registers {};
	ShaderVertexSourceSnapshot vertex_source;
	std::vector<Range> ranges;
	ShaderCode fetch_code;
	uint64_t fetch_address = 0;
	uint64_t shader_generation = 0;
	bool ready = false;

	static bool BackingFor(uint64_t address, uint64_t bytes, std::vector<Backing>* spans)
	{
		if (address == 0 || bytes == 0 || address > UINT64_MAX - bytes || spans == nullptr) return false;
		spans->clear();
		for (uint64_t cursor = address; cursor < address + bytes;)
		{
			Backing span;
			if (spans->size() >= 256 || !Core::VirtualMemory::QueryGuestBackingSpan(cursor, address + bytes - cursor, &span) ||
			    span.identity == 0 || span.address != cursor || span.size == 0 || span.size > address + bytes - cursor) return false;
			spans->push_back(span);
			cursor += span.size;
		}
		return true;
	}
	static bool SameBacking(const std::vector<Backing>& first, const std::vector<Backing>& second)
	{
		if (first.size() != second.size()) return false;
		for (size_t i = 0; i < first.size(); ++i)
			if (first[i].identity != second[i].identity || first[i].offset != second[i].offset ||
			    first[i].address != second[i].address || first[i].size != second[i].size ||
			    first[i].generation != second[i].generation) return false;
		return true;
	}
	bool CaptureRange(uint64_t address, uint64_t bytes, Range* result, const ShaderSourceWords* shared = nullptr)
	{
		using namespace Core::VirtualMemory;
		if (result == nullptr || bytes == 0 || bytes > 2u * 1024u * 1024u || bytes % sizeof(uint32_t) != 0 ||
		    address == 0 || address > UINT64_MAX - bytes) return false;
		if (!ShaderPrepareMemoryRead(address, bytes)) return false;
		GuestDataLease lease;
		if (!TryAcquireGuestDataLease(address, bytes, Mode::Read, &lease, false)) return false;
		Range candidate;
		candidate.address = address;
		candidate.bytes = bytes;
		candidate.seal = std::make_unique<GuestDataRangeSeal>();
		if (!CaptureGuestDataRangeSeal(lease, candidate.seal.get()) ||
		    !BackingFor(address, bytes, &candidate.backing)) return false;
		if (shared != nullptr)
		{
			if (!*shared || shared->Bytes() != bytes ||
			    std::memcmp(shared->Data(), reinterpret_cast<const void*>(lease.HostBegin()), bytes) != 0) return false;
			candidate.shared = *shared;
		} else
		{
			candidate.words.resize(static_cast<size_t>(bytes / sizeof(uint32_t)));
			std::memcpy(candidate.words.data(), reinterpret_cast<const void*>(lease.HostBegin()), bytes);
		}
		*result = std::move(candidate);
		return true;
	}
	bool AddRange(uint64_t address, uint64_t bytes, const ShaderSourceWords* shared = nullptr)
	{
		for (const auto& range: ranges)
			if (address >= range.address && address - range.address <= range.bytes &&
			    bytes <= range.bytes - (address - range.address)) return true;
		uint64_t total = bytes;
		for (const auto& range: ranges) total += range.bytes;
		if (ranges.size() >= 128 || total > 4u * 1024u * 1024u) return false;
		Range captured;
		if (!CaptureRange(address, bytes, &captured, shared)) return false;
		ranges.emplace_back(std::move(captured));
		return true;
	}
	OwnedShaderReadContext Reads() const
	{
		OwnedShaderReadContext result;
		result.ranges.reserve(ranges.size());
		for (const auto& range: ranges) result.ranges.push_back({range.address, range.Data(), range.bytes});
		result.fetch = fetch_address == 0 ? nullptr : &fetch_code;
		result.fetch_address = fetch_address;
		return result;
	}
	bool CaptureFetch(uint64_t address, uint64_t table)
	{
		if (address == 0 || table == 0 || (address & 3u) != 0 || (table & 3u) != 0 || fetch_address != 0) return false;
		// FETCH has no binary header. Bounded decoding distinguishes its terminal
		// from identical literal words. A range not wholly readable falls back
		// before any pause; the ordinary parser remains the conservative path.
		for (uint64_t bytes = 256; bytes <= 65536; bytes *= 2)
		{
			Range captured;
			if (!CaptureRange(address, bytes, &captured)) return false;
			ShaderCode code;
			code.SetType(ShaderType::Fetch);
			code.SetThreadTraceMode(ShaderThreadTraceMode::Disabled);
			OwnedShaderReadContext reads;
			reads.ranges.push_back({address, captured.Data(), bytes});
			uint32_t consumed = 0;
			{
				ScopedOwnedShaderReads scope(reads);
				consumed = ShaderTryParseBounded(captured.Data(), static_cast<uint32_t>(bytes / sizeof(uint32_t)), &code);
			}
			if (reads.refused) return false;
			if (consumed == 0) continue;
			if (consumed > bytes / sizeof(uint32_t) || captured.Data()[consumed - 1] != 0xbe802000u) return false;
			fetch_address = address;
			fetch_code = std::move(code);
			ranges.emplace_back(std::move(captured));
			for (const auto& instruction: fetch_code.GetInstructions())
				if (instruction.type == ShaderInstructionType::SLoadDwordx4)
				{
					if (instruction.src[1].type != ShaderOperandType::LiteralConstant ||
					    (instruction.src[1].constant.u & 3u) != 0 ||
					    table > UINT64_MAX - instruction.src[1].constant.u ||
					    !AddRange(table + instruction.src[1].constant.u, 4 * sizeof(uint32_t))) return false;
				}
			return true;
		}
		return false;
	}
	bool CaptureDependencies(uint64_t source, const HW::UserSgprInfo& sgprs, uint32_t declared, bool is_vertex)
	{
		if (!ShaderVertexUserSgprStateSupported(declared, sgprs.count)) return false;
		auto reads = Reads();
		const auto* owned = reads.Resolve(source, 2 * sizeof(uint32_t));
		if (owned == nullptr) return false;
		ShaderUsageInfo usages;
		{
			ScopedOwnedShaderReads scope(reads);
			usages = GetUsageSlots(owned);
		}
		if (!usages.valid || reads.refused) return false;
		// Copy metadata before growing ranges can move a captured vector owner.
		std::vector<ShaderUsageSlot> slots;
		if (usages.slots_num != 0) slots.assign(usages.slots, usages.slots + usages.slots_num);
		uint64_t extended = 0, fetch = 0, table = 0;
		const auto pointer = [&](const ShaderUsageSlot& slot, uint64_t* output) {
			if (slot.start_register + 1 >= HW::UserSgprInfo::SGPRS_MAX ||
			    slot.start_register + 1 >= declared || slot.start_register + 1 >= sgprs.count || *output != 0) return false;
			*output = uint64_t(sgprs.value[slot.start_register]) | (uint64_t(sgprs.value[slot.start_register + 1]) << 32u);
			return *output != 0 && (*output & 3u) == 0;
		};
		for (const auto& slot: slots)
		{
			if (is_vertex && !ShaderVertexUsageTupleFits(slot, declared, sgprs.count)) return false;
			if (slot.type == 0x1b && (slot.flags != 0 || slot.slot != 1 || !pointer(slot, &extended))) return false;
			if (slot.type == 0x12 && (!is_vertex || slot.flags != 0 || slot.slot != 0 || !pointer(slot, &fetch))) return false;
			if (slot.type == 0x17 && (!is_vertex || slot.flags != 0 || slot.slot != 0 || !pointer(slot, &table))) return false;
		}
		for (const auto& slot: slots)
		{
			uint32_t words = 0;
			switch (slot.type)
			{
				case 0x00: case 0x04:
					if (slot.flags != 0 && slot.flags != 3) return false;
					words = slot.flags == 3 ? 8 : 4; break;
				case 0x01: case 0x02: if (slot.flags != 0) return false; words = 4; break;
				case 0x07: if (slot.flags != 0) return false; words = 1; break;
				case 0x12: case 0x17: case 0x1b: continue;
				default: return false;
			}
			bool indirect = false;
			if (!ShaderResolveResourceAddressing(slot.start_register, words, reinterpret_cast<const uint32_t*>(extended), &indirect)) return false;
			if (indirect)
			{
				const uint64_t offset = uint64_t(slot.start_register - HW::UserSgprInfo::SGPRS_MAX) * sizeof(uint32_t);
				if (extended > UINT64_MAX - offset || !AddRange(extended + offset, words * sizeof(uint32_t))) return false;
			}
		}
		if ((fetch == 0) != (table == 0)) return false;
		return fetch == 0 || CaptureFetch(fetch, table);
	}
};

ShaderOwnedDrawInputs::ShaderOwnedDrawInputs() = default;
ShaderOwnedDrawInputs::~ShaderOwnedDrawInputs() = default;

bool ShaderOwnedDrawInputs::Capture(const HW::VertexShaderInfo* vertex, const HW::PixelShaderInfo* pixel,
    const HW::ShaderRegisters* registers, const ShaderVertexSourceSnapshot* retained_vertex)
{
	m_impl.reset();
	if (vertex == nullptr || pixel == nullptr || registers == nullptr || g_owned_shader_reads != nullptr) return false;
	auto candidate = std::make_unique<Impl>();
	candidate->vertex = *vertex;
	candidate->pixel = *pixel;
	candidate->registers = *registers;
	candidate->shader_generation = g_vertex_parse_cache_generation.load(std::memory_order_acquire);
	if (!vertex->vs_embedded)
	{
		if (retained_vertex != nullptr)
		{
			if (retained_vertex->Address() != vertex->vs_regs.data_addr || retained_vertex->Data() == nullptr) return false;
			auto& owned = candidate->vertex_source;
			owned.m_address = retained_vertex->m_address;
			owned.m_code_words = retained_vertex->m_code_words;
			owned.m_bytes = retained_vertex->m_bytes;
			owned.m_words = retained_vertex->m_words;
		} else if (!candidate->vertex_source.Capture(vertex->vs_regs.data_addr, nullptr)) return false;
		if (!candidate->AddRange(candidate->vertex_source.Address(), candidate->vertex_source.Bytes(),
		        &candidate->vertex_source.OwnedWords())) return false;
	}
	if (!pixel->ps_embedded)
	{
		ShaderVertexSourceSnapshot source;
		if (!source.Capture(pixel->ps_regs.data_addr, nullptr) ||
		    !candidate->AddRange(source.Address(), source.Bytes(), &source.OwnedWords())) return false;
	}
	if ((!vertex->vs_embedded && !candidate->CaptureDependencies(vertex->vs_regs.data_addr, vertex->vs_user_sgpr,
	        vertex->vs_regs.rsrc2.user_sgpr, true)) ||
	    (!pixel->ps_embedded && !candidate->CaptureDependencies(pixel->ps_regs.data_addr, pixel->ps_user_sgpr,
	        pixel->ps_regs.rsrc2.user_sgpr, false))) return false;
	if (candidate->shader_generation != g_vertex_parse_cache_generation.load(std::memory_order_acquire)) return false;
	candidate->ready = true;
	m_impl = std::move(candidate);
	return true;
}

bool ShaderOwnedDrawInputs::Analyze(ShaderVertexInputInfo* vertex, ShaderPixelInputInfo* pixel) const
{
	if (m_impl == nullptr || !m_impl->ready || vertex == nullptr || pixel == nullptr || g_owned_shader_reads != nullptr ||
	    m_impl->shader_generation != g_vertex_parse_cache_generation.load(std::memory_order_acquire)) return false;
	auto reads = m_impl->Reads();
	ScopedOwnedShaderReads owned(reads);
	ScopedShaderVertexSource source(VertexSource());
	ShaderVertexInputInfo decoded_vertex {};
	ShaderPixelInputInfo decoded_pixel {};
	ShaderGetInputInfoVS(&m_impl->vertex, &m_impl->registers, &decoded_vertex);
	ShaderGetInputInfoPS(&m_impl->pixel, &m_impl->registers, &decoded_vertex, &decoded_pixel);
	if (reads.refused || m_impl->shader_generation != g_vertex_parse_cache_generation.load(std::memory_order_acquire)) return false;
	*vertex = decoded_vertex;
	*pixel = decoded_pixel;
	return true;
}

bool ShaderOwnedDrawInputs::Validate() const
{
	using namespace Core::VirtualMemory;
	if (m_impl == nullptr || !m_impl->ready || g_owned_shader_reads != nullptr ||
	    m_impl->shader_generation != g_vertex_parse_cache_generation.load(std::memory_order_acquire)) return false;
	for (const auto& range: m_impl->ranges)
	{
		if (!ShaderPrepareMemoryRead(range.address, range.bytes)) return false;
		GuestDataLease lease;
		std::vector<Impl::Backing> backing;
		if (!TryAcquireGuestDataLease(range.address, range.bytes, Mode::Read, &lease, false) ||
		    range.seal == nullptr || !ValidateGuestDataRangeSeal(lease, *range.seal) ||
		    !Impl::BackingFor(range.address, range.bytes, &backing) || !Impl::SameBacking(range.backing, backing) ||
		    std::memcmp(range.Data(), reinterpret_cast<const void*>(lease.HostBegin()), range.bytes) != 0) return false;
	}
	return m_impl->shader_generation == g_vertex_parse_cache_generation.load(std::memory_order_acquire);
}

const ShaderVertexSourceSnapshot* ShaderOwnedDrawInputs::VertexSource() const
{
	return m_impl != nullptr && m_impl->ready && m_impl->vertex_source.Data() != nullptr ? &m_impl->vertex_source : nullptr;
}

bool ShaderOwnedDrawInputs::VisitRanges(bool (*visitor)(void*, uint64_t, uint64_t), void* context) const
{
	if (m_impl == nullptr || !m_impl->ready || visitor == nullptr) return false;
	for (const auto& range: m_impl->ranges)
		if (!visitor(context, range.address, range.bytes)) return false;
	return true;
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool ShaderOwnedDrawInputsContractTest()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	using namespace Core::VirtualMemory;
	Init();
	constexpr uint64_t bytes = 65536;
	struct File { FILE* value = std::tmpfile(); ~File() { if (value != nullptr) std::fclose(value); } } file;
	if (file.value == nullptr || ftruncate(fileno(file.value), bytes) != 0) return false;
	struct Mapping { uint64_t address; ~Mapping() { if (address != 0) Free(address); } };
	Mapping mapping {Alloc(0, bytes, Mode::ReadWrite)}, alias {Alloc(0, bytes, Mode::ReadWrite)};
	if (mapping.address == 0 || alias.address == 0 ||
	    !BindFreshGuestMappingToSharedBacking(mapping.address, bytes, fileno(file.value), 0, Mode::ReadWrite) ||
	    !BindFreshGuestMappingToSharedBacking(alias.address, bytes, fileno(file.value), 0, Mode::ReadWrite)) return false;
	struct Fixture { uint32_t code[16] = {0xbeeb03ffu, 7u, 0xbe860300u, 0xbf810000u}; ShaderBinaryInfo header {}; } vs, ps;
	const ShaderUsageSlot slots[] {{0x1b, 1, 0, 0}, {0x02, 0, 16, 0}};
	ps.header.num_input_usage_slots = 2;
	std::memcpy(ps.code + 14, slots, sizeof(slots));
	const uint32_t descriptors[] {0x10111213u, 0x20212223u, 0x30313233u, 0x40414243u};
	std::memcpy(reinterpret_cast<void*>(mapping.address), &vs, sizeof(vs));
	std::memcpy(reinterpret_cast<void*>(mapping.address + 4096), &ps, sizeof(ps));
	std::memcpy(reinterpret_cast<void*>(mapping.address + 8192), descriptors, sizeof(descriptors));
	HW::VertexShaderInfo vertex {};
	HW::PixelShaderInfo pixel {};
	HW::ShaderRegisters registers {};
	vertex.vs_regs.data_addr = mapping.address;
	vertex.vs_regs.rsrc1.float_mode = 0xc0;
	vertex.vs_regs.rsrc1.dx10_clamp = true;
	vertex.vs_regs.rsrc2.user_sgpr = 2;
	pixel.ps_regs.data_addr = mapping.address + 4096;
	pixel.ps_regs.rsrc2.user_sgpr = 2;
	pixel.ps_user_sgpr.count = 2;
	pixel.ps_user_sgpr.value[0] = static_cast<uint32_t>(mapping.address + 8192);
	pixel.ps_user_sgpr.value[1] = static_cast<uint32_t>((mapping.address + 8192) >> 32u);
	ShaderVertexInputInfo synchronous_vertex {};
	ShaderPixelInputInfo synchronous_pixel {};
	ShaderGetInputInfoVS(&vertex, &registers, &synchronous_vertex);
	ShaderGetInputInfoPS(&pixel, &registers, &synchronous_vertex, &synchronous_pixel);
	ShaderOwnedDrawInputs captured;
	ShaderVertexSourceSnapshot retained;
	if (!retained.Capture(vertex.vs_regs.data_addr, nullptr) ||
	    !captured.Capture(&vertex, &pixel, &registers, &retained) || !captured.Validate() ||
	    captured.VertexSource() == nullptr || !captured.VertexSource()->OwnedWords().SameStorage(retained.OwnedWords()) ||
	    CurrentThreadHasMemoryLease()) return false;
	const auto equal = [&](const ShaderVertexInputInfo& a, const ShaderPixelInputInfo& b) {
		return a.export_count == synchronous_vertex.export_count &&
		    a.bind.direct_sgprs.sgprs_num == synchronous_vertex.bind.direct_sgprs.sgprs_num &&
		    a.bind.push_constant_size == synchronous_vertex.bind.push_constant_size &&
		    b.bind.extended.used == synchronous_pixel.bind.extended.used &&
		    b.bind.extended.data.Base() == synchronous_pixel.bind.extended.data.Base() &&
		    b.bind.storage_buffers.buffers_num == 1 && b.bind.storage_buffers.extended[0] &&
		    b.bind.storage_buffers.usages[0] == ShaderStorageUsage::Constant &&
		    b.bind.storage_buffers.start_register[0] == 16 &&
		    b.bind.push_constant_size == synchronous_pixel.bind.push_constant_size &&
		    b.bind.push_constant_offset == synchronous_pixel.bind.push_constant_offset &&
		    std::memcmp(b.bind.storage_buffers.buffers[0].fields,
		        synchronous_pixel.bind.storage_buffers.buffers[0].fields, sizeof(descriptors)) == 0;
	};
	// No live mapping is readable while Analyze runs, including its PS header
	// and indirect descriptors. A second worker exercises independent TLS state.
	if (!Protect(mapping.address, bytes, Mode::NoAccess)) return false;
	ShaderVertexInputInfo owned_vertex {}, peer_vertex {};
	ShaderPixelInputInfo owned_pixel {}, peer_pixel {};
	bool peer_ok = false;
	std::thread peer([&] { peer_ok = captured.Analyze(&peer_vertex, &peer_pixel) && equal(peer_vertex, peer_pixel); });
	const bool analyzed = captured.Analyze(&owned_vertex, &owned_pixel) && equal(owned_vertex, owned_pixel);
	peer.join();
	if (!analyzed || !peer_ok || captured.Validate() || CurrentThreadHasMemoryLease()) return false;
	if (!Protect(mapping.address, bytes, Mode::ReadWrite) || captured.Validate()) return false;
	if (!captured.Capture(&vertex, &pixel, &registers) || !captured.Validate()) return false;
	// Writes through a distinct accepted host alias still invalidate the bytes.
	auto* alias_descriptor = reinterpret_cast<uint32_t*>(alias.address + 8192);
	alias_descriptor[2] ^= 0x40u;
	if (captured.Validate()) return false;
	alias_descriptor[2] ^= 0x40u;
	if (!captured.Validate()) return false;
	// Replacing a mapping with identical bytes cannot revive its old seal.
	const uint64_t old_address = mapping.address;
	if (!Free(old_address)) return false;
	mapping.address = 0;
	if (!AllocFixed(old_address, bytes, Mode::ReadWrite)) return false;
	mapping.address = old_address;
	if (!BindFreshGuestMappingToSharedBacking(old_address, bytes, fileno(file.value), 0, Mode::ReadWrite) ||
	    captured.Validate()) return false;
	if (!captured.Capture(&vertex, &pixel, &registers) || !captured.Validate()) return false;
	// A missing indirect dependency is refused before the caller may pause.
	HW::PixelShaderInfo missing = pixel;
	missing.ps_user_sgpr.value[0] = 1;
	missing.ps_user_sgpr.value[1] = 0;
	ShaderOwnedDrawInputs refused;
	if (refused.Capture(&vertex, &missing, &registers) || refused.Analyze(&owned_vertex, &owned_pixel)) return false;
	{
		auto reads = captured.m_impl->Reads();
		ScopedOwnedShaderReads scope(reads);
		if (ShaderPrepareMemoryRead(mapping.address + 8192, sizeof(descriptors)) || !reads.refused) return false;
		reads.refused = false;
		const uint32_t tail[] {0xbe860300u, 0xbf810000u};
		reads.ranges.push_back({0, tail, sizeof(tail)});
		auto parsed = ShaderVertexResourceProfileCode(vertex.vs_regs.rsrc1);
		if (ShaderTryParseBounded(tail, 2, &parsed) != 2 || reads.refused || parsed.GetInstructions().Size() != 2) return false;
	}
	std::fprintf(stderr, "[GFX_OWNED_DRAW_INPUTS_TEST] synchronous-equal=pass no-live-read=pass concurrent-tls=pass"
	    " retained-vs=pass alias-mutation=denied rights-roundtrip=denied mapping-replacement=denied"
	    " missing-dependency=denied bounded-private-tail=pass\n");
	return true;
#else
	return false;
#endif
}

bool ShaderPixelDirectSgprContractTest()
{
	if (!ShaderDirectSgprTypeSupported(HW::UserSgprType::Unknown) ||
	    !ShaderDirectSgprTypeSupported(HW::UserSgprType::Region) ||
	    ShaderDirectSgprTypeSupported(HW::UserSgprType::Vsharp))
	{
		return false;
	}

	struct Fixture
	{
		uint32_t         code[16] = {};
		ShaderBinaryInfo info {};
	};
	static_assert(offsetof(Fixture, info) == 16 * sizeof(uint32_t));

	Fixture fixture;
	fixture.code[0]                         = 0xbeeb03ffu;
	fixture.code[1]                         = 7;
	fixture.info.chunk_usage_base_offset_dw = 0;
	fixture.info.num_input_usage_slots      = 1;
	const ShaderUsageSlot storage           = {0x00, 0, 4, 0};
	std::memcpy(fixture.code + 15, &storage, sizeof(storage));

	HW::PixelShaderInfo regs {};
	regs.ps_regs.data_addr       = reinterpret_cast<uint64_t>(fixture.code);
	regs.ps_regs.rsrc2.user_sgpr = 8;
	regs.ps_user_sgpr.count      = 8;
	for (uint32_t i = 0; i < 4; ++i)
	{
		regs.ps_user_sgpr.value[i] = 0x11111111u * (i + 1u);
		// A packet-wide Region marker may cover both this direct-scalar
		// prefix and the constant-buffer tuple below.
		regs.ps_user_sgpr.type[i]  = HW::UserSgprType::Region;
		regs.ps_user_sgpr.value[i + 4] = 0x01010101u * (i + 1u);
		regs.ps_user_sgpr.type[i + 4]  = HW::UserSgprType::Region;
	}

	ShaderVertexInputInfo vertex {};
	vertex.bind.storage_buffers.buffers_num = 2;
	vertex.bind.push_constant_size          = 32;
	HW::ShaderRegisters shader_regs {};
	ShaderPixelInputInfo input {};
	ShaderGetInputInfoPS(&regs, &shader_regs, &vertex, &input);
	if (input.bind.storage_buffers.buffers_num != 1 || input.bind.storage_buffers.binding_index != 0 ||
	    input.bind.storage_buffers.usages[0] != ShaderStorageUsage::ReadOnly || input.bind.storage_buffers.start_register[0] != 4 ||
	    input.bind.storage_buffers.extended[0] || input.bind.direct_sgprs.sgprs_num != 4 || input.bind.descriptor_set_slot != 1 ||
	    input.bind.push_constant_offset != 32 || input.bind.push_constant_size != 32)
	{
		return false;
	}
	for (int i = 0; i < 4; ++i)
	{
		if (input.bind.direct_sgprs.start_register[i] != i ||
		    input.bind.direct_sgprs.sgprs[i].field != 0x11111111u * static_cast<uint32_t>(i + 1))
		{
			return false;
		}
	}
	return true;
}
#endif

void ShaderGetInputInfoCS(const HW::ComputeShaderInfo* regs, const HW::ShaderRegisters* /*sh*/, ShaderComputeInputInfo* info)
{
	EXIT_IF(info == nullptr);
	EXIT_IF(regs == nullptr);

	info->threads_num[0] = regs->cs_regs.num_thread_x;
	info->threads_num[1] = regs->cs_regs.num_thread_y;
	info->threads_num[2] = regs->cs_regs.num_thread_z;
	info->group_id[0]    = regs->cs_regs.tgid_x_en != 0;
	info->group_id[1]    = regs->cs_regs.tgid_y_en != 0;
	info->group_id[2]    = regs->cs_regs.tgid_z_en != 0;
	info->thread_ids_num = regs->cs_regs.tidig_comp_cnt + 1;

	info->workgroup_register = regs->cs_regs.user_sgpr;

	info->bind.push_constant_offset = 0;
	info->bind.push_constant_size   = 0;
	info->bind.descriptor_set_slot  = 0;

	ShaderParsedUsage usage;

	ShaderParseUsage("cs", regs->cs_regs.data_addr, &usage, &info->bind, regs->cs_user_sgpr, regs->cs_regs.user_sgpr);

	EXIT_NOT_IMPLEMENTED(usage.samplers > 0);
	EXIT_NOT_IMPLEMENTED(usage.fetch || usage.vertex_buffer);

	ShaderCalcBindingIndices(&info->bind);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void ShaderDbgDumpResources(const ShaderBindResources& bind)
{
	printf("\t descriptor_set_slot            = %u\n", bind.descriptor_set_slot);
	printf("\t push_constant_offset           = %u\n", bind.push_constant_offset);
	printf("\t push_constant_size             = %u\n", bind.push_constant_size);
	printf("\t storage_buffers.buffers_num    = %d\n", bind.storage_buffers.buffers_num);
	printf("\t storage_buffers.binding_index  = %d\n", bind.storage_buffers.binding_index);
	printf("\t textures.textures_num          = %d\n", bind.textures2D.textures_num);
	printf("\t textures.binding_sampled_index = %d\n", bind.textures2D.binding_sampled_index);
	printf("\t textures.binding_storage_index = %d\n", bind.textures2D.binding_storage_index);
	printf("\t samplers.samplers_num          = %d\n", bind.samplers.samplers_num);
	printf("\t samplers.binding_index         = %d\n", bind.samplers.binding_index);
	printf("\t gds_pointers.pointers_num      = %d\n", bind.gds_pointers.pointers_num);
	printf("\t gds_pointers.binding_index     = %d\n", bind.gds_pointers.binding_index);
	printf("\t direct_sgprs.sgprs_num         = %d\n", bind.direct_sgprs.sgprs_num);
	printf("\t extended.used                  = %s\n", (bind.extended.used ? "true" : "false"));
	printf("\t extended.slot                  = %d\n", bind.extended.slot);
	printf("\t extended.start_register        = %d\n", bind.extended.start_register);
	printf("\t extended.data.Base             = %" PRIx64 "\n", bind.extended.data.Base());

	for (int i = 0; i < bind.storage_buffers.buffers_num; i++)
	{
		const auto& r = bind.storage_buffers.buffers[i];

		printf("\t StorageBuffer %d\n", i);

		printf("\t\t fields           = %08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "\n", r.fields[3], r.fields[2], r.fields[1],
		       r.fields[0]);
		printf("\t\t Base()           = %" PRIx64 "\n", r.Base44());
		printf("\t\t Stride()         = %" PRIu16 "\n", r.Stride());
		printf("\t\t SwizzleEnabled() = %s\n", r.SwizzleEnabled() ? "true" : "false");
		printf("\t\t NumRecords()     = %" PRIu32 "\n", r.NumRecords());
		printf("\t\t DstSelX()        = %" PRIu8 "\n", r.DstSelX());
		printf("\t\t DstSelY()        = %" PRIu8 "\n", r.DstSelY());
		printf("\t\t DstSelZ()        = %" PRIu8 "\n", r.DstSelZ());
		printf("\t\t DstSelW()        = %" PRIu8 "\n", r.DstSelW());
		printf("\t\t Nfmt()           = %" PRIu8 "\n", r.Nfmt());
		printf("\t\t Dfmt()           = %" PRIu8 "\n", r.Dfmt());
		printf("\t\t MemoryType()     = 0x%02" PRIx8 "\n", r.MemoryType());
		printf("\t\t AddTid()         = %s\n", r.AddTid() ? "true" : "false");
		printf("\t\t slot             = %d\n", bind.storage_buffers.slots[i]);
		printf("\t\t start_register   = %d\n", bind.storage_buffers.start_register[i]);
		printf("\t\t extended         = %s\n", (bind.storage_buffers.extended[i] ? "true" : "false"));
		printf("\t\t usage            = %s\n", Core::EnumName8(bind.storage_buffers.usages[i]).c_str());
	}

	for (int i = 0; i < bind.textures2D.textures_num; i++)
	{
		const auto& r = bind.textures2D.desc[i].texture;

		printf("\t Texture %d\n", i);

		printf("\t\t fields = %08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "\n",
		       r.fields[7], r.fields[6], r.fields[5], r.fields[4], r.fields[3], r.fields[2], r.fields[1], r.fields[0]);
		printf("\t\t Base()          = %016" PRIx64 "\n", r.Base38());
		printf("\t\t MinLod()        = %" PRIu16 "\n", r.MinLod());
		printf("\t\t Dfmt()          = %" PRIu8 "\n", r.Dfmt());
		printf("\t\t Nfmt()          = %" PRIu8 "\n", r.Nfmt());
		printf("\t\t PerfMod()       = %" PRIu8 "\n", r.PerfMod());
		printf("\t\t Interlaced()    = %s\n", r.Interlaced() ? "true" : "false");
		printf("\t\t MemoryType()    = 0x%02" PRIx8 "\n", r.MemoryType());
		printf("\t\t Pow2Pad()       = %s\n", r.Pow2Pad() ? "true" : "false");
		printf("\t\t Pitch()         = %" PRIu16 "\n", r.Pitch());
		printf("\t\t BaseArray()     = %" PRIu16 "\n", r.BaseArray());
		printf("\t\t LastArray()     = %" PRIu16 "\n", r.LastArray());
		printf("\t\t MinLodWarn()    = %" PRIu16 "\n", r.MinLodWarn());
		printf("\t\t LodHdwCntEn()   = %s\n", r.LodHdwCntEn() ? "true" : "false");
		printf("\t\t CounterBankId() = %" PRIu8 "\n", r.CounterBankId());
		printf("\t\t Width()         = %" PRIu16 "\n", r.Width4());
		printf("\t\t Height()        = %" PRIu16 "\n", r.Height4());
		printf("\t\t DstSelX()       = %" PRIu8 "\n", r.DstSelX());
		printf("\t\t DstSelY()       = %" PRIu8 "\n", r.DstSelY());
		printf("\t\t DstSelZ()       = %" PRIu8 "\n", r.DstSelZ());
		printf("\t\t DstSelW()       = %" PRIu8 "\n", r.DstSelW());
		printf("\t\t BaseLevel()     = %" PRIu8 "\n", r.BaseLevel());
		printf("\t\t LastLevel()     = %" PRIu8 "\n", r.LastLevel());
		printf("\t\t TileMode()      = %" PRIu8 "\n", r.TileMode());
		printf("\t\t Type()          = %" PRIu8 "\n", r.Type());
		printf("\t\t Depth()         = %" PRIu16 "\n", r.Depth());
		printf("\t\t slot            = %d\n", bind.textures2D.desc[i].slot);
		printf("\t\t start_register  = %d\n", bind.textures2D.desc[i].start_register);
		printf("\t\t extended        = %s\n", (bind.textures2D.desc[i].extended ? "true" : "false"));
		printf("\t\t usage           = %s\n", Core::EnumName8(bind.textures2D.desc[i].usage).c_str());
	}

	for (int i = 0; i < bind.samplers.samplers_num; i++)
	{
		const auto& r = bind.samplers.samplers[i];

		printf("\t Sampler %d\n", i);

		printf("\t\t fields = %08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "\n", r.fields[3], r.fields[2], r.fields[1], r.fields[0]);

		printf("\t\t ClampX()           = %" PRIu8 "\n", r.ClampX());
		printf("\t\t ClampY()           = %" PRIu8 "\n", r.ClampY());
		printf("\t\t ClampZ()           = %" PRIu8 "\n", r.ClampZ());
		printf("\t\t MaxAnisoRatio()    = %" PRIu8 "\n", r.MaxAnisoRatio());
		printf("\t\t DepthCompareFunc() = %" PRIu8 "\n", r.DepthCompareFunc());
		printf("\t\t ForceUnormCoords() = %s\n", r.ForceUnormCoords() ? "true" : "false");
		printf("\t\t AnisoThreshold()   = %" PRIu8 "\n", r.AnisoThreshold());
		printf("\t\t McCoordTrunc()     = %s\n", r.McCoordTrunc() ? "true" : "false");
		printf("\t\t ForceDegamma()     = %s\n", r.ForceDegamma() ? "true" : "false");
		printf("\t\t AnisoBias()        = %" PRIu8 "\n", r.AnisoBias());
		printf("\t\t TruncCoord()       = %s\n", r.TruncCoord() ? "true" : "false");
		printf("\t\t DisableCubeWrap()  = %s\n", r.DisableCubeWrap() ? "true" : "false");
		printf("\t\t FilterMode()       = %" PRIu8 "\n", r.FilterMode());
		printf("\t\t MinLod()           = %" PRIu16 "\n", r.MinLod());
		printf("\t\t MaxLod()           = %" PRIu16 "\n", r.MaxLod());
		printf("\t\t PerfMip()          = %" PRIu8 "\n", r.PerfMip());
		printf("\t\t PerfZ()            = %" PRIu8 "\n", r.PerfZ());
		printf("\t\t LodBias()          = %" PRIu16 "\n", r.LodBias());
		printf("\t\t LodBiasSec()       = %" PRIu8 "\n", r.LodBiasSec());
		printf("\t\t XyMagFilter()      = %" PRIu8 "\n", r.XyMagFilter());
		printf("\t\t XyMinFilter()      = %" PRIu8 "\n", r.XyMinFilter());
		printf("\t\t ZFilter()          = %" PRIu8 "\n", r.ZFilter());
		printf("\t\t MipFilter()        = %" PRIu8 "\n", r.MipFilter());
		printf("\t\t BorderColorPtr()   = %" PRIu16 "\n", r.BorderColorPtr());
		printf("\t\t BorderColorType()  = %" PRIu8 "\n", r.BorderColorType());
		printf("\t\t slot               = %d\n", bind.samplers.slots[i]);
		printf("\t\t start_register     = %d\n", bind.samplers.start_register[i]);
		printf("\t\t extended           = %s\n", (bind.samplers.extended[i] ? "true" : "false"));
	}

	for (int i = 0; i < bind.gds_pointers.pointers_num; i++)
	{
		const auto& r = bind.gds_pointers.pointers[i];

		printf("\t Gds Pointer %d\n", i);

		printf("\t\t field = %08" PRIx32 "\n", r.field);

		printf("\t\t Base()         = %" PRIu16 "\n", r.Base());
		printf("\t\t Size()         = %" PRIu16 "\n", r.Size());
		printf("\t\t slot           = %d\n", bind.gds_pointers.slots[i]);
		printf("\t\t start_register = %d\n", bind.gds_pointers.start_register[i]);
		printf("\t\t extended       = %s\n", (bind.gds_pointers.extended[i] ? "true" : "false"));
	}

	for (int i = 0; i < bind.direct_sgprs.sgprs_num; i++)
	{
		const auto& r = bind.direct_sgprs.sgprs[i];

		printf("\t Direct Sgprs %d\n", i);

		printf("\t\t field = %08" PRIx32 "\n", r.field);

		printf("\t\t start_register = %d\n", bind.direct_sgprs.start_register[i]);
	}
}

void ShaderDbgDumpInputInfo(const ShaderVertexInputInfo* info)
{
	LUMEN_PROFILER_BLOCK("ShaderDbgDumpInputInfo(Vs)");

	printf("ShaderDbgDumpInputInfo()\n");

	printf("\t fetch_external = %s\n", info->fetch_external ? "true" : "false");
	printf("\t fetch_inline   = %s\n", info->fetch_inline ? "true" : "false");
	printf("\t export_count   = %d\n", info->export_count);

	for (int i = 0; i < info->resources_num; i++)
	{
		printf("\t input %d\n", i);

		const auto& r  = info->resources[i];
		const auto& rd = info->resources_dst[i];

		printf("\t\t register_start   = %d\n", rd.register_start);
		printf("\t\t registers_num    = %d\n", rd.registers_num);
		printf("\t\t fields           = %08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "\n", r.fields[3], r.fields[2], r.fields[1],
		       r.fields[0]);
		printf("\t\t Base()           = %" PRIx64 "\n", r.Base44());
		printf("\t\t Stride()         = %" PRIu16 "\n", r.Stride());
		printf("\t\t SwizzleEnabled() = %s\n", r.SwizzleEnabled() ? "true" : "false");
		printf("\t\t NumRecords()     = %" PRIu32 "\n", r.NumRecords());
		printf("\t\t DstSelX()        = %" PRIu8 "\n", r.DstSelX());
		printf("\t\t DstSelY()        = %" PRIu8 "\n", r.DstSelY());
		printf("\t\t DstSelZ()        = %" PRIu8 "\n", r.DstSelZ());
		printf("\t\t DstSelW()        = %" PRIu8 "\n", r.DstSelW());
		printf("\t\t Nfmt()           = %" PRIu8 "\n", r.Nfmt());
		printf("\t\t Dfmt()           = %" PRIu8 "\n", r.Dfmt());
		printf("\t\t MemoryType()     = 0x%02" PRIx8 "\n", r.MemoryType());
		printf("\t\t AddTid()         = %s\n", r.AddTid() ? "true" : "false");
	}

	for (int i = 0; i < info->buffers_num; i++)
	{
		printf("\t buffer %d\n", i);

		const auto& r = info->buffers[i];
		printf("\t\t addr        = %" PRIx64 "\n", r.addr);
		printf("\t\t stride      = %" PRIu32 "\n", r.stride);
		printf("\t\t num_records = %" PRIu32 "\n", r.num_records);
		printf("\t\t attr_num    = %" PRId32 "\n", r.attr_num);
		for (int j = 0; j < r.attr_num; j++)
		{
			printf("\t\t attr_indices[%d]  = %d\n", j, r.attr_indices[j]);
			printf("\t\t attr_offsets[%d]  = %u\n", j, r.attr_offsets[j]);
		}
	}

	ShaderDbgDumpResources(info->bind);
}

void ShaderDbgDumpInputInfo(const ShaderPixelInputInfo* info)
{
	LUMEN_PROFILER_BLOCK("ShaderDbgDumpInputInfo(Ps)");

	printf("ShaderDbgDumpInputInfo()\n");

	printf("\t input_num            = %u\n", info->input_num);
	printf("\t ps_pos_xy            = %s\n", info->ps_pos_xy ? "true" : "false");
	printf("\t ps_pixel_kill_enable = %s\n", info->ps_pixel_kill_enable ? "true" : "false");
	printf("\t ps_early_z           = %s\n", info->ps_early_z ? "true" : "false");
	printf("\t ps_execute_on_noop   = %s\n", info->ps_execute_on_noop ? "true" : "false");
	printf("\t shader_mask          = 0x%08" PRIx32 "\n", info->shader_mask);

	for (uint32_t i = 0; i < info->input_num; i++)
	{
		printf("\t interpolator_settings[%u] = %u\n", i, info->interpolator_settings[i]);
	}

	ShaderDbgDumpResources(info->bind);
}

void ShaderDbgDumpInputInfo(const ShaderComputeInputInfo* info)
{
	printf("ShaderDbgDumpInputInfo()\n");

	printf("\t workgroup_register = %d\n", info->workgroup_register);
	printf("\t thread_ids_num     = %d\n", info->thread_ids_num);
	printf("\t threads_num        = {%u, %u, %u}\n", info->threads_num[0], info->threads_num[1], info->threads_num[2]);
	printf("\t threadgroup_id     = {%s, %s, %s}\n", info->group_id[0] ? "true" : "false", info->group_id[1] ? "true" : "false",
	       info->group_id[2] ? "true" : "false");

	ShaderDbgDumpResources(info->bind);
}

class ShaderLogHelper
{
public:
	explicit ShaderLogHelper(const char* type)
	{
		static std::atomic_int id = 0;

		switch (Config::GetShaderLogDirection())
		{
			case Config::ShaderLogDirection::Console:
				m_console = true;
				m_enabled = true;
				break;
			case Config::ShaderLogDirection::File:
			{
				m_file_name = Config::GetShaderLogFolder().FixDirectorySlash() +
				              String::FromPrintf("%04d_%04d_shader_%s.log", GraphicsRunGetFrameNum(), id++, type);
				Core::File::CreateDirectories(m_file_name.DirectoryWithoutFilename());
				m_file.Create(m_file_name);
				if (m_file.IsInvalid())
				{
					printf(FG_BRIGHT_RED "Can't create file: %s\n" FG_DEFAULT, m_file_name.C_Str());
					m_enabled = false;
				}
				m_enabled = true;
				m_console = false;
			}
			break;
			default: m_enabled = false; break;
		}
	}
	virtual ~ShaderLogHelper()
	{
		if (m_enabled && !m_console && !m_file.IsInvalid())
		{
			m_file.Close();
		}
	}

	LUMEN_CLASS_NO_COPY(ShaderLogHelper);

	void DumpOriginalShader(const ShaderCode& code)
	{
		if (m_enabled)
		{
			if (m_console)
			{
				printf("--------- Original Shader ---------\n");
				printf("crc32 = %08" PRIx32 "\n", code.GetCrc32());
				printf("hash0 = %08" PRIx32 "\n", code.GetHash0());
				printf("---------\n");
				printf("%s", code.DbgDump().c_str());
				printf("---------\n");
			} else if (!m_file.IsInvalid())
			{
				m_file.Printf("--------- Original Shader ---------\n");
				m_file.Printf("crc32 = %08" PRIx32 "\n", code.GetCrc32());
				m_file.Printf("hash0 = %08" PRIx32 "\n", code.GetHash0());
				m_file.Printf("---------\n");
				m_file.Printf("%s", code.DbgDump().c_str());
				m_file.Printf("---------\n");
			}
		}
	}

	void DumpRecompiledShader(const String8& source)
	{
		if (m_enabled)
		{
			if (m_console)
			{
				printf("--------- Recompiled Shader ---------\n");
				printf("%s\n", source.c_str());
				printf("---------\n");
			} else if (!m_file.IsInvalid())
			{
				m_file.Printf("--------- Recompiled Shader ---------\n");
				m_file.Printf("%s\n", source.c_str());
				m_file.Printf("---------\n");
			}
		}
	}

	void DumpOptimizedShader(const Vector<uint32_t>& bin)
	{
		if (m_enabled)
		{
			String8 text;
			if (!SpirvDisassemble(bin.GetDataConst(), bin.Size(), &text))
			{
				EXIT("SpirvDisassemble() failed\n");
			}
			if (m_console)
			{
				printf("--------- Optimized Shader ---------\n");
				printf("%s\n", text.c_str());
				printf("---------\n");
			} else if (!m_file.IsInvalid())
			{
				m_file.Printf("--------- Optimized Shader ---------\n");
				m_file.Printf("%s\n", Log::RemoveColors(String::FromUtf8(text.c_str())).C_Str());
				m_file.Printf("---------\n");
			}
		}
	}

	void DumpGlslShader(const Vector<uint32_t>& bin)
	{
		if (m_enabled)
		{
			String8 text;
			if (!SpirvToGlsl(bin.GetDataConst(), bin.Size(), &text))
			{
				EXIT("SpirvToGlsl() failed\n");
			}
			if (m_console)
			{
				printf("--------- Glsl Shader ---------\n");
				printf("%s\n", text.c_str());
				printf("---------\n");
			} else if (!m_file.IsInvalid())
			{
				m_file.Printf("--------- Glsl Shader ---------\n");
				m_file.Printf("%s\n", Log::RemoveColors(String::FromUtf8(text.c_str())).C_Str());
				m_file.Printf("---------\n");
			}
		}
	}

	void DumpBinary(const Vector<uint32_t>& bin)
	{
		if (m_enabled && !m_console && !m_file.IsInvalid())
		{
			Core::File file;
			String     file_name = m_file_name.FilenameWithoutExtension() + ".spv";
			file.Create(file_name);
			if (file.IsInvalid())
			{
				printf(FG_BRIGHT_RED "Can't create file: %s\n" FG_DEFAULT, file_name.C_Str());
			} else
			{
				file.Write(bin.GetDataConst(), bin.Size() * 4);
				file.Close();
			}
		}
	}

private:
	bool       m_console = false;
	bool       m_enabled = false;
	Core::File m_file;
	String     m_file_name;
};

ShaderCode ShaderParseVS(const HW::VertexShaderInfo* regs, const HW::ShaderRegisters* sh)
{
	LUMEN_PROFILER_FUNCTION(profiler::colors::Amber300);

	EXIT_IF(regs == nullptr);
	EXIT_IF(sh == nullptr);

	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	code.SetThreadTraceMode(ShaderThreadTraceMode::Disabled);

	if (regs->vs_embedded)
	{
		code.SetVsEmbedded(true);
		code.SetVsEmbeddedId(regs->vs_embedded_id);
	} else
	{
		uint32_t hash0 = 0;
		uint32_t crc32 = 0;

		const auto* src = ShaderResolveVertexSource(reinterpret_cast<const uint32_t*>(regs->vs_regs.data_addr));

		EXIT_NOT_IMPLEMENTED(src == nullptr);

		vs_print("ShaderParseVS()", *regs, *sh);
		vs_check(*regs, *sh);
		code.SetFp32Mode(ShaderFp32Mode::RneFlushInOut);

		EXIT_NOT_IMPLEMENTED(
		    !ShaderVertexUserSgprStateSupported(regs->vs_regs.rsrc2.user_sgpr, regs->vs_user_sgpr.count));

		const auto* header = GetBinaryInfo(src);

		EXIT_NOT_IMPLEMENTED(header == nullptr);

		bi_print("ShaderParseVS():ShaderBinaryInfo", *header);

		hash0 = header->hash0;
		crc32 = header->crc32;

		code.SetCrc32(crc32);
		code.SetHash0(hash0);
		if (!regs->vs_regs.rsrc1.dx10_clamp)
		{
			code.SetDx10ClampMode(ShaderDx10ClampMode::Disabled);
			ShaderDx10ClampZeroReceipt("vs", 1u << 0u);
		}
		if (const auto* owned = ShaderOwnedVertexSource(regs->vs_regs.data_addr))
		{
			if (ShaderTryParseBounded(owned->Data(), owned->CodeWords(), &code) == 0)
			{
				uint32_t tail[8] {};
				const uint32_t tail_words = std::min<uint32_t>(owned->CodeWords(), 8);
				std::memcpy(tail, owned->Data() + owned->CodeWords() - tail_words, tail_words * sizeof(uint32_t));
				EXIT("Bounded vertex source refused: address=%016" PRIx64 " code_words=%u hash0=%08" PRIx32
				     " crc32=%08" PRIx32 " tail_words=%u tail=%08" PRIx32 ":%08" PRIx32 ":%08" PRIx32 ":%08" PRIx32
				     ":%08" PRIx32 ":%08" PRIx32 ":%08" PRIx32 ":%08" PRIx32 "\n",
				     owned->Address(), owned->CodeWords(), hash0, crc32, tail_words,
				     tail[0], tail[1], tail[2], tail[3], tail[4], tail[5], tail[6], tail[7]);
			}
		} else
		{
			ShaderParse(src, &code);
		}

		if (g_debug_printfs != nullptr)
		{
			auto id = (static_cast<uint64_t>(hash0) << 32u) | crc32;
			if (auto index = g_debug_printfs->Find(id, [](auto cmd, auto id) { return cmd.id == id; }); g_debug_printfs->IndexValid(index))
			{
				code.GetDebugPrintfs() = g_debug_printfs->At(index).cmds;
			}
		}
	}

	return code;
}

static String8 ShaderPrepareVS(const ShaderCode& code, const ShaderVertexInputInfo* input_info, ShaderLogHelper& log)
{
	LUMEN_PROFILER_FUNCTION(profiler::colors::Amber300);
	EXIT_IF(input_info == nullptr);
	EXIT_NOT_IMPLEMENTED(ShaderVertexReadOnlyStorageHasWriteConflict(code, input_info->bind));
	const char* resource_rejection = nullptr;
	uint32_t resource_rejection_pc = UINT32_MAX;
	if (!ShaderVertexResourceProfileSupported(code, *input_info, &resource_rejection, &resource_rejection_pc))
		EXIT("Vertex resource profile rejected in compiler: %s, pc=%08" PRIx32 "\n", resource_rejection, resource_rejection_pc);
	const char* rect_rejection = nullptr;
	uint32_t rect_rejection_pc = UINT32_MAX;
	if (input_info->rect_list && !ShaderRectListProfileSupported(code, *input_info, &rect_rejection, &rect_rejection_pc))
	{
		EXIT("RectList shader rejected: %s, pc=%08" PRIx32 ", hash0=%08" PRIx32 ", crc32=%08" PRIx32 "\n%s\n",
		     rect_rejection, rect_rejection_pc, code.GetHash0(), code.GetCrc32(), code.DbgDump().c_str());
	}

	String8          source;

	if (code.IsVsEmbedded())
	{
		source = SpirvGetEmbeddedVs(code.GetVsEmbeddedId());
	} else
	{
		for (int i = 0; i < input_info->bind.storage_buffers.buffers_num; i++)
		{
			const auto& r = input_info->bind.storage_buffers.buffers[i];
			EXIT_NOT_IMPLEMENTED(((r.Stride() * r.NumRecords()) & 0x3u) != 0);
		}

		log.DumpOriginalShader(code);

		source = SpirvGenerateSource(code, input_info, nullptr, nullptr);
	}

	log.DumpRecompiledShader(source);

	return source;
}

Vector<uint32_t> ShaderRecompileVS(const ShaderCode& code, const ShaderVertexInputInfo* input_info)
{
	ShaderLogHelper log("vs");
	const auto source = ShaderPrepareVS(code, input_info, log);
	Vector<uint32_t> ret;
	const bool android_fex_structural_profile = ShaderAndroidFexStructuralProfileRequired(code);
	if (String8 err_msg; !SpirvRun(source, &ret, &err_msg, ShaderEffectiveOptimization(code, Config::GetShaderOptimizationType()),
	                              android_fex_structural_profile))
	{
		EXIT("SpirvRun() failed:\n%s\n", err_msg.c_str());
	}

	log.DumpOptimizedShader(ret);

	return ret;
}

ShaderCode ShaderParsePS(const HW::PixelShaderInfo* regs, const HW::ShaderRegisters* sh)
{
	LUMEN_PROFILER_FUNCTION(profiler::colors::Blue300);

	EXIT_IF(regs == nullptr);
	EXIT_IF(sh == nullptr);

	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	code.SetThreadTraceMode(ShaderThreadTraceMode::Disabled);

	if (regs->ps_embedded)
	{
		code.SetPsEmbedded(true);
		code.SetPsEmbeddedId(regs->ps_embedded_id);
	} else
	{
		uint32_t hash0 = 0;
		uint32_t crc32 = 0;

		ps_print("ShaderParsePS()", regs->ps_regs, *sh);
		ps_check(regs->ps_regs, *sh);
		code.SetFp32Mode(ShaderFp32Mode::RneFlushInOut);

		EXIT_NOT_IMPLEMENTED(regs->ps_regs.rsrc2.user_sgpr > regs->ps_user_sgpr.count);

		const auto* src = reinterpret_cast<const uint32_t*>(regs->ps_regs.data_addr);

		EXIT_NOT_IMPLEMENTED(src == nullptr);

		const auto* header = GetBinaryInfo(src);

		EXIT_NOT_IMPLEMENTED(header == nullptr);

		bi_print("ShaderParsePS():ShaderBinaryInfo", *header);

		hash0 = header->hash0;
		crc32 = header->crc32;

		code.SetCrc32(crc32);
		code.SetHash0(hash0);
		if (!regs->ps_regs.rsrc1.dx10_clamp)
		{
			code.SetDx10ClampMode(ShaderDx10ClampMode::Disabled);
			ShaderDx10ClampZeroReceipt("ps", 1u << 1u);
		}
		// shader_parse(0, src, nullptr, &code);
		ShaderParse(src, &code);

		if (g_debug_printfs != nullptr)
		{
			auto id = (static_cast<uint64_t>(hash0) << 32u) | crc32;
			if (auto index = g_debug_printfs->Find(id, [](auto cmd, auto id) { return cmd.id == id; }); g_debug_printfs->IndexValid(index))
			{
				code.GetDebugPrintfs() = g_debug_printfs->At(index).cmds;
			}
		}
	}

	return code;
}

static String8 ShaderPreparePS(const ShaderCode& code, const ShaderPixelInputInfo* input_info, ShaderLogHelper& log)
{
	LUMEN_PROFILER_FUNCTION(profiler::colors::Blue300);
	EXIT_IF(input_info == nullptr);
	EXIT_NOT_IMPLEMENTED(!ShaderIsPixelColorExportCompatible(code, input_info->target_output_mode[0], input_info->shader_mask));

	String8          source;

	if (code.IsPsEmbedded())
	{
		source = SpirvGetEmbeddedPs(code.GetPsEmbeddedId());
	} else
	{
		//		for (uint32_t i = 0; i < input_info->input_num; i++)
		//		{
		//			EXIT_NOT_IMPLEMENTED(input_info->interpolator_settings[i] != i);
		//		}

		for (int i = 0; i < input_info->bind.storage_buffers.buffers_num; i++)
		{
			const auto& r = input_info->bind.storage_buffers.buffers[i];
			EXIT_NOT_IMPLEMENTED(((r.Stride() * r.NumRecords()) & 0x3u) != 0);
		}

		log.DumpOriginalShader(code);

		source = SpirvGenerateSource(code, nullptr, input_info, nullptr);
	}

	log.DumpRecompiledShader(source);

	return source;
}

Vector<uint32_t> ShaderRecompilePS(const ShaderCode& code, const ShaderPixelInputInfo* input_info)
{
	ShaderLogHelper log("ps");
	const auto source = ShaderPreparePS(code, input_info, log);
	Vector<uint32_t> ret;
	const bool android_fex_structural_profile = ShaderAndroidFexStructuralProfileRequired(code);
	if (String8 err_msg; !SpirvRun(source, &ret, &err_msg, ShaderEffectiveOptimization(code, Config::GetShaderOptimizationType()),
	                              android_fex_structural_profile))
	{
		EXIT("SpirvRun() failed:\n%s\n", err_msg.c_str());
	}

	log.DumpOptimizedShader(ret);

	return ret;
}

static CompiledProgramCache::Key ShaderOwnedProgramKey(const String8& source, const ShaderCode& code)
{
	return {std::string(source.GetDataConst(), source.Size()),
	        static_cast<int>(ShaderEffectiveOptimization(code, Config::GetShaderOptimizationType())),
	        ShaderAndroidFexStructuralProfileRequired(code), Config::ShaderValidationEnabled()};
}

static CompiledProgramCache::Result ShaderCompileOwnedProgram(const CompiledProgramCache::Key& key, bool cache)
{
	const auto compile = [&key] {
		// Only owned text and captured compiler options cross the worker boundary.
		// Parsing, source generation, guest reads and renderer state stay on the caller.
		const String8 source(key.source.data(), static_cast<uint32_t>(key.source.size()));
		Vector<uint32_t> words;
		String8 error;
		CompiledProgramCache::Result result;
		result.success = SpirvRun(source, &words, &error, static_cast<Config::ShaderOptimizationType>(key.optimization),
		                          key.structural, key.validation);
		if (result.success) result.words.assign(words.GetDataConst(), words.GetDataConst() + words.Size());
		else result.error.assign(error.GetDataConst(), error.Size());
		return result;
	};
	return cache ? ShaderCompiledPrograms().Compile(key, compile) : compile();
}

struct ShaderOwnedCompileJob
{
	CompiledProgramCache::Key key;
	CompiledProgramCache::Result result;
	bool cache = true;
};

static void ShaderCompileOwnedPair(ShaderOwnedCompileJob* vertex, ShaderOwnedCompileJob* pixel, bool serial = false)
{
	const bool vertex_ready = vertex->cache && ShaderCompiledPrograms().TryGet(vertex->key, &vertex->result);
	const bool pixel_ready = pixel->cache && ShaderCompiledPrograms().TryGet(pixel->key, &pixel->result);
	bool started = false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX
	pthread_t worker {};
	if (!serial && !vertex_ready && !pixel_ready)
	{
		// Bounded to one worker per synchronous pipeline miss. Failure to create it
		// falls back to the same compiler on the caller, without dropping a draw.
		started = pthread_create(&worker, nullptr, [](void* opaque) -> void* {
			auto* job = static_cast<ShaderOwnedCompileJob*>(opaque);
			job->result = ShaderCompileOwnedProgram(job->key, job->cache);
			return nullptr;
		}, vertex) == 0;
	}
#else
	(void)serial;
#endif
	if (!started && !vertex_ready) vertex->result = ShaderCompileOwnedProgram(vertex->key, vertex->cache);
	if (!pixel_ready) pixel->result = ShaderCompileOwnedProgram(pixel->key, pixel->cache);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX
	if (started) EXIT_IF(pthread_join(worker, nullptr) != 0);
#endif
	// Both bytecode results are complete before Vulkan creation or command recording.
}

void ShaderRecompileGraphicsPair(const ShaderCode& vertex_code, const ShaderVertexInputInfo* vertex_input,
                                const ShaderCode& pixel_code, const ShaderPixelInputInfo* pixel_input,
                                Vector<uint32_t>* vertex_words, Vector<uint32_t>* pixel_words)
{
	EXIT_IF(vertex_words == nullptr || pixel_words == nullptr || vertex_words == pixel_words);
	const bool cached = ShaderCompiledProgramsEnabled();
	const bool parallel = ShaderParallelCompilationEnabled();
	if (!cached && !parallel)
	{
		*vertex_words = ShaderRecompileVS(vertex_code, vertex_input);
		*pixel_words = ShaderRecompilePS(pixel_code, pixel_input);
		return;
	}
	static std::atomic_bool reported {false};
	if (!reported.exchange(true, std::memory_order_relaxed))
		std::fprintf(stderr, "[GFX_COMPILED_PROGRAMS] cache=%u parallel=%u owned-text=1 wait-before-draw=1\n",
		             cached ? 1u : 0u, parallel ? 1u : 0u);
	ShaderLogHelper vertex_log("vs"), pixel_log("ps");
	ShaderOwnedCompileJob vertex {ShaderOwnedProgramKey(ShaderPrepareVS(vertex_code, vertex_input, vertex_log), vertex_code), {}, cached};
	ShaderOwnedCompileJob pixel {ShaderOwnedProgramKey(ShaderPreparePS(pixel_code, pixel_input, pixel_log), pixel_code), {}, cached};
	ShaderCompileOwnedPair(&vertex, &pixel, !parallel);
	if (!vertex.result.success) EXIT("SpirvRun() failed:\n%s\n", vertex.result.error.c_str());
	if (!pixel.result.success) EXIT("SpirvRun() failed:\n%s\n", pixel.result.error.c_str());
	vertex_words->Clear();
	pixel_words->Clear();
	vertex_words->Add(vertex.result.words.data(), vertex.result.words.size());
	pixel_words->Add(pixel.result.words.data(), pixel.result.words.size());
	vertex_log.DumpOptimizedShader(*vertex_words);
	pixel_log.DumpOptimizedShader(*pixel_words);
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool ShaderCompiledProgramsContractTest()
{
	const auto vertex_source = SpirvGetEmbeddedVs(0);
	const auto pixel_source = SpirvGetEmbeddedPs(0);
	using Optimization = Config::ShaderOptimizationType;
	for (const auto optimization: {Optimization::None, Optimization::Size, Optimization::Performance})
	for (const bool structural: {false, true})
	for (const bool validate: {false, true})
	{
		Vector<uint32_t> expected_vertex, expected_pixel;
		String8 error;
		if (!SpirvRun(vertex_source, &expected_vertex, &error, optimization, structural, validate) ||
		    !SpirvRun(pixel_source, &expected_pixel, &error, optimization, structural, validate)) return false;
		for (const bool serial: {false, true})
		for (const bool cached: {false, true})
		{
			ShaderCompiledPrograms().Reset();
			for (int repeat = 0; repeat < 2; ++repeat)
			{
			ShaderOwnedCompileJob vertex {{std::string(vertex_source.c_str(), vertex_source.Size()), static_cast<int>(optimization), structural, validate}, {}, cached};
			ShaderOwnedCompileJob pixel {{std::string(pixel_source.c_str(), pixel_source.Size()), static_cast<int>(optimization), structural, validate}, {}, cached};
			ShaderCompileOwnedPair(&vertex, &pixel, serial);
			if (!vertex.result.success || !pixel.result.success || vertex.result.words.size() != expected_vertex.Size() ||
			    pixel.result.words.size() != expected_pixel.Size() ||
			    !std::equal(vertex.result.words.begin(), vertex.result.words.end(), expected_vertex.GetDataConst()) ||
			    !std::equal(pixel.result.words.begin(), pixel.result.words.end(), expected_pixel.GetDataConst())) return false;
			}
		}
	}
	ShaderCompiledPrograms().Reset();
	// Exercise the renderer-facing entry, including source preparation and the
	// process-selected OFF/cache/parallel combination used by the CTest matrix.
	ShaderCode vertex_code, pixel_code;
	vertex_code.SetType(ShaderType::Vertex);
	vertex_code.SetVsEmbedded(true);
	vertex_code.SetVsEmbeddedId(0);
	pixel_code.SetType(ShaderType::Pixel);
	pixel_code.SetPsEmbedded(true);
	pixel_code.SetPsEmbeddedId(0);
	ShaderVertexInputInfo vertex_input {};
	ShaderPixelInputInfo pixel_input {};
	const auto expected_vertex = ShaderRecompileVS(vertex_code, &vertex_input);
	const auto expected_pixel = ShaderRecompilePS(pixel_code, &pixel_input);
	Vector<uint32_t> actual_vertex, actual_pixel;
	ShaderRecompileGraphicsPair(vertex_code, &vertex_input, pixel_code, &pixel_input, &actual_vertex, &actual_pixel);
	return actual_vertex.Size() == expected_vertex.Size() && actual_pixel.Size() == expected_pixel.Size() &&
	       std::equal(actual_vertex.begin(), actual_vertex.end(), expected_vertex.begin()) &&
	       std::equal(actual_pixel.begin(), actual_pixel.end(), expected_pixel.begin());
}
#endif

ShaderCode ShaderParseCS(const HW::ComputeShaderInfo* regs, const HW::ShaderRegisters* sh)
{
	LUMEN_PROFILER_FUNCTION(profiler::colors::CyanA700);

	EXIT_IF(regs == nullptr);
	EXIT_IF(sh == nullptr);

	const auto* src = reinterpret_cast<const uint32_t*>(regs->cs_regs.data_addr);

	EXIT_NOT_IMPLEMENTED(src == nullptr);

	cs_print("ShaderParseCS()", regs->cs_regs, *sh);
	cs_check(regs->cs_regs, *sh);

	EXIT_NOT_IMPLEMENTED(regs->cs_regs.user_sgpr > regs->cs_user_sgpr.count);

	const auto* header = GetBinaryInfo(src);

	EXIT_NOT_IMPLEMENTED(header == nullptr);

	bi_print("ShaderParseCS():ShaderBinaryInfo", *header);

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	code.SetThreadTraceMode(ShaderThreadTraceMode::Disabled);

	code.SetCrc32(header->crc32);
	code.SetHash0(header->hash0);
	// shader_parse(0, src, nullptr, &code);
	ShaderParse(src, &code);

	if (g_debug_printfs != nullptr)
	{
		auto id = (static_cast<uint64_t>(header->hash0) << 32u) | header->crc32;
		if (auto index = g_debug_printfs->Find(id, [](auto cmd, auto id) { return cmd.id == id; }); g_debug_printfs->IndexValid(index))
		{
			code.GetDebugPrintfs() = g_debug_printfs->At(index).cmds;
		}
	}

	return code;
}

Vector<uint32_t> ShaderRecompileCS(const ShaderCode& code, const ShaderComputeInputInfo* input_info)
{
	LUMEN_PROFILER_FUNCTION(profiler::colors::CyanA700);

	ShaderLogHelper log("cs");

	for (int i = 0; i < input_info->bind.storage_buffers.buffers_num; i++)
	{
		const auto& r = input_info->bind.storage_buffers.buffers[i];
		EXIT_NOT_IMPLEMENTED(((r.Stride() * r.NumRecords()) & 0x3u) != 0);
	}

	Vector<uint32_t> ret;

	log.DumpOriginalShader(code);

	auto source = SpirvGenerateSource(code, nullptr, nullptr, input_info);

	log.DumpRecompiledShader(source);

	if (String8 err_msg; !SpirvRun(source, &ret, &err_msg, ShaderEffectiveOptimization(code, Config::GetShaderOptimizationType())))
	{
		EXIT("SpirvRun() failed:\n%s\n", err_msg.c_str());
	}

	log.DumpOptimizedShader(ret);
	// log.DumpGlslShader(ret);
	log.DumpBinary(ret);

	return ret;
}

//// NOLINTNEXTLINE(readability-function-cognitive-complexity)
// static ShaderBindParameters ShaderUpdateBindInfo(const ShaderCode& code, const ShaderBindResources* bind)
//{
//	ShaderBindParameters p {};
//
//	auto find_image_op = [&](int index, int s, bool& found, bool& without_sampler)
//	{
//		const auto& insts = code.GetInstructions();
//		int         size  = static_cast<int>(insts.Size());
//		for (int i = index; i < size; i++)
//		{
//			const auto& inst = insts.At(i);
//
//			if ((inst.dst.type == ShaderOperandType::Sgpr && s >= inst.dst.register_id && s < inst.dst.register_id + inst.dst.size) ||
//			    (inst.dst2.type == ShaderOperandType::Sgpr && s >= inst.dst2.register_id && s < inst.dst2.register_id + inst.dst2.size) ||
//			    inst.type == ShaderInstructionType::SEndpgm)
//			{
//				break;
//			}
//
//			if (inst.type == ShaderInstructionType::ImageStore || inst.type == ShaderInstructionType::ImageStoreMip)
//			{
//				if (inst.src[1].register_id == s)
//				{
//					EXIT_NOT_IMPLEMENTED(found && !without_sampler);
//					without_sampler = true;
//					found           = true;
//				}
//			} else if (inst.type == ShaderInstructionType::ImageSample || inst.type == ShaderInstructionType::ImageLoad)
//			{
//				if (inst.src[1].register_id == s)
//				{
//					EXIT_NOT_IMPLEMENTED(found && without_sampler);
//					without_sampler = false;
//					found           = true;
//				}
//			}
//		}
//	};
//
//	if (bind->textures2D.textures_num > 0)
//	{
//		const auto& insts = code.GetInstructions();
//
//		for (int ti = 0; ti < bind->textures2D.textures_num; ti++)
//		{
//			bool found = false;
//			if (bind->textures2D.desc[ti].extended)
//			{
//				int s = bind->extended.start_register;
//
//				int index = 0;
//				for (const auto& inst: insts)
//				{
//					if ((inst.dst.type == ShaderOperandType::Sgpr && s >= inst.dst.register_id &&
//					     s < inst.dst.register_id + inst.dst.size) ||
//					    (inst.dst2.type == ShaderOperandType::Sgpr && s >= inst.dst2.register_id &&
//					     s < inst.dst2.register_id + inst.dst2.size) ||
//					    inst.type == ShaderInstructionType::SEndpgm)
//					{
//						break;
//					}
//
//					if (inst.type == ShaderInstructionType::SLoadDwordx8 && inst.src[0].register_id == s &&
//					    static_cast<int>(inst.src[1].constant.u >> 2u) + 16 == bind->textures2D.desc[ti].start_register)
//					{
//						find_image_op(index + 1, inst.dst.register_id, found, p.textures2d_without_sampler[ti]);
//					}
//
//					index++;
//				}
//			} else
//			{
//				find_image_op(0, bind->textures2D.desc[ti].start_register, found, p.textures2d_without_sampler[ti]);
//			}
//
//			EXIT_NOT_IMPLEMENTED(!found);
//
//			if (p.textures2d_without_sampler[ti])
//			{
//				p.textures2d_storage_num++;
//			} else
//			{
//				p.textures2d_sampled_num++;
//			}
//		}
//	}
//	return p;
//}
//
// ShaderBindParameters ShaderGetBindParametersVS(const ShaderCode& code, const ShaderVertexInputInfo* input_info)
//{
//	return ShaderUpdateBindInfo(code, &input_info->bind);
//}
//
// ShaderBindParameters ShaderGetBindParametersPS(const ShaderCode& code, const ShaderPixelInputInfo* input_info)
//{
//	return ShaderUpdateBindInfo(code, &input_info->bind);
//}
//
// ShaderBindParameters ShaderGetBindParametersCS(const ShaderCode& code, const ShaderComputeInputInfo* input_info)
//{
//	return ShaderUpdateBindInfo(code, &input_info->bind);
//}

class ShaderIdWords
{
public:
	// Bind: five counts, image format/profile, extension used/slot/register,
	// then each declaration's existing serialized fields. Stage prefixes
	// include every currently optional marker; the final LOD marker is shared.
	static constexpr size_t BIND_WORDS = 15 + 4 * ShaderStorageResources::BUFFERS_MAX +
	    5 * ShaderTextureResources::RES_MAX + 3 * ShaderSamplerResources::RES_MAX +
	    3 * ShaderGdsResources::POINTERS_MAX + ShaderDirectSgprsResources::SGPRS_MAX;
	static constexpr size_t VS_WORDS = 10 + 11 * ShaderVertexInputInfo::RES_MAX + 1 +
	    ShaderVertexInputInfo::RES_MAX * (2 + 2 * ShaderVertexInputBuffer::ATTR_MAX) + BIND_WORDS + 1;
	static constexpr size_t PS_INPUTS = std::extent_v<decltype(ShaderPixelInputInfo::interpolator_settings)>;
	static constexpr size_t PS_WORDS = 9 + PS_INPUTS + BIND_WORDS + 1;
	static constexpr size_t CS_WORDS = 9 + BIND_WORDS + 1;
	static constexpr size_t CAPACITY = std::max({VS_WORDS, PS_WORDS, CS_WORDS, size_t{2}});

	bool TryAdd(uint32_t word)
	{
		if (m_size == CAPACITY) return false;
		m_words[m_size++] = word;
		return true;
	}
	void Add(uint32_t word) { EXIT_IF(!TryAdd(word)); }
	void Publish(ShaderId* id) const
	{
		EXIT_IF(id == nullptr || !id->ids.IsEmpty());
		// One exclusive Vector append avoids a copy-on-write mutex operation
		// for every word. The key order and all fresh descriptor reads stay put.
		id->ids.Add(m_words.data(), static_cast<uint32_t>(m_size));
	}
	[[nodiscard]] size_t Size() const { return m_size; }

private:
	std::array<uint32_t, CAPACITY> m_words; // Only the written prefix is read.
	size_t m_size = 0;
};

static bool ShaderIdBindCountsSupported(const ShaderBindResources& bind)
{
	return bind.storage_buffers.buffers_num >= 0 && bind.storage_buffers.buffers_num <= ShaderStorageResources::BUFFERS_MAX &&
	    bind.textures2D.textures_num >= 0 && bind.textures2D.textures_num <= ShaderTextureResources::RES_MAX &&
	    bind.samplers.samplers_num >= 0 && bind.samplers.samplers_num <= ShaderSamplerResources::RES_MAX &&
	    bind.gds_pointers.pointers_num >= 0 && bind.gds_pointers.pointers_num <= ShaderGdsResources::POINTERS_MAX &&
	    bind.direct_sgprs.sgprs_num >= 0 && bind.direct_sgprs.sgprs_num <= ShaderDirectSgprsResources::SGPRS_MAX;
}

static bool ShaderIdVertexCountsSupported(const ShaderVertexInputInfo& input)
{
	if (!ShaderIdBindCountsSupported(input.bind) || input.resources_num < 0 || input.resources_num > ShaderVertexInputInfo::RES_MAX ||
	    input.buffers_num < 0 || input.buffers_num > ShaderVertexInputInfo::RES_MAX) return false;
	for (int index = 0; index < input.buffers_num; ++index)
		if (input.buffers[index].attr_num < 0 || input.buffers[index].attr_num > ShaderVertexInputBuffer::ATTR_MAX) return false;
	return true;
}

static void ShaderGetBindIds(ShaderIdWords* words, const ShaderBindResources& bind)
{
	EXIT_IF(!ShaderIdBindCountsSupported(bind));
	words->Add(bind.storage_buffers.buffers_num);

	for (int i = 0; i < bind.storage_buffers.buffers_num; i++)
	{
		// const auto& r = bind.storage_buffers.buffers[i];

		// ret->ids.Add(static_cast<uint32_t>(r.SwizzleEnabled()));
		// ret->ids.Add(r.DstSelX());
		// ret->ids.Add(r.DstSelY());
		// ret->ids.Add(r.DstSelZ());
		// ret->ids.Add(r.DstSelW());
		// ret->ids.Add(r.Nfmt());
		// ret->ids.Add(r.Dfmt());
		// ret->ids.Add(static_cast<uint32_t>(r.AddTid()));
		words->Add(bind.storage_buffers.slots[i]);
		words->Add(bind.storage_buffers.start_register[i]);
		words->Add(static_cast<uint32_t>(bind.storage_buffers.extended[i]));
		words->Add(static_cast<uint32_t>(bind.storage_buffers.usages[i]));
	}

	words->Add(bind.textures2D.textures_num);
	// The storage image format is part of the SPIR-V OpTypeImage declaration.
	// Keep it in the pipeline identity while leaving guest addresses out so
	// equivalent descriptor updates retain A-B-A cache reuse.
	words->Add(static_cast<uint32_t>(ShaderGetUniformStorageImageFormat(bind.textures2D)));
	// Non-zero storage base levels use a distinct, descriptor-validated host
	// representation. Keep the profile (not guest addresses or dimensions) in
	// the module identity so a historical base-zero module cannot bypass the
	// instruction gate through a cache hit.
	words->Add(ShaderTextureStorageMipProfileId(bind.textures2D));

	for (int i = 0; i < bind.textures2D.textures_num; i++)
	{
		// const auto& r = bind.textures2D.textures[i];
		// ret->ids.Add(r.MinLod());
		// ret->ids.Add(r.Dfmt());
		// ret->ids.Add(r.Nfmt());
		// ret->ids.Add(r.Width());
		// ret->ids.Add(r.Height());
		// ret->ids.Add(r.PerfMod());
		// ret->ids.Add(static_cast<uint32_t>(r.Interlaced()));
		// ret->ids.Add(r.DstSelX());
		// ret->ids.Add(r.DstSelY());
		// ret->ids.Add(r.DstSelZ());
		// ret->ids.Add(r.DstSelW());
		// ret->ids.Add(r.BaseLevel());
		// ret->ids.Add(r.LastLevel());
		// ret->ids.Add(r.TilingIdx());
		// ret->ids.Add(static_cast<uint32_t>(r.Pow2Pad()));
		const auto texture_type =
		    bind.textures2D.desc[i].IsNullSampledImage() ? ShaderTextureResource::TYPE_2D : bind.textures2D.desc[i].texture.Type();
		words->Add(texture_type);
		// ret->ids.Add(r.Depth());
		// ret->ids.Add(r.Pitch());
		// ret->ids.Add(r.BaseArray());
		// ret->ids.Add(r.LastArray());
		// ret->ids.Add(r.MinLodWarn());
		// ret->ids.Add(r.CounterBankId());
		// ret->ids.Add(static_cast<uint32_t>(r.LodHdwCntEn()));
		words->Add(bind.textures2D.desc[i].slot);
		words->Add(bind.textures2D.desc[i].start_register);
		words->Add(static_cast<uint32_t>(bind.textures2D.desc[i].extended));
		words->Add(static_cast<uint32_t>(bind.textures2D.desc[i].usage));
	}

	words->Add(bind.samplers.samplers_num);

	for (int i = 0; i < bind.samplers.samplers_num; i++)
	{
		// const auto& r = bind.samplers.samplers[i];

		// ret->ids.Add(r.ClampX());
		// ret->ids.Add(r.ClampY());
		// ret->ids.Add(r.ClampZ());
		// ret->ids.Add(r.MaxAnisoRatio());
		// ret->ids.Add(r.DepthCompareFunc());
		// ret->ids.Add(static_cast<uint32_t>(r.ForceUnormCoords()));
		// ret->ids.Add(r.AnisoThreshold());
		// ret->ids.Add(static_cast<uint32_t>(r.McCoordTrunc()));
		// ret->ids.Add(static_cast<uint32_t>(r.ForceDegamma()));
		// ret->ids.Add(r.AnisoBias());
		// ret->ids.Add(static_cast<uint32_t>(r.TruncCoord()));
		// ret->ids.Add(static_cast<uint32_t>(r.DisableCubeWrap()));
		// ret->ids.Add(r.FilterMode());
		// ret->ids.Add(r.MinLod());
		// ret->ids.Add(r.MaxLod());
		// ret->ids.Add(r.PerfMip());
		// ret->ids.Add(r.PerfZ());
		// ret->ids.Add(r.LodBias());
		// ret->ids.Add(r.LodBiasSec());
		// ret->ids.Add(r.XyMagFilter());
		// ret->ids.Add(r.XyMinFilter());
		// ret->ids.Add(r.ZFilter());
		// ret->ids.Add(r.MipFilter());
		// ret->ids.Add(r.BorderColorPtr());
		// ret->ids.Add(r.BorderColorType());
		words->Add(bind.samplers.slots[i]);
		words->Add(bind.samplers.start_register[i]);
		words->Add(static_cast<uint32_t>(bind.samplers.extended[i]));
	}

	words->Add(bind.gds_pointers.pointers_num);

	for (int i = 0; i < bind.gds_pointers.pointers_num; i++)
	{
		// const auto& r = bind.gds_pointers.pointers[i];

		words->Add(bind.gds_pointers.slots[i]);
		words->Add(bind.gds_pointers.start_register[i]);
		words->Add(static_cast<uint32_t>(bind.gds_pointers.extended[i]));
	}

	words->Add(bind.direct_sgprs.sgprs_num);

	for (int i = 0; i < bind.direct_sgprs.sgprs_num; i++)
	{
		words->Add(bind.direct_sgprs.start_register[i]);
	}

	words->Add(static_cast<uint32_t>(bind.extended.used));
	words->Add(bind.extended.slot);
	words->Add(bind.extended.start_register);
	if (bind.constant_storage == ShaderConstantStorage::UniformBuffer)
	{
		words->Add(0x55424f31u); // UBO1: native constant storage is part of the module.
		words->Add(bind.push_constant_offset);
		words->Add(bind.push_constant_size);
		words->Add(ShaderBindResources::UNIFORM_DESCRIPTOR_SET);
		words->Add(ShaderBindResources::UNIFORM_BINDING);
	}
}

static void ShaderAppendImplicitSampleLodZeroCacheIdentity(ShaderIdWords* words, const ShaderBindResources& bind)
{
	if (ShaderImplicitSampleLodZeroExperimentEnabled() && bind.textures2D.textures2d_sampled_num > 0 &&
	    bind.samplers.samplers_num > 0)
	{
		// Generated modules change only in the opt-in process, while shader and
		// pipeline caches can survive it. Keep diagnostic artifacts isolated.
		words->Add(0x494c5a31u); // implicit ImageSample LOD-zero profile, revision 1
	}
}

ShaderId ShaderGetIdVS(const HW::VertexShaderInfo* regs, const ShaderVertexInputInfo* input_info)
{
	LUMEN_PROFILER_FUNCTION();

	ShaderId ret;
	ShaderIdWords words;

	if (regs->vs_embedded)
	{
		words.Add(regs->vs_embedded_id);
		if (ShaderAndroidFexStructuralProfileExperimentEnabled()) words.Add(0x41465331u); // Android FEX SPIR-V size profile, revision 1
		words.Publish(&ret);
		return ret;
	}

	EXIT_IF(!ShaderIdVertexCountsSupported(*input_info));
	const auto* src = ShaderResolveVertexSource(reinterpret_cast<const uint32_t*>(regs->vs_regs.data_addr));

	EXIT_NOT_IMPLEMENTED(src == nullptr);

	const auto* header = GetBinaryInfo(src);

	EXIT_NOT_IMPLEMENTED(header == nullptr);

	ret.hash0 = header->hash0;
	ret.crc32 = header->crc32;
	ret.dx10_clamp_disabled = !regs->vs_regs.rsrc1.dx10_clamp;
	words.Add(header->length);
	if (ShaderAndroidFexStructuralProfileExperimentEnabled())
	{
		// Shader caches can outlive the process-latched compatibility selector.
		words.Add(0x41465331u); // Android FEX SPIR-V size profile, revision 1
	}
	if (ShaderVRsqClampF32ExperimentEnabled())
	{
		// The diagnostic profile is process-latched but shader caches persist
		// across processes. Keep enabled artifacts outside the ordinary keyspace.
		words.Add(0x52535131u); // V_RSQ_CLAMP_F32 diagnostic profile, revision 1
	}

	words.Add(static_cast<uint32_t>(input_info->fetch_external));
	// Preserve the established Gen4 shader-cache key layout.
	words.Add(0u);
	words.Add(static_cast<uint32_t>(input_info->fetch_inline));
	words.Add(input_info->resources_num);
	words.Add(input_info->export_count);
	if (ShaderVertexResourceProfileRequired(*input_info))
	{
		// Conditional extension preserves every ordinary shader identity.
		words.Add(0x56535231u); // read-only vertex resource profile, revision 1
	}
	if (input_info->rect_list)
	{
		// Ordinary shader identities retain their existing fields and order.
		words.Add(0x52454354u);
	}

	for (int i = 0; i < input_info->resources_num; i++)
	{
		const auto& r  = input_info->resources[i];
		const auto& rd = input_info->resources_dst[i];

		words.Add(rd.register_start);
		words.Add(rd.registers_num);
		words.Add(r.Stride());
		words.Add(static_cast<uint32_t>(r.SwizzleEnabled()));
		words.Add(r.DstSelX());
		words.Add(r.DstSelY());
		words.Add(r.DstSelZ());
		words.Add(r.DstSelW());
		words.Add(r.Nfmt());
		words.Add(r.Dfmt());
		words.Add(static_cast<uint32_t>(r.AddTid()));
	}

	words.Add(input_info->buffers_num);

	for (int i = 0; i < input_info->buffers_num; i++)
	{
		const auto& r = input_info->buffers[i];
		words.Add(r.attr_num);
		words.Add(r.stride);
		for (int j = 0; j < r.attr_num; j++)
		{
			words.Add(r.attr_indices[j]);
			words.Add(r.attr_offsets[j]);
		}
	}

	ShaderGetBindIds(&words, input_info->bind);
	ShaderAppendImplicitSampleLodZeroCacheIdentity(&words, input_info->bind);

	words.Publish(&ret);
	return ret;
}

ShaderId ShaderGetIdPS(const HW::PixelShaderInfo* regs, const ShaderPixelInputInfo* input_info)
{
	LUMEN_PROFILER_FUNCTION();

	ShaderId ret;
	ShaderIdWords words;

	if (regs->ps_embedded)
	{
		words.Add(regs->ps_embedded_id);
		if (ShaderAndroidFexStructuralProfileExperimentEnabled()) words.Add(0x41465331u); // Android FEX SPIR-V size profile, revision 1
		words.Publish(&ret);
		return ret;
	}

	EXIT_IF(input_info->input_num > ShaderIdWords::PS_INPUTS || !ShaderIdBindCountsSupported(input_info->bind));
	const auto* src = reinterpret_cast<const uint32_t*>(regs->ps_regs.data_addr);

	EXIT_NOT_IMPLEMENTED(src == nullptr);

	const auto* header = GetBinaryInfo(src);

	EXIT_NOT_IMPLEMENTED(header == nullptr);

	ret.hash0 = header->hash0;
	ret.crc32 = header->crc32;
	ret.dx10_clamp_disabled = !regs->ps_regs.rsrc1.dx10_clamp;

	words.Add(header->length);
	if (ShaderAndroidFexStructuralProfileExperimentEnabled())
	{
		// Shader caches can outlive the process-latched compatibility selector.
		words.Add(0x41465331u); // Android FEX SPIR-V size profile, revision 1
	}

	words.Add(input_info->input_num);
	words.Add(static_cast<uint32_t>(input_info->ps_pos_xy));
	words.Add(static_cast<uint32_t>(input_info->ps_pixel_kill_enable));
	words.Add(static_cast<uint32_t>(input_info->ps_early_z));
	words.Add(static_cast<uint32_t>(input_info->ps_execute_on_noop));
	words.Add(input_info->target_output_mode[0]);
	words.Add(input_info->shader_mask);

	for (uint32_t i = 0; i < input_info->input_num; i++)
	{
		words.Add(input_info->interpolator_settings[i]);
	}

	ShaderGetBindIds(&words, input_info->bind);
	ShaderAppendImplicitSampleLodZeroCacheIdentity(&words, input_info->bind);

	words.Publish(&ret);
	return ret;
}

ShaderId ShaderGetIdCS(const HW::ComputeShaderInfo* regs, const ShaderComputeInputInfo* input_info)
{
	const auto* src = reinterpret_cast<const uint32_t*>(regs->cs_regs.data_addr);

	EXIT_NOT_IMPLEMENTED(src == nullptr);

	const auto* header = GetBinaryInfo(src);

	EXIT_NOT_IMPLEMENTED(header == nullptr);

	ShaderId ret;
	ShaderIdWords words;

	ret.hash0 = header->hash0;
	ret.crc32 = header->crc32;

	words.Add(header->length);

	words.Add(input_info->workgroup_register);
	words.Add(input_info->thread_ids_num);

	for (int i = 0; i < 3; i++)
	{
		words.Add(input_info->threads_num[i]);
		words.Add(static_cast<uint32_t>(input_info->group_id[i]));
	}

	ShaderGetBindIds(&words, input_info->bind);
	ShaderAppendImplicitSampleLodZeroCacheIdentity(&words, input_info->bind);

	words.Publish(&ret);
	return ret;
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool ShaderIdBulkAppendContractTest()
{
	static_assert(ShaderIdWords::BIND_WORDS == 214 && ShaderIdWords::VS_WORDS == 946 &&
	              ShaderIdWords::PS_WORDS == 256 && ShaderIdWords::CS_WORDS == 224);
	// Independent legacy word-order oracle, including maximum array extents.
	// These records exercise key serialization only; no shader is admitted.
	struct Fixture { uint32_t code[2] {0xbeeb03ffu, 0}; ShaderBinaryInfo header {}; } fixture;
	static_assert(offsetof(Fixture, header) == 2 * sizeof(uint32_t));
	fixture.header.length = 0x1234;
	fixture.header.hash0 = 0x10203040;
	fixture.header.crc32 = 0x50607080;
	HW::VertexShaderInfo vs_regs {};
	HW::PixelShaderInfo ps_regs {};
	HW::ComputeShaderInfo cs_regs {};
	vs_regs.vs_regs.data_addr = ps_regs.ps_regs.data_addr = cs_regs.cs_regs.data_addr = reinterpret_cast<uint64_t>(&fixture);
	const bool structural = ShaderAndroidFexStructuralProfileExperimentEnabled();
	const bool rsq = ShaderVRsqClampF32ExperimentEnabled();
	const bool lod = ShaderImplicitSampleLodZeroExperimentEnabled();
	const auto matches = [&](const ShaderId& id, const std::vector<uint32_t>& words, bool dx10) {
		return id.hash0 == fixture.header.hash0 && id.crc32 == fixture.header.crc32 && id.dx10_clamp_disabled == dx10 &&
		    id.ids.Size() == words.size() && std::memcmp(id.ids.GetDataConst(), words.data(), words.size() * sizeof(uint32_t)) == 0;
	};
	const auto append_bind = [&](std::vector<uint32_t>& words, const ShaderBindResources& bind) {
		const auto append = [&](std::initializer_list<uint32_t> values) { words.insert(words.end(), values); };
		append({static_cast<uint32_t>(bind.storage_buffers.buffers_num)});
		for (int i = 0; i < bind.storage_buffers.buffers_num; ++i)
			append({static_cast<uint32_t>(bind.storage_buffers.slots[i]), static_cast<uint32_t>(bind.storage_buffers.start_register[i]),
			        static_cast<uint32_t>(bind.storage_buffers.extended[i]), static_cast<uint32_t>(bind.storage_buffers.usages[i])});
		append({static_cast<uint32_t>(bind.textures2D.textures_num), static_cast<uint32_t>(ShaderStorageImageFormat::Invalid), 0});
		for (int i = 0; i < bind.textures2D.textures_num; ++i)
		{
			const auto& desc = bind.textures2D.desc[i];
			append({ShaderTextureResource::TYPE_2D, static_cast<uint32_t>(desc.slot), static_cast<uint32_t>(desc.start_register),
			        static_cast<uint32_t>(desc.extended), static_cast<uint32_t>(ShaderTextureUsage::ReadOnly)});
		}
		append({static_cast<uint32_t>(bind.samplers.samplers_num)});
		for (int i = 0; i < bind.samplers.samplers_num; ++i)
			append({static_cast<uint32_t>(bind.samplers.slots[i]), static_cast<uint32_t>(bind.samplers.start_register[i]),
			        static_cast<uint32_t>(bind.samplers.extended[i])});
		append({static_cast<uint32_t>(bind.gds_pointers.pointers_num)});
		for (int i = 0; i < bind.gds_pointers.pointers_num; ++i)
			append({static_cast<uint32_t>(bind.gds_pointers.slots[i]), static_cast<uint32_t>(bind.gds_pointers.start_register[i]),
			        static_cast<uint32_t>(bind.gds_pointers.extended[i])});
		append({static_cast<uint32_t>(bind.direct_sgprs.sgprs_num)});
		for (int i = 0; i < bind.direct_sgprs.sgprs_num; ++i) append({static_cast<uint32_t>(bind.direct_sgprs.start_register[i])});
		append({static_cast<uint32_t>(bind.extended.used), static_cast<uint32_t>(bind.extended.slot), static_cast<uint32_t>(bind.extended.start_register)});
		if (lod && bind.textures2D.textures2d_sampled_num > 0 && bind.samplers.samplers_num > 0) append({0x494c5a31u});
	};
	for (int variant = 0; variant < 3; ++variant)
	{
		ShaderVertexInputInfo vs {};
		vs.resources_num = vs.buffers_num = variant == 2 ? ShaderVertexInputInfo::RES_MAX : variant;
		vs.export_count = 5 + variant;
		vs.fetch_external = variant == 1;
		vs.rect_list = variant == 2;
		for (int i = 0; i < vs.resources_num; ++i)
		{
			vs.resources[i].fields[1] = static_cast<uint32_t>(i + 1) << 16u;
			vs.resources_dst[i] = {i + 1, i % 4 + 1};
			vs.buffers[i].attr_num = variant == 2 ? ShaderVertexInputBuffer::ATTR_MAX : 1;
			vs.buffers[i].stride = 16 + i;
			for (int j = 0; j < vs.buffers[i].attr_num; ++j)
			{ vs.buffers[i].attr_indices[j] = j; vs.buffers[i].attr_offsets[j] = i + j * 4; }
		}
		auto& bind = vs.bind;
		bind.storage_buffers.buffers_num = variant == 2 ? ShaderStorageResources::BUFFERS_MAX : variant;
		bind.textures2D.textures_num = variant == 2 ? ShaderTextureResources::RES_MAX : variant;
		bind.textures2D.textures2d_sampled_num = bind.textures2D.textures_num;
		bind.samplers.samplers_num = variant == 2 ? ShaderSamplerResources::RES_MAX : variant;
		bind.gds_pointers.pointers_num = variant == 0 ? 0 : ShaderGdsResources::POINTERS_MAX;
		bind.direct_sgprs.sgprs_num = variant == 2 ? ShaderDirectSgprsResources::SGPRS_MAX : variant;
		for (int i = 0; i < bind.storage_buffers.buffers_num; ++i)
		{ bind.storage_buffers.slots[i] = i; bind.storage_buffers.start_register[i] = 4 * i; bind.storage_buffers.extended[i] = i >= 4;
		  bind.storage_buffers.usages[i] = i % 2 == 0 ? ShaderStorageUsage::Constant : ShaderStorageUsage::ReadOnly; }
		for (int i = 0; i < bind.textures2D.textures_num; ++i)
		{ auto& desc = bind.textures2D.desc[i]; desc.slot = i; desc.start_register = 8 * i; desc.extended = i >= 2;
		  desc.usage = ShaderTextureUsage::ReadOnly;
		  if ((i & 1) == 0) desc.texture.fields[3] = uint32_t{ShaderTextureResource::TYPE_2D} << 28u; }
		for (int i = 0; i < bind.samplers.samplers_num; ++i)
		{ bind.samplers.slots[i] = i; bind.samplers.start_register[i] = 4 * i; bind.samplers.extended[i] = i >= 4; }
		for (int i = 0; i < bind.direct_sgprs.sgprs_num; ++i) bind.direct_sgprs.start_register[i] = 8 + i;
		bind.extended.used = variant != 0; bind.extended.slot = 2; bind.extended.start_register = 14;
		std::vector<uint32_t> expected_vs {fixture.header.length};
		if (structural) expected_vs.push_back(0x41465331u);
		if (rsq) expected_vs.push_back(0x52535131u);
		expected_vs.insert(expected_vs.end(), {static_cast<uint32_t>(vs.fetch_external), 0, 0,
		                                     static_cast<uint32_t>(vs.resources_num), static_cast<uint32_t>(vs.export_count)});
		if (variant != 0) expected_vs.push_back(0x56535231u);
		if (vs.rect_list) expected_vs.push_back(0x52454354u);
		for (int i = 0; i < vs.resources_num; ++i)
			expected_vs.insert(expected_vs.end(), {static_cast<uint32_t>(i + 1), static_cast<uint32_t>(i % 4 + 1),
			                                      static_cast<uint32_t>(i + 1), 0, 0, 0, 0, 0, 0, 0, 0});
		expected_vs.push_back(vs.buffers_num);
		for (int i = 0; i < vs.buffers_num; ++i)
		{ expected_vs.push_back(vs.buffers[i].attr_num); expected_vs.push_back(16 + i);
		  for (int j = 0; j < vs.buffers[i].attr_num; ++j) { expected_vs.push_back(j); expected_vs.push_back(i + j * 4); } }
		append_bind(expected_vs, bind);
		if (!matches(ShaderGetIdVS(&vs_regs, &vs), expected_vs, true) || expected_vs.size() > ShaderIdWords::VS_WORDS) return false;
		ShaderPixelInputInfo ps {}; ps.bind = bind;
		ps.input_num = variant == 2 ? ShaderIdWords::PS_INPUTS : variant;
		for (uint32_t i = 0; i < ps.input_num; ++i) ps.interpolator_settings[i] = 0x100u + i;
		std::vector<uint32_t> expected_ps {fixture.header.length};
		if (structural) expected_ps.push_back(0x41465331u);
		expected_ps.insert(expected_ps.end(), {ps.input_num, 0, 0, 0, 0, 0, 0});
		for (uint32_t i = 0; i < ps.input_num; ++i) expected_ps.push_back(0x100u + i);
		append_bind(expected_ps, bind);
		if (!matches(ShaderGetIdPS(&ps_regs, &ps), expected_ps, true) || expected_ps.size() > ShaderIdWords::PS_WORDS) return false;
		ShaderComputeInputInfo cs {}; cs.bind = bind; cs.workgroup_register = 7; cs.thread_ids_num = 3;
		cs.threads_num[0] = 8; cs.threads_num[1] = 4; cs.threads_num[2] = 2; cs.group_id[0] = cs.group_id[2] = true;
		std::vector<uint32_t> expected_cs {fixture.header.length, 7, 3, 8, 1, 4, 0, 2, 1};
		append_bind(expected_cs, bind);
		if (!matches(ShaderGetIdCS(&cs_regs, &cs), expected_cs, false) || expected_cs.size() > ShaderIdWords::CS_WORDS) return false;
		const auto push_id = ShaderGetIdPS(&ps_regs, &ps);
		ps.bind.constant_storage = ShaderConstantStorage::UniformBuffer;
		ps.bind.push_constant_offset = 160;
		ps.bind.push_constant_size = 112;
		const auto uniform_id = ShaderGetIdPS(&ps_regs, &ps);
		if (uniform_id == push_id) return false;
		ps.bind.push_constant_offset = 176;
		if (uniform_id == ShaderGetIdPS(&ps_regs, &ps)) return false;
		ps.bind.push_constant_offset = 160;
		ps.bind.push_constant_size = 128;
		if (uniform_id == ShaderGetIdPS(&ps_regs, &ps)) return false;
		ps.bind.push_constant_size = 112;
		if (uniform_id != ShaderGetIdPS(&ps_regs, &ps)) return false;
		const auto before = ShaderGetIdVS(&vs_regs, &vs);
		fixture.header.hash0 ^= 1; const auto changed = ShaderGetIdVS(&vs_regs, &vs); fixture.header.hash0 ^= 1;
		if (before == changed || before != ShaderGetIdVS(&vs_regs, &vs)) return false;
		if (variant != 0)
		{ ++vs.resources[0].fields[0]; // Address updates keep the established shape key.
		  if (before != ShaderGetIdVS(&vs_regs, &vs)) return false;
		  vs.resources[0].fields[1] += 1u << 16u;
		  if (before == ShaderGetIdVS(&vs_regs, &vs)) return false; }
	}
	vs_regs.vs_embedded = ps_regs.ps_embedded = true;
	vs_regs.vs_embedded_id = ps_regs.ps_embedded_id = 17;
	const auto embedded_vs = ShaderGetIdVS(&vs_regs, nullptr);
	const auto embedded_ps = ShaderGetIdPS(&ps_regs, nullptr);
	if (embedded_vs != embedded_ps || embedded_vs.hash0 != 0 || embedded_vs.crc32 != 0 || embedded_vs.dx10_clamp_disabled ||
	    embedded_vs.ids.Size() != (structural ? 2u : 1u) || embedded_vs.ids.At(0) != 17 ||
	    (structural && embedded_vs.ids.At(1) != 0x41465331u)) return false;
	ShaderIdWords full;
	for (size_t i = 0; i < ShaderIdWords::CAPACITY; ++i) if (!full.TryAdd(static_cast<uint32_t>(i))) return false;
	if (full.TryAdd(0xdeadbeefu) || full.Size() != ShaderIdWords::CAPACITY) return false;
	ShaderId maximum; full.Publish(&maximum);
	for (size_t i = 0; i < full.Size(); ++i) if (maximum.ids.At(static_cast<uint32_t>(i)) != i) return false;
	for (int count: {-1, 17})
	{ ShaderVertexInputInfo bad {}; bad.resources_num = count;
	  if (ShaderIdVertexCountsSupported(bad)) return false;
	  bad.resources_num = 0; bad.buffers_num = 1; bad.buffers[0].attr_num = count;
	  if (ShaderIdVertexCountsSupported(bad)) return false;
	  bad.buffers[0].attr_num = 0; bad.bind.textures2D.textures_num = count;
	  if (ShaderIdBindCountsSupported(bad.bind)) return false; }
	std::fprintf(stderr, "[GFX_SHADER_ID_BULK_TEST] stages=3 variants=3 embedded=2 bind_max=%zu vs_max=%zu ps_max=%zu cs_max=%zu"
	                     " byte_exact=1 fresh_header=1 fresh_shape=1 overflow=closed structural=%u rsq=%u lod=%u\n",
	             ShaderIdWords::BIND_WORDS, ShaderIdWords::VS_WORDS, ShaderIdWords::PS_WORDS, ShaderIdWords::CS_WORDS,
	             structural ? 1u : 0u, rsq ? 1u : 0u, lod ? 1u : 0u);
	return true;
}
#endif

bool ShaderIsDisabled(uint64_t addr, bool vertex_source)
{
	const auto* src = reinterpret_cast<const uint32_t*>(addr);
	if (vertex_source) src = ShaderResolveVertexSource(src);
	EXIT_NOT_IMPLEMENTED(src == nullptr);

	const auto* header = GetBinaryInfo(src);
	EXIT_NOT_IMPLEMENTED(header == nullptr);

	auto id = (static_cast<uint64_t>(header->hash0) << 32u) | header->crc32;

	bool disabled = (g_disabled_shaders != nullptr && g_disabled_shaders->Contains(id));

	printf("Shader 0x%016" PRIx64 ": id = 0x%016" PRIx64 " - %s\n", addr, id, (disabled ? "disabled" : "enabled"));

	return disabled;
}

void ShaderDisable(uint64_t id)
{
	if (g_disabled_shaders == nullptr)
	{
		g_disabled_shaders = new Vector<uint64_t>;
	}

	if (!g_disabled_shaders->Contains(id))
	{
		g_disabled_shaders->Add(id);
	}
}

void ShaderInjectDebugPrintf(uint64_t id, const ShaderDebugPrintf& cmd)
{
	if (g_debug_printfs == nullptr)
	{
		g_debug_printfs = new Vector<ShaderDebugPrintfCmds>;
	}

	for (auto& c: *g_debug_printfs)
	{
		if (c.id == id)
		{
			c.cmds.Add(cmd);
			return;
		}
	}

	ShaderDebugPrintfCmds c;
	c.id = id;
	c.cmds.Add(cmd);

	g_debug_printfs->Add(c);
}

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED
