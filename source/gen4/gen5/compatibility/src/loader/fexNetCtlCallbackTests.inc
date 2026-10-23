// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
// Exercise the real service through translated guest callbacks. The host
// observer owns only scalar registration IDs and counters, with no guest
// data borrow surviving a callback or recursive service dispatch.
namespace NetCtlFixture = Libs::Network::NetCtl;

enum class NetCtlFixtureMode { Recursive, ReplacePeer, ReplaceSelf, Terminate, ResetPeer };
struct NetCtlFixtureState {
    NetCtlFixtureMode mode {};
    std::uint64_t callback {};
    std::array<unsigned, 5> calls {};
    int self_id {-1};
    int peer_id {-1};
    bool pass {true};
};
static NetCtlFixtureState net_ctl_fixture;

static void NativeNetCtlFixtureCallback(int event, void* tag) {
    auto& state = net_ctl_fixture;
    const auto index = reinterpret_cast<std::uintptr_t>(tag);
    if (event != 1 || index == 0 || index >= state.calls.size()) {
        state.pass = false;
        return;
    }
    // Bound an accidental repeat before it can recurse indefinitely. An
    // observed duplicate still fails the contract even if dispatch completes.
    if (++state.calls[index] != 1) {
        state.pass = false;
        return;
    }
    if (index != 1) return;
    const auto callback = reinterpret_cast<NetCtlFixture::NetCtlCallback>(state.callback);
    switch (state.mode) {
    case NetCtlFixtureMode::Recursive:
        state.pass &= NetCtlFixture::NetCtlCheckCallback() == ::OK;
        break;
    case NetCtlFixtureMode::ReplacePeer: {
        int replacement {-1};
        state.pass &= NetCtlFixture::NetCtlUnregisterCallback(state.peer_id) == ::OK;
        state.pass &= NetCtlFixture::NetCtlRegisterCallback(
            callback, reinterpret_cast<void*>(3), &replacement) == ::OK;
        state.pass &= replacement == state.peer_id;
        break;
    }
    case NetCtlFixtureMode::ReplaceSelf: {
        int replacement {-1};
        state.pass &= NetCtlFixture::NetCtlUnregisterCallback(state.self_id) == ::OK;
        state.pass &= NetCtlFixture::NetCtlRegisterCallback(
            callback, reinterpret_cast<void*>(4), &replacement) == ::OK;
        state.pass &= replacement == state.self_id;
        break;
    }
    case NetCtlFixtureMode::Terminate:
        NetCtlFixture::NetCtlTerm();
        break;
    case NetCtlFixtureMode::ResetPeer: {
        NetCtlFixture::NetCtlTerm();
        int replacement_self {-1}, replacement_peer {-1};
        state.pass &= NetCtlFixture::NetCtlRegisterCallback(
            callback, reinterpret_cast<void*>(3), &replacement_self) == ::OK;
        state.pass &= NetCtlFixture::NetCtlRegisterCallback(
            callback, reinterpret_cast<void*>(4), &replacement_peer) == ::OK;
        state.pass &= replacement_self == state.self_id && replacement_peer == state.peer_id;
        break;
    }
    }
}

