#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Audio.h"
#include "Emulator/Config.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/NpInGameMessage.h"
#include "Emulator/Libs/NpParental.h"
#include "Emulator/Libs/NpPlus.h"
#include "Emulator/Libs/NpRequests.h"
#include "Emulator/Libs/Remoteplay.h"
#include "Emulator/Libs/Rudp.h"
#include "Emulator/Libs/Voice.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "Emulator/Network.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <spawn.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

namespace Voice = Lumen::Libs::Voice;
namespace Audio = Lumen::Libs::Audio;
namespace Ngs2 = Audio::Ngs2;
namespace Message = Lumen::Libs::NpInGameMessage;
namespace Rudp = Lumen::Libs::Rudp;
namespace Requests = Lumen::Libs::NpRequests;
namespace Network = Lumen::Libs::Network;
namespace Np = Network::NpManager;
namespace Vm = Lumen::Core::VirtualMemory;
using Result = Voice::ModelResult;

constexpr const char* SELECTOR = "LUMEN_EXPERIMENTAL_VOICE_OFFLINE";
constexpr const char* NGS2_SELECTOR = "LUMEN_EXPERIMENTAL_NGS2_WAVEFORM_FRAME_INFO";
constexpr const char* NGS2_STATE_FLAGS_SELECTOR = "LUMEN_EXPERIMENTAL_NGS2_VOICE_STATE_FLAGS";
constexpr const char* NGS2_AUDIO_RENDER_SELECTOR = "LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER";
constexpr uint32_t MAXIMUM = std::numeric_limits<uint32_t>::max();
constexpr uintptr_t MAX_TOKEN = std::numeric_limits<uintptr_t>::max();
constexpr uint64_t OTHER_VALUE = 0x12345678abcdef91;
constexpr size_t DESCRIPTOR_BYTES = 24;
constexpr size_t DESCRIPTOR = 67;
constexpr size_t OUTPUT = 131;
constexpr size_t INFO_BYTES = 28;
using InfoFunction = decltype(&Voice::GetPortInfo);
InfoFunction g_get_port_info = &Voice::GetPortInfo;
using WaveformFunction = decltype(&Ngs2::Ngs2GetWaveformFrameInfo);
WaveformFunction g_get_waveform_frame_info = &Ngs2::Ngs2GetWaveformFrameInfo;
using WaveformResult = Ngs2::Ngs2WaveformFrameInfoResult;
using StateFlagsFunction = decltype(&Ngs2::Ngs2VoiceGetStateFlags);
StateFlagsFunction g_get_voice_state_flags = &Ngs2::Ngs2VoiceGetStateFlags;
using StateFlagsResult = Ngs2::Ngs2VoiceStateFlagsResult;
static_assert(Result::InvalidInfoOutput != Result::InvalidOutput);

// Original fixtures for VOICE_PORT_EXTENDED_INTERFACE.md's local profile.
// The Init configuration stays opaque. Port bytes, lifetime and outcomes test
// the selected local contract, not console conformance or an audio service.
bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "VoiceTest: %s\n", message);
	return condition;
}

bool State(bool initialized, uintptr_t configuration = 0, uint32_t value = 0, uint32_t ports = 0, bool started = false)
{
	const auto snapshot = Voice::GetModelSnapshot();
	return snapshot.initialized == initialized && snapshot.configuration_token == configuration &&
	       snapshot.initialization_value == value && snapshot.active_ports == ports && snapshot.started == started;
}

class Mapping
{
public:
	explicit Mapping(size_t size): m_size(size), m_address(Vm::Alloc(0, size, Vm::Mode::ReadWrite)) {}
	~Mapping() { if (m_address != 0) Vm::Free(m_address); }
	Mapping(const Mapping&) = delete;
	Mapping& operator=(const Mapping&) = delete;
	bool Valid() const { return m_address != 0; }
	size_t Size() const { return m_size; }
	uint64_t Address(size_t offset = 0) const { return m_address + offset; }
	uint8_t* Data(size_t offset = 0) const { return reinterpret_cast<uint8_t*>(Address(offset)); }
	bool Protect(size_t offset, size_t size, Vm::Mode mode)
	{
		return Valid() && offset <= m_size && size <= m_size - offset && Vm::Protect(Address(offset), size, mode);
	}
	bool Free()
	{
		if (!Valid() || !Vm::Free(m_address)) return false;
		m_address = 0;
		return true;
	}

private:
	size_t m_size;
	uint64_t m_address;
};

void Store32(uint8_t* bytes, uint32_t value)
{
	for (size_t i = 0; i < 4; ++i) bytes[i] = static_cast<uint8_t>(value >> (i * 8));
}

uint32_t Load32(const uint8_t* bytes)
{
	uint32_t value = 0;
	for (size_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(bytes[i]) << (i * 8);
	return value;
}

std::array<uint8_t, DESCRIPTOR_BYTES> Descriptor(uint32_t type, uint8_t seed = 29)
{
	std::array<uint8_t, DESCRIPTOR_BYTES> bytes {};
	for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(seed + i * 37);
	Store32(bytes.data(), type);
	return bytes;
}

void Fill(Mapping& memory, uint32_t type = 0, uint8_t seed = 29)
{
	for (size_t i = 0; i < memory.Size(); ++i) memory.Data()[i] = static_cast<uint8_t>(83 + i * 17);
	const auto descriptor = Descriptor(type, seed);
	std::memcpy(memory.Data(DESCRIPTOR), descriptor.data(), descriptor.size());
}

std::vector<uint8_t> Snapshot(const Mapping& memory)
{
	return {memory.Data(), memory.Data() + memory.Size()};
}

bool Matches(const Mapping& memory, const std::vector<uint8_t>& bytes)
{
	return bytes.size() == memory.Size() && std::memcmp(memory.Data(), bytes.data(), bytes.size()) == 0;
}

bool Port(uint32_t id, const std::array<uint8_t, DESCRIPTOR_BYTES>& descriptor)
{
	const auto port = Voice::GetPortSnapshot(id);
	return port.live && port.id == id && port.descriptor == descriptor;
}

bool Rejected(Mapping& memory, uint64_t output, uint64_t descriptor, Result expected)
{
	const auto before = Snapshot(memory);
	const auto state = Voice::GetModelSnapshot();
	return Check(Voice::CreatePortModel(output, descriptor) == expected, "port rejection returned the wrong local outcome") &&
	       Check(Matches(memory, before) && State(state.initialized, state.configuration_token, state.initialization_value,
	                                            state.active_ports, state.started),
	             "rejected creation changed guest bytes or administrative state");
}

bool Create(Mapping& memory, uint32_t* id, size_t output = OUTPUT, size_t descriptor = DESCRIPTOR, bool wrapper = false)
{
	auto expected = Snapshot(memory);
	std::array<uint8_t, DESCRIPTOR_BYTES> retained {};
	std::memcpy(retained.data(), memory.Data(descriptor), retained.size());
	const auto before = Voice::GetModelSnapshot();
	const bool created = wrapper ? Voice::CreatePort(reinterpret_cast<uint32_t*>(memory.Address(output)), memory.Data(descriptor)) == 0
	                             : Voice::CreatePortModel(memory.Address(output), memory.Address(descriptor)) == Result::Ok;
	if (!Check(created, "supported descriptor did not create a local resource")) return false;
	*id = Load32(memory.Data(output));
	Store32(expected.data() + output, *id);
	return Check(*id != 0 && Port(*id, retained) && Matches(memory, expected) &&
	                 State(before.initialized, before.configuration_token, before.initialization_value, before.active_ports + 1, before.started),
	             "creation did not publish one complete resource and exactly four identifier bytes");
}

void ExpectedInfo(uint8_t* output, uint32_t type)
{
	std::memset(output, 0, INFO_BYTES);
	Store32(output, type);
}

bool Info(Mapping& memory, uint32_t id, uint32_t type, size_t output = OUTPUT, bool wrapper = false)
{
	auto expected = Snapshot(memory);
	ExpectedInfo(expected.data() + output, type);
	const auto state = Voice::GetModelSnapshot();
	const auto port = Voice::GetPortSnapshot(id);
	const bool queried = wrapper ? g_get_port_info(id, memory.Data(output)) == 0
	                             : Voice::GetPortInfoModel(id, memory.Address(output)) == Result::Ok;
	return Check(queried && Matches(memory, expected) && Port(id, port.descriptor) &&
	                 State(state.initialized, state.configuration_token, state.initialization_value, state.active_ports, state.started),
	             "port info did not write only its 28 local fields or changed administrative state");
}

bool InfoRejected(Mapping& memory, uint32_t id, uint64_t output, Result expected)
{
	const auto bytes = Snapshot(memory);
	const auto state = Voice::GetModelSnapshot();
	const auto port = Voice::GetPortSnapshot(id);
	const auto result = Voice::GetPortInfoModel(id, output);
	const auto after = Voice::GetPortSnapshot(id);
	return Check(result == expected && Matches(memory, bytes) && after.live == port.live && after.id == port.id &&
	                 after.descriptor == port.descriptor &&
	                 State(state.initialized, state.configuration_token, state.initialization_value, state.active_ports, state.started),
	             "rejected port info changed bytes/state or returned the wrong precedence");
}

bool Live(int request)
{
	Requests::LiveRequestGuard guard(request);
	return guard.IsLive();
}

bool OtherState(bool initialized)
{
	const auto message = Message::GetModelSnapshot();
	const auto rudp = Rudp::GetModelSnapshot();
	return message.initialized == initialized && message.initialization_token == (initialized ? 77 : 0) &&
	       rudp.initialized == initialized && rudp.workspace_token == (initialized ? MAX_TOKEN : 0) &&
	       rudp.initialization_value == (initialized ? OTHER_VALUE : 0) && rudp.io_requested == initialized &&
	       rudp.io_value_1 == (initialized ? 7 : 0) && rudp.io_value_2 == (initialized ? 8 : 0);
}

bool InitializeOtherServices()
{
	return Message::InitializeModel(77, nullptr) == Message::ModelResult::Ok &&
	       Rudp::InitializeModel(reinterpret_cast<const void*>(MAX_TOKEN), OTHER_VALUE) == Rudp::ModelResult::Ok &&
	       Rudp::EnableInternalIOThreadModel(7, 8) == Rudp::ModelResult::Ok && OtherState(true);
}

bool Selectors(bool voice, bool other, bool ngs2 = false, bool ngs2_state_flags = false,
               bool ngs2_audio_render = false)
{
	return Check(Voice::ExperimentalEnabled() == voice && Message::ExperimentalEnabled() == other &&
	                 Rudp::ExperimentalEnabled() == other && Lumen::Libs::NpPlus::ExperimentalEnabled() == other &&
	                 Lumen::Libs::NpParental::ExperimentalEnabled() == other &&
	                 !Lumen::Libs::Remoteplay::ExperimentalEnabled() &&
	                 Ngs2::Ngs2WaveformFrameInfoExperimentalEnabled() == ngs2 &&
	                 Ngs2::Ngs2VoiceStateFlagsExperimentalEnabled() == ngs2_state_flags &&
	                 Ngs2::Ngs2AudioRenderExperimentalEnabled() == ngs2_audio_render,
	             "independent selectors changed") &&
	       Check(Requests::TrackingEnabled() == other, "Voice selection changed NP request tracking");
}

bool Registration(bool enabled, bool ngs2 = false, bool ngs2_state_flags = false)
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libAudio_1", &symbols), "Audio library initialization failed")) return false;
	Lumen::Loader::SymbolResolve exact {};
	exact.name = "9TrhuGzberQ";
	exact.library = "libSceVoice";
	exact.library_version = 1;
	exact.module = "libSceVoice";
	exact.module_version_major = 0;
	exact.module_version_minor = 0;
	exact.type = Lumen::Loader::SymbolType::Func;
	const auto* found = symbols.Find(exact);
	struct Binding { const char* nid; uint64_t address; };
	const std::array<Binding, 6> voice {{
	    {"9TrhuGzberQ", reinterpret_cast<uint64_t>(&Voice::Init)},
	    {"nXpje5yNpaE", reinterpret_cast<uint64_t>(&Voice::CreatePort)},
	    {"b7kJI+nx2hg", reinterpret_cast<uint64_t>(&Voice::DeletePort)},
	    {"54phPH2LZls", reinterpret_cast<uint64_t>(&Voice::Start)},
	    {"Ao2YNSA7-Qo", reinterpret_cast<uint64_t>(&Voice::Stop)},
	    {"Oo0S5PH7FIQ", reinterpret_cast<uint64_t>(&Voice::End)},
	}};
	for (const auto& binding: voice)
	{
		exact.name = binding.nid;
		found = symbols.Find(exact);
		if (!Check(enabled ? found != nullptr && found->vaddr == binding.address : found == nullptr,
		           "Voice export selection or target changed")) return false;
		for (int version: {0, 2})
		{
			auto query = exact;
			query.library_version = version;
			if (!Check(symbols.Find(query) == nullptr, "unsupported Voice library version resolved")) return false;
		}
		for (int version: {1, 2})
		{
			auto query = exact;
			query.module_version_major = version;
			if (!Check(symbols.Find(query) == nullptr, "unsupported Voice module major resolved")) return false;
			query = exact;
			query.module_version_minor = version;
			if (!Check(symbols.Find(query) == nullptr, "unsupported Voice module minor resolved")) return false;
		}
		for (const char* name: {"libSceVoiceQoS", "libSceAudioOut", "libSceNpManager"})
		{
			auto query = exact;
			query.library = name;
			if (!Check(symbols.Find(query) == nullptr, "Voice resolved in another library")) return false;
			query = exact;
			query.module = name;
			if (!Check(symbols.Find(query) == nullptr, "Voice resolved in another module")) return false;
			query.library = name;
			if (!Check(symbols.Find(query) == nullptr, "Voice resolved in another complete namespace")) return false;
		}
		auto query = exact;
		query.type = Lumen::Loader::SymbolType::Object;
		if (!Check(symbols.Find(query) == nullptr, "Voice resolved as an object")) return false;
	}
	auto port_info = exact;
	port_info.name = "CrLqDwWLoXM";
	const auto* port_info_export = symbols.Find(port_info);
	if (!Check(enabled ? port_info_export != nullptr && port_info_export->vaddr == reinterpret_cast<uint64_t>(&Voice::GetPortInfo)
	                   : port_info_export == nullptr,
	           "Voice GetPortInfo export is missing when selected or present by default")) return false;
	if (enabled) g_get_port_info = reinterpret_cast<InfoFunction>(port_info_export->vaddr);
	for (unsigned mismatch = 0; mismatch < 9; ++mismatch)
	{
		auto wrong = port_info;
		switch (mismatch)
		{
			case 0: wrong.library = "libSceVoiceQoS"; break;
			case 1: wrong.module = "libSceAudioOut"; break;
			case 2: wrong.library_version = 2; break;
			case 3: wrong.module_version_major = 1; break;
			case 4: wrong.module_version_minor = 1; break;
			case 5: wrong.type = Lumen::Loader::SymbolType::Object; break;
			case 6: wrong.library_version = 0; break;
			case 7: wrong.module_version_major = 2; break;
			case 8: wrong.module_version_minor = 2; break;
		}
		if (!Check(symbols.Find(wrong) == nullptr, "Voice GetPortInfo resolved with an unsupported identity")) return false;
	}
	auto query = exact;
	for (const char* name: {"U8IfNl6-Css", "sceVoiceInit", "sceVoiceEnd", "sceVoiceCreatePort", "sceVoiceDeletePort",
	                       "sceVoiceStart", "sceVoiceStop", "sceVoiceGetPortInfo", "original-voice-test-unknown-nid"})
	{
		query = exact;
		query.name = name;
		if (!Check(symbols.Find(query) == nullptr, "VoiceQoS or standard-name fallback resolved as Voice")) return false;
	}
	struct Export { const char* nid; const char* library; int major; int minor; uint64_t address; };
	// Address checks only: none of these audio entrypoints is invoked.
	const std::array<Export, 33> existing {{
	    {"U8IfNl6-Css", "libSceVoiceQoS", 0, 0, reinterpret_cast<uint64_t>(&Audio::VoiceQoS::VoiceQoSInit)},
	    {"JfEPXVxhFqA", "libSceAudioOut", 1, 1, reinterpret_cast<uint64_t>(&Audio::AudioOut::AudioOutInit)},
	    {"ekNvsT22rsY", "libSceAudioOut", 1, 1, reinterpret_cast<uint64_t>(&Audio::AudioOut::AudioOutOpen)},
	    {"b+uAV89IlxE", "libSceAudioOut", 1, 1, reinterpret_cast<uint64_t>(&Audio::AudioOut::AudioOutSetVolume)},
	    {"w3PdaSTSwGE", "libSceAudioOut", 1, 1, reinterpret_cast<uint64_t>(&Audio::AudioOut::AudioOutOutputs)},
	    {"QOQtbeDqsT4", "libSceAudioOut", 1, 1, reinterpret_cast<uint64_t>(&Audio::AudioOut::AudioOutOutput)},
	    {"s1--uE9mBFw", "libSceAudioOut", 1, 1, reinterpret_cast<uint64_t>(&Audio::AudioOut::AudioOutClose)},
	    {"GrQ9s4IrNaQ", "libSceAudioOut", 1, 1, reinterpret_cast<uint64_t>(&Audio::AudioOut::AudioOutGetPortState)},
	    {"5NE8Sjc7VC8", "libSceAudioIn", 1, 1, reinterpret_cast<uint64_t>(&Audio::AudioIn::AudioInOpen)},
	    {"LozEOU8+anM", "libSceAudioIn", 1, 1, reinterpret_cast<uint64_t>(&Audio::AudioIn::AudioInInput)},
	    {"aS66RI0gGgo", "libSceAvPlayer", 1, 0, reinterpret_cast<uint64_t>(&Audio::AvPlayer::AvPlayerInit)},
	    {"KMcEa+rHsIo", "libSceAvPlayer", 1, 0, reinterpret_cast<uint64_t>(&Audio::AvPlayer::AvPlayerAddSource)},
	    {"OVths0xGfho", "libSceAvPlayer", 1, 0, reinterpret_cast<uint64_t>(&Audio::AvPlayer::AvPlayerSetLooping)},
	    {"JdksQu8pNdQ", "libSceAvPlayer", 1, 0, reinterpret_cast<uint64_t>(&Audio::AvPlayer::AvPlayerGetVideoDataEx)},
	    {"Wnp1OVcrZgk", "libSceAvPlayer", 1, 0, reinterpret_cast<uint64_t>(&Audio::AvPlayer::AvPlayerGetAudioData)},
	    {"UbQoYawOsfY", "libSceAvPlayer", 1, 0, reinterpret_cast<uint64_t>(&Audio::AvPlayer::AvPlayerIsActive)},
	    {"ZC17w3vB5Lo", "libSceAvPlayer", 1, 0, reinterpret_cast<uint64_t>(&Audio::AvPlayer::AvPlayerStop)},
	    {"NkJwDzKmIlw", "libSceAvPlayer", 1, 0, reinterpret_cast<uint64_t>(&Audio::AvPlayer::AvPlayerClose)},
	    {"UmCvjSmuZIw", "libSceAudio3d", 1, 1, reinterpret_cast<uint64_t>(&Audio::Audio3d::Audio3dInitialize)},
	    {"Im+jOoa5WAI", "libSceAudio3d", 1, 1, reinterpret_cast<uint64_t>(&Audio::Audio3d::Audio3dGetDefaultOpenParameters)},
	    {"XeDDK0xJWQA", "libSceAudio3d", 1, 1, reinterpret_cast<uint64_t>(&Audio::Audio3d::Audio3dPortOpen)},
	    {"Yq9bfUQ0uJg", "libSceAudio3d", 1, 1, reinterpret_cast<uint64_t>(&Audio::Audio3d::Audio3dPortSetAttribute)},
	    {"YaaDbDwKpFM", "libSceAudio3d", 1, 1, reinterpret_cast<uint64_t>(&Audio::Audio3d::Audio3dPortGetQueueLevel)},
	    {"lw0qrdSjZt8", "libSceAudio3d", 1, 1, reinterpret_cast<uint64_t>(&Audio::Audio3d::Audio3dPortAdvance)},
	    {"VEVhZ9qd4ZY", "libSceAudio3d", 1, 1, reinterpret_cast<uint64_t>(&Audio::Audio3d::Audio3dPortPush)},
	    {"mPYgU4oYpuY", "libSceNgs2", 1, 1, reinterpret_cast<uint64_t>(&Audio::Ngs2::Ngs2SystemCreateWithAllocator)},
	    {"U546k6orxQo", "libSceNgs2", 1, 1, reinterpret_cast<uint64_t>(&Audio::Ngs2::Ngs2RackCreateWithAllocator)},
	    {"cLV4aiT9JpA", "libSceNgs2", 1, 1, reinterpret_cast<uint64_t>(&Audio::Ngs2::Ngs2RackCreate)},
	    {"0eFLVCfWVds", "libSceNgs2", 1, 1, reinterpret_cast<uint64_t>(&Audio::Ngs2::Ngs2RackQueryBufferSize)},
	    {"MwmHz8pAdAo", "libSceNgs2", 1, 1, reinterpret_cast<uint64_t>(&Audio::Ngs2::Ngs2RackGetVoiceHandle)},
	    {"uu94irFOGpA", "libSceNgs2", 1, 1, reinterpret_cast<uint64_t>(&Audio::Ngs2::Ngs2VoiceControl)},
	    {"-TOuuAQ-buE", "libSceNgs2", 1, 1, reinterpret_cast<uint64_t>(&Audio::Ngs2::Ngs2VoiceGetState)},
	    {"i0VnXM-C9fc", "libSceNgs2", 1, 1, reinterpret_cast<uint64_t>(&Audio::Ngs2::Ngs2SystemRender)},
	}};
	for (const auto& entry: existing)
	{
		query = exact;
		query.name = entry.nid;
		query.library = entry.library;
		query.module = entry.library;
		query.module_version_major = entry.major;
		query.module_version_minor = entry.minor;
		found = symbols.Find(query);
		if (!Check(found != nullptr && found->vaddr == entry.address, "an existing audio export changed")) return false;
	}
	Lumen::Loader::SymbolResolve waveform {};
	waveform.name = "ekGJmmoc8j4";
	waveform.library = "libSceNgs2";
	waveform.library_version = 1;
	waveform.module = "libSceNgs2";
	waveform.module_version_major = 1;
	waveform.module_version_minor = 1;
	waveform.type = Lumen::Loader::SymbolType::Func;
	const auto* waveform_export = symbols.Find(waveform);
	if (!Check(ngs2 ? waveform_export != nullptr &&
	                         waveform_export->vaddr == reinterpret_cast<uint64_t>(&Ngs2::Ngs2GetWaveformFrameInfo)
	                : waveform_export == nullptr,
	           "NGS2 WaveformFrameInfo export selection or target changed")) return false;
	if (ngs2) g_get_waveform_frame_info = reinterpret_cast<WaveformFunction>(waveform_export->vaddr);
	for (unsigned mismatch = 0; mismatch < 11; ++mismatch)
	{
		auto wrong = waveform;
		switch (mismatch)
		{
			case 0: wrong.library = "libSceAudio3d"; break;
			case 1: wrong.module = "libSceAudioOut"; break;
			case 2: wrong.library_version = 0; break;
			case 3: wrong.library_version = 2; break;
			case 4: wrong.module_version_major = 0; break;
			case 5: wrong.module_version_major = 2; break;
			case 6: wrong.module_version_minor = 0; break;
			case 7: wrong.module_version_minor = 2; break;
			case 8: wrong.type = Lumen::Loader::SymbolType::Object; break;
			case 9: wrong.type = Lumen::Loader::SymbolType::NoType; break;
			case 10: wrong.name = "original-ngs2-waveform-test-unknown-nid"; break;
		}
		if (!Check(symbols.Find(wrong) == nullptr, "NGS2 WaveformFrameInfo resolved with an unsupported identity")) return false;
	}
	if (!Check(symbols.FindByStdName(U"sceNgs2GetWaveformFrameInfo") == nullptr &&
	               symbols.FindByStdName(U"ekGJmmoc8j4") == nullptr,
	           "NGS2 WaveformFrameInfo gained a standard-name alias")) return false;
	Lumen::Loader::SymbolResolve state_flags {};
	state_flags.name = "rEh728kXk3w";
	state_flags.library = "libSceNgs2";
	state_flags.library_version = 1;
	state_flags.module = "libSceNgs2";
	state_flags.module_version_major = 1;
	state_flags.module_version_minor = 1;
	state_flags.type = Lumen::Loader::SymbolType::Func;
	const auto* state_flags_export = symbols.Find(state_flags);
	if (!Check(ngs2_state_flags ? state_flags_export != nullptr &&
	                                    state_flags_export->vaddr == reinterpret_cast<uint64_t>(&Ngs2::Ngs2VoiceGetStateFlags)
	                            : state_flags_export == nullptr,
	           "NGS2 VoiceGetStateFlags export selection or target changed")) return false;
	if (ngs2_state_flags)
		g_get_voice_state_flags = reinterpret_cast<StateFlagsFunction>(state_flags_export->vaddr);
	for (unsigned mismatch = 0; mismatch < 11; ++mismatch)
	{
		auto wrong = state_flags;
		switch (mismatch)
		{
			case 0: wrong.library = "libSceAudio3d"; break;
			case 1: wrong.module = "libSceAudioOut"; break;
			case 2: wrong.library_version = 0; break;
			case 3: wrong.library_version = 2; break;
			case 4: wrong.module_version_major = 0; break;
			case 5: wrong.module_version_major = 2; break;
			case 6: wrong.module_version_minor = 0; break;
			case 7: wrong.module_version_minor = 2; break;
			case 8: wrong.type = Lumen::Loader::SymbolType::Object; break;
			case 9: wrong.type = Lumen::Loader::SymbolType::NoType; break;
			case 10: wrong.name = "original-ngs2-state-flags-test-unknown-nid"; break;
		}
		if (!Check(symbols.Find(wrong) == nullptr,
		           "NGS2 VoiceGetStateFlags resolved with an unsupported identity")) return false;
	}
	if (!Check(symbols.FindByStdName(U"sceNgs2VoiceGetStateFlags") == nullptr &&
	               symbols.FindByStdName(U"rEh728kXk3w") == nullptr,
	           "NGS2 VoiceGetStateFlags gained a standard-name alias")) return false;
	const uint32_t targets = Lumen::Loader::AuthoritativeHostCallRegistryEnabled()
	                           ? 33U + (enabled ? 7U : 0U) + (ngs2 ? 1U : 0U) +
	                                 (ngs2_state_flags ? 1U : 0U)
	                           : 0U;
	return Check(symbols.StdNameMapSize() == 0 && symbols.HostCallTargetCount() == targets,
	             "unexpected alias or Audio HostCall target was registered");
}

