// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
#include "core/libraries/ngs2/ngs2.h"
#include "core/libraries/ngs2/ngs2_sampler.h"
#include "core/libraries/ngs2/ngs2_error.h"
#include "core/libraries/ngs2/ngs2_sampler_runtime.h"
using namespace Libraries::Ngs2;
static void Check(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
#include "atrac9_test_data.h"
static void State(OrbisNgs2Handle voice, u32 expected, const void* identity) {
    u32 flags = ~0u;
    OrbisNgs2SamplerVoiceState state{};
    Check(sceNgs2VoiceGetStateFlags(voice, &flags) == 0 && flags == expected,
          "state flags reflect pending Play without rendering");
    Check(sceNgs2VoiceGetState(voice, &state.voiceState, sizeof(state)) == 0 &&
              state.voiceState.stateFlags == expected,
          "full state agrees with flags");
    struct { OrbisNgs2VoiceState state; u32 canary; } base{{~0u}, 0x71adf023};
    Check(sceNgs2VoiceGetState(voice, &base.state, sizeof(base.state)) == 0 &&
              base.state.stateFlags == expected && base.canary == 0x71adf023,
          "base state agrees with flags and preserves canary with every selector");
    Check(state.waveformData == identity, "state preserves active block identity");
    Check(state.numDecodedSamples == 0 && state.decodedDataSize == 0 && state.userData == 0,
          "host decode sizes and opaque packet data are not guest progress counters");
}
static void ControlEnvelopes(OrbisNgs2Handle voice) {
    for (const auto [id, size] : std::array<std::pair<u32, u16>, 6>{
             {{0x10000005, 16}, {0x10000006, 24}, {0x10000007, 40}, {0x1000000a, 48},
              {1, 24}, {3, 16}}}) {
        alignas(8) std::array<u8, 56> bytes;
        bytes.fill(0xa5);
        OrbisNgs2VoiceParamHeader header{size, 0, id};
        std::memcpy(bytes.data(), &header, sizeof(header));
        Check(sceNgs2VoiceControl(voice,
                                 reinterpret_cast<const OrbisNgs2VoiceParamHeader*>(bytes.data())) == 0,
              "documented control envelope accepts opaque body without pointer reads");
        header.size = size + 8;
        std::memcpy(bytes.data(), &header, sizeof(header));
        Check(sceNgs2VoiceControl(voice,
                                 reinterpret_cast<const OrbisNgs2VoiceParamHeader*>(bytes.data())) ==
                  ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE,
              "control envelope requires exact size");
    }
    OrbisNgs2VoiceParamHeader unknown{sizeof(unknown), 0, 0x1000000b};
    Check(sceNgs2VoiceControl(voice, &unknown) == ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID,
          "unknown Sampler control remains rejected");
}
static void PortMatrixSelection(OrbisNgs2Handle voice) {
    // The local audio profile retains this signed selector; it does not look
    // it up in the optional user matrix table (this rack has zero matrices).
    for (const s32 matrix : std::array<s32, 6>{-1, -2, 0, 1,
              std::numeric_limits<s32>::min(), std::numeric_limits<s32>::max()}) {
        OrbisNgs2VoicePortMatrixParam select{{sizeof(select), 0, 2}, 0, matrix};
        Check(sceNgs2VoiceControl(voice, &select.header) == 0,
              "local mixer accepts signed port selector without user matrix lookup");
        OrbisNgs2VoicePortInfo port{};
        Check(sceNgs2VoiceGetPortInfo(voice, 0, &port, sizeof(port)) == 0 &&
                  port.matrixId == matrix && port.volume == 1 && port.destHandle == 0,
              "port query preserves selector without modifying routing or gain");
    }
    struct RejectedList {
        OrbisNgs2VoicePortMatrixParam valid;
        OrbisNgs2VoicePortMatrixParam invalid;
    } list{{{sizeof(OrbisNgs2VoicePortMatrixParam), sizeof(OrbisNgs2VoicePortMatrixParam), 2},
             0, 0},
           {{sizeof(OrbisNgs2VoicePortMatrixParam), 0, 2}, 1, 0}};
    Check(sceNgs2VoiceControl(voice, &list.valid.header) == ORBIS_NGS2_ERROR_INVALID_PORT_INDEX,
          "matrix selection still rejects unallocated port");
    OrbisNgs2VoicePortInfo port{};
    Check(sceNgs2VoiceGetPortInfo(voice, 0, &port, sizeof(port)) == 0 &&
              port.matrixId == std::numeric_limits<s32>::max(),
          "invalid tail rolls back preceding port selector");
    list.valid.header.next = 0;
    --list.valid.header.size;
    Check(sceNgs2VoiceControl(voice, &list.valid.header) ==
              ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE,
          "port selector requires exact envelope size");
}
static std::vector<float> Reference(const std::vector<u8>& data) {
    std::array<unsigned char,4> config{0xfe,0x74,0x1f,0xf0};
    std::shared_ptr<void> decoder(Atrac9GetHandle(),Atrac9ReleaseHandle);
    Check(Atrac9InitDecoder(decoder.get(),config.data())==0,"reference init");
    std::vector<float> pcm;
    for(size_t start=0;start<data.size();start+=1024){size_t offset=start,remaining=1024;for(int f=0;f<4;++f){
        std::array<float,512> frame{};int used{};
        Check(Atrac9DecodeF32(decoder.get(),data.data()+offset,static_cast<int>(remaining),frame.data(),&used,0)==0&&used>0&&static_cast<size_t>(used)<=remaining,"reference bounded decode");
        pcm.insert(pcm.end(),frame.begin(),frame.end());offset+=used;remaining-=used;
    }}return pcm;
}
static void ModelTest(){
    const auto first=Ngs2TestMakeStereoAtrac9Superframe(0x4b595459u),second=Ngs2TestMakeStereoAtrac9Superframe(0x4c5a555au),third=Ngs2TestMakeStereoAtrac9Superframe(0x4d5b565bu);
    std::vector<u8> joined=first;joined.insert(joined.end(),second.begin(),second.end());joined.insert(joined.end(),third.begin(),third.end());
    const auto reference=Reference(joined);const OrbisNgs2WaveformFormat format{0x40,2,48000,0xfe741ff0,0,0};
    Detail::SamplerRuntime sampler;Check(sampler.Append(format,{first,0,1024,0x111,0}),"first snapshot");
    auto malformed=second;auto bad=third;bad[0]|=0x40;malformed.insert(malformed.end(),bad.begin(),bad.end());
    auto* previous=sampler.history.get();Check(!sampler.Append(format,{malformed,0,2048,0x333,0}),"malformed late frame rejected");
    Check(sampler.blocks.size()==1&&sampler.history.get()==previous,"failed packet preserves history");
    Check(sampler.Append(format,{second,0,1024,0x222,0})&&sampler.Append(format,{third,0,1024,0x333,0}),"continuous append");
    std::vector<float> output;for(const auto& b:sampler.blocks)output.insert(output.end(),b.pcm->begin(),b.pcm->end());
    Check(output==reference,"separate appends equal continuous decoder history");
    Check(!sampler.Append(format,{{1,2},0,1,0,0}),"truncated packet");
    Detail::SamplerRuntime primed;Check(primed.Append(format,{first,256,768,0,0})&&primed.Append(format,{second,0,1024,0,0}),"priming and continuity");
    Check(std::equal(primed.blocks[0].pcm->begin(),primed.blocks[0].pcm->end(),reference.begin()+512),"trimmed prefix");
    Check(std::equal(primed.blocks[1].pcm->begin(),primed.blocks[1].pcm->end(),reference.begin()+2048),"append after primed packet");
}

static Detail::SamplerPacket PcmPacket(const std::vector<s16>& samples, size_t first,
                                       size_t count, u32 channels, u64 identity = 1) {
    Detail::SamplerPacket packet;
    packet.playable = count;
    packet.identity = identity;
    for (size_t index = first * channels; index < (first + count) * channels; ++index) {
        const u16 value = static_cast<u16>(samples.at(index));
        packet.encoded.push_back(static_cast<u8>(value));
        packet.encoded.push_back(static_cast<u8>(value >> 8));
    }
    return packet;
}

static std::vector<float> PcmReference(const std::vector<s16>& samples, u32 channels, u32 rate) {
    const size_t frames = samples.size() / channels;
    const size_t output_frames = (frames * 48000 + rate - 1) / rate;
    std::vector<float> result(output_frames * channels);
    for (size_t frame = 0; frame < output_frames; ++frame) {
        const u64 position = static_cast<u64>(frame) * rate;
        const size_t first = position / 48000;
        const size_t second = std::min(first + 1, frames - 1);
        const double fraction = static_cast<double>(position % 48000) / 48000.0;
        for (u32 channel = 0; channel < channels; ++channel) {
            const double a = samples[first * channels + channel] / 32768.0;
            const double b = samples[second * channels + channel] / 32768.0;
            result[frame * channels + channel] = static_cast<float>(a * (1.0 - fraction) + b * fraction);
        }
    }
    return result;
}

static void SamePcm(std::span<const float> actual, std::span<const float> expected, const char* reason) {
    Check(actual.size() == expected.size(), reason);
    for (size_t index = 0; index < actual.size(); ++index)
        Check(std::fabs(actual[index] - expected[index]) <= 1.0e-7f, reason);
}

static std::vector<float> RenderPcm(Detail::SamplerRuntime& sampler, u32 channels,
                                     size_t output_frames, bool variable_grain) {
    u32 state = 0;
    sampler.Event(0, state, channels);
    std::vector<float> result;
    size_t call = 0;
    while (state == 3) {
        const size_t grain = variable_grain ? std::array<size_t, 3>{64, 256, 128}[call++ % 3] : 1;
        std::vector<float> output(grain * channels);
        sampler.Mix(state, channels, output, channels);
        result.insert(result.end(), output.begin(), output.end());
        Check(result.size() <= (output_frames + 256) * channels, "PCM reaches bounded EOF");
    }
    Check(result.size() >= output_frames * channels, "PCM does not reach EOF too early");
    for (size_t index = output_frames * channels; index < result.size(); ++index)
        Check(result[index] == 0, "PCM padding after EOF is silent");
    result.resize(output_frames * channels);
    return result;
}

static void PcmModelTest() {
    constexpr u32 channels = 2;
    constexpr size_t frames = 1537;
    std::vector<s16> source;
    for (size_t frame = 0; frame < frames; ++frame) {
        source.push_back(static_cast<s16>(static_cast<int>(frame % 701) * 31 - 15000));
        source.push_back(static_cast<s16>(14000 - static_cast<int>(frame % 643) * 37));
    }
    for (const u32 rate : {44100u, 48000u}) {
        const OrbisNgs2WaveformFormat format{0x12, channels, rate, 0, 0, 0};
        const auto expected = PcmReference(source, channels, rate);
        Detail::SamplerRuntime whole, split;
        auto packet = PcmPacket(source, 0, frames, channels);
        Check(whole.Append(format, packet), "owned S16 PCM snapshot accepted");
        std::fill(packet.encoded.begin(), packet.encoded.end(), 0);
        size_t first = 0;
        for (const size_t end : {size_t{1}, size_t{97}, size_t{442}, size_t{997}, frames}) {
            auto part = PcmPacket(source, first, end - first, channels, end);
            Check(split.Append(format, part), "PCM split packet accepted");
            std::fill(part.encoded.begin(), part.encoded.end(), 0);
            first = end;
        }
        const auto whole_pcm = RenderPcm(whole, channels, expected.size() / channels, false);
        const auto split_pcm = RenderPcm(split, channels, expected.size() / channels, true);
        SamePcm(whole_pcm, expected, "S16/rational resample matches independent ramp reference");
        SamePcm(split_pcm, expected, "packet and grain boundaries preserve continuous PCM");
        Check(whole.phase == split.phase && whole.phase ==
                  ((expected.size() / channels) * static_cast<u64>(rate)) % 48000,
              "EOF preserves exact rational phase without drift");

        Detail::SamplerRuntime skipped;
        packet = PcmPacket(source, 0, frames, channels);
        packet.skip = 7;
        packet.playable = 113;
        Check(skipped.Append(format, packet), "PCM skip/playable window accepted");
        const std::vector<s16> window(source.begin() + 7 * channels, source.begin() + 120 * channels);
        const auto window_reference = PcmReference(window, channels, rate);
        SamePcm(RenderPcm(skipped, channels, window_reference.size() / channels, true),
                window_reference, "PCM skip selects exact owned samples");
        Check(skipped.AtEnd(channels), "PCM final block remains available for replay");
        SamePcm(RenderPcm(skipped, channels, window_reference.size() / channels, true),
                window_reference, "explicit PCM replay restarts cursor and rational phase");
    }

    const OrbisNgs2WaveformFormat format{0x12, channels, 44100, 0, 0, 0};
    Detail::SamplerRuntime streaming;
    Check(streaming.Append(format, PcmPacket(source, 0, 100, channels)), "PCM streaming prefix");
    u32 state = 0;
    streaming.Event(0, state, channels);
    std::vector<float> prefix(64 * channels);
    streaming.Mix(state, channels, prefix, channels);
    const auto expected = PcmReference(source, channels, 44100);
    SamePcm(prefix, std::span(expected).first(prefix.size()), "PCM first partial grain");
    const size_t cursor = streaming.cursor;
    const u32 phase = streaming.phase;
    streaming.Event(4, state, channels);
    std::array<float, 128> paused{};
    streaming.Mix(state, channels, paused, channels);
    Check(state == 5 && streaming.cursor == cursor && streaming.phase == phase &&
              std::all_of(paused.begin(), paused.end(), [](float value) { return value == 0; }),
          "PCM pause preserves fractional cursor and writes no samples");
    streaming.Event(5, state, channels);
    const auto before = streaming;
    auto malformed = PcmPacket(source, 100, 30, channels);
    malformed.encoded.pop_back();
    Check(!streaming.Append(format, malformed), "partial PCM frame rejected");
    malformed = PcmPacket(source, 100, 30, channels);
    malformed.skip = 31;
    Check(!streaming.Append(format, malformed), "PCM skip outside source rejected");
    malformed.skip = 1;
    Check(!streaming.Append(format, malformed), "PCM playable exceeding remaining source rejected");
    malformed = PcmPacket(source, 100, 30, channels);
    malformed.playable = 0;
    Check(!streaming.Append(format, malformed), "empty PCM timeline rejected");
    malformed = PcmPacket(source, 100, 30, channels);
    for (unsigned invalid = 0; invalid < 8; ++invalid) {
        auto rejected = format;
        switch (invalid) {
        case 0: rejected.numChannels = 0; break;
        case 1: rejected.numChannels = 9; break;
        case 2: rejected.sampleRate = 0; break;
        case 3: rejected.sampleRate = 22050; break;
        case 4: rejected.frameOffset = 1; break;
        case 5: rejected.frameMargin = 1; break;
        case 6: rejected.configData = 1; break;
        case 7: rejected.sampleRate = 48000; break;
        }
        Check(!streaming.Append(rejected, malformed), "invalid or changing PCM geometry rejected");
    }
    Check(streaming.cursor == before.cursor && streaming.phase == before.phase &&
              streaming.retained == before.retained && streaming.blocks.size() == before.blocks.size() &&
              streaming.blocks.front().pcm == before.blocks.front().pcm &&
              streaming.source_sample_rate == before.source_sample_rate && streaming.history == before.history,
          "malformed PCM append leaves samples, history and fractional cursor unchanged");
    Check(streaming.Append(format, PcmPacket(source, 100, frames - 100, channels)), "PCM live refill");
    std::vector<float> remaining((expected.size() / channels - 64) * channels);
    streaming.Mix(state, channels, remaining, channels);
    SamePcm(remaining, std::span(expected).subspan(prefix.size()), "refill and pause preserve timeline");
    Check(state == 0, "PCM refill reaches exact EOF");

    // With no queued lookahead, the final source value is held at the edge.
    // A later refill cannot change that emitted value, but must retain phase:
    // 73 + 74 source frames are exactly 160 output frames, not 80 + 81.
    Detail::SamplerRuntime late;
    Check(late.Append(format, PcmPacket(source, 0, 73, channels)), "late refill prefix");
    const auto first_output = RenderPcm(late, channels, 80, false);
    Check(late.phase == 24000 && first_output.size() == 80 * channels, "EOF retains half-frame phase");
    Check(late.Append(format, PcmPacket(source, 73, 74, channels)), "append after PCM EOF");
    Check(late.phase == 24000, "EOF refill does not round source position to zero");
    const auto last_output = RenderPcm(late, channels, 80, false);
    const std::vector<s16> short_source(source.begin(), source.begin() + 147 * channels);
    const auto short_reference = PcmReference(short_source, channels, 44100);
    SamePcm(last_output, std::span(short_reference).subspan(80 * channels), "late refill keeps global phase");
    Check(late.phase == 0, "147 source frames consume exactly 160 output frames");
    late.Event(2, state, channels);
    Check(state == 0 && late.blocks.empty() && late.cursor == 0 && late.retained == 0 && late.phase == 0,
          "PCM stop clears samples and resampling phase");

    Detail::SamplerRuntime endpoints;
    const std::vector<s16> limits{-32768, 0, 32767};
    Check(endpoints.Append({0x12, 1, 48000, 0, 0, 0}, PcmPacket(limits, 0, 3, 1)), "PCM endpoints");
    u32 playing = 0;
    endpoints.Event(0, playing, 1);
    std::array<float, 6> stereo{};
    endpoints.Mix(playing, 1, stereo, 2);
    Check(stereo == std::array<float, 6>{-1, -1, 0, 0, 32767.0f / 32768, 32767.0f / 32768},
          "48k S16 endpoints and mono duplication are exact");

    for (const u32 rate : {44100u, 48000u}) {
        Detail::SamplerRuntime notifications;
        const OrbisNgs2WaveformFormat callback_format{0x12, channels, rate, 0, 0, 0};
        auto first_block = PcmPacket(source, 0, 73, channels, 0x111);
        auto second_block = PcmPacket(source, 73, 74, channels, 0x222);
        first_block.user = 11;
        second_block.user = 22;
        Check(notifications.Append(callback_format, first_block) &&
                  notifications.Append(callback_format, second_block), "completion fixture blocks");
        std::vector<Detail::DecodedBlock> completed;
        state = 0;
        notifications.Event(0, state, channels);
        std::array<float, 256 * channels> output{};
        notifications.Mix(state, channels, output, channels, &completed);
        Check(state == 0 && completed.size() == 2 && completed[0].identity == 0x111 &&
                  completed[0].user == 11 && completed[0].dataAddress == 0x111 &&
                  completed[0].dataSize == 73 * channels * sizeof(s16) &&
                  completed[1].identity == 0x222 && completed[1].user == 22 &&
                  completed[1].dataSize == 74 * channels * sizeof(s16),
              "each consumed block publishes ordered completion metadata");
        // Simulate a streaming owner keeping the exhausted voice Playing.
        state = 3;
        output.fill(0);
        notifications.Mix(state, channels, output, channels, &completed);
        Check(completed.size() == 2 && notifications.final_completion_reported &&
                  std::all_of(output.begin(), output.end(), [](float value) { return value == 0; }),
              "retained final block cannot notify twice while waiting for refill");
        notifications.Event(4, state, channels);
        notifications.Event(5, state, channels);
        notifications.Mix(state, channels, output, channels, &completed);
        Check(completed.size() == 2, "pause/resume cannot replay final completion");
        notifications.Event(0, state, channels);
        output.fill(0);
        notifications.Mix(state, channels, output, channels, &completed);
        Check(completed.size() == 3 && completed.back().identity == 0x222,
              "explicit retained-block replay publishes one new completion");
        auto refill = PcmPacket(source, 147, 31, channels, 0x333);
        Check(notifications.Append(callback_format, refill) && !notifications.final_completion_reported,
              "EOF replacement rearms only the new block completion");
        notifications.Event(0, state, channels);
        output.fill(0);
        notifications.Mix(state, channels, output, channels, &completed);
        Check(completed.size() == 4 && completed.back().identity == 0x333,
              "replacement block completes exactly once");
        notifications.Event(2, state, channels);
        Check(!notifications.final_completion_reported && notifications.blocks.empty(),
              "stop resets completion state");
    }
}

static void PublicTest(bool enabled){
    OrbisNgs2SystemOption option{};Check(sceNgs2SystemResetOption(&option)==0,"default option");option.numGrainSamples=512;
    OrbisNgs2ContextBufferInfo system_info{};Check(sceNgs2SystemQueryBufferSize(&option,&system_info)==0,"system query");
    std::vector<u64> system_memory((system_info.hostBufferSize+7)/8);system_info.hostBuffer=system_memory.data();OrbisNgs2Handle system{};
    Check(sceNgs2SystemCreate(&option,&system_info,&system)==0,"system create");
    OrbisNgs2SamplerRackOption rack_option{};rack_option.rackOption.size=sizeof(rack_option);rack_option.rackOption.maxVoices=1;rack_option.rackOption.maxPorts=1;rack_option.maxWaveformBlocks=1;
    OrbisNgs2ContextBufferInfo rack_info{};Check(sceNgs2RackQueryBufferSize(0x1000,&rack_option.rackOption,&rack_info)==0,"rack query");
    std::vector<u64> rack_memory((rack_info.hostBufferSize+7)/8);rack_info.hostBuffer=rack_memory.data();OrbisNgs2Handle rack{},voice{};
    Check(sceNgs2RackCreate(system,0x1000,&rack_option.rackOption,&rack_info,&rack)==0&&sceNgs2RackGetVoiceHandle(rack,0,&voice)==0,"voice create");
    if (!enabled) {
        OrbisNgs2VoiceMatrixLevelsParam invalid_matrix{{sizeof(invalid_matrix), 0, 1},
                                                      ~0u, 0, nullptr};
        Check(sceNgs2VoiceControl(voice, &invalid_matrix.header) ==
                  ORBIS_NGS2_ERROR_INVALID_MATRIX_INDEX,
              "disabled mixer retains typed matrix validation");
        OrbisNgs2VoicePortVolumeParam invalid_volume{{sizeof(invalid_volume), 0, 3}, ~0u, 1};
        Check(sceNgs2VoiceControl(voice, &invalid_volume.header) ==
                  ORBIS_NGS2_ERROR_INVALID_PORT_INDEX,
              "disabled mixer retains typed volume validation");
        OrbisNgs2VoicePortMatrixParam invalid_select{{sizeof(invalid_select), 0, 2}, 0, 0};
        Check(sceNgs2VoiceControl(voice, &invalid_select.header) ==
                  ORBIS_NGS2_ERROR_INVALID_MATRIX_INDEX,
              "disabled mixer retains user matrix range validation");
        OrbisNgs2VoiceEventParam play{{sizeof(play), 0, 6}, 0};
        Check(sceNgs2VoiceControl(voice, &play.header) == 0, "baseline pending Play");
        State(voice, 0, nullptr);
        // Selector is latched: changing the environment cannot enable the mixer.
        setenv("LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER", "1", 1);
        State(voice, 0, nullptr);
        std::array<float, 512 * 8> pcm;
        pcm.fill(0.5f);
        OrbisNgs2RenderBufferInfo output{pcm.data(), sizeof(pcm), 0x18, 8};
        Check(sceNgs2SystemRender(system, &output, 1) == 0, "baseline render");
        State(voice, 3, nullptr);
        Check(std::all_of(pcm.begin(), pcm.end(), [](float value) { return value == 0; }),
              "disabled mixer preserves silent baseline");
        Check(sceNgs2SystemRender(system, &output, 1) == 0, "baseline next render");
        State(voice, 0, nullptr);
        Check(sceNgs2SystemDestroy(system, nullptr) == 0, "baseline teardown");
        return;
    }
    OrbisNgs2SamplerVoiceSetupParam setup{{sizeof(setup),0,0x10000000},{0x40,2,48000,0xfe741ff0,0,0},0,0};
    Check(sceNgs2VoiceControl(voice,&setup.header)==0,"format control");
    State(voice, 0, nullptr);
    OrbisNgs2VoiceEventParam play{{sizeof(play),0,6},0};
    Check(sceNgs2VoiceControl(voice,&play.header)==0,"Play before first refill");
    State(voice, 3, nullptr);
    State(voice, 3, nullptr);
    // Replacing pending Play with Pause proves that queries did not consume it
    // or commit Playing: only an accepted render may change stored state.
    OrbisNgs2VoiceEventParam pause{{sizeof(pause),0,6},4};
    Check(sceNgs2VoiceControl(voice,&pause.header)==0,"replace pending Play");
    State(voice, 0, nullptr);
    Check(sceNgs2VoiceControl(voice,&play.header)==0,"restore pending Play");
    ControlEnvelopes(voice);
    PortMatrixSelection(voice);
    State(voice, 3, nullptr);
    struct InvalidList {
        OrbisNgs2VoiceEventParam stop;
        OrbisNgs2VoiceParamHeader bad;
    } invalid{{{sizeof(OrbisNgs2VoiceEventParam), sizeof(OrbisNgs2VoiceEventParam), 6}, 2},
              {sizeof(OrbisNgs2VoiceParamHeader), 0, 0x10000005}};
    Check(sceNgs2VoiceControl(voice, &invalid.stop.header) ==
              ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE,
          "bad envelope rejects complete control list");
    State(voice, 3, nullptr);
    auto first=Ngs2TestMakeStereoAtrac9Superframe(0x4b595459u),second=Ngs2TestMakeStereoAtrac9Superframe(0x4c5a555au);
    auto joined=first;joined.insert(joined.end(),second.begin(),second.end());const auto reference=Reference(joined);
    struct Descriptor{u32 offset,size,flags,skip;u64 samples,user;};
    struct Param{OrbisNgs2VoiceParamHeader header;const void* data;u32 count,opaque;const Descriptor* descriptor;};
    Descriptor descriptor{0,1024,0,0,1024,0x99};Param param{{sizeof(param),0,0x10000001},first.data(),1,0,&descriptor};
    Check(sceNgs2VoiceControl(voice,&param.header)==0,"first encoded snapshot");param.data=second.data();Check(sceNgs2VoiceControl(voice,&param.header)==0,"second encoded snapshot");
    State(voice, 3, first.data());
    std::fill(first.begin(),first.end(),0);std::fill(second.begin(),second.end(),0);
    std::array<float,512*8> pcm{};OrbisNgs2RenderBufferInfo output{pcm.data(),sizeof(pcm),0x18,8};
    auto rejected_output = output;
    rejected_output.bufferSize -= sizeof(float);
    Check(sceNgs2SystemRender(system, &rejected_output, 1) != 0,
          "invalid render rejected before pending Play or cursor commit");
    State(voice, 3, first.data());
    bool audible=false;
    for(size_t grain=0;grain<4;++grain){Check(sceNgs2SystemRender(system,&output,1)==0,"render accepted FIFO");for(size_t f=0;f<512;++f)for(size_t c=0;c<8;++c){
        float expected=c<2?reference[(grain*512+f)*2+c]:0;expected=std::isfinite(expected)?std::clamp(expected,-1.0f,1.0f):0;
        Check(pcm[f*8+c]==expected,"render samples match independent continuous decode");audible|=std::fabs(expected)>1e-7f;
    }}Check(audible,"synthetic fixture has nonzero audio");
    u32 flags{};Check(sceNgs2VoiceGetStateFlags(voice,&flags)==0&&flags==0,"EOF state");
    State(voice, 0, second.data());
    Check(sceNgs2VoiceControl(voice,&play.header)==0&&sceNgs2SystemRender(system,&output,1)==0,"EOF replay");
    for(size_t f=0;f<512;++f)for(size_t c=0;c<2;++c)Check(pcm[f*8+c]==std::clamp(reference[(1024+f)*2+c],-1.0f,1.0f),"retained final block rewind");
    Check(sceNgs2SystemDestroy(system,nullptr)==0,"mixer teardown");
}

// Exercise the public HLE using owned ATRAC9 input and a separate continuous
// decode as the oracle. No sampler internals or render helpers are inspected.
static s16 ReferenceS16(float sample) {
    if (!std::isfinite(sample)) return 0;
    const double scaled = static_cast<double>(std::clamp(sample, -1.0f, 1.0f)) * 32768.0;
    // Nearest integer, ties away from zero, independent of the process rounding mode.
    const double rounded = std::copysign(std::floor(std::fabs(scaled) + 0.5), scaled);
    return static_cast<s16>(std::clamp(rounded, -32768.0, 32767.0));
}

static void RenderLayoutTest(u32 grain, u32 channels, bool integer_output, u32 copies = 1) {
    auto first = Ngs2TestMakeStereoAtrac9Superframe(0x4b595459u);
    auto second = Ngs2TestMakeStereoAtrac9Superframe(0x4c5a555au);
    auto encoded = first;
    encoded.insert(encoded.end(), second.begin(), second.end());
    const auto reference = Reference(encoded);
    if (!copies) {
        float peak = 0;
        for (float sample : reference) peak = std::max(peak, std::fabs(sample));
        Check(peak > 0 && std::isfinite(peak), "saturation source has a finite nonzero peak");
        const double required = std::ceil(1.25 / static_cast<double>(peak));
        Check(required <= 4096, "saturation fixture stays within its voice/memory bound");
        copies = std::max(2u, static_cast<u32>(required));
    }
    const size_t frames = reference.size() / 2;
    const size_t sample_bytes = integer_output ? sizeof(s16) : sizeof(float);
    const size_t output_bytes = static_cast<size_t>(grain) * channels * sample_bytes;

    OrbisNgs2SystemOption option{};
    Check(sceNgs2SystemResetOption(&option) == 0, "layout default options");
    Check(option.numGrainSamples == 256, "default grain remains 256");
    option.maxGrainSamples = std::max(option.maxGrainSamples, grain);
    option.numGrainSamples = grain;
    OrbisNgs2ContextBufferInfo system_info{};
    Check(sceNgs2SystemQueryBufferSize(&option, &system_info) == 0, "layout system size");
    // Extra host allocation lets the live-backing alias rejection below use a
    // fully accessible output span, without changing the advertised backing size.
    std::vector<u64> system_memory((std::max(system_info.hostBufferSize, output_bytes) + 7) / 8);
    system_info.hostBuffer = system_memory.data();
    OrbisNgs2Handle system{};
    Check(sceNgs2SystemCreate(&option, &system_info, &system) == 0, "layout system create");
    OrbisNgs2SamplerRackOption rack_option{};
    rack_option.rackOption.size = sizeof(rack_option);
    rack_option.rackOption.maxVoices = copies;
    rack_option.rackOption.maxPorts = 1;
    rack_option.maxWaveformBlocks = 1;
    OrbisNgs2ContextBufferInfo rack_info{};
    Check(sceNgs2RackQueryBufferSize(0x1000, &rack_option.rackOption, &rack_info) == 0,
          "layout rack size");
    std::vector<u64> rack_memory((rack_info.hostBufferSize + 7) / 8);
    rack_info.hostBuffer = rack_memory.data();
    OrbisNgs2Handle rack{};
    Check(sceNgs2RackCreate(system, 0x1000, &rack_option.rackOption, &rack_info, &rack) == 0,
          "layout rack create");
    std::vector<OrbisNgs2Handle> voices(copies);
    OrbisNgs2SamplerVoiceSetupParam setup{{sizeof(setup), 0, 0x10000000},
                                         {0x40, 2, 48000, 0xfe741ff0, 0, 0}, 0, 0};
    struct Descriptor { u32 offset, size, flags, skip; u64 samples, user; };
    struct Param {
        OrbisNgs2VoiceParamHeader header;
        const void* data;
        u32 count, opaque;
        const Descriptor* descriptor;
    };
    Descriptor descriptor{0, 1024, 0, 0, 1024, 0};
    Param param{{sizeof(param), 0, 0x10000001}, nullptr, 1, 0, &descriptor};
    OrbisNgs2VoiceEventParam play{{sizeof(play), 0, 6}, 0};
    for (u32 index = 0; index < copies; ++index) {
        auto& voice = voices[index];
        Check(sceNgs2RackGetVoiceHandle(rack, index, &voice) == 0, "layout voice create");
        Check(sceNgs2VoiceControl(voice, &setup.header) == 0, "layout ATRAC9 format");
        param.data = first.data();
        Check(sceNgs2VoiceControl(voice, &param.header) == 0, "layout first owned block");
        param.data = second.data();
        Check(sceNgs2VoiceControl(voice, &param.header) == 0, "layout second owned block");
        Check(sceNgs2VoiceControl(voice, &play.header) == 0, "layout pending Play");
    }
    std::fill(first.begin(), first.end(), 0);
    std::fill(second.begin(), second.end(), 0);

    std::vector<u64> storage((output_bytes + 2 * sizeof(u64) + 7) / 8, 0xa5a5a5a5a5a5a5a5ULL);
    auto* output_data = reinterpret_cast<u8*>(storage.data()) + sizeof(u64);
    OrbisNgs2RenderBufferInfo output{output_data, output_bytes, integer_output ? 0x12u : 0x18u, channels};
    const auto untouched = storage;
    auto rejected = output;
    --rejected.bufferSize;
    Check(sceNgs2SystemRender(system, &rejected, 1) != 0 && storage == untouched,
          "short output leaves all bytes unchanged");
    rejected = output;
    ++rejected.bufferSize;
    Check(sceNgs2SystemRender(system, &rejected, 1) != 0 && storage == untouched,
          "oversize output remains rejected without writes");
    struct Alias {
        OrbisNgs2RenderBufferInfo descriptor;
        std::array<u8, 1024 * 8 * sizeof(float)> bytes{};
    } alias;
    alias.descriptor = output;
    alias.descriptor.buffer = &alias;
    std::array<u8, sizeof(alias)> alias_before{};
    std::memcpy(alias_before.data(), &alias, sizeof(alias));
    Check(sceNgs2SystemRender(system, &alias.descriptor, 1) != 0 &&
              std::memcmp(alias_before.data(), &alias, sizeof(alias)) == 0,
          "descriptor/output alias is rejected without modifying descriptor or output");
    rejected = output;
    rejected.buffer = system_memory.data();
    const auto backing_before = system_memory;
    Check(sceNgs2SystemRender(system, &rejected, 1) != 0 && system_memory == backing_before,
          "live system backing cannot become render output");
    // QueryState alone exposes pending Play as Playing. Replacing the event
    // with Pause distinguishes an unconsumed Play from a committed state.
    OrbisNgs2VoiceEventParam pause{{sizeof(pause), 0, 6}, 4};
    for (const auto voice : voices) {
        State(voice, 3, first.data());
        Check(sceNgs2VoiceControl(voice, &pause.header) == 0, "replace rejected render Play");
        State(voice, 0, first.data());
        Check(sceNgs2VoiceControl(voice, &play.header) == 0, "restore unconsumed Play");
    }

    bool audible = false, clipped = false;
    for (size_t frame = 0; frame < frames; frame += grain) {
        // Rejection after playback starts must also preserve the PCM cursor.
        if (frame == grain) {
            rejected = output;
            --rejected.bufferSize;
            const auto previous = storage;
            Check(sceNgs2SystemRender(system, &rejected, 1) != 0 && storage == previous,
                  "rejected midstream render leaves previous PCM untouched");
        }
        Check(sceNgs2SystemRender(system, &output, 1) == 0, "variable layout render");
        Check(storage.front() == untouched.front() && storage.back() == untouched.back(),
              "render writes only the exact output span");
        for (size_t index = 0; index < grain; ++index) {
            for (u32 channel = 0; channel < channels; ++channel) {
                float mixed = 0;
                if (channel < 2) {
                    for (u32 copy = 0; copy < copies; ++copy) mixed += reference[(frame + index) * 2 + channel];
                }
                clipped |= std::fabs(mixed) >= 1.0f;
                const size_t offset = (index * channels + channel) * sample_bytes;
                if (integer_output) {
                    s16 actual{};
                    std::memcpy(&actual, output_data + offset, sizeof(actual));
                    Check(actual == ReferenceS16(mixed), "S16 output matches independently quantized ATRAC9");
                    audible |= actual != 0;
                } else {
                    float actual{};
                    std::memcpy(&actual, output_data + offset, sizeof(actual));
                    const float expected = std::isfinite(mixed) ? std::clamp(mixed, -1.0f, 1.0f) : 0;
                    Check(actual == expected, "Float32 output preserves ATRAC9 samples at variable grain");
                    audible |= actual != 0;
                }
            }
        }
    }
    Check(audible, "new render layout produces nonzero samples");
    if (copies > 1) Check(clipped, "multi-voice S16 fixture exercises saturation");
    for (const auto voice : voices) State(voice, 0, second.data());
    Check(sceNgs2SystemDestroy(system, nullptr) == 0, "layout teardown");
}

#include "ngs2_pcm_stream_test.h"

int main(int argc, char** argv) {
    try {
        const bool enabled = argc == 1;
        Check(enabled || (argc == 2 && std::strcmp(argv[1], "--disabled") == 0),
              "expected --disabled or no arguments");
        if (enabled) setenv("LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER", "1", 1);
        ModelTest();
        PcmModelTest();
        PublicTest(enabled);
        if (enabled) {
            PcmStreamTest();
            PcmCallbackStreamTest();
            BaseVoiceStateTest();
            RenderLayoutTest(256, 2, true);
            RenderLayoutTest(256, 2, true, 0);
            RenderLayoutTest(64, 1, false);
            RenderLayoutTest(1024, 6, false);
        }
        std::puts("NGS2 bounded AT9 continuity, rollback, snapshots and render passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
