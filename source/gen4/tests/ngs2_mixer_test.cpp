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
int main(int argc, char** argv) {
    try {
        const bool enabled = argc == 1;
        Check(enabled || (argc == 2 && std::strcmp(argv[1], "--disabled") == 0),
              "expected --disabled or no arguments");
        if (enabled) setenv("LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER", "1", 1);
        ModelTest();
        PublicTest(enabled);
        std::puts("NGS2 bounded AT9 continuity, rollback, snapshots and render passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
