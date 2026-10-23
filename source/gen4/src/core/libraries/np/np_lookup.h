// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/libraries/np/np_types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Np::NpLookup {

s32 PS4_SYSV_ABI sceNpLookupCreateTitleCtx(const OrbisNpId* self_np_id);
s32 PS4_SYSV_ABI sceNpLookupDeleteTitleCtx(s32 title_id);
s32 PS4_SYSV_ABI sceNpLookupCreateRequest(s32 title_id);
s32 PS4_SYSV_ABI sceNpLookupDeleteRequest(s32 request_id);
void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Np::NpLookup
