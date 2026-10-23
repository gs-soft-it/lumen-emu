#include "Emulator/Graphics/ShaderSpirv.h"

#include "Lumen/Core/ArrayWrapper.h"
#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/Hashmap.h"
#include "Lumen/Core/MagicEnum.h"
#include "Lumen/Core/String8.h"
#include "Lumen/Core/Vector.h"
#include "Lumen/VCvtOffF32I4SpirvCore.h"
#include "Lumen/VMacLegacyF32SpirvCore.h"
#include "Lumen/VRsqClampF32SpirvCore.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Shader.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
#include "Emulator/Graphics/ShaderParse.h"
#include "spirv-tools/libspirv.hpp"
#endif

#ifdef LUMEN_EMU_ENABLED

#define LUMEN_RECOMPILER_ARGS                                                                                                               \
	[[maybe_unused]] uint32_t index, [[maybe_unused]] const ShaderCode &code, [[maybe_unused]] String8 *dst_source,                        \
	    [[maybe_unused]] Spirv *spirv, [[maybe_unused]] const char *const *param, [[maybe_unused]] SccCheck scc_check
#define LUMEN_RECOMPILER_FUNC(f) static bool f(LUMEN_RECOMPILER_ARGS)

namespace Lumen::Core {

LUMEN_HASH_DEFINE_CALC(Lumen::Libs::Graphics::ShaderInstructionTypeFormat)
{
	return hash32(static_cast<uint32_t>(key->type)) ^ hash64(static_cast<uint64_t>(key->format));
}

LUMEN_HASH_DEFINE_EQUALS(Lumen::Libs::Graphics::ShaderInstructionTypeFormat)
{
	return key_a->type == key_b->type && key_a->format == key_b->format;
}
} // namespace Lumen::Core

namespace Lumen::Libs::Graphics {

bool ShaderRectListExperimentEnabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_GFX_RECT_LIST");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

bool ShaderRectListInputSupported(const ShaderVertexInputInfo& input)
{
	if (input.fetch_external || input.fetch_inline || input.resources_num != 0 || input.buffers_num != 0 ||
	    input.export_count < 0 || input.export_count > 5 || input.bind.extended.used ||
	    input.bind.textures2D.textures_num != 0 || input.bind.samplers.samplers_num != 0 ||
	    input.bind.gds_pointers.pointers_num != 0 || input.bind.storage_buffers.buffers_num < 0 ||
	    input.bind.storage_buffers.buffers_num > ShaderStorageResources::BUFFERS_MAX)
	{
		return false;
	}
	for (int index = 0; index < input.bind.storage_buffers.buffers_num; ++index)
	{
		const auto usage = input.bind.storage_buffers.usages[index];
		if (input.bind.storage_buffers.extended[index] ||
		    (usage != ShaderStorageUsage::ReadOnly && usage != ShaderStorageUsage::Constant)) return false;
	}
	return true;
}

static constexpr bool IsImageReadOperation(ShaderInstructionType type)
{
	return type == ShaderInstructionType::ImageLoad || type == ShaderInstructionType::ImageLoadMip ||
	       type == ShaderInstructionType::ImageSample || type == ShaderInstructionType::ImageSampleLz ||
	       type == ShaderInstructionType::ImageSampleLzO;
}

static constexpr bool IsSampledImageOperation(ShaderInstructionType type)
{
	return type == ShaderInstructionType::ImageGetResinfo || IsImageReadOperation(type);
}

static String8 ImageSampleImplicitLodExpression(uint32_t index)
{
	const char* expression = ShaderImplicitSampleLodZeroExperimentEnabled()
	                             ? "OpImageSampleExplicitLod %v4float %t38_<index> %t42_<index> Lod %float_0_000000"
	                             : "OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>";
	return String8(expression).ReplaceStr("<index>", String8::FromPrintf("%u", index));
}

static bool IsPixelType8SampledProfile(const ShaderCode& code, const ShaderTextureResources& textures)
{
	if (code.GetType() != ShaderType::Pixel)
	{
		return false;
	}
	for (int index = 0; index < textures.textures_num; ++index)
	{
		const auto& desc = textures.desc[index];
		if (desc.usage == ShaderTextureUsage::ReadOnly && !desc.IsNullSampledImage() &&
		    desc.texture.Type() == ShaderTextureResource::TYPE_8_COMPAT)
		{
			return true;
		}
	}
	return false;
}

static const ShaderTextureDescriptor* TextureDescriptorForStart(const ShaderTextureResources& textures, ShaderTextureUsage usage,
	                                                            int start_register, bool extended)
{
	const ShaderTextureDescriptor* match = nullptr;
	for (int index = 0; index < textures.textures_num; ++index)
	{
		const auto& desc = textures.desc[index];
		if (desc.usage != usage || desc.start_register != start_register || desc.extended != extended)
		{
			continue;
		}
		if (match != nullptr)
		{
			return nullptr;
		}
		match = &desc;
	}
	return match;
}

struct StorageAdjacentSingleWindowDescriptorProfile
{
	const ShaderTextureDescriptor* sampled = nullptr;
	const ShaderTextureDescriptor* storage = nullptr;
};

static bool IsCanonicalPow2Pitch(uint32_t width, uint32_t pitch)
{
	if (width == 0 || pitch < 8 || (pitch & (pitch - 1u)) != 0 || pitch < width)
	{
		return false;
	}
	return pitch == 8 ? width <= 8 : (pitch >> 1u) < width;
}

static uint32_t TextureMipLevelsForExtent(uint32_t width, uint32_t height)
{
	uint32_t extent = width > height ? width : height;
	uint32_t levels = 0;
	while (extent != 0)
	{
		++levels;
		extent >>= 1u;
	}
	return levels;
}

static bool GetStorageAdjacentSingleWindowDescriptorProfile(
	const ShaderTextureResources& textures, StorageAdjacentSingleWindowDescriptorProfile* profile)
{
	if (textures.textures_num != 2 || textures.textures2d_sampled_num != 1 || textures.textures2d_storage_num != 1)
	{
		return false;
	}

	StorageAdjacentSingleWindowDescriptorProfile candidate {};
	for (int index = 0; index < textures.textures_num; ++index)
	{
		const auto& desc = textures.desc[index];
		if (desc.usage == ShaderTextureUsage::ReadOnly && !desc.textures2d_without_sampler && !desc.IsNullSampledImage())
		{
			if (candidate.sampled != nullptr)
			{
				return false;
			}
			candidate.sampled = &desc;
		} else if (desc.usage == ShaderTextureUsage::ReadWrite && desc.textures2d_without_sampler && !desc.IsNullSampledImage())
		{
			if (candidate.storage != nullptr)
			{
				return false;
			}
			candidate.storage = &desc;
		} else
		{
			return false;
		}
	}
	if (candidate.sampled == nullptr || candidate.storage == nullptr)
	{
		return false;
	}

	const auto& sampled = candidate.sampled->texture;
	const auto& storage = candidate.storage->texture;
	const auto descriptor_supported = [](const ShaderTextureResource& texture)
	{
		const uint32_t width = static_cast<uint32_t>(texture.Width4()) + 1u;
		const uint32_t pitch = static_cast<uint32_t>(texture.Pitch()) + 1u;
		return texture.Type() == ShaderTextureResource::TYPE_2D && texture.Dfmt() == 10 && texture.Nfmt() == 0 &&
		       texture.DstSelXYZW() == DstSel(4, 5, 6, 7) && texture.TileMode() == 13 && texture.MemoryType() == 0x6d &&
		       texture.Pow2Pad() && texture.Base38() != 0 && texture.MinLod() == 0 && texture.PerfMod() == 0 &&
		       !texture.Interlaced() && texture.Depth() == 0 && texture.BaseArray() == 0 && texture.LastArray() == 0 &&
		       texture.MinLodWarn() == 0 && !texture.LodHdwCntEn() && texture.CounterBankId() == 0 &&
		       IsCanonicalPow2Pitch(width, pitch) && texture.fields[5] == 0 &&
		       texture.fields[6] == 0 && texture.fields[7] == 0;
	};
	const uint32_t sampled_base = sampled.BaseLevel();
	const uint32_t storage_base = storage.BaseLevel();
	const uint32_t physical_mip_levels =
	    TextureMipLevelsForExtent(static_cast<uint32_t>(storage.Width4()) + 1u,
	                              static_cast<uint32_t>(storage.Height4()) + 1u);
	if (!descriptor_supported(sampled) || !descriptor_supported(storage) || sampled_base != sampled.LastLevel() ||
	    storage_base != storage.LastLevel() || storage_base >= 15u || storage_base >= physical_mip_levels ||
	    storage_base != sampled_base + 1u)
	{
		return false;
	}

	constexpr uint32_t mip_window_mask = 0x000ff000u;
	for (int word = 0; word < 8; ++word)
	{
		const uint32_t mask = word == 3 ? ~mip_window_mask : UINT32_MAX;
		if ((sampled.fields[word] & mask) != (storage.fields[word] & mask))
		{
			return false;
		}
	}
	if (profile != nullptr)
	{
		*profile = candidate;
	}
	return true;
}

uint32_t ShaderTextureStorageMipProfileId(const ShaderTextureResources& textures)
{
	return GetStorageAdjacentSingleWindowDescriptorProfile(textures, nullptr)
	           ? SHADER_TEXTURE_STORAGE_MIP_PROFILE_ADJACENT_SINGLE_WINDOW
	           : 0u;
}

bool ShaderTextureDescriptorProfileSupportedPreCache(const ShaderTextureResources& textures, ShaderType shader_type)
{
	bool storage_with_nonzero_base = false;
	for (int index = 0; index < textures.textures_num; ++index)
	{
		const auto& desc = textures.desc[index];
		if (desc.usage == ShaderTextureUsage::ReadWrite && !desc.IsNullSampledImage() && desc.texture.BaseLevel() != 0)
		{
			storage_with_nonzero_base = true;
		}
	}
	return !storage_with_nonzero_base ||
	       (shader_type == ShaderType::Compute && GetStorageAdjacentSingleWindowDescriptorProfile(textures, nullptr));
}

static bool OperandWritesSgprRange(const ShaderOperand& operand, int start_register, int register_count)
{
	if (operand.type != ShaderOperandType::Sgpr || start_register < 0 || register_count <= 0)
	{
		return false;
	}
	const int operand_count = operand.size > 0 ? operand.size : 1;
	return operand.register_id < start_register + register_count && start_register < operand.register_id + operand_count;
}

static constexpr bool IsControlFlowTransfer(ShaderInstructionType type)
{
	return type == ShaderInstructionType::SBranch || type == ShaderInstructionType::SCbranchExecz ||
	       type == ShaderInstructionType::SCbranchScc0 || type == ShaderInstructionType::SCbranchScc1 ||
	       type == ShaderInstructionType::SCbranchVccz || type == ShaderInstructionType::SCbranchVccnz ||
	       type == ShaderInstructionType::SSetpcB64 || type == ShaderInstructionType::SSwappcB64;
}

static bool IsControlFlowTarget(const ShaderCode& code, uint32_t pc)
{
	for (const auto& label: code.GetLabels())
	{
		if (label.GetDst() == pc)
		{
			return true;
		}
	}
	for (const auto& label: code.GetIndirectLabels())
	{
		if (label.GetDst() == pc)
		{
			return true;
		}
	}
	return false;
}

static bool ExtendedPointerIsUnmodifiedEntryValue(const ShaderCode& code, uint32_t load_index, int pointer_register)
{
	for (uint32_t scan = 0; scan < load_index; ++scan)
	{
		const auto& inst = code.GetInstructions().At(scan);
		if (OperandWritesSgprRange(inst.dst, pointer_register, 2) || OperandWritesSgprRange(inst.dst2, pointer_register, 2))
		{
			return false;
		}
	}
	return true;
}

static const ShaderTextureDescriptor* ResolveTextureDescriptorForInstruction(const ShaderCode& code,
	                                                                          const ShaderBindResources& bind,
	                                                                          uint32_t instruction_index,
	                                                                          ShaderTextureUsage usage)
{
	if (instruction_index >= code.GetInstructions().Size())
	{
		return nullptr;
	}
	const auto& inst = code.GetInstructions().At(instruction_index);
	if (inst.src_num >= 2 && inst.src[1].type == ShaderOperandType::Sgpr)
	{
		const int resource_register = inst.src[1].register_id;
		const auto* direct_descriptor = TextureDescriptorForStart(bind.textures2D, usage, resource_register, false);
		if (direct_descriptor != nullptr)
		{
			bool entry_descriptor_unchanged = true;
			for (uint32_t scan = 0; scan < instruction_index; ++scan)
			{
				const auto& writer = code.GetInstructions().At(scan);
				if (OperandWritesSgprRange(writer.dst, resource_register, 8) ||
				    OperandWritesSgprRange(writer.dst2, resource_register, 8))
				{
					entry_descriptor_unchanged = false;
					break;
				}
			}
			if (entry_descriptor_unchanged)
			{
				return direct_descriptor;
			}
		}
		if (IsControlFlowTarget(code, inst.pc))
		{
			return nullptr;
		}
		for (uint32_t scan = instruction_index; scan > 0; --scan)
		{
			const auto& writer = code.GetInstructions().At(scan - 1);
			// A linear backward scan is only authoritative inside the current
			// basic block. Crossing a branch could select a writer that does not
			// dominate this sample, so fail closed instead.
			if (IsControlFlowTransfer(writer.type))
			{
				return nullptr;
			}
			const bool writes_resource = OperandWritesSgprRange(writer.dst, resource_register, 8) ||
			                             OperandWritesSgprRange(writer.dst2, resource_register, 8);
			if (!writes_resource)
			{
				if (IsControlFlowTarget(code, writer.pc))
				{
					return nullptr;
				}
				continue;
			}
			if (bind.extended.used && writer.type == ShaderInstructionType::SLoadDwordx8 &&
			    writer.dst.type == ShaderOperandType::Sgpr && writer.dst.register_id == resource_register && writer.dst.size == 8 &&
			    writer.src_num == 2 && writer.src[0].type == ShaderOperandType::Sgpr &&
			    writer.src[0].register_id == bind.extended.start_register && writer.src[0].size == 2 &&
			    writer.src[1].type == ShaderOperandType::LiteralConstant && (writer.src[1].constant.u & 3u) == 0 &&
			    ExtendedPointerIsUnmodifiedEntryValue(code, scan - 1, bind.extended.start_register))
			{
				const uint32_t offset = writer.src[1].constant.u >> 2u;
				if (offset <= static_cast<uint32_t>(ShaderExtendedResources::DATA_DWORDS_MAX - 8))
				{
					return TextureDescriptorForStart(bind.textures2D, usage,
					                                 HW::UserSgprInfo::SGPRS_MAX + static_cast<int>(offset), true);
				}
			}
			return nullptr;
		}
		if (direct_descriptor != nullptr)
		{
			return direct_descriptor;
		}
	}
	return nullptr;
}

static uint8_t ResolveTextureTypeForInstruction(const ShaderCode& code, const ShaderBindResources& bind, uint32_t instruction_index,
	                                            ShaderTextureUsage usage)
{
	if (const auto* descriptor = ResolveTextureDescriptorForInstruction(code, bind, instruction_index, usage); descriptor != nullptr)
	{
		return descriptor->IsNullSampledImage() ? ShaderTextureResource::TYPE_2D : descriptor->texture.Type();
	}
	const auto uniform_type = ShaderGetUniformTextureType(bind.textures2D, usage);
	// Pixel TYPE_8 is intentionally provenance-sensitive. Never turn an
	// unresolved or ambiguous mapping into acceptance merely because every
	// descriptor happens to have the same guest dimension.
	if (code.GetType() == ShaderType::Pixel && uniform_type == ShaderTextureResource::TYPE_8_COMPAT)
	{
		return 0;
	}
	// Preserve established homogeneous descriptor behavior, including shaders
	// that first load an extended descriptor into a different SGPR tuple.
	return uniform_type;
}

static constexpr bool IsAdmittedPixelType8Sample(const ShaderInstruction& inst)
{
	return inst.type == ShaderInstructionType::ImageSample &&
	       inst.format == ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF && !inst.unnormalized_coordinates &&
	       !inst.globally_coherent && inst.dst.size == 4 && inst.src_num == 3 && inst.src[0].size == 3 && inst.src[1].size == 8 &&
	       inst.src[2].size == 4;
}

static constexpr bool IsAdmittedMixedType2DPixelSample(const ShaderInstruction& inst)
{
	if (inst.type != ShaderInstructionType::ImageSample || inst.unnormalized_coordinates || inst.globally_coherent ||
	    inst.src_num != 3 || inst.dst.type != ShaderOperandType::Vgpr || inst.src[0].type != ShaderOperandType::Vgpr ||
	    inst.src[1].type != ShaderOperandType::Sgpr || inst.src[2].type != ShaderOperandType::Sgpr ||
	    inst.src[0].size != 3 || inst.src[1].size != 8 || inst.src[2].size != 4)
	{
		return false;
	}
	// These masks already have ordinary 2D sample lowerings. Sharing the host
	// image array with a 1D descriptor does not change their coordinates or
	// packed destination channels; only the resolved 1D operation needs its bridge.
	switch (inst.format)
	{
		case ShaderInstructionFormat::Vdata1Vaddr3StSsDmask1:
		case ShaderInstructionFormat::Vdata1Vaddr3StSsDmask2:
		case ShaderInstructionFormat::Vdata1Vaddr3StSsDmask4:
		case ShaderInstructionFormat::Vdata1Vaddr3StSsDmask8: return inst.dst.size == 1;
		case ShaderInstructionFormat::Vdata2Vaddr3StSsDmask3:
		case ShaderInstructionFormat::Vdata2Vaddr3StSsDmask5:
		case ShaderInstructionFormat::Vdata2Vaddr3StSsDmask9: return inst.dst.size == 2;
		case ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7: return inst.dst.size == 3;
		case ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF: return inst.dst.size == 4;
		default: return false;
	}
}

static constexpr bool IsAdmittedGloballyCoherentImageStore(const ShaderInstruction& inst)
{
	return inst.type == ShaderInstructionType::ImageStore && inst.format == ShaderInstructionFormat::Vdata4Vaddr3StDmaskF &&
	       inst.unnormalized_coordinates && inst.globally_coherent && inst.dst.size == 4 && inst.src_num == 2 &&
	       inst.src[0].size == 3 && inst.src[1].size == 8;
}

static constexpr bool IsAdmittedR8UnormImageLoad(const ShaderInstruction& inst)
{
	return inst.type == ShaderInstructionType::ImageLoadMip &&
	       inst.format == ShaderInstructionFormat::Vdata4Vaddr4StDmaskF && inst.unnormalized_coordinates &&
	       !inst.globally_coherent && inst.dst.size == 4 && inst.src_num == 2 && inst.src[0].size == 4 && inst.src[1].size == 8;
}

static bool IsStorageAdjacentSingleWindowInstructionProfile(const ShaderCode& code, const ShaderBindResources& bind)
{
	if (code.GetType() != ShaderType::Compute)
	{
		return false;
	}
	StorageAdjacentSingleWindowDescriptorProfile profile {};
	if (!GetStorageAdjacentSingleWindowDescriptorProfile(bind.textures2D, &profile))
	{
		return false;
	}

	uint32_t sampled_loads = 0;
	uint32_t storage_stores = 0;
	uint32_t instruction_index = 0;
	for (const auto& inst: code.GetInstructions())
	{
		if (IsSampledImageOperation(inst.type))
		{
			if (!IsAdmittedR8UnormImageLoad(inst) ||
			    ResolveTextureDescriptorForInstruction(code, bind, instruction_index, ShaderTextureUsage::ReadOnly) != profile.sampled)
			{
				return false;
			}
			sampled_loads++;
		} else if (inst.type == ShaderInstructionType::ImageStore || inst.type == ShaderInstructionType::ImageStoreMip)
		{
			if (!IsAdmittedGloballyCoherentImageStore(inst) ||
			    ResolveTextureDescriptorForInstruction(code, bind, instruction_index, ShaderTextureUsage::ReadWrite) != profile.storage)
			{
				return false;
			}
			storage_stores++;
		}
		instruction_index++;
	}
	return sampled_loads > 0 && storage_stores == 1 && ShaderComputeRequiresPostDispatchImageStoreBarrier(code);
}

static bool IsR8UnormStorageTransferProfile(const ShaderCode& code, const ShaderTextureResources& textures)
{
	if (code.GetType() != ShaderType::Compute || textures.textures_num != 2 || textures.textures2d_sampled_num != 1 ||
	    textures.textures2d_storage_num != 1)
	{
		return false;
	}

	bool sampled_descriptor = false;
	bool storage_descriptor = false;
	for (int index = 0; index < textures.textures_num; ++index)
	{
		const auto& desc = textures.desc[index];
		if (desc.usage == ShaderTextureUsage::ReadOnly && !desc.textures2d_without_sampler &&
		    ShaderTextureR8UnormSampledCompatible(desc.texture))
		{
			if (sampled_descriptor)
			{
				return false;
			}
			sampled_descriptor = true;
		} else if (desc.usage == ShaderTextureUsage::ReadWrite && desc.textures2d_without_sampler &&
		           ShaderTextureR8UnormStorageCompatible(desc.texture))
		{
			if (storage_descriptor)
			{
				return false;
			}
			storage_descriptor = true;
		} else
		{
			return false;
		}
	}

	uint32_t loads  = 0;
	uint32_t stores = 0;
	for (const auto& inst: code.GetInstructions())
	{
		if (IsSampledImageOperation(inst.type))
		{
			if (!IsAdmittedR8UnormImageLoad(inst))
			{
				return false;
			}
			++loads;
		} else if (inst.type == ShaderInstructionType::ImageStore || inst.type == ShaderInstructionType::ImageStoreMip)
		{
			if (!IsAdmittedGloballyCoherentImageStore(inst))
			{
				return false;
			}
			++stores;
		}
	}
	return sampled_descriptor && storage_descriptor && loads == 1 && stores == 1 &&
	       ShaderComputeRequiresPostDispatchImageStoreBarrier(code);
}

static bool GloballyCoherentType2DStorageDescriptorsSupported(const ShaderTextureResources& textures)
{
	bool storage_descriptor_seen = false;
	for (int index = 0; index < textures.textures_num; ++index)
	{
		const auto& desc = textures.desc[index];
		if (desc.usage != ShaderTextureUsage::ReadWrite || desc.IsNullSampledImage())
		{
			continue;
		}
		storage_descriptor_seen = true;
		const auto& texture      = desc.texture;
		if (!desc.textures2d_without_sampler || texture.Type() != ShaderTextureResource::TYPE_2D || texture.Dfmt() != 10 ||
		    texture.Nfmt() != 0 || texture.DstSelXYZW() != DstSel(4, 5, 6, 7) || texture.BaseLevel() != 0 ||
		    texture.LastLevel() != 0 || (texture.TileMode() != 8 && texture.TileMode() != 13) || texture.MemoryType() != 0x6d)
		{
			return false;
		}
	}
	return storage_descriptor_seen;
}

bool ShaderComputeRequiresPostDispatchImageStoreBarrier(const ShaderCode& code)
{
	if (code.GetType() != ShaderType::Compute)
	{
		return false;
	}

	bool coherent_store_seen = false;
	for (const auto& inst: code.GetInstructions())
	{
		const bool coherent_storage_operation =
		    inst.globally_coherent &&
		    (inst.type == ShaderInstructionType::ImageStore || inst.type == ShaderInstructionType::ImageStoreMip);
		if (coherent_storage_operation)
		{
			if (!IsAdmittedGloballyCoherentImageStore(inst))
			{
				return false;
			}
			coherent_store_seen = true;
		} else if (coherent_store_seen && (IsImageReadOperation(inst.type) || IsControlFlowTransfer(inst.type)))
		{
			return false;
		}
	}
	return coherent_store_seen;
}

ShaderComputeImageMutationClass ShaderClassifyComputeImageMutation(const ShaderCode& code,
	                                                                const ShaderBindResources& bind)
{
	if (code.GetType() != ShaderType::Compute ||
	    !ShaderComputeRequiresPostDispatchImageStoreBarrier(code))
	{
		return ShaderComputeImageMutationClass::None;
	}
	if (IsStorageAdjacentSingleWindowInstructionProfile(code, bind))
	{
		return ShaderComputeImageMutationClass::AdjacentSampledToStorage;
	}

	const auto& textures = bind.textures2D;
	if (textures.textures2d_storage_num != 1)
	{
		return ShaderComputeImageMutationClass::None;
	}
	const ShaderTextureDescriptor* storage = nullptr;
	for (int index = 0; index < textures.textures_num; ++index)
	{
		const auto& descriptor = textures.desc[index];
		if (descriptor.usage != ShaderTextureUsage::ReadWrite)
		{
			continue;
		}
		if (storage != nullptr || !descriptor.textures2d_without_sampler || descriptor.texture.Base38() == 0)
		{
			return ShaderComputeImageMutationClass::None;
		}
		storage = &descriptor;
	}
	if (storage == nullptr)
	{
		return ShaderComputeImageMutationClass::None;
	}

	bool     store_seen       = false;
	uint32_t instruction_index = 0;
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.type == ShaderInstructionType::ImageStore || inst.type == ShaderInstructionType::ImageStoreMip)
		{
			if (!IsAdmittedGloballyCoherentImageStore(inst) ||
			    ResolveTextureDescriptorForInstruction(code, bind, instruction_index, ShaderTextureUsage::ReadWrite) != storage)
			{
				return ShaderComputeImageMutationClass::None;
			}
			store_seen = true;
		}
		++instruction_index;
	}
	return store_seen ? ShaderComputeImageMutationClass::UniqueStorageTarget
	                  : ShaderComputeImageMutationClass::None;
}

bool ShaderTextureOperationsSupported(const ShaderCode& code, const ShaderBindResources* bind)
{
	bool single_component_sample = false;
	for (const auto& inst: code.GetInstructions())
		if (inst.format == ShaderInstructionFormat::Vdata1Vaddr3StSsDmask2 ||
		    inst.format == ShaderInstructionFormat::Vdata1Vaddr3StSsDmask4)
		{
			if (!ShaderImageSampleSingleComponentInstructionSupported(inst)) return false;
			single_component_sample = true;
		}
	if (single_component_sample &&
	    (code.GetType() != ShaderType::Pixel || bind == nullptr ||
	     bind->textures2D.textures_num <= 0 || bind->textures2D.textures_num > ShaderTextureResources::RES_MAX ||
	     bind->textures2D.textures2d_sampled_num <= 0 || bind->textures2D.textures2d_sampled_num > bind->textures2D.textures_num ||
	     bind->samplers.samplers_num <= 0 || bind->samplers.samplers_num > ShaderSamplerResources::RES_MAX)) return false;
	if (bind == nullptr)
	{
		return true;
	}
	if (!ShaderTextureDescriptorProfileSupportedPreCache(bind->textures2D, code.GetType()))
	{
		return false;
	}
	const uint32_t storage_mip_profile = ShaderTextureStorageMipProfileId(bind->textures2D);
	if (storage_mip_profile != 0)
	{
		return storage_mip_profile == SHADER_TEXTURE_STORAGE_MIP_PROFILE_ADJACENT_SINGLE_WINDOW &&
		       IsStorageAdjacentSingleWindowInstructionProfile(code, *bind);
	}
	if (bind->textures2D.textures2d_sampled_num > 0)
	{
		const auto uniform_sampled_type = ShaderGetUniformTextureType(bind->textures2D, ShaderTextureUsage::ReadOnly);
		const bool mixed_sampled_types  = uniform_sampled_type == 0;
		const bool pixel_type8_profile = IsPixelType8SampledProfile(code, bind->textures2D);
		if (pixel_type8_profile)
		{
			for (const auto& inst: code.GetInstructions())
			{
				if (IsControlFlowTransfer(inst.type))
				{
					return false;
				}
			}
		}
		for (int descriptor_index = 0; descriptor_index < bind->textures2D.textures_num; ++descriptor_index)
		{
			const auto& desc = bind->textures2D.desc[descriptor_index];
			if (desc.usage != ShaderTextureUsage::ReadOnly || desc.IsNullSampledImage())
			{
				continue;
			}
			if (desc.texture.Type() != ShaderTextureResource::TYPE_8_COMPAT &&
			    desc.texture.Type() != ShaderTextureResource::TYPE_2D)
			{
				return false;
			}
			if (code.GetType() == ShaderType::Pixel && desc.texture.Type() == ShaderTextureResource::TYPE_8_COMPAT &&
			    !ShaderTextureType8PixelSampleCompatible(desc.texture))
			{
				return false;
			}
		}
		uint32_t instruction_index = 0;
		for (const auto& inst: code.GetInstructions())
		{
			if (!IsSampledImageOperation(inst.type))
			{
				instruction_index++;
				continue;
			}
			const auto type = pixel_type8_profile
			                      ? ResolveTextureTypeForInstruction(code, *bind, instruction_index, ShaderTextureUsage::ReadOnly)
			                      : uniform_sampled_type;
			if (type != ShaderTextureResource::TYPE_8_COMPAT && type != ShaderTextureResource::TYPE_2D)
			{
				return false;
			}
			if (mixed_sampled_types && type == ShaderTextureResource::TYPE_2D && !IsAdmittedMixedType2DPixelSample(inst))
			{
				return false;
			}
			if (type == ShaderTextureResource::TYPE_8_COMPAT &&
			    !((code.GetType() == ShaderType::Compute && inst.type == ShaderInstructionType::ImageLoadMip &&
			       inst.unnormalized_coordinates) ||
			      (code.GetType() == ShaderType::Pixel && IsAdmittedPixelType8Sample(inst))))
			{
				return false;
			}
			instruction_index++;
		}
	}
	if (bind->textures2D.textures2d_storage_num > 0)
	{
		const auto type = ShaderGetUniformTextureType(bind->textures2D, ShaderTextureUsage::ReadWrite);
		const auto storage_format = ShaderGetUniformStorageImageFormat(bind->textures2D);
		const bool r8_unorm_profile = storage_format == ShaderStorageImageFormat::R8Unorm;
		const bool coherent_store_barrier = ShaderComputeRequiresPostDispatchImageStoreBarrier(code);
		const bool coherent_type2d_descriptors = GloballyCoherentType2DStorageDescriptorsSupported(bind->textures2D);
		if ((type != ShaderTextureResource::TYPE_8_COMPAT && type != ShaderTextureResource::TYPE_2D) ||
		    storage_format == ShaderStorageImageFormat::Invalid ||
		    (r8_unorm_profile && !IsR8UnormStorageTransferProfile(code, bind->textures2D)))
		{
			return false;
		}
		for (const auto& inst: code.GetInstructions())
		{
			const bool storage_image_op =
			    inst.type == ShaderInstructionType::ImageStore || inst.type == ShaderInstructionType::ImageStoreMip;
			if (type == ShaderTextureResource::TYPE_8_COMPAT && storage_image_op &&
			    (inst.type != ShaderInstructionType::ImageStore || !inst.unnormalized_coordinates || !inst.globally_coherent))
			{
				return false;
			}
			if (!r8_unorm_profile && type == ShaderTextureResource::TYPE_2D && storage_image_op && inst.globally_coherent)
			{
				if (!coherent_store_barrier || !coherent_type2d_descriptors || !IsAdmittedGloballyCoherentImageStore(inst))
				{
					return false;
				}
			}
		}
	}
	return true;
}

static void ValidateTextureOperations(const ShaderCode& code, const ShaderBindResources* bind)
{
	if (!ShaderTextureOperationsSupported(code, bind))
	{
		const int texture_count = bind == nullptr ? 0 : bind->textures2D.textures_num;
		const int sampled_count = bind == nullptr ? 0 : bind->textures2D.textures2d_sampled_num;
		const int storage_count = bind == nullptr ? 0 : bind->textures2D.textures2d_storage_num;
		const int sampler_count = bind == nullptr ? 0 : bind->samplers.samplers_num;
		const bool counts_valid = texture_count >= 0 && texture_count <= ShaderTextureResources::RES_MAX &&
		                          sampled_count >= 0 && sampled_count <= texture_count &&
		                          storage_count >= 0 && storage_count <= texture_count &&
		                          sampler_count >= 0 && sampler_count <= ShaderSamplerResources::RES_MAX;
		const int descriptor_diagnostics = texture_count < 0 ? 0 : texture_count > ShaderTextureResources::RES_MAX ?
		                                                                 ShaderTextureResources::RES_MAX : texture_count;
		// Rejection diagnostics may receive deliberately invalid metadata. Do
		// not call descriptor resolvers with their original unbounded counts.
		const auto* diagnostic_bind = counts_valid ? bind : nullptr;
		const auto sampled_type = diagnostic_bind == nullptr ? 0 :
		                              ShaderGetUniformTextureType(diagnostic_bind->textures2D, ShaderTextureUsage::ReadOnly);
		const auto storage_type = diagnostic_bind == nullptr ? 0 :
		                              ShaderGetUniformTextureType(diagnostic_bind->textures2D, ShaderTextureUsage::ReadWrite);
		if (!counts_valid)
			std::fprintf(stderr,
			             "[GFX_TEXTURE_OPERATION_UNSUPPORTED] invalid-count-metadata textures=%d texture_capacity=%d sampled=%d "
			             "storage=%d samplers=%d sampler_capacity=%d descriptor_diagnostics=%d\n",
			             texture_count, ShaderTextureResources::RES_MAX, sampled_count, storage_count, sampler_count,
			             ShaderSamplerResources::RES_MAX, descriptor_diagnostics);
		std::fprintf(stderr,
		             "[GFX_TEXTURE_OPERATION_UNSUPPORTED] shader_type=%u instructions=%u textures=%d sampled=%d sampled_type=%u "
		             "storage=%d storage_type=%u\n",
		             static_cast<unsigned>(code.GetType()), code.GetInstructions().Size(), bind == nullptr ? 0 : bind->textures2D.textures_num,
		             bind == nullptr ? 0 : bind->textures2D.textures2d_sampled_num, sampled_type,
		             bind == nullptr ? 0 : bind->textures2D.textures2d_storage_num, storage_type);
		if (bind != nullptr)
		{
			for (int index = 0; index < descriptor_diagnostics; ++index)
			{
				const auto& desc = bind->textures2D.desc[index];
				std::fprintf(stderr,
				             "[GFX_TEXTURE_OPERATION_UNSUPPORTED] descriptor=%d usage=%u type=%u null=%u dfmt=%u nfmt=%u "
				             "width=%u height=%u pitch=%u base_level=%u last_level=%u tile=%u\n",
				             index, static_cast<unsigned>(desc.usage), desc.texture.Type(), desc.IsNullSampledImage() ? 1u : 0u,
				             desc.texture.Dfmt(), desc.texture.Nfmt(), static_cast<unsigned>(desc.texture.Width4() + 1u),
				             static_cast<unsigned>(desc.texture.Height4() + 1u), static_cast<unsigned>(desc.texture.Pitch() + 1u),
				             desc.texture.BaseLevel(), desc.texture.LastLevel(), desc.texture.TileMode());
			}
		}
		uint32_t instruction_index = 0;
		for (const auto& inst: code.GetInstructions())
		{
			const bool image_op = inst.type == ShaderInstructionType::ImageGetResinfo || inst.type == ShaderInstructionType::ImageLoad ||
			                      inst.type == ShaderInstructionType::ImageLoadMip || inst.type == ShaderInstructionType::ImageSample ||
			                      inst.type == ShaderInstructionType::ImageSampleLz || inst.type == ShaderInstructionType::ImageSampleLzO ||
			                      inst.type == ShaderInstructionType::ImageStore || inst.type == ShaderInstructionType::ImageStoreMip;
			if (image_op)
			{
				const auto* sampled_descriptor = diagnostic_bind == nullptr ? nullptr : ResolveTextureDescriptorForInstruction(
				                                                        code, *diagnostic_bind, instruction_index, ShaderTextureUsage::ReadOnly);
				const auto* storage_descriptor = diagnostic_bind == nullptr ? nullptr : ResolveTextureDescriptorForInstruction(
				                                                        code, *diagnostic_bind, instruction_index, ShaderTextureUsage::ReadWrite);
				const int sampled_descriptor_index = sampled_descriptor == nullptr ? -1 :
				                                         static_cast<int>(sampled_descriptor - bind->textures2D.desc);
				const int storage_descriptor_index = storage_descriptor == nullptr ? -1 :
				                                         static_cast<int>(storage_descriptor - bind->textures2D.desc);
				const int resource_type = inst.src_num >= 2 ? static_cast<int>(inst.src[1].type) : -1;
				const int resource_register = inst.src_num >= 2 ? inst.src[1].register_id : -1;
				const int resource_size = inst.src_num >= 2 ? inst.src[1].size : 0;
				std::fprintf(stderr,
				             "[GFX_TEXTURE_OPERATION_UNSUPPORTED] instruction=%u pc=0x%08" PRIx32
				             " type=%u format=0x%016" PRIx64
				             " unorm=%u glc=%u resource_type=%d resource_register=%d resource_size=%d sampled_descriptor=%d "
				             "storage_descriptor=%d\n",
				             instruction_index, inst.pc, static_cast<unsigned>(inst.type), static_cast<uint64_t>(inst.format),
				             inst.unnormalized_coordinates ? 1u : 0u, inst.globally_coherent ? 1u : 0u, resource_type,
				             resource_register, resource_size, sampled_descriptor_index, storage_descriptor_index);
			}
			++instruction_index;
		}
		EXIT("unsupported texture operation profile\n");
	}
}

constexpr char FUNC_FETCH_4[] = R"(
       ; Function fetch_f1_f1_f1_f1_vf4_
       ; void fetch(out float p1, out float p2, out float p3, out float p4, in vec4 attr)
       ; {
       ; p1 = attr.x;
       ; p2 = attr.y;
       ; p3 = attr.z;
       ; p4 = attr.w;
       ; }
%fetch_f1_f1_f1_f1_vf4_ = OpFunction %void None %function_fetch4 
 %fetch_p1 = OpFunctionParameter %_ptr_Function_float
 %fetch_p2 = OpFunctionParameter %_ptr_Function_float
 %fetch_p3 = OpFunctionParameter %_ptr_Function_float
 %fetch_p4 = OpFunctionParameter %_ptr_Function_float
%fetch_attr = OpFunctionParameter %_ptr_Function_v4float
%fetch_label = OpLabel
 %fetch_20 = OpAccessChain %_ptr_Function_float %fetch_attr %uint_0 
 %fetch_21 = OpLoad %float %fetch_20
             OpStore %fetch_p1 %fetch_21
 %fetch_23 = OpAccessChain %_ptr_Function_float %fetch_attr %uint_1 
 %fetch_24 = OpLoad %float %fetch_23
             OpStore %fetch_p2 %fetch_24
 %fetch_26 = OpAccessChain %_ptr_Function_float %fetch_attr %uint_2 
 %fetch_27 = OpLoad %float %fetch_26
             OpStore %fetch_p3 %fetch_27 
 %fetch_29 = OpAccessChain %_ptr_Function_float %fetch_attr %uint_3 
 %fetch_30 = OpLoad %float %fetch_29
             OpStore %fetch_p4 %fetch_30
             OpReturn
             OpFunctionEnd
)";

constexpr char FUNC_FETCH_3[] = R"(
       ; Function fetch_f1_f1_f1_vf3_
       ; void fetch(out float p1, out float p2, out float p3, in vec3 attr)
       ; {
       ; p1 = attr.x;
       ; p2 = attr.y;
       ; p3 = attr.z;
       ; }
%fetch_f1_f1_f1_vf3_ = OpFunction %void None %function_fetch3
       %fetch3_p1_0 = OpFunctionParameter %_ptr_Function_float
       %fetch3_p2_0 = OpFunctionParameter %_ptr_Function_float
       %fetch3_p3_0 = OpFunctionParameter %_ptr_Function_float
     %fetch3_attr_0 = OpFunctionParameter %_ptr_Function_v3float
         %fetch3_26 = OpLabel
         %fetch3_53 = OpAccessChain %_ptr_Function_float %fetch3_attr_0 %uint_0
         %fetch3_54 = OpLoad %float %fetch3_53
               OpStore %fetch3_p1_0 %fetch3_54
         %fetch3_55 = OpAccessChain %_ptr_Function_float %fetch3_attr_0 %uint_1
         %fetch3_56 = OpLoad %float %fetch3_55
               OpStore %fetch3_p2_0 %fetch3_56
         %fetch3_57 = OpAccessChain %_ptr_Function_float %fetch3_attr_0 %uint_2
         %fetch3_58 = OpLoad %float %fetch3_57
               OpStore %fetch3_p3_0 %fetch3_58
               OpReturn
               OpFunctionEnd
)";

constexpr char FUNC_FETCH_2[] = R"(
       ; Function fetch_f1_f1_vf2_
       ; void fetch(out float p1, out float p2, in vec2 attr)
       ; {
       ; p1 = attr.x;
       ; p2 = attr.y;
       ; }
%fetch_f1_f1_vf2_ = OpFunction %void None %function_fetch2
       %fetch2_p1_1 = OpFunctionParameter %_ptr_Function_float
       %fetch2_p2_1 = OpFunctionParameter %_ptr_Function_float
     %fetch2_attr_1 = OpFunctionParameter %_ptr_Function_v2float
         %fetch2_34 = OpLabel
         %fetch2_59 = OpAccessChain %_ptr_Function_float %fetch2_attr_1 %uint_0
         %fetch2_60 = OpLoad %float %fetch2_59
               OpStore %fetch2_p1_1 %fetch2_60
         %fetch2_61 = OpAccessChain %_ptr_Function_float %fetch2_attr_1 %uint_1
         %fetch2_62 = OpLoad %float %fetch2_61
               OpStore %fetch2_p2_1 %fetch2_62
               OpReturn
               OpFunctionEnd
)";

constexpr char FUNC_FETCH_1[] = R"(
       ; Function fetch_f1_f1_
       ; void fetch(out float p1, in float attr)
       ; {
       ; p1 = attr;
       ; }
%fetch_f1_f1_ = OpFunction %void None %function_fetch1
       %fetch1_p1_2 = OpFunctionParameter %_ptr_Function_float
     %fetch1_attr_2 = OpFunctionParameter %_ptr_Function_float
         %fetch1_39 = OpLabel
         %fetch1_63 = OpLoad %float %fetch1_attr_2
               OpStore %fetch1_p1_2 %fetch1_63
               OpReturn
               OpFunctionEnd
)";

constexpr char FUNC_ABS_DIFF[] = R"(
                    ; uint abs_diff(uint u1, uint u2)
                    ; {
                    ; 	return max(u1,u2)-min(u1,u2);	
                    ; }
%abs_diff = OpFunction %uint None %function_u_u
         %abs_diff_18 = OpFunctionParameter %uint
         %abs_diff_19 = OpFunctionParameter %uint
         %abs_diff_21 = OpLabel
         %abs_diff_50 = OpExtInst %uint %GLSL_std_450 UMax %abs_diff_18 %abs_diff_19
         %abs_diff_53 = OpExtInst %uint %GLSL_std_450 UMin %abs_diff_18 %abs_diff_19
         %abs_diff_54 = OpISub %uint %abs_diff_50 %abs_diff_53
               OpReturnValue %abs_diff_54
               OpFunctionEnd
)";

constexpr char FUNC_WQM[] = R"(
                    ; uint w(uint u, uint s, uint m)
                    ; {
                    ; 	return ((u >> s) & 0xF) != 0 ? m : 0;
                    ; }
         %wqm = OpFunction %uint None %function_u_u_u
         %wqm_155 = OpFunctionParameter %uint
         %wqm_156 = OpFunctionParameter %uint
         %wqm_161 = OpFunctionParameter %uint
         %wqm_50 = OpLabel
        %wqm_157 = OpShiftRightLogical %uint %wqm_155 %wqm_156
        %wqm_159 = OpBitwiseAnd %uint %wqm_157 %uint_15
        %wqm_160 = OpINotEqual %bool %wqm_159 %uint_0
        %wqm_162 = OpSelect %uint %wqm_160 %wqm_161 %uint_0
               OpReturnValue %wqm_162
               OpFunctionEnd
)";

constexpr char FUNC_ADDC[] = R"(
                  ; uvec2 addc(uint a, uint b, uint c)
                  ; {
                  ; 	uint cc = 0;
                  ; 	uint sum = uaddCarry(a, b, cc) + c;
                  ; 	return uvec2(sum, (cc != 0 || (c !=0 && sum == 0)) ? 1u : 0u);
                  ; }
         %addc = OpFunction %v2uint None %function_u2_u_u_u
         %addc_47 = OpFunctionParameter %uint
         %addc_48 = OpFunctionParameter %uint
         %addc_49 = OpFunctionParameter %uint
         %addc_51 = OpLabel
        %addc_156 = OpIAddCarry %ResTypeU %addc_47 %addc_48
        %addc_157 = OpCompositeExtract %uint %addc_156 1
        %addc_158 = OpCompositeExtract %uint %addc_156 0
        %addc_160 = OpIAdd %uint %addc_158 %addc_49
        %addc_163 = OpINotEqual %bool %addc_157 %uint_0
        %addc_164 = OpLogicalNot %bool %addc_163
               OpSelectionMerge %addc_166 None
               OpBranchConditional %addc_164 %addc_165 %addc_166
        %addc_165 = OpLabel
        %addc_168 = OpINotEqual %bool %addc_49 %uint_0
        %addc_170 = OpIEqual %bool %addc_160 %uint_0
        %addc_171 = OpLogicalAnd %bool %addc_168 %addc_170
               OpBranch %addc_166
        %addc_166 = OpLabel
        %addc_172 = OpPhi %bool %addc_163 %addc_51 %addc_171 %addc_165
        %addc_173 = OpSelect %uint %addc_172 %uint_1 %uint_0
        %addc_174 = OpCompositeConstruct %v2uint %addc_160 %addc_173
               OpReturnValue %addc_174
               OpFunctionEnd
)";

constexpr char FUNC_MIPMAP[] = R"(
                  ; uvec2 mipmap(uint lod, uint width, uint height)
                  ; {
                  ; 	uint mip_width  = width;
                  ; 	uint mip_height = height;
                  ; 	uint mip_x      = 0;
                  ; 	uint mip_y      = 0;
                  ; 	for (uint i = 0; i < 16; i++)
                  ; 	{
                  ; 		if (i == lod)
                  ; 		{
                  ; 			return uvec2(mip_x, mip_y);
                  ; 		}
                  ; 		bool odd = ((i & 1u) != 0u);
                  ; 		mip_x += (odd ? mip_width : 0u);
                  ; 		mip_y += (odd ? 0u : mip_height);
                  ; 		mip_width >>= (mip_width > 1u ? 1u : 0u);
                  ; 		mip_height >>= (mip_height > 1u ? 1u : 0u);
                  ; 	}
                  ; 	return uvec2(mip_x, mip_y);
                  ; }
         %mipmap = OpFunction %v2uint None %function_u2_u_u_u
         %mipmap_33 = OpFunctionParameter %uint
         %mipmap_16 = OpFunctionParameter %uint
         %mipmap_18 = OpFunctionParameter %uint		 
         %mipmap_14 = OpLabel
               OpSelectionMerge %mipmap_188 None
               OpSwitch %uint_0 %mipmap_191
        %mipmap_191 = OpLabel
               OpBranch %mipmap_23
         %mipmap_23 = OpLabel
        %mipmap_296 = OpPhi %uint %uint_0 %mipmap_191 %mipmap_56 %mipmap_26
        %mipmap_295 = OpPhi %uint %mipmap_18 %mipmap_191 %mipmap_66 %mipmap_26
        %mipmap_294 = OpPhi %uint %uint_0 %mipmap_191 %mipmap_51 %mipmap_26
        %mipmap_293 = OpPhi %uint %mipmap_16 %mipmap_191 %mipmap_61 %mipmap_26
        %mipmap_292 = OpPhi %uint %uint_0 %mipmap_191 %mipmap_70 %mipmap_26
               OpLoopMerge %mipmap_25 %mipmap_26 None
               OpBranch %mipmap_27
         %mipmap_27 = OpLabel
         %mipmap_31 = OpULessThan %bool %mipmap_292 %uint_16
               OpBranchConditional %mipmap_31 %mipmap_24 %mipmap_25
         %mipmap_24 = OpLabel
         %mipmap_34 = OpIEqual %bool %mipmap_292 %mipmap_33
               OpSelectionMerge %mipmap_36 None
               OpBranchConditional %mipmap_34 %mipmap_35 %mipmap_36
         %mipmap_35 = OpLabel
         %mipmap_39 = OpCompositeConstruct %v2uint %mipmap_294 %mipmap_296
               OpBranch %mipmap_25
         %mipmap_36 = OpLabel
         %mipmap_45 = OpBitwiseAnd %uint %mipmap_292 %uint_1
         %mipmap_46 = OpINotEqual %bool %mipmap_45 %uint_0
         %mipmap_49 = OpSelect %uint %mipmap_46 %mipmap_293 %uint_0
         %mipmap_51 = OpIAdd %uint %mipmap_294 %mipmap_49
         %mipmap_54 = OpSelect %uint %mipmap_46 %uint_0 %mipmap_295
         %mipmap_56 = OpIAdd %uint %mipmap_296 %mipmap_54
         %mipmap_58 = OpUGreaterThan %bool %mipmap_293 %uint_1
         %mipmap_59 = OpSelect %uint %mipmap_58 %uint_1 %uint_0
         %mipmap_61 = OpShiftRightLogical %uint %mipmap_293 %mipmap_59
         %mipmap_63 = OpUGreaterThan %bool %mipmap_295 %uint_1
         %mipmap_64 = OpSelect %uint %mipmap_63 %uint_1 %uint_0
         %mipmap_66 = OpShiftRightLogical %uint %mipmap_295 %mipmap_64
               OpBranch %mipmap_26
         %mipmap_26 = OpLabel
         %mipmap_70 = OpIAdd %uint %mipmap_292 %int_1
               OpBranch %mipmap_23
         %mipmap_25 = OpLabel
        %mipmap_302 = OpPhi %v2uint %undef_v2uint %mipmap_27 %mipmap_39 %mipmap_35
        %mipmap_297 = OpPhi %bool %false %mipmap_27 %true %mipmap_35
               OpSelectionMerge %mipmap_195 None
               OpBranchConditional %mipmap_297 %mipmap_188 %mipmap_195
        %mipmap_195 = OpLabel
         %mipmap_73 = OpCompositeConstruct %v2uint %mipmap_294 %mipmap_296
               OpBranch %mipmap_188
        %mipmap_188 = OpLabel
        %mipmap_301 = OpPhi %v2uint %mipmap_302 %mipmap_25 %mipmap_73 %mipmap_195
               OpReturnValue %mipmap_301
               OpFunctionEnd
)";

constexpr char FUNC_ORDERED[] = R"(
                  ; bool unordered(float f1, float f2)
                  ; {
                  ; 	return (isnan(f1) || isnan(f2));
                  ; }
                  ; bool ordered(float f1, float f2)
                  ; {
                  ; 	return !unordered(f1, f2);
                  ; }
  %unordered = OpFunction %bool None %function_b_f_f
         %ord_49 = OpFunctionParameter %float
         %ord_50 = OpFunctionParameter %float
         %ord_52 = OpLabel
        %ord_156 = OpIsNan %bool %ord_49
        %ord_157 = OpLogicalNot %bool %ord_156
               OpSelectionMerge %ord_159 None
               OpBranchConditional %ord_157 %ord_158 %ord_159
        %ord_158 = OpLabel
        %ord_161 = OpIsNan %bool %ord_50
               OpBranch %ord_159
        %ord_159 = OpLabel
        %ord_162 = OpPhi %bool %ord_156 %ord_52 %ord_161 %ord_158
               OpReturnValue %ord_162
               OpFunctionEnd
    %ordered = OpFunction %bool None %function_b_f_f
         %ord_53 = OpFunctionParameter %float
         %ord_54 = OpFunctionParameter %float
         %ord_56 = OpLabel
        %ord_169 = OpFunctionCall %bool %unordered %ord_53 %ord_54
        %ord_170 = OpLogicalNot %bool %ord_169
               OpReturnValue %ord_170
               OpFunctionEnd
)";

constexpr char FUNC_MUL_EXTENDED[] = R"(
               ; uint mul_lo_uint(uint u1, uint u2)
               ; {
               ; 	uint r1, r2;
               ; 	umulExtended(u1, u2, r1, r2);
               ; 	return r2;
               ; }
               ; uint mul_hi_uint(uint u1, uint u2)
               ; {
               ; 	uint r1, r2;
               ; 	umulExtended(u1, u2, r1, r2);
               ; 	return r1;
               ; }
               ; int mul_lo_int(int i1, int i2)
               ; {
               ; 	int r1, r2;
               ; 	imulExtended(i1, i2, r1, r2);
               ; 	return r2;
               ; }
               ; int mul_hi_int(int i1, int i2)
               ; {
               ; 	int r1, r2;
               ; 	imulExtended(i1, i2, r1, r2);
               ; 	return r1;
               ; }
         %mul_lo_uint = OpFunction %uint None %function_u_u
         %22 = OpFunctionParameter %uint
         %23 = OpFunctionParameter %uint
         %25 = OpLabel
         %79 = OpUMulExtended %ResTypeU %22 %23
         %80 = OpCompositeExtract %uint %79 0
               OpReturnValue %80
               OpFunctionEnd
         %mul_hi_uint = OpFunction %uint None %function_u_u
         %26 = OpFunctionParameter %uint
         %27 = OpFunctionParameter %uint
         %29 = OpLabel
         %89 = OpUMulExtended %ResTypeU %26 %27
         %91 = OpCompositeExtract %uint %89 1
               OpReturnValue %91
               OpFunctionEnd
         %mul_lo_int = OpFunction %int None %function_i_i
         %31 = OpFunctionParameter %int
         %32 = OpFunctionParameter %int
         %34 = OpLabel
        %100 = OpSMulExtended %ResTypeI %31 %32
        %101 = OpCompositeExtract %int %100 0
               OpReturnValue %101
               OpFunctionEnd
         %mul_hi_int = OpFunction %int None %function_i_i
         %35 = OpFunctionParameter %int
         %36 = OpFunctionParameter %int
         %38 = OpLabel
        %110 = OpSMulExtended %ResTypeI %35 %36
        %112 = OpCompositeExtract %int %110 1
               OpReturnValue %112
               OpFunctionEnd
)";

constexpr char FUNC_SHIFT_RIGHT[] = R"(
                    ; void shift_r(out uint d0, out uint d1, in uint s0, in uint s1, in uint n)
                    ; {
                    ; 	d0 = n < 32 ? (s0 >> n) | (n != 0 ? (s1 << (32 - n)) : 0) : (n < 64 ? s1 >> (n - 32) : 0) ;
                    ; 	d1 = n < 32 ? s1 >> n : 0;
                    ; }
%shift_right = OpFunction %void None %function_shift
          %shr_9 = OpFunctionParameter %_ptr_Function_uint
         %shr_10 = OpFunctionParameter %_ptr_Function_uint
         %shr_11 = OpFunctionParameter %_ptr_Function_uint
         %shr_12 = OpFunctionParameter %_ptr_Function_uint
         %shr_13 = OpFunctionParameter %_ptr_Function_uint
         %shr_15 = OpLabel
         %shr_27 = OpVariable %_ptr_Function_uint Function
         %shr_36 = OpVariable %_ptr_Function_uint Function
         %shr_50 = OpVariable %_ptr_Function_uint Function
         %shr_62 = OpVariable %_ptr_Function_uint Function
         %shr_23 = OpLoad %uint %shr_13
         %shr_26 = OpULessThan %bool %shr_23 %uint_32
               OpSelectionMerge %shr_29 None
               OpBranchConditional %shr_26 %shr_28 %shr_46
         %shr_28 = OpLabel
         %shr_30 = OpLoad %uint %shr_11
         %shr_31 = OpLoad %uint %shr_13
         %shr_32 = OpShiftRightLogical %uint %shr_30 %shr_31
         %shr_33 = OpLoad %uint %shr_13
         %shr_35 = OpINotEqual %bool %shr_33 %uint_0
               OpSelectionMerge %shr_38 None
               OpBranchConditional %shr_35 %shr_37 %shr_43
         %shr_37 = OpLabel
         %shr_39 = OpLoad %uint %shr_12
         %shr_40 = OpLoad %uint %shr_13
         %shr_41 = OpISub %uint %uint_32 %shr_40
         %shr_42 = OpShiftLeftLogical %uint %shr_39 %shr_41
               OpStore %shr_36 %shr_42
               OpBranch %shr_38
         %shr_43 = OpLabel
               OpStore %shr_36 %uint_0
               OpBranch %shr_38
         %shr_38 = OpLabel
        %shr_331 = OpPhi %uint %shr_42 %shr_37 %uint_0 %shr_43
         %shr_45 = OpBitwiseOr %uint %shr_32 %shr_331
               OpStore %shr_27 %shr_45
               OpBranch %shr_29
         %shr_46 = OpLabel
         %shr_47 = OpLoad %uint %shr_13
         %shr_49 = OpULessThan %bool %shr_47 %uint_64
               OpSelectionMerge %shr_52 None
               OpBranchConditional %shr_49 %shr_51 %shr_57
         %shr_51 = OpLabel
         %shr_53 = OpLoad %uint %shr_12
         %shr_54 = OpLoad %uint %shr_13
         %shr_55 = OpISub %uint %shr_54 %uint_32
         %shr_56 = OpShiftRightLogical %uint %shr_53 %shr_55
               OpStore %shr_50 %shr_56
               OpBranch %shr_52
         %shr_57 = OpLabel
               OpStore %shr_50 %uint_0
               OpBranch %shr_52
         %shr_52 = OpLabel
        %shr_330 = OpPhi %uint %shr_56 %shr_51 %uint_0 %shr_57
               OpStore %shr_27 %shr_330
               OpBranch %shr_29
         %shr_29 = OpLabel
        %shr_332 = OpPhi %uint %shr_45 %shr_38 %shr_330 %shr_52
               OpStore %shr_9 %shr_332
         %shr_60 = OpLoad %uint %shr_13
         %shr_61 = OpULessThan %bool %shr_60 %uint_32
               OpSelectionMerge %shr_64 None
               OpBranchConditional %shr_61 %shr_63 %shr_68
         %shr_63 = OpLabel
         %shr_65 = OpLoad %uint %shr_12
         %shr_66 = OpLoad %uint %shr_13
         %shr_67 = OpShiftRightLogical %uint %shr_65 %shr_66
               OpStore %shr_62 %shr_67
               OpBranch %shr_64
         %shr_68 = OpLabel
               OpStore %shr_62 %uint_0
               OpBranch %shr_64
         %shr_64 = OpLabel
        %shr_333 = OpPhi %uint %shr_67 %shr_63 %uint_0 %shr_68
               OpStore %shr_10 %shr_333
               OpReturn
               OpFunctionEnd
)";

constexpr char FUNC_SHIFT_LEFT[] = R"(
                    ; void shift_l(out uint d0, out uint d1, in uint s0, in uint s1, in uint n)
                    ; {
                    ; 	d0 = n < 32 ? s0 << n : 0;
                    ; 	d1 = n < 32 ? (n!=0 ? s0 >> (32-n) : 0) | (s1 << n) : (n < 64 ? s0 << (n-32) : 0);
                    ; }
%shift_left = OpFunction %void None %function_shift
         %shl_16 = OpFunctionParameter %_ptr_Function_uint
         %shl_17 = OpFunctionParameter %_ptr_Function_uint
         %shl_18 = OpFunctionParameter %_ptr_Function_uint
         %shl_19 = OpFunctionParameter %_ptr_Function_uint
         %shl_20 = OpFunctionParameter %_ptr_Function_uint
         %shl_22 = OpLabel
         %shl_72 = OpVariable %_ptr_Function_uint Function
         %shl_82 = OpVariable %_ptr_Function_uint Function
         %shl_87 = OpVariable %_ptr_Function_uint Function
        %shl_103 = OpVariable %_ptr_Function_uint Function
         %shl_70 = OpLoad %uint %shl_20
         %shl_71 = OpULessThan %bool %shl_70 %uint_32
               OpSelectionMerge %shl_74 None
               OpBranchConditional %shl_71 %shl_73 %shl_78
         %shl_73 = OpLabel
         %shl_75 = OpLoad %uint %shl_18
         %shl_76 = OpLoad %uint %shl_20
         %shl_77 = OpShiftLeftLogical %uint %shl_75 %shl_76
               OpStore %shl_72 %shl_77
               OpBranch %shl_74
         %shl_78 = OpLabel
               OpStore %shl_72 %uint_0
               OpBranch %shl_74
         %shl_74 = OpLabel
        %shl_334 = OpPhi %uint %shl_77 %shl_73 %uint_0 %shl_78
               OpStore %shl_16 %shl_334
         %shl_80 = OpLoad %uint %shl_20
         %shl_81 = OpULessThan %bool %shl_80 %uint_32
               OpSelectionMerge %shl_84 None
               OpBranchConditional %shl_81 %shl_83 %shl_100
         %shl_83 = OpLabel
         %shl_85 = OpLoad %uint %shl_20
         %shl_86 = OpINotEqual %bool %shl_85 %uint_0
               OpSelectionMerge %shl_89 None
               OpBranchConditional %shl_86 %shl_88 %shl_94
         %shl_88 = OpLabel
         %shl_90 = OpLoad %uint %shl_18
         %shl_91 = OpLoad %uint %shl_20
         %shl_92 = OpISub %uint %uint_32 %shl_91
         %shl_93 = OpShiftRightLogical %uint %shl_90 %shl_92
               OpStore %shl_87 %shl_93
               OpBranch %shl_89
         %shl_94 = OpLabel
               OpStore %shl_87 %uint_0
               OpBranch %shl_89
         %shl_89 = OpLabel
        %shl_336 = OpPhi %uint %shl_93 %shl_88 %uint_0 %shl_94
         %shl_96 = OpLoad %uint %shl_19
         %shl_97 = OpLoad %uint %shl_20
         %shl_98 = OpShiftLeftLogical %uint %shl_96 %shl_97
         %shl_99 = OpBitwiseOr %uint %shl_336 %shl_98
               OpStore %shl_82 %shl_99
               OpBranch %shl_84
        %shl_100 = OpLabel
        %shl_101 = OpLoad %uint %shl_20
        %shl_102 = OpULessThan %bool %shl_101 %uint_64
               OpSelectionMerge %shl_105 None
               OpBranchConditional %shl_102 %shl_104 %shl_110
        %shl_104 = OpLabel
        %shl_106 = OpLoad %uint %shl_18
        %shl_107 = OpLoad %uint %shl_20
        %shl_108 = OpISub %uint %shl_107 %uint_32
        %shl_109 = OpShiftLeftLogical %uint %shl_106 %shl_108
               OpStore %shl_103 %shl_109
               OpBranch %shl_105
        %shl_110 = OpLabel
               OpStore %shl_103 %uint_0
               OpBranch %shl_105
        %shl_105 = OpLabel
        %shl_335 = OpPhi %uint %shl_109 %shl_104 %uint_0 %shl_110
               OpStore %shl_82 %shl_335
               OpBranch %shl_84
         %shl_84 = OpLabel
        %shl_337 = OpPhi %uint %shl_99 %shl_89 %shl_335 %shl_105
               OpStore %shl_17 %shl_337
               OpReturn
               OpFunctionEnd
)";

constexpr char BUFFER_LOAD_FLOAT1[] = R"(
             ; void buffer_load_float1(out float p1, in int index, in int offset, in int stride, in int buffer_index)
             ; {
             ; 	int addr = (offset + index * stride)/4;
             ; 	p1 = buf[buffer_index].data[addr+0];
             ; }
%buffer_load_float1 = OpFunction %void None %function_buffer_load_store_float1
         %buf_l_f1_11 = OpFunctionParameter %_ptr_Function_float 
         %buf_l_f1_12 = OpFunctionParameter %_ptr_Function_int
         %buf_l_f1_13 = OpFunctionParameter %_ptr_Function_int
         %buf_l_f1_14 = OpFunctionParameter %_ptr_Function_int
         %buf_l_f1_15 = OpFunctionParameter %_ptr_Function_int
         %buf_l_f1_17 = OpLabel
         %buf_l_f1_42 = OpVariable %_ptr_Function_int Function
         %buf_l_f1_43 = OpLoad %int %buf_l_f1_13
         %buf_l_f1_44 = OpLoad %int %buf_l_f1_12
         %buf_l_f1_45 = OpLoad %int %buf_l_f1_14
         %buf_l_f1_46 = OpIMul %int %buf_l_f1_44 %buf_l_f1_45
         %buf_l_f1_47 = OpIAdd %int %buf_l_f1_43 %buf_l_f1_46
         %buf_l_f1_49 = OpSDiv %int %buf_l_f1_47 %int_4
               OpStore %buf_l_f1_42 %buf_l_f1_49
         %buf_l_f1_57 = OpLoad %int %buf_l_f1_15
         %buf_l_f1_62 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_l_f1_57 %int_0 %buf_l_f1_49
         %buf_l_f1_63 = OpLoad %float %buf_l_f1_62
               OpStore %buf_l_f1_11 %buf_l_f1_63
               OpReturn
               OpFunctionEnd
)";

constexpr char BUFFER_LOAD_SSCALED16_X[] = R"(
             ; Load the low signed 16-bit component and convert SSCALED to float.
%buffer_load_sscaled16_x = OpFunction %void None %function_buffer_load_store_float1
  %buf_l_s16_x_out = OpFunctionParameter %_ptr_Function_float
  %buf_l_s16_x_index_ptr = OpFunctionParameter %_ptr_Function_int
  %buf_l_s16_x_offset_ptr = OpFunctionParameter %_ptr_Function_int
  %buf_l_s16_x_stride_ptr = OpFunctionParameter %_ptr_Function_int
  %buf_l_s16_x_buffer_ptr = OpFunctionParameter %_ptr_Function_int
  %buf_l_s16_x_entry = OpLabel
  %buf_l_s16_x_offset = OpLoad %int %buf_l_s16_x_offset_ptr
  %buf_l_s16_x_index = OpLoad %int %buf_l_s16_x_index_ptr
  %buf_l_s16_x_stride = OpLoad %int %buf_l_s16_x_stride_ptr
  %buf_l_s16_x_record_offset = OpIMul %int %buf_l_s16_x_index %buf_l_s16_x_stride
  %buf_l_s16_x_byte_addr = OpIAdd %int %buf_l_s16_x_offset %buf_l_s16_x_record_offset
  %buf_l_s16_x_word_addr = OpSDiv %int %buf_l_s16_x_byte_addr %int_4
  %buf_l_s16_x_buffer = OpLoad %int %buf_l_s16_x_buffer_ptr
  %buf_l_s16_x_word_ptr = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_l_s16_x_buffer %int_0 %buf_l_s16_x_word_addr
  %buf_l_s16_x_word_float = OpLoad %float %buf_l_s16_x_word_ptr
  %buf_l_s16_x_word = OpBitcast %int %buf_l_s16_x_word_float
  %buf_l_s16_x_value = OpBitFieldSExtract %int %buf_l_s16_x_word %int_0 %int_16
  %buf_l_s16_x_float = OpConvertSToF %float %buf_l_s16_x_value
               OpStore %buf_l_s16_x_out %buf_l_s16_x_float
               OpReturn
               OpFunctionEnd
)";

constexpr char BUFFER_LOAD_FLOAT2[] = R"(
             ; void buffer_load_float2(out float p1, out float p2, in int index, in int offset, in int stride, in int buffer_index)
             ; {
             ; 	int addr = (offset + index * stride)/4;
             ; 	p1 = buf[buffer_index].data[addr+0];
             ; 	p2 = buf[buffer_index].data[addr+1];
             ; }
%buffer_load_float2 = OpFunction %void None %function_buffer_load_store_float2
  %buf_l_f2_21 = OpFunctionParameter %_ptr_Function_float
  %buf_l_f2_22 = OpFunctionParameter %_ptr_Function_float
  %buf_l_f2_23 = OpFunctionParameter %_ptr_Function_int
  %buf_l_f2_24 = OpFunctionParameter %_ptr_Function_int
  %buf_l_f2_25 = OpFunctionParameter %_ptr_Function_int
  %buf_l_f2_26 = OpFunctionParameter %_ptr_Function_int
  %buf_l_f2_28 = OpLabel
  %buf_l_f2_29 = OpLoad %int %buf_l_f2_24
  %buf_l_f2_30 = OpLoad %int %buf_l_f2_23
  %buf_l_f2_31 = OpLoad %int %buf_l_f2_25
  %buf_l_f2_32 = OpIMul %int %buf_l_f2_30 %buf_l_f2_31
  %buf_l_f2_33 = OpIAdd %int %buf_l_f2_29 %buf_l_f2_32
  %buf_l_f2_34 = OpSDiv %int %buf_l_f2_33 %int_4
  %buf_l_f2_35 = OpLoad %int %buf_l_f2_26
  %buf_l_f2_36 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_l_f2_35 %int_0 %buf_l_f2_34
  %buf_l_f2_37 = OpLoad %float %buf_l_f2_36
        OpStore %buf_l_f2_21 %buf_l_f2_37
  %buf_l_f2_38 = OpIAdd %int %buf_l_f2_34 %int_1
  %buf_l_f2_39 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_l_f2_35 %int_0 %buf_l_f2_38
  %buf_l_f2_40 = OpLoad %float %buf_l_f2_39
        OpStore %buf_l_f2_22 %buf_l_f2_40
        OpReturn
        OpFunctionEnd
)";

constexpr char BUFFER_LOAD_FLOAT4[] = R"(
             ; Function buffer_load_float4
             ;void buffer_load_float4(out float p1, out float p2, out float p3, out float p4, in int index, 
             ;                                in int offset, in int stride, in int buffer_index)
             ;{
             ;	int addr = (offset + index * stride)/4;
             ;	p1 = buf[buffer_index].data[addr+0];
             ;	p2 = buf[buffer_index].data[addr+1];
             ;	p3 = buf[buffer_index].data[addr+2];
             ;	p4 = buf[buffer_index].data[addr+3];
             ;}
%buffer_load_float4 = OpFunction %void None %function_buffer_load_float4
  %buf_l_f4_21 = OpFunctionParameter %_ptr_Function_float
  %buf_l_f4_22 = OpFunctionParameter %_ptr_Function_float
  %buf_l_f4_23 = OpFunctionParameter %_ptr_Function_float
  %buf_l_f4_24 = OpFunctionParameter %_ptr_Function_float
  %buf_l_f4_25 = OpFunctionParameter %_ptr_Function_int
  %buf_l_f4_26 = OpFunctionParameter %_ptr_Function_int
  %buf_l_f4_27 = OpFunctionParameter %_ptr_Function_int
  %buf_l_f4_28 = OpFunctionParameter %_ptr_Function_int
  %buf_l_f4_30 = OpLabel
  %buf_l_f4_44 = OpVariable %_ptr_Function_int Function
  %buf_l_f4_45 = OpLoad %int %buf_l_f4_26
  %buf_l_f4_46 = OpLoad %int %buf_l_f4_25
  %buf_l_f4_47 = OpLoad %int %buf_l_f4_27
  %buf_l_f4_48 = OpIMul %int %buf_l_f4_46 %buf_l_f4_47 
  %buf_l_f4_49 = OpIAdd %int %buf_l_f4_45 %buf_l_f4_48 
  %buf_l_f4_51 = OpSDiv %int %buf_l_f4_49 %int_4 
        OpStore %buf_l_f4_44 %buf_l_f4_51
  %buf_l_f4_58 = OpLoad %int %buf_l_f4_28
  %buf_l_f4_63 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_l_f4_58 %int_0 %buf_l_f4_51
  %buf_l_f4_64 = OpLoad %float %buf_l_f4_63 
        OpStore %buf_l_f4_21 %buf_l_f4_64
  %buf_l_f4_65 = OpLoad %int %buf_l_f4_28
  %buf_l_f4_68 = OpIAdd %int %buf_l_f4_51 %int_1
  %buf_l_f4_69 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_l_f4_65 %int_0 %buf_l_f4_68
  %buf_l_f4_70 = OpLoad %float %buf_l_f4_69
        OpStore %buf_l_f4_22 %buf_l_f4_70
  %buf_l_f4_71 = OpLoad %int %buf_l_f4_28
  %buf_l_f4_74 = OpIAdd %int %buf_l_f4_51 %int_2
  %buf_l_f4_75 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_l_f4_71 %int_0 %buf_l_f4_74
  %buf_l_f4_76 = OpLoad %float %buf_l_f4_75
        OpStore %buf_l_f4_23 %buf_l_f4_76
  %buf_l_f4_77 = OpLoad %int %buf_l_f4_28
  %buf_l_f4_80 = OpIAdd %int %buf_l_f4_51 %int_3
  %buf_l_f4_81 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_l_f4_77 %int_0 %buf_l_f4_80
  %buf_l_f4_82 = OpLoad %float %buf_l_f4_81
        OpStore %buf_l_f4_24 %buf_l_f4_82
        OpReturn
        OpFunctionEnd 
			)";

constexpr char BUFFER_LOAD_SSCALED16_XYZW[] = R"(
             ; Load four packed signed 16-bit components and convert SSCALED to float.
%buffer_load_sscaled16_xyzw = OpFunction %void None %function_buffer_load_float4
  %buf_l_s16_xyzw_out_x = OpFunctionParameter %_ptr_Function_float
  %buf_l_s16_xyzw_out_y = OpFunctionParameter %_ptr_Function_float
  %buf_l_s16_xyzw_out_z = OpFunctionParameter %_ptr_Function_float
  %buf_l_s16_xyzw_out_w = OpFunctionParameter %_ptr_Function_float
  %buf_l_s16_xyzw_index_ptr = OpFunctionParameter %_ptr_Function_int
  %buf_l_s16_xyzw_offset_ptr = OpFunctionParameter %_ptr_Function_int
  %buf_l_s16_xyzw_stride_ptr = OpFunctionParameter %_ptr_Function_int
  %buf_l_s16_xyzw_buffer_ptr = OpFunctionParameter %_ptr_Function_int
  %buf_l_s16_xyzw_entry = OpLabel
  %buf_l_s16_xyzw_offset = OpLoad %int %buf_l_s16_xyzw_offset_ptr
  %buf_l_s16_xyzw_index = OpLoad %int %buf_l_s16_xyzw_index_ptr
  %buf_l_s16_xyzw_stride = OpLoad %int %buf_l_s16_xyzw_stride_ptr
  %buf_l_s16_xyzw_record_offset = OpIMul %int %buf_l_s16_xyzw_index %buf_l_s16_xyzw_stride
  %buf_l_s16_xyzw_byte_addr = OpIAdd %int %buf_l_s16_xyzw_offset %buf_l_s16_xyzw_record_offset
  %buf_l_s16_xyzw_word_addr_0 = OpSDiv %int %buf_l_s16_xyzw_byte_addr %int_4
  %buf_l_s16_xyzw_word_addr_1 = OpIAdd %int %buf_l_s16_xyzw_word_addr_0 %int_1
  %buf_l_s16_xyzw_buffer = OpLoad %int %buf_l_s16_xyzw_buffer_ptr
  %buf_l_s16_xyzw_word_ptr_0 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_l_s16_xyzw_buffer %int_0 %buf_l_s16_xyzw_word_addr_0
  %buf_l_s16_xyzw_word_float_0 = OpLoad %float %buf_l_s16_xyzw_word_ptr_0
  %buf_l_s16_xyzw_word_0 = OpBitcast %int %buf_l_s16_xyzw_word_float_0
  %buf_l_s16_xyzw_word_ptr_1 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_l_s16_xyzw_buffer %int_0 %buf_l_s16_xyzw_word_addr_1
  %buf_l_s16_xyzw_word_float_1 = OpLoad %float %buf_l_s16_xyzw_word_ptr_1
  %buf_l_s16_xyzw_word_1 = OpBitcast %int %buf_l_s16_xyzw_word_float_1
  %buf_l_s16_xyzw_x = OpBitFieldSExtract %int %buf_l_s16_xyzw_word_0 %int_0 %int_16
  %buf_l_s16_xyzw_y = OpBitFieldSExtract %int %buf_l_s16_xyzw_word_0 %int_16 %int_16
  %buf_l_s16_xyzw_z = OpBitFieldSExtract %int %buf_l_s16_xyzw_word_1 %int_0 %int_16
  %buf_l_s16_xyzw_w = OpBitFieldSExtract %int %buf_l_s16_xyzw_word_1 %int_16 %int_16
  %buf_l_s16_xyzw_x_float = OpConvertSToF %float %buf_l_s16_xyzw_x
  %buf_l_s16_xyzw_y_float = OpConvertSToF %float %buf_l_s16_xyzw_y
  %buf_l_s16_xyzw_z_float = OpConvertSToF %float %buf_l_s16_xyzw_z
  %buf_l_s16_xyzw_w_float = OpConvertSToF %float %buf_l_s16_xyzw_w
               OpStore %buf_l_s16_xyzw_out_x %buf_l_s16_xyzw_x_float
               OpStore %buf_l_s16_xyzw_out_y %buf_l_s16_xyzw_y_float
               OpStore %buf_l_s16_xyzw_out_z %buf_l_s16_xyzw_z_float
               OpStore %buf_l_s16_xyzw_out_w %buf_l_s16_xyzw_w_float
               OpReturn
               OpFunctionEnd
)";

constexpr char BUFFER_STORE_FLOAT1[] = R"(
             ; void buffer_store_float1(in float p1, in int index, in int offset, in int stride, in int buffer_index)
             ; {
             ; 	int addr = (offset + index * stride)/4;
             ; 	buf[buffer_index].data[addr+0] = p1;
             ; }
%buffer_store_float1 = OpFunction %void None %function_buffer_load_store_float1
         %buf_s_f1_18 = OpFunctionParameter %_ptr_Function_float
         %buf_s_f1_19 = OpFunctionParameter %_ptr_Function_int 
         %buf_s_f1_20 = OpFunctionParameter %_ptr_Function_int 
         %buf_s_f1_21 = OpFunctionParameter %_ptr_Function_int 
         %buf_s_f1_22 = OpFunctionParameter %_ptr_Function_int 
         %buf_s_f1_24 = OpLabel
         %buf_s_f1_64 = OpVariable %_ptr_Function_int Function 
         %buf_s_f1_65 = OpLoad %int %buf_s_f1_20
         %buf_s_f1_66 = OpLoad %int %buf_s_f1_19
         %buf_s_f1_67 = OpLoad %int %buf_s_f1_21
         %buf_s_f1_68 = OpIMul %int %buf_s_f1_66 %buf_s_f1_67
         %buf_s_f1_69 = OpIAdd %int %buf_s_f1_65 %buf_s_f1_68
         %buf_s_f1_70 = OpSDiv %int %buf_s_f1_69 %int_4
               OpStore %buf_s_f1_64 %buf_s_f1_70
         %buf_s_f1_71 = OpLoad %int %buf_s_f1_22
         %buf_s_f1_74 = OpLoad %float %buf_s_f1_18
         %buf_s_f1_75 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_s_f1_71 %int_0 %buf_s_f1_70
               OpStore %buf_s_f1_75 %buf_s_f1_74
               OpReturn
               OpFunctionEnd
)";

constexpr char BUFFER_STORE_FLOAT2[] = R"(
                      ; void buffer_store_float2(in float p1, in float p2, in int index, in int offset, in int stride, in int buffer_index)
                      ; {
                      ; 	int addr = (offset + index * stride)/4;
                      ; 	buf[buffer_index].data[addr+0] = p1;
                      ; 	buf[buffer_index].data[addr+1] = p2;
                      ; }
%buffer_store_float2 = OpFunction %void None %function_buffer_load_store_float2
         %buf_s_f2_51 = OpFunctionParameter %_ptr_Function_float
         %buf_s_f2_52 = OpFunctionParameter %_ptr_Function_float
         %buf_s_f2_53 = OpFunctionParameter %_ptr_Function_int
         %buf_s_f2_54 = OpFunctionParameter %_ptr_Function_int
         %buf_s_f2_55 = OpFunctionParameter %_ptr_Function_int
         %buf_s_f2_56 = OpFunctionParameter %_ptr_Function_int
         %buf_s_f2_58 = OpLabel
        %buf_s_f2_143 = OpVariable %_ptr_Function_int Function
        %buf_s_f2_144 = OpLoad %int %buf_s_f2_54
        %buf_s_f2_145 = OpLoad %int %buf_s_f2_53
        %buf_s_f2_146 = OpLoad %int %buf_s_f2_55
        %buf_s_f2_147 = OpIMul %int %buf_s_f2_145 %buf_s_f2_146
        %buf_s_f2_148 = OpIAdd %int %buf_s_f2_144 %buf_s_f2_147
        %buf_s_f2_149 = OpSDiv %int %buf_s_f2_148 %int_4
               OpStore %buf_s_f2_143 %buf_s_f2_149
        %buf_s_f2_150 = OpLoad %int %buf_s_f2_56
        %buf_s_f2_153 = OpLoad %float %buf_s_f2_51
        %buf_s_f2_154 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_s_f2_150 %int_0 %buf_s_f2_149
               OpStore %buf_s_f2_154 %buf_s_f2_153
        %buf_s_f2_155 = OpLoad %int %buf_s_f2_56
        %buf_s_f2_158 = OpIAdd %int %buf_s_f2_149 %int_1
        %buf_s_f2_159 = OpLoad %float %buf_s_f2_52
        %buf_s_f2_160 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_s_f2_155 %int_0 %buf_s_f2_158
               OpStore %buf_s_f2_160 %buf_s_f2_159
               OpReturn
               OpFunctionEnd
)";

constexpr char TBUFFER_LOAD_FORMAT_XYZW[] = R"(
             ; Function tbuffer_load_format_xyzw
             ; void tbuffer_load_format_xyzw(out float p1, out float p2, out float p3, out float p4, 
             ;                               in int index, in int offset, in int stride, in int buffer_index, in int dfmt_nfmt)
             ; {
             ; 	if (dfmt_nfmt == 119) // dfmt = 14, nfmt = 7
             ; 	{
             ; 		buffer_load_float4(p1, p2, p3, p4, index, offset, stride, buffer_index);
             ; 	}
             ; }
%tbuffer_load_format_xyzw = OpFunction %void None %function_tbuffer_load_format_xyzw
%tbuf_l_f_xyzw_54 = OpFunctionParameter %_ptr_Function_float
%tbuf_l_f_xyzw_55 = OpFunctionParameter %_ptr_Function_float
%tbuf_l_f_xyzw_56 = OpFunctionParameter %_ptr_Function_float
%tbuf_l_f_xyzw_57 = OpFunctionParameter %_ptr_Function_float
%tbuf_l_f_xyzw_58 = OpFunctionParameter %_ptr_Function_int
%tbuf_l_f_xyzw_59 = OpFunctionParameter %_ptr_Function_int
%tbuf_l_f_xyzw_60 = OpFunctionParameter %_ptr_Function_int
%tbuf_l_f_xyzw_61 = OpFunctionParameter %_ptr_Function_int
%tbuf_l_f_xyzw_62 = OpFunctionParameter %_ptr_Function_int
%tbuf_l_f_xyzw_64 = OpLabel
%tbuf_l_f_xyzw_166 = OpVariable %_ptr_Function_float Function
%tbuf_l_f_xyzw_167 = OpVariable %_ptr_Function_float Function
%tbuf_l_f_xyzw_168 = OpVariable %_ptr_Function_float Function
%tbuf_l_f_xyzw_169 = OpVariable %_ptr_Function_float Function
%tbuf_l_f_xyzw_170 = OpVariable %_ptr_Function_int Function
%tbuf_l_f_xyzw_172 = OpVariable %_ptr_Function_int Function
%tbuf_l_f_xyzw_174 = OpVariable %_ptr_Function_int Function
%tbuf_l_f_xyzw_176 = OpVariable %_ptr_Function_int Function
%tbuf_l_f_xyzw_161 = OpLoad %int %tbuf_l_f_xyzw_62
%tbuf_l_f_xyzw_163 = OpIEqual %bool %tbuf_l_f_xyzw_161 %int_119
   OpSelectionMerge %tbuf_l_f_xyzw_165 None
   OpBranchConditional %tbuf_l_f_xyzw_163 %tbuf_l_f_xyzw_164 %tbuf_l_f_xyzw_165
%tbuf_l_f_xyzw_164 = OpLabel
%tbuf_l_f_xyzw_171 = OpLoad %int %tbuf_l_f_xyzw_58
   OpStore %tbuf_l_f_xyzw_170 %tbuf_l_f_xyzw_171
%tbuf_l_f_xyzw_173 = OpLoad %int %tbuf_l_f_xyzw_59
   OpStore %tbuf_l_f_xyzw_172 %tbuf_l_f_xyzw_173
%tbuf_l_f_xyzw_175 = OpLoad %int %tbuf_l_f_xyzw_60
   OpStore %tbuf_l_f_xyzw_174 %tbuf_l_f_xyzw_175
%tbuf_l_f_xyzw_177 = OpLoad %int %tbuf_l_f_xyzw_61
   OpStore %tbuf_l_f_xyzw_176 %tbuf_l_f_xyzw_177
%tbuf_l_f_xyzw_178 = OpFunctionCall %void %buffer_load_float4 %tbuf_l_f_xyzw_166 %tbuf_l_f_xyzw_167 %tbuf_l_f_xyzw_168 %tbuf_l_f_xyzw_169 %tbuf_l_f_xyzw_170 %tbuf_l_f_xyzw_172 %tbuf_l_f_xyzw_174 %tbuf_l_f_xyzw_176
%tbuf_l_f_xyzw_179 = OpLoad %float %tbuf_l_f_xyzw_166
   OpStore %tbuf_l_f_xyzw_54 %tbuf_l_f_xyzw_179
%tbuf_l_f_xyzw_180 = OpLoad %float %tbuf_l_f_xyzw_167
   OpStore %tbuf_l_f_xyzw_55 %tbuf_l_f_xyzw_180
%tbuf_l_f_xyzw_181 = OpLoad %float %tbuf_l_f_xyzw_168
   OpStore %tbuf_l_f_xyzw_56 %tbuf_l_f_xyzw_181
%tbuf_l_f_xyzw_182 = OpLoad %float %tbuf_l_f_xyzw_169
   OpStore %tbuf_l_f_xyzw_57 %tbuf_l_f_xyzw_182
   OpBranch %tbuf_l_f_xyzw_165
%tbuf_l_f_xyzw_165 = OpLabel
   OpReturn
   OpFunctionEnd
			)";

constexpr char TBUFFER_LOAD_FORMAT_X[] = R"(
             ; void tbuffer_load_format_x(out float p1, in int index, in int offset, in int stride, in int buffer_index, in int dfmt_nfmt)
             ; {
             ; 	// Existing profiles expose a raw 32-bit first component. Key 99 is packed SINT16 SSCALED.
             ; }
%tbuffer_load_format_x = OpFunction %void None %function_tbuffer_load_store_format_x
         %tbuf_l_f_x_26 = OpFunctionParameter %_ptr_Function_float
         %tbuf_l_f_x_27 = OpFunctionParameter %_ptr_Function_int 
         %tbuf_l_f_x_28 = OpFunctionParameter %_ptr_Function_int 
         %tbuf_l_f_x_29 = OpFunctionParameter %_ptr_Function_int 
         %tbuf_l_f_x_30 = OpFunctionParameter %_ptr_Function_int 
         %tbuf_l_f_x_31 = OpFunctionParameter %_ptr_Function_int 
         %tbuf_l_f_x_33 = OpLabel 
         %tbuf_l_f_x_82 = OpVariable %_ptr_Function_float Function
         %tbuf_l_f_x_83 = OpVariable %_ptr_Function_int Function
         %tbuf_l_f_x_85 = OpVariable %_ptr_Function_int Function
         %tbuf_l_f_x_87 = OpVariable %_ptr_Function_int Function
         %tbuf_l_f_x_89 = OpVariable %_ptr_Function_int Function
         %tbuf_l_f_x_76 = OpLoad %int %tbuf_l_f_x_31
         %tbuf_l_f_x_84 = OpLoad %int %tbuf_l_f_x_27
               OpStore %tbuf_l_f_x_83 %tbuf_l_f_x_84
         %tbuf_l_f_x_86 = OpLoad %int %tbuf_l_f_x_28
               OpStore %tbuf_l_f_x_85 %tbuf_l_f_x_86
         %tbuf_l_f_x_88 = OpLoad %int %tbuf_l_f_x_29
               OpStore %tbuf_l_f_x_87 %tbuf_l_f_x_88
         %tbuf_l_f_x_90 = OpLoad %int %tbuf_l_f_x_30
               OpStore %tbuf_l_f_x_89 %tbuf_l_f_x_90
               OpSelectionMerge %tbuf_l_f_x_81 None
               OpSwitch %tbuf_l_f_x_76 %tbuf_l_f_x_81 36 %tbuf_l_f_x_raw 39 %tbuf_l_f_x_raw 92 %tbuf_l_f_x_raw 95 %tbuf_l_f_x_raw 119 %tbuf_l_f_x_raw 99 %tbuf_l_f_x_sscaled16
         %tbuf_l_f_x_raw = OpLabel
         %tbuf_l_f_x_91 = OpFunctionCall %void %buffer_load_float1 %tbuf_l_f_x_82 %tbuf_l_f_x_83 %tbuf_l_f_x_85 %tbuf_l_f_x_87 %tbuf_l_f_x_89
         %tbuf_l_f_x_92 = OpLoad %float %tbuf_l_f_x_82
               OpStore %tbuf_l_f_x_26 %tbuf_l_f_x_92
               OpBranch %tbuf_l_f_x_81
         %tbuf_l_f_x_sscaled16 = OpLabel
         %tbuf_l_f_x_93 = OpFunctionCall %void %buffer_load_sscaled16_x %tbuf_l_f_x_82 %tbuf_l_f_x_83 %tbuf_l_f_x_85 %tbuf_l_f_x_87 %tbuf_l_f_x_89
         %tbuf_l_f_x_94 = OpLoad %float %tbuf_l_f_x_82
               OpStore %tbuf_l_f_x_26 %tbuf_l_f_x_94
               OpBranch %tbuf_l_f_x_81
         %tbuf_l_f_x_81 = OpLabel
               OpReturn
               OpFunctionEnd
)";

constexpr char TBUFFER_STORE_FORMAT_X[] = R"(
             ; void tbuffer_store_format_x(in float p1, in int index, in int offset, in int stride, in int buffer_index, in int dfmt_nfmt)
             ; {
             ; 	if (dfmt_nfmt == 36 || dfmt_nfmt == 39) // dmft = 4, nfmt = 4 or 7
             ; 	{
             ; 		buffer_store_float1(p1, index, offset, stride, buffer_index);
             ; 	}
             ; }
%tbuffer_store_format_x = OpFunction %void None %function_tbuffer_load_store_format_x
         %tbuf_s_f_x_34 = OpFunctionParameter %_ptr_Function_float 
         %tbuf_s_f_x_35 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_x_36 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_x_37 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_x_38 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_x_39 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_x_41 = OpLabel
         %tbuf_s_f_x_97 = OpVariable %_ptr_Function_float Function
         %tbuf_s_f_x_99 = OpVariable %_ptr_Function_int Function
        %tbuf_s_f_x_101 = OpVariable %_ptr_Function_int Function
        %tbuf_s_f_x_103 = OpVariable %_ptr_Function_int Function
        %tbuf_s_f_x_105 = OpVariable %_ptr_Function_int Function
         %tbuf_s_f_x_93 = OpLoad %int %tbuf_s_f_x_39
         %tbuf_s_f_x_94 = OpIEqual %bool %tbuf_s_f_x_93 %int_36
         %tbuf_s_f_x_94_2 = OpIEqual %bool %tbuf_s_f_x_93 %int_39
         %tbuf_s_f_x_94_3 = OpLogicalOr %bool %tbuf_s_f_x_94 %tbuf_s_f_x_94_2
               OpSelectionMerge %tbuf_s_f_x_96 None
               OpBranchConditional %tbuf_s_f_x_94_3 %tbuf_s_f_x_95 %tbuf_s_f_x_96
         %tbuf_s_f_x_95 = OpLabel
         %tbuf_s_f_x_98 = OpLoad %float %tbuf_s_f_x_34
               OpStore %tbuf_s_f_x_97 %tbuf_s_f_x_98
        %tbuf_s_f_x_100 = OpLoad %int %tbuf_s_f_x_35
               OpStore %tbuf_s_f_x_99 %tbuf_s_f_x_100
        %tbuf_s_f_x_102 = OpLoad %int %tbuf_s_f_x_36
               OpStore %tbuf_s_f_x_101 %tbuf_s_f_x_102
        %tbuf_s_f_x_104 = OpLoad %int %tbuf_s_f_x_37
               OpStore %tbuf_s_f_x_103 %tbuf_s_f_x_104
        %tbuf_s_f_x_106 = OpLoad %int %tbuf_s_f_x_38
               OpStore %tbuf_s_f_x_105 %tbuf_s_f_x_106
        %tbuf_s_f_x_107 = OpFunctionCall %void %buffer_store_float1 %tbuf_s_f_x_97 %tbuf_s_f_x_99 %tbuf_s_f_x_101 %tbuf_s_f_x_103 %tbuf_s_f_x_105
               OpBranch %tbuf_s_f_x_96
         %tbuf_s_f_x_96 = OpLabel
               OpReturn
               OpFunctionEnd
)";

constexpr char TBUFFER_STORE_FORMAT_XY[] = R"(
                        ; void tbuffer_store_format_xy(in float p1, in float p2, in int index, in int offset, in int stride, in int buffer_index, in int dfmt_nfmt)
                        ; {
                        ; 	if (dfmt_nfmt == 92 || dfmt_nfmt == 95) // dmft = 11, nfmt = 4 or 7
                        ; 	{
                        ; 		buffer_store_float2(p1, p2, index, offset, stride, buffer_index);
                        ; 	}
                        ; }
%tbuffer_store_format_xy = OpFunction %void None %function_tbuffer_load_store_format_xy
         %tbuf_s_f_xy_60 = OpFunctionParameter %_ptr_Function_float
         %tbuf_s_f_xy_61 = OpFunctionParameter %_ptr_Function_float
         %tbuf_s_f_xy_62 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_xy_63 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_xy_64 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_xy_65 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_xy_66 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_xy_68 = OpLabel
        %tbuf_s_f_xy_170 = OpVariable %_ptr_Function_float Function
        %tbuf_s_f_xy_172 = OpVariable %_ptr_Function_float Function
        %tbuf_s_f_xy_174 = OpVariable %_ptr_Function_int Function
        %tbuf_s_f_xy_176 = OpVariable %_ptr_Function_int Function
        %tbuf_s_f_xy_178 = OpVariable %_ptr_Function_int Function
        %tbuf_s_f_xy_180 = OpVariable %_ptr_Function_int Function
        %tbuf_s_f_xy_161 = OpLoad %int %tbuf_s_f_xy_66
        %tbuf_s_f_xy_163 = OpIEqual %bool %tbuf_s_f_xy_161 %int_92
        %tbuf_s_f_xy_164 = OpLoad %int %tbuf_s_f_xy_66
        %tbuf_s_f_xy_166 = OpIEqual %bool %tbuf_s_f_xy_164 %int_95
        %tbuf_s_f_xy_167 = OpLogicalOr %bool %tbuf_s_f_xy_163 %tbuf_s_f_xy_166
               OpSelectionMerge %tbuf_s_f_xy_169 None
               OpBranchConditional %tbuf_s_f_xy_167 %tbuf_s_f_xy_168 %tbuf_s_f_xy_169
        %tbuf_s_f_xy_168 = OpLabel
        %tbuf_s_f_xy_171 = OpLoad %float %tbuf_s_f_xy_60
               OpStore %tbuf_s_f_xy_170 %tbuf_s_f_xy_171
        %tbuf_s_f_xy_173 = OpLoad %float %tbuf_s_f_xy_61
               OpStore %tbuf_s_f_xy_172 %tbuf_s_f_xy_173
        %tbuf_s_f_xy_175 = OpLoad %int %tbuf_s_f_xy_62
               OpStore %tbuf_s_f_xy_174 %tbuf_s_f_xy_175
        %tbuf_s_f_xy_177 = OpLoad %int %tbuf_s_f_xy_63
               OpStore %tbuf_s_f_xy_176 %tbuf_s_f_xy_177
        %tbuf_s_f_xy_179 = OpLoad %int %tbuf_s_f_xy_64
               OpStore %tbuf_s_f_xy_178 %tbuf_s_f_xy_179
        %tbuf_s_f_xy_181 = OpLoad %int %tbuf_s_f_xy_65
               OpStore %tbuf_s_f_xy_180 %tbuf_s_f_xy_181
        %tbuf_s_f_xy_182 = OpFunctionCall %void %buffer_store_float2 %tbuf_s_f_xy_170 %tbuf_s_f_xy_172 %tbuf_s_f_xy_174 %tbuf_s_f_xy_176 %tbuf_s_f_xy_178 %tbuf_s_f_xy_180
               OpBranch %tbuf_s_f_xy_169
        %tbuf_s_f_xy_169 = OpLabel
               OpReturn
               OpFunctionEnd
)";

constexpr char SBUFFER_LOAD_DWORD[] = R"(
                     ; void sbuffer_load_dword(out uint p1, in int offset, in int buffer_index)
                     ; {
                     ; 	int addr = offset/4;
                     ; 	p1 = floatBitsToUint(buf[buffer_index].data[addr+0]);
                     ; }
%sbuffer_load_dword = OpFunction %void None %function_sbuffer_load_dword
         %sbuf_dw_45 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw_46 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw_47 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw_49 = OpLabel
        %sbuf_dw_115 = OpVariable %_ptr_Function_int Function
        %sbuf_dw_116 = OpLoad %int %sbuf_dw_46
        %sbuf_dw_117 = OpSDiv %int %sbuf_dw_116 %int_4
               OpStore %sbuf_dw_115 %sbuf_dw_117
        %sbuf_dw_118 = OpLoad %int %sbuf_dw_47
        %sbuf_dw_121 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw_118 %int_0 %sbuf_dw_117
        %sbuf_dw_122 = OpLoad %float %sbuf_dw_121
        %sbuf_dw_123 = OpBitcast %uint %sbuf_dw_122
               OpStore %sbuf_dw_45 %sbuf_dw_123
               OpReturn
               OpFunctionEnd
)";

constexpr char SBUFFER_LOAD_DWORD_2[] = R"(
                      ; void sbuffer_load_dwordx2(out uint p1, out uint p2, in int offset, in int buffer_index)
                      ; {
                      ; 	int addr = offset/4;
                      ; 	p1 = floatBitsToUint(buf[buffer_index].data[addr+0]);
                      ; 	p2 = floatBitsToUint(buf[buffer_index].data[addr+1]);
                      ; }
%sbuffer_load_dword_2 = OpFunction %void None %function_sbuffer_load_dword_2
         %sbuf_dw2_11 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw2_12 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw2_13 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw2_14 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw2_16 = OpLabel
         %sbuf_dw2_17 = OpVariable %_ptr_Function_int Function
         %sbuf_dw2_18 = OpLoad %int %sbuf_dw2_13
         %sbuf_dw2_20 = OpSDiv %int %sbuf_dw2_18 %int_4
               OpStore %sbuf_dw2_17 %sbuf_dw2_20
         %sbuf_dw2_28 = OpLoad %int %sbuf_dw2_14
         %sbuf_dw2_33 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw2_28 %int_0 %sbuf_dw2_20
         %sbuf_dw2_34 = OpLoad %float %sbuf_dw2_33
         %sbuf_dw2_35 = OpBitcast %uint %sbuf_dw2_34
               OpStore %sbuf_dw2_11 %sbuf_dw2_35
         %sbuf_dw2_36 = OpLoad %int %sbuf_dw2_14
         %sbuf_dw2_39 = OpIAdd %int %sbuf_dw2_20 %int_1
         %sbuf_dw2_40 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw2_36 %int_0 %sbuf_dw2_39
         %sbuf_dw2_41 = OpLoad %float %sbuf_dw2_40
         %sbuf_dw2_42 = OpBitcast %uint %sbuf_dw2_41
               OpStore %sbuf_dw2_12 %sbuf_dw2_42
               OpReturn
               OpFunctionEnd
)";

constexpr char SBUFFER_LOAD_DWORD_4[] = R"(
                     ; void sbuffer_load_dwordx4(out uint p1, out uint p2, out uint p3, out uint p4, in int offset, in int buffer_index)
                     ; {
                     ; 	int addr = offset/4;
                     ; 	p1 = floatBitsToUint(buf[buffer_index].data[addr+0]);
                     ; 	p2 = floatBitsToUint(buf[buffer_index].data[addr+1]);
                     ; 	p3 = floatBitsToUint(buf[buffer_index].data[addr+2]);
                     ; 	p4 = floatBitsToUint(buf[buffer_index].data[addr+3]);
                     ; }
%sbuffer_load_dword_4 = OpFunction %void None %function_sbuffer_load_dword_4
         %sbuf_dw4_51 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw4_52 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw4_53 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw4_54 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw4_55 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw4_56 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw4_58 = OpLabel
        %sbuf_dw4_133 = OpVariable %_ptr_Function_int Function
        %sbuf_dw4_134 = OpLoad %int %sbuf_dw4_55
        %sbuf_dw4_135 = OpSDiv %int %sbuf_dw4_134 %int_4
               OpStore %sbuf_dw4_133 %sbuf_dw4_135
        %sbuf_dw4_136 = OpLoad %int %sbuf_dw4_56
        %sbuf_dw4_139 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw4_136 %int_0 %sbuf_dw4_135
        %sbuf_dw4_140 = OpLoad %float %sbuf_dw4_139
        %sbuf_dw4_141 = OpBitcast %uint %sbuf_dw4_140
               OpStore %sbuf_dw4_51 %sbuf_dw4_141
        %sbuf_dw4_142 = OpLoad %int %sbuf_dw4_56
        %sbuf_dw4_145 = OpIAdd %int %sbuf_dw4_135 %int_1
        %sbuf_dw4_146 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw4_142 %int_0 %sbuf_dw4_145
        %sbuf_dw4_147 = OpLoad %float %sbuf_dw4_146
        %sbuf_dw4_148 = OpBitcast %uint %sbuf_dw4_147
               OpStore %sbuf_dw4_52 %sbuf_dw4_148
        %sbuf_dw4_149 = OpLoad %int %sbuf_dw4_56
        %sbuf_dw4_152 = OpIAdd %int %sbuf_dw4_135 %int_2
        %sbuf_dw4_153 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw4_149 %int_0 %sbuf_dw4_152
        %sbuf_dw4_154 = OpLoad %float %sbuf_dw4_153
        %sbuf_dw4_155 = OpBitcast %uint %sbuf_dw4_154
               OpStore %sbuf_dw4_53 %sbuf_dw4_155
        %sbuf_dw4_156 = OpLoad %int %sbuf_dw4_56
        %sbuf_dw4_159 = OpIAdd %int %sbuf_dw4_135 %int_3
        %sbuf_dw4_160 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw4_156 %int_0 %sbuf_dw4_159
        %sbuf_dw4_161 = OpLoad %float %sbuf_dw4_160
        %sbuf_dw4_162 = OpBitcast %uint %sbuf_dw4_161
               OpStore %sbuf_dw4_54 %sbuf_dw4_162
               OpReturn
               OpFunctionEnd
)";

constexpr char SBUFFER_LOAD_DWORD_8[] = R"(
                     ; void sbuffer_load_dwordx8(out uint p1, out uint p2, out uint p3, out uint p4, 
                     ;                           out uint p5, out uint p6, out uint p7, out uint p8, in int offset, in int buffer_index)
                     ; {
                     ; 	int addr = offset/4;
                     ; 	p1 = floatBitsToUint(buf[buffer_index].data[addr+0]);
                     ; 	p2 = floatBitsToUint(buf[buffer_index].data[addr+1]);
                     ; 	p3 = floatBitsToUint(buf[buffer_index].data[addr+2]);
                     ; 	p4 = floatBitsToUint(buf[buffer_index].data[addr+3]);
                     ; 	p5 = floatBitsToUint(buf[buffer_index].data[addr+4]);
                     ; 	p6 = floatBitsToUint(buf[buffer_index].data[addr+5]);
                     ; 	p7 = floatBitsToUint(buf[buffer_index].data[addr+6]);
                     ; 	p8 = floatBitsToUint(buf[buffer_index].data[addr+7]);
                     ; }
%sbuffer_load_dword_8 = OpFunction %void None %function_sbuffer_load_dword_8
         %sbuf_dw8_60 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw8_61 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw8_62 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw8_63 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw8_64 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw8_65 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw8_66 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw8_67 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw8_68 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw8_69 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw8_71 = OpLabel
        %sbuf_dw8_197 = OpVariable %_ptr_Function_int Function
        %sbuf_dw8_198 = OpLoad %int %sbuf_dw8_68
        %sbuf_dw8_199 = OpSDiv %int %sbuf_dw8_198 %int_4
               OpStore %sbuf_dw8_197 %sbuf_dw8_199
        %sbuf_dw8_200 = OpLoad %int %sbuf_dw8_69
        %sbuf_dw8_203 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw8_200 %int_0 %sbuf_dw8_199
        %sbuf_dw8_204 = OpLoad %float %sbuf_dw8_203
        %sbuf_dw8_205 = OpBitcast %uint %sbuf_dw8_204
               OpStore %sbuf_dw8_60 %sbuf_dw8_205
        %sbuf_dw8_206 = OpLoad %int %sbuf_dw8_69
        %sbuf_dw8_208 = OpIAdd %int %sbuf_dw8_199 %int_1
        %sbuf_dw8_209 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw8_206 %int_0 %sbuf_dw8_208
        %sbuf_dw8_210 = OpLoad %float %sbuf_dw8_209
        %sbuf_dw8_211 = OpBitcast %uint %sbuf_dw8_210
               OpStore %sbuf_dw8_61 %sbuf_dw8_211
        %sbuf_dw8_212 = OpLoad %int %sbuf_dw8_69
        %sbuf_dw8_214 = OpIAdd %int %sbuf_dw8_199 %int_2
        %sbuf_dw8_215 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw8_212 %int_0 %sbuf_dw8_214
        %sbuf_dw8_216 = OpLoad %float %sbuf_dw8_215
        %sbuf_dw8_217 = OpBitcast %uint %sbuf_dw8_216
               OpStore %sbuf_dw8_62 %sbuf_dw8_217
        %sbuf_dw8_218 = OpLoad %int %sbuf_dw8_69
        %sbuf_dw8_220 = OpIAdd %int %sbuf_dw8_199 %int_3
        %sbuf_dw8_221 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw8_218 %int_0 %sbuf_dw8_220
        %sbuf_dw8_222 = OpLoad %float %sbuf_dw8_221
        %sbuf_dw8_223 = OpBitcast %uint %sbuf_dw8_222
               OpStore %sbuf_dw8_63 %sbuf_dw8_223
        %sbuf_dw8_224 = OpLoad %int %sbuf_dw8_69
        %sbuf_dw8_226 = OpIAdd %int %sbuf_dw8_199 %int_4
        %sbuf_dw8_227 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw8_224 %int_0 %sbuf_dw8_226
        %sbuf_dw8_228 = OpLoad %float %sbuf_dw8_227
        %sbuf_dw8_229 = OpBitcast %uint %sbuf_dw8_228
               OpStore %sbuf_dw8_64 %sbuf_dw8_229
        %sbuf_dw8_230 = OpLoad %int %sbuf_dw8_69
        %sbuf_dw8_233 = OpIAdd %int %sbuf_dw8_199 %int_5
        %sbuf_dw8_234 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw8_230 %int_0 %sbuf_dw8_233
        %sbuf_dw8_235 = OpLoad %float %sbuf_dw8_234
        %sbuf_dw8_236 = OpBitcast %uint %sbuf_dw8_235
               OpStore %sbuf_dw8_65 %sbuf_dw8_236
        %sbuf_dw8_237 = OpLoad %int %sbuf_dw8_69
        %sbuf_dw8_240 = OpIAdd %int %sbuf_dw8_199 %int_6
        %sbuf_dw8_241 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw8_237 %int_0 %sbuf_dw8_240
        %sbuf_dw8_242 = OpLoad %float %sbuf_dw8_241
        %sbuf_dw8_243 = OpBitcast %uint %sbuf_dw8_242
               OpStore %sbuf_dw8_66 %sbuf_dw8_243
        %sbuf_dw8_244 = OpLoad %int %sbuf_dw8_69
        %sbuf_dw8_247 = OpIAdd %int %sbuf_dw8_199 %int_7
        %sbuf_dw8_248 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw8_244 %int_0 %sbuf_dw8_247
        %sbuf_dw8_249 = OpLoad %float %sbuf_dw8_248
        %sbuf_dw8_250 = OpBitcast %uint %sbuf_dw8_249
               OpStore %sbuf_dw8_67 %sbuf_dw8_250
               OpReturn
               OpFunctionEnd
)";

constexpr char SBUFFER_LOAD_DWORD_16[] = R"(
                     ; void sbuffer_load_dwordx16(out uint p1, out uint p2, out uint p3, out uint p4, 
                     ;                            out uint p5, out uint p6, out uint p7, out uint p8,
                     ;                            out uint p9, out uint p10, out uint p11, out uint p12,
                     ;                            out uint p13, out uint p14, out uint p15, out uint p16, in int offset, in int buffer_index)
                     ; {
                     ; 	int addr = offset/4;
                     ; 	p1 = floatBitsToUint(buf[buffer_index].data[addr+0]);
                     ; 	p2 = floatBitsToUint(buf[buffer_index].data[addr+1]);
                     ; 	p3 = floatBitsToUint(buf[buffer_index].data[addr+2]);
                     ; 	p4 = floatBitsToUint(buf[buffer_index].data[addr+3]);
                     ; 	p5 = floatBitsToUint(buf[buffer_index].data[addr+4]);
                     ; 	p6 = floatBitsToUint(buf[buffer_index].data[addr+5]);
                     ; 	p7 = floatBitsToUint(buf[buffer_index].data[addr+6]);
                     ; 	p8 = floatBitsToUint(buf[buffer_index].data[addr+7]);
                     ; 	p9 = floatBitsToUint(buf[buffer_index].data[addr+8]);
                     ; 	p10 = floatBitsToUint(buf[buffer_index].data[addr+9]);
                     ; 	p11 = floatBitsToUint(buf[buffer_index].data[addr+10]);
                     ; 	p12 = floatBitsToUint(buf[buffer_index].data[addr+11]);
                     ; 	p13 = floatBitsToUint(buf[buffer_index].data[addr+12]);
                     ; 	p14 = floatBitsToUint(buf[buffer_index].data[addr+13]);
                     ; 	p15 = floatBitsToUint(buf[buffer_index].data[addr+14]);
                     ; 	p16 = floatBitsToUint(buf[buffer_index].data[addr+15]);
                     ; }
%sbuffer_load_dword_16 = OpFunction %void None %function_sbuffer_load_dword_16
         %sbuf_dw16_60 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_61 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_62 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_63 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_64 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_65 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_66 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_67 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_68 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_69 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_70 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_71 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_72 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_73 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_74 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_75 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_76 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw16_77 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw16_79 = OpLabel
        %sbuf_dw16_184 = OpVariable %_ptr_Function_int Function
        %sbuf_dw16_185 = OpLoad %int %sbuf_dw16_76
        %sbuf_dw16_186 = OpSDiv %int %sbuf_dw16_185 %int_4
               OpStore %sbuf_dw16_184 %sbuf_dw16_186
        %sbuf_dw16_187 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_190 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_187 %int_0 %sbuf_dw16_186
        %sbuf_dw16_191 = OpLoad %float %sbuf_dw16_190
        %sbuf_dw16_192 = OpBitcast %uint %sbuf_dw16_191
               OpStore %sbuf_dw16_60 %sbuf_dw16_192
        %sbuf_dw16_193 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_195 = OpIAdd %int %sbuf_dw16_186 %int_1
        %sbuf_dw16_196 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_193 %int_0 %sbuf_dw16_195
        %sbuf_dw16_197 = OpLoad %float %sbuf_dw16_196
        %sbuf_dw16_198 = OpBitcast %uint %sbuf_dw16_197
               OpStore %sbuf_dw16_61 %sbuf_dw16_198
        %sbuf_dw16_199 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_201 = OpIAdd %int %sbuf_dw16_186 %int_2
        %sbuf_dw16_202 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_199 %int_0 %sbuf_dw16_201
        %sbuf_dw16_203 = OpLoad %float %sbuf_dw16_202
        %sbuf_dw16_204 = OpBitcast %uint %sbuf_dw16_203
               OpStore %sbuf_dw16_62 %sbuf_dw16_204
        %sbuf_dw16_205 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_207 = OpIAdd %int %sbuf_dw16_186 %int_3
        %sbuf_dw16_208 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_205 %int_0 %sbuf_dw16_207
        %sbuf_dw16_209 = OpLoad %float %sbuf_dw16_208
        %sbuf_dw16_210 = OpBitcast %uint %sbuf_dw16_209
               OpStore %sbuf_dw16_63 %sbuf_dw16_210
        %sbuf_dw16_211 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_213 = OpIAdd %int %sbuf_dw16_186 %int_4
        %sbuf_dw16_214 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_211 %int_0 %sbuf_dw16_213
        %sbuf_dw16_215 = OpLoad %float %sbuf_dw16_214
        %sbuf_dw16_216 = OpBitcast %uint %sbuf_dw16_215
               OpStore %sbuf_dw16_64 %sbuf_dw16_216
        %sbuf_dw16_217 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_220 = OpIAdd %int %sbuf_dw16_186 %int_5
        %sbuf_dw16_221 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_217 %int_0 %sbuf_dw16_220
        %sbuf_dw16_222 = OpLoad %float %sbuf_dw16_221
        %sbuf_dw16_223 = OpBitcast %uint %sbuf_dw16_222
               OpStore %sbuf_dw16_65 %sbuf_dw16_223
        %sbuf_dw16_224 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_227 = OpIAdd %int %sbuf_dw16_186 %int_6
        %sbuf_dw16_228 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_224 %int_0 %sbuf_dw16_227
        %sbuf_dw16_229 = OpLoad %float %sbuf_dw16_228
        %sbuf_dw16_230 = OpBitcast %uint %sbuf_dw16_229
               OpStore %sbuf_dw16_66 %sbuf_dw16_230
        %sbuf_dw16_231 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_234 = OpIAdd %int %sbuf_dw16_186 %int_7
        %sbuf_dw16_235 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_231 %int_0 %sbuf_dw16_234
        %sbuf_dw16_236 = OpLoad %float %sbuf_dw16_235
        %sbuf_dw16_237 = OpBitcast %uint %sbuf_dw16_236
               OpStore %sbuf_dw16_67 %sbuf_dw16_237
        %sbuf_dw16_238 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_241 = OpIAdd %int %sbuf_dw16_186 %int_8
        %sbuf_dw16_242 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_238 %int_0 %sbuf_dw16_241
        %sbuf_dw16_243 = OpLoad %float %sbuf_dw16_242
        %sbuf_dw16_244 = OpBitcast %uint %sbuf_dw16_243
               OpStore %sbuf_dw16_68 %sbuf_dw16_244
        %sbuf_dw16_245 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_248 = OpIAdd %int %sbuf_dw16_186 %int_9
        %sbuf_dw16_249 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_245 %int_0 %sbuf_dw16_248
        %sbuf_dw16_250 = OpLoad %float %sbuf_dw16_249
        %sbuf_dw16_251 = OpBitcast %uint %sbuf_dw16_250
               OpStore %sbuf_dw16_69 %sbuf_dw16_251
        %sbuf_dw16_252 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_255 = OpIAdd %int %sbuf_dw16_186 %int_10
        %sbuf_dw16_256 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_252 %int_0 %sbuf_dw16_255
        %sbuf_dw16_257 = OpLoad %float %sbuf_dw16_256
        %sbuf_dw16_258 = OpBitcast %uint %sbuf_dw16_257
               OpStore %sbuf_dw16_70 %sbuf_dw16_258
        %sbuf_dw16_259 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_262 = OpIAdd %int %sbuf_dw16_186 %int_11
        %sbuf_dw16_263 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_259 %int_0 %sbuf_dw16_262
        %sbuf_dw16_264 = OpLoad %float %sbuf_dw16_263
        %sbuf_dw16_265 = OpBitcast %uint %sbuf_dw16_264
               OpStore %sbuf_dw16_71 %sbuf_dw16_265
        %sbuf_dw16_266 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_269 = OpIAdd %int %sbuf_dw16_186 %int_12
        %sbuf_dw16_270 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_266 %int_0 %sbuf_dw16_269
        %sbuf_dw16_271 = OpLoad %float %sbuf_dw16_270
        %sbuf_dw16_272 = OpBitcast %uint %sbuf_dw16_271
               OpStore %sbuf_dw16_72 %sbuf_dw16_272
        %sbuf_dw16_273 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_276 = OpIAdd %int %sbuf_dw16_186 %int_13
        %sbuf_dw16_277 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_273 %int_0 %sbuf_dw16_276
        %sbuf_dw16_278 = OpLoad %float %sbuf_dw16_277
        %sbuf_dw16_279 = OpBitcast %uint %sbuf_dw16_278
               OpStore %sbuf_dw16_73 %sbuf_dw16_279
        %sbuf_dw16_280 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_283 = OpIAdd %int %sbuf_dw16_186 %int_14
        %sbuf_dw16_284 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_280 %int_0 %sbuf_dw16_283
        %sbuf_dw16_285 = OpLoad %float %sbuf_dw16_284
        %sbuf_dw16_286 = OpBitcast %uint %sbuf_dw16_285
               OpStore %sbuf_dw16_74 %sbuf_dw16_286
        %sbuf_dw16_287 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_290 = OpIAdd %int %sbuf_dw16_186 %int_15
        %sbuf_dw16_291 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_287 %int_0 %sbuf_dw16_290
        %sbuf_dw16_292 = OpLoad %float %sbuf_dw16_291
        %sbuf_dw16_293 = OpBitcast %uint %sbuf_dw16_292
               OpStore %sbuf_dw16_75 %sbuf_dw16_293
               OpReturn
               OpFunctionEnd
)";

constexpr char EMBEDDED_SHADER_VS_0[] = R"(
               ; #version 450
               ;
               ; void main() 
               ; {
               ; 	float x = gl_VertexIndex == 0 || gl_VertexIndex == 2 ? 1.0 : -1.0;
               ; 	float y = gl_VertexIndex == 2 || gl_VertexIndex == 3 ? -1.0 : 1.0;
               ;
               ;     gl_Position = vec4(x,y, 0.0, 1.0);
               ; }

               OpCapability Shader
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint Vertex %4 "main" %gl_VertexIndex %43

               ; Annotations
               OpDecorate %gl_VertexIndex BuiltIn VertexIndex
               OpMemberDecorate %_struct_41 0 BuiltIn Position
               OpMemberDecorate %_struct_41 1 BuiltIn PointSize
               OpMemberDecorate %_struct_41 2 BuiltIn ClipDistance
               OpMemberDecorate %_struct_41 3 BuiltIn CullDistance
               OpDecorate %_struct_41 Block

               ; Types, variables and constants
       %void = OpTypeVoid
          %3 = OpTypeFunction %void
      %float = OpTypeFloat 32
%_ptr_Function_float = OpTypePointer Function %float
       %bool = OpTypeBool
        %int = OpTypeInt 32 1
%_ptr_Input_int = OpTypePointer Input %int
%gl_VertexIndex = OpVariable %_ptr_Input_int Input
      %int_0 = OpConstant %int 0
      %int_2 = OpConstant %int 2
    %float_1 = OpConstant %float 1
   %float_n1 = OpConstant %float -1
      %int_3 = OpConstant %int 3
    %v4float = OpTypeVector %float 4
       %uint = OpTypeInt 32 0
     %uint_1 = OpConstant %uint 1
%_arr_float_uint_1 = OpTypeArray %float %uint_1
 %_struct_41 = OpTypeStruct %v4float %float %_arr_float_uint_1 %_arr_float_uint_1
%_ptr_Output__struct_41 = OpTypePointer Output %_struct_41
         %43 = OpVariable %_ptr_Output__struct_41 Output
    %float_0 = OpConstant %float 0
%_ptr_Output_v4float = OpTypePointer Output %v4float

               ; Function 4
          %4 = OpFunction %void None %3
          %5 = OpLabel
          %8 = OpVariable %_ptr_Function_float Function
         %26 = OpVariable %_ptr_Function_float Function
         %13 = OpLoad %int %gl_VertexIndex
         %15 = OpIEqual %bool %13 %int_0
         %16 = OpLogicalNot %bool %15
               OpSelectionMerge %18 None
               OpBranchConditional %16 %17 %18
         %17 = OpLabel
         %21 = OpIEqual %bool %13 %int_2
               OpBranch %18
         %18 = OpLabel
         %22 = OpPhi %bool %15 %5 %21 %17
         %25 = OpSelect %float %22 %float_1 %float_n1
               OpStore %8 %25
         %28 = OpIEqual %bool %13 %int_2
         %29 = OpLogicalNot %bool %28
               OpSelectionMerge %31 None
               OpBranchConditional %29 %30 %31
         %30 = OpLabel
         %34 = OpIEqual %bool %13 %int_3
               OpBranch %31
         %31 = OpLabel
         %35 = OpPhi %bool %28 %18 %34 %30
         %36 = OpSelect %float %35 %float_n1 %float_1
               OpStore %26 %36
         %47 = OpCompositeConstruct %v4float %25 %36 %float_0 %float_1
         %49 = OpAccessChain %_ptr_Output_v4float %43 %int_0
               OpStore %49 %47
               OpReturn
               OpFunctionEnd
)";

constexpr char EMBEDDED_SHADER_PS_0[] = R"(
               ; #version 450
               ;
               ; layout(location = 0) out vec4 outColor;
               ;
               ; void main() {
               ; 	outColor = vec4(0);
               ; }

               OpCapability Shader
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %4 "main" %9
               OpExecutionMode %4 OriginUpperLeft

               ; Annotations
               OpDecorate %9 Location 0

               ; Types, variables and constants
       %void = OpTypeVoid
          %3 = OpTypeFunction %void
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
%_ptr_Output_v4float = OpTypePointer Output %v4float
          %9 = OpVariable %_ptr_Output_v4float Output
    %float_0 = OpConstant %float 0
         %11 = OpConstantComposite %v4float %float_0 %float_0 %float_0 %float_0

               ; Function 4
          %4 = OpFunction %void None %3
          %5 = OpLabel
               OpStore %9 %11
               OpReturn
               OpFunctionEnd
)";

constexpr char EXECZ[] = R"(
        %z191_<index> = OpLoad %uint %exec_lo
        %z192_<index> = OpIEqual %bool %z191_<index> %uint_0
        %z193_<index> = OpLoad %uint %exec_hi
        %z194_<index> = OpIEqual %bool %z193_<index> %uint_0
        %z195_<index> = OpLogicalAnd %bool %z192_<index> %z194_<index>
        %z196_<index> = OpSelect %uint %z195_<index> %uint_1 %uint_0
               OpStore %execz %z196_<index>
)";

constexpr char SCC_NZ_1[] = R"(
        %snz1_118_<index> = OpLoad %uint %<dst>
        %snz1_121_<index> = OpINotEqual %bool %snz1_118_<index> %uint_0
        %snz1_123_<index> = OpSelect %uint %snz1_121_<index> %uint_1 %uint_0
               OpStore %scc %snz1_123_<index>
)";

constexpr char SCC_NZ_2[] = R"(
        %snz2_124_<index> = OpLoad %uint %<dst0>
        %snz2_125_<index> = OpINotEqual %bool %snz2_124_<index> %uint_0
        %snz2_127_<index> = OpLoad %uint %<dst1>
        %snz2_128_<index> = OpINotEqual %bool %snz2_127_<index> %uint_0
        %snz2_129_<index> = OpLogicalOr %bool %snz2_125_<index> %snz2_128_<index>
        %snz2_130_<index> = OpSelect %uint %snz2_129_<index> %uint_1 %uint_0
               OpStore %scc %snz2_130_<index>
)";

constexpr char SCC_OVERFLOW_ADD_1[] = R"(
        %so1_124_<index> = OpExtInst %int %GLSL_std_450 SSign %t0_<index>
        %so1_127_<index> = OpExtInst %int %GLSL_std_450 SSign %t1_<index>
        %so1_129_<index> = OpLoad %uint %<dst>
        %so1_130_<index> = OpBitcast %int %so1_129_<index>
        %so1_131_<index> = OpExtInst %int %GLSL_std_450 SSign %so1_130_<index>
        %so1_135_<index> = OpIEqual %bool %so1_124_<index> %so1_127_<index>
        %so1_138_<index> = OpINotEqual %bool %so1_131_<index> %so1_124_<index>
        %so1_139_<index> = OpLogicalAnd %bool %so1_135_<index> %so1_138_<index>
        %so1_142_<index> = OpSelect %uint %so1_139_<index> %uint_1 %uint_0
               OpStore %scc %so1_142_<index>
)";

constexpr char SCC_OVERFLOW_SUB_1[] = R"(
        %so1_124_<index> = OpExtInst %int %GLSL_std_450 SSign %t0_<index>
        %so1_127_<index> = OpExtInst %int %GLSL_std_450 SSign %t1_<index>
        %so1_129_<index> = OpLoad %uint %<dst>
        %so1_130_<index> = OpBitcast %int %so1_129_<index>
        %so1_131_<index> = OpExtInst %int %GLSL_std_450 SSign %so1_130_<index>
        %so1_135_<index> = OpINotEqual %bool %so1_124_<index> %so1_127_<index>
        %so1_138_<index> = OpINotEqual %bool %so1_131_<index> %so1_124_<index>
        %so1_139_<index> = OpLogicalAnd %bool %so1_135_<index> %so1_138_<index>
        %so1_142_<index> = OpSelect %uint %so1_139_<index> %uint_1 %uint_0
               OpStore %scc %so1_142_<index>
)";

constexpr char SCC_CARRY_1[] = R"(
        OpStore %scc %carry_<index>
)";

constexpr char CLAMP[] = R"(
		%c197_<index> = OpLoad %float %<dst>
        %c200_<index> = OpExtInst %float %GLSL_std_450 FClamp %c197_<index> %float_0_000000 %float_1_000000
               OpStore %<dst> %c200_<index>
)";

constexpr char MULTIPLY[] = R"(
		%m197_<index> = OpLoad %float %<dst>
        %m200_<index> = OpFMul %float %m197_<index> %<mul>
               OpStore %<dst> %m200_<index>
)";

class Spirv;

enum class SccCheck
{
	None,
	NonZero,
	OverflowAdd,
	OverflowSub,
	CarryOut,
};

using inst_recompile_func_t = bool (*)(LUMEN_RECOMPILER_ARGS);

enum class SpirvType
{
	Unknown,
	Float,
	Int,
	Uint
};

struct SpirvValue
{
	SpirvType type = SpirvType::Unknown;
	String8   value;
};

class ShaderExtendedMapping
{
public:
	static constexpr int SIZE = ShaderExtendedResources::DATA_DWORDS_MAX;

	void Reset()
	{
		for (auto& entry: m_entries)
		{
			entry = {};
		}
	}

	[[nodiscard]] bool TryMap(int offset, int buffer, int field)
	{
		if (offset < 0 || offset >= SIZE || m_entries[offset].valid)
		{
			return false;
		}
		m_entries[offset] = {buffer, field, true};
		return true;
	}

	[[nodiscard]] bool TryResolve(int offset, int* buffer, int* field) const
	{
		if (buffer == nullptr || field == nullptr || offset < 0 || offset >= SIZE || !m_entries[offset].valid)
		{
			return false;
		}
		*buffer = m_entries[offset].buffer;
		*field  = m_entries[offset].field;
		return true;
	}

private:
	struct Entry
	{
		int  buffer = 0;
		int  field  = 0;
		bool valid  = false;
	};

	Entry m_entries[SIZE] = {};
};

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
static bool ShaderVertexResourceContractTest();
bool ShaderConstantStorageContractTest()
{
	ShaderBindResources vs {}, ps {};
	vs.push_constant_size = 160;
	ps.push_constant_offset = 160;
	ps.push_constant_size = 96;
	if (!ShaderSelectConstantStorage(&vs, &ps, 256, 16384, 4) || vs.constant_storage != ShaderConstantStorage::PushConstants) return false;
	ps.push_constant_size = 112;
	if (!ShaderSelectConstantStorage(&vs, &ps, 256, 272, 3) || vs.constant_storage != ShaderConstantStorage::UniformBuffer ||
	    ps.constant_storage != ShaderConstantStorage::UniformBuffer) return false;
	if (ShaderSelectConstantStorage(&vs, &ps, 256, 271, 4) || ShaderSelectConstantStorage(&vs, &ps, 256, 272, 2)) return false;
	ps.push_constant_offset = 144;
	if (ShaderSelectConstantStorage(&vs, &ps, 256, 16384, 4)) return false;
	ps.push_constant_offset = UINT32_MAX - 15;
	if (ShaderSelectConstantStorage(&vs, &ps, 256, UINT32_MAX, 4)) return false;
	ps.push_constant_offset = 161;
	if (ShaderSelectConstantStorage(&vs, &ps, 256, 16384, 4)) return false;
	ps.push_constant_offset = 160;
	for (const int invalid: {-1, ShaderStorageResources::BUFFERS_MAX + 1})
	{
		vs.storage_buffers.buffers_num = invalid;
		if (ShaderSelectConstantStorage(&vs, &ps, 256, 16384, 4)) return false;
	}
	vs.storage_buffers.buffers_num = 0;
	ps.push_constant_size = 0;
	if (!ShaderSelectConstantStorage(&vs, &ps, 128, 16384, 4) || vs.constant_storage != ShaderConstantStorage::UniformBuffer) return false;
	vs.push_constant_size = 0;
	if (!ShaderSelectConstantStorage(&vs, &ps, 128, 16384, 4) || vs.constant_storage != ShaderConstantStorage::PushConstants) return false;
	vs.push_constant_size = ShaderBindResources::CONSTANT_DATA_MAX + 16;
	if (ShaderSelectConstantStorage(&vs, nullptr, 256, 16384, 4)) return false;

	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	validator.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position, const char* message) {
		std::fprintf(stderr, "constant UBO SPIR-V validation at %zu: %s\n", position.index, message);
	});
	for (const auto stage: {ShaderType::Vertex, ShaderType::Pixel, ShaderType::Compute})
	{
		// Two ordinary scalar moves precede END, matching its production
		// look-behind contract in all three stages. Owned padding is not ISA.
		const uint32_t isa[9] {0xbe9f03ffu, 0x3f800000u, 0xbea003ffu, 0x40000000u, 0xbf810000u};
		ShaderCode code;
		code.SetType(stage);
		if (ShaderTryParseBounded(isa, 9, &code) != 5 || code.GetInstructions().Size() != 3) return false;
		ShaderVertexInputInfo vertex {};
		ShaderPixelInputInfo pixel {};
		ShaderComputeInputInfo compute {};
		compute.threads_num[0] = compute.threads_num[1] = compute.threads_num[2] = 1;
		auto& bind = stage == ShaderType::Vertex ? vertex.bind : stage == ShaderType::Pixel ? pixel.bind : compute.bind;
		bind.constant_storage = ShaderConstantStorage::UniformBuffer;
		bind.direct_sgprs.sgprs_num = 1;
		bind.direct_sgprs.start_register[0] = 0;
		bind.push_constant_size = stage == ShaderType::Vertex ? 160 : 112;
		bind.push_constant_offset = stage == ShaderType::Pixel ? 160 : 0;
		if (stage == ShaderType::Compute)
		{
			// 34 records exercise constant indices beyond the old 32-record cap.
			bind.storage_buffers.buffers_num = 16;
			bind.samplers.samplers_num = 16;
			bind.gds_pointers.pointers_num = 1;
			for (int index = 0; index < 16; ++index)
			{
				bind.storage_buffers.extended[index] = true;
				bind.storage_buffers.start_register[index] = 16 + index * 4;
				bind.samplers.start_register[index] = index * 4;
			}
			bind.gds_pointers.start_register[0] = 64;
			bind.direct_sgprs.start_register[0] = 65;
			ShaderCalcBindingIndices(&bind);
			if (!ShaderSelectConstantStorage(&bind, nullptr, 256, 16384, 4)) return false;
		}
		const auto source = SpirvGenerateSource(code, stage == ShaderType::Vertex ? &vertex : nullptr,
		    stage == ShaderType::Pixel ? &pixel : nullptr, stage == ShaderType::Compute ? &compute : nullptr);
		if (!source.ContainsStr("OpTypeArray %v4uint") || !source.ContainsStr("OpTypePointer Uniform %uint") ||
		    source.ContainsStr("OpTypePointer PushConstant") || !source.ContainsStr("OpDecorate %vsharp DescriptorSet 2") ||
		    (stage == ShaderType::Pixel && !source.ContainsStr("OpMemberDecorate %BufferResource 0 Offset 160"))) return false;
		std::vector<uint32_t> module;
		if (!validator.Assemble(source.c_str(), &module) || !validator.Validate(module)) return false;
	}
	std::fprintf(stderr, "[GFX_CONSTANT_STORAGE_TEST] boundary256=push span272=ubo stages=3 std140=vec4 offset160=preserved overflow=closed\n");
	return true;
}

bool ShaderExtendedMappingContractTest()
{
	ShaderExtendedMapping mapping;
	int                   buffer = -1;
	int                   field  = -1;

	if (mapping.TryResolve(0, &buffer, &field) || mapping.TryResolve(7, &buffer, &field) || !mapping.TryMap(8, 3, 2) ||
	    !mapping.TryResolve(8, &buffer, &field) || buffer != 3 || field != 2 || mapping.TryMap(8, 4, 0) || mapping.TryMap(-1, 0, 0) ||
	    mapping.TryMap(ShaderExtendedMapping::SIZE, 0, 0) || mapping.TryResolve(-1, &buffer, &field) ||
	    mapping.TryResolve(ShaderExtendedMapping::SIZE, &buffer, &field) || mapping.TryResolve(8, nullptr, &field) ||
	    mapping.TryResolve(8, &buffer, nullptr))
	{
		return false;
	}

	mapping.Reset();
	return !mapping.TryResolve(8, &buffer, &field) && mapping.TryMap(8, 5, 1) && mapping.TryResolve(8, &buffer, &field) && buffer == 5 &&
	       field == 1 && ShaderVertexResourceContractTest();
}
#endif

class Spirv
{
public:
	Spirv()          = default;
	virtual ~Spirv() = default;
	LUMEN_CLASS_DEFAULT_COPY(Spirv);

	[[nodiscard]] const ShaderCode& GetCode() const { return m_code; }
	ShaderCode&                     GetCode() { return m_code; }
	void                            SetCode(const ShaderCode& m_code) { this->m_code = m_code; }

	void GenerateSource();

	[[nodiscard]] const String8& GetSource() const { return m_source; }

	void                                       SetVsInputInfo(const ShaderVertexInputInfo* input_info) { m_vs_input_info = input_info; }
	[[nodiscard]] const ShaderVertexInputInfo* GetVsInputInfo() const { return m_vs_input_info; }

	void                                        SetCsInputInfo(const ShaderComputeInputInfo* input_info) { m_cs_input_info = input_info; }
	[[nodiscard]] const ShaderComputeInputInfo* GetCsInputInfo() const { return m_cs_input_info; }

	void                                      SetPsInputInfo(const ShaderPixelInputInfo* input_info) { m_ps_input_info = input_info; }
	[[nodiscard]] const ShaderPixelInputInfo* GetPsInputInfo() const { return m_ps_input_info; }

	[[nodiscard]] const ShaderBindResources* GetBindInfo() const { return m_bind; }
	//[[nodiscard]] const ShaderBindParameters& GetBindParams() const { return m_bind_params; }

	void                  AddConstantUint(uint32_t u);
	void                  AddConstantInt(int i);
	void                  AddConstantFloat(float f);
	void                  AddConstant(ShaderOperand op);
	[[nodiscard]] String8 GetConstantUint(uint32_t u) const;
	[[nodiscard]] String8 GetConstantInt(int i) const;
	[[nodiscard]] String8 GetConstantFloat(float f) const;
	[[nodiscard]] String8 GetConstant(ShaderOperand op) const;

	void GetMappedIndex(int offset, int* buffer, int* field) const
	{
		EXIT_NOT_IMPLEMENTED(!m_extended_mapping.TryResolve(offset, buffer, field));
	}

private:
	struct Variable
	{
		ShaderOperand op;
	};

	struct Constant
	{
		SpirvType      type     = SpirvType::Unknown;
		ShaderConstant constant = {0};
		String8        type_str;
		String8        value_str;
		String8        id;
	};

	void AddConstant(SpirvType type, ShaderConstant constant);
	void AddVariable(ShaderOperand op);
	void AddVariable(ShaderOperandType type, int register_id, int size);

	void WriteHeader();
	void WriteDebug();
	void WriteAnnotations();
	void WriteTypes();
	void WriteConstants();
	void WriteGlobalVariables();
	void WriteMainProlog();
	void WriteLocalVariables();
	void WriteInstructions();
	void WriteMainEpilog();
	void WriteFunctions();
	void WriteLabel(int index, bool& block_terminated);

	void FindConstants();
	void FindVariables();

	void ModifyCode();

	String8                       m_source;
	ShaderCode                    m_code;
	Vector<Constant>              m_constants;
	Vector<Variable>              m_variables;
	const ShaderVertexInputInfo*  m_vs_input_info = nullptr;
	const ShaderComputeInputInfo* m_cs_input_info = nullptr;
	const ShaderPixelInputInfo*   m_ps_input_info = nullptr;
	const ShaderBindResources*    m_bind          = nullptr;
	// ShaderBindParameters          m_bind_params;

	ShaderExtendedMapping m_extended_mapping;
};

// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct RecompilerFunc
{
	inst_recompile_func_t           func      = nullptr;
	ShaderInstructionType           type      = ShaderInstructionType::Unknown;
	ShaderInstructionFormat::Format format    = ShaderInstructionFormat::Unknown;
	const char*                     param[4]  = {nullptr, nullptr, nullptr, nullptr};
	SccCheck                        scc_check = SccCheck::None;
};

static bool operand_is_constant(ShaderOperand op)
{
	return (op.type == ShaderOperandType::LiteralConstant || op.type == ShaderOperandType::IntegerInlineConstant ||
	        op.type == ShaderOperandType::FloatInlineConstant);
}

// Only instructions whose local lowering has no externally visible writes are
// replayable. New instruction kinds must be reviewed before entering this list.
static bool RectListInstructionIsPure(const ShaderCode& code, const ShaderInstruction& inst)
{
	switch (inst.type)
	{
		case ShaderInstructionType::STTraceData: return ShaderThreadTraceDataInstructionSupported(code, inst);
		case ShaderInstructionType::VCvtOffF32I4: return ShaderCvtOffF32I4InstructionSupported(inst);
		case ShaderInstructionType::BufferLoadDword:
		case ShaderInstructionType::BufferLoadFormatX:
		case ShaderInstructionType::BufferLoadFormatXy:
		case ShaderInstructionType::BufferLoadFormatXyz:
		case ShaderInstructionType::BufferLoadFormatXyzw:
		case ShaderInstructionType::Exp:
		case ShaderInstructionType::SAddcU32:
		case ShaderInstructionType::SAddI32:
		case ShaderInstructionType::SAddU32:
		case ShaderInstructionType::SAndB32:
		case ShaderInstructionType::SAndB64:
		case ShaderInstructionType::SAndn2B64:
		case ShaderInstructionType::SAndSaveexecB64:
		case ShaderInstructionType::SBfeU32:
		case ShaderInstructionType::SBfeU64:
		case ShaderInstructionType::SBfmB32:
		case ShaderInstructionType::SBufferLoadDword:
		case ShaderInstructionType::SBufferLoadDwordx16:
		case ShaderInstructionType::SBufferLoadDwordx2:
		case ShaderInstructionType::SBufferLoadDwordx4:
		case ShaderInstructionType::SBufferLoadDwordx8:
		case ShaderInstructionType::SCbranchExecz:
		case ShaderInstructionType::SCbranchScc0:
		case ShaderInstructionType::SCbranchScc1:
		case ShaderInstructionType::SCbranchVccz:
		case ShaderInstructionType::SCbranchVccnz:
		case ShaderInstructionType::SCmpEqI32:
		case ShaderInstructionType::SCmpEqU32:
		case ShaderInstructionType::SCmpGeI32:
		case ShaderInstructionType::SCmpGeU32:
		case ShaderInstructionType::SCmpGtI32:
		case ShaderInstructionType::SCmpGtU32:
		case ShaderInstructionType::SCmpLeI32:
		case ShaderInstructionType::SCmpLeU32:
		case ShaderInstructionType::SCmpLgI32:
		case ShaderInstructionType::SCmpLgU32:
		case ShaderInstructionType::SCmpLtI32:
		case ShaderInstructionType::SCmpLtU32:
		case ShaderInstructionType::SCselectB32:
		case ShaderInstructionType::SCselectB64:
		case ShaderInstructionType::SEndpgm:
		case ShaderInstructionType::SLshlB32:
		case ShaderInstructionType::SLshrB32:
		case ShaderInstructionType::SLshlB64:
		case ShaderInstructionType::SLshrB64:
		case ShaderInstructionType::SMovB32:
		case ShaderInstructionType::SMovB64:
		case ShaderInstructionType::SMovkI32:
		case ShaderInstructionType::SMulHiU32:
		case ShaderInstructionType::SMulI32:
		case ShaderInstructionType::SMulkI32:
		case ShaderInstructionType::SNandB64:
		case ShaderInstructionType::SNotB32:
		case ShaderInstructionType::SNorB64:
		case ShaderInstructionType::SOrB32:
		case ShaderInstructionType::SOrB64:
		case ShaderInstructionType::SOrn2B64:
		case ShaderInstructionType::SSubI32:
		case ShaderInstructionType::SWaitcnt:
		case ShaderInstructionType::SWqmB64:
		case ShaderInstructionType::SXnorB64:
		case ShaderInstructionType::SXorB64:
		case ShaderInstructionType::TBufferLoadFormatX:
		case ShaderInstructionType::TBufferLoadFormatXyzw:
		case ShaderInstructionType::VAddF32:
		case ShaderInstructionType::VAddI32:
		case ShaderInstructionType::VAndB32:
		case ShaderInstructionType::VAshrI32:
		case ShaderInstructionType::VAshrrevI32:
		case ShaderInstructionType::VBcntU32B32:
		case ShaderInstructionType::VBfeU32:
		case ShaderInstructionType::VBfmB32:
		case ShaderInstructionType::VBfrevB32:
		case ShaderInstructionType::VCeilF32:
		case ShaderInstructionType::VCmpEqF32:
		case ShaderInstructionType::VCmpEqI32:
		case ShaderInstructionType::VCmpEqU32:
		case ShaderInstructionType::VCmpFF32:
		case ShaderInstructionType::VCmpFI32:
		case ShaderInstructionType::VCmpFU32:
		case ShaderInstructionType::VCmpGeF32:
		case ShaderInstructionType::VCmpGeI32:
		case ShaderInstructionType::VCmpGeU32:
		case ShaderInstructionType::VCmpGtF32:
		case ShaderInstructionType::VCmpGtI32:
		case ShaderInstructionType::VCmpGtU32:
		case ShaderInstructionType::VCmpLeF32:
		case ShaderInstructionType::VCmpLeI32:
		case ShaderInstructionType::VCmpLeU32:
		case ShaderInstructionType::VCmpLgF32:
		case ShaderInstructionType::VCmpLtF32:
		case ShaderInstructionType::VCmpLtI32:
		case ShaderInstructionType::VCmpLtU32:
		case ShaderInstructionType::VCmpNeI32:
		case ShaderInstructionType::VCmpNeqF32:
		case ShaderInstructionType::VCmpNeU32:
		case ShaderInstructionType::VCmpNgeF32:
		case ShaderInstructionType::VCmpNgtF32:
		case ShaderInstructionType::VCmpNleF32:
		case ShaderInstructionType::VCmpNlgF32:
		case ShaderInstructionType::VCmpNltF32:
		case ShaderInstructionType::VCmpOF32:
		case ShaderInstructionType::VCmpTI32:
		case ShaderInstructionType::VCmpTruF32:
		case ShaderInstructionType::VCmpTU32:
		case ShaderInstructionType::VCmpUF32:
		case ShaderInstructionType::VCmpxEqU32:
		case ShaderInstructionType::VCmpxGeI32:
		case ShaderInstructionType::VCmpxGeU32:
		case ShaderInstructionType::VCmpxGtF32:
		case ShaderInstructionType::VCmpxGtU32:
		case ShaderInstructionType::VCmpxLtF32:
		case ShaderInstructionType::VCmpxNeqF32:
		case ShaderInstructionType::VCmpxNeU32:
		case ShaderInstructionType::VCndmaskB32:
		case ShaderInstructionType::VCosF32:
		case ShaderInstructionType::VCvtF32F16:
		case ShaderInstructionType::VCvtF32I32:
		case ShaderInstructionType::VCvtI32F32:
		case ShaderInstructionType::VCvtF32U32:
		case ShaderInstructionType::VCvtF32Ubyte0:
		case ShaderInstructionType::VCvtF32Ubyte1:
		case ShaderInstructionType::VCvtF32Ubyte2:
		case ShaderInstructionType::VCvtF32Ubyte3:
		case ShaderInstructionType::VCvtPkrtzF16F32:
		case ShaderInstructionType::VCvtU32F32:
		case ShaderInstructionType::VExpF32:
		case ShaderInstructionType::VFloorF32:
		case ShaderInstructionType::VFmaF32:
		case ShaderInstructionType::VFractF32:
		case ShaderInstructionType::VLogF32:
		case ShaderInstructionType::VLshlB32:
		case ShaderInstructionType::VLshlrevB32:
		case ShaderInstructionType::VLshrB32:
		case ShaderInstructionType::VLshrrevB32:
		case ShaderInstructionType::VMacF32:
		case ShaderInstructionType::VMadakF32:
		case ShaderInstructionType::VMadF32:
		case ShaderInstructionType::VMadmkF32:
		case ShaderInstructionType::VMadU32U24:
		case ShaderInstructionType::VMax3F32:
		case ShaderInstructionType::VMaxF32:
		case ShaderInstructionType::VMbcntHiU32B32:
		case ShaderInstructionType::VMbcntLoU32B32:
		case ShaderInstructionType::VMed3F32:
		case ShaderInstructionType::VMin3F32:
		case ShaderInstructionType::VMinF32:
		case ShaderInstructionType::VMovB32:
		case ShaderInstructionType::VMulF32:
		case ShaderInstructionType::VMulHiU32:
		case ShaderInstructionType::VMulLoI32:
		case ShaderInstructionType::VMulLoU32:
		case ShaderInstructionType::VMulU32U24:
		case ShaderInstructionType::VNotB32:
		case ShaderInstructionType::VOrB32:
		case ShaderInstructionType::VRcpF32:
		case ShaderInstructionType::VRndneF32:
		case ShaderInstructionType::VRsqF32:
		case ShaderInstructionType::VRsqClampF32:
		case ShaderInstructionType::VSadU32:
		case ShaderInstructionType::VSinF32:
		case ShaderInstructionType::VSqrtF32:
		case ShaderInstructionType::VSubF32:
		case ShaderInstructionType::VSubI32:
		case ShaderInstructionType::VSubrevF32:
		case ShaderInstructionType::VSubrevI32:
		case ShaderInstructionType::VTruncF32:
		case ShaderInstructionType::VXorB32:
		case ShaderInstructionType::VMulLegacyF32:
		case ShaderInstructionType::VMacLegacyF32:
		case ShaderInstructionType::VMinU32:
			return true;
		default: return false;
	}
}

static bool RectListMutatesExec(const ShaderInstruction& instruction);

// A bounded proof for the new forward-only VS profile. In an else-if chain,
// ReadBlock stops at the next condition rather than at the common join.
static bool VertexForwardPathsReach(const ShaderCode& code, uint32_t first_pc, uint32_t join_pc)
{
	const auto& insts = code.GetInstructions();
	if (insts.IsEmpty() || insts.Size() > 16384 || first_pc >= join_pc) return false;
	uint32_t first = insts.Size(), join = insts.Size();
	for (uint32_t i = 0; i < insts.Size(); ++i)
	{
		if (i > 0 && insts.At(i).pc <= insts.At(i - 1).pc) return false;
		if (insts.At(i).pc == first_pc) first = i;
		if (insts.At(i).pc == join_pc) join = i;
	}
	if (first >= join || join >= insts.Size()) return false;
	std::vector<bool> reaches(join + 1, false);
	reaches[join] = true;
	for (uint32_t reverse = join; reverse > first; --reverse)
	{
		const uint32_t i = reverse - 1;
		const auto& inst = insts.At(i);
		if (inst.type == ShaderInstructionType::SEndpgm || inst.type == ShaderInstructionType::SSetpcB64 ||
		    inst.type == ShaderInstructionType::SSwappcB64) continue;
		if (!IsControlFlowTransfer(inst.type)) { reaches[i] = reaches[i + 1]; continue; }
		if (inst.src_num != 1 || inst.format != ShaderInstructionFormat::Label) continue;
		const int64_t target_pc = static_cast<int64_t>(inst.pc) + 4 + inst.src[0].constant.i;
		uint32_t target = i + 1;
		while (target <= join && insts.At(target).pc != target_pc) ++target;
		if (target > join || target_pc <= inst.pc) continue;
		reaches[i] = reaches[target] && (inst.type == ShaderInstructionType::SBranch || reaches[i + 1]);
	}
	return reaches[first];
}

bool ShaderVertexResourceProfileRequired(const ShaderVertexInputInfo& input)
{
	return input.bind.extended.used || input.bind.textures2D.textures_num != 0 || input.bind.samplers.samplers_num != 0;
}

static bool VertexSgprRange(const ShaderOperand& operand, int size)
{
	return size > 0 && size <= 104 && operand.type == ShaderOperandType::Sgpr && operand.size == size && operand.register_id >= 0 &&
	       operand.register_id <= 104 - size;
}

static bool VertexSgprTuple(const ShaderOperand& operand, int size)
{
	return VertexSgprRange(operand, size) && !operand.absolute && !operand.negate && !operand.clamp && operand.multiplier == 1.0f;
}

static bool VertexFloatArithmeticSgprSourceSupported(const ShaderInstruction& inst, int source)
{
	// These exact VOP3 sources already use operand_load_float. Resource
	// tuples and scalar/integer operations retain the plain tuple predicate.
	int sources = 0;
	switch (inst.type)
	{
		case ShaderInstructionType::VMadF32:
		case ShaderInstructionType::VFmaF32:
		case ShaderInstructionType::VMin3F32:
		case ShaderInstructionType::VMax3F32:
		case ShaderInstructionType::VMed3F32: sources = 3; break;
		case ShaderInstructionType::VAddF32:
		case ShaderInstructionType::VMacF32:
		case ShaderInstructionType::VMaxF32:
		case ShaderInstructionType::VMinF32:
		case ShaderInstructionType::VMulF32:
		case ShaderInstructionType::VSubF32:
		case ShaderInstructionType::VSubrevF32: sources = 2; break;
		default: return false;
	}
	const auto format = sources == 3 ? ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2 : ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	if (inst.format != format || inst.src_num != sources || source < 0 || source >= sources) return false;
	const auto plain = [](const ShaderOperand& operand) {
		return !operand.absolute && !operand.negate && !operand.clamp && operand.multiplier == 1.0f;
	};
	const auto empty = [&](const ShaderOperand& operand) {
		return operand.type == ShaderOperandType::Unknown && operand.constant.u == 0 && operand.register_id == 0 &&
		       operand.size == 0 && plain(operand);
	};
	if (inst.dst.type != ShaderOperandType::Vgpr || inst.dst.size != 1 || inst.dst.register_id < 0 || inst.dst.register_id >= 256 ||
	    inst.dst.constant.u != 0 || inst.dst.absolute || inst.dst.negate ||
	    (inst.dst.multiplier != 1.0f && inst.dst.multiplier != 2.0f && inst.dst.multiplier != 4.0f && inst.dst.multiplier != 0.5f) ||
	    !empty(inst.dst2) || inst.globally_coherent || inst.unnormalized_coordinates ||
	    inst.export_valid_mask || inst.export_done) return false;
	for (int index = sources; index < 4; ++index)
	{
		// VOP3 decodes all three raw source fields before reducing src_num.
		// A neutral inactive binary field remains the plain s0 operand;
		// the ordinary VOP2 decoder leaves that same IR slot empty.
		const auto& operand = inst.src[index];
		if (!empty(operand) && !(sources == 2 && index == 2 && VertexSgprTuple(operand, 1) &&
		                        operand.register_id == 0 && operand.constant.u == 0)) return false;
	}
	for (int index = 0; index < sources; ++index)
	{
		const auto& operand = inst.src[index];
		if (operand.clamp || operand.multiplier != 1.0f) return false;
		switch (operand.type)
		{
			case ShaderOperandType::Sgpr:
				if (!VertexSgprRange(operand, 1) || operand.constant.u != 0) return false;
				break;
			case ShaderOperandType::Vgpr:
				if (operand.size != 1 || operand.register_id < 0 || operand.register_id >= 256 || operand.constant.u != 0) return false;
				break;
			case ShaderOperandType::LiteralConstant:
			case ShaderOperandType::IntegerInlineConstant:
			case ShaderOperandType::FloatInlineConstant:
				if (operand.size != 0 || operand.register_id != 0) return false;
				break;
			case ShaderOperandType::VccLo: case ShaderOperandType::VccHi:
			case ShaderOperandType::ExecLo: case ShaderOperandType::ExecHi:
			case ShaderOperandType::ExecZ: case ShaderOperandType::M0:
				if (operand.size != 1 || operand.register_id != 0 || operand.constant.u != 0) return false;
				break;
			default: return false;
		}
	}
	return VertexSgprRange(inst.src[source], 1);
}

static bool VertexFetchImmediateInitialization(const ShaderInstruction& inst, ShaderOperandType destination)
{
	const auto plain = [](const ShaderOperand& operand) {
		return !operand.absolute && !operand.negate && !operand.clamp && operand.multiplier == 1.0f;
	};
	const auto empty = [&](const ShaderOperand& operand) {
		return operand.type == ShaderOperandType::Unknown && operand.constant.u == 0 && operand.register_id == 0 &&
		       operand.size == 0 && plain(operand);
	};
	if (inst.type != ShaderInstructionType::SMovB32 || inst.format != ShaderInstructionFormat::SVdstSVsrc0 || inst.src_num != 1 ||
	    inst.dst.type != destination || inst.dst.size != 1 || inst.dst.register_id != 0 || inst.dst.constant.u != 0 ||
	    !plain(inst.dst) || !plain(inst.src[0]) || inst.src[0].size != 0 || inst.src[0].register_id != 0 ||
	    (inst.src[0].type != ShaderOperandType::IntegerInlineConstant && inst.src[0].type != ShaderOperandType::LiteralConstant) ||
	    !empty(inst.dst2) || inst.globally_coherent || inst.unnormalized_coordinates || inst.export_valid_mask || inst.export_done)
		return false;
	for (int i = 1; i < 4; ++i) if (!empty(inst.src[i])) return false;
	return true;
}

// Locate the one external FETCH substitution. Beyond the legacy entry layout,
// only immediate VCC_HI/M0 moves and suppressed trace events may precede
// the moved call. Both moves retain their ordinary lowering. This is not a
// model for arbitrary PC transfers.
static bool VertexExternalFetchEntryCall(const ShaderCode& code, const ShaderVertexInputInfo& input,
                                        uint32_t* call_index, const char** rejection_reason = nullptr)
{
	const auto reject = [&](const char* reason) { if (rejection_reason != nullptr) *rejection_reason = reason; return false; };
	const auto& insts = code.GetInstructions();
	if (call_index == nullptr || code.GetType() != ShaderType::Vertex || !input.fetch_external || input.fetch_inline ||
	    input.fetch_shader_reg != 0 || insts.Size() < 2 || insts.Size() > 16384) return reject("vertex fetch entry layout");
	*call_index = UINT32_MAX;
	for (uint32_t i = 0; i < insts.Size(); ++i)
	{
		if (insts.At(i).type != ShaderInstructionType::SSwappcB64) continue;
		if (*call_index != UINT32_MAX) return reject("vertex multiple fetch calls");
		*call_index = i;
	}
	if (*call_index == UINT32_MAX) return reject("vertex missing fetch call");
	const auto& call = insts.At(*call_index);
	if (call.format != ShaderInstructionFormat::Sdst2Ssrc02 || call.src_num != 1 || !VertexSgprTuple(call.dst, 2) ||
	    !VertexSgprTuple(call.src[0], 2) || call.dst.register_id != 0 || call.src[0].register_id != 0 ||
	    call.dst.constant.u != 0 || call.src[0].constant.u != 0 || call.globally_coherent || call.unnormalized_coordinates ||
	    call.export_valid_mask || call.export_done)
		return reject("vertex noncanonical fetch call");
	const auto unused = [](const ShaderOperand& operand) {
		return operand.type == ShaderOperandType::Unknown && operand.constant.u == 0 && operand.register_id == 0 && operand.size == 0 &&
		       !operand.absolute && !operand.negate && !operand.clamp && operand.multiplier == 1.0f;
	};
	if (!unused(call.dst2)) return reject("vertex fetch unused operand");
	for (int i = 1; i < 4; ++i) if (!unused(call.src[i])) return reject("vertex fetch unused operand");
	if (*call_index == 1)
	{
		// Preserve the existing resource profile's entry initialization.
		return (insts.At(0).type == ShaderInstructionType::SMovB32 && insts.At(0).dst.type == ShaderOperandType::VccHi) ||
		       reject("vertex fetch entry call");
	}
	bool trace_seen = false;
	bool initialized = false;
	bool m0_initialized = false;
	for (uint32_t i = 0; i < *call_index; ++i)
	{
		const auto& inst = insts.At(i);
		if (ShaderThreadTraceDataInstructionSupported(code, inst)) { trace_seen = true; continue; }
		if (!initialized && VertexFetchImmediateInitialization(inst, ShaderOperandType::VccHi)) { initialized = true; continue; }
		if (!m0_initialized && VertexFetchImmediateInitialization(inst, ShaderOperandType::M0)) { m0_initialized = true; continue; }
		return reject("vertex fetch prefix effect");
	}
	if (!trace_seen || !initialized) return reject("vertex fetch prefix lacks trace or VCC initialization");
	// No backedge may reenter the prefix/call, and no indirect transfer may
	// bypass its attribute initialization. The resource proof below validates
	// every remaining forward edge and the omitted scalar effects.
	for (const auto& label: code.GetLabels())
		if (!label.IsDisabled() && label.GetDst() <= call.pc) return reject("vertex fetch prefix is a branch target");
	for (const auto& label: code.GetIndirectLabels())
		if (!label.IsDisabled() && label.GetDst() <= call.pc) return reject("vertex fetch prefix is an indirect target");
	for (uint32_t i = *call_index + 1; i < insts.Size(); ++i)
	{
		const auto& inst = insts.At(i);
		if (inst.type == ShaderInstructionType::SSetpcB64) return reject("vertex fetch indirect continuation");
		// The allocation-dependent SGPR alias of VCC is not represented in
		// this profile. Do not claim its omitted fetch effects are dead merely
		// because the continuation accesses the VCC operand name instead.
		if (inst.type == ShaderInstructionType::SCbranchVccz || inst.type == ShaderInstructionType::SCbranchVccnz)
			return reject("vertex fetch VCC alias observation");
		for (const auto& operand: inst.src)
			if (operand.type == ShaderOperandType::VccLo || operand.type == ShaderOperandType::VccHi)
				return reject("vertex fetch VCC alias observation");
		for (const auto* operand: {&inst.dst, &inst.dst2})
			if (operand->type == ShaderOperandType::VccLo || operand->type == ShaderOperandType::VccHi)
				return reject("vertex fetch VCC alias mutation");
	}
	return true;
}

bool ShaderVertexFetchEffectsSupported(const ShaderCode& code, int fetch_register, int table_register,
                                      uint64_t* written_sgprs, const char** rejection_reason)
{
	const auto reject = [&](const char* reason) { if (rejection_reason != nullptr) *rejection_reason = reason; return false; };
	if (written_sgprs == nullptr) return reject("missing fetch effect output");
	written_sgprs[0] = written_sgprs[1] = 0;
	if (code.GetType() != ShaderType::Fetch || fetch_register != 0 || table_register != 2 ||
	    code.GetInstructions().Size() < 2 || code.GetInstructions().Size() > 256 || !code.GetLabels().IsEmpty() ||
	    !code.GetIndirectLabels().IsEmpty()) return reject("fetch layout or control flow");
	int descriptor_start[104];
	for (int& start: descriptor_start) start = -1;
	bool table_live = true;
	int loads = 0;
	int attributes = 0;
	for (uint32_t i = 0; i < code.GetInstructions().Size(); ++i)
	{
		const auto& inst = code.GetInstructions().At(i);
		if (inst.type == ShaderInstructionType::SLoadDwordx4)
		{
			if (!table_live || inst.src_num != 2 || !VertexSgprTuple(inst.src[0], 2) || inst.src[0].register_id != table_register ||
			    !VertexSgprTuple(inst.dst, 4) || inst.src[1].type != ShaderOperandType::LiteralConstant ||
			    (inst.src[1].constant.u & 3u) != 0 || inst.src[1].constant.u > 4096u - 16u || ++loads > 16 ||
			    OperandWritesSgprRange(inst.dst, fetch_register, 2)) return reject("fetch descriptor load");
			// A partial overwrite invalidates every affected descriptor tuple.
			for (int r = 0; r < 104; ++r)
				if (descriptor_start[r] >= 0 && OperandWritesSgprRange(inst.dst, descriptor_start[r], 4)) descriptor_start[r] = -1;
			for (int lane = 0; lane < 4; ++lane)
			{
				const int r = inst.dst.register_id + lane;
				descriptor_start[r] = inst.dst.register_id;
				written_sgprs[r / 64] |= uint64_t{1} << (r % 64);
			}
			table_live = !OperandWritesSgprRange(inst.dst, table_register, 2);
			continue;
		}
		int width = 0;
		switch (inst.type)
		{
			case ShaderInstructionType::BufferLoadFormatX: width = 1; break;
			case ShaderInstructionType::BufferLoadFormatXy: width = 2; break;
			case ShaderInstructionType::BufferLoadFormatXyz: width = 3; break;
			case ShaderInstructionType::BufferLoadFormatXyzw: width = 4; break;
			default: break;
		}
		if (width != 0)
		{
			const ShaderInstructionFormat::Format formats[] = {ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen,
			    ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen, ShaderInstructionFormat::Vdata3VaddrSvSoffsIdxen,
			    ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen};
			if (inst.src_num != 3 || inst.dst.type != ShaderOperandType::Vgpr || inst.dst.size != width ||
			    inst.format != formats[width - 1] ||
			    inst.dst.register_id <= 0 || inst.dst.register_id > 256 - width ||
			    inst.src[0].type != ShaderOperandType::Vgpr || inst.src[0].register_id != 0 || inst.src[0].size != 1 ||
			    !VertexSgprTuple(inst.src[1], 4) || inst.src[2].type != ShaderOperandType::IntegerInlineConstant ||
			    inst.src[2].constant.i != 0 || inst.globally_coherent || ++attributes > 16)
				return reject("fetch attribute load");
			for (int lane = 0; lane < 4; ++lane)
				if (descriptor_start[inst.src[1].register_id + lane] != inst.src[1].register_id)
					return reject("fetch descriptor provenance");
			continue;
		}
		if (inst.type == ShaderInstructionType::STTraceData)
		{
			if (!ShaderThreadTraceDataInstructionSupported(code, inst)) return reject("fetch thread trace contract");
			continue;
		}
		if (inst.type == ShaderInstructionType::SWaitcnt) continue;
		if (inst.type != ShaderInstructionType::SSetpcB64 || i + 1 != code.GetInstructions().Size() || inst.src_num != 1 ||
		    !VertexSgprTuple(inst.src[0], 2) || inst.src[0].register_id != fetch_register)
			return reject("fetch has effects not represented by vertex attributes");
	}
	if (code.GetInstructions().At(code.GetInstructions().Size() - 1).type != ShaderInstructionType::SSetpcB64 || loads == 0 || loads != attributes)
		return reject("fetch terminal or descriptor count");
	return true;
}

// This proof runs before cache lookup as well as compilation. Each descriptor
// word keeps its declared origin across a forward CFG; joins retain an origin
// only when every incoming path agrees. No homogeneous-resource fallback is used.
bool ShaderVertexResourceProfileSupported(const ShaderCode& code, const ShaderVertexInputInfo& input,
                                         const char** rejection_reason, uint32_t* rejection_pc)
{
	if (!ShaderVertexResourceProfileRequired(input))
	{
		// Keep the legacy non-resource layout. A moved external call needs the
		// same scalar-effect/CFG proof even when no sampled resource is bound.
		bool moved_fetch_call = false;
		if (input.fetch_external)
			for (uint32_t i = 0; i < code.GetInstructions().Size(); ++i)
				if (code.GetInstructions().At(i).type == ShaderInstructionType::SSwappcB64 && i != 1) moved_fetch_call = true;
		if (!moved_fetch_call) return true;
	}
	const auto reject = [&](const char* reason, uint32_t pc = UINT32_MAX) {
		if (rejection_reason != nullptr) *rejection_reason = reason;
		if (rejection_pc != nullptr) *rejection_pc = pc;
		return false;
	};
	const auto& bind = input.bind;
	const auto& insts = code.GetInstructions();
	if (bind.storage_buffers.buffers_num < 0 || bind.storage_buffers.buffers_num > ShaderStorageResources::BUFFERS_MAX ||
	    bind.textures2D.textures_num < 0 || bind.textures2D.textures_num > ShaderTextureResources::RES_MAX ||
	    bind.samplers.samplers_num < 0 || bind.samplers.samplers_num > ShaderSamplerResources::RES_MAX ||
	    bind.direct_sgprs.sgprs_num < 0 || bind.direct_sgprs.sgprs_num > ShaderDirectSgprsResources::SGPRS_MAX)
		return reject("vertex resource count");
	if (code.GetType() != ShaderType::Vertex || input.rect_list || input.fetch_inline || insts.IsEmpty() || insts.Size() > 16384 ||
	    bind.gds_pointers.pointers_num != 0 || bind.textures2D.textures2d_storage_num != 0 ||
	    (input.fetch_external && (!input.fetch_effects_valid || input.fetch_shader_reg != 0)))
		return reject("vertex resource profile layout");
	// The parser also uses this collection for static conditional fallthrough.
	for (const auto& label: code.GetIndirectLabels())
	{
		bool found = false;
		for (uint32_t i = 0; i + 1 < insts.Size(); ++i)
		{
			const auto& inst = insts.At(i);
			if (IsControlFlowTransfer(inst.type) && inst.type != ShaderInstructionType::SBranch &&
			    inst.type != ShaderInstructionType::SSetpcB64 && inst.type != ShaderInstructionType::SSwappcB64 &&
			    inst.pc == label.GetSrc() && label.GetDst() == inst.pc + 4 && insts.At(i + 1).pc == label.GetDst()) { found = true; break; }
		}
		if (!found) return reject("vertex nonstatic control-flow metadata", label.GetSrc());
	}
	struct State { int16_t origin[104] = {}; bool defined[104] = {}; bool reached = false; };
	std::vector<State> states(insts.Size());
	State entry;
	entry.reached = true;
	// Positive origins are declaration word indices plus one; negative origins
	// identify the two pointer lanes, which must never become ordinary shader data.
	int kind[80] = {};
	int start[80] = {};
	int widths[80] = {};
	const auto descriptor = [&](int first, int width, int resource_kind, bool extended) {
		if (first < 0 || first > 80 - width || extended != (first >= 16) || (!extended && first + width > 16)) return false;
		for (int lane = 0; lane < width; ++lane) if (kind[first + lane] != 0) return false;
		for (int lane = 0; lane < width; ++lane)
		{
			kind[first + lane] = resource_kind; start[first + lane] = first; widths[first + lane] = width;
			if (!extended) { entry.origin[first + lane] = static_cast<int16_t>(first + lane + 1); entry.defined[first + lane] = true; }
		}
		return true;
	};
	for (int i = 0; i < bind.storage_buffers.buffers_num; ++i)
		if ((bind.storage_buffers.usages[i] != ShaderStorageUsage::ReadOnly && bind.storage_buffers.usages[i] != ShaderStorageUsage::Constant) ||
		    !descriptor(bind.storage_buffers.start_register[i], 4, 1, bind.storage_buffers.extended[i])) return reject("vertex buffer declaration");
	for (int i = 0; i < bind.textures2D.textures_num; ++i)
	{
		const auto& desc = bind.textures2D.desc[i];
		if (desc.usage != ShaderTextureUsage::ReadOnly || desc.IsNullSampledImage() || desc.texture.Type() != ShaderTextureResource::TYPE_2D ||
		    !descriptor(desc.start_register, 8, 2, desc.extended)) return reject("vertex sampled declaration");
	}
	for (int i = 0; i < bind.samplers.samplers_num; ++i)
		if (!descriptor(bind.samplers.start_register[i], 4, 3, bind.samplers.extended[i])) return reject("vertex sampler declaration");
	for (int i = 0; i < bind.direct_sgprs.sgprs_num; ++i)
	{
		const int r = bind.direct_sgprs.start_register[i];
		if (r < 0 || r >= 16 || entry.defined[r]) return reject("vertex direct scalar overlap");
		entry.defined[r] = true;
	}
	if (bind.extended.used)
	{
		const int r = bind.extended.start_register;
		if (r < 0 || r > 14 || entry.defined[r] || entry.defined[r + 1]) return reject("vertex extension pointer overlap");
		entry.origin[r] = -1; entry.origin[r + 1] = -2;
		entry.defined[r] = entry.defined[r + 1] = true;
	}
	if (input.fetch_external)
	{
		for (int r = 0; r < 4; ++r) if (entry.defined[r]) return reject("vertex fetch pointer overlaps live input");
	}
	uint32_t fetch_call_index = UINT32_MAX;
	if (input.fetch_external)
	{
		const char* entry_rejection = nullptr;
		if (!VertexExternalFetchEntryCall(code, input, &fetch_call_index, &entry_rejection))
			return reject(entry_rejection, fetch_call_index < insts.Size() ? insts.At(fetch_call_index).pc : UINT32_MAX);
	}
	states[0] = entry;
	const auto merge = [&](uint32_t index, const State& incoming) {
		auto& target = states[index];
		if (!target.reached) { target = incoming; target.reached = true; return; }
		for (int r = 0; r < 104; ++r)
		{
			target.defined[r] = target.defined[r] && incoming.defined[r];
			if (target.origin[r] != incoming.origin[r])
			{
				// Different descriptor origins are still rewritten host words;
				// joining them must not turn either value into ordinary guest data.
				if (target.origin[r] != 0 || incoming.origin[r] != 0) target.defined[r] = false;
				target.origin[r] = 0;
			}
		}
	};
	for (uint32_t i = 0; i < insts.Size(); ++i)
	{
		const auto& inst = insts.At(i);
		if (i != 0 && inst.pc <= insts.At(i - 1).pc) return reject("vertex instruction order", inst.pc);
		if (!states[i].reached) return reject("vertex unreachable instruction", inst.pc);
		State state = states[i];
		// The admitted explicit-LOD lowering executes with the prologue's active
		// lane. Its profile does not yet model an EXEC-masked image result.
		if (RectListMutatesExec(inst)) return reject("vertex resource profile changes EXEC", inst.pc);
		const bool load = inst.type == ShaderInstructionType::SLoadDwordx4 || inst.type == ShaderInstructionType::SLoadDwordx8;
		const bool call = inst.type == ShaderInstructionType::SSwappcB64;
		const bool branch = IsControlFlowTransfer(inst.type) && !call;
		if (!RectListInstructionIsPure(code, inst) && inst.type != ShaderInstructionType::SBranch && !load && !call &&
		    inst.type != ShaderInstructionType::ImageSampleLz) return reject("vertex instruction outside read-only profile", inst.pc);
		int resource_source = -1;
		switch (inst.type)
		{
			case ShaderInstructionType::SBufferLoadDword: case ShaderInstructionType::SBufferLoadDwordx2:
			case ShaderInstructionType::SBufferLoadDwordx4: case ShaderInstructionType::SBufferLoadDwordx8:
			case ShaderInstructionType::SBufferLoadDwordx16: resource_source = 0; break;
			case ShaderInstructionType::BufferLoadDword: case ShaderInstructionType::BufferLoadFormatX:
			case ShaderInstructionType::BufferLoadFormatXy: case ShaderInstructionType::BufferLoadFormatXyz:
			case ShaderInstructionType::BufferLoadFormatXyzw: case ShaderInstructionType::TBufferLoadFormatX:
			case ShaderInstructionType::TBufferLoadFormatXyzw: resource_source = 1; break;
			default: break;
		}
		const auto resource = [&](const ShaderOperand& operand, int width, int wanted_kind) {
			if (!VertexSgprTuple(operand, width)) return false;
			const int first = state.origin[operand.register_id] - 1;
			if (first < 0 || first > 80 - width || start[first] != first || widths[first] != width || kind[first] != wanted_kind) return false;
			for (int lane = 0; lane < width; ++lane)
				if (!state.defined[operand.register_id + lane] || state.origin[operand.register_id + lane] != first + lane + 1) return false;
			return true;
		};
		if (resource_source >= 0 && (inst.src_num <= resource_source || !resource(inst.src[resource_source], 4, 1)))
			return reject("vertex buffer provenance", inst.pc);
		const bool sample = inst.type == ShaderInstructionType::ImageSampleLz;
		if (sample && (inst.format != ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7 || inst.src_num != 3 ||
		               inst.globally_coherent || inst.unnormalized_coordinates || !resource(inst.src[1], 8, 2) || !resource(inst.src[2], 4, 3)))
			return reject("vertex explicit-LOD sample provenance or format", inst.pc);
		int loaded_first = -1;
		int loaded_width = 0;
		if (load)
		{
			loaded_width = inst.type == ShaderInstructionType::SLoadDwordx4 ? 4 : 8;
			if (!bind.extended.used || inst.src_num != 2 || !VertexSgprTuple(inst.src[0], 2) ||
			    inst.src[0].register_id != bind.extended.start_register || !VertexSgprTuple(inst.dst, loaded_width) ||
			    inst.src[1].type != ShaderOperandType::LiteralConstant || (inst.src[1].constant.u & 3u) != 0 ||
			    inst.src[1].constant.u > static_cast<uint32_t>((64 - loaded_width) * 4) ||
			    !state.defined[inst.src[0].register_id] || !state.defined[inst.src[0].register_id + 1] ||
			    state.origin[inst.src[0].register_id] != -1 || state.origin[inst.src[0].register_id + 1] != -2)
				return reject("vertex extension load or pointer lifetime", inst.pc);
			loaded_first = 16 + static_cast<int>(inst.src[1].constant.u / 4u);
			if (kind[loaded_first] == 0 || start[loaded_first] != loaded_first || widths[loaded_first] != loaded_width)
				return reject("vertex extension load is not a complete mapped descriptor", inst.pc);
		}
		if (call)
		{
			if (!input.fetch_external || i != fetch_call_index || inst.src_num != 1 || !VertexSgprTuple(inst.dst, 2) ||
			    !VertexSgprTuple(inst.src[0], 2) || inst.dst.register_id != 0 || inst.src[0].register_id != 0)
				return reject("vertex noncanonical fetch call", inst.pc);
			for (int r = 0; r < 104; ++r)
				if (r < 2 || (input.fetch_written_sgprs[r / 64] & (uint64_t{1} << (r % 64))) != 0)
				{ state.defined[r] = false; state.origin[r] = 0; }
		}
		if (!call)
		{
			if (inst.src_num < 0 || inst.src_num > 4) return reject("vertex operand count", inst.pc);
			for (int source = 0; source < inst.src_num; ++source)
			{
				const auto& operand = inst.src[source];
				if (operand.type != ShaderOperandType::Sgpr) continue;
				const bool descriptor_use = source == resource_source || (sample && (source == 1 || source == 2)) || (load && source == 0);
				if (!VertexSgprTuple(operand, operand.size) &&
				    (descriptor_use || !VertexFloatArithmeticSgprSourceSupported(inst, source)))
				{
					// Keep the terminal gate bounded and distinguish a rejected
					// arithmetic modifier from an invalid register span.
					const char* cause = operand.size <= 0 || operand.size > 104 ? "size" :
					                    operand.register_id < 0 || operand.register_id > 104 - operand.size ? "bank" : "modifier";
					static std::atomic<unsigned> diagnostic_count {0};
					unsigned record = diagnostic_count.load(std::memory_order_relaxed);
					if (rejection_reason != nullptr)
					{
						while (record < 16 && !diagnostic_count.compare_exchange_weak(record, record + 1, std::memory_order_relaxed)) {}
						if (record < 16)
							std::fprintf(stderr,
							             "[GFX_VERTEX_SGPR_OPERAND_REJECT] cause=%s index=%u pc=%08" PRIx32
							             " opcode_id=%u opcode=%s format=%016" PRIx64 " source=%d register=%d size=%d abs=%u neg=%u clamp=%u multiplier=%g\n",
							             cause, i, inst.pc, static_cast<unsigned>(inst.type), Core::EnumName8(inst.type).c_str(),
							             static_cast<uint64_t>(inst.format), source,
							             operand.register_id, operand.size, operand.absolute ? 1u : 0u, operand.negate ? 1u : 0u,
							             operand.clamp ? 1u : 0u, static_cast<double>(operand.multiplier));
					}
					return reject("vertex SGPR range", inst.pc);
				}
				for (int lane = 0; lane < operand.size; ++lane)
					if (!state.defined[operand.register_id + lane] || (!descriptor_use && state.origin[operand.register_id + lane] != 0))
						return reject("vertex undefined SGPR, fetch effect, or raw descriptor read", inst.pc);
			}
			// SMulk reads its previous destination although the parser has only
			// the immediate in src[]. Other admitted scalar destinations overwrite.
			if (inst.type == ShaderInstructionType::SMulkI32 && (!VertexSgprTuple(inst.dst, 1) ||
			    !state.defined[inst.dst.register_id] || state.origin[inst.dst.register_id] != 0)) return reject("vertex scalar read-modify-write", inst.pc);
			for (const auto* dst: {&inst.dst, &inst.dst2})
			{
				if (dst->type != ShaderOperandType::Sgpr) continue;
				if (!VertexSgprTuple(*dst, dst->size) || dst->size <= 0) return reject("vertex destination range", inst.pc);
				const bool scalar = inst.type >= ShaderInstructionType::SAddcU32 && inst.type <= ShaderInstructionType::SXorB64;
				for (int lane = 0; lane < dst->size; ++lane)
				{
					if (!scalar && state.origin[dst->register_id + lane] != 0) state.defined[dst->register_id + lane] = false;
					state.origin[dst->register_id + lane] = 0;
					// A vector comparison can be predicated. It cannot discharge an
					// undefined fetch effect merely because it names an SGPR output.
					if (scalar) state.defined[dst->register_id + lane] = true;
				}
			}
			for (int lane = 0; lane < loaded_width; ++lane)
			{ state.origin[inst.dst.register_id + lane] = static_cast<int16_t>(loaded_first + lane + 1); state.defined[inst.dst.register_id + lane] = true; }
		}
		if (inst.type == ShaderInstructionType::SEndpgm)
		{
			if (i + 1 != insts.Size()) return reject("vertex early termination", inst.pc);
			continue;
		}
		if (branch)
		{
			if (inst.type == ShaderInstructionType::SSetpcB64 || inst.src_num != 1 || inst.format != ShaderInstructionFormat::Label)
				return reject("vertex indirect branch", inst.pc);
			const int64_t destination = static_cast<int64_t>(inst.pc) + 4 + inst.src[0].constant.i;
			uint32_t target = i + 1;
			while (target < insts.Size() && insts.At(target).pc != destination) ++target;
			if (destination <= inst.pc || target == insts.Size()) return reject("vertex branch is not forward to an instruction", inst.pc);
			merge(target, state);
			if (inst.type == ShaderInstructionType::SBranch) continue;
		}
		if (i + 1 == insts.Size()) return reject("vertex missing termination", inst.pc);
		merge(i + 1, state);
	}
	return true;
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
static bool ShaderVertexFloatSgprSourceContractTest()
{
	struct Opcode { uint32_t raw; ShaderInstructionType type; int sources; };
	const Opcode opcodes[] {
	    {0x103, ShaderInstructionType::VAddF32, 2}, {0x104, ShaderInstructionType::VSubF32, 2},
	    {0x105, ShaderInstructionType::VSubrevF32, 2}, {0x108, ShaderInstructionType::VMulF32, 2},
	    {0x10f, ShaderInstructionType::VMinF32, 2}, {0x110, ShaderInstructionType::VMaxF32, 2},
	    {0x11f, ShaderInstructionType::VMacF32, 2}, {0x141, ShaderInstructionType::VMadF32, 3},
	    {0x14b, ShaderInstructionType::VFmaF32, 3}, {0x151, ShaderInstructionType::VMin3F32, 3},
	    {0x154, ShaderInstructionType::VMax3F32, 3}, {0x157, ShaderInstructionType::VMed3F32, 3}};
	ShaderVertexInputInfo input {};
	input.bind.extended.used = true;
	input.bind.extended.start_register = 6;
	const auto decode = [](const Opcode& opcode, uint32_t absolute, uint32_t negate, uint32_t omod, bool clamp, ShaderCode* code) {
		// Original owned ISA fixture: three scalar definitions, one VOP3,
		// S_ENDPGM, then bounded-reader padding that is never executed.
		const uint32_t words[13] {
		    0xbe9f03ffu, 0x3f800000u, 0xbea003ffu, 0xc0000000u, 0xbea103ffu, 0x3f000000u,
		    0xd0000000u | (opcode.raw << 17u) | (absolute << 8u) | (clamp ? 1u << 11u : 0u) | 8u,
		    31u | (32u << 9u) | (opcode.sources == 3 ? 33u << 18u : 0u) | (omod << 27u) | (negate << 29u),
		    0xbf810000u};
		code->SetType(ShaderType::Vertex);
		code->SetFp32Mode(ShaderFp32Mode::RneFlushInOut);
		return ShaderTryParseBounded(words, 13, code) == 9 && code->GetInstructions().Size() == 5 &&
		       code->GetInstructions().At(3).pc == 24 && code->GetInstructions().At(3).type == opcode.type &&
		       code->GetInstructions().At(3).src_num == opcode.sources;
	};
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	uint32_t modules = 0;
	const auto validate = [&](const ShaderCode& code, int sources, uint32_t absolute, uint32_t negate) {
		if (!ShaderVertexResourceProfileSupported(code, input)) return false;
		const auto source = SpirvGenerateSource(code, &input, nullptr, nullptr);
		const std::string_view text(source.c_str(), source.Size());
		for (int lane = 0; lane < sources; ++lane)
		{
			if (text.find(String8::FromPrintf("= OpLoad %%uint %%s%d", 31 + lane).c_str()) == std::string_view::npos) return false;
			const bool abs = (absolute & (1u << lane)) != 0, neg = (negate & (1u << lane)) != 0;
			if (abs && neg)
			{
				const auto first = text.find(String8::FromPrintf("%%abs_at%d_3 = OpExtInst %%float %%GLSL_std_450 FAbs %%at%d_3", lane, lane).c_str());
				const auto second = text.find(String8::FromPrintf("%%t%d_3 = OpFNegate %%float %%abs_at%d_3", lane, lane).c_str());
				if (first == std::string_view::npos || second == std::string_view::npos || first >= second) return false;
			} else if (abs)
			{
				if (text.find(String8::FromPrintf("%%t%d_3 = OpExtInst %%float %%GLSL_std_450 FAbs %%at%d_3", lane, lane).c_str()) ==
				    std::string_view::npos) return false;
			} else if (neg)
			{
				if (text.find(String8::FromPrintf("%%t%d_3 = OpFNegate %%float %%nt%d_3", lane, lane).c_str()) == std::string_view::npos) return false;
			}
		}
		const auto module = ShaderRecompileVS(code, &input);
		if (module.Size() < 5 || !validator.Validate(module.GetDataConst(), module.Size())) return false;
		++modules;
		return true;
	};
	for (const auto& opcode: opcodes)
		for (uint32_t absolute = 0; absolute < (1u << opcode.sources); ++absolute)
			for (uint32_t negate = 0; negate < (1u << opcode.sources); ++negate)
			{
				ShaderCode code;
				if (!decode(opcode, absolute, negate, 0, false, &code) || !validate(code, opcode.sources, absolute, negate)) return false;
			}
	// The destination's supported clamp/output modifiers remain independent
	// from source modifiers; verify both arities through the actual decoder.
	for (const auto index: {3u, 7u})
		for (uint32_t omod = 0; omod < 4; ++omod)
			for (const bool clamp: {false, true})
			{
				ShaderCode code;
				if (!decode(opcodes[index], 1, 1, omod, clamp, &code) || !validate(code, opcodes[index].sources, 1, 1)) return false;
			}
	ShaderCode base;
	if (!decode(opcodes[7], 0, 1, 0, false, &base)) return false;
	const auto rejected = [&](const ShaderCode& code) { return !ShaderVertexResourceProfileSupported(code, input); };
	for (const int size: {std::numeric_limits<int>::min(), -1, 0, 2, 105, std::numeric_limits<int>::max()})
	{
		auto bad = base; bad.GetInstructions()[3].src[0].size = size;
		if (!rejected(bad)) return false;
	}
	for (const int reg: {std::numeric_limits<int>::min(), -1, 104, std::numeric_limits<int>::max()})
	{
		auto bad = base; bad.GetInstructions()[3].src[0].register_id = reg;
		if (!rejected(bad)) return false;
	}
	for (int variant = 0; variant < 24; ++variant)
	{
		auto bad = base; auto& inst = bad.GetInstructions()[3];
		switch (variant)
		{
			case 0: inst.src[0].clamp = true; break;
			case 1: inst.src[0].multiplier = 2.0f; break;
			case 2: inst.src[0].constant.u = 1; break;
			case 3: inst.format = ShaderInstructionFormat::SVdstSVsrc0SVsrc1; break;
			case 4: inst.src_num = 2; break;
			case 5: inst.type = ShaderInstructionType::VMadU32U24; break;
			case 6: inst.type = ShaderInstructionType::VMadakF32; break;
			case 7: inst.type = ShaderInstructionType::VMadmkF32; break;
			case 8: inst.dst.absolute = true; break;
			case 9: inst.dst.negate = true; break;
			case 10: inst.dst.multiplier = 3.0f; break;
			case 11: inst.dst.register_id = 256; break;
			case 12: inst.dst.size = 2; break;
			case 13: inst.dst.constant.u = 1; break;
			case 14: inst.dst2.type = ShaderOperandType::Sgpr; break;
			case 15: inst.src[3].constant.u = 1; break;
			case 16: inst.globally_coherent = true; break;
			case 17: inst.unnormalized_coordinates = true; break;
			case 18: inst.export_valid_mask = true; break;
			case 19: inst.export_done = true; break;
			case 20: inst.src[1].type = ShaderOperandType::Null; break;
			case 21: inst.src[1].type = ShaderOperandType::Vgpr; inst.src[1].register_id = 256; break;
			case 22: inst.src[0].register_id = 6; break; // The extension pointer is never ordinary float data.
			case 23: bad.GetInstructions()[0].dst.register_id = 34; break; // s31 loses its dominating definition.
		}
		if (!rejected(bad)) return false;
	}
	// A modified descriptor word cannot pass the ordinary-data origin gate.
	auto descriptor_input = input;
	descriptor_input.bind.storage_buffers.buffers_num = 1;
	descriptor_input.bind.storage_buffers.start_register[0] = 8;
	descriptor_input.bind.storage_buffers.usages[0] = ShaderStorageUsage::Constant;
	auto descriptor = base; descriptor.GetInstructions()[3].src[0].register_id = 8;
	if (ShaderVertexResourceProfileSupported(descriptor, descriptor_input)) return false;
	// Binary VOP3 has one neutral inactive raw source; VOP2 leaves it empty.
	ShaderCode binary;
	if (!decode(opcodes[3], 0, 1, 0, false, &binary) || !ShaderVertexResourceProfileSupported(binary, input)) return false;
	auto empty_binary = binary; empty_binary.GetInstructions()[3].src[2] = {};
	if (!ShaderVertexResourceProfileSupported(empty_binary, input)) return false;
	for (int variant = 0; variant < 3; ++variant)
	{
		auto bad = binary;
		if (variant == 0) bad.GetInstructions()[3].src[2].register_id = 1;
		if (variant == 1) bad.GetInstructions()[3].src[2].negate = true;
		if (variant == 2) bad.GetInstructions()[3].src[2].absolute = true;
		if (!rejected(bad)) return false;
	}
	// Original raw-ISA integration fixtures keep the same canonical ABS+NEG
	// VMul source at a join. Positive controls prove that fetch/branch shape
	// admission succeeds; negatives must reach the shared defined/origin gate.
	constexpr uint32_t multiply_word0 = 0xd0000000u | (0x108u << 17u) | (1u << 8u) | 8u;
	constexpr uint32_t multiply_word1 = 16u | (242u << 9u) | (1u << 29u); // -abs(s16), float-inline 1.0
	const auto parse_integration = [](const uint32_t* words, uint32_t count, uint32_t consumed, ShaderCode* code) {
		code->SetType(ShaderType::Vertex);
		code->SetThreadTraceMode(ShaderThreadTraceMode::Disabled);
		code->SetFp32Mode(ShaderFp32Mode::RneFlushInOut);
		return ShaderTryParseBounded(words, count, code) == consumed;
	};
	const auto provenance_rejected = [](const ShaderCode& code, const ShaderVertexInputInfo& bound, uint32_t read_index) {
		const auto& arithmetic = code.GetInstructions().At(read_index);
		if (arithmetic.type != ShaderInstructionType::VMulF32 || !arithmetic.src[0].absolute || !arithmetic.src[0].negate ||
		    arithmetic.src[0].register_id != 16 || !VertexFloatArithmeticSgprSourceSupported(arithmetic, 0)) return false;
		const char* reason = nullptr;
		uint32_t pc = UINT32_MAX;
		return !ShaderVertexResourceProfileSupported(code, bound, &reason, &pc) && reason != nullptr &&
		       std::strcmp(reason, "vertex undefined SGPR, fetch effect, or raw descriptor read") == 0 && pc == arithmetic.pc;
	};
	auto fetch_input = input;
	fetch_input.fetch_external = true;
	fetch_input.fetch_effects_valid = true;
	fetch_input.fetch_written_sgprs[0] = uint64_t {15} << 16;
	uint32_t fetch_words[12] {
	    0xbeeb03ffu, 0u, 0xbe802100u, // VCC_HI initialization and canonical external FETCH call
	    0xbe9003ffu, 0x3f800000u, multiply_word0, multiply_word1, 0xbf810000u};
	ShaderCode restored_fetch;
	if (!parse_integration(fetch_words, 12, 8, &restored_fetch) || restored_fetch.GetInstructions().Size() != 5 ||
	    !ShaderVertexResourceProfileSupported(restored_fetch, fetch_input)) return false;
	// Defining an unrelated lane cannot make the omitted FETCH-written s16
	// available. The source helper itself passes, so the rejection is defined.
	fetch_words[3] = 0xbea003ffu; // s32, instead of the ordinary s16 redefinition
	ShaderCode omitted_fetch;
	if (!parse_integration(fetch_words, 12, 8, &omitted_fetch) || !provenance_rejected(omitted_fetch, fetch_input, 3)) return false;
	// Reloading a complete declared descriptor after FETCH proves s16 defined,
	// but retains host-word origin. An ordinary MOV after that reload is the
	// positive control that discharges origin before the modified float read.
	fetch_input.bind.samplers.samplers_num = 1;
	fetch_input.bind.samplers.start_register[0] = 16;
	fetch_input.bind.samplers.extended[0] = true;
	uint32_t descriptor_words[12] {
	    0xbeeb03ffu, 0u, 0xbe802100u,
	    0xc0000000u | (2u << 22u) | (16u << 15u) | (3u << 9u) | (1u << 8u), // S_LOAD_DWORDX4 s[16:19],s[6:7],0
	    0xbe9003ffu, 0x3f800000u, multiply_word0, multiply_word1, 0xbf810000u};
	ShaderCode restored_descriptor;
	if (!parse_integration(descriptor_words, 12, 9, &restored_descriptor) || restored_descriptor.GetInstructions().Size() != 6 ||
	    !ShaderVertexResourceProfileSupported(restored_descriptor, fetch_input)) return false;
	descriptor_words[4] = 0xbea003ffu;
	ShaderCode descriptor_fetch;
	if (!parse_integration(descriptor_words, 12, 9, &descriptor_fetch) || !provenance_rejected(descriptor_fetch, fetch_input, 4)) return false;
	for (const uint32_t condition: {4u, 5u}) // SCC==0 and SCC==1: preserve both parser branch forms.
	{
		uint32_t fork_words[12] {
		    0xbe9003ffu, 0x3f800000u, 0xbf800002u | (condition << 16u),
		    0xbe9003ffu, 0x40000000u, multiply_word0, multiply_word1, 0xbf810000u};
		ShaderCode dominating;
		if (!parse_integration(fork_words, 12, 8, &dominating) || dominating.GetInstructions().Size() != 5 ||
		    !ShaderVertexResourceProfileSupported(dominating, input)) return false;
		fork_words[0] = 0xbea003ffu; // s16 is now defined only in the fallthrough arm.
		ShaderCode partial;
		if (!parse_integration(fork_words, 12, 8, &partial) || partial.GetLabels().Size() != 1 ||
		    partial.GetIndirectLabels().Size() != 1 || partial.GetLabels().At(0).GetSrc() != 8 ||
		    partial.GetLabels().At(0).GetDst() != 20 || partial.GetIndirectLabels().At(0).GetSrc() != 8 ||
		    partial.GetIndirectLabels().At(0).GetDst() != 12 || !VertexForwardPathsReach(partial, 12, 20) ||
		    !provenance_rejected(partial, input, 3)) return false;
		const auto& conditional = partial.GetInstructions().At(1);
		if (conditional.type != (condition == 4 ? ShaderInstructionType::SCbranchScc0 : ShaderInstructionType::SCbranchScc1) ||
		    conditional.src[0].constant.i != 8) return false;
		for (const uint32_t scc: {0u, 1u})
		{
			const bool taken = condition == 4 ? scc == 0 : scc == 1;
			const uint32_t next_pc = taken ? conditional.pc + 4u + conditional.src[0].constant.i : conditional.pc + 4u;
			// Each condition selects one bypass and one defining arm; both
			// routes reach the same read, rather than an unsupported backedge.
			if (next_pc != (taken ? 20u : 12u) || (taken ? next_pc : next_pc + 8u) != 20u) return false;
		}
	}
	std::fprintf(stderr, "[GFX_VERTEX_SGPR_FLOAT_SOURCE_TEST] modules=%u opcode-classes=12 abs-before-neg=pass source-temporaries=unique"
	                     " bounds=reject origin=ordinary-only canonical-ir=pass destination-modifiers=preserved"
	                     " fetch-clobber=reject descriptor-reload=reject partial-scc-join=reject ordinary-redefinition=pass\n", modules);
	return modules == 448;
}

static bool ShaderVertexFetchTraceEntryContractTest()
{
	const auto sgpr = [](int r, int size = 1) { ShaderOperand op; op.type = ShaderOperandType::Sgpr; op.register_id = r; op.size = size; return op; };
	const auto literal = [](uint32_t value) { ShaderOperand op; op.type = ShaderOperandType::LiteralConstant; op.constant.u = value; return op; };
	const auto move = [&](int r, uint32_t value) {
		ShaderInstruction inst; inst.type = ShaderInstructionType::SMovB32; inst.format = ShaderInstructionFormat::SVdstSVsrc0;
		inst.dst = sgpr(r); inst.src_num = 1; inst.src[0] = literal(value); return inst;
	};
	const auto append = [](ShaderCode& code, ShaderInstruction inst) { inst.pc = code.GetInstructions().Size() * 4; code.GetInstructions().Add(inst); };
	ShaderInstruction trace; trace.type = ShaderInstructionType::STTraceData; trace.format = ShaderInstructionFormat::Imm;
	trace.src_num = 1; trace.src[0] = literal(0);
	auto header = move(0, 0); header.dst.type = ShaderOperandType::VccHi;
	ShaderInstruction call; call.type = ShaderInstructionType::SSwappcB64; call.format = ShaderInstructionFormat::Sdst2Ssrc02;
	call.dst = sgpr(0, 2); call.src_num = 1; call.src[0] = sgpr(0, 2);
	ShaderInstruction end; end.type = ShaderInstructionType::SEndpgm; end.format = ShaderInstructionFormat::Empty;
	ShaderCode baseline; baseline.SetType(ShaderType::Vertex); baseline.SetThreadTraceMode(ShaderThreadTraceMode::Disabled);
	baseline.SetFp32Mode(ShaderFp32Mode::RneFlushInOut);
	append(baseline, header); append(baseline, call); append(baseline, end);
	ShaderCode traced; traced.SetType(ShaderType::Vertex); traced.SetThreadTraceMode(ShaderThreadTraceMode::Disabled);
	traced.SetFp32Mode(ShaderFp32Mode::RneFlushInOut);
	append(traced, trace); append(traced, header); append(traced, trace); append(traced, call); append(traced, end);
	ShaderVertexInputInfo input {};
	input.fetch_external = true; input.fetch_effects_valid = true; input.fetch_written_sgprs[0] = uint64_t{15} << 16;
	input.resources_num = 1;
	input.resources[0].fields[1] = 16u << 16u;
	input.resources[0].fields[2] = 4;
	input.resources[0].fields[3] = DstSel(4, 5, 6, 7) | (7u << 12u) | (14u << 15u);
	input.resources_dst[0] = {4, 4};
	uint32_t call_index = UINT32_MAX;
	if (!VertexExternalFetchEntryCall(traced, input, &call_index) || call_index != 3 ||
	    !ShaderVertexResourceProfileSupported(traced, input)) return false;
	const auto baseline_module = ShaderRecompileVS(baseline, &input);
	const auto traced_module = ShaderRecompileVS(traced, &input);
	const auto opcodes = [](const Vector<uint32_t>& module, std::vector<uint16_t>* result) {
		if (module.Size() < 5 || module.At(0) != 0x07230203u) return false;
		for (uint32_t word = 5; word < module.Size();)
		{
			const uint32_t count = module.At(word) >> 16;
			if (count == 0 || count > module.Size() - word) return false;
			result->push_back(static_cast<uint16_t>(module.At(word) & 0xffffu));
			word += count;
		}
		return true;
	};
	std::vector<uint16_t> baseline_ops, traced_ops;
	if (!opcodes(baseline_module, &baseline_ops) || !opcodes(traced_module, &traced_ops) || baseline_ops != traced_ops) return false;
	// Original synthetic ISA words exercise both literal widths and the real
	// PC of the moved call. Arbitrary immediate values are carried through
	// the ordinary MOV lowerings; no guest address or title value is selected.
	for (const auto& immediates: {std::array<uint32_t, 2> {0x76543210u, 0x89abcdefu},
	                             std::array<uint32_t, 2> {0u, 0xffffffffu}})
	{
		// The seven-word program is complete without padding. Removing END
		// remains a real truncation; extra owned padding is never executed.
		const uint32_t words[11] = {0xbeeb03ffu, immediates[0], 0xbefc03ffu, immediates[1],
		                            0xbf960000u, 0xbe802100u, 0xbf810000u};
		ShaderCode parsed; parsed.SetType(ShaderType::Vertex); parsed.SetThreadTraceMode(ShaderThreadTraceMode::Disabled);
		parsed.SetFp32Mode(ShaderFp32Mode::RneFlushInOut);
		if (ShaderTryParseBounded(words, 6, &parsed) != 0 || ShaderTryParseBounded(words, 7, &parsed) != 7 ||
		    ShaderTryParseBounded(words, 11, &parsed) != 7 ||
		    parsed.GetInstructions().Size() != 5 ||
		    parsed.GetInstructions().At(3).pc != 20 || !VertexExternalFetchEntryCall(parsed, input, &call_index) ||
		    call_index != 3 || !ShaderVertexResourceProfileSupported(parsed, input)) return false;
		const auto source = SpirvGenerateSource(parsed, &input, nullptr, nullptr);
		const std::string_view text(source.c_str(), source.Size());
		const auto vcc_store = text.find("OpStore %vcc_hi %t0_0");
		const auto m0_store = text.find("OpStore %m0 %t0_1");
		const auto attribute_load = text.find("%t1_0_3 = OpLoad %v4float %attr0");
		if (vcc_store == std::string_view::npos || m0_store == std::string_view::npos || attribute_load == std::string_view::npos ||
		    vcc_store >= m0_store || m0_store >= attribute_load || ShaderRecompileVS(parsed, &input).IsEmpty()) return false;
		for (const auto wrong_destination: {ShaderOperandType::Sgpr, ShaderOperandType::Vgpr, ShaderOperandType::ExecLo})
		{
			auto bad = parsed; bad.GetInstructions()[1].dst.type = wrong_destination;
			if (ShaderVertexResourceProfileSupported(bad, input)) return false;
		}
	}
	for (int variant = 0; variant < 24; ++variant)
	{
		auto bad = traced; auto bad_input = input;
		if (variant == 0) bad.SetThreadTraceMode(ShaderThreadTraceMode::Unknown);
		if (variant == 1) bad.SetThreadTraceMode(ShaderThreadTraceMode::Enabled);
		if (variant == 2) bad.GetInstructions()[0].src[0] = literal(1);
		if (variant == 3) bad.GetInstructions()[0].src_num = 2;
		if (variant == 4) bad.GetInstructions()[0].type = ShaderInstructionType::SWaitcnt;
		if (variant == 5) bad.GetInstructions()[1].src[0] = sgpr(6);
		if (variant == 6) bad.GetInstructions()[1].dst = sgpr(0);
		if (variant == 7) bad.GetInstructions()[1] = trace;
		if (variant == 8) bad.GetInstructions()[3].dst = sgpr(2, 2);
		if (variant == 9) bad.GetInstructions()[3].src[0] = sgpr(2, 2);
		if (variant == 10) bad.GetInstructions()[3].src[0].negate = true;
		if (variant == 11) bad_input.fetch_effects_valid = false;
		if (variant == 12) { bad.GetInstructions()[4] = call; append(bad, end); }
		if (variant == 13)
		{
			ShaderInstruction branch; branch.type = ShaderInstructionType::SBranch; branch.format = ShaderInstructionFormat::Label;
			branch.pc = 16; branch.src_num = 1; branch.src[0] = literal(static_cast<uint32_t>(-8));
			bad.GetInstructions()[4] = branch; append(bad, end); bad.GetLabels().Add(ShaderLabel(12, 16));
		}
		if (variant == 14) bad.GetIndirectLabels().Add(ShaderLabel(8, 16));
		if (variant == 15) bad_input.fetch_shader_reg = 2;
		if (variant == 16) bad.SetType(ShaderType::Pixel);
		if (variant == 17)
		{
			ShaderInstruction branch; branch.type = ShaderInstructionType::SBranch; branch.format = ShaderInstructionFormat::Label;
			branch.src_num = 1; branch.src[0] = literal(12); bad.GetInstructions()[0] = branch;
			bad.GetLabels().Add(ShaderLabel(16, 0));
		}
		if (variant == 18) bad.GetInstructions()[3].src[1] = sgpr(6);
		if (variant == 19) bad.GetInstructions()[3].dst2 = sgpr(8);
		if (variant == 20) bad.GetInstructions()[3].dst.constant.u = 42;
		if (variant == 21) bad.GetInstructions()[3].globally_coherent = true;
		if (variant == 22) bad.GetInstructions()[3].src[0].constant.u = 42;
		if (variant == 23) bad.GetInstructions()[3].format = ShaderInstructionFormat::Saddr;
		if (ShaderVertexResourceProfileSupported(bad, bad_input)) return false;
	}
	for (const int read_register: {0, 1, 16, 19})
	{
		auto bad = traced;
		auto read = move(32, 0); read.src[0] = sgpr(read_register);
		bad.GetInstructions().RemoveAt(bad.GetInstructions().Size() - 1); append(bad, read); append(bad, end);
		if (ShaderVertexResourceProfileSupported(bad, input)) return false;
		auto restored = traced;
		restored.GetInstructions().RemoveAt(restored.GetInstructions().Size() - 1); append(restored, move(read_register, 0)); append(restored, read); append(restored, end);
		if (!ShaderVertexResourceProfileSupported(restored, input)) return false;
	}
	{
		auto bad = traced;
		bad.GetInstructions().RemoveAt(bad.GetInstructions().Size() - 1);
		ShaderInstruction multiply; multiply.type = ShaderInstructionType::SMulkI32; multiply.format = ShaderInstructionFormat::SVdstSVsrc0;
		multiply.dst = sgpr(16); multiply.src_num = 1; multiply.src[0].type = ShaderOperandType::IntegerInlineConstant;
		multiply.src[0].constant.i = 2; append(bad, multiply); append(bad, end);
		if (ShaderVertexResourceProfileSupported(bad, input)) return false;
	}
	{
		auto bad = traced;
		bad.GetInstructions().RemoveAt(bad.GetInstructions().Size() - 1);
		ShaderInstruction branch; branch.type = ShaderInstructionType::SCbranchScc0; branch.format = ShaderInstructionFormat::Label;
		branch.src_num = 1; branch.src[0] = literal(4); append(bad, branch); append(bad, move(16, 0));
		auto read = move(32, 0); read.src[0] = sgpr(16); append(bad, read); append(bad, end);
		bad.GetLabels().Add(ShaderLabel(24, 16)); bad.GetIndirectLabels().Add(ShaderLabel(20, 16));
		if (ShaderVertexResourceProfileSupported(bad, input)) return false;
	}
	{
		auto bad = traced;
		bad.GetInstructions().RemoveAt(bad.GetInstructions().Size() - 1);
		ShaderInstruction compare; compare.type = ShaderInstructionType::VCmpEqF32; compare.dst = sgpr(16, 2);
		compare.src_num = 2; compare.src[0].type = compare.src[1].type = ShaderOperandType::Vgpr;
		compare.src[0].size = compare.src[1].size = 1; append(bad, compare);
		auto read = move(32, 0); read.src[0] = sgpr(16); append(bad, read); append(bad, end);
		if (ShaderVertexResourceProfileSupported(bad, input)) return false;
	}
	for (const auto alias: {ShaderOperandType::VccLo, ShaderOperandType::VccHi})
	{
		auto bad = traced;
		bad.GetInstructions().RemoveAt(bad.GetInstructions().Size() - 1);
		auto read = move(32, 0); read.src[0] = sgpr(0); read.src[0].type = alias;
		append(bad, read); append(bad, end);
		if (ShaderVertexResourceProfileSupported(bad, input)) return false;
		bad = traced;
		bad.GetInstructions().RemoveAt(bad.GetInstructions().Size() - 1);
		auto write = move(0, 0); write.dst.type = alias; append(bad, write); append(bad, end);
		if (ShaderVertexResourceProfileSupported(bad, input)) return false;
	}
	for (const auto branch_type: {ShaderInstructionType::SCbranchVccz, ShaderInstructionType::SCbranchVccnz})
	{
		auto bad = traced;
		bad.GetInstructions().RemoveAt(bad.GetInstructions().Size() - 1);
		ShaderInstruction branch; branch.type = branch_type; branch.format = ShaderInstructionFormat::Label;
		branch.src_num = 1; branch.src[0] = literal(0); append(bad, branch); append(bad, end);
		bad.GetLabels().Add(ShaderLabel(20, 16)); bad.GetIndirectLabels().Add(ShaderLabel(20, 16));
		if (ShaderVertexResourceProfileSupported(bad, input)) return false;
	}
	std::fprintf(stderr, "[GFX_VERTEX_FETCH_TRACE_ENTRY_TEST] bounded-prefix=pass literal-pc=20 moves=preserved scalar-effects=dead-or-reject"
	                     " return-pair=dead-or-reject branch-bypass=reject vcc-alias=reject spirv-opcodes=unchanged\n");
	return true;
}

static bool ShaderVertexResourceContractTest()
{
	if (!ShaderVertexFetchTraceEntryContractTest() || !ShaderVertexFloatSgprSourceContractTest()) return false;
	const auto sgpr = [](int r, int size = 1) { ShaderOperand op; op.type = ShaderOperandType::Sgpr; op.register_id = r; op.size = size; return op; };
	const auto vgpr = [](int r, int size = 1) { ShaderOperand op; op.type = ShaderOperandType::Vgpr; op.register_id = r; op.size = size; return op; };
	const auto literal = [](uint32_t value) { ShaderOperand op; op.type = ShaderOperandType::LiteralConstant; op.constant.u = value; return op; };
	const auto move = [&](int r, uint32_t value) {
		ShaderInstruction inst; inst.type = ShaderInstructionType::SMovB32; inst.format = ShaderInstructionFormat::SVdstSVsrc0;
		inst.dst = sgpr(r); inst.src_num = 1; inst.src[0] = literal(value); return inst;
	};
	const auto load = [&](int r, int base, uint32_t offset) {
		ShaderInstruction inst; inst.type = ShaderInstructionType::SLoadDwordx4; inst.format = ShaderInstructionFormat::Sdst4SbaseSoffset;
		inst.dst = sgpr(r, 4); inst.src_num = 2; inst.src[0] = sgpr(base, 2); inst.src[1] = literal(offset); return inst;
	};
	const auto append = [](ShaderCode& code, ShaderInstruction inst) { inst.pc = code.GetInstructions().Size() * 4; code.GetInstructions().Add(inst); };
	ShaderCode fetch; fetch.SetType(ShaderType::Fetch);
	append(fetch, load(16, 2, 0));
	ShaderInstruction attribute; attribute.type = ShaderInstructionType::BufferLoadFormatXyzw;
	attribute.format = ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen; attribute.dst = vgpr(4, 4);
	attribute.src_num = 3; attribute.src[0] = vgpr(0); attribute.src[1] = sgpr(16, 4);
	attribute.src[2].type = ShaderOperandType::IntegerInlineConstant;
	append(fetch, attribute);
	ShaderInstruction ret; ret.type = ShaderInstructionType::SSetpcB64; ret.src_num = 1; ret.src[0] = sgpr(0, 2); append(fetch, ret);
	uint64_t writes[2] = {};
	if (!ShaderVertexFetchEffectsSupported(fetch, 0, 2, writes) || writes[0] != (uint64_t{15} << 16) || writes[1] != 0) return false;
	for (int variant = 0; variant < 5; ++variant)
	{
		auto bad = fetch;
		if (variant == 0) bad.GetInstructions()[0].src[1] = literal(4096);
		if (variant == 1) bad.GetInstructions()[0].dst = sgpr(0, 4);
		if (variant == 2) bad.GetInstructions()[1].dst = vgpr(0, 4);
		if (variant == 3) bad.GetInstructions()[1].type = ShaderInstructionType::BufferStoreDword;
		if (variant == 4) bad.GetInstructions()[2].src[0] = sgpr(4, 2);
		if (ShaderVertexFetchEffectsSupported(bad, 0, 2, writes)) return false;
	}
	ShaderVertexInputInfo input;
	input.bind.extended.used = true; input.bind.extended.start_register = 6;
	input.bind.storage_buffers.buffers_num = 1; input.bind.storage_buffers.start_register[0] = 20;
	input.bind.storage_buffers.extended[0] = true; input.bind.storage_buffers.usages[0] = ShaderStorageUsage::Constant;
	input.bind.samplers.samplers_num = 1; input.bind.samplers.start_register[0] = 16; input.bind.samplers.extended[0] = true;
	input.bind.textures2D.textures_num = 1; input.bind.textures2D.textures2d_sampled_num = 1;
	input.bind.textures2D.desc[0].start_register = 8; input.bind.textures2D.desc[0].usage = ShaderTextureUsage::ReadOnly;
	input.bind.textures2D.desc[0].texture.fields[3] = 9u << 28; // ordinary 2D dimension
	ShaderCode code; code.SetType(ShaderType::Vertex);
	append(code, load(16, 6, 16));
	ShaderInstruction read; read.type = ShaderInstructionType::SBufferLoadDwordx4; read.format = ShaderInstructionFormat::Sdst4SvSoffset;
	read.dst = sgpr(16, 4); read.src_num = 2; read.src[0] = sgpr(16, 4); read.src[1] = literal(0); append(code, read);
	append(code, load(0, 6, 0));
	ShaderInstruction sample; sample.type = ShaderInstructionType::ImageSampleLz;
	sample.format = ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7; sample.dst = vgpr(1, 3); sample.src_num = 3;
	sample.src[0] = vgpr(1, 3); sample.src[1] = sgpr(8, 8); sample.src[2] = sgpr(0, 4); append(code, sample);
	append(code, move(6, 0)); // The pointer is dead after the last SLoad.
	ShaderInstruction end; end.type = ShaderInstructionType::SEndpgm; append(code, end);
	if (!ShaderVertexResourceProfileSupported(code, input)) return false;
	for (int variant = 0; variant < 12; ++variant)
	{
		auto bad = code; auto bad_input = input;
		if (variant == 0) bad.GetInstructions()[0].src[1] = literal(18);
		if (variant == 1) bad.GetInstructions()[0].src[1] = literal(32); // unmapped hole
		if (variant == 2) bad.GetInstructions()[1].dst = sgpr(4, 4); // clobbers pointer before sampler load
		if (variant == 3) bad.GetInstructions()[3].src[1] = sgpr(16, 8);
		if (variant == 4) bad.GetInstructions()[3].type = ShaderInstructionType::ImageSample;
		if (variant == 5) bad.GetInstructions()[3].unnormalized_coordinates = true;
		if (variant == 6) bad.GetInstructions()[1].type = ShaderInstructionType::BufferStoreDword;
		if (variant == 7) bad_input.bind.storage_buffers.usages[0] = ShaderStorageUsage::ReadWrite;
		if (variant == 8) bad_input.rect_list = true;
		if (variant == 9) { auto scalar = move(32, 0); scalar.pc = 16; scalar.src[0] = sgpr(8); bad.GetInstructions()[4] = scalar; }
		if (variant == 10) bad.GetInstructions()[3].src[2] = sgpr(4, 4);
		if (variant == 11) bad.GetInstructions()[4].dst.type = ShaderOperandType::ExecLo;
		if (ShaderVertexResourceProfileSupported(bad, bad_input)) return false;
	}
	// A conditional path that skips the only definition must not inherit the
	// linear scan's descriptor. Its synthetic indirect label is static fallthrough.
	ShaderCode fork; fork.SetType(ShaderType::Vertex);
	ShaderInstruction branch; branch.type = ShaderInstructionType::SCbranchVccz; branch.format = ShaderInstructionFormat::Label;
	branch.src_num = 1; branch.src[0] = literal(4); append(fork, branch);
	append(fork, load(0, 6, 0)); append(fork, sample); append(fork, end);
	fork.GetLabels().Add(ShaderLabel(8, 0)); fork.GetIndirectLabels().Add(ShaderLabel(4, 0));
	if (ShaderVertexResourceProfileSupported(fork, input)) return false;
	ShaderCode common; common.SetType(ShaderType::Vertex);
	append(common, load(0, 6, 0)); append(common, branch);
	ShaderInstruction wait; wait.type = ShaderInstructionType::SWaitcnt; append(common, wait);
	append(common, sample); append(common, end);
	common.GetLabels().Add(ShaderLabel(12, 4)); common.GetIndirectLabels().Add(ShaderLabel(8, 4));
	if (!ShaderVertexResourceProfileSupported(common, input)) return false;
	common.GetInstructions()[1].src[0].constant.i = -8;
	if (ShaderVertexResourceProfileSupported(common, input)) return false;
	// A masked comparison may leave the old host descriptor word intact.
	ShaderCode masked; masked.SetType(ShaderType::Vertex);
	ShaderInstruction compare; compare.type = ShaderInstructionType::VCmpEqF32; compare.dst = sgpr(8, 2);
	compare.src_num = 2; compare.src[0] = vgpr(0); compare.src[1] = vgpr(1); append(masked, compare);
	auto consume_scalar = move(32, 0); consume_scalar.src[0] = sgpr(8); append(masked, consume_scalar); append(masked, end);
	if (ShaderVertexResourceProfileSupported(masked, input)) return false;
	ShaderCode mixed; mixed.SetType(ShaderType::Vertex);
	append(mixed, branch); append(mixed, move(8, 0)); append(mixed, consume_scalar); append(mixed, end);
	mixed.GetLabels().Add(ShaderLabel(8, 0)); mixed.GetIndirectLabels().Add(ShaderLabel(4, 0));
	if (ShaderVertexResourceProfileSupported(mixed, input)) return false;
	// Fetch effects are not modeled as scalar execution: no read may observe
	// an omitted SGPR write, even when a module with the same hash was cached.
	ShaderCode with_fetch; with_fetch.SetType(ShaderType::Vertex);
	auto header = move(0, 0); header.dst.type = ShaderOperandType::VccHi; append(with_fetch, header);
	ShaderInstruction call; call.type = ShaderInstructionType::SSwappcB64; call.format = ShaderInstructionFormat::Sdst2Ssrc02;
	call.dst = sgpr(0, 2); call.src_num = 1; call.src[0] = sgpr(0, 2); append(with_fetch, call);
	for (const auto& inst: code.GetInstructions()) append(with_fetch, inst);
	auto fetch_input = input; fetch_input.fetch_external = true; fetch_input.fetch_effects_valid = true;
	fetch_input.fetch_written_sgprs[0] = uint64_t{15} << 16;
	if (!ShaderVertexResourceProfileSupported(with_fetch, fetch_input)) return false;
	with_fetch.GetInstructions()[2] = move(32, 0); with_fetch.GetInstructions()[2].pc = 8;
	if (ShaderVertexResourceProfileSupported(with_fetch, fetch_input)) return false;
	// Three alternatives share one join. The second condition is not the
	// outer merge: taking the first arm bypasses it entirely.
	ShaderCode chain; chain.SetType(ShaderType::Vertex);
	auto outer = branch; outer.src[0] = literal(8); append(chain, outer); // 0 -> 12
	append(chain, move(32, 1));
	auto jump = branch; jump.type = ShaderInstructionType::SBranch; jump.src[0] = literal(20); append(chain, jump); // 8 -> 32
	auto inner = branch; inner.src[0] = literal(8); append(chain, inner); // 12 -> 24
	append(chain, move(32, 2)); jump.src[0] = literal(8); append(chain, jump); // 20 -> 32
	append(chain, move(32, 3)); append(chain, wait); append(chain, end);
	if (!VertexForwardPathsReach(chain, 12, 32) || VertexForwardPathsReach(chain, 12, 24)) return false;
	for (int outer_taken = 0; outer_taken < 2; ++outer_taken)
		for (int inner_taken = 0; inner_taken < 2; ++inner_taken)
		{
			uint32_t pc = 0; uint32_t result = 0; int steps = 0;
			while (pc != 32 && ++steps < 10)
			{
				const auto& inst = chain.GetInstructions().At(pc / 4);
				const bool take = inst.type == ShaderInstructionType::SBranch ||
				                  (inst.type == ShaderInstructionType::SCbranchVccz && (pc == 0 ? outer_taken : inner_taken));
				if (inst.type == ShaderInstructionType::SMovB32) result = inst.src[0].constant.u;
				pc = take ? pc + 4 + inst.src[0].constant.i : pc + 4;
			}
			if (pc != 32 || result != static_cast<uint32_t>(!outer_taken ? 1 : !inner_taken ? 2 : 3)) return false;
		}
	auto early_end = chain; early_end.GetInstructions()[6].type = ShaderInstructionType::SEndpgm;
	if (VertexForwardPathsReach(early_end, 12, 32)) return false;
	auto backedge = chain; backedge.GetInstructions()[5].src[0].constant.i = -12;
	if (VertexForwardPathsReach(backedge, 12, 32)) return false;

	// Signed 16-bit vectors enter the guest's typeless VGPRs as sign-extended
	// integer bit patterns. Keep an ordinary float pair beside them so the test
	// proves that only the exact signed formats change the SPIR-V interface type.
	ShaderCode typed_fetch;
	typed_fetch.SetType(ShaderType::Vertex);
	typed_fetch.SetFp32Mode(ShaderFp32Mode::RneFlushInOut);
	auto typed_wait   = wait;
	typed_wait.format = ShaderInstructionFormat::Imm;
	typed_wait.src_num = 1;
	typed_wait.src[0]  = literal(0);
	append(typed_fetch, typed_wait);
	append(typed_fetch, call);
	auto typed_end   = end;
	typed_end.format = ShaderInstructionFormat::Empty;
	append(typed_fetch, typed_end);
	ShaderVertexInputInfo typed_input {};
	typed_input.fetch_external  = true;
	typed_input.fetch_shader_reg = 0;
	typed_input.resources_num    = 4;
	typed_input.resources[0].fields[1] = 32u << 16u;
	typed_input.resources[0].fields[2] = 4;
	typed_input.resources[0].fields[3] = DstSel(4, 5) | (5u << 12u) | (5u << 15u);
	typed_input.resources_dst[0]       = {16, 2};
	typed_input.resources[1].fields[1] = 8u << 16u;
	typed_input.resources[1].fields[2] = 4;
	typed_input.resources[1].fields[3] = DstSel(4, 5) | (7u << 12u) | (11u << 15u);
	typed_input.resources_dst[1]       = {18, 2};
	typed_input.resources[2].fields[1] = 16u << 16u;
	typed_input.resources[2].fields[2] = 4;
	typed_input.resources[2].fields[3] = DstSel(4, 5, 6, 7) | (5u << 12u) | (12u << 15u);
	typed_input.resources_dst[2]       = {20, 4};
	typed_input.resources[3].fields[1] = 32u << 16u;
	typed_input.resources[3].fields[2] = 4;
	typed_input.resources[3].fields[3] = DstSel(4, 5, 6, 7) | (1u << 12u) | (10u << 15u);
	typed_input.resources_dst[3]       = {24, 3};
	if (!ShaderVertexInputSint16x2Compatible(typed_input.resources[0], 2) ||
	    ShaderVertexInputSint16x2Compatible(typed_input.resources[0], 1) ||
	    ShaderVertexInputSint16x2Compatible(typed_input.resources[1], 2) ||
	    !ShaderVertexInputSint16x4Compatible(typed_input.resources[2], 4) ||
	    ShaderVertexInputSint16x4Compatible(typed_input.resources[2], 2) ||
	    ShaderVertexInputSint16x4Compatible(typed_input.resources[1], 4) ||
	    !ShaderVertexInputSnorm8x4XyzCompatible(typed_input.resources[3], 3) ||
	    ShaderVertexInputSnorm8x4XyzCompatible(typed_input.resources[3], 4))
	{
		return false;
	}
	const auto             typed_source = SpirvGenerateSource(typed_fetch, &typed_input, nullptr, nullptr);
	const std::string_view typed_text(typed_source.c_str(), typed_source.Size());
	if (typed_text.find("%attr0 = OpVariable %_ptr_Input_v2int Input") == std::string_view::npos ||
	    typed_text.find("%t1_0_1 = OpLoad %v2int %attr0") == std::string_view::npos ||
	    typed_text.find("%t2_0_1 = OpBitcast %v2float %t1_0_1") == std::string_view::npos ||
	    typed_text.find("%attr1 = OpVariable %_ptr_Input_v2float Input") == std::string_view::npos ||
	    typed_text.find("%t1_1_1 = OpLoad %v2float %attr1") == std::string_view::npos ||
	    typed_text.find("%attr2 = OpVariable %_ptr_Input_v4int Input") == std::string_view::npos ||
	    typed_text.find("%t1_2_1 = OpLoad %v4int %attr2") == std::string_view::npos ||
	    typed_text.find("%t2_2_1 = OpBitcast %v4float %t1_2_1") == std::string_view::npos ||
	    typed_text.find("%t3_2_1 = OpFunctionCall %void %fetch_f1_f1_f1_f1_vf4_") == std::string_view::npos ||
	    typed_text.find("%attr3 = OpVariable %_ptr_Input_v3float Input") == std::string_view::npos ||
	    typed_text.find("%t1_3_1 = OpLoad %v3float %attr3") == std::string_view::npos ||
	    typed_text.find("%t2_3_1 = OpFunctionCall %void %fetch_f1_f1_f1_vf3_") == std::string_view::npos ||
	    typed_text.find("OpLoad %v2float %attr0") != std::string_view::npos ||
	    typed_text.find("OpBitcast %v2float %t1_1_1") != std::string_view::npos ||
	    typed_text.find("OpLoad %v4float %attr2") != std::string_view::npos ||
	    typed_text.find("OpLoad %v4float %attr3") != std::string_view::npos ||
	    typed_text.find("OpBitcast %v3float %t1_3_1") != std::string_view::npos)
	{
		return false;
	}
	const auto typed_module = ShaderRecompileVS(typed_fetch, &typed_input);
	spvtools::SpirvTools typed_validator(SPV_ENV_VULKAN_1_1);
	if (typed_module.Size() < 5 || typed_module.At(0) != 0x07230203u ||
	    !typed_validator.Validate(typed_module.GetDataConst(), typed_module.Size()))
	{
		return false;
	}
	// Original descriptor fixtures: permutation, repeated lanes and typed
	// constants must be selected from the original four-component attribute.
	struct SwizzleCase { uint32_t selectors; const char* lanes; };
	const SwizzleCase swizzles[] {
	    {DstSel(4, 5, 6, 7), nullptr}, {DstSel(7, 6, 5, 4), "3 2 1 0"},
	    {DstSel(6, 4, 6, 5), "2 0 2 1"}, {DstSel(0, 1, 4, 7), "4 5 0 3"},
	    {DstSel(1, 0, 1, 0), "5 4 5 4"}, {DstSel(5, 5, 5, 5), "1 1 1 1"}};
	for (const auto nfmt: {0u, 7u})
	{
		const uint32_t dfmt = nfmt == 0 ? 10u : 14u;
		for (const auto& fixture: swizzles)
		{
			auto input = typed_input;
			input.resources_num = 5;
			auto& resource = input.resources[4];
			resource.fields[1] = 16u << 16u;
			resource.fields[2] = 4;
			resource.fields[3] = fixture.selectors | (nfmt << 12u) | (dfmt << 15u);
			input.resources_dst[4] = {28, 4};
			if (!ShaderVertexInputFloat4SwizzleCompatible(resource, 4) ||
			    ShaderVertexInputFloat4SwizzleCompatible(resource, 3) ||
			    ShaderVertexInputFloat4SwizzleCompatible(typed_input.resources[2], 4)) return false;
			const auto source = SpirvGenerateSource(typed_fetch, &input, nullptr, nullptr);
			const std::string_view text(source.c_str(), source.Size());
			if (text.find("%t1_4_1 = OpLoad %v4float %attr4") == std::string_view::npos) return false;
			if (fixture.lanes == nullptr)
			{
				if (text.find("swizzle_value_4_1") != std::string_view::npos) return false;
			} else
			{
				const std::string expected = std::string("OpVectorShuffle %v4float %t1_4_1 %swizzle_constants_4_1 ") + fixture.lanes;
				if (text.find(expected) == std::string_view::npos ||
				    text.find("OpStore %temp_v4float %swizzle_value_4_1") == std::string_view::npos ||
				    text.find("OpCompositeConstruct %v4float %float_0_000000 %float_1_000000 %float_0_000000 %float_0_000000") == std::string_view::npos)
					return false;
			}
			const auto module = ShaderRecompileVS(typed_fetch, &input);
			if (module.Size() < 5 || !typed_validator.Validate(module.GetDataConst(), module.Size())) return false;
			for (const auto reserved: {2u, 3u})
			{
				auto invalid = resource;
				invalid.fields[3] = (invalid.fields[3] & ~0xfffu) | DstSel(4, 5, 6, reserved);
				if (ShaderVertexInputFloat4SwizzleCompatible(invalid, 4)) return false;
			}
		}
	}
	return true;
}
#endif

static bool RectListPlainOperand(const ShaderOperand& operand)
{
	return !operand.absolute && !operand.negate && !operand.clamp && operand.multiplier == 1.0f;
}

static bool RectListWrites(const ShaderInstruction& instruction, const ShaderOperand& operand)
{
	const auto overlaps = [&](const ShaderOperand& destination) {
		return destination.type == operand.type && destination.size > 0 && operand.size > 0 &&
		       destination.register_id < operand.register_id + operand.size &&
		       operand.register_id < destination.register_id + destination.size;
	};
	return overlaps(instruction.dst) || overlaps(instruction.dst2);
}

static int RectListLastWriter(const Vector<ShaderInstruction>& instructions, int before, const ShaderOperand& operand)
{
	for (int index = before - 1; index >= 0; --index)
	{
		if (RectListWrites(instructions.At(index), operand)) return index;
	}
	return -1;
}

static bool RectListMutatesExec(const ShaderInstruction& instruction)
{
	const auto explicit_exec = [](const ShaderOperand& destination) {
		return destination.type == ShaderOperandType::ExecLo || destination.type == ShaderOperandType::ExecHi;
	};
	if (explicit_exec(instruction.dst) || explicit_exec(instruction.dst2)) return true;
	switch (instruction.type)
	{
		case ShaderInstructionType::SAndSaveexecB64:
		case ShaderInstructionType::VCmpxEqU32:
		case ShaderInstructionType::VCmpxGeI32:
		case ShaderInstructionType::VCmpxGeU32:
		case ShaderInstructionType::VCmpxGtF32:
		case ShaderInstructionType::VCmpxGtU32:
		case ShaderInstructionType::VCmpxLtF32:
		case ShaderInstructionType::VCmpxNeqF32:
		case ShaderInstructionType::VCmpxNeU32: return true;
		default: return false;
	}
}

bool ShaderVertexParameterExportsSupported(const ShaderCode& code, const ShaderVertexInputInfo& input,
                                          const char** rejection_reason, uint32_t* rejection_pc,
                                          uint32_t* parameter_export_mask)
{
	if (parameter_export_mask != nullptr) *parameter_export_mask = 0;
	const auto reject = [&](const char* reason, uint32_t pc = UINT32_MAX) {
		if (rejection_reason != nullptr) *rejection_reason = reason;
		if (rejection_pc != nullptr) *rejection_pc = pc;
		return false;
	};
	if (input.export_count < 0 || input.export_count > 32) return reject("parameter export count");
	bool added_profile = input.export_count > 5;
	for (const auto& inst: code.GetInstructions())
		added_profile |= inst.format == ShaderInstructionFormat::ParamVsrc0Vsrc1Vsrc2Vsrc3;
	if (!added_profile) return true; // Preserve the established PARAM0..4 path.
	if (code.GetType() != ShaderType::Vertex || input.rect_list) return reject("parameter export stage or RectList");
	uint32_t produced = 0;
	for (const auto& inst: code.GetInstructions())
	{
		if (RectListMutatesExec(inst)) return reject("parameter export profile changes EXEC", inst.pc);
		bool parameter_format = false;
		switch (inst.format)
		{
			case ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3:
			case ShaderInstructionFormat::Param1Vsrc0Vsrc1Vsrc2Vsrc3:
			case ShaderInstructionFormat::Param2Vsrc0Vsrc1Vsrc2Vsrc3:
			case ShaderInstructionFormat::Param3Vsrc0Vsrc1Vsrc2Vsrc3:
			case ShaderInstructionFormat::Param4Vsrc0Vsrc1Vsrc2Vsrc3:
			case ShaderInstructionFormat::ParamVsrc0Vsrc1Vsrc2Vsrc3: parameter_format = true; break;
			default: break;
		}
		if (!parameter_format) continue;
		const int parameter = ShaderGetParameterExportIndex(inst);
		if (parameter < 0 || parameter >= input.export_count || inst.src_num != 4 || inst.export_done || inst.export_valid_mask)
			return reject("parameter export target, flags or width", inst.pc);
		for (const auto& source: inst.src)
			if (source.type != ShaderOperandType::Vgpr || source.size != 1 || source.register_id < 0 || source.register_id > 255 ||
			    !RectListPlainOperand(source)) return reject("parameter export operand", inst.pc);
		// Only shifts 0..31 occur. No full-count (1<<32) mask is required.
		produced |= uint32_t{1} << parameter;
	}
	if (parameter_export_mask != nullptr) *parameter_export_mask = produced;
	return true;
}

static bool RectListExecActiveBefore(const Vector<ShaderInstruction>& instructions, int before, int common_tail)
{
	int mutation = before - 1;
	while (mutation >= 0 && !RectListMutatesExec(instructions.At(mutation))) --mutation;
	if (mutation < 0) return true; // The ordinary guest prolog initializes EXEC=(1,0).
	if (mutation < common_tail) return false;
	const auto& restore = instructions.At(mutation);
	if (restore.type != ShaderInstructionType::SMovB64 || restore.format != ShaderInstructionFormat::Sdst2Ssrc02 ||
	    restore.dst.type != ShaderOperandType::ExecLo || restore.dst.size != 2 || restore.src_num != 1 ||
	    !RectListPlainOperand(restore.dst) || !RectListPlainOperand(restore.src[0])) return false;
	if (restore.src[0].type == ShaderOperandType::IntegerInlineConstant && restore.src[0].size == 2 && restore.src[0].constant.u == 1) return true;
	if (restore.src[0].type != ShaderOperandType::Sgpr || restore.src[0].size != 2) return false;
	const int saved_writer = RectListLastWriter(instructions, mutation, restore.src[0]);
	if (saved_writer < 0) return false;
	const auto& save = instructions.At(saved_writer);
	if (save.type != ShaderInstructionType::SAndSaveexecB64 || save.format != ShaderInstructionFormat::Sdst2Ssrc02 ||
	    save.dst.type != ShaderOperandType::Sgpr || save.dst.register_id != restore.src[0].register_id || save.dst.size != 2 ||
	    !RectListPlainOperand(save.dst)) return false;
	// A save in the entry prefix dominates every path and captures the known
	// initial EXEC value. LastWriter has already excluded any later lane clobber.
	for (int index = 0; index < saved_writer; ++index)
	{
		if (instructions.At(index).format == ShaderInstructionFormat::Label || RectListMutatesExec(instructions.At(index))) return false;
	}
	return true;
}

// The bounded profile has a common, unconditional output tail. Its W is one
// and its Z is constant or read from an immutable scalar constant descriptor.
// Consequently only XY needs extrapolation; preserving Z/W avoids cancellation
// or overflow of identical depth values. Attributes are completed after the VS.
bool ShaderRectListProfileSupported(const ShaderCode& code, const ShaderVertexInputInfo& input,
                                    const char** rejection_reason, uint32_t* rejection_pc)
{
	if (rejection_reason != nullptr) *rejection_reason = nullptr;
	if (rejection_pc != nullptr) *rejection_pc = UINT32_MAX;
	const auto reject = [&](const char* reason, uint32_t pc = UINT32_MAX) {
		if (rejection_reason != nullptr) *rejection_reason = reason;
		if (rejection_pc != nullptr) *rejection_pc = pc;
		return false;
	};
	if (code.GetType() != ShaderType::Vertex || code.IsVsEmbedded()) return reject("shader stage or embedded mode");
	if (!ShaderRectListInputSupported(input)) return reject("input resources or export count");
	if (!code.GetDebugPrintfs().IsEmpty()) return reject("debug output");
	const auto& instructions = code.GetInstructions();
	const int count = static_cast<int>(instructions.Size());
	if (count < 3 || instructions.At(count - 1).type != ShaderInstructionType::SEndpgm) return reject("missing final end instruction");
	// The parser calls conditional-branch fallthrough labels "indirect"; they
	// are static pc+4 edges, not SSetpc/SSwappc guest transfers.
	for (const auto& label: code.GetIndirectLabels())
	{
		bool fallthrough = false;
		for (int index = 0; index + 1 < count; ++index)
		{
			const auto& instruction = instructions.At(index);
			const bool conditional = instruction.type == ShaderInstructionType::SCbranchExecz ||
			                         instruction.type == ShaderInstructionType::SCbranchScc0 ||
			                         instruction.type == ShaderInstructionType::SCbranchScc1 ||
			                         instruction.type == ShaderInstructionType::SCbranchVccz ||
			                         instruction.type == ShaderInstructionType::SCbranchVccnz;
			if (conditional && instruction.pc == label.GetSrc() && label.GetDst() == instruction.pc + 4 &&
			    instructions.At(index + 1).pc == label.GetDst()) { fallthrough = true; break; }
		}
		if (!fallthrough) return reject("metadata is not a conditional-branch fallthrough", label.GetSrc());
	}
	int first_export = -1;
	int position_export = -1;
	int common_tail = 0;
	uint32_t parameters = 0;
	for (int index = 0; index < count; ++index)
	{
		const auto& instruction = instructions.At(index);
		if (!RectListInstructionIsPure(code, instruction)) return reject("instruction is outside replay allowlist", instruction.pc);
		if (index > 0 && instruction.pc <= instructions.At(index - 1).pc) return reject("instruction PCs are not strictly increasing", instruction.pc);
		if (instruction.type == ShaderInstructionType::SEndpgm && index != count - 1) return reject("early end instruction", instruction.pc);
		const bool branch = instruction.type == ShaderInstructionType::SBranch || instruction.type == ShaderInstructionType::SCbranchExecz ||
		                    instruction.type == ShaderInstructionType::SCbranchScc0 || instruction.type == ShaderInstructionType::SCbranchScc1 ||
		                    instruction.type == ShaderInstructionType::SCbranchVccz || instruction.type == ShaderInstructionType::SCbranchVccnz;
		if (branch != (instruction.format == ShaderInstructionFormat::Label)) return reject("branch format mismatch", instruction.pc);
		if (instruction.format == ShaderInstructionFormat::Label)
		{
			if (first_export >= 0 || !operand_is_constant(instruction.src[0])) return reject("branch in export tail or nonconstant target", instruction.pc);
			const int64_t target = static_cast<int64_t>(instruction.pc) + 4 + instruction.src[0].constant.i;
			int target_index = -1;
			for (int next = index + 1; next < count; ++next)
			{
				if (instructions.At(next).pc == target) { target_index = next; break; }
			}
			if (target_index < 0) return reject("branch target is not a later instruction", instruction.pc);
			common_tail = std::max({common_tail, index + 1, target_index});
		}
		if (instruction.type == ShaderInstructionType::Exp)
		{
			if (first_export < 0) first_export = index;
			if (instruction.src_num != 4) return reject("export is not complete float4", instruction.pc);
			for (const auto& operand: instruction.src)
			{
				if (operand.type != ShaderOperandType::Vgpr || operand.size != 1 || !RectListPlainOperand(operand)) return reject("export operand is not a plain VGPR lane", instruction.pc);
			}
			if (instruction.format == ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done)
			{
				if (position_export >= 0) return reject("duplicate position export", instruction.pc);
				position_export = index;
			} else
			{
				int parameter = -1;
				switch (instruction.format)
				{
					case ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3: parameter = 0; break;
					case ShaderInstructionFormat::Param1Vsrc0Vsrc1Vsrc2Vsrc3: parameter = 1; break;
					case ShaderInstructionFormat::Param2Vsrc0Vsrc1Vsrc2Vsrc3: parameter = 2; break;
					case ShaderInstructionFormat::Param3Vsrc0Vsrc1Vsrc2Vsrc3: parameter = 3; break;
					case ShaderInstructionFormat::Param4Vsrc0Vsrc1Vsrc2Vsrc3: parameter = 4; break;
					default: return reject("unsupported export target", instruction.pc);
				}
				if (parameter >= input.export_count || (parameters & (1u << parameter)) != 0) return reject("undeclared or duplicate parameter export", instruction.pc);
				parameters |= 1u << parameter;
			}
		} else if (first_export >= 0 && index != count - 1)
		{
			// The same reviewed replay allowlist applies before and between
			// exports. Completed outputs no longer depend on their source VGPRs;
			// W/Z are proved separately at Pos0, including intervening writers.
			// Keep every later export unconditional without another EXEC join.
			if (RectListMutatesExec(instruction)) return reject("EXEC mutation in export tail", instruction.pc);
		}
	}
	if (position_export < 0) return reject("missing position export");
	if (common_tail >= first_export) return reject("branch bypasses common output setup", instructions.At(first_export).pc);
	if (parameters != ((1u << input.export_count) - 1u)) return reject("missing declared parameter export");
	const auto& position = instructions.At(position_export);
	const int w_writer = RectListLastWriter(instructions, position_export, position.src[3]);
	const int z_writer = RectListLastWriter(instructions, position_export, position.src[2]);
	if (w_writer < common_tail) return reject("W writer does not dominate output tail", position.pc);
	if (z_writer < common_tail) return reject("Z writer does not dominate output tail", position.pc);
	if (!RectListExecActiveBefore(instructions, w_writer, common_tail)) return reject("EXEC active proof failed at W writer", instructions.At(w_writer).pc);
	if (!RectListExecActiveBefore(instructions, z_writer, common_tail)) return reject("EXEC active proof failed at Z writer", instructions.At(z_writer).pc);
	for (int index = first_export; index < count; ++index)
	{
		if (instructions.At(index).type == ShaderInstructionType::Exp && !RectListExecActiveBefore(instructions, index, common_tail))
			return reject("EXEC active proof failed at export", instructions.At(index).pc);
	}
	const auto plain_move = [](const ShaderInstruction& move, const ShaderOperand& output) {
		return move.type == ShaderInstructionType::VMovB32 && move.format == ShaderInstructionFormat::SVdstSVsrc0 &&
		       move.src_num == 1 && move.dst.type == output.type && move.dst.register_id == output.register_id &&
		       move.dst.size == 1 && RectListPlainOperand(move.dst) && RectListPlainOperand(move.src[0]);
	};
	const auto& w_move = instructions.At(w_writer);
	const auto& z_move = instructions.At(z_writer);
	if (!plain_move(w_move, position.src[3]) || !operand_is_constant(w_move.src[0]) || w_move.src[0].constant.u != 0x3f800000u)
		return reject("W is not a plain move of exactly one", w_move.pc);
	if (!plain_move(z_move, position.src[2])) return reject("Z is not a plain move", z_move.pc);
	if (operand_is_constant(z_move.src[0])) return true;
	if (z_move.src[0].type != ShaderOperandType::Sgpr || z_move.src[0].size != 1) return reject("Z source is not constant or scalar", z_move.pc);
	const int scalar_writer = RectListLastWriter(instructions, z_writer, z_move.src[0]);
	if (scalar_writer < common_tail) return reject("Z scalar load does not dominate output tail", z_move.pc);
	const auto& load = instructions.At(scalar_writer);
	if (load.type != ShaderInstructionType::SBufferLoadDword && load.type != ShaderInstructionType::SBufferLoadDwordx2 &&
	    load.type != ShaderInstructionType::SBufferLoadDwordx4 && load.type != ShaderInstructionType::SBufferLoadDwordx8 &&
	    load.type != ShaderInstructionType::SBufferLoadDwordx16) return reject("Z scalar source is not a constant-buffer load", load.pc);
	if (load.src_num != 2 || load.src[0].type != ShaderOperandType::Sgpr || load.src[0].size != 4 ||
	    !operand_is_constant(load.src[1]) || !RectListPlainOperand(load.src[0]) || !RectListPlainOperand(load.src[1]) ||
	    !RectListPlainOperand(load.dst) || RectListLastWriter(instructions, scalar_writer, load.src[0]) >= 0)
		return reject("Z load offset, operands or descriptor immutability are unproven", load.pc);
	int matches = 0;
	for (int index = 0; index < input.bind.storage_buffers.buffers_num; ++index)
	{
		if (input.bind.storage_buffers.start_register[index] == load.src[0].register_id &&
		    input.bind.storage_buffers.usages[index] == ShaderStorageUsage::Constant) ++matches;
	}
	return matches == 1 || reject("Z descriptor is not one declared Constant buffer", load.pc);
}

static bool operand_is_variable(ShaderOperand op)
{
	return (op.type == ShaderOperandType::Vgpr || op.type == ShaderOperandType::VccLo || op.type == ShaderOperandType::VccHi ||
	        op.type == ShaderOperandType::Sgpr || op.type == ShaderOperandType::ExecLo || op.type == ShaderOperandType::ExecHi ||
	        op.type == ShaderOperandType::ExecZ || op.type == ShaderOperandType::Scc || op.type == ShaderOperandType::M0);
}

static SpirvValue operand_variable_to_str(ShaderOperand op)
{
	SpirvValue ret;

	EXIT_IF(op.size != 1);

	switch (op.type)
	{
		case ShaderOperandType::Vgpr:
			ret.value = String8::FromPrintf("v%d", op.register_id);
			ret.type  = SpirvType::Float;
			break;
		case ShaderOperandType::Sgpr:
			ret.value = String8::FromPrintf("s%d", op.register_id);
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::VccLo:
			ret.value = "vcc_lo";
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::VccHi:
			ret.value = "vcc_hi";
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::ExecLo:
			ret.value = "exec_lo";
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::ExecHi:
			ret.value = "exec_hi";
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::ExecZ:
			ret.value = "execz";
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::Scc:
			ret.value = "scc";
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::M0:
			ret.value = "m0";
			ret.type  = SpirvType::Uint;
			break;
		default: break;
	}

	return ret;
}

static SpirvValue operand_variable_to_str(ShaderOperand op, int shift)
{
	SpirvValue ret;

	EXIT_IF(op.size < 2 || op.size <= shift);

	switch (op.type)
	{
		case ShaderOperandType::Vgpr:
			ret.value = String8::FromPrintf("v%d", op.register_id + shift);
			ret.type  = SpirvType::Float;
			break;
		case ShaderOperandType::Sgpr:
			ret.value = String8::FromPrintf("s%d", op.register_id + shift);
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::VccLo:
			if (shift == 0)
			{
				ret.value = "vcc_lo";
				ret.type  = SpirvType::Uint;
			} else if (shift == 1)
			{
				ret.value = "vcc_hi";
				ret.type  = SpirvType::Uint;
			}
			break;
		case ShaderOperandType::ExecLo:
			if (shift == 0)
			{
				ret.value = "exec_lo";
				ret.type  = SpirvType::Uint;
			} else if (shift == 1)
			{
				ret.value = "exec_hi";
				ret.type  = SpirvType::Uint;
			}
			break;
		default: break;
	}

	return ret;
}

static bool operand_is_exec(ShaderOperand op)
{
	switch (op.type)
	{
		case ShaderOperandType::ExecLo:
		case ShaderOperandType::ExecHi:
		case ShaderOperandType::ExecZ: return true;
		default: break;
	}
	return false;
}

static bool operand_load_int(Spirv* spirv, ShaderOperand op, const String8& result_id, const String8& index, String8* load)
{
	EXIT_IF(load == nullptr);

	EXIT_NOT_IMPLEMENTED(op.negate || op.absolute);

	if (operand_is_constant(op))
	{
		String8 id = spirv->GetConstant(op);

		*load = String8("%<result_id> = OpBitcast %int %<id>")
		            .ReplaceStr("<index>", index)
		            .ReplaceStr("<id>", id)
		            .ReplaceStr("<result_id>", result_id);
	} else if (operand_is_variable(op))
	{
		auto value = operand_variable_to_str(op);

		if (value.type == SpirvType::Float)
		{
			*load = (String8("%t<result_id> = OpLoad %float %<id>\n") + String8(' ', 10) +
			         String8("%<result_id> = OpBitcast %int %t<result_id>\n"))
			            .ReplaceStr("<index>", index)
			            .ReplaceStr("<id>", value.value)
			            .ReplaceStr("<result_id>", result_id);
		} else if (value.type == SpirvType::Uint)
		{
			*load = (String8("%t<result_id> = OpLoad %uint %<id>\n") + String8(' ', 10) +
			         String8("%<result_id> = OpBitcast %int %t<result_id>\n"))
			            .ReplaceStr("<index>", index)
			            .ReplaceStr("<id>", value.value)
			            .ReplaceStr("<result_id>", result_id);
		}
	} else
	{
		return false;
	}
	return true;
}

static bool operand_load_uint(Spirv* spirv, ShaderOperand op, const String8& result_id, const String8& index, String8* load, int shift = -1)
{
	EXIT_IF(load == nullptr);

	EXIT_NOT_IMPLEMENTED(op.negate || op.absolute);

	if (operand_is_constant(op))
	{
		if (op.size == 2)
		{
			EXIT_NOT_IMPLEMENTED(shift < 0 || shift >= 2);

			if (shift == 0)
			{
				String8 id = spirv->GetConstant(op);
				*load      = String8("%<result_id> = OpBitcast %uint %<id>")
				            .ReplaceStr("<index>", index)
				            .ReplaceStr("<id>", id)
				            .ReplaceStr("<result_id>", result_id);
			} else
			{
				if (op.type == ShaderOperandType::IntegerInlineConstant && op.constant.i < 0)
				{
					*load = String8("%<result_id> = OpBitcast %uint %uint_0xffffffff")
					            .ReplaceStr("<index>", index)
					            .ReplaceStr("<result_id>", result_id);
				} else
				{
					*load =
					    String8("%<result_id> = OpBitcast %uint %uint_0").ReplaceStr("<index>", index).ReplaceStr("<result_id>", result_id);
				}
			}
		} else
		{
			String8 id = spirv->GetConstant(op);
			*load      = String8("%<result_id> = OpBitcast %uint %<id>")
			            .ReplaceStr("<index>", index)
			            .ReplaceStr("<id>", id)
			            .ReplaceStr("<result_id>", result_id);
		}
	} else if (operand_is_variable(op))
	{
		auto value = (shift >= 0 ? operand_variable_to_str(op, shift) : operand_variable_to_str(op));

		if (value.type == SpirvType::Float)
		{
			*load = (String8("%t<result_id> = OpLoad %float %<id>\n") + String8(' ', 10) +
			         String8("%<result_id> = OpBitcast %uint %t<result_id>\n"))
			            .ReplaceStr("<index>", index)
			            .ReplaceStr("<id>", value.value)
			            .ReplaceStr("<result_id>", result_id);
		} else if (value.type == SpirvType::Uint)
		{
			*load = (String8("%<result_id> = OpLoad %uint %<id>"))
			            .ReplaceStr("<index>", index)
			            .ReplaceStr("<id>", value.value)
			            .ReplaceStr("<result_id>", result_id);
		} else
		{
			return false;
		}
	} else
	{
		return false;
	}
	return true;
}

static bool operand_load_float(Spirv* spirv, ShaderOperand op, const String8& result_id, const String8& index, String8* load)
{
	EXIT_IF(load == nullptr);

	String8 l;

	if (operand_is_constant(op))
	{
		String8 id = spirv->GetConstant(op);

		l = String8("%<result_id> = OpBitcast %float %<id>").ReplaceStr("<id>", id);
	} else if (operand_is_variable(op))
	{
		auto value = operand_variable_to_str(op);

		if (value.type == SpirvType::Float)
		{
			l = String8("%<result_id> = OpLoad %float %<id>\n").ReplaceStr("<id>", value.value);
		} else if (value.type == SpirvType::Uint)
		{
			l = (String8("%t<result_id> = OpLoad %uint %<id>\n") + String8(' ', 10) +
			     String8("%<result_id> = OpBitcast %float %t<result_id>\n"))
			        .ReplaceStr("<id>", value.value);
		} else
		{
			return false;
		}
	} else
	{
		return false;
	}

	if (op.negate && op.absolute)
	{
		l += String8(' ', 10) + String8("%abs_<result_id> = OpExtInst %float %GLSL_std_450 FAbs %<result_id>\n") + String8(' ', 10) +
		     String8("%<result> = OpFNegate %float %abs_<result_id>\n");

		*load = l.ReplaceStr("<index>", index).ReplaceStr("<result_id>", "a" + result_id).ReplaceStr("<result>", result_id);

		return true;
	}

	if (op.absolute)
	{
		l += String8(' ', 10) + String8("%<result> = OpExtInst %float %GLSL_std_450 FAbs %<result_id>\n");
		*load = l.ReplaceStr("<index>", index).ReplaceStr("<result_id>", "a" + result_id).ReplaceStr("<result>", result_id);
	} else if (op.negate)
	{
		l += String8(' ', 10) + String8("%<result> = OpFNegate %float %<result_id>\n");
		*load = l.ReplaceStr("<index>", index).ReplaceStr("<result_id>", "n" + result_id).ReplaceStr("<result>", result_id);
	} else
	{
		*load = l.ReplaceStr("<index>", index).ReplaceStr("<result_id>", result_id);
	}

	return true;
}

static String8 get_scc_check(SccCheck scc_check, int dst_num)
{
	EXIT_IF(dst_num < 1 || dst_num > 2);

	if (dst_num == 1)
	{
		switch (scc_check)
		{
			case SccCheck::NonZero: return SCC_NZ_1; break;
			case SccCheck::OverflowAdd: return SCC_OVERFLOW_ADD_1; break;
			case SccCheck::OverflowSub: return SCC_OVERFLOW_SUB_1; break;
			case SccCheck::CarryOut: return SCC_CARRY_1; break;
			default: break;
		}
	} else if (dst_num == 2)
	{
		switch (scc_check)
		{
			case SccCheck::NonZero: return SCC_NZ_2; break;
			case SccCheck::OverflowAdd: LUMEN_NOT_IMPLEMENTED; break;
			case SccCheck::OverflowSub: LUMEN_NOT_IMPLEMENTED; break;
			case SccCheck::CarryOut: LUMEN_NOT_IMPLEMENTED; break;
			default: break;
		}
	}
	return "";
}

LUMEN_RECOMPILER_FUNC(Recompile_BufferLoadDword_Vdata1VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto dst_value   = operand_variable_to_str(inst.dst);
		auto src0_value  = operand_variable_to_str(inst.src[0]);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src1_value1 = operand_variable_to_str(inst.src[1], 1);
		// auto   src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset = spirv->GetConstant(inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);
		// EXIT_NOT_IMPLEMENTED(src1_value3.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>                
               OpStore %temp_int_2 %<offset>
		;%t206_<index> = OpLoad %uint %<src1_value3>
        ;%t208_<index> = OpShiftRightLogical %uint %t206_<index> %int_12
        ;%t210_<index> = OpBitwiseAnd %uint %t208_<index> %uint_127
        ;%t211_<index> = OpBitcast %int %t210_<index>
        ;       OpStore %temp_int_5 %t211_<index>
        %t110_<index> = OpFunctionCall %void %buffer_load_float1 %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   //.ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_BufferLoadFormatX_Vdata1VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		// EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));
		// EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value   = operand_variable_to_str(inst.dst);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		auto    src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset      = spirv->GetConstant(inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value3.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>                
               OpStore %temp_int_2 %<offset>
		%t206_<index> = OpLoad %uint %<src1_value3>
        %t208_<index> = OpShiftRightLogical %uint %t206_<index> %int_12
        %t210_<index> = OpBitwiseAnd %uint %t208_<index> %uint_127
        %t211_<index> = OpBitcast %int %t210_<index>
               OpStore %temp_int_5 %t211_<index>
               OpSelectionMerge %mubuf_f_x_merge_<index> None
               OpSwitch %t211_<index> %mubuf_f_x_load_<index> 80 %mubuf_f_x_constant_0_<index>
        %mubuf_f_x_constant_0_<index> = OpLabel
               OpStore %<p0> %float_0_000000
               OpBranch %mubuf_f_x_merge_<index>
        %mubuf_f_x_load_<index> = OpLabel
        %t110_<index> = OpFunctionCall %void %tbuffer_load_format_x %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5
               OpBranch %mubuf_f_x_merge_<index>
        %mubuf_f_x_merge_<index> = OpLabel
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_BufferLoadFormatXyzw_Vdata4VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto    dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		auto    src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset      = spirv->GetConstant(inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float || dst_value1.type != SpirvType::Float ||
		                     dst_value2.type != SpirvType::Float || dst_value3.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint || src1_value1.type != SpirvType::Uint ||
		                     src1_value3.type != SpirvType::Uint);

		// MUBUF format loads take DFMT, NFMT and destination selection from
		// the resource descriptor. PrepareStorageBuffers admits only the exact
		// profiles handled below; keeping the choice dynamic makes shader-cache
		// reuse independent of descriptor contents.
		static const char* text = R"(
        %mubuf_fxyzw_index_bits_<index> = OpLoad %float %<src0>
        %mubuf_fxyzw_index_<index> = OpBitcast %int %mubuf_fxyzw_index_bits_<index>
               OpStore %temp_int_1 %mubuf_fxyzw_index_<index>
        %mubuf_fxyzw_stride_word_<index> = OpLoad %uint %<src1_value1>
        %mubuf_fxyzw_stride_shifted_<index> = OpShiftRightLogical %uint %mubuf_fxyzw_stride_word_<index> %int_16
        %mubuf_fxyzw_stride_bits_<index> = OpBitwiseAnd %uint %mubuf_fxyzw_stride_shifted_<index> %uint_0x00003fff
        %mubuf_fxyzw_stride_<index> = OpBitcast %int %mubuf_fxyzw_stride_bits_<index>
               OpStore %temp_int_3 %mubuf_fxyzw_stride_<index>
        %mubuf_fxyzw_buffer_word_<index> = OpLoad %uint %<src1_value0>
        %mubuf_fxyzw_buffer_<index> = OpBitcast %int %mubuf_fxyzw_buffer_word_<index>
               OpStore %temp_int_4 %mubuf_fxyzw_buffer_<index>
               OpStore %temp_int_2 %<offset>
        %mubuf_fxyzw_format_word_<index> = OpLoad %uint %<src1_value3>
        %mubuf_fxyzw_format_shifted_<index> = OpShiftRightLogical %uint %mubuf_fxyzw_format_word_<index> %int_12
        %mubuf_fxyzw_format_bits_<index> = OpBitwiseAnd %uint %mubuf_fxyzw_format_shifted_<index> %uint_127
        %mubuf_fxyzw_format_<index> = OpBitcast %int %mubuf_fxyzw_format_bits_<index>
               OpSelectionMerge %mubuf_fxyzw_merge_<index> None
               OpSwitch %mubuf_fxyzw_format_<index> %mubuf_fxyzw_merge_<index> 36 %mubuf_fxyzw_x_uint_<index> 39 %mubuf_fxyzw_x_float_<index> 80 %mubuf_fxyzw_unorm8_constant_0011_<index> 92 %mubuf_fxyzw_xy_uint_<index> 95 %mubuf_fxyzw_xy_float_<index> 99 %mubuf_fxyzw_xyz_sscaled16_<index> 119 %mubuf_fxyzw_xyzw_float_<index>
        %mubuf_fxyzw_x_uint_<index> = OpLabel
        %mubuf_fxyzw_load_x_uint_<index> = OpFunctionCall %void %buffer_load_float1 %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
               OpStore %<p1> %float_0_000000
               OpStore %<p2> %float_0_000000
               OpStore %<p3> %float_0_000000
               OpBranch %mubuf_fxyzw_merge_<index>
        %mubuf_fxyzw_x_float_<index> = OpLabel
        %mubuf_fxyzw_load_x_float_<index> = OpFunctionCall %void %buffer_load_float1 %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
               OpStore %<p1> %float_0_000000
               OpStore %<p2> %float_0_000000
               OpStore %<p3> %float_1_000000
               OpBranch %mubuf_fxyzw_merge_<index>
        %mubuf_fxyzw_unorm8_constant_0011_<index> = OpLabel
               OpStore %<p0> %float_0_000000
               OpStore %<p1> %float_0_000000
               OpStore %<p2> %float_1_000000
               OpStore %<p3> %float_1_000000
               OpBranch %mubuf_fxyzw_merge_<index>
        %mubuf_fxyzw_xy_uint_<index> = OpLabel
        %mubuf_fxyzw_load_xy_uint_<index> = OpFunctionCall %void %buffer_load_float2 %<p0> %<p1> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
               OpStore %<p2> %float_0_000000
               OpStore %<p3> %float_0_000000
               OpBranch %mubuf_fxyzw_merge_<index>
        %mubuf_fxyzw_xy_float_<index> = OpLabel
        %mubuf_fxyzw_load_xy_float_<index> = OpFunctionCall %void %buffer_load_float2 %<p0> %<p1> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
               OpStore %<p2> %float_0_000000
               OpStore %<p3> %float_1_000000
               OpBranch %mubuf_fxyzw_merge_<index>
        %mubuf_fxyzw_xyz_sscaled16_<index> = OpLabel
        %mubuf_fxyzw_load_xyz_sscaled16_<index> = OpFunctionCall %void %buffer_load_sscaled16_xyzw %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
               OpStore %<p3> %float_0_000000
               OpBranch %mubuf_fxyzw_merge_<index>
        %mubuf_fxyzw_xyzw_float_<index> = OpLabel
        %mubuf_fxyzw_load_xyzw_float_<index> = OpFunctionCall %void %buffer_load_float4 %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
               OpBranch %mubuf_fxyzw_merge_<index>
        %mubuf_fxyzw_merge_<index> = OpLabel
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value)
		                   .ReplaceStr("<p3>", dst_value3.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_BufferStoreDword_Vdata1VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto dst_value   = operand_variable_to_str(inst.dst);
		auto src0_value  = operand_variable_to_str(inst.src[0]);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src1_value1 = operand_variable_to_str(inst.src[1], 1);
		// auto   src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset = spirv->GetConstant(inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);
		// EXIT_NOT_IMPLEMENTED(src1_value3.type != SpirvType::Uint);

		// TODO() check VSKIP

		static const char* text = R"(
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %t278_<index> None
               OpBranchConditional %exec_lo_b_<index> %t277_<index> %t278_<index>       
		%t277_<index> = OpLabel

        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>                
               OpStore %temp_int_2 %<offset>
		;%t206_<index> = OpLoad %uint %<src1_value3>
        ;%t208_<index> = OpShiftRightLogical %uint %t206_<index> %int_12
        ;%t210_<index> = OpBitwiseAnd %uint %t208_<index> %uint_127
        ;%t211_<index> = OpBitcast %int %t210_<index>
        ;       OpStore %temp_int_5 %t211_<index>
        %t110_<index> = OpFunctionCall %void %buffer_store_float1 %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 

               OpBranch %t278_<index>			   
        %t278_<index> = OpLabel
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   //.ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_BufferStoreFormatX_Vdata1VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value   = operand_variable_to_str(inst.dst);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		auto    src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset      = spirv->GetConstant(inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value3.type != SpirvType::Uint);

		// TODO() check VSKIP

		static const char* text = R"(
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %t278_<index> None
               OpBranchConditional %exec_lo_b_<index> %t277_<index> %t278_<index>       
		%t277_<index> = OpLabel

        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>                
               OpStore %temp_int_2 %<offset>
		%t206_<index> = OpLoad %uint %<src1_value3>
        %t208_<index> = OpShiftRightLogical %uint %t206_<index> %int_12
        %t210_<index> = OpBitwiseAnd %uint %t208_<index> %uint_127
        %t211_<index> = OpBitcast %int %t210_<index>
               OpStore %temp_int_5 %t211_<index>
        %t110_<index> = OpFunctionCall %void %tbuffer_store_format_x %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5 

               OpBranch %t278_<index>			   
        %t278_<index> = OpLabel
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_BufferStoreFormatXy_Vdata2VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		auto    src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset      = spirv->GetConstant(inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value3.type != SpirvType::Uint);

		// TODO() check VSKIP

		static const char* text = R"(
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %t278_<index> None
               OpBranchConditional %exec_lo_b_<index> %t277_<index> %t278_<index>       
		%t277_<index> = OpLabel

        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>                
               OpStore %temp_int_2 %<offset>
		%t206_<index> = OpLoad %uint %<src1_value3>
        %t208_<index> = OpShiftRightLogical %uint %t206_<index> %int_12
        %t210_<index> = OpBitwiseAnd %uint %t208_<index> %uint_127
        %t211_<index> = OpBitcast %int %t210_<index>
               OpStore %temp_int_5 %t211_<index>
        %t110_<index> = OpFunctionCall %void %tbuffer_store_format_xy %<p0> %<p1> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5 

               OpBranch %t278_<index>			   
        %t278_<index> = OpLabel
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_DsAppend_VdstGds)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->gds_pointers.pointers_num > 0)
	{
		String8 index_str = String8::FromPrintf("%u", index);

		EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

		auto dst_value = operand_variable_to_str(inst.dst);

		EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t192_<index> = OpLoad %uint %m0
        %t194_<index> = OpShiftRightLogical %uint %t192_<index> %int_16
        %t196_<index> = OpAccessChain %_ptr_StorageBuffer_uint %gds %int_0 %t194_<index>
        %t198_<index> = OpAtomicIAdd %uint %t196_<index> %uint_1 %uint_0 %uint_1
        %t199_<index> = OpBitcast %float %t198_<index>
               OpStore %<dst> %t199_<index>
               OpMemoryBarrier %uint_1 %uint_72
)";
		*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<index>", index_str);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_DsConsume_VdstGds)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->gds_pointers.pointers_num > 0)
	{
		String8 index_str = String8::FromPrintf("%u", index);

		EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

		auto dst_value = operand_variable_to_str(inst.dst);

		EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t192_<index> = OpLoad %uint %m0
        %t194_<index> = OpShiftRightLogical %uint %t192_<index> %int_16
        %t196_<index> = OpAccessChain %_ptr_StorageBuffer_uint %gds %int_0 %t194_<index>
        %t198_<index> = OpAtomicISub %uint %t196_<index> %uint_1 %uint_0 %uint_1
        %t199_<index> = OpBitcast %float %t198_<index>
               OpStore %<dst> %t199_<index>
               OpMemoryBarrier %uint_1 %uint_72
)";
		*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<index>", index_str);

		return true;
	}

	return false;
}

static bool PixelExportHasFinalExecValidMask(const ShaderInstruction& inst)
{
	// GCN permits VM on intermediate exports and uses the final transmitted mask.
	// Applying OpKill is therefore safe here only when this export is also DONE.
	return inst.export_valid_mask && inst.export_done;
}

static bool PixelShaderMayDiscard(const ShaderCode& code, const ShaderPixelInputInfo& info)
{
	if (info.ps_pixel_kill_enable) { return true; }
	for (const auto& inst: code.GetInstructions())
	{
		// Final VM exports lower to an EXEC guard with OpKill independently of
		// KILL_ENABLE, so the register alone does not describe every discard.
		if (inst.type == ShaderInstructionType::Exp && PixelExportHasFinalExecValidMask(inst)) { return true; }
	}
	return false;
}

static void WritePixelExportExecValidMaskBegin(String8* dst_source, uint32_t index, bool enabled)
{
	if (!enabled) { return; }
	static const char* text = R"(
	         %exp_vm_exec_<index> = OpLoad %uint %exec_lo
	       %exp_vm_active_<index> = OpINotEqual %bool %exp_vm_exec_<index> %uint_0
               OpSelectionMerge %exp_vm_merge_<index> None
               OpBranchConditional %exp_vm_active_<index> %exp_vm_export_<index> %exp_vm_discard_<index>
%exp_vm_discard_<index> = OpLabel
               OpKill
 %exp_vm_export_<index> = OpLabel
)";
	*dst_source += String8(text).ReplaceStr("<index>", String8::FromPrintf("%u", index));
}

static void WritePixelExportExecValidMaskEnd(String8* dst_source, uint32_t index, bool enabled)
{
	if (!enabled) { return; }
	static const char* text = R"(
               OpBranch %exp_vm_merge_<index>
  %exp_vm_merge_<index> = OpLabel
)";
	*dst_source += String8(text).ReplaceStr("<index>", String8::FromPrintf("%u", index));
}

LUMEN_RECOMPILER_FUNC(Recompile_Exp_Mrt0OffOffComprVmDone)
{
	EXIT_NOT_IMPLEMENTED(index == 0 || index + 1 >= code.GetInstructions().Size());

	const auto& prev_inst = code.GetInstructions().At(index - 1);
	const auto& inst      = code.GetInstructions().At(index);
	auto        block     = code.ReadBlock(prev_inst.pc);

	if (!block.is_discard)
	{
		return false;
	}

	const auto* info = spirv->GetPsInputInfo();

	EXIT_NOT_IMPLEMENTED(info == nullptr || !info->ps_pixel_kill_enable);

	EXIT_NOT_IMPLEMENTED(inst.src_num > 0);

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
        OpKill
)";

	*dst_source += String8(text);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_Exp_Mrt0Vsrc0Vsrc1ComprVmDone)
{
	const auto& inst = code.GetInstructions().At(index);
	const bool check_exec_valid_mask = PixelExportHasFinalExecValidMask(inst);

	EXIT_NOT_IMPLEMENTED(spirv->GetPsInputInfo()->target_output_mode[0] != 4);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[1]));

	auto src0_value = operand_variable_to_str(inst.src[0]);
	auto src1_value = operand_variable_to_str(inst.src[1]);

	// TODO() check VSKIP
	WritePixelExportExecValidMaskBegin(dst_source, index, check_exec_valid_mask);

	static const char* text = R"(
         %t1_<index> = OpLoad %float %<src0> 
         %t2_<index> = OpBitcast %uint %t1_<index>
         %t3_<index> = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %t2_<index>
         %t4_<index> = OpCompositeExtract %float %t3_<index> 0 
         %t5_<index> = OpCompositeExtract %float %t3_<index> 1 
         %t6_<index> = OpLoad %float %<src1> 
         %t7_<index> = OpBitcast %uint %t6_<index> 
         %t8_<index> = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %t7_<index> 
         %t9_<index> = OpCompositeExtract %float %t8_<index> 0 
         %t10_<index> = OpCompositeExtract %float %t8_<index> 1 
         %t11_<index> = OpCompositeConstruct %v4float %t4_<index> %t5_<index> %t9_<index> %t10_<index> 
               OpStore %outColor %t11_<index>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<src0>", src0_value.value)
	                   .ReplaceStr("<src1>", src1_value.value);
	WritePixelExportExecValidMaskEnd(dst_source, index, check_exec_valid_mask);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_Exp_Mrt0Vsrc0OffComprVmDone)
{
	const auto& inst = code.GetInstructions().At(index);
	const bool check_exec_valid_mask = PixelExportHasFinalExecValidMask(inst);

	EXIT_NOT_IMPLEMENTED(spirv->GetPsInputInfo()->target_output_mode[0] != 4);
	EXIT_NOT_IMPLEMENTED(inst.src_num != 1);
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));

	auto src0_value = operand_variable_to_str(inst.src[0]);

	// TODO() check VSKIP
	WritePixelExportExecValidMaskBegin(dst_source, index, check_exec_valid_mask);

	static const char* text = R"(
         %t1_<index> = OpLoad %float %<src0>
         %t2_<index> = OpBitcast %uint %t1_<index>
         %t3_<index> = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %t2_<index>
         %t4_<index> = OpCompositeExtract %float %t3_<index> 0
         %t5_<index> = OpCompositeExtract %float %t3_<index> 1
         %t6_<index> = OpCompositeConstruct %v4float %t4_<index> %t5_<index> %float_0_000000 %float_1_000000
               OpStore %outColor %t6_<index>
)";

	*dst_source += String8(text).ReplaceStr("<index>", String8::FromPrintf("%u", index)).ReplaceStr("<src0>", src0_value.value);
	WritePixelExportExecValidMaskEnd(dst_source, index, check_exec_valid_mask);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_Exp_Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone)
{
	const auto& inst = code.GetInstructions().At(index);
	const bool check_exec_valid_mask = PixelExportHasFinalExecValidMask(inst);

	EXIT_NOT_IMPLEMENTED(spirv->GetPsInputInfo()->target_output_mode[0] != 9);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[1]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[2]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[3]));

	auto src0_value = operand_variable_to_str(inst.src[0]);
	auto src1_value = operand_variable_to_str(inst.src[1]);
	auto src2_value = operand_variable_to_str(inst.src[2]);
	auto src3_value = operand_variable_to_str(inst.src[3]);

	// TODO() check VSKIP
	WritePixelExportExecValidMaskBegin(dst_source, index, check_exec_valid_mask);

	static const char* text = R"(
         %t0_<index> = OpLoad %float %<src0>
         %t1_<index> = OpLoad %float %<src1>
         %t2_<index> = OpLoad %float %<src2>
         %t3_<index> = OpLoad %float %<src3>
         %t11_<index> = OpCompositeConstruct %v4float %t0_<index> %t1_<index> %t2_<index> %t3_<index> 
               OpStore %outColor %t11_<index>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<src0>", src0_value.value)
	                   .ReplaceStr("<src1>", src1_value.value)
	                   .ReplaceStr("<src2>", src2_value.value)
	                   .ReplaceStr("<src3>", src3_value.value);
	WritePixelExportExecValidMaskEnd(dst_source, index, check_exec_valid_mask);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_Exp_Param_XXX_Vsrc0Vsrc1Vsrc2Vsrc3)
{
	const auto& inst = code.GetInstructions().At(index);
	const bool generic = inst.format == ShaderInstructionFormat::ParamVsrc0Vsrc1Vsrc2Vsrc3;
	const int parameter = ShaderGetParameterExportIndex(inst);
	const auto* vertex = spirv->GetVsInputInfo();
	if (generic && (code.GetType() != ShaderType::Vertex || vertex == nullptr || parameter < 5 ||
	                parameter >= vertex->export_count || vertex->export_count > 32)) return false;

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[1]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[2]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[3]));

	auto src0_value = operand_variable_to_str(inst.src[0]);
	auto src1_value = operand_variable_to_str(inst.src[1]);
	auto src2_value = operand_variable_to_str(inst.src[2]);
	auto src3_value = operand_variable_to_str(inst.src[3]);

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
         %t0_<index> = OpLoad %float %<src0>
         %t1_<index> = OpLoad %float %<src1>
         %t2_<index> = OpLoad %float %<src2>
         %t3_<index> = OpLoad %float %<src3>
         %t4_<index> = OpCompositeConstruct %v4float %t0_<index> %t1_<index> %t2_<index> %t3_<index>
               OpStore %<param> %t4_<index>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<src0>", src0_value.value)
	                   .ReplaceStr("<src1>", src1_value.value)
	                   .ReplaceStr("<src2>", src2_value.value)
	                   .ReplaceStr("<src3>", src3_value.value)
	                   .ReplaceStr("<param>", generic ? String8::FromPrintf("param%d", parameter) : String8(param[0]));

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_Exp_Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done)
{
	const auto& inst = code.GetInstructions().At(index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[1]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[2]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[3]));

	auto src0_value = operand_variable_to_str(inst.src[0]);
	auto src1_value = operand_variable_to_str(inst.src[1]);
	auto src2_value = operand_variable_to_str(inst.src[2]);
	auto src3_value = operand_variable_to_str(inst.src[3]);

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
         %t0_<index> = OpLoad %float %<src0>
         %t1_<index> = OpLoad %float %<src1>
         %t2_<index> = OpLoad %float %<src2>
         %t3_<index> = OpLoad %float %<src3>
         %t4_<index> = OpCompositeConstruct %v4float %t0_<index> %t1_<index> %t2_<index> %t3_<index>
         %t5_<index> = OpAccessChain %_ptr_Output_v4float %outPerVertex %int_per_vertex_0
               OpStore %t5_<index> %t4_<index>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<src0>", src0_value.value)
	                   .ReplaceStr("<src1>", src1_value.value)
	                   .ReplaceStr("<src2>", src2_value.value)
	                   .ReplaceStr("<src3>", src3_value.value);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_ImageSample_Vdata1Vaddr3StSsSingleChannel)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	if (inst.format == ShaderInstructionFormat::Vdata1Vaddr3StSsDmask2 ||
	    inst.format == ShaderInstructionFormat::Vdata1Vaddr3StSsDmask4)
		EXIT_NOT_IMPLEMENTED(!ShaderImageSampleSingleComponentInstructionSupported(inst));

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst);
		auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
		auto src0_value1 = operand_variable_to_str(inst.src[0], 1);
		auto src0_value2 = operand_variable_to_str(inst.src[0], 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = <implicit_sample>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_<channel>
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<channel>", param[0])
		                   .ReplaceStr("<implicit_sample>", ImageSampleImplicitLodExpression(index))
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_ImageSample_Vdata1Vaddr3StSsDmask8)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst);
		auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
		auto src0_value1 = operand_variable_to_str(inst.src[0], 1);
		auto src0_value2 = operand_variable_to_str(inst.src[0], 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = <implicit_sample>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<implicit_sample>", ImageSampleImplicitLodExpression(index))
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_ImageSample_Vdata2Vaddr3StSsDmask3)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
		auto src0_value1 = operand_variable_to_str(inst.src[0], 1);
		auto src0_value2 = operand_variable_to_str(inst.src[0], 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = <implicit_sample>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value1> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<implicit_sample>", ImageSampleImplicitLodExpression(index))
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_ImageSample_Vdata2Vaddr3StSsDmask5)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
		auto src0_value1 = operand_variable_to_str(inst.src[0], 1);
		auto src0_value2 = operand_variable_to_str(inst.src[0], 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = <implicit_sample>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value1> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<implicit_sample>", ImageSampleImplicitLodExpression(index))
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_ImageSample_Vdata2Vaddr3StSsDmask9)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
		auto src0_value1 = operand_variable_to_str(inst.src[0], 1);
		auto src0_value2 = operand_variable_to_str(inst.src[0], 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = <implicit_sample>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value1> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<implicit_sample>", ImageSampleImplicitLodExpression(index))
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_ImageSample_Vdata3Vaddr3StSsDmask7)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	// const auto& bind_params = spirv->GetBindParams();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
		auto src0_value1 = operand_variable_to_str(inst.src[0], 1);
		auto src0_value2 = operand_variable_to_str(inst.src[0], 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = <implicit_sample>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<implicit_sample>", ImageSampleImplicitLodExpression(index))
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_ImageSampleLz_Vdata3Vaddr3StSsDmask7)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
		auto src0_value1 = operand_variable_to_str(inst.src[0], 1);
		auto src0_value2 = operand_variable_to_str(inst.src[0], 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>

         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>

         %t43_<index> = OpImageSampleExplicitLod %v4float %t38_<index> %t42_<index> Lod %float_0_000000
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_ImageSampleLz_Vdata4Vaddr3StSsDmaskF)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
		auto src0_value1 = operand_variable_to_str(inst.src[0], 1);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>

         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>

         %t43_<index> = OpImageSampleExplicitLod %v4float %t38_<index> %t42_<index> Lod %float_0_000000
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
         %t57_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t58_<index> = OpLoad %float %t57_<index>
               OpStore %<dst_value3> %t58_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value)
		                   .ReplaceStr("<dst_value3>", dst_value3.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_ImageSampleLzO_Vdata3Vaddr4StSsDmask7)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
		auto src0_value1 = operand_variable_to_str(inst.src[0], 1);
		auto src0_value2 = operand_variable_to_str(inst.src[0], 2);
		auto src0_value3 = operand_variable_to_str(inst.src[0], 3);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>

         %t39_<index> = OpLoad %float %<src0_value1>
         %t40_<index> = OpLoad %float %<src0_value2>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>

         %90_<index> = OpLoad %float %<src0_value0>
         %91_<index> = OpBitcast %int %90_<index>
         %98_<index> = OpBitFieldSExtract %int %91_<index> %int_0 %int_6
        %101_<index> = OpBitFieldSExtract %int %91_<index> %int_8 %int_6		
        %102_<index> = OpCompositeConstruct %v2int %98_<index> %101_<index>

         %130_<index> = OpConvertSToF %v2float %102_<index>
         %138_<index> = OpImage %ImageS %t38_<index>
        %139_<index> = OpImageQuerySizeLod %v2int %138_<index> %int_0
        %140_<index> = OpConvertSToF %v2float %139_<index>
        %141_<index> = OpFDiv %v2float %130_<index> %140_<index>
        %142_<index> = OpFAdd %v2float %t42_<index> %141_<index>

         %t43_<index> = OpImageSampleExplicitLod %v4float %t38_<index> %142_<index> Lod %float_0_000000
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src0_value3>", src0_value3.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_ImageSample_Vdata4Vaddr3StSsDmaskF)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	// const auto& bind_params = spirv->GetBindParams();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		const auto texture_type = IsPixelType8SampledProfile(code, bind_info->textures2D)
		                              ? ResolveTextureTypeForInstruction(code, *bind_info, index, ShaderTextureUsage::ReadOnly)
		                              : ShaderGetUniformTextureType(bind_info->textures2D, ShaderTextureUsage::ReadOnly);
		EXIT_NOT_IMPLEMENTED(texture_type != ShaderTextureResource::TYPE_8_COMPAT && texture_type != ShaderTextureResource::TYPE_2D);
		const String8 coordinate_y = texture_type == ShaderTextureResource::TYPE_8_COMPAT
		                                 ? String8("%float_0_500000")
		                                 : String8::FromPrintf("%%t40_%u", index);
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
		auto src0_value1 = operand_variable_to_str(inst.src[0], 1);
		auto src0_value2 = operand_variable_to_str(inst.src[0], 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
	     %t42_<index> = OpCompositeConstruct %v2float %t39_<index> <coordinate_y>
         %t43_<index> = <implicit_sample>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
         %t57_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t58_<index> = OpLoad %float %t57_<index>
               OpStore %<dst_value3> %t58_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<implicit_sample>", ImageSampleImplicitLodExpression(index))
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<coordinate_y>", coordinate_y)
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value)
		                   .ReplaceStr("<dst_value3>", dst_value3.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_ImageLoad_Vdata4Vaddr3StDmaskF)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	// const auto& bind_params = spirv->GetBindParams();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0)
	{
		const auto texture_type = ShaderGetUniformTextureType(bind_info->textures2D, ShaderTextureUsage::ReadOnly);
		EXIT_NOT_IMPLEMENTED(texture_type != ShaderTextureResource::TYPE_8_COMPAT && texture_type != ShaderTextureResource::TYPE_2D);
		const String8 coordinate_y =
		    texture_type == ShaderTextureResource::TYPE_8_COMPAT ? String8("%uint_0") : String8::FromPrintf("%%t71_%u", index);
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
		auto src0_value1 = operand_variable_to_str(inst.src[0], 1);
		auto src0_value2 = operand_variable_to_str(inst.src[0], 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED
		// TODO() swizzle channels
		// TODO() convert SRGB -> LINEAR if SRGB format was replaced with UNORM

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t67_<index> = OpLoad %float %<src0_value0>
         %t69_<index> = OpBitcast %uint %t67_<index>
         %t70_<index> = OpLoad %float %<src0_value1>
         %t71_<index> = OpBitcast %uint %t70_<index>
		 %t73_<index> = OpCompositeConstruct %v2uint %t69_<index> <coordinate_y>
         %t74_<index> = OpImageFetch %v4float %t27_<index> %t73_<index>
               OpStore %temp_v4float %t74_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
         %t57_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t58_<index> = OpLoad %float %t57_<index>
               OpStore %<dst_value3> %t58_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<coordinate_y>", coordinate_y)
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value)
		                   .ReplaceStr("<dst_value3>", dst_value3.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_ImageLoadMip_Vdata4Vaddr4StDmaskF)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0)
	{
		const auto texture_type = ShaderGetUniformTextureType(bind_info->textures2D, ShaderTextureUsage::ReadOnly);
		EXIT_NOT_IMPLEMENTED(texture_type != ShaderTextureResource::TYPE_8_COMPAT && texture_type != ShaderTextureResource::TYPE_2D);
		const String8 coordinate_y =
		    texture_type == ShaderTextureResource::TYPE_8_COMPAT ? String8("%uint_0") : String8::FromPrintf("%%t71_%u", index);
		const bool adjacent_single_window =
		    ShaderTextureStorageMipProfileId(bind_info->textures2D) == SHADER_TEXTURE_STORAGE_MIP_PROFILE_ADJACENT_SINGLE_WINDOW;
		const String8 mip = texture_type == ShaderTextureResource::TYPE_8_COMPAT || adjacent_single_window
		                        ? String8("%uint_0")
		                        : String8::FromPrintf("%%t75_%u", index);
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
		auto src0_value1 = operand_variable_to_str(inst.src[0], 1);
		auto src0_value2 = operand_variable_to_str(inst.src[0], 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);

		// Type-8 and the adjacent-single-window profile both expose one physical
		// host mip. Keep their host LOD canonical instead of forwarding the guest
		// address lane; ordinary guest 2D retains the existing (x, y, lod) lowering.
		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t67_<index> = OpLoad %float %<src0_value0>
         %t69_<index> = OpBitcast %uint %t67_<index>
         %t70_<index> = OpLoad %float %<src0_value1>
         %t71_<index> = OpBitcast %uint %t70_<index>
         %t74_<index> = OpLoad %float %<src0_value2>
         %t75_<index> = OpBitcast %uint %t74_<index>
         %t73_<index> = OpCompositeConstruct %v2uint %t69_<index> <coordinate_y>
         %t76_<index> = OpImageFetch %v4float %t27_<index> %t73_<index> Lod <mip>
               OpStore %temp_v4float %t76_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
         %t57_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t58_<index> = OpLoad %float %t57_<index>
               OpStore %<dst_value3> %t58_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<coordinate_y>", coordinate_y)
		                   .ReplaceStr("<mip>", mip)
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value)
		                   .ReplaceStr("<dst_value3>", dst_value3.value);
		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_ImageStore_Vdata4Vaddr3StDmaskF)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	// const auto& bind_params = spirv->GetBindParams();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_storage_num > 0)
	{
		const auto texture_type = ShaderGetUniformTextureType(bind_info->textures2D, ShaderTextureUsage::ReadWrite);
		EXIT_NOT_IMPLEMENTED(texture_type != ShaderTextureResource::TYPE_8_COMPAT && texture_type != ShaderTextureResource::TYPE_2D);
		const bool storage_legacy_atlas =
		    ShaderTextureStorageMipProfileId(bind_info->textures2D) == SHADER_TEXTURE_STORAGE_MIP_PROFILE_LEGACY_ATLAS;
		const String8 coordinate_y = texture_type == ShaderTextureResource::TYPE_8_COMPAT
		                                 ? String8("%uint_0")
		                                 : (storage_legacy_atlas ? String8::FromPrintf("%%storage_mip_y_%u", index)
		                                                         : String8::FromPrintf("%%t71_%u", index));
		String8 storage_mip_offset;
		auto dst_value0 = operand_variable_to_str(inst.dst, 0);
		auto dst_value1 = operand_variable_to_str(inst.dst, 1);
		auto dst_value2 = operand_variable_to_str(inst.dst, 2);
		auto dst_value3 = operand_variable_to_str(inst.dst, 3);

		auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
		auto src0_value1 = operand_variable_to_str(inst.src[0], 1);

		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src1_value2 = operand_variable_to_str(inst.src[1], 2);
		auto src1_value3 = operand_variable_to_str(inst.src[1], 3);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		if (storage_legacy_atlas)
		{
			storage_mip_offset = String8(R"(
         %storage_mip_word_<index> = OpLoad %uint %<src1_value3>
        %storage_mip_shift_<index> = OpShiftRightLogical %uint %storage_mip_word_<index> %uint_12
         %storage_mip_base_<index> = OpBitwiseAnd %uint %storage_mip_shift_<index> %uint_15
       %storage_mip_is_one_<index> = OpIEqual %bool %storage_mip_base_<index> %uint_1
       %storage_mip_offset_<index> = OpSelect %uint %storage_mip_is_one_<index> %t151_<index> %uint_0
            %storage_mip_y_<index> = OpIAdd %uint %t71_<index> %storage_mip_offset_<index>
)")
			                         .ReplaceStr("<index>", String8::FromPrintf("%u", index))
			                         .ReplaceStr("<src1_value3>", src1_value3.value);
		}

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED
		// TODO() swizzle channels
		// TODO() convert SRGB -> LINEAR if SRGB format was replaced with UNORM

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t25_<index> = OpLoad %uint %<src1_value2>
		%t143_<index> = OpShiftRightLogical %uint %t25_<index> %uint_0
        %t145_<index> = OpBitwiseAnd %uint %t143_<index> %uint_0x00003fff
        %t146_<index> = OpIAdd %uint %t145_<index> %uint_1
        %t149_<index> = OpShiftRightLogical %uint %t25_<index> %uint_14
        %t150_<index> = OpBitwiseAnd %uint %t149_<index> %uint_0x00003fff
        %t151_<index> = OpIAdd %uint %t150_<index> %uint_1
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageL %textures2D_L %t24_<index>
         %t27_<index> = OpLoad %ImageL %t26_<index>
         %t67_<index> = OpLoad %float %<src0_value0>
         %t69_<index> = OpBitcast %uint %t67_<index>
         %t70_<index> = OpLoad %float %<src0_value1>
         %t71_<index> = OpBitcast %uint %t70_<index>
<storage_mip_offset>
		 %t73_<index> = OpCompositeConstruct %v2uint %t69_<index> <coordinate_y>
         %t84_<index> = OpLoad %float %<dst_value0>
         %t85_<index> = OpLoad %float %<dst_value1>
         %t86_<index> = OpLoad %float %<dst_value2>
         %t87_<index> = OpLoad %float %<dst_value3>
         %t88_<index> = OpCompositeConstruct %v4float %t84_<index> %t85_<index> %t86_<index> %t87_<index>
               OpImageWrite %t27_<index> %t73_<index> %t88_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<coordinate_y>", coordinate_y)
		                   .ReplaceStr("<storage_mip_offset>", storage_mip_offset)
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value2>", src1_value2.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value)
		                   .ReplaceStr("<dst_value3>", dst_value3.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_ImageStoreMip_Vdata4Vaddr4StDmaskF)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	// const auto& bind_params = spirv->GetBindParams();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_storage_num > 0)
	{
		auto dst_value0 = operand_variable_to_str(inst.dst, 0);
		auto dst_value1 = operand_variable_to_str(inst.dst, 1);
		auto dst_value2 = operand_variable_to_str(inst.dst, 2);
		auto dst_value3 = operand_variable_to_str(inst.dst, 3);

		auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
		auto src0_value1 = operand_variable_to_str(inst.src[0], 1);
		auto src0_value2 = operand_variable_to_str(inst.src[0], 2);

		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src1_value2 = operand_variable_to_str(inst.src[1], 2);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED
		// TODO() swizzle channels
		// TODO() convert SRGB -> LINEAR if SRGB format was replaced with UNORM

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t25_<index> = OpLoad %uint %<src1_value2>
		%t143_<index> = OpShiftRightLogical %uint %t25_<index> %uint_0
        %t145_<index> = OpBitwiseAnd %uint %t143_<index> %uint_0x00003fff
        %t146_<index> = OpIAdd %uint %t145_<index> %uint_1
        %t149_<index> = OpShiftRightLogical %uint %t25_<index> %uint_14
        %t150_<index> = OpBitwiseAnd %uint %t149_<index> %uint_0x00003fff
        %t151_<index> = OpIAdd %uint %t150_<index> %uint_1
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageL %textures2D_L %t24_<index>
         %t27_<index> = OpLoad %ImageL %t26_<index>
         %t67_<index> = OpLoad %float %<src0_value0>
         %t69_<index> = OpBitcast %uint %t67_<index>
         %t70_<index> = OpLoad %float %<src0_value1>
         %t71_<index> = OpBitcast %uint %t70_<index>
         %t701_<index> = OpLoad %float %<src0_value2>
         %t711_<index> = OpBitcast %uint %t701_<index>
         %t160_<index> = OpFunctionCall %v2uint %mipmap %t711_<index> %t146_<index> %t151_<index>
         %t73_<index> = OpCompositeConstruct %v2uint %t69_<index> %t71_<index>
         %t84_<index> = OpLoad %float %<dst_value0>
         %t85_<index> = OpLoad %float %<dst_value1>
         %t86_<index> = OpLoad %float %<dst_value2>
         %t87_<index> = OpLoad %float %<dst_value3>
         %t172_<index> = OpIAdd %v2uint %t160_<index> %t73_<index>
         %t88_<index> = OpCompositeConstruct %v4float %t84_<index> %t85_<index> %t86_<index> %t87_<index>
               OpImageWrite %t27_<index> %t172_<index> %t88_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value2>", src1_value2.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value)
		                   .ReplaceStr("<dst_value3>", dst_value3.value);

		return true;
	}

	return false;
}

/* XXX: Andn2, Or, Nor, Cselect */
LUMEN_RECOMPILER_FUNC(Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	String8 load0;
	String8 load1;
	String8 load2;
	String8 load3;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t2_<index>", index_str, &load2, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t3_<index>", index_str, &load3, 1))
	{
		return false;
	}

	static const char* text = R"(    
    <load0>
    <load1>
    <load2>
    <load3>
    <param0>
    <param1>
    <param2>
    <param3>
    OpStore %<dst0> %tb_<index>
    OpStore %<dst1> %td_<index>
    <execz>
    <scc>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<load3>", load3)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", param[1])
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_S_Lshl_B64_Sdst2Ssrc02Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	String8 load0;
	String8 load1;
	String8 load2;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	static const char* text = R"(
     <load0>
     <load1>
     <load2>
     <param0>
     <param1>
     <param2>
     <param3>
%t22_<index> = OpBitwiseAnd %uint %t2_<index> %uint_63
     OpStore %temp_uint_2 %t0_<index>
     OpStore %temp_uint_3 %t1_<index>
     OpStore %temp_uint_4 %t22_<index>
%t_<index> = OpFunctionCall %void %shift_left %temp_uint_0 %temp_uint_1 %temp_uint_2 %temp_uint_3 %temp_uint_4
%r0_<index> = OpLoad %uint %temp_uint_0 
%r1_<index> = OpLoad %uint %temp_uint_1
     OpStore %<dst0> %r0_<index>
     OpStore %<dst1> %r1_<index>
     <execz>
     <scc>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_S_Lshr_B64_Sdst2Ssrc02Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	String8 load0;
	String8 load1;
	String8 load2;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	static const char* text = R"(
     <load0>
     <load1>
     <load2>
     <param0>
     <param1>
     <param2>
     <param3>
%t22_<index> = OpBitwiseAnd %uint %t2_<index> %uint_63
     OpStore %temp_uint_2 %t0_<index>
     OpStore %temp_uint_3 %t1_<index>
     OpStore %temp_uint_4 %t22_<index>
%t_<index> = OpFunctionCall %void %shift_right %temp_uint_0 %temp_uint_1 %temp_uint_2 %temp_uint_3 %temp_uint_4
%r0_<index> = OpLoad %uint %temp_uint_0 
%r1_<index> = OpLoad %uint %temp_uint_1
     OpStore %<dst0> %r0_<index>
     OpStore %<dst1> %r1_<index>
     <execz>
     <scc>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_S_Bfe_U64_Sdst2Ssrc02Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	String8 load0;
	String8 load1;
	String8 load2;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	static const char* text = R"(
     <load0>
     <load1>
     <load2>
     <param0>
     <param1>
     <param2>
     <param3>
 %to_<index> = OpBitFieldUExtract %uint %t2_<index> %uint_0  %uint_6
 %ts_<index> = OpBitFieldUExtract %uint %t2_<index> %uint_16 %uint_7
%tn0_<index> = OpISub %uint %uint_64 %to_<index>
%ts2_<index> = OpExtInst %uint %GLSL_std_450 UMin %ts_<index> %tn0_<index>
%tn1_<index> = OpISub %uint %uint_64 %ts2_<index> 
%tn2_<index> = OpISub %uint %tn1_<index> %to_<index>
     OpStore %temp_uint_2 %t0_<index>
     OpStore %temp_uint_3 %t1_<index>
     OpStore %temp_uint_4 %tn2_<index>
%tf1_<index> = OpFunctionCall %void %shift_left %temp_uint_0 %temp_uint_1 %temp_uint_2 %temp_uint_3 %temp_uint_4
     OpStore %temp_uint_4 %tn1_<index>
%tf2_<index> = OpFunctionCall %void %shift_right %temp_uint_2 %temp_uint_3 %temp_uint_0 %temp_uint_1 %temp_uint_4
 %r0_<index> = OpLoad %uint %temp_uint_2 
 %r1_<index> = OpLoad %uint %temp_uint_3
     OpStore %<dst0> %r0_<index>
     OpStore %<dst1> %r1_<index>
     <execz>
     <scc>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: And, Lshl, Lshr, CSelect, Or */
LUMEN_RECOMPILER_FUNC(Recompile_S_XXX_B32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Uint);
	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
              <load0>
              <load1>
              <param0>
              <param1>
              <param2>
              OpStore %<dst> %t_<index>
              <scc>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 1))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Add, Mul, Sub */
LUMEN_RECOMPILER_FUNC(Recompile_S_XXX_I32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Uint);
	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_int(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
              <load0>
              <load1>
              <param>
              %tu_<index> = OpBitcast %uint %t_<index>
              OpStore %<dst> %tu_<index>
              <scc>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 1))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Add, Addc, Bfe, Lshl4Add, MulHi */
LUMEN_RECOMPILER_FUNC(Recompile_S_XXX_U32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Uint);
	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
              <load0>
              <load1>
              <param0>
              <param1>
              <param2>
              <param3>
              OpStore %<dst> %t_<index>
              <scc>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 1))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_SAndSaveexecB64_Sdst2Ssrc02)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	String8 load0;
	String8 load1;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}

	static const char* text = R"(
        <load0>
        <load1>
        %t190_<index> = OpLoad %uint %exec_lo
               OpStore %<dst0> %t190_<index>
        %t191_<index> = OpLoad %uint %exec_hi
               OpStore %<dst1> %t191_<index>
        %t194_<index> = OpBitwiseAnd %uint %t0_<index> %t190_<index>
               OpStore %exec_lo %t194_<index>
        %t197_<index> = OpBitwiseAnd %uint %t1_<index> %t191_<index>
               OpStore %exec_hi %t197_<index>    
        <execz>
        <scc>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<execz>", EXECZ)
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Eq, Ge, Gt, Lg, Lt, Le */
LUMEN_RECOMPILER_FUNC(Recompile_SCmp_XXX_I32_Ssrc0Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_int(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <param> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %scc %t3_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Eq, Le, Lg, Gt, Lt */
LUMEN_RECOMPILER_FUNC(Recompile_SCmp_XXX_U32_Ssrc0Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <param> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %scc %t3_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_SBufferLoadDword_SdstSvSoffset)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[1]));

		auto    dst_value   = operand_variable_to_str(inst.dst);
		auto    src0_value0 = operand_variable_to_str(inst.src[0], 0);
		String8 offset      = spirv->GetConstant(inst.src[1]);

		EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

		static const char* text = R"(
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
        %t102_<index> = OpBitcast %int %<offset>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword %<p0> %temp_int_1 %temp_int_2 
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<p0>", dst_value.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_SBufferLoadDwordx2_Sdst2SvSoffset)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[1]));

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    src0_value0 = operand_variable_to_str(inst.src[0], 0);
		String8 offset      = spirv->GetConstant(inst.src[1]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

		EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

		static const char* text = R"(
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
        %t102_<index> = OpBitcast %int %<offset>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword_2 %<p0> %<p1> %temp_int_1 %temp_int_2 
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_SBufferLoadDwordx4_Sdst4SvSoffset)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		// EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[1]));

		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
		// String8 offset      = spirv->GetConstant(inst.src[1]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

		EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

		String8 index_str = String8::FromPrintf("%u", index);

		String8 load1;

		if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
		{
			return false;
		}

		static const char* text = R"(
        <load1>
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
        %t102_<index> = OpBitcast %int %t1_<index>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword_4 %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2 
)";
		*dst_source += String8(text)
		                   //.ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<load1>", load1)
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value)
		                   .ReplaceStr("<p3>", dst_value3.value)
		                   .ReplaceStr("<index>", index_str);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_SBufferLoadDwordx8_Sdst8SvSoffset)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[1]));

		SpirvValue dst_value[8];

		for (int i = 0; i < 8; i++)
		{
			dst_value[i] = operand_variable_to_str(inst.dst, i);
		}

		auto    src0_value0 = operand_variable_to_str(inst.src[0], 0);
		String8 offset      = spirv->GetConstant(inst.src[1]);

		EXIT_NOT_IMPLEMENTED(dst_value[0].type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Uint);

		EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

		String8 text = R"(
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
        %t102_<index> = OpBitcast %int %<offset>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword_8 %<p0> %<p1> %<p2> %<p3> %<p4> %<p5> %<p6> %<p7> %temp_int_1 %temp_int_2 
)";

		for (int i = 0; i < 8; i++)
		{
			text = text.ReplaceStr(String8::FromPrintf("<p%d>", i), dst_value[i].value);
		}

		*dst_source += text.ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src0_value0>", src0_value0.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_SBufferLoadDwordx16_Sdst16SvSoffset)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[1]));

		SpirvValue dst_value[16];

		for (int i = 0; i < 16; i++)
		{
			dst_value[i] = operand_variable_to_str(inst.dst, i);
		}

		auto    src0_value0 = operand_variable_to_str(inst.src[0], 0);
		String8 offset      = spirv->GetConstant(inst.src[1]);

		EXIT_NOT_IMPLEMENTED(dst_value[0].type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Uint);

		EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

		String8 text = R"(
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
        %t102_<index> = OpBitcast %int %<offset>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword_16 %<p0> %<p1> %<p2> %<p3> %<p4> %<p5> %<p6> %<p7> %<p8> %<p9> %<p10> %<p11> %<p12> %<p13> %<p14> %<p15> %temp_int_1 %temp_int_2 
)";

		for (int i = 0; i < 16; i++)
		{
			text = text.ReplaceStr(String8::FromPrintf("<p%d>", i), dst_value[i].value);
		}

		*dst_source += text.ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src0_value0>", src0_value0.value);

		return true;
	}

	return false;
}

// LUMEN_RECOMPILER_FUNC(Recompile_SCbranchExecz_Label)
//{
//	const auto& inst = code.GetInstructions().At(index);
//
//	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));
//
//	EXIT_NOT_IMPLEMENTED(code.ReadBlock(ShaderLabel(inst).GetDst()).is_discard);
//
//	String8 label = ShaderLabel(inst).ToString();
//
//	static const char* text = R"(
//         %execz_u_<index> = OpLoad %uint %execz
//         %execz_b_<index> = OpINotEqual %bool %execz_u_<index> %uint_0
//                OpSelectionMerge %<label> None
//                OpBranchConditional %execz_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//
//	*dst_source += String8(text).ReplaceStr("<index>", String8::FromPrintf("%u", index)).ReplaceStr("<label>", label);
//
//	return true;
// }

LUMEN_RECOMPILER_FUNC(Recompile_SBranch_Label)
{
	const auto& inst = code.GetInstructions().At(index);

	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));

	EXIT_NOT_IMPLEMENTED(code.ReadBlock(ShaderLabel(inst).GetDst()).is_discard);

	String8 label = ShaderLabel(inst).ToString();

	static const char* text = R"(
                OpBranch %<label>
)";

	*dst_source += String8(text).ReplaceStr("<index>", String8::FromPrintf("%u", index)).ReplaceStr("<label>", label);

	return true;
}

/* XXX: Execz, Scc0, Scc1, Vccz, Vccnz */
LUMEN_RECOMPILER_FUNC(Recompile_SCbranch_XXX_Label)
{
	EXIT_NOT_IMPLEMENTED(index + 1 >= code.GetInstructions().Size());

	const auto& inst      = code.GetInstructions().At(index);
	const auto& next_inst = code.GetInstructions().At(index + 1);

	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));

	// TODO(): analyze control flow graph
	auto label            = ShaderLabel(inst);
	auto dst_block        = code.ReadBlock(label.GetDst());
	auto next_block       = code.ReadBlock(next_inst.pc);
	bool discard          = dst_block.is_discard;
	auto label_next_block = ShaderLabel(next_block.last);
	auto label_dst_block  = ShaderLabel(dst_block.last);

	bool if_else = next_block.is_valid && !next_block.is_discard && dst_block.is_valid && !dst_block.is_discard &&
	               ((next_block.last.type == ShaderInstructionType::SBranch && label_next_block.GetDst() >= dst_block.pc &&
	                 label_next_block.GetDst() <= dst_block.last.pc) ||
	                (dst_block.last.type == ShaderInstructionType::SBranch && label_dst_block.GetDst() >= next_block.pc &&
	                 label_dst_block.GetDst() <= next_block.last.pc));

	String8 label_str = label.ToString();
	String8 label_merge =
	    if_else ? (dst_block.last.type == ShaderInstructionType::SBranch ? label_dst_block.ToString() : label_next_block.ToString()) : "";
	if (!if_else && spirv->GetVsInputInfo() != nullptr && ShaderVertexResourceProfileRequired(*spirv->GetVsInputInfo()) &&
	    next_block.is_valid && !next_block.is_discard && next_block.last.type == ShaderInstructionType::SBranch &&
	    VertexForwardPathsReach(code, label.GetDst(), label_next_block.GetDst()))
	{
		// Preserve the two guest successors; only the structural merge changes.
		// Existing label emission orders shared joins from inner to outer scope.
		if_else = true;
		label_merge = label_next_block.ToString();
	}

	//	if (condition)
	//	{
	//		L1:
	//		...
	//	}
	// L2: /* merge */
	//	...
	static const char* text_variant_a = R"(
        <param0>
        <param1>
               OpSelectionMerge %<label> None
               OpBranchConditional %cc_b_<index> %<label> %t230_<index>
        %t230_<index> = OpLabel
)";

	//	if (condition)
	//	{
	//		L2:
	//		...
	//		discard;
	//	}
	// L1: /* merge */
	//	...
	static const char* text_variant_b = R"(
        <param0>
        <param1>
               OpSelectionMerge %t230_<index> None
               OpBranchConditional %cc_b_<index> %<label> %t230_<index>
        %t230_<index> = OpLabel
)";

	//	if (condition)
	//	{
	//		L1:
	//		...
	//	} else
	//	{
	// 		L2:
	//		...
	//	}
	//	 /* merge */
	static const char* text_variant_c = R"(
        <param0>
        <param1>
               OpSelectionMerge %<merge> None
               OpBranchConditional %cc_b_<index> %<label> %t230_<index>
        %t230_<index> = OpLabel
)";

	*dst_source += String8(if_else ? text_variant_c : (discard ? text_variant_b : text_variant_a))
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", param[1])
	                   .ReplaceStr("<merge>", label_merge)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<label>", label_str);

	return true;
}

// LUMEN_RECOMPILER_FUNC(Recompile_SCbranchScc0_Label)
//{
//	const auto& inst = code.GetInstructions().At(index);
//
//	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));
//
//	auto label = ShaderLabel(inst);
//
//	// TODO(): analyze control flow graph
//	bool discard = code.ReadBlock(label.GetDst()).is_discard;
//
//	String8 label_str = label.ToString();
//
//	static const char* text_variant_a = R"(
//         %scc_u_<index> = OpLoad %uint %scc
//         %scc_b_<index> = OpIEqual %bool %scc_u_<index> %uint_0
//                OpSelectionMerge %<label> None
//                OpBranchConditional %scc_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//	static const char* text_variant_b = R"(
//         %scc_u_<index> = OpLoad %uint %scc
//         %scc_b_<index> = OpIEqual %bool %scc_u_<index> %uint_0
//                OpSelectionMerge %t230_<index> None
//                OpBranchConditional %scc_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//
//	*dst_source += String8(discard ? text_variant_b : text_variant_a)
//	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
//	                   .ReplaceStr("<label>", label_str);
//
//	return true;
// }

// LUMEN_RECOMPILER_FUNC(Recompile_SCbranchScc1_Label)
//{
//	const auto& inst = code.GetInstructions().At(index);
//
//	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));
//
//	auto label = ShaderLabel(inst);
//
//	// TODO(): analyze control flow graph
//	bool discard = code.ReadBlock(label.GetDst()).is_discard;
//
//	String8 label_str = label.ToString();
//
//	static const char* text_variant_a = R"(
//         %scc_u_<index> = OpLoad %uint %scc
//         %scc_b_<index> = OpIEqual %bool %scc_u_<index> %uint_1
//                OpSelectionMerge %<label> None
//                OpBranchConditional %scc_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//	static const char* text_variant_b = R"(
//         %scc_u_<index> = OpLoad %uint %scc
//         %scc_b_<index> = OpIEqual %bool %scc_u_<index> %uint_1
//                OpSelectionMerge %t230_<index> None
//                OpBranchConditional %scc_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//
//	*dst_source += String8(discard ? text_variant_b : text_variant_a)
//	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
//	                   .ReplaceStr("<label>", label_str);
//
//	return true;
// }
//
// LUMEN_RECOMPILER_FUNC(Recompile_SCbranchVccz_Label)
//{
//	const auto& inst = code.GetInstructions().At(index);
//
//	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));
//
//	auto label = ShaderLabel(inst);
//
//	// TODO(): analyze control flow graph
//	bool discard = code.ReadBlock(label.GetDst()).is_discard;
//
//	String8 label_str = label.ToString();
//
//	static const char* text_variant_a = R"(
//         %vcc_lo_u_<index> = OpLoad %uint %vcc_lo
//         %vcc_lo_b_<index> = OpIEqual %bool %vcc_lo_u_<index> %uint_0
//                OpSelectionMerge %<label> None
//                OpBranchConditional %vcc_lo_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//	static const char* text_variant_b = R"(
//         %vcc_lo_u_<index> = OpLoad %uint %vcc_lo
//         %vcc_lo_b_<index> = OpIEqual %bool %vcc_lo_u_<index> %uint_0
//                OpSelectionMerge %t230_<index> None
//                OpBranchConditional %vcc_lo_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//
//	*dst_source += String8(discard ? text_variant_b : text_variant_a)
//	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
//	                   .ReplaceStr("<label>", label_str);
//
//	return true;
// }

LUMEN_RECOMPILER_FUNC(Recompile_SEndpgm_Empty)
{
	// const auto* info = spirv->GetPsInputInfo();

	// EXIT_NOT_IMPLEMENTED(info == nullptr || !info->ps_pixel_kill_enable);

	static const char* text = R"(
       OpReturn
)";

	EXIT_NOT_IMPLEMENTED(index < 2);

	const auto& prev_prev_inst = code.GetInstructions().At(index - 2);
	const auto& prev_inst      = code.GetInstructions().At(index - 1);

	bool after_kill =
	    (prev_prev_inst.type == ShaderInstructionType::SMovB64 && prev_prev_inst.format == ShaderInstructionFormat::Sdst2Ssrc02 &&
	     prev_prev_inst.dst.type == ShaderOperandType::ExecLo && prev_prev_inst.src[0].type == ShaderOperandType::IntegerInlineConstant &&
	     prev_prev_inst.src[0].constant.i == 0 && prev_inst.type == ShaderInstructionType::Exp &&
	     prev_inst.format == ShaderInstructionFormat::Mrt0OffOffComprVmDone);

	if (!after_kill)
	{
		*dst_source += String8(text);
	}

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_SLoadDwordx4_Sdst4SbaseSoffset)
{
	const auto& inst = code.GetInstructions().At(index);

	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->extended.used)
	{
		EXIT_NOT_IMPLEMENTED(inst.src[1].type != ShaderOperandType::LiteralConstant);
		EXIT_NOT_IMPLEMENTED(inst.src[0].register_id != bind_info->extended.start_register);

		// TODO() check pointer

		SpirvValue dst_value[4];

		for (int i = 0; i < 4; i++)
		{
			dst_value[i] = operand_variable_to_str(inst.dst, i);
		}
		auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
		auto src0_value1 = operand_variable_to_str(inst.src[0], 1);
		int  offset      = static_cast<int>(inst.src[1].constant.u >> 2u);

		EXIT_NOT_IMPLEMENTED(dst_value[0].type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src0_value1.type != SpirvType::Uint);

		static const char* text = R"(
		         %vsharp_<index>_<reg> = OpAccessChain %_ptr_PushConstant_uint %vsharp %int_0 %int_<buffer> %int_<field>
		         %vsharp_<index>_value_<reg> = OpLoad %uint %vsharp_<index>_<reg>
		               OpStore %<reg> %vsharp_<index>_value_<reg>
				)";

		for (int i = 0; i < 4; i++)
		{
			int buffer = 0;
			int field  = 0;
			spirv->GetMappedIndex(offset + i, &buffer, &field);

			*dst_source += String8(text)
			                   .ReplaceStr("<reg>", dst_value[i].value)
			                   .ReplaceStr("<buffer>", String8::FromPrintf("%d", buffer))
			                   .ReplaceStr("<field>", String8::FromPrintf("%d", field))
			                   .ReplaceStr("<index>", String8::FromPrintf("%u", index));
		}

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_SLoadDwordx8_Sdst8SbaseSoffset)
{
	const auto& inst = code.GetInstructions().At(index);

	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->extended.used)
	{
		EXIT_NOT_IMPLEMENTED(inst.src[1].type != ShaderOperandType::LiteralConstant);
		EXIT_NOT_IMPLEMENTED(inst.src[0].register_id != bind_info->extended.start_register);

		// TODO() check pointer

		SpirvValue dst_value[8];

		for (int i = 0; i < 8; i++)
		{
			dst_value[i] = operand_variable_to_str(inst.dst, i);
		}
		auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
		auto src0_value1 = operand_variable_to_str(inst.src[0], 1);
		int  offset      = static_cast<int>(inst.src[1].constant.u >> 2u);

		EXIT_NOT_IMPLEMENTED(dst_value[0].type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src0_value1.type != SpirvType::Uint);

		static const char* text = R"(
		         %vsharp_<index>_<reg> = OpAccessChain %_ptr_PushConstant_uint %vsharp %int_0 %int_<buffer> %int_<field>
		         %vsharp_<index>_value_<reg> = OpLoad %uint %vsharp_<index>_<reg>
		               OpStore %<reg> %vsharp_<index>_value_<reg>
				)";

		for (int i = 0; i < 8; i++)
		{
			int buffer = 0;
			int field  = 0;
			spirv->GetMappedIndex(offset + i, &buffer, &field);

			*dst_source += String8(text)
			                   .ReplaceStr("<reg>", dst_value[i].value)
			                   .ReplaceStr("<buffer>", String8::FromPrintf("%d", buffer))
			                   .ReplaceStr("<field>", String8::FromPrintf("%d", field))
			                   .ReplaceStr("<index>", String8::FromPrintf("%u", index));
		}

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_SMulkI32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Uint);
	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	String8 load0;

	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	String8 load_dst;

	if (!operand_load_int(spirv, inst.dst, "tdst_<index>", index_str, &load_dst))
	{
		return false;
	}

	static const char* text = R"(    
    <load0>
    <load_dst>
%t_<index> = OpIMul %int %tdst_<index> %t0_<index>
%tu_<index> = OpBitcast %uint %t_<index>
    OpStore %<dst> %tu_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load_dst>", load_dst)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_SMovB32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Uint);
	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	String8 load0;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	static const char* text = R"(    
    <load0>
    OpStore %<dst> %t0_<index>
)";
	*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<load0>", load0).ReplaceStr("<index>", index_str);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_SMovB64_Sdst2Ssrc02)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	// EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	String8 load0;
	String8 load1;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}

	static const char* text = R"(    
    <load0>
    <load1>
    OpStore %<dst0> %t0_<index>
    OpStore %<dst1> %t1_<index>
    <execz>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_SSwappcB64_Sdst2Ssrc02)
{
	const auto& inst       = code.GetInstructions().At(index);
	const auto* input_info = spirv->GetVsInputInfo();

	if (input_info != nullptr)
	{
		EXIT_NOT_IMPLEMENTED(!input_info->fetch_external);
		EXIT_NOT_IMPLEMENTED(input_info->fetch_shader_reg != 0);
	}

	bool entry_supported = index == 1;
	if (!entry_supported && input_info != nullptr)
	{
		uint32_t entry_index = UINT32_MAX;
		const char* rejection = "vertex fetch entry index";
		entry_supported = VertexExternalFetchEntryCall(code, *input_info, &entry_index, &rejection) &&
		                  entry_index == static_cast<uint32_t>(index) && ShaderVertexResourceProfileSupported(code, *input_info, &rejection);
		if (!entry_supported)
			std::fprintf(stderr, "[SHADER_FETCH_ENTRY_REJECT] index=%u pc=%08" PRIx32 " reason=%s\n",
			             static_cast<uint32_t>(index), inst.pc, rejection);
	}
	if (input_info != nullptr && input_info->fetch_external && inst.dst.type == ShaderOperandType::Sgpr && inst.dst.register_id == 0 &&
	    inst.src[0].type == ShaderOperandType::Sgpr && inst.src[0].register_id == 0 && entry_supported)
	{
		for (int i = 0; i < input_info->resources_num; i++)
		{
			const auto& r = input_info->resources_dst[i];

			String8 text;

			if (ShaderVertexInputSint16x2Compatible(input_info->resources[i], r.registers_num))
			{
				text = R"(
				         %t1_<index> = OpLoad %v2int %<attr>
				         %t2_<index> = OpBitcast %v2float %t1_<index>
				                       OpStore %temp_v2float %t2_<index>
				         %t3_<index> = OpFunctionCall %void %fetch_f1_f1_vf2_ %<p0> %<p1> %temp_v2float
				)";
			} else if (ShaderVertexInputSint16x4Compatible(input_info->resources[i], r.registers_num))
			{
				text = R"(
				         %t1_<index> = OpLoad %v4int %<attr>
				         %t2_<index> = OpBitcast %v4float %t1_<index>
				                       OpStore %temp_v4float %t2_<index>
				         %t3_<index> = OpFunctionCall %void %fetch_f1_f1_f1_f1_vf4_ %<p0> %<p1> %<p2> %<p3> %temp_v4float
				)";
			} else
			{
				switch (r.registers_num)
				{
					case 1:
						text = R"(
				         %t1_<index> = OpLoad %float %<attr> 
				                       OpStore %temp_float %t1_<index> 
				         %t2_<index> = OpFunctionCall %void %fetch_f1_f1_ %<p0> %temp_float
				)";
						break;
					case 2:
						text = R"(
				         %t1_<index> = OpLoad %v2float %<attr> 
				                       OpStore %temp_v2float %t1_<index> 
				         %t2_<index> = OpFunctionCall %void %fetch_f1_f1_vf2_ %<p0> %<p1> %temp_v2float
				)";
						break;
					case 3:
						text = R"(
				         %t1_<index> = OpLoad %v3float %<attr> 
				                       OpStore %temp_v3float %t1_<index> 
				         %t2_<index> = OpFunctionCall %void %fetch_f1_f1_f1_vf3_ %<p0> %<p1> %<p2> %temp_v3float
				)";
						break;
					case 4:
					{
						text = R"(
				         %t1_<index> = OpLoad %v4float %<attr>
				         <swizzle>
				                       OpStore %temp_v4float %<value>
				         %t2_<index> = OpFunctionCall %void %fetch_f1_f1_f1_f1_vf4_ %<p0> %<p1> %<p2> %<p3> %temp_v4float
				)";
						const auto& resource = input_info->resources[i];
						String8 swizzle;
						String8 value = "t1_<index>";
						if (resource.DstSelXYZW() != DstSel(4, 5, 6, 7) &&
						    ShaderVertexInputFloat4SwizzleCompatible(resource, r.registers_num))
						{
							// Keep the physical vertex format unchanged. Apply the descriptor
							// selection once, after host normalization and before guest VGPRs.
							const auto lane = [](uint32_t sel) { return sel < 2 ? sel + 4 : sel - 4; };
							swizzle = String8::FromPrintf(
							    "%%swizzle_constants_<index> = OpCompositeConstruct %%v4float %%float_0_000000 %%float_1_000000 %%float_0_000000 %%float_0_000000\n"
							    "%%swizzle_value_<index> = OpVectorShuffle %%v4float %%t1_<index> %%swizzle_constants_<index> %u %u %u %u\n",
							    lane(resource.DstSelX()), lane(resource.DstSelY()), lane(resource.DstSelZ()), lane(resource.DstSelW()));
							value = "swizzle_value_<index>";
						}
						text = text.ReplaceStr("<swizzle>", swizzle).ReplaceStr("<value>", value);
						break;
					}
					default: EXIT("invalid registers_num: %d\n", r.registers_num);
				}
			}

			*dst_source += String8(text)
			                   .ReplaceStr("<index>", String8::FromPrintf("%d_%u", i, index))
			                   .ReplaceStr("<p0>", String8::FromPrintf("v%d", r.register_start + 0))
			                   .ReplaceStr("<p1>", String8::FromPrintf("v%d", r.register_start + 1))
			                   .ReplaceStr("<p2>", String8::FromPrintf("v%d", r.register_start + 2))
			                   .ReplaceStr("<p3>", String8::FromPrintf("v%d", r.register_start + 3))
			                   .ReplaceStr("<attr>", String8::FromPrintf("attr%d", i));
		}
		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_SWqmB64_Sdst2Ssrc02)
{
	const auto& inst = code.GetInstructions().At(index);

	if (inst.dst.type == ShaderOperandType::ExecLo && inst.src[0].type == ShaderOperandType::ExecLo)
	{
		return true;
	}

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	// EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	String8 load0;
	String8 load1;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}

	static const char* text = R"(
        <load0>
        <load1>
        %t170_<index> = OpFunctionCall %uint %wqm %t0_<index> %uint_0 %uint_15
        %t172_<index> = OpBitwiseOr %uint %uint_0 %t170_<index>
        %t179_<index> = OpFunctionCall %uint %wqm %t0_<index> %uint_4 %uint_240
        %t181_<index> = OpBitwiseOr %uint %t172_<index> %t179_<index>
        %t188_<index> = OpFunctionCall %uint %wqm %t0_<index> %uint_8 %uint_0x00000f00
        %t190_<index> = OpBitwiseOr %uint %t181_<index> %t188_<index>
        %t197_<index> = OpFunctionCall %uint %wqm %t0_<index> %uint_12 %uint_0x0000f000
        %t199_<index> = OpBitwiseOr %uint %t190_<index> %t197_<index>
        %t206_<index> = OpFunctionCall %uint %wqm %t0_<index> %uint_16 %uint_0x000f0000
        %t208_<index> = OpBitwiseOr %uint %t199_<index> %t206_<index>
        %t215_<index> = OpFunctionCall %uint %wqm %t0_<index> %uint_20 %uint_0x00f00000
        %t217_<index> = OpBitwiseOr %uint %t208_<index> %t215_<index>
        %t224_<index> = OpFunctionCall %uint %wqm %t0_<index> %uint_24 %uint_0x0f000000
        %t226_<index> = OpBitwiseOr %uint %t217_<index> %t224_<index>
        %t233_<index> = OpFunctionCall %uint %wqm %t0_<index> %uint_28 %uint_0xf0000000
        %t235_<index> = OpBitwiseOr %uint %t226_<index> %t233_<index>
        %t1701_<index> = OpFunctionCall %uint %wqm %t1_<index> %uint_0 %uint_15
        %t1721_<index> = OpBitwiseOr %uint %uint_0 %t1701_<index>
        %t1791_<index> = OpFunctionCall %uint %wqm %t1_<index> %uint_4 %uint_240
        %t1811_<index> = OpBitwiseOr %uint %t1721_<index> %t1791_<index>
        %t1881_<index> = OpFunctionCall %uint %wqm %t1_<index> %uint_8 %uint_0x00000f00
        %t1901_<index> = OpBitwiseOr %uint %t1811_<index> %t1881_<index>
        %t1971_<index> = OpFunctionCall %uint %wqm %t1_<index> %uint_12 %uint_0x0000f000
        %t1991_<index> = OpBitwiseOr %uint %t1901_<index> %t1971_<index>
        %t2061_<index> = OpFunctionCall %uint %wqm %t1_<index> %uint_16 %uint_0x000f0000
        %t2081_<index> = OpBitwiseOr %uint %t1991_<index> %t2061_<index>
        %t2151_<index> = OpFunctionCall %uint %wqm %t1_<index> %uint_20 %uint_0x00f00000
        %t2171_<index> = OpBitwiseOr %uint %t2081_<index> %t2151_<index>
        %t2241_<index> = OpFunctionCall %uint %wqm %t1_<index> %uint_24 %uint_0x0f000000
        %t2261_<index> = OpBitwiseOr %uint %t2171_<index> %t2241_<index>
        %t2331_<index> = OpFunctionCall %uint %wqm %t1_<index> %uint_28 %uint_0xf0000000
        %t2351_<index> = OpBitwiseOr %uint %t2261_<index> %t2331_<index>
               OpStore %<dst0> %t235_<index>
               OpStore %<dst1> %t2351_<index>
        <execz>
        <scc>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* SInstPrefetch, SWaitcnt, SSendmsg */
LUMEN_RECOMPILER_FUNC(Recompile_Skip)
{
	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_STTraceData_Imm)
{
	if (!ShaderThreadTraceDataInstructionSupported(code, code.GetInstructions().At(index))) return false;
	// TTRACE_EN=0 suppresses this event. No register, memory or ordering
	// instruction is emitted for the dedicated trace IR.
	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_TBufferLoadFormatX_Vdata1VaddrSvSoffsIdxenFloat1)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value0  = operand_variable_to_str(inst.dst);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = spirv->GetConstant(inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>                
               OpStore %temp_int_2 %<offset> 
               OpStore %temp_int_5 %int_36
        %t110_<index> = OpFunctionCall %void %tbuffer_load_format_x %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5 
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_TBufferLoadFormatXyzw_Vdata4VaddrSvSoffsIdxenFloat4)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto    dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = spirv->GetConstant(inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>                
               OpStore %temp_int_2 %<offset> 
               OpStore %temp_int_5 %int_119
        %t110_<index> = OpFunctionCall %void %tbuffer_load_format_xyzw %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5 
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value)
		                   .ReplaceStr("<p3>", dst_value3.value);

		return true;
	}

	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_TBufferLoadFormatXyzw_Vdata4Vaddr2SvSoffsOffenIdxenFloat4)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto    dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto    src0_value0 = operand_variable_to_str(inst.src[0], 0);
		auto    src0_value1 = operand_variable_to_str(inst.src[0], 1);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = spirv->GetConstant(inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value1.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
       %to100_<index> = OpLoad %float %<src0_value1>
       %to101_<index> = OpBitcast %int %to100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
      %offset_<index> = OpIAdd %int %to101_<index> %<offset>
               OpStore %temp_int_4 %t156_<index>                
               OpStore %temp_int_2 %offset_<index> 
               OpStore %temp_int_5 %int_119
        %t110_<index> = OpFunctionCall %void %tbuffer_load_format_xyzw %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5 
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value)
		                   .ReplaceStr("<p3>", dst_value3.value);

		return true;
	}

	return false;
}

/* XXX: F, Eq, Ge, Gt, Le, Lg, Lt, Neq, Nge, Ngt, Nlg, Nlt, O, Tru, U */
LUMEN_RECOMPILER_FUNC(Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <param> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %<dst0> %t3_<index>
          OpStore %<dst1> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Eq, Ne, Gt, Ge, F, Le, T */
LUMEN_RECOMPILER_FUNC(Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_int(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <param> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %<dst0> %t3_<index>
          OpStore %<dst1> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Le, Ge, F, Gt, Lt, T */
LUMEN_RECOMPILER_FUNC(Recompile_VCmp_XXX_U32_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <param> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %<dst0> %t3_<index>
          OpStore %<dst1> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Eq, Ne */
LUMEN_RECOMPILER_FUNC(Recompile_VCmpx_XXX_I32_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_int(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <param> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %<dst0> %t3_<index>
          OpStore %<dst1> %uint_0
          OpStore %exec_lo %t3_<index>
          OpStore %exec_hi %uint_0
          <execz>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<execz>", EXECZ)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Gt, Ge */
LUMEN_RECOMPILER_FUNC(Recompile_VCmpx_XXX_U32_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <param> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %<dst0> %t3_<index>
          OpStore %<dst1> %uint_0
          OpStore %exec_lo %t3_<index>
          OpStore %exec_hi %uint_0
          <execz>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<execz>", EXECZ)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Neq, Gt, Lt */
LUMEN_RECOMPILER_FUNC(Recompile_VCmpx_XXX_F32_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <param> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %<dst0> %t3_<index>
          OpStore %<dst1> %uint_0
          OpStore %exec_lo %t3_<index>
          OpStore %exec_hi %uint_0
          <execz>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<execz>", EXECZ)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_VCndmaskB32_VdstVsrc0Vsrc1Smask2)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[2]));

	auto src_bool_value0 = operand_variable_to_str(inst.src[2], 0);
	auto src_bool_value1 = operand_variable_to_str(inst.src[2], 1);

	EXIT_NOT_IMPLEMENTED(src_bool_value0.type != SpirvType::Uint);

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(    
    <load0>
    <load1>
    %t22_<index> = OpLoad %uint %<src0>
    %t23_<index> = OpLoad %uint %<src1> ; unused
    %tb_<index> = OpBitwiseAnd %uint %t22_<index> %uint_1
    %t2_<index> = OpINotEqual %bool %tb_<index> %uint_0
    %t3_<index> = OpSelect %float %t2_<index> %t1_<index> %t0_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t3_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<src0>", src_bool_value0.value)
	                   .ReplaceStr("<src1>", src_bool_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_VCvtPkrtzF16F32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check DX10_CLAMP

	static const char* text = R"(    
    <load0>
    <load1>
    %t0u_<index> = OpBitcast %uint %t0_<index>
    %t0uu_<index> = OpBitwiseAnd %uint %t0u_<index> %uint_0xffffe000
    %t0f_<index> = OpBitcast %float %t0uu_<index>
    %t1u_<index> = OpBitcast %uint %t1_<index>
    %t1uu_<index> = OpBitwiseAnd %uint %t1u_<index> %uint_0xffffe000
    %t1f_<index> = OpBitcast %float %t1uu_<index>
    %t2_<index> = OpCompositeConstruct %v2float %t0f_<index> %t1f_<index> 
    %t3_<index> = OpExtInst %uint %GLSL_std_450 PackHalf2x16 %t2_<index>
    %t4_<index> = OpBitcast %float %t3_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t4_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_VInterpP1F32_VdstVsrcAttrChan)
{
	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_VInterpP2F32_VdstVsrcAttrChan)
{
	const auto& inst = code.GetInstructions().At(index);
	const auto* pixel = spirv->GetPsInputInfo();
	if (code.GetType() != ShaderType::Pixel || pixel == nullptr || pixel->input_num > 32 ||
	    inst.src[1].constant.u >= pixel->input_num || inst.src[2].constant.u > 3) return false;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));
	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[1]));
	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

	auto dst_value = operand_variable_to_str(inst.dst);

	String8 load0 = String8::FromPrintf("%%t0_<index> = OpAccessChain %%_ptr_Input_float %%attr%u %%uint_%u", inst.src[1].constant.u,
	                                    inst.src[2].constant.u);

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(    
         <load0>
         %t1_<index> = OpLoad %float %t0_<index>
                       OpStore %<dst> %t1_<index>
)";
	*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<load0>", load0).ReplaceStr("<index>", index_str);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_VInterpMovF32_VdstVsrcAttrChan)
{
	const auto& inst = code.GetInstructions().At(index);
	const auto* pixel = spirv->GetPsInputInfo();
	if (code.GetType() != ShaderType::Pixel || pixel == nullptr || pixel->input_num > 32 ||
	    inst.src[1].constant.u >= pixel->input_num || inst.src[2].constant.u > 3) return false;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));
	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[1]));
	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

	EXIT_NOT_IMPLEMENTED(inst.src[0].constant.u != 2);

	auto dst_value = operand_variable_to_str(inst.dst);

	String8 load0 = String8::FromPrintf("%%t0_<index> = OpAccessChain %%_ptr_Input_float %%attr%u %%uint_%u", inst.src[1].constant.u,
	                                    inst.src[2].constant.u);

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(    
         <load0>
         %t1_<index> = OpLoad %float %t0_<index>
                       OpStore %<dst> %t1_<index>
)";
	*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<load0>", load0).ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Mad, Madak, Madmk, Max3, Min3, Med3, Fma */
LUMEN_RECOMPILER_FUNC(Recompile_V_XXX_F32_VdstVsrc0Vsrc1Vsrc2)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;
	String8 load2;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	// EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	// EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[2], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check SP_ROUND
	// TODO() check DX10_CLAMP
	// TODO() check IEEE

	static const char* text = R"(
              <load0>
              <load1>
              <load2>
              <param0>
              <param1>
              <param2>
              <param3>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %tl2_<index> None
               OpBranchConditional %exec_lo_b_<index> %tl1_<index> %tl2_<index>
         %tl1_<index> = OpLabel
               OpStore %<dst> %t_<index>
              <multiply>
              <clamp>
               OpBranch %tl2_<index>
         %tl2_<index> = OpLabel
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<multiply>", (inst.dst.multiplier != 1.0f
	                                                  ? String8(MULTIPLY).ReplaceStr("<mul>", spirv->GetConstantFloat(inst.dst.multiplier))
	                                                  : ""))
	                   .ReplaceStr("<clamp>", (inst.dst.clamp ? CLAMP : ""))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_VMbcntHiU32B32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	// if (inst.src[0].type == ShaderOperandType::ExecHi)
	//{
	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(    
	    <load0>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t1_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
	)";
	*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<load0>", load0).ReplaceStr("<index>", index_str);

	return true;
	//}

	//	return false;
}

LUMEN_RECOMPILER_FUNC(Recompile_VMbcntLoU32B32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	// if (inst.src[0].type == ShaderOperandType::ExecLo)
	//{
	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(    
	    <load0>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t1_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
	)";
	*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<load0>", load0).ReplaceStr("<index>", index_str);

	return true;
	//}

	// return false;
}

/* XXX: Bfrev, Not */
LUMEN_RECOMPILER_FUNC(Recompile_V_XXX_B32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(    
              <load0>
              <param0>
              %tf_<index> = OpBitcast %float %t_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %tf_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_VMovB32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(    
    <load0>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t0_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<load0>", load0).ReplaceStr("<index>", index_str);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_VMulLegacyF32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);
	const auto  type = code.GetType();

	EXIT_NOT_IMPLEMENTED(type != ShaderType::Vertex && type != ShaderType::Pixel);
	EXIT_NOT_IMPLEMENTED(code.GetFp32Mode() != ShaderFp32Mode::RneFlushInOut);
	EXIT_NOT_IMPLEMENTED(inst.format != ShaderInstructionFormat::SVdstSVsrc0SVsrc1 || inst.src_num != 2);
	EXIT_NOT_IMPLEMENTED(inst.src[0].absolute || inst.src[1].absolute || inst.src[0].negate || inst.src[1].negate);
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp || inst.dst.multiplier != 1.0f);
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	const auto dst_value = operand_variable_to_str(inst.dst);
	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	const String8 index_str = String8::FromPrintf("%u", index);
	String8       load0;
	String8       load1;
	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0) ||
	    !operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// FLOAT_MODE 0xC0 for the admitted graphics path is RNE with F32 input and
	// output denorm flush. Classify exponent-zero inputs as effective zero using
	// integer operations so NaNs are not touched by an eager floating operation.
	static const char* text = R"(
              <load0>
              <load1>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %vmul_legacy_exec_merge_<index> None
               OpBranchConditional %exec_lo_b_<index> %vmul_legacy_exec_<index> %vmul_legacy_exec_merge_<index>
 %vmul_legacy_exec_<index> = OpLabel
 %vmul_legacy_src0_bits_<index> = OpBitcast %uint %t0_<index>
 %vmul_legacy_src1_bits_<index> = OpBitcast %uint %t1_<index>
  %vmul_legacy_src0_exp_<index> = OpBitFieldUExtract %uint %vmul_legacy_src0_bits_<index> %uint_23 %uint_8
  %vmul_legacy_src1_exp_<index> = OpBitFieldUExtract %uint %vmul_legacy_src1_bits_<index> %uint_23 %uint_8
 %vmul_legacy_src0_zero_<index> = OpIEqual %bool %vmul_legacy_src0_exp_<index> %uint_0
 %vmul_legacy_src1_zero_<index> = OpIEqual %bool %vmul_legacy_src1_exp_<index> %uint_0
  %vmul_legacy_any_zero_<index> = OpLogicalOr %bool %vmul_legacy_src0_zero_<index> %vmul_legacy_src1_zero_<index>
               OpSelectionMerge %vmul_legacy_value_merge_<index> None
               OpBranchConditional %vmul_legacy_any_zero_<index> %vmul_legacy_zero_<index> %vmul_legacy_multiply_<index>
 %vmul_legacy_zero_<index> = OpLabel
               OpBranch %vmul_legacy_value_merge_<index>
 %vmul_legacy_multiply_<index> = OpLabel
    %vmul_legacy_product_<index> = OpFMul %float %t0_<index> %t1_<index>
 %vmul_legacy_product_bits_<index> = OpBitcast %uint %vmul_legacy_product_<index>
 %vmul_legacy_product_exp_<index> = OpBitFieldUExtract %uint %vmul_legacy_product_bits_<index> %uint_23 %uint_8
 %vmul_legacy_product_zero_<index> = OpIEqual %bool %vmul_legacy_product_exp_<index> %uint_0
               OpSelectionMerge %vmul_legacy_output_merge_<index> None
               OpBranchConditional %vmul_legacy_product_zero_<index> %vmul_legacy_flush_<index> %vmul_legacy_keep_<index>
 %vmul_legacy_flush_<index> = OpLabel
 %vmul_legacy_src0_sign_<index> = OpBitFieldUExtract %uint %vmul_legacy_src0_bits_<index> %uint_31 %uint_1
 %vmul_legacy_src1_sign_<index> = OpBitFieldUExtract %uint %vmul_legacy_src1_bits_<index> %uint_31 %uint_1
 %vmul_legacy_result_sign_<index> = OpBitwiseXor %uint %vmul_legacy_src0_sign_<index> %vmul_legacy_src1_sign_<index>
 %vmul_legacy_signed_zero_bits_<index> = OpShiftLeftLogical %uint %vmul_legacy_result_sign_<index> %uint_31
 %vmul_legacy_src0_frac_<index> = OpBitFieldUExtract %uint %vmul_legacy_src0_bits_<index> %uint_0 %uint_23
 %vmul_legacy_src1_frac_<index> = OpBitFieldUExtract %uint %vmul_legacy_src1_bits_<index> %uint_0 %uint_23
 %vmul_legacy_src0_sig_<index> = OpBitwiseOr %uint %vmul_legacy_src0_frac_<index> %uint_0x00800000
 %vmul_legacy_src1_sig_<index> = OpBitwiseOr %uint %vmul_legacy_src1_frac_<index> %uint_0x00800000
 %vmul_legacy_exp_sum_<index> = OpIAdd %uint %vmul_legacy_src0_exp_<index> %vmul_legacy_src1_exp_<index>
 %vmul_legacy_boundary_exp_<index> = OpIEqual %bool %vmul_legacy_exp_sum_<index> %uint_127
 %vmul_legacy_sig_product_<index> = OpUMulExtended %ResTypeU %vmul_legacy_src0_sig_<index> %vmul_legacy_src1_sig_<index>
 %vmul_legacy_sig_product_lo_<index> = OpCompositeExtract %uint %vmul_legacy_sig_product_<index> 0
 %vmul_legacy_sig_product_hi_<index> = OpCompositeExtract %uint %vmul_legacy_sig_product_<index> 1
 %vmul_legacy_boundary_hi_<index> = OpIEqual %bool %vmul_legacy_sig_product_hi_<index> %uint_0x00007fff
 %vmul_legacy_boundary_lo_<index> = OpUGreaterThanEqual %bool %vmul_legacy_sig_product_lo_<index> %uint_0xff800000
 %vmul_legacy_boundary_product_<index> = OpLogicalAnd %bool %vmul_legacy_boundary_hi_<index> %vmul_legacy_boundary_lo_<index>
 %vmul_legacy_rounds_normal_<index> = OpLogicalAnd %bool %vmul_legacy_boundary_exp_<index> %vmul_legacy_boundary_product_<index>
 %vmul_legacy_signed_min_normal_bits_<index> = OpBitwiseOr %uint %vmul_legacy_signed_zero_bits_<index> %uint_0x00800000
 %vmul_legacy_underflow_bits_<index> = OpSelect %uint %vmul_legacy_rounds_normal_<index> %vmul_legacy_signed_min_normal_bits_<index> %vmul_legacy_signed_zero_bits_<index>
 %vmul_legacy_underflow_<index> = OpBitcast %float %vmul_legacy_underflow_bits_<index>
               OpBranch %vmul_legacy_output_merge_<index>
 %vmul_legacy_keep_<index> = OpLabel
               OpBranch %vmul_legacy_output_merge_<index>
 %vmul_legacy_output_merge_<index> = OpLabel
 %vmul_legacy_ordinary_<index> = OpPhi %float %vmul_legacy_underflow_<index> %vmul_legacy_flush_<index> %vmul_legacy_product_<index> %vmul_legacy_keep_<index>
               OpBranch %vmul_legacy_value_merge_<index>
 %vmul_legacy_value_merge_<index> = OpLabel
                 %t_<index> = OpPhi %float %float_0_000000 %vmul_legacy_zero_<index> %vmul_legacy_ordinary_<index> %vmul_legacy_output_merge_<index>
               OpStore %<dst> %t_<index>
               OpBranch %vmul_legacy_exec_merge_<index>
 %vmul_legacy_exec_merge_<index> = OpLabel
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_VMacLegacyF32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);
	const auto  type = code.GetType();

	EXIT_NOT_IMPLEMENTED(type != ShaderType::Vertex);
	EXIT_NOT_IMPLEMENTED(code.GetFp32Mode() != ShaderFp32Mode::RneFlushInOut);
	EXIT_NOT_IMPLEMENTED(code.GetDx10ClampMode() != ShaderDx10ClampMode::Disabled);
	EXIT_NOT_IMPLEMENTED(inst.format != ShaderInstructionFormat::SVdstSVsrc0SVsrc1 || inst.src_num != 2);
	EXIT_NOT_IMPLEMENTED(inst.src[0].type != ShaderOperandType::Vgpr || inst.src[1].type != ShaderOperandType::Vgpr ||
	                     inst.dst.type != ShaderOperandType::Vgpr);
	EXIT_NOT_IMPLEMENTED(inst.src[0].absolute || inst.src[1].absolute || inst.src[0].negate || inst.src[1].negate);
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp || inst.dst.multiplier != 1.0f);

	const auto dst_value = operand_variable_to_str(inst.dst);
	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	const String8 index_str = String8::FromPrintf("%u", index);
	String8       load0;
	String8       load1;
	String8       load_acc;
	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0) ||
	    !operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1) ||
	    !operand_load_uint(spirv, inst.dst, "tacc_<index>", index_str, &load_acc))
	{
		return false;
	}

	// Experimental diagnostic profile. The shared integer-only helpers
	// materialize the legacy binary32 product and then perform a distinct
	// binary32 RNE add with input/output denormal flush. Loading all operands
	// before the EXEC-controlled store preserves old-vdst accumulator semantics
	// even when either source aliases the destination.
	static const char* text = R"(
              <load0>
              <load1>
              <load_acc>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %vmac_legacy_exec_merge_<index> None
               OpBranchConditional %exec_lo_b_<index> %vmac_legacy_exec_<index> %vmac_legacy_exec_merge_<index>
 %vmac_legacy_exec_<index> = OpLabel
 %vmac_legacy_product_bits_<index> = OpFunctionCall %uint %lumen_vmac_mul_legacy_bits %t0_<index> %t1_<index>
  %vmac_legacy_result_bits_<index> = OpFunctionCall %uint %lumen_vmac_add_rne_flush_bits %vmac_legacy_product_bits_<index> %tacc_<index>
                 %t_<index> = OpBitcast %float %vmac_legacy_result_bits_<index>
               OpStore %<dst> %t_<index>
               OpBranch %vmac_legacy_exec_merge_<index>
 %vmac_legacy_exec_merge_<index> = OpLabel
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load_acc>", load_acc)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_VRsqClampF32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);
	const auto  type = code.GetType();

	EXIT_NOT_IMPLEMENTED(type != ShaderType::Vertex);
	EXIT_NOT_IMPLEMENTED(inst.format != ShaderInstructionFormat::SVdstSVsrc0 || inst.src_num != 1);
	EXIT_NOT_IMPLEMENTED(inst.src[0].type != ShaderOperandType::Vgpr || inst.dst.type != ShaderOperandType::Vgpr);
	EXIT_NOT_IMPLEMENTED(inst.src[0].absolute || inst.src[0].negate);
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp || inst.dst.multiplier != 1.0f);

	const auto dst_value = operand_variable_to_str(inst.dst);
	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	const String8 index_str = String8::FromPrintf("%u", index);
	String8       load0;
	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	// Experimental diagnostic profile. The shared helper operates only on
	// integer representations, including normalization and RNE rounding, so
	// neither guest nor host floating-point modes affect the result. The source
	// is loaded before the EXEC-controlled write, preserving source/destination
	// aliasing and inactive-lane state.
	static const char* text = R"(
              <load0>
 %vrsq_clamp_result_bits_<index> = OpFunctionCall %uint %lumen_vrsq_clamp_f32_bits %t0_<index>
                 %t_<index> = OpBitcast %float %vrsq_clamp_result_bits_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %vrsq_clamp_exec_merge_<index> None
               OpBranchConditional %exec_lo_b_<index> %vrsq_clamp_exec_<index> %vrsq_clamp_exec_merge_<index>
 %vrsq_clamp_exec_<index> = OpLabel
               OpStore %<dst> %t_<index>
               OpBranch %vrsq_clamp_exec_merge_<index>
 %vrsq_clamp_exec_merge_<index> = OpLabel
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Mac, Max, Min, Mul, Sub, Subrev, Add */
LUMEN_RECOMPILER_FUNC(Recompile_V_XXX_F32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;
	String8 load_dst;
	String8 param0 = param[0];

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}
	if (param0.ContainsStr("tdst_<index>") && !operand_load_float(spirv, inst.dst, "tdst_<index>", index_str, &load_dst))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check SP_DENORM
	// TODO() check SP_ROUND
	// TODO() check DX10_CLAMP
	// TODO() check IEEE

	static const char* text = R"(
              <load0>
              <load1>
              <load_dst>
              <param>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %tl2_<index> None
               OpBranchConditional %exec_lo_b_<index> %tl1_<index> %tl2_<index>
         %tl1_<index> = OpLabel
               OpStore %<dst> %t_<index>
              <multiply>
              <clamp>
               OpBranch %tl2_<index>
         %tl2_<index> = OpLabel
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<multiply>", (inst.dst.multiplier != 1.0f
	                                                  ? String8(MULTIPLY).ReplaceStr("<mul>", spirv->GetConstantFloat(inst.dst.multiplier))
	                                                  : ""))
	                   .ReplaceStr("<clamp>", (inst.dst.clamp ? CLAMP : ""))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load_dst>", load_dst)
	                   .ReplaceStr("<param>", param0)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Rcp, Rsq, Sqrt, Ceil, Floor, Fract, Rndne, Trunc, Exp, Log, Cos, Sin */
LUMEN_RECOMPILER_FUNC(Recompile_V_XXX_F32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	// EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	// EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check DX10_CLAMP
	// TODO() check IEEE

	static const char* text = R"(    
    <load0>
    <param0>
    <param1>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %tl2_<index> None
               OpBranchConditional %exec_lo_b_<index> %tl1_<index> %tl2_<index>
         %tl1_<index> = OpLabel
               OpStore %<dst> %t_<index>
              <multiply>
              <clamp>
               OpBranch %tl2_<index>
         %tl2_<index> = OpLabel
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<multiply>", (inst.dst.multiplier != 1.0f
	                                                  ? String8(MULTIPLY).ReplaceStr("<mul>", spirv->GetConstantFloat(inst.dst.multiplier))
	                                                  : ""))
	                   .ReplaceStr("<clamp>", (inst.dst.clamp ? CLAMP : ""))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: And, Or, Bcnt, Bfm, Lshr, Lshl, Lshlrev, Lshrrev, MulU32U24, MulLoU32, MulHiU32 */
LUMEN_RECOMPILER_FUNC(Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(
              <load0>
              <load1>
              <param0>
              <param1>
              <param2>
              %tf_<index> = OpBitcast %float %t_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %tf_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Ashr, Ashrrev, MulLo */
LUMEN_RECOMPILER_FUNC(Recompile_V_XXX_I32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_int(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(
              <load0>
              <load1>
              <param0>
              <param1>
              %tf_<index> = OpBitcast %float %t_<index>
              OpStore %<dst> %tf_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %tf_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: U32 */
LUMEN_RECOMPILER_FUNC(Recompile_VCvt_XXX_F32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check EXEC
	// TODO() check SP_DENORM_IN

	static const char* text = R"(    
    <load0>
    <param0>
    <param1>
    <param2>
    %t_<index> = OpBitcast %float %t2_<index>
    OpStore %<dst> %t_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", param[1])
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: U32, I32, UbyteX, F16 */
LUMEN_RECOMPILER_FUNC(Recompile_VCvtF32_XXX_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check SP_ROUND

	static const char* text = R"(    
    <load0>
    <param0>
    <param1>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_VCvtOffF32I4_SVdstSVsrc0)
{
	EXIT_NOT_IMPLEMENTED(!ShaderCvtOffF32I4InstructionSupported(code.GetInstructions().At(index)));
	const auto arithmetic = String8(Detail::kVCvtOffF32I4SpirvArithmetic)
	                            .ReplaceStr("<input>", "t0_<index>")
	                            .ReplaceStr("<result>", "t_<index>");
	const char* parameters[] = {arithmetic.c_str(), nullptr};
	return Recompile_VCvtF32_XXX_SVdstSVsrc0(index, code, dst_source, spirv, parameters, scc_check);
}

/* XXX: Sad, Bfe, MadU32U24 */
LUMEN_RECOMPILER_FUNC(Recompile_V_XXX_U32_VdstVsrc0Vsrc1Vsrc2)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;
	String8 load2;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[2], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() Sad: use only lower 16 bits of Vaccum

	static const char* text = R"(
               <load0>
               <load1>
               <load2>
               <param0>
               <param1>
               <param2>
               <param3>
         %tf_<index> = OpBitcast %float %t_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %tf_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", param[1])
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Add, Sub, Subrev */
LUMEN_RECOMPILER_FUNC(Recompile_V_XXX_U32_VdstSdst2Vsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst2));

	auto dst_value   = operand_variable_to_str(inst.dst);
	auto dst2_value0 = operand_variable_to_str(inst.dst2, 0);
	auto dst2_value1 = operand_variable_to_str(inst.dst2, 1);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst2));

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);
	EXIT_NOT_IMPLEMENTED(dst2_value0.type != SpirvType::Uint);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
              <load0>
              <load1>
        <param>
        %t208_<index> = OpCompositeExtract %uint %t_<index> 1
        %t209_<index> = OpCompositeExtract %uint %t_<index> 0
        %t210_<index> = OpBitcast %float %t209_<index>
               OpStore %<dst> %t210_<index>		   
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %t213_<index> = OpSelect %uint %exec_lo_b_<index> %t208_<index> %uint_0
               OpStore %<dst2_0> %t213_<index>
               OpStore %<dst2_1> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<dst2_0>", dst2_value0.value)
	                   .ReplaceStr("<dst2_1>", dst2_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

LUMEN_RECOMPILER_FUNC(Recompile_Inject_Debug)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	bool injected = false;
	int  str_id   = 0;
	for (const auto& c: code.GetDebugPrintfs())
	{
		if (c.pc == inst.pc)
		{
			Core::StringList8 loads;
			Core::StringList8 params;
			int               arg_id = 0;
			EXIT_IF(c.args.Size() != c.types.Size());
			for (const auto& a: c.args)
			{
				auto    type = c.types.At(arg_id);
				String8 load;
				bool    ok        = false;
				String8 result_id = String8::FromPrintf("t_%d_<index>", arg_id);
				switch (type)
				{
					case ShaderDebugPrintf::Type::Uint: ok = operand_load_uint(spirv, a, result_id, index_str, &load); break;
					case ShaderDebugPrintf::Type::Int: ok = operand_load_int(spirv, a, result_id, index_str, &load); break;
					case ShaderDebugPrintf::Type::Float: ok = operand_load_float(spirv, a, result_id, index_str, &load); break;
				}
				EXIT_NOT_IMPLEMENTED(!ok);
				loads.Add(load);
				params.Add("%" + result_id);
				arg_id++;
			}

			static const char* text = R"(
                <loads>
     %tt_<index> = OpExtInst %void %NonSemantic_DebugPrintf 1 %printf_str_<str_id> <params>
		)";
			*dst_source += String8(text)
			                   .ReplaceStr("<loads>", loads.Concat("\n"))
			                   .ReplaceStr("<str_id>", String8::FromPrintf("%d", str_id))
			                   .ReplaceStr("<params>", params.Concat(" "))
			                   .ReplaceStr("<index>", index_str);
			injected = true;
		}
		str_id++;
	}

	return injected;
}

const RecompilerFunc* RecompFunc(ShaderInstructionType type, ShaderInstructionFormat::Format format)
{
	static const RecompilerFunc g_recomp_func[] = {
	    // clang-format off
    {Recompile_BufferLoadDword_Vdata1VaddrSvSoffsIdxen,     ShaderInstructionType::BufferLoadDword,      ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen,        {""}},
    {Recompile_BufferLoadFormatX_Vdata1VaddrSvSoffsIdxen,   ShaderInstructionType::BufferLoadFormatX,    ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen,        {""}},
    {Recompile_BufferLoadFormatXyzw_Vdata4VaddrSvSoffsIdxen, ShaderInstructionType::BufferLoadFormatXyzw, ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen,       {""}},
    {Recompile_BufferStoreDword_Vdata1VaddrSvSoffsIdxen,    ShaderInstructionType::BufferStoreDword,     ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen,        {""}},
    {Recompile_BufferStoreFormatX_Vdata1VaddrSvSoffsIdxen,  ShaderInstructionType::BufferStoreFormatX,   ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen,        {""}},
    {Recompile_BufferStoreFormatXy_Vdata2VaddrSvSoffsIdxen, ShaderInstructionType::BufferStoreFormatXy,  ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen,        {""}},

    {Recompile_DsAppend_VdstGds,                           ShaderInstructionType::DsAppend,            ShaderInstructionFormat::VdstGds,                        {""}},
    {Recompile_DsConsume_VdstGds,                          ShaderInstructionType::DsConsume,           ShaderInstructionFormat::VdstGds,                        {""}},

    {Recompile_Exp_Mrt0OffOffComprVmDone,                  ShaderInstructionType::Exp,                 ShaderInstructionFormat::Mrt0OffOffComprVmDone,          {""}},
	{Recompile_Exp_Mrt0Vsrc0OffComprVmDone,                 ShaderInstructionType::Exp,                 ShaderInstructionFormat::Mrt0Vsrc0OffComprVmDone,        {""}},
    {Recompile_Exp_Mrt0Vsrc0Vsrc1ComprVmDone,              ShaderInstructionType::Exp,                 ShaderInstructionFormat::Mrt0Vsrc0Vsrc1ComprVmDone,      {""}},
    {Recompile_Exp_Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone,         ShaderInstructionType::Exp,                 ShaderInstructionFormat::Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone, {""}},
    {Recompile_Exp_Param_XXX_Vsrc0Vsrc1Vsrc2Vsrc3,         ShaderInstructionType::Exp,                 ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3,     {"param0"}},
    {Recompile_Exp_Param_XXX_Vsrc0Vsrc1Vsrc2Vsrc3,         ShaderInstructionType::Exp,                 ShaderInstructionFormat::Param1Vsrc0Vsrc1Vsrc2Vsrc3,     {"param1"}},
    {Recompile_Exp_Param_XXX_Vsrc0Vsrc1Vsrc2Vsrc3,         ShaderInstructionType::Exp,                 ShaderInstructionFormat::Param2Vsrc0Vsrc1Vsrc2Vsrc3,     {"param2"}},
    {Recompile_Exp_Param_XXX_Vsrc0Vsrc1Vsrc2Vsrc3,         ShaderInstructionType::Exp,                 ShaderInstructionFormat::Param3Vsrc0Vsrc1Vsrc2Vsrc3,     {"param3"}},
	{Recompile_Exp_Param_XXX_Vsrc0Vsrc1Vsrc2Vsrc3,         ShaderInstructionType::Exp,                 ShaderInstructionFormat::Param4Vsrc0Vsrc1Vsrc2Vsrc3,     {"param4"}},
	{Recompile_Exp_Param_XXX_Vsrc0Vsrc1Vsrc2Vsrc3,         ShaderInstructionType::Exp,                 ShaderInstructionFormat::ParamVsrc0Vsrc1Vsrc2Vsrc3,      {""}},
    {Recompile_Exp_Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done,           ShaderInstructionType::Exp,                 ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done,   {""}},
    {Recompile_ImageLoad_Vdata4Vaddr3StDmaskF,             ShaderInstructionType::ImageLoad,           ShaderInstructionFormat::Vdata4Vaddr3StDmaskF,           {""}},
    {Recompile_ImageLoadMip_Vdata4Vaddr4StDmaskF,          ShaderInstructionType::ImageLoadMip,        ShaderInstructionFormat::Vdata4Vaddr4StDmaskF,           {""}},
    {Recompile_ImageSample_Vdata1Vaddr3StSsSingleChannel,  ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata1Vaddr3StSsDmask1,         {"0"}},
    {Recompile_ImageSample_Vdata1Vaddr3StSsSingleChannel,  ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata1Vaddr3StSsDmask2,         {"1"}},
    {Recompile_ImageSample_Vdata1Vaddr3StSsSingleChannel,  ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata1Vaddr3StSsDmask4,         {"2"}},
    {Recompile_ImageSample_Vdata1Vaddr3StSsDmask8,         ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata1Vaddr3StSsDmask8,         {""}},
    {Recompile_ImageSample_Vdata2Vaddr3StSsDmask3,         ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata2Vaddr3StSsDmask3,         {""}},
    {Recompile_ImageSample_Vdata2Vaddr3StSsDmask5,         ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata2Vaddr3StSsDmask5,         {""}},
    {Recompile_ImageSample_Vdata2Vaddr3StSsDmask9,         ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata2Vaddr3StSsDmask9,         {""}},
    {Recompile_ImageSample_Vdata3Vaddr3StSsDmask7,         ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7,         {""}},
    {Recompile_ImageSample_Vdata4Vaddr3StSsDmaskF,         ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF,         {""}},
    {Recompile_ImageSampleLz_Vdata3Vaddr3StSsDmask7,       ShaderInstructionType::ImageSampleLz,       ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7,         {""}},
    {Recompile_ImageSampleLz_Vdata4Vaddr3StSsDmaskF,       ShaderInstructionType::ImageSampleLz,       ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF,         {""}},
    {Recompile_ImageSampleLzO_Vdata3Vaddr4StSsDmask7,      ShaderInstructionType::ImageSampleLzO,      ShaderInstructionFormat::Vdata3Vaddr4StSsDmask7,         {""}},
    {Recompile_ImageStore_Vdata4Vaddr3StDmaskF,            ShaderInstructionType::ImageStore,          ShaderInstructionFormat::Vdata4Vaddr3StDmaskF,           {""}},
    {Recompile_ImageStoreMip_Vdata4Vaddr4StDmaskF,         ShaderInstructionType::ImageStoreMip,       ShaderInstructionFormat::Vdata4Vaddr4StDmaskF,           {""}},

    {Recompile_SBufferLoadDword_SdstSvSoffset,             ShaderInstructionType::SBufferLoadDword,    ShaderInstructionFormat::SdstSvSoffset,                  {""}},
    {Recompile_SBufferLoadDwordx2_Sdst2SvSoffset,          ShaderInstructionType::SBufferLoadDwordx2,  ShaderInstructionFormat::Sdst2SvSoffset,                 {""}},
    {Recompile_SBufferLoadDwordx4_Sdst4SvSoffset,          ShaderInstructionType::SBufferLoadDwordx4,  ShaderInstructionFormat::Sdst4SvSoffset,                 {""}},
    {Recompile_SBufferLoadDwordx8_Sdst8SvSoffset,          ShaderInstructionType::SBufferLoadDwordx8,  ShaderInstructionFormat::Sdst8SvSoffset,                 {""}},
    {Recompile_SBufferLoadDwordx16_Sdst16SvSoffset,        ShaderInstructionType::SBufferLoadDwordx16, ShaderInstructionFormat::Sdst16SvSoffset,                {""}},

    {Recompile_SCbranch_XXX_Label,                         ShaderInstructionType::SCbranchExecz,       ShaderInstructionFormat::Label,                          {"%cc_u_<index> = OpLoad %uint %execz",  "%cc_b_<index> = OpINotEqual %bool %cc_u_<index> %uint_0"}},
    {Recompile_SCbranch_XXX_Label,                         ShaderInstructionType::SCbranchScc0,        ShaderInstructionFormat::Label,                          {"%cc_u_<index> = OpLoad %uint %scc",    "%cc_b_<index> = OpIEqual    %bool %cc_u_<index> %uint_0"}},
    {Recompile_SCbranch_XXX_Label,                         ShaderInstructionType::SCbranchScc1,        ShaderInstructionFormat::Label,                          {"%cc_u_<index> = OpLoad %uint %scc",    "%cc_b_<index> = OpIEqual    %bool %cc_u_<index> %uint_1"}},
    {Recompile_SCbranch_XXX_Label,                         ShaderInstructionType::SCbranchVccz,        ShaderInstructionFormat::Label,                          {"%cc_u_<index> = OpLoad %uint %vcc_lo", "%cc_b_<index> = OpIEqual    %bool %cc_u_<index> %uint_0"}},
    {Recompile_SCbranch_XXX_Label,                         ShaderInstructionType::SCbranchVccnz,       ShaderInstructionFormat::Label,                          {"%cc_u_<index> = OpLoad %uint %vcc_lo", "%cc_b_<index> = OpINotEqual %bool %cc_u_<index> %uint_0"}},
    {Recompile_SBranch_Label,                              ShaderInstructionType::SBranch,             ShaderInstructionFormat::Label,                          {""}},

    {Recompile_SEndpgm_Empty,                              ShaderInstructionType::SEndpgm,             ShaderInstructionFormat::Empty,                          {""}},

    {Recompile_SLoadDwordx4_Sdst4SbaseSoffset,             ShaderInstructionType::SLoadDwordx4,        ShaderInstructionFormat::Sdst4SbaseSoffset,              {""}},
    {Recompile_SLoadDwordx8_Sdst8SbaseSoffset,             ShaderInstructionType::SLoadDwordx8,        ShaderInstructionFormat::Sdst8SbaseSoffset,              {""}},

    {Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12, ShaderInstructionType::SAndn2B64,   ShaderInstructionFormat::Sdst2Ssrc02Ssrc12, {"%ta_<index> = OpNot %uint %t2_<index>",
                                                                                                                             "%tb_<index> = OpBitwiseAnd %uint %t0_<index> %ta_<index>",
                                                                                                                             "%tc_<index> = OpNot %uint %t3_<index>",
                                                                                                                             "%td_<index> = OpBitwiseAnd %uint %t1_<index> %tc_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12, ShaderInstructionType::SOrn2B64,    ShaderInstructionFormat::Sdst2Ssrc02Ssrc12, {"%ta_<index> = OpNot %uint %t2_<index>",
                                                                                                                             "%tb_<index> = OpBitwiseOr %uint %t0_<index> %ta_<index>",
                                                                                                                             "%tc_<index> = OpNot %uint %t3_<index>",
                                                                                                                             "%td_<index> = OpBitwiseOr %uint %t1_<index> %tc_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12, ShaderInstructionType::SAndB64,     ShaderInstructionFormat::Sdst2Ssrc02Ssrc12, {"%tb_<index> = OpBitwiseAnd %uint %t0_<index> %t2_<index>",
                                                                                                                             "%td_<index> = OpBitwiseAnd %uint %t1_<index> %t3_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12, ShaderInstructionType::SNorB64,     ShaderInstructionFormat::Sdst2Ssrc02Ssrc12, {"%ta_<index> = OpBitwiseOr %uint %t0_<index> %t2_<index>",
                                                                                                                             "%tb_<index> = OpNot %uint %ta_<index>",
                                                                                                                             "%tc_<index> = OpBitwiseOr %uint %t1_<index> %t3_<index>",
                                                                                                                             "%td_<index> = OpNot %uint %tc_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12, ShaderInstructionType::SNandB64,    ShaderInstructionFormat::Sdst2Ssrc02Ssrc12, {"%ta_<index> = OpBitwiseAnd %uint %t0_<index> %t2_<index>",
                                                                                                                             "%tb_<index> = OpNot %uint %ta_<index>",
                                                                                                                             "%tc_<index> = OpBitwiseAnd %uint %t1_<index> %t3_<index>",
                                                                                                                             "%td_<index> = OpNot %uint %tc_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12, ShaderInstructionType::SXnorB64,    ShaderInstructionFormat::Sdst2Ssrc02Ssrc12, {"%ta_<index> = OpBitwiseXor %uint %t0_<index> %t2_<index>",
                                                                                                                             "%tb_<index> = OpNot %uint %ta_<index>",
                                                                                                                             "%tc_<index> = OpBitwiseXor %uint %t1_<index> %t3_<index>",
                                                                                                                             "%td_<index> = OpNot %uint %tc_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12, ShaderInstructionType::SOrB64,      ShaderInstructionFormat::Sdst2Ssrc02Ssrc12, {"%tb_<index> = OpBitwiseOr %uint %t0_<index> %t2_<index>",
                                                                                                                             "%td_<index> = OpBitwiseOr %uint %t1_<index> %t3_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12, ShaderInstructionType::SXorB64,     ShaderInstructionFormat::Sdst2Ssrc02Ssrc12, {"%tb_<index> = OpBitwiseXor %uint %t0_<index> %t2_<index>",
                                                                                                                             "%td_<index> = OpBitwiseXor %uint %t1_<index> %t3_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12, ShaderInstructionType::SCselectB64, ShaderInstructionFormat::Sdst2Ssrc02Ssrc12, {"%ts_<index> = OpLoad %uint %scc",
                                                                                                                             "%tsb_<index> = OpINotEqual %bool %ts_<index> %uint_0",
                                                                                                                             "%tb_<index> = OpSelect %uint %tsb_<index> %t0_<index> %t2_<index>",
                                                                                                                             "%td_<index> = OpSelect %uint %tsb_<index> %t1_<index> %t3_<index>" }, SccCheck::None},

    {Recompile_S_Bfe_U64_Sdst2Ssrc02Ssrc1,  ShaderInstructionType::SBfeU64,     ShaderInstructionFormat::Sdst2Ssrc02Ssrc1,  {"", ""} , SccCheck::NonZero},
    {Recompile_S_Lshl_B64_Sdst2Ssrc02Ssrc1, ShaderInstructionType::SLshlB64,    ShaderInstructionFormat::Sdst2Ssrc02Ssrc1,  {"", ""} , SccCheck::NonZero},
    {Recompile_S_Lshr_B64_Sdst2Ssrc02Ssrc1, ShaderInstructionType::SLshrB64,    ShaderInstructionFormat::Sdst2Ssrc02Ssrc1,  {"", ""} , SccCheck::NonZero},

    {Recompile_S_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SAndB32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpBitwiseAnd %uint %t0_<index> %t1_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SBfmB32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%tcount_<index> = OpBitwiseAnd %uint %t0_<index> %uint_31", "%toffset_<index> = OpBitwiseAnd %uint %t1_<index> %uint_31", "%t_<index> = OpBitFieldInsert %uint %uint_0 %uint_0xffffffff %toffset_<index> %tcount_<index>"}, SccCheck::None},
    {Recompile_S_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SCselectB32,     ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t22_<index> = OpLoad %uint %scc", "%t2_<index> = OpINotEqual %bool %t22_<index> %uint_0", "%t_<index> = OpSelect %uint %t2_<index> %t0_<index> %t1_<index>"}, SccCheck::None},
    {Recompile_S_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SLshlB32,        ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%ts_<index> = OpBitwiseAnd %uint %t1_<index> %uint_31", "%t_<index> = OpShiftLeftLogical %uint %t0_<index> %ts_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SLshrB32,        ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%ts_<index> = OpBitwiseAnd %uint %t1_<index> %uint_31", "%t_<index> = OpShiftRightLogical %uint %t0_<index> %ts_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SOrB32,          ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpBitwiseOr %uint %t0_<index> %t1_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_I32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SAddI32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpIAdd %int %t0_<index> %t1_<index>"}, SccCheck::OverflowAdd},
    {Recompile_S_XXX_I32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SMulI32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpIMul %int %t0_<index> %t1_<index>"}, SccCheck::None},
    {Recompile_S_XXX_I32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SSubI32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpISub %int %t0_<index> %t1_<index>"}, SccCheck::OverflowSub},
    {Recompile_S_XXX_U32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SAddcU32,        ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%tscc_<index> = OpLoad %uint %scc", "%ts_<index> = OpFunctionCall %v2uint %addc %t0_<index> %t1_<index> %tscc_<index>", "%t_<index> = OpCompositeExtract %uint %ts_<index> 0", "%carry_<index> = OpCompositeExtract %uint %ts_<index> 1"}, SccCheck::CarryOut},
    {Recompile_S_XXX_U32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SAddU32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%ts_<index> = OpIAddCarry %ResTypeU %t0_<index> %t1_<index>", "%t_<index> = OpCompositeExtract %uint %ts_<index> 0", "%carry_<index> = OpCompositeExtract %uint %ts_<index> 1"}, SccCheck::CarryOut},
    {Recompile_S_XXX_U32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SBfeU32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%to_<index> = OpBitFieldUExtract %uint %t1_<index> %uint_0  %uint_5", "%ts_<index> = OpBitFieldUExtract %uint %t1_<index> %uint_16 %uint_7", "%t_<index> = OpBitFieldUExtract %uint %t0_<index> %to_<index> %ts_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_U32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SMulHiU32,       ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpFunctionCall %uint %mul_hi_uint %t0_<index> %t1_<index>"}, SccCheck::None},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VAndB32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpBitwiseAnd %uint %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VBcntU32B32,     ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%tb_<index> = OpBitCount %int %t0_<index>", "%tbu_<index> = OpBitcast %uint %tb_<index>", "%t_<index> = OpIAdd %uint %tbu_<index> %t1_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VBfmB32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%tcount_<index> = OpBitwiseAnd %uint %t0_<index> %uint_31", "%toffset_<index> = OpBitwiseAnd %uint %t1_<index> %uint_31", "%t_<index> = OpBitFieldInsert %uint %uint_0 %uint_0xffffffff %toffset_<index> %tcount_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VLshlB32,        ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%ts_<index> = OpBitwiseAnd %uint %t1_<index> %uint_31", "%t_<index> = OpShiftLeftLogical %uint %t0_<index> %ts_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VLshlrevB32,     ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%ts_<index> = OpBitwiseAnd %uint %t0_<index> %uint_31", "%t_<index> = OpShiftLeftLogical %uint %t1_<index> %ts_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VLshrB32,        ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%ts_<index> = OpBitwiseAnd %uint %t1_<index> %uint_31", "%t_<index> = OpShiftRightLogical %uint %t0_<index> %ts_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VLshrrevB32,     ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%ts_<index> = OpBitwiseAnd %uint %t0_<index> %uint_31", "%t_<index> = OpShiftRightLogical %uint %t1_<index> %ts_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VMulHiU32,       ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpFunctionCall %uint %mul_hi_uint %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VMulLoU32,       ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpFunctionCall %uint %mul_lo_uint %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VMulU32U24,      ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%tu0_<index> = OpBitwiseAnd %uint %t0_<index> %uint_0x00ffffff", "%tu1_<index> = OpBitwiseAnd %uint %t1_<index> %uint_0x00ffffff", "%t_<index> = OpFunctionCall %uint %mul_lo_uint %tu0_<index> %tu1_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VMinU32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpExtInst %uint %GLSL_std_450 UMin %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VOrB32,          ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpBitwiseOr %uint %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VXorB32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpBitwiseXor %uint %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VAddF32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpFAdd %float %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VMacF32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpExtInst %float %GLSL_std_450 Fma %t0_<index> %t1_<index> %tdst_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VMaxF32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpExtInst %float %GLSL_std_450 FMax %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VMinF32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpExtInst %float %GLSL_std_450 FMin %t0_<index> %t1_<index>"}},
    {Recompile_VMulLegacyF32_SVdstSVsrc0SVsrc1,   ShaderInstructionType::VMulLegacyF32,   ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {""}},
    {Recompile_VMacLegacyF32_SVdstSVsrc0SVsrc1,   ShaderInstructionType::VMacLegacyF32,   ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {""}},
    {Recompile_V_XXX_F32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VMulF32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpFMul %float %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VSubF32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpFSub %float %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VSubrevF32,      ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpFSub %float %t1_<index> %t0_<index>"}},
    {Recompile_V_XXX_I32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VAshrI32,        ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%ts_<index> = OpBitwiseAnd %int %t1_<index> %int_31", "%t_<index> = OpShiftRightArithmetic %int %t0_<index> %ts_<index>"}},
    {Recompile_V_XXX_I32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VAshrrevI32,     ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%ts_<index> = OpBitwiseAnd %int %t0_<index> %int_31", "%t_<index> = OpShiftRightArithmetic %int %t1_<index> %ts_<index>"}},
    {Recompile_V_XXX_I32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VMulLoI32,       ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpFunctionCall %int %mul_lo_int %t0_<index> %t1_<index>"}},
    {Recompile_VCvtPkrtzF16F32_SVdstSVsrc0SVsrc1, ShaderInstructionType::VCvtPkrtzF16F32, ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {""}},
    {Recompile_VMbcntHiU32B32_SVdstSVsrc0SVsrc1,  ShaderInstructionType::VMbcntHiU32B32,  ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {""}},
    {Recompile_VMbcntLoU32B32_SVdstSVsrc0SVsrc1,  ShaderInstructionType::VMbcntLoU32B32,  ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {""}},


    {Recompile_SMovB32_SVdstSVsrc0,            ShaderInstructionType::SMovB32,             ShaderInstructionFormat::SVdstSVsrc0, {""}},
    {Recompile_SMovB32_SVdstSVsrc0,            ShaderInstructionType::SMovkI32,            ShaderInstructionFormat::SVdstSVsrc0, {""}},
    {Recompile_SMulkI32_SVdstSVsrc0,           ShaderInstructionType::SMulkI32,            ShaderInstructionFormat::SVdstSVsrc0, {""}},
    {Recompile_V_XXX_B32_SVdstSVsrc0,          ShaderInstructionType::VBfrevB32,           ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpBitReverse %uint %t0_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0,          ShaderInstructionType::VNotB32,             ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpNot %uint %t0_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VCeilF32,            ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpExtInst %float %GLSL_std_450 Ceil %t0_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VCosF32,             ShaderInstructionFormat::SVdstSVsrc0, {"%tr_<index> = OpFMul %float %t0_<index> %float_2pi", "%t_<index> = OpExtInst %float %GLSL_std_450 Cos %tr_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VExpF32,             ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpExtInst %float %GLSL_std_450 Exp2 %t0_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VFloorF32,           ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpExtInst %float %GLSL_std_450 Floor %t0_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VFractF32,           ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpExtInst %float %GLSL_std_450 Fract %t0_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VLogF32,             ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpExtInst %float %GLSL_std_450 Log2 %t0_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VRcpF32,             ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpFDiv %float %float_1_000000 %t0_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VRndneF32,           ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpExtInst %float %GLSL_std_450 RoundEven %t0_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VRsqF32,             ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpExtInst %float %GLSL_std_450 InverseSqrt %t0_<index>"}},
    {Recompile_VRsqClampF32_SVdstSVsrc0,       ShaderInstructionType::VRsqClampF32,        ShaderInstructionFormat::SVdstSVsrc0, {""}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VSinF32,             ShaderInstructionFormat::SVdstSVsrc0, {"%tr_<index> = OpFMul %float %t0_<index> %float_2pi", "%t_<index> = OpExtInst %float %GLSL_std_450 Sin %tr_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VSqrtF32,            ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpExtInst %float %GLSL_std_450 Sqrt %t0_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VTruncF32,           ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpExtInst %float %GLSL_std_450 Trunc %t0_<index>"}},
    {Recompile_VCvt_XXX_F32_SVdstSVsrc0,       ShaderInstructionType::VCvtU32F32,          ShaderInstructionFormat::SVdstSVsrc0, {"%t1_<index> = OpExtInst %float %GLSL_std_450 Trunc %t0_<index>", "%t2_<index> = OpConvertFToU %uint %t1_<index>"}},
    {Recompile_VCvtF32_XXX_SVdstSVsrc0,        ShaderInstructionType::VCvtF32F16,          ShaderInstructionFormat::SVdstSVsrc0, {"%ts_<index> = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %t0_<index>", "%t_<index> = OpCompositeExtract %float %ts_<index> 0"}},
    {Recompile_VCvtF32_XXX_SVdstSVsrc0,        ShaderInstructionType::VCvtF32I32,          ShaderInstructionFormat::SVdstSVsrc0, {"%ti_<index> = OpBitcast %int %t0_<index>", "%t_<index> = OpConvertSToF %float %ti_<index>"}},
    {Recompile_VCvtOffF32I4_SVdstSVsrc0,       ShaderInstructionType::VCvtOffF32I4,        ShaderInstructionFormat::SVdstSVsrc0, {""}},
    {Recompile_VCvtF32_XXX_SVdstSVsrc0,        ShaderInstructionType::VCvtF32U32,          ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpConvertUToF %float %t0_<index>"}},
    {Recompile_VCvtF32_XXX_SVdstSVsrc0,        ShaderInstructionType::VCvtF32Ubyte0,       ShaderInstructionFormat::SVdstSVsrc0, {"%tb_<index> = OpBitFieldUExtract %uint %t0_<index> %uint_0 %uint_8", "%t_<index> = OpConvertUToF %float %tb_<index>"}},
    {Recompile_VCvtF32_XXX_SVdstSVsrc0,        ShaderInstructionType::VCvtF32Ubyte1,       ShaderInstructionFormat::SVdstSVsrc0, {"%tb_<index> = OpBitFieldUExtract %uint %t0_<index> %uint_8 %uint_8", "%t_<index> = OpConvertUToF %float %tb_<index>"}},
    {Recompile_VCvtF32_XXX_SVdstSVsrc0,        ShaderInstructionType::VCvtF32Ubyte2,       ShaderInstructionFormat::SVdstSVsrc0, {"%tb_<index> = OpBitFieldUExtract %uint %t0_<index> %uint_16 %uint_8", "%t_<index> = OpConvertUToF %float %tb_<index>"}},
    {Recompile_VCvtF32_XXX_SVdstSVsrc0,        ShaderInstructionType::VCvtF32Ubyte3,       ShaderInstructionFormat::SVdstSVsrc0, {"%tb_<index> = OpBitFieldUExtract %uint %t0_<index> %uint_24 %uint_8", "%t_<index> = OpConvertUToF %float %tb_<index>"}},
    {Recompile_VMovB32_SVdstSVsrc0,            ShaderInstructionType::VMovB32,             ShaderInstructionFormat::SVdstSVsrc0, {""}},

    {Recompile_SAndSaveexecB64_Sdst2Ssrc02,    ShaderInstructionType::SAndSaveexecB64,     ShaderInstructionFormat::Sdst2Ssrc02, {""}, SccCheck::NonZero},
    {Recompile_SMovB64_Sdst2Ssrc02,            ShaderInstructionType::SMovB64,             ShaderInstructionFormat::Sdst2Ssrc02, {""}},
    {Recompile_SSwappcB64_Sdst2Ssrc02,         ShaderInstructionType::SSwappcB64,          ShaderInstructionFormat::Sdst2Ssrc02, {""}},
    {Recompile_SWqmB64_Sdst2Ssrc02,            ShaderInstructionType::SWqmB64,             ShaderInstructionFormat::Sdst2Ssrc02, {""}, SccCheck::NonZero},

	{Recompile_Skip,                           ShaderInstructionType::SInstPrefetch,       ShaderInstructionFormat::Imm,         {""}},
	{Recompile_Skip,                           ShaderInstructionType::SSendmsg,            ShaderInstructionFormat::Imm,         {""}},
	{Recompile_STTraceData_Imm,                ShaderInstructionType::STTraceData,          ShaderInstructionFormat::Imm,         {""}},
    {Recompile_Skip,                           ShaderInstructionType::SWaitcnt,            ShaderInstructionFormat::Imm,         {""}},

    {Recompile_TBufferLoadFormatX_Vdata1VaddrSvSoffsIdxenFloat1,          ShaderInstructionType::TBufferLoadFormatX,    ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxenFloat1,  {""}},
    {Recompile_TBufferLoadFormatXyzw_Vdata4Vaddr2SvSoffsOffenIdxenFloat4, ShaderInstructionType::TBufferLoadFormatXyzw, ShaderInstructionFormat::Vdata4Vaddr2SvSoffsOffenIdxenFloat4,  {""}},
    {Recompile_TBufferLoadFormatXyzw_Vdata4VaddrSvSoffsIdxenFloat4,       ShaderInstructionType::TBufferLoadFormatXyzw, ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxenFloat4,  {""}},

    {Recompile_V_XXX_U32_VdstSdst2Vsrc0Vsrc1,  ShaderInstructionType::VAddI32,    ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1,  {"%t_<index> = OpIAddCarry %ResTypeU %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_U32_VdstSdst2Vsrc0Vsrc1,  ShaderInstructionType::VSubI32,    ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1,  {"%t_<index> = OpISubBorrow %ResTypeU %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_U32_VdstSdst2Vsrc0Vsrc1,  ShaderInstructionType::VSubrevI32, ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1,  {"%t_<index> = OpISubBorrow %ResTypeU %t1_<index> %t0_<index>"}},

    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpEqF32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFOrdEqual"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpFF32,     ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpIEqual %bool %uint_0 %uint_1 ; "}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpGeF32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFOrdGreaterThanEqual"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpGtF32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFOrdGreaterThan"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpLeF32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFOrdLessThanEqual"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpLgF32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFOrdNotEqual"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpLtF32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFOrdLessThan"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpNeqF32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFUnordNotEqual"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpNgeF32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFUnordLessThan"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpNgtF32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFUnordLessThanEqual"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpNleF32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFUnordGreaterThan"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpNlgF32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFUnordEqual"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpNltF32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFUnordGreaterThanEqual"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpOF32,     ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFunctionCall %bool %ordered %t0_<index> %t1_<index> ; "}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpTruF32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpIEqual %bool %uint_0 %uint_0 ; "}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpUF32,     ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFunctionCall %bool %unordered %t0_<index> %t1_<index> ; "}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpEqI32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpIEqual"}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpEqU32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpIEqual"}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpFI32,     ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpIEqual %bool %uint_0 %uint_1 ; "}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpGeI32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpSGreaterThanEqual"}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpGtI32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpSGreaterThan"}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpLeI32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpSLessThanEqual"}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpLtI32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpSLessThan"}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpNeI32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpINotEqual"}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpNeU32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpINotEqual"}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpTI32,     ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpIEqual %bool %uint_0 %uint_0 ; "}},
    {Recompile_VCmp_XXX_U32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpFU32,     ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpIEqual %bool %uint_0 %uint_1 ; "}},
    {Recompile_VCmp_XXX_U32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpGeU32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpUGreaterThanEqual"}},
    {Recompile_VCmp_XXX_U32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpGtU32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpUGreaterThan"}},
    {Recompile_VCmp_XXX_U32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpLeU32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpULessThanEqual"}},
    {Recompile_VCmp_XXX_U32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpLtU32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpULessThan"}},
    {Recompile_VCmp_XXX_U32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpTU32,     ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpIEqual %bool %uint_0 %uint_0 ; "}},
    {Recompile_VCmpx_XXX_F32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxNeqF32,  ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFUnordNotEqual"}},
    {Recompile_VCmpx_XXX_F32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxGtF32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFOrdGreaterThan"}},
    {Recompile_VCmpx_XXX_F32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxLtF32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFOrdLessThan"}},
    {Recompile_VCmpx_XXX_I32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxEqU32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpIEqual"}},
    {Recompile_VCmpx_XXX_I32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxNeU32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpINotEqual"}},
    {Recompile_VCmpx_XXX_U32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxGeU32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpUGreaterThanEqual"}},
    {Recompile_VCmpx_XXX_U32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxGtU32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpUGreaterThan"}},

    {Recompile_SCmp_XXX_I32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpEqI32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpIEqual"}},
    {Recompile_SCmp_XXX_I32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpGeI32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpSGreaterThanEqual"}},
    {Recompile_SCmp_XXX_I32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpGtI32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpSGreaterThan"}},
    {Recompile_SCmp_XXX_I32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpLgI32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpINotEqual"}},
    {Recompile_SCmp_XXX_I32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpLtI32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpSLessThan"}},
    {Recompile_SCmp_XXX_I32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpLeI32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpSLessThanEqual"}},
    {Recompile_SCmp_XXX_U32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpEqU32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpIEqual"}},
    {Recompile_SCmp_XXX_U32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpGeU32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpUGreaterThanEqual"}},
    {Recompile_SCmp_XXX_U32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpGtU32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpUGreaterThan"}},
    {Recompile_SCmp_XXX_U32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpLeU32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpULessThanEqual"}},
    {Recompile_SCmp_XXX_U32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpLtU32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpULessThan"}},
    {Recompile_SCmp_XXX_U32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpLgU32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpINotEqual"}},

    {Recompile_VCndmaskB32_VdstVsrc0Vsrc1Smask2,   ShaderInstructionType::VCndmaskB32,  ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2, {""}},

    {Recompile_VInterpMovF32_VdstVsrcAttrChan,     ShaderInstructionType::VInterpMovF32, ShaderInstructionFormat::VdstVsrcAttrChan,    {""}},
    {Recompile_VInterpP1F32_VdstVsrcAttrChan,      ShaderInstructionType::VInterpP1F32, ShaderInstructionFormat::VdstVsrcAttrChan,     {""}},
    {Recompile_VInterpP2F32_VdstVsrcAttrChan,      ShaderInstructionType::VInterpP2F32, ShaderInstructionFormat::VdstVsrcAttrChan,     {""}},

    {Recompile_V_XXX_F32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VMadF32,    ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%t_<index> = OpExtInst %float %GLSL_std_450 Fma %t0_<index> %t1_<index> %t2_<index>"}},
    {Recompile_V_XXX_F32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VFmaF32,    ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%t_<index> = OpExtInst %float %GLSL_std_450 Fma %t0_<index> %t1_<index> %t2_<index>"}},
    {Recompile_V_XXX_F32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VMadakF32,  ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%t_<index> = OpExtInst %float %GLSL_std_450 Fma %t0_<index> %t1_<index> %t2_<index>"}},
    {Recompile_V_XXX_F32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VMadmkF32,  ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%t_<index> = OpExtInst %float %GLSL_std_450 Fma %t0_<index> %t1_<index> %t2_<index>"}},
    {Recompile_V_XXX_F32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VMax3F32,   ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%tm_<index> = OpExtInst %float %GLSL_std_450 FMax %t0_<index> %t1_<index>",
                                                                                                                                  "%t_<index> = OpExtInst %float %GLSL_std_450 FMax %tm_<index> %t2_<index>"}},
    {Recompile_V_XXX_F32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VMin3F32,   ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%tm_<index> = OpExtInst %float %GLSL_std_450 FMin %t0_<index> %t1_<index>",
                                                                                                                                  "%t_<index> = OpExtInst %float %GLSL_std_450 FMin %tm_<index> %t2_<index>"}},
    {Recompile_V_XXX_F32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VMed3F32,   ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%t3_<index> = OpExtInst %float %GLSL_std_450 FMin %t0_<index> %t1_<index>",
                                                                                                                                  "%t4_<index> = OpExtInst %float %GLSL_std_450 FMax %t0_<index> %t1_<index>",
                                                                                                                                  "%t5_<index> = OpExtInst %float %GLSL_std_450 FMin %t4_<index> %t2_<index>",
                                                                                                                                  "%t_<index> = OpExtInst %float %GLSL_std_450 FMax %t3_<index> %t5_<index>"}},
    {Recompile_V_XXX_U32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VSadU32,    ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%td_<index> = OpFunctionCall %uint %abs_diff %t0_<index> %t1_<index>",
                                                                                                                                  "%t_<index> = OpIAdd %uint %td_<index> %t2_<index>"}},
    {Recompile_V_XXX_U32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VBfeU32,    ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%to_<index> = OpBitwiseAnd %uint %t1_<index> %uint_31",
                                                                                                                                  "%ts_<index> = OpBitwiseAnd %uint %t2_<index> %uint_31",
                                                                                                                                  "%t_<index> = OpBitFieldUExtract %uint %t0_<index> %to_<index> %ts_<index>"}},
    {Recompile_V_XXX_U32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VMadU32U24, ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%tu0_<index> = OpBitwiseAnd %uint %t0_<index> %uint_0x00ffffff",
                                                                                                                                  "%tu1_<index> = OpBitwiseAnd %uint %t1_<index> %uint_0x00ffffff",
                                                                                                                                  "%tm_<index> = OpFunctionCall %uint %mul_lo_uint %tu0_<index> %tu1_<index>",
                                                                                                                                  "%t_<index> = OpIAdd %uint %tm_<index> %t2_<index>"}},

	    // clang-format on
	};

	static Core::Hashmap<ShaderInstructionTypeFormat, const RecompilerFunc*>* map = nullptr;

	if (map == nullptr)
	{
		map = new Core::Hashmap<ShaderInstructionTypeFormat, const RecompilerFunc*>();

		for (const auto& func: g_recomp_func)
		{
			ShaderInstructionTypeFormat p = {func.type, func.format};
			EXIT_IF(map->Contains(p));
			map->Put(p, &func);
		}
	}

	ShaderInstructionTypeFormat p = {type, format};

	return map->Get(p, nullptr);
}

void Spirv::AddConstantUint(uint32_t u)
{
	ShaderConstant c {};
	c.u = u;
	AddConstant(SpirvType::Uint, c);
}

void Spirv::AddConstantInt(int i)
{
	ShaderConstant c {};
	c.i = i;
	AddConstant(SpirvType::Int, c);
}

void Spirv::AddConstantFloat(float f)
{
	ShaderConstant c {};
	c.f = f;
	AddConstant(SpirvType::Float, c);
}

void Spirv::AddConstant(ShaderOperand op)
{
	SpirvType type = SpirvType::Unknown;

	if (op.type == ShaderOperandType::LiteralConstant)
	{
		type = SpirvType::Uint;
	}
	if (op.type == ShaderOperandType::IntegerInlineConstant)
	{
		type = SpirvType::Int;
	}
	if (op.type == ShaderOperandType::FloatInlineConstant)
	{
		type = SpirvType::Float;
	}

	EXIT_NOT_IMPLEMENTED(type == SpirvType::Unknown);

	AddConstant(type, op.constant);
}

void Spirv::AddConstant(SpirvType type, ShaderConstant constant)
{
	for (const auto& c: m_constants)
	{
		if (c.type == type && c.constant.u == constant.u)
		{
			return;
		}
	}

	Constant c {};
	c.type     = type;
	c.constant = constant;
	c.type_str = Core::EnumName(type).ToLower().C_Str();

	if (type == SpirvType::Uint)
	{
		c.value_str = constant.u < 256 ? String8::FromPrintf("%u", constant.u) : String8::FromPrintf("0x%08" PRIx32, constant.u);
	}
	if (type == SpirvType::Int)
	{
		c.value_str = String8::FromPrintf("%d", constant.i);
	}
	if (type == SpirvType::Float)
	{
		c.value_str = String8::FromPrintf("%f", constant.f);
	}

	c.id = String8::FromPrintf("%s_%s", c.type_str.c_str(), c.value_str.ReplaceChar('.', '_').ReplaceChar('-', 'm').c_str());

	m_constants.Add(c);
}

void Spirv::AddVariable(ShaderOperandType type, int register_id, int size)
{
	ShaderOperand op;
	op.type        = type;
	op.register_id = register_id;
	op.size        = size;
	AddVariable(op);
}

void Spirv::AddVariable(ShaderOperand op)
{
	if (operand_is_variable(op))
	{
		EXIT_IF(op.size == 0);

		for (int i = 0; i < op.size; i++)
		{
			Variable v;
			v.op.type        = op.type;
			v.op.register_id = op.register_id + i;
			v.op.size        = 1;

			if (op.type == ShaderOperandType::VccLo && op.size == 2 && i == 1)
			{
				v.op.type        = ShaderOperandType::VccHi;
				v.op.register_id = 0;
			}

			if (op.type == ShaderOperandType::ExecLo && op.size == 2 && i == 1)
			{
				v.op.type        = ShaderOperandType::ExecHi;
				v.op.register_id = 0;
			}

			if (!m_variables.Contains(v, [](auto v1, auto v2) { return v1.op == v2.op; }))
			{
				m_variables.Add(v);
			}
		}
	}
}

String8 Spirv::GetConstantUint(uint32_t u) const
{
	for (const auto& c: m_constants)
	{
		if (c.type == SpirvType::Uint && c.constant.u == u)
		{
			return c.id;
		}
	}

	return "unknown_uint_constant";
}

String8 Spirv::GetConstantInt(int i) const
{
	for (const auto& c: m_constants)
	{
		if (c.type == SpirvType::Int && c.constant.i == i)
		{
			return c.id;
		}
	}

	return "unknown_int_constant";
}

String8 Spirv::GetConstantFloat(float f) const
{
	for (const auto& c: m_constants)
	{
		if (c.type == SpirvType::Float && c.constant.f == f)
		{
			return c.id;
		}
	}

	return "unknown_float_constant";
}

String8 Spirv::GetConstant(ShaderOperand op) const
{
	SpirvType type = SpirvType::Unknown;

	if (op.type == ShaderOperandType::LiteralConstant)
	{
		type = SpirvType::Uint;
	}
	if (op.type == ShaderOperandType::IntegerInlineConstant)
	{
		type = SpirvType::Int;
	}
	if (op.type == ShaderOperandType::FloatInlineConstant)
	{
		type = SpirvType::Float;
	}

	for (const auto& c: m_constants)
	{
		if (c.type == type && c.constant.u == op.constant.u)
		{
			return c.id;
		}
	}

	return "unknown_operand_constant";
}

void Spirv::GenerateSource()
{
	m_source.Clear();

	switch (m_code.GetType())
	{
		case ShaderType::Pixel: m_bind = (m_ps_input_info != nullptr ? &m_ps_input_info->bind : nullptr); break;
		case ShaderType::Vertex: m_bind = (m_vs_input_info != nullptr ? &m_vs_input_info->bind : nullptr); break;
		case ShaderType::Compute: m_bind = (m_cs_input_info != nullptr ? &m_cs_input_info->bind : nullptr); break;
		default: m_bind = nullptr; break;
	}
	ValidateTextureOperations(m_code, m_bind);

	WriteHeader();
	WriteDebug();
	WriteAnnotations();
	WriteTypes();
	WriteConstants();
	WriteGlobalVariables();
	WriteMainProlog();
	WriteLocalVariables();
	WriteInstructions();
	WriteMainEpilog();
	WriteFunctions();
	if (m_bind != nullptr && m_bind->constant_storage == ShaderConstantStorage::UniformBuffer)
	{
		// All vsharp loads share these pointer IDs, including extended SGPRs.
		m_source = m_source.ReplaceStr("%_ptr_PushConstant_uint", "%_ptr_UniformConstantData_uint");
	}
}

void Spirv::WriteHeader()
{
	static const char* header = R"(
                ; Header
                OpCapability Shader  
                OpCapability ImageQuery              
                OpCapability SampledImageArrayDynamicIndexing
                OpCapability StorageBufferArrayDynamicIndexing
                OpCapability StorageImageArrayDynamicIndexing
                <Capabilities>
                <Extensions>
                <Imports>
                OpMemoryModel Logical GLSL450 
                OpEntryPoint <Type> %main "main" <Variables>
                <ExecutionModes>
)";

	String8 header_str;

	Core::StringList8 vars;
	Core::StringList8 capabilities;
	Core::StringList8 extensions;
	Core::StringList8 imports;
	Core::StringList8 execution_modes;

	imports.Add("%GLSL_std_450 = OpExtInstImport \"GLSL.std.450\"");

	if (Config::SpirvDebugPrintfEnabled())
	{
		extensions.Add("OpExtension \"SPV_KHR_non_semantic_info\"");
		imports.Add("%NonSemantic_DebugPrintf = OpExtInstImport \"NonSemantic.DebugPrintf\"");
	}
	if (m_bind != nullptr && m_bind->textures2D.textures2d_storage_num > 0 &&
	    ShaderGetUniformStorageImageFormat(m_bind->textures2D) == ShaderStorageImageFormat::R8Unorm)
	{
		capabilities.Add("OpCapability StorageImageExtendedFormats");
	}
	// SPIR-V 1.3 permits only Input and Output variables in OpEntryPoint's
	// interface list. Descriptor and push-constant globals are referenced by
	// the function body but intentionally omitted here.

	switch (m_code.GetType())
	{
		case ShaderType::Pixel:
			vars.Add("%outColor");
			if (m_ps_input_info != nullptr)
			{
				for (uint32_t i = 0; i < m_ps_input_info->input_num; i++)
				{
					vars.Add(String8::FromPrintf("%%attr%d", i));
				}
				if (m_ps_input_info->ps_pos_xy)
				{
					vars.Add("%gl_FragCoord");
				}
				// Guest early-Z eligibility must not force host depth/stencil writes
				// before a possible discard. EarlyFragmentTests makes those writes
				// survive OpKill; ordinary fragment tests preserve the final mask.
				if (m_ps_input_info->ps_early_z && !PixelShaderMayDiscard(m_code, *m_ps_input_info))
				{
					execution_modes.Add("OpExecutionMode %main EarlyFragmentTests\n");
				}
			}
			header_str = String8(header).ReplaceStr("<Type>", "Fragment");
			execution_modes.Add("OpExecutionMode %main OriginUpperLeft\n");
			// TODO() do we need PixelCenterInteger mode?
			break;
		case ShaderType::Vertex:
			if (m_vs_input_info != nullptr)
			{
				for (int i = 0; i < m_vs_input_info->resources_num; i++)
				{
					vars.Add(String8::FromPrintf("%%attr%d", i));
				}
				for (int i = 0; i < m_vs_input_info->export_count; i++)
				{
					vars.Add(String8::FromPrintf("%%param%d", i));
				}
			}
			vars.Add("%gl_VertexIndex");
			vars.Add("%gl_InstanceIndex");
			vars.Add("%outPerVertex");
			// vars.Add("%param0");
			header_str = String8(header).ReplaceStr("<Type>", "Vertex");
			break;
		case ShaderType::Compute:
			if (m_cs_input_info != nullptr)
			{
				execution_modes.Add(String8::FromPrintf("OpExecutionMode %%main LocalSize %u %u %u", m_cs_input_info->threads_num[0],
				                                        m_cs_input_info->threads_num[1], m_cs_input_info->threads_num[2]));
			}
			vars.Add("%gl_LocalInvocationID");
			vars.Add("%gl_WorkGroupID");
			header_str = String8(header).ReplaceStr("<Type>", "GLCompute");
			break;
		default: EXIT("unknown type: %s\n", Core::EnumName8(m_code.GetType()).c_str()); break;
	}

	m_source += header_str.ReplaceStr("<Variables>", vars.Concat(' '))
	                .ReplaceStr("<ExecutionModes>", execution_modes.Concat("\n" + String8(' ', 15)))
	                .ReplaceStr("<Capabilities>", capabilities.Concat("\n" + String8(' ', 16)))
	                .ReplaceStr("<Imports>", imports.Concat("\n" + String8(' ', 15)))
	                .ReplaceStr("<Extensions>", extensions.Concat("\n" + String8(' ', 15)));
}

void Spirv::WriteDebug()
{
	if (Config::SpirvDebugPrintfEnabled())
	{
		int index = 0;
		for (const auto& p: m_code.GetDebugPrintfs())
		{
			m_source += String8::FromPrintf("%%printf_str_%d = OpString \"%s\"", index, p.format.C_Str());
			index++;
		}
	}
}

void Spirv::WriteAnnotations()
{
	static const char* pixel_annotations   = R"(
               ; Annotations
               OpDecorate %outColor Location 0
               <Variables>
)";
	static const char* vertex_annotations  = R"(
               ; Annotations
               OpDecorate %gl_VertexIndex BuiltIn VertexIndex
               OpDecorate %gl_InstanceIndex BuiltIn InstanceIndex
               OpMemberDecorate %gl_PerVertex 0 BuiltIn Position
               OpMemberDecorate %gl_PerVertex 1 BuiltIn PointSize 
               OpMemberDecorate %gl_PerVertex 2 BuiltIn ClipDistance
               OpMemberDecorate %gl_PerVertex 3 BuiltIn CullDistance
               OpDecorate %gl_PerVertex Block
               ; OpDecorate %param0 Location 0
               <Variables>
)";
	static const char* compute_annotations = R"(
               ; Annotations
               OpDecorate %gl_LocalInvocationID BuiltIn LocalInvocationId
               OpDecorate %gl_WorkGroupID BuiltIn WorkgroupId
               OpDecorate %gl_WorkGroupSize BuiltIn WorkgroupSize
               <Variables>
)";

	Core::StringList8 vars;

	switch (m_code.GetType())
	{
		case ShaderType::Pixel:
			if (m_ps_input_info != nullptr)
			{
				for (uint32_t i = 0; i < m_ps_input_info->input_num; i++)
				{
					EXIT_NOT_IMPLEMENTED((m_ps_input_info->interpolator_settings[i] & ~static_cast<uint32_t>(0x41fu)) != 0);

					bool     flat     = (m_ps_input_info->interpolator_settings[i] & 0x400u) != 0;
					uint32_t location = m_ps_input_info->interpolator_settings[i] & 0x1fu;

					if (flat)
					{
						vars.Add(String8::FromPrintf("OpDecorate %%attr%u Flat", i));
					}
					vars.Add(String8::FromPrintf("OpDecorate %%attr%u Location %u", i, location));
				}
				if (m_ps_input_info->ps_pos_xy)
				{
					vars.Add("OpDecorate %gl_FragCoord BuiltIn FragCoord");
				}
			}
			m_source += String8(pixel_annotations).ReplaceStr("<Variables>", vars.Concat("\n" + String8(' ', 15)));
			break;
		case ShaderType::Vertex:
			if (m_vs_input_info != nullptr)
			{
				for (int i = 0; i < m_vs_input_info->resources_num; i++)
				{
					vars.Add(String8::FromPrintf("OpDecorate %%attr%d Location %d", i, i));
				}
				for (int i = 0; i < m_vs_input_info->export_count; i++)
				{
					vars.Add(String8::FromPrintf("OpDecorate %%param%d Location %d", i, i));
				}
			}
			m_source += String8(vertex_annotations).ReplaceStr("<Variables>", vars.Concat("\n" + String8(' ', 15)));
			break;
		case ShaderType::Compute:
			m_source += String8(compute_annotations).ReplaceStr("<Variables>", vars.Concat("\n" + String8(' ', 15)));
			break;
		default: EXIT("unknown type: %s\n", Core::EnumName8(m_code.GetType()).c_str()); break;
	}

	static const char* storage_buffers_annotations = R"(
       OpDecorate %buffers_runtimearr_float ArrayStride 4
       OpMemberDecorate %BufferObject 0 Offset 0
       OpDecorate %BufferObject Block
       OpDecorate %buf DescriptorSet <DescriptorSet>
       OpDecorate %buf Binding <BindingIndex>
)";

	static const char* textures_annotations_s = R"(
       OpDecorate %textures2D_S DescriptorSet <DescriptorSet>
       OpDecorate %textures2D_S Binding <BindingIndex>
)";

	static const char* textures_annotations_l = R"(
       OpDecorate %textures2D_L DescriptorSet <DescriptorSet>
       OpDecorate %textures2D_L Binding <BindingIndex>
)";

	static const char* samplers_annotations = R"(
       OpDecorate %samplers DescriptorSet <DescriptorSet>
       OpDecorate %samplers Binding <BindingIndex>
)";
	static const char* gds_annotations      = R"(
               OpDecorate %gds_runtimearr_uint ArrayStride 4
               OpMemberDecorate %GDS 0 Coherent
               OpMemberDecorate %GDS 0 Offset 0
               OpDecorate %GDS Block
               OpDecorate %gds DescriptorSet <DescriptorSet>
               OpDecorate %gds Binding <BindingIndex>
)";

	static const char* vsharp_annotations = R"(
       OpDecorate %vsharp_arr_uint_uint_4 ArrayStride 4
       OpDecorate %vsharp_arr__arr_uint_uint_4_uint_<buffers_num> ArrayStride 16
	   OpMemberDecorate %BufferResource 0 Offset <Offset>
       OpDecorate %BufferResource Block
)";
	static const char* vsharp_uniform_annotations = R"(
       OpDecorate %vsharp_uniform_arr_uint_<buffers_num> ArrayStride 16
       OpMemberDecorate %BufferResource 0 Offset <Offset>
       OpDecorate %BufferResource Block
       OpDecorate %vsharp DescriptorSet 2
       OpDecorate %vsharp Binding 0
)";

	if (m_bind != nullptr)
	{
		if (m_bind->storage_buffers.buffers_num > 0)
		{
			m_source += String8(storage_buffers_annotations)
			                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
			                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->storage_buffers.binding_index));
			if (!ShaderCodeHasStorageBufferWrites(m_code))
			{
				m_source += "\n       OpDecorate %buf NonWritable\n";
			}
		}
		if (m_bind->textures2D.textures2d_sampled_num > 0)
		{
			m_source += String8(textures_annotations_s)
			                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
			                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->textures2D.binding_sampled_index));
		}
		if (m_bind->textures2D.textures2d_storage_num > 0)
		{
			m_source += String8(textures_annotations_l)
			                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
			                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->textures2D.binding_storage_index));
			if (ShaderGetUniformTextureType(m_bind->textures2D, ShaderTextureUsage::ReadWrite) == ShaderTextureResource::TYPE_8_COMPAT ||
			    ShaderComputeRequiresPostDispatchImageStoreBarrier(m_code))
			{
				m_source += "\n       OpDecorate %textures2D_L Coherent\n";
			}
		}
		if (m_bind->samplers.samplers_num > 0)
		{
			m_source += String8(samplers_annotations)
			                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
			                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->samplers.binding_index));
		}
		if (m_bind->gds_pointers.pointers_num > 0)
		{
			m_source += String8(gds_annotations)
			                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
			                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->gds_pointers.binding_index));
		}
		if (m_bind->push_constant_size > 0)
		{
			m_source += String8(m_bind->constant_storage == ShaderConstantStorage::UniformBuffer ?
			                        vsharp_uniform_annotations : vsharp_annotations)
			                .ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->push_constant_size / 16))
			                .ReplaceStr("<Offset>", String8::FromPrintf("%u", m_bind->push_constant_offset));
		}
	}
}

void Spirv::WriteTypes()
{
	static const char* types = R"(
                               ; Types
                         %void = OpTypeVoid 
                        %float = OpTypeFloat 32
                          %int = OpTypeInt 32 1 
                         %uint = OpTypeInt 32 0
                         %bool = OpTypeBool 
                      %v2float = OpTypeVector %float 2
                      %v3float = OpTypeVector %float 3 
                      %v4float = OpTypeVector %float 4
                       %v2uint = OpTypeVector %uint 2 
                       %v3uint = OpTypeVector %uint 3
                       %v4uint = OpTypeVector %uint 4
                        %v2int = OpTypeVector %int 2
                        %v4int = OpTypeVector %int 4
                 %undef_v2uint = OpUndef %v2uint
               %_ptr_Input_int = OpTypePointer Input %int
              %_ptr_Input_uint = OpTypePointer Input %uint
             %_ptr_Input_float = OpTypePointer Input %float
           %_ptr_Input_v2float = OpTypePointer Input %v2float
             %_ptr_Input_v2int = OpTypePointer Input %v2int
             %_ptr_Input_v4int = OpTypePointer Input %v4int
           %_ptr_Input_v3float = OpTypePointer Input %v3float
           %_ptr_Input_v4float = OpTypePointer Input %v4float
            %_ptr_Input_v3uint = OpTypePointer Input %v3uint
          %_ptr_Output_v4float = OpTypePointer Output %v4float
          %_ptr_Function_float = OpTypePointer Function %float
           %_ptr_Function_bool = OpTypePointer Function %bool
            %_ptr_Function_int = OpTypePointer Function %int
           %_ptr_Function_uint = OpTypePointer Function %uint
        %_ptr_Function_v2float = OpTypePointer Function %v2float
        %_ptr_Function_v3float = OpTypePointer Function %v3float
        %_ptr_Function_v4float = OpTypePointer Function %v4float
           %_ptr_Uniform_float = OpTypePointer Uniform %float
     %_ptr_StorageBuffer_float = OpTypePointer StorageBuffer %float
      %_ptr_StorageBuffer_uint = OpTypePointer StorageBuffer %uint
                     %ResTypeI = OpTypeStruct %int %int
                     %ResTypeU = OpTypeStruct %uint %uint                       
                %function_void = OpTypeFunction %void
              %function_fetch1 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float 
              %function_fetch2 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_v2float
              %function_fetch3 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_v3float
              %function_fetch4 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_v4float
                 %function_u_u = OpTypeFunction %uint %uint %uint
               %function_u_u_u = OpTypeFunction %uint %uint %uint %uint
                  %function_u1 = OpTypeFunction %uint %uint
            %function_u2_u_u_u = OpTypeFunction %v2uint %uint %uint %uint
               %function_b_f_f = OpTypeFunction %bool %float %float
                 %function_i_i = OpTypeFunction %int %int %int
               %function_shift = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint
    %function_tbuffer_load_format_xyzw = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
    %function_buffer_load_store_float1 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
    %function_buffer_load_store_float2 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
          %function_buffer_load_float4 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
 %function_tbuffer_load_store_format_x = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
%function_tbuffer_load_store_format_xy = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
          %function_sbuffer_load_dword = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_int %_ptr_Function_int
        %function_sbuffer_load_dword_2 = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_int %_ptr_Function_int
        %function_sbuffer_load_dword_4 = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_int %_ptr_Function_int
        %function_sbuffer_load_dword_8 = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_int %_ptr_Function_int
       %function_sbuffer_load_dword_16 = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_int %_ptr_Function_int
)";

	static const char* pixel_types = R"(	
)";

	static const char* vertex_types = R"(	
            %array_length = OpConstant %uint 1
        %int_per_vertex_0 = OpConstant %int 0
       %_arr_float_uint_1 = OpTypeArray %float %array_length
            %gl_PerVertex = OpTypeStruct %v4float %float %_arr_float_uint_1 %_arr_float_uint_1
%_ptr_Output_gl_PerVertex = OpTypePointer Output %gl_PerVertex
)";

	static const char* compute_types = R"(	
)";

	m_source += types;
	if (m_vs_input_info != nullptr && m_vs_input_info->rect_list)
	{
		m_source += "%rect_function = OpTypeFunction %void %int\n";
	}

	switch (m_code.GetType())
	{
		case ShaderType::Vertex: m_source += vertex_types; break;
		case ShaderType::Pixel: m_source += pixel_types; break;
		case ShaderType::Compute: m_source += compute_types; break;
		default: EXIT("unknown type: %s\n", Core::EnumName8(m_code.GetType()).c_str()); break;
	}

	static const char* storage_buffers_types = R"(	
                               %buffers_runtimearr_float = OpTypeRuntimeArray %float
                                           %BufferObject = OpTypeStruct %buffers_runtimearr_float
                         %buffers_num_uint_<buffers_num> = OpConstant %uint <buffers_num>
                   %_arr_BufferObject_uint_<buffers_num> = OpTypeArray %BufferObject %buffers_num_uint_<buffers_num>
%_ptr_StorageBuffer__arr_BufferObject_uint_<buffers_num> = OpTypePointer StorageBuffer %_arr_BufferObject_uint_<buffers_num>
)";

	static const char* textures_sampled_types = R"(
                                             %ImageS = OpTypeImage %float 2D 0 0 0 1 Unknown
                    %textures2D_S_uint_<buffers_num> = OpConstant %uint <buffers_num>
                     %_arr_ImageS_uint_<buffers_num> = OpTypeArray %ImageS %textures2D_S_uint_<buffers_num>
%_ptr_UniformConstant__arr_ImageS_uint_<buffers_num> = OpTypePointer UniformConstant %_arr_ImageS_uint_<buffers_num>
                        %_ptr_UniformConstant_ImageS = OpTypePointer UniformConstant %ImageS
                                       %SampledImage = OpTypeSampledImage %ImageS	
)";

	static const char* textures_loaded_types = R"(
                                             %ImageL = OpTypeImage %float 2D 0 0 0 2 <StorageImageFormat>
                    %textures2D_L_uint_<buffers_num> = OpConstant %uint <buffers_num>
                     %_arr_ImageL_uint_<buffers_num> = OpTypeArray %ImageL %textures2D_L_uint_<buffers_num>
%_ptr_UniformConstant__arr_ImageL_uint_<buffers_num> = OpTypePointer UniformConstant %_arr_ImageL_uint_<buffers_num>
                        %_ptr_UniformConstant_ImageL = OpTypePointer UniformConstant %ImageL	
)";

	static const char* samplers_types = R"(	
                                             %Sampler = OpTypeSampler
                         %samplers_uint_<buffers_num> = OpConstant %uint <buffers_num>
                     %_arr_Sampler_uint_<buffers_num> = OpTypeArray %Sampler %samplers_uint_<buffers_num>
%_ptr_UniformConstant__arr_Sampler_uint_<buffers_num> = OpTypePointer UniformConstant %_arr_Sampler_uint_<buffers_num>
                        %_ptr_UniformConstant_Sampler = OpTypePointer UniformConstant %Sampler
)";

	static const char* gds_types = R"(	
            %gds_runtimearr_uint = OpTypeRuntimeArray %uint
                    %GDS = OpTypeStruct %gds_runtimearr_uint
            %_ptr_StorageBuffer_GDS = OpTypePointer StorageBuffer %GDS
)";

	static const char* vsharp_types = R"(	
         %vsharp_buffers_num_uint_<buffers_num> = OpConstant %uint <buffers_num>
                             %vsharp_num_uint_4 = OpConstant %uint 4
                        %vsharp_arr_uint_uint_4 = OpTypeArray %uint %vsharp_num_uint_4
%vsharp_arr__arr_uint_uint_4_uint_<buffers_num> = OpTypeArray %vsharp_arr_uint_uint_4 %vsharp_buffers_num_uint_<buffers_num>
                                %BufferResource = OpTypeStruct %vsharp_arr__arr_uint_uint_4_uint_<buffers_num>           
              %_ptr_PushConstant_BufferResource = OpTypePointer PushConstant %BufferResource
                        %_ptr_PushConstant_uint = OpTypePointer PushConstant %uint
)";
	// std140 arrays of scalars would have stride 16 per scalar. A vec4 array
	// retains the existing tightly packed four-dword resource records.
	static const char* vsharp_uniform_types = R"(
         %vsharp_buffers_num_uint_<buffers_num> = OpConstant %uint <buffers_num>
         %vsharp_uniform_arr_uint_<buffers_num> = OpTypeArray %v4uint %vsharp_buffers_num_uint_<buffers_num>
                                %BufferResource = OpTypeStruct %vsharp_uniform_arr_uint_<buffers_num>
              %_ptr_UniformConstantData_BufferResource = OpTypePointer Uniform %BufferResource
                        %_ptr_PushConstant_uint = OpTypePointer Uniform %uint
)";

	if (m_bind != nullptr)
	{
		if (m_bind->storage_buffers.buffers_num > 0)
		{
			m_source +=
			    String8(storage_buffers_types).ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->storage_buffers.buffers_num));
		}
		if (m_bind->textures2D.textures2d_sampled_num > 0)
		{
			m_source += String8(textures_sampled_types)
			                .ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->textures2D.textures2d_sampled_num));
		}
		if (m_bind->textures2D.textures2d_storage_num > 0)
		{
			const auto storage_format = ShaderGetUniformStorageImageFormat(m_bind->textures2D);
			EXIT_NOT_IMPLEMENTED(storage_format == ShaderStorageImageFormat::Invalid);
			const char* spirv_format = storage_format == ShaderStorageImageFormat::R8Unorm ? "R8" : "Rgba8";
			m_source += String8(textures_loaded_types)
			                .ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->textures2D.textures2d_storage_num))
			                .ReplaceStr("<StorageImageFormat>", spirv_format);
		}
		if (m_bind->samplers.samplers_num > 0)
		{
			m_source += String8(samplers_types).ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->samplers.samplers_num));
		}
		if (m_bind->gds_pointers.pointers_num > 0)
		{
			m_source += String8(gds_types);
		}
		if (m_bind->push_constant_size > 0)
		{
			m_source += String8(m_bind->constant_storage == ShaderConstantStorage::UniformBuffer ?
			                        vsharp_uniform_types : vsharp_types)
			                .ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->push_constant_size / 16));
		}
	}
}

void Spirv::WriteConstants()
{
	FindConstants();

	static const char* comment = R"(	
    ; Constants
         %true = OpConstantTrue %bool
        %false = OpConstantFalse %bool
    %float_2pi = OpConstant %float 6.283185307179586476925286766559
)";

	m_source += comment;

	for (const auto& c: m_constants)
	{
		m_source += String8::FromPrintf("%%%s = OpConstant %%%s %s\n", c.id.c_str(), c.type_str.c_str(), c.value_str.c_str());
	}
}

void Spirv::WriteGlobalVariables()
{
	static const char* pixel_variables   = R"(
              ;Variables
   %outColor = OpVariable %_ptr_Output_v4float Output
               <Variables> 
)";
	static const char* vertex_variables  = R"(
              ;Variables
    %gl_VertexIndex = OpVariable %_ptr_Input_int Input 
  %gl_InstanceIndex = OpVariable %_ptr_Input_int Input
      %outPerVertex = OpVariable %_ptr_Output_gl_PerVertex Output
            ; %param0 = OpVariable %_ptr_Output_v4float Output            
               <Variables>
)";
	static const char* compute_variables = R"(
              ;Variables
%gl_LocalInvocationID = OpVariable %_ptr_Input_v3uint Input 
      %gl_WorkGroupID = OpVariable %_ptr_Input_v3uint Input           
               <Variables>
)";

	Core::StringList8 vars;

	if (m_bind != nullptr)
	{
		if (m_bind->storage_buffers.buffers_num > 0)
		{
			vars.Add(String8::FromPrintf("%%buf = OpVariable %%_ptr_StorageBuffer__arr_BufferObject_uint_%d StorageBuffer",
			                             m_bind->storage_buffers.buffers_num));
		}
		if (m_bind->textures2D.textures2d_sampled_num > 0)
		{
			vars.Add(String8::FromPrintf("%%textures2D_S = OpVariable %%_ptr_UniformConstant__arr_ImageS_uint_%d UniformConstant",
			                             m_bind->textures2D.textures2d_sampled_num));
		}
		if (m_bind->textures2D.textures2d_storage_num > 0)
		{
			vars.Add(String8::FromPrintf("%%textures2D_L = OpVariable %%_ptr_UniformConstant__arr_ImageL_uint_%d UniformConstant",
			                             m_bind->textures2D.textures2d_storage_num));
		}
		if (m_bind->samplers.samplers_num > 0)
		{
			vars.Add(String8::FromPrintf("%%samplers = OpVariable %%_ptr_UniformConstant__arr_Sampler_uint_%d UniformConstant",
			                             m_bind->samplers.samplers_num));
		}
		if (m_bind->gds_pointers.pointers_num > 0)
		{
			vars.Add("%gds = OpVariable %_ptr_StorageBuffer_GDS StorageBuffer");
		}
		if (m_bind->push_constant_size > 0)
		{
			vars.Add(m_bind->constant_storage == ShaderConstantStorage::UniformBuffer ?
			             "%vsharp = OpVariable %_ptr_UniformConstantData_BufferResource Uniform" :
			             "%vsharp = OpVariable %_ptr_PushConstant_BufferResource PushConstant");
		}
	}

	switch (m_code.GetType())
	{
		case ShaderType::Pixel:
			if (m_ps_input_info != nullptr)
			{
				for (uint32_t i = 0; i < m_ps_input_info->input_num; i++)
				{
					vars.Add(String8::FromPrintf("%%attr%d = OpVariable %%_ptr_Input_v4float Input", i));
				}
				if (m_ps_input_info->ps_pos_xy)
				{
					vars.Add("%gl_FragCoord = OpVariable %_ptr_Input_v4float Input");
				}
			}
			m_source += String8(pixel_variables).ReplaceStr("<Variables>", vars.Concat("\n" + String8(' ', 15)));
			break;
		case ShaderType::Vertex:
			if (m_vs_input_info != nullptr)
			{
				for (int i = 0; i < m_vs_input_info->resources_num; i++)
				{
					const auto& destination = m_vs_input_info->resources_dst[i];
					if (ShaderVertexInputSint16x2Compatible(m_vs_input_info->resources[i], destination.registers_num))
					{
						vars.Add(String8::FromPrintf("%%attr%d = OpVariable %%_ptr_Input_v2int Input", i));
						continue;
					}
					if (ShaderVertexInputSint16x4Compatible(m_vs_input_info->resources[i], destination.registers_num))
					{
						vars.Add(String8::FromPrintf("%%attr%d = OpVariable %%_ptr_Input_v4int Input", i));
						continue;
					}
					switch (destination.registers_num)
					{
						case 1: vars.Add(String8::FromPrintf("%%attr%d = OpVariable %%_ptr_Input_float Input", i)); break;
						case 2: vars.Add(String8::FromPrintf("%%attr%d = OpVariable %%_ptr_Input_v2float Input", i)); break;
						case 3: vars.Add(String8::FromPrintf("%%attr%d = OpVariable %%_ptr_Input_v3float Input", i)); break;
						case 4: vars.Add(String8::FromPrintf("%%attr%d = OpVariable %%_ptr_Input_v4float Input", i)); break;
						default: EXIT("invalid registers_num: %d\n", destination.registers_num);
					}
				}
				for (int i = 0; i < m_vs_input_info->export_count; i++)
				{
					vars.Add(String8::FromPrintf("%%param%d = OpVariable %%_ptr_Output_v4float Output", i));
				}
			}
			m_source += String8(vertex_variables).ReplaceStr("<Variables>", vars.Concat("\n" + String8(' ', 15)));
			break;
		case ShaderType::Compute:
			if (m_cs_input_info != nullptr)
			{
				vars.Add(String8::FromPrintf("%%gl_WorkGroupSize = OpConstantComposite %%v3uint %%uint_%u %%uint_%u %%uint_%u",
				                             m_cs_input_info->threads_num[0], m_cs_input_info->threads_num[1],
				                             m_cs_input_info->threads_num[2]));
			}
			m_source += String8(compute_variables).ReplaceStr("<Variables>", vars.Concat("\n" + String8(' ', 15)));
			break;
		default: EXIT("unknown type: %s\n", Core::EnumName8(m_code.GetType()).c_str()); break;
	}
}

void Spirv::WriteMainProlog()
{
	if (m_vs_input_info != nullptr && m_vs_input_info->rect_list)
	{
		m_source += "%rect_guest = OpFunction %void None %rect_function\n"
		            "%rect_source_index = OpFunctionParameter %int\n"
		            "%main_label = OpLabel\n";
		return;
	}
	static const char* text = R"(
                   ; Function main
                   ; Prolog
       %main       = OpFunction %void None %function_void
       %main_label = OpLabel 
)";

	m_source += text;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void Spirv::WriteLocalVariables()
{
	FindVariables();

	static const char* comment = R"(	
    ; Registers
)";

	m_source += comment;

	for (const auto& c: m_variables)
	{
		auto value = operand_variable_to_str(c.op);
		m_source += String8::FromPrintf("%%%s = OpVariable %%_ptr_Function_%s Function\n", value.value.c_str(),
		                                Core::EnumName(value.type).ToLower().C_Str());
	}

	static const char* common_vars = R"(
             %temp_float = OpVariable %_ptr_Function_float Function
           %temp_v2float = OpVariable %_ptr_Function_v2float Function
           %temp_v3float = OpVariable %_ptr_Function_v3float Function
	       %temp_v4float = OpVariable %_ptr_Function_v4float Function
           %temp_int_0 = OpVariable %_ptr_Function_int Function
           %temp_int_1 = OpVariable %_ptr_Function_int Function
           %temp_int_2 = OpVariable %_ptr_Function_int Function
           %temp_int_3 = OpVariable %_ptr_Function_int Function
           %temp_int_4 = OpVariable %_ptr_Function_int Function
           %temp_int_5 = OpVariable %_ptr_Function_int Function
           %temp_uint_0 = OpVariable %_ptr_Function_uint Function
           %temp_uint_1 = OpVariable %_ptr_Function_uint Function
           %temp_uint_2 = OpVariable %_ptr_Function_uint Function
           %temp_uint_3 = OpVariable %_ptr_Function_uint Function
           %temp_uint_4 = OpVariable %_ptr_Function_uint Function
           %temp_uint_5 = OpVariable %_ptr_Function_uint Function
)";

	m_source += common_vars;

	if (m_code.GetType() == ShaderType::Vertex)
	{
		static const char* text = R"(
       %vertex_index_int = OpLoad %int %gl_VertexIndex
           %vertex_index = OpBitcast %float %vertex_index_int
                           OpStore %<v> %vertex_index           
       %instance_index_int = OpLoad %int %gl_InstanceIndex
           %instance_index = OpBitcast %float %instance_index_int
                           OpStore %<i> %instance_index           
)";
		auto initialization = String8(text).ReplaceStr("<v>", "v0").ReplaceStr("<i>", "v3");
		if (m_vs_input_info != nullptr && m_vs_input_info->rect_list)
		{
			initialization = initialization.ReplaceStr("OpLoad %int %gl_VertexIndex", "OpCopyObject %int %rect_source_index");
		}
		m_source += initialization;
	}

	if (m_code.GetType() == ShaderType::Pixel)
	{
		if (m_ps_input_info != nullptr && m_ps_input_info->ps_pos_xy)
		{
			static const char* text = R"(  
         %FragCoord_px = OpAccessChain %_ptr_Input_float %gl_FragCoord %uint_0
         %FragCoord_x = OpLoad %float %FragCoord_px
               OpStore %v2 %FragCoord_x
         %FragCoord_py = OpAccessChain %_ptr_Input_float %gl_FragCoord %uint_1
         %FragCoord_y = OpLoad %float %FragCoord_py
               OpStore %v3 %FragCoord_y     
)";
			m_source += text;
		}
	}

	if (m_code.GetType() == ShaderType::Compute)
	{
		static const char* text_thread_id = R"(
		%LocalInvocationID_114_<i> = OpAccessChain %_ptr_Input_uint %gl_LocalInvocationID %uint_<i>
        %LocalInvocationID_115_<i> = OpLoad %uint %LocalInvocationID_114_<i>
        %LocalInvocationID_116_<i> = OpBitcast %float %LocalInvocationID_115_<i>
               OpStore %v<i> %LocalInvocationID_116_<i>		   
)";

		static const char* text_group_id = R"(
        %WorkGroupID_120_<i> = OpAccessChain %_ptr_Input_uint %gl_WorkGroupID %uint_<i>
        %WorkGroupID_121_<i> = OpLoad %uint %WorkGroupID_120_<i>
               OpStore %<WorkGroupReg> %WorkGroupID_121_<i>       
)";
		if (m_cs_input_info != nullptr)
		{
			for (int i = 0; i < m_cs_input_info->thread_ids_num; i++)
			{
				m_source += String8(text_thread_id).ReplaceStr("<i>", String8::FromPrintf("%d", i));
			}

			int reg = 0;
			for (int i = 0; i < 3; i++)
			{
				if (m_cs_input_info->group_id[i])
				{
					m_source += String8(text_group_id)
					                .ReplaceStr("<WorkGroupReg>", String8::FromPrintf("s%u", m_cs_input_info->workgroup_register + reg))
					                .ReplaceStr("<i>", String8::FromPrintf("%d", i));
					reg++;
				}
			}
		}
	}

	if (m_bind != nullptr)
	{
		static const char* text = R"(
         %vsharp_<reg> = OpAccessChain %_ptr_PushConstant_uint %vsharp %int_0 %int_<buffer> %int_<field>
         %vsharp_value_<reg> = OpLoad %uint %vsharp_<reg>
               OpStore %<reg> %vsharp_value_<reg>
		)";

		int buffer_index = 0;

		m_extended_mapping.Reset();

		for (int i = 0; i < m_bind->storage_buffers.buffers_num; i++)
		{
			int  start_reg = m_bind->storage_buffers.start_register[i];
			bool extended  = m_bind->storage_buffers.extended[i];

			EXIT_IF(buffer_index + i >= static_cast<int>(m_bind->push_constant_size) / 16);

			String8 buffer = String8::FromPrintf("%d", buffer_index + i);
			for (int f = 0; f < 4; f++)
			{
				if (extended)
				{
					EXIT_IF(start_reg < 16);
					EXIT_NOT_IMPLEMENTED(!m_extended_mapping.TryMap(start_reg - 16 + f, buffer_index + i, f));
				} else
				{
					String8 reg   = String8::FromPrintf("s%d", start_reg + f);
					String8 field = String8::FromPrintf("%d", f);
					m_source += String8(text).ReplaceStr("<reg>", reg).ReplaceStr("<buffer>", buffer).ReplaceStr("<field>", field);
				}
			}
		}

		buffer_index += m_bind->storage_buffers.buffers_num;

		for (int i = 0; i < m_bind->textures2D.textures_num; i++)
		{
			int  start_reg = m_bind->textures2D.desc[i].start_register;
			bool extended  = m_bind->textures2D.desc[i].extended;

			for (int ti = 0; ti < 2; ti++)
			{
				EXIT_IF(buffer_index + i * 2 + ti >= static_cast<int>(m_bind->push_constant_size) / 16);

				String8 buffer = String8::FromPrintf("%d", buffer_index + i * 2 + ti);
				for (int f = 0; f < 4; f++)
				{
					if (extended)
					{
						EXIT_IF(start_reg < 16);
						EXIT_NOT_IMPLEMENTED(!m_extended_mapping.TryMap(start_reg - 16 + 4 * ti + f, buffer_index + i * 2 + ti, f));
					} else
					{
						String8 reg   = String8::FromPrintf("s%d", start_reg + 4 * ti + f);
						String8 field = String8::FromPrintf("%d", f);
						m_source += String8(text).ReplaceStr("<reg>", reg).ReplaceStr("<buffer>", buffer).ReplaceStr("<field>", field);
					}
				}
			}
		}

		buffer_index += m_bind->textures2D.textures_num * 2;

		for (int i = 0; i < m_bind->samplers.samplers_num; i++)
		{
			int  start_reg = m_bind->samplers.start_register[i];
			bool extended  = m_bind->samplers.extended[i];

			EXIT_IF(buffer_index + i >= static_cast<int>(m_bind->push_constant_size) / 16);

			String8 buffer = String8::FromPrintf("%d", buffer_index + i);
			for (int f = 0; f < 4; f++)
			{
				if (extended)
				{
					EXIT_IF(start_reg < 16);
					EXIT_NOT_IMPLEMENTED(!m_extended_mapping.TryMap(start_reg - 16 + f, buffer_index + i, f));
				} else
				{
					String8 reg   = String8::FromPrintf("s%d", start_reg + f);
					String8 field = String8::FromPrintf("%d", f);
					m_source += String8(text).ReplaceStr("<reg>", reg).ReplaceStr("<buffer>", buffer).ReplaceStr("<field>", field);
				}
			}
		}

		buffer_index += m_bind->samplers.samplers_num;

		for (int i = 0; i < m_bind->gds_pointers.pointers_num; i++)
		{
			int  start_reg = m_bind->gds_pointers.start_register[i];
			bool extended  = m_bind->gds_pointers.extended[i];

			EXIT_IF(buffer_index + i / 4 >= static_cast<int>(m_bind->push_constant_size) / 16);

			if (extended)
			{
				EXIT_IF(start_reg < 16);
				EXIT_NOT_IMPLEMENTED(!m_extended_mapping.TryMap(start_reg - 16, buffer_index + i / 4, i % 4));
			} else
			{
				String8 buffer = String8::FromPrintf("%d", buffer_index + i / 4);
				String8 reg    = String8::FromPrintf("s%d", start_reg);
				String8 field  = String8::FromPrintf("%d", i % 4);
				m_source += String8(text).ReplaceStr("<reg>", reg).ReplaceStr("<buffer>", buffer).ReplaceStr("<field>", field);
			}
		}

		buffer_index += (m_bind->gds_pointers.pointers_num > 0 ? (m_bind->gds_pointers.pointers_num - 1) / 4 + 1 : 0);

		for (int i = 0; i < m_bind->direct_sgprs.sgprs_num; i++)
		{
			int start_reg = m_bind->direct_sgprs.start_register[i];

			EXIT_IF(buffer_index + i / 4 >= static_cast<int>(m_bind->push_constant_size) / 16);

			String8 buffer = String8::FromPrintf("%d", buffer_index + i / 4);
			String8 reg    = String8::FromPrintf("s%d", start_reg);
			String8 field  = String8::FromPrintf("%d", i % 4);
			m_source += String8(text).ReplaceStr("<reg>", reg).ReplaceStr("<buffer>", buffer).ReplaceStr("<field>", field);
		}

		/* buffer_index += (m_bind->direct_sgprs.sgprs_num > 0 ? (m_bind->direct_sgprs.sgprs_num - 1) / 4 + 1 : 0); */

		if (m_bind->extended.used)
		{
			// TODO() load pointer

			printf("Extended mapping: ");
			for (int i = 0; i < ShaderExtendedMapping::SIZE; i++)
			{
				int buffer = 0;
				int field  = 0;
				if (m_extended_mapping.TryResolve(i, &buffer, &field))
				{
					printf("{%d, %d} ", buffer, field);
				} else
				{
					// Preserve the established diagnostic shape; validity is enforced by
					// TryResolve rather than encoded through this legacy dump.
					printf("{0, 0} ");
				}
			}
			printf("\n");
		}
	}

	static const char* common_init = R"(
               OpStore %exec_lo %uint_1
               OpStore %exec_hi %uint_0
               OpStore %execz %uint_0
               OpStore %scc %uint_0
	)";

	m_source += common_init;
	m_source += "\n";
}

void Spirv::WriteLabel(int index, bool& block_terminated)
{
	if (index > 0)
	{
		const auto& instructions = m_code.GetInstructions();
		const auto& inst         = instructions.At(index);

		auto& labels     = m_code.GetLabels();
		int   labels_num = 0;
		for (uint32_t i = labels.Size(); i > 0; i--)
		{
			auto& label = labels[i - 1];
			if (!label.IsDisabled() && label.GetDst() == inst.pc)
			{
				static const char* text = R"(
                   <branch>
                   %<label> = OpLabel
		)";

				bool discard = m_code.ReadBlock(label.GetDst()).is_discard;

				bool skip_branch = (discard || (block_terminated && labels_num == 0));

				m_source +=
				    String8(text).ReplaceStr("<branch>", (skip_branch ? "" : "OpBranch %<label>")).ReplaceStr("<label>", label.ToString());
				labels_num++;
				block_terminated = false;

				if (discard)
				{
					label.Disable();
					break;
				}
			}
		}
	}
}

void Spirv::ModifyCode()
{
	struct DiscardLabel
	{
		ShaderControlFlowBlock block;
		int                    num = 0;
	};
	const auto&          labels = m_code.GetLabels();
	Vector<DiscardLabel> dls;
	for (const auto& l: labels)
	{
		if (!l.IsDisabled())
		{
			int  num = 0;
			auto pc  = l.GetDst();
			for (const auto& l2: labels)
			{
				if (l2.GetDst() == pc)
				{
					num++;
				}
			}
			EXIT_IF(num == 0);
			if (num > 1)
			{
				if (auto block = m_code.ReadBlock(pc); block.is_discard)
				{
					if (!dls.Contains(pc, [](auto d, auto pc) { return d.block.pc == pc; }))
					{
						dls.Add(DiscardLabel({block, num - 1}));
					}
				}
			}
		}
	}
	for (const auto& dl: dls)
	{
		auto block = m_code.ReadIntructions(dl.block);
		for (int i = 0; i < dl.num; i++)
		{
			// Duplicate discard block if there are different branches with the same label
			m_code.GetInstructions().Add(block);
		}
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void Spirv::WriteInstructions()
{
	ModifyCode();

	int         index        = -1;
	const auto& instructions = m_code.GetInstructions();
	bool        need_debug   = (Config::SpirvDebugPrintfEnabled() && !m_code.GetDebugPrintfs().IsEmpty());
	bool        block_terminated = false;
	for (const auto& inst: instructions)
	{
		index++;

		WriteLabel(index, block_terminated);

		String8 src = ShaderCode::DbgInstructionToStr(inst);
		String8 dst;
		String8 dst_debug;

		bool ok = false;

		const auto* func = RecompFunc(inst.type, inst.format);

		if (func != nullptr)
		{
			EXIT_IF(func->type != inst.type);
			EXIT_IF(func->format != inst.format);
			ok = func->func(index, m_code, &dst, this, func->param, func->scc_check);
		}

		if (!ok)
		{
			printf("%s\n", m_source.c_str());
			const auto* vertex_input = GetVsInputInfo();
			if (inst.type == ShaderInstructionType::SSwappcB64)
			{
				const auto current_index = static_cast<uint32_t>(index);
				const uint32_t first = current_index > 4 ? current_index - 4 : 0;
				for (uint32_t prefix_index = first; prefix_index < current_index; ++prefix_index)
				{
					const auto& prefix = instructions.At(prefix_index);
					std::fprintf(stderr, "[SHADER_FETCH_PREFIX] index=%u pc=%08" PRIx32 " instruction=%s\n",
					             prefix_index, prefix.pc, ShaderCode::DbgInstructionToStr(prefix).c_str());
				}
			}
			EXIT("Can't recompile: %s (stage=%s index=%u pc=%08" PRIx32 " fetch_external=%u fetch_shader_reg=%d)\n",
			     src.c_str(), Core::EnumName8(m_code.GetType()).c_str(), static_cast<uint32_t>(index), inst.pc,
			     vertex_input != nullptr && vertex_input->fetch_external ? 1u : 0u,
			     vertex_input != nullptr ? vertex_input->fetch_shader_reg : -1);
		}

		m_source += String8::FromPrintf("; %s\n", src.c_str());
		m_source += String8::FromPrintf("%s\n", dst.c_str());
		// Empty lowerings do not reopen a terminated SPIR-V block. Retain the
		// emitted state across trace, waits and other nonemitting instructions.
		if (!dst.IsEmpty())
			block_terminated = (inst.type == ShaderInstructionType::SBranch || inst.type == ShaderInstructionType::SEndpgm ||
			                    (inst.type == ShaderInstructionType::Exp && inst.format == ShaderInstructionFormat::Mrt0OffOffComprVmDone));

		if (need_debug && Recompile_Inject_Debug(index, m_code, &dst_debug, this, nullptr, SccCheck::None))
		{
			// Debug output is an actual instruction, not an empty lowering. A
			// post-terminator injection has no admitted insertion point.
			if (block_terminated) EXIT("cannot emit shader debug printf after a terminated block\n");
			m_source += String8::FromPrintf("%s\n", dst_debug.c_str());
			block_terminated = false;
		}
	}
}

void Spirv::WriteMainEpilog()
{
	static const char* text = R"(
                   ; Epilog
                   OpFunctionEnd
)";

	m_source += text;
	if (m_vs_input_info == nullptr || !m_vs_input_info->rect_list) return;

	// Each call owns fresh Function registers and runs the complete guest prolog.
	// The admitted common output tail writes every output before it is captured.
	m_source += R"(
%main = OpFunction %void None %function_void
%rect_main_label = OpLabel
%rect_vertex = OpLoad %int %gl_VertexIndex
%rect_is_fourth = OpIEqual %bool %rect_vertex %int_3
OpSelectionMerge %rect_merge None
OpBranchConditional %rect_is_fourth %rect_fourth %rect_original
%rect_original = OpLabel
%rect_original_call = OpFunctionCall %void %rect_guest %rect_vertex
OpBranch %rect_merge
%rect_fourth = OpLabel
%rect_position_ptr = OpAccessChain %_ptr_Output_v4float %outPerVertex %int_per_vertex_0
)";
	for (int vertex = 0; vertex < 3; ++vertex)
	{
		m_source += String8::FromPrintf("%%rect_call_%d = OpFunctionCall %%void %%rect_guest %%int_%d\n"
		                                "%%rect_position_%d = OpLoad %%v4float %%rect_position_ptr\n", vertex, vertex, vertex);
		for (int parameter = 0; parameter < m_vs_input_info->export_count; ++parameter)
		{
			m_source += String8::FromPrintf("%%rect_param_%d_%d = OpLoad %%v4float %%param%d\n", vertex, parameter, parameter);
		}
	}
	m_source += R"(
%rect_xy_0 = OpVectorShuffle %v2float %rect_position_0 %rect_position_0 0 1
%rect_xy_1 = OpVectorShuffle %v2float %rect_position_1 %rect_position_1 0 1
%rect_xy_2 = OpVectorShuffle %v2float %rect_position_2 %rect_position_2 0 1
%rect_xy_sum = OpFAdd %v2float %rect_xy_1 %rect_xy_2
%rect_xy = OpFSub %v2float %rect_xy_sum %rect_xy_0
%rect_z = OpCompositeExtract %float %rect_position_0 2
%rect_w = OpCompositeExtract %float %rect_position_0 3
%rect_position = OpCompositeConstruct %v4float %rect_xy %rect_z %rect_w
OpStore %rect_position_ptr %rect_position
)";
	for (int parameter = 0; parameter < m_vs_input_info->export_count; ++parameter)
	{
		m_source += String8::FromPrintf(
		    "%%rect_param_sum_%d = OpFAdd %%v4float %%rect_param_1_%d %%rect_param_2_%d\n"
		    "%%rect_param_%d = OpFSub %%v4float %%rect_param_sum_%d %%rect_param_0_%d\n"
		    "OpStore %%param%d %%rect_param_%d\n", parameter, parameter, parameter, parameter, parameter, parameter,
		    parameter, parameter);
	}
	m_source += "OpBranch %rect_merge\n%rect_merge = OpLabel\nOpReturn\nOpFunctionEnd\n";
}

void Spirv::WriteFunctions()
{
	if (m_code.HasAnyOf({ShaderInstructionType::VRsqClampF32}))
	{
		m_source += Detail::kVRsqClampF32SpirvCore;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::VMacLegacyF32}))
	{
		m_source += Detail::kVMacLegacyF32SpirvShrJamU32;
		m_source += Detail::kVMacLegacyF32SpirvMulLegacyBits;
		m_source += Detail::kVMacLegacyF32SpirvAddRneFlushBits;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::VSadU32}))
	{
		m_source += FUNC_ABS_DIFF;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::SWqmB64}))
	{
		m_source += FUNC_WQM;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::SAddcU32}))
	{
		m_source += FUNC_ADDC;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::ImageStoreMip}))
	{
		m_source += FUNC_MIPMAP;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::VCmpOF32, ShaderInstructionType::VCmpUF32}))
	{
		m_source += FUNC_ORDERED;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::VMulLoI32, ShaderInstructionType::VMulLoU32, ShaderInstructionType::VMulHiU32,
	                     ShaderInstructionType::VMadU32U24, ShaderInstructionType::VMulU32U24, ShaderInstructionType::SMulHiU32}))
	{
		m_source += FUNC_MUL_EXTENDED;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::SLshrB64, ShaderInstructionType::SBfeU64}))
	{
		m_source += FUNC_SHIFT_RIGHT;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::SLshlB64, ShaderInstructionType::SBfeU64}))
	{
		m_source += FUNC_SHIFT_LEFT;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::SSwappcB64}))
	{
		m_source += FUNC_FETCH_1;
		m_source += FUNC_FETCH_2;
		m_source += FUNC_FETCH_3;
		m_source += FUNC_FETCH_4;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::BufferLoadDword, ShaderInstructionType::BufferLoadFormatX,
	                     ShaderInstructionType::BufferLoadFormatXy, ShaderInstructionType::BufferLoadFormatXyz,
	                     ShaderInstructionType::BufferLoadFormatXyzw, ShaderInstructionType::TBufferLoadFormatX,
	                     ShaderInstructionType::TBufferLoadFormatXyzw}))
	{
		m_source += BUFFER_LOAD_FLOAT1;
		m_source += BUFFER_LOAD_SSCALED16_X;
		if (m_code.HasAnyOf({ShaderInstructionType::BufferLoadFormatXyzw}))
		{
			m_source += BUFFER_LOAD_FLOAT2;
		}
		m_source += BUFFER_LOAD_FLOAT4;
		m_source += BUFFER_LOAD_SSCALED16_XYZW;
		m_source += TBUFFER_LOAD_FORMAT_X;
		m_source += TBUFFER_LOAD_FORMAT_XYZW;
	}

	if (ShaderCodeHasStorageBufferWrites(m_code))
	{
		m_source += BUFFER_STORE_FLOAT1;
		m_source += BUFFER_STORE_FLOAT2;
		m_source += TBUFFER_STORE_FORMAT_X;
		m_source += TBUFFER_STORE_FORMAT_XY;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::SBufferLoadDword, ShaderInstructionType::SBufferLoadDwordx2,
	                     ShaderInstructionType::SBufferLoadDwordx4, ShaderInstructionType::SBufferLoadDwordx8,
	                     ShaderInstructionType::SBufferLoadDwordx16}))
	{
		m_source += SBUFFER_LOAD_DWORD;
		m_source += SBUFFER_LOAD_DWORD_2;
		m_source += SBUFFER_LOAD_DWORD_4;
		m_source += SBUFFER_LOAD_DWORD_8;
		m_source += SBUFFER_LOAD_DWORD_16;
	}
}

void Spirv::FindConstants()
{
	m_constants.Clear();
	AddConstantFloat(0.0f);
	AddConstantFloat(0.5f);
	AddConstantFloat(1.0f);
	AddConstantFloat(2.0f);
	AddConstantFloat(4.0f);
	if (m_code.HasAnyOf({ShaderInstructionType::VCvtOffF32I4})) AddConstantFloat(0.0625f);
	const int record_constants = m_bind != nullptr ? static_cast<int>(m_bind->push_constant_size / 16) : 0;
	for (int i = 0; i <= std::max(32, record_constants); i++)
	{
		AddConstantInt(i);
		AddConstantUint(i);
	}
	for (const auto& inst: m_code.GetInstructions())
	{
		for (int i = 0; i < inst.src_num; i++)
		{
			if (operand_is_constant(inst.src[i]))
			{
				AddConstant(inst.src[i]);
			}
		}
	}
	if (m_code.HasAnyOf({ShaderInstructionType::VRsqClampF32}))
	{
		AddConstantInt(-126);
		AddConstantInt(126);
		AddConstantInt(127);
		AddConstantUint(64);
		AddConstantUint(126);
		AddConstantUint(128);
		AddConstantUint(255);
		AddConstantUint(0x007fffff);
		AddConstantUint(0x00800000);
		AddConstantUint(0x01000000);
		AddConstantUint(0x7f7fffff);
		AddConstantUint(0x7fc00000);
		AddConstantUint(0xff7fffff);
	}
	if (m_code.HasAnyOf({ShaderInstructionType::VMulLegacyF32, ShaderInstructionType::VMacLegacyF32}))
	{
		AddConstantUint(127);
		AddConstantUint(0x00800000);
		AddConstantUint(0x00007fff);
		AddConstantUint(0xff800000);
	}
	if (m_code.HasAnyOf({ShaderInstructionType::VMacLegacyF32}))
	{
		AddConstantUint(128);
		AddConstantUint(255);
		AddConstantUint(0x00000100);
		AddConstantUint(0x00008000);
		AddConstantUint(0x00010000);
		AddConstantUint(0x00100000);
		AddConstantUint(0x003fffff);
		AddConstantUint(0x00400000);
		AddConstantUint(0x007fffff);
		AddConstantUint(0x01000000);
		AddConstantUint(0x7f800000);
		AddConstantUint(0x7fc00000);
	}
	if (m_vs_input_info != nullptr || m_ps_input_info != nullptr || m_cs_input_info != nullptr)
	{
		AddConstantInt(12);
		AddConstantInt(16);
		AddConstantInt(31);
		AddConstantInt(36);
		AddConstantInt(39);
		AddConstantInt(92);
		AddConstantInt(95);
		AddConstantInt(99);
		AddConstantInt(119);
		AddConstantUint(24);
		AddConstantUint(31);
		AddConstantUint(32);
		AddConstantUint(63);
		AddConstantUint(64);
		AddConstantUint(72);
		AddConstantUint(127);
		AddConstantUint(0x3fff);
		AddConstantUint(0xffffff);
		AddConstantUint(0xffffe000);
		AddConstantUint(0xffffffff);
		AddConstantUint(0x0000000f);
		AddConstantUint(0x000000f0);
		AddConstantUint(0x00000f00);
		AddConstantUint(0x0000f000);
		AddConstantUint(0x000f0000);
		AddConstantUint(0x00f00000);
		AddConstantUint(0x0f000000);
		AddConstantUint(0xf0000000);
	}
	if (m_cs_input_info != nullptr)
	{
		AddConstantUint(m_cs_input_info->threads_num[0]);
		AddConstantUint(m_cs_input_info->threads_num[1]);
		AddConstantUint(m_cs_input_info->threads_num[2]);
	}
}

void Spirv::FindVariables()
{
	m_variables.Clear();

	AddVariable(ShaderOperandType::Vgpr, 0, 1);
	AddVariable(ShaderOperandType::ExecLo, 0, 2);
	AddVariable(ShaderOperandType::ExecZ, 0, 1);
	AddVariable(ShaderOperandType::Scc, 0, 1);

	for (const auto& inst: m_code.GetInstructions())
	{
		AddVariable(inst.dst);
		AddVariable(inst.dst2);
		for (int i = 0; i < inst.src_num; i++)
		{
			AddVariable(inst.src[i]);
		}
	}

	if (m_vs_input_info != nullptr)
	{
		AddVariable(ShaderOperandType::Vgpr, 3, 1);
		for (int i = 0; i < m_vs_input_info->resources_num; i++)
		{
			AddVariable(ShaderOperandType::Vgpr, m_vs_input_info->resources_dst[i].register_start,
			            m_vs_input_info->resources_dst[i].registers_num);
		}
	}

	if (m_ps_input_info != nullptr)
	{
		if (m_ps_input_info->ps_pos_xy)
		{
			AddVariable(ShaderOperandType::Vgpr, 2, 1);
			AddVariable(ShaderOperandType::Vgpr, 3, 1);
		}
	}

	if (m_cs_input_info != nullptr)
	{
		AddVariable(ShaderOperandType::Vgpr, 0, 3);
		AddVariable(ShaderOperandType::Sgpr, m_cs_input_info->workgroup_register, 3);
	}

	if (m_bind != nullptr)
	{
		for (int i = 0; i < m_bind->storage_buffers.buffers_num; i++)
		{
			int storage_start = m_bind->storage_buffers.start_register[i];
			AddVariable(ShaderOperandType::Sgpr, storage_start, 4);
		}
		for (int i = 0; i < m_bind->textures2D.textures_num; i++)
		{
			int storage_start = m_bind->textures2D.desc[i].start_register;
			AddVariable(ShaderOperandType::Sgpr, storage_start, 8);
		}
		for (int i = 0; i < m_bind->samplers.samplers_num; i++)
		{
			int storage_start = m_bind->samplers.start_register[i];
			AddVariable(ShaderOperandType::Sgpr, storage_start, 8);
		}
		for (int i = 0; i < m_bind->direct_sgprs.sgprs_num; i++)
		{
			AddVariable(ShaderOperandType::Sgpr, m_bind->direct_sgprs.start_register[i], 1);
		}
	}
}

String8 SpirvGenerateSource(const ShaderCode& code, const ShaderVertexInputInfo* vs_input_info, const ShaderPixelInputInfo* ps_input_info,
                            const ShaderComputeInputInfo* cs_input_info)
{
	// Revalidate before producing any source: constructed IR, copied code or a
	// revoked contract must not bypass the parser's admission check.
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.type == ShaderInstructionType::STTraceData && !ShaderThreadTraceDataInstructionSupported(code, inst))
			EXIT("unsupported S_TTRACEDATA IR requires explicitly disabled thread trace and canonical SIMM16=0\n");
		if (inst.type == ShaderInstructionType::VCvtOffF32I4)
			EXIT_NOT_IMPLEMENTED(!ShaderCvtOffF32I4InstructionSupported(inst));
		if (inst.format == ShaderInstructionFormat::Vdata1Vaddr3StSsDmask2 ||
		    inst.format == ShaderInstructionFormat::Vdata1Vaddr3StSsDmask4)
			EXIT_NOT_IMPLEMENTED(!ShaderImageSampleSingleComponentInstructionSupported(inst));
	}
	EXIT_NOT_IMPLEMENTED(vs_input_info != nullptr && vs_input_info->rect_list && !ShaderRectListProfileSupported(code, *vs_input_info));
	if (vs_input_info != nullptr)
	{
		uint32_t produced = 0;
		EXIT_NOT_IMPLEMENTED(!ShaderVertexParameterExportsSupported(code, *vs_input_info, nullptr, nullptr, &produced));
		if (vs_input_info->export_count > 5)
			EXIT_NOT_IMPLEMENTED(!vs_input_info->parameter_exports_valid || produced != vs_input_info->parameter_export_mask ||
			                     !vs_input_info->host_parameter_interface_valid || vs_input_info->host_vertex_output_components == 0 ||
			                     uint64_t{4} * static_cast<uint32_t>(vs_input_info->export_count) + 7 > vs_input_info->host_vertex_output_components);
	}
	EXIT_NOT_IMPLEMENTED(ps_input_info != nullptr && ps_input_info->input_num > 32);
	if (ps_input_info != nullptr)
		for (const auto& inst: code.GetInstructions())
			if ((inst.type == ShaderInstructionType::VInterpP1F32 || inst.type == ShaderInstructionType::VInterpP2F32 ||
			     inst.type == ShaderInstructionType::VInterpMovF32) &&
			    (code.GetType() != ShaderType::Pixel || inst.src_num != 3 ||
			     inst.src[1].type != ShaderOperandType::IntegerInlineConstant || inst.src[2].type != ShaderOperandType::IntegerInlineConstant ||
			     inst.src[1].constant.u >= ps_input_info->input_num || inst.src[2].constant.u > 3))
				EXIT("unsupported pixel interpolator input index or channel\n");
	Spirv spirv;
	spirv.SetCode(code);
	spirv.SetVsInputInfo(vs_input_info);
	spirv.SetPsInputInfo(ps_input_info);
	spirv.SetCsInputInfo(cs_input_info);
	spirv.GenerateSource();

	return spirv.GetSource();
}

String8 SpirvGetEmbeddedVs(uint32_t id)
{
	EXIT_NOT_IMPLEMENTED(id != 0);

	return EMBEDDED_SHADER_VS_0;
}

String8 SpirvGetEmbeddedPs(uint32_t id)
{
	EXIT_NOT_IMPLEMENTED(id != 0);

	return EMBEDDED_SHADER_PS_0;
}

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED
