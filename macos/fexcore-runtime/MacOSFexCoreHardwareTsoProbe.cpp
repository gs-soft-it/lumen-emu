// Lumen, 2026. SPDX-License-Identifier: GPL-2.0-only
// Standalone SDK/API probe. This does not change any FEX configuration or
// qualify hardware TSO merely because the kernel capability query succeeds.
// Build with: xcrun clang++ -std=c++20 -O2 -pthread <this file> -o <probe>
#include <mach/mach_traps.h>
#include <mach/mach_error.h>
#include <os/arch/arm64.h>
#include <pthread.h>

#include <array>
#include <cstdint>
#include <cstdio>

namespace {
struct ThreadResult {
    int creation {};
    int join {};
    int identity {};
    std::uint64_t threadId {};
    kern_return_t resetBefore {KERN_FAILURE};
    kern_return_t enable {KERN_FAILURE};
    kern_return_t resetAfter {KERN_FAILURE};
};

void* ProbeThread(void* opaque) {
    auto& result = *static_cast<ThreadResult*>(opaque);
    result.identity = pthread_threadid_np(nullptr, &result.threadId);
    // A fresh disposable pthread has no guest work, memory borrows, or FEX
    // context. Reset before and after the request rather than leave a changed
    // compatibility mode on a thread that could execute ordinary application
    // code. A rejected request is evidence of refusal, not a software fallback.
    result.resetBefore = thread_set_x86_64_compat(0);
    result.enable = thread_set_x86_64_compat(1);
    result.resetAfter = thread_set_x86_64_compat(0);
    return nullptr;
}
} // namespace

int main() {
#if defined(__APPLE__) && defined(__arm64__)
    bool queryAvailable = false;
    bool kernelSupported = false;
    if (__builtin_available(macOS 26.6, *)) {
        queryAvailable = true;
        kernelSupported = os_cross_arch_is_supported(OS_CROSS_ARCH_X86_64);
    }
    std::array<ThreadResult, 2> results {};
    bool completed = true;
    bool permitted = queryAvailable && kernelSupported;
    for (auto& result : results) {
        pthread_t thread {};
        result.creation = pthread_create(&thread, nullptr, ProbeThread, &result);
        if (result.creation == 0) result.join = pthread_join(thread, nullptr);
        completed &= result.creation == 0 && result.join == 0 && result.identity == 0;
        permitted &= result.resetBefore == KERN_SUCCESS &&
            result.enable == KERN_SUCCESS && result.resetAfter == KERN_SUCCESS;
    }
    std::printf("{\"schema\":\"lumen-macos-hardware-tso-api-probe-v1\","
                "\"kernel_query_available\":%s,\"kernel_supported\":%s,"
                "\"thread_requests_permitted\":%s,\"fex_hardware_tso_qualified\":false,"
                "\"threads\":[",
                queryAvailable ? "true" : "false", kernelSupported ? "true" : "false",
                permitted ? "true" : "false");
    for (std::size_t index = 0; index != results.size(); ++index) {
        const auto& result = results[index];
        std::printf("%s{\"thread_id\":%llu,\"create\":%d,\"join\":%d,"
                    "\"identity\":%d,\"reset_before\":%d,\"enable\":%d,"
                    "\"reset_after\":%d}", index == 0 ? "" : ",",
                    static_cast<unsigned long long>(result.threadId),
                    result.creation, result.join, result.identity,
                    result.resetBefore, result.enable, result.resetAfter);
        if (result.creation == 0 && result.enable != KERN_SUCCESS)
            std::fprintf(stderr, "TSO request refused on thread %llu: %s (%d)\n",
                         static_cast<unsigned long long>(result.threadId),
                         mach_error_string(result.enable), result.enable);
    }
    std::printf("]}\n");
    return completed ? 0 : 1;
#else
    std::fprintf(stderr, "This probe requires a macOS ARM64 host and SDK.\n");
    return 1;
#endif
}
