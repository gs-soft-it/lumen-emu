// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <bit>
#include <mutex>
#include "core/libraries/np/np_manager.h"

namespace Libraries::Np::NpManager::Detail {

// Host-owned state. Snapshots never hold the mutex across guest execution.
class CallbackRegistry final {
public:
    struct PresenceEntry {
        s32 id{};
        OrbisNpGamePresenceCallback function{};
        void* userdata{};
        bool occupied{};
    };
    struct PlusEntry {
        OrbisNpPlusEventCallback function{};
        void* userdata{};
    };
    struct Snapshot {
        PresenceEntry legacy;
        std::array<PresenceEntry, 8> presence;
        PlusEntry plus;
    };

    void RegisterLegacy(OrbisNpGamePresenceCallback function, void* userdata) {
        std::scoped_lock lock{mutex};
        state.legacy.function = function;
        state.legacy.userdata = userdata;
    }
    s32 RegisterPresence(OrbisNpGamePresenceCallback function, void* userdata) {
        if (function == nullptr) return ORBIS_NP_ERROR_INVALID_ARGUMENT;
        std::scoped_lock lock{mutex};
        for (auto& entry : state.presence) {
            if (!entry.occupied) {
                const auto id = std::bit_cast<s32>(++next_id);
                entry = {id, function, userdata, true};
                return id;
            }
        }
        return ORBIS_NP_ERROR_CALLBACK_MAX;
    }
    s32 UnregisterPresence(s32 id) {
        std::scoped_lock lock{mutex};
        for (auto& entry : state.presence) {
            if (entry.occupied && entry.id == id) {
                entry = {};
                return ORBIS_OK;
            }
        }
        return ORBIS_NP_ERROR_CALLBACK_NOT_REGISTERED;
    }
    s32 RegisterPlus(OrbisNpPlusEventCallback function, void* userdata) {
        if (function == nullptr) return ORBIS_NP_ERROR_INVALID_ARGUMENT;
        std::scoped_lock lock{mutex};
        if (state.plus.function != nullptr) return ORBIS_NP_ERROR_CALLBACK_ALREADY_REGISTERED;
        state.plus = {function, userdata};
        return ORBIS_OK;
    }
    s32 UnregisterPlus() {
        std::scoped_lock lock{mutex};
        if (state.plus.function == nullptr) return ORBIS_NP_ERROR_CALLBACK_NOT_REGISTERED;
        state.plus.function = nullptr;
        return ORBIS_OK;
    }
    [[nodiscard]] Snapshot Read() const {
        std::scoped_lock lock{mutex};
        return state;
    }
    // Called before a new session admits guest execution.
    void Reset() {
        std::scoped_lock lock{mutex};
        state = {};
        next_id = 0;
    }
private:
    mutable std::mutex mutex;
    Snapshot state{};
    u32 next_id{};
};

inline CallbackRegistry& Callbacks() {
    static CallbackRegistry registry;
    return registry;
}

} // namespace Libraries::Np::NpManager::Detail
