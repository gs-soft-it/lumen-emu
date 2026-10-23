#include "Emulator/Common.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("Coredump", 1, "libkernel", 1, 1);

namespace CoredumpStubs {

static int LUMEN_SYSV_ABI sceCoredumpRegisterCoredumpHandler(void* handler, size_t stack_size, void* user_data)
{
	PRINT_NAME();
	return 0;
}

static int LUMEN_SYSV_ABI sceCoredumpAttachMemoryRegion(const char* name, void* addr, size_t size)
{
	PRINT_NAME();
	return 0;
}

static int LUMEN_SYSV_ABI sceCoredumpWriteUserData(const void* data, size_t size)
{
	PRINT_NAME();
	return 0;
}

} // namespace CoredumpStubs

LIB_DEFINE(InitCoredump_1)
{
	LIB_FUNC("8zLSfEfW5AU", CoredumpStubs::sceCoredumpRegisterCoredumpHandler);
	LIB_FUNC("+YX0z-GUSNw", CoredumpStubs::sceCoredumpAttachMemoryRegion);
	LIB_FUNC("Dbbkj6YHWdo", CoredumpStubs::sceCoredumpWriteUserData);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
