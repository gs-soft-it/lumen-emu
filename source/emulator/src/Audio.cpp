#include "Emulator/Audio.h"
#include "Emulator/AudioOutputBackend.h"
#include "Emulator/AudioOutputHostWait.h"

#include "Emulator/Config.h"
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
#include "Emulator/AvPlayerMac.h"
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#include "Emulator/AvPlayerAndroid.h"
#endif

#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/MagicEnum.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Kernel/Semaphore.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Libs/AjmCore.h"

#include <libatrac9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdarg>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#if defined(LUMEN_AVPLAYER_LIFETIME_TEST_HOOK) || defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
#include "../../cpu/runtime/MemoryHandoff.h"
#include <thread>
#endif

// Windows Media Foundation for video decoding
#ifdef _WIN32
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#ifdef _MSC_VER
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#endif
#endif

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Audio {

static bool BorrowAudioGuestRange(const void* address, uint64_t bytes,
                                 Core::VirtualMemory::Mode mode,
                                 Core::VirtualMemory::GuestDataLease* lease)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || \
    LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	return Core::VirtualMemory::TryAcquireGuestDataLease(
	    reinterpret_cast<uint64_t>(address), bytes, mode, lease);
#else
	// The native Windows VM backend has no mapping-lease capability yet.
	(void)address; (void)bytes; (void)mode; (void)lease;
	return true;
#endif
}

static bool SnapshotAudioGuestString(const char* source, size_t limit, std::string* owned)
{
	if (source == nullptr || owned == nullptr) return false;
	owned->clear();
	const auto address = reinterpret_cast<uintptr_t>(source);
	for (size_t i = 0; i < limit; ++i)
	{
		if (i > UINTPTR_MAX - address) return false;
		auto* character = reinterpret_cast<const char*>(address + i);
		Core::VirtualMemory::GuestDataLease input;
		if (!BorrowAudioGuestRange(character, 1, Core::VirtualMemory::Mode::Read, &input)) return false;
		const char value = *character;
		if (value == '\0') return true;
		owned->push_back(value);
	}
	return false;
}

static bool AudioOutputTraceEnabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_AUDIO_OUTPUT_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

static bool AudioLogEnabled()
{
	return AudioOutputTraceEnabled() || Config::DiagnosticsEnabled() ||
	       Log::GetDirection() != Log::Direction::Silent ||
	       Log::IsCustomLogEnabled();
}

static void AudioLog(const char* format, ...)
{
	if (!AudioLogEnabled())
	{
		return;
	}

	va_list args {};
	va_start(args, format);
	String line;
	line.Printf(format, args);
	va_end(args);

	if (Log::GetDirection() == Log::Direction::Silent)
	{
		if (AudioOutputTraceEnabled() || Config::DiagnosticsEnabled() ||
		    Log::IsCustomLogEnabled())
		{
			dbg_printf("%s", line.C_Str());
		}
	} else
	{
		printf("%s", line.C_Str());
	}
}

template <size_t Capacity>
class MutexLockSet
{
public:
	MutexLockSet() = default;
	~MutexLockSet()
	{
		while (m_size != 0)
		{
			m_mutexes[--m_size]->Unlock();
		}
	}

	LUMEN_CLASS_NO_COPY(MutexLockSet);

	void Lock(Core::Mutex* mutex)
	{
		EXIT_IF(mutex == nullptr || m_size >= Capacity);
		mutex->Lock();
		m_mutexes[m_size++] = mutex;
	}

private:
	Core::Mutex* m_mutexes[Capacity] {};
	size_t       m_size = 0;
};

class Audio
{
public:
	enum class Format
	{
		Unknown,
		Signed16bitMono,
		Signed16bitStereo,
		Signed16bit8Ch,
		FloatMono,
		FloatStereo,
		Float8Ch,
		Signed16bit8ChStd,
		Float8ChStd,
	};

	class Id
	{
	public:
		explicit Id(int id): m_id(id - 1) {}
		[[nodiscard]] int  ToInt() const { return m_id + 1; }
		[[nodiscard]] bool IsValid() const { return m_id >= 0; }

		friend class Audio;

	private:
		Id() = default;
		static Id Invalid() { return {}; }
		static Id Create(int audio_id)
		{
			Id r;
			r.m_id = audio_id;
			return r;
		}
		[[nodiscard]] int GetId() const { return m_id; }

		int m_id = -1;
	};

	struct OutputParam
	{
		Id          handle;
		const void* data = nullptr;
	};

	Audio();
	virtual ~Audio();

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
	struct SessionResetTestTag
	{
	};
	explicit Audio(SessionResetTestTag) {}
	void SeedSessionResetContractState();
	bool HasClearedSessionResetContractState();
#endif

	LUMEN_CLASS_NO_COPY(Audio);

	Id       AudioOutOpen(int type, uint32_t samples_num, uint32_t freq, Format format);
	bool     AudioOutClose(Id handle);
	bool     AudioOutValid(Id handle);
	int      AudioOutSetVolume(Id handle, uint32_t bitflag, const int* volume);
	int      AudioOutOutputs(OutputParam* params, uint32_t num, int* first_channels_num = nullptr);
	bool     AudioOutGetStatus(Id handle, int* type, int* channels_num, uint32_t* samples_num);

	Id       AudioInOpen(uint32_t type, uint32_t samples_num, uint32_t freq, Format format);
	bool     AudioInValid(Id handle);
	uint32_t AudioInInput(Id handle, void* dest);
	void     ResetBasePortsForNextSession();

	static constexpr int OUT_PORTS_MAX = 32;
	static constexpr int IN_PORTS_MAX  = 8;

private:
	struct PortOut
	{
		bool                                used                  = false;
		int                                 type                  = 0;
		uint32_t                            samples_num           = 0;
		uint32_t                            freq                  = 0;
		Format                              format                = Format::Unknown;
		Host::PacingState                   pacing;
		uint64_t                            backend_submit_errors = 0;
		uint64_t                            backend_trace_submits  = 0;
		int                                 channels_num          = 0;
		int                                 volume[8]             = {};
		Core::Mutex                         submit_mutex;
		std::unique_ptr<Host::OutputStream> output;
	};

	struct PortIn
	{
		bool     used            = false;
		uint32_t type            = 0;
		uint32_t samples_num     = 0;
		uint32_t freq            = 0;
		Format   format          = Format::Unknown;
		uint64_t last_input_time = 0;
	};

	void CloseOutputStream(PortOut* port, int id);

	Host::OutputBackend m_output_backend;
	Core::Mutex         m_mutex;
	PortOut             m_out_ports[OUT_PORTS_MAX];
	PortIn              m_in_ports[IN_PORTS_MAX];
};

static Audio* g_audio = nullptr;

Audio::Audio()
{
	std::string error;
	if (m_output_backend.Initialize(&error))
	{
		AudioLog("[AudioOut] backend=sdl driver=%s status=ready\n", m_output_backend.DriverName().c_str());
	} else
	{
		AudioLog("[AudioOut] backend=timed-fallback status=ready error=%s\n", error.c_str());
	}
}

Audio::~Audio()
{
	for (int id = 0; id < OUT_PORTS_MAX; id++)
	{
		CloseOutputStream(&m_out_ports[id], id);
	}
}

void Audio::CloseOutputStream(PortOut* port, int id)
{
	EXIT_IF(port == nullptr);

	if (port->output != nullptr)
	{
		const auto stats = port->output->GetStats();
		AudioLog("[AudioOut] port=%d closed blocks=%" PRIu64 " input_bytes=%" PRIu64 " output_bytes=%" PRIu64
		         " consumed_bytes=%" PRIu64 " callbacks=%" PRIu64 " underrun_frames=%" PRIu64
		         " inaudible_blocks=%" PRIu64 " submitted_peak=%.9g callback_peak=%.9g queue_resets=%" PRIu64
		         " overflow_events=%" PRIu64 " dropped_overrun_frames=%" PRIu64 " submit_errors=%" PRIu64
		         " capacity_waits=%" PRIu64 " capacity_wait_us=%" PRIu64 " capacity_timeouts=%" PRIu64
		         " capacity_cancels=%" PRIu64 "\n",
		         id + 1, stats.submitted_blocks, stats.input_bytes, stats.output_bytes, stats.consumed_output_bytes,
		         stats.callback_calls, stats.underrun_frames, stats.inaudible_blocks, static_cast<double>(stats.max_submitted_peak),
		         static_cast<double>(stats.max_callback_peak), stats.queue_resets, stats.queue_overflow_events,
		         stats.dropped_overrun_frames, stats.submit_errors, stats.capacity_wait_events, stats.capacity_wait_us,
		         stats.capacity_wait_timeouts, stats.capacity_wait_cancels);
		port->output.reset();
	}
	port->backend_submit_errors = 0;
	port->backend_trace_submits  = 0;
}

LUMEN_SUBSYSTEM_INIT(Audio)
{
	EXIT_IF(g_audio != nullptr);

	g_audio = new Audio;
}

// The desktop atexit path clears RuntimeLinker before UnexpectedShutdown, so
// guest-backed allocator callbacks and Ngs2 lists are no longer safe here.
// Repeatable-session teardown calls ResetForNextSession explicitly while the
// linker and guest mappings still exist.
LUMEN_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Audio) {}

LUMEN_SUBSYSTEM_DESTROY(Audio)
{
	delete g_audio;
	g_audio = nullptr;
}

void Audio::ResetBasePortsForNextSession()
{
	Core::LockGuard lock(m_mutex);
	MutexLockSet<OUT_PORTS_MAX> submit_locks;
	for (auto& port: m_out_ports)
	{
		if (port.output != nullptr) port.output->CancelPendingSubmissions();
	}
	for (auto& port: m_out_ports)
	{
		submit_locks.Lock(&port.submit_mutex);
	}

	for (int id = 0; id < OUT_PORTS_MAX; id++)
	{
		auto& port = m_out_ports[id];
		CloseOutputStream(&port, id);
		port.used         = false;
		port.type         = 0;
		port.samples_num  = 0;
		port.freq         = 0;
		port.format       = Format::Unknown;
		port.pacing       = {};
		port.channels_num = 0;
		for (int& volume: port.volume)
		{
			volume = 0;
		}
	}

	for (auto& port: m_in_ports)
	{
		port.used            = false;
		port.type            = 0;
		port.samples_num     = 0;
		port.freq            = 0;
		port.format          = Format::Unknown;
		port.last_input_time = 0;
	}
}

bool ResetBasePortsForNextSession()
{
	if (g_audio == nullptr)
	{
		return false;
	}

	g_audio->ResetBasePortsForNextSession();
	return true;
}

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
void Audio::SeedSessionResetContractState()
{
	auto& output                 = m_out_ports[0];
	output.used                  = true;
	output.type                  = 3;
	output.samples_num           = 1024;
	output.freq                  = 48000;
	output.format                = Format::FloatStereo;
	output.pacing.deadline_us    = 1234;
	output.pacing.remainder      = 12;
	output.pacing.sample_rate    = 48000;
	output.pacing.refill_pending = true;
	output.backend_submit_errors = 7;
	output.backend_trace_submits  = 11;
	output.channels_num          = 2;
	output.volume[0]             = 32768;

	auto& input           = m_in_ports[0];
	input.used            = true;
	input.type            = 1;
	input.samples_num     = 512;
	input.freq            = 48000;
	input.format          = Format::Signed16bitMono;
	input.last_input_time = 9876;
}

bool Audio::HasClearedSessionResetContractState()
{
	for (const auto& output: m_out_ports)
	{
		if (output.used || output.type != 0 || output.samples_num != 0 || output.freq != 0 || output.format != Format::Unknown ||
		    output.pacing.deadline_us != 0 || output.pacing.remainder != 0 || output.pacing.sample_rate != 0 ||
		    output.pacing.refill_pending ||
		    output.backend_submit_errors != 0 || output.backend_trace_submits != 0 ||
		    output.channels_num != 0 || output.output != nullptr)
		{
			return false;
		}
		for (int volume: output.volume)
		{
			if (volume != 0)
			{
				return false;
			}
		}
	}
	for (const auto& input: m_in_ports)
	{
		if (input.used || input.type != 0 || input.samples_num != 0 || input.freq != 0 || input.format != Format::Unknown ||
		    input.last_input_time != 0)
		{
			return false;
		}
	}
	return true;
}

static bool AudioBasePortsSessionResetContractTest()
{
	Audio audio(Audio::SessionResetTestTag {});
	for (int cycle = 0; cycle < 2; cycle++)
	{
		audio.SeedSessionResetContractState();
		audio.ResetBasePortsForNextSession();
		if (!audio.HasClearedSessionResetContractState())
		{
			return false;
		}
	}
	return true;
}
#endif

Audio::Id Audio::AudioOutOpen(int type, uint32_t samples_num, uint32_t freq, Format format)
{
	Core::LockGuard lock(m_mutex);

	for (int id = 0; id < OUT_PORTS_MAX; id++)
	{
		if (!m_out_ports[id].used)
		{
			auto& port = m_out_ports[id];
			EXIT_IF(port.output != nullptr);

			port.used                  = true;
			port.type                  = type;
			port.samples_num           = samples_num;
			port.freq                  = freq;
			port.format                = format;
			port.pacing                = {};
			port.backend_submit_errors = 0;
			port.backend_trace_submits  = 0;

			switch (format)
			{
				case Format::Signed16bitMono:
				case Format::FloatMono: port.channels_num = 1; break;
				case Format::Signed16bitStereo:
				case Format::FloatStereo: port.channels_num = 2; break;
				case Format::Signed16bit8Ch:
				case Format::Float8Ch:
				case Format::Signed16bit8ChStd:
				case Format::Float8ChStd: port.channels_num = 8; break;
				default: EXIT("unknown format");
			}

			for (int& channel_volume: port.volume)
			{
				channel_volume = 32768;
			}

			Host::StreamSpec spec {};
			spec.sample_rate      = port.freq;
			spec.frames_per_block = port.samples_num;
			spec.channels         = static_cast<uint32_t>(port.channels_num);
			spec.sample_format    = (format == Format::Signed16bitMono || format == Format::Signed16bitStereo ||
			                         format == Format::Signed16bit8Ch || format == Format::Signed16bit8ChStd)
			                            ? Host::SampleFormat::Signed16
			                            : Host::SampleFormat::Float32;
			spec.standard_8ch = (format == Format::Signed16bit8ChStd || format == Format::Float8ChStd);

			std::string error;
			port.output = m_output_backend.Open(spec, &error);
			if (port.output != nullptr)
			{
				AudioLog("[AudioOut] port=%d backend=sdl driver=%s format=%s rate=%u channels=%d frames=%u\n", id + 1,
				         m_output_backend.DriverName().c_str(), Core::EnumName(format).C_Str(), port.freq, port.channels_num,
				         port.samples_num);
			} else
			{
				AudioLog("[AudioOut] port=%d backend=timed-fallback format=%s rate=%u channels=%d frames=%u error=%s\n", id + 1,
				         Core::EnumName(format).C_Str(), port.freq, port.channels_num, port.samples_num, error.c_str());
			}

			return Id::Create(id);
		}
	}

	return Id::Invalid();
}

bool Audio::AudioOutClose(Id handle)
{
	Core::LockGuard lock(m_mutex);

	if (AudioOutValid(handle))
	{
		auto& port = m_out_ports[handle.GetId()];
		if (port.output != nullptr) port.output->CancelPendingSubmissions();
		Core::LockGuard submit_lock(port.submit_mutex);
		CloseOutputStream(&port, handle.GetId());
		port.used   = false;
		port.pacing = {};
		return true;
	}

	return false;
}

bool Audio::AudioOutValid(Id handle)
{
	Core::LockGuard lock(m_mutex);

	return (handle.GetId() >= 0 && handle.GetId() < OUT_PORTS_MAX && m_out_ports[handle.GetId()].used);
}

bool Audio::AudioOutGetStatus(Id handle, int* type, int* channels_num, uint32_t* samples_num)
{
	Core::LockGuard lock(m_mutex);

	if (AudioOutValid(handle))
	{
		auto& port = m_out_ports[handle.GetId()];

		if (type != nullptr)
		{
			*type = port.type;
		}
		if (channels_num != nullptr)
		{
			*channels_num = port.channels_num;
		}
		if (samples_num != nullptr)
		{
			*samples_num = port.samples_num;
		}

		return true;
	}

	return false;
}

int Audio::AudioOutSetVolume(Id handle, uint32_t bitflag, const int* volume)
{
	int channels = 0;
	Format format = Format::Unknown;
	{
		Core::LockGuard lock(m_mutex);
		if (!AudioOutValid(handle)) return AUDIO_OUT_ERROR_INVALID_PORT;
		const auto& port = m_out_ports[handle.GetId()];
		channels = port.channels_num;
		format = port.format;
	}
	std::array<int, 8> owned_volume {};
	for (int i = 0; i < channels; ++i)
	{
		if ((bitflag & (1u << i)) == 0) continue;
		int source = i;
		if (format == Format::Float8ChStd || format == Format::Signed16bit8ChStd)
		{
			switch (i)
			{
				case 4: source = 6; break;
				case 5: source = 7; break;
				case 6: source = 4; break;
				case 7: source = 5; break;
				default: break;
			}
		}
		Core::VirtualMemory::GuestDataLease input;
		if (!BorrowAudioGuestRange(volume + source, sizeof(int), Core::VirtualMemory::Mode::Read, &input))
			return AUDIO_OUT_ERROR_INVALID_POINTER;
		owned_volume[source] = volume[source];
	}
	volume = owned_volume.data();
	Core::LockGuard lock(m_mutex);

	if (!AudioOutValid(handle))
	{
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}

	auto& port = m_out_ports[handle.GetId()];
	if (port.channels_num != channels || port.format != format) return AUDIO_OUT_ERROR_INVALID_PORT;
	int   source_indices[8] {};
	for (int i = 0; i < port.channels_num; i++)
	{
		source_indices[i] = i;
		if (port.format == Format::Float8ChStd || port.format == Format::Signed16bit8ChStd)
		{
			switch (i)
			{
				case 4: source_indices[i] = 6; break;
				case 5: source_indices[i] = 7; break;
				case 6: source_indices[i] = 4; break;
				case 7: source_indices[i] = 5; break;
				default:;
			}
		}
		if ((bitflag & (1u << i)) != 0 && volume[source_indices[i]] > 32768)
		{
			return AUDIO_OUT_ERROR_INVALID_VOLUME;
		}
	}

	for (int i = 0; i < port.channels_num; i++)
	{
		if ((bitflag & (1u << i)) != 0)
		{
			port.volume[i] = volume[source_indices[i]];
			printf("\t port.volume[%d] = volume[%d] (%d)\n", i, source_indices[i], volume[source_indices[i]]);
		}
	}

	return OK;
}

int Audio::AudioOutOutputs(OutputParam* params, uint32_t num, int* first_channels_num)
{
	if (params == nullptr)
	{
		return AUDIO_OUT_ERROR_INVALID_POINTER;
	}
	if (num == 0 || num > OUT_PORTS_MAX)
	{
		return AUDIO_OUT_ERROR_INVALID_SIZE;
	}
	struct InputSnapshot
	{
		uint32_t samples = 0, frequency = 0;
		int channels = 0;
		Format format = Format::Unknown;
		Host::OutputStream* stream = nullptr;
	};
	std::array<InputSnapshot, OUT_PORTS_MAX> input_specs {};
	std::array<std::vector<uint8_t>, OUT_PORTS_MAX> input_bytes;
	{
		Core::LockGuard lock(m_mutex);
		for (uint32_t i = 0; i < num; ++i)
		{
			const int id = params[i].handle.GetId();
			if (id < 0 || id >= OUT_PORTS_MAX || !m_out_ports[id].used)
				return AUDIO_OUT_ERROR_INVALID_PORT;
			for (uint32_t previous = 0; previous < i; ++previous)
				if (params[previous].handle.GetId() == id) return AUDIO_OUT_ERROR_INVALID_PORT;
			const auto& port = m_out_ports[id];
			input_specs[i] = {port.samples_num, port.freq, port.channels_num, port.format, port.output.get()};
			if (i != 0 && port.samples_num != input_specs[0].samples) return AUDIO_OUT_ERROR_INVALID_SIZE;
		}
	}
	// Resolve GPU producers and copy the guest payload before taking port or
	// backend locks. Capacity waits subsequently use owned audio samples.
	for (uint32_t i = 0; i < num; ++i)
	{
		const auto& spec = input_specs[i];
		if (params[i].data == nullptr || spec.stream == nullptr) continue;
		const bool signed16 = spec.format == Format::Signed16bitMono || spec.format == Format::Signed16bitStereo ||
		                      spec.format == Format::Signed16bit8Ch || spec.format == Format::Signed16bit8ChStd;
		const uint64_t bytes = uint64_t {spec.samples} * static_cast<uint64_t>(spec.channels) * (signed16 ? 2U : 4U);
		Core::VirtualMemory::GuestDataLease input;
		if (!BorrowAudioGuestRange(params[i].data, bytes, Core::VirtualMemory::Mode::Read, &input))
			return AUDIO_OUT_ERROR_INVALID_POINTER;
		input_bytes[i].resize(static_cast<size_t>(bytes));
		std::memcpy(input_bytes[i].data(), params[i].data, static_cast<size_t>(bytes));
	}

	uint64_t max_pacing_deadline     = 0;
	uint64_t nominal_pacing_deadline = 0;
	bool     buffered_refill        = false;
	uint32_t samples_num            = 0;
	Host::OutputBatchItem batch[OUT_PORTS_MAX] {};
	std::array<std::array<int, 8>, OUT_PORTS_MAX> batch_volumes {};
	Host::PacingState batch_pacing {};
	uint32_t batch_count = 0;
	uint64_t current_time = 0;
	// Both wrappers supply host-stack descriptors and first_channels_num. All
	// PCM is now owned, with no live guest lease; the remainder uses native port
	// state only. Drop the reader before taking any port lock, and release every
	// submit lock before readmission (also on validation/submission failures).
	Host::OutputHostWait<Loader::ScopedGuestHostWait, MutexLockSet<OUT_PORTS_MAX>> host_work;
	auto& submit_locks = host_work.Locks();
	{
		Core::LockGuard lock(m_mutex);

		for (uint32_t i = 0; i < num; i++)
		{
			const int id = params[i].handle.GetId();
			if (id < 0 || id >= OUT_PORTS_MAX || !m_out_ports[id].used)
			{
				return AUDIO_OUT_ERROR_INVALID_PORT;
			}
			const auto& port = m_out_ports[id];
			const auto& spec = input_specs[i];
			if (port.samples_num != spec.samples || port.freq != spec.frequency || port.channels_num != spec.channels ||
			    port.format != spec.format || port.output.get() != spec.stream)
				return AUDIO_OUT_ERROR_INVALID_PORT;
			for (uint32_t previous = 0; previous < i; previous++)
			{
				if (params[previous].handle.GetId() == id)
				{
					return AUDIO_OUT_ERROR_INVALID_PORT;
				}
			}
		}

		samples_num = m_out_ports[params[0].handle.GetId()].samples_num;
		if (first_channels_num != nullptr)
		{
			*first_channels_num = m_out_ports[params[0].handle.GetId()].channels_num;
		}
		for (uint32_t i = 1; i < num; i++)
		{
			if (m_out_ports[params[i].handle.GetId()].samples_num != samples_num)
			{
				return AUDIO_OUT_ERROR_INVALID_SIZE;
			}
		}

		int ordered_ids[OUT_PORTS_MAX] {};
		for (uint32_t i = 0; i < num; i++)
		{
			ordered_ids[i] = params[i].handle.GetId();
		}
		std::sort(ordered_ids, ordered_ids + num);
		for (uint32_t i = 0; i < num; i++)
		{
			submit_locks.Lock(&m_out_ports[ordered_ids[i]].submit_mutex);
		}

		current_time = LibKernel::KernelGetProcessTime();
		bool              have_batch_pacing = false;
		for (uint32_t i = 0; i < num; i++)
		{
			auto&          port     = m_out_ports[params[i].handle.GetId()];
			const uint64_t deadline = Host::AdvancePacingDeadline(&port.pacing, current_time, port.samples_num, port.freq);
			if (!have_batch_pacing || deadline > batch_pacing.deadline_us ||
			    (deadline == batch_pacing.deadline_us && port.pacing.remainder > batch_pacing.remainder))
			{
				batch_pacing      = port.pacing;
				have_batch_pacing = true;
			}
		}
		max_pacing_deadline = batch_pacing.deadline_us;
		nominal_pacing_deadline = max_pacing_deadline;

		for (uint32_t i = 0; i < num; i++)
		{
			auto& port = m_out_ports[params[i].handle.GetId()];
			if (params[i].data != nullptr && port.output != nullptr)
			{
				batch[batch_count].stream            = port.output.get();
				batch[batch_count].data              = input_bytes[i].data();
				std::copy_n(port.volume, 8, batch_volumes[batch_count].data());
				batch[batch_count].canonical_volumes = batch_volumes[batch_count].data();
				batch_count++;
			}
			port.pacing = batch_pacing;
		}
		if (batch_count != num)
		{
			for (uint32_t i = 0; i < num; i++)
			{
				m_out_ports[params[i].handle.GetId()].pacing.refill_pending = false;
			}
		}

	}

	// Port submit locks preserve stream lifetime; volume snapshots are owned.
	// Close/reset can request cancellation under m_mutex while this host-only
	// capacity wait completes, without reacquiring m_mutex or the VM barrier.
	if (batch_count != 0)
	{
		std::string error;
		const bool  submitted = m_output_backend.SubmitBlockingBatch(batch, batch_count, &error);
		if (!submitted)
		{
			for (uint32_t i = 0; i < num; i++)
			{
				auto& port = m_out_ports[params[i].handle.GetId()];
				if (params[i].data != nullptr && port.output != nullptr)
				{
					port.backend_submit_errors++;
					if (port.backend_submit_errors <= 3 ||
					    (port.backend_submit_errors & (port.backend_submit_errors - 1)) == 0)
					{
						AudioLog("[AudioOut] port=%d submit_error=%" PRIu64 " error=%s\n", params[i].handle.ToInt(),
						         port.backend_submit_errors, error.c_str());
					}
				}
			}
			// Host capacity/close failures never acknowledge uncommitted PCM.
			// SYSTEM_RESOURCE is a local fail-closed mapping, not a qualified console result.
			return AUDIO_OUT_ERROR_SYSTEM_RESOURCE;
		} else
		{
			// Refill every submitted port after a producer stall, then resume
			// nominal block pacing. A partial batch cannot advance the clock.
			if (batch_count == num)
			{
				constexpr uint64_t host_stereo_frame_bytes = 2 * sizeof(float);
				std::array<size_t, OUT_PORTS_MAX> queued_frames {};
				std::array<size_t, OUT_PORTS_MAX> target_frames {};
				for (uint32_t i = 0; i < num; i++)
				{
					const auto& port = m_out_ports[params[i].handle.GetId()];
					const auto stats = port.output->GetStats();
					target_frames[i] = Host::BufferedQueueTargetFrames(port.samples_num, port.freq);
					queued_frames[i] = static_cast<size_t>(stats.queued_output_bytes / host_stereo_frame_bytes);
					if (stats.queued_output_bytes % host_stereo_frame_bytes != 0)
					{
						target_frames[i] = 0;
					}
				}
				const bool allow_refill =
				    Host::ShouldRefillBufferedBatch(queued_frames.data(), target_frames.data(), batch_count, num);
				buffered_refill = batch_pacing.refill_pending && allow_refill;
				const uint64_t refill_now = buffered_refill ? LibKernel::KernelGetProcessTime() : current_time;
				max_pacing_deadline = 0;
				for (uint32_t i = 0; i < num; i++)
				{
					auto& port = m_out_ports[params[i].handle.GetId()];
					max_pacing_deadline = std::max(max_pacing_deadline, Host::ApplyBufferedPacingDeadline(
					    &port.pacing, refill_now, port.samples_num, port.freq, queued_frames[i], allow_refill));
				}
			}
			for (uint32_t i = 0; i < num; i++)
			{
				if (!AudioLogEnabled()) break;
				auto& port = m_out_ports[params[i].handle.GetId()];
				if (params[i].data == nullptr || port.output == nullptr)
				{
					continue;
				}
				port.backend_trace_submits++;
				if (port.backend_trace_submits <= 3 ||
				    (port.backend_trace_submits &
				     (port.backend_trace_submits - 1)) == 0)
				{
					const auto stats = port.output->GetStats();
					const uint64_t wall_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
					    std::chrono::steady_clock::now().time_since_epoch()).count());
					const uint64_t process_us = LibKernel::KernelGetProcessTime();
					// Snapshot the pending sleep before logging; the actual bounded wait is recomputed below.
					const uint64_t sleep_requested_us =
					    max_pacing_deadline > process_us ? max_pacing_deadline - process_us : 0;
					AudioLog("[AudioOut] port=%d blocks=%" PRIu64 " consumed_bytes=%" PRIu64 " queued_bytes=%" PRIu64
					         " callbacks=%" PRIu64 " underrun_frames=%" PRIu64 " inaudible_blocks=%" PRIu64
					         " submitted_peak=%.9g callback_peak=%.9g queue_resets=%" PRIu64
					         " overflow_events=%" PRIu64 " dropped_overrun_frames=%" PRIu64
					         " wall_us=%" PRIu64 " process_us=%" PRIu64 " nominal_deadline_us=%" PRIu64
					         " pacing_deadline_us=%" PRIu64 " buffer_refill=%u sleep_requested_us=%" PRIu64
					         " queued_frames=%" PRIu64 " target_frames=%zu capacity_waits=%" PRIu64
					         " capacity_wait_us=%" PRIu64 " capacity_timeouts=%" PRIu64 " capacity_cancels=%" PRIu64 "\n",
					         params[i].handle.ToInt(), stats.submitted_blocks, stats.consumed_output_bytes,
					         stats.queued_output_bytes, stats.callback_calls, stats.underrun_frames, stats.inaudible_blocks,
					         static_cast<double>(stats.max_submitted_peak), static_cast<double>(stats.max_callback_peak),
					         stats.queue_resets, stats.queue_overflow_events, stats.dropped_overrun_frames,
					         wall_us, process_us, nominal_pacing_deadline, max_pacing_deadline,
					         static_cast<unsigned>(buffered_refill), sleep_requested_us,
					         stats.queued_output_bytes / (2 * sizeof(float)),
					         Host::BufferedQueueTargetFrames(port.samples_num, port.freq), stats.capacity_wait_events,
					         stats.capacity_wait_us, stats.capacity_wait_timeouts, stats.capacity_wait_cancels);
				}
			}
		}
	}

	const uint64_t after_submit = LibKernel::KernelGetProcessTime();
	// Keep the submit locks through pacing to serialize output and bound the
	// backend queue. host_work releases them before restoring the guest reader.
	Core::Thread::SleepMicro(max_pacing_deadline > after_submit ? max_pacing_deadline - after_submit : 0);
	return static_cast<int>(samples_num);
}

Audio::Id Audio::AudioInOpen(uint32_t type, uint32_t samples_num, uint32_t freq, Format format)
{
	Core::LockGuard lock(m_mutex);

	for (int id = 0; id < IN_PORTS_MAX; id++)
	{
		if (!m_in_ports[id].used)
		{
			auto& port = m_in_ports[id];

			port.used        = true;
			port.type        = type;
			port.samples_num = samples_num;
			port.freq        = freq;
			port.format      = format;

			switch (format)
			{
				case Format::Signed16bitMono:
				case Format::Signed16bitStereo: break;
				default: EXIT("unknown format");
			}

			return Id::Create(id);
		}
	}

	return Id::Invalid();
}

bool Audio::AudioInValid(Id handle)
{
	Core::LockGuard lock(m_mutex);

	return (handle.GetId() >= 0 && handle.GetId() < IN_PORTS_MAX && m_in_ports[handle.GetId()].used);
}

uint32_t Audio::AudioInInput(Id handle, void* dest)
{
	EXIT_NOT_IMPLEMENTED(!AudioInValid(handle));
	EXIT_NOT_IMPLEMENTED(dest == nullptr);

	const auto& port = m_in_ports[handle.GetId()];

	uint64_t block_time   = (1000000 * port.samples_num) / port.freq;
	uint64_t current_time = LibKernel::KernelGetProcessTime();

	uint64_t next_time = m_in_ports[handle.GetId()].last_input_time + block_time;
	uint64_t wait_time = (next_time > current_time ? next_time - current_time : 0);

	// TODO(): Audio input is not yet implemented, so simulate audio delay
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	{
		Loader::ScopedGuestHostWait host_wait;
		Core::Thread::SleepMicro(wait_time);
	}
#else
	Core::Thread::SleepMicro(wait_time);
#endif

	m_in_ports[handle.GetId()].last_input_time = LibKernel::KernelGetProcessTime();

	return port.samples_num;
}

namespace AudioOut {

LIB_NAME("AudioOut", "AudioOut");

struct AudioOutOutputParam
{
	int         handle;
	const void* ptr;
};

struct AudioOutPortState
{
	uint16_t output;
	uint8_t  channel;
	uint8_t  reserved1[1];
	int16_t  volume;
	uint16_t reroute_counter;
	uint64_t flag;
	uint64_t reserved2[2];
};

int LUMEN_SYSV_ABI AudioOutInit()
{
	PRINT_NAME();

	return OK;
}

int LUMEN_SYSV_ABI AudioOutOpen(int user_id, int type, int index, uint32_t len, uint32_t freq, uint32_t param)
{
	PRINT_NAME();

	printf("\t user_id = %d\n", user_id);
	printf("\t type    = %d\n", type);
	printf("\t index   = %d\n", index);
	printf("\t len     = %u\n", len);
	printf("\t freq    = %u\n", freq);

	if (user_id != 255 && user_id != 1)
	{
		return AUDIO_OUT_ERROR_INVALID_PARAM;
	}
	if (type < 0 || type > 4)
	{
		return AUDIO_OUT_ERROR_INVALID_PORT_TYPE;
	}
	if (index != 0)
	{
		return AUDIO_OUT_ERROR_INVALID_PARAM;
	}
	if (len == 0 || len > 2048 || (len & 0xffu) != 0)
	{
		return AUDIO_OUT_ERROR_INVALID_SIZE;
	}
	if (freq != 48000)
	{
		return AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ;
	}
	constexpr uint32_t AUDIO_OUT_PARAM_ATTR_RESTRICTED  = 0x00010000u;
	constexpr uint32_t AUDIO_OUT_PARAM_ATTR_MIX_TO_MAIN = 0x00020000u;
	constexpr uint32_t AUDIO_OUT_PARAM_ALLOWED = 0xffu | AUDIO_OUT_PARAM_ATTR_RESTRICTED | AUDIO_OUT_PARAM_ATTR_MIX_TO_MAIN;
	const uint32_t attributes = param & ~0xffu;
	if ((param & ~AUDIO_OUT_PARAM_ALLOWED) != 0 ||
	    attributes == (AUDIO_OUT_PARAM_ATTR_RESTRICTED | AUDIO_OUT_PARAM_ATTR_MIX_TO_MAIN) ||
	    (type != 3 && attributes == AUDIO_OUT_PARAM_ATTR_MIX_TO_MAIN))
	{
		return AUDIO_OUT_ERROR_INVALID_FORMAT;
	}

	Audio::Format format = Audio::Format::Unknown;
	const uint32_t format_param = param & 0xffu;

	switch (format_param)
	{
		case 0: format = Audio::Format::Signed16bitMono; break;
		case 1: format = Audio::Format::Signed16bitStereo; break;
		case 2: format = Audio::Format::Signed16bit8Ch; break;
		case 3: format = Audio::Format::FloatMono; break;
		case 4: format = Audio::Format::FloatStereo; break;
		case 5: format = Audio::Format::Float8Ch; break;
		case 6: format = Audio::Format::Signed16bit8ChStd; break;
		case 7: format = Audio::Format::Float8ChStd; break;
		default:;
	}

	printf("\t param   = %u, format = %u (%s)\n", param, format_param, Core::EnumName(format).C_Str());

	if (format == Audio::Format::Unknown)
	{
		return AUDIO_OUT_ERROR_INVALID_FORMAT;
	}
	if (attributes != 0)
	{
		AudioLog("[AudioOut] open attributes=0x%08x status=accepted\n", attributes);
	}

	EXIT_IF(g_audio == nullptr);

	auto id = g_audio->AudioOutOpen(type, len, freq, format);

	if (!id.IsValid())
	{
		return AUDIO_OUT_ERROR_PORT_FULL;
	}

	return id.ToInt();
}

int LUMEN_SYSV_ABI AudioOutClose(int handle)
{
	PRINT_NAME();

	if (!g_audio->AudioOutClose(Audio::Id(handle)))
	{
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}

	return OK;
}

int LUMEN_SYSV_ABI AudioOutGetPortState(int handle, AudioOutPortState* state)
{
	PRINT_NAME();

	if (state == nullptr)
	{
		return AUDIO_OUT_ERROR_INVALID_POINTER;
	}
	Core::VirtualMemory::GuestDataLease output;
	if (!BorrowAudioGuestRange(state, sizeof(*state), Core::VirtualMemory::Mode::Write, &output))
		return AUDIO_OUT_ERROR_INVALID_POINTER;

	std::memset(state, 0, sizeof(*state));

	int type         = 0;
	int channels_num = 0;

	if (!g_audio->AudioOutGetStatus(Audio::Id(handle), &type, &channels_num, nullptr))
	{
		return AUDIO_OUT_ERROR_INVALID_PORT;
	}

	state->reroute_counter = 0;
	state->volume          = -1;

	switch (type)
	{
		case 0:
		case 1:
			state->output  = 1;
			state->channel = (channels_num > 2 ? 2 : channels_num);
			break;
		case 2:
		case 3:
			state->output  = 0x40;
			state->channel = 1;
			break;
		case 127:
			state->output  = 0x80;
			state->channel = 0;
			break;
		case 4:
			state->output  = 4;
			state->channel = 1;
			state->volume  = 127;
			break;
		default: EXIT("unknown port type: %d\n", type);
	}

	printf("\t output  = %" PRIu16 "\n", state->output);
	printf("\t channel = %" PRIu8 "\n", state->channel);

	return OK;
}

int LUMEN_SYSV_ABI AudioOutSetVolume(int handle, uint32_t flag, int* vol)
{
	PRINT_NAME();

	printf("\t handle = %d\n", handle);
	printf("\t flag   = %u\n", flag);

	EXIT_IF(g_audio == nullptr);
	if (vol == nullptr)
	{
		return AUDIO_OUT_ERROR_INVALID_POINTER;
	}
	if ((flag & ~0xffu) != 0)
	{
		return AUDIO_OUT_ERROR_INVALID_FLAG;
	}
	return g_audio->AudioOutSetVolume(Audio::Id(handle), flag, vol);
}

int LUMEN_SYSV_ABI AudioOutOutputs(AudioOutOutputParam* param, uint32_t num)
{
	PRINT_NAME();

	if (param == nullptr)
	{
		return AUDIO_OUT_ERROR_INVALID_POINTER;
	}
	if (num == 0 || num > 25)
	{
		return AUDIO_OUT_ERROR_PORT_FULL;
	}
	std::array<AudioOutOutputParam, 25> descriptor {};
	{
		Core::VirtualMemory::GuestDataLease input;
		if (!BorrowAudioGuestRange(param, sizeof(*param) * num, Core::VirtualMemory::Mode::Read, &input))
			return AUDIO_OUT_ERROR_INVALID_POINTER;
		std::memcpy(descriptor.data(), param, sizeof(*param) * num);
	}
	param = descriptor.data();

	for (uint32_t i = 0; i < num; i++)
	{
		printf("\t handle[%u] = %d\n", i, param[i].handle);
	}

	Audio::OutputParam params[Audio::OUT_PORTS_MAX];

	EXIT_IF(g_audio == nullptr);

	for (uint32_t i = 0; i < num; i++)
	{
		params[i].handle = Audio::Id(param[i].handle);
		params[i].data   = param[i].ptr;
	}

	return g_audio->AudioOutOutputs(params, num);
}

int LUMEN_SYSV_ABI AudioOutOutput(int handle, const void* ptr)
{
	PRINT_NAME();

	printf("\t handle = %d\n", handle);

	// EXIT_NOT_IMPLEMENTED(ptr == nullptr);

	Audio::OutputParam params[1];

	EXIT_IF(g_audio == nullptr);

	params[0].handle = Audio::Id(handle);
	params[0].data   = ptr;

	int       channels_num = 0;
	const int result       = g_audio->AudioOutOutputs(params, 1, &channels_num);
	if (result < 0)
	{
		return result;
	}
	return (ptr == nullptr ? 0 : result * channels_num);
}

} // namespace AudioOut

