// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>
#include <imgui.h>

#include "core/emulator_settings.h"
#include "imgui/imgui_texture.h"

namespace ImGuiEmuSettings {

class SettingsWindow {

public:
    SettingsWindow();
    void Prepare();
    void DrawSettings(bool* open, const std::function<void()>& applySettings);

private:
    enum class SettingsCategory {
        General,
        Graphics,
        Input,
        Trophy,
        Log,
        Experimental,
    };

    void SaveSettings(std::string profile);
    void LoadSettings(std::string profile);

    void SetupWindow();
    void DeInit();

    void DrawMainContent(bool* open, const std::function<void()>& applySettings);
    void DrawSettingsTable(SettingsCategory);
    void DrawCategoryTabs();
    void AddCategory(std::string name, const ImGui::RefCountedTexture& texture,
                     SettingsCategory category);

    void AddSettingCheckbox(std::string name, bool& value);
    void AddSettingSliderInt(std::string name, int& value, int min, int max);
    void AddSettingSliderFloat(std::string name, float& value, int min, int max, int precision);
    void AddSettingCombo(std::string name, int& value, std::vector<std::string> options);
    int GetComboIndex(std::string selection, std::vector<std::string> options);

    float uiScale = 1.0f;
    SettingsCategory currentCategory = SettingsCategory::General;
    std::string currentProfile = "Global";

    std::string runningGameSerial = "";
    bool closeOnSave = false;

    ImGui::RefCountedTexture generalTexture;
    ImGui::RefCountedTexture experimentalTexture;
    ImGui::RefCountedTexture graphicsTexture;
    ImGui::RefCountedTexture inputTexture;
    ImGui::RefCountedTexture trophyTexture;
    ImGui::RefCountedTexture logTexture;

    //////////////////// options for comboboxes
    const std::map<std::string, int> languageMap = {{"Arabic", 21},
                                                    {"Czech", 23},
                                                    {"Danish", 14},
                                                    {"Dutch", 6},
                                                    {"English (United Kingdom)", 18},
                                                    {"English (United States)", 1},
                                                    {"Finnish", 12},
                                                    {"French (Canada)", 22},
                                                    {"French (France)", 2},
                                                    {"German", 4},
                                                    {"Greek", 25},
                                                    {"Hungarian", 24},
                                                    {"Indonesian", 29},
                                                    {"Italian", 5},
                                                    {"Japanese", 0},
                                                    {"Korean", 9},
                                                    {"Norwegian (Bokmaal)", 15},
                                                    {"Polish", 16},
                                                    {"Portuguese (Brazil)", 17},
                                                    {"Portuguese (Portugal)", 7},
                                                    {"Romanian", 26},
                                                    {"Russian", 8},
                                                    {"Simplified Chinese", 11},
                                                    {"Spanish (Latin America)", 20},
                                                    {"Spanish (Spain)", 3},
                                                    {"Swedish", 13},
                                                    {"Thai", 27},
                                                    {"Traditional Chinese", 10},
                                                    {"Turkish", 19},
                                                    {"Ukrainian", 30},
                                                    {"Vietnamese", 28}};
    std::vector<std::string> languageOptions; // assigned from keys above
    const std::vector<std::string> fullscreenModeOptions = {"Windowed", "Fullscreen",
                                                            "Fullscreen (Borderless)"};
    const std::vector<std::string> audioBackendOptions = {"SDL", "OpenAL"};
    const std::vector<std::string> presentModeOptions = {"Mailbox", "Fifo", "Immediate"};
    const std::vector<std::string> hideCursorOptions = {"Never", "Idle", "Always"};
    const std::vector<std::string> trophySideOptions = {"left", "right", "top", "bottom"};
    const std::vector<std::string> readbacksModeOptions = {"Disabled", "Relaxed", "Precise"};

    //////////////// Setting Variables
    //////////////// Note:: Use int for all comboboxes as needed by ImGui

    // General tab
    int consoleLanguageSetting;
    int volumeSetting;
    bool showSplashSetting;
    int audioBackendSetting;

    // Graphics tab
    int fullscreenModeSetting;
    int presentModeSetting;
    int windowWidthSetting;
    int windowHeightSetting;
    bool hdrAllowedSetting;
    bool fsrEnabledSetting;
    bool rcasEnabledSetting;
    float rcasAttenuationSetting;

    // Input tab
    bool motionControlsSetting;
    bool backgroundControllerSetting;
    int cursorStateSetting;
    int cursorTimeoutSetting;

    // Trophy tab
    bool trophyPopupDisabledSetting;
    int trophySideSetting;
    float trophyDurationSetting;

    // Log tab
    bool logEnableSetting;
    bool logSeparateSetting;
    bool logSyncSetting;

    // Experimental tab
    int readbacksModeSetting;
    bool readbackLinearImagesSetting;
    bool directMemoryAccessSetting;
    bool devkitConsoleSetting;
    bool neoModeSetting;
    bool shadnetEnabledSetting;
    bool connectedNetworkSetting;
    bool pipelineCacheEnabledSetting;
    bool pipelineCacheArchiveSetting;
    int extraDmemSetting;
    int vblankFrequencySetting;
};

} // namespace ImGuiEmuSettings
