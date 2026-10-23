#include "Emulator/Audio.h"
#include "Emulator/Common.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/Voice.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cstdio>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

namespace LibAudioOut {

LIB_VERSION("AudioOut", 1, "AudioOut", 1, 1);

namespace AudioOut = Audio::AudioOut;

LIB_DEFINE(InitAudio_1_AudioOut)
{
	LIB_FUNC("JfEPXVxhFqA", AudioOut::AudioOutInit);
	LIB_FUNC("ekNvsT22rsY", AudioOut::AudioOutOpen);
	LIB_FUNC("b+uAV89IlxE", AudioOut::AudioOutSetVolume);
	LIB_FUNC("w3PdaSTSwGE", AudioOut::AudioOutOutputs);
	LIB_FUNC("QOQtbeDqsT4", AudioOut::AudioOutOutput);
	LIB_FUNC("s1--uE9mBFw", AudioOut::AudioOutClose);
	LIB_FUNC("GrQ9s4IrNaQ", AudioOut::AudioOutGetPortState);
}

} // namespace LibAudioOut

namespace LibAudioIn {

LIB_VERSION("AudioIn", 1, "AudioIn", 1, 1);

namespace AudioIn = Audio::AudioIn;

LIB_DEFINE(InitAudio_1_AudioIn)
{
	LIB_FUNC("5NE8Sjc7VC8", AudioIn::AudioInOpen);
	LIB_FUNC("LozEOU8+anM", AudioIn::AudioInInput);
}

} // namespace LibAudioIn

namespace LibVoiceQoS {

LIB_VERSION("VoiceQoS", 1, "VoiceQoS", 0, 0);

namespace VoiceQoS = Audio::VoiceQoS;

LIB_DEFINE(InitAudio_1_VoiceQoS)
{
	LIB_FUNC("U8IfNl6-Css", VoiceQoS::VoiceQoSInit);
}

} // namespace LibVoiceQoS

namespace LibVoice {

LIB_VERSION("Voice", 1, "Voice", 0, 0);

LIB_DEFINE(InitAudio_1_Voice)
{
	if (!Voice::ExperimentalEnabled()) return;
	LIB_FUNC("9TrhuGzberQ", Voice::Init);
	LIB_FUNC("nXpje5yNpaE", Voice::CreatePort);
	LIB_FUNC("b7kJI+nx2hg", Voice::DeletePort);
	LIB_FUNC("CrLqDwWLoXM", Voice::GetPortInfo);
	LIB_FUNC("54phPH2LZls", Voice::Start);
	LIB_FUNC("Ao2YNSA7-Qo", Voice::Stop);
	LIB_FUNC("Oo0S5PH7FIQ", Voice::End);
	std::fprintf(stderr,
	             "[VOICE_OFFLINE_EXPERIMENT] enabled=1 exports=7 experimental=1 "
	             "console_contract=unverified configuration=opaque-token scalar=opaque audio=0 networking=0\n");
}

} // namespace LibVoice

namespace LibAvPlayer {

LIB_VERSION("AvPlayer", 1, "AvPlayer", 1, 0);

namespace AvPlayer = Audio::AvPlayer;

LIB_DEFINE(InitAudio_1_AvPlayer)
{
	LIB_FUNC("aS66RI0gGgo", AvPlayer::AvPlayerInit);
	LIB_FUNC("KMcEa+rHsIo", AvPlayer::AvPlayerAddSource);
	LIB_FUNC("OVths0xGfho", AvPlayer::AvPlayerSetLooping);
	LIB_FUNC("JdksQu8pNdQ", AvPlayer::AvPlayerGetVideoDataEx);
	LIB_FUNC("Wnp1OVcrZgk", AvPlayer::AvPlayerGetAudioData);
	LIB_FUNC("UbQoYawOsfY", AvPlayer::AvPlayerIsActive);
	LIB_FUNC("ZC17w3vB5Lo", AvPlayer::AvPlayerStop);
	LIB_FUNC("NkJwDzKmIlw", AvPlayer::AvPlayerClose);
}

} // namespace LibAvPlayer

