// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
// Included by fexRuntimeTests.cpp after its common fixture helpers.
namespace {
constexpr std::uint64_t direct_allocation_page = 0x4000;
constexpr std::uint64_t direct_allocation_guard = 0x6469726563746775ULL;
constexpr std::uint64_t direct_allocation_sentinel = 0x13579bdf2468ace0ULL;

struct DirectAllocationObservation {
    std::array<std::uint64_t, 4> frame {};
    bool captured {};
};
std::array<DirectAllocationObservation, 9> direct_allocation_observations {};
bool direct_allocation_mutation_denied {};

std::uint64_t NativeDirectAllocationMutationProbe() {
    const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    if (token != 0) LumenAndroidFexCoreEndGuestMemoryMutationV1(token, 0, 0, 0);
    return token == 0 ? 1 : 0;
}

std::uint64_t NativeDirectAllocationOuter(std::uint64_t callback,
    std::uint64_t null_callback, std::uint64_t mutation_callback,
    std::uint64_t begin, std::uint64_t end) {
    // Each Invoke retains this outer HLE while the guest calls the real
    // six-argument allocation wrapper at the second HLE level.
    const std::array<std::array<std::uint64_t, 5>, 9> arguments {{
        {UINT64_MAX, end, direct_allocation_page, direct_allocation_page, 3},
        {begin, begin, direct_allocation_page, direct_allocation_page, 3},
        {begin, end, 0, direct_allocation_page, 3},
        {begin, end, direct_allocation_page + 1, direct_allocation_page, 3},
        {begin, end, direct_allocation_page, 1, 3},
        {begin, end, direct_allocation_page, direct_allocation_page, 3},
        {begin, end, direct_allocation_page, direct_allocation_page, 3},
        {begin, end, direct_allocation_page, direct_allocation_page, 3},
        {begin, end, direct_allocation_page, direct_allocation_page, 3}
    }};
    bool invoked = true;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        std::uint64_t frame {};
        auto& observation = direct_allocation_observations[index];
        const bool called = Invoke(index == 5 ? null_callback : callback,
            arguments[index].data(), arguments[index].size(), &frame);
        observation.captured = called && Memory::ReadGuestMemory(nullptr, frame,
            observation.frame.data(), sizeof(observation.frame));
        invoked &= observation.captured;
    }
    std::uint64_t denied {};
    direct_allocation_mutation_denied = Invoke(mutation_callback, nullptr, 0, &denied) && denied == 1;
    return invoked ? 1 : 0;
}
} // namespace

