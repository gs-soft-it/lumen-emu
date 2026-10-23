// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include "common/alignment.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/emulator_state.h"
#include "core/user_settings.h"
#include "input/controller.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_upload_barrier.h"
#include "video_core/texture_cache/tile_manager.h"

namespace Core {
class PthreadWaitContractTest final {
public:
    static void InitializeBootMetadata() {
        auto& info = Common::ElfInfo::Instance();
        info.game_serial = "CUSA00000";
        info.title = "Lumen detile source contract";
        info.sdk_ver = Common::ElfInfo::FW_900;
        info.initialized = true;
    }
};
} // namespace Core

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Barrier(vk::CommandBuffer commands, const vk::BufferMemoryBarrier2& barrier) {
    commands.pipelineBarrier2(vk::DependencyInfo{
        .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier});
}

// This writes only on the GPU. No host upload or broad post-upload barrier can
// accidentally publish the producer's writes before the production dependency.
constexpr std::string_view WriterShader = R"(
#version 450
layout(local_size_x = 64) in;
layout(set = 0, binding = 0, std430) writeonly buffer Output { uint words[]; } output_data;
layout(push_constant) uniform Pattern { uint value; } pattern;
void main() { output_data.words[gl_GlobalInvocationID.x] = pattern.value; }
)";

