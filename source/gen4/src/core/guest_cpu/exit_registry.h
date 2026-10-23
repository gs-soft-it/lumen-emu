// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace Core::GuestCpu {

// Removing the entry before invocation makes finalization reentrant and ensures
// concurrent finalizers cannot invoke a destructor twice. Never hold the table
// mutex across guest execution: destructors may register or finalize more work.
class ExitRegistry final {
public:
    struct Entry {
        std::uintptr_t function{}, argument{}, dso{};
        bool has_argument{true};
    };
    bool Register(Entry entry) {
        if (!entry.function) return false;
        std::lock_guard lock{mutex};
        entries.push_back(entry);
        return true;
    }
    template <typename Invoke>
    void Finalize(std::uintptr_t dso, Invoke&& invoke) {
        for (;;) {
            Entry entry;
            {
                std::lock_guard lock{mutex};
                auto it = entries.end();
                while (it != entries.begin()) {
                    --it;
                    if (!dso || it->dso == dso) break;
                }
                if (it == entries.end() || (dso && it->dso != dso)) return;
                entry = *it;
                entries.erase(it);
            }
            invoke(entry);
        }
    }
    void Reset() {
        std::lock_guard lock{mutex};
        entries.clear();
    }
private:
    std::mutex mutex;
    std::vector<Entry> entries;
};

} // namespace Core::GuestCpu
