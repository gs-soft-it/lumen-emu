// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "shader_recompiler/ir/type.h"
#include "video_core/amdgpu/resource.h"

#include <boost/container/static_vector.hpp>
#include <boost/container/small_vector.hpp>

namespace Shader {

static constexpr u32 NUM_USER_DATA_REGS = 16;
static constexpr u32 NUM_IMAGES = 64;
static constexpr u32 NUM_BUFFERS = 40;
static constexpr u32 NUM_SAMPLERS = 32;
static constexpr u32 NUM_FMASKS = 8;

using SharpLocation = u16;

constexpr SharpLocation UNKNOWN_LOCATION = std::numeric_limits<u16>::max();

template <typename T>
struct SharpFetch {
    static constexpr std::size_t N = sizeof(T) / sizeof(u32);
    static_assert(N <= 8);

    std::array<u32, N> immediates;
    std::array<SharpLocation, N> offsets;
    u8 load_mask;

    bool operator==(const SharpFetch&) const = default;

    template <u32 num_dwords = N>
        requires(num_dwords <= N)
    constexpr bool Fetch(const u32* flatbuf, T* out) const {
        u8 mask = load_mask;
        for (u32 i = 0; i < num_dwords; i++) {
            if (offsets[i] == UNKNOWN_LOCATION) {
                return false;
            }
        }
        std::array<u32, num_dwords> out_dw;
        for (u32 i = 0; i < num_dwords; i++) {
            out_dw[i] = (mask & 1) ? flatbuf[offsets[i]] : immediates[i];
            mask >>= 1;
        }
        std::memcpy(out, out_dw.data(), sizeof(out_dw));
        return true;
    }
};

enum class SharpFetchPostOp : u8 {
    None,
    // For buffers
    BitwiseOrDw1WithImm,
    OffsetByProgramBase,
    // For images,
    ConvertCubeTo2DArray,
    // For samplers
    DisableAnisoIfSingleLod,
    ForceRepeatXyzClamp,
    ForceLastTexelXyClamp,
    ClearAnisoRatioAndThreshold,
};

enum class BufferType : u8 {
    Guest,
    Flatbuf,
    BdaPagetable,
    FaultBuffer,
    GdsBuffer,
    SharedMemory,
    ClipPlanes,
};

struct BufferResource {
    SharpFetch<AmdGpu::Buffer> sharp_fetch{};
    IR::Type used_types{};
    BufferType buffer_type{};
    bool is_written{};
    bool is_formatted{};
    bool used_as_readconst{};
    SharpFetchPostOp post_op{};
    u32 post_op_dw1_mask{};

    bool IsSpecial() const noexcept {
        return buffer_type != BufferType::Guest;
    }

    bool IsStorage([[maybe_unused]] const AmdGpu::Buffer buffer) const noexcept {
        // When using uniform buffers, a size is required at compilation time, so we need to
        // either compile a lot of shader specializations to handle each size or just force it to
        // the maximum possible size always. However, for some vendors the shader-supplied size is
        // used for bounds checking uniform buffer accesses, so the latter would effectively turn
        // off buffer robustness behavior. Instead, force storage buffers which are bounds checked
        // using the actual buffer size. We are assuming the performance hit from this is
        // acceptable.
        return true; // buffer.GetSize() > profile.max_ubo_size || is_written;
    }

    constexpr AmdGpu::Buffer GetSharp(const auto& info) const noexcept {
        AmdGpu::Buffer buffer;
        if (!sharp_fetch.Fetch(info.flattened_ud_buf.data(), &buffer)) {
            return AmdGpu::Buffer::Null();
        }
        if (post_op == SharpFetchPostOp::BitwiseOrDw1WithImm) {
            reinterpret_cast<u32*>(&buffer)[1] |= post_op_dw1_mask;
        } else if (post_op == SharpFetchPostOp::OffsetByProgramBase) {
            buffer.base_address += info.pgm_base;
        }
        if (!buffer.Valid()) {
            return AmdGpu::Buffer::Null();
        }
        if (buffer.data_format == 15 ||
            (buffer.num_format == 6 && buffer.data_format != 1 && buffer.data_format != 2 &&
             buffer.data_format != 3 && buffer.data_format != 5 && buffer.data_format != 10 &&
             buffer.data_format != 12)) {
            buffer.num_format = 0;
            buffer.data_format = 0;
        }
        const auto sanitize_selector = [](u32 selector) {
            return selector == 2 || selector == 3 ? 0u : selector;
        };
        buffer.dst_sel_x = sanitize_selector(buffer.dst_sel_x);
        buffer.dst_sel_y = sanitize_selector(buffer.dst_sel_y);
        buffer.dst_sel_z = sanitize_selector(buffer.dst_sel_z);
        buffer.dst_sel_w = sanitize_selector(buffer.dst_sel_w);
        return buffer;
    }
};
using BufferResourceList = boost::container::static_vector<BufferResource, NUM_BUFFERS>;

enum class MipStorageFallbackMode : u16 {
    None,
    DynamicIndex,
    ConstantIndex,
};

struct ImageResource {
    static constexpr f32 Hosted2DSampleY = 0.5f;
    static constexpr u32 Hosted2DIntegerY = 0u;


