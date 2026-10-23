// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
// Included by fexRuntimeTests.cpp after its common fixture helpers.
namespace {
std::atomic<bool> rw_wait_started {};
std::atomic<bool> rw_wait_entered {};
std::atomic<bool> rw_wait_finished {};
std::atomic<bool> rw_wait_acquired {};
std::atomic<bool> rw_wait_nonblocking_correct {};
std::atomic<std::int64_t> rw_wait_deadline {};
Libs::LibKernel::PthreadRwlock* rw_wait_guest_slot {};

std::uint64_t NativeRwlockWait(std::uint64_t write) {
    namespace P = Libs::LibKernel;
    if (!rw_wait_started.load(std::memory_order_acquire)) {
        return SteadyNanoseconds() < rw_wait_deadline.load(std::memory_order_acquire)
            ? 0 : UINT64_MAX;
    }
    const auto fail = [] {
        rw_wait_finished.store(true, std::memory_order_release);
        return UINT64_MAX;
    };
    if (write > 1) return fail();
    const auto try_result = write ? P::PthreadRwlockTrywrlock(rw_wait_guest_slot)
                                  : P::PthreadRwlockTryrdlock(rw_wait_guest_slot);
    const auto zero_result = write ? P::PthreadRwlockTimedwrlock(rw_wait_guest_slot, 0)
                                   : P::PthreadRwlockTimedrdlock(rw_wait_guest_slot, 0);
    const auto timed_result = write ? P::PthreadRwlockTimedwrlock(rw_wait_guest_slot, 1000)
                                    : P::PthreadRwlockTimedrdlock(rw_wait_guest_slot, 1000);
    const bool nonblocking = try_result == P::KERNEL_ERROR_EBUSY &&
        zero_result == P::KERNEL_ERROR_ETIMEDOUT && timed_result == P::KERNEL_ERROR_ETIMEDOUT;
    rw_wait_nonblocking_correct.store(nonblocking, std::memory_order_release);
    if (!nonblocking) return fail();

    // This notification and the real contended API run inside the SAME
    // counted FEX HLE callback. The main thread cannot acquire an exclusive
    // epoch before this reader is donated by the native wait itself.
    rw_wait_entered.store(true, std::memory_order_release);
    const auto result = write ? P::PthreadRwlockWrlock(rw_wait_guest_slot)
                              : P::PthreadRwlockRdlock(rw_wait_guest_slot);
    if (result != ::OK) return fail();
    rw_wait_acquired.store(true, std::memory_order_release);
    const bool unlocked = P::PthreadRwlockUnlock(rw_wait_guest_slot) == ::OK;
    rw_wait_finished.store(true, std::memory_order_release);
    return unlocked ? 1 : UINT64_MAX;
}
} // namespace