int CreateRequest()
{
	// Existing local NP fixture, unrelated to the opaque Voice configuration.
	alignas(8) std::array<uint8_t, 24> parameter {};
	parameter[0] = 24;
	parameter[8] = 1;
	parameter[16] = 64;
	return Np::NpCreateAsyncRequest(reinterpret_cast<const Np::NpCreateAsyncRequestParameter*>(parameter.data()));
}

bool LegacyRequests()
{
	if (!Check(!Requests::TrackingEnabled() && Network::ResetForNextSession(), "legacy request setup failed") ||
	    !Check(CreateRequest() == 1 && !Live(1), "Voice changed NP tracking or request IDs")) return false;
	uint32_t state = 0xffffffff;
	int polled = -1;
	return Check(Np::NpGetState(9, &state) == 0 && state == 0 && Np::NpPollAsync(1, &polled) == 0 && polled == 0 &&
	                 Np::NpCheckNpAvailability(1, "original-voice-test", nullptr) == 0,
	             "legacy NP state, polling or availability changed") &&
	       Check(Np::NpDeleteRequest(1) == 0 && Np::NpDeleteRequest(1) == 0 && Network::ResetForNextSession(),
	             "legacy NP deletion or cleanup changed");
}

bool Disabled()
{
	Voice::ResetSessionState();
	return Selectors(false, false) && Registration(false) &&
	       Check(Voice::InitializeModel(reinterpret_cast<const void*>(MAX_TOKEN), MAXIMUM) == Result::Disabled &&
	                 Voice::InitializeModel(nullptr, 0) == Result::Disabled && State(false), "disabled initialization changed state") &&
	       Check(Voice::CreatePortModel(MAX_TOKEN, MAX_TOKEN) == Result::Disabled &&
	                 Voice::DeletePortModel(MAXIMUM) == Result::Disabled && Voice::StartModel() == Result::Disabled &&
	                 Voice::StopModel() == Result::Disabled && Voice::EndModel() == Result::Disabled &&
	                 Voice::GetPortInfoModel(MAXIMUM, MAX_TOKEN) == Result::Disabled &&
	                 !Voice::GetPortSnapshot(1).live && State(false), "disabled port or lifecycle operation changed state") &&
	       LegacyRequests();
}

