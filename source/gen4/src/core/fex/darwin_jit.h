// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cerrno>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#if TARGET_OS_IPHONE
#include "LumenDarwinJitWrite.h"
#endif
#endif

namespace Core::Fex {

// Test the current process, not its signing profile or installation channel.
// This probe deliberately precedes translator allocation and signal setup, and
// can be retried after the system grants JIT to an already running process.
inline int ProbeDarwinJit([[maybe_unused]] bool allowPreparedDebuggerJit = false) noexcept {
#if defined(__APPLE__) && defined(__aarch64__)
    constexpr std::size_t pageSize = 16384;
    if (sysconf(_SC_PAGESIZE) != pageSize) return ENOTSUP;
#if TARGET_OS_IPHONE
    // iOS exposes only the callback-based write API. Its presence does not
    // imply an executable-memory grant for the current process.
    if (!__builtin_available(iOS 17.4, *)) return ENOTSUP;
    if (!allowPreparedDebuggerJit) {
        const char* debuggerMode = std::getenv("LUMEN_IOS_JIT_DEBUGGER");
        if (debuggerMode) {
            if (std::strcmp(debuggerMode, "1") != 0) return EACCES;
            if (const int error = FEXCore::Allocator::InitializeDebuggerJit(); error != 0) return error;
        }
    }
    const bool debuggerJit = FEXCore::Allocator::IsDebuggerJit();
    if (debuggerJit && !FEXCore::Allocator::IsDebuggerJitReady() && !allowPreparedDebuggerJit)
        return EAGAIN;
#else
    if (!pthread_jit_write_protect_supported_np()) return ENOTSUP;
#endif
    void* mapping =
#if TARGET_OS_IPHONE
        debuggerJit ? FEXCore::Allocator::AllocateDebuggerJit(pageSize) :
#endif
        mmap(nullptr, pageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    if (!mapping || mapping == MAP_FAILED) return errno != 0 ? errno : EACCES;
#if !TARGET_OS_IPHONE
    pthread_jit_write_protect_np(0);
#endif
    // mov w0, #42; ret. mmap's executable grant must precede execution.
    constexpr std::uint32_t instructions[] {0x52800540, 0xd65f03c0};
#if TARGET_OS_IPHONE
    FEXCore::Allocator::WithJITWriteProtection([&] {
        std::memcpy(FEXCore::Allocator::JitWritableAddress(mapping), instructions, sizeof(instructions));
    });
#else
    std::memcpy(mapping, instructions, sizeof(instructions));
#endif
#if TARGET_OS_IPHONE
    if (debuggerJit) sys_dcache_flush(FEXCore::Allocator::JitWritableAddress(mapping), sizeof(instructions));
#endif
    sys_icache_invalidate(mapping, sizeof(instructions));
#if !TARGET_OS_IPHONE
    pthread_jit_write_protect_np(1);
#endif
    const auto result = reinterpret_cast<unsigned (*)()>(mapping)();
#if TARGET_OS_IPHONE
    // FEX also patches already published code. Qualify a second write/execute
    // cycle through the same allocator and writer used by the translator.
    constexpr std::uint32_t rewritten = 0x52800560; // mov w0, #43
    FEXCore::Allocator::WithJITWriteProtection([&] {
        std::memcpy(FEXCore::Allocator::JitWritableAddress(mapping), &rewritten, sizeof(rewritten));
    });
    if (debuggerJit) sys_dcache_flush(FEXCore::Allocator::JitWritableAddress(mapping), sizeof(instructions));
    sys_icache_invalidate(mapping, sizeof(instructions));
    const auto patchedResult = reinterpret_cast<unsigned (*)()>(mapping)();
    const int unmapResult = debuggerJit ? (FEXCore::Allocator::FreeDebuggerJit(mapping, pageSize), 0)
                                       : munmap(mapping, pageSize);
    if (result != 42 || patchedResult != 43) return EIO;
#else
    const int unmapResult = munmap(mapping, pageSize);
    if (result != 42) return EIO;
#endif
    return unmapResult == 0 ? 0 : errno;
#else
    return ENOTSUP;
#endif
}

inline const char* DarwinJitFailureDescription(int error) noexcept {
    if (error == 0) return "FEX JIT is available in this process";
    if (error == ENOTSUP)
        return "FEX requires ARM64, 16 KiB pages and a supported JIT write mode";
    if (error == EACCES || error == EPERM)
        return "iOS has not granted executable JIT memory to this process";
    if (error == EAGAIN)
        return "The JIT provider has not completed preparation and detachment";
    return "The FEX executable memory probe failed";
}

} // namespace Core::Fex
