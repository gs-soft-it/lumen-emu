// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"

#include "video_core/host_shaders/color_to_ms_depth_frag.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/ms_image_blit_frag.h"
#include "video_core/host_shaders/sample_readback_comp.h"

namespace VideoCore {

static vk::SampleCountFlagBits ToSampleCount(u32 num_samples) {
    switch (num_samples) {
    case 1:
        return vk::SampleCountFlagBits::e1;
    case 2:
        return vk::SampleCountFlagBits::e2;
    case 4:
        return vk::SampleCountFlagBits::e4;
    case 8:
        return vk::SampleCountFlagBits::e8;
    case 16:
        return vk::SampleCountFlagBits::e16;
    default:
        UNREACHABLE_MSG("Unknown samples count = {}", num_samples);
    }
}

BlitHelper::BlitHelper(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_},
      uses_push_descriptors{instance_.IsPushDescriptorSupported()},
      desc_heap{instance_, scheduler_.GetMasterSemaphore(), pool_sizes, 64} {
    CreateShaders();
    CreatePipelineLayouts();
}

BlitHelper::~BlitHelper() {
    const auto device = instance.GetDevice();
    device.destroy(fs_tri_vertex);
    device.destroy(color_to_ms_depth_frag);
    device.destroy(src_msaa_copy_frag);
    device.destroy(src_non_msaa_copy_frag);
}

void BlitHelper::ReinterpretColorAsMsDepth(u32 width, u32 height, u32 num_samples,
                                           vk::Format src_pixel_format, vk::Format dst_pixel_format,
                                           vk::Image source, vk::Image dest) {
    const vk::ImageViewUsageCreateInfo color_usage_ci{.usage = vk::ImageUsageFlagBits::eSampled};
    const vk::ImageViewCreateInfo color_view_ci = {
        .pNext = &color_usage_ci,
        .image = source,
        .viewType = vk::ImageViewType::e2D,
        .format = src_pixel_format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0U,
            .levelCount = 1U,
            .baseArrayLayer = 0U,
            .layerCount = 1U,
        },
    };
    const auto [color_view_result, color_view] =
        instance.GetDevice().createImageView(color_view_ci);
    ASSERT_MSG(color_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(color_view_result));
    const vk::ImageViewUsageCreateInfo depth_usage_ci{
        .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment};
    const vk::ImageViewCreateInfo depth_view_ci = {
        .pNext = &depth_usage_ci,
        .image = dest,
        .viewType = vk::ImageViewType::e2D,
        .format = dst_pixel_format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eDepth,
            .baseMipLevel = 0U,
            .levelCount = 1U,
            .baseArrayLayer = 0U,
            .layerCount = 1U,
        },
    };
    const auto [depth_view_result, depth_view] =
        instance.GetDevice().createImageView(depth_view_ci);
    ASSERT_MSG(depth_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(depth_view_result));
    scheduler.DeferOperation([device = instance.GetDevice(), color_view, depth_view] {
        device.destroyImageView(color_view);
        device.destroyImageView(depth_view);
    });

    Vulkan::RenderState state{};
    state.width = width;
    state.height = height;
    state.num_layers = 1;
    state.depth_stencil_attachment.image_view = depth_view;
    state.depth_stencil_attachment.image_layout = vk::ImageLayout::eDepthAttachmentOptimal;
    state.depth_stencil_attachment.has_depth = true;
    state.depth_stencil_attachment.depth_clear = true;
    scheduler.BeginRendering(state);

    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::DescriptorImageInfo image_info = {
        .sampler = VK_NULL_HANDLE,
        .imageView = color_view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet texture_write = {
        .dstSet = VK_NULL_HANDLE,
        .dstBinding = 0U,
        .dstArrayElement = 0U,
        .descriptorCount = 1U,
        .descriptorType = vk::DescriptorType::eSampledImage,
        .pImageInfo = &image_info,
    };
    if (uses_push_descriptors) {
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *single_texture_pl_layout, 0U,
                                    texture_write);
    } else {
        const auto desc_set = desc_heap.Commit(*single_texture_descriptor_set_layout);
        vk::WriteDescriptorSet set_write = texture_write;
        set_write.dstSet = desc_set;
        instance.GetDevice().updateDescriptorSets(set_write, {});
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *single_texture_pl_layout, 0U,
                                  desc_set, {});
    }

    const MsPipelineKey key{num_samples, dst_pixel_format, false};
    auto it = std::ranges::find(color_to_ms_depth_pl, key, &MsPipeline::first);
    if (it == color_to_ms_depth_pl.end()) {
        CreateColorToMSDepthPipeline(key);
        it = --color_to_ms_depth_pl.end();
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *it->second);

    const vk::Viewport viewport = {
        .x = 0,
        .y = 0,
        .width = float(state.width),
        .height = float(state.height),
        .minDepth = 0.f,
        .maxDepth = 1.f,
    };
    cmdbuf.setViewportWithCount(viewport);

    const vk::Rect2D scissor = {
        .offset = {0, 0},
        .extent = {state.width, state.height},
    };
    cmdbuf.setScissorWithCount(scissor);

    cmdbuf.draw(3, 1, 0, 0);

    scheduler.EndRendering();
    scheduler.GetDynamicState().Invalidate();
}

