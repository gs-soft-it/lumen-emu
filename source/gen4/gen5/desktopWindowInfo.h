// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <SDL3/SDL.h>
#include <fmt/format.h>
#include <imgui.h>

#include "loader/systemContent.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace Libs::Graphics::DesktopWindowInfo {

inline std::string Title() {
	std::string id;
	std::string title;
	std::string version;
	Loader::SystemContentParamSfoGetString("TITLE_ID", &id);
	Loader::SystemContentParamSfoGetString("TITLE", &title);
	Loader::SystemContentParamSfoGetString("APP_VER", &version);
	if (id.empty() && title.empty() && version.empty()) {
		return "Lumen";
	}
	return fmt::format("Lumen | {} - {} <{}>", id.empty() ? "Unknown ID" : id,
	                   title.empty() ? "Unknown title" : title,
	                   version.empty() ? "Unknown version" : version);
}

#if defined(__ANDROID__) || defined(LUMEN_IOS_FEX_MODULE)
inline constexpr bool DesktopFpsAvailable = false;
#else
inline constexpr bool DesktopFpsAvailable = true;
#endif

// F10 is handled on the window thread and the renderer reads this revision.
inline std::atomic<uint32_t> fps_visibility_revision {0};

inline bool FpsEnabled() noexcept {
	return DesktopFpsAvailable && (fps_visibility_revision.load(std::memory_order_relaxed) % 2 == 0);
}

inline bool HandleFpsKey(const SDL_Event& event) noexcept {
	if (!DesktopFpsAvailable || (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP) ||
	    event.key.key != SDLK_F10) {
		return false;
	}
	if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
		fps_visibility_revision.fetch_add(1, std::memory_order_relaxed);
	}
	return true;
}

inline void DrawFps(uint64_t presents) {
	const auto revision = fps_visibility_revision.load(std::memory_order_relaxed);
	if (!DesktopFpsAvailable || revision % 2 != 0 || presents == 0) {
		return;
	}

	using Clock = std::chrono::steady_clock;
	static Clock::time_point last_sample {};
	static uint64_t last_presents = 0;
	static uint32_t last_revision = 0;
	static double fps = 0.0;
	const auto now = Clock::now();
	if (last_sample == Clock::time_point {} || revision != last_revision || presents < last_presents) {
		last_sample = now;
		last_presents = presents;
		last_revision = revision;
		fps = 0.0;
	} else if (now - last_sample >= std::chrono::milliseconds(250)) {
		fps = static_cast<double>(presents - last_presents) /
		      std::chrono::duration<double>(now - last_sample).count();
		last_sample = now;
		last_presents = presents;
	}

	ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x - 8.0f, 8.0f}, ImGuiCond_Always, {1.0f, 0.0f});
	constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
	    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
	    ImGuiWindowFlags_NoFocusOnAppearing;
	if (ImGui::Begin("##LumenFps", nullptr, flags)) {
		const ImVec4 color = fps < 10.0 ? ImVec4(1.0f, 0.0f, 0.0f, 1.0f) :
		    fps < 20.0 ? ImVec4(1.0f, 0.5f, 0.0f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
		if (fps > 0.0) {
			ImGui::TextColored(color, "%.0f FPS (%.1f ms)", fps, 1000.0 / fps);
		} else {
			ImGui::TextUnformatted("— FPS");
		}
	}
	ImGui::End();
}

} // namespace Libs::Graphics::DesktopWindowInfo
