#include "Lumen/Core/MemoryAlloc.h"

#include "Lumen/Core/ArrayWrapper.h" // IWYU pragma: keep
#include "Lumen/Core/Common.h"
#include "Lumen/Core/DateTime.h" // IWYU pragma: keep
#include "Lumen/Core/Debug.h"    // IWYU pragma: keep
#include "Lumen/Core/Hashmap.h"  // IWYU pragma: keep
#include "Lumen/Core/MemoryInitGate.h"
#include "Lumen/Core/RefCounterPolicy.h"
#include "Lumen/Sys/SysHeap.h"
#include "Lumen/Sys/SysSync.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace Lumen::Core {

#if LUMEN_ENABLE_MEMORY_TRACKER && !defined(LUMEN_SHARED_DLL) &&                                                                            \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS || LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX)
#define MEM_TRACKER
#endif

#define MEM_ALLOC_ALIGNED

#ifdef MEM_ALLOC_ALIGNED
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
constexpr int MEM_ALLOC_ALIGN = 8;
#else
constexpr int MEM_ALLOC_ALIGN = 16;
#endif
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_BITNESS == 64
[[maybe_unused]] constexpr int STACK_CHECK_FROM = 5;
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
[[maybe_unused]] constexpr int STACK_CHECK_FROM = 4;
#else
[[maybe_unused]] constexpr int STACK_CHECK_FROM = 2;
#endif

static SysCS*           g_mem_cs          = nullptr;
static MemoryDetail::InitGate g_mem_initialization;
static thread_local bool g_mem_bootstrap_active = false;
static sys_heap_id_t    g_default_heap    = nullptr;
static std::atomic_size_t g_mem_max_size {0};
static std::atomic_bool g_mem_native_heap_concurrency {false};

#ifdef MEM_TRACKER

using pattern_t = uint32_t;

constexpr size_t PATTERN_SIZE = (sizeof(pattern_t));
constexpr size_t PATTERNS_NUM = 4;

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init,hicpp-member-init)
struct MemBlockInfoT
{
	uintptr_t  addr;
	size_t     size;
	int        state;
	DebugStack stack;
	pattern_t  left_pattern;
	pattern_t  right_pattern;
};

thread_local int  g_mem_depth           = 0;
thread_local bool g_mem_tracker_enabled = true;

static Hashmap<uintptr_t, MemBlockInfoT*>* g_mem_map         = nullptr;
static int                                 g_mem_state       = 0;
static size_t                              g_total_allocated = 0;

#define LUMEN_MDBG(str, ptr)                                                                                                                \
	{                                                                                                                                      \
		if (false)                                                                                                                         \
		{                                                                                                                                  \
			printf("%s, %016" PRIx64 "\n", str, reinterpret_cast<uint64_t>(ptr));                                                          \
		}                                                                                                                                  \
	}

#endif

class MemLock
{
public:
	MemLock(): m_locked(!g_mem_native_heap_concurrency.load(std::memory_order_relaxed))
	{
		if (m_locked) g_mem_cs->Enter();

#ifdef MEM_TRACKER
		g_mem_depth++;
#endif
	}

	~MemLock()
	{
#ifdef MEM_TRACKER
		g_mem_depth--;
#endif

		if (m_locked) g_mem_cs->Leave();
	}

	LUMEN_CLASS_NO_COPY(MemLock);

#ifdef MEM_TRACKER
	// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
	[[nodiscard]] bool IsRecursive() const
	{
		return g_mem_depth > 1;
	}
#endif

private:
	bool m_locked;
};

#if LUMEN_COMPILER == LUMEN_COMPILER_MSVC
#pragma code_seg(push)
#endif

#if LUMEN_COMPILER == LUMEN_COMPILER_MSVC
#pragma code_seg(".mem_a")
#endif

#if LUMEN_COMPILER == LUMEN_COMPILER_MSVC
#pragma code_seg(".mem_b")
#endif

#ifdef MEM_TRACKER

static const Array<pattern_t, 4> g_mem_patterns = {0xAAAAAAAA, 0xCCCCCCCC, 0x55555555, 0x33333333};

static int g_pattern_id = 0;

