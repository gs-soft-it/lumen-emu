#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_MEMORY_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_MEMORY_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/Subsystems.h"

#include "Emulator/Common.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::LibKernel::Memory {

LUMEN_SUBSYSTEM_DEFINE(Memory);

using callback_func_t = void (*)(uintptr_t addr, size_t size);

struct alignas(8) OrbisVirtualQueryInfo
{
	uint64_t start;
	uint64_t end;
	uint64_t offset;
	int32_t  protection;
	int32_t  memory_type;
	uint8_t  category_flags;
	char     name[32];
	uint8_t  reserved[7];
};

// Callback registration normally happens during guest startup. On a FEX guest
// thread it fails closed instead of waiting behind an active memory lifecycle,
// because that wait would keep the FEX host-call reader acquired.
[[nodiscard]] bool RegisterCallbacks(callback_func_t alloc_func, callback_func_t free_func);

// Releases every remaining guest physical/flexible mapping after GPU/HLE
// owners have been purged. Allocation callbacks are invoked while their guest
// ranges are still known, then detached for the following session.
[[nodiscard]] bool ResetForNextSession();

int32_t LUMEN_SYSV_ABI KernelReserveVirtualRange(void** address, uint64_t length, int32_t flags, uint64_t alignment);
int LUMEN_SYSV_ABI     KernelMapNamedFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags, const char* name);
int LUMEN_SYSV_ABI     KernelMapFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags);
int LUMEN_SYSV_ABI     KernelMunmap(uint64_t vaddr, size_t len);
size_t LUMEN_SYSV_ABI  KernelGetDirectMemorySize();
uint32_t LUMEN_SYSV_ABI KernelIsAddressSanitizerEnabled();
int LUMEN_SYSV_ABI     KernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len, size_t alignment, int memory_type,
                                                  int64_t* phys_addr_out);
int LUMEN_SYSV_ABI     KernelAllocateMainDirectMemory(size_t len, size_t alignment, int memory_type, int64_t* phys_addr_out);
int LUMEN_SYSV_ABI     KernelCheckedReleaseDirectMemory(int64_t start, size_t len);
int LUMEN_SYSV_ABI     KernelReleaseDirectMemory(int64_t start, size_t len);
int LUMEN_SYSV_ABI     KernelMapDirectMemory(void** addr, size_t len, int prot, int flags, int64_t direct_memory_start, size_t alignment);
int LUMEN_SYSV_ABI     KernelMapNamedDirectMemory(void** addr, size_t len, int prot, int flags, off_t direct_memory_start, size_t alignment,
                                                  const char* name);
int LUMEN_SYSV_ABI     KernelQueryMemoryProtection(void* addr, void** start, void** end, int* prot);
int LUMEN_SYSV_ABI     KernelDirectMemoryQuery(int64_t offset, int flags, void* info, size_t info_size);
int LUMEN_SYSV_ABI     KernelAvailableFlexibleMemorySize(size_t* size);
int LUMEN_SYSV_ABI     KernelMprotect(const void* addr, size_t len, int prot);
// Re-protection for HLE materialisations that install contents while a range
// is writable and only then expose the guest-requested protection
// (file-backed posix_mmap). Unlike KernelMprotect this is not a guest export
// and it accepts the POSIX prot bits 0..7 of the Posix_v1 mmap import.
int LUMEN_SYSV_ABI     KernelApplyPosixProtection(uint64_t vaddr, size_t len, int prot);
int32_t LUMEN_SYSV_ABI KernelSetVirtualRangeName(uint64_t address, uint64_t length, uint64_t guest_name);
int32_t LUMEN_SYSV_ABI KernelVirtualQuery(uint64_t address, int32_t flags, uint64_t guest_info, uint64_t info_size);

#if defined(LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK)
enum class VirtualRangeFaultForTest : uint32_t
{
	None,
	HleSlotPreflight,
	FinalGuestWrite,
};

struct VirtualRangeStateForTest
{
	size_t   live            = 0;
	size_t   pending_cleanup = 0;
	uint64_t first_base      = 0;
};

struct MemoryLifecycleStateForTest
{
	uint64_t active      = 0;
	uint64_t waiting     = 0;
	uint64_t try_denials = 0;
};

struct RangeNameStateForTest
{
	size_t live     = 0;
	size_t capacity = 0;
};

struct GpuOwnerLifecycleStateForTest
{
	uint64_t allocations             = 0;
	uint64_t waits                   = 0;
	uint64_t frees                   = 0;
	uint64_t last_allocation_address = 0;
	uint64_t last_allocation_size    = 0;
	uint64_t last_free_address       = 0;
	uint64_t last_free_size          = 0;
};

void                          FailNextVirtualRangeOperationForTest(VirtualRangeFaultForTest fault);
VirtualRangeStateForTest      GetVirtualRangeStateForTest();
MemoryLifecycleStateForTest   GetMemoryLifecycleStateForTest();
RangeNameStateForTest         GetRangeNameStateForTest();
void                          SetGpuOwnerLifecycleProbeForTest(bool enabled);
GpuOwnerLifecycleStateForTest GetGpuOwnerLifecycleStateForTest();
#endif

} // namespace Lumen::Libs::LibKernel::Memory

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_MEMORY_H_ */
