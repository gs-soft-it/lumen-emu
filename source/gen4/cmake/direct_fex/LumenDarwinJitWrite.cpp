// SPDX-License-Identifier: MIT
#include "LumenDarwinJitWrite.h"

#include <TargetConditionals.h>
#include <cstdlib>
#include <exception>
#include <pthread.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#if TARGET_OS_IPHONE
#include <mach/mach.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif

#if TARGET_OS_IPHONE && defined(__aarch64__)
// An ordinary debugger leaves the legacy handshake at zero. The matching LLDB
// helper supplies the magic. An explicitly admitted universal provider uses the
// documented prepare/detach protocol without that LLDB-specific challenge.
extern "C" __attribute__((visibility("default"), noinline, optnone, naked))
std::uintptr_t LumenIOSJitDebuggerHandshake() {
    __asm__("mov x0, #0\nret\n");
}
extern "C" __attribute__((visibility("default"), noinline, optnone, naked))
void* LumenIOSJitPrepareRegion(void*, std::size_t) {
    __asm__("mov x16, #1\nbrk #0xf00d\nret\n");
}
extern "C" __attribute__((visibility("default"), noinline, optnone, naked))
void LumenIOSJitDetach() {
    __asm__("mov x16, #0\nbrk #0xf00d\nret\n");
}
#endif