bool OpaqueInputsAndReset()
{
	const long page_size = sysconf(_SC_PAGESIZE);
	if (!Check(page_size > 0, "host page size unavailable")) return false;
	void* inaccessible = mmap(nullptr, static_cast<size_t>(page_size), PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (!Check(inaccessible != MAP_FAILED, "inaccessible fixture mapping failed")) return false;
	std::array<uint8_t, 64> storage {};
	for (size_t byte = 0; byte < storage.size(); ++byte) storage[byte] = static_cast<uint8_t>(31 + byte * 43);
	const auto original = storage;
	const std::array<const void*, 6> pointers {{nullptr, reinterpret_cast<const void*>(uintptr_t(1)),
	    reinterpret_cast<const void*>(MAX_TOKEN), reinterpret_cast<const void*>(uintptr_t(0x12345678abcdef91)),
	    storage.data(), inaccessible}};
	bool passed = true;
	for (const void* pointer: pointers)
	{
		for (uint32_t value: {uint32_t(0), uint32_t(1), uint32_t(0x91abcdef), MAXIMUM})
		{
			Voice::ResetSessionState();
			passed = Check(State(false) && Voice::Init(pointer, value) == 0 &&
			                   State(true, reinterpret_cast<uintptr_t>(pointer), value) && storage == original,
			               "initialization accessed memory or changed an opaque input") && passed;
			passed = Check(Voice::InitializeModel(reinterpret_cast<const void*>(uintptr_t(3)), value ^ MAXIMUM) == Result::AlreadyInitialized &&
			                   State(true, reinterpret_cast<uintptr_t>(pointer), value) && storage == original,
			               "duplicate initialization changed retained inputs or guest storage") && passed;
		}
	}
	passed = Check(Lumen::Libs::ResetSessionState() && State(false), "aggregate reset retained Voice state") && passed;
	passed = Check(Voice::InitializeModel(nullptr, 7) == Result::Ok && State(true, 0, 7), "initialization after reset failed") && passed;
	Voice::ResetSessionState();
	Voice::ResetSessionState();
	passed = Check(State(false) && storage == original, "repeated reset retained state or changed storage") && passed;
	return Check(munmap(inaccessible, static_cast<size_t>(page_size)) == 0, "fixture unmap failed") && passed;
}

bool ConcurrentInitialization()
{
	Voice::ResetSessionState();
	std::atomic<bool> start {false};
	std::array<Result, 2> results {{Result::Disabled, Result::Disabled}};
	const std::array<uintptr_t, 2> tokens {{0, MAX_TOKEN}};
	const std::array<uint32_t, 2> values {{MAXIMUM, 0}};
	std::array<std::thread, 2> callers;
	for (size_t index = 0; index < callers.size(); ++index)
		callers[index] = std::thread([&, index] {
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			results[index] = Voice::InitializeModel(reinterpret_cast<const void*>(tokens[index]), values[index]);
		});
	start.store(true, std::memory_order_release);
	for (auto& caller: callers) caller.join();
	const bool first = results[0] == Result::Ok && results[1] == Result::AlreadyInitialized && State(true, tokens[0], values[0]);
	const bool second = results[1] == Result::Ok && results[0] == Result::AlreadyInitialized && State(true, tokens[1], values[1]);
	Voice::ResetSessionState();
	return Check((first || second) && State(false), "concurrent initialization failed to commit exactly one intact input pair");
}

bool PortLifecycle(Mapping& memory)
{
	Voice::ResetSessionState();
	Fill(memory);
	if (!Rejected(memory, 0, 0, Result::NotInitialized) ||
	    !Check(Voice::DeletePortModel(0) == Result::NotInitialized && Voice::StartModel() == Result::NotInitialized &&
	               Voice::StopModel() == Result::NotInitialized && Voice::EndModel() == Result::NotInitialized && State(false),
	           "uninitialized port/lifecycle operation did not preserve state") ||
	    !Check(Voice::Init(nullptr, 7) == 0 && State(true, 0, 7), "port initialization failed")) return false;
	std::array<uint32_t, 6> ids {};
	std::array<std::array<uint8_t, DESCRIPTOR_BYTES>, 6> descriptors {};
	for (uint32_t type = 0; type < ids.size(); ++type)
	{
		const auto seed = static_cast<uint8_t>(11 + type * 41);
		Fill(memory, type, seed);
		descriptors[type] = Descriptor(type, seed);
		if (!Create(memory, &ids[type], OUTPUT, DESCRIPTOR, type == 0) ||
		    !Check(type == 0 ? ids[type] == 1 : ids[type] == ids[type - 1] + 1, "initial identifiers are not positive consecutive values"))
			return false;
		// A query is an owned copy; neither caller storage nor that copy owns the port.
		auto copy = Voice::GetPortSnapshot(ids[type]);
		copy.descriptor.fill(0);
		std::memset(memory.Data(DESCRIPTOR), 0xff, DESCRIPTOR_BYTES);
		if (!Check(Port(ids[type], descriptors[type]), "port borrowed caller storage or exposed mutable snapshot storage")) return false;
	}
	if (!Check(Voice::DeletePort(ids[2]) == 0 && !Voice::GetPortSnapshot(ids[2]).live && State(true, 0, 7, 5),
	           "delete did not release exactly the selected resource")) return false;
	for (uint32_t invalid: {uint32_t(0), ids[2], MAXIMUM})
		if (!Check(Voice::DeletePortModel(invalid) == Result::InvalidPort && State(true, 0, 7, 5),
		           "unknown or deleted identifier changed the resource set")) return false;
	for (size_t i = 0; i < ids.size(); ++i)
		if (i != 2 && !Check(Port(ids[i], descriptors[i]), "delete changed another port")) return false;
	Fill(memory, 5);
	uint32_t replacement = 0;
	if (!Create(memory, &replacement) || !Check(replacement == ids.back() + 1, "delete recycled an identifier")) return false;
	if (!Check(Lumen::Libs::ResetSessionState() && State(false), "aggregate reset retained port state")) return false;
	for (uint32_t id: ids)
		if (!Check(!Voice::GetPortSnapshot(id).live, "reset retained an earlier port")) return false;
	if (!Check(!Voice::GetPortSnapshot(replacement).live && Voice::InitializeModel(nullptr, 8) == Result::Ok &&
	               Voice::DeletePortModel(replacement) == Result::InvalidPort, "reinitialization made a stale port valid")) return false;
	uint32_t next = 0;
	if (!Create(memory, &next) || !Check(next == replacement + 1, "reset rewound the process identifier allocator")) return false;
	Voice::ResetSessionState();
	Voice::ResetSessionState();
	return Check(State(false) && !Voice::GetPortSnapshot(next).live, "repeated reset retained a port");
}

bool PortMemory(Mapping& memory, size_t page_size)
{
	Voice::ResetSessionState();
	Fill(memory, 1);
	if (!Check(Voice::InitializeModel(nullptr, 0) == Result::Ok, "memory-test initialization failed")) return false;
	uint32_t first = 0;
	if (!Create(memory, &first)) return false;
	const auto retained = Descriptor(1);
	Mapping freed(page_size);
	if (!Check(freed.Valid(), "freed fixture allocation failed")) return false;
	const auto unmapped = freed.Address();
	if (!Check(freed.Free(), "freed fixture release failed")) return false;
	constexpr auto maximum = std::numeric_limits<uint64_t>::max();
	for (uint64_t invalid: {uint64_t(0), uint64_t(1), unmapped, maximum, maximum - DESCRIPTOR_BYTES + 2})
		if (!Rejected(memory, 0, invalid, Result::InvalidDescriptor)) return false;
	for (uint32_t type: {uint32_t(6), uint32_t(0x80000000), MAXIMUM})
	{
		Store32(memory.Data(DESCRIPTOR), type);
		if (!Rejected(memory, 0, memory.Address(DESCRIPTOR), Result::UnsupportedDescriptor)) return false;
	}
	Store32(memory.Data(DESCRIPTOR), 1);
	for (uint64_t invalid: {uint64_t(0), uint64_t(1), unmapped, maximum, maximum - 2})
		if (!Rejected(memory, invalid, memory.Address(DESCRIPTOR), Result::InvalidOutput)) return false;
	uint32_t second = 0;
	if (!Create(memory, &second) || !Check(second == first + 1 && Port(first, retained), "failed validation consumed an ID or changed a live port"))
		return false;

	Mapping protected_memory(page_size);
	if (!Check(protected_memory.Valid(), "protected fixture allocation failed")) return false;
	Fill(protected_memory, 4, 0xff);
	const auto protected_before = Snapshot(protected_memory);
	if (!Check(protected_memory.Protect(0, page_size, Vm::Mode::Read), "read-only fixture protection failed") ||
	    !Rejected(memory, protected_memory.Address(OUTPUT), memory.Address(DESCRIPTOR), Result::InvalidOutput)) return false;
	auto expected = Snapshot(memory);
	if (!Check(Voice::CreatePortModel(memory.Address(OUTPUT), protected_memory.Address(DESCRIPTOR)) == Result::Ok,
	           "read-only descriptor was rejected")) return false;
	uint32_t read_only_id = Load32(memory.Data(OUTPUT));
	Store32(expected.data() + OUTPUT, read_only_id);
	if (!Check(Port(read_only_id, Descriptor(4, 0xff)) && Matches(memory, expected) && Matches(protected_memory, protected_before),
	           "read-only input was changed or its descriptor was not retained") ||
	    !Check(protected_memory.Protect(0, page_size, Vm::Mode::NoAccess), "no-access fixture protection failed") ||
	    !Rejected(memory, memory.Address(OUTPUT), protected_memory.Address(DESCRIPTOR), Result::InvalidDescriptor) ||
	    !Rejected(memory, protected_memory.Address(OUTPUT), memory.Address(DESCRIPTOR), Result::InvalidOutput) ||
	    !Check(protected_memory.Protect(0, page_size, Vm::Mode::ReadWrite), "protected fixture restore failed") ||
	    !Check(Matches(protected_memory, protected_before), "rejected protected access wrote to the fixture")) return false;

	Mapping boundary(page_size * 2);
	if (!Check(boundary.Valid(), "boundary fixture allocation failed")) return false;
	Fill(boundary);
	const auto at_edge = Descriptor(3, 0xb7);
	std::memcpy(boundary.Data(page_size - DESCRIPTOR_BYTES), at_edge.data(), at_edge.size());
	const auto boundary_before = Snapshot(boundary);
	if (!Check(boundary.Protect(page_size, page_size, Vm::Mode::NoAccess), "boundary protection failed")) return false;
	for (size_t readable: {size_t(1), DESCRIPTOR_BYTES - 1})
		if (!Rejected(memory, memory.Address(OUTPUT), boundary.Address(page_size - readable), Result::InvalidDescriptor)) return false;
	expected = Snapshot(memory);
	if (!Check(Voice::CreatePortModel(memory.Address(OUTPUT), boundary.Address(page_size - DESCRIPTOR_BYTES)) == Result::Ok,
	           "complete descriptor ending at a protected page was rejected")) return false;
	const auto edge_id = Load32(memory.Data(OUTPUT));
	Store32(expected.data() + OUTPUT, edge_id);
	if (!Check(Port(edge_id, at_edge) && Matches(memory, expected), "exact-boundary descriptor was not retained") ||
	    !Rejected(memory, boundary.Address(page_size - 3), memory.Address(DESCRIPTOR), Result::InvalidOutput) ||
	    !Check(std::memcmp(boundary.Data(), boundary_before.data(), page_size) == 0,
	           "rejected crossing output changed its accessible prefix") ||
	    !Check(Voice::CreatePortModel(boundary.Address(page_size - 4), memory.Address(DESCRIPTOR)) == Result::Ok,
	           "four-byte output ending at a protected page was rejected")) return false;
	const auto output_id = Load32(boundary.Data(page_size - 4));
	auto boundary_expected = boundary_before;
	Store32(boundary_expected.data() + page_size - 4, output_id);
	if (!Check(Port(output_id, retained) && boundary.Protect(page_size, page_size, Vm::Mode::ReadWrite) &&
	               Matches(boundary, boundary_expected), "boundary output touched bytes outside its four-byte interval")) return false;

	// Two readable regions with different protections must form one valid input.
	const auto crossing = Descriptor(2, 0x69);
	std::memcpy(boundary.Data(page_size - 7), crossing.data(), crossing.size());
	const auto crossing_before = Snapshot(boundary);
	if (!Check(boundary.Protect(page_size, page_size, Vm::Mode::Read), "cross-region protection failed")) return false;
	expected = Snapshot(memory);
	if (!Check(Voice::CreatePortModel(memory.Address(OUTPUT), boundary.Address(page_size - 7)) == Result::Ok,
	           "readable descriptor crossing adjacent regions was rejected")) return false;
	const auto crossing_id = Load32(memory.Data(OUTPUT));
	Store32(expected.data() + OUTPUT, crossing_id);
	if (!Check(Port(crossing_id, crossing) && Matches(memory, expected) && Matches(boundary, crossing_before),
	           "cross-region input or unrelated output bytes changed") ||
	    !Check(boundary.Protect(page_size, page_size, Vm::Mode::ReadWrite), "cross-region restore failed")) return false;

	for (size_t output: {DESCRIPTOR - 3, DESCRIPTOR, DESCRIPTOR + 1, DESCRIPTOR + 7, DESCRIPTOR + 20, DESCRIPTOR + 23})
	{
		Fill(memory, 5, 0x93);
		uint32_t alias_id = 0;
		if (!Create(memory, &alias_id, output, DESCRIPTOR, true) ||
		    !Check(Voice::DeletePortModel(alias_id) == Result::Ok, "overlap fixture cleanup failed")) return false;
	}
	Voice::ResetSessionState();
	return Check(State(false), "memory-test reset retained state");
}

bool PortCapacity(Mapping& memory)
{
	Voice::ResetSessionState();
	Fill(memory, 2);
	if (!Check(Voice::PORT_CAPACITY == 64 && Voice::InitializeModel(nullptr, 0) == Result::Ok,
	           "local capacity or initialization changed")) return false;
	std::array<uint32_t, 64> ids {};
	for (size_t i = 0; i < ids.size(); ++i)
		if (!Create(memory, &ids[i]) || !Check(i == 0 || ids[i] == ids[i - 1] + 1, "capacity allocation skipped an ID")) return false;
	if (!Rejected(memory, 0, 0, Result::InvalidDescriptor)) return false;
	Store32(memory.Data(DESCRIPTOR), MAXIMUM);
	if (!Rejected(memory, 0, memory.Address(DESCRIPTOR), Result::UnsupportedDescriptor)) return false;
	Store32(memory.Data(DESCRIPTOR), 2);
	if (!Rejected(memory, 0, memory.Address(DESCRIPTOR), Result::InvalidOutput) ||
	    !Rejected(memory, memory.Address(OUTPUT), memory.Address(DESCRIPTOR), Result::CapacityExceeded)) return false;
	for (auto id: ids)
		if (!Check(Port(id, Descriptor(2)), "capacity rejection corrupted a live resource")) return false;
	if (!Check(Voice::DeletePortModel(ids[17]) == Result::Ok && State(true, 0, 0, 63), "deletion did not restore capacity")) return false;
	uint32_t replacement = 0;
	if (!Create(memory, &replacement) || !Check(replacement == ids.back() + 1 && State(true, 0, 0, 64),
	                                           "failed creation consumed an ID or deletion recycled one")) return false;
	Voice::ResetSessionState();
	return Check(!Voice::GetPortSnapshot(replacement).live && State(false), "capacity reset retained resources");
}

bool ConcurrentPorts(Mapping& memory)
{
	Voice::ResetSessionState();
	Fill(memory);
	if (!Check(memory.Size() >= 4096 && Voice::InitializeModel(nullptr, 0) == Result::Ok, "concurrency fixture setup failed")) return false;
	constexpr size_t count = 64;
	constexpr size_t stride = 48;
	for (size_t i = 0; i < count; ++i)
	{
		const auto descriptor = Descriptor(static_cast<uint32_t>(i % 6), static_cast<uint8_t>(i * 3));
		std::memcpy(memory.Data(i * stride + 3), descriptor.data(), descriptor.size());
	}
	auto expected = Snapshot(memory);
	std::array<Result, count> results {};
	std::array<uint32_t, count> ids {};
	std::array<std::thread, 8> workers;
	std::atomic<bool> start {false};
	for (size_t worker = 0; worker < workers.size(); ++worker)
		workers[worker] = std::thread([&, worker] {
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			for (size_t i = worker; i < count; i += workers.size())
				results[i] = Voice::CreatePortModel(memory.Address(i * stride + 33), memory.Address(i * stride + 3));
		});
	start.store(true, std::memory_order_release);
	for (auto& worker: workers) worker.join();
	for (size_t i = 0; i < count; ++i)
	{
		ids[i] = Load32(memory.Data(i * stride + 33));
		Store32(expected.data() + i * stride + 33, ids[i]);
		if (!Check(results[i] == Result::Ok && ids[i] > 0 &&
		               Port(ids[i], Descriptor(static_cast<uint32_t>(i % 6), static_cast<uint8_t>(i * 3))),
		           "concurrent creation returned an incomplete or mismatched resource")) return false;
	}
	auto ordered = ids;
	std::sort(ordered.begin(), ordered.end());
	for (size_t i = 1; i < count; ++i)
		if (!Check(ordered[i] == ordered[i - 1] + 1, "concurrent identifiers were duplicated or skipped")) return false;
	if (!Check(Matches(memory, expected) && State(true, 0, 0, 64), "concurrent creation changed unrelated bytes or lost a resource")) return false;
	std::array<std::array<Result, count>, 2> deleted {};
	start.store(false, std::memory_order_relaxed);
	for (size_t worker = 0; worker < workers.size(); ++worker)
		workers[worker] = std::thread([&, worker] {
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			for (size_t i = worker % 4; i < count; i += 4) deleted[worker / 4][i] = Voice::DeletePortModel(ids[i]);
		});
	start.store(true, std::memory_order_release);
	for (auto& worker: workers) worker.join();
	for (size_t i = 0; i < count; ++i)
		if (!Check(((deleted[0][i] == Result::Ok && deleted[1][i] == Result::InvalidPort) ||
		            (deleted[1][i] == Result::Ok && deleted[0][i] == Result::InvalidPort)) && !Voice::GetPortSnapshot(ids[i]).live,
		           "concurrent deletion did not remove each resource exactly once")) return false;
	if (!Check(State(true) && Matches(memory, expected), "concurrent deletion retained ports or changed guest storage")) return false;
	Fill(memory, 1);
	uint32_t next = 0;
	if (!Create(memory, &next) || !Check(next == ordered.back() + 1, "concurrent deletion changed the allocator")) return false;
	Voice::ResetSessionState();
	return Check(State(false), "concurrency reset retained state");
}

bool StartStopEnd(Mapping& memory)
{
	Voice::ResetSessionState();
	Fill(memory, 3);
	if (!Check(Voice::InitializeModel(nullptr, 19) == Result::Ok && Voice::StopModel() == Result::AlreadyStopped && State(true, 0, 19),
	           "initial stopped state changed") ||
	    !Check(Voice::StartModel() == Result::Ok && State(true, 0, 19, 0, true) && Voice::StopModel() == Result::Ok,
	           "administrative start incorrectly requires a port")) return false;
	uint32_t before = 0;
	if (!Create(memory, &before)) return false;
	const auto original = Snapshot(memory);
	const std::array<const void*, 5> tokens {{nullptr, reinterpret_cast<const void*>(uintptr_t(1)),
	    reinterpret_cast<const void*>(MAX_TOKEN), reinterpret_cast<const void*>(uintptr_t(0x12345678abcdef91)), memory.Data()}};
	for (auto token: tokens)
	{
		if (!Check(Voice::Start(token) == 0 && State(true, 0, 19, 1, true) &&
		               Voice::StartModel() == Result::AlreadyStarted && Port(before, Descriptor(3)) && Matches(memory, original),
		           "start accessed an opaque token or changed ports") ||
		    !Check(Voice::Stop() == 0 && Voice::StopModel() == Result::AlreadyStopped && State(true, 0, 19, 1),
		           "stop did not preserve initialized ports")) return false;
	}
	if (!Check(memory.Protect(0, memory.Size(), Vm::Mode::NoAccess), "opaque Start fixture protection failed") ||
	    !Check(Voice::Start(memory.Data()) == 0 && Voice::Stop() == 0, "Start accessed its opaque inaccessible token") ||
	    !Check(memory.Protect(0, memory.Size(), Vm::Mode::ReadWrite) && Matches(memory, original),
	           "Start changed its opaque fixture")) return false;
	if (!Check(Voice::StartModel() == Result::Ok, "start before CRUD failed")) return false;
	uint32_t active = 0;
	if (!Create(memory, &active) || !Check(Voice::DeletePortModel(before) == Result::Ok && State(true, 0, 19, 1, true),
	                                    "port CRUD while started changed lifecycle")) return false;
	if (!Check(Voice::End() == 0 && State(false) && !Voice::GetPortSnapshot(active).live &&
	               Voice::EndModel() == Result::NotInitialized && Voice::StartModel() == Result::NotInitialized &&
	               Voice::StopModel() == Result::NotInitialized, "End did not clear all administrative state")) return false;
	if (!Check(Voice::InitializeModel(nullptr, 0) == Result::Ok && Voice::DeletePortModel(active) == Result::InvalidPort,
	           "End followed by initialization revalidated a stale port")) return false;
	uint32_t next = 0;
	if (!Create(memory, &next) || !Check(next == active + 1 && Voice::StartModel() == Result::Ok, "End rewound IDs or prevented restart")) return false;
	Voice::ResetSessionState();
	return Check(State(false) && !Voice::GetPortSnapshot(next).live, "reset retained started state or ports");
}

bool PortInfoLifecycle(Mapping& memory)
{
	Voice::ResetSessionState();
	Fill(memory);
	if (!InfoRejected(memory, MAXIMUM, MAX_TOKEN, Result::NotInitialized) ||
	    !Check(Voice::InitializeModel(nullptr, 73) == Result::Ok, "info initialization failed")) return false;
	uint32_t previous = 0;
	for (uint32_t type = 0; type < 6; ++type)
	{
		Fill(memory, type);
		uint32_t id = 0;
		if (!Create(memory, &id) || !Check(previous == 0 || id == previous + 1, "info query consumed an identifier")) return false;
		const auto descriptor = Descriptor(type);
		std::memset(memory.Data(DESCRIPTOR), 0xff, DESCRIPTOR_BYTES);
		std::memset(memory.Data(OUTPUT), 0xff, INFO_BYTES);
		auto copied = Voice::GetPortSnapshot(id);
		copied.descriptor.fill(0);
		if (!Info(memory, id, type, OUTPUT, true) || !Info(memory, id, type, DESCRIPTOR) ||
		    !Check(Port(id, descriptor), "info query borrowed a mutated guest descriptor or snapshot") ||
		    !Check(Voice::StartModel() == Result::Ok, "info start failed") || !Info(memory, id, type) ||
		    !Check(Voice::StopModel() == Result::Ok, "info stop failed") || !Info(memory, id, type)) return false;
		for (uint32_t invalid: {uint32_t(0), MAXIMUM})
			if (!InfoRejected(memory, invalid, MAX_TOKEN, Result::InvalidPort)) return false;
		if (!InfoRejected(memory, id, 0, Result::InvalidInfoOutput) ||
		    !Check(Voice::DeletePortModel(id) == Result::Ok, "info fixture deletion failed") ||
		    !InfoRejected(memory, id, memory.Address(OUTPUT), Result::InvalidPort)) return false;
		previous = id;
	}
	Fill(memory, 5);
	uint32_t ended = 0;
	if (!Create(memory, &ended) || !Check(ended == previous + 1 && Voice::EndModel() == Result::Ok, "info End fixture failed") ||
	    !InfoRejected(memory, ended, MAX_TOKEN, Result::NotInitialized) ||
	    !Check(Voice::InitializeModel(nullptr, 74) == Result::Ok, "info reinitialize after End failed") ||
	    !InfoRejected(memory, ended, memory.Address(OUTPUT), Result::InvalidPort)) return false;
	uint32_t reset = 0;
	if (!Create(memory, &reset) || !Check(reset == ended + 1, "info failure after End consumed an ID")) return false;
	Voice::ResetSessionState();
	if (!InfoRejected(memory, reset, MAX_TOKEN, Result::NotInitialized) ||
	    !Check(Voice::InitializeModel(nullptr, 75) == Result::Ok, "info reinitialize after reset failed") ||
	    !InfoRejected(memory, reset, memory.Address(OUTPUT), Result::InvalidPort)) return false;
	uint32_t next = 0;
	if (!Create(memory, &next) || !Check(next == reset + 1, "info failure after reset consumed an ID")) return false;
	Voice::ResetSessionState();
	return Check(State(false), "info lifecycle cleanup retained state");
}

bool PortInfoMemory(Mapping& memory, size_t page_size)
{
	Voice::ResetSessionState();
	Fill(memory, 4);
	if (!Check(Voice::InitializeModel(nullptr, 0) == Result::Ok, "info memory initialization failed")) return false;
	uint32_t id = 0;
	if (!Create(memory, &id)) return false;
	Mapping freed(page_size);
	if (!Check(freed.Valid(), "info freed fixture allocation failed")) return false;
	const auto unmapped = freed.Address();
	if (!Check(freed.Free(), "info freed fixture release failed")) return false;
	for (uint64_t output: {uint64_t(0), uint64_t(1), unmapped, uint64_t(MAX_TOKEN), uint64_t(MAX_TOKEN - INFO_BYTES + 2)})
		if (!InfoRejected(memory, id, output, Result::InvalidInfoOutput)) return false;
	if (!Info(memory, id, 4, OUTPUT) || !Info(memory, id, 4, OUTPUT + 1)) return false;

	Mapping protected_memory(page_size);
	if (!Check(protected_memory.Valid(), "info protected fixture allocation failed")) return false;
	Fill(protected_memory, 2);
	const auto protected_before = Snapshot(protected_memory);
	for (auto mode: {Vm::Mode::Read, Vm::Mode::NoAccess})
	{
		if (!Check(protected_memory.Protect(0, page_size, mode), "info output protection failed") ||
		    !InfoRejected(memory, id, protected_memory.Address(OUTPUT), Result::InvalidInfoOutput) ||
		    !Check(protected_memory.Protect(0, page_size, Vm::Mode::ReadWrite), "info output restore failed") ||
		    !Check(Matches(protected_memory, protected_before), "rejected info output changed protected bytes")) return false;
	}
	Mapping boundary(page_size * 2);
	if (!Check(boundary.Valid(), "info boundary allocation failed")) return false;
	Fill(boundary);
	auto expected = Snapshot(boundary);
	if (!Check(boundary.Protect(page_size, page_size, Vm::Mode::NoAccess), "info boundary protection failed") ||
	    !InfoRejected(memory, id, boundary.Address(page_size - INFO_BYTES + 1), Result::InvalidInfoOutput) ||
	    !Check(boundary.Protect(page_size, page_size, Vm::Mode::ReadWrite) && Matches(boundary, expected),
	           "partly writable info output changed its writable prefix")) return false;
	ExpectedInfo(expected.data() + page_size - INFO_BYTES, 4);
	if (!Check(boundary.Protect(page_size, page_size, Vm::Mode::NoAccess), "info exact-tail protection failed") ||
	    !Check(Voice::GetPortInfoModel(id, boundary.Address(page_size - INFO_BYTES)) == Result::Ok,
	           "exact 28-byte info tail validated inaccessible padding") ||
	    !Check(boundary.Protect(page_size, page_size, Vm::Mode::ReadWrite) && Matches(boundary, expected),
	           "exact info tail wrote padding or adjacent bytes") ||
	    !Check(Port(id, Descriptor(4)) && State(true, 0, 0, 1), "info memory queries changed port state")) return false;
	Voice::ResetSessionState();
	return Check(State(false), "info memory cleanup retained state");
}

bool ConcurrentPortInfo(Mapping& memory)
{
	for (unsigned operation = 0; operation < 3; ++operation)
	{
		for (unsigned round = 0; round < 16; ++round)
		{
			Voice::ResetSessionState();
			Fill(memory, 3);
			if (!Check(Voice::InitializeModel(nullptr, 0) == Result::Ok, "concurrent info initialization failed")) return false;
			uint32_t id = 0;
			if (!Create(memory, &id)) return false;
			const auto before = Snapshot(memory);
			auto expected = before;
			ExpectedInfo(expected.data() + OUTPUT, 3);
			std::atomic<bool> start {false};
			Result queried = Result::Disabled;
			Result removed = Result::Disabled;
			std::thread reader([&] {
				while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
				queried = Voice::GetPortInfoModel(id, memory.Address(OUTPUT));
			});
			std::thread remover([&] {
				while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
				if (operation == 0) removed = Voice::DeletePortModel(id);
				else if (operation == 1) removed = Voice::EndModel();
				else { Voice::ResetSessionState(); removed = Result::Ok; }
			});
			start.store(true, std::memory_order_release);
			reader.join();
			remover.join();
			const auto rejected = operation == 0 ? Result::InvalidPort : Result::NotInitialized;
			if (!Check(removed == Result::Ok && (queried == Result::Ok ? Matches(memory, expected) :
			                                       queried == rejected && Matches(memory, before)) &&
			               !Voice::GetPortSnapshot(id).live && State(operation == 0),
			           "concurrent query/teardown returned a partial output or inconsistent outcome") ||
			    !InfoRejected(memory, id, memory.Address(OUTPUT), rejected)) return false;
		}
	}
	Voice::ResetSessionState();
	return Check(State(false), "concurrent info cleanup retained state");
}

bool EnabledPorts()
{
	const long host_page_size = sysconf(_SC_PAGESIZE);
	if (!Check(host_page_size > 0, "host page size unavailable")) return false;
	const auto page_size = static_cast<size_t>(host_page_size);
	Mapping memory(page_size);
	return Check(memory.Valid(), "port fixture allocation failed") && PortLifecycle(memory) && PortMemory(memory, page_size) &&
	       PortCapacity(memory) && ConcurrentPorts(memory) && StartStopEnd(memory) && PortInfoLifecycle(memory) &&
	       PortInfoMemory(memory, page_size) && ConcurrentPortInfo(memory);
}

bool Latch(bool expected)
{
	if (!Check(Voice::ExperimentalEnabled() == expected, "initial selector disagreed with the expected value") ||
	    !Check(setenv(SELECTOR, expected ? "0" : "1", 1) == 0, "selector mutation failed")) return false;
	Voice::ResetSessionState();
	if (!Check(Lumen::Libs::ResetSessionState(), "latch-test aggregate reset failed") ||
	    !Selectors(expected, false) || !Registration(expected)) return false;
	const auto result = Voice::InitializeModel(reinterpret_cast<const void*>(uintptr_t(11)), 23);
	const bool passed = Check(result == (expected ? Result::Ok : Result::Disabled) && State(expected, expected ? 11 : 0, expected ? 23 : 0),
	                          "environment mutation or reset changed the process latch");
	Voice::ResetSessionState();
	return passed && Check(Voice::ExperimentalEnabled() == expected && State(false), "direct reset changed the latch");
}

bool Coexist()
{
	if (!Selectors(true, true) || !Registration(true) ||
	    !Check(Network::ResetForNextSession() && Lumen::Libs::ResetSessionState(), "coexistence reset failed") ||
	    !Check(CreateRequest() == 1 && CreateRequest() == 2 && Live(1) && Live(2) && InitializeOtherServices(),
	           "coexistence fixture setup failed") ||
	    !Check(Voice::InitializeModel(reinterpret_cast<const void*>(uintptr_t(1)), 42) == Result::Ok &&
	               Voice::InitializeModel(nullptr, MAXIMUM) == Result::AlreadyInitialized && State(true, 1, 42) &&
	               OtherState(true) && Live(1) && Live(2), "Voice initialization changed another service")) return false;
	Voice::ResetSessionState();
	if (!Check(State(false) && OtherState(true) && Live(1) && Live(2), "Voice reset changed another service") ||
	    !Check(Voice::InitializeModel(reinterpret_cast<const void*>(MAX_TOKEN), 23) == Result::Ok, "Voice reinitialization failed")) return false;
	Message::ResetSessionState();
	Rudp::ResetSessionState();
	if (!Check(OtherState(false) && State(true, MAX_TOKEN, 23) && Live(1) && Live(2), "other service resets changed Voice or NP state") ||
	    !Check(InitializeOtherServices() && Np::NpDeleteRequest(1) == 0 && !Live(1) && Live(2) &&
	               State(true, MAX_TOKEN, 23) && OtherState(true), "NP deletion changed Voice or another model")) return false;
	// Live() releases its guard before aggregate reset acquires the NP mutex.
	if (!Check(Lumen::Libs::ResetSessionState() && State(false) && OtherState(false) && !Live(1) && !Live(2) &&
	               CreateRequest() == 3 && Live(3), "aggregate reset retained state or changed the Network allocator")) return false;
	return Check(Network::ResetForNextSession() && !Live(3), "coexistence cleanup retained a request") && Selectors(true, true);
}

bool PortCoexist()
{
	const long page_size = sysconf(_SC_PAGESIZE);
	if (!Check(page_size > 0, "coexistence page size unavailable")) return false;
	Mapping memory(static_cast<size_t>(page_size));
	if (!Check(memory.Valid() && Network::ResetForNextSession() && Lumen::Libs::ResetSessionState(),
	           "port coexistence reset failed")) return false;
	Fill(memory, 4);
	if (!Check(CreateRequest() == 1 && Live(1) && InitializeOtherServices() && Voice::InitializeModel(nullptr, 41) == Result::Ok,
	           "port coexistence initialization failed")) return false;
	uint32_t first = 0;
	if (!Create(memory, &first) || !Check(Voice::StartModel() == Result::Ok && OtherState(true) && Live(1),
	                                    "Voice creation or start changed another service")) return false;
	if (!Info(memory, first, 4) || !Check(OtherState(true) && Live(1), "Voice info changed another service")) return false;
	Message::ResetSessionState();
	Rudp::ResetSessionState();
	if (!Check(OtherState(false) && Port(first, Descriptor(4)) && State(true, 0, 41, 1, true) && Live(1),
	           "another service reset changed a live Voice port") ||
	    !Check(InitializeOtherServices() && Voice::EndModel() == Result::Ok && !Voice::GetPortSnapshot(first).live &&
	               State(false) && OtherState(true) && Live(1), "Voice End changed another service") ||
	    !Check(Voice::InitializeModel(nullptr, 0) == Result::Ok, "port coexistence reinitialization failed")) return false;
	uint32_t next = 0;
	if (!Create(memory, &next) || !Check(next == first + 1 && Np::NpDeleteRequest(1) == 0 && !Live(1) &&
	                                     OtherState(true) && Port(next, Descriptor(4)), "NP deletion or Voice End changed another allocator/resource"))
		return false;
	return Check(Lumen::Libs::ResetSessionState() && State(false) && !Voice::GetPortSnapshot(next).live &&
	                 OtherState(false) && Network::ResetForNextSession(), "aggregate reset retained a coexisting service") &&
	       Selectors(true, true);
}

constexpr size_t NGS2_INFO_BYTES = 24;
constexpr size_t NGS2_INFO_OFFSET = 257; // Deliberately unaligned.
constexpr size_t NGS2_OUTPUT_BASE = 701;
using WaveformOutputs = std::array<uint64_t, 4>;

constexpr uint32_t Atrac9Config(uint32_t sample_rate_index, uint32_t channel_config_index,
                                uint32_t frame_bytes, uint32_t superframe_index,
                                uint32_t validation_bit = 0, uint32_t reserved = 0,
                                uint32_t magic = 0xfe)
{
	return (magic << 24) | ((sample_rate_index & 0xf) << 20) |
	       ((channel_config_index & 0x7) << 17) | ((validation_bit & 0x1) << 16) |
	       (((frame_bytes - 1) & 0x7ff) << 5) | ((superframe_index & 0x3) << 3) |
	       (reserved & 0x7);
}

static_assert(Atrac9Config(7, 0, 64, 2) == 0xfe7007f0);

void PrepareWaveform(Mapping& memory, uint32_t type = 0x12, uint32_t channels = 2,
                     uint32_t sample_rate = 48000, uint32_t config_data = 0,
                     uint32_t frame_offset = 0, uint32_t frame_margin = 0,
                     size_t info_offset = NGS2_INFO_OFFSET)
{
	for (size_t byte = 0; byte < memory.Size(); ++byte)
		memory.Data()[byte] = static_cast<uint8_t>(0x5d + byte * 29);
	const std::array<uint32_t, 6> fields {{type, channels, sample_rate, config_data, frame_offset, frame_margin}};
	for (size_t field = 0; field < fields.size(); ++field)
		Store32(memory.Data(info_offset + field * sizeof(uint32_t)), fields[field]);
}

WaveformOutputs DefaultWaveformOutputs(const Mapping& memory)
{
	return {{memory.Address(NGS2_OUTPUT_BASE), memory.Address(NGS2_OUTPUT_BASE + 13),
	         memory.Address(NGS2_OUTPUT_BASE + 26), memory.Address(NGS2_OUTPUT_BASE + 39)}};
}

bool WaveformComplete(Mapping& memory, uint64_t info, const WaveformOutputs& outputs,
                      const std::array<uint32_t, 4>& values, const char* message)
{
	auto expected = Snapshot(memory);
	for (size_t index = 0; index < outputs.size(); ++index)
	{
		if (outputs[index] == 0) continue;
		const size_t offset = static_cast<size_t>(outputs[index] - memory.Address());
		if (offset > expected.size() - sizeof(uint32_t)) return Check(false, "test output is outside its fixture");
		Store32(expected.data() + offset, values[index]);
	}
	const int32_t result = g_get_waveform_frame_info(
	    reinterpret_cast<const Ngs2::Ngs2WaveformInfo*>(info), reinterpret_cast<uint32_t*>(outputs[0]),
	    reinterpret_cast<uint32_t*>(outputs[1]), reinterpret_cast<uint32_t*>(outputs[2]),
	    reinterpret_cast<uint32_t*>(outputs[3]));
	return Check(result == 0, message) && Check(Matches(memory, expected), "NGS2 waveform output or adjacent canary changed");
}

bool WaveformRejected(Mapping& memory, uint64_t info, const WaveformOutputs& outputs,
                      WaveformResult expected, const char* message)
{
	const auto before = Snapshot(memory);
	const auto result = Ngs2::Ngs2GetWaveformFrameInfoModel(info, outputs[0], outputs[1], outputs[2], outputs[3]);
	return Check(result == expected, message) &&
	       Check(Matches(memory, before), "rejected NGS2 waveform query changed input, output, or a canary");
}

bool Ngs2PcmMatrix(Mapping& memory)
{
	constexpr std::array<uint32_t, 12> bytes_per_sample {{1, 1, 2, 2, 3, 3, 4, 4, 4, 4, 8, 8}};
	for (uint32_t type = 0x10; type <= 0x1b; ++type)
	{
		for (uint32_t channels: {1U, 2U, 6U, 8U})
		{
			PrepareWaveform(memory, type, channels);
			if (!WaveformComplete(memory, memory.Address(NGS2_INFO_OFFSET), DefaultWaveformOutputs(memory),
			                      {{channels * bytes_per_sample[type - 0x10], 1, 1, 0}},
			                      "valid PCM type/channel pair did not complete through the registered wrapper")) return false;
		}
	}
	for (uint32_t sample_rate: {1U, MAXIMUM})
	{
		PrepareWaveform(memory, 0x1b, 8, sample_rate);
		if (!WaveformComplete(memory, memory.Address(NGS2_INFO_OFFSET), DefaultWaveformOutputs(memory),
		                      {{64, 1, 1, 0}}, "nonzero sample-rate boundary was rejected")) return false;
	}
	return true;
}

bool Ngs2NativeWaveformMetadata(Mapping& memory)
{
	struct Vector
	{
		uint32_t type;
		uint32_t channels;
		uint32_t sample_rate;
		uint32_t config_data;
		std::array<uint32_t, 4> expected;
	};
	// Literal observations from original bounded PS4 probes LMNQ10034 (all
	// sixteen config rate indices, SF1/SF4) and LMNQ10033 (channels, PCM and
	// declared 24/48 kHz). These API outputs do not describe decoder PCM geometry.
	constexpr std::array<Vector, 32> rate_vectors {{
	    {0x40u, 1u, 11025u, 0xfe001fe0u, {{256, 0, 1, 0}}},
	    {0x40u, 1u, 11025u, 0xfe001ff0u, {{1024, 0, 4, 0}}},
	    {0x40u, 1u, 12000u, 0xfe101fe0u, {{256, 64, 1, 64}}},
	    {0x40u, 1u, 12000u, 0xfe101ff0u, {{1024, 256, 4, 64}}},
	    {0x40u, 1u, 16000u, 0xfe201fe0u, {{256, 0, 1, 0}}},
	    {0x40u, 1u, 16000u, 0xfe201ff0u, {{1024, 0, 4, 0}}},
	    {0x40u, 1u, 22050u, 0xfe301fe0u, {{256, 0, 1, 0}}},
	    {0x40u, 1u, 22050u, 0xfe301ff0u, {{1024, 0, 4, 0}}},
	    {0x40u, 1u, 24000u, 0xfe401fe0u, {{256, 128, 1, 128}}},
	    {0x40u, 1u, 24000u, 0xfe401ff0u, {{1024, 512, 4, 128}}},
	    {0x40u, 1u, 32000u, 0xfe501fe0u, {{256, 0, 1, 0}}},
	    {0x40u, 1u, 32000u, 0xfe501ff0u, {{1024, 0, 4, 0}}},
	    {0x40u, 1u, 44100u, 0xfe601fe0u, {{256, 0, 1, 0}}},
	    {0x40u, 1u, 44100u, 0xfe601ff0u, {{1024, 0, 4, 0}}},
	    {0x40u, 1u, 48000u, 0xfe701fe0u, {{256, 256, 1, 256}}},
	    {0x40u, 1u, 48000u, 0xfe701ff0u, {{1024, 1024, 4, 256}}},
	    {0x40u, 1u, 44100u, 0xfe801fe0u, {{256, 0, 1, 0}}},
	    {0x40u, 1u, 44100u, 0xfe801ff0u, {{1024, 0, 4, 0}}},
	    {0x40u, 1u, 48000u, 0xfe901fe0u, {{256, 0, 1, 0}}},
	    {0x40u, 1u, 48000u, 0xfe901ff0u, {{1024, 0, 4, 0}}},
	    {0x40u, 1u, 64000u, 0xfea01fe0u, {{256, 0, 1, 0}}},
	    {0x40u, 1u, 64000u, 0xfea01ff0u, {{1024, 0, 4, 0}}},
	    {0x40u, 1u, 88200u, 0xfeb01fe0u, {{256, 0, 1, 0}}},
	    {0x40u, 1u, 88200u, 0xfeb01ff0u, {{1024, 0, 4, 0}}},
	    {0x40u, 1u, 96000u, 0xfec01fe0u, {{256, 0, 1, 0}}},
	    {0x40u, 1u, 96000u, 0xfec01ff0u, {{1024, 0, 4, 0}}},
	    {0x40u, 1u, 128000u, 0xfed01fe0u, {{256, 0, 1, 0}}},
	    {0x40u, 1u, 128000u, 0xfed01ff0u, {{1024, 0, 4, 0}}},
	    {0x40u, 1u, 176400u, 0xfee01fe0u, {{256, 0, 1, 0}}},
	    {0x40u, 1u, 176400u, 0xfee01ff0u, {{1024, 0, 4, 0}}},
	    {0x40u, 1u, 192000u, 0xfef01fe0u, {{256, 0, 1, 0}}},
	    {0x40u, 1u, 192000u, 0xfef01ff0u, {{1024, 0, 4, 0}}},
	}};
	constexpr std::array<Vector, 24> channel_vectors {{
	    {0x40u, 1u, 48000u, 0xfe7007f0u, {{256, 1024, 4, 256}}},
	    {0x40u, 1u, 11025u, 0xfe001fe0u, {{256, 0, 1, 0}}},
	    {0x40u, 1u, 11025u, 0xfe001ff0u, {{1024, 0, 4, 0}}},
	    {0x40u, 2u, 11025u, 0xfe041fe0u, {{256, 0, 1, 0}}},
	    {0x40u, 2u, 11025u, 0xfe041ff0u, {{1024, 0, 4, 0}}},
	    {0x40u, 1u, 24000u, 0xfe401fe0u, {{256, 128, 1, 128}}},
	    {0x40u, 1u, 24000u, 0xfe401ff0u, {{1024, 512, 4, 128}}},
	    {0x40u, 2u, 24000u, 0xfe441fe0u, {{256, 128, 1, 128}}},
	    {0x40u, 2u, 24000u, 0xfe441ff0u, {{1024, 512, 4, 128}}},
	    {0x40u, 6u, 24000u, 0xfe461fe0u, {{256, 128, 1, 128}}},
	    {0x40u, 6u, 24000u, 0xfe461ff0u, {{1024, 512, 4, 128}}},
	    {0x40u, 8u, 24000u, 0xfe481fe0u, {{256, 128, 1, 128}}},
	    {0x40u, 8u, 24000u, 0xfe481ff0u, {{1024, 512, 4, 128}}},
	    {0x40u, 1u, 48000u, 0xfe701fe0u, {{256, 256, 1, 256}}},
	    {0x40u, 1u, 48000u, 0xfe701ff0u, {{1024, 1024, 4, 256}}},
	    {0x40u, 2u, 48000u, 0xfe741fe0u, {{256, 256, 1, 256}}},
	    {0x40u, 2u, 48000u, 0xfe741ff0u, {{1024, 1024, 4, 256}}},
	    {0x40u, 6u, 48000u, 0xfe761fe0u, {{256, 256, 1, 256}}},
	    {0x40u, 6u, 48000u, 0xfe761ff0u, {{1024, 1024, 4, 256}}},
	    {0x40u, 8u, 48000u, 0xfe781fe0u, {{256, 256, 1, 256}}},
	    {0x40u, 8u, 48000u, 0xfe781ff0u, {{1024, 1024, 4, 256}}},
	    {0x18u, 2u, 48000u, 0x0u, {{8, 1, 1, 0}}},
	    {0x40u, 2u, 24000u, 0xfe440ff0u, {{512, 512, 4, 128}}},
	    {0x40u, 2u, 48000u, 0xfe440ff0u, {{512, 512, 4, 128}}},
	}};
	auto check_vectors = [&](const auto& vectors) {
		for (const auto& vector: vectors)
		{
			PrepareWaveform(memory, vector.type, vector.channels, vector.sample_rate, vector.config_data);
			if (!WaveformComplete(memory, memory.Address(NGS2_INFO_OFFSET), DefaultWaveformOutputs(memory),
			                      vector.expected, "native waveform metadata observation changed"))
			{
				std::fprintf(stderr, "NGS2 metadata fixture: type=%x channels=%u rate=%u config=%08x expected=%u,%u,%u,%u\n",
				             vector.type, vector.channels, vector.sample_rate, vector.config_data,
				             vector.expected[0], vector.expected[1], vector.expected[2], vector.expected[3]);
				return false;
			}
		}
		return true;
	};
	return check_vectors(rate_vectors) && check_vectors(channel_vectors);
}

bool Ngs2Atrac9Matrix(Mapping& memory)
{
	struct Vector
	{
		uint32_t sample_rate_index;
		uint32_t sample_rate;
		uint32_t channel_config_index;
		uint32_t channels;
		uint32_t frame_bytes;
		uint32_t superframe_index;
		uint32_t frames_per_superframe;
		uint32_t metadata_samples_per_unit;
	};
	// These original vectors cross both low/high sample-rate tables, every
	// locally admitted channel count, both admitted superframe sizes, every
	// frame-sample geometry, and the encoded byte-size boundaries.
	constexpr std::array<Vector, 6> vectors {{
	    {0, 11025, 0, 1, 1, 0, 1, 0},
	    {8, 44100, 2, 2, 2048, 2, 4, 0},
	    {2, 16000, 3, 6, 127, 0, 1, 0},
	    {10, 64000, 4, 8, 513, 2, 4, 0},
	    {5, 32000, 0, 1, 64, 0, 1, 0},
	    {15, 192000, 2, 2, 257, 2, 4, 0},
	}};
	for (const auto& vector: vectors)
	{
		PrepareWaveform(memory, 0x40, vector.channels, vector.sample_rate,
		                Atrac9Config(vector.sample_rate_index, vector.channel_config_index,
		                             vector.frame_bytes, vector.superframe_index));
		if (!WaveformComplete(
		        memory, memory.Address(NGS2_INFO_OFFSET), DefaultWaveformOutputs(memory),
		        {{vector.frame_bytes * vector.frames_per_superframe,
		          vector.metadata_samples_per_unit * vector.frames_per_superframe,
		          vector.frames_per_superframe, vector.metadata_samples_per_unit}},
		        "valid ATRAC9 configuration did not complete through the registered wrapper")) return false;
	}
	PrepareWaveform(memory, 0x40, 2, 48000, 0xfe440ff0);
	if (!WaveformComplete(memory, memory.Address(NGS2_INFO_OFFSET), DefaultWaveformOutputs(memory),
	                      {{512, 512, 4, 128}},
	                      "24 kHz ATRAC9 with 48 kHz output rate was rejected or used output-domain frame geometry"))
		return false;
	return true;
}

bool Ngs2Atrac9OptionalOutputs(Mapping& memory)
{
	constexpr std::array<uint32_t, 4> observed {{256, 1024, 4, 256}};
	const auto all = DefaultWaveformOutputs(memory);
	auto check_mask = [&](unsigned mask) {
		PrepareWaveform(memory, 0x40, 1, 48000, 0xfe7007f0);
		WaveformOutputs selected {};
		for (size_t output = 0; output < selected.size(); ++output)
			if ((mask & (1U << output)) != 0) selected[output] = all[output];
		return WaveformComplete(memory, memory.Address(NGS2_INFO_OFFSET), selected, observed,
		                        "valid ATRAC9 optional-output mask was rejected");
	};

	// The real frontier supplied outputs 0, 1 and 3. Keep that exact 0xb shape
	// explicit, then cover each of the other fourteen non-empty masks once.
	if (!check_mask(0xb)) return false;
	for (unsigned mask = 1; mask < 16; ++mask)
		if (mask != 0xb && !check_mask(mask)) return false;
	return true;
}

bool Ngs2OptionalOutputs(Mapping& memory)
{
	const auto all = DefaultWaveformOutputs(memory);
	for (unsigned mask = 1; mask < 16; ++mask)
	{
		PrepareWaveform(memory, 0x14, 6);
		WaveformOutputs selected {};
		for (size_t output = 0; output < selected.size(); ++output)
			if ((mask & (1U << output)) != 0) selected[output] = all[output];
		if (!WaveformComplete(memory, memory.Address(NGS2_INFO_OFFSET), selected, {{18, 1, 1, 0}},
		                      "valid optional-output mask was rejected")) return false;
	}
	PrepareWaveform(memory);
	if (!WaveformRejected(memory, memory.Address(NGS2_INFO_OFFSET), {}, WaveformResult::NoOutputs,
	                      "all-null output tuple returned the wrong outcome")) return false;

	PrepareWaveform(memory, 0x12, 2);
	const WaveformOutputs adjacent {{memory.Address(NGS2_INFO_OFFSET - 4), memory.Address(NGS2_INFO_OFFSET + NGS2_INFO_BYTES),
	                                 memory.Address(NGS2_INFO_OFFSET + NGS2_INFO_BYTES + 4),
	                                 memory.Address(NGS2_INFO_OFFSET + NGS2_INFO_BYTES + 8)}};
	return WaveformComplete(memory, memory.Address(NGS2_INFO_OFFSET), adjacent, {{4, 1, 1, 0}},
	                        "exactly adjacent input/output ranges were rejected");
}

bool Ngs2InvalidFields(Mapping& memory)
{
	const auto outputs = DefaultWaveformOutputs(memory);
	for (uint32_t type: {0U, 0x0fU, 0x1cU, 0x3fU, 0x41U, MAXIMUM})
	{
		PrepareWaveform(memory, type);
		if (!WaveformRejected(memory, memory.Address(NGS2_INFO_OFFSET), outputs, WaveformResult::UnsupportedType,
		                      "unsupported waveform type returned the wrong outcome")) return false;
	}
	for (uint32_t channels: {0U, 3U, 4U, 5U, 7U, MAXIMUM})
	{
		PrepareWaveform(memory, 0x12, channels);
		if (!WaveformRejected(memory, memory.Address(NGS2_INFO_OFFSET), outputs, WaveformResult::InvalidChannels,
		                      "invalid channel count returned the wrong outcome")) return false;
	}
	PrepareWaveform(memory, 0x12, 2, 0);
	if (!WaveformRejected(memory, memory.Address(NGS2_INFO_OFFSET), outputs, WaveformResult::InvalidSampleRate,
	                      "zero sample rate returned the wrong outcome")) return false;
	for (uint32_t value: {1U, MAXIMUM})
	{
		PrepareWaveform(memory, 0x12, 2, 48000, value);
		if (!WaveformRejected(memory, memory.Address(NGS2_INFO_OFFSET), outputs, WaveformResult::UnsupportedConfigData,
		                      "nonzero configData returned the wrong outcome")) return false;
		PrepareWaveform(memory, 0x12, 2, 48000, 0, value);
		if (!WaveformRejected(memory, memory.Address(NGS2_INFO_OFFSET), outputs, WaveformResult::UnsupportedFrameOffset,
		                      "nonzero frameOffset returned the wrong outcome")) return false;
		PrepareWaveform(memory, 0x12, 2, 48000, 0, 0, value);
		if (!WaveformRejected(memory, memory.Address(NGS2_INFO_OFFSET), outputs, WaveformResult::UnsupportedFrameMargin,
		                      "nonzero frameMargin returned the wrong outcome")) return false;
	}
	return true;
}

bool Ngs2Atrac9InvalidFields(Mapping& memory)
{
	const auto outputs = DefaultWaveformOutputs(memory);
	auto reject_config = [&](uint32_t config, const char* message) {
		PrepareWaveform(memory, 0x40, 1, 11025, config);
		return WaveformRejected(memory, memory.Address(NGS2_INFO_OFFSET), outputs,
		                        WaveformResult::InvalidAtrac9Config, message);
	};
	for (uint32_t config: {
	         Atrac9Config(0, 0, 64, 0, 0, 0, 0xfd),
	         Atrac9Config(0, 0, 64, 0, 1),
	         Atrac9Config(0, 0, 64, 0, 0, 1),
	         Atrac9Config(0, 0, 64, 0, 0, 7),
	         Atrac9Config(0, 6, 64, 0),
	         Atrac9Config(0, 7, 64, 0),
	         Atrac9Config(0, 0, 64, 1),
	         Atrac9Config(0, 0, 64, 3),
	     })
	{
		if (!reject_config(config, "corrupt ATRAC9 magic, validation, reserved, or index was admitted")) return false;
	}

	const uint32_t mono_11025 = Atrac9Config(0, 0, 64, 0);
	PrepareWaveform(memory, 0x40, 2, 11025, mono_11025);
	if (!WaveformRejected(memory, memory.Address(NGS2_INFO_OFFSET), outputs,
	                      WaveformResult::Atrac9ChannelsMismatch,
	                      "ATRAC9 descriptor/config channel mismatch returned the wrong outcome")) return false;
	PrepareWaveform(memory, 0x40, 1, 12000, mono_11025);
	if (!WaveformRejected(memory, memory.Address(NGS2_INFO_OFFSET), outputs,
	                      WaveformResult::Atrac9SampleRateMismatch,
	                      "ATRAC9 descriptor/config sample-rate mismatch returned the wrong outcome")) return false;
	PrepareWaveform(memory, 0x40, 2, 44100, 0xfe440ff0);
	if (!WaveformRejected(memory, memory.Address(NGS2_INFO_OFFSET), outputs,
	                      WaveformResult::Atrac9SampleRateMismatch,
	                      "unqualified 24 kHz ATRAC9 output rate was admitted")) return false;
	PrepareWaveform(memory, 0x40, 2, 11025, Atrac9Config(0, 5, 64, 0));
	if (!WaveformRejected(memory, memory.Address(NGS2_INFO_OFFSET), outputs,
	                      WaveformResult::Atrac9ChannelsMismatch,
	                      "ATRAC9 decoder channel count outside the local profile was admitted")) return false;

	PrepareWaveform(memory, 0x40, 1, 11025, mono_11025, 1);
	if (!WaveformRejected(memory, memory.Address(NGS2_INFO_OFFSET), outputs,
	                      WaveformResult::UnsupportedFrameOffset,
	                      "nonzero ATRAC9 frameOffset returned the wrong outcome")) return false;
	PrepareWaveform(memory, 0x40, 1, 11025, mono_11025, 0, 1);
	if (!WaveformRejected(memory, memory.Address(NGS2_INFO_OFFSET), outputs,
	                      WaveformResult::UnsupportedFrameMargin,
	                      "nonzero ATRAC9 frameMargin returned the wrong outcome")) return false;

	for (size_t invalid_index = 0; invalid_index < outputs.size(); ++invalid_index)
	{
		for (uint64_t invalid: {uint64_t(MAX_TOKEN), uint64_t(MAX_TOKEN - 2)})
		{
			PrepareWaveform(memory, 0x40, 1, 48000, 0xfe7007f0);
			auto invalid_outputs = outputs;
			invalid_outputs[invalid_index] = invalid;
			if (!WaveformRejected(memory, memory.Address(NGS2_INFO_OFFSET), invalid_outputs,
			                      WaveformResult::InvalidOutput,
			                      "invalid ATRAC9 output did not fail before every write")) return false;
		}
	}
	return true;
}

bool Ngs2Aliases(Mapping& memory)
{
	for (size_t offset: {size_t(0), size_t(1), size_t(20), size_t(23)})
	{
		PrepareWaveform(memory);
		WaveformOutputs outputs {};
		outputs[0] = memory.Address(NGS2_INFO_OFFSET + offset);
		if (!WaveformRejected(memory, memory.Address(NGS2_INFO_OFFSET), outputs, WaveformResult::AliasedMemory,
		                      "input/output overlap was admitted")) return false;
	}
	for (size_t first = 0; first < 4; ++first)
	{
		for (size_t second = first + 1; second < 4; ++second)
		{
			for (size_t delta: {size_t(0), size_t(1), size_t(3)})
			{
				PrepareWaveform(memory);
				WaveformOutputs outputs {};
				outputs[first] = memory.Address(NGS2_OUTPUT_BASE);
				outputs[second] = memory.Address(NGS2_OUTPUT_BASE + delta);
				if (!WaveformRejected(memory, memory.Address(NGS2_INFO_OFFSET), outputs, WaveformResult::AliasedMemory,
				                      "output/output overlap was admitted")) return false;
			}
		}
	}
	PrepareWaveform(memory, 0x1a, 2);
	const WaveformOutputs adjacent {{memory.Address(NGS2_OUTPUT_BASE), memory.Address(NGS2_OUTPUT_BASE + 4),
	                                 memory.Address(NGS2_OUTPUT_BASE + 8), memory.Address(NGS2_OUTPUT_BASE + 12)}};
	return WaveformComplete(memory, memory.Address(NGS2_INFO_OFFSET), adjacent, {{16, 1, 1, 0}},
	                        "exactly adjacent outputs were rejected");
}

bool Ngs2Memory(Mapping& memory, size_t page_size)
{
	PrepareWaveform(memory);
	const auto outputs = DefaultWaveformOutputs(memory);
	Mapping freed(page_size);
	if (!Check(freed.Valid(), "NGS2 unmapped fixture allocation failed")) return false;
	const uint64_t unmapped = freed.Address();
	if (!Check(freed.Free(), "NGS2 unmapped fixture release failed")) return false;
	for (uint64_t invalid: {uint64_t(0), uint64_t(1), unmapped, uint64_t(MAX_TOKEN), uint64_t(MAX_TOKEN - 22)})
	{
		PrepareWaveform(memory);
		if (!WaveformRejected(memory, invalid, outputs, WaveformResult::InvalidInfo,
		                      "invalid or overflowing waveform input was admitted")) return false;
	}
	for (size_t selected = 0; selected < 4; ++selected)
	{
		for (uint64_t invalid: {uint64_t(1), unmapped, uint64_t(MAX_TOKEN), uint64_t(MAX_TOKEN - 2)})
		{
			PrepareWaveform(memory);
			auto invalid_outputs = outputs;
			invalid_outputs[selected] = invalid;
			if (!WaveformRejected(memory, memory.Address(NGS2_INFO_OFFSET), invalid_outputs,
			                      WaveformResult::InvalidOutput,
			                      "invalid output did not fail before every write")) return false;
		}
	}

	Mapping protected_memory(page_size);
	if (!Check(protected_memory.Valid(), "NGS2 protected fixture allocation failed")) return false;
	PrepareWaveform(protected_memory);
	const auto protected_before = Snapshot(protected_memory);
	for (auto mode: {Vm::Mode::Read, Vm::Mode::NoAccess})
	{
		PrepareWaveform(memory);
		const auto main_before = Snapshot(memory);
		if (!Check(protected_memory.Protect(0, page_size, mode), "NGS2 protected fixture setup failed")) return false;
		const auto result = Ngs2::Ngs2GetWaveformFrameInfoModel(
		    memory.Address(NGS2_INFO_OFFSET), protected_memory.Address(NGS2_OUTPUT_BASE), 0, 0, 0);
		if (!Check(protected_memory.Protect(0, page_size, Vm::Mode::ReadWrite), "NGS2 protected fixture restore failed") ||
		    !Check(result == WaveformResult::InvalidOutput, "protected output returned the wrong outcome") ||
		    !Check(Matches(memory, main_before) && Matches(protected_memory, protected_before),
		           "protected output rejection changed caller storage")) return false;
	}
	if (!Check(protected_memory.Protect(0, page_size, Vm::Mode::NoAccess), "NGS2 inaccessible input setup failed")) return false;
	PrepareWaveform(memory);
	const auto inaccessible_before = Snapshot(memory);
	const auto inaccessible = Ngs2::Ngs2GetWaveformFrameInfoModel(protected_memory.Address(NGS2_INFO_OFFSET),
	                                                              outputs[0], outputs[1], outputs[2], outputs[3]);
	if (!Check(protected_memory.Protect(0, page_size, Vm::Mode::ReadWrite), "NGS2 inaccessible input restore failed") ||
	    !Check(inaccessible == WaveformResult::InvalidInfo && Matches(memory, inaccessible_before) &&
	               Matches(protected_memory, protected_before),
	           "inaccessible input was admitted or changed outputs")) return false;

	Mapping boundary(page_size * 2);
	if (!Check(boundary.Valid(), "NGS2 boundary fixture allocation failed")) return false;
	PrepareWaveform(boundary, 0x13, 2, 48000, 0, 0, 0, page_size - NGS2_INFO_BYTES);
	const auto boundary_before = Snapshot(boundary);
	PrepareWaveform(memory);
	auto main_expected = Snapshot(memory);
	Store32(main_expected.data() + NGS2_OUTPUT_BASE, 4);
	if (!Check(boundary.Protect(page_size, page_size, Vm::Mode::NoAccess), "NGS2 info boundary setup failed") ||
	    !Check(g_get_waveform_frame_info(
	               reinterpret_cast<const Ngs2::Ngs2WaveformInfo*>(boundary.Address(page_size - NGS2_INFO_BYTES)),
	               reinterpret_cast<uint32_t*>(outputs[0]), nullptr, nullptr, nullptr) == 0,
	           "exact 24-byte input tail was rejected") ||
	    !Check(Matches(memory, main_expected), "exact input-tail query changed an adjacent byte") ||
	    !Check(Ngs2::Ngs2GetWaveformFrameInfoModel(boundary.Address(page_size - NGS2_INFO_BYTES + 1),
	                                               outputs[0], 0, 0, 0) == WaveformResult::InvalidInfo,
	           "truncated input tail was admitted") ||
	    !Check(boundary.Protect(page_size, page_size, Vm::Mode::ReadWrite), "NGS2 info boundary restore failed") ||
	    !Check(Matches(boundary, boundary_before), "NGS2 input boundary changed caller storage")) return false;

	PrepareWaveform(memory, 0x12, 2);
	PrepareWaveform(boundary);
	auto exact_output = Snapshot(boundary);
	Store32(exact_output.data() + page_size - sizeof(uint32_t), 4);
	if (!Check(boundary.Protect(page_size, page_size, Vm::Mode::NoAccess), "NGS2 output boundary setup failed") ||
	    !Check(Ngs2::Ngs2GetWaveformFrameInfoModel(memory.Address(NGS2_INFO_OFFSET),
	                                               boundary.Address(page_size - sizeof(uint32_t)), 0, 0, 0) == WaveformResult::Ok,
	           "exact four-byte output tail was rejected") ||
	    !Check(Ngs2::Ngs2GetWaveformFrameInfoModel(memory.Address(NGS2_INFO_OFFSET),
	                                               boundary.Address(page_size - sizeof(uint32_t) + 1), 0, 0, 0) ==
	               WaveformResult::InvalidOutput,
	           "truncated output tail was admitted") ||
	    !Check(boundary.Protect(page_size, page_size, Vm::Mode::ReadWrite), "NGS2 output boundary restore failed") ||
	    !Check(Matches(boundary, exact_output), "truncated output changed its writable prefix or an adjacent byte")) return false;
	return true;
}

bool Ngs2Disabled()
{
	return Selectors(false, false, false) && Registration(false, false) &&
	       Check(Ngs2::Ngs2GetWaveformFrameInfoModel(MAX_TOKEN, MAX_TOKEN, MAX_TOKEN, MAX_TOKEN, MAX_TOKEN) ==
	                 WaveformResult::Disabled,
	             "disabled NGS2 waveform model accessed opaque addresses or returned the wrong outcome");
}

bool Ngs2Enabled()
{
	const long host_page_size = sysconf(_SC_PAGESIZE);
	if (!Check(host_page_size > 0, "NGS2 host page size unavailable")) return false;
	Mapping memory(static_cast<size_t>(host_page_size));
	return Check(memory.Valid(), "NGS2 waveform fixture allocation failed") && Selectors(false, false, true) &&
	       Registration(false, true) && Ngs2NativeWaveformMetadata(memory) && Ngs2PcmMatrix(memory) && Ngs2Atrac9Matrix(memory) &&
	       Ngs2OptionalOutputs(memory) && Ngs2Atrac9OptionalOutputs(memory) &&
	       Ngs2InvalidFields(memory) && Ngs2Atrac9InvalidFields(memory) && Ngs2Aliases(memory) &&
	       Ngs2Memory(memory, static_cast<size_t>(host_page_size));
}

bool Ngs2Latch(bool expected)
{
	if (!Check(Ngs2::Ngs2WaveformFrameInfoExperimentalEnabled() == expected,
	           "initial NGS2 selector disagreed with the expected value") ||
	    !Check(setenv(NGS2_SELECTOR, expected ? "0" : "1", 1) == 0, "NGS2 selector mutation failed") ||
	    !Selectors(false, false, expected) || !Registration(false, expected)) return false;
	if (!expected)
		return Check(Ngs2::Ngs2GetWaveformFrameInfoModel(MAX_TOKEN, MAX_TOKEN, MAX_TOKEN, MAX_TOKEN, MAX_TOKEN) ==
		                 WaveformResult::Disabled,
		             "environment mutation enabled a latched-off NGS2 model");
	const long page_size = sysconf(_SC_PAGESIZE);
	if (!Check(page_size > 0, "NGS2 latch page size unavailable")) return false;
	Mapping memory(static_cast<size_t>(page_size));
	if (!Check(memory.Valid(), "NGS2 latch fixture allocation failed")) return false;
	PrepareWaveform(memory);
	const auto outputs = DefaultWaveformOutputs(memory);
	return WaveformComplete(memory, memory.Address(NGS2_INFO_OFFSET), {{outputs[0], 0, 0, 0}}, {{4, 1, 1, 0}},
	                        "environment mutation disabled a latched-on NGS2 model") &&
	       Check(Ngs2::Ngs2WaveformFrameInfoExperimentalEnabled(), "NGS2 process latch changed after a successful call");
}

constexpr size_t NGS2_STATE_FLAGS_OUTPUT = 1701; // Deliberately unaligned.

void PrepareStateFlagsMemory(Mapping& memory)
{
	for (size_t byte = 0; byte < memory.Size(); ++byte)
		memory.Data()[byte] = static_cast<uint8_t>(0xa7 + byte * 43);
}

bool StateFlagsRejected(Mapping& memory, uintptr_t voice_handle, uint64_t output,
                        StateFlagsResult expected, const char* message)
{
	const auto before = Snapshot(memory);
	const auto result = Ngs2::Ngs2VoiceGetStateFlagsModel(voice_handle, output);
	return Check(result == expected, message) &&
	       Check(Matches(memory, before), "rejected NGS2 state-flags query changed output or a canary");
}

bool Ngs2StateFlagsInvalidInputs(Mapping& memory, size_t page_size)
{
	PrepareStateFlagsMemory(memory);
	Mapping freed(page_size);
	if (!Check(freed.Valid(), "NGS2 state-flags unmapped fixture allocation failed")) return false;
	const uint64_t unmapped = freed.Address();
	if (!Check(freed.Free(), "NGS2 state-flags unmapped fixture release failed")) return false;
	for (uint64_t invalid: {uint64_t(0), uint64_t(1), unmapped, uint64_t(MAX_TOKEN),
	                        uint64_t(MAX_TOKEN - 2)})
	{
		PrepareStateFlagsMemory(memory);
		if (!StateFlagsRejected(memory, MAX_TOKEN, invalid, StateFlagsResult::InvalidOutput,
		                        "invalid or overflowing NGS2 state-flags output was admitted")) return false;
	}

	Mapping protected_memory(page_size);
	if (!Check(protected_memory.Valid(), "NGS2 state-flags protected fixture allocation failed")) return false;
	PrepareStateFlagsMemory(protected_memory);
	const auto protected_before = Snapshot(protected_memory);
	for (auto mode: {Vm::Mode::Read, Vm::Mode::NoAccess})
	{
		if (!Check(protected_memory.Protect(0, page_size, mode),
		           "NGS2 state-flags protected fixture setup failed")) return false;
		const auto result = Ngs2::Ngs2VoiceGetStateFlagsModel(
		    MAX_TOKEN, protected_memory.Address(NGS2_STATE_FLAGS_OUTPUT));
		if (!Check(protected_memory.Protect(0, page_size, Vm::Mode::ReadWrite),
		           "NGS2 state-flags protected fixture restore failed") ||
		    !Check(result == StateFlagsResult::InvalidOutput,
		           "protected NGS2 state-flags output returned the wrong outcome") ||
		    !Check(Matches(protected_memory, protected_before),
		           "protected NGS2 state-flags rejection changed caller storage")) return false;
	}

	Mapping boundary(page_size * 2);
	if (!Check(boundary.Valid(), "NGS2 state-flags boundary fixture allocation failed")) return false;
	PrepareStateFlagsMemory(boundary);
	const auto boundary_before = Snapshot(boundary);
	if (!Check(boundary.Protect(page_size, page_size, Vm::Mode::NoAccess),
	           "NGS2 state-flags output boundary setup failed")) return false;
	const auto exact_tail = Ngs2::Ngs2VoiceGetStateFlagsModel(
	    MAX_TOKEN, boundary.Address(page_size - sizeof(uint32_t)));
	const auto crossed_tail = Ngs2::Ngs2VoiceGetStateFlagsModel(
	    MAX_TOKEN, boundary.Address(page_size - sizeof(uint32_t) + 1));
	if (!Check(boundary.Protect(page_size, page_size, Vm::Mode::ReadWrite),
	           "NGS2 state-flags output boundary restore failed") ||
	    !Check(exact_tail == StateFlagsResult::InvalidHandle,
	           "exact four-byte NGS2 state-flags output tail was not fully preflighted") ||
	    !Check(crossed_tail == StateFlagsResult::InvalidOutput,
	           "cross-page NGS2 state-flags output was admitted") ||
	    !Check(Matches(boundary, boundary_before),
	           "NGS2 state-flags boundary rejection changed a writable prefix or canary")) return false;

	for (uintptr_t invalid_handle: {uintptr_t(0), uintptr_t(1), MAX_TOKEN,
	                                static_cast<uintptr_t>(memory.Address(256)),
	                                static_cast<uintptr_t>(memory.Address(257))})
	{
		PrepareStateFlagsMemory(memory);
		if (!StateFlagsRejected(memory, invalid_handle, memory.Address(NGS2_STATE_FLAGS_OUTPUT),
		                        StateFlagsResult::InvalidHandle,
		                        "null, foreign, or misaligned NGS2 voice handle was admitted")) return false;
	}
	return true;
}

bool Ngs2StateFlagsDisabled()
{
	return Selectors(false, false, false, false) && Registration(false, false, false) &&
	       Check(Ngs2::Ngs2VoiceGetStateFlagsModel(MAX_TOKEN, MAX_TOKEN) == StateFlagsResult::Disabled,
	             "disabled NGS2 state-flags model accessed opaque addresses or returned the wrong outcome");
}

bool Ngs2StateFlagsEnabled()
{
	const long host_page_size = sysconf(_SC_PAGESIZE);
	if (!Check(host_page_size > 0, "NGS2 state-flags host page size unavailable")) return false;
	Mapping memory(static_cast<size_t>(host_page_size));
	return Check(memory.Valid(), "NGS2 state-flags fixture allocation failed") &&
	       Selectors(false, false, false, true) && Registration(false, false, true) &&
	       Ngs2StateFlagsInvalidInputs(memory, static_cast<size_t>(host_page_size));
}

bool Ngs2StateFlagsLatch(bool expected)
{
	if (!Check(Ngs2::Ngs2VoiceStateFlagsExperimentalEnabled() == expected,
	           "initial NGS2 state-flags selector disagreed with the expected value") ||
	    !Check(setenv(NGS2_STATE_FLAGS_SELECTOR, expected ? "0" : "1", 1) == 0,
	           "NGS2 state-flags selector mutation failed") ||
	    !Selectors(false, false, false, expected) || !Registration(false, false, expected)) return false;
	if (!expected)
		return Check(Ngs2::Ngs2VoiceGetStateFlagsModel(MAX_TOKEN, MAX_TOKEN) == StateFlagsResult::Disabled,
		             "environment mutation enabled a latched-off NGS2 state-flags model");
	const long page_size = sysconf(_SC_PAGESIZE);
	if (!Check(page_size > 0, "NGS2 state-flags latch page size unavailable")) return false;
	Mapping memory(static_cast<size_t>(page_size));
	if (!Check(memory.Valid(), "NGS2 state-flags latch fixture allocation failed")) return false;
	PrepareStateFlagsMemory(memory);
	return StateFlagsRejected(memory, 0, memory.Address(NGS2_STATE_FLAGS_OUTPUT),
	                          StateFlagsResult::InvalidHandle,
	                          "environment mutation disabled a latched-on NGS2 state-flags model") &&
	       Check(Ngs2::Ngs2VoiceStateFlagsExperimentalEnabled(),
	             "NGS2 state-flags process latch changed after a model call");
}

bool Ngs2StateFlagsIndependence(bool waveform_enabled, bool state_flags_enabled)
{
	if (!Selectors(false, false, waveform_enabled, state_flags_enabled) ||
	    !Registration(false, waveform_enabled, state_flags_enabled)) return false;
	const auto state_result = Ngs2::Ngs2VoiceGetStateFlagsModel(MAX_TOKEN, MAX_TOKEN);
	if (state_flags_enabled)
		return Check(state_result == StateFlagsResult::InvalidOutput,
		             "Waveform-off implicitly disabled the selected NGS2 state-flags model");
	return Check(state_result == StateFlagsResult::Disabled,
	             "Waveform-on implicitly enabled the unselected NGS2 state-flags model");
}

bool Ngs2AudioRenderDisabled()
{
	return Selectors(false, false, false, false, false) && Registration(false, false, false);
}

bool Ngs2AudioRenderEnabled()
{
	return Selectors(false, false, false, false, true) && Registration(false, false, false);
}

bool Ngs2AudioRenderLatch(bool expected)
{
	return Check(Ngs2::Ngs2AudioRenderExperimentalEnabled() == expected,
	             "initial NGS2 audio-render selector disagreed with the expected value") &&
	       Check(setenv(NGS2_AUDIO_RENDER_SELECTOR, expected ? "0" : "1", 1) == 0,
	             "NGS2 audio-render selector mutation failed") &&
	       Selectors(false, false, false, false, expected) && Registration(false, false, false) &&
	       Check(Ngs2::Ngs2AudioRenderExperimentalEnabled() == expected,
	             "NGS2 audio-render process latch changed after environment mutation");
}

bool Ngs2AudioRenderIndependence(bool waveform_enabled, bool state_flags_enabled, bool audio_render_enabled)
{
	return Selectors(false, false, waveform_enabled, state_flags_enabled, audio_render_enabled) &&
	       Registration(false, waveform_enabled, state_flags_enabled);
}

int Ngs2StateFlagsFatalChild(const char* scenario)
{
	const long page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) return 3;
	Mapping memory(static_cast<size_t>(page_size) * 2);
	if (!memory.Valid()) return 3;
	PrepareStateFlagsMemory(memory);
	uintptr_t voice_handle = MAX_TOKEN;
	uint64_t output = memory.Address(NGS2_STATE_FLAGS_OUTPUT);
	if (std::strcmp(scenario, "ngs2-state-disabled") == 0)
		output = MAX_TOKEN;
	else if (std::strcmp(scenario, "ngs2-state-handle-null") == 0)
		voice_handle = 0;
	else if (std::strcmp(scenario, "ngs2-state-handle-foreign") == 0)
		voice_handle = memory.Address(256);
	else if (std::strcmp(scenario, "ngs2-state-output-null") == 0)
		output = 0;
	else if (std::strcmp(scenario, "ngs2-state-output-read-only") == 0)
	{
		if (!memory.Protect(0, static_cast<size_t>(page_size), Vm::Mode::Read)) return 3;
	} else if (std::strcmp(scenario, "ngs2-state-output-cross-page") == 0)
	{
		output = memory.Address(static_cast<size_t>(page_size) - sizeof(uint32_t) + 1);
		if (!memory.Protect(static_cast<size_t>(page_size), static_cast<size_t>(page_size),
		                    Vm::Mode::NoAccess)) return 3;
	} else return 3;
	(void)g_get_voice_state_flags(voice_handle, reinterpret_cast<uint32_t*>(output));
	return 4;
}

