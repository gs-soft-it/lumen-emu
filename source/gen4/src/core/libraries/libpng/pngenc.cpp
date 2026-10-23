// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <png.h>
#include <vector>
#include "common/logging/log.h"
#include "core/libraries/libpng/png_codec.h"
#include "core/libraries/libpng/pngenc.h"
#include "core/libraries/libpng/pngenc_error.h"
#include "core/libraries/libs.h"
#include "core/memory.h"

namespace Libraries::PngEnc {
namespace {
Png::Registry registry;

bool TraceEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("LUMEN_PNG_ENC_TRACE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
std::atomic<u32> trace_count{};

bool ReadbackEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("LUMEN_PNG_CPU_READBACK");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

struct Writer {
    u8* bytes;
    size_t capacity;
    size_t size{};
    bool overflow{};
};

void Write(png_structp png, png_bytep bytes, png_size_t size) {
    auto& writer = *static_cast<Writer*>(png_get_io_ptr(png));
    if (size > writer.capacity - writer.size) {
        writer.overflow = true;
        png_error(png, "PNG output buffer too small");
    }
    std::memcpy(writer.bytes + writer.size, bytes, size);
    writer.size += size;
}

int Filter(u16 filter) {
    if (filter == 0) return PNG_FILTER_NONE;
    int flags{};
    if (filter & 1) flags |= PNG_FILTER_SUB;
    if (filter & 2) flags |= PNG_FILTER_UP;
    if (filter & 4) flags |= PNG_FILTER_AVG;
    if (filter & 8) flags |= PNG_FILTER_PAETH;
    return flags;
}

// No C++ objects with destructors are created across libpng's longjmp region.
s32 Encode(const OrbisPngEncEncodeParam& param, const u8* pixels, Writer& writer) {
    auto* png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) return ORBIS_PNG_ENC_ERROR_FATAL;
    auto* info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, nullptr);
        return ORBIS_PNG_ENC_ERROR_FATAL;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        return writer.overflow ? ORBIS_PNG_ENC_ERROR_DATA_OVERFLOW : ORBIS_PNG_ENC_ERROR_FATAL;
    }
    const bool alpha = param.color_space == OrbisPngEncColorSpace::RGBA;
    png_set_write_fn(png, &writer, Write, nullptr);
    png_set_IHDR(png, info, param.image_width, param.image_height, 8,
                 alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_compression_level(png, param.compression_level);
    png_set_filter(png, PNG_FILTER_TYPE_BASE, Filter(param.filter_type));
    png_write_info(png, info);
    const size_t row_size = size_t{param.image_width} * (alpha ? 4 : 3);
    for (u32 y = 0; y < param.image_height; ++y) {
        png_write_row(png, const_cast<u8*>(pixels + y * row_size));
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    return static_cast<s32>(writer.size);
}

s32 ValidateCreate(const OrbisPngEncCreateParam* param) {
    if (!Png::RangeValid(param, sizeof(*param))) return ORBIS_PNG_ENC_ERROR_INVALID_ADDR;
    if (param->this_size != sizeof(*param) || param->attribute != 0 ||
        param->max_filter_number > 5) return ORBIS_PNG_ENC_ERROR_INVALID_PARAM;
    if (param->max_image_width == 0 || param->max_image_width > 1000001)
        return ORBIS_PNG_ENC_ERROR_INVALID_SIZE;
    return Png::WorkspaceSize;
}
} // namespace

s32 PS4_SYSV_ABI scePngEncCreate(const OrbisPngEncCreateParam* param, void* memoryAddress,
                                u32 memorySize, OrbisPngEncHandle* handle) {
    const auto result = ValidateCreate(param);
    if (result < 0) return result;
    if (!Png::RangeValid(memoryAddress, Png::WorkspaceSize, true) ||
        !Png::RangeValid(handle, sizeof(*handle), true)) return ORBIS_PNG_ENC_ERROR_INVALID_ADDR;
    if (memorySize < Png::WorkspaceSize) return ORBIS_PNG_ENC_ERROR_INVALID_SIZE;
    try {
        if (!registry.Create(memoryAddress, param->max_image_width))
            return ORBIS_PNG_ENC_ERROR_INVALID_HANDLE;
    } catch (const std::bad_alloc&) {
        return ORBIS_PNG_ENC_ERROR_FATAL;
    }
    *handle = memoryAddress;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePngEncDelete(OrbisPngEncHandle handle) {
    return registry.Delete(handle) ? ORBIS_OK : ORBIS_PNG_ENC_ERROR_INVALID_HANDLE;
}

s32 PS4_SYSV_ABI scePngEncEncode(OrbisPngEncHandle handle, const OrbisPngEncEncodeParam* param,
                                OrbisPngEncOutputInfo* outputInfo) {
    auto state = registry.Find(handle);
    if (!state) return ORBIS_PNG_ENC_ERROR_INVALID_HANDLE;
    std::scoped_lock lock{state->mutex};
    if (!state->alive) return ORBIS_PNG_ENC_ERROR_INVALID_HANDLE;
    if (!Png::RangeValid(param, sizeof(*param))) return ORBIS_PNG_ENC_ERROR_INVALID_PARAM;
    const auto input = *param;
    if (!input.image_mem_addr || !input.png_mem_addr ||
        (outputInfo && !Png::RangeValid(outputInfo, sizeof(*outputInfo), true)))
        return ORBIS_PNG_ENC_ERROR_INVALID_ADDR;
    if (!input.image_width || !input.image_height || !input.image_mem_size ||
        !input.png_mem_size || input.image_width > state->max_width)
        return ORBIS_PNG_ENC_ERROR_INVALID_SIZE;
    if ((input.pixel_format != OrbisPngEncPixelFormat::R8G8B8A8 &&
         input.pixel_format != OrbisPngEncPixelFormat::B8G8R8A8) ||
        (input.color_space != OrbisPngEncColorSpace::RGB &&
         input.color_space != OrbisPngEncColorSpace::RGBA) || input.bit_depth != 8 ||
        input.clut_number != 0 || input.compression_level > 9 || input.filter_type > 15)
        return ORBIS_PNG_ENC_ERROR_INVALID_PARAM;
    const u64 row_size = u64{input.image_width} * 4;
    const u64 pitch = input.image_pitch ? input.image_pitch : row_size;
    const u64 source_size = (u64{input.image_height} - 1) * pitch + row_size;
    const bool alpha = input.color_space == OrbisPngEncColorSpace::RGBA;
    const u64 packed_size = u64{input.image_width} * input.image_height * (alpha ? 4 : 3);
    // PNG deflate overhead is small; the extra row/chunk budget avoids reserving
    // an arbitrary guest capacity for a small thumbnail.
    const u64 output_size = std::min<u64>(input.png_mem_size,
        packed_size + packed_size / 8 + u64{input.image_height} * 16 + 65536);
    if (pitch < row_size || source_size > input.image_mem_size ||
        packed_size + output_size > Png::MaxStagingBytes)
        return ORBIS_PNG_ENC_ERROR_INVALID_SIZE;
    if (!Png::RangeValid(input.image_mem_addr, source_size) ||
        !Png::RangeValid(input.png_mem_addr, input.png_mem_size, true))
        return ORBIS_PNG_ENC_ERROR_INVALID_ADDR;
    if (Png::Overlaps(input.png_mem_addr, input.png_mem_size, outputInfo, sizeof(*outputInfo)) ||
        Png::Overlaps(input.png_mem_addr, input.png_mem_size, param, sizeof(*param)) ||
        Png::Overlaps(input.png_mem_addr, input.png_mem_size, input.image_mem_addr, source_size) ||
        Png::Overlaps(input.png_mem_addr, input.png_mem_size, handle, Png::WorkspaceSize) ||
        Png::Overlaps(outputInfo, sizeof(*outputInfo), input.image_mem_addr, source_size) ||
        Png::Overlaps(outputInfo, sizeof(*outputInfo), param, sizeof(*param)) ||
        Png::Overlaps(outputInfo, sizeof(*outputInfo), handle, Png::WorkspaceSize))
        return ORBIS_PNG_ENC_ERROR_INVALID_PARAM;
    try {
        const Png::StagingReservation staging{packed_size + output_size + 1_MB};
        const bool trace = TraceEnabled() && trace_count.load(std::memory_order_relaxed) < 16;
        if (trace || ReadbackEnabled()) {
            Core::Memory::Instance()->ReadGpuMemory(reinterpret_cast<VAddr>(input.image_mem_addr),
                                                   source_size, ReadbackEnabled(), trace);
        }
        std::vector<u8> pixels(packed_size);
        std::vector<u8> encoded(output_size);
        const bool bgra = input.pixel_format == OrbisPngEncPixelFormat::B8G8R8A8;
        u64 nonzero_rgb{}, nonzero_alpha{}, input_hash{14695981039346656037ull};
        for (u32 y = 0; y < input.image_height; ++y) {
            const auto* source = input.image_mem_addr + y * pitch;
            auto* target = pixels.data() + u64{y} * input.image_width * (alpha ? 4 : 3);
            for (u32 x = 0; x < input.image_width; ++x) {
                *target++ = source[bgra ? 2 : 0];
                *target++ = source[1];
                *target++ = source[bgra ? 0 : 2];
                if (alpha) *target++ = source[3];
                if (trace) {
                    nonzero_rgb += source[0] != 0 || source[1] != 0 || source[2] != 0;
                    nonzero_alpha += source[3] != 0;
                    for (u32 channel = 0; channel < 4; ++channel)
                        input_hash = (input_hash ^ source[channel]) * 1099511628211ull;
                }
                source += 4;
            }
        }
        Writer writer{encoded.data(), encoded.size()};
        const s32 result = Encode(input, pixels.data(), writer);
        if (result < 0) return result;
        if (trace && trace_count.fetch_add(1, std::memory_order_relaxed) < 16) {
            LOG_WARNING(Lib_Png, "[PNG_ENC_TRACE] image={:#x} width={} height={} pitch={} "
                        "format={} color={} source_bytes={} png_bytes={} nonzero_rgb={} "
                        "nonzero_alpha={} input_fnv1a64={:#x}",
                        reinterpret_cast<uintptr_t>(input.image_mem_addr), input.image_width,
                        input.image_height, pitch, u16(input.pixel_format), u16(input.color_space),
                        source_size, result, nonzero_rgb, nonzero_alpha, input_hash);
        }
        std::memcpy(input.png_mem_addr, encoded.data(), writer.size);
        if (outputInfo) *outputInfo = {static_cast<u32>(writer.size), input.image_height};
        return result;
    } catch (const std::bad_alloc&) {
        return ORBIS_PNG_ENC_ERROR_FATAL;
    }
}

s32 PS4_SYSV_ABI scePngEncQueryMemorySize(const OrbisPngEncCreateParam* param) {
    return ValidateCreate(param);
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("7aGTPfrqT9s", "libScePngEnc", 1, "libScePngEnc", scePngEncCreate);
    LIB_FUNCTION("RUrWdwTWZy8", "libScePngEnc", 1, "libScePngEnc", scePngEncDelete);
    LIB_FUNCTION("xgDjJKpcyHo", "libScePngEnc", 1, "libScePngEnc", scePngEncEncode);
    LIB_FUNCTION("9030RnBDoh4", "libScePngEnc", 1, "libScePngEnc", scePngEncQueryMemorySize);
};

} // namespace Libraries::PngEnc