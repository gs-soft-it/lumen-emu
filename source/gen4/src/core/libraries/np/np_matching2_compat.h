// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"
namespace Libraries::Np::NpMatching2 {
// Compatibility handlers only log and return zero. They do not read or write
// caller buffers, assign request IDs, change member data or deliver messages.
s32 PS4_SYSV_ABI sceNpMatching2GetRoomMemberDataInternal();
s32 PS4_SYSV_ABI sceNpMatching2SetRoomMemberDataInternal();
s32 PS4_SYSV_ABI sceNpMatching2SignalingGetLocalNetInfo(u16 context_id, void* info);
s32 PS4_SYSV_ABI sceNpMatching2SendRoomChatMessage(u16 context_id, void* request,
                                                void* options, s32* request_id);
} // namespace Libraries::Np::NpMatching2
