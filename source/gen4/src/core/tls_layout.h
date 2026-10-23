// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>
#include "core/tls.h"

namespace Core {

struct ThreadLocalImage {
    u32 align;
    u32 image_size;
    u32 offset;
    u32 modid;
    VAddr image_virtual_addr;
    u32 init_image_size;
};

struct StaticTlsLayout {
    static constexpr u32 TcbAlignment = 0x20;
    static constexpr u32 TcbSize = 0x40;
    u32 size{};
    u32 alignment{TcbAlignment};
    std::vector<ThreadLocalImage> images;
};

inline u64 StaticTlsAllocationSize(const StaticTlsLayout& layout) {
    return u64{layout.size} + StaticTlsLayout::TcbSize + layout.alignment - 1;
}

inline uintptr_t AlignStaticTlsAllocation(const StaticTlsLayout& layout, uintptr_t original) {
    const auto padding = static_cast<uintptr_t>(layout.alignment - 1);
    if (original > std::numeric_limits<uintptr_t>::max() - padding)
        throw std::runtime_error("Static TLS allocation address overflows");
    return (original + padding) & ~padding;
}

inline StaticTlsLayout BuildStaticTlsLayout(std::span<const ThreadLocalImage> images) {
    StaticTlsLayout layout;
    u64 offset{};
    for (auto image : images) {
        if (image.image_size == 0) continue;
        const u64 alignment = std::max<u32>(image.align, 1);
        if ((alignment & (alignment - 1)) != 0 || image.modid == 0 ||
            image.init_image_size > image.image_size ||
            (image.init_image_size && !image.image_virtual_addr) ||
            std::ranges::any_of(layout.images, [&](const auto& previous) {
                return previous.modid == image.modid;
            }))
            throw std::runtime_error("Invalid static TLS image");
        // Variant II: each image lies below the TCB. Keep the main image
        // nearest FS, including its link-time local-exec negative offset.
        offset = (offset + image.image_size + alignment - 1) & ~(alignment - 1);
        if (offset > std::numeric_limits<u32>::max())
            throw std::runtime_error("Static TLS layout overflows the guest ABI");
        image.offset = static_cast<u32>(offset);
        layout.alignment = std::max<u32>(layout.alignment, alignment);
        layout.images.push_back(image);
    }
    offset = (offset + layout.alignment - 1) & ~u64{layout.alignment - 1};
    if (offset > std::numeric_limits<u32>::max())
        throw std::runtime_error("Static TLS alignment overflows the guest ABI");
    layout.size = static_cast<u32>(offset);
    return layout;
}

inline void InitializeStaticTls(const StaticTlsLayout& layout, void* base, DtvEntry* table,
                                u32 table_count) {
    if (!base || reinterpret_cast<uintptr_t>(base) % layout.alignment != 0)
        throw std::runtime_error("Static TLS allocation is misaligned");
    auto* tcb = static_cast<u8*>(base) + layout.size;
    std::memset(base, 0, layout.size + StaticTlsLayout::TcbSize);
    for (const auto& image : layout.images) {
        if (image.modid > table_count)
            throw std::runtime_error("Static TLS image exceeds its thread directory");
        auto* destination = tcb - image.offset;
        if (image.init_image_size)
            std::memcpy(destination, reinterpret_cast<const void*>(image.image_virtual_addr),
                        image.init_image_size);
        table[image.modid + 1].pointer = destination;
    }
}

inline bool IsStaticTlsPointer(const StaticTlsLayout& layout, const void* base,
                               const void* pointer) {
    const auto begin = reinterpret_cast<uintptr_t>(base);
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    return address >= begin && address - begin < layout.size;
}
} // namespace Core
