#include "graphics/presentation/systemOverlay.h"

#include <SDL3/SDL.h>

#include "common/assert.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/graphicContext.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "libs/controller.h"
#include "libs/dialog.h"
#include "libs/ime.h"
#include "libs/imeDialog.h"
#include "ime/KeyboardPanel.h"
#if defined(LUMEN_PS5_ANDROID_ENGINE)
#include "AndroidPs5RuntimeUi.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace Libs::Graphics {

namespace {

namespace CoreIme   = Libs::Ime;
namespace DialogIme = Libs::Dialog::ImeDialog;
namespace ErrorDialog = Libs::Dialog::ErrorDialog;

bool MatchesActiveIme(uint64_t generation);

namespace Ime {

using Type           = ImeCommon::Type;
using EnterLabel     = ImeCommon::EnterLabel;
using Alignment      = ImeCommon::Alignment;
using ExternalAction = ImeCommon::ExternalAction;
using ExternalInput  = ImeCommon::ExternalInput;
using HostSnapshot   = ImeCommon::HostSnapshot;

constexpr uint32_t OPTION_MULTILINE            = ImeCommon::OPTION_MULTILINE;
constexpr uint32_t OPTION_NO_AUTO_CAPITALIZE   = ImeCommon::OPTION_NO_AUTO_CAPITALIZE;
constexpr uint32_t OPTION_FIXED_POSITION       = ImeCommon::OPTION_FIXED_POSITION;
constexpr uint32_t OPTION_DISABLE_POSITION_ADJ = ImeCommon::OPTION_DISABLE_POSITION_ADJUST;
constexpr uint32_t OPTION_USE_OVER_2K          = ImeCommon::OPTION_USE_OVER_2K;
constexpr uint32_t DISABLE_DEVICE_CONTROLLER   = ImeCommon::DISABLE_DEVICE_CONTROLLER;
constexpr uint32_t DISABLE_DEVICE_EXT_KEYBOARD = ImeCommon::DISABLE_DEVICE_EXT_KEYBOARD;
constexpr uint64_t CORE_GENERATION_BIT         = uint64_t {1} << 63;

uint64_t PackCoreGeneration(uint64_t generation) {
	return generation | CORE_GENERATION_BIT;
}

bool IsCoreGeneration(uint64_t generation) {
	return (generation & CORE_GENERATION_BIT) != 0;
}

uint64_t UnpackGeneration(uint64_t generation) {
	return generation & ~CORE_GENERATION_BIT;
}

bool GetHostSnapshot(HostSnapshot* snapshot) {
	if (snapshot == nullptr) {
		return false;
	}
	if (CoreIme::GetHostSnapshot(snapshot)) {
		snapshot->generation = PackCoreGeneration(snapshot->generation);
		return true;
	}
	return DialogIme::GetHostSnapshot(snapshot);
}

bool HostInsertText(uint64_t generation, std::u16string_view text) {
	if (!MatchesActiveIme(generation)) return false;
	return IsCoreGeneration(generation)
	           ? CoreIme::HostInsertText(UnpackGeneration(generation), text)
	           : DialogIme::HostInsertText(generation, text);
}

bool HostBackspace(uint64_t generation) {
	if (!MatchesActiveIme(generation)) return false;
	return IsCoreGeneration(generation) ? CoreIme::HostBackspace(UnpackGeneration(generation))
	                                    : DialogIme::HostBackspace(generation);
}

bool HostAccept(uint64_t generation) {
	if (!MatchesActiveIme(generation)) return false;
	return IsCoreGeneration(generation) ? CoreIme::HostAccept(UnpackGeneration(generation))
	                                    : DialogIme::HostAccept(generation);
}

bool HostCancel(uint64_t generation) {
	if (!MatchesActiveIme(generation)) return false;
	return IsCoreGeneration(generation) ? CoreIme::HostCancel(UnpackGeneration(generation))
	                                    : DialogIme::HostCancel(generation);
}

bool HostQueueExternalInput(uint64_t generation, ExternalInput input) {
	if (!MatchesActiveIme(generation)) return false;
	return IsCoreGeneration(generation)
	           ? CoreIme::HostQueueExternalInput(UnpackGeneration(generation), std::move(input))
	           : DialogIme::HostQueueExternalInput(generation, std::move(input));
}

bool HostMoveCursor(uint64_t generation, int delta) {
	if (!MatchesActiveIme(generation)) return false;
	if (!IsCoreGeneration(generation)) return DialogIme::HostMoveCursor(generation, delta);
	if (delta != -1 && delta != 1) return false;
	ExternalInput input;
	input.action = delta < 0 ? ExternalAction::MoveLeft : ExternalAction::MoveRight;
	return CoreIme::HostQueueExternalInput(UnpackGeneration(generation), std::move(input));
}

} // namespace Ime

enum class OverlayKind : uint8_t { None, Ime, Error };

struct OverlaySession {
	OverlayKind kind       = OverlayKind::None;
	uint64_t    generation = 0;

