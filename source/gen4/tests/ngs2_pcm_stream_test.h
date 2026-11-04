// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>
#include "core/libraries/ngs2/ngs2.h"
#include "core/libraries/ngs2/ngs2_mastering.h"
#include "core/libraries/ngs2/ngs2_sampler.h"

// Include after the test harness defines Check(bool, const char*).
namespace Ngs2PcmStreamTestDetail {
using namespace Libraries::Ngs2;

struct Fixture {
    std::vector<u64> system_memory, rack_memory;
    OrbisNgs2Handle system{}, rack{};
    std::array<OrbisNgs2Handle, 2> voices{};

    Fixture() {
        OrbisNgs2SystemOption option{};
        Check(sceNgs2SystemResetOption(&option) == 0, "PCM stream default options");
        option.numGrainSamples = 256;
        option.sampleRate = 48000;
        OrbisNgs2ContextBufferInfo system_info{};
        Check(sceNgs2SystemQueryBufferSize(&option, &system_info) == 0, "PCM stream system query");
        system_memory.resize((system_info.hostBufferSize + 7) / 8);
        system_info.hostBuffer = system_memory.data();
        Check(sceNgs2SystemCreate(&option, &system_info, &system) == 0, "PCM stream system create");

        OrbisNgs2SamplerRackOption rack_option{};
        rack_option.rackOption.size = sizeof(rack_option);
        rack_option.rackOption.maxVoices = static_cast<u32>(voices.size());
        rack_option.rackOption.maxPorts = 1;
        rack_option.maxWaveformBlocks = 8;
        OrbisNgs2ContextBufferInfo rack_info{};
        Check(sceNgs2RackQueryBufferSize(0x1000, &rack_option.rackOption, &rack_info) == 0,
              "PCM stream rack query");
        rack_memory.resize((rack_info.hostBufferSize + 7) / 8);
        rack_info.hostBuffer = rack_memory.data();
        Check(sceNgs2RackCreate(system, 0x1000, &rack_option.rackOption, &rack_info, &rack) == 0,
              "PCM stream rack create");
        for (u32 index = 0; index < voices.size(); ++index) {
            Check(sceNgs2RackGetVoiceHandle(rack, index, &voices[index]) == 0,
                  "PCM stream voice handle");
            OrbisNgs2SamplerVoiceSetupParam setup{
                {sizeof(setup), 0, 0x10000000}, {0x12, 2, 44100, 0, 0, 0}, 0, 0};
            Check(sceNgs2VoiceControl(voices[index], &setup.header) == 0,
                  "public PCM stereo 44100 setup");
        }
    }

    ~Fixture() {
        if (system) (void)sceNgs2SystemDestroy(system, nullptr);
    }
};

// Compute each output position independently in source integer-sample units.
// The oracle does not use the runtime's cursor, phase, PCM conversion, or mixer.
static s16 ReferenceSample(const std::vector<s16>& source, size_t frame, size_t channel) {
    const u64 numerator = static_cast<u64>(frame) * 44100;
    const size_t frames = source.size() / 2;
    if (numerator >= static_cast<u64>(frames) * 48000) return 0;
    const size_t first = numerator / 48000;
    const size_t second = std::min(first + 1, frames - 1);
    const long double weight = static_cast<long double>(numerator % 48000) / 48000;
    const long double value = source[first * 2 + channel] * (1 - weight) +
                              source[second * 2 + channel] * weight;
    return static_cast<s16>(std::clamp(std::lround(value), -32768L, 32767L));
}
} // namespace Ngs2PcmStreamTestDetail

