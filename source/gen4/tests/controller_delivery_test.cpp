// SPDX-License-Identifier: GPL-2.0-or-later
#include "input/controller.h"
#include "lumen/virtual_input.h"

#include <array>
#include <cstdio>
#include <vector>

namespace {
using Button = Libraries::Pad::OrbisPadButtonDataOffset;
using Input::GameController;
using Input::State;

int failures{};
void Check(bool value, const char* name) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", name);
    failures += !value;
}

void Apply(GameController& controller, Lumen::Gen4::VirtualInput::Queue& queue) {
    for (const auto& state : queue.Drain()) {
        controller.ApplyRemoteState(static_cast<Button>(state.buttons), state.axes, false, 0, 0);
    }
}

void Tap(GameController& controller) {
    Lumen::Gen4::VirtualInput::Queue queue;
    queue.Button(static_cast<u32>(Button::Cross), true);
    queue.Button(static_cast<u32>(Button::Cross), false);
    Apply(controller, queue);
}

void TimerTick(GameController& controller) {
    // The production PollController callback runs these methods every 4 ms.
    controller.UpdateAxisSmoothing();
    controller.Gyro(0);
    controller.Acceleration(0);
}

std::vector<State> Read(GameController& controller, int count = 64) {
    std::vector<State> states(count);
    bool connected{};
    int connected_count{};
    const int received = controller.ReadStates(states.data(), count, &connected, &connected_count);
    if (!connected || connected_count != 1 || received < 0 || received > count) {
        Check(false, "connected bounded read");
        return {};
    }
    states.resize(received);
    return states;
}

std::vector<Button> Edges(const std::vector<State>& states) {
    std::vector<Button> result;
    for (const auto& state : states) {
        if (result.empty() || result.back() != state.buttonsState) result.push_back(state.buttonsState);
    }
    return result;
}

void TapAfterTimerTicks(int ticks, const char* name) {
    GameController controller;
    Tap(controller);
    for (int i = 0; i < ticks; ++i) TimerTick(controller);
    const auto states = Read(controller);
    Check(Edges(states) == std::vector{Button::Cross, Button::None}, name);
    for (std::size_t i = 1; i < states.size(); ++i) {
        if (states[i].time < states[i - 1].time) {
            Check(false, "sample timestamps remain ordered");
            break;
        }
    }
}
} // namespace

