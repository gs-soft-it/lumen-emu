// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>
#include "core/libraries/ngs2/ngs2.h"
#include "core/libraries/ngs2/ngs2_error.h"
#include "core/libraries/ngs2/ngs2_sampler.h"
#include "core/libraries/ajm/ajm.h"
#include "core/libraries/ajm/ajm_error.h"

static void Check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
static void NgsLifecycle() {
    using namespace Libraries::Ngs2;
    OrbisNgs2SystemOption option{};Check(sceNgs2SystemResetOption(&option)==0,"system defaults");
    OrbisNgs2ContextBufferInfo query{};Check(sceNgs2SystemQueryBufferSize(&option,&query)==0&&query.hostBufferSize>8,"real system allocation");
    std::vector<u64> backing((query.hostBufferSize+7)/8);query.hostBuffer=backing.data();
    OrbisNgs2Handle system{};Check(sceNgs2SystemCreate(&option,&query,&system)==0&&system>1,"registered system");
    OrbisNgs2Handle duplicate{};Check(sceNgs2SystemCreate(&option,&query,&duplicate)==ORBIS_NGS2_ERROR_INVALID_BUFFER_ADDRESS,"overlapping backing rejected");
    OrbisNgs2SamplerRackOption rack_option{};rack_option.rackOption.size=sizeof(rack_option);rack_option.rackOption.maxVoices=2;rack_option.rackOption.maxMatrices=1;rack_option.rackOption.maxPorts=1;
    OrbisNgs2ContextBufferInfo rack_query{};Check(sceNgs2RackQueryBufferSize(0x1000,&rack_option.rackOption,&rack_query)==0,"rack size");
    std::vector<u64> rack_backing((rack_query.hostBufferSize+7)/8);rack_query.hostBuffer=rack_backing.data();
    OrbisNgs2Handle rack{},voice{},second{};Check(sceNgs2RackCreate(system,0x1000,&rack_option.rackOption,&rack_query,&rack)==0,"rack create");
    Check(sceNgs2RackGetVoiceHandle(rack,0,&voice)==0&&sceNgs2RackGetVoiceHandle(rack,1,&second)==0&&voice!=second,"distinct voices");
    OrbisNgs2Handle owner{};u32 index=99;Check(sceNgs2VoiceGetOwner(second,&owner,&index)==0&&owner==rack&&index==1,"voice owner outputs");
    Check(sceNgs2SystemEnumRackHandles(system,&owner,1)==1&&owner==rack,"rack enumeration");
    Check(sceNgs2SystemSetUserData(system,0x1234)==0,"set user data");uintptr_t data{};Check(sceNgs2SystemGetUserData(system,&data)==0&&data==0x1234,"get user data");
    Check(sceNgs2SystemLock(system)==0,"system lock");s32 cross_thread{};std::thread contender([&]{cross_thread=sceNgs2SystemSetGrainSamples(system,64);});contender.join();Check(cross_thread==ORBIS_NGS2_ERROR_INVALID_OPERATION,"lock ownership");Check(sceNgs2SystemUnlock(system)==0,"unlock");
    OrbisNgs2VoiceEventParam play{{sizeof(play),0,6},0};Check(sceNgs2VoiceControl(voice,&play.header)==0,"play event");
    std::array<float,1024> pcm;pcm.fill(0.5f);OrbisNgs2RenderBufferInfo buffer{pcm.data(),sizeof(pcm),0x18,2};
    Check(sceNgs2SystemRender(system,&buffer,1)==0,"baseline render");u32 flags{};Check(sceNgs2VoiceGetStateFlags(voice,&flags)==0&&flags==3,"playing state");
    Check(std::all_of(pcm.begin(),pcm.begin()+512,[](float x){return x==0;}),"initialized render output");Check(pcm[512]==0.5f,"render keeps trailing bytes");
    OrbisNgs2SamplerVoiceState state{};Check(sceNgs2VoiceGetState(voice,&state.voiceState,sizeof(state))==0&&state.voiceState.stateFlags==3&&state.envelopeHeight==1,"sampler state output");
    struct InvalidList{OrbisNgs2VoiceEventParam event;OrbisNgs2VoiceParamHeader bad;} invalid{{{sizeof(OrbisNgs2VoiceEventParam),sizeof(OrbisNgs2VoiceEventParam),6},4},{sizeof(OrbisNgs2VoiceParamHeader),0,0x7fff}};
    Check(sceNgs2VoiceControl(voice,&invalid.event.header)==ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID,"invalid tail rejected");
    Check(sceNgs2SystemRender(system,&buffer,1)==0&&sceNgs2VoiceGetStateFlags(voice,&flags)==0&&flags==0,"failed control did not commit pause");
    OrbisNgs2SystemInfo info{};Check(sceNgs2SystemGetInfo(system,&info,sizeof(info))==0&&info.rackCount==1&&info.renderCount==2,"system metadata");
    OrbisNgs2WaveformFormat format{0x18,2,48000,0,0,0};u32 frame{},samples{},units{},delay{};Check(sceNgs2GetWaveformFrameInfo(&format,&frame,&samples,&units,&delay)==0&&frame==8&&samples==1&&units==1&&delay==0,"PCM geometry");
    Check(sceNgs2GetWaveformFrameInfo(&format,&frame,&frame,nullptr,nullptr)==ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS,"aliased geometry output");
    Check(sceNgs2SystemDestroy(system,nullptr)==0,"system destruction");
    Check(sceNgs2VoiceGetStateFlags(voice,&flags)==ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE,"destroy invalidates voices");
    Check(sceNgs2RackDestroy(rack,nullptr)==ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE,"destroy invalidates racks");
    Check(sceNgs2SystemDestroy(system,nullptr)==ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE,"stale system rejected");
}
static void AjmLifecycle() {
    using namespace Libraries::Ajm;
    u32 context{};Check(sceAjmInitialize(0,&context)==0,"AJM initialize");
    Check(sceAjmModuleUnregister(context,AjmCodecType::At9Dec)==ORBIS_AJM_ERROR_CODEC_NOT_REGISTERED,"unregistered codec error");
    Check(sceAjmModuleRegister(context,AjmCodecType::At9Dec,0)==0,"register AT9");
    Check(sceAjmModuleRegister(context,AjmCodecType::At9Dec,0)==ORBIS_AJM_ERROR_CODEC_ALREADY_REGISTERED,"duplicate register");
    u32 instance{};Check(sceAjmInstanceCreate(context,AjmCodecType::At9Dec,1,&instance)==0,"create codec instance");
    Check(sceAjmModuleUnregister(context,AjmCodecType::At9Dec)==ORBIS_AJM_ERROR_BUSY,"live codec blocks unregister");
    Check(sceAjmInstanceDestroy(context,instance)==0,"destroy codec instance");
    Check(sceAjmInstanceDestroy(context,instance)==ORBIS_AJM_ERROR_INVALID_INSTANCE,"stale instance rejected");
    Check(sceAjmModuleUnregister(context,AjmCodecType::At9Dec)==0,"unregister released codec");
    Check(sceAjmInstanceCreate(context,AjmCodecType::At9Dec,1,&instance)==ORBIS_AJM_ERROR_CODEC_NOT_REGISTERED,"unregistered codec cannot instantiate");
    u32 batch{};Check(sceAjmBatchStartBuffer(context,nullptr,0,0,nullptr,&batch)==0,"empty batch");
    AjmBatchError result{};Check(sceAjmBatchWait(context,batch,0,&result)==0&&result.error_code==0,"empty batch completes once");
    Check(sceAjmBatchWait(context,batch,0,&result)==ORBIS_AJM_ERROR_INVALID_BATCH,"consumed batch invalid");
    Check(sceAjmFinalize(context)==0,"AJM finalize");Check(sceAjmFinalize(context)==ORBIS_AJM_ERROR_INVALID_CONTEXT,"finalize invalidates context");
    u32 next{};Check(sceAjmInitialize(0,&next)==0&&next!=context,"context identities not reused");Check(sceAjmFinalize(next)==0,"finalize new context");
}
int main(){try{NgsLifecycle();AjmLifecycle();std::puts("NGS2 and AJM lifecycle/output contracts passed");return 0;}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
