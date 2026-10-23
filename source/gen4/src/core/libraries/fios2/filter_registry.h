// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <cstdint>
#include <mutex>

namespace Libraries::Fios2 {

class FilterRegistry {
public:
    static constexpr int32_t BadPointer = static_cast<int32_t>(0x80820006u);
    static constexpr int32_t BadIndex = static_cast<int32_t>(0x8082001cu);
    static constexpr int32_t EventNotHandled = static_cast<int32_t>(0x8082001du);
    struct Entry { void* callback{}; void* context{}; };

    template <class Invoke>
    int32_t Add(int index, Entry entry, Invoke&& invoke) {
        if (!entry.callback) return BadPointer;
        if (index < 0 || index >= static_cast<int>(entries.size())) return BadIndex;
        // Guest code may reenter the registry during initialization/termination.
        const auto result = invoke(entry, 0);
        if (result != 0 && result != EventNotHandled) return result;
        {
            std::scoped_lock lock{mutex};
            if (!entries[index].callback) {
                entries[index] = entry;
                return 0;
            }
        }
        invoke(entry, 1);
        return BadIndex;
    }

    template <class Invoke>
    int32_t Remove(int index, Invoke&& invoke) {
        Entry removed;
        {
            std::scoped_lock lock{mutex};
            if (index < 0 || index >= static_cast<int>(entries.size()) ||
                !entries[index].callback) return BadIndex;
            removed = entries[index];
            entries[index] = {};
        }
        const auto result = invoke(removed, 1);
        return result == EventNotHandled ? 0 : result;
    }

    int32_t Get(int index, Entry& output) {
        std::scoped_lock lock{mutex};
        if (index < 0 || index >= static_cast<int>(entries.size()) ||
            !entries[index].callback) return BadIndex;
        output = entries[index];
        return 0;
    }

private:
    std::mutex mutex;
    std::array<Entry, 256> entries{};
};

} // namespace Libraries::Fios2
