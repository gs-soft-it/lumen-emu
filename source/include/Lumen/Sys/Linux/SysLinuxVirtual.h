#ifndef INCLUDE_LUMEN_SYS_LINUX_SYSLINUXVIRTUAL_H_
#define INCLUDE_LUMEN_SYS_LINUX_SYSLINUXVIRTUAL_H_

#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID && LUMEN_PLATFORM != LUMEN_PLATFORM_LINUX && LUMEN_PLATFORM != LUMEN_PLATFORM_OSX && \
    LUMEN_PLATFORM != LUMEN_PLATFORM_IOS
//#error "LUMEN_PLATFORM != LUMEN_PLATFORM_LINUX"
#else

#include "Lumen/Core/Common.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/VirtualMemory.h"

namespace Lumen::Core {

void sys_get_system_info(SystemInfo* info);

void sys_virtual_init();

uint64_t sys_virtual_alloc(uint64_t address, uint64_t size, VirtualMemory::Mode mode);
uint64_t sys_virtual_alloc_aligned(uint64_t address, uint64_t size, VirtualMemory::Mode mode, uint64_t alignment);
bool     sys_virtual_alloc_fixed(uint64_t address, uint64_t size, VirtualMemory::Mode mode);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
bool sys_virtual_requires_uniform_host_page_data_protection();
VirtualMemory::ManagedRegionState sys_virtual_query_managed_region(uint64_t address, VirtualMemory::ManagedRegion* region);
// Internal data preflight: describe only the portion from address to end,
// without scanning unrelated equal-permission pages before or after it.
// The caller retains the same Core/FEX memory admission as an ordinary query.
VirtualMemory::ManagedRegionState sys_virtual_query_managed_data_region(uint64_t address, uint64_t end,
                                                                      VirtualMemory::ManagedRegion* region);
bool                              sys_virtual_is_managed_address(uint64_t address);
// Exact owner extent for the base-only Free API; queried under the Core FEX
// mutation barrier. This read-only query never grants memory access.
bool sys_virtual_query_allocation_range(uint64_t address, uint64_t* begin, uint64_t* size);
bool sys_virtual_query_backing_span(uint64_t address, uint64_t maximum_bytes,
                                   VirtualMemory::GuestBackingSpan* span) noexcept;
VirtualMemory::GuestBackingPresence sys_virtual_query_backing_presence(
    uint64_t identity, uint64_t offset, uint64_t bytes) noexcept;
#endif
bool     sys_virtual_free(uint64_t address);
bool     sys_virtual_protect(uint64_t address, uint64_t size, VirtualMemory::Mode mode, VirtualMemory::Mode* old_mode = nullptr);
bool     sys_virtual_flush_instruction_cache(uint64_t address, uint64_t size);
bool     sys_virtual_patch_replace(uint64_t vaddr, uint64_t value);

} // namespace Lumen::Core

#endif

#endif /* INCLUDE_LUMEN_SYS_LINUX_SYSLINUXVIRTUAL_H_ */
