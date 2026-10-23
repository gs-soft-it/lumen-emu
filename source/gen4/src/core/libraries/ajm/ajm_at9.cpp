// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ajm_result.h"
#include "atrac9_guard.h"
#include "common/assert.h"
#include "core/libraries/ajm/ajm_at9.h"
#include "error_codes.h"

extern "C" {
#include <decoder.h>
#include <libatrac9.h>
}

namespace Libraries::Ajm {

struct ChunkHeader {
    u32 tag;
    u32 length;
};
static_assert(sizeof(ChunkHeader) == 8);

struct AudioFormat {
    u16 fmt_type;
    u16 num_channels;
    u32 avg_sample_rate;
    u32 avg_byte_rate;
    u16 block_align;
    u16 bits_per_sample;
    u16 ext_size;
    union {
        u16 valid_bits_per_sample;
        u16 samples_per_block;
        u16 reserved;
    };
    u32 channel_mask;
    u8 guid[16];
    u32 version;
    u8 config_data[4];
    u32 reserved2;
};
static_assert(sizeof(AudioFormat) == 52);

struct SampleData {
    u32 sample_length;
    u32 encoder_delay;
    u32 encoder_delay2;
};
static_assert(sizeof(SampleData) == 12);

struct RIFFHeader {
    u32 riff;
    u32 size;
    u32 wave;
};
static_assert(sizeof(RIFFHeader) == 12);

AjmAt9Decoder::AjmAt9Decoder(AjmFormatEncoding format, AjmAt9CodecFlags flags, u32)
    : m_format(format), m_flags(flags), m_handle(Atrac9GetHandle()) {}

AjmAt9Decoder::~AjmAt9Decoder() {
    Atrac9ReleaseHandle(m_handle);
}

void AjmAt9Decoder::Reset() {
    std::scoped_lock lock{Atrac9LibraryMutex()};
    m_is_initialized = false;
    m_codec_info = {};
    m_num_frames = m_superframe_bytes_remain = 0;
    Atrac9ReleaseHandle(m_handle);
    m_handle = Atrac9GetHandle();
    const u32 config = (u32(m_config_data[0]) << 24) | (u32(m_config_data[1]) << 16) |
        (u32(m_config_data[2]) << 8) | m_config_data[3];
    if (!m_handle || (config >> 24) != 0xfe || ((config >> 17) & 7) > 5 ||
        (config & 0x10007) || (((config >> 3) & 3) != 0 && ((config >> 3) & 3) != 2) ||
        Atrac9InitDecoder(m_handle, m_config_data) || Atrac9GetCodecInfo(m_handle, &m_codec_info)) return;
    if (m_codec_info.channels < 1 || m_codec_info.channels > 8 ||
        (m_codec_info.frameSamples != 64 && m_codec_info.frameSamples != 128 && m_codec_info.frameSamples != 256) ||
        (m_codec_info.framesInSuperframe != 1 && m_codec_info.framesInSuperframe != 4) ||
        m_codec_info.superframeSize <= 0 || m_codec_info.superframeSize > 8192) return;
    m_superframe_bytes_remain = m_codec_info.superframeSize;
    m_is_initialized = true;
}

void AjmAt9Decoder::Initialize(const void* buffer, u32 buffer_size) {
    if (!buffer || buffer_size != sizeof(AjmDecAt9InitializeParameters)) { m_is_initialized = false; return; }
    const auto params = reinterpret_cast<const AjmDecAt9InitializeParameters*>(buffer);
    std::memcpy(m_config_data, params->config_data, ORBIS_AT9_CONFIG_DATA_SIZE);
    AjmAt9Decoder::Reset();
    if (!m_is_initialized) return;
    m_pcm_buffer.resize(m_codec_info.frameSamples * m_codec_info.channels * GetPCMSize(m_format),
                        0);
    m_is_initialized = true;
}

void AjmAt9Decoder::GetInfo(void* out_info) const {
    auto* info = reinterpret_cast<AjmSidebandDecAt9CodecInfo*>(out_info);
    info->super_frame_size = m_codec_info.superframeSize;
    info->frames_in_super_frame = m_codec_info.framesInSuperframe;
    info->next_frame_size = m_superframe_bytes_remain;
    info->frame_samples = m_codec_info.frameSamples;
}

u8 g_at9_guid[] = {0xD2, 0x42, 0xE1, 0x47, 0xBA, 0x36, 0x8D, 0x4D,
                   0x88, 0xFC, 0x61, 0x65, 0x4F, 0x8C, 0x83, 0x6C};

bool AjmAt9Decoder::ParseRIFFHeader(std::span<u8>& in_buf, AjmInstanceGapless& gapless) {
    auto remaining = in_buf;
    if (remaining.size() < sizeof(RIFFHeader)) return false;
    RIFFHeader header{};std::memcpy(&header, remaining.data(), sizeof(header));
    if (header.riff != 'FFIR' || header.wave != 'EVAW') return false;
    remaining = remaining.subspan(sizeof(header));
    std::optional<AudioFormat> format;
    std::optional<SampleData> samples;
    while (remaining.size() >= sizeof(ChunkHeader)) {
        ChunkHeader chunk{};std::memcpy(&chunk, remaining.data(), sizeof(chunk));
        remaining = remaining.subspan(sizeof(chunk));
        if (chunk.tag == 'atad') {
            if (!format) return false;
            AjmDecAt9InitializeParameters parameters{};
            std::memcpy(parameters.config_data, format->config_data, ORBIS_AT9_CONFIG_DATA_SIZE);
            Initialize(&parameters, sizeof(parameters));
            if (!m_is_initialized) return false;
            if (samples) {
                gapless.init.total_samples = samples->sample_length;
                gapless.init.skip_samples = samples->encoder_delay;
                gapless.Reset();
            }
            in_buf = remaining;
            return true;
        }
        if (chunk.length > remaining.size()) return false;
        if (chunk.tag == ' tmf') {
            if (chunk.length != sizeof(AudioFormat) || format) return false;
            AudioFormat value{};std::memcpy(&value, remaining.data(), sizeof(value));
            if (value.fmt_type != 0xfffe || std::memcmp(value.guid, g_at9_guid, 16)) return false;
            format = value;
        } else if (chunk.tag == 'tcaf') {
            if (chunk.length != sizeof(SampleData) || samples) return false;
            SampleData value{};std::memcpy(&value, remaining.data(), sizeof(value));samples = value;
        }
        const size_t bytes = static_cast<size_t>(chunk.length) + (chunk.length & 1);
        if (bytes > remaining.size()) return false;
        remaining = remaining.subspan(bytes);
    }
    return false;
}

u32 AjmAt9Decoder::GetMinimumInputSize() const {
    return m_superframe_bytes_remain;
}

DecoderResult AjmAt9Decoder::ProcessData(std::span<u8>& in_buf, SparseOutputBuffer& output,
                                         AjmInstanceGapless& gapless) {
    std::scoped_lock lock{Atrac9LibraryMutex()};
    DecoderResult result{};
    if (True(m_flags & AjmAt9CodecFlags::ParseRiffHeader) &&
        in_buf.size() >= sizeof(u32) && *reinterpret_cast<u32*>(in_buf.data()) == 'FFIR') {
        if (!ParseRIFFHeader(in_buf, gapless)) {
            result.result = ORBIS_AJM_RESULT_CODEC_ERROR | ORBIS_AJM_RESULT_FATAL;
            return result;
        }
        result.is_reset = true;
    }

    if (!m_is_initialized) {
        result.result = ORBIS_AJM_RESULT_NOT_INITIALIZED;
        return result;
    }

    if (in_buf.size() < static_cast<size_t>(m_superframe_bytes_remain) || m_superframe_bytes_remain <= 0) {
        result.result = ORBIS_AJM_RESULT_PARTIAL_INPUT;
        return result;
    }
    const int bounded_bytes = m_superframe_bytes_remain;
    int ret = 0;
    int bytes_used = 0;
    switch (m_format) {
    case AjmFormatEncoding::S16:
        ret = Atrac9Decode(m_handle, in_buf.data(), bounded_bytes, reinterpret_cast<s16*>(m_pcm_buffer.data()),
                           &bytes_used, True(m_flags & AjmAt9CodecFlags::NonInterleavedOutput));
        break;
    case AjmFormatEncoding::S32:
        ret = Atrac9DecodeS32(m_handle, in_buf.data(), bounded_bytes, reinterpret_cast<s32*>(m_pcm_buffer.data()),
                              &bytes_used, True(m_flags & AjmAt9CodecFlags::NonInterleavedOutput));
        break;
    case AjmFormatEncoding::Float:
        ret =
            Atrac9DecodeF32(m_handle, in_buf.data(), bounded_bytes, reinterpret_cast<float*>(m_pcm_buffer.data()),
                            &bytes_used, True(m_flags & AjmAt9CodecFlags::NonInterleavedOutput));
        break;
    default:
        UNREACHABLE();
    }
    if (ret != At9Status::ERR_SUCCESS || bytes_used <= 0 || bytes_used > bounded_bytes) {
        LOG_ERROR(Lib_Ajm, "Atrac9Decode failed ret = {:#x}", ret);
        result.result = ORBIS_AJM_RESULT_CODEC_ERROR | ORBIS_AJM_RESULT_FATAL;
        result.internal_result = ret;
        return result;
    }

    result.frames_decoded += 1;
    in_buf = in_buf.subspan(bytes_used);

    m_superframe_bytes_remain -= bytes_used;

    u32 skip_samples = 0;
    if (gapless.current.skip_samples > 0) {
        skip_samples = std::min(u16(m_codec_info.frameSamples), gapless.current.skip_samples);
        gapless.current.skip_samples -= skip_samples;
    }

    const auto max_pcm = gapless.init.total_samples != 0
                             ? gapless.current.total_samples * m_codec_info.channels
                             : std::numeric_limits<u32>::max();

    size_t pcm_written = 0;
    switch (m_format) {
    case AjmFormatEncoding::S16:
        pcm_written = WriteOutputSamples<s16>(output, skip_samples, max_pcm);
        break;
    case AjmFormatEncoding::S32:
        pcm_written = WriteOutputSamples<s32>(output, skip_samples, max_pcm);
        break;
    case AjmFormatEncoding::Float:
        pcm_written = WriteOutputSamples<float>(output, skip_samples, max_pcm);
        break;
    default:
        UNREACHABLE();
    }

    result.samples_written = pcm_written / m_codec_info.channels;
    gapless.current.skipped_samples += m_codec_info.frameSamples - result.samples_written;
    if (gapless.init.total_samples != 0) {
        gapless.current.total_samples -= result.samples_written;
    }

    m_num_frames += 1;
    if ((m_num_frames % m_codec_info.framesInSuperframe) == 0) {
        if (m_superframe_bytes_remain) {
            in_buf = in_buf.subspan(m_superframe_bytes_remain);
        }
        m_superframe_bytes_remain = m_codec_info.superframeSize;
        m_num_frames = 0;
    } else if (gapless.IsEnd()) {
        // Drain the remaining superframe
        std::vector<s16> buf(m_codec_info.frameSamples * m_codec_info.channels, 0);
        while ((m_num_frames % m_codec_info.framesInSuperframe) != 0) {
            ret = Atrac9Decode(m_handle, in_buf.data(), static_cast<int>(std::min<size_t>(in_buf.size(),m_superframe_bytes_remain)), buf.data(), &bytes_used,
                               True(m_flags & AjmAt9CodecFlags::NonInterleavedOutput));
            if (ret != At9Status::ERR_SUCCESS || bytes_used <= 0 || static_cast<size_t>(bytes_used) > in_buf.size() || bytes_used > m_superframe_bytes_remain) {
                result.result = ORBIS_AJM_RESULT_CODEC_ERROR | ORBIS_AJM_RESULT_FATAL;
                result.internal_result = ret;
                return result;
            }
            in_buf = in_buf.subspan(bytes_used);
            m_superframe_bytes_remain -= bytes_used;
            result.frames_decoded += 1;
            m_num_frames += 1;
        }
        in_buf = in_buf.subspan(m_superframe_bytes_remain);
        m_superframe_bytes_remain = m_codec_info.superframeSize;
        m_num_frames = 0;
    }

    return result;
}

AjmSidebandFormat AjmAt9Decoder::GetFormat() const {
    if (!m_is_initialized) return AjmSidebandFormat{.sample_encoding=m_format};
    return AjmSidebandFormat{
        .num_channels = u32(m_codec_info.channels),
        .channel_mask = GetChannelMask(u32(m_codec_info.channels)),
        .sampl_freq = u32(m_codec_info.samplingRate),
        .sample_encoding = m_format,
        .bitrate = u32((m_codec_info.samplingRate * m_codec_info.superframeSize * 8) /
                       (m_codec_info.framesInSuperframe * m_codec_info.frameSamples)),
        .reserved = 0,
    };
}

u32 AjmAt9Decoder::GetNextFrameSize(const AjmInstanceGapless& gapless) const {
    const auto skip_samples =
        std::min<u32>(gapless.current.skip_samples, m_codec_info.frameSamples);
    const auto samples =
        gapless.init.total_samples != 0
            ? std::min<u32>(gapless.current.total_samples, m_codec_info.frameSamples - skip_samples)
            : m_codec_info.frameSamples - skip_samples;
    return samples * m_codec_info.channels * GetPCMSize(m_format);
}

} // namespace Libraries::Ajm
