// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <type_traits>
#include "core/guest_cpu/hle_call_adapter.h"
#include "core/libraries/np/np_matching2.h"
#include "core/libraries/np/np_matching2_compat.h"
#include "core/loader/symbols_resolver.h"

using namespace Libraries::Np::NpMatching2;
using NoArgument = s32 (PS4_SYSV_ABI *)();
using LocalNetInfo = s32 (PS4_SYSV_ABI *)(u16, void*);
using ChatMessage = s32 (PS4_SYSV_ABI *)(u16, void*, void*, s32*);
using RequestCallback = void (PS4_SYSV_ABI *)(u16, u32, u16, int, void*, void*);
static_assert(std::is_same_v<OrbisNpMatching2RequestId, u32>);
static_assert(std::is_same_v<OrbisNpMatching2RequestCallback, RequestCallback>);
static_assert(std::is_same_v<decltype(&sceNpMatching2GetRoomMemberDataInternal), NoArgument>);
static_assert(std::is_same_v<decltype(&sceNpMatching2SetRoomMemberDataInternal), NoArgument>);
static_assert(std::is_same_v<decltype(&sceNpMatching2SignalingGetLocalNetInfo), LocalNetInfo>);
static_assert(std::is_same_v<decltype(&sceNpMatching2SendRoomChatMessage), ChatMessage>);

static void Check(bool condition) {
    if (!condition) throw std::runtime_error("NP matching compatibility contract failed");
}

static void CheckExports(Core::Loader::SymbolsResolver& symbols) {
    using namespace Core::Loader;
    constexpr std::array nids{"5lhvOqheFBA", "HoqTrkS9c5Q", "380EWm2DrVg", "opDpl74pi2E"};
    alignas(16) std::array<u8, 128> output;
    output.fill(0xa5);
    for (const char* nid : nids) {
        SymbolResolver key{.name=nid, .library="libSceNpMatching2", .library_version=1,
                           .module="libSceNpMatching2", .type=SymbolType::Function};
        const auto* registered = symbols.FindSymbol(key);
        Check(registered != nullptr);
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
        Check(registered->hle_adapter && !registered->hle_fallback);
        for (const u64 pointer : {u64{0}, ~u64{0}, reinterpret_cast<u64>(output.data())}) {
            Core::GuestCpu::HleCallFrame frame{};
            frame.gpr.fill(0xfedcba9876543210ull);
            frame.gpr[7] = 0xffff00000000002aull;
            frame.gpr[6] = pointer;
            frame.gpr[2] = pointer;
            frame.gpr[1] = pointer;
            const auto before = frame.gpr;
            const auto result = registered->hle_adapter->Invoke(frame);
            Check(std::holds_alternative<bool>(result));
            Check(frame.gpr[0] == 0);
            Check(std::equal(frame.gpr.begin()+1, frame.gpr.end(), before.begin()+1));
            Check(std::all_of(output.begin(), output.end(), [](u8 value) { return value == 0xa5; }));
        }
#endif
        key.library = "libSceNpManager";
        Check(symbols.FindSymbol(key) == nullptr);
        key.library = "libSceNpMatching2";
        key.library_version = 2;
        Check(symbols.FindSymbol(key) == nullptr);
    }
}

static void CheckRequestIdAbi(Core::Loader::SymbolsResolver& symbols) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    using namespace Core::Loader;
    using Core::GuestCpu::HleCallFrame;
    const auto find_adapter = [&](const char* nid) {
        SymbolResolver key{.name=nid, .library="libSceNpMatching2", .library_version=1,
                           .module="libSceNpMatching2", .type=SymbolType::Function};
        const auto* symbol = symbols.FindSymbol(key);
        Check(symbol != nullptr && symbol->hle_adapter && !symbol->hle_fallback);
        return symbol->hle_adapter;
    };
    const auto initialize = find_adapter("10t3e5+JPnU");
    const auto terminate = find_adapter("Mqp3lJ+sjy4");
    const auto get_worlds = find_adapter("rJNPJqDCpiI");
    const auto invoke = [](const auto& adapter, HleCallFrame& frame) {
        Check(std::holds_alternative<bool>(adapter->Invoke(frame)));
        return static_cast<s32>(frame.gpr[0]);
    };

    struct Output {
        u32 prefix{0x12345678};
        OrbisNpMatching2RequestId id{0xa5a5a5a5};
        u32 suffix{0x87654321};
    } output;
    struct Validation {
        uintptr_t output_address;
        size_t output_size{};
        bool output_writable{};
    } validation{reinterpret_cast<uintptr_t>(&output.id)};
    OrbisNpMatching2GetWorldInfoListRequest request{0xac};
    HleCallFrame frame{};
    frame.gpr[7] = 0xffff00000000002aull;
    frame.gpr[6] = reinterpret_cast<uintptr_t>(&request);
    frame.gpr[1] = reinterpret_cast<uintptr_t>(&output.id);
    frame.validate_context = &validation;
    frame.validate_range = [](void* context, uintptr_t address, size_t size, bool write) {
        auto& observed = *static_cast<Validation*>(context);
        if (address == observed.output_address) {
            observed.output_size = size;
            observed.output_writable = write;
        }
        return true;
    };
    Check(invoke(get_worlds, frame) == ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED);
    Check(output.id == 0xa5a5a5a5);
    HleCallFrame lifecycle{};
    Check(invoke(initialize, lifecycle) == 0);

    // Exercise the production output pointer and counter across the old 16-bit
    // boundary. No callback is registered and no network work is requested.
    for (u32 expected = 1; expected <= 0x10001; ++expected) {
        output.id = 0xa5a5a5a5;
        Check(invoke(get_worlds, frame) == 0);
        Check(output.id == expected);
        Check(output.prefix == 0x12345678 && output.suffix == 0x87654321);
        Check(validation.output_size == sizeof(u32) && validation.output_writable);
    }
    frame.gpr[6] = 0;
    output.id = 0xa5a5a5a5;
    Check(invoke(get_worlds, frame) == ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT);
    Check(output.id == 0xa5a5a5a5);
    Check(invoke(terminate, lifecycle) == 0);
#endif
}

int main() {
    // The production registrar has no game metadata or session prerequisites.
    Core::Loader::SymbolsResolver production;
    RegisterLib(&production);
    CheckExports(production);
    CheckRequestIdAbi(production);
    Check(sceNpMatching2GetRoomMemberDataInternal() == 0);
    Check(sceNpMatching2SetRoomMemberDataInternal() == 0);
    Check(sceNpMatching2SignalingGetLocalNetInfo(0, nullptr) == 0);
    Check(sceNpMatching2SendRoomChatMessage(0xffff, nullptr, nullptr, nullptr) == 0);
    std::puts("NP matching registrations, 32-bit request IDs and no-output ABI PASS");
}
