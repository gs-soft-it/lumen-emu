// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/threads/sleepq.h"
#include "core/linker.h"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <pthread.h>
#include <thread>
#include <unistd.h>

#ifndef LUMEN_GEN4_DIRECT_FEX
#error This contract exercises the direct pthread implementation.
#endif

namespace Libraries::Kernel {
s32 PS4_SYSV_ABI posix_pthread_mutex_init(PthreadMutexT*, const PthreadMutexAttrT*);
s32 PS4_SYSV_ABI posix_pthread_mutex_destroy(PthreadMutexT*);
s32 PS4_SYSV_ABI posix_pthread_mutex_lock(PthreadMutexT*);
}

namespace Core {
// Reuse the existing boot-metadata fixture access; no guest image is loaded.
class PthreadWaitContractTest final {
public:
    static void InitializeBootMetadata() {
        auto& info = Common::ElfInfo::Instance();
        info.sdk_ver = Common::ElfInfo::FW_900;
        info.initialized = true;
    }
};
} // namespace Core

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)

namespace {
using namespace Libraries::Kernel;
using namespace std::chrono_literals;

template <typename Predicate> void Await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

// Own the real metadata required by synchronization, as in the equeue fixture.
// VM-backed ThreadState allocation, registry, GC and translated execution are
// outside this test; none of the wait/signal operations are replaced.
struct ThreadRecord {
    Pthread thread{};
    ThreadRecord() {
        thread.tid = 2;
        thread.refcount = 1;
        thread.cancel_enable = true;
        thread.sleepqueue = new SleepQueue{};
    }
    ~ThreadRecord() {
        CHECK(thread.wchan == nullptr && thread.sleepqueue != nullptr);
        CHECK(thread.locklevel == 0 && !thread.cancel_point);
        delete thread.sleepqueue;
    }
};

struct CurrentThread {
    explicit CurrentThread(ThreadRecord& record) {
        CHECK(g_curthread == nullptr);
        g_curthread = &record.thread;
    }
    ~CurrentThread() { g_curthread = nullptr; }
};

struct Mutex {
    PthreadMutexT value{};
    explicit Mutex(PthreadMutexType type = PthreadMutexType::ErrorCheck) {
        PthreadMutexAttr attr{type, PthreadMutexProt::None, 0};
        auto* pointer = &attr;
        CHECK(posix_pthread_mutex_init(&value, &pointer) == 0);
    }
    ~Mutex() { CHECK(posix_pthread_mutex_destroy(&value) == 0); }
};

size_t Queued(PthreadCond& condition) {
    SleepqLock(&condition);
    auto* queue = SleepqLookup(&condition);
    const auto count = queue == nullptr ? 0 : queue->sq_blocked.size();
    SleepqUnlock(&condition);
    return count;
}

void CheckWaitFinished(PthreadCond& condition, PthreadMutex& mutex, Pthread& thread) {
    CHECK(mutex.IsOwned(&thread) == 0);
    CHECK(thread.wchan == nullptr && thread.sleepqueue != nullptr);
    CHECK(thread.mutex_obj == nullptr && !thread.cancel_point && thread.locklevel == 0);
    CHECK(Queued(condition) == 0);
}

void CheckMutexAndTimeoutPrecedence() {
    Mutex mutex(PthreadMutexType::Recursive);
    PthreadCond condition{};
    // The complete Wait checks ownership before consulting the timeout.
    const auto* unreadable = reinterpret_cast<const OrbisKernelTimespec*>(1);
    CHECK(condition.Wait(&mutex.value, unreadable) == POSIX_EPERM);
    CHECK(mutex.value->Lock(nullptr) == 0);
    CHECK(mutex.value->Lock(unreadable) == 0);
    CHECK(mutex.value->m_count == 1);
    CHECK(posix_pthread_mutex_destroy(&mutex.value) == POSIX_EBUSY);
    CHECK(mutex.value->Unlock() == 0);
    CHECK(mutex.value->Unlock() == 0);
    CHECK(mutex.value->Unlock() == POSIX_EPERM);

    CHECK(posix_pthread_mutex_destroy(&mutex.value) == 0);
    CHECK(mutex.value == reinterpret_cast<PthreadMutex*>(2));
    CHECK(posix_pthread_mutex_lock(&mutex.value) == 0);
    CHECK(mutex.value->IsOwned(g_curthread) == 0);
    CHECK(mutex.value->Unlock() == 0);
}

void CheckClock(ClockId clock) {
    Mutex mutex(PthreadMutexType::Recursive);
    PthreadCond condition{};
    condition.clock_id = clock;
    ThreadRecord record;
    std::atomic_bool finished{};
    std::thread waiter([&] {
        CurrentThread current(record);
        OrbisKernelTimespec deadline{};
        CHECK(posix_clock_gettime(static_cast<u32>(clock), &deadline) == 0);
        deadline.tv_sec += 60;
        CHECK(mutex.value->Lock(nullptr) == 0);
        CHECK(mutex.value->Lock(nullptr) == 0);
        CHECK(condition.Wait(&mutex.value, &deadline) == 0);
        CheckWaitFinished(condition, *mutex.value, record.thread);
        CHECK(mutex.value->m_count == 1);
        CHECK(mutex.value->Unlock() == 0);
        CHECK(mutex.value->Unlock() == 0);
        finished = true;
    });
    Await([&] { return Queued(condition) == 1 || finished.load(); });
    CHECK(!finished);
    CHECK(condition.Signal(nullptr) == 0);
    waiter.join();
    CHECK(finished);

    CHECK(mutex.value->Lock(nullptr) == 0);
    const OrbisKernelTimespec expired{0, 0};
    CHECK(condition.Wait(&mutex.value, &expired) == POSIX_ETIMEDOUT);
    CheckWaitFinished(condition, *mutex.value, *g_curthread);
    CHECK(condition.Wait(&mutex.value, THR_RELTIME, 0) == POSIX_ETIMEDOUT);
    CheckWaitFinished(condition, *mutex.value, *g_curthread);
    CHECK(mutex.value->Unlock() == 0);
}

void CheckFifoAndSignals() {
    constexpr size_t Count = 6;
    Mutex mutex;
    PthreadCond condition{};
    std::array<ThreadRecord, Count> records;
    std::array<std::thread, Count> waiters;
    std::array<std::atomic_bool, Count> finished{};
    CHECK(condition.Signal(nullptr) == 0);
    for (size_t i = 0; i < Count; ++i) {
        waiters[i] = std::thread([&, i] {
            CurrentThread current(records[i]);
            CHECK(mutex.value->Lock(nullptr) == 0);
            CHECK(condition.Wait(&mutex.value, nullptr) == 0);
            CHECK(mutex.value->IsOwned(&records[i].thread) == 0);
            CHECK(records[i].thread.wchan == nullptr && !records[i].thread.cancel_point);
            CHECK(mutex.value->Unlock() == 0);
            finished[i] = true;
        });
        Await([&] { return Queued(condition) == i + 1; });
    }
    CHECK(Queued(condition) == Count);
    // Ordinary signals select FIFO registration, including deferred wake while
    // the signalling thread owns the mutex. Wait for each result, not scheduling.
    for (size_t i = 0; i < 3; ++i) {
        CHECK(mutex.value->Lock(nullptr) == 0);
        CHECK(condition.Signal(nullptr) == 0);
        CHECK(g_curthread->nwaiter_defer == 1);
        CHECK(!finished[i]);
        CHECK(mutex.value->Unlock() == 0);
        CHECK(g_curthread->nwaiter_defer == 0);
        Await([&] { return finished[i].load(); });
        for (size_t next = i + 1; next < Count; ++next) CHECK(!finished[next]);
    }
    // Host-origin broadcast has no current guest thread and wakes the rest.
    auto* current = g_curthread;
    g_curthread = nullptr;
    CHECK(condition.Broadcast() == 0);
    g_curthread = current;
    for (auto& waiter : waiters) waiter.join();
    for (auto& done : finished) CHECK(done);
    CHECK(Queued(condition) == 0 && !condition.has_user_waiters);
}

void CheckRelativeRetryDeadline() {
    Mutex mutex;
    PthreadCond condition{};
    ThreadRecord record;
    std::atomic_bool finished{};
    std::thread waiter([&] {
        CurrentThread current(record);
        CHECK(mutex.value->Lock(nullptr) == 0);
        CHECK(condition.Wait(&mutex.value, THR_RELTIME, 100'000) == POSIX_ETIMEDOUT);
        CheckWaitFinished(condition, *mutex.value, record.thread);
        CHECK(mutex.value->Unlock() == 0);
        finished = true;
    });
    Await([&] { return Queued(condition) == 1 || finished.load(); });
    // Wake permits without queue removal exercise the production retry loop.
    // Keep at most one permit pending; completion must retain the original
    // relative deadline despite repeated wakeups. The watchdog tests liveness,
    // and does not measure or assert a performance threshold.
    Await([&] {
        if (finished) return true;
        SleepqLock(&condition);
        if (record.thread.wchan == &condition) {
            record.thread.ClearWake();
            record.thread.wake_sema.release();
        }
        SleepqUnlock(&condition);
        std::this_thread::sleep_for(1ms);
        return finished.load();
    });
    waiter.join();
}

enum class CancelAt { BeforeWait, DuringWait, ChainUnlock };
struct CancelContext {
    ThreadRecord record;
    Mutex mutex{PthreadMutexType::Recursive};
    PthreadCond condition{};
    CancelAt at;
    std::atomic_bool start{}, cleaned{}, after_call{};
    explicit CancelContext(CancelAt value) : at(value) {}
};

void PS4_SYSV_ABI CancelCleanup(void* opaque) {
    auto& context = *static_cast<CancelContext*>(opaque);
    auto& thread = context.record.thread;
    CHECK(g_curthread == &thread && thread.cancelling && thread.no_cancel);
    CHECK(thread.wchan == nullptr && thread.sleepqueue != nullptr);
    CHECK(thread.mutex_obj == nullptr && !thread.cancel_point && thread.locklevel == 0);
    CHECK(Queued(context.condition) == 0);
    CHECK(context.mutex.value->IsOwned(&thread) == 0);
    CHECK(context.mutex.value->m_count == 1);
    CHECK(context.mutex.value->Unlock() == 0);
    CHECK(context.mutex.value->Unlock() == 0);
    CHECK(!context.cleaned.exchange(true));
    // Observe production cancellation through removal, recursive re-lock and
    // native cleanup dispatch, then terminate this native worker here. Full
    // guest exit invokes VM-backed ThreadState, outside this isolated contract.
    CHECK(thread.ret == reinterpret_cast<void*>(1) && thread.cleanup.empty());
    thread.native_thr.Exit();
    CHECK(false);
}

void* CancelWorker(void* opaque) {
    auto& context = *static_cast<CancelContext*>(opaque);
    context.start.wait(false);
    auto& thread = context.record.thread;
    g_curthread = &thread;
    thread.native_thr.Initialize();
    PthreadCleanup cleanup{CancelCleanup, &context, 0};
    thread.cleanup.push_front(&cleanup);
    CHECK(context.mutex.value->Lock(nullptr) == 0);
    CHECK(context.mutex.value->Lock(nullptr) == 0);
    if (context.at == CancelAt::BeforeWait) thread.cancel_pending = true;
    if (context.at == CancelAt::ChainUnlock) {
        SleepqLock(&context.condition);
        CHECK(thread.locklevel == 1);
        thread.cancel_async = true;
        thread.cancel_pending = true;
        PthreadTestCancel();
        CHECK(!context.cleaned && !thread.cancelling);
        SleepqUnlock(&context.condition);
    } else {
        (void)context.condition.Wait(&context.mutex.value, nullptr);
    }
    context.after_call = true;
    CHECK(false);
    return nullptr;
}

void CheckCancellation(CancelAt at) {
    CancelContext context(at);
    auto& thread = context.record.thread;
    CHECK(thread.native_thr.Create(CancelWorker, &context) == 0);
    const auto handle = std::bit_cast<pthread_t>(thread.native_thr.GetHandle());
    context.start = true;
    context.start.notify_one();
    if (at == CancelAt::DuringWait) {
        Await([&] { return Queued(context.condition) == 1; });
        std::scoped_lock lock{thread.lock};
        // Exercise the real pending+wake primitive. Native signal delivery and
        // translated cancellation are covered by separate runtime contracts.
        CHECK(thread.RequestCancellation());
        CHECK(!thread.RequestCancellation());
    }
    CHECK(pthread_join(handle, nullptr) == 0);
    CHECK(context.cleaned && !context.after_call);
    CHECK(thread.ret == reinterpret_cast<void*>(1) && thread.cleanup.empty());
}
} // namespace

int main() {
    alarm(14);
    const auto path = std::filesystem::temp_directory_path() /
        ("lumen-thread-sync-contract-" + std::to_string(getpid()));
    Common::FS::InitializeUserPaths(path);
    EmulatorSettingsImpl::SetInstance(std::make_shared<EmulatorSettingsImpl>());
    Common::Log::Setup("thread-sync-contract.log");
    Core::PthreadWaitContractTest::InitializeBootMetadata();
    Common::Singleton<Core::Linker>::Instance();
    ThreadRecord main_record;
    CurrentThread current(main_record);
    CheckMutexAndTimeoutPrecedence();
    CheckClock(ClockId::Realtime);
    CheckClock(ClockId::Monotonic);
    CheckFifoAndSignals();
    CheckRelativeRetryDeadline();
    for (const auto at : {CancelAt::BeforeWait, CancelAt::DuringWait, CancelAt::ChainUnlock})
        CheckCancellation(at);
    alarm(0);
    std::puts("direct-thread-sync=PASS production clock/FIFO/broadcast/deferred-wake/relative-retry/mutex/cancel-relock/chain-guard; target registry, guest exit, native signal delivery, translated execution and GC excluded");
}
