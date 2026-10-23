// Lumen adapter, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "loader/symbolDatabase.h"
#include <cstdint>

namespace Loader {
// A JUMP_SLOT provides the callable provenance missing from STT_NOTYPE.
// Preserve the relocation's original type for diagnostics/eligibility while
// looking up known functions with the same type used by LIB_FUNC exports.
constexpr SymbolType FunctionImportLookupType(SymbolType type, bool jump_slot) {
    return type == SymbolType::NoType && jump_slot ? SymbolType::Func : type;
}

// No type inference for data relocations or arithmetic inside a thunk.
constexpr bool FunctionFallbackEligible(bool enabled, SymbolType type, bool weak,
                                        bool jump_slot, std::int64_t addend) {
    return enabled && !weak && addend == 0 &&
        (type == SymbolType::Func || (type == SymbolType::NoType && jump_slot));
}

bool InitializeFunctionFallbackHostCall();
std::uint64_t RegisterFunctionFallback(std::int32_t owner, std::uint64_t patch_address,
    const char* symbol, const char* program, std::uint32_t relocation, bool plt);
void ReleaseFunctionFallbacks(std::int32_t owner);
std::uint64_t FunctionFallbackCallCount(std::int32_t owner, std::uint64_t patch_address);
#if defined(LUMEN_PS5_FEXCORE)
bool UnresolvedFallbackSelfTest(std::uint64_t stack_top) noexcept;
#endif
} // namespace Loader
