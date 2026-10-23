// Lumen adapter, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "loader/hostCall.h"
#include "graphics/shader/shader.h"

#include <bit>
#include <cstddef>
#include <type_traits>

namespace Loader::HostCallDetail {
// The upstream by-value register is two uint32_t fields in one INTEGER
// eightbyte. SysV uses one GP argument/return register, or one 8-byte spill.
// Complete layout is visible before registration; other aggregates stay denied.
template <> struct IntegerEightbyte<Libs::Graphics::ShaderRegister> : std::true_type {};
using AgcRegister = Libs::Graphics::ShaderRegister;
static_assert(sizeof(AgcRegister) == 8 && alignof(AgcRegister) == alignof(std::uint32_t));
static_assert(std::is_trivially_copyable_v<AgcRegister> &&
              std::is_standard_layout_v<AgcRegister>);
static_assert(offsetof(AgcRegister, offset) == 0 && offsetof(AgcRegister, value) == 4);
static_assert(std::is_same_v<decltype(AgcRegister::offset), std::uint32_t> &&
              std::is_same_v<decltype(AgcRegister::value), std::uint32_t>);
static_assert(std::endian::native == std::endian::little);
} // namespace Loader::HostCallDetail
