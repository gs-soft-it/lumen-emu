#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/Singleton.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/VirtualMemory.h"
#include "Lumen/Math/Rand.h"

#include "Emulator/Common.h"
#include "Emulator/Config.h"
#include "Emulator/Kernel/EventFlag.h"
#include "Emulator/Kernel/EventQueue.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Kernel/Semaphore.h"
#include "Emulator/Kernel/Umtx.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/SymbolDatabase.h"

#if defined(LUMEN_FEXCORE_RUNTIME)
#include "AndroidFexCoreRuntime.h"
#include "GuestCpuRuntime.h"
#endif

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
#include <process.h>
#else
#include <unistd.h>
#endif

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

using HleEffect = Lumen::GuestCpu::HostCallEffect;

LIB_VERSION("libkernel", 1, "libkernel", 1, 1);

namespace LibKernel {

using KernelModule                   = int32_t;
using get_thread_atexit_count_func_t = LUMEN_SYSV_ABI int (*)(KernelModule);
using thread_atexit_report_func_t    = LUMEN_SYSV_ABI void (*)(KernelModule);

#pragma pack(1)

struct KernelLoadModuleOpt
{
	size_t size;
};

struct KernelUnloadModuleOpt
{
	size_t size;
};

struct TlsInfo
{
	Loader::Program* program;
	uint64_t         offset;
};

struct MallocReplace
{
	uint64_t size               = sizeof(MallocReplace);
	void*    malloc_initialize  = nullptr;
	void*    malloc_finalize    = nullptr;
	void*    malloc             = nullptr;
	void*    free               = nullptr;
	void*    calloc             = nullptr;
	void*    realloc            = nullptr;
	void*    memalign           = nullptr;
	void*    reallocalign       = nullptr;
	void*    posix_memalign     = nullptr;
	void*    malloc_stats       = nullptr;
	void*    malloc_stats_fast  = nullptr;
	void*    malloc_usable_size = nullptr;
	void*    aligned_alloc      = nullptr;
};

struct NewReplace
{
	uint64_t size                           = sizeof(NewReplace);
	void*    new_p                          = nullptr;
	void*    new_nothrow                    = nullptr;
	void*    new_array                      = nullptr;
	void*    new_array_nothrow              = nullptr;
	void*    delete_p                       = nullptr;
	void*    delete_nothrow                 = nullptr;
	void*    delete_array                   = nullptr;
	void*    delete_array_nothrow           = nullptr;
	void*    delete_with_size               = nullptr;
	void*    delete_with_size_nothrow       = nullptr;
	void*    delete_array_with_size         = nullptr;
	void*    delete_array_with_size_nothrow = nullptr;
};

static_assert(sizeof(MallocReplace) == 112 && sizeof(NewReplace) == 104);

struct OrbisSigSet
{
	uint32_t bits[4];
};

static_assert(sizeof(OrbisSigSet) == 16);

struct ModuleInfo
{
	uint64_t     size;
	uint64_t     info[32];
	KernelModule handle;
	uint8_t      pad[156];
};

struct ModuleInfoForUnwind
{
	uint64_t st_size;
	char     name[256];
	uint64_t eh_frame_hdr_addr;
	uint64_t eh_frame_addr;
	uint64_t eh_frame_size;
	uint64_t seg0_addr;
	uint64_t seg0_size;
};

static_assert(sizeof(ModuleInfoForUnwind) == 0x130);
static_assert(offsetof(ModuleInfoForUnwind, name) == 0x8);
static_assert(offsetof(ModuleInfoForUnwind, eh_frame_hdr_addr) == 0x108);
static_assert(offsetof(ModuleInfoForUnwind, eh_frame_addr) == 0x110);
static_assert(offsetof(ModuleInfoForUnwind, eh_frame_size) == 0x118);
static_assert(offsetof(ModuleInfoForUnwind, seg0_addr) == 0x120);
static_assert(offsetof(ModuleInfoForUnwind, seg0_size) == 0x128);

#pragma pack()

constexpr size_t PROGNAME_MAX_SIZE = 511;

// Exported ABI objects must be guest mappings, including the pointee of
// __progname. Publishing native image addresses works with direct host loads
// but is not a valid memory contract for a translated guest.
struct ProgramNameObject
{
	const char* name;
	char        text[PROGNAME_MAX_SIZE + 1];
};
static const uint64_t* g_stack_chk_guard = nullptr;
static ProgramNameObject* g_progname = nullptr;
static std::once_flag g_kernel_abi_objects_once;

static void PrepareKernelAbiObjects()
{
	std::call_once(g_kernel_abi_objects_once, [] {
		constexpr uint64_t page_size = 16 * 1024;
		const auto guard = Core::VirtualMemory::Alloc(0, page_size, Core::VirtualMemory::Mode::ReadWrite);
		EXIT_IF(guard == 0);
		*reinterpret_cast<uint64_t*>(guard) = 0xDeadBeef5533CCAA;
		EXIT_IF(!Core::VirtualMemory::Protect(guard, page_size, Core::VirtualMemory::Mode::Read));
		g_stack_chk_guard = reinterpret_cast<const uint64_t*>(guard);
		const auto name = Core::VirtualMemory::Alloc(0, page_size, Core::VirtualMemory::Mode::ReadWrite);
		EXIT_IF(name == 0);
		g_progname = reinterpret_cast<ProgramNameObject*>(name);
		std::memset(g_progname, 0, sizeof(*g_progname));
		g_progname->name = g_progname->text;
	});
}

static get_thread_atexit_count_func_t g_get_thread_atexit_count_func = nullptr;
static thread_atexit_report_func_t    g_thread_atexit_report_func    = nullptr;
// The guest owns these writable ABI objects. Allocate their managed backing
// before publishing the getter imports: a getter under an HLE reader must not
// mutate VM ownership or expose unrelated native image bytes.
struct SanitizerReplacementObjects
{
	MallocReplace malloc_replace {};
	NewReplace    new_replace {};
};
static SanitizerReplacementObjects* g_sanitizer_replacements = nullptr;
static std::once_flag g_sanitizer_replacements_once;

static void PrepareSanitizerReplacementObjects()
{
	std::call_once(g_sanitizer_replacements_once, [] {
		constexpr uint64_t page_size = 16 * 1024;
		const auto address = Core::VirtualMemory::Alloc(0, page_size, Core::VirtualMemory::Mode::ReadWrite);
		EXIT_IF(address == 0);
		g_sanitizer_replacements = reinterpret_cast<SanitizerReplacementObjects*>(address);
		const SanitizerReplacementObjects initial {};
		std::memcpy(g_sanitizer_replacements, &initial, sizeof(initial));
	});
}

static void ResetExceptionHandlers();

void ResetSessionState()
{
	g_get_thread_atexit_count_func = nullptr;
	g_thread_atexit_report_func    = nullptr;
	ResetExceptionHandlers();
	// Session shutdown has already drained guest roots. Keep pointer identity,
	// while clearing callbacks that may refer to the previous guest image.
	if (g_sanitizer_replacements != nullptr)
	{
		const SanitizerReplacementObjects initial {};
		std::memcpy(g_sanitizer_replacements, &initial, sizeof(initial));
	}
	if (g_progname != nullptr)
	{
#if defined(LUMEN_EMU_HOST_ARM64)
		Core::VirtualMemory::GuestDataLease lease;
		EXIT_IF(!Core::VirtualMemory::TryAcquireGuestDataLease(
		    reinterpret_cast<uint64_t>(g_progname), sizeof(*g_progname),
		    Core::VirtualMemory::Mode::Write, &lease));
#endif
		std::memset(g_progname->text, 0, sizeof(g_progname->text));
		g_progname->name = g_progname->text;
	}
}

static int InvokeThreadAtexitCount(get_thread_atexit_count_func_t callback, KernelModule module)
{
#if defined(LUMEN_EMU_HOST_ARM64)
	if (Loader::IsNativeHostCallTarget(reinterpret_cast<uint64_t>(callback))) return callback(module);
	return static_cast<int>(Loader::RunGuestX64(reinterpret_cast<uint64_t>(callback),
	                                            static_cast<uint64_t>(static_cast<int64_t>(module))));
#else
	return callback(module);
#endif
}

static void InvokeThreadAtexitReport(thread_atexit_report_func_t callback, KernelModule module)
{
#if defined(LUMEN_EMU_HOST_ARM64)
	if (Loader::IsNativeHostCallTarget(reinterpret_cast<uint64_t>(callback)))
	{
		callback(module);
	} else
	{
		Loader::RunGuestX64(reinterpret_cast<uint64_t>(callback), static_cast<uint64_t>(static_cast<int64_t>(module)));
	}
#else
	callback(module);
#endif
}

void SetProgName(const String& name)
{
	PrepareKernelAbiObjects();
#if defined(LUMEN_EMU_HOST_ARM64)
	Core::VirtualMemory::GuestDataLease lease;
	EXIT_IF(!Core::VirtualMemory::TryAcquireGuestDataLease(
	    reinterpret_cast<uint64_t>(g_progname), sizeof(*g_progname),
	    Core::VirtualMemory::Mode::Write, &lease));
#endif
	strncpy(g_progname->text, name.C_Str(), PROGNAME_MAX_SIZE);
	g_progname->text[PROGNAME_MAX_SIZE] = '\0';
}

static LUMEN_SYSV_ABI int* get_error_addr()
{
	PRINT_NAME();

	return Posix::GetErrorAddr();
}

static LUMEN_SYSV_ABI void stack_chk_fail()
{
	PRINT_NAME();

	EXIT("stack fail!!!");
}

static LUMEN_SYSV_ABI int rtld_sigprocmask(int /*how*/,
	                                       const OrbisSigSet* /*set*/,
	                                       OrbisSigSet* /*oset*/)
{
	return 0;
}

static LUMEN_SYSV_ABI int sigprocmask(int /*how*/, const OrbisSigSet* /*set*/,
	                                  OrbisSigSet* /*oset*/)
{
	// Guest signal delivery is not modeled yet. Keep this fail-safe no-op until
	// the HLE bridge can validate guest read/write ranges for set and oset.
	return 0;
}

static LUMEN_SYSV_ABI int sigfillset(OrbisSigSet* set)
{
	// Orbis inherits FreeBSD's 128-signal ABI: sigset_t is four 32-bit words.
	// Keep a null guest pointer from turning this HLE call into a host crash.
	if (set == nullptr)
	{
		*Posix::GetErrorAddr() = Posix::POSIX_EFAULT;
		return -1;
	}

	OrbisSigSet filled;
	std::memset(&filled, 0xff, sizeof(filled));
#if defined(LUMEN_FEXCORE_RUNTIME)
	if (!Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(set), &filled, sizeof(filled)))
	{
		*Posix::GetErrorAddr() = Posix::POSIX_EFAULT;
		return -1;
	}
#else
	std::memcpy(set, &filled, sizeof(filled));
#endif
	return 0;
}

static LUMEN_SYSV_ABI int is_signal_return(uint64_t program_counter)
{
	// Guest signal delivery is not modeled by this runtime yet, so no guest PC
	// can currently denote a signal-return trampoline.  Returning zero makes
	// libc's unwinder treat the frame as an ordinary DWARF frame.  Keep this as
	// an explicit classifier instead of a generic unresolved-symbol stub so the
	// ABI and the eventual signal-frame implementation remain well defined.
	(void)program_counter;
	return 0;
}

static LUMEN_SYSV_ABI int posix_getpagesize()
{
	return 16 * 1024;
}