static bool TestNetCtlCallbacks(std::uint64_t stack_top) {
    if (!RegisterHostFunction<NativeNetCtlFixtureCallback>("self-test-netctl-callback-observer") ||
        !RegisterHostFunction<NetCtlFixture::NetCtlCheckCallback>("self-test-netctl-check-callback"))
        return false;
    constexpr std::uint64_t page = 0x4000;
    const auto code_base = Memory::AllocateProgramMemory(
        0, page, Vm::Mode::ReadWrite, "netctl-callback-contract-code");
    if (code_base == 0) return false;
    std::array<std::uint8_t, 128> bytes {};
    constexpr std::array<std::uint8_t, 6> prefix {0x48,0x83,0xec,0x08,0x48,0xb8};
    constexpr std::array<std::uint8_t, 7> suffix {0xff,0xd0,0x48,0x83,0xc4,0x08,0xc3};
    const auto relay = [&](std::size_t offset, std::uint64_t target) {
        std::copy(prefix.begin(), prefix.end(), bytes.begin() + offset);
        std::memcpy(bytes.data() + offset + prefix.size(), &target, sizeof(target));
        std::copy(suffix.begin(), suffix.end(), bytes.begin() + offset + prefix.size() + 8);
    };
    relay(0, reinterpret_cast<std::uint64_t>(NetCtlFixture::NetCtlCheckCallback));
    relay(64, reinterpret_cast<std::uint64_t>(NativeNetCtlFixtureCallback));
    bool pass = Memory::WriteGuestMemory(nullptr, code_base, bytes.data(), bytes.size()) &&
        Memory::ProtectGuestMemory(code_base, page, Vm::Mode::ExecuteRead);
    const auto dispatch = [&] {
        std::uint64_t result {UINT64_MAX};
        return Invoke(code_base, nullptr, 0, &result, stack_top) && result == ::OK;
    };
    constexpr std::array<NetCtlFixtureMode, 5> modes {
        NetCtlFixtureMode::Recursive, NetCtlFixtureMode::ReplacePeer,
        NetCtlFixtureMode::ReplaceSelf, NetCtlFixtureMode::Terminate,
        NetCtlFixtureMode::ResetPeer};
    constexpr std::array<const char*, 5> names {
        "netctl-recursive-check-delivers-each-registration-once",
        "netctl-peer-slot-reuse-skips-stale-snapshot-generation",
        "netctl-self-slot-reuse-preserves-new-pending-event",
        "netctl-term-during-callback-cancels-remaining-snapshot",
        "netctl-term-and-slot-reuse-preserves-registration-generation"};
    for (std::size_t mode = 0; mode != modes.size(); ++mode) {
        NetCtlFixture::NetCtlTerm();
        net_ctl_fixture = {};
        auto& state = net_ctl_fixture;
        state.mode = modes[mode];
        state.callback = code_base + 64;
        const auto callback = reinterpret_cast<NetCtlFixture::NetCtlCallback>(state.callback);
        bool round = pass && NetCtlFixture::NetCtlInit() == ::OK &&
            NetCtlFixture::NetCtlRegisterCallback(callback, reinterpret_cast<void*>(1),
                &state.self_id) == ::OK;
        if (state.mode != NetCtlFixtureMode::ReplaceSelf)
            round &= NetCtlFixture::NetCtlRegisterCallback(callback, reinterpret_cast<void*>(2),
                &state.peer_id) == ::OK;
        round = round && dispatch() && state.pass && state.calls[1] == 1;
        if (state.mode == NetCtlFixtureMode::Recursive)
            round &= state.calls[2] == 1;
        else if (state.mode == NetCtlFixtureMode::ReplacePeer)
            round &= state.calls[2] == 0 && state.calls[3] == 0;
        else if (state.mode == NetCtlFixtureMode::ReplaceSelf)
            round &= state.calls[4] == 0;
        else {
            round &= state.calls[2] == 0;
            if (state.mode == NetCtlFixtureMode::ResetPeer)
                round &= state.calls[3] == 0 && state.calls[4] == 0;
        }
        round = dispatch() && state.pass && state.calls[1] == 1 && round;
        if (state.mode == NetCtlFixtureMode::ReplacePeer)
            round &= state.calls[3] == 1 && state.calls[2] == 0;
        if (state.mode == NetCtlFixtureMode::ReplaceSelf)
            round &= state.calls[4] == 1;
        if (state.mode == NetCtlFixtureMode::ResetPeer)
            round &= state.calls[3] == 1 && state.calls[4] == 1 && state.calls[2] == 0;
        const auto delivered = state.calls;
        round = dispatch() && state.pass && state.calls == delivered && round;
        CheckCase(names[mode], round, pass);
    }
    NetCtlFixture::NetCtlTerm();
    pass = Memory::FreeGuestMemory(code_base, page) && pass;
    return pass;
}
