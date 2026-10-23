// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/loader/compiled_sdk.h"

#include <algorithm>
#include "core/linker.h"
#include "core/loader/elf.h"

namespace Core {

s32 ReadCompiledSdkVersion(const std::filesystem::path& file) {
    Core::Loader::Elf elf;
    elf.Open(file);
    if (!elf.IsElfFile()) {
        return 0;
    }
    const auto elf_pheader = elf.GetProgramHeader();
    auto i_procparam = std::find_if(elf_pheader.begin(), elf_pheader.end(), [](const auto& entry) {
        return entry.p_type == PT_SCE_PROCPARAM;
    });

    if (i_procparam != elf_pheader.end()) {
        Core::OrbisProcParam param{};
        elf.LoadSegment(u64(&param), i_procparam->p_offset, i_procparam->p_filesz);
        return param.sdk_version;
    }
    return 0;
}

} // namespace Core
