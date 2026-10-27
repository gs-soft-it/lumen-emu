// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace Common::Arm64MemoryAccess {
// Architectural register image; x[31] is SP, while register 31 in a data
// operand is handled as ZR. SIMD registers retain all 128 bits.
struct State {
    std::array<uint64_t, 32> x {};
    std::array<std::array<std::byte, 16>, 32> v {};
};
enum class Access { Read, Write, ReadWrite };
using Resolve = bool (*)(void*, uint64_t, size_t, Access, void**) noexcept;
// Execute exactly one decoded A64 memory instruction through a protected host
// alias. The resolver must validate the entire operand before any access and
// resolve GPU ownership. Unknown/invalid instructions leave registers unchanged.
// LL/SC exclusive instructions are rejected: a value-only CAS reservation would
// not preserve monitor invalidation (ABA/CLREX). LSE atomics use native atomics.
bool Execute(uint32_t instruction, uint64_t fault_address, State& state,
             Resolve resolve, void* opaque) noexcept;
}
