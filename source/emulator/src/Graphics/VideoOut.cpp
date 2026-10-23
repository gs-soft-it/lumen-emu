#include "Emulator/Graphics/VideoOut.h"

#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/LinkList.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Timer.h"
#include "Lumen/Core/Vector.h"
#include "Lumen/Core/VirtualMemory.h"

#if defined(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY) || \
    defined(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY) || \
    defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE) || \
    defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE) || \
    defined(LUMEN_A64_DYNAREC_PHASE6CM_PROFILE) || \
    defined(LUMEN_A64_DYNAREC_PHASE6CO_PROFILE)
#include "CpuX64.h"
#endif
#include "Emulator/Common.h"
#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Profiler.h"
#include "MemoryHandoff.h"

#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && defined(LUMEN_ANDROID_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME))
#include "../../../../android/app/src/main/cpp/AndroidFexCoreRuntime.h"
#endif
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
#include "../../../../android/app/src/main/cpp/AndroidFexCoreMutationBarrier.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <vector>
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#include <android/log.h>
#endif
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
#include <chrono>
#include <cstring>
#include <thread>
#endif

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {
struct GraphicContext;
} // namespace Lumen::Libs::Graphics

namespace Lumen::Libs::VideoOut {

LIB_NAME("VideoOut", "VideoOut");

namespace EventQueue = LibKernel::EventQueue;

static bool write_guest_output(void* destination, const void* source, size_t size)
{
	if (destination == nullptr || source == nullptr) return false;
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	return Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(destination), source, size);
#else
	std::memcpy(destination, source, size);
	return true;
#endif
}

static bool read_guest_input(const void* source, void* destination, size_t size)
{
	if (source == nullptr || destination == nullptr) return false;
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	return Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(source), destination, size);
#else
	std::memcpy(destination, source, size);
	return true;
#endif
}

bool VideoOutEventDataEnabled()
{
	// Freeze the selector for the process, including every session and both
	// producer and import registration. A live environment change cannot split
	// the representation from the decoder that was installed by InitAll.
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_VIDEOOUT_EVENT_DATA");
		const bool requested = value != nullptr && value[0] == '1' && value[1] == '\0';
		if (requested) std::fputs("[VIDEOOUT_EVENT_DATA] enabled=1 experimental=1 local_payload=unsigned48 native_flip=unqualified\n", stderr);
		return requested;
	}();
	return enabled;
}

static constexpr uint64_t VIDEO_OUT_EVENT_PAYLOAD_MAX = UINT64_C(0x0000ffffffffffff);

VideoOutEventPayloadResult VideoOutEncodeEventPayload(uint64_t payload, uint64_t& encoded)
{
	if (!VideoOutEventDataEnabled())
	{
		encoded = payload;
		return VideoOutEventPayloadResult::LegacyRaw;
	}
	if (payload > VIDEO_OUT_EVENT_PAYLOAD_MAX) return VideoOutEventPayloadResult::OutOfRange;
	// An original local representation adapter. The low16 zero bits are not a
	// claim about native Flip events; their producer has not been qualified.
	encoded = payload << 16u;
	return VideoOutEventPayloadResult::Encoded;
}

uint64_t VideoOutPrepareEventPayload(uint64_t payload)
{
	uint64_t encoded = 0;
	if (VideoOutEncodeEventPayload(payload, encoded) == VideoOutEventPayloadResult::OutOfRange)
		EXIT("VideoOut event payload rejected: experimental unsigned48 representation cannot preserve the value\n");
	return encoded;
}

static VideoOutEventPayloadResult PrepareNextVblankEvent(uint64_t current, uint64_t& next, uint64_t& encoded)
{
	if (VideoOutEventDataEnabled() && current >= VIDEO_OUT_EVENT_PAYLOAD_MAX)
		return VideoOutEventPayloadResult::OutOfRange;
	const uint64_t candidate = current + 1;
	const auto result = VideoOutEncodeEventPayload(candidate, encoded);
	if (result != VideoOutEventPayloadResult::OutOfRange) next = candidate;
	return result;
}

static void TraceVideoOutEventData(uint64_t event, uint64_t output, uint64_t decoded, int result)
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_KERNEL_EVENT_DELIVERY_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	if (!enabled) return;
	static std::atomic<uint32_t> emitted {0};
	constexpr uint32_t limit = 64;
	uint32_t sequence = emitted.load(std::memory_order_relaxed);
	while (sequence <= limit && !emitted.compare_exchange_weak(sequence, sequence + 1, std::memory_order_relaxed)) {}
	if (sequence > limit) return;
	if (sequence == limit)
	{
		std::fprintf(stderr, "[VIDEOOUT_EVENT_DATA_RESULT] capped=1 limit=%u\n", limit);
		return;
	}
	std::fprintf(stderr,
	             "[VIDEOOUT_EVENT_DATA_RESULT] sequence=%u thread=%d event=0x%016" PRIx64
	             " output=0x%016" PRIx64 " decoded=0x%016" PRIx64 " result=0x%08" PRIx32 "\n",
	             sequence + 1, Core::Thread::GetThreadIdUnique(), event, output, decoded, static_cast<uint32_t>(result));
}

LUMEN_SYSV_ABI int VideoOutGetEventData(const EventQueue::KernelEvent* event, int64_t* data)
{
	EXIT_NOT_IMPLEMENTED(!VideoOutEventDataEnabled());
	static_assert(sizeof(EventQueue::KernelEvent) == 32 && offsetof(EventQueue::KernelEvent, data) == 16);
	static_assert(sizeof(*data) == 8);
	const uint64_t source = reinterpret_cast<uint64_t>(event);
	const uint64_t destination = reinterpret_cast<uint64_t>(data);
	// These pointer checks are defensive local policy, not observed native
	// error semantics. Keep the input immutable, including overlapping outputs.
	if (source == 0 || destination == 0 || source % alignof(EventQueue::KernelEvent) != 0 || destination % alignof(int64_t) != 0 ||
	    source > UINT64_MAX - sizeof(*event) || destination > UINT64_MAX - sizeof(*data) ||
	    (source < destination + sizeof(*data) && destination < source + sizeof(*event)))
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	EventQueue::KernelEvent snapshot {};
	if (!read_guest_input(event, &snapshot, sizeof(snapshot))) return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	if (snapshot.filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT) return VIDEO_OUT_ERROR_INVALID_EVENT;
	// Native08/09 establish this decoder for the tested records. Ignoring the
	// remaining metadata is the explicit general model, not native producer parity.
	const int64_t decoded = static_cast<int64_t>(static_cast<uint64_t>(snapshot.data) >> 16u);
	const int result = write_guest_output(data, &decoded, sizeof(decoded)) ? OK : VIDEO_OUT_ERROR_INVALID_ADDRESS;
	// The write helper has released its lease; trace only saved host values.
	TraceVideoOutEventData(source, destination, static_cast<uint64_t>(decoded), result);
	return result;
}

bool VideoOutModeInitEnabled()
{
	static const bool enabled = []()
	{
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_VIDEOOUT_MODE_INIT");
		const bool requested = value != nullptr && std::strcmp(value, "1") == 0;
		if (requested)
		{
			std::fputs("[VIDEOOUT_MODE_INIT] enabled: experimental/unverified initializer; local budget=65536 bytes\n", stderr);
		}
		return requested;
	}();
	return enabled;
}

bool VideoOutConfigureAnySdrEnabled()
{
	static const bool enabled = []()
	{
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_VIDEOOUT_CONFIGURE_ANY_SDR");
		const bool requested = value != nullptr && std::strcmp(value, "1") == 0;
		if (requested)
		{
			std::fputs("[VIDEOOUT_CONFIGURE_ANY_SDR] enabled: experimental/unverified all-any SDR configuration\n", stderr);
		}
		return requested;
	}();
	return enabled;
}

bool VideoOutMatchesAnySdrMode(const std::array<uint8_t, 32>& host_record)
{
	uint32_t size = 0;
	std::memcpy(&size, host_record.data(), sizeof(size));
	return size == host_record.size() &&
	       std::all_of(host_record.begin() + sizeof(size), host_record.end(), [](uint8_t byte) { return byte == 0xff; });
}

bool VideoOutTryInitializeMode(void* output, uint32_t size)
{
	// This budget limits host staging resources; it is not a PS4 record size.
	constexpr uint32_t local_budget = 64 * 1024;
	const uint64_t address = reinterpret_cast<uint64_t>(output);
	if (address == 0 || size < sizeof(size) || size > local_budget || address > UINT64_MAX - size) return false;
	auto bytes = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[size]);
	if (bytes == nullptr) return false;
	std::memset(bytes.get(), 0xff, size);
	std::memcpy(bytes.get(), &size, sizeof(size));
	// The checked writer validates the complete range before publishing it.
	// No guest reads, field-by-field writes or display changes are involved.
	return write_guest_output(output, bytes.get(), size);
}

LUMEN_SYSV_ABI void VideoOutModeSetAny(void* output, uint32_t size)
{
	PRINT_NAME();
	if (!VideoOutModeInitEnabled() || !VideoOutTryInitializeMode(output, size))
	{
		EXIT("VideoOutModeSetAny: experimental initialization rejected, output=%p size=%u (local budget 65536 bytes)\n",
		     output, size);
	}
}

LUMEN_SYSV_ABI int VideoOutColorSettingsSetGamma(void* settings, float gamma)
{
	PRINT_NAME();
	// This helper prepares the gamma field only. Applying color settings to a
	// display is a separate operation. Nonfinite inputs are rejected locally.
	if (!std::isfinite(gamma) || gamma < 0.1f || gamma > 2.0f)
	{
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}
	if (settings == nullptr) return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	static_assert(sizeof(float) == 4);
	if (!write_guest_output(settings, &gamma, sizeof(gamma)))
	{
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}
	return 0;
}

constexpr int VIDEO_OUT_EVENT_FLIP             = 0;
constexpr int VIDEO_OUT_EVENT_VBLANK           = 1;

struct VideoOutResolutionStatus
{
	uint32_t fullWidth        = 1920;
	uint32_t fullHeight       = 1080;
	uint32_t paneWidth        = 1920;
	uint32_t paneHeight       = 1080;
	uint64_t refreshRate      = 3;
	float    screenSizeInInch = 50;
	uint16_t flags            = 0;
	uint16_t reserved0        = 0;
	uint32_t reserved1[3]     = {0};
	// The guest-facing object representation is 48 bytes. Make the host ABI's
	// former tail padding explicit so every published byte is deterministic;
	// these bytes have no locally assigned semantics.
	uint32_t abiTailPadding   = 0;
};

static_assert(offsetof(VideoOutResolutionStatus, abiTailPadding) == 44);
static_assert(sizeof(VideoOutResolutionStatus) == 48);
static_assert(sizeof(VideoOutResolutionStatus) ==
              offsetof(VideoOutResolutionStatus, abiTailPadding) + sizeof(uint32_t));

struct VideoOutBufferAttribute
{
	uint32_t pixel_format;
	uint32_t tiling_mode;
	uint32_t aspect_ratio;
	uint32_t width;
	uint32_t height;
	uint32_t pitch_in_pixel;
	uint32_t option;
	uint32_t reserved0;
	uint64_t reserved1;
};

struct VideoOutFlipStatus
{
	uint64_t count          = 0;
	uint64_t processTime    = 0;
	uint64_t tsc            = 0;
	int64_t  flipArg        = 0;
	uint64_t submitTsc      = 0;
	uint64_t reserved0      = 0;
	int32_t  gcQueueNum     = 0;
	int32_t  flipPendingNum = 0;
	int32_t  currentBuffer  = 0;
	uint32_t reserved1      = 0;
};

struct VideoOutVblankStatus
{
	uint64_t count       = 0;
	uint64_t processTime = 0;
	uint64_t tsc         = 0;
	uint64_t reserved[1] = {0};
	uint8_t  flags       = 0;
	uint8_t  pad1[7]     = {};
};

struct VideoOutBufferSet
{
	VideoOutBufferAttribute attr;

	int start_index = 0;
	int num         = 0;
	int set_id      = 0;
};

struct VideoOutBufferInfo
{
	const void*                    buffer        = nullptr;
	Graphics::VideoOutVulkanImage* buffer_vulkan = nullptr;
	Graphics::GpuMemoryObjectHandle tracked_handle;
	uint64_t                       buffer_size   = 0;
	uint64_t                       buffer_pitch  = 0;
	uint64_t                       generation    = 0;
	int                            set_id        = 0;
	bool                           tiled         = false;
	bool                           neo           = false;
};

struct VideoOutConfig
{
	Core::Mutex                      mutex;
	VideoOutResolutionStatus         resolution;
	bool                             opened    = false;
	std::atomic_uint64_t             generation {0};
	int                              flip_rate = 0;
	Vector<EventQueue::KernelEqueue> flip_eqs;
	Vector<EventQueue::KernelEqueue> pre_vblank_eqs;
	Vector<EventQueue::KernelEqueue> vblank_eqs;
	VideoOutFlipStatus               flip_status;
	VideoOutVblankStatus             pre_vblank_status;
	VideoOutVblankStatus             vblank_status;
	VideoOutBufferInfo               buffers[16];
	Vector<VideoOutBufferSet>        buffers_sets;
	int                              buffers_sets_seq = 0;
};

struct VideoOutPublishedBufferTrace
{
	const void* address              = nullptr;
	uint64_t    size                 = 0;
	uint64_t    generation           = 0;
	uint64_t    publication_sequence = 0;
	uint64_t    miss_count           = 0;
	int         handle               = -1;
	int         set_id               = -1;
	bool        closed               = false;
};

using VideoOutBufferCreateFunc = Graphics::VideoOutVulkanImage* (*)(
    void* context, Graphics::GraphicContext* graphic_ctx, uint64_t vaddr, uint64_t size,
    const Graphics::VideoOutBufferObject& info, Graphics::GpuMemoryObjectHandle* tracked_handle);

struct FramePresentLimitConfig
{
	bool     valid = true;
	uint64_t limit = 0;
};

static FramePresentLimitConfig parse_frame_present_limit(const char* value)
{
	if (value == nullptr)
	{
		return {};
	}
	if (value[0] == '\0')
	{
		return {false, 0};
	}

	uint64_t result = 0;
	for (const char* cursor = value; cursor[0] != '\0'; cursor++)
	{
		if (cursor[0] < '0' || cursor[0] > '9')
		{
			return {false, 0};
		}
		const uint64_t digit = static_cast<uint64_t>(cursor[0] - '0');
		if (result > (UINT64_MAX - digit) / 10)
		{
			return {false, 0};
		}
		result = result * 10 + digit;
	}
	return {true, result};
}

static uint64_t get_frame_present_limit()
{
	const auto config = parse_frame_present_limit(std::getenv("LUMEN_FRAME_PRESENT_LIMIT"));
	if (!config.valid)
	{
		EXIT("LUMEN_FRAME_PRESENT_LIMIT must be an unsigned decimal integer\n");
	}
	return config.limit;
}

static uint64_t get_frame_present_checkpoint()
{
	const auto config = parse_frame_present_limit(std::getenv("LUMEN_FRAME_CHECKPOINT_PRESENT"));
	if (!config.valid)
	{
		EXIT("LUMEN_FRAME_CHECKPOINT_PRESENT must be an unsigned decimal integer\n");
	}
	return config.limit;
}

class FramePresentLimitState
{
public:
	explicit FramePresentLimitState(uint64_t limit): m_limit(limit) {}

	[[nodiscard]] uint64_t Limit() const { return m_limit; }
	[[nodiscard]] uint64_t PresentCount() const { return m_present_count; }
	[[nodiscard]] bool CanPresent() const { return !m_reached; }

	bool CompletePresent()
	{
		if (m_reached)
		{
			return false;
		}
		if (m_present_count != UINT64_MAX)
		{
			m_present_count++;
		}
		if (m_limit != 0 && m_present_count == m_limit)
		{
			m_reached = true;
			return true;
		}
		return false;
	}

private:
	uint64_t m_limit         = 0;
	uint64_t m_present_count = 0;
	bool     m_reached       = false;
};

class FramePresentCheckpointState
{
public:
	explicit FramePresentCheckpointState(uint64_t target): m_target(target) {}

	[[nodiscard]] uint64_t Target() const { return m_target; }
	[[nodiscard]] bool     Pending() const { return m_target != 0 && !m_reported; }

	bool CompletePresent(uint64_t present_count)
	{
		if (!Pending() || present_count != m_target)
		{
			return false;
		}
		m_reported = true;
		return true;
	}

private:
	uint64_t m_target   = 0;
	bool     m_reported = false;
};

struct FramePresentTimingSnapshot
{
	uint64_t cumulative_ticks     = 0;
	uint64_t cumulative_intervals = 0;
	uint64_t partial_ticks        = 0;
	uint64_t partial_intervals    = 0;
	uint64_t latency_ticks        = 0;
	uint64_t latency_samples      = 0;
	uint64_t partial_max_ticks    = 0;
	uint64_t partial_max_present  = 0;
	uint64_t partial_over_50ms    = 0;
	uint64_t partial_over_100ms   = 0;
};

class FramePresentTimingState
{
public:
	void Observe(uint64_t present_tsc, uint64_t submit_tsc, uint64_t frequency, uint64_t present_count)
	{
		if (m_last_present_tsc != 0)
		{
			const uint64_t interval = present_tsc - m_last_present_tsc;
			m_cumulative_ticks += interval;
			m_cumulative_intervals++;
			m_partial_ticks += interval;
			m_partial_intervals++;
			if (m_partial_intervals == 1 || interval > m_partial_max_ticks)
			{
				m_partial_max_ticks   = interval;
				m_partial_max_present = present_count;
			}
			// Integer ticks > floor(frequency / divisor) is an exact strict
			// threshold comparison, including clocks not divisible by 20 or 10.
			m_partial_over_50ms += interval > frequency / 20u ? 1u : 0u;
			m_partial_over_100ms += interval > frequency / 10u ? 1u : 0u;
		}
		m_last_present_tsc = present_tsc;
		m_latency_ticks += present_tsc - submit_tsc;
		m_latency_samples++;
	}

	[[nodiscard]] FramePresentTimingSnapshot GetSnapshot() const
	{
		return {
		    m_cumulative_ticks, m_cumulative_intervals, m_partial_ticks,
		    m_partial_intervals, m_latency_ticks, m_latency_samples,
		    m_partial_max_ticks, m_partial_max_present, m_partial_over_50ms, m_partial_over_100ms};
	}

	void ResetPartial()
	{
		m_partial_ticks       = 0;
		m_partial_intervals   = 0;
		m_latency_ticks       = 0;
		m_latency_samples     = 0;
		m_partial_max_ticks   = 0;
		m_partial_max_present = 0;
		m_partial_over_50ms   = 0;
		m_partial_over_100ms  = 0;
	}

private:
	uint64_t m_last_present_tsc     = 0;
	uint64_t m_cumulative_ticks     = 0;
	uint64_t m_cumulative_intervals = 0;
	uint64_t m_partial_ticks        = 0;
	uint64_t m_partial_intervals    = 0;
	uint64_t m_latency_ticks        = 0;
	uint64_t m_latency_samples      = 0;
	uint64_t m_partial_max_ticks    = 0;
	uint64_t m_partial_max_present  = 0;
	uint64_t m_partial_over_50ms    = 0;
	uint64_t m_partial_over_100ms   = 0;
};

struct FlipQueueTraceSnapshot
{
	uint32_t pending_same_generation  = 0;
	uint32_t pending_other_generation = 0;
	uint64_t flip_count                = 0;
	int32_t  current_buffer            = -1;
};

class FlipQueue
{
public:
	FlipQueue()
	    : m_frame_present_limit(get_frame_present_limit()), m_frame_present_checkpoint(get_frame_present_checkpoint())
	{
		EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());
	}
	virtual ~FlipQueue() { LUMEN_NOT_IMPLEMENTED; }
	LUMEN_CLASS_NO_COPY(FlipQueue);

	bool Submit(VideoOutConfig* cfg, int handle, int index, int64_t flip_arg);
	bool Flip(uint32_t micros, VideoOutWaitService service = nullptr, void* userdata = nullptr);
	void GetFlipStatus(VideoOutConfig* cfg, VideoOutFlipStatus* out);
	void Wait(VideoOutConfig* cfg, int index);
	FlipQueueTraceSnapshot TraceSnapshot(VideoOutConfig* cfg, uint64_t generation);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	VideoOutIOSFlipCancelResult CancelIOSRuntimeFlips();
	bool                        ResetIOSRuntimeSession();
	bool                        IsDormantIOSRuntimeSession();
#endif

private:
	bool WaitForRequestLocked(uint32_t micros, VideoOutWaitService service, void* userdata);

	struct Request
	{
		VideoOutConfig* cfg;
		uint64_t        generation;
		int             handle;
		int             index;
		int64_t         flip_arg;
		uint64_t        event_data;
		uint64_t        submit_tsc;
	};

	Core::Mutex            m_mutex;
	Core::CondVar          m_submit_cond_var;
	Core::CondVar          m_done_cond_var;
	Core::List<Request>    m_requests;
	FramePresentLimitState      m_frame_present_limit;
	FramePresentCheckpointState m_frame_present_checkpoint;
	FramePresentTimingState     m_frame_present_timing;
	bool                        m_frame_limit_configuration_reported = false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	std::atomic_bool m_ios_stop_requested    = false;
	bool             m_ios_present_in_flight = false;
