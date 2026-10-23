// SPDX-License-Identifier: GPL-2.0-only
#include "emulator/src/Graphics/Metal/MetalVulkanInternal.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#define REQUIRE(condition) do { if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); std::abort(); } } while (false)
#define SUCCESS(call) REQUIRE((call) == VK_SUCCESS)

int main() {
    @autoreleasepool {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_2;
        const char* surfaceExtension = VK_EXT_METAL_SURFACE_EXTENSION_NAME;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &app;
        instanceInfo.enabledExtensionCount = 1;
        instanceInfo.ppEnabledExtensionNames = &surfaceExtension;
        VkInstance instance{};
        SUCCESS(vkCreateInstance(&instanceInfo, nullptr, &instance));
        uint32_t count = 1;
        VkPhysicalDevice physical{};
        SUCCESS(vkEnumeratePhysicalDevices(instance, &count, &physical));
        REQUIRE(physical != nullptr);
        VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features12};
        vkGetPhysicalDeviceFeatures2(physical, &features);
        REQUIRE(features12.timelineSemaphore && features12.bufferDeviceAddress);
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        REQUIRE(properties.apiVersion == VK_API_VERSION_1_2);

        const float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
        VkDevice device{};
        SUCCESS(vkCreateDevice(physical, &deviceInfo, nullptr, &device));
        VkQueue queue{};
        vkGetDeviceQueue(device, 0, 0, &queue);

        // Resolve the exact API used by Vulkan-Hpp in DriverPipelineCache.
        // Missing dispatch previously crashed the renderer before its first frame.
        const auto createCache = reinterpret_cast<PFN_vkCreatePipelineCache>(vkGetDeviceProcAddr(device, "vkCreatePipelineCache"));
        const auto destroyCache = reinterpret_cast<PFN_vkDestroyPipelineCache>(vkGetDeviceProcAddr(device, "vkDestroyPipelineCache"));
        const auto getCacheData = reinterpret_cast<PFN_vkGetPipelineCacheData>(vkGetDeviceProcAddr(device, "vkGetPipelineCacheData"));
        const auto mergeCaches = reinterpret_cast<PFN_vkMergePipelineCaches>(vkGetDeviceProcAddr(device, "vkMergePipelineCaches"));
        REQUIRE(createCache && destroyCache && getCacheData && mergeCaches);
        VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        VkPipelineCache cache{};
        SUCCESS(createCache(device, &cacheInfo, nullptr, &cache));
        REQUIRE(cache != VK_NULL_HANDLE);
        size_t cacheSize{};
        SUCCESS(getCacheData(device, cache, &cacheSize, nullptr));
        REQUIRE(cacheSize == 32);
        std::array<uint8_t, 40> cacheBytes;
        cacheBytes.fill(0xA5);
        cacheSize = 31;
        REQUIRE(getCacheData(device, cache, &cacheSize, cacheBytes.data()) == VK_INCOMPLETE);
        REQUIRE(cacheSize == 0);
        for (const auto byte : cacheBytes) REQUIRE(byte == 0xA5);
        cacheSize = cacheBytes.size();
        SUCCESS(getCacheData(device, cache, &cacheSize, cacheBytes.data()));
        REQUIRE(cacheSize == 32);
        std::array<uint32_t, 4> cacheHeader{};
        std::memcpy(cacheHeader.data(), cacheBytes.data(), sizeof(cacheHeader));
        REQUIRE(cacheHeader[0] == 32 && cacheHeader[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE);
        REQUIRE(cacheHeader[2] == properties.vendorID && cacheHeader[3] == properties.deviceID);
        REQUIRE(std::memcmp(cacheBytes.data() + 16, properties.pipelineCacheUUID, VK_UUID_SIZE) == 0);
        for (size_t i = cacheSize; i < cacheBytes.size(); ++i) REQUIRE(cacheBytes[i] == 0xA5);
        cacheInfo.initialDataSize = cacheSize;
        cacheInfo.pInitialData = cacheBytes.data();
        VkPipelineCache restoredCache{};
        SUCCESS(createCache(device, &cacheInfo, nullptr, &restoredCache));
        SUCCESS(mergeCaches(device, restoredCache, 1, &cache));
        std::array<uint8_t, 32> restoredBytes{};
        SUCCESS(getCacheData(device, restoredCache, &cacheSize, restoredBytes.data()));
        REQUIRE(std::memcmp(restoredBytes.data(), cacheBytes.data(), restoredBytes.size()) == 0);
        destroyCache(device, restoredCache, nullptr);
        // A cache from an incompatible device starts empty with this device's header.
        cacheBytes[8] ^= 0xFF;
        SUCCESS(createCache(device, &cacheInfo, nullptr, &restoredCache));
        SUCCESS(getCacheData(device, restoredCache, &cacheSize, cacheBytes.data()));
        REQUIRE(std::memcmp(restoredBytes.data(), cacheBytes.data(), restoredBytes.size()) == 0);
        destroyCache(device, restoredCache, nullptr);
        VkDevice otherDevice{};
        SUCCESS(vkCreateDevice(physical, &deviceInfo, nullptr, &otherDevice));
        REQUIRE(getCacheData(otherDevice, cache, &cacheSize, nullptr) == VK_ERROR_INITIALIZATION_FAILED);
        vkDestroyDevice(otherDevice, nullptr);
        destroyCache(device, cache, nullptr);
        destroyCache(device, VK_NULL_HANDLE, nullptr);

        CAMetalLayer* layer = [CAMetalLayer layer];
        VkMetalSurfaceCreateInfoEXT surfaceInfo{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
        surfaceInfo.pLayer = layer;
        VkSurfaceKHR surface{};
        SUCCESS(vkCreateMetalSurfaceEXT(instance, &surfaceInfo, nullptr, &surface));
        vkDestroySurfaceKHR(instance, surface, nullptr);

        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        VkSemaphore semaphore{};
        SUCCESS(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore));
        uint64_t value{};
        SUCCESS(vkGetSemaphoreCounterValue(device, semaphore, &value));
        REQUIRE(value == 0);
        uint64_t waitValue = 1;
        VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        waitInfo.semaphoreCount = 1; waitInfo.pSemaphores = &semaphore; waitInfo.pValues = &waitValue;
        REQUIRE(vkWaitSemaphores(device, &waitInfo, 0) == VK_TIMEOUT);

        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = 4096;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VkBuffer buffer{};
        SUCCESS(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer));
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size + 256; allocation.memoryTypeIndex = 1;
        VkDeviceMemory memory{};
        SUCCESS(vkAllocateMemory(device, &allocation, nullptr, &memory));
        SUCCESS(vkBindBufferMemory(device, buffer, memory, 256));
        VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        addressInfo.buffer = buffer;
        const auto address = vkGetBufferDeviceAddress(device, &addressInfo);
        REQUIRE(address != 0);
        {
            std::lock_guard lock(device->mutex);
            REQUIRE(Lumen::MetalVulkan::QueryBufferAddressFromShader(device, buffer) == address);
        }

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkCommandPool pool{};
        SUCCESS(vkCreateCommandPool(device, &poolInfo, nullptr, &pool));
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = pool; commandInfo.commandBufferCount = 1;
        VkCommandBuffer command{};
        SUCCESS(vkAllocateCommandBuffers(device, &commandInfo, &command));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        SUCCESS(vkBeginCommandBuffer(command, &begin));
        vkCmdFillBuffer(command, buffer, 0, 4096, 0x1234ABCD);
        VkDependencyInfo barrier{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        barrier.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
        vkCmdPipelineBarrier2(command, &barrier);
        VkMemoryBarrier legacyBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        legacyBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        legacyBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             VK_DEPENDENCY_BY_REGION_BIT, 1, &legacyBarrier, 0, nullptr, 0, nullptr);
        SUCCESS(vkEndCommandBuffer(command));

        // Future timeline wait must not block vkQueueSubmit. The host signal is
        // deliberately issued only after submission has returned.
        uint64_t signalValue = 2;
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.waitSemaphoreValueCount = 1; timeline.pWaitSemaphoreValues = &waitValue;
        timeline.signalSemaphoreValueCount = 1; timeline.pSignalSemaphoreValues = &signalValue;
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline};
        submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &semaphore; submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &semaphore;
        SUCCESS(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        signal.semaphore = semaphore; signal.value = 1;
        SUCCESS(vkSignalSemaphore(device, &signal));
        waitValue = 2;
        SUCCESS(vkWaitSemaphores(device, &waitInfo, 5'000'000'000));
        SUCCESS(vkQueueWaitIdle(queue));
        void* mapped{};
        SUCCESS(vkMapMemory(device, memory, 256, 4096, 0, &mapped));
        for (size_t i = 0; i < 1024; ++i) REQUIRE(static_cast<uint32_t*>(mapped)[i] == 0x1234ABCD);
        vkUnmapMemory(device, memory);

        // Dynamic rendering writes a real attachment; verify every pixel through
        // the same GPU-to-host copy contract consumed by the new renderer.
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D; imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {4, 4, 1}; imageInfo.mipLevels = imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT; imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkImage image{};
        SUCCESS(vkCreateImage(device, &imageInfo, nullptr, &image));
        vkGetImageMemoryRequirements(device, image, &requirements);
        allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = 0;
        VkDeviceMemory imageMemory{};
        SUCCESS(vkAllocateMemory(device, &allocation, nullptr, &imageMemory));
        SUCCESS(vkBindImageMemory(device, image, imageMemory, 0));
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image; viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D; viewInfo.format = imageInfo.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view{};
        SUCCESS(vkCreateImageView(device, &viewInfo, nullptr, &view));
        SUCCESS(vkResetCommandBuffer(command, 0));
        SUCCESS(vkBeginCommandBuffer(command, &begin));
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = view; color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color.float32[0] = 1; color.clearValue.color.float32[3] = 1;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = {4, 4}; rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1; rendering.pColorAttachments = &color;
        vkCmdBeginRendering(command, &rendering);
        vkCmdEndRendering(command);
        vkCmdPipelineBarrier2(command, &barrier);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {4, 4, 1};
        vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        SUCCESS(vkEndCommandBuffer(command));
        VkSubmitInfo renderSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        renderSubmit.commandBufferCount = 1; renderSubmit.pCommandBuffers = &command;
        SUCCESS(vkQueueSubmit(queue, 1, &renderSubmit, VK_NULL_HANDLE));
        SUCCESS(vkQueueWaitIdle(queue));
        SUCCESS(vkMapMemory(device, memory, 256, 4096, 0, &mapped));
        for (size_t i = 0; i < 16; ++i) REQUIRE(static_cast<uint32_t*>(mapped)[i] == 0xFF0000FF);
        vkUnmapMemory(device, memory);
        REQUIRE(vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR") == vkGetDeviceProcAddr(device, "vkCmdBeginRendering"));
        REQUIRE(vkGetDeviceProcAddr(device, "vkWaitSemaphoresKHR") == vkGetDeviceProcAddr(device, "vkWaitSemaphores"));
        vkDestroyImageView(device, view, nullptr);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, imageMemory, nullptr);

        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, memory, nullptr);
        vkDestroySemaphore(device, semaphore, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        std::puts("native Metal pipeline cache lifecycle, timeline/future-wait, BDA/macOS12 address path, fill, sync2, dynamic rendering pixels and extension aliases: PASS");
    }
}
