// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <png.h>
#include <vector>
#include "common/logging/log.h"
#include "core/libraries/libpng/png_codec.h"
#include "core/libraries/libpng/pngdec.h"
#include "core/libraries/libpng/pngdec_error.h"
#include "core/libraries/libs.h"

namespace Libraries::PngDec {
namespace {
Png::Registry registry;
struct Reader {
    const u8* data;
    size_t size;
    size_t offset{};
};

void Read(png_structp png, png_bytep output, png_size_t size) {
    auto& reader = *static_cast<Reader*>(png_get_io_ptr(png));
    if (size > reader.size - reader.offset) png_error(png, "Truncated PNG");
    std::memcpy(output, reader.data + reader.offset, size);
    reader.offset += size;
}

OrbisPngDecImageInfo ImageInfo(png_structp png, png_infop info) {
    OrbisPngDecImageInfo result{};
    result.image_width = png_get_image_width(png, info);
    result.image_height = png_get_image_height(png, info);
    result.bit_depth = png_get_bit_depth(png, info);
    switch (png_get_color_type(png, info)) {
    case PNG_COLOR_TYPE_GRAY: result.color_space = OrbisPngDecColorSpace::Grayscale; break;
    case PNG_COLOR_TYPE_GRAY_ALPHA: result.color_space = OrbisPngDecColorSpace::GrayscaleAlpha; break;
    case PNG_COLOR_TYPE_PALETTE: result.color_space = OrbisPngDecColorSpace::Clut; break;
    case PNG_COLOR_TYPE_RGB: result.color_space = OrbisPngDecColorSpace::Rgb; break;
    case PNG_COLOR_TYPE_RGBA: result.color_space = OrbisPngDecColorSpace::Rgba; break;
    default: png_error(png, "Unsupported PNG color type");
    }
    if (png_get_interlace_type(png, info) == PNG_INTERLACE_ADAM7)
        result.image_flag |= OrbisPngDecImageFlag::Adam7Interlace;
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        result.image_flag |= OrbisPngDecImageFlag::TrnsChunkExist;
    return result;
}

// A header-only call and a complete decode both own their libpng state. Error
// recovery never jumps over C++ destructors and never leaves a poisoned handle.
s32 ReadImage(Reader& reader, const OrbisPngDecDecodeParam* param, u8* pixels,
              OrbisPngDecImageInfo& output) {
    auto* png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) return ORBIS_PNG_DEC_ERROR_FATAL;
    auto* info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        return ORBIS_PNG_DEC_ERROR_FATAL;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        return ORBIS_PNG_DEC_ERROR_INVALID_DATA;
    }
    png_set_read_fn(png, &reader, Read);
    png_set_user_limits(png, 1000001, 1000001);
    png_read_info(png, info);
    output = ImageInfo(png, info);
    if (param) {
        const int color = png_get_color_type(png, info);
        const bool transparency = png_get_valid(png, info, PNG_INFO_tRNS);
        if (output.bit_depth == 16) png_set_strip_16(png);
        if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
        if (color == PNG_COLOR_TYPE_GRAY && output.bit_depth < 8)
            png_set_expand_gray_1_2_4_to_8(png);
        if (transparency) png_set_tRNS_to_alpha(png);
        if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
            png_set_gray_to_rgb(png);
        if (!(color & PNG_COLOR_MASK_ALPHA) && !transparency)
            png_set_add_alpha(png, param->alpha_value & 255, PNG_FILLER_AFTER);
        if (param->pixel_format == OrbisPngDecPixelFormat::B8G8R8A8) png_set_bgr(png);
        const int passes = png_set_interlace_handling(png);
        png_read_update_info(png, info);
        const size_t row_size = size_t{output.image_width} * 4;
        if (png_get_rowbytes(png, info) != row_size)
            png_error(png, "Unexpected decoded row size");
        for (int pass = 0; pass < passes; ++pass) {
            for (u32 y = 0; y < output.image_height; ++y)
                png_read_row(png, pixels + y * row_size, nullptr);
        }
        png_read_end(png, info);
    }
    png_destroy_read_struct(&png, &info, nullptr);
    return ORBIS_OK;
}

