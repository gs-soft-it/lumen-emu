// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "atrac9_test_data.h"
#include "core/libraries/ajm/ajm_at9.h"
#include "core/libraries/ajm/ajm_result.h"

using namespace Libraries::Ajm;

static void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

static void AppendU32(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}

static void AppendTag(std::vector<u8>& bytes, const char* tag) {
    bytes.insert(bytes.end(), tag, tag + 4);
}

static std::vector<u8> MakeRiff(std::span<const u8> encoded) {
    std::vector<u8> riff;
    AppendTag(riff, "RIFF");
    AppendU32(riff, 0);
    AppendTag(riff, "WAVE");
    AppendTag(riff, "JUNK");
    AppendU32(riff, 3);
    riff.insert(riff.end(), {1, 2, 3});
    // The extracted parser skips the exact chunk length, without RIFF padding.
    AppendTag(riff, "fmt ");
    AppendU32(riff, 52);
    std::array<u8, 52> format{};
    format[0] = 0xfe;
    format[1] = 0xff;
    constexpr std::array<u8, 16> guid{0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
                                     0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};
    std::copy(guid.begin(), guid.end(), format.begin() + 24);
    constexpr std::array<u8, 4> config{0xfe, 0x74, 0x1f, 0xf0};
    std::copy(config.begin(), config.end(), format.begin() + 44);
    riff.insert(riff.end(), format.begin(), format.end());
    AppendTag(riff, "fact");
    AppendU32(riff, 12);
    AppendU32(riff, 100);
    AppendU32(riff, 32);
    AppendU32(riff, 0);
    AppendTag(riff, "data");
    AppendU32(riff, static_cast<u32>(encoded.size()));
    riff.insert(riff.end(), encoded.begin(), encoded.end());
    const auto riff_size = static_cast<u32>(riff.size() - 8);
    std::memcpy(riff.data() + 4, &riff_size, sizeof(riff_size));
    return riff;
}

int main() {
    try {
        const auto flags = static_cast<AjmAt9CodecFlags>(0);
        AjmDecAt9InitializeParameters parameters{};
        constexpr std::array<u8, 4> config{0xfe, 0x74, 0x1f, 0xf0};
        std::copy(config.begin(), config.end(), parameters.config_data);
        const auto encoded = Ngs2TestMakeStereoAtrac9Superframe(0x4b595459u);
        for (const auto encoding : {AjmFormatEncoding::S16, AjmFormatEncoding::S32,
                                    AjmFormatEncoding::Float}) {
            AjmAt9Decoder decoder(encoding, flags, 2);
            std::array<u8, 8192> pcm{};
            std::array<std::span<u8>, 1> spans{std::span<u8>(pcm)};
            SparseOutputBuffer output(spans);
            AjmInstanceGapless gapless{};
            std::span<u8> empty;
            Check(decoder.GetMinimumInputSize() == 0 &&
                      decoder.ProcessData(empty, output, gapless).result ==
                          ORBIS_AJM_RESULT_NOT_INITIALIZED,
                  "initial decoder state");
            decoder.Initialize(&parameters, sizeof(parameters));
            AjmSidebandDecAt9CodecInfo info{};
            decoder.GetInfo(&info);
            Check(info.super_frame_size == 1024 && info.frames_in_super_frame == 4 &&
                      info.frame_samples == 256 && decoder.GetMinimumInputSize() == 1024,
                  "initialized AT9 geometry");
            Check(decoder.GetFormat().num_channels == 2, "initialized format");

            auto packet = encoded;
            std::span<u8> partial(packet.data(), packet.size() - 1);
            const auto first = decoder.ProcessData(partial, output, gapless);
            Check(first.result == 0 && first.frames_decoded == 1 &&
                      first.samples_written == 256 && partial.size() < 1023,
                  "target decodes first frame without requiring a full superframe");

            decoder.Reset();
            gapless.Reset();
            spans[0] = pcm;
            SparseOutputBuffer reset_output(spans);
            std::span<u8> input(packet);
            u32 samples = 0;
            for (u32 frame = 0; frame < 4; ++frame) {
                const auto result = decoder.ProcessData(input, reset_output, gapless);
                Check(result.result == 0 && result.frames_decoded == 1 &&
                          result.samples_written == 256,
                      "reset preserves initialization and decodes each frame");
                samples += result.samples_written;
            }
            Check(input.empty() && samples == 1024 && decoder.GetMinimumInputSize() == 1024,
                  "superframe consumption and reset geometry");
        }

        AjmAt9Decoder decoder(AjmFormatEncoding::S16, flags, 2);
        decoder.Initialize(&parameters, sizeof(parameters));
        auto packet = encoded;
        std::span<u8> input(packet);
        std::array<u8, 1024> pcm{};
        std::array<std::span<u8>, 1> spans{std::span<u8>(pcm)};
        SparseOutputBuffer output(spans);
        AjmInstanceGapless gapless{};
        gapless.init.total_samples = 100;
        gapless.Reset();
        const auto drained = decoder.ProcessData(input, output, gapless);
        Check(drained.result == 0 && drained.frames_decoded == 4 &&
                  drained.samples_written == 100 && input.empty(),
              "gapless completion drains the superframe");

        AjmAt9Decoder riff_decoder(AjmFormatEncoding::S16, AjmAt9CodecFlags::ParseRiffHeader, 2);
        auto riff = MakeRiff(encoded);
        input = riff;
        spans[0] = pcm;
        SparseOutputBuffer riff_output(spans);
        gapless = {};
        const auto parsed = riff_decoder.ProcessData(input, riff_output, gapless);
        Check(parsed.result == 0 && parsed.is_reset && parsed.frames_decoded == 4 &&
                  parsed.samples_written == 100 && input.empty() &&
                  gapless.init.skip_samples == 32 && gapless.current.skip_samples == 0,
              "target RIFF parser consumes exact chunk lengths and fact delay");
        std::puts("AJM DIRECT AT9 S16/S32/F32, partial input, reset, drain and RIFF passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