#endif
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	uint32_t m_done_waiters_for_test   = 0;
	uint32_t m_submit_waiters_for_test = 0;
#if defined(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
	std::atomic_bool m_phase6af_pause_after_claim_for_test = false;
	std::atomic_bool m_phase6af_open_before_add_for_test    = false;
	std::atomic_bool m_phase6af_claim_reached_for_test     = false;
	std::atomic_bool m_phase6af_request_inserted_for_test  = false;
	std::atomic_bool m_phase6af_release_submit_for_test    = false;
#endif
	friend bool VideoOutIOSFlipCancelContractTest();
	friend bool VideoOutWaitServiceContractTest();
#endif
#if defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE_TEST)
	std::atomic_bool m_host_pc_finish_after_unlock_for_test = false;
#endif
};

class VideoOutContext
{
public:
	static constexpr int VIDEO_OUT_NUM_MAX = 2;

	VideoOutContext() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~VideoOutContext() { LUMEN_NOT_IMPLEMENTED; }

	LUMEN_CLASS_NO_COPY(VideoOutContext);

	int             Open();
	int             Close(int handle);
	bool            IsOpen(int handle);
	bool            GetOpenGeneration(int handle, uint64_t* generation);
	int             ConfigureAnySdrResult(int handle, uint64_t generation, bool has_sdr_output);
	int             GetResolutionStatus(int handle, VideoOutResolutionStatus* status);
	int             SetFlipRate(int handle, int rate);
	int             AddFlipEvent(EventQueue::KernelEqueue eq, int handle, void* udata);
	int             AddVblankEvent(EventQueue::KernelEqueue eq, int handle, void* udata);
	int             SubmitFlip(int handle, int index, int flip_mode, int64_t flip_arg);
	int             GetFlipStatus(int handle, VideoOutFlipStatus* status);
	int             GetVblankStatus(int handle, VideoOutVblankStatus* status);
	int             SetWindowModeMargins(int handle, int top, int bottom);
	VideoOutConfig* Get(int handle);
	int             AdjustColor(int handle, const void* settings);
	int             RegisterBuffers(int handle, int start_index, const void* const* addresses, int buffer_num,
	                                const VideoOutBufferAttribute* attribute, Graphics::GraphicContext* graphic_ctx,
	                                VideoOutBufferCreateFunc create_func, void* create_context);

	VideoOutBufferImageInfo FindImage(const void* buffer, const VideoOutImageLookupIdentity* identity = nullptr);

	void Init(uint32_t width, uint32_t height);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	VideoOutIOSFlipCancelResult CancelIOSRuntimeFlips();
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	bool BeginIOSRuntimeSession(uint32_t width, uint32_t height);
	bool ResetIOSRuntimeSession();
#endif

	Graphics::GraphicContext* GetGraphicCtx()
	{
		Core::LockGuard lock(m_mutex);

		if (m_graphic_ctx == nullptr)
		{
			m_graphic_ctx = Graphics::WindowGetGraphicContext();
		}

		return m_graphic_ctx;
	}

	FlipQueue& GetFlipQueue() { return m_flip_queue; }

	void VblankBegin();
	void VblankEnd();

private:
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	friend bool VideoOutBufferPublicationContractTest();
#endif
	Core::Mutex               m_mutex;
	VideoOutConfig            m_video_out_ctx[VIDEO_OUT_NUM_MAX];
	Graphics::GraphicContext* m_graphic_ctx = nullptr;
	FlipQueue                 m_flip_queue;
	static constexpr uint32_t TRACE_BUFFER_HISTORY_MAX = 64;
	VideoOutPublishedBufferTrace m_trace_buffer_history[TRACE_BUFFER_HISTORY_MAX];
	uint32_t                     m_trace_buffer_history_next = 0;
	uint32_t                     m_trace_buffer_history_size = 0;
	uint64_t                     m_generation_sequence       = 0;
	uint64_t                     m_publication_sequence      = 0;
	uint64_t                     m_unmatched_find_miss_count = 0;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	std::atomic_bool m_ios_runtime_session_active = false;
#endif
};

static VideoOutContext* g_video_out_context = nullptr;

static bool videoout_open_param_ignore_enabled()
{
	static const bool enabled = []()
	{
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_VIDEOOUT_OPEN_PARAM_IGNORE");
		const bool requested = value != nullptr && std::strcmp(value, "1") == 0;
		if (requested)
		{
			std::fputs("[VIDEOOUT_OPEN_PARAM_IGNORE] enabled: experimental/unverified opaque parameter ignored\n", stderr);
		}
		return requested;
	}();
	return enabled;
}

