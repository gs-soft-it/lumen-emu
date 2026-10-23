#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/LinkList.h"
#include "Lumen/Core/MSpace.h"
#include "Lumen/Core/Singleton.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/Printf.h"
#include "Emulator/Libs/VaContext.h"
#include "Emulator/Loader/HostCallScalar.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <limits>
#include <mutex>
#include <string>

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
#include "AndroidFexCoreRuntime.h"
#endif

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

static void ResetNeedFlag(uint32_t* flag)
{
	if (flag == nullptr) return;
#if defined(LUMEN_EMU_HOST_ARM64)
	Core::VirtualMemory::GuestDataLease lease;
	EXIT_IF(!Core::VirtualMemory::TryAcquireGuestDataLease(
	    reinterpret_cast<uint64_t>(flag), sizeof(*flag), Core::VirtualMemory::Mode::Write, &lease));
#endif
	*flag = 1;
}

static void PrepareNeedFlag(uint32_t*& flag, std::once_flag& once)
{
	// Export the managed backing itself. Guest writes and session resets must
	// refer to the same persistent object, rather than a copied host static.
	std::call_once(once, [&flag] {
		constexpr uint64_t page_size = 16 * 1024;
		const auto address = Core::VirtualMemory::Alloc(0, page_size, Core::VirtualMemory::Mode::ReadWrite);
		EXIT_IF(address == 0);
		flag = reinterpret_cast<uint32_t*>(address);
		ResetNeedFlag(flag);
	});
}

namespace LibC {

LIB_VERSION("libc", 1, "libc", 1, 1);

static uint32_t* g_need_flag = nullptr;
static std::once_flag g_need_flag_once;

using cxa_destructor_func_t = void (*)(void*);

struct CxaDestructor
{
	cxa_destructor_func_t destructor_func;
	void*                 destructor_object;
	void*                 module_id;
};

struct CContext
{
	Core::List<CxaDestructor> cxa;
#if defined(LUMEN_EMU_HOST_ARM64)
	Core::List<void (*)()> at_exit;
#endif
};

void ResetSessionState()
{
	auto* context = Core::Singleton<CContext>::Instance();
	context->cxa.Clear();
#if defined(LUMEN_EMU_HOST_ARM64)
	context->at_exit.Clear();
#endif
	ResetNeedFlag(g_need_flag);
}

#if defined(LUMEN_EMU_HOST_ARM64)
static void run_guest_atexit_callbacks()
{
	auto* cc = Core::Singleton<CContext>::Instance();

	while (cc->at_exit.Size() != 0)
	{
		auto index    = cc->at_exit.Last();
		auto callback = cc->at_exit[index];
		cc->at_exit.Remove(index);
		if (Loader::IsNativeHostCallTarget(reinterpret_cast<uint64_t>(callback)))
		{
			callback();
		} else
		{
			Loader::RunGuestX64(reinterpret_cast<uint64_t>(callback));
		}
	}
}
#endif

static LUMEN_SYSV_ABI void exit(int code)
{
	PRINT_NAME();
	LUMEN_LOGE("Guest libc exit(%d)\n", code);
	Libs::DumpLastCalls();

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	(void)Loader::RequestGuestSessionExit();
#else
	::exit(code);
#endif
}

static LUMEN_SYSV_ABI void init_env()
{
	PRINT_NAME();
}

static LUMEN_SYSV_ABI int atexit(void (*func)())
{
	PRINT_NAME();

	::printf("func = %" PRIx64 "\n", reinterpret_cast<uint64_t>(func));

#if defined(LUMEN_EMU_HOST_ARM64)
	EXIT_NOT_IMPLEMENTED(func == nullptr);

	auto* cc = Core::Singleton<CContext>::Instance();

	static const int ok = ::atexit(run_guest_atexit_callbacks);
	EXIT_NOT_IMPLEMENTED(ok != 0);

	cc->at_exit.Add(func);
#else
	int ok = ::atexit(func);

	EXIT_NOT_IMPLEMENTED(ok != 0);
#endif

	return 0;
}

static LUMEN_SYSV_ABI int libc_printf(VA_ARGS)
{
	VA_CONTEXT(ctx); // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

	PRINT_NAME();

	return GetPrintfCtxFunc()(&ctx);
}

static LUMEN_SYSV_ABI int puts(const char* s)
{
	PRINT_NAME();

	return GetPrintfStdFunc()("%s\n", s);
}

// Standard memory/string routines are also imported from LibcInternal, which
// is provided by HLE on every host. ARM64 additionally uses these helpers for
// libc imports to avoid interpreting the guest's byte/string loops. Preserve
// the guest calling convention on native x86-64 hosts as well.
namespace {

class GuestRange
{
public:
	bool Acquire(const void* pointer, size_t size, bool write)
	{
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID) && defined(LUMEN_FEXCORE_RUNTIME)
		if (size == 0) return true;
		const auto address = reinterpret_cast<uint64_t>(pointer);
		if (!Core::VirtualMemory::TryAcquireGuestDataLease(
		        address, size, write ? Core::VirtualMemory::Mode::Write : Core::VirtualMemory::Mode::Read, &m_lease))
		{
			Loader::ReportGuestMemoryFault(address, size, write);
			return false;
		}
#else
		(void)pointer;
		(void)size;
		(void)write;
#endif
		return true;
	}

private:
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID) && defined(LUMEN_FEXCORE_RUNTIME)
	Core::VirtualMemory::GuestDataLease m_lease;
#endif
};

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
bool CopyGuestString(const char* pointer, std::string* text,
	                 size_t limit = (std::numeric_limits<size_t>::max)())
{
	return Loader::ReadGuestString(reinterpret_cast<uint64_t>(pointer), limit, text);
}

