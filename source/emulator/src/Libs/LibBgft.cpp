#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <atomic>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("Bgft", 1, "Bgft", 1, 1);

namespace Bgft {

static std::atomic<bool> g_initialized {false};
static std::atomic<int>  g_next_task_id {1};

void ResetSessionState()
{
	g_initialized.store(false, std::memory_order_release);
	g_next_task_id.store(1, std::memory_order_release);
}

static int LUMEN_SYSV_ABI BgftServiceIntInit(const void* params)
{
	PRINT_NAME();
	if (params == nullptr)
	{
		return -1;
	}
	g_initialized.store(true);
	return OK;
}

static int LUMEN_SYSV_ABI BgftServiceIntTerm()
{
	PRINT_NAME();
	g_initialized.store(false);
	return OK;
}

static int LUMEN_SYSV_ABI BgftServiceIntDownloadRegisterTaskByStorageEx(const void* params, int* task_id)
{
	PRINT_NAME();
	if (!g_initialized.load() || params == nullptr || task_id == nullptr)
	{
		return -1;
	}
	*task_id = g_next_task_id.fetch_add(1);
	return OK;
}

static int LUMEN_SYSV_ABI BgftServiceDownloadStartTask(int task_id)
{
	PRINT_NAME();
	return (g_initialized.load() && task_id > 0 ? OK : -1);
}

} // namespace Bgft

LIB_DEFINE(InitBgft_1)
{
	(void)g_library_version;
	(void)g_module_version_major;
	(void)g_module_version_minor;
	LIB_FUNC_STD("sceBgftServiceIntInit", Bgft::BgftServiceIntInit);
	LIB_FUNC_STD("sceBgftServiceIntTerm", Bgft::BgftServiceIntTerm);
	LIB_FUNC_STD("sceBgftServiceIntDownloadRegisterTaskByStorageEx", Bgft::BgftServiceIntDownloadRegisterTaskByStorageEx);
	LIB_FUNC_STD("sceBgftServiceDownloadStartTask", Bgft::BgftServiceDownloadStartTask);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
