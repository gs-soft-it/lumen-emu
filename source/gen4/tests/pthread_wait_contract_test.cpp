// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/guest_cpu/fex_hle_bridge.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/threads.h"
#include "core/linker.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <thread>
#include <unistd.h>

namespace Libraries::Kernel {
int PS4_SYSV_ABI posix_pthread_once(PthreadOnce*, void PS4_SYSV_ABI (*)());
}
namespace Core {
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
using namespace Core::GuestCpu;
using namespace std::chrono_literals;

struct Event {
    std::atomic_bool value{};
    void Set() { value.store(true); value.notify_all(); }
    void Wait() { value.wait(false); }
};
template <typename Predicate> void Await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
struct CurrentThread {
    Pthread thread{};
    CurrentThread() {
        CHECK(g_curthread == nullptr);
        g_curthread = &thread;
        thread.cancel_enable = true;
    }
    ~CurrentThread() {
        PthreadCallbacks().Clear(reinterpret_cast<uintptr_t>(&thread));
        g_curthread = nullptr;
    }
};

bool Access(void*, std::uintptr_t address, std::size_t size, bool) {
    return address != 0 && size != 0 && address <= UINTPTR_MAX - size;
}
u64 WaitJoin(u64 target) {
    return reinterpret_cast<Pthread*>(target)->WaitForJoin(g_curthread, nullptr);
}
u64 Once(u64 control, u64 initializer) {
    return posix_pthread_once(reinterpret_cast<PthreadOnce*>(control),
        reinterpret_cast<void PS4_SYSV_ABI (*)()>(initializer));
}
struct Bridge {
    HleCallRegistry registry;
    HleGuestBridge bridge{registry, Access, nullptr};
    std::shared_ptr<HleCallAdapter> join{registry.Register(MakeHleCallAdapter(WaitJoin), "join-wait")};
    std::shared_ptr<HleCallAdapter> once{registry.Register(MakeHleCallAdapter(Once), "once")};
    u64 Invoke(const std::shared_ptr<HleCallAdapter>& adapter, u64 arg0, u64 arg1 = 0) {
        HleCallFrame frame{};
        frame.operation = adapter->Operation();
        frame.gpr[7] = arg0;
        frame.gpr[6] = arg1;
        CHECK(std::holds_alternative<bool>(bridge.Invoke(frame)));
        return frame.gpr[0];
    }
};

void CheckJoinWake() {
    for (unsigned i = 0; i < 64; ++i) {
        Pthread target{};
        target.tid = 2;
        Event entered;
        auto waiter = std::async(std::launch::async, [&] {
            CurrentThread current;
            entered.Set();
            CHECK(target.WaitForJoin(&current.thread, nullptr) == 0);
            CHECK(!current.thread.cancel_point);
            CHECK(target.ret == reinterpret_cast<void*>(0x1234));
        });
        entered.Wait();
        if (i & 1) std::this_thread::yield();
        target.ret = reinterpret_cast<void*>(0x1234);
        target.PublishTermination();
        CHECK(waiter.wait_for(3s) == std::future_status::ready);
        waiter.get();
    }
    CurrentThread current;
    Pthread target{};
    target.tid = 2;
    const OrbisKernelTimespec expired{0, 0};
    CHECK(target.WaitForJoin(&current.thread, &expired) == POSIX_ETIMEDOUT);
    CHECK(!current.thread.cancel_point);
    target.PublishTermination();
    CHECK(target.WaitForJoin(&current.thread, &expired) == 0);
}

std::atomic_uint cancellation_cleanups{};
void PS4_SYSV_ABI CancelCleanup(void* opaque) {
    auto* target = static_cast<Pthread*>(opaque);
    {
        std::scoped_lock lock{g_curthread->lock};
        g_curthread->join_target = nullptr;
    }
    std::scoped_lock lock{target->lock};
    target->joiner = nullptr;
    ++cancellation_cleanups;
}
void CheckJoinCancel(Bridge& bridge) {
    for (const bool before_wait : {true, false}) {
        Pthread target{}, joining{};
        target.tid = 2;
        joining.cancel_enable = true;
        joining.join_target = &target;
        target.joiner = &joining;
        if (before_wait) {
            std::scoped_lock lock{joining.lock};
            CHECK(joining.RequestCancellation());
        }
        auto waiter = std::async(std::launch::async, [&] {
            CHECK(g_curthread == nullptr);
            g_curthread = &joining;
            PthreadCallbacks().PushCleanup(reinterpret_cast<uintptr_t>(&joining),
                {reinterpret_cast<uintptr_t>(&CancelCleanup), reinterpret_cast<uintptr_t>(&target),
                 CallbackOrigin::Native});
            try {
                bridge.Invoke(bridge.join, reinterpret_cast<u64>(&target));
                CHECK(false);
            } catch (const Core::Fex::GuestThreadExit&) {
                CHECK(joining.cancelling && joining.no_cancel && !joining.cancel_point);
                CHECK(joining.ret == reinterpret_cast<void*>(1));
                CHECK(joining.join_target == nullptr && target.joiner == nullptr);
                CHECK(!IsGuestHleCallActive());
            }
            g_curthread = nullptr;
        });
        if (!before_wait) {
            Await([&] { return joining.cancel_point.load(); });
            std::scoped_lock lock{joining.lock};
            CHECK(joining.RequestCancellation());
        }
        CHECK(waiter.wait_for(3s) == std::future_status::ready);
        waiter.get();
        {
            std::scoped_lock lock{joining.lock};
            CHECK(!joining.RequestCancellation());
        }
        CHECK(joining.wake_sema.try_acquire());
        CHECK(!joining.wake_sema.try_acquire());
        CHECK(target.join_wait_mutex.try_lock());
        target.join_wait_mutex.unlock();
    }
    CHECK(cancellation_cleanups == 2);
    // A normal wake and a first cancellation wake can coexist without exceeding capacity.
    Pthread target{};
    target.wake_sema.release();
    {
        std::scoped_lock lock{target.lock};
        CHECK(target.RequestCancellation());
        CHECK(!target.RequestCancellation());
    }
    CHECK(target.wake_sema.try_acquire());
    CHECK(target.wake_sema.try_acquire());
    CHECK(!target.wake_sema.try_acquire());
    target.wake_sema.release();
    target.wake_sema.release();
    target.ClearWake();
    CHECK(!target.wake_sema.try_acquire());
}

std::atomic_uint initializations{};
Event* init_entered{};
Event* init_release{};
bool cancel_first_initializer{};
void PS4_SYSV_ABI InitializeOnce() {
    const auto invocation = initializations.fetch_add(1);
    if (invocation == 0) {
        init_entered->Set();
        init_release->Wait();
        if (cancel_first_initializer) posix_pthread_exit(reinterpret_cast<void*>(1));
    }
}
void CheckOnce(Bridge& bridge, bool cancel) {
    PthreadOnce control{};
    Event entered, release;
    initializations = 0;
    init_entered = &entered;
    init_release = &release;
    cancel_first_initializer = cancel;
    auto first = std::async(std::launch::async, [&] {
        CurrentThread current;
        try {
            CHECK(bridge.Invoke(bridge.once, reinterpret_cast<u64>(&control),
                                reinterpret_cast<u64>(&InitializeOnce)) == 0);
            CHECK(!cancel);
        } catch (const Core::Fex::GuestThreadExit&) {
            CHECK(cancel);
            CHECK(current.thread.cancelling && current.thread.ret == reinterpret_cast<void*>(1));
            CHECK(!IsGuestHleCallActive());
        }
    });
    entered.Wait();
    std::vector<std::future<void>> waiters;
    for (unsigned i = 0; i < 8; ++i) {
        waiters.push_back(std::async(std::launch::async, [&] {
            CurrentThread current;
            CHECK(bridge.Invoke(bridge.once, reinterpret_cast<u64>(&control),
                                reinterpret_cast<u64>(&InitializeOnce)) == 0);
        }));
    }
    Await([&] { return control.state.load() == PthreadOnceState::Wait; });
    release.Set();
    CHECK(first.wait_for(3s) == std::future_status::ready);
    first.get();
    for (auto& waiter : waiters) {
        CHECK(waiter.wait_for(3s) == std::future_status::ready);
        waiter.get();
    }
    CHECK(control.state == PthreadOnceState::Done);
    CHECK(initializations == (cancel ? 2U : 1U));
    CurrentThread current;
    CHECK(posix_pthread_once(&control, InitializeOnce) == 0);
    CHECK(initializations == (cancel ? 2U : 1U));
    control.state = static_cast<PthreadOnceState>(17);
    CHECK(posix_pthread_once(&control, InitializeOnce) == POSIX_EINVAL);
}
} // namespace

int main() {
    const auto path = std::filesystem::temp_directory_path() /
        ("lumen-pthread-wait-contract-" + std::to_string(getpid()));
    Common::FS::InitializeUserPaths(path);
    EmulatorSettingsImpl::SetInstance(std::make_shared<EmulatorSettingsImpl>());
    Common::Log::Setup("pthread-wait-contract.log");
    Core::PthreadWaitContractTest::InitializeBootMetadata();
    // Initialize the real callback-address classifier before concurrent Once calls.
    Common::Singleton<Core::Linker>::Instance();
    auto bridge = std::make_unique<Bridge>();
    CheckJoinWake();
    CheckJoinCancel(*bridge);
    CheckOnce(*bridge, false);
    CheckOnce(*bridge, true);
    Common::Log::Shutdown();
    std::filesystem::remove_all(path);
    std::puts("Production pthread wait/notify PASS: 64 exit races, deadline, deferred cancel before/during wait, one-shot wake, simultaneous normal/cancel permits, Once contention/reset/retry through HLE. Scope excludes ThreadState lookup/GC and pure-JIT async cancellation.");
}
