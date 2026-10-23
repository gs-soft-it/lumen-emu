#include "Emulator/Graphics/Window.h"

#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/SafeDelete.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Subsystems.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Timer.h"
#include "Lumen/Core/Vector.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "CpuX64.h"
#include "Emulator/Controller.h"
#include "Emulator/Graphics/ControllerHotplug.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/Image.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/VertexBuffer.h"
#include "Emulator/Graphics/PresentationPacer.h"
#include "Emulator/Graphics/RenderTargetVulkanContract.h"
#include "Emulator/Graphics/SdrPresentationPolicy.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
#include "Emulator/Graphics/WindowIOS.h"
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
#include "Emulator/Loader/RuntimeLinker.h"
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#include "Emulator/Graphics/WindowAndroid.h"
#endif
#include "Emulator/Loader/SystemContent.h"
#include "Emulator/Libs/ImeDialogInput.h"
#include "Emulator/Profiler.h"
#include "Emulator/StartupTrace.h"
#include "../../../../android/app/src/main/cpp/CriticalPathTrace.h"

#include "lumen_config.h"

#include "SDL.h"
#include "SDL_error.h"
#include "SDL_events.h"
#include "SDL_gamecontroller.h"
#include "SDL_hints.h"
#include "SDL_joystick.h"
#include "SDL_keyboard.h"
#include "SDL_keycode.h"
#include "SDL_mouse.h"
#include "SDL_stdinc.h"
#include "SDL_surface.h"
#include "SDL_thread.h"
#include "SDL_touch.h"
#include "SDL_video.h"
#include "SDL_vulkan.h"
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
#include "SDL_syswm.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <limits>
#if defined(LUMEN_ANDROID_VULKAN_VALIDATION)
#include <mutex>
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
#include <pthread.h>
#endif
#include <string>
#include <thread>
#include <vulkan/vk_enum_string_helper.h>
#include <vulkan/vk_platform.h>

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
extern "C" bool LumenMetalFinalizeShaderFunctionCache()
    __attribute__((weak_import));
extern "C" bool LumenMetalFinalizeDepthStencilStateCache()
    __attribute__((weak_import));
extern "C" bool LumenMetalFinalizeStateOnlyRecordElision()
    __attribute__((weak_import));
extern "C" bool LumenMetalFinalizeCompletionPassReuse()
    __attribute__((weak_import));
#endif

// IWYU pragma: no_include <intrin.h>

#ifdef LUMEN_EMU_ENABLED

//#define LUMEN_ENABLE_BEST_PRACTICES
#define LUMEN_ENABLE_DEBUG_PRINTF
#define LUMEN_DBG_INPUT

namespace Lumen::Libs::Graphics {

constexpr float FPS_AVERAGE_FRAMES = 5.0f;
constexpr float FPS_UPDATE_TIME    = 0.25f;

struct EventKeyboard
{
	bool     down;
	bool     up;
	bool     pressed;
	bool     released;
	bool     repeat;
	int      scan_code;
	int      key_code;
	uint16_t mod;
	double   timestamp_seconds;
};

struct EventMouse
{
	bool   down;
	bool   up;
	bool   left;
	bool   middle;
	bool   right;
	bool   x1;
	bool   x2;
	bool   touch;
	bool   pressed;
	bool   released;
	int    num_of_clicks;
	bool   wheel;
	int    x;
	int    y;
	bool   motion;
	int    motion_x;
	int    motion_y;
	double timestamp_seconds;
};

struct EventFinger
{
	bool   down;
	bool   up;
	bool   motion;
	int    touch_id;
	int    finger_id;
	float  x;
	float  y;
	float  dx;
	float  dy;
	float  pressure;
	double timestamp_seconds;
};

struct EventController
{
	int    id;
	int    button;
	int    axis_id;
	int    axis_value;
	bool   down;
	bool   up;
	bool   added;
	bool   removed;
	bool   remapped;
	bool   axis;
	bool   pressed;
	bool   released;
	double timestamp_seconds;
};

enum class DisplayOrientation
{
	Unknown,          /* The display orientation can't be determined */
	Landscape,        /* The display is in landscape mode, with the right side up, relative to portrait mode */
	LandscapeFlipped, /* The display is in landscape mode, with the left side up, relative to portrait mode */
	Portrait,         /* The display is in portrait mode */
	PortraitFlipped,  /* The display is in portrait mode, upside down */

	DisplayEventOrientation = 0xF0
};

struct EventDisplay
{
	DisplayOrientation orientation;
};

constexpr uint32_t LUMEN_SDL_BUTTON_LMASK  = SDL_BUTTON_LMASK;  // NOLINT(hicpp-signed-bitwise)
constexpr uint32_t LUMEN_SDL_BUTTON_MMASK  = SDL_BUTTON_MMASK;  // NOLINT(hicpp-signed-bitwise)
constexpr uint32_t LUMEN_SDL_BUTTON_RMASK  = SDL_BUTTON_RMASK;  // NOLINT(hicpp-signed-bitwise)
constexpr uint32_t LUMEN_SDL_BUTTON_X1MASK = SDL_BUTTON_X1MASK; // NOLINT(hicpp-signed-bitwise)
constexpr uint32_t LUMEN_SDL_BUTTON_X2MASK = SDL_BUTTON_X2MASK; // NOLINT(hicpp-signed-bitwise)

// struct GraphicContext;

struct GameApi
{
	bool (*init)(GameApi* game, const Core::Timer& timer, void* data)            = nullptr;
	bool (*render_and_update)(GameApi* game, const Core::Timer& timer)           = nullptr;
	bool (*close)(GameApi* game)                                                 = nullptr;
	void (*event_quit)(GameApi* game)                                            = nullptr;
	void (*event_terminate)(GameApi* game)                                       = nullptr;
	void (*event_keyboard)(GameApi* game, const EventKeyboard* key)              = nullptr;
	void (*event_mouse)(GameApi* game, const EventMouse* mb)                     = nullptr;
	void (*event_finger)(GameApi* game, const EventFinger* f)                    = nullptr;
	void (*event_controller)(GameApi* game, const EventController* f)            = nullptr;
	void (*event_display)(GameApi* game, const EventDisplay* d)                  = nullptr;
	void (*event_low_memory)(GameApi* game)                                      = nullptr;
	void (*event_will_enter_background)(GameApi* game)                           = nullptr;
	void (*event_did_enter_background)(GameApi* game)                            = nullptr;
	void (*event_will_enter_foreground)(GameApi* game)                           = nullptr;
	void (*event_did_enter_foreground)(GameApi* game)                            = nullptr;
	void (*event_resize)(GameApi* game, uint32_t new_width, uint32_t new_height) = nullptr;
	void (*show_window)(GameApi* game, const Core::Timer& timer)                 = nullptr;
	bool (*need_exit)(GameApi* game)                                             = nullptr;
	bool (*is_paused)(GameApi* game)                                             = nullptr;

	int (*poll_event)(GameApi* game)                    = nullptr;
	int (*wait_event)(GameApi* game)                    = nullptr;
	void (*process_event)(GameApi* game, double time_s) = nullptr;

	void* data1 = nullptr;
	void* data2 = nullptr;

	bool     m_game_need_exit        = {false};
	bool     m_game_is_paused        = {false};
	uint32_t m_screen_width          = {0};
	uint32_t m_screen_height         = {0};
	double   m_current_time_seconds  = {0.0};
	double   m_previous_time_seconds = {0.0};
	int      m_update_num            = {0};
	int      m_frame_num             = {0};
	double   m_update_time_seconds   = {0.0};
	double   m_current_fps           = {0.0};
	int      m_max_updates_per_frame = {4};
	double   m_update_fixed_time     = 1.0 / 60.0;
	int      m_fps_frames_num        = {0};
	double   m_fps_start_time        = {0};
};

struct GameApiPrivateStruct
{
	GameApiPrivateStruct() = default;

	Core::Mutex     mutex;
	int             skip_frames = 0;
	GraphicContext* ctx         = nullptr;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	PresentationPacer presentation_pacer;
#endif
};

// void game_main_loop(GameApi* game);

GameApi* game_create_api();
void     game_delete_api(GameApi* api);
int      game_poll_event(GameApi* game);
int      game_wait_event(GameApi* game);
void     game_process_event(GameApi* game, double time_s);

struct VulkanExtensions
{
	bool enable_validation_layers = false;

