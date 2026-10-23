// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/libraries/libpng/pngenc.h"
#include "core/libraries/libpng/pngenc_error.h"
#include "core/libraries/libpng/pngdec.h"
#include "core/libraries/libpng/pngdec_error.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/fex_hle_bridge.h"
#endif

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)

int main() {
    namespace Enc = Libraries::PngEnc;
    namespace Dec = Libraries::PngDec;
    alignas(8) std::array<u8, 16> enc_work{}, dec_work{};
    Enc::OrbisPngEncCreateParam create_enc{16, 0, 1024, 5};
    Dec::OrbisPngDecCreateParam create_dec{12, 0, 1024};
    Enc::OrbisPngEncHandle encoder{};
    Dec::OrbisPngDecHandle decoder{};
    CHECK(Enc::scePngEncQueryMemorySize(&create_enc) == 16);
    CHECK(Dec::scePngDecQueryMemorySize(&create_dec) == 16);
    CHECK(Enc::scePngEncCreate(&create_enc, enc_work.data(), enc_work.size(), &encoder) == 0);
    CHECK(Dec::scePngDecCreate(&create_dec, dec_work.data(), dec_work.size(), &decoder) == 0);
    CHECK(encoder == enc_work.data() && decoder == dec_work.data());
    // Padded two-row BGRA source. Padding has a conspicuous non-image color.
    const std::array<u8, 24> source{3,2,1,17, 6,5,4,34, 255,0,255,255,
                                   9,8,7,51, 12,11,10,68, 255,0,255,255};
    std::array<u8, 1024> png{};
    Enc::OrbisPngEncEncodeParam encode{source.data(), png.data(), source.size(), png.size(),
        2, 2, 12, Enc::OrbisPngEncPixelFormat::B8G8R8A8,
        Enc::OrbisPngEncColorSpace::RGBA, 8, 0, 0, 1};
    Enc::OrbisPngEncOutputInfo encoded_info{};
    std::vector<u8> valid;
    // A handle is reused for successive thumbnails, changing filter/compression
    // and RGB/RGBA output. Both decoder and encoder must start a new PNG stream.
    for (u16 iteration = 0; iteration < 12; ++iteration) {
        encode.filter_type = iteration % 2 ? 15 : 0;
        encode.compression_level = iteration % 10;
        const bool alpha = iteration % 2 == 0;
        encode.color_space = alpha ? Enc::OrbisPngEncColorSpace::RGBA : Enc::OrbisPngEncColorSpace::RGB;
        const s32 bytes = Enc::scePngEncEncode(encoder, &encode, &encoded_info);
        CHECK(bytes > 0 && encoded_info.data_size == u32(bytes) && encoded_info.processed_height == 2);
        valid.assign(png.begin(), png.begin() + bytes);
        Dec::OrbisPngDecParseParam parse{png.data(), u32(bytes), 0};
        Dec::OrbisPngDecImageInfo image_info{};
        CHECK(Dec::scePngDecParseHeader(&parse, &image_info) == 0);
        CHECK(image_info.image_width == 2 && image_info.image_height == 2 && image_info.bit_depth == 8);
        std::array<u8, 28> pixels;
        pixels.fill(0xa5);
        Dec::OrbisPngDecDecodeParam decode{png.data(), pixels.data() + 4, u32(bytes), 20,
            Dec::OrbisPngDecPixelFormat::R8G8B8A8, 255, 12};
        CHECK(Dec::scePngDecDecode(decoder, &decode, &image_info) == 0x00020002);
        const std::array<u8,16> expected{1,2,3,u8(alpha ? 17 : 255), 4,5,6,u8(alpha ? 34 : 255),
                                       7,8,9,u8(alpha ? 51 : 255), 10,11,12,u8(alpha ? 68 : 255)};
        CHECK(std::memcmp(pixels.data()+4, expected.data(), 8) == 0);
        CHECK(std::memcmp(pixels.data()+16, expected.data()+8, 8) == 0);
        for (size_t i : {0,1,2,3,12,13,14,15,24,25,26,27}) CHECK(pixels[i] == 0xa5);
        decode.image_mem_size = 19;
        pixels.fill(0x6b);
        CHECK(Dec::scePngDecDecode(decoder, &decode, nullptr) == ORBIS_PNG_DEC_ERROR_INVALID_SIZE);
        CHECK(std::all_of(pixels.begin(), pixels.end(), [](u8 b) { return b == 0x6b; }));
    }
    // Every truncation is contained and leaves both output records unchanged.
    for (size_t length = 8; length < valid.size(); ++length) {
        std::array<u8, 16> output;
        output.fill(0x7c);
        Dec::OrbisPngDecImageInfo info{111,222,Dec::OrbisPngDecColorSpace::Rgba,7,{}};
        const auto original_info = info;
        Dec::OrbisPngDecDecodeParam decode{valid.data(), output.data(), u32(length), output.size(),
            Dec::OrbisPngDecPixelFormat::R8G8B8A8, 255, 0};
        CHECK(Dec::scePngDecDecode(decoder, &decode, &info) < 0);
        CHECK(std::all_of(output.begin(), output.end(), [](u8 b) { return b == 0x7c; }));
        CHECK(std::memcmp(&info, &original_info, sizeof(info)) == 0);
    }
    // A corrupted IDAT CRC is rejected after decoding without publishing pixels.
    auto corrupt = valid;
    for (size_t i = 8; i + 12 <= corrupt.size();) {
        const u32 size = (u32(corrupt[i]) << 24) | (u32(corrupt[i+1]) << 16) |
                         (u32(corrupt[i+2]) << 8) | corrupt[i+3];
        if (std::memcmp(corrupt.data()+i+4, "IDAT", 4) == 0) {
            corrupt[i+8+size] ^= 1;
            break;
        }
        i += 12 + size;
    }
    std::array<u8, 16> output;
    output.fill(0x6e);
    Dec::OrbisPngDecDecodeParam decode{corrupt.data(), output.data(), u32(corrupt.size()), output.size(),
        Dec::OrbisPngDecPixelFormat::R8G8B8A8, 255, 0};
    CHECK(Dec::scePngDecDecode(decoder, &decode, nullptr) < 0);
    CHECK(std::all_of(output.begin(), output.end(), [](u8 b) { return b == 0x6e; }));
    decode.png_mem_addr = valid.data();
    CHECK(Dec::scePngDecDecode(decoder, &decode, nullptr) == 0x00020002);
    png.fill(0x3d);
    encoded_info = {99, 98};
    encode.png_mem_size = 12;
    CHECK(Enc::scePngEncEncode(encoder, &encode, &encoded_info) == ORBIS_PNG_ENC_ERROR_DATA_OVERFLOW);
    CHECK(std::all_of(png.begin(), png.end(), [](u8 b) { return b == 0x3d; }));
    CHECK(encoded_info.data_size == 99 && encoded_info.processed_height == 98);
    encode.png_mem_size = png.size();
    CHECK(Enc::scePngEncEncode(encoder, &encode, &encoded_info) > 0);
    CHECK(Enc::scePngEncEncode(encoder, nullptr, nullptr) == ORBIS_PNG_ENC_ERROR_INVALID_PARAM);
    png.fill(0x4a);
    CHECK(Enc::scePngEncEncode(encoder, &encode,
        reinterpret_cast<Enc::OrbisPngEncOutputInfo*>(png.data())) == ORBIS_PNG_ENC_ERROR_INVALID_PARAM);
    CHECK(std::all_of(png.begin(), png.end(), [](u8 b) { return b == 0x4a; }));
    output.fill(0x5b);
    CHECK(Dec::scePngDecDecode(decoder, &decode,
        reinterpret_cast<Dec::OrbisPngDecImageInfo*>(output.data())) == ORBIS_PNG_DEC_ERROR_INVALID_PARAM);
    CHECK(std::all_of(output.begin(), output.end(), [](u8 b) { return b == 0x5b; }));
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    // Invoke the real bridge: nested buffers must use the active guest range
    // validator, not merely pass the typed adapter's parameter-block check.
    Core::GuestCpu::HleCallRegistry calls;
    auto enc_adapter = calls.Register(Core::GuestCpu::MakeHleCallAdapter(Enc::scePngEncEncode), "png-encode");
    auto dec_adapter = calls.Register(Core::GuestCpu::MakeHleCallAdapter(Dec::scePngDecDecode), "png-decode");
    struct Denied { uintptr_t address; bool writes_only; } denied{};
    Core::GuestCpu::HleGuestBridge bridge(calls,
        [](void* context, uintptr_t address, size_t, bool write) {
            const auto& deny = *static_cast<Denied*>(context);
            return address != deny.address || (deny.writes_only && !write);
        }, &denied);
    const auto invoke = [&](const auto& adapter, void* handle, const void* parameter) {
        Core::GuestCpu::HleCallFrame frame{};
        frame.operation = adapter->Operation();
        frame.gpr[7] = reinterpret_cast<uintptr_t>(handle);
        frame.gpr[6] = reinterpret_cast<uintptr_t>(parameter);
        CHECK(std::holds_alternative<bool>(bridge.Invoke(frame)));
        return static_cast<s32>(frame.gpr[0]);
    };
    denied = {0x10000, false};
    encode.image_mem_addr = reinterpret_cast<const u8*>(denied.address);
    CHECK(invoke(enc_adapter, encoder, &encode) == ORBIS_PNG_ENC_ERROR_INVALID_ADDR);
    encode.image_mem_addr = source.data();
    denied = {reinterpret_cast<uintptr_t>(png.data()), true};
    CHECK(invoke(enc_adapter, encoder, &encode) == ORBIS_PNG_ENC_ERROR_INVALID_ADDR);
    denied = {0x10000, false};
    decode.png_mem_addr = reinterpret_cast<const u8*>(denied.address);
    CHECK(invoke(dec_adapter, decoder, &decode) == ORBIS_PNG_DEC_ERROR_INVALID_ADDR);
    decode.png_mem_addr = valid.data();
    denied = {reinterpret_cast<uintptr_t>(output.data()), true};
    CHECK(invoke(dec_adapter, decoder, &decode) == ORBIS_PNG_DEC_ERROR_INVALID_ADDR);
#endif
    CHECK(Dec::scePngDecDelete(decoder) == 0);
    CHECK(Dec::scePngDecDelete(decoder) == ORBIS_PNG_DEC_ERROR_INVALID_HANDLE);
    CHECK(Dec::scePngDecDecode(decoder, &decode, nullptr) == ORBIS_PNG_DEC_ERROR_INVALID_HANDLE);
    CHECK(Enc::scePngEncDelete(encoder) == 0);
    CHECK(Enc::scePngEncDelete(encoder) == ORBIS_PNG_ENC_ERROR_INVALID_HANDLE);
    // The same allocator workspace can be used for a fresh decoder lifecycle.
    CHECK(Dec::scePngDecCreate(&create_dec, dec_work.data(), dec_work.size(), &decoder) == 0);
    CHECK(Dec::scePngDecDecode(decoder, &decode, nullptr) == 0x00020002);
    CHECK(Dec::scePngDecDelete(decoder) == 0);
    std::puts("png-codec=PASS repeated-thumbnail/BGRA/RGB/alpha/pitch/canary/truncation/CRC/overflow/recovery/delete/alias/FEX-nested-ranges");
}
