// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/libraries/libs.h"
#include "core/libraries/np/np_lookup.h"
#include "core/libraries/np/np_lookup_state.h"

namespace Libraries::Np::NpLookup {

s32 PS4_SYSV_ABI sceNpLookupCreateTitleCtx(const OrbisNpId* self_np_id) {
    return Detail::LookupState().CreateTitle(self_np_id);
}

s32 PS4_SYSV_ABI sceNpLookupDeleteTitleCtx(s32 title_id) {
    return Detail::LookupState().DeleteTitle(title_id);
}

s32 PS4_SYSV_ABI sceNpLookupCreateRequest(s32 title_id) {
    return Detail::LookupState().CreateRequest(title_id);
}

s32 PS4_SYSV_ABI sceNpLookupDeleteRequest(s32 request_id) {
    return Detail::LookupState().DeleteRequest(request_id);
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    Detail::LookupState().Reset();
    LIB_FUNCTION("8533Q+LU7EQ", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpLookupCreateTitleCtx);
    LIB_FUNCTION("mtqDK9zkoIE", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpLookupDeleteTitleCtx);
    LIB_FUNCTION("iQr9UxPHUFs", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpLookupCreateRequest);
    LIB_FUNCTION("wLaxchvEEnk", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpLookupDeleteRequest);
}

} // namespace Libraries::Np::NpLookup