static void PcmStreamTest() {
    using namespace Libraries::Ngs2;
    using namespace Ngs2PcmStreamTestDetail;
    Fixture fixture;
    constexpr u32 source_frames = 588, prefix_frames = 7, frame_bytes = 2 * sizeof(s16);
    std::vector<s16> source(source_frames * 2);
    for (u32 frame = 0; frame < source_frames; ++frame) {
        source[frame * 2] = static_cast<s16>(static_cast<s32>((frame * 137) % 24001) - 12000);
        source[frame * 2 + 1] = static_cast<s16>(11000 - static_cast<s32>((frame * 193) % 22001));
    }
    std::vector<s16> guest((prefix_frames + source_frames) * 2, 30000);
    std::copy(source.begin(), source.end(), guest.begin() + prefix_frames * 2);

    OrbisNgs2WaveformBlock first{
        0, (prefix_frames + 147) * frame_bytes, 0, prefix_frames, 147, 0, 0x11};
    OrbisNgs2SamplerVoiceWaveformBlocksParam packet{
        {sizeof(packet), 0, 0x10000001}, guest.data(), 1, 1, &first};
    Check(sceNgs2VoiceControl(fixture.voices[0], &packet.header) == 0,
          "public PCM first block accepts explicit samples and skip");

    // The first descriptor is valid; the second is readable but asks for more
    // samples than its dataSize contains. No part of this packet may commit.
    std::array<OrbisNgs2WaveformBlock, 2> blocks{{
        {(prefix_frames + 147) * frame_bytes, 147 * frame_bytes, 0, 0, 147, 0, 0x22},
        {(prefix_frames + 294) * frame_bytes, frame_bytes, 0, 0, 2, 0, 0x33}}};
    packet.numBlocks = static_cast<u32>(blocks.size());
    packet.aBlock = blocks.data();
    Check(sceNgs2VoiceControl(fixture.voices[0], &packet.header) != 0,
          "malformed PCM tail rejects the complete public packet");
    blocks[1].dataSize = 294 * frame_bytes;
    blocks[1].numSamples = 294;
    Check(sceNgs2VoiceControl(fixture.voices[0], &packet.header) == 0,
          "public PCM packet appends two blocks in descriptor order");

    // Accepted data belongs to the runtime even if the caller reuses every
    // source byte before its first render.
    std::fill(guest.begin(), guest.end(), -30000);
    OrbisNgs2VoiceEventParam play{{sizeof(play), 0, 6}, 0};
    Check(sceNgs2VoiceControl(fixture.voices[0], &play.header) == 0, "public PCM Play");
    struct Output {
        u64 before{0x123456789abcdef0ull};
        std::array<s16, 256 * 2> samples{};
        u64 after{0x0fedcba987654321ull};
    } output;
    OrbisNgs2RenderBufferInfo buffer{output.samples.data(), sizeof(output.samples), 0x12, 2};
    // 588 source frames produce 640 output frames. Three grains also verify
    // that the remaining 128 frames are cleared after the queued data ends.
    for (size_t grain = 0; grain < 3; ++grain) {
        output.samples.fill(12345);
        Check(sceNgs2SystemRender(fixture.system, &buffer, 1) == 0, "public PCM stream render");
        Check(output.before == 0x123456789abcdef0ull && output.after == 0x0fedcba987654321ull,
              "public PCM render preserves output canaries");
        for (size_t frame = 0; frame < 256; ++frame) {
            for (size_t channel = 0; channel < 2; ++channel) {
                const s32 expected = ReferenceSample(source, grain * 256 + frame, channel);
                const s32 actual = output.samples[frame * 2 + channel];
                Check(std::abs(actual - expected) <= 1,
                      "public PCM stream preserves skip, order, rollback, snapshot and fractional rate");
            }
        }
    }
    u32 flags = ~0u;
    Check(sceNgs2VoiceGetStateFlags(fixture.voices[1], &flags) == 0 && flags == 0,
          "public PCM stream leaves the other voice idle");
    Check(sceNgs2SystemDestroy(fixture.system, nullptr) == 0, "public PCM stream teardown");
    fixture.system = 0;
}

