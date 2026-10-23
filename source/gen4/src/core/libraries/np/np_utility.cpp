// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/logging/log.h"
#include "core/libraries/libs.h"
#include "core/libraries/np/np_utility.h"
namespace Libraries::Np::NpUtility {
s32 PS4_SYSV_ABI sceNpBandwidthTestGetStatus() {
    LOG_ERROR(Lib_NpCommon, "(STUBBED) called");
    return 0;
}

s32 PS4_SYSV_ABI sceNpBandwidthTestInitStart() {
    LOG_ERROR(Lib_NpCommon, "(STUBBED) called");
    return 0;
}

s32 PS4_SYSV_ABI sceNpBandwidthTestShutdown() {
    LOG_ERROR(Lib_NpCommon, "(STUBBED) called");
    return 0;
}

s32 PS4_SYSV_ABI sceNpWordFilterAbortRequest() {
    LOG_ERROR(Lib_NpCommon, "(STUBBED) called");
    return 0;
}

s32 PS4_SYSV_ABI sceNpWordFilterCensorComment() {
    LOG_ERROR(Lib_NpCommon, "(STUBBED) called");
    return 0;
}

s32 PS4_SYSV_ABI sceNpWordFilterCreateAsyncRequest() {
    LOG_ERROR(Lib_NpCommon, "(STUBBED) called");
    return 0;
}

s32 PS4_SYSV_ABI sceNpWordFilterCreateRequest() {
    LOG_ERROR(Lib_NpCommon, "(STUBBED) called");
    return 0;
}

s32 PS4_SYSV_ABI sceNpWordFilterCreateTitleCtx() {
    LOG_ERROR(Lib_NpCommon, "(STUBBED) called");
    return 0;
}

s32 PS4_SYSV_ABI sceNpWordFilterDeleteRequest() {
    LOG_ERROR(Lib_NpCommon, "(STUBBED) called");
    return 0;
}

s32 PS4_SYSV_ABI sceNpWordFilterDeleteTitleCtx() {
    LOG_ERROR(Lib_NpCommon, "(STUBBED) called");
    return 0;
}

s32 PS4_SYSV_ABI sceNpWordFilterPollAsync() {
    LOG_ERROR(Lib_NpCommon, "(STUBBED) called");
    return 0;
}

s32 PS4_SYSV_ABI sceNpWordFilterSanitizeComment() {
    LOG_ERROR(Lib_NpCommon, "(STUBBED) called");
    return 0;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("BYIZGKm6bO4", "libSceNpUtility", 1, "libSceNpUtility", sceNpBandwidthTestGetStatus);
    LIB_FUNCTION("jktww3yJXnc", "libSceNpUtility", 1, "libSceNpUtility", sceNpBandwidthTestInitStart);
    LIB_FUNCTION("pLr1fEQS1z8", "libSceNpUtility", 1, "libSceNpUtility", sceNpBandwidthTestShutdown);
    LIB_FUNCTION("rAOOqDAxBIk", "libSceNpUtility", 1, "libSceNpUtility", sceNpWordFilterAbortRequest);
    LIB_FUNCTION("1dMndqL-QgE", "libSceNpUtility", 1, "libSceNpUtility", sceNpWordFilterCensorComment);
    LIB_FUNCTION("IEB+vgVoQbw", "libSceNpUtility", 1, "libSceNpUtility", sceNpWordFilterCreateAsyncRequest);
    LIB_FUNCTION("iCq5xW5KQW4", "libSceNpUtility", 1, "libSceNpUtility", sceNpWordFilterCreateRequest);
    LIB_FUNCTION("r9BgI0PfJZg", "libSceNpUtility", 1, "libSceNpUtility", sceNpWordFilterCreateTitleCtx);
    LIB_FUNCTION("PYFS1H70bDs", "libSceNpUtility", 1, "libSceNpUtility", sceNpWordFilterDeleteRequest);
    LIB_FUNCTION("t0P5z5yuFPA", "libSceNpUtility", 1, "libSceNpUtility", sceNpWordFilterDeleteTitleCtx);
    LIB_FUNCTION("ur5SShyG0dk", "libSceNpUtility", 1, "libSceNpUtility", sceNpWordFilterPollAsync);
    LIB_FUNCTION("Jj4mkpFO2gE", "libSceNpUtility", 1, "libSceNpUtility", sceNpWordFilterSanitizeComment);
}
} // namespace Libraries::Np::NpUtility
