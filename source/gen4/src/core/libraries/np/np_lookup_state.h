// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <bit>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include "core/libraries/np/np_error.h"
#include "core/libraries/np/np_types.h"

namespace Libraries::Np::NpLookup::Detail {

struct Request final {
    explicit Request(s32 title) : title_id{title} {}

    void Cancel() {
        {
            std::scoped_lock lock{mutex};
            aborted = true;
            if (!result) result = ORBIS_NP_COMMUNITY_ERROR_ABORTED;
        }
        completion.notify_all();
    }

    const s32 title_id;
    std::mutex mutex;
    std::condition_variable completion;
    std::optional<s32> result;
    bool aborted{};
};

// A single lock owns the handle tables. Retained request references survive deletion,
// and cancellation completes them before their public handles are removed.
class State final {
public:
    s32 CreateTitle(const OrbisNpId* self_np_id) {
        if (!self_np_id) return ORBIS_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT;
        std::scoped_lock lock{mutex};
        if (titles.size() >= 32) return ORBIS_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS;
        const s32 id = std::bit_cast<s32>(next_title++);
        titles[id] = *self_np_id;
        return id;
    }

    s32 DeleteTitle(s32 id) {
        std::scoped_lock lock{mutex};
        if (!titles.contains(id)) return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
        for (auto it = requests.begin(); it != requests.end();) {
            if (it->second->title_id == id) {
                it->second->Cancel();
                it = requests.erase(it);
            } else {
                ++it;
            }
        }
        titles.erase(id);
        return ORBIS_OK;
    }

    s32 CreateRequest(s32 title_id) {
        std::scoped_lock lock{mutex};
        if (!titles.contains(title_id)) return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
        if (requests.size() >= 32) return ORBIS_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS;
        const s32 id = std::bit_cast<s32>(next_request++);
        requests[id] = std::make_shared<Request>(title_id);
        return id;
    }

    s32 DeleteRequest(s32 id) {
        std::scoped_lock lock{mutex};
        const auto it = requests.find(id);
        if (it == requests.end()) return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
        it->second->Cancel();
        requests.erase(it);
        return ORBIS_OK;
    }

    std::shared_ptr<Request> FindRequest(s32 id) const {
        std::scoped_lock lock{mutex};
        const auto it = requests.find(id);
        return it == requests.end() ? nullptr : it->second;
    }

    std::optional<OrbisNpId> ReadTitle(s32 id) const {
        std::scoped_lock lock{mutex};
        const auto it = titles.find(id);
        if (it == titles.end()) return std::nullopt;
        return it->second;
    }

    void Reset() {
        std::scoped_lock lock{mutex};
        for (auto& [id, request] : requests) request->Cancel();
        requests.clear();
        titles.clear();
        next_title = next_request = 1;
    }

private:
    mutable std::mutex mutex;
    std::map<s32, OrbisNpId> titles;
    std::map<s32, std::shared_ptr<Request>> requests;
    u32 next_title{1};
    u32 next_request{1};
};

inline State& LookupState() {
    static State state;
    return state;
}

} // namespace Libraries::Np::NpLookup::Detail
