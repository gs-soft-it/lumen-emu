// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cctype>
#include <cstddef>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/guest_callback_scratch.h"
#endif
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/kernel/threads.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#include "core/libraries/np/np_common.h"
#include "core/libraries/np/np_error.h"
#include "core/libraries/np/np_guest_callback.h"
#include "core/libraries/np/np_signaling/np_signaling_state.h"
#include "core/libraries/np/np_types.h"

namespace Libraries::Np::NpCommon {

namespace {
static_assert(sizeof(OrbisNpCalloutEntry) == 0x20);
static_assert(offsetof(OrbisNpCalloutContext, thread) == 0x00);
static_assert(offsetof(OrbisNpCalloutContext, mutex) == 0x08);
static_assert(offsetof(OrbisNpCalloutContext, cond) == 0x10);
static_assert(offsetof(OrbisNpCalloutContext, active) == 0x18);
static_assert(offsetof(OrbisNpCalloutContext, stop_requested) == 0x1c);
static_assert(offsetof(OrbisNpCalloutContext, head) == 0x20);
static_assert(sizeof(OrbisNpCalloutContext) == 0x28);

s32 NormalizeNpCommonResult(int rc) {
    if (rc <= 0) {
        return rc;
    }
    return Libraries::Kernel::ErrnoToSceKernelError(rc);
}

u32 NormalizeNpTryLockResult(int rc) {
    const s32 sce_rc = NormalizeNpCommonResult(rc);
    if (sce_rc == ORBIS_KERNEL_ERROR_EBUSY) {
        return ORBIS_NP_LW_MUTEX_ERROR_BUSY;
    }
    return static_cast<u32>(sce_rc);
}

#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
struct GuestCalloutState {
    OrbisNpCalloutContext* context{};
    Libraries::Kernel::PthreadT thread{};
    Libraries::Kernel::PthreadMutexT mutex{};
    Libraries::Kernel::PthreadCondT cond{};
    std::mutex wake_mutex;
    std::condition_variable wake;
    u64 generation{};
    bool ready{};
    bool stop{};
    bool finished{};

