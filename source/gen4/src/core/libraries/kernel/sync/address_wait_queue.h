// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Libraries::Kernel {
// Waiters contain no guest pointer dereferences. Subscribe before inspecting the
// guest predicate, so a wake between the inspection and host wait is retained.
class AddressWaitQueue {
    struct Waiter { std::mutex mutex; std::condition_variable condition; bool released{}; };
public:
    class Ticket {
    public:
        Ticket(Ticket&&) = default;
        Ticket& operator=(Ticket&&) = delete;
        Ticket(const Ticket&) = delete;
        ~Ticket() { if (waiter) owner.Remove(key, waiter); }
        bool Released() const { std::lock_guard lock{waiter->mutex}; return waiter->released; }
        bool WaitFor(std::chrono::nanoseconds duration) {
            std::unique_lock lock{waiter->mutex};
            return waiter->condition.wait_for(lock, duration, [&] { return waiter->released; });
        }
    private:
        friend class AddressWaitQueue;
        Ticket(AddressWaitQueue& owner_, uintptr_t key_, std::shared_ptr<Waiter> waiter_)
            : owner(owner_), key(key_), waiter(std::move(waiter_)) {}
        AddressWaitQueue& owner;
        uintptr_t key;
        std::shared_ptr<Waiter> waiter;
    };
    Ticket Subscribe(uintptr_t key) {
        auto waiter = std::make_shared<Waiter>();
        std::lock_guard lock{mutex};
        waiters[key].push_back(waiter);
        return {*this, key, std::move(waiter)};
    }
    size_t Wake(uintptr_t key, size_t count) {
        std::lock_guard lock{mutex};
        auto found = waiters.find(key);
        if (found == waiters.end()) return 0;
        size_t released{};
        for (auto& weak : found->second) {
            auto waiter = weak.lock();
            if (!waiter) continue;
            std::lock_guard state{waiter->mutex};
            if (released == count) break;
            if (waiter->released) continue;
            waiter->released = true;
            ++released;
            waiter->condition.notify_one();
        }
        return released;
    }
private:
    void Remove(uintptr_t key, const std::shared_ptr<Waiter>& waiter) {
        std::lock_guard lock{mutex};
        auto found = waiters.find(key);
        if (found == waiters.end()) return;
        std::erase_if(found->second, [&](const auto& weak) {
            const auto value = weak.lock(); return !value || value == waiter;
        });
        if (found->second.empty()) waiters.erase(found);
    }
    std::mutex mutex;
    std::unordered_map<uintptr_t, std::vector<std::weak_ptr<Waiter>>> waiters;
};
} // namespace Libraries::Kernel
