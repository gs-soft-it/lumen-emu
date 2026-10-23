// SPDX-License-Identifier: MIT
#include "core/guest_cpu/thread_callback_registry.h"
#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>
using namespace Core::GuestCpu;
static void Require(bool value) { if (!value) std::abort(); }
int main() {
    ThreadCallbackRegistry registry;
    ThreadCallback input{0x1234, 55, CallbackOrigin::Guest};
    registry.SetStart(1, input);
    input.function = 0x9999; input.origin = CallbackOrigin::Native;
    const auto start = registry.TakeStart(1);
    Require(start && start->function == 0x1234 && start->origin == CallbackOrigin::Guest);
    Require(!registry.TakeStart(1));
    bool guest{}, native{};
    const int failure = DispatchCallback(start->origin,
        [&] { guest = true; return -14; }, // An invalid guest address fails in guest dispatch.
        [&] { native = true; return 0; });
    Require(failure == -14 && guest && !native);
    registry.PushCleanup(1, {1,10,CallbackOrigin::Guest});
    registry.PushCleanup(1, {2,20,CallbackOrigin::Native});
    registry.PushCleanup(2, {3,30,CallbackOrigin::Guest});
    auto cleanup = registry.PopCleanup(1);
    Require(cleanup && cleanup->function == 2);
    DispatchCallback(cleanup->origin,
        [] { std::abort(); },
        [&] {
            // Reentry is safe because pop removed the record before callback.
            registry.PushCleanup(1, {4,40,CallbackOrigin::Guest});
            Require(registry.PopCleanup(1)->function == 4);
        });
    Require(registry.PopCleanup(1)->function == 1);
    Require(!registry.PopCleanup(1));
    Require(registry.PopCleanup(2)->function == 3);
    registry.SetStart(7, {0x77,0,CallbackOrigin::Guest});
    std::atomic_uint taken{};
    auto take = [&] { if (registry.TakeStart(7)) ++taken; };
    std::thread a{take}, b{take}; a.join(); b.join(); Require(taken == 1);
    registry.PushCleanup(8, {0x88}); registry.Clear(8); Require(!registry.PopCleanup(8));
}