static LUMEN_SYSV_ABI int32_t posix_getpid()
{
	// Lumen hosts one guest process per emulator process.  Exposing that
	// one-to-one identity gives the guest a positive, run-unique POSIX pid
	// without adding title-specific process state.
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	return static_cast<int32_t>(_getpid());
#else
	return static_cast<int32_t>(::getpid());
#endif
}

static LUMEN_SYSV_ABI void* posix_mmap(void* addr, size_t len, int prot,
	                                    int flags, int fd, int64_t offset)
{
	constexpr uintptr_t ORBIS_PAGE_SIZE   = 16 * 1024;
	constexpr uintptr_t DEFAULT_MMAP_HINT = UINT64_C(0x200000000);
	constexpr int       MAP_SHARED_ORBIS  = 0x0001;
	constexpr int       MAP_PRIVATE_ORBIS = 0x0002;
	constexpr int       MAP_ANON_ORBIS    = 0x1000;
	constexpr int       SUPPORTED_FLAGS   = MAP_PRIVATE_ORBIS | MAP_ANON_ORBIS;
	auto fail = [](int error)
	{
		*Posix::GetErrorAddr() = error;
		return reinterpret_cast<void*>(
		    std::numeric_limits<uintptr_t>::max());
	};
	const bool trace = Config::DiagnosticsEnabled();
	if (trace)
	{
		std::fprintf(stderr,
		             "POSIX mmap request: addr=0x%016" PRIxPTR
		             " len=0x%zx prot=0x%x flags=0x%x fd=%d offset=0x%016" PRIx64 "\n",
		             reinterpret_cast<uintptr_t>(addr), len, prot, flags, fd,
		             static_cast<uint64_t>(offset));
	}

	// Anonymous mappings remain private and fixed-hint only: MAP_FIXED needs
	// collision-safe guest address handling. File-backed mappings are served
	// as a private copy in guest flexible memory, so a guest descriptor never
	// reaches the host mapping API; a writable MAP_SHARED request would need
	// host writeback and is refused instead of being served as a private copy.
	const bool anonymous = fd == -1;
	if (len == 0 || len > std::numeric_limits<size_t>::max() -
	                           (ORBIS_PAGE_SIZE - 1) ||
	    prot < 0 || (prot & ~0x7) != 0)
	{
		if (trace) std::fprintf(stderr, "POSIX mmap rejected: unsupported arguments\n");
		return fail(Posix::POSIX_EINVAL);
	}
	if (anonymous)
	{
		if (flags != SUPPORTED_FLAGS || offset != 0)
		{
			if (trace) std::fprintf(stderr, "POSIX mmap rejected: unsupported anonymous arguments\n");
			return fail(Posix::POSIX_EINVAL);
		}
	} else
	{
		const bool shared = (flags & MAP_SHARED_ORBIS) != 0;
		if ((flags & ~(MAP_SHARED_ORBIS | MAP_PRIVATE_ORBIS)) != 0 ||
		    (shared && (prot & 0x2) != 0) || offset < 0 ||
		    (static_cast<uint64_t>(offset) & (ORBIS_PAGE_SIZE - 1)) != 0)
		{
			if (trace) std::fprintf(stderr, "POSIX mmap rejected: unsupported file arguments\n");
			return fail(Posix::POSIX_EINVAL);
		}
	}

	const uintptr_t raw_hint = reinterpret_cast<uintptr_t>(addr);
	if (raw_hint > std::numeric_limits<uintptr_t>::max() -
	                   (ORBIS_PAGE_SIZE - 1))
	{
		if (trace) std::fprintf(stderr, "POSIX mmap rejected: hint overflow\n");
		return fail(Posix::POSIX_EINVAL);
	}
	const uintptr_t aligned_hint = raw_hint == 0
	                                   ? DEFAULT_MMAP_HINT
	                                   : (raw_hint + ORBIS_PAGE_SIZE - 1) &
	                                         ~(ORBIS_PAGE_SIZE - 1);
	const size_t aligned_len =
	    (len + ORBIS_PAGE_SIZE - 1) & ~(ORBIS_PAGE_SIZE - 1);
	// File-backed contents are installed while the range is writable and the
	// requested protection is applied afterwards, so the guest never observes
	// the staging protection state.
	const int mapping_prot = anonymous ? prot : (prot | 0x2);
	void* mapped = reinterpret_cast<void*>(aligned_hint);
	const int result =
	    Memory::KernelMapFlexibleMemory(&mapped, aligned_len, mapping_prot, 0);
	if (result != OK)
	{
		if (trace)
		{
			std::fprintf(stderr,
			             "POSIX mmap failed: kernel=%d hint=0x%016" PRIxPTR
			             " len=0x%zx\n",
			             result, aligned_hint, aligned_len);
		}
		return fail(LibKernel::KernelToPosix(result));
	}

	const uintptr_t mapped_address = reinterpret_cast<uintptr_t>(mapped);
	if (mapped_address == 0 ||
	    (mapped_address & (ORBIS_PAGE_SIZE - 1)) != 0)
	{
		if (trace)
		{
			std::fprintf(stderr,
			             "POSIX mmap failed: unaligned result=0x%016" PRIxPTR
			             " hint=0x%016" PRIxPTR " len=0x%zx\n",
			             mapped_address, aligned_hint, aligned_len);
		}
		if (mapped_address != 0)
		{
			(void)Memory::KernelMunmap(mapped_address, aligned_len);
		}
		return fail(Posix::POSIX_ENOMEM);
	}
	if (!anonymous)
	{
		uint64_t   filled      = 0;
		const int read_result = FileSystem::KernelReadMappedRange(
		    fd, reinterpret_cast<void*>(mapped_address), aligned_len, offset, &filled);
		if (read_result != OK)
		{
			if (trace)
			{
				std::fprintf(stderr,
				             "POSIX mmap file read failed: kernel=%d fd=%d offset=0x%016" PRIx64
				             " len=0x%zx\n",
				             read_result, fd, static_cast<uint64_t>(offset), aligned_len);
			}
			(void)Memory::KernelMunmap(mapped_address, aligned_len);
			return fail(LibKernel::KernelToPosix(read_result));
		}
		if (prot != mapping_prot)
		{
			const int protect_result = Memory::KernelApplyPosixProtection(mapped_address, aligned_len, prot);
			if (protect_result != OK)
			{
				if (trace)
				{
					std::fprintf(stderr, "POSIX mmap reprotect failed: kernel=%d prot=0x%x\n", protect_result, prot);
				}
				(void)Memory::KernelMunmap(mapped_address, aligned_len);
				return fail(LibKernel::KernelToPosix(protect_result));
			}
		}
		if (trace)
		{
			std::fprintf(stderr,
			             "POSIX mmap file-backed success: result=0x%016" PRIxPTR
			             " len=0x%zx filled=0x%zx fd=%d\n",
			             mapped_address, aligned_len, static_cast<size_t>(filled), fd);
		}
	}
	if (trace)
	{
		std::fprintf(stderr,
		             "POSIX mmap success: result=0x%016" PRIxPTR
		             " hint=0x%016" PRIxPTR " len=0x%zx\n",
		             mapped_address, aligned_hint, aligned_len);
	}
	return mapped;
}

static LUMEN_SYSV_ABI int posix_msync(void* addr, size_t len, int flags)
{
	(void)flags;
	const uint64_t address = reinterpret_cast<uint64_t>(addr);
	if (address == 0 || len == 0 || address > UINT64_MAX - len)
	{
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}
#if defined(__aarch64__)
	if (!Loader::ValidateGuestMemory(address, len, false))
	{
		*Posix::GetErrorAddr() = Posix::POSIX_ENOMEM;
		return -1;
	}
#endif
	// shadPS4 models msync as a successful no-op. Guest mappings are anonymous
	// in this runtime path, so there is no backing file to flush on Android.
	return 0;
}