int main() {
    TapAfterTimerTicks(0, "tap survives one JNI drain");
    TapAfterTimerTicks(31, "tap survives 31 sensor timer ticks");
    TapAfterTimerTicks(32, "tap survives 32 sensor timer ticks");
    TapAfterTimerTicks(500, "tap survives prolonged unchanged sensor traffic");

    {
        GameController controller;
        Tap(controller);
        Tap(controller);
        for (int i = 0; i < 100; ++i) TimerTick(controller);
        Check(Edges(Read(controller)) ==
                  std::vector{Button::Cross, Button::None, Button::Cross, Button::None},
              "two unread taps retain all four edges");
    }
    {
        GameController controller;
        controller.ConnectController(nullptr);
        TimerTick(controller);
        Read(controller);
        for (int value = 0; value < 255; ++value) {
            controller.Axis(Input::Axis::LeftX, value, false);
            TimerTick(controller);
        }
        controller.Axis(Input::Axis::LeftX, 128, false);
        TimerTick(controller);
        const auto states = Read(controller, 1);
        Check(states.size() == 1 && states.front().axes[0] == 128,
              "unread analog burst returns latest centered position");
    }
    {
        GameController controller;
        // Begin from the sensor values already published by the running timer.
        controller.ConnectController(nullptr);
        TimerTick(controller);
        Read(controller);
        Tap(controller);
        const auto press = Read(controller, 1);
        for (int i = 0; i < 100; ++i) TimerTick(controller);
        const auto release = Read(controller, 1);
        Check(press.size() == 1 && press[0].buttonsState == Button::Cross &&
                  release.size() == 1 && release[0].buttonsState == Button::None,
              "partial read preserves pending release");
        Check(Read(controller).empty(), "consumed samples are not replayed");
    }
    {
        GameController controller;
        Tap(controller);
        State latest;
        bool connected{};
        int count{};
        controller.ReadState(&latest, &connected, &count);
        Check(connected && latest.buttonsState == Button::None &&
                  Edges(Read(controller)) == std::vector{Button::Cross, Button::None},
              "ReadState stays latest-only and does not consume buffered edges");
    }
    {
        GameController controller;
        controller.ConnectController(nullptr);
        TimerTick(controller);
        Read(controller);
        controller.Button(Button::Cross, true);
        State pressed;
        bool connected{};
        int count{};
        controller.ReadState(&pressed, &connected, &count);
        for (int i = 0; i < 500; ++i) TimerTick(controller);
        const auto held = Read(controller);
        Check(held.size() == 1 && held[0].time == pressed.time &&
                  held[0].buttonsState == Button::Cross,
              "identical timer samples preserve press timestamp");
        controller.Button(Button::Cross, false);
        State released;
        controller.ReadState(&released, &connected, &count);
        for (int i = 0; i < 500; ++i) TimerTick(controller);
        const auto neutral = Read(controller);
        Check(neutral.size() == 1 && neutral[0].time == released.time &&
                  neutral[0].buttonsState == Button::None,
              "identical timer samples preserve release timestamp");
    }
    {
        GameController controller;
        controller.ConnectController(nullptr);
        controller.SetTouchpadState(0, true, 0.1f, 0.2f);
        controller.SetTouchpadState(0, true, 0.3f, 0.4f);
        controller.SetTouchpadState(1, true, 0.5f, 0.6f);
        controller.SetTouchpadState(0, false, 0.3f, 0.4f);
        controller.SetTouchpadState(1, false, 0.5f, 0.6f);
        const auto states = Read(controller);
        Check(states.size() == 5 && states[0].touchpad[0].state &&
                  states[1].touchpad[0].x != states[0].touchpad[0].x &&
                  states[2].touchpad[1].state && !states[3].touchpad[0].state &&
                  !states[4].touchpad[1].state,
              "touch press movement second finger and releases stay distinct");
    }
    {
        GameController controller;
        controller.ConnectController(nullptr);
        const float first[3]{1, 2, 3}, second[3]{4, 5, 6};
        controller.UpdateGyro(first);
        controller.Gyro(0);
        controller.UpdateGyro(second);
        controller.Gyro(0);
        controller.UpdateAcceleration(first);
        controller.Acceleration(0);
        const auto states = Read(controller);
        Check(states.size() == 3 && states[0].angularVelocity.x == 1 &&
                  states[1].angularVelocity.x == 4 && states[2].acceleration.x == 1,
              "changed motion samples remain distinct");
    }
    {
        GameController controller;
        controller.ConnectController(nullptr);
        for (int i = 0; i < 66; ++i) controller.Button(Button::Cross, i % 2 == 0);
        const auto states = Read(controller);
        Check(states.size() == 64 && states.front().buttonsState == Button::Cross &&
                  states.back().buttonsState == Button::None,
              "digital-only overflow keeps existing 64-sample policy");
        Tap(controller);
        Check(Edges(Read(controller)) == std::vector{Button::Cross, Button::None},
              "queue remains usable after capacity wrap");
    }
    {
        GameController controller;
        controller.ConnectController(nullptr);
        TimerTick(controller);
        Read(controller);
        for (int i = 0; i < 66; ++i) controller.Button(Button::Cross, i % 2 == 0);
        Read(controller, 63);
        controller.Axis(Input::Axis::LeftX, 255, false);
        TimerTick(controller);
        controller.Axis(Input::Axis::LeftX, 128, false);
        TimerTick(controller);
        const auto states = Read(controller);
        Check(states.size() == 1 && states[0].buttonsState == Button::None &&
                  states[0].axes[0] == 128,
              "compaction updates the unread tail after capacity wrap and partial read");
    }
    std::printf("controller-delivery failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