namespace AudioIn {

LIB_NAME("AudioIn", "AudioIn");

int LUMEN_SYSV_ABI AudioInOpen(int user_id, uint32_t type, uint32_t index, uint32_t len, uint32_t freq, uint32_t param)
{
	PRINT_NAME();

	printf("\t user_id = %d\n", user_id);
	printf("\t type    = %u\n", type);
	printf("\t index   = %d\n", index);
	printf("\t len     = %u\n", len);
	printf("\t freq    = %u\n", freq);

	EXIT_NOT_IMPLEMENTED(user_id != 255 && user_id != 1);
	EXIT_NOT_IMPLEMENTED(type != 1);
	EXIT_NOT_IMPLEMENTED(index != 0);

	Audio::Format format = Audio::Format::Unknown;

	switch (param)
	{
		case 0: format = Audio::Format::Signed16bitMono; break;
		case 2: format = Audio::Format::Signed16bitStereo; break;
		default:;
	}

	printf("\t param   = %u (%s)\n", param, Core::EnumName(format).C_Str());

	EXIT_NOT_IMPLEMENTED(format == Audio::Format::Unknown);

	EXIT_IF(g_audio == nullptr);

	auto id = g_audio->AudioInOpen(type, len, freq, format);

	if (!id.IsValid())
	{
		return AUDIO_IN_ERROR_PORT_FULL;
	}

	return id.ToInt();
}

int LUMEN_SYSV_ABI AudioInInput(int handle, void* dest)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(dest == nullptr);

	EXIT_IF(g_audio == nullptr);

	if (!g_audio->AudioInValid(Audio::Id(handle)))
	{
		return AUDIO_IN_ERROR_INVALID_HANDLE;
	}

	return static_cast<int>(g_audio->AudioInInput(Audio::Id(handle), dest));
}

} // namespace AudioIn

namespace VoiceQoS {

LIB_NAME("VoiceQoS", "VoiceQoS");

int LUMEN_SYSV_ABI VoiceQoSInit(void* mem_block, uint32_t mem_size, int32_t app_type)
{
	PRINT_NAME();

	printf("\t mem_block = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(mem_block));
	printf("\t mem_size = %" PRIu32 "\n", mem_size);
	printf("\t app_type = %" PRId32 "\n", app_type);

	return OK;
}

} // namespace VoiceQoS

namespace AvPlayer {

LIB_NAME("AvPlayer", "AvPlayer");

using AvPlayerAllocate          = LUMEN_SYSV_ABI void* (*)(void*, uint32_t, uint32_t);
using AvPlayerDeallocate        = LUMEN_SYSV_ABI void (*)(void*, void*);
using AvPlayerAllocateTexture   = LUMEN_SYSV_ABI void* (*)(void*, uint32_t, uint32_t);
using AvPlayerDeallocateTexture = LUMEN_SYSV_ABI void (*)(void*, void*);
using AvPlayerOpenFile          = LUMEN_SYSV_ABI int (*)(void*, const char*);
using AvPlayerCloseFile         = LUMEN_SYSV_ABI int (*)(void*);
using AvPlayerReadOffsetFile    = LUMEN_SYSV_ABI int (*)(void*, uint8_t*, uint64_t, uint32_t);
using AvPlayerSizeFile          = LUMEN_SYSV_ABI uint64_t (*)(void*);
using AvPlayerEventCallback     = LUMEN_SYSV_ABI void (*)(void*, int32_t, int32_t, void*);

struct AvPlayerMemAllocator
{
	void*                     object_pointer     = nullptr;
	AvPlayerAllocate          allocate           = nullptr;
	AvPlayerDeallocate        deallocate         = nullptr;
	AvPlayerAllocateTexture   allocate_texture   = nullptr;
	AvPlayerDeallocateTexture deallocate_texture = nullptr;
};

struct AvPlayerFileReplacement
{
	void*                  object_pointer = nullptr;
	AvPlayerOpenFile       open           = nullptr;
	AvPlayerCloseFile      close          = nullptr;
	AvPlayerReadOffsetFile read_offset    = nullptr;
	AvPlayerSizeFile       size           = nullptr;
};

struct AvPlayerEventReplacement
{
	void*                 object_pointer = nullptr;
	AvPlayerEventCallback event_callback = nullptr;
};

enum class AvPlayerDebuglevels : int
{
	AvplayerDbgNone = 0,
	AvplayerDbgInfo = 1,
	AvplayerDbgWarnings = 2,
	AvplayerDbgAll = 3
};

struct AvPlayerInitData
{
	AvPlayerMemAllocator     memory_replacement;
	AvPlayerFileReplacement  file_replacement;
	AvPlayerEventReplacement event_replacement;
	AvPlayerDebuglevels      debug_level                   = AvPlayerDebuglevels::AvplayerDbgNone;
	uint32_t                 base_priority                 = 0;
	int32_t                  num_output_video_framebuffers = 0;
	Bool                     auto_start                    = 0;
	uint8_t                  reserved[3]                   = {};
	const char*              default_language              = nullptr;
};

struct AvPlayerAudioEx
{
	uint16_t channel_count;
	uint8_t  reserved[2];
	uint32_t sample_rate;
	uint32_t size;
	uint8_t  language_code[4];
	uint8_t  reserved1[64];
};

struct AvPlayerVideoEx
{
	uint32_t width;
	uint32_t height;
	float    aspect_ratio;
	uint8_t  language_code[4];
	uint32_t framerate;
	uint32_t crop_left_offset;
	uint32_t crop_right_offset;
	uint32_t crop_top_offset;
	uint32_t crop_bottom_offset;
	uint32_t pitch;
	uint8_t  luma_bit_depth;
	uint8_t  chroma_bit_depth;
	Bool     video_full_tange_flag;
	uint8_t  reserved1[37];
};

struct AvPlayerTimedTextEx
{
	uint8_t language_code[4];
	uint8_t reserved[12];
	uint8_t reserved1[64];
};

union AvPlayerStreamDetailsEx
{
	AvPlayerAudioEx     audio;
	AvPlayerVideoEx     video;
	AvPlayerTimedTextEx subs;
	uint8_t             reserved1[80];
};

struct AvPlayerFrameInfoEx
{
	void*                   data;
	uint8_t                 reserved[4];
	uint64_t                time_stamp;
	AvPlayerStreamDetailsEx details;
};

struct AvPlayerAudio
{
	uint16_t channel_count;
	uint8_t  reserved[2];
	uint32_t sample_rate;
	uint32_t size;
	uint8_t  language_code[4];
};

union AvPlayerStreamDetails
{
	AvPlayerAudio audio;
	uint8_t       reserved[16];
};

struct AvPlayerFrameInfo
{
	void*                 data;
	uint8_t               reserved[4];
	uint64_t              time_stamp;
	AvPlayerStreamDetails details;
};

static_assert(sizeof(AvPlayerAudio) == 16);
static_assert(sizeof(AvPlayerStreamDetails) == 16);
static_assert(sizeof(AvPlayerFrameInfo) == 40);
static_assert(offsetof(AvPlayerFrameInfo, data) == 0);
static_assert(offsetof(AvPlayerFrameInfo, time_stamp) == 16);
static_assert(offsetof(AvPlayerFrameInfo, details) == 24);

struct AvPlayerInternal
{
	String               filename;
	bool                 loop = false;
	AvPlayerMemAllocator mem;
	AvPlayerFileReplacement file;
	Core::Mutex          mutex;
	bool                 closing = false;
	uint64_t             buffer_epoch = 0;
	uint32_t             buffer_borrows = 0;

	// Real video decoding state (Windows Media Foundation)
	bool                 mf_initialized    = false;
#ifdef _WIN32
	IMFSourceReader*     mf_reader         = nullptr;
	wchar_t              mf_temp_path[MAX_PATH] = {};
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	struct MacAudioBuffer
	{
		void*    data     = nullptr;
		uint32_t capacity = 0;
	};

	Mac::Decoder*               mac_decoder = nullptr;
	std::vector<void*>          mac_video_frames;
	std::vector<MacAudioBuffer> mac_audio_frames;
	uint32_t                    mac_video_framebuffer_count = 2;
	size_t                      mac_next_video_frame        = 0;
	size_t                      mac_next_audio_frame        = 0;
	uint16_t                    mac_audio_channel_count     = 0;
	uint32_t                    mac_audio_sample_rate       = 0;
	bool                        mac_audio_allocation_failed = false;
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	struct AndroidAudioBuffer
	{
		void*    data     = nullptr;
		uint32_t capacity = 0;
	};

	Android::Decoder*               android_decoder = nullptr;
	std::vector<void*>              android_video_frames;
	std::vector<AndroidAudioBuffer> android_audio_frames;
	uint32_t                        android_video_framebuffer_count = 2;
	size_t                          android_next_video_frame        = 0;
	size_t                          android_next_audio_frame        = 0;
	uint16_t                        android_audio_channel_count     = 0;
	uint32_t                        android_audio_sample_rate       = 0;
	bool                            android_audio_allocation_failed = false;
#endif
	uint32_t             video_width       = 0;
	uint32_t             video_height      = 0;
	float                video_frame_rate  = 30.0f;
	uint32_t             video_pitch       = 0;
	void*                video_frame       = nullptr;  // NV12 frame buffer allocated via game allocator
	uint32_t             video_frame_size  = 0;
	bool                 video_eof         = false;
	uint64_t             frame_timestamp   = 0;        // in milliseconds
	uint32_t             frame_count       = 0;

};

using AvPlayerHandle = std::shared_ptr<AvPlayerInternal>;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || \
    LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
static bool avplayer_borrow_buffer(AvPlayerInternal* player, void* destination, uint64_t bytes,
                                   Core::VirtualMemory::GuestDataLease* lease)
{
	const auto epoch = player->buffer_epoch;
	++player->buffer_borrows;
	// The public operation retains a shared player owner. Completion and VM
	// admission run without the player mutex; callers revalidate their selected
	// buffer after the mutex is reacquired and before touching its contents.
	struct Unlocked
	{
		Core::Mutex& mutex;
		explicit Unlocked(Core::Mutex& value): mutex(value) { mutex.Unlock(); }
		~Unlocked() { mutex.Lock(); }
	};
	bool acquired = false;
	{
		Unlocked unlocked(player->mutex);
		acquired = BorrowAudioGuestRange(destination, bytes, Core::VirtualMemory::Mode::Write, lease);
	}
	--player->buffer_borrows;
	return acquired && !player->closing && player->buffer_epoch == epoch;
}

static bool avplayer_initialize_buffer(AvPlayerInternal* player, void* destination, uint64_t bytes)
{
	Core::VirtualMemory::GuestDataLease output;
	if (!avplayer_borrow_buffer(player, destination, bytes, &output)) return false;
	std::memset(destination, 0, static_cast<size_t>(bytes));
	return true;
}
#endif

static std::mutex& avplayer_registry_mutex()
{
	static std::mutex mutex;
	return mutex;
}

static std::unordered_map<AvPlayerInternal*, AvPlayerHandle>& avplayer_registry()
{
	static std::unordered_map<AvPlayerInternal*, AvPlayerHandle> registry;
	return registry;
}

static bool avplayer_register(const AvPlayerHandle& player)
{
	if (player == nullptr)
	{
		return false;
	}
	std::lock_guard lock(avplayer_registry_mutex());
	return avplayer_registry().emplace(player.get(), player).second;
}

static AvPlayerHandle avplayer_acquire(AvPlayerInternal* player)
{
	if (player == nullptr)
	{
		return {};
	}
	std::lock_guard lock(avplayer_registry_mutex());
	const auto it = avplayer_registry().find(player);
	return it == avplayer_registry().end() ? AvPlayerHandle {} : it->second;
}

static AvPlayerHandle avplayer_remove(AvPlayerInternal* player)
{
	if (player == nullptr)
	{
		return {};
	}
	std::lock_guard lock(avplayer_registry_mutex());
	auto&      registry = avplayer_registry();
	const auto it       = registry.find(player);
	if (it == registry.end())
	{
		return {};
	}
	auto handle = std::move(it->second);
	registry.erase(it);
	return handle;
}

static std::vector<AvPlayerHandle> avplayer_remove_all()
{
	std::vector<AvPlayerHandle> players;
	std::lock_guard lock(avplayer_registry_mutex());
	auto& registry = avplayer_registry();
	players.reserve(registry.size());
	for (auto& entry: registry)
	{
		players.emplace_back(std::move(entry.second));
	}
	registry.clear();
	return players;
}

#if defined(LUMEN_AVPLAYER_LIFETIME_TEST_HOOK)
static std::atomic<bool> g_avplayer_test_hold_audio_read {false};
static std::atomic<bool> g_avplayer_test_audio_read_entered {false};
static std::atomic<bool> g_avplayer_test_release_audio_read {false};
static std::atomic<bool> g_avplayer_test_close_observed {false};
static std::atomic<bool> g_avplayer_test_close_lock_attempted {false};
static std::atomic<bool> g_avplayer_test_close_cleanup_started {false};
#endif

// =====================================================================
// Media Foundation video decoder
// =====================================================================

#ifdef _WIN32
static bool g_mf_startup_done = false;

static FILE* open_diagnostics_file(const char* filename)
{
	return Config::DiagnosticsEnabled() ? fopen(filename, "a") : nullptr;
}

static void mf_ensure_startup()
{
	if (!g_mf_startup_done)
	{
		HRESULT hr = MFStartup(MF_VERSION);
		if (SUCCEEDED(hr))
		{
			g_mf_startup_done = true;
		}
		else
		{
			printf("[AvPlayer] MFStartup failed: 0x%08x\n", static_cast<unsigned>(hr));
		}
	}
}

static bool mf_materialize_callback_stream(AvPlayerInternal* r, const char* filename)
{
	if (r->file.open == nullptr || r->file.close == nullptr || r->file.read_offset == nullptr || r->file.size == nullptr)
	{
		return false;
	}

	int open_result = r->file.open(r->file.object_pointer, filename);
	if (FILE* probe = open_diagnostics_file("_avplayer_file_probe.log"); probe != nullptr)
	{
		fprintf(probe, "open=%d object=%p path=%s\n", open_result, r->file.object_pointer, filename);
		fclose(probe);
	}
	if (open_result < 0)
	{
		printf("[AvPlayer] Stream open failed: %d\n", open_result);
		return false;
	}

	uint64_t stream_size = r->file.size(r->file.object_pointer);
	if (FILE* probe = open_diagnostics_file("_avplayer_file_probe.log"); probe != nullptr)
	{
		fprintf(probe, "size=%" PRIu64 "\n", stream_size);
		fclose(probe);
	}
	if (stream_size == 0)
	{
		printf("[AvPlayer] Stream has an invalid size\n");
		r->file.close(r->file.object_pointer);
		return false;
	}

	wchar_t temp_dir[MAX_PATH] = {};
	if (GetTempPathW(MAX_PATH, temp_dir) == 0 || GetTempFileNameW(temp_dir, L"kyt", 0, r->mf_temp_path) == 0)
	{
		printf("[AvPlayer] Unable to create a temporary stream path\n");
		r->file.close(r->file.object_pointer);
		return false;
	}

	HANDLE output = CreateFileW(r->mf_temp_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
	if (output == INVALID_HANDLE_VALUE)
	{
		printf("[AvPlayer] Unable to create the temporary stream file\n");
		r->file.close(r->file.object_pointer);
		DeleteFileW(r->mf_temp_path);
		r->mf_temp_path[0] = L'\0';
		return false;
	}

	constexpr uint32_t chunk_size = 1024 * 1024;
	auto*              chunk      = new uint8_t[chunk_size];
	uint64_t           offset     = 0;
	bool               ok         = true;

	while (offset < stream_size)
	{
		uint32_t request = static_cast<uint32_t>(std::min<uint64_t>(chunk_size, stream_size - offset));
		int      read    = r->file.read_offset(r->file.object_pointer, chunk, offset, request);
		if (read <= 0 || static_cast<uint32_t>(read) > request)
		{
			printf("[AvPlayer] Stream read failed at offset %" PRIu64 ": %d\n", offset, read);
			ok = false;
			break;
		}

		DWORD written = 0;
		if (WriteFile(output, chunk, static_cast<DWORD>(read), &written, nullptr) == 0 || written != static_cast<DWORD>(read))
		{
			printf("[AvPlayer] Temporary stream write failed at offset %" PRIu64 "\n", offset);
			ok = false;
			break;
		}
		offset += static_cast<uint32_t>(read);
	}

	delete[] chunk;
	CloseHandle(output);
	int close_result = r->file.close(r->file.object_pointer);
	if (close_result < 0)
	{
		printf("[AvPlayer] Stream close failed: %d\n", close_result);
	}

	if (!ok || offset != stream_size)
	{
		DeleteFileW(r->mf_temp_path);
		r->mf_temp_path[0] = L'\0';
		return false;
	}

	printf("[AvPlayer] Materialized callback stream: %" PRIu64 " bytes\n", stream_size);
	return true;
}

static uint64_t find_native_fios_export(const char* nid)
{
	auto* linker = Loader::RuntimeLinker::Instance();
	if (linker == nullptr)
	{
		return 0;
	}
	Loader::SymbolResolve symbol {};
	symbol.name                 = String::FromUtf8(nid);
	symbol.library              = U"Fios2";
	symbol.library_version      = 1;
	symbol.module               = U"Fios2";
	symbol.module_version_major = 1;
	symbol.module_version_minor = 1;
	symbol.type                 = Loader::SymbolType::Func;
	return linker->FindExport(symbol);
}

static bool mf_materialize_fios_stream(AvPlayerInternal* r, const char* filename)
{
	using FiosFilterCallback = LUMEN_SYSV_ABI int (*)(void*, int, void*);
	using FiosFilterGetInfo  = LUMEN_SYSV_ABI int (*)(int, FiosFilterCallback*, void**);
	using FiosOpen         = LUMEN_SYSV_ABI int (*)(void*, int32_t*, const char*, void*);
	using FiosFilteredOpen = LUMEN_SYSV_ABI int (*)(void*, int32_t*, const char*, void*, int);
	using FiosSize         = LUMEN_SYSV_ABI int64_t (*)(int32_t);
	using FiosPread        = LUMEN_SYSV_ABI int64_t (*)(void*, int32_t, void*, int64_t, int64_t);
	using FiosClose        = LUMEN_SYSV_ABI int (*)(void*, int32_t);
	using FiosErrorString  = LUMEN_SYSV_ABI const char* (*)(int, char*, uint32_t);

	auto open  = reinterpret_cast<FiosOpen>(find_native_fios_export("b44anV2D7K0"));
	if (open == nullptr)
	{
		open = reinterpret_cast<FiosOpen>(find_native_fios_export("Bi1MyH3ConE"));
	}
	auto filtered_open = reinterpret_cast<FiosFilteredOpen>(find_native_fios_export("w13Ojm7ON9o"));
	auto size  = reinterpret_cast<FiosSize>(find_native_fios_export("FdjoqFQOlt0"));
	auto pread = reinterpret_cast<FiosPread>(find_native_fios_export("2m9+Opco-hk"));
	auto close = reinterpret_cast<FiosClose>(find_native_fios_export("ZzE6WN+QBbE"));
	auto error_string = reinterpret_cast<FiosErrorString>(find_native_fios_export("8BLW2WiSIuI"));
	if (FILE* probe = open_diagnostics_file("_avplayer_fios_probe.log"); probe != nullptr)
	{
		fprintf(probe, "exports open=%p size=%p pread=%p close=%p\n", reinterpret_cast<void*>(open), reinterpret_cast<void*>(size),
		        reinterpret_cast<void*>(pread), reinterpret_cast<void*>(close));
		fclose(probe);
	}
	int active_filters[256] = {};
	int active_filter_count = 0;
	if (auto get_info = reinterpret_cast<FiosFilterGetInfo>(find_native_fios_export("gHxxOQxKW3E")); get_info != nullptr)
	{
		FILE* probe = open_diagnostics_file("_avplayer_fios_probe.log");
		for (int index = 0; index < 256; index++)
		{
			FiosFilterCallback callback = nullptr;
			void*              context  = nullptr;
			const int          result   = get_info(index, &callback, &context);
			if (result >= 0 && callback != nullptr)
			{
				uint16_t filter_id = static_cast<uint16_t>(index);
				const int select_result = callback(context, -1, &filter_id);
				if (select_result >= 0)
				{
					active_filters[active_filter_count++] = filter_id;
				}
				if (probe != nullptr)
				{
					fprintf(probe, "filter index=%d id=%u select=%d callback=%p context=%p\n", index, filter_id, select_result,
					        reinterpret_cast<void*>(callback), context);
				}
			}
		}
		if (probe != nullptr)
		{
			fclose(probe);
		}
	}
	if (open == nullptr || size == nullptr || pread == nullptr || close == nullptr)
	{
		return false;
	}

	int32_t handle      = -1;
	int     open_result = open(nullptr, &handle, filename, nullptr);
	if (open_result < 0 && filtered_open != nullptr)
	{
		for (int i = 0; i < active_filter_count && open_result < 0; i++)
		{
			handle      = -1;
			open_result = filtered_open(nullptr, &handle, filename, nullptr, active_filters[i]);
		}
	}
	String  clean_path;
	if (open_result < 0 && filename[0] == '/' && filename[1] == '/')
	{
		clean_path  = String::FromUtf8(filename + 1);
		handle      = -1;
		open_result = open(nullptr, &handle, clean_path.C_Str(), nullptr);
		if (open_result < 0 && filtered_open != nullptr)
		{
			for (int i = 0; i < active_filter_count && open_result < 0; i++)
			{
				handle      = -1;
				open_result = filtered_open(nullptr, &handle, clean_path.C_Str(), nullptr, active_filters[i]);
			}
		}
	}
	if (FILE* probe = open_diagnostics_file("_avplayer_fios_probe.log"); probe != nullptr)
	{
		char error_buffer[128] = {};
		const char* error_name = (error_string != nullptr ? error_string(open_result, error_buffer, sizeof(error_buffer)) : nullptr);
		fprintf(probe, "open=%d (%s) handle=%d path=%s\n", open_result, error_name != nullptr ? error_name : "unknown", handle,
		        clean_path.IsEmpty() ? filename : clean_path.C_Str());
		fclose(probe);
	}
	if (open_result < 0 || handle < 0)
	{
		return false;
	}

	const int64_t stream_size = size(handle);
	if (FILE* probe = open_diagnostics_file("_avplayer_fios_probe.log"); probe != nullptr)
	{
		fprintf(probe, "size=%" PRId64 "\n", stream_size);
		fclose(probe);
	}
	if (stream_size <= 0)
	{
		close(nullptr, handle);
		return false;
	}

	wchar_t temp_dir[MAX_PATH] = {};
	if (GetTempPathW(MAX_PATH, temp_dir) == 0 || GetTempFileNameW(temp_dir, L"kyt", 0, r->mf_temp_path) == 0)
	{
		close(nullptr, handle);
		return false;
	}

	HANDLE output = CreateFileW(r->mf_temp_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
	if (output == INVALID_HANDLE_VALUE)
	{
		close(nullptr, handle);
		DeleteFileW(r->mf_temp_path);
		r->mf_temp_path[0] = L'\0';
		return false;
	}

	constexpr int64_t chunk_size = 1024 * 1024;
	auto*             chunk      = new uint8_t[chunk_size];
	int64_t           offset     = 0;
	bool              ok         = true;
	while (offset < stream_size)
	{
		const int64_t request = (std::min)(chunk_size, stream_size - offset);
		const int64_t read    = pread(nullptr, handle, chunk, request, offset);
		if (read <= 0 || read > request)
		{
			ok = false;
			break;
		}
		DWORD written = 0;
		if (WriteFile(output, chunk, static_cast<DWORD>(read), &written, nullptr) == 0 || written != static_cast<DWORD>(read))
		{
			ok = false;
			break;
		}
		offset += read;
	}

	delete[] chunk;
	CloseHandle(output);
	close(nullptr, handle);
	if (!ok || offset != stream_size)
	{
		DeleteFileW(r->mf_temp_path);
		r->mf_temp_path[0] = L'\0';
		return false;
	}
	printf("[AvPlayer] Materialized FIOS stream: %" PRId64 " bytes\n", stream_size);
	return true;
}

// Open a video file with MF Source Reader, configure NV12 output
static bool mf_open_video(AvPlayerInternal* r, const wchar_t* path)
{
	mf_ensure_startup();
	if (!g_mf_startup_done) return false;

	HRESULT hr;

	// Create source reader
	IMFSourceReader* reader = nullptr;
	hr = MFCreateSourceReaderFromURL(path, nullptr, &reader);
	if (FAILED(hr) || reader == nullptr)
	{
		printf("[AvPlayer] MFCreateSourceReaderFromURL failed: 0x%08x\n", static_cast<unsigned>(hr));
		return false;
	}

	// Select only the first video stream, deselect everything else
	reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
	reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

	// Get native media type to discover dimensions
	IMFMediaType* native_type = nullptr;
	hr = reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native_type);
	if (FAILED(hr))
	{
		printf("[AvPlayer] GetNativeMediaType failed: 0x%08x\n", static_cast<unsigned>(hr));
		reader->Release();
		return false;
	}

	// Get frame size
	uint64_t frame_size = 0;
	hr = native_type->GetUINT64(MF_MT_FRAME_SIZE, &frame_size);
	uint32_t width  = (frame_size >> 32) & 0xFFFFFFFF;
	uint32_t height = frame_size & 0xFFFFFFFF;

	// Get frame rate
	uint64_t frame_rate_packed = 0;
	native_type->GetUINT64(MF_MT_FRAME_RATE, &frame_rate_packed);
	uint32_t fps_num = (frame_rate_packed >> 32) & 0xFFFFFFFF;
	uint32_t fps_den = frame_rate_packed & 0xFFFFFFFF;
	float    fps     = (fps_den > 0) ? (static_cast<float>(fps_num) / static_cast<float>(fps_den)) : 30.0f;

	native_type->Release();

	printf("[AvPlayer] Video: %ux%u @ %.2f fps\n", width, height, static_cast<double>(fps));

	// Configure output to NV12
	IMFMediaType* output_type = nullptr;
	hr = MFCreateMediaType(&output_type);
	if (FAILED(hr))
	{
		reader->Release();
		return false;
	}

	output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
	MFSetAttributeSize(output_type, MF_MT_FRAME_SIZE, width, height);

	hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, output_type);
	output_type->Release();

	if (FAILED(hr))
	{
		printf("[AvPlayer] SetCurrentMediaType(NV12) failed: 0x%08x, trying YUY2\n", static_cast<unsigned>(hr));
		// Some decoders don't support NV12 directly. Try YUY2 and we'll convert later.
		// Actually, for now just fall back to fake
		reader->Release();
		return false;
	}

	// Allocate frame buffer via game's allocator
	// NV12: Y plane = width*height, UV plane = width*height/2
	uint32_t frame_size_bytes = width * height + width * height / 2;
	void*    frame_buf = r->mem.allocate_texture(r->mem.object_pointer, 256, frame_size_bytes);
	if (frame_buf == nullptr)
	{
		printf("[AvPlayer] Failed to allocate frame buffer (%u bytes)\n", frame_size_bytes);
		reader->Release();
		return false;
	}
	memset(frame_buf, 0, frame_size_bytes);

	r->mf_reader         = reader;
	r->mf_initialized    = true;
	r->video_width       = width;
	r->video_height      = height;
	r->video_frame_rate  = fps;
	r->video_pitch       = width;  // NV12 pitch = width for luma
	r->video_frame       = frame_buf;
	r->video_frame_size  = frame_size_bytes;
	r->video_eof         = false;
	r->frame_timestamp   = 0;
	r->frame_count       = 0;

	printf("[AvPlayer] MF video decoder initialized successfully\n");
	return true;
}

// Read the next video frame from MF, copy NV12 data to game buffer
static bool mf_read_frame(AvPlayerInternal* r, AvPlayerFrameInfoEx* info)
{
	if (!r->mf_initialized || r->mf_reader == nullptr || r->video_eof)
		return false;

	DWORD        flags     = 0;
	LONGLONG     timestamp = 0;
	IMFSample*   sample    = nullptr;

	HRESULT hr = r->mf_reader->ReadSample(
		static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
		0,        // no flags
		nullptr,  // actual stream index (don't care)
		&flags,
		&timestamp,
		&sample
	);

	if (FAILED(hr))
	{
		printf("[AvPlayer] ReadSample failed: 0x%08x\n", static_cast<unsigned>(hr));
		r->video_eof = true;
		return false;
	}

	if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
	{
		r->video_eof = true;
		if (sample) sample->Release();
		return false;
	}

	if (sample == nullptr)
	{
		// Gap in stream, try again next call
		return false;
	}

	// Get buffer from sample
	IMFMediaBuffer* buffer = nullptr;
	hr = sample->ConvertToContiguousBuffer(&buffer);
	if (FAILED(hr) || buffer == nullptr)
	{
		sample->Release();
		return false;
	}

	BYTE*  src_data = nullptr;
	DWORD  src_len  = 0;
	hr = buffer->Lock(&src_data, nullptr, &src_len);
	if (SUCCEEDED(hr) && src_data != nullptr)
	{
		// Copy NV12 data to game buffer
		// MF may output with stride != width, need to handle that
		// For NV12: first width*height bytes are Y, then width*height/2 bytes are interleaved UV

		uint32_t expected_size = r->video_width * r->video_height + r->video_width * r->video_height / 2;
		uint32_t copy_size = (src_len < expected_size) ? src_len : expected_size;
		if (copy_size > r->video_frame_size)
			copy_size = r->video_frame_size;

		// Check if MF stride matches our expected width
		// MF may pad rows for alignment. Try to detect and handle stride mismatch.
		// For most H.264 decoders NV12, stride = width or next multiple of 16/32
		uint32_t stride = r->video_width;
		uint32_t expected_simple = stride * r->video_height * 3 / 2;

		if (src_len >= expected_simple)
		{
			// Simple case: stride == width, direct copy
			memcpy(r->video_frame, src_data, copy_size);
		}
		else if (src_len > 0)
		{
			// MF might have different stride; try row-by-row copy
			// Estimate stride from total data size
			// NV12 total = stride * height * 3 / 2
			uint32_t mf_stride = static_cast<uint32_t>(src_len * 2 / (3 * r->video_height));
			if (mf_stride < r->video_width) mf_stride = r->video_width;

			auto* dst = static_cast<uint8_t*>(r->video_frame);

			// Copy Y plane
			for (uint32_t row = 0; row < r->video_height && (row * mf_stride) < src_len; row++)
			{
				uint32_t row_bytes = (r->video_width < mf_stride) ? r->video_width : mf_stride;
				if ((row * mf_stride + row_bytes) > src_len) break;
				memcpy(dst + row * r->video_width, src_data + row * mf_stride, row_bytes);
			}

			// Copy UV plane
			uint32_t uv_offset_src = mf_stride * r->video_height;
			uint32_t uv_offset_dst = r->video_width * r->video_height;
			uint32_t uv_rows = r->video_height / 2;
			for (uint32_t row = 0; row < uv_rows && (uv_offset_src + row * mf_stride) < src_len; row++)
			{
				uint32_t row_bytes = (r->video_width < mf_stride) ? r->video_width : mf_stride;
				if ((uv_offset_src + row * mf_stride + row_bytes) > src_len) break;
				memcpy(dst + uv_offset_dst + row * r->video_width,
				       src_data + uv_offset_src + row * mf_stride, row_bytes);
			}
		}

		buffer->Unlock();
	}

	buffer->Release();
	sample->Release();

	// Fill out frame info for the game
	r->frame_timestamp = static_cast<uint64_t>(timestamp / 10000); // 100ns units -> ms

	info->data       = r->video_frame;
	info->time_stamp = r->frame_timestamp;

	info->details.video.width                 = r->video_width;
	info->details.video.height                = r->video_height;
	info->details.video.aspect_ratio          = static_cast<float>(r->video_width) / static_cast<float>(r->video_height);
	info->details.video.language_code[0]      = 'e';
	info->details.video.language_code[1]      = 'n';
	info->details.video.language_code[2]      = 'g';
	info->details.video.language_code[3]      = '\0';
	info->details.video.framerate             = 0;
	info->details.video.crop_left_offset      = 0;
	info->details.video.crop_right_offset     = 0;
	info->details.video.crop_top_offset       = 0;
	info->details.video.crop_bottom_offset    = 0;
	info->details.video.pitch                 = r->video_pitch;
	info->details.video.luma_bit_depth        = 8;
	info->details.video.chroma_bit_depth      = 8;
	info->details.video.video_full_tange_flag = 0;

	r->frame_count++;
	return true;
}

static void mf_close(AvPlayerInternal* r)
{
	if (r->mf_reader != nullptr)
	{
		r->mf_reader->Release();
		r->mf_reader = nullptr;
	}
	if (r->video_frame != nullptr)
	{
		r->mem.deallocate_texture(r->mem.object_pointer, r->video_frame);
		r->video_frame = nullptr;
	}
	r->mf_initialized = false;
	r->video_eof      = true;
	if (r->mf_temp_path[0] != L'\0')
	{
		DeleteFileW(r->mf_temp_path);
		r->mf_temp_path[0] = L'\0';
	}
}
#endif // _WIN32

