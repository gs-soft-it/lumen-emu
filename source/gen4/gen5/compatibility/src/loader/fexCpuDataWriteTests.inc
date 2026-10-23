// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
// Checked native writes between two real CPU reads/executions. The query
// observer proves QEMU keeps a warmed permission view for pure NX copyout.
struct CpuDataWriteFixture {
    enum class Operation { Write, ReadOnly, Execute } operation {Operation::Write};
    unsigned queries {}, after_host {}, calls {};
    bool write_expected {true}, native_valid {};
    bool retain_write_borrow {}, mutation_denied_with_borrow {}, borrow_balanced {};
};
static thread_local CpuDataWriteFixture* cpu_data_write_fixture {};

static std::uint64_t NativeCpuDataWrite(std::uint64_t address, std::uint64_t word) {
    auto& fixture = *cpu_data_write_fixture;
    ++fixture.calls;
    switch (fixture.operation) {
    case CpuDataWriteFixture::Operation::Write: {
        const bool retained = fixture.retain_write_borrow &&
            LumenAndroidFexCoreRetainGuestDataLeaseV1() != 0;
        const auto deny_mutation = [] {
            const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
            if (token != 0) LumenAndroidFexCoreEndGuestMemoryMutationV1(token, 0, 0, 0);
            return token == 0;
        };
        const bool denied_before = retained && deny_mutation();
        fixture.native_valid = Memory::WriteGuestMemory(nullptr, address, &word, sizeof(word)) ==
            fixture.write_expected;
        if (fixture.retain_write_borrow) {
            // An exclusive writer cannot open during this live HLE borrow.
            // A successful copy proves the stack data route kept the reader.
            fixture.mutation_denied_with_borrow = denied_before && deny_mutation();
            fixture.borrow_balanced = retained &&
                LumenAndroidFexCoreReleaseGuestDataLeaseV1() != 0 &&
                LumenAndroidFexCoreReleaseGuestDataLeaseV1() == 0;
        }
        break;
    }
    case CpuDataWriteFixture::Operation::ReadOnly:
        fixture.native_valid = Memory::ProtectGuestMemory(address & ~std::uint64_t {0x3fff},
            0x4000, Vm::Mode::Read);
        break;
    case CpuDataWriteFixture::Operation::Execute:
        fixture.native_valid = Memory::ProtectGuestMemory(address & ~std::uint64_t {0x3fff},
            0x4000, Vm::Mode::ExecuteRead);
        break;
    }
    return fixture.native_valid ? 1 : 0;
}

static std::uint32_t CpuDataWriteExecutableQuery(void* opaque, std::uint64_t address,
    std::uint64_t* base, std::uint64_t* size, std::uint32_t* writable) noexcept {
    ++static_cast<CpuDataWriteFixture*>(opaque)->queries;
    return Memory::QueryGuestExecutable(nullptr, address, base, size, writable);
}

static std::uint32_t CpuDataWriteHostCall(void* opaque, std::uint64_t target,
    std::uint64_t* gprs, LumenAndroidFexCoreSimdViewV2* simd,
    std::uint64_t* next_rip) noexcept {
    std::uint64_t return_address {};
    if (gprs == nullptr || next_rip == nullptr ||
        !Memory::ReadGuestMemory(nullptr, gprs[4], &return_address, sizeof(return_address)) ||
        !Loader::DispatchHostCall(target, gprs, simd, Memory::ReadGuestMemory, nullptr))
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    auto& fixture = *static_cast<CpuDataWriteFixture*>(opaque);
    fixture.after_host = fixture.queries;
    gprs[4] += 8;
    *next_rip = return_address;
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
}

