// SPDX-License-Identifier: GPL-2.0-only
#include "core/libraries/fios2/filter_registry.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#define CHECK(condition) do { if (!(condition)) std::abort(); } while (false)

int main() {
    using Registry = Libraries::Fios2::FilterRegistry;
    Registry registry;
    int callback{}, context{};
    Registry::Entry entry{&callback, &context}, output;
    unsigned initialized{}, terminated{};
    auto invoke = [&](Registry::Entry received, int event) {
        CHECK(received.callback == &callback && received.context == &context);
        Registry::Entry nested;
        (void)registry.Get(3, nested); // Must not hold the registry lock across callbacks.
        event == 0 ? ++initialized : ++terminated;
        return Registry::EventNotHandled;
    };
    CHECK(registry.Add(-1, entry, invoke) == Registry::BadIndex);
    CHECK(registry.Add(256, entry, invoke) == Registry::BadIndex);
    CHECK(registry.Add(3, {}, invoke) == Registry::BadPointer);
    CHECK(initialized == 0 && terminated == 0);
    CHECK(registry.Add(3, entry, invoke) == 0);
    CHECK(registry.Get(3, output) == 0 && output.context == &context);
    CHECK(registry.Add(3, entry, invoke) == Registry::BadIndex);
    CHECK(initialized == 2 && terminated == 1);
    CHECK(registry.Remove(3, invoke) == 0);
    CHECK(registry.Remove(3, invoke) == Registry::BadIndex);
    CHECK(terminated == 2);
    CHECK(registry.Add(3, entry, [](auto, int) { return -99; }) == -99);
    CHECK(registry.Get(3, output) == Registry::BadIndex);

    std::atomic<unsigned> successes{};
    std::vector<std::thread> contenders;
    for (int i = 0; i != 16; ++i) contenders.emplace_back([&] {
        if (registry.Add(7, entry, [](auto, int) { return 0; }) == 0) ++successes;
    });
    for (auto& contender : contenders) contender.join();
    CHECK(successes == 1);
    CHECK(registry.Remove(7, [](auto, int event) { CHECK(event == 1); return -98; }) == -98);
    CHECK(registry.Get(7, output) == Registry::BadIndex);
    std::puts("Fios filter lifecycle PASS");
}
