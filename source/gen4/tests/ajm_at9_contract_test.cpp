// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "atrac9_test_data.h"
#include "core/libraries/ajm/ajm_at9.h"
#include "core/libraries/ajm/ajm_result.h"
using namespace Libraries::Ajm;
static void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){try{
    const auto flags=static_cast<AjmAt9CodecFlags>(0);
    AjmDecAt9InitializeParameters parameters{};std::array<u8,4> config{0xfe,0x74,0x1f,0xf0};std::memcpy(parameters.config_data,config.data(),config.size());
    for(const auto encoding:{AjmFormatEncoding::S16,AjmFormatEncoding::S32,AjmFormatEncoding::Float}) {
        AjmAt9Decoder decoder(encoding,flags,2);decoder.Initialize(&parameters,sizeof(parameters));
        Check(decoder.GetMinimumInputSize()==1024,"bounded AT9 geometry");
        std::array<u8,8192> pcm;pcm.fill(0x5a);std::array<std::span<u8>,1> spans{std::span<u8>(pcm)};SparseOutputBuffer output(spans);
        auto encoded=Ngs2TestMakeStereoAtrac9Superframe(0x4b595459u);std::span<u8> truncated(encoded.data(),encoded.size()-1);AjmInstanceGapless gapless{};
        const auto rejected=decoder.ProcessData(truncated,output,gapless);
        Check(rejected.result==ORBIS_AJM_RESULT_PARTIAL_INPUT&&truncated.size()==1023&&pcm[0]==0x5a,"truncated superframe leaves input/output unchanged");
        std::span<u8> input(encoded);u32 total{};for(int i=0;i<4;++i){const auto decoded=decoder.ProcessData(input,output,gapless);Check(decoded.result==0&&decoded.frames_decoded==1&&decoded.samples_written==256,"decode complete frame");total+=decoded.samples_written;}
        Check(input.empty()&&total==1024,"bounded superframe consumed exactly");
        decoder.Initialize(nullptr,0);Check(decoder.GetFormat().num_channels==0,"invalid initialization has safe format query");
        input=encoded;Check(decoder.ProcessData(input,output,gapless).result==ORBIS_AJM_RESULT_NOT_INITIALIZED,"invalid initialization rejects decode");
    }
    // Gapless completion drains the remaining codec frames without crossing the packet end.
    AjmAt9Decoder drain(AjmFormatEncoding::S16,flags,2);drain.Initialize(&parameters,sizeof(parameters));
    auto encoded=Ngs2TestMakeStereoAtrac9Superframe(0x4b595459u);std::span<u8> input(encoded);std::array<u8,1024> pcm{};std::array<std::span<u8>,1> spans{std::span<u8>(pcm)};SparseOutputBuffer output(spans);
    AjmInstanceGapless gapless{};gapless.init.total_samples=100;gapless.Reset();const auto result=drain.ProcessData(input,output,gapless);
    Check(result.result==0&&result.frames_decoded==4&&result.samples_written==100&&input.empty(),"gapless drain bounded across four frames");
    AjmAt9Decoder riff(AjmFormatEncoding::S16,AjmAt9CodecFlags::ParseRiffHeader,2);std::array<u8,4> broken_riff{'R','I','F','F'};input=broken_riff;
    Check(riff.ProcessData(input,output,gapless).result&(ORBIS_AJM_RESULT_CODEC_ERROR|ORBIS_AJM_RESULT_FATAL),"truncated RIFF returns error");
    std::puts("AJM bounded AT9 S16/S32/F32, partial input, initialization and drain passed");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
