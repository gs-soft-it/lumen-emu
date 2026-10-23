// SPDX-License-Identifier: GPL-2.0-only
// PS4 guest adapter for the separately imported shared Gen5 IME host model.
// Provenance: source/gen4/gen5/UPSTREAM.json; reuse is confined to this Lumen derivative.
#ifndef LUMEN_EMULATOR_LIBS_IME_DIALOG_ADAPTER_H_
#define LUMEN_EMULATOR_LIBS_IME_DIALOG_ADAPTER_H_

#include "Emulator/Common.h"
#include "libs/imeDialog.h"

#include <cstdint>
#include <string_view>

#ifdef LUMEN_EMU_ENABLED
namespace Lumen::Libs::Dialog::ImeDialog {

using Param           = ::Libs::Dialog::ImeDialog::Param;
using ExtendedParam   = ::Libs::Dialog::ImeDialog::ExtendedParam;
using Result          = ::Libs::Dialog::ImeDialog::Result;
using PositionAndForm = ::Libs::Dialog::ImeDialog::PositionAndForm;
using Status          = ::Libs::Dialog::ImeDialog::Status;
using EndStatus       = ::Libs::Dialog::ImeDialog::EndStatus;
using Type            = ::Libs::Dialog::ImeDialog::Type;
using EnterLabel      = ::Libs::Dialog::ImeDialog::EnterLabel;
using Alignment       = ::Libs::Dialog::ImeDialog::Alignment;
using HostSnapshot    = ::Libs::Dialog::ImeDialog::HostSnapshot;
using VisualState     = ::Libs::Dialog::ImeDialog::VisualState;
using VisibilityCallback = ::Libs::Dialog::ImeDialog::VisibilityCallback;

constexpr uint32_t IME_DIALOG_MAX_TEXT_LENGTH = ::Libs::Dialog::ImeDialog::IME_DIALOG_MAX_TEXT_LENGTH;

int32_t LUMEN_SYSV_ABI ImeDialogInit(const Param* param, const ExtendedParam* extended);
int32_t LUMEN_SYSV_ABI ImeDialogGetStatus();
int32_t LUMEN_SYSV_ABI ImeDialogAbort();
int32_t LUMEN_SYSV_ABI ImeDialogGetResult(Result* result);
int32_t LUMEN_SYSV_ABI ImeDialogTerm();
int32_t LUMEN_SYSV_ABI ImeDialogGetPanelSize(const Param* param, uint32_t* width, uint32_t* height);
int32_t LUMEN_SYSV_ABI ImeDialogGetPanelSizeExtended(const Param* param, const ExtendedParam* extended,
                                                 uint32_t* width, uint32_t* height);
int32_t LUMEN_SYSV_ABI ImeDialogGetPanelPositionAndForm(PositionAndForm* form);

// SDL may edit only host-owned text. Guest publication occurs synchronously in
// a guest poll/result/term call, after revalidating the original VM authority.
VisualState GetVisualState() noexcept;
void SetVisibilityCallback(VisibilityCallback callback) noexcept;
bool GetHostSnapshot(HostSnapshot* snapshot);
bool HostInsertText(uint64_t generation, std::u16string_view text);
bool HostReplaceText(uint64_t generation, std::u16string_view text, uint32_t cursor);
bool HostBackspace(uint64_t generation);
bool HostMoveCursor(uint64_t generation, int delta);
bool HostAccept(uint64_t generation);
bool HostCancel(uint64_t generation);
void ResetSessionState();

} // namespace Lumen::Libs::Dialog::ImeDialog
#endif
#endif