static LUMEN_SYSV_ABI KernelModule KernelLoadStartModule(const char* module_file_name, size_t args, const void* argp, uint32_t flags,
                                                        const KernelLoadModuleOpt* opt, int* res)
{
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	printf("\tmodule_file_name = %s\n", module_file_name);
	printf("\targs = 0x%016" PRIx64 "\n", static_cast<uint64_t>(args));
	printf("\targp = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(argp));
	printf("\tflags = 0x%08" PRIx32 "\n", flags);
	printf("\topt = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(opt));
	printf("\tres = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(res));

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();

#if defined(__aarch64__)
	const char* const module_args_trace = std::getenv("LUMEN_MODULE_ARGS_TRACE");
	if (module_args_trace != nullptr && module_args_trace[0] == '1' && module_args_trace[1] == '\0')
	{
		// Bounded loader diagnostic: each accepted request records name, scalar
		// arguments and the guest return address of the caller, so a replay can
		// attribute a module load without any per-title information. The guest
		// name and stack are read through the ordinary validated helpers.
		static std::atomic<uint32_t> module_start_trace_count {0};
		if (module_start_trace_count.fetch_add(1, std::memory_order_relaxed) < 64)
		{
			std::array<char, 256> trace_name {};
			const char* name_text = "<unreadable>";
			if (module_file_name != nullptr &&
			    Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(module_file_name), trace_name.data(),
			                            trace_name.size() - 1))
			{
				trace_name.back() = '\0';
				name_text = trace_name.data();
			}
			uint64_t guest_caller = 0;
			if (const uint64_t* gprs = Loader::TryGetActiveHostCallGuestGprs();
			    gprs != nullptr && !Loader::ReadGuestMemory(gprs[4], &guest_caller, sizeof(guest_caller)))
			{
				guest_caller = 0;
			}
			char caller_module[128] = "?";
			if (guest_caller != 0)
			{
				if (const auto* caller_program = rt->FindProgramByAddr(guest_caller); caller_program != nullptr)
				{
					const String module_name = caller_program->file_name.FilenameWithoutDirectory();
					const size_t module_name_size =
					    std::min(static_cast<size_t>(module_name.Size()), sizeof(caller_module) - 1);
					std::memcpy(caller_module, module_name.C_Str(), module_name_size);
					caller_module[module_name_size] = '\0';
				}
			}
			std::fprintf(stderr,
			             "[RUNTIME_MODULE_START_TRACE] thread=%u name=\"%s\" args=0x%016" PRIx64
			             " argp=0x%016" PRIx64 " flags=0x%08" PRIx32 " caller=0x%016" PRIx64 " callerModule=%s\n",
			             static_cast<unsigned>(Core::Thread::GetThreadIdUnique()), name_text,
			             static_cast<uint64_t>(args), reinterpret_cast<uint64_t>(argp), flags, guest_caller,
			             caller_module);
		}
	}
	if (module_args_trace != nullptr && module_args_trace[0] == '1' &&
	    module_args_trace[1] == '\0' && args != 0 && argp != nullptr)
	{
		constexpr size_t max_trace_size = 256;
		const size_t trace_size = std::min(args, max_trace_size);
		std::array<uint8_t, max_trace_size> trace {};
		if (Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(argp), trace.data(), trace_size))
		{
			printf("\targp_bytes[%zu] =", trace_size);
			for (size_t i = 0; i < trace_size; ++i) printf(" %02x", trace[i]);
			printf("\n");
		} else
		{
			printf("\targp_bytes[%zu] = <unreadable>\n", trace_size);
		}
	}
#endif

	EXIT_NOT_IMPLEMENTED(flags != 0);
	EXIT_NOT_IMPLEMENTED(opt != nullptr);

	// The console loader keeps one live instance per module file, like dlopen:
	// opening a module that is already live returns that same instance, and its
	// initializer is not run a second time. A repeated request therefore only
	// takes one more load reference instead of mapping a second copy.
	const String module_name = FileSystem::GetRealFilename(String::FromUtf8(module_file_name));
	auto         admission   = rt->AcquireModuleLoadReference(module_name, true);

	auto* program = admission.program;
	auto  handle  = program->unique_id;

	if (admission.already_loaded)
	{
#if defined(__aarch64__)
		const char* const module_args_trace = std::getenv("LUMEN_MODULE_ARGS_TRACE");
		if (module_args_trace != nullptr && module_args_trace[0] == '1' && module_args_trace[1] == '\0')
		{
			std::fprintf(stderr, "[RUNTIME_MODULE_REUSE] thread=%u handle=%d name=\"%s\"\n",
			             static_cast<unsigned>(Core::Thread::GetThreadIdUnique()), static_cast<int>(handle),
			             module_name.C_Str());
		}
#endif
		if (res != nullptr)
		{
			*res = 0;
		}
		return static_cast<KernelModule>(handle);
	}

	// A module loaded through the guest loader can import optional functions
	// that are never called. Preserve only unresolved PLT functions so the
	// runtime traps at the actual call site; data and non-PLT relocations keep
	// the ordinary strict policy.
	program->dbg_print_reloc = true;

	rt->RelocateAll();

	int result = rt->StartModule(program, args, argp, nullptr);

	printf("\tmodule_start() result = %d\n", result);

	EXIT_NOT_IMPLEMENTED(result < 0);

	if (res != nullptr)
	{
		*res = result;
	}

	return static_cast<KernelModule>(handle);
}

static int LUMEN_SYSV_ABI KernelStopUnloadModule(KernelModule handle, size_t args, const void* argp, uint32_t flags,
                                                const KernelUnloadModuleOpt* opt, int* res)
{
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();

	EXIT_NOT_IMPLEMENTED(flags != 0);
	EXIT_NOT_IMPLEMENTED(opt != nullptr);

	auto* program = rt->FindProgramById(handle);

	EXIT_NOT_IMPLEMENTED(program == nullptr);

	if (g_get_thread_atexit_count_func != nullptr && InvokeThreadAtexitCount(g_get_thread_atexit_count_func, program->unique_id) > 0)
	{
		printf("KernelStopUnloadModule: cannot unload %s\n", program->file_name.C_Str());
		if (g_thread_atexit_report_func != nullptr)
		{
			InvokeThreadAtexitReport(g_thread_atexit_report_func, program->unique_id);
		}
		return KERNEL_ERROR_EBUSY;
	}
	int result = 0;
	// A module opened more than once keeps its live instance until the last
	// guest reference is released; only then may it stop and unload.
	if (!rt->ReleaseModuleLoadReference(program))
	{
		if (res != nullptr)
		{
			*res = 0;
		}
		return OK;
	}

	if (!rt->StopUnloadProgram(program, args, argp, nullptr, &result))
	{
		printf("KernelStopUnloadModule: module is still referenced\n");
		return KERNEL_ERROR_EBUSY;
	}

	printf("\tmodule_stop() result = %d\n", result);

	EXIT_NOT_IMPLEMENTED(result < 0);

	if (res != nullptr)
	{
		*res = result;
	}

	return OK;
}

// Bounded copy of a NUL-terminated guest symbol name. The validated lease path
// is used on aarch64 hosts; the native path maps guest memory directly.
static bool KernelReadGuestSymbolName(const char* guest_symbol, std::string* name)
{
	constexpr size_t limit = 1024;
	name->clear();
#if defined(__aarch64__)
	uint64_t address = reinterpret_cast<uint64_t>(guest_symbol);
	while (name->size() < limit)
	{
		char    buffer[128];
		const size_t page_left = 4096 - static_cast<size_t>(address & 4095);
		size_t      chunk = sizeof(buffer) < page_left ? sizeof(buffer) : page_left;
		if (chunk > limit - name->size()) chunk = limit - name->size();
		if (!Loader::ReadGuestMemory(address, buffer, chunk))
		{
			return false;
		}
		const auto* end = static_cast<const char*>(std::memchr(buffer, 0, chunk));
		name->append(buffer, end != nullptr ? static_cast<size_t>(end - buffer) : chunk);
		if (end != nullptr)
		{
			return true;
		}
		address += chunk;
	}
	return true;
#else
	name->assign(guest_symbol, strnlen(guest_symbol, limit));
	return true;
#endif
}

static int LUMEN_SYSV_ABI KernelDlsym(KernelModule handle, const char* symbol, void** address)
{
	PRINT_NAME();

	if (symbol == nullptr || address == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	std::string name;
	if (!KernelReadGuestSymbolName(symbol, &name))
	{
		return KERNEL_ERROR_EFAULT;
	}
	printf("\tsymbol = %s\n", name.c_str());

	const String nid = Loader::ComputeSymbolNid(name.data(), name.size());

	auto*    rt    = Core::Singleton<Loader::RuntimeLinker>::Instance();
	uint64_t vaddr = 0;
	if (rt == nullptr || !rt->FindProgramExportByNid(handle, nid, &vaddr))
	{
		return KERNEL_ERROR_ESRCH;
	}

#if defined(__aarch64__)
	if (!Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(address), &vaddr, sizeof(vaddr)))
	{
		return KERNEL_ERROR_EFAULT;
	}
#else
	*address = reinterpret_cast<void*>(vaddr);
#endif
	return OK;
}

static void* LUMEN_SYSV_ABI tls_get_addr(TlsInfo* info)
{
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	return Loader::RuntimeLinker::TlsGetAddr(info->program) + info->offset;
}

static void* LUMEN_SYSV_ABI KernelGetProcParam()
{
	PRINT_NAME();

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance(); // NOLINT

	return reinterpret_cast<void*>(rt->GetProcParam());
}

static void LUMEN_SYSV_ABI KernelRtldSetApplicationHeapAPI(void* api[])
{
	PRINT_NAME();
	EXIT_IF(api == nullptr);

	for (int i = 0; i < 10; i++)
	{
		printf("\tapi[%d] = 0x%016" PRIx64 "\n", i, reinterpret_cast<uint64_t>(api[i]));
	}

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();
	rt->SetHeapAPI(api);
}

static int64_t LUMEN_SYSV_ABI write(int d, const char* str, int64_t size)
{
	// PRINT_NAME();

	if (d < 0 || d == 0) return POSIX_N_CALL(static_cast<int64_t>(KERNEL_ERROR_EBADF));
	if (size < 0 || size > (std::numeric_limits<int>::max)())
		return POSIX_N_CALL(static_cast<int64_t>(KERNEL_ERROR_EINVAL));
	if (size == 0) return 0;
	if (str == nullptr) return POSIX_N_CALL(static_cast<int64_t>(KERNEL_ERROR_EFAULT));

	if (d > 2)
	{
		return POSIX_N_CALL(FileSystem::KernelWrite(d, str, size));
	}

#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	Core::VirtualMemory::GuestDataLease input;
	if (!Core::VirtualMemory::TryAcquireGuestDataLease(
	        reinterpret_cast<uint64_t>(str), static_cast<size_t>(size), Core::VirtualMemory::Mode::Read, &input))
		return POSIX_N_CALL(static_cast<int64_t>(KERNEL_ERROR_EFAULT));
#endif
	int size_int = static_cast<int>(size);

	emu_printf(FG_BRIGHT_MAGENTA "%.*s" DEFAULT, size_int, str);

	return size_int;
}

static int LUMEN_SYSV_ABI open(const char* path, int flags, int mode)
{
	return POSIX_N_CALL(FileSystem::KernelOpen(path, flags, mode));
}

static int LUMEN_SYSV_ABI close(int d)
{
	if (d <= 2) return POSIX_CALL(KERNEL_ERROR_EBADF);

	return POSIX_CALL(FileSystem::KernelClose(d));
}

static int64_t LUMEN_SYSV_ABI read(int d, void* buf, uint64_t nbytes)
{
	// PRINT_NAME();

	if (d < 0 || d == 1 || d == 2) return POSIX_N_CALL(static_cast<int64_t>(KERNEL_ERROR_EBADF));
	if (nbytes > (std::numeric_limits<uint32_t>::max)())
		return POSIX_N_CALL(static_cast<int64_t>(KERNEL_ERROR_EINVAL));
	if (nbytes == 0) return 0;
	if (buf == nullptr) return POSIX_N_CALL(static_cast<int64_t>(KERNEL_ERROR_EFAULT));
	if (d > 2) return POSIX_N_CALL(FileSystem::KernelRead(d, buf, static_cast<size_t>(nbytes)));

#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	Core::VirtualMemory::GuestDataLease output;
	if (!Core::VirtualMemory::TryAcquireGuestDataLease(
	        reinterpret_cast<uint64_t>(buf), static_cast<size_t>(nbytes), Core::VirtualMemory::Mode::Write, &output))
		return POSIX_N_CALL(static_cast<int64_t>(KERNEL_ERROR_EFAULT));
#endif
	const size_t read_size = std::fread(buf, 1, static_cast<size_t>(nbytes), stdin);
	if (read_size == 0 && std::ferror(stdin) != 0)
		return POSIX_N_CALL(static_cast<int64_t>(KERNEL_ERROR_EIO));
	return static_cast<int64_t>(read_size);
}

static int LUMEN_SYSV_ABI KernelGetModuleInfoFromAddr(uint64_t addr, int n, ModuleInfo* r)
{
	PRINT_NAME();

	printf("\taddr = %016" PRIx64 "\n", addr);
	printf("\tn = %d\n", n);

	EXIT_NOT_IMPLEMENTED(n != 2);
	EXIT_NOT_IMPLEMENTED(r == nullptr);

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();

	auto* p = rt->FindProgramByAddr(addr);

	if (p == nullptr)
	{
		printf("\thandle: not found\n");
		r->handle = 0;
		return -1;
	}

	r->handle = p->unique_id;

	printf("\thandle: %d\n", r->handle);

	return 0;
}

static int LUMEN_SYSV_ABI KernelGetModuleInfoForUnwind(
	uint64_t addr, int flags, ModuleInfoForUnwind* info)
{
	if (info == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}
	[[maybe_unused]] const uint64_t guest_info = reinterpret_cast<uint64_t>(info);
	ModuleInfoForUnwind result {};
	if (flags >= 3)
	{
		#if defined(__aarch64__)
		if (!Loader::WriteGuestMemory(
		        guest_info, &result, sizeof(result)))
		{
			return KERNEL_ERROR_EFAULT;
		}
		#else
		std::memcpy(info, &result, sizeof(result));
		#endif
		return KERNEL_ERROR_EINVAL;
	}
	uint64_t supplied_size = 0;
	#if defined(__aarch64__)
	if (!Loader::ReadGuestMemory(
	        guest_info, &supplied_size, sizeof(supplied_size)))
	{
		return KERNEL_ERROR_EFAULT;
	}
	#else
	std::memcpy(&supplied_size, info, sizeof(supplied_size));
	#endif
	if (supplied_size < sizeof(result))
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();
	Loader::ProgramUnwindInfo program_info {};
	if (rt == nullptr || !rt->QueryProgramUnwindInfo(addr, &program_info))
	{
		return KERNEL_ERROR_ESRCH;
	}

	std::memcpy(result.name, program_info.name, sizeof(result.name));
	result.eh_frame_hdr_addr = program_info.eh_frame_hdr_addr;
	result.eh_frame_addr     = program_info.eh_frame_addr;
	result.eh_frame_size     = program_info.eh_frame_size;
	result.seg0_addr         = program_info.seg0_addr;
	result.seg0_size         = program_info.seg0_size;
	#if defined(__aarch64__)
	if (!Loader::WriteGuestMemory(guest_info, &result, sizeof(result)))
	{
		return KERNEL_ERROR_EFAULT;
	}
	#else
	std::memcpy(info, &result, sizeof(result));
	#endif
	return OK;
}

static void LUMEN_SYSV_ABI KernelDebugRaiseExceptionOnReleaseMode(int /*c1*/, int /*c2*/)
{
	PRINT_NAME();
}

static void LUMEN_SYSV_ABI KernelDebugRaiseException(int /*c1*/, int /*c2*/)
{
	PRINT_NAME();
}

static bool KernelExceptionRoutingAvailable()
{
#if defined(LUMEN_FEXCORE_RUNTIME)
	LumenAndroidFexCoreRuntimeStatusV1 status {};
	const uint32_t code = LumenAndroidFexCoreRuntimeProbeV1(&status);
	// FEX stops translated guest faults through native signal handlers; QEMU
	// returns checked guest faults and requested interrupts through its C run
	// boundary. Both then use the same registered guest exception delivery.
	const bool checked_stop = status.signalStopInstalled != 0 ||
	    GuestCpu::Selected() == GuestCpu::Provider::QemuTcg;
	return code == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 && checked_stop && status.backendRouteReady != 0;
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
	auto* runtime = Loader::RuntimeLinker::Instance();
	return runtime != nullptr && runtime->GuestExceptionRoutingReady();
#else
	return false;
#endif
}

// PS4 exception interface (libkernel_unity). The console keeps one handler per
// allowed signal and raising an exception is a signal delivery to the target
// thread, whose handler runs with (signum, ucontext). KernelRaiseException
// accepts only SIGUSR1, resolves the guest pthread handle through the live pool
// and queues the request on the target; the target consumes it at its next HLE
// boundary or between blocking host-wait slices (Pthread.cpp), running the
// installed handler as a nested guest call with a ucontext reconstructed from
// the interrupted GP state. Install is one-shot per signal and fail-closed.
constexpr int32_t kExceptionSignalSlots = 32;
// The console raise entry point accepts only the thread-suspend exception.
constexpr int32_t kExceptionRaiseSignal = 30;

static std::array<std::atomic<uint64_t>, kExceptionSignalSlots> g_exception_handlers {};

static bool IsAllowedExceptionSignal(int32_t signum)
{
	// SIGHUP, SIGILL, SIGFPE, SIGBUS, SIGSEGV and SIGUSR1 in the PS4 signal
	// numbering. SIGUSR1 (30) is the thread-suspend exception used by Unity's
	// GC stop-the-world protocol.
	switch (signum)
	{
	case 1:
	case 4:
	case 8:
	case 10:
	case 11:
	case 30: return true;
	default: return false;
	}
}

static void ResetExceptionHandlers()
{
	for (auto& handler: g_exception_handlers)
	{
		handler.store(0, std::memory_order_release);
	}
}

void KernelTraceExceptionEvent(const char* operation, int32_t signum, uint64_t thread, uint64_t handler, int32_t result)
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_KERNEL_EXCEPTION_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	if (!enabled) return;
	static std::atomic<uint32_t> records {0};
	if (records.fetch_add(1, std::memory_order_relaxed) >= 256) return;
	// Numeric snapshot only: the thread and handler values are reported as
	// recorded, and the diagnostic itself never dereferences a guest pointer.
	const auto now_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                                std::chrono::steady_clock::now().time_since_epoch())
	                                                .count());
	if (std::fprintf(stderr,
	                 "[KERNEL_EXCEPTION_TRACE] ns=%" PRIu64 " thread=%d operation=%s signal=%d"
	                 " target=0x%016" PRIx64 " installed=0x%016" PRIx64 " result=0x%08x\n",
	                 now_ns, Core::Thread::GetThreadIdUnique(), operation, signum, thread, handler,
	                 static_cast<uint32_t>(result)) < 0)
	{
		return;
	}
	std::fflush(stderr);
}

