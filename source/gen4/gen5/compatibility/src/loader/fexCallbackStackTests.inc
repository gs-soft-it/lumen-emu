// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
// Included by fexRuntimeTests.cpp after its common fixture helpers.
namespace {
constexpr std::uint64_t callback_stack_canary = 0x7363616c6c626163ULL;
constexpr std::uint64_t callback_stack_sentinel = 0x13579bdf2468ace0ULL;
bool callback_stack_outer_preserved {};
bool callback_stack_inner_preserved {};
bool callback_stack_nested_mutation_denied {};
bool callback_stack_leaf_lease_balanced {};

using CallbackStackFrame = std::array<std::uint8_t, 144>;
bool CaptureCallbackStackFrame(CallbackStackFrame& bytes, std::uint64_t& address) {
    LumenAndroidFexCoreGuestStateV1 state {};
    if (!CaptureCurrentGuestState(&state) || state.gprs[4] < 128) return false;
    address = state.gprs[4] - 128;
    if (!Memory::ReadGuestMemory(nullptr, address, bytes.data(), bytes.size())) return false;
    std::uint64_t canary {};
    std::memcpy(&canary, bytes.data() + 64, sizeof(canary));
    return canary == callback_stack_canary;
}

bool CallbackStackFrameUnchanged(const CallbackStackFrame& before, std::uint64_t address) {
    CallbackStackFrame after {};
    return Memory::ReadGuestMemory(nullptr, address, after.data(), after.size()) && before == after;
}

std::uint64_t NativeCallbackStackLeaf(std::uint64_t a, std::uint64_t b,
    std::uint64_t c, std::uint64_t d, std::uint64_t e, std::uint64_t f,
    std::uint64_t g, std::uint64_t h) {
    // The stack-data writer must not grant destructive VM admission to any
    // retained outer HLE, and every preparation borrow must end before reentry.
    const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    callback_stack_nested_mutation_denied = token == 0;
    if (token != 0) LumenAndroidFexCoreEndGuestMemoryMutationV1(token, 0, 0, 0);
    callback_stack_leaf_lease_balanced = LumenAndroidFexCoreRetainGuestDataLeaseV1() != 0;
    if (callback_stack_leaf_lease_balanced)
        callback_stack_leaf_lease_balanced = LumenAndroidFexCoreReleaseGuestDataLeaseV1() != 0;
    return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h;
}

std::uint64_t NativeCallbackStackInner(std::uint64_t target) {
    CallbackStackFrame before {};
    std::uint64_t address {}, value {};
    const bool captured = CaptureCallbackStackFrame(before, address);
    const std::array<std::uint64_t, 8> arguments {1, 2, 3, 4, 5, 6, 7, 8};
    const bool invoked = Invoke(target, arguments.data(), arguments.size(), &value);
    callback_stack_inner_preserved = captured && CallbackStackFrameUnchanged(before, address);
    return invoked ? value : UINT64_MAX;
}

std::uint64_t NativeCallbackStackOuter(std::uint64_t target, std::uint64_t inner_target) {
    CallbackStackFrame before {};
    std::uint64_t address {}, value {};
    const bool captured = CaptureCallbackStackFrame(before, address);
    const bool invoked = Invoke(target, &inner_target, 1, &value);
    callback_stack_outer_preserved = captured && CallbackStackFrameUnchanged(before, address);
    return invoked ? value : UINT64_MAX;
}

std::uint64_t NativeCallbackStackProbe(std::uint64_t rsp, std::uint64_t count,
    std::uint64_t flags, std::uint64_t root_return) {
    const std::array<std::uint64_t, 2> spills {0x1020304050607080ULL, 0x8070605040302010ULL};
    const bool prepared = Memory::PrepareGuestCallbackStack(nullptr, rsp, root_return,
        (flags & 1) != 0 ? nullptr : spills.data(), static_cast<std::size_t>(count));
    return prepared == ((flags & 2) != 0) ? 1 : 0;
}

std::uint64_t NativeHostStackReadProbe(std::uint64_t address, std::uint64_t flags,
                                     std::uint64_t expected) {
    std::uint64_t result = callback_stack_sentinel;
    const bool copied = Memory::TryReadOwnedGuestStackForHostCall(address, &result, sizeof(result));
    bool valid = copied == ((flags & 1) != 0) &&
        result == (copied ? expected : callback_stack_sentinel);
    result = callback_stack_sentinel;
    const bool fallback = Memory::ReadGuestMemoryForHostCall(nullptr, address, &result, sizeof(result));
    valid &= fallback == ((flags & 2) != 0) &&
        result == (fallback ? expected : callback_stack_sentinel);
    const auto lease = LumenAndroidFexCoreRetainGuestDataLeaseV1();
    valid &= lease != 0 && LumenAndroidFexCoreReleaseGuestDataLeaseV1() != 0;
    return valid ? 1 : 0;
}
} // namespace

