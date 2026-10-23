// SPDX-License-Identifier: MIT
#include "emulator/src/Graphics/Metal/MetalVulkanInternal.h"
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifndef LUMEN_TILING_SHADER_PATH
#define LUMEN_TILING_SHADER_PATH "source/gen4/src/video_core/host_shaders/tiling.comp"
#endif
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::abort(); } } while (false)
#define OK(x) CHECK((x) == VK_SUCCESS)

static std::vector<uint32_t> Compile(uint32_t bits, uint32_t samples, bool depth, bool tile) {
    std::ifstream file(LUMEN_TILING_SHADER_PATH); CHECK(file.good());
    const std::string source{std::istreambuf_iterator<char>(file), {}};
    const char* text = source.c_str();
    glslang::TShader shader(EShLangCompute);
    shader.setStrings(&text, 1);
    const std::string defines = "#define BITS_PER_PIXEL " + std::to_string(bits) +
        "\n#define NUM_SAMPLES " + std::to_string(samples) +
        "\n#define MICRO_TILE_MODE " + std::to_string(depth ? 2 : 1) +
        "\n#define ARRAY_MODE 2\n#define MICRO_TILE_THICKNESS 1\n" +
        (tile ? "#define IS_TILER 1\n" : "");
    shader.setPreamble(defines.c_str());
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangCompute, glslang::EShClientVulkan, 450);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    const auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
        std::fprintf(stderr, "%s\n", shader.getInfoLog()); std::abort();
    }
    glslang::TProgram program; program.addShader(&shader); CHECK(program.link(messages));
    std::vector<uint32_t> words;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute), words);
    return words;
}

static uint32_t Morton8(uint32_t x, uint32_t y) {
    uint32_t result = 0;
    for (uint32_t bit = 0; bit != 3; ++bit) {
        result |= ((x >> bit) & 1) << (2 * bit);
        result |= ((y >> bit) & 1) << (2 * bit + 1);
    }
    return result;
}

struct Mip { uint32_t size, pitch, height, offset; };
struct Params { uint32_t bank, layers, count; Mip mips[16]; };
static_assert(sizeof(Params) == 268);