	#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
static constexpr size_t MAC_AUDIO_BUFFER_COUNT = 4;

static void* mac_allocate_texture(AvPlayerInternal* player, uint32_t alignment, uint32_t size)
{
	if (player->mem.allocate_texture == nullptr)
	{
		return nullptr;
	}
#if defined(__aarch64__)
	if (Loader::IsNativeHostCallTarget(reinterpret_cast<uint64_t>(player->mem.allocate_texture)))
	{
		return player->mem.allocate_texture(player->mem.object_pointer, alignment, size);
	}
	return reinterpret_cast<void*>(Loader::RunGuestX64(reinterpret_cast<uint64_t>(player->mem.allocate_texture),
	                                                   reinterpret_cast<uint64_t>(player->mem.object_pointer), alignment, size));
#else
	return player->mem.allocate_texture(player->mem.object_pointer, alignment, size);
#endif
}

static void* mac_allocate_audio(AvPlayerInternal* player, uint32_t alignment, uint32_t size)
{
	if (player->mem.allocate == nullptr)
	{
		return nullptr;
	}
#if defined(__aarch64__)
	if (Loader::IsNativeHostCallTarget(reinterpret_cast<uint64_t>(player->mem.allocate)))
	{
		return player->mem.allocate(player->mem.object_pointer, alignment, size);
	}
	return reinterpret_cast<void*>(Loader::RunGuestX64(reinterpret_cast<uint64_t>(player->mem.allocate),
	                                                   reinterpret_cast<uint64_t>(player->mem.object_pointer), alignment, size));
#else
	return player->mem.allocate(player->mem.object_pointer, alignment, size);
#endif
}

static void mac_deallocate_audio(AvPlayerInternal* player, void* buffer)
{
	if (buffer == nullptr || player->mem.deallocate == nullptr)
	{
		return;
	}
#if defined(__aarch64__)
	if (Loader::IsNativeHostCallTarget(reinterpret_cast<uint64_t>(player->mem.deallocate)))
	{
		player->mem.deallocate(player->mem.object_pointer, buffer);
	} else
	{
		Loader::RunGuestX64(reinterpret_cast<uint64_t>(player->mem.deallocate),
		                    reinterpret_cast<uint64_t>(player->mem.object_pointer), reinterpret_cast<uint64_t>(buffer));
	}
#else
	player->mem.deallocate(player->mem.object_pointer, buffer);
#endif
}

static void mac_deallocate_texture(AvPlayerInternal* player, void* frame)
{
	if (frame == nullptr || player->mem.deallocate_texture == nullptr) return;
#if defined(__aarch64__)
	if (Loader::IsNativeHostCallTarget(reinterpret_cast<uint64_t>(player->mem.deallocate_texture)))
	{
		player->mem.deallocate_texture(player->mem.object_pointer, frame);
	} else
	{
		Loader::RunGuestX64(reinterpret_cast<uint64_t>(player->mem.deallocate_texture),
		                    reinterpret_cast<uint64_t>(player->mem.object_pointer), reinterpret_cast<uint64_t>(frame));
	}
#else
	player->mem.deallocate_texture(player->mem.object_pointer, frame);
#endif
}

static void mac_deallocate_textures(AvPlayerInternal* player)
{
	for (void* frame: player->mac_video_frames) mac_deallocate_texture(player, frame);
	player->mac_video_frames.clear();
	player->mac_next_video_frame = 0;
	player->video_frame          = nullptr;
	player->video_frame_size     = 0;
}

static void mac_deallocate_audio_buffers(AvPlayerInternal* player)
{
	for (const auto& frame: player->mac_audio_frames)
	{
		mac_deallocate_audio(player, frame.data);
	}
	player->mac_audio_frames.clear();
	player->mac_next_audio_frame        = 0;
	player->mac_audio_channel_count     = 0;
	player->mac_audio_sample_rate       = 0;
	player->mac_audio_allocation_failed = false;
}

static bool mac_open_media(AvPlayerInternal* player, const char* filename)
{
	if (player->mac_decoder == nullptr)
	{
		return false;
	}
	++player->buffer_epoch;

	auto clean_path = String::FromUtf8(filename);
	while (clean_path.Size() >= 2 && clean_path.At(0) == U'/' && clean_path.At(1) == U'/')
	{
		clean_path = clean_path.Mid(1);
	}
	auto real_path = LibKernel::FileSystem::GetRealFilename(clean_path);
	printf("[AvPlayer] Resolved macOS path: %s\n", real_path.C_Str());

	mac_deallocate_textures(player);
	mac_deallocate_audio_buffers(player);
	player->video_eof = true;

	Mac::VideoFormat video_format {};
	Mac::AudioFormat audio_format {};
	if (!Mac::Open(player->mac_decoder, real_path.C_Str(), &video_format, &audio_format))
	{
		const auto error = Mac::GetLastError(player->mac_decoder);
		printf("[AvPlayer] AVFoundation open failed: %s\n", error.c_str());
		return false;
	}
	if (video_format.buffer_size == 0 || video_format.buffer_size > UINT32_MAX)
	{
		printf("[AvPlayer] AVFoundation frame buffer size is invalid: %zu\n", video_format.buffer_size);
		Mac::Stop(player->mac_decoder);
		return false;
	}
	if ((audio_format.channel_count == 0) != (audio_format.sample_rate == 0))
	{
		printf("[AvPlayer] AVFoundation audio format is incomplete: channels=%u rate=%u\n", audio_format.channel_count,
		       audio_format.sample_rate);
		Mac::Stop(player->mac_decoder);
		return false;
	}
	if (audio_format.channel_count != 0 && (player->mem.allocate == nullptr || player->mem.deallocate == nullptr))
	{
		printf("[AvPlayer] Guest audio allocator callbacks are unavailable\n");
		Mac::Stop(player->mac_decoder);
		return false;
	}

	player->mac_video_frames.reserve(player->mac_video_framebuffer_count);
	for (uint32_t index = 0; index < player->mac_video_framebuffer_count; index++)
	{
		void* frame = mac_allocate_texture(player, 256, static_cast<uint32_t>(video_format.buffer_size));
		if (frame == nullptr)
		{
			printf("[AvPlayer] Failed to allocate macOS frame buffer %u/%u (%zu bytes)\n", index + 1, player->mac_video_framebuffer_count,
			       video_format.buffer_size);
			mac_deallocate_textures(player);
			Mac::Stop(player->mac_decoder);
			return false;
		}
		if (!avplayer_initialize_buffer(player, frame, video_format.buffer_size))
		{
			mac_deallocate_texture(player, frame);
			return false;
		}
		player->mac_video_frames.push_back(frame);
	}

	player->video_width      = video_format.width;
	player->video_height     = video_format.height;
	player->video_frame_rate = video_format.frame_rate > 0.0f ? video_format.frame_rate : 30.0f;
	player->video_pitch      = video_format.pitch;
	player->video_frame_size = static_cast<uint32_t>(video_format.buffer_size);
	player->video_eof        = false;
	player->frame_timestamp  = 0;
	player->frame_count      = 0;
	if (audio_format.channel_count != 0)
	{
		player->mac_audio_channel_count = audio_format.channel_count;
		player->mac_audio_sample_rate   = audio_format.sample_rate;
		player->mac_audio_frames.resize(MAC_AUDIO_BUFFER_COUNT);
	}
	printf("[AvPlayer] AVFoundation video: %ux%u pitch=%u @ %.2f fps, buffers=%u\n", player->video_width, player->video_height,
	       player->video_pitch, static_cast<double>(player->video_frame_rate), player->mac_video_framebuffer_count);
	if (audio_format.channel_count != 0)
	{
		printf("[AvPlayer] AVFoundation audio: %u channels @ %u Hz, buffers=%zu\n", audio_format.channel_count,
		       audio_format.sample_rate, MAC_AUDIO_BUFFER_COUNT);
	}
	return true;
}

static bool mac_read_frame(AvPlayerInternal* player, AvPlayerFrameInfoEx* info)
{
	if (player->mac_decoder == nullptr || player->mac_video_frames.empty() || player->video_eof)
	{
		return false;
	}

	void*           destination = player->mac_video_frames[player->mac_next_video_frame];
	const auto index = player->mac_next_video_frame;
	const auto bytes = player->video_frame_size;
	Core::VirtualMemory::GuestDataLease output;
	if (!avplayer_borrow_buffer(player, destination, bytes, &output) ||
	    player->mac_next_video_frame != index || index >= player->mac_video_frames.size() ||
	    player->mac_video_frames[index] != destination || player->video_frame_size != bytes) return false;
	Mac::VideoFrame frame {};
	if (!Mac::ReadFrame(player->mac_decoder, destination, player->video_frame_size, &frame))
	{
		player->video_eof = !Mac::IsActive(player->mac_decoder);
		if (player->video_eof)
		{
			const auto error = Mac::GetLastError(player->mac_decoder);
			if (!error.empty())
			{
				printf("[AvPlayer] AVFoundation playback ended: %s\n", error.c_str());
			}
		}
		return false;
	}

	player->video_frame          = destination;
	player->mac_next_video_frame = (player->mac_next_video_frame + 1) % player->mac_video_frames.size();
	player->frame_timestamp      = frame.time_stamp;
	info->data                   = player->video_frame;
	info->time_stamp             = player->frame_timestamp;

	info->details.video.width                 = player->video_width;
	info->details.video.height                = player->video_height;
	info->details.video.aspect_ratio          = static_cast<float>(player->video_width) / static_cast<float>(player->video_height);
	info->details.video.language_code[0]      = 'e';
	info->details.video.language_code[1]      = 'n';
	info->details.video.language_code[2]      = 'g';
	info->details.video.language_code[3]      = '\0';
	info->details.video.framerate             = 0;
	info->details.video.crop_left_offset      = 0;
	info->details.video.crop_right_offset     = 0;
	info->details.video.crop_top_offset       = 0;
	info->details.video.crop_bottom_offset    = 0;
	info->details.video.pitch                 = player->video_pitch;
	info->details.video.luma_bit_depth        = 8;
	info->details.video.chroma_bit_depth      = 8;
	info->details.video.video_full_tange_flag = frame.full_range ? 1 : 0;

	player->frame_count++;
	return true;
}

static bool mac_read_audio_frame(AvPlayerInternal* player, AvPlayerFrameInfo* info)
{
	if (player->mac_decoder == nullptr || player->mac_audio_frames.empty() || player->mac_audio_channel_count == 0 ||
	    player->mac_audio_sample_rate == 0 || player->mac_audio_allocation_failed)
	{
		return false;
	}

	const size_t required_size = Mac::GetPendingAudioFrameSize(player->mac_decoder);
	if (required_size == 0)
	{
		return false;
	}
	if (required_size > UINT32_MAX || player->mem.allocate == nullptr || player->mem.deallocate == nullptr)
	{
		printf("[AvPlayer] Cannot allocate macOS audio frame (%zu bytes)\n", required_size);
		player->mac_audio_allocation_failed = true;
		Mac::Stop(player->mac_decoder);
		player->video_eof = true;
		return false;
	}

	const auto index = player->mac_next_audio_frame;
	auto& buffer = player->mac_audio_frames[index];
	if (buffer.capacity < required_size)
	{
		void* replacement = mac_allocate_audio(player, 16, static_cast<uint32_t>(required_size));
		if (replacement == nullptr)
		{
			printf("[AvPlayer] Guest audio allocation failed (%zu bytes)\n", required_size);
			player->mac_audio_allocation_failed = true;
			Mac::Stop(player->mac_decoder);
			player->video_eof = true;
			return false;
		}
		const auto previous = buffer.data;
		if (!avplayer_initialize_buffer(player, replacement, required_size) ||
		    player->mac_next_audio_frame != index || index >= player->mac_audio_frames.size() ||
		    player->mac_audio_frames[index].data != previous)
		{
			mac_deallocate_audio(player, replacement);
			return false;
		}
		mac_deallocate_audio(player, buffer.data);
		buffer.data     = replacement;
		buffer.capacity = static_cast<uint32_t>(required_size);
	}

	Mac::AudioFrame frame {};
	void* destination = buffer.data;
	const auto capacity = buffer.capacity;
	Core::VirtualMemory::GuestDataLease output;
	if (!avplayer_borrow_buffer(player, destination, required_size, &output) ||
	    player->mac_next_audio_frame != index || index >= player->mac_audio_frames.size() ||
	    player->mac_audio_frames[index].data != destination ||
	    player->mac_audio_frames[index].capacity != capacity ||
	    Mac::GetPendingAudioFrameSize(player->mac_decoder) != required_size) return false;
	if (!Mac::ReadAudioFrame(player->mac_decoder, buffer.data, required_size, &frame))
	{
		return false;
	}
	if (frame.data_size == 0 || frame.data_size > buffer.capacity || frame.data_size > UINT32_MAX)
	{
		printf("[AvPlayer] AVFoundation returned an invalid audio frame size: %zu/%u\n", frame.data_size, buffer.capacity);
		return false;
	}

	AvPlayerFrameInfo result {};
	result.data                         = buffer.data;
	result.time_stamp                   = frame.time_stamp;
	result.details.audio.channel_count  = player->mac_audio_channel_count;
	result.details.audio.sample_rate    = player->mac_audio_sample_rate;
	result.details.audio.size           = static_cast<uint32_t>(frame.data_size);
	result.details.audio.language_code[0] = 'u';
	result.details.audio.language_code[1] = 'n';
	result.details.audio.language_code[2] = 'd';
	result.details.audio.language_code[3] = '\0';

	*info = result;
	player->mac_next_audio_frame = (player->mac_next_audio_frame + 1) % player->mac_audio_frames.size();
	return true;
}
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
static constexpr size_t ANDROID_AUDIO_BUFFER_COUNT = 4;

static void* android_allocate(AvPlayerInternal* player, bool texture,
                              uint32_t alignment, uint32_t size)
{
	const auto callback = texture ? player->mem.allocate_texture
	                              : player->mem.allocate;
	if (callback == nullptr)
	{
		return nullptr;
	}
#if defined(__aarch64__)
	if (Loader::IsNativeHostCallTarget(reinterpret_cast<uint64_t>(callback)))
	{
		return callback(player->mem.object_pointer, alignment, size);
	}
	return reinterpret_cast<void*>(Loader::RunGuestX64(
	    reinterpret_cast<uint64_t>(callback),
	    reinterpret_cast<uint64_t>(player->mem.object_pointer), alignment, size));
#else
	return callback(player->mem.object_pointer, alignment, size);
#endif
}

static void android_deallocate(AvPlayerInternal* player, bool texture,
                               void* buffer)
{
	const auto callback = texture ? player->mem.deallocate_texture
	                              : player->mem.deallocate;
	if (buffer == nullptr || callback == nullptr)
	{
		return;
	}
#if defined(__aarch64__)
	if (Loader::IsNativeHostCallTarget(reinterpret_cast<uint64_t>(callback)))
	{
		callback(player->mem.object_pointer, buffer);
	} else
	{
		Loader::RunGuestX64(
		    reinterpret_cast<uint64_t>(callback),
		    reinterpret_cast<uint64_t>(player->mem.object_pointer),
		    reinterpret_cast<uint64_t>(buffer));
	}
#else
	callback(player->mem.object_pointer, buffer);
#endif
}

static void android_deallocate_video_frames(AvPlayerInternal* player)
{
	for (void* frame: player->android_video_frames)
	{
		android_deallocate(player, true, frame);
	}
	player->android_video_frames.clear();
	player->android_next_video_frame = 0;
	player->video_frame              = nullptr;
	player->video_frame_size         = 0;
}

static void android_deallocate_audio_frames(AvPlayerInternal* player)
{
	for (const auto& frame: player->android_audio_frames)
	{
		android_deallocate(player, false, frame.data);
	}
	player->android_audio_frames.clear();
	player->android_next_audio_frame        = 0;
	player->android_audio_channel_count     = 0;
	player->android_audio_sample_rate       = 0;
	player->android_audio_allocation_failed = false;
}

static bool android_open_media(AvPlayerInternal* player, const char* filename)
{
	if (player->android_decoder == nullptr)
	{
		return false;
	}
	++player->buffer_epoch;

	auto clean_path = String::FromUtf8(filename);
	while (clean_path.Size() >= 2 && clean_path.At(0) == U'/' &&
	       clean_path.At(1) == U'/')
	{
		clean_path = clean_path.Mid(1);
	}
	auto real_path = LibKernel::FileSystem::GetRealFilename(clean_path);
	printf("[AvPlayer] Resolved Android path: %s\n", real_path.C_Str());

	android_deallocate_video_frames(player);
	android_deallocate_audio_frames(player);
	player->video_eof = true;

	Android::VideoFormat video_format {};
	Android::AudioFormat audio_format {};
	if (!Android::Open(player->android_decoder, real_path.C_Str(),
	                   &video_format, &audio_format))
	{
		const auto error = Android::GetLastError(player->android_decoder);
		printf("[AvPlayer] Android MediaCodec open failed: %s\n", error.c_str());
		return false;
	}
	if (video_format.buffer_size == 0 || video_format.buffer_size > UINT32_MAX)
	{
		printf("[AvPlayer] Android NV12 frame size is invalid: %zu\n",
		       video_format.buffer_size);
		Android::Stop(player->android_decoder);
		return false;
	}
	if (player->mem.allocate_texture == nullptr ||
	    player->mem.deallocate_texture == nullptr)
	{
		printf("[AvPlayer] Guest texture allocator callbacks are unavailable\n");
		Android::Stop(player->android_decoder);
		return false;
	}
	if ((audio_format.channel_count == 0) != (audio_format.sample_rate == 0))
	{
		printf("[AvPlayer] Android audio format is incomplete: channels=%u rate=%u\n",
		       audio_format.channel_count, audio_format.sample_rate);
		Android::Stop(player->android_decoder);
		return false;
	}
	if (audio_format.channel_count != 0 &&
	    (player->mem.allocate == nullptr || player->mem.deallocate == nullptr))
	{
		printf("[AvPlayer] Guest audio allocator callbacks are unavailable\n");
		Android::Stop(player->android_decoder);
		return false;
	}

	player->android_video_frames.reserve(
	    player->android_video_framebuffer_count);
	for (uint32_t index = 0; index < player->android_video_framebuffer_count;
	     index++)
	{
		void* frame = android_allocate(
		    player, true, 256, static_cast<uint32_t>(video_format.buffer_size));
		if (frame == nullptr)
		{
			printf("[AvPlayer] Failed to allocate Android NV12 frame %u/%u (%zu bytes)\n",
			       index + 1, player->android_video_framebuffer_count,
			       video_format.buffer_size);
			android_deallocate_video_frames(player);
			Android::Stop(player->android_decoder);
			return false;
		}
		if (!avplayer_initialize_buffer(player, frame, video_format.buffer_size))
		{
			android_deallocate(player, true, frame);
			return false;
		}
		player->android_video_frames.push_back(frame);
	}

	player->video_width      = video_format.width;
	player->video_height     = video_format.height;
	player->video_frame_rate = video_format.frame_rate > 0.0f
	                               ? video_format.frame_rate
	                               : 30.0f;
	player->video_pitch      = video_format.pitch;
	player->video_frame_size = static_cast<uint32_t>(video_format.buffer_size);
	player->video_eof        = false;
	player->frame_timestamp  = 0;
	player->frame_count      = 0;
	if (audio_format.channel_count != 0)
	{
		player->android_audio_channel_count = audio_format.channel_count;
		player->android_audio_sample_rate   = audio_format.sample_rate;
		player->android_audio_frames.resize(ANDROID_AUDIO_BUFFER_COUNT);
	}
	printf("[AvPlayer] Android MediaCodec video: %ux%u pitch=%u @ %.2f fps, buffers=%u\n",
	       player->video_width, player->video_height, player->video_pitch,
	       static_cast<double>(player->video_frame_rate),
	       player->android_video_framebuffer_count);
	if (audio_format.channel_count != 0)
	{
		printf("[AvPlayer] Android MediaCodec audio: %u channels @ %u Hz, buffers=%zu\n",
		       audio_format.channel_count, audio_format.sample_rate,
		       ANDROID_AUDIO_BUFFER_COUNT);
	}
	return true;
}

static bool android_read_frame(AvPlayerInternal* player,
	                           AvPlayerFrameInfoEx* info)
{
	if (player->android_decoder == nullptr ||
	    player->android_video_frames.empty() || player->video_eof)
	{
		return false;
	}

	void* destination =
	    player->android_video_frames[player->android_next_video_frame];
	const auto index = player->android_next_video_frame;
	const auto bytes = player->video_frame_size;
	Core::VirtualMemory::GuestDataLease output;
	if (!avplayer_borrow_buffer(player, destination, bytes, &output) ||
	    player->android_next_video_frame != index || index >= player->android_video_frames.size() ||
	    player->android_video_frames[index] != destination || player->video_frame_size != bytes) return false;
	Android::VideoFrame frame {};
	if (!Android::ReadFrame(player->android_decoder, destination,
	                        player->video_frame_size, &frame))
	{
		player->video_eof = !Android::IsActive(player->android_decoder);
		if (player->video_eof)
		{
			const auto error = Android::GetLastError(player->android_decoder);
			if (!error.empty())
			{
				printf("[AvPlayer] Android MediaCodec playback ended: %s\n",
				       error.c_str());
			}
		}
		return false;
	}

	player->video_frame = destination;
	player->android_next_video_frame =
	    (player->android_next_video_frame + 1) %
	    player->android_video_frames.size();
	player->frame_timestamp = frame.time_stamp;
	info->data               = player->video_frame;
	info->time_stamp         = player->frame_timestamp;
	info->details.video.width        = player->video_width;
	info->details.video.height       = player->video_height;
	info->details.video.aspect_ratio =
	    static_cast<float>(player->video_width) /
	    static_cast<float>(player->video_height);
	info->details.video.language_code[0]      = 'u';
	info->details.video.language_code[1]      = 'n';
	info->details.video.language_code[2]      = 'd';
	info->details.video.language_code[3]      = '\0';
	info->details.video.framerate             = 0;
	info->details.video.crop_left_offset      = 0;
	info->details.video.crop_right_offset     = 0;
	info->details.video.crop_top_offset       = 0;
	info->details.video.crop_bottom_offset    = 0;
	info->details.video.pitch                 = player->video_pitch;
	info->details.video.luma_bit_depth        = 8;
	info->details.video.chroma_bit_depth      = 8;
	info->details.video.video_full_tange_flag = frame.full_range ? 1 : 0;
	player->frame_count++;
	return true;
}

static bool android_read_audio_frame(AvPlayerInternal* player,
	                                 AvPlayerFrameInfo* info)
{
	if (player->android_decoder == nullptr ||
	    player->android_audio_frames.empty() ||
	    player->android_audio_channel_count == 0 ||
	    player->android_audio_sample_rate == 0 ||
	    player->android_audio_allocation_failed)
	{
		return false;
	}

	const size_t required_size =
	    Android::GetPendingAudioFrameSize(player->android_decoder);
	if (required_size == 0)
	{
		return false;
	}
	if (required_size > UINT32_MAX)
	{
		printf("[AvPlayer] Android PCM16 frame is too large: %zu\n",
		       required_size);
		player->android_audio_allocation_failed = true;
		Android::Stop(player->android_decoder);
		player->video_eof = true;
		return false;
	}

	const auto index = player->android_next_audio_frame;
	auto& buffer = player->android_audio_frames[index];
	if (buffer.capacity < required_size)
	{
		void* replacement = android_allocate(
		    player, false, 16, static_cast<uint32_t>(required_size));
		if (replacement == nullptr)
		{
			printf("[AvPlayer] Guest Android audio allocation failed (%zu bytes)\n",
			       required_size);
			player->android_audio_allocation_failed = true;
			Android::Stop(player->android_decoder);
			player->video_eof = true;
			return false;
		}
		const auto previous = buffer.data;
		if (!avplayer_initialize_buffer(player, replacement, required_size) ||
		    player->android_next_audio_frame != index || index >= player->android_audio_frames.size() ||
		    player->android_audio_frames[index].data != previous)
		{
			android_deallocate(player, false, replacement);
			return false;
		}
		android_deallocate(player, false, buffer.data);
		buffer.data     = replacement;
		buffer.capacity = static_cast<uint32_t>(required_size);
	}

	Android::AudioFrame frame {};
	void* destination = buffer.data;
	const auto capacity = buffer.capacity;
	Core::VirtualMemory::GuestDataLease output;
	if (!avplayer_borrow_buffer(player, destination, required_size, &output) ||
	    player->android_next_audio_frame != index || index >= player->android_audio_frames.size() ||
	    player->android_audio_frames[index].data != destination ||
	    player->android_audio_frames[index].capacity != capacity ||
	    Android::GetPendingAudioFrameSize(player->android_decoder) != required_size) return false;
	if (!Android::ReadAudioFrame(player->android_decoder, buffer.data,
	                             required_size, &frame))
	{
		return false;
	}
	if (frame.data_size == 0 || frame.data_size > buffer.capacity ||
	    frame.data_size > UINT32_MAX)
	{
		printf("[AvPlayer] Android MediaCodec returned an invalid audio frame size: %zu/%u\n",
		       frame.data_size, buffer.capacity);
		return false;
	}

	AvPlayerFrameInfo result {};
	result.data                        = buffer.data;
	result.time_stamp                  = frame.time_stamp;
	result.details.audio.channel_count = player->android_audio_channel_count;
	result.details.audio.sample_rate   = player->android_audio_sample_rate;
	result.details.audio.size          = static_cast<uint32_t>(frame.data_size);
	result.details.audio.language_code[0] = 'u';
	result.details.audio.language_code[1] = 'n';
	result.details.audio.language_code[2] = 'd';
	result.details.audio.language_code[3] = '\0';
	*info = result;
	player->android_next_audio_frame =
	    (player->android_next_audio_frame + 1) %
	    player->android_audio_frames.size();
	return true;
}
#endif

// =====================================================================
// AvPlayer API implementation
// =====================================================================

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || \
    LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
static bool avplayer_release_closed_buffer(AvPlayerDeallocate callback,
                                          void* object, void* buffer)
{
	if (callback == nullptr || buffer == nullptr) return true;
#if defined(__aarch64__)
	if (!Loader::IsNativeHostCallTarget(reinterpret_cast<uint64_t>(callback)))
	{
		uint64_t result = 0;
		const bool called = Loader::TryRunGuestX64WithoutGuestBorrow(
		    reinterpret_cast<uint64_t>(callback), reinterpret_cast<uint64_t>(object),
		    reinterpret_cast<uint64_t>(buffer), 0, &result);
		if (!called)
		{
			EXIT("AvPlayer guest close deallocator could not execute safely");
			return false;
		}
		return true;
	}
#endif
	callback(object, buffer);
	return true;
}
#endif

static int avplayer_close_handle(const AvPlayerHandle& player)
{
	if (player == nullptr)
	{
		return 0;
	}
	auto* h = player.get();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || \
    LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	AvPlayerMemAllocator callbacks;
	std::vector<void*> video_frames;
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	Mac::Decoder* decoder = nullptr;
	std::vector<AvPlayerInternal::MacAudioBuffer> audio_frames;
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	Android::Decoder* decoder = nullptr;
	std::vector<AvPlayerInternal::AndroidAudioBuffer> audio_frames;
#endif
#if defined(LUMEN_AVPLAYER_LIFETIME_TEST_HOOK)
	if (g_avplayer_test_close_observed.load(std::memory_order_acquire))
	{
		g_avplayer_test_close_lock_attempted.store(true, std::memory_order_release);
	}
#endif
	{
		Core::LockGuard lock(h->mutex);
#if defined(LUMEN_AVPLAYER_LIFETIME_TEST_HOOK)
		if (g_avplayer_test_close_observed.load(std::memory_order_acquire))
		{
			g_avplayer_test_close_cleanup_started.store(true, std::memory_order_release);
		}
#endif
		h->closing = true;
		++h->buffer_epoch;
#ifdef _WIN32
		mf_close(h);
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		decoder = h->mac_decoder;
		h->mac_decoder = nullptr;
		video_frames.swap(h->mac_video_frames);
		audio_frames.swap(h->mac_audio_frames);
		h->mac_next_video_frame        = 0;
		h->mac_next_audio_frame        = 0;
		h->mac_audio_channel_count     = 0;
		h->mac_audio_sample_rate       = 0;
		h->mac_audio_allocation_failed = false;
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		decoder = h->android_decoder;
		h->android_decoder = nullptr;
		video_frames.swap(h->android_video_frames);
		audio_frames.swap(h->android_audio_frames);
		h->android_next_video_frame        = 0;
		h->android_next_audio_frame        = 0;
		h->android_audio_channel_count     = 0;
		h->android_audio_sample_rate       = 0;
		h->android_audio_allocation_failed = false;
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || \
    LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		callbacks = h->mem;
		h->video_frame = nullptr;
		h->video_frame_size = 0;
#endif
	}

	// Removing the registry entry and draining the player mutex excludes new
	// readers. Keep the native owner alive while destroying the detached decoder
	// and releasing guest buffers; no player lock or guest borrow spans a callback.
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	Mac::Destroy(decoder);
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	Android::Destroy(decoder);
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || \
    LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	for (void* frame: video_frames)
	{
		if (!avplayer_release_closed_buffer(callbacks.deallocate_texture,
		                                    callbacks.object_pointer, frame)) return -1;
	}
	for (const auto& frame: audio_frames)
	{
		if (!avplayer_release_closed_buffer(callbacks.deallocate,
		                                    callbacks.object_pointer, frame.data)) return -1;
	}
#endif
	return 0;
}

AvPlayerInternal* LUMEN_SYSV_ABI AvPlayerInit(AvPlayerInitData* init)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(init == nullptr);
	AvPlayerInitData owned_init;
	{
		Core::VirtualMemory::GuestDataLease input;
		if (!BorrowAudioGuestRange(init, sizeof(*init), Core::VirtualMemory::Mode::Read, &input)) return nullptr;
		std::memcpy(&owned_init, init, sizeof(owned_init));
	}
	init = &owned_init;
	std::string owned_language;
	if (init->default_language != nullptr &&
	    !SnapshotAudioGuestString(init->default_language, 64, &owned_language)) return nullptr;

	printf("\t memory_replacement.object_pointer     = %016" PRIx64 "\n",
	       reinterpret_cast<uint64_t>(init->memory_replacement.object_pointer));
	printf("\t memory_replacement.allocate           = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(init->memory_replacement.allocate));
	printf("\t memory_replacement.deallocate         = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(init->memory_replacement.deallocate));
	printf("\t memory_replacement.allocate_texture   = %016" PRIx64 "\n",
	       reinterpret_cast<uint64_t>(init->memory_replacement.allocate_texture));
	printf("\t memory_replacement.deallocate_texture = %016" PRIx64 "\n",
	       reinterpret_cast<uint64_t>(init->memory_replacement.deallocate_texture));
	printf("\t file_replacement.object_pointer       = %016" PRIx64 "\n",
	       reinterpret_cast<uint64_t>(init->file_replacement.object_pointer));
	printf("\t file_replacement.open                 = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(init->file_replacement.open));
	printf("\t file_replacement.close                = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(init->file_replacement.close));
	printf("\t file_replacement.read_offset          = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(init->file_replacement.read_offset));
	printf("\t file_replacement.size                 = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(init->file_replacement.size));
	printf("\t event_replacement.object_pointer      = %016" PRIx64 "\n",
	       reinterpret_cast<uint64_t>(init->event_replacement.object_pointer));
	printf("\t event_replacement.event_callback      = %016" PRIx64 "\n",
	       reinterpret_cast<uint64_t>(init->event_replacement.event_callback));
	printf("\t debug_level                           = %s\n", Core::EnumName(init->debug_level).C_Str());
	printf("\t num_output_video_framebuffers         = %d\n", init->num_output_video_framebuffers);
	printf("\t base_priority                         = %u\n", init->base_priority);
	printf("\t auto_start                            = %u\n", init->auto_start);
	printf("\t default_language                      = %s\n", init->default_language == nullptr ? "(null)" : owned_language.c_str());

	auto  player = std::make_shared<AvPlayerInternal>();
	auto* r      = player.get();

	// Relax these checks - some games may use file/event replacements
	// EXIT_NOT_IMPLEMENTED(init->auto_start != 0);
	// EXIT_NOT_IMPLEMENTED(init->file_replacement.object_pointer != nullptr);
	// EXIT_NOT_IMPLEMENTED(init->event_replacement.object_pointer != nullptr);

	r->mem  = init->memory_replacement;
	r->file = init->file_replacement;

	#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	r->mac_decoder                 = Mac::Create();
	r->mac_video_framebuffer_count = static_cast<uint32_t>(std::min(std::max(2, init->num_output_video_framebuffers), 16));
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	r->android_decoder = Android::Create();
	r->android_video_framebuffer_count = static_cast<uint32_t>(
	    std::min(std::max(2, init->num_output_video_framebuffers), 16));
#endif

	// Don't create fake video yet - wait for AddSource to try real decoding first

	const bool registered = avplayer_register(player);
	EXIT_IF(!registered);
	if (!registered)
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		Mac::Destroy(r->mac_decoder);
		r->mac_decoder = nullptr;
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		Android::Destroy(r->android_decoder);
		r->android_decoder = nullptr;
#endif
		return nullptr;
	}
	return r;
}

int LUMEN_SYSV_ABI AvPlayerAddSource(AvPlayerInternal* h, const char* filename)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(h == nullptr);
	auto player = avplayer_acquire(h);
	if (player == nullptr)
	{
		return -1;
	}
	h = player.get();
	std::string owned_filename;
	if (!SnapshotAudioGuestString(filename, 4096, &owned_filename)) return -1;
	filename = owned_filename.c_str();

	printf("\t filename = %s\n", filename);

	Core::LockGuard lock(h->mutex);
	if (h->closing || h->buffer_borrows != 0)
	{
		return -1;
	}

	h->filename = filename;

	#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (mac_open_media(h, filename))
	{
		printf("[AvPlayer] Real media decoding enabled through AVFoundation: %s\n", filename);
		return 0;
	}
	h->video_eof = true;
	return -1;
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (android_open_media(h, filename))
	{
		printf("[AvPlayer] Real media decoding enabled through Android MediaCodec: %s\n",
		       filename);
		return 0;
	}
	h->video_eof = true;
	return -1;
#endif

#ifdef _WIN32
	// Try to open the real video file using Media Foundation
	// Normalize path: collapse leading double slashes (//app0/ → /app0/) 
	// PS4 games sometimes use //app0/ but mount points use /app0/
	auto clean_path = String::FromUtf8(filename);
	while (clean_path.Size() >= 2 && clean_path.At(0) == U'/' && clean_path.At(1) == U'/')
	{
		clean_path = clean_path.Mid(1);
	}

	// Resolve PS4 path to Windows path
	auto real_path = LibKernel::FileSystem::GetRealFilename(clean_path);
	printf("[AvPlayer] Resolved path: %s\n", real_path.C_Str());

	// Convert to wide string for MF API
	// First convert forward slashes to backslashes
	auto win_path = real_path.ReplaceChar(U'/', U'\\');
	
	int wlen = MultiByteToWideChar(CP_UTF8, 0, win_path.C_Str(), -1, nullptr, 0);
	if (wlen > 0)
	{
		auto* wpath = new wchar_t[wlen];
		MultiByteToWideChar(CP_UTF8, 0, win_path.C_Str(), -1, wpath, wlen);

		if (mf_open_video(h, wpath))
		{
			printf("[AvPlayer] Real video decoding enabled for: %s\n", filename);
			delete[] wpath;
			return 0;
		}
		delete[] wpath;
	}

	// File replacements are needed only when the source cannot be resolved
	// through a mounted host path. Prefer the resolved file to avoid crossing a
	// guest ABI boundary merely to copy data already available to the host.
	if (mf_materialize_callback_stream(h, filename))
	{
		if (mf_open_video(h, h->mf_temp_path))
		{
			printf("[AvPlayer] Real video decoding enabled through callback stream: %s\n", filename);
			return 0;
		}
		DeleteFileW(h->mf_temp_path);
		h->mf_temp_path[0] = L'\0';
	}
	if (mf_materialize_fios_stream(h, filename))
	{
		if (mf_open_video(h, h->mf_temp_path))
		{
			printf("[AvPlayer] Real video decoding enabled through FIOS: %s\n", filename);
			return 0;
		}
		DeleteFileW(h->mf_temp_path);
		h->mf_temp_path[0] = L'\0';
	}

	printf("[AvPlayer] MF decoding failed\n");
#endif

	// Keep the player in a clean end-of-stream state. Some titles do not handle
	// AddSource errors and wait forever, while IsActive/GetVideoData correctly
	// describe an unavailable stream as inactive/no frame.
	h->video_eof = true;
	return 0;
}

int LUMEN_SYSV_ABI AvPlayerSetLooping(AvPlayerInternal* h, Bool loop)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(h == nullptr);
	auto player = avplayer_acquire(h);
	if (player == nullptr)
	{
		return -1;
	}
	h = player.get();

	printf("\t loop = %u\n", loop);

	Core::LockGuard lock(h->mutex);
	if (h->closing)
	{
		return -1;
	}

	h->loop = (loop != 0);

	#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	Mac::SetLooping(h->mac_decoder, h->loop);
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	Android::SetLooping(h->android_decoder, h->loop);
#endif

	return 0;
}

Bool LUMEN_SYSV_ABI AvPlayerGetVideoDataEx(AvPlayerInternal* h, AvPlayerFrameInfoEx* video_info)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(h == nullptr);
	EXIT_NOT_IMPLEMENTED(video_info == nullptr);
	auto player = avplayer_acquire(h);
	if (player == nullptr)
	{
		return 0;
	}
	h = player.get();
	AvPlayerFrameInfoEx result {};
	const auto decode = [&]() -> Bool {

	Core::LockGuard lock(h->mutex);
	if (h->closing || h->buffer_borrows != 0)
	{
		return 0;
	}

#ifdef _WIN32
	// Try real video decoding first
	if (h->mf_initialized)
	{
		if (mf_read_frame(h, &result))
		{
			return 1; // true
		}
		// MF reached EOF or error
		return 0; // false
	}
#endif

	#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (mac_read_frame(h, &result))
	{
		return 1;
	}
	return 0;
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	return android_read_frame(h, &result) ? 1 : 0;
#endif

	return 0; // false
	};
	if (!decode()) return 0;
	Core::VirtualMemory::GuestDataLease metadata;
	if (!BorrowAudioGuestRange(video_info, sizeof(*video_info), Core::VirtualMemory::Mode::Write, &metadata)) return 0;
	std::memcpy(video_info, &result, sizeof(result));
	return 1;
}

Bool LUMEN_SYSV_ABI AvPlayerGetAudioData(AvPlayerInternal* h, AvPlayerFrameInfo* audio_info)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(h == nullptr);
	EXIT_NOT_IMPLEMENTED(audio_info == nullptr);
	auto player = avplayer_acquire(h);
	if (player == nullptr)
	{
		return 0;
	}
	h = player.get();
	AvPlayerFrameInfo result {};
	const auto decode = [&]() -> Bool {

	Core::LockGuard lock(h->mutex);
	if (h->closing || h->buffer_borrows != 0)
	{
		return 0;
	}

#if defined(LUMEN_AVPLAYER_LIFETIME_TEST_HOOK)
	if (g_avplayer_test_hold_audio_read.load(std::memory_order_acquire))
	{
		g_avplayer_test_audio_read_entered.store(true, std::memory_order_release);
		while (!g_avplayer_test_release_audio_read.load(std::memory_order_acquire))
		{
			std::this_thread::yield();
		}
	}
#endif

	#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	return mac_read_audio_frame(h, &result) ? 1 : 0;
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	return android_read_audio_frame(h, &result) ? 1 : 0;
#endif

	return 0;
	};
	if (!decode()) return 0;
	Core::VirtualMemory::GuestDataLease metadata;
	if (!BorrowAudioGuestRange(audio_info, sizeof(*audio_info), Core::VirtualMemory::Mode::Write, &metadata)) return 0;
	std::memcpy(audio_info, &result, sizeof(result));
	return 1;
}

Bool LUMEN_SYSV_ABI AvPlayerIsActive(AvPlayerInternal* h)
{
	PRINT_NAME();

	auto player = avplayer_acquire(h);
	if (player != nullptr)
	{
		h = player.get();
		Core::LockGuard lock(h->mutex);
		if (h->closing)
		{
			return 0;
		}

#ifdef _WIN32
		if (h->mf_initialized)
		{
			return h->video_eof ? 0 : 1;
		}
#endif

	#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		if (h->mac_decoder != nullptr)
		{
			return Mac::IsActive(h->mac_decoder) ? 1 : 0;
		}
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		if (h->android_decoder != nullptr)
		{
			return Android::IsActive(h->android_decoder) ? 1 : 0;
		}
#endif
	}

	return 0; // false
}

int LUMEN_SYSV_ABI AvPlayerStop(AvPlayerInternal* h)
{
	PRINT_NAME();

	#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || \
	    LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	auto player = avplayer_acquire(h);
	if (player != nullptr)
	{
		h = player.get();
		Core::LockGuard lock(h->mutex);
		if (h->closing)
		{
			return 0;
		}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		Mac::Stop(h->mac_decoder);
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		Android::Stop(h->android_decoder);
#endif
		h->video_eof = true;
	}
#endif

	// Stub: game calls this to stop playback before closing
	// Just return success - the close call will clean up
	return 0;
}

int LUMEN_SYSV_ABI AvPlayerClose(AvPlayerInternal* h)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(h == nullptr);
	auto player = avplayer_remove(h);
	if (player == nullptr)
	{
		return 0;
	}
	return avplayer_close_handle(player);
}

bool ResetForNextSession()
{
	bool ok = true;
	for (const auto& player: avplayer_remove_all())
	{
		if (avplayer_close_handle(player) != 0)
		{
			ok = false;
		}
	}

	std::lock_guard lock(avplayer_registry_mutex());
	return ok && avplayer_registry().empty();
}

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool SessionResetContractTest()
{
	for (int cycle = 0; cycle < 2; cycle++)
	{
		auto player = std::make_shared<AvPlayerInternal>();
		auto weak   = std::weak_ptr<AvPlayerInternal>(player);
		if (!avplayer_register(player))
		{
			return false;
		}
		player.reset();
		if (!ResetForNextSession() || !weak.expired())
		{
			return false;
		}
	}
	return true;
}
#endif

#if defined(LUMEN_AVPLAYER_LIFETIME_TEST_HOOK)
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || \
    LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
struct AvPlayerCloseTestContext
{
	AvPlayerInternal* player = nullptr;
	uint8_t video_buffer = 0;
	uint8_t audio_buffer = 0;
	unsigned video_releases = 0;
	unsigned audio_releases = 0;
	bool valid = true;
};

static void LUMEN_SYSV_ABI avplayer_close_test_deallocate(void* object, void* buffer)
{
	auto& context = *static_cast<AvPlayerCloseTestContext*>(object);
	if (buffer == &context.video_buffer) ++context.video_releases;
	else if (buffer == &context.audio_buffer) ++context.audio_releases;
	else context.valid = false;
	context.valid = context.valid && avplayer_acquire(context.player) == nullptr &&
	                AvPlayerClose(context.player) == 0 && AvPlayerIsActive(context.player) == 0;

	// Core's mutex is recursive, so the callback's own thread would not prove
	// that close released it. A peer must acquire it without waiting.
	bool detached_and_unlocked = false;
	std::thread observer([&] {
		auto* player = context.player;
		if (!player->mutex.TryLock()) return;
		detached_and_unlocked = player->closing && player->video_frame == nullptr &&
		                        player->video_frame_size == 0;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		detached_and_unlocked = detached_and_unlocked && player->mac_decoder == nullptr &&
		                        player->mac_video_frames.empty() && player->mac_audio_frames.empty();
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		detached_and_unlocked = detached_and_unlocked && player->android_decoder == nullptr &&
		                        player->android_video_frames.empty() && player->android_audio_frames.empty();
#endif
		player->mutex.Unlock();
	});
	observer.join();
	context.valid = context.valid && detached_and_unlocked;
}
#endif

bool AvPlayerLifetimeRaceTest()
{
	PRINT_NAME_ENABLE(false);
	auto owner = std::make_shared<AvPlayerInternal>();
	auto weak  = std::weak_ptr<AvPlayerInternal>(owner);
	auto* raw  = owner.get();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || \
    LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	AvPlayerCloseTestContext callbacks;
	callbacks.player = raw;
	owner->mem.object_pointer = &callbacks;
	owner->mem.deallocate = avplayer_close_test_deallocate;
	owner->mem.deallocate_texture = avplayer_close_test_deallocate;
	owner->video_frame = &callbacks.video_buffer;
	owner->video_frame_size = 1;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	owner->mac_video_frames.push_back(&callbacks.video_buffer);
	owner->mac_audio_frames.push_back({&callbacks.audio_buffer, 1});
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	owner->android_video_frames.push_back(&callbacks.video_buffer);
	owner->android_audio_frames.push_back({&callbacks.audio_buffer, 1});
#endif
#endif
	if (!avplayer_register(owner))
	{
		return false;
	}
	owner.reset();

	std::atomic<bool> reader_ok {true};
	std::atomic<bool> close_done {false};
	std::atomic<int>  close_result {-1};
	g_avplayer_test_hold_audio_read.store(true, std::memory_order_release);
	g_avplayer_test_audio_read_entered.store(false, std::memory_order_release);
	g_avplayer_test_release_audio_read.store(false, std::memory_order_release);
	g_avplayer_test_close_observed.store(true, std::memory_order_release);
	g_avplayer_test_close_lock_attempted.store(false, std::memory_order_release);
	g_avplayer_test_close_cleanup_started.store(false, std::memory_order_release);

	auto wait_until = [](const auto& predicate) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!predicate())
		{
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return false;
			}
			std::this_thread::yield();
		}
		return true;
	};

	std::thread reader([&] {
		PRINT_NAME_ENABLE(false);
		AvPlayerFrameInfo info {};
		reader_ok.store(AvPlayerGetAudioData(raw, &info) == 0, std::memory_order_release);
	});

	const bool reader_entered = wait_until([&] { return g_avplayer_test_audio_read_entered.load(std::memory_order_acquire); });
	if (!reader_entered)
	{
		g_avplayer_test_release_audio_read.store(true, std::memory_order_release);
		reader.join();
		AvPlayerClose(raw);
		return false;
	}

	std::thread closer([&] {
		PRINT_NAME_ENABLE(false);
		close_result.store(AvPlayerClose(raw), std::memory_order_release);
		close_done.store(true, std::memory_order_release);
	});

	const bool removed = wait_until([&] { return avplayer_acquire(raw) == nullptr; });
	const bool close_attempted = wait_until([&] { return g_avplayer_test_close_lock_attempted.load(std::memory_order_acquire); });
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	const bool retained_while_reader_active = !weak.expired();
	const bool cleanup_waited_for_reader = !g_avplayer_test_close_cleanup_started.load(std::memory_order_acquire) &&
	                                       !close_done.load(std::memory_order_acquire);

	g_avplayer_test_release_audio_read.store(true, std::memory_order_release);
	reader.join();
	closer.join();
	g_avplayer_test_hold_audio_read.store(false, std::memory_order_release);
	g_avplayer_test_close_observed.store(false, std::memory_order_release);

	bool callbacks_ok = true;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || \
    LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	callbacks_ok = callbacks.valid && callbacks.video_releases == 1 && callbacks.audio_releases == 1;
