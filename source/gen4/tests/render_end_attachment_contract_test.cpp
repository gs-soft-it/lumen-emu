// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/emulator_state.h"
#include "core/user_settings.h"
#include "input/controller.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/render_end_attachment_barrier.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"

namespace Core {
class PthreadWaitContractTest final {
public:
    static void InitializeBootMetadata() {
        auto& info = Common::ElfInfo::Instance();
        info.game_serial = "CUSA00000";
        info.title = "Lumen render end attachment contract";
        info.sdk_ver = Common::ElfInfo::FW_900;
        info.initialized = true;
    }
};
} // namespace Core

namespace {
constexpr u32 Width = 32, Height = 32, Pixels = Width * Height;
constexpr u32 GuardBytes = 256, ImageBytes = Pixels * sizeof(u32);
constexpr u32 TotalBytes = GuardBytes + ImageBytes * 2 + GuardBytes;
constexpr u32 Canary = 0x3c5a7e19;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void CheckContract(bool enabled) {
    Require(Vulkan::RenderEndAttachmentBarrierEnabled() == enabled,
            "render end selector did not default on with exact-zero rollback");
    for (bool color : {false, true}) {
        for (bool depth : {false, true}) {
            const auto barrier = Vulkan::GetRenderEndAttachmentBarrier(color, depth);
            Require(barrier.has_value() == (enabled && (color || depth)),
                    "barrier decision includes disabled or attachment-free rendering");
            if (!barrier) continue;
            const auto required_stages =
                (color ? vk::PipelineStageFlags2{vk::PipelineStageFlagBits2::eColorAttachmentOutput}
                       : vk::PipelineStageFlags2{}) |
                (depth ? vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                             vk::PipelineStageFlagBits2::eLateFragmentTests
                       : vk::PipelineStageFlags2{});
            const auto required_access =
                (color ? vk::AccessFlags2{vk::AccessFlagBits2::eColorAttachmentWrite}
                       : vk::AccessFlags2{}) |
                (depth ? vk::AccessFlags2{vk::AccessFlagBits2::eDepthStencilAttachmentWrite}
                       : vk::AccessFlags2{});
            Require(barrier->srcStageMask == required_stages &&
                        barrier->srcAccessMask == required_access,
                    "attachment stores are missing their color/early/late source scopes");
            Require(barrier->dstStageMask == vk::PipelineStageFlagBits2::eAllCommands &&
                        barrier->dstAccessMask ==
                            (vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite),
                    "later attachment loads/writes and other consumers are not covered");
        }
    }
    // Process configuration must stay fixed even if the environment changes.
    Require(setenv("LUMEN_RENDER_END_ATTACHMENT_BARRIER", enabled ? "0" : "1", 1) == 0,
            "failed to change test environment");
    Require(Vulkan::RenderEndAttachmentBarrierEnabled() == enabled,
            "render end selector was not latched for the process");
}

constexpr std::string_view VertexShader = R"(
#version 450
void main() {
    const vec2 p[3] = vec2[3](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
    gl_Position = vec4(p[gl_VertexIndex], 0.5, 1.0);
}
)";
constexpr std::string_view FragmentShader = R"(
#version 450
layout(push_constant) uniform Params { uint round; uint consumer; } params;
layout(location = 0) out vec4 color;
void main() {
    if (params.consumer == 0) {
        gl_FragDepth = ((uint(gl_FragCoord.x) + params.round) & 1u) == 0u ? 0.25 : 0.75;
        color = vec4(12u + params.round, 31u, 77u, 255u) / 255.0;
    } else {
        gl_FragDepth = 0.5;
        color = vec4(211u, 53u + params.round, 19u, 255u) / 255.0;
    }
}
)";

struct TestImage {
    VideoCore::UniqueImage image;
    vk::UniqueImageView view;
    vk::ImageAspectFlags aspect;
    vk::ImageLayout attachment_layout;

