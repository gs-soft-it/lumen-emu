// Lumen game-launch cache preflight. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <filesystem>
#include <string>

namespace Loader::GameCache {

// Called before any CPU admission. Primitive runtime self-tests have no game
// manifest and retain the native cache selector's separate default.
bool Prepare(const std::filesystem::path& root, const std::filesystem::path& executable,
             std::string* error);

} // namespace Loader::GameCache