static bool videoout_lifecycle_trace_enabled()
{
	static const bool enabled = []()
	{
		const char* value = std::getenv("LUMEN_VIDEOOUT_LIFECYCLE_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

bool VideoOutLifecycleTraceEnabled()
{
	return videoout_lifecycle_trace_enabled();
}

struct VideoOutLifecycleTraceLine
{
	uint64_t sequence = 0;
	char     text[768] {};
};

struct VideoOutLifecycleTraceState
{
	static constexpr uint32_t LINE_CAPACITY = 256;
	static constexpr uint32_t DUMP_MAX      = 4;
	Core::Mutex                mutex;
	VideoOutLifecycleTraceLine lines[LINE_CAPACITY];
	uint32_t                   next            = 0;
	uint32_t                   size            = 0;
	uint32_t                   dumps           = 0;
	uint64_t                   sequence        = 0;
	uint64_t                   last_dumped     = 0;
	bool                       config_recorded = false;
};

static VideoOutLifecycleTraceState& get_videoout_lifecycle_trace_state()
{
	// Process-owned diagnostic state; deliberate lifetime avoids shutdown-order
	// dependencies between stdio and Core locks.
	static auto* state = new VideoOutLifecycleTraceState;
	return *state;
}

static void videoout_lifecycle_trace_record(const char* format, ...)
{
	if (!videoout_lifecycle_trace_enabled())
	{
		return;
	}

	auto& state = get_videoout_lifecycle_trace_state();
	Core::LockGuard lock(state.mutex);
	auto add_line = [&state](const char* text)
	{
		auto& line   = state.lines[state.next];
		line.sequence = ++state.sequence;
		std::snprintf(line.text, sizeof(line.text), "%s", text);
		state.next = (state.next + 1u) % VideoOutLifecycleTraceState::LINE_CAPACITY;
		if (state.size < VideoOutLifecycleTraceState::LINE_CAPACITY)
		{
			state.size++;
		}
	};
	if (!state.config_recorded)
	{
		add_line("event=config enabled=1 ring=256 dumps=4 history=64 miss_policy=first8_then_powers_of_two");
		state.config_recorded = true;
	}

	char    text[768] {};
	va_list args;
	va_start(args, format);
	std::vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	add_line(text);
}

void VideoOutDumpLifecycleTrace(const char* reason)
{
	if (!videoout_lifecycle_trace_enabled())
	{
		return;
	}

	uint32_t size = 0;
	uint64_t latest_sequence = 0;
	std::unique_ptr<VideoOutLifecycleTraceLine[]> lines;
	{
		auto& state = get_videoout_lifecycle_trace_state();
		Core::LockGuard lock(state.mutex);
		if (state.size == 0 || state.last_dumped == state.sequence || state.dumps >= VideoOutLifecycleTraceState::DUMP_MAX)
		{
			return;
		}
		size            = state.size;
		latest_sequence = state.sequence;
		lines           = std::make_unique<VideoOutLifecycleTraceLine[]>(size);
		const uint32_t first = state.size == VideoOutLifecycleTraceState::LINE_CAPACITY ? state.next : 0;
		for (uint32_t offset = 0; offset < size; ++offset)
		{
			lines[offset] = state.lines[(first + offset) % VideoOutLifecycleTraceState::LINE_CAPACITY];
		}
		state.last_dumped = state.sequence;
		state.dumps++;
	}

	std::fprintf(stderr, "[VIDEOOUT_LIFECYCLE_DUMP] reason=%s lines=%u latest_seq=%" PRIu64 "\n", reason, size, latest_sequence);
	for (uint32_t index = 0; index < size; ++index)
	{
		std::fprintf(stderr, "[VIDEOOUT_LIFECYCLE] seq=%" PRIu64 " %s\n", lines[index].sequence, lines[index].text);
	}
	std::fprintf(stderr, "[VIDEOOUT_LIFECYCLE_DUMP_END] latest_seq=%" PRIu64 "\n", latest_sequence);
	std::fflush(stderr);
}

static bool videoout_lifecycle_trace_count(uint64_t count)
{
	return count <= 8 || (count & (count - 1u)) == 0;
}

static bool frame_pipeline_trace_enabled()
{
	static const bool enabled = []()
	{
		const char* value = std::getenv("LUMEN_FRAME_PIPELINE_TRACE");
		return value != nullptr && value[0] != '\0' && value[0] != '0';
	}();

	return enabled;
}

static uint64_t videoout_readback_frame()
{
	static const uint64_t target_frame = []()
	{
		const char* value = std::getenv("LUMEN_VIDEOOUT_READBACK_FRAME");
		if (value == nullptr || value[0] == '\0')
		{
			return uint64_t {0};
		}
		char*      end    = nullptr;
		const auto parsed = std::strtoull(value, &end, 10);
		const bool valid  = end != nullptr && end != value && end[0] == '\0' && parsed > 0;
		return valid ? static_cast<uint64_t>(parsed) : uint64_t {0};
	}();

	return target_frame;
}

static void trace_videoout_cpu_buffer(uint64_t frame, const VideoOutBufferInfo& buffer)
{
	if (frame != videoout_readback_frame() || buffer.buffer == nullptr || buffer.buffer_size == 0)
	{
		return;
	}

#if defined(LUMEN_FEXCORE_RUNTIME)
	// The framebuffer address survives registration and is read by the native
	// flip worker. Acquire its data on each diagnostic read, outside queue and
	// configuration locks, and retain admission until the complete scan ends.
	Core::VirtualMemory::GuestDataLease data_lease;
	if (!Core::VirtualMemory::TryAcquireGuestDataLease(
	        reinterpret_cast<uint64_t>(buffer.buffer), buffer.buffer_size,
	        Core::VirtualMemory::Mode::Read, &data_lease))
	{
		return;
	}
#endif
	const auto* bytes = static_cast<const uint8_t*>(buffer.buffer);
	uint64_t non_zero_bytes = 0;
	uint64_t first_non_zero = UINT64_MAX;
	uint64_t hash = 1469598103934665603ULL;
	for (uint64_t index = 0; index < buffer.buffer_size; index++)
	{
		if (bytes[index] != 0)
		{
			non_zero_bytes++;
			if (first_non_zero == UINT64_MAX)
			{
				first_non_zero = index;
			}
		}
		hash ^= bytes[index];
		hash *= 1099511628211ULL;
	}
	dbg_printf("[VIDEOOUT_CPU_BUFFER] frame=%" PRIu64 " size=%" PRIu64 " pitch=%" PRIu64 " nonzero_bytes=%" PRIu64
	           " first=%" PRIu64 " hash=0x%016" PRIx64 "\n",
	           frame, buffer.buffer_size, buffer.buffer_pitch, non_zero_bytes, first_non_zero, hash);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	__android_log_print(ANDROID_LOG_INFO, "LumenAndroidRuntime",
	                    "LUMEN_ANDROID_VIDEOOUT_CPU_BUFFER_V1 frame=%" PRIu64
	                    " address=0x%016" PRIx64
	                    " bytes=%" PRIu64 " nonZero=%" PRIu64
	                    " first=%" PRIu64 " hash=0x%016" PRIx64,
	                    frame, reinterpret_cast<uint64_t>(buffer.buffer), buffer.buffer_size, non_zero_bytes,
	                    first_non_zero, hash);
#endif
}

static void calc_buffer_size(const VideoOutBufferAttribute* attribute, uint64_t* out_size, uint64_t* out_align, uint64_t* out_pitch)
{
	EXIT_IF(out_size == nullptr);
	EXIT_IF(out_pitch == nullptr);
	EXIT_IF(attribute == nullptr);

	const bool     tile   = attribute->tiling_mode == 0;
	const bool     neo    = Config::IsNeo();
	const uint32_t width  = attribute->width;
	const uint32_t height = attribute->height;
	const uint32_t pitch  = attribute->pitch_in_pixel;

	EXIT_NOT_IMPLEMENTED(attribute->option != 0);
	EXIT_NOT_IMPLEMENTED(attribute->aspect_ratio != 0);
	EXIT_NOT_IMPLEMENTED(attribute->pixel_format != 0x80000000 && attribute->pixel_format != 0x80002200);

	Graphics::TileSizeAlign size32 {};
	Graphics::TileGetVideoOutSize(width, height, pitch, tile, neo, &size32);

	*out_size  = size32.size;
	*out_align = size32.align;
	*out_pitch = pitch;
}

void VideoOutInit(uint32_t width, uint32_t height)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (g_video_out_context == nullptr)
	{
		g_video_out_context = new VideoOutContext;
	}
	EXIT_IF(!g_video_out_context->BeginIOSRuntimeSession(width, height));
#else
	EXIT_IF(g_video_out_context != nullptr);

	g_video_out_context = new VideoOutContext;

	g_video_out_context->Init(width, height);
#endif
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
bool VideoOutResetIOSRuntimeSession()
{
	if (g_video_out_context == nullptr)
	{
		return false;
	}
	return g_video_out_context->ResetIOSRuntimeSession();
}
#endif

void VideoOutContext::Init(uint32_t width, uint32_t height)
{
	for (auto& ctx: m_video_out_ctx)
	{
		ctx.resolution.fullWidth  = width;
		ctx.resolution.fullHeight = height;
		ctx.resolution.paneWidth  = width;
		ctx.resolution.paneHeight = height;
	}
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
bool VideoOutContext::BeginIOSRuntimeSession(uint32_t width, uint32_t height)
{
	Core::LockGuard lock(m_mutex);
	if (m_ios_runtime_session_active.load(std::memory_order_acquire) || !m_flip_queue.IsDormantIOSRuntimeSession())
	{
		return false;
	}
	for (auto& ctx: m_video_out_ctx)
	{
		Core::LockGuard config_lock(ctx.mutex);
		if (ctx.opened || ctx.flip_rate != 0 || !ctx.flip_eqs.IsEmpty() || !ctx.pre_vblank_eqs.IsEmpty() || !ctx.vblank_eqs.IsEmpty() ||
		    !ctx.buffers_sets.IsEmpty() || ctx.buffers_sets_seq != 0)
		{
			return false;
		}
		for (const auto& buffer: ctx.buffers)
		{
			if (buffer.buffer != nullptr || buffer.buffer_vulkan != nullptr || buffer.buffer_size != 0 || buffer.buffer_pitch != 0)
			{
				return false;
			}
		}
	}

	Init(width, height);
	m_graphic_ctx                = nullptr;
	m_ios_runtime_session_active.store(true, std::memory_order_release);
	return true;
}
#endif

int VideoOutContext::Open()
{
	Core::LockGuard lock(m_mutex);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	EXIT_IF(!m_ios_runtime_session_active.load(std::memory_order_acquire));
#endif

	int handle = -1;

	for (int i = 1; i < VIDEO_OUT_NUM_MAX; i++)
	{
		if (!m_video_out_ctx[i].opened)
		{
			handle = i;
			break;
		}
	}
	if (handle < 0)
	{
		videoout_lifecycle_trace_record("event=open_result thread=%d result=resource_busy api_result=%d",
		                                Core::Thread::GetThreadIdUnique(), VIDEO_OUT_ERROR_RESOURCE_BUSY);
		return -1;
	}

	EXIT_IF(!m_video_out_ctx[handle].flip_eqs.IsEmpty());
	EXIT_IF(!m_video_out_ctx[handle].pre_vblank_eqs.IsEmpty());
	EXIT_IF(!m_video_out_ctx[handle].vblank_eqs.IsEmpty());
	EXIT_IF(m_video_out_ctx[handle].flip_rate != 0);

	EXIT_IF(m_generation_sequence == UINT64_MAX);
	auto& config                              = m_video_out_ctx[handle];
	const uint64_t generation                 = ++m_generation_sequence;
	config.generation.store(generation, std::memory_order_release);
	config.opened                             = true;
	config.flip_status                        = VideoOutFlipStatus();
	config.flip_status.flipArg                = -1;
	config.flip_status.currentBuffer          = -1;
	config.flip_status.count                  = 0;
	config.pre_vblank_status                  = VideoOutVblankStatus();
	config.vblank_status                      = VideoOutVblankStatus();
	videoout_lifecycle_trace_record("event=open_result thread=%d result=ok handle=%d generation=%" PRIu64,
	                                Core::Thread::GetThreadIdUnique(), handle, generation);

	return handle;
}

static bool delete_registered_events(Vector<EventQueue::KernelEqueue>* queues, uintptr_t ident)
{
	EXIT_IF(queues == nullptr);
	for (auto& registered_queue: *queues)
	{
		if (registered_queue == nullptr)
		{
			continue;
		}
		const auto result = EventQueue::KernelDeleteEvent(registered_queue, ident, EventQueue::KERNEL_EVFILT_VIDEO_OUT);
		if (result != OK && result != LibKernel::KERNEL_ERROR_ENOENT)
		{
			return false;
		}
		// The normal delete callback also clears this slot. Assign it here as
		// well so an already-absent registration is made dormant deterministically.
		registered_queue = nullptr;
	}
	return true;
}

static void reset_video_out_config(VideoOutConfig* config)
{
	EXIT_IF(config == nullptr);
	Core::LockGuard lock(config->mutex);
	config->opened             = false;
	config->flip_rate          = 0;
	config->flip_status        = VideoOutFlipStatus {};
	config->pre_vblank_status  = VideoOutVblankStatus {};
	config->vblank_status      = VideoOutVblankStatus {};
	for (auto& buffer: config->buffers)
	{
		buffer = VideoOutBufferInfo {};
	}
	config->flip_eqs.Clear();
	config->pre_vblank_eqs.Clear();
	config->vblank_eqs.Clear();
	config->buffers_sets.Clear();
	config->buffers_sets_seq = 0;
}

int VideoOutContext::Close(int handle)
{
	Core::LockGuard lock(m_mutex);

	if (handle < 0 || handle >= VIDEO_OUT_NUM_MAX || !m_video_out_ctx[handle].opened)
	{
		videoout_lifecycle_trace_record("event=close_result thread=%d handle=%d result=invalid_handle api_result=%d",
		                                Core::Thread::GetThreadIdUnique(), handle, VIDEO_OUT_ERROR_INVALID_HANDLE);
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	auto* config = &m_video_out_ctx[handle];
	const uint64_t generation = config->generation.load(std::memory_order_acquire);
	if (videoout_lifecycle_trace_enabled())
	{
		const auto queue = m_flip_queue.TraceSnapshot(config, generation);
		videoout_lifecycle_trace_record(
		    "event=close_begin thread=%d handle=%d generation=%" PRIu64
		    " sets=%u pending_same_generation_at_snapshot=%u pending_other_generation_at_snapshot=%u"
		    " flip_count_at_snapshot=%" PRIu64 " current_buffer_at_snapshot=%d",
		    Core::Thread::GetThreadIdUnique(), handle, generation, config->buffers_sets.Size(), queue.pending_same_generation,
		    queue.pending_other_generation, queue.flip_count, queue.current_buffer);
	}
	EXIT_NOT_IMPLEMENTED(!config->pre_vblank_eqs.IsEmpty());
	EXIT_NOT_IMPLEMENTED(!delete_registered_events(&config->flip_eqs, VIDEO_OUT_EVENT_FLIP));
	EXIT_NOT_IMPLEMENTED(!delete_registered_events(&config->vblank_eqs, VIDEO_OUT_EVENT_VBLANK));
	for (uint32_t index = 0; index < m_trace_buffer_history_size; ++index)
	{
		auto& record = m_trace_buffer_history[index];
		if (record.handle == handle && record.generation == generation)
		{
			record.closed = true;
		}
	}
	reset_video_out_config(config);
	videoout_lifecycle_trace_record("event=close_result thread=%d handle=%d generation=%" PRIu64 " result=ok",
	                                Core::Thread::GetThreadIdUnique(), handle, generation);
	return OK;
}

bool VideoOutContext::IsOpen(int handle)
{
	Core::LockGuard lock(m_mutex);
	return handle >= 0 && handle < VIDEO_OUT_NUM_MAX && m_video_out_ctx[handle].opened;
}

bool VideoOutContext::GetOpenGeneration(int handle, uint64_t* generation)
{
	Core::LockGuard lock(m_mutex);
	if (generation == nullptr || handle < 0 || handle >= VIDEO_OUT_NUM_MAX || !m_video_out_ctx[handle].opened) return false;
	*generation = m_video_out_ctx[handle].generation.load(std::memory_order_acquire);
	return true;
}

int VideoOutContext::ConfigureAnySdrResult(int handle, uint64_t generation, bool has_sdr_output)
{
	Core::LockGuard lock(m_mutex);
	if (handle < 0 || handle >= VIDEO_OUT_NUM_MAX || !m_video_out_ctx[handle].opened ||
	    m_video_out_ctx[handle].generation.load(std::memory_order_acquire) != generation)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	return has_sdr_output ? OK : VIDEO_OUT_ERROR_UNSUPPORTED_OPERATION;
}

int VideoOutContext::GetResolutionStatus(int handle, VideoOutResolutionStatus* status)
{
	Core::LockGuard lock(m_mutex);
	if (handle < 0 || handle >= VIDEO_OUT_NUM_MAX || !m_video_out_ctx[handle].opened)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	*status = m_video_out_ctx[handle].resolution;
	return OK;
}

int VideoOutContext::SetFlipRate(int handle, int rate)
{
	Core::LockGuard lock(m_mutex);
	if (handle < 0 || handle >= VIDEO_OUT_NUM_MAX || !m_video_out_ctx[handle].opened)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	EXIT_NOT_IMPLEMENTED(rate < 0 || rate > 2);
	printf("\trate = %d\n", rate);
	auto& config = m_video_out_ctx[handle];
	Core::LockGuard config_lock(config.mutex);
	config.flip_rate = rate;
	return OK;
}

int VideoOutContext::SubmitFlip(int handle, int index, int flip_mode, int64_t flip_arg)
{
	(void)VideoOutPrepareEventPayload(static_cast<uint64_t>(flip_arg));
	Core::LockGuard lock(m_mutex);
	if (handle < 0 || handle >= VIDEO_OUT_NUM_MAX || !m_video_out_ctx[handle].opened)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	EXIT_NOT_IMPLEMENTED(flip_mode != 1);
	if (index < 0 || index > 15)
	{
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}
	return m_flip_queue.Submit(&m_video_out_ctx[handle], handle, index, flip_arg)
	           ? OK
	           : VIDEO_OUT_ERROR_FLIP_QUEUE_FULL;
}

int VideoOutContext::GetFlipStatus(int handle, VideoOutFlipStatus* status)
{
	Core::LockGuard lock(m_mutex);
	if (handle < 0 || handle >= VIDEO_OUT_NUM_MAX || !m_video_out_ctx[handle].opened)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	m_flip_queue.GetFlipStatus(&m_video_out_ctx[handle], status);
	return OK;
}

int VideoOutContext::GetVblankStatus(int handle, VideoOutVblankStatus* status)
{
	Core::LockGuard lock(m_mutex);
	if (handle < 0 || handle >= VIDEO_OUT_NUM_MAX || !m_video_out_ctx[handle].opened)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	auto& config = m_video_out_ctx[handle];
	Core::LockGuard config_lock(config.mutex);
	*status = config.vblank_status;
	return OK;
}

int VideoOutContext::SetWindowModeMargins(int handle, int top, int bottom)
{
	Core::LockGuard lock(m_mutex);
	if (handle < 0 || handle >= VIDEO_OUT_NUM_MAX || !m_video_out_ctx[handle].opened)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	printf("\t top    = %d\n", top);
	printf("\t bottom = %d\n", bottom);
	return OK;
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
bool VideoOutContext::ResetIOSRuntimeSession()
{
	Core::LockGuard lock(m_mutex);
	if (!m_ios_runtime_session_active.load(std::memory_order_acquire) || !m_flip_queue.ResetIOSRuntimeSession())
	{
		return false;
	}

	for (auto& config: m_video_out_ctx)
	{
		// There is currently no guest API that creates pre-vblank entries. Do
		// not silently orphan an unknown callback if that contract ever changes.
		if (!config.pre_vblank_eqs.IsEmpty())
		{
			return false;
		}
		if (!delete_registered_events(&config.flip_eqs, VIDEO_OUT_EVENT_FLIP) ||
		    !delete_registered_events(&config.vblank_eqs, VIDEO_OUT_EVENT_VBLANK))
		{
			return false;
		}
	}

	for (auto& config: m_video_out_ctx)
	{
		reset_video_out_config(&config);
	}
	for (uint32_t index = 0; index < m_trace_buffer_history_size; ++index)
	{
		m_trace_buffer_history[index].closed = true;
	}
	videoout_lifecycle_trace_record("event=ios_session_reset thread=%d result=ok", Core::Thread::GetThreadIdUnique());
	m_graphic_ctx                = nullptr;
	m_ios_runtime_session_active.store(false, std::memory_order_release);
	return true;
}
#endif

int VideoOutContext::AdjustColor(int handle, const void* settings)
{
	Core::LockGuard lock(m_mutex);
	if (handle < 0 || handle >= VIDEO_OUT_NUM_MAX || !m_video_out_ctx[handle].opened)
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	float gamma = 0.0f;
	if (!read_guest_input(settings, &gamma, sizeof(gamma)))
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	printf("\tadjust_color_gamma = %.9g\n", static_cast<double>(gamma));
	if (!std::isfinite(gamma) || gamma < 0.1f || gamma > 2.0f)
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	// The current presentation path supports the neutral transfer. Report its
	// capability honestly until a nonneutral rendering transform is available.
	if (gamma != 1.0f) return VIDEO_OUT_ERROR_PORT_UNSUPPORTED_FUNCTION;
	return OK;
}

VideoOutConfig* VideoOutContext::Get(int handle)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	EXIT_IF(!m_ios_runtime_session_active.load(std::memory_order_acquire));
#endif
	EXIT_NOT_IMPLEMENTED(handle < 0 || handle >= VIDEO_OUT_NUM_MAX);
	EXIT_NOT_IMPLEMENTED(!m_video_out_ctx[handle].opened);

	return m_video_out_ctx + handle;
}

void VideoOutContext::VblankBegin()
{
	Core::LockGuard lock(m_mutex);

	for (int i = 1; i < VIDEO_OUT_NUM_MAX; i++)
	{
		auto& ctx = m_video_out_ctx[i];
		if (ctx.opened)
		{
			ctx.mutex.Lock();
			uint64_t next = 0, event_data = 0;
			EXIT_NOT_IMPLEMENTED(PrepareNextVblankEvent(ctx.pre_vblank_status.count, next, event_data) == VideoOutEventPayloadResult::OutOfRange);
			ctx.pre_vblank_status.count = next;
			ctx.pre_vblank_status.processTime = LibKernel::KernelGetProcessTime();
			ctx.pre_vblank_status.tsc         = LibKernel::KernelReadTsc();

			for (auto& vblank_eq: ctx.pre_vblank_eqs)
			{
				if (vblank_eq != nullptr)
				{
					auto result = EventQueue::KernelTriggerEvent(vblank_eq, VIDEO_OUT_EVENT_VBLANK, EventQueue::KERNEL_EVFILT_VIDEO_OUT,
					                                             &event_data);
					EXIT_NOT_IMPLEMENTED(result != OK);
				}
			}
			ctx.mutex.Unlock();
		}
	}
}

void VideoOutContext::VblankEnd()
{
	Core::LockGuard lock(m_mutex);

	for (int i = 1; i < VIDEO_OUT_NUM_MAX; i++)
	{
		auto& ctx = m_video_out_ctx[i];
		if (ctx.opened)
		{
			ctx.mutex.Lock();
			uint64_t next = 0, event_data = 0;
			EXIT_NOT_IMPLEMENTED(PrepareNextVblankEvent(ctx.vblank_status.count, next, event_data) == VideoOutEventPayloadResult::OutOfRange);
			ctx.vblank_status.count = next;
			ctx.vblank_status.processTime = LibKernel::KernelGetProcessTime();
			ctx.vblank_status.tsc         = LibKernel::KernelReadTsc();

			for (auto& vblank_eq: ctx.vblank_eqs)
			{
				if (vblank_eq != nullptr)
				{
					auto result = EventQueue::KernelTriggerEvent(vblank_eq, VIDEO_OUT_EVENT_VBLANK, EventQueue::KERNEL_EVFILT_VIDEO_OUT,
					                                             &event_data);
					EXIT_NOT_IMPLEMENTED(result != OK);
				}
			}
			ctx.mutex.Unlock();
		}
	}
}

VideoOutBufferImageInfo VideoOutContext::FindImage(const void* buffer, const VideoOutImageLookupIdentity* identity)
{
	VideoOutBufferImageInfo ret;

	Core::LockGuard lock(m_mutex);

	for (auto& ctx: m_video_out_ctx)
	{
		if (ctx.opened)
		{
			for (const auto& set: ctx.buffers_sets)
			{
				const int end_index = set.start_index + set.num;
				for (int j = set.start_index; j < end_index; j++)
				{
					if (ctx.buffers[j].buffer == buffer)
					{
						ret.image             = ctx.buffers[j].buffer_vulkan;
						ret.tracked_handle    = ctx.buffers[j].tracked_handle;
						ret.buffer_size       = ctx.buffers[j].buffer_size;
						ret.buffer_pitch      = ctx.buffers[j].buffer_pitch;
						ret.index             = j - set.start_index;
						ret.slot              = j;
						ret.generation        = ctx.buffers[j].generation;
						ret.set_id            = ctx.buffers[j].set_id;
						ret.registered_tiled  = ctx.buffers[j].tiled;
						ret.registered_neo    = ctx.buffers[j].neo;
						if (identity != nullptr && identity->kind == VideoOutImageLookupKind::SampledTexture &&
						    videoout_lifecycle_trace_enabled())
						{
							static std::atomic_uint64_t sampled_hit_count {0};
							const uint64_t hit_count = sampled_hit_count.fetch_add(1, std::memory_order_relaxed) + 1u;
							if (videoout_lifecycle_trace_count(hit_count) || (hit_count % 120u) == 0)
							{
								videoout_lifecycle_trace_record(
								    "event=find_hit kind=sampled thread=%d handle=%d generation=%" PRIu64
								    " set=%d slot=%d address=%p hit_count=%" PRIu64
								    " registered_size=%" PRIu64 " registered_pitch=%" PRIu64
								    " requested_size=%" PRIu64 " requested_pitch=%" PRIu64
								    " width=%u height=%u dfmt=%u nfmt=%u tile=%u swizzle=0x%x"
								    " base_level=%u levels=%u read_only=%u storage=%u"
								    " command_buffer=%p submit_id=%" PRIu64 " recording=%" PRIu64
								    " prior_ordinal=%" PRIu64 " queue=%d session=%" PRIu64 " submit_serial=%" PRIu64,
								    Core::Thread::GetThreadIdUnique(), static_cast<int>(&ctx - m_video_out_ctx),
								    ctx.generation.load(std::memory_order_acquire), set.set_id, j, buffer, hit_count,
								    ret.buffer_size, ret.buffer_pitch,
								    identity->requested_size, identity->requested_pitch, identity->requested_width,
								    identity->requested_height, identity->requested_data_format,
								    identity->requested_number_format, identity->requested_tile_mode,
								    identity->requested_swizzle, identity->requested_base_level,
								    identity->requested_levels, identity->requested_read_only ? 1u : 0u,
								    identity->requested_storage_descriptor ? 1u : 0u, identity->command_buffer,
								    identity->submit_id, identity->recording_id, identity->command_ordinal, identity->queue,
								    identity->submission_session_epoch, identity->recording_submit_serial);
							}
						}
						goto END;
					}
				}
			}
		}
	}
END:
	if (ret.image == nullptr && videoout_lifecycle_trace_enabled())
	{
		VideoOutPublishedBufferTrace* previous = nullptr;
		for (uint32_t index = 0; index < m_trace_buffer_history_size; ++index)
		{
			auto& candidate = m_trace_buffer_history[index];
			if (candidate.address == buffer &&
			    (previous == nullptr || candidate.publication_sequence > previous->publication_sequence))
			{
				previous = &candidate;
			}
		}
		const uint64_t miss_count = previous == nullptr ? ++m_unmatched_find_miss_count : ++previous->miss_count;
		if (videoout_lifecycle_trace_count(miss_count))
		{
			int      open_handle     = -1;
			uint64_t open_generation = 0;
			uint32_t open_sets       = 0;
			for (int handle = 0; handle < VIDEO_OUT_NUM_MAX; ++handle)
			{
				if (m_video_out_ctx[handle].opened)
				{
					open_handle     = handle;
					open_generation = m_video_out_ctx[handle].generation.load(std::memory_order_acquire);
					open_sets       = m_video_out_ctx[handle].buffers_sets.Size();
					break;
				}
			}
			const VideoOutImageLookupIdentity empty {};
			const auto& command = identity == nullptr ? empty : *identity;
			videoout_lifecycle_trace_record(
			    "event=find_miss thread=%d address=%p miss_count=%" PRIu64
			    " history_match=%u previous_handle=%d previous_generation=%" PRIu64
			    " previous_set=%d previous_size=%" PRIu64 " previous_closed=%u"
			    " open_handle=%d open_generation=%" PRIu64 " open_sets=%u command_buffer=%p"
			    " submit_id=%" PRIu64 " recording=%" PRIu64 " prior_ordinal=%" PRIu64
			    " queue=%d session=%" PRIu64 " submit_serial=%" PRIu64
			    " begin_submit_cutoff=%" PRIu64 " begin_completed_cutoff=%" PRIu64,
			    Core::Thread::GetThreadIdUnique(), buffer, miss_count, previous != nullptr ? 1u : 0u,
			    previous == nullptr ? -1 : previous->handle, previous == nullptr ? 0 : previous->generation,
			    previous == nullptr ? -1 : previous->set_id, previous == nullptr ? 0 : previous->size,
			    previous != nullptr && previous->closed ? 1u : 0u, open_handle, open_generation, open_sets,
			    command.command_buffer, command.submit_id, command.recording_id, command.command_ordinal, command.queue,
			    command.submission_session_epoch, command.recording_submit_serial, command.begin_submit_serial_cutoff,
			    command.begin_completed_serial_cutoff);
		}
		ret.lifecycle_trace_dump = previous != nullptr && miss_count == 1;
	}
	return ret;
}

FlipQueueTraceSnapshot FlipQueue::TraceSnapshot(VideoOutConfig* cfg, uint64_t generation)
{
	Core::LockGuard lock(m_mutex);
	FlipQueueTraceSnapshot snapshot;
	FOR_LIST(index, m_requests)
	{
		const auto& request = m_requests.At(index);
		if (request.cfg == cfg)
		{
			if (request.generation == generation)
			{
				snapshot.pending_same_generation++;
			} else
			{
				snapshot.pending_other_generation++;
			}
		}
	}
	snapshot.flip_count     = cfg->flip_status.count;
	snapshot.current_buffer = cfg->flip_status.currentBuffer;
	return snapshot;
}

bool FlipQueue::Submit(VideoOutConfig* cfg, int handle, int index, int64_t flip_arg)
{
	const uint64_t event_data = VideoOutPrepareEventPayload(static_cast<uint64_t>(flip_arg));
#if defined(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY)
	struct A64DynarecStartupNotifyGuard
	{
		bool armed = false;

		~A64DynarecStartupNotifyGuard()
		{
			if (armed)
			{
				Cpu::X64FinishA64DynarecFirstVideoSubmit();
			}
		}
	} startup_telemetry_notify;
#endif
#if defined(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
	struct A64DynarecPhase6AFNotifyGuard
	{
		bool armed = false;

		~A64DynarecPhase6AFNotifyGuard()
		{
			if (armed)
			{
				Cpu::X64FinishA64DynarecPhase6AFFirstVideoSubmit();
			}
		}
	} phase6af_telemetry_notify;
#endif
#if defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE)
	struct A64DynarecHostPcProfileNotifyGuard
	{
		bool armed = false;
#if defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE_TEST)
		Core::Mutex* queue_mutex = nullptr;
		std::atomic_bool* finish_after_unlock = nullptr;
#endif

		~A64DynarecHostPcProfileNotifyGuard()
		{
			if (armed)
			{
#if defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE_TEST)
				bool unlocked = false;
				if (queue_mutex != nullptr && queue_mutex->TryLock())
				{
					unlocked = true;
					queue_mutex->Unlock();
				}
				if (finish_after_unlock != nullptr)
				{
					finish_after_unlock->store(
					    unlocked, std::memory_order_release);
				}
#endif
				Cpu::X64FinishA64DynarecHostPcProfileFirstSubmit();
			}
		}
	} host_pc_profile_notify;
#if defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE_TEST)
	host_pc_profile_notify.queue_mutex = &m_mutex;
	host_pc_profile_notify.finish_after_unlock =
	    &m_host_pc_finish_after_unlock_for_test;
#endif
#endif

	Core::LockGuard lock(m_mutex);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	if (m_ios_stop_requested.load(std::memory_order_acquire))
	{
		return false;
	}
#endif

	if (m_requests.Size() >= 2)
	{
		return false;
	}

	Request r {};
	r.cfg        = cfg;
	r.generation = cfg->generation.load(std::memory_order_acquire);
	r.handle     = handle;
	r.index      = index;
	r.flip_arg   = flip_arg;
	r.event_data = event_data;
	r.submit_tsc = LibKernel::KernelReadTsc();

#if defined(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY) && \
    defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	if (m_phase6af_pause_after_claim_for_test.load(
	        std::memory_order_acquire))
	{
		m_phase6af_open_before_add_for_test.store(
		    Cpu::X64A64DynarecPhase6AFTelemetryIsOpenForTest(),
		    std::memory_order_relaxed);
	}
#endif
	m_requests.Add(r);
#if defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE)
	host_pc_profile_notify.armed =
	    Cpu::X64ClaimA64DynarecHostPcProfileFirstSubmit();
#endif
#if defined(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY)
	startup_telemetry_notify.armed =
	    Cpu::X64ClaimA64DynarecFirstVideoSubmit();
#endif
#if defined(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
	phase6af_telemetry_notify.armed =
	    Cpu::X64ClaimA64DynarecPhase6AFFirstVideoSubmit();
#if defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	if (phase6af_telemetry_notify.armed &&
	    m_phase6af_pause_after_claim_for_test.load(std::memory_order_acquire))
	{
		m_phase6af_request_inserted_for_test.store(
		    m_requests.Size() == 1, std::memory_order_relaxed);
		m_phase6af_claim_reached_for_test.store(
		    true, std::memory_order_release);
		while (!m_phase6af_release_submit_for_test.load(
		    std::memory_order_acquire))
		{
			std::this_thread::yield();
		}
	}
#endif
#endif

	cfg->flip_status.flipPendingNum = static_cast<int>(m_requests.Size());
	cfg->flip_status.gcQueueNum     = 0;
	if (videoout_lifecycle_trace_enabled())
	{
		static std::atomic_uint64_t count {0};
		const uint64_t current = count.fetch_add(1, std::memory_order_relaxed) + 1u;
		if (videoout_lifecycle_trace_count(current) || (current % 120u) == 0)
		{
			videoout_lifecycle_trace_record(
			    "event=flip_submit thread=%d handle=%d generation=%" PRIu64 " count=%" PRIu64
			    " index=%d arg=%" PRId64 " pending=%u",
			    Core::Thread::GetThreadIdUnique(), handle, r.generation, current, index, flip_arg, m_requests.Size());
		}
	}

	if (frame_pipeline_trace_enabled())
	{
		static uint64_t submit_count    = 0;
		static uint64_t last_submit_tsc = 0;
		static uint64_t submit_ticks    = 0;
		static uint64_t submit_intervals = 0;

		const auto frequency = LibKernel::KernelGetTscFrequency();
		if (last_submit_tsc != 0)
		{
			submit_ticks += r.submit_tsc - last_submit_tsc;
			submit_intervals++;
		}
		last_submit_tsc = r.submit_tsc;
		submit_count++;

		if (submit_count == 1 || (submit_count % 120) == 0)
		{
			dbg_printf("[FRAME_PIPELINE] submit=%" PRIu64 " avg_interval=%.3f ms pending=%" PRIu32 " index=%d arg=%" PRId64
			           " rate=%d\n",
			           submit_count,
			           submit_intervals == 0
			               ? 0.0
			               : (static_cast<double>(submit_ticks) * 1000.0 / static_cast<double>(frequency) /
			                  static_cast<double>(submit_intervals)),
			           m_requests.Size(), index, flip_arg, cfg->flip_rate);
			submit_ticks     = 0;
			submit_intervals = 0;
		}
	}

	m_submit_cond_var.Signal();

	return true;
}

void FlipQueue::Wait(VideoOutConfig* cfg, int index)
{
	Core::LockGuard lock(m_mutex);

	while (
	    m_requests.IndexValid(m_requests.Find(cfg, index, [](auto r, auto cfg, auto index) { return r.cfg == cfg && r.index == index; })))
	{
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		m_done_waiters_for_test++;
#endif
		m_done_cond_var.Wait(&m_mutex);
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		m_done_waiters_for_test--;
#endif
	}
}

bool FlipQueue::WaitForRequestLocked(uint32_t micros, VideoOutWaitService service, void* userdata)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(micros);
	while (m_requests.Size() == 0)
	{
		uint32_t wait_micros = micros;
		if (service != nullptr)
		{
			const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
			    deadline - std::chrono::steady_clock::now()).count();
			if (remaining <= 0) return false;
			wait_micros = static_cast<uint32_t>(std::min<int64_t>(remaining, 8000));
		}
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		m_submit_waiters_for_test++;
#endif
		m_submit_cond_var.WaitFor(&m_mutex, wait_micros);
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		m_submit_waiters_for_test--;
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
		if (m_ios_stop_requested.load(std::memory_order_acquire)) return false;
#endif
		if (service == nullptr) return m_requests.Size() != 0;

		// A request may arrive during this callback. It remains unclaimed until
		// events have been serviced and the caller has accepted presentation.
		m_mutex.Unlock();
		const bool keep_waiting = service(userdata);
		m_mutex.Lock();
		if (!keep_waiting || !m_frame_present_limit.CanPresent()) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
		if (m_ios_stop_requested.load(std::memory_order_acquire)) return false;
#endif
	}
	return true;
}

bool FlipQueue::Flip(uint32_t micros, VideoOutWaitService service, void* userdata)
{
	LUMEN_PROFILER_BLOCK("FlipQueue::Flip");

	m_mutex.Lock();
	if (!m_frame_present_limit.CanPresent())
	{
		m_mutex.Unlock();
		return false;
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	if (m_ios_stop_requested.load(std::memory_order_acquire))
	{
		m_mutex.Unlock();
		return false;
	}
#endif
	if (m_requests.Size() == 0 && !WaitForRequestLocked(micros, service, userdata))
	{
		m_mutex.Unlock();
		return false;
	}
	auto first = m_requests.First();
	auto r     = m_requests.At(first);
	if (videoout_lifecycle_trace_enabled())
	{
		static std::atomic_uint64_t count {0};
		const uint64_t current = count.fetch_add(1, std::memory_order_relaxed) + 1u;
		if (videoout_lifecycle_trace_count(current) || (current % 120u) == 0)
		{
			videoout_lifecycle_trace_record(
			    "event=flip_begin thread=%d handle=%d generation=%" PRIu64 " count=%" PRIu64
			    " index=%d arg=%" PRId64 " pending=%u",
			    Core::Thread::GetThreadIdUnique(), r.handle, r.generation, current, r.index, r.flip_arg, m_requests.Size());
		}
	}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	m_ios_present_in_flight = true;
#endif

	m_mutex.Unlock();

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	// Cancellation first publishes the sticky stop flag, then attempts the queue
	// lock. Recheck after releasing the queue lock and before touching the
	// guest-backed image so a concurrent stop cannot become a new presentation.
	m_mutex.Lock();
	if (m_ios_stop_requested.load(std::memory_order_acquire))
	{
		m_ios_present_in_flight = false;
		m_mutex.Unlock();
		return false;
	}
	m_mutex.Unlock();
#endif

	const auto& buffer_info = r.cfg->buffers[r.index];
	static uint64_t videoout_frame = 0;
	videoout_frame++;
	trace_videoout_cpu_buffer(videoout_frame, buffer_info);
	auto* buffer = buffer_info.buffer_vulkan;

	Graphics::WindowDrawBuffer(buffer);

	m_mutex.Lock();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	m_ios_present_in_flight = false;
#endif

	r.cfg->mutex.Lock();
	for (auto& flip_eq: r.cfg->flip_eqs)
	{
		if (flip_eq != nullptr)
		{
			auto result = EventQueue::KernelTriggerEvent(flip_eq, VIDEO_OUT_EVENT_FLIP, EventQueue::KERNEL_EVFILT_VIDEO_OUT,
			                                             &r.event_data);
			EXIT_NOT_IMPLEMENTED(result != OK);
		}
	}
	r.cfg->mutex.Unlock();

	if (Config::DiagnosticsEnabled())
	{
		printf("Flip done: %d\n", r.index);
	}

	// m_mutex.Lock();

	m_requests.Remove(first);
	m_done_cond_var.Signal();

	r.cfg->flip_status.count++;
	r.cfg->flip_status.processTime    = LibKernel::KernelGetProcessTime();
	r.cfg->flip_status.tsc            = LibKernel::KernelReadTsc();
	r.cfg->flip_status.submitTsc      = r.submit_tsc;
	r.cfg->flip_status.flipArg        = r.flip_arg;
	r.cfg->flip_status.currentBuffer  = r.index;
	r.cfg->flip_status.flipPendingNum = static_cast<int>(m_requests.Size());

	const bool frame_trace_enabled     = frame_pipeline_trace_enabled();
	const bool lifecycle_trace_enabled = videoout_lifecycle_trace_enabled();
	const bool observe_present =
	    frame_trace_enabled || lifecycle_trace_enabled || m_frame_present_limit.Limit() != 0 || m_frame_present_checkpoint.Pending();
	bool       frame_limit_reached = false;
#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
	bool phase6ck_checkpoint_reached = false;
#endif
#if defined(LUMEN_A64_DYNAREC_PHASE6CM_PROFILE)
	bool phase6cm_checkpoint_reached = false;
#endif
#if defined(LUMEN_A64_DYNAREC_PHASE6CO_PROFILE)
	bool phase6co_checkpoint_reached = false;
#endif
	if (observe_present)
	{
		const auto present_tsc = LibKernel::KernelReadTsc();
		const auto frequency   = LibKernel::KernelGetTscFrequency();
		frame_limit_reached          = m_frame_present_limit.CompletePresent();
		const uint64_t present_count = m_frame_present_limit.PresentCount();
		m_frame_present_timing.Observe(present_tsc, r.submit_tsc, frequency, present_count);
		const bool checkpoint_reached = m_frame_present_checkpoint.CompletePresent(present_count);
		if (lifecycle_trace_enabled &&
		    (videoout_lifecycle_trace_count(present_count) || (present_count % 120u) == 0 || frame_limit_reached))
		{
			videoout_lifecycle_trace_record(
			    "event=flip_complete thread=%d handle=%d generation=%" PRIu64 " present=%" PRIu64
			    " index=%d arg=%" PRId64 " pending=%u",
			    Core::Thread::GetThreadIdUnique(), r.handle, r.generation, present_count, r.index, r.flip_arg, m_requests.Size());
		}
#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
		phase6ck_checkpoint_reached = checkpoint_reached;
#endif
#if defined(LUMEN_A64_DYNAREC_PHASE6CM_PROFILE)
		phase6cm_checkpoint_reached = checkpoint_reached;
#endif
#if defined(LUMEN_A64_DYNAREC_PHASE6CO_PROFILE)
		phase6co_checkpoint_reached = checkpoint_reached;
#endif
		const auto timing             = m_frame_present_timing.GetSnapshot();

		if (m_frame_present_limit.Limit() != 0 && !m_frame_limit_configuration_reported)
		{
			m_frame_limit_configuration_reported = true;
			std::fprintf(stderr, "[FRAME_LIMIT] configured limit=%" PRIu64 " domain=present action=SDL_QUIT\n",
			             m_frame_present_limit.Limit());
			std::fflush(stderr);
		}

		if (checkpoint_reached)
		{
			const double cumulative_ms =
			    static_cast<double>(timing.cumulative_ticks) * 1000.0 / static_cast<double>(frequency);
			const double partial_ms = static_cast<double>(timing.partial_ticks) * 1000.0 / static_cast<double>(frequency);
			std::fprintf(stderr,
			             "[FRAME_CHECKPOINT] present=%" PRIu64 " target=%" PRIu64
			             " cumulative_ms=%.3f cumulative_intervals=%" PRIu64
			             " partial_ms=%.3f partial_intervals=%" PRIu64 " terminal=0\n",
			             present_count, m_frame_present_checkpoint.Target(), cumulative_ms, timing.cumulative_intervals, partial_ms,
			             timing.partial_intervals);
			std::fflush(stderr);
		}

		if (frame_trace_enabled && (present_count == 1 || (present_count % 120) == 0 || frame_limit_reached))
		{
			// Existing report parsers require the exact FRAME_PIPELINE line.
			// Emit pacing beside it in the same bounded write and window.
			dbg_printf("[FRAME_PIPELINE] present=%" PRIu64 " avg_interval=%.3f ms avg_latency=%.3f ms pending=%" PRIu32 " index=%d "
			           "arg=%" PRId64 "\n"
			           "[FRAME_PACING] present=%" PRIu64 " intervals=%" PRIu64
			           " max_interval_ms=%.3f max_interval_present=%" PRIu64
			           " intervals_over_50ms=%" PRIu64 " intervals_over_100ms=%" PRIu64 "\n",
			           present_count,
			           timing.partial_intervals == 0
			               ? 0.0
			               : (static_cast<double>(timing.partial_ticks) * 1000.0 / static_cast<double>(frequency) /
			                  static_cast<double>(timing.partial_intervals)),
			           static_cast<double>(timing.latency_ticks) * 1000.0 / static_cast<double>(frequency) /
			               static_cast<double>(timing.latency_samples),
			           m_requests.Size(), r.index, r.flip_arg, present_count, timing.partial_intervals,
			           static_cast<double>(timing.partial_max_ticks) * 1000.0 / static_cast<double>(frequency),
			           timing.partial_max_present, timing.partial_over_50ms, timing.partial_over_100ms);
		}
		if (present_count == 1 || (present_count % 120) == 0 || (frame_trace_enabled && frame_limit_reached))
		{
			m_frame_present_timing.ResetPartial();
		}
	}

	m_mutex.Unlock();

#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
	Lumen::Cpu::X64PollA64DynarecPhase6CKProfile(
	    m_frame_present_limit.PresentCount(), phase6ck_checkpoint_reached);
#endif
#if defined(LUMEN_A64_DYNAREC_PHASE6CM_PROFILE)
	Lumen::Cpu::X64PollA64DynarecPhase6CMProfile(
	    m_frame_present_limit.PresentCount(), phase6cm_checkpoint_reached);
#endif
#if defined(LUMEN_A64_DYNAREC_PHASE6CO_PROFILE)
	Lumen::Cpu::X64PollA64DynarecPhase6COProfile(
	    m_frame_present_limit.PresentCount(), phase6co_checkpoint_reached);
#endif

	Graphics::GpuMemoryFrameDone();
	Graphics::GpuMemoryDbgDump();
	if (frame_limit_reached)
	{
		const bool queued = Graphics::WindowRequestExit();
		std::fprintf(stderr, "[FRAME_LIMIT] reached present=%" PRIu64 " limit=%" PRIu64 " action=SDL_QUIT queued=%u\n",
		             m_frame_present_limit.PresentCount(), m_frame_present_limit.Limit(), queued ? 1u : 0u);
		std::fflush(stderr);
		if (!queued)
		{
			EXIT("Frame present limit could not queue SDL_QUIT\n");
		}
	}

	return true;
}

void FlipQueue::GetFlipStatus(VideoOutConfig* cfg, VideoOutFlipStatus* out)
{
	EXIT_IF(cfg == nullptr);
	EXIT_IF(out == nullptr);

	Core::LockGuard lock(m_mutex);

	*out = cfg->flip_status;
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
VideoOutIOSFlipCancelResult FlipQueue::CancelIOSRuntimeFlips()
{
	// Publish stop before attempting the lock. Submit/Flip then fail closed even
	// when an existing presentation temporarily owns the queue state.
	m_ios_stop_requested.store(true, std::memory_order_release);
	if (!m_mutex.TryLock())
	{
		return VideoOutIOSFlipCancelResult::Busy;
	}
	if (m_ios_present_in_flight)
	{
		m_mutex.Unlock();
		return VideoOutIOSFlipCancelResult::Busy;
	}

	const bool had_requests = m_requests.Size() != 0;
	FOR_LIST(index, m_requests)
	{
		auto& request = m_requests[index];
		// The config is process-owned by VideoOutContext. Preserve the last
		// completed flip and only clear queue-derived status; cancellation must
		// not touch the guest buffer or signal a synthetic presentation event.
		request.cfg->flip_status.flipPendingNum = 0;
		request.cfg->flip_status.gcQueueNum     = 0;
	}
	m_requests.Clear();

	// Stop may strand multiple guest command processors in WaitFlipDone and a
	// presentation worker in Flip. Broadcast both conditions while the queue
	// state is already coherent so every waiter observes the empty queue.
	m_done_cond_var.SignalAll();
	m_submit_cond_var.SignalAll();
	m_mutex.Unlock();
	return had_requests ? VideoOutIOSFlipCancelResult::Cancelled : VideoOutIOSFlipCancelResult::Idle;
}

bool FlipQueue::ResetIOSRuntimeSession()
{
	Core::LockGuard lock(m_mutex);
	if (m_requests.Size() != 0 || m_ios_present_in_flight)
	{
		return false;
	}
	m_ios_stop_requested.store(false, std::memory_order_release);
	m_done_cond_var.SignalAll();
	m_submit_cond_var.SignalAll();
	return true;
}

bool FlipQueue::IsDormantIOSRuntimeSession()
{
	Core::LockGuard lock(m_mutex);
	return m_requests.Size() == 0 && !m_ios_present_in_flight && !m_ios_stop_requested.load(std::memory_order_acquire);
}
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
VideoOutIOSFlipCancelResult VideoOutContext::CancelIOSRuntimeFlips()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (!m_ios_runtime_session_active.load(std::memory_order_acquire))
	{
		return VideoOutIOSFlipCancelResult::Failed;
	}
#endif
	return m_flip_queue.CancelIOSRuntimeFlips();
}

VideoOutIOSFlipCancelResult VideoOutCancelIOSRuntimeFlips()
{
	if (g_video_out_context == nullptr)
	{
		return VideoOutIOSFlipCancelResult::Failed;
	}
	return g_video_out_context->CancelIOSRuntimeFlips();
}
#endif

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool VideoOutWaitServiceContractTest()
{
	using Clock = std::chrono::steady_clock;
	auto await = [](const std::atomic_bool& flag)
	{
		const auto deadline = Clock::now() + std::chrono::seconds(2);
		while (!flag.load(std::memory_order_acquire) && Clock::now() < deadline) std::this_thread::yield();
		return flag.load(std::memory_order_acquire);
	};
	// Exercise the real condition variable, including wakes with no request.
	// Its absolute deadline must not restart after a wake or a service call.
	for (bool spurious: {false, true})
	{
		auto* queue = new FlipQueue; // Production queues intentionally have process lifetime.
		std::atomic_bool done {false};
		std::thread signaler;
		if (spurious)
		{
			signaler = std::thread([&]()
			{
				while (!done.load(std::memory_order_acquire))
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					Core::LockGuard lock(queue->m_mutex);
					queue->m_submit_cond_var.SignalAll();
				}
			});
		}
		uint32_t callbacks = 0;
		const auto service = +[](void* data) { ++*static_cast<uint32_t*>(data); return true; };
		const auto start = Clock::now();
		const bool presented = queue->Flip(40000, service, &callbacks);
		const auto elapsed = Clock::now() - start;
		done.store(true, std::memory_order_release);
		if (signaler.joinable()) signaler.join();
		if (presented || callbacks == 0 || elapsed < std::chrono::milliseconds(35) ||
		    elapsed > std::chrono::seconds(2) || queue->m_submit_waiters_for_test != 0 ||
		    queue->m_ios_present_in_flight || queue->m_frame_present_limit.PresentCount() != 0) return false;
	}

	// A second thread must acquire the queue lock and submit while the callback
	// is active. TryLock on the callback thread would not prove this, since the
	// repository mutex is recursive. The waiter never reaches Vulkan here.
	for (int action = 0; action < 4; ++action)
	{
		auto* queue = new FlipQueue;
		auto* config = new VideoOutConfig;
		struct Probe
		{
			FlipQueue* queue;
			int action;
			std::atomic_bool entered {false};
			std::atomic_bool submitted {false};
			bool callback_completed = false;
		} probe {queue, action};
		bool worker_unlocked = false;
		bool worker_submitted = false;
		std::thread submitter([&]()
		{
			if (await(probe.entered))
			{
				worker_unlocked = queue->m_mutex.TryLock();
				if (worker_unlocked)
				{
					queue->m_mutex.Unlock();
					worker_submitted = queue->Submit(config, 0, 0, 71) && queue->Submit(config, 0, 1, 72);
				}
			}
			probe.submitted.store(true, std::memory_order_release);
		});
		const auto service = +[](void* data)
		{
			auto& state = *static_cast<Probe*>(data);
			state.entered.store(true, std::memory_order_release);
			const auto deadline = Clock::now() + std::chrono::seconds(2);
			while (!state.submitted.load(std::memory_order_acquire) && Clock::now() < deadline) std::this_thread::yield();
			state.callback_completed = state.submitted.load(std::memory_order_acquire);
			if (state.action == 2) state.queue->CancelIOSRuntimeFlips();
			if (state.action == 3)
			{
				Core::LockGuard lock(state.queue->m_mutex);
				state.queue->m_frame_present_limit = FramePresentLimitState(1);
				state.queue->m_frame_present_limit.CompletePresent();
			}
			return state.action != 1 && state.callback_completed;
		};
		queue->m_mutex.Lock();
		const bool ready = queue->WaitForRequestLocked(100000, service, &probe);
		queue->m_mutex.Unlock();
		submitter.join();
		if (!worker_unlocked || !worker_submitted || !probe.callback_completed || ready != (action == 0) ||
		    queue->m_ios_present_in_flight || config->flip_status.count != 0) return false;
		if (action != 2)
		{
			if (queue->m_requests.Size() != 2 || config->flip_status.flipPendingNum != 2) return false;
			const auto first = queue->m_requests.First();
			const auto second = queue->m_requests.Next(first);
			if (queue->m_requests.At(first).index != 0 || queue->m_requests.At(first).flip_arg != 71 ||
			    queue->m_requests.At(second).index != 1 || queue->m_requests.At(second).flip_arg != 72) return false;
		}
		else if (queue->m_requests.Size() != 0 || config->flip_status.flipPendingNum != 0) return false;
		queue->CancelIOSRuntimeFlips();
	}
	return true;
}

bool VideoOutFramePresentLimitContractTest()
{
	const auto absent       = parse_frame_present_limit(nullptr);
	const auto off          = parse_frame_present_limit("0");
	const auto p2300        = parse_frame_present_limit("2300");
	const auto empty        = parse_frame_present_limit("");
	const auto negative     = parse_frame_present_limit("-1");
	const auto signed_value = parse_frame_present_limit("+1");
	const auto spaced       = parse_frame_present_limit(" 1");
	const auto suffix       = parse_frame_present_limit("2300x");
	const auto overflow     = parse_frame_present_limit("18446744073709551616");
	if (!absent.valid || absent.limit != 0 || !off.valid || off.limit != 0 || !p2300.valid || p2300.limit != 2300 || empty.valid ||
	    negative.valid || signed_value.valid || spaced.valid || suffix.valid || overflow.valid)
	{
		return false;
	}

	FramePresentLimitState state(2300);
	for (uint64_t present = 1; present < 2300; present++)
	{
		if (!state.CanPresent() || state.CompletePresent() || state.PresentCount() != present)
		{
			return false;
		}
	}
	if (!state.CanPresent() || !state.CompletePresent() || state.PresentCount() != 2300 || state.CanPresent() || state.CompletePresent() ||
	    state.PresentCount() != 2300)
	{
		return false;
	}
	return true;
}

bool VideoOutFramePresentCheckpointContractTest()
{
	for (const uint64_t frequency: {1000u, 1001u})
	{
		FramePresentTimingState pacing;
		pacing.Observe(1000, 999, frequency, 1);
		const auto first = pacing.GetSnapshot();
		if (first.cumulative_ticks != 0 || first.cumulative_intervals != 0 || first.partial_ticks != 0 ||
		    first.partial_intervals != 0 || first.partial_max_ticks != 0 || first.partial_max_present != 0 ||
		    first.partial_over_50ms != 0 || first.partial_over_100ms != 0 || first.latency_ticks != 1 ||
		    first.latency_samples != 1) return false;
		pacing.ResetPartial();
		pacing.Observe(1050, 1049, frequency, 2); // Exactly 50 ms only for frequency 1000: excluded.
		pacing.Observe(1101, 1100, frequency, 3);
		pacing.Observe(1201, 1200, frequency, 4); // Exactly 100 ms only for frequency 1000: excluded.
		pacing.Observe(1302, 1301, frequency, 5);
		pacing.Observe(1403, 1402, frequency, 6); // Equal maxima retain the first endpoint.
		const auto window = pacing.GetSnapshot();
		if (window.cumulative_ticks != 403 || window.cumulative_intervals != 5 || window.partial_ticks != 403 ||
		    window.partial_intervals != 5 || window.partial_max_ticks != 101 || window.partial_max_present != 5 ||
		    window.partial_over_50ms != 4 || window.partial_over_100ms != 2 || window.latency_ticks != 5 ||
		    window.latency_samples != 5) return false;
		pacing.ResetPartial();
		const auto reset = pacing.GetSnapshot();
		if (reset.cumulative_ticks != 403 || reset.cumulative_intervals != 5 || reset.partial_ticks != 0 ||
		    reset.partial_intervals != 0 || reset.partial_max_ticks != 0 || reset.partial_max_present != 0 ||
		    reset.partial_over_50ms != 0 || reset.partial_over_100ms != 0 || reset.latency_ticks != 0 ||
		    reset.latency_samples != 0) return false;
		pacing.Observe(1404, 1403, frequency, 7);
		const auto next = pacing.GetSnapshot();
		if (next.cumulative_ticks != 404 || next.cumulative_intervals != 6 || next.partial_ticks != 1 ||
		    next.partial_intervals != 1 || next.partial_max_ticks != 1 || next.partial_max_present != 7 ||
		    next.partial_over_50ms != 0 || next.partial_over_100ms != 0 || next.latency_ticks != 1 ||
		    next.latency_samples != 1) return false;
	}

	FramePresentCheckpointState disabled(0);
	if (disabled.Pending() || disabled.CompletePresent(1))
	{
		return false;
	}

	FramePresentCheckpointState checkpoint(2300);
	FramePresentTimingState     timing;
	uint64_t                    checkpoint_emissions = 0;
	for (uint64_t present = 1; present <= 2400; present++)
	{
		timing.Observe(present * 10, present * 10 - 1, 1000, present);
		const auto before  = timing.GetSnapshot();
		const bool emitted = checkpoint.CompletePresent(present);
		const auto after   = timing.GetSnapshot();
		if (before.cumulative_ticks != after.cumulative_ticks ||
		    before.cumulative_intervals != after.cumulative_intervals || before.partial_ticks != after.partial_ticks ||
		    before.partial_intervals != after.partial_intervals || before.latency_ticks != after.latency_ticks ||
		    before.latency_samples != after.latency_samples || before.partial_max_ticks != after.partial_max_ticks ||
		    before.partial_max_present != after.partial_max_present || before.partial_over_50ms != after.partial_over_50ms ||
		    before.partial_over_100ms != after.partial_over_100ms)
		{
			return false;
		}
		if (emitted)
		{
			checkpoint_emissions++;
			if (present != 2300 || before.cumulative_ticks != 22990 || before.cumulative_intervals != 2299 ||
			    before.partial_ticks != 200 || before.partial_intervals != 20)
			{
				return false;
			}
		}
		else if (present == 2300)
		{
			return false;
		}

		if (present == 2400 && (before.partial_ticks != 1200 || before.partial_intervals != 120 ||
		                        before.partial_max_ticks != 10 || before.partial_max_present != 2281 ||
		                        before.partial_over_50ms != 0 || before.partial_over_100ms != 0))
		{
			return false;
		}
		if (present == 1 || (present % 120) == 0)
		{
			timing.ResetPartial();
		}
	}

	return checkpoint_emissions == 1 && !checkpoint.Pending() && !checkpoint.CompletePresent(2300);
}

bool VideoOutIOSFlipCancelContractTest()
{
	#if defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE_TEST)
	const char* host_pc_oracle =
	    std::getenv("LUMEN_A64_DYNAREC_HOST_PC_PROFILE_ORACLE");
	if (host_pc_oracle != nullptr)
	{
		const bool rejected = std::strcmp(host_pc_oracle, "rejected") == 0;
		const bool accepted = std::strcmp(host_pc_oracle, "accepted") == 0;
		if (!rejected && !accepted) return false;
		Cpu::X64PrepareA64DynarecHostPcProfile();
		Cpu::X64BeginA64DynarecHostPcProfile();

		auto* host_pc_queue  = new FlipQueue;
		auto* host_pc_config = new VideoOutConfig;
		if (rejected)
		{
			const bool submit_rejected =
			    host_pc_queue->CancelIOSRuntimeFlips() ==
			        VideoOutIOSFlipCancelResult::Idle &&
			    !host_pc_queue->Submit(host_pc_config, 0, 0, 1);
			const bool claim =
			    Cpu::X64ClaimA64DynarecHostPcProfileFirstSubmit();
			if (claim)
			{
				Cpu::X64FinishA64DynarecHostPcProfileFirstSubmit();
			}
			const bool complete =
			    Cpu::X64ValidateA64DynarecHostPcProfileSnapshotForTest();
			const bool exactly_once =
			    !Cpu::X64ClaimA64DynarecHostPcProfileFirstSubmit();
			return submit_rejected && claim && complete && exactly_once;
		}

		const bool submit_accepted =
		    host_pc_queue->Submit(host_pc_config, 0, 0, 2) &&
		    host_pc_config->flip_status.flipPendingNum == 1;
		const bool finish_after_unlock =
		    host_pc_queue->m_host_pc_finish_after_unlock_for_test.load(
		        std::memory_order_acquire);
		const bool complete =
		    Cpu::X64ValidateA64DynarecHostPcProfileSnapshotForTest();
		const bool exactly_once =
		    !Cpu::X64ClaimA64DynarecHostPcProfileFirstSubmit();
		return submit_accepted && finish_after_unlock && complete &&
		       exactly_once;
	}
	#endif
	#if defined(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY)
	// A stopped queue rejects before insertion. It must not consume the startup
	// cutoff; the explicit test close below must still be able to win it.
	if (!Cpu::X64BeginA64DynarecStartupVideoBoundaryForTest()) return false;
	auto* rejected_queue = new FlipQueue;
	auto* rejected_config = new VideoOutConfig;
	if (rejected_queue->CancelIOSRuntimeFlips() !=
	        VideoOutIOSFlipCancelResult::Idle ||
	    rejected_queue->Submit(rejected_config, 0, 0, 1) ||
	    !Cpu::X64A64DynarecStartupTelemetryIsOpenForTest() ||
	    !Cpu::X64CloseA64DynarecStartupVideoBoundaryForTest())
	{
		return false;
	}
	#endif
	#if defined(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
	// The new epoch owns an independent cutoff. A rejected submit must leave it
	// open just as it leaves the phase-6AD startup epoch open.
	if (!Cpu::X64BeginA64DynarecPhase6AFVideoBoundaryForTest()) return false;
	auto* phase6af_rejected_queue = new FlipQueue;
	auto* phase6af_rejected_config = new VideoOutConfig;
	if (phase6af_rejected_queue->CancelIOSRuntimeFlips() !=
	        VideoOutIOSFlipCancelResult::Idle ||
	    phase6af_rejected_queue->Submit(phase6af_rejected_config, 0, 0, 1) ||
	    !Cpu::X64A64DynarecPhase6AFTelemetryIsOpenForTest() ||
	    !Cpu::X64CloseA64DynarecPhase6AFVideoBoundaryForTest())
	{
		return false;
	}

	// Pause an accepted submit immediately after Add+claim while it still owns
	// the queue mutex. The epoch cannot be restarted at that point: reporting is
	// deferred until the guard runs after LockGuard has released the mutex. Once
	// Submit returns, a test restart is accepted only from the Reported phase.
	if (!Cpu::X64BeginA64DynarecPhase6AFVideoBoundaryForTest()) return false;
	auto* phase6af_accepted_queue  = new FlipQueue;
	auto* phase6af_accepted_config = new VideoOutConfig;
	phase6af_accepted_queue->m_phase6af_pause_after_claim_for_test.store(
	    true, std::memory_order_release);
	std::atomic_bool phase6af_submit_result   = false;
	std::atomic_bool phase6af_submit_returned = false;
	std::thread phase6af_submitter(
	    [phase6af_accepted_queue, phase6af_accepted_config,
	     &phase6af_submit_result, &phase6af_submit_returned]()
	    {
		    phase6af_submit_result.store(
		        phase6af_accepted_queue->Submit(
		            phase6af_accepted_config, 0, 0, 2),
		        std::memory_order_relaxed);
		    phase6af_submit_returned.store(true, std::memory_order_release);
	    });
	const auto phase6af_claim_deadline =
	    std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!phase6af_accepted_queue->m_phase6af_claim_reached_for_test.load(
	           std::memory_order_acquire) &&
	       std::chrono::steady_clock::now() < phase6af_claim_deadline)
	{
		std::this_thread::yield();
	}
	const bool phase6af_claim_reached =
	    phase6af_accepted_queue->m_phase6af_claim_reached_for_test.load(
	        std::memory_order_acquire);
	bool phase6af_queue_locked = false;
	if (phase6af_claim_reached)
	{
		phase6af_queue_locked = !phase6af_accepted_queue->m_mutex.TryLock();
		if (!phase6af_queue_locked)
		{
			phase6af_accepted_queue->m_mutex.Unlock();
		}
	}
	const bool phase6af_inserted_before_claim =
	    phase6af_accepted_queue->m_phase6af_request_inserted_for_test.load(
	        std::memory_order_relaxed);
	const bool phase6af_open_before_add =
	    phase6af_accepted_queue->m_phase6af_open_before_add_for_test.load(
	        std::memory_order_relaxed);
	const bool phase6af_cutoff_claimed =
	    !Cpu::X64A64DynarecPhase6AFTelemetryIsOpenForTest();
	const bool phase6af_reported_while_locked =
	    Cpu::X64BeginA64DynarecPhase6AFVideoBoundaryForTest();
	const bool phase6af_report_complete_while_locked =
	    Cpu::X64ValidateA64DynarecPhase6AFVideoBoundaryReportForTest();
	if (phase6af_reported_while_locked)
	{
		Cpu::X64CloseA64DynarecPhase6AFVideoBoundaryForTest();
	}
	phase6af_accepted_queue->m_phase6af_release_submit_for_test.store(
	    true, std::memory_order_release);
	phase6af_submitter.join();
	const bool phase6af_submit_accepted =
	    phase6af_submit_returned.load(std::memory_order_acquire) &&
	    phase6af_submit_result.load(std::memory_order_relaxed) &&
	    phase6af_accepted_config->flip_status.flipPendingNum == 1;
	const bool phase6af_queue_unlocked =
	    phase6af_accepted_queue->m_mutex.TryLock();
	if (phase6af_queue_unlocked)
	{
		phase6af_accepted_queue->m_mutex.Unlock();
	}
	const bool phase6af_cutoff_exactly_once =
	    !Cpu::X64CloseA64DynarecPhase6AFVideoBoundaryForTest();
	const bool phase6af_report_complete_after_return =
	    Cpu::X64ValidateA64DynarecPhase6AFVideoBoundaryReportForTest();
	const bool phase6af_reported_after_return =
	    !phase6af_reported_while_locked &&
	    Cpu::X64BeginA64DynarecPhase6AFVideoBoundaryForTest();
	const bool phase6af_restart_closed_once =
	    phase6af_reported_after_return &&
	    Cpu::X64CloseA64DynarecPhase6AFVideoBoundaryForTest() &&
	    !Cpu::X64CloseA64DynarecPhase6AFVideoBoundaryForTest();
	if (!phase6af_claim_reached || !phase6af_open_before_add ||
	    !phase6af_inserted_before_claim || !phase6af_queue_locked ||
	    !phase6af_cutoff_claimed || phase6af_reported_while_locked ||
	    phase6af_report_complete_while_locked ||
	    !phase6af_submit_accepted ||
	    !phase6af_queue_unlocked || !phase6af_cutoff_exactly_once ||
	    !phase6af_report_complete_after_return ||
	    !phase6af_reported_after_return || !phase6af_restart_closed_once)
	{
		return false;
	}
	#endif

	auto wait_for = [](auto&& predicate, std::chrono::milliseconds timeout)
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (!predicate() && std::chrono::steady_clock::now() < deadline)
		{
			std::this_thread::yield();
		}
		return predicate();
	};
	auto waiters_equal = [](FlipQueue* queue, uint32_t done, uint32_t submit)
	{
		if (!queue->m_mutex.TryLock())
		{
			return false;
		}
		const bool equal = queue->m_done_waiters_for_test == done && queue->m_submit_waiters_for_test == submit;
		queue->m_mutex.Unlock();
		return equal;
	};

	// FlipQueue is process-owned in production and intentionally has a fail-closed
	// destructor. Match that lifetime in this short headless executable.
	auto* done_queue = new FlipQueue;
	auto* config     = new VideoOutConfig;
	config->flip_status.count         = 7;
	config->flip_status.flipArg       = 41;
	config->flip_status.currentBuffer = 5;
	if (!done_queue->Submit(config, 0, 0, 100) || !done_queue->Submit(config, 0, 1, 101) ||
	    config->flip_status.flipPendingNum != 2)
	{
		return false;
	}

	std::atomic_uint done_waiters_finished = 0;
	std::thread      done_waiter_zero(
	         [done_queue, config, &done_waiters_finished]()
	         {
		         done_queue->Wait(config, 0);
		         done_waiters_finished.fetch_add(1, std::memory_order_release);
	         });
	std::thread done_waiter_one(
	    [done_queue, config, &done_waiters_finished]()
	    {
		    done_queue->Wait(config, 1);
		    done_waiters_finished.fetch_add(1, std::memory_order_release);
	    });
	const bool both_done_waiters_blocked = wait_for([&]() { return waiters_equal(done_queue, 2, 0); }, std::chrono::seconds(2));
	const auto done_cancel_result        = done_queue->CancelIOSRuntimeFlips();
	const bool both_done_waiters_woke =
	    wait_for([&]() { return done_waiters_finished.load(std::memory_order_acquire) == 2; }, std::chrono::seconds(2));
	done_waiter_zero.join();
	done_waiter_one.join();

	const bool cancelled_state = done_cancel_result == VideoOutIOSFlipCancelResult::Cancelled && both_done_waiters_blocked &&
	                             both_done_waiters_woke && done_queue->m_requests.Size() == 0 &&
	                             config->flip_status.flipPendingNum == 0 && config->flip_status.gcQueueNum == 0 &&
	                             config->flip_status.count == 7 && config->flip_status.flipArg == 41 &&
	                             config->flip_status.currentBuffer == 5 && !done_queue->Submit(config, 0, 0, 102);
	const bool reset_rearms_submit = done_queue->ResetIOSRuntimeSession() && done_queue->IsDormantIOSRuntimeSession() &&
	                                done_queue->Submit(config, 0, 0, 103) &&
	                                done_queue->CancelIOSRuntimeFlips() == VideoOutIOSFlipCancelResult::Cancelled &&
	                                done_queue->ResetIOSRuntimeSession() && done_queue->IsDormantIOSRuntimeSession();

	// An empty presentation waiter must be released by the submit-condition
	// broadcast without reaching WindowDrawBuffer.
	auto*            submit_queue = new FlipQueue;
	std::atomic_bool flip_finished = false;
	std::atomic_bool flip_result   = true;
	std::thread      flip_waiter(
	         [submit_queue, &flip_finished, &flip_result]()
	         {
		         flip_result.store(submit_queue->Flip(5000000), std::memory_order_relaxed);
		         flip_finished.store(true, std::memory_order_release);
	         });
	const bool submit_waiter_blocked = wait_for([&]() { return waiters_equal(submit_queue, 0, 1); }, std::chrono::seconds(2));
	const auto submit_cancel_result  = submit_queue->CancelIOSRuntimeFlips();
	const bool submit_waiter_woke = wait_for([&]() { return flip_finished.load(std::memory_order_acquire); }, std::chrono::seconds(2));
	flip_waiter.join();
	const bool submit_wakeup = submit_waiter_blocked && submit_cancel_result == VideoOutIOSFlipCancelResult::Idle &&
	                           submit_waiter_woke && !flip_result.load(std::memory_order_relaxed) &&
	                           submit_queue->ResetIOSRuntimeSession();

	// Lock contention is reported immediately. The sticky stop is already
	// visible, and a later retry performs the coherent cancellation.
	auto* busy_queue = new FlipQueue;
	busy_queue->m_mutex.Lock();
	std::atomic_bool busy_finished = false;
	std::atomic_int  busy_result   = static_cast<int>(VideoOutIOSFlipCancelResult::Failed);
	std::thread      busy_canceller(
	         [busy_queue, &busy_finished, &busy_result]()
	         {
		         busy_result.store(static_cast<int>(busy_queue->CancelIOSRuntimeFlips()), std::memory_order_relaxed);
		         busy_finished.store(true, std::memory_order_release);
	         });
	const bool busy_returned = wait_for([&]() { return busy_finished.load(std::memory_order_acquire); }, std::chrono::milliseconds(250));
	busy_queue->m_mutex.Unlock();
	busy_canceller.join();
	const bool busy_is_bounded = busy_returned &&
	                             busy_result.load(std::memory_order_relaxed) ==
	                                 static_cast<int>(VideoOutIOSFlipCancelResult::Busy) &&
	                             busy_queue->CancelIOSRuntimeFlips() == VideoOutIOSFlipCancelResult::Idle &&
	                             busy_queue->ResetIOSRuntimeSession();

	// Invalid event queues are rejected before the per-handle mutex is acquired.
	// A leaked lock here would make the later session reset unbounded.
	auto* saved_context      = g_video_out_context;
	auto* invalid_eq_context = new VideoOutContext;
	g_video_out_context      = invalid_eq_context;
	const int handle         = invalid_eq_context->Open();
	if (handle < 0)
	{
		g_video_out_context = saved_context;
		return false;
	}
	auto*     event_config   = invalid_eq_context->Get(handle);
	const bool flip_rejected = VideoOutAddFlipEvent(nullptr, handle, nullptr) == VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	const bool flip_unlocked = event_config->mutex.TryLock();
	if (flip_unlocked)
	{
		event_config->mutex.Unlock();
	}
	const bool vblank_rejected = flip_unlocked &&
	                             VideoOutAddVblankEvent(nullptr, handle, nullptr) == VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	const bool vblank_unlocked = vblank_rejected && event_config->mutex.TryLock();
	if (vblank_unlocked)
	{
		event_config->mutex.Unlock();
	}
	g_video_out_context = saved_context;

	return cancelled_state && reset_rearms_submit && submit_wakeup && busy_is_bounded && flip_rejected && flip_unlocked &&
	       vblank_rejected && vblank_unlocked;
}
#endif

