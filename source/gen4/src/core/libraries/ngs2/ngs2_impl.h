// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include <cstddef>

namespace Libraries::Ngs2 {

static const int ORBIS_NGS2_SYSTEM_NAME_LENGTH = 16;
static const int ORBIS_NGS2_RACK_NAME_LENGTH = 16;

typedef uintptr_t OrbisNgs2Handle;

struct OrbisNgs2ContextBufferInfo {
    void* hostBuffer;
    size_t hostBufferSize;
    uintptr_t reserved[5];
    uintptr_t userData;
};

struct OrbisNgs2SystemOption {
    size_t size;
    char name[ORBIS_NGS2_SYSTEM_NAME_LENGTH];

    u32 flags;
    u32 maxGrainSamples;
    u32 numGrainSamples;
    u32 sampleRate;
    u32 aReserved[6];
};

using OrbisNgs2BufferAllocHandler =
    s32 PS4_SYSV_ABI (*)(OrbisNgs2ContextBufferInfo* io_buffer_info);
using OrbisNgs2BufferFreeHandler = s32 PS4_SYSV_ABI (*)(OrbisNgs2ContextBufferInfo* io_buffer_info);

struct OrbisNgs2SystemInfo {
    char name[ORBIS_NGS2_SYSTEM_NAME_LENGTH]; // 0

    OrbisNgs2Handle systemHandle;          // 16
    OrbisNgs2ContextBufferInfo bufferInfo; // 24

    u32 uid;             // 88
    u32 minGrainSamples; // 92
    u32 maxGrainSamples; // 96

    u32 stateFlags;        // 100
    u32 rackCount;         // 104
    float lastRenderRatio; // 108
    s64 lastRenderTick;    // 112
    s64 renderCount;       // 120
    u32 sampleRate;        // 128
    u32 numGrainSamples;   // 132
};

struct OrbisNgs2RackInfo {
    char name[ORBIS_NGS2_RACK_NAME_LENGTH]; // 0

    OrbisNgs2Handle rackHandle;            // 16
    OrbisNgs2ContextBufferInfo bufferInfo; // 24

    OrbisNgs2Handle ownerSystemHandle; // 88

    u32 type;            // 96
    u32 rackId;          // 100
    u32 uid;             // 104
    u32 minGrainSamples; // 108
    u32 maxGrainSamples; // 112
    u32 maxVoices;       // 116
    u32 maxChannelWorks; // 120
    u32 maxInputs;       // 124
    u32 maxMatrices;     // 128
    u32 maxPorts;        // 132

    u32 stateFlags;             // 136
    float lastProcessRatio;     // 140
    u64 lastProcessTick;        // 144
    u64 renderCount;            // 152
    u32 activeVoiceCount;       // 160
    u32 activeChannelWorkCount; // 164
};

} // namespace Libraries::Ngs2
