// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>
#include "core/emulator_settings.h"
#include "video_core/renderer_vulkan/image_binding.h"

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

AmdGpu::Image MakeImage() {
    auto image = AmdGpu::Image::Null(false);
    image.base_address = 0x810000;
    image.data_format = u64(AmdGpu::DataFormat::Format10_11_11);
    image.num_format = u64(AmdGpu::NumberFormat::Snorm);
    return image;
}

void CheckFormats() {
    using namespace Vulkan::LiverpoolToVK;
    Require(!IsSurfaceFormatSupported(AmdGpu::DataFormat::Format10_11_11,
                                      AmdGpu::NumberFormat::Snorm),
            "the unsupported 6/1 format was invented");
    Require(IsSurfaceFormatSupported(AmdGpu::DataFormat::Format10_11_11,
                                     AmdGpu::NumberFormat::Float),
            "the valid packed float format was rejected");
    for (u32 data = 0; data != 64; ++data) {
        for (u32 number = 0; number != 16; ++number) {
            const auto dfmt = static_cast<AmdGpu::DataFormat>(data);
            const auto nfmt = static_cast<AmdGpu::NumberFormat>(number);
            const bool expected = std::ranges::any_of(SurfaceFormats(), [&](const auto& entry) {
                return entry.data_format == dfmt && entry.number_format == nfmt &&
                       entry.vk_format != vk::Format::eUndefined;
            });
            Require(IsSurfaceFormatSupported(dfmt, nfmt) == expected,
                    "format support differs from the existing conversion table");
        }
    }
}

void CheckBindingValidation() {
    auto image = MakeImage();
    unsigned mapping_checks{};
    bool mapped = true;
    const auto is_mapped = [&](VAddr address, u64 size) {
        ++mapping_checks;
        Require(address == image.Address() && size == 1, "mapping probe changed address or size");
        return mapped;
    };
    Require(!Vulkan::IsImageBindingValid(image, is_mapped),
            "the crashing 6/1 descriptor reached image construction");
    Require(mapping_checks == 1, "non-null descriptor did not check its GPU mapping");

    image.num_format = u64(AmdGpu::NumberFormat::Float);
    Require(Vulkan::IsImageBindingValid(image, is_mapped), "valid packed float image rejected");
    const VideoCore::TextureCache::ImageDesc valid_desc{image, Shader::ImageResource{}};
    Require(valid_desc.info.pixel_format == vk::Format::eB10G11R11UfloatPack32,
            "valid descriptor changed its Vulkan format");

    mapped = false;
    Require(!Vulkan::IsImageBindingValid(image, is_mapped), "unmapped image accepted");
    mapped = true;
    mapping_checks = 0;
    image.base_address = u64{1} << 32;
    Require(!Vulkan::IsImageBindingValid(image, is_mapped), "address outside 40 bits accepted");
    image.base_address = 0;
    Require(!Vulkan::IsImageBindingValid(image, is_mapped), "null image accepted");
    image.base_address = 0x810000;
    image.data_format = 0;
    Require(!Vulkan::IsImageBindingValid(image, is_mapped), "invalid data format accepted");
    Require(mapping_checks == 0, "null/invalid/high address performed a mapping probe");

    image.data_format = 15;
    Require(!Vulkan::IsImageBindingValid(image, is_mapped), "reserved data format accepted");
    image.data_format = u64(AmdGpu::DataFormat::Format10_11_11);
    image.num_format = 8;
    Require(!Vulkan::IsImageBindingValid(image, is_mapped), "reserved number format accepted");
    image.num_format = 14;
    Require(!Vulkan::IsImageBindingValid(image, is_mapped), "out of range number format accepted");
}

AmdGpu::Image MakeLinearGeneralImage(u32 width, u32 height, u32 pitch) {
    auto image = AmdGpu::Image::Null(false);
    image.base_address = 0x318440000ull >> 8;
    image.width = width - 1;
    image.height = height - 1;
    image.pitch = pitch - 1;
    image.tiling_index = u64(AmdGpu::TileMode::DisplayLinearGeneral);
    return image;
}

void RequireMip(const VideoCore::ImageInfo& info, u32 level, u32 bytes, u32 pitch,
                u32 height, u32 offset) {
    const auto& mip = info.mips_layout[level];
    if (mip.size != bytes || mip.pitch != pitch || mip.height != height || mip.offset != offset) {
        std::fprintf(stderr, "Mip mismatch mode=%u bits=%u level=%u: actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n",
                     u32(info.array_mode), info.num_bits, level, mip.size, mip.pitch, mip.height,
                     mip.offset, bytes, pitch, height, offset);
    }
    Require(mip.size == bytes && mip.pitch == pitch && mip.height == height &&
                mip.offset == offset,
            "linear-general mip byte extent or copy stride changed");
}