bool VideoOutFlipWindow(uint32_t micros, VideoOutWaitService service, void* userdata)
{
	EXIT_IF(g_video_out_context == nullptr);

	return g_video_out_context->GetFlipQueue().Flip(micros, service, userdata);
}

void VideoOutBeginVblank()
{
	EXIT_IF(g_video_out_context == nullptr);

	// g_video_out_context->VblankBegin();
}

void VideoOutEndVblank()
{
	EXIT_IF(g_video_out_context == nullptr);

	g_video_out_context->VblankEnd();
}

LUMEN_SYSV_ABI int VideoOutOpen(int user_id, int bus_type, int index, const void* param)
{
	PRINT_NAME();
	// Latch the process-wide experiment even when the first call has no parameter.
	const bool ignore_param = videoout_open_param_ignore_enabled();

	EXIT_IF(g_video_out_context == nullptr);

	EXIT_NOT_IMPLEMENTED(user_id != 255 && user_id != 0);
	EXIT_NOT_IMPLEMENTED(bus_type != 0);
	EXIT_NOT_IMPLEMENTED(index != 0);
	// The public declarations currently available for this ABI expose only an
	// opaque pointer. The opt-in experiment ignores it without validating or
	// reading guest memory; normal operation rejects this unsupported variant.
	if (param != nullptr && !ignore_param) return VIDEO_OUT_ERROR_UNSUPPORTED_OPERATION;

	int handle = g_video_out_context->Open();

	if (handle < 0)
	{
		return VIDEO_OUT_ERROR_RESOURCE_BUSY;
	}

	return handle;
}