	Vector<const char*>           required_extensions;
	Vector<VkExtensionProperties> available_extensions;
	Vector<const char*>           required_layers;
	Vector<VkLayerProperties>     available_layers;
};

struct SurfaceCapabilities
{
	VkSurfaceCapabilitiesKHR   capabilities {};
	Vector<VkSurfaceFormatKHR> formats;
	Vector<VkPresentModeKHR>   present_modes;
	bool                       format_srgb_bgra32  = false;
	bool                       format_unorm_bgra32 = false;
};

constexpr uint32_t SelectSwapchainImageCount(bool ios)
{
	// iOS cannot disable CAMetalLayer vsync. Keep one additional drawable
	// available so the CPU can prepare the next guest frame while Core
	// Animation retains earlier drawables for FIFO presentation.
	return ios ? 3u : 2u;
}

constexpr uint32_t PREFERRED_SWAPCHAIN_IMAGE_COUNT =
	SelectSwapchainImageCount(LUMEN_PLATFORM == LUMEN_PLATFORM_IOS);

constexpr bool VulkanResultRequiresSurfaceReplacement(VkResult result)
{
	return result == VK_ERROR_SURFACE_LOST_KHR;
}

constexpr bool ShouldRecreateSwapchainAfterPresent(
	bool android, bool acquire_suboptimal, VkResult present_result)
{
	if (present_result == VK_ERROR_OUT_OF_DATE_KHR)
	{
		return true;
	}
	if (android)
	{
		// VK_SUBOPTIMAL_KHR still permits presentation. Android surfaces can
		// report it persistently even when rebuilding produces the same extent
		// and image count; SURFACE_LOST recovery, drawable-size and OUT_OF_DATE
		// paths remain responsible for an actual replacement.
		return false;
	}
	return acquire_suboptimal || present_result == VK_SUBOPTIMAL_KHR;
}

constexpr bool ShouldSampleDiagnosticCounter(uint64_t count)
{
	return count <= 3 || (count & (count - 1)) == 0;
}

static bool WindowSwapchainTraceEnabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_FRAME_PIPELINE_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

constexpr VkSurfaceTransformFlagBitsKHR SelectSwapchainPreTransform(
	VkSurfaceTransformFlagsKHR supported_transforms,
	VkSurfaceTransformFlagBitsKHR current_transform,
	bool presentation_engine_handles_rotation)
{
	// The renderer records an axis-aligned blit and does not pre-rotate the
	// image. On Android, prefer an identity swapchain so the presentation
	// engine applies the device's current rotation.
	if (presentation_engine_handles_rotation &&
	    (supported_transforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0)
	{
		return VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	}
	return current_transform;
}

constexpr bool SelectGuestAspectFit(bool ios, bool android)
{
	// Mobile drawables follow the physical panel and can be wider than the
	// title-controlled VideoOut buffer. Keep the complete guest frame visible;
	// UtilBlitImage clears the unused area to opaque black.
	return ios || android;
}

struct WindowPresentationConfiguration
{
	bool     valid          = false;
	uint32_t logical_width  = 0;
	uint32_t logical_height = 0;
	bool     high_dpi       = false;
};

constexpr WindowPresentationConfiguration ResolveWindowPresentationConfiguration(
	uint32_t guest_width, uint32_t guest_height, uint32_t configured_window_width,
	uint32_t configured_window_height, bool high_dpi)
{
	const bool has_window_width  = configured_window_width != 0;
	const bool has_window_height = configured_window_height != 0;
	if (guest_width == 0 || guest_height == 0 || has_window_width != has_window_height)
	{
		return {};
	}

	const uint32_t logical_width  = has_window_width ? configured_window_width : guest_width;
	const uint32_t logical_height = has_window_height ? configured_window_height : guest_height;
	if (logical_width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
	    logical_height > static_cast<uint32_t>(std::numeric_limits<int>::max()))
	{
		return {};
	}

	return {true, logical_width, logical_height, high_dpi};
}

constexpr uint32_t SelectSdlWindowFlags(bool ios, bool macos, bool high_dpi)
{
	uint32_t flags = static_cast<uint32_t>(SDL_WINDOW_HIDDEN) |
	                 static_cast<uint32_t>(SDL_WINDOW_VULKAN);
	if (ios || (macos && high_dpi))
	{
		flags |= static_cast<uint32_t>(SDL_WINDOW_ALLOW_HIGHDPI);
	}
	return flags;
}

#if LUMEN_PLATFORM != LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
static bool SelectUncappedPresentMode(const VkPresentModeKHR* present_modes, uint32_t present_mode_count,
                                      VkPresentModeKHR* selected_present_mode)
{
	if (present_modes == nullptr || present_mode_count == 0 || selected_present_mode == nullptr)
	{
		return false;
	}

	for (uint32_t index = 0; index < present_mode_count; index++)
	{
		if (present_modes[index] == VK_PRESENT_MODE_IMMEDIATE_KHR)
		{
			*selected_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
			return true;
		}
	}
	for (uint32_t index = 0; index < present_mode_count; index++)
	{
		if (present_modes[index] == VK_PRESENT_MODE_MAILBOX_KHR)
		{
			*selected_present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
			return true;
		}
	}
	return false;
}
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
class IOSWindowLifecycle
{
public:
	enum class State
	{
		Uninitialized,
		Dormant,
		Presenting,
	};

	bool Initialize()
	{
		if (m_state != State::Uninitialized)
		{
			return false;
		}
		m_state = State::Dormant;
		return true;
	}

	bool BeginSession()
	{
		if (m_state != State::Dormant)
		{
			return false;
		}
		m_state = State::Presenting;
		return true;
	}

	bool FinishSession()
	{
		if (m_state != State::Presenting)
		{
			return false;
		}
		m_state = State::Dormant;
		return true;
	}

	[[nodiscard]] bool IsDormant() const { return m_state == State::Dormant; }

private:
	State m_state = State::Uninitialized;
};
#endif

struct WindowContext
{
	GraphicContext       graphic_ctx;
	VulkanSwapchain*     swapchain            = nullptr;
	SDL_Window*          window               = nullptr;
	bool                 window_hidden        = true;
	uint32_t             drawable_width       = 0;
	uint32_t             drawable_height      = 0;
	VkSurfaceKHR         surface              = nullptr;
	SurfaceCapabilities* surface_capabilities = nullptr;
	GameApi*             game                 = nullptr;

	char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {0};
	char processor_name[64]                            = {0};

	Core::Mutex   mutex;
	Core::Mutex   swapchain_mutex;
	bool          graphic_initialized            = false;
	bool          startup_first_present_reported = false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	std::atomic_bool android_surface_recovery_pending {false};
#endif
	Core::CondVar graphic_initialized_condvar;
	std::chrono::steady_clock::time_point last_title_update;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	IOSWindowLifecycle ios_lifecycle;
#endif
};

class DeferredStartupTraceFlush final
{
public:
	~DeferredStartupTraceFlush()
	{
		if (m_armed)
		{
			Lumen::Emulator::StartupTrace::Flush();
		}
	}

	void Arm() { m_armed = true; }

private:
	bool m_armed = false;
};

static WindowContext* g_window_ctx = nullptr;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
static bool MobileRuntimeLoopExitRequested(bool game_exit_requested, bool guest_stop_requested)
{
	return game_exit_requested || guest_stop_requested;
}
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
constexpr Uint64 CONTROLLER_RECONCILE_INTERVAL_MS  = 250;
static Uint64    g_next_controller_reconcile_ticks = 0;
#endif

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool WindowIOSRuntimeLifecycleStateTest()
{
	IOSWindowLifecycle lifecycle;
	if (!lifecycle.Initialize() || !lifecycle.IsDormant())
	{
		return false;
	}
	if (lifecycle.Initialize() || lifecycle.FinishSession())
	{
		return false;
	}
	if (!lifecycle.BeginSession() || lifecycle.BeginSession() || lifecycle.IsDormant())
	{
		return false;
	}
	if (!lifecycle.FinishSession() || !lifecycle.IsDormant() || lifecycle.FinishSession())
	{
		return false;
	}
	if (!lifecycle.BeginSession() || !lifecycle.FinishSession() || !lifecycle.IsDormant())
	{
		return false;
	}
	// A guest exit can be latched before WindowRun flushes stale SDL events, or
	// SDL_PushEvent itself may fail. The presentation loop must still close.
	return !MobileRuntimeLoopExitRequested(false, false) && MobileRuntimeLoopExitRequested(true, false) &&
	       MobileRuntimeLoopExitRequested(false, true);
}

bool WindowUncappedPresentModeSelectionTest()
{
	constexpr VkPresentModeKHR all_modes[] = {
	    VK_PRESENT_MODE_FIFO_KHR,
	    VK_PRESENT_MODE_MAILBOX_KHR,
	    VK_PRESENT_MODE_IMMEDIATE_KHR,
	};
	constexpr VkPresentModeKHR mailbox_modes[] = {
	    VK_PRESENT_MODE_FIFO_KHR,
	    VK_PRESENT_MODE_MAILBOX_KHR,
	};
	constexpr VkPresentModeKHR fifo_only[] = {VK_PRESENT_MODE_FIFO_KHR};

	VkPresentModeKHR selected = VK_PRESENT_MODE_FIFO_KHR;
	if (!SelectUncappedPresentMode(all_modes, static_cast<uint32_t>(std::size(all_modes)), &selected) ||
	    selected != VK_PRESENT_MODE_IMMEDIATE_KHR)
	{
		return false;
	}
	if (!SelectUncappedPresentMode(mailbox_modes, static_cast<uint32_t>(std::size(mailbox_modes)), &selected) ||
	    selected != VK_PRESENT_MODE_MAILBOX_KHR)
	{
		return false;
	}
	return !SelectUncappedPresentMode(fifo_only, static_cast<uint32_t>(std::size(fifo_only)), &selected) &&
	       !SelectUncappedPresentMode(nullptr, 0, &selected);
}

bool WindowSwapchainImageCountSelectionTest()
{
	return SelectSwapchainImageCount(false) == 2 &&
	       SelectSwapchainImageCount(true) == 3 &&
	       PREFERRED_SWAPCHAIN_IMAGE_COUNT ==
	           (LUMEN_PLATFORM == LUMEN_PLATFORM_IOS ? 3u : 2u);
}

bool WindowSwapchainRecreationPolicyTest()
{
	return !ShouldRecreateSwapchainAfterPresent(true, false, VK_SUCCESS) &&
	       !ShouldRecreateSwapchainAfterPresent(true, true, VK_SUCCESS) &&
	       !ShouldRecreateSwapchainAfterPresent(true, false,
	                                            VK_SUBOPTIMAL_KHR) &&
	       !ShouldRecreateSwapchainAfterPresent(true, true,
	                                            VK_SUBOPTIMAL_KHR) &&
	       ShouldRecreateSwapchainAfterPresent(
	           true, false, VK_ERROR_OUT_OF_DATE_KHR) &&
	       ShouldRecreateSwapchainAfterPresent(false, true, VK_SUCCESS) &&
	       ShouldRecreateSwapchainAfterPresent(false, false,
	                                            VK_SUBOPTIMAL_KHR) &&
	       ShouldRecreateSwapchainAfterPresent(
	           false, false, VK_ERROR_OUT_OF_DATE_KHR);
}

bool WindowSwapchainPreTransformSelectionTest()
{
	constexpr auto identity = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	constexpr auto rotate90 = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR;
	return SelectSwapchainPreTransform(identity | rotate90, rotate90, true) == identity &&
	       SelectSwapchainPreTransform(rotate90, rotate90, true) == rotate90 &&
	       SelectSwapchainPreTransform(identity | rotate90, rotate90, false) == rotate90 &&
	       SelectSwapchainPreTransform(identity, identity, true) == identity;
}

bool WindowSurfaceReplacementResultPolicyTest()
{
	return VulkanResultRequiresSurfaceReplacement(VK_ERROR_SURFACE_LOST_KHR) &&
	       !VulkanResultRequiresSurfaceReplacement(VK_SUCCESS) &&
	       !VulkanResultRequiresSurfaceReplacement(VK_SUBOPTIMAL_KHR) &&
	       !VulkanResultRequiresSurfaceReplacement(VK_ERROR_OUT_OF_DATE_KHR) &&
	       !VulkanResultRequiresSurfaceReplacement(VK_ERROR_DEVICE_LOST);
}

bool WindowGuestPresentationLayoutTest()
{
	if (!SelectGuestAspectFit(true, false) || !SelectGuestAspectFit(false, true) ||
	    !SelectGuestAspectFit(true, true) || SelectGuestAspectFit(false, false))
	{
		return false;
	}

	const auto ultrawide = UtilComputeBlitDestinationRect(1920, 1080, 2400, 1080, true);
	const auto tall      = UtilComputeBlitDestinationRect(1920, 1080, 1920, 1200, true);
	const auto exact     = UtilComputeBlitDestinationRect(1920, 1080, 1920, 1080, true);
	const auto stretched = UtilComputeBlitDestinationRect(1920, 1080, 2400, 1080, false);
	return ultrawide.x == 240 && ultrawide.y == 0 && ultrawide.width == 1920 && ultrawide.height == 1080 &&
	       tall.x == 0 && tall.y == 60 && tall.width == 1920 && tall.height == 1080 &&
	       exact.x == 0 && exact.y == 0 && exact.width == 1920 && exact.height == 1080 &&
	       stretched.x == 0 && stretched.y == 0 && stretched.width == 2400 && stretched.height == 1080;
}

bool WindowPresentationConfigurationTest()
{
	constexpr auto legacy = ResolveWindowPresentationConfiguration(1920, 1080, 0, 0, false);
	constexpr auto retina = ResolveWindowPresentationConfiguration(1920, 1080, 960, 540, true);
	constexpr auto partial_width = ResolveWindowPresentationConfiguration(1920, 1080, 960, 0, true);
	constexpr auto partial_height = ResolveWindowPresentationConfiguration(1920, 1080, 0, 540, true);
	constexpr auto overflow = ResolveWindowPresentationConfiguration(
	    1920, 1080, static_cast<uint32_t>(std::numeric_limits<int>::max()) + 1u, 540, true);
	constexpr uint32_t default_macos_flags = SelectSdlWindowFlags(false, true, false);
	constexpr uint32_t retina_macos_flags  = SelectSdlWindowFlags(false, true, true);
	constexpr uint32_t ios_flags           = SelectSdlWindowFlags(true, false, false);
	constexpr uint32_t windows_flags       = SelectSdlWindowFlags(false, false, true);
	constexpr uint32_t high_dpi_flag       = static_cast<uint32_t>(SDL_WINDOW_ALLOW_HIGHDPI);

	return legacy.valid && legacy.logical_width == 1920 && legacy.logical_height == 1080 && !legacy.high_dpi &&
	       retina.valid && retina.logical_width == 960 && retina.logical_height == 540 && retina.high_dpi &&
	       !partial_width.valid && !partial_height.valid && !overflow.valid &&
	       (default_macos_flags & high_dpi_flag) == 0 &&
	       (retina_macos_flags & high_dpi_flag) != 0 &&
	       (ios_flags & high_dpi_flag) != 0 && (windows_flags & high_dpi_flag) == 0;
}
#endif

constexpr const char* LUMEN_SDL_WINDOW_CAPTION = "Lumen emu";
// constexpr uint32_t    LUMEN_SDL_WINDOW_FLAGS       = (static_cast<uint32_t>(SDL_WINDOW_HIDDEN) |
// static_cast<uint32_t>(SDL_WINDOW_OPENGL));
constexpr int      LUMEN_SDL_WINDOWPOS_CENTERED = SDL_WINDOWPOS_CENTERED; /*NOLINT(hicpp-signed-bitwise)*/

static int game_sdl_event_filter(void* /*userdata*/, SDL_Event* /*event*/)
{
	//	game_api_private_t *p = (game_api_private_t*)userdata;
	//
	//	if (event->type == SDL_WINDOWEVENT)
	//	{
	//		if (event->window.event == SDL_WINDOWEVENT_RESIZED || event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
	//		{
	//			p->mutex.Lock();
	//			{
	//				//p->resize_events_num++;
	//				//printf("game_sdl_event_filter\n");
	//				p->skip_frames++;
	//			}
	//			p->mutex.Unlock();
	//		}
	//	}

	return 0;
}

static void CalcFrameTime(GameApi* game, double game_time_s)
{
	game->m_previous_time_seconds = game->m_current_time_seconds;
	game->m_current_time_seconds  = game_time_s;

	game->m_frame_num++;

	int fps_model = 1;

	if (fps_model == 1)
	{
		game->m_fps_frames_num++;
		if (game->m_current_time_seconds - game->m_fps_start_time > FPS_UPDATE_TIME)
		{
			game->m_current_fps    = static_cast<double>(game->m_fps_frames_num) / (game->m_current_time_seconds - game->m_fps_start_time);
			game->m_fps_frames_num = 0;
			game->m_fps_start_time = game->m_current_time_seconds;
		}
	} else
	{
		game->m_current_fps = (1.0f / (game->m_current_time_seconds - game->m_previous_time_seconds)) * (1.0f / FPS_AVERAGE_FRAMES) +
		                      game->m_current_fps * (1.0f - (1.0f / FPS_AVERAGE_FRAMES));
	}
}

static bool Init(GameApi* /*game*/)
{
	return true;
}
static bool Update(GameApi* /*game*/)
{
	return true;
}
static bool Render(GameApi* /*game*/)
{
	return true;
}
static bool Close(GameApi* /*game*/)
{
	return true;
}
static void SetPause(GameApi* game, bool flag)
{
	printf("Pause: %s\n", flag ? "true" : "false");

	game->m_game_is_paused = flag;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (auto* p = static_cast<GameApiPrivateStruct*>(game->data1); p != nullptr) p->presentation_pacer.Reset();
#endif
}

static bool RenderAndUpdate(GameApi* game)
{
	static double lag = 0.0;

	lag += game->m_current_time_seconds - game->m_previous_time_seconds;

	int num = 0;

	bool ok = true;

	while (lag >= game->m_update_fixed_time)
	{
		if (num < game->m_max_updates_per_frame)
		{
			ok = ok && Update(game);

			game->m_update_num++;
			num++;
			game->m_update_time_seconds = game->m_update_num * game->m_update_fixed_time;
		}

		lag -= game->m_update_fixed_time;
	}

	ok = ok && Render(game);

	return ok;
}

bool game_init(GameApi* game, const Core::Timer& timer, void* data)
{
	EXIT_IF(game == nullptr);
	EXIT_IF(data == nullptr);
	EXIT_IF(game->data1 || game->data2);

	auto* ctx = static_cast<GraphicContext*>(data);

	EXIT_IF(ctx->screen_width == 0 || ctx->screen_height == 0);

	auto* pdata = new GameApiPrivateStruct;
	pdata->ctx  = ctx;

	game->data1 = pdata;
	game->data2 = new SDL_Event;

	SDL_AddEventWatch(game_sdl_event_filter, game->data1);
	Dialog::ImeDialog::InitSdlInput();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	std::fprintf(stderr, "[FRAME_RATE_LIMIT] hz=60 domain=completed-flip-cycle clock=steady catch_up=0\n");
#endif

	game->m_screen_width  = ctx->screen_width;
	game->m_screen_height = ctx->screen_height;

	// SDL_ShowWindow(ctx->window);

	CalcFrameTime(game, timer.GetTimeS());

	return Init(game);
}

bool game_render_and_update(GameApi* game, const Core::Timer& /*timer*/)
{
	return RenderAndUpdate(game);
}

bool game_close(GameApi* game)
{
	EXIT_IF(!game);

	EXIT_IF(!game->data1 || !game->data2);

	SDL_DelEventWatch(game_sdl_event_filter, game->data1);
	Dialog::ImeDialog::CloseSdlInput();
	delete (static_cast<GameApiPrivateStruct*>(game->data1));
	delete (static_cast<SDL_Event*>(game->data2));
	game->data1 = nullptr;
	game->data2 = nullptr;

	return Close(game);
}

struct WindowFlipWaitContext
{
	GameApi* game;
	const Core::Timer* timer;
};

static bool ServiceWindowEventsDuringFlipWait(void* userdata)
{
	EXIT_IF(!Core::Thread::IsMainThread());
	auto* context = static_cast<WindowFlipWaitContext*>(userdata);
	EXIT_IF(context == nullptr || context->game == nullptr || context->timer == nullptr);
	auto* game = context->game;
	auto may_present = [game]()
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		if (Loader::GuestExecutionStopRequested()) return false;
#endif
		return !game->need_exit(game) && !game->is_paused(game);
	};
	if (!may_present()) return false;
	// Bound a single service pass without discarding queued events. All events
	// use the same dispatch path and ordering as the ordinary outer loop.
	for (uint32_t count = 0; count < 64 && game->poll_event(game) != 0; ++count)
	{
		const auto* event = static_cast<const SDL_Event*>(game->data2);
		const bool resized = event->type == SDL_WINDOWEVENT &&
		                     (event->window.event == SDL_WINDOWEVENT_RESIZED || event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED);
		if (game->process_event != nullptr) game->process_event(game, context->timer->GetTimeS());
		// Resize/lifecycle work is processed before any flip is claimed or Vulkan
		// presentation state is entered. Let the outer loop handle the new state.
		if (resized || !may_present()) return false;
	}
	return true;
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool WindowFlipWaitEventServiceTest()
{
	Core::Timer timer;
	timer.Start();
	for (int interrupt = 0; interrupt < 5; ++interrupt)
	{
		struct Events
		{
			int interrupt;
			uint32_t next = 0;
			uint32_t processed = 0;
			bool ordered = true;
		} events {interrupt};
		GameApi game;
		SDL_Event event {};
		game.data1 = &events;
		game.data2 = &event;
		game.need_exit = +[](GameApi* api) { return api->m_game_need_exit; };
		game.is_paused = +[](GameApi* api) { return api->m_game_is_paused; };
		game.poll_event = +[](GameApi* api)
		{
			auto& input = *static_cast<Events*>(api->data1);
			if (input.next == 65) return 0;
			auto& out = *static_cast<SDL_Event*>(api->data2);
			out = {};
			out.type = SDL_USEREVENT;
			out.user.code = static_cast<int>(input.next);
			if (input.next == 1 && input.interrupt >= 3)
			{
				out.type = SDL_WINDOWEVENT;
				out.window.event = input.interrupt == 3 ? SDL_WINDOWEVENT_RESIZED : SDL_WINDOWEVENT_SIZE_CHANGED;
			}
			++input.next;
			return 1;
		};
		game.process_event = +[](GameApi* api, double)
		{
			auto& input = *static_cast<Events*>(api->data1);
			const auto& in = *static_cast<const SDL_Event*>(api->data2);
			input.ordered = input.ordered && input.next == input.processed + 1u &&
			                (in.type != SDL_USEREVENT || in.user.code == static_cast<int>(input.processed));
			if (input.processed == 1)
			{
				api->m_game_need_exit = input.interrupt == 1;
				api->m_game_is_paused = input.interrupt == 2;
			}
			++input.processed;
		};
		WindowFlipWaitContext context {&game, &timer};
		const bool keep_waiting = ServiceWindowEventsDuringFlipWait(&context);
		if (interrupt == 0)
		{
			if (!keep_waiting || events.processed != 64 || !ServiceWindowEventsDuringFlipWait(&context) ||
			    events.processed != 65) return false;
		}
		else if (keep_waiting || events.processed != 2) return false;
		if (!events.ordered || events.next != events.processed) return false;
		if ((interrupt == 1 || interrupt == 2) &&
		    (ServiceWindowEventsDuringFlipWait(&context) || events.processed != 2)) return false;
	}
	return true;
}
#endif

void game_show_window(GameApi* game, const Core::Timer& timer)
{
	EXIT_IF(!game);

	auto* p = static_cast<GameApiPrivateStruct*>(game->data1);

	EXIT_IF(!p);

#if ((LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID) && \
     LUMEN_ABI == LUMEN_ABI_ARM64_V8A) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	constexpr bool service_events = true;
#else
	constexpr bool service_events = false;
#endif
	p->mutex.Lock();
	if (p->skip_frames > 0)
	{
		p->skip_frames--;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		p->presentation_pacer.Reset();
#endif
		printf("skip frame %d\n", p->skip_frames);
		p->mutex.Unlock();
		return;
	}
	if (service_events) p->mutex.Unlock();
	VideoOut::VideoOutBeginVblank();
	// Synchronous callback only: game and timer stay owned by the main loop,
	// and this stack context cannot survive the call or reach game_close.
	WindowFlipWaitContext context {game, &timer};
	const bool flipped = VideoOut::VideoOutFlipWindow(
	    100000, service_events ? ServiceWindowEventsDuringFlipWait : nullptr, service_events ? &context : nullptr);
	VideoOut::VideoOutEndVblank();
	if (!service_events) p->mutex.Unlock();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (flipped)
	{
		// The flip, its completion callbacks and vblank have returned: no CP
		// epoch, guest lease, queue or swapchain lock crosses this native wait.
		// Rendering/request/VSync time already consumed counts toward the period.
		(void)p->presentation_pacer.CompleteFrame(
		    [] { return PresentationPacer::Clock::now(); },
		    [](auto deadline) { std::this_thread::sleep_until(deadline); },
		    [&] { return ServiceWindowEventsDuringFlipWait(&context); });
	} else
	{
		p->presentation_pacer.Reset();
	}
#endif
	if (flipped) CalcFrameTime(game, timer.GetTimeS());
}

void game_event_quit(GameApi* game)
{
	printf("Event: quit\n");
	std::fprintf(stderr, "[WINDOW_EXIT] SDL_QUIT\n");
	std::fflush(stderr);

	game->m_game_need_exit = true;
}

void game_event_terminate(GameApi* game)
{
	printf("Event: terminate\n");
	std::fprintf(stderr, "[WINDOW_EXIT] SDL_APP_TERMINATING\n");
	std::fflush(stderr);

	game->m_game_need_exit = true;
}

void game_event_keyboard(GameApi* game, const EventKeyboard* key)
{
#ifdef LUMEN_DBG_INPUT
	printf("Key: time = %.04f, %s%s, %s%s, %s, scan = %d, key = %d, mod = %04" PRIx16 "\n", key->timestamp_seconds,
	       (key->down ? "down" : ""), (key->up ? "up" : ""), (key->pressed ? "pressed" : ""), (key->released ? "released" : ""),
	       (key->repeat ? "repeat" : ""), key->scan_code, key->key_code, key->mod);
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS || LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX
	if (key->down && key->key_code == SDLK_ESCAPE)
	{
		game->m_game_need_exit = true;
	}

	if (key->down && key->key_code == SDLK_SPACE)
	{
		SetPause(game, !game->m_game_is_paused);
	}
#endif

	if (!key->repeat)
	{
		uint32_t button = 0;
		switch (key->key_code)
		{
			case SDLK_RETURN: button = Controller::PAD_BUTTON_CROSS; break;
			case SDLK_TAB: button = Controller::PAD_BUTTON_OPTIONS; break;
			case SDLK_BACKSPACE: button = Controller::PAD_BUTTON_TOUCH_PAD; break;
			case SDLK_UP: button = Controller::PAD_BUTTON_UP; break;
			case SDLK_RIGHT: button = Controller::PAD_BUTTON_RIGHT; break;
			case SDLK_DOWN: button = Controller::PAD_BUTTON_DOWN; break;
			case SDLK_LEFT: button = Controller::PAD_BUTTON_LEFT; break;
			case SDLK_1: button = Controller::PAD_BUTTON_L2; break;
			case SDLK_2: button = Controller::PAD_BUTTON_R2; break;
			case SDLK_q: button = Controller::PAD_BUTTON_L1; break;
			case SDLK_w: button = Controller::PAD_BUTTON_R1; break;
			case SDLK_s: button = Controller::PAD_BUTTON_TRIANGLE; break;
			case SDLK_x: button = Controller::PAD_BUTTON_CIRCLE; break;
			case SDLK_z: button = Controller::PAD_BUTTON_CROSS; break;
			case SDLK_a: button = Controller::PAD_BUTTON_SQUARE; break;
			default: break;
		}
		if (button != 0)
		{
			Controller::ControllerButton(Controller::VIRTUAL_CONTROLLER_ID, button, key->down);
		}
	}
}

void game_event_mouse([[maybe_unused]] GameApi* game, [[maybe_unused]] const EventMouse* mb)
{
#ifdef LUMEN_DBG_INPUT
	if (mb->wheel)
	{
		printf("Mouse wheel: time = %.04f, %s[%d, %d]\n", mb->timestamp_seconds, (mb->touch ? "touch, " : ""), mb->x, mb->y);
	} else if (mb->motion)
	{
		printf("Mouse motion: time = %.04f, %s%s%s%s%s%s, [%d, %d], (%d, %d)\n", mb->timestamp_seconds, (mb->left ? "left" : ""),
		       (mb->middle ? "middle" : ""), (mb->right ? "right" : ""), (mb->x1 ? "x1" : ""), (mb->x2 ? "x2" : ""),
		       (mb->touch ? "_touch" : ""), mb->x, mb->y, mb->motion_x, mb->motion_y);
	} else
	{
		printf("Mouse click: time = %.04f, %d, %s%s%s%s%s%s, %s%s, %s%s, [%d, %d]\n", mb->timestamp_seconds, mb->num_of_clicks,
		       (mb->left ? "left" : ""), (mb->middle ? "middle" : ""), (mb->right ? "right" : ""), (mb->x1 ? "x1" : ""),
		       (mb->x2 ? "x2" : ""), (mb->touch ? "_touch" : ""), (mb->down ? "down" : ""), (mb->up ? "up" : ""),
		       (mb->pressed ? "pressed" : ""), (mb->released ? "released" : ""), mb->x, mb->y);
	}
#endif
}

void game_event_finger([[maybe_unused]] GameApi* game, [[maybe_unused]] const EventFinger* f)
{
#ifdef LUMEN_DBG_INPUT
	if (f->motion)
	{
		printf("Finger motion: time = %.04f, %d, %d, (x,y) = [%f, %f], (dx,dy) = [%f, %f], pressure = %f\n", f->timestamp_seconds,
		       f->touch_id, f->finger_id, f->x, f->y, f->dx, f->dy, f->pressure);
	} else
	{
		printf("Finger press: time = %.04f, %d, %d, %s%s, (x,y) = [%f, %f], (dx,dy) = [%f, %f], pressure = %f\n", f->timestamp_seconds,
		       f->touch_id, f->finger_id, (f->down ? "down" : ""), (f->up ? "up" : ""), f->x, f->y, f->dx, f->dy, f->pressure);
	}
#endif
}

// static int controller_get_axis(int min, int max, int value)
//{
//	int v = (255 * (value - min)) / (max - min);
//	return (v < 0 ? 0 : (v > 255 ? 255 : v));
//}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
static bool g_android_physical_controller_connected = false;
static bool HasOpenedAndroidGameController();
static void RefreshAndroidPhysicalControllerState(bool notify_on_connect);
#endif

static void OnGameControllerOpened(const OpenedGameController& opened, void* /*context*/)
{
	Controller::ControllerConnect(opened.instance_id);
	printf("LUMEN_CONTROLLER_DEVICE phase=open source=%s index=%d instance=%d controller=%s\n", GameControllerOpenOriginName(opened.origin),
	       opened.device_index, opened.instance_id,
	       SDL_GameControllerName(opened.controller) != nullptr ? SDL_GameControllerName(opened.controller) : "unknown");
	std::fflush(stdout);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	WindowIOSRefreshVirtualControllerAvailability();
	if (GameControllerOpenShouldNotify(opened.origin))
	{
		WindowIOSControllerDidConnect();
	}
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (SDL_GameControllerGetAttached(opened.controller) == SDL_TRUE)
	{
		RefreshAndroidPhysicalControllerState(GameControllerOpenShouldNotify(opened.origin));
	}
#endif
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
static bool HasOpenedAndroidGameController()
{
	if ((SDL_WasInit(SDL_INIT_GAMECONTROLLER) & SDL_INIT_GAMECONTROLLER) == 0)
	{
		return false;
	}
	const int joystick_count = SDL_NumJoysticks();
	for (int device_index = 0; device_index < joystick_count; ++device_index)
	{
		if (SDL_IsGameController(device_index) != SDL_TRUE)
		{
			continue;
		}
		const SDL_JoystickID instance_id = SDL_JoystickGetDeviceInstanceID(device_index);
		SDL_GameController* controller = instance_id >= 0 ? SDL_GameControllerFromInstanceID(instance_id) : nullptr;
		if (controller != nullptr && SDL_GameControllerGetAttached(controller) == SDL_TRUE)
		{
			return true;
		}
	}
	return false;
}

static void RefreshAndroidPhysicalControllerState(bool notify_on_connect)
{
	const bool connected = HasOpenedAndroidGameController();
	if (connected == g_android_physical_controller_connected)
	{
		return;
	}
	g_android_physical_controller_connected = connected;
	WindowAndroidControllerStateChanged(connected, connected && notify_on_connect);
}
#endif

static GameControllerOpenResult OpenGameController(int device_index, GameControllerOpenOrigin origin)
{
	const char* name = SDL_JoystickNameForIndex(device_index);
	char        guid[33] {};
	SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(device_index), guid, sizeof(guid));
	const SDL_bool supported   = SDL_IsGameController(device_index);
	const int      instance_id = SDL_JoystickGetDeviceInstanceID(device_index);
	printf("LUMEN_CONTROLLER_DEVICE phase=scan source=%s index=%d instance=%d supported=%d name=%s guid=%s\n",
	       GameControllerOpenOriginName(origin), device_index, instance_id, supported == SDL_TRUE ? 1 : 0,
	       name != nullptr ? name : "unknown", guid);

	const auto result = EnsureGameControllerOpen(device_index, origin, OnGameControllerOpened, nullptr);
	switch (result.status)
	{
		case GameControllerOpenStatus::NotGameController:
			printf("LUMEN_CONTROLLER_DEVICE phase=ignored index=%d reason=not-game-controller\n", device_index);
			break;
		case GameControllerOpenStatus::InvalidInstance:
			printf("LUMEN_CONTROLLER_DEVICE phase=ignored index=%d reason=invalid-instance sdl_error=%s\n", device_index, SDL_GetError());
			break;
		case GameControllerOpenStatus::AlreadyOpen:
			printf("LUMEN_CONTROLLER_DEVICE phase=already-open index=%d instance=%d\n", device_index, result.instance_id);
			break;
		case GameControllerOpenStatus::OpenFailed:
			printf("LUMEN_CONTROLLER_DEVICE phase=open-failed index=%d instance=%d sdl_error=%s\n", device_index, result.instance_id,
			       SDL_GetError());
			break;
		case GameControllerOpenStatus::Opened: break;
	}
	std::fflush(stdout);
	return result;
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
static void ReconcileRuntimeGameControllers()
{
	const Uint64 now = SDL_GetTicks64();
	if (g_next_controller_reconcile_ticks != 0 && now < g_next_controller_reconcile_ticks)
	{
		return;
	}
	g_next_controller_reconcile_ticks = now + CONTROLLER_RECONCILE_INTERVAL_MS;

	const int opened = ReconcileUnopenedGameControllers(GameControllerOpenOrigin::RuntimeFallback, OnGameControllerOpened, nullptr);
	if (opened > 0)
	{
		printf("LUMEN_CONTROLLER_SCAN phase=runtime-reconcile opened=%d count=%d\n", opened, SDL_NumJoysticks());
		std::fflush(stdout);
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	RefreshAndroidPhysicalControllerState(true);
#endif
}
#endif

void game_event_controller([[maybe_unused]] GameApi* game, [[maybe_unused]] const EventController* f)
{
	if (f->remapped)
	{
		printf("Controller remapped: %d, time = %.04f\n", f->id, f->timestamp_seconds);
		std::fflush(stdout);
		return;
	}

#ifdef LUMEN_DBG_INPUT
	if (f->added || f->removed)
	{
		printf("Controller %s: %d, time = %.04f\n", (f->added ? "added" : "removed"), f->id, f->timestamp_seconds);
	} else if (f->axis)
	{
		printf("Controller axis: %d, axis = %d, value = %d, time = %.04f\n", f->id, f->axis_id, f->axis_value, f->timestamp_seconds);
	} else
	{
		printf("Controller button: "
		       "%d, %s%s, %s%s, button = %d, time = %.04f\n",
		       f->id, (f->down ? "down" : ""), (f->up ? "up" : ""), (f->pressed ? "pressed" : ""), (f->released ? "released" : ""),
		       f->button, f->timestamp_seconds);
	}
#endif

	if (f->added)
	{
		OpenGameController(f->id, GameControllerOpenOrigin::DeviceAddedEvent);
	}

	if (f->removed)
	{
		Controller::ControllerDisconnect(f->id);
		SDL_GameControllerClose(SDL_GameControllerFromInstanceID(f->id));
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		WindowIOSRefreshVirtualControllerAvailability();
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		RefreshAndroidPhysicalControllerState(false);
#endif
	}

	if (f->down || f->up)
	{
		uint32_t button = 0;
		switch (f->button)
		{
			case SDL_CONTROLLER_BUTTON_A: button = Controller::PAD_BUTTON_CROSS; break;
			case SDL_CONTROLLER_BUTTON_B: button = Controller::PAD_BUTTON_CIRCLE; break;
			case SDL_CONTROLLER_BUTTON_X: button = Controller::PAD_BUTTON_SQUARE; break;
			case SDL_CONTROLLER_BUTTON_Y: button = Controller::PAD_BUTTON_TRIANGLE; break;
			case SDL_CONTROLLER_BUTTON_BACK: button = Controller::PAD_BUTTON_TOUCH_PAD; break;
			case SDL_CONTROLLER_BUTTON_GUIDE: break;
			case SDL_CONTROLLER_BUTTON_START: button = Controller::PAD_BUTTON_OPTIONS; break;
			case SDL_CONTROLLER_BUTTON_LEFTSTICK: button = Controller::PAD_BUTTON_L3; break;
			case SDL_CONTROLLER_BUTTON_RIGHTSTICK: button = Controller::PAD_BUTTON_R3; break;
			case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: button = Controller::PAD_BUTTON_L1; break;
			case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: button = Controller::PAD_BUTTON_R1; break;
			case SDL_CONTROLLER_BUTTON_DPAD_UP: button = Controller::PAD_BUTTON_UP; break;
			case SDL_CONTROLLER_BUTTON_DPAD_DOWN: button = Controller::PAD_BUTTON_DOWN; break;
			case SDL_CONTROLLER_BUTTON_DPAD_LEFT: button = Controller::PAD_BUTTON_LEFT; break;
			case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: button = Controller::PAD_BUTTON_RIGHT; break;
			default: break;
		}
		if (button != 0)
		{
			Controller::ControllerButton(f->id, button, f->down);
		}
	}

	if (f->axis)
	{
		int value = -1;
		if (f->axis_id == SDL_CONTROLLER_AXIS_TRIGGERLEFT || f->axis_id == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
		{
			value = Controller::controller_get_axis(0, SDL_JOYSTICK_AXIS_MAX, f->axis_value);
		} else
		{
			value = Controller::controller_get_axis(SDL_JOYSTICK_AXIS_MIN, SDL_JOYSTICK_AXIS_MAX, f->axis_value);
		}

		Controller::Axis axis = Controller::Axis::AxisMax;
		switch (f->axis_id)
		{
			case SDL_CONTROLLER_AXIS_LEFTX: axis = Controller::Axis::LeftX; break;
			case SDL_CONTROLLER_AXIS_LEFTY: axis = Controller::Axis::LeftY; break;
			case SDL_CONTROLLER_AXIS_RIGHTX: axis = Controller::Axis::RightX; break;
			case SDL_CONTROLLER_AXIS_RIGHTY: axis = Controller::Axis::RightY; break;
			case SDL_CONTROLLER_AXIS_TRIGGERLEFT: axis = Controller::Axis::TriggerLeft; break;
			case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: axis = Controller::Axis::TriggerRight; break;
		}

		if (axis != Controller::Axis::AxisMax)
		{
			Controller::ControllerAxis(f->id, axis, value);
		}
	}
}

void game_event_display([[maybe_unused]] GameApi* game, [[maybe_unused]] const EventDisplay* d)
{
	auto* p   = static_cast<GameApiPrivateStruct*>(game->data1);
	auto* ctx = static_cast<GraphicContext*>(p->ctx);

	p->mutex.Lock();
	game->m_screen_width  = ctx->screen_width;
	game->m_screen_height = ctx->screen_height;
	p->mutex.Unlock();
}

void game_event_low_memory(GameApi* /*game*/)
{
	printf("Event: low_memory\n");
}

void game_event_will_enter_background(GameApi* game)
{
	printf("Event: will_enter_background\n");

	SetPause(game, true);
}

void game_event_did_enter_background(GameApi* /*game*/)
{
	printf("Event: did_enter_background\n");
}

void game_event_will_enter_foreground(GameApi* /*game*/)
{
	printf("Event: will_enter_foreground\n");
}

void game_event_did_enter_foreground(GameApi* game)
{
	printf("Event: did_enter_foreground\n");

	SetPause(game, false);
}

bool game_need_exit(GameApi* game)
{
	return game->m_game_need_exit;
}

bool game_is_paused(GameApi* game)
{
	return game->m_game_is_paused;
}

void game_event_resize(GameApi* game, uint32_t new_width, uint32_t new_height)
{
	EXIT_IF(!game);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (auto* p = static_cast<GameApiPrivateStruct*>(game->data1); p != nullptr) p->presentation_pacer.Reset();
#endif
	if (new_width == 0 || new_height == 0)
	{
		Core::LockGuard lock(g_window_ctx->mutex);
		g_window_ctx->drawable_width  = 0;
		g_window_ctx->drawable_height = 0;
		return;
	}

	auto* p = static_cast<GameApiPrivateStruct*>(game->data1);
	EXIT_IF(p == nullptr);

	auto* ctx = static_cast<GraphicContext*>(p->ctx);
	EXIT_IF(ctx == nullptr);

	p->mutex.Lock();
	{
		p->skip_frames++;
#if LUMEN_PLATFORM != LUMEN_PLATFORM_IOS
		ctx->screen_width  = new_width;
		ctx->screen_height = new_height;
#endif

		game->m_screen_width  = ctx->screen_width;
		game->m_screen_height = ctx->screen_height;
	}
	p->mutex.Unlock();

	int drawable_width  = 0;
	int drawable_height = 0;
	SDL_Vulkan_GetDrawableSize(g_window_ctx->window, &drawable_width, &drawable_height);
	if (drawable_width > 0 && drawable_height > 0)
	{
		Core::LockGuard lock(g_window_ctx->mutex);
		g_window_ctx->drawable_width  = static_cast<uint32_t>(drawable_width);
		g_window_ctx->drawable_height = static_cast<uint32_t>(drawable_height);
	}
}

static void process_window_event(GameApi* game, SDL_WindowEvent window)
{
	switch (window.event)
	{
		case SDL_WINDOWEVENT_SHOWN: printf("Window %" PRIu32 " shown\n", window.windowID); break;

		case SDL_WINDOWEVENT_HIDDEN: printf("Window %" PRIu32 " hidden\n", window.windowID); break;

		case SDL_WINDOWEVENT_EXPOSED: printf("Window %" PRIu32 " exposed\n", window.windowID); break;

		case SDL_WINDOWEVENT_MOVED:
			printf("Window %" PRIu32 " moved to %" PRId32 ",%" PRId32 "\n", window.windowID, window.data1, window.data2);
			break;

		case SDL_WINDOWEVENT_RESIZED:
			printf("Window %" PRIu32 " resized to %" PRId32 "x%" PRId32 "\n", window.windowID, window.data1, window.data2);

			if (game->event_resize != nullptr)
			{
				printf("m: %d\n", static_cast<int>(SDL_ThreadID()));
				//				dbg_check();
				game->event_resize(game, window.data1, window.data2);
			}

			break;

		case SDL_WINDOWEVENT_SIZE_CHANGED:
			printf("Window %" PRIu32 " size changed to %" PRId32 "x%" PRId32 "\n", window.windowID, window.data1, window.data2);

			if (game->event_resize != nullptr)
			{
				printf("m: %d\n", static_cast<int>(SDL_ThreadID()));
				//				dbg_check();
				game->event_resize(game, window.data1, window.data2);
			}

			break;

		case SDL_WINDOWEVENT_MINIMIZED: printf("Window %" PRIu32 " minimized\n", window.windowID); break;
		case SDL_WINDOWEVENT_MAXIMIZED: printf("Window %" PRIu32 " maximized\n", window.windowID); break;
		case SDL_WINDOWEVENT_RESTORED: printf("Window %" PRIu32 " restored\n", window.windowID); break;
		case SDL_WINDOWEVENT_ENTER: printf("Mouse entered window %" PRIu32 "\n", window.windowID); break;
		case SDL_WINDOWEVENT_LEAVE: printf("Mouse left window %" PRIu32 "\n", window.windowID); break;
		case SDL_WINDOWEVENT_FOCUS_GAINED: printf("Window %" PRIu32 " gained keyboard focus\n", window.windowID); break;
		case SDL_WINDOWEVENT_FOCUS_LOST: printf("Window %" PRIu32 " lost keyboard focus\n", window.windowID); break;
		case SDL_WINDOWEVENT_CLOSE: printf("Window %" PRIu32 " closed\n", window.windowID); break;
		default: printf("Window %" PRIu32 " got unknown event %" PRIu8 "\n", window.windowID, window.event); break;
	}
}

static void process_display_event(GameApi* game, SDL_DisplayEvent display)
{
	bool sdl = false;

	switch (display.event)
	{
		case SDL_DISPLAYEVENT_ORIENTATION: sdl = true; // @suppress("No break at end of case")
		case static_cast<Uint8>(DisplayOrientation::DisplayEventOrientation):
		{
			printf("Display %" PRIu32 "[%s] changed orientation to %d - ", display.display, sdl ? "SDL" : "Lumen",
			       static_cast<int>(display.data1));

			EventDisplay d {};
			d.orientation = DisplayOrientation::Unknown;

			switch (display.data1)
			{
				case SDL_ORIENTATION_UNKNOWN: printf("UNKNOWN\n"); break;
				case SDL_ORIENTATION_LANDSCAPE:
					printf("LANDSCAPE\n");
					d.orientation = DisplayOrientation::Landscape;
					break;
				case SDL_ORIENTATION_LANDSCAPE_FLIPPED:
					printf("LANDSCAPE_FLIPPED\n");
					d.orientation = DisplayOrientation::LandscapeFlipped;
					break;
				case SDL_ORIENTATION_PORTRAIT:
					printf("PORTRAIT\n");
					d.orientation = DisplayOrientation::Portrait;
					break;
				case SDL_ORIENTATION_PORTRAIT_FLIPPED:
					d.orientation = DisplayOrientation::PortraitFlipped;
					printf("PORTRAIT_FLIPPED\n");
					break;
				default: printf("???\n");
			}

			if (!sdl)
			{
				if (game->event_display != nullptr)
				{
					//				dbg_check();
					game->event_display(game, &d);
				}
			}

			break;
		}
		default: printf("Display %" PRIu32 " got unknown event 0x%" PRIx8 "\n", display.display, display.event); break;
	}
}

int game_poll_event(GameApi* game)
{
	EXIT_IF(!game);
	Dialog::ImeDialog::SyncSdlInput(g_window_ctx != nullptr ? g_window_ctx->window : nullptr);

	auto* event = static_cast<SDL_Event*>(game->data2);

	EXIT_IF(!event);

	const int result = SDL_PollEvent(event);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	ReconcileRuntimeGameControllers();
#endif
	return result;
}

int game_wait_event(GameApi* game)
{
	EXIT_IF(!game);

	auto* event = static_cast<SDL_Event*>(game->data2);

	EXIT_IF(!event);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	// Keep a paused presentation responsive to the guest-stop latch even when
	// the quit event was flushed during startup or SDL could not enqueue it.
	const int result = SDL_WaitEventTimeout(event, 50);
#else
	const int result = SDL_WaitEvent(event);
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	ReconcileRuntimeGameControllers();
#endif
	return result;
}

void game_process_event(GameApi* game, double time_s)
{
	EXIT_IF(!game);

	auto* event = static_cast<SDL_Event*>(game->data2);

	EXIT_IF(!event);

	// printf("Event: 0x%04" PRIx32 "\n", event.type);

	EXIT_IF(SDL_GetEventState(SDL_DISPLAYEVENT) != SDL_ENABLE);
	if (Dialog::ImeDialog::ProcessSdlEvent(*event)) return;

	switch (event->type)
	{
		case SDL_QUIT:
			if (game->event_quit != nullptr)
			{
				game->event_quit(game);
			}
			break;

		case SDL_APP_TERMINATING:
			if (game->event_terminate != nullptr)
			{
				game->event_terminate(game);
			}
			break;

		case SDL_APP_LOWMEMORY:
			if (game->event_low_memory != nullptr)
			{
				game->event_low_memory(game);
			}
			break;

		case SDL_APP_WILLENTERBACKGROUND:
			if (game->event_will_enter_background != nullptr)
			{
				game->event_will_enter_background(game);
			}
			break;

		case SDL_APP_DIDENTERBACKGROUND:
			if (game->event_did_enter_background != nullptr)
			{
				game->event_did_enter_background(game);
			}
			break;

		case SDL_APP_WILLENTERFOREGROUND:
			if (game->event_will_enter_foreground != nullptr)
			{
				game->event_will_enter_foreground(game);
			}
			break;

		case SDL_APP_DIDENTERFOREGROUND:
			if (game->event_did_enter_foreground != nullptr)
			{
				game->event_did_enter_foreground(game);
			}
			break;

		case SDL_KEYDOWN:
		case SDL_KEYUP:
		{
			EventKeyboard key {};

			key.down              = (event->type == SDL_KEYDOWN);
			key.up                = (event->type == SDL_KEYUP);
			key.pressed           = (event->key.state == SDL_PRESSED);
			key.released          = (event->key.state == SDL_RELEASED);
			key.repeat            = (event->key.repeat != 0u);
			key.scan_code         = event->key.keysym.scancode;
			key.key_code          = event->key.keysym.sym;
			key.mod               = event->key.keysym.mod;
			key.timestamp_seconds = time_s;

			if (game->event_keyboard != nullptr)
			{
				game->event_keyboard(game, &key);
			}

			break;
		}

		case SDL_WINDOWEVENT: process_window_event(game, event->window); break;

		case SDL_DISPLAYEVENT: process_display_event(game, event->display); break;

		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
		{
			EventMouse mb {};

			// printf("event.button.which = %" PRIu32"\n", event.button.which);

			mb.down              = (event->button.type == SDL_MOUSEBUTTONDOWN);
			mb.up                = (event->button.type == SDL_MOUSEBUTTONUP);
			mb.left              = (event->button.button == SDL_BUTTON_LEFT);
			mb.middle            = (event->button.button == SDL_BUTTON_MIDDLE);
			mb.right             = (event->button.button == SDL_BUTTON_RIGHT);
			mb.x1                = (event->button.button == SDL_BUTTON_X1);
			mb.x2                = (event->button.button == SDL_BUTTON_X2);
			mb.touch             = (event->button.which == SDL_TOUCH_MOUSEID);
			mb.pressed           = (event->button.state == SDL_PRESSED);
			mb.released          = (event->button.state == SDL_RELEASED);
			mb.num_of_clicks     = event->button.clicks;
			mb.wheel             = false;
			mb.x                 = event->button.x;
			mb.y                 = event->button.y;
			mb.motion            = false;
			mb.motion_x          = 0;
			mb.motion_y          = 0;
			mb.timestamp_seconds = time_s;

			if (game->event_mouse != nullptr)
			{
				game->event_mouse(game, &mb);
			}

			break;
		}

		case SDL_MOUSEWHEEL:
		{
			EventMouse mb {};

			mb.down              = false;
			mb.up                = false;
			mb.left              = false;
			mb.middle            = false;
			mb.right             = false;
			mb.x1                = false;
			mb.x2                = false;
			mb.touch             = (event->wheel.which == SDL_TOUCH_MOUSEID);
			mb.pressed           = false;
			mb.released          = false;
			mb.num_of_clicks     = 0;
			mb.wheel             = true;
			mb.x                 = event->wheel.x;
			mb.y                 = event->wheel.y;
			mb.motion            = false;
			mb.motion_x          = 0;
			mb.motion_y          = 0;
			mb.timestamp_seconds = time_s;

			if (game->event_mouse != nullptr)
			{
				game->event_mouse(game, &mb);
			}

			break;
		}

		case SDL_MOUSEMOTION:
		{
			EventMouse mb {};

			mb.down              = false;
			mb.up                = false;
			mb.left              = ((event->motion.state & LUMEN_SDL_BUTTON_LMASK) != 0u);
			mb.middle            = ((event->motion.state & LUMEN_SDL_BUTTON_MMASK) != 0u);
			mb.right             = ((event->motion.state & LUMEN_SDL_BUTTON_RMASK) != 0u);
			mb.x1                = ((event->motion.state & LUMEN_SDL_BUTTON_X1MASK) != 0u);
			mb.x2                = ((event->motion.state & LUMEN_SDL_BUTTON_X2MASK) != 0u);
			mb.touch             = (event->motion.which == SDL_TOUCH_MOUSEID);
			mb.pressed           = false;
			mb.released          = false;
			mb.num_of_clicks     = 0;
			mb.wheel             = false;
			mb.x                 = event->motion.x;
			mb.y                 = event->motion.y;
			mb.motion            = true;
			mb.motion_x          = event->motion.xrel;
			mb.motion_y          = event->motion.yrel;
			mb.timestamp_seconds = time_s;

			if (game->event_mouse != nullptr)
			{
				game->event_mouse(game, &mb);
			}

			break;
		}

		case SDL_FINGERMOTION:
		case SDL_FINGERDOWN:
		case SDL_FINGERUP:
		{
			EventFinger f {};

			f.down              = (event->tfinger.type == SDL_FINGERDOWN);
			f.up                = (event->tfinger.type == SDL_FINGERUP);
			f.motion            = (event->tfinger.type == SDL_FINGERMOTION);
			f.finger_id         = static_cast<int>(event->tfinger.fingerId);
			f.touch_id          = static_cast<int>(event->tfinger.touchId);
			f.x                 = event->tfinger.x;
			f.y                 = event->tfinger.y;
			f.dx                = event->tfinger.dx;
			f.dy                = event->tfinger.dy;
			f.pressure          = event->tfinger.pressure;
			f.timestamp_seconds = time_s;

			if (game->event_finger != nullptr)
			{
				game->event_finger(game, &f);
			}

			break;
		}

			//		case SDL_JOYAXISMOTION:
			//		case SDL_JOYBALLMOTION:
			//		case SDL_JOYHATMOTION:
			//		case SDL_JOYBUTTONDOWN:
			//		case SDL_JOYBUTTONUP:
			//		case SDL_JOYDEVICEADDED:
			//		case SDL_JOYDEVICEREMOVED:
			//		{
			//			EXIT("joystick event: %d\n", static_cast<int>(event->type));
			//			break;
			//		}

		case SDL_CONTROLLERAXISMOTION:
		{
			if ((event->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || event->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) &&
			    Controller::ControllerTriggerTraceEnabled())
			{
				Controller::ControllerTraceTriggerEvent(event->caxis.which, event->caxis.axis, event->caxis.value,
				                                       event->caxis.timestamp, SDL_GetTicks());
			}
			EventController c {};

			c.id                = event->caxis.which;
			c.button            = SDL_CONTROLLER_BUTTON_INVALID;
			c.axis_id           = event->caxis.axis;
			c.axis_value        = event->caxis.value;
			c.down              = false;
			c.up                = false;
			c.added             = false;
			c.removed           = false;
			c.remapped          = false;
			c.axis              = true;
			c.pressed           = false;
			c.released          = false;
			c.timestamp_seconds = time_s;

			if (game->event_controller != nullptr)
			{
				game->event_controller(game, &c);
			}

			break;
		}

		case SDL_CONTROLLERBUTTONDOWN:
		case SDL_CONTROLLERBUTTONUP:
		{
			if (event->cbutton.button == SDL_CONTROLLER_BUTTON_START && Controller::ControllerOptionsTraceEnabled())
			{
				auto* controller = SDL_GameControllerFromInstanceID(event->cbutton.which);
				auto* joystick = controller != nullptr ? SDL_GameControllerGetJoystick(controller) : nullptr;
				char guid[33] = "unavailable";
				if (joystick != nullptr) SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guid, sizeof(guid));
				Controller::ControllerTraceOptionsEvent(event->cbutton.which, event->cbutton.type == SDL_CONTROLLERBUTTONDOWN,
				                                       event->cbutton.timestamp, SDL_GetTicks(),
				                                       controller != nullptr ? SDL_GameControllerName(controller) : nullptr,
				                                       controller != nullptr ? SDL_GameControllerPath(controller) : nullptr, guid);
			}
			EventController c {};

			c.id                = event->cbutton.which;
			c.button            = event->cbutton.button;
			c.axis_id           = SDL_CONTROLLER_AXIS_INVALID;
			c.axis_value        = 0;
			c.down              = (event->cbutton.type == SDL_CONTROLLERBUTTONDOWN);
			c.up                = (event->cbutton.type == SDL_CONTROLLERBUTTONUP);
			c.added             = false;
			c.removed           = false;
			c.remapped          = false;
			c.axis              = false;
			c.pressed           = (event->cbutton.state == SDL_PRESSED);
			c.released          = (event->cbutton.state == SDL_RELEASED);
			c.timestamp_seconds = time_s;

			if (game->event_controller != nullptr)
			{
				game->event_controller(game, &c);
			}

			break;
		}

		case SDL_CONTROLLERDEVICEADDED:
		case SDL_CONTROLLERDEVICEREMOVED:
		case SDL_CONTROLLERDEVICEREMAPPED:
		{
			EventController c {};

			c.id                = event->cdevice.which;
			c.button            = SDL_CONTROLLER_BUTTON_INVALID;
			c.axis_id           = SDL_CONTROLLER_AXIS_INVALID;
			c.axis_value        = 0;
			c.down              = false;
			c.up                = false;
			c.added             = (event->cdevice.type == SDL_CONTROLLERDEVICEADDED);
			c.removed           = (event->cdevice.type == SDL_CONTROLLERDEVICEREMOVED);
			c.remapped          = (event->cdevice.type == SDL_CONTROLLERDEVICEREMAPPED);
			c.axis              = false;
			c.pressed           = false;
			c.released          = false;
			c.timestamp_seconds = time_s;

			if (game->event_controller != nullptr)
			{
				game->event_controller(game, &c);
			}

			break;
		}
	}
}

GameApi* game_create_api()
{
	auto* api = new GameApi;

	// memset(api, 0, sizeof(GameApi));

	api->init              = game_init;
	api->render_and_update = game_render_and_update;
	api->close             = game_close;

	api->event_quit                  = game_event_quit;
	api->event_terminate             = game_event_terminate;
	api->event_keyboard              = game_event_keyboard;
	api->event_mouse                 = game_event_mouse;
	api->event_finger                = game_event_finger;
	api->event_controller            = game_event_controller;
	api->event_display               = game_event_display;
	api->event_low_memory            = game_event_low_memory;
	api->event_will_enter_background = game_event_will_enter_background;
	api->event_did_enter_background  = game_event_did_enter_background;
	api->event_will_enter_foreground = game_event_will_enter_foreground;
	api->event_did_enter_foreground  = game_event_did_enter_foreground;
	api->event_resize                = game_event_resize;

	api->show_window = game_show_window;

	api->need_exit = game_need_exit;
	api->is_paused = game_is_paused;

	api->poll_event    = game_poll_event;
	api->wait_event    = game_wait_event;
	api->process_event = game_process_event;

	return api;
}

void game_delete_api(GameApi* api)
{
	Delete(api);
}

void game_main_loop(GameApi* game, void* data)
{
	bool need_exit = false;

	Core::Timer timer;
	timer.Start();

	if (!game->init(game, timer, data))
	{
		need_exit = true;
	}

	for (;;)
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		need_exit = MobileRuntimeLoopExitRequested(need_exit, Loader::GuestExecutionStopRequested());
#endif
		if (need_exit)
		{
			break;
		}

		if (game->poll_event(game) != 0)
		{
			if (game->process_event != nullptr)
			{
				game->process_event(game, timer.GetTimeS());
			}
			continue;
		}

		if (game->is_paused(game))
		{
			if (!timer.IsPaused())
			{
				timer.Pause();
			}

			const int wait_result = game->wait_event(game);

			if (wait_result != 0 && game->process_event != nullptr)
			{
				game->process_event(game, timer.GetTimeS());
			}
			need_exit = game->need_exit(game);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
			need_exit = MobileRuntimeLoopExitRequested(need_exit, Loader::GuestExecutionStopRequested());
#endif
			continue;
		}

		need_exit = game->need_exit(game);

		if (game->is_paused(game))
		{
			if (!timer.IsPaused())
			{
				timer.Pause();
			}
		} else
		{
			if (timer.IsPaused())
			{
				timer.Resume();
			}

			if (!need_exit)
			{
				if (!game->render_and_update(game, timer))
				{
					std::fprintf(stderr, "[WINDOW_EXIT] RenderAndUpdate returned false\n");
					std::fflush(stderr);
					need_exit = true;
				}
			}

			if (!need_exit)
			{
				//				dbg_inc();
				game->show_window(game, timer);
			}
		}
	}

	game->close(game);
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
void WindowUpdateIcon();

static void ShowWindowsStartupWindow(SDL_Window* window)
{
	SDL_ShowWindow(window);

	SDL_SysWMinfo info {};
	SDL_VERSION(&info.version);
	if (SDL_GetWindowWMInfo(window, &info) != SDL_TRUE || info.subsystem != SDL_SYSWM_WINDOWS)
	{
		std::fprintf(stderr, "[WINDOW_STARTUP_VISIBILITY] native_window=unavailable error=%s\n", SDL_GetError());
		std::fflush(stderr);
		return;
	}

	const HWND hwnd = info.info.win.window;
	const bool retry_show = IsWindowVisible(hwnd) == FALSE;
	if (retry_show)
	{
		// STARTF_USESHOWWINDOW/SW_HIDE can override SDL's first ShowWindow.
		// SDL then caches SHOWN, so a second SDL_ShowWindow would do nothing.
		// Retry the native call once at startup; later user minimization stays
		// under the user's control and is never undone by the render loop.
		ShowWindow(hwnd, SW_SHOW);
	}

	USEROBJECTFLAGS station_flags {};
	DWORD station_bytes = 0;
	const bool station_known = GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS,
	    &station_flags, sizeof(station_flags), &station_bytes) != FALSE;
	const int station_visible = station_known ? ((station_flags.dwFlags & WSF_VISIBLE) != 0 ? 1 : 0) : -1;
	// WS_VISIBLE alone cannot make a window on a noninteractive station
	// appear on the user's desktop. Keep that launch-environment limit explicit.
	std::fprintf(stderr,
	             "[WINDOW_STARTUP_VISIBILITY] native_visible=%u retry_show=%u window_station_visible=%d\n",
	             IsWindowVisible(hwnd) != FALSE ? 1u : 0u, retry_show ? 1u : 0u, station_visible);
	std::fflush(stderr);
}
#endif

static void WindowCreate(WindowContext* ctx)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->window != nullptr);
	EXIT_IF(ctx->graphic_ctx.screen_width == 0);
	EXIT_IF(ctx->graphic_ctx.screen_height == 0);