namespace Ngs2PcmStreamTestDetail {
enum class CallbackAction { Refill, Kill, Setup, Destroy };

struct CallbackContext {
    Fixture* fixture{};
    CallbackAction action{};
    std::vector<OrbisNgs2VoiceCallbackInfo> received;
    std::vector<s16> refill;
    s32 result{-1};
};

static void PS4_SYSV_ABI CompletePcmBlock(const OrbisNgs2VoiceCallbackInfo* info) {
    auto& context = *reinterpret_cast<CallbackContext*>(info->callbackData);
    context.received.push_back(*info);
    if (context.received.size() != 1) return;
    auto& fixture = *context.fixture;
    switch (context.action) {
    case CallbackAction::Refill: {
        OrbisNgs2WaveformBlock block{
            0, static_cast<u32>(context.refill.size() * sizeof(s16)), 0, 0,
            static_cast<u32>(context.refill.size() / 2), 0, 0x44};
        OrbisNgs2SamplerVoiceWaveformBlocksParam packet{
            {sizeof(packet), 0, 0x10000001}, context.refill.data(), 0x13, 1, &block};
        context.result = sceNgs2VoiceControl(fixture.voices[0], &packet.header);
        std::fill(context.refill.begin(), context.refill.end(), -30000);
        break;
    }
    case CallbackAction::Kill: {
        OrbisNgs2VoiceEventParam event{{sizeof(event), 0, 6}, 3};
        context.result = sceNgs2VoiceControl(fixture.voices[0], &event.header);
        break;
    }
    case CallbackAction::Setup: {
        OrbisNgs2SamplerVoiceSetupParam setup{
            {sizeof(setup), 0, 0x10000000}, {0x12, 2, 44100, 0, 0, 0}, 0, 0};
        context.result = sceNgs2VoiceControl(fixture.voices[0], &setup.header);
        break;
    }
    case CallbackAction::Destroy:
        context.result = sceNgs2SystemDestroy(fixture.system, nullptr);
        if (context.result == 0) fixture.system = 0;
        break;
    }
}

static void RegisterCallback(CallbackContext& context) {
    OrbisNgs2VoiceCallbackParam callback{
        {sizeof(callback), 0, 7}, CompletePcmBlock,
        reinterpret_cast<uintptr_t>(&context), 1, 0};
    Check(sceNgs2VoiceControl(context.fixture->voices[0], &callback.header) == 0,
          "public PCM completion callback registration");
}
} // namespace Ngs2PcmStreamTestDetail

