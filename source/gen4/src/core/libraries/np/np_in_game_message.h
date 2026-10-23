// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include "core/libraries/np/np_types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Np::NpInGameMessage {

struct PeerAddress {
    OrbisNpOnlineId online_id;
    OrbisNpPlatformType platform;
};
static_assert(sizeof(PeerAddress) == 24);

struct MessageData {
    u8 data[128];
    u64 size;
};
static_assert(offsetof(MessageData, size) == 128);
static_assert(sizeof(MessageData) == 136);

s32 PS4_SYSV_ABI sceNpInGameMessageInitialize(u64 pool_size, const void* reserved);
s32 PS4_SYSV_ABI sceNpInGameMessageTerminate(s32 context_id);
s32 PS4_SYSV_ABI sceNpInGameMessageCreateHandle(s32 context_id);
s32 PS4_SYSV_ABI sceNpInGameMessageDeleteHandle(s32 context_id, s32 handle_id);
// Keep the callback as an opaque guest address until a transport can deliver the
// complete, verified callback ABI through the guest CPU facade.
s32 PS4_SYSV_ABI sceNpInGameMessagePrepare(s32 context_id, s32 handle_id, s32 user_id,
                                         u64 callback_address, void* userdata);
s32 PS4_SYSV_ABI sceNpInGameMessageSendData(s32 context_id, const PeerAddress* destination,
                                          const PeerAddress* source, const MessageData* message);
void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Np::NpInGameMessage