	const auto presentation = ResolveWindowPresentationConfiguration(
	    ctx->graphic_ctx.screen_width, ctx->graphic_ctx.screen_height,
	    Config::GetWindowWidth(), Config::GetWindowHeight(), Config::WindowHighDpiEnabled());
	if (!presentation.valid)
	{
		EXIT("WindowWidth and WindowHeight must either both be zero or both fit SDL window dimensions\n");
	}

	const int width  = static_cast<int>(presentation.logical_width);
	const int height = static_cast<int>(presentation.logical_height);
	const uint32_t window_flags = SelectSdlWindowFlags(
	    LUMEN_PLATFORM == LUMEN_PLATFORM_IOS, LUMEN_PLATFORM == LUMEN_PLATFORM_OSX,
	    presentation.high_dpi);
	Lumen::Emulator::StartupTrace::Emit("window", "sdl.init.begin");

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	WindowIOSLogControllerState("pre-sdl-init", nullptr);
	if (SDL_SetHintWithPriority(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight", SDL_HINT_OVERRIDE) == SDL_FALSE)
	{
		EXIT("Unable to lock the iOS runtime window to landscape orientation\n");
	}
	if (SDL_SetHintWithPriority(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "0", SDL_HINT_OVERRIDE) == SDL_FALSE)
	{
		EXIT("Unable to keep iOS system gestures available\n");
	}
	printf("WindowCreate(): iOS runtime orientation = landscape\n");
	printf("WindowCreate(): iOS home indicator = visible, deferred gesture edges = none\n");
#endif

	if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0)
	{
		EXIT("%s\n", SDL_GetError());
	}
	Lumen::Emulator::StartupTrace::Emit("window", "sdl.init.end");

	SDL_GameControllerEventState(SDL_ENABLE);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	g_next_controller_reconcile_ticks = 0;
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	g_android_physical_controller_connected = false;
#endif
	const int joystick_count = SDL_NumJoysticks();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	WindowIOSLogControllerState("post-sdl-init", nullptr);
#endif
	printf("LUMEN_CONTROLLER_SCAN phase=initial count=%d\n", joystick_count);
	for (int device_index = 0; device_index < joystick_count; device_index++)
	{
		OpenGameController(device_index, GameControllerOpenOrigin::InitialScan);
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	RefreshAndroidPhysicalControllerState(false);
	WindowAndroidControllerStateChanged(g_android_physical_controller_connected, false);
	WindowAndroidRuntimeInputReady(true);
#endif
	std::fflush(stdout);

	printf("WindowCreate(): width = %d, height = %d\n", width, height);
	if (Config::GetWindowWidth() != 0 || Config::GetWindowHeight() != 0 || presentation.high_dpi)
	{
		printf("[WINDOW_PRESENTATION] guest=%ux%u logical=%dx%d high_dpi=%u\n",
		       ctx->graphic_ctx.screen_width, ctx->graphic_ctx.screen_height, width, height,
		       (window_flags & static_cast<uint32_t>(SDL_WINDOW_ALLOW_HIGHDPI)) != 0 ? 1u : 0u);
	}

	Lumen::Emulator::StartupTrace::Emit("window", "create.begin");
	ctx->window = SDL_CreateWindow(LUMEN_SDL_WINDOW_CAPTION, LUMEN_SDL_WINDOWPOS_CENTERED, LUMEN_SDL_WINDOWPOS_CENTERED, width, height,
	                               window_flags);

	ctx->window_hidden = true;

	if (ctx->window == nullptr)
	{
		EXIT("%s\n", SDL_GetError());
	}

	SDL_SetWindowResizable(ctx->window, SDL_FALSE);
	Lumen::Emulator::StartupTrace::Emit("window", "create.end");

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	WindowUpdateIcon();
	ShowWindowsStartupWindow(ctx->window);
	ctx->window_hidden = false;
	Lumen::Emulator::StartupTrace::Emit("window", "shown.startup");
#elif LUMEN_PLATFORM != LUMEN_PLATFORM_IOS
	const char* show_window_early = std::getenv("LUMEN_SHOW_WINDOW_EARLY");
	if (show_window_early != nullptr && show_window_early[0] != '\0' && show_window_early[0] != '0')
	{
		SDL_ShowWindow(ctx->window);
		ctx->window_hidden = false;
		Lumen::Emulator::StartupTrace::Emit("window", "shown.early");
	}
#endif
}

static VkResult VulkanGetSurfaceCapabilities(VkPhysicalDevice physical_device, VkSurfaceKHR surface, SurfaceCapabilities* r)
{
	EXIT_IF(physical_device == nullptr);
	EXIT_IF(surface == nullptr);
	EXIT_IF(r == nullptr);

	r->capabilities        = {};
	r->formats             = {};
	r->present_modes       = {};
	r->format_srgb_bgra32  = false;
	r->format_unorm_bgra32 = false;

	auto result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &r->capabilities);
	if (result != VK_SUCCESS)
	{
		return result;
	}

	uint32_t formats_count = 0;
	result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &formats_count, nullptr);
	if (result != VK_SUCCESS || formats_count == 0)
	{
		return result != VK_SUCCESS ? result : VK_ERROR_FORMAT_NOT_SUPPORTED;
	}

