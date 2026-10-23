// SPDX-License-Identifier: GPL-2.0-only
#include "emulator/src/Graphics/Metal/MetalVulkanInternal.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define REQUIRE(condition) do { if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); std::abort(); } } while (false)
#define SUCCESS(call) REQUIRE((call) == VK_SUCCESS)

static void CheckDelayedEventCompletion(VkDevice device, VkQueue queue, VkCommandBuffer command,
                                       const VkDependencyInfo& dependency) {
    struct CallbackGate {
        std::mutex mutex;
        std::condition_variable condition;
        bool entered = false;
        bool release = false;
    };
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
    VkSemaphore timeline{};
    SUCCESS(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &timeline));
    uint64_t tick = 0;
    for (bool sync2 : {false, true}) {
        for (bool destroyBeforeCallback : {false, true}) {
            auto gate = std::make_shared<CallbackGate>();
            VkEventCreateInfo eventInfo{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
            VkEvent event{};
            SUCCESS(vkCreateEvent(device, &eventInfo, nullptr, &event));
            SUCCESS(vkResetCommandBuffer(command, 0));
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            SUCCESS(vkBeginCommandBuffer(command, &begin));
            // Delay CPU notification while the real GPU timeline is free to
            // advance. This models a descheduled Metal completion callback.
            command->operations.push_back({"delay-event-notification", [gate](auto& context) {
                [context.commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>) {
                    std::unique_lock lock(gate->mutex);
                    gate->entered = true;
                    gate->condition.notify_all();
                    REQUIRE(gate->condition.wait_for(lock, std::chrono::seconds(5),
                                                     [&] { return gate->release; }));
                }];
                return VK_SUCCESS;
            }});
            if (sync2) vkCmdSetEvent2(command, event, &dependency);
            else vkCmdSetEvent(command, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            SUCCESS(vkEndCommandBuffer(command));
            // Reset after recording is valid; generation is captured at submit.
            SUCCESS(vkResetEvent(device, event));
            ++tick;
            VkTimelineSemaphoreSubmitInfo timelineInfo{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            timelineInfo.signalSemaphoreValueCount = 1;
            timelineInfo.pSignalSemaphoreValues = &tick;
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timelineInfo};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &timeline;
            SUCCESS(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
            {
                std::unique_lock lock(gate->mutex);
                REQUIRE(gate->condition.wait_for(lock, std::chrono::seconds(5),
                                                 [&] { return gate->entered; }));
            }
            uint64_t completed{};
            SUCCESS(vkGetSemaphoreCounterValue(device, timeline, &completed));
            REQUIRE(completed == tick);
            REQUIRE(vkGetEventStatus(device, event) == VK_EVENT_RESET);
            if (destroyBeforeCallback) vkDestroyEvent(device, event, nullptr);
            else SUCCESS(vkResetEvent(device, event));
            {
                std::lock_guard lock(gate->mutex);
                gate->release = true;
                gate->condition.notify_all();
            }
            SUCCESS(vkQueueWaitIdle(queue));
            if (!destroyBeforeCallback) {
                // An old callback must not republish SET after the reset.
                REQUIRE(vkGetEventStatus(device, event) == VK_EVENT_RESET);
                // Replaying the same recorded command uses the new generation.
                ++tick;
                SUCCESS(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
                SUCCESS(vkQueueWaitIdle(queue));
                REQUIRE(vkGetEventStatus(device, event) == VK_EVENT_SET);
                vkDestroyEvent(device, event, nullptr);
            }
            SUCCESS(vkResetCommandBuffer(command, 0));
        }
    }
    vkDestroySemaphore(device, timeline, nullptr);
    std::puts("native Metal delayed event callback reset, replay and destruction: PASS");
}

int main() {
    @autoreleasepool {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &app;
        VkInstance instance{};
        SUCCESS(vkCreateInstance(&instanceInfo, nullptr, &instance));
        uint32_t count = 1;
        VkPhysicalDevice physical{};
        SUCCESS(vkEnumeratePhysicalDevices(instance, &count, &physical));
        REQUIRE(physical != VK_NULL_HANDLE);
        const float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        VkDevice device{}, otherDevice{};
        SUCCESS(vkCreateDevice(physical, &deviceInfo, nullptr, &device));
        SUCCESS(vkCreateDevice(physical, &deviceInfo, nullptr, &otherDevice));
        VkQueue queue{};
        vkGetDeviceQueue(device, 0, 0, &queue);

        const auto resetPool = reinterpret_cast<PFN_vkResetDescriptorPool>(
            vkGetDeviceProcAddr(device, "vkResetDescriptorPool"));
        const auto setEvent2 = reinterpret_cast<PFN_vkCmdSetEvent2>(
            vkGetDeviceProcAddr(device, "vkCmdSetEvent2"));
        REQUIRE(resetPool != nullptr && setEvent2 != nullptr);
        REQUIRE(vkGetDeviceProcAddr(device, "vkCmdSetEvent2KHR") ==
                reinterpret_cast<PFN_vkVoidFunction>(setEvent2));

        // Gen4 allocates batches of 32 and resets an exhausted completed pool.
        // FREE_DESCRIPTOR_SET is deliberately absent: reset must still reclaim it.
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        VkDescriptorSetLayout layout{};
        SUCCESS(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout));
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32};
        VkDescriptorPoolCreateInfo descriptorInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        descriptorInfo.maxSets = 32;
        descriptorInfo.poolSizeCount = 1;
        descriptorInfo.pPoolSizes = &poolSize;
        VkDescriptorPool descriptors{}, independent{};
        SUCCESS(vkCreateDescriptorPool(device, &descriptorInfo, nullptr, &descriptors));
        SUCCESS(vkCreateDescriptorPool(device, &descriptorInfo, nullptr, &independent));
        std::array<VkDescriptorSetLayout, 32> layouts;
        layouts.fill(layout);
        std::array<VkDescriptorSet, 32> sets{};
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = descriptors;
        allocate.descriptorSetCount = layouts.size();
        allocate.pSetLayouts = layouts.data();
        for (unsigned cycle = 0; cycle != 3; ++cycle) {
            SUCCESS(vkAllocateDescriptorSets(device, &allocate, sets.data()));
            VkDescriptorSet overflow{};
            auto one = allocate;
            one.descriptorSetCount = 1;
            REQUIRE(vkAllocateDescriptorSets(device, &one, &overflow) == VK_ERROR_OUT_OF_POOL_MEMORY);
            REQUIRE(resetPool(otherDevice, descriptors, 0) == VK_ERROR_INITIALIZATION_FAILED);
            REQUIRE(resetPool(device, descriptors, 1) == VK_ERROR_INITIALIZATION_FAILED);
            REQUIRE(vkAllocateDescriptorSets(device, &one, &overflow) == VK_ERROR_OUT_OF_POOL_MEMORY);
            SUCCESS(resetPool(device, descriptors, 0));
        }
        REQUIRE(resetPool(device, VK_NULL_HANDLE, 0) == VK_ERROR_INITIALIZATION_FAILED);
        allocate.descriptorPool = independent;
        SUCCESS(vkAllocateDescriptorSets(device, &allocate, sets.data()));
        SUCCESS(resetPool(device, descriptors, 0));
        VkDescriptorSet overflow{};
        allocate.descriptorSetCount = 1;
        REQUIRE(vkAllocateDescriptorSets(device, &allocate, &overflow) == VK_ERROR_OUT_OF_POOL_MEMORY);
        SUCCESS(resetPool(device, independent, 0));

        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = 4096;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buffer{};
        SUCCESS(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer));
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        VkMemoryAllocateInfo memoryInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        memoryInfo.allocationSize = requirements.size;
        memoryInfo.memoryTypeIndex = 1;
        VkDeviceMemory memory{};
        SUCCESS(vkAllocateMemory(device, &memoryInfo, nullptr, &memory));
        SUCCESS(vkBindBufferMemory(device, buffer, memory, 0));
        void* mapped{};
        SUCCESS(vkMapMemory(device, memory, 0, 4096, 0, &mapped));

        VkCommandPoolCreateInfo commandPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkCommandPool commands{};
        SUCCESS(vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commands));
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = commands;
        commandInfo.commandBufferCount = 1;
        VkCommandBuffer command{};
        SUCCESS(vkAllocateCommandBuffers(device, &commandInfo, &command));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VkEventCreateInfo eventInfo{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        VkEvent event{}, foreignEvent{};
        SUCCESS(vkCreateEvent(device, &eventInfo, nullptr, &event));
        SUCCESS(vkCreateEvent(otherDevice, &eventInfo, nullptr, &foreignEvent));
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;

        for (unsigned cycle = 0; cycle != 2; ++cycle) {
            SUCCESS(vkResetEvent(device, event));
            SUCCESS(vkBeginCommandBuffer(command, &begin));
            const uint32_t expected = 0xC1300000U + cycle;
            vkCmdFillBuffer(command, buffer, 0, 4096, expected);
            setEvent2(command, event, &dependency);
            SUCCESS(vkEndCommandBuffer(command));
            REQUIRE(vkGetEventStatus(device, event) == VK_EVENT_RESET);
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command;
            SUCCESS(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            VkResult status{};
            while ((status = vkGetEventStatus(device, event)) == VK_EVENT_RESET &&
                   std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            REQUIRE(status == VK_EVENT_SET);
            // The event itself is the completion proof; no queue wait precedes this read.
            for (size_t i = 0; i != 1024; ++i) REQUIRE(static_cast<uint32_t*>(mapped)[i] == expected);
            SUCCESS(vkQueueWaitIdle(queue));
            SUCCESS(vkResetCommandBuffer(command, 0));
        }

        CheckDelayedEventCompletion(device, queue, command, dependency);

        // Padding stays within Metal's 4096-byte inline-buffer limit, including
        // partial updates at the last word; overflowing writes fail at recording.
        constexpr VkShaderStageFlags pushStages = VK_SHADER_STAGE_VERTEX_BIT |
            VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        const VkPushConstantRange pushRange{pushStages, 0, 4096};
        VkPipelineLayoutCreateInfo pushLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pushLayoutInfo.pushConstantRangeCount = 1;
        pushLayoutInfo.pPushConstantRanges = &pushRange;
        VkPipelineLayout pushLayout{};
        SUCCESS(vkCreatePipelineLayout(device, &pushLayoutInfo, nullptr, &pushLayout));
        const uint32_t lastWord = 0xAABBCCDD;
        SUCCESS(vkBeginCommandBuffer(command, &begin));
        vkCmdPushConstants(command, pushLayout, pushStages, 4092, sizeof(lastWord), &lastWord);
        SUCCESS(vkEndCommandBuffer(command));
        SUCCESS(vkResetCommandBuffer(command, 0));
        SUCCESS(vkBeginCommandBuffer(command, &begin));
        vkCmdPushConstants(command, pushLayout, pushStages, 4096, sizeof(lastWord), &lastWord);
        REQUIRE(vkEndCommandBuffer(command) == VK_ERROR_INITIALIZATION_FAILED);
        SUCCESS(vkResetCommandBuffer(command, 0));
        vkDestroyPipelineLayout(device, pushLayout, nullptr);

        SUCCESS(vkResetEvent(device, event));
        SUCCESS(vkBeginCommandBuffer(command, &begin));
        setEvent2(command, foreignEvent, &dependency);
        REQUIRE(vkEndCommandBuffer(command) == VK_ERROR_INITIALIZATION_FAILED);
        SUCCESS(vkResetCommandBuffer(command, 0));
        SUCCESS(vkBeginCommandBuffer(command, &begin));
        auto malformed = dependency;
        malformed.pMemoryBarriers = nullptr;
        setEvent2(command, event, &malformed);
        REQUIRE(vkEndCommandBuffer(command) == VK_ERROR_FEATURE_NOT_PRESENT);
        REQUIRE(vkGetEventStatus(device, event) == VK_EVENT_RESET);
        SUCCESS(vkResetCommandBuffer(command, 0));
        SUCCESS(vkBeginCommandBuffer(command, &begin));
        setEvent2(command, event, nullptr);
        REQUIRE(vkEndCommandBuffer(command) == VK_ERROR_FEATURE_NOT_PRESENT);

        vkDestroyEvent(otherDevice, foreignEvent, nullptr);
        vkDestroyEvent(device, event, nullptr);
        vkDestroyCommandPool(device, commands, nullptr);
        vkUnmapMemory(device, memory);
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, memory, nullptr);
        vkDestroyDescriptorPool(device, descriptors, nullptr);
        vkDestroyDescriptorPool(device, independent, nullptr);
        vkDestroyDescriptorSetLayout(device, layout, nullptr);
        vkDestroyDevice(otherDevice, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        std::puts("native Metal Gen4 descriptor-pool reuse and sync2 event visibility: PASS");
    }
}