LUMEN_SYSV_ABI int VideoOutClose(int handle)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);

	return g_video_out_context->Close(handle);
}

LUMEN_SYSV_ABI int VideoOutGetDeviceCapabilityInfo(int handle, void* info, size_t size_of_info)
{
	PRINT_NAME();
	(void)info;
	(void)size_of_info;

	// The public ABI identifies an opaque output record and its byte size, but
	// does not establish the record layout. Preserve the common handle contract
	// while leaving every valid-handle query explicitly unsupported and without
	// touching guest memory.
	if (g_video_out_context == nullptr || !g_video_out_context->IsOpen(handle))
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	return VIDEO_OUT_ERROR_UNSUPPORTED_OPERATION;
}

LUMEN_SYSV_ABI int VideoOutIsFlipPending(int handle)
{
	PRINT_NAME();
	// The public ABI establishes only the handle argument and scalar result.
	// Reject invalid handles normally, but do not guess whether a valid result
	// is a boolean, count or another state encoding.
	if (g_video_out_context == nullptr || !g_video_out_context->IsOpen(handle))
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	return VIDEO_OUT_ERROR_UNSUPPORTED_OPERATION;
}

static void videoout_configure_any_sdr_trace(const char* reason, uint32_t mode_size, uint32_t options_size, int result,
                                            bool sdr_verified)
{
	if (result == OK)
	{
		static std::atomic_bool reported {false};
		if (reported.exchange(true, std::memory_order_relaxed)) return;
	} else
	{
		static std::atomic_uint reported {0};
		unsigned count = reported.load(std::memory_order_relaxed);
		while (count < 8 && !reported.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) {}
		if (count >= 8) return;
	}
	std::fprintf(stderr, "[VIDEOOUT_CONFIGURE_ANY_SDR] reason=%s mode_size=%u options_size=%u result=%d sdr_verified=%u\n",
	             reason, mode_size, options_size, result, static_cast<unsigned>(sdr_verified));
}