void CheckLinearGeneralLayouts() {
    const auto construct = [](const AmdGpu::Image& image) {
        return VideoCore::TextureCache::ImageDesc{image, Shader::ImageResource{}}.info;
    };
    // Reproduce the sampled descriptor that previously asserted in UpdateSize.
    auto image = MakeLinearGeneralImage(8192, 4096, 8192);
    auto info = construct(image);
    Require(info.guest_address == 0x318440000ull && info.guest_size == 134217728 &&
                info.array_mode == AmdGpu::ArrayMode::ArrayLinearGeneral &&
                !info.props.is_tiled && !info.IsTiled() && info.resources.levels == 1 &&
                info.resources.layers == 1 && info.num_samples == 1,
            "valid sampled linear-general descriptor was not preserved");
    RequireMip(info, 0, 134217728, 8192, 4096, 0);

    struct FormatCase {
        AmdGpu::DataFormat format;
        AmdGpu::NumberFormat number;
        u32 bytes;
    };
    constexpr std::array formats{
        FormatCase{AmdGpu::DataFormat::Format8, AmdGpu::NumberFormat::Unorm, 1},
        FormatCase{AmdGpu::DataFormat::Format16, AmdGpu::NumberFormat::Unorm, 2},
        FormatCase{AmdGpu::DataFormat::Format8_8_8_8, AmdGpu::NumberFormat::Unorm, 4},
        FormatCase{AmdGpu::DataFormat::Format16_16_16_16, AmdGpu::NumberFormat::Unorm, 8},
        FormatCase{AmdGpu::DataFormat::Format32_32_32_32, AmdGpu::NumberFormat::Float, 16},
    };
    for (const auto& format : formats) {
        image = MakeLinearGeneralImage(13, 7, 19);
        image.data_format = u64(format.format);
        image.num_format = u64(format.number);
        info = construct(image);
        RequireMip(info, 0, 133 * format.bytes, 19, 7, 0);
        Require(info.guest_size == 133 * format.bytes,
                "linear-general added row or slice padding");
    }

    image = MakeLinearGeneralImage(13, 7, 16);
    image.pow2pad = 1;
    image.last_level = 4;
    info = construct(image);
    RequireMip(info, 0, 512, 16, 8, 0);
    RequireMip(info, 1, 128, 8, 4, 512);
    // Preserve the existing per-level order: shrink the logical height, then
    // apply pow2 padding (7 >> 2 is 1, rather than shrinking padded height 8).
    RequireMip(info, 2, 16, 4, 1, 640);
    RequireMip(info, 3, 8, 2, 1, 656);
    RequireMip(info, 4, 4, 1, 1, 664);
    Require(info.guest_size == 668, "linear-general pow2 mip extent changed");

    image = MakeLinearGeneralImage(8, 4, 8);
    image.type = u64(AmdGpu::ImageType::Color2DArray);
    image.depth = 2;
    image.last_level = 3;
    info = construct(image);
    Require(info.resources.layers == 3 && !info.props.is_volume,
            "linear-general array lost layers");
    RequireMip(info, 0, 384, 8, 4, 0);
    RequireMip(info, 1, 96, 4, 2, 384);
    RequireMip(info, 2, 24, 2, 1, 480);
    RequireMip(info, 3, 12, 1, 1, 504);
    Require(info.guest_size == 516, "linear-general array byte extent changed");

    image.type = u64(AmdGpu::ImageType::Color3D);
    image.depth = 3;
    info = construct(image);
    Require(info.resources.layers == 1 && info.props.is_volume && info.size.depth == 4,
            "linear-general volume lost depth");
    RequireMip(info, 0, 512, 8, 4, 0);
    RequireMip(info, 1, 64, 4, 2, 512);
    RequireMip(info, 2, 8, 2, 1, 576);
    RequireMip(info, 3, 4, 1, 1, 584);
    Require(info.guest_size == 588, "linear-general volume mip extent changed");

    for (const auto format : {AmdGpu::DataFormat::FormatBc1, AmdGpu::DataFormat::FormatBc3}) {
        const u32 scale = format == AmdGpu::DataFormat::FormatBc1 ? 1 : 2;
        image = MakeLinearGeneralImage(16, 8, 16);
        image.data_format = u64(format);
        image.last_level = 4;
        info = construct(image);
        Require(info.props.is_block, "compressed descriptor lost block format");
        RequireMip(info, 0, 64 * scale, 16, 8, 0);
        RequireMip(info, 1, 16 * scale, 8, 4, 64 * scale);
        RequireMip(info, 2, 8 * scale, 4, 4, 80 * scale);
        RequireMip(info, 3, 8 * scale, 4, 4, 88 * scale);
        RequireMip(info, 4, 8 * scale, 4, 4, 96 * scale);
        Require(info.guest_size == 104 * scale,
                "linear-general compressed mip retained microtile padding");
    }

    // The existing aligned and microtiled layouts keep their previous padding.
    image = MakeLinearGeneralImage(13, 7, 19);
    image.tiling_index = u64(AmdGpu::TileMode::DisplayLinearAligned);
    info = construct(image);
    RequireMip(info, 0, 1792, 64, 7, 0);
    Require(!info.IsTiled(), "linear-aligned was reclassified as tiled");
    image.tiling_index = u64(AmdGpu::TileMode::Display1DThin);
    info = construct(image);
    RequireMip(info, 0, 768, 24, 8, 0);
    Require(info.IsTiled(), "microtiled was reclassified as linear");
    std::puts("IMAGE_LINEAR_GENERAL_CONTRACT_READY sampled_128m=1 formats=5 mip=1 array=1 volume=1 bc1_bc3=1 aligned_tiled_unchanged=1");
}

