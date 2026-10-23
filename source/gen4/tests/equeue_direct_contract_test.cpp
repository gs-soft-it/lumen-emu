// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/threads/pthread.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <latch>
#include <thread>
#include <unistd.h>

#ifndef LUMEN_GEN4_DIRECT_FEX
#error This contract exercises the direct event queue implementation.
#endif

namespace Libraries::Kernel {
int PS4_SYSV_ABI sceKernelCreateEqueue(OrbisKernelEqueue*, const char*);
int PS4_SYSV_ABI sceKernelDeleteEqueue(OrbisKernelEqueue);
int PS4_SYSV_ABI sceKernelWaitEqueue(OrbisKernelEqueue, OrbisKernelEvent*, int, int*,
                                  OrbisKernelUseconds*);
int PS4_SYSV_ABI sceKernelAddUserEvent(OrbisKernelEqueue, int);
int PS4_SYSV_ABI sceKernelAddUserEventEdge(OrbisKernelEqueue, int);
int PS4_SYSV_ABI sceKernelTriggerUserEvent(OrbisKernelEqueue, int, void*);
int PS4_SYSV_ABI sceKernelDeleteUserEvent(OrbisKernelEqueue, int);
class EventFlagInternal;
struct OrbisKernelEventFlagOptParam;
int PS4_SYSV_ABI sceKernelCreateEventFlag(EventFlagInternal**, const char*, u32, u64,
                                       const OrbisKernelEventFlagOptParam*);
int PS4_SYSV_ABI sceKernelDeleteEventFlag(EventFlagInternal*);
int PS4_SYSV_ABI sceKernelWaitEventFlag(EventFlagInternal*, u64, u32, u64*, OrbisKernelUseconds*);
int PS4_SYSV_ABI sceKernelPollEventFlag(EventFlagInternal*, u64, u32, u64*);
int PS4_SYSV_ABI sceKernelSetEventFlag(EventFlagInternal*, u64);
int PS4_SYSV_ABI sceKernelCancelEventFlag(EventFlagInternal*, u64, int*);
s32 PS4_SYSV_ABI sceKernelCreateSema(u32*, const char*, u32, s32, s32, const void*);
s32 PS4_SYSV_ABI sceKernelDeleteSema(u32);
s32 PS4_SYSV_ABI sceKernelWaitSema(u32, s32, u32*);
s32 PS4_SYSV_ABI sceKernelSignalSema(u32, s32);
s32 PS4_SYSV_ABI sceKernelPollSema(u32, s32);
s32 PS4_SYSV_ABI sceKernelCancelSema(u32, s32, s32*);
} // namespace Libraries::Kernel

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)