    TestImage(const Vulkan::Instance& instance, bool depth)
        : image{instance.GetDevice(), instance.GetAllocator()},
          aspect{depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor},
          attachment_layout{depth ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                  : vk::ImageLayout::eColorAttachmentOptimal} {
        const auto format = depth ? vk::Format::eD32Sfloat : vk::Format::eR8G8B8A8Unorm;
        image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D, .format = format, .extent = {Width, Height, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eTransferSrc |
                     (depth ? vk::ImageUsageFlagBits::eDepthStencilAttachment
                            : vk::ImageUsageFlagBits::eColorAttachment),
        });
        auto result = instance.GetDevice().createImageViewUnique(vk::ImageViewCreateInfo{
            .image = image, .viewType = vk::ImageViewType::e2D, .format = format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        });
        Require(result.result == vk::Result::eSuccess, "attachment view creation failed");
        view = std::move(result.value);
    }
};

vk::UniquePipeline MakePipeline(vk::Device device, vk::PipelineLayout layout, bool consumer) {
    const auto vs = Vulkan::Compile(VertexShader, vk::ShaderStageFlagBits::eVertex, device);
    const auto fs = Vulkan::Compile(FragmentShader, vk::ShaderStageFlagBits::eFragment, device);
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex, .module = vs, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment, .module = fs, .pName = "main"},
    };
    const vk::PipelineVertexInputStateCreateInfo vertex{};
    const vk::PipelineInputAssemblyStateCreateInfo assembly{
        .topology = vk::PrimitiveTopology::eTriangleList};
    const vk::Viewport vp{0, 0, Width, Height, 0, 1};
    const vk::Rect2D sc{{0, 0}, {Width, Height}};
    const vk::PipelineViewportStateCreateInfo viewport{
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
    const vk::PipelineRasterizationStateCreateInfo raster{.lineWidth = 1.0f};
    const vk::PipelineMultisampleStateCreateInfo samples{
        .rasterizationSamples = vk::SampleCountFlagBits::e1};
    const vk::PipelineDepthStencilStateCreateInfo depth{
        .depthTestEnable = true, .depthWriteEnable = true,
        .depthCompareOp = consumer ? vk::CompareOp::eLess : vk::CompareOp::eAlways};
    const vk::PipelineColorBlendAttachmentState blend_attachment{
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
    const vk::PipelineColorBlendStateCreateInfo blend{
        .attachmentCount = 1, .pAttachments = &blend_attachment};
    const vk::Format color_format = vk::Format::eR8G8B8A8Unorm;
    const vk::PipelineRenderingCreateInfo rendering{
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &color_format,
        .depthAttachmentFormat = vk::Format::eD32Sfloat};
    auto result = device.createGraphicsPipelineUnique({}, vk::GraphicsPipelineCreateInfo{
        .pNext = &rendering, .stageCount = static_cast<u32>(stages.size()), .pStages = stages.data(),
        .pVertexInputState = &vertex, .pInputAssemblyState = &assembly,
        .pViewportState = &viewport, .pRasterizationState = &raster,
        .pMultisampleState = &samples, .pDepthStencilState = &depth,
        .pColorBlendState = &blend, .layout = layout,
    });
    device.destroyShaderModule(vs);
    device.destroyShaderModule(fs);
    Require(result.result == vk::Result::eSuccess, "attachment pipeline creation failed");
    return std::move(result.value);
}

void CheckGpu(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler) {
    TestImage color(instance, false), depth(instance, true);
    VideoCore::Buffer readback{instance, scheduler, VideoCore::MemoryUsage::Download, 0,
                              VideoCore::AllFlags, TotalBytes};
    const auto device = instance.GetDevice();
    const vk::PushConstantRange range{vk::ShaderStageFlagBits::eFragment, 0, 2 * sizeof(u32)};
    auto layout_result = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo{
        .pushConstantRangeCount = 1, .pPushConstantRanges = &range});
    Require(layout_result.result == vk::Result::eSuccess, "attachment pipeline layout failed");
    auto layout = std::move(layout_result.value);
    const auto producer = MakePipeline(device, *layout, false);
    const auto consumer = MakePipeline(device, *layout, true);
    const auto attachment_stages = vk::PipelineStageFlagBits2::eColorAttachmentOutput |
                                   vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                   vk::PipelineStageFlagBits2::eLateFragmentTests;
    const auto attachment_access = vk::AccessFlagBits2::eColorAttachmentRead |
                                   vk::AccessFlagBits2::eColorAttachmentWrite |
                                   vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                   vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    for (u32 round = 0; round < 9; ++round) {
        auto commands = scheduler.CommandBuffer();
        commands.fillBuffer(readback.Handle(), 0, TotalBytes, Canary);
        const vk::BufferMemoryBarrier2 prepare_readback{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .buffer = readback.Handle(), .offset = 0, .size = TotalBytes};
        std::array<vk::ImageMemoryBarrier2, 2> image_barriers;
        u32 index = 0;
        for (auto* image : {&color, &depth}) {
            image_barriers[index++] = vk::ImageMemoryBarrier2{
                .srcStageMask = round ? vk::PipelineStageFlagBits2::eCopy
                                      : vk::PipelineStageFlagBits2::eNone,
                .srcAccessMask = round ? vk::AccessFlagBits2::eTransferRead
                                       : vk::AccessFlagBits2::eNone,
                .dstStageMask = attachment_stages, .dstAccessMask = attachment_access,
                .oldLayout = round ? vk::ImageLayout::eTransferSrcOptimal : vk::ImageLayout::eUndefined,
                .newLayout = image->attachment_layout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image->image, .subresourceRange = {image->aspect, 0, 1, 0, 1},
            };
        }
        commands.pipelineBarrier2(vk::DependencyInfo{
            .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &prepare_readback,
            .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = image_barriers.data()});
        Vulkan::RenderState state{};
        state.width = Width; state.height = Height; state.num_layers = 1;
        state.num_color_attachments = 1;
        state.color_attachments[0].image_view = *color.view;
        state.color_attachments[0].image_layout = color.attachment_layout;
        state.color_attachments[0].is_clear = true;
        auto& ds = state.depth_stencil_attachment;
        ds.image_view = *depth.view; ds.image_layout = depth.attachment_layout;
        ds.has_depth = true; ds.depth_clear = true;
        ds.clear_value[0] = std::bit_cast<u32>(1.0f);
        scheduler.BeginRendering(state);
        commands.bindPipeline(vk::PipelineBindPoint::eGraphics, *producer);
        const std::array<u32, 2> producer_params{round, 0};
        commands.pushConstants(*layout, vk::ShaderStageFlagBits::eFragment, 0,
                               sizeof(producer_params), producer_params.data());
        commands.draw(3, 1, 0, 0);

        // No manual barrier, layout change, CPU wait or host upload separates
        // the producer from the LOAD consumer. Exercise production render ends:
        // explicit end, automatic clear-to-load state change, and queue submit.
        if (round % 3 == 0) {
            scheduler.EndRendering();
            scheduler.EndRendering(); // Already inactive: no new end/barrier.
        } else if (round % 3 == 2) {
            scheduler.Flush(); // No wait: next pass uses a fresh command buffer.
        }
        state.color_attachments[0].is_clear = false;
        ds.depth_clear = false;
        scheduler.BeginRendering(state);
        commands = scheduler.CommandBuffer();
        commands.bindPipeline(vk::PipelineBindPoint::eGraphics, *consumer);
        const std::array<u32, 2> consumer_params{round, 1};
        commands.pushConstants(*layout, vk::ShaderStageFlagBits::eFragment, 0,
                               sizeof(consumer_params), consumer_params.data());
        commands.draw(3, 1, 0, 0);
        scheduler.EndRendering();

        for (auto& barrier : image_barriers) {
            barrier.srcStageMask = attachment_stages;
            barrier.srcAccessMask = attachment_access;
            barrier.dstStageMask = vk::PipelineStageFlagBits2::eCopy;
            barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
            barrier.oldLayout = barrier.newLayout;
            barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
        }
        commands.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = image_barriers.data()});
        index = 0;
        for (auto* image : {&color, &depth}) {
            const vk::BufferImageCopy copy{
                .bufferOffset = GuardBytes + index++ * ImageBytes,
                .imageSubresource = {image->aspect, 0, 0, 1},
                .imageExtent = {Width, Height, 1}};
            commands.copyImageToBuffer(image->image, vk::ImageLayout::eTransferSrcOptimal,
                                       readback.Handle(), copy);
        }
        const vk::BufferMemoryBarrier2 host_barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
            .buffer = readback.Handle(), .offset = 0, .size = TotalBytes};
        commands.pipelineBarrier2(vk::DependencyInfo{
            .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &host_barrier});
        scheduler.Finish();
        const auto* bytes = readback.MapReadback();
        readback.InvalidateReadback();
        const auto* words = reinterpret_cast<const u32*>(bytes);
        for (u32 i = 0; i < GuardBytes / sizeof(u32); ++i) {
            Require(words[i] == Canary && words[(TotalBytes - GuardBytes) / sizeof(u32) + i] == Canary,
                    "attachment readback overwrote a guard");
        }
        for (u32 pixel = 0; pixel < Pixels; ++pixel) {
            const bool accepted = ((pixel % Width + round) & 1) != 0;
            const std::array<int, 4> expected = accepted ? std::array<int, 4>{211, int(53 + round), 19, 255}
                                                        : std::array<int, 4>{int(12 + round), 31, 77, 255};
            for (u32 channel = 0; channel < 4; ++channel) {
                Require(std::abs(int(bytes[GuardBytes + pixel * 4 + channel]) - expected[channel]) <= 1,
                        "STORE->LOAD lost color or the loaded depth test selected the wrong pixels");
            }
            const float actual_depth = std::bit_cast<float>(words[(GuardBytes + ImageBytes) / sizeof(u32) + pixel]);
            Require(actual_depth == (accepted ? 0.5f : 0.25f), "STORE->LOAD lost depth contents");
        }
    }
    std::fprintf(stderr,
                 "RENDER_END_ATTACHMENT_GPU rounds=9 pixels=1024 explicit_end=3 state_restart=3 "
                 "submit_restart=3 color_load=1 depth_load=1 guards=1 PASS\n");
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc == 2 ? argv[1] : "";
        const bool selector_off = mode == "--selector-off";
        const bool contract_only = selector_off || mode == "--contract-only";
        const auto path = std::filesystem::temp_directory_path() /
                          ("lumen-render-end-attachment-" + std::to_string(getpid()));
        Common::FS::InitializeUserPaths(path);
        EmulatorState::SetInstance(std::make_shared<EmulatorState>());
        EmulatorSettingsImpl::SetInstance(std::make_shared<EmulatorSettingsImpl>());
        Core::PthreadWaitContractTest::InitializeBootMetadata();
        Common::Log::Setup("render-end-attachment-contract.log");
        UserSettings.Load();
        for (auto& [name, logger] : Common::Log::ALL_LOGGERS) logger->set_level(spdlog::level::info);
        CheckContract(!selector_off);
        if (!contract_only) {
            Input::GameControllers controllers;
            Frontend::WindowSDL window(320, 240, &controllers, "Lumen attachment contract",
                                       "render-end-attachment-contract");
            Vulkan::Instance instance(window, -1, false, false);
            Vulkan::Scheduler scheduler(instance);
            CheckGpu(instance, scheduler);
        }
        Common::Log::Shutdown();
        UserSettingsImpl::SetInstance(nullptr);
        std::filesystem::remove_all(path);
        std::puts(selector_off ? "render end attachment selector OFF PASS"
                               : "render end attachment contract PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "render end attachment contract failed: %s\n", error.what());
        return 1;
    }
}
