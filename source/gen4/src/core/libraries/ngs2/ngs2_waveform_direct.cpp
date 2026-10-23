// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/libraries/ngs2/ngs2.h"
#include "core/libraries/ngs2/ngs2_error.h"

#include <array>
#include <cstring>
#include <limits>

namespace Libraries::Ngs2 {
namespace {

constexpr std::array<u32, 12> PcmBytes{1, 1, 2, 2, 3, 3, 4, 4, 4, 4, 8, 8};
constexpr std::array<u32, 16> Atrac9UnitSamples{0, 64, 0, 0, 128, 0, 0, 256,
                                             0, 0, 0, 0, 0, 0, 0, 0};

struct FrameGeometry {
    u32 type;
    u32 bytes;
    u32 samples;
    u32 units;
    u32 delay;
};

s32 GetFrameGeometry(const OrbisNgs2WaveformFormat* format, FrameGeometry& geometry) {
    if (!format) {
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
    }
    geometry.type = format->waveformType;
    const u32 pcm_index = geometry.type - 0x10;
    if (pcm_index < PcmBytes.size() || geometry.type == 0x1c) {
        const u32 bytes = geometry.type == 0x1c ? 16 : PcmBytes[pcm_index];
        const u64 frame_bytes = static_cast<u64>(format->numChannels) * bytes;
        if (frame_bytes > std::numeric_limits<u32>::max()) {
            return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE;
        }
        geometry.bytes = static_cast<u32>(frame_bytes);
        geometry.samples = geometry.type == 0x1c ? 28 : 1;
        geometry.units = 1;
        geometry.delay = 0;
        return 0;
    }
    if (geometry.type != 0x40) {
        return ORBIS_NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
    }
    const u32 config = format->configData;
    if ((config >> 24) != 0xfe) {
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_CONFIG;
    }
    geometry.delay = Atrac9UnitSamples[(config >> 20) & 15];
    if (!geometry.delay) {
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_CONFIG;
    }
    const u32 shift = (config >> 3) & 3;
    geometry.units = 1u << shift;
    geometry.bytes = (((config >> 5) & 0x7ff) + 1) << shift;
    geometry.samples = geometry.delay << shift;
    return 0;
}

} // namespace

s32 PS4_SYSV_ABI sceNgs2SystemResetOption(OrbisNgs2SystemOption* outOption) {
    if (!outOption) {
        return ORBIS_NGS2_ERROR_INVALID_OPTION_ADDRESS;
    }
    constexpr OrbisNgs2SystemOption option{sizeof(OrbisNgs2SystemOption), "", 0, 512, 256,
                                          48000, {0}};
    static_assert(sizeof(option) == 64);
    std::memcpy(outOption, &option, sizeof(option));
    return 0;
}

s32 PS4_SYSV_ABI sceNgs2GetWaveformFrameInfo(const OrbisNgs2WaveformFormat* format,
                                           u32* outFrameSize, u32* outNumFrameSamples,
                                           u32* outUnitsPerFrame, u32* outNumDelaySamples) {
    if (outFrameSize) {
        *outFrameSize = 0;
    }
    if (outNumFrameSamples) {
        *outNumFrameSamples = 0;
    }
    if (outUnitsPerFrame) {
        *outUnitsPerFrame = 0;
    }
    if (outNumDelaySamples) {
        *outNumDelaySamples = 0;
    }
    FrameGeometry geometry{};
    if (const s32 result = GetFrameGeometry(format, geometry); result != 0) {
        return result;
    }
    if (outFrameSize) {
        *outFrameSize = geometry.bytes;
    }
    if (outNumFrameSamples) {
        *outNumFrameSamples = geometry.samples;
    }
    if (outUnitsPerFrame) {
        *outUnitsPerFrame = geometry.units;
    }
    if (outNumDelaySamples) {
        *outNumDelaySamples = geometry.delay;
    }
    return 0;
}

s32 PS4_SYSV_ABI sceNgs2CalcWaveformBlock(const OrbisNgs2WaveformFormat* format, u32 samplePos,
                                        u32 numSamples, OrbisNgs2WaveformBlock* outBlock) {
    if (!outBlock) {
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    *outBlock = {};
    FrameGeometry geometry{};
    if (const s32 result = GetFrameGeometry(format, geometry); result != 0) {
        return result;
    }
    const u64 first_frame = samplePos / geometry.samples;
    const u64 end_frame = (static_cast<u64>(samplePos) + numSamples + geometry.delay +
                           geometry.samples - 1) /
                          geometry.samples;
    const u64 frames = end_frame - first_frame;
    const u64 data_offset = first_frame * geometry.bytes;
    const u64 data_size = frames * geometry.bytes;
    const u64 end_offset = end_frame * geometry.bytes;
    const u64 sample_count = geometry.type == 0x1c ? frames * geometry.samples : numSamples;
    constexpr u64 Max = std::numeric_limits<u32>::max();
    if (data_offset > Max || data_size > Max || end_offset > Max || sample_count > Max) {
        return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE;
    }
    outBlock->dataOffset = static_cast<u32>(data_offset);
    outBlock->dataSize = static_cast<u32>(data_size);
    outBlock->numSamples = static_cast<u32>(sample_count);
    if (geometry.type == 0x40) {
        outBlock->numSkipSamples = geometry.delay + samplePos -
                                   static_cast<u32>(first_frame) * geometry.samples;
    }
    return 0;
}

} // namespace Libraries::Ngs2
