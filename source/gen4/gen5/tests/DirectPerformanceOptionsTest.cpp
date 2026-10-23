// SPDX-License-Identifier: GPL-2.0-only
#include "../direct_fex/DirectPerformanceOptions.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr std::array names {
    "LUMEN_PS5_FAST_SIGNAL_CHECK", "LUMEN_PS5_SRT_ARM64",
    "LUMEN_PS5_HLE_INDEXED_DISPATCH", "LUMEN_PS5_ASYNC_PIPELINE_CACHE",
    "LUMEN_PS5_BARYCENTRIC_FALLBACK", "LUMEN_PS5_FILESYSTEM_CASE_CACHE",
    "LUMEN_PS5_RELEASE_MEM_BATCH", "LUMEN_PS5_UNIFORM_METADATA"};
std::array<const char*, names.size()> values {};
const char* Lookup(const char* name) {
    for (std::size_t index = 0; index < names.size(); ++index)
        if (std::strcmp(name, names[index]) == 0) return values[index];
    std::abort();
}
void Check(bool condition) {
    if (!condition) std::abort();
}
}

int main(int argc, char** argv) {
    using namespace Lumen::Ps5::DirectPerformance;
    // Cover all absent/0/1 combinations, including each independent rollback.
    for (unsigned combination = 0; combination < 6561; ++combination) {
        unsigned digits = combination;
        std::array<bool, names.size()> expected {};
        for (unsigned index = 0; index < names.size(); ++index) {
            const unsigned selection = digits % 3;
            digits /= 3;
            values[index] = selection == 0 ? nullptr : selection == 1 ? "0" : "1";
            expected[index] = selection != 1;
        }
        const auto option = ReadOptions(Lookup);
        Check(option.valid && option.fast_signal_check == expected[0]
              && option.srt_arm64 == expected[1]
              && option.hle_indexed_dispatch == expected[2]
              && option.async_pipeline_cache == expected[3]
              && option.barycentric_fallback == expected[4]
              && option.filesystem_case_cache == expected[5]
              && option.release_mem_batch == expected[6]
              && option.uniform_metadata == expected[7]);
    }
    values.fill("0");
    const auto disabled = ReadOptions(Lookup);
    Check(disabled.valid && !disabled.fast_signal_check && !disabled.srt_arm64
          && !disabled.hle_indexed_dispatch && !disabled.async_pipeline_cache
          && !disabled.barycentric_fallback && !disabled.filesystem_case_cache
          && !disabled.release_mem_batch && !disabled.uniform_metadata);
    for (std::size_t index = 0; index < names.size(); ++index) {
        for (const char* invalid : {"", "true", "false", "01", " 1", "1 ", "2"}) {
            values[index] = invalid;
            Check(!ReadOptions(Lookup).valid);
        }
        values[index] = "0";
    }
    const bool initial_off = argc == 2 && std::strcmp(argv[1], "off") == 0;
    const bool fallback_on = argc == 2 && std::strcmp(argv[1], "fallback-on") == 0;
    const bool fallback_off = argc == 2 && std::strcmp(argv[1], "fallback-off") == 0;
    const bool filesystem_cache_on = argc == 2 && std::strcmp(argv[1], "filesystem-cache-on") == 0;
    const bool filesystem_cache_off = argc == 2 && std::strcmp(argv[1], "filesystem-cache-off") == 0;
    const bool batch_on = argc == 2 && std::strcmp(argv[1], "release-mem-batch-on") == 0;
    const bool uniform_metadata_on = argc == 2 && std::strcmp(argv[1], "uniform-metadata-on") == 0;
    const bool batch_off = argc == 2 && std::strcmp(argv[1], "release-mem-batch-off") == 0;
    const bool uniform_metadata_off = argc == 2 && std::strcmp(argv[1], "uniform-metadata-off") == 0;
    const bool expected_batch = !initial_off && !batch_off;
    const bool expected_uniform_metadata = !initial_off && !uniform_metadata_off;
    const bool expected_filesystem_cache = !initial_off && !filesystem_cache_off;
    const bool expected_fallback = !initial_off && !fallback_off;
    for (const char* name : names)
        Check((initial_off ? setenv(name, "0", 1) : unsetenv(name)) == 0);
    if (uniform_metadata_on) Check(setenv(names[7], "1", 1) == 0);
    if (batch_on) Check(setenv(names[6], "1", 1) == 0);
    if (batch_off) Check(setenv(names[6], "0", 1) == 0);
    if (uniform_metadata_off) Check(setenv(names[7], "0", 1) == 0);
    if (fallback_on) Check(setenv(names[4], "1", 1) == 0);
    if (fallback_off) Check(setenv(names[4], "0", 1) == 0);
    if (filesystem_cache_on) Check(setenv(names[5], "1", 1) == 0);
    if (filesystem_cache_off) Check(setenv(names[5], "0", 1) == 0);
    Check(Get().valid && Get().fast_signal_check == !initial_off && Get().srt_arm64 == !initial_off
          && Get().hle_indexed_dispatch == !initial_off && Get().async_pipeline_cache == !initial_off
          && Get().barycentric_fallback == expected_fallback
          && Get().filesystem_case_cache == expected_filesystem_cache
          && Get().release_mem_batch == expected_batch
          && Get().uniform_metadata == expected_uniform_metadata);
    for (const char* name : names) Check(setenv(name, initial_off ? "1" : "0", 1) == 0);
    Check(setenv(names[4], expected_fallback ? "0" : "1", 1) == 0);
    Check(setenv(names[5], expected_filesystem_cache ? "0" : "1", 1) == 0);
    Check(setenv(names[6], expected_batch ? "0" : "1", 1) == 0);
    Check(setenv(names[7], expected_uniform_metadata ? "0" : "1", 1) == 0);
    Check(Get().fast_signal_check == !initial_off && Get().srt_arm64 == !initial_off
          && Get().hle_indexed_dispatch == !initial_off && Get().async_pipeline_cache == !initial_off
          && Get().barycentric_fallback == expected_fallback
          && Get().filesystem_case_cache == expected_filesystem_cache
          && Get().release_mem_batch == expected_batch
          && Get().uniform_metadata == expected_uniform_metadata);
    std::puts("PASS: Android direct, barycentric fallback, filesystem, release batching and uniform metadata defaults ON, independent strict selectors latched once");
}