static pattern_t pattern_next()
{
	return g_mem_patterns[g_pattern_id++ % g_mem_patterns.Size()];
}

static void pattern_write(MemBlockInfoT* info)
{
	auto* ptr = reinterpret_cast<uint8_t*>(info->addr);
	for (int i = 0; i < 4; i++)
	{
		(reinterpret_cast<pattern_t*>(ptr - PATTERN_SIZE * PATTERNS_NUM))[i] = info->left_pattern;
		(reinterpret_cast<pattern_t*>(ptr + info->size))[i]                  = info->right_pattern;
	}
}

static bool pattern_check(MemBlockInfoT* info)
{
	auto* ptr = reinterpret_cast<uint8_t*>(info->addr);

	for (int i = 0; i < 4; i++)
	{
		if ((reinterpret_cast<pattern_t*>(ptr - PATTERN_SIZE * PATTERNS_NUM))[i] != info->left_pattern ||
		    (reinterpret_cast<pattern_t*>(ptr + info->size))[i] != info->right_pattern)
		{
			return false;
		}
	}
	return true;
}

#endif

static void mem_init()
{
	// Bootstrap must not allocate through global new or std::call_once: the
	// tracker constructs containers that recursively enter this allocator.
	g_mem_initialization.Ensure(g_mem_bootstrap_active, [] {
		// Publish the recursive mutex to this bootstrap thread before constructing
		// anything that can allocate. Other threads wait for the final release.
		void* mutex_storage = std::malloc(sizeof(SysCS));
		if (mutex_storage == nullptr) std::abort();
		g_mem_cs = new (mutex_storage) SysCS;
		g_mem_cs->Init();

#ifdef MEM_TRACKER
		g_mem_depth++;
		srand(DateTime::FromSystemUTC().GetTime().MsecTotal());
		g_pattern_id = static_cast<int>(rand() % g_mem_patterns.Size()); // NOLINT(cert-msc30-c,cert-msc50-cpp)
		g_mem_map = new Hashmap<uintptr_t, MemBlockInfoT*>;
#endif

		g_default_heap = sys_heap_create();

#ifdef MEM_TRACKER
		g_mem_depth--;
#endif
	});
}

void core_memory_init()
{
	mem_init();
	mem_configure_native_heap_concurrency();
	ConfigureAtomicRefsFromEnvironment();
	if (std::getenv("LUMEN_NATIVE_HEAP_CONCURRENCY") != nullptr)
		std::fprintf(stderr, "[NATIVE_HEAP_CONCURRENCY] supported=%u enabled=%u\n",
		             mem_native_heap_concurrency_supported() ? 1u : 0u,
		             mem_native_heap_concurrency_enabled() ? 1u : 0u);
}

bool mem_native_heap_concurrency_supported()
{
	mem_init();
#if !defined(MEM_TRACKER) && (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || \
                            LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX)
	// These platforms select SysLinuxHeap's libc malloc/realloc/free backend.
	// A non-null/custom heap is deliberately not qualified by this candidate.
	return g_default_heap == nullptr;
#else
	return false;
#endif
}

void mem_configure_native_heap_concurrency()
{
	const bool supported = mem_native_heap_concurrency_supported();
	const char* value = std::getenv("LUMEN_NATIVE_HEAP_CONCURRENCY");
	const bool requested = value != nullptr && value[0] == '1' && value[1] == '\0';
	// The allocation family and pointer layout never change. Calls that began
	// under the legacy mutex can safely finish while a later call bypasses it.
	g_mem_native_heap_concurrency.store(supported && requested, std::memory_order_release);
}

bool mem_native_heap_concurrency_enabled()
{
	return g_mem_native_heap_concurrency.load(std::memory_order_acquire);
}

void* mem_alloc_check_alignment(void* ptr)
{
#ifdef MEM_ALLOC_ALIGNED
	if ((reinterpret_cast<uintptr_t>(ptr) & static_cast<uintptr_t>(MEM_ALLOC_ALIGN - 1)) != 0u)
	{
		EXIT("mem alloc not aligned!\n");
	}
#endif

	return ptr;
}