uint64_t KernelExceptionHandlerForSignal(int32_t signum)
{
	return signum > 0 && signum < kExceptionSignalSlots
	    ? g_exception_handlers[signum].load(std::memory_order_acquire)
	    : 0;
}

static int32_t LUMEN_SYSV_ABI KernelInstallExceptionHandler(int32_t signum, uint64_t handler)
{
	PRINT_NAME();
	if (!KernelExceptionRoutingAvailable()) return KERNEL_ERROR_ENOSYS;
	if (!IsAllowedExceptionSignal(signum)) return KERNEL_ERROR_EINVAL;
	uint64_t expected = 0;
	if (!g_exception_handlers[signum].compare_exchange_strong(expected, handler))
	{
		KernelTraceExceptionEvent("install-rejected", signum, 0, handler, KERNEL_ERROR_EAGAIN);
		return KERNEL_ERROR_EAGAIN;
	}
	KernelTraceExceptionEvent("install", signum, 0, handler, OK);
	return OK;
}

static int32_t LUMEN_SYSV_ABI KernelRaiseException(uint64_t thread, int32_t signum)
{
	PRINT_NAME();
	const uint64_t handler = KernelExceptionHandlerForSignal(signum);
	if (signum != kExceptionRaiseSignal || handler == 0)
	{
		KernelTraceExceptionEvent("raise-rejected", signum, thread, handler, KERNEL_ERROR_EINVAL);
		return KERNEL_ERROR_EINVAL;
	}
	if (!KernelExceptionRoutingAvailable())
	{
		KernelTraceExceptionEvent("raise", signum, thread, handler, KERNEL_ERROR_ENOSYS);
		return KERNEL_ERROR_ENOSYS;
	}
	const int32_t result = PthreadQueueExceptionForHandle(thread, signum);
	KernelTraceExceptionEvent("raise", signum, thread, handler, result);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	// The guest uses this signal to start a stop-the-world collection; the
	// optional bounded snapshot lets a later fault be compared with the state
	// at each raise. Disabled unless the selector is set.
	static std::atomic<uint32_t> raise_snapshots {0};
	std::array<char, 16> snapshot_tag {};
	std::snprintf(snapshot_tag.data(), snapshot_tag.size(), "raise%u",
	              raise_snapshots.fetch_add(1, std::memory_order_relaxed) + 1);
	Core::VirtualMemory::SnapshotRecordedRegions(snapshot_tag.data());
#endif
	return result;
}

template <auto Function>
bool RegisterKernelUnityFunction(Loader::SymbolDatabase* symbols, const char* nid, const char32_t* debug_name)
{
	Loader::SymbolResolve entry {};
	entry.name                 = nid;
	entry.library              = "libkernel_unity";
	entry.library_version      = 1;
	entry.module               = "libkernel";
	entry.module_version_major = 1;
	entry.module_version_minor = 1;
	entry.type                 = Loader::SymbolType::Func;
	const auto target          = reinterpret_cast<uint64_t>(Function);
	if (!symbols->RegisterHostCallTarget(target)) return false;
	symbols->AddExact(entry, target, debug_name);
	constexpr auto metadata = Loader::MakeHostCallScalarMetadata<Function>();
	return symbols->SetHostCallScalarMetadata(target, metadata);
}

static void LUMEN_SYSV_ABI exit(int code)
{
	PRINT_NAME();
	LUMEN_LOGE("Guest libkernel exit(%d)\n", code);
	Libs::DumpLastCalls();

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	(void)Loader::RequestGuestSessionExit();
#else
	::exit(code);
#endif
}

static LUMEN_SYSV_ABI MallocReplace* KernelGetSanitizerMallocReplaceExternal()
{
	PRINT_NAME();

	EXIT_IF(g_sanitizer_replacements == nullptr);
	return &g_sanitizer_replacements->malloc_replace;
}

static LUMEN_SYSV_ABI NewReplace* KernelGetSanitizerNewReplaceExternal()
{
	PRINT_NAME();

	EXIT_IF(g_sanitizer_replacements == nullptr);
	return &g_sanitizer_replacements->new_replace;
}

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool SessionResetContractTest()
{
	PrepareSanitizerReplacementObjects();
	auto* malloc_replace = KernelGetSanitizerMallocReplaceExternal();
	auto* new_replace    = KernelGetSanitizerNewReplaceExternal();
	if (malloc_replace == nullptr || new_replace == nullptr)
	{
		return false;
	}

	malloc_replace->size              = 0;
	malloc_replace->malloc_initialize = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
	malloc_replace->free              = reinterpret_cast<void*>(static_cast<uintptr_t>(2));
	malloc_replace->aligned_alloc     = reinterpret_cast<void*>(static_cast<uintptr_t>(3));
	new_replace->size                 = 0;
	new_replace->new_p                = reinterpret_cast<void*>(static_cast<uintptr_t>(4));
	new_replace->delete_array         = reinterpret_cast<void*>(static_cast<uintptr_t>(5));
	new_replace->delete_with_size     = reinterpret_cast<void*>(static_cast<uintptr_t>(6));

	ResetSessionState();
	return KernelGetSanitizerMallocReplaceExternal() == malloc_replace &&
	       KernelGetSanitizerNewReplaceExternal() == new_replace &&
	       malloc_replace->size == sizeof(MallocReplace) &&
	       malloc_replace->malloc_initialize == nullptr && malloc_replace->malloc_finalize == nullptr &&
	       malloc_replace->malloc == nullptr && malloc_replace->free == nullptr &&
	       malloc_replace->calloc == nullptr && malloc_replace->realloc == nullptr &&
	       malloc_replace->memalign == nullptr && malloc_replace->reallocalign == nullptr &&
	       malloc_replace->posix_memalign == nullptr && malloc_replace->malloc_stats == nullptr &&
	       malloc_replace->malloc_stats_fast == nullptr && malloc_replace->malloc_usable_size == nullptr &&
	       malloc_replace->aligned_alloc == nullptr && new_replace->size == sizeof(NewReplace) &&
	       new_replace->new_p == nullptr && new_replace->new_nothrow == nullptr &&
	       new_replace->new_array == nullptr && new_replace->new_array_nothrow == nullptr &&
	       new_replace->delete_p == nullptr && new_replace->delete_nothrow == nullptr &&
	       new_replace->delete_array == nullptr && new_replace->delete_array_nothrow == nullptr &&
	       new_replace->delete_with_size == nullptr && new_replace->delete_with_size_nothrow == nullptr &&
	       new_replace->delete_array_with_size == nullptr &&
	       new_replace->delete_array_with_size_nothrow == nullptr;
}
#endif

static LUMEN_SYSV_ABI int elf_phdr_match_addr(ModuleInfo* m, uint64_t dtor_vaddr)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(m == nullptr);

	auto* rt     = Core::Singleton<Loader::RuntimeLinker>::Instance();
	auto* p      = rt->FindProgramByAddr(dtor_vaddr);
	int   result = (p != nullptr && p->unique_id == m->handle) ? 1 : 0;

	printf("\thandle     = %" PRId32 "\n", m->handle);
	printf("\tdtor_vaddr = %016" PRIx64 "\n", dtor_vaddr);
	printf("\tmatch      = %s\n", result == 1 ? "true" : "false");

	return result;
}

int LUMEN_SYSV_ABI KernelUuidCreate(uint32_t* uuid)
{
	PRINT_NAME();

	if (uuid == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	uuid[0] = Lumen::Math::Rand::Uint();
	uuid[1] = Lumen::Math::Rand::Uint();
	uuid[2] = Lumen::Math::Rand::Uint();
	uuid[3] = Lumen::Math::Rand::Uint();

	return OK;
}

static LUMEN_SYSV_ABI void pthread_cxa_finalize(void* /*p*/)
{
	PRINT_NAME();
}

void LUMEN_SYSV_ABI KernelSetThreadAtexitCount(get_thread_atexit_count_func_t func)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(g_get_thread_atexit_count_func != nullptr);

	g_get_thread_atexit_count_func = func;
}

