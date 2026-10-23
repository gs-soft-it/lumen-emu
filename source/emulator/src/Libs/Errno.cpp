#include "Emulator/Libs/Errno.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/VirtualMemory.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Posix {

namespace {
struct ThreadErrorStorage
{
	int* address = nullptr;
	~ThreadErrorStorage() { ReleaseCurrentThreadError(); }
};
thread_local ThreadErrorStorage g_errno;
}

void PrepareCurrentThreadError()
{
	if (g_errno.address != nullptr) return;
	const auto address = Core::VirtualMemory::Alloc(0, 16 * 1024, Core::VirtualMemory::Mode::ReadWrite);
	EXIT_IF(address == 0);
	auto* value = reinterpret_cast<int*>(address);
	*value = 0;
	g_errno.address = value;
}

void ReleaseCurrentThreadError()
{
	if (g_errno.address == nullptr) return;
	EXIT_IF(!Core::VirtualMemory::Free(reinterpret_cast<uint64_t>(g_errno.address)));
	g_errno.address = nullptr;
}

LUMEN_SYSV_ABI int* GetErrorAddr()
{
	PrepareCurrentThreadError();
	return g_errno.address;
}

} // namespace Lumen::Libs::Posix

#endif
