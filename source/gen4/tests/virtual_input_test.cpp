// SPDX-License-Identifier: GPL-2.0-only
#include "lumen/virtual_input.h"
#include <cstdio>
#include <stdexcept>

int main() {
    using namespace Lumen::Gen4::VirtualInput;
    auto require = [](bool value) { if (!value) throw std::runtime_error("Virtual input delivery failed"); };
    Queue queue;
    queue.Button(0x10, true);
    queue.Button(0x10, false);
    auto tap = queue.Drain();
    require(tap.size() == 2 && tap[0].buttons == 0x10 && tap[1].buttons == 0);
    require(queue.Drain().empty());
    queue.Axis(0, 240);
    queue.Axis(0, 255);
    queue.Button(0x20, true);
    queue.Axis(0, 128);
    auto mixed = queue.Drain();
    require(mixed.size() == 2 && mixed[0].axes[0] == 255 && mixed[1].axes[0] == 128);
    require(mixed[0].buttons == 0x20 && mixed[1].buttons == 0x20);
    queue.Button(0x40, true);
    queue.Reset();
    auto reset = queue.Drain();
    require(reset.size() == 1 && reset[0] == State{});
    queue.Button(0x80, true);
    require(queue.Drain().front().buttons == 0x80);
    queue.SetEnabled(false);
    queue.Button(0x10, true);
    queue.Axis(0, 0);
    require(queue.Drain().front() == State{});
    queue.SetEnabled(true);
    queue.Button(0x20, true);
    require(queue.Drain().front().buttons == 0x20);
    std::puts("Virtual input quick taps, axis ordering and reset PASS");
}