static void PcmCallbackStreamTest() {
    using namespace Libraries::Ngs2;
    using namespace Ngs2PcmStreamTestDetail;
    // This public API test registers a native callback from native code. The
    // FEX bridge must preserve that explicit registration provenance; this does
    // not qualify translated callback execution or guest scratch publication.
    {
        Fixture fixture;
        constexpr u32 block_frames = 448, block_bytes = block_frames * 2 * sizeof(s16);
        std::vector<s16> source(block_frames * 4 * 2);
        for (size_t frame = 0; frame < source.size() / 2; ++frame) {
            source[frame * 2] = static_cast<s16>(static_cast<s32>((frame * 83) % 22001) - 11000);
            source[frame * 2 + 1] = static_cast<s16>(10000 - static_cast<s32>((frame * 127) % 20001));
        }
        std::vector<s16> initial((block_frames + 2) * 2, 30000);
        std::copy_n(source.begin(), block_frames * 2, initial.begin() + 4);
        std::vector<s16> second(source.begin() + block_frames * 2, source.begin() + block_frames * 4);
        std::vector<s16> third(source.begin() + block_frames * 4, source.begin() + block_frames * 6);
        CallbackContext context{&fixture, CallbackAction::Refill};
        context.refill.assign(source.begin() + block_frames * 6, source.end());
        RegisterCallback(context);
        const std::array<const void*, 4> addresses{
            initial.data() + 4, second.data(), third.data(), context.refill.data()};

        OrbisNgs2WaveformBlock block{4 * sizeof(s16), block_bytes, 0, 0, UINT32_MAX, 0, 0x41};
        OrbisNgs2SamplerVoiceWaveformBlocksParam packet{
            {sizeof(packet), 0, 0x10000001}, initial.data(), 1, 1, &block};
        Check(sceNgs2VoiceControl(fixture.voices[0], &packet.header) == 0,
              "PCM sentinel uses physically available source frames");
        block.dataOffset = 0;
        block.numSamples = block_frames;
        packet.flags = 0x13;
        packet.data = second.data();
        block.userData = 0x42;
        Check(sceNgs2VoiceControl(fixture.voices[0], &packet.header) == 0,
              "PCM raw refill accepts physical stereo frames");
        packet.data = third.data();
        block.userData = 0x43;
        Check(sceNgs2VoiceControl(fixture.voices[0], &packet.header) == 0,
              "PCM second raw refill queues independently");
        std::fill(initial.begin(), initial.end(), -30000);
        std::fill(second.begin(), second.end(), -30000);
        std::fill(third.begin(), third.end(), -30000);
        OrbisNgs2VoiceEventParam play{{sizeof(play), 0, 6}, 0};
        Check(sceNgs2VoiceControl(fixture.voices[0], &play.header) == 0, "PCM callback stream Play");
        std::array<s16, 256 * 2> output{};
        OrbisNgs2RenderBufferInfo buffer{output.data(), sizeof(output), 0x12, 2};
        for (size_t grain = 0; grain < 9; ++grain) {
            output.fill(12345);
            Check(sceNgs2SystemRender(fixture.system, &buffer, 1) == 0,
                  "PCM callback stream renders with reentrant refill");
            for (size_t frame = 0; frame < 256; ++frame) {
                for (size_t channel = 0; channel < 2; ++channel) {
                    const s32 expected = ReferenceSample(source, grain * 256 + frame, channel);
                    Check(std::abs(static_cast<s32>(output[frame * 2 + channel]) - expected) <= 1,
                          "callback refill preserves owned PCM and continuous fractional position");
                }
            }
        }
        Check(context.result == 0 && context.received.size() == 4,
              "each PCM block completes once and callback can reenter VoiceControl");
        for (size_t index = 0; index < context.received.size(); ++index) {
            const auto& info = context.received[index];
            const auto& waveform = info.param.waveformBlock;
            Check(info.callbackData == reinterpret_cast<uintptr_t>(&context) &&
                      info.voiceHandle == fixture.voices[0] && info.flag == 1 && info.reserved == 0 &&
                      waveform.userData == 0x41 + index && waveform.data == addresses[index] &&
                      waveform.dataSize == block_bytes && waveform.repeatedCount == 0 &&
                      waveform.attributeFlags == 0 && waveform.reserved2 == 0,
                  "PCM callback returns exact completed block metadata");
        }
    }

    // Both short blocks finish within one grain. The first deferred callback
    // changes the voice/system; the already queued second callback is stale.
    for (const auto action : {CallbackAction::Kill, CallbackAction::Setup, CallbackAction::Destroy}) {
        Fixture fixture;
        CallbackContext context{&fixture, action};
        RegisterCallback(context);
        std::array<s16, 64 * 2> input{};
        input.fill(8192);
        std::array<OrbisNgs2WaveformBlock, 2> blocks{{
            {0, 32 * 2 * sizeof(s16), 0, 0, 32, 0, 1},
            {32 * 2 * sizeof(s16), 32 * 2 * sizeof(s16), 0, 0, 32, 0, 2}}};
        OrbisNgs2SamplerVoiceWaveformBlocksParam packet{
            {sizeof(packet), 0, 0x10000001}, input.data(), 1, 2, blocks.data()};
        Check(sceNgs2VoiceControl(fixture.voices[0], &packet.header) == 0,
              "PCM callback invalidation queues two short blocks");
        OrbisNgs2VoiceEventParam play{{sizeof(play), 0, 6}, 0};
        Check(sceNgs2VoiceControl(fixture.voices[0], &play.header) == 0,
              "PCM callback invalidation Play");
        std::array<s16, 256 * 2> output{};
        OrbisNgs2RenderBufferInfo buffer{output.data(), sizeof(output), 0x12, 2};
        Check(sceNgs2SystemRender(fixture.system, &buffer, 1) == 0,
              "PCM callback can change or destroy its source without deadlock");
        Check(context.result == 0 && context.received.size() == 1,
              "callback source changes suppress the remaining stale completion");
        if (fixture.system) {
            Check(sceNgs2SystemRender(fixture.system, &buffer, 1) == 0 && context.received.size() == 1,
                  "invalidated PCM completion never reappears on the next render");
        }
    }
}