	bool operator==(const OverlaySession&) const = default;
};

struct OverlaySnapshot {
	OverlaySession            session;
	Ime::HostSnapshot         ime;
	ErrorDialog::HostSnapshot error;
};

bool GetOverlaySnapshot(OverlaySnapshot* snapshot) {
	if (ErrorDialog::GetHostSnapshot(&snapshot->error)) {
		snapshot->session = {OverlayKind::Error, snapshot->error.generation};
		return true;
	}
	if (Ime::GetHostSnapshot(&snapshot->ime)) {
		snapshot->session = {OverlayKind::Ime, snapshot->ime.generation};
		return true;
	}
	snapshot->session = {};
	return false;
}

bool MatchesActiveIme(uint64_t generation) {
	OverlaySnapshot snapshot;
	return GetOverlaySnapshot(&snapshot) && snapshot.session.kind == OverlayKind::Ime &&
	       snapshot.session.generation == generation;
}

constexpr size_t INPUT_QUEUE_CAPACITY = 128;

enum class InputKind : uint8_t {
	Button,
	Axis,
	MousePosition,
	MouseButton,
	MouseWheel,
	ResetController
};

struct InputEvent {
	InputKind      kind;
	OverlaySession session;
	int            id;
	float          x;
	float          y;
};

struct VisibilityUpdate {
	OverlaySession session;
	bool           visible;
	bool           capture_controller;
	bool           capture_keyboard;
	bool           text_input;
	bool           multiline;
};

std::atomic<Uint32>          g_visibility_event {static_cast<Uint32>(-1)};
std::mutex                   g_visibility_mutex;
std::mutex                   g_input_mutex;
std::deque<InputEvent>       g_input_events;
std::deque<VisibilityUpdate> g_visibility_updates;
size_t                       g_missing_visibility_wakeups = 0;
bool                         g_input_reset_requested      = false;
uint16_t                     g_last_external_keycode      = 0;
uint32_t                     g_last_external_status       = 0;
OverlaySession               g_input_session;
bool                         g_input_active               = false;
bool                         g_input_controller           = false;
bool                         g_input_keyboard             = false;
bool                         g_input_multiline            = false;
bool                         g_input_lifecycle_active     = false;
bool                         g_controller_captured        = false;
OverlaySession               g_session;
SDL_Window*                  g_input_window               = nullptr;
OverlaySession               g_preedit_session;
std::u16string               g_preedit;
#if defined(LUMEN_PS5_ANDROID_ENGINE)
std::atomic<Uint32>          g_android_sync_event {static_cast<Uint32>(-1)};
std::atomic<bool>            g_android_sync_pending {false};
SDL_TimerID                  g_android_sync_timer = 0;

Uint32 SDLCALL WakeAndroidImeSync(void*, SDL_TimerID, Uint32 interval) {
	const Uint32 type = g_android_sync_event.load(std::memory_order_acquire);
	if (type == static_cast<Uint32>(-1)) return 0;
	bool pending = false;
	if (!g_android_sync_pending.compare_exchange_strong(pending, true, std::memory_order_acq_rel)) return interval;
	SDL_Event event {};
	event.type = type;
	if (!SDL_PushEvent(&event)) g_android_sync_pending.store(false, std::memory_order_release);
	return interval;
}

void SyncAndroidImeOnMainThread() {
	if (g_input_window == nullptr) return;
	ImeCommon::HostSnapshot snapshot;
	SyncAndroidImeDialogUi(GetSystemImeHostSnapshot(&snapshot) ? &snapshot : nullptr);
}
#endif

void ClearInputEvents() {
	std::scoped_lock lock(g_input_mutex);
	g_input_events.clear();
	g_input_reset_requested = false;
	g_preedit_session = {};
	g_preedit.clear();
}

std::u16string GetPreedit(OverlaySession session) {
	std::scoped_lock lock(g_input_mutex);
	return g_preedit_session == session ? g_preedit : std::u16string {};
}

void QueueInput(InputEvent event) {
	std::scoped_lock lock(g_input_mutex);
	const bool       replaceable =
	    event.kind == InputKind::Axis || event.kind == InputKind::MousePosition;
	if (replaceable && !g_input_events.empty()) {
		auto& last = g_input_events.back();
		if (last.session == event.session && last.kind == event.kind && last.id == event.id) {
			last = event;
			return;
		}
	}
	if (g_input_events.size() == INPUT_QUEUE_CAPACITY) {
		g_input_events.clear();
		g_input_reset_requested = true;
	}
	g_input_events.push_back(event);
}

void RetryVisibilityWakeup() {
	const Uint32 type = g_visibility_event.load(std::memory_order_acquire);
	if (type == static_cast<Uint32>(-1)) {
		return;
	}
	std::scoped_lock lock(g_input_mutex);
	if (g_missing_visibility_wakeups == 0) {
		return;
	}
	SDL_Event event {};
	event.type = type;
	if (SDL_PushEvent(&event)) {
		g_missing_visibility_wakeups--;
	}
}

void RefreshVisibility() {
	std::scoped_lock visibility_lock(g_visibility_mutex);
	if (!g_input_lifecycle_active) {
		return;
	}
	OverlaySnapshot snapshot;
	const bool      visible = GetOverlaySnapshot(&snapshot);
	if (snapshot.session == g_session) {
		return;
	}
	g_session               = snapshot.session;
	bool capture_controller = false;
	bool capture_keyboard   = false;
	bool text_input         = false;
	bool multiline          = false;
	if (snapshot.session.kind == OverlayKind::Error) {
		capture_controller = true;
		capture_keyboard   = true;
	} else if (snapshot.session.kind == OverlayKind::Ime) {
		capture_controller = (snapshot.ime.disable_device & Ime::DISABLE_DEVICE_CONTROLLER) == 0;
		capture_keyboard   = (snapshot.ime.disable_device & Ime::DISABLE_DEVICE_EXT_KEYBOARD) == 0;
		text_input         = capture_keyboard;
		multiline          = (snapshot.ime.option & Ime::OPTION_MULTILINE) != 0;
	}
	const bool was_controller = std::exchange(g_controller_captured, capture_controller);
	if (capture_controller || was_controller) {
		Controller::ResetInputState();
	}
	const Uint32 type = g_visibility_event.load(std::memory_order_acquire);
	if (type != static_cast<Uint32>(-1)) {
		SDL_Event event {};
		event.type = type;
		std::scoped_lock input_lock(g_input_mutex);
		g_visibility_updates.push_back({snapshot.session, visible, capture_controller,
		                                capture_keyboard, text_input, multiline});
		if (g_missing_visibility_wakeups != 0 || !SDL_PushEvent(&event)) {
			g_missing_visibility_wakeups++;
		}
	}
}

void OnCoreVisibilityChanged(bool, uint64_t) {
	RefreshVisibility();
}

void OnDialogVisibilityChanged(bool, uint64_t) {
	RefreshVisibility();
}

uint32_t ExternalKeyStatus(SDL_Keymod modifiers, bool character_valid) {
	uint32_t status = 0x00000001 | (character_valid ? 0x00000002 : 0);
	if ((modifiers & SDL_KMOD_LCTRL) != 0) status |= 0x00000100;
	if ((modifiers & SDL_KMOD_LSHIFT) != 0) status |= 0x00000200;
	if ((modifiers & SDL_KMOD_LALT) != 0) status |= 0x00000400;
	if ((modifiers & SDL_KMOD_LGUI) != 0) status |= 0x00000800;
	if ((modifiers & SDL_KMOD_RCTRL) != 0) status |= 0x00001000;
	if ((modifiers & SDL_KMOD_RSHIFT) != 0) status |= 0x00002000;
	if ((modifiers & SDL_KMOD_RALT) != 0) status |= 0x00004000;
	if ((modifiers & SDL_KMOD_RGUI) != 0) status |= 0x00008000;
	if ((modifiers & SDL_KMOD_NUM) != 0) status |= 0x00010000;
	if ((modifiers & SDL_KMOD_CAPS) != 0) status |= 0x00020000;
	return status;
}

Ime::ExternalInput MakeExternalInput(Ime::ExternalAction action, uint16_t keycode,
                                     uint32_t status) {
	Ime::ExternalInput input {};
	input.key.keycode = keycode;
	input.key.status  = status;
	input.key.type    = 4;
	input.action      = action;
	return input;
}

std::u16string Utf8ToUtf16(std::string_view text) {
	std::u16string result;
	result.reserve(text.size());
	for (size_t i = 0; i < text.size();) {
		const auto first     = static_cast<uint8_t>(text[i++]);
		uint32_t   codepoint = 0;
		uint32_t   remaining = 0;
		if (first < 0x80) {
			codepoint = first;
		} else if ((first & 0xe0) == 0xc0) {
			codepoint = first & 0x1f;
			remaining = 1;
		} else if ((first & 0xf0) == 0xe0) {
			codepoint = first & 0x0f;
			remaining = 2;
		} else if ((first & 0xf8) == 0xf0) {
			codepoint = first & 0x07;
			remaining = 3;
		} else {
			return {};
		}
		const uint32_t minimum = remaining == 1 ? 0x80 : remaining == 2 ? 0x800 : remaining == 3 ? 0x10000 : 0;
		if (i + remaining > text.size()) {
			return {};
		}
		bool valid = true;
		for (uint32_t j = 0; j < remaining; j++) {
			const auto next = static_cast<uint8_t>(text[i++]);
			if ((next & 0xc0) != 0x80) {
				valid = false;
				break;
			}
			codepoint = (codepoint << 6) | (next & 0x3f);
		}
		if (!valid || codepoint < minimum || codepoint > 0x10ffff ||
		    (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
			return {};
		}
		if (codepoint <= 0xffff) {
			result.push_back(static_cast<char16_t>(codepoint));
		} else {
			codepoint -= 0x10000;
			result.push_back(static_cast<char16_t>(0xd800 + (codepoint >> 10)));
			result.push_back(static_cast<char16_t>(0xdc00 + (codepoint & 0x3ff)));
		}
	}
	return result;
}

float AlignmentPivot(Ime::Alignment alignment) {
	switch (alignment) {
		case Ime::Alignment::Start: return 0.0f;
		case Ime::Alignment::End: return 1.0f;
		default: return 0.5f;
	}
}

ImGuiKey ControllerButtonToKey(int button) {
	switch (button) {
		case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
		case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
		case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
		case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
		case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
		case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
		case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
		case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
		default: return ImGuiKey_None;
	}
}

PFN_vkVoidFunction LoadVulkanFunction(const char* name, void* user_data) {
	auto& graphics = *static_cast<GraphicContext*>(user_data);
	return graphics.instance.getProcAddr(name);
}

void CheckVulkanResult(VkResult result) {
	EXIT_IF(result != VK_SUCCESS);
}

} // namespace

bool GetSystemImeHostSnapshot(ImeCommon::HostSnapshot* snapshot) {
	if (snapshot == nullptr) return false;
	OverlaySnapshot current;
	if (!GetOverlaySnapshot(&current) || current.session.kind != OverlayKind::Ime) return false;
	*snapshot = std::move(current.ime);
	return true;
}

bool SystemImeHostReplaceText(uint64_t generation, std::u16string_view text, uint32_t cursor) {
	if (!MatchesActiveIme(generation)) return false;
	return Ime::IsCoreGeneration(generation)
	           ? CoreIme::HostReplaceText(Ime::UnpackGeneration(generation), text, cursor)
	           : DialogIme::HostReplaceText(generation, text, cursor);
}

bool SystemImeHostAccept(uint64_t generation) { return Ime::HostAccept(generation); }
bool SystemImeHostCancel(uint64_t generation) { return Ime::HostCancel(generation); }

void InitializeSystemOverlayInput(SDL_Window* window) {
	EXIT_IF(window == nullptr);
#if defined(LUMEN_PS5_ANDROID_ENGINE)
	const Uint32 type = SDL_RegisterEvents(2);
#else
	const Uint32 type = SDL_RegisterEvents(1);
#endif
	EXIT_IF(type == static_cast<Uint32>(-1));
	g_input_window = window;
	{
		std::scoped_lock lock(g_visibility_mutex);
		g_input_lifecycle_active = true;
		g_visibility_event.store(type, std::memory_order_release);
	}
	CoreIme::SetVisibilityCallback(OnCoreVisibilityChanged);
	DialogIme::SetVisibilityCallback(OnDialogVisibilityChanged);
	ErrorDialog::SetVisibilityCallback(RefreshVisibility);
	RefreshVisibility();
#if defined(LUMEN_PS5_ANDROID_ENGINE)
	// The ordinary host wait may block indefinitely. This callback only posts
	// an event; Activity/JNI publication always runs on the SDL main thread.
	g_android_sync_pending.store(false, std::memory_order_release);
	g_android_sync_event.store(type + 1, std::memory_order_release);
	g_android_sync_timer = SDL_AddTimer(100, WakeAndroidImeSync, nullptr);
	EXIT_IF(g_android_sync_timer == 0);
#endif
}

void ShutdownSystemOverlayInput() {
#if defined(LUMEN_PS5_ANDROID_ENGINE)
	g_android_sync_event.store(static_cast<Uint32>(-1), std::memory_order_release);
	if (g_android_sync_timer != 0) SDL_RemoveTimer(std::exchange(g_android_sync_timer, 0));
	g_android_sync_pending.store(false, std::memory_order_release);
	SyncAndroidImeDialogUi(nullptr);
#endif
	CoreIme::SetVisibilityCallback(nullptr);
	DialogIme::SetVisibilityCallback(nullptr);
	ErrorDialog::SetVisibilityCallback(nullptr);
	{
		std::scoped_lock lock(g_visibility_mutex);
		g_input_lifecycle_active = false;
		g_session                = {};
		if (std::exchange(g_controller_captured, false)) {
			Controller::ResetInputState();
		}
		g_visibility_event.store(static_cast<Uint32>(-1), std::memory_order_release);
	}
	ClearInputEvents();
	{
		std::scoped_lock lock(g_input_mutex);
		g_visibility_updates.clear();
		g_missing_visibility_wakeups = 0;
	}
	g_input_active     = false;
	g_input_session    = {};
	g_input_controller = false;
	g_input_keyboard   = false;
	g_input_multiline  = false;
	if (g_input_window != nullptr && SDL_TextInputActive(g_input_window)) {
		SDL_StopTextInput(g_input_window);
	}
	g_input_window = nullptr;
}

SystemOverlayVisualState GetSystemOverlayVisualState() noexcept {
	const auto core   = CoreIme::GetVisualState();
	const auto dialog = DialogIme::GetVisualState();
	const auto error  = ErrorDialog::GetVisualState();
#if defined(LUMEN_PS5_ANDROID_ENGINE)
	// Android's native text panel is the sole IME presentation owner. Keep the
	// existing Vulkan error overlay and revision refresh for its lifecycle.
	return {error.active,
#else
	return {core.active || dialog.active || error.active,
#endif
	        core.revision + dialog.revision + error.revision};
}

bool ProcessSystemOverlayInput(const SDL_Event& event) {
	RetryVisibilityWakeup();
#if defined(LUMEN_PS5_ANDROID_ENGINE)
	if (event.type == g_android_sync_event.load(std::memory_order_acquire)) {
		g_android_sync_pending.store(false, std::memory_order_release);
		SyncAndroidImeOnMainThread();
		return true;
	}
#endif
	if (event.type == g_visibility_event.load(std::memory_order_acquire)) {
		VisibilityUpdate update {};
		{
			std::scoped_lock lock(g_input_mutex);
			if (g_visibility_updates.empty()) {
				return true;
			}
			update = g_visibility_updates.front();
			g_visibility_updates.pop_front();
		}
		ClearInputEvents();
		g_last_external_keycode = 0;
		g_last_external_status  = 0;
		g_input_session         = update.session;
		g_input_active          = update.visible;
		g_input_controller      = update.capture_controller;
		g_input_keyboard        = update.capture_keyboard;
		g_input_multiline       = update.multiline;
#if defined(LUMEN_PS5_ANDROID_ENGINE)
		if (SDL_TextInputActive(g_input_window)) SDL_StopTextInput(g_input_window);
		SyncAndroidImeOnMainThread();
#else
		if (update.text_input) {
			SDL_StartTextInput(g_input_window);
		} else if (SDL_TextInputActive(g_input_window)) {
			SDL_StopTextInput(g_input_window);
		}
#endif
		SDL_FlushEvents(SDL_EVENT_TEXT_EDITING, SDL_EVENT_TEXT_INPUT);
		return true;
	}
	if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
		if (g_input_active && g_input_controller) {
			QueueInput({InputKind::ResetController, g_input_session, 0, 0.0f, 0.0f});
		}
		return false;
	}
	if (!g_input_active) {
		return false;
	}

	const auto     session          = g_input_session;
	const uint64_t generation       = session.generation;
	const bool     controller_event = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
	                                  event.type == SDL_EVENT_GAMEPAD_BUTTON_UP ||
	                                  event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION;
	if (controller_event && !g_input_controller) {
		return false;
	}
	const bool keyboard_event = event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_TEXT_EDITING ||
	                            event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP;
	if (keyboard_event && !g_input_keyboard) {
		return false;
	}
	if (keyboard_event && session.kind == OverlayKind::Error) {
		if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
		    (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)) {
			ErrorDialog::HostAccept(generation);
		}
		return true;
	}
	switch (event.type) {
		case SDL_EVENT_TEXT_INPUT: {
			{
				std::scoped_lock lock(g_input_mutex);
				g_preedit.clear();
				g_preedit_session = {};
			}
			const auto text = Utf8ToUtf16(event.text.text);
			if (!text.empty()) {
				auto input = MakeExternalInput(Ime::ExternalAction::Text, g_last_external_keycode,
				                               g_last_external_status | 0x00000002);
				input.key.character = text.front();
				input.text          = text;
				Ime::HostQueueExternalInput(generation, std::move(input));
			}
			return true;
		}
		case SDL_EVENT_TEXT_EDITING: {
			const auto text = Utf8ToUtf16(event.edit.text != nullptr ? event.edit.text : "");
			if (session.kind == OverlayKind::Ime && MatchesActiveIme(generation)) {
				std::scoped_lock lock(g_input_mutex);
				g_preedit = text;
				g_preedit_session = session;
			}
			return true;
		}
		case SDL_EVENT_KEY_DOWN: {
			g_last_external_keycode = static_cast<uint16_t>(event.key.scancode);
			g_last_external_status  = ExternalKeyStatus(event.key.mod, false);
			auto action = Ime::ExternalAction::Text;
			bool queue  = true;
			if (event.key.key == SDLK_BACKSPACE) {
				action = Ime::ExternalAction::Backspace;
			} else if (event.key.key == SDLK_LEFT) {
				action = Ime::ExternalAction::MoveLeft;
			} else if (event.key.key == SDLK_RIGHT) {
				action = Ime::ExternalAction::MoveRight;
			} else if (event.key.key == SDLK_ESCAPE) {
				action = Ime::ExternalAction::Cancel;
			} else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
				action =
				    g_input_multiline ? Ime::ExternalAction::Newline : Ime::ExternalAction::Accept;
			} else if (event.key.key == SDLK_TAB) {
				action = Ime::ExternalAction::None;
			} else {
				queue = false;
			}
			if (queue) {
				Ime::HostQueueExternalInput(
				    generation,
				    MakeExternalInput(action, g_last_external_keycode, g_last_external_status));
			}
			return true;
		}
		case SDL_EVENT_KEY_UP: return true;
		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		case SDL_EVENT_GAMEPAD_BUTTON_UP:
			QueueInput({InputKind::Button, session, event.gbutton.button,
			            event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? 1.0f : 0.0f, 0.0f});
			return true;
		case SDL_EVENT_GAMEPAD_AXIS_MOTION:
			QueueInput({InputKind::Axis, session, event.gaxis.axis,
			            static_cast<float>(event.gaxis.value), 0.0f});
			return true;
		case SDL_EVENT_MOUSE_MOTION:
			QueueInput({InputKind::MousePosition, session, 0, static_cast<float>(event.motion.x),
			            static_cast<float>(event.motion.y)});
			return true;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			QueueInput({InputKind::MousePosition, session, 0, static_cast<float>(event.button.x),
			            static_cast<float>(event.button.y)});
			QueueInput({InputKind::MouseButton, session, event.button.button,
			            event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 1.0f : 0.0f, 0.0f});
			return true;
		case SDL_EVENT_MOUSE_WHEEL:
			QueueInput({InputKind::MouseWheel, session, 0, static_cast<float>(event.wheel.x),
			            static_cast<float>(event.wheel.y)});
			return true;
		default: return false;
	}
}