#endif
	return removed && close_attempted && retained_while_reader_active && cleanup_waited_for_reader &&
	       g_avplayer_test_close_cleanup_started.load(std::memory_order_acquire) && reader_ok.load(std::memory_order_acquire) &&
	       close_done.load(std::memory_order_acquire) && close_result.load(std::memory_order_acquire) == 0 && weak.expired() &&
	       avplayer_acquire(raw) == nullptr && callbacks_ok;
}

namespace {
struct BufferHandoffProbe
{
	AvPlayerInternal* player = nullptr;
	uint64_t address = 0, bytes = 0;
	unsigned calls = 0;
	bool permit = true, change_epoch = false, unlocked = true, precise = true;
};
thread_local BufferHandoffProbe* current_buffer_handoff_probe = nullptr;

bool BufferHandoffProbeQuery(uint64_t address, uint64_t bytes, GuestCpu::MemoryAccessIntent intent) noexcept
{
	auto* probe = current_buffer_handoff_probe;
	if (probe == nullptr) return true;
	++probe->calls;
	probe->precise = probe->precise && address == probe->address && bytes == probe->bytes &&
	                 intent == GuestCpu::MemoryAccessIntent::Write;
	bool peer_acquired = false;
	std::thread peer([&] {
		if (!probe->player->mutex.TryLock()) return;
		peer_acquired = probe->player->buffer_borrows == 1;
		if (probe->change_epoch) ++probe->player->buffer_epoch;
		probe->player->mutex.Unlock();
	});
	peer.join();
	probe->unlocked = probe->unlocked && peer_acquired;
	return probe->permit;
}
}

bool AvPlayerBufferHandoffTest()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || \
    LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	using namespace Core::VirtualMemory;
	using namespace GuestCpu;
	auto player = std::make_shared<AvPlayerInternal>();
	std::array<uint8_t, 64> destination;
	destination.fill(0x35);
	BufferHandoffProbe probe;
	probe.player = player.get();
	probe.address = reinterpret_cast<uint64_t>(destination.data());
	probe.bytes = destination.size();
	if (!RegisterMemoryHandoffQuery(BufferHandoffProbeQuery)) return false;
	MarkHandoffRange(probe.address, probe.bytes);
	current_buffer_handoff_probe = &probe;
	bool ok = true;
	{
		Core::LockGuard lock(player->mutex);
		probe.permit = false;
		ok = !avplayer_initialize_buffer(player.get(), destination.data(), destination.size()) &&
		     destination.front() == 0x35 && destination.back() == 0x35;
		probe.permit = true;
		probe.change_epoch = true;
		ok = !avplayer_initialize_buffer(player.get(), destination.data(), destination.size()) &&
		     destination.front() == 0x35 && destination.back() == 0x35 && ok;
		probe.change_epoch = false;
		ok = avplayer_initialize_buffer(player.get(), destination.data(), destination.size()) && ok;
		// Repeating the zero fill keeps byte contents identical but must still
		// acquire a new write intention and producer handoff.
		ok = avplayer_initialize_buffer(player.get(), destination.data(), destination.size()) && ok;
	}
	current_buffer_handoff_probe = nullptr;
	return ok && probe.calls == 4 && probe.unlocked && probe.precise && player->buffer_borrows == 0 &&
	       std::all_of(destination.begin(), destination.end(), [](uint8_t value) { return value == 0; });
#else
	return true;
#endif
}
#endif

} // namespace AvPlayer

namespace Audio3d {

LIB_NAME("Audio3d", "Audio3d");

namespace Semaphore = LibKernel::Semaphore;

struct Audio3dOpenParameters
{
	size_t   size        = 0x20;
	uint32_t granularity = 256;
	uint32_t rate        = 0;
	uint32_t max_objects = 512;
	uint32_t queue_depth = 2;
	uint32_t buffer_mode = 2;
	uint32_t pad         = 0;
	// uint32_t num_beds;
};

struct Audio3dData
{
	enum class State
	{
		Empty,
		Ready,
		Play
	};

	std::atomic<State> state = State::Empty;
};

struct Audio3dInternal
{
	Audio3dData*          data                        = nullptr;
	Core::Mutex*          data_mutex                  = nullptr;
	uint64_t              data_delay                  = 0;
	Semaphore::KernelSema playback_sema               = nullptr;
	Audio3dOpenParameters params                      = {};
	int                   user_id                     = 0;
	float                 late_reverb_level           = 0.0f;
	float                 downmix_spread_radius       = 2.0f;
	int                   downmix_spread_height_aware = 0;
	uint32_t              data_index                  = 0;
	bool                  used                        = false;
	std::atomic_bool      playback_stop_requested    = false;
	std::atomic_bool      playback_finished           = false;
};

constexpr uint32_t MAX_PORTS = 4;

static Audio3dInternal g_ports[MAX_PORTS] = {};

static void playback_simulate(void* arg)
{
	auto* port = static_cast<Audio3dInternal*>(arg);
	EXIT_IF(port == nullptr);
	EXIT_IF(port->data_mutex == nullptr);
	EXIT_IF(port->data == nullptr);

	for (;;)
	{
		if (port->playback_stop_requested.load(std::memory_order_acquire))
		{
			break;
		}

		int result = Semaphore::KernelWaitSema(port->playback_sema, 1, nullptr);

		if (result != OK || port->playback_stop_requested.load(std::memory_order_acquire))
		{
			break;
		}

		Audio3dData* play_data = nullptr;

		port->data_mutex->Lock();
		{
			for (uint32_t i = 0; i < port->params.queue_depth; i++)
			{
				uint32_t index = (port->data_index + i) % port->params.queue_depth;

				if (port->data[index].state == Audio3dData::State::Play)
				{
					play_data = &port->data[index];
					break;
				}
			}
		}
		port->data_mutex->Unlock();

		EXIT_IF(play_data == nullptr);

		if (play_data != nullptr)
		{
			// TODO(): Audio output is not yet implemented, so simulate audio delay
			Core::Thread::SleepMicro(port->data_delay);
			play_data->state = Audio3dData::State::Empty;
		}
	}

	port->playback_finished.store(true, std::memory_order_release);
}

int LUMEN_SYSV_ABI Audio3dInitialize(int64_t reserved)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(reserved != 0);

	return OK;
}

void LUMEN_SYSV_ABI Audio3dGetDefaultOpenParameters(Audio3dOpenParameters* p)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(sizeof(Audio3dOpenParameters) != 0x20);
	Core::VirtualMemory::GuestDataLease output;
	if (!BorrowAudioGuestRange(p, sizeof(*p), Core::VirtualMemory::Mode::Write, &output)) return;

	*p = Audio3dOpenParameters();
}

int LUMEN_SYSV_ABI Audio3dPortOpen(int user_id, const Audio3dOpenParameters* parameters, uint32_t* id)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(parameters == nullptr);
	EXIT_NOT_IMPLEMENTED(id == nullptr);
	Audio3dOpenParameters owned_parameters;
	{
		Core::VirtualMemory::GuestDataLease input;
		if (!BorrowAudioGuestRange(parameters, sizeof(*parameters), Core::VirtualMemory::Mode::Read, &input)) return -1;
		std::memcpy(&owned_parameters, parameters, sizeof(owned_parameters));
	}
	parameters = &owned_parameters;
	Core::VirtualMemory::GuestDataLease output;
	if (!BorrowAudioGuestRange(id, sizeof(*id), Core::VirtualMemory::Mode::Write, &output)) return -1;
	EXIT_NOT_IMPLEMENTED(parameters->size != 0x20);

	printf("\t user_id     = %d\n", user_id);
	printf("\t granularity = %u\n", parameters->granularity);
	printf("\t rate        = %u\n", parameters->rate);
	printf("\t max_objects = %u\n", parameters->max_objects);
	printf("\t queue_depth = %u\n", parameters->queue_depth);
	printf("\t buffer_mode = %u\n", parameters->buffer_mode);

	EXIT_NOT_IMPLEMENTED(parameters->buffer_mode != 2);
	EXIT_NOT_IMPLEMENTED(user_id != 255 && user_id != 1);

	uint32_t port = 0;
	for (; port < MAX_PORTS; port++)
	{
		if (!g_ports[port].used)
		{
			break;
		}
	}

	EXIT_NOT_IMPLEMENTED(port >= MAX_PORTS);

	g_ports[port].user_id = user_id;
	g_ports[port].params  = *parameters;
	g_ports[port].used    = true;

	EXIT_IF(g_ports[port].data != nullptr);
	EXIT_IF(g_ports[port].data_mutex != nullptr);
	EXIT_IF(g_ports[port].playback_sema != nullptr);

	g_ports[port].data       = new Audio3dData[parameters->queue_depth];
	g_ports[port].data_index = 0;
	g_ports[port].data_mutex = new Core::Mutex;
	g_ports[port].data_delay = (1000000 * static_cast<uint64_t>(parameters->granularity)) / 48000;

	for (uint32_t d = 0; d < parameters->queue_depth; d++)
	{
		g_ports[port].data[d].state = Audio3dData::State::Empty;
	}

	int result = Semaphore::KernelCreateSema(&g_ports[port].playback_sema, "audio3d_play", 0x01, 0,
	                                         static_cast<int>(parameters->queue_depth), nullptr);
	EXIT_NOT_IMPLEMENTED(result != OK);

	g_ports[port].playback_stop_requested.store(false, std::memory_order_release);
	g_ports[port].playback_finished.store(false, std::memory_order_release);
	Core::Thread playback_thread(playback_simulate, &g_ports[port]);
	playback_thread.Detach();

	*id = port;

	return OK;
}

int LUMEN_SYSV_ABI Audio3dPortSetAttribute(uint32_t port_id, uint32_t attribute_id, const void* attribute, size_t attribute_size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(port_id >= MAX_PORTS);
	EXIT_NOT_IMPLEMENTED(!g_ports[port_id].used);
	EXIT_NOT_IMPLEMENTED(attribute == nullptr);
	std::array<uint8_t, 4> owned_attribute {};
	float float_attribute = 0.0f;
	int int_attribute = 0;
	if (attribute_size == owned_attribute.size())
	{
		Core::VirtualMemory::GuestDataLease input;
		if (!BorrowAudioGuestRange(attribute, owned_attribute.size(), Core::VirtualMemory::Mode::Read, &input)) return -1;
		std::memcpy(owned_attribute.data(), attribute, owned_attribute.size());
		std::memcpy(&float_attribute, owned_attribute.data(), sizeof(float_attribute));
		std::memcpy(&int_attribute, owned_attribute.data(), sizeof(int_attribute));
	}

	printf("\t attribute_id = 0x%" PRIx32 "\n", attribute_id);

	switch (attribute_id)
	{
		case 0x10001:
			EXIT_NOT_IMPLEMENTED(attribute_size != 4);
			g_ports[port_id].late_reverb_level = float_attribute;
			printf("\t late_reverb_level = %f\n", g_ports[port_id].late_reverb_level);
			break;
		case 0x10002:
			EXIT_NOT_IMPLEMENTED(attribute_size != 4);
			g_ports[port_id].downmix_spread_radius = float_attribute;
			printf("\t downmix_spread_radius = %f\n", g_ports[port_id].downmix_spread_radius);
			break;
		case 0x10003:
			EXIT_NOT_IMPLEMENTED(attribute_size != 4);
			g_ports[port_id].downmix_spread_height_aware = int_attribute;
			printf("\t downmix_spread_height_aware = %d\n", g_ports[port_id].downmix_spread_height_aware);
			break;
		default: EXIT("unknown attribute: 0x%" PRIx32 "\n", attribute_id);
	}

	return OK;
}

int LUMEN_SYSV_ABI Audio3dPortGetQueueLevel(uint32_t port_id, uint32_t* queue_level, uint32_t* queue_available)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(port_id >= MAX_PORTS);
	EXIT_NOT_IMPLEMENTED(!g_ports[port_id].used);
	EXIT_NOT_IMPLEMENTED(queue_level == nullptr && queue_available == nullptr);
	Core::VirtualMemory::GuestDataLease level_output;
	Core::VirtualMemory::GuestDataLease available_output;
	if (queue_level != nullptr &&
	    !BorrowAudioGuestRange(queue_level, sizeof(*queue_level), Core::VirtualMemory::Mode::Write, &level_output)) return -1;
	if (queue_available != nullptr &&
	    !BorrowAudioGuestRange(queue_available, sizeof(*queue_available), Core::VirtualMemory::Mode::Write, &available_output)) return -1;

	auto* port = &g_ports[port_id];

	uint32_t empty_num = 0;

	port->data_mutex->Lock();
	{
		for (uint32_t i = 0; i < port->params.queue_depth; i++)
		{
			uint32_t index = (port->data_index + i) % port->params.queue_depth;

			if (port->data[index].state == Audio3dData::State::Empty)
			{
				empty_num++;
			} else
			{
				break;
			}
		}
	}
	port->data_mutex->Unlock();

	EXIT_IF(empty_num > port->params.queue_depth);

	printf("\t queue_available = %u\n", empty_num);

	if (queue_level != nullptr)
	{
		*queue_level = port->params.queue_depth - empty_num;
	}
	if (queue_available != nullptr)
	{
		*queue_available = empty_num;
	}

	return OK;
}

int LUMEN_SYSV_ABI Audio3dPortAdvance(uint32_t port_id)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(port_id >= MAX_PORTS);
	EXIT_NOT_IMPLEMENTED(!g_ports[port_id].used);

	auto* port = &g_ports[port_id];

	port->data_mutex->Lock();
	{
		uint32_t current_index = port->data_index;
		uint32_t next_index    = (current_index + 1) % port->params.queue_depth;

		if (port->data[current_index].state == Audio3dData::State::Empty)
		{
			port->data[current_index].state = Audio3dData::State::Ready;
		}

		EXIT_NOT_IMPLEMENTED(port->data[current_index].state != Audio3dData::State::Ready);

		port->data_index = next_index;

		printf("\t %u -> %u\n", current_index, next_index);
	}
	port->data_mutex->Unlock();

	return OK;
}

int LUMEN_SYSV_ABI Audio3dPortPush(uint32_t port_id, uint32_t blocking)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(port_id >= MAX_PORTS);
	EXIT_NOT_IMPLEMENTED(!g_ports[port_id].used);

	auto* port = &g_ports[port_id];

	EXIT_NOT_IMPLEMENTED(blocking != 1);

	printf("\t blocking = %u\n", blocking);

	int          data_num   = 0;
	Audio3dData* first_data = nullptr;

	port->data_mutex->Lock();
	{
		first_data = port->data + port->data_index;

		for (uint32_t i = 0; i < port->params.queue_depth; i++)
		{
			uint32_t index = (port->data_index + i) % port->params.queue_depth;

			if (port->data[index].state == Audio3dData::State::Ready)
			{
				port->data[index].state = Audio3dData::State::Play;
				data_num++;
			}
		}
	}
	port->data_mutex->Unlock();

	printf("\t push num = %d\n", data_num);

	if (data_num > 0)
	{
		Semaphore::KernelSignalSema(port->playback_sema, data_num);

		if (blocking == 1)
		{
			auto wait_time = port->data_delay / 8;
			while (first_data->state != Audio3dData::State::Empty
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
			       && !Loader::GuestExecutionStopRequested()
#endif
			)
			{
			#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
				Loader::ScopedGuestHostWait host_wait;
			#endif
				Core::Thread::SleepMicro(wait_time);
			}
		}
	}

	return OK;
}

bool ResetForNextSession()
{
	for (auto& port: g_ports)
	{
		if (port.used && (port.data == nullptr || port.data_mutex == nullptr || port.playback_sema == nullptr))
		{
			return false;
		}
		if (!port.used && (port.data != nullptr || port.data_mutex != nullptr || port.playback_sema != nullptr))
		{
			return false;
		}
	}

	for (auto& port: g_ports)
	{
		if (!port.used)
		{
			continue;
		}
		port.playback_stop_requested.store(true, std::memory_order_release);
		// Cancel both wakes a current waiter and leaves a credit for the race in
		// which the worker enters KernelWaitSema immediately after cancellation.
		const int wake_count = port.params.queue_depth == 0 ? 0 : 1;
		(void)Semaphore::KernelCancelSema(port.playback_sema, wake_count, nullptr);
	}

	constexpr uint32_t WAIT_STEP_US = 1000;
	constexpr uint32_t WAIT_STEPS   = 5000;
	for (uint32_t step = 0; step < WAIT_STEPS; step++)
	{
		bool all_finished = true;
		for (const auto& port: g_ports)
		{
			if (port.used && !port.playback_finished.load(std::memory_order_acquire))
			{
				all_finished = false;
				break;
			}
		}
		if (all_finished)
		{
			break;
		}
		Core::Thread::SleepMicro(WAIT_STEP_US);
	}

	for (const auto& port: g_ports)
	{
		if (port.used && !port.playback_finished.load(std::memory_order_acquire))
		{
			return false;
		}
	}

	bool ok = true;
	for (auto& port: g_ports)
	{
		if (!port.used)
		{
			continue;
		}
		if (Semaphore::KernelDeleteSema(port.playback_sema) != OK)
		{
			ok = false;
		}
		port.playback_sema = nullptr;
		delete[] port.data;
		port.data = nullptr;
		delete port.data_mutex;
		port.data_mutex                  = nullptr;
		port.data_delay                  = 0;
		port.params                      = {};
		port.user_id                     = 0;
		port.late_reverb_level           = 0.0f;
		port.downmix_spread_radius       = 2.0f;
		port.downmix_spread_height_aware = 0;
		port.data_index                  = 0;
		port.used                        = false;
		port.playback_stop_requested.store(false, std::memory_order_release);
		port.playback_finished.store(false, std::memory_order_release);
	}
	return ok;
}

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool SessionResetContractTest()
{
	Audio3dOpenParameters parameters;
	uint32_t              first_id = UINT32_MAX;
	uint32_t              second_id = UINT32_MAX;
	if (Audio3dPortOpen(255, &parameters, &first_id) != OK || first_id != 0 || !ResetForNextSession())
	{
		return false;
	}
	if (Audio3dPortOpen(255, &parameters, &second_id) != OK || second_id != first_id)
	{
		return false;
	}
	return ResetForNextSession();
}
#endif

} // namespace Audio3d

namespace Ngs2 {

LIB_NAME("Ngs2", "Ngs2");

namespace {

[[maybe_unused]] constexpr size_t NGS2_WAVEFORM_INFO_SIZE = 24;
[[maybe_unused]] constexpr size_t NGS2_WAVEFORM_OUTPUT_SIZE = sizeof(uint32_t);
[[maybe_unused]] constexpr uint32_t NGS2_WAVEFORM_TYPE_ATRAC9 = 0x40;
constexpr uint32_t NGS2_ATRAC9_MAX_CODEC_FRAME_BYTES = 2048;
constexpr uint32_t NGS2_ATRAC9_MAX_SUPERFRAME_BYTES = 8192;
constexpr uint32_t NGS2_RENDER_WAVEFORM_TYPE_FLOAT32 = 0x18;
constexpr size_t   NGS2_AUDIO_RENDER_MAX_ENCODED_BYTES = 64 * 1024 * 1024;
constexpr size_t   NGS2_AUDIO_RENDER_MAX_PCM_SAMPLES = 64 * 1024 * 1024;

struct Ngs2WaveformMemoryRange
{
	uint64_t begin = 0;
	uint64_t end   = 0;
};

struct Ngs2WaveformInfoTrace
{
	bool     captured     = false;
	uint32_t type         = 0;
	uint32_t channels     = 0;
	uint32_t sample_rate  = 0;
	uint32_t config_data  = 0;
	uint32_t frame_offset = 0;
	uint32_t frame_margin = 0;
};

struct Ngs2Atrac9Geometry
{
	uint32_t superframe_bytes          = 0;
	uint32_t superframe_samples        = 0;
	uint32_t codec_frames_per_superframe = 0;
	uint32_t output_rate_multiplier    = 1;
};

[[maybe_unused]] uint32_t Ngs2LoadLe32(const uint8_t* bytes)
{
	uint32_t value = 0;
	for (size_t byte = 0; byte < sizeof(value); ++byte)
		value |= static_cast<uint32_t>(bytes[byte]) << (byte * 8);
	return value;
}

[[maybe_unused]] std::array<uint8_t, sizeof(uint32_t)> Ngs2StoreLe32(uint32_t value)
{
	std::array<uint8_t, sizeof(value)> bytes {};
	for (size_t byte = 0; byte < bytes.size(); ++byte)
		bytes[byte] = static_cast<uint8_t>(value >> (byte * 8));
	return bytes;
}

bool Ngs2MakeMemoryRange(uint64_t address, size_t size, Ngs2WaveformMemoryRange* range)
{
	if (range == nullptr || address == 0 || size == 0 || address > UINT64_MAX - size) return false;
	range->begin = address;
	range->end   = address + size;
	return true;
}

bool Ngs2MemoryRangesOverlap(const Ngs2WaveformMemoryRange& left, const Ngs2WaveformMemoryRange& right)
{
	return left.begin < right.end && right.begin < left.end;
}

[[maybe_unused]] uint32_t Ngs2PcmBytesPerSample(uint32_t type)
{
	switch (type)
	{
		case 0x10:
		case 0x11: return 1;
		case 0x12:
		case 0x13: return 2;
		case 0x14:
		case 0x15: return 3;
		case 0x16:
		case 0x17:
		case 0x18:
		case 0x19: return 4;
		case 0x1a:
		case 0x1b: return 8;
		default: return 0;
	}
}

[[maybe_unused]] Ngs2WaveformFrameInfoResult Ngs2GetAtrac9Geometry(uint32_t config_data, uint32_t expected_channels,
                                                  uint32_t expected_sample_rate, Ngs2Atrac9Geometry* geometry)
{
	if (geometry == nullptr) return Ngs2WaveformFrameInfoResult::InvalidAtrac9Geometry;

	const uint32_t header                 = config_data >> 24;
	const uint32_t channel_config_index   = (config_data >> 17) & 0x7u;
	const uint32_t validation_bit         = (config_data >> 16) & 0x1u;
	const uint32_t codec_frame_bytes      = ((config_data >> 5) & 0x7ffu) + 1u;
	const uint32_t superframe_index       = (config_data >> 3) & 0x3u;
	const uint32_t reserved_bits          = config_data & 0x7u;
	if (header != 0xfeu || channel_config_index > 5u || validation_bit != 0u ||
	    (superframe_index != 0u && superframe_index != 2u) || reserved_bits != 0u ||
	    codec_frame_bytes > NGS2_ATRAC9_MAX_CODEC_FRAME_BYTES)
		return Ngs2WaveformFrameInfoResult::InvalidAtrac9Config;

	std::array<unsigned char, ATRAC9_CONFIG_DATA_SIZE> config_bytes {{
	    static_cast<unsigned char>(config_data >> 24), static_cast<unsigned char>(config_data >> 16),
	    static_cast<unsigned char>(config_data >> 8), static_cast<unsigned char>(config_data)}};
	Atrac9CodecInfo codec_info {};
	int             init_result = -1;
	int             info_result = -1;
	{
		std::lock_guard lock(Ajm::Atrac9LibraryMutex());
		void*            handle = Atrac9GetHandle();
		if (handle == nullptr) return Ngs2WaveformFrameInfoResult::InvalidAtrac9Geometry;
		init_result = Atrac9InitDecoder(handle, config_bytes.data());
		if (init_result == 0) info_result = Atrac9GetCodecInfo(handle, &codec_info);
		Atrac9ReleaseHandle(handle);
	}
	if (init_result != 0 || info_result != 0) return Ngs2WaveformFrameInfoResult::InvalidAtrac9Config;
	if (codec_info.channels <= 0 || static_cast<uint32_t>(codec_info.channels) != expected_channels)
		return Ngs2WaveformFrameInfoResult::Atrac9ChannelsMismatch;
	if (codec_info.samplingRate <= 0 ||
	    (static_cast<uint32_t>(codec_info.samplingRate) != expected_sample_rate &&
	     !(codec_info.samplingRate == 24000 && expected_sample_rate == 48000)))
		return Ngs2WaveformFrameInfoResult::Atrac9SampleRateMismatch;
	if (codec_info.framesInSuperframe != 1 && codec_info.framesInSuperframe != 4)
		return Ngs2WaveformFrameInfoResult::InvalidAtrac9Geometry;
	if (codec_info.frameSamples != 64 && codec_info.frameSamples != 128 && codec_info.frameSamples != 256)
		return Ngs2WaveformFrameInfoResult::InvalidAtrac9Geometry;
	if (codec_info.superframeSize <= 0 ||
	    static_cast<uint32_t>(codec_info.superframeSize) > NGS2_ATRAC9_MAX_SUPERFRAME_BYTES)
		return Ngs2WaveformFrameInfoResult::InvalidAtrac9Geometry;

	const uint32_t codec_frames = static_cast<uint32_t>(codec_info.framesInSuperframe);
	const uint32_t superframe_bytes = static_cast<uint32_t>(codec_info.superframeSize);
	if (superframe_bytes % codec_frames != 0 || superframe_bytes / codec_frames != codec_frame_bytes)
		return Ngs2WaveformFrameInfoResult::InvalidAtrac9Geometry;
	const uint64_t sample_count = static_cast<uint64_t>(codec_info.frameSamples) * codec_frames;
	if (sample_count == 0 || sample_count > UINT32_MAX)
		return Ngs2WaveformFrameInfoResult::InvalidAtrac9Geometry;

	geometry->superframe_bytes            = superframe_bytes;
	geometry->superframe_samples          = static_cast<uint32_t>(sample_count);
	geometry->codec_frames_per_superframe = codec_frames;
	geometry->output_rate_multiplier      =
	    codec_info.samplingRate == 24000 && expected_sample_rate == 48000 ? 2u : 1u;
	return Ngs2WaveformFrameInfoResult::Ok;
}

const char* Ngs2WaveformFrameInfoResultName(Ngs2WaveformFrameInfoResult result)
{
	switch (result)
	{
		case Ngs2WaveformFrameInfoResult::Ok: return "complete-local";
		case Ngs2WaveformFrameInfoResult::Disabled: return "disabled";
		case Ngs2WaveformFrameInfoResult::InvalidInfo: return "info-unreadable";
		case Ngs2WaveformFrameInfoResult::NoOutputs: return "outputs-absent";
		case Ngs2WaveformFrameInfoResult::UnsupportedType: return "waveform-type-unsupported";
		case Ngs2WaveformFrameInfoResult::InvalidChannels: return "channels-invalid";
		case Ngs2WaveformFrameInfoResult::InvalidSampleRate: return "sample-rate-invalid";
		case Ngs2WaveformFrameInfoResult::UnsupportedConfigData: return "config-data-unsupported";
		case Ngs2WaveformFrameInfoResult::InvalidAtrac9Config: return "atrac9-config-invalid";
		case Ngs2WaveformFrameInfoResult::Atrac9ChannelsMismatch: return "atrac9-channels-mismatch";
		case Ngs2WaveformFrameInfoResult::Atrac9SampleRateMismatch: return "atrac9-sample-rate-mismatch";
		case Ngs2WaveformFrameInfoResult::InvalidAtrac9Geometry: return "atrac9-geometry-invalid";
		case Ngs2WaveformFrameInfoResult::UnsupportedFrameOffset: return "frame-offset-unsupported";
		case Ngs2WaveformFrameInfoResult::UnsupportedFrameMargin: return "frame-margin-unsupported";
		case Ngs2WaveformFrameInfoResult::InvalidOutput: return "output-unwritable";
		case Ngs2WaveformFrameInfoResult::AliasedMemory: return "memory-alias-unsupported";
	}
	return "unknown";
}

Ngs2WaveformFrameInfoResult Ngs2GetWaveformFrameInfoModelImpl(
    uint64_t info_address, uint64_t frame_size_address, uint64_t samples_per_frame_address,
    uint64_t units_per_frame_address, uint64_t delay_samples_address, Ngs2WaveformInfoTrace* trace);

} // namespace

bool Ngs2WaveformFrameInfoExperimentalEnabled()
{
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID) && \
    LUMEN_ABI == LUMEN_ABI_ARM64_V8A
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_NGS2_WAVEFORM_FRAME_INFO");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
#else
	return false;
#endif
}

bool Ngs2VoiceStateFlagsExperimentalEnabled()
{
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID) && \
    LUMEN_ABI == LUMEN_ABI_ARM64_V8A
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_NGS2_VOICE_STATE_FLAGS");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
#else
	return false;
#endif
}

bool Ngs2AudioRenderExperimentalEnabled()
{
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID) && \
    LUMEN_ABI == LUMEN_ABI_ARM64_V8A
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
#else
	return false;
#endif
}

Ngs2WaveformFrameInfoResult Ngs2GetWaveformFrameInfoModel(uint64_t info_address, uint64_t frame_size_address,
                                                          uint64_t samples_per_frame_address,
                                                          uint64_t units_per_frame_address,
                                                          uint64_t delay_samples_address)
{
	return Ngs2GetWaveformFrameInfoModelImpl(info_address, frame_size_address, samples_per_frame_address,
	                                         units_per_frame_address, delay_samples_address, nullptr);
}

namespace {

Ngs2WaveformFrameInfoResult Ngs2GetWaveformFrameInfoModelImpl(
    uint64_t info_address, uint64_t frame_size_address, uint64_t samples_per_frame_address,
    uint64_t units_per_frame_address, uint64_t delay_samples_address, Ngs2WaveformInfoTrace* trace)
{
#if (LUMEN_PLATFORM != LUMEN_PLATFORM_OSX && \
     LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID) || \
    LUMEN_ABI != LUMEN_ABI_ARM64_V8A
	(void)info_address;
	(void)frame_size_address;
	(void)samples_per_frame_address;
	(void)units_per_frame_address;
	(void)delay_samples_address;
	return Ngs2WaveformFrameInfoResult::Disabled;
#else
	if (!Ngs2WaveformFrameInfoExperimentalEnabled()) return Ngs2WaveformFrameInfoResult::Disabled;

	Ngs2WaveformMemoryRange info_range;
	if (!Ngs2MakeMemoryRange(info_address, NGS2_WAVEFORM_INFO_SIZE, &info_range))
		return Ngs2WaveformFrameInfoResult::InvalidInfo;
	std::array<uint8_t, NGS2_WAVEFORM_INFO_SIZE> info {};
	if (!Loader::ReadGuestMemory(info_address, info.data(), info.size()))
		return Ngs2WaveformFrameInfoResult::InvalidInfo;

	const std::array<uint64_t, 4> output_addresses {{
	    frame_size_address, samples_per_frame_address, units_per_frame_address, delay_samples_address}};
	bool has_output = false;
	for (uint64_t address: output_addresses) has_output = has_output || address != 0;
	if (!has_output) return Ngs2WaveformFrameInfoResult::NoOutputs;

	const uint32_t type             = Ngs2LoadLe32(info.data());
	const uint32_t channels         = Ngs2LoadLe32(info.data() + 4);
	const uint32_t sample_rate      = Ngs2LoadLe32(info.data() + 8);
	const uint32_t config_data      = Ngs2LoadLe32(info.data() + 12);
	const uint32_t frame_offset     = Ngs2LoadLe32(info.data() + 16);
	const uint32_t frame_margin     = Ngs2LoadLe32(info.data() + 20);
	if (trace != nullptr)
	{
		*trace = {true, type, channels, sample_rate, config_data, frame_offset, frame_margin};
	}
	if (channels != 1 && channels != 2 && channels != 6 && channels != 8)
		return Ngs2WaveformFrameInfoResult::InvalidChannels;
	if (sample_rate == 0) return Ngs2WaveformFrameInfoResult::InvalidSampleRate;
	if (frame_offset != 0) return Ngs2WaveformFrameInfoResult::UnsupportedFrameOffset;
	if (frame_margin != 0) return Ngs2WaveformFrameInfoResult::UnsupportedFrameMargin;

	std::array<uint32_t, 4> output_values {};
	const uint32_t          bytes_per_sample = Ngs2PcmBytesPerSample(type);
	if (bytes_per_sample != 0)
	{
		if (config_data != 0) return Ngs2WaveformFrameInfoResult::UnsupportedConfigData;
		output_values = {{channels * bytes_per_sample, 1, 1, 0}};
	} else if (type == NGS2_WAVEFORM_TYPE_ATRAC9)
	{
		Ngs2Atrac9Geometry geometry;
		const auto result = Ngs2GetAtrac9Geometry(config_data, channels, sample_rate, &geometry);
		if (result != Ngs2WaveformFrameInfoResult::Ok) return result;
		// API metadata observed by original PS4 probes LMNQ10033/LMNQ10034.
		// Zero sample metadata is valid and remains separate from the positive
		// codec geometry used to decode PCM, including declared 24/48 kHz output.
		constexpr std::array<uint32_t, 16> samples_per_unit {{
		    0, 64, 0, 0, 128, 0, 0, 256, 0, 0, 0, 0, 0, 0, 0, 0}};
		const uint32_t metadata_samples = samples_per_unit[(config_data >> 20u) & 0xfu];
		output_values = {{geometry.superframe_bytes, metadata_samples * geometry.codec_frames_per_superframe,
		                  geometry.codec_frames_per_superframe, metadata_samples}};
	} else
	{
		return Ngs2WaveformFrameInfoResult::UnsupportedType;
	}
	std::array<Ngs2WaveformMemoryRange, 4> output_ranges {};
	for (size_t index = 0; index < output_addresses.size(); ++index)
	{
		const uint64_t address = output_addresses[index];
		if (address == 0) continue;
		if (!Ngs2MakeMemoryRange(address, NGS2_WAVEFORM_OUTPUT_SIZE, &output_ranges[index]) ||
		    !Loader::ValidateGuestMemory(address, NGS2_WAVEFORM_OUTPUT_SIZE, true))
			return Ngs2WaveformFrameInfoResult::InvalidOutput;
		if (Ngs2MemoryRangesOverlap(info_range, output_ranges[index]))
			return Ngs2WaveformFrameInfoResult::AliasedMemory;
		for (size_t previous = 0; previous < index; ++previous)
		{
			if (output_addresses[previous] != 0 &&
			    Ngs2MemoryRangesOverlap(output_ranges[previous], output_ranges[index]))
				return Ngs2WaveformFrameInfoResult::AliasedMemory;
		}
	}

	for (size_t index = 0; index < output_addresses.size(); ++index)
	{
		if (output_addresses[index] == 0) continue;
		const auto bytes = Ngs2StoreLe32(output_values[index]);
		if (!Loader::WriteGuestMemory(output_addresses[index], bytes.data(), bytes.size()))
			return Ngs2WaveformFrameInfoResult::InvalidOutput;
	}
	return Ngs2WaveformFrameInfoResult::Ok;
#endif
}

} // namespace

int32_t LUMEN_SYSV_ABI Ngs2GetWaveformFrameInfo(const Ngs2WaveformInfo* info, uint32_t* frame_size,
                                                uint32_t* samples_per_frame, uint32_t* units_per_frame,
                                                uint32_t* delay_samples)
{
	PRINT_NAME();
	const uint32_t output_mask = (frame_size != nullptr ? 0x1u : 0u) |
	                             (samples_per_frame != nullptr ? 0x2u : 0u) |
	                             (units_per_frame != nullptr ? 0x4u : 0u) |
	                             (delay_samples != nullptr ? 0x8u : 0u);
	Ngs2WaveformInfoTrace trace;
	const auto result = Ngs2GetWaveformFrameInfoModelImpl(
	    reinterpret_cast<uint64_t>(info), reinterpret_cast<uint64_t>(frame_size),
	    reinterpret_cast<uint64_t>(samples_per_frame), reinterpret_cast<uint64_t>(units_per_frame),
	    reinterpret_cast<uint64_t>(delay_samples), &trace);
	if (trace.captured)
	{
		std::fprintf(stderr,
		             "[NGS2_WAVEFORM_FRAME_INFO_LOCAL_EXPERIMENT] result=%s experimental=1 "
		             "console_contract=unverified profile=pcm-interleaved+atrac9-superframe "
		             "codecs=pcm,atrac9 "
		             "type=0x%08x channels=%u sample_rate=%u config_data=0x%08x "
		             "frame_offset=%u frame_margin=%u output_mask=0x%x\n",
		             Ngs2WaveformFrameInfoResultName(result), trace.type, trace.channels, trace.sample_rate,
		             trace.config_data, trace.frame_offset, trace.frame_margin, output_mask);
	} else
	{
		std::fprintf(stderr,
		             "[NGS2_WAVEFORM_FRAME_INFO_LOCAL_EXPERIMENT] result=%s experimental=1 "
		             "console_contract=unverified profile=pcm-interleaved+atrac9-superframe "
		             "codecs=pcm,atrac9 "
		             "fields=unavailable output_mask=0x%x\n",
		             Ngs2WaveformFrameInfoResultName(result), output_mask);
	}
	std::fflush(stderr);
	if (result != Ngs2WaveformFrameInfoResult::Ok)
	{
		EXIT("NGS2 WaveformFrameInfo local experiment cannot model request: %s",
		     Ngs2WaveformFrameInfoResultName(result));
		std::abort();
	}
	return 0;
}

