#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Utils.h"
#include "Lumen/Core/Database.h"
#include "Lumen/Math/Rand.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using namespace Lumen::Libs::Graphics;
constexpr uint32_t Width = 32, Height = 8;
constexpr uint64_t ImageBytes = Width * Height * 4;

struct Probe
{
	GraphicContext ctx {};
	VkQueue queue = nullptr;
	uint32_t queue_family = UINT32_MAX;
	VkCommandPool pool = nullptr;
	std::array<VkCommandBuffer, 2> commands {};
	std::array<VkFence, 2> fences {};
	std::array<VkImage, 2> images {};
	std::array<VkDeviceMemory, 2> image_memory {};
	std::array<UploadRecordingBatch*, 2> owned_batches {};
	VulkanBuffer readback;
	void* mapped = nullptr;
	~Probe()
	{
		if (ctx.device != nullptr)
		{
			vkDeviceWaitIdle(ctx.device);
			for (auto* batch: owned_batches) if (batch != nullptr) UtilReleaseUploadRecordingBatch(&ctx, batch);
			UtilResetUploadStaging(&ctx);
			if (mapped != nullptr) vkUnmapMemory(ctx.device, readback.memory.memory);
			if (readback.buffer != nullptr) vkDestroyBuffer(ctx.device, readback.buffer, nullptr);
			if (readback.memory.memory != nullptr) vkFreeMemory(ctx.device, readback.memory.memory, nullptr);
			for (size_t i = 0; i < images.size(); ++i)
			{
				if (images[i] != nullptr) vkDestroyImage(ctx.device, images[i], nullptr);
				if (image_memory[i] != nullptr) vkFreeMemory(ctx.device, image_memory[i], nullptr);
				if (fences[i] != nullptr) vkDestroyFence(ctx.device, fences[i], nullptr);
			}
			if (pool != nullptr) vkDestroyCommandPool(ctx.device, pool, nullptr);
			vkDestroyDevice(ctx.device, nullptr);
		}
		if (ctx.instance != nullptr) vkDestroyInstance(ctx.instance, nullptr);
	}
	bool Allocate(VkMemoryRequirements requirements, VkMemoryPropertyFlags flags, VkDeviceMemory* memory)
	{
		VkPhysicalDeviceMemoryProperties properties {};
		vkGetPhysicalDeviceMemoryProperties(ctx.physical_device, &properties);
		for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
		{
			if ((requirements.memoryTypeBits & (uint32_t{1} << i)) == 0 ||
			    (properties.memoryTypes[i].propertyFlags & flags) != flags) continue;
			VkMemoryAllocateInfo info {};
			info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			info.allocationSize = requirements.size;
			info.memoryTypeIndex = i;
			if (vkAllocateMemory(ctx.device, &info, nullptr, memory) == VK_SUCCESS) return true;
		}
		return false;
	}
	bool Init()
	{
		VkApplicationInfo application {};
		application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		application.pApplicationName = "Lumen upload staging probe";
		application.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo instance {};
		instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		instance.pApplicationInfo = &application;
		if (vkCreateInstance(&instance, nullptr, &ctx.instance) != VK_SUCCESS) return false;
		uint32_t count = 0;
		if (vkEnumeratePhysicalDevices(ctx.instance, &count, nullptr) != VK_SUCCESS || count == 0) return false;
		std::vector<VkPhysicalDevice> devices(count);
		if (vkEnumeratePhysicalDevices(ctx.instance, &count, devices.data()) != VK_SUCCESS) return false;
		uint32_t family = UINT32_MAX;
		for (auto device: devices)
		{
			uint32_t families = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(device, &families, nullptr);
			std::vector<VkQueueFamilyProperties> properties(families);
			vkGetPhysicalDeviceQueueFamilyProperties(device, &families, properties.data());
			for (uint32_t i = 0; i < families; ++i)
				if (properties[i].queueCount != 0 && (properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
				{ ctx.physical_device = device; family = i; break; }
			if (family != UINT32_MAX) break;
		}
		if (family == UINT32_MAX) return false;
		queue_family = family;
		const float priority = 1;
		VkDeviceQueueCreateInfo queue_info {};
		queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queue_info.queueFamilyIndex = family; queue_info.queueCount = 1; queue_info.pQueuePriorities = &priority;
		VkDeviceCreateInfo device {};
		device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		device.queueCreateInfoCount = 1; device.pQueueCreateInfos = &queue_info;
		if (vkCreateDevice(ctx.physical_device, &device, nullptr, &ctx.device) != VK_SUCCESS) return false;
		vkGetDeviceQueue(ctx.device, family, 0, &queue);
		VkCommandPoolCreateInfo pool_info {};
		pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pool_info.queueFamilyIndex = family;
		if (vkCreateCommandPool(ctx.device, &pool_info, nullptr, &pool) != VK_SUCCESS) return false;
		VkCommandBufferAllocateInfo commands_info {};
		commands_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		commands_info.commandPool = pool; commands_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commands_info.commandBufferCount = commands.size();
		if (vkAllocateCommandBuffers(ctx.device, &commands_info, commands.data()) != VK_SUCCESS) return false;
		VkFenceCreateInfo fence {};
		fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		for (size_t i = 0; i < images.size(); ++i)
		{
			if (vkCreateFence(ctx.device, &fence, nullptr, &fences[i]) != VK_SUCCESS) return false;
			VkImageCreateInfo image {};
			image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			image.imageType = VK_IMAGE_TYPE_2D; image.format = VK_FORMAT_R8G8B8A8_UNORM;
			image.extent = {Width, Height, 1}; image.mipLevels = image.arrayLayers = 1;
			image.samples = VK_SAMPLE_COUNT_1_BIT; image.tiling = VK_IMAGE_TILING_OPTIMAL;
			image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			if (vkCreateImage(ctx.device, &image, nullptr, &images[i]) != VK_SUCCESS) return false;
			VkMemoryRequirements requirements {};
			vkGetImageMemoryRequirements(ctx.device, images[i], &requirements);
			if (!Allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &image_memory[i]) ||
			    vkBindImageMemory(ctx.device, images[i], image_memory[i], 0) != VK_SUCCESS) return false;
		}
		VkBufferCreateInfo buffer {};
		buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buffer.size = ImageBytes * 2 + 512; buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		if (vkCreateBuffer(ctx.device, &buffer, nullptr, &readback.buffer) != VK_SUCCESS) return false;
		vkGetBufferMemoryRequirements(ctx.device, readback.buffer, &readback.memory.requirements);
		if (!Allocate(readback.memory.requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		              &readback.memory.memory) ||
		    vkBindBufferMemory(ctx.device, readback.buffer, readback.memory.memory, 0) != VK_SUCCESS ||
		    vkMapMemory(ctx.device, readback.memory.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return false;
		return mapped != nullptr;
	}
};
} // namespace

int GraphicsUploadStagingVulkanProbe()
{
	using namespace Lumen::Libs::Graphics;
	Probe probe;
	if (!probe.Init()) { std::fprintf(stderr, "Upload staging probe initialization failed\n"); return 1; }
	Lumen::Math::Rand::Init();
	Lumen::Core::Database::Init();
	GpuMemoryInit();
	std::array<VkBuffer, 2> previous {};
	for (uint32_t wave = 0; wave < 2; ++wave)
	{
		std::memset(probe.mapped, 0xa5, ImageBytes * 2 + 512);
		auto& batches = probe.owned_batches;
		std::array<std::vector<uint8_t>, 2> expected;
		std::array<VkBuffer, 2> buffers {};
		for (uint32_t i = 0; i < 2; ++i)
		{
			batches[i] = UtilCreateUploadRecordingBatch(&probe.ctx, probe.queue_family);
			std::array<uint8_t, 13> padding {};
			uint64_t ignored = 0;
			if (!UtilUploadStagingCopyForTest(&probe.ctx, batches[i], padding.data(), padding.size(), 256, &buffers[i], &ignored)) return 1;
			if (i == 1 && buffers[0] == buffers[1]) return 1;
			if (wave != 0 && buffers[i] != previous[i]) return 1;
			std::array<std::vector<uint8_t>, 2> halves;
			expected[i].resize(ImageBytes);
			std::array<VkBufferImageCopy, 2> copies {};
			for (uint32_t half = 0; half < 2; ++half)
			{
				halves[half].resize(ImageBytes / 2);
				for (uint32_t y = 0; y < Height; ++y)
					for (uint32_t x = 0; x < Width / 2; ++x)
						for (uint32_t c = 0; c < 4; ++c)
						{
							const uint8_t value = uint8_t(wave * 73 + i * 41 + half * 29 + y * 13 + x * 3 + c);
							halves[half][(y * Width / 2 + x) * 4 + c] = value;
							expected[i][(y * Width + half * Width / 2 + x) * 4 + c] = value;
						}
				VkBuffer source = nullptr;
				uint64_t offset = 0;
				if (!UtilUploadStagingCopyForTest(&probe.ctx, batches[i], halves[half].data(), halves[half].size(), 768, &source, &offset) ||
				    source != buffers[i] || offset == 0 || offset % 768 != 0) return 1;
				copies[half].bufferOffset = offset;
				copies[half].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				copies[half].imageSubresource.layerCount = 1;
				copies[half].imageOffset.x = half * Width / 2;
				copies[half].imageExtent = {Width / 2, Height, 1};
			}
			VkCommandBufferBeginInfo begin {};
			begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			auto command = probe.commands[i];
			if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) return 1;
			VkImageMemoryBarrier image {};
			image.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			image.srcAccessMask = wave == 0 ? 0 : VK_ACCESS_TRANSFER_READ_BIT;
			image.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			image.oldLayout = wave == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			image.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			image.srcQueueFamilyIndex = image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			image.image = probe.images[i];
			image.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			vkCmdPipelineBarrier(command, wave == 0 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &image);
			vkCmdCopyBufferToImage(command, buffers[i], probe.images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copies.size(), copies.data());
			image.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; image.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			image.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; image.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &image);
			VkBufferImageCopy output {};
			output.bufferOffset = 256 + i * ImageBytes;
			output.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			output.imageExtent = {Width, Height, 1};
			vkCmdCopyImageToBuffer(command, probe.images[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, probe.readback.buffer, 1, &output);
			VkMemoryBarrier host {};
			host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
			vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
			if (vkEndCommandBuffer(command) != VK_SUCCESS) return 1;
			VkSubmitInfo submit {};
			submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
			if (vkQueueSubmit(probe.queue, 1, &submit, probe.fences[i]) != VK_SUCCESS) return 1;
		}
		if (UtilResetUploadStaging(&probe.ctx)) return 1; // Both batches retain their GPU readers.
		if (vkWaitForFences(probe.ctx.device, probe.fences.size(), probe.fences.data(), VK_TRUE, 5'000'000'000ull) != VK_SUCCESS) return 1;
		for (uint32_t i = 0; i < 2; ++i)
		{
			if (std::memcmp(static_cast<uint8_t*>(probe.mapped) + 256 + i * ImageBytes, expected[i].data(), ImageBytes) != 0) return 1;
			UtilReleaseUploadRecordingBatch(&probe.ctx, batches[i]);
			batches[i] = nullptr;
		}
		for (uint32_t i = 0; i < 256; ++i)
			if (static_cast<uint8_t*>(probe.mapped)[i] != 0xa5 ||
			    static_cast<uint8_t*>(probe.mapped)[256 + ImageBytes * 2 + i] != 0xa5) return 1;
		previous = buffers;
		if (vkResetFences(probe.ctx.device, probe.fences.size(), probe.fences.data()) != VK_SUCCESS ||
		    vkResetCommandPool(probe.ctx.device, probe.pool, 0) != VK_SUCCESS) return 1;
	}
	// Model a distinct queue family without submitting unsupported queues on
	// single-family devices. Even an idle EXCLUSIVE slab must not cross owners.
	const uint32_t other_family = probe.queue_family == 0 ? 1 : 0;
	probe.owned_batches[0] = UtilCreateUploadRecordingBatch(&probe.ctx, other_family);
	std::array<uint8_t, 16> foreign_bytes {};
	VkBuffer foreign = nullptr;
	uint64_t foreign_offset = 0;
	if (!UtilUploadStagingCopyForTest(&probe.ctx, probe.owned_batches[0], foreign_bytes.data(), foreign_bytes.size(),
	    256, &foreign, &foreign_offset) || foreign == previous[0] || foreign == previous[1]) return 1;
	UtilReleaseUploadRecordingBatch(&probe.ctx, probe.owned_batches[0]);
	probe.owned_batches[0] = nullptr;
	if (!UtilResetUploadStaging(&probe.ctx)) return 1;
	std::puts("GraphicsUploadStagingVulkanProbe: PASS pixels=byte-exact offsets=nonzero two-submissions-in-flight persistent-map=real reuse=verified guards=clean family-isolation=metadata-negative");
	return 0;
}