static std::vector<std::uint8_t> CpuDataWriteCode(std::uint64_t destination,
    std::uint64_t word, std::uint64_t read_or_execute, bool execute, bool warm = true,
    bool store = false) {
    std::vector<std::uint8_t> code;
    const auto bytes = [&](std::initializer_list<std::uint8_t> input) {
        code.insert(code.end(), input.begin(), input.end());
    };
    const auto immediate = [&](std::uint64_t value) {
        const auto* data = reinterpret_cast<const std::uint8_t*>(&value);
        code.insert(code.end(), data, data + 8);
    };
    const auto access = [&](bool final) {
        bytes({0x48,0xb8}); immediate(read_or_execute);
        if (execute) bytes({0xff,0xd0}); // call rax
        else if (final && store) bytes({0x48,0x89,0x00}); // mov [rax],rax
        else bytes({0x48,0x8b,0x00}); // mov rax,[rax]
    };
    bytes({0x48,0x83,0xec,0x08});
    if (warm) access(false);
    bytes({0x48,0xbf}); immediate(destination);
    bytes({0x48,0xbe}); immediate(word);
    bytes({0x48,0xb8}); immediate(reinterpret_cast<std::uint64_t>(NativeCpuDataWrite));
    bytes({0xff,0xd0});
    if (read_or_execute != 0) access(true);
    bytes({0x48,0x83,0xc4,0x08,0xc3});
    return code;
}

static RunResult RunCpuDataWriteFixture(std::uint64_t main_code, std::uint64_t stack_top,
                                        CpuDataWriteFixture& fixture) {
    auto request = TestRequest(main_code, stack_top);
    request.native_root_entry = true;
    request.opaque = &fixture;
    request.host_call = CpuDataWriteHostCall;
    request.executable_query = CpuDataWriteExecutableQuery;
    cpu_data_write_fixture = &fixture;
    const auto result = Run(request);
    cpu_data_write_fixture = nullptr;
    return result;
}