namespace {
using namespace Libraries::Kernel;

void CheckUserEvent(const OrbisKernelEvent& event, int id, void* user_data, bool edge) {
    CHECK(event.ident == static_cast<u64>(id));
    CHECK(event.filter == OrbisKernelEvent::Filter::User);
    CHECK(event.flags == (edge ? OrbisKernelEvent::Flags::Clear : 0));
    CHECK(event.fflags == 0);
    CHECK(event.data == 0);
    CHECK(event.udata == user_data);
}

void CheckEmptyPoll(OrbisKernelEqueue queue) {
    OrbisKernelEvent event{};
    OrbisKernelUseconds poll = 0;
    int count = -1;
    CHECK(sceKernelWaitEqueue(queue, &event, 1, &count, &poll) ==
          ORBIS_KERNEL_ERROR_ETIMEDOUT);
    CHECK(count == 0);
}

void CheckThreadEvent(OrbisKernelEqueue queue, int id, void* user_data, bool finite_wait) {
    std::latch start{1};
    std::thread producer([&] {
        start.wait();
        // Give the consumer an opportunity to enter the production wait. No
        // elapsed duration or scheduling order is asserted by this contract.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(sceKernelTriggerUserEvent(queue, id, user_data) == ORBIS_OK);
    });
    OrbisKernelEvent event{};
    OrbisKernelUseconds timeout = 2'000'000;
    int count = -1;
    start.count_down();
    const auto result = sceKernelWaitEqueue(queue, &event, 1, &count,
                                           finite_wait ? &timeout : nullptr);
    producer.join();
    CHECK(result == ORBIS_OK);
    CHECK(count == 1);
    CheckUserEvent(event, id, user_data, true);
    CheckEmptyPoll(queue);
}

// Kernel waits read the calling thread's priority/name. Use its real metadata
// type without starting a guest VM or replacing any synchronization operation.
struct CurrentThread {
    Pthread thread{};
    CurrentThread() {
        CHECK(g_curthread == nullptr);
        thread.attr.prio = ORBIS_KERNEL_PRIO_FIFO_DEFAULT;
        g_curthread = &thread;
    }
    ~CurrentThread() { g_curthread = nullptr; }
};

enum class FlagAction { Set, Cancel, Delete };

void CheckEventFlagWaiter(FlagAction action) {
    constexpr u32 SingleFifo = 0x11;
    constexpr u32 WaitAnd = 0x01;
    EventFlagInternal* flag{};
    CHECK(sceKernelCreateEventFlag(&flag, "event-flag-contract", SingleFifo, 0, nullptr) ==
          ORBIS_OK);
    CHECK(flag != nullptr);
    CurrentThread current;
    u64 result{};
    int wait_result{};
    std::thread waiter([&] {
        CurrentThread current;
        do {
            wait_result = sceKernelWaitEventFlag(flag, 1, WaitAnd, &result, nullptr);
            // A zero-timeout poll can briefly own the sole waiter slot before
            // this thread enters. Retry that valid SINGLE-mode response.
        } while (wait_result == ORBIS_KERNEL_ERROR_EPERM);
    });

    // Observe registration through the public SINGLE-mode contract. No
    // fabricated waiter, private layout access or scheduling delay is used.
    for (;;) {
        u64 polled{};
        const auto status = sceKernelPollEventFlag(flag, 2, WaitAnd, &polled);
        if (status == ORBIS_KERNEL_ERROR_EPERM) break;
        CHECK(status == ORBIS_KERNEL_ERROR_EBUSY);
        std::this_thread::yield();
    }
    switch (action) {
    case FlagAction::Set:
        CHECK(sceKernelSetEventFlag(flag, 1) == ORBIS_OK);
        break;
    case FlagAction::Cancel: {
        int count = -1;
        CHECK(sceKernelCancelEventFlag(flag, 8, &count) == ORBIS_OK);
        CHECK(count == 1);
        break;
    }
    case FlagAction::Delete:
        CHECK(sceKernelDeleteEventFlag(flag) == ORBIS_OK);
        break;
    }
    waiter.join();
    if (action == FlagAction::Set) {
        CHECK(wait_result == ORBIS_OK);
        CHECK(result == 1);
    } else if (action == FlagAction::Cancel) {
        CHECK(wait_result == ORBIS_KERNEL_ERROR_ECANCELED);
        CHECK(result == 8);
    } else {
        CHECK(wait_result == ORBIS_KERNEL_ERROR_EACCES);
        CHECK(result == 0);
    }
    if (action != FlagAction::Delete) CHECK(sceKernelDeleteEventFlag(flag) == ORBIS_OK);
}

void CheckSemaphoreWake() {
    u32 sem{};
    CHECK(sceKernelCreateSema(&sem, "semaphore-wake-contract", 1, 0, 4, nullptr) == ORBIS_OK);
    CHECK(sceKernelPollSema(sem, 1) == ORBIS_KERNEL_ERROR_EBUSY);
    std::latch start{1};
    s32 wait_result{};
    std::thread waiter([&] {
        CurrentThread current;
        start.count_down();
        wait_result = sceKernelWaitSema(sem, 2, nullptr);
    });
    start.wait();
    // As for the event queue, allow the real waiter to enter without making
    // a scheduler-order or elapsed-time assertion.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(sceKernelSignalSema(sem, 2) == ORBIS_OK);
    waiter.join();
    CHECK(wait_result == ORBIS_OK);
    CHECK(sceKernelPollSema(sem, 1) == ORBIS_KERNEL_ERROR_EBUSY);
    CHECK(sceKernelDeleteSema(sem) == ORBIS_OK);
}

void CheckSemaphoreCancel() {
    u32 sem{};
    CHECK(sceKernelCreateSema(&sem, "semaphore-cancel-contract", 1, 0, 4, nullptr) == ORBIS_OK);
    s32 wait_result{};
    std::thread waiter([&] {
        CurrentThread current;
        wait_result = sceKernelWaitSema(sem, 1, nullptr);
    });
    // Cancel reports the number of actual registered waiters. A zero-waiter
    // cancellation leaves the zero token count unchanged; retry until this
    // thread has entered the production wait, then check its cancellation.
    for (;;) {
        s32 count = -1;
        CHECK(sceKernelCancelSema(sem, 0, &count) == ORBIS_OK);
        if (count == 1) break;
        CHECK(count == 0);
        std::this_thread::yield();
    }
    waiter.join();
    CHECK(wait_result == ORBIS_KERNEL_ERROR_ECANCELED);
    CHECK(sceKernelPollSema(sem, 1) == ORBIS_KERNEL_ERROR_EBUSY);
    s32 count = -1;
    CHECK(sceKernelCancelSema(sem, 3, &count) == ORBIS_OK);
    CHECK(count == 0);
    CHECK(sceKernelPollSema(sem, 2) == ORBIS_OK);
    CHECK(sceKernelPollSema(sem, 1) == ORBIS_OK);
    CHECK(sceKernelPollSema(sem, 1) == ORBIS_KERNEL_ERROR_EBUSY);
    CHECK(sceKernelDeleteSema(sem) == ORBIS_OK);
}
void CheckSemaphoreInvalidHandle() {
    constexpr u32 invalid = 0xFFFFFFFFu;
    CHECK(sceKernelWaitSema(invalid, 1, nullptr) == ORBIS_KERNEL_ERROR_ESRCH);
    CHECK(sceKernelSignalSema(invalid, 1) == ORBIS_KERNEL_ERROR_ESRCH);
    CHECK(sceKernelPollSema(invalid, 1) == ORBIS_KERNEL_ERROR_ESRCH);
    CHECK(sceKernelCancelSema(invalid, 0, nullptr) == ORBIS_KERNEL_ERROR_ESRCH);
    CHECK(sceKernelDeleteSema(invalid) == ORBIS_KERNEL_ERROR_ESRCH);
}

void CheckSemaphoreRegistryConcurrency() {
    std::array<std::thread, 4> workers;
    for (auto& worker : workers) {
        worker = std::thread([] {
            for (unsigned iteration = 0; iteration < 512; ++iteration) {
                u32 sem{};
                CHECK(sceKernelCreateSema(&sem, "semaphore-registry-contract", 1, 1, 4, nullptr) == ORBIS_OK);
                CHECK(sceKernelPollSema(sem, 1) == ORBIS_OK);
                CHECK(sceKernelSignalSema(sem, 1) == ORBIS_OK);
                CHECK(sceKernelWaitSema(sem, 1, nullptr) == ORBIS_OK);
                CHECK(sceKernelCancelSema(sem, 0, nullptr) == ORBIS_OK);
                CHECK(sceKernelDeleteSema(sem) == ORBIS_OK);
            }
        });
    }
    for (auto& worker : workers) worker.join();
}

void CheckSemaphoreDeleteWaiters() {
    // Exercise deletion while waiters still need to reacquire the object's
    // mutex, for finite and indefinite waits. Each API invocation must retain
    // the old object until all waiting calls have returned.
    size_t registered_deletions = 0;
    for (unsigned iteration = 0; iteration < 64; ++iteration) {
        u32 sem{};
        CHECK(sceKernelCreateSema(&sem, "semaphore-delete-contract", 1, 0, 4, nullptr) == ORBIS_OK);
        constexpr size_t Count = 4;
        std::latch start{Count};
        std::array<s32, Count> results{};
        std::array<std::thread, Count> waiters;
        for (size_t i = 0; i < Count; ++i) {
            waiters[i] = std::thread([&, i] {
                CurrentThread current;
                u32 timeout = 2'000'000;
                start.count_down();
                results[i] = sceKernelWaitSema(sem, 1, i % 2 ? &timeout : nullptr);
            });
        }
        start.wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(sceKernelDeleteSema(sem) == ORBIS_OK);
        CHECK(sceKernelPollSema(sem, 1) == ORBIS_KERNEL_ERROR_ESRCH);
        // The waiter may not have entered before deletion. Such calls must
        // fail with ESRCH; require actual blocked-waiter coverage overall.
        for (auto& waiter : waiters) waiter.join();
        for (auto result : results) {
            CHECK(result == ORBIS_KERNEL_ERROR_EACCES || result == ORBIS_KERNEL_ERROR_ESRCH);
            registered_deletions += result == ORBIS_KERNEL_ERROR_EACCES;
        }
        CHECK(sceKernelDeleteSema(sem) == ORBIS_KERNEL_ERROR_ESRCH);
    }
    CHECK(registered_deletions > 0);
}

void CheckSemaphoreCancelDeleteAndReuse() {
    // Cancel gives deterministic proof that the production waiter registered.
    // Immediately retiring and reusing the handle must not destroy the native
    // object before that canceled waiter has finished using its mutex.
    for (unsigned iteration = 0; iteration < 64; ++iteration) {
        u32 sem{};
        CHECK(sceKernelCreateSema(&sem, "semaphore-retire-contract", 1, 0, 4, nullptr) == ORBIS_OK);
        s32 result{};
        std::thread waiter([&] {
            CurrentThread current;
            result = sceKernelWaitSema(sem, 1, nullptr);
        });
        for (;;) {
            s32 count = -1;
            CHECK(sceKernelCancelSema(sem, 0, &count) == ORBIS_OK);
            if (count == 1) break;
            CHECK(count == 0);
            std::this_thread::yield();
        }
        CHECK(sceKernelDeleteSema(sem) == ORBIS_OK);
        u32 replacement{};
        CHECK(sceKernelCreateSema(&replacement, "semaphore-replacement", 1, 1, 4, nullptr) == ORBIS_OK);
        CHECK(replacement == sem);
        waiter.join();
        CHECK(result == ORBIS_KERNEL_ERROR_ECANCELED);
        CHECK(sceKernelPollSema(replacement, 1) == ORBIS_OK);
        CHECK(sceKernelPollSema(replacement, 1) == ORBIS_KERNEL_ERROR_EBUSY);
        CHECK(sceKernelDeleteSema(replacement) == ORBIS_OK);
    }
}

} // namespace

