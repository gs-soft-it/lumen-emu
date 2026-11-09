// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace Libs::Graphics::SpirvBinary {

struct Issue {
    std::size_t offset = 0;
    const char* message = nullptr;
};

// Visit reference operands, excluding the instruction's result and result type.
using IdUseVisitor = bool (*)(void*, std::span<const uint32_t>, std::size_t);

// Check the binary structure emitted by the Gen5 shader compiler. Metal still
// parses and compiles the module, including target-specific semantic checks.
Issue Inspect(std::span<const uint32_t> words, IdUseVisitor visit = nullptr,
              void* context = nullptr);

} // namespace Libs::Graphics::SpirvBinary