namespace FEXCore::Allocator {
namespace {
thread_local unsigned callbackDepth {};
constexpr std::size_t jitPageSize = 16384;
constexpr std::size_t debuggerArenaSize = 512ULL * 1024 * 1024;
std::atomic<std::uintptr_t> debuggerRX {};
std::atomic<bool> debuggerReady {};
std::uintptr_t debuggerRW {};
std::mutex arenaMutex;
bool universalSessionPending {};
bool universalDetached {};
bool universalDetachRequested {};
struct Span { std::size_t offset; std::size_t size; };
std::vector<Span> freeSpans;
std::map<std::uintptr_t, std::size_t> allocations;

std::size_t PageRound(std::size_t size) {
    if (size == 0 || size > debuggerArenaSize) return 0;
    return (size + jitPageSize - 1) & ~(jitPageSize - 1);
}

struct Work {
    void (*invoke)(void*);
    void* context;
    std::exception_ptr failure;
};

int WriteJit(void* context) {
    auto& work = *static_cast<Work*>(context);
    ++callbackDepth;
    try {
        work.invoke(work.context);
    } catch (...) {
        work.failure = std::current_exception();
    }
    --callbackDepth;
    return 0;
}

#if TARGET_OS_IPHONE
PTHREAD_JIT_WRITE_ALLOW_CALLBACKS_NP(WriteJit);

int ReadDebuggerAttached(bool& attached) {
    kinfo_proc info {};
    std::size_t infoSize = sizeof(info);
    int mib[] {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    if (sysctl(mib, 4, &info, &infoSize, nullptr, 0) != 0)
        return errno != 0 ? errno : EACCES;
    if (infoSize != sizeof(info)) return EACCES;
    attached = (info.kp_proc.p_flag & P_TRACED) != 0;
    return 0;
}
#endif
} // namespace

int InitializeDebuggerJit() {
    return InitializeDebuggerJit(DebuggerJitProvider::LegacyLLDB);
}

int InitializeDebuggerJit(DebuggerJitProvider provider) {
    if (provider != DebuggerJitProvider::LegacyLLDB && provider != DebuggerJitProvider::Universal)
        return EINVAL;
#if TARGET_OS_IPHONE && defined(__aarch64__)
    std::lock_guard lock(arenaMutex);
    if (debuggerRX.load(std::memory_order_acquire)) {
        if (debuggerReady.load(std::memory_order_acquire)) return 0;
        if (provider == DebuggerJitProvider::Universal &&
            (universalSessionPending || universalDetached)) {
            bool attached = false;
            if (ReadDebuggerAttached(attached) != 0) return EACCES;
            if (attached) {
                // Explicit admission of a newly attached provider may retry a
                // prepared arena whose post-detach execution probe failed.
                universalDetachRequested = false;
                universalSessionPending = true;
                universalDetached = false;
                return 0;
            }
            // A detach we requested can finish after its observation timeout.
            // An unsolicited debugger disappearance is not successful detach.
            if (universalDetached || universalDetachRequested) {
                universalSessionPending = false;
                universalDetached = true;
                return 0;
            }
            return EACCES;
        }
        return EBUSY;
    }
    if (provider == DebuggerJitProvider::LegacyLLDB) {
        const char* requested = std::getenv("LUMEN_IOS_JIT_DEBUGGER");
        if (!requested || std::strcmp(requested, "1") != 0) return ENOTSUP;
    }
    if (sysconf(_SC_PAGESIZE) != jitPageSize) return ENOTSUP;
    bool attached = false;
    if (ReadDebuggerAttached(attached) != 0 || !attached) return EACCES;
    if (provider == DebuggerJitProvider::LegacyLLDB &&
        LumenIOSJitDebuggerHandshake() != 0x4c4a4954) return EACCES;
    // Free never needs to allocate storage, including the maximally fragmented
    // one-page case. Reserve before asking debugserver to commit any pages.
    try {
        freeSpans.reserve(debuggerArenaSize / jitPageSize + 1);
    } catch (...) {
        return ENOMEM;
    }
    // Recheck after bookkeeping allocation, immediately before the protocol
    // call. CS_DEBUGGED alone can remain set after the debugger has detached.
    if (ReadDebuggerAttached(attached) != 0 || !attached) return EACCES;
    if (provider == DebuggerJitProvider::Universal) {
        universalSessionPending = true;
        universalDetached = false;
        universalDetachRequested = false;
    }
    void* rx = LumenIOSJitPrepareRegion(nullptr, debuggerArenaSize);
    if (!rx || rx == MAP_FAILED) return EACCES;
    vm_address_t rw = 0;
    vm_prot_t current = 0, maximum = 0;
    const auto result = vm_remap(mach_task_self(), &rw, debuggerArenaSize, 0,
        VM_FLAGS_ANYWHERE, mach_task_self(), reinterpret_cast<vm_address_t>(rx),
        false, &current, &maximum, VM_INHERIT_DEFAULT);
    if (result != KERN_SUCCESS) {
        munmap(rx, debuggerArenaSize);
        return ENOMEM;
    }
    if (mprotect(reinterpret_cast<void*>(rw), debuggerArenaSize, PROT_READ | PROT_WRITE) != 0) {
        const int error = errno;
        munmap(reinterpret_cast<void*>(rw), debuggerArenaSize);
        munmap(rx, debuggerArenaSize);
        return error;
    }
    // Publish only when both views and allocation bookkeeping are ready.
    freeSpans.push_back({0, debuggerArenaSize});
    debuggerRW = rw;
    debuggerRX.store(reinterpret_cast<std::uintptr_t>(rx), std::memory_order_release);
    debuggerReady.store(provider == DebuggerJitProvider::LegacyLLDB, std::memory_order_release);
    return 0;
#else
    return ENOTSUP;
#endif
}

int DetachUniversalDebuggerJit() {
#if TARGET_OS_IPHONE && defined(__aarch64__)
    std::lock_guard lock(arenaMutex);
    bool attached = false;
    if (!universalSessionPending && !universalDetached) return EINVAL;
    if (ReadDebuggerAttached(attached) != 0) return EACCES;
    if (!universalSessionPending) return !attached ? 0 : EACCES;
    if (!attached) {
        if (!universalDetachRequested) return EACCES;
        universalSessionPending = false;
        universalDetached = true;
        return 0;
    }
    universalDetachRequested = true;
    LumenIOSJitDetach();
    // The universal detach request normally resumes the task after detaching.
    // Allow a bounded interval for the observable task flag to catch up.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (;;) {
        const auto error = ReadDebuggerAttached(attached);
        if (error != 0) return error;
        if (!attached) {
            universalSessionPending = false;
            universalDetached = true;
            return 0;
        }
        if (std::chrono::steady_clock::now() >= deadline) return ETIMEDOUT;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#else
    return ENOTSUP;
#endif
}

int CompleteUniversalDebuggerJit() {
#if TARGET_OS_IPHONE && defined(__aarch64__)
    std::lock_guard lock(arenaMutex);
    bool attached = false;
    if (!debuggerRX.load(std::memory_order_acquire) || !universalDetached || universalSessionPending)
        return EINVAL;
    if (ReadDebuggerAttached(attached) != 0 || attached) return EACCES;
    debuggerReady.store(true, std::memory_order_release);
    return 0;
#else
    return ENOTSUP;
#endif
}

bool IsDebuggerJit() {
    return debuggerRX.load(std::memory_order_acquire) != 0;
}

bool IsDebuggerJitReady() {
    return debuggerReady.load(std::memory_order_acquire);
}

void* JitWritableAddress(const void* pointer) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto rx = debuggerRX.load(std::memory_order_acquire);
    if (rx && address >= rx && address - rx < debuggerArenaSize)
        return reinterpret_cast<void*>(debuggerRW + (address - rx));
    return const_cast<void*>(pointer);
}

void* AllocateDebuggerJit(std::size_t size, void* hint) {
    const auto rounded = PageRound(size);
    std::lock_guard lock(arenaMutex);
    const auto rx = debuggerRX.load(std::memory_order_acquire);
    if (!rx || !rounded) { errno = EINVAL; return nullptr; }
    // Like mmap without MAP_FIXED, the hint is advisory only.
    (void)hint;
    for (auto it = freeSpans.begin(); it != freeSpans.end(); ++it) {
        if (it->size < rounded) continue;
        const auto result = rx + it->offset;
        try {
            allocations.emplace(result, rounded);
        } catch (...) {
            errno = ENOMEM;
            return nullptr;
        }
        it->offset += rounded;
        it->size -= rounded;
        if (!it->size) freeSpans.erase(it);
        // Match anonymous mmap's zero-fill contract on first use and reuse.
        std::memset(reinterpret_cast<void*>(debuggerRW + (result - rx)), 0, rounded);
        return reinterpret_cast<void*>(result);
    }
    errno = ENOMEM;
    return nullptr;
}

bool FreeDebuggerJit(void* pointer, std::size_t size) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto rx = debuggerRX.load(std::memory_order_acquire);
    if (!rx || address < rx || address - rx >= debuggerArenaSize) return false;
    std::lock_guard lock(arenaMutex);
    const auto allocation = allocations.find(address);
    if (allocation == allocations.end() || allocation->second != PageRound(size)) std::abort();
    Span released {address - rx, allocation->second};
    allocations.erase(allocation);
    auto next = freeSpans.begin();
    while (next != freeSpans.end() && next->offset < released.offset) ++next;
    next = freeSpans.insert(next, released);
    if (next != freeSpans.begin()) {
        auto previous = next - 1;
        if (previous->offset + previous->size == next->offset) {
            previous->size += next->size;
            next = freeSpans.erase(next) - 1;
        }
    }
    if (next + 1 != freeSpans.end() && next->offset + next->size == (next + 1)->offset) {
        next->size += (next + 1)->size;
        freeSpans.erase(next + 1);
    }
    return true;
}

void InvokeJitWriteCallback(void (*invoke)(void*), void* context) {
    Work work {invoke, context, {}};
#if TARGET_OS_IPHONE
    if (IsDebuggerJit() || callbackDepth != 0) {
        // pthread's write callback must never be nested; the outer invocation
        // already owns this thread's writable JIT window.
        WriteJit(&work);
    } else if (__builtin_available(iOS 17.4, *)) {
        pthread_jit_write_with_callback_np(WriteJit, &work);
    } else {
        std::abort(); // The capability gate rejects this before allocation.
    }
#else
    WriteJit(&work);
#endif
    if (work.failure) std::rethrow_exception(work.failure);
}
} // namespace FEXCore::Allocator
