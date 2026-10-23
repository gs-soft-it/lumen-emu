// SPDX-License-Identifier: GPL-2.0-only
#include "common/uniformMetadata.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void Check(bool condition) {
    if (!condition) std::abort();
}
}

int main(int argc, char** argv) {
    using namespace Common::UniformMetadata;
    const char* selector = argc == 2 ? argv[1] : nullptr;
    if (selector) Check(setenv("LUMEN_PS5_UNIFORM_METADATA", selector, 1) == 0);
    else Check(unsetenv("LUMEN_PS5_UNIFORM_METADATA") == 0);
    const bool enabled = Enabled(); // Invalid selectors must fail here.
    Check(enabled == (selector == nullptr || std::strcmp(selector, "1") == 0));
    Check(setenv("LUMEN_PS5_UNIFORM_METADATA", enabled ? "0" : "1", 1) == 0);
    Check(Enabled() == enabled);

    KnownRanges ranges;
    uint32_t value = 0;
    constexpr uint64_t address = 0x10000;
    constexpr uint64_t size = 0x1000;
    ranges.Remember(address, size, 0x40404040u, 2);
    Check(ranges.Lookup(address, size, 2, &value) && value == 0x40404040u);
    Check(ranges.Lookup(address + 4, size - 8, 2, &value));
    Check(!ranges.Lookup(address - 4, size, 2, &value));
    Check(!ranges.Lookup(address, size + 4, 2, &value));
    Check(!ranges.Lookup(address + 1, 4, 2, &value));
    Check(!ranges.Lookup(address, 1, 2, &value));
    Check(!ranges.Lookup(address, size, 0, &value));
    Check(!ranges.Lookup(address, size, 3, &value));
    Check(!ranges.Lookup(address, size, 4, &value));
    Check(!ranges.Lookup(address, size, 2, nullptr));
    ranges.Invalidate(address + size, 1);
    Check(ranges.Lookup(address, size, 2, &value));
    // A one-byte unknown write invalidates the whole prior proof, including a
    // disjoint subrange that used to be covered by that same uniform write.
    ranges.Invalidate(address + 1, 1);
    Check(!ranges.Lookup(address + 0x800, 4, 2, &value));

    ranges.Remember(address, size, 0x80808080u, 2);
    ranges.Remember(address + size, size, 0x80808080u, 2);
    Check(!ranges.Lookup(address, size * 2, 2, &value));
    ranges.Remember(address + 4, 4, 0xc0c0c0c0u, 2);
    Check(!ranges.Lookup(address, 4, 2, &value));
    Check(ranges.Lookup(address + 4, 4, 2, &value) && value == 0xc0c0c0c0u);
    Check(ranges.Lookup(address + size, size, 2, &value) && value == 0x80808080u);
    ranges.Remember(address + 2 * size, size, 0, 4);
    Check(!ranges.Lookup(address + size, size, 2, &value));
    Check(ranges.Lookup(address + 2 * size, size, 4, &value) && value == 0);

    ranges.Clear();
    ranges.Remember(0, size, 1, 2);
    ranges.Remember(UINT64_MAX - 3, 8, 1, 2);
    ranges.Remember(address, 0, 1, 2);
    ranges.Remember(address + 1, 4, 1, 2);
    ranges.Remember(address, 5, 1, 2);
    ranges.Remember(address, size, 1, 0);
    ranges.Remember(address, size, 1, 3);
    Check(!ranges.Lookup(address, size, 2, &value));
    ranges.Remember(address, size, 1, 2);
    ranges.Invalidate(UINT64_MAX - 3, 8);
    Check(!ranges.Lookup(address, size, 2, &value));

    // Saturation may evict proofs, but cannot return a value from an old slot.
    for (uint64_t index = 0; index <= KnownRanges::Capacity; ++index)
        ranges.Remember(address + index * 2 * size, size, static_cast<uint32_t>(index), 6);
    unsigned retained = 0;
    for (uint64_t index = 0; index <= KnownRanges::Capacity; ++index) {
        if (ranges.Lookup(address + index * 2 * size, size, 6, &value)) {
            Check(value == index);
            ++retained;
        }
    }
    Check(retained == KnownRanges::Capacity);
    Check(ranges.Lookup(address + KnownRanges::Capacity * 2 * size, size, 6, &value) &&
          value == KnownRanges::Capacity);
    ranges.Clear();
    Check(!ranges.Lookup(address + KnownRanges::Capacity * 2 * size, size, 6, &value));
    std::puts("PASS: uniform metadata strict selector/latch, exact coverage/alignment, one-byte invalidation, generation rejection and bounded eviction");
}