void* mem_alloc(size_t size)
{
	if (size == 0)
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX
		size = 1;
#else
		EXIT("size == 0\n");
#endif
	}

	const size_t max_size = g_mem_max_size.load(std::memory_order_relaxed);
	if ((max_size != 0u) && size > max_size)
	{
		EXIT("mem_alloc(): size(%" PRIu64 ") > max(%" PRIu64 ")\n", uint64_t(size), uint64_t(max_size));
	}

	mem_init();
	MemLock lock;

#ifdef MEM_TRACKER
	DebugStack stack;
	DebugStack::Trace(&stack);

	if (lock.IsRecursive() || !g_mem_tracker_enabled)
	{
		// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
		void* r = std::malloc(size);
		LUMEN_MDBG("- std alloc -", r);
		return mem_alloc_check_alignment(r);
	}
#endif

#ifdef MEM_TRACKER
	auto* ptr_p = static_cast<pattern_t*>(sys_heap_alloc(g_default_heap, size + PATTERN_SIZE * PATTERNS_NUM * 2));
	void* ptr   = ptr_p + PATTERNS_NUM;
#else
	void* ptr  = sys_heap_alloc(g_default_heap, size);
#endif

	if (ptr == nullptr)
	{
		EXIT("mem_alloc(): can't alloc %" PRIu64 " bytes\n", uint64_t(size));
	}

#ifdef MEM_TRACKER

	// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
	auto* info = static_cast<MemBlockInfoT*>(std::malloc(sizeof(MemBlockInfoT)));

	info->addr  = reinterpret_cast<uintptr_t>(ptr);
	info->size  = size;
	info->state = g_mem_state;
	stack.CopyTo(&info->stack);

	info->left_pattern  = pattern_next();
	info->right_pattern = pattern_next();

	pattern_write(info);

	g_mem_map->Put(reinterpret_cast<uintptr_t>(ptr), info);

	g_total_allocated += size;

	LUMEN_MDBG("- mem_alloc -", ptr);

#endif

	return mem_alloc_check_alignment(ptr);
}

void* mem_realloc(void* ptr, size_t size)
{
	EXIT_IF(size == 0);

	const size_t max_size = g_mem_max_size.load(std::memory_order_relaxed);
	if ((max_size != 0u) && size > max_size)
	{
		EXIT("mem_realloc(): size(%" PRIu64 ") > max(%" PRIu64 ")\n", uint64_t(size), uint64_t(max_size));
	}

	mem_init();
	MemLock lock;

#ifdef MEM_TRACKER
	DebugStack stack;
	DebugStack::Trace(&stack);

	if (lock.IsRecursive() || !g_mem_tracker_enabled)
	{

		// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
		void* ptr2 = std::realloc(ptr, size);
		LUMEN_MDBG("- std realloc old -", ptr);
		LUMEN_MDBG("- std realloc new -", ptr2);
		return mem_alloc_check_alignment(ptr2);
	}

	// Allocations made while tracking is disabled intentionally use the
	// system allocator. They may later be resized after tracking is restored
	// (Lua's long-lived containers do this), so keep them on the same heap.
	MemBlockInfoT* tracked_info = nullptr;
	if (ptr != nullptr)
	{
		MemBlockInfoT* const* info_p = g_mem_map->Find(reinterpret_cast<uintptr_t>(ptr));
		if (info_p == nullptr)
		{
			// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
			void* ptr2 = std::realloc(ptr, size);
			if (ptr2 == nullptr)
			{
				EXIT("mem_realloc(): can't alloc %" PRIu64 " bytes\n", uint64_t(size));
			}
			return mem_alloc_check_alignment(ptr2);
		}
		tracked_info = *info_p;
	}
#endif

#ifdef MEM_TRACKER
	auto* ptr2_b = static_cast<pattern_t*>(sys_heap_realloc(
	    g_default_heap, ptr != nullptr ? (static_cast<pattern_t*>(ptr)) - PATTERNS_NUM : nullptr, size + PATTERN_SIZE * PATTERNS_NUM * 2));
	void* ptr2   = ptr2_b + PATTERNS_NUM;
#else
	void* ptr2 = sys_heap_realloc(g_default_heap, ptr, size);
#endif

	if (ptr2 == nullptr)
	{
		EXIT("mem_realloc(): can't alloc %" PRIu64 " bytes\n", uint64_t(size));
	}

#ifdef MEM_TRACKER
	if (ptr != nullptr)
	{
		MemBlockInfoT* info = tracked_info;
		EXIT_IF(info == nullptr);

		g_total_allocated -= info->size;
		g_total_allocated += size;

		if (ptr == ptr2)
		{
			info->size  = size;
			info->state = g_mem_state;
			stack.CopyTo(&info->stack);
		} else
		{
			info->addr  = reinterpret_cast<uintptr_t>(ptr2);
			info->size  = size;
			info->state = g_mem_state;
			stack.CopyTo(&info->stack);
			g_mem_map->Put(reinterpret_cast<uintptr_t>(ptr2), info);
			g_mem_map->Remove(reinterpret_cast<uintptr_t>(ptr));
		}

		pattern_write(info);
	} else
	{

		// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
		auto* info  = static_cast<MemBlockInfoT*>(std::malloc(sizeof(MemBlockInfoT)));
		info->addr  = reinterpret_cast<uintptr_t>(ptr2);
		info->size  = size;
		info->state = g_mem_state;
		stack.CopyTo(&info->stack);

		info->left_pattern  = pattern_next();
		info->right_pattern = pattern_next();

		pattern_write(info);

		g_total_allocated += size;

		g_mem_map->Put(reinterpret_cast<uintptr_t>(ptr2), info);
	}

	LUMEN_MDBG("- mem_realloc old -", ptr);
	LUMEN_MDBG("- mem_realloc new -", ptr2);
#endif

	// g_mem_cs->Leave();

	return mem_alloc_check_alignment(ptr2);
}

