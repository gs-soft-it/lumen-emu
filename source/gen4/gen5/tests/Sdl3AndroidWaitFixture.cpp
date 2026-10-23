// SPDX-License-Identifier: GPL-2.0-only
// Isolated host fixture. SDL bodies are extracted verbatim by Sdl3AndroidWaitTests.py.
// SDL 3.4.16 is zlib licensed; production includes retain source provenance in
// receipt.json and the unchanged pinned SDL source tree.
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>

using Uint32 = uint32_t; using Uint64 = uint64_t;
using Sint64 = int64_t; using Sint32 = int32_t;
#define SDL_PLATFORM_ANDROID 1
#define SDL_assert assert
#define SDL_arraysize(x) int(sizeof(x) / sizeof((x)[0]))
#define SDL_copyp(a,b) (*(a) = *(b))
#define SDL_zero(x) std::memset(&(x), 0, sizeof(x))
#define SDL_MS_TO_NS(x) (Sint64(x) * 1000000)
#define CHECK_PARAM(x) if (x)
#define SDL_INIT_EVENTS 1
#define SDL_MAX_QUEUED_EVENTS 65535
#define SDL_MAIN_CALLBACK_WAITING 0
#define SDL_MAIN_CALLBACK_COMPLETE 1
#define SDL_MAIN_CALLBACK_CANCELED 2
enum { SDL_EVENT_FIRST=0, SDL_EVENT_QUIT=0x100,
       SDL_EVENT_WILL_ENTER_BACKGROUND=0x103, SDL_EVENT_DID_ENTER_BACKGROUND=0x104,
       SDL_EVENT_WILL_ENTER_FOREGROUND=0x105, SDL_EVENT_DID_ENTER_FOREGROUND=0x106,
       SDL_EVENT_TERMINATING=0x101, SDL_EVENT_LOW_MEMORY=0x102,
       SDL_EVENT_POLL_SENTINEL=0x7f00, SDL_EVENT_USER=0x8000, SDL_EVENT_LAST=0xffff };
enum SDL_EventAction { SDL_ADDEVENT, SDL_PEEKEVENT, SDL_GETEVENT };
struct SDL_Event { union { Uint32 type; struct { Uint32 type; Uint64 timestamp; } common; }; };
struct SDL_AtomicInt { int value{}; };
static int SDL_GetAtomicInt(SDL_AtomicInt* p) { return std::atomic_ref(p->value).load(); }
static int SDL_AddAtomicInt(SDL_AtomicInt* p, int v) { return std::atomic_ref(p->value).fetch_add(v); }
static void SDL_SetAtomicInt(SDL_AtomicInt* p, int v) { std::atomic_ref(p->value).store(v); }
using SDL_Mutex = std::mutex;
struct SDL_Semaphore { std::mutex mutex; std::condition_variable cv; int count; };
static SDL_Mutex lifecycle_mutex, queue_mutex, callback_mutex;
static SDL_Semaphore lifecycle_sem{{}, {}, 0};
static std::atomic<int> pump_count{}, wake_count{}, blocking_waits{};
static std::thread::id main_thread;
static bool sentinel_enabled = true;
static SDL_Mutex* Android_LifecycleMutex = &lifecycle_mutex;
static SDL_Semaphore* Android_LifecycleEventSem = &lifecycle_sem;
static void SDL_LockMutex(SDL_Mutex* p) { p->lock(); }
static void SDL_UnlockMutex(SDL_Mutex* p) { p->unlock(); }
static SDL_Semaphore* SDL_CreateSemaphore(int n) { return new SDL_Semaphore{{}, {}, n}; }
static void SDL_DestroySemaphore(SDL_Semaphore* p) { delete p; }
static void SDL_SignalSemaphore(SDL_Semaphore* p) {
    { std::lock_guard lock(p->mutex); ++p->count; }
    p->cv.notify_one();
}
static bool SDL_WaitSemaphoreTimeoutNS(SDL_Semaphore* p, Sint64 timeout) {
    std::unique_lock lock(p->mutex);
    if (timeout != 0 && p == Android_LifecycleEventSem && p->count == 0) ++blocking_waits;
    if (timeout < 0) p->cv.wait(lock, [&] { return p->count > 0; });
    else if (!p->cv.wait_for(lock, std::chrono::nanoseconds(timeout), [&] { return p->count > 0; })) return false;
    --p->count;
    return true;
}
static void SDL_WaitSemaphore(SDL_Semaphore* p) { SDL_WaitSemaphoreTimeoutNS(p, -1); }
static bool SDL_IsMainThread() { return std::this_thread::get_id() == main_thread; }
static bool SDL_WasInit(int) { return true; }
static Uint64 SDL_GetTicksNS() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static bool SDL_SetError(const char*, ...) { return false; }
static void SDL_ClearError() {}
static void SDL_InvalidParamError(const char*) {}
static void* SDL_malloc(size_t n) { return std::malloc(n); }
static void SDL_free(void* p) { std::free(p); }
static void SDL_memmove(void* to, const void* from, size_t n) { std::memmove(to, from, n); }
struct SDL_EventEntry { SDL_Event event; void* memory; SDL_EventEntry* prev; SDL_EventEntry* next; };
static struct { SDL_Mutex* lock; bool active; SDL_AtomicInt count; int max_events_seen;
    SDL_EventEntry* head; SDL_EventEntry* tail; SDL_EventEntry* free;
} SDL_EventQ{&queue_mutex, true, {}, 0, nullptr, nullptr, nullptr};
static SDL_AtomicInt SDL_sentinel_pending;
static Uint32 SDL_last_event_id{};
static int SDL_EventLoggingVerbosity{};
static void SDL_LogEvent(SDL_Event*) {}
static void SDL_TransferTemporaryMemoryToEvent(SDL_EventEntry*) {}
static void SDL_TransferTemporaryMemoryFromEvent(SDL_EventEntry*) {}
static void SDL_FreeTemporaryMemory() {}
static void SDL_ReleaseAutoReleaseKeys() {}
static void SDL_PumpEventMaintenance() { ++pump_count; }
static bool SDL_EventEnabled(int type) { return type != SDL_EVENT_POLL_SENTINEL || sentinel_enabled; }
static bool SDL_CallEventWatchers(SDL_Event*) { return true; }
using SDL_MainThreadCallback = void (*)(void*);
struct SDL_MainThreadCallbackEntry { SDL_MainThreadCallback callback; void* userdata;
    SDL_AtomicInt state; SDL_Semaphore* semaphore; SDL_MainThreadCallbackEntry* next; };
