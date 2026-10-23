#ifndef EMULATOR_INCLUDE_EMULATOR_AUDIO_H_
#define EMULATOR_INCLUDE_EMULATOR_AUDIO_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/Subsystems.h"

#include "Emulator/Common.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Audio {

LUMEN_SUBSYSTEM_DEFINE(Audio);

// Close every session-owned audio service while preserving the process-owned
// output backend for another guest. The caller must have stopped guest code,
// but RuntimeLinker, pthread/semaphore services and guest memory must remain
// valid until this returns because allocator callbacks may still be released.
[[nodiscard]] bool ResetForNextSession();

// Lower-level base-port reset used by the aggregate lifecycle above.
[[nodiscard]] bool ResetBasePortsForNextSession();

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
[[nodiscard]] bool AudioSessionResetContractTest();
#endif

namespace AudioOut {

struct AudioOutOutputParam;
struct AudioOutPortState;

int LUMEN_SYSV_ABI AudioOutInit();
int LUMEN_SYSV_ABI AudioOutOpen(int user_id, int type, int index, uint32_t len, uint32_t freq, uint32_t param);
int LUMEN_SYSV_ABI AudioOutSetVolume(int handle, uint32_t flag, int* vol);
int LUMEN_SYSV_ABI AudioOutOutputs(AudioOutOutputParam* param, uint32_t num);
int LUMEN_SYSV_ABI AudioOutOutput(int handle, const void* ptr);
int LUMEN_SYSV_ABI AudioOutClose(int handle);
int LUMEN_SYSV_ABI AudioOutGetPortState(int handle, AudioOutPortState* state);

} // namespace AudioOut

namespace AudioIn {

int LUMEN_SYSV_ABI AudioInOpen(int user_id, uint32_t type, uint32_t index, uint32_t len, uint32_t freq, uint32_t param);
int LUMEN_SYSV_ABI AudioInInput(int handle, void* dest);

} // namespace AudioIn

namespace VoiceQoS {

int LUMEN_SYSV_ABI VoiceQoSInit(void* mem_block, uint32_t mem_size, int32_t app_type);

} // namespace VoiceQoS

namespace AvPlayer {

struct AvPlayerInitData;
struct AvPlayerFrameInfoEx;
struct AvPlayerFrameInfo;
struct AvPlayerInternal;

using Bool = uint8_t;

AvPlayerInternal* LUMEN_SYSV_ABI AvPlayerInit(AvPlayerInitData* init);
int LUMEN_SYSV_ABI               AvPlayerAddSource(AvPlayerInternal* h, const char* filename);
int LUMEN_SYSV_ABI               AvPlayerSetLooping(AvPlayerInternal* h, Bool loop);
Bool LUMEN_SYSV_ABI              AvPlayerGetVideoDataEx(AvPlayerInternal* h, AvPlayerFrameInfoEx* video_info);
Bool LUMEN_SYSV_ABI              AvPlayerGetAudioData(AvPlayerInternal* h, AvPlayerFrameInfo* audio_info);
Bool LUMEN_SYSV_ABI              AvPlayerIsActive(AvPlayerInternal* h);
int LUMEN_SYSV_ABI               AvPlayerStop(AvPlayerInternal* h);
int LUMEN_SYSV_ABI               AvPlayerClose(AvPlayerInternal* h);

#if defined(LUMEN_AVPLAYER_LIFETIME_TEST_HOOK)
bool AvPlayerLifetimeRaceTest();
bool AvPlayerBufferHandoffTest();
#endif

} // namespace AvPlayer

namespace Audio3d {

struct Audio3dOpenParameters;

int LUMEN_SYSV_ABI  Audio3dInitialize(int64_t reserved);
void LUMEN_SYSV_ABI Audio3dGetDefaultOpenParameters(Audio3dOpenParameters* p);
int LUMEN_SYSV_ABI  Audio3dPortOpen(int user_id, const Audio3dOpenParameters* parameters, uint32_t* id);
int LUMEN_SYSV_ABI  Audio3dPortSetAttribute(uint32_t port_id, uint32_t attribute_id, const void* attribute, size_t attribute_size);
int LUMEN_SYSV_ABI  Audio3dPortGetQueueLevel(uint32_t port_id, uint32_t* queue_level, uint32_t* queue_available);
int LUMEN_SYSV_ABI  Audio3dPortAdvance(uint32_t port_id);
int LUMEN_SYSV_ABI  Audio3dPortPush(uint32_t port_id, uint32_t blocking);

} // namespace Audio3d