struct Ngs2SystemOption
{
	size_t   size              = 0;
	char     name[16]          = {};
	uint32_t flags             = 0;
	uint32_t max_grain_samples = 0;
	uint32_t num_grain_samples = 0;
	uint32_t sample_rate       = 0;
	uint32_t reserved[6]       = {};
};

struct Ngs2RackOption
{
	size_t   size                   = 0;
	char     name[16]               = {};
	uint32_t flags                  = 0;
	uint32_t max_grain_samples      = 0;
	uint32_t max_voices             = 0;
	uint32_t max_input_delay_blocks = 0;
	uint32_t max_matrices           = 0;
	uint32_t max_ports              = 0;
	uint32_t reserved[20]           = {};
};

struct Ngs2MasteringRackOption
{
	Ngs2RackOption rack_option;
	uint32_t       max_channels          = 0;
	uint32_t       num_peak_meter_blocks = 0;
};

struct Ngs2SubmixerRackOption
{
	Ngs2RackOption rack_option;
	uint32_t       max_channels          = 0;
	uint32_t       max_envelope_points   = 0;
	uint32_t       max_filters           = 0;
	uint32_t       max_inputs            = 0;
	uint32_t       num_peak_meter_blocks = 0;
};

struct Ngs2SamplerRackOption
{
	Ngs2RackOption rack_option;
	uint32_t       max_channel_works        = 0;
	uint32_t       max_codec_caches         = 0;
	uint32_t       max_waveform_blocks      = 0;
	uint32_t       max_envelope_points      = 0;
	uint32_t       max_filters              = 0;
	uint32_t       max_atrac9_decoders      = 0;
	uint32_t       max_atrac9_channel_works = 0;
	uint32_t       max_ajm_atrac9_decoders  = 0;
	uint32_t       num_peak_meter_blocks    = 0;
};

struct Ngs2ReverbRackOption
{
	Ngs2RackOption rack_option;
	uint32_t       max_channels = 0;
	uint32_t       reverb_size  = 0;
};

struct Ngs2CustomModuleOption
{
	uint32_t size = 0;
};

struct Ngs2CustomRackModuleInfo
{
	const Ngs2CustomModuleOption* option           = nullptr;
	uint32_t                      module_id        = 0;
	uint32_t                      source_buffer_id = 0;
	uint32_t                      extra_buffer_id  = 0;
	uint32_t                      dest_buffer_id   = 0;
	uint32_t                      state_offset     = 0;
	uint32_t                      state_size       = 0;
	uint32_t                      reserved         = 0;
	uint32_t                      reserved2        = 0;
};

struct Ngs2CustomRackPortInfo
{
	uint32_t source_buffer_id = 0;
	uint32_t reserved         = 0;
};

struct Ngs2CustomRackOption
{
	Ngs2RackOption           rack_option;
	uint32_t                 state_size  = 0;
	uint32_t                 num_buffers = 0;
	uint32_t                 num_modules = 0;
	uint32_t                 reserved    = 0;
	Ngs2CustomRackModuleInfo module[24];
	Ngs2CustomRackPortInfo   port[16];
};

struct Ngs2CustomSubmixerRackOption
{
	Ngs2CustomRackOption custom_rack_option;
	uint32_t             max_channels = 0;
	uint32_t             max_inputs   = 0;
};

union Ngs2RackOptionUnion
{
	Ngs2RackOption               common;
	Ngs2SamplerRackOption        sampler;
	Ngs2MasteringRackOption      mastering;
	Ngs2SubmixerRackOption       submixer;
	Ngs2ReverbRackOption         reverb;
	Ngs2CustomSubmixerRackOption custom_submixer;
};

struct Ngs2ContextBufferInfo
{
	void*     host_buffer      = nullptr;
	size_t    host_buffer_size = 0;
	uintptr_t reserved[5]      = {};
	uintptr_t user_data        = 0;
};

using Ngs2BufferAllocHandler = int32_t LUMEN_SYSV_ABI (*)(Ngs2ContextBufferInfo*);
using Ngs2BufferFreeHandler  = int32_t  LUMEN_SYSV_ABI (*)(Ngs2ContextBufferInfo*);

static int32_t InvokeNgs2BufferAllocHandler(Ngs2BufferAllocHandler handler, Ngs2ContextBufferInfo* buffer_info)
{
#if defined(LUMEN_EMU_HOST_ARM64)
	if (Loader::IsNativeHostCallTarget(reinterpret_cast<uint64_t>(handler))) return handler(buffer_info);
	return static_cast<int32_t>(Loader::RunGuestX64(reinterpret_cast<uint64_t>(handler),
	                                                reinterpret_cast<uint64_t>(buffer_info)));
#else
	return handler(buffer_info);
#endif
}

static int32_t InvokeNgs2BufferFreeHandler(Ngs2BufferFreeHandler handler, Ngs2ContextBufferInfo* buffer_info)
{
#if defined(LUMEN_EMU_HOST_ARM64)
	if (Loader::IsNativeHostCallTarget(reinterpret_cast<uint64_t>(handler))) return handler(buffer_info);
	return static_cast<int32_t>(Loader::RunGuestX64(reinterpret_cast<uint64_t>(handler), reinterpret_cast<uint64_t>(buffer_info)));
#else
	return handler(buffer_info);
#endif
}

struct Ngs2BufferAllocator
{
	Ngs2BufferAllocHandler alloc_handler = nullptr;
	Ngs2BufferFreeHandler  free_handler  = nullptr;
	uintptr_t              user_data     = 0;
};

template <class T>
static bool Ngs2ReadGuestObject(const T* source, T* owned)
{
	if (source == nullptr || owned == nullptr) return false;
	Core::VirtualMemory::GuestDataLease input;
	if (!BorrowAudioGuestRange(source, sizeof(T), Core::VirtualMemory::Mode::Read, &input)) return false;
	std::memcpy(owned, source, sizeof(T));
	return true;
}

static bool Ngs2SnapshotRackOption(uint32_t rack_id, const Ngs2RackOption* source, Ngs2RackOptionUnion* owned)
{
	size_t bytes = 0;
	switch (rack_id)
	{
		case 0x1000: bytes = sizeof(Ngs2SamplerRackOption); break;
		case 0x2000: bytes = sizeof(Ngs2SubmixerRackOption); break;
		case 0x2001: bytes = sizeof(Ngs2ReverbRackOption); break;
		case 0x3000: bytes = sizeof(Ngs2MasteringRackOption); break;
		case 0x4002: bytes = sizeof(Ngs2CustomSubmixerRackOption); break;
		default: return false;
	}
	Core::VirtualMemory::GuestDataLease input;
	if (!BorrowAudioGuestRange(source, bytes, Core::VirtualMemory::Mode::Read, &input)) return false;
	std::memcpy(owned, source, bytes);
	return true;
}

struct Ngs2Internal
{
	Ngs2SystemOption    option;
	Ngs2BufferAllocator allocator;
	Ngs2Internal*       next = nullptr;
	Core::Mutex         mutex;
	uintptr_t           guest_token = 0;
	size_t              guest_capacity = 0;
};

enum class Ngs2RackType
{
	Sampler,
	Submixer,
	Mastering,
	Reverb,
	CustomSubmixer,
};

struct Ngs2RackInternal
{
	Ngs2Internal*       ngs  = nullptr;
	Ngs2RackInternal*   next = nullptr;
	Ngs2RackType        type = Ngs2RackType::Sampler;
	Ngs2RackOptionUnion option;
	Ngs2BufferAllocator allocator;
	uintptr_t           guest_token = 0;
	size_t              guest_capacity = 0;
};

// Encoded bytes are copied and decoded during VoiceControl. The original base is
// an opaque identity for GetState, never a source for a later guest memory read.
struct Ngs2DecodedBlock
{
	std::vector<float> pcm;
	uint64_t           waveform_data = 0;
};

using Ngs2Atrac9Decoder = std::unique_ptr<void, decltype(&Atrac9ReleaseHandle)>;

struct Ngs2SamplerRuntime
{
	std::deque<Ngs2DecodedBlock> blocks;
	size_t                       cursor_frames = 0;
	size_t                       retained_pcm_samples = 0;
	// Owned decoder history follows the encoded end of the FIFO, independently
	// of its playback cursor. It is committed with each accepted full packet.
	Ngs2Atrac9Decoder             decoder {nullptr, Atrac9ReleaseHandle};
};

static bool Ngs2QueueDecodedBlock(Ngs2SamplerRuntime* runtime, Ngs2DecodedBlock&& block, uint32_t channels)
{
	constexpr size_t max_queued_blocks = 4096;
	if (runtime == nullptr || channels == 0 || block.pcm.empty() || block.pcm.size() % channels != 0 ||
	    block.pcm.size() > NGS2_AUDIO_RENDER_MAX_PCM_SAMPLES)
		return false;
	const bool replace_eof = runtime->blocks.size() == 1 &&
	                         runtime->cursor_frames == runtime->blocks.front().pcm.size() / channels;
	const size_t retained = replace_eof ? 0 : runtime->retained_pcm_samples;
	if ((!replace_eof && runtime->blocks.size() >= max_queued_blocks) ||
	    retained > NGS2_AUDIO_RENDER_MAX_PCM_SAMPLES - block.pcm.size())
		return false;
	const size_t new_samples = block.pcm.size();
	// Allocate before retiring anything so a rejected input cannot consume audio.
	runtime->blocks.push_back(std::move(block));
	if (replace_eof)
	{
		runtime->blocks.pop_front();
		runtime->cursor_frames = 0;
	}
	runtime->retained_pcm_samples = retained + new_samples;
	return true;
}

enum class Ngs2VoicePlayState
{
	Empty,
	Playing,
	Paused,
	Stopped
};

enum class Ngs2VoicePlayEvent
{
	None,
	Play,
	Pause,
	Resume,
	Stop,
	StopImm,
	Kill
};

struct Ngs2VoiceInternal
{
	Ngs2VoicePlayEvent event = Ngs2VoicePlayEvent::None;
	Ngs2VoicePlayState state = Ngs2VoicePlayState::Empty;
	Ngs2RackInternal*  rack  = nullptr;
};

struct Ngs2VoiceAudioState
{
	std::unique_ptr<Ngs2SamplerRuntime> sampler_runtime;
	uint32_t                            waveform_type        = 0;
	uint32_t                            waveform_channels    = 0;
	uint32_t                            waveform_sample_rate = 0;
	uint32_t                            waveform_config_data = 0;
};

struct Ngs2VoiceParamHeader
{
	uint16_t size;
	int16_t  next;
	uint32_t id;
};

struct Ngs2VoiceParamHeaderSnapshot
{
	uint16_t size = 0;
	int16_t  next = 0;
	uint32_t id   = 0;
};

static bool Ngs2ReadVoiceParamHeader(uint64_t address, Ngs2VoiceParamHeaderSnapshot* header)
{
	if (header == nullptr) return false;
	std::array<uint8_t, sizeof(Ngs2VoiceParamHeader)> bytes {};
	Core::VirtualMemory::GuestDataLease lease;
	if (!Core::VirtualMemory::TryAcquireGuestDataLease(
	        address, bytes.size(), Core::VirtualMemory::Mode::Read, &lease))
		return false;
	std::memcpy(bytes.data(), reinterpret_cast<const void*>(lease.HostBegin()), bytes.size());
	const uint16_t next = static_cast<uint16_t>(bytes[2]) |
	                      (static_cast<uint16_t>(bytes[3]) << 8u);
	header->size = static_cast<uint16_t>(bytes[0]) |
	               (static_cast<uint16_t>(bytes[1]) << 8u);
	header->next = next <= 0x7fffu ? static_cast<int16_t>(next)
	                              : static_cast<int16_t>(static_cast<int32_t>(next) - 0x10000);
	header->id = Ngs2LoadLe32(bytes.data() + 4);
	return true;
}

static uint64_t Ngs2LoadLe64(const uint8_t* bytes)
{
	return static_cast<uint64_t>(Ngs2LoadLe32(bytes)) |
	       (static_cast<uint64_t>(Ngs2LoadLe32(bytes + sizeof(uint32_t))) << 32u);
}

struct Ngs2VoiceEventParam
{
	Ngs2VoiceParamHeader header;
	uint32_t             event_id;
};

struct Ngs2SamplerFormatParam
{
	Ngs2VoiceParamHeader header;
	uint32_t             type;
	uint32_t             channels;
	uint32_t             sample_rate;
	uint32_t             config_data;
	uint32_t             frame_offset;
	uint32_t             frame_margin;
	uint64_t             reserved;
};

struct Ngs2SamplerBlockParam
{
	Ngs2VoiceParamHeader header;
	const void*          data;
	uint32_t             descriptor_count;
	uint32_t             field_14;
	const void*          descriptors;
};

struct Ngs2SamplerBlockDescriptor
{
	uint32_t data_offset;
	uint32_t encoded_size;
	uint32_t field_08;
	uint32_t skip_samples;
	uint64_t playable_samples;
	uint64_t user_data;
};

struct Ngs2RenderBufferSnapshot
{
	uint64_t data;
	uint64_t size;
	uint32_t type;
	uint32_t channels;
};

static_assert(sizeof(Ngs2SamplerFormatParam) == 40);
static_assert(sizeof(Ngs2SamplerBlockParam) == 32);
static_assert(sizeof(Ngs2SamplerBlockDescriptor) == 32);
static_assert(sizeof(Ngs2RenderBufferSnapshot) == 24);

static std::unordered_map<Ngs2VoiceInternal*, Ngs2VoiceAudioState> g_ngs2_voice_audio_states;

static Ngs2VoiceAudioState* Ngs2FindVoiceAudioState(Ngs2VoiceInternal* voice)
{
	const auto found = g_ngs2_voice_audio_states.find(voice);
	return found == g_ngs2_voice_audio_states.end() ? nullptr : &found->second;
}

// Temporary aggregate diagnostics. Callers already hold the registry/system
// locks; no guest identity or audio payload is retained or emitted here.
struct Ngs2AudioDiagnosticChannel
{
	uint64_t formats = 0, discarded_format_frames = 0, appends = 0, eof_replacements = 0, decoded_frames = 0;
	uint64_t encoded_bytes = 0, encoded_bytes_min = 0, encoded_bytes_max = 0;
	uint64_t superframes = 0, superframes_min = 0, superframes_max = 0;
	uint64_t skip_samples_min = 0, skip_samples_max = 0, skips_at_least_codec_frame = 0;
	uint64_t playable_samples_min = 0, playable_samples_max = 0, offset_nonzero = 0;
	uint64_t codec_frame_samples_min = 0, codec_frame_samples_max = 0;
	uint64_t eof = 0, play_rewinds = 0, discarded_event_frames = 0, overwritten_events = 0;
	uint64_t voice_grains = 0, no_fifo_grains = 0, silent_voice_grains = 0, mixed_frames = 0;
	std::array<uint64_t, 7> received {}, applied {};
};

struct Ngs2AudioDiagnostics
{
	uint64_t system = 0, grains = 0, output_frames = 0, silent_grains = 0, clipped_samples = 0, nonfinite_samples = 0;
	uint64_t voice_trace_records = 0, suppressed_voice_records = 0;
	std::unordered_map<Ngs2VoiceInternal*, uint64_t> voice_ordinals;
	uint32_t transitions = 0, suppressed_transitions = 0;
	float preclamp_peak = 0.0f;
	bool has_output = false, output_silent = true;
	std::array<Ngs2AudioDiagnosticChannel, 9> channels {};
};

static std::unordered_map<Ngs2Internal*, Ngs2AudioDiagnostics> g_ngs2_audio_diagnostics;
static uint64_t g_ngs2_audio_diagnostic_system = 0;

static bool Ngs2AudioDiagnosticsEnabled()
{
	static const bool enabled = AudioOutputTraceEnabled() || Config::DiagnosticsEnabled();
	return enabled;
}

static uint64_t Ngs2AudioDiagnosticTimeUs()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count());
}

static Ngs2AudioDiagnostics* Ngs2FindAudioDiagnostics(Ngs2Internal* ngs)
{
	if (!Ngs2AudioDiagnosticsEnabled()) return nullptr;
	const auto found = g_ngs2_audio_diagnostics.find(ngs);
	return found == g_ngs2_audio_diagnostics.end() ? nullptr : &found->second;
}

static Ngs2AudioDiagnosticChannel* Ngs2FindVoiceDiagnostics(Ngs2VoiceInternal* voice)
{
	if (!Ngs2AudioDiagnosticsEnabled() || voice == nullptr || voice->rack == nullptr ||
	    voice->rack->type != Ngs2RackType::Sampler) return nullptr;
	auto* diagnostics = Ngs2FindAudioDiagnostics(voice->rack->ngs);
	if (diagnostics == nullptr) return nullptr;
	const auto* audio = Ngs2FindVoiceAudioState(voice);
	const uint32_t channels = audio != nullptr ? audio->waveform_channels : 0;
	return &diagnostics->channels[channels < diagnostics->channels.size() ? channels : 0];
}

static uint64_t Ngs2DiagnosticPendingFrames(const Ngs2VoiceAudioState* audio)
{
	if (audio == nullptr || audio->sampler_runtime == nullptr || audio->waveform_channels == 0) return 0;
	const auto& runtime = *audio->sampler_runtime;
	return runtime.retained_pcm_samples / audio->waveform_channels - runtime.cursor_frames;
}

static void Ngs2TraceVoice(Ngs2VoiceInternal* voice, const char* action, bool rendering = false,
                          uint32_t descriptor_count = 0, bool opaque20_nonzero = false,
                          uint32_t skip_samples = 0, uint64_t playable_samples = 0,
                          Ngs2VoicePlayEvent prior_event = Ngs2VoicePlayEvent::None)
{
	if (voice == nullptr || voice->rack == nullptr || voice->rack->type != Ngs2RackType::Sampler) return;
	auto* diagnostics = Ngs2FindAudioDiagnostics(voice->rack->ngs);
	if (diagnostics == nullptr) return;
	// Bound both output and the side table. Ordinals describe this host session;
	// neither guest addresses nor waveform identities/payloads are emitted.
	constexpr uint64_t max_voice_records = 4096;
	if (diagnostics->voice_trace_records >= max_voice_records)
	{
		++diagnostics->suppressed_voice_records;
		return;
	}
	const auto [ordinal, inserted] = diagnostics->voice_ordinals.try_emplace(
	    voice, diagnostics->voice_ordinals.size() + 1);
	(void)inserted;
	const auto* audio = Ngs2FindVoiceAudioState(voice);
	const auto* runtime = audio != nullptr ? audio->sampler_runtime.get() : nullptr;
	const uint32_t channels = audio != nullptr ? audio->waveform_channels : 0;
	const uint64_t block_frames = runtime != nullptr && !runtime->blocks.empty() && channels != 0 ?
	                                  runtime->blocks.front().pcm.size() / channels : 0;
	const uint64_t cursor_frames = runtime != nullptr ? runtime->cursor_frames : 0;
	const bool at_eof = runtime != nullptr && runtime->blocks.size() == 1 && cursor_frames == block_frames;
	++diagnostics->voice_trace_records;
	AudioLog("[NGS2_DIAG_VOICE] system=%" PRIu64 " t_us=%" PRIu64 " grain=%" PRIu64
	         " voice=%" PRIu64 " action=%s state=%u event=%u prior_event=%u channels=%u"
	         " blocks=%zu cursor_frames=%" PRIu64 " block_frames=%" PRIu64 " pending_frames=%" PRIu64
	         " at_eof=%u descriptor_count=%u opaque20_nonzero=%u skip_samples=%u playable_samples=%" PRIu64 "\n",
	         diagnostics->system, Ngs2AudioDiagnosticTimeUs(), diagnostics->grains + (rendering ? 1 : 0),
	         ordinal->second, action, static_cast<unsigned>(voice->state), static_cast<unsigned>(voice->event),
	         static_cast<unsigned>(prior_event), channels, runtime != nullptr ? runtime->blocks.size() : 0,
	         cursor_frames, block_frames, Ngs2DiagnosticPendingFrames(audio), static_cast<unsigned>(at_eof),
	         descriptor_count, static_cast<unsigned>(opaque20_nonzero), skip_samples, playable_samples);
}

static void Ngs2ReportAudioDiagnostics(Ngs2AudioDiagnostics* diagnostics, const char* reason)
{
	if (diagnostics == nullptr) return;
	const uint64_t time_us = Ngs2AudioDiagnosticTimeUs();
	AudioLog("[NGS2_DIAG] system=%" PRIu64 " t_us=%" PRIu64 " grain=%" PRIu64 " reason=%s output_frames=%" PRIu64
	         " silent_grains=%" PRIu64 " preclamp_peak=%.9g clipped_samples=%" PRIu64 " nonfinite_samples=%" PRIu64
	         " suppressed_transitions=%u voice_trace_records=%" PRIu64 " suppressed_voice_records=%" PRIu64 "\n",
	         diagnostics->system, time_us, diagnostics->grains, reason,
	         diagnostics->output_frames, diagnostics->silent_grains, static_cast<double>(diagnostics->preclamp_peak),
	         diagnostics->clipped_samples, diagnostics->nonfinite_samples, diagnostics->suppressed_transitions,
	         diagnostics->voice_trace_records, diagnostics->suppressed_voice_records);
	for (uint32_t channels: {0u, 1u, 2u, 6u, 8u})
	{
		auto& c = diagnostics->channels[channels];
		uint64_t events = 0;
		for (uint32_t event = 1; event < c.received.size(); ++event) events += c.received[event] + c.applied[event];
		if (c.formats == 0 && c.appends == 0 && c.voice_grains == 0 && c.discarded_format_frames == 0 && events == 0) continue;
		AudioLog("[NGS2_DIAG_CHANNEL] system=%" PRIu64 " t_us=%" PRIu64 " grain=%" PRIu64 " channels=%u"
		         " formats=%" PRIu64 " format_discarded_frames=%" PRIu64 " appends=%" PRIu64 " eof_replacements=%" PRIu64
		         " decoded_frames=%" PRIu64 " encoded_bytes=%" PRIu64 " encoded_bytes_min=%" PRIu64 " encoded_bytes_max=%" PRIu64
		         " superframes=%" PRIu64 " superframes_min=%" PRIu64 " superframes_max=%" PRIu64
		         " skip_samples_min=%" PRIu64 " skip_samples_max=%" PRIu64 " skips_at_least_codec_frame=%" PRIu64
		         " codec_frame_samples_min=%" PRIu64 " codec_frame_samples_max=%" PRIu64
		         " playable_samples_min=%" PRIu64 " playable_samples_max=%" PRIu64 " offset_nonzero=%" PRIu64
		         " eof=%" PRIu64 " play_rewinds=%" PRIu64 " event_discarded_frames=%" PRIu64
		         " overwritten_events=%" PRIu64 " voice_grains=%" PRIu64 " no_fifo_grains=%" PRIu64
		         " silent_voice_grains=%" PRIu64 " mixed_frames=%" PRIu64
		         " received_play=%" PRIu64 " received_pause=%" PRIu64 " received_resume=%" PRIu64 " received_stop=%" PRIu64
		         " received_stop_imm=%" PRIu64 " received_kill=%" PRIu64
		         " applied_play=%" PRIu64 " applied_pause=%" PRIu64 " applied_resume=%" PRIu64 " applied_stop=%" PRIu64
		         " applied_stop_imm=%" PRIu64 " applied_kill=%" PRIu64 "\n",
		         diagnostics->system, time_us, diagnostics->grains, channels, c.formats, c.discarded_format_frames, c.appends,
		         c.eof_replacements, c.decoded_frames, c.encoded_bytes, c.encoded_bytes_min, c.encoded_bytes_max,
		         c.superframes, c.superframes_min, c.superframes_max, c.skip_samples_min, c.skip_samples_max,
		         c.skips_at_least_codec_frame, c.codec_frame_samples_min, c.codec_frame_samples_max,
		         c.playable_samples_min, c.playable_samples_max, c.offset_nonzero,
		         c.eof, c.play_rewinds, c.discarded_event_frames, c.overwritten_events,
		         c.voice_grains, c.no_fifo_grains, c.silent_voice_grains, c.mixed_frames,
		         c.received[1], c.received[2], c.received[3], c.received[4], c.received[5], c.received[6],
		         c.applied[1], c.applied[2], c.applied[3], c.applied[4], c.applied[5], c.applied[6]);
		c = {};
	}
	diagnostics->output_frames = diagnostics->silent_grains = diagnostics->clipped_samples = diagnostics->nonfinite_samples = 0;
	diagnostics->preclamp_peak = 0.0f;
	diagnostics->transitions = diagnostics->suppressed_transitions = 0;
}

static void Ngs2RecordAudioDiagnosticOutput(Ngs2AudioDiagnostics* diagnostics, uint32_t frames,
                                          const std::vector<float>& mix)
{
	if (diagnostics == nullptr) return;
	float peak = 0.0f;
	for (const float sample: mix)
	{
		if (!std::isfinite(sample)) ++diagnostics->nonfinite_samples;
		else
		{
			peak = std::max(peak, std::fabs(sample));
			if (std::fabs(sample) > 1.0f) ++diagnostics->clipped_samples;
		}
	}
	++diagnostics->grains;
	diagnostics->output_frames += frames;
	diagnostics->preclamp_peak = std::max(diagnostics->preclamp_peak, peak);
	const bool silent = peak <= 1.0e-6f;
	if (silent) ++diagnostics->silent_grains;
	if (!diagnostics->has_output || diagnostics->output_silent != silent)
	{
		if (diagnostics->transitions++ < 16)
			AudioLog("[NGS2_DIAG_OUTPUT] system=%" PRIu64 " t_us=%" PRIu64 " grain=%" PRIu64
			         " silent=%u preclamp_peak=%.9g\n", diagnostics->system, Ngs2AudioDiagnosticTimeUs(), diagnostics->grains,
			         static_cast<unsigned>(silent), static_cast<double>(peak));
		else ++diagnostics->suppressed_transitions;
	}
	diagnostics->has_output = true;
	diagnostics->output_silent = silent;
	if (diagnostics->grains % 128 == 0) Ngs2ReportAudioDiagnostics(diagnostics, "periodic");
}

static bool Ngs2InstallSamplerFormat(Ngs2VoiceInternal* voice, const Ngs2VoiceParamHeader* param,
                                     const uint8_t* owned_bytes = nullptr)
{
	if (voice == nullptr || param == nullptr || voice->rack == nullptr || voice->rack->ngs == nullptr)
		return false;
	std::array<uint8_t, sizeof(Ngs2SamplerFormatParam)> bytes {};
	if (owned_bytes != nullptr) std::memcpy(bytes.data(), owned_bytes, bytes.size());
	else if (!Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(param), bytes.data(), bytes.size())) return false;
	const uint16_t size          = static_cast<uint16_t>(bytes[0]) |
	                               (static_cast<uint16_t>(bytes[1]) << 8u);
	const uint32_t id            = Ngs2LoadLe32(bytes.data() + 4);
	const uint32_t type          = Ngs2LoadLe32(bytes.data() + 8);
	const uint32_t channels      = Ngs2LoadLe32(bytes.data() + 12);
	const uint32_t sample_rate   = Ngs2LoadLe32(bytes.data() + 16);
	const uint32_t config_data   = Ngs2LoadLe32(bytes.data() + 20);
	const uint32_t frame_offset  = Ngs2LoadLe32(bytes.data() + 24);
	const uint32_t frame_margin  = Ngs2LoadLe32(bytes.data() + 28);
	const uint64_t reserved      = Ngs2LoadLe64(bytes.data() + 32);
	Ngs2Atrac9Geometry geometry;
	if (size != sizeof(Ngs2SamplerFormatParam) || id != 0x10000000u ||
	    type != NGS2_WAVEFORM_TYPE_ATRAC9 ||
	    (channels != 1 && channels != 2 && channels != 6 && channels != 8) || sample_rate != 48000 ||
	    sample_rate != voice->rack->ngs->option.sample_rate || frame_offset != 0 || frame_margin != 0 ||
	    reserved != 0 ||
	    Ngs2GetAtrac9Geometry(config_data, channels, sample_rate, &geometry) !=
	        Ngs2WaveformFrameInfoResult::Ok)
		return false;

	auto& state                  = g_ngs2_voice_audio_states[voice];
	Ngs2TraceVoice(voice, "format_reset");
	if (auto* diagnostics = Ngs2FindVoiceDiagnostics(voice); diagnostics != nullptr)
		diagnostics->discarded_format_frames += Ngs2DiagnosticPendingFrames(&state);
	state.sampler_runtime.reset();
	state.waveform_type        = type;
	state.waveform_channels    = channels;
	state.waveform_sample_rate = sample_rate;
	state.waveform_config_data = config_data;
	if (auto* diagnostics = Ngs2FindVoiceDiagnostics(voice); diagnostics != nullptr) ++diagnostics->formats;
	Ngs2TraceVoice(voice, "format");
	return true;
}

static bool Ngs2UpsampleAtrac9Twofold(const std::vector<float>& source, uint32_t channels,
	                                   std::vector<float>* output)
{
	if (output == nullptr || channels == 0 || source.empty() || source.size() % channels != 0 ||
	    source.size() > NGS2_AUDIO_RENDER_MAX_PCM_SAMPLES / 2)
		return false;
	const size_t frames = source.size() / channels;
	std::vector<float> expanded(source.size() * 2);
	// The final half-frame holds the endpoint; no next block is available in this snapshot.
	for (size_t frame = 0; frame < frames; ++frame)
	{
		const size_t next = (std::min)(frame + 1, frames - 1);
		for (uint32_t channel = 0; channel < channels; ++channel)
		{
			const float current = source[frame * channels + channel];
			const float following = source[next * channels + channel];
			expanded[(frame * 2) * channels + channel] = current;
			expanded[(frame * 2 + 1) * channels + channel] = (current + following) * 0.5f;
		}
	}
	*output = std::move(expanded);
	return true;
}

static bool Ngs2DecodeAtrac9Snapshot(uint32_t config_data, uint32_t channels, uint32_t sample_rate,
                                    const std::vector<uint8_t>& encoded, uint64_t skip_samples,
                                    uint64_t playable_samples, std::vector<float>* output,
                                    Ngs2Atrac9Geometry* diagnostic_geometry = nullptr,
                                    const void* previous_decoder = nullptr,
                                    Ngs2Atrac9Decoder* decoded_history = nullptr)
{
	if (output == nullptr || playable_samples == 0 || channels == 0) return false;
	Ngs2Atrac9Geometry geometry;
	if (Ngs2GetAtrac9Geometry(config_data, channels, sample_rate, &geometry) !=
	        Ngs2WaveformFrameInfoResult::Ok ||
	    geometry.superframe_bytes == 0 || geometry.superframe_samples == 0 ||
	    geometry.codec_frames_per_superframe == 0 ||
	    encoded.empty() || encoded.size() % geometry.superframe_bytes != 0)
		return false;

	const uint64_t superframes = encoded.size() / geometry.superframe_bytes;
	if (superframes > UINT64_MAX / geometry.superframe_samples) return false;
	const uint64_t available_samples = superframes * geometry.superframe_samples;
	if (skip_samples > available_samples || playable_samples > available_samples - skip_samples ||
	    playable_samples > NGS2_AUDIO_RENDER_MAX_PCM_SAMPLES /
	                           (channels * geometry.output_rate_multiplier))
		return false;

	const uint32_t frame_samples = geometry.superframe_samples / geometry.codec_frames_per_superframe;
	if (frame_samples == 0 || frame_samples > SIZE_MAX / channels) return false;
	std::vector<float> frame_pcm(static_cast<size_t>(frame_samples) * channels);
	std::vector<float> decoded;
	decoded.reserve(static_cast<size_t>(playable_samples) * channels);

	std::array<unsigned char, ATRAC9_CONFIG_DATA_SIZE> config {{
	    static_cast<unsigned char>(config_data >> 24),
	    static_cast<unsigned char>(config_data >> 16),
	    static_cast<unsigned char>(config_data >> 8),
	    static_cast<unsigned char>(config_data)}};
	std::lock_guard library_lock(Ajm::Atrac9LibraryMutex());
	Ngs2Atrac9Decoder handle(previous_decoder != nullptr ? Atrac9CloneHandle(previous_decoder) : Atrac9GetHandle(),
	                        Atrac9ReleaseHandle);
	if (handle == nullptr) return false;
	if (previous_decoder != nullptr)
	{
		Atrac9CodecInfo codec_info {};
		if (Atrac9GetCodecInfo(handle.get(), &codec_info) != 0 ||
		    !std::equal(config.begin(), config.end(), codec_info.configData)) return false;
	} else if (Atrac9InitDecoder(handle.get(), config.data()) != 0)
	{
		return false;
	}

	uint64_t skip_remaining = skip_samples;
	uint64_t write_remaining = playable_samples;
	for (size_t superframe_offset = 0;
	     superframe_offset < encoded.size() && write_remaining != 0;
	     superframe_offset += geometry.superframe_bytes)
	{
		size_t cursor    = superframe_offset;
		size_t remaining = geometry.superframe_bytes;
		for (uint32_t frame = 0; frame < geometry.codec_frames_per_superframe; ++frame)
		{
			std::fill(frame_pcm.begin(), frame_pcm.end(), 0.0f);
			int bytes_used = 0;
			const int result = Atrac9DecodeF32(handle.get(), encoded.data() + cursor,
			                                   static_cast<int>(remaining), frame_pcm.data(),
			                                   &bytes_used, 0);
			if (result != 0 || bytes_used <= 0 || static_cast<size_t>(bytes_used) > remaining)
				return false;

			const uint64_t frame_skip = std::min<uint64_t>(skip_remaining, frame_samples);
			skip_remaining -= frame_skip;
			const uint64_t frame_available = frame_samples - frame_skip;
			const uint64_t frame_write = (std::min)(write_remaining, frame_available);
			if (frame_write != 0)
			{
				const size_t begin = static_cast<size_t>(frame_skip) * channels;
				const size_t count = static_cast<size_t>(frame_write) * channels;
				decoded.insert(decoded.end(), frame_pcm.begin() + static_cast<ptrdiff_t>(begin),
				               frame_pcm.begin() + static_cast<ptrdiff_t>(begin + count));
				write_remaining -= frame_write;
			}
			cursor += static_cast<size_t>(bytes_used);
			remaining -= static_cast<size_t>(bytes_used);
		}
	}

	if (skip_remaining != 0 || write_remaining != 0 ||
	    decoded.size() != static_cast<size_t>(playable_samples) * channels)
		return false;
	if (geometry.output_rate_multiplier == 2)
	{
		if (!Ngs2UpsampleAtrac9Twofold(decoded, channels, output)) return false;
	} else
	{
		*output = std::move(decoded);
	}
	if (diagnostic_geometry != nullptr) *diagnostic_geometry = geometry;
	if (decoded_history != nullptr)
	{
		// A trimmed packet does not describe continuity to its encoded end.
		// Its eager decoder may have advanced beyond the last playable sample.
		if (playable_samples == available_samples - skip_samples) *decoded_history = std::move(handle);
		else decoded_history->reset();
	}
	return true;
}

struct Ngs2SamplerBlockSnapshot
{
	std::array<uint8_t, sizeof(Ngs2SamplerBlockParam)> block_bytes {};
	uint32_t data_offset = 0, encoded_size = 0, skip_samples = 0;
	uint64_t playable_samples = 0;
	std::vector<uint8_t> encoded;
};

static bool Ngs2SnapshotSamplerBlock(uint64_t param_address, const uint8_t* owned_block,
								   Ngs2SamplerBlockSnapshot* snapshot)
{
	if (snapshot == nullptr) return false;
	std::array<uint8_t, sizeof(Ngs2SamplerBlockParam)> block_bytes {};
	std::array<uint8_t, sizeof(Ngs2SamplerBlockDescriptor)> descriptor_bytes {};
	uint32_t data_offset      = 0;
	uint32_t encoded_size     = 0;
	uint32_t skip_samples     = 0;
	uint64_t playable_samples = 0;
	std::vector<uint8_t> encoded;
	{
		Core::VirtualMemory::GuestDataLease block_lease;
		if (owned_block == nullptr && !Core::VirtualMemory::TryAcquireGuestDataLease(
		        param_address, block_bytes.size(),
		        Core::VirtualMemory::Mode::Read, &block_lease))
			return false;
		if (owned_block != nullptr) std::memcpy(block_bytes.data(), owned_block, block_bytes.size());
		else std::memcpy(block_bytes.data(), reinterpret_cast<const void*>(block_lease.HostBegin()), block_bytes.size());
		Core::VirtualMemory::ReleaseGuestDataLease(&block_lease);
		const uint16_t size               = static_cast<uint16_t>(block_bytes[0]) |
		                                    (static_cast<uint16_t>(block_bytes[1]) << 8u);
		const uint32_t id                 = Ngs2LoadLe32(block_bytes.data() + 4);
		const uint64_t data_address       = Ngs2LoadLe64(block_bytes.data() + 8);
		const uint32_t descriptor_count   = Ngs2LoadLe32(block_bytes.data() + 16);
		const uint64_t descriptor_address = Ngs2LoadLe64(block_bytes.data() + 24);
		if (size != sizeof(Ngs2SamplerBlockParam) || id != 0x10000001u ||
		    data_address == 0 || descriptor_address == 0 || descriptor_count != 1)
			return false;

		Core::VirtualMemory::GuestDataLease descriptor_lease;
		if (!Core::VirtualMemory::TryAcquireGuestDataLease(
		        descriptor_address, descriptor_bytes.size(), Core::VirtualMemory::Mode::Read,
		        &descriptor_lease))
			return false;
		std::memcpy(descriptor_bytes.data(),
		            reinterpret_cast<const void*>(descriptor_lease.HostBegin()), descriptor_bytes.size());
		Core::VirtualMemory::ReleaseGuestDataLease(&descriptor_lease);
		data_offset             = Ngs2LoadLe32(descriptor_bytes.data());
		encoded_size            = Ngs2LoadLe32(descriptor_bytes.data() + 4);
		const uint32_t field_08 = Ngs2LoadLe32(descriptor_bytes.data() + 8);
		skip_samples            = Ngs2LoadLe32(descriptor_bytes.data() + 12);
		playable_samples        = Ngs2LoadLe64(descriptor_bytes.data() + 16);
		if (field_08 != 0 || encoded_size == 0 || encoded_size > NGS2_AUDIO_RENDER_MAX_ENCODED_BYTES ||
		    data_address > UINT64_MAX - data_offset)
			return false;
		const uint64_t encoded_address = data_address + data_offset;
		Ngs2WaveformMemoryRange block_range;
		Ngs2WaveformMemoryRange descriptor_range;
		Ngs2WaveformMemoryRange encoded_range;
		if (!Ngs2MakeMemoryRange(param_address, block_bytes.size(), &block_range) ||
		    !Ngs2MakeMemoryRange(descriptor_address, descriptor_bytes.size(), &descriptor_range) ||
		    !Ngs2MakeMemoryRange(encoded_address, encoded_size, &encoded_range) ||
		    Ngs2MemoryRangesOverlap(block_range, descriptor_range) ||
		    Ngs2MemoryRangesOverlap(block_range, encoded_range) ||
		    Ngs2MemoryRangesOverlap(descriptor_range, encoded_range))
			return false;

		Core::VirtualMemory::GuestDataLease data_lease;
		if (!Core::VirtualMemory::TryAcquireGuestDataLease(
		        encoded_address, encoded_size, Core::VirtualMemory::Mode::Read, &data_lease))
			return false;
		encoded.resize(encoded_size);
		std::memcpy(encoded.data(), reinterpret_cast<const void*>(data_lease.HostBegin()), encoded.size());
	}
	snapshot->block_bytes = block_bytes;
	snapshot->data_offset = data_offset;
	snapshot->encoded_size = encoded_size;
	snapshot->skip_samples = skip_samples;
	snapshot->playable_samples = playable_samples;
	snapshot->encoded = std::move(encoded);
	return true;
}

