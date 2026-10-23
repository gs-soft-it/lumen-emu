// SPDX-License-Identifier: GPL-2.0-or-later

#include <stdexcept>
#include "video_core/buffer_cache/source_page_tracker.h"

namespace {
void Require(bool value) {
    if (!value) throw std::runtime_error("source page tracking contract failed");
}
}

int main() {
    VideoCore::SourcePageTracker tracker;
    tracker.Register(0x1000);
    tracker.Register(0x2000);
    Require(tracker.Generation() == 0 && tracker.Get(0x1000).generation == 0);
    tracker.Mark(0x1ffc, 8, 11);
    Require(tracker.Generation() == 1);
    Require(tracker.Get(0x1000).generation == 1 && tracker.Get(0x2000).generation == 1);
    tracker.Register(0x1000);
    Require(tracker.Get(0x1000).buffer == 11);
    tracker.Mark(0x3000, 4, 12);
    Require(tracker.Generation() == 1);
    tracker.Mark(0x2000, 4, 13);
    Require(tracker.Get(0x1000).generation == 1 && tracker.Get(0x2000).generation == 2);
    Require(tracker.Get(0x2000).buffer == 13);
    tracker.Mark(0x1000, 0x100000, 14);
    Require(tracker.Generation() == 3 && tracker.Get(0x1000).buffer == 14 &&
            tracker.Get(0x2000).generation == 3);
    tracker.Mark(0x1000, 0, 15);
    Require(tracker.Generation() == 3);

    VideoCore::SourcePageTracker bounded;
    for (u64 i = 1; i <= 257; ++i) bounded.Register(i * 4096);
    bounded.Mark(0x1000, 257 * 4096, 21);
    Require(bounded.Generation() == 1 && bounded.Get(256 * 4096).generation == 1);
    Require(bounded.Get(257 * 4096).generation == 0);
    bounded.Register(0x1000);
    bounded.Mark(0x1000, 4, 22);
    Require(bounded.Get(0x1000).buffer == 22 && bounded.Generation() == 2);
    Require(bounded.Get(0x2000).generation == 1);
}
