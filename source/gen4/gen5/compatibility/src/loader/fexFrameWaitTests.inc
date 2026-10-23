// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
// Included by fexRuntimeTests.cpp after its common fixture helpers.
namespace {
std::atomic<bool> frame_wait_started {}, frame_wait_entered {}, frame_wait_finished {};
std::atomic<std::int64_t> frame_wait_deadline {};
std::atomic<bool> frame_control_started {}, frame_control_entered {}, frame_control_finished {};
std::atomic<bool> frame_writer_started {};
Libs::LibKernel::EventFlag::KernelEventFlag frame_wait_flag {};
std::uint64_t frame_wait_output {}, frame_wait_timeout {}, frame_control_output {};

std::uint64_t NativeFrameEventWait(std::uint64_t mode) {
    if (!frame_wait_started.load(std::memory_order_acquire)) {
        return SteadyNanoseconds() < frame_wait_deadline.load(std::memory_order_acquire)
            ? 0 : UINT64_MAX;
    }
    // The notification and actual wait are inside the SAME counted HLE.
    // An exclusive writer can enter only once the API donates this reader.
    frame_wait_entered.store(true, std::memory_order_release);
    const auto result = mode < 2 || mode >= 4
        ? Libs::LibKernel::EventFlag::KernelWaitEventFlag(frame_wait_flag, 1, 0x21,
            reinterpret_cast<std::uint64_t*>(frame_wait_output),
            reinterpret_cast<Libs::LibKernel::KernelUseconds*>(frame_wait_timeout))
        : Libs::VideoOut::VideoOutVblankWaitSelfTest(1);
    frame_wait_finished.store(true, std::memory_order_release);
    return static_cast<std::uint32_t>(result) == static_cast<std::uint32_t>(
        mode == 1 ? Libs::LibKernel::KERNEL_ERROR_EFAULT
                  : mode == 4 ? Libs::LibKernel::KERNEL_ERROR_ECANCELED
                              : mode == 5 ? Libs::LibKernel::KERNEL_ERROR_EACCES : ::OK) ? 1 : UINT64_MAX;
}

std::uint64_t NativeFrameEventControl(std::uint64_t mode) {
    if (!frame_control_started.load(std::memory_order_acquire)) {
        return SteadyNanoseconds() < frame_wait_deadline.load(std::memory_order_acquire)
            ? 0 : UINT64_MAX;
    }
    frame_control_entered.store(true, std::memory_order_release);
    while (!frame_writer_started.load(std::memory_order_acquire)) std::this_thread::yield();
    // Hold this genuine HLE reader long enough for the competing real writer
    // to publish intent. Cancel/Delete then wake A while its readmission is
    // blocked by C; their drain must donate B's reader so C can complete.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto result = mode == 4
        ? Libs::LibKernel::EventFlag::KernelCancelEventFlag(frame_wait_flag, 8,
            reinterpret_cast<int*>(frame_control_output))
        : Libs::LibKernel::EventFlag::KernelDeleteEventFlag(frame_wait_flag);
    frame_control_finished.store(true, std::memory_order_release);
    return result == ::OK ? 1 : UINT64_MAX;
}
} // namespace

