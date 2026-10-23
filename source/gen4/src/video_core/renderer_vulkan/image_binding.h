// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <tuple>
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

template <typename IsMapped>
bool IsImageBindingValid(const AmdGpu::Image& image, IsMapped&& is_mapped) {
    const auto data_format = image.GetDataFmt();
    const auto number_format = image.GetNumberFmt();
    if (image.Address() == 0 || data_format == AmdGpu::DataFormat::FormatInvalid) {
        return false;
    }
    constexpr u64 valid_data_formats = 0xfffff3ff007f7ffe;
    return image.Address() < (u64{1} << 40) && is_mapped(image.Address(), 1) &&
           u32(data_format) <= 63 && (valid_data_formats & (u64{1} << u32(data_format))) != 0 &&
           u32(number_format) <= 13 && u32(number_format) != 8 &&
           LiverpoolToVK::IsSurfaceFormatSupported(data_format, number_format);
}

template <typename IsMapped>
bool IsIndexedImageBindingValid(const AmdGpu::Image& image,
                                AmdGpu::ImageType expected_view_type, bool expected_integer,
                                bool is_array, IsMapped&& is_mapped) {
    const auto data_format = image.GetDataFmt();
    const auto number_format = image.GetNumberFmt();
    constexpr u64 valid_data_formats = 0xfffff3ff007f7ffe;
    if (image.Address() == 0 || data_format == AmdGpu::DataFormat::FormatInvalid ||
        u32(data_format) > 63 || (valid_data_formats & (u64{1} << u32(data_format))) == 0 ||
        u32(number_format) > 13 || u32(number_format) == 8 ||
        !LiverpoolToVK::IsSurfaceFormatSupported(data_format, number_format)) {
        return false;
    }
    return image.Address() < (u64{1} << 40) && is_mapped(image.Address(), 1) &&
           image.GetViewType(is_array) == expected_view_type &&
           ((u32(number_format) & 0xe) == 4) == expected_integer;
}

template <typename ImageBindings, typename ArraySizes>
void AppendNullImageBindings(const Shader::ImageResource& resource, u32 num_bindings,
                             ImageBindings& image_bindings, ArraySizes& array_sizes) {
    for (u32 i = 0; i < num_bindings; ++i) {
        auto& [image_id, desc] = image_bindings.emplace_back(
            std::piecewise_construct, std::tuple{}, std::tuple{});
        desc.type = resource.is_written ? VideoCore::TextureCache::BindingType::Storage
                                        : VideoCore::TextureCache::BindingType::Texture;
    }
    array_sizes.push_back(num_bindings);
}

} // namespace Vulkan