void mem_free(void* ptr)
{
	if (ptr == nullptr) return;
	mem_init();

#ifdef MEM_TRACKER
	// A recursive allocation made while constructing the tracking map uses
	// std::malloc. The map cannot be consulted until its constructor returns.
	if (g_mem_bootstrap_active && g_mem_map == nullptr)
	{
		std::free(ptr);
		return;
	}
#endif

	MemLock lock;

#ifdef MEM_TRACKER
	MemBlockInfoT* const* info = g_mem_map->Find(reinterpret_cast<uintptr_t>(ptr));

	if (info != nullptr)
	{

		if (!pattern_check(*info))
		{
			EXIT("memory overflow\n");
		}

		g_total_allocated -= (*info)->size;
		// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
		std::free(*info);
		g_mem_map->Remove(reinterpret_cast<uintptr_t>(ptr));
		// printf("heap_free: %x\n", uintptr_t(ptr));
#endif

#ifdef MEM_TRACKER
		sys_heap_free(g_default_heap, ptr != nullptr ? (static_cast<pattern_t*>(ptr)) - PATTERNS_NUM : nullptr);
#else
	sys_heap_free(g_default_heap, ptr);
#endif

#ifdef MEM_TRACKER
	} else
	{
		if (ptr != nullptr)
		{
			// printf("free: %x\n", uintptr_t(ptr));
			// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
			std::free(ptr);
		}
	}

	LUMEN_MDBG("- mem_free -", ptr);
#endif
}

bool mem_check([[maybe_unused]] const void* ptr)
{
#ifdef MEM_TRACKER
	EXIT_IF(!g_mem_initialization.Ready());

	MemLock lock;

	MemBlockInfoT* const* info = g_mem_map->Find(reinterpret_cast<uintptr_t>(ptr));

	return info != nullptr && pattern_check(*info);
#else
	return true;
#endif
}

#ifdef MEM_TRACKER
static bool LUMEN_HASH_CALL mem_map_print_callback(const uintptr_t* /*key*/, MemBlockInfoT* const* value, void* arg)
{
	int state = static_cast<int>(reinterpret_cast<intptr_t>(arg));

	if (state <= (*value)->state)
	{
		if (sizeof(uintptr_t) == 4)
		{
			printf("\n%08" PRIx32 ", %" PRIu32 ", %d\n", static_cast<uint32_t>((*value)->addr), static_cast<uint32_t>((*value)->size),
			       (*value)->state);
			(*value)->stack.Print(STACK_CHECK_FROM);
		} else
		{
			printf("\n%016" PRIx64 ", %" PRIu64 ", %d\n", static_cast<uint64_t>((*value)->addr), static_cast<uint64_t>((*value)->size),
			       (*value)->state);
			(*value)->stack.Print(STACK_CHECK_FROM);
		}
	}
	return true;
}