void CheckGpuSource(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, bool tiled) {
    constexpr u32 Canary = 0x3c5a7e19;
    VideoCore::ImageInfo info{};
    info.type = AmdGpu::ImageType::Color2D;
    info.resources = {1, 1};
    info.size = {64, 64, 1};
    info.pitch = 64;
    info.num_bits = 32;
    info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    info.props.is_tiled = tiled;
    info.tile_mode = tiled ? AmdGpu::TileMode::Display1DThin
                           : AmdGpu::TileMode::DisplayLinearAligned;
    info.array_mode = AmdGpu::GetArrayMode(info.tile_mode);
    info.UpdateSize();
    Require(info.guest_size == 64 * 64 * sizeof(u32), "unexpected test image padding");
    const u32 offset = static_cast<u32>(Common::AlignUp(u64{256}, instance.StorageMinAlignment()));
    const u32 total_size = offset + info.guest_size + offset;
    VideoCore::Buffer source{instance, scheduler, VideoCore::MemoryUsage::DeviceLocal, 0,
                            VideoCore::AllFlags, total_size};
    VideoCore::Buffer readback{instance, scheduler, VideoCore::MemoryUsage::Download, 0,
                              VideoCore::AllFlags, total_size};
    VideoCore::StreamBuffer params{instance, scheduler, VideoCore::MemoryUsage::Upload, 1_MB};
    VideoCore::TileManager detiler{instance, scheduler, params};
    const auto device = instance.GetDevice();
    const vk::DescriptorSetLayoutBinding binding{
        .binding = 0, .descriptorType = vk::DescriptorType::eStorageBuffer,
        .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute};
    auto layout_result = device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo{
        .bindingCount = 1, .pBindings = &binding});
    Require(layout_result.result == vk::Result::eSuccess, "writer descriptor layout failed");
    auto descriptor_layout = std::move(layout_result.value);
    const vk::DescriptorSetLayout raw_layout = *descriptor_layout;
    const vk::PushConstantRange push_range{
        .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(u32)};
    auto pipeline_layout_result = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo{
        .setLayoutCount = 1, .pSetLayouts = &raw_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push_range});
    Require(pipeline_layout_result.result == vk::Result::eSuccess, "writer pipeline layout failed");
    auto pipeline_layout = std::move(pipeline_layout_result.value);
    const auto shader = Vulkan::Compile(WriterShader, vk::ShaderStageFlagBits::eCompute, device);
    auto pipeline_result = device.createComputePipelineUnique({}, vk::ComputePipelineCreateInfo{
        .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = shader, .pName = "main"},
        .layout = *pipeline_layout});
    device.destroyShaderModule(shader);
    Require(pipeline_result.result == vk::Result::eSuccess, "writer pipeline failed");
    auto pipeline = std::move(pipeline_result.value);
    const vk::DescriptorPoolSize pool_size{vk::DescriptorType::eStorageBuffer, 1};
    auto pool_result = device.createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo{
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size});
    Require(pool_result.result == vk::Result::eSuccess, "writer descriptor pool failed");
    auto pool = std::move(pool_result.value);
    auto sets = device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
        .descriptorPool = *pool, .descriptorSetCount = 1, .pSetLayouts = &raw_layout});
    Require(sets.result == vk::Result::eSuccess, "writer descriptor allocation failed");
    const auto descriptor = sets.value.front();
    const vk::DescriptorBufferInfo source_info{source.Handle(), offset, info.guest_size};
    device.updateDescriptorSets(vk::WriteDescriptorSet{
        .dstSet = descriptor, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &source_info}, {});

    if (auto barrier = source.GetBarrier(vk::AccessFlagBits2::eTransferWrite,
                                         vk::PipelineStageFlagBits2::eTransfer)) {
        Barrier(scheduler.CommandBuffer(), *barrier);
    }
    scheduler.CommandBuffer().fillBuffer(source.Handle(), 0, total_size, Canary);
    const auto consumer_stage = tiled ? vk::PipelineStageFlagBits2::eComputeShader
                                     : vk::PipelineStageFlagBits2::eTransfer;
    const auto consumer_access = tiled ? vk::AccessFlagBits2::eShaderRead
                                      : vk::AccessFlagBits2::eTransferRead;
    for (u32 round = 0; round < 8; ++round) {
        const auto commands = scheduler.CommandBuffer();
        const auto before_write = source.GetBarrier(vk::AccessFlagBits2::eShaderWrite,
                                                     vk::PipelineStageFlagBits2::eComputeShader);
        Require(before_write.has_value(), "the next GPU writer lost its read-to-write dependency");
        if (round != 0) {
            Require(bool(before_write->srcStageMask & consumer_stage) &&
                        bool(before_write->srcAccessMask & consumer_access),
                    "tracked consumer does not protect source reuse after detiling/copy");
        }
        Barrier(commands, *before_write);
        commands.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
        commands.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeline_layout,
                                    0, descriptor, {});
        const u32 pattern = 0x10203040u + round * 0x07050301u;
        commands.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute,
                                0, sizeof(pattern), &pattern);
        commands.dispatch(info.guest_size / sizeof(u32) / 64, 1, 1);

        const auto dependency = VideoCore::GetImageUploadSourceBarrier(source, tiled);
        Require(dependency.has_value(), "GPU producer has no image-upload dependency");
        // An independent access-scope check makes the missing dependency
        // deterministic even on GPUs which happen to serialize both commands.
        Require(bool(dependency->srcStageMask & vk::PipelineStageFlagBits2::eComputeShader) &&
                    bool(dependency->srcAccessMask & vk::AccessFlagBits2::eShaderWrite) &&
                    bool(dependency->dstStageMask & consumer_stage) &&
                    bool(dependency->dstAccessMask & consumer_access),
                "GPU producer is not visible to the actual next consumer");
        Require(dependency->offset <= offset && dependency->size >= offset + info.guest_size,
                "dependency does not cover the nonzero source offset");
        Barrier(commands, *dependency);
        const auto [linear, linear_offset] = detiler.DetileImage(source.Handle(), offset, info);
        if (tiled) {
            Barrier(scheduler.CommandBuffer(), vk::BufferMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                .buffer = linear, .offset = linear_offset, .size = info.guest_size});
        } else {
            Require(linear == source.Handle() && linear_offset == offset,
                    "linear upload unexpectedly allocated or rebased its source");
        }
        Barrier(scheduler.CommandBuffer(), vk::BufferMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eHost,
            .srcAccessMask = vk::AccessFlagBits2::eHostRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .buffer = readback.Handle(), .offset = 0, .size = total_size});
        scheduler.CommandBuffer().copyBuffer(linear, readback.Handle(),
                                             vk::BufferCopy{linear_offset, 0, info.guest_size});
        Barrier(scheduler.CommandBuffer(), vk::BufferMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
            .buffer = readback.Handle(), .offset = 0, .size = total_size});
        scheduler.Finish();
        auto* pixels = reinterpret_cast<const u32*>(readback.MapReadback());
        readback.InvalidateReadback();
        Require(std::all_of(pixels, pixels + info.guest_size / sizeof(u32),
                            [=](u32 value) { return value == pattern; }),
                "GPU-written image source was stale or corrupted after detiling/copy");
    }
    // Guard bytes share the same source buffer, but the compute descriptor and
    // detiler must read/write only their nonzero-offset payload.
    if (auto barrier = source.GetBarrier(vk::AccessFlagBits2::eTransferRead,
                                         vk::PipelineStageFlagBits2::eTransfer)) {
        Barrier(scheduler.CommandBuffer(), *barrier);
    }
    Barrier(scheduler.CommandBuffer(), vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eHost,
        .srcAccessMask = vk::AccessFlagBits2::eHostRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = readback.Handle(), .offset = 0, .size = total_size});
    scheduler.CommandBuffer().copyBuffer(source.Handle(), readback.Handle(),
                                         vk::BufferCopy{0, 0, total_size});
    Barrier(scheduler.CommandBuffer(), vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        .buffer = readback.Handle(), .offset = 0, .size = total_size});
    scheduler.Finish();
    auto* words = reinterpret_cast<const u32*>(readback.MapReadback());
    readback.InvalidateReadback();
    Require(std::all_of(words, words + offset / sizeof(u32), [](u32 value) { return value == Canary; }) &&
                std::all_of(words + (offset + info.guest_size) / sizeof(u32),
                            words + total_size / sizeof(u32), [](u32 value) { return value == Canary; }),
            "GPU writer or detiler changed a source guard");
    std::fprintf(stderr, "DETILE_SOURCE_CONTRACT_GPU tiled=%d rounds=8 pixels=4096 source_offset=%u guards=1 PASS\n",
                 tiled, offset);
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc == 2 ? argv[1] : "";
        const bool selector_off = mode == "--selector-off";
        const bool selector_only = selector_off || mode == "--selector-on";
        const auto path = std::filesystem::temp_directory_path() /
                          ("lumen-detile-source-contract-" + std::to_string(getpid()));
        Common::FS::InitializeUserPaths(path);
        EmulatorState::SetInstance(std::make_shared<EmulatorState>());
        EmulatorSettingsImpl::SetInstance(std::make_shared<EmulatorSettingsImpl>());
        Core::PthreadWaitContractTest::InitializeBootMetadata();
        Common::Log::Setup("detile-source-contract.log");
        UserSettings.Load();
        for (auto& [name, logger] : Common::Log::ALL_LOGGERS) logger->set_level(spdlog::level::info);
        Require(VideoCore::DetileSourceBarrierEnabled() != selector_off,
                "detile source selector did not default on with exact-zero rollback");
        Require(setenv("LUMEN_DETILE_SOURCE_BARRIER", selector_off ? "1" : "0", 1) == 0,
                "failed to change test environment");
        Require(VideoCore::DetileSourceBarrierEnabled() != selector_off,
                "detile source selector was not latched for the process");
        if (!selector_only) {
            Input::GameControllers controllers;
            Frontend::WindowSDL window(320, 240, &controllers, "Lumen detile source contract",
                                       "detile-source-contract");
            Vulkan::Instance instance(window, -1, false, false);
            Vulkan::Scheduler scheduler(instance);
            CheckGpuSource(instance, scheduler, true);
            CheckGpuSource(instance, scheduler, false);
        }
        Common::Log::Shutdown();
        UserSettingsImpl::SetInstance(nullptr);
        std::filesystem::remove_all(path);
        std::puts(selector_off ? "detile source selector OFF PASS"
                               : selector_only ? "detile source selector ON PASS"
                                               : "detile GPU source contract PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "detile source contract failed: %s\n", error.what());
        return 1;
    }
}