void BlitHelper::CopyBetweenMsImages(u32 width, u32 height, u32 num_samples,
                                     vk::Format pixel_format, bool src_msaa, vk::Image source,
                                     vk::Image dest) {
    const vk::ImageViewUsageCreateInfo src_usage_ci{.usage = vk::ImageUsageFlagBits::eSampled};
    const vk::ImageViewCreateInfo src_view_ci = {
        .pNext = &src_usage_ci,
        .image = source,
        .viewType = vk::ImageViewType::e2D,
        .format = pixel_format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0U,
            .levelCount = 1U,
            .baseArrayLayer = 0U,
            .layerCount = 1U,
        },
    };
    const auto [src_view_result, src_view] = instance.GetDevice().createImageView(src_view_ci);
    ASSERT_MSG(src_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(src_view_result));

    const vk::ImageViewUsageCreateInfo dst_usage_ci{.usage =
                                                        vk::ImageUsageFlagBits::eColorAttachment};
    const vk::ImageViewCreateInfo dst_view_ci = {
        .pNext = &dst_usage_ci,
        .image = dest,
        .viewType = vk::ImageViewType::e2D,
        .format = pixel_format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0U,
            .levelCount = 1U,
            .baseArrayLayer = 0U,
            .layerCount = 1U,
        },
    };
    const auto [dst_view_result, dst_view] = instance.GetDevice().createImageView(dst_view_ci);
    ASSERT_MSG(dst_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(dst_view_result));
    scheduler.DeferOperation([device = instance.GetDevice(), src_view, dst_view] {
        device.destroyImageView(src_view);
        device.destroyImageView(dst_view);
    });

    Vulkan::RenderState state{};
    state.width = width;
    state.height = height;
    state.num_layers = 1;
    state.num_color_attachments = 1;
    state.color_attachments[0].image_view = dst_view;
    state.color_attachments[0].image_layout = vk::ImageLayout::eColorAttachmentOptimal;
    state.color_attachments[0].is_clear = true;
    scheduler.BeginRendering(state);

    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::DescriptorImageInfo image_info = {
        .sampler = VK_NULL_HANDLE,
        .imageView = src_view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet texture_write = {
        .dstSet = VK_NULL_HANDLE,
        .dstBinding = 0U,
        .dstArrayElement = 0U,
        .descriptorCount = 1U,
        .descriptorType = vk::DescriptorType::eSampledImage,
        .pImageInfo = &image_info,
    };
    if (uses_push_descriptors) {
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *single_texture_pl_layout, 0U,
                                    texture_write);
    } else {
        const auto desc_set = desc_heap.Commit(*single_texture_descriptor_set_layout);
        vk::WriteDescriptorSet set_write = texture_write;
        set_write.dstSet = desc_set;
        instance.GetDevice().updateDescriptorSets(set_write, {});
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *single_texture_pl_layout, 0U,
                                  desc_set, {});
    }

    const MsPipelineKey key{num_samples, pixel_format, src_msaa};
    auto it = std::ranges::find(ms_image_copy_pl, key, &MsPipeline::first);
    if (it == ms_image_copy_pl.end()) {
        CreateMsCopyPipeline(key);
        it = --ms_image_copy_pl.end();
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *it->second);

    const vk::Viewport viewport = {
        .x = 0,
        .y = 0,
        .width = float(state.width),
        .height = float(state.height),
        .minDepth = 0.f,
        .maxDepth = 1.f,
    };
    cmdbuf.setViewportWithCount(viewport);

    const vk::Rect2D scissor = {
        .offset = {0, 0},
        .extent = {state.width, state.height},
    };
    cmdbuf.setScissorWithCount(scissor);

    cmdbuf.draw(3, 1, 0, 0);

    scheduler.EndRendering();
    scheduler.GetDynamicState().Invalidate();
}