void LUMEN_SYSV_ABI KernelSetThreadAtexitReport(thread_atexit_report_func_t func)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(g_thread_atexit_report_func != nullptr);

	g_thread_atexit_report_func = func;
}

int LUMEN_SYSV_ABI KernelRtldThreadAtexitIncrement(uint64_t* /*c*/)
{
	PRINT_NAME();

	//__sync_fetch_and_add(c, 1);

	return 0;
}

int LUMEN_SYSV_ABI KernelRtldThreadAtexitDecrement(uint64_t* /*c*/)
{
	PRINT_NAME();

	//__sync_fetch_and_sub(c, 1);

	return 0;
}

int LUMEN_SYSV_ABI KernelIsNeoMode()
{
	PRINT_NAME();

	return (Config::IsNeo() ? 1 : 0);
}

int LUMEN_SYSV_ABI clock_gettime(int clock_id, LibKernel::KernelTimespec* time)
{
	PRINT_NAME();

	return POSIX_CALL(LibKernel::KernelClockGettime(clock_id, time));
}

void LUMEN_SYSV_ABI KernelSetGPO(uint32_t bits)
{
	PRINT_NAME();

	printf("\t bits = %08" PRIx32 "\n", bits);
}

} // namespace LibKernel

namespace Posix {

LIB_VERSION("Posix", 1, "libkernel", 1, 1);

struct Timezone
{
	int32_t minutes_west;
	int32_t dst_time;
};

int LUMEN_SYSV_ABI clock_gettime(int clock_id, LibKernel::KernelTimespec* time)
{
	PRINT_NAME();

	return POSIX_CALL(LibKernel::KernelClockGettime(clock_id, time));
}

int LUMEN_SYSV_ABI nanosleep(const LibKernel::KernelTimespec* rqtp, LibKernel::KernelTimespec* rmtp)
{
	PRINT_NAME();

	return POSIX_CALL(LibKernel::KernelNanosleep(rqtp, rmtp));
}

int LUMEN_SYSV_ABI gettimeofday(LibKernel::KernelTimeval* time, Timezone* timezone)
{
	PRINT_NAME();

	if (time != nullptr &&
	    !Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(time), sizeof(*time), true))
	{
		*GetErrorAddr() = POSIX_EFAULT;
		return -1;
	}
	if (timezone != nullptr &&
	    !Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(timezone), sizeof(*timezone), true))
	{
		*GetErrorAddr() = POSIX_EFAULT;
		return -1;
	}
	if (time != nullptr)
	{
		const int result = LibKernel::KernelGettimeofday(time);
		if (result != OK)
		{
			*GetErrorAddr() = LibKernel::KernelToPosix(result);
			return -1;
		}
	}
	if (timezone != nullptr)
	{
		// FreeBSD kernels conventionally report this legacy structure as zero;
		// applications obtain the active UTC offset through localtime instead.
		constexpr Timezone neutral_timezone {};
		if (!Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(timezone),
		                              &neutral_timezone, sizeof(neutral_timezone)))
		{
			*GetErrorAddr() = POSIX_EFAULT;
			return -1;
		}
	}
	return 0;
}

int LUMEN_SYSV_ABI stat(const char* path, LibKernel::FileSystem::FileStat* sb)
{
	PRINT_NAME();

	return POSIX_CALL(LibKernel::FileSystem::KernelStat(path, sb));
}

int LUMEN_SYSV_ABI fstat(int descriptor, LibKernel::FileSystem::FileStat* state)
{
	PRINT_NAME();
	return POSIX_CALL(LibKernel::FileSystem::KernelFstat(descriptor, state));
}

int LUMEN_SYSV_ABI fchmod(int descriptor, uint32_t mode)
{
	PRINT_NAME();
	return POSIX_CALL(LibKernel::FileSystem::KernelFchmod(descriptor, mode));
}

int LUMEN_SYSV_ABI utimes(const char* path, const LibKernel::KernelTimeval* times)
{
	PRINT_NAME();
	return POSIX_CALL(LibKernel::FileSystem::KernelUtimes(path, times));
}

int LUMEN_SYSV_ABI futimes(int descriptor, const LibKernel::KernelTimeval* times)
{
	PRINT_NAME();
	return POSIX_CALL(LibKernel::FileSystem::KernelFutimes(descriptor, times));
}

int LUMEN_SYSV_ABI unlink(const char* path)
{
	PRINT_NAME();
	return POSIX_CALL(LibKernel::FileSystem::KernelUnlink(path));
}

int LUMEN_SYSV_ABI rename(const char* old_path, const char* new_path)
{
	PRINT_NAME();
	return POSIX_CALL(LibKernel::FileSystem::KernelRename(old_path, new_path));
}

int LUMEN_SYSV_ABI mkdir(const char* path, uint16_t mode)
{
	PRINT_NAME();
	return POSIX_CALL(LibKernel::FileSystem::KernelMkdir(path, mode));
}

int LUMEN_SYSV_ABI chmod(const char* path, uint32_t mode)
{
	PRINT_NAME();
	return POSIX_CALL(LibKernel::FileSystem::KernelChmod(path, mode));
}

int LUMEN_SYSV_ABI rmdir(const char* path)
{
	PRINT_NAME();
	return POSIX_CALL(LibKernel::FileSystem::KernelRmdir(path));
}

int LUMEN_SYSV_ABI sched_yield()
{
	LibKernel::PthreadYield();
	return 0;
}

int LUMEN_SYSV_ABI munmap(void* address, size_t length)
{
	PRINT_NAME();
	return POSIX_CALL(LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(address), length));
}

int LUMEN_SYSV_ABI fcntl(int descriptor, int command, uint64_t argument)
{
	PRINT_NAME();
	return POSIX_CALL(LibKernel::FileSystem::KernelFcntl(descriptor, command, argument));
}

int LUMEN_SYSV_ABI fsync(int descriptor)
{
	PRINT_NAME();
	return POSIX_CALL(LibKernel::FileSystem::KernelFsync(descriptor));
}

int LUMEN_SYSV_ABI pthread_cond_timedwait(
    LibKernel::PthreadCond* condition, LibKernel::PthreadMutex* mutex,
    const LibKernel::KernelTimespec* deadline)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(
	    LibKernel::PthreadCondTimedwaitAbsolute(condition, mutex, deadline));
}

int LUMEN_SYSV_ABI pthread_setcancelstate(int state, int* previous_state)
{
	PRINT_NAME();
	return POSIX_PTHREAD_CALL(
	    LibKernel::PthreadSetcancelstate(state, previous_state));
}

int LUMEN_SYSV_ABI ftruncate(int descriptor, int64_t length)
{
	PRINT_NAME();
	return POSIX_CALL(LibKernel::FileSystem::KernelFtruncate(descriptor, length));
}

int64_t LUMEN_SYSV_ABI lseek(int descriptor, int64_t offset, int whence)
{
	PRINT_NAME();
	return POSIX_N_CALL(LibKernel::FileSystem::KernelLseek(descriptor, offset, whence));
}

int PosixOfflineNetworkFailure()
{
	*GetErrorAddr() = POSIX_ENETDOWN;
	return -1;
}

int LUMEN_SYSV_ABI bind(int, const void*, uint32_t) { return PosixOfflineNetworkFailure(); }
int LUMEN_SYSV_ABI accept(int, void*, uint32_t*) { return PosixOfflineNetworkFailure(); }
int LUMEN_SYSV_ABI accept4(int, void*, uint32_t*, int) { return PosixOfflineNetworkFailure(); }
int LUMEN_SYSV_ABI connect(int, const void*, uint32_t) { return PosixOfflineNetworkFailure(); }
int LUMEN_SYSV_ABI getpeername(int, void*, uint32_t*) { return PosixOfflineNetworkFailure(); }
int LUMEN_SYSV_ABI getsockname(int, void*, uint32_t*) { return PosixOfflineNetworkFailure(); }
int LUMEN_SYSV_ABI getsockopt(int, int, int, void*, uint32_t*) { return PosixOfflineNetworkFailure(); }
int LUMEN_SYSV_ABI inet_pton(int, const char*, void*) { return PosixOfflineNetworkFailure(); }
int LUMEN_SYSV_ABI listen(int, int) { return PosixOfflineNetworkFailure(); }
int64_t LUMEN_SYSV_ABI recv(int, void*, size_t, int) { return PosixOfflineNetworkFailure(); }
int64_t LUMEN_SYSV_ABI recvfrom(int, void*, size_t, int, void*, uint32_t*) { return PosixOfflineNetworkFailure(); }
int64_t LUMEN_SYSV_ABI recvmsg(int, void*, int) { return PosixOfflineNetworkFailure(); }
int LUMEN_SYSV_ABI select(int, void*, void*, void*, LibKernel::KernelTimeval*) { return PosixOfflineNetworkFailure(); }
int64_t LUMEN_SYSV_ABI send(int, const void*, size_t, int) { return PosixOfflineNetworkFailure(); }
int64_t LUMEN_SYSV_ABI sendto(int, const void*, size_t, int, const void*, uint32_t) { return PosixOfflineNetworkFailure(); }
int64_t LUMEN_SYSV_ABI sendmsg(int, const void*, int) { return PosixOfflineNetworkFailure(); }
int LUMEN_SYSV_ABI setsockopt(int, int, int, const void*, uint32_t) { return PosixOfflineNetworkFailure(); }
int LUMEN_SYSV_ABI shutdown(int, int) { return PosixOfflineNetworkFailure(); }
int LUMEN_SYSV_ABI socket(int, int, int) { return PosixOfflineNetworkFailure(); }
int LUMEN_SYSV_ABI socketpair(int, int, int, int*) { return PosixOfflineNetworkFailure(); }