int Ngs2FatalChild(const char* scenario)
{
	const long page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) return 3;
	Mapping memory(static_cast<size_t>(page_size));
	if (!memory.Valid()) return 3;
	PrepareWaveform(memory);
	auto outputs = DefaultWaveformOutputs(memory);
	const Ngs2::Ngs2WaveformInfo* info = reinterpret_cast<const Ngs2::Ngs2WaveformInfo*>(memory.Address(NGS2_INFO_OFFSET));
	if (std::strcmp(scenario, "ngs2-disabled") == 0)
	{
		info = reinterpret_cast<const Ngs2::Ngs2WaveformInfo*>(MAX_TOKEN);
		outputs.fill(MAX_TOKEN);
	} else if (std::strcmp(scenario, "ngs2-info") == 0)
		info = nullptr;
	else if (std::strcmp(scenario, "ngs2-no-outputs") == 0)
		outputs.fill(0);
	else if (std::strcmp(scenario, "ngs2-type") == 0)
		PrepareWaveform(memory, 0x1c);
	else if (std::strcmp(scenario, "ngs2-channels") == 0)
		PrepareWaveform(memory, 0x12, 3);
	else if (std::strcmp(scenario, "ngs2-rate") == 0)
		PrepareWaveform(memory, 0x12, 2, 0);
	else if (std::strcmp(scenario, "ngs2-config") == 0)
		PrepareWaveform(memory, 0x12, 2, 48000, 1);
	else if (std::strcmp(scenario, "ngs2-atrac9-config") == 0)
		PrepareWaveform(memory, 0x40, 1, 11025, Atrac9Config(0, 0, 64, 0, 0, 0, 0xfd));
	else if (std::strcmp(scenario, "ngs2-offset") == 0)
		PrepareWaveform(memory, 0x12, 2, 48000, 0, 1);
	else if (std::strcmp(scenario, "ngs2-margin") == 0)
		PrepareWaveform(memory, 0x12, 2, 48000, 0, 0, 1);
	else if (std::strcmp(scenario, "ngs2-output") == 0)
	{
		outputs.fill(0);
		outputs[0] = MAX_TOKEN;
	} else if (std::strcmp(scenario, "ngs2-alias-input") == 0)
	{
		outputs.fill(0);
		outputs[0] = memory.Address(NGS2_INFO_OFFSET + 1);
	} else if (std::strcmp(scenario, "ngs2-alias-output") == 0)
	{
		outputs.fill(0);
		outputs[0] = memory.Address(NGS2_OUTPUT_BASE);
		outputs[1] = memory.Address(NGS2_OUTPUT_BASE + 1);
	} else return 3;
	(void)g_get_waveform_frame_info(info, reinterpret_cast<uint32_t*>(outputs[0]),
	                                reinterpret_cast<uint32_t*>(outputs[1]),
	                                reinterpret_cast<uint32_t*>(outputs[2]),
	                                reinterpret_cast<uint32_t*>(outputs[3]));
	return 4;
}