namespace Ngs2 {

struct Ngs2SystemOption;
struct Ngs2RackOption;
struct Ngs2BufferAllocator;
struct Ngs2VoiceParamHeader;
struct Ngs2RenderBufferInfo;
struct Ngs2ContextBufferInfo;
struct Ngs2VoiceState;
struct Ngs2WaveformInfo;

enum class Ngs2WaveformFrameInfoResult
{
	Ok,
	Disabled,
	InvalidInfo,
	NoOutputs,
	UnsupportedType,
	InvalidChannels,
	InvalidSampleRate,
	UnsupportedConfigData,
	InvalidAtrac9Config,
	Atrac9ChannelsMismatch,
	Atrac9SampleRateMismatch,
	InvalidAtrac9Geometry,
	UnsupportedFrameOffset,
	UnsupportedFrameMargin,
	InvalidOutput,
	AliasedMemory
};

enum class Ngs2VoiceStateFlagsResult
{
	Ok,
	Disabled,
	InvalidHandle,
	InvalidState,
	InvalidOutput,
	AliasedMemory
};

bool Ngs2WaveformFrameInfoExperimentalEnabled();
Ngs2WaveformFrameInfoResult Ngs2GetWaveformFrameInfoModel(uint64_t info_address, uint64_t frame_size_address,
                                                          uint64_t samples_per_frame_address,
                                                          uint64_t units_per_frame_address,
                                                          uint64_t delay_samples_address);
int32_t LUMEN_SYSV_ABI Ngs2GetWaveformFrameInfo(const Ngs2WaveformInfo* info, uint32_t* frame_size,
                                               uint32_t* samples_per_frame, uint32_t* units_per_frame,
                                               uint32_t* delay_samples);

bool Ngs2VoiceStateFlagsExperimentalEnabled();
Ngs2VoiceStateFlagsResult Ngs2VoiceGetStateFlagsModel(uintptr_t voice_handle, uint64_t state_flags_address);
int32_t LUMEN_SYSV_ABI Ngs2VoiceGetStateFlags(uintptr_t voice_handle, uint32_t* state_flags);

bool Ngs2AudioRenderExperimentalEnabled();

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
[[nodiscard]] bool VoiceGetStateOutputContractTest(size_t page_size);
[[nodiscard]] bool AudioRenderQueueContractTest();
#endif

int LUMEN_SYSV_ABI Ngs2RackQueryBufferSize(uint32_t rack_id, const Ngs2RackOption* option, Ngs2ContextBufferInfo* buffer_info);
int LUMEN_SYSV_ABI Ngs2RackCreate(uintptr_t system_handle, uint32_t rack_id, const Ngs2RackOption* option,
                                 const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle);
int LUMEN_SYSV_ABI Ngs2SystemCreateWithAllocator(const Ngs2SystemOption* option, const Ngs2BufferAllocator* allocator, uintptr_t* handle);
int LUMEN_SYSV_ABI Ngs2RackCreateWithAllocator(uintptr_t system_handle, uint32_t rack_id, const Ngs2RackOption* option,
                                              const Ngs2BufferAllocator* allocator, uintptr_t* handle);
int LUMEN_SYSV_ABI Ngs2RackGetVoiceHandle(uintptr_t rack_handle, uint32_t voice_id, uintptr_t* handle);
int LUMEN_SYSV_ABI Ngs2VoiceControl(uintptr_t voice_handle, const Ngs2VoiceParamHeader* param_list);
int LUMEN_SYSV_ABI Ngs2VoiceGetState(uintptr_t voice_handle, Ngs2VoiceState* state, size_t state_size);
int LUMEN_SYSV_ABI Ngs2SystemRender(uintptr_t system_handle, const Ngs2RenderBufferInfo* buffer_info, uint32_t num_buffer_info);

} // namespace Ngs2

} // namespace Lumen::Libs::Audio

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_AUDIO_H_ */
