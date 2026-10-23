#pragma once

#include <array>
#include <cstdint>

namespace Lumen::FexCore::FpBoundaryProbe {

// Original numerical fixtures. RDI owns {float(1.75), MXCSR}; RSI owns four
// result words, and RBX is a registered synthetic HLE target. No ROM bytes or
// guest address constants are used. Both programs finish at the ordinary
// adapter root-return sentinel supplied on their independently owned stacks.
inline constexpr std::array<std::uint8_t, 16> RootConversion {
    0xf3, 0x0f, 0x10, 0x07,       // movss xmm0, [rdi]
    0xf3, 0x0f, 0x2d, 0xc0,       // cvtss2si eax, xmm0
    0x89, 0x46, 0x00,             // mov [rsi], eax
    0x0f, 0xae, 0x5e, 0x04,       // stmxcsr [rsi+4]
    0xc3,
};

inline constexpr std::array<std::uint8_t, 37> AcrossHostCall {
    0x0f, 0xae, 0x57, 0x04,       // ldmxcsr [rdi+4]
    0xf3, 0x0f, 0x10, 0x07,       // movss xmm0, [rdi]
    0xf3, 0x0f, 0x2d, 0xc0,       // cvtss2si eax, xmm0
    0x89, 0x46, 0x00,             // mov [rsi], eax
    0x0f, 0xae, 0x5e, 0x04,       // stmxcsr [rsi+4]
    0xff, 0xd3,                   // call rbx
    0xf3, 0x0f, 0x10, 0x07,       // movss xmm0, [rdi]
    0xf3, 0x0f, 0x2d, 0xc0,       // cvtss2si eax, xmm0
    0x89, 0x46, 0x08,             // mov [rsi+8], eax
    0x0f, 0xae, 0x5e, 0x0c,       // stmxcsr [rsi+12]
    0xc3,
};

inline constexpr std::uint32_t InputBits = 0x3fe00000; // exact float 1.75
inline constexpr std::uint32_t GuestDownwardMxcsr = 0x3f80;
inline constexpr std::uint32_t DefaultMxcsr = 0x1f80;

} // namespace Lumen::FexCore::FpBoundaryProbe