    SharpFetch<AmdGpu::Image> sharp_fetch{};
    bool is_depth{};
    bool is_atomic{};
    bool is_array{};
    bool is_written{};
    bool is_r128{};
    u8 constant_mip_index{};
    MipStorageFallbackMode mip_fallback_mode{};
    SharpFetchPostOp post_op{};
    u8 table_ud_reg{};
    u8 table_count{};
    u16 table_stride{};
    u32 table_offset{};

    bool Is1DHostedAs2D(const AmdGpu::Image& sharp) const noexcept {
        return !is_array && !is_written && !is_depth && !is_atomic && !is_r128 &&
               sharp.GetType() == AmdGpu::ImageType::Color1D && sharp.height == 0 &&
               sharp.NumLayers() == 1 && sharp.NumLevels() == 1 && sharp.NumSamples() == 1 &&
               sharp.GetTileMode() == AmdGpu::TileMode::Thin2DThin;
    }

    AmdGpu::ImageType GetHostViewType(const AmdGpu::Image& sharp) const noexcept {
        return Is1DHostedAs2D(sharp) ? AmdGpu::ImageType::Color2D
                                    : sharp.GetViewType(is_array);
    }

    constexpr AmdGpu::Image GetIndexedSharp(const auto& info, u32 index) const noexcept {
        if (info.user_data.size() <= u32(table_ud_reg) + 1) {
            return AmdGpu::Image::Null(is_depth);
        }
        const u64 table_address = u64(info.user_data[table_ud_reg]) |
                                  (u64(info.user_data[table_ud_reg + 1]) << 32);
        if (table_address == 0) {
            return AmdGpu::Image::Null(is_depth);
        }
        AmdGpu::Image image{};
        const u64 address = table_address + table_offset + u64(table_stride) * index;
        std::memcpy(&image, reinterpret_cast<const void*>(address), sizeof(u64));
        if (image.base_address == 0) {
            return AmdGpu::Image::Null(is_depth);
        }
        std::memcpy(reinterpret_cast<u8*>(&image) + sizeof(u64),
                    reinterpret_cast<const void*>(address + sizeof(u64)),
                    sizeof(image) - sizeof(u64));
        if (post_op == SharpFetchPostOp::ConvertCubeTo2DArray) {
            image.type = u64(AmdGpu::ImageType::Color2DArray);
            image.depth = (image.depth + 1) * 6 - 1;
        }
        if (!image.Valid()) {
            return AmdGpu::Image::Null(is_depth);
        }
        if (is_depth && image.GetDataFmt() != AmdGpu::DataFormat::Format16 &&
            image.GetDataFmt() != AmdGpu::DataFormat::Format32) {
            return AmdGpu::Image::Null(true);
        }
        return image;
    }

    constexpr AmdGpu::Image GetSharp(const auto& info) const noexcept {
        if (table_count != 0) {
            for (u32 index = 0; index < table_count; ++index) {
                const auto image = GetIndexedSharp(info, index);
                if (image.base_address != 0) {
                    return image;
                }
            }
            return AmdGpu::Image::Null(is_depth);
        }
        AmdGpu::Image image{};
        if (!Fetch(info.flattened_ud_buf.data(), &image) || image.base_address == 0) {
            return AmdGpu::Image::Null(is_depth);
        }
        if (post_op == SharpFetchPostOp::ConvertCubeTo2DArray) {
            image.type = u64(AmdGpu::ImageType::Color2DArray);
            image.depth = (image.depth + 1) * 6 - 1;
        }
        if (!image.Valid()) {
            image = AmdGpu::Image::Null(is_depth);
        } else if (is_depth) {
            const auto data_fmt = image.GetDataFmt();
            if (data_fmt != AmdGpu::DataFormat::Format16 &&
                data_fmt != AmdGpu::DataFormat::Format32) {
                image = AmdGpu::Image::Null(true);
            }
        }
        return image;
    }

