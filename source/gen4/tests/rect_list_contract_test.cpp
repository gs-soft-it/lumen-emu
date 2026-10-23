// SPDX-License-Identifier: GPL-2.0-or-later
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "common/object_pool.h"
#include "shader_recompiler/backend/spirv/emit_spirv_rect_list.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)
#define SUCCESS(x) CHECK((x) == VK_SUCCESS)

namespace {
using Sirit::Id;
namespace SPV = Shader::Backend::SPIRV;
using Vec = std::array<float, 4>;
struct Vertex { Vec position; Vec smooth; Vec flat; };
constexpr unsigned Width = 64, Height = 64, Bytes = Width * Height * 4;

std::vector<Vertex> Inputs(unsigned edge) {
    std::vector<Vertex> result;
    for (unsigned rect = 0; rect < 2; ++rect) {
        const float x = rect ? 0.1f : -0.85f;
        const std::array<Vertex, 3> corners{{
            {{x, -0.65f, 0.22f, 1.0f}, {0.15f, 0.2f, 0.3f, 1}, {0.2f, 0, 0, 1}},
            {{x + 0.5f, -0.65f, 0.31f, 1.1f}, {0.55f, 0.3f, 0.4f, 1}, {0.5f, 0, 0, 1}},
            {{x, -0.1f, 0.42f, 0.9f}, {0.25f, 0.7f, 0.6f, 1}, {0.8f, 0, 0, 1}},
        }};
        // Rotate source order so the rectangle's shared edge corner occupies
        // each possible input index. The reference knows the original geometry.
        for (unsigned i = 0; i < 3; ++i) result.push_back(corners[(i + 3 - edge) % 3]);
    }
    return result;
}

std::vector<Vertex> Reference(const std::vector<Vertex>& input, unsigned edge, bool last) {
    std::vector<Vertex> output;
    for (unsigned rect = 0; rect < input.size() / 3; ++rect) {
        const auto* v = input.data() + rect * 3;
        std::array<Vertex, 4> corners{v[edge], v[(edge + 1) % 3], v[(edge + 2) % 3], {}};
        for (unsigned c = 0; c < 4; ++c) {
            corners[3].position[c] = corners[1].position[c] + corners[2].position[c] - corners[0].position[c];
            corners[3].smooth[c] = corners[1].smooth[c] + corners[2].smooth[c] - corners[0].smooth[c];
        }
        for (auto& corner : corners) corner.flat = v[last ? 2 : 0].flat;
        // Tessellation at unit factors creates the same two triangles. This
        // reference contains expanded CPU data, no replay or private index.
        for (unsigned i : {0u, 1u, 2u, 1u, 3u, 2u}) output.push_back(corners[i]);
    }
    return output;
}

std::vector<u32> VertexShader(const std::vector<Vertex>& vertices, bool wrapped, bool last, bool transform) {
    Shader::Info info{};
    info.stage = Shader::Stage::Vertex;
    info.l_stage = Shader::LogicalStage::Vertex;
    for (unsigned c = 0; c < 4; ++c) {
        info.stores.Set(Shader::IR::Attribute::Position0, c);
        info.stores.Set(Shader::IR::Attribute::Param0, c);
        info.stores.Set(Shader::IR::Attribute::Param1, c);
    }
    Shader::RuntimeInfo runtime{};
    runtime.vs_info.emulate_rect_list = wrapped;
    runtime.vs_info.rect_list_flat_mask = 1u << 1;
    runtime.vs_info.rect_list_provoking_last = last;
    runtime.vs_info.clip_disable = wrapped && transform;
    runtime.vs_info.emulate_depth_negative_one_to_one = wrapped && transform;
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010500;
    profile.max_viewport_width = profile.max_viewport_height = 128;
    Shader::Backend::Bindings bindings{};
    SPV::EmitContext ctx(profile, runtime, info, bindings);
    ctx.AddCapability(spv::Capability::DrawParameters);
    const auto arrayType = ctx.TypeArray(ctx.F32[4], ctx.ConstU32(static_cast<u32>(vertices.size())));
    const auto makeArray = [&](auto member) {
        std::vector<Id> values;
        for (const auto& vertex : vertices) {
            const auto& v = vertex.*member;
            values.push_back(ctx.ConstF32(v[0], v[1], v[2], v[3]));
        }
        const auto variable = ctx.DefineVar(arrayType, spv::StorageClass::Private,
                                            ctx.ConstantComposite(arrayType, values));
        ctx.interfaces.push_back(variable);
        return variable;
    };
    const auto positions = makeArray(&Vertex::position);
    const auto smooth = makeArray(&Vertex::smooth);
    const auto flat = makeArray(&Vertex::flat);
    const auto guest = ctx.OpFunction(ctx.void_id, spv::FunctionControlMask::MaskNone,
                                      ctx.TypeFunction(ctx.void_id));
    ctx.AddLabel();
    const auto vertex = ctx.OpLoad(ctx.U32[1], wrapped ? ctx.rect_vertex_index : ctx.vertex_index);
    const auto index = ctx.OpISub(ctx.U32[1], vertex, ctx.OpLoad(ctx.U32[1], ctx.base_vertex));
    const auto ptr = ctx.TypePointer(spv::StorageClass::Private, ctx.F32[4]);
    auto position = ctx.OpLoad(ctx.F32[4], ctx.OpAccessChain(ptr, positions, index));
    // firstInstance is deliberately nonzero. Two instances translate the same
    // rectangles vertically, proving replay preserves InstanceIndex.
    const auto instance = ctx.OpConvertUToF(ctx.F32[1], ctx.OpLoad(ctx.U32[1], ctx.instance_id));
    const auto dy = ctx.OpFMul(ctx.F32[1], ctx.OpFSub(ctx.F32[1], instance, ctx.ConstF32(3.0f)), ctx.ConstF32(0.8f));
    const auto y = ctx.OpFAdd(ctx.F32[1], ctx.OpCompositeExtract(ctx.F32[1], position, 1), dy);
    position = ctx.OpCompositeInsert(ctx.F32[4], y, position, 1);
    if (!wrapped && transform) {
        // Independent reference for viewport push data (60,56,48,40) and
        // depth [-w,+w] -> [0,+w]. The replay wrapper owns the real epilogue.
        const auto x = ctx.OpCompositeExtract(ctx.F32[1], position, 0);
        const auto z = ctx.OpCompositeExtract(ctx.F32[1], position, 2);
        const auto w = ctx.OpCompositeExtract(ctx.F32[1], position, 3);
        const auto tx = ctx.OpFSub(ctx.F32[1], ctx.OpFMul(ctx.F32[1], x, ctx.ConstF32(0.75f)), ctx.ConstF32(0.0625f));
        const auto ty = ctx.OpFSub(ctx.F32[1], ctx.OpFMul(ctx.F32[1], y, ctx.ConstF32(0.625f)), ctx.ConstF32(0.125f));
        const auto tz = ctx.OpFMul(ctx.F32[1], ctx.OpFAdd(ctx.F32[1], z, w), ctx.ConstF32(0.5f));
        const std::array components{tx, ty, tz, w};
        position = ctx.OpCompositeConstruct(ctx.F32[4], components);
    }
    ctx.OpStore(ctx.output_position, position);
    ctx.OpStore(ctx.output_params[0].id, ctx.OpLoad(ctx.F32[4], ctx.OpAccessChain(ptr, smooth, index)));
    ctx.OpStore(ctx.output_params[1].id, ctx.OpLoad(ctx.F32[4], ctx.OpAccessChain(ptr, flat, index)));
    SPV::EmitEpilogue(ctx);
    ctx.OpReturn();
    ctx.OpFunctionEnd();
    const auto main = wrapped ? SPV::EmitRectListVertexWrapper(ctx, guest) : guest;
    ctx.AddEntryPoint(spv::ExecutionModel::Vertex, main, "main",
                      std::span(ctx.interfaces.data(), ctx.interfaces.size()));
    return ctx.Assemble();
}

std::vector<u32> FragmentShader() {
    Sirit::Module m(0x00010000);
    m.AddCapability(spv::Capability::Shader);
    m.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    const auto v = m.TypeVoid(), f = m.TypeFloat(32), vec = m.TypeVector(f, 4);
    const auto in = m.TypePointer(spv::StorageClass::Input, vec);
    const auto smooth = m.AddGlobalVariable(in, spv::StorageClass::Input);
    const auto flat = m.AddGlobalVariable(in, spv::StorageClass::Input);
    const auto output = m.AddGlobalVariable(m.TypePointer(spv::StorageClass::Output, vec), spv::StorageClass::Output);
    m.Decorate(smooth, spv::Decoration::Location, 0);
    m.Decorate(flat, spv::Decoration::Location, 1);
    m.Decorate(flat, spv::Decoration::Flat);
    m.Decorate(output, spv::Decoration::Location, 0);
    const auto main = m.OpFunction(v, spv::FunctionControlMask::MaskNone, m.TypeFunction(v));
    m.AddLabel();
    const auto color = m.OpLoad(vec, smooth);
    const auto alpha = m.OpCompositeExtract(f, m.OpLoad(vec, flat), 0);
    m.OpStore(output, m.OpCompositeInsert(vec, alpha, color, 3));
    m.OpReturn(); m.OpFunctionEnd();
    const std::array interfaces{smooth, flat, output};
    m.AddEntryPoint(spv::ExecutionModel::Fragment, main, "main", interfaces);
    m.AddExecutionMode(main, spv::ExecutionMode::OriginUpperLeft);
    return m.Assemble();
}

struct Gpu {
    VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{};
    VkCommandPool pool{}; VkCommandBuffer command{}; VkPipelineLayout layout{};
    VkImage color{}, depth{}; VkImageView colorView{}, depthView{};
    VkDeviceMemory colorMemory{}, depthMemory{}, readMemory{}; VkBuffer readback{};
    void* mapped{};
    Gpu() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo = &app;
        SUCCESS(vkCreateInstance(&ci, nullptr, &instance));
        uint32_t count = 1; SUCCESS(vkEnumeratePhysicalDevices(instance, &count, &physical));
        VkPhysicalDeviceVulkan11Features features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features};
        vkGetPhysicalDeviceFeatures2(physical, &query);
        CHECK(!features.shaderDrawParameters); // DrawIndex is not a public capability.
        float priority = 1; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueCount = 1; qi.pQueuePriorities = &priority;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
        SUCCESS(vkCreateDevice(physical, &di, nullptr, &device)); vkGetDeviceQueue(device, 0, 0, &queue);
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        SUCCESS(vkCreateCommandPool(device, &pi, nullptr, &pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        SUCCESS(vkAllocateCommandBuffers(device, &ai, &command));
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Shader::PushData)};
        li.pushConstantRangeCount = 1; li.pPushConstantRanges = &range;
        SUCCESS(vkCreatePipelineLayout(device, &li, nullptr, &layout));
        MakeImage(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, color, colorMemory, colorView);
        MakeImage(VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, depth, depthMemory, depthView);
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = Bytes * 2; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        SUCCESS(vkCreateBuffer(device, &bi, nullptr, &readback));
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device, readback, &req);
        readMemory = Allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        SUCCESS(vkBindBufferMemory(device, readback, readMemory, 0));
        SUCCESS(vkMapMemory(device, readMemory, 0, Bytes * 2, 0, &mapped));
    }
    VkDeviceMemory Allocate(VkMemoryRequirements req, VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size;
        VkPhysicalDeviceMemoryProperties props{}; vkGetPhysicalDeviceMemoryProperties(physical, &props);
        for (; ai.memoryTypeIndex < props.memoryTypeCount; ++ai.memoryTypeIndex)
            if ((req.memoryTypeBits & (1u << ai.memoryTypeIndex)) &&
                (props.memoryTypes[ai.memoryTypeIndex].propertyFlags & flags) == flags) break;
        CHECK(ai.memoryTypeIndex < props.memoryTypeCount);
        VkDeviceMemory memory{}; SUCCESS(vkAllocateMemory(device, &ai, nullptr, &memory)); return memory;
    }
    void MakeImage(VkFormat format, VkImageAspectFlags aspect, VkImage& image, VkDeviceMemory& memory, VkImageView& view) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = format; ii.extent = {Width, Height, 1}; ii.mipLevels = ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        SUCCESS(vkCreateImage(device, &ii, nullptr, &image)); VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, image, &req); memory = Allocate(req);
        SUCCESS(vkBindImageMemory(device, image, memory, 0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format; vi.subresourceRange = {aspect, 0, 1, 0, 1}; SUCCESS(vkCreateImageView(device, &vi, nullptr, &view));
    }
    VkShaderModule Module(const std::vector<u32>& words) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize = words.size() * 4; ci.pCode = words.data();
        VkShaderModule module{}; SUCCESS(vkCreateShaderModule(device, &ci, nullptr, &module)); return module;
    }
    std::vector<uint8_t> Render(const std::vector<u32>& vs) {
        const auto vert = Module(vs), frag = Module(FragmentShader());
        const VkPipelineShaderStageCreateInfo stages[] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main"},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main"}};
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp{0, 0, Width, Height, 0, 1}; VkRect2D sc{{0, 0}, {Width, Height}};
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1; viewport.pViewports = &vp; viewport.pScissors = &sc;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendAttachmentState attachment{}; attachment.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &attachment;
        const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &format; rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
        ci.stageCount = 2; ci.pStages = stages; ci.pVertexInputState = &vertex; ci.pInputAssemblyState = &assembly;
        ci.pViewportState = &viewport; ci.pRasterizationState = &raster; ci.pMultisampleState = &samples;
        ci.pDepthStencilState = &ds; ci.pColorBlendState = &blend; ci.layout = layout;
        VkPipeline pipeline{}; SUCCESS(vkCreateGraphicsPipelines(device, nullptr, 1, &ci, nullptr, &pipeline));
        SUCCESS(vkResetCommandBuffer(command, 0)); VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        SUCCESS(vkBeginCommandBuffer(command, &begin));
        std::array<VkImageMemoryBarrier, 2> barriers{};
        for (unsigned i = 0; i < barriers.size(); ++i) {
            auto& barrier = barriers[i]; barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.dstAccessMask = i ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.newLayout = i ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = i ? depth : color;
            barrier.subresourceRange = {i ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            0, 0, nullptr, 0, nullptr, barriers.size(), barriers.data());
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; ca.imageView = colorView;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; da.imageView = depthView;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        da.clearValue.depthStencil.depth = 1;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = sc; ri.layerCount = 1;
        ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca; ri.pDepthAttachment = &da;
        vkCmdBeginRendering(command, &ri); vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        Shader::PushData push{}; push.xoffset = 60; push.yoffset = 56; push.xscale = 48; push.yscale = 40;
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        vkCmdDraw(command, 12, 2, 7, 3); vkCmdEndRendering(command);
        for (auto& barrier : barriers) {
            barrier.srcAccessMask = barrier.dstAccessMask; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout = barrier.newLayout; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, barriers.size(), barriers.data());
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {Width, Height, 1};
        vkCmdCopyImageToBuffer(command, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
        copy.bufferOffset = Bytes; copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCmdCopyImageToBuffer(command, depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
        SUCCESS(vkEndCommandBuffer(command)); VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        SUCCESS(vkQueueSubmit(queue, 1, &submit, nullptr)); SUCCESS(vkQueueWaitIdle(queue));
        std::vector<uint8_t> result(Bytes * 2); std::memcpy(result.data(), mapped, result.size());
        vkDestroyPipeline(device, pipeline, nullptr); vkDestroyShaderModule(device, vert, nullptr); vkDestroyShaderModule(device, frag, nullptr);
        return result;
    }
    ~Gpu() {
        vkUnmapMemory(device, readMemory); vkDestroyBuffer(device, readback, nullptr); vkFreeMemory(device, readMemory, nullptr);
        vkDestroyImageView(device, colorView, nullptr); vkDestroyImageView(device, depthView, nullptr);
        vkDestroyImage(device, color, nullptr); vkDestroyImage(device, depth, nullptr);
        vkFreeMemory(device, colorMemory, nullptr); vkFreeMemory(device, depthMemory, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr); vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyDevice(device, nullptr); vkDestroyInstance(instance, nullptr);
    }
};

void CheckReplayPolicy() {
    namespace IR = Shader::IR;
    Shader::Info info{}; info.stage = Shader::Stage::Vertex; info.l_stage = Shader::LogicalStage::Vertex;
    Common::ObjectPool<IR::Inst> pool{16}; IR::Block block{pool}; IR::Program program{info}; program.blocks.push_back(&block);
    CHECK(SPV::RectListReplayUnsupportedReason(program) == nullptr);
    info.has_fetch_shader = true;
    CHECK(SPV::RectListReplayUnsupportedReason(program) != nullptr);
    info.has_fetch_shader = false;
    info.uses_lane_id = true;
    CHECK(SPV::RectListReplayUnsupportedReason(program) != nullptr);
    info.uses_lane_id = false;
    info.uses_shader_clock = true;
    CHECK(SPV::RectListReplayUnsupportedReason(program) != nullptr);
    info.uses_shader_clock = false;
    info.shared_types = IR::Type::U32;
    CHECK(SPV::RectListReplayUnsupportedReason(program) != nullptr);
    info.shared_types = IR::Type::Void;
    CHECK(SPV::RectListReplayUnsupportedReason(program) == nullptr);
    block.AppendNewInst(IR::Opcode::StoreBufferU32, {IR::Value{0u}, IR::Value{0u}, IR::Value{1u}});
    CHECK(SPV::RectListReplayUnsupportedReason(program) != nullptr);
}
void CheckLineWidthPolicy() {
    using AmdGpu::PrimitiveType;
    using AmdGpu::PolygonMode;
    using Stages = AmdGpu::ShaderStageEnable;
    using Vulkan::LiverpoolToVK::RasterizationLineWidth;
    for (const float width : {0.0f, 1.0f, 2.5f}) {
        for (const auto primitive : {PrimitiveType::TriangleList, PrimitiveType::TriangleStrip,
                                     PrimitiveType::TriangleFan, PrimitiveType::AdjTriangleList,
                                     PrimitiveType::AdjTriangleStrip, PrimitiveType::RectList,
                                     PrimitiveType::QuadList, PrimitiveType::QuadStrip,
                                     PrimitiveType::Polygon}) {
            CHECK(RasterizationLineWidth(Stages::Vs, primitive, PolygonMode::Fill, width) == 1.0f);
            CHECK(RasterizationLineWidth(Stages::Vs, primitive, PolygonMode::Point, width) == 1.0f);
            CHECK(RasterizationLineWidth(Stages::Vs, primitive, PolygonMode::Line, width) == width);
            for (const auto stages : {Stages::EsGs, Stages::LsHs, Stages::LsHsEsGs}) {
                CHECK(RasterizationLineWidth(stages, primitive, PolygonMode::Fill, width) == width);
            }
        }
        for (const auto primitive : {PrimitiveType::LineList, PrimitiveType::LineStrip,
                                     PrimitiveType::LineLoop, PrimitiveType::AdjLineList,
                                     PrimitiveType::AdjLineStrip, PrimitiveType::PatchPrimitive,
                                     static_cast<PrimitiveType>(0xffffffffu)}) {
            CHECK(RasterizationLineWidth(Stages::Vs, primitive, PolygonMode::Fill, width) == width);
        }
        CHECK(RasterizationLineWidth(Stages::Vs, PrimitiveType::PointList, PolygonMode::Line, width) == 1.0f);
    }
    std::puts("Line width: filled rect/quad/triangle state normalized; real lines, wireframe and shader-generated topology preserved PASS");
}
} // namespace