	r->formats = Vector<VkSurfaceFormatKHR>(formats_count); // @suppress("Ambiguous problem")
	result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &formats_count, r->formats.GetData());
	if (result != VK_SUCCESS)
	{
		return result;
	}

	uint32_t present_modes_count = 0;
	result = vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &present_modes_count, nullptr);
	if (result != VK_SUCCESS || present_modes_count == 0)
	{
		return result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED;
	}

	r->present_modes = Vector<VkPresentModeKHR>(present_modes_count); // @suppress("Ambiguous problem")
	result = vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &present_modes_count,
	                                                   r->present_modes.GetData());
	if (result != VK_SUCCESS)
	{
		return result;
	}

	for (const auto& f: r->formats)
	{
		if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			r->format_srgb_bgra32 = true;
		}
		if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			r->format_unorm_bgra32 = true;
		}
	}
	return VK_SUCCESS;
}

static bool CheckFormat(VkPhysicalDevice device, VkFormat format, bool tile, VkFormatFeatureFlags features)
{
	VkFormatProperties format_props {};
	vkGetPhysicalDeviceFormatProperties(device, format, &format_props);
	if (tile)
	{
		if ((format_props.optimalTilingFeatures & features) == features)
		{
			return true;
		}
	} else
	{
		if ((format_props.linearTilingFeatures & features) == features)
		{
			return true;
		}
	}
	return false;
}

struct QueueInfo
{
	uint32_t family   = 0;
	uint32_t index    = 0;
	bool     graphics = false;
	bool     compute  = false;
	bool     transfer = false;
	bool     present  = false;
};

struct VulkanQueues
{
	uint32_t          family_count = 0;
	bool              handoff_single_queue = false;
	uint32_t          handoff_family = UINT32_MAX;
	uint32_t          handoff_queue_index = UINT32_MAX;
	Vector<uint32_t>  family_used;
	Vector<QueueInfo> all;
	Vector<QueueInfo> available;
	Vector<QueueInfo> graphics;
	Vector<QueueInfo> compute;
	Vector<QueueInfo> transfer;
	Vector<QueueInfo> present;
};

static void VulkanDumpQueues(const VulkanQueues& qs)
{
	printf("Queues selected:\n");
	printf("\t family_count = %u\n", qs.family_count);
	if (qs.handoff_single_queue)
	{
		printf("\t handoff_resource_queue = family %u, index %u (graphics/compute/transfer)\n",
		       qs.handoff_family, qs.handoff_queue_index);
	}
	Core::StringList nums;
	for (auto u: qs.family_used)
	{
		nums.Add(String::FromPrintf("%u", u));
	}
	printf("\t family_used = [%s]\n", nums.Concat(U", ").C_Str());
	printf("\t graphics:\n");
	for (const auto& q: qs.graphics)
	{
		printf("\t\t family = %u, index = %u\n", q.family, q.index);
	}
	printf("\t compute:\n");
	for (const auto& q: qs.compute)
	{
		printf("\t\t family = %u, index = %u\n", q.family, q.index);
	}
	printf("\t transfer:\n");
	for (const auto& q: qs.transfer)
	{
		printf("\t\t family = %u, index = %u\n", q.family, q.index);
	}
	printf("\t present:\n");
	for (const auto& q: qs.present)
	{
		printf("\t\t family = %u, index = %u\n", q.family, q.index);
	}
}

