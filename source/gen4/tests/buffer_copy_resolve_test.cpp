// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <compare>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>
#include "common/slot_vector.h"
#include "video_core/buffer_cache/buffer_copy_resolve.h"

namespace Common::Log {
std::unordered_map<std::string_view, std::shared_ptr<spdlog::logger>> ALL_LOGGERS;
}
void assert_fail_impl() { std::abort(); }

namespace {
void Require(bool condition) {
    if (!condition) std::abort();
}

struct MockBuffer {
    std::uint64_t base;
    std::vector<std::uint8_t> bytes;
    MockBuffer(std::uint64_t base_, std::size_t size) : base{base_}, bytes(size) {}
    MockBuffer(MockBuffer&&) = default;
    MockBuffer(const MockBuffer&) = delete;
};

struct Cache {
    Common::SlotVector<MockBuffer> slots;
    Common::SlotId source, destination, retired;
    bool merge;
    std::uintptr_t original_source;
    unsigned allocations{};

    Cache(bool grow, bool merge_) : merge{merge_} {
        source = slots.insert(0x10000, 0x4000);
        std::fill_n(slots[source].bytes.begin(), 0x1000, 0xA7);
        if (grow) {
            for (unsigned i = 1; i < 2048; ++i) slots.insert(0x100000 + i * 0x4000, 1);
        }
        original_source = reinterpret_cast<std::uintptr_t>(&slots[source]);
    }

    Common::SlotId Find(std::uint64_t address, std::uint32_t size) {
        if (address == 0x10000) return source;
        Require(address == (merge ? 0x13f00 : 0x20000));
        if (destination) return destination;
        ++allocations;
        if (!merge) {
            destination = slots.insert(address, size);
        } else {
            destination = slots.insert(0x10000, 0x8000);
            std::copy(slots[source].bytes.begin(), slots[source].bytes.end(),
                      slots[destination].bytes.begin());
            retired = source;
            slots.erase(retired); // Model retirement completing before the eventual copy.
            source = destination;
        }
        return destination;
    }
};

void CopyCase(bool grow, bool merge) {
    Cache cache{grow, merge};
    constexpr std::uint64_t src = 0x10000;
    const std::uint64_t dst = merge ? 0x13f00 : 0x20000;
    constexpr std::uint32_t size = 0x1000;
    const auto ids = VideoCore::ResolveBufferCopyIds(src, dst, size,
        [&](auto address, auto count) { return cache.Find(address, count); });
    Require(cache.allocations == 1); // The final source lookup must not allocate.
    Require(ids.first == cache.source && ids.second == cache.destination);
    if (merge) {
        Require(ids.first == ids.second && ids.first != cache.retired);
        Require(!cache.slots.is_allocated(cache.retired));
    } else {
        Require((reinterpret_cast<std::uintptr_t>(&cache.slots[ids.first]) !=
                 cache.original_source) == grow);
    }
    auto& source = cache.slots[ids.first];
    auto& destination = cache.slots[ids.second];
    const auto src_offset = src - source.base;
    const auto dst_offset = dst - destination.base;
    Require(src_offset + size <= source.bytes.size());
    Require(dst_offset + size <= destination.bytes.size());
    // The merge fixture shares a 16 KiB cache page but not the actual copy range.
    if (merge) Require(!VideoCore::BufferCopyRangesOverlap(src_offset, dst_offset, size));
    std::copy_n(source.bytes.begin() + src_offset, size, destination.bytes.begin() + dst_offset);
    Require(std::all_of(destination.bytes.begin() + dst_offset,
                        destination.bytes.begin() + dst_offset + size,
                        [](auto byte) { return byte == 0xA7; }));
}
} // namespace

int main(int argc, char** argv) {
    using namespace VideoCore;
    if (argc == 2) {
        Require(BufferCopyResolveEnabled() == (argv[1][0] == '1'));
        return 0;
    }
    Require(BufferCopyResolveSelectorEnabled(nullptr));
    Require(!BufferCopyResolveSelectorEnabled("0"));
    Require(BufferCopyResolveSelectorEnabled("1"));
    for (auto value : {"", "00", "01", "10", "true", "false", "invalid", " 0", "0 "}) {
        Require(BufferCopyResolveSelectorEnabled(value));
    }
    CopyCase(false, false);
    CopyCase(true, false);
    CopyCase(false, true);
    CopyCase(true, true);
    Require(!BufferCopyRangesOverlap(0, 0, 0));
    Require(BufferCopyRangesOverlap(0, 0, 1));
    Require(!BufferCopyRangesOverlap(0, 16, 16));
    Require(BufferCopyRangesOverlap(0, 15, 16));
    Require(BufferCopyRangesOverlap(15, 0, 16));
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    Require(!BufferCopyRangesOverlap(0, max - 16, 16));
    Require(!BufferCopyRangesOverlap(max - 16, 0, 16));
    Require(BufferCopyRangesOverlap(max - 16, max - 8, 16));
    std::puts("buffer copy resolution: growth, retirement, payload and bounds PASS");
}