int FatalChild(const char* scenario)
{
	if (std::strncmp(scenario, "ngs2-state-", 11) == 0) return Ngs2StateFlagsFatalChild(scenario);
	if (std::strncmp(scenario, "ngs2-", 5) == 0) return Ngs2FatalChild(scenario);
	if (std::strcmp(scenario, "disabled") == 0) (void)Voice::Init(nullptr, 0);
	else if (std::strcmp(scenario, "duplicate") == 0)
	{
		if (Voice::Init(nullptr, 0) != 0) return 3;
		(void)Voice::Init(reinterpret_cast<const void*>(MAX_TOKEN), MAXIMUM);
	} else if (std::strcmp(scenario, "create-disabled") == 0 || std::strcmp(scenario, "create-uninitialized") == 0)
		(void)Voice::CreatePort(reinterpret_cast<uint32_t*>(MAX_TOKEN), reinterpret_cast<const void*>(MAX_TOKEN));
	else if (std::strcmp(scenario, "delete-disabled") == 0 || std::strcmp(scenario, "delete-uninitialized") == 0)
		(void)Voice::DeletePort(MAXIMUM);
	else if (std::strcmp(scenario, "info-disabled") == 0 || std::strcmp(scenario, "info-uninitialized") == 0)
		(void)Voice::GetPortInfo(MAXIMUM, reinterpret_cast<void*>(MAX_TOKEN));
	else if (std::strcmp(scenario, "start-uninitialized") == 0) (void)Voice::Start(reinterpret_cast<const void*>(MAX_TOKEN));
	else if (std::strcmp(scenario, "stop-uninitialized") == 0) (void)Voice::Stop();
	else if (std::strcmp(scenario, "end-uninitialized") == 0) (void)Voice::End();
	else
	{
		if (Voice::Init(nullptr, 0) != 0) return 3;
		if (std::strcmp(scenario, "create-descriptor") == 0) (void)Voice::CreatePort(nullptr, nullptr);
		else if (std::strcmp(scenario, "delete-invalid") == 0) (void)Voice::DeletePort(0);
		else if (std::strcmp(scenario, "info-invalid") == 0) (void)Voice::GetPortInfo(0, reinterpret_cast<void*>(MAX_TOKEN));
		else if (std::strcmp(scenario, "start-duplicate") == 0)
		{
			if (Voice::Start(nullptr) != 0) return 3;
			(void)Voice::Start(nullptr);
		} else if (std::strcmp(scenario, "stop-duplicate") == 0) (void)Voice::Stop();
		else if (std::strcmp(scenario, "end-duplicate") == 0)
		{
			if (Voice::End() != 0) return 3;
			(void)Voice::End();
		} else
		{
			const long page_size = sysconf(_SC_PAGESIZE);
			if (page_size <= 0) return 3;
			Mapping memory(static_cast<size_t>(page_size));
			if (!memory.Valid()) return 3;
			Fill(memory, std::strcmp(scenario, "create-type") == 0 ? 6 : 0);
			if (std::strcmp(scenario, "create-output") == 0) (void)Voice::CreatePort(nullptr, memory.Data(DESCRIPTOR));
			else if (std::strcmp(scenario, "info-unwritable") == 0)
			{
				uint32_t id = 0;
				if (!Create(memory, &id) || !memory.Protect(0, memory.Size(), Vm::Mode::Read)) return 3;
				(void)Voice::GetPortInfo(id, memory.Data(OUTPUT));
			} else if (std::strcmp(scenario, "create-type") == 0)
				(void)Voice::CreatePort(reinterpret_cast<uint32_t*>(memory.Address(OUTPUT)), memory.Data(DESCRIPTOR));
			else if (std::strcmp(scenario, "create-capacity") == 0)
			{
				for (size_t i = 0; i < 64; ++i)
					if (Voice::CreatePortModel(memory.Address(OUTPUT), memory.Address(DESCRIPTOR)) != Result::Ok) return 3;
				(void)Voice::CreatePort(reinterpret_cast<uint32_t*>(memory.Address(OUTPUT)), memory.Data(DESCRIPTOR));
			} else return 3;
		}
	}
	return 4;
}

