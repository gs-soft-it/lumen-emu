#include "Lumen/Core/Common.h"

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS

#include "Lumen/Sys/SysDbg.h"

#include <cstdio>

namespace Lumen {

static thread_local sys_dbg_stack_info_t g_stack = {};

void sys_stack_walk(void** /*stack*/, int* depth)
{
	*depth = 0;
}

void sys_stack_usage_print(sys_dbg_stack_info_t& stack)
{
	printf("stack: (0x%" PRIx64 ", %" PRIu64 ")\n", static_cast<uint64_t>(stack.commited_addr),
	       static_cast<uint64_t>(stack.commited_size));
	printf("code: (0x%" PRIx64 ", %" PRIu64 ")\n", static_cast<uint64_t>(stack.code_addr),
	       static_cast<uint64_t>(stack.code_size));
}

void sys_stack_usage(sys_dbg_stack_info_t& stack)
{
	stack = {};
}

void sys_get_code_info(uintptr_t* addr, size_t* size)
{
	if (g_stack.code_size == 0)
	{
		sys_stack_usage(g_stack);
	}
	*addr = g_stack.code_addr;
	*size = g_stack.code_size;
}

void sys_set_exception_filter(exception_filter_func_t /*func*/) {}

} // namespace Lumen

#endif
