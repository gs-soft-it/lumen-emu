// SPDX-License-Identifier: GPL-2.0-only
// Original Lumen desktop IME presentation. Dear ImGui and its embedded fonts
// retain their MIT notices in source/gen4/gen5/compatibility/3rdparty/imgui.
#include "Emulator/Libs/ImeDialogDesktopUi.h"

#ifdef LUMEN_EMU_ENABLED

#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID && LUMEN_PLATFORM != LUMEN_PLATFORM_IOS
#include "Emulator/Libs/ImeDialogInput.h"
#include "ime/KeyboardPanel.h"
#include "SDL.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <array>
#include <cstdio>
#include <string>

namespace Lumen::Libs::Dialog::ImeDialog {
namespace {
SDL_Window* g_window = nullptr;
SDL_Window* g_parent = nullptr;
SDL_Renderer* g_renderer = nullptr;
ImGuiContext* g_context = nullptr;
uint64_t g_generation = 0, g_failed_generation = 0;
Lumen::ImeUi::KeyboardPanelState g_panel_state;
bool g_platform_ready = false, g_renderer_ready = false;

class ContextScope {
public:
    ContextScope(): previous(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(g_context); }
    ~ContextScope() { ImGui::SetCurrentContext(previous); }
private:
    ImGuiContext* previous;
};

bool CreateDesktopUi(const HostSnapshot& snapshot, SDL_Window* parent)
{
    g_parent = parent;
    int display = parent != nullptr ? SDL_GetWindowDisplayIndex(parent) : 0;
    if (display < 0) display = 0;
    const int center = SDL_WINDOWPOS_CENTERED_DISPLAY(display);
    g_window = SDL_CreateWindow("Text input - Lumen", center, center, 720, 680,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (g_window == nullptr) return false;
    SDL_SetWindowMinimumSize(g_window, 620, 640);
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    if (g_renderer == nullptr) return false;
    IMGUI_CHECKVERSION();
    ImGuiContext* previous = ImGui::GetCurrentContext();
    g_context = ImGui::CreateContext();
    if (g_context == nullptr) return false;
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = 20.0f;
    style.WindowPadding = ImVec2(20, 16);
    style.ItemSpacing = ImVec2(7, 6);
    style.FramePadding = ImVec2(8, 8);
    style.FrameRounding = 5;
    io.Fonts->AddFontDefaultVector();
    // The stock SDL backend changes global mouse/IME hints during Init. Keep
    // those settings owned by the existing runtime while using its UI backend.
    const std::array<const char*, 3> hints {{SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH,
        SDL_HINT_MOUSE_AUTO_CAPTURE, SDL_HINT_IME_SHOW_UI}};
    std::array<std::string, 3> old_values;
    std::array<bool, 3> present {};
    for (size_t i = 0; i < hints.size(); ++i) {
        const char* value = SDL_GetHint(hints[i]);
        present[i] = value != nullptr;
        if (value != nullptr) old_values[i] = value;
    }
    g_platform_ready = ImGui_ImplSDL2_InitForSDLRenderer(g_window, g_renderer);
    for (size_t i = 0; i < hints.size(); ++i) {
        if (present[i]) SDL_SetHintWithPriority(hints[i], old_values[i].c_str(), SDL_HINT_OVERRIDE);
        else SDL_ResetHint(hints[i]);
    }
    if (g_platform_ready) {
        // The text editor owns physical text input. This backend may neither
        // open controllers nor control SDL's shared IME lifecycle.
        ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, nullptr, 0);
        ImGui_ImplSDL2_SetMouseCaptureMode(ImGui_ImplSDL2_MouseCaptureMode_Disabled);
        ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;
        g_renderer_ready = ImGui_ImplSDLRenderer2_Init(g_renderer);
    }
    ImGui::SetCurrentContext(previous);
    if (!g_platform_ready || !g_renderer_ready) return false;
    if (parent != nullptr) SDL_SetWindowModalFor(g_window, parent);
    SDL_ShowWindow(g_window);
    SDL_RaiseWindow(g_window);
    g_generation = snapshot.generation;
    g_panel_state.Reset(snapshot.generation);
    std::fprintf(stderr, "[IME_DIALOG_UI] desktop=visible renderer=software\n");
    return true;
}

void DrawDialog(const HostSnapshot& snapshot)
{
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##HostIme", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    const Lumen::ImeUi::KeyboardPanelActions actions {
        HostInsertText, HostReplaceText, HostBackspace, HostMoveCursor, HostAccept, HostCancel};
    Lumen::ImeUi::DrawKeyboardPanel(snapshot, HostPreedit(), actions, g_panel_state);
    ImGui::End();
}

uint32_t EventWindow(const SDL_Event& event)
{
    switch (event.type) {
        case SDL_WINDOWEVENT: return event.window.windowID;
        case SDL_MOUSEMOTION: return event.motion.windowID;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: return event.button.windowID;
        case SDL_MOUSEWHEEL: return event.wheel.windowID;
        case SDL_KEYDOWN: case SDL_KEYUP: return event.key.windowID;
        case SDL_TEXTINPUT: return event.text.windowID;
        case SDL_TEXTEDITING: return event.edit.windowID;
        default: return 0;
    }
}
} // namespace

