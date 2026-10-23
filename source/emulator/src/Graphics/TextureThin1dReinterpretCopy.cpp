#include "Emulator/Graphics/TextureThin1dReinterpretCopy.h"

#include "Lumen/Core/DbgAssert.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/TextureThin1dReinterpretShader.h"

#include "spirv-tools/libspirv.hpp"

#include <array>
#include <atomic>
#include <vector>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

namespace {

using TextureThin1dReinterpretShader::Direction;
using TextureThin1dReinterpretShader::PushConstants;

constexpr uint32_t TEXTURE_THIN1D_REINTERPRET_MAX_IN_FLIGHT = 16;
std::atomic_uint32_t g_texture_thin1d_reinterpret_in_flight {0};

constexpr Direction SelectDirection(VkFormat source, VkFormat destination)
{
	if (source == VK_FORMAT_R8_UNORM && destination == VK_FORMAT_R8G8B8A8_UNORM)
	{
		return Direction::R8ToRgba8;
	}
	if (source == VK_FORMAT_R8G8B8A8_UNORM && destination == VK_FORMAT_R8_UNORM)
	{
		return Direction::Rgba8ToR8;
	}
	if (source == VK_FORMAT_R8G8B8A8_UNORM && destination == VK_FORMAT_R8G8B8A8_UNORM)
	{
		return Direction::Rgba8ToRgba8;
	}
	return Direction::Unsupported;
}

constexpr uint32_t BytesPerElement(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_R8_UNORM: return 1;
		case VK_FORMAT_R8G8B8A8_UNORM: return 4;
		default: break;
	}
	return 0;
}

bool ExactByteSize(uint32_t pitch, uint32_t height, uint32_t bytes_per_element, uint64_t* result)
{
	if (result == nullptr || pitch == 0 || height == 0 || bytes_per_element == 0 ||
	    height > UINT64_MAX / pitch)
	{
		return false;
	}
	const uint64_t elements = static_cast<uint64_t>(pitch) * height;
	if (elements > UINT64_MAX / bytes_per_element)
	{
		return false;
	}
	*result = elements * bytes_per_element;
	return *result != 0;
}

bool StorageImageFormatAllowed(GraphicContext* ctx, VkFormat format)
{
	VkFormatProperties properties {};
	vkGetPhysicalDeviceFormatProperties(ctx->physical_device, format, &properties);
	return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}

struct RetiredResources
{
	VkDevice              device                = VK_NULL_HANDLE;
	VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
	VkDescriptorPool      descriptor_pool       = VK_NULL_HANDLE;
	VkPipelineLayout      pipeline_layout       = VK_NULL_HANDLE;
	VkShaderModule        shader_module         = VK_NULL_HANDLE;
	VkPipeline            pipeline              = VK_NULL_HANDLE;
};

void DestroyResources(GraphicContext* /*ctx*/, void* object)
{
	auto* resources = static_cast<RetiredResources*>(object);
	EXIT_IF(resources == nullptr || resources->device == VK_NULL_HANDLE);
	vkDestroyPipeline(resources->device, resources->pipeline, nullptr);
	vkDestroyShaderModule(resources->device, resources->shader_module, nullptr);
	vkDestroyPipelineLayout(resources->device, resources->pipeline_layout, nullptr);
	vkDestroyDescriptorPool(resources->device, resources->descriptor_pool, nullptr);
	vkDestroyDescriptorSetLayout(resources->device, resources->descriptor_set_layout, nullptr);
	delete resources;
	g_texture_thin1d_reinterpret_in_flight.fetch_sub(1, std::memory_order_relaxed);
}