namespace LibAudio3d {

LIB_VERSION("Audio3d", 1, "Audio3d", 1, 1);

namespace Audio3d = Audio::Audio3d;

LIB_DEFINE(InitAudio_1_Audio3d)
{
	LIB_FUNC("UmCvjSmuZIw", Audio3d::Audio3dInitialize);
	LIB_FUNC("Im+jOoa5WAI", Audio3d::Audio3dGetDefaultOpenParameters);
	LIB_FUNC("XeDDK0xJWQA", Audio3d::Audio3dPortOpen);
	LIB_FUNC("Yq9bfUQ0uJg", Audio3d::Audio3dPortSetAttribute);
	LIB_FUNC("YaaDbDwKpFM", Audio3d::Audio3dPortGetQueueLevel);
	LIB_FUNC("lw0qrdSjZt8", Audio3d::Audio3dPortAdvance);
	LIB_FUNC("VEVhZ9qd4ZY", Audio3d::Audio3dPortPush);
}

} // namespace LibAudio3d

namespace LibNgs2 {

LIB_VERSION("Ngs2", 1, "Ngs2", 1, 1);

namespace Ngs2 = Audio::Ngs2;

LIB_DEFINE(InitAudio_1_Ngs2)
{
	LIB_FUNC("mPYgU4oYpuY", Ngs2::Ngs2SystemCreateWithAllocator);
	LIB_FUNC("U546k6orxQo", Ngs2::Ngs2RackCreateWithAllocator);
	LIB_FUNC("cLV4aiT9JpA", Ngs2::Ngs2RackCreate);
	LIB_FUNC("0eFLVCfWVds", Ngs2::Ngs2RackQueryBufferSize);
	LIB_FUNC("MwmHz8pAdAo", Ngs2::Ngs2RackGetVoiceHandle);
	LIB_FUNC("uu94irFOGpA", Ngs2::Ngs2VoiceControl);
	LIB_FUNC("-TOuuAQ-buE", Ngs2::Ngs2VoiceGetState);
	LIB_FUNC("i0VnXM-C9fc", Ngs2::Ngs2SystemRender);
	if (Ngs2::Ngs2WaveformFrameInfoExperimentalEnabled())
	{
		LIB_FUNC("ekGJmmoc8j4", Ngs2::Ngs2GetWaveformFrameInfo);
		std::fprintf(stderr,
		             "[NGS2_WAVEFORM_FRAME_INFO_LOCAL_EXPERIMENT] enabled=1 exports=1 experimental=1 "
		             "console_contract=unverified profile=pcm-interleaved+atrac9-superframe "
		             "codecs=pcm,atrac9\n");
	}
	if (Ngs2::Ngs2VoiceStateFlagsExperimentalEnabled())
	{
		LIB_FUNC("rEh728kXk3w", Ngs2::Ngs2VoiceGetStateFlags);
		std::fprintf(stderr,
		             "[NGS2_VOICE_STATE_FLAGS_LOCAL_EXPERIMENT] enabled=1 exports=1 experimental=1 "
		             "console_contract=unverified profile=existing-state-word\n");
	}
	if (Ngs2::Ngs2AudioRenderExperimentalEnabled())
	{
		std::fprintf(stderr,
		             "[NGS2_AUDIO_RENDER_LOCAL_EXPERIMENT] enabled=1 exports=0 experimental=1 "
		             "console_contract=unverified profile=atrac9-snapshot+float8-render\n");
	}
}

} // namespace LibNgs2

LIB_DEFINE(InitAudio_1)
{
	LibAudioOut::InitAudio_1_AudioOut(s);
	LibAudioIn::InitAudio_1_AudioIn(s);
	LibVoiceQoS::InitAudio_1_VoiceQoS(s);
	LibVoice::InitAudio_1_Voice(s);
	LibAvPlayer::InitAudio_1_AvPlayer(s);
	LibAudio3d::InitAudio_1_Audio3d(s);
	LibNgs2::InitAudio_1_Ngs2(s);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
