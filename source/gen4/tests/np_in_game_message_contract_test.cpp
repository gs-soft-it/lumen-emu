// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include "core/libraries/np/np_in_game_message.h"
#include "core/libraries/np/np_in_game_message_state.h"

using namespace Libraries::Np::NpInGameMessage;

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        auto& state = Detail::MessageState();
        state.Reset();
        Check(sceNpInGameMessageInitialize(0, nullptr) ==
                  ORBIS_NP_IN_GAME_MESSAGE_ERROR_INVALID_ARGUMENT, "empty pool rejected");
        Check(sceNpInGameMessageInitialize(16384, reinterpret_cast<void*>(0x10000)) ==
                  ORBIS_NP_IN_GAME_MESSAGE_ERROR_INVALID_ARGUMENT, "reserved layout not read");
        const s32 context = sceNpInGameMessageInitialize(16384, nullptr);
        Check(context == 1, "context ID returned");
        const s32 handle = sceNpInGameMessageCreateHandle(context);
        Check(handle == 1, "positive handle");
        Check(sceNpInGameMessageCreateHandle(-1) ==
                  ORBIS_NP_IN_GAME_MESSAGE_ERROR_LIB_CONTEXT_NOT_FOUND, "unknown context");
        Check(sceNpInGameMessageDeleteHandle(context, -1) ==
                  ORBIS_NP_IN_GAME_MESSAGE_ERROR_HANDLE_NOT_FOUND, "unknown handle");
        PeerAddress peer{};
        MessageData message{};
        Check(sceNpInGameMessageSendData(context, &peer, &peer, &message) ==
                  ORBIS_NP_IN_GAME_MESSAGE_ERROR_NOT_PREPARED, "unprepared send");
        Check(sceNpInGameMessagePrepare(context, handle, 0, 0, nullptr) ==
                  ORBIS_NP_IN_GAME_MESSAGE_ERROR_INVALID_ARGUMENT, "null callback");
        auto* userdata = reinterpret_cast<void*>(0x20000);
        Check(sceNpInGameMessagePrepare(context, handle, 0, 0x10000, userdata) ==
                  ORBIS_NP_IN_GAME_MESSAGE_ERROR_NOT_SIGNED_IN, "offline preparation explicit");
        const auto retained = state.ReadHandle(context, handle);
        Check(retained && retained->callback_address == 0x10000 && retained->userdata == userdata &&
                  !retained->transport_prepared, "registration retained without false readiness");
        message.size = 128;
        Check(sceNpInGameMessageSendData(context, &peer, &peer, &message) ==
                  ORBIS_NP_IN_GAME_MESSAGE_ERROR_NOT_SIGNED_IN, "offline send cannot succeed");
        message.size = 129;
        Check(sceNpInGameMessageSendData(context, &peer, &peer, &message) ==
                  ORBIS_NP_IN_GAME_MESSAGE_ERROR_INVALID_ARGUMENT, "message bound");
        Check(sceNpInGameMessageDeleteHandle(context, handle) == ORBIS_OK, "delete handle");
        Check(!state.ReadHandle(context, handle), "deleted callback removed");
        Check(sceNpInGameMessageCreateHandle(context) == 2, "stale handle not reused");
        Check(sceNpInGameMessageTerminate(context) == ORBIS_OK, "terminate context");
        Check(sceNpInGameMessageCreateHandle(context) ==
                  ORBIS_NP_IN_GAME_MESSAGE_ERROR_LIB_CONTEXT_NOT_FOUND, "terminated context");
        Check(sceNpInGameMessageTerminate(context) ==
                  ORBIS_NP_IN_GAME_MESSAGE_ERROR_LIB_CONTEXT_NOT_FOUND, "stale context");

        const auto small = sceNpInGameMessageInitialize(sizeof(Detail::Handle) * 2, nullptr);
        const auto first = sceNpInGameMessageCreateHandle(small);
        Check(first > 0 && sceNpInGameMessageCreateHandle(small) > 0, "two pool allocations");
        Check(sceNpInGameMessageCreateHandle(small) == ORBIS_NP_IN_GAME_MESSAGE_ERROR_OUT_OF_MEMORY,
              "real pool exhaustion");
        Check(sceNpInGameMessageDeleteHandle(small, first) == ORBIS_OK, "release pool allocation");
        Check(sceNpInGameMessageCreateHandle(small) == 3, "pool slot reused with fresh handle");
        Check(sceNpInGameMessageTerminate(small) == ORBIS_OK, "pool freed");

        const auto concurrent = sceNpInGameMessageInitialize(16384, nullptr);
        std::array<std::thread, 16> workers;
        std::array<s32, 16> ids{};
        for (std::size_t i = 0; i < workers.size(); ++i) {
            workers[i] = std::thread([&, i] { ids[i] = sceNpInGameMessageCreateHandle(concurrent); });
        }
        for (auto& worker : workers) worker.join();
        std::sort(ids.begin(), ids.end());
        for (std::size_t i = 0; i < ids.size(); ++i) {
            Check(ids[i] == static_cast<s32>(i + 1), "concurrent allocation unique");
        }
        state.Reset();
        Check(sceNpInGameMessageInitialize(16384, nullptr) == 1, "new session lifecycle");
        state.Reset();
        std::puts("NP in-game message offline lifecycle contract passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
