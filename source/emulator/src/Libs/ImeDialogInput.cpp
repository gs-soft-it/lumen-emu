// SPDX-License-Identifier: GPL-2.0-only
#include "Emulator/Libs/ImeDialogInput.h"
#include "Emulator/Libs/ImeDialog.h"
#include "Emulator/Libs/ImeDialogDesktopUi.h"
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#include "Emulator/Libs/ImeDialogAndroidUi.h"
#endif
#include "Lumen/Core/String.h"

#ifdef LUMEN_EMU_ENABLED

#include "SDL.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

namespace Lumen::Libs::Dialog::ImeDialog {
namespace {
std::atomic<uint32_t> g_wake_event {UINT32_MAX};
uint64_t g_generation = 0;
bool g_started_text_input = false;
std::array<uint64_t, SDL_NUM_SCANCODES> g_consumed_keys {};
std::array<bool, SDL_NUM_SCANCODES> g_forwarded_keys {};
std::mutex g_preedit_mutex;
std::u16string g_preedit;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
struct SavedHint {
	const char* name;
	std::string value;
	bool present = false;
};
std::array<SavedHint, 4> g_editor_hints {{{"SDL_ANDROID_IME_INPUT_TYPE", {}},
    {"SDL_IME_NO_PERSONALIZED_LEARNING", {}}, {SDL_HINT_RETURN_KEY_HIDES_IME, {}},
    {"SDL_ANDROID_IME_BACK_KEY", {}}}};
bool g_hints_saved = false;

void RestoreEditorHints()
{
	if (!g_hints_saved) return;
	for (const auto& hint: g_editor_hints)
	{
		if (hint.present) SDL_SetHintWithPriority(hint.name, hint.value.c_str(), SDL_HINT_OVERRIDE);
		else SDL_ResetHint(hint.name);
	}
	g_hints_saved = false;
}

void ConfigureEditorHints(const HostSnapshot& snapshot)
{
	if (!g_hints_saved)
	{
		for (auto& hint: g_editor_hints)
		{
			const char* value = SDL_GetHint(hint.name);
			hint.present = value != nullptr;
			hint.value = value != nullptr ? value : "";
		}
		g_hints_saved = true;
	}
	const bool password = (snapshot.option & 4u) != 0;
	const char* type = "text";
	if (snapshot.type == Type::Number) type = password ? "number-password" : "number";
	else if (password) type = "password";
	else if (snapshot.type == Type::Url) type = "url";
	else if (snapshot.type == Type::Mail) type = "mail";
	else if ((snapshot.option & 1u) != 0) type = "text-multiline";
	SDL_SetHintWithPriority(g_editor_hints[0].name, type, SDL_HINT_OVERRIDE);
	SDL_SetHintWithPriority(g_editor_hints[1].name, password || (snapshot.option & 0x20u) != 0 ? "1" : "0", SDL_HINT_OVERRIDE);
	SDL_SetHintWithPriority(g_editor_hints[2].name, "0", SDL_HINT_OVERRIDE);
	SDL_SetHintWithPriority(g_editor_hints[3].name, "escape", SDL_HINT_OVERRIDE);
}
#endif

void WakeWindow(bool, uint64_t)
{
	const uint32_t type = g_wake_event.load(std::memory_order_acquire);
	if (type == UINT32_MAX) return;
	SDL_Event wake {};
	wake.type = type;
	SDL_PushEvent(&wake);
}

bool DecodeUtf8(const char* bytes, std::u16string* output)
{
	output->clear();
	const auto* data = reinterpret_cast<const unsigned char*>(bytes);
	const size_t count = std::strlen(bytes);
	for (size_t i = 0; i < count;)
	{
		uint32_t value = data[i++];
		uint32_t extra = 0, minimum = 0;
		if (value >= 0xc2 && value <= 0xdf) { extra = 1; value &= 0x1f; minimum = 0x80; }
		else if (value >= 0xe0 && value <= 0xef) { extra = 2; value &= 0x0f; minimum = 0x800; }
		else if (value >= 0xf0 && value <= 0xf4) { extra = 3; value &= 0x07; minimum = 0x10000; }
		else if (value >= 0x80) return false;
		if (extra > count - i) return false;
		for (uint32_t n = 0; n < extra; ++n)
		{
			if ((data[i] & 0xc0) != 0x80) return false;
			value = (value << 6) | (data[i++] & 0x3f);
		}
		if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
		if (value < 0x10000) output->push_back(static_cast<char16_t>(value));
		else
		{
			value -= 0x10000;
			output->push_back(static_cast<char16_t>(0xd800 + (value >> 10)));
			output->push_back(static_cast<char16_t>(0xdc00 + (value & 0x3ff)));
		}
	}
	return true;
}

void SetPreedit(std::u16string text)
{
	std::lock_guard lock(g_preedit_mutex);
	g_preedit = std::move(text);
}
} // namespace

void InitSdlInput()
{
	g_generation = 0;
	g_consumed_keys.fill(0);
	g_forwarded_keys.fill(false);
	g_wake_event.store(SDL_RegisterEvents(1), std::memory_order_release);
	SetVisibilityCallback(WakeWindow);
}

void CloseSdlInput()
{
	SetVisibilityCallback(nullptr);
	g_wake_event.store(UINT32_MAX, std::memory_order_release);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	SyncAndroidImeDialog(nullptr);
#else
	CloseDesktopUi();
#endif
	if (g_started_text_input) SDL_StopTextInput();
	g_started_text_input = false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	RestoreEditorHints();
#endif
	SDL_FlushEvents(SDL_TEXTEDITING, SDL_TEXTINPUT);
	g_generation = 0;
	g_consumed_keys.fill(0);
	g_forwarded_keys.fill(false);
	SetPreedit({});
}

void SyncSdlInput(SDL_Window* window)
{
	HostSnapshot snapshot;
	const bool active = GetHostSnapshot(&snapshot);
	if (window != nullptr)
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		SyncAndroidImeDialog(active ? &snapshot : nullptr);
#else
		SyncDesktopUi(active ? &snapshot : nullptr, window);
#endif
	}
	if (active && g_generation != snapshot.generation)
	{
		// SDL text events have no session token. Discard input queued before
		// this editor became the active owner of the platform text stream.
		SDL_FlushEvents(SDL_TEXTEDITING, SDL_TEXTINPUT);
		g_generation = snapshot.generation;
		SetPreedit({});
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		// The native dialog owns a real EditText. Do not open SDL's hidden
		// editor as a second keyboard owner underneath it.
		if (SDL_IsTextInputActive() == SDL_TRUE) SDL_StopTextInput();
		ConfigureEditorHints(snapshot);
#else
		if (SDL_IsTextInputActive() == SDL_FALSE)
		{
			SDL_StartTextInput();
			g_started_text_input = true;
		}
		SDL_Rect rectangle {0, 0, 1, 1};
		SDL_SetTextInputRect(&rectangle);
#endif
	}
	else if (!active && g_generation != 0)
	{
		if (g_started_text_input) SDL_StopTextInput();
		g_started_text_input = false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		RestoreEditorHints();
#endif
		SDL_FlushEvents(SDL_TEXTEDITING, SDL_TEXTINPUT);
		g_generation = 0;
		SetPreedit({});
	}
}

