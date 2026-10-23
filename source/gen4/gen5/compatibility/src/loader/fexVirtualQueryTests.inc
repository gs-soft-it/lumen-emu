// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
// Included by fexRuntimeTests.cpp after its common fixture helpers.
namespace {
constexpr std::uint64_t query_page = 0x4000, query_guard = 0x766d717565727967ULL;
struct QueryObservation {
    std::array<std::uint64_t, 12> frame {};
    bool captured {};
};
std::array<std::array<std::uint64_t, 4>, 9> query_arguments {};
std::array<QueryObservation, 10> query_observations {};
std::thread query_drain_writer;
std::atomic<bool> query_writer_started {}, query_writer_done {}, query_writer_succeeded {};
bool query_completed_before_writer_admission {}, query_reader_preserved {};

std::uint64_t NativeQueryDrainProbe(const void* addr, int flags,
    Memory::VirtualQueryInfo* output, std::uint64_t size) {
    query_drain_writer = std::thread([] {
        GuestWriterGate gate(GuestWriterGate::Admission::NativeRootEntry);
        query_writer_started.store(true, std::memory_order_release);
        const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        query_writer_succeeded.store(token != 0, std::memory_order_release);
        if (token != 0) LumenAndroidFexCoreEndGuestMemoryMutationV1(token, 0, 0, 0);
        query_writer_done.store(true, std::memory_order_release);
    });
    while (!query_writer_started.load(std::memory_order_acquire)) std::this_thread::yield();
    // This peer calls the real exclusive Begin while a retained outer HLE
    // exists. It cannot complete until both current callbacks return. The
    // short wait gives it a scheduling opportunity; this test observes only
    // pending admission and eventual completion, not internal drain counters.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool pending_before = !query_writer_done.load(std::memory_order_acquire);
    const int result = Memory::KernelVirtualQuery(addr, flags, output, size);
    query_completed_before_writer_admission = pending_before && result == ::OK &&
        !query_writer_done.load(std::memory_order_acquire);
    const bool retained = LumenAndroidFexCoreRetainGuestDataLeaseV1() != 0;
    query_reader_preserved = retained && LumenAndroidFexCoreReleaseGuestDataLeaseV1() != 0;
    return static_cast<std::uint32_t>(result);
}

std::uint64_t NativeQueryOuter(std::uint64_t stack_callback,
    std::uint64_t external_callback, std::uint64_t drain_callback) {
    bool pass = true;
    for (std::size_t index = 0; index < query_arguments.size(); ++index) {
        std::uint64_t frame {};
        auto& observation = query_observations[index];
        observation.captured = Invoke(index == 0 || index == 8 ? stack_callback : external_callback,
            query_arguments[index].data(), 4, &frame) &&
            Memory::ReadGuestMemory(nullptr, frame, observation.frame.data(), sizeof(observation.frame));
        pass = observation.captured && pass;
    }
    std::uint64_t frame {};
    auto& observation = query_observations.back();
    observation.captured = Invoke(drain_callback, query_arguments[0].data(), 4, &frame) &&
        Memory::ReadGuestMemory(nullptr, frame, observation.frame.data(), sizeof(observation.frame));
    return pass && observation.captured ? 1 : 0;
}

bool TestVirtualQueryNested(std::uint64_t stack_top) {
    if (!RegisterHostFunction<Memory::KernelVirtualQuery>("self-test-virtual-query") ||
        !RegisterHostFunction<NativeQueryOuter>("self-test-virtual-query-outer") ||
        !RegisterHostFunction<NativeQueryDrainProbe>("self-test-virtual-query-drain")) return false;
    const auto scratch = Memory::AllocateRuntimeMemory(0, query_page, Vm::Mode::ReadWrite,
                                                       "fex-query-output");
    const auto code_base = Memory::AllocateProgramMemory(0, query_page, Vm::Mode::ReadWrite,
                                                         "fex-query-code");
    const auto rx_output = Memory::AllocateProgramMemory(0, query_page, Vm::Mode::ReadWrite,
                                                         "fex-query-rx-output");
    std::int64_t physical {-1};
    std::array<void*, 2> aliases {};
    if (scratch == 0 || code_base == 0 || rx_output == 0 ||
        Memory::KernelAllocateDirectMemory(0, Memory::KernelGetDirectMemorySize(),
            query_page, query_page, 3, &physical) != ::OK ||
        Memory::KernelMapDirectMemory(&aliases[0], query_page, 3, 0, physical, query_page) != ::OK ||
        Memory::KernelMapDirectMemory(&aliases[1], query_page, 0x33, 0, physical, query_page) != ::OK) return false;
    std::array<std::uint64_t, 9> guards;
    guards.fill(query_guard);
    const auto revoked_output = stack_top - 2 * query_page;
    std::array<std::uint64_t, 9> saved_stack {};
    bool pass = Memory::ReadGuestMemory(nullptr, revoked_output, saved_stack.data(), sizeof(saved_stack)) &&
        Memory::WriteGuestMemory(nullptr, revoked_output, guards.data(), sizeof(guards)) &&
        Memory::WriteGuestMemory(nullptr, scratch, guards.data(), sizeof(guards)) &&
        Memory::WriteGuestMemory(nullptr, rx_output, guards.data(), sizeof(guards)) &&
        Memory::WriteGuestMemory(nullptr, reinterpret_cast<std::uint64_t>(aliases[0]), guards.data(), sizeof(guards)) &&
        Memory::ProtectGuestMemory(rx_output, query_page, Vm::Mode::ExecuteRead) &&
        Memory::KernelMprotect(reinterpret_cast<void*>(revoked_output), query_page, 1) == ::OK;
    std::array<std::uint8_t, 1024> code {};
    constexpr std::uint8_t root_prefix[] {0x48,0x83,0xec,0x08,0x48,0xb8};
    constexpr std::uint8_t root_tail[] {0xff,0xd0,0x48,0x83,0xc4,0x08,0xc3};
    std::memcpy(code.data(), root_prefix, sizeof(root_prefix));
    Put64(code, sizeof(root_prefix), reinterpret_cast<std::uint64_t>(NativeQueryOuter));
    std::memcpy(code.data() + sizeof(root_prefix) + 8, root_tail, sizeof(root_tail));
    const auto callback = [&](std::size_t offset, bool stack_output, std::uint64_t target) {
        auto cursor = offset;
        const auto emit = [&](std::initializer_list<std::uint8_t> values) {
            for (const auto byte: values) code[cursor++] = byte;
        };
        emit({0x48,0x83,0xec,0x68,0x48,0xb8});
        Put64(code, cursor, query_guard); cursor += 8;
        for (std::uint8_t index = 0; index < 12; ++index)
            emit({0x48,0x89,0x44,0x24,static_cast<std::uint8_t>(index * 8)});
        if (stack_output) emit({0x48,0x8d,0x54,0x24,0x08});
        emit({0x48,0xb8}); Put64(code, cursor, target); cursor += 8;
        emit({0xff,0xd0,0x48,0x89,0x44,0x24,0x58,0x48,0x8d,0x04,0x24,
              0x48,0x83,0xc4,0x68,0xc3});
    };
    callback(128, true, reinterpret_cast<std::uint64_t>(Memory::KernelVirtualQuery));
    callback(256, false, reinterpret_cast<std::uint64_t>(Memory::KernelVirtualQuery));
    callback(384, true, reinterpret_cast<std::uint64_t>(NativeQueryDrainProbe));
    pass = Memory::WriteGuestMemory(nullptr, code_base, code.data(), code.size()) &&
        Memory::ProtectGuestMemory(code_base, query_page, Vm::Mode::ExecuteRead) && pass;
    const auto query_size = sizeof(Memory::VirtualQueryInfo);
    query_arguments = {{
        {scratch, 0, 0, query_size}, {scratch, 0, scratch, query_size},
        {scratch, 0, rx_output, query_size}, {scratch, 0, revoked_output, query_size},
        {scratch, 0, reinterpret_cast<std::uint64_t>(aliases[1]), query_size},
        {scratch, 0, 0, query_size}, {scratch, 2, scratch, query_size},
        {scratch, 0, scratch, query_size - 1}, {UINT64_MAX, 0, 0, query_size}
    }};
    query_observations = {};
    query_writer_started = query_writer_done = query_writer_succeeded = false;
    query_completed_before_writer_admission = query_reader_preserved = false;
    const std::array<std::uint64_t, 3> callbacks {code_base + 128, code_base + 256, code_base + 384};
    std::uint64_t value {};
    const bool invoked = Invoke(code_base, callbacks.data(), 3, &value, stack_top);
    if (query_drain_writer.joinable()) query_drain_writer.join();
    const auto result_is = [&](std::size_t index, int result) {
        const auto& observation = query_observations[index];
        return observation.captured && observation.frame[0] == query_guard && observation.frame[10] == query_guard &&
            static_cast<std::uint32_t>(observation.frame[11]) == static_cast<std::uint32_t>(result);
    };
    CheckCase("virtual-query-nested-owned-stack-snapshot-copyout",
        invoked && value == 1 && result_is(0, ::OK) && query_observations[0].frame[1] == scratch &&
        query_observations[0].frame[2] == scratch + query_page, pass);
    CheckCase("virtual-query-nested-rw-nonstack-and-gpu-alias-stay-busy",
        result_is(1, Libs::LibKernel::KERNEL_ERROR_EBUSY) && result_is(4, Libs::LibKernel::KERNEL_ERROR_EBUSY), pass);
    CheckCase("virtual-query-rx-and-revoked-stack-output-denied",
        result_is(2, Libs::LibKernel::KERNEL_ERROR_EFAULT) && result_is(3, Libs::LibKernel::KERNEL_ERROR_EFAULT), pass);
    CheckCase("virtual-query-invalid-null-flags-size-and-missing-range-no-copyout",
        result_is(5, Libs::LibKernel::KERNEL_ERROR_EINVAL) && result_is(6, Libs::LibKernel::KERNEL_ERROR_EINVAL) &&
        result_is(7, Libs::LibKernel::KERNEL_ERROR_EINVAL) && result_is(8, Libs::LibKernel::KERNEL_ERROR_EACCES) &&
        std::all_of(query_observations[8].frame.begin() + 1, query_observations[8].frame.begin() + 10,
                    [](auto item) { return item == query_guard; }), pass);
    CheckCase("virtual-query-completes-before-pending-real-peer-writer-admits",
        result_is(9, ::OK) && query_completed_before_writer_admission && query_reader_preserved &&
        query_writer_done.load() && query_writer_succeeded.load(), pass);
    for (const auto output: {scratch, rx_output, revoked_output, reinterpret_cast<std::uint64_t>(aliases[0]),
                            reinterpret_cast<std::uint64_t>(aliases[1])}) {
        std::array<std::uint64_t, 9> witness {};
        pass = Memory::ReadGuestMemory(nullptr, output, witness.data(), sizeof(witness)) && witness == guards && pass;
    }
    CheckCase("virtual-query-negative-outputs-and-all-alias-bytes-unchanged", pass, pass);
    // A normal admitted top-level guest output still uses the general writer.
    std::uint64_t frame {};
    QueryObservation top;
    const bool top_invoked = Invoke(code_base + 256, query_arguments[1].data(), 4, &frame, stack_top) &&
        Memory::ReadGuestMemory(nullptr, frame, top.frame.data(), sizeof(top.frame));
    Memory::VirtualQueryInfo general {};
    CheckCase("virtual-query-top-level-rw-nonstack-preserves-general-writer",
        top_invoked && static_cast<std::uint32_t>(top.frame[11]) == ::OK &&
        Memory::ReadGuestMemory(nullptr, scratch, &general, sizeof(general)) && general.start == scratch, pass);
    pass = Memory::KernelMprotect(reinterpret_cast<void*>(revoked_output), query_page, 3) == ::OK &&
        Memory::WriteGuestMemory(nullptr, revoked_output, saved_stack.data(), sizeof(saved_stack)) && pass;
    pass = Memory::KernelCheckedReleaseDirectMemory(physical, query_page) == ::OK && pass;
    pass = Memory::FreeGuestMemory(rx_output, query_page) && pass;
    pass = Memory::FreeGuestMemory(code_base, query_page) && pass;
    pass = Memory::FreeGuestMemory(scratch, query_page) && pass;
    return pass;
}
} // namespace
