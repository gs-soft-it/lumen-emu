// SPDX-License-Identifier: GPL-2.0-only
// Original Lumen presentation shared by the SDL2 and SDL3 engines. Dear ImGui
// and its embedded fonts retain their own MIT notices.
#include "KeyboardPanel.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <string>

namespace Lumen::ImeUi {
namespace {
using ::Libs::ImeCommon::EnterLabel;
using ::Libs::ImeCommon::HostSnapshot;
using ::Libs::ImeCommon::Type;

std::string Utf8(std::u16string_view text)
{
    std::string result;
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t value = text[i];
        if (value >= 0xd800 && value <= 0xdbff) {
            if (i + 1 >= text.size() || text[i + 1] < 0xdc00 || text[i + 1] > 0xdfff) value = 0xfffd;
            else { value = 0x10000 + ((value - 0xd800) << 10) + (text[++i] - 0xdc00); }
        } else if (value >= 0xdc00 && value <= 0xdfff) value = 0xfffd;
        if (value < 0x80) result.push_back(static_cast<char>(value));
        else if (value < 0x800) {
            result.push_back(static_cast<char>(0xc0 | (value >> 6)));
            result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        } else if (value < 0x10000) {
            result.push_back(static_cast<char>(0xe0 | (value >> 12)));
            result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        } else {
            result.push_back(static_cast<char>(0xf0 | (value >> 18)));
            result.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        }
    }
    return result;
}

std::string Preview(const HostSnapshot& snapshot, std::u16string_view preedit,
                    size_t max_units = 160, bool single_line = false)
{
    const size_t cursor = std::min<size_t>(snapshot.cursor, snapshot.text.size());
    size_t begin = cursor > max_units / 2 ? cursor - max_units / 2 : 0;
    size_t end = std::min(snapshot.text.size(), begin + max_units);
    if (begin != 0 && snapshot.text[begin] >= 0xdc00 && snapshot.text[begin] <= 0xdfff) --begin;
    if (end < snapshot.text.size() && end != 0 && snapshot.text[end - 1] >= 0xd800 && snapshot.text[end - 1] <= 0xdbff) ++end;
    std::u16string visible = snapshot.text.substr(begin, end - begin);
    std::u16string composition(preedit);
    if ((snapshot.option & ::Libs::ImeCommon::OPTION_PASSWORD) != 0) {
        visible.assign(visible.size(), u'*');
        composition.assign(composition.size(), u'*');
    }
    visible.insert(cursor - begin, u"|");
    if (!composition.empty()) visible.insert(cursor - begin, u"[" + composition + u"]");
    if (begin != 0) visible.insert(0, u"... ");
    if (end != snapshot.text.size()) visible.append(u" ...");
    if (single_line) {
        std::replace(visible.begin(), visible.end(), u'\n', u' ');
        std::replace(visible.begin(), visible.end(), u'\r', u' ');
    }
    return Utf8(visible);
}

const char* AcceptLabel(EnterLabel label)
{
    switch (label) {
        case EnterLabel::Send: return "Send";
        case EnterLabel::Search: return "Search";
        case EnterLabel::Go: return "Go";
        default: return "OK";
    }
}

void DrawKeys(const HostSnapshot& snapshot, const KeyboardPanelActions& actions,
              KeyboardPanelState& state, float scale)
{
    const bool number = snapshot.type == Type::Number;
    const std::array<std::u16string_view, 4> letters {{u"1234567890", u"qwertyuiop", u"asdfghjkl", u"zxcvbnm"}};
    const std::array<std::u16string_view, 4> symbols {{u"!@#$%^&*()", u"-_=+[]{}", u"\\|;:'\",.?", u"/<>`~"}};
    const std::array<std::u16string_view, 4> numbers {{u"123", u"456", u"789", u"-0.,"}};
    const auto& rows = number ? numbers : state.symbols ? symbols : letters;
    const float width = std::max(1.0f, (ImGui::GetContentRegionAvail().x - 9 * ImGui::GetStyle().ItemSpacing.x) / 10);
    const float height = std::max(28.0f, 38.0f * scale);
    for (size_t row = 0; row < rows.size(); ++row) {
        ImGui::PushID(static_cast<int>(row));
        for (size_t column = 0; column < rows[row].size(); ++column) {
            char16_t value = rows[row][column];
            if (state.shift && value >= u'a' && value <= u'z') value -= u'a' - u'A';
            ImGui::PushID(static_cast<int>(column));
            const auto label = Utf8(std::u16string_view(&value, 1));
            if (ImGui::Button(label.c_str(), ImVec2(number ? width * 2 : width, height)))
                actions.insert_text(snapshot.generation, std::u16string_view(&value, 1));
            if (state.focus_pending) { ImGui::SetItemDefaultFocus(); state.focus_pending = false; }
            ImGui::PopID();
            if (column + 1 < rows[row].size()) ImGui::SameLine();
        }
        ImGui::PopID();
    }
}
} // namespace