bool ProcessSdlEvent(const SDL_Event& event)
{
	if (event.type == g_wake_event.load(std::memory_order_acquire)) return true;
#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID
	if (ProcessDesktopUi(event)) return true;
#endif
	const bool key = event.type == SDL_KEYDOWN || event.type == SDL_KEYUP;
	const bool tracked_key = key && event.key.keysym.scancode >= 0 && event.key.keysym.scancode < SDL_NUM_SCANCODES;
	if (event.type == SDL_KEYUP && tracked_key)
	{
		g_forwarded_keys[event.key.keysym.scancode] = false;
		if (g_consumed_keys[event.key.keysym.scancode])
		{
			g_consumed_keys[event.key.keysym.scancode] = 0;
			return true;
		}
	}
	HostSnapshot snapshot;
	if (!GetHostSnapshot(&snapshot))
	{
		if (event.type == SDL_KEYDOWN && tracked_key && g_consumed_keys[event.key.keysym.scancode] != 0) return true;
		if (event.type == SDL_KEYDOWN && tracked_key) g_forwarded_keys[event.key.keysym.scancode] = true;
		return false;
	}
	// Repeats belong to the editor which consumed the original press. They
	// remain useful in that session, but cannot become guest/new-editor input.
	if (event.type == SDL_KEYDOWN && tracked_key && g_consumed_keys[event.key.keysym.scancode] != 0 &&
	    g_consumed_keys[event.key.keysym.scancode] != snapshot.generation) return true;
	// A key pressed before the dialog still needs its original release routed.
	// Its auto-repeat cannot transfer ownership to the text editor.
	if (event.type == SDL_KEYDOWN && tracked_key && g_forwarded_keys[event.key.keysym.scancode]) return true;
	if (event.type == SDL_TEXTEDITING)
	{
		if (snapshot.generation != g_generation) return true;
		std::u16string text;
		if (DecodeUtf8(event.edit.text, &text)) SetPreedit(std::move(text));
		return true;
	}
	if (event.type == SDL_TEXTINPUT)
	{
		if (snapshot.generation != g_generation) return true;
		SetPreedit({});
		std::u16string text;
		if (DecodeUtf8(event.text.text, &text))
		{
			const bool accept = (snapshot.option & 1u) == 0 && !text.empty() &&
			    (text.back() == u'\n' || text.back() == u'\r');
			if (accept) text.pop_back();
			if (!text.empty()) HostInsertText(snapshot.generation, text);
			if (accept) HostAccept(snapshot.generation);
		}
		return true;
	}
	if (event.type != SDL_KEYDOWN) return false;
	if (event.key.keysym.scancode >= 0 && event.key.keysym.scancode < SDL_NUM_SCANCODES)
		g_consumed_keys[event.key.keysym.scancode] = snapshot.generation;
	switch (event.key.keysym.sym)
	{
		case SDLK_BACKSPACE: HostBackspace(snapshot.generation); break;
		case SDLK_LEFT: HostMoveCursor(snapshot.generation, -1); break;
		case SDLK_RIGHT: HostMoveCursor(snapshot.generation, 1); break;
		case SDLK_HOME: HostMoveCursor(snapshot.generation, -static_cast<int>(snapshot.max_text_length)); break;
		case SDLK_END: HostMoveCursor(snapshot.generation, static_cast<int>(snapshot.max_text_length)); break;
		case SDLK_ESCAPE: HostCancel(snapshot.generation); break;
		case SDLK_RETURN:
		case SDLK_KP_ENTER:
			if (!event.key.repeat)
			{
				if ((snapshot.option & 1u) != 0 && (event.key.keysym.mod & KMOD_CTRL) == 0)
					HostInsertText(snapshot.generation, u"\n");
				else HostAccept(snapshot.generation);
			}
			break;
		default: break;
	}
	return true;
}

std::u16string HostPreedit()
{
	std::lock_guard lock(g_preedit_mutex);
	return g_preedit;
}

std::string HostCaption()
{
	HostSnapshot snapshot;
	if (!GetHostSnapshot(&snapshot)) return {};
	std::u16string text = snapshot.text;
	std::u16string preedit = HostPreedit();
	const bool password = (snapshot.option & 4u) != 0;
	if (password) { text.assign(text.size(), u'*'); preedit.assign(preedit.size(), u'*'); }
	const size_t cursor = std::min<size_t>(snapshot.cursor, text.size());
	text.insert(cursor, preedit.empty() ? u"|" : u"[" + preedit + u"]");
	std::replace(text.begin(), text.end(), u'\n', u' ');
	const auto title = String::FromUtf16(snapshot.title.empty() ? u"Text input" : snapshot.title.c_str());
	const auto preview = String::FromUtf16(text.c_str());
	return std::string(title.C_Str()) + " — " + preview.C_Str() + "   [Enter: OK · Esc: Cancel]";
}
} // namespace Lumen::Libs::Dialog::ImeDialog

#endif // LUMEN_EMU_ENABLED