bool ExpectFatal(const char* executable, const char* scenario, const char* outcome,
                 const char* operation = "initialize", bool ngs2 = false,
                 bool ngs2_state_flags = false)
{
	int descriptors[2] {};
	if (!Check(pipe(descriptors) == 0, "fatal-child pipe failed")) return false;
	posix_spawn_file_actions_t actions;
	int error = posix_spawn_file_actions_init(&actions);
	if (error != 0) { close(descriptors[0]); close(descriptors[1]); return Check(false, "fatal-child actions failed"); }
	error = posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
	if (error == 0) error = posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO);
	if (error == 0) error = posix_spawn_file_actions_addclose(&actions, descriptors[0]);
	if (error == 0) error = posix_spawn_file_actions_addclose(&actions, descriptors[1]);
	pid_t child = 0;
	char* arguments[] {const_cast<char*>(executable), const_cast<char*>("fatal-child"), const_cast<char*>(scenario), nullptr};
	if (error == 0) error = posix_spawnp(&child, executable, &actions, nullptr, arguments, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(descriptors[1]);
	if (error != 0) { close(descriptors[0]); return Check(false, "fatal-child spawn failed"); }
	std::string captured;
	std::array<char, 1024> buffer {};
	bool read_ok = true;
	for (;;)
	{
		const auto count = read(descriptors[0], buffer.data(), buffer.size());
		if (count > 0) captured.append(buffer.data(), static_cast<size_t>(count));
		else if (count == 0) break;
		else if (errno != EINTR) { read_ok = false; break; }
	}
	close(descriptors[0]);
	int status = 0;
	pid_t waited;
	do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
	const std::string marker = ngs2_state_flags
	                               ? std::string("[NGS2_VOICE_STATE_FLAGS_LOCAL_EXPERIMENT] result=") + outcome
	                           : ngs2
	                               ? std::string("[NGS2_WAVEFORM_FRAME_INFO_LOCAL_EXPERIMENT] result=") + outcome
	                               : std::string("[VOICE_OFFLINE_EXPERIMENT] operation=") + operation + " result=" + outcome;
	const std::string diagnostic = ngs2_state_flags
	                                   ? std::string("NGS2 VoiceGetStateFlags local experiment cannot model request: ") + outcome
	                               : ngs2
	                                   ? std::string("NGS2 WaveformFrameInfo local experiment cannot model request: ") + outcome
	                                   : std::string("Voice offline experiment cannot model ") +
	                                         (std::strcmp(operation, "initialize") == 0 ? "initialization" : operation) + ": " + outcome;
	const char* profile = ngs2_state_flags ? "profile=pending-play-visible-state" :
	                      ngs2 ? "profile=pcm-interleaved+atrac9-superframe codecs=pcm,atrac9" :
	                             "audio=0 networking=0";
	const bool passed = read_ok && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 65 &&
	                    captured.find(marker) != std::string::npos && captured.find(diagnostic) != std::string::npos &&
	                    captured.find("console_contract=unverified") != std::string::npos &&
	                    captured.find(profile) != std::string::npos;
	if (!passed) std::fprintf(stderr, "VoiceTest: fatal scenario %s status=%d output=%s\n", scenario, status, captured.c_str());
	return Check(passed, "unsupported wrapper did not exit explicitly with its specific diagnostic");
}

