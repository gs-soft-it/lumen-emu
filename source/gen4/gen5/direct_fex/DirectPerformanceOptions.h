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
};

// Read before native runtime initialization. The Android direct features
// are approved defaults; an explicit 0 restores the previous path individually.
// The shared filesystem case cache, release batching and uniform metadata are ON.
// Barycentric fallback is ON for every title; the renderer uses it only when
// native barycentrics are unavailable and the required GPU features exist.
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
    return result;
}

inline const Options& Get() {
    static const Options options = ReadOptions([](const char* name) -> const char* {
        return std::getenv(name);
    });
    return options;
}

} // namespace Lumen::Ps5::DirectPerformance