s32 ValidateCreate(const OrbisPngDecCreateParam* param) {
    if (!Png::RangeValid(param, sizeof(*param))) return ORBIS_PNG_DEC_ERROR_INVALID_PARAM;
    if (param->this_size != sizeof(*param) || param->attribute > 1)
        return ORBIS_PNG_DEC_ERROR_INVALID_PARAM;
    if (!param->max_image_width || param->max_image_width > 1000001)
        return ORBIS_PNG_DEC_ERROR_INVALID_SIZE;
    return Png::WorkspaceSize;
}
} // namespace

s32 PS4_SYSV_ABI scePngDecCreate(const OrbisPngDecCreateParam* param, void* memoryAddress,
                                u32 memorySize, OrbisPngDecHandle* handle) {
    const auto result = ValidateCreate(param);
    if (result < 0) return result;
    if (!Png::RangeValid(memoryAddress, Png::WorkspaceSize, true) ||
        !Png::RangeValid(handle, sizeof(*handle), true)) return ORBIS_PNG_DEC_ERROR_INVALID_ADDR;
    if (memorySize < Png::WorkspaceSize) return ORBIS_PNG_DEC_ERROR_INVALID_SIZE;
    try {
        if (!registry.Create(memoryAddress, param->max_image_width))
            return ORBIS_PNG_DEC_ERROR_INVALID_HANDLE;
    } catch (const std::bad_alloc&) {
        return ORBIS_PNG_DEC_ERROR_FATAL;
    }
    *handle = memoryAddress;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePngDecDecode(OrbisPngDecHandle handle, const OrbisPngDecDecodeParam* param,
                                OrbisPngDecImageInfo* imageInfo) {
    auto state = registry.Find(handle);
    if (!state) return ORBIS_PNG_DEC_ERROR_INVALID_HANDLE;
    std::scoped_lock lock{state->mutex};
    if (!state->alive) return ORBIS_PNG_DEC_ERROR_INVALID_HANDLE;
    if (!Png::RangeValid(param, sizeof(*param))) return ORBIS_PNG_DEC_ERROR_INVALID_PARAM;
    const auto input = *param;
    if (!Png::RangeValid(input.png_mem_addr, input.png_mem_size) ||
        !Png::RangeValid(input.image_mem_addr, input.image_mem_size, true) ||
        (imageInfo && !Png::RangeValid(imageInfo, sizeof(*imageInfo), true)))
        return ORBIS_PNG_DEC_ERROR_INVALID_ADDR;
    if (input.png_mem_size < 8 || input.png_mem_size > Png::MaxStagingBytes)
        return ORBIS_PNG_DEC_ERROR_INVALID_SIZE;
    if (input.pixel_format != OrbisPngDecPixelFormat::R8G8B8A8 &&
        input.pixel_format != OrbisPngDecPixelFormat::B8G8R8A8)
        return ORBIS_PNG_DEC_ERROR_INVALID_PARAM;
    if (Png::Overlaps(input.image_mem_addr, input.image_mem_size, imageInfo, sizeof(*imageInfo)) ||
        Png::Overlaps(input.image_mem_addr, input.image_mem_size, param, sizeof(*param)) ||
        Png::Overlaps(input.image_mem_addr, input.image_mem_size, input.png_mem_addr, input.png_mem_size) ||
        Png::Overlaps(input.image_mem_addr, input.image_mem_size, handle, Png::WorkspaceSize) ||
        Png::Overlaps(imageInfo, sizeof(*imageInfo), input.png_mem_addr, input.png_mem_size) ||
        Png::Overlaps(imageInfo, sizeof(*imageInfo), param, sizeof(*param)) ||
        Png::Overlaps(imageInfo, sizeof(*imageInfo), handle, Png::WorkspaceSize))
        return ORBIS_PNG_DEC_ERROR_INVALID_PARAM;
    try {
        const Png::StagingReservation input_staging{u64{input.png_mem_size} + 1_MB};
        std::vector<u8> encoded(input.png_mem_addr, input.png_mem_addr + input.png_mem_size);
        Reader reader{encoded.data(), encoded.size()};
        OrbisPngDecImageInfo info{};
        auto result = ReadImage(reader, nullptr, nullptr, info);
        if (result < 0) return result;
        const u64 row_size = u64{info.image_width} * 4;
        const u64 pitch = input.image_pitch ? input.image_pitch : row_size;
        const u64 output_size = (u64{info.image_height} - 1) * pitch + row_size;
        const u64 packed_size = row_size * info.image_height;
        if (info.image_width > state->max_width || pitch < row_size ||
            output_size > input.image_mem_size ||
            packed_size + encoded.size() > Png::MaxStagingBytes)
            return ORBIS_PNG_DEC_ERROR_INVALID_SIZE;
        const Png::StagingReservation output_staging{packed_size};
        std::vector<u8> pixels(packed_size);
        reader.offset = 0;
        result = ReadImage(reader, &input, pixels.data(), info);
        if (result < 0) return result;
        for (u32 y = 0; y < info.image_height; ++y)
            std::memcpy(input.image_mem_addr + y * pitch, pixels.data() + y * row_size, row_size);
        if (imageInfo) *imageInfo = info;
        return (info.image_width > 32767 || info.image_height > 32767) ? 0 :
            static_cast<s32>((info.image_width << 16) | info.image_height);
    } catch (const std::bad_alloc&) {
        return ORBIS_PNG_DEC_ERROR_FATAL;
    }
}

s32 PS4_SYSV_ABI scePngDecDecodeWithInputControl() {
    LOG_ERROR(Lib_Png, "(STUBBED)called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePngDecDelete(OrbisPngDecHandle handle) {
    return registry.Delete(handle) ? ORBIS_OK : ORBIS_PNG_DEC_ERROR_INVALID_HANDLE;
}

s32 PS4_SYSV_ABI scePngDecParseHeader(const OrbisPngDecParseParam* param,
                                     OrbisPngDecImageInfo* imageInfo) {
    if (!Png::RangeValid(param, sizeof(*param))) return ORBIS_PNG_DEC_ERROR_INVALID_PARAM;
    const auto input = *param;
    if (!Png::RangeValid(input.png_mem_addr, input.png_mem_size) ||
        !Png::RangeValid(imageInfo, sizeof(*imageInfo), true)) return ORBIS_PNG_DEC_ERROR_INVALID_ADDR;
    if (input.png_mem_size < 8) return ORBIS_PNG_DEC_ERROR_INVALID_DATA;
    Reader reader{input.png_mem_addr, input.png_mem_size};
    OrbisPngDecImageInfo info{};
    const auto result = ReadImage(reader, nullptr, nullptr, info);
    if (result == ORBIS_OK) *imageInfo = info;
    return result;
}

s32 PS4_SYSV_ABI scePngDecQueryMemorySize(const OrbisPngDecCreateParam* param) {
    return ValidateCreate(param);
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("m0uW+8pFyaw", "libScePngDec", 1, "libScePngDec", scePngDecCreate);
    LIB_FUNCTION("WC216DD3El4", "libScePngDec", 1, "libScePngDec", scePngDecDecode);
    LIB_FUNCTION("cJ--1xAbj-I", "libScePngDec", 1, "libScePngDec", scePngDecDecodeWithInputControl);
    LIB_FUNCTION("QbD+eENEwo8", "libScePngDec", 1, "libScePngDec", scePngDecDelete);
    LIB_FUNCTION("U6h4e5JRPaQ", "libScePngDec", 1, "libScePngDec", scePngDecParseHeader);
    LIB_FUNCTION("-6srIGbLTIU", "libScePngDec", 1, "libScePngDec", scePngDecQueryMemorySize);
    LIB_FUNCTION("cJ--1xAbj-I", "libScePngDec_jvm", 1, "libScePngDec",
                 scePngDecDecodeWithInputControl);
};

} // namespace Libraries::PngDec