bool CallbackStackSelfTest(std::uint64_t stack_top) {
    if (!RegisterHostFunction<NativeCallbackStackOuter>("self-test-callback-stack-outer") ||
        !RegisterHostFunction<NativeCallbackStackInner>("self-test-callback-stack-inner") ||
        !RegisterHostFunction<NativeCallbackStackLeaf>("self-test-callback-stack-leaf") ||
        !RegisterHostFunction<NativeCallbackStackProbe>("self-test-callback-stack-probe") ||
        !RegisterHostFunction<NativeHostStackReadProbe>("self-test-host-stack-read-probe")) return false;
    std::uint64_t stack_base {}, stack_size {};
    if (!Libs::LibKernel::PthreadGetGuestStack(Libs::LibKernel::PthreadSelf(),
            &stack_base, &stack_size) || stack_size < 0x30000 ||
        stack_base > UINT64_MAX - stack_size || stack_top < stack_base + 0x20000) return false;

    const auto code_base = Memory::AllocateProgramMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                         "fex-callback-stack-code");
    const auto runtime_data = Memory::AllocateRuntimeMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                            "fex-callback-stack-runtime-denial");
    const auto other_stack = Memory::AllocateGuestStackMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                              "fex-callback-stack-other-owner");
    if (code_base == 0 || runtime_data == 0 || other_stack == 0) {
        if (code_base != 0) Memory::FreeGuestMemory(code_base, 0x4000);
        if (runtime_data != 0) Memory::FreeGuestMemory(runtime_data, 0x4000);
        if (other_stack != 0) Memory::KernelMunmap(other_stack, 0x4000);
        return false;
    }
    bool pass = true;
    std::array<std::uint8_t, 1024> code {};
    const auto canary_relay = [&](std::size_t offset, std::uint64_t target) {
        constexpr std::uint8_t prefix[] {0x48,0x83,0xec,0x08,0x48,0xb8};
        constexpr std::uint8_t store_and_load[] {0x48,0x89,0x44,0x24,0xc0,0x48,0xb8};
        constexpr std::uint8_t tail[] {0xff,0xd0,0x48,0x83,0xc4,0x08,0xc3};
        std::memcpy(code.data() + offset, prefix, sizeof(prefix));
        Put64(code, offset + 6, callback_stack_canary);
        std::memcpy(code.data() + offset + 14, store_and_load, sizeof(store_and_load));
        Put64(code, offset + 21, target);
        std::memcpy(code.data() + offset + 29, tail, sizeof(tail));
    };
    canary_relay(0, reinterpret_cast<std::uint64_t>(NativeCallbackStackOuter));
    canary_relay(64, reinterpret_cast<std::uint64_t>(NativeCallbackStackInner));
    // The inner callback forwards both spilled arguments to the real typed HLE.
    constexpr std::uint8_t sum_prefix[] {
        0x48,0x83,0xec,0x18,0x48,0x8b,0x44,0x24,0x20,0x48,0x89,0x04,0x24,
        0x48,0x8b,0x44,0x24,0x28,0x48,0x89,0x44,0x24,0x08,0x48,0xb8
    };
    std::memcpy(code.data() + 128, sum_prefix, sizeof(sum_prefix));
    Put64(code, 128 + sizeof(sum_prefix), reinterpret_cast<std::uint64_t>(NativeCallbackStackLeaf));
    constexpr std::uint8_t sum_tail[] {0xff,0xd0,0x48,0x83,0xc4,0x18,0xc3};
    std::memcpy(code.data() + 136 + sizeof(sum_prefix), sum_tail, sizeof(sum_tail));
    canary_relay(256, reinterpret_cast<std::uint64_t>(NativeCallbackStackProbe));
    canary_relay(320, reinterpret_cast<std::uint64_t>(NativeHostStackReadProbe));
    pass &= Memory::WriteGuestMemory(nullptr, code_base, code.data(), code.size());
    Memory::SetProgramMemoryProtection(code_base, 0x4000, Vm::Mode::ExecuteRead);

    callback_stack_outer_preserved = callback_stack_inner_preserved = false;
    callback_stack_nested_mutation_denied = callback_stack_leaf_lease_balanced = false;
    const std::array<std::uint64_t, 2> nested {code_base + 64, code_base + 128};
    std::uint64_t value {};
    const bool invoked = Invoke(code_base, nested.data(), nested.size(), &value, stack_top);
    CheckCase("callback-stack-two-retained-hle-levels-and-eight-arguments", invoked && value == 204, pass);
    CheckCase("callback-stack-preserves-parent-red-zones-and-return-frames", invoked &&
        callback_stack_outer_preserved && callback_stack_inner_preserved, pass);
    CheckCase("callback-stack-does-not-admit-nested-destructive-mutation", invoked &&
        callback_stack_nested_mutation_denied && callback_stack_leaf_lease_balanced, pass);

    const std::array<std::uint64_t, 5> witness {
        0x0011223344556677ULL,0x8899aabbccddeeffULL,0xfedcba9876543210ULL,
        0x1029384756abcdefULL,0x76543210fedcba98ULL
    };
    const auto probe_rsp = stack_base + 0x20108;
    pass &= Memory::WriteGuestMemory(nullptr, probe_rsp - 16, witness.data(), sizeof(witness));
    std::uint64_t outside_read = callback_stack_sentinel;
    CheckCase("host-stack-read-requires-counted-hle-reader",
        !Memory::TryReadOwnedGuestStackForHostCall(probe_rsp - 16, &outside_read, sizeof(outside_read)) &&
        outside_read == callback_stack_sentinel, pass);
    const auto stack_read = [&](const char* name, std::uint64_t address,
                                std::uint64_t flags, std::uint64_t expected) {
        const std::array<std::uint64_t, 3> args {address, flags, expected};
        CheckCase(name, Invoke(code_base + 320, args.data(), args.size(), &value,
            stack_top - 0x10000) && value == 1, pass);
    };
    stack_read("host-stack-read-private-owner-and-balanced-borrow", probe_rsp - 16, 3, witness[0]);
    stack_read("host-stack-read-null-denied-without-output", 0, 0, 0);
    stack_read("host-stack-read-overflow-denied-without-output", UINT64_MAX - 3, 0, 0);
    const std::array<std::uint64_t, 2> spills {1, 2};
    std::array<std::uint64_t, 5> after {};
    const bool outside_denied = !Memory::PrepareGuestCallbackStack(nullptr, probe_rsp,
        callback_stack_sentinel, spills.data(), spills.size());
    CheckCase("callback-stack-requires-active-hle-reader", outside_denied &&
        Memory::ReadGuestMemory(nullptr, probe_rsp - 16, after.data(), sizeof(after)) &&
        after == witness, pass);
    const std::array<std::uint64_t, 4> prepare_args {probe_rsp, 2, 2, callback_stack_sentinel};
    auto prepared_witness = witness;
    prepared_witness[2] = callback_stack_sentinel;
    prepared_witness[3] = 0x1020304050607080ULL;
    prepared_witness[4] = 0x8070605040302010ULL;
    CheckCase("callback-stack-writes-only-sentinel-and-spills", Invoke(code_base + 256,
        prepare_args.data(), prepare_args.size(), &value, stack_top) && value == 1 &&
        Memory::ReadGuestMemory(nullptr, probe_rsp - 16, after.data(), sizeof(after)) &&
        after == prepared_witness, pass);

    // Use a lower root stack for negative probes so even the end-of-stack
    // witness cannot be changed by the legitimate root's return sentinel.
    const auto denied = [&](const char* name, std::uint64_t rsp, std::uint64_t count,
        bool null_spills = false, std::uint64_t observe = 0, std::size_t bytes = 0) {
        std::array<std::uint8_t, 40> before {}, result {};
        const bool readable = bytes == 0 || (bytes <= before.size() &&
            Memory::ReadGuestMemory(nullptr, observe, before.data(), bytes));
        const std::array<std::uint64_t, 4> args {rsp, count, null_spills ? 1ULL : 0ULL,
                                               callback_stack_sentinel};
        const bool called = Invoke(code_base + 256, args.data(), args.size(), &value,
                                   stack_top - 0x10000);
        const bool unchanged = bytes == 0 || (Memory::ReadGuestMemory(nullptr, observe,
            result.data(), bytes) && before == result);
        CheckCase(name, readable && called && value == 1 && unchanged, pass);
    };
    denied("callback-stack-null-address-rejected", 0, 2);
    denied("callback-stack-guard-rejected", stack_base - 8, 2);
    denied("callback-stack-overflow-rejected", UINT64_MAX - 7, 2);
    denied("callback-stack-spill-overflow-has-no-effects", probe_rsp, UINT64_MAX,
           false, probe_rsp - 16, sizeof(witness));
    denied("callback-stack-null-spills-have-no-effects", probe_rsp, 2,
           true, probe_rsp - 16, sizeof(witness));
    denied("callback-stack-code-owner-rejected", code_base + 520, 2,
           false, code_base + 504, sizeof(witness));
    pass &= Memory::WriteGuestMemory(nullptr, runtime_data + 248, witness.data(), sizeof(witness));
    stack_read("host-stack-read-runtime-uses-fallback", runtime_data + 248, 2, witness[0]);
    denied("callback-stack-runtime-owner-has-no-effects", runtime_data + 264, 2,
           false, runtime_data + 248, sizeof(witness));
    pass &= Memory::WriteGuestMemory(nullptr, other_stack + 248, witness.data(), sizeof(witness));
    stack_read("host-stack-read-other-owner-uses-fallback", other_stack + 248, 2, witness[0]);
    denied("callback-stack-other-stack-owner-has-no-effects", other_stack + 264, 2,
           false, other_stack + 248, sizeof(witness));
    const auto stack_end = stack_base + stack_size;
    pass &= Memory::WriteGuestMemory(nullptr, stack_end - 24, witness.data(), 24);
    denied("callback-stack-crosses-owner-end-without-partial-write", stack_end - 8, 2,
           false, stack_end - 24, 24);

    alignas(16) std::array<std::uint64_t, 8> native_buffer {};
    const auto native_before = native_buffer;
    denied("callback-stack-native-buffer-rejected",
           reinterpret_cast<std::uint64_t>(native_buffer.data()) + 24, 2);
    CheckCase("callback-stack-native-buffer-unchanged", native_buffer == native_before, pass);
    const auto protected_page = stack_base + 0x4000;
    const auto protected_rsp = protected_page + 264;
    pass &= Memory::WriteGuestMemory(nullptr, protected_rsp - 16, witness.data(), sizeof(witness));
    const auto protected_denial = [&](const char* name, int protection) {
        const bool protected_ok = Memory::KernelMprotect(reinterpret_cast<void*>(protected_page),
                                                         0x4000, protection) == ::OK;
        if (protected_ok) {
            denied(name, protected_rsp, 2, false, protected_rsp - 16, sizeof(witness));
            stack_read(protection == 1 ? "host-stack-read-ro-snapshot-supported" :
                protection == 7 ? "host-stack-read-executable-uses-fallback" :
                                  "host-stack-read-gpu-mapping-uses-fallback",
                protected_rsp - 16, protection == 1 ? 3 : 2, witness[0]);
        }
        else CheckCase(name, false, pass);
        pass &= Memory::KernelMprotect(reinterpret_cast<void*>(protected_page), 0x4000, 3) == ::OK;
    };
    protected_denial("callback-stack-read-only-has-no-effects", 1);
    protected_denial("callback-stack-executable-stack-has-no-effects", 7);
    protected_denial("callback-stack-gpu-stack-has-no-effects", 0x33);
    const bool no_access = Memory::KernelMprotect(reinterpret_cast<void*>(protected_page),
                                                 0x4000, 0) == ::OK;
    if (no_access) stack_read("host-stack-read-revoked-rights-denied", protected_rsp - 16, 0, 0);
    else CheckCase("host-stack-read-noaccess-protection", false, pass);
    pass &= Memory::KernelMprotect(reinterpret_cast<void*>(protected_page), 0x4000, 3) == ::OK;
    stack_read("host-stack-read-restored-rights-uses-new-snapshot", protected_rsp - 16, 3, witness[0]);

    // A leaked preparation borrow would prevent this ordinary root-side epoch.
    const auto mutation = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    const bool mutation_closed = mutation != 0 &&
        LumenAndroidFexCoreEndGuestMemoryMutationV2(mutation, 0, 0, 0, nullptr, nullptr) != 0;
    CheckCase("callback-stack-preparation-borrows-fully-released", mutation_closed, pass);
    pass &= Memory::FreeGuestMemory(code_base, 0x4000);
    pass &= Memory::FreeGuestMemory(runtime_data, 0x4000);
    pass &= Memory::KernelMunmap(other_stack, 0x4000) == ::OK;
    return pass;
}
