// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ngs2.h"
#include "ngs2_error.h"
#include "ngs2_sampler.h"
#include "ngs2_mastering.h"
#include "ngs2_submixer.h"
#include "ngs2_reverb.h"
#include "ngs2_custom.h"
#include "ngs2_sampler_runtime.h"
#include <cstdlib>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
extern "C" {
#include <libatrac9.h>
}
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/guest_callback.h"
#include "core/guest_cpu/guest_callback_scratch.h"
#include "core/guest_cpu/hle_call_adapter.h"
#endif

namespace Libraries::Ngs2 {
namespace {
// Opaque handles identify native-owned state. The application owns the context
// backing, but never hosts a C++ mutex, vector or decoder object in that memory.
struct Voice {
    u32 state{}, event{~0u};
    OrbisNgs2WaveformFormat waveform{};
    Detail::SamplerRuntime sampler;
    std::vector<OrbisNgs2VoicePortInfo> ports;
    std::vector<OrbisNgs2VoiceMatrixInfo> matrices;
};
struct System {
    OrbisNgs2SystemOption option{};
    OrbisNgs2ContextBufferInfo backing{};
    OrbisNgs2BufferFreeHandler free{};
    uintptr_t user{};
    u64 renders{};
    u64 nonzero_renders{};
    float peak{};
    std::thread::id lock_owner{};
    u32 lock_depth{};
};
struct Rack {
    OrbisNgs2Handle system{};
    u32 id{};
    OrbisNgs2RackOption option{};
    OrbisNgs2ContextBufferInfo backing{};
    OrbisNgs2BufferFreeHandler free{};
    uintptr_t user{};
    std::vector<Voice> voices;
    OrbisNgs2Handle first_voice{};
    std::thread::id lock_owner{};
    u32 lock_depth{};
};
std::mutex registry_mutex;
std::map<OrbisNgs2Handle, System> systems;
std::map<OrbisNgs2Handle, Rack> racks;
// Never reuse an exposed identity, including when an allocator reuses storage.
OrbisNgs2Handle next_handle{0x10000};
constexpr size_t SystemBytes = 512;
constexpr size_t RackHeaderBytes = 4096;
constexpr size_t VoiceBytes = 256;
constexpr u32 MaxVoices = 65535;
bool MixerEnabled() {
    static const bool enabled=[] { const char* value=std::getenv("LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER");return value && std::strcmp(value,"1")==0; }();
    return enabled;
}
bool AudioTraceEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("LUMEN_NGS2_AUDIO_TRACE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
u32 QueryState(const Voice& voice) {
    // A producer can query immediately after Play before supplying its first
    // block. Expose that pending transition without consuming the event: only
    // an accepted render commits state or advances the PCM cursor.
    return MixerEnabled() && voice.state == 0 && voice.event == 0 ? 3 : voice.state;
}
s32 FrameInfo(const OrbisNgs2WaveformFormat&,std::array<u32,4>&);


bool Range(const void* p, size_t n, bool write) {
    const auto address = reinterpret_cast<uintptr_t>(p);
    if (!p || !n || n > UINTPTR_MAX - address) return false;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    return Core::GuestCpu::ValidateHlePointer(p, n, write);
#else
    return true;
#endif
}
template<class T> bool Read(const T* p, T& out) {
    if (!Range(p, sizeof(T), false)) return false;
    std::memcpy(&out, p, sizeof(T));
    return true;
}
template<class T> bool Write(T* p, const T& value) {
    if (!Range(p, sizeof(T), true)) return false;
    std::memcpy(p, &value, sizeof(T));
    return true;
}
bool Overlap(const void* a, size_t an, const void* b, size_t bn) {
    const auto x = reinterpret_cast<uintptr_t>(a), y = reinterpret_cast<uintptr_t>(b);
    return an && bn && x < y + bn && y < x + an;
}
bool AliasesBacking(const void* p, size_t n) {
    for (const auto& [id, s] : systems)
        if (Overlap(p, n, s.backing.hostBuffer, s.backing.hostBufferSize)) return true;
    for (const auto& [id, r] : racks)
        if (Overlap(p, n, r.backing.hostBuffer, r.backing.hostBufferSize)) return true;
    return false;
}
bool CanAccess(const auto& object) {
    return object.lock_depth == 0 || object.lock_owner == std::this_thread::get_id();
}
OrbisNgs2Handle AllocateHandles(size_t count) {
    if (!count || count > UINTPTR_MAX - next_handle) return 0;
    const auto result = next_handle;
    next_handle += count;
    return result;
}
Rack* FindVoice(OrbisNgs2Handle handle, Voice** voice, u32* index = nullptr) {
    for (auto& [id, rack] : racks) {
        if (handle >= rack.first_voice && handle - rack.first_voice < rack.voices.size()) {
            const auto n = static_cast<u32>(handle - rack.first_voice);
            *voice = &rack.voices[n];
            if (index) *index = n;
            return &rack;
        }
    }
    return nullptr;
}
s32 InvokeAllocator(OrbisNgs2BufferAllocHandler callback, OrbisNgs2ContextBufferInfo& info) {
    if (!callback) return ORBIS_NGS2_ERROR_INVALID_BUFFER_ALLOCATOR;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    Core::GuestCpu::GuestCallbackScratch<OrbisNgs2ContextBufferInfo> scratch{info};
    if (!scratch.Data()) return ORBIS_NGS2_ERROR_UNABLE_CALLBACK;
    const std::array<u64, 1> args{reinterpret_cast<u64>(scratch.Data())};
    const auto result = Common::Singleton<Core::Linker>::Instance()->RunGuestFunction(
        reinterpret_cast<VAddr>(callback), args);
    if (std::holds_alternative<Core::GuestExecutionFailure>(result) || !scratch.Read(info))
        return ORBIS_NGS2_ERROR_UNABLE_CALLBACK;
    return static_cast<s32>(std::get<u64>(result));
#else
    return callback(&info);
#endif
}
s32 ValidateSystemOption(const OrbisNgs2SystemOption* p, OrbisNgs2SystemOption& o) {
    o = {sizeof(o), "", 0, 512, 256, 48000, {0}};
    if (p && !Read(p, o)) return ORBIS_NGS2_ERROR_INVALID_OPTION_ADDRESS;
    if (o.size != sizeof(o)) return ORBIS_NGS2_ERROR_INVALID_OPTION_SIZE;
    if (o.maxGrainSamples < 64 || o.maxGrainSamples > 1024 || o.maxGrainSamples % 64)
        return ORBIS_NGS2_ERROR_INVALID_MAX_GRAIN_SAMPLES;
    if (o.numGrainSamples < 64 || o.numGrainSamples > o.maxGrainSamples || o.numGrainSamples % 64)
        return ORBIS_NGS2_ERROR_INVALID_NUM_GRAIN_SAMPLES;
    constexpr std::array rates{11025u,12000u,22050u,24000u,44100u,48000u,88200u,96000u,176400u,192000u};
    if (std::find(rates.begin(),rates.end(),o.sampleRate)==rates.end()) return ORBIS_NGS2_ERROR_INVALID_SAMPLE_RATE;
    if (o.flags & 0x7fffffffu) return ORBIS_NGS2_ERROR_INVALID_OPTION_FLAG;
    return 0;
}
s32 ValidateRackOption(u32 id, const OrbisNgs2RackOption* p, OrbisNgs2RackOption& o) {
    if (!Read(p, o)) return ORBIS_NGS2_ERROR_INVALID_OPTION_ADDRESS;
    size_t expected{};
    switch (id) {
    case 0x1000: expected=sizeof(OrbisNgs2SamplerRackOption);break;
    case 0x2000: expected=sizeof(OrbisNgs2SubmixerRackOption);break;
    case 0x2001: expected=sizeof(OrbisNgs2ReverbRackOption);break;
    case 0x3000: expected=sizeof(OrbisNgs2MasteringRackOption);break;
    case 0x4002: expected=sizeof(OrbisNgs2CustomSubmixerRackOption);break;
    default:return ORBIS_NGS2_ERROR_INVALID_RACK_ID;
    }
    if (o.size != expected || !Range(p, expected, false)) return ORBIS_NGS2_ERROR_INVALID_OPTION_SIZE;
    if (!o.maxVoices || o.maxVoices > MaxVoices) return ORBIS_NGS2_ERROR_INVALID_MAX_VOICES;
    if (o.maxPorts > 64) return ORBIS_NGS2_ERROR_INVALID_MAX_PORTS;
    if (o.maxMatrices > 64) return ORBIS_NGS2_ERROR_INVALID_MAX_MATRICES;
    if (o.maxGrainSamples && (o.maxGrainSamples < 64 || o.maxGrainSamples > 1024 || o.maxGrainSamples % 64))
        return ORBIS_NGS2_ERROR_INVALID_MAX_GRAIN_SAMPLES;
    return 0;
}
s32 ValidateBacking(const OrbisNgs2ContextBufferInfo& info, size_t minimum) {
    if (!info.hostBuffer || !Range(info.hostBuffer, info.hostBufferSize, true)) return ORBIS_NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    if (reinterpret_cast<uintptr_t>(info.hostBuffer) % 8) return ORBIS_NGS2_ERROR_INVALID_BUFFER_ALIGN;
    if (info.hostBufferSize < minimum) return ORBIS_NGS2_ERROR_INVALID_BUFFER_SIZE;
    return 0;
}
s32 CreateSystem(const OrbisNgs2SystemOption& option, const OrbisNgs2ContextBufferInfo& backing,
                 OrbisNgs2BufferFreeHandler free, OrbisNgs2Handle* output) {
    if (!Range(output, sizeof(*output), true)) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (const auto error=ValidateBacking(backing,SystemBytes);error) return error;
    std::scoped_lock lock{registry_mutex};
    if (AliasesBacking(backing.hostBuffer,backing.hostBufferSize) || AliasesBacking(output,sizeof(*output)) ||
        Overlap(output,sizeof(*output),backing.hostBuffer,backing.hostBufferSize)) return ORBIS_NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    const auto handle=AllocateHandles(1);
    if (!handle) return ORBIS_NGS2_ERROR_FAIL;
    systems.emplace(handle,System{.option=option,.backing=backing,.free=free,.user=backing.userData});
    std::memcpy(output,&handle,sizeof(handle));
    return 0;
}
s32 CreateRack(OrbisNgs2Handle system, u32 id, const OrbisNgs2RackOption& option,
               const OrbisNgs2ContextBufferInfo& backing, OrbisNgs2BufferFreeHandler free,
               OrbisNgs2Handle* output) {
    if (!Range(output,sizeof(*output),true)) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (const auto error=ValidateBacking(backing,RackHeaderBytes+VoiceBytes*option.maxVoices);error) return error;
    std::scoped_lock lock{registry_mutex};
    const auto sys=systems.find(system);
    if (sys==systems.end()) return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    if (!CanAccess(sys->second)) return ORBIS_NGS2_ERROR_INVALID_OPERATION;
    if (AliasesBacking(backing.hostBuffer,backing.hostBufferSize) || AliasesBacking(output,sizeof(*output)) ||
        Overlap(output,sizeof(*output),backing.hostBuffer,backing.hostBufferSize)) return ORBIS_NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    Rack rack{.system=system,.id=id,.option=option,.backing=backing,.free=free,.user=backing.userData};
    rack.voices.resize(option.maxVoices);
    for (auto& voice:rack.voices) {
        voice.ports.resize(option.maxPorts,OrbisNgs2VoicePortInfo{-1,1.0f,0,0,0});
        voice.matrices.resize(option.maxMatrices);
    }
    const auto handle=AllocateHandles(static_cast<size_t>(option.maxVoices)+1);
    if (!handle) return ORBIS_NGS2_ERROR_FAIL;
    rack.first_voice=handle+1;
    racks.emplace(handle,std::move(rack));
    std::memcpy(output,&handle,sizeof(handle));
    return 0;
}
}

#ifndef LUMEN_GEN4_DIRECT_FEX
s32 PS4_SYSV_ABI sceNgs2SystemResetOption(OrbisNgs2SystemOption* outOption) {
    const OrbisNgs2SystemOption option{sizeof(option),"",0,512,256,48000,{0}};
    return Write(outOption,option)?0:ORBIS_NGS2_ERROR_INVALID_OPTION_ADDRESS;
}
#endif
s32 PS4_SYSV_ABI sceNgs2SystemQueryBufferSize(const OrbisNgs2SystemOption* option, OrbisNgs2ContextBufferInfo* out) {
    OrbisNgs2SystemOption owned{};
    if (auto error=ValidateSystemOption(option,owned);error) return error;
    const OrbisNgs2ContextBufferInfo info{nullptr,SystemBytes,{0},0};
    return Write(out,info)?0:ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
}
s32 PS4_SYSV_ABI sceNgs2RackQueryBufferSize(u32 id,const OrbisNgs2RackOption* option,OrbisNgs2ContextBufferInfo* out) {
    OrbisNgs2RackOption owned{};
    if (auto error=ValidateRackOption(id,option,owned);error) return error;
    const OrbisNgs2ContextBufferInfo info{nullptr,RackHeaderBytes+VoiceBytes*owned.maxVoices,{0},0};
    return Write(out,info)?0:ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
}
s32 PS4_SYSV_ABI sceNgs2SystemCreate(const OrbisNgs2SystemOption* option,const OrbisNgs2ContextBufferInfo* info,OrbisNgs2Handle* out) {
    OrbisNgs2SystemOption owned{};OrbisNgs2ContextBufferInfo backing{};
    if (auto error=ValidateSystemOption(option,owned);error) return error;
    if (!Read(info,backing)) return ORBIS_NGS2_ERROR_INVALID_BUFFER_INFO;
    return CreateSystem(owned,backing,nullptr,out);
}
s32 PS4_SYSV_ABI sceNgs2RackCreate(OrbisNgs2Handle system,u32 id,const OrbisNgs2RackOption* option,const OrbisNgs2ContextBufferInfo* info,OrbisNgs2Handle* out) {
    OrbisNgs2RackOption owned{};OrbisNgs2ContextBufferInfo backing{};
    if (auto error=ValidateRackOption(id,option,owned);error) return error;
    if (!Read(info,backing)) return ORBIS_NGS2_ERROR_INVALID_BUFFER_INFO;
    return CreateRack(system,id,owned,backing,nullptr,out);
}
s32 PS4_SYSV_ABI sceNgs2SystemCreateWithAllocator(const OrbisNgs2SystemOption* option,const OrbisNgs2BufferAllocator* allocator,OrbisNgs2Handle* out) {
    OrbisNgs2SystemOption owned{};OrbisNgs2BufferAllocator alloc{};
    if (auto error=ValidateSystemOption(option,owned);error) return error;
    if (!Read(allocator,alloc) || !alloc.allocHandler || !alloc.freeHandler) return ORBIS_NGS2_ERROR_INVALID_BUFFER_ALLOCATOR;
    if (!Range(out,sizeof(*out),true)) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    OrbisNgs2ContextBufferInfo backing{nullptr,SystemBytes,{0},alloc.userData};
    if (auto error=InvokeAllocator(alloc.allocHandler,backing);error<0) return error;
    const auto result=CreateSystem(owned,backing,alloc.freeHandler,out);
    if (result<0) InvokeAllocator(alloc.freeHandler,backing);
    return result;
}
s32 PS4_SYSV_ABI sceNgs2RackCreateWithAllocator(OrbisNgs2Handle system,u32 id,const OrbisNgs2RackOption* option,const OrbisNgs2BufferAllocator* allocator,OrbisNgs2Handle* out) {
    OrbisNgs2RackOption owned{};OrbisNgs2BufferAllocator alloc{};
    if (auto error=ValidateRackOption(id,option,owned);error) return error;
    if (!Read(allocator,alloc) || !alloc.allocHandler || !alloc.freeHandler) return ORBIS_NGS2_ERROR_INVALID_BUFFER_ALLOCATOR;
    if (!Range(out,sizeof(*out),true)) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    {std::scoped_lock lock{registry_mutex};if (!systems.contains(system)) return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;}
    OrbisNgs2ContextBufferInfo backing{nullptr,RackHeaderBytes+VoiceBytes*owned.maxVoices,{0},alloc.userData};
    if (auto error=InvokeAllocator(alloc.allocHandler,backing);error<0) return error;
    const auto result=CreateRack(system,id,owned,backing,alloc.freeHandler,out);
    if (result<0) InvokeAllocator(alloc.freeHandler,backing);
    return result;
}
s32 PS4_SYSV_ABI sceNgs2RackDestroy(OrbisNgs2Handle handle,OrbisNgs2ContextBufferInfo* out) {
    if (out && !Range(out,sizeof(*out),true)) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    OrbisNgs2ContextBufferInfo backing{};OrbisNgs2BufferFreeHandler free{};
    {
        std::scoped_lock lock{registry_mutex};const auto it=racks.find(handle);
        if (it==racks.end()) return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
        if (!CanAccess(it->second) || !CanAccess(systems.at(it->second.system))) return ORBIS_NGS2_ERROR_INVALID_OPERATION;
        if (out && AliasesBacking(out,sizeof(*out))) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
        backing=it->second.backing;free=it->second.free;racks.erase(it);
    }
    if (out) std::memcpy(out,&backing,sizeof(backing));
    return free?InvokeAllocator(free,backing):0;
}
s32 PS4_SYSV_ABI sceNgs2SystemDestroy(OrbisNgs2Handle handle,OrbisNgs2ContextBufferInfo* out) {
    if (out && !Range(out,sizeof(*out),true)) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::vector<std::pair<OrbisNgs2ContextBufferInfo,OrbisNgs2BufferFreeHandler>> release;
    OrbisNgs2ContextBufferInfo backing{};OrbisNgs2BufferFreeHandler free{};
    {
        std::scoped_lock lock{registry_mutex};const auto it=systems.find(handle);
        if (it==systems.end()) return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
        if (!CanAccess(it->second)) return ORBIS_NGS2_ERROR_INVALID_OPERATION;
        if (out && AliasesBacking(out,sizeof(*out))) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
        for (const auto& [id,rack]:racks) if (rack.system==handle && !CanAccess(rack)) return ORBIS_NGS2_ERROR_INVALID_OPERATION;
        for (auto r=racks.begin();r!=racks.end();) {
            if (r->second.system==handle) {release.emplace_back(r->second.backing,r->second.free);r=racks.erase(r);}else ++r;
        }
        backing=it->second.backing;free=it->second.free;systems.erase(it);
    }
    if (out) std::memcpy(out,&backing,sizeof(backing));
    s32 result=0;
    for (auto& [buffer,callback]:release) if (callback) {auto e=InvokeAllocator(callback,buffer);if (e<0) result=e;}
    if (free) {auto e=InvokeAllocator(free,backing);if (e<0) result=e;}
    return result;
}
s32 PS4_SYSV_ABI sceNgs2RackGetVoiceHandle(OrbisNgs2Handle handle,u32 index,OrbisNgs2Handle* out) {
    if (!Range(out,sizeof(*out),true)) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::scoped_lock lock{registry_mutex};const auto it=racks.find(handle);
    if (it==racks.end()) return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
    if (index>=it->second.voices.size()) return ORBIS_NGS2_ERROR_INVALID_VOICE_INDEX;
    if (AliasesBacking(out,sizeof(*out))) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    const auto token=it->second.first_voice+index;std::memcpy(out,&token,sizeof(token));return 0;
}

static s32 VoiceControl(OrbisNgs2Handle handle,const OrbisNgs2VoiceParamHeader* list,
                        OrbisNgs2VoiceParamHeader* observed) {
    struct Command {OrbisNgs2VoiceParamHeader header;std::vector<u8> bytes;std::vector<float> levels;Detail::SamplerPacket packet;};
    std::vector<Command> commands;
    auto address=reinterpret_cast<uintptr_t>(list);
    size_t encoded_total=0;
    for (u32 n=0;;++n) {
        if (n==4096) return ORBIS_NGS2_ERROR_DETECTED_CIRCULAR_VOICE_CONTROL;
        Command cmd{};
        if (!Read(reinterpret_cast<const OrbisNgs2VoiceParamHeader*>(address),cmd.header)) return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ADDRESS;
        if (observed) *observed = cmd.header;
        if (cmd.header.size<sizeof(cmd.header) || cmd.header.size>4096 || !Range(reinterpret_cast<void*>(address),cmd.header.size,false))
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
        cmd.bytes.resize(cmd.header.size);std::memcpy(cmd.bytes.data(),reinterpret_cast<void*>(address),cmd.bytes.size());
        OrbisNgs2VoiceParamHeader stable{};std::memcpy(&stable,cmd.bytes.data(),sizeof(stable));
        if (std::memcmp(&stable,&cmd.header,sizeof(stable))) return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ADDRESS;
        if (!MixerEnabled() && cmd.header.id==1 && cmd.header.size==sizeof(OrbisNgs2VoiceMatrixLevelsParam)) {
            OrbisNgs2VoiceMatrixLevelsParam p{};std::memcpy(&p,cmd.bytes.data(),sizeof(p));
            if (p.numLevels>ORBIS_NGS2_MAX_MATRIX_LEVELS || (p.numLevels && !Range(p.aLevel,p.numLevels*sizeof(float),false)))
                return ORBIS_NGS2_ERROR_INVALID_MATRIX_LEVEL_ADDRESS;
            cmd.levels.resize(p.numLevels);if (p.numLevels) std::memcpy(cmd.levels.data(),p.aLevel,p.numLevels*sizeof(float));
        }
        if (MixerEnabled() && cmd.header.id==0x10000001) {
            // Original Lumen packet ABI: descriptor count is at +0x10. The
            // adjacent +0x14 field remains opaque and is not a block count.
            struct BlockParam {OrbisNgs2VoiceParamHeader header;const void* data;u32 count,opaque;const void* descriptors;};
            struct Descriptor {u32 offset,size,flags,skip;u64 samples,user;};
            static_assert(sizeof(BlockParam)==32 && sizeof(Descriptor)==32);
            if(cmd.bytes.size()!=sizeof(BlockParam))return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
            BlockParam p{};std::memcpy(&p,cmd.bytes.data(),sizeof(p));Descriptor d{};
            if(p.count!=1)return ORBIS_NGS2_ERROR_INVALID_NUM_WAVEFORM_BLOCKS;
            if(!Read(static_cast<const Descriptor*>(p.descriptors),d))return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_BLOCK_ADDRESS;
            const auto base=reinterpret_cast<uintptr_t>(p.data);
            if(!base||base>UINTPTR_MAX-d.offset||!d.size||d.size>Detail::MaxEncodedBytes-encoded_total||d.flags)return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_DATA;
            const auto* encoded=reinterpret_cast<const u8*>(base+d.offset);
            if(!Range(encoded,d.size,false)||Overlap(encoded,d.size,p.descriptors,sizeof(d))||Overlap(encoded,d.size,reinterpret_cast<void*>(address),cmd.bytes.size())||Overlap(p.descriptors,sizeof(d),reinterpret_cast<void*>(address),cmd.bytes.size()))return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS;
            cmd.packet.encoded.assign(encoded,encoded+d.size);encoded_total+=d.size;
            cmd.packet.skip=d.skip;cmd.packet.playable=d.samples;cmd.packet.identity=base;cmd.packet.user=d.user;
        }
        const s16 next=cmd.header.next;commands.push_back(std::move(cmd));
        if (!next) break;
        if (next<static_cast<s16>(sizeof(OrbisNgs2VoiceParamHeader)) || address>UINTPTR_MAX-static_cast<u16>(next))
            return ORBIS_NGS2_ERROR_DETECTED_CIRCULAR_VOICE_CONTROL;
        address+=static_cast<u16>(next);
    }
    std::scoped_lock lock{registry_mutex};Voice* voice{};Rack* rack=FindVoice(handle,&voice);
    if (!rack) return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    if (!CanAccess(*rack) || !CanAccess(systems.at(rack->system))) return ORBIS_NGS2_ERROR_INVALID_OPERATION;
    Voice updated=*voice;
    for (const auto& cmd:commands) {
        if (observed) *observed = cmd.header;
        if (cmd.header.id&0x8000u) return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID;
        const auto owner=cmd.header.id>>16;const auto id=cmd.header.id&0x7fffu;
        if (owner) {
            if (owner!=rack->id && !(owner==0x4000 && rack->id==0x4002)) return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID;
            if (MixerEnabled() && owner==0x1000) {
                if(id==0) {
                    if(cmd.bytes.size()!=sizeof(OrbisNgs2SamplerVoiceSetupParam))return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
                    OrbisNgs2SamplerVoiceSetupParam setup{};std::memcpy(&setup,cmd.bytes.data(),sizeof(setup));
                    std::array<u32,4> geometry{};
                    if(setup.flags||setup.reserved||setup.format.waveformType!=0x40)return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
                    if(auto error=FrameInfo(setup.format,geometry);error)return error;
                    updated.waveform=setup.format;updated.sampler={};
                } else if(id==1) {
                    if(!updated.sampler.Append(updated.waveform,cmd.packet))return ORBIS_NGS2_ERROR_CODEC_DECODE_FAIL;
                } else {
                    // These envelopes were observed alongside format/refill.
                    // Their bodies have no qualified gain, loop or routing
                    // semantics in this opt-in mixer and remain opaque.
                    size_t expected{};
                    switch (id) {
                    case 5: expected = 16; break;
                    case 6: expected = 24; break;
                    case 7: expected = 40; break;
                    case 0xa: expected = 48; break;
                    default: return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID;
                    }
                    if (cmd.bytes.size() != expected)
                        return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
                }
            }
            continue;
        }
        // The opt-in mixer has no matrix/volume routing. Preserve the qualified
        // common envelopes without interpreting their bodies or dereferencing
        // pointer-shaped data as matrix storage.
        if (MixerEnabled() && (id == 1 || id == 3)) {
            const size_t expected = id == 1 ? 24 : 16;
            if (cmd.bytes.size() != expected)
                return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
            continue;
        }
        switch(id) {
        case 1: {
            if (cmd.header.size!=sizeof(OrbisNgs2VoiceMatrixLevelsParam)) return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
            OrbisNgs2VoiceMatrixLevelsParam p{};std::memcpy(&p,cmd.bytes.data(),sizeof(p));
            if (p.matrixId>=updated.matrices.size()) return ORBIS_NGS2_ERROR_INVALID_MATRIX_INDEX;
            auto& matrix=updated.matrices[p.matrixId];matrix={};matrix.numLevels=p.numLevels;
            std::copy(cmd.levels.begin(),cmd.levels.end(),matrix.aLevel);break;
        }
        case 2: {
            if (cmd.header.size!=sizeof(OrbisNgs2VoicePortMatrixParam)) return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
            OrbisNgs2VoicePortMatrixParam p{};std::memcpy(&p,cmd.bytes.data(),sizeof(p));
            if (p.port>=updated.ports.size()) return ORBIS_NGS2_ERROR_INVALID_PORT_INDEX;
            // The qualified local mixer keeps this signed selection as port
            // metadata. It does not route through the optional user matrix
            // table, so requiring an entry there rejects valid initialization
            // sequences on racks with no user matrices.
            if (!MixerEnabled() && (p.matrixId < -1 ||
                (p.matrixId>=0 && static_cast<u32>(p.matrixId)>=updated.matrices.size())))
                return ORBIS_NGS2_ERROR_INVALID_MATRIX_INDEX;
            updated.ports[p.port].matrixId=p.matrixId;break;
        }
        case 3: {
            if (cmd.header.size!=sizeof(OrbisNgs2VoicePortVolumeParam)) return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
            OrbisNgs2VoicePortVolumeParam p{};std::memcpy(&p,cmd.bytes.data(),sizeof(p));
            if (p.port>=updated.ports.size()) return ORBIS_NGS2_ERROR_INVALID_PORT_INDEX;
            if (!std::isfinite(p.level)) return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
            updated.ports[p.port].volume=p.level;break;
        }
        case 4: {
            if (cmd.header.size!=sizeof(OrbisNgs2VoicePortDelayParam)) return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
            OrbisNgs2VoicePortDelayParam p{};std::memcpy(&p,cmd.bytes.data(),sizeof(p));
            if (p.port>=updated.ports.size()) return ORBIS_NGS2_ERROR_INVALID_PORT_INDEX;
            updated.ports[p.port].numDelaySamples=p.numSamples;break;
        }
        case 5: {
            if (cmd.header.size!=sizeof(OrbisNgs2VoicePatchParam)) return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
            OrbisNgs2VoicePatchParam p{};std::memcpy(&p,cmd.bytes.data(),sizeof(p));
            if (p.port>=updated.ports.size()) return ORBIS_NGS2_ERROR_INVALID_PORT_INDEX;
            if (p.destHandle) {Voice* destination{};const auto target=FindVoice(p.destHandle,&destination);
                if (!target || target->system!=rack->system) return ORBIS_NGS2_ERROR_INVALID_PATCH;}
            updated.ports[p.port].destHandle=p.destHandle;updated.ports[p.port].destInputId=p.destInputId;break;
        }
        case 6: {
            if (cmd.header.size!=sizeof(OrbisNgs2VoiceEventParam)) return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_SIZE;
            OrbisNgs2VoiceEventParam p{};std::memcpy(&p,cmd.bytes.data(),sizeof(p));
            if (p.eventId>5) return ORBIS_NGS2_ERROR_INVALID_EVENT_TYPE;
            updated.event=p.eventId;break;
        }
        default:return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID;
        }
    }
    *voice=std::move(updated);return 0;
}
s32 PS4_SYSV_ABI sceNgs2VoiceControl(OrbisNgs2Handle handle,
                                   const OrbisNgs2VoiceParamHeader* list) {
    const bool trace = AudioTraceEnabled();
    OrbisNgs2VoiceParamHeader observed{};
    const auto result = VoiceControl(handle, list, trace ? &observed : nullptr);
    if (trace) {
        // Keep initialization events from exhausting the sampler/refill or
        // matrix-selection evidence; the number of buckets remains bounded.
        static std::array<std::atomic<u32>, 19> successes{};
        static std::atomic<u32> errors{};
        const u32 owner = observed.id >> 16, id = observed.id & 0xffff;
        const size_t bucket = owner == 0 && id <= 6 ? id :
                              owner == 0x1000 && id <= 10 ? 7 + id : 18;
        auto& count = result == 0 ? successes[bucket] : errors;
        if (count.fetch_add(1, std::memory_order_relaxed) < (result == 0 ? 4u : 32u)) {
            std::fprintf(stderr, "[NGS2_AUDIO_CONTROL] result=%#x id=%#x size=%u mixer=%u\n",
                         static_cast<u32>(result), observed.id,
                         static_cast<unsigned>(observed.size),
                         static_cast<unsigned>(MixerEnabled()));
        }
    }
    return result;
}
s32 PS4_SYSV_ABI sceNgs2VoiceGetStateFlags(OrbisNgs2Handle handle,u32* out) {
    if (!Range(out,sizeof(*out),true)) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::scoped_lock lock{registry_mutex};Voice* voice{};
    if (!FindVoice(handle,&voice)) return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    if (AliasesBacking(out,sizeof(*out))) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    const auto state = QueryState(*voice);
    std::memcpy(out,&state,sizeof(*out));return 0;
}
s32 PS4_SYSV_ABI sceNgs2VoiceGetState(OrbisNgs2Handle handle,OrbisNgs2VoiceState* out,size_t size) {
    if (!Range(out,size,true)) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::scoped_lock lock{registry_mutex};Voice* voice{};const auto rack=FindVoice(handle,&voice);
    if (!rack) return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    const auto expected=rack->id==0x1000?sizeof(OrbisNgs2SamplerVoiceState):rack->id==0x3000?sizeof(OrbisNgs2MasteringVoiceState):sizeof(OrbisNgs2VoiceState);
    if (size!=expected) return ORBIS_NGS2_ERROR_INVALID_VOICE_STATE_SIZE;
    if (AliasesBacking(out,size)) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::array<u8,sizeof(OrbisNgs2MasteringVoiceState)> data{};
    if (rack->id==0x1000) {
        OrbisNgs2SamplerVoiceState state{};state.voiceState.stateFlags=QueryState(*voice);state.envelopeHeight=1.0f;
        if(MixerEnabled()&&!voice->sampler.blocks.empty()) {
            const auto& block=voice->sampler.blocks.front();
            state.waveformData=reinterpret_cast<const void*>(block.identity);
            // The owned PCM size and opaque packet metadata are not guest
            // progress counters. Keep those unqualified fields zero.
        }
        std::memcpy(data.data(),&state,sizeof(state));
    }else {
        const auto state = QueryState(*voice);
        std::memcpy(data.data(),&state,sizeof(state));
    }
    std::memcpy(out,data.data(),size);return 0;
}
s32 PS4_SYSV_ABI sceNgs2VoiceGetOwner(OrbisNgs2Handle handle,OrbisNgs2Handle* out,u32* index) {
    if ((!out&&!index) || (out&&!Range(out,sizeof(*out),true)) || (index&&!Range(index,sizeof(*index),true))) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::scoped_lock lock{registry_mutex};Voice* voice{};u32 n{};const auto owner=FindVoice(handle,&voice,&n);
    if (!owner) return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    if ((out&&AliasesBacking(out,sizeof(*out))) || (index&&AliasesBacking(index,sizeof(*index)))) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    const auto rack_handle=owner->first_voice-1;if (out)std::memcpy(out,&rack_handle,sizeof(*out));if(index)std::memcpy(index,&n,sizeof(n));return 0;
}
s32 PS4_SYSV_ABI sceNgs2VoiceGetPortInfo(OrbisNgs2Handle handle,u32 port,OrbisNgs2VoicePortInfo* out,size_t size) {
    if (size!=sizeof(*out)) return ORBIS_NGS2_ERROR_INVALID_OUT_SIZE;
    if (!Range(out,size,true)) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::scoped_lock lock{registry_mutex};Voice* voice{};if (!FindVoice(handle,&voice)) return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    if (port>=voice->ports.size()) return ORBIS_NGS2_ERROR_INVALID_PORT_INDEX;
    if (AliasesBacking(out,size)) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::memcpy(out,&voice->ports[port],size);return 0;
}
s32 PS4_SYSV_ABI sceNgs2VoiceGetMatrixInfo(OrbisNgs2Handle handle,u32 matrix,OrbisNgs2VoiceMatrixInfo* out,size_t size) {
    if (size!=sizeof(*out)) return ORBIS_NGS2_ERROR_INVALID_OUT_SIZE;
    if (!Range(out,size,true)) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::scoped_lock lock{registry_mutex};Voice* voice{};if (!FindVoice(handle,&voice)) return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    if (matrix>=voice->matrices.size()) return ORBIS_NGS2_ERROR_INVALID_MATRIX_INDEX;
    if (AliasesBacking(out,size)) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::memcpy(out,&voice->matrices[matrix],size);return 0;
}
namespace {
u32 PcmSize(u32 type) {
    switch(type) {case 0x10:case 0x11:return 1;case 0x12:case 0x13:return 2;
        case 0x14:case 0x15:return 3;case 0x16:case 0x17:case 0x18:case 0x19:return 4;
        case 0x1a:case 0x1b:return 8;default:return 0;}
}
void Advance(Voice& v) {
    switch(v.event) {
    case ~0u:if(v.state==3||v.state==0xb)v.state=0;break;
    case 0:if(v.state==0)v.state=3;break;
    case 1:if(v.state==3)v.state=0xb;break;
    case 2:case 3:v.state=0;break;
    case 4:if(v.state==3)v.state=5;break;
    case 5:if(v.state==5)v.state=3;break;
    }
    v.event=~0u;
}
}
static s32 RenderSystem(OrbisNgs2Handle handle,const OrbisNgs2RenderBufferInfo* inputs,u32 count) {
    if (!count || count>64 || !Range(inputs,count*sizeof(*inputs),false)) return ORBIS_NGS2_ERROR_INVALID_BUFFER_INFO;
    std::vector<OrbisNgs2RenderBufferInfo> buffers(count);std::memcpy(buffers.data(),inputs,count*sizeof(*inputs));
    for(const auto& b:buffers) if(!b.numChannels||b.numChannels>8||!PcmSize(b.waveformType)||!Range(b.buffer,b.bufferSize,true)) return ORBIS_NGS2_ERROR_INVALID_BUFFER_INFO;
    std::scoped_lock lock{registry_mutex};auto it=systems.find(handle);
    if(it==systems.end()) return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    if(!CanAccess(it->second)) return ORBIS_NGS2_ERROR_INVALID_OPERATION;
    for(const auto& b:buffers) {
        const size_t bytes=static_cast<size_t>(it->second.option.numGrainSamples)*b.numChannels*PcmSize(b.waveformType);
        if(b.bufferSize<bytes||AliasesBacking(b.buffer,bytes))return ORBIS_NGS2_ERROR_INVALID_BUFFER_SIZE;
    }
    for(const auto& [id,rack]:racks) if(rack.system==handle&&!CanAccess(rack))return ORBIS_NGS2_ERROR_INVALID_OPERATION;
    if(MixerEnabled()) {
        if(count!=1||it->second.option.numGrainSamples!=512||it->second.option.sampleRate!=48000||buffers[0].waveformType!=0x18||buffers[0].numChannels!=8||buffers[0].bufferSize!=512*8*sizeof(float)||Overlap(inputs,count*sizeof(*inputs),buffers[0].buffer,buffers[0].bufferSize))return ORBIS_NGS2_ERROR_INVALID_BUFFER_INFO;
        std::vector<float> mix(512*8);
        for(auto& [id,rack]:racks)if(rack.system==handle)for(auto& voice:rack.voices){
            voice.sampler.Event(voice.event,voice.state,voice.waveform.numChannels);voice.event=~0u;
            if(rack.id==0x1000)voice.sampler.Mix(voice.state,voice.waveform.numChannels,mix,8);
        }
        for(auto& sample:mix)sample=std::isfinite(sample)?std::clamp(sample,-1.0f,1.0f):0.0f;
        std::memcpy(buffers[0].buffer,mix.data(),mix.size()*sizeof(float));
        if (AudioTraceEnabled()) {
            float peak{};
            for (const float sample : mix) peak = std::max(peak, std::fabs(sample));
            auto& system = it->second;
            system.nonzero_renders += peak > 0;
            system.peak = std::max(system.peak, peak);
            const u64 count = system.renders + 1;
            if (count <= 4 || (count <= 16384 && (count & (count - 1)) == 0)) {
                std::fprintf(stderr, "[NGS2_AUDIO_RENDER] grains=%llu nonzero=%llu peak=%.9g current_peak=%.9g\n",
                             static_cast<unsigned long long>(count),
                             static_cast<unsigned long long>(system.nonzero_renders), system.peak, peak);
            }
        }
    } else {
        for(auto& [id,rack]:racks) if(rack.system==handle) for(auto& voice:rack.voices) Advance(voice);
        for(const auto& b:buffers)std::memset(b.buffer,0,static_cast<size_t>(it->second.option.numGrainSamples)*b.numChannels*PcmSize(b.waveformType));
    }
    ++it->second.renders;return 0;
}
s32 PS4_SYSV_ABI sceNgs2SystemRender(OrbisNgs2Handle handle,
                                   const OrbisNgs2RenderBufferInfo* inputs,u32 count) {
    const auto result = RenderSystem(handle, inputs, count);
    if (result != 0 && AudioTraceEnabled()) {
        static std::atomic<u32> errors{};
        if (errors.fetch_add(1, std::memory_order_relaxed) < 8) {
            std::fprintf(stderr, "[NGS2_AUDIO_RENDER_ERROR] result=%#x buffers=%u\n",
                         static_cast<u32>(result), count);
        }
    }
    return result;
}

s32 PS4_SYSV_ABI sceNgs2SystemGetInfo(OrbisNgs2Handle handle, OrbisNgs2SystemInfo* out, size_t size) {
    if (size != sizeof(*out)) return ORBIS_NGS2_ERROR_INVALID_OUT_SIZE;
    if (!Range(out,size,true)) return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::scoped_lock lock{registry_mutex};const auto it=systems.find(handle);
    if(it==systems.end()) return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    if(AliasesBacking(out,size))return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    const auto& s=it->second;OrbisNgs2SystemInfo info{};
    std::memcpy(info.name,s.option.name,sizeof(info.name));info.systemHandle=handle;info.bufferInfo=s.backing;
    info.uid=static_cast<u32>(handle);info.minGrainSamples=64;info.maxGrainSamples=s.option.maxGrainSamples;
    info.stateFlags=s.lock_depth?1:0;info.renderCount=s.renders;info.sampleRate=s.option.sampleRate;info.numGrainSamples=s.option.numGrainSamples;
    for(const auto& [id,r]:racks)if(r.system==handle)++info.rackCount;
    std::memcpy(out,&info,size);return 0;
}
s32 PS4_SYSV_ABI sceNgs2RackGetInfo(OrbisNgs2Handle handle, OrbisNgs2RackInfo* out, size_t size) {
    if(size!=sizeof(*out))return ORBIS_NGS2_ERROR_INVALID_OUT_SIZE;
    if(!Range(out,size,true))return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::scoped_lock lock{registry_mutex};const auto it=racks.find(handle);
    if(it==racks.end())return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
    if(AliasesBacking(out,size))return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    const auto& r=it->second;OrbisNgs2RackInfo info{};
    std::memcpy(info.name,r.option.name,sizeof(info.name));info.rackHandle=handle;info.bufferInfo=r.backing;
    info.ownerSystemHandle=r.system;info.type=r.id;info.rackId=r.id;info.uid=static_cast<u32>(handle);
    info.minGrainSamples=64;info.maxGrainSamples=r.option.maxGrainSamples?r.option.maxGrainSamples:systems.at(r.system).option.maxGrainSamples;
    info.maxVoices=r.option.maxVoices;info.maxMatrices=r.option.maxMatrices;info.maxPorts=r.option.maxPorts;
    info.stateFlags=r.lock_depth?1:0;info.renderCount=systems.at(r.system).renders;
    for(const auto& v:r.voices)if(v.state==3)++info.activeVoiceCount;
    std::memcpy(out,&info,size);return 0;
}
s32 PS4_SYSV_ABI sceNgs2SystemEnumHandles(OrbisNgs2Handle* out,u32 capacity) {
    if(capacity && (!Range(out,static_cast<size_t>(capacity)*sizeof(*out),true)))return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::scoped_lock lock{registry_mutex};if(capacity&&AliasesBacking(out,static_cast<size_t>(capacity)*sizeof(*out)))return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    u32 index=0;for(const auto& [handle,s]:systems){if(index<capacity)std::memcpy(out+index,&handle,sizeof(handle));++index;}return static_cast<s32>(index);
}
s32 PS4_SYSV_ABI sceNgs2SystemEnumRackHandles(OrbisNgs2Handle system,OrbisNgs2Handle* out,u32 capacity) {
    if(capacity&&!Range(out,static_cast<size_t>(capacity)*sizeof(*out),true))return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::scoped_lock lock{registry_mutex};if(!systems.contains(system))return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    if(capacity&&AliasesBacking(out,static_cast<size_t>(capacity)*sizeof(*out)))return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    u32 index=0;for(const auto& [handle,r]:racks)if(r.system==system){if(index<capacity)std::memcpy(out+index,&handle,sizeof(handle));++index;}return static_cast<s32>(index);
}
namespace {
s32 UserData(bool system,OrbisNgs2Handle handle,uintptr_t* out,uintptr_t value,bool set) {
    if(!set&&!Range(out,sizeof(*out),true))return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::scoped_lock lock{registry_mutex};uintptr_t* data{};
    if(system){auto it=systems.find(handle);if(it==systems.end())return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;if(!CanAccess(it->second))return ORBIS_NGS2_ERROR_INVALID_OPERATION;data=&it->second.user;}
    else {auto it=racks.find(handle);if(it==racks.end())return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;if(!CanAccess(it->second)||!CanAccess(systems.at(it->second.system)))return ORBIS_NGS2_ERROR_INVALID_OPERATION;data=&it->second.user;}
    if(set)*data=value;else{if(AliasesBacking(out,sizeof(*out)))return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;std::memcpy(out,data,sizeof(*out));}return 0;
}
s32 LockContext(bool system,OrbisNgs2Handle handle,bool acquire) {
    std::scoped_lock lock{registry_mutex};std::thread::id* owner{};u32* depth{};
    if(system){auto it=systems.find(handle);if(it==systems.end())return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;owner=&it->second.lock_owner;depth=&it->second.lock_depth;}
    else {auto it=racks.find(handle);if(it==racks.end())return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;if(!CanAccess(systems.at(it->second.system)))return ORBIS_NGS2_ERROR_INVALID_OPERATION;owner=&it->second.lock_owner;depth=&it->second.lock_depth;}
    const auto self=std::this_thread::get_id();
    if(acquire){if((*depth&&*owner!=self)||*depth==UINT32_MAX)return ORBIS_NGS2_ERROR_INVALID_OPERATION;*owner=self;++*depth;}
    else {if(!*depth||*owner!=self)return ORBIS_NGS2_ERROR_INVALID_OPERATION;if(!--*depth)*owner={};}return 0;
}
}
s32 PS4_SYSV_ABI sceNgs2SystemGetUserData(OrbisNgs2Handle h,uintptr_t* p){return UserData(true,h,p,0,false);}
s32 PS4_SYSV_ABI sceNgs2RackGetUserData(OrbisNgs2Handle h,uintptr_t* p){return UserData(false,h,p,0,false);}
s32 PS4_SYSV_ABI sceNgs2SystemSetUserData(OrbisNgs2Handle h,uintptr_t v){return UserData(true,h,nullptr,v,true);}
s32 PS4_SYSV_ABI sceNgs2RackSetUserData(OrbisNgs2Handle h,uintptr_t v){return UserData(false,h,nullptr,v,true);}
s32 PS4_SYSV_ABI sceNgs2SystemLock(OrbisNgs2Handle h){return LockContext(true,h,true);}
s32 PS4_SYSV_ABI sceNgs2SystemUnlock(OrbisNgs2Handle h){return LockContext(true,h,false);}
s32 PS4_SYSV_ABI sceNgs2RackLock(OrbisNgs2Handle h){return LockContext(false,h,true);}
s32 PS4_SYSV_ABI sceNgs2RackUnlock(OrbisNgs2Handle h){return LockContext(false,h,false);}
s32 PS4_SYSV_ABI sceNgs2SystemSetGrainSamples(OrbisNgs2Handle handle,u32 samples) {
    std::scoped_lock lock{registry_mutex};auto it=systems.find(handle);if(it==systems.end())return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    if(!CanAccess(it->second))return ORBIS_NGS2_ERROR_INVALID_OPERATION;
    if(samples<64||samples>it->second.option.maxGrainSamples||samples%64)return ORBIS_NGS2_ERROR_INVALID_NUM_GRAIN_SAMPLES;
    it->second.option.numGrainSamples=samples;return 0;
}
s32 PS4_SYSV_ABI sceNgs2SystemSetSampleRate(OrbisNgs2Handle handle,u32 rate) {
    std::scoped_lock lock{registry_mutex};auto it=systems.find(handle);if(it==systems.end())return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    if(!CanAccess(it->second))return ORBIS_NGS2_ERROR_INVALID_OPERATION;
    // The copied option is native-owned; validation here must not classify it as guest memory.
    constexpr std::array rates{11025u,12000u,22050u,24000u,44100u,48000u,88200u,96000u,176400u,192000u};
    if(std::find(rates.begin(),rates.end(),rate)==rates.end())return ORBIS_NGS2_ERROR_INVALID_SAMPLE_RATE;
    it->second.option.sampleRate=rate;return 0;
}

namespace {
s32 FrameInfo(const OrbisNgs2WaveformFormat& format,std::array<u32,4>& result) {
    if(format.numChannels!=1&&format.numChannels!=2&&format.numChannels!=6&&format.numChannels!=8)return ORBIS_NGS2_ERROR_INVALID_NUM_CHANNELS;
    if(!format.sampleRate)return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SAMPLE_RATE;
    if(format.frameOffset||format.frameMargin)return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_FRAME;
    if(const auto bytes=PcmSize(format.waveformType)) {
        if(format.configData)return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_CONFIG;
        result={bytes*format.numChannels,1,1,0};return 0;
    }
    if(format.waveformType!=0x40)return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_TYPE;
    const u32 config=format.configData;
    if((config>>24)!=0xfe||((config>>17)&7)>5||(config&0x10007)||(((config>>3)&3)!=0&&((config>>3)&3)!=2))return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_CONFIG;
    std::array<unsigned char,4> bytes{static_cast<u8>(config>>24),static_cast<u8>(config>>16),static_cast<u8>(config>>8),static_cast<u8>(config)};
    std::scoped_lock library_lock{Ajm::Atrac9LibraryMutex()};
    std::unique_ptr<void,decltype(&Atrac9ReleaseHandle)> decoder(Atrac9GetHandle(),Atrac9ReleaseHandle);
    if(!decoder)return ORBIS_NGS2_ERROR_CODEC_EMPTY_INSTANCE;
    if(Atrac9InitDecoder(decoder.get(),bytes.data()))return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_CONFIG;
    Atrac9CodecInfo info{};if(Atrac9GetCodecInfo(decoder.get(),&info))return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_CONFIG;
    if(info.channels!=format.numChannels || (info.samplingRate!=format.sampleRate && !(info.samplingRate==24000&&format.sampleRate==48000)))return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SAMPLE_RATE;
    if(info.superframeSize<=0||info.superframeSize>8192||(info.framesInSuperframe!=1&&info.framesInSuperframe!=4))return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_CONFIG;
    constexpr std::array<u32,16> samples{0,64,0,0,128,0,0,256,0,0,0,0,0,0,0,0};const u32 unit=samples[(config>>20)&15];
    result={static_cast<u32>(info.superframeSize),unit*static_cast<u32>(info.framesInSuperframe),static_cast<u32>(info.framesInSuperframe),unit};return 0;
}
}
#ifndef LUMEN_GEN4_DIRECT_FEX
s32 PS4_SYSV_ABI sceNgs2GetWaveformFrameInfo(const OrbisNgs2WaveformFormat* input,u32* frame,u32* samples,u32* units,u32* delay) {
    OrbisNgs2WaveformFormat format{};if(!Read(input,format))return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS;
    std::array<u32*,4> pointers{frame,samples,units,delay};bool any=false;
    for(size_t n=0;n<pointers.size();++n)if(pointers[n]){
        any=true;if(!Range(pointers[n],sizeof(u32),true)||Overlap(input,sizeof(*input),pointers[n],sizeof(u32)))return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
        for(size_t prev=0;prev<n;++prev)if(pointers[prev]&&Overlap(pointers[prev],sizeof(u32),pointers[n],sizeof(u32)))return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    if(!any)return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::array<u32,4> values{};if(auto error=FrameInfo(format,values);error)return error;
    std::scoped_lock lock{registry_mutex};for(auto* p:pointers)if(p&&AliasesBacking(p,sizeof(u32)))return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    for(size_t n=0;n<pointers.size();++n)if(pointers[n])std::memcpy(pointers[n],&values[n],sizeof(u32));return 0;
}
s32 PS4_SYSV_ABI sceNgs2CalcWaveformBlock(const OrbisNgs2WaveformFormat* input,u32 position,u32 count,OrbisNgs2WaveformBlock* out) {
    OrbisNgs2WaveformFormat format{};if(!Read(input,format))return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_ADDRESS;
    if(!Range(out,sizeof(*out),true)||Overlap(input,sizeof(*input),out,sizeof(*out)))return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::array<u32,4> info{};if(auto error=FrameInfo(format,info);error)return error;
    // PCM positions have exact byte addressing. Encoded block construction also
    // needs stream delay/trim metadata, which this ABI does not establish here.
    if(!PcmSize(format.waveformType))return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_FORMAT;
    if(position>UINT32_MAX/info[0]||count>UINT32_MAX/info[0])return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE;
    OrbisNgs2WaveformBlock block{position*info[0],count*info[0],0,0,count,0,0};
    std::scoped_lock lock{registry_mutex};if(AliasesBacking(out,sizeof(*out)))return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    std::memcpy(out,&block,sizeof(block));return 0;
}
#endif
} // namespace Libraries::Ngs2
