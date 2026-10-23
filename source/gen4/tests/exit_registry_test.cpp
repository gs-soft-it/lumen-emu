// SPDX-License-Identifier: MIT
#include "core/guest_cpu/exit_registry.h"
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>

int main() {
    Core::GuestCpu::ExitRegistry registry;
    std::vector<uintptr_t> calls;
    assert(!registry.Register({0, 0, 1}));
    registry.Register({1, 0, 10});
    registry.Register({2, 0, 20});
    registry.Register({3, 0, 10});
    registry.Finalize(10, [&](const auto& e) { calls.push_back(e.function); });
    assert((calls == std::vector<uintptr_t>{3, 1}));
    registry.Finalize(10, [&](const auto&) { assert(false); });
    registry.Register({4, 0, 20});
    registry.Finalize(20, [&](const auto& e) {
        calls.push_back(e.function);
        if (e.function == 4) {
            registry.Register({5, 0, 20});
            registry.Finalize(20, [&](const auto& nested) { calls.push_back(nested.function); });
        }
    });
    assert((calls == std::vector<uintptr_t>{3, 1, 4, 5, 2}));
    std::atomic_uint count{};
    for (unsigned i = 1; i <= 500; ++i) registry.Register({i, 0, 1});
    auto finalize = [&] { registry.Finalize(0, [&](const auto&) { ++count; }); };
    std::thread a{finalize}, b{finalize};
    a.join(); b.join();
    assert(count == 500);
    registry.Register({1, 0, 1}); registry.Reset();
    registry.Finalize(0, [&](const auto&) { assert(false); });
}
