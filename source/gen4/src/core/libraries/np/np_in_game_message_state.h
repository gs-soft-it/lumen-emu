// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include "core/libraries/np/np_error.h"
#include "core/libraries/np/np_in_game_message.h"

namespace Libraries::Np::NpInGameMessage::Detail {

struct Handle {
    s32 id{};
    s32 user_id{-1};
    u64 callback_address{};
    void* userdata{};
    bool allocated{};
    bool transport_prepared{};
};

struct Context {
    explicit Context(std::size_t bytes)
        : pool{std::make_unique<std::byte[]>(bytes)}, pool_size{bytes},
          slots{reinterpret_cast<Handle*>(pool.get()), bytes / sizeof(Handle)} {
        for (auto& slot : slots) std::construct_at(&slot);
    }
    ~Context() {
        for (auto& slot : slots) std::destroy_at(&slot);
    }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    std::unique_ptr<std::byte[]> pool;
    const std::size_t pool_size;
    const std::span<Handle> slots;
    u32 next_handle{1};
};

// Local resource ownership is implemented even when no authenticated transport
// exists. An offline preparation never advertises readiness or sends an event.
class State final {
public:
    s32 Initialize(u64 pool_size, const void* reserved) {
        // The optional object's layout is unknown; never dereference it.
        if (reserved || pool_size < sizeof(Handle) ||
            pool_size > std::numeric_limits<std::size_t>::max()) {
            return ORBIS_NP_IN_GAME_MESSAGE_ERROR_INVALID_ARGUMENT;
        }
        std::scoped_lock lock{mutex};
        if (next_context > static_cast<u32>(std::numeric_limits<s32>::max())) {
            return ORBIS_NP_IN_GAME_MESSAGE_ERROR_OUT_OF_MEMORY;
        }
        try {
            auto context = std::make_unique<Context>(static_cast<std::size_t>(pool_size));
            const s32 id = static_cast<s32>(next_context);
            contexts.emplace(id, std::move(context));
            ++next_context;
            return id;
        } catch (const std::bad_alloc&) {
            return ORBIS_NP_IN_GAME_MESSAGE_ERROR_OUT_OF_MEMORY;
        }
    }

    s32 Terminate(s32 context_id) {
        std::scoped_lock lock{mutex};
        return contexts.erase(context_id) ? ORBIS_OK
                                         : ORBIS_NP_IN_GAME_MESSAGE_ERROR_LIB_CONTEXT_NOT_FOUND;
    }

    s32 CreateHandle(s32 context_id) {
        std::scoped_lock lock{mutex};
        auto* context = FindContext(context_id);
        if (!context) return ORBIS_NP_IN_GAME_MESSAGE_ERROR_LIB_CONTEXT_NOT_FOUND;
        if (context->next_handle > static_cast<u32>(std::numeric_limits<s32>::max())) {
            return ORBIS_NP_IN_GAME_MESSAGE_ERROR_OUT_OF_MEMORY;
        }
        for (auto& slot : context->slots) {
            if (!slot.allocated) {
                const s32 id = static_cast<s32>(context->next_handle++);
                slot = Handle{.id = id, .allocated = true};
                return id;
            }
        }
        return ORBIS_NP_IN_GAME_MESSAGE_ERROR_OUT_OF_MEMORY;
    }

    s32 DeleteHandle(s32 context_id, s32 handle_id) {
        std::scoped_lock lock{mutex};
        auto* context = FindContext(context_id);
        if (!context) return ORBIS_NP_IN_GAME_MESSAGE_ERROR_LIB_CONTEXT_NOT_FOUND;
        auto* handle = FindHandle(*context, handle_id);
        if (!handle) return ORBIS_NP_IN_GAME_MESSAGE_ERROR_HANDLE_NOT_FOUND;
        *handle = {};
        return ORBIS_OK;
    }

    s32 Prepare(s32 context_id, s32 handle_id, s32 user_id, u64 callback_address, void* userdata) {
        std::scoped_lock lock{mutex};
        auto* context = FindContext(context_id);
        if (!context) return ORBIS_NP_IN_GAME_MESSAGE_ERROR_LIB_CONTEXT_NOT_FOUND;
        auto* handle = FindHandle(*context, handle_id);
        if (!handle) return ORBIS_NP_IN_GAME_MESSAGE_ERROR_HANDLE_NOT_FOUND;
        if (!callback_address) return ORBIS_NP_IN_GAME_MESSAGE_ERROR_INVALID_ARGUMENT;
        if (user_id < 0) return ORBIS_NP_IN_GAME_MESSAGE_ERROR_SIGNED_IN_USER_NOT_FOUND;
        handle->user_id = user_id;
        handle->callback_address = callback_address;
        handle->userdata = userdata;
        handle->transport_prepared = false;
        return ORBIS_NP_IN_GAME_MESSAGE_ERROR_NOT_SIGNED_IN;
    }

    s32 SendData(s32 context_id, const PeerAddress* destination, const PeerAddress* source,
                 const MessageData* message) {
        std::scoped_lock lock{mutex};
        const auto* context = FindContext(context_id);
        if (!context) return ORBIS_NP_IN_GAME_MESSAGE_ERROR_LIB_CONTEXT_NOT_FOUND;
        if (!destination || !source || !message || message->size > sizeof(message->data)) {
            return ORBIS_NP_IN_GAME_MESSAGE_ERROR_INVALID_ARGUMENT;
        }
        for (const auto& handle : context->slots) {
            if (handle.allocated && handle.callback_address) {
                return ORBIS_NP_IN_GAME_MESSAGE_ERROR_NOT_SIGNED_IN;
            }
        }
        return ORBIS_NP_IN_GAME_MESSAGE_ERROR_NOT_PREPARED;
    }

    std::optional<Handle> ReadHandle(s32 context_id, s32 handle_id) const {
        std::scoped_lock lock{mutex};
        const auto it = contexts.find(context_id);
        if (it == contexts.end()) return std::nullopt;
        const auto* handle = FindHandle(*it->second, handle_id);
        return handle ? std::optional{*handle} : std::nullopt;
    }

    void Reset() {
        std::scoped_lock lock{mutex};
        contexts.clear();
        next_context = 1;
    }

private:
    Context* FindContext(s32 id) const {
        const auto it = contexts.find(id);
        return it == contexts.end() ? nullptr : it->second.get();
    }
    static Handle* FindHandle(Context& context, s32 id) {
        for (auto& handle : context.slots) {
            if (handle.allocated && handle.id == id) return &handle;
        }
        return nullptr;
    }
    mutable std::mutex mutex;
    std::map<s32, std::unique_ptr<Context>> contexts;
    u32 next_context{1};
};

inline State& MessageState() {
    static State state;
    return state;
}

} // namespace Libraries::Np::NpInGameMessage::Detail