LIB_DEFINE(InitLibKernel_1_Posix)
{
	LIB_FUNC("lLMT9vJAck0", clock_gettime);
	LIB_FUNC("n88vx3C5nW8", gettimeofday);
	LIB_FUNC("yS8U2TGCe1A", nanosleep);
	LIB_FUNC("E6ao34wPw+U", stat);
	LIB_FUNC("mqQMh1zPPT8", fstat);
	LIB_FUNC("n01yNbQO5W4", fchmod);
	LIB_FUNC("GDuV00CHrUg", utimes);
	LIB_FUNC("+0EDo7YzcoU", futimes);
	LIB_FUNC("wuCroIGjt2g", LibKernel::open);
	LIB_FUNC("VAzswvTOCzI", unlink);
	LIB_FUNC("FN4gaPmuFV8", LibKernel::write);
	LIB_FUNC("NN01qLRhiqU", rename);
	LIB_FUNC("JGMio+21L4c", mkdir);
	LIB_FUNC("z0dtnPxYgtg", chmod);
	LIB_FUNC("AqBioC2vF3I", LibKernel::read);
	LIB_FUNC("c7ZnT7V1B98", rmdir);
	LIB_FUNC("bY-PO6JhzhQ", LibKernel::close);
	LIB_FUNC("6XG4B33N09g", sched_yield);
	LIB_FUNC("UqDGjXA5yUM", munmap);
	LIB_FUNC_SCALAR("8nY19bKoiZk", fcntl);
	LIB_FUNC_SCALAR("k+AXqu2-eBc", LibKernel::posix_getpagesize);
	LIB_FUNC_SCALAR("juWbTNM+8hw", fsync);
	LIB_FUNC_SCALAR("ih4CD9-gghM", ftruncate);
	LIB_FUNC_SCALAR("Oy6IpwgtYOk", lseek);
	LIB_FUNC_SCALAR("HoLVWNanBBc", LibKernel::posix_getpid);
	LIB_FUNC_EFFECTS("27bAgiJmOh0", pthread_cond_timedwait, HleEffect::GuestMemory | HleEffect::Blocking | HleEffect::GuestCallback);
	LIB_FUNC("lZzFeSxPl08", pthread_setcancelstate);
	LIB_FUNC_SCALAR("nYBrkGDqxh8", LibKernel::PthreadTestcancel);
	LIB_FUNC("KuOmgKoqCdY", bind);
	LIB_FUNC("3e+4Iv7IJ8U", accept);
	LIB_FUNC("G9QiVuWFeAE", accept4);
	LIB_FUNC("XVL8So3QJUk", connect);
	LIB_FUNC("TXFFFiNldU8", getpeername);
	LIB_FUNC("RenI1lL1WFk", getsockname);
	LIB_FUNC("6O8EwYOgH9Y", getsockopt);
	LIB_FUNC("4n51s0zEf0c", inet_pton);
	LIB_FUNC("pxnCmagrtao", listen);
	LIB_FUNC("Ez8xjo9UF4E", recv);
	LIB_FUNC("lUk6wrGXyMw", recvfrom);
	LIB_FUNC("hI7oVeOluPM", recvmsg);
	LIB_FUNC("T8fER+tIGgk", select);
	LIB_FUNC("fZOeZIOEmLw", send);
	LIB_FUNC("oBr313PppNE", sendto);
	LIB_FUNC("aNeavPDNKzA", sendmsg);
	LIB_FUNC("fFxGkxF2bVo", setsockopt);
	LIB_FUNC("TUuiYS2kE8s", shutdown);
	LIB_FUNC("TU-d9PfIHPM", socket);
	LIB_FUNC("MZb0GKT3mo8", socketpair);
	LIB_FUNC("BPE9s9vQQXo", LibKernel::posix_mmap);
	LIB_FUNC("tZY4+SZNFhA", LibKernel::posix_msync);
	LIB_FUNC("pDuPEf3m4fI", Posix::sem_init);
	LIB_FUNC("IKP8typ0QUk", Posix::sem_post);
	LIB_FUNC("YCV5dGGBcCo", Posix::sem_wait);
	LIB_FUNC("WBWzsRifCEA", Posix::sem_trywait);
	LIB_FUNC("w5IHyvahg-o", Posix::sem_timedwait);
	LIB_FUNC("cDW233RAwWo", Posix::sem_destroy);
	LIB_FUNC("04AjkP0jO9U", LibKernel::Umtx::UmtxOp);

	LIB_FUNC("OxhIB8LB-PQ", Posix::pthread_create);
	LIB_FUNC("h9CcP3J0oVM", Posix::pthread_join);
	LIB_FUNC("Xs9hdiD7sAA", Posix::pthread_setschedparam);
	LIB_FUNC("zHchY8ft5pk", Posix::pthread_attr_destroy);
	LIB_FUNC("wtkt-teR1so", Posix::pthread_attr_init);
	LIB_FUNC("2Q0z6rnBrTE", Posix::pthread_attr_setstacksize);
	LIB_FUNC("0D4-FVvEikw", Posix::pthread_cancel);
	LIB_FUNC("+U1R4WtXvoc", Posix::pthread_detach);
	LIB_FUNC("7Xl257M4VNI", Posix::pthread_equal);
	LIB_FUNC("FJrT5LuUBAU", Posix::pthread_exit);
	LIB_FUNC("FIs3-UQT9sg", Posix::pthread_getschedparam);
	LIB_FUNC_SCALAR_EFFECTS("EotR8a3ASf4", Posix::pthread_self, HleEffect::None);
	LIB_FUNC_SCALAR_EFFECTS("7H0iTOciTLo", Posix::pthread_mutex_lock, HleEffect::GuestMemory | HleEffect::Blocking | HleEffect::GuestCallback);
	LIB_FUNC("K-jXhbt2gn4", Posix::pthread_mutex_trylock);
	LIB_FUNC_SCALAR_EFFECTS("2Z+PpY6CaJg", Posix::pthread_mutex_unlock, HleEffect::GuestMemory);
	LIB_FUNC("ttHNfU+qDBU", Posix::pthread_mutex_init);
	LIB_FUNC("dQHWEsJtoE4", Posix::pthread_mutexattr_init);
	LIB_FUNC("mDmgMOGVUqg", Posix::pthread_mutexattr_settype);
	LIB_FUNC("5txKfcMUAok", Posix::pthread_mutexattr_setprotocol);
	LIB_FUNC("HF7lK46xzjY", Posix::pthread_mutexattr_destroy);
	LIB_FUNC("ltCfaGr2JGE", Posix::pthread_mutex_destroy);
	LIB_FUNC("1471ajPzxh0", Posix::pthread_rwlock_destroy);
	LIB_FUNC("ytQULN-nhL4", Posix::pthread_rwlock_init);
	LIB_FUNC("iGjsr1WAtI0", Posix::pthread_rwlock_rdlock);
	LIB_FUNC("SFxTMOfuCkE", Posix::pthread_rwlock_tryrdlock);
	LIB_FUNC("XhWHn6P5R7U", Posix::pthread_rwlock_trywrlock);
	LIB_FUNC("EgmLo6EWgso", Posix::pthread_rwlock_unlock);
	LIB_FUNC("sIlRvQqsN2Y", Posix::pthread_rwlock_wrlock);
	LIB_FUNC("qsdmgXjqSgk", Posix::pthread_rwlockattr_destroy);
	LIB_FUNC("xFebsA4YsFI", Posix::pthread_rwlockattr_init);
	LIB_FUNC("mkx2fVhNMsg", Posix::pthread_cond_broadcast);
	LIB_FUNC("RXXqi4CtF8w", Posix::pthread_cond_destroy);
	LIB_FUNC("0TyVk4MSLt0", Posix::pthread_cond_init);
	LIB_FUNC("2MOy+rUfuhQ", Posix::pthread_cond_signal);
	LIB_FUNC_EFFECTS("Op8TBGY5KHg", Posix::pthread_cond_wait, HleEffect::GuestMemory | HleEffect::Blocking | HleEffect::GuestCallback);
	LIB_FUNC("mqULNdimTn0", Posix::pthread_key_create);
	LIB_FUNC("6BpEZuDT7YI", Posix::pthread_key_delete);
	LIB_FUNC("WrOLvHU0yQM", Posix::pthread_setspecific);
	LIB_FUNC_SCALAR_EFFECTS("0-KXaS70xy4", Posix::pthread_getspecific, HleEffect::None);
}

} // namespace Posix

namespace FileSystem = LibKernel::FileSystem;
namespace Memory     = LibKernel::Memory;
namespace EventQueue = LibKernel::EventQueue;
namespace EventFlag  = LibKernel::EventFlag;
namespace Semaphore  = LibKernel::Semaphore;

LIB_DEFINE(InitLibKernel_1_FS)
{
	LIB_FUNC("1G3lF1Gg1k8", FileSystem::KernelOpen);
	LIB_FUNC("UK2Tl2DWUns", FileSystem::KernelClose);
	LIB_FUNC("Cg4srZ6TKbU", FileSystem::KernelRead);
	LIB_FUNC("4wSze92BhLI", FileSystem::KernelWrite);
	LIB_FUNC("+r3rMFwItV4", FileSystem::KernelPread);
	LIB_FUNC("nKWi-N2HBV4", FileSystem::KernelPwrite);
	LIB_FUNC("eV9wAD2riIA", FileSystem::KernelStat);
	LIB_FUNC("kBwCPsYX-m4", FileSystem::KernelFstat);
	LIB_FUNC_SCALAR("SoZkxZkCHaw", FileSystem::KernelFcntl);
	LIB_FUNC("AUXVxWeJU-A", FileSystem::KernelUnlink);
	LIB_FUNC_SCALAR("52NcYU9+lEo", FileSystem::KernelRename);
	LIB_FUNC_SCALAR("naInUjYt3so", FileSystem::KernelRmdir);
	LIB_FUNC_SCALAR("c7ZnT7V1B98", Posix::rmdir);
	LIB_FUNC_SCALAR("fTx66l5iWIA", FileSystem::KernelFsync);
	LIB_FUNC_SCALAR("VW3TVZiM4-E", FileSystem::KernelFtruncate);
	LIB_FUNC_SCALAR("UtszJWHrDcA", FileSystem::KernelFchmod);
	LIB_FUNC_SCALAR("fgIsQ10xYVA", FileSystem::KernelChmod);
	LIB_FUNC_SCALAR("0Cq8ipKr9n0", FileSystem::KernelUtimes);
	LIB_FUNC_SCALAR("NLq2b1jOaN0", FileSystem::KernelFutimes);
	LIB_FUNC("taRWhTJFTgE", FileSystem::KernelGetdirentries);
	LIB_FUNC("oib76F-12fk", FileSystem::KernelLseek);
	LIB_FUNC("j2AIqSqJP0w", FileSystem::KernelGetdents);
	LIB_FUNC("1-LFLmRFxxM", FileSystem::KernelMkdir);
}

LIB_DEFINE(InitLibKernel_1_Mem)
{
	LIB_FUNC_SCALAR("7oxv3PPCumo", Memory::KernelReserveVirtualRange);
	LIB_FUNC_SCALAR("DGMG3JshrZU", Memory::KernelSetVirtualRangeName);
	LIB_FUNC_SCALAR("rVjRvHJ0X6c", Memory::KernelVirtualQuery);
	LIB_FUNC("mL8NDH86iQI", Memory::KernelMapNamedFlexibleMemory);
	LIB_FUNC("IWIBBdTHit4", Memory::KernelMapFlexibleMemory);
	LIB_FUNC_EFFECTS("cQke9UuBQOk", Memory::KernelMunmap, HleEffect::GuestMemory | HleEffect::VmMutation | HleEffect::Blocking);
	LIB_FUNC("pO96TwzOm5E", Memory::KernelGetDirectMemorySize);
	LIB_FUNC_SCALAR("jh+8XiK4LeE", Memory::KernelIsAddressSanitizerEnabled);
	LIB_FUNC_EFFECTS("rTXw65xmLIA", Memory::KernelAllocateDirectMemory, HleEffect::GuestMemory | HleEffect::VmMutation | HleEffect::Blocking);
	LIB_FUNC("B+vc2AO2Zrc", Memory::KernelAllocateMainDirectMemory);
	LIB_FUNC_EFFECTS("L-Q3LEjIbgA", Memory::KernelMapDirectMemory, HleEffect::GuestMemory | HleEffect::VmMutation | HleEffect::Blocking);
	LIB_FUNC("NcaWUxfMNIQ", Memory::KernelMapNamedDirectMemory);
	LIB_FUNC("MBuItvba6z8", Memory::KernelReleaseDirectMemory);
	LIB_FUNC("hwVSPCmp5tM", Memory::KernelCheckedReleaseDirectMemory);
	LIB_FUNC("WFcfL2lzido", Memory::KernelQueryMemoryProtection);
	LIB_FUNC("BHouLQzh0X0", Memory::KernelDirectMemoryQuery);
	LIB_FUNC("aNz11fnnzi4", Memory::KernelAvailableFlexibleMemorySize);
	LIB_FUNC_EFFECTS("vSMAm3cxYTY", Memory::KernelMprotect, HleEffect::GuestMemory | HleEffect::VmMutation | HleEffect::Blocking);
}