bool FatalPaths(const char* executable)
{
	struct Failure { const char* scenario; const char* operation; const char* outcome; };
	const std::array<Failure, 20> failures {{
	    {"disabled", "initialize", "disabled"}, {"duplicate", "initialize", "already-initialized"},
	    {"create-disabled", "create-port", "disabled"}, {"delete-disabled", "delete-port", "disabled"},
	    {"create-uninitialized", "create-port", "not-initialized"}, {"delete-uninitialized", "delete-port", "not-initialized"},
	    {"create-descriptor", "create-port", "descriptor-unreadable"}, {"create-type", "create-port", "descriptor-type-unsupported"},
	    {"create-output", "create-port", "identifier-unwritable"}, {"create-capacity", "create-port", "port-capacity-exceeded"},
	    {"delete-invalid", "delete-port", "port-not-live"}, {"start-uninitialized", "start", "not-initialized"},
	    {"stop-uninitialized", "stop", "not-initialized"}, {"end-uninitialized", "end", "not-initialized"},
	    {"start-duplicate", "start", "already-started"}, {"stop-duplicate", "stop", "already-stopped"},
	    {"info-disabled", "get-port-info", "disabled"}, {"info-uninitialized", "get-port-info", "not-initialized"},
	    {"info-invalid", "get-port-info", "port-not-live"}, {"info-unwritable", "get-port-info", "info-unwritable"},
	}};
	for (const auto& failure: failures)
		if (!ExpectFatal(executable, failure.scenario, failure.outcome, failure.operation)) return false;
	return ExpectFatal(executable, "end-duplicate", "not-initialized", "end");
}

