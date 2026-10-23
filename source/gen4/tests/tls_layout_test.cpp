// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/tls_layout.h"
#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
void Require(bool condition) {
    if (!condition) { std::fputs("Static TLS regression failed\n", stderr); std::abort(); }
}
template <typename F> void Reject(F&& function) {
    try { function(); } catch (const std::runtime_error&) { return; }
    Require(false);
}
}

int main() {
    using namespace Core;
    const std::array<u8, 4> initialized{0x11, 0x22, 0x33, 0x44};
    const std::array<ThreadLocalImage, 4> images{{
        {16, 528, 0, 1, 0, 0},
        {}, // Module handles without PT_TLS must not consume a DTV module ID.
        {16, 1168, 0, 2, reinterpret_cast<VAddr>(initialized.data()), 4},
        {16, 64, 0, 3, reinterpret_cast<VAddr>(initialized.data()), 2},
    }};
    const auto layout = BuildStaticTlsLayout(images);
    Require(layout.size == 1760 && layout.alignment == 32 && layout.images.size() == 3);
    Require(layout.images[0].offset == 528);
    Require(layout.images[1].offset == 1696 && layout.images[2].offset == 1760);
    alignas(256) std::array<u8, 4096> first, second;
    first.fill(0xaa); second.fill(0xbb);
    std::array<DtvEntry, 7> first_dtv{}, second_dtv{};
    InitializeStaticTls(layout, first.data(), first_dtv.data(), 5);
    InitializeStaticTls(layout, second.data(), second_dtv.data(), 5);
    for (const auto& image : layout.images) {
        const auto* one = first_dtv[image.modid + 1].pointer;
        const auto* two = second_dtv[image.modid + 1].pointer;
        Require(one == first.data() + layout.size - image.offset);
        Require(two == second.data() + layout.size - image.offset && one != two);
        Require(reinterpret_cast<uintptr_t>(one) % image.align == 0);
        Require(IsStaticTlsPointer(layout, first.data(), one));
        for (u32 byte = 0; byte < image.image_size; ++byte)
            Require(one[byte] == (byte < image.init_image_size ? initialized[byte] : 0));
    }
    // Initial DTV slots for late modules remain empty. Freeing a thread must
    // distinguish them from every preloaded image inside the shared TLS slab.
    Require(first_dtv[5].pointer == nullptr && first_dtv[6].pointer == nullptr);
    Require(!IsStaticTlsPointer(layout, first.data(), second.data()));
    Require(!IsStaticTlsPointer(layout, first.data(), first.data() + layout.size));
    first_dtv[3].pointer[0] = 0xfe;
    Require(second_dtv[3].pointer[0] == initialized[0]);
    Require(first[layout.size + StaticTlsLayout::TcbSize] == 0xaa);

    const std::array<ThreadLocalImage, 3> mixed{{
        {8, 24, 0, 1, 0, 0}, {256, 300, 0, 4, 0, 0}, {32, 17, 0, 6, 0, 0},
    }};
    const auto aligned = BuildStaticTlsLayout(mixed);
    Require(aligned.alignment == 256 && aligned.size % 256 == 0);
    for (uintptr_t padding = 0; padding < aligned.alignment; ++padding) {
        const auto original = uintptr_t{0x10000} + padding;
        const auto base = AlignStaticTlsAllocation(aligned, original);
        Require(base >= original && base % aligned.alignment == 0);
        Require(base + aligned.size + StaticTlsLayout::TcbSize <=
                original + StaticTlsAllocationSize(aligned));
    }
    Reject([&] { AlignStaticTlsAllocation(aligned, UINTPTR_MAX); });
    std::array<DtvEntry, 8> mixed_dtv{};
    InitializeStaticTls(aligned, first.data(), mixed_dtv.data(), 6);
    Require(mixed_dtv[3].pointer == nullptr && mixed_dtv[4].pointer == nullptr);
    Require(reinterpret_cast<uintptr_t>(mixed_dtv[5].pointer) % 256 == 0);
    Require(reinterpret_cast<uintptr_t>(mixed_dtv[7].pointer) % 32 == 0);
    Reject([&] { InitializeStaticTls(layout, first.data() + 1, first_dtv.data(), 5); });
    Reject([&] { InitializeStaticTls(layout, first.data(), first_dtv.data(), 1); });
    auto bad = images; bad[2].modid = 1;
    Reject([&] { BuildStaticTlsLayout(bad); });
    bad = images; bad[2].align = 3;
    Reject([&] { BuildStaticTlsLayout(bad); });
    bad = images; bad[2].init_image_size = bad[2].image_size + 1;
    Reject([&] { BuildStaticTlsLayout(bad); });
    bad = images; bad[2].image_size = UINT32_MAX;
    Reject([&] { BuildStaticTlsLayout(bad); });
    const auto empty = BuildStaticTlsLayout({});
    Require(empty.size == 0 && empty.alignment == 32);
    std::puts("Static TLS preload layout, DTV, alignment, BSS and isolation PASS");
}