    ~GuestCalloutState() {
        if (cond) sceNpCondDestroy(&cond);
        if (mutex) sceNpMutexDestroy(&mutex);
    }
};

std::mutex g_guest_callouts_mutex;
std::unordered_map<OrbisNpCalloutContext*, std::shared_ptr<GuestCalloutState>> g_guest_callouts;

bool IsGuestCalloutContext(const OrbisNpCalloutContext* context) {
    return context != &NpSignaling::g_callout_ctx;
}

class CalloutAccess {
public:
    explicit CalloutAccess(OrbisNpCalloutContext* context) : guest{IsGuestCalloutContext(context)} {
        if (guest) publication.emplace();
        valid = !guest || (publication->Valid() &&
            Core::GuestCpu::IsMappedCallbackMemory(context, sizeof(*context), true));
    }
    bool Valid() const { return valid; }
    bool EntryValid(const OrbisNpCalloutEntry* entry) const {
        return entry != nullptr && (!guest ||
            Core::GuestCpu::IsMappedCallbackMemory(entry, sizeof(*entry), true));
    }
    bool PointerValid(const void* pointer, size_t size) const {
        return !guest || Core::GuestCpu::IsMappedCallbackMemory(pointer, size, true);
    }
    void Publish(const void* pointer, size_t size) {
        if (guest) publication->Publish(reinterpret_cast<VAddr>(pointer), size);
    }
private:
    std::optional<Core::Fex::MemoryMutationScope> publication;
    bool guest{}, valid{};
};

bool ValidateCalloutEntries(const CalloutAccess& access, OrbisNpCalloutEntry* head) {
    std::unordered_set<const OrbisNpCalloutEntry*> visited;
    for (auto* entry = head; entry != nullptr; entry = entry->next) {
        if (!access.EntryValid(entry) || !visited.insert(entry).second) return false;
    }
    return true;
}

std::shared_ptr<GuestCalloutState> FindGuestCallout(OrbisNpCalloutContext* context) {
    std::lock_guard lock(g_guest_callouts_mutex);
    const auto found = g_guest_callouts.find(context);
    return found == g_guest_callouts.end() ? nullptr : found->second;
}

bool GuestCalloutReady(OrbisNpCalloutContext* context) {
    if (!IsGuestCalloutContext(context)) return true;
    const auto state = FindGuestCallout(context);
    if (!state || context->thread != state->thread || context->mutex != state->mutex ||
        context->cond != state->cond) return false;
    std::lock_guard lock(state->wake_mutex);
    return state->ready && !state->stop && !state->finished;
}

void NotifyGuestCallout(OrbisNpCalloutContext* context) {
    if (auto state = FindGuestCallout(context)) {
        {
            std::lock_guard lock(state->wake_mutex);
            ++state->generation;
        }
        state->wake.notify_all();
    }
}

void RemoveGuestCallout(const std::shared_ptr<GuestCalloutState>& state) {
    std::lock_guard lock(g_guest_callouts_mutex);
    const auto found = g_guest_callouts.find(state->context);
    if (found != g_guest_callouts.end() && found->second == state) g_guest_callouts.erase(found);
}

void* PS4_SYSV_ABI GuestNpCalloutThreadMain(void* argument) {
    std::unique_ptr<std::shared_ptr<GuestCalloutState>> owner{
        static_cast<std::shared_ptr<GuestCalloutState>*>(argument)};
    const auto state = *owner;
    {
        std::unique_lock lock(state->wake_mutex);
        state->wake.wait(lock, [&] { return state->ready || state->stop; });
    }
    for (;;) {
        u64 generation{};
        {
            std::lock_guard lock(state->wake_mutex);
            if (state->stop) break;
            generation = state->generation;
        }
        OrbisNpCalloutHandler handler{};
        u64 handler_arg{};
        u64 wait_usec{};
        bool dispatch{};
        {
            // Acquire admission before the kernel mutex. The worker keeps only
            // owned synchronization state while sleeping or entering guest code.
            CalloutAccess access{state->context};
            if (!access.Valid()) break;
            {
                std::lock_guard lock(state->wake_mutex);
                if (state->stop) break;
            }
            sceNpMutexLock(&state->mutex);
            auto* ctx = state->context;
            if (!ctx->active || ctx->stop_requested || ctx->mutex != state->mutex ||
                ctx->thread != state->thread || ctx->cond != state->cond) {
                sceNpMutexUnlock(&state->mutex);
                break;
            }
            auto* entry = ctx->head;
            if (entry != nullptr) {
                if (!access.EntryValid(entry)) {
                    sceNpMutexUnlock(&state->mutex);
                    break;
                }
                s64 now_usec{};
                sceNpGetSystemClockUsec(&now_usec);
                const auto now = static_cast<u64>(now_usec);
                const auto deadline = static_cast<u64>(entry->deadline_usec);
                if (deadline <= now) {
                    ctx->head = entry->next;
                    access.Publish(ctx, sizeof(*ctx));
                    handler = entry->handler;
                    handler_arg = entry->arg;
                    dispatch = true;
                } else {
                    wait_usec = std::min<u64>(deadline - now, 0xffffffffULL);
                }
            }
            sceNpMutexUnlock(&state->mutex);
        }
        if (dispatch) {
            InvokeNpCallback(handler, "NP callout", handler_arg);
            continue;
        }
        std::unique_lock lock(state->wake_mutex);
        const auto changed = [&] { return state->stop || state->generation != generation; };
        if (wait_usec == 0) state->wake.wait(lock, changed);
        else state->wake.wait_for(lock, std::chrono::microseconds(wait_usec), changed);
    }
    {
        std::lock_guard lock(state->wake_mutex);
        state->finished = true;
    }
    return nullptr;
}

s32 InitializeGuestCallout(OrbisNpCalloutContext* context, const char* name,
                          u64 stack_size, s32 priority, u64 affinity_mask) {
    {
        CalloutAccess access{context};
        if (!access.Valid()) return ORBIS_KERNEL_ERROR_EFAULT;
        if (context->active > 0 || FindGuestCallout(context))
            return ORBIS_NP_CALLOUT_ERROR_ALREADY_INITIALIZED;
    }
    auto state = std::make_shared<GuestCalloutState>();
    state->context = context;
    s32 result = static_cast<s32>(sceNpMutexInit(&state->mutex, name, 1));
    if (result < 0) return result;
    result = sceNpCondInit(&state->cond, name, 0);
    if (result < 0) return result;
    auto* argument = new std::shared_ptr<GuestCalloutState>(state);
    result = sceNpCreateThread(&state->thread, GuestNpCalloutThreadMain, argument,
                              priority, stack_size, affinity_mask, name);
    if (result < 0) {
        delete argument;
        return result;
    }
    {
        CalloutAccess access{context};
        if (!access.Valid()) {
            result = ORBIS_KERNEL_ERROR_EFAULT;
        } else {
            std::lock_guard registry_lock(g_guest_callouts_mutex);
            if (!g_guest_callouts.emplace(context, state).second) {
                result = ORBIS_NP_CALLOUT_ERROR_ALREADY_INITIALIZED;
            } else {
                *context = {state->thread, state->mutex, state->cond, 1, 0, nullptr};
                access.Publish(context, sizeof(*context));
            }
        }
    }
    {
        std::lock_guard lock(state->wake_mutex);
        state->ready = true;
        state->stop = result < 0;
    }
    state->wake.notify_all();
    if (result < 0) sceNpJoinThread(state->thread, nullptr);
    return result;
}

void TerminateGuestCallout(OrbisNpCalloutContext* context) {
    const auto state = FindGuestCallout(context);
    if (!state) return;
    {
        std::lock_guard lock(state->wake_mutex);
        if (state->stop) return;
        state->stop = true;
        ++state->generation;
    }
    state->wake.notify_all();
    // posix_pthread_join donates HLE admission while waiting. A callback may
    // terminate its own context; its shared state then survives until return.
    if (state->thread == Libraries::Kernel::g_curthread) {
        Libraries::Kernel::posix_pthread_detach(state->thread);
    } else {
        sceNpJoinThread(state->thread, nullptr);
    }
    {
        CalloutAccess access{context};
        if (access.Valid() && context->thread == state->thread) {
            *context = {};
            access.Publish(context, sizeof(*context));
        }
    }
    RemoveGuestCallout(state);
}
#endif

void* PS4_SYSV_ABI NpCalloutThreadMain(void* arg) {
    auto* ctx = static_cast<OrbisNpCalloutContext*>(arg);
    const auto mutex = &ctx->mutex;
    sceNpMutexLock(mutex);

    for (;;) {
        s64 now_usec = 0;
        sceNpGetSystemClockUsec(&now_usec);

        auto* entry = ctx->head;
        u32 wait_usec = 0;

        if (entry == nullptr) {
            if (ctx->stop_requested > 0) {
                break;
            }
        } else {
            const u64 deadline = static_cast<u64>(entry->deadline_usec);
            const u64 now = static_cast<u64>(now_usec);
            const u64 remaining = deadline - now;
            if (deadline < now || remaining == 0) {
                ctx->head = entry->next;
                const auto handler = entry->handler;
                const auto handler_arg = entry->arg;
                sceNpMutexUnlock(mutex);
                if (!NpSignaling::DispatchInternalCallout(ctx, handler, handler_arg)) {
                    InvokeNpCallback(handler, "NP callout", handler_arg);
                }
                sceNpMutexLock(mutex);
                continue;
            }

            if (ctx->stop_requested > 0) {
                break;
            }

            wait_usec = static_cast<u32>(remaining > 0xffffffffULL ? 0xffffffffULL : remaining);
        }

        sceNpCondTimedwait(&ctx->cond, mutex, wait_usec);
        if (ctx->stop_requested > 0) {
            break;
        }
    }

    sceNpMutexUnlock(mutex);
    return nullptr;
}
} // namespace

s32 PS4_SYSV_ABI sceNpCmpNpId(OrbisNpId* np_id1, OrbisNpId* np_id2) {
    if (np_id1 == nullptr || np_id2 == nullptr) {
        return ORBIS_NP_ERROR_INVALID_ARGUMENT;
    }

    if (std::strncmp(np_id1->handle.data, np_id2->handle.data, ORBIS_NP_ONLINEID_MAX_LENGTH) != 0) {
        return ORBIS_NP_UTIL_ERROR_NOT_MATCH;
    }

    for (u32 i = 0; i < 8; i++) {
        if (np_id1->opt[i] != np_id2->opt[i]) {
            return ORBIS_NP_UTIL_ERROR_NOT_MATCH;
        }
    }

    for (u32 i = 0; i < 8; i++) {
        if (np_id1->reserved[i] != np_id2->reserved[i]) {
            return ORBIS_NP_UTIL_ERROR_NOT_MATCH;
        }
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpCmpNpIdInOrder(OrbisNpId* np_id1, OrbisNpId* np_id2, u32* out_result) {
    if (np_id1 == nullptr || np_id2 == nullptr || out_result == nullptr) {
        return ORBIS_NP_ERROR_INVALID_ARGUMENT;
    }

    u32 compare =
        std::strncmp(np_id1->handle.data, np_id2->handle.data, ORBIS_NP_ONLINEID_MAX_LENGTH);
    if (compare < 0) {
        *out_result = -1;
        return ORBIS_OK;
    } else if (compare > 0) {
        *out_result = 1;
        return ORBIS_OK;
    }

    for (u32 i = 0; i < 8; i++) {
        if (np_id1->opt[i] < np_id2->opt[i]) {
            *out_result = -1;
            return ORBIS_OK;
        } else if (np_id1->opt[i] > np_id2->opt[i]) {
            *out_result = 1;
            return ORBIS_OK;
        }
    }

    for (u32 i = 0; i < 8; i++) {
        if (np_id1->reserved[i] < np_id2->reserved[i]) {
            *out_result = -1;
            return ORBIS_OK;
        } else if (np_id1->reserved[i] > np_id2->reserved[i]) {
            *out_result = 1;
            return ORBIS_OK;
        }
    }

    *out_result = 0;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpCmpOnlineId(OrbisNpOnlineId* online_id1, OrbisNpOnlineId* online_id2) {
    if (online_id1 == nullptr || online_id2 == nullptr) {
        return ORBIS_NP_ERROR_INVALID_ARGUMENT;
    }

    if (std::strncmp(online_id1->data, online_id2->data, ORBIS_NP_ONLINEID_MAX_LENGTH) != 0) {
        return ORBIS_NP_UTIL_ERROR_NOT_MATCH;
    }
    return ORBIS_OK;
}

u32 PS4_SYSV_ABI sceNpMutexLock(Libraries::Kernel::PthreadMutexT* mutex) {
    LOG_TRACE(Lib_NpCommon, "mutex={:p}", fmt::ptr(mutex));
    return static_cast<u32>(
        NormalizeNpCommonResult(Libraries::Kernel::posix_pthread_mutex_lock(mutex)));
}

u32 PS4_SYSV_ABI sceNpMutexUnlock(Libraries::Kernel::PthreadMutexT* mutex) {
    LOG_TRACE(Lib_NpCommon, "mutex={:p}", fmt::ptr(mutex));
    return static_cast<u32>(
        NormalizeNpCommonResult(Libraries::Kernel::posix_pthread_mutex_unlock(mutex)));
}

u32 PS4_SYSV_ABI sceNpMutexInit(Libraries::Kernel::PthreadMutexT* mutex, const char* name,
                                u64 flags) {
    LOG_DEBUG(Lib_NpCommon, "mutex={:p} name={:p} flags={:#x}", fmt::ptr(mutex), fmt::ptr(name),
              flags);

    Libraries::Kernel::PthreadMutexAttrT attr = nullptr;
    int rc = Libraries::Kernel::posix_pthread_mutexattr_init(&attr);
    if (rc == 0) {
        if ((flags & 1) != 0) {
            rc = Libraries::Kernel::posix_pthread_mutexattr_settype(
                &attr, Libraries::Kernel::PthreadMutexType::Recursive);
        }
        if (rc == 0) {
            rc = Libraries::Kernel::scePthreadMutexInit(mutex, &attr, name);
        }
        Libraries::Kernel::posix_pthread_mutexattr_destroy(&attr);
    }
    return static_cast<u32>(NormalizeNpCommonResult(rc));
}

s32 PS4_SYSV_ABI sceNpMutexDestroy(Libraries::Kernel::PthreadMutexT* mutex) {
    LOG_DEBUG(Lib_NpCommon, "mutex={:p}", fmt::ptr(mutex));
    return NormalizeNpCommonResult(Libraries::Kernel::posix_pthread_mutex_destroy(mutex));
}

u32 PS4_SYSV_ABI sceNpMutexTryLock(Libraries::Kernel::PthreadMutexT* mutex) {
    LOG_TRACE(Lib_NpCommon, "mutex={:p}", fmt::ptr(mutex));
    return NormalizeNpTryLockResult(Libraries::Kernel::posix_pthread_mutex_trylock(mutex));
}

u32 PS4_SYSV_ABI sceNpLwMutexLock(Libraries::Kernel::PthreadMutexT* mutex) {
    LOG_TRACE(Lib_NpCommon, "mutex={:p}", fmt::ptr(mutex));
    return sceNpMutexLock(mutex);
}

u32 PS4_SYSV_ABI sceNpLwMutexUnlock(Libraries::Kernel::PthreadMutexT* mutex) {
    LOG_TRACE(Lib_NpCommon, "mutex={:p}", fmt::ptr(mutex));
    return sceNpMutexUnlock(mutex);
}

u32 PS4_SYSV_ABI sceNpLwMutexInit(Libraries::Kernel::PthreadMutexT* mutex, const char* name,
                                  u64 flags) {
    LOG_DEBUG(Lib_NpCommon, "mutex={:p} name={:p} flags={:#x}", fmt::ptr(mutex), fmt::ptr(name),
              flags);
    return sceNpMutexInit(mutex, name, flags);
}

s32 PS4_SYSV_ABI sceNpLwMutexDestroy(Libraries::Kernel::PthreadMutexT* mutex) {
    LOG_DEBUG(Lib_NpCommon, "mutex={:p}", fmt::ptr(mutex));
    return sceNpMutexDestroy(mutex);
}

u32 PS4_SYSV_ABI sceNpLwMutexTryLock(Libraries::Kernel::PthreadMutexT* mutex) {
    LOG_TRACE(Lib_NpCommon, "mutex={:p}", fmt::ptr(mutex));
    return sceNpMutexTryLock(mutex);
}

s32 PS4_SYSV_ABI sceNpCondInit(Libraries::Kernel::PthreadCondT* cond, const char* name, u64 flags) {
    LOG_DEBUG(Lib_NpCommon, "cond={:p} name={:p} flags={:#x}", fmt::ptr(cond), fmt::ptr(name),
              flags);

    Libraries::Kernel::PthreadCondAttrT attr = nullptr;
    int rc = Libraries::Kernel::posix_pthread_condattr_init(&attr);
    if (rc == 0) {
        rc = Libraries::Kernel::scePthreadCondInit(cond, &attr, name);
        Libraries::Kernel::posix_pthread_condattr_destroy(&attr);
    }
    return NormalizeNpCommonResult(rc);
}

s32 PS4_SYSV_ABI sceNpCondDestroy(Libraries::Kernel::PthreadCondT* cond) {
    LOG_DEBUG(Lib_NpCommon, "cond={:p}", fmt::ptr(cond));
    return NormalizeNpCommonResult(Libraries::Kernel::posix_pthread_cond_destroy(cond));
}

s32 PS4_SYSV_ABI sceNpCondSignal(Libraries::Kernel::PthreadCondT* cond) {
    LOG_DEBUG(Lib_NpCommon, "cond={:p}", fmt::ptr(cond));
    return NormalizeNpCommonResult(Libraries::Kernel::posix_pthread_cond_signal(cond));
}

s32 PS4_SYSV_ABI sceNpCondTimedwait(Libraries::Kernel::PthreadCondT* cond,
                                    Libraries::Kernel::PthreadMutexT* mutex, u32 usec) {
    LOG_DEBUG(Lib_NpCommon, "cond={:p} mutex={:p} usec={:#x}", fmt::ptr(cond), fmt::ptr(mutex),
              usec);
    if (usec == 0) {
        return NormalizeNpCommonResult(Libraries::Kernel::posix_pthread_cond_wait(cond, mutex));
    }
    const s32 rc = NormalizeNpCommonResult(
        Libraries::Kernel::posix_pthread_cond_reltimedwait_np(cond, mutex, usec));
    if (rc == ORBIS_KERNEL_ERROR_ETIMEDOUT) {
        return ORBIS_NP_LW_COND_ERROR_TIMEDOUT;
    }
    return rc;
}

s32 PS4_SYSV_ABI sceNpCreateThread(Libraries::Kernel::PthreadT* thread, NpThreadEntry start_routine,
                                   void* arg, s32 priority, u64 stack_size, u64 affinity_mask,
                                   const char* name) {
    LOG_DEBUG(Lib_NpCommon,
              "thread={:p} start={:#x} arg={:p} priority={} stack={:#x} "
              "affinity={:#x} name={:p}",
              fmt::ptr(thread), reinterpret_cast<u64>(start_routine), fmt::ptr(arg), priority,
              stack_size, affinity_mask, fmt::ptr(name));

    Libraries::Kernel::PthreadAttrT attr = nullptr;
    int rc = Libraries::Kernel::posix_pthread_attr_init(&attr);
    if (rc != 0) {
        return NormalizeNpCommonResult(rc);
    }

    rc = Libraries::Kernel::posix_pthread_attr_setstacksize(&attr, static_cast<size_t>(stack_size));
    if (rc == 0) {
        if (priority != 0) {
            s32 sdk_version = 0;
            rc = Libraries::Kernel::sceKernelGetCompiledSdkVersion(&sdk_version);
            if (rc >= 0 && sdk_version >= 0x2500000) {
                rc = Libraries::Kernel::posix_pthread_attr_setinheritsched(&attr, 0);
                if (rc == 0) {
                    rc = Libraries::Kernel::posix_pthread_attr_setschedpolicy(
                        &attr, Libraries::Kernel::SchedPolicy::Fifo);
                }
            }
            if (rc >= 0) {
                Libraries::Kernel::SchedParam param{.sched_priority = priority};
                rc = Libraries::Kernel::posix_pthread_attr_setschedparam(&attr, &param);
            }
        }
        if (rc == 0 && affinity_mask != 0) {
            rc = Libraries::Kernel::scePthreadAttrSetaffinity(&attr, affinity_mask);
        }
        if (rc == 0) {
            rc = Libraries::Kernel::posix_pthread_create_name_np(thread, &attr, start_routine, arg,
                                                                 name);
        }
    }

    Libraries::Kernel::posix_pthread_attr_destroy(&attr);
    return NormalizeNpCommonResult(rc);
}

u32 PS4_SYSV_ABI sceNpJoinThread(Libraries::Kernel::PthreadT thread, void** ret) {
    LOG_DEBUG(Lib_NpCommon, "thread={:p} ret={:p}", fmt::ptr(thread), fmt::ptr(ret));
    return static_cast<u32>(
        NormalizeNpCommonResult(Libraries::Kernel::posix_pthread_join(thread, ret)));
}

s32 PS4_SYSV_ABI sceNpGetSystemClockUsec(s64* usec) {
    LOG_DEBUG(Lib_NpCommon, "usec={:p}", fmt::ptr(usec));
    Libraries::Kernel::OrbisKernelTimespec ts{};
    const s32 rc =
        Libraries::Kernel::sceKernelClockGettime(Libraries::Kernel::ORBIS_CLOCK_MONOTONIC, &ts);
    if (rc == ORBIS_OK) {
        *usec = ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    }
    return rc;
}

s32 PS4_SYSV_ABI sceNpGetPlatformType(const OrbisNpId* npid) {
    LOG_DEBUG(Lib_NpCommon, "np_id={:p}", fmt::ptr(npid));

    if (npid == nullptr) {
        return ORBIS_NP_ERROR_INVALID_ARGUMENT;
    }

    static constexpr u32 kTagPs3 = 0x00337370;  // "ps3"
    static constexpr u32 kTagVita = 0x32707370; // "psp2(VITA)"
    static constexpr u32 kTagPs4 = 0x00347370;  // "ps4"

    if (npid->opt[4] == 0) {
        return OrbisNpPlatformType::None;
    }
    u32 tag = 0;
    std::memcpy(&tag, npid->opt + 4, sizeof(tag));
    switch (tag) {
    case kTagPs3:
        return OrbisNpPlatformType::PS3;
    case kTagVita:
        return OrbisNpPlatformType::Vita;
    case kTagPs4:
        return OrbisNpPlatformType::PS4;
    default:
        return ORBIS_NP_ERROR_INVALID_PLATFORM_TYPE;
    }
}

s32 PS4_SYSV_ABI sceNpIntIsValidOnlineId(const OrbisNpOnlineId* id) {
    LOG_DEBUG(Lib_NpCommon, "online_id={:p}", fmt::ptr(id));

    if (id == nullptr || id->term != 0) {
        return 0;
    }

    const size_t len = strnlen(id->data, ORBIS_NP_ONLINEID_MAX_LENGTH + 1);
    if (len < 3 || len > ORBIS_NP_ONLINEID_MAX_LENGTH) {
        return 0;
    }

    const unsigned char first = static_cast<unsigned char>(id->data[0]);
    if (!std::isalnum(first)) {
        return 0;
    }

    for (size_t i = 1; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(id->data[i]);
        if (std::isalnum(c) || c == '_' || c == '-') {
            continue;
        }
        return 0;
    }

    return 1;
}

s32 PS4_SYSV_ABI sceNpCalloutInitCtx(OrbisNpCalloutContext* callout_ctx, const char* name,
                                     u64 stack_size, s32 priority, u64 affinity_mask) {
    LOG_DEBUG(Lib_NpCommon, "ctx={:p} name={:p} stack={:#x} priority={} affinity={:#x}",
              fmt::ptr(callout_ctx), fmt::ptr(name), stack_size, priority, affinity_mask);

#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    if (IsGuestCalloutContext(callout_ctx))
        return InitializeGuestCallout(callout_ctx, name, stack_size, priority, affinity_mask);
#endif
    s32 rc = ORBIS_NP_CALLOUT_ERROR_ALREADY_INITIALIZED;
    if (callout_ctx->active < 1) {
        callout_ctx->stop_requested = 0;
        callout_ctx->head = nullptr;
        rc = static_cast<s32>(sceNpMutexInit(&callout_ctx->mutex, name, 1));
        if (rc >= 0) {
            rc = sceNpCondInit(&callout_ctx->cond, name, 0);
            if (rc >= 0) {
                rc = sceNpCreateThread(&callout_ctx->thread, NpCalloutThreadMain, callout_ctx,
                                       priority, stack_size, affinity_mask, name);
                if (rc >= 0) {
                    callout_ctx->active = 1;
                    return ORBIS_OK;
                }
                sceNpCondDestroy(&callout_ctx->cond);
            }
            sceNpMutexDestroy(&callout_ctx->mutex);
        }
    }
    return rc;
}

s32 PS4_SYSV_ABI sceNpCalloutStartOnCtx(OrbisNpCalloutContext* callout_ctx,
                                        OrbisNpCalloutEntry* entry, u64 delay_usec, u64 handler,
                                        u64 arg) {
    LOG_DEBUG(Lib_NpCommon, "ctx={:p} callout={:p} delay={:#x} handler={:p} arg={:#x}",
              fmt::ptr(callout_ctx), fmt::ptr(entry), delay_usec,
              fmt::ptr(reinterpret_cast<void*>(static_cast<uintptr_t>(handler))), arg);

#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    CalloutAccess access{callout_ctx};
    if (!access.Valid() || !access.EntryValid(entry)) return ORBIS_KERNEL_ERROR_EFAULT;
    if (!GuestCalloutReady(callout_ctx)) return ORBIS_NP_CALLOUT_ERROR_NOT_INITIALIZED;
#endif
    if (callout_ctx->active == 0) {
        return ORBIS_NP_CALLOUT_ERROR_NOT_INITIALIZED;
    }

    sceNpMutexLock(&callout_ctx->mutex);
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    if (!ValidateCalloutEntries(access, callout_ctx->head)) {
        sceNpMutexUnlock(&callout_ctx->mutex);
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
#endif

    for (auto* cur = callout_ctx->head; cur != nullptr; cur = cur->next) {
        if (cur == entry) {
            sceNpMutexUnlock(&callout_ctx->mutex);
            return ORBIS_NP_CALLOUT_ERROR_DUPLICATE_ENTRY;
        }
    }

    entry->next = nullptr;
    entry->handler = reinterpret_cast<OrbisNpCalloutHandler>(handler);
    entry->arg = arg;
    sceNpGetSystemClockUsec(&entry->deadline_usec);
    entry->deadline_usec += static_cast<s64>(static_cast<u32>(delay_usec));
    OrbisNpCalloutEntry** prev_next = &callout_ctx->head;
    for (auto* cur = callout_ctx->head; cur != nullptr; cur = cur->next) {
        if (cur->deadline_usec >= entry->deadline_usec) {
            break;
        }
        prev_next = &cur->next;
    }

    entry->next = *prev_next;
    *prev_next = entry;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    access.Publish(entry, sizeof(*entry));
    access.Publish(prev_next, sizeof(*prev_next));
#endif
    sceNpCondSignal(&callout_ctx->cond);
    sceNpMutexUnlock(&callout_ctx->mutex);
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    NotifyGuestCallout(callout_ctx);
#endif
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpCalloutStartOnCtx64(OrbisNpCalloutContext* callout_ctx,
                                          OrbisNpCalloutEntry* entry, s64 delay_usec, u64 handler,
                                          u64 arg) {
    LOG_DEBUG(Lib_NpCommon, "ctx={:p} callout={:p} delay={} handler={:p} arg={:#x}",
              fmt::ptr(callout_ctx), fmt::ptr(entry), delay_usec,
              fmt::ptr(reinterpret_cast<void*>(static_cast<uintptr_t>(handler))), arg);

#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    CalloutAccess access{callout_ctx};
    if (!access.Valid() || !access.EntryValid(entry)) return ORBIS_KERNEL_ERROR_EFAULT;
    if (!GuestCalloutReady(callout_ctx)) return ORBIS_NP_CALLOUT_ERROR_NOT_INITIALIZED;
#endif
    if (callout_ctx->active == 0) {
        return ORBIS_NP_CALLOUT_ERROR_NOT_INITIALIZED;
    }

    sceNpMutexLock(&callout_ctx->mutex);
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    if (!ValidateCalloutEntries(access, callout_ctx->head)) {
        sceNpMutexUnlock(&callout_ctx->mutex);
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
#endif

    for (auto* cur = callout_ctx->head; cur != nullptr; cur = cur->next) {
        if (cur == entry) {
            sceNpMutexUnlock(&callout_ctx->mutex);
            return ORBIS_NP_CALLOUT_ERROR_DUPLICATE_ENTRY;
        }
    }

    entry->next = nullptr;
    entry->handler = reinterpret_cast<OrbisNpCalloutHandler>(handler);
    entry->arg = arg;
    sceNpGetSystemClockUsec(&entry->deadline_usec);
    entry->deadline_usec += delay_usec;
    OrbisNpCalloutEntry** prev_next = &callout_ctx->head;
    for (auto* cur = callout_ctx->head; cur != nullptr; cur = cur->next) {
        if (cur->deadline_usec >= entry->deadline_usec) {
            break;
        }
        prev_next = &cur->next;
    }

    entry->next = *prev_next;
    *prev_next = entry;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    access.Publish(entry, sizeof(*entry));
    access.Publish(prev_next, sizeof(*prev_next));
#endif
    sceNpCondSignal(&callout_ctx->cond);
    sceNpMutexUnlock(&callout_ctx->mutex);
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    NotifyGuestCallout(callout_ctx);
#endif
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpCalloutStopOnCtx(OrbisNpCalloutContext* callout_ctx,
                                       OrbisNpCalloutEntry* entry, u32* removed) {
    LOG_DEBUG(Lib_NpCommon, "ctx={:p} callout={:p} removed={:p}", fmt::ptr(callout_ctx),
              fmt::ptr(entry), fmt::ptr(removed));

#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    CalloutAccess access{callout_ctx};
    if (!access.Valid() || (removed && !access.PointerValid(removed, sizeof(*removed))))
        return ORBIS_KERNEL_ERROR_EFAULT;
    if (!GuestCalloutReady(callout_ctx)) return ORBIS_NP_CALLOUT_ERROR_NOT_INITIALIZED;
#endif
    if (callout_ctx->active == 0) {
        return ORBIS_NP_CALLOUT_ERROR_NOT_INITIALIZED;
    }

    sceNpMutexLock(&callout_ctx->mutex);
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    if (!ValidateCalloutEntries(access, callout_ctx->head)) {
        sceNpMutexUnlock(&callout_ctx->mutex);
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
#endif
    u32 found = 0;
    OrbisNpCalloutEntry** prev_next = &callout_ctx->head;
    for (auto* cur = callout_ctx->head; cur != nullptr; cur = cur->next) {
        if (cur == entry) {
            *prev_next = cur->next;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
            access.Publish(prev_next, sizeof(*prev_next));
#endif
            found = 1;
            break;
        }
        prev_next = &cur->next;
    }
    if (removed != nullptr) {
        *removed = found;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
        access.Publish(removed, sizeof(*removed));
#endif
    }
    sceNpMutexUnlock(&callout_ctx->mutex);
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    NotifyGuestCallout(callout_ctx);
#endif
    return ORBIS_OK;
}

void PS4_SYSV_ABI sceNpCalloutTermCtx(OrbisNpCalloutContext* callout_ctx) {
    LOG_DEBUG(Lib_NpCommon, "ctx={:p}", fmt::ptr(callout_ctx));

#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    if (IsGuestCalloutContext(callout_ctx)) {
        TerminateGuestCallout(callout_ctx);
        return;
    }
#endif
    if (callout_ctx->active != 0) {
        sceNpMutexLock(&callout_ctx->mutex);
        callout_ctx->stop_requested = 1;
        sceNpCondSignal(&callout_ctx->cond);
        sceNpMutexUnlock(&callout_ctx->mutex);
        sceNpJoinThread(callout_ctx->thread, nullptr);
        sceNpCondDestroy(&callout_ctx->cond);
        sceNpMutexDestroy(&callout_ctx->mutex);
        callout_ctx->active = 0;
    }
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("i8UmXTSq7N4", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpCmpNpId);
    LIB_FUNCTION("TcwEFnakiSc", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpCmpNpIdInOrder);
    LIB_FUNCTION("dj+O5aD2a0Q", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpCmpOnlineId);
    LIB_FUNCTION("1a+iY5YUJcI", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpCondDestroy);
    LIB_FUNCTION("9+m5nRdJ-wQ", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpCalloutInitCtx);
    LIB_FUNCTION("AqJ4xkWsV+I", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpCalloutTermCtx);
    LIB_FUNCTION("18j+qk6dRwk", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpLwMutexLock);
    LIB_FUNCTION("1CiXI-MyEKs", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpLwMutexInit);
    LIB_FUNCTION("4zxevggtYrQ", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpLwMutexDestroy);
    LIB_FUNCTION("CQG2oyx1-nM", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpLwMutexUnlock);
    LIB_FUNCTION("DuslmoqQ+nk", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpMutexTryLock);
    LIB_FUNCTION("EjMsfO3GCIA", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpJoinThread);
    LIB_FUNCTION("fClnlkZmA6k", "libSceNpCommonCompat", 1, "libSceNpCommon",
                 sceNpCalloutStartOnCtx);
    LIB_FUNCTION("fhJ5uKzcn0w", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpCreateThread);
    LIB_FUNCTION("hkeX9iuCwlI", "libSceNpCommonCompat", 1, "libSceNpCommon",
                 sceNpIntIsValidOnlineId);
    LIB_FUNCTION("hp0kVgu5Fxw", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpLwMutexTryLock);
    LIB_FUNCTION("in19gH7G040", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpCalloutStopOnCtx);
    LIB_FUNCTION("lQ11BpMM4LU", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpMutexDestroy);
    LIB_FUNCTION("lpr66Gby8dQ", "libSceNpCommonCompat", 1, "libSceNpCommon",
                 sceNpCalloutStartOnCtx64);
    LIB_FUNCTION("oZyb9ktuCpA", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpMutexUnlock);
    LIB_FUNCTION("PVVsRmMkO1g", "libSceNpCommonCompat", 1, "libSceNpCommon",
                 sceNpGetSystemClockUsec);
    LIB_FUNCTION("q2tsVO3lM4A", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpCondInit);
    LIB_FUNCTION("r9Bet+s6fKc", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpMutexLock);
    LIB_FUNCTION("sXVQUIGmk2U", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpGetPlatformType);
    LIB_FUNCTION("ss2xO9IJxKQ", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpCondTimedwait);
    LIB_FUNCTION("uEwag-0YZPc", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpMutexInit);
    LIB_FUNCTION("uMJFOA62mVU", "libSceNpCommonCompat", 1, "libSceNpCommon", sceNpCondSignal);
    LIB_FUNCTION("i8UmXTSq7N4", "libSceNpCommon", 1, "libSceNpCommon", sceNpCmpNpId);
    LIB_FUNCTION("TcwEFnakiSc", "libSceNpCommon", 1, "libSceNpCommon", sceNpCmpNpIdInOrder);
    LIB_FUNCTION("dj+O5aD2a0Q", "libSceNpCommon", 1, "libSceNpCommon", sceNpCmpOnlineId);
    LIB_FUNCTION("1a+iY5YUJcI", "libSceNpCommon", 1, "libSceNpCommon", sceNpCondDestroy);
    LIB_FUNCTION("9+m5nRdJ-wQ", "libSceNpCommon", 1, "libSceNpCommon", sceNpCalloutInitCtx);
    LIB_FUNCTION("AqJ4xkWsV+I", "libSceNpCommon", 1, "libSceNpCommon", sceNpCalloutTermCtx);
    LIB_FUNCTION("18j+qk6dRwk", "libSceNpCommon", 1, "libSceNpCommon", sceNpLwMutexLock);
    LIB_FUNCTION("1CiXI-MyEKs", "libSceNpCommon", 1, "libSceNpCommon", sceNpLwMutexInit);
    LIB_FUNCTION("4zxevggtYrQ", "libSceNpCommon", 1, "libSceNpCommon", sceNpLwMutexDestroy);
    LIB_FUNCTION("CQG2oyx1-nM", "libSceNpCommon", 1, "libSceNpCommon", sceNpLwMutexUnlock);
    LIB_FUNCTION("DuslmoqQ+nk", "libSceNpCommon", 1, "libSceNpCommon", sceNpMutexTryLock);
    LIB_FUNCTION("EjMsfO3GCIA", "libSceNpCommon", 1, "libSceNpCommon", sceNpJoinThread);
    LIB_FUNCTION("fClnlkZmA6k", "libSceNpCommon", 1, "libSceNpCommon", sceNpCalloutStartOnCtx);
    LIB_FUNCTION("fhJ5uKzcn0w", "libSceNpCommon", 1, "libSceNpCommon", sceNpCreateThread);
    LIB_FUNCTION("hkeX9iuCwlI", "libSceNpCommon", 1, "libSceNpCommon", sceNpIntIsValidOnlineId);
    LIB_FUNCTION("hp0kVgu5Fxw", "libSceNpCommon", 1, "libSceNpCommon", sceNpLwMutexTryLock);
    LIB_FUNCTION("in19gH7G040", "libSceNpCommon", 1, "libSceNpCommon", sceNpCalloutStopOnCtx);
    LIB_FUNCTION("lQ11BpMM4LU", "libSceNpCommon", 1, "libSceNpCommon", sceNpMutexDestroy);
    LIB_FUNCTION("lpr66Gby8dQ", "libSceNpCommon", 1, "libSceNpCommon", sceNpCalloutStartOnCtx64);
    LIB_FUNCTION("oZyb9ktuCpA", "libSceNpCommon", 1, "libSceNpCommon", sceNpMutexUnlock);
    LIB_FUNCTION("PVVsRmMkO1g", "libSceNpCommon", 1, "libSceNpCommon", sceNpGetSystemClockUsec);
    LIB_FUNCTION("q2tsVO3lM4A", "libSceNpCommon", 1, "libSceNpCommon", sceNpCondInit);
    LIB_FUNCTION("r9Bet+s6fKc", "libSceNpCommon", 1, "libSceNpCommon", sceNpMutexLock);
    LIB_FUNCTION("sXVQUIGmk2U", "libSceNpCommon", 1, "libSceNpCommon", sceNpGetPlatformType);
    LIB_FUNCTION("ss2xO9IJxKQ", "libSceNpCommon", 1, "libSceNpCommon", sceNpCondTimedwait);
    LIB_FUNCTION("uEwag-0YZPc", "libSceNpCommon", 1, "libSceNpCommon", sceNpMutexInit);
    LIB_FUNCTION("uMJFOA62mVU", "libSceNpCommon", 1, "libSceNpCommon", sceNpCondSignal);
};

} // namespace Libraries::Np::NpCommon
