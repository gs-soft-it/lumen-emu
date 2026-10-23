#include "libs/errno.h"
#include "common/assert.h"
#include "kernel/memory.h"

namespace Libs::Posix {

namespace {
struct ThreadErrorStorage {
	int* address = nullptr;
	~ThreadErrorStorage() { ReleaseCurrentThreadError(); }
};
thread_local ThreadErrorStorage g_errno;
}

void PrepareCurrentThreadError() {
	if (g_errno.address != nullptr) return;
	const auto address = LibKernel::Memory::AllocateRuntimeMemory(
	    0, 0x4000, Common::VirtualMemory::Mode::ReadWrite, "thread errno ABI");
	EXIT_IF(address == 0);
	const int initial = 0;
	EXIT_IF(!LibKernel::Memory::WriteGuestMemory(nullptr, address, &initial, sizeof(initial)));
	g_errno.address = reinterpret_cast<int*>(address);
}

void ReleaseCurrentThreadError() {
	if (g_errno.address == nullptr) return;
	EXIT_IF(!LibKernel::Memory::FreeGuestMemory(reinterpret_cast<uint64_t>(g_errno.address), 0x4000));
	g_errno.address = nullptr;
}

KYTY_SYSV_ABI int* GetErrorAddr() {
	PrepareCurrentThreadError();
	return g_errno.address;
}

} // namespace Libs::Posix
