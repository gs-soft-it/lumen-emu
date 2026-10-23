#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Vector.h"
#include "Lumen/Core/VirtualMemory.h"
#include "Lumen/Sys/SysDbg.h"

#include "Emulator/Common.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <algorithm>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("DbgAddressSanitizer", 1, "DbgAddressSanitizer", 1, 1);

namespace DbgAddressSanitizer {

struct SourceLocation
{
	const char* filename;
	int         line_no;
	int         column_no;
};

struct Global
{
	uintptr_t       beg;
	uintptr_t       size;
	uintptr_t       size_with_redzone;
	const char*     name;
	const char*     module_name;
	uintptr_t       has_dynamic_init;
	SourceLocation* location;
	uintptr_t       odr_indicator;
};

struct Shadow
{
	uintptr_t start = 0;
	int       count = 0;
};

struct AsanContext
{
	Core::Mutex    mutex;
	Vector<Shadow> shadows;
};

static uint32_t     g_fake_stack = 0;
static AsanContext* g_ctx        = nullptr;

bool ResetMemoryInstrumentationSessionState()
{
	if (g_ctx == nullptr)
	{
		g_fake_stack = 0;
		return true;
	}

	bool complete = true;
	{
		Core::LockGuard lock(g_ctx->mutex);
		for (const auto& shadow: g_ctx->shadows)
		{
			if (shadow.start != 0 && !Core::VirtualMemory::Free(shadow.start))
			{
				complete = false;
			}
		}
		g_ctx->shadows.Clear();
	}
	delete g_ctx;
	g_ctx        = nullptr;
	g_fake_stack = 0;
	return complete;
}

static void AllocShadow(uintptr_t min_addr, uintptr_t max_addr)
{
	EXIT_IF(g_ctx == nullptr);

	Core::LockGuard lock(g_ctx->mutex);

	static const uintptr_t page_size = 0x10000ull;

	auto min_shadow = ((min_addr >> 3u) + 0x10000000000ull) & ~(page_size - 1);
	auto max_shadow = ((max_addr >> 3u) + 0x10000000000ull) & ~(page_size - 1);

	auto size = max_shadow - min_shadow + page_size;

	printf("\t shadow = %016" PRIx64 "\n", static_cast<uint64_t>(min_shadow));
	printf("\t size   = %016" PRIx64 "\n", static_cast<uint64_t>(size));

	for (uintptr_t page = min_shadow; page <= max_shadow; page += page_size)
	{
		if (auto index = g_ctx->shadows.Find(page, [](auto& s, auto p) { return s.start == p; }); g_ctx->shadows.IndexValid(index))
		{
			g_ctx->shadows[index].count++;
		} else
		{
			if (!Core::VirtualMemory::AllocFixed(page, page_size, Core::VirtualMemory::Mode::ReadWrite))
			{
				EXIT("can't allocate shadow memory\n");
			}

			Shadow s;
			s.start = page;
			s.count = 1;
			g_ctx->shadows.Add(s);
		}
	}
}

static void FreeShadow(uintptr_t min_addr, uintptr_t max_addr)
{
	EXIT_IF(g_ctx == nullptr);

	Core::LockGuard lock(g_ctx->mutex);

	static const uintptr_t page_size = 0x10000ull;

	auto min_shadow = ((min_addr >> 3u) + 0x10000000000ull) & ~(page_size - 1);
	auto max_shadow = ((max_addr >> 3u) + 0x10000000000ull) & ~(page_size - 1);

	auto size = max_shadow - min_shadow + page_size;

	printf("\t shadow = %016" PRIx64 "\n", static_cast<uint64_t>(min_shadow));
	printf("\t size   = %016" PRIx64 "\n", static_cast<uint64_t>(size));

	for (uintptr_t page = min_shadow; page <= max_shadow; page += page_size)
	{
		if (auto index = g_ctx->shadows.Find(page, [](auto& s, auto p) { return s.start == p; }); g_ctx->shadows.IndexValid(index))
		{
			g_ctx->shadows[index].count--;

			if (g_ctx->shadows[index].count <= 0)
			{
				if (!Core::VirtualMemory::Free(page))
				{
					EXIT("can't free shadow memory\n");
				}

				g_ctx->shadows.RemoveAt(index);
			}
		}
	}
}

static void KernelAlloc(uintptr_t addr, size_t size)
{
	AllocShadow(addr, addr + size - 1);
}

static void KernelFree(uintptr_t addr, size_t size)
{
	FreeShadow(addr, addr + size - 1);
}

static void LUMEN_SYSV_ABI asan_init()
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(g_ctx != nullptr);