bool Ngs2FatalPaths(const char* executable)
{
	struct Failure { const char* scenario; const char* outcome; };
	const std::array<Failure, 13> failures {{
	    {"ngs2-disabled", "disabled"}, {"ngs2-info", "info-unreadable"},
	    {"ngs2-no-outputs", "outputs-absent"}, {"ngs2-type", "waveform-type-unsupported"},
	    {"ngs2-channels", "channels-invalid"}, {"ngs2-rate", "sample-rate-invalid"},
	    {"ngs2-config", "config-data-unsupported"}, {"ngs2-offset", "frame-offset-unsupported"},
	    {"ngs2-atrac9-config", "atrac9-config-invalid"},
	    {"ngs2-margin", "frame-margin-unsupported"}, {"ngs2-output", "output-unwritable"},
	    {"ngs2-alias-input", "memory-alias-unsupported"},
	    {"ngs2-alias-output", "memory-alias-unsupported"},
	}};
	for (const auto& failure: failures)
		if (!ExpectFatal(executable, failure.scenario, failure.outcome, "get-waveform-frame-info", true)) return false;
	return true;
}

bool Ngs2StateFlagsFatalPaths(const char* executable)
{
	struct Failure { const char* scenario; const char* outcome; };
	const std::array<Failure, 6> failures {{
	    {"ngs2-state-disabled", "disabled"},
	    {"ngs2-state-handle-null", "voice-handle-invalid"},
	    {"ngs2-state-handle-foreign", "voice-handle-invalid"},
	    {"ngs2-state-output-null", "state-flags-unwritable"},
	    {"ngs2-state-output-read-only", "state-flags-unwritable"},
	    {"ngs2-state-output-cross-page", "state-flags-unwritable"},
	}};
	for (const auto& failure: failures)
		if (!ExpectFatal(executable, failure.scenario, failure.outcome,
		                 "get-state-flags", false, true)) return false;
	return true;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc == 3 && std::strcmp(argv[1], "fatal-child") == 0)
	{
		const bool ngs2_state_flags_fatal = std::strncmp(argv[2], "ngs2-state-", 11) == 0;
		const bool ngs2_fatal = !ngs2_state_flags_fatal && std::strncmp(argv[2], "ngs2-", 5) == 0;
		if (setenv(SELECTOR, ngs2_fatal || ngs2_state_flags_fatal ? "0" :
		                     (std::strstr(argv[2], "disabled") != nullptr ? "0" : "1"), 1) != 0 ||
		    setenv(NGS2_SELECTOR,
		           ngs2_fatal && std::strcmp(argv[2], "ngs2-disabled") != 0 ? "1" : "0", 1) != 0 ||
		    setenv(NGS2_STATE_FLAGS_SELECTOR,
		           ngs2_state_flags_fatal && std::strcmp(argv[2], "ngs2-state-disabled") != 0 ? "1" : "0", 1) != 0 ||
		    setenv(NGS2_AUDIO_RENDER_SELECTOR, "0", 1) != 0) return 3;
		for (const char* selector: {"LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE", "LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE",
		                           "LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE", "LUMEN_EXPERIMENTAL_RUDP_OFFLINE",
		                           "LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE"})
			if (setenv(selector, "0", 1) != 0) return 3;
	}
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Network::NetworkSubsystem::Instance()->Init(nullptr);
	bool passed = false;
	if (argc == 2 && std::strcmp(argv[1], "disabled") == 0) passed = Disabled();
	else if (argc == 2 && std::strcmp(argv[1], "enabled") == 0)
		passed = Selectors(true, false) && Registration(true) && OpaqueInputsAndReset() && ConcurrentInitialization() &&
		         EnabledPorts() && LegacyRequests();
	else if (argc == 3 && std::strcmp(argv[1], "latch") == 0 && (std::strcmp(argv[2], "0") == 0 || std::strcmp(argv[2], "1") == 0))
		passed = Latch(std::strcmp(argv[2], "1") == 0);
	else if (argc == 2 && std::strcmp(argv[1], "coexist") == 0) passed = Coexist() && PortCoexist();
	else if (argc == 2 && std::strcmp(argv[1], "fatal-paths") == 0)
		passed = FatalPaths(argv[0]);
	else if (argc == 2 && std::strcmp(argv[1], "ngs2-disabled") == 0) passed = Ngs2Disabled();
	else if (argc == 2 && std::strcmp(argv[1], "ngs2-enabled") == 0) passed = Ngs2Enabled();
	else if (argc == 3 && std::strcmp(argv[1], "ngs2-latch") == 0 &&
	         (std::strcmp(argv[2], "0") == 0 || std::strcmp(argv[2], "1") == 0))
		passed = Ngs2Latch(std::strcmp(argv[2], "1") == 0);
	else if (argc == 2 && std::strcmp(argv[1], "ngs2-fatal-paths") == 0)
		passed = Ngs2FatalPaths(argv[0]);
	else if (argc == 2 && std::strcmp(argv[1], "ngs2-state-disabled") == 0)
		passed = Ngs2StateFlagsDisabled();
	else if (argc == 2 && std::strcmp(argv[1], "ngs2-state-enabled") == 0)
		passed = Ngs2StateFlagsEnabled();
	else if (argc == 3 && std::strcmp(argv[1], "ngs2-state-latch") == 0 &&
	         (std::strcmp(argv[2], "0") == 0 || std::strcmp(argv[2], "1") == 0))
		passed = Ngs2StateFlagsLatch(std::strcmp(argv[2], "1") == 0);
	else if (argc == 2 && std::strcmp(argv[1], "ngs2-state-independence-waveform") == 0)
		passed = Ngs2StateFlagsIndependence(true, false);
	else if (argc == 2 && std::strcmp(argv[1], "ngs2-state-independence-state") == 0)
		passed = Ngs2StateFlagsIndependence(false, true);
	else if (argc == 2 && std::strcmp(argv[1], "ngs2-state-fatal-paths") == 0)
		passed = Ngs2StateFlagsFatalPaths(argv[0]);
#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
	else if (argc == 2 && std::strcmp(argv[1], "ngs2-get-state-memory") == 0)
	{
		const long page_size = sysconf(_SC_PAGESIZE);
		passed = Check(page_size > 0 && Ngs2::VoiceGetStateOutputContractTest(static_cast<size_t>(page_size)),
		               "NGS2 full state output memory contract failed");
	}
	else if (argc == 2 && std::strcmp(argv[1], "ngs2-render-queue") == 0)
		passed = Check(Ngs2::AudioRenderQueueContractTest(), "NGS2 queued audio render contract failed");
#endif
	else if (argc == 2 && std::strcmp(argv[1], "ngs2-render-disabled") == 0)
		passed = Ngs2AudioRenderDisabled();
	else if (argc == 2 && std::strcmp(argv[1], "ngs2-render-enabled") == 0)
		passed = Ngs2AudioRenderEnabled();
	else if (argc == 3 && std::strcmp(argv[1], "ngs2-render-latch") == 0 &&
	         (std::strcmp(argv[2], "0") == 0 || std::strcmp(argv[2], "1") == 0))
		passed = Ngs2AudioRenderLatch(std::strcmp(argv[2], "1") == 0);
	else if (argc == 2 && std::strcmp(argv[1], "ngs2-render-independence-waveform") == 0)
		passed = Ngs2AudioRenderIndependence(true, false, false);
	else if (argc == 2 && std::strcmp(argv[1], "ngs2-render-independence-state") == 0)
		passed = Ngs2AudioRenderIndependence(false, true, false);
	else if (argc == 2 && std::strcmp(argv[1], "ngs2-render-independence-render") == 0)
		passed = Ngs2AudioRenderIndependence(false, false, true);
	else if (argc == 3 && std::strcmp(argv[1], "fatal-child") == 0) return FatalChild(argv[2]);
	else
	{
		std::fprintf(stderr,
		             "Usage: VoiceTest disabled|enabled|latch 0|1|coexist|fatal-paths|"
		             "ngs2-disabled|ngs2-enabled|ngs2-latch 0|1|ngs2-fatal-paths|"
		             "ngs2-state-disabled|ngs2-state-enabled|ngs2-state-latch 0|1|"
		             "ngs2-state-independence-waveform|ngs2-state-independence-state|"
		             "ngs2-state-fatal-paths|ngs2-render-disabled|ngs2-render-enabled|"
		             "ngs2-render-latch 0|1|ngs2-render-independence-waveform|"
		             "ngs2-render-independence-state|ngs2-render-independence-render"
#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
		             "|ngs2-get-state-memory|ngs2-render-queue"
#endif
		             "\n");
		return 2;
	}
	if (!passed) return 1;
	std::puts("VoiceTest: PASS");
	return 0;
}