static bool TestCpuDataWritePublication(std::uint64_t stack_top) {
    if (!RegisterHostFunction<NativeCpuDataWrite>("self-test-cpu-data-write-publication")) return false;
    constexpr std::uint64_t page = 0x4000;
    const auto main = Memory::AllocateProgramMemory(0, page, Vm::Mode::ReadWrite,
                                                   "cpu-copyout-main");
    const auto data = Memory::AllocateRuntimeMemory(0, page, Vm::Mode::ReadWrite,
                                                   "cpu-copyout-data");
    const auto writable_code = Memory::AllocateProgramMemory(0, page * 2, Vm::Mode::ReadWrite,
                                                             "cpu-copyout-code");
    if (main == 0 || data == 0 || writable_code == 0) return false;
    bool pass = true;
    const bool qemu = Lumen::GuestCpu::Selected() == Lumen::GuestCpu::Provider::QemuTcg;
    const auto install_main = [&](const auto& code) {
        return Memory::ProtectGuestMemory(main, page, Vm::Mode::ReadWrite) &&
            Memory::WriteGuestMemory(nullptr, main, code.data(), code.size()) &&
            Memory::ProtectGuestMemory(main, page, Vm::Mode::ExecuteRead);
    };
    const std::array<std::uint8_t, 8> before {0xb8,42,0,0,0,0xc3,0x90,0x90};
    auto after = before;
    after[1] = 43;
    std::uint64_t after_word {};
    std::memcpy(&after_word, after.data(), sizeof(after_word));
    const std::uint64_t old_data = 17, new_data = 29;
    pass &= Memory::WriteGuestMemory(nullptr, data, &old_data, sizeof(old_data));
    pass &= install_main(CpuDataWriteCode(data, new_data, data, false));
    CpuDataWriteFixture nx {};
    const auto copied = RunCpuDataWriteFixture(main, stack_top, nx);
    CheckCase("cpu-nx-copyout-visible-with-warm-permission-view", copied.completed &&
        copied.state.gprs[0] == new_data && nx.calls == 1 && nx.native_valid &&
        (!qemu || nx.queries == nx.after_host), pass);

    std::uint64_t stack_base {}, stack_size {};
    if (!Libs::LibKernel::PthreadGetGuestStack(Libs::LibKernel::PthreadSelf(),
            &stack_base, &stack_size) || stack_size < page * 3 ||
        stack_base > UINT64_MAX - stack_size) return false;
    const auto stack_output = stack_base + page + 256;
    const std::array<std::uint64_t, 3> stack_canaries {
        0x1234567890abcdefULL, old_data, 0xfedcba0987654321ULL};
    auto stack_expected = stack_canaries;
    stack_expected[1] = new_data;
    pass &= Memory::WriteGuestMemory(nullptr, stack_output - 8,
        stack_canaries.data(), sizeof(stack_canaries)) &&
        install_main(CpuDataWriteCode(stack_output, new_data, stack_output, false));
    CpuDataWriteFixture owned_stack {};
    owned_stack.retain_write_borrow = true;
    const auto stack_copied = RunCpuDataWriteFixture(main, stack_top, owned_stack);
    std::array<std::uint64_t, 3> stack_after {};
    const bool stack_observed = Memory::ReadGuestMemory(nullptr, stack_output - 8,
        stack_after.data(), sizeof(stack_after));
    CheckCase("cpu-owned-stack-copyout-keeps-reader-and-cannot-open-epoch",
        stack_copied.completed && stack_copied.state.gprs[0] == new_data &&
        owned_stack.calls == 1 && owned_stack.native_valid &&
        owned_stack.mutation_denied_with_borrow && owned_stack.borrow_balanced, pass);
    CheckCase("cpu-owned-stack-copyout-preserves-adjacent-canaries",
        stack_observed && stack_after == stack_expected, pass);

    const auto split_stack_output = stack_base + 2 * page - 4;
    pass &= Memory::WriteGuestMemory(nullptr, split_stack_output - 8,
        stack_canaries.data(), sizeof(stack_canaries)) &&
        install_main(CpuDataWriteCode(split_stack_output, new_data, split_stack_output, false));
    CpuDataWriteFixture split_stack {};
    split_stack.retain_write_borrow = true;
    const auto split_stack_copied = RunCpuDataWriteFixture(main, stack_top, split_stack);
    stack_after.fill(0);
    CheckCase("cpu-owned-stack-cross-page-copyout-keeps-reader-and-canaries",
        split_stack_copied.completed && split_stack_copied.state.gprs[0] == new_data &&
        split_stack.native_valid && split_stack.mutation_denied_with_borrow &&
        split_stack.borrow_balanced && Memory::ReadGuestMemory(nullptr,
            split_stack_output - 8, stack_after.data(), sizeof(stack_after)) &&
        stack_after == stack_expected, pass);
    const auto next_stack_page = stack_base + 2 * page;
    pass &= Memory::KernelMprotect(reinterpret_cast<void*>(next_stack_page), page, 1) == ::OK &&
        install_main(CpuDataWriteCode(split_stack_output, 51, split_stack_output, false));
    CpuDataWriteFixture split_stack_denied {};
    split_stack_denied.write_expected = false;
    const auto split_stack_rejected = RunCpuDataWriteFixture(main, stack_top, split_stack_denied);
    stack_after.fill(0);
    CheckCase("cpu-owned-stack-cross-page-read-only-tail-has-no-partial-write",
        split_stack_rejected.completed && split_stack_rejected.state.gprs[0] == new_data &&
        split_stack_denied.native_valid && Memory::ReadGuestMemory(nullptr,
            split_stack_output - 8, stack_after.data(), sizeof(stack_after)) &&
        stack_after == stack_expected, pass);
    pass &= Memory::KernelMprotect(reinterpret_cast<void*>(next_stack_page), page, 3) == ::OK;

    const auto stack_page = stack_base + page;
    for (const auto protection: {1, 5}) {
        pass &= Memory::KernelMprotect(reinterpret_cast<void*>(stack_page), page,
            protection) == ::OK &&
            install_main(CpuDataWriteCode(stack_output, 51, stack_output, false));
        CpuDataWriteFixture stack_denied {};
        stack_denied.write_expected = false;
        const auto stack_rejected = RunCpuDataWriteFixture(main, stack_top, stack_denied);
        stack_after.fill(0);
        const bool unchanged = Memory::ReadGuestMemory(nullptr, stack_output - 8,
            stack_after.data(), sizeof(stack_after)) && stack_after == stack_expected;
        CheckCase(protection == 1 ? "cpu-owned-stack-read-only-copyout-refused-without-effects"
                                 : "cpu-owned-stack-executable-read-only-copyout-refused-without-effects",
            stack_rejected.completed && stack_rejected.state.gprs[0] == new_data &&
            stack_denied.calls == 1 && stack_denied.native_valid && unchanged, pass);
        pass &= Memory::KernelMprotect(reinterpret_cast<void*>(stack_page), page, 3) == ::OK;
    }

    pass &= Memory::KernelMprotect(reinterpret_cast<void*>(stack_page), page, 2) == ::OK &&
        install_main(CpuDataWriteCode(stack_output, 51, 0, false, false));
    CpuDataWriteFixture write_only_stack {};
    const auto write_only_copied = RunCpuDataWriteFixture(main, stack_top, write_only_stack);
    pass &= Memory::KernelMprotect(reinterpret_cast<void*>(stack_page), page, 3) == ::OK;
    stack_expected[1] = 51;
    stack_after.fill(0);
    CheckCase("cpu-owned-write-only-stack-copyout-preserves-writer-fallback",
        write_only_copied.completed && write_only_copied.state.gprs[0] == 1 &&
        write_only_stack.calls == 1 && write_only_stack.native_valid &&
        Memory::ReadGuestMemory(nullptr, stack_output - 8, stack_after.data(), sizeof(stack_after)) &&
        stack_after == stack_expected, pass);

    const auto other_stack = Memory::AllocateGuestStackMemory(0, page, Vm::Mode::ReadWrite,
                                                            "cpu-copyout-other-stack");
    if (other_stack == 0) return false;
    pass &= Memory::WriteGuestMemory(nullptr, other_stack, &old_data, sizeof(old_data)) &&
        install_main(CpuDataWriteCode(other_stack, new_data, other_stack, false));
    CpuDataWriteFixture other_stack_write {};
    const auto other_copied = RunCpuDataWriteFixture(main, stack_top, other_stack_write);
    CheckCase("cpu-nonowned-stack-copyout-keeps-original-writer-contract",
        other_copied.completed && other_copied.state.gprs[0] == new_data &&
        other_stack_write.calls == 1 && other_stack_write.native_valid, pass);
    pass &= Memory::KernelMunmap(other_stack, page) == ::OK;

    pass &= Memory::WriteGuestMemory(nullptr, writable_code + page, before.data(), before.size()) &&
        Memory::ProtectGuestMemory(writable_code + page, page, Vm::Mode::ExecuteReadWrite) &&
        install_main(CpuDataWriteCode(writable_code + page, after_word, writable_code + page, true));
    CpuDataWriteFixture wx {};
    const auto rewritten = RunCpuDataWriteFixture(main, stack_top, wx);
    CheckCase(qemu ? "cpu-wx-copyout-retires-warm-translated-code"
                   : "cpu-fex-wx-execution-refused-before-copyout",
        qemu ? rewritten.completed && rewritten.state.gprs[0] == 43 && wx.calls == 1 &&
            wx.native_valid && wx.queries > wx.after_host
        : !rewritten.completed &&
            rewritten.code == LUMEN_ANDROID_FEXCORE_RUNTIME_EXECUTABLE_RANGE_DENIED_V1 &&
            wx.calls == 0, pass);

    // A split write begins in NX storage and ends in a WX code page. The
    // complete checked span must not be classified from its first byte alone.
    const auto split = writable_code + page - 4;
    std::array<std::uint8_t, 8> split_before {0x90,0x90,0x90,0x90,0xb8,42,0,0};
    auto split_after = split_before;
    split_after[5] = 44;
    std::uint64_t split_word {};
    std::memcpy(&split_word, split_after.data(), 8);
    pass &= Memory::WriteGuestMemory(nullptr, split, split_before.data(), split_before.size()) &&
        install_main(CpuDataWriteCode(split, split_word, writable_code + page, true));
    CpuDataWriteFixture mixed {};
    const auto crossed = RunCpuDataWriteFixture(main, stack_top, mixed);
    CheckCase(qemu ? "cpu-mixed-nx-wx-copyout-retires-code"
                   : "cpu-fex-mixed-wx-execution-refused-before-copyout",
        qemu ? crossed.completed && crossed.state.gprs[0] == 44 && mixed.calls == 1 && mixed.native_valid
        : !crossed.completed &&
            crossed.code == LUMEN_ANDROID_FEXCORE_RUNTIME_EXECUTABLE_RANGE_DENIED_V1 &&
            mixed.calls == 0, pass);

    pass &= Memory::ProtectGuestMemory(data, page, Vm::Mode::Read) &&
        install_main(CpuDataWriteCode(data, 51, data, false));
    CpuDataWriteFixture denied {};
    denied.write_expected = false;
    const auto rejected = RunCpuDataWriteFixture(main, stack_top, denied);
    CheckCase("cpu-read-only-copyout-refused-without-effects", rejected.completed &&
        rejected.state.gprs[0] == new_data && denied.calls == 1 && denied.native_valid &&
        (!qemu || denied.queries == denied.after_host), pass);

    pass &= Memory::ProtectGuestMemory(data, page, Vm::Mode::ReadWrite);
    CpuDataWriteFixture revoked {};
    revoked.operation = CpuDataWriteFixture::Operation::ReadOnly;
    // Revocation of write permission must be visible to the following CPU
    // store, independently of preserving warmed views for ordinary copies.
    pass &= install_main(CpuDataWriteCode(data, 0, data, false, true, true));
    const auto retired = RunCpuDataWriteFixture(main, stack_top, revoked);
    CheckCase("cpu-protect-revocation-retires-warm-permission-view", !retired.completed &&
        retired.code == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 && revoked.native_valid &&
        retired.state.faultAddress == data, pass);

    pass &= Memory::ProtectGuestMemory(writable_code, page, Vm::Mode::ReadWrite) &&
        Memory::WriteGuestMemory(nullptr, writable_code, after.data(), after.size()) &&
        install_main(CpuDataWriteCode(writable_code, 0, writable_code, true, false));
    CpuDataWriteFixture upgraded {};
    upgraded.operation = CpuDataWriteFixture::Operation::Execute;
    const auto executable = RunCpuDataWriteFixture(main, stack_top, upgraded);
    CheckCase("cpu-nx-to-executable-protect-publishes-written-code", executable.completed &&
        executable.state.gprs[0] == 43 && upgraded.native_valid, pass);

    std::int64_t physical = -1;
    void* source = nullptr;
    void* alias = nullptr;
    const auto allocate_result = Memory::KernelAllocateDirectMemory(0,
        static_cast<std::int64_t>(Memory::KernelGetDirectMemorySize()), page, page, 11,
        &physical);
    const auto source_result = allocate_result == ::OK
        ? Memory::KernelMapDirectMemory(&source, page, 0x03, 0, physical, page) : -1;
    const auto alias_result = source_result == ::OK
        ? Memory::KernelMapDirectMemory(&alias, page, 0x03, 0, physical, page) : -1;
    // Direct-map admission correctly rejects EXEC. Exercise the existing
    // checked protection transition after both physical mappings exist.
    const auto execute_result = alias_result == ::OK
        ? Memory::KernelMprotect(alias, page, 0x05) : -1;
    bool alias_valid = allocate_result == ::OK && source_result == ::OK &&
        alias_result == ::OK && execute_result == ::OK;
    if (alias_valid) {
        const auto source_base = reinterpret_cast<std::uint64_t>(source);
        const auto alias_base = reinterpret_cast<std::uint64_t>(alias);
        alias_valid = Memory::WriteGuestMemory(nullptr, source_base, before.data(), before.size()) &&
            install_main(CpuDataWriteCode(source_base, after_word, alias_base, true));
        CpuDataWriteFixture alias_write {};
        const auto aliased = RunCpuDataWriteFixture(main, stack_top, alias_write);
        std::printf("PS5_CPU_ALIAS_WRITE_TEST setup=%u code=%u completed=%u calls=%u native=%u value=%llu fault=%#llx\n",
            alias_valid ? 1U : 0U, aliased.code, aliased.completed ? 1U : 0U,
            alias_write.calls, alias_write.native_valid ? 1U : 0U,
            static_cast<unsigned long long>(aliased.state.gprs[0]),
            static_cast<unsigned long long>(aliased.state.faultAddress));
        alias_valid = alias_valid && aliased.completed && aliased.state.gprs[0] == 43 &&
            alias_write.native_valid;
    }
    std::printf("PS5_CPU_ALIAS_WRITE_TEST allocation=%d source_mapping=%d alias_mapping=%d executable_protection=%d\n",
        allocate_result, source_result, alias_result, execute_result);
    CheckCase("cpu-nx-physical-alias-write-retires-executable-va", alias_valid, pass);
    if (physical >= 0) pass &= Memory::KernelCheckedReleaseDirectMemory(physical, page) == ::OK;
    pass &= Memory::FreeGuestMemory(writable_code, page * 2);
    pass &= Memory::FreeGuestMemory(data, page);
    pass &= Memory::FreeGuestMemory(main, page);
    return pass;
}