namespace {
struct SampleFormat {
    vk::Format view;
    u32 bits, channels, depth_bits = 0;
    bool floating = false, packed10 = false;
};
SampleFormat ReadbackFormat(vk::Format format, vk::ImageAspectFlags aspect, u32 bits) {
    if (aspect == vk::ImageAspectFlagBits::eStencil) return {format, 8, 1};
    if (aspect == vk::ImageAspectFlagBits::eDepth) {
        if (bits == 16) return {format, 16, 1, 16, true};
        if (format == vk::Format::eD24UnormS8Uint) return {format, 32, 1, 24, true};
        return {format, 32, 1, 32, true};
    }
    // Integer views in the Vulkan format compatibility class expose the
    // stored bits, including signed-normalized endpoints and NaN payloads.
    switch (bits) {
    case 8: return {vk::Format::eR8Uint, 8, 1};
    case 16: return {vk::Format::eR16Uint, 16, 1};
    case 32: return {vk::Format::eR32Uint, 32, 1};
    case 64: return {vk::Format::eR32G32Uint, 32, 2};
    case 128: return {vk::Format::eR32G32B32A32Uint, 32, 4};
    default: UNREACHABLE_MSG("Unsupported per-sample texel size {}", bits);
    }
}
} // namespace

void BlitHelper::DownloadSamples(vk::Image source, vk::Format format, u32 samples, u32 bits,
                                 std::span<const vk::BufferImageCopy> copies, vk::Buffer destination) {
    const auto device = instance.GetDevice();
    if (!sample_readback_set_layout) {
        const std::array bindings{
            vk::DescriptorSetLayoutBinding{0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            vk::DescriptorSetLayoutBinding{1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        };
        const auto flags = uses_push_descriptors ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR : vk::DescriptorSetLayoutCreateFlags{};
        auto [result, layout] = device.createDescriptorSetLayoutUnique({.flags = flags, .bindingCount = u32(bindings.size()), .pBindings = bindings.data()});
        ASSERT(result == vk::Result::eSuccess);
        sample_readback_set_layout = std::move(layout);
        const auto set_layout = *sample_readback_set_layout;
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 9 * sizeof(u32)};
        auto [pl_result, pl] = device.createPipelineLayoutUnique({.setLayoutCount = 1, .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push});
        ASSERT(pl_result == vk::Result::eSuccess);
        sample_readback_pl_layout = std::move(pl);
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    for (const auto& copy : copies) {
        ASSERT(copy.imageExtent.depth == 1 && copy.imageOffset.z == 0 && copy.bufferOffset <= UINT32_MAX);
        const auto packing = ReadbackFormat(format, copy.imageSubresource.aspectMask, bits);
        const u64 key = u64(packing.bits) | (u64(packing.channels) << 8) |
                        (u64(packing.depth_bits) << 16) | (u64(packing.floating) << 24) | (u64(packing.packed10) << 25);
        auto found = std::ranges::find(sample_readback_pipelines, key, [](const auto& pair) { return pair.first; });
        if (found == sample_readback_pipelines.end()) {
            const std::vector<std::string> defines{
                fmt::format("FLOAT_INPUT={}", packing.floating ? 1 : 0),
                fmt::format("DEPTH_BITS={}", packing.depth_bits),
                fmt::format("CHANNEL_BITS={}", packing.bits),
                fmt::format("CHANNELS={}", packing.channels),
                fmt::format("TEXEL_BYTES={}", packing.bits * packing.channels / 8),
                fmt::format("PACKED_10={}", packing.packed10 ? 1 : 0),
            };
            const auto shader = Vulkan::Compile(HostShaders::SAMPLE_READBACK_COMP, vk::ShaderStageFlagBits::eCompute, device, defines);
            const vk::PipelineShaderStageCreateInfo stage{.stage = vk::ShaderStageFlagBits::eCompute, .module = shader, .pName = "main"};
            const vk::ComputePipelineCreateInfo info{.stage = stage, .layout = *sample_readback_pl_layout};
            auto [result, pipeline] = device.createComputePipelineUnique({}, info);
            device.destroyShaderModule(shader);
            ASSERT_MSG(result == vk::Result::eSuccess, "Per-sample download pipeline: {}", vk::to_string(result));
            sample_readback_pipelines.emplace_back(key, std::move(pipeline));
            found = std::prev(sample_readback_pipelines.end());
        }
        const vk::ImageViewUsageCreateInfo view_usage{.usage = vk::ImageUsageFlagBits::eSampled};
        const vk::ImageViewCreateInfo view_info{
            .pNext = &view_usage, .image = source, .viewType = vk::ImageViewType::e2DArray,
            .format = packing.view,
            .subresourceRange{copy.imageSubresource.aspectMask, copy.imageSubresource.mipLevel, 1,
                              copy.imageSubresource.baseArrayLayer, copy.imageSubresource.layerCount},
        };
        const auto [view_result, view] = device.createImageView(view_info);
        ASSERT_MSG(view_result == vk::Result::eSuccess, "Per-sample download view: {}", vk::to_string(view_result));
        scheduler.DeferOperation([device, view] { device.destroyImageView(view); });
        const vk::DescriptorImageInfo image_info{{}, view, vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorBufferInfo buffer_info{destination, 0, VK_WHOLE_SIZE};
        std::array writes{
            vk::WriteDescriptorSet{.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &image_info},
            vk::WriteDescriptorSet{.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &buffer_info},
        };
        if (uses_push_descriptors) {
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *sample_readback_pl_layout, 0, writes);
        } else {
            const auto set = desc_heap.Commit(*sample_readback_set_layout);
            for (auto& write : writes) write.dstSet = set;
            device.updateDescriptorSets(writes, {});
            cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *sample_readback_pl_layout, 0, set, {});
        }
        const std::array<u32, 9> params{
            u32(copy.bufferOffset), copy.bufferRowLength ? copy.bufferRowLength : copy.imageExtent.width,
            copy.bufferImageHeight ? copy.bufferImageHeight : copy.imageExtent.height,
            copy.imageExtent.width, copy.imageExtent.height, copy.imageSubresource.layerCount,
            samples, u32(copy.imageOffset.x), u32(copy.imageOffset.y),
        };
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *found->second);
        cmdbuf.pushConstants(*sample_readback_pl_layout, vk::ShaderStageFlagBits::eCompute, 0,
                            u32(sizeof(params)), params.data());
        cmdbuf.dispatch((copy.imageExtent.width + 7) / 8, (copy.imageExtent.height + 7) / 8,
                        copy.imageSubresource.layerCount);
    }
}

