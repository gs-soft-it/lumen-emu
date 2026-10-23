// SPDX-License-Identifier: MIT
#pragma once

#include <cassert>
#include <cstddef>

namespace VideoCore {

// Used only by the GPU thread inside an exclusive guest publication. Native
// callbacks are opaque writers even when their current implementation reads.
class GuestPublicationInspection final {
public:
    class CallbackScope final {
    public:
        explicit CallbackScope(GuestPublicationInspection* owner = nullptr) : owner{owner} {
            if (!owner) return;
            ++owner->active_callbacks;
            owner->nested_host_callback = true;
        }
        ~CallbackScope() {
            if (!owner) return;
            assert(owner->active_callbacks != 0);
            --owner->active_callbacks;
            // A callback may inspect, then issue an unreported native store.
            // Its return must not leave that store classified as inspected.
            owner->nested_host_callback = true;
        }
        CallbackScope(const CallbackScope&) = delete;
        CallbackScope& operator=(const CallbackScope&) = delete;

    private:
        GuestPublicationInspection* owner;
    };

    [[nodiscard]] CallbackScope TrackNestedHostCallback(bool active = true) {
        return CallbackScope{active ? this : nullptr};
    }

    // A native-only packet may defer its boundary, but must not consume
    // any pending writer while deciding whether the boundary is redundant.
    [[nodiscard]] bool CanDeferPublication(bool pending_refresh) const {
        return !pending_refresh && !nested_host_callback && active_callbacks == 0;
    }

    [[nodiscard]] bool BeginInspection(bool requested, bool pending_refresh) {
        if (!requested && CanDeferPublication(pending_refresh)) return false;
        // Clear before inspecting: a callback invoked by cache invalidation
        // must remain pending for the next boundary, not be cleared on return.
        nested_host_callback = false;
        return true;
    }

private:
    bool nested_host_callback{};
    std::size_t active_callbacks{};
};

} // namespace VideoCore
