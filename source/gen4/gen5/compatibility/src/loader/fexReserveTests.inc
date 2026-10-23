// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
// Included by fexRuntimeTests.cpp after its common fixture helpers.
namespace {
constexpr std::uint64_t reserve_page = 0x4000;
constexpr std::uint64_t reserve_guard = 0x7265736572766764ULL;
std::atomic<bool> reserve_started {};
std::atomic<unsigned> reserve_prestart {};
std::atomic<unsigned> reserve_ready {};
std::atomic<std::int64_t> reserve_deadline {};
unsigned reserve_worker_count {};
std::array<std::uint64_t, 2> reserve_worker_results {};

std::uint64_t NativeReserveContender(std::uint64_t record) {
    std::uint64_t worker {};
    if (!Memory::ReadGuestMemory(nullptr, record, &worker, sizeof(worker)) || worker > 1)
        return UINT64_MAX;
    reserve_prestart.fetch_or(1U << worker, std::memory_order_acq_rel);
    if (!reserve_started.load(std::memory_order_acquire))
        return SteadyNanoseconds() < reserve_deadline.load(std::memory_order_acquire) ? 0 : UINT64_MAX;
    reserve_ready.fetch_add(1, std::memory_order_acq_rel);
    // Both real HLE readers precede either reservation. All retained state is
    // scalar; the fresh entry must donate the contender and revalidate output.
    while (reserve_ready.load(std::memory_order_acquire) != reserve_worker_count) {
        if (SteadyNanoseconds() >= reserve_deadline.load(std::memory_order_acquire)) return UINT64_MAX;
        std::this_thread::yield();
    }
    const auto result = Memory::KernelReserveVirtualRange(
        reinterpret_cast<void**>(record + 8), reserve_page, 0, reserve_page);
    return static_cast<std::uint32_t>(result) + UINT64_C(1);
}

bool RunReserveContenders(std::uint64_t entry, std::uint64_t records, unsigned count,
                         bool revoke_output_before_readmission) {
    namespace P = Libs::LibKernel;
    reserve_started.store(false, std::memory_order_release);
    reserve_prestart.store(0, std::memory_order_release);
    reserve_ready.store(0, std::memory_order_release);
    reserve_deadline.store(SteadyNanoseconds() + 2'000'000'000LL, std::memory_order_release);
    reserve_worker_count = count;
    reserve_worker_results.fill(UINT64_MAX);
    std::array<P::Pthread, 2> children {};
    unsigned created {};
    for (; created < count; ++created) {
        if (P::PthreadCreate(&children[created], nullptr,
                reinterpret_cast<P::pthread_entry_func_t>(entry),
                reinterpret_cast<void*>(records + created * 128), "fex-reserve-contender") != ::OK) break;
    }
    const unsigned mask = (1U << count) - 1;
    while (created == count && reserve_prestart.load(std::memory_order_acquire) != mask &&
           SteadyNanoseconds() < reserve_deadline.load(std::memory_order_acquire)) std::this_thread::yield();
    bool pass = created == count && reserve_prestart.load(std::memory_order_acquire) == mask;
    if (revoke_output_before_readmission && pass) {
        // Runs are already initialized. Holding the ordinary writer gate now
        // makes the real Reserve entry donate while it waits for writer intent.
        // Revoke under the normal exclusive epoch before releasing that gate;
        // this cannot rely on timing or a stale preflight pointer snapshot.
        std::lock_guard writer_intent(Loader::GuestExecution::guest_writer_gate);
        reserve_started.store(true, std::memory_order_release);
        while (reserve_ready.load(std::memory_order_acquire) != 1 &&
               SteadyNanoseconds() < reserve_deadline.load(std::memory_order_acquire))
            std::this_thread::yield();
        pass &= reserve_ready.load(std::memory_order_acquire) == 1;
        if (pass) pass &= Memory::ProtectGuestMemory(records, reserve_page, Vm::Mode::Read);
    } else {
        reserve_started.store(true, std::memory_order_release);
    }
    for (unsigned index = 0; index < created; ++index) {
        void* result {};
        pass &= P::PthreadJoinHost(children[index], &result) == ::OK;
        reserve_worker_results[index] = reinterpret_cast<std::uint64_t>(result);
    }
    return pass;
}
} // namespace

bool TestReserveVirtualRange(std::uint64_t stack_top) {
    namespace P = Libs::LibKernel;
    if (!RegisterHostFunction<Memory::KernelReserveVirtualRange>("self-test-reserve") ||
        !RegisterHostFunction<Memory::KernelMemoryPoolReserve>("self-test-reserve-pool-native-local") ||
        !RegisterHostFunction<NativeReserveContender>("self-test-reserve-contender")) return false;
    const auto code_base = Memory::AllocateProgramMemory(0, reserve_page, Vm::Mode::ReadWrite,
                                                         "fex-reserve-code");
    const auto records = Memory::AllocateRuntimeMemory(0, 2 * reserve_page, Vm::Mode::ReadWrite,
                                                       "fex-reserve-records");
    const auto victim = Memory::AllocateRuntimeMemory(0, reserve_page, Vm::Mode::ReadWrite,
                                                      "fex-reserve-victim");
    if (code_base == 0 || records == 0 || victim == 0) return false;
    std::array<std::uint8_t, 1024> code {};
    const auto relay = [&](std::size_t offset, std::uint64_t target, bool poll) {
        constexpr std::uint8_t prefix[] {0x48,0x83,0xec,0x08,0x48,0xb8};
        constexpr std::uint8_t tail[] {0xff,0xd0,0x48,0x83,0xc4,0x08,0xc3};
        // Save RDI across HLE polling so a second call retains its record.
        constexpr std::uint8_t polling_prefix[] {0x53,0x48,0x89,0xfb,0x48,0x89,0xdf,0x48,0xb8};
        constexpr std::uint8_t polling_tail[] {
            0xff,0xd0,0x48,0x85,0xc0,0x74,0xec,0x5b,0xc3
        };
        if (poll) {
            std::memcpy(code.data() + offset, polling_prefix, sizeof(polling_prefix));
            Put64(code, offset + sizeof(polling_prefix), target);
            std::memcpy(code.data() + offset + sizeof(polling_prefix) + 8,
                        polling_tail, sizeof(polling_tail));
        } else {
            std::memcpy(code.data() + offset, prefix, sizeof(prefix));
            Put64(code, offset + sizeof(prefix), target);
            std::memcpy(code.data() + offset + sizeof(prefix) + 8, tail, sizeof(tail));
        }
    };
    relay(0, reinterpret_cast<std::uint64_t>(Memory::KernelReserveVirtualRange), false);
    relay(64, reinterpret_cast<std::uint64_t>(NativeReserveContender), true);
    relay(128, reinterpret_cast<std::uint64_t>(Memory::KernelMemoryPoolReserve), false);
    bool pass = Memory::WriteGuestMemory(nullptr, code_base, code.data(), code.size());
    Memory::SetProgramMemoryProtection(code_base, reserve_page, Vm::Mode::ExecuteRead);
    const auto slot = records + 8;
    const auto put = [&](std::uint64_t hint) {
        const std::array<std::uint64_t, 3> frame {reserve_guard, hint, reserve_guard};
        return Memory::WriteGuestMemory(nullptr, records, frame.data(), sizeof(frame));
    };
    const auto preserved = [&](std::uint64_t hint) {
        std::array<std::uint64_t, 3> frame {};
        return Memory::ReadGuestMemory(nullptr, records, frame.data(), sizeof(frame)) &&
            frame == std::array<std::uint64_t, 3> {reserve_guard, hint, reserve_guard};
    };
    const auto call = [&](std::uint64_t output, std::uint64_t len, std::uint64_t flags,
                          std::uint64_t alignment, int expected) {
        const std::array<std::uint64_t, 4> arguments {output, len, flags, alignment};
        std::uint64_t result {};
        return Invoke(code_base, arguments.data(), arguments.size(), &result, stack_top) &&
            static_cast<std::uint32_t>(result) == static_cast<std::uint32_t>(expected);
    };
    const auto read_output = [&](std::uint64_t output) {
        std::uint64_t value {};
        return Memory::ReadGuestMemory(nullptr, output, &value, sizeof(value)) ? value : UINT64_MAX;
    };
    const auto victim_unchanged = [&]() {
        std::uint64_t value {};
        return Memory::QueryGuestAccess(victim, reserve_page, Vm::Mode::ReadWrite) &&
            Memory::ReadGuestMemory(nullptr, victim, &value, sizeof(value)) && value == reserve_guard;
    };
    pass &= Memory::WriteGuestMemory(nullptr, victim, &reserve_guard, sizeof(reserve_guard));
    CheckCase("reserve-legacy-null-and-scalar-errors-preserve-output", put(victim) &&
        call(0, reserve_page, 0, reserve_page, P::KERNEL_ERROR_EINVAL) &&
        call(slot, 0, 0, reserve_page, P::KERNEL_ERROR_EINVAL) &&
        call(slot, reserve_page + 1, 0, reserve_page, P::KERNEL_ERROR_EINVAL) &&
        call(slot, reserve_page, 0, reserve_page + 1, P::KERNEL_ERROR_EINVAL) &&
        preserved(victim) && victim_unchanged(), pass);
    void* native_hint = reinterpret_cast<void*>(victim);
    CheckCase("reserve-invalid-and-native-outside-guest-slot-have-no-effects",
        call(UINT64_MAX, reserve_page, 0x10, reserve_page, P::KERNEL_ERROR_EFAULT) &&
        call(reinterpret_cast<std::uint64_t>(&native_hint), reserve_page, 0x10,
             reserve_page, P::KERNEL_ERROR_EFAULT) && native_hint == reinterpret_cast<void*>(victim) &&
        victim_unchanged(), pass);
    pass &= put(victim) && Memory::ProtectGuestMemory(records, reserve_page, Vm::Mode::Read);
    CheckCase("reserve-readonly-slot-rejects-before-fixed-replacement",
        call(slot, reserve_page, 0x10, reserve_page, P::KERNEL_ERROR_EFAULT) &&
        preserved(victim) && victim_unchanged(), pass);
    pass &= Memory::ProtectGuestMemory(records, reserve_page, Vm::Mode::ReadWrite);
    const auto crossing = records + reserve_page - 4;
    pass &= Memory::WriteGuestMemory(nullptr, crossing, &victim, sizeof(victim)) &&
        Memory::ProtectGuestMemory(records + reserve_page, reserve_page, Vm::Mode::NoAccess);
    CheckCase("reserve-slot-crossing-rights-boundary-has-no-effects",
        call(crossing, reserve_page, 0x10, reserve_page, P::KERNEL_ERROR_EFAULT) &&
        victim_unchanged(), pass);
    pass &= Memory::ProtectGuestMemory(records + reserve_page, reserve_page, Vm::Mode::ReadWrite);
    CheckCase("reserve-crossing-slot-remains-untouched", read_output(crossing) == victim, pass);
    pass &= Memory::WriteGuestMemory(nullptr, victim + 8, &victim, sizeof(victim));
    CheckCase("reserve-fixed-self-replacement-rejected-before-revoking-output",
        call(victim + 8, reserve_page, 0x10, reserve_page, P::KERNEL_ERROR_EINVAL) &&
        read_output(victim + 8) == victim && victim_unchanged(), pass);
    CheckCase("reserve-fixed-no-overwrite-keeps-existing-owner", put(victim) &&
        call(slot, reserve_page, 0x90, reserve_page, P::KERNEL_ERROR_ENOMEM) &&
        preserved(victim) && victim_unchanged(), pass);

    CheckCase("reserve-real-wrapper-publishes-noaccess-reservation", put(0) &&
        call(slot, reserve_page, 0, reserve_page, ::OK), pass);
    const auto reserved = read_output(slot);
    Memory::VirtualQueryInfo info {};
    CheckCase("reserve-output-canaries-and-owner-metadata",
        reserved != 0 && reserved != UINT64_MAX && preserved(reserved) &&
        Memory::KernelVirtualQuery(reinterpret_cast<void*>(reserved), 0, &info, sizeof(info)) == ::OK &&
        info.protection == 0 && !info.is_committed && !info.is_direct &&
        !Memory::QueryGuestAccess(reserved, 8, Vm::Mode::Read), pass);
    if (reserved != 0 && reserved != UINT64_MAX) pass &= Memory::KernelMunmap(reserved, reserve_page) == ::OK;

    std::array<std::uint64_t, 32> worker_records {};
    worker_records[16] = 1;
    pass &= Memory::WriteGuestMemory(nullptr, records, worker_records.data(), sizeof(worker_records));
    const auto begun = std::chrono::steady_clock::now();
    const bool competed = RunReserveContenders(code_base + 64, records, 2, false);
    const auto first = read_output(records + 8);
    const auto second = read_output(records + 136);
    CheckCase("reserve-two-real-fex-hle-contenders-complete-with-distinct-owners",
        competed && reserve_worker_results[0] == 1 && reserve_worker_results[1] == 1 &&
        first != 0 && first != UINT64_MAX && second != 0 && second != UINT64_MAX && first != second &&
        std::chrono::steady_clock::now() - begun < std::chrono::seconds(2), pass);
    if (first != 0 && first != UINT64_MAX) pass &= Memory::KernelMunmap(first, reserve_page) == ::OK;
    if (second != 0 && second != UINT64_MAX) pass &= Memory::KernelMunmap(second, reserve_page) == ::OK;

    pass &= Memory::WriteGuestMemory(nullptr, records, worker_records.data(), sizeof(worker_records));
    CheckCase("reserve-readmission-rechecks-revoked-output-before-any-reservation",
        RunReserveContenders(code_base + 64, records, 1, true) &&
        reserve_worker_results[0] == static_cast<std::uint32_t>(P::KERNEL_ERROR_EFAULT) + UINT64_C(1) &&
        read_output(records + 8) == 0 && !Memory::QueryGuestAccess(records, 8, Vm::Mode::Write), pass);
    pass &= Memory::ProtectGuestMemory(records, reserve_page, Vm::Mode::ReadWrite);

    // The public pool entry retains an exclusive owner while its private
    // Reserve helper uses a real native local, despite an active guest state.
    const std::array<std::uint64_t, 5> pool_arguments {0, 0x200000, 0x200000, 0, slot};
    std::uint64_t pool_result {};
    CheckCase("reserve-pool-internal-native-local-under-owned-epoch", put(0) &&
        Invoke(code_base + 128, pool_arguments.data(), pool_arguments.size(), &pool_result, stack_top) &&
        static_cast<std::uint32_t>(pool_result) == ::OK, pass);
    const auto pool = read_output(slot);
    if (pool != 0 && pool != UINT64_MAX) pass &= Memory::KernelMunmap(pool, 0x200000) == ::OK;
    pass &= Memory::FreeGuestMemory(victim, reserve_page);
    pass &= Memory::FreeGuestMemory(records, 2 * reserve_page);
    pass &= Memory::FreeGuestMemory(code_base, reserve_page);
    return pass;
}
