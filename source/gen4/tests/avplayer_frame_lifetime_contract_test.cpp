// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/libraries/avplayer/avplayer_source.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)

namespace Libraries::AvPlayer {
// Only fixture setup and decoder-pool access are private. Every publication
// and failed poll goes through the actual GetVideoData overloads.
class AvPlayerFrameLifetimeTestPeer {
public:
    static constexpr u32 Bytes = 16 * 16 * 3 / 2;
    enum class Wait { Empty, Paused, AudioAhead, TimerAhead, Inactive };

    static void Init(AvPlayerSource& source, unsigned& allocations) {
        source.m_memory_replacement = {&allocations, Allocate, Free, Allocate, Free};
    }
    static void Queue(AvPlayerSource& source, u8 value, u64 timestamp) {
        GuestBuffer buffer(source.m_memory_replacement, 16, Bytes, true);
        Enqueue(source, std::move(buffer), value, timestamp);
    }
    static void Block(AvPlayerSource& source, Wait reason) {
        switch (reason) {
        case Wait::Empty: break;
        case Wait::Inactive: source.m_is_eof = true; break;
        case Wait::Paused: source.m_is_paused = true; break;
        case Wait::AudioAhead:
            source.m_audio_stream_index = 0;
            source.m_last_audio_ts = 0;
            break;
        case Wait::TimerAhead:
            source.m_start_time = std::chrono::high_resolution_clock::now() -
                                  std::chrono::seconds(1);
            break;
        }
    }
    static void Unblock(AvPlayerSource& source) {
        source.m_is_eof = false;
        source.m_is_paused = false;
    }
    static size_t FreeCount(AvPlayerSource& source) {
        return source.m_video_buffers.Size();
    }
    static void DecodeRecycled(AvPlayerSource& source, u8 value) {
        auto buffer = source.m_video_buffers.Pop();
        CHECK(buffer.has_value());
        // Models the actual decoder's write to a pool buffer. It must never
        // obtain the current published allocation during a false poll.
        Enqueue(source, std::move(*buffer), value, 0);
    }
private:
    static void Enqueue(AvPlayerSource& source, GuestBuffer buffer, u8 value, u64 timestamp) {
        std::memset(buffer.GetBuffer(), value, Bytes);
        AvPlayerFrameInfoEx info{};
        info.p_data = buffer.GetBuffer();
        info.timestamp = timestamp;
        info.details.video.width = 16;
        info.details.video.height = 16;
        source.m_video_frames.Push(Frame{std::move(buffer), info});
    }
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
using Peer = AvPlayerFrameLifetimeTestPeer;

class State final : public AvPlayerStateCallback {
public:
    AvPlayerAvSyncMode mode = AvPlayerAvSyncMode::None;
    AvPlayerAvSyncMode GetSyncMode() override { return mode; }
    void OnWarning(u32) override { CHECK(false); }
    void OnError() override { CHECK(false); }
    void OnEOF() override {}
};

template<class Info>
static void Case(Peer::Wait wait) {
    unsigned allocations{};
    {
        State state;
        AvPlayerSource source(state, false);
        Peer::Init(source, allocations);
        Peer::Queue(source, 0x73, 0);
        Info info{};
        CHECK(source.GetVideoData(info));
        const auto* first = static_cast<const u8*>(info.p_data);
        CHECK(first && allocations == 1);
        if (wait != Peer::Wait::Empty && wait != Peer::Wait::Inactive)
            Peer::Queue(source, 0xa9, 60'000);
        state.mode = AvPlayerAvSyncMode::Default;
        Peer::Block(source, wait);
        for (u32 poll = 0; poll < 3; ++poll) {
            const auto before = info;
            CHECK(!source.GetVideoData(info));
            CHECK(std::memcmp(&before, &info, sizeof(info)) == 0);
            CHECK(Peer::FreeCount(source) == 0);
            CHECK(std::all_of(first, first + Peer::Bytes, [](u8 b) { return b == 0x73; }));
        }
        Peer::Unblock(source);
        state.mode = AvPlayerAvSyncMode::None;
        if (wait == Peer::Wait::Empty || wait == Peer::Wait::Inactive)
            Peer::Queue(source, 0xa9, 60'000);
        CHECK(source.GetVideoData(info));
        CHECK(info.p_data != first && allocations == 2 && Peer::FreeCount(source) == 1);
        const auto* second = static_cast<const u8*>(info.p_data);
        Peer::DecodeRecycled(source, 0);
        CHECK(std::all_of(first, first + Peer::Bytes, [](u8 b) { return b == 0; }));
        CHECK(std::all_of(second, second + Peer::Bytes, [](u8 b) { return b == 0xa9; }));
        CHECK(source.GetVideoData(info) && info.p_data == first);
        // With the minimum two buffers, each successful publication releases
        // exactly one buffer so the next frame remains decodable.
        for (u8 value = 1; value <= 16; ++value) {
            Peer::DecodeRecycled(source, value);
            CHECK(source.GetVideoData(info));
            CHECK(allocations == 2 && Peer::FreeCount(source) == 1);
            const auto* current = static_cast<const u8*>(info.p_data);
            CHECK(std::all_of(current, current + Peer::Bytes,
                              [value](u8 b) { return b == value; }));
        }
    }
    CHECK(allocations == 0);
}

int main() {
    for (const auto wait : {Peer::Wait::Empty, Peer::Wait::Paused, Peer::Wait::AudioAhead,
                            Peer::Wait::TimerAhead, Peer::Wait::Inactive}) {
        Case<AvPlayerFrameInfo>(wait);
        Case<AvPlayerFrameInfoEx>(wait);
    }
    std::puts("AvPlayer production video publication lifetime contracts PASS");
}