char* OriginalStringResult(const char* original, const std::string& copy, const char* result)
{
	return result == nullptr ? nullptr : const_cast<char*>(original) + (result - copy.c_str());
}

int CompareGuestStrings(const char* lhs, const char* rhs, size_t limit)
{
	uint64_t left_address = reinterpret_cast<uint64_t>(lhs);
	uint64_t right_address = reinterpret_cast<uint64_t>(rhs);
	std::string left, right;
	while (limit != 0)
	{
		// Compare each readable prefix before approaching either next page.
		// strncmp may finish at a mismatch without consuming its full bound.
		const size_t count = std::min({size_t(4096 - left_address % 4096),
		                              size_t(4096 - right_address % 4096), limit});
		if (!Loader::ReadGuestString(left_address, count, &left) ||
		    !Loader::ReadGuestString(right_address, count, &right)) return 0;
		const int result = std::memcmp(left.data(), right.data(), std::min(left.size(), right.size()));
		if (result != 0) return result;
		if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
		if (left.size() < count || count == limit) return 0;
		if (left_address > UINT64_MAX - count || right_address > UINT64_MAX - count)
		{
			Loader::ReportGuestMemoryFault(left_address > UINT64_MAX - count ? left_address : right_address, count, false);
			return 0;
		}
		left_address += count;
		right_address += count;
		limit -= count;
	}
	return 0;
}

char* FindGuestCharacter(const char* str, int value)
{
	uint64_t address = reinterpret_cast<uint64_t>(str);
	const auto character = static_cast<unsigned char>(value);
	std::string text;
	for (;;)
	{
		const size_t count = 4096 - address % 4096;
		if (!Loader::ReadGuestString(address, count, &text)) return nullptr;
		const auto* result = static_cast<const char*>(std::memchr(text.data(), character, text.size()));
		if (result != nullptr)
			return reinterpret_cast<char*>(address + static_cast<size_t>(result - text.data()));
		if (text.size() < count)
			return character == 0 ? reinterpret_cast<char*>(address + text.size()) : nullptr;
		if (address > UINT64_MAX - count)
		{
			Loader::ReportGuestMemoryFault(address, count, false);
			return nullptr;
		}
		address += count;
	}
}
#endif

} // namespace

static LUMEN_SYSV_ABI void* hle_memcpy(void* dst, const void* src, size_t size)
{
	GuestRange read, write;
	if (size == 0 || !read.Acquire(src, size, false) || !write.Acquire(dst, size, true)) return dst;
	return std::memcpy(dst, src, size);
}

static LUMEN_SYSV_ABI void* hle_memmove(void* dst, const void* src, size_t size)
{
	GuestRange read, write;
	if (size == 0 || !read.Acquire(src, size, false) || !write.Acquire(dst, size, true)) return dst;
	return std::memmove(dst, src, size);
}

static LUMEN_SYSV_ABI int hle_memcmp(const void* lhs, const void* rhs, size_t size)
{
	GuestRange left, right;
	if (size == 0 || !left.Acquire(lhs, size, false) || !right.Acquire(rhs, size, false)) return 0;
	return std::memcmp(lhs, rhs, size);
}