void DrawKeyboardPanel(const HostSnapshot& snapshot, std::u16string_view preedit,
                       const KeyboardPanelActions& actions, KeyboardPanelState& state, float scale)
{
    if (state.generation != snapshot.generation) state.Reset(snapshot.generation);
    scale = std::max(scale, 0.25f);
    const float height = std::max(28.0f, 38.0f * scale);
    std::u16string title = snapshot.title.empty() ? u"Enter text" : snapshot.title;
    std::replace(title.begin(), title.end(), u'\n', u' ');
    std::replace(title.begin(), title.end(), u'\r', u' ');
    if (!snapshot.key_panel_visible) {
        // External-keyboard profiles retain the compact panel dimensions given
        // by the guest. The text and actions must not require a full keys grid.
        const float glyph_width = std::max(ImGui::CalcTextSize("M").x, 1.0f);
        const float available = ImGui::GetContentRegionAvail().x;
        const size_t title_units = std::max<size_t>(1, static_cast<size_t>(std::max(available, 0.0f) / (3 * glyph_width)));
        if (title.size() > title_units) {
            size_t end = title_units;
            if (end != 0 && title[end - 1] >= 0xd800 && title[end - 1] <= 0xdbff) --end;
            title.resize(end);
            title.append(u"...");
        }
        std::string compact = Utf8(title) + ": ";
        const float text_width = std::max(available - ImGui::CalcTextSize(compact.c_str()).x, glyph_width);
        const size_t visible_units = std::max<size_t>(6, static_cast<size_t>(text_width / glyph_width));
        if (snapshot.text.empty() && preedit.empty() && !snapshot.placeholder.empty()) {
            std::u16string placeholder = snapshot.placeholder;
            std::replace(placeholder.begin(), placeholder.end(), u'\n', u' ');
            std::replace(placeholder.begin(), placeholder.end(), u'\r', u' ');
            compact += Utf8(placeholder);
        } else compact += Preview(snapshot, preedit, visible_units, true);
        ImGui::TextUnformatted(compact.c_str());
        const float compact_height = 28.0f * scale;
        if (ImGui::Button("Cancel", ImVec2(90 * scale, compact_height))) actions.cancel(snapshot.generation);
        ImGui::SameLine();
        if (ImGui::Button(AcceptLabel(snapshot.enter_label), ImVec2(90 * scale, compact_height))) actions.accept(snapshot.generation);
        return;
    }
    const auto title_utf8 = Utf8(title);
    if (ImGui::BeginTable("##ImeHeader", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Cancel", ImGuiTableColumnFlags_WidthFixed, 90 * scale);
        ImGui::TableSetupColumn("Accept", ImGuiTableColumnFlags_WidthFixed, 90 * scale);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(title_utf8.c_str());
        ImGui::TableNextColumn();
        if (ImGui::Button("Cancel", ImVec2(-1, height))) actions.cancel(snapshot.generation);
        ImGui::TableNextColumn();
        if (ImGui::Button(AcceptLabel(snapshot.enter_label), ImVec2(-1, height))) actions.accept(snapshot.generation);
        ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::BeginChild("##ImeText", ImVec2(0, std::max(38.0f, 75.0f * scale)), ImGuiChildFlags_Borders);
    if (snapshot.text.empty() && preedit.empty() && !snapshot.placeholder.empty()) {
        const auto placeholder = Utf8(snapshot.placeholder);
        ImGui::TextDisabled("%s", placeholder.c_str());
    }
    const auto preview = Preview(snapshot, preedit);
    ImGui::TextWrapped("%s", preview.c_str());
    ImGui::EndChild();
    ImGui::TextDisabled("%zu / %u", snapshot.text.size(), snapshot.max_text_length);
    // Only virtual keys scroll. Confirmation and the owned text preview stay
    // visible in both a desktop window and a small engine overlay.
    ImGui::BeginChild("##ImeKeys", ImVec2(0, 0));
    DrawKeys(snapshot, actions, state, scale);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const int count = snapshot.type == Type::Number ? 1 : 4;
    const float action_width = std::max(1.0f, (ImGui::GetContentRegionAvail().x - (count - 1) * spacing) / count);
    if (snapshot.type != Type::Number) {
        if (ImGui::Button(state.shift ? "Lower" : "Shift", ImVec2(action_width, height))) state.shift = !state.shift;
        ImGui::SameLine();
        if (ImGui::Button(state.symbols ? "ABC" : "Symbols", ImVec2(action_width, height))) state.symbols = !state.symbols;
        ImGui::SameLine();
        if (ImGui::Button("Space", ImVec2(action_width, height))) actions.insert_text(snapshot.generation, u" ");
        ImGui::SameLine();
    }
    if (ImGui::Button("Backspace", ImVec2(action_width, height))) actions.backspace(snapshot.generation);
    const bool multiline = (snapshot.option & ::Libs::ImeCommon::OPTION_MULTILINE) != 0;
    const int edit_count = multiline ? 4 : 3;
    const float edit_width = std::max(1.0f, (ImGui::GetContentRegionAvail().x - (edit_count - 1) * spacing) / edit_count);
    if (ImGui::Button("Left", ImVec2(edit_width, height))) actions.move_cursor(snapshot.generation, -1);
    ImGui::SameLine();
    if (ImGui::Button("Right", ImVec2(edit_width, height))) actions.move_cursor(snapshot.generation, 1);
    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(edit_width, height))) actions.replace_text(snapshot.generation, u"", 0);
    if (multiline) {
        ImGui::SameLine();
        if (ImGui::Button("New line", ImVec2(edit_width, height))) actions.insert_text(snapshot.generation, u"\n");
    }
    ImGui::EndChild();
}
} // namespace Lumen::ImeUi