void CheckNullBindings() {
    struct FixtureInfo {
    std::array<u32, 16> user_data{};
        std::array<u32, 8> flattened_ud_buf{};
    } info;
    auto image = MakeImage();
    image.base_level = 2;
    image.last_level = 5;
    Shader::ImageResource resource{};
    resource.sharp_fetch.immediates = std::bit_cast<std::array<u32, 8>>(image);
    resource.mip_fallback_mode = Shader::MipStorageFallbackMode::DynamicIndex;
    using Binding = std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc>;
    std::vector<Binding> bindings;
    std::vector<u32> sizes;
    const u32 count = resource.NumBindings(info);
    Require(count == 4, "dynamic mip descriptor count changed");
    Vulkan::AppendNullImageBindings(resource, count, bindings, sizes);
    Require(bindings.size() == 4 && sizes == std::vector<u32>{4},
            "null fallback lost dynamic mip bindings");
    resource.is_written = true;
    Vulkan::AppendNullImageBindings(resource, 1, bindings, sizes);
    Require(bindings.size() == 5 && sizes == std::vector<u32>({4, 1}),
            "null fallback changed descriptor array order");
    for (u32 i = 0; i != bindings.size(); ++i) {
        const auto& [image_id, desc] = bindings[i];
        Require(!image_id, "null fallback created a real image");
        Require(desc.type == (i == 4 ? VideoCore::TextureCache::BindingType::Storage
                                    : VideoCore::TextureCache::BindingType::Texture),
                "null fallback lost sampled/storage descriptor type");
        Require(desc.info.pixel_format == vk::Format::eUndefined &&
                    desc.view_info.type == AmdGpu::ImageType::Color2D &&
                    desc.view_info.format == vk::Format::eR8G8B8A8Unorm,
                "null fallback interpreted an invalid descriptor");
    }
}

} // namespace

int main(int argc, char** argv) {
    EmulatorSettingsImpl::SetInstance(std::make_shared<EmulatorSettingsImpl>());
    if (argc == 2 && std::string_view{argv[1]} == "--linear-overflow") {
        // The u32 image-cache extent cannot represent a 4 GiB mip. Reject it
        // before multiplication or narrowing can wrap it into an empty image.
        auto image = MakeLinearGeneralImage(16384, 16384, 16384);
        image.data_format = u64(AmdGpu::DataFormat::Format32_32_32_32);
        image.num_format = u64(AmdGpu::NumberFormat::Float);
        const VideoCore::TextureCache::ImageDesc desc{image, Shader::ImageResource{}};
        std::printf("Unexpected oversized image construction: %u\n", desc.info.guest_size);
        return 2;
    }
    if (argc == 2 && std::string_view{argv[1]} == "--linear-mip-overflow") {
        auto image = MakeLinearGeneralImage(16384, 13000, 16384);
        image.data_format = u64(AmdGpu::DataFormat::Format32_32_32_32);
        image.num_format = u64(AmdGpu::NumberFormat::Float);
        image.last_level = 2;
        const VideoCore::TextureCache::ImageDesc desc{image, Shader::ImageResource{}};
        std::printf("Unexpected oversized mip chain: %u\n", desc.info.guest_size);
        return 2;
    }
    if (argc == 2 && std::string_view{argv[1]} == "--unguarded") {
        // Reproduce the previous call path: the conversion assertion must remain active.
        const VideoCore::TextureCache::ImageDesc desc{MakeImage(), Shader::ImageResource{}};
        std::printf("Unexpected unsupported image construction: %u\n", u32(desc.info.pixel_format));
        return 2;
    }
    try {
        CheckFormats();
        CheckBindingValidation();
        CheckLinearGeneralLayouts();
        CheckNullBindings();
        std::puts("Image binding format, mapping, null and storage/mip contracts PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Image binding contract FAIL: %s\n", error.what());
        return 1;
    }
}
