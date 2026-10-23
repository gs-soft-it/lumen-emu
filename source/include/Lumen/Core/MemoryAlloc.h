#ifndef INCLUDE_LUMEN_CORE_MEMORYALLOC_H_
#define INCLUDE_LUMEN_CORE_MEMORYALLOC_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"

namespace Lumen::Core {

// SUBSYSTEM_DEFINE(Memory);
void core_memory_init();

// Refresh after process/session environment is configured. Exact value "1"
// opts in; all other values keep the legacy allocator serialization. Only
// qualified libc heaps without compiled memory tracking can enable it.
void mem_configure_native_heap_concurrency();
bool mem_native_heap_concurrency_supported();
bool mem_native_heap_concurrency_enabled();

struct MemStats
{
	int      state;
	size_t   total_allocated;
	uint32_t blocks_num;
};

void* mem_alloc(size_t size);
void* mem_realloc(void* ptr, size_t size);
void  mem_free(void* ptr);
void  mem_print(int from_state);
void  mem_get_stat(MemStats* s);
void  mem_set_max_size(size_t size);
int   mem_new_state();
bool  mem_tracker_enabled();
void  mem_tracker_enable();
void  mem_tracker_disable();
bool  mem_check(const void* ptr);

#define LUMEN_MEM_CHECK(ptr) EXIT_IF(!mem_check(ptr))

} // namespace Lumen::Core

#endif /* INCLUDE_LUMEN_CORE_MEMORYALLOC_H_ */