bool TestDirectAllocationNested(std::uint64_t stack_top) {
    if (!RegisterHostFunction<Memory::KernelAllocateDirectMemory>("self-test-direct-allocation") ||
        !RegisterHostFunction<NativeDirectAllocationOuter>("self-test-direct-allocation-outer") ||
        !RegisterHostFunction<NativeDirectAllocationMutationProbe>("self-test-direct-allocation-mutation"))
        return false;

    std::int64_t window_begin {};
    std::size_t available {};
    const auto physical_size = Memory::KernelGetDirectMemorySize();
    if (physical_size > INT64_MAX || Memory::KernelAvailableDirectMemorySize(0,
            static_cast<std::int64_t>(physical_size), direct_allocation_page,
            &window_begin, &available) != ::OK || window_begin < 0 ||
        available < 2 * direct_allocation_page) return false;
    const auto window_end = window_begin + 2 * direct_allocation_page;

    const auto code_base = Memory::AllocateProgramMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                         "fex-direct-allocation-code");
    if (code_base == 0) return false;
    std::array<std::uint8_t, 1024> code {};
    // Root guest -> outer HLE -> callback guest -> actual allocation HLE.
    constexpr std::uint8_t relay_prefix[] {0x48,0x83,0xec,0x08,0x48,0xb8};
    constexpr std::uint8_t relay_tail[] {0xff,0xd0,0x48,0x83,0xc4,0x08,0xc3};
    std::memcpy(code.data(), relay_prefix, sizeof(relay_prefix));
    Put64(code, sizeof(relay_prefix), reinterpret_cast<std::uint64_t>(NativeDirectAllocationOuter));
    std::memcpy(code.data() + sizeof(relay_prefix) + 8, relay_tail, sizeof(relay_tail));

    const auto allocation_callback = [&](std::size_t offset, bool null_output) {
        // Reserve an aligned guest frame with guard/output/guard/result words.
        // Only the output word's address is passed in the sixth GP argument R9.
        constexpr std::uint8_t prefix[] {0x48,0x83,0xec,0x28,0x48,0xb8};
        constexpr std::uint8_t first_store[] {0x48,0x89,0x04,0x24,0x48,0xb8};
        constexpr std::uint8_t output_store[] {0x48,0x89,0x44,0x24,0x08,0x48,0xb8};
        constexpr std::uint8_t guard_store[] {0x48,0x89,0x44,0x24,0x10};
        constexpr std::uint8_t own_output[] {0x4c,0x8d,0x4c,0x24,0x08};
        constexpr std::uint8_t no_output[] {0x45,0x31,0xc9};
        constexpr std::uint8_t load_target[] {0x48,0xb8};
        constexpr std::uint8_t tail[] {
            0xff,0xd0,0x48,0x89,0x44,0x24,0x18,
            0x48,0x8d,0x04,0x24,0x48,0x83,0xc4,0x28,0xc3
        };
        auto cursor = offset;
        const auto emit = [&](const auto& bytes) {
            std::memcpy(code.data() + cursor, bytes, sizeof(bytes));
            cursor += sizeof(bytes);
        };
        emit(prefix);
        Put64(code, cursor, direct_allocation_guard); cursor += 8;
        emit(first_store);
        Put64(code, cursor, direct_allocation_sentinel); cursor += 8;
        emit(output_store);
        Put64(code, cursor, direct_allocation_guard); cursor += 8;
        emit(guard_store);
        if (null_output) emit(no_output); else emit(own_output);
        emit(load_target);
        Put64(code, cursor, reinterpret_cast<std::uint64_t>(Memory::KernelAllocateDirectMemory));
        cursor += 8;
        emit(tail);
    };
    allocation_callback(64, false);
    allocation_callback(192, true);
    std::memcpy(code.data() + 320, relay_prefix, sizeof(relay_prefix));
    Put64(code, 320 + sizeof(relay_prefix),
          reinterpret_cast<std::uint64_t>(NativeDirectAllocationMutationProbe));
    std::memcpy(code.data() + 328 + sizeof(relay_prefix), relay_tail, sizeof(relay_tail));

    bool pass = Memory::WriteGuestMemory(nullptr, code_base, code.data(), code.size());
    Memory::SetProgramMemoryProtection(code_base, 0x4000, Vm::Mode::ExecuteRead);
    direct_allocation_observations = {};
    direct_allocation_mutation_denied = false;
    const std::array<std::uint64_t, 5> nested {code_base + 64, code_base + 192,
        code_base + 320, static_cast<std::uint64_t>(window_begin),
        static_cast<std::uint64_t>(window_end)};
    std::uint64_t value {};
    const bool invoked = Invoke(code_base, nested.data(), nested.size(), &value, stack_top);
    CheckCase("direct-allocation-real-wrapper-at-two-hle-levels", invoked && value == 1, pass);
    const auto result_is = [&](std::size_t index, int expected) {
        const auto& observation = direct_allocation_observations[index];
        return observation.captured && observation.frame[0] == direct_allocation_guard &&
            observation.frame[2] == direct_allocation_guard &&
            static_cast<std::uint32_t>(observation.frame[3]) == static_cast<std::uint32_t>(expected);
    };
    const std::array<const char*, 6> invalid_names {
        "direct-allocation-negative-search-has-no-copyout",
        "direct-allocation-empty-window-has-no-copyout",
        "direct-allocation-zero-length-has-no-copyout",
        "direct-allocation-unaligned-length-has-no-copyout",
        "direct-allocation-unaligned-alignment-has-no-copyout",
        "direct-allocation-null-output-has-no-copyout"
    };
    for (std::size_t index = 0; index < invalid_names.size(); ++index)
        CheckCase(invalid_names[index], result_is(index, Libs::LibKernel::KERNEL_ERROR_EINVAL) &&
            direct_allocation_observations[index].frame[1] == direct_allocation_sentinel, pass);

    const auto first = direct_allocation_observations[6].frame[1];
    const auto second = direct_allocation_observations[7].frame[1];
    const auto in_window = [&](std::uint64_t address) {
        return address >= static_cast<std::uint64_t>(window_begin) &&
            address < static_cast<std::uint64_t>(window_end) &&
            (address & (direct_allocation_page - 1)) == 0;
    };
    CheckCase("direct-allocation-invalid-calls-consume-no-reservation", result_is(6, ::OK) &&
        result_is(7, ::OK) && in_window(first) && in_window(second) && first != second, pass);
    CheckCase("direct-allocation-exhausted-window-keeps-output", result_is(8, Libs::LibKernel::KERNEL_ERROR_EAGAIN) &&
        direct_allocation_observations[8].frame[1] == direct_allocation_sentinel, pass);
    CheckCase("direct-allocation-keeps-nested-vm-mutation-denied",
              invoked && direct_allocation_mutation_denied, pass);

    // Queries and destructive release happen only after every guest/HLE Run
    // returns. The originally free two-page window contains only our reserves.
    std::int64_t available_address {};
    std::size_t available_size {};
    CheckCase("direct-allocation-reservations-are-visible-to-root-query",
        Memory::KernelAvailableDirectMemorySize(window_begin, window_end,
            direct_allocation_page, &available_address, &available_size) == Libs::LibKernel::KERNEL_ERROR_ENOMEM &&
        available_address == 0 && available_size == 0, pass);
    const bool first_released = Memory::KernelCheckedReleaseDirectMemory(window_begin,
        direct_allocation_page) == ::OK;
    const bool second_released = Memory::KernelCheckedReleaseDirectMemory(
        window_begin + direct_allocation_page, direct_allocation_page) == ::OK;
    CheckCase("direct-allocation-root-releases-both-reservations",
              first_released && second_released, pass);
    CheckCase("direct-allocation-release-restores-original-available-window",
        Memory::KernelAvailableDirectMemorySize(window_begin, window_end,
            direct_allocation_page, &available_address, &available_size) == ::OK &&
        available_address == window_begin && available_size == 2 * direct_allocation_page, pass);
    pass &= Memory::FreeGuestMemory(code_base, 0x4000);
    return pass;
}