void BlitHelper::CreateShaders() {
    const auto device = instance.GetDevice();
    fs_tri_vertex =
        Vulkan::Compile(HostShaders::FS_TRI_VERT, vk::ShaderStageFlagBits::eVertex, device);
    color_to_ms_depth_frag = Vulkan::Compile(HostShaders::COLOR_TO_MS_DEPTH_FRAG,
                                             vk::ShaderStageFlagBits::eFragment, device);
    src_msaa_copy_frag = Vulkan::Compile(HostShaders::MS_IMAGE_BLIT_FRAG,
                                         vk::ShaderStageFlagBits::eFragment, device, {"SRC_MSAA"});
    src_non_msaa_copy_frag = Vulkan::Compile(HostShaders::MS_IMAGE_BLIT_FRAG,
                                             vk::ShaderStageFlagBits::eFragment, device);
}

void BlitHelper::CreatePipelineLayouts() {
    const vk::DescriptorSetLayoutBinding texture_binding = {
        .binding = 0,
        .descriptorType = vk::DescriptorType::eSampledImage,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };
    const vk::DescriptorSetLayoutCreateFlags layout_flags =
        uses_push_descriptors ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                              : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = layout_flags,
        .bindingCount = 1U,
        .pBindings = &texture_binding,
    };
    auto [desc_layout_result, desc_layout] =
        instance.GetDevice().createDescriptorSetLayoutUnique(desc_layout_ci);
    single_texture_descriptor_set_layout = std::move(desc_layout);
    const vk::DescriptorSetLayout set_layout = *single_texture_descriptor_set_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 0U,
        .pPushConstantRanges = nullptr,
    };
    auto [layout_result, pipeline_layout] =
        instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics pipeline layout: {}", vk::to_string(layout_result));
    Vulkan::SetObjectName(instance.GetDevice(), *pipeline_layout, "Single texture pipeline layout");
    single_texture_pl_layout = std::move(pipeline_layout);
}