namespace Ngs2PcmStreamTestDetail {
static void CheckBaseState(OrbisNgs2Handle voice, u32 expected, bool sampler) {
    // Repeat all query forms without rendering: none may consume an event.
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        struct GuardedState {
            u32 before{0x12345678}, state{~0u}, after{0x87654321};
        } base;
        Check(sceNgs2VoiceGetState(voice, reinterpret_cast<OrbisNgs2VoiceState*>(&base.state),
                                  sizeof(base.state)) == 0 && base.state == expected &&
                  base.before == 0x12345678 && base.after == 0x87654321,
              "base voice state writes exactly four bytes and preserves canaries");
        u32 flags = ~0u;
        Check(sceNgs2VoiceGetStateFlags(voice, &flags) == 0 && flags == base.state,
              "base voice state agrees with the flags query");
        if (sampler) {
            OrbisNgs2SamplerVoiceState full{};
            Check(sceNgs2VoiceGetState(voice, &full.voiceState, sizeof(full)) == 0 &&
                      full.voiceState.stateFlags == base.state,
                  "full sampler state agrees with the base query");
        } else {
            OrbisNgs2MasteringVoiceState full{};
            Check(sceNgs2VoiceGetState(voice, &full.voiceState, sizeof(full)) == 0 &&
                      full.voiceState.stateFlags == base.state,
                  "full mastering state agrees with the base query");
        }
    }
    for (const size_t size : {8u, 47u, 49u}) {
        alignas(8) std::array<u8, 64> output;
        output.fill(0xa5);
        const auto before = output;
        Check(sceNgs2VoiceGetState(voice,
                                  reinterpret_cast<OrbisNgs2VoiceState*>(output.data() + 8), size) != 0 &&
                  output == before,
              "invalid voice state sizes preserve the entire destination");
    }
}
} // namespace Ngs2PcmStreamTestDetail

static void BaseVoiceStateTest() {
    using namespace Libraries::Ngs2;
    using namespace Ngs2PcmStreamTestDetail;
    Fixture fixture;
    const auto voice = fixture.voices[0];
    CheckBaseState(voice, 0, true);
    const auto event = [&](u32 id) {
        OrbisNgs2VoiceEventParam command{{sizeof(command), 0, 6}, id};
        Check(sceNgs2VoiceControl(voice, &command.header) == 0, "base state test event");
    };
    event(0);
    CheckBaseState(voice, 3, true);
    // Replacing pending Play with Pause exposes Empty. If a query had committed
    // Play, the visible state would instead still be Playing at this point.
    event(4);
    CheckBaseState(voice, 0, true);
    event(0);
    CheckBaseState(voice, 3, true);

    std::array<s16, 588 * 2> input{};
    input.fill(8192);
    OrbisNgs2WaveformBlock block{0, sizeof(input), 0, 0, 588, 0, 0};
    OrbisNgs2SamplerVoiceWaveformBlocksParam packet{
        {sizeof(packet), 0, 0x10000001}, input.data(), 0, 1, &block};
    Check(sceNgs2VoiceControl(voice, &packet.header) == 0, "base state finite PCM source");
    std::array<s16, 256 * 2> output{};
    OrbisNgs2RenderBufferInfo buffer{output.data(), sizeof(output), 0x12, 2};
    const auto render = [&] {
        Check(sceNgs2SystemRender(fixture.system, &buffer, 1) == 0, "base state accepted render");
    };
    render();
    CheckBaseState(voice, 3, true);
    event(4);
    CheckBaseState(voice, 3, true);
    render();
    CheckBaseState(voice, 5, true);
    Check(std::all_of(output.begin(), output.end(), [](s16 sample) { return sample == 0; }),
          "base state queries do not consume the pending pause");
    event(5);
    CheckBaseState(voice, 5, true);
    render();
    CheckBaseState(voice, 3, true);
    render();
    CheckBaseState(voice, 0, true);

    // The base query belongs to every voice, independently of its full state
    // layout. Exercise the larger mastering state through the same API.
    OrbisNgs2MasteringRackOption option{};
    option.rackOption.size = sizeof(option);
    option.rackOption.maxVoices = 1;
    option.maxChannels = 2;
    OrbisNgs2ContextBufferInfo backing{};
    Check(sceNgs2RackQueryBufferSize(0x3000, &option.rackOption, &backing) == 0,
          "base state mastering rack query");
    std::vector<u64> memory((backing.hostBufferSize + 7) / 8);
    backing.hostBuffer = memory.data();
    OrbisNgs2Handle rack{}, mastering{};
    Check(sceNgs2RackCreate(fixture.system, 0x3000, &option.rackOption, &backing, &rack) == 0 &&
              sceNgs2RackGetVoiceHandle(rack, 0, &mastering) == 0,
          "base state mastering voice create");
    CheckBaseState(mastering, 0, false);
    OrbisNgs2VoiceEventParam play{{sizeof(play), 0, 6}, 0};
    Check(sceNgs2VoiceControl(mastering, &play.header) == 0, "base state mastering pending Play");
    CheckBaseState(mastering, 3, false);
    render();
    CheckBaseState(mastering, 3, false);
    Check(sceNgs2RackDestroy(rack, nullptr) == 0, "base state mastering teardown");
}