static LUMEN_SYSV_ABI void* hle_memset(void* dst, int value, size_t size)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	static const bool trace_owned_stack = [] {
		const char* value = std::getenv("LUMEN_FEX_MEMSET_OWNED_STACK_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	if (trace_owned_stack)
		LumenMacOSFexCoreObserveMemsetOwnedStackCoverageV1(reinterpret_cast<uint64_t>(dst), size);
#endif
	GuestRange write;
	if (size == 0 || !write.Acquire(dst, size, true)) return dst;
	return std::memset(dst, value, size);
}

static LUMEN_SYSV_ABI void* hle_memchr(const void* data, int value, size_t size)
{
	GuestRange read;
	if (size == 0 || !read.Acquire(data, size, false)) return nullptr;
	return const_cast<void*>(std::memchr(data, value, size));
}

static LUMEN_SYSV_ABI size_t hle_strlen(const char* str)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	std::string text;
	return CopyGuestString(str, &text) ? text.size() : 0;
#else
	return std::strlen(str);
#endif
}

static LUMEN_SYSV_ABI int hle_strcmp(const char* lhs, const char* rhs)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	return CompareGuestStrings(lhs, rhs, (std::numeric_limits<size_t>::max)());
#else
	return std::strcmp(lhs, rhs);
#endif
}

static LUMEN_SYSV_ABI int hle_strncmp(const char* lhs, const char* rhs, size_t size)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	return CompareGuestStrings(lhs, rhs, size);
#else
	return std::strncmp(lhs, rhs, size);
#endif
}

static LUMEN_SYSV_ABI char* hle_strcpy(char* dst, const char* src)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	std::string text;
	GuestRange write;
	if (!CopyGuestString(src, &text) || !write.Acquire(dst, text.size() + 1, true)) return dst;
	std::memcpy(dst, text.c_str(), text.size() + 1);
	return dst;
#else
	return std::strcpy(dst, src);
#endif
}

static LUMEN_SYSV_ABI char* hle_strncpy(char* dst, const char* src, size_t size)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	std::string text;
	GuestRange write;
	if (size == 0 || !CopyGuestString(src, &text, size) || !write.Acquire(dst, size, true)) return dst;
	std::memcpy(dst, text.data(), text.size());
	std::memset(dst + text.size(), 0, size - text.size());
	return dst;
#else
	return std::strncpy(dst, src, size);
#endif
}

static LUMEN_SYSV_ABI char* hle_strcat(char* dst, const char* src)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	std::string destination, source;
	if (!CopyGuestString(dst, &destination) || !CopyGuestString(src, &source)) return dst;
	const auto address = reinterpret_cast<uint64_t>(dst);
	if (address > UINT64_MAX - destination.size())
	{
		Loader::ReportGuestMemoryFault(address, destination.size(), true);
		return dst;
	}
	auto* append = reinterpret_cast<char*>(address + destination.size());
	GuestRange write;
	if (!write.Acquire(append, source.size() + 1, true)) return dst;
	std::memcpy(append, source.c_str(), source.size() + 1);
	return dst;
#else
	return std::strcat(dst, src);
#endif
}

static LUMEN_SYSV_ABI char* hle_strchr(const char* str, int value)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	return FindGuestCharacter(str, value);
#else
	return const_cast<char*>(std::strchr(str, value));
#endif
}

static LUMEN_SYSV_ABI char* hle_strrchr(const char* str, int value)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	std::string text;
	if (!CopyGuestString(str, &text)) return nullptr;
	return OriginalStringResult(str, text, std::strrchr(text.c_str(), value));
#else
	return const_cast<char*>(std::strrchr(str, value));
#endif
}

static LUMEN_SYSV_ABI char* hle_strstr(const char* str, const char* needle)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	std::string text, search;
	if (!CopyGuestString(str, &text) || !CopyGuestString(needle, &search)) return nullptr;
	return OriginalStringResult(str, text, std::strstr(text.c_str(), search.c_str()));
#else
	return const_cast<char*>(std::strstr(str, needle));
#endif
}

static LUMEN_SYSV_ABI size_t hle_strspn(const char* str, const char* accept)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	std::string text, set;
	if (!CopyGuestString(str, &text) || !CopyGuestString(accept, &set)) return 0;
	return std::strspn(text.c_str(), set.c_str());
#else
	return std::strspn(str, accept);
#endif
}

static LUMEN_SYSV_ABI size_t hle_strcspn(const char* str, const char* reject)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	std::string text, set;
	if (!CopyGuestString(str, &text) || !CopyGuestString(reject, &set)) return 0;
	return std::strcspn(text.c_str(), set.c_str());