int main() {
    CheckLineWidthPolicy();
    CheckReplayPolicy();
    Gpu gpu;
    for (bool transform : {false, true}) for (unsigned edge = 0; edge < 3; ++edge) for (bool last : {false, true}) {
        const auto inputs = Inputs(edge);
        const auto expected = gpu.Render(VertexShader(Reference(inputs, edge, last), false, last, transform));
        const auto actual = gpu.Render(VertexShader(inputs, true, last, transform));
        unsigned colored = 0;
        for (unsigned i = 0; i < Bytes; ++i) {
            CHECK(std::abs(int(expected[i]) - int(actual[i])) <= 1);
            colored += (i % 4 == 3 && actual[i] != 0);
        }
        CHECK(colored > (transform ? 200u : 400u));
        for (unsigned i = Bytes; i < Bytes * 2; i += 4) {
            float a{}, b{}; std::memcpy(&a, actual.data() + i, 4); std::memcpy(&b, expected.data() + i, 4);
            CHECK(std::isfinite(a) && std::abs(a - b) < 0.00002f);
        }
        std::printf("RectList corner=%u flat=%s clip/depth-transform=%u pixels=%u color/depth=verified\n", edge, last ? "last" : "first", transform, colored);
    }
    std::puts("RectList replay policy, all corners, variable z/w, smooth/flat attributes, clip-disable viewport and depth conversion, multiple rectangles, firstVertex=7 and firstInstance=3 PASS");
}
