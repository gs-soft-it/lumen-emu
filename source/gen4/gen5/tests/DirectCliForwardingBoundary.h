// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <cstddef>

namespace Emulator { struct RunOptions; }

using DirectCliOptionsLayout = std::array<std::size_t, 8>;

// The consumer is compiled independently against the engine's public header.
// Check layout before dereferencing fields, so header drift fails predictably.
bool CheckDirectCliOptionsAtEngineBoundary(const Emulator::RunOptions& options,
                                          const DirectCliOptionsLayout& caller_layout,
                                          const char* expected_root, bool customized);