LIB_DEFINE(InitLibKernel_1_Equeue)
{
	LIB_FUNC("D0OdFMjp46I", EventQueue::KernelCreateEqueue);
	LIB_FUNC("jpFjmgAC5AE", EventQueue::KernelDeleteEqueue);
	LIB_FUNC_EFFECTS("fzyMKs9kim0", EventQueue::KernelWaitEqueue, HleEffect::GuestMemory | HleEffect::Blocking | HleEffect::GuestCallback);
	LIB_FUNC_SCALAR("23CPPI1tyBY", EventQueue::KernelGetEventFilter);
	LIB_FUNC_SCALAR("mJ7aghmgvfc", EventQueue::KernelGetEventId);
	LIB_FUNC_SCALAR("kwGyyjohI50", EventQueue::KernelGetEventData);
	LIB_FUNC_SCALAR("Q0qr9AyqJSk", EventQueue::KernelGetEventFflags);
	LIB_FUNC("vz+pg2zdopI", EventQueue::KernelGetEventUserData);
	LIB_FUNC("4R6-OvI2cEA", EventQueue::KernelAddUserEvent);
	LIB_FUNC_SCALAR("WDszmSbWuDk", EventQueue::KernelAddUserEventEdge);
	LIB_FUNC("F6e0kwo4cnk", EventQueue::KernelTriggerUserEvent);
	LIB_FUNC("LJDwdSNTnDg", EventQueue::KernelDeleteUserEvent);
}

LIB_DEFINE(InitLibKernel_1_EventFlag)
{
	LIB_FUNC("BpFoboUJoZU", EventFlag::KernelCreateEventFlag);
	LIB_FUNC("8mql9OcQnd4", EventFlag::KernelDeleteEventFlag);
	LIB_FUNC("JTvBflhYazQ", EventFlag::KernelWaitEventFlag);
	LIB_FUNC_SCALAR("9lvj5DjHZiA", EventFlag::KernelPollEventFlag);
	LIB_FUNC("IOnSvHzqu6A", EventFlag::KernelSetEventFlag);
	LIB_FUNC("7uhBFWRAS60", EventFlag::KernelClearEventFlag);
	LIB_FUNC_SCALAR("PZku4ZrXJqg", EventFlag::KernelCancelEventFlag);
}

LIB_DEFINE(InitLibKernel_1_Semaphore)
{
	// These are POSIX ABI aliases, not scePthreadSem* kernel-error adapters.
	LIB_FUNC("pDuPEf3m4fI", Posix::sem_init);
	LIB_FUNC("IKP8typ0QUk", Posix::sem_post);
	LIB_FUNC("YCV5dGGBcCo", Posix::sem_wait);
	LIB_FUNC("WBWzsRifCEA", Posix::sem_trywait);
	LIB_FUNC("w5IHyvahg-o", Posix::sem_timedwait);
	LIB_FUNC("cDW233RAwWo", Posix::sem_destroy);
	LIB_FUNC("04AjkP0jO9U", LibKernel::Umtx::UmtxOp);
	LIB_FUNC("188x57JYp0g", Semaphore::KernelCreateSema);
	LIB_FUNC("R1Jvn8bSCW8", Semaphore::KernelDeleteSema);
	LIB_FUNC_EFFECTS("Zxa0VhQVTsk", Semaphore::KernelWaitSema, HleEffect::GuestMemory | HleEffect::Blocking | HleEffect::GuestCallback);
	LIB_FUNC("4czppHBiriw", Semaphore::KernelSignalSema);
	LIB_FUNC_SCALAR("12wOHk8ywb0", Semaphore::KernelPollSema);
	LIB_FUNC_SCALAR("4DM06U2BNEY", Semaphore::KernelCancelSema);
	LIB_FUNC("GEnUkDZoUwY", Semaphore::PthreadSemInit);
	LIB_FUNC("Vwc+L05e6oE", Semaphore::PthreadSemDestroy);
	LIB_FUNC("C36iRE0F5sE", Semaphore::PthreadSemWait);
	LIB_FUNC("H2a+IN9TP0E", Semaphore::PthreadSemTrywait);
	LIB_FUNC("fjN6NQHhK8k", Semaphore::PthreadSemTimedwait);
	LIB_FUNC("aishVAiFaYM", Semaphore::PthreadSemPost);
	LIB_FUNC("DjpBvGlaWbQ", Semaphore::PthreadSemGetvalue);
}

LIB_DEFINE(InitLibKernel_1_Pthread)
{
	LIB_FUNC_SCALAR_EFFECTS("9UK1vLZQft4", LibKernel::PthreadMutexLock, HleEffect::GuestMemory | HleEffect::Blocking | HleEffect::GuestCallback);
	LIB_FUNC_SCALAR_EFFECTS("tn3VlD0hG60", LibKernel::PthreadMutexUnlock, HleEffect::GuestMemory);
	LIB_FUNC("2Of0f+3mhhE", LibKernel::PthreadMutexDestroy);
	LIB_FUNC("cmo1RIYva9o", LibKernel::PthreadMutexInit);
	LIB_FUNC("upoVrzMHFeE", LibKernel::PthreadMutexTrylock);
	LIB_FUNC_EFFECTS("IafI2PxcPnQ", LibKernel::PthreadMutexTimedlock, HleEffect::GuestMemory | HleEffect::Blocking | HleEffect::GuestCallback);
	LIB_FUNC("smWEktiyyG0", LibKernel::PthreadMutexattrDestroy);
	LIB_FUNC("F8bUHwAG284", LibKernel::PthreadMutexattrInit);
	LIB_FUNC("iMp8QpE+XO4", LibKernel::PthreadMutexattrSettype);
	LIB_FUNC("1FGvU0i9saQ", LibKernel::PthreadMutexattrSetprotocol);
	LIB_FUNC_SCALAR_EFFECTS("14bOACANTBo", LibKernel::PthreadOnce, HleEffect::GuestMemory | HleEffect::Blocking | HleEffect::GuestCallback);

	LIB_FUNC_SCALAR_EFFECTS("aI+OeCz8xrQ", LibKernel::PthreadSelf, HleEffect::None);
	LIB_FUNC_EFFECTS("6UgtwV+0zb4", LibKernel::PthreadCreate, HleEffect::GuestMemory | HleEffect::GuestCallback | HleEffect::RetainsGuestPointer);
#if !defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && \
     defined(LUMEN_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && \
     defined(LUMEN_ANDROID_FEXCORE_RUNTIME))
	LIB_FUNC("3kg7rT0NQIs", LibKernel::PthreadExit);
#endif
	LIB_FUNC_SCALAR("3PtV6p3QNX4", LibKernel::PthreadEqual);
	LIB_FUNC_EFFECTS("onNY9Byn-W8", LibKernel::PthreadJoin, HleEffect::GuestMemory | HleEffect::Blocking | HleEffect::GuestCallback);
	LIB_FUNC("4qGrR6eoP9Y", LibKernel::PthreadDetach);
	LIB_FUNC_SCALAR("qBDmpCyGssE", LibKernel::PthreadCancel);
	LIB_FUNC_SCALAR("OAmWq+OHSjw", LibKernel::PthreadSetcancelstate);
	LIB_FUNC_SCALAR("sCJd99Phct0", LibKernel::PthreadSetcanceltype);
	LIB_FUNC_SCALAR("LapIb799SSE", LibKernel::PthreadTestcancel);
	// POSIX ABI alias of the same cancellation point: a title's statically
	// linked semaphore code calls it without arguments on its error path.
	LIB_FUNC_SCALAR("nYBrkGDqxh8", LibKernel::PthreadTestcancel);
	LIB_FUNC("How7B8Oet6k", LibKernel::PthreadGetname);
	LIB_FUNC_SCALAR("GBUY7ywdULE", LibKernel::PthreadRename);
	LIB_FUNC("rcrVFJsQWRY", LibKernel::PthreadGetaffinity);
	LIB_FUNC_SCALAR("VOx8NGmHXTs", LibKernel::KernelGetCpumode);
	LIB_FUNC("bt3CTBKmGyI", LibKernel::PthreadSetaffinity);
	LIB_FUNC("1tKyG7RlMJo", LibKernel::PthreadGetprio);
	LIB_FUNC("P41kTWUS3EI", LibKernel::PthreadGetschedparam);
	LIB_FUNC("oIRFTjoILbg", LibKernel::PthreadSetschedparam);
	LIB_FUNC("W0Hpm2X0uPE", LibKernel::PthreadSetprio);
	LIB_FUNC("T72hz6ffq08", LibKernel::PthreadYield);
	LIB_FUNC_SCALAR_EFFECTS("EI-5-jlq2dE", LibKernel::PthreadGetthreadid, HleEffect::None);
	LIB_FUNC_SCALAR("1xvtUVx1-Sg", LibKernel::PthreadCleanupPush);
	LIB_FUNC_SCALAR("iWsFlYMf3Kw", LibKernel::PthreadCleanupPop);
	LIB_FUNC_EFFECTS("geDaqgH9lTg", LibKernel::PthreadKeyCreate, HleEffect::GuestMemory | HleEffect::GuestCallback | HleEffect::RetainsGuestPointer);
	LIB_FUNC("PrdHuuDekhY", LibKernel::PthreadKeyDelete);
	LIB_FUNC_EFFECTS("+BzXYkqYeLE", LibKernel::PthreadSetspecific, HleEffect::GuestMemory | HleEffect::GuestCallback | HleEffect::RetainsGuestPointer);
	LIB_FUNC_SCALAR_EFFECTS("eoht7mQOCmo", LibKernel::PthreadGetspecific, HleEffect::None);

	LIB_FUNC("62KCwEMmzcM", LibKernel::PthreadAttrDestroy);
	LIB_FUNC("x1X76arYMxU", LibKernel::PthreadAttrGet);
	LIB_FUNC("8+s5BzZjxSg", LibKernel::PthreadAttrGetaffinity);
	LIB_FUNC("nsYoNRywwNg", LibKernel::PthreadAttrInit);
	LIB_FUNC("JaRMy+QcpeU", LibKernel::PthreadAttrGetdetachstate);
	LIB_FUNC_SCALAR("FXPWHNk8Of0", LibKernel::PthreadAttrGetschedparam);
	LIB_FUNC("Ru36fiTtJzA", LibKernel::PthreadAttrGetstackaddr);
	LIB_FUNC("-fA+7ZlGDQs", LibKernel::PthreadAttrGetstacksize);
	LIB_FUNC("txHtngJ+eyc", LibKernel::PthreadAttrGetguardsize);
	LIB_FUNC("UTXzJbWhhTE", LibKernel::PthreadAttrSetstacksize);
	LIB_FUNC("-Wreprtu0Qs", LibKernel::PthreadAttrSetdetachstate);
	LIB_FUNC("El+cQ20DynU", LibKernel::PthreadAttrSetguardsize);
	LIB_FUNC("eXbUSpEaTsA", LibKernel::PthreadAttrSetinheritsched);
	LIB_FUNC("DzES9hQF4f4", LibKernel::PthreadAttrSetschedparam);
	LIB_FUNC("4+h9EzwKF4I", LibKernel::PthreadAttrSetschedpolicy);
	LIB_FUNC("3qxgM4ezETA", LibKernel::PthreadAttrSetaffinity);
	LIB_FUNC_SCALAR("lpMP8HhkBbg", LibKernel::PthreadAttrGetinheritsched);
	LIB_FUNC_SCALAR("NMyIQ9WgWbU", LibKernel::PthreadAttrGetschedpolicy);
	LIB_FUNC_SCALAR("-quPa4SEJUw", LibKernel::PthreadAttrGetstack);
	LIB_FUNC_SCALAR("Bvn74vj6oLo", LibKernel::PthreadAttrSetstack);
	LIB_FUNC_SCALAR("F+yfmduIBB8", LibKernel::PthreadAttrSetstackaddr);

	LIB_FUNC("6ULAa0fq4jA", LibKernel::PthreadRwlockInit);
	LIB_FUNC("BB+kb08Tl9A", LibKernel::PthreadRwlockDestroy);
	LIB_FUNC_EFFECTS("Ox9i0c7L5w0", LibKernel::PthreadRwlockRdlock, HleEffect::GuestMemory | HleEffect::Blocking | HleEffect::GuestCallback);
	LIB_FUNC("+L98PIbGttk", LibKernel::PthreadRwlockUnlock);
	LIB_FUNC_EFFECTS("mqdNorrB+gI", LibKernel::PthreadRwlockWrlock, HleEffect::GuestMemory | HleEffect::Blocking | HleEffect::GuestCallback);
	LIB_FUNC_SCALAR_EFFECTS("iPtZRWICjrM", LibKernel::PthreadRwlockTimedrdlock, HleEffect::GuestMemory | HleEffect::Blocking | HleEffect::GuestCallback);
	LIB_FUNC_SCALAR_EFFECTS("adh--6nIqTk", LibKernel::PthreadRwlockTimedwrlock, HleEffect::GuestMemory | HleEffect::Blocking | HleEffect::GuestCallback);
	LIB_FUNC_SCALAR("XD3mDeybCnk", LibKernel::PthreadRwlockTryrdlock);
	LIB_FUNC_SCALAR("bIHoZCTomsI", LibKernel::PthreadRwlockTrywrlock);
	LIB_FUNC_SCALAR("i2ifZ3fS2fo", LibKernel::PthreadRwlockattrDestroy);
	LIB_FUNC_SCALAR("yOfGg-I1ZII", LibKernel::PthreadRwlockattrInit);
	LIB_FUNC_SCALAR("Kyls1ChFyrc", LibKernel::PthreadRwlockattrGettype);
	LIB_FUNC_SCALAR("h-OifiouBd8", LibKernel::PthreadRwlockattrSettype);

	LIB_FUNC("2Tb92quprl0", LibKernel::PthreadCondInit);
	LIB_FUNC("g+PZd2hiacg", LibKernel::PthreadCondDestroy);
	LIB_FUNC_EFFECTS("WKAXJ4XBPQ4", LibKernel::PthreadCondWait, HleEffect::GuestMemory | HleEffect::Blocking | HleEffect::GuestCallback);
	LIB_FUNC("JGgj7Uvrl+A", LibKernel::PthreadCondBroadcast);
	LIB_FUNC("kDh-NfxgMtE", LibKernel::PthreadCondSignal);
	LIB_FUNC_EFFECTS("BmMjYxmew1w", LibKernel::PthreadCondTimedwait, HleEffect::GuestMemory | HleEffect::Blocking | HleEffect::GuestCallback);
	LIB_FUNC("m5-2bsNfv7s", LibKernel::PthreadCondattrInit);
	LIB_FUNC("waPcxYiR3WA", LibKernel::PthreadCondattrDestroy);

	LIB_FUNC("QBi7HCK03hw", LibKernel::KernelClockGettime);
	LIB_FUNC_SCALAR("wRYVA5Zolso", LibKernel::KernelClockGetres);
	LIB_FUNC_SCALAR_EFFECTS("QvsZxomvUHs", LibKernel::KernelNanosleep, HleEffect::GuestMemory | HleEffect::Blocking | HleEffect::GuestCallback);
	LIB_FUNC("ejekcaNQNq0", LibKernel::KernelGettimeofday);
	// These explicit libkernel identities retain the POSIX return/errno ABI.
	LIB_FUNC_SCALAR("n88vx3C5nW8", Posix::gettimeofday);
	LIB_FUNC_SCALAR("yS8U2TGCe1A", Posix::nanosleep);
	LIB_FUNC_SCALAR_EFFECTS("-2IRUCO--PM", LibKernel::KernelReadTsc, HleEffect::None);
	LIB_FUNC_SCALAR_EFFECTS("1j3S3n-tTW4", LibKernel::KernelGetTscFrequency, HleEffect::None);
	LIB_FUNC_SCALAR_EFFECTS("4J2sUJmuHZQ", LibKernel::KernelGetProcessTime, HleEffect::None);
	LIB_FUNC_SCALAR_EFFECTS("BNowx2l588E", LibKernel::KernelGetProcessTimeCounterFrequency, HleEffect::None);
	LIB_FUNC_SCALAR_EFFECTS("fgxnMeTNUtY", LibKernel::KernelGetProcessTimeCounter, HleEffect::None);

	LIB_FUNC_SCALAR_EFFECTS("7H0iTOciTLo", Posix::pthread_mutex_lock, HleEffect::GuestMemory | HleEffect::Blocking | HleEffect::GuestCallback);
	LIB_FUNC_SCALAR_EFFECTS("2Z+PpY6CaJg", Posix::pthread_mutex_unlock, HleEffect::GuestMemory);
	LIB_FUNC("1471ajPzxh0", Posix::pthread_rwlock_destroy);
	LIB_FUNC("ytQULN-nhL4", Posix::pthread_rwlock_init);
	LIB_FUNC("iGjsr1WAtI0", Posix::pthread_rwlock_rdlock);
	LIB_FUNC("SFxTMOfuCkE", Posix::pthread_rwlock_tryrdlock);
	LIB_FUNC("XhWHn6P5R7U", Posix::pthread_rwlock_trywrlock);
	LIB_FUNC("EgmLo6EWgso", Posix::pthread_rwlock_unlock);
	LIB_FUNC("sIlRvQqsN2Y", Posix::pthread_rwlock_wrlock);
	LIB_FUNC("qsdmgXjqSgk", Posix::pthread_rwlockattr_destroy);
	LIB_FUNC("xFebsA4YsFI", Posix::pthread_rwlockattr_init);
	LIB_FUNC("mkx2fVhNMsg", Posix::pthread_cond_broadcast);
	LIB_FUNC_EFFECTS("Op8TBGY5KHg", Posix::pthread_cond_wait, HleEffect::GuestMemory | HleEffect::Blocking | HleEffect::GuestCallback);
}

