#include "Emulator/Dialog.h"

#include "Lumen/Core/String.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <cstring>
#include <mutex>

#ifdef LUMEN_EMU_ENABLED

// NOLINTNEXTLINE(modernize-concat-nested-namespaces)
namespace Lumen::Libs::Dialog {

namespace CommonDialog {

LIB_NAME("CommonDialog", "CommonDialog");

int LUMEN_SYSV_ABI CommonDialogInitialize()
{
	PRINT_NAME();

	return OK;
}

} // namespace CommonDialog

namespace ErrorDialog {

LIB_NAME("ErrorDialog", "ErrorDialog");

uint32_t LUMEN_SYSV_ABI ErrorDialogUpdateStatus()
{
	PRINT_NAME();
	// No ErrorDialog can be opened by the current service. Polling observes
	// that absence; it must not complete or acknowledge another dialog.
	return static_cast<uint32_t>(Status::NONE);
}

} // namespace ErrorDialog

namespace NpCommerceDialog {

int32_t LUMEN_SYSV_ABI NpCommerceDialogUpdateStatus()
{
	static std::once_flag marker_once;
	std::call_once(marker_once, [] {
		std::fputs("[NP_COMMERCE_DIALOG_OFFLINE_EXPERIMENT] operation=update-status result=none-local value=0 experimental=1 "
		           "console_contract=unverified dialog=absent ui=0 network=0 purchases=0 entitlements=0\n",
		           stderr);
	});

	return int32_t {0};
}

} // namespace NpCommerceDialog

namespace SaveDataDialog {

LIB_NAME("SaveDataDialog", "SaveDataDialog");

// Existing local immediate-completion model; this is not a qualified console lifecycle.
static std::mutex g_dialog_mutex;
static Status g_dialog_status = Status::NONE;
static uint32_t g_dialog_result = 0;
// The old service wrote 48 bytes. No evidence yet requires writing the rest
// of the ABI record's reserved tail, so retain that publication boundary.
static constexpr size_t RESULT_WRITE_SIZE = 48;

uint32_t LUMEN_SYSV_ABI SaveDataDialogInitialize()
{
	PRINT_NAME();
	std::lock_guard<std::mutex> lock(g_dialog_mutex);

	g_dialog_status = Status::INITIALIZED;
	g_dialog_result = 0;

	return OK;
}

uint32_t LUMEN_SYSV_ABI SaveDataDialogOpen(const void* param)
{
	PRINT_NAME();
	std::lock_guard<std::mutex> lock(g_dialog_mutex);

	// Simulate immediate dialog completion
	// In a real implementation, this would show UI and wait for user input
	g_dialog_status = Status::FINISHED;
	g_dialog_result = 0;  // SCE_OK

	printf("\tDialog opened and immediately completed (stub)\n");

	return OK;
}

uint32_t LUMEN_SYSV_ABI SaveDataDialogGetStatus()
{
	PRINT_NAME();
	std::lock_guard<std::mutex> lock(g_dialog_mutex);

	printf("\tStatus = %d\n", static_cast<int>(g_dialog_status));

	return static_cast<uint32_t>(g_dialog_status);
}

uint32_t LUMEN_SYSV_ABI SaveDataDialogGetResult(SaveDataDialogResult* result)
{
	PRINT_NAME();

#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__APPLE__)
	Core::VirtualMemory::GuestDataLease output;
	if (result != nullptr && !Core::VirtualMemory::TryAcquireGuestDataLease(
	        reinterpret_cast<uint64_t>(result), RESULT_WRITE_SIZE, Core::VirtualMemory::Mode::Write, &output))
	{
		// Local error policy: an inaccessible output neither consumes the result
		// nor writes a prefix. The declarations do not assign this error case.
		return uint32_t {0x80b8000a};
	}
#endif
	std::lock_guard<std::mutex> lock(g_dialog_mutex);
	if (result != nullptr)
	{
		// Preserve the zero-result model and its historical write footprint.
		const SaveDataDialogResult payload {0, g_dialog_result, 0, 0, 0, 0, 0, {}};
		std::memcpy(result, &payload, RESULT_WRITE_SIZE);
	}

	// Reset status after getting result
	g_dialog_status = Status::NONE;

	return OK;
}

uint32_t LUMEN_SYSV_ABI SaveDataDialogUpdateStatus()
{
	PRINT_NAME();
	std::lock_guard<std::mutex> lock(g_dialog_mutex);

	return static_cast<uint32_t>(g_dialog_status);
}

uint32_t LUMEN_SYSV_ABI SaveDataDialogTerminate()
{
	PRINT_NAME();
	std::lock_guard<std::mutex> lock(g_dialog_mutex);

	g_dialog_status = Status::NONE;
	g_dialog_result = 0;

	return OK;
}

uint32_t LUMEN_SYSV_ABI SaveDataDialogProgressBarSetValue(uint32_t target, uint32_t rate)
{
	PRINT_NAME();

	printf("\t target = %u\n", target);
	printf("\t rate   = %u\n", rate);

	return OK;
}

uint32_t LUMEN_SYSV_ABI SaveDataDialogProgressBarInc(uint32_t target, uint32_t delta)
{
	PRINT_NAME();

	printf("\t target = %u\n", target);
	printf("\t delta  = %u\n", delta);

	return OK;
}

uint32_t LUMEN_SYSV_ABI SaveDataDialogClose()
{
	PRINT_NAME();
	std::lock_guard<std::mutex> lock(g_dialog_mutex);

	g_dialog_status = Status::NONE;

	return OK;
}

} // namespace SaveDataDialog

namespace MsgDialog {

LIB_NAME("MsgDialog", "MsgDialog");

static Status g_msg_dialog_status = Status::NONE;

int LUMEN_SYSV_ABI MsgDialogInitialize()
{
	PRINT_NAME();

	g_msg_dialog_status = Status::INITIALIZED;

	return OK;
}

int LUMEN_SYSV_ABI MsgDialogOpen(void* param)
{
	PRINT_NAME();

	// Simulate immediate dialog completion
	g_msg_dialog_status = Status::FINISHED;

	return OK;
}

int LUMEN_SYSV_ABI MsgDialogGetStatus()
{
	PRINT_NAME();

	return static_cast<int>(g_msg_dialog_status);
}

int LUMEN_SYSV_ABI MsgDialogGetResult(void* result)
{
	PRINT_NAME();

	if (result != nullptr)
	{
		// Zero out the result structure (first 48 bytes covers common fields)
		memset(result, 0, 48);
	}

	g_msg_dialog_status = Status::NONE;

	return OK;
}

int LUMEN_SYSV_ABI MsgDialogUpdateStatus()
{
	PRINT_NAME();

	return static_cast<int>(g_msg_dialog_status);
}

int LUMEN_SYSV_ABI MsgDialogTerminate()
{
	PRINT_NAME();

	g_msg_dialog_status = Status::NONE;

	return OK;
}

int LUMEN_SYSV_ABI MsgDialogClose()
{
	PRINT_NAME();

	g_msg_dialog_status = Status::NONE;

	return OK;
}

int LUMEN_SYSV_ABI MsgDialogProgressBarSetValue(int target, uint32_t rate)
{
	PRINT_NAME();

	return OK;
}

int LUMEN_SYSV_ABI MsgDialogProgressBarInc(int target, uint32_t delta)
{
	PRINT_NAME();

	return OK;
}

int LUMEN_SYSV_ABI MsgDialogProgressBarSetMsg(int target, const char* msg)
{
	PRINT_NAME();

	return OK;
}

} // namespace MsgDialog

} // namespace Lumen::Libs::Dialog

#endif // LUMEN_EMU_ENABLED