static bool Ngs2InstallSamplerBlock(Ngs2VoiceInternal* voice, const Ngs2VoiceParamHeader* param,
									const Ngs2SamplerBlockSnapshot* prepared = nullptr)
{
	auto* state = Ngs2FindVoiceAudioState(voice);
	if (voice == nullptr || param == nullptr || voice->rack == nullptr || state == nullptr ||
	    state->waveform_type != NGS2_WAVEFORM_TYPE_ATRAC9)
		return false;

	Ngs2SamplerBlockSnapshot local;
	if (prepared == nullptr)
	{
		if (!Ngs2SnapshotSamplerBlock(reinterpret_cast<uint64_t>(param), nullptr, &local)) return false;
		prepared = &local;
	}
	const auto& block_bytes = prepared->block_bytes;
	const auto& encoded = prepared->encoded;
	const auto data_offset = prepared->data_offset;
	const auto encoded_size = prepared->encoded_size;
	const auto skip_samples = prepared->skip_samples;
	const auto playable_samples = prepared->playable_samples;
	const uint32_t max_blocks = voice->rack->option.sampler.max_waveform_blocks;
	if (max_blocks != 0 && Ngs2LoadLe32(block_bytes.data() + 16) > max_blocks) return false;
	Ngs2DecodedBlock candidate;
	auto* diagnostics = Ngs2FindVoiceDiagnostics(voice);
	Ngs2Atrac9Geometry diagnostic_geometry;
	const bool replace_eof = state->sampler_runtime != nullptr && state->sampler_runtime->blocks.size() == 1 &&
	                         state->sampler_runtime->cursor_frames ==
	                             state->sampler_runtime->blocks.front().pcm.size() / state->waveform_channels;
	// Nonzero skip provides this packet's own priming. An EOF replacement starts
	// a new source; only an unprimed append to the preserved FIFO carries history.
	const void* previous_decoder = skip_samples == 0 && !replace_eof && state->sampler_runtime != nullptr ?
	                                   state->sampler_runtime->decoder.get() : nullptr;
	Ngs2Atrac9Decoder decoded_history {nullptr, Atrac9ReleaseHandle};
	if (!Ngs2DecodeAtrac9Snapshot(state->waveform_config_data, state->waveform_channels,
	                              state->waveform_sample_rate, encoded, skip_samples,
	                              playable_samples, &candidate.pcm,
	                              diagnostics != nullptr ? &diagnostic_geometry : nullptr,
	                              previous_decoder, &decoded_history))
		return false;
	candidate.waveform_data = Ngs2LoadLe64(block_bytes.data() + 8);
	const size_t decoded_frames = diagnostics != nullptr ? candidate.pcm.size() / state->waveform_channels : 0;
	if (state->sampler_runtime != nullptr)
	{
		if (!Ngs2QueueDecodedBlock(state->sampler_runtime.get(), std::move(candidate), state->waveform_channels)) return false;
	} else
	{
		auto runtime = std::make_unique<Ngs2SamplerRuntime>();
		if (!Ngs2QueueDecodedBlock(runtime.get(), std::move(candidate), state->waveform_channels)) return false;
		state->sampler_runtime = std::move(runtime);
	}
	state->sampler_runtime->decoder = std::move(decoded_history);
	Ngs2TraceVoice(voice, "block", false, Ngs2LoadLe32(block_bytes.data() + 16),
	               Ngs2LoadLe32(block_bytes.data() + 20) != 0, skip_samples, playable_samples);
	if (diagnostics != nullptr)
	{
		const bool first_append = diagnostics->appends == 0;
		const uint64_t superframes = encoded_size / diagnostic_geometry.superframe_bytes;
		const uint64_t codec_frame_samples = diagnostic_geometry.superframe_samples /
		                                     diagnostic_geometry.codec_frames_per_superframe;
		auto record_range = [first_append](uint64_t value, uint64_t& minimum, uint64_t& maximum)
		{
			minimum = first_append ? value : std::min(minimum, value);
			maximum = first_append ? value : std::max(maximum, value);
		};
		diagnostics->encoded_bytes += encoded_size;
		diagnostics->superframes += superframes;
		record_range(encoded_size, diagnostics->encoded_bytes_min, diagnostics->encoded_bytes_max);
		record_range(superframes, diagnostics->superframes_min, diagnostics->superframes_max);
		record_range(skip_samples, diagnostics->skip_samples_min, diagnostics->skip_samples_max);
		record_range(playable_samples, diagnostics->playable_samples_min, diagnostics->playable_samples_max);
		record_range(codec_frame_samples, diagnostics->codec_frame_samples_min, diagnostics->codec_frame_samples_max);
		if (skip_samples >= codec_frame_samples) ++diagnostics->skips_at_least_codec_frame;
		if (data_offset != 0) ++diagnostics->offset_nonzero;
		++diagnostics->appends;
		diagnostics->decoded_frames += decoded_frames;
		if (replace_eof) ++diagnostics->eof_replacements;
	}
	return true;
}

struct Ngs2VoicePatchParam
{
	Ngs2VoiceParamHeader header;
	uint32_t             port;
	uint32_t             dest_input_id;
	uintptr_t            dest_handle;
};

struct Ngs2VoicePortMatrixParam
{
	Ngs2VoiceParamHeader header;
	uint32_t             port;
	int32_t              matrix_id;
};

struct Ngs2VoiceState
{
	uint32_t state_flags;
};

struct Ngs2SamplerVoiceState
{
	Ngs2VoiceState voice_state;
	float          envelope_height;
	float          peak_height;
	uint32_t       reserved;
	uint64_t       num_decoded_samples;
	uint64_t       decoded_data_size;
	uint64_t       user_data;
	const void*    waveform_data;
};

struct Ngs2MasteringVoiceState
{
	Ngs2VoiceState voice_state;
	uint8_t        opaque[72];
};

static_assert(sizeof(Ngs2MasteringVoiceState) == 76);

static Ngs2Internal*     g_ngs_list   = nullptr;
static Ngs2RackInternal* g_racks_list = nullptr;
static std::mutex        g_ngs_registry_mutex;

static bool Ngs2VoiceStateFlagsValue(Ngs2VoicePlayState state, uint32_t* flags)
{
	if (flags == nullptr) return false;
	switch (state)
	{
		case Ngs2VoicePlayState::Empty: *flags = 0; return true;
		case Ngs2VoicePlayState::Playing: *flags = 0x3; return true;
		case Ngs2VoicePlayState::Paused: *flags = 0x5; return true;
		case Ngs2VoicePlayState::Stopped: *flags = 0xb; return true;
	}
	return false;
}

static Ngs2VoicePlayState Ngs2VisibleVoiceState(const Ngs2VoiceInternal* voice)
{
	// The streaming producer queries immediately after Play. Report the
	// requested Playing state while the render event is pending, without
	// consuming the event or advancing the PCM cursor.
	if (Ngs2AudioRenderExperimentalEnabled() && voice->state == Ngs2VoicePlayState::Empty &&
	    voice->event == Ngs2VoicePlayEvent::Play)
		return Ngs2VoicePlayState::Playing;
	return voice->state;
}

static bool Ngs2SystemRegisteredLocked(const Ngs2Internal* expected)
{
	for (const auto* ngs = g_ngs_list; ngs != nullptr; ngs = ngs->next)
		if (ngs == expected) return true;
	return false;
}

static Ngs2Internal* Ngs2FindSystemLocked(uintptr_t token)
{
	if (token == 0) return nullptr;
	for (auto* ngs = g_ngs_list; ngs != nullptr; ngs = ngs->next)
		if (ngs->guest_token == token) return ngs;
	return nullptr;
}

static Ngs2RackInternal* Ngs2FindRackLocked(uintptr_t token)
{
	if (token == 0) return nullptr;
	for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next)
		if (rack->guest_token == token) return rack;
	return nullptr;
}

static bool Ngs2RackStorageRange(const Ngs2RackInternal* rack, Ngs2WaveformMemoryRange* range)
{
	if (rack == nullptr || range == nullptr) return false;
	const size_t voices = rack->option.common.max_voices;
	if (voices > (SIZE_MAX - sizeof(Ngs2RackInternal)) / sizeof(Ngs2VoiceInternal)) return false;
	return Ngs2MakeMemoryRange(rack->guest_token,
	                          sizeof(Ngs2RackInternal) + voices * sizeof(Ngs2VoiceInternal), range);
}

[[maybe_unused]] static bool Ngs2OutputAliasesLiveMemoryLocked(const Ngs2WaveformMemoryRange& output, bool* registry_valid)
{
	if (registry_valid == nullptr) return true;
	*registry_valid = true;
	for (const auto* ngs = g_ngs_list; ngs != nullptr; ngs = ngs->next)
	{
		Ngs2WaveformMemoryRange range;
		if (!Ngs2MakeMemoryRange(ngs->guest_token, sizeof(*ngs), &range))
		{
			*registry_valid = false;
			return true;
		}
		if (Ngs2MemoryRangesOverlap(output, range)) return true;
	}
	for (const auto* rack = g_racks_list; rack != nullptr; rack = rack->next)
	{
		Ngs2WaveformMemoryRange range;
		if (!Ngs2RackStorageRange(rack, &range))
		{
			*registry_valid = false;
			return true;
		}
		if (Ngs2MemoryRangesOverlap(output, range)) return true;
	}
	return false;
}

[[maybe_unused]] static Ngs2VoiceInternal* Ngs2FindVoiceLocked(uintptr_t voice_handle, Ngs2RackInternal** owner)
{
	if (owner != nullptr) *owner = nullptr;
	if (voice_handle == 0) return nullptr;
	for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next)
	{
		if (rack->ngs == nullptr || !Ngs2SystemRegisteredLocked(rack->ngs)) continue;
		const size_t voices = rack->option.common.max_voices;
		if (voices == 0 || voices > (SIZE_MAX - sizeof(Ngs2RackInternal)) / sizeof(Ngs2VoiceInternal)) continue;
		if (rack->guest_token > UINTPTR_MAX - sizeof(Ngs2RackInternal)) continue;
		const uintptr_t begin = rack->guest_token + sizeof(Ngs2RackInternal);
		if (voice_handle < begin) continue;
		const uintptr_t delta = voice_handle - begin;
		if (delta % sizeof(Ngs2VoiceInternal) != 0 || delta / sizeof(Ngs2VoiceInternal) >= voices) continue;
		auto* voice = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1) + delta / sizeof(Ngs2VoiceInternal);
		if (voice->rack != rack) return nullptr;
		if (owner != nullptr) *owner = rack;
		return voice;
	}
	return nullptr;
}

static bool Ngs2PrepareRenderBufferLocked(const Ngs2Internal* ngs, const Ngs2RenderBufferInfo* buffer_info,
	                                      uint32_t num_buffer_info, Ngs2RenderBufferSnapshot* descriptor,
	                                      std::vector<float>* mix,
	                                      Core::VirtualMemory::GuestDataLease* output_lease,
	                                      const Ngs2RenderBufferSnapshot* prepared = nullptr)
{
	if (ngs == nullptr || buffer_info == nullptr || descriptor == nullptr || mix == nullptr || output_lease == nullptr ||
	    num_buffer_info != 1 || ngs->option.num_grain_samples != 512 || ngs->option.sample_rate != 48000)
		return false;
	std::array<uint8_t, sizeof(Ngs2RenderBufferSnapshot)> bytes {};
	if (prepared == nullptr)
	{
		Core::VirtualMemory::GuestDataLease descriptor_lease;
		if (!Core::VirtualMemory::TryAcquireGuestDataLease(
		        reinterpret_cast<uint64_t>(buffer_info), bytes.size(), Core::VirtualMemory::Mode::Read,
		        &descriptor_lease))
			return false;
		std::memcpy(bytes.data(), reinterpret_cast<const void*>(descriptor_lease.HostBegin()), bytes.size());
		descriptor->data     = Ngs2LoadLe64(bytes.data());
		descriptor->size     = Ngs2LoadLe64(bytes.data() + 8);
		descriptor->type     = Ngs2LoadLe32(bytes.data() + 16);
		descriptor->channels = Ngs2LoadLe32(bytes.data() + 20);
	} else
	{
		*descriptor = *prepared;
	}
	if (descriptor->type != NGS2_RENDER_WAVEFORM_TYPE_FLOAT32 || descriptor->channels != 8 ||
	    ngs->option.num_grain_samples > SIZE_MAX / descriptor->channels ||
	    static_cast<size_t>(ngs->option.num_grain_samples) * descriptor->channels > SIZE_MAX / sizeof(float))
		return false;
	const size_t samples = static_cast<size_t>(ngs->option.num_grain_samples) * descriptor->channels;
	const size_t expected_size = samples * sizeof(float);
	if (descriptor->size != expected_size) return false;
	if (prepared == nullptr)
	{
		if (!Core::VirtualMemory::TryAcquireGuestDataLease(
		        descriptor->data, expected_size, Core::VirtualMemory::Mode::Write, output_lease)) return false;
	} else if (!output_lease->IsActive() || output_lease->HostBegin() != descriptor->data ||
	           output_lease->ByteCount() != expected_size) return false;
	Ngs2WaveformMemoryRange output_range;
	Ngs2WaveformMemoryRange descriptor_range;
	bool                    registry_valid = false;
	if (!Ngs2MakeMemoryRange(reinterpret_cast<uint64_t>(buffer_info), bytes.size(), &descriptor_range) ||
	    !Ngs2MakeMemoryRange(descriptor->data, expected_size, &output_range) ||
	    Ngs2MemoryRangesOverlap(descriptor_range, output_range) ||
	    Ngs2OutputAliasesLiveMemoryLocked(output_range, &registry_valid))
		return false;
	mix->assign(samples, 0.0f);
	return true;
}

static void Ngs2ApplyAudioRenderEvent(Ngs2VoiceInternal* voice)
{
	if (voice == nullptr) return;
	auto* audio_state = Ngs2FindVoiceAudioState(voice);
	auto* diagnostics = Ngs2FindVoiceDiagnostics(voice);
	if (diagnostics != nullptr && voice->event != Ngs2VoicePlayEvent::None)
		++diagnostics->applied[static_cast<size_t>(voice->event)];
	switch (voice->event)
	{
		case Ngs2VoicePlayEvent::None: break;
		case Ngs2VoicePlayEvent::Play:
			if (audio_state != nullptr && audio_state->sampler_runtime != nullptr &&
			    audio_state->waveform_channels != 0 && audio_state->sampler_runtime->blocks.size() == 1 &&
			    audio_state->sampler_runtime->cursor_frames ==
			        audio_state->sampler_runtime->blocks.front().pcm.size() / audio_state->waveform_channels)
			{
				audio_state->sampler_runtime->cursor_frames = 0;
				if (diagnostics != nullptr) ++diagnostics->play_rewinds;
			}
			voice->state = Ngs2VoicePlayState::Playing;
			break;
		case Ngs2VoicePlayEvent::Pause:
			if (voice->state == Ngs2VoicePlayState::Playing) voice->state = Ngs2VoicePlayState::Paused;
			break;
		case Ngs2VoicePlayEvent::Resume:
			if (voice->state == Ngs2VoicePlayState::Paused) voice->state = Ngs2VoicePlayState::Playing;
			break;
		case Ngs2VoicePlayEvent::Stop:
			if (voice->state == Ngs2VoicePlayState::Playing || voice->state == Ngs2VoicePlayState::Paused)
				voice->state = Ngs2VoicePlayState::Stopped;
			break;
		case Ngs2VoicePlayEvent::StopImm:
		case Ngs2VoicePlayEvent::Kill:
			voice->state = Ngs2VoicePlayState::Empty;
			if (diagnostics != nullptr) diagnostics->discarded_event_frames += Ngs2DiagnosticPendingFrames(audio_state);
			if (audio_state != nullptr) audio_state->sampler_runtime.reset();
			break;
	}
	if (voice->event != Ngs2VoicePlayEvent::None) Ngs2TraceVoice(voice, "applied", true);
	voice->event = Ngs2VoicePlayEvent::None;
}

static void Ngs2MixSamplerVoice(Ngs2VoiceInternal* voice, uint32_t grain_frames,
	                            uint32_t output_channels, std::vector<float>* mix)
{
	auto* audio_state = Ngs2FindVoiceAudioState(voice);
	auto* diagnostics = Ngs2FindVoiceDiagnostics(voice);
	if (diagnostics != nullptr && voice->state == Ngs2VoicePlayState::Playing)
	{
		++diagnostics->voice_grains;
		if (audio_state == nullptr || audio_state->sampler_runtime == nullptr ||
		    audio_state->sampler_runtime->blocks.empty() || audio_state->waveform_channels == 0)
		{
			++diagnostics->no_fifo_grains;
		}
	}
	if (voice == nullptr || mix == nullptr || voice->state != Ngs2VoicePlayState::Playing ||
	    audio_state == nullptr || audio_state->sampler_runtime == nullptr ||
	    audio_state->waveform_channels == 0)
	{
		if (diagnostics != nullptr && voice->state == Ngs2VoicePlayState::Playing) ++diagnostics->silent_voice_grains;
		return;
	}
	auto& runtime = *audio_state->sampler_runtime;
	size_t mixed_frames = 0;
	float source_peak = 0.0f;
	while (!runtime.blocks.empty() && mixed_frames < grain_frames)
	{
		const auto& block = runtime.blocks.front();
		const size_t total_frames = block.pcm.size() / audio_state->waveform_channels;
		const size_t frames = std::min<size_t>(grain_frames - mixed_frames, total_frames - runtime.cursor_frames);
		if (diagnostics != nullptr)
		{
			const size_t begin = runtime.cursor_frames * audio_state->waveform_channels;
			const size_t end = begin + frames * audio_state->waveform_channels;
			for (size_t sample = begin; sample < end; ++sample)
				if (std::isfinite(block.pcm[sample])) source_peak = std::max(source_peak, std::fabs(block.pcm[sample]));
		}
		for (size_t frame = 0; frame < frames; ++frame)
		{
			const size_t source = (runtime.cursor_frames + frame) * audio_state->waveform_channels;
			const size_t destination = (mixed_frames + frame) * output_channels;
			if (audio_state->waveform_channels == 1 && output_channels >= 2)
			{
				const float sample = block.pcm[source];
				(*mix)[destination] += sample;
				(*mix)[destination + 1] += sample;
			} else
			{
				const uint32_t channels = (std::min)(audio_state->waveform_channels, output_channels);
				for (uint32_t channel = 0; channel < channels; ++channel)
					(*mix)[destination + channel] += block.pcm[source + channel];
			}
		}
		runtime.cursor_frames += frames;
		mixed_frames += frames;
		if (runtime.cursor_frames == total_frames)
		{
			if (runtime.blocks.size() == 1)
			{
				// Retain the complete final block for an explicit Play after EOF.
				voice->state = Ngs2VoicePlayState::Empty;
				if (diagnostics != nullptr) ++diagnostics->eof;
				Ngs2TraceVoice(voice, "eof", true);
				break;
			}
			runtime.retained_pcm_samples -= block.pcm.size();
			runtime.blocks.pop_front();
			runtime.cursor_frames = 0;
			Ngs2TraceVoice(voice, "next_block", true);
		}
	}
	if (diagnostics != nullptr)
	{
		diagnostics->mixed_frames += mixed_frames;
		if (source_peak <= 1.0e-6f) ++diagnostics->silent_voice_grains;
	}
}

static bool Ngs2RenderAudioLocked(Ngs2Internal* ngs, const Ngs2RenderBufferInfo* buffer_info,
	                              uint32_t num_buffer_info,
	                              const Ngs2RenderBufferSnapshot* prepared = nullptr,
	                              Core::VirtualMemory::GuestDataLease* prepared_output = nullptr)
{
	Ngs2RenderBufferSnapshot descriptor {};
	std::vector<float>       mix;
	Core::VirtualMemory::GuestDataLease local_output;
	auto* output_lease = prepared_output != nullptr ? prepared_output : &local_output;
	if (!Ngs2PrepareRenderBufferLocked(ngs, buffer_info, num_buffer_info, &descriptor, &mix,
	                                   output_lease, prepared))
		return false;
	for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next)
	{
		if (rack->ngs != ngs) continue;
		auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);
		for (uint32_t index = 0; index < rack->option.common.max_voices; ++index)
		{
			auto& voice = voices[index];
			Ngs2ApplyAudioRenderEvent(&voice);
			if (rack->type == Ngs2RackType::Sampler)
				Ngs2MixSamplerVoice(&voice, ngs->option.num_grain_samples, descriptor.channels, &mix);
		}
	}
	Ngs2RecordAudioDiagnosticOutput(Ngs2FindAudioDiagnostics(ngs), ngs->option.num_grain_samples, mix);
	for (float& sample: mix)
	{
		if (!std::isfinite(sample)) sample = 0.0f;
		sample = std::clamp(sample, -1.0f, 1.0f);
	}
	std::memcpy(reinterpret_cast<void*>(output_lease->HostBegin()), mix.data(), mix.size() * sizeof(float));
	return true;
}

static const char* Ngs2VoiceStateFlagsResultName(Ngs2VoiceStateFlagsResult result)
{
	switch (result)
	{
		case Ngs2VoiceStateFlagsResult::Ok: return "complete-local";
		case Ngs2VoiceStateFlagsResult::Disabled: return "disabled";
		case Ngs2VoiceStateFlagsResult::InvalidHandle: return "voice-handle-invalid";
		case Ngs2VoiceStateFlagsResult::InvalidState: return "voice-state-invalid";
		case Ngs2VoiceStateFlagsResult::InvalidOutput: return "state-flags-unwritable";
		case Ngs2VoiceStateFlagsResult::AliasedMemory: return "ngs2-memory-alias-unsupported";
	}
	return "unknown";
}

static Ngs2VoiceStateFlagsResult Ngs2VoiceGetStateFlagsModelImpl(
	uintptr_t voice_handle, uint64_t state_flags_address, bool require_selector)
{
#if (LUMEN_PLATFORM != LUMEN_PLATFORM_OSX && \
     LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID) || \
    LUMEN_ABI != LUMEN_ABI_ARM64_V8A
	(void)voice_handle;
	(void)state_flags_address;
	(void)require_selector;
	return Ngs2VoiceStateFlagsResult::Disabled;
#else
	if (require_selector && !Ngs2VoiceStateFlagsExperimentalEnabled())
		return Ngs2VoiceStateFlagsResult::Disabled;
	Ngs2WaveformMemoryRange output_range;
	Core::VirtualMemory::GuestDataLease output_lease;
	if (!Ngs2MakeMemoryRange(state_flags_address, sizeof(uint32_t), &output_range) ||
	    !Core::VirtualMemory::TryAcquireGuestDataLease(
	        state_flags_address, sizeof(uint32_t), Core::VirtualMemory::Mode::Write, &output_lease))
		return Ngs2VoiceStateFlagsResult::InvalidOutput;

	std::lock_guard registry_lock(g_ngs_registry_mutex);
	bool            registry_valid = false;
	if (Ngs2OutputAliasesLiveMemoryLocked(output_range, &registry_valid))
		return registry_valid ? Ngs2VoiceStateFlagsResult::AliasedMemory
		                      : Ngs2VoiceStateFlagsResult::InvalidHandle;

	Ngs2RackInternal* owner = nullptr;
	Ngs2VoiceInternal* voice = Ngs2FindVoiceLocked(voice_handle, &owner);
	if (voice == nullptr || owner == nullptr || owner->ngs == nullptr)
		return Ngs2VoiceStateFlagsResult::InvalidHandle;
	Ngs2Internal* ngs = owner->ngs;
	Core::LockGuard system_lock(ngs->mutex);

	Ngs2RackInternal* confirmed_owner = nullptr;
	Ngs2VoiceInternal* confirmed_voice = Ngs2FindVoiceLocked(voice_handle, &confirmed_owner);
	if (confirmed_voice != voice || confirmed_owner != owner || confirmed_owner->ngs != ngs)
		return Ngs2VoiceStateFlagsResult::InvalidHandle;

	uint32_t state_flags = 0;
	if (!Ngs2VoiceStateFlagsValue(Ngs2VisibleVoiceState(voice), &state_flags))
		return Ngs2VoiceStateFlagsResult::InvalidState;
	const auto bytes = Ngs2StoreLe32(state_flags);
	std::memcpy(reinterpret_cast<void*>(output_lease.HostBegin()), bytes.data(), bytes.size());
	return Ngs2VoiceStateFlagsResult::Ok;
#endif
}

Ngs2VoiceStateFlagsResult Ngs2VoiceGetStateFlagsModel(uintptr_t voice_handle, uint64_t state_flags_address)
{
	return Ngs2VoiceGetStateFlagsModelImpl(voice_handle, state_flags_address, true);
}

int32_t LUMEN_SYSV_ABI Ngs2VoiceGetStateFlags(uintptr_t voice_handle, uint32_t* state_flags)
{
	PRINT_NAME();
	const auto result = Ngs2VoiceGetStateFlagsModel(voice_handle, reinterpret_cast<uint64_t>(state_flags));
	// This query runs for every grain. Keep a success marker without doing
	// synchronous stderr I/O for every realtime state check.
	static std::atomic<bool> logged_success {false};
	if (result != Ngs2VoiceStateFlagsResult::Ok || !logged_success.exchange(true, std::memory_order_relaxed))
	{
		std::fprintf(stderr,
		             "[NGS2_VOICE_STATE_FLAGS_LOCAL_EXPERIMENT] result=%s experimental=1 "
		             "console_contract=unverified profile=pending-play-visible-state\n",
		             Ngs2VoiceStateFlagsResultName(result));
		std::fflush(stderr);
	}
	if (result != Ngs2VoiceStateFlagsResult::Ok)
	{
		EXIT("NGS2 VoiceGetStateFlags local experiment cannot model request: %s",
		     Ngs2VoiceStateFlagsResultName(result));
		std::abort();
	}
	return 0;
}

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
static int g_ngs_test_allocations = 0;
static int g_ngs_test_frees       = 0;
static bool g_ngs_test_controls_unchanged = true;

static int32_t LUMEN_SYSV_ABI Ngs2SessionResetTestAlloc(Ngs2ContextBufferInfo* buffer)
{
	if (buffer == nullptr || buffer->host_buffer_size == 0)
	{
		return -1;
	}
	buffer->host_buffer = new uint8_t[buffer->host_buffer_size];
	std::memset(buffer->host_buffer, 0xa5, buffer->host_buffer_size);
	g_ngs_test_allocations++;
	return OK;
}

static int32_t LUMEN_SYSV_ABI Ngs2SessionResetTestFree(Ngs2ContextBufferInfo* buffer)
{
	if (buffer == nullptr || buffer->host_buffer == nullptr)
	{
		return -1;
	}
	const auto* bytes = static_cast<const uint8_t*>(buffer->host_buffer);
	g_ngs_test_controls_unchanged = g_ngs_test_controls_unchanged &&
	    std::all_of(bytes, bytes + buffer->host_buffer_size, [](uint8_t value) { return value == 0xa5; });
	delete[] static_cast<uint8_t*>(buffer->host_buffer);
	buffer->host_buffer = nullptr;
	g_ngs_test_frees++;
	return OK;
}
#endif

int LUMEN_SYSV_ABI Ngs2RackQueryBufferSize(uint32_t rack_id, const Ngs2RackOption* option, Ngs2ContextBufferInfo* buffer_info)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(option == nullptr);
	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr);
	Ngs2RackOption owned_option {};
	if (!Ngs2ReadGuestObject(option, &owned_option)) return -1;
	option = &owned_option;
	Core::VirtualMemory::GuestDataLease output;
	if (!BorrowAudioGuestRange(&buffer_info->host_buffer_size, sizeof(buffer_info->host_buffer_size),
	                          Core::VirtualMemory::Mode::Write, &output)) return -1;

	printf("\t rack_id    = 0x%" PRIx32 "\n", rack_id);
	printf("\t max_voices = %u\n", option->max_voices);

	const size_t max_voices = option->max_voices;
	EXIT_NOT_IMPLEMENTED(max_voices >
	                     (SIZE_MAX - sizeof(Ngs2RackInternal)) / sizeof(Ngs2VoiceInternal));
	buffer_info->host_buffer_size = sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal) * max_voices;

	return OK;
}

int LUMEN_SYSV_ABI Ngs2SystemCreateWithAllocator(const Ngs2SystemOption* option, const Ngs2BufferAllocator* allocator, uintptr_t* handle)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(option == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator == nullptr);
	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	Ngs2SystemOption owned_option {};
	Ngs2BufferAllocator owned_allocator {};
	if (!Ngs2ReadGuestObject(option, &owned_option) || !Ngs2ReadGuestObject(allocator, &owned_allocator)) return -1;
	option = &owned_option;
	allocator = &owned_allocator;
	EXIT_NOT_IMPLEMENTED(allocator->alloc_handler == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator->free_handler == nullptr);

	EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SystemOption));

	printf("\t name              = %.16s\n", option->name);
	printf("\t flags             = %u\n", option->flags);
	printf("\t max_grain_samples = %u\n", option->max_grain_samples);
	printf("\t num_grain_samples = %u\n", option->num_grain_samples);
	printf("\t sample_rate       = %u\n", option->sample_rate);
	printf("\t alloc_handler     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(allocator->alloc_handler));
	printf("\t free_handler      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(allocator->free_handler));
	printf("\t user_data         = 0x%016" PRIx64 "\n", static_cast<uint64_t>(reinterpret_cast<uintptr_t>(allocator->user_data)));

	Ngs2ContextBufferInfo buf {};
	buf.host_buffer      = nullptr;
	buf.host_buffer_size = sizeof(Ngs2Internal);
	buf.user_data        = allocator->user_data;

	int result = InvokeNgs2BufferAllocHandler(allocator->alloc_handler, &buf);

	EXIT_NOT_IMPLEMENTED(result != OK);
	EXIT_NOT_IMPLEMENTED(buf.host_buffer == nullptr);

	// Guest allocator storage supplies the opaque identity required by the API.
	// Mutexes, lists and voice state are native-owned and never dereference that
	// persistent guest address after registration.
	auto* ngs = new Ngs2Internal {};
	ngs->guest_token = reinterpret_cast<uintptr_t>(buf.host_buffer);
	ngs->guest_capacity = buf.host_buffer_size;

	ngs->option    = *option;
	ngs->allocator = *allocator;
	Core::VirtualMemory::GuestDataLease output;
	if (!BorrowAudioGuestRange(handle, sizeof(*handle), Core::VirtualMemory::Mode::Write, &output))
	{
		delete ngs;
		(void)InvokeNgs2BufferFreeHandler(allocator->free_handler, &buf);
		return -1;
	}

	{
		std::lock_guard registry_lock(g_ngs_registry_mutex);
		EXIT_NOT_IMPLEMENTED(Ngs2FindSystemLocked(ngs->guest_token) != nullptr);
		ngs->next  = g_ngs_list;
		g_ngs_list = ngs;
		if (Ngs2AudioDiagnosticsEnabled())
			g_ngs2_audio_diagnostics[ngs].system = ++g_ngs2_audio_diagnostic_system;
	}

	*handle = ngs->guest_token;

	return OK;
}

int LUMEN_SYSV_ABI Ngs2RackCreate(uintptr_t system_handle, uint32_t rack_id, const Ngs2RackOption* option,
                                 const Ngs2ContextBufferInfo* buffer_info, uintptr_t* handle)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(option == nullptr);
	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr);
	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	Ngs2RackOptionUnion owned_option {};
	Ngs2ContextBufferInfo owned_buffer {};
	if (!Ngs2SnapshotRackOption(rack_id, option, &owned_option) || !Ngs2ReadGuestObject(buffer_info, &owned_buffer)) return -1;
	option = &owned_option.common;
	buffer_info = &owned_buffer;
	Core::VirtualMemory::GuestDataLease output;
	if (!BorrowAudioGuestRange(handle, sizeof(*handle), Core::VirtualMemory::Mode::Write, &output)) return -1;
	EXIT_NOT_IMPLEMENTED(buffer_info->host_buffer == nullptr);
	EXIT_NOT_IMPLEMENTED(buffer_info->host_buffer_size == 0);
	EXIT_NOT_IMPLEMENTED(system_handle == 0);

	EXIT_NOT_IMPLEMENTED(option->size < sizeof(Ngs2RackOption));

	printf("\t rack_id                = 0x%" PRIx32 "\n", rack_id);
	printf("\t name                   = %.16s\n", option->name);
	printf("\t flags                  = %u\n", option->flags);
	printf("\t max_grain_samples      = %u\n", option->max_grain_samples);
	printf("\t max_voices             = %u\n", option->max_voices);
	printf("\t max_input_delay_blocks = %u\n", option->max_input_delay_blocks);
	printf("\t max_matrices           = %u\n", option->max_matrices);
	printf("\t max_ports              = %u\n", option->max_ports);
	printf("\t host_buffer            = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(buffer_info->host_buffer));
	printf("\t host_buffer_size      = 0x%016" PRIx64 "\n", static_cast<uint64_t>(buffer_info->host_buffer_size));

	std::lock_guard registry_lock(g_ngs_registry_mutex);
	auto* ngs = Ngs2FindSystemLocked(system_handle);
	EXIT_NOT_IMPLEMENTED(ngs == nullptr);
	if (ngs == nullptr) return -1;
	Core::LockGuard system_lock(ngs->mutex);
	const size_t max_voices = option->max_voices;
	EXIT_NOT_IMPLEMENTED(max_voices >
	                     (SIZE_MAX - sizeof(Ngs2RackInternal)) / sizeof(Ngs2VoiceInternal));
	EXIT_NOT_IMPLEMENTED(buffer_info->host_buffer_size <
	                     sizeof(Ngs2RackInternal) + max_voices * sizeof(Ngs2VoiceInternal));
	const auto token = reinterpret_cast<uintptr_t>(buffer_info->host_buffer);
	const size_t native_size = sizeof(Ngs2RackInternal) + max_voices * sizeof(Ngs2VoiceInternal);
	EXIT_NOT_IMPLEMENTED(token > UINTPTR_MAX - native_size || Ngs2FindRackLocked(token) != nullptr);
	if (token > UINTPTR_MAX - native_size || Ngs2FindRackLocked(token) != nullptr) return -1;
	std::unique_ptr<uint8_t[]> shadow(new uint8_t[native_size]);
	auto* rack = new (shadow.get()) Ngs2RackInternal {};
	auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);
	rack->guest_token = token;
	rack->guest_capacity = buffer_info->host_buffer_size;

	switch (rack_id)
	{
		case 0x1000:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SamplerRackOption));
			rack->option.sampler = *reinterpret_cast<const Ngs2SamplerRackOption*>(option);
			rack->type           = Ngs2RackType::Sampler;
			break;
		case 0x2000:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2SubmixerRackOption));
			rack->option.submixer = *reinterpret_cast<const Ngs2SubmixerRackOption*>(option);
			rack->type            = Ngs2RackType::Submixer;
			break;
		case 0x2001:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2ReverbRackOption));
			rack->option.reverb = *reinterpret_cast<const Ngs2ReverbRackOption*>(option);
			rack->type          = Ngs2RackType::Reverb;
			break;
		case 0x3000:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2MasteringRackOption));
			rack->option.mastering = *reinterpret_cast<const Ngs2MasteringRackOption*>(option);
			rack->type             = Ngs2RackType::Mastering;
			break;
		case 0x4002:
			EXIT_NOT_IMPLEMENTED(option->size != sizeof(Ngs2CustomSubmixerRackOption));
			rack->option.custom_submixer = *reinterpret_cast<const Ngs2CustomSubmixerRackOption*>(option);
			rack->type                   = Ngs2RackType::CustomSubmixer;
			break;
		default: EXIT("unknown rack_id: 0x%" PRIx32 "\n", rack_id);
	}
	printf("\t type                   = %s\n", Core::EnumName(rack->type).C_Str());

	rack->allocator = Ngs2BufferAllocator();
	rack->ngs       = ngs;

	rack->next   = g_racks_list;
	g_racks_list = rack;

	for (uint32_t i = 0; i < option->max_voices; i++)
	{
		new (voices + i) Ngs2VoiceInternal {};
		voices[i].rack  = rack;
		voices[i].event = Ngs2VoicePlayEvent::None;
		voices[i].state = Ngs2VoicePlayState::Empty;
	}

	shadow.release();
	*handle = rack->guest_token;

	return OK;
}