struct SystemOverlay::Impl {
	explicit Impl(GraphicContext& context): graphics(context) {}

	~Impl() {
		ReleaseVulkan();
		if (imgui_context != nullptr) {
			ImGui::DestroyContext(imgui_context);
		}
	}

	void EnsureContext() {
		if (imgui_context != nullptr) {
			ImGui::SetCurrentContext(imgui_context);
			return;
		}
		IMGUI_CHECKVERSION();
		imgui_context = ImGui::CreateContext();
		ImGui::SetCurrentContext(imgui_context);
		auto& io       = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.LogFilename = nullptr;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
		io.ConfigNavCursorVisibleAlways = true;
		io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
		io.BackendPlatformName = "Kyty system overlay input";
		ImGui::StyleColorsDark();
		auto& style          = ImGui::GetStyle();
		style.WindowRounding = 10.0f;
		style.FrameRounding  = 6.0f;
		style.ItemSpacing    = {8.0f, 8.0f};
	}

	void EnsureVulkan(vk::Format format, uint32_t image_count) {
		EnsureContext();
		if (vulkan_initialized) {
			return;
		}
		EXIT_IF(image_count < 2);
		EXIT_IF(!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, LoadVulkanFunction, &graphics));

		const VkFormat            color_format = static_cast<VkFormat>(format);
		ImGui_ImplVulkan_InitInfo info {};
		info.ApiVersion                   = VK_API_VERSION_1_3;
		info.Instance                     = static_cast<VkInstance>(graphics.instance);
		info.PhysicalDevice               = static_cast<VkPhysicalDevice>(graphics.physical_device);
		info.Device                       = static_cast<VkDevice>(graphics.device);
		info.QueueFamily                  = graphics.queue_family;
		info.Queue                        = static_cast<VkQueue>(graphics.queue);
		info.DescriptorPoolSize           = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
		info.MinImageCount                = image_count;
		info.ImageCount                   = image_count;
		info.UseDynamicRendering          = true;
		info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
		info.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
		    VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
		info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount    = 1;
		info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
		info.CheckVkResultFn = CheckVulkanResult;
		EXIT_IF(!ImGui_ImplVulkan_Init(&info));
		vulkan_initialized = true;
	}

	void DrainInput(OverlaySession session) {
		std::deque<InputEvent> events;
		bool                   reset = false;
		{
			std::scoped_lock lock(g_input_mutex);
			events.swap(g_input_events);
			reset                   = g_input_reset_requested;
			g_input_reset_requested = false;
		}
		auto& io = ImGui::GetIO();
		if (reset) {
			io.ClearEventsQueue();
			io.ClearInputKeys();
			io.ClearInputMouse();
			right_stick = {};
		}
		for (const auto& event: events) {
			if (event.session != session) {
				continue;
			}
			switch (event.kind) {
				case InputKind::Button: {
					const bool down = event.x != 0.0f;
					if (session.kind == OverlayKind::Ime && down) {
						if (event.id == SDL_GAMEPAD_BUTTON_EAST) {
							Ime::HostCancel(session.generation);
						} else if (event.id == SDL_GAMEPAD_BUTTON_NORTH) {
							Ime::HostBackspace(session.generation);
						}
					}
					const ImGuiKey key = ControllerButtonToKey(event.id);
					if (key != ImGuiKey_None) {
						io.AddKeyEvent(key, down);
					}
					break;
				}
				case InputKind::Axis: {
					const float value = event.x / (event.x < 0.0f ? 32768.0f : 32767.0f);
					if (event.id == SDL_GAMEPAD_AXIS_LEFTX) {
						io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, value < -0.25f,
						                     std::max(-value, 0.0f));
						io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, value > 0.25f,
						                     std::max(value, 0.0f));
					} else if (event.id == SDL_GAMEPAD_AXIS_LEFTY) {
						io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, value < -0.25f,
						                     std::max(-value, 0.0f));
						io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, value > 0.25f,
						                     std::max(value, 0.0f));
					} else if (event.id == SDL_GAMEPAD_AXIS_RIGHTX) {
						right_stick.x = std::abs(value) > 0.2f ? value : 0.0f;
					} else if (event.id == SDL_GAMEPAD_AXIS_RIGHTY) {
						right_stick.y = std::abs(value) > 0.2f ? value : 0.0f;
					}
					break;
				}
				case InputKind::MousePosition: io.AddMousePosEvent(event.x, event.y); break;
				case InputKind::MouseButton: {
					int button = -1;
					if (event.id == SDL_BUTTON_LEFT) button = 0;
					if (event.id == SDL_BUTTON_RIGHT) button = 1;
					if (event.id == SDL_BUTTON_MIDDLE) button = 2;
					if (button >= 0) io.AddMouseButtonEvent(button, event.x != 0.0f);
					break;
				}
				case InputKind::MouseWheel: io.AddMouseWheelEvent(event.x, event.y); break;
				case InputKind::ResetController:
					io.ClearEventsQueue();
					io.ClearInputKeys();
					right_stick = {};
					break;
			}
		}
	}

	void DrawIme(const Ime::HostSnapshot& snapshot, vk::Extent2D extent) {
		const ImVec2 display(static_cast<float>(extent.width), static_cast<float>(extent.height));
		const bool   over_2k          = (snapshot.option & Ime::OPTION_USE_OVER_2K) != 0;
		const float  reference_width  = over_2k ? 3840.0f : 1920.0f;
		const float  reference_height = over_2k ? 2160.0f : 1080.0f;
		const float  scale_x          = display.x / reference_width;
		const float  scale_y          = display.y / reference_height;
		const float  width            = static_cast<float>(snapshot.panel_width) * scale_x;
		const float  height           = static_cast<float>(snapshot.panel_height) * scale_y;
		ui_scale                      = std::min(scale_x, scale_y) * (over_2k ? 2.0f : 1.0f);

		const ImVec2 pivot {AlignmentPivot(snapshot.horizontal_alignment),
		                    AlignmentPivot(snapshot.vertical_alignment)};
		if ((snapshot.option & Ime::OPTION_FIXED_POSITION) == 0) {
			const float movement = 600.0f * ImGui::GetIO().DeltaTime;
			panel_offset.x += right_stick.x * movement;
			panel_offset.y += right_stick.y * movement;
		}
		const ImVec2 base_position {snapshot.posx * scale_x - pivot.x * width,
		                            snapshot.posy * scale_y - pivot.y * height};
		ImVec2       position {base_position.x + panel_offset.x, base_position.y + panel_offset.y};
		if ((snapshot.option & Ime::OPTION_DISABLE_POSITION_ADJ) == 0) {
			position.x   = std::clamp(position.x, 0.0f, std::max(display.x - width, 0.0f));
			position.y   = std::clamp(position.y, 0.0f, std::max(display.y - height, 0.0f));
			panel_offset = {position.x - base_position.x, position.y - base_position.y};
		}
		ImGui::SetNextWindowPos(position, ImGuiCond_Always);
		ImGui::SetNextWindowSize({width, height}, ImGuiCond_Always);
		constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		                                   ImGuiWindowFlags_NoSavedSettings;
		ImGui::Begin("##Ime", nullptr, flags);

		const Lumen::ImeUi::KeyboardPanelActions actions {
			Ime::HostInsertText, SystemImeHostReplaceText, Ime::HostBackspace,
			Ime::HostMoveCursor, SystemImeHostAccept, SystemImeHostCancel};
		const auto preedit = GetPreedit({OverlayKind::Ime, snapshot.generation});
		Lumen::ImeUi::DrawKeyboardPanel(snapshot, preedit, actions, panel_state, ui_scale);
		ImGui::End();
	}

	void DrawError(const ErrorDialog::HostSnapshot& snapshot, vk::Extent2D extent) {
		const ImVec2 display(static_cast<float>(extent.width), static_cast<float>(extent.height));
		const float  scale = std::max(std::min(display.x / 1280.0f, display.y / 720.0f), 0.5f);
		ImGui::GetBackgroundDrawList()->AddRectFilled({0.0f, 0.0f}, display,
		                                              IM_COL32(0, 0, 0, 160));
		ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always,
		                        {0.5f, 0.5f});
		ImGui::SetNextWindowSize(
		    {std::max(std::min(620.0f * scale, display.x - 32.0f), 1.0f), 0.0f}, ImGuiCond_Always);
		if (focus_pending) {
			ImGui::SetNextWindowFocus();
		}
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24.0f * scale, 24.0f * scale});
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12.0f * scale, 16.0f * scale});
		ImGui::PushFont(nullptr, 20.0f * scale);
		constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		                                   ImGuiWindowFlags_NoSavedSettings |
		                                   ImGuiWindowFlags_AlwaysAutoResize;
		ImGui::Begin("##SystemError", nullptr, flags);
		ImGui::TextUnformatted("Error");
		ImGui::Separator();
		const auto error_code = static_cast<uint32_t>(snapshot.error_code);
		ImGui::TextWrapped("%s", error_code == 0x80550006u
		                             ? "You are not signed in to PlayStation Network."
		                             : "An error has occurred.");
		ImGui::TextDisabled("Error code: 0x%08X", error_code);
		const float button_width = std::min(140.0f * scale, ImGui::GetContentRegionAvail().x);
		ImGui::SetCursorPosX((ImGui::GetWindowSize().x - button_width) * 0.5f);
		const bool accepted = ImGui::Button("OK", {button_width, 44.0f * scale});
		if (focus_pending) {
			ImGui::SetItemDefaultFocus();
			focus_pending = false;
		}
		ImGui::End();
		ImGui::PopFont();
		ImGui::PopStyleVar(2);
		if (accepted) {
			ErrorDialog::HostAccept(snapshot.generation);
		}
	}

	bool PrepareFrame(vk::Extent2D frame_extent, vk::Format format, uint32_t image_count) {
		OverlaySnapshot snapshot;
		if (!GetOverlaySnapshot(&snapshot)) {
			return false;
		}
#if defined(LUMEN_PS5_ANDROID_ENGINE)
		if (snapshot.session.kind == OverlayKind::Ime) return false;
#endif
		const auto prepared_session = snapshot.session;
		EnsureVulkan(format, image_count);
		if (session != snapshot.session) {
			session       = snapshot.session;
			focus_pending = true;
			panel_state.Reset(snapshot.ime.generation,
			    (snapshot.ime.option & Ime::OPTION_NO_AUTO_CAPITALIZE) == 0, true);
			panel_offset  = {};
			right_stick   = {};
			auto& io      = ImGui::GetIO();
			io.ClearEventsQueue();
			io.ClearInputKeys();
			io.ClearInputMouse();
		}
		DrainInput(snapshot.session);

		auto& io       = ImGui::GetIO();
		io.DisplaySize = {static_cast<float>(frame_extent.width),
		                  static_cast<float>(frame_extent.height)};
		const auto now = std::chrono::steady_clock::now();
		io.DeltaTime   = last_frame == std::chrono::steady_clock::time_point {}
		                     ? 1.0f / 60.0f
		                     : std::clamp(std::chrono::duration<float>(now - last_frame).count(),
		                                  1.0f / 1000.0f, 0.1f);
		last_frame     = now;
		ImGui_ImplVulkan_NewFrame();
		ImGui::NewFrame();
		if (!GetOverlaySnapshot(&snapshot) || snapshot.session != prepared_session) {
			ImGui::EndFrame();
			return false;
		}
		if (snapshot.session.kind == OverlayKind::Error) {
			DrawError(snapshot.error, frame_extent);
		} else {
			DrawIme(snapshot.ime, frame_extent);
		}
		ImGui::Render();
		extent = frame_extent;
		return true;
	}

	void Record(vk::CommandBuffer command, vk::ImageView target) {
		vk::RenderingAttachmentInfo color {};
		color.sType       = vk::StructureType::eRenderingAttachmentInfo;
		color.imageView   = target;
		color.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
		color.loadOp      = vk::AttachmentLoadOp::eLoad;
		color.storeOp     = vk::AttachmentStoreOp::eStore;
		vk::RenderingInfo rendering {};
		rendering.sType                = vk::StructureType::eRenderingInfo;
		rendering.renderArea.extent    = extent;
		rendering.layerCount           = 1;
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachments    = &color;
		command.beginRendering(rendering);
		{
			Common::LockGuard queue_lock(graphics.queue_mutex);
			ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),
			                                static_cast<VkCommandBuffer>(command));
		}
		command.endRendering();
	}

	void ReleaseVulkan() {
		if (!vulkan_initialized) {
			return;
		}
		ImGui::SetCurrentContext(imgui_context);
		ImGui_ImplVulkan_Shutdown();
		vulkan_initialized = false;
	}

	GraphicContext&                       graphics;
	ImGuiContext*                         imgui_context      = nullptr;
	bool                                  vulkan_initialized = false;
	Lumen::ImeUi::KeyboardPanelState        panel_state;
	bool                                  focus_pending      = true;
	float                                 ui_scale           = 1.0f;
	ImVec2                                panel_offset {};
	ImVec2                                right_stick {};
	OverlaySession                        session;
	vk::Extent2D                          extent {};
	std::chrono::steady_clock::time_point last_frame;
};

SystemOverlay::SystemOverlay(GraphicContext& graphics): m_impl(std::make_unique<Impl>(graphics)) {}

SystemOverlay::~SystemOverlay() = default;

bool SystemOverlay::PrepareFrame(vk::Extent2D extent, vk::Format format, uint32_t image_count) {
	return m_impl->PrepareFrame(extent, format, image_count);
}

void SystemOverlay::Record(vk::CommandBuffer command, vk::ImageView target) {
	m_impl->Record(command, target);
}

void SystemOverlay::ReleaseVulkan() {
	m_impl->ReleaseVulkan();
}

} // namespace Libs::Graphics