LUMEN_SYSV_ABI int VideoOutConfigureOutputMode(int handle, uint32_t reserved, const void* mode, const void* options,
                                             uint32_t mode_size, uint32_t options_size)
{
	PRINT_NAME();
	const bool enabled = VideoOutConfigureAnySdrEnabled();
	const auto finish = [&](int result, const char* reason, bool sdr_verified = false)
	{
		if (enabled) videoout_configure_any_sdr_trace(reason, mode_size, options_size, result, sdr_verified);
		return result;
	};
	uint64_t generation = 0;
	if (g_video_out_context == nullptr || !g_video_out_context->GetOpenGeneration(handle, &generation))
	{
		return finish(VIDEO_OUT_ERROR_INVALID_HANDLE, "invalid_handle");
	}
	if (!enabled) return VIDEO_OUT_ERROR_UNSUPPORTED_OPERATION;
	// The null options pointer admits two sizes as a local compatibility policy;
	// neither size establishes an options record layout.
	if (reserved != 0 || mode_size != 32 || options != nullptr || (options_size != 0 && options_size != 16))
	{
		return finish(VIDEO_OUT_ERROR_UNSUPPORTED_OPERATION, "unsupported_shape");
	}
	std::array<uint8_t, 32> snapshot {};
	if (!read_guest_input(mode, snapshot.data(), snapshot.size()))
	{
		return finish(VIDEO_OUT_ERROR_INVALID_ADDRESS, "invalid_address");
	}
	if (!VideoOutMatchesAnySdrMode(snapshot))
	{
		return finish(VIDEO_OUT_ERROR_UNSUPPORTED_OPERATION, "unsupported_record");
	}
	// Query the presenter without holding the VideoOut mutex, then require the
	// same open generation. The profile keeps every swapchain recreation SDR.
	const bool has_sdr_output = Graphics::WindowHasSdrOutput();
	const int result = g_video_out_context->ConfigureAnySdrResult(handle, generation, has_sdr_output);
	return finish(result, result == OK ? "accepted" :
	                         (result == VIDEO_OUT_ERROR_INVALID_HANDLE ? "handle_changed" : "no_sdr_output"),
	              has_sdr_output);
}

LUMEN_SYSV_ABI int VideoOutAdjustColor(int handle, const void* settings)
{
	PRINT_NAME();
	if (settings == nullptr) return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	if (g_video_out_context == nullptr) return VIDEO_OUT_ERROR_INVALID_HANDLE;
	return g_video_out_context->AdjustColor(handle, settings);
}

LUMEN_SYSV_ABI int VideoOutGetResolutionStatus(int handle, VideoOutResolutionStatus* status)
{
	PRINT_NAME();
	if (status == nullptr) return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	if (g_video_out_context == nullptr) return VIDEO_OUT_ERROR_INVALID_HANDLE;
	VideoOutResolutionStatus snapshot {};
	const int result = g_video_out_context->GetResolutionStatus(handle, &snapshot);
	if (result != OK) return result;
	if (!write_guest_output(status, &snapshot, sizeof(snapshot)))
	{
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}
	return OK;
}

LUMEN_SYSV_ABI void VideoOutSetBufferAttribute(VideoOutBufferAttribute* attribute, uint32_t pixel_format, uint32_t tiling_mode,
                                              uint32_t aspect_ratio, uint32_t width, uint32_t height, uint32_t pitch_in_pixel)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attribute == nullptr);

	printf("\t pixel_format   = %08" PRIx32 "\n", pixel_format);
	printf("\t tiling_mode    = %" PRIu32 "\n", tiling_mode);
	printf("\t aspect_ratio   = %" PRIu32 "\n", aspect_ratio);
	printf("\t width          = %" PRIu32 "\n", width);
	printf("\t height         = %" PRIu32 "\n", height);
	printf("\t pitch_in_pixel = %" PRIu32 "\n", pitch_in_pixel);

	memset(attribute, 0, sizeof(VideoOutBufferAttribute));

	attribute->pixel_format   = pixel_format;
	attribute->tiling_mode    = tiling_mode;
	attribute->aspect_ratio   = aspect_ratio;
	attribute->width          = width;
	attribute->height         = height;
	attribute->pitch_in_pixel = pitch_in_pixel;
}

LUMEN_SYSV_ABI int VideoOutSetFlipRate(int handle, int rate)
{
	PRINT_NAME();

	if (g_video_out_context == nullptr) return VIDEO_OUT_ERROR_INVALID_HANDLE;
	return g_video_out_context->SetFlipRate(handle, rate);
}

static void flip_event_reset_func(LibKernel::EventQueue::KernelEqueueEvent* event)
{
	EXIT_IF(event == nullptr);
	event->triggered    = false;
	event->event.fflags = 0;
	event->event.data   = 0;
}

static void flip_event_delete_func(EventQueue::KernelEqueue eq, LibKernel::EventQueue::KernelEqueueEvent* event)
{
	EXIT_IF(event == nullptr);
	EXIT_IF(event->filter.data == nullptr);

	EXIT_NOT_IMPLEMENTED(event->event.ident != VIDEO_OUT_EVENT_FLIP);
	EXIT_NOT_IMPLEMENTED(event->event.filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT);

	if (event->filter.data != nullptr)
	{
		auto* video_out = static_cast<VideoOutConfig*>(event->filter.data);
		video_out->mutex.Lock();
		EXIT_IF(video_out->flip_eqs.IsEmpty());
		auto index = video_out->flip_eqs.Find(eq);
		EXIT_NOT_IMPLEMENTED(!video_out->flip_eqs.IndexValid(index));
		video_out->flip_eqs[index] = nullptr;
		video_out->mutex.Unlock();
	}
}

static void flip_event_trigger_func(LibKernel::EventQueue::KernelEqueueEvent* event, void* trigger_data)
{
	EXIT_IF(event == nullptr);
	EXIT_IF(trigger_data == nullptr);
	// KernelTriggerEvent invokes this callback synchronously. The producer
	// owns a uint64_t snapshot for the call; packed high bits are never cast to
	// a possibly noncanonical host pointer.
	std::memcpy(&event->event.data, trigger_data, sizeof(event->event.data));
	event->triggered = true;
	event->event.fflags++;
}

static void vblank_event_reset_func(LibKernel::EventQueue::KernelEqueueEvent* event)
{
	EXIT_IF(event == nullptr);
	event->triggered    = false;
	event->event.fflags = 0;
	event->event.data   = 0;
}

static void vblank_event_delete_func(EventQueue::KernelEqueue eq, LibKernel::EventQueue::KernelEqueueEvent* event)
{
	EXIT_IF(event == nullptr);
	EXIT_IF(event->filter.data == nullptr);

	EXIT_NOT_IMPLEMENTED(event->event.ident != VIDEO_OUT_EVENT_VBLANK);
	EXIT_NOT_IMPLEMENTED(event->event.filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT);

	if (event->filter.data != nullptr)
	{
		auto* video_out = static_cast<VideoOutConfig*>(event->filter.data);
		video_out->mutex.Lock();
		EXIT_IF(video_out->vblank_eqs.IsEmpty());
		auto index = video_out->vblank_eqs.Find(eq);
		EXIT_NOT_IMPLEMENTED(!video_out->vblank_eqs.IndexValid(index));
		video_out->vblank_eqs[index] = nullptr;
		video_out->mutex.Unlock();
	}
}

static void vblank_event_trigger_func(LibKernel::EventQueue::KernelEqueueEvent* event, void* trigger_data)
{
	EXIT_IF(event == nullptr);
	EXIT_IF(trigger_data == nullptr);
	std::memcpy(&event->event.data, trigger_data, sizeof(event->event.data));
	event->triggered = true;
	event->event.fflags++;
}

bool VideoOutEventDataProducerContractTest()
{
	const bool enabled = VideoOutEventDataEnabled();
	// Exercise the real queue trigger/reset callbacks without creating a window
	// or presenting a frame. The payload format changes, not event identity.
	EventQueue::KernelEqueue queue = nullptr;
	if (EventQueue::KernelCreateEqueue(&queue, "videoout-event-data-contract") != OK) return false;
	bool passed = true;
	for (const uintptr_t id: {uintptr_t(0), uintptr_t(1)})
	{
		EventQueue::KernelEqueueEvent event {};
		event.event.ident = id;
		event.event.filter = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
		event.event.udata = &queue;
		event.filter.trigger_func = id == 0 ? flip_event_trigger_func : vblank_event_trigger_func;
		event.filter.reset_func = id == 0 ? flip_event_reset_func : vblank_event_reset_func;
		if (EventQueue::KernelAddEvent(queue, event) != OK) { passed = false; break; }
		for (const uint64_t payload: {UINT64_C(0), UINT64_C(0x0000123480000001), VIDEO_OUT_EVENT_PAYLOAD_MAX})
		{
			uint64_t encoded = VideoOutPrepareEventPayload(payload);
			if (EventQueue::KernelTriggerEvent(queue, id, EventQueue::KERNEL_EVFILT_VIDEO_OUT, &encoded) != OK ||
			    EventQueue::KernelTriggerEvent(queue, id, EventQueue::KERNEL_EVFILT_VIDEO_OUT, &encoded) != OK)
			{
				passed = false;
				break;
			}
			EventQueue::KernelEvent delivered {};
			LibKernel::KernelUseconds timeout = 0;
			int count = -1;
			if (EventQueue::KernelWaitEqueue(queue, &delivered, 1, &count, &timeout) != OK || count != 1 ||
			    delivered.ident != id || delivered.filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT || delivered.flags != 0 ||
			    delivered.fflags != 2 || delivered.udata != &queue ||
			    static_cast<uint64_t>(EventQueue::KernelGetEventData(&delivered)) != encoded)
			{
				passed = false;
				break;
			}
			const auto snapshot = delivered;
			int64_t decoded = -1;
			if (enabled && (VideoOutGetEventData(&delivered, &decoded) != OK || static_cast<uint64_t>(decoded) != payload)) passed = false;
			if (EventQueue::KernelWaitEqueue(queue, &delivered, 1, &count, &timeout) != LibKernel::KERNEL_ERROR_ETIMEDOUT || count != 0 ||
			    std::memcmp(&delivered, &snapshot, sizeof(delivered)) != 0) passed = false;
		}
		if (EventQueue::KernelDeleteEvent(queue, id, EventQueue::KERNEL_EVFILT_VIDEO_OUT) != OK) passed = false;
	}
	uint64_t next = 7, encoded = 11;
	if (PrepareNextVblankEvent(VIDEO_OUT_EVENT_PAYLOAD_MAX - 1, next, encoded) == VideoOutEventPayloadResult::OutOfRange ||
	    next != VIDEO_OUT_EVENT_PAYLOAD_MAX || encoded != (enabled ? VIDEO_OUT_EVENT_PAYLOAD_MAX << 16u : VIDEO_OUT_EVENT_PAYLOAD_MAX)) passed = false;
	for (const uint64_t current: {VIDEO_OUT_EVENT_PAYLOAD_MAX, UINT64_MAX})
	{
		next = 7;
		encoded = 11;
		const auto result = PrepareNextVblankEvent(current, next, encoded);
		if (enabled ? (result != VideoOutEventPayloadResult::OutOfRange || next != 7 || encoded != 11) :
		              (result != VideoOutEventPayloadResult::LegacyRaw || next != current + 1 || encoded != next)) passed = false;
	}
	return EventQueue::KernelDeleteEqueue(queue) == OK && passed;
}

int VideoOutContext::AddFlipEvent(EventQueue::KernelEqueue eq, int handle, void* udata)
{
	Core::LockGuard lock(m_mutex);
	if (handle < 0 || handle >= VIDEO_OUT_NUM_MAX || !m_video_out_ctx[handle].opened)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	if (eq == nullptr)
	{
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}

	auto& config = m_video_out_ctx[handle];
	Core::LockGuard config_lock(config.mutex);
	EXIT_NOT_IMPLEMENTED(config.flip_eqs.Contains(eq));

	EventQueue::KernelEqueueEvent event;
	event.triggered                = false;
	event.event.ident              = VIDEO_OUT_EVENT_FLIP;
	event.event.filter             = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
	event.event.udata              = udata;
	event.event.fflags             = 0;
	event.event.data               = 0;
	event.filter.delete_event_func = flip_event_delete_func;
	event.filter.reset_func        = flip_event_reset_func;
	event.filter.trigger_func      = flip_event_trigger_func;
	event.filter.data              = &config;

	int result = EventQueue::KernelAddEvent(eq, event);
	config.flip_eqs.Add(eq);

	return result;
}

int VideoOutContext::AddVblankEvent(EventQueue::KernelEqueue eq, int handle, void* udata)
{
	Core::LockGuard lock(m_mutex);
	if (handle < 0 || handle >= VIDEO_OUT_NUM_MAX || !m_video_out_ctx[handle].opened)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	if (eq == nullptr)
	{
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}

	auto& config = m_video_out_ctx[handle];
	Core::LockGuard config_lock(config.mutex);
	EXIT_NOT_IMPLEMENTED(config.vblank_eqs.Contains(eq));

	EventQueue::KernelEqueueEvent event;
	event.triggered                = false;
	event.event.ident              = VIDEO_OUT_EVENT_VBLANK;
	event.event.filter             = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
	event.event.udata              = udata;
	event.event.fflags             = 0;
	event.event.data               = 0;
	event.filter.delete_event_func = vblank_event_delete_func;
	event.filter.reset_func        = vblank_event_reset_func;
	event.filter.trigger_func      = vblank_event_trigger_func;
	event.filter.data              = &config;

	int result = EventQueue::KernelAddEvent(eq, event);
	config.vblank_eqs.Add(eq);

	return result;
}

LUMEN_SYSV_ABI int VideoOutAddFlipEvent(EventQueue::KernelEqueue eq, int handle, void* udata)
{
	PRINT_NAME();
	if (g_video_out_context == nullptr) return VIDEO_OUT_ERROR_INVALID_HANDLE;
	return g_video_out_context->AddFlipEvent(eq, handle, udata);
}

LUMEN_SYSV_ABI int VideoOutAddVblankEvent(LibKernel::EventQueue::KernelEqueue eq, int handle, void* udata)
{
	PRINT_NAME();
	if (g_video_out_context == nullptr) return VIDEO_OUT_ERROR_INVALID_HANDLE;
	return g_video_out_context->AddVblankEvent(eq, handle, udata);
}

static Graphics::VideoOutVulkanImage* create_video_out_buffer(
    void*, Graphics::GraphicContext* graphic_ctx, uint64_t vaddr, uint64_t size,
    const Graphics::VideoOutBufferObject& info, Graphics::GpuMemoryObjectHandle* tracked_handle)
{
	return static_cast<Graphics::VideoOutVulkanImage*>(
	    Graphics::GpuMemoryCreateObjectTracked(0, graphic_ctx, nullptr, vaddr, size, info, tracked_handle));
}