void DestroyUnrecordedResources(RetiredResources* resources)
{
	if (resources == nullptr)
	{
		return;
	}
	if (resources->pipeline != VK_NULL_HANDLE) vkDestroyPipeline(resources->device, resources->pipeline, nullptr);
	if (resources->shader_module != VK_NULL_HANDLE) vkDestroyShaderModule(resources->device, resources->shader_module, nullptr);
	if (resources->pipeline_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(resources->device, resources->pipeline_layout, nullptr);
	if (resources->descriptor_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(resources->device, resources->descriptor_pool, nullptr);
	if (resources->descriptor_set_layout != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(resources->device, resources->descriptor_set_layout, nullptr);
	}
	delete resources;
	g_texture_thin1d_reinterpret_in_flight.fetch_sub(1, std::memory_order_relaxed);
}

VkImageSubresourceRange ColorRange()
{
	VkImageSubresourceRange range {};
	range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	range.baseMipLevel   = 0;
	range.levelCount     = 1;
	range.baseArrayLayer = 0;
	range.layerCount     = 1;
	return range;
}

} // namespace

bool TextureThin1dReinterpretCopyPreflight(GraphicContext* ctx, CommandBuffer* buffer,
	                                       const TextureThin1dReinterpretCopyInfo& info)
{
	const Direction direction = info.source == nullptr || info.destination == nullptr
	                                ? Direction::Unsupported
	                                : SelectDirection(info.source->format, info.destination->format);
	if (ctx == nullptr || ctx->device == VK_NULL_HANDLE || ctx->physical_device == VK_NULL_HANDLE || buffer == nullptr ||
	    !buffer->CanRecordTextureTransfer() || info.source == nullptr || info.destination == nullptr ||
	    info.source == info.destination || info.source_pitch == 0 || info.destination_pitch == 0 ||
	    info.source_size == 0 || info.destination_size == 0 ||
	    info.source->type != VulkanImageType::StorageTexture ||
	    info.destination->type != VulkanImageType::StorageTexture || info.source->image == VK_NULL_HANDLE ||
	    info.destination->image == VK_NULL_HANDLE || info.source->image == info.destination->image ||
	    info.source->image_view[VulkanImage::VIEW_DEFAULT] == VK_NULL_HANDLE ||
	    info.destination->image_view[VulkanImage::VIEW_DEFAULT] == VK_NULL_HANDLE ||
	    info.source->image_view[VulkanImage::VIEW_DEFAULT] ==
	        info.destination->image_view[VulkanImage::VIEW_DEFAULT] ||
	    info.source->memory.memory == VK_NULL_HANDLE || info.destination->memory.memory == VK_NULL_HANDLE ||
	    info.source->memory.unique_id == 0 || info.destination->memory.unique_id == 0 ||
	    info.source->memory.unique_id == info.destination->memory.unique_id ||
	    info.source->memory.requirements.size == 0 || info.destination->memory.requirements.size == 0 ||
	    direction == Direction::Unsupported ||
	    info.source->layout != VK_IMAGE_LAYOUT_GENERAL || info.destination->layout != VK_IMAGE_LAYOUT_UNDEFINED ||
	    (info.source->usage & (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) !=
	        (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
	    (info.destination->usage & (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) !=
	        (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
	    info.source->mip_levels != 1 || info.destination->mip_levels != 1 ||
	    info.source->extent.width != info.source_pitch || info.source->extent.height == 0 ||
	    info.destination->extent.width == 0 || info.destination->extent.height == 0 ||
	    (info.source->extent.width & 7u) != 0 || (info.source->extent.height & 7u) != 0)
	{
		return false;
	}

	const auto* source      = static_cast<const StorageTextureVulkanImage*>(info.source);
	const auto* destination = static_cast<const StorageTextureVulkanImage*>(info.destination);
	if (source->physical_extent.width != source->extent.width ||
	    source->physical_extent.height != source->extent.height || source->descriptor_base_level != 0 ||
	    source->descriptor_levels != 1 || destination->physical_extent.width != destination->extent.width ||
	    destination->physical_extent.height != destination->extent.height || destination->descriptor_base_level != 0 ||
	    destination->descriptor_levels != 1)
	{
		return false;
	}

	uint64_t expected_source      = 0;
	uint64_t expected_destination = 0;
	const bool strict_destination =
	    !info.destination_pow2_pad && info.destination->extent.width == info.destination_pitch &&
	    (info.destination->extent.width & 7u) == 0 && (info.destination->extent.height & 7u) == 0 &&
	    ExactByteSize(info.destination_pitch, info.destination->extent.height,
	                  BytesPerElement(info.destination->format), &expected_destination);
	const bool rgba8_destination =
	    (direction == Direction::R8ToRgba8 || direction == Direction::Rgba8ToRgba8) &&
	    TextureThin1dSingleLevelRgba8DestinationSize(
	        info.destination->extent.width, info.destination->extent.height, info.destination_pitch,
	        info.destination_pow2_pad, &expected_destination);
	if (!ExactByteSize(info.source_pitch, info.source->extent.height, BytesPerElement(info.source->format),
	                   &expected_source) || (!strict_destination && !rgba8_destination) ||
	    info.source_size != expected_source || info.destination_size != expected_destination ||
	    info.source_size > UINT32_MAX || info.destination_size > UINT32_MAX ||
	    info.destination_size >= info.source_size ||
	    g_texture_thin1d_reinterpret_in_flight.load(std::memory_order_relaxed) >=
	        TEXTURE_THIN1D_REINTERPRET_MAX_IN_FLIGHT)
	{
		return false;
	}

	VkPhysicalDeviceFeatures features {};
	vkGetPhysicalDeviceFeatures(ctx->physical_device, &features);
	if (features.shaderStorageImageExtendedFormats != VK_TRUE ||
	    !StorageImageFormatAllowed(ctx, info.source->format) ||
	    !StorageImageFormatAllowed(ctx, info.destination->format))
	{
		return false;
	}

	const int queue = buffer->GetQueue();
	if (queue < 0 || queue >= GraphicContext::QUEUES_NUM)
	{
		return false;
	}
	if (ctx->queues[queue].mutex == nullptr || ctx->queues[queue].vk_queue == VK_NULL_HANDLE)
	{
		return false;
	}
	const uint32_t family = ctx->queues[queue].family;
	uint32_t family_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &family_count, nullptr);
	if (family >= family_count)
	{
		return false;
	}
	std::vector<VkQueueFamilyProperties> families(family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &family_count, families.data());
	return family < family_count && (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
}

bool TextureThin1dReinterpretCopyRecord(GraphicContext* ctx, CommandBuffer* buffer,
	                                    const TextureThin1dReinterpretCopyInfo& info)
{
	if (!TextureThin1dReinterpretCopyPreflight(ctx, buffer, info))
	{
		return false;
	}
	const Direction direction = SelectDirection(info.source->format, info.destination->format);
	const auto      assembly  = TextureThin1dReinterpretShader::GetAssembly(direction);
	if (assembly.data == nullptr || assembly.size == 0)
	{
		return false;
	}

	const uint32_t previous = g_texture_thin1d_reinterpret_in_flight.fetch_add(1, std::memory_order_relaxed);
	if (previous >= TEXTURE_THIN1D_REINTERPRET_MAX_IN_FLIGHT)
	{
		g_texture_thin1d_reinterpret_in_flight.fetch_sub(1, std::memory_order_relaxed);
		return false;
	}

	auto* resources = new RetiredResources;
	resources->device = ctx->device;
	VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
	auto failed = [&]()
	{
		DestroyUnrecordedResources(resources);
		return false;
	};

	std::array<VkDescriptorSetLayoutBinding, 2> bindings {};
	for (uint32_t index = 0; index < bindings.size(); ++index)
	{
		bindings[index].binding         = index;
		bindings[index].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		bindings[index].descriptorCount = 1;
		bindings[index].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo descriptor_layout_info {};
	descriptor_layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	descriptor_layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
	descriptor_layout_info.pBindings    = bindings.data();
	if (vkCreateDescriptorSetLayout(ctx->device, &descriptor_layout_info, nullptr,
	                                &resources->descriptor_set_layout) != VK_SUCCESS)
	{
		return failed();
	}

	VkPushConstantRange push_range {};
	push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	push_range.size       = sizeof(PushConstants);
	VkPipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &resources->descriptor_set_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_range;
	if (vkCreatePipelineLayout(ctx->device, &pipeline_layout_info, nullptr, &resources->pipeline_layout) != VK_SUCCESS)
	{
		return failed();
	}

	VkDescriptorPoolSize pool_size {};
	pool_size.type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	pool_size.descriptorCount = 2;
	VkDescriptorPoolCreateInfo pool_info {};
	pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets       = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes    = &pool_size;
	if (vkCreateDescriptorPool(ctx->device, &pool_info, nullptr, &resources->descriptor_pool) != VK_SUCCESS)
	{
		return failed();
	}
	VkDescriptorSetAllocateInfo set_info {};
	set_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	set_info.descriptorPool     = resources->descriptor_pool;
	set_info.descriptorSetCount = 1;
	set_info.pSetLayouts        = &resources->descriptor_set_layout;
	if (vkAllocateDescriptorSets(ctx->device, &set_info, &descriptor_set) != VK_SUCCESS)
	{
		return failed();
	}

	std::array<VkDescriptorImageInfo, 2> images {};
	images[0].imageView   = info.source->image_view[VulkanImage::VIEW_DEFAULT];
	images[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	images[1].imageView   = info.destination->image_view[VulkanImage::VIEW_DEFAULT];
	images[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	std::array<VkWriteDescriptorSet, 2> writes {};
	for (uint32_t index = 0; index < writes.size(); ++index)
	{
		writes[index].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[index].dstSet          = descriptor_set;
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[index].pImageInfo      = &images[index];
	}
	vkUpdateDescriptorSets(ctx->device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

	spvtools::SpirvTools assembler(SPV_ENV_VULKAN_1_1);
	std::vector<uint32_t> binary;
	if (!assembler.Assemble(assembly.data, assembly.size, &binary) || !assembler.Validate(binary))
	{
		return failed();
	}
	VkShaderModuleCreateInfo shader_info {};
	shader_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shader_info.codeSize = binary.size() * sizeof(uint32_t);
	shader_info.pCode    = binary.data();
	if (vkCreateShaderModule(ctx->device, &shader_info, nullptr, &resources->shader_module) != VK_SUCCESS)
	{
		return failed();
	}
	VkPipelineShaderStageCreateInfo stage_info {};
	stage_info.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage_info.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	stage_info.module = resources->shader_module;
	stage_info.pName  = "main";
	VkComputePipelineCreateInfo pipeline_info {};
	pipeline_info.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipeline_info.stage  = stage_info;
	pipeline_info.layout = resources->pipeline_layout;
	if (vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
	                             &resources->pipeline) != VK_SUCCESS)
	{
		return failed();
	}

	auto* command = buffer->GetBufferOutsideRenderPass();
	std::array<VkImageMemoryBarrier, 2> before {};
	for (auto& barrier: before)
	{
		barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.subresourceRange     = ColorRange();
	}
	before[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	before[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	before[0].oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
	before[0].image         = info.source->image;
	before[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	before[1].oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
	before[1].image         = info.destination->image;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
	                     nullptr, 0, nullptr, static_cast<uint32_t>(before.size()), before.data());

	const PushConstants push {
	    info.source_pitch / 8u,
	    info.destination_pitch / 8u,
	    info.source->extent.width,
	    info.source->extent.height,
	    info.destination->extent.width,
	    info.destination->extent.height,
	};
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, resources->pipeline);
	vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, resources->pipeline_layout, 0, 1,
	                        &descriptor_set, 0, nullptr);
	vkCmdPushConstants(command, resources->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	vkCmdDispatch(command, (info.destination->extent.width + 7u) / 8u,
	              (info.destination->extent.height + 7u) / 8u, 1);

	std::array<VkImageMemoryBarrier, 2> after {};
	for (auto& barrier: after)
	{
		barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
		barrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
		barrier.dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.subresourceRange     = ColorRange();
	}
	after[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	after[0].image         = info.source->image;
	after[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	after[1].image         = info.destination->image;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
	                     nullptr, 0, nullptr, static_cast<uint32_t>(after.size()), after.data());
	info.destination->layout = VK_IMAGE_LAYOUT_GENERAL;
	buffer->Retire(resources, DestroyResources);
	return true;
}

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED
