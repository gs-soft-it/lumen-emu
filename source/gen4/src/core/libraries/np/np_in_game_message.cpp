// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/libraries/libs.h"
#include "core/libraries/np/np_in_game_message.h"
#include "core/libraries/np/np_in_game_message_state.h"

namespace Libraries::Np::NpInGameMessage {

s32 PS4_SYSV_ABI sceNpInGameMessageInitialize(u64 pool_size, const void* reserved) {
    return Detail::MessageState().Initialize(pool_size, reserved);
}
s32 PS4_SYSV_ABI sceNpInGameMessageTerminate(s32 context_id) {
    return Detail::MessageState().Terminate(context_id);
}
s32 PS4_SYSV_ABI sceNpInGameMessageCreateHandle(s32 context_id) {
    return Detail::MessageState().CreateHandle(context_id);
}
s32 PS4_SYSV_ABI sceNpInGameMessageDeleteHandle(s32 context_id, s32 handle_id) {
    return Detail::MessageState().DeleteHandle(context_id, handle_id);
}
s32 PS4_SYSV_ABI sceNpInGameMessagePrepare(s32 context_id, s32 handle_id, s32 user_id,
                                         u64 callback_address, void* userdata) {
    return Detail::MessageState().Prepare(context_id, handle_id, user_id, callback_address, userdata);
}
s32 PS4_SYSV_ABI sceNpInGameMessageSendData(s32 context_id, const PeerAddress* destination,
                                          const PeerAddress* source, const MessageData* message) {
    return Detail::MessageState().SendData(context_id, destination, source, message);
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    Detail::MessageState().Reset();
    LIB_FUNCTION("GFhVUpRmbHE", "libSceNpManager", 1, "libSceNpManager",
                 sceNpInGameMessageInitialize);
    LIB_FUNCTION("bMG3cVmUmuk", "libSceNpManager", 1, "libSceNpManager",
                 sceNpInGameMessageTerminate);
    LIB_FUNCTION("s4UEa5iBJdc", "libSceNpManager", 1, "libSceNpManager",
                 sceNpInGameMessageCreateHandle);
    LIB_FUNCTION("+anuSx2avHQ", "libSceNpManager", 1, "libSceNpManager",
                 sceNpInGameMessageDeleteHandle);
    LIB_FUNCTION("Vh1bhUG6mSs", "libSceNpManager", 1, "libSceNpManager",
                 sceNpInGameMessagePrepare);
    LIB_FUNCTION("ON7Sf5XEMmI", "libSceNpManager", 1, "libSceNpManager",
                 sceNpInGameMessageSendData);
}

} // namespace Libraries::Np::NpInGameMessage