int VideoOutContext::RegisterBuffers(int handle, int start_index, const void* const* addresses, int buffer_num,
                                     const VideoOutBufferAttribute* attribute, Graphics::GraphicContext* graphic_ctx,
                                     VideoOutBufferCreateFunc create_func, void* create_context)
{
	if (handle < 0 || handle >= VIDEO_OUT_NUM_MAX)
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	EXIT_IF(addresses == nullptr || attribute == nullptr || create_func == nullptr);
	if (start_index < 0 || start_index > 15 || buffer_num < 1 || buffer_num > 16 || start_index + buffer_num > 16)
	{
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}
	uint64_t buffer_size  = 0;
	uint64_t buffer_align = 0;
	uint64_t buffer_pitch = 0;
	calc_buffer_size(attribute, &buffer_size, &buffer_align, &buffer_pitch);

	EXIT_NOT_IMPLEMENTED(buffer_size == 0);
	EXIT_NOT_IMPLEMENTED(buffer_pitch == 0);
	EXIT_NOT_IMPLEMENTED(buffer_pitch > UINT32_MAX);

	const bool     tile   = attribute->tiling_mode == 0;
	const bool     neo    = Config::IsNeo();
	const uint32_t width  = attribute->width;
	const uint32_t height = attribute->height;

	Graphics::VideoOutBufferFormat format = Graphics::VideoOutBufferFormat::Unknown;

	if (attribute->pixel_format == 0x80000000)
	{
		format = Graphics::VideoOutBufferFormat::B8G8R8A8Srgb;
	} else if (attribute->pixel_format == 0x80002200)
	{
		format = Graphics::VideoOutBufferFormat::R8G8B8A8Srgb;
	}

	Graphics::VideoOutBufferObject vulkan_buffer_info(format, width, height, tile, neo, static_cast<uint32_t>(buffer_pitch));

	// FindImage uses this same mutex. Keep the complete lookup/create/publish
	// window indivisible so it can observe either the old set or a fully formed
	// replacement, never an address whose host image is still null.
	Core::LockGuard lock(m_mutex);
	auto& ctx = m_video_out_ctx[handle];
	if (!ctx.opened)
	{
		videoout_lifecycle_trace_record("event=register_result thread=%d handle=%d result=invalid_handle api_result=%d",
		                                Core::Thread::GetThreadIdUnique(), handle, VIDEO_OUT_ERROR_INVALID_HANDLE);
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	const uint64_t generation = ctx.generation.load(std::memory_order_acquire);
	videoout_lifecycle_trace_record(
	    "event=register_begin thread=%d handle=%d generation=%" PRIu64
	    " start=%d count=%d format=0x%08" PRIx32 " width=%u height=%u pitch=%" PRIu64 " size=%" PRIu64,
	    Core::Thread::GetThreadIdUnique(), handle, generation, start_index, buffer_num, attribute->pixel_format, width, height,
	    buffer_pitch, buffer_size);

	// Reject the whole request before creating a host object or changing the set
	// sequence. The previous implementation published buffers_sets before this
	// check and could leave a partial set on the nonfatal occupied-slot path.
	for (int i = 0; i < buffer_num; i++)
	{
		if (ctx.buffers[i + start_index].buffer != nullptr)
		{
			videoout_lifecycle_trace_record(
			    "event=register_result thread=%d handle=%d generation=%" PRIu64
			    " result=slot_occupied slot=%d api_result=%d",
			    Core::Thread::GetThreadIdUnique(), handle, generation, i + start_index, VIDEO_OUT_ERROR_SLOT_OCCUPIED);
			return VIDEO_OUT_ERROR_SLOT_OCCUPIED;
		}

		EXIT_NOT_IMPLEMENTED((reinterpret_cast<uint64_t>(addresses[i]) & (buffer_align - 1u)) != 0);
	}
	for (int i = 0; i < buffer_num; ++i)
	{
		for (int previous = 0; previous < i; ++previous)
		{
			if (addresses[i] == addresses[previous])
			{
				videoout_lifecycle_trace_record(
				    "event=register_result thread=%d handle=%d generation=%" PRIu64
				    " result=duplicate_request_address first=%d second=%d address=%p api_result=%d",
				    Core::Thread::GetThreadIdUnique(), handle, generation, previous + start_index, i + start_index,
				    addresses[i], VIDEO_OUT_ERROR_INVALID_ADDRESS);
				return VIDEO_OUT_ERROR_INVALID_ADDRESS;
			}
		}
		for (int published_handle = 0; published_handle < VIDEO_OUT_NUM_MAX; ++published_handle)
		{
			const auto& published_ctx = m_video_out_ctx[published_handle];
			for (int published_slot = 0; published_slot < 16; ++published_slot)
			{
				if (published_ctx.buffers[published_slot].buffer == addresses[i])
				{
					videoout_lifecycle_trace_record(
					    "event=register_result thread=%d handle=%d generation=%" PRIu64
					    " result=address_already_published request_slot=%d published_handle=%d published_slot=%d "
					    "address=%p api_result=%d",
					    Core::Thread::GetThreadIdUnique(), handle, generation, i + start_index, published_handle,
					    published_slot, addresses[i], VIDEO_OUT_ERROR_INVALID_ADDRESS);
					return VIDEO_OUT_ERROR_INVALID_ADDRESS;
				}
			}
		}
	}

	VideoOutBufferInfo staged[16] {};
	const int          set_id = ctx.buffers_sets_seq;
	for (int i = 0; i < buffer_num; i++)
	{
		auto& buffer         = staged[i];
		buffer.set_id        = set_id;
		buffer.buffer        = addresses[i];
		buffer.buffer_size   = buffer_size;
		buffer.buffer_pitch  = buffer_pitch;
		buffer.generation    = generation;
		buffer.tiled         = tile;
		buffer.neo           = neo;
		videoout_lifecycle_trace_record(
		    "event=register_create_begin thread=%d handle=%d generation=%" PRIu64
		    " set=%d slot=%d address=%p size=%" PRIu64,
		    Core::Thread::GetThreadIdUnique(), handle, generation, set_id, i + start_index, addresses[i], buffer_size);
		buffer.buffer_vulkan = create_func(create_context, graphic_ctx, reinterpret_cast<uint64_t>(addresses[i]), buffer_size,
		                                   vulkan_buffer_info, &buffer.tracked_handle);
		EXIT_NOT_IMPLEMENTED(buffer.buffer_vulkan == nullptr);
		videoout_lifecycle_trace_record(
		    "event=register_create_result thread=%d handle=%d generation=%" PRIu64
		    " set=%d slot=%d address=%p image=%p result=ok",
		    Core::Thread::GetThreadIdUnique(), handle, generation, set_id, i + start_index, addresses[i],
		    static_cast<void*>(buffer.buffer_vulkan));
	}

	for (int i = 0; i < buffer_num; i++)
	{
		ctx.buffers[i + start_index] = staged[i];
		printf("\tbuffers[%d] = %016" PRIx64 "\n", i + start_index, reinterpret_cast<uint64_t>(addresses[i]));
	}

	VideoOutBufferSet new_set {};
	new_set.start_index = start_index;
	new_set.num         = buffer_num;
	new_set.set_id      = set_id;
	new_set.attr        = *attribute;
	ctx.buffers_sets.Add(new_set);
	ctx.buffers_sets_seq++;
	if (videoout_lifecycle_trace_enabled())
	{
		for (int i = 0; i < buffer_num; ++i)
		{
			auto& record                = m_trace_buffer_history[m_trace_buffer_history_next];
			record.address               = addresses[i];
			record.size                  = buffer_size;
			record.generation            = generation;
			record.publication_sequence  = ++m_publication_sequence;
			record.miss_count            = 0;
			record.handle                = handle;
			record.set_id                = set_id;
			record.closed                = false;
			m_trace_buffer_history_next = (m_trace_buffer_history_next + 1u) % TRACE_BUFFER_HISTORY_MAX;
			if (m_trace_buffer_history_size < TRACE_BUFFER_HISTORY_MAX)
			{
				m_trace_buffer_history_size++;
			}
		}
		videoout_lifecycle_trace_record(
		    "event=register_commit thread=%d handle=%d generation=%" PRIu64
		    " set=%d start=%d count=%d sets=%u next_set=%d",
		    Core::Thread::GetThreadIdUnique(), handle, generation, set_id, start_index, buffer_num,
		    ctx.buffers_sets.Size(), ctx.buffers_sets_seq);
	}

	return set_id;
}

template <typename End> class VideoOutRegistrationEpoch final
{
public:
	VideoOutRegistrationEpoch(uint64_t token, End& end): m_token(token), m_end(end) {}
	~VideoOutRegistrationEpoch() { EXIT_IF(!m_end(m_token)); }
	VideoOutRegistrationEpoch(const VideoOutRegistrationEpoch&) = delete;
	VideoOutRegistrationEpoch& operator=(const VideoOutRegistrationEpoch&) = delete;
private:
	uint64_t m_token;
	End& m_end;
};

template <typename Begin, typename Owns, typename End, typename Register>
static int register_buffers_with_handoff_claim(bool enabled, Begin begin, Owns owns, End end, Register registration)
{
	if (!enabled) return registration(0);
	const uint64_t token = begin();
	if (token == 0) return VIDEO_OUT_ERROR_RESOURCE_BUSY;
	VideoOutRegistrationEpoch<End> epoch(token, end);
	if (!owns(token)) return VIDEO_OUT_ERROR_RESOURCE_BUSY;
	return registration(token);
}

#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && defined(LUMEN_ANDROID_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME))
template <typename Register>
static int register_buffers_with_runtime_handoff_claim(bool enabled, Register registration)
{
	// Acquire before VideoOut, renderer or tracker locks and retain the epoch
	// through upload and enrollment. A self HLE reader transfers to the writer;
	// its ordinary admission is restored only after publication releases locks.
	return register_buffers_with_handoff_claim(enabled,
	    [] { return LumenAndroidFexCoreBeginGuestMemoryMutationV2(LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2); },
	    [](uint64_t token) { return LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(token) == 1; },
	    [](uint64_t token) { return LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 1; },
	    [&](uint64_t token) {
		    std::optional<Core::VirtualMemory::GuestMemoryMutationOwnerScope> core_owner;
		    std::optional<GuestCpu::MemoryHandoffBypass> bypass;
		    if (token != 0)
		    {
			    core_owner.emplace(token);
			    if (!core_owner->IsActive()) return VIDEO_OUT_ERROR_RESOURCE_BUSY;
			    bypass.emplace();
		    }
		    return registration(token);
	    });
}
#endif

static int register_buffers_internal(VideoOutContext* owner, int handle, int start_index, const void* const* addresses, int buffer_num,
                                     const VideoOutBufferAttribute* attribute)
{
	EXIT_IF(owner == nullptr);
	Graphics::WindowWaitForGraphicInitialized();
	Graphics::GraphicsRenderCreateContext();

	// GetGraphicCtx takes VideoOutContext::m_mutex itself. Resolve it before the
	// publication transaction to preserve the established lock order.
	auto* graphic_ctx = owner->GetGraphicCtx();
	const auto registration = [&](uint64_t) {
		return owner->RegisterBuffers(handle, start_index, addresses, buffer_num, attribute, graphic_ctx, create_video_out_buffer, nullptr);
	};
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && defined(LUMEN_ANDROID_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME))
	// Enroll the snapshot and every backing guard under one exclusive epoch.
	// This point owns no VideoOut, renderer or tracker lock and no data borrow.
	// Registration returns with its locks released before the epoch readmits CPU.
	return register_buffers_with_runtime_handoff_claim(Graphics::GpuMemoryCpuHandoffEnabled(), registration);
#else
	return registration(0);
#endif
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
struct VideoOutEnrollmentFactoryState
{
	Android::FexCoreLab::GuestMemoryMutationBarrier* barrier = nullptr;
	std::atomic<uint64_t>* progress = nullptr;
	std::atomic<bool>* peer_checkpoint = nullptr;
	uint64_t token = 0;
	unsigned calls = 0;
	uint32_t uploaded = 0;
	bool upload_exact = false;
	bool valid = true;
};

static Graphics::VideoOutVulkanImage* create_video_out_buffer_for_enrollment_test(
    void* opaque, Graphics::GraphicContext*, uint64_t address, uint64_t,
    const Graphics::VideoOutBufferObject&, Graphics::GpuMemoryObjectHandle* tracked_handle)
{
	auto& state = *static_cast<VideoOutEnrollmentFactoryState*>(opaque);
	state.valid &= state.barrier->OwnsMutation(state.token) && !state.barrier->OwnsAdditiveMutation(state.token) &&
	               !state.barrier->CurrentThreadOwnsHostMemoryReadLease() && state.peer_checkpoint->load(std::memory_order_acquire);
	std::memcpy(&state.uploaded, reinterpret_cast<const void*>(address), sizeof(state.uploaded));
	state.upload_exact = state.uploaded != 0 && state.uploaded == state.progress->load();
	++state.calls;
	// Native allocation/upload is modeled; the admission and publication method
	// are real. This opaque test image is never submitted or dereferenced.
	auto* image = reinterpret_cast<Graphics::VideoOutVulkanImage*>(opaque);
	*tracked_handle = {image, 1, 0, 0};
	return image;
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
struct VideoOutRuntimeEnrollmentState
{
	VideoOutContext* context = nullptr;
	VideoOutBufferAttribute attribute {};
	int handle = -1;
	uint64_t address = 0;
	uint64_t token = 0;
	unsigned calls = 0;
	unsigned uploads = 0;
	bool valid = true;
};

static Graphics::VideoOutVulkanImage* create_video_out_buffer_for_runtime_enrollment_test(
    void* opaque, Graphics::GraphicContext*, uint64_t address, uint64_t,
    const Graphics::VideoOutBufferObject&, Graphics::GpuMemoryObjectHandle* tracked_handle)
{
	auto& state = *static_cast<VideoOutRuntimeEnrollmentState*>(opaque);
	uint32_t value = 0;
	std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
	state.valid &= value == 0x1234abcd && LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(state.token) == 1 &&
	               LumenAndroidFexCoreOwnsGuestMemoryReadLeaseV1() == 0;
	++state.uploads;
	// Only allocation/upload is modeled; runtime admission, Core owner scope,
	// HLE transition and the production registration transaction are real.
	auto* image = reinterpret_cast<Graphics::VideoOutVulkanImage*>(opaque);
	*tracked_handle = {image, 1, 0, 0};
	return image;
}

static uint32_t video_out_runtime_enrollment_host_call(void* opaque, uint64_t target, uint64_t* gprs, uint64_t* next) noexcept
{
	auto& state = *static_cast<VideoOutRuntimeEnrollmentState*>(opaque);
	if (target != reinterpret_cast<uint64_t>(&video_out_runtime_enrollment_host_call))
		return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	++state.calls;
	state.valid &= LumenAndroidFexCoreOwnsGuestMemoryReadLeaseV1() == 1;
	const void* addresses[] = {reinterpret_cast<const void*>(state.address)};
	const auto registration = [&](uint64_t token) {
		state.token = token;
		state.valid &= token != 0 && LumenAndroidFexCoreOwnsGuestMemoryReadLeaseV1() == 0;
		return state.context->RegisterBuffers(state.handle, 0, addresses, 1, &state.attribute, nullptr,
		    create_video_out_buffer_for_runtime_enrollment_test, &state);
	};
	const int published = register_buffers_with_runtime_handoff_claim(true, registration);
	state.valid &= published == 0 && LumenAndroidFexCoreOwnsGuestMemoryReadLeaseV1() == 1 &&
	               LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(state.token) == 0;
	const int refused = register_buffers_with_runtime_handoff_claim(true, registration);
	state.valid &= refused == VIDEO_OUT_ERROR_SLOT_OCCUPIED && LumenAndroidFexCoreOwnsGuestMemoryReadLeaseV1() == 1 &&
	               LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(state.token) == 0;
	Core::VirtualMemory::GuestDataLease return_address;
	if (!Core::VirtualMemory::TryAcquireGuestDataLease(gprs[4], sizeof(*next), Core::VirtualMemory::Mode::Read, &return_address))
		return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	std::memcpy(next, reinterpret_cast<const void*>(return_address.HostBegin()), sizeof(*next));
	gprs[4] += sizeof(*next);
	gprs[0] = published;
	return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
}

static uint32_t video_out_runtime_enrollment_code_query(void*, uint64_t address, uint64_t* base, uint64_t* size,
                                                      uint32_t* writable) noexcept
{
	constexpr uint64_t page = 4096;
	const uint64_t start = address & ~(page - 1);
	Core::VirtualMemory::CodeReadLease lease;
	if (!Core::VirtualMemory::TryAcquireCodeReadLease(start, page, &lease)) return 0;
	*base = start; *size = page; *writable = 0;
	Core::VirtualMemory::ReleaseCodeReadLease(&lease);
	return 1;
}

static bool video_out_runtime_enrollment_test(const VideoOutBufferAttribute& attribute, uint64_t bytes, uint64_t align)
{
	namespace Vm = Core::VirtualMemory;
	constexpr uint64_t mapping_size = 0x4000;
	const uint64_t code = Vm::Alloc(0, mapping_size, Vm::Mode::ReadWrite);
	const uint64_t stack = Vm::Alloc(0, mapping_size, Vm::Mode::ReadWrite);
	std::vector<uint8_t> backing(static_cast<size_t>(bytes + align));
	const auto base = reinterpret_cast<uintptr_t>(backing.data());
	VideoOutRuntimeEnrollmentState fixture;
	fixture.context = new VideoOutContext;
	fixture.handle = fixture.context->Open();
	fixture.attribute = attribute;
	fixture.address = ((base + align - 1) / align) * align;
	const uint32_t uploaded = 0x1234abcd;
	std::memcpy(reinterpret_cast<void*>(fixture.address), &uploaded, sizeof(uploaded));
	// Original x86 leaf: movabs rax,registered target; call rax; ret.
	std::array<uint8_t, 13> program {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0, 0xc3};
	const uint64_t target = reinterpret_cast<uint64_t>(&video_out_runtime_enrollment_host_call);
	std::memcpy(program.data() + 2, &target, sizeof(target));
	bool ready = code != 0 && stack != 0 && fixture.handle >= 0 && LumenAndroidFexCoreOwnsGuestMemoryReadLeaseV1() == 0;
	if (ready)
	{
		std::memcpy(reinterpret_cast<void*>(code), program.data(), program.size());
		ready = Vm::Protect(code, mapping_size, Vm::Mode::ExecuteRead) && LumenAndroidFexCoreRegisterHostCallTargetV1(target) == 1;
	}
	LumenAndroidFexCoreGuestStateV1 state {};
	state.structSize = sizeof(state); state.abiVersion = 1; state.rip = code;
	state.gprs[4] = stack + mapping_size - sizeof(uint64_t);
	if (stack != 0)
	{
		const uint64_t root = LumenAndroidFexCoreRootReturnAddressV1();
		std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &root, sizeof(root));
	}
	const uint32_t result = ready ? LumenAndroidFexCoreRunGuestV1(&state, video_out_runtime_enrollment_host_call,
	    video_out_runtime_enrollment_code_query, &fixture) : UINT32_MAX;
	const bool outside = LumenAndroidFexCoreOwnsGuestMemoryReadLeaseV1() == 0;
	const bool closed = fixture.handle >= 0 && fixture.context->Close(fixture.handle) == OK;
	const bool code_freed = code != 0 && Vm::Free(code);
	const bool stack_freed = stack != 0 && Vm::Free(stack);
	const bool valid = ready && result == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 && state.gprs[0] == 0 &&
	                   fixture.valid && fixture.calls == 1 && fixture.uploads == 1 && outside && closed && code_freed && stack_freed;
	std::fprintf(stderr, "[VIDEOOUT_HANDOFF_RUNTIME_ENROLLMENT_TEST] pass=%u real-FEX=1 production-claim=1"
	    " HLE-reader=restored exclusive-reader=absent outside-reader=absent publication=once occupied-slot=epoch-closed"
	    " native-upload=modeled runtime=%u calls=%u uploads=%u\n", valid ? 1u : 0u, result, fixture.calls, fixture.uploads);
	return valid;
}
#endif

bool VideoOutRegistrationHandoffContractTest()
{
	using Barrier = Android::FexCoreLab::GuestMemoryMutationBarrier;
	using Scope = Android::FexCoreLab::GuestMemoryMutationScope;
	Barrier barrier;
	VideoOutBufferAttribute attribute {};
	attribute.pixel_format = 0x80000000;
	// Use the smallest layout supported by the production VideoOut tile table.
	attribute.width = attribute.pitch_in_pixel = 1280;
	attribute.height = 720;
	uint64_t bytes = 0, align = 0, pitch = 0;
	calc_buffer_size(&attribute, &bytes, &align, &pitch);
	if (bytes == 0 || align == 0 || bytes > 4 * 1024 * 1024 || align > 4 * 1024 * 1024)
	{
		std::fprintf(stderr, "[VIDEOOUT_HANDOFF_ENROLLMENT_TEST] pass=0 stage=buffer-layout bytes=%" PRIu64 " align=%" PRIu64 "\n",
		             bytes, align);
		return false;
	}
	std::vector<uint8_t> backing(static_cast<size_t>(bytes + align));
	const auto base = reinterpret_cast<uintptr_t>(backing.data());
	const auto address = ((base + align - 1) / align) * align;
	const void* addresses[] = {reinterpret_cast<const void*>(address)};
	// VideoOut contexts have process lifetime; the existing publication gate
	// follows the same lifetime because their destructor is deliberately fatal.
	auto* context = new VideoOutContext;
	const int handle = context->Open();
	if (handle < 0 || !barrier.EnterExecution())
	{
		std::fprintf(stderr, "[VIDEOOUT_HANDOFF_ENROLLMENT_TEST] pass=0 stage=execution-entry handle=%d\n", handle);
		return false;
	}
	if (!barrier.SuspendExecutionForHostCall())
	{
		barrier.LeaveExecution();
		(void)context->Close(handle);
		std::fprintf(stderr, "[VIDEOOUT_HANDOFF_ENROLLMENT_TEST] pass=0 stage=HLE-entry\n");
		return false;
	}
	std::atomic<bool> stop {false}, started {false}, peer_valid {true}, peer_checkpoint {false};
	std::atomic<uint64_t> progress {0};
	std::thread peer([&] {
		peer_valid.store(barrier.EnterExecution());
		started.store(true, std::memory_order_release);
		while (!stop.load(std::memory_order_acquire) && peer_valid.load())
		{
			const auto value = static_cast<uint32_t>(progress.fetch_add(1) + 1);
			std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
			peer_checkpoint.store(true, std::memory_order_release);
			peer_valid.store(barrier.CheckpointSafepoint());
			peer_checkpoint.store(false, std::memory_order_release);
		}
		barrier.LeaveExecution();
	});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while ((!started.load(std::memory_order_acquire) || progress.load() == 0) &&
	       std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
	VideoOutEnrollmentFactoryState factory {&barrier, &progress, &peer_checkpoint};
	unsigned begins = 0, closes = 0, registrations = 0;
	bool close_valid = true;
	Barrier::MutationDenialDiagnostic denial {};
	const auto begin = [&] {
		++begins;
		factory.token = barrier.BeginMutation(Scope::Exclusive, [](uint64_t) { return true; }, [](uint64_t) {},
		                                     std::chrono::seconds(2), &denial);
		return factory.token;
	};
	const auto owns = [&](uint64_t token) { return barrier.OwnsMutation(token) && !barrier.OwnsAdditiveMutation(token); };
	const auto end = [&](uint64_t token) { ++closes; const bool valid = barrier.EndMutation(token); close_valid &= valid; return valid; };
	const auto register_set = [&](uint64_t) {
		++registrations;
		return context->RegisterBuffers(handle, 0, addresses, 1, &attribute, nullptr,
		                                create_video_out_buffer_for_enrollment_test, &factory);
	};
	const int published = register_buffers_with_handoff_claim(true, begin, owns, end, register_set);
	const bool upload_exact = factory.upload_exact;
	const int refused = register_buffers_with_handoff_claim(true, begin, owns, end, register_set);
	const auto progress_after_refusal = progress.load();
	const auto refusal_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (progress.load() == progress_after_refusal && std::chrono::steady_clock::now() < refusal_deadline)
		std::this_thread::yield();
	const bool peer_readmitted = progress.load() > progress_after_refusal;
	const bool retained = barrier.RetainGuestDataLease();
	const int denied = register_buffers_with_handoff_claim(true, begin, owns, end, register_set);
	const bool released = retained && barrier.ReleaseGuestDataLease();
	const bool reader_restored = barrier.CurrentThreadOwnsHostMemoryReadLease();
	stop.store(true, std::memory_order_release);
	peer.join();
	const bool resumed = barrier.ResumeExecutionAfterHostCall();
	barrier.LeaveExecution();
	const bool closed = context->Close(handle) == OK;
	const bool valid = started.load() && peer_valid.load() && factory.valid && upload_exact && published == 0 &&
	                   refused == VIDEO_OUT_ERROR_SLOT_OCCUPIED && denied == VIDEO_OUT_ERROR_RESOURCE_BUSY &&
	                   retained && released && begins == 3 && closes == 2 && registrations == 2 && factory.calls == 1 &&
	                   close_valid && peer_readmitted && reader_restored && !denial.writer && denial.requestEpoch == 0 &&
	                   denial.hostCallReaders == 1 && denial.reason == Barrier::MutationDenialReason::GuestDataLeaseActive &&
	                   resumed && closed;
	std::fprintf(stderr, "[VIDEOOUT_HANDOFF_ENROLLMENT_TEST] pass=%u real-barrier=1 HLE-upgrade=1 foreign-execution=parked"
	                     " upload-exact=%u registration-calls=%u native-model-calls=%u claims=%u closes=%u"
	                     " occupied-slot=epoch-closed foreign-execution-readmitted=%u data-lease-denial=before-registration native-upload=modeled\n",
	             valid ? 1u : 0u, upload_exact ? 1u : 0u, registrations, factory.calls, begins, closes, peer_readmitted ? 1u : 0u);
	if (!valid) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	return video_out_runtime_enrollment_test(attribute, bytes, align);
#else
	return true;
#endif
}

struct VideoOutBufferPublicationFactoryState
{
	Graphics::VideoOutVulkanImage* images[2] = {};
	std::atomic_uint               calls {0};
	std::atomic_bool               second_call_entered {false};
	std::atomic_bool               release_second_call {false};
	bool                           profile_valid = true;
};

static Graphics::VideoOutVulkanImage* create_video_out_buffer_for_publication_test(
    void* context, Graphics::GraphicContext* graphic_ctx, uint64_t, uint64_t,
    const Graphics::VideoOutBufferObject& info, Graphics::GpuMemoryObjectHandle* tracked_handle)
{
	auto* state = static_cast<VideoOutBufferPublicationFactoryState*>(context);
	if (state == nullptr || tracked_handle == nullptr)
	{
		return nullptr;
	}
	const uint32_t index = state->calls.fetch_add(1, std::memory_order_relaxed);
	state->profile_valid = state->profile_valid && graphic_ctx == nullptr && index < 2 &&
	                       info.type == Graphics::GpuMemoryObjectType::VideoOutBuffer &&
	                       info.params[Graphics::VideoOutBufferObject::PARAM_FORMAT] ==
	                           static_cast<uint64_t>(Graphics::VideoOutBufferFormat::B8G8R8A8Srgb) &&
	                       info.params[Graphics::VideoOutBufferObject::PARAM_WIDTH] == 1920 &&
	                       info.params[Graphics::VideoOutBufferObject::PARAM_HEIGHT] == 1080 &&
	                       info.params[Graphics::VideoOutBufferObject::PARAM_TILED] == 1 &&
	                       info.params[Graphics::VideoOutBufferObject::PARAM_NEO] == (Config::IsNeo() ? 1u : 0u) &&
	                       info.params[Graphics::VideoOutBufferObject::PARAM_PITCH] == 1920;
	if (index >= 2)
	{
		return nullptr;
	}
	if (index == 1)
	{
		state->second_call_entered.store(true, std::memory_order_release);
		while (!state->release_second_call.load(std::memory_order_acquire))
		{
			std::this_thread::yield();
		}
	}
	*tracked_handle = {state->images[index], index + 1u, 0, static_cast<int>(index)};
	return state->images[index];
}

bool VideoOutBufferPublicationContractTest()
{
	auto wait_for = [](auto&& predicate, std::chrono::milliseconds timeout)
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (!predicate() && std::chrono::steady_clock::now() < deadline)
		{
			std::this_thread::yield();
		}
		return predicate();
	};

	// VideoOutContext is process-owned in production and intentionally has a
	// fail-closed destructor. Match that lifetime in this headless contract test.
	auto* context = new VideoOutContext;
	const int handle = context->Open();
	if (handle < 0)
	{
		std::fprintf(stderr, "VideoOut publication setup failed: handle=%d\n", handle);
		return false;
	}
	const uint64_t initial_generation = context->m_video_out_ctx[handle].generation.load(std::memory_order_acquire);
	const int      busy_open          = context->Open();
	const int      invalid_close_low  = context->Close(-1);
	const int      invalid_close_high = context->Close(VideoOutContext::VIDEO_OUT_NUM_MAX);
	const bool handle_contract = busy_open < 0 && invalid_close_low == VIDEO_OUT_ERROR_INVALID_HANDLE &&
	                             invalid_close_high == VIDEO_OUT_ERROR_INVALID_HANDLE &&
	                             context->m_video_out_ctx[handle].opened && initial_generation != 0;

	VideoOutBufferAttribute attribute {};
	attribute.pixel_format  = 0x80000000;
	attribute.tiling_mode   = 0;
	attribute.aspect_ratio  = 0;
	attribute.width         = 1920;
	attribute.height        = 1080;
	attribute.pitch_in_pixel = 1920;
	attribute.option        = 0;

	uint64_t buffer_size  = 0;
	uint64_t buffer_align = 0;
	uint64_t buffer_pitch = 0;
	calc_buffer_size(&attribute, &buffer_size, &buffer_align, &buffer_pitch);
	if (buffer_size == 0 || buffer_align == 0 || buffer_pitch != 1920)
	{
		std::fprintf(stderr, "VideoOut publication setup failed: size=%" PRIu64 " align=%" PRIu64 " pitch=%" PRIu64 "\n",
		             buffer_size, buffer_align, buffer_pitch);
		return false;
	}
	const uint64_t first_addr = ((0x100000000ull + buffer_align - 1u) / buffer_align) * buffer_align;
	const uint64_t span       = ((buffer_size + buffer_align - 1u) / buffer_align) * buffer_align;
	void* const addresses[2]  = {reinterpret_cast<void*>(first_addr), reinterpret_cast<void*>(first_addr + span)};

	auto* const first_image  = reinterpret_cast<Graphics::VideoOutVulkanImage*>(uintptr_t {0x1000});
	auto* const second_image = reinterpret_cast<Graphics::VideoOutVulkanImage*>(uintptr_t {0x2000});
	VideoOutBufferPublicationFactoryState factory;
	factory.images[0] = first_image;
	factory.images[1] = second_image;

	const int invalid_handle_low = context->RegisterBuffers(-1, 2, addresses, 2, &attribute, nullptr,
	                                                        create_video_out_buffer_for_publication_test, &factory);
	const int invalid_handle_high = context->RegisterBuffers(VideoOutContext::VIDEO_OUT_NUM_MAX, 2, addresses, 2, &attribute, nullptr,
	                                                         create_video_out_buffer_for_publication_test, &factory);
	const int invalid_start = context->RegisterBuffers(handle, -1, addresses, 2, &attribute, nullptr,
	                                                   create_video_out_buffer_for_publication_test, &factory);
	const int invalid_count = context->RegisterBuffers(handle, 2, addresses, 17, &attribute, nullptr,
	                                                   create_video_out_buffer_for_publication_test, &factory);
	const int invalid_span = context->RegisterBuffers(handle, 15, addresses, 2, &attribute, nullptr,
	                                                  create_video_out_buffer_for_publication_test, &factory);
	const auto& initial_config = context->m_video_out_ctx[handle];
	const bool invalid_inputs_atomic = invalid_handle_low == VIDEO_OUT_ERROR_INVALID_HANDLE &&
	                                   invalid_handle_high == VIDEO_OUT_ERROR_INVALID_HANDLE &&
	                                   invalid_start == VIDEO_OUT_ERROR_INVALID_VALUE &&
	                                   invalid_count == VIDEO_OUT_ERROR_INVALID_VALUE &&
	                                   invalid_span == VIDEO_OUT_ERROR_INVALID_VALUE &&
	                                   factory.calls.load(std::memory_order_relaxed) == 0 &&
	                                   initial_config.buffers_sets.IsEmpty() && initial_config.buffers_sets_seq == 0;
	void* const duplicate_addresses[2] = {addresses[0], addresses[0]};
	const int duplicate_request = context->RegisterBuffers(handle, 4, duplicate_addresses, 2, &attribute, nullptr,
	                                                      create_video_out_buffer_for_publication_test, &factory);
	const bool duplicate_request_atomic = duplicate_request == VIDEO_OUT_ERROR_INVALID_ADDRESS &&
	                                      factory.calls.load(std::memory_order_relaxed) == 0 &&
	                                      initial_config.buffers_sets.IsEmpty() && initial_config.buffers_sets_seq == 0 &&
	                                      initial_config.buffers[4].buffer == nullptr &&
	                                      initial_config.buffers[5].buffer == nullptr;

	std::atomic_int  publication_result {VIDEO_OUT_ERROR_INVALID_VALUE};
	std::atomic_bool publication_finished {false};
	std::thread publisher(
	    [context, handle, &addresses, &attribute, &factory, &publication_result, &publication_finished]()
	    {
		    publication_result.store(
		        context->RegisterBuffers(handle, 2, addresses, 2, &attribute, nullptr,
		                                 create_video_out_buffer_for_publication_test, &factory),
		        std::memory_order_relaxed);
		    publication_finished.store(true, std::memory_order_release);
	    });

	const bool factory_paused = wait_for(
	    [&]() { return factory.second_call_entered.load(std::memory_order_acquire); }, std::chrono::seconds(2));
	bool publication_lock_held = false;
	if (factory_paused)
	{
		publication_lock_held = !context->m_mutex.TryLock();
		if (!publication_lock_held)
		{
			context->m_mutex.Unlock();
		}
	}

	VideoOutBufferImageInfo observed_during_publication;
	std::atomic_bool         finder_started {false};
	std::atomic_bool         finder_finished {false};
	std::thread finder(
	    [context, &addresses, &observed_during_publication, &finder_started, &finder_finished]()
	    {
		    finder_started.store(true, std::memory_order_release);
		    observed_during_publication = context->FindImage(addresses[1]);
		    finder_finished.store(true, std::memory_order_release);
	    });
	const bool finder_entered = wait_for(
	    [&]() { return finder_started.load(std::memory_order_acquire); }, std::chrono::seconds(2));
	const bool finder_blocked = finder_entered && !finder_finished.load(std::memory_order_acquire);

	factory.release_second_call.store(true, std::memory_order_release);
	publisher.join();
	finder.join();

	const auto first  = context->FindImage(addresses[0]);
	const auto second = context->FindImage(addresses[1]);
	const auto& config = context->m_video_out_ctx[handle];
	const bool first_metadata = first.slot == 2 && first.generation == initial_generation && first.set_id == 0 &&
	                            first.registered_tiled && first.registered_neo == Config::IsNeo() &&
	                            first.tracked_handle.host_object == first_image &&
	                            first.tracked_handle.lifetime_generation == 1 && first.tracked_handle.heap_id == 0 &&
	                            first.tracked_handle.object_id == 0;
	const bool second_metadata = second.slot == 3 && second.generation == initial_generation && second.set_id == 0 &&
	                             second.registered_tiled && second.registered_neo == Config::IsNeo() &&
	                             second.tracked_handle.host_object == second_image &&
	                             second.tracked_handle.lifetime_generation == 2 && second.tracked_handle.heap_id == 0 &&
	                             second.tracked_handle.object_id == 1;
	const bool published = invalid_inputs_atomic && duplicate_request_atomic && factory_paused && publication_lock_held && finder_blocked &&
	                       publication_finished.load(std::memory_order_acquire) && publication_result.load(std::memory_order_relaxed) == 0 &&
	                       factory.calls.load(std::memory_order_relaxed) == 2 && factory.profile_valid &&
	                       config.buffers_sets.Size() == 1 && config.buffers_sets_seq == 1 &&
	                       first.image == first_image && first.index == 0 && first.buffer_size == buffer_size && first_metadata &&
	                       first.buffer_pitch == buffer_pitch && second.image == second_image && second.index == 1 &&
	                       second.buffer_size == buffer_size && second.buffer_pitch == buffer_pitch && second_metadata &&
	                       observed_during_publication.image == second_image && observed_during_publication.index == 1 &&
	                       observed_during_publication.buffer_size == buffer_size &&
	                       observed_during_publication.buffer_pitch == buffer_pitch;

	const uint32_t calls_before_rejection = factory.calls.load(std::memory_order_relaxed);
	const int      occupied = context->RegisterBuffers(handle, 2, addresses, 2, &attribute, nullptr,
	                                                  create_video_out_buffer_for_publication_test, &factory);
	const bool rejection_atomic = occupied == VIDEO_OUT_ERROR_SLOT_OCCUPIED &&
	                              factory.calls.load(std::memory_order_relaxed) == calls_before_rejection &&
	                              config.buffers_sets.Size() == 1 && config.buffers_sets_seq == 1 &&
	                              config.buffers[2].buffer == addresses[0] &&
	                              config.buffers[2].buffer_vulkan == first_image &&
	                              config.buffers[3].buffer == addresses[1] &&
	                              config.buffers[3].buffer_vulkan == second_image;
	void* const published_address[1] = {addresses[0]};
	const uint32_t calls_before_published_rejection = factory.calls.load(std::memory_order_relaxed);
	const int published_address_result = context->RegisterBuffers(handle, 4, published_address, 1, &attribute, nullptr,
	                                                            create_video_out_buffer_for_publication_test, &factory);
	const bool published_address_atomic = published_address_result == VIDEO_OUT_ERROR_INVALID_ADDRESS &&
	                                      factory.calls.load(std::memory_order_relaxed) == calls_before_published_rejection &&
	                                      config.buffers_sets.Size() == 1 && config.buffers_sets_seq == 1 &&
	                                      config.buffers[4].buffer == nullptr && config.buffers[4].buffer_vulkan == nullptr &&
	                                      config.buffers[2].buffer == addresses[0] &&
	                                      config.buffers[2].buffer_vulkan == first_image &&
	                                      config.buffers[3].buffer == addresses[1] &&
	                                      config.buffers[3].buffer_vulkan == second_image;

	const int close_result = context->Close(handle);
	const bool closed = close_result == OK && context->FindImage(addresses[0]).image == nullptr &&
	                    context->FindImage(addresses[1]).image == nullptr;
	const int repeated_close = context->Close(handle);
	const uint32_t calls_before_closed_rejection = factory.calls.load(std::memory_order_relaxed);
	const int closed_registration = context->RegisterBuffers(handle, 2, addresses, 2, &attribute, nullptr,
	                                                         create_video_out_buffer_for_publication_test, &factory);
	const bool closed_rejected = closed_registration == VIDEO_OUT_ERROR_INVALID_HANDLE &&
	                             factory.calls.load(std::memory_order_relaxed) == calls_before_closed_rejection;

	factory.calls.store(0, std::memory_order_relaxed);
	factory.second_call_entered.store(false, std::memory_order_relaxed);
	factory.release_second_call.store(false, std::memory_order_relaxed);
	const int reopened_handle = context->Open();
	const uint64_t reopened_generation = reopened_handle >= 0
	                                          ? context->m_video_out_ctx[reopened_handle].generation.load(std::memory_order_acquire)
	                                          : 0;
	const int last_slot_result = context->RegisterBuffers(reopened_handle, 15, addresses, 1, &attribute, nullptr,
	                                                      create_video_out_buffer_for_publication_test, &factory);
	const auto last_slot = context->FindImage(addresses[0]);
	const bool last_slot_valid = reopened_handle == handle && reopened_generation > initial_generation && last_slot_result == 0 &&
	                             factory.calls.load(std::memory_order_relaxed) == 1 &&
	                             last_slot.image == first_image && last_slot.index == 0 &&
	                             context->m_video_out_ctx[reopened_handle].buffers[15].buffer == addresses[0];
	const int reopened_close = context->Close(reopened_handle);

	if (!handle_contract || !published || !rejection_atomic || !published_address_atomic || !closed ||
	    repeated_close != VIDEO_OUT_ERROR_INVALID_HANDLE ||
	    !closed_rejected || !last_slot_valid || reopened_close != OK)
	{
		std::fprintf(stderr,
		             "VideoOut publication detail handle_contract=%u invalid_inputs=%u paused=%u locked=%u finder_entered=%u "
		             "finder_blocked=%u duplicate_request=%u published=%u rejected=%u published_address=%u closed=%u "
		             "repeated_close=%d closed_rejected=%u last_slot=%u "
		             "reopened_close=%d result=%d calls=%u profile=%u sets=%u seq=%d\n",
		             handle_contract ? 1u : 0u, invalid_inputs_atomic ? 1u : 0u, factory_paused ? 1u : 0u,
		             publication_lock_held ? 1u : 0u,
		             finder_entered ? 1u : 0u,
		             finder_blocked ? 1u : 0u, duplicate_request_atomic ? 1u : 0u, published ? 1u : 0u,
		             rejection_atomic ? 1u : 0u, published_address_atomic ? 1u : 0u, closed ? 1u : 0u,
		             repeated_close, closed_rejected ? 1u : 0u, last_slot_valid ? 1u : 0u, reopened_close,
		             publication_result.load(std::memory_order_relaxed), factory.calls.load(std::memory_order_relaxed),
		             factory.profile_valid ? 1u : 0u, config.buffers_sets.Size(), config.buffers_sets_seq);
	}
	return handle_contract && published && rejection_atomic && published_address_atomic && closed &&
	       repeated_close == VIDEO_OUT_ERROR_INVALID_HANDLE && closed_rejected && last_slot_valid && reopened_close == OK;
}
#endif

LUMEN_SYSV_ABI int VideoOutRegisterBuffers(int handle, int start_index, void* const* addresses, int buffer_num,
                                          const VideoOutBufferAttribute* attribute)
{
	PRINT_NAME();

	EXIT_IF(g_video_out_context == nullptr);
	if (!g_video_out_context->IsOpen(handle))
	{
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	if (addresses == nullptr)
	{
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (attribute == nullptr)
	{
		return VIDEO_OUT_ERROR_INVALID_OPTION;
	}

	if (start_index < 0 || start_index > 15 || buffer_num < 1 || buffer_num > 16 || start_index + buffer_num > 16)
	{
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	printf("\t start_index    = %d\n", start_index);
	printf("\t buffer_num     = %d\n", buffer_num);
	printf("\t pixel_format   = 0x%08" PRIx32 "\n", attribute->pixel_format);
	printf("\t tiling_mode    = %" PRIu32 "\n", attribute->tiling_mode);
	printf("\t aspect_ratio   = %" PRIu32 "\n", attribute->aspect_ratio);
	printf("\t width          = %" PRIu32 "\n", attribute->width);
	printf("\t height         = %" PRIu32 "\n", attribute->height);
	printf("\t pitch_in_pixel = %" PRIu32 "\n", attribute->pitch_in_pixel);
	printf("\t option         = %" PRIu32 "\n", attribute->option);

	// EXIT_NOT_IMPLEMENTED(attribute->pixel_format != 0x80000000);
	EXIT_NOT_IMPLEMENTED(attribute->tiling_mode != 0);
	EXIT_NOT_IMPLEMENTED(attribute->aspect_ratio != 0);
	EXIT_NOT_IMPLEMENTED(attribute->pitch_in_pixel != attribute->width);
	EXIT_NOT_IMPLEMENTED(attribute->option != 0);

	return register_buffers_internal(g_video_out_context, handle, start_index, addresses, buffer_num, attribute);
}

VideoOutBufferImageInfo VideoOutGetImage(uint64_t addr, const VideoOutImageLookupIdentity* identity)
{
	EXIT_IF(g_video_out_context == nullptr);

	auto result = g_video_out_context->FindImage(reinterpret_cast<void*>(addr), identity);
	if (result.lifecycle_trace_dump)
	{
		// The VideoOut mutex is released before the trace touches stderr.
		VideoOutDumpLifecycleTrace("registered_address_find_miss");
	}
	return result;
}

LUMEN_SYSV_ABI int VideoOutSubmitFlip(int handle, int index, int flip_mode, int64_t flip_arg)
{
	PRINT_NAME();
	if (g_video_out_context == nullptr) return VIDEO_OUT_ERROR_INVALID_HANDLE;
	return g_video_out_context->SubmitFlip(handle, index, flip_mode, flip_arg);
}

void VideoOutWaitFlipDone(int handle, int index)
{
	EXIT_IF(g_video_out_context == nullptr);

	auto* ctx = g_video_out_context->Get(handle);

	EXIT_NOT_IMPLEMENTED(index < 0 || index > 15);

	g_video_out_context->GetFlipQueue().Wait(ctx, index);
}

LUMEN_SYSV_ABI int VideoOutGetFlipStatus(int handle, VideoOutFlipStatus* status)
{
	PRINT_NAME();

	if (status == nullptr)
	{
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}
	VideoOutFlipStatus snapshot {};
	if (g_video_out_context == nullptr) return VIDEO_OUT_ERROR_INVALID_HANDLE;
	const int result = g_video_out_context->GetFlipStatus(handle, &snapshot);
	if (result != OK) return result;
	if (!write_guest_output(status, &snapshot, sizeof(snapshot)))
	{
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	printf("\t count = %" PRIu64 "\n", snapshot.count);
	printf("\t processTime = %" PRIu64 "\n", snapshot.processTime);
	printf("\t tsc = %" PRIu64 "\n", snapshot.tsc);
	printf("\t submitTsc = %" PRIu64 "\n", snapshot.submitTsc);
	printf("\t flipArg = %" PRId64 "\n", snapshot.flipArg);
	printf("\t gcQueueNum = %d\n", snapshot.gcQueueNum);
	printf("\t flipPendingNum = %d\n", snapshot.flipPendingNum);
	printf("\t currentBuffer = %d\n", snapshot.currentBuffer);

	return OK;
}

LUMEN_SYSV_ABI int VideoOutGetVblankStatus(int handle, VideoOutVblankStatus* status)
{
	PRINT_NAME();

	if (status == nullptr)
	{
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}
	VideoOutVblankStatus snapshot {};
	if (g_video_out_context == nullptr) return VIDEO_OUT_ERROR_INVALID_HANDLE;
	const int result = g_video_out_context->GetVblankStatus(handle, &snapshot);
	if (result != OK) return result;
	if (!write_guest_output(status, &snapshot, sizeof(snapshot)))
	{
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	printf("\t count = %" PRIu64 "\n", snapshot.count);
	printf("\t processTime = %" PRIu64 "\n", snapshot.processTime);
	printf("\t tsc = %" PRIu64 "\n", snapshot.tsc);

	return OK;
}

LUMEN_SYSV_ABI int VideoOutSetWindowModeMargins(int handle, int top, int bottom)
{
	PRINT_NAME();

	if (g_video_out_context == nullptr) return VIDEO_OUT_ERROR_INVALID_HANDLE;
	return g_video_out_context->SetWindowModeMargins(handle, top, bottom);
}

} // namespace Lumen::Libs::VideoOut

#endif // LUMEN_EMU_ENABLED
