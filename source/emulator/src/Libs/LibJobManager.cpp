#include "Emulator/Common.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("JobManager", 1, "JobManager", 1, 1);

namespace JobManagerStubs {

static int LUMEN_SYSV_ABI JobManagerStub_VTKe() { PRINT_NAME(); return 0; }
static int LUMEN_SYSV_ABI JobManagerStub_cwMz() { PRINT_NAME(); return 0; }
static int LUMEN_SYSV_ABI JobManagerStub_WyXg() { PRINT_NAME(); return 0; }
static int LUMEN_SYSV_ABI JobManagerStub_4g1e() { PRINT_NAME(); return 0; }
static int LUMEN_SYSV_ABI JobManagerStub_chxn() { PRINT_NAME(); return 0; }
static int LUMEN_SYSV_ABI JobManagerStub_Nm2B() { PRINT_NAME(); return 0; }
static int LUMEN_SYSV_ABI JobManagerStub_GS9D() { PRINT_NAME(); return 0; }

} // namespace JobManagerStubs

LIB_DEFINE(InitJobManager_1)
{
	LIB_FUNC("VTKezfYu-v4", JobManagerStubs::JobManagerStub_VTKe);
	LIB_FUNC("cwMzzdesLlY", JobManagerStubs::JobManagerStub_cwMz);
	LIB_FUNC("W-yXgQmaozU", JobManagerStubs::JobManagerStub_WyXg);
	LIB_FUNC("4g1ee34TMH8", JobManagerStubs::JobManagerStub_4g1e);
	LIB_FUNC("chxnvsxxfBY", JobManagerStubs::JobManagerStub_chxn);
	LIB_FUNC("Nm2BhAwYNFk", JobManagerStubs::JobManagerStub_Nm2B);
	LIB_FUNC("GS9DSbzC+MY", JobManagerStubs::JobManagerStub_GS9D);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
