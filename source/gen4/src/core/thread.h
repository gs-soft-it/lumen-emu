// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Libraries::Kernel {
struct PthreadAttr;
} // namespace Libraries::Kernel

namespace Core {
#ifdef WIN32
using ThreadFunc = DWORD (*)(void*);
#else
using ThreadFunc = void* (*)(void*);
#endif

class NativeThread {
public:
    NativeThread();
    ~NativeThread();

    int Create(ThreadFunc func, void* arg);
    void Exit();
    // Prepare while the owner is alive; terminate without touching it after
    // its completion notification has allowed other threads to collect it.
    bool PrepareExit();
    [[noreturn]] static void ExitCurrentThread();

    void Initialize();

    uintptr_t GetHandle() {
        return reinterpret_cast<uintptr_t>(native_handle);
    }

    u64 GetTid() {
        return tid;
    }

private:
#ifdef _WIN64
    void* native_handle;
#else
    uintptr_t native_handle;
    void* sig_stack_ptr = nullptr;
#endif
    u64 tid;
};

} // namespace Core