LIB_DEFINE(InitLibKernel_1)
{
	LibKernel::PrepareKernelAbiObjects();
	LibKernel::PrepareSanitizerReplacementObjects();
	InitLibKernel_1_FS(s);
	InitLibKernel_1_Mem(s);
	InitLibKernel_1_Equeue(s);
	InitLibKernel_1_EventFlag(s);
	InitLibKernel_1_Semaphore(s);
	InitLibKernel_1_Pthread(s);
	Posix::InitLibKernel_1_Posix(s);

	LIB_OBJECT("f7uOxY9mM1U", LibKernel::g_stack_chk_guard);
	LIB_OBJECT("djxxOmW6-aw", &LibKernel::g_progname->name);
	EXIT_IF(!LibKernel::RegisterKernelUnityFunction<&LibKernel::KernelInstallExceptionHandler>(
	    s, "WkwEd3N7w0Y", U"LibKernel::KernelInstallExceptionHandler"));
	EXIT_IF(!LibKernel::RegisterKernelUnityFunction<&LibKernel::KernelRaiseException>(
	    s, "il03nluKfMk", U"LibKernel::KernelRaiseException"));

	LIB_FUNC_EFFECTS("1jfXLRVzisc", LibKernel::KernelUsleep, HleEffect::Blocking | HleEffect::GuestCallback);
	LIB_FUNC_EFFECTS("-ZR+hG7aDHw", LibKernel::KernelSleep, HleEffect::Blocking | HleEffect::GuestCallback);
	LIB_FUNC("6c3rCVE-fTU", LibKernel::open);
	LIB_FUNC("6xVpy0Fdq+I", LibKernel::rtld_sigprocmask);
	LIB_FUNC("aPcyptbOiZs", LibKernel::sigprocmask);
	LIB_FUNC("VkTAsrZDcJ0", LibKernel::sigfillset);
	LIB_FUNC("crb5j7mkk1c", LibKernel::is_signal_return);
	LIB_FUNC("k+AXqu2-eBc", LibKernel::posix_getpagesize);
	LIB_FUNC("BPE9s9vQQXo", LibKernel::posix_mmap);
	LIB_FUNC("tZY4+SZNFhA", LibKernel::posix_msync);
	LIB_FUNC("6Z83sYWFlA8", LibKernel::exit);
	LIB_FUNC("8OnWXlgQlvo", LibKernel::KernelRtldThreadAtexitDecrement);
	LIB_FUNC("959qrazPIrg", LibKernel::KernelGetProcParam);
	LIB_FUNC_SCALAR("9BcDykPmo1I", LibKernel::get_error_addr);
	LIB_FUNC_SCALAR("bnZxYgAFeA0", LibKernel::KernelGetSanitizerNewReplaceExternal);
	LIB_FUNC("ca7v6Cxulzs", LibKernel::KernelSetGPO);
	LIB_FUNC("DRuBt2pvICk", LibKernel::read);
	LIB_FUNC("f7KBOafysXo", LibKernel::KernelGetModuleInfoFromAddr);
	LIB_FUNC("RpQJJVKTiFM", LibKernel::KernelGetModuleInfoForUnwind);
	LIB_FUNC("Fjc4-n1+y2g", LibKernel::elf_phdr_match_addr);
	LIB_FUNC("FxVZqBAA7ks", LibKernel::write);
	LIB_FUNC("kbw4UHHSYy0", LibKernel::pthread_cxa_finalize);
	LIB_FUNC("lLMT9vJAck0", LibKernel::clock_gettime);
	LIB_FUNC("NNtFaKJbPt0", LibKernel::close);
	LIB_FUNC("OMDRKKAZ8I4", LibKernel::KernelDebugRaiseException);
	LIB_FUNC("Ou3iL1abvng", LibKernel::stack_chk_fail);
	LIB_FUNC("p5EcQeEeJAE", LibKernel::KernelRtldSetApplicationHeapAPI);
	LIB_FUNC("pB-yGZ2nQ9o", LibKernel::KernelSetThreadAtexitCount);
	LIB_FUNC_SCALAR("py6L8jiVAN8", LibKernel::KernelGetSanitizerMallocReplaceExternal);
	LIB_FUNC("QKd0qM58Qes", LibKernel::KernelStopUnloadModule);
	LIB_FUNC_EFFECTS("rNhWz+lvOMU", LibKernel::KernelSetThreadDtors, HleEffect::GuestMemory | HleEffect::GuestCallback | HleEffect::RetainsGuestPointer);
	LIB_FUNC("Tz4RNUCBbGI", LibKernel::KernelRtldThreadAtexitIncrement);
	LIB_FUNC_SCALAR("vNe1w4diLCs", LibKernel::tls_get_addr);
	LIB_FUNC("WhCc1w3EhSI", LibKernel::KernelSetThreadAtexitReport);
	LIB_FUNC("WslcK1FQcGI", LibKernel::KernelIsNeoMode);
	LIB_FUNC("wzvqT4UqKX8", LibKernel::KernelLoadStartModule);
	LIB_FUNC("LwG8g3niqwA", LibKernel::KernelDlsym);
	LIB_FUNC("Xjoosiw+XPI", LibKernel::KernelUuidCreate);
	LIB_FUNC("zE-wXIZjLoM", LibKernel::KernelDebugRaiseExceptionOnReleaseMode);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