int LUMEN_SYSV_ABI Ngs2RackCreateWithAllocator(uintptr_t system_handle, uint32_t rack_id, const Ngs2RackOption* option,
                                              const Ngs2BufferAllocator* allocator, uintptr_t* handle)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(option == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator == nullptr);
	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	Ngs2RackOptionUnion owned_option {};
	Ngs2BufferAllocator owned_allocator {};
	if (!Ngs2SnapshotRackOption(rack_id, option, &owned_option) || !Ngs2ReadGuestObject(allocator, &owned_allocator)) return -1;
	option = &owned_option.common;
	allocator = &owned_allocator;
	EXIT_NOT_IMPLEMENTED(allocator->alloc_handler == nullptr);
	EXIT_NOT_IMPLEMENTED(allocator->free_handler == nullptr);
	EXIT_NOT_IMPLEMENTED(system_handle == 0);

	EXIT_NOT_IMPLEMENTED(option->size < sizeof(Ngs2RackOption));

	printf("\t rack_id                = 0x%" PRIx32 "\n", rack_id);
	printf("\t name                   = %.16s\n", option->name);
	printf("\t flags                  = %u\n", option->flags);
	printf("\t max_grain_samples      = %u\n", option->max_grain_samples);
	printf("\t max_voices             = %u\n", option->max_voices);
	printf("\t max_input_delay_blocks = %u\n", option->max_input_delay_blocks);
	printf("\t max_matrices           = %u\n", option->max_matrices);
	printf("\t max_ports              = %u\n", option->max_ports);
	printf("\t alloc_handler          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(allocator->alloc_handler));
	printf("\t free_handler           = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(allocator->free_handler));
	printf("\t user_data              = 0x%016" PRIx64 "\n", static_cast<uint64_t>(reinterpret_cast<uintptr_t>(allocator->user_data)));

	Ngs2ContextBufferInfo buf {};
	buf.host_buffer      = nullptr;
	buf.host_buffer_size = 0;
	buf.user_data        = allocator->user_data;

	Ngs2RackQueryBufferSize(rack_id, option, &buf);

	EXIT_NOT_IMPLEMENTED(buf.host_buffer_size == 0);

	int result = InvokeNgs2BufferAllocHandler(allocator->alloc_handler, &buf);

	EXIT_NOT_IMPLEMENTED(result != OK);
	EXIT_NOT_IMPLEMENTED(buf.host_buffer == nullptr);

	result = Ngs2RackCreate(system_handle, rack_id, option, &buf, handle);

	if (result == OK)
	{
		std::lock_guard registry_lock(g_ngs_registry_mutex);
		auto* rack = Ngs2FindRackLocked(reinterpret_cast<uintptr_t>(buf.host_buffer));
		if (rack == nullptr) return -1;
		rack->allocator = *allocator;
	} else
	{
		(void)InvokeNgs2BufferFreeHandler(allocator->free_handler, &buf);
	}

	return result;
}

int LUMEN_SYSV_ABI Ngs2SystemRender(uintptr_t system_handle, const Ngs2RenderBufferInfo* buffer_info, uint32_t num_buffer_info)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(buffer_info == nullptr);
	EXIT_NOT_IMPLEMENTED(system_handle == 0);
	EXIT_NOT_IMPLEMENTED(num_buffer_info == 0);
	Ngs2RenderBufferSnapshot prepared {};
	Core::VirtualMemory::GuestDataLease output;
	if (Ngs2AudioRenderExperimentalEnabled())
	{
		std::array<uint8_t, sizeof(prepared)> bytes {};
		{
			Core::VirtualMemory::GuestDataLease input;
			if (!BorrowAudioGuestRange(buffer_info, bytes.size(), Core::VirtualMemory::Mode::Read, &input)) return -1;
			std::memcpy(bytes.data(), buffer_info, bytes.size());
		}
		prepared.data = Ngs2LoadLe64(bytes.data());
		prepared.size = Ngs2LoadLe64(bytes.data() + 8);
		prepared.type = Ngs2LoadLe32(bytes.data() + 16);
		prepared.channels = Ngs2LoadLe32(bytes.data() + 20);
		const bool valid = num_buffer_info == 1 && prepared.channels == 8 &&
		                   prepared.type == NGS2_RENDER_WAVEFORM_TYPE_FLOAT32 &&
		                   prepared.size == 512 * 8 * sizeof(float) &&
		                   BorrowAudioGuestRange(reinterpret_cast<void*>(prepared.data), prepared.size,
		                                         Core::VirtualMemory::Mode::Write, &output);
		EXIT_NOT_IMPLEMENTED(!valid);
		if (!valid) return -1;
	}

	std::lock_guard registry_lock(g_ngs_registry_mutex);
	auto* ngs = Ngs2FindSystemLocked(system_handle);
	EXIT_NOT_IMPLEMENTED(ngs == nullptr);
	if (ngs == nullptr) return -1;
	Core::LockGuard system_lock(ngs->mutex);
	if (Ngs2AudioRenderExperimentalEnabled())
	{
		EXIT_NOT_IMPLEMENTED(!Ngs2RenderAudioLocked(ngs, buffer_info, num_buffer_info, &prepared, &output));
		return OK;
	}

	for (auto* rack = g_racks_list; rack != nullptr; rack = rack->next)
	{
		if (rack->ngs == ngs)
		{
			auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);

			for (uint32_t i = 0; i < rack->option.common.max_voices; i++)
			{
				auto& voice = voices[i];
				switch (voice.event)
				{
					case Ngs2VoicePlayEvent::None:
						if (voice.state == Ngs2VoicePlayState::Playing || voice.state == Ngs2VoicePlayState::Stopped)
						{
							voice.state = Ngs2VoicePlayState::Empty;
						}
						break;
					case Ngs2VoicePlayEvent::Play:
						if (voice.state == Ngs2VoicePlayState::Empty)
						{
							voice.state = Ngs2VoicePlayState::Playing;
						}
						break;
					case Ngs2VoicePlayEvent::Pause:
						if (voice.state == Ngs2VoicePlayState::Playing)
						{
							voice.state = Ngs2VoicePlayState::Paused;
						}
						break;
					case Ngs2VoicePlayEvent::Resume:
						if (voice.state == Ngs2VoicePlayState::Paused)
						{
							voice.state = Ngs2VoicePlayState::Playing;
						}
						break;
					case Ngs2VoicePlayEvent::Stop:
						if (voice.state == Ngs2VoicePlayState::Playing)
						{
							voice.state = Ngs2VoicePlayState::Stopped;
						}
						break;
					case Ngs2VoicePlayEvent::StopImm:
					case Ngs2VoicePlayEvent::Kill: voice.state = Ngs2VoicePlayState::Empty; break;
				}
				voice.event = Ngs2VoicePlayEvent::None;
			}
		}
	}

	return OK;
}

int LUMEN_SYSV_ABI Ngs2RackGetVoiceHandle(uintptr_t rack_handle, uint32_t voice_id, uintptr_t* handle)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(handle == nullptr);
	EXIT_NOT_IMPLEMENTED(rack_handle == 0);
	Core::VirtualMemory::GuestDataLease output;
	if (!BorrowAudioGuestRange(handle, sizeof(*handle), Core::VirtualMemory::Mode::Write, &output)) return -1;

	printf("\t voice_id = %u\n", voice_id);

	std::lock_guard registry_lock(g_ngs_registry_mutex);
	auto* rack = Ngs2FindRackLocked(rack_handle);
	EXIT_NOT_IMPLEMENTED(rack == nullptr);
	if (rack == nullptr) return -1;
	auto* voices = reinterpret_cast<Ngs2VoiceInternal*>(rack + 1);

	EXIT_NOT_IMPLEMENTED(voice_id >= rack->option.common.max_voices);

	EXIT_IF(voices[voice_id].rack != rack);

	*handle = rack->guest_token + sizeof(Ngs2RackInternal) + voice_id * sizeof(Ngs2VoiceInternal);

	return OK;
}

struct Ngs2VoiceControlSnapshot
{
	uintptr_t address = 0;
	Ngs2VoiceParamHeaderSnapshot header;
	std::array<uint8_t, sizeof(Ngs2SamplerFormatParam)> bytes {};
	std::unique_ptr<Ngs2SamplerBlockSnapshot> block;
};

static bool Ngs2SnapshotVoiceControl(const Ngs2VoiceParamHeader* list,
                                    std::vector<Ngs2VoiceControlSnapshot>* owned)
{
	uintptr_t address = reinterpret_cast<uintptr_t>(list);
	for (uint32_t count = 0; count < 4096; ++count)
	{
		Ngs2VoiceControlSnapshot value;
		value.address = address;
		if (!Ngs2ReadVoiceParamHeader(address, &value.header)) return false;
		const auto rack_id = value.header.id >> 16u;
		const auto cid = value.header.id & 0x7fffu;
		size_t payload_size = 0;
		if (rack_id == 0)
		{
			if (cid == 2) payload_size = sizeof(Ngs2VoicePortMatrixParam);
			if (cid == 5) payload_size = sizeof(Ngs2VoicePatchParam);
			if (cid == 6) payload_size = sizeof(Ngs2VoiceEventParam);
		} else if (rack_id == 0x1000 && Ngs2AudioRenderExperimentalEnabled())
		{
			if (cid == 0) payload_size = sizeof(Ngs2SamplerFormatParam);
			if (cid == 1) payload_size = sizeof(Ngs2SamplerBlockParam);
		}
		if (payload_size != 0)
		{
			if (payload_size > value.bytes.size() || value.header.size != payload_size ||
			    !Loader::ReadGuestMemory(address, value.bytes.data(), payload_size)) return false;
			// Use one stable header/payload interpretation for this operation.
			if (Ngs2LoadLe32(value.bytes.data() + 4) != value.header.id ||
			    value.bytes[0] != static_cast<uint8_t>(value.header.size) ||
			    value.bytes[1] != static_cast<uint8_t>(value.header.size >> 8) ||
			    value.bytes[2] != static_cast<uint8_t>(value.header.next) ||
			    value.bytes[3] != static_cast<uint8_t>(static_cast<uint16_t>(value.header.next) >> 8)) return false;
			if (rack_id == 0x1000 && cid == 1)
			{
				value.block = std::make_unique<Ngs2SamplerBlockSnapshot>();
				if (!Ngs2SnapshotSamplerBlock(address, value.bytes.data(), value.block.get())) return false;
			}
		}
		const auto next = value.header.next;
		owned->push_back(std::move(value));
		if (next == 0) return true;
		if (next < static_cast<int16_t>(sizeof(Ngs2VoiceParamHeader)) ||
		    address > UINTPTR_MAX - static_cast<uint16_t>(next)) return false;
		address += static_cast<uint16_t>(next);
	}
	return false;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int LUMEN_SYSV_ABI Ngs2VoiceControl(uintptr_t voice_handle, const Ngs2VoiceParamHeader* param_list)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(param_list == nullptr);
	EXIT_NOT_IMPLEMENTED(voice_handle == 0);
	if (param_list == nullptr || voice_handle == 0) return -1;
	std::vector<Ngs2VoiceControlSnapshot> parameters;
	const bool readable = Ngs2SnapshotVoiceControl(param_list, &parameters);
	EXIT_NOT_IMPLEMENTED(!readable);
	if (!readable) return -1;

	std::lock_guard registry_lock(g_ngs_registry_mutex);
	Ngs2RackInternal* owner = nullptr;
	auto* voice = Ngs2FindVoiceLocked(voice_handle, &owner);
	EXIT_NOT_IMPLEMENTED(voice == nullptr || owner == nullptr || owner->ngs == nullptr);
	if (voice == nullptr || owner == nullptr || owner->ngs == nullptr) return -1;
	Core::LockGuard lock(owner->ngs->mutex);

	for (const auto& prepared: parameters)
	{
		const auto& param = prepared.header;
		const auto param_address = prepared.address;
		printf("\t id   = 0x%08" PRIx32 "\n", param.id);
		printf("\t size = %" PRIu16 "\n", param.size);
		printf("\t next = %" PRId16 "\n", param.next);

		auto rack_id = param.id >> 16u;

		EXIT_NOT_IMPLEMENTED(((param.id >> 15u) & 0x1u) != 0);

		switch (rack_id)
		{
			case 0x0000:
			{
				auto cid = param.id & 0x7fffu;
				switch (cid)
				{
					case 0x0001:
						EXIT_NOT_IMPLEMENTED(!Ngs2AudioRenderExperimentalEnabled() || param.size != 24);
						break;
					case 0x0003:
						EXIT_NOT_IMPLEMENTED(!Ngs2AudioRenderExperimentalEnabled() || param.size != 16);
						break;
					case 0x0002:
					{
						EXIT_NOT_IMPLEMENTED(param.size != sizeof(Ngs2VoicePortMatrixParam));
						const auto& bytes = prepared.bytes;
						printf("\t port      = %u\n", Ngs2LoadLe32(bytes.data() + 8));
						printf("\t matrix_id = %d\n", static_cast<int32_t>(Ngs2LoadLe32(bytes.data() + 12)));
						break;
					}
					case 0x0005:
					{
						EXIT_NOT_IMPLEMENTED(param.size != sizeof(Ngs2VoicePatchParam));
						const auto& bytes = prepared.bytes;
						printf("\t connect->port          = %u\n", Ngs2LoadLe32(bytes.data() + 8));
						printf("\t connect->dest_input_id = %u\n", Ngs2LoadLe32(bytes.data() + 12));
						printf("\t connect->dest_handle   = 0x%016" PRIx64 "\n", Ngs2LoadLe64(bytes.data() + 16));
						break;
					}
					case 0x0006:
					{
						EXIT_NOT_IMPLEMENTED(param.size != sizeof(Ngs2VoiceEventParam));
						const auto& bytes = prepared.bytes;
						const uint32_t event_id = Ngs2LoadLe32(bytes.data() + 8);
						const auto prior_event = voice->event;
						switch (event_id)
						{
							case 0: voice->event = Ngs2VoicePlayEvent::Play; break;
							case 1: voice->event = Ngs2VoicePlayEvent::Stop; break;
							case 2: voice->event = Ngs2VoicePlayEvent::StopImm; break;
							case 3: voice->event = Ngs2VoicePlayEvent::Kill; break;
							case 4: voice->event = Ngs2VoicePlayEvent::Pause; break;
							case 5: voice->event = Ngs2VoicePlayEvent::Resume; break;
							default: EXIT("unknown event_id: 0x%08" PRIx32 "\n", event_id);
						}
						if (auto* diagnostics = Ngs2FindVoiceDiagnostics(voice); diagnostics != nullptr)
						{
							++diagnostics->received[static_cast<size_t>(voice->event)];
							if (prior_event != Ngs2VoicePlayEvent::None) ++diagnostics->overwritten_events;
						}
						Ngs2TraceVoice(voice, "received", false, 0, false, 0, 0, prior_event);
						printf("\t event = %u\n", event_id);
						break;
					}
					default: EXIT("unknown id: 0x%04" PRIx32 "\n", cid);
				}
				break;
			}
			case 0x1000:
			{
				EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Sampler);
				if (!Ngs2AudioRenderExperimentalEnabled()) break;
				const auto cid = param.id & 0x7fffu;
				if (cid == 0x0000)
				{
					EXIT_NOT_IMPLEMENTED(param.size != sizeof(Ngs2SamplerFormatParam));
					EXIT_NOT_IMPLEMENTED(!Ngs2InstallSamplerFormat(
					    voice, reinterpret_cast<const Ngs2VoiceParamHeader*>(param_address), prepared.bytes.data()));
				} else if (cid == 0x0001)
				{
					EXIT_NOT_IMPLEMENTED(param.size != sizeof(Ngs2SamplerBlockParam));
					EXIT_NOT_IMPLEMENTED(!Ngs2InstallSamplerBlock(
					    voice, reinterpret_cast<const Ngs2VoiceParamHeader*>(param_address), prepared.block.get()));
				} else if (cid == 0x0005)
				{
					EXIT_NOT_IMPLEMENTED(param.size != 16);
				} else if (cid == 0x0006)
				{
					EXIT_NOT_IMPLEMENTED(param.size != 24);
				} else if (cid == 0x0007)
				{
					EXIT_NOT_IMPLEMENTED(param.size != 40);
				} else if (cid == 0x000a)
				{
					EXIT_NOT_IMPLEMENTED(param.size != 48);
				} else
					EXIT("unknown sampler id: 0x%04" PRIx32 "\n", cid);
				break;
			}
			case 0x2000: EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Submixer); break;
			case 0x2001: EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Reverb); break;
			case 0x3000: EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::Mastering); break;
			case 0x4000: EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::CustomSubmixer); break;
			case 0x4002: EXIT_NOT_IMPLEMENTED(voice->rack->type != Ngs2RackType::CustomSubmixer); break;
			default: EXIT("unknown rack_id: 0x%" PRIx32 "\n", rack_id);
		}

	}

	return OK;
}

static bool Ngs2VoiceGetStateModelImpl(uintptr_t voice_handle, uint64_t state_address, size_t state_size)
{

	if (state_address == 0 || voice_handle == 0) return false;
	if (state_size != sizeof(Ngs2SamplerVoiceState) && state_size != sizeof(Ngs2MasteringVoiceState)) return false;
	Ngs2WaveformMemoryRange output_range;
	Core::VirtualMemory::GuestDataLease output_lease;
	if (!Ngs2MakeMemoryRange(state_address, state_size, &output_range) ||
	    !Core::VirtualMemory::TryAcquireGuestDataLease(
	        state_address, state_size, Core::VirtualMemory::Mode::Write, &output_lease)) return false;

	std::lock_guard registry_lock(g_ngs_registry_mutex);
	Ngs2RackInternal* owner = nullptr;
	auto* voice = Ngs2FindVoiceLocked(voice_handle, &owner);
	if (voice == nullptr || owner == nullptr || owner->ngs == nullptr) return false;
	Core::LockGuard lock(owner->ngs->mutex);

	const size_t expected_size = owner->type == Ngs2RackType::Sampler ? sizeof(Ngs2SamplerVoiceState)
	                            : owner->type == Ngs2RackType::Mastering ? sizeof(Ngs2MasteringVoiceState)
	                                                                      : 0;
	if (expected_size == 0 || state_size != expected_size) return false;
	bool registry_valid = false;
	const bool output_aliases = Ngs2OutputAliasesLiveMemoryLocked(output_range, &registry_valid);
	if (output_aliases || !registry_valid) return false;

	uint32_t state_flags = 0;
	const bool valid_state = Ngs2VoiceStateFlagsValue(Ngs2VisibleVoiceState(voice), &state_flags);
	if (!valid_state) return false;
	std::array<uint8_t, sizeof(Ngs2MasteringVoiceState)> snapshot {};

	switch (owner->type)
	{
		case Ngs2RackType::Sampler:
		{
			Ngs2SamplerVoiceState sampler_snapshot {};
			auto* sampler = &sampler_snapshot;
			sampler->voice_state.state_flags = state_flags;
			sampler->envelope_height = 1.0f;
			if (Ngs2AudioRenderExperimentalEnabled())
			{
				const auto* audio = Ngs2FindVoiceAudioState(voice);
				if (audio != nullptr && audio->sampler_runtime != nullptr && !audio->sampler_runtime->blocks.empty())
					sampler->waveform_data = reinterpret_cast<const void*>(audio->sampler_runtime->blocks.front().waveform_data);
			}
			printf("\t state_flags = %u\n", sampler->voice_state.state_flags);
			std::memcpy(snapshot.data(), sampler, sizeof(*sampler));
			break;
		}
		case Ngs2RackType::Mastering:
		{
			Ngs2MasteringVoiceState mastering_snapshot {};
			auto* mastering = &mastering_snapshot;
			mastering->voice_state.state_flags = state_flags;
			printf("\t state_flags = %u\n", mastering->voice_state.state_flags);
			std::memcpy(snapshot.data(), mastering, sizeof(*mastering));
			break;
		}
		default: return false;
	}
	std::memcpy(reinterpret_cast<void*>(output_lease.HostBegin()), snapshot.data(), expected_size);

	return true;
}

int LUMEN_SYSV_ABI Ngs2VoiceGetState(uintptr_t voice_handle, Ngs2VoiceState* state, size_t state_size)
{
	PRINT_NAME();
	const bool complete = Ngs2VoiceGetStateModelImpl(voice_handle, reinterpret_cast<uint64_t>(state), state_size);
	EXIT_NOT_IMPLEMENTED(!complete);
	return complete ? OK : -1;
}

bool ResetForNextSession()
{
	bool ok = true;

	Ngs2RackInternal* rack = nullptr;
	Ngs2Internal*     ngs  = nullptr;
	{
		std::lock_guard registry_lock(g_ngs_registry_mutex);
		rack = g_racks_list;
		ngs  = g_ngs_list;
		g_racks_list = nullptr;
		g_ngs_list   = nullptr;
		g_ngs2_voice_audio_states.clear();
		for (auto& diagnostics: g_ngs2_audio_diagnostics) Ngs2ReportAudioDiagnostics(&diagnostics.second, "reset");
		g_ngs2_audio_diagnostics.clear();
		g_ngs2_audio_diagnostic_system = 0;
	}
	while (rack != nullptr)
	{
		auto* const next = rack->next;
		const auto  allocator = rack->allocator;
		const auto guest_token = rack->guest_token;
		const auto guest_capacity = rack->guest_capacity;
		const auto  voices_num = static_cast<size_t>(rack->option.common.max_voices);
		size_t      buffer_size = 0;
		if (voices_num <= (SIZE_MAX - sizeof(Ngs2RackInternal)) / sizeof(Ngs2VoiceInternal))
		{
			buffer_size = sizeof(Ngs2RackInternal) + sizeof(Ngs2VoiceInternal) * voices_num;
		} else
		{
			ok = false;
		}
		rack->ngs  = nullptr;
		rack->next = nullptr;
		if (allocator.free_handler != nullptr)
		{
			Ngs2ContextBufferInfo buffer {};
			buffer.host_buffer      = reinterpret_cast<void*>(guest_token);
			buffer.host_buffer_size = guest_capacity != 0 ? guest_capacity : buffer_size;
			buffer.user_data        = allocator.user_data;
			if (InvokeNgs2BufferFreeHandler(allocator.free_handler, &buffer) != OK)
			{
				ok = false;
			}
		}
		rack->~Ngs2RackInternal();
		delete[] reinterpret_cast<uint8_t*>(rack);
		rack = next;
	}

	while (ngs != nullptr)
	{
		auto* const next      = ngs->next;
		const auto  allocator = ngs->allocator;
		const auto guest_token = ngs->guest_token;
		const auto guest_capacity = ngs->guest_capacity;
		ngs->next = nullptr;
		if (allocator.free_handler != nullptr)
		{
			Ngs2ContextBufferInfo buffer {};
			buffer.host_buffer      = reinterpret_cast<void*>(guest_token);
			buffer.host_buffer_size = guest_capacity;
			buffer.user_data        = allocator.user_data;
			if (InvokeNgs2BufferFreeHandler(allocator.free_handler, &buffer) != OK)
			{
				ok = false;
			}
		}
		delete ngs;
		ngs = next;
	}

	return ok;
}

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
struct Ngs2HandoffOrderingProbe
{
	struct Access
	{
		uint64_t address = 0, bytes = 0;
		GuestCpu::MemoryAccessIntent intent = GuestCpu::MemoryAccessIntent::Read;
	};
	Ngs2Internal* system = nullptr;
	uint64_t address = 0, bytes = 0;
	GuestCpu::MemoryAccessIntent intent = GuestCpu::MemoryAccessIntent::Read;
	unsigned calls = 0;
	bool unlocked = true, precise = true;
	std::array<Access, 2> additional {};
};
thread_local Ngs2HandoffOrderingProbe* g_ngs_handoff_probe = nullptr;

[[maybe_unused]] static bool Ngs2HandoffOrderingQuery(uint64_t address, uint64_t bytes,
                                    GuestCpu::MemoryAccessIntent intent) noexcept
{
	auto* probe = g_ngs_handoff_probe;
	if (probe == nullptr) return true;
	++probe->calls;
	bool exact = address == probe->address && intent == probe->intent &&
	             (bytes == probe->bytes || (intent == GuestCpu::MemoryAccessIntent::Read &&
	                                        bytes == sizeof(Ngs2VoiceParamHeader)));
	for (const auto& access: probe->additional)
		if (access.bytes != 0 && address == access.address && bytes == access.bytes && intent == access.intent) exact = true;
	probe->precise = probe->precise && exact;
	bool acquired = false;
	std::thread peer([&] {
		if (!g_ngs_registry_mutex.try_lock()) return;
		if (probe->system->mutex.TryLock())
		{
			acquired = true;
			probe->system->mutex.Unlock();
		}
		g_ngs_registry_mutex.unlock();
	});
	peer.join();
	probe->unlocked = probe->unlocked && acquired;
	return true;
}

struct Ngs2ScopedHandoffProbe
{
	explicit Ngs2ScopedHandoffProbe(Ngs2HandoffOrderingProbe* probe)
	{
		GuestCpu::MarkHandoffRange(probe->address, probe->bytes);
		for (const auto& access: probe->additional)
			if (access.bytes != 0) GuestCpu::MarkHandoffRange(access.address, access.bytes);
		g_ngs_handoff_probe = probe;
	}
	~Ngs2ScopedHandoffProbe() { g_ngs_handoff_probe = nullptr; }
};

static void Ngs2TestWriteBits(uint8_t* data, int* bit_position, uint32_t value, int count)
{
	for (int bit = count - 1; bit >= 0; --bit)
	{
		const int byte_index = *bit_position / 8;
		const int bit_index  = 7 - (*bit_position % 8);
		data[byte_index] |= static_cast<uint8_t>(((value >> bit) & 1u) << bit_index);
		++*bit_position;
	}
}

static uint32_t Ngs2TestNextRandom(uint32_t* state)
{
	*state ^= *state << 13u;
	*state ^= *state >> 17u;
	*state ^= *state << 5u;
	return *state;
}

static void Ngs2TestWriteRandomBits(uint8_t* data, int* bit_position, int count, uint32_t* state)
{
	for (int bit = 0; bit < count; ++bit)
		Ngs2TestWriteBits(data, bit_position, Ngs2TestNextRandom(state) & 1u, 1);
}

static void Ngs2TestWriteStereoAtrac9Frame(uint8_t* data, int* bit_position,
	                                       bool first_in_superframe, uint32_t* random_state)
{
	Ngs2TestWriteBits(data, bit_position, first_in_superframe ? 0u : 1u, 1);
	Ngs2TestWriteBits(data, bit_position, 0, 1);
	Ngs2TestWriteBits(data, bit_position, 0, 4);
	Ngs2TestWriteBits(data, bit_position, 0, 4);
	Ngs2TestWriteBits(data, bit_position, 0, 1);
	Ngs2TestWriteBits(data, bit_position, 0, 2);
	Ngs2TestWriteBits(data, bit_position, 0, 6);
	Ngs2TestWriteBits(data, bit_position, 0, 6);
	Ngs2TestWriteBits(data, bit_position, 0, 5);
	Ngs2TestWriteBits(data, bit_position, 0, 5);
	Ngs2TestWriteBits(data, bit_position, 0, 4);
	Ngs2TestWriteBits(data, bit_position, 0, 1);
	Ngs2TestWriteBits(data, bit_position, 0, 1);
	Ngs2TestWriteBits(data, bit_position, 0, 1);
	Ngs2TestWriteBits(data, bit_position, 1, 2);
	Ngs2TestWriteBits(data, bit_position, 3, 2);
	for (int factor = 0; factor < 10; ++factor)
		Ngs2TestWriteBits(data, bit_position, 20, 5);
	Ngs2TestWriteRandomBits(data, bit_position, 528, random_state);
	Ngs2TestWriteBits(data, bit_position, 1, 2);
	Ngs2TestWriteBits(data, bit_position, 3, 2);
	for (int factor = 0; factor < 10; ++factor)
		Ngs2TestWriteBits(data, bit_position, 0, 3);
	Ngs2TestWriteRandomBits(data, bit_position, 528, random_state);
	*bit_position = (*bit_position + 7) & ~7;
}

static std::vector<uint8_t> Ngs2TestMakeStereoAtrac9Superframe(uint32_t seed)
{
	std::vector<uint8_t> encoded(1024);
	int bit_position = 0;
	for (uint32_t frame = 0; frame < 4; ++frame)
		Ngs2TestWriteStereoAtrac9Frame(encoded.data(), &bit_position, frame == 0, &seed);
	Ngs2TestWriteRandomBits(encoded.data(), &bit_position,
	                        static_cast<int>(encoded.size() * 8u) - bit_position, &seed);
	return encoded;
}

static bool Ngs2Atrac9DecoderHistorySyntheticTest()
{
	// Original local bitstreams: one continuous decode is the independent
	// reference for separated appends. No game data or external vectors are used.
	const auto first = Ngs2TestMakeStereoAtrac9Superframe(0x4b595459u);
	const auto second = Ngs2TestMakeStereoAtrac9Superframe(0x4c5a555au);
	const auto third = Ngs2TestMakeStereoAtrac9Superframe(0x4d5b565bu);
	std::vector<uint8_t> joined(first);
	joined.insert(joined.end(), second.begin(), second.end());
	joined.insert(joined.end(), third.begin(), third.end());
	std::vector<float> reference;
	if (!Ngs2DecodeAtrac9Snapshot(0xfe741ff0u, 2, 48000, joined, 0, 3072, &reference) ||
	    reference.size() != 6144) return false;
	Ngs2Atrac9Decoder history {nullptr, Atrac9ReleaseHandle};
	std::vector<float> separated, block;
	for (const auto* encoded: {&first, &second, &third})
	{
		Ngs2Atrac9Decoder candidate {nullptr, Atrac9ReleaseHandle};
		if (!Ngs2DecodeAtrac9Snapshot(0xfe741ff0u, 2, 48000, *encoded, 0, 1024, &block,
		                              nullptr, history.get(), &candidate) || candidate == nullptr) return false;
		separated.insert(separated.end(), block.begin(), block.end());
		history = std::move(candidate);
	}
	if (separated != reference) return false;
	std::vector<float> reset_second;
	if (!Ngs2DecodeAtrac9Snapshot(0xfe741ff0u, 2, 48000, second, 0, 1024, &reset_second) ||
	    reset_second.size() != 2048) return false;
	bool reset_lost_history = false;
	for (size_t sample = 0; sample < 512; ++sample)
		reset_lost_history |= std::abs(reset_second[sample] - reference[2048 + sample]) > 1.0e-7f;
	if (!reset_lost_history || !std::equal(reset_second.begin() + 512, reset_second.end(), reference.begin() + 2560))
		return false;

	// A primed independent snapshot retains its final decoder history so the
	// next unprimed append starts at the same encoded boundary.
	if (!Ngs2DecodeAtrac9Snapshot(0xfe741ff0u, 2, 48000, first, 256, 768, &block,
	                              nullptr, nullptr, &history) || history == nullptr ||
	    !std::equal(block.begin(), block.end(), reference.begin() + 512)) return false;
	Ngs2Atrac9Decoder clone {nullptr, Atrac9ReleaseHandle};
	{
		std::lock_guard library_lock(Ajm::Atrac9LibraryMutex());
		clone.reset(Atrac9CloneHandle(history.get()));
	}
	if (clone == nullptr) return false;
	history.reset();
	if (!Ngs2DecodeAtrac9Snapshot(0xfe741ff0u, 2, 48000, second, 0, 1024, &block,
	                              nullptr, clone.get(), &history) || history == nullptr ||
	    !std::equal(block.begin(), block.end(), reference.begin() + 2048)) return false;

	// The first superframe succeeds, then an invalid reuse flag rejects the
	// candidate. The source decoder, output canary and returned history survive.
	if (!Ngs2DecodeAtrac9Snapshot(0xfe741ff0u, 2, 48000, first, 0, 1024, &block,
	                              nullptr, nullptr, &history) || history == nullptr) return false;
	std::vector<uint8_t> malformed(second);
	auto invalid = third;
	invalid[0] |= 0x40u;
	malformed.insert(malformed.end(), invalid.begin(), invalid.end());
	std::vector<float> untouched {17.0f};
	Ngs2Atrac9Decoder rejected {nullptr, Atrac9ReleaseHandle};
	const void* const original_history = history.get();
	if (Ngs2DecodeAtrac9Snapshot(0xfe741ff0u, 2, 48000, malformed, 0, 2048, &untouched,
	                             nullptr, history.get(), &rejected) || rejected != nullptr ||
	    history.get() != original_history || untouched != std::vector<float> {17.0f}) return false;
	if (!Ngs2DecodeAtrac9Snapshot(0xfe741ff0u, 2, 48000, second, 0, 1024, &block,
	                              nullptr, history.get(), &rejected) || rejected == nullptr ||
	    !std::equal(block.begin(), block.end(), reference.begin() + 2048)) return false;
	if (Ngs2DecodeAtrac9Snapshot(0xfe740ff0u, 2, 48000, second, 0, 1024, &untouched,
	                             nullptr, history.get(), nullptr) || untouched != std::vector<float> {17.0f}) return false;
	if (!Ngs2DecodeAtrac9Snapshot(0xfe741ff0u, 2, 48000, first, 0, 600, &block,
	                              nullptr, nullptr, &rejected) || rejected != nullptr || block.size() != 1200 ||
	    !std::equal(block.begin(), block.end(), reference.begin())) return false;

	// Clone the initialized graphs for every locally admitted channel layout,
	// rate and superframe shape, and release the source before querying the clone.
	std::lock_guard library_lock(Ajm::Atrac9LibraryMutex());
	if (Atrac9CloneHandle(nullptr) != nullptr) return false;
	Ngs2Atrac9Decoder uninitialized {Atrac9GetHandle(), Atrac9ReleaseHandle};
	if (uninitialized == nullptr || Atrac9CloneHandle(uninitialized.get()) != nullptr) return false;
	for (uint32_t rate: {4u, 7u})
		for (uint32_t layout = 0; layout <= 5; ++layout)
			for (uint32_t superframe_index: {0u, 2u})
			{
				const uint32_t config = 0xfe000000u | (rate << 20u) | (layout << 17u) | (255u << 5u) |
				                        (superframe_index << 3u);
				std::array<unsigned char, 4> bytes {{static_cast<unsigned char>(config >> 24u),
				                                    static_cast<unsigned char>(config >> 16u),
				                                    static_cast<unsigned char>(config >> 8u),
				                                    static_cast<unsigned char>(config)}};
				Ngs2Atrac9Decoder source {Atrac9GetHandle(), Atrac9ReleaseHandle};
				Atrac9CodecInfo source_info {}, clone_info {};
				if (source == nullptr || Atrac9InitDecoder(source.get(), bytes.data()) != 0 ||
				    Atrac9GetCodecInfo(source.get(), &source_info) != 0) return false;
				Ngs2Atrac9Decoder copied {Atrac9CloneHandle(source.get()), Atrac9ReleaseHandle};
				if (copied == nullptr) return false;
				source.reset();
				if (Atrac9GetCodecInfo(copied.get(), &clone_info) != 0 ||
				    source_info.channels != clone_info.channels || source_info.samplingRate != clone_info.samplingRate ||
				    source_info.superframeSize != clone_info.superframeSize ||
				    source_info.framesInSuperframe != clone_info.framesInSuperframe ||
				    source_info.frameSamples != clone_info.frameSamples ||
				    !std::equal(bytes.begin(), bytes.end(), clone_info.configData)) return false;
			}
	return true;
}

static bool Ngs2Atrac9DecodeSyntheticTest()
{
	std::vector<uint8_t> superframe(1024);
	int                  bit_position = 0;
	uint32_t             random_state = 0x4b595459u;
	for (uint32_t frame = 0; frame < 4; ++frame)
	{
		Ngs2TestWriteStereoAtrac9Frame(superframe.data(), &bit_position, frame == 0, &random_state);
		if (frame == 0 && bit_position / 8 != 149) return false;
	}
	Ngs2TestWriteRandomBits(superframe.data(), &bit_position,
	                        static_cast<int>(superframe.size() * 8u) - bit_position, &random_state);

	std::vector<float> decoded;
	if (!Ngs2DecodeAtrac9Snapshot(0xfe741ff0u, 2, 48000, superframe, 0, 1024, &decoded) ||
	    decoded.size() != 2048 ||
	    std::none_of(decoded.begin(), decoded.end(), [](float sample) { return std::abs(sample) > 1.0e-6f; }))
		return false;
	std::vector<float> after_skip;
	if (!Ngs2DecodeAtrac9Snapshot(0xfe741ff0u, 2, 48000, superframe, 256, 768, &after_skip) ||
	    after_skip.size() != 1536 ||
	    !std::equal(after_skip.begin(), after_skip.end(), decoded.begin() + 512))
		return false;
	std::vector<uint8_t> malformed(superframe.begin(), superframe.end() - 1);
	std::vector<float>   untouched {17.0f};
	return !Ngs2DecodeAtrac9Snapshot(0xfe741ff0u, 2, 48000, malformed, 0, 256, &untouched) &&
	       untouched.size() == 1 && untouched[0] == 17.0f && Ngs2Atrac9DecoderHistorySyntheticTest();
}

static bool Ngs2Atrac9ResampleSyntheticTest()
{
	Ngs2Atrac9Geometry geometry;
	if (Ngs2GetAtrac9Geometry(0xfe440ff0u, 2, 48000, &geometry) !=
	        Ngs2WaveformFrameInfoResult::Ok ||
	    geometry.superframe_bytes != 512 || geometry.superframe_samples != 512 ||
	    geometry.output_rate_multiplier != 2)
		return false;
	const std::vector<float> native {0.0f, 1.0f, 0.5f, -0.5f, -1.0f, 0.25f};
	const std::vector<float> expected {0.0f, 1.0f, 0.25f, 0.25f,
	                                   0.5f, -0.5f, -0.25f, -0.125f,
	                                   -1.0f, 0.25f, -1.0f, 0.25f};
	std::vector<float> expanded;
	if (!Ngs2UpsampleAtrac9Twofold(native, 2, &expanded) || expanded != expected ||
	    expanded.size() / 2 != 2 * (native.size() / 2))
		return false;
	std::vector<float> untouched {17.0f};
	return !Ngs2UpsampleAtrac9Twofold(native, 4, &untouched) && untouched.size() == 1 &&
	       untouched[0] == 17.0f;
}