int main() {
    alarm(10);
    OrbisKernelEqueue queue = -1;
    CHECK(sceKernelCreateEqueue(&queue, "equeue-direct-contract") == ORBIS_OK);
    CHECK(GetEqueue(queue) != nullptr);
    CHECK(GetEqueue(queue)->GetName() == "equeue-direct-contract");
    CheckEmptyPoll(queue);

    int first_data{}, second_data{};
    constexpr int level_id = 17;
    CHECK(sceKernelAddUserEvent(queue, level_id) == ORBIS_OK);
    CHECK(sceKernelTriggerUserEvent(queue, level_id, &first_data) == ORBIS_OK);
    OrbisKernelEvent event{};
    int count = -1;
    CHECK(sceKernelWaitEqueue(queue, &event, 1, &count, nullptr) == ORBIS_OK);
    CHECK(count == 1);
    CheckUserEvent(event, level_id, &first_data, false);
    // Ordinary user events remain triggered until removed; Clear events below
    // are consumed by the same production GetTriggeredEvents implementation.
    OrbisKernelUseconds poll = 0;
    count = -1;
    CHECK(sceKernelWaitEqueue(queue, &event, 1, &count, &poll) == ORBIS_OK);
    CHECK(count == 1);
    CheckUserEvent(event, level_id, &first_data, false);
    CHECK(sceKernelDeleteUserEvent(queue, level_id) == ORBIS_OK);
    CheckEmptyPoll(queue);

    constexpr int edge_id = 23;
    CHECK(sceKernelAddUserEventEdge(queue, edge_id) == ORBIS_OK);
    CheckEmptyPoll(queue);
    CHECK(sceKernelTriggerUserEvent(queue, edge_id, &second_data) == ORBIS_OK);
    count = -1;
    CHECK(sceKernelWaitEqueue(queue, &event, 1, &count, &poll) == ORBIS_OK);
    CHECK(count == 1);
    CheckUserEvent(event, edge_id, &second_data, true);
    CheckEmptyPoll(queue);

    OrbisKernelUseconds timeout = 1'000;
    count = -1;
    CHECK(sceKernelWaitEqueue(queue, &event, 1, &count, &timeout) ==
          ORBIS_KERNEL_ERROR_ETIMEDOUT);
    CHECK(count == 0);
    CheckThreadEvent(queue, edge_id, &first_data, true);
    CheckThreadEvent(queue, edge_id, &second_data, false);

    CHECK(sceKernelDeleteUserEvent(queue, edge_id) == ORBIS_OK);
    CHECK(sceKernelDeleteEqueue(queue) == ORBIS_OK);
    CHECK(GetEqueue(queue) == nullptr);
    CheckEventFlagWaiter(FlagAction::Set);
    CheckEventFlagWaiter(FlagAction::Cancel);
    CheckEventFlagWaiter(FlagAction::Delete);
    CheckSemaphoreInvalidHandle();
    CheckSemaphoreRegistryConcurrency();
    CheckSemaphoreWake();
    CheckSemaphoreCancel();
    CheckSemaphoreDeleteWaiters();
    CheckSemaphoreCancelDeleteAndReuse();
    alarm(0);
    std::puts("EqueueDirectContractTest: PASS production create/delete, user event output, "
              "level retention, edge clear, empty poll, timeout, finite/indefinite thread wake; "
              "EventFlag registered waiter set/cancel/delete; "
              "Semaphore wait/signal, registered waiter cancel/count, token reset, "
              "concurrent registry/delete/wait and canceled waiter handle reuse; "
              "scope=native-api timers/signals/guest-bridge=not-tested");
}