    constexpr bool Fetch(const u32* flatbuf, AmdGpu::Image* out) const {
        if (!is_r128) {
            // Fetch full 8 byte T#
            return sharp_fetch.Fetch(flatbuf, out);
        }
        // Fetch r128 T# and fix pitch
        if (!sharp_fetch.Fetch<4>(flatbuf, out)) {
            return false;
        }
        out->pitch = out->width;
        return true;
    }

    u32 NumBindings(const auto& info) const {
        if (table_count != 0) {
            return table_count;
        }
        const AmdGpu::Image tsharp = GetSharp(info);
        return (mip_fallback_mode == MipStorageFallbackMode::DynamicIndex)
                   ? (tsharp.last_level - tsharp.base_level + 1)
                   : 1;
    }
};
using ImageResourceList = boost::container::small_vector<ImageResource, NUM_IMAGES>;

struct SamplerResource {
    SharpFetch<AmdGpu::Sampler> sharp_fetch{};
    SharpFetchPostOp post_op{};
    SharpLocation post_op_tsharp_dw3_off{};

    constexpr AmdGpu::Sampler GetSharp(const auto& info) const noexcept {
        AmdGpu::Sampler sampler{};
        sharp_fetch.Fetch(info.flattened_ud_buf.data(), &sampler);
        if (post_op == SharpFetchPostOp::DisableAnisoIfSingleLod) {
            const u32 tsharp_dw3 = info.flattened_ud_buf[post_op_tsharp_dw3_off];
            if (((tsharp_dw3 >> 12) & 0xff) == 0) {
                sampler.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            }
        } else if (post_op == SharpFetchPostOp::ForceRepeatXyzClamp) {
            sampler.clamp_x.Assign(AmdGpu::ClampMode::Wrap);
            sampler.clamp_y.Assign(AmdGpu::ClampMode::Wrap);
            sampler.clamp_z.Assign(AmdGpu::ClampMode::Wrap);
        } else if (post_op == SharpFetchPostOp::ForceLastTexelXyClamp) {
            sampler.clamp_x.Assign(AmdGpu::ClampMode::ClampLastTexel);
            sampler.clamp_y.Assign(AmdGpu::ClampMode::ClampLastTexel);
            sampler.clamp_z.Assign(AmdGpu::ClampMode::Wrap);
        } else if (post_op == SharpFetchPostOp::ClearAnisoRatioAndThreshold) {
            sampler.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            sampler.aniso_threshold.Assign(0);
        }
        return sampler;
    }
};
using SamplerResourceList = boost::container::small_vector<SamplerResource, NUM_SAMPLERS>;

struct FMaskResource {
    SharpLocation sharp_idx;

    constexpr AmdGpu::Image GetSharp(const auto& info) const noexcept {
        return info.template ReadUdSharp<AmdGpu::Image>(sharp_idx);
    }
};
using FMaskResourceList = boost::container::static_vector<FMaskResource, NUM_FMASKS>;

struct PushData {
    static constexpr u32 XOffsetIndex = 0;
    static constexpr u32 YOffsetIndex = 1;
    static constexpr u32 XScaleIndex = 2;
    static constexpr u32 YScaleIndex = 3;
    static constexpr u32 UdRegsIndex = 4;
    static constexpr u32 BufOffsetIndex = UdRegsIndex + NUM_USER_DATA_REGS / 4;

    float xoffset;
    float yoffset;
    float xscale;
    float yscale;
    std::array<u32, NUM_USER_DATA_REGS> ud_regs;
    std::array<u8, NUM_BUFFERS> buf_offsets;

    void AddOffset(u32 binding, u32 offset) {
        ASSERT(offset < 256 && binding < buf_offsets.size());
        buf_offsets[binding] = offset;
    }
};
static_assert(sizeof(PushData) <= 128,
              "PushData size is greater than minimum size guaranteed by Vulkan spec");

} // namespace Shader