void CloseDesktopUi()
{
    SDL_Window* parent = g_parent;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    if (g_context != nullptr) {
        ImGui::SetCurrentContext(g_context);
        if (g_renderer_ready) ImGui_ImplSDLRenderer2_Shutdown();
        if (g_platform_ready) ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext(g_context);
        ImGui::SetCurrentContext(previous == g_context ? nullptr : previous);
    }
    g_context = nullptr; g_platform_ready = false; g_renderer_ready = false;
    if (g_renderer != nullptr) SDL_DestroyRenderer(g_renderer);
    if (g_window != nullptr) SDL_DestroyWindow(g_window);
    const bool restore_focus = g_generation != 0 && parent != nullptr;
    g_renderer = nullptr; g_window = nullptr; g_parent = nullptr; g_generation = 0;
    if (restore_focus) SDL_RaiseWindow(parent);
}

void SyncDesktopUi(const HostSnapshot* snapshot, SDL_Window* parent)
{
    if (snapshot == nullptr) { CloseDesktopUi(); g_failed_generation = 0; return; }
    if (g_generation != snapshot->generation) {
        CloseDesktopUi();
        if (g_failed_generation == snapshot->generation) return;
        if (!CreateDesktopUi(*snapshot, parent)) {
            g_failed_generation = snapshot->generation;
            std::fprintf(stderr, "[IME_DIALOG_UI] desktop=unavailable operation=create software_renderer=required\n");
            CloseDesktopUi();
            HostCancel(snapshot->generation);
            return;
        }
        g_failed_generation = 0;
    }
    ContextScope context;
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    DrawDialog(*snapshot);
    ImGui::Render();
    SDL_SetRenderDrawColor(g_renderer, 24, 28, 36, 255);
    SDL_RenderClear(g_renderer);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_renderer);
    SDL_RenderPresent(g_renderer);
}

bool ProcessDesktopUi(const SDL_Event& event)
{
    if (g_window == nullptr || EventWindow(event) != SDL_GetWindowID(g_window)) return false;
    if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE) {
        HostCancel(g_generation);
        CloseDesktopUi();
        return true;
    }
    ContextScope context;
    ImGui_ImplSDL2_ProcessEvent(&event);
    // Physical keys/text continue to the common IME editor exactly once.
    return event.type == SDL_WINDOWEVENT || event.type == SDL_MOUSEMOTION ||
           event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP || event.type == SDL_MOUSEWHEEL;
}

} // namespace Lumen::Libs::Dialog::ImeDialog
#else
namespace Lumen::Libs::Dialog::ImeDialog {
void SyncDesktopUi(const HostSnapshot*, SDL_Window*) {}
bool ProcessDesktopUi(const SDL_Event&) { return false; }
void CloseDesktopUi() {}
}
#endif
#endif