	g_ctx = new AsanContext;

	sys_dbg_stack_info_t s {};
	sys_stack_usage(s);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX ||          \
    LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	AllocShadow(s.addr, reinterpret_cast<uintptr_t>(&s));
#else
	AllocShadow(s.reserved_addr, reinterpret_cast<uintptr_t>(&s));
#endif

	if (!LibKernel::Memory::RegisterCallbacks(KernelAlloc, KernelFree))
	{
		EXIT("unable to register address-sanitizer memory callbacks\n");
	}
}

static void LUMEN_SYSV_ABI asan_version_mismatch_check_v8()
{
	PRINT_NAME();
}

static void LUMEN_SYSV_ABI asan_register_elf_globals(uintptr_t* flag, Global* start, Global* stop)
{
	PRINT_NAME();

	printf("\t flag  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(flag));
	printf("\t start = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(start));
	printf("\t stop  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(stop));

	if (flag == nullptr || start == nullptr || stop == nullptr)
	{
		return;
	}

	if (*flag != 0)
	{
		return;
	}

	auto num = stop - start;

	if (num != 0)
	{
		uintptr_t min_addr = UINTPTR_MAX;
		uintptr_t max_addr = 0;

		for (ptrdiff_t i = 0; i < num; i++)
		{
			auto* g = start + i;

			printf("asan global:\n");
			printf("\t beg               = %016" PRIx64 "\n", static_cast<uint64_t>(g->beg));
			printf("\t size              = %016" PRIx64 "\n", static_cast<uint64_t>(g->size));
			printf("\t size_with_redzone = %016" PRIx64 "\n", static_cast<uint64_t>(g->size_with_redzone));
			printf("\t name              = %s\n", g->name);
			printf("\t module_name       = %s\n", g->module_name);
			printf("\t has_dynamic_init  = %" PRIu64 "\n", static_cast<uint64_t>(g->has_dynamic_init));
			printf("\t odr_indicator     = %016" PRIx64 "\n", static_cast<uint64_t>(g->odr_indicator));
			if (g->location != nullptr)
			{
				printf("\t location          = %s:%d\n", g->location->filename, g->location->line_no);
			}

			min_addr = std::min(g->beg, min_addr);
			max_addr = std::max(g->beg + g->size_with_redzone - 1, max_addr);
		}

		AllocShadow(min_addr, max_addr);
	}

	*flag = 0;
}

static void LUMEN_SYSV_ABI asan_before_dynamic_init(const char* module_name)
{
	PRINT_NAME();

	printf("\t name = %s\n", module_name);
}

static void LUMEN_SYSV_ABI asan_after_dynamic_init()
{
	PRINT_NAME();
}

static void* LUMEN_SYSV_ABI asan_memcpy(void* dst, const void* src, size_t size)
{
	return std::memcpy(dst, src, size);
}

static void* LUMEN_SYSV_ABI asan_memset(void* dst, int ch, size_t size)
{
	return std::memset(dst, ch, size);
}

} // namespace DbgAddressSanitizer

bool ResetMemoryInstrumentationSessionState()
{
	return DbgAddressSanitizer::ResetMemoryInstrumentationSessionState();
}

LIB_DEFINE(InitDbgAddressSanitizer_1)
{
	LIB_OBJECT("OmecxEgrhCs", &DbgAddressSanitizer::g_fake_stack);

	LIB_FUNC("xLoVJKHvRMU", DbgAddressSanitizer::asan_init);
	LIB_FUNC("GY2I-Okc7q0", DbgAddressSanitizer::asan_version_mismatch_check_v8);
	LIB_FUNC("W+2HQAGdPuU", DbgAddressSanitizer::asan_register_elf_globals);
	LIB_FUNC("G9xSYlaNv+Y", DbgAddressSanitizer::asan_before_dynamic_init);
	LIB_FUNC("z5Gy5T3tptE", DbgAddressSanitizer::asan_after_dynamic_init);
	LIB_FUNC("g+Neom2EHO8", DbgAddressSanitizer::asan_memcpy);
	LIB_FUNC("q-hZf47nHhQ", DbgAddressSanitizer::asan_memset);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