bool TestFrameEventHostWait(std::uint64_t stack_top) {
    namespace P = Libs::LibKernel;
    namespace E = P::EventFlag;
    constexpr std::uint64_t page = 0x4000;
    constexpr std::uint64_t guard = 0x6672616d65776169ULL;
    constexpr std::uint64_t marker = 0x123456789abcdef0ULL;
    if (!RegisterHostFunction<NativeFrameEventWait>("self-test-frame-event-native-wait") ||
        !RegisterHostFunction<NativeFrameEventControl>("self-test-frame-event-native-control") ||
        !RegisterHostFunction<E::KernelWaitEventFlag>("self-test-event-flag-wait")) return false;
    const auto code_base = Memory::AllocateProgramMemory(0, page, Vm::Mode::ReadWrite,
                                                        "fex-frame-wait-code");
    const auto records = Memory::AllocateRuntimeMemory(0, page * 2, Vm::Mode::ReadWrite,
                                                       "fex-frame-wait-records");
    if (code_base == 0 || records == 0) {
        if (records != 0) Memory::FreeGuestMemory(records, page * 2);
        if (code_base != 0) Memory::FreeGuestMemory(code_base, page);
        return false;
    }
    std::array<std::uint8_t, 1024> code {};
    constexpr std::uint8_t loop[] {
        0x53,0x48,0x89,0xfb,0x48,0x89,0xdf,0x48,0xb8,
        0,0,0,0,0,0,0,0,0xff,0xd0,0x48,0x85,0xc0,0x74,0xec,0x5b,0xc3};
    std::memcpy(code.data(), loop, sizeof(loop));
    Put64(code, 9, reinterpret_cast<std::uint64_t>(NativeFrameEventWait));
    std::memcpy(code.data() + 64, loop, sizeof(loop));
    Put64(code, 73, reinterpret_cast<std::uint64_t>(NativeFrameEventControl));
    constexpr std::uint8_t prefix[] {0x48,0x83,0xec,0x08,0x48,0xb8};
    constexpr std::uint8_t tail[] {0xff,0xd0,0x48,0x83,0xc4,0x08,0xc3};
    std::memcpy(code.data() + 32, prefix, sizeof(prefix));
    Put64(code, 32 + sizeof(prefix), reinterpret_cast<std::uint64_t>(E::KernelWaitEventFlag));
    std::memcpy(code.data() + 32 + sizeof(prefix) + 8, tail, sizeof(tail));
    bool pass = Memory::WriteGuestMemory(nullptr, code_base, code.data(), code.size());
    Memory::SetProgramMemoryProtection(code_base, page, Vm::Mode::ExecuteRead);
    frame_wait_output = records + 8;
    frame_wait_timeout = records + 32;
    frame_control_output = records + 64;
    const std::array<std::uint64_t, 3> expected {guard, marker, guard};
    const auto signal = [](unsigned mode) {
        if (mode < 2) return E::KernelSetEventFlag(frame_wait_flag, 1) == ::OK;
        return Libs::VideoOut::VideoOutVblankWaitSelfTest(mode == 3 ? 3 : 2) == ::OK;
    };

    for (unsigned mode = 0; mode != 4; ++mode) {
        bool round = Memory::ProtectGuestMemory(records, page * 2, Vm::Mode::ReadWrite);
        // Revoke ONLY timeout in the negative case: the result remains RW,
        // proving both destinations are validated before the first copyout.
        frame_wait_timeout = records + (mode == 1 ? page : 0) + 32;
        round &= Memory::WriteGuestMemory(nullptr, records, expected.data(), sizeof(expected));
        const P::KernelUseconds timeout = 4'000'000;
        round &= Memory::WriteGuestMemory(nullptr, frame_wait_timeout, &timeout, sizeof(timeout));
        round &= mode < 2
            ? E::KernelCreateEventFlag(&frame_wait_flag, "fex-frame-event-wait", 0x10, 0, nullptr) == ::OK
            : Libs::VideoOut::VideoOutVblankWaitSelfTest(0) == ::OK;
        frame_wait_started.store(false, std::memory_order_release);
        frame_wait_entered.store(false, std::memory_order_release);
        frame_wait_finished.store(false, std::memory_order_release);
        frame_wait_deadline.store(SteadyNanoseconds() + 10'000'000'000LL, std::memory_order_release);
        P::Pthread child {};
        const bool created = round && P::PthreadCreate(&child, nullptr,
            reinterpret_cast<P::pthread_entry_func_t>(code_base),
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(mode)),
            "fex-frame-wait-peer") == ::OK;
        round &= created;
        std::atomic<bool> signaled {};
        // Bound old implementations that keep the reader forever. The ordering
        // witness still fails if this fallback is needed, even if Begin later
        // obtains an epoch after the old HLE has finished.
        std::thread fallback([&] {
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!signaled.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < end)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!signaled.exchange(true, std::memory_order_acq_rel) && !signal(mode)) std::abort();
        });
        frame_wait_started.store(true, std::memory_order_release);
        while (created && !frame_wait_entered.load(std::memory_order_acquire) &&
               !frame_wait_finished.load(std::memory_order_acquire) &&
               SteadyNanoseconds() < frame_wait_deadline.load(std::memory_order_acquire))
            std::this_thread::yield();
        const bool entered = created && frame_wait_entered.load(std::memory_order_acquire);
        const auto token = entered ? LumenAndroidFexCoreBeginGuestMemoryMutationV1() : 0;
        bool epoch_before_wake = token != 0 && !signaled.load(std::memory_order_acquire) &&
            !frame_wait_finished.load(std::memory_order_acquire);
        if (epoch_before_wake) {
            round &= Memory::ProtectGuestMemory(records + page, page, Vm::Mode::Read);
            if (!signaled.exchange(true, std::memory_order_acq_rel)) round &= signal(mode);
            // The wake cannot complete guest output while the writer owns VM.
            round &= !frame_wait_finished.load(std::memory_order_acquire);
        }
        if (token != 0) LumenAndroidFexCoreEndGuestMemoryMutationV1(token, 0, 0, 0);
        if (!signaled.exchange(true, std::memory_order_acq_rel)) round &= signal(mode);
        fallback.join();
        void* result {};
        round &= created && P::PthreadJoinHost(child, &result) == ::OK &&
            reinterpret_cast<std::uint64_t>(result) == 1;
        CheckCase(mode == 0 ? "event-flag-normal-wait-donates-reader-before-writer-wake"
                  : mode == 1 ? "event-flag-revoked-timeout-wait-donates-reader-before-writer-wake"
                  : mode == 2 ? "vblank-normal-wait-donates-reader-before-writer-wake"
                              : "vblank-reopened-wait-donates-reader-before-writer-wake",
            round && entered && epoch_before_wake, pass);
        std::array<std::uint64_t, 3> actual {};
        round &= Memory::ReadGuestMemory(nullptr, records, actual.data(), sizeof(actual));
        const auto expected_output = mode == 0 ? std::array<std::uint64_t, 3> {guard, 1, guard}
                                              : expected;
        round &= actual == expected_output;
        if (mode == 1) {
            P::KernelUseconds actual_timeout {};
            round &= Memory::ReadGuestMemory(nullptr, frame_wait_timeout,
                                             &actual_timeout, sizeof(actual_timeout)) &&
                actual_timeout == timeout;
        }
        if (mode < 2) {
            std::uint64_t remaining = UINT64_MAX;
            round &= E::KernelPollEventFlag(frame_wait_flag, 1, 1, &remaining) == P::KERNEL_ERROR_EBUSY &&
                remaining == 0; // Clear-bits behavior survives the wait/copyout boundary.
            round &= E::KernelDeleteEventFlag(frame_wait_flag) == ::OK;
            frame_wait_flag = nullptr;
        }
        CheckCase(mode == 1 ? "event-flag-revoked-copyout-preserves-both-outputs"
                  : mode == 3 ? "vblank-close-reopen-does-not-retarget-old-waiter"
                  : mode == 0 ? "event-flag-normal-wait-preserves-output-guards"
                              : "vblank-normal-wait-preserves-output-guards", round, pass);
    }

    // A genuine x86 HLE timeout still reports bits and updates remaining time.
    frame_wait_timeout = records + 32;
    pass &= Memory::ProtectGuestMemory(records, page * 2, Vm::Mode::ReadWrite);
    pass &= E::KernelCreateEventFlag(&frame_wait_flag, "fex-frame-event-timeout", 0x10, 2, nullptr) == ::OK;
    P::KernelUseconds timeout = 1000;
    pass &= Memory::WriteGuestMemory(nullptr, frame_wait_timeout, &timeout, sizeof(timeout));
    const std::array<std::uint64_t, 5> args {
        reinterpret_cast<std::uint64_t>(frame_wait_flag), 1, 1, frame_wait_output, frame_wait_timeout};
    std::uint64_t result {}, bits {};
    bool timed = Invoke(code_base + 32, args.data(), args.size(), &result, stack_top) &&
        static_cast<std::uint32_t>(result) == static_cast<std::uint32_t>(P::KERNEL_ERROR_ETIMEDOUT);
    timed &= Memory::ReadGuestMemory(nullptr, frame_wait_output, &bits, sizeof(bits)) && bits == 2;
    timed &= Memory::ReadGuestMemory(nullptr, frame_wait_timeout, &timeout, sizeof(timeout)) && timeout == 0;
    CheckCase("event-flag-timeout-staged-copyout", timed, pass);
    pass &= E::KernelDeleteEventFlag(frame_wait_flag) == ::OK;
    frame_wait_flag = nullptr;

    for (unsigned mode: {4U, 5U}) {
        bool round = Memory::ProtectGuestMemory(records, page * 2, Vm::Mode::ReadWrite);
        round &= E::KernelCreateEventFlag(&frame_wait_flag,
            "fex-frame-event-control", 0x10, 0, nullptr) == ::OK;
        const auto old_handle = frame_wait_flag;
        timeout = 4'000'000;
        int waiting = -1;
        round &= Memory::WriteGuestMemory(nullptr, frame_wait_timeout, &timeout, sizeof(timeout));
        round &= Memory::WriteGuestMemory(nullptr, frame_control_output, &waiting, sizeof(waiting));
        frame_wait_entered.store(false, std::memory_order_release);
        frame_wait_finished.store(false, std::memory_order_release);
        frame_wait_started.store(true, std::memory_order_release);
        frame_control_started.store(false, std::memory_order_release);
        frame_control_entered.store(false, std::memory_order_release);
        frame_control_finished.store(false, std::memory_order_release);
        frame_writer_started.store(false, std::memory_order_release);
        frame_wait_deadline.store(SteadyNanoseconds() + 10'000'000'000LL, std::memory_order_release);
        P::Pthread waiter {}, controller {};
        const bool waiter_created = round && P::PthreadCreate(&waiter, nullptr,
            reinterpret_cast<P::pthread_entry_func_t>(code_base),
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(mode)),
            "fex-frame-wait-control-peer") == ::OK;
        round &= waiter_created;
        while (waiter_created && !frame_wait_entered.load(std::memory_order_acquire) &&
               SteadyNanoseconds() < frame_wait_deadline.load(std::memory_order_acquire))
            std::this_thread::yield();
        // This real epoch verifies A is registered/quiescent before B starts.
        const auto setup_token = waiter_created ? LumenAndroidFexCoreBeginGuestMemoryMutationV1() : 0;
        round &= setup_token != 0 && !frame_wait_finished.load(std::memory_order_acquire);
        if (setup_token != 0) LumenAndroidFexCoreEndGuestMemoryMutationV1(setup_token, 0, 0, 0);
        const bool control_created = round && P::PthreadCreate(&controller, nullptr,
            reinterpret_cast<P::pthread_entry_func_t>(code_base + 64),
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(mode)),
            "fex-frame-cancel-delete-peer") == ::OK;
        round &= control_created;
        bool writer_before_drain = false;
        std::thread writer([&] {
            while (control_created && !frame_control_entered.load(std::memory_order_acquire) &&
                   SteadyNanoseconds() < frame_wait_deadline.load(std::memory_order_acquire))
                std::this_thread::yield();
            frame_writer_started.store(true, std::memory_order_release);
            const auto token = control_created ? LumenAndroidFexCoreBeginGuestMemoryMutationV1() : 0;
            writer_before_drain = token != 0 &&
                !frame_wait_finished.load(std::memory_order_acquire) &&
                !frame_control_finished.load(std::memory_order_acquire);
            if (token != 0) {
                writer_before_drain &= Memory::ProtectGuestMemory(records + page, page, Vm::Mode::Read);
                LumenAndroidFexCoreEndGuestMemoryMutationV1(token, 0, 0, 0);
            }
        });
        frame_control_started.store(true, std::memory_order_release);
        writer.join();
        if (!control_created && waiter_created) round &= E::KernelSetEventFlag(frame_wait_flag, 1) == ::OK;
        void* waiter_result {}, *control_result {};
        round &= waiter_created && P::PthreadJoinHost(waiter, &waiter_result) == ::OK &&
            reinterpret_cast<std::uint64_t>(waiter_result) == 1;
        round &= control_created && P::PthreadJoinHost(controller, &control_result) == ::OK &&
            reinterpret_cast<std::uint64_t>(control_result) == 1;
        round &= Memory::ReadGuestMemory(nullptr, frame_wait_output, &bits, sizeof(bits)) &&
            bits == (mode == 4 ? 8U : 0U);
        if (mode == 4) {
            round &= Memory::ReadGuestMemory(nullptr, frame_control_output, &waiting, sizeof(waiting)) &&
                waiting == 1;
            round &= E::KernelDeleteEventFlag(frame_wait_flag) == ::OK;
        } else {
            // Delete revoked the handle while the waiter retained its native
            // owner; a replacement must not give the old handle new meaning.
            round &= E::KernelSetEventFlag(old_handle, 1) == P::KERNEL_ERROR_ESRCH;
            E::KernelEventFlag replacement {};
            round &= E::KernelCreateEventFlag(&replacement, "fex-frame-replacement", 0x10, 0, nullptr) == ::OK &&
                replacement != old_handle;
            round &= E::KernelSetEventFlag(old_handle, 1) == P::KERNEL_ERROR_ESRCH;
            round &= E::KernelDeleteEventFlag(replacement) == ::OK;
        }
        CheckCase(mode == 4 ? "event-flag-guest-cancel-drain-donates-reader-to-pending-writer"
                          : "event-flag-guest-delete-drain-donates-reader-and-pins-lifetime",
            round && writer_before_drain, pass);
        frame_wait_flag = nullptr;
    }
    frame_wait_output = frame_wait_timeout = frame_control_output = 0;
    pass &= Memory::FreeGuestMemory(records, page * 2);
    pass &= Memory::FreeGuestMemory(code_base, page);
    return pass;
}