#else
	return std::strcspn(str, reject);
#endif
}

static LUMEN_SYSV_ABI char* hle_strpbrk(const char* str, const char* accept)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	std::string text, set;
	if (!CopyGuestString(str, &text) || !CopyGuestString(accept, &set)) return nullptr;
	return OriginalStringResult(str, text, std::strpbrk(text.c_str(), set.c_str()));
#else
	return const_cast<char*>(std::strpbrk(str, accept));
#endif
}

static LUMEN_SYSV_ABI void catchReturnFromMain(int status)
{
	PRINT_NAME();

	::printf("return from main = %d\n", status);
}

static LUMEN_SYSV_ABI int cxa_atexit(void (*func)(void*), void* arg, void* d)
{
	PRINT_NAME();

	auto* cc = Core::Singleton<CContext>::Instance();

	CxaDestructor c {};
	c.destructor_func   = func;
	c.destructor_object = arg;
	c.module_id         = d;

	cc->cxa.Add(c);

	return 0;
}

void LUMEN_SYSV_ABI cxa_finalize(void* d)
{
	PRINT_NAME();

	auto* cc = Core::Singleton<CContext>::Instance();

	FOR_LIST_R(i, cc->cxa)
	{
		auto& c = cc->cxa[i];
		if (c.module_id == d && c.destructor_func != nullptr)
		{
#if defined(LUMEN_EMU_HOST_ARM64)
			if (Loader::IsNativeHostCallTarget(reinterpret_cast<uint64_t>(c.destructor_func)))
			{
				c.destructor_func(c.destructor_object);
			} else
			{
				Loader::RunGuestX64(reinterpret_cast<uint64_t>(c.destructor_func), reinterpret_cast<uint64_t>(c.destructor_object));
			}
#else
			c.destructor_func(c.destructor_object);
#endif
			c.destructor_func = nullptr;
		}
	}
}

} // namespace LibC

namespace LibcInternalExt {

LIB_VERSION("LibcInternalExt", 1, "LibcInternal", 1, 1);

struct HeapTraceState
{
	uint64_t mspace_atomic_id_mask = 0;
	uint64_t mstate_table[64] {};
};
static_assert(sizeof(HeapTraceState) == 65 * sizeof(uint64_t));
static HeapTraceState* g_heap_trace_state = nullptr;
static std::once_flag g_heap_trace_state_once;

static void ClearHeapTraceState()
{
	if (g_heap_trace_state == nullptr) return;
#if defined(LUMEN_EMU_HOST_ARM64)
	Core::VirtualMemory::GuestDataLease lease;
	EXIT_IF(!Core::VirtualMemory::TryAcquireGuestDataLease(
	    reinterpret_cast<uint64_t>(g_heap_trace_state), sizeof(HeapTraceState),
	    Core::VirtualMemory::Mode::Write, &lease));
#endif
	const HeapTraceState initial {};
	std::memcpy(g_heap_trace_state, &initial, sizeof(initial));
}

static void PrepareHeapTraceState()
{
	// Publish this persistent writable ABI backing before any HLE root can
	// call the getter. A getter under a reader lease cannot add VM ownership.
	std::call_once(g_heap_trace_state_once, [] {
		constexpr uint64_t page_size = 16 * 1024;
		const auto address = Core::VirtualMemory::Alloc(0, page_size, Core::VirtualMemory::Mode::ReadWrite);
		EXIT_IF(address == 0);
		g_heap_trace_state = reinterpret_cast<HeapTraceState*>(address);
		ClearHeapTraceState();
	});
}

void ResetSessionState()
{
	// The session owner has drained guest roots. Clear the old heap pointers
	// while retaining stable backing and pointer identity for the next session.
	ClearHeapTraceState();
}

struct Info
{
	uint64_t  size;
	uint32_t  unknown1;
	uint32_t  unknown2;
	uint64_t* mspace_atomic_id_mask;
	uint64_t* mstate_table;
};
static_assert(sizeof(Info) == 32);

void LUMEN_SYSV_ABI LibcHeapGetTraceInfo(Info* info)
{
	PRINT_NAME();

#if defined(LUMEN_EMU_HOST_ARM64)
	Core::VirtualMemory::GuestDataLease lease;
	const auto address = reinterpret_cast<uint64_t>(info);
	if (!Core::VirtualMemory::TryAcquireGuestDataLease(address, sizeof(Info), Core::VirtualMemory::Mode::ReadWrite, &lease))
	{
#if (defined(LUMEN_FEXCORE_RUNTIME) || defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)) && defined(__APPLE__)
		Loader::ReportGuestMemoryFault(address, sizeof(Info), true);
		return;
#else
		EXIT("LibcHeapGetTraceInfo guest output lease denied");
#endif
	}
#endif
	EXIT_NOT_IMPLEMENTED(info == nullptr || info->size != sizeof(Info));
	EXIT_IF(g_heap_trace_state == nullptr);

	info->mspace_atomic_id_mask = &g_heap_trace_state->mspace_atomic_id_mask;
	info->mstate_table          = g_heap_trace_state->mstate_table;
}

LIB_DEFINE(InitLibcInternalExt_1)
{
	LibcInternalExt::PrepareHeapTraceState();
	LIB_FUNC_SCALAR("NWtTN10cJzE", LibcInternalExt::LibcHeapGetTraceInfo);
}

} // namespace LibcInternalExt

namespace LibcInternal {

LIB_VERSION("LibcInternal", 1, "LibcInternal", 1, 1);

static uint32_t* g_need_flag = nullptr;
static std::once_flag g_need_flag_once;

void ResetSessionState()
{
	ResetNeedFlag(g_need_flag);
}

int LUMEN_SYSV_ABI vprintf(const char* str, VaList* c)
{
	PRINT_NAME();

	return GetVprintfFunc()(str, c);
}

static LUMEN_SYSV_ABI int snprintf(VA_ARGS)
{
	VA_CONTEXT(ctx); // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

	PRINT_NAME();

	return GetSnrintfCtxFunc()(&ctx);
}

int LUMEN_SYSV_ABI fflush(FILE* stream)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(stream != stdout);

	return ::fflush(stream);
}

void* LUMEN_SYSV_ABI memset(void* s, int c, size_t n)
{
	PRINT_NAME();

	return LibC::hle_memset(s, c, n);
}

void* LUMEN_SYSV_ABI LibcMspaceCreate(const char* name, void* base, size_t capacity, uint32_t flag)
{
	PRINT_NAME();

	printf("\t name     = %s\n", name);
	printf("\t base     = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(base));
	printf("\t capacity = %016" PRIx64 "\n", capacity);
	printf("\t flag     = %u\n", flag);

	EXIT_NOT_IMPLEMENTED(flag != 0 && flag != 1);
	EXIT_NOT_IMPLEMENTED(name == nullptr);
	EXIT_NOT_IMPLEMENTED(base == nullptr);
	EXIT_NOT_IMPLEMENTED(capacity == 0);

	bool thread_safe = true;

	if (flag == 1)
	{
		thread_safe = false;
	}

	auto* msp = Core::MSpaceCreate(name, base, capacity, thread_safe, nullptr);

	EXIT_NOT_IMPLEMENTED(msp == nullptr);

	return msp;
}

void* LUMEN_SYSV_ABI LibcMspaceMalloc(void* msp, size_t size)
{
	PRINT_NAME();

	printf("\t size = %016" PRIx64 "\n", size);

	auto* buf = Core::MSpaceMalloc(msp, size);

	printf("\t buf  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(buf));

	EXIT_NOT_IMPLEMENTED(buf == nullptr);

	return buf;
}

LIB_DEFINE(InitLibcInternal_1)
{
	PrepareNeedFlag(LibcInternal::g_need_flag, LibcInternal::g_need_flag_once);
	LibcInternalExt::InitLibcInternalExt_1(s);

	LIB_OBJECT("ZT4ODD2Ts9o", LibcInternal::g_need_flag);
	LIB_OBJECT("2sWzhYqFH4E", stdout);

	LIB_FUNC("GMpvxPFW924", LibcInternal::vprintf);
	LIB_FUNC("MUjC4lbHrK4", LibcInternal::fflush);
	LIB_FUNC("8zTFvBIAIN8", LibcInternal::memset);
	LIB_FUNC("eLdDw6l0-bU", LibcInternal::snprintf);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	EXIT_IF(!s->SetHostCallVariadicKind(reinterpret_cast<uint64_t>(LibcInternal::snprintf), Loader::HostCallVariadicKind::Snprintf));
#endif

	// These standard NIDs belong to both library namespaces. Register them
	// explicitly so an internal-library import retains exact symbol matching.
	LIB_FUNC("Q3VBxCXhUHs", LibC::hle_memcpy);
	LIB_FUNC("+P6FRGH4LfA", LibC::hle_memmove);
	LIB_FUNC("DfivPArhucg", LibC::hle_memcmp);
	LIB_FUNC("8u8lPzUEq+U", LibC::hle_memchr);
	LIB_FUNC("j4ViWNHEgww", LibC::hle_strlen);
	LIB_FUNC("Ovb2dSJOAuE", LibC::hle_strcmp);
	LIB_FUNC("aesyjrHVWy4", LibC::hle_strncmp);
	LIB_FUNC("kiZSXIWd9vg", LibC::hle_strcpy);
	LIB_FUNC("6sJWiWSRuqk", LibC::hle_strncpy);
	LIB_FUNC("Ls4tzzhimqQ", LibC::hle_strcat);
	LIB_FUNC("ob5xAW4ln-0", LibC::hle_strchr);
	LIB_FUNC("9yDWMxEFdJU", LibC::hle_strrchr);
	LIB_FUNC("viiwFMaNamA", LibC::hle_strstr);
	LIB_FUNC("-kU6bB4M-+k", LibC::hle_strspn);
	LIB_FUNC("q0F6yS-rCms", LibC::hle_strcspn);
	LIB_FUNC("kDZvoVssCgQ", LibC::hle_strpbrk);

	LIB_FUNC("tsvEmnenz48", LibC::cxa_atexit);
	LIB_FUNC("H2e8t5ScQGc", LibC::cxa_finalize);

	LIB_FUNC("-hn1tcVHq5Q", LibcInternal::LibcMspaceCreate);
	LIB_FUNC("OJjm-QOIHlI", LibcInternal::LibcMspaceMalloc);
}

} // namespace LibcInternal

LIB_USING(LibC);

LIB_DEFINE(InitLibC_1)
{
	EXIT("deprecated\n");

	PrepareNeedFlag(LibC::g_need_flag, LibC::g_need_flag_once);
	LibcInternal::InitLibcInternal_1(s);

	LIB_OBJECT("P330P3dFF68", LibC::g_need_flag);

	LIB_FUNC("uMei1W9uyNo", LibC::exit);
	LIB_FUNC("bzQExy189ZI", LibC::init_env);
	LIB_FUNC("8G2LB+A3rzg", LibC::atexit);
	LIB_FUNC("hcuQgD53UxM", LibC::libc_printf);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	EXIT_IF(!s->SetHostCallVariadicKind(reinterpret_cast<uint64_t>(LibC::libc_printf), Loader::HostCallVariadicKind::Printf));
#endif
	LIB_FUNC("YQ0navp+YIc", LibC::puts);
	LIB_FUNC("XKRegsFpEpk", LibC::catchReturnFromMain);
	LIB_FUNC("tsvEmnenz48", LibC::cxa_atexit);
	LIB_FUNC("H2e8t5ScQGc", LibC::cxa_finalize);
}

	#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || \
	     LUMEN_PLATFORM == LUMEN_PLATFORM_IOS) && defined(__aarch64__)