// Native resource aliases have no cross-queue semaphore/ownership protocol.
// Use one native queue for graphics, compute and copy during handoff while
// keeping logical processors separate. Presentation retains its sharing policy.
static void SelectVulkanQueueRoles(VulkanQueues* qs, uint32_t graphics_num, uint32_t compute_num,
                                  uint32_t transfer_num, uint32_t present_num)
{
	for (uint32_t i = 0; i < graphics_num; i++)
	{
		if (auto index = qs->available.Find(true, [](auto& q, auto& b) { return q.graphics == b; }); qs->available.IndexValid(index))
		{
			qs->family_used[qs->available.At(index).family]++;
			qs->graphics.Add(qs->available.At(index));
			qs->available.RemoveAt(index);
		}
	}

	if (qs->handoff_single_queue && !qs->graphics.IsEmpty())
	{
		qs->handoff_family = qs->graphics.At(0).family;
		qs->handoff_queue_index = qs->graphics.At(0).index;
	}
	const auto resource_queue = [qs](const QueueInfo& q) {
		return !qs->handoff_single_queue ||
		    (q.family == qs->handoff_family && q.index == qs->handoff_queue_index);
	};

	// Reserve queues for transfer and presentation before taking optional
	// additional compute queues. Some portability drivers expose several
	// single-queue families instead of one family with many queues.
	for (uint32_t i = 0; i < transfer_num; i++)
	{
		if (auto index = qs->available.Find(true, [&](auto& q, auto& b) { return q.transfer == b && resource_queue(q); }); qs->available.IndexValid(index))
		{
			qs->family_used[qs->available.At(index).family]++;
			qs->transfer.Add(qs->available.At(index));
			qs->available.RemoveAt(index);
		}
	}

	for (uint32_t i = 0; i < present_num; i++)
	{
		if (auto index = qs->available.Find(true, [](auto& q, auto& b) { return q.present == b; }); qs->available.IndexValid(index))
		{
			qs->family_used[qs->available.At(index).family]++;
			qs->present.Add(qs->available.At(index));
			qs->available.RemoveAt(index);
		}
	}

	for (uint32_t i = 0; i < compute_num; i++)
	{
		if (auto index = qs->available.Find(true, [&](auto& q, auto& b) { return q.compute == b && resource_queue(q); }); qs->available.IndexValid(index))
		{
			qs->family_used[qs->available.At(index).family]++;
			qs->compute.Add(qs->available.At(index));
			qs->available.RemoveAt(index);
		}
	}

	// A Vulkan queue can serve more than one role. Prefer distinct queues when
	// they exist, then share compatible handles (MoltenVK commonly exposes one
	// universal queue on Apple GPUs).
	auto share_compatible = [qs](Vector<QueueInfo>* selected, uint32_t requested, auto supports)
	{
		for (const auto& candidate: qs->all)
		{
			if (selected->Size() >= requested)
			{
				break;
			}
			if (!supports(candidate))
			{
				continue;
			}

			bool already_selected = false;
			for (const auto& queue: *selected)
			{
				if (queue.family == candidate.family && queue.index == candidate.index)
				{
					already_selected = true;
					break;
				}
			}
			if (!already_selected)
			{
				selected->Add(candidate);
			}
		}
	};

	share_compatible(&qs->graphics, graphics_num, [&](const auto& q) { return q.graphics && resource_queue(q); });
	share_compatible(&qs->transfer, transfer_num, [&](const auto& q) { return q.transfer && resource_queue(q); });
	share_compatible(&qs->present, present_num, [](const auto& q) { return q.present; });
	share_compatible(&qs->compute, compute_num, [&](const auto& q) { return q.compute && resource_queue(q); });

	// Queue roles may now alias. Request each family only through its highest
	// selected queue index instead of counting the same handle more than once.
	for (uint32_t i = 0; i < qs->family_count; i++)
	{
		qs->family_used[i] = 0;
	}
	auto mark_used = [qs](const Vector<QueueInfo>& selected)
	{
		for (const auto& queue: selected)
		{
			const uint32_t required_count = queue.index + 1;
			if (qs->family_used[queue.family] < required_count)
			{
				qs->family_used[queue.family] = required_count;
			}
		}
	};
	mark_used(qs->graphics);
	mark_used(qs->transfer);
	mark_used(qs->present);
	mark_used(qs->compute);
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool WindowHandoffQueueFamilySelectionTest()
{
	const auto select = [](const Vector<QueueInfo>& queues, uint32_t family_count, bool handoff) {
		VulkanQueues result;
		result.family_count = family_count;
		result.handoff_single_queue = handoff;
		result.all = queues;
		result.available = queues;
		for (uint32_t family = 0; family < family_count; ++family) result.family_used.Add(0);
		SelectVulkanQueueRoles(&result, 1, 8, 1, 1);
		return result;
	};
	// Observed Adreno 840 topology: four universal family-0 queues and one
	// separate compute queue. Handoff uses one native queue for all resources.
	Vector<QueueInfo> target;
	for (uint32_t index = 0; index < 4; ++index) target.Add({0, index, true, true, true, true});
	target.Add({1, 0, false, true, true, false});
	const auto isolated = select(target, 2, true);
	if (isolated.graphics.Size() != 1 || isolated.transfer.Size() != 1 || isolated.present.Size() != 1 ||
	    isolated.compute.Size() != 1 || isolated.handoff_family != 0 || isolated.handoff_queue_index != 0 ||
	    isolated.family_used.At(0) != 2 || isolated.family_used.At(1) != 0 ||
	    isolated.graphics.At(0).index != isolated.transfer.At(0).index || isolated.compute.At(0).index != 0) return false;
	if (isolated.compute.At(0).family != 0 || isolated.transfer.At(0).family != 0 ||
	    isolated.present.At(0).index != 1) return false;
	const auto ordinary = select(target, 2, false);
	if (ordinary.compute.Size() != 5 || ordinary.family_used.At(1) != 1 ||
	    ordinary.graphics.At(0).index == ordinary.transfer.At(0).index) return false;

	// One universal queue may serve all resource roles while presentation uses
	// its own family. Request the shared native handle only once.
	const auto shared = select({{0, 0, true, true, true, false}, {1, 0, false, false, false, true}}, 2, true);
	if (shared.graphics.Size() != 1 || shared.transfer.Size() != 1 || shared.compute.Size() != 1 ||
	    shared.present.Size() != 1 || shared.present.At(0).family != 1 ||
	    shared.graphics.At(0).family != 0 || shared.transfer.At(0).family != 0 || shared.compute.At(0).family != 0 ||
	    shared.family_used.At(0) != 1 || shared.family_used.At(1) != 1) return false;

	// A compute-only peer cannot rescue an incompatible graphics family: the
	// existing required-role validation rejects the empty compute selection.
	const auto incompatible = select({{0, 0, true, false, true, true}, {1, 0, false, true, true, false}}, 2, true);
	if (!incompatible.compute.IsEmpty() || incompatible.family_used.At(1) != 0) return false;
	std::fprintf(stderr, "[GPU_MEMORY_HANDOFF_QUEUE_FAMILY_TEST] one-native-resource-queue=checked legacy-distinct=checked"
	                     " separate-present=checked incompatible-compute=denied\n");
	return true;
}
#endif

static VulkanQueues VulkanFindQueues(VkPhysicalDevice device, VkSurfaceKHR surface, uint32_t graphics_num, uint32_t compute_num,
                                     uint32_t transfer_num, uint32_t present_num)
{
	EXIT_IF(device == nullptr);
	EXIT_IF(surface == nullptr);

	VulkanQueues qs;

	uint32_t queue_family_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, nullptr);
	Vector<VkQueueFamilyProperties> queue_families(queue_family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, queue_families.GetData());

	qs.family_count = queue_family_count;

	uint32_t family = 0;
	for (auto& f: queue_families)
	{
		VkBool32 presentation_supported = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(device, family, surface, &presentation_supported);

		printf("\tqueue family: %s [count = %u], [present = %s]\n", string_VkQueueFlags(f.queueFlags).c_str(), f.queueCount,
		       (presentation_supported == VK_TRUE ? "true" : "false"));

		for (uint32_t i = 0; i < f.queueCount; i++)
		{
			QueueInfo info;
			info.family   = family;
			info.index    = i;
			info.graphics = (f.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
			info.compute  = (f.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
			// Vulkan guarantees transfer operations on graphics and compute
			// queues even when the driver omits the optional TRANSFER flag.
			info.transfer = (f.queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0;
			info.present  = (presentation_supported == VK_TRUE);

			qs.all.Add(info);
			qs.available.Add(info);
		}

		qs.family_used.Add(0);

		family++;
	}

	qs.handoff_single_queue = GpuMemoryCpuHandoffEnabled();
	SelectVulkanQueueRoles(&qs, graphics_num, compute_num, transfer_num, present_num);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (qs.handoff_single_queue)
	{
		__android_log_print(ANDROID_LOG_INFO, "LumenAndroidRuntime",
		    "[GPU_MEMORY_HANDOFF_QUEUE_POLICY] resource_family=%u resource_index=%u graphics=%u compute=%u transfer=%u present_family=%u present_index=%u",
		    qs.handoff_family, qs.handoff_queue_index, qs.graphics.Size(), qs.compute.Size(), qs.transfer.Size(),
		    qs.present.IsEmpty() ? UINT32_MAX : qs.present.At(0).family,
		    qs.present.IsEmpty() ? UINT32_MAX : qs.present.At(0).index);
	}
#endif
	return qs;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void VulkanFindPhysicalDevice(VkInstance instance, VkSurfaceKHR surface, const Vector<const char*>& device_extensions,
                                     SurfaceCapabilities* out_capabilities, VkPhysicalDevice* out_device, VulkanQueues* out_queues)
{
	EXIT_IF(instance == nullptr);
	EXIT_IF(surface == nullptr);
	EXIT_IF(out_capabilities == nullptr);
	EXIT_IF(out_device == nullptr);
	EXIT_IF(out_queues == nullptr);

	uint32_t devices_count = 0;
	const auto enumerate_count_result = vkEnumeratePhysicalDevices(instance, &devices_count, nullptr);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	std::fprintf(stderr, "[VULKAN_DEVICE_SELECTION] enumerate_count=%s (%d), devices=%u\n",
	             string_VkResult(enumerate_count_result), static_cast<int>(enumerate_count_result), devices_count);
	std::fflush(stderr);
#else
	(void)enumerate_count_result;
#endif

	EXIT_NOT_IMPLEMENTED(devices_count == 0);

	Vector<VkPhysicalDevice> devices(devices_count);
	const auto enumerate_devices_result = vkEnumeratePhysicalDevices(instance, &devices_count, devices.GetData());
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	std::fprintf(stderr, "[VULKAN_DEVICE_SELECTION] enumerate_devices=%s (%d), devices=%u\n",
	             string_VkResult(enumerate_devices_result), static_cast<int>(enumerate_devices_result), devices_count);
	std::fflush(stderr);
#else
	(void)enumerate_devices_result;
#endif

	VkPhysicalDevice best_device = nullptr;
	VulkanQueues     best_queues;

	for (const auto& device: devices)
	{
		bool skip_device = false;

		VkPhysicalDeviceProperties device_properties {};
		VkPhysicalDeviceFeatures   device_features {};

		vkGetPhysicalDeviceProperties(device, &device_properties);
		// The API-26 Vulkan loader exports the Vulkan 1.0 entry point. No chained
		// feature structures are queried here, so the baseline call is sufficient
		// and avoids importing vkGetPhysicalDeviceFeatures2 from a newer API.
		vkGetPhysicalDeviceFeatures(device, &device_features);
		auto reject_device = [&](const char* reason)
		{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
			std::fprintf(stderr, "[VULKAN_DEVICE_REJECTED] device=%s reason=%s\n", device_properties.deviceName, reason);
			std::fflush(stderr);
#else
			(void)reason;
#endif
			skip_device = true;
		};

		printf("Vulkan device: %s\n", device_properties.deviceName);

		auto qs = VulkanFindQueues(device, surface, GraphicContext::QUEUE_GFX_NUM, GraphicContext::QUEUE_COMPUTE_NUM,
		                           GraphicContext::QUEUE_UTIL_NUM, GraphicContext::QUEUE_PRESENT_NUM);

		VulkanDumpQueues(qs);

		if (qs.graphics.Size() != GraphicContext::QUEUE_GFX_NUM ||
		    !(qs.compute.Size() >= 1 && qs.compute.Size() <= GraphicContext::QUEUE_COMPUTE_NUM) ||
		    qs.transfer.Size() != GraphicContext::QUEUE_UTIL_NUM || qs.present.Size() != GraphicContext::QUEUE_PRESENT_NUM)
		{
			printf("Not enough queues\n");
			reject_device("not enough queues");
		}

		if (device_features.fragmentStoresAndAtomics != VK_TRUE)
		{
			printf("fragmentStoresAndAtomics is not supported\n");
			reject_device("fragmentStoresAndAtomics is not supported");
		}

		if (device_features.samplerAnisotropy != VK_TRUE)
		{
			printf("samplerAnisotropy is not supported\n");
			reject_device("samplerAnisotropy is not supported");
		}

		if (device_features.textureCompressionBC != VK_TRUE)
		{
			printf("textureCompressionBC is not supported\n");
			reject_device("textureCompressionBC is not supported");
		}

		if (device_features.shaderStorageBufferArrayDynamicIndexing != VK_TRUE ||
		    device_features.shaderSampledImageArrayDynamicIndexing != VK_TRUE ||
		    device_features.shaderStorageImageArrayDynamicIndexing != VK_TRUE)
		{
			printf("Required shader descriptor-array dynamic indexing is not supported\n");
			reject_device("required shader descriptor-array dynamic indexing is not supported");
		}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		if (device_features.depthClamp != VK_TRUE)
		{
			printf("depthClamp is not supported\n");
			reject_device("depthClamp is not supported");
		}
#endif

		//		if (device_features2.features.shaderImageGatherExtended != VK_TRUE)
		//		{
		//			printf("shaderImageGatherExtended is not supported\n");
		//			skip_device = true;
		//		}

		if (!skip_device)
		{
			uint32_t extensions_count = 0;
			vkEnumerateDeviceExtensionProperties(device, nullptr, &extensions_count, nullptr);

			EXIT_NOT_IMPLEMENTED(extensions_count == 0);

			Vector<VkExtensionProperties> available_extensions(extensions_count);
			vkEnumerateDeviceExtensionProperties(device, nullptr, &extensions_count, available_extensions.GetData());

			for (const char* ext: device_extensions)
			{
				if (!available_extensions.Contains(ext, [](auto p, auto ext) { return strcmp(p.extensionName, ext) == 0; }))
				{
					printf("Required Vulkan device extension is not available: %s\n", ext);
					reject_device("required Vulkan device extension is not available");
					break;
				}
			}

			if (skip_device)
			{
				for (const auto& ext: available_extensions)
				{
					printf("Vulkan available extension: %s, version = %u\n", ext.extensionName, ext.specVersion);
				}
			}
		}

		if (!skip_device)
		{
			const auto capabilities_result = VulkanGetSurfaceCapabilities(device, surface, out_capabilities);
			if (capabilities_result != VK_SUCCESS)
			{
				printf("Could not query surface capabilities: %s (%d)\n", string_VkResult(capabilities_result),
				       static_cast<int>(capabilities_result));
				reject_device("could not query surface capabilities");
			}

			if (!skip_device &&
			    (out_capabilities->capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
			{
				printf("Surface cannot be destination of blit\n");
				reject_device("surface cannot be destination of blit");
			}
		}

		constexpr VkFormatFeatureFlags video_output_features =
		    VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
		if (!skip_device && !CheckFormat(device, VK_FORMAT_R8G8B8A8_SRGB, true, video_output_features))
		{
			printf("Format VK_FORMAT_R8G8B8A8_SRGB cannot be used as a linearly filtered blit source\n");
			reject_device("VK_FORMAT_R8G8B8A8_SRGB is not a linearly filtered blit source");
		}

		if (!skip_device && !CheckFormat(device, VK_FORMAT_B8G8R8A8_SRGB, true, video_output_features))
		{
			printf("Format VK_FORMAT_B8G8R8A8_SRGB cannot be used as a linearly filtered blit source\n");
			reject_device("VK_FORMAT_B8G8R8A8_SRGB is not a linearly filtered blit source");
		}

		if (!skip_device)
		{
			VkFormat surface_format = out_capabilities->formats.At(0).format;
			if (out_capabilities->format_unorm_bgra32)
			{
				surface_format = VK_FORMAT_B8G8R8A8_UNORM;
			} else if (out_capabilities->format_srgb_bgra32)
			{
				surface_format = VK_FORMAT_B8G8R8A8_SRGB;
			}
			if (!CheckFormat(device, surface_format, true, VK_FORMAT_FEATURE_BLIT_DST_BIT))
			{
				printf("Surface format %d cannot be used as blit destination\n", static_cast<int>(surface_format));
				reject_device("surface format cannot be used as blit destination");
			}
		}

		if (!skip_device && !CheckFormat(device, VK_FORMAT_D32_SFLOAT, true, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
		{
			printf("Format VK_FORMAT_D32_SFLOAT cannot be used as depth buffer\n");
			reject_device("VK_FORMAT_D32_SFLOAT cannot be used as depth buffer");
		}

		if (!skip_device && !CheckFormat(device, VK_FORMAT_D32_SFLOAT_S8_UINT, true, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
		{
			printf("Format VK_FORMAT_D32_SFLOAT_S8_UINT cannot be used as depth buffer\n");
			reject_device("VK_FORMAT_D32_SFLOAT_S8_UINT cannot be used as depth buffer");
		}

		if (!skip_device && !CheckFormat(device, VK_FORMAT_D16_UNORM, true, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
		{
			printf("Format VK_FORMAT_D16_UNORM cannot be used as depth buffer\n");
			reject_device("VK_FORMAT_D16_UNORM cannot be used as depth buffer");
		}

#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID && LUMEN_PLATFORM != LUMEN_PLATFORM_OSX && \
    LUMEN_PLATFORM != LUMEN_PLATFORM_IOS && LUMEN_PLATFORM != LUMEN_PLATFORM_WINDOWS
		if (!skip_device && !CheckFormat(device, VK_FORMAT_D24_UNORM_S8_UINT, true,
		                                 VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
		{
			printf("Format VK_FORMAT_D24_UNORM_S8_UINT cannot be used as depth buffer\n");
			reject_device("VK_FORMAT_D24_UNORM_S8_UINT cannot be used as depth buffer");
		}
#endif

		constexpr VkFormatFeatureFlags bc_texture_features =
		    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
		    VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
		const VkFormat bc_texture_formats[] = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK,
		                                       VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK,
		                                       VK_FORMAT_BC7_UNORM_BLOCK};
		for (const auto format: bc_texture_formats)
		{
			if (!skip_device && !CheckFormat(device, format, true, bc_texture_features))
			{
				printf("Required BC texture format %d is not supported\n", static_cast<int>(format));
				reject_device("required BC texture format is not supported");
			}
		}

		if (!skip_device &&
		    !CheckFormat(device, VK_FORMAT_R8G8B8A8_SRGB, true, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
		{
			printf("Format VK_FORMAT_R8G8B8A8_SRGB cannot be used as texture\n");
			reject_device("VK_FORMAT_R8G8B8A8_SRGB cannot be used as texture");
		}

		if (!skip_device &&
		    !CheckFormat(device, VK_FORMAT_R8_UNORM, true, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
		{
			printf("Format VK_FORMAT_R8_UNORM cannot be used as texture\n");
			reject_device("VK_FORMAT_R8_UNORM cannot be used as texture");
		}

		if (!skip_device && !CheckFormat(device, GetRenderTextureVulkanFormat(RenderTextureFormat::R8G8Unorm), true,
		                                 GetRenderTextureRequiredFormatFeatures()))
		{
			printf("Format VK_FORMAT_R8G8_UNORM cannot be used as render target and texture\n");
			reject_device("VK_FORMAT_R8G8_UNORM cannot be used as render target and texture");
		}

		if (!skip_device &&
		    !CheckFormat(device, VK_FORMAT_R8G8B8A8_SRGB, true, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
		{
			printf("Format VK_FORMAT_R8G8B8A8_SRGB cannot be used as texture\n");

			if (!skip_device && !CheckFormat(device, VK_FORMAT_R8G8B8A8_UNORM, true,
			                                 VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
			{
				printf("Format VK_FORMAT_R8G8B8A8_UNORM cannot be used as texture\n");
				reject_device("VK_FORMAT_R8G8B8A8_UNORM cannot be used as texture");
			}
		}

		if (!skip_device &&
		    !CheckFormat(device, VK_FORMAT_B8G8R8A8_SRGB, true, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
		{
			printf("Format VK_FORMAT_B8G8R8A8_SRGB cannot be used as texture\n");

			if (!skip_device && !CheckFormat(device, VK_FORMAT_B8G8R8A8_UNORM, true,
			                                 VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
			{
				printf("Format VK_FORMAT_B8G8R8A8_UNORM cannot be used as texture\n");
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
				// Stock mobile drivers commonly expose storage images for RGBA but
				// not BGRA. Do not reject the whole GPU for that optional shader
				// path; StorageTexture keeps its per-use format check fail-closed.
				LUMEN_LOGI("Vulkan capability: BGRA storage images unavailable; deferring the optional path\n");
#else
				reject_device("BGRA storage image formats are not supported");
#endif
			}
		}

		/*if (!skip_device && !CheckFormat(device, VK_FORMAT_S8_UINT, true, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
		{
		    printf("Format VK_FORMAT_S8_UINT cannot be used as depth buffer");
		    skip_device = true;
		}*/

		if (!skip_device && device_properties.limits.maxSamplerAnisotropy < 16.0f)
		{
			printf("maxSamplerAnisotropy < 16.0f");
			reject_device("maxSamplerAnisotropy < 16.0f");
		}

		if (skip_device)
		{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
			std::fflush(stdout);
#endif
			continue;
		}

		if (best_device == nullptr || device_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
		{
			best_device = device;
			best_queues = qs;
		}
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	std::fflush(stdout);
#endif

	*out_device = best_device;
	*out_queues = best_queues;
}

static VkDevice VulkanCreateDevice(VkPhysicalDevice physical_device, VkSurfaceKHR surface, const VulkanExtensions* r,
                                   const VulkanQueues& queues, const Vector<const char*>& device_extensions)
{
	EXIT_IF(physical_device == nullptr);
	EXIT_IF(r == nullptr);
	EXIT_IF(surface == nullptr);

	Vector<VkDeviceQueueCreateInfo> queue_create_info(queues.family_count);
	Vector<Vector<float>>           queue_priority(queues.family_count);
	uint32_t                        queue_create_info_num = 0;

	for (uint32_t i = 0; i < queues.family_count; i++)
	{
		if (queues.family_used[i] != 0)
		{
			for (uint32_t pi = 0; pi < queues.family_used[i]; pi++)
			{
				queue_priority[queue_create_info_num].Add(1.0f);
			}

			queue_create_info[queue_create_info_num].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queue_create_info[queue_create_info_num].pNext            = nullptr;
			queue_create_info[queue_create_info_num].flags            = 0;
			queue_create_info[queue_create_info_num].queueFamilyIndex = i;
			queue_create_info[queue_create_info_num].queueCount       = queues.family_used[i];
			queue_create_info[queue_create_info_num].pQueuePriorities = queue_priority[queue_create_info_num].GetDataConst();

			queue_create_info_num++;
		}
	}

	VkPhysicalDeviceFeatures device_features {};
	VkPhysicalDeviceFeatures available_features {};
	vkGetPhysicalDeviceFeatures(physical_device, &available_features);
	device_features.fragmentStoresAndAtomics = VK_TRUE;
	device_features.samplerAnisotropy        = VK_TRUE;
	device_features.textureCompressionBC     = VK_TRUE;
	device_features.shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
	device_features.shaderSampledImageArrayDynamicIndexing  = VK_TRUE;
	device_features.shaderStorageImageArrayDynamicIndexing  = VK_TRUE;
	// R8 typed storage images use the core extended-format capability. Enable
	// it opportunistically so devices without it still run the established
	// RGBA8 path and fail only if that specific R8 route is requested.
	device_features.shaderStorageImageExtendedFormats = available_features.shaderStorageImageExtendedFormats;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	device_features.depthClamp               = VK_TRUE;
#endif
	// device_features.shaderImageGatherExtended = VK_TRUE;

	VkDeviceCreateInfo create_info {};
	create_info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	create_info.pNext                   = nullptr;
	create_info.flags                   = 0;
	create_info.pQueueCreateInfos       = queue_create_info.GetDataConst();
	create_info.queueCreateInfoCount    = queue_create_info_num;
	// Validation layers are instance layers. Device layers are a deprecated
	// Vulkan 1.0 compatibility field and must not be enabled on modern Vulkan.
	create_info.enabledLayerCount       = 0;
	create_info.ppEnabledLayerNames     = nullptr;
	create_info.enabledExtensionCount   = device_extensions.Size();
	create_info.ppEnabledExtensionNames = device_extensions.GetDataConst();
	create_info.pEnabledFeatures        = &device_features;

	VkDevice device = nullptr;

	const auto result = vkCreateDevice(physical_device, &create_info, nullptr, &device);
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS || device == VK_NULL_HANDLE);
#if defined(LUMEN_VULKAN_DYNAMIC_DISPATCH)
	if (const auto* missing = VulkanDispatchLoadDevice(device))
	{
		EXIT("Could not bind Vulkan device dispatch: %s\n", missing);
	}
	std::fprintf(stderr, "[LUMEN_PS4_VULKAN_DISPATCH_V1] stage=device source=sdl2 complete=1\n");
#endif

	return device;
}

static void VulkanGetExtensions(SDL_Window* window, VulkanExtensions* r)
{
	EXIT_IF(window == nullptr);
	EXIT_IF(r == nullptr);

	uint32_t required_extensions_count  = 0;
	uint32_t available_extensions_count = 0;
	uint32_t available_layers_count     = 0;

	auto sdl_result = SDL_Vulkan_GetInstanceExtensions(window, &required_extensions_count, nullptr);

	EXIT_NOT_IMPLEMENTED(sdl_result == SDL_FALSE);
	EXIT_NOT_IMPLEMENTED(required_extensions_count == 0);

	r->required_extensions = Vector<const char*>(required_extensions_count, false); // @suppress("Ambiguous problem")
	r->required_extensions.Memset(0);

	sdl_result = SDL_Vulkan_GetInstanceExtensions(window, &required_extensions_count, r->required_extensions.GetData());

	EXIT_NOT_IMPLEMENTED(sdl_result == SDL_FALSE);
	EXIT_NOT_IMPLEMENTED(required_extensions_count == 0);
	EXIT_NOT_IMPLEMENTED(required_extensions_count != r->required_extensions.Size());

	vkEnumerateInstanceExtensionProperties(nullptr, &available_extensions_count, nullptr);

	r->available_extensions = Vector<VkExtensionProperties>(available_extensions_count); // @suppress("Ambiguous problem")
	r->available_extensions.Memset(0);

	vkEnumerateInstanceExtensionProperties(nullptr, &available_extensions_count, r->available_extensions.GetData());

	EXIT_NOT_IMPLEMENTED(available_extensions_count != r->available_extensions.Size());

	r->enable_validation_layers = Config::VulkanValidationEnabled();

	bool debug_utils_available = r->available_extensions.Contains(
	    VK_EXT_DEBUG_UTILS_EXTENSION_NAME, [](auto s, auto l) { return strcmp(s.extensionName, l) == 0; });
#if defined(LUMEN_ANDROID_VULKAN_VALIDATION)
	// Packaged explicit layers can provide DEBUG_UTILS themselves. Android's
	// global enumeration need not advertise an extension from a disabled layer.
	if (!debug_utils_available && r->enable_validation_layers)
	{
		uint32_t count = 0;
		if (vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &count, nullptr) == VK_SUCCESS)
		{
			Vector<VkExtensionProperties> extensions(count);
			if (vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &count, extensions.GetData()) == VK_SUCCESS)
			{
				debug_utils_available = extensions.Contains(VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
				    [](auto s, auto l) { return strcmp(s.extensionName, l) == 0; });
			}
		}
	}
#endif
	if (debug_utils_available)
	{
		r->required_extensions.Add(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	} else
	{
		r->enable_validation_layers = false;
	}

	for (const char* ext: r->required_extensions)
	{
		printf("Vulkan required extension: %s\n", ext);
	}

	for (const auto& ext: r->available_extensions)
	{
		printf("Vulkan available extension: %s, version = %u\n", ext.extensionName, ext.specVersion);
	}

	vkEnumerateInstanceLayerProperties(&available_layers_count, nullptr);

	r->available_layers = Vector<VkLayerProperties>(available_layers_count); // @suppress("Ambiguous problem")
	r->available_layers.Memset(0);
	vkEnumerateInstanceLayerProperties(&available_layers_count, r->available_layers.GetData());

	EXIT_NOT_IMPLEMENTED(available_layers_count != r->available_layers.Size());

	for (const auto& l: r->available_layers)
	{
		printf("Vulkan available layer: %s, specVersion = %u, implVersion = %u, %s\n", l.layerName, l.specVersion, l.implementationVersion,
		       l.description);
	}

	r->required_layers = {"VK_LAYER_KHRONOS_validation"};

	if (r->enable_validation_layers)
	{
		for (const char* l: r->required_layers)
		{
			if (!r->available_layers.Contains(l, [](auto s, auto l) { return strcmp(s.layerName, l) == 0; }))
			{
				printf("no validation layer: %s\n", l);
				r->enable_validation_layers = false;
				break;
			}
		}
	}

	if (r->enable_validation_layers)
	{
		vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &available_extensions_count, nullptr);

		Vector<VkExtensionProperties> available_extensions(available_extensions_count);

		vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &available_extensions_count, available_extensions.GetData());

		for (const auto& ext: available_extensions)
		{
			printf("VK_LAYER_KHRONOS_validation available extension: %s, version = %u\n", ext.extensionName, ext.specVersion);
		}

		if (available_extensions.Contains("VK_EXT_validation_features", [](auto s, auto l) { return strcmp(s.extensionName, l) == 0; }))
		{
			r->required_extensions.Add("VK_EXT_validation_features");
		} else
		{
			r->enable_validation_layers = false;
		}
	}
#if defined(LUMEN_ANDROID_VULKAN_VALIDATION)
	EXIT_NOT_IMPLEMENTED(!r->enable_validation_layers);
#endif
}

static VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT      message_severity,
                                                                   VkDebugUtilsMessageTypeFlagsEXT             message_types,
                                                                   const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
                                                                   void* /*user_data*/)
{
	const char* severity_str   = nullptr;
	const char* severity_color = FG_DEFAULT;
	bool        skip           = false;
	bool        error          = false;
	bool        debug_printf   = false;
	switch (message_severity)
	{
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
			severity_str   = "V";
			severity_color = FG_BRIGHT_WHITE;
			skip           = true;
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
			if ((message_types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0 && Config::SpirvDebugPrintfEnabled() &&
			    strcmp(callback_data->pMessageIdName, "UNASSIGNED-DEBUG-PRINTF") == 0)
			{
				debug_printf   = true;
				severity_color = FG_BRIGHT_YELLOW;
				skip           = true;
			} else
			{
				severity_str   = "I";
				severity_color = FG_DEFAULT;
			}
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
			severity_str   = "W";
			severity_color = FG_RED;
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
			severity_str   = "E";
			severity_color = FG_BRIGHT_RED;
			error          = true;
			break;
		default: severity_str = "?";
	}

	if (error)
	{
#if defined(LUMEN_ANDROID_VULKAN_VALIDATION)
		// The diagnostic collects startup and gameplay in one process. Keep
		// checking every Vulkan call, but print only the first error per ID and
		// at most 64 full error reports; the ordinary callback still fails fast.
		static std::mutex report_mutex;
		static int32_t reported_ids[64] {};
		static uint32_t reported_count = 0;
		static bool cap_reported = false;
		{
			std::lock_guard<std::mutex> lock(report_mutex);
			for (uint32_t i = 0; i < reported_count; ++i)
			{
				if (reported_ids[i] == callback_data->messageIdNumber) return VK_FALSE;
			}
			if (reported_count == 64)
			{
				if (!cap_reported)
				{
					std::fprintf(stderr, "[LUMEN_VULKAN_VALIDATION_LOG_CAP_V1] errors=64 validationContinues=1\n");
					cap_reported = true;
				}
				return VK_FALSE;
			}
			reported_ids[reported_count++] = callback_data->messageIdNumber;
		}
		std::fprintf(stderr, "[LUMEN_VULKAN_VALIDATION_ERROR_V1] id=%s code=%d message=%s\n",
		             callback_data->pMessageIdName != nullptr ? callback_data->pMessageIdName : "unknown",
		             callback_data->messageIdNumber, callback_data->pMessage);
		return VK_FALSE;
#else
		EXIT("%s[Vulkan][%s][%u]: %s%s\n", severity_color, severity_str, static_cast<uint32_t>(message_types), callback_data->pMessage,
		     FG_DEFAULT);
#endif
	}

	if (!skip)
	{
		printf("%s[Vulkan][%s][%u]: %s%s\n", severity_color, severity_str, static_cast<uint32_t>(message_types), callback_data->pMessage,
		       FG_DEFAULT);
	}

	if (debug_printf)
	{
		auto strs = String::FromUtf8(callback_data->pMessage).Split(U'|');
		if (!strs.IsEmpty())
		{
			printf("%s%s%s\n", severity_color, strs.At(strs.Size() - 1).C_Str(), FG_DEFAULT);
		}
	}

	return VK_FALSE;
}

static VKAPI_ATTR VkResult VKAPI_CALL VulkanCreateDebugUtilsMessengerEXT(VkInstance                                instance,
                                                                         const VkDebugUtilsMessengerCreateInfoEXT* create_info,
                                                                         const VkAllocationCallbacks*              allocator,
                                                                         VkDebugUtilsMessengerEXT*                 messenger)
{
	EXIT_IF(instance == nullptr);

	if (auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
	    func != nullptr)
	{
		return func(instance, create_info, allocator, messenger);
	}
	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

[[maybe_unused]] static VkSwapchainKHR VulkanCreateSwapchainInternal(
    VkDevice device, VkSurfaceKHR surface, uint32_t width, uint32_t height, uint32_t image_count, uint32_t graphics_queue_family,
    uint32_t present_queue_family, SurfaceCapabilities* r, VkSwapchainKHR old_swapchain, VkFormat* swapchain_format,
    VkColorSpaceKHR* swapchain_color_space,
    VkExtent2D* swapchain_extent, VkImage** swapchain_images, VkImageView** swapchain_image_views,
    uint32_t* swapchain_images_count, VkResult* swapchain_create_result)
{
	EXIT_IF(device == nullptr);
	EXIT_IF(surface == nullptr);
	EXIT_IF(r == nullptr);
	EXIT_IF(swapchain_format == nullptr);
	EXIT_IF(swapchain_color_space == nullptr);
	EXIT_IF(swapchain_extent == nullptr);
	EXIT_IF(swapchain_images == nullptr);
	EXIT_IF(swapchain_image_views == nullptr);
	EXIT_IF(swapchain_images_count == nullptr);
	EXIT_IF(graphics_queue_family == static_cast<uint32_t>(-1));
	EXIT_IF(present_queue_family == static_cast<uint32_t>(-1));
	if (swapchain_create_result != nullptr)
	{
		*swapchain_create_result = VK_ERROR_INITIALIZATION_FAILED;
	}

	EXIT_NOT_IMPLEMENTED(r->formats.IsEmpty());
	const uint32_t preferred_image_count = image_count;

	VkExtent2D extent {};
	if (r->capabilities.currentExtent.width != UINT32_MAX && r->capabilities.currentExtent.height != UINT32_MAX)
	{
		extent = r->capabilities.currentExtent;
	} else
	{
		extent.width  = std::clamp(width, r->capabilities.minImageExtent.width, r->capabilities.maxImageExtent.width);
		extent.height = std::clamp(height, r->capabilities.minImageExtent.height, r->capabilities.maxImageExtent.height);
	}

	image_count = (std::max)(image_count, r->capabilities.minImageCount);
	if (r->capabilities.maxImageCount != 0)
	{
		image_count = (std::min)(image_count, r->capabilities.maxImageCount);
	}

	VkSwapchainCreateInfoKHR create_info {};
	create_info.sType         = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	create_info.pNext         = nullptr;
	create_info.flags         = 0;
	create_info.surface       = surface;
	create_info.minImageCount = image_count;

	if (VideoOut::VideoOutConfigureAnySdrEnabled())
	{
		VkSurfaceFormatKHR selected {};
		if (!TrySelectSdrSurfaceFormat(r->formats.GetData(), r->formats.Size(), &selected))
		{
			std::fputs("[VIDEOOUT_CONFIGURE_ANY_SDR] swapchain rejected: no supported SDR surface format\n", stderr);
			return nullptr;
		}
		create_info.imageFormat = selected.format;
		create_info.imageColorSpace = selected.colorSpace;
	} else if (r->format_unorm_bgra32)
	{
		create_info.imageFormat     = VK_FORMAT_B8G8R8A8_UNORM;
		create_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	} else if (r->format_srgb_bgra32)
	{
		create_info.imageFormat     = VK_FORMAT_B8G8R8A8_SRGB;
		create_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	} else
	{
		create_info.imageFormat     = r->formats.At(0).format;
		create_info.imageColorSpace = r->formats.At(0).colorSpace;
	}

	create_info.imageExtent           = extent;
	create_info.imageArrayLayers      = 1;
	create_info.imageUsage            = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	const uint32_t queue_family_indices[] = {graphics_queue_family, present_queue_family};
	if (graphics_queue_family != present_queue_family)
	{
		create_info.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
		create_info.queueFamilyIndexCount = 2;
		create_info.pQueueFamilyIndices   = queue_family_indices;
	} else
	{
		create_info.imageSharingMode      = VK_SHARING_MODE_EXCLUSIVE;
		create_info.queueFamilyIndexCount = 0;
		create_info.pQueueFamilyIndices   = nullptr;
	}

	create_info.preTransform = SelectSwapchainPreTransform(
	    r->capabilities.supportedTransforms, r->capabilities.currentTransform,
	    LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID);
	constexpr VkCompositeAlphaFlagBitsKHR composite_alpha_preferences[] = {
	    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
	    VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
	    VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
	    VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
	};
	create_info.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(0);
	for (const auto candidate: composite_alpha_preferences)
	{
		if ((r->capabilities.supportedCompositeAlpha & candidate) != 0)
		{
			create_info.compositeAlpha = candidate;
			break;
		}
	}
	EXIT_NOT_IMPLEMENTED(create_info.compositeAlpha == 0);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	create_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
#else
	if (!SelectUncappedPresentMode(r->present_modes.GetDataConst(), r->present_modes.Size(), &create_info.presentMode))
	{
		printf("Surface does not expose an uncapped present mode (IMMEDIATE or MAILBOX)\n");
		return nullptr;
	}
#endif
	create_info.clipped               = VK_TRUE;
	create_info.oldSwapchain          = old_swapchain;

	*swapchain_format = create_info.imageFormat;
	*swapchain_color_space = create_info.imageColorSpace;
	*swapchain_extent = extent;

	VkSwapchainKHR swapchain = nullptr;

	auto result = vkCreateSwapchainKHR(device, &create_info, nullptr, &swapchain);
	if (swapchain_create_result != nullptr)
	{
		*swapchain_create_result = result;
	}
	if (result != VK_SUCCESS)
	{
		printf("vkCreateSwapchainKHR failed: %s (%d)\n", string_VkResult(result), static_cast<int>(result));
		return nullptr;
	}

	result = vkGetSwapchainImagesKHR(device, swapchain, swapchain_images_count, nullptr);
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
	EXIT_NOT_IMPLEMENTED(*swapchain_images_count == 0);
	static std::atomic<uint64_t> swapchain_create_count {0};
	const uint64_t create_count =
	    swapchain_create_count.fetch_add(1, std::memory_order_relaxed) + 1;
	if ((Config::DiagnosticsEnabled() || WindowSwapchainTraceEnabled()) &&
	    ShouldSampleDiagnosticCounter(create_count))
	{
		dbg_printf("[VULKAN_SWAPCHAIN] event=create count=%" PRIu64
		           " preferred=%u create=%u actual=%u min=%u max=%u"
		           " extent=%ux%u mode=%d currentTransform=0x%x preTransform=0x%x\n",
		           create_count, preferred_image_count, image_count,
		           *swapchain_images_count,
		           r->capabilities.minImageCount,
		           r->capabilities.maxImageCount,
		           extent.width, extent.height,
		           static_cast<int>(create_info.presentMode),
		           static_cast<unsigned>(r->capabilities.currentTransform),
		           static_cast<unsigned>(create_info.preTransform));
	}

	*swapchain_images = new VkImage[*swapchain_images_count];
	result            = vkGetSwapchainImagesKHR(device, swapchain, swapchain_images_count, *swapchain_images);
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

	*swapchain_image_views = new VkImageView[*swapchain_images_count];
	for (uint32_t i = 0; i < *swapchain_images_count; i++)
	{
		VkImageViewCreateInfo create_info {};
		create_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		create_info.pNext                           = nullptr;
		create_info.flags                           = 0;
		create_info.image                           = (*swapchain_images)[i];
		create_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
		create_info.format                          = *swapchain_format;
		create_info.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
		create_info.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
		create_info.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
		create_info.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
		create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		create_info.subresourceRange.baseArrayLayer = 0;
		create_info.subresourceRange.baseMipLevel   = 0;
		create_info.subresourceRange.layerCount     = 1;
		create_info.subresourceRange.levelCount     = 1;

		result = vkCreateImageView(device, &create_info, nullptr, &((*swapchain_image_views)[i]));
		EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
	}

	return swapchain;
}

static void VulkanCreateQueues(GraphicContext* ctx, const VulkanQueues& queues)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->device == nullptr);
	EXIT_IF(queues.graphics.Size() != 1);
	EXIT_IF(queues.transfer.Size() != 1);
	EXIT_IF(queues.present.Size() != 1);
	EXIT_IF(!(queues.compute.Size() >= 1 && queues.compute.Size() <= GraphicContext::QUEUE_COMPUTE_NUM));

	auto get_queue = [ctx](int id, const QueueInfo& info)
	{
		Core::Mutex* mutex = nullptr;
		for (const auto& queue: ctx->queues)
		{
			if (queue.vk_queue != nullptr && queue.family == info.family && queue.index == info.index)
			{
				mutex = queue.mutex;
				break;
			}
		}

		ctx->queues[id].family = info.family;
		ctx->queues[id].index  = info.index;
		EXIT_IF(ctx->queues[id].vk_queue != nullptr);
		EXIT_IF(ctx->queues[id].mutex != nullptr);
		vkGetDeviceQueue(ctx->device, ctx->queues[id].family, ctx->queues[id].index, &ctx->queues[id].vk_queue);
		EXIT_NOT_IMPLEMENTED(ctx->queues[id].vk_queue == nullptr);

		if (mutex == nullptr)
		{
			mutex = new Core::Mutex;
		}
		ctx->queues[id].mutex = mutex;
	};

	get_queue(GraphicContext::QUEUE_GFX, queues.graphics.At(0));
	get_queue(GraphicContext::QUEUE_UTIL, queues.transfer.At(0));
	get_queue(GraphicContext::QUEUE_PRESENT, queues.present.At(0));

	for (int id = 0; id < GraphicContext::QUEUE_COMPUTE_NUM; id++)
	{
		get_queue(GraphicContext::QUEUE_COMPUTE_START + id, queues.compute.At(id % queues.compute.Size()));
	}
}

static VulkanSwapchain* VulkanCreateSwapchainForSurface(GraphicContext* ctx, VkSurfaceKHR surface,
                                                        SurfaceCapabilities* surface_capabilities,
                                                        uint32_t drawable_width, uint32_t drawable_height,
                                                        uint32_t image_count, VkSwapchainKHR old_swapchain = nullptr,
                                                        VkResult* swapchain_create_result = nullptr)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(surface == nullptr);
	EXIT_IF(surface_capabilities == nullptr);
	EXIT_IF(drawable_width == 0);
	EXIT_IF(drawable_height == 0);

	auto* s = new VulkanSwapchain;

	s->swapchain = VulkanCreateSwapchainInternal(
	    ctx->device, surface, drawable_width, drawable_height, image_count,
	    ctx->queues[GraphicContext::QUEUE_GFX].family, ctx->queues[GraphicContext::QUEUE_PRESENT].family,
	    surface_capabilities, old_swapchain, &s->swapchain_format, &s->swapchain_color_space, &s->swapchain_extent, &s->swapchain_images,
	    &s->swapchain_image_views, &s->swapchain_images_count, swapchain_create_result);
	if (s->swapchain == nullptr)
	{
		delete s;
		return nullptr;
	}

	s->current_index = static_cast<uint32_t>(-1);

	VkSemaphoreCreateInfo semaphore_info {};
	semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	semaphore_info.pNext = nullptr;
	semaphore_info.flags = 0;

	auto result = vkCreateSemaphore(ctx->device, &semaphore_info, nullptr, &s->acquire_semaphore);
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

	s->swapchain_image_layouts = new VkImageLayout[s->swapchain_images_count];
	s->submit_semaphores       = new VkSemaphore[s->swapchain_images_count] {};

	for (uint32_t i = 0; i < s->swapchain_images_count; i++)
	{
		s->swapchain_image_layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
		result                        = vkCreateSemaphore(ctx->device, &semaphore_info, nullptr, &s->submit_semaphores[i]);
		EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
	}

	return s;
}

static VulkanSwapchain* VulkanCreateSwapchain(GraphicContext* ctx, uint32_t image_count, VkSwapchainKHR old_swapchain = nullptr)
{
	EXIT_IF(g_window_ctx == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->screen_width == 0);
	EXIT_IF(ctx->screen_height == 0);

	Core::LockGuard lock(g_window_ctx->mutex);
	const uint32_t drawable_width  = g_window_ctx->drawable_width != 0 ? g_window_ctx->drawable_width : ctx->screen_width;
	const uint32_t drawable_height = g_window_ctx->drawable_height != 0 ? g_window_ctx->drawable_height : ctx->screen_height;
	return VulkanCreateSwapchainForSurface(ctx, g_window_ctx->surface, g_window_ctx->surface_capabilities,
	                                       drawable_width, drawable_height, image_count, old_swapchain);
}

static void VulkanDeleteSwapchain(GraphicContext* ctx, VulkanSwapchain* swapchain)
{
	EXIT_IF(ctx == nullptr);
	if (swapchain == nullptr)
	{
		return;
	}

	for (uint32_t i = 0; i < swapchain->swapchain_images_count; i++)
	{
		if (swapchain->swapchain_image_views != nullptr && swapchain->swapchain_image_views[i] != nullptr)
		{
			vkDestroyImageView(ctx->device, swapchain->swapchain_image_views[i], nullptr);
		}
		if (swapchain->submit_semaphores != nullptr && swapchain->submit_semaphores[i] != nullptr)
		{
			vkDestroySemaphore(ctx->device, swapchain->submit_semaphores[i], nullptr);
		}
	}
	if (swapchain->acquire_semaphore != nullptr)
	{
		vkDestroySemaphore(ctx->device, swapchain->acquire_semaphore, nullptr);
	}
	if (swapchain->swapchain != nullptr)
	{
		vkDestroySwapchainKHR(ctx->device, swapchain->swapchain, nullptr);
	}

	delete[] swapchain->swapchain_images;
	delete[] swapchain->swapchain_image_views;
	delete[] swapchain->swapchain_image_layouts;
	delete[] swapchain->submit_semaphores;
	delete swapchain;
}

static VkResult VulkanSwapchainQueuesWaitIdle(GraphicContext* ctx)
{
	EXIT_IF(ctx == nullptr);

	const int queue_ids[] = {GraphicContext::QUEUE_GFX, GraphicContext::QUEUE_PRESENT};
	VkQueue   waited[2]   = {};
	uint32_t  waited_count = 0;
	for (const int queue_id: queue_ids)
	{
		const auto& queue = ctx->queues[queue_id];
		EXIT_IF(queue.vk_queue == nullptr);
		if (queue.mutex == nullptr)
		{
			EXIT("Swapchain queue has no synchronization mutex\n");
		}

		bool duplicate = false;
		for (uint32_t i = 0; i < waited_count; i++)
		{
			if (waited[i] == queue.vk_queue)
			{
				duplicate = true;
				break;
			}
		}
		if (duplicate)
		{
			continue;
		}

		Core::LockGuard lock(*queue.mutex);
		const auto      result = vkQueueWaitIdle(queue.vk_queue);
		if (result != VK_SUCCESS)
		{
			return result;
		}
		waited[waited_count++] = queue.vk_queue;
	}
	return VK_SUCCESS;
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
static bool VulkanRecoverAndroidSurfaceAndSwapchain()
{
	EXIT_IF(g_window_ctx == nullptr);
	EXIT_IF(g_window_ctx->window == nullptr);
	EXIT_IF(g_window_ctx->surface == nullptr);
	EXIT_IF(g_window_ctx->surface_capabilities == nullptr);
	EXIT_IF(g_window_ctx->swapchain == nullptr);

	/* vkCreateSwapchainKHR retires oldSwapchain even when creation fails. Once
	   surface replacement starts, never acquire from the old chain again; retry
	   this whole transaction on later frames until a new native window is ready. */
	g_window_ctx->android_surface_recovery_pending.store(true, std::memory_order_release);

	auto*        ctx                 = &g_window_ctx->graphic_ctx;
	VkSurfaceKHR replacement_surface = nullptr;
	if (SDL_Vulkan_CreateSurface(g_window_ctx->window, ctx->instance, &replacement_surface) == SDL_FALSE)
	{
		printf("Android Vulkan surface recreation is not ready: %s\n", SDL_GetError());
		std::fflush(stdout);
		return false;
	}

	SurfaceCapabilities replacement_capabilities {};
	auto result = VulkanGetSurfaceCapabilities(ctx->physical_device, replacement_surface,
	                                           &replacement_capabilities);
	if (result != VK_SUCCESS)
	{
		printf("Android replacement surface capabilities failed: %s (%d)\n", string_VkResult(result),
		       static_cast<int>(result));
		vkDestroySurfaceKHR(ctx->instance, replacement_surface, nullptr);
		std::fflush(stdout);
		return false;
	}

	const auto present_family = ctx->queues[GraphicContext::QUEUE_PRESENT].family;
	EXIT_IF(present_family == static_cast<uint32_t>(-1));
	VkBool32 present_supported = VK_FALSE;
	result = vkGetPhysicalDeviceSurfaceSupportKHR(ctx->physical_device, present_family, replacement_surface,
	                                              &present_supported);
	if (result != VK_SUCCESS || present_supported != VK_TRUE ||
	    (replacement_capabilities.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
	{
		printf("Android replacement surface is incompatible: query=%s (%d), present=%u, transferDst=%u\n",
		       string_VkResult(result), static_cast<int>(result), static_cast<uint32_t>(present_supported),
		       (replacement_capabilities.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0 ? 1U : 0U);
		vkDestroySurfaceKHR(ctx->instance, replacement_surface, nullptr);
		std::fflush(stdout);
		return false;
	}

	int drawable_width  = 0;
	int drawable_height = 0;
	SDL_Vulkan_GetDrawableSize(g_window_ctx->window, &drawable_width, &drawable_height);
	const auto& current_extent = replacement_capabilities.capabilities.currentExtent;
	if (current_extent.width != UINT32_MAX && current_extent.height != UINT32_MAX)
	{
		drawable_width  = static_cast<int>(current_extent.width);
		drawable_height = static_cast<int>(current_extent.height);
	}
	if (drawable_width <= 0 || drawable_height <= 0)
	{
		printf("Android replacement surface has no drawable yet\n");
		vkDestroySurfaceKHR(ctx->instance, replacement_surface, nullptr);
		std::fflush(stdout);
		return false;
	}

	auto* replacement_swapchain = VulkanCreateSwapchainForSurface(
	    ctx, replacement_surface, &replacement_capabilities, static_cast<uint32_t>(drawable_width),
	    static_cast<uint32_t>(drawable_height), PREFERRED_SWAPCHAIN_IMAGE_COUNT);
	if (replacement_swapchain == nullptr)
	{
		vkDestroySurfaceKHR(ctx->instance, replacement_surface, nullptr);
		std::fflush(stdout);
		return false;
	}

	result = VulkanSwapchainQueuesWaitIdle(ctx);
	if (result != VK_SUCCESS)
	{
		printf("Android surface recovery queue idle failed: %s (%d)\n", string_VkResult(result),
		       static_cast<int>(result));
		VulkanDeleteSwapchain(ctx, replacement_swapchain);
		vkDestroySurfaceKHR(ctx->instance, replacement_surface, nullptr);
		std::fflush(stdout);
		return false;
	}

	auto* const old_swapchain = g_window_ctx->swapchain;
	const auto  old_surface   = g_window_ctx->surface;
	{
		Core::LockGuard lock(g_window_ctx->mutex);
		g_window_ctx->surface   = replacement_surface;
		g_window_ctx->swapchain = replacement_swapchain;
		*g_window_ctx->surface_capabilities = replacement_capabilities;
		g_window_ctx->drawable_width         = static_cast<uint32_t>(drawable_width);
		g_window_ctx->drawable_height        = static_cast<uint32_t>(drawable_height);
	}
	g_window_ctx->android_surface_recovery_pending.store(false, std::memory_order_release);

	VulkanDeleteSwapchain(ctx, old_swapchain);
	vkDestroySurfaceKHR(ctx->instance, old_surface, nullptr);
	printf("Android Vulkan surface recovered: %ux%u, images=%u\n", replacement_swapchain->swapchain_extent.width,
	       replacement_swapchain->swapchain_extent.height, replacement_swapchain->swapchain_images_count);
	__android_log_write(ANDROID_LOG_INFO, "LumenAndroidRuntime", "LUMEN_ANDROID_SURFACE_RECOVERY_V1 PASS");
	std::fflush(stdout);
	return true;
}
#endif

static bool VulkanRecreateSwapchain()
{
	EXIT_IF(g_window_ctx == nullptr);
	EXIT_IF(g_window_ctx->swapchain == nullptr);

	auto* ctx = &g_window_ctx->graphic_ctx;
	auto* old = g_window_ctx->swapchain;

	int drawable_width  = 0;
	int drawable_height = 0;
	SDL_Vulkan_GetDrawableSize(g_window_ctx->window, &drawable_width, &drawable_height);
	if (drawable_width <= 0 || drawable_height <= 0)
	{
		Core::LockGuard lock(g_window_ctx->mutex);
		g_window_ctx->drawable_width  = drawable_width > 0 ? static_cast<uint32_t>(drawable_width) : 0;
		g_window_ctx->drawable_height = drawable_height > 0 ? static_cast<uint32_t>(drawable_height) : 0;
		return false;
	}

	SurfaceCapabilities replacement_capabilities {};
	const auto capabilities_result =
	    VulkanGetSurfaceCapabilities(ctx->physical_device, g_window_ctx->surface, &replacement_capabilities);
	if (capabilities_result != VK_SUCCESS)
	{
		printf("Surface capabilities refresh failed: %s (%d)\n", string_VkResult(capabilities_result),
		       static_cast<int>(capabilities_result));
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		if (capabilities_result == VK_ERROR_SURFACE_LOST_KHR)
		{
			return VulkanRecoverAndroidSurfaceAndSwapchain();
		}
#endif
		return false;
	}
	const auto& extent = replacement_capabilities.capabilities.currentExtent;
	if (extent.width != UINT32_MAX && extent.height != UINT32_MAX)
	{
		drawable_width  = static_cast<int>(extent.width);
		drawable_height = static_cast<int>(extent.height);
	}
	if (drawable_width <= 0 || drawable_height <= 0)
	{
		Core::LockGuard lock(g_window_ctx->mutex);
		g_window_ctx->drawable_width  = drawable_width > 0 ? static_cast<uint32_t>(drawable_width) : 0;
		g_window_ctx->drawable_height = drawable_height > 0 ? static_cast<uint32_t>(drawable_height) : 0;
		return false;
	}

	auto result = VulkanSwapchainQueuesWaitIdle(ctx);
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

	VkResult replacement_create_result = VK_SUCCESS;
	auto* replacement = VulkanCreateSwapchainForSurface(
	    ctx, g_window_ctx->surface, &replacement_capabilities, static_cast<uint32_t>(drawable_width),
	    static_cast<uint32_t>(drawable_height), PREFERRED_SWAPCHAIN_IMAGE_COUNT, old->swapchain,
	    &replacement_create_result);
	if (replacement == nullptr)
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		if (VulkanResultRequiresSurfaceReplacement(replacement_create_result))
		{
			return VulkanRecoverAndroidSurfaceAndSwapchain();
		}
#endif
		// Once vkCreateSwapchainKHR has been called with oldSwapchain, the old
		// object may be retired even when creation fails. Retry without it; do
		// not attempt to acquire another image from a potentially retired chain.
		printf("Swapchain recreation with oldSwapchain failed; retrying without it\n");
		replacement_create_result = VK_SUCCESS;
		replacement = VulkanCreateSwapchainForSurface(
		    ctx, g_window_ctx->surface, &replacement_capabilities, static_cast<uint32_t>(drawable_width),
		    static_cast<uint32_t>(drawable_height), PREFERRED_SWAPCHAIN_IMAGE_COUNT, nullptr,
		    &replacement_create_result);
		if (replacement == nullptr)
		{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
			if (VulkanResultRequiresSurfaceReplacement(replacement_create_result))
			{
				return VulkanRecoverAndroidSurfaceAndSwapchain();
			}
#endif
			EXIT("Could not recreate swapchain for drawable %dx%d\n", drawable_width, drawable_height);
		}
	}

	{
		Core::LockGuard lock(g_window_ctx->mutex);
		g_window_ctx->swapchain = replacement;
		*g_window_ctx->surface_capabilities = replacement_capabilities;
		g_window_ctx->drawable_width         = static_cast<uint32_t>(drawable_width);
		g_window_ctx->drawable_height        = static_cast<uint32_t>(drawable_height);
	}
	VulkanDeleteSwapchain(ctx, old);
	printf("Swapchain recreated: %ux%u, images=%u\n", replacement->swapchain_extent.width,
	       replacement->swapchain_extent.height, replacement->swapchain_images_count);
	std::fflush(stdout);
	return true;
}

static void VulkanCreate(WindowContext* ctx)
{
	EXIT_IF(ctx->window == nullptr);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	// VkInstance/VkDevice and the queue-owned command pools are process-lifetime
	// on iOS. A later guest session gets a fresh UIKit surface and swapchain on
	// the already selected physical device.
	if (ctx->graphic_ctx.instance != nullptr)
	{
		EXIT_IF(ctx->graphic_ctx.physical_device == nullptr);
		EXIT_IF(ctx->graphic_ctx.device == nullptr);
		EXIT_IF(ctx->surface != nullptr);
		EXIT_IF(ctx->surface_capabilities != nullptr);
		EXIT_IF(ctx->swapchain != nullptr);

		if (SDL_Vulkan_CreateSurface(ctx->window, ctx->graphic_ctx.instance, &ctx->surface) == SDL_FALSE)
		{
			EXIT("Could not recreate the iOS Vulkan surface: %s\n", SDL_GetError());
		}

		if (!ctx->window_hidden || !WindowIOSShowRuntime(ctx->window, ctx->graphic_ctx.screen_width,
		                                               ctx->graphic_ctx.screen_height))
		{
			EXIT("Could not reactivate the opaque iOS runtime window\n");
		}
		ctx->window_hidden = false;

		int drawable_width  = 0;
		int drawable_height = 0;
		SDL_Vulkan_GetDrawableSize(ctx->window, &drawable_width, &drawable_height);
		if (drawable_width > 0 && drawable_height > 0)
		{
			ctx->drawable_width  = static_cast<uint32_t>(drawable_width);
			ctx->drawable_height = static_cast<uint32_t>(drawable_height);
		}
		printf("Vulkan drawable (reused device): %ux%u\n", ctx->drawable_width, ctx->drawable_height);

		ctx->surface_capabilities = new SurfaceCapabilities {};
		const auto capabilities_result =
		    VulkanGetSurfaceCapabilities(ctx->graphic_ctx.physical_device, ctx->surface, ctx->surface_capabilities);
		EXIT_NOT_IMPLEMENTED(capabilities_result != VK_SUCCESS);

		const auto present_family = ctx->graphic_ctx.queues[GraphicContext::QUEUE_PRESENT].family;
		EXIT_IF(present_family == static_cast<uint32_t>(-1));
		VkBool32 present_supported = VK_FALSE;
		const auto present_result =
		    vkGetPhysicalDeviceSurfaceSupportKHR(ctx->graphic_ctx.physical_device, present_family, ctx->surface, &present_supported);
		EXIT_NOT_IMPLEMENTED(present_result != VK_SUCCESS || present_supported != VK_TRUE);
		EXIT_NOT_IMPLEMENTED((ctx->surface_capabilities->capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0);

		ctx->swapchain = VulkanCreateSwapchain(&ctx->graphic_ctx, PREFERRED_SWAPCHAIN_IMAGE_COUNT);
		if (ctx->swapchain == nullptr)
		{
			EXIT("Could not recreate the iOS swapchain\n");
		}
		return;
	}
#endif

	EXIT_IF(ctx->graphic_ctx.instance != nullptr);
	EXIT_IF(ctx->graphic_ctx.physical_device != nullptr);
	EXIT_IF(ctx->graphic_ctx.device != nullptr);
	EXIT_IF(ctx->surface_capabilities != nullptr);

	VulkanExtensions r;
#if defined(LUMEN_VULKAN_DYNAMIC_DISPATCH)
	// SDL loads the configured driver when creating the Vulkan window. Its
	// surface adapter and the entire PS4 renderer must use that same GIPA.
	if (const auto* missing = VulkanDispatchLoadGlobal(
	        reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr())))
	{
		EXIT("Could not bind Vulkan loader dispatch: %s\n", missing);
	}
	std::fprintf(stderr, "[LUMEN_PS4_VULKAN_DISPATCH_V1] stage=global source=sdl2 complete=1\n");
#endif
	VulkanGetExtensions(ctx->window, &r);

	VkApplicationInfo app_info {};
	app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pNext              = nullptr;
	app_info.pApplicationName   = "Lumen emu";
	app_info.applicationVersion = 1;
	app_info.pEngineName        = "Lumen";
	app_info.engineVersion      = 1;
	app_info.apiVersion         = VK_API_VERSION_1_1; // NOLINT

	VkValidationFeatureDisableEXT disabled_features[] = {};
	VkValidationFeatureEnableEXT  enabled_features[]  = {
#if defined(LUMEN_ANDROID_VULKAN_VALIDATION)
	    VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
#endif
#ifdef LUMEN_ENABLE_BEST_PRACTICES
	    VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT,
#endif
#ifdef LUMEN_ENABLE_DEBUG_PRINTF
	    VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT,
#endif
	};

	uint32_t enabled_features_count = sizeof(enabled_features) / sizeof(VkValidationFeatureEnableEXT);

#ifdef LUMEN_ENABLE_DEBUG_PRINTF
	if (!Config::SpirvDebugPrintfEnabled())
	{
		enabled_features_count--;
	}
#endif

	VkValidationFeaturesEXT validation_features {};
	validation_features.sType                          = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
	validation_features.pNext                          = nullptr;
	validation_features.enabledValidationFeatureCount  = enabled_features_count;
	validation_features.pEnabledValidationFeatures     = enabled_features;
	validation_features.disabledValidationFeatureCount = 0;
	validation_features.pDisabledValidationFeatures    = disabled_features;

	VkDebugUtilsMessengerCreateInfoEXT dbg_create_info {};
	dbg_create_info.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	dbg_create_info.pNext           = &validation_features;
	dbg_create_info.flags           = 0;
	dbg_create_info.messageSeverity = static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) |
	                                  static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) |
	                                  static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) |
	                                  static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT);
	dbg_create_info.messageType = static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) |
	                              static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) |
	                              static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT);
	dbg_create_info.pfnUserCallback = VulkanDebugMessengerCallback;
	dbg_create_info.pUserData       = nullptr;

	VkInstanceCreateInfo inst_info {};
	inst_info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	inst_info.pNext                   = (r.enable_validation_layers ? &dbg_create_info : nullptr);
	inst_info.flags                   = 0;
	inst_info.pApplicationInfo        = &app_info;
	inst_info.enabledExtensionCount   = r.required_extensions.Size();
	inst_info.ppEnabledExtensionNames = r.required_extensions.GetDataConst();
	inst_info.enabledLayerCount       = (r.enable_validation_layers ? r.required_layers.Size() : 0);
	inst_info.ppEnabledLayerNames     = (r.enable_validation_layers ? r.required_layers.GetDataConst() : nullptr);

	VkResult result = vkCreateInstance(&inst_info, nullptr, &ctx->graphic_ctx.instance);
#if defined(LUMEN_ANDROID_VULKAN_VALIDATION)
	if (result == VK_SUCCESS)
	{
		std::fprintf(stderr, "[LUMEN_VULKAN_SYNCHRONIZATION_VALIDATION_V1] packaged=1 sync=1 core=1 errorLogCap=64 failFast=0 layer=1.4.357.0\n");
	}
#endif
	if (result == VK_ERROR_INCOMPATIBLE_DRIVER)
	{
		EXIT("Unable to find a compatible Vulkan Driver");
	} else if (result != VK_SUCCESS)
	{
		EXIT("Could not create a Vulkan instance (for unknown reasons)");
	}
#if defined(LUMEN_VULKAN_DYNAMIC_DISPATCH)
	if (const auto* missing = VulkanDispatchLoadInstance(ctx->graphic_ctx.instance))
	{
		EXIT("Could not bind Vulkan instance dispatch: %s\n", missing);
	}
	std::fprintf(stderr, "[LUMEN_PS4_VULKAN_DISPATCH_V1] stage=instance source=sdl2 complete=1\n");
#endif

	if (r.enable_validation_layers)
	{
		dbg_create_info.pNext = nullptr;
		if (VulkanCreateDebugUtilsMessengerEXT(ctx->graphic_ctx.instance, &dbg_create_info, nullptr, &ctx->graphic_ctx.debug_messenger) !=
		    VK_SUCCESS)
		{
			EXIT("Could not create debug messenger");
		}
	}

	if (SDL_Vulkan_CreateSurface(ctx->window, ctx->graphic_ctx.instance, &ctx->surface) == SDL_FALSE)
	{
		EXIT("Could not create a Vulkan surface");
	}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	// UIKit window mutations must happen on the process main thread. Resolve
	// the public SDL UIKit handle, install an opaque known background and prove
	// the key-window handoff before the guest thread can submit a frame.
	if (!ctx->window_hidden || !WindowIOSShowRuntime(ctx->window, ctx->graphic_ctx.screen_width,
	                                               ctx->graphic_ctx.screen_height))
	{
		EXIT("Could not activate the opaque iOS runtime window\n");
	}
	ctx->window_hidden = false;
#endif

	int drawable_width  = 0;
	int drawable_height = 0;
	SDL_Vulkan_GetDrawableSize(ctx->window, &drawable_width, &drawable_height);
	if (drawable_width > 0 && drawable_height > 0)
	{
		ctx->drawable_width  = static_cast<uint32_t>(drawable_width);
		ctx->drawable_height = static_cast<uint32_t>(drawable_height);
	}
	printf("Vulkan drawable: %ux%u\n", ctx->drawable_width, ctx->drawable_height);

	Vector<const char*> device_extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, "VK_KHR_maintenance1"};
#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID && LUMEN_PLATFORM != LUMEN_PLATFORM_OSX && LUMEN_PLATFORM != LUMEN_PLATFORM_IOS
	device_extensions.Add(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
#endif

#ifdef LUMEN_ENABLE_DEBUG_PRINTF
	if (Config::SpirvDebugPrintfEnabled())
	{
		device_extensions.Add(VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME);
	}
#endif

	ctx->surface_capabilities = new SurfaceCapabilities {};

	VulkanQueues queues;

	VulkanFindPhysicalDevice(ctx->graphic_ctx.instance, ctx->surface, device_extensions, ctx->surface_capabilities,
	                         &ctx->graphic_ctx.physical_device, &queues);

	if (ctx->graphic_ctx.physical_device == nullptr)
	{
		EXIT("Could not find suitable device");
	}

	VkPhysicalDeviceProperties device_properties {};
	vkGetPhysicalDeviceProperties(ctx->graphic_ctx.physical_device, &device_properties);

	printf("Select device: %s\n", device_properties.deviceName);
	#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	std::fprintf(stderr,
	             "[VULKAN_DEVICE_SELECTED] name=%s vendor=0x%04x device=0x%04x type=%u driver=%u api=%u\n",
	             device_properties.deviceName, device_properties.vendorID, device_properties.deviceID,
	             static_cast<unsigned>(device_properties.deviceType), device_properties.driverVersion,
	             device_properties.apiVersion);
	#endif
	#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	ctx->graphic_ctx.guest_d24s8_format =
	    CheckFormat(ctx->graphic_ctx.physical_device, VK_FORMAT_D24_UNORM_S8_UINT, true,
	                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
	        ? VK_FORMAT_D24_UNORM_S8_UINT
	        : VK_FORMAT_D32_SFLOAT_S8_UINT;
	#endif
	#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	std::fprintf(stderr, "[VULKAN_DEPTH_STENCIL_FORMAT] guest_d24s8=%s\n",
	             ctx->graphic_ctx.guest_d24s8_format == VK_FORMAT_D24_UNORM_S8_UINT ? "D24_UNORM_S8_UINT"
	                                                                                 : "D32_SFLOAT_S8_UINT");
	std::fflush(stderr);
	#elif LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	LUMEN_LOGI("[VULKAN_DEPTH_STENCIL_FORMAT] guest_d24s8=%s\n",
	           ctx->graphic_ctx.guest_d24s8_format == VK_FORMAT_D24_UNORM_S8_UINT ? "D24_UNORM_S8_UINT"
	                                                                               : "D32_SFLOAT_S8_UINT");
	#endif

	memcpy(ctx->device_name, device_properties.deviceName, sizeof(ctx->device_name));
	memcpy(ctx->processor_name, Core::GetSystemInfo().ProcessorName.C_Str(), sizeof(ctx->processor_name));

	ctx->graphic_ctx.device = VulkanCreateDevice(ctx->graphic_ctx.physical_device, ctx->surface, &r, queues, device_extensions);
	if (ctx->graphic_ctx.device == nullptr)
	{
		EXIT("Could not create device");
	}

	VulkanCreateQueues(&ctx->graphic_ctx, queues);

	ctx->swapchain = VulkanCreateSwapchain(&ctx->graphic_ctx, PREFERRED_SWAPCHAIN_IMAGE_COUNT);
	if (ctx->swapchain == nullptr)
	{
		EXIT("Could not create swapchain");
	}
}

void WindowInit(uint32_t width, uint32_t height)
{
	EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (g_window_ctx == nullptr)
	{
		g_window_ctx = new WindowContext;
		EXIT_IF(!g_window_ctx->ios_lifecycle.Initialize());
	} else
	{
		EXIT_IF(g_window_ctx->window != nullptr);
		EXIT_IF(g_window_ctx->surface != nullptr);
		EXIT_IF(g_window_ctx->surface_capabilities != nullptr);
		EXIT_IF(g_window_ctx->swapchain != nullptr);
		EXIT_IF(g_window_ctx->game != nullptr);
		EXIT_IF(g_window_ctx->graphic_initialized);
		EXIT_IF(!g_window_ctx->ios_lifecycle.IsDormant());
	}
#else
	EXIT_IF(g_window_ctx != nullptr);
	g_window_ctx = new WindowContext;
#endif

	g_window_ctx->graphic_ctx.screen_width  = width;
	g_window_ctx->graphic_ctx.screen_height = height;
	g_window_ctx->startup_first_present_reported = false;
}

void WindowWaitForGraphicInitialized()
{
	EXIT_IF(g_window_ctx == nullptr);

	Core::LockGuard lock(g_window_ctx->mutex);

	while (!g_window_ctx->graphic_initialized)
	{
		g_window_ctx->graphic_initialized_condvar.Wait(&g_window_ctx->mutex);
	}
}

bool WindowRequestExit()
{
	SDL_Event event {};
	event.type = SDL_QUIT;
	return SDL_PushEvent(&event) > 0;
}

void WindowRun()
{
	EXIT_IF(g_window_ctx == nullptr);
	Lumen::Emulator::StartupTrace::Emit("window", "run.begin");

	// LUMEN_PROFILER_THREAD("Thread_Window");
	EASY_MAIN_THREAD

	GameApi* api = nullptr;

	g_window_ctx->mutex.Lock();
	{
		EXIT_IF(g_window_ctx->graphic_initialized);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		EXIT_IF(!g_window_ctx->ios_lifecycle.BeginSession());
		SDL_FlushEvent(SDL_QUIT);
#endif

		WindowCreate(g_window_ctx);
		Lumen::Emulator::StartupTrace::Emit("window", "vulkan.begin");
		VulkanCreate(g_window_ctx);
		Lumen::Emulator::StartupTrace::Emit("window", "vulkan.end");
		api = game_create_api();

		g_window_ctx->game = api;

		g_window_ctx->graphic_initialized = true;
		// Every submitter waiting for this shared readiness predicate may resume.
		g_window_ctx->graphic_initialized_condvar.SignalAll();
		Lumen::Emulator::StartupTrace::Emit("window", "graphics.ready");
	}
	g_window_ctx->mutex.Unlock();

	game_main_loop(api, &g_window_ctx->graphic_ctx);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	WindowAndroidRuntimeInputReady(false);
#endif
	g_window_ctx->game = nullptr;
	game_delete_api(api);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	// SDL_QUIT ends the presentation loop; the guest interpreter observes this
	// request at its next bounded execution slice. The owning launch thread then
	// joins it and finalizes the iOS surface before returning to UIKit.
	Loader::RequestGuestExecutionStop();
	return;
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	// Android runs the emulator in its isolated :runtime process. Let the guest
	// thread unwind and Runtime.cpp destroy the subsystems before the entry point
	// performs the process-terminal exit.
	Loader::RequestGuestExecutionStop();
	return;
#else
#if defined(LUMEN_FEXCORE_RUNTIME)
	// The dedicated FEX product does not compile or execute the NODA backend.
	// Its disabled ABI stubs intentionally report zero coverage, so they must not
	// decide whether an otherwise clean FEX lifecycle exits successfully.
	constexpr bool x64_backend_finalized = true;
#else
	const bool dense_async_finalized =
	    Lumen::Cpu::X64FinalizeA64DynarecDenseAsyncCompilation();
	const bool stable_negative_route_finalized =
	    Lumen::Cpu::X64FinalizeA64DynarecStableNegativeRoute();
	const bool profitable_block_admission_finalized =
	    Lumen::Cpu::X64FinalizeA64DynarecProfitableBlockAdmission();
	const bool native_only_admission_finalized =
	    Lumen::Cpu::X64FinalizeA64DynarecNativeOnlyAdmission();
	const bool operation_snapshot_elision_finalized =
	    Lumen::Cpu::
	        X64FinalizeA64DynarecGuestMemoryOperationSnapshotElision();
	const bool scalar_memory_fallthrough_admission_finalized =
	    Lumen::Cpu::
	        X64FinalizeA64DynarecScalarMemoryFallthroughAdmission();
	const bool dead_rflags_elision_finalized =
	    Lumen::Cpu::X64FinalizeFastBlockDeadRflagsElision();
	const bool fast_block_dense_directory_finalized =
	    Lumen::Cpu::X64FinalizeFastBlockDenseDirectory();
	const bool fast_block_inline_fixed_signature_match_finalized =
	    Lumen::Cpu::X64FinalizeFastBlockInlineFixedSignatureMatch();
	const bool fast_block_unrolled_fixed_signature_batching_finalized =
	    Lumen::Cpu::X64FinalizeFastBlockUnrolledFixedSignatureBatching();
	const bool fast_block_idempotent_mru_store_elision_finalized =
	    Lumen::Cpu::X64FinalizeFastBlockIdempotentMruStoreElision();
	const bool fast_block_callback_free_ordinary_specialization_finalized =
	    Lumen::Cpu::
	        X64FinalizeFastBlockCallbackFreeOrdinarySpecialization();
#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
	const bool dense_profile_finalized =
	    Lumen::Cpu::X64FinalizeA64DynarecPhase6CKProfile();
#endif
#if defined(LUMEN_A64_DYNAREC_PHASE6CM_PROFILE)
	const bool ordinary_profile_finalized =
	    Lumen::Cpu::X64FinalizeA64DynarecPhase6CMProfile();
#endif
#if defined(LUMEN_A64_DYNAREC_PHASE6CO_PROFILE)
	const bool snapshot_profile_finalized =
	    Lumen::Cpu::X64FinalizeA64DynarecPhase6COProfile();
#endif
	const bool x64_backend_finalized =
	    dense_async_finalized && stable_negative_route_finalized &&
	    profitable_block_admission_finalized &&
	    native_only_admission_finalized &&
	    operation_snapshot_elision_finalized &&
	    scalar_memory_fallthrough_admission_finalized &&
	    dead_rflags_elision_finalized &&
	    fast_block_dense_directory_finalized &&
	    fast_block_inline_fixed_signature_match_finalized &&
	    fast_block_unrolled_fixed_signature_batching_finalized &&
	    fast_block_idempotent_mru_store_elision_finalized &&
	    fast_block_callback_free_ordinary_specialization_finalized &&
#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
	    dense_profile_finalized &&
#endif
#if defined(LUMEN_A64_DYNAREC_PHASE6CM_PROFILE)
	    ordinary_profile_finalized &&
#endif
#if defined(LUMEN_A64_DYNAREC_PHASE6CO_PROFILE)
	    snapshot_profile_finalized &&
#endif
	    true;
#endif
	const bool null_sampled_image_finalized = GraphicsRenderReportNullSampledImageFinal();
	const bool dynamic_vertex_finalized = VertexBufferReportDynamicHostVisibleFinal();
	const bool root_ownership_finalized = GraphicsRunReportRootOwnershipFinal();
	Core::SubsystemsListSingleton::Instance()->ShutdownAll();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	const bool metal_shader_function_cache_finalized =
	    LumenMetalFinalizeShaderFunctionCache == nullptr ||
	    LumenMetalFinalizeShaderFunctionCache();
	const bool metal_depth_stencil_state_cache_finalized =
	    LumenMetalFinalizeDepthStencilStateCache == nullptr ||
	    LumenMetalFinalizeDepthStencilStateCache();
	const bool metal_state_only_record_elision_finalized =
	    LumenMetalFinalizeStateOnlyRecordElision == nullptr ||
	    LumenMetalFinalizeStateOnlyRecordElision();
	const bool metal_completion_pass_reuse_finalized =
	    LumenMetalFinalizeCompletionPassReuse == nullptr ||
	    LumenMetalFinalizeCompletionPassReuse();
#else
	constexpr bool metal_shader_function_cache_finalized = true;
	constexpr bool metal_depth_stencil_state_cache_finalized = true;
	constexpr bool metal_state_only_record_elision_finalized = true;
	constexpr bool metal_completion_pass_reuse_finalized = true;
#endif
	Lumen::Emulator::StartupTrace::Flush();
	const bool finalization_clean =
	    x64_backend_finalized && null_sampled_image_finalized && dynamic_vertex_finalized &&
	    root_ownership_finalized && metal_shader_function_cache_finalized &&
	    metal_depth_stencil_state_cache_finalized && metal_state_only_record_elision_finalized &&
	    metal_completion_pass_reuse_finalized;
	std::_Exit(finalization_clean ? 0 : 74);
#endif
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
bool WindowDrainIOSRuntimeSession()
{
	if (g_window_ctx == nullptr || g_window_ctx->window == nullptr)
	{
		// Keep the bounded caller in its fail-closed path instead of terminating
		// the host process through an assertion during teardown.
		return true;
	}

	// Once stop begins, presenting a queued guest buffer can submit new Vulkan
	// work and race the bounded device-idle boundary. Cancel the queue instead:
	// VideoOut wakes every waiter and rejects refills until session reset.
	const auto result = VideoOut::VideoOutCancelIOSRuntimeFlips();
	SDL_PumpEvents();
	// Treat an invalid VideoOut state as still busy so the caller reaches its
	// deadline and poisons the session instead of proceeding with teardown.
	return result != VideoOut::VideoOutIOSFlipCancelResult::Idle;
}

bool WindowAbortIOSRuntimeSession()
{
	if (g_window_ctx == nullptr || g_window_ctx->window == nullptr)
	{
		return false;
	}
	// A guest that failed to quiesce may still own every SDL/Vulkan object.
	// Restore UIKit, but deliberately keep the presentation session intact; the
	// runtime boundary will poison the process instead of freeing live objects.
	return WindowIOSReturnToDashboard(g_window_ctx->window);
}

namespace {

struct IOSDeviceIdleWaitState
{
	VkDevice         device = nullptr;
	std::atomic_int  result = VK_NOT_READY;
	std::atomic_bool finished = false;
	std::atomic_uint references = 2;
};

void ReleaseIOSDeviceIdleWaitState(IOSDeviceIdleWaitState* state)
{
	if (state != nullptr && state->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
	{
		delete state;
	}
}

void* WaitForIOSDeviceIdle(void* data)
{
	auto* state = static_cast<IOSDeviceIdleWaitState*>(data);
	state->result.store(static_cast<int>(vkDeviceWaitIdle(state->device)), std::memory_order_relaxed);
	state->finished.store(true, std::memory_order_release);
	ReleaseIOSDeviceIdleWaitState(state);
	return nullptr;
}

} // namespace

bool WindowFinishIOSRuntimeSession()
{
	if (g_window_ctx == nullptr || g_window_ctx->window == nullptr)
	{
		std::fprintf(stderr, "iOS runtime presentation teardown has no active SDL window\n");
		std::fflush(stderr);
		return false;
	}

	constexpr auto drain_timeout = std::chrono::seconds(10);
	const auto     drain_deadline = std::chrono::steady_clock::now() + drain_timeout;
	auto fail_drain = [](const char* reason)
	{
		std::fprintf(stderr, "iOS runtime presentation teardown timed out while %s\n", reason);
		std::fflush(stderr);
		// Restore UIKit without releasing any resource that a worker may still
		// reference. The launch boundary will remain poisoned and fail closed.
		WindowIOSReturnToDashboard(g_window_ctx->window);
		return false;
	};

	auto* ctx = &g_window_ctx->graphic_ctx;
	if (ctx->device != nullptr)
	{
		// Do not call GraphicsRunWait here: its BufferWait path may initialize
		// command buffers when the title exits before its first submission. Poll
		// the existing rings and drain any flip they are waiting on instead.
		while (!GraphicsRunAreSubmitsAllowed())
		{
			if (std::chrono::steady_clock::now() >= drain_deadline)
			{
				return fail_drain("waiting for graphics submissions to quiesce");
			}
			WindowDrainIOSRuntimeSession();
		}
		while (WindowDrainIOSRuntimeSession())
		{
			if (std::chrono::steady_clock::now() >= drain_deadline)
			{
				return fail_drain("draining pending video flips");
			}
		}
		auto* idle_state = new IOSDeviceIdleWaitState;
		idle_state->device = ctx->device;
		pthread_t idle_thread {};
		if (pthread_create(&idle_thread, nullptr, WaitForIOSDeviceIdle, idle_state) != 0)
		{
			delete idle_state;
			return fail_drain("starting the bounded Vulkan device-idle wait");
		}
		while (!idle_state->finished.load(std::memory_order_acquire))
		{
			if (std::chrono::steady_clock::now() >= drain_deadline)
			{
				const int detach_result = pthread_detach(idle_thread);
				ReleaseIOSDeviceIdleWaitState(idle_state);
				std::fprintf(stderr, "iOS runtime Vulkan device-idle wait exceeded its deadline (detach=%d)\n",
				             detach_result);
				std::fflush(stderr);
				return fail_drain("waiting for the Vulkan device to become idle");
			}
			Core::Thread::SleepMicro(1000);
		}
		const int join_result = pthread_join(idle_thread, nullptr);
		const auto result      = static_cast<VkResult>(idle_state->result.load(std::memory_order_relaxed));
		ReleaseIOSDeviceIdleWaitState(idle_state);
		if (join_result != 0)
		{
			std::fprintf(stderr, "iOS runtime Vulkan device-idle thread join failed: %d\n", join_result);
			std::fflush(stderr);
			return fail_drain("joining the Vulkan device-idle wait");
		}
		if (result != VK_SUCCESS)
		{
			std::fprintf(stderr, "iOS runtime device idle wait failed during dashboard return: %s (%d)\n",
			             string_VkResult(result), static_cast<int>(result));
			std::fflush(stderr);
			// Keep every Vulkan/session object alive when idleness is not proven.
			// UIKit may still restore the dashboard, but the runtime stays poisoned
			// and cannot reuse or unmap resources that the device may reference.
			WindowIOSReturnToDashboard(g_window_ctx->window);
			return false;
		}
		VulkanDeleteSwapchain(ctx, g_window_ctx->swapchain);
		g_window_ctx->swapchain = nullptr;
	}

	const bool dashboard_restored = WindowIOSReturnToDashboard(g_window_ctx->window);
	if (g_window_ctx->surface != nullptr && ctx->instance != nullptr)
	{
		vkDestroySurfaceKHR(ctx->instance, g_window_ctx->surface, nullptr);
		g_window_ctx->surface = nullptr;
	}
	SDL_DestroyWindow(g_window_ctx->window);
	g_window_ctx->window = nullptr;
	const bool dashboard_finalized = WindowIOSFinalizeDashboardReturn();
	g_window_ctx->window_hidden = true;
	g_window_ctx->drawable_width = 0;
	g_window_ctx->drawable_height = 0;
	delete g_window_ctx->surface_capabilities;
	g_window_ctx->surface_capabilities = nullptr;
	g_window_ctx->last_title_update = std::chrono::steady_clock::time_point {};
	{
		Core::LockGuard lock(g_window_ctx->mutex);
		if (!g_window_ctx->ios_lifecycle.FinishSession())
		{
			std::fprintf(stderr, "iOS runtime window lifecycle could not enter the dormant state\n");
			std::fflush(stderr);
			return false;
		}
		g_window_ctx->graphic_initialized = false;
	}
	return dashboard_restored && dashboard_finalized;
}

#endif

VkSurfaceCapabilitiesKHR* VulkanGetSurfaceCapabilities()
{
	EXIT_IF(g_window_ctx == nullptr);

	Core::LockGuard lock(g_window_ctx->mutex);

	// auto* ctx = &g_window_ctx->graphic_ctx;

	return &g_window_ctx->surface_capabilities->capabilities;
}

GraphicContext* WindowGetGraphicContext()
{
	EXIT_IF(g_window_ctx == nullptr);

	Core::LockGuard lock(g_window_ctx->mutex);

	return &g_window_ctx->graphic_ctx;
}

bool WindowHasSdrOutput()
{
	if (g_window_ctx == nullptr) return false;
	// Match the draw/recreation lock order so a retired or replacing chain is
	// never mistaken for a live output. Initial creation holds mutex as well.
	Core::LockGuard swapchain_lock(g_window_ctx->swapchain_mutex);
	Core::LockGuard lock(g_window_ctx->mutex);
	if (!g_window_ctx->graphic_initialized) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (g_window_ctx->android_surface_recovery_pending.load(std::memory_order_acquire)) return false;
#endif
	const auto* swapchain = g_window_ctx->swapchain;
	return swapchain != nullptr && swapchain->swapchain != nullptr &&
	       IsSupportedSdrSurfaceFormat({swapchain->swapchain_format, swapchain->swapchain_color_space});
}

void WindowUpdateIcon()
{
	EXIT_IF(g_window_ctx == nullptr);
	EXIT_IF(g_window_ctx->window == nullptr);

	auto* icon = Loader::SystemContentGetIcon();

	if (icon != nullptr)
	{
		SDL_SetWindowIcon(g_window_ctx->window, static_cast<SDL_Surface*>(icon->GetSdlSurface()));
	}
}

void WindowUpdateTitle()
{
	EXIT_IF(g_window_ctx == nullptr);
	EXIT_IF(g_window_ctx->game == nullptr);

	const auto now = std::chrono::steady_clock::now();
	if (g_window_ctx->last_title_update != std::chrono::steady_clock::time_point {} &&
	    now - g_window_ctx->last_title_update < std::chrono::milliseconds(500))
	{
		return;
	}
	g_window_ctx->last_title_update = now;

	char title[128]   = {};
	char title_id[12] = {};
	char app_ver[8]   = {};
	const bool has_title    = Loader::SystemContentParamSfoGetString("TITLE", title, sizeof(title));
	const bool has_title_id = Loader::SystemContentParamSfoGetString("TITLE_ID", title_id, sizeof(title_id));
	const bool has_app_ver  = Loader::SystemContentParamSfoGetString("APP_VER", app_ver, sizeof(app_ver));

	auto fps = String::FromPrintf("%s%s%s%s%s%s[%s] [%s], frame: %d, fps: %f", (has_title ? title : ""), (has_title ? ", " : ""),
	                              (has_title_id ? title_id : ""), (has_title_id ? ", " : ""), (has_app_ver ? app_ver : ""),
	                              (has_app_ver ? ", " : ""), g_window_ctx->device_name, g_window_ctx->processor_name,
	                              g_window_ctx->game->m_frame_num, g_window_ctx->game->m_current_fps);

	SDL_SetWindowTitle(g_window_ctx->window, fps.C_Str());
}

static uint64_t GetPositiveEnvironmentValue(const char* name)
{
	const char* value = std::getenv(name);
	if (value == nullptr || value[0] == '\0')
	{
		return 0;
	}
	char*      end    = nullptr;
	const auto parsed = std::strtoull(value, &end, 10);
	const bool valid  = end != value && end != nullptr && end[0] == '\0' && parsed > 0;
	return valid ? static_cast<uint64_t>(parsed) : uint64_t {0};
}

static uint64_t GetVideoReadbackFrame()
{
	static const uint64_t target_frame = GetPositiveEnvironmentValue("LUMEN_VIDEOOUT_READBACK_FRAME");
	return target_frame;
}

static uint64_t GetVideoReadbackInterval()
{
	static const uint64_t interval = GetPositiveEnvironmentValue("LUMEN_VIDEOOUT_READBACK_INTERVAL");
	return interval;
}

static uint64_t GetVideoReadbackMaxFrame()
{
	static const uint64_t max_frame = GetPositiveEnvironmentValue("LUMEN_VIDEOOUT_READBACK_MAX_FRAME");
	return max_frame;
}

void WindowDrawBuffer(VideoOutVulkanImage* image)
{
	LUMEN_PROFILER_FUNCTION();
	Lumen::CriticalPath::Scope publication(Lumen::CriticalPath::Reason::WindowPublishFrame);
	const bool diagnostics_enabled = Config::DiagnosticsEnabled();
	const auto diagnostic_start    = diagnostics_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};

	EXIT_IF(image == nullptr);
	EXIT_IF(g_window_ctx == nullptr);
	EXIT_IF(g_window_ctx->swapchain == nullptr);
	DeferredStartupTraceFlush startup_trace_flush;
	Core::LockGuard swapchain_lock(g_window_ctx->swapchain_mutex);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (g_window_ctx->android_surface_recovery_pending.load(std::memory_order_acquire))
	{
		VulkanRecoverAndroidSurfaceAndSwapchain();
		return;
	}
#endif
	bool drawable_missing = false;
	{
		Core::LockGuard lock(g_window_ctx->mutex);
		drawable_missing = g_window_ctx->drawable_width == 0 || g_window_ctx->drawable_height == 0;
	}
	if (drawable_missing && (SDL_GetWindowFlags(g_window_ctx->window) & SDL_WINDOW_MINIMIZED) == 0)
	{
		// A restored window can keep the same logical size, so SDL need not
		// deliver another resize event after the drawable was cached as zero.
		int restored_width  = 0;
		int restored_height = 0;
		SDL_Vulkan_GetDrawableSize(g_window_ctx->window, &restored_width, &restored_height);
		if (restored_width > 0 && restored_height > 0)
		{
			Core::LockGuard lock(g_window_ctx->mutex);
			if (g_window_ctx->drawable_width == 0 || g_window_ctx->drawable_height == 0)
			{
				g_window_ctx->drawable_width  = static_cast<uint32_t>(restored_width);
				g_window_ctx->drawable_height = static_cast<uint32_t>(restored_height);
				if (WindowSwapchainTraceEnabled())
				{
					dbg_printf("[VULKAN_SWAPCHAIN] event=drawable_restored drawable=%dx%d\n",
					           restored_width, restored_height);
				}
			}
		}
	}
	bool     drawable_changed = false;
	uint32_t drawable_width   = 0;
	uint32_t drawable_height  = 0;
	uint32_t swapchain_width  = 0;
	uint32_t swapchain_height = 0;
	{
		Core::LockGuard lock(g_window_ctx->mutex);
		if (g_window_ctx->drawable_width == 0 || g_window_ctx->drawable_height == 0)
		{
			return;
		}
		drawable_width   = g_window_ctx->drawable_width;
		drawable_height  = g_window_ctx->drawable_height;
		swapchain_width  = g_window_ctx->swapchain->swapchain_extent.width;
		swapchain_height = g_window_ctx->swapchain->swapchain_extent.height;
		drawable_changed = swapchain_width != drawable_width ||
		                   swapchain_height != drawable_height;
	}
	if (drawable_changed && WindowSwapchainTraceEnabled())
	{
		static std::atomic<uint64_t> mismatch_count {0};
		const uint64_t count =
		    mismatch_count.fetch_add(1, std::memory_order_relaxed) + 1;
		if (ShouldSampleDiagnosticCounter(count))
		{
			dbg_printf("[VULKAN_SWAPCHAIN] event=drawable_mismatch count=%" PRIu64
			           " swapchain=%ux%u drawable=%ux%u\n",
			           count, swapchain_width, swapchain_height,
			           drawable_width, drawable_height);
		}
	}
	if (drawable_changed && !VulkanRecreateSwapchain())
	{
		return;
	}

	if (g_window_ctx->window_hidden)
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		EXIT("The iOS runtime window was not activated on the main thread\n");
#else
		WindowUpdateIcon();

		SDL_ShowWindow(g_window_ctx->window);

		g_window_ctx->window_hidden = false;
		Lumen::Emulator::StartupTrace::Emit("window", "shown.first_frame");
#endif
	}

	constexpr uint64_t AcquireTimeoutNs = 100'000'000ULL;
	VkResult           result           = VK_SUCCESS;
	for (uint32_t attempt = 0; attempt < 2; attempt++)
	{
		g_window_ctx->swapchain->current_index = static_cast<uint32_t>(-1);
		{
			Lumen::CriticalPath::Scope acquire(Lumen::CriticalPath::Reason::VulkanAcquire);
			result = vkAcquireNextImageKHR(g_window_ctx->graphic_ctx.device, g_window_ctx->swapchain->swapchain, AcquireTimeoutNs,
			                               g_window_ctx->swapchain->acquire_semaphore, nullptr,
			                               &g_window_ctx->swapchain->current_index);
		}
		if (result == VK_TIMEOUT || result == VK_NOT_READY)
		{
			return;
		}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		if (VulkanResultRequiresSurfaceReplacement(result))
		{
			printf("vkAcquireNextImageKHR: %s; rebuilding Android surface and swapchain\n", string_VkResult(result));
			VulkanRecoverAndroidSurfaceAndSwapchain();
			return;
		}
#endif
		if (result != VK_ERROR_OUT_OF_DATE_KHR)
		{
			break;
		}

		printf("vkAcquireNextImageKHR: %s; recreating swapchain\n", string_VkResult(result));
		if (!VulkanRecreateSwapchain())
		{
			return;
		}
		if (attempt + 1 == 2)
		{
			// The UIKit drawable can need another run-loop turn while rotating.
			// Drop this presentation and let the next video flip retry.
			return;
		}
	}

	const bool recreate_after_present = result == VK_SUBOPTIMAL_KHR;
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
	{
		EXIT("vkAcquireNextImageKHR failed: %s (%d)\n", string_VkResult(result), static_cast<int>(result));
	}
	EXIT_NOT_IMPLEMENTED(g_window_ctx->swapchain->current_index == static_cast<uint32_t>(-1));
	EXIT_NOT_IMPLEMENTED(g_window_ctx->swapchain->current_index >= g_window_ctx->swapchain->swapchain_images_count);

	auto* blt_src_image = image;
	auto* blt_dst_image = g_window_ctx->swapchain;

	EXIT_IF(blt_src_image == nullptr);
	EXIT_IF(blt_dst_image == nullptr);

	CommandBuffer buffer(GraphicContext::QUEUE_GFX);

	EXIT_NOT_IMPLEMENTED(buffer.IsInvalid());
	static uint64_t video_frame = 0;
	video_frame++;
	const uint64_t readback_frame = GetVideoReadbackFrame();
	const uint64_t readback_interval = GetVideoReadbackInterval();
	const uint64_t readback_max_frame = GetVideoReadbackMaxFrame();
	static bool visible_frame_observed = false;
	const bool readback_in_range = readback_frame != 0 && video_frame >= readback_frame &&
	                               (readback_max_frame == 0 || video_frame <= readback_max_frame);
	const bool readback_due = readback_in_range && !visible_frame_observed &&
	                          (readback_interval == 0 ? video_frame == readback_frame
	                                                  : (video_frame - readback_frame) % readback_interval == 0);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	const int guest_frame = g_window_ctx->game != nullptr ? g_window_ctx->game->m_frame_num : -1;
	if (readback_frame != 0 && (video_frame == 1 || video_frame % 300 == 0))
	{
		__android_log_print(ANDROID_LOG_INFO, "LumenAndroidRuntime",
		                    "LUMEN_ANDROID_FRAME_PROGRESS_V1 videoFrame=%" PRIu64
		                    " guestFrame=%d",
		                    video_frame, guest_frame);
	}
#endif
	const bool readback_format_supported = image->format == VK_FORMAT_R8G8B8A8_SRGB ||
	                                       image->format == VK_FORMAT_B8G8R8A8_SRGB ||
	                                       image->format == VK_FORMAT_R8G8B8A8_UNORM ||
	                                       image->format == VK_FORMAT_B8G8R8A8_UNORM;
	const uint64_t pixel_count = static_cast<uint64_t>(image->extent.width) * static_cast<uint64_t>(image->extent.height);
	const bool readback_extent_valid = pixel_count > 0 && pixel_count <= SIZE_MAX / sizeof(uint32_t);
	const bool capture_readback = readback_due && readback_format_supported &&
	                              readback_extent_valid;
	VulkanBuffer readback_buffer {};
	if (capture_readback)
	{
		readback_buffer.usage           = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		readback_buffer.memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		VulkanCreateBuffer(&g_window_ctx->graphic_ctx, pixel_count * sizeof(uint32_t), &readback_buffer);
	} else if (readback_due)
	{
		dbg_printf("[VIDEOOUT_READBACK] frame=%" PRIu64 " skipped format=%d extent=%ux%u\n", video_frame,
		           static_cast<int>(image->format), image->extent.width, image->extent.height);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		__android_log_print(ANDROID_LOG_WARN, "LumenAndroidRuntime",
		                    "LUMEN_ANDROID_VIDEOOUT_READBACK_V1 SKIP frame=%" PRIu64
		                    " format=%d width=%u height=%u",
		                    video_frame, static_cast<int>(image->format),
		                    image->extent.width, image->extent.height);
#endif
	}

	buffer.Begin();
	const uint64_t presentation_recording = buffer.GetRecordingId();
	if (capture_readback)
	{
		UtilImageToBuffer(&buffer, image, &readback_buffer, image->extent.width,
		                  static_cast<uint64_t>(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
	}

	UtilBlitImage(&buffer, blt_src_image, blt_dst_image,
	              SelectGuestAspectFit(LUMEN_PLATFORM == LUMEN_PLATFORM_IOS,
	                                   LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID));

	buffer.End();
	buffer.ExecuteWithSemaphores(g_window_ctx->swapchain->acquire_semaphore, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                             g_window_ctx->swapchain->submit_semaphores[g_window_ctx->swapchain->current_index]);
	if (capture_readback)
	{
		buffer.WaitForFence();
		void* data = nullptr;
		VulkanMapMemory(&g_window_ctx->graphic_ctx, &readback_buffer.memory, &data);
		const auto* bytes = static_cast<const uint8_t*>(data);
		uint64_t non_black = 0;
		uint64_t first_non_black = UINT64_MAX;
		uint64_t hash = 1469598103934665603ULL;
		for (uint64_t index = 0; index < pixel_count; index++)
		{
			const uint64_t offset = index * sizeof(uint32_t);
			if ((bytes[offset] | bytes[offset + 1] | bytes[offset + 2]) != 0)
			{
				non_black++;
				if (first_non_black == UINT64_MAX)
				{
					first_non_black = index;
				}
			}
			for (uint64_t channel = 0; channel < sizeof(uint32_t); channel++)
			{
				hash ^= bytes[offset + channel];
				hash *= 1099511628211ULL;
			}
		}
		uint32_t center = 0;
		std::memcpy(&center, bytes + (pixel_count / 2) * sizeof(uint32_t), sizeof(center));
		visible_frame_observed = non_black != 0;
		dbg_printf("[VIDEOOUT_READBACK] frame=%" PRIu64 " source=%ux%u format=%d layout=%d swapchain=%ux%u format=%d "
		           "rgb_nonzero=%" PRIu64 "/%" PRIu64 " first=%" PRIu64 " center=0x%08" PRIx32 " hash=0x%016" PRIx64 "\n",
		           video_frame, image->extent.width, image->extent.height, static_cast<int>(image->format),
		           static_cast<int>(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
		           g_window_ctx->swapchain->swapchain_extent.width, g_window_ctx->swapchain->swapchain_extent.height,
		           static_cast<int>(g_window_ctx->swapchain->swapchain_format), non_black, pixel_count, first_non_black, center, hash);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		__android_log_print(ANDROID_LOG_INFO, "LumenAndroidRuntime",
		                    "LUMEN_ANDROID_VIDEOOUT_READBACK_V1 PASS frame=%" PRIu64
		                    " guestFrame=%d nonBlack=%" PRIu64 " pixels=%" PRIu64
		                    " visible=%u final=%u center=0x%08" PRIx32
		                    " hash=0x%016" PRIx64,
		                    video_frame, guest_frame, non_black, pixel_count,
		                    visible_frame_observed ? 1U : 0U,
		                    (visible_frame_observed || video_frame == readback_max_frame) ? 1U : 0U,
		                    center, hash);
#endif
		VulkanUnmapMemory(&g_window_ctx->graphic_ctx, &readback_buffer.memory);
		VulkanDeleteBuffer(&g_window_ctx->graphic_ctx, &readback_buffer);
	}

	VkPresentInfoKHR present {};
	present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.pNext              = nullptr;
	present.swapchainCount     = 1;
	present.pSwapchains        = &g_window_ctx->swapchain->swapchain;
	present.pImageIndices      = &g_window_ctx->swapchain->current_index;
	present.pWaitSemaphores    = &g_window_ctx->swapchain->submit_semaphores[g_window_ctx->swapchain->current_index];
	present.waitSemaphoreCount = 1;
	present.pResults           = nullptr;

	const auto& queue = g_window_ctx->graphic_ctx.queues[GraphicContext::QUEUE_PRESENT];

	EXIT_IF(queue.mutex == nullptr);
	{
		Lumen::CriticalPath::Scope present_wait(Lumen::CriticalPath::Reason::VulkanPresent, 0, presentation_recording);
		Core::LockGuard lock(*queue.mutex);
		result = vkQueuePresentKHR(queue.vk_queue, &present);
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (VulkanResultRequiresSurfaceReplacement(result))
	{
		printf("vkQueuePresentKHR: %s; rebuilding Android surface and swapchain\n", string_VkResult(result));
		VulkanRecoverAndroidSurfaceAndSwapchain();
		return;
	}
#endif
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR && result != VK_ERROR_OUT_OF_DATE_KHR)
	{
		EXIT("vkQueuePresentKHR failed: %s (%d)\n", string_VkResult(result), static_cast<int>(result));
	}
	if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
	{
		// Present waits on the semaphore signalled by this exact blit submission.
		// This edge does not prove a unique guest frame or join other GPU/HLE work.
		(void)Lumen::CriticalPath::Frame(Lumen::CriticalPath::Reason::AcceptedPresent, video_frame, presentation_recording);
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
	{
		static uint64_t presented_frames = 0;
		WindowAndroidDidPresentFrame(++presented_frames);
		static std::atomic_bool first_present_reported {false};
		if (!first_present_reported.exchange(true, std::memory_order_relaxed))
		{
			__android_log_write(ANDROID_LOG_INFO, "LumenAndroidRuntime",
			                    "LUMEN_ANDROID_FIRST_PRESENT_V1 PASS");
		}
	}
#endif
	if (Lumen::Emulator::StartupTrace::Enabled() && !g_window_ctx->startup_first_present_reported &&
	    (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR))
	{
		g_window_ctx->startup_first_present_reported = true;
		Lumen::Emulator::StartupTrace::Emit("window", "present.first_success");
		startup_trace_flush.Arm();
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
#if !LUMEN_APPLE_NATIVE_METAL
	if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
	{
		static uint64_t presented_frames = 0;
		WindowIOSDidPresentFrame(++presented_frames);
	}
#endif
#endif
	const bool should_recreate = ShouldRecreateSwapchainAfterPresent(
	    LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID, recreate_after_present,
	    result);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (!should_recreate &&
	    (recreate_after_present || result == VK_SUBOPTIMAL_KHR) &&
	    WindowSwapchainTraceEnabled())
	{
		static std::atomic<uint64_t> deferred_suboptimal_count {0};
		const uint64_t count = deferred_suboptimal_count.fetch_add(
		                           1, std::memory_order_relaxed) +
		                       1;
		if (ShouldSampleDiagnosticCounter(count))
		{
			dbg_printf("[VULKAN_SWAPCHAIN] event=suboptimal_deferred count=%" PRIu64
			           " acquire=%u present=%d\n",
			           count, recreate_after_present ? 1U : 0U,
			           static_cast<int>(result));
		}
	}
#endif
	if (should_recreate)
	{
		printf("Swapchain became suboptimal/out-of-date (acquire=%s, present=%s); recreating\n",
		       recreate_after_present ? "VK_SUBOPTIMAL_KHR" : "VK_SUCCESS", string_VkResult(result));
		VulkanRecreateSwapchain();
	}

#if LUMEN_PLATFORM != LUMEN_PLATFORM_IOS
	WindowUpdateTitle();
#endif

	if (diagnostics_enabled)
	{
		static uint64_t diagnostic_frames = 0;
		static uint64_t diagnostic_us     = 0;
		diagnostic_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		                                                std::chrono::steady_clock::now() - diagnostic_start)
		                                                .count());
		diagnostic_frames++;
		if (diagnostic_frames == 1 || (diagnostic_frames % 120) == 0)
		{
			dbg_printf("[PERF] present frames=%" PRIu64 " avg=%.3f ms\n", diagnostic_frames,
			           static_cast<double>(diagnostic_us) / static_cast<double>(diagnostic_frames) / 1000.0);
		}
	}
}

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED
