// Lumen, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#include "kernel/guestMemoryRights.h"

#include <array>
#include <cstdio>

int main() {
    using namespace Libs::LibKernel::Memory::GuestRights;
    constexpr std::array ranges {
        Range {0x10000, 0x4000, Read | Write},
        Range {0x14000, 0x4000, Read},
        Range {0x20000, 0x4000, Read | Execute},
        Range {0x28000, 0x4000, 0},
    };
    bool pass = Allows(ranges, 0x10000, 0x8000, Read) &&
        Allows(ranges, 0x13fff, 1, Write) &&
        !Allows(ranges, 0x13fff, 2, Write) &&
        !Allows(ranges, 0x17fff, 2, Read) &&
        Allows(ranges, 0x23fff, 1, Execute) &&
        !Allows(ranges, 0x20000, 1, Write) &&
        !Allows(ranges, 0x28000, 1, Read) &&
        !Allows(ranges, UINT64_MAX, 2, Read) &&
        !Allows(ranges, 0, 1, Read) &&
        !Allows(ranges, 0x10000, 0, Read) &&
        !Allows(ranges, 0x10000, 1, 0) &&
        !Allows(ranges, 0x10000, 1, 0x10) &&
        !Allows({}, 0x10000, 1, Read);
    constexpr std::array stacks {
        Range {0x40000, 0x4000, Read | Write, true},
        Range {0x44000, 0x4000, Read, true},
        Range {0x48000, 0x4000, Read | Write, false},
        Range {0x4c000, 0x4000, Read | Execute, true},
        Range {0x54000, 0x4000, Read, true},
    };
    pass &= AllowsOwnedStackRead(stacks, 0x43ffc, 8, 0x40000, 0x10000) &&
        !AllowsOwnedStackRead(stacks, 0x47ffc, 8, 0x40000, 0x10000) &&
        !AllowsOwnedStackRead(stacks, 0x4c000, 8, 0x40000, 0x10000) &&
        !AllowsOwnedStackRead(stacks, 0x4fffc, 8, 0x40000, 0x18000) &&
        !AllowsOwnedStackRead(stacks, 0x43ffc, 8, 0x40000, 0x4000) &&
        !AllowsOwnedStackRead(stacks, 0x40000, 8, 0x44000, 0x4000) &&
        !AllowsOwnedStackRead(stacks, 0x40000, 0, 0x40000, 0x4000) &&
        !AllowsOwnedStackRead(stacks, 0x40000, UINT64_MAX, 0x40000, 0x4000) &&
        !AllowsOwnedStackRead(stacks, UINT64_MAX - 4, 8, UINT64_MAX - 8, 16) &&
        !AllowsOwnedStackRead({}, 0x40000, 8, 0x40000, 0x4000);
    std::puts(pass ? "PS5 guest memory rights: PASS" : "PS5 guest memory rights: FAIL");
    return pass ? 0 : 1;
}
