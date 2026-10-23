// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/logging/log.h"
#include "core/libraries/np/np_matching2_compat.h"

namespace Libraries::Np::NpMatching2 {

s32 PS4_SYSV_ABI sceNpMatching2GetRoomMemberDataInternal() {
    LOG_DEBUG(Lib_NpMatching2, "(STUBBED) called");
    return 0;
}

s32 PS4_SYSV_ABI sceNpMatching2SetRoomMemberDataInternal() {
    LOG_DEBUG(Lib_NpMatching2, "(STUBBED) called");
    return 0;
}

s32 PS4_SYSV_ABI sceNpMatching2SignalingGetLocalNetInfo(u16 context_id, void*) {
    LOG_DEBUG(Lib_NpMatching2, "(STUBBED) called, ctxId = {}", context_id);
    return 0;
}

s32 PS4_SYSV_ABI sceNpMatching2SendRoomChatMessage(u16 context_id, void*, void*, s32*) {
    LOG_DEBUG(Lib_NpMatching2, "(STUBBED) called, ctxId = {}", context_id);
    return 0;
}

} // namespace Libraries::Np::NpMatching2
