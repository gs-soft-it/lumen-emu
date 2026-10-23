// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include "common/types.h"

namespace Core {

s32 ReadCompiledSdkVersion(const std::filesystem::path& file);

} // namespace Core
