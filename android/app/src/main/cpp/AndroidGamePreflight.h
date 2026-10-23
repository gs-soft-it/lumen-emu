// Lumen PS5 integration modifications, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>

enum LumenAndroidGamePreflightCodeV1 : uint32_t
{
    LUMEN_ANDROID_GAME_PREFLIGHT_OK_V1 = 0,
    LUMEN_ANDROID_GAME_PREFLIGHT_INVALID_ARGUMENT_V1 = 1,
    LUMEN_ANDROID_GAME_PREFLIGHT_REQUIRED_FILE_MISSING_V1 = 2,
    LUMEN_ANDROID_GAME_PREFLIGHT_INVALID_PARAM_SFO_V1 = 3,
    LUMEN_ANDROID_GAME_PREFLIGHT_PS5_REQUIRES_SEPARATE_RUNTIME_V1 = 4,
};

struct LumenAndroidGamePreflightStatusV1
{
    uint32_t abiVersion;
    uint32_t requiredFileCount;
    uint64_t requiredFileBytes;
    char titleId[16];
    char appVersion[16];
    char contentId[64];
};

extern "C" uint32_t LumenAndroidGamePreflightV1(
    const char* gameDirectory,
    LumenAndroidGamePreflightStatusV1* status);
