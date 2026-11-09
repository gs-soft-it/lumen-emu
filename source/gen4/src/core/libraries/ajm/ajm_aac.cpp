// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Lumen modification, 2026-11-09: use the native FFmpeg AAC decoder.

#include "ajm_aac.h"
#include "ajm_result.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>

extern "C" {
#include <libavcodec/adts_parser.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}

namespace Libraries::Ajm {
namespace {

constexpr std::array<u8, 9> RawChannelConfigurations{2, 1, 2, 3, 4, 5, 6, 11, 12};
constexpr u32 AacLcObjectType = 2;
constexpr u32 LastStandardSampleRateIndex = 12;

AVSampleFormat PackedSampleFormat(AjmFormatEncoding format) {
    switch (format) {
    case AjmFormatEncoding::S16:
        return AV_SAMPLE_FMT_S16;
    case AjmFormatEncoding::S32:
        return AV_SAMPLE_FMT_S32;
    case AjmFormatEncoding::Float:
        return AV_SAMPLE_FMT_FLT;
    }
    return AV_SAMPLE_FMT_NONE;
}

u32 AacChannelMask(const AVChannelLayout& layout) {
    switch (layout.nb_channels) {
    case 1:
    case 2:
    case 4:
    case 6:
    case 8:
        return GetChannelMask(layout.nb_channels);
    default:
        // For layouts not represented by the usual AJM channel counts, report
        // the actual speakers using the shared WAV/AVChannel bit positions.
        return layout.order == AV_CHANNEL_ORDER_NATIVE ? static_cast<u32>(layout.u.mask) : 0;
    }
}

DecoderResult CodecError(int error, DecoderResult result = {}) {
    LOG_ERROR(Lib_Ajm, "FFmpeg AAC decoding failed: {}", error);
    result.result = ORBIS_AJM_RESULT_CODEC_ERROR | ORBIS_AJM_RESULT_FATAL;
    result.internal_result = error;
    return result;
}

} // namespace

AjmAacDecoder::AjmAacDecoder(AjmFormatEncoding format, AjmAacCodecFlags flags, u32 channels)
    : m_format(format), m_flags(flags), m_channels(channels),
      m_stream_format{.sample_encoding = format}, m_packet(av_packet_alloc()),
      m_frame(av_frame_alloc()), m_pcm_frame(av_frame_alloc()), m_resampler(swr_alloc()) {}

AjmAacDecoder::~AjmAacDecoder() {
    swr_free(&m_resampler);
    av_frame_free(&m_pcm_frame);
    av_frame_free(&m_frame);
    av_packet_free(&m_packet);
    avcodec_free_context(&m_decoder);
}

void AjmAacDecoder::Reset() {
    avcodec_free_context(&m_decoder);
    m_frame_samples = 0;
    m_stream_format = {.sample_encoding = m_format};
    m_skip_frames = True(m_flags & AjmAacCodecFlags::EnableNondelayOutput) ? 0 : 2;

    if (m_init_params.config_type != ConfigType::ADTS && m_init_params.config_type != ConfigType::RAW) {
        return;
    }
    if (m_init_params.config_type == ConfigType::RAW &&
        (m_init_params.sampling_freq_type > LastStandardSampleRateIndex ||
         m_channels >= RawChannelConfigurations.size())) {
        return;
    }
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!codec || !m_packet || !m_frame || !m_pcm_frame || !m_resampler) {
        return;
    }
    m_decoder = avcodec_alloc_context3(codec);
    if (!m_decoder) {
        return;
    }

    if (m_init_params.config_type == ConfigType::RAW) {
        // MPEG-4 AudioSpecificConfig with AAC-LC, the guest's rate and channel
        // configuration, and the default 1024-sample GA frame length.
        const u32 asc = (AacLcObjectType << 11) | (m_init_params.sampling_freq_type << 7) |
                        (RawChannelConfigurations[m_channels] << 3);
        constexpr int AscSize = 2;
        m_decoder->extradata = static_cast<u8*>(av_mallocz(AscSize + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!m_decoder->extradata) {
            avcodec_free_context(&m_decoder);
            return;
        }
        m_decoder->extradata_size = AscSize;
        m_decoder->extradata[0] = static_cast<u8>(asc >> 8);
        m_decoder->extradata[1] = static_cast<u8>(asc);
    }

    const int status = avcodec_open2(m_decoder, codec, nullptr);
    if (status < 0) {
        LOG_ERROR(Lib_Ajm, "Could not initialize FFmpeg AAC decoder: {}", status);
        avcodec_free_context(&m_decoder);
    }
}

void AjmAacDecoder::Initialize(const void* buffer, u32 buffer_size) {
    m_init_params = {};
    if (buffer && buffer_size == sizeof(m_init_params)) {
        std::memcpy(&m_init_params, buffer, sizeof(m_init_params));
    }
    Reset();
}

void AjmAacDecoder::GetInfo(void* out_info) const {
    auto* codec_info = static_cast<AjmSidebandDecM4aacCodecInfo*>(out_info);
    *codec_info = {
        .heaac = True(m_flags & AjmAacCodecFlags::EnableSbrDecode),
    };
}

AjmSidebandFormat AjmAacDecoder::GetFormat() const {
    return m_stream_format;
}

u32 AjmAacDecoder::GetMinimumInputSize() const {
    return m_init_params.config_type == ConfigType::ADTS ? AV_AAC_ADTS_HEADER_SIZE : 1;
}

u32 AjmAacDecoder::GetNextFrameSize(const AjmInstanceGapless& gapless) const {
    const auto skip_samples = std::min<u32>(gapless.current.skip_samples, m_frame_samples);
    const auto available = m_frame_samples - skip_samples;
    const auto samples = gapless.init.total_samples != 0
                             ? std::min(gapless.current.total_samples, available)
                             : available;
    return samples * m_stream_format.num_channels * GetPCMSize(m_format);
}

int AjmAacDecoder::ConvertFrame() {
    av_frame_unref(m_pcm_frame);
    m_pcm_frame->format = PackedSampleFormat(m_format);
    m_pcm_frame->sample_rate = m_frame->sample_rate;
    int status = av_channel_layout_copy(&m_pcm_frame->ch_layout, &m_frame->ch_layout);
    if (status < 0) {
        return status;
    }
    status = swr_convert_frame(m_resampler, m_pcm_frame, m_frame);
    if (status == AVERROR_INPUT_CHANGED || status == AVERROR_OUTPUT_CHANGED ||
        status == (AVERROR_INPUT_CHANGED | AVERROR_OUTPUT_CHANGED)) {
        swr_close(m_resampler);
        status = swr_convert_frame(m_resampler, m_pcm_frame, m_frame);
    }
    return status;
}

u32 AjmAacDecoder::WriteFrame(SparseOutputBuffer& output, AjmInstanceGapless& gapless) const {
    const u32 samples = m_pcm_frame->nb_samples;
    const auto skip_samples = std::min<u32>(gapless.current.skip_samples, samples);
    const auto available = samples - skip_samples;
    const auto wanted = gapless.init.total_samples != 0
                            ? std::min(gapless.current.total_samples, available)
                            : available;
    const size_t stride = m_stream_format.num_channels * GetPCMSize(m_format);
    const auto writable = std::min<size_t>(wanted, output.Size() / stride);
    const auto pcm = std::span<const u8>(m_pcm_frame->data[0], samples * stride);
    const u32 written = output.Write(pcm.subspan(skip_samples * stride, writable * stride)) / stride;

    gapless.current.skip_samples -= skip_samples;
    gapless.current.skipped_samples += samples - written;
    if (gapless.init.total_samples != 0) {
        gapless.current.total_samples -= written;
    }
    return written;
}

DecoderResult AjmAacDecoder::ProcessData(std::span<u8>& input, SparseOutputBuffer& output,
                                        AjmInstanceGapless& gapless) {
    if (!m_decoder) {
        return {.result = ORBIS_AJM_RESULT_NOT_INITIALIZED};
    }
    if (input.size() < GetMinimumInputSize()) {
        return {.result = ORBIS_AJM_RESULT_PARTIAL_INPUT};
    }

    size_t packet_size = input.size();
    if (m_init_params.config_type == ConfigType::ADTS) {
        // A run buffer may contain multiple ADTS frames. Preserve the next
        // packet for the next AJM iteration and leave incomplete input intact.
        const size_t header_size = (input[1] & 1) ? AV_AAC_ADTS_HEADER_SIZE : AV_AAC_ADTS_HEADER_SIZE + 2;
        packet_size = ((input[3] & 3) << 11) | (input[4] << 3) | (input[5] >> 5);
        if (input[0] != 0xff || (input[1] & 0xf6) != 0xf0 || packet_size <= header_size) {
            return CodecError(AVERROR_INVALIDDATA);
        }
        if (packet_size > input.size()) {
            return {.result = ORBIS_AJM_RESULT_PARTIAL_INPUT};
        }
    }
    if (packet_size >= std::numeric_limits<int>::max() / 8) {
        return CodecError(AVERROR_INVALIDDATA);
    }

    int status = av_new_packet(m_packet, static_cast<int>(packet_size));
    if (status < 0) {
        return CodecError(status);
    }
    std::memcpy(m_packet->data, input.data(), packet_size);
    status = avcodec_send_packet(m_decoder, m_packet);
    av_packet_unref(m_packet);
    if (status < 0) {
        return CodecError(status);
    }

    DecoderResult result{};
    const auto remaining = input.subspan(packet_size);
    while ((status = avcodec_receive_frame(m_decoder, m_frame)) >= 0) {
        input = remaining;
        if (m_frame->ch_layout.nb_channels <= 0 || m_frame->nb_samples <= 0 ||
            m_frame->sample_rate <= 0) {
            return CodecError(AVERROR_INVALIDDATA, result);
        }
        if (m_frame->ch_layout.nb_channels > 8) {
            result.result = ORBIS_AJM_RESULT_TOO_MANY_CHANNELS;
            return result;
        }
        m_frame_samples = m_frame->nb_samples;
        const u64 bitrate = packet_size * 8ull * m_frame->sample_rate / m_frame_samples;
        m_stream_format = {
            .num_channels = static_cast<u32>(m_frame->ch_layout.nb_channels),
            .channel_mask = AacChannelMask(m_frame->ch_layout),
            .sampl_freq = static_cast<u32>(m_frame->sample_rate),
            .sample_encoding = m_format,
            .bitrate = static_cast<u32>(std::min<u64>(bitrate, std::numeric_limits<u32>::max())),
        };
        ++result.frames_decoded;
        if (m_skip_frames > 0) {
            --m_skip_frames;
            continue;
        }

        const int conversion = ConvertFrame();
        if (conversion < 0) {
            return CodecError(conversion, result);
        }
        result.samples_written += WriteFrame(output, gapless);
    }
    if (status != AVERROR(EAGAIN) && status != AVERROR_EOF) {
        return CodecError(status, result);
    }
    input = remaining;
    return result;
}

} // namespace Libraries::Ajm
