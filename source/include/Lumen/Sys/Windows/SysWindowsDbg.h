#ifndef SYS_WIN32_INCLUDE_LUMEN_SYSDBG_H_
#define SYS_WIN32_INCLUDE_LUMEN_SYSDBG_H_

// IWYU pragma: private

#include "Lumen/Core/Common.h"

#if LUMEN_PLATFORM != LUMEN_PLATFORM_WINDOWS
//#error "LUMEN_PLATFORM != LUMEN_PLATFORM_WINDOWS"
#else

namespace Lumen {

// NOLINTNEXTLINE(readability-identifier-naming)
struct sys_dbg_stack_info_t
{
	uintptr_t addr;

	uintptr_t reserved_addr;
	size_t    reserved_size;
	uintptr_t guard_addr;
	size_t    guard_size;
	uintptr_t commited_addr;
	size_t    commited_size;

	size_t total_size;
};

using exception_filter_func_t = void (*)(void* addr);

void sys_stack_walk(void** stack, int* depth);
void sys_stack_usage(sys_dbg_stack_info_t& s);           // NOLINT(google-runtime-references)
void sys_stack_usage_print(sys_dbg_stack_info_t& stack); // NOLINT(google-runtime-references)
void sys_get_code_info(uintptr_t* addr, size_t* size);
void sys_set_exception_filter(exception_filter_func_t func);

} // namespace Lumen

#endif

#endif /* SYS_WIN32_INCLUDE_LUMEN_SYSDBG_H_ */