void BlitHelper::CreateColorToMSDepthPipeline(const MsPipelineKey& key) {
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling = {
        .rasterizationSamples = ToSampleCount(key.num_samples),
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_state = {
        .depthTestEnable = true,
        .depthWriteEnable = true,
        .depthCompareOp = vk::CompareOp::eAlways,
    };
    const std::array dynamic_states = {vk::DynamicState::eViewportWithCount,
                                       vk::DynamicState::eScissorWithCount};
    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages;
    shader_stages[0] = {
        .stage = vk::ShaderStageFlagBits::eVertex,
        .module = fs_tri_vertex,
        .pName = "main",
    };
    shader_stages[1] = {
        .stage = vk::ShaderStageFlagBits::eFragment,
        .module = color_to_ms_depth_frag,
        .pName = "main",
    };

    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci = {
        .colorAttachmentCount = 0U,
        .pColorAttachmentFormats = nullptr,
        .depthAttachmentFormat = key.attachment_format,
        .stencilAttachmentFormat = vk::Format::eUndefined,
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending{};
    const vk::PipelineViewportStateCreateInfo viewport_info{};
    const vk::PipelineVertexInputStateCreateInfo vertex_input_info{};
    const vk::PipelineRasterizationStateCreateInfo raster_state{.lineWidth = 1.f};

    const vk::GraphicsPipelineCreateInfo pipeline_info = {
        .pNext = &pipeline_rendering_ci,
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depth_state,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *single_texture_pl_layout,
    };

    auto [pipeline_result, pipeline] =
        instance.GetDevice().createGraphicsPipelineUnique(VK_NULL_HANDLE, pipeline_info);
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create graphics pipeline: {}",
               vk::to_string(pipeline_result));
    Vulkan::SetObjectName(instance.GetDevice(), *pipeline, "Color to MS Depth {}", key.num_samples);

    color_to_ms_depth_pl.emplace_back(key, std::move(pipeline));
}

void BlitHelper::CreateMsCopyPipeline(const MsPipelineKey& key) {
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling = {
        .rasterizationSamples = ToSampleCount(key.num_samples),
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_state = {
        .depthTestEnable = false,
        .depthWriteEnable = false,
        .depthCompareOp = vk::CompareOp::eAlways,
    };
    const std::array dynamic_states = {vk::DynamicState::eViewportWithCount,
                                       vk::DynamicState::eScissorWithCount};
    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages;
    shader_stages[0] = {
        .stage = vk::ShaderStageFlagBits::eVertex,
        .module = fs_tri_vertex,
        .pName = "main",
    };
    shader_stages[1] = {
        .stage = vk::ShaderStageFlagBits::eFragment,
        .module = key.src_msaa ? src_msaa_copy_frag : src_non_msaa_copy_frag,
        .pName = "main",
    };

    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci = {
        .colorAttachmentCount = 1u,
        .pColorAttachmentFormats = &key.attachment_format,
        .depthAttachmentFormat = vk::Format::eUndefined,
        .stencilAttachmentFormat = vk::Format::eUndefined,
    };

    const vk::PipelineColorBlendAttachmentState attachment = {
        .blendEnable = false,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable = false,
        .logicOp = vk::LogicOp::eCopy,
        .attachmentCount = 1u,
        .pAttachments = &attachment,
    };
    const vk::PipelineViewportStateCreateInfo viewport_info{};
    const vk::PipelineVertexInputStateCreateInfo vertex_input_info{};
    const vk::PipelineRasterizationStateCreateInfo raster_state{.lineWidth = 1.f};

    const vk::GraphicsPipelineCreateInfo pipeline_info = {
        .pNext = &pipeline_rendering_ci,
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depth_state,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *single_texture_pl_layout,
    };

    auto [pipeline_result, pipeline] =
        instance.GetDevice().createGraphicsPipelineUnique(VK_NULL_HANDLE, pipeline_info);
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create graphics pipeline: {}",
               vk::to_string(pipeline_result));
    Vulkan::SetObjectName(instance.GetDevice(), *pipeline, "Non MS Image to MS Image {}",
                          key.num_samples);

    ms_image_copy_pl.emplace_back(key, std::move(pipeline));
}

} // namespace VideoCore