bool TestRwlockHostWait() {
    namespace P = Libs::LibKernel;
    constexpr std::uint64_t page = 0x4000;
    constexpr std::uint64_t guard = 0x72776c6f636b7761ULL;
    constexpr std::uint64_t marker = 0x1029384756abcdefULL;
    if (!RegisterHostFunction<NativeRwlockWait>("self-test-rwlock-native-wait")) return false;
    const auto code_base = Memory::AllocateProgramMemory(0, page, Vm::Mode::ReadWrite,
                                                        "fex-rwlock-wait-code");
    const auto records = Memory::AllocateRuntimeMemory(0, page * 2, Vm::Mode::ReadWrite,
                                                       "fex-rwlock-wait-records");
    if (code_base == 0 || records == 0) {
        if (records != 0) Memory::FreeGuestMemory(records, page * 2);
        if (code_base != 0) Memory::FreeGuestMemory(code_base, page);
        return false;
    }
    // A real guest pthread polls setup through HLE, retaining its argument in
    // RBX and keeping the SysV call stack aligned. No native sleep or callback
    // is used to fabricate the barrier ordering.
    std::array<std::uint8_t, 26> code {
        0x53,0x48,0x89,0xfb,0x48,0x89,0xdf,0x48,0xb8,
        0,0,0,0,0,0,0,0,0xff,0xd0,0x48,0x85,0xc0,0x74,0xec,0x5b,0xc3};
    const auto target = reinterpret_cast<std::uint64_t>(NativeRwlockWait);
    std::memcpy(code.data() + 9, &target, sizeof(target));
    bool pass = Memory::WriteGuestMemory(nullptr, code_base, code.data(), code.size());
    Memory::SetProgramMemoryProtection(code_base, page, Vm::Mode::ExecuteRead);
    const auto slot = records + 8;
    const auto scratch = records + page;
    rw_wait_guest_slot = reinterpret_cast<P::PthreadRwlock*>(slot);

    for (const bool write: {true, false}) {
        P::PthreadRwlock native_rwlock {};
        bool round = P::PthreadRwlockInit(&native_rwlock, nullptr, "fex-rwlock-wait") == ::OK;
        const std::array<std::uint64_t, 3> slot_frame {
            guard, reinterpret_cast<std::uint64_t>(native_rwlock), guard};
        const std::array<std::uint64_t, 3> scratch_frame {guard, marker, guard};
        round &= Memory::WriteGuestMemory(nullptr, slot - 8, slot_frame.data(), sizeof(slot_frame));
        round &= Memory::WriteGuestMemory(nullptr, scratch, scratch_frame.data(), sizeof(scratch_frame));
        bool parent_locked = round && (write ? P::PthreadRwlockRdlock(&native_rwlock)
                                              : P::PthreadRwlockWrlock(&native_rwlock)) == ::OK;
        round &= parent_locked;
        rw_wait_started.store(false, std::memory_order_release);
        rw_wait_entered.store(false, std::memory_order_release);
        rw_wait_finished.store(false, std::memory_order_release);
        rw_wait_acquired.store(false, std::memory_order_release);
        rw_wait_nonblocking_correct.store(false, std::memory_order_release);
        rw_wait_deadline.store(SteadyNanoseconds() + 10'000'000'000LL, std::memory_order_release);
        P::Pthread child {};
        const bool created = round && P::PthreadCreate(&child, nullptr,
            reinterpret_cast<P::pthread_entry_func_t>(code_base),
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(write)),
            "fex-rwlock-wait-peer") == ::OK;
        round &= created;
        rw_wait_started.store(true, std::memory_order_release);
        while (created && !rw_wait_entered.load(std::memory_order_acquire) &&
               !rw_wait_finished.load(std::memory_order_acquire) &&
               SteadyNanoseconds() < rw_wait_deadline.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const bool entered = created && rw_wait_entered.load(std::memory_order_acquire);
        // A denial is a bounded test failure, not an assert: the parent still
        // unlocks and joins the real guest waiter on the old implementation.
        const auto token = entered ? LumenAndroidFexCoreBeginGuestMemoryMutationV1() : 0;
        bool epoch = token != 0;
        if (epoch) {
            epoch &= Memory::ProtectGuestMemory(scratch, page, Vm::Mode::Read);
            epoch &= Memory::QueryGuestAccess(scratch, sizeof(scratch_frame), Vm::Mode::Read) &&
                     !Memory::QueryGuestAccess(scratch, sizeof(scratch_frame), Vm::Mode::Write);
        }
        if (parent_locked) {
            round &= P::PthreadRwlockUnlock(&native_rwlock) == ::OK;
            parent_locked = false;
        }
        bool pinned = false;
        if (token != 0) {
            // With the old logical owner gone, a real try-lock must succeed.
            // The child cannot acquire until barrier readmission; Destroy is
            // still EBUSY because the native operation pin outlives that wait.
            const bool no_owner = P::PthreadRwlockTrywrlock(&native_rwlock) == ::OK;
            if (no_owner) round &= P::PthreadRwlockUnlock(&native_rwlock) == ::OK;
            pinned = no_owner && !rw_wait_acquired.load(std::memory_order_acquire) &&
                P::PthreadRwlockDestroy(&native_rwlock) == P::KERNEL_ERROR_EBUSY;
            LumenAndroidFexCoreEndGuestMemoryMutationV1(token, 0, 0, 0);
        }
        void* result {};
        const bool joined = created && P::PthreadJoinHost(child, &result) == ::OK;
        const bool child_ok = joined && reinterpret_cast<std::uint64_t>(result) == 1 &&
            rw_wait_acquired.load(std::memory_order_acquire) &&
            rw_wait_nonblocking_correct.load(std::memory_order_acquire);
        CheckCase(write ? "rwlock-writer-wait-allows-exclusive-vm-and-readmission"
                        : "rwlock-reader-wait-allows-exclusive-vm-and-readmission",
            round && entered && epoch && pinned && child_ok, pass);
        round = Memory::ProtectGuestMemory(scratch, page, Vm::Mode::ReadWrite);
        std::array<std::uint64_t, 3> actual_slot {}, actual_scratch {};
        round &= Memory::ReadGuestMemory(nullptr, slot - 8, actual_slot.data(), sizeof(actual_slot)) &&
                 actual_slot == slot_frame;
        round &= Memory::ReadGuestMemory(nullptr, scratch, actual_scratch.data(), sizeof(actual_scratch)) &&
                 actual_scratch == scratch_frame;
        if (native_rwlock != nullptr) round &= P::PthreadRwlockDestroy(&native_rwlock) == ::OK;
        CheckCase(write ? "rwlock-writer-wait-preserves-guards-and-releases-pin"
                        : "rwlock-reader-wait-preserves-guards-and-releases-pin", round, pass);
    }
    rw_wait_guest_slot = nullptr;
    pass &= Memory::FreeGuestMemory(records, page * 2);
    pass &= Memory::FreeGuestMemory(code_base, page);
    return pass;
}
