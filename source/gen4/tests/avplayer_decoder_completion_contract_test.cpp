// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/libraries/avplayer/avplayer_source.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)

namespace Libraries::AvPlayer {
// Owns only fixture setup. Both workers, decoded publication and consumer calls
// execute production AvPlayerSource code, including its completion guards.
class AvPlayerDecoderCompletionTestPeer {
public:
    static constexpr u32 Samples = 32;
    static constexpr u32 Bytes = Samples * 2 * sizeof(u16);

    static void Init(AvPlayerSource& source, unsigned& allocations, bool open_codec = true) {
        source.m_memory_replacement = {&allocations, Allocate, Free, Allocate, Free};
        source.m_avformat_context.reset(avformat_alloc_context());
        CHECK(source.m_avformat_context);
        auto* stream = avformat_new_stream(source.m_avformat_context.get(), nullptr);
        CHECK(stream);
        stream->time_base = AVRational{1, 48000};
        source.m_audio_stream_index = stream->index;
        const auto* codec = avcodec_find_decoder(AV_CODEC_ID_PCM_S16LE);
        CHECK(codec);
        source.m_audio_codec_context.reset(avcodec_alloc_context3(codec));
        CHECK(source.m_audio_codec_context);
        auto* context = source.m_audio_codec_context.get();
        context->sample_rate = 48000;
        context->sample_fmt = AV_SAMPLE_FMT_S16;
        context->pkt_timebase = stream->time_base;
        av_channel_layout_default(&context->ch_layout, 2);
        if (open_codec) CHECK(avcodec_open2(context, codec, nullptr) == 0);
        source.ResetDecoderCompletion();
    }

    static void QueuePacket(AvPlayerSource& source) {
        AvPlayerSource::AVPacketPtr packet(av_packet_alloc(), &AvPlayerSource::ReleaseAVPacket);
        CHECK(packet && av_new_packet(packet.get(), Bytes) == 0);
        for (u32 index = 0; index < Bytes; ++index) packet->data[index] = Pattern(index);
        packet->pts = packet->dts = 480;
        source.m_audio_packets.Push(std::move(packet));
    }
    static void SupplyBuffers(AvPlayerSource& source) {
        // The production video minimum is two buffers; this also lets the
        // decoder receive EAGAIN after its final frame without another getter.
        for (unsigned index = 0; index < 2; ++index)
            source.m_audio_buffers.Push(GuestBuffer(source.m_memory_replacement, 16, Bytes, false));
        source.m_audio_buffers_cv.Notify();
    }
    static void EOFReached(AvPlayerSource& source) {
        source.m_is_eof.store(true);
        source.m_audio_packets_cv.Notify();
        source.m_video_packets_cv.Notify();
    }
    static bool PacketTaken(AvPlayerSource& source) { return source.m_audio_packets.Size() == 0; }
    static bool VideoDone(AvPlayerSource& source) { return source.m_video_decoder_done.load(); }
    static bool AudioDone(AvPlayerSource& source) { return source.m_audio_decoder_done.load(); }
    static bool FrameQueued(AvPlayerSource& source) { return source.m_audio_frames.Size() == 1; }
    static void AudioWorker(AvPlayerSource& source, std::stop_token stop) { source.AudioDecoderThread(stop); }
    static void VideoWorker(AvPlayerSource& source, std::stop_token stop) { source.VideoDecoderThread(stop); }
    static u8 Pattern(u32 index) { return static_cast<u8>(index * 37 + 19); }

private:
    static void* PS4_SYSV_ABI Allocate(void* state, u32 align, u32 size) {
        void* allocation{};
        CHECK(posix_memalign(&allocation, align, size) == 0);
        ++*static_cast<unsigned*>(state);
        return allocation;
    }
    static void PS4_SYSV_ABI Free(void* state, void* allocation) {
        --*static_cast<unsigned*>(state);
        std::free(allocation);
    }
};
} // namespace Libraries::AvPlayer

using namespace Libraries::AvPlayer;
using Peer = AvPlayerDecoderCompletionTestPeer;

class State final : public AvPlayerStateCallback {
public:
    std::atomic<unsigned> errors{};
    AvPlayerAvSyncMode GetSyncMode() override { return AvPlayerAvSyncMode::None; }
    void OnWarning(u32) override { CHECK(false); }
    void OnError() override { ++errors; }
    void OnEOF() override {}
};

template<class Pred>
static void Await(Pred pred) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!pred()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

static void LastPacket(bool enabled, bool cancel) {
    unsigned allocations{};
    {
        State state;
        AvPlayerSource source(state, false);
        Peer::Init(source, allocations);
        Peer::QueuePacket(source);
        std::jthread video([&](std::stop_token stop) { Peer::VideoWorker(source, stop); });
        std::jthread audio([&](std::stop_token stop) { Peer::AudioWorker(source, stop); });
        // The decoder owns the final compressed packet but cannot publish:
        // all externally visible queues are empty, with demux EOF imminent.
        Await([&] { return Peer::PacketTaken(source); });
        Peer::EOFReached(source);
        video.join();
        CHECK(Peer::VideoDone(source));
        for (unsigned poll = 0; poll < 32; ++poll) {
            CHECK(source.IsActive() == enabled);
            AvPlayerFrameInfo absent{};
            CHECK(!source.GetAudioData(absent));
        }
        if (cancel) {
            audio.request_stop();
            audio.join();
            CHECK(Peer::AudioDone(source) && !source.IsActive());
        } else {
            Peer::SupplyBuffers(source);
            audio.join();
            CHECK(Peer::AudioDone(source) && Peer::FrameQueued(source));
            CHECK(source.IsActive());
            AvPlayerFrameInfo info{};
            CHECK(source.GetAudioData(info));
            CHECK(info.timestamp == 10 && info.details.audio.sample_rate == 48000);
            CHECK(info.details.audio.channel_count == 2 && info.details.audio.size == Peer::Bytes);
            const auto* samples = static_cast<const u8*>(info.p_data);
            for (u32 index = 0; index < Peer::Bytes; ++index) CHECK(samples[index] == Peer::Pattern(index));
            CHECK(!source.IsActive());
        }
        CHECK(state.errors == 0);
    }
    CHECK(allocations == 0);
}

static void EarlyError() {
    unsigned allocations{};
    {
        State state;
        AvPlayerSource source(state, false);
        // An unopened real codec rejects send_packet. Completion must still
        // publish on that early-return path, without Stop or another getter.
        Peer::Init(source, allocations, false);
        Peer::QueuePacket(source);
        Peer::EOFReached(source);
        std::jthread video([&](std::stop_token stop) { Peer::VideoWorker(source, stop); });
        std::jthread audio([&](std::stop_token stop) { Peer::AudioWorker(source, stop); });
        video.join();
        audio.join();
        CHECK(state.errors == 1 && Peer::AudioDone(source) && Peer::VideoDone(source));
        CHECK(!source.IsActive());
    }
    CHECK(allocations == 0);
}

int main(int argc, char** argv) {
    const bool enabled = argc == 1;
    CHECK(enabled || (argc == 2 && std::strcmp(argv[1], "--selector-off") == 0));
    LastPacket(enabled, false);
    LastPacket(enabled, true);
    EarlyError();
    std::puts("AvPlayer real PCM decoder completion, EOF, disabled video, cancellation and error contracts PASS");
}
