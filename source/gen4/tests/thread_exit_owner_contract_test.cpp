// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/thread.h"

#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef LUMEN_GEN4_DIRECT_FEX
#error This contract exercises the direct NativeThread implementation.
#endif

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)

namespace {
constexpr int RetiredOwnerFault = 77;
std::uintptr_t owner_begin{};
std::size_t owner_size{};

void Fault(int, siginfo_t* info, void*) {
    const auto address = reinterpret_cast<std::uintptr_t>(info->si_addr);
    _exit(address >= owner_begin && address - owner_begin < owner_size ? RetiredOwnerFault : 78);
}

struct Context {
    Core::NativeThread* owner{};
    bool prepared{};
    std::atomic_bool start{}, ready{}, retired{};
    std::atomic_uint cleanups{};
};

void Cleanup(void* opaque) {
    ++static_cast<Context*>(opaque)->cleanups;
}

void* Run(void* opaque) {
    auto& context = *static_cast<Context*>(opaque);
    context.start.wait(false);
    context.owner->Initialize();
    pthread_cleanup_push(Cleanup, &context);
    if (context.prepared) {
        CHECK(context.owner->PrepareExit());
    }
    // Deterministically give the collector the entire owner after the point
    // at which the kernel publishes termination. No sleeps or race lottery.
    context.ready = true;
    context.ready.notify_one();
    context.retired.wait(false);
    if (context.prepared) {
        Core::NativeThread::ExitCurrentThread();
    } else {
        context.owner->Exit();
    }
    CHECK(false);
    pthread_cleanup_pop(0);
    return nullptr;
}

void Child(bool prepared) {
    alarm(5);
    owner_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    CHECK(owner_size >= sizeof(Core::NativeThread));
    void* mapping = mmap(nullptr, owner_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(mapping != MAP_FAILED);
    owner_begin = reinterpret_cast<std::uintptr_t>(mapping);
    struct sigaction action{};
    action.sa_sigaction = Fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGSEGV, &action, nullptr) == 0);
    CHECK(sigaction(SIGBUS, &action, nullptr) == 0);

    auto* owner = std::construct_at(static_cast<Core::NativeThread*>(mapping));
    Context context{owner, prepared};
    CHECK(owner->Create(Run, &context) == 0);
    const auto handle = std::bit_cast<pthread_t>(owner->GetHandle());
    context.start = true;
    context.start.notify_one();
    context.ready.wait(false);
    CHECK(mprotect(mapping, owner_size, PROT_NONE) == 0);
    context.retired = true;
    context.retired.notify_one();
    CHECK(pthread_join(handle, nullptr) == 0);
    CHECK(prepared && context.cleanups == 1);
    CHECK(mprotect(mapping, owner_size, PROT_READ | PROT_WRITE) == 0);
    std::destroy_at(owner);
    CHECK(munmap(mapping, owner_size) == 0);
    _exit(0);
}
} // namespace

int main() {
    for (const bool prepared : {false, true}) {
        const pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) Child(prepared);
        int status{};
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == (prepared ? 0 : RetiredOwnerFault));
    }
    std::puts("NativeThread retired-owner contract PASS: legacy access faults at retired owner; prepared static exit joins with cleanup. Kernel GC scheduling and FEX execution are outside this fixture.");
}