static SDL_Mutex* SDL_main_callbacks_lock = &callback_mutex;
static SDL_MainThreadCallbackEntry* SDL_main_callbacks_head{};
static SDL_MainThreadCallbackEntry* SDL_main_callbacks_tail{};
enum SDL_AndroidLifecycleEvent { SDL_ANDROID_LIFECYCLE_WAKE, SDL_ANDROID_LIFECYCLE_PAUSE,
    SDL_ANDROID_LIFECYCLE_RESUME, SDL_ANDROID_LIFECYCLE_LOWMEMORY, SDL_ANDROID_LIFECYCLE_DESTROY };
static SDL_AndroidLifecycleEvent Android_LifecycleEvents[8];
static int Android_NumLifecycleEvents{};
static bool Android_Paused{}, Android_BlockOnPause=true, Android_Destroyed{};
static void Android_SendLifecycleEvent(SDL_AndroidLifecycleEvent);
static bool Android_WaitLifecycleEvent(SDL_AndroidLifecycleEvent*, Sint64);
static void Android_PumpEvents(Sint64);
static bool SDL_PushEvent(SDL_Event*);
static int SDL_PeepEvents(SDL_Event*, int, SDL_EventAction, Uint32, Uint32);
static bool SDL_WaitEventTimeoutNS(SDL_Event*, Sint64);
static void Android_OnPause(); static void Android_OnResume();
static void Android_OnLowMemory(); static void Android_OnDestroy();

#ifdef TEST_BASELINE
#include "production-baseline.inc"
#else
#include "production-proposed.inc"
#endif