int main() { @autoreleasepool {
    CHECK(glslang::InitializeProcess());
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; VkInstance instance{};
    OK(vkCreateInstance(&ici, nullptr, &instance)); uint32_t count = 1; VkPhysicalDevice physical{};
    OK(vkEnumeratePhysicalDevices(instance, &count, &physical));
    const float priority = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueCount = 1; qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice device{}; OK(vkCreateDevice(physical, &dci, nullptr, &device));
    VkQueue queue{}; vkGetDeviceQueue(device, 0, 0, &queue);
    constexpr size_t BufferSize = 65536, DescriptorOffset = 256;
    std::array<VkBuffer, 3> buffers{};
    std::array<VkDeviceMemory, 3> memories{};
    std::array<void*, 3> mapped{};
    for (size_t i = 0; i != 3; ++i) {
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size = BufferSize;
        ci.usage = i == 2 ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        OK(vkCreateBuffer(device, &ci, nullptr, &buffers[i]));
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = BufferSize; ai.memoryTypeIndex = 1;
        OK(vkAllocateMemory(device, &ai, nullptr, &memories[i]));
        OK(vkBindBufferMemory(device, buffers[i], memories[i], 0));
        OK(vkMapMemory(device, memories[i], 0, BufferSize, 0, &mapped[i]));
    }
    const std::array<VkDescriptorSetLayoutBinding, 3> bindings{{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = bindings.size(); lci.pBindings = bindings.data();
    VkDescriptorSetLayout setLayout{}; OK(vkCreateDescriptorSetLayout(device, &lci, nullptr, &setLayout));
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1; plci.pSetLayouts = &setLayout;
    VkPipelineLayout layout{}; OK(vkCreatePipelineLayout(device, &plci, nullptr, &layout));
    const std::array<VkDescriptorPoolSize, 2> sizes{{
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1; dpci.poolSizeCount = sizes.size(); dpci.pPoolSizes = sizes.data();
    VkDescriptorPool pool{}; OK(vkCreateDescriptorPool(device, &dpci, nullptr, &pool));
    VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    sai.descriptorPool = pool; sai.descriptorSetCount = 1; sai.pSetLayouts = &setLayout;
    VkDescriptorSet set{}; OK(vkAllocateDescriptorSets(device, &sai, &set));
    std::array<VkDescriptorBufferInfo, 3> infos{};
    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t i = 0; i != 3; ++i) {
        infos[i] = {buffers[i], DescriptorOffset, BufferSize - DescriptorOffset};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, i, 0, 1,
            i == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            nullptr, &infos[i], nullptr};
    }
    vkUpdateDescriptorSets(device, writes.size(), writes.data(), 0, nullptr);
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool commands{}; OK(vkCreateCommandPool(device, &cpci, nullptr, &commands));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = commands; cbai.commandBufferCount = 1;
    VkCommandBuffer command{}; OK(vkAllocateCommandBuffers(device, &cbai, &command));
    for (const uint32_t bits : {8U, 16U, 32U, 64U, 128U}) {
        for (const bool depth : {false, true}) {
            for (const uint32_t samples : {1U, 4U}) {
                if (samples == 1 && bits != 32) continue;
                const auto bytes = bits / 8;
                Params params{}; params.layers = 2; params.count = 2;
                params.mips[0] = {16 * 8 * 2 * samples * bytes, 16, 8, 64};
                params.mips[1] = {8 * 8 * 2 * samples * bytes, 8, 8, 128 + params.mips[0].size};
                const auto end = params.mips[1].offset + params.mips[1].size;
                CHECK(DescriptorOffset + end + 64 < BufferSize);
                std::array<uint8_t, BufferSize> linear, tiled;
                linear.fill(0xA5); tiled.fill(0xA5);
                for (uint32_t m = 0; m != 2; ++m) {
                    const auto& mip = params.mips[m];
                    for (uint32_t layer = 0; layer != 2; ++layer)
                    for (uint32_t y = 0; y != mip.height; ++y)
                    for (uint32_t x = 0; x != mip.pitch; ++x)
                    for (uint32_t sample = 0; sample != samples; ++sample) {
                        const auto linearIndex = ((layer * mip.height + y) * mip.pitch + x) * samples + sample;
                        const auto tileIndex = layer * (mip.pitch / 8) + x / 8;
                        const auto within = depth ? Morton8(x, y) * samples + sample
                                                  : sample * 64 + Morton8(x, y);
                        const auto tiledIndex = tileIndex * 64 * samples + within;
                        for (uint32_t lane = 0; lane != bytes; ++lane) {
                            const uint8_t value = 1 + (x * 7 + y * 13 + layer * 37 + sample * 47 + m * 19 + lane * 31) % 251;
                            linear[DescriptorOffset + mip.offset + linearIndex * bytes + lane] = value;
                            tiled[DescriptorOffset + mip.offset + tiledIndex * bytes + lane] = value;
                        }
                    }
                }
                std::memcpy(static_cast<uint8_t*>(mapped[2]) + DescriptorOffset, &params, sizeof(params));
                for (const bool tile : {true, false}) {
                    std::memcpy(mapped[0], tiled.data(), BufferSize);
                    std::memcpy(mapped[1], linear.data(), BufferSize);
                    std::memset(mapped[tile ? 0 : 1], 0xA5, BufferSize);
                    auto spv = Compile(bits, samples, depth, tile);
                    VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                    sci.codeSize = spv.size() * 4; sci.pCode = spv.data(); VkShaderModule shader{};
                    OK(vkCreateShaderModule(device, &sci, nullptr, &shader));
                    VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pci.layout = layout;
                    pci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                        VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr};
                    VkPipeline compute{}; OK(vkCreateComputePipelines(device, {}, 1, &pci, nullptr, &compute));
                    OK(vkResetCommandBuffer(command, 0));
                    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                    OK(vkBeginCommandBuffer(command, &begin));
                    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, compute);
                    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
                    vkCmdDispatch(command, ((end / bytes) + 63) / 64, 1, 1);
                    OK(vkEndCommandBuffer(command));
                    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
                    OK(vkQueueSubmit(queue, 1, &submit, {})); OK(vkQueueWaitIdle(queue));
                    const auto& expected = tile ? tiled : linear;
                    const auto* actual = static_cast<const uint8_t*>(mapped[tile ? 0 : 1]);
                    if (std::memcmp(actual, expected.data(), BufferSize) != 0) {
                        for (size_t i = 0; i != BufferSize; ++i) if (actual[i] != expected[i]) {
                            std::fprintf(stderr, "bits=%u samples=%u depth=%d tile=%d byte=%zu got=%02x expected=%02x\n",
                                bits, samples, depth, tile, i, actual[i], expected[i]); break;
                        }
                        std::abort();
                    }
                    vkDestroyPipeline(device, compute, nullptr); vkDestroyShaderModule(device, shader, nullptr);
                }
            }
        }
    }
    vkDestroyCommandPool(device, commands, nullptr); vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr); vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    for (size_t i = 0; i != 3; ++i) {
        vkUnmapMemory(device, memories[i]); vkDestroyBuffer(device, buffers[i], nullptr);
        vkFreeMemory(device, memories[i], nullptr);
    }
    vkDestroyDevice(device, nullptr); vkDestroyInstance(instance, nullptr); glslang::FinalizeProcess();
    std::puts("native Metal tiling: independent color/depth sample layouts, two mips/layers and padding PASS");
}}
