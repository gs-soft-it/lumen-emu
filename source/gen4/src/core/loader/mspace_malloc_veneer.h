// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace Core::Loader {

inline bool ZeroMspaceAllocationsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("LUMEN_ZERO_MSPACE_ALLOCATIONS");
        // Deterministic payloads are the shared compatibility default. Keep
        // an exact-zero rollback for diagnosis without selecting a title.
        return value == nullptr || value[0] != '0' || value[1] != '\0';
    }();
    return enabled;
}

constexpr std::array<std::uint8_t, 34> MakeZeroMspaceMallocVeneer(std::uint64_t target) {
    // Guest x86-64 SysV. Preserve size and align the stack for the original
    // call. Return its pointer and zero exactly the requested bytes on success.
    std::array<std::uint8_t, 34> code{
        0x56,                                                   // push rsi
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,                     // movabs rax, target
        0xff, 0xd0,                                             // call rax
        0x59,                                                   // pop rcx
        0x48, 0x85, 0xc0,                                       // test rax, rax
        0x74, 0x0e,                                             // jz ret
        0x48, 0x89, 0xc7,                                       // mov rdi, rax
        0x48, 0x89, 0xc2,                                       // mov rdx, rax
        0x31, 0xc0,                                             // xor eax, eax
        0xfc,                                                   // cld
        0xf3, 0xaa,                                             // rep stosb
        0x48, 0x89, 0xd0,                                       // mov rax, rdx
        0xc3,                                                   // ret
    };
    constexpr std::size_t target_offset = 3;
    for (std::size_t index = 0; index < sizeof(target); ++index) {
        code[target_offset + index] = static_cast<std::uint8_t>(target >> (8 * index));
    }
    return code;
}

} // namespace Core::Loader