LIB_DEFINE(InitLibCArm64Hle_1)
{
	LIB_FUNC("Q3VBxCXhUHs", LibC::hle_memcpy);
	LIB_FUNC("+P6FRGH4LfA", LibC::hle_memmove);
	LIB_FUNC("DfivPArhucg", LibC::hle_memcmp);
	LIB_FUNC("8zTFvBIAIN8", LibC::hle_memset);
	LIB_FUNC("8u8lPzUEq+U", LibC::hle_memchr);
	LIB_FUNC("j4ViWNHEgww", LibC::hle_strlen);
	LIB_FUNC("Ovb2dSJOAuE", LibC::hle_strcmp);
	LIB_FUNC("aesyjrHVWy4", LibC::hle_strncmp);
	LIB_FUNC("kiZSXIWd9vg", LibC::hle_strcpy);
	LIB_FUNC("6sJWiWSRuqk", LibC::hle_strncpy);
	LIB_FUNC("Ls4tzzhimqQ", LibC::hle_strcat);
	LIB_FUNC("ob5xAW4ln-0", LibC::hle_strchr);
	LIB_FUNC("9yDWMxEFdJU", LibC::hle_strrchr);
	LIB_FUNC("viiwFMaNamA", LibC::hle_strstr);
	LIB_FUNC("-kU6bB4M-+k", LibC::hle_strspn);
	LIB_FUNC("q0F6yS-rCms", LibC::hle_strcspn);
	LIB_FUNC("kDZvoVssCgQ", LibC::hle_strpbrk);
}
#endif

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
