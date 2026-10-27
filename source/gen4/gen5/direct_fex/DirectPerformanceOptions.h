// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdlib>
#include <cstring>

namespace Lumen::Ps5::DirectPerformance {

struct Options {
    bool valid = true;
    bool fast_signal_check = true;
    bool srt_arm64 = true;
    bool hle_indexed_dispatch = true;
    bool async_pipeline_cache = true;
    bool barycentric_fallback = true;
    bool filesystem_case_cache = true;
    bool release_mem_batch = true;
    bool uniform_metadata = true;
    bool vm_access_query = true;
    bool srt_exact_readback = false;
};

// Read before native runtime initialization. Approved direct features are ON;
// an explicit 0 restores the previous path individually.
// The shared filesystem case cache, release batching and uniform metadata are ON.
// Barycentric fallback is ON for every title; the renderer uses it only when
// native barycentrics are unavailable and the required GPU features exist.
// Allocation-free VM access validation is shared by every direct host/title.
// Exact ordinary SRT readback and its eligible checked ARM64 walker are
// experimental and OFF for every direct host/title; explicit 1 enables them.
// Malformed values deny startup instead of silently changing the selection.
inline Options ReadOptions(const char* (*lookup)(const char*)) {
    Options result;
    const auto read = [&](const char* name, bool& enabled) {
        const char* value = lookup(name);
        if (value == nullptr) return;
        if (std::strcmp(value, "0") == 0) enabled = false;
        else if (std::strcmp(value, "1") == 0) enabled = true;
        else result.valid = false;
    };
    read("LUMEN_PS5_FAST_SIGNAL_CHECK", result.fast_signal_check);
    read("LUMEN_PS5_SRT_ARM64", result.srt_arm64);
    read("LUMEN_PS5_HLE_INDEXED_DISPATCH", result.hle_indexed_dispatch);
    read("LUMEN_PS5_ASYNC_PIPELINE_CACHE", result.async_pipeline_cache);
    read("LUMEN_PS5_BARYCENTRIC_FALLBACK", result.barycentric_fallback);
    read("LUMEN_PS5_FILESYSTEM_CASE_CACHE", result.filesystem_case_cache);
    read("LUMEN_PS5_RELEASE_MEM_BATCH", result.release_mem_batch);
    read("LUMEN_PS5_UNIFORM_METADATA", result.uniform_metadata);
    read("LUMEN_PS5_VM_ACCESS_QUERY", result.vm_access_query);
    read("LUMEN_PS5_SRT_EXACT_READBACK", result.srt_exact_readback);
    return result;
}

// Keep the one-time environment parser out of the hot accessor so callers can
// inline its initialized guard and option load. Initialization and publication
// still belong to the function-local static in Get().
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
inline Options ReadEnvironmentOptions() noexcept {
    return ReadOptions([](const char* name) -> const char* {
        return std::getenv(name);
    });
}

inline const Options& Get() {
    static const Options options = ReadEnvironmentOptions();
    return options;
}

} // namespace Lumen::Ps5::DirectPerformance
