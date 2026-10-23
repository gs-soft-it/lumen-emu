#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise actual pinned SDL wake/lifecycle functions with host semaphore primitives.

This isolates the Android event wake contract. It does not test Android rendering,
controllers, or measure frame rate. The unmodified source must reproduce self-wake.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def function(text, signature):
    begin = text.index(signature)
    opening = text.index("{", begin)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[begin:end]


PRELUDE = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#define SDL_PLATFORM_ANDROID 1
#define SDL_assert assert
#define SDL_memmove std::memmove
#define SDL_arraysize(a) (sizeof(a) / sizeof((a)[0]))
using Sint64 = int64_t;
using SDL_ThreadID = uint64_t;
SDL_ThreadID SDL_GetCurrentThreadID() {
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
}
SDL_ThreadID SDL_VideoThreadID = SDL_GetCurrentThreadID();
SDL_ThreadID SDL_EventsThreadID = 0;
SDL_ThreadID SDL_MainThreadID = SDL_VideoThreadID;
struct Semaphore {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned count = 0;
    std::atomic<bool> entered_wait{false};
};
std::mutex lifecycle_mutex;
Semaphore lifecycle_sem;
auto* Android_LifecycleMutex = &lifecycle_mutex;
auto* Android_LifecycleEventSem = &lifecycle_sem;
void SDL_LockMutex(std::mutex* mutex) { mutex->lock(); }
void SDL_UnlockMutex(std::mutex* mutex) { mutex->unlock(); }
void SDL_SignalSemaphore(Semaphore* semaphore) {
    std::lock_guard lock(semaphore->mutex);
    ++semaphore->count;
    semaphore->changed.notify_one();
}
bool SDL_WaitSemaphoreTimeoutNS(Semaphore* semaphore, Sint64 timeout) {
    std::unique_lock lock(semaphore->mutex);
    if (!semaphore->count && timeout) {
        semaphore->entered_wait = true;
        if (timeout < 0) {
            semaphore->changed.wait(lock, [&] { return semaphore->count; });
        } else {
            semaphore->changed.wait_for(lock, std::chrono::nanoseconds(timeout),
                                       [&] { return semaphore->count; });
        }
    }
    if (!semaphore->count) return false;
    --semaphore->count;
    return true;
}
enum SDL_AndroidLifecycleEvent {
    SDL_ANDROID_LIFECYCLE_WAKE, SDL_ANDROID_LIFECYCLE_PAUSE,
    SDL_ANDROID_LIFECYCLE_RESUME, SDL_ANDROID_LIFECYCLE_LOWMEMORY,
    SDL_ANDROID_LIFECYCLE_DESTROY, SDL_NUM_ANDROID_LIFECYCLE_EVENTS
};
SDL_AndroidLifecycleEvent Android_LifecycleEvents[SDL_NUM_ANDROID_LIFECYCLE_EVENTS];
int Android_NumLifecycleEvents = 0;
'''

TEST = r'''
int main() {
    SDL_AndroidLifecycleEvent event;
    // This is the same wake function called when the event pump adds its sentinel.
    SDL_SendWakeupEvent();
    if (Android_WaitLifecycleEvent(&event, 0)) {
        std::puts("REPRODUCED: event-thread self-wake prevents blocking");
        return 2;
    }
    auto begin = std::chrono::steady_clock::now();
    assert(!Android_WaitLifecycleEvent(&event, 20000000));
    assert(std::chrono::steady_clock::now() - begin >= std::chrono::milliseconds(15));

    // A producer that wins the race before the wait must leave a wake token.
    std::atomic<int> payload{0};
    std::thread before([&] { payload = 42; SDL_SendWakeupEvent(); });
    before.join();
    assert(Android_WaitLifecycleEvent(&event, 1000000000));
    assert(event == SDL_ANDROID_LIFECYCLE_WAKE && payload == 42);

    // A producer arriving after the consumer parks must release the same wait.
    lifecycle_sem.entered_wait = false;
    std::thread during([&] {
        while (!lifecycle_sem.entered_wait.load()) std::this_thread::yield();
        payload = 43;
        SDL_SendWakeupEvent();
    });
    assert(Android_WaitLifecycleEvent(&event, 1000000000));
    during.join();
    assert(event == SDL_ANDROID_LIFECYCLE_WAKE && payload == 43);

    // Coalescing and actual Android lifecycle delivery remain unchanged.
    std::thread duplicate([] { SDL_SendWakeupEvent(); SDL_SendWakeupEvent(); });
    duplicate.join();
    assert(Android_WaitLifecycleEvent(&event, 0));
    assert(event == SDL_ANDROID_LIFECYCLE_WAKE);
    assert(!Android_WaitLifecycleEvent(&event, 0));
    for (auto expected : {SDL_ANDROID_LIFECYCLE_PAUSE, SDL_ANDROID_LIFECYCLE_RESUME,
                          SDL_ANDROID_LIFECYCLE_LOWMEMORY, SDL_ANDROID_LIFECYCLE_DESTROY}) {
        Android_SendLifecycleEvent(expected);
        assert(Android_WaitLifecycleEvent(&event, 0) && event == expected);
    }
    std::puts("PASS: no self-wake; worker wakes before/during wait; lifecycle and coalescing intact");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--overlay", type=Path, required=True)
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    android = (args.upstream / "src/core/android/SDL_android.c").read_text()
    sdl = (args.upstream / "src/SDL.c").read_text()
    notice = sdl[:sdl.index('#include')]
    common = PRELUDE + function(sdl, "bool SDL_IsMainThread(void)")
    for signature in ("static int FindLifecycleEvent(", "static void RemoveLifecycleEvent(",
                      "void Android_SendLifecycleEvent(", "bool Android_WaitLifecycleEvent("):
        common += "\n" + function(android, signature)
    with tempfile.TemporaryDirectory(prefix="gen4-event-wake-") as temp:
        temp = Path(temp)
        for name, source, expected in (("upstream", args.upstream, 2), ("overlay", args.overlay, 0)):
            events = (source / "src/events/SDL_events.c").read_text()
            body = function(events, "static void SDL_SendWakeupEvent(void)")
            code = temp / (name + ".cpp")
            code.write_text(notice + common + "\n" + body + TEST)
            binary = temp / name
            subprocess.run([args.cxx, "-std=c++20", "-O2", "-pthread", str(code), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            print(name + ": " + result.stdout.strip(), flush=True)
            if result.returncode != expected:
                raise RuntimeError(f"{name}: unexpected exit {result.returncode}: {result.stderr}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