bool AudioRenderQueueContractTest()
{
	if (!Ngs2AudioRenderExperimentalEnabled()) return false;
	if (!GuestCpu::RegisterMemoryHandoffQuery(Ngs2HandoffOrderingQuery)) return false;
	Ngs2BufferAllocator allocator {};
	allocator.alloc_handler = Ngs2SessionResetTestAlloc;
	allocator.free_handler = Ngs2SessionResetTestFree;
	Ngs2SystemOption option {};
	option.size = sizeof(option);
	option.max_grain_samples = option.num_grain_samples = 512;
	option.sample_rate = 48000;
	Ngs2SamplerRackOption rack_option {};
	rack_option.rack_option.size = sizeof(rack_option);
	rack_option.rack_option.max_voices = 1;
	rack_option.rack_option.max_grain_samples = 512;
	rack_option.max_waveform_blocks = 1;
	uintptr_t system = 0, rack = 0, handle = 0;
	if (Ngs2SystemCreateWithAllocator(&option, &allocator, &system) != OK ||
	    Ngs2RackCreateWithAllocator(system, 0x1000, &rack_option.rack_option, &allocator, &rack) != OK ||
	    Ngs2RackGetVoiceHandle(rack, 0, &handle) != OK)
	{
		(void)ResetForNextSession();
		return false;
	}
	auto* voice = Ngs2FindVoiceLocked(handle, nullptr);
	auto& audio = g_ngs2_voice_audio_states[voice];
	audio.waveform_channels = 2;
	audio.sampler_runtime = std::make_unique<Ngs2SamplerRuntime>();
	const bool passed = [&]() {
		auto append = [&](size_t frames, float value, uint64_t token) {
			Ngs2DecodedBlock block;
			block.pcm.assign(frames * 2, value);
			block.waveform_data = token;
			return Ngs2QueueDecodedBlock(audio.sampler_runtime.get(), std::move(block), 2);
		};
		struct Output { uint64_t before = 17; std::array<float, 4096> pcm {}; uint64_t after = 19; } output;
		Ngs2RenderBufferSnapshot descriptor {reinterpret_cast<uint64_t>(output.pcm.data()),
		                                    sizeof(output.pcm), NGS2_RENDER_WAVEFORM_TYPE_FLOAT32, 8};
		auto render = [&]() {
			if (descriptor.size == sizeof(output.pcm))
			{
				Ngs2HandoffOrderingProbe probe;
				probe.system = Ngs2FindSystemLocked(system);
				probe.address = descriptor.data;
				probe.bytes = descriptor.size;
				probe.intent = GuestCpu::MemoryAccessIntent::Write;
				probe.additional[0] = {reinterpret_cast<uint64_t>(&descriptor), sizeof(descriptor),
				                       GuestCpu::MemoryAccessIntent::Read};
				Ngs2ScopedHandoffProbe active(&probe);
				return Ngs2SystemRender(system, reinterpret_cast<const Ngs2RenderBufferInfo*>(&descriptor), 1) == OK &&
				       probe.calls >= 2 && probe.unlocked && probe.precise && output.before == 17 && output.after == 19;
			}
			std::lock_guard registry_lock(g_ngs_registry_mutex);
			Core::LockGuard system_lock(Ngs2FindSystemLocked(system)->mutex);
			return Ngs2RenderAudioLocked(Ngs2FindSystemLocked(system),
			                            reinterpret_cast<const Ngs2RenderBufferInfo*>(&descriptor), 1) &&
			       output.before == 17 && output.after == 19;
		};
		auto range_matches = [&](size_t begin, size_t end, float value) {
			for (size_t frame = begin; frame < end; ++frame)
				for (size_t channel = 0; channel < 8; ++channel)
					if (output.pcm[frame * 8 + channel] != (channel < 2 ? value : 0.0f)) return false;
			return true;
		};
		auto identity = [&](uint64_t expected) {
			struct State { uint64_t before = 23; Ngs2SamplerVoiceState value {}; uint64_t after = 29; } state;
			return Ngs2VoiceGetState(handle, &state.value.voice_state, sizeof(state.value)) == OK &&
			       reinterpret_cast<uint64_t>(state.value.waveform_data) == expected &&
			       state.value.num_decoded_samples == 0 && state.value.decoded_data_size == 0 && state.value.user_data == 0 &&
			       state.before == 23 && state.after == 29;
		};
		auto visible_state = [&](uint32_t expected) {
			struct Flags { uint32_t before = 23; uint32_t value = UINT32_MAX; uint32_t after = 29; } flags;
			struct State { uint32_t before = 31; Ngs2SamplerVoiceState value {}; uint32_t after = 37; } state;
			const auto query_flags = Ngs2VoiceStateFlagsExperimentalEnabled()
			    ? Ngs2VoiceGetStateFlagsModel(handle, reinterpret_cast<uint64_t>(&flags.value))
			    : Ngs2VoiceGetStateFlagsModelImpl(handle, reinterpret_cast<uint64_t>(&flags.value), false);
			return query_flags == Ngs2VoiceStateFlagsResult::Ok && flags.before == 23 &&
			       flags.value == expected && flags.after == 29 &&
			       Ngs2VoiceGetState(handle, &state.value.voice_state, sizeof(state.value)) == OK &&
			       state.before == 31 && state.value.voice_state.state_flags == expected && state.after == 37;
		};
		if (!append(600, 0.25f, 101) || !append(900, -0.5f, 202) || !identity(101) ||
		    !visible_state(0)) return false;
		// Enter through the public control path as well as the render helper, so
		// trace ON/OFF covers receipt of an event before its first valid render.
		Ngs2VoiceEventParam play {{sizeof(Ngs2VoiceEventParam), 0, 0x00000006}, 0};
		if (Ngs2VoiceControl(handle, &play.header) != OK || voice->event != Ngs2VoicePlayEvent::Play) return false;
		if (!visible_state(0x3)) return false;
		// A rejected render leaves both the event and its visible state intact.
		descriptor.size -= sizeof(float);
		if (render() || voice->state != Ngs2VoicePlayState::Empty ||
		    voice->event != Ngs2VoicePlayEvent::Play || !visible_state(0x3)) return false;
		descriptor.size += sizeof(float);
		if (!render() || !range_matches(0, 512, 0.25f) || !identity(101) ||
		    !visible_state(0x3)) return false;
		// Invalid output cannot advance the queue or consolidate an event.
		descriptor.size -= sizeof(float);
		if (render() || audio.sampler_runtime->cursor_frames != 512 || !identity(101)) return false;
		descriptor.size += sizeof(float);
		if (!render() || !range_matches(0, 88, 0.25f) || !range_matches(88, 512, -0.5f) || !identity(202)) return false;
		if (!append(40, 0.75f, 303)) return false;
		Ngs2DecodedBlock invalid;
		invalid.pcm.assign(3, 1.0f);
		if (Ngs2QueueDecodedBlock(audio.sampler_runtime.get(), std::move(invalid), 2) ||
		    audio.sampler_runtime->cursor_frames != 424 || !identity(202)) return false;
		voice->event = Ngs2VoicePlayEvent::Pause;
		if (!visible_state(0x3) || !render() || !visible_state(0x5) ||
		    !range_matches(0, 512, 0.0f) || !identity(202) || audio.sampler_runtime->cursor_frames != 424) return false;
		voice->event = Ngs2VoicePlayEvent::Resume;
		if (!visible_state(0x5) || !render() || !visible_state(0x3) ||
		    !range_matches(0, 476, -0.5f) || !range_matches(476, 512, 0.75f) || !identity(303)) return false;
		voice->event = Ngs2VoicePlayEvent::Stop;
		if (!visible_state(0x3) || !render() || !visible_state(0xb) ||
		    !range_matches(0, 512, 0.0f) || audio.sampler_runtime->cursor_frames != 36 || !identity(303)) return false;
		voice->event = Ngs2VoicePlayEvent::Play;
		if (!visible_state(0xb) || !render() || !visible_state(0) ||
		    !range_matches(0, 4, 0.75f) || !range_matches(4, 512, 0.0f) || voice->state != Ngs2VoicePlayState::Empty) return false;
		if (!render() || !range_matches(0, 512, 0.0f) || !identity(303)) return false;
		voice->event = Ngs2VoicePlayEvent::Play;
		if (!render() || !range_matches(0, 40, 0.75f) || !range_matches(40, 512, 0.0f)) return false;
		// EOF append replaces the retained final block but never starts playback.
		if (!append(512, -0.25f, 404) || !render() || !range_matches(0, 512, 0.0f) || !identity(404)) return false;
		if (!append(12, 0.5f, 505) || !append(500, 0.125f, 606)) return false;
		voice->event = Ngs2VoicePlayEvent::Play;
		if (!render() || !range_matches(0, 512, -0.25f) || !identity(505) || audio.sampler_runtime->cursor_frames != 0) return false;
		if (!render() || !range_matches(0, 12, 0.5f) || !range_matches(12, 512, 0.125f) || !identity(606)) return false;
		for (const auto event: {Ngs2VoicePlayEvent::Kill, Ngs2VoicePlayEvent::StopImm})
		{
			voice->event = event;
			if (!render() || audio.sampler_runtime != nullptr || !identity(0)) return false;
			audio.sampler_runtime = std::make_unique<Ngs2SamplerRuntime>();
			if (!append(10, 0.25f, 707)) return false;
		}
		// Bound rejection, including replacement of a consumed last block.
		Ngs2SamplerRuntime bounded;
		for (size_t i = 0; i < 4096; ++i)
			if (!Ngs2QueueDecodedBlock(&bounded, Ngs2DecodedBlock {{0.25f, 0.25f}, i}, 2)) return false;
		if (Ngs2QueueDecodedBlock(&bounded, Ngs2DecodedBlock {{0.5f, 0.5f}, 9000}, 2) ||
		    bounded.blocks.size() != 4096 || bounded.retained_pcm_samples != 8192) return false;
		bounded.blocks.clear();
		bounded.cursor_frames = 0;
		bounded.retained_pcm_samples = NGS2_AUDIO_RENDER_MAX_PCM_SAMPLES; // arithmetic boundary without a 256 MiB fixture
		if (Ngs2QueueDecodedBlock(&bounded, Ngs2DecodedBlock {{0.5f, 0.5f}, 9000}, 2) || !bounded.blocks.empty()) return false;
		// Install a real synthetic ATRAC9 block at nonzero offset: echo its base,
		// then mutate the encoded source to prove render owns its decoded snapshot.
		std::vector<uint8_t> encoded(1040);
		int bit = 0;
		uint32_t random = 0x4b595459u;
		for (unsigned frame = 0; frame < 4; ++frame)
			Ngs2TestWriteStereoAtrac9Frame(encoded.data() + 16, &bit, frame == 0, &random);
		Ngs2TestWriteRandomBits(encoded.data() + 16, &bit, 8192 - bit, &random);
		Ngs2SamplerFormatParam format {};
		format.header.size = sizeof(format); format.header.id = 0x10000000;
		format.type = NGS2_WAVEFORM_TYPE_ATRAC9; format.channels = 2; format.sample_rate = 48000; format.config_data = 0xfe741ff0;
		{
			Ngs2HandoffOrderingProbe probe;
			probe.system = Ngs2FindSystemLocked(system);
			probe.address = reinterpret_cast<uint64_t>(&format);
			probe.bytes = sizeof(format);
			Ngs2ScopedHandoffProbe active(&probe);
			if (Ngs2VoiceControl(handle, &format.header) != OK || probe.calls < 2 || !probe.unlocked || !probe.precise ||
			    audio.sampler_runtime != nullptr) return false;
		}
		if (!identity(0)) return false;
		Ngs2SamplerBlockDescriptor block_descriptor {16, 1024, 0, 0, 1024, 0};
		Ngs2SamplerBlockParam param {{sizeof(Ngs2SamplerBlockParam), 0, 0x10000001}, encoded.data(), 1, 1, &block_descriptor};
		{
			Ngs2HandoffOrderingProbe probe;
			probe.system = Ngs2FindSystemLocked(system);
			probe.address = reinterpret_cast<uint64_t>(&param);
			probe.bytes = sizeof(param);
			probe.additional[0] = {reinterpret_cast<uint64_t>(&block_descriptor), sizeof(block_descriptor),
			                       GuestCpu::MemoryAccessIntent::Read};
			probe.additional[1] = {reinterpret_cast<uint64_t>(encoded.data() + 16), 1024,
			                       GuestCpu::MemoryAccessIntent::Read};
			Ngs2ScopedHandoffProbe active(&probe);
			if (Ngs2VoiceControl(handle, &param.header) != OK || probe.calls < 4 || !probe.unlocked || !probe.precise) return false;
		}
		if (!identity(reinterpret_cast<uint64_t>(encoded.data()))) return false;
		const auto expected = audio.sampler_runtime->blocks.front().pcm;
		block_descriptor.encoded_size = 0;
		if (Ngs2InstallSamplerBlock(voice, &param.header) || audio.sampler_runtime->blocks.front().pcm != expected) return false;
		std::fill(encoded.begin(), encoded.end(), 0);
		voice->event = Ngs2VoicePlayEvent::Play;
		if (!render()) return false;
		for (size_t frame = 0; frame < 512; ++frame)
			for (size_t channel = 0; channel < 8; ++channel)
				if (output.pcm[frame * 8 + channel] != (channel < 2 ? std::clamp(expected[frame * 2 + channel], -1.0f, 1.0f) : 0.0f)) return false;
		// The actual block installer must carry history across a preserved FIFO,
		// including after the already-decoded guest payload has been overwritten.
		const auto first = Ngs2TestMakeStereoAtrac9Superframe(0x4b595459u);
		auto second = Ngs2TestMakeStereoAtrac9Superframe(0x4c5a555au);
		auto third = Ngs2TestMakeStereoAtrac9Superframe(0x4d5b565bu);
		std::vector<uint8_t> joined(first);
		joined.insert(joined.end(), second.begin(), second.end());
		joined.insert(joined.end(), third.begin(), third.end());
		std::vector<float> continuous, fresh_second, fresh_third;
		if (!Ngs2DecodeAtrac9Snapshot(0xfe741ff0u, 2, 48000, joined, 0, 3072, &continuous) ||
		    !Ngs2DecodeAtrac9Snapshot(0xfe741ff0u, 2, 48000, second, 0, 1024, &fresh_second) ||
		    !Ngs2DecodeAtrac9Snapshot(0xfe741ff0u, 2, 48000, third, 0, 1024, &fresh_third)) return false;
		block_descriptor = {0, 1024, 0, 0, 1024, 0};
		param.data = second.data();
		if (!Ngs2InstallSamplerBlock(voice, &param.header) || audio.sampler_runtime->blocks.size() != 2 ||
		    audio.sampler_runtime->decoder == nullptr || audio.sampler_runtime->cursor_frames != 512 ||
		    !std::equal(audio.sampler_runtime->blocks.back().pcm.begin(), audio.sampler_runtime->blocks.back().pcm.end(),
		                continuous.begin() + 2048)) return false;
		const void* const accepted_history = audio.sampler_runtime->decoder.get();
		const size_t retained = audio.sampler_runtime->retained_pcm_samples;
		std::vector<uint8_t> invalid_packet(third);
		auto invalid_tail = first;
		invalid_tail[0] |= 0x40u;
		invalid_packet.insert(invalid_packet.end(), invalid_tail.begin(), invalid_tail.end());
		param.data = invalid_packet.data(); block_descriptor.encoded_size = 2048; block_descriptor.playable_samples = 2048;
		if (Ngs2InstallSamplerBlock(voice, &param.header) || audio.sampler_runtime->decoder.get() != accepted_history ||
		    audio.sampler_runtime->blocks.size() != 2 || audio.sampler_runtime->retained_pcm_samples != retained) return false;
		param.data = third.data(); block_descriptor.encoded_size = 1024; block_descriptor.playable_samples = 1024;
		audio.sampler_runtime->retained_pcm_samples = NGS2_AUDIO_RENDER_MAX_PCM_SAMPLES;
		if (Ngs2InstallSamplerBlock(voice, &param.header) || audio.sampler_runtime->decoder.get() != accepted_history ||
		    audio.sampler_runtime->blocks.size() != 2) return false;
		audio.sampler_runtime->retained_pcm_samples = retained;
		if (!Ngs2InstallSamplerBlock(voice, &param.header) || audio.sampler_runtime->blocks.size() != 3 ||
		    !std::equal(audio.sampler_runtime->blocks.back().pcm.begin(), audio.sampler_runtime->blocks.back().pcm.end(),
		                continuous.begin() + 4096)) return false;
		// Same-format cid0 is a source reset; a completed last block also starts
		// fresh when replaced. Neither source boundary inherits the old history.
		if (!Ngs2InstallSamplerFormat(voice, &format.header) || audio.sampler_runtime != nullptr) return false;
		param.data = second.data();
		if (!Ngs2InstallSamplerBlock(voice, &param.header) || audio.sampler_runtime->blocks.front().pcm != fresh_second)
			return false;
		audio.sampler_runtime->cursor_frames = 1024;
		voice->state = Ngs2VoicePlayState::Empty;
		param.data = third.data();
		if (!Ngs2InstallSamplerBlock(voice, &param.header) || audio.sampler_runtime->blocks.size() != 1 ||
		    audio.sampler_runtime->cursor_frames != 0 || audio.sampler_runtime->blocks.front().pcm != fresh_third ||
		    voice->state != Ngs2VoicePlayState::Empty) return false;
		// Explicit priming starts a fresh packet even with a retained FIFO. A
		// trimmed packet cannot publish history at an unrepresented encoded end.
		block_descriptor.skip_samples = 1; block_descriptor.playable_samples = 1023;
		if (!Ngs2InstallSamplerBlock(voice, &param.header) || audio.sampler_runtime->decoder == nullptr ||
		    !std::equal(audio.sampler_runtime->blocks.back().pcm.begin(), audio.sampler_runtime->blocks.back().pcm.end(),
		                fresh_third.begin() + 2)) return false;
		block_descriptor.skip_samples = 0; block_descriptor.playable_samples = 600;
		if (!Ngs2InstallSamplerBlock(voice, &param.header) || audio.sampler_runtime->decoder != nullptr) return false;
		block_descriptor.playable_samples = 1024; param.data = second.data();
		if (!Ngs2InstallSamplerBlock(voice, &param.header) || audio.sampler_runtime->blocks.back().pcm != fresh_second)
			return false;
		for (const auto event: {Ngs2VoicePlayEvent::Kill, Ngs2VoicePlayEvent::StopImm})
		{
			if (audio.sampler_runtime == nullptr && !Ngs2InstallSamplerBlock(voice, &param.header)) return false;
			voice->event = event;
			if (!render() || audio.sampler_runtime != nullptr || !identity(0)) return false;
		}
		return true;
	}();
	return ResetForNextSession() && passed;
}

static bool Ngs2AudioRenderSyntheticTest()
{
	if (!Ngs2Atrac9DecodeSyntheticTest() || !Ngs2Atrac9ResampleSyntheticTest()) return false;
	Ngs2BufferAllocator allocator {};
	allocator.alloc_handler = Ngs2SessionResetTestAlloc;
	allocator.free_handler  = Ngs2SessionResetTestFree;

	Ngs2SystemOption system_option {};
	system_option.size              = sizeof(system_option);
	system_option.max_grain_samples = 512;
	system_option.num_grain_samples = 512;
	system_option.sample_rate       = 48000;
	Ngs2SamplerRackOption rack_option {};
	rack_option.rack_option.size              = sizeof(rack_option);
	rack_option.rack_option.max_grain_samples = 512;
	rack_option.rack_option.max_voices        = 1;
	rack_option.max_waveform_blocks           = 1;

	uintptr_t system_handle = 0;
	uintptr_t rack_handle   = 0;
	uintptr_t voice_handle  = 0;
	if (Ngs2SystemCreateWithAllocator(&system_option, &allocator, &system_handle) != OK ||
	    Ngs2RackCreateWithAllocator(system_handle, 0x1000, &rack_option.rack_option,
	                                &allocator, &rack_handle) != OK ||
	    Ngs2RackGetVoiceHandle(rack_handle, 0, &voice_handle) != OK)
	{
		(void)ResetForNextSession();
		return false;
	}

	auto* voice = Ngs2FindVoiceLocked(voice_handle, nullptr);
	auto& audio_state = g_ngs2_voice_audio_states[voice];
	audio_state.waveform_channels = 1;
	audio_state.sampler_runtime = std::make_unique<Ngs2SamplerRuntime>();
	audio_state.sampler_runtime->blocks.emplace_back();
	audio_state.sampler_runtime->blocks.front().pcm.assign(1024, 0.25f);
	audio_state.sampler_runtime->blocks.front().pcm[0]   = 0.25f;
	audio_state.sampler_runtime->blocks.front().pcm[1]   = -0.5f;
	audio_state.sampler_runtime->blocks.front().pcm[2]   = 0.75f;
	audio_state.sampler_runtime->blocks.front().pcm[3]   = -1.0f;
	audio_state.sampler_runtime->blocks.front().pcm[512] = 0.125f;
	audio_state.sampler_runtime->blocks.front().pcm[513] = -0.25f;
	audio_state.sampler_runtime->blocks.front().pcm[514] = 0.5f;
	audio_state.sampler_runtime->blocks.front().pcm[515] = -0.75f;
	audio_state.sampler_runtime->retained_pcm_samples = audio_state.sampler_runtime->blocks.front().pcm.size();

	Ngs2VoiceEventParam play {};
	play.header.size = sizeof(play);
	play.header.id   = 0x00000006;
	play.event_id    = 0;
	if (Ngs2VoiceControl(voice_handle, &play.header) != OK)
	{
		(void)ResetForNextSession();
		return false;
	}

	struct OutputFixture
	{
		uint64_t              before = UINT64_C(0x123456789abcdef0);
		std::array<float, 4096> samples {};
		uint64_t              after = UINT64_C(0xfedcba9876543210);
	} fixture;
	fixture.samples.fill(17.0f);
	Ngs2RenderBufferSnapshot descriptor {};
	descriptor.data     = reinterpret_cast<uint64_t>(fixture.samples.data());
	descriptor.size     = fixture.samples.size() * sizeof(float) - sizeof(float);
	descriptor.type     = NGS2_RENDER_WAVEFORM_TYPE_FLOAT32;
	descriptor.channels = 8;

	bool invalid_rejected = false;
	bool first_render      = false;
	bool second_render     = false;
	{
		std::lock_guard registry_lock(g_ngs_registry_mutex);
		Core::LockGuard system_lock(Ngs2FindSystemLocked(system_handle)->mutex);
		invalid_rejected = !Ngs2RenderAudioLocked(
		    Ngs2FindSystemLocked(system_handle),
		    reinterpret_cast<const Ngs2RenderBufferInfo*>(&descriptor), 1) &&
		                   voice->state == Ngs2VoicePlayState::Empty &&
		                   voice->event == Ngs2VoicePlayEvent::Play &&
		                   audio_state.sampler_runtime->cursor_frames == 0;
		descriptor.size = fixture.samples.size() * sizeof(float);
		first_render = Ngs2RenderAudioLocked(
		    Ngs2FindSystemLocked(system_handle),
		    reinterpret_cast<const Ngs2RenderBufferInfo*>(&descriptor), 1);
	}
	const auto sample_matches = [&](size_t frame, float expected)
	{
		const size_t base = frame * descriptor.channels;
		if (fixture.samples[base] != expected || fixture.samples[base + 1] != expected) return false;
		for (uint32_t channel = 2; channel < descriptor.channels; ++channel)
			if (fixture.samples[base + channel] != 0.0f) return false;
		return true;
	};
	const bool first_matches = first_render && voice->state == Ngs2VoicePlayState::Playing &&
	                           audio_state.sampler_runtime->cursor_frames == 512 &&
	                           sample_matches(0, 0.25f) && sample_matches(1, -0.5f) &&
	                           sample_matches(2, 0.75f) && sample_matches(3, -1.0f);
	{
		std::lock_guard registry_lock(g_ngs_registry_mutex);
		Core::LockGuard system_lock(Ngs2FindSystemLocked(system_handle)->mutex);
		second_render = Ngs2RenderAudioLocked(
		    Ngs2FindSystemLocked(system_handle),
		    reinterpret_cast<const Ngs2RenderBufferInfo*>(&descriptor), 1);
	}
	const bool second_matches = second_render && voice->state == Ngs2VoicePlayState::Empty &&
	                            audio_state.sampler_runtime->cursor_frames == 1024 &&
	                            sample_matches(0, 0.125f) && sample_matches(1, -0.25f) &&
	                            sample_matches(2, 0.5f) && sample_matches(3, -0.75f);
	const bool canaries_match = fixture.before == UINT64_C(0x123456789abcdef0) &&
	                            fixture.after == UINT64_C(0xfedcba9876543210);
	return invalid_rejected && first_matches && second_matches && canaries_match && ResetForNextSession();
}

bool VoiceGetStateOutputContractTest(size_t page_size)
{
	namespace Vm = Core::VirtualMemory;
	if (page_size < sizeof(Ngs2MasteringVoiceState) * 2 || page_size > SIZE_MAX / 2 ||
	    Ngs2AudioRenderExperimentalEnabled())
		return false;
	{
		std::lock_guard registry_lock(g_ngs_registry_mutex);
		if (g_ngs_list != nullptr || g_racks_list != nullptr) return false;
	}
	struct SessionCleanup
	{
		~SessionCleanup() { (void)ResetForNextSession(); }
	} session_cleanup;
	struct OutputMapping
	{
		explicit OutputMapping(size_t bytes): address(Vm::Alloc(0, bytes, Vm::Mode::ReadWrite)) {}
		~OutputMapping() { if (address != 0) (void)Vm::Free(address); }
		uint64_t address;
	} output(page_size * 2);
	if (output.address == 0) return false;
	auto* bytes = reinterpret_cast<uint8_t*>(output.address);
	const size_t mapping_size = page_size * 2;

	Ngs2BufferAllocator allocator {};
	allocator.alloc_handler = Ngs2SessionResetTestAlloc;
	allocator.free_handler = Ngs2SessionResetTestFree;
	Ngs2SystemOption system_option {};
	system_option.size = sizeof(system_option);
	Ngs2SamplerRackOption sampler_option {};
	sampler_option.rack_option.size = sizeof(sampler_option);
	sampler_option.rack_option.max_voices = 8;
	sampler_option.max_waveform_blocks = 1;
	Ngs2MasteringRackOption mastering_option {};
	mastering_option.rack_option.size = sizeof(mastering_option);
	mastering_option.rack_option.max_voices = 8;
	uintptr_t system_handle = 0;
	std::array<uintptr_t, 2> racks {};
	std::array<uintptr_t, 2> voices {};
	if (Ngs2SystemCreateWithAllocator(&system_option, &allocator, &system_handle) != OK ||
	    Ngs2RackCreateWithAllocator(system_handle, 0x1000, &sampler_option.rack_option,
	                                &allocator, &racks[0]) != OK ||
	    Ngs2RackCreateWithAllocator(system_handle, 0x3000, &mastering_option.rack_option,
	                                &allocator, &racks[1]) != OK ||
	    Ngs2RackGetVoiceHandle(racks[0], 0, &voices[0]) != OK ||
	    Ngs2RackGetVoiceHandle(racks[1], 0, &voices[1]) != OK)
		return false;
	static_assert(sizeof(Ngs2SamplerVoiceState) == 48);
	static_assert(sizeof(Ngs2MasteringVoiceState) == 76);
	const std::array<size_t, 2> sizes {{48, 76}};
	const std::array<Ngs2VoicePlayState, 4> states {{
	    Ngs2VoicePlayState::Empty, Ngs2VoicePlayState::Playing,
	    Ngs2VoicePlayState::Paused, Ngs2VoicePlayState::Stopped}};
	const std::array<uint8_t, 4> flags {{0, 3, 5, 11}};
	auto expected_output = [&](size_t index, size_t offset, uint8_t state_flags)
	{
		std::vector<uint8_t> expected(mapping_size, 0xa5);
		std::fill_n(expected.data() + offset, sizes[index], 0);
		expected[offset] = state_flags;
		if (index == 0)
		{
			expected[offset + 6] = 0x80; // Float32 envelope_height = 1.0, little-endian.
			expected[offset + 7] = 0x3f;
		}
		return expected;
	};
	auto untouched = [&]
	{
		return std::all_of(bytes, bytes + mapping_size, [](uint8_t byte) { return byte == 0xa5; });
	};
	for (size_t index = 0; index < voices.size(); ++index)
	{
		auto* voice = Ngs2FindVoiceLocked(voices[index], nullptr);
		for (size_t state_index = 0; state_index < states.size(); ++state_index)
		{
			voice->state = states[state_index];
			voice->event = Ngs2VoicePlayEvent::Resume;
			for (const size_t offset: {size_t {16}, size_t {17}})
			{
				std::memset(bytes, 0xa5, mapping_size);
				const auto expected = expected_output(index, offset, flags[state_index]);
				if (!Ngs2VoiceGetStateModelImpl(voices[index], output.address + offset, sizes[index]) ||
				    std::memcmp(bytes, expected.data(), mapping_size) != 0 ||
				    voice->state != states[state_index] || voice->event != Ngs2VoicePlayEvent::Resume)
					return false;
			}
		}
		std::memset(bytes, 0xa5, mapping_size);
		const std::array<uintptr_t, 6> invalid_handles {{
		    0, UINTPTR_MAX, static_cast<uintptr_t>(output.address), voices[index] + 1, racks[index], system_handle}};
		for (const uintptr_t invalid_handle: invalid_handles)
			if (Ngs2VoiceGetStateModelImpl(invalid_handle, output.address + 17, sizes[index]) || !untouched())
				return false;
		for (const size_t invalid_size: {size_t {0}, sizes[index] - 1, sizes[index] + 1, SIZE_MAX})
			if (Ngs2VoiceGetStateModelImpl(voices[index], output.address + 17, invalid_size) || !untouched())
				return false;
		for (const uint64_t invalid_output: {uint64_t {0}, UINT64_MAX, UINT64_MAX - sizes[index] + 2})
			if (Ngs2VoiceGetStateModelImpl(voices[index], invalid_output, sizes[index]) || !untouched())
				return false;

		if (!Vm::Protect(output.address, page_size, Vm::Mode::Read)) return false;
		const bool read_only_rejected =
		    !Ngs2VoiceGetStateModelImpl(voices[index], output.address + 17, sizes[index]) && untouched();
		if (!Vm::Protect(output.address, page_size, Vm::Mode::ReadWrite) || !read_only_rejected) return false;
		if (!Vm::Protect(output.address + page_size, page_size, Vm::Mode::NoAccess)) return false;
		const bool cross_guard_rejected = !Ngs2VoiceGetStateModelImpl(
		    voices[index], output.address + page_size - sizes[index] + 1, sizes[index]);
		if (!Vm::Protect(output.address + page_size, page_size, Vm::Mode::ReadWrite) ||
		    !cross_guard_rejected || !untouched())
			return false;

		const size_t tail_offset = page_size - sizes[index];
		const auto expected = expected_output(index, tail_offset, 11);
		if (!Vm::Protect(output.address + page_size, page_size, Vm::Mode::NoAccess)) return false;
		const bool exact_tail_accepted = Ngs2VoiceGetStateModelImpl(
		    voices[index], output.address + tail_offset, sizes[index]);
		if (!Vm::Protect(output.address + page_size, page_size, Vm::Mode::ReadWrite) ||
		    !exact_tail_accepted || std::memcmp(bytes, expected.data(), mapping_size) != 0)
			return false;

		for (size_t aliased_index = 0; aliased_index < racks.size(); ++aliased_index)
		{
			const size_t rack_bytes = sizeof(Ngs2RackInternal) + 8 * sizeof(Ngs2VoiceInternal);
			const auto* rack_data = reinterpret_cast<const uint8_t*>(racks[aliased_index]);
			const std::vector<uint8_t> before(rack_data, rack_data + rack_bytes);
			for (const uintptr_t alias: {racks[aliased_index], voices[aliased_index], voices[aliased_index] - 1})
				if (Ngs2VoiceGetStateModelImpl(voices[index], alias, sizes[index]) ||
				    std::memcmp(rack_data, before.data(), rack_bytes) != 0)
					return false;
		}
		if (Ngs2VoiceGetStateModelImpl(voices[index], system_handle, sizes[index])) return false;
	}
	std::memset(bytes, 0xa5, mapping_size);
	if (!ResetForNextSession()) return false;
	for (size_t index = 0; index < voices.size(); ++index)
		if (Ngs2VoiceGetStateModelImpl(voices[index], output.address + 17, sizes[index]) || !untouched())
			return false;
	return true;
}

bool SessionResetContractTest()
{
	if (!GuestCpu::RegisterMemoryHandoffQuery(Ngs2HandoffOrderingQuery)) return false;
	{
		std::lock_guard registry_lock(g_ngs_registry_mutex);
		if (g_ngs_list != nullptr || g_racks_list != nullptr) return false;
	}
	g_ngs_test_controls_unchanged = true;
	if (!Ngs2AudioRenderSyntheticTest() || !g_ngs_test_controls_unchanged) return false;

	g_ngs_test_allocations = 0;
	g_ngs_test_frees       = 0;
	Ngs2BufferAllocator allocator {};
	allocator.alloc_handler = Ngs2SessionResetTestAlloc;
	allocator.free_handler  = Ngs2SessionResetTestFree;
	allocator.user_data     = 0x1234;

	for (int cycle = 0; cycle < 2; cycle++)
	{
		Ngs2SystemOption system_option {};
		system_option.size = sizeof(system_option);
		Ngs2MasteringRackOption rack_option {};
		rack_option.rack_option.size       = sizeof(rack_option);
		rack_option.rack_option.max_voices = 2;

		std::array<uintptr_t, 2> system_handles {};
		std::array<uintptr_t, 2> rack_handles {};
		for (size_t index = 0; index < system_handles.size(); ++index)
		{
			if (Ngs2SystemCreateWithAllocator(&system_option, &allocator, &system_handles[index]) != OK ||
			    system_handles[index] == 0 ||
			    Ngs2RackCreateWithAllocator(system_handles[index], 0x3000, &rack_option.rack_option,
			                                &allocator, &rack_handles[index]) != OK ||
			    rack_handles[index] == 0)
			{
				(void)ResetForNextSession();
				return false;
			}
		}

		std::array<uintptr_t, 3> voice_handles {};
		if (Ngs2RackGetVoiceHandle(rack_handles[0], 0, &voice_handles[0]) != OK ||
		    Ngs2RackGetVoiceHandle(rack_handles[0], 1, &voice_handles[1]) != OK ||
		    Ngs2RackGetVoiceHandle(rack_handles[1], 0, &voice_handles[2]) != OK ||
		    voice_handles[0] == 0 || voice_handles[1] == 0 || voice_handles[2] == 0)
		{
			(void)ResetForNextSession();
			return false;
		}

		struct
		{
			uint32_t before = 0x12345678;
			uint32_t value  = 0xa5a5a5a5;
			uint32_t after  = 0x89abcdef;
		} flags_fixture;
		auto flags_match = [&](uintptr_t handle, uint32_t expected_flags)
		{
			flags_fixture.before = 0x12345678;
			flags_fixture.value  = 0xa5a5a5a5;
			flags_fixture.after  = 0x89abcdef;
			Ngs2HandoffOrderingProbe probe;
			probe.system = Ngs2FindSystemLocked(system_handles[0]);
			probe.address = reinterpret_cast<uint64_t>(&flags_fixture.value);
			probe.bytes = sizeof(flags_fixture.value);
			probe.intent = GuestCpu::MemoryAccessIntent::Write;
			Ngs2ScopedHandoffProbe active(&probe);
			return Ngs2VoiceGetStateFlagsModelImpl(
			           handle, reinterpret_cast<uint64_t>(&flags_fixture.value), false) == Ngs2VoiceStateFlagsResult::Ok &&
			       flags_fixture.before == 0x12345678 && flags_fixture.value == expected_flags &&
			       flags_fixture.after == 0x89abcdef && probe.calls != 0 && probe.unlocked && probe.precise;
		};
		auto flags_rejected = [&](uintptr_t handle, uint64_t output, Ngs2VoiceStateFlagsResult expected)
		{
			flags_fixture.before = 0x12345678;
			flags_fixture.value  = 0xa5a5a5a5;
			flags_fixture.after  = 0x89abcdef;
			return Ngs2VoiceGetStateFlagsModelImpl(handle, output, false) == expected &&
			       flags_fixture.before == 0x12345678 && flags_fixture.value == 0xa5a5a5a5 &&
			       flags_fixture.after == 0x89abcdef;
		};

		struct
		{
			uint32_t                 before = 0x12345678;
			Ngs2MasteringVoiceState state {};
			uint32_t                 after = 0x89abcdef;
		} state_fixture;
		auto full_state_matches = [&](uint32_t expected_flags)
		{
			state_fixture.before = 0x12345678;
			state_fixture.after  = 0x89abcdef;
			std::memset(&state_fixture.state, 0xa5, sizeof(state_fixture.state));
			Ngs2HandoffOrderingProbe probe;
			probe.system = Ngs2FindSystemLocked(system_handles[0]);
			probe.address = reinterpret_cast<uint64_t>(&state_fixture.state);
			probe.bytes = sizeof(state_fixture.state);
			probe.intent = GuestCpu::MemoryAccessIntent::Write;
			Ngs2ScopedHandoffProbe active(&probe);
			if (Ngs2VoiceGetState(voice_handles[0], &state_fixture.state.voice_state,
			                     sizeof(state_fixture.state)) != OK ||
			    state_fixture.before != 0x12345678 || state_fixture.after != 0x89abcdef ||
			    state_fixture.state.voice_state.state_flags != expected_flags ||
			    probe.calls == 0 || !probe.unlocked || !probe.precise)
				return false;
			for (uint8_t value: state_fixture.state.opaque)
				if (value != 0) return false;
			return true;
		};
		auto queue_event = [&](uintptr_t handle, uint32_t event_id)
		{
			Ngs2VoiceEventParam event {};
			event.header.size = sizeof(event);
			event.header.next = 0;
			event.header.id   = 0x00000006;
			event.event_id    = event_id;
			Ngs2HandoffOrderingProbe probe;
			probe.system = Ngs2FindSystemLocked(system_handles[0]);
			probe.address = reinterpret_cast<uint64_t>(&event);
			probe.bytes = sizeof(event);
			Ngs2ScopedHandoffProbe active(&probe);
			return Ngs2VoiceControl(handle, &event.header) == OK && probe.calls >= 2 && probe.unlocked && probe.precise;
		};
		uint8_t render_buffer = 0;
		auto render = [&](uintptr_t system)
		{
			return Ngs2SystemRender(system, reinterpret_cast<const Ngs2RenderBufferInfo*>(&render_buffer), 1) == OK;
		};
		auto isolated = [&](uint32_t primary)
		{
			return flags_match(voice_handles[0], primary) && flags_match(voice_handles[1], 0) &&
			       flags_match(voice_handles[2], 0);
		};

		const uint32_t pending_play_flags = Ngs2AudioRenderExperimentalEnabled() ? 0x3 : 0;
		if (!isolated(0) || !full_state_matches(0) ||
		    !queue_event(voice_handles[0], 0) || !isolated(pending_play_flags) ||
		    !full_state_matches(pending_play_flags) || !render(system_handles[0]) ||
		    !isolated(0x3) || !full_state_matches(0x3) ||
		    !queue_event(voice_handles[0], 4) || !isolated(0x3) || !render(system_handles[0]) ||
		    !isolated(0x5) || !full_state_matches(0x5) ||
		    !queue_event(voice_handles[0], 5) || !isolated(0x5) || !render(system_handles[0]) ||
		    !isolated(0x3) || !queue_event(voice_handles[0], 1) || !isolated(0x3) ||
		    !render(system_handles[0]) || !isolated(0xb) || !full_state_matches(0xb) ||
		    !flags_rejected(0, reinterpret_cast<uint64_t>(&flags_fixture.value),
		                    Ngs2VoiceStateFlagsResult::InvalidHandle) ||
		    !flags_rejected(reinterpret_cast<uintptr_t>(&flags_fixture),
		                    reinterpret_cast<uint64_t>(&flags_fixture.value),
		                    Ngs2VoiceStateFlagsResult::InvalidHandle) ||
		    !flags_rejected(voice_handles[0] + 1, reinterpret_cast<uint64_t>(&flags_fixture.value),
		                    Ngs2VoiceStateFlagsResult::InvalidHandle) ||
		    !flags_rejected(voice_handles[0], 0, Ngs2VoiceStateFlagsResult::InvalidOutput) ||
		    !flags_rejected(voice_handles[0], UINT64_MAX, Ngs2VoiceStateFlagsResult::InvalidOutput) ||
		    Ngs2VoiceGetStateFlagsModelImpl(voice_handles[0], system_handles[0], false) !=
		        Ngs2VoiceStateFlagsResult::AliasedMemory ||
		    Ngs2VoiceGetStateFlagsModelImpl(voice_handles[0], rack_handles[0], false) !=
		        Ngs2VoiceStateFlagsResult::AliasedMemory ||
		    Ngs2VoiceGetStateFlagsModelImpl(voice_handles[0], voice_handles[0], false) !=
		        Ngs2VoiceStateFlagsResult::AliasedMemory ||
		    !isolated(0xb))
		{
			(void)ResetForNextSession();
			return false;
		}

		const uintptr_t stale_voice = voice_handles[0];
		const int       expected = (cycle + 1) * 4;
		if (g_ngs_test_allocations != expected || !ResetForNextSession() ||
		    g_ngs_test_frees != expected ||
		    Ngs2VoiceGetStateFlagsModelImpl(stale_voice, reinterpret_cast<uint64_t>(&flags_fixture.value), false) !=
		        Ngs2VoiceStateFlagsResult::InvalidHandle)
			return false;
		{
			std::lock_guard registry_lock(g_ngs_registry_mutex);
			if (g_ngs_list != nullptr || g_racks_list != nullptr) return false;
		}
	}
	return g_ngs_test_controls_unchanged;
}
#endif

} // namespace Ngs2

bool ResetForNextSession()
{
	bool ok = AvPlayer::ResetForNextSession();
	if (!Audio3d::ResetForNextSession())
	{
		ok = false;
	}
	if (!Ngs2::ResetForNextSession())
	{
		ok = false;
	}
	if (!ResetBasePortsForNextSession())
	{
		ok = false;
	}
	return ok;
}

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool AudioSessionResetContractTest()
{
	return AudioBasePortsSessionResetContractTest() && AvPlayer::SessionResetContractTest() && Audio3d::SessionResetContractTest() &&
	       Ngs2::SessionResetContractTest();
}
#endif

} // namespace Lumen::Libs::Audio

#endif // LUMEN_EMU_ENABLED
