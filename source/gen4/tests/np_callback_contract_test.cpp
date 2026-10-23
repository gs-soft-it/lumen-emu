// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#include "core/guest_cpu/hle_call_adapter.h"
#include "core/libraries/np/np_callback_registry.h"

namespace {
using namespace Libraries::Np::NpManager;
void Require(bool condition) {
    if (!condition) throw std::runtime_error("NP callback registration contract failed");
}
}

int main() {
    using namespace Libraries::Np::NpManager;
    using Core::GuestCpu::HleCallFrame;
    static_assert(std::is_void_v<decltype(sceNpRegisterGamePresenceCallback(nullptr, nullptr))>);
    static_assert(std::is_same_v<decltype(sceNpRegisterGamePresenceCallbackA(nullptr, nullptr)), s32>);
    auto callback = reinterpret_cast<OrbisNpGamePresenceCallback>(uintptr_t{0x10000});
    auto plus = reinterpret_cast<OrbisNpPlusEventCallback>(uintptr_t{0x20000});
    int user_a{}, user_b{};
    Detail::Callbacks().Reset();
    sceNpRegisterGamePresenceCallback(callback, &user_a);
    auto snapshot = Detail::Callbacks().Read();
    Require(snapshot.legacy.function == callback && snapshot.legacy.userdata == &user_a);
    sceNpRegisterGamePresenceCallback(nullptr, &user_b);
    Require(Detail::Callbacks().Read().legacy.function == nullptr);
    Require(Detail::Callbacks().Read().legacy.userdata == &user_b);
    // Previously copied state is owned and remains stable after replacement.
    Require(snapshot.legacy.function == callback && snapshot.legacy.userdata == &user_a);

    Require(sceNpRegisterGamePresenceCallbackA(nullptr, nullptr) == ORBIS_NP_ERROR_INVALID_ARGUMENT);
    std::array<s32, 8> ids{};
    for (auto& id : ids) id = sceNpRegisterGamePresenceCallbackA(callback, &user_a);
    Require(ids.front() == 1 && ids.back() == 8); // Duplicate function pointers occupy distinct IDs.
    Require(sceNpRegisterGamePresenceCallbackA(callback, &user_b) == ORBIS_NP_ERROR_CALLBACK_MAX);
    Require(sceNpUnregisterGamePresenceCallbackA(0) == ORBIS_NP_ERROR_CALLBACK_NOT_REGISTERED);
    Require(sceNpUnregisterGamePresenceCallbackA(ids[3]) == ORBIS_OK);
    const auto replacement = sceNpRegisterGamePresenceCallbackA(callback, &user_b);
    Require(replacement == 9);
    Require(sceNpUnregisterGamePresenceCallbackA(ids[3]) == ORBIS_NP_ERROR_CALLBACK_NOT_REGISTERED);
    snapshot = Detail::Callbacks().Read();
    Require(snapshot.presence[3].id == replacement && snapshot.presence[3].userdata == &user_b);
    for (const auto id : ids) if (id != ids[3]) Require(sceNpUnregisterGamePresenceCallbackA(id) == ORBIS_OK);
    Require(sceNpUnregisterGamePresenceCallbackA(replacement) == ORBIS_OK);

    Require(sceNpRegisterPlusEventCallback(nullptr, nullptr) == ORBIS_NP_ERROR_INVALID_ARGUMENT);
    Require(sceNpRegisterPlusEventCallback(plus, &user_a) == ORBIS_OK);
    Require(sceNpRegisterPlusEventCallback(plus, &user_b) == ORBIS_NP_ERROR_CALLBACK_ALREADY_REGISTERED);
    Require(Detail::Callbacks().Read().plus.userdata == &user_a);
    Require(sceNpUnregisterPlusEventCallback() == ORBIS_OK);
    Require(sceNpUnregisterPlusEventCallback() == ORBIS_NP_ERROR_CALLBACK_NOT_REGISTERED);

    // Exercise the production typed adapter, including the distinct void/int guest returns.
    HleCallFrame frame{};
    frame.gpr[0] = ~u64{0};
    frame.gpr[7] = reinterpret_cast<uintptr_t>(callback);
    frame.gpr[6] = reinterpret_cast<uintptr_t>(&user_a);
    Core::GuestCpu::detail::TypedHleCallAdapter<decltype(&sceNpRegisterGamePresenceCallback)> legacy{&sceNpRegisterGamePresenceCallback};
    Require(std::holds_alternative<bool>(legacy.Invoke(frame)) && frame.gpr[0] == 0);
    Require(Detail::Callbacks().Read().legacy.function == callback);
    Core::GuestCpu::detail::TypedHleCallAdapter<decltype(&sceNpRegisterGamePresenceCallbackA)> modern{&sceNpRegisterGamePresenceCallbackA};
    frame.gpr[7] = 0;
    Require(std::holds_alternative<bool>(modern.Invoke(frame)));
    Require(frame.gpr[0] == static_cast<u64>(static_cast<s64>(ORBIS_NP_ERROR_INVALID_ARGUMENT)));

    Detail::Callbacks().Reset();
    std::array<s32, 32> competing{};
    std::atomic<bool> start{};
    std::vector<std::thread> threads;
    for (size_t i = 0; i < competing.size(); ++i) {
        threads.emplace_back([&, i] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            competing[i] = sceNpRegisterGamePresenceCallbackA(callback, &user_a);
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    std::vector<s32> admitted;
    for (auto id : competing) {
        if (id > 0) admitted.push_back(id);
        else Require(id == ORBIS_NP_ERROR_CALLBACK_MAX);
    }
    std::sort(admitted.begin(), admitted.end());
    Require(admitted == std::vector<s32>({1, 2, 3, 4, 5, 6, 7, 8}));
    Detail::Callbacks().Reset();
    Require(Detail::Callbacks().Read().legacy.function == nullptr);
    Require(Detail::Callbacks().Read().plus.function == nullptr);
    Require(sceNpRegisterGamePresenceCallbackA(callback, &user_a) == 1);
    std::puts("NP typed callback registration, bounds, lifecycle, concurrency and ABI PASS");
}
