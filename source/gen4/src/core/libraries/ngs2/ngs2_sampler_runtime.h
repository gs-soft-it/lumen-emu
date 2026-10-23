// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <memory>
#include <span>
#include <vector>
#include "ngs2.h"
#include "core/libraries/ajm/atrac9_guard.h"
extern "C" {
#include <libatrac9.h>
}
namespace Libraries::Ngs2::Detail {
constexpr size_t MaxEncodedBytes = 64 * 1024 * 1024;
constexpr size_t MaxPcmSamples = 64 * 1024 * 1024;
struct SamplerPacket {
    std::vector<u8> encoded;
    u64 skip{}, playable{}, identity{}, user{};
};
struct DecodedBlock {
    std::shared_ptr<const std::vector<float>> pcm;
    u64 identity{}, user{};
};
struct SamplerRuntime {
    std::deque<DecodedBlock> blocks;
    size_t cursor{}, retained{};
    // History is immutable once committed. Candidate appends clone it, so a
    // late malformed frame cannot corrupt the previously accepted FIFO.
    std::shared_ptr<void> history;
    bool AtEnd(u32 channels) const {
        return channels && blocks.size()==1 && cursor==blocks.front().pcm->size()/channels;
    }
    bool Append(const OrbisNgs2WaveformFormat& format,const SamplerPacket& packet) {
        if(format.waveformType!=0x40||format.frameOffset||format.frameMargin||packet.encoded.empty()||packet.encoded.size()>MaxEncodedBytes||!packet.playable)return false;
        const u32 config=format.configData;
        if((config>>24)!=0xfe||((config>>17)&7)>5||(config&0x10007)||(((config>>3)&3)!=0&&((config>>3)&3)!=2))return false;
        std::array<unsigned char,4> bytes{static_cast<u8>(config>>24),static_cast<u8>(config>>16),static_cast<u8>(config>>8),static_cast<u8>(config)};
        const bool replace=AtEnd(format.numChannels);
        const bool carry=packet.skip==0&&!replace&&history;
        std::scoped_lock library_lock{Ajm::Atrac9LibraryMutex()};
        std::shared_ptr<void> decoder(carry?Atrac9CloneHandle(history.get()):Atrac9GetHandle(),Atrac9ReleaseHandle);
        if(!decoder||(!carry&&Atrac9InitDecoder(decoder.get(),bytes.data())))return false;
        Atrac9CodecInfo info{};if(Atrac9GetCodecInfo(decoder.get(),&info)||!std::equal(bytes.begin(),bytes.end(),info.configData))return false;
        if(info.channels<=0||info.channels!=format.numChannels||info.superframeSize<=0||info.superframeSize>8192||
            (info.framesInSuperframe!=1&&info.framesInSuperframe!=4)||(info.frameSamples!=64&&info.frameSamples!=128&&info.frameSamples!=256))return false;
        const size_t multiplier=info.samplingRate==24000&&format.sampleRate==48000?2:1;
        if(info.samplingRate!=format.sampleRate&&multiplier!=2)return false;
        if(packet.encoded.size()%info.superframeSize)return false;
        const u64 available=packet.encoded.size()/info.superframeSize*static_cast<u64>(info.frameSamples)*info.framesInSuperframe;
        if(packet.skip>available||packet.playable>available-packet.skip||packet.playable>MaxPcmSamples/(info.channels*multiplier))return false;
        const size_t pcm_samples=static_cast<size_t>(packet.playable)*info.channels*multiplier;
        const size_t old_samples=replace?0:retained;
        if(old_samples>MaxPcmSamples-pcm_samples||(!replace&&blocks.size()>=4096))return false;
        std::vector<float> frame(static_cast<size_t>(info.frameSamples)*info.channels);
        std::vector<float> decoded;decoded.reserve(pcm_samples);
        u64 skip=packet.skip,remaining=packet.playable;
        for(size_t offset=0;offset<packet.encoded.size()&&remaining;offset+=info.superframeSize){
            size_t cursor_bytes=offset,bytes_left=info.superframeSize;
            for(int index=0;index<info.framesInSuperframe;++index){
                int used{};
                if(Atrac9DecodeF32(decoder.get(),packet.encoded.data()+cursor_bytes,static_cast<int>(bytes_left),frame.data(),&used,0)||used<=0||static_cast<size_t>(used)>bytes_left)return false;
                const auto trim=std::min<u64>(skip,info.frameSamples);skip-=trim;
                const auto take=std::min<u64>(remaining,info.frameSamples-trim);
                decoded.insert(decoded.end(),frame.begin()+trim*info.channels,frame.begin()+(trim+take)*info.channels);remaining-=take;
                cursor_bytes+=used;bytes_left-=used;
            }
        }
        if(skip||remaining)return false;
        if(multiplier==2){
            std::vector<float> expanded(pcm_samples);const size_t frames=decoded.size()/info.channels;
            for(size_t frame_index=0;frame_index<frames;++frame_index)for(int channel=0;channel<info.channels;++channel){
                const float a=decoded[frame_index*info.channels+channel],b=decoded[std::min(frame_index+1,frames-1)*info.channels+channel];
                expanded[(frame_index*2)*info.channels+channel]=a;expanded[(frame_index*2+1)*info.channels+channel]=(a+b)*0.5f;
            }
            decoded=std::move(expanded);
        }
        DecodedBlock block{std::make_shared<const std::vector<float>>(std::move(decoded)),packet.identity,packet.user};
        blocks.push_back(std::move(block));
        if(replace){blocks.pop_front();cursor=0;}
        retained=old_samples+pcm_samples;
        history=packet.playable==available-packet.skip?std::move(decoder):std::shared_ptr<void>{};
        return true;
    }
    void Event(u32 event,u32& state,u32 channels) {
        switch(event){
        case 0:if(AtEnd(channels))cursor=0;state=3;break;
        case 1:if(state==3||state==5)state=0xb;break;
        case 2:case 3:state=0;blocks.clear();cursor=retained=0;history.reset();break;
        case 4:if(state==3)state=5;break;
        case 5:if(state==5)state=3;break;
        default:break;
        }
    }
    void Mix(u32& state,u32 channels,std::span<float> output,u32 output_channels) {
        if(state!=3||!channels||!output_channels)return;
        size_t written=0;const size_t grain=output.size()/output_channels;
        while(!blocks.empty()&&written<grain){
            const auto& pcm=*blocks.front().pcm;const auto frames=pcm.size()/channels;
            const auto count=std::min(grain-written,frames-cursor);
            for(size_t frame=0;frame<count;++frame){
                const size_t source=(cursor+frame)*channels,dest=(written+frame)*output_channels;
                if(channels==1&&output_channels>=2){output[dest]+=pcm[source];output[dest+1]+=pcm[source];}
                else for(u32 channel=0;channel<std::min(channels,output_channels);++channel)output[dest+channel]+=pcm[source+channel];
            }
            cursor+=count;written+=count;
            if(cursor==frames){if(blocks.size()==1){state=0;break;}retained-=pcm.size();blocks.pop_front();cursor=0;}
        }
    }
};
}
