// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "loader/hostCall.h"
#include "libs/agc.h"
#include "libs/audio.h"
#include <bit>
#include <cstddef>
#include <type_traits>

namespace Loader::HostCallDetail {
using SizeAlign = Libs::Graphics::SizeAlign;
using Position = Libs::Audio::AudioOut2::AudioOut2Position;
using SpeakerArray = Libs::Audio::AudioOut2::AudioOut2SpeakerArrayHandle;
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(SizeAlign) == 16 && alignof(SizeAlign) == 8);
static_assert(std::is_standard_layout_v<SizeAlign> && std::is_trivially_copyable_v<SizeAlign>);
static_assert(offsetof(SizeAlign, m_size) == 0 && offsetof(SizeAlign, m_align) == 8);
static_assert(std::is_same_v<decltype(SizeAlign::m_size), std::uint64_t> &&
              std::is_same_v<decltype(SizeAlign::m_align), std::size_t> && sizeof(std::size_t) == 8);
static_assert(sizeof(Position) == 12 && alignof(Position) == 4);
static_assert(std::is_standard_layout_v<Position> && std::is_trivially_copyable_v<Position>);
static_assert(offsetof(Position, x) == 0 && offsetof(Position, y) == 4 && offsetof(Position, z) == 8);
static_assert(std::is_same_v<decltype(Position::x), float> &&
              std::is_same_v<decltype(Position::y), float> && std::is_same_v<decltype(Position::z), float>);

// Two INTEGER eightbytes return in x86 RAX/RDX. The native AAPCS return
// registers are handled by the compiler at the typed call below.
template <> struct CustomSignature<SizeAlign(KYTY_SYSV_ABI*)()> {
    static constexpr bool supported = true;
    template <auto Function> static bool Invoke(std::uint64_t* gpr,
        const LumenAndroidFexCoreSimdViewV2*) {
        const auto result = Function();
        gpr[0] = result.m_size;
        gpr[2] = result.m_align;
        return true;
    }
    template <auto Function> static HostCallMetadata Describe() {
        return {0, {}, HostScalar::Custom, nullptr, nullptr, &Invoke<Function>};
    }
};

// The 12-byte float position is SSE/SSE: x,y occupy the low eight bytes
// of XMM0 and z the low four bytes of XMM1. Subsequent floats use XMM2/3;
// independent INTEGER arguments continue in RDI, RSI, RDX and RCX.
template <> struct CustomSignature<int(KYTY_SYSV_ABI*)(
    SpeakerArray, Position, float, float*, std::uint32_t, std::uint8_t, float)> {
    static constexpr bool supported = true;
    template <auto Function> static bool Invoke(std::uint64_t* gpr,
        const LumenAndroidFexCoreSimdViewV2* simd) {
        if (!simd || simd->structSize != sizeof(*simd) || !simd->registers ||
            simd->registerCount < 4 || simd->registerStride < 16 || simd->registerBytes < 16) return false;
        Position position {};
        float spread {}, downmix {};
        std::memcpy(&position.x, simd->registers, 8);
        std::memcpy(&position.z, simd->registers + simd->registerStride, 4);
        std::memcpy(&spread, simd->registers + 2 * simd->registerStride, 4);
        std::memcpy(&downmix, simd->registers + 3 * simd->registerStride, 4);
        const auto result = Function(reinterpret_cast<SpeakerArray>(gpr[7]), position, spread,
            reinterpret_cast<float*>(gpr[6]), static_cast<std::uint32_t>(gpr[2]),
            static_cast<std::uint8_t>(gpr[1]), downmix);
        gpr[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
        return true;
    }
    template <auto Function> static HostCallMetadata Describe() {
        return {7, {}, HostScalar::Custom, nullptr, nullptr, &Invoke<Function>};
    }
};
} // namespace Loader::HostCallDetail
