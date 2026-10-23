// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"
namespace Core::Loader { class SymbolsResolver; }
namespace Libraries::Np::NpUtility {
// These compatibility entry points only log and return zero. They do not
// allocate requests, write output buffers, filter text or measure bandwidth.
s32 PS4_SYSV_ABI sceNpBandwidthTestGetStatus();
s32 PS4_SYSV_ABI sceNpBandwidthTestInitStart();
s32 PS4_SYSV_ABI sceNpBandwidthTestShutdown();
s32 PS4_SYSV_ABI sceNpWordFilterAbortRequest();
s32 PS4_SYSV_ABI sceNpWordFilterCensorComment();
s32 PS4_SYSV_ABI sceNpWordFilterCreateAsyncRequest();
s32 PS4_SYSV_ABI sceNpWordFilterCreateRequest();
s32 PS4_SYSV_ABI sceNpWordFilterCreateTitleCtx();
s32 PS4_SYSV_ABI sceNpWordFilterDeleteRequest();
s32 PS4_SYSV_ABI sceNpWordFilterDeleteTitleCtx();
s32 PS4_SYSV_ABI sceNpWordFilterPollAsync();
s32 PS4_SYSV_ABI sceNpWordFilterSanitizeComment();
void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::Np::NpUtility
