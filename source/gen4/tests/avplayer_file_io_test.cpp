// SPDX-License-Identifier: MIT
#include "core/libraries/avplayer/avplayer_file_io.h"

#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace Libraries::AvPlayer;

static void Require(bool value) {
    if (!value) std::abort();
}

int main() {
    std::atomic_flag active = ATOMIC_FLAG_INIT;
    {
        AvPlayerFileOperation owner(active);
        Require(static_cast<bool>(owner));
        // Both direct reentry and a callback joining a peer must complete
        // without waiting for the outer operation to return.
        {
            AvPlayerFileOperation nested(active);
            Require(!nested);
        }
        std::thread peer([&] {
            AvPlayerFileOperation concurrent(active);
            Require(!concurrent);
        });
        peer.join();
    }
    {
        AvPlayerFileOperation next(active);
        Require(static_cast<bool>(next));
    }
    unsigned accepted = 0;
    auto contend = [&] {
        unsigned completed = 0;
        while (completed < 10000) {
            AvPlayerFileOperation operation(active);
            if (!operation) continue;
            ++accepted;
            ++completed;
        }
    };
    std::thread first(contend), second(contend), third(contend);
    first.join(); second.join(); third.join();
    Require(accepted == 30000);

    std::vector<u8> source(9001);
    for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<u8>(i * 13);
    std::array<u8, kAvPlayerGuestReadChunkMax> bounce{};
    std::vector<u8> cache{42};
    u64 next_position = 0;
    auto read = [&](u64 position, u32 requested) -> s32 {
        Require(position == next_position);
        Require(requested > 0 && requested <= kAvPlayerGuestReadChunkMax);
        if (position == source.size()) return 0;
        // Short successful reads are not EOF and the next offset must advance
        // by the actual count, including the final bounded request.
        const auto count = static_cast<u32>(std::min<u64>(
            std::min(requested, u32{1237}), source.size() - position));
        std::memcpy(bounce.data(), source.data() + position, count);
        next_position += count;
        return count;
    };
    auto copy = [&](u8* output, u32 length) {
        std::memcpy(output, bounce.data(), length);
        return true;
    };
    Require(AvPlayerPreloadFile(source.size(), read, copy, cache));
    Require(cache == source && next_position == source.size());
    next_position = 0;
    Require(AvPlayerPreloadFile(0, read, copy, cache));
    Require(cache == source);

    const auto intact = cache;
    for (const s32 failure : {-1, 0, 4097}) {
        u32 calls = 0;
        Require(!AvPlayerPreloadFile(9001,
            [&](u64 position, u32) -> s32 {
                Require(position == (calls == 0 ? 0 : 4096));
                return calls++ == 0 ? 4096 : failure;
            }, copy, cache));
        // A partial attempt must never replace the published file.
        Require(cache == intact);
    }
    Require(!AvPlayerPreloadFile(4, [](u64, u32) { return 4; },
        [](u8*, u32) { return false; }, cache));
    Require(cache == intact);
    next_position = 0;
    Require(AvPlayerPreloadFile(source.size(), read, copy, cache));
    Require(cache == source); // Retrying never appends duplicate partial bytes.

    u32 copies = 0;
    Require(AvPlayerPreloadFile(0, [](u64, u32) { return 0; },
        [&](u8*, u32) { ++copies; return true; }, cache));
    Require(cache.empty() && copies == 0);
    std::puts("AvPlayer file operation/cache contracts PASS");
}
