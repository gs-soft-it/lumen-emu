// SPDX-License-Identifier: MIT
#include "core/libraries/kernel/sync/address_wait_queue.h"
#include <atomic>
#include <cstdlib>
#include <thread>
static void Require(bool value) { if (!value) std::abort(); }
int main() {
    using namespace std::chrono_literals;
    Libraries::Kernel::AddressWaitQueue queue;
    auto first = queue.Subscribe(1), second = queue.Subscribe(1), other = queue.Subscribe(2);
    Require(queue.Wake(1, 0) == 0);
    Require(queue.Wake(1, 1) == 1);
    Require(first.Released() && !second.Released() && !other.Released());
    Require(first.WaitFor(1ns)); // Wake before starting a wait is retained.
    Require(!second.WaitFor(1ms));
    std::atomic_bool observed{};
    std::thread waiter{[&] { observed = second.WaitFor(1s); }};
    Require(queue.Wake(1, 1) == 1);
    waiter.join(); Require(observed && !other.Released());
    { auto removed = queue.Subscribe(3); }
    Require(queue.Wake(3, 1) == 0);
    Require(queue.Wake(2, SIZE_MAX) == 1 && other.Released());
}
