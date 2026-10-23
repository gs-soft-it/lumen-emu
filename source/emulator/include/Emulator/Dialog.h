#ifndef EMULATOR_INCLUDE_EMULATOR_DIALOG_H_
#define EMULATOR_INCLUDE_EMULATOR_DIALOG_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"

#include <cstddef>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Dialog {

// Dialog status values (shared by all dialog types)
enum class Status : int {
	NONE        = 0,
	INITIALIZED = 1,
	RUNNING     = 2,
	FINISHED    = 3
};

namespace CommonDialog {

int LUMEN_SYSV_ABI CommonDialogInitialize();

} // namespace CommonDialog

namespace ErrorDialog {

uint32_t LUMEN_SYSV_ABI ErrorDialogUpdateStatus();

} // namespace ErrorDialog

namespace NpCommerceDialog {

int32_t LUMEN_SYSV_ABI NpCommerceDialogUpdateStatus();

} // namespace NpCommerceDialog

namespace SaveDataDialog {

// Guest ABI64 layout. Addresses remain opaque.
struct SaveDataDialogResult {
	uint32_t mode;
	uint32_t result;
	uint32_t buttonId;
	uint32_t pad1;
	uint64_t dirName;
	uint64_t param;
	uint64_t userData;
	uint8_t  reserved[32];
};

static_assert(sizeof(SaveDataDialogResult) == 72);
static_assert(alignof(SaveDataDialogResult) == 8);
static_assert(offsetof(SaveDataDialogResult, mode) == 0);
static_assert(offsetof(SaveDataDialogResult, result) == 4);
static_assert(offsetof(SaveDataDialogResult, buttonId) == 8);
static_assert(offsetof(SaveDataDialogResult, dirName) == 16);
static_assert(offsetof(SaveDataDialogResult, param) == 24);
static_assert(offsetof(SaveDataDialogResult, userData) == 32);
static_assert(offsetof(SaveDataDialogResult, reserved) == 40);

uint32_t LUMEN_SYSV_ABI SaveDataDialogInitialize();
uint32_t LUMEN_SYSV_ABI SaveDataDialogOpen(const void* param);
uint32_t LUMEN_SYSV_ABI SaveDataDialogGetStatus();
uint32_t LUMEN_SYSV_ABI SaveDataDialogGetResult(SaveDataDialogResult* result);
uint32_t LUMEN_SYSV_ABI SaveDataDialogUpdateStatus();
uint32_t LUMEN_SYSV_ABI SaveDataDialogTerminate();
uint32_t LUMEN_SYSV_ABI SaveDataDialogProgressBarSetValue(uint32_t target, uint32_t rate);
uint32_t LUMEN_SYSV_ABI SaveDataDialogProgressBarInc(uint32_t target, uint32_t delta);
uint32_t LUMEN_SYSV_ABI SaveDataDialogClose();

} // namespace SaveDataDialog

namespace MsgDialog {

int LUMEN_SYSV_ABI MsgDialogInitialize();
int LUMEN_SYSV_ABI MsgDialogOpen(void* param);
int LUMEN_SYSV_ABI MsgDialogGetStatus();
int LUMEN_SYSV_ABI MsgDialogGetResult(void* result);
int LUMEN_SYSV_ABI MsgDialogUpdateStatus();
int LUMEN_SYSV_ABI MsgDialogTerminate();
int LUMEN_SYSV_ABI MsgDialogClose();
int LUMEN_SYSV_ABI MsgDialogProgressBarSetValue(int target, uint32_t rate);
int LUMEN_SYSV_ABI MsgDialogProgressBarInc(int target, uint32_t delta);
int LUMEN_SYSV_ABI MsgDialogProgressBarSetMsg(int target, const char* msg);

} // namespace MsgDialog

} // namespace Lumen::Libs::Dialog

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_DIALOG_H_ */