static bool LUMEN_HASH_CALL mem_map_stat_callback(const uintptr_t* /*key*/, MemBlockInfoT* const* value, void* arg)
{
	auto* s = static_cast<MemStats*>(arg);

	if (s->state <= (*value)->state)
	{
		s->total_allocated += (*value)->size;
		s->blocks_num++;
	}
	return true;
}
#endif

void mem_get_stat(MemStats* s)
{
#ifdef MEM_TRACKER
	if (!g_mem_initialization.Ready())
	{
#endif
		s->total_allocated = 0;
		s->blocks_num      = 0;
#ifdef MEM_TRACKER
		return;
	}

	MemLock lock;

	if (s->state == 0)
	{
		s->total_allocated = g_total_allocated;
		s->blocks_num      = g_mem_map->Size();
	} else
	{
		s->total_allocated = 0;
		s->blocks_num      = 0;

		g_mem_map->ForEach(mem_map_stat_callback, static_cast<void*>(s));
	}

#endif
}

int mem_new_state()
{
#ifdef MEM_TRACKER
	if (!g_mem_initialization.Ready())
	{
#endif
		return 0;
#ifdef MEM_TRACKER
	}

	MemLock lock;

	g_mem_state++;

	return g_mem_state;
#endif
}

void mem_print(int from_state)
{
#ifdef MEM_TRACKER
	if (!g_mem_initialization.Ready())
	{
		return;
	}

	MemLock lock;

	intptr_t s = from_state;

	g_mem_map->ForEach(mem_map_print_callback, reinterpret_cast<void*>(s));

#endif
}

} // namespace Lumen::Core

#if !defined(LUMEN_SHARED_DLL) && LUMEN_PLATFORM != LUMEN_PLATFORM_OSX && LUMEN_PLATFORM != LUMEN_PLATFORM_IOS
void* operator new(size_t size)
{
	return Lumen::Core::mem_alloc(size);
}

void* operator new(std::size_t size, const std::nothrow_t& /*nothrow_value*/) noexcept
{
	return Lumen::Core::mem_alloc(size);
}

void* operator new[](size_t size)
{
	return Lumen::Core::mem_alloc(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& /*nothrow_value*/) noexcept
{
	return Lumen::Core::mem_alloc(size);
}

void operator delete(void* block) noexcept
{
	Lumen::Core::mem_free(block);
}

void operator delete[](void* block) noexcept
{
	Lumen::Core::mem_free(block);
}
#else
//#error "haha"
#endif

extern "C" {
void* mem_alloc_c(size_t size)
{
	return Lumen::Core::mem_alloc(size);
}

void* mem_realloc_c(void* ptr, size_t size)
{
	return Lumen::Core::mem_realloc(ptr, size);
}

void mem_free_c(void* ptr)
{
	Lumen::Core::mem_free(ptr);
}
}

namespace Lumen::Core {

bool mem_tracker_enabled()
{
#ifdef MEM_TRACKER
	return g_mem_tracker_enabled;
#else
	return false;
#endif
}

void mem_tracker_enable()
{
#ifdef MEM_TRACKER
	g_mem_tracker_enabled = true;
#endif
}

void mem_tracker_disable()
{
#ifdef MEM_TRACKER
	g_mem_tracker_enabled = false;
#endif
}

#if LUMEN_COMPILER == LUMEN_COMPILER_MSVC
#pragma code_seg(".mem_c")
#endif

#if LUMEN_COMPILER == LUMEN_COMPILER_MSVC
#pragma code_seg(pop)
#endif

void mem_set_max_size(size_t size)
{
	g_mem_max_size.store(size, std::memory_order_relaxed);
}

} // namespace Lumen::Core
