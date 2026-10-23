// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
// Real VM/code publication notifications, independent of the provider's
// synthetic rights oracle. Final permissions stay valid while the parent retires.
#if defined(LUMEN_WITH_QEMU_TCG) && LUMEN_WITH_QEMU_TCG
struct FreshMappingFixtureState {
    std::uint64_t callback {}, stack_page {}, code_write_address {};
    unsigned roots {}, callbacks {};
    bool code_write {}, write_succeeded {}, capture {}, child_refused {}, final_rights {};
    int protected_result {-1};
};
static FreshMappingFixtureState fresh_mapping_fixture;

static std::uint64_t NativeFreshMappingCallback() {
    auto& state = fresh_mapping_fixture;
    ++state.callbacks;
    if (state.code_write) {
        // Publish the parent's existing continuation bytes through the real
        // CPU-write path. This must retire its executable lifetime even though
        // the bytes and RWX permissions are unchanged.
        constexpr std::array<std::uint8_t, 4> continuation {0x48,0x83,0xc4,0x08};
        state.write_succeeded = Memory::WriteGuestMemory(nullptr, state.code_write_address,
            continuation.data(), continuation.size());
        std::uint64_t base {}, size {};
        std::uint32_t writable {};
        state.final_rights = Memory::QueryGuestExecutable(nullptr, state.code_write_address,
            &base, &size, &writable) != 0 && writable == 1 &&
            Memory::QueryGuestAccess(state.code_write_address, continuation.size(), Vm::Mode::ReadWrite);
        return state.write_succeeded ? 0 : UINT64_MAX;
    }
    // A real VM structural operation with unchanged final permissions must
    // still retire the detached parent's lifetime. Rights-only revalidation
    // would incorrectly admit this case.
    state.protected_result = Memory::KernelMprotect(
        reinterpret_cast<void*>(state.stack_page), 0x4000, 0x03);
    state.final_rights = Memory::QueryGuestAccess(
        state.stack_page, 0x4000, Vm::Mode::ReadWrite);
    return static_cast<std::uint64_t>(state.protected_result);
}

static std::uint64_t NativeFreshMappingRoot() {
    auto& state = fresh_mapping_fixture;
    ++state.roots;
    LumenAndroidFexCoreGuestStateV1 parent {};
    state.capture = CaptureCurrentGuestState(&parent);
    if (!state.capture) return 0;
    state.stack_page = parent.gprs[4] & ~std::uint64_t {0x3fff};
    std::uint64_t result {UINT64_MAX};
    state.child_refused = !InvokeWithoutGuestBorrow(state.callback, nullptr, 0, &result);
    return 1;
}

static bool TestFreshCallbackRetiredLifetime(std::uint64_t stack_top, bool code_write) {
    if (Lumen::GuestCpu::Selected() != Lumen::GuestCpu::Provider::QemuTcg) return false;
    if (!RegisterHostFunction<NativeFreshMappingRoot>("self-test-fresh-mapping-root") ||
        !RegisterHostFunction<NativeFreshMappingCallback>("self-test-fresh-mapping-callback"))
        return false;
    constexpr std::uint64_t page = 0x4000;
    const auto code_base = Memory::AllocateProgramMemory(
        0, page, Vm::Mode::ReadWrite, "fresh-mapping-contract-code");
    const auto witness = Memory::AllocateRuntimeMemory(
        0, page, Vm::Mode::ReadWrite, "fresh-mapping-contract-witness");
    if (code_base == 0 || witness == 0) return false;
    std::array<std::uint8_t, 128> bytes {};
    constexpr std::array<std::uint8_t, 6> prefix {0x48,0x83,0xec,0x08,0x48,0xb8};
    constexpr std::array<std::uint8_t, 6> return_to_guest {0xff,0xd0,0x48,0x83,0xc4,0x08};
    std::copy(prefix.begin(), prefix.end(), bytes.begin());
    auto target = reinterpret_cast<std::uint64_t>(NativeFreshMappingRoot);
    std::memcpy(bytes.data() + 6, &target, 8);
    std::copy(return_to_guest.begin(), return_to_guest.end(), bytes.begin() + 14);
    bytes[20] = 0x48; bytes[21] = 0xba; // movabs rdx, witness
    std::memcpy(bytes.data() + 22, &witness, 8);
    constexpr std::array<std::uint8_t, 8> denied_tail {
        0x48,0xc7,0x02,0x44,0x33,0x22,0x11,0xc3};
    std::copy(denied_tail.begin(), denied_tail.end(), bytes.begin() + 30);
    std::copy(prefix.begin(), prefix.end(), bytes.begin() + 64);
    target = reinterpret_cast<std::uint64_t>(NativeFreshMappingCallback);
    std::memcpy(bytes.data() + 70, &target, 8);
    std::copy(return_to_guest.begin(), return_to_guest.end(), bytes.begin() + 78);
    bytes[84] = 0xc3;
    const std::uint64_t untouched {};
    bool pass = Memory::WriteGuestMemory(nullptr, code_base, bytes.data(), bytes.size()) &&
        Memory::WriteGuestMemory(nullptr, witness, &untouched, sizeof(untouched)) &&
        Memory::ProtectGuestMemory(code_base, page,
            code_write ? Vm::Mode::ExecuteReadWrite : Vm::Mode::ExecuteRead);
    fresh_mapping_fixture = {};
    fresh_mapping_fixture.callback = code_base + 64;
    fresh_mapping_fixture.code_write = code_write;
    fresh_mapping_fixture.code_write_address = code_base + 16;
    std::uint64_t result {}, observed {UINT64_MAX};
    pass = pass && !Invoke(code_base, nullptr, 0, &result, stack_top) &&
        Memory::ReadGuestMemory(nullptr, witness, &observed, sizeof(observed)) && observed == 0 &&
        fresh_mapping_fixture.roots == 1 && fresh_mapping_fixture.callbacks == 1 &&
        fresh_mapping_fixture.capture && fresh_mapping_fixture.child_refused &&
        fresh_mapping_fixture.final_rights && (code_write
            ? fresh_mapping_fixture.write_succeeded
            : fresh_mapping_fixture.protected_result == ::OK);
    pass = Memory::FreeGuestMemory(witness, page) && pass;
    pass = Memory::FreeGuestMemory(code_base, page) && pass;
    return pass;
}

static bool TestFreshCallbackMappingLifetime(std::uint64_t stack_top) {
    return TestFreshCallbackRetiredLifetime(stack_top, false);
}

static bool TestFreshCallbackCodeWriteLifetime(std::uint64_t stack_top) {
    return TestFreshCallbackRetiredLifetime(stack_top, true);
}
#endif
