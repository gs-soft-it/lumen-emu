#ifdef LUMEN_GEN4_DIRECT_FEX
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/libraries/kernel/threads/pthread.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/guest_callback.h"
#endif
#include "core/libraries/libs.h"
#include "core/linker.h"

namespace Libraries::Kernel {

void PS4_SYSV_ABI __pthread_cleanup_push_imp(PthreadCleanupFunc routine, void* arg,
                                             PthreadCleanup* newbuf) {
    newbuf->routine = routine;
    newbuf->routine_arg = arg;
    newbuf->onheap = 0;
    g_curthread->cleanup.push_front(newbuf);
}

void PS4_SYSV_ABI posix_pthread_cleanup_push(PthreadCleanupFunc routine, void* arg) {
    Pthread* curthread = g_curthread;
    auto* newbuf = new (std::nothrow) PthreadCleanup{};
    if (newbuf == nullptr) {
        return;
    }

    newbuf->routine = routine;
    newbuf->routine_arg = arg;
    newbuf->onheap = 1;
    curthread->cleanup.push_front(newbuf);
}

void PS4_SYSV_ABI posix_pthread_cleanup_pop(int execute) {
    Pthread* curthread = g_curthread;
    if (!curthread->cleanup.empty()) {
        PthreadCleanup* old = curthread->cleanup.front();
        curthread->cleanup.pop_front();
        if (execute) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
            if (Core::GuestCpu::IsGuestFunctionAddress(
                    reinterpret_cast<const void*>(old->routine))) {
                Core::GuestCpu::RunGuestFunctionOrAbort(
                    reinterpret_cast<void*>(old->routine), "pthread cleanup pop",
                    old->routine_arg);
            } else {
                old->routine(old->routine_arg);
            }
#else
            old->routine(old->routine_arg);
#endif
        }
        if (old->onheap) {
            delete old;
        }
    }
}

void RegisterPthreadClean(Core::Loader::SymbolsResolver* sym) {
    // Posix
    LIB_FUNCTION("4ZeZWcMsAV0", "libScePosix", 1, "libkernel", posix_pthread_cleanup_push);
    LIB_FUNCTION("RVxb0Ssa5t0", "libScePosix", 1, "libkernel", posix_pthread_cleanup_pop);

    // Posix-Kernel
    LIB_FUNCTION("1xvtUVx1-Sg", "libkernel", 1, "libkernel", __pthread_cleanup_push_imp);
    LIB_FUNCTION("iWsFlYMf3Kw", "libkernel", 1, "libkernel", posix_pthread_cleanup_pop);
}

} // namespace Libraries::Kernel

#else
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/libraries/kernel/threads/pthread.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/guest_callback.h"
#endif
#include "core/libraries/libs.h"
#include "core/linker.h"

namespace Libraries::Kernel {

Core::GuestCpu::ThreadCallbackRegistry& PthreadCallbacks() {
    static Core::GuestCpu::ThreadCallbackRegistry callbacks;
    return callbacks;
}

void RunPthreadCleanup(const Core::GuestCpu::ThreadCallback& entry) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    Core::GuestCpu::DispatchCallback(entry.origin,
        [&] { Core::GuestCpu::RunGuestFunctionOrAbort(reinterpret_cast<void*>(entry.function),
                                                     "pthread cleanup", entry.argument); },
        [&] { reinterpret_cast<PthreadCleanupFunc>(entry.function)(reinterpret_cast<void*>(entry.argument)); });
#else
    reinterpret_cast<PthreadCleanupFunc>(entry.function)(reinterpret_cast<void*>(entry.argument));
#endif
}

void PS4_SYSV_ABI __pthread_cleanup_push_imp(PthreadCleanupFunc routine, void* arg,
                                             PthreadCleanup* newbuf) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    {
        Core::Fex::MemoryMutationScope publication;
        ASSERT(publication.Valid() && Core::GuestCpu::ValidateHlePointer(newbuf, sizeof(*newbuf), true));
        *newbuf = {routine, arg, 0};
        publication.Publish(reinterpret_cast<u64>(newbuf), sizeof(*newbuf));
    }
#else
    *newbuf = {routine, arg, 0};
#endif
    // Capture all fields now. Pop/exit must never dereference a guest stack
    // record after waits/callbacks may have changed or unmapped that stack.
    PthreadCallbacks().PushCleanup(reinterpret_cast<uintptr_t>(g_curthread),
        {reinterpret_cast<uintptr_t>(routine), reinterpret_cast<uintptr_t>(arg), Core::GuestCpu::CallbackOrigin::Guest});
}

void PS4_SYSV_ABI posix_pthread_cleanup_push(PthreadCleanupFunc routine, void* arg) {
    PthreadCallbacks().PushCleanup(reinterpret_cast<uintptr_t>(g_curthread),
        {reinterpret_cast<uintptr_t>(routine), reinterpret_cast<uintptr_t>(arg), Core::GuestCpu::CallbackOrigin::Guest});
}

void PS4_SYSV_ABI posix_pthread_cleanup_pop(int execute) {
    const auto entry = PthreadCallbacks().PopCleanup(reinterpret_cast<uintptr_t>(g_curthread));
    if (entry && execute) RunPthreadCleanup(*entry);
}

void RegisterPthreadClean(Core::Loader::SymbolsResolver* sym) {
    // Posix
    LIB_FUNCTION("4ZeZWcMsAV0", "libScePosix", 1, "libkernel", posix_pthread_cleanup_push);
    LIB_FUNCTION("RVxb0Ssa5t0", "libScePosix", 1, "libkernel", posix_pthread_cleanup_pop);

    // Posix-Kernel
    LIB_FUNCTION("1xvtUVx1-Sg", "libkernel", 1, "libkernel", __pthread_cleanup_push_imp);
    LIB_FUNCTION("iWsFlYMf3Kw", "libkernel", 1, "libkernel", posix_pthread_cleanup_pop);
}

} // namespace Libraries::Kernel

#endif
