// SPDX-License-Identifier: GPL-2.0-only
#include "../direct_fex/DirectFexVeneerCache.h"

#include <cstdio>

int main() {
    using Lumen::Ps5::DirectFex::VeneerReturnCache;
    VeneerReturnCache cache;
    constexpr std::uint64_t first = 0x10000, collision = first + 0x8000, other = first + 4;
    const auto check = [](bool passed, const char* operation) {
        if (!passed) std::fprintf(stderr, "Veneer cache failed: %s\n", operation);
        return passed;
    };
    if (!check(cache.Find(first) == 0, "unregistered target miss")) return 1;
    cache.Remember(first, 0x20013);
    cache.Remember(other, 0x30013);
    if (!check(cache.Find(first) == 0x20013 && cache.Find(other) == 0x30013, "independent hits")) return 1;
    if (!check(cache.Find(collision) == 0, "full key required on collision")) return 1;
    cache.Remember(collision, 0x40013);
    if (!check(cache.Find(first) == 0 && cache.Find(collision) == 0x40013, "collision replaces slot")) return 1;
    cache.Remember(first, 0);
    if (!check(cache.Find(collision) == 0x40013, "miss cannot evict a positive entry")) return 1;
    cache.Remember(first, 0x20013);
    if (!check(cache.Find(first) == 0x20013 && cache.Find(collision) == 0, "evicted target republished")) return 1;
    VeneerReturnCache secondThread;
    if (!check(secondThread.Find(first) == 0, "cache instances are independent")) return 1;
    std::puts("PS5_DIRECT_FEX_VENEER_CACHE_TEST PASS");
}