static void Push(Uint32 type) { SDL_Event e{}; e.type=type; assert(SDL_PushEvent(&e)); }
// Lifecycle effect adapters: only event delivery and paused/destroyed state,
// no audio, EGL, input/controller or application behavior is exercised.
static void Android_OnPause() { Android_Paused=true; Push(SDL_EVENT_WILL_ENTER_BACKGROUND); Push(SDL_EVENT_DID_ENTER_BACKGROUND); }
static void Android_OnResume() { Android_Paused=false; Push(SDL_EVENT_WILL_ENTER_FOREGROUND); Push(SDL_EVENT_DID_ENTER_FOREGROUND); }
static void Android_OnLowMemory() { Push(SDL_EVENT_LOW_MEMORY); }
static void Android_OnDestroy() { Android_Destroyed=true; Push(SDL_EVENT_QUIT); Push(SDL_EVENT_TERMINATING); }
static void Reset() {
    assert(SDL_main_callbacks_head == nullptr);
    SDL_Event ignored{};
    while (SDL_PeepEventsInternal(&ignored, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST, true) > 0) {}
    SDL_SetAtomicInt(&SDL_sentinel_pending, 0);
    { std::lock_guard lock(lifecycle_mutex); Android_NumLifecycleEvents=0; }
    { std::lock_guard lock(lifecycle_sem.mutex); lifecycle_sem.count=0; }
    Android_Paused=false; Android_Destroyed=false; sentinel_enabled=true;
    pump_count=0; wake_count=0; blocking_waits=0;
}
static void Delay(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static void WaitUntilBlocked() {
    const auto end=SDL_GetTicksNS()+SDL_MS_TO_NS(200);
    while (blocking_waits==0 && SDL_GetTicksNS()<end) std::this_thread::yield();
    assert(blocking_waits>0);
}
int main() {
    main_thread=std::this_thread::get_id();
    SDL_Event event{};
    Reset();
    auto start=SDL_GetTicksNS();
    assert(!SDL_WaitEventTimeout(&event, 50));
    const auto elapsed=(SDL_GetTicksNS()-start)/1000000;
    std::printf("idle50ms pumps=%d blocking_waits=%d elapsed_ms=%llu\n",pump_count.load(),blocking_waits.load(),(unsigned long long)elapsed);
#ifdef TEST_BASELINE
    assert(pump_count>100 && blocking_waits==0);
    std::puts("baseline self-wake reproduced");
    return 0;
#else
    assert(pump_count<=3 && blocking_waits>0 && elapsed>=45);
    Reset();
    std::thread producer([] { WaitUntilBlocked(); Push(SDL_EVENT_USER); });
    assert(SDL_WaitEvent(&event) && event.type==SDL_EVENT_USER);
    producer.join(); assert(pump_count<=5); std::puts("external event wakes indefinite wait: PASS");
    Reset();
    std::atomic<bool> callback_done{};
    std::thread callback([&] {
        WaitUntilBlocked();
        assert(SDL_RunOnMainThread([](void* data) {
            assert(SDL_IsMainThread()); *static_cast<std::atomic<bool>*>(data)=true;
            Push(SDL_EVENT_USER+1);
        }, &callback_done, true));
    });
    assert(SDL_WaitEvent(&event) && event.type==SDL_EVENT_USER+1);
    callback.join(); assert(callback_done); std::puts("main callback wait-complete wake: PASS");
    Reset();
    Push(SDL_EVENT_USER+2); Push(SDL_EVENT_USER+3);
    assert(SDL_PollEvent(&event) && event.type==SDL_EVENT_USER+2);
    Push(SDL_EVENT_USER+4);
    assert(SDL_PollEvent(&event) && event.type==SDL_EVENT_USER+3);
    assert(!SDL_PollEvent(&event));
    assert(SDL_PollEvent(&event) && event.type==SDL_EVENT_USER+4);
    assert(!SDL_PollEvent(&event)); std::puts("poll sentinel batch boundary: PASS");
    Reset();
    std::thread lifecycle([] {
        WaitUntilBlocked(); Android_SendLifecycleEvent(SDL_ANDROID_LIFECYCLE_PAUSE);
        Delay(20); Android_SendLifecycleEvent(SDL_ANDROID_LIFECYCLE_RESUME);
    });
    assert(SDL_WaitEventTimeout(&event,200) && event.type==SDL_EVENT_WILL_ENTER_BACKGROUND);
    lifecycle.join(); assert(!Android_Paused);
    assert(SDL_WaitEvent(&event) && event.type==SDL_EVENT_DID_ENTER_BACKGROUND);
    assert(SDL_WaitEvent(&event) && event.type==SDL_EVENT_WILL_ENTER_FOREGROUND);
    assert(SDL_WaitEvent(&event) && event.type==SDL_EVENT_DID_ENTER_FOREGROUND);
    std::puts("pause/resume order and unblock: PASS");
    Reset();
    std::thread lowmem([] { WaitUntilBlocked(); Android_SendLifecycleEvent(SDL_ANDROID_LIFECYCLE_LOWMEMORY); });
    assert(SDL_WaitEvent(&event) && event.type==SDL_EVENT_LOW_MEMORY); lowmem.join();
    std::puts("low-memory lifecycle wakes wait: PASS");
    Reset();
    std::thread destroy([] { WaitUntilBlocked(); Android_SendLifecycleEvent(SDL_ANDROID_LIFECYCLE_DESTROY); });
    assert(SDL_WaitEvent(&event) && event.type==SDL_EVENT_QUIT); destroy.join();
    assert(SDL_WaitEvent(&event) && event.type==SDL_EVENT_TERMINATING);
    std::puts("destroy lifecycle wake and events: PASS");
    Reset(); sentinel_enabled=false;
    assert(!SDL_WaitEventTimeout(&event,20)); assert(pump_count<=3 && blocking_waits>0);
    std::puts("sentinel disabled idle wait: PASS");
    std::puts("all proposed production-body fixture cases: PASS");
#endif
}
