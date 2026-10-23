#include "Emulator/Loader/RuntimeLinker.h"

#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/File.h"
#include "Lumen/Core/MagicEnum.h"
#include "Lumen/Core/Singleton.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"
#include "Lumen/Sys/SysDbg.h"

#include "Emulator/Config.h"
#include "CpuX64.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Printf.h"
#include "Emulator/Libs/VaContext.h"
#include "Emulator/Loader/Elf.h"
#include "Emulator/Loader/Jit.h"
#include "Emulator/Loader/HostCallSymbolCache.h"
#include "Emulator/Loader/PrelinkedPlt.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Profiler.h"
#include "Emulator/StartupTrace.h"

#if defined(LUMEN_FEXCORE_RUNTIME)
#include "AndroidFexCoreRuntime.h"
#include "GuestCpuRuntime.h"
#if defined(LUMEN_WITH_QEMU_TCG)
#include "QemuProvider.h"
#endif
#include "FexRelocationPatchEpoch.h"
#if defined(__APPLE__)
#include "Emulator/Kernel/FileSystem.h"
#endif
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <string>

#if defined(__ANDROID__)
#include <android/log.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
#include <SDL_events.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

#if defined(__APPLE__) && defined(__aarch64__)
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
#include <mach/mach_vm.h>
#else
#include <mach/vm_map.h>
#endif
#endif

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::LibKernel {
void SetProgName(const String& name);
} // namespace Lumen::Libs::LibKernel

#if defined(__aarch64__)
#if defined(__APPLE__)
namespace Lumen::Loader {
uint64_t translate_guest_address(uint64_t address, size_t size, bool write);
}
#endif

#if !defined(LUMEN_FEXCORE_RUNTIME)
static bool resolve_diagnostic_module(const char* name, uint64_t* base, uint64_t* size)
{
	auto* program = Lumen::Loader::RuntimeLinker::Instance()->FindProgramByName(name);
	if (program == nullptr || base == nullptr || size == nullptr) return false;
	*base = program->base_vaddr;
	*size = program->base_size;
	return true;
}

static void print_diagnostic_location(uint64_t rip, uint64_t frame_ptr)
{
	auto* linker = Lumen::Loader::RuntimeLinker::Instance();
	auto* program = linker->FindProgramByAddr(rip);
	std::fprintf(stderr, "Diagnostic guest location: thread=%d rip=0x%016" PRIx64,
	             Lumen::Core::Thread::GetThreadIdUnique(), rip);
	if (program != nullptr)
	{
		std::fprintf(stderr, " module=%s base=0x%016" PRIx64 " offset=0x%016" PRIx64,
		             program->file_name.FilenameWithoutDirectory().C_Str(), program->base_vaddr, rip - program->base_vaddr);
	}
	std::fprintf(stderr, "\n");
#if defined(__APPLE__)
	std::fprintf(stderr, "Diagnostic guest frame chain:");
	for (uint32_t depth = 0; depth < 16; depth++)
	{
		const uint64_t translated = Lumen::Loader::translate_guest_address(frame_ptr, 2 * sizeof(uint64_t), false);
		if (translated == 0) break;
		uint64_t next_frame = 0;
		uint64_t return_rip = 0;
		std::memcpy(&next_frame, reinterpret_cast<const void*>(translated), sizeof(next_frame));
		std::memcpy(&return_rip, reinterpret_cast<const void*>(translated + sizeof(uint64_t)), sizeof(return_rip));
		std::fprintf(stderr, " %016" PRIx64, return_rip);
		if (next_frame <= frame_ptr) break;
		frame_ptr = next_frame;
	}
	std::fprintf(stderr, "\n");
#else
	linker->StackTrace(frame_ptr);
#endif
}
#endif
#endif

namespace Lumen::Loader {

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
// Private x86-64 stack allocation owning the current guest run's RSP on this
// thread. RunGuestX64 pushes its own bounds for the duration of the synchronous
// run; nested runs restore the previous ones. Zero outside a run. The local
// guest-root snapshot uses it to bound its stack window; it never authorizes an
// access to a guest address.
thread_local uint64_t g_guest_run_stack_base = 0;
thread_local uint64_t g_guest_run_stack_size = 0;

class ScopedGuestRunStackBounds
{
public:
	ScopedGuestRunStackBounds(uint64_t base, uint64_t size) noexcept
	    : m_previous_base(g_guest_run_stack_base),
	      m_previous_size(g_guest_run_stack_size)
	{
		g_guest_run_stack_base = base;
		g_guest_run_stack_size = size;
	}
	~ScopedGuestRunStackBounds() noexcept
	{
		g_guest_run_stack_base = m_previous_base;
		g_guest_run_stack_size = m_previous_size;
	}

	LUMEN_CLASS_NO_COPY(ScopedGuestRunStackBounds);

private:
	uint64_t m_previous_base = 0;
	uint64_t m_previous_size = 0;
};

bool CurrentGuestRunStackBounds(uint64_t* base, uint64_t* size)
{
	if (base == nullptr || size == nullptr || g_guest_run_stack_size == 0) return false;
	*base = g_guest_run_stack_base;
	*size = g_guest_run_stack_size;
	return true;
}
#endif

ScopedGuestHostWait::ScopedGuestHostWait() noexcept
{
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID) && \
    defined(LUMEN_FEXCORE_RUNTIME)
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	if (!Core::VirtualMemory::CurrentThreadHasMemoryLease())
		m_token = LumenAndroidFexCoreBeginQuiescentHostWaitV1();
#else
	// Android data borrows retain this FEX host-call reader. The adapter denies
	// a quiescent wait while any of those borrows is still outstanding.
	m_token = LumenAndroidFexCoreBeginQuiescentHostWaitV1();
#endif
#endif
}

ScopedGuestHostWait::~ScopedGuestHostWait() noexcept
{
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID) && \
    defined(LUMEN_FEXCORE_RUNTIME)
	if (m_token != 0 && LumenAndroidFexCoreEndQuiescentHostWaitV1(m_token) == 0)
	{
		std::fputs("FEX host wait failed to restore its guest-memory reader\n", stderr);
		std::abort();
	}
#endif
	// With the guest reader restored and the wait's own locks released, deliver
	// a pending guest exception before the caller resumes. The handler runs
	// nested on this thread with the interrupted boundary state of the wait and
	// the wait then continues when it returns.
	Libs::LibKernel::PthreadDeliverPendingException();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	// Slice tick of the optional managed-region change ledger: bounded, off by
	// default and interval-gated inside, so a run that enables it (selector
	// LUMEN_MACOS_VM_LEDGER) dates the small managed regions between explicit
	// triggers without another thread.
	Core::VirtualMemory::TickRecordedRegionLedger("tick", false);
#endif
}

namespace {
std::atomic_uint32_t g_guest_execution_backend_failure = 0;
}

void ResetGuestExecutionBackendFailure()
{
	g_guest_execution_backend_failure.store(0, std::memory_order_release);
}

bool RecordGuestExecutionBackendFailure(uint32_t code)
{
	if (code == 0) return false;
	uint32_t expected = 0;
	return g_guest_execution_backend_failure.compare_exchange_strong(expected, code,
	    std::memory_order_acq_rel, std::memory_order_acquire);
}

uint32_t GuestExecutionBackendFailure()
{
	return g_guest_execution_backend_failure.load(std::memory_order_acquire);
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
namespace {

std::atomic_bool g_guest_execution_stop_requested = false;
std::atomic_uint32_t g_guest_execution_active = 0;
thread_local uint32_t g_guest_execution_thread_activity = 0;

class GuestExecutionScope
{
public:
	GuestExecutionScope() { GuestExecutionActivityBegin(); }
	~GuestExecutionScope() { GuestExecutionActivityEnd(); }

	LUMEN_CLASS_NO_COPY(GuestExecutionScope);
};

} // namespace

void ResetGuestExecutionStop()
{
	ResetGuestExecutionBackendFailure();
	g_guest_execution_stop_requested.store(false, std::memory_order_release);
}

void RequestGuestExecutionStop()
{
	g_guest_execution_stop_requested.store(true, std::memory_order_release);
}

bool RequestGuestSessionExit()
{
	// Latch CPU cancellation before waking the presentation loop so both sides
	// observe the same fail-closed session-exit request.
	RequestGuestExecutionStop();
	SDL_Event event {};
	event.type = SDL_QUIT;
	return SDL_PushEvent(&event) > 0;
}

bool GuestExecutionStopRequested()
{
	return g_guest_execution_stop_requested.load(std::memory_order_acquire);
}

bool GuestExecutionIsIdle()
{
	return g_guest_execution_active.load(std::memory_order_acquire) == 0;
}

void GuestExecutionActivityBegin()
{
	g_guest_execution_active.fetch_add(1, std::memory_order_acq_rel);
	g_guest_execution_thread_activity++;
}

void GuestExecutionActivityEnd()
{
	if (g_guest_execution_thread_activity == 0)
	{
		return;
	}
	g_guest_execution_thread_activity--;
	g_guest_execution_active.fetch_sub(1, std::memory_order_acq_rel);
}

void GuestExecutionActivityEndAllForCurrentThread()
{
	const uint32_t count = g_guest_execution_thread_activity;
	g_guest_execution_thread_activity = 0;
	if (count > 0)
	{
		g_guest_execution_active.fetch_sub(count, std::memory_order_acq_rel);
	}
}
#endif

#pragma pack(1)

struct EntryParams
{
	int         argc;
	uint32_t    pad;
	const char* argv[33];
};

#pragma pack()

using atexit_func_t          = LUMEN_SYSV_ABI void (*)();
using entry_func_t           = LUMEN_SYSV_ABI void (*)(EntryParams* params, atexit_func_t atexit_func);
using module_ini_fini_func_t = LUMEN_SYSV_ABI int (*)(size_t args, const void* argp, module_func_t func);

enum class BindType
{
	Unknown,
	Local,
	Global,
	Weak
};

struct RelocationInfo
{
	bool       resolved   = false;
	BindType   bind       = BindType::Unknown;
	SymbolType type       = SymbolType::Unknown;
	uint64_t   value      = 0;
	uint64_t   vaddr      = 0;
	uint64_t   base_vaddr = 0;
	String     name;
	String     dbg_name;
	bool       bind_self = false;
};

// The structure will be passed via the stack
// since the size of an object is larger than 16 bytes
struct RelocateHandlerStack
{
	uint64_t stack[3];
};

static void LUMEN_SYSV_ABI RelocateHandler(RelocateHandlerStack s);

constexpr uint64_t SYSTEM_RESERVED  = 0x800000000u;
constexpr uint64_t CODE_BASE_INCR   = 0x010000000u;
constexpr uint64_t INVALID_OFFSET   = 0x040000000u;
constexpr uint64_t CODE_BASE_OFFSET = 0x100000000u;
constexpr uint64_t INVALID_MEMORY   = SYSTEM_RESERVED + INVALID_OFFSET;

constexpr size_t   XSAVE_BUFFER_SIZE = 2688;
constexpr uint64_t XSAVE_CHK_GUARD   = 0xDeadBeef5533CCAAu;

static uint64_t g_desired_base_addr = SYSTEM_RESERVED + CODE_BASE_OFFSET;
static uint64_t g_invalid_memory    = 0;

static Program* g_tls_main_program = nullptr;
alignas(64) static uint8_t g_tls_reg_save_area[XSAVE_BUFFER_SIZE + sizeof(XSAVE_CHK_GUARD)];
static uint8_t g_tls_spinlock = 0;

static LUMEN_MS_ABI uint8_t* TlsMainGetAddr();

#if defined(_WIN32) && defined(__x86_64__)
struct NativeTraceConfig
{
	uint64_t address = 0;
	uint64_t module_base = 0;
	uint64_t module_size = 0;
	uint64_t offset = 0;
	uint64_t occurrence = 1;
	uint64_t matches = 0;
	uint32_t trace_steps = 0;
	uint32_t watch_size = 0;
	const char* filter_register_name = nullptr;
	int      filter_register = -1;
	int      watch_register = -1;
	uint64_t filter_value = 0;
	uint64_t watch_delta = 0;
	uint8_t  original_byte = 0;
	bool     installed = false;
	bool     arm_watch = false;
};

static NativeTraceConfig g_native_trace;
thread_local bool g_native_trace_single_step = false;
thread_local uint32_t g_native_trace_remaining = 0;
thread_local uint64_t g_native_watch_address = 0;

static uint64_t env_u64(const char* name, uint64_t default_value)
{
	const char* value = std::getenv(name);
	if (value == nullptr || value[0] == '\0') return default_value;
	char* end = nullptr;
	const uint64_t parsed = std::strtoull(value, &end, 0);
	return end != value && *end == '\0' ? parsed : default_value;
}

static int native_register_index(const char* name)
{
	static constexpr const char* names[] = {
	    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
	    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
	};
	if (name == nullptr) return -1;
	for (int i = 0; i < 16; i++)
	{
		if (std::strcmp(name, names[i]) == 0) return i;
	}
	return -1;
}

static uint64_t context_register(const CONTEXT* context, int index)
{
	const uint64_t values[] = {
	    context->Rax, context->Rcx, context->Rdx, context->Rbx, context->Rsp, context->Rbp, context->Rsi, context->Rdi,
	    context->R8,  context->R9,  context->R10, context->R11, context->R12, context->R13, context->R14, context->R15,
	};
	return index >= 0 && index < 16 ? values[index] : 0;
}

static void native_write_code_byte(uint64_t address, uint8_t value)
{
	DWORD old_protect = 0;
	if (VirtualProtect(reinterpret_cast<void*>(address), 1, PAGE_EXECUTE_READWRITE, &old_protect) == FALSE) return;
	*reinterpret_cast<volatile uint8_t*>(address) = value;
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(address), 1);
	DWORD ignored = 0;
	VirtualProtect(reinterpret_cast<void*>(address), 1, old_protect, &ignored);
}

static void print_native_trace_context(const char* event, const CONTEXT* context, bool frame_chain)
{
	auto* program = RuntimeLinker::Instance()->FindProgramByAddr(context->Rip);
	std::fprintf(stderr,
	             "%s: thread=%lu module=%s module_base=0x%016" PRIx64 " module_size=0x%016" PRIx64
	             " offset=0x%016" PRIx64 " rip=0x%016" PRIx64 "\n",
	             event, static_cast<unsigned long>(GetCurrentThreadId()),
	             program == nullptr ? "?" : program->file_name.FilenameWithoutDirectory().C_Str(),
	             program == nullptr ? 0 : program->base_vaddr, program == nullptr ? 0 : program->base_size,
	             program == nullptr ? 0 : context->Rip - program->base_vaddr, context->Rip);
	std::fprintf(stderr,
	             "  rax=%016" PRIx64 " rbx=%016" PRIx64 " rcx=%016" PRIx64 " rdx=%016" PRIx64
	             " rsi=%016" PRIx64 " rdi=%016" PRIx64 " rsp=%016" PRIx64 " rbp=%016" PRIx64 "\n",
	             context->Rax, context->Rbx, context->Rcx, context->Rdx, context->Rsi, context->Rdi, context->Rsp, context->Rbp);
	std::fprintf(stderr,
	             "  r8 =%016" PRIx64 " r9 =%016" PRIx64 " r10=%016" PRIx64 " r11=%016" PRIx64
	             " r12=%016" PRIx64 " r13=%016" PRIx64 " r14=%016" PRIx64 " r15=%016" PRIx64 "\n",
	             context->R8, context->R9, context->R10, context->R11, context->R12, context->R13, context->R14, context->R15);
	if (frame_chain) RuntimeLinker::Instance()->StackTrace(context->Rbp);
}

static LONG WINAPI native_trace_exception_handler(PEXCEPTION_POINTERS exception)
{
	auto* record = exception->ExceptionRecord;
	auto* context = exception->ContextRecord;
	if (record->ExceptionCode == EXCEPTION_BREAKPOINT &&
	    reinterpret_cast<uint64_t>(record->ExceptionAddress) == g_native_trace.address)
	{
		native_write_code_byte(g_native_trace.address, g_native_trace.original_byte);
		context->Rip = g_native_trace.address;
		const bool filter_matches = g_native_trace.filter_register < 0 ||
		                            context_register(context, g_native_trace.filter_register) == g_native_trace.filter_value;
		const uint64_t match = filter_matches ? ++g_native_trace.matches : g_native_trace.matches;
		const bool selected = filter_matches && match == g_native_trace.occurrence;
		if (selected)
		{
			std::fprintf(stderr,
			             "Guest native module trace triggered: module_base=0x%016" PRIx64
			             " module_size=0x%016" PRIx64 " offset=0x%016" PRIx64 " rip=0x%016" PRIx64
			             " occurrence=%" PRIu64 " filter_register=%s filter_value=0x%016" PRIx64 " thread=%lu\n",
			             g_native_trace.module_base, g_native_trace.module_size, g_native_trace.offset,
			             g_native_trace.address, match,
			             g_native_trace.filter_register_name == nullptr ? "-" : g_native_trace.filter_register_name,
			             g_native_trace.filter_value,
			             static_cast<unsigned long>(GetCurrentThreadId()));
			print_native_trace_context("Guest native trace context", context, true);
			g_native_trace_remaining = g_native_trace.trace_steps;
			if (g_native_trace.arm_watch)
			{
				const uint64_t register_value = context_register(context, g_native_trace.watch_register);
				g_native_watch_address = register_value + g_native_trace.watch_delta;
				uint64_t initial_value = 0;
				std::memcpy(&initial_value, reinterpret_cast<const void*>(g_native_watch_address), g_native_trace.watch_size);
				context->Dr0 = g_native_watch_address;
				context->Dr7 = (context->Dr7 & ~((3ULL << 16) | (3ULL << 18))) | 1ULL | (1ULL << 16) |
				               ((g_native_trace.watch_size == 4 ? 3ULL : g_native_trace.watch_size == 2 ? 1ULL : 0ULL) << 18);
				std::fprintf(stderr,
				             "Guest native watchpoint armed: register_value=0x%016" PRIx64
				             " delta=0x%016" PRIx64 " address=0x%016" PRIx64 " size=%" PRIu32
				             " initial_value=0x%016" PRIx64 " thread=%lu\n",
				             register_value, g_native_trace.watch_delta, g_native_watch_address, g_native_trace.watch_size,
				             initial_value, static_cast<unsigned long>(GetCurrentThreadId()));
			}
		}
		g_native_trace_single_step = !selected || g_native_trace_remaining != 0;
		if (g_native_trace_single_step) context->EFlags |= 0x100;
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	if (record->ExceptionCode == EXCEPTION_SINGLE_STEP)
	{
		if ((context->Dr6 & 1) != 0 && g_native_watch_address != 0)
		{
			context->Dr6 &= ~1ULL;
			uint64_t value = 0;
			std::memcpy(&value, reinterpret_cast<const void*>(g_native_watch_address), g_native_trace.watch_size);
			std::fprintf(stderr,
			             "Watched native guest write: address=0x%016" PRIx64 " size=%" PRIu32
			             " value=0x%016" PRIx64 " thread=%lu\n",
			             g_native_watch_address, g_native_trace.watch_size, value,
			             static_cast<unsigned long>(GetCurrentThreadId()));
			print_native_trace_context("Watched native guest write context", context, true);
		}
		if (g_native_trace_single_step)
		{
			g_native_trace_single_step = false;
			if (g_native_trace.matches != g_native_trace.occurrence)
			{
				native_write_code_byte(g_native_trace.address, 0xcc);
			}
			if (g_native_trace_remaining != 0)
			{
				print_native_trace_context("Guest native trace step", context, false);
				g_native_trace_remaining--;
				if (g_native_trace_remaining != 0)
				{
					g_native_trace_single_step = true;
					context->EFlags |= 0x100;
				}
			}
		}
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

static void install_native_module_trace(Program* program)
{
	if (g_native_trace.installed || program == nullptr) return;
	const char* trace_module = std::getenv("LUMEN_X64_TRACE_MODULE");
	const char* arm_module = std::getenv("LUMEN_X64_WATCH_ARM_MODULE");
	const bool arm = arm_module != nullptr && arm_module[0] != '\0';
	const char* module = arm ? arm_module : trace_module;
	if (module == nullptr || module[0] == '\0' ||
	    !program->file_name.FilenameWithoutDirectory().ToLower().Equal(String::FromUtf8(module).ToLower()))
	{
		return;
	}
	const uint64_t offset = env_u64(arm ? "LUMEN_X64_WATCH_ARM_OFFSET" : "LUMEN_X64_TRACE_OFFSET", UINT64_MAX);
	if (offset == UINT64_MAX || offset >= program->base_size) return;
	g_native_trace.module_base = program->base_vaddr;
	g_native_trace.module_size = program->base_size;
	g_native_trace.offset = offset;
	g_native_trace.address = program->base_vaddr + offset;
	g_native_trace.occurrence = env_u64(arm ? "LUMEN_X64_WATCH_ARM_OCCURRENCE" : "LUMEN_X64_TRACE_OCCURRENCE", 1);
	g_native_trace.trace_steps = static_cast<uint32_t>(env_u64("LUMEN_X64_WATCH_TRACE_STEPS", 0));
	g_native_trace.arm_watch = arm;
	const char* filter_register = std::getenv("LUMEN_X64_TRACE_FILTER_REGISTER");
	const char* filter_value = std::getenv("LUMEN_X64_TRACE_FILTER_VALUE");
	g_native_trace.filter_register_name = filter_register;
	g_native_trace.filter_register =
	    filter_register != nullptr && filter_register[0] != '\0' ? native_register_index(filter_register) : -1;
	g_native_trace.filter_value = env_u64("LUMEN_X64_TRACE_FILTER_VALUE", 0);
	g_native_trace.watch_register = native_register_index(std::getenv("LUMEN_X64_WATCH_ARM_REGISTER"));
	g_native_trace.watch_delta = env_u64("LUMEN_X64_WATCH_ARM_DELTA", 0);
	g_native_trace.watch_size = static_cast<uint32_t>(env_u64("LUMEN_X64_WATCH_ARM_SIZE", 4));
	if (g_native_trace.occurrence == 0 ||
	    ((filter_register == nullptr || filter_register[0] == '\0') != (filter_value == nullptr || filter_value[0] == '\0')) ||
	    (filter_register != nullptr && filter_register[0] != '\0' && g_native_trace.filter_register < 0) ||
	    (arm && (g_native_trace.watch_register < 0 ||
	    (g_native_trace.watch_size != 1 && g_native_trace.watch_size != 2 && g_native_trace.watch_size != 4)))) return;
	g_native_trace.original_byte = *reinterpret_cast<const uint8_t*>(g_native_trace.address);
	if (AddVectoredExceptionHandler(1, native_trace_exception_handler) == nullptr) return;
	native_write_code_byte(g_native_trace.address, 0xcc);
	g_native_trace.installed = true;
	std::fprintf(stderr,
	             "Guest native module trace installed: module=%s base=0x%016" PRIx64 " size=0x%016" PRIx64
	             " entry_offset=0x%016" PRIx64 " offset=0x%016" PRIx64 " rip=0x%016" PRIx64
	             " original_byte=0x%02x occurrence=%" PRIu64 " filter_register=%s filter_value=0x%016" PRIx64 "\n",
	             module, program->base_vaddr, program->base_size, program->elf->GetEntry(), offset,
	             g_native_trace.address, g_native_trace.original_byte, g_native_trace.occurrence,
	             g_native_trace.filter_register_name == nullptr ? "-" : g_native_trace.filter_register_name,
	             g_native_trace.filter_value);
}
#endif

#if defined(__aarch64__)
#if defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
struct CachedGuestRegion
{
	uint64_t start       = 0;
	uint64_t end         = 0;
	uint32_t permissions = 0;
};

struct GuestRegionCacheState
{
	CachedGuestRegion regions[16] {};
	uint32_t          next                     = 0;
	uint32_t          mru                      = 0;
	uint32_t          cross_region_trace_count = 0;
	uint32_t          previous_mru             = 0;
	uint64_t          mutation_generation      = 0;
};

static_assert(sizeof(GuestRegionCacheState) == 408);

thread_local GuestRegionCacheState g_guest_region_cache {};

#if defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)
thread_local uint64_t g_guest_memory_pipeline_watch_begin = 0;
thread_local uint64_t g_guest_memory_pipeline_watch_end = 0;
thread_local size_t g_guest_memory_pipeline_export_count = 0;
thread_local size_t g_guest_memory_pipeline_watched_translate_count = 0;
thread_local bool g_guest_memory_pipeline_non_reentrant_observed = false;
#endif

static void synchronize_guest_region_cache(GuestRegionCacheState& cache)
{
	const uint64_t generation = Core::VirtualMemory::GetMutationGeneration();
	if (cache.mutation_generation == generation) return;

	const uint32_t cross_region_trace_count = cache.cross_region_trace_count;
	cache                                   = {};
	cache.cross_region_trace_count          = cross_region_trace_count;
	cache.mutation_generation               = generation;
}

static void mark_guest_region_mru(GuestRegionCacheState& cache, uint32_t index)
{
	if (index == cache.mru) return;
	cache.previous_mru = cache.mru;
	cache.mru          = index;
}

template <typename Predicate>
static const CachedGuestRegion* find_cached_guest_region(GuestRegionCacheState& cache, Predicate&& matches)
{
	synchronize_guest_region_cache(cache);
	const uint32_t mru = cache.mru;
	if (matches(cache.regions[mru])) return &cache.regions[mru];

	for (uint32_t index = 0; index < static_cast<uint32_t>(std::size(cache.regions)); index++)
	{
		if (index == mru) continue;
		if (matches(cache.regions[index]))
		{
			mark_guest_region_mru(cache, index);
			return &cache.regions[index];
		}
	}
	return nullptr;
}

static bool cross_region_trace_enabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_X64_REGION_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	return enabled;
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
static bool query_android_process_region(uint64_t address, uint64_t* start,
	                                     uint64_t* end, uint32_t* permissions)
{
	if (address == 0 || start == nullptr || end == nullptr ||
	    permissions == nullptr)
	{
		return false;
	}

	auto* maps = std::fopen("/proc/self/maps", "r");
	if (maps == nullptr) return false;

	bool found = false;
	char line[1024] {};
	while (std::fgets(line, sizeof(line), maps) != nullptr)
	{
		uint64_t region_start = 0;
		uint64_t region_end = 0;
		char access[5] {};
		if (std::sscanf(line, "%" SCNx64 "-%" SCNx64 " %4s",
		                &region_start, &region_end, access) != 3 ||
		    region_start >= region_end || address < region_start ||
		    address >= region_end)
		{
			continue;
		}

		uint32_t region_permissions = 0;
		if (access[0] == 'r')
		{
			region_permissions |=
			    static_cast<uint32_t>(Cpu::X64MemoryPermission::Read);
		}
		if (access[1] == 'w')
		{
			region_permissions |=
			    static_cast<uint32_t>(Cpu::X64MemoryPermission::Write);
		}
		*start = region_start;
		*end = region_end;
		*permissions = region_permissions;
		found = true;
		break;
	}
	std::fclose(maps);
	return found;
}
#endif

static bool query_guest_region(uint64_t address, bool write, uint64_t* start, uint64_t* end)
{
	if (start == nullptr || end == nullptr) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	// Bionic's allocator may return top-byte-tagged pointers. Kernel mapping
	// metadata is untagged, while the pointer passed back to memcpy must retain
	// its tag. Keep cache intervals in the caller's tagged address space.
	constexpr uint64_t android_address_mask = UINT64_C(0x00ffffffffffffff);
	const uint64_t address_tag = address & ~android_address_mask;
	const uint64_t query_address = address & android_address_mask;
	const auto retag_region = [address_tag](
	                              uint64_t region_start, uint64_t region_end,
	                              uint64_t* tagged_start,
	                              uint64_t* tagged_end)
	{
		if ((region_start & ~android_address_mask) != 0 ||
		    (region_end & ~android_address_mask) != 0 ||
		    region_start >= region_end)
		{
			return false;
		}
		*tagged_start = address_tag | region_start;
		*tagged_end = address_tag | region_end;
		return true;
	};
#else
	const uint64_t query_address = address;
#endif
	const uint32_t required = static_cast<uint32_t>(
	    write ? Cpu::X64MemoryPermission::Write : Cpu::X64MemoryPermission::Read);
	auto& cache = g_guest_region_cache;
	synchronize_guest_region_cache(cache);
	if (const auto* cached = find_cached_guest_region(cache, [address](const CachedGuestRegion& region)
	    { return address >= region.start && address < region.end; });
	    cached != nullptr)
	{
		*start = cached->start;
		*end = cached->end;
		return (cached->permissions & required) != 0;
	}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	// The kernel VMA is already a permission-uniform interval and can cover a
	// multi-gigabyte guest heap. Prefer it over walking the per-page tracking map
	// on every small cache window; Core VM mutations invalidate this TLS cache.
	uint64_t process_start = 0;
	uint64_t process_end = 0;
	uint32_t process_permissions = 0;
	if (query_android_process_region(query_address, &process_start,
	                                 &process_end, &process_permissions) &&
	    retag_region(process_start, process_end, start, end))
	{
		const uint32_t insert = cache.next;
		cache.regions[insert] = {*start, *end, process_permissions};
		mark_guest_region_mru(cache, insert);
		cache.next =
		    (insert + 1) % static_cast<uint32_t>(std::size(cache.regions));
		return (process_permissions & required) != 0;
	}
#endif

	Core::VirtualMemory::ManagedRegion managed {};
	const auto managed_state = Core::VirtualMemory::QueryManagedRegion(query_address, &managed);
	if (managed_state != Core::VirtualMemory::ManagedRegionState::Unmanaged)
	{
		uint32_t permissions = 0;
		if (managed_state == Core::VirtualMemory::ManagedRegionState::Visible)
		{
			const uint32_t mode = static_cast<uint32_t>(managed.mode);
			if (mode != static_cast<uint32_t>(Core::VirtualMemory::Mode::NoAccess))
			{
				permissions |= static_cast<uint32_t>(Cpu::X64MemoryPermission::Read);
			}
			if ((mode & static_cast<uint32_t>(Core::VirtualMemory::Mode::Write)) != 0)
			{
				permissions |= static_cast<uint32_t>(Cpu::X64MemoryPermission::Write);
			}
		}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		if (!retag_region(managed.begin, managed.end, start, end)) return false;
#else
		*start = managed.begin;
		*end = managed.end;
#endif
		const uint32_t insert = cache.next;
		cache.regions[insert] = {*start, *end, permissions};
		mark_guest_region_mru(cache, insert);
		cache.next =
		    (insert + 1) % static_cast<uint32_t>(std::size(cache.regions));
		return (permissions & required) != 0;
	}

	uint32_t permissions = 0;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	return false;
#else
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	mach_vm_address_t region_address = address;
	mach_vm_size_t    region_size    = 0;
#else
	vm_address_t region_address = static_cast<vm_address_t>(address);
	vm_size_t    region_size    = 0;
#endif
	vm_region_basic_info_data_64_t info {};
	mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t object_name = MACH_PORT_NULL;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	const kern_return_t result = mach_vm_region(mach_task_self(), &region_address, &region_size, VM_REGION_BASIC_INFO_64,
	                                             reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);
#else
	const kern_return_t result = vm_region_64(mach_task_self(), &region_address, &region_size, VM_REGION_BASIC_INFO_64,
	                                          reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);
#endif
	if (object_name != MACH_PORT_NULL)
	{
		mach_port_deallocate(mach_task_self(), object_name);
	}
	if (result != KERN_SUCCESS || region_address > address || region_size > UINT64_MAX - region_address ||
	    address >= region_address + region_size || (info.protection & required) == 0)
	{
		return false;
	}
	*start = region_address;
	*end = region_address + region_size;
	if ((info.protection & VM_PROT_READ) != 0)
	{
		permissions |= static_cast<uint32_t>(Cpu::X64MemoryPermission::Read);
	}
	if ((info.protection & VM_PROT_WRITE) != 0)
	{
		permissions |= static_cast<uint32_t>(Cpu::X64MemoryPermission::Write);
	}
#endif
	const uint32_t insert = cache.next;
	cache.regions[insert] = {*start, *end, permissions};
	mark_guest_region_mru(cache, insert);
	cache.next            = (insert + 1) % static_cast<uint32_t>(std::size(cache.regions));
	return true;
}

static uint64_t translate_guest_address_with_cache(GuestRegionCacheState& cache, uint64_t address, size_t size,
                                                   bool write)
{
	if (size == 0 || address > UINT64_MAX - size) return 0;
	const uint64_t end = address + size;
	const uint32_t required = static_cast<uint32_t>(
	    write ? Cpu::X64MemoryPermission::Write : Cpu::X64MemoryPermission::Read);
	if (find_cached_guest_region(cache, [address, end, required](const CachedGuestRegion& region)
	    { return address >= region.start && end <= region.end && (region.permissions & required) != 0; }) != nullptr)
	{
		return address;
	}
	uint64_t first_start = 0;
	uint64_t first_end = 0;
	if (!query_guest_region(address, write, &first_start, &first_end) ||
	    first_start > address || first_end <= address)
	{
		return 0;
	}
	if (end <= first_end) return address;
	if (!Cpu::X64ValidateAddressRange(address, size, write, query_guest_region)) return 0;
	if (cross_region_trace_enabled() && cache.cross_region_trace_count++ < 64)
	{
		std::fprintf(stderr,
		             "Guest cross-region access: %s address=0x%016" PRIx64 " size=0x%zx"
		             " first=[0x%016" PRIx64 ",0x%016" PRIx64 ") end=0x%016" PRIx64 "\n",
		             write ? "write" : "read", address, size, first_start, first_end, end);
	}
	return address;
}

uint64_t translate_guest_address(uint64_t address, size_t size, bool write)
{
	return translate_guest_address_with_cache(g_guest_region_cache, address, size, write);
}

#if !defined(LUMEN_FEXCORE_RUNTIME) || defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)
static uint64_t translate_guest_address_with_context(void* context, uint64_t address, size_t size, bool write)
{
	// Certification boundary for the explicit NonReentrant capability below:
	// this callback only traverses the TLS region cache, VM generation/region
	// queries, native mapping metadata and bounded range validation. It never
	// dispatches an HLE callback or enters X64Interpreter::Run.
	if (context == nullptr) return 0;
#if defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)
	g_guest_memory_pipeline_non_reentrant_observed =
	    g_guest_memory_pipeline_non_reentrant_observed ||
	    Cpu::X64ActiveAddressTranslatorReentryCapabilityForTest() ==
	        Cpu::X64AddressTranslatorReentryCapability::NonReentrant;
	if (address >= g_guest_memory_pipeline_watch_begin &&
	    address < g_guest_memory_pipeline_watch_end)
	{
		g_guest_memory_pipeline_watched_translate_count++;
	}
#endif
	return translate_guest_address_with_cache(*static_cast<GuestRegionCacheState*>(context), address, size, write);
}

static size_t export_guest_memory_regions_with_context(
	void* context, Cpu::X64MemoryTranslationRegion* regions, size_t capacity)
{
	if (context == nullptr || regions == nullptr || capacity == 0) return 0;
#if defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)
	g_guest_memory_pipeline_export_count++;
#endif
	auto& cache = *static_cast<GuestRegionCacheState*>(context);
	// This is also the entry barrier for stale translations: Protect/Free bump
	// the VM generation, so no host interval copied into native entry state can
	// survive a mapping mutation observed before this call.
	synchronize_guest_region_cache(cache);

	size_t copied = 0;
	const auto copy_region = [&](uint32_t index)
	{
		if (copied >= capacity) return;
		const auto& cached = cache.regions[index];
		if (cached.start >= cached.end || cached.permissions == 0) return;

		regions[copied++] = {cached.start, cached.end, cached.start, cached.permissions};
	};

	copy_region(cache.mru);
	if (cache.previous_mru != cache.mru) copy_region(cache.previous_mru);
	for (uint32_t offset = 1;
	     offset < static_cast<uint32_t>(std::size(cache.regions)) && copied < capacity;
	     offset++)
	{
		const uint32_t index = static_cast<uint32_t>((cache.mru + offset) % std::size(cache.regions));
		if (index == cache.previous_mru) continue;
		copy_region(index);
	}
	return copied;
}
#endif

#if defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)
size_t ExportGuestMemoryRegionsForTest(Cpu::X64MemoryTranslationRegion* regions, size_t capacity)
{
	return export_guest_memory_regions_with_context(&g_guest_region_cache, regions, capacity);
}

Cpu::X64RunResult RunGuestMemoryPipelineForTest(Cpu::X64State* state, size_t max_steps)
{
	return Cpu::X64Interpreter(nullptr, nullptr, nullptr, nullptr, false,
	                           translate_guest_address_with_context, &g_guest_region_cache,
	                           export_guest_memory_regions_with_context,
	                           {Cpu::X64AddressTranslatorReentryCapability::NonReentrant})
	    .Run(state, max_steps);
}

void ConfigureGuestMemoryPipelineWatchForTest(uint64_t begin, uint64_t end)
{
	g_guest_memory_pipeline_watch_begin = begin;
	g_guest_memory_pipeline_watch_end = end;
}

void ResetGuestMemoryPipelineCountersForTest()
{
	g_guest_memory_pipeline_export_count = 0;
	g_guest_memory_pipeline_watched_translate_count = 0;
	g_guest_memory_pipeline_non_reentrant_observed = false;
}

size_t GuestMemoryPipelineExportCountForTest()
{
	return g_guest_memory_pipeline_export_count;
}

size_t GuestMemoryPipelineWatchedTranslateCountForTest()
{
	return g_guest_memory_pipeline_watched_translate_count;
}

bool GuestMemoryPipelineNonReentrantCapabilityObservedForTest()
{
	return g_guest_memory_pipeline_non_reentrant_observed;
}
#endif
#endif

[[maybe_unused]] static uint64_t translate_checked_guest_memory(
	uint64_t address, size_t size, bool write)
{
	if (address == 0 || size == 0 || address > UINT64_MAX - size)
	{
		return 0;
	}
#if defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	return translate_guest_address(address, size, write);
#else
	(void)write;
	return address;
#endif
}

bool ValidateGuestMemory(uint64_t address, size_t size, bool write)
{
#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	Core::VirtualMemory::GuestDataLease lease;
	return Core::VirtualMemory::TryAcquireGuestDataLease(address, size,
	    write ? Core::VirtualMemory::Mode::Write : Core::VirtualMemory::Mode::Read, &lease);
#else
	return translate_checked_guest_memory(address, size, write) != 0;
#endif
}

bool ReadGuestMemory(uint64_t address, void* destination, size_t size)
{
#if defined(LUMEN_FEXCORE_RUNTIME) && LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	return Core::VirtualMemory::TryReadGuestData(address, destination, size);
#else
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__APPLE__)
	Core::VirtualMemory::GuestDataLease lease;
	if (destination == nullptr || !Core::VirtualMemory::TryAcquireGuestDataLease(
	        address, size, Core::VirtualMemory::Mode::Read, &lease)) return false;
	const uint64_t translated = lease.HostBegin();
#else
	const uint64_t translated =
	    translate_checked_guest_memory(address, size, false);
	if (destination == nullptr || translated == 0)
	{
		return false;
	}
#endif
	std::memcpy(destination, reinterpret_cast<const void*>(translated), size);
	return true;
#endif
}

bool WriteGuestMemory(uint64_t address, const void* source, size_t size)
{
#if defined(LUMEN_FEXCORE_RUNTIME) && LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	return Core::VirtualMemory::TryWriteGuestData(address, source, size);
#else
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__APPLE__)
	Core::VirtualMemory::GuestDataLease lease;
	if (source == nullptr || !Core::VirtualMemory::TryAcquireGuestDataLease(
	        address, size, Core::VirtualMemory::Mode::Write, &lease)) return false;
	const uint64_t translated = lease.HostBegin();
#else
	const uint64_t translated =
	    translate_checked_guest_memory(address, size, true);
	if (source == nullptr || translated == 0)
	{
		return false;
	}
#endif
	std::memcpy(reinterpret_cast<void*>(translated), source, size);
	return true;
#endif
}

static bool read_diagnostic_memory(uint64_t address, void* destination, size_t size)
{
	if (address == 0 || destination == nullptr || size == 0 || address > UINT64_MAX - size) return false;

#if defined(__APPLE__)
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	mach_vm_size_t copied = 0;
	const kern_return_t result =
	    mach_vm_read_overwrite(mach_task_self(), address, size, reinterpret_cast<mach_vm_address_t>(destination), &copied);
#else
	vm_size_t copied = 0;
	const kern_return_t result = vm_read_overwrite(mach_task_self(), static_cast<vm_address_t>(address), static_cast<vm_size_t>(size),
	                                                reinterpret_cast<vm_address_t>(destination), &copied);
#endif
	return result == KERN_SUCCESS && copied == size;
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	// Android guest addresses are mapped directly. Fault diagnostics must not
	// enter VM metadata locks or fault again while reading a stale raw pointer.
	iovec local {destination, size};
	iovec remote {reinterpret_cast<void*>(address), size};
	const auto copied = ::syscall(
	    SYS_process_vm_readv, ::getpid(), &local, 1UL, &remote, 1UL, 0UL);
	return copied == static_cast<ssize_t>(size);
#else
	// Non-Darwin ARM64 currently uses directly mapped guest addresses and has
	// no address translator. Preserve that contract while still rejecting
	// null and overflowing diagnostic accesses.
	std::memcpy(destination, reinterpret_cast<const void*>(address), size);
	return true;
#endif
}

#if defined(__APPLE__)
static bool host_wait_trace_enabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_X64_HOST_WAIT_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	return enabled;
}

static const char* host_call_symbol(uint64_t target, bool enabled = true)
{
	if (!enabled) return nullptr;
	// HLE targets live in the process's native runtime images. Their dladdr
	// names remain valid for the process lifetime, including asynchronous trace
	// entries. Hot diagnostic boundaries need neither dladdr nor a cache lock.
	static HostCallSymbolCache<4096> cache;
	return cache.Lookup(true, target, [](uint64_t address) -> const char*
	{
		Dl_info info {};
		return dladdr(reinterpret_cast<const void*>(address), &info) != 0 ? info.dli_sname : nullptr;
	});
}

#if defined(LUMEN_FEXCORE_RUNTIME)
// Bounded native HLE observation, using only registered host targets and their
// existing native symbol names. No guest pointer is followed by this trace.
struct HostStorageDialogTraceEntry
{
	uint64_t target = 0;
	uint64_t calls = 0;
	uint64_t changes = 0;
	uint64_t last_result = 0;
	bool has_result = false;
};

static std::array<HostStorageDialogTraceEntry, 128> g_storage_dialog_trace;
static std::mutex g_storage_dialog_trace_mutex;
static uint64_t g_storage_dialog_call_id = 0;
static uint32_t g_storage_dialog_records = 0;
static constexpr uint32_t STORAGE_DIALOG_RECORD_LIMIT = 4096;

struct HostStorageDialogTraceCall
{
	HostStorageDialogTraceEntry* entry = nullptr;
	const char* symbol = nullptr;
	uint64_t target = 0;
	uint64_t id = 0;
	uint64_t occurrence = 0;
	int thread = 0;
	bool entered = false;
	std::chrono::steady_clock::time_point start {};
};

struct HostStorageDialogTraceErrno
{
	int saved = errno;
	~HostStorageDialogTraceErrno() { errno = saved; }
};

static bool storage_dialog_trace_sample(uint64_t occurrence)
{
	return occurrence <= 4 || (occurrence & (occurrence - 1)) == 0;
}

static void print_storage_dialog_trace(const HostStorageDialogTraceCall& call, const char* phase,
	                                    uint64_t result, bool result_valid, bool handled, uint32_t fault_size)
{
	const auto now = std::chrono::steady_clock::now();
	const auto monotonic_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
	const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - call.start).count();
	std::fprintf(stderr,
	    "[HOST_STORAGE_DIALOG_TRACE] monotonicUs=%lld call=%" PRIu64 " occurrence=%" PRIu64
	    " thread=%d target=0x%016" PRIx64 " symbol=%s phase=%s result=0x%016" PRIx64
	    " result_valid=%u handled=%u memory_fault_bytes=%u elapsedUs=%lld entry_recorded=%u\n",
	    static_cast<long long>(monotonic_us), call.id, call.occurrence, call.thread, call.target,
	    call.symbol, phase, result, result_valid ? 1U : 0U, handled ? 1U : 0U, fault_size,
	    static_cast<long long>(elapsed_us), call.entered ? 1U : 0U);
	std::fflush(stderr);
}

static HostStorageDialogTraceCall begin_storage_dialog_trace(uint64_t target, const HostCallDispatchMetadata& dispatch)
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_X64_HOST_STORAGE_DIALOG_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	if (!enabled || dispatch.classification != HostCallRegistryClassification::Host) return {};
	const HostStorageDialogTraceErrno preserve_errno;
	const char* symbol = host_call_symbol(target);
	if ((std::strstr(symbol, "SaveData") == nullptr && std::strstr(symbol, "Dialog") == nullptr &&
	     std::strstr(symbol, "FileSystem") == nullptr) || std::strstr(symbol, "ImeDialog") != nullptr) return {};
	HostStorageDialogTraceCall call {};
	{
		std::lock_guard lock(g_storage_dialog_trace_mutex);
		if (g_storage_dialog_records >= STORAGE_DIALOG_RECORD_LIMIT || g_storage_dialog_call_id == UINT64_MAX) return {};
		for (auto& entry: g_storage_dialog_trace)
		{
			if (entry.target != target && entry.target != 0) continue;
			if (entry.calls == UINT64_MAX) return {};
			entry.target = target;
			call.entry = &entry;
			call.occurrence = ++entry.calls;
			break;
		}
		if (call.entry == nullptr) return {};
		call.id = ++g_storage_dialog_call_id;
		call.entered = storage_dialog_trace_sample(call.occurrence) &&
		    g_storage_dialog_records + 2 <= STORAGE_DIALOG_RECORD_LIMIT;
		// Reserve a matching exit before emitting an entry, even if other threads
		// exhaust the budget while this host call is in progress.
		if (call.entered) g_storage_dialog_records += 2;
	}
	call.target = target;
	call.symbol = symbol;
	call.thread = Core::Thread::GetThreadIdUnique();
	call.start = std::chrono::steady_clock::now();
	if (call.entered) print_storage_dialog_trace(call, "enter", 0, false, false, 0);
	return call;
}

static void end_storage_dialog_trace(const HostStorageDialogTraceCall& call, const HostCallDispatchMetadata& dispatch,
	                                  const Cpu::X64State& state, bool handled)
{
	if (call.entry == nullptr) return;
	const HostStorageDialogTraceErrno preserve_errno;
	const bool result_valid = handled && state.memory_fault_size == 0 &&
	    (dispatch.scalar == nullptr || dispatch.scalar->result == HostCallScalarType::Gp);
	bool emit = call.entered;
	{
		std::lock_guard lock(g_storage_dialog_trace_mutex);
		auto& entry = *call.entry;
		if (result_valid && (!entry.has_result || entry.last_result != state.gpr[0]))
		{
			entry.has_result = true;
			entry.last_result = state.gpr[0];
			if (entry.changes != UINT64_MAX) ++entry.changes;
			// Result transitions have an independent first-four/powers-of-two
			// sample, so hot alternating polls cannot consume the global budget.
			if (!emit && storage_dialog_trace_sample(entry.changes) &&
			    g_storage_dialog_records < STORAGE_DIALOG_RECORD_LIMIT)
			{
				++g_storage_dialog_records;
				emit = true;
			}
		}
	}
	if (emit) print_storage_dialog_trace(call, "exit", state.gpr[0], result_valid, handled, state.memory_fault_size);
}

// Optional read-only observation of one caller-selected 512-byte interval.
// This samples HLE boundaries, not individual writes: concurrent guest threads
// may change the interval between observations, so target is context, not blame.
static void trace_memory_watch(const char* phase, uint64_t target, const Cpu::X64State& state)
{
	static const uint64_t address = []() -> uint64_t {
		const char* text = std::getenv("LUMEN_X64_MEMORY_WATCH_ADDRESS");
		if (text == nullptr || text[0] != '0' || text[1] != 'x') return 0;
		uint64_t value = 0;
		unsigned digits = 0;
		for (const char* p = text + 2; *p != '\0'; ++p)
		{
			unsigned digit = *p >= '0' && *p <= '9' ? static_cast<unsigned>(*p - '0') :
			                 *p >= 'a' && *p <= 'f' ? static_cast<unsigned>(*p - 'a' + 10) :
			                 *p >= 'A' && *p <= 'F' ? static_cast<unsigned>(*p - 'A' + 10) : 16;
			if (++digits > 16 || digit > 15) return 0;
			value = (value << 4) | digit;
		}
		return digits != 0 && value <= UINT64_MAX - 512 ? value : 0;
	}();
	if (address == 0) return;
	static std::atomic<bool> exhausted {false};
	if (exhausted.load(std::memory_order_relaxed)) return;
	static std::mutex mutex;
	static std::array<unsigned char, 512> previous {};
	static bool observed = false;
	static bool was_readable = false;
	static uint32_t records = 0;
	static uint32_t samples = 0;
	std::unique_lock lock(mutex);
	if (records >= 4096 || samples >= 64000000)
	{
		if (exhausted.exchange(true, std::memory_order_relaxed)) return;
		const uint32_t final_records = records;
		const uint32_t final_samples = samples;
		lock.unlock();
		std::fprintf(stderr, "[MEMORY_WATCH_EXHAUSTED] records=%u samples=%u\n", final_records, final_samples);
		return;
	}
	++samples;
	std::array<unsigned char, 512> bytes {};
	const bool readable = read_diagnostic_memory(address, bytes.data(), bytes.size());
	if (observed && readable == was_readable && (!readable || bytes == previous)) return;
	observed = true;
	was_readable = readable;
	previous = bytes;
	const uint32_t index = records++;
	const uint32_t sample = samples;
	const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count();
	lock.unlock();
	std::array<char, 1025> hex_bytes {};
	constexpr char hex[] = "0123456789abcdef";
	if (readable) for (size_t i = 0; i < bytes.size(); ++i)
	{
		hex_bytes[2 * i] = hex[bytes[i] >> 4];
		hex_bytes[2 * i + 1] = hex[bytes[i] & 15];
	}
	std::fprintf(stderr,
	    "[MEMORY_WATCH_TRACE] index=%u sample=%u monotonicUs=%lld thread=%d phase=%s "
	    "address=0x%016" PRIx64 " readable=%u target=0x%016" PRIx64 " symbol=%s "
	    "guestRsp=0x%016" PRIx64 " args=0x%016" PRIx64 ",0x%016" PRIx64 ",0x%016" PRIx64 " bytes=%s\n",
	    index, sample, static_cast<long long>(now_us), Core::Thread::GetThreadIdUnique(), phase,
	    address, readable ? 1U : 0U, target, host_call_symbol(target), state.gpr[4],
	    state.gpr[7], state.gpr[6], state.gpr[2], hex_bytes.data());
}

static void trace_guest_console(uint64_t target, uint64_t guest_return, const Cpu::X64State& state)
{
	if (target != reinterpret_cast<uint64_t>(&Libs::LibKernel::FileSystem::KernelWrite) ||
	    (state.gpr[7] != 1 && state.gpr[7] != 2) || state.gpr[2] == 0) return;
	static std::atomic<size_t> remaining {64 * 1024};
	size_t available = remaining.load(std::memory_order_relaxed);
	size_t size = 0;
	do {
		size = std::min<size_t>(std::min<uint64_t>(512, state.gpr[2]), available);
		if (size == 0) return;
	} while (!remaining.compare_exchange_weak(available, available - size, std::memory_order_relaxed));
	std::array<unsigned char, 512> bytes {};
	const bool readable = read_diagnostic_memory(state.gpr[6], bytes.data(), size);
	std::array<char, 4 * 512 + 1> escaped {};
	size_t at = 0;
	constexpr char hex[] = "0123456789abcdef";
	if (readable) for (size_t i = 0; i < size; ++i)
	{
		const unsigned char c = bytes[i];
		if (c >= 32 && c <= 126 && c != '"' && c != '\\') escaped[at++] = static_cast<char>(c);
		else { escaped[at++] = '\\'; escaped[at++] = 'x'; escaped[at++] = hex[c >> 4]; escaped[at++] = hex[c & 15]; }
	}
	const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count();
	std::fprintf(stderr,
	    "[GUEST_CONSOLE_TRACE] monotonicMs=%lld thread=%d fd=%" PRIu64 " return=0x%016" PRIx64
	    " requested=%" PRIu64 " captured=%zu readable=%u text=\"%s\"\n",
	    static_cast<long long>(now_ms), Core::Thread::GetThreadIdUnique(), state.gpr[7], guest_return,
	    state.gpr[2], readable ? size : 0, readable ? 1U : 0U, escaped.data());
}

static void trace_host_error(const SymbolDatabase* symbols, uint64_t target,
                             uint64_t guest_return, const Cpu::X64State& state)
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_X64_HOST_ERROR_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	if (!enabled) return;
	trace_guest_console(target, guest_return, state);
	const auto* scalar = symbols != nullptr ? symbols->HostCallScalarMetadataFor(target) : nullptr;
	if (scalar != nullptr && scalar->result != HostCallScalarType::Gp) return;
	const uint64_t result = state.gpr[0];
	const uint32_t code = static_cast<uint32_t>(result);
	// Diagnostic candidates only: a negative 32-bit status, zero- or sign-
	// extended by its ABI. Pointer and floating-point results are not decoded.
	if ((result >> 32) != 0 && (result >> 32) != UINT32_MAX) return;
	if ((code & 0xff000000U) != 0x80000000U ||
	    code == 0x80020023U || code == 0x8002003cU ||
	    code == 0x80410123U || code == 0x8041013cU || code == 0x80960007U) return;
	struct Entry { uint64_t target = 0; uint32_t code = 0; uint32_t seen = 0; };
	static std::array<Entry, 128> entries {};
	static std::mutex mutex;
	static uint32_t count = 0;
	uint32_t index = 0;
	uint32_t occurrence = 0;
	{
		std::lock_guard lock(mutex);
		if (count >= 2048) return;
		Entry* entry = nullptr;
		for (auto& candidate: entries)
		{
			if (candidate.target == target && candidate.code == code) { entry = &candidate; break; }
			if (candidate.target == 0) { candidate.target = target; candidate.code = code; entry = &candidate; break; }
		}
		if (entry == nullptr || entry->seen == UINT32_MAX) return;
		occurrence = ++entry->seen;
		// Preserve first occurrences and occasional repetitions without letting
		// polling on one status consume the whole diagnostic window.
		if (occurrence > 4 && (occurrence & (occurrence - 1)) != 0) return;
		index = count++;
	}
	const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count();
	std::fprintf(stderr,
	    "[HLE_ERROR_TRACE] index=%u occurrence=%u monotonicMs=%lld thread=%d target=0x%016" PRIx64
	    " symbol=%s return=0x%016" PRIx64 " result=0x%016" PRIx64
	    " guestRsp=0x%016" PRIx64
	    " args=0x%016" PRIx64 ",0x%016" PRIx64 ",0x%016" PRIx64 ",0x%016" PRIx64 "\n",
	    index, occurrence, static_cast<long long>(now_ms), Core::Thread::GetThreadIdUnique(), target,
	    host_call_symbol(target), guest_return, result, state.gpr[4], state.gpr[7], state.gpr[6], state.gpr[2], state.gpr[1]);
}
#endif

static bool is_potentially_blocking_host_call(const char* symbol)
{
	if (symbol == nullptr || symbol[0] == '\0' || symbol[0] == '?') return false;
	return std::strstr(symbol, "Wait") != nullptr || std::strstr(symbol, "wait") != nullptr ||
	       std::strstr(symbol, "Join") != nullptr || std::strstr(symbol, "join") != nullptr ||
	       std::strstr(symbol, "Sleep") != nullptr || std::strstr(symbol, "sleep") != nullptr;
}

struct HostWaitTraceEntry
{
	bool active = false;
	bool reported = false;
	uint64_t call_id = 0;
	int thread_id = 0;
	uint64_t guest_rip = 0;
	uint64_t target = 0;
	const char* symbol = nullptr;
	uint64_t args[3] {};
	std::chrono::steady_clock::time_point start {};
};

static HostWaitTraceEntry g_host_wait_trace[64];
static std::mutex g_host_wait_trace_mutex;
static std::atomic<uint64_t> g_host_wait_call_id {0};

static void print_host_wait_trace(const char* phase, const HostWaitTraceEntry& entry,
                                  std::chrono::steady_clock::time_point now, uint64_t result)
{
	auto* program = RuntimeLinker::Instance()->FindProgramByAddr(entry.guest_rip);
	const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.start).count();
	flockfile(stderr);
	std::fprintf(stderr, "Guest HLE wait %s: id=%" PRIu64 " thread=%d elapsed_ms=%" PRId64 " rip=0x%016" PRIx64,
	             phase, entry.call_id, entry.thread_id, static_cast<int64_t>(elapsed_ms), entry.guest_rip);
	if (program != nullptr)
	{
		std::fprintf(stderr, " module=%s offset=0x%016" PRIx64,
		             program->file_name.FilenameWithoutDirectory().C_Str(), entry.guest_rip - program->base_vaddr);
	}
	std::fprintf(stderr,
	             " target=0x%016" PRIx64 " symbol=%s args=0x%016" PRIx64 ",0x%016" PRIx64 ",0x%016" PRIx64
	             " result=0x%016" PRIx64 "\n",
	             entry.target, entry.symbol, entry.args[0], entry.args[1], entry.args[2], result);
	std::fflush(stderr);
	funlockfile(stderr);
}

static void report_stalled_host_waits(std::chrono::steady_clock::time_point now)
{
	constexpr auto threshold = std::chrono::seconds(1);
	for (auto& entry: g_host_wait_trace)
	{
		if (entry.active && !entry.reported && now - entry.start >= threshold)
		{
			entry.reported = true;
			print_host_wait_trace("blocked", entry, now, 0);
		}
	}
}

static uint64_t begin_host_wait_trace(uint64_t guest_rip, uint64_t target, const char* symbol,
                                      const Cpu::X64State* state)
{
	const auto now = std::chrono::steady_clock::now();
	std::lock_guard lock(g_host_wait_trace_mutex);
	report_stalled_host_waits(now);
	for (auto& entry: g_host_wait_trace)
	{
		if (!entry.active)
		{
			entry = {};
			entry.active = true;
			entry.call_id = g_host_wait_call_id.fetch_add(1, std::memory_order_relaxed) + 1;
			entry.thread_id = Core::Thread::GetThreadIdUnique();
			entry.guest_rip = guest_rip;
			entry.target = target;
			entry.symbol = symbol;
			entry.args[0] = state->gpr[7];
			entry.args[1] = state->gpr[6];
			entry.args[2] = state->gpr[2];
			entry.start = now;
			return entry.call_id;
		}
	}
	return 0;
}

static void end_host_wait_trace(uint64_t call_id, uint64_t result)
{
	if (call_id == 0) return;
	const auto now = std::chrono::steady_clock::now();
	std::lock_guard lock(g_host_wait_trace_mutex);
	report_stalled_host_waits(now);
	for (auto& entry: g_host_wait_trace)
	{
		if (entry.active && entry.call_id == call_id)
		{
			if (entry.reported) print_host_wait_trace("leave", entry, now, result);
			entry = {};
			return;
		}
	}
}
#endif

using HostCallGuestMemoryRead = bool (*)(Cpu::X64State* state,
	uint64_t base, size_t offset, void* destination, size_t size);
thread_local HostCallGuestMemoryRead g_host_call_guest_memory_read = nullptr;

// GP registers captured at the HLE boundary of the host call currently
// executing on this thread. Set for the whole synchronous host call so
// blocking HLE waits can reconstruct the interrupted state of a pending guest
// exception. Thread-local and always restored by its scope.
thread_local const uint64_t* g_active_host_call_gprs = nullptr;

class ScopedActiveHostCallGprs
{
public:
	explicit ScopedActiveHostCallGprs(const uint64_t* gprs) noexcept
	    : m_previous(g_active_host_call_gprs)
	{
		g_active_host_call_gprs = gprs;
	}
	~ScopedActiveHostCallGprs() noexcept { g_active_host_call_gprs = m_previous; }

	LUMEN_CLASS_NO_COPY(ScopedActiveHostCallGprs);

private:
	const uint64_t* m_previous = nullptr;
};

const uint64_t* TryGetActiveHostCallGuestGprs()
{
	return g_active_host_call_gprs;
}

#if (defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)) || \
    (defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK) && defined(__APPLE__))
static thread_local Cpu::X64State* g_hle_memory_fault_state = nullptr;

void ReportGuestMemoryFault(uint64_t address, size_t size, bool write)
{
	auto* state = g_hle_memory_fault_state;
	if (state == nullptr || state->memory_fault_size != 0) return;
	state->memory_fault_address = address;
	state->memory_fault_size = static_cast<uint32_t>(std::min<size_t>(
	    std::max<size_t>(size, 1), UINT32_MAX));
	state->memory_fault_write = write;
}
#endif

#if (defined(LUMEN_FEXCORE_RUNTIME) || defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)) && \
    (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
bool ReadGuestString(uint64_t address, size_t limit, std::string* text, bool report_fault)
{
	if (text == nullptr) return false;
#if !defined(__APPLE__)
	(void)report_fault;
#endif
	text->clear();
	while (text->size() < limit)
	{
		constexpr uint64_t guest_page = 4096;
		size_t count = std::min<size_t>(guest_page - address % guest_page, limit - text->size());
		Core::VirtualMemory::ManagedRegion region {};
		const auto state = Core::VirtualMemory::QueryManagedRegion(address, &region);
		if (state == Core::VirtualMemory::ManagedRegionState::Visible && region.end > address)
			count = std::min<uint64_t>(count, region.end - address);
		Core::VirtualMemory::GuestDataLease read;
		if (!Core::VirtualMemory::TryAcquireGuestDataLease(
		        address, count, Core::VirtualMemory::Mode::Read, &read))
		{
#if defined(__APPLE__)
			if (report_fault) ReportGuestMemoryFault(address, count, false);
#endif
			return false;
		}
		const auto* bytes = reinterpret_cast<const char*>(read.HostBegin());
		const auto* end = static_cast<const char*>(std::memchr(bytes, 0, count));
		text->append(bytes, end != nullptr ? static_cast<size_t>(end - bytes) : count);
		if (end != nullptr || text->size() == limit) return true;
		if (address > UINT64_MAX - count)
		{
#if defined(__APPLE__)
			if (report_fault) ReportGuestMemoryFault(address, count, false);
#endif
			return false;
		}
		address += count;
	}
	return true;
}
#endif

static bool marshal_host_call(uint64_t target, Cpu::X64State* state, HostCallGpMetadata metadata)
{
	if (target == 0 || state == nullptr || metadata.arity < HOST_CALL_GP_LEGACY_ARITY ||
	    metadata.arity > HOST_CALL_GP_MAX_ARITY)
	{
		return false;
	}
	if (metadata.arity == HOST_CALL_GP_LEGACY_ARITY && metadata.invoker != nullptr) return false;
	if (metadata.arity > HOST_CALL_GP_LEGACY_ARITY && metadata.invoker == nullptr) return false;

	std::array<uint64_t, HOST_CALL_GP_MAX_ARITY> args {
	    state->gpr[7], state->gpr[6], state->gpr[2], state->gpr[1], state->gpr[8], state->gpr[9],
	};
	for (uint32_t index = HOST_CALL_GP_REGISTER_ARGUMENT_COUNT; index < metadata.arity; index++)
	{
		const size_t stack_offset =
		    sizeof(uint64_t) * (index - HOST_CALL_GP_REGISTER_ARGUMENT_COUNT + 1);
		const bool read = g_host_call_guest_memory_read != nullptr
		                      ? g_host_call_guest_memory_read(
		                            state, state->gpr[4], stack_offset,
		                            &args[index], sizeof(args[index]))
		                      : Cpu::X64ReadGuestMemory(
		                            state->gpr[4], stack_offset,
		                            &args[index], sizeof(args[index]));
		if (!read) return true;
	}

	if (metadata.invoker != nullptr)
	{
		state->gpr[0] = metadata.invoker(args.data());
		return true;
	}

	using host_func_8_t = LUMEN_SYSV_ABI uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
	state->gpr[0] = reinterpret_cast<host_func_8_t>(target)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
	return true;
}

static bool marshal_scalar_host_call(Cpu::X64State* state, const HostCallScalarMetadata& metadata)
{
	if (state == nullptr || metadata.invoker == nullptr || metadata.arity > HOST_CALL_SCALAR_MAX_ARITY)
		return false;
	if (state->memory_fault_size != 0) return true;
	constexpr uint32_t gp_registers[] = {7, 6, 2, 1, 8, 9};
	uint32_t gp_index = 0;
	uint32_t fp_index = 0;
	size_t stack_offset = sizeof(uint64_t);
	std::array<uint64_t, HOST_CALL_SCALAR_MAX_ARITY> arguments {};
	for (uint32_t index = 0; index < metadata.arity; ++index)
	{
		const auto type = metadata.arguments[index];
		const bool gp = type == HostCallScalarType::Gp;
		if (!gp && type != HostCallScalarType::Float32 && type != HostCallScalarType::Float64) return false;
		const size_t size = type == HostCallScalarType::Float32 ? sizeof(uint32_t) : sizeof(uint64_t);
		if (gp && gp_index < std::size(gp_registers))
		{
			arguments[index] = state->gpr[gp_registers[gp_index++]];
		} else if (!gp && fp_index < 8)
		{
			// Scalar payloads occupy the low bits; unused SIMD bits are preserved.
			std::memcpy(&arguments[index], state->xmm[fp_index++], size);
		} else
		{
			const bool read = g_host_call_guest_memory_read != nullptr
			    ? g_host_call_guest_memory_read(state, state->gpr[4], stack_offset, &arguments[index], size)
			    : Cpu::X64ReadGuestMemory(state->gpr[4], stack_offset, &arguments[index], size);
			if (!read) return true;
			stack_offset += sizeof(uint64_t);
		}
	}
	const uint64_t result = metadata.invoker(arguments.data());
	switch (metadata.result)
	{
		case HostCallScalarType::Void: break;
		case HostCallScalarType::Gp: state->gpr[0] = result; break;
		case HostCallScalarType::Float32: std::memcpy(state->xmm[0], &result, sizeof(uint32_t)); break;
		case HostCallScalarType::Float64: std::memcpy(state->xmm[0], &result, sizeof(uint64_t)); break;
		default: return false;
	}
	return true;
}

static bool marshal_variadic_host_call(Cpu::X64State* state, HostCallVariadicKind kind)
{
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__APPLE__)
	if (state == nullptr || state->gpr[4] > UINT64_MAX - sizeof(uint64_t) ||
	    (state->gpr[0] & 0xff) > 8) return false;
	if (state->memory_fault_size != 0) return true;
	alignas(16) Libs::VaContext context {};
	constexpr unsigned registers[] = {7, 6, 2, 1, 8, 9};
	for (unsigned index = 0; index != 6; ++index) context.reg_save_area.gp[index] = state->gpr[registers[index]];
	for (unsigned index = 0; index != 8; ++index) std::memcpy(&context.reg_save_area.fp[index], state->xmm[index], 16);
	context.va_list = {offsetof(Libs::VaRegSave, gp), offsetof(Libs::VaRegSave, fp),
	    reinterpret_cast<void*>(state->gpr[4] + sizeof(uint64_t)), &context.reg_save_area};
	struct Reader
	{
		Cpu::X64State* state;
		const Libs::VaRegSave* registers;
		bool failed = false;
		Libs::VaReadHook previous = Libs::g_va_read_hook;
		void* previous_opaque = Libs::g_va_read_opaque;
		bool* previous_error = Libs::g_va_read_error;
		~Reader()
		{
			Libs::g_va_read_hook = previous; Libs::g_va_read_opaque = previous_opaque;
			Libs::g_va_read_error = previous_error;
		}
	} reader {state, &context.reg_save_area};
	Libs::g_va_read_opaque = &reader;
	Libs::g_va_read_error = &reader.failed;
	Libs::g_va_read_hook = [](void* opaque, const void* address, void* destination, size_t size, bool extended_float)
	{
		auto& current = *static_cast<Reader*>(opaque);
		if (current.failed) return false;
		// The Darwin host long double is not the guest's x87 memory format.
		if (extended_float) { current.failed = true; return false; }
		const uint64_t at = reinterpret_cast<uint64_t>(address);
		const uint64_t base = reinterpret_cast<uint64_t>(current.registers);
		if (at >= base && at - base < sizeof(Libs::VaRegSave))
		{
			const size_t offset = static_cast<size_t>(at - base);
			const size_t fp_begin = offsetof(Libs::VaRegSave, fp);
			const size_t limit = offset < fp_begin ? fp_begin : fp_begin + (current.state->gpr[0] & 0xff) * 16;
			if (offset > limit || size > limit - offset) { current.failed = true; return false; }
			std::memcpy(destination, address, size);
			return true;
		}
		const bool read = g_host_call_guest_memory_read != nullptr
		    ? g_host_call_guest_memory_read(current.state, at, 0, destination, size)
		    : Cpu::X64ReadGuestMemory(at, 0, destination, size);
		current.failed = !read;
		return read;
	};
	const int result = kind == HostCallVariadicKind::Printf
	    ? Libs::GetPrintfCtxFunc()(&context) : Libs::GetSnrintfCtxFunc()(&context);
	if (reader.failed) return state->memory_fault_size != 0;
	state->gpr[0] = static_cast<uint64_t>(static_cast<int64_t>(result));
	return true;
#else
	(void)state;
	(void)kind;
	return false;
#endif
}

static bool invoke_host_call_with_metadata(uint64_t target, Cpu::X64State* state,
                                          const HostCallDispatchMetadata& dispatch)
{
#if (defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)) || \
    (defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK) && defined(__APPLE__))
	struct FaultScope
	{
		Cpu::X64State* previous = g_hle_memory_fault_state;
		explicit FaultScope(Cpu::X64State* current) { g_hle_memory_fault_state = current; }
		~FaultScope() { g_hle_memory_fault_state = previous; }
	} fault_scope(state);
#endif
	const auto metadata = dispatch.gp;
	const auto* scalar = dispatch.scalar;
	const auto variadic = dispatch.variadic;
	const bool handled = target != 0 && (variadic != HostCallVariadicKind::None
	    ? marshal_variadic_host_call(state, variadic) : (scalar != nullptr
	    ? marshal_scalar_host_call(state, *scalar) : marshal_host_call(target, state, metadata)));
	if (handled && state != nullptr && state->memory_fault_size != 0) Cpu::X64RollbackHostCallStack(state);
	return handled;
}

bool InvokeHostCall(const SymbolDatabase* symbols, uint64_t target, Cpu::X64State* state)
{
	const auto dispatch = symbols != nullptr ? symbols->ResolveHostCall(target) : HostCallDispatchMetadata {};
	return invoke_host_call_with_metadata(target, state, dispatch);
}

#if defined(__APPLE__)
namespace {

constexpr size_t NATIVE_HOST_TARGET_CACHE_SIZE = 512;
static_assert((NATIVE_HOST_TARGET_CACHE_SIZE & (NATIVE_HOST_TARGET_CACHE_SIZE - 1)) == 0);

struct NativeHostTargetCacheEntry
{
	uint64_t target              = 0;
	uint64_t negative_generation = 0;
};

static_assert(sizeof(NativeHostTargetCacheEntry) == 2 * sizeof(uint64_t));

std::atomic<uint64_t> g_native_host_image_generation {1};
thread_local std::array<NativeHostTargetCacheEntry, NATIVE_HOST_TARGET_CACHE_SIZE> g_native_host_target_cache {};
#if defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)
thread_local bool     g_native_host_dladdr_count_enabled = false;
thread_local uint64_t g_native_host_dladdr_call_count    = 0;
#endif

void native_host_image_added(const mach_header*, intptr_t)
{
	g_native_host_image_generation.fetch_add(1, std::memory_order_release);
}

struct NativeHostImageRegistration
{
	NativeHostImageRegistration() { _dyld_register_func_for_add_image(native_host_image_added); }
};

const NativeHostImageRegistration g_native_host_image_registration {};

size_t native_host_target_cache_index(uint64_t target)
{
	uint64_t key = target >> 2u;
	key ^= key >> 19u;
	key ^= key >> 37u;
	key *= 0x9e3779b97f4a7c15ULL;
	key ^= key >> 29u;
	return static_cast<size_t>(key) & (NATIVE_HOST_TARGET_CACHE_SIZE - 1);
}

} // namespace
#endif

#if defined(__ANDROID__) || defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)
namespace {

// An interpreted x86 CALL reaches this classifier for both guest and HLE
// targets. Android's dladdr takes the dynamic-linker symbol lock and is far too
// expensive to run at every guest CALL. Cache only classifications whose
// lifetime is proven: anonymous mappings owned by VirtualMemory are invalidated
// by its mutation generation, while HLE functions in this process-lifetime
// runtime image remain loaded until the dedicated runtime process exits.
constexpr size_t ANDROID_NATIVE_HOST_TARGET_CACHE_SIZE = 512;
static_assert((ANDROID_NATIVE_HOST_TARGET_CACHE_SIZE &
               (ANDROID_NATIVE_HOST_TARGET_CACHE_SIZE - 1)) == 0);

struct AndroidManagedGuestTargetCacheEntry
{
	uint64_t target     = 0;
	uint64_t generation = 0;
};

static_assert(sizeof(AndroidManagedGuestTargetCacheEntry) ==
	          2 * sizeof(uint64_t));

struct AndroidNativeHostTargetCache
{
	std::array<uint64_t, ANDROID_NATIVE_HOST_TARGET_CACHE_SIZE> native_targets {};
	std::array<AndroidManagedGuestTargetCacheEntry,
	           ANDROID_NATIVE_HOST_TARGET_CACHE_SIZE>
	    managed_guest_targets {};

	[[nodiscard]] bool HasNative(size_t index, uint64_t target) const
	{
		return native_targets[index] == target;
	}

	[[nodiscard]] bool HasManagedGuest(size_t index, uint64_t target,
	                                   uint64_t generation) const
	{
		const auto& entry = managed_guest_targets[index];
		return entry.target == target && entry.generation == generation;
	}

	void StoreNative(size_t index, uint64_t target)
	{
		native_targets[index] = target;
	}

	void StoreManagedGuest(size_t index, uint64_t target,
	                       uint64_t generation)
	{
		managed_guest_targets[index] = {target, generation};
	}
};

// The former mixed cache used three uint64_t-sized slots per entry after
// alignment. Splitting it preserves that exact per-thread footprint while
// preventing ordinary guest CALLs from evicting stable runtime-image targets.
static_assert(sizeof(AndroidNativeHostTargetCache) ==
	          ANDROID_NATIVE_HOST_TARGET_CACHE_SIZE * 3 * sizeof(uint64_t));

#if defined(__ANDROID__)
thread_local AndroidNativeHostTargetCache g_android_native_host_target_cache {};
#endif

size_t android_native_host_target_cache_index(uint64_t target)
{
	uint64_t key = target >> 2u;
	key ^= key >> 19u;
	key ^= key >> 37u;
	key *= 0x9e3779b97f4a7c15ULL;
	key ^= key >> 29u;
	return static_cast<size_t>(key) &
	       (ANDROID_NATIVE_HOST_TARGET_CACHE_SIZE - 1);
}

#if defined(__ANDROID__)
const void* android_runtime_image_base()
{
	static const void* base = []
	{
		Dl_info info {};
		return dladdr(reinterpret_cast<const void*>(&IsNativeHostCallTarget),
		              &info) != 0
		           ? info.dli_fbase
		           : nullptr;
	}();
	return base;
}
#endif

} // namespace
#endif

bool IsNativeHostCallTarget(uint64_t target)
{
	if (target == 0) return false;
#if defined(__APPLE__)
	// Darwin treats fixed-address VM allocations as hints when overwrite is not
	// requested. Guest code can therefore land below SYSTEM_RESERVED. dladdr
	// recognizes addresses in loaded native Mach-O images without mistaking an
	// anonymous guest mapping (including interpreter trampolines) for host code.
	auto& entry = g_native_host_target_cache[native_host_target_cache_index(target)];
	if (entry.target == target && entry.negative_generation == 0) return true;

	const uint64_t generation_before = g_native_host_image_generation.load(std::memory_order_acquire);
	if (entry.target == target && entry.negative_generation == generation_before) return false;

	// Guest mappings created through Core::VirtualMemory are tracked exactly;
	// reject their whole allocation before the much more expensive dyld lookup.
	if (Core::VirtualMemory::IsManagedAddress(target)) return false;

	Dl_info info {};
#if defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)
	if (g_native_host_dladdr_count_enabled) g_native_host_dladdr_call_count++;
#endif
	const bool native = dladdr(reinterpret_cast<const void*>(target), &info) != 0 && info.dli_fbase != nullptr;
	if (native)
	{
		if (info.dli_fbase == static_cast<const void*>(_dyld_get_image_header(0))) entry = {target, 0};
	} else
	{
		const uint64_t generation_after = g_native_host_image_generation.load(std::memory_order_acquire);
		if (generation_before == generation_after) entry = {target, generation_before};
	}
	return native;
#elif defined(__ANDROID__)
	// Android ASLR places native libraries well above SYSTEM_RESERVED. dladdr
	// identifies linker-loaded ELF images while ignoring anonymous guest maps.
	// Avoid taking the dynamic-linker lock again for hot guest CALL targets and
	// for HLE functions linked into the process-lifetime runtime image.
	const size_t cache_index = android_native_host_target_cache_index(target);
	if (g_android_native_host_target_cache.HasNative(cache_index, target))
		return true;
	const uint64_t generation =
	    Core::VirtualMemory::GetMutationGeneration();
	if (g_android_native_host_target_cache.HasManagedGuest(
	        cache_index, target, generation))
	{
		return false;
	}
	if (Core::VirtualMemory::IsManagedAddress(target))
	{
		g_android_native_host_target_cache.StoreManagedGuest(
		    cache_index, target, generation);
		return false;
	}
	Dl_info info {};
	const bool native =
	    dladdr(reinterpret_cast<const void*>(target), &info) != 0 &&
	    info.dli_fbase != nullptr;
	if (native && info.dli_fbase == android_runtime_image_base())
	{
		g_android_native_host_target_cache.StoreNative(cache_index, target);
	}
	return native;
#else
	return target < SYSTEM_RESERVED;
#endif
}

#if defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)
bool ValidateAndroidNativeHostTargetCacheSeparationForTest()
{
	AndroidNativeHostTargetCache cache {};
	std::array<uint64_t, ANDROID_NATIVE_HOST_TARGET_CACHE_SIZE>
	    first_target_by_index {};
	uint64_t native_target = 0;
	uint64_t guest_target  = 0;
	size_t   collision_index = 0;
	for (uint64_t ordinal = 1;
	     ordinal <= ANDROID_NATIVE_HOST_TARGET_CACHE_SIZE + 1; ordinal++)
	{
		const uint64_t candidate = ordinal << 2u;
		const size_t index = android_native_host_target_cache_index(candidate);
		if (first_target_by_index[index] != 0)
		{
			native_target   = first_target_by_index[index];
			guest_target    = candidate;
			collision_index = index;
			break;
		}
		first_target_by_index[index] = candidate;
	}
	if (native_target == 0 || guest_target == 0 || native_target == guest_target)
		return false;

	constexpr uint64_t generation = 0x123456789abcdef0ULL;
	cache.StoreNative(collision_index, native_target);
	cache.StoreManagedGuest(collision_index, guest_target, generation);
	if (!cache.HasNative(collision_index, native_target) ||
	    cache.HasNative(collision_index, guest_target) ||
	    !cache.HasManagedGuest(collision_index, guest_target, generation) ||
	    cache.HasManagedGuest(collision_index, native_target, generation) ||
	    cache.HasManagedGuest(collision_index, guest_target, generation + 1))
	{
		return false;
	}

	// Replacing either direct-mapped classification must leave the colliding
	// entry in the other classification intact.
	cache.StoreManagedGuest(collision_index, guest_target, generation + 1);
	if (!cache.HasNative(collision_index, native_target) ||
	    !cache.HasManagedGuest(collision_index, guest_target, generation + 1))
	{
		return false;
	}
	cache.StoreNative(collision_index, guest_target);
	return cache.HasNative(collision_index, guest_target) &&
	       cache.HasManagedGuest(collision_index, guest_target, generation + 1);
}

void ResetNativeHostDladdrCallCountForTest()
{
	g_native_host_dladdr_call_count    = 0;
	g_native_host_dladdr_count_enabled = true;
}

uint64_t NativeHostDladdrCallCountForTest()
{
	return g_native_host_dladdr_call_count;
}
#endif

static bool run_host_call_impl(uint64_t target, Cpu::X64State* state,
	                            const SymbolDatabase* supplied_symbols,
	                            bool symbols_supplied,
	                            const HostCallDispatchMetadata* supplied_dispatch = nullptr)
{
	if (state == nullptr) return false;
	// Preserve the native-image dispatch order when the registry is disabled:
	// reject non-native targets before resolving SymbolDatabase and avoid the
	// registry's acquire/classification hot path entirely.
	static const bool authoritative_registry_enabled =
	    AuthoritativeHostCallRegistryEnabled();
	if (!authoritative_registry_enabled && !IsNativeHostCallTarget(target))
	{
		return false;
	}

	const SymbolDatabase* symbols = supplied_symbols;
	if (!symbols_supplied)
	{
		if (auto* linker = RuntimeLinker::Instance();
		    linker != nullptr && linker->Symbols() != nullptr)
		{
			symbols = linker->Symbols();
		}
	}
	const auto dispatch = supplied_dispatch != nullptr ? *supplied_dispatch :
	    (symbols != nullptr && symbols->HostCallRegistrySealed() ? symbols->ResolveHostCall(target) : HostCallDispatchMetadata {});
	if (authoritative_registry_enabled)
	{
		const auto registered_target = dispatch.classification;
		if (registered_target == HostCallRegistryClassification::Guest ||
		    (registered_target == HostCallRegistryClassification::Unsealed &&
		     !IsNativeHostCallTarget(target)))
		{
			return false;
		}
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (GuestExecutionStopRequested())
	{
		// `false` means "not a host target" and would make the interpreter try
		// to execute the native ARM64 address as guest x86-64. Complete the call
		// as a cancelled no-op and stop this Run without skipping its cleanup.
		state->gpr[0] = 0;
		state->execution_cancel_requested = true;
		return true;
	}
#endif

	// Publish this host call's boundary GP state for the whole synchronous
	// call so a blocking wait inside the import can reconstruct the interrupted
	// state, then consume one queued guest exception before the import runs.
	// The handler executes as a nested guest call at this boundary and the
	// import then proceeds normally when it returns.
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	// Local capture diagnostics: attribute later records to this thread and its
	// most recent HLE import (the ledger) and seal this boundary state for the
	// guest-root snapshot. Both selectors are off by default; every call here is
	// a no-op then. The boundary RIP stays zero on the FEX route, whose bridge
	// state carries no execution history.
	Core::VirtualMemory::NoteCaptureDiagnosticHostCall(target);
	{
		uint64_t guest_stack_base = 0;
		uint64_t guest_stack_size = 0;
		(void)CurrentGuestRunStackBounds(&guest_stack_base, &guest_stack_size);
		const uint64_t boundary_guest_rip =
		    state->instruction_trace[(state->instruction_trace_index - 1) & 31];
		Libs::LibKernel::KernelGuestThreadPublishBoundaryState(
		    state->gpr, boundary_guest_rip, target, guest_stack_base,
		    guest_stack_size);
	}
#endif
	ScopedActiveHostCallGprs active_host_call_gprs(state->gpr);
	Libs::LibKernel::PthreadDeliverPendingException(state->gpr);

	const uint64_t arg0  = state->gpr[7];
	const uint64_t arg1  = state->gpr[6];
	const uint64_t arg2  = state->gpr[2];
#if defined(__APPLE__)
	const bool     trace_requested = host_wait_trace_enabled();
	const char*    symbol          = host_call_symbol(target, trace_requested);
	const bool     trace_wait      = trace_requested && is_potentially_blocking_host_call(symbol);
	const uint64_t guest_rip       = state->instruction_trace[(state->instruction_trace_index - 1) & 31];
	const uint64_t call_id         = trace_wait ? begin_host_wait_trace(guest_rip, target, symbol, state) : 0;
#endif
#if defined(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY)
	// A native HLE target may wait indefinitely. End and exclude the whole
	// fallback sample immediately before entering it so the first-submit report
	// never waits for host-side synchronization to return.
	Cpu::X64ExcludeA64DynarecStartupFallbackForHostCall();
#endif
#if defined(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
	// The paired phase-6AF sample owns the surrounding TryExecuteFastBlock
	// scope as well as its optional CompiledBlock::Execute segment. Disarm the
	// complete sample before a native callback can block or re-enter the guest.
	Cpu::X64ExcludeA64DynarecPhase6AFSampleForHostCall();
#endif
#if defined(LUMEN_A64_DYNAREC_PHASE6CM_PROFILE)
	// HLE may block or re-enter guest execution. Discard the complete sampled
	// ordinary window before InvokeHostCall so wait time is never elected as
	// removable ExecuteFastBlock CPU work.
	Cpu::X64ExcludeA64DynarecPhase6CMSampleForHostCall();
#endif
#if defined(LUMEN_A64_DYNAREC_PHASE6CO_PROFILE)
	// Host callbacks can wait or enter a nested guest. Exclude the whole 6CO
	// cluster before crossing that boundary; no elapsed HLE time is attributed
	// to an operation-snapshot candidate.
	Cpu::X64ExcludeA64DynarecPhase6COSampleForHostCall();
#endif
	// The sealed database cannot change while a nested guest exception runs.
	// Before seal, preserve the previous late metadata resolution after it.
#if defined(__APPLE__) && defined(LUMEN_FEXCORE_RUNTIME)
	const auto storage_dialog_call = begin_storage_dialog_trace(target, dispatch);
#endif
	const bool handled = dispatch.classification != HostCallRegistryClassification::Unsealed
	    ? invoke_host_call_with_metadata(target, state, dispatch) : InvokeHostCall(symbols, target, state);
#if defined(__APPLE__) && defined(LUMEN_FEXCORE_RUNTIME)
	end_storage_dialog_trace(storage_dialog_call, dispatch, *state, handled);
#endif
#if defined(__APPLE__)
	if (trace_wait) end_host_wait_trace(call_id, state->gpr[0]);
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	// A guest exit HLE requests the session stop from inside InvokeHostCall.
	// Publish that request into this interpreter invocation before returning to
	// guest code, so no instruction following exit can run.
	const bool cancelled = GuestExecutionStopRequested();
	if (cancelled)
	{
		state->execution_cancel_requested = true;
	}
#endif
	if (!handled)
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		// Treat a concurrently cancelled native-target call as consumed so the
		// interpreter unwinds its tentative guest CALL frame before stopping.
		return cancelled;
#else
		return false;
#endif
	}
	if (state->memory_fault_size == 0 && state->diagnostic_history_enabled)
	{
		const uint32_t call_index           = state->host_call_index++ & 255;
		state->host_call_rip[call_index]    = state->instruction_trace[(state->instruction_trace_index - 1) & 31];
		state->host_call_target[call_index] = target;
		state->host_call_result[call_index] = state->gpr[0];
		state->host_call_arg0[call_index]   = arg0;
		state->host_call_arg1[call_index]   = arg1;
		state->host_call_arg2[call_index]   = arg2;
	}
	return true;
}

#if defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)
bool RunHostCallForTest(const SymbolDatabase* symbols, uint64_t target,
	                    Cpu::X64State* state)
{
	return run_host_call_impl(target, state, symbols, true);
}
#endif

[[maybe_unused]] static bool run_host_call(uint64_t target, Cpu::X64State* state)
{
	return run_host_call_impl(target, state, nullptr, false);
}

#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID) && \
    (defined(LUMEN_FEXCORE_RUNTIME) || defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK))
namespace {

bool fex_unresolved_argument_regions_enabled(const char* value)
{
	return value != nullptr && value[0] == '1' && value[1] == '\0';
}

bool format_fex_unresolved_argument_region(char* output, size_t capacity,
	int thread_id, size_t index, uint64_t address)
{
	if (output == nullptr || capacity == 0) return false;
	Core::VirtualMemory::ManagedRegion region {};
	const auto region_state = Core::VirtualMemory::QueryManagedRegion(address, &region);
	const auto mode = static_cast<uint32_t>(region.mode);
	const int written = std::snprintf(
	    output, capacity,
	    "FEX unresolved argument region: thread=%d index=%zu address=0x%016" PRIx64
	    " state=%u begin=0x%016" PRIx64 " end=0x%016" PRIx64
	    " mode=%u readable=%u writable=%u executable=%u\n",
	    thread_id, index, address, static_cast<unsigned>(region_state),
	    region.begin, region.end, mode,
	    (mode & static_cast<uint32_t>(Core::VirtualMemory::Mode::Read)) != 0 ? 1u : 0u,
	    (mode & static_cast<uint32_t>(Core::VirtualMemory::Mode::Write)) != 0 ? 1u : 0u,
	    (mode & static_cast<uint32_t>(Core::VirtualMemory::Mode::Execute)) != 0 ? 1u : 0u);
	return written >= 0 && static_cast<size_t>(written) < capacity;
}

} // namespace

#if defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)
bool FexUnresolvedArgumentRegionsEnabledForTest(const char* value)
{
	return fex_unresolved_argument_regions_enabled(value);
}

bool FormatFexUnresolvedArgumentRegionForTest(char* output, size_t capacity,
	int thread_id, size_t index, uint64_t address)
{
	return format_fex_unresolved_argument_region(
	    output, capacity, thread_id, index, address);
}
#endif
#endif

#if defined(LUMEN_FEXCORE_RUNTIME)
namespace {
struct FexGuestRunOutcome
{
	uint32_t depth = 0;
	bool thread_exit = false;
	uint64_t exit_value = 0;
	bool callback_failed = false;
};

thread_local uint32_t g_fex_guest_run_depth = 0;
thread_local uint32_t g_fresh_guest_callback_depth = 0;
thread_local uint64_t g_fresh_guest_callback_tls = 0;
thread_local bool g_fresh_guest_callback_code_extent = false;
struct GuestX64RunContext
{
	uint64_t stack_base;
	uint64_t stack_size;
	uint64_t tls;
};
thread_local const GuestX64RunContext* g_guest_x64_run_context = nullptr;

class ScopedGuestX64RunContext final
{
public:
	explicit ScopedGuestX64RunContext(const GuestX64RunContext& context)
	    : m_parent(g_guest_x64_run_context) { g_guest_x64_run_context = &context; }
	~ScopedGuestX64RunContext() { g_guest_x64_run_context = m_parent; }
	LUMEN_CLASS_NO_COPY(ScopedGuestX64RunContext);
private:
	const GuestX64RunContext* m_parent;
};

class FreshGuestCallbackFrameMutation final
{
public:
	explicit FreshGuestCallbackFrameMutation(bool fresh)
	{
		if (fresh) m_token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
		m_valid = !fresh || (m_token != 0 && LumenAndroidFexCoreValidateFreshGuestCallbackParentV1() != 0);
	}
	~FreshGuestCallbackFrameMutation() { Close(); }
	bool Valid() const { return m_valid; }
	void Close()
	{
		if (m_token != 0)
		{
			const auto token = m_token;
			m_token = 0;
			if (LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 0)
				EXIT("Guest callback frame mutation close failed");
		}
	}
	LUMEN_CLASS_NO_COPY(FreshGuestCallbackFrameMutation);
private:
	uint64_t m_token = 0;
	bool m_valid = false;
};

class FexGuestRunDepthScope
{
public:
	FexGuestRunDepthScope()
	{
		if (g_fex_guest_run_depth == UINT32_MAX) EXIT("Guest run nesting overflow");
		++g_fex_guest_run_depth;
	}
	~FexGuestRunDepthScope() { --g_fex_guest_run_depth; }
	LUMEN_CLASS_NO_COPY(FexGuestRunDepthScope);
};
} // namespace

bool RuntimeLinker::DescribeFexUnresolvedPlt(
	Program* program, uint64_t index, String* description)
{
	if (description == nullptr) return false;
	description->Clear();

	Core::LockGuard lock(m_mutex);
	if (program == nullptr || !m_programs.Contains(program) ||
	    program->dynamic_info == nullptr ||
	    program->dynamic_info->jmprela_table == nullptr ||
	    program->dynamic_info->symbol_table == nullptr ||
	    program->dynamic_info->str_table == nullptr)
	{
		return false;
	}
	const uint64_t relocation_count =
	    program->dynamic_info->jmprela_table_size / sizeof(Elf64_Rela);
	if (index >= relocation_count) return false;

	const auto& relocation = program->dynamic_info->jmprela_table[index];
	const uint64_t symbol_index = relocation.GetSymbol();
	const uint64_t symbol_count =
	    program->dynamic_info->symbol_table_total_size / sizeof(Elf64_Sym);
	if (symbol_index >= symbol_count) return false;
	const auto& symbol = program->dynamic_info->symbol_table[symbol_index];
	if (symbol.st_name >= program->dynamic_info->str_table_size) return false;
	const char* symbol_name =
	    program->dynamic_info->str_table + symbol.st_name;
	const size_t remaining =
	    program->dynamic_info->str_table_size - symbol.st_name;
	if (::strnlen(symbol_name, remaining) == remaining) return false;

	const LibraryId* import_library = nullptr;
	const ModuleId* import_module = nullptr;
	const auto ids = String::FromUtf8(symbol_name).Split(U'#');
	if (ids.Size() == 3)
	{
		for (const auto& library: program->dynamic_info->import_libs)
			if (library.id == ids.At(1)) { import_library = &library; break; }
		for (const auto& module: program->dynamic_info->import_modules)
			if (module.id == ids.At(2)) { import_module = &module; break; }
	}
	*description = String::FromPrintf(
	    "module=%s index=%" PRIu64 " symbol=%s library=%s v%d import_module=%s v%d.%d",
	    program->file_name.FilenameWithoutDirectory().C_Str(), index, symbol_name,
	    import_library != nullptr ? import_library->name.C_Str() : "?",
	    import_library != nullptr ? import_library->version : -1,
	    import_module != nullptr ? import_module->name.C_Str() : "?",
	    import_module != nullptr ? import_module->version_major : -1,
	    import_module != nullptr ? import_module->version_minor : -1);
	return true;
}

namespace {

class ScopedFexHostCallMemoryReader
{
public:
	explicit ScopedFexHostCallMemoryReader(HostCallGuestMemoryRead reader)
	    : previous_(g_host_call_guest_memory_read)
	{
		g_host_call_guest_memory_read = reader;
	}

	~ScopedFexHostCallMemoryReader()
	{
		g_host_call_guest_memory_read = previous_;
	}

	LUMEN_CLASS_NO_COPY(ScopedFexHostCallMemoryReader);

private:
	HostCallGuestMemoryRead previous_ = nullptr;
};

bool read_fex_host_call_guest_memory(Cpu::X64State* state, uint64_t base,
	                                  size_t offset, void* destination,
	                                  size_t size)
{
	if (state == nullptr || destination == nullptr || size == 0 ||
	    base > UINT64_MAX - offset)
	{
		if (state != nullptr)
		{
			state->memory_fault_address = base;
			state->memory_fault_size = static_cast<uint32_t>(size);
			state->memory_fault_write = false;
		}
		return false;
	}
	const uint64_t address = base + offset;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	if (LumenMacOSFexCoreTryReadOwnedStackForHostCallV1(address, destination, size) != 0)
		return true;
#endif
	if (!ReadGuestMemory(address, destination, size))
	{
		state->memory_fault_address = address;
		state->memory_fault_size = static_cast<uint32_t>(size);
		state->memory_fault_write = false;
		return false;
	}
	return true;
}

void report_fex_unresolved_plt(Cpu::X64State* state)
{
	std::array<uint64_t, 3> stack_words {};
	if (state == nullptr ||
	    !read_fex_host_call_guest_memory(
	        state, state->gpr[4], 0, stack_words.data(),
	        sizeof(stack_words)))
	{
		std::fprintf(stderr,
		             "FEX unresolved PLT: unreadable guest stack rsp=0x%016" PRIx64
		             "\n",
		             state != nullptr ? state->gpr[4] : 0);
		return;
	}

	auto* linker = RuntimeLinker::Instance();
	auto* program = reinterpret_cast<Program*>(stack_words[0]);
	String description;
	const bool described =
	    linker != nullptr && linker->DescribeFexUnresolvedPlt(
	                             program, stack_words[1], &description);
	std::fprintf(
	    stderr,
	    "FEX unresolved PLT: %s program=0x%016" PRIx64
	    " index=%" PRIu64 " return=0x%016" PRIx64
	    " args=[0x%016" PRIx64 ",0x%016" PRIx64
	    ",0x%016" PRIx64 ",0x%016" PRIx64
	    ",0x%016" PRIx64 ",0x%016" PRIx64 "]\n",
	    described ? description.C_Str() : "invalid-metadata",
	    stack_words[0], stack_words[1], stack_words[2], state->gpr[7],
	    state->gpr[6], state->gpr[2], state->gpr[1], state->gpr[8],
	    state->gpr[9]);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	// Classify candidate pointer arguments without reading them. An unresolved
	// PLT remains fatal; these regions are call-site evidence, not an ABI guess.
	static const bool trace_argument_regions = [] {
		return fex_unresolved_argument_regions_enabled(
		    std::getenv("LUMEN_FEX_UNRESOLVED_ARGUMENT_REGIONS"));
	}();
	if (trace_argument_regions)
	{
		const std::array<uint64_t, 6> arguments = {
		    state->gpr[7], state->gpr[6], state->gpr[2],
		    state->gpr[1], state->gpr[8], state->gpr[9],
		};
		for (size_t index = 0; index < arguments.size(); ++index)
		{
			std::array<char, 256> line {};
			if (format_fex_unresolved_argument_region(
			        line.data(), line.size(), Core::Thread::GetThreadIdUnique(),
			        index, arguments[index]))
				std::fputs(line.data(), stderr);
		}
	}
#endif
	std::fflush(stderr);
}

uint32_t fex_host_call_impl(void* opaque, uint64_t target, uint64_t* guest_gprs,
	                        LumenAndroidFexCoreSimdViewV2* simd, uint64_t* next_guest_rip) noexcept
{
	if (guest_gprs == nullptr || next_guest_rip == nullptr || target == 0)
		return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	const bool kernel_exit = target == reinterpret_cast<uint64_t>(&Libs::LibKernel::PthreadExit);
	if (kernel_exit || target == reinterpret_cast<uint64_t>(&Libs::Posix::pthread_exit))
	{
		const auto* outcome = static_cast<FexGuestRunOutcome*>(opaque);
		const bool managed = Libs::LibKernel::PthreadHasManagedExitBoundary();
		// Preserve every unmanaged/unsupported receipt, even after many ordinary
		// worker exits, while bounding the normal managed-worker diagnostics.
		static std::atomic_uint managed_exit_reports = 0;
		if (!managed || outcome == nullptr || outcome->depth != 1 ||
		    managed_exit_reports.fetch_add(1, std::memory_order_relaxed) < 8)
		{
			__android_log_print(ANDROID_LOG_INFO, "LumenGuestCpu",
			    "LUMEN_ANDROID_GUEST_PTHREAD_EXIT_V1 route=%s nativeTid=%d depth=%u managed=%u stopRequested=%u",
			    kernel_exit ? "kernel-adapter" : "posix-native", static_cast<int>(gettid()),
			    outcome != nullptr ? outcome->depth : 0u, managed ? 1u : 0u,
			    GuestExecutionStopRequested() ? 1u : 0u);
		}
	}
#endif

	if (target == reinterpret_cast<uint64_t>(&Libs::LibKernel::PthreadExit))
	{
		auto* outcome = static_cast<FexGuestRunOutcome*>(opaque);
		const bool managed = Libs::LibKernel::PthreadHasManagedExitBoundary();
		if (outcome == nullptr || outcome->depth != 1 || !managed)
		{
			std::fprintf(stderr, "FEX pthread exit unsupported: depth=%u managed=%u\n",
			             outcome != nullptr ? outcome->depth : 0u, managed ? 1u : 0u);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
			__android_log_print(ANDROID_LOG_ERROR, "LumenGuestCpu",
			    "LUMEN_ANDROID_GUEST_PTHREAD_EXIT_DENIED_V1 nativeTid=%d depth=%u managed=%u stopRequested=%u rsp=0x%016" PRIx64,
			    static_cast<int>(gettid()), outcome != nullptr ? outcome->depth : 0u, managed ? 1u : 0u,
			    GuestExecutionStopRequested() ? 1u : 0u, guest_gprs[4]);
#endif
			return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
		}
		// Stop without consuming the guest CALL return address. The adapter must
		// release its run, TLS and barrier before the native thread can exit.
		outcome->thread_exit = true;
		outcome->exit_value = guest_gprs[7];
		guest_gprs[0] = outcome->exit_value;
		return LUMEN_ANDROID_FEXCORE_HOST_CALL_STOP_V1;
	}

#if defined(__aarch64__) && \
	(LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX)
	Cpu::X64State state {Cpu::X64State::HostCallOnly {}};
#else
	Cpu::X64State state {};
#endif
	std::copy_n(guest_gprs, std::size(state.gpr), state.gpr);
#if defined(__APPLE__)
	trace_memory_watch("before", target, state);
#endif
	auto* linker = RuntimeLinker::Instance();
	const auto* symbols = linker != nullptr ? linker->Symbols() : nullptr;
	const auto dispatch = symbols != nullptr ? symbols->ResolveHostCall(target) : HostCallDispatchMetadata {};
	const auto* scalar = dispatch.scalar;
	// A fixed GP-only signature never consumes or publishes SIMD state. Leave
	// the guest registers in FEX untouched instead of copying unused XMM data.
	const bool scalar_vector_state = scalar != nullptr &&
	    (scalar->result == HostCallScalarType::Float32 || scalar->result == HostCallScalarType::Float64 ||
	     std::any_of(scalar->arguments.begin(), scalar->arguments.begin() + scalar->arity,
	                 [](HostCallScalarType type) {
	                     return type == HostCallScalarType::Float32 || type == HostCallScalarType::Float64;
	                 }));
	const bool vector_state = scalar != nullptr ? scalar_vector_state :
	    dispatch.variadic != HostCallVariadicKind::None;
	if (vector_state)
	{
		if (simd == nullptr || simd->structSize != sizeof(*simd) || simd->registers == nullptr ||
		    simd->registerCount != 16 || simd->registerBytes != 16 ||
		    (simd->registerStride != 16 && simd->registerStride != 32))
			return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
		for (uint32_t index = 0; index < 16; ++index)
			std::memcpy(state.xmm[index], simd->registers + index * simd->registerStride, 16);
	}
	if (target == reinterpret_cast<uint64_t>(&RelocateHandler))
	{
		report_fex_unresolved_plt(&state);
		return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	}
	{
		ScopedFexHostCallMemoryReader memory_reader(
		    read_fex_host_call_guest_memory);
		if (!run_host_call_impl(target, &state, symbols, true, &dispatch) || state.memory_fault_size != 0)
		{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
			if (state.memory_fault_size != 0)
			{
				__android_log_print(ANDROID_LOG_ERROR, "LumenGuestCpu",
				    "LUMEN_ANDROID_HLE_MEMORY_DENIED_V1 nativeTid=%d target=0x%016" PRIx64
				    " address=0x%016" PRIx64 " size=%u write=%u",
				    static_cast<int>(gettid()), target, state.memory_fault_address,
				    state.memory_fault_size, state.memory_fault_write ? 1u : 0u);
			}
#endif
			return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
		}
	}
#if defined(__APPLE__)
	trace_memory_watch("after", target, state);
#endif
	std::copy_n(state.gpr, std::size(state.gpr), guest_gprs);
	if (vector_state)
	{
		for (uint32_t index = 0; index < 16; ++index)
			std::memcpy(simd->registers + index * simd->registerStride, state.xmm[index], 16);
	}

	if (state.execution_cancel_requested
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	    || GuestExecutionStopRequested()
#endif
	)
		return LUMEN_ANDROID_FEXCORE_HOST_CALL_STOP_V1;

	// Keep the CALL frame in guest execution, as an ordinary x86 RET. This
	// avoids a host lease/copy for each return and retains FEX's call-return
	// prediction. The runtime restores admission before reading the slot.
	// QEMU retains its existing callback contract; provider selection is fixed
	// before the first host call. The selector permits an exact A/B rollback.
	static const bool guest_return = [] {
		const char* value = std::getenv("LUMEN_FEX_HLE_RETURN_GUEST");
		const bool enabled = GuestCpu::Selected() == GuestCpu::Provider::Fex &&
		    value != nullptr && std::strcmp(value, "1") == 0;
		std::fprintf(stderr, "LUMEN_FEX_HLE_RETURN_GUEST_V1 enabled=%u\n", enabled ? 1u : 0u);
		return enabled;
	}();
	if (guest_return) return LUMEN_ANDROID_FEXCORE_HOST_CALL_RETURN_GUEST_V1;

	if (!read_fex_host_call_guest_memory(&state, state.gpr[4], 0,
	                                   next_guest_rip, sizeof(*next_guest_rip)))
		return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	if (*next_guest_rip == 0 ||
	    state.gpr[4] > UINT64_MAX - sizeof(uint64_t))
	{
		return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	}
	guest_gprs[4] = state.gpr[4] + sizeof(uint64_t);
#if defined(__APPLE__)
	trace_host_error(linker != nullptr ? linker->Symbols() : nullptr, target, *next_guest_rip, state);
#endif
	return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
}

#if LUMEN_PLATFORM != LUMEN_PLATFORM_OSX && \
    LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID
uint32_t fex_host_call(void* opaque, uint64_t target, uint64_t* guest_gprs,
	                   uint64_t* next_guest_rip) noexcept
{
	return fex_host_call_impl(opaque, target, guest_gprs, nullptr, next_guest_rip);
}
#else
uint32_t fex_host_call_v2(void* opaque, uint64_t target, uint64_t* guest_gprs,
	                      LumenAndroidFexCoreSimdViewV2* simd, uint64_t* next_guest_rip) noexcept
{
	return fex_host_call_impl(opaque, target, guest_gprs, simd, next_guest_rip);
}
#endif

uint32_t fex_executable_query(void*, uint64_t address,
	                          uint64_t* range_base,
	                          uint64_t* range_size,
	                          uint32_t* writable) noexcept
{
	if (address == 0 || range_base == nullptr || range_size == nullptr ||
	    writable == nullptr)
	{
		return 0;
	}
	Core::VirtualMemory::ManagedRegion region {};
	const auto managed_state =
	    Core::VirtualMemory::QueryManagedRegion(address, &region);
	if (managed_state != Core::VirtualMemory::ManagedRegionState::Visible ||
	    region.begin == 0 || region.begin >= region.end ||
	    address < region.begin || address >= region.end ||
	    !Core::VirtualMemory::IsExecute(region.mode))
	{
#if defined(__ANDROID__)
		// Cache relocation metadata also probes ordinary non-executable data.
		// Bound those diagnostics before native-image/symbol lookup; exhaustion
		// changes neither this denial nor the independent backend fault report.
		static std::atomic<uint32_t> emitted {0};
		auto report = emitted.load(std::memory_order_relaxed);
		do
		{
			if (report >= 64) return 0;
		} while (!emitted.compare_exchange_weak(report, report + 1, std::memory_order_relaxed));
		Dl_info native_info {};
		const bool native_image =
		    dladdr(reinterpret_cast<const void*>(address), &native_info) != 0 &&
		    native_info.dli_fbase != nullptr;
		auto* linker = RuntimeLinker::Instance();
		const auto* symbols = linker != nullptr ? linker->Symbols() : nullptr;
		const auto classification =
		    symbols != nullptr
		        ? symbols->ClassifyHostCallTarget(address)
		        : HostCallRegistryClassification::Unsealed;
		const uint64_t image_base = native_image
		                                ? reinterpret_cast<uint64_t>(native_info.dli_fbase)
		                                : 0;
		std::fprintf(
		    stderr,
		    "FEX executable range denied: address=0x%016" PRIx64
		    " managedState=%u begin=0x%016" PRIx64
		    " end=0x%016" PRIx64 " mode=%u hostClass=%u"
		    " nativeImage=%u imageBase=0x%016" PRIx64
		    " imageOffset=0x%016" PRIx64 " image=%s symbol=%s"
		    " report=%u diagnosticCap=64\n",
		    address, static_cast<unsigned>(managed_state), region.begin,
		    region.end, static_cast<unsigned>(region.mode),
		    static_cast<unsigned>(classification), native_image ? 1u : 0u,
		    image_base, native_image ? address - image_base : 0,
		    native_image && native_info.dli_fname != nullptr
		        ? native_info.dli_fname
		        : "-",
		    native_image && native_info.dli_sname != nullptr
		        ? native_info.dli_sname
		        : "-", report + 1);
#endif
		return 0;
	}
	const bool is_writable =
	    (static_cast<uint32_t>(region.mode) &
	     static_cast<uint32_t>(Core::VirtualMemory::Mode::Write)) != 0;
	if (is_writable)
	{
		// The Android adapter has no upstream Linux write-fault tracker.  Its
		// qualified MTRACK profile therefore accepts immutable RX code only;
		// edits must transition through VirtualMemory and the mutation barrier.
		return 0;
	}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	// Apple Silicon applies permissions at 16 KiB host-page granularity while
	// the guest VM tracks 4 KiB pages. A logically RX page can otherwise remain
	// physically writable because one of its siblings is RW. Validate the host
	// envelope and expose only the validated guest page; subsequent VM changes
	// cross the FEX mutation barrier before publishing new permissions.
	constexpr uint64_t guest_page_size = 4096;
	const uint64_t page_base = address & ~(guest_page_size - 1u);
	if (page_base > UINT64_MAX - guest_page_size)
	{
		return 0;
	}
	const uint64_t page_end = page_base + guest_page_size;
	uint64_t validated_base = region.begin > page_base
	                                    ? region.begin
	                                    : page_base;
	uint64_t validated_end = region.end < page_end
	                                   ? region.end
	                                   : page_end;
	if (validated_base >= validated_end)
	{
		return 0;
	}
	Core::VirtualMemory::CodeReadLease lease;
	if (!Core::VirtualMemory::TryAcquireCodeReadLease(
	        validated_base, validated_end - validated_base, &lease))
	{
		return 0;
	}
	Core::VirtualMemory::ReleaseCodeReadLease(&lease);
	// During BeginFreshCallback only, these outputs define a conservative
	// lifetime watch over the whole logical RX extent. Execution queries never
	// see this flag and still authorize only the physically proven guest page.
	if (g_fresh_guest_callback_code_extent)
	{
		validated_base = region.begin;
		validated_end = region.end;
	}
	*range_base = validated_base;
	*range_size = validated_end - validated_base;
#else
	*range_base = region.begin;
	*range_size = region.end - region.begin;
#endif
	*writable = 0;
	return 1;
}

} // namespace
#endif

namespace {

constexpr size_t GUEST_X64_STACK_SIZE = 2u * 1024u * 1024u;
constexpr size_t GUEST_X64_STACK_ALIGNMENT = 16;
// RunGuestX64 enters guest code as a synthetic SysV callee. Keep its return
// slot and the maximum host-call incoming GP footprint inside the nominal
// stack allocation, so stack_top remains a hard boundary rather than relying
// on allocator-owned bytes above it.
constexpr size_t GUEST_X64_ROOT_RETURN_SLOT_COUNT = 1;
constexpr size_t GUEST_X64_ROOT_STACK_ARGUMENT_COUNT =
	HOST_CALL_SCALAR_MAX_ARITY - HOST_CALL_GP_REGISTER_ARGUMENT_COUNT;
constexpr size_t GUEST_X64_ROOT_FRAME_SLOT_COUNT =
	GUEST_X64_ROOT_RETURN_SLOT_COUNT + GUEST_X64_ROOT_STACK_ARGUMENT_COUNT;
constexpr size_t GUEST_X64_ROOT_FRAME_SIZE =
	GUEST_X64_ROOT_FRAME_SLOT_COUNT * sizeof(uint64_t);

static_assert(GUEST_X64_STACK_SIZE % GUEST_X64_STACK_ALIGNMENT == 0);
static_assert(HOST_CALL_SCALAR_MAX_ARITY >= HOST_CALL_GP_MAX_ARITY);
static_assert(GUEST_X64_ROOT_FRAME_SIZE == 88);
static_assert(GUEST_X64_ROOT_FRAME_SIZE % GUEST_X64_STACK_ALIGNMENT == 8);

// A primary guest thread stack belongs to the process address space: another
// guest thread may dereference a pointer to a still-live local. Nested HLE
// callbacks retain their existing private, synchronous ownership contract.
thread_local uint32_t g_guest_x64_stack_depth = 0;

class GuestX64Stack
{
public:
	GuestX64Stack()
	{
#if defined(__aarch64__) && defined(LUMEN_FEXCORE_RUNTIME) && \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
		bool managed_root = g_guest_x64_stack_depth == 0;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		managed_root = managed_root && !Core::VirtualMemory::CurrentThreadHasMemoryLease();
#endif
		if (managed_root)
		{
			const auto address = Core::VirtualMemory::Alloc(
			    0, GUEST_X64_STACK_SIZE, Core::VirtualMemory::Mode::ReadWrite);
			m_data = reinterpret_cast<uint8_t*>(address);
			m_managed = m_data != nullptr;
		} else
#endif
		{
#if defined(__ANDROID__)
		// Bionic heap allocations may carry a top-byte tag. Guest code relies on
		// stack pointers having clear upper 16 bits and may reuse those bits for
		// metadata, so keep the guest stack outside that allocator.
		void* mapping = ::mmap(nullptr, GUEST_X64_STACK_SIZE,
		                       PROT_READ | PROT_WRITE,
		                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (mapping != MAP_FAILED)
		{
			const auto address = reinterpret_cast<uintptr_t>(mapping);
			if ((address & UINT64_C(0xffff000000000000)) == 0)
			{
				m_data = static_cast<uint8_t*>(mapping);
			} else
			{
				::munmap(mapping, GUEST_X64_STACK_SIZE);
			}
		}
#else
		size_t allocation_alignment = GUEST_X64_STACK_ALIGNMENT;
#if defined(LUMEN_WITH_QEMU_TCG) && defined(LUMEN_FEXCORE_RUNTIME) && defined(__APPLE__)
		// TCG authorizes complete 4 KiB guest pages from this exact live
		// interval; neither end may share a page with allocator-owned padding.
		if (GuestCpu::Selected() == GuestCpu::Provider::QemuTcg)
			allocation_alignment = 4096;
#endif
		m_data = static_cast<uint8_t*>(
		    std::aligned_alloc(allocation_alignment,
		                       GUEST_X64_STACK_SIZE));
#endif
		}
		if (m_data != nullptr)
		{
			EXIT_IF(g_guest_x64_stack_depth == UINT32_MAX);
			++g_guest_x64_stack_depth;
		}
	}

	~GuestX64Stack()
	{
		if (m_data == nullptr) return;
		EXIT_IF(g_guest_x64_stack_depth == 0);
		// Only a primary root takes managed storage. Its provider has returned,
		// so retirement can drain peer readers and invalidate their cached access
		// without upgrading an inherited HLE callback or Core memory borrow.
		if (m_managed)
		{
			if (!Core::VirtualMemory::Free(reinterpret_cast<uint64_t>(m_data)))
				EXIT("Unable to retire primary guest stack");
		} else
		{
#if defined(__ANDROID__)
		::munmap(m_data, GUEST_X64_STACK_SIZE);
#else
		std::free(m_data);
#endif
		}
		--g_guest_x64_stack_depth;
	}

	GuestX64Stack(const GuestX64Stack&) = delete;
	GuestX64Stack& operator=(const GuestX64Stack&) = delete;

	uint8_t* Data() const
	{
		return m_data;
	}

private:
	uint8_t* m_data = nullptr;
	bool m_managed = false;
};

bool initialize_guest_x64_root_stack(uint8_t* stack, size_t stack_size,
	                                 uint64_t* initial_rsp)
{
	if (stack == nullptr || initial_rsp == nullptr ||
	    stack_size < GUEST_X64_ROOT_FRAME_SIZE)
	{
		return false;
	}
	const uintptr_t stack_begin = reinterpret_cast<uintptr_t>(stack);
	if ((stack_begin & UINT64_C(0xffff000000000000)) != 0) return false;
	if (stack_begin > UINTPTR_MAX - stack_size) return false;
	const uintptr_t stack_top = stack_begin + stack_size;
	if (stack_top % GUEST_X64_STACK_ALIGNMENT != 0) return false;

	const uintptr_t root_rsp = stack_top - GUEST_X64_ROOT_FRAME_SIZE;
	std::memset(reinterpret_cast<void*>(root_rsp), 0,
	            GUEST_X64_ROOT_FRAME_SIZE);
	*initial_rsp = static_cast<uint64_t>(root_rsp);
	return true;
}

#if defined(LUMEN_FEXCORE_RUNTIME)
[[gnu::noinline]] void report_fex_guest_failure(
	uint32_t fex_code, const LumenAndroidFexCoreGuestStateV1& fex_state,
	const uint8_t* guest_stack)
{
	Core::VirtualMemory::ManagedRegion fault_region {};
	Core::VirtualMemory::ManagedRegionState fault_region_state =
	    Core::VirtualMemory::ManagedRegionState::Unmanaged;
	Core::VirtualMemory::ManagedRegion rip_region {};
	Core::VirtualMemory::ManagedRegionState rip_region_state =
	    Core::VirtualMemory::ManagedRegionState::Unmanaged;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	const bool fault_region_queried =
	    Core::VirtualMemory::TryQueryManagedRegionSnapshot(
	        fex_state.faultAddress, &fault_region_state, &fault_region);
	const bool rip_region_queried =
	    Core::VirtualMemory::TryQueryManagedRegionSnapshot(
	        fex_state.rip, &rip_region_state, &rip_region);
#else
	const bool fault_region_queried = false;
	const bool rip_region_queried = false;
#endif
	// The guest fault may have abandoned a native HLE while it owned an
	// unrelated mutex or FILE lock. Keep the primary record numeric, bounded
	// and free of linker/stdio locks. A single write minimizes cross-thread
	// splicing, while the repeated thread id keeps every line attributable.
	std::array<char, 8192> fault_report {};
	size_t fault_report_size = 0;
	const auto append = [&](const char* format, auto... args)
	{
		if (fault_report_size >= fault_report.size() - 1) return;
		const size_t remaining = fault_report.size() - fault_report_size;
		int written = 0;
		if constexpr (sizeof...(args) == 0)
		{
			written = std::snprintf(
			    fault_report.data() + fault_report_size, remaining,
			    "%s", format);
		} else
		{
			written = std::snprintf(
			    fault_report.data() + fault_report_size, remaining,
			    format, args...);
		}
		if (written <= 0) return;
		fault_report_size += std::min(
		    static_cast<size_t>(written), remaining - 1);
	};
	const int fault_thread = Core::Thread::GetThreadIdUnique();
	append("FEXCore backend stopped: thread=%d code=%u rip=0x%016" PRIx64
	       " signal=%u fault=0x%016" PRIx64 "\n",
	       fault_thread, fex_code, fex_state.rip, fex_state.faultSignal,
	       fex_state.faultAddress);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	// Keep the harness contract near the front of the bounded record so even a
	// short best-effort write retains the fail-closed disposition.
	append("[MACOS_FEXCORE_FAIL_CLOSED_V1] code=%u exit=74\n", fex_code);
#endif
	append("FEX guest registers: thread=%d rax=%016" PRIx64
	       " rbx=%016" PRIx64 " rcx=%016" PRIx64
	       " rdx=%016" PRIx64 " rsi=%016" PRIx64
	       " rdi=%016" PRIx64 " rsp=%016" PRIx64
	       " rbp=%016" PRIx64 " r10=%016" PRIx64
	       " r11=%016" PRIx64 "\n",
	       fault_thread, fex_state.gprs[0], fex_state.gprs[3],
	       fex_state.gprs[1], fex_state.gprs[2], fex_state.gprs[6],
	       fex_state.gprs[7], fex_state.gprs[4], fex_state.gprs[5],
	       fex_state.gprs[10], fex_state.gprs[11]);
	append("FEX guest extended registers: thread=%d r8=%016" PRIx64
	       " r9=%016" PRIx64 " r12=%016" PRIx64
	       " r13=%016" PRIx64 " r14=%016" PRIx64
	       " r15=%016" PRIx64 "\n",
	       fault_thread, fex_state.gprs[8], fex_state.gprs[9],
	       fex_state.gprs[12], fex_state.gprs[13], fex_state.gprs[14],
	       fex_state.gprs[15]);
	append("FEX guest frame chain: thread=%d", fault_thread);
	const char* const stack_words_value =
	    std::getenv("LUMEN_MACOS_FEXCORE_FAULT_STACK_WORDS");
	const bool include_stack_words =
	    stack_words_value != nullptr && stack_words_value[0] == '1' &&
	    stack_words_value[1] == '\0';
	std::array<uint64_t, 32> frame_addresses {};
	std::array<std::array<uint64_t, 6>, 32> frame_words {};
	std::array<bool, 32> frame_words_available {};
	uint32_t frame_count = 0;
	uint64_t frame = fex_state.gprs[5];
	const uint64_t stack_begin = reinterpret_cast<uint64_t>(guest_stack);
	const uint64_t stack_end = stack_begin + GUEST_X64_STACK_SIZE;
	for (uint32_t depth = 0;
	     depth < 32 && frame >= stack_begin &&
	     frame <= stack_end - 2 * sizeof(uint64_t);
	     ++depth)
	{
		std::array<uint64_t, 2> chain_words {};
		if (!read_diagnostic_memory(frame, chain_words.data(), sizeof(chain_words))) break;
		auto& words = frame_words[depth];
		words[0] = chain_words[0];
		words[1] = chain_words[1];
		if (include_stack_words &&
		    frame <= stack_end - words.size() * sizeof(uint64_t))
		{
			frame_words_available[depth] =
			    read_diagnostic_memory(frame, words.data(), sizeof(words));
		}
		frame_addresses[depth] = frame;
		frame_count = depth + 1;
		append(" %016" PRIx64, chain_words[1]);
		if (chain_words[0] <= frame) break;
		frame = chain_words[0];
	}
	append("\n");
	if (include_stack_words)
	{
		for (uint32_t depth = 0; depth < frame_count; ++depth)
		{
			const auto& words = frame_words[depth];
			if (!frame_words_available[depth])
			{
				append("FEX guest frame words: thread=%d depth=%u fp=0x%016" PRIx64
				       " <unreadable>\n",
				       fault_thread, depth, frame_addresses[depth]);
				continue;
			}
			append("FEX guest frame words: thread=%d depth=%u fp=0x%016" PRIx64
			       " w2=0x%016" PRIx64 " w3=0x%016" PRIx64
			       " w4=0x%016" PRIx64 " w5=0x%016" PRIx64 "\n",
			       fault_thread, depth, frame_addresses[depth], words[2],
			       words[3], words[4], words[5]);
		}
	}
	append("FEX fault region: thread=%d address=0x%016" PRIx64
	       " snapshotAvailable=%u state=%u begin=0x%016" PRIx64
	       " end=0x%016" PRIx64 " mode=%u\n",
	       fault_thread, fex_state.faultAddress,
	       fault_region_queried ? 1U : 0U,
	       static_cast<unsigned>(fault_region_state), fault_region.begin,
	       fault_region.end, static_cast<unsigned>(fault_region.mode));
	append("FEX RIP region: thread=%d address=0x%016" PRIx64
	       " snapshotAvailable=%u state=%u begin=0x%016" PRIx64
	       " end=0x%016" PRIx64 " mode=%u\n",
	       fault_thread, fex_state.rip,
	       rip_region_queried ? 1U : 0U,
	       static_cast<unsigned>(rip_region_state), rip_region.begin,
	       rip_region.end, static_cast<unsigned>(rip_region.mode));
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	// These are raw register observations, not interpreted object fields. A
	// translated block may still hold live SSA values outside its static bank.
	// Capture a fixed window for each callee-saved guest register without
	// following pointers or taking VM/linker locks after a backend failure.
	for (uint32_t reg = 12; reg < 16; ++reg)
	{
		std::array<uint64_t, 32> words {};
		const uint64_t address = fex_state.gprs[reg];
		const bool readable =
		    read_diagnostic_memory(address, words.data(), sizeof(words));
		append("FEX raw register memory: thread=%d register=r%u address=0x%016" PRIx64
		       " rawOnly=1 size=%zu available=%u\n",
		       fault_thread, reg, address, sizeof(words), readable ? 1U : 0U);
		if (!readable) continue;
		for (size_t word = 0; word < words.size(); word += 4)
		{
			append("FEX raw register words: thread=%d register=r%u offset=0x%02zx"
			       " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 "\n",
			       fault_thread, reg, word * sizeof(uint64_t), words[word],
			       words[word + 1], words[word + 2], words[word + 3]);
		}
	}
#endif
	// Optional local observation: repeat the bounded managed-region snapshots
	// recorded at creation (selector LUMEN_MACOS_VM_REGION_SNAPSHOT) and dump
	// the whole managed region that contains the denied instruction pointer, so
	// both can be compared with the bytes the mapping started with. Bounded to
	// small regions and gated by exact selectors; files stay next to the run
	// outputs.
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	(void)Core::VirtualMemory::SnapshotRecordedRegions("fault");
#endif
	{
		static const bool dump_region = [] {
			const char* value =
			    std::getenv("LUMEN_MACOS_FEXCORE_FAULT_REGION_DUMP");
			return value != nullptr && value[0] == '1' && value[1] == '\0';
		}();

		constexpr uint64_t kMaximumRegionDumpSize = 4 * 1024 * 1024;
		if (dump_region && rip_region_queried &&
		    rip_region.end > rip_region.begin &&
		    rip_region.end - rip_region.begin <= kMaximumRegionDumpSize)
		{
			std::array<char, 64> dump_path {};
			std::snprintf(dump_path.data(), dump_path.size(),
			              "_faultregion_%016" PRIx64 ".bin",
			              rip_region.begin);
			if (FILE* file = std::fopen(dump_path.data(), "wb"))
			{
				std::array<uint8_t, 4096> chunk {};
				bool readable = true;
				for (uint64_t offset = 0;
				     offset < rip_region.end - rip_region.begin;
				     offset += chunk.size())
				{
					const uint64_t remaining =
					    rip_region.end - rip_region.begin - offset;
					const size_t block = static_cast<size_t>(
					    remaining < chunk.size() ? remaining : chunk.size());
					if (!read_diagnostic_memory(
					        rip_region.begin + offset, chunk.data(), block))
					{
						readable = false;
						break;
					}
					std::fwrite(chunk.data(), 1, block, file);
				}
				std::fclose(file);
				append("FEX fault region dump: thread=%d file=%s base=0x%016" PRIx64
				       " size=0x%016" PRIx64 " readable=%u\n",
				       fault_thread, dump_path.data(), rip_region.begin,
				       rip_region.end - rip_region.begin, readable ? 1U : 0U);
			}
		}
	}
	// Optional operator-selected guest memory dump. Ranges arrive from the
	// environment as hex address:length pairs, never from guest state, and
	// every read goes through the same guarded helper as the frame walk, so
	// the record stays bounded, read-only and free of linker/stdio locks.
	const char* const dump_ranges =
	    std::getenv("LUMEN_MACOS_FEXCORE_FAULT_MEMORY_DUMP");
	if (dump_ranges != nullptr && dump_ranges[0] != '\0')
	{
		const auto parse_hex = [](const char*& cursor) -> uint64_t
		{
			const char* p = cursor;
			if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
			const char* const start = p;
			uint64_t value = 0;
			for (;; ++p)
			{
				const char c = *p;
				uint32_t digit = 0;
				if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
				else if (c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(c - 'a') + 10;
				else if (c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(c - 'A') + 10;
				else break;
				value = value > (UINT64_MAX >> 4) ? UINT64_MAX : (value << 4) | digit;
			}
			cursor = p;
			return p == start ? UINT64_MAX : value;
		};
		const char* cursor = dump_ranges;
		for (uint32_t range = 0; range < 8 && *cursor != '\0'; ++range)
		{
			// An optional register or frame anchor keeps a range usable when the
			// target image slides between runs: rip+0x1fd411d:8 reads eight
			// bytes at a fixed delta from the faulting instruction pointer, and
			// rax/r12/... name the captured guest registers. The f0..f7 anchors
			// instead reference the return address recorded for that frame depth
			// by the chain walk above, which follows the sliding image that owns
			// the faulting call stack.
			static constexpr struct
			{
				const char* name;
				uint32_t    index;
				bool        instruction_pointer;
			} register_anchors[] = {
			    {"rip", 0, true},  {"rsp", 4, false}, {"rbp", 5, false},
			    {"rax", 0, false}, {"rcx", 1, false}, {"rdx", 2, false},
			    {"rbx", 3, false}, {"rsi", 6, false}, {"rdi", 7, false},
			    {"r8", 8, false},  {"r9", 9, false},  {"r10", 10, false},
			    {"r11", 11, false}, {"r12", 12, false}, {"r13", 13, false},
			    {"r14", 14, false}, {"r15", 15, false},
			};
			uint64_t dump_address = 0;
			bool     anchored = false;
			for (const auto& anchor: register_anchors)
			{
				const size_t length = std::strlen(anchor.name);
				if (std::strncmp(cursor, anchor.name, length) != 0) continue;
				dump_address = anchor.instruction_pointer
				    ? fex_state.rip : fex_state.gprs[anchor.index];
				cursor += length;
				anchored = true;
				break;
			}
			if (!anchored)
			{
				if (cursor[0] == 'f' && cursor[1] >= '0' && cursor[1] <= '7')
				{
					const uint32_t anchor_depth =
					    static_cast<uint32_t>(cursor[1] - '0');
					dump_address = anchor_depth < frame_count
					    ? frame_words[anchor_depth][1] : UINT64_MAX;
					cursor += 2;
				} else
				{
					dump_address = parse_hex(cursor);
				}
			}
			if (*cursor == '+' || *cursor == '-')
			{
				const bool subtract = *cursor == '-';
				++cursor;
				const uint64_t delta = parse_hex(cursor);
				if (delta == UINT64_MAX ||
				    (subtract && delta > dump_address) ||
				    (!subtract && dump_address > UINT64_MAX - delta))
					dump_address = UINT64_MAX;
				else
					dump_address = subtract ? dump_address - delta : dump_address + delta;
			}
			if (*cursor != ':') break;
			++cursor;
			const uint64_t dump_length = parse_hex(cursor);
			if (*cursor == ',') ++cursor;
			if (dump_address == UINT64_MAX || dump_length == UINT64_MAX ||
			    dump_length == 0 || dump_length > 64) continue;
			std::array<uint8_t, 64> dump_bytes {};
			const bool dump_readable =
			    read_diagnostic_memory(dump_address, dump_bytes.data(),
			                           static_cast<size_t>(dump_length));
			append("FEX guest memory: thread=%d address=0x%016" PRIx64 " len=%u bytes=",
			       fault_thread, dump_address, static_cast<unsigned>(dump_length));
			if (!dump_readable)
			{
				append("<unreadable>\n");
				continue;
			}
			for (uint64_t i = 0; i < dump_length; ++i)
				append("%02x", static_cast<unsigned>(dump_bytes[i]));
			append("\n");
		}
	}
	(void)::write(STDERR_FILENO, fault_report.data(), fault_report_size);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	// Default launches retain neither fd2 nor a diagnostic file. Forward the
	// existing bounded record to logcat, without adding guest reads or linker
	// queries. Split long lines so Android's per-record limit cannot hide them.
	for (size_t offset = 0; offset < fault_report_size;)
	{
		const size_t remaining = fault_report_size - offset;
		const auto* newline = static_cast<const char*>(std::memchr(fault_report.data() + offset, '\n', remaining));
		const size_t line_size = newline != nullptr ? static_cast<size_t>(newline - fault_report.data() - offset) : remaining;
		const size_t chunk = std::min(line_size, size_t {3000});
		__android_log_print(ANDROID_LOG_ERROR, "LumenGuestCpu", "%.*s", static_cast<int>(chunk), fault_report.data() + offset);
		offset += chunk + (newline != nullptr && chunk == line_size ? 1u : 0u);
	}
#endif
}
#endif

} // namespace

#if defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)
bool InitializeGuestX64RootStackForTest(uint8_t* stack, size_t stack_size,
	                                    uint64_t* initial_rsp)
{
	return initialize_guest_x64_root_stack(stack, stack_size, initial_rsp);
}

size_t GuestX64StackSizeForTest()
{
	return GUEST_X64_STACK_SIZE;
}

size_t GuestX64RootFrameSizeForTest()
{
	return GUEST_X64_ROOT_FRAME_SIZE;
}
#endif

#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__aarch64__)
// Generated-code boundary delivery for queued guest exceptions. The FEX
// adapter calls this on the target thread inside its pause path, after the SRA
// spill published the complete interrupted state and after any pending VM
// mutation parked this reader, so the handler runs with the state of the
// block boundary rather than the next HLE boundary. The handler itself is an
// ordinary nested guest call.
static std::uint32_t deliver_boundary_guest_exception(const std::uint64_t* gprs, std::uint64_t rip) noexcept
{
	return Libs::LibKernel::PthreadDeliverPendingExceptionAtBoundary(gprs, rip) ? 1U : 0U;
}

static void register_boundary_guest_exception_delivery()
{
	static std::once_flag once;
	std::call_once(once, [] {
		if (LumenAndroidFexCoreRegisterBoundaryDeliveryV1(&deliver_boundary_guest_exception) == 0)
		{
			// The ordinary HLE boundary and host-wait routes stay active.
			std::fputs("FEX boundary exception delivery registration failed\n", stderr);
			std::fflush(stderr);
		}
	});
}
#endif

static uint64_t run_guest_x64_impl(uint64_t addr, uint64_t arg0, uint64_t arg1, uint64_t arg2
#if defined(LUMEN_FEXCORE_RUNTIME)
                                 , FexGuestRunOutcome* outcome
#endif
)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	GuestExecutionScope execution_scope;
	if (GuestExecutionStopRequested())
	{
#if defined(LUMEN_FEXCORE_RUNTIME)
		outcome->callback_failed = true;
#endif
		return 0;
	}
#endif
	// Cached nested roots must not create TLS or errno while an outer HLE
	// retains guest pointers. Prepare every loaded module at native admission.
	bool fresh_callback = false;
#if defined(LUMEN_FEXCORE_RUNTIME)
	fresh_callback = g_fresh_guest_callback_depth != 0 &&
	                 g_fresh_guest_callback_depth == g_fex_guest_run_depth;
	FreshGuestCallbackFrameMutation callback_frame_mutation(fresh_callback);
	if (!callback_frame_mutation.Valid())
	{
		outcome->callback_failed = true;
		return 0;
	}
#endif
	if (!fresh_callback)
	{
		RuntimeLinker::Instance()->PrepareCurrentThreadTls();
		Libs::Posix::PrepareCurrentThreadError();
	}
	GuestX64Stack guest_stack_storage;
	uint8_t* const guest_stack = guest_stack_storage.Data();
	if (guest_stack == nullptr)
	{
		EXIT("Unable to allocate guest x86-64 stack");
		return 0;
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	// The x86-64 allocation below owns this run's RSP; nested runs push
	// their own bounds and restore these on return.
	ScopedGuestRunStackBounds guest_run_stack_bounds(
	    reinterpret_cast<uint64_t>(guest_stack), GUEST_X64_STACK_SIZE);
#endif

	Cpu::X64State state {};
	state.rip    = addr;
#if defined(LUMEN_FEXCORE_RUNTIME)
	state.fs_base = fresh_callback ? g_fresh_guest_callback_tls :
	                               reinterpret_cast<uint64_t>(TlsMainGetAddr());
#else
	state.fs_base = reinterpret_cast<uint64_t>(TlsMainGetAddr());
#endif
	state.gpr[7] = arg0; // RDI
	state.gpr[6] = arg1; // RSI
	state.gpr[2] = arg2; // RDX
	if (!initialize_guest_x64_root_stack(guest_stack, GUEST_X64_STACK_SIZE,
	                                     &state.gpr[4]))
	{
		EXIT("Unable to initialize guest x86-64 root stack frame");
		return 0;
	}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	// Local capture diagnostic: publish the run entry as this thread's initial
	// boundary state so every guest thread is reported from its first run even
	// before its first HLE call. Gated by LUMEN_MACOS_VM_ROOT_SNAPSHOT.
	Libs::LibKernel::KernelGuestThreadPublishBoundaryState(
	    state.gpr, state.rip, 0, reinterpret_cast<uint64_t>(guest_stack),
	    GUEST_X64_STACK_SIZE);
#endif

#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__aarch64__)
	register_boundary_guest_exception_delivery();
#endif

#if defined(LUMEN_FEXCORE_RUNTIME)
	const uint64_t root_return =
	    LumenAndroidFexCoreRootReturnAddressV1();
	if (root_return == 0)
	{
		std::fprintf(stderr,
		             "FEXCore backend rejected RunGuestX64: root return unavailable\n");
		std::fflush(stderr);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		// The standalone macOS FEX product has no alternate x86-64 backend.
		// Returning here would continue the title with an unexecuted guest
		// function and can leave the window loop hung. Keep this route strictly
		// fail-closed and expose the failure to automation.
		std::_Exit(74);
#endif
		return 0;
	}
	std::memcpy(reinterpret_cast<void*>(state.gpr[4]), &root_return,
	            sizeof(root_return));
	// The new callback stack is private native-owned storage. The writer
	// validates the saved parent before any child frame preparation and closes
	// before CPU entry, where the provider rechecks the lifetime once more.
	callback_frame_mutation.Close();
	const GuestX64RunContext run_context {
	    reinterpret_cast<uint64_t>(guest_stack), GUEST_X64_STACK_SIZE, state.fs_base};
	ScopedGuestX64RunContext run_context_scope(run_context);

	LumenAndroidFexCoreGuestStateV1 fex_state {
	    sizeof(LumenAndroidFexCoreGuestStateV1),
	    1,
	    state.rip,
	    {},
	    state.fs_base,
	    state.gs_base,
	    0,
	    0,
	    0,
	};
	std::copy_n(state.gpr, std::size(state.gpr), fex_state.gprs);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	LumenAndroidFexCoreGuestStateV3 root_state {};
	root_state.gp = fex_state;
	root_state.mxcsr = state.mxcsr;
	root_state.x87ControlWord = state.x87_control_word;
	for (size_t index = 0; index != std::size(state.xmm); ++index)
		std::memcpy(root_state.vectorRegisters[index], state.xmm[index], sizeof(state.xmm[index]));
	// GuestX64Stack owns this allocation until the synchronous Run and all
	// nested callbacks return. Nested stacks remain private and need no mapping
	// mutation while an outer HLE retains guest-pointer leases.
	const uint32_t fex_code = LumenMacOSFexCoreRunGuestWithOwnedStackV3(
	    &root_state, fex_host_call_v2, fex_executable_query, outcome,
	    reinterpret_cast<uint64_t>(guest_stack), GUEST_X64_STACK_SIZE,
	    +[](uint64_t address, uint64_t size, uint32_t write,
	        uint64_t owned_begin, uint64_t owned_size) noexcept -> uint32_t {
		    return write <= 1 && Core::VirtualMemory::CheckOwnedStackAccessUnderFexExecutionBarrier(
		        address, size, write != 0, owned_begin, owned_size) ? 1U : 0U;
	    });
	fex_state = root_state.gp;
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	const uint32_t fex_code = [&]() -> uint32_t {
		if (LumenAndroidFexCoreRegisterFreshCallbackDataAccessQueryV1(
		    +[](uint64_t address, uint64_t size, uint32_t write) noexcept -> uint32_t {
		        return write <= 1 && Core::VirtualMemory::CheckGuestDataAccessUnderFexExecutionBarrier(
		            address, size, write != 0) ? 1U : 0U;
		    }) == 0) return LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1;
#if defined(LUMEN_WITH_QEMU_TCG)
		if (GuestCpu::Selected() == GuestCpu::Provider::QemuTcg) {
			if (!GuestCpu::RegisterMemoryAccessQuery(
			    +[](uint64_t address, uint64_t size, uint32_t write) noexcept -> uint32_t {
			        return write <= 1 && Core::VirtualMemory::CheckGuestDataAccessUnderFexExecutionBarrier(
			            address, size, write != 0) ? 1U : 0U;
			    })) return LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1;
			LumenAndroidFexCoreGuestStateV3 root_state {};
			root_state.gp = fex_state;
			root_state.mxcsr = state.mxcsr;
			root_state.x87ControlWord = state.x87_control_word;
			for (size_t index = 0; index != std::size(state.xmm); ++index)
				std::memcpy(root_state.vectorRegisters[index], state.xmm[index], sizeof(state.xmm[index]));
			const auto result = GuestCpu::QemuRunWithOwnedStack(&root_state,
			    fex_host_call_v2, fex_executable_query, outcome,
			    reinterpret_cast<uint64_t>(guest_stack), GUEST_X64_STACK_SIZE,
			    +[](uint64_t address, uint64_t size, uint32_t write,
			        uint64_t base, uint64_t span) noexcept -> uint32_t {
			        return write <= 1 && Core::VirtualMemory::CheckOwnedStackAccessUnderFexExecutionBarrier(
			            address, size, write != 0, base, span) ? 1U : 0U;
			    });
			fex_state = root_state.gp;
			return result;
		}
#endif
		return LumenAndroidFexCoreRunGuestV2(
		    &fex_state, fex_host_call_v2, fex_executable_query, outcome);
	}();
#else
	const uint32_t fex_code = LumenAndroidFexCoreRunGuestV1(
	    &fex_state, fex_host_call, fex_executable_query, outcome);
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	static std::once_flag fex_route_receipt_once;
	std::call_once(fex_route_receipt_once, [fex_code, addr] {
		LumenAndroidFexCoreRuntimeStatusV1 status {};
		const uint32_t runtime_code =
		    LumenAndroidFexCoreRuntimeProbeV1(&status);
		const bool technical_runtime_ready =
		    runtime_code == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
		    status.backendRouteReady != 0;
		const bool rom_route = technical_runtime_ready &&
		    fex_code == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1;
		const char* const selector_value = std::getenv("LUMEN_MACOS_FEXCORE_RUN");
		const char* selector = "malformed";
		if (selector_value == nullptr)
		{
			selector = "absent";
		} else if (selector_value[0] == '\0')
		{
			selector = "empty";
		} else if (std::strcmp(selector_value, "0") == 0 || std::strcmp(selector_value, "1") == 0)
		{
			selector = selector_value;
		}
		std::fprintf(stderr,
		             "[MACOS_FEXCORE_ROUTE_V1] requested=%s effective=%u "
		             "technicalRuntimeReady=%u qualifiedRuntimeReady=0 "
		             "romRoute=%u guestRunCode=%u entry=0x%016" PRIx64 " "
		             "diagnostic=1 fallback=0 "
		             "interpreterRoute=0 nodaRoute=0 runtimeCode=%u "
		             "\n%s\n",
		             selector,
		             rom_route ? 1U : 0U,
		             technical_runtime_ready ? 1U : 0U,
		             rom_route ? 1U : 0U, fex_code, addr, runtime_code,
		             LumenAndroidFexCoreRuntimeReceiptV1());
		std::fflush(stderr);
	});
#endif
	if (fex_code != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1)
	{
		outcome->callback_failed = true;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		// Publish the first genuine backend error before cancellation makes other
		// roots leave cleanly. The launch result must not turn this into PASS 0.
		const bool first_failure = RecordGuestExecutionBackendFailure(fex_code);
		__android_log_print(ANDROID_LOG_ERROR, "LumenGuestCpu",
		    "LUMEN_ANDROID_GUEST_BACKEND_FAILURE_V1 nativeTid=%d entry=0x%016" PRIx64
		    " depth=%u code=%u first=%u stopRequested=%u rip=0x%016" PRIx64,
		    static_cast<int>(gettid()), addr, outcome->depth, fex_code, first_failure ? 1u : 0u,
		    GuestExecutionStopRequested() ? 1u : 0u, fex_state.rip);
#endif
		report_fex_guest_failure(fex_code, fex_state, guest_stack);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		RequestGuestExecutionStop();
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		std::_Exit(74);
#endif
	}
	return fex_state.gprs[0];
#else

	// The translator context below points into this host thread's TLS. Keep
	// construction and every Run synchronous in this RunGuestX64 invocation.
	Cpu::X64Interpreter interpreter(run_host_call, nullptr, resolve_diagnostic_module, print_diagnostic_location,
	                                Config::DiagnosticsEnabled()
#if defined(__APPLE__)
	                                    || host_wait_trace_enabled()
#endif
#if defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	                                , translate_guest_address_with_context, &g_guest_region_cache,
	                                export_guest_memory_regions_with_context,
	                                {Cpu::X64AddressTranslatorReentryCapability::NonReentrant}
#endif
	);
	const char* progress_value = std::getenv("LUMEN_X64_PROGRESS");
	const bool progress_enabled =
	    progress_value != nullptr && progress_value[0] == '1' && progress_value[1] == '\0';
	uint64_t progress_steps = 0;
	auto result = Cpu::X64RunResult::StepLimit;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	// A bounded slice lets the mobile exit request be observed without adding
	// executable memory, signals or platform-private cancellation APIs.
	constexpr uint64_t guest_run_step_budget = 100000;
#else
	constexpr uint64_t guest_run_step_budget = 100000000;
#endif
	while (result == Cpu::X64RunResult::StepLimit
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	       && !GuestExecutionStopRequested()
#endif
	)
	{
		result = interpreter.Run(&state, guest_run_step_budget);
		progress_steps += guest_run_step_budget;
		if (progress_enabled && result == Cpu::X64RunResult::StepLimit)
		{
			std::fprintf(stderr,
			             "Guest interpreter progress: steps=%" PRIu64 " rip=0x%016" PRIx64
			             " rsp=0x%016" PRIx64 " rbp=0x%016" PRIx64 "\n",
			             progress_steps, state.rip, state.gpr[4], state.gpr[5]);
			print_diagnostic_location(state.rip, state.gpr[5]);
			std::fflush(stderr);
		}
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (GuestExecutionStopRequested())
	{
		auto return_value = state.gpr[0];
		return return_value;
	}
#endif
	if (result != Cpu::X64RunResult::Returned)
	{
		auto print_guest_location = [](const char* label, uint64_t address) {
			if (auto* program = Core::Singleton<Loader::RuntimeLinker>::Instance()->FindProgramByAddr(address); program != nullptr)
			{
				std::fprintf(stderr, "%s: %s+0x%016" PRIx64 "\n", label, program->file_name.C_Str(), address - program->base_vaddr);
			}
		};
		std::fprintf(stderr, "x86-64 backend stopped: result=%d, rip=0x%016" PRIx64 "\n", static_cast<int>(result), state.rip);
		if (result == Cpu::X64RunResult::MemoryFault)
		{
			std::fprintf(stderr, "Guest memory fault: %s address=0x%016" PRIx64 " size=%" PRIu32 "\n",
			             state.memory_fault_write ? "write" : "read", state.memory_fault_address, state.memory_fault_size);
			if (state.last_data_read_size != 0)
			{
				std::fprintf(stderr, "Previous guest data read: rip=0x%016" PRIx64 " address=0x%016" PRIx64
				                     " size=%" PRIu32 " value=0x%016" PRIx64 "\n",
				             state.last_data_read_rip, state.last_data_read_address, state.last_data_read_size,
				             state.last_data_read_value);
				Cpu::X64MemoryWriteInfo write {};
				if (Cpu::X64FindLastMemoryWrite(state.last_data_read_address, state.last_data_read_size, &write))
				{
					std::fprintf(stderr, "Last overlapping guest write: rip=0x%016" PRIx64 " address=0x%016" PRIx64
					                     " size=%" PRIu32 " value=0x%016" PRIx64 "\n",
					             write.rip, write.address, write.size, write.value);
					print_guest_location("Last overlapping guest write location", write.rip);
				}
				const uint32_t read_count = state.data_read_index < 8 ? state.data_read_index : 8;
				std::fprintf(stderr, "Recent guest data reads:\n");
				for (uint32_t i = 0; i < read_count; i++)
				{
					const uint32_t index = (state.data_read_index - read_count + i) & 15;
					std::fprintf(stderr, "  rip=0x%016" PRIx64 " address=0x%016" PRIx64 " size=%" PRIu32
					                     " value=0x%016" PRIx64,
					             state.data_read_rip[index], state.data_read_address[index], state.data_read_size[index],
					             state.data_read_value[index]);
					Cpu::X64MemoryWriteInfo recent_write {};
					if (Cpu::X64FindLastMemoryWrite(state.data_read_address[index], state.data_read_size[index], &recent_write))
					{
						std::fprintf(stderr, " last_write_rip=0x%016" PRIx64 " last_write_value=0x%016" PRIx64,
						             recent_write.rip, recent_write.value);
					}
					std::fprintf(stderr, "\n");
				}
			}
		}
		if (auto* program = Core::Singleton<Loader::RuntimeLinker>::Instance()->FindProgramByAddr(state.rip); program != nullptr)
		{
			std::fprintf(stderr, "Guest location: %s+0x%016" PRIx64 "\n", program->file_name.C_Str(), state.rip - program->base_vaddr);
			const char* dump_path = std::getenv("LUMEN_X64_DUMP_IMAGE");
			if (dump_path != nullptr && dump_path[0] != '\0')
			{
				if (auto* dump = std::fopen(dump_path, "wb"); dump != nullptr)
				{
					const size_t written = std::fwrite(reinterpret_cast<const void*>(program->base_vaddr), 1, program->base_size, dump);
					std::fclose(dump);
					std::fprintf(stderr, "Guest image dump: %s base=0x%016" PRIx64 " size=0x%016" PRIx64
					                     " written=0x%016" PRIx64 "\n",
					             dump_path, program->base_vaddr, program->base_size, static_cast<uint64_t>(written));
				}
			}
		}
		std::fprintf(stderr, "Guest registers: rax=%016" PRIx64 " rbx=%016" PRIx64 " rcx=%016" PRIx64 " rdx=%016" PRIx64
		       " rsi=%016" PRIx64 " rdi=%016" PRIx64 " rsp=%016" PRIx64 " rbp=%016" PRIx64 "\n",
		       state.gpr[0], state.gpr[3], state.gpr[1], state.gpr[2], state.gpr[6], state.gpr[7], state.gpr[4], state.gpr[5]);
		std::fprintf(stderr, "Guest registers: r8 =%016" PRIx64 " r9 =%016" PRIx64 " r10=%016" PRIx64 " r11=%016" PRIx64
		                     " r12=%016" PRIx64 " r13=%016" PRIx64 " r14=%016" PRIx64 " r15=%016" PRIx64 "\n",
		             state.gpr[8], state.gpr[9], state.gpr[10], state.gpr[11], state.gpr[12], state.gpr[13], state.gpr[14],
		             state.gpr[15]);
		std::fprintf(stderr, "Guest invocation: entry=%016" PRIx64 " arg0=%016" PRIx64 " arg1=%016" PRIx64 " arg2=%016" PRIx64,
		       addr, arg0, arg1, arg2);
		uint64_t return_address = 0;
		if (state.gpr[5] <= UINT64_MAX - sizeof(uint64_t) &&
		    read_diagnostic_memory(state.gpr[5] + sizeof(uint64_t), &return_address, sizeof(return_address)))
		{
			std::fprintf(stderr, " return=%016" PRIx64, return_address);
		}
		std::fprintf(stderr, "\n");
		std::fprintf(stderr, "Recent guest instructions:");
		const bool instruction_trace_full =
		    state.instruction_trace_index >= 32 || state.instruction_trace[state.instruction_trace_index & 31] != 0;
		const uint32_t instruction_count = state.diagnostic_history_enabled
		                                       ? (instruction_trace_full ? 32 : state.instruction_trace_index)
		                                       : 0;
		for (uint32_t i = 0; i < instruction_count; i++)
		{
			auto index = (state.instruction_trace_index - instruction_count + i) & 31;
			std::fprintf(stderr, " %016" PRIx64, state.instruction_trace[index]);
		}
		std::fprintf(stderr, "\n");
		std::fprintf(stderr, "Recent instruction bytes:\n");
		const uint32_t byte_history_count = instruction_count < 8 ? instruction_count : 8;
		for (uint32_t i = instruction_count - byte_history_count; i < instruction_count; i++)
		{
			auto index = (state.instruction_trace_index - instruction_count + i) & 31;
			auto instruction = state.instruction_trace[index];
			std::fprintf(stderr, "  %016" PRIx64 ":", instruction);
			for (uint32_t byte = 0; byte < 8; byte++)
			{
				uint8_t value = 0;
				if (instruction > UINT64_MAX - byte ||
				    !read_diagnostic_memory(instruction + byte, &value, sizeof(value)))
				{
					std::fprintf(stderr, " <unmapped>");
					break;
				}
				std::fprintf(stderr, " %02x", value);
			}
			std::fprintf(stderr, "\n");
		}
		std::fprintf(stderr, "Current instruction bytes:");
		for (uint32_t byte = 0; byte < 15; byte++)
		{
			uint8_t value = 0;
			if (state.rip > UINT64_MAX - byte ||
			    !read_diagnostic_memory(state.rip + byte, &value, sizeof(value)))
			{
				std::fprintf(stderr, " <unmapped>");
				break;
			}
			std::fprintf(stderr, " %02x", value);
		}
		std::fprintf(stderr, "\n");
		std::fprintf(stderr, "Guest frame chain:");
		uint64_t frame = state.gpr[5];
		const auto stack_begin = reinterpret_cast<uint64_t>(guest_stack);
		const auto stack_end   = stack_begin + GUEST_X64_STACK_SIZE;
		for (uint32_t depth = 0; depth < 32 && frame >= stack_begin && frame <= stack_end - 2 * sizeof(uint64_t);
		     depth++)
		{
			uint64_t next_frame = 0;
			uint64_t return_rip = 0;
			std::memcpy(&next_frame, reinterpret_cast<const void*>(frame), sizeof(next_frame));
			std::memcpy(&return_rip, reinterpret_cast<const void*>(frame + sizeof(uint64_t)), sizeof(return_rip));
			std::fprintf(stderr, " %016" PRIx64, return_rip);
			if (next_frame <= frame)
			{
				break;
			}
			frame = next_frame;
		}
		std::fprintf(stderr, "\n");
		const uint32_t host_call_count = state.host_call_index < 256 ? state.host_call_index : 256;
		std::fprintf(stderr, "Recent guest host calls (excluding mutex lock/unlock):\n");
		for (uint32_t i = 0; i < host_call_count; i++)
		{
			const uint32_t index = (state.host_call_index - host_call_count + i) & 255;
#if defined(__APPLE__)
			Dl_info info {};
			const char* symbol = dladdr(reinterpret_cast<const void*>(state.host_call_target[index]), &info) != 0 &&
			                             info.dli_sname != nullptr
			                         ? info.dli_sname
			                         : "?";
#else
			const char* symbol = "?";
#endif
			if (std::strstr(symbol, "PthreadMutexLock") != nullptr || std::strstr(symbol, "PthreadMutexUnlock") != nullptr) continue;
			std::fprintf(stderr, "  rip=%016" PRIx64 " target=%016" PRIx64 " result=%016" PRIx64
			                     " args=%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64 " %s",
			             state.host_call_rip[index], state.host_call_target[index], state.host_call_result[index],
			             state.host_call_arg0[index], state.host_call_arg1[index], state.host_call_arg2[index], symbol);
			if (std::strstr(symbol, "KernelOpen") != nullptr)
			{
				const auto path_address = state.host_call_arg0[index];
				std::fprintf(stderr, " path=\"");
				bool path_readable = false;
				bool path_terminated = false;
				for (uint32_t byte = 0; byte < 1024 && path_address <= UINT64_MAX - byte; byte++)
				{
					char value = '\0';
					if (!read_diagnostic_memory(path_address + byte, &value, sizeof(value))) break;
					path_readable = true;
					if (value == '\0')
					{
						path_terminated = true;
						break;
					}
					std::fputc(value, stderr);
				}
				if (!path_readable) std::fprintf(stderr, "<unmapped>");
				else if (!path_terminated) std::fprintf(stderr, "...");
				std::fprintf(stderr, "\"");
			}
			std::fprintf(stderr, "\n");
		}
		const uint32_t x87_count = state.x87_instruction_count < 16 ? static_cast<uint32_t>(state.x87_instruction_count) : 16;
		std::fprintf(stderr, "Guest x87 instructions: count=%" PRIu64 " recent:", state.x87_instruction_count);
		for (uint32_t i = 0; i < x87_count; i++)
		{
			const uint32_t index = static_cast<uint32_t>(state.x87_instruction_count - x87_count + i) & 15;
			std::fprintf(stderr, " %016" PRIx64, state.x87_instruction_rip[index]);
		}
		std::fprintf(stderr, "\n");
		EXIT("Native Apple Silicon CPU backend could not continue guest execution");
	}

	auto return_value = state.gpr[0];
	return return_value;
#endif
}

uint64_t RunGuestX64(uint64_t addr, uint64_t arg0, uint64_t arg1, uint64_t arg2)
{
#if defined(LUMEN_FEXCORE_RUNTIME)
	FexGuestRunOutcome outcome;
	uint64_t result = 0;
	{
		FexGuestRunDepthScope depth;
		outcome.depth = g_fex_guest_run_depth;
		result = run_guest_x64_impl(addr, arg0, arg1, arg2, &outcome);
	}
	if (outcome.thread_exit)
	{
		// All guest-run storage and depth scopes are gone before pthread_exit
		// invokes run_thread's cleanup and the host's thread-specific cleanup.
		Libs::LibKernel::PthreadExit(reinterpret_cast<void*>(outcome.exit_value));
	}
	return result;
#else
	return run_guest_x64_impl(addr, arg0, arg1, arg2);
#endif
}

bool TryRunGuestX64WithoutGuestBorrow(uint64_t addr, uint64_t arg0,
                                    uint64_t arg1, uint64_t arg2, uint64_t* result)
{
	if (result == nullptr || addr == 0) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (GuestExecutionStopRequested()) return false;
#endif
#if defined(LUMEN_FEXCORE_RUNTIME)
	if (TryGetActiveHostCallGuestGprs() != nullptr)
	{
		if (g_guest_x64_run_context == nullptr || g_fex_guest_run_depth == UINT32_MAX)
			return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		if (Core::VirtualMemory::CurrentThreadHasMemoryLease()) return false;
#endif
		const auto parent = *g_guest_x64_run_context;
		Core::VirtualMemory::ManagedRegion tls_region {};
		if (parent.tls == 0 ||
		    Core::VirtualMemory::QueryManagedRegion(parent.tls, &tls_region) !=
		        Core::VirtualMemory::ManagedRegionState::Visible ||
		    tls_region.end <= tls_region.begin || parent.tls < tls_region.begin ||
		    parent.tls > UINT64_MAX - 8 || parent.tls + 8 > tls_region.end)
			return false;
		g_fresh_guest_callback_code_extent = true;
		const auto token = LumenAndroidFexCoreBeginFreshGuestCallbackV1(parent.stack_base,
		    parent.stack_size, tls_region.begin, tls_region.end - tls_region.begin);
		g_fresh_guest_callback_code_extent = false;
		if (token == 0) return false;
		const auto parent_depth = g_fresh_guest_callback_depth;
		const auto parent_tls = g_fresh_guest_callback_tls;
		g_fresh_guest_callback_depth = g_fex_guest_run_depth + 1;
		g_fresh_guest_callback_tls = parent.tls;
		FexGuestRunOutcome outcome;
		uint64_t value = 0;
		{
			FexGuestRunDepthScope depth;
			outcome.depth = g_fex_guest_run_depth;
			value = run_guest_x64_impl(addr, arg0, arg1, arg2, &outcome);
		}
		g_fresh_guest_callback_depth = parent_depth;
		g_fresh_guest_callback_tls = parent_tls;
		const bool restored = LumenAndroidFexCoreEndFreshGuestCallbackV1(token) != 0;
		if (!restored || outcome.callback_failed) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		if (GuestExecutionStopRequested()) return false;
#endif
		if (outcome.thread_exit)
			Libs::LibKernel::PthreadExit(reinterpret_cast<void*>(outcome.exit_value));
		*result = value;
		return true;
	}
#endif
	*result = RunGuestX64(addr, arg0, arg1, arg2);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (GuestExecutionStopRequested()) return false;
#endif
	return true;
}
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
static bool windows_guest_memory_readable(DWORD protection)
{
	if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
	const DWORD access = protection & 0xffu;
	return access == PAGE_READONLY || access == PAGE_READWRITE || access == PAGE_WRITECOPY ||
	       access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
}

static bool windows_guest_memory_accessible(uint64_t address, size_t size, bool write)
{
	if (address == 0 || size == 0 || address > UINT64_MAX - size) return false;
	const uint64_t end = address + size;
	for (uint64_t cursor = address; cursor < end;)
	{
		MEMORY_BASIC_INFORMATION region {};
		if (VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) != sizeof(region) ||
		    region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
			return false;

		const DWORD access = region.Protect & 0xffu;
		const bool writable = access == PAGE_READWRITE || access == PAGE_WRITECOPY || access == PAGE_EXECUTE_READWRITE ||
		                      access == PAGE_EXECUTE_WRITECOPY;
		if (!(write ? writable : windows_guest_memory_readable(region.Protect))) return false;

		const auto region_base = reinterpret_cast<uint64_t>(region.BaseAddress);
		if (region_base > UINT64_MAX - region.RegionSize) return false;
		const uint64_t region_end = region_base + region.RegionSize;
		if (region_end <= cursor) return false;
		cursor = region_end < end ? region_end : end;
	}
	return true;
}

static bool windows_guest_snapshot_readable(uint64_t address, size_t size)
{
	// VirtualQuery can walk a large coalesced region even for a tiny read.
	// Query only the requested resident pages, without retaining permissions.
	static const uint64_t page_size = [] {
		SYSTEM_INFO info {};
		GetSystemInfo(&info);
		return static_cast<uint64_t>(info.dwPageSize);
	}();
	if (page_size == 0) return windows_guest_memory_accessible(address, size, false);
	uint64_t page = address - address % page_size;
	uint64_t remaining = (address + size - 1 - page) / page_size + 1;
	PSAPI_WORKING_SET_EX_INFORMATION pages[64] {};
	while (remaining != 0)
	{
		const auto count = static_cast<DWORD>(remaining < 64 ? remaining : 64);
		for (DWORD i = 0; i < count; ++i)
			pages[i].VirtualAddress = reinterpret_cast<void*>(page + i * page_size);
		if (K32QueryWorkingSetEx(GetCurrentProcess(), pages, count * sizeof(pages[0])) == 0)
			return windows_guest_memory_accessible(address, size, false);
		for (DWORD i = 0; i < count; ++i)
		{
			const auto attributes = pages[i].VirtualAttributes;
			// Nonresident pages have no usable Win32Protection. The original
			// region check admits readable committed pages before RPM faults them in.
			if (!attributes.Valid) return windows_guest_memory_accessible(address, size, false);
			if (!windows_guest_memory_readable(static_cast<DWORD>(attributes.Win32Protection))) return false;
		}
		remaining -= count;
		if (remaining != 0) page += count * page_size;
	}
	return true;
}

bool ValidateGuestMemory(uint64_t address, size_t size, bool write)
{
	return windows_guest_memory_accessible(address, size, write);
}

bool ReadGuestMemory(uint64_t address, void* destination, size_t size)
{
	if (destination == nullptr || address == 0 || size == 0 || address > UINT64_MAX - size ||
	    !windows_guest_snapshot_readable(address, size)) return false;
	// Copy under the OS access check, so an inaccessible source cannot fault
	// the host between validation and memcpy. Failed/partial snapshots are unusable.
	SIZE_T copied = 0;
	return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), destination, size, &copied) != 0 &&
	       copied == size;
}

bool WriteGuestMemory(uint64_t address, const void* source, size_t size)
{
	if (source == nullptr || !windows_guest_memory_accessible(address, size, true)) return false;
	std::memcpy(reinterpret_cast<void*>(address), source, size);
	return true;
}
#endif

static LUMEN_SYSV_ABI void run_entry(uint64_t addr, EntryParams* params, atexit_func_t atexit_func)
{
#if defined(__aarch64__)
	RunGuestX64(addr, reinterpret_cast<uint64_t>(params), reinterpret_cast<uint64_t>(atexit_func), 0);
#else
	reinterpret_cast<entry_func_t>(addr)(params, atexit_func);
#endif
}

static LUMEN_SYSV_ABI int run_ini_fini(uint64_t addr, size_t args, const void* argp, module_func_t func)
{
#if defined(__aarch64__)
	return static_cast<int>(RunGuestX64(addr, args, reinterpret_cast<uint64_t>(argp), reinterpret_cast<uint64_t>(func)));
#else
	return reinterpret_cast<module_ini_fini_func_t>(addr)(args, argp, func);
#endif
}

static bool try_get_aligned_segment_size(const Elf64_Phdr* p,
	                                     uint64_t* aligned_size)
{
	if (p == nullptr || aligned_size == nullptr || p->p_filesz > p->p_memsz ||
	    p->p_offset > UINT64_MAX - p->p_filesz)
	{
		return false;
	}

	const uint64_t alignment = p->p_align;
	if (alignment != 0 && (alignment & (alignment - 1)) != 0) return false;
	const uint64_t padding = alignment > 1 ? alignment - 1 : 0;
	if (p->p_memsz > UINT64_MAX - padding) return false;

	const uint64_t size = alignment > 1
	                          ? (p->p_memsz + padding) & ~padding
	                          : p->p_memsz;
	if (p->p_vaddr > UINT64_MAX - size) return false;
	*aligned_size = size;
	return true;
}

static uint64_t get_aligned_size(const Elf64_Phdr* p)
{
	uint64_t aligned_size = 0;
	const bool valid_segment =
	    try_get_aligned_segment_size(p, &aligned_size);
	if (!valid_segment) EXIT("Invalid ELF segment layout\n");
	return aligned_size;
}

static void dbg_dump_symbols(const String& folder, Elf64_Sym* symbols, uint64_t size, const char* names)
{
	auto folder_str = folder.FixDirectorySlash();

	Core::File::CreateDirectories(folder_str);

	Core::File f;
	f.Create(folder_str + "symbols.txt");

	for (auto* sym = symbols; reinterpret_cast<uint8_t*>(sym) < reinterpret_cast<uint8_t*>(symbols) + size; sym++)
	{
		f.Printf("----\n");
		f.Printf("st_name = %" PRIu32 ", %s\n", sym->st_name, names + sym->st_name);
		f.Printf("st_info = 0x%02" PRIx8 "\n", sym->st_info);
		f.Printf("st_other = 0x%02" PRIx8 "\n", sym->st_other);
		f.Printf("st_shndx = 0x%04" PRIx16 "\n", sym->st_shndx);
		f.Printf("st_value = 0x%016" PRIx64 "\n", sym->st_value);
		f.Printf("st_size = %" PRIu64 "\n", sym->st_size);
	}

	f.Close();
}

static void dbg_dump_rela(const String& folder, Elf64_Rela* records, uint64_t size, const char* /*names*/, const char* file_name)
{
	auto folder_str = folder.FixDirectorySlash();

	Core::File::CreateDirectories(folder_str);

	Core::File f;
	f.Create(folder_str + file_name);

	for (auto* r = records; reinterpret_cast<uint8_t*>(r) < reinterpret_cast<uint8_t*>(records) + size; r++)
	{
		f.Printf("----\n"
		         "r_offset = 0x%016" PRIx64 "\n"
		         "r_info = 0x%016" PRIx64 "\n"
		         "r_addend = %" PRId64 "\n",
		         r->r_offset, r->r_info, r->r_addend);
	}

	f.Close();
}

static Core::VirtualMemory::Mode get_mode(Elf64_Word flags)
{
	switch (flags)
	{
		case PF_R: return Core::VirtualMemory::Mode::Read;
		case PF_W: return Core::VirtualMemory::Mode::Write;
		case PF_R | PF_W: return Core::VirtualMemory::Mode::ReadWrite;
		case PF_X: return Core::VirtualMemory::Mode::Execute;
		case PF_X | PF_R: return Core::VirtualMemory::Mode::ExecuteRead;
		case PF_X | PF_W: return Core::VirtualMemory::Mode::ExecuteWrite;
		case PF_X | PF_W | PF_R: return Core::VirtualMemory::Mode::ExecuteReadWrite;

		default: return Core::VirtualMemory::Mode::NoAccess;
	}
}

struct FrameS
{
	FrameS*   next;
	uintptr_t ret_addr;
};

static void LUMEN_SYSV_ABI stackwalk_x86(uint64_t rbp, void** stack, int* depth, uintptr_t stack_addr, size_t stack_size,
                                        uintptr_t code_addr, size_t code_size)
{
	auto* frame = reinterpret_cast<FrameS*>(rbp);

	int d = *depth;
	int i = 0;

	for (; i < d; i++)
	{
		if (!(reinterpret_cast<uintptr_t>(frame) >= stack_addr && reinterpret_cast<uintptr_t>(frame) < stack_addr + stack_size))
		{
			break;
		}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
		if (!ValidateGuestMemory(reinterpret_cast<uint64_t>(frame), sizeof(*frame), false))
		{
			break;
		}
#endif

		if (!(frame->ret_addr >= code_addr && frame->ret_addr < code_addr + code_size))
		{
			break;
		}

		stack[i] = reinterpret_cast<void*>(frame->ret_addr);

		frame = frame->next;
	}

	*depth = i;
}

void LUMEN_SYSV_ABI sys_stack_walk_x86(uint64_t rbp, void** stack, int* depth)
{
	stackwalk_x86(rbp, stack, depth, 0, UINT64_MAX, SYSTEM_RESERVED + CODE_BASE_OFFSET,
	              g_desired_base_addr - (SYSTEM_RESERVED + CODE_BASE_OFFSET));
}

static void lumen_exception_handler(const Core::VirtualMemory::ExceptionHandler::ExceptionInfo* info)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	std::fprintf(stderr,
	             "[GUEST_EXCEPTION] code=0x%08" PRIx32 " type=%u exception=0x%016" PRIx64
	             " rbp=0x%016" PRIx64 " access_type=%u access_address=0x%016" PRIx64 "\n",
	             info->exception_win_code, static_cast<unsigned>(info->type), info->exception_address, info->rbp,
	             static_cast<unsigned>(info->access_violation_type), info->access_violation_vaddr);
	std::fflush(stderr);
#else
	printf("lumen_exception_handler: %016" PRIx64 "\n", info->exception_address);
#endif

	if (info->type == Core::VirtualMemory::ExceptionHandler::ExceptionType::AccessViolation)
	{
		if (info->rbp != 0)
		{
			Core::Singleton<Loader::RuntimeLinker>::Instance()->StackTrace(info->rbp);
		}

		EXIT("Access violation: %s [%016" PRIx64 "] %s\n", Core::EnumName(info->access_violation_type).C_Str(),
		     info->access_violation_vaddr, (info->access_violation_vaddr == g_invalid_memory ? "(Unpatched object)" : ""));
	}

	EXIT("Unknown exception!!! (%08" PRIx32 ")", info->exception_win_code);
}

static void encode_id_64(uint16_t in_id, String* out_id)
{
	static const char32_t* str = U"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
	if (in_id < 0x40u)
	{
		*out_id += str[in_id];
	} else
	{
		if (in_id < 0x1000u)
		{
			*out_id += str[static_cast<uint16_t>(in_id >> 6u) & 0x3fu];
			*out_id += str[in_id & 0x3fu];
		} else
		{
			*out_id += str[static_cast<uint16_t>(in_id >> 12u) & 0x3fu];
			*out_id += str[static_cast<uint16_t>(in_id >> 6u) & 0x3fu];
			*out_id += str[in_id & 0x3fu];
		}
	}
}

template <class T>
static void get_dyn_data_os(Elf64* elf, T* out, Elf64_Sxword tag)
{
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr)
	{
		*out = elf->GetDynamicData<T>(dyn->d_un.d_ptr);
	}
}

template <class T>
static void get_dyn_data(Elf64* elf, uint64_t base_vaddr, T* out, Elf64_Sxword tag)
{
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr)
	{
		*out = reinterpret_cast<T>(base_vaddr + dyn->d_un.d_ptr);
	}
}

template <class T>
static void get_dyn_value(Elf64* elf, T* out, Elf64_Sxword tag)
{
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr)
	{
		*out = dyn->d_un.d_val;
	}
}

template <class T>
static void get_dyn_values(Elf64* elf, T* out, Elf64_Sxword tag)
{
	for (const auto* dyn: elf->GetDynList(tag))
	{
		out->Add(dyn->d_un.d_val);
	}
}

template <class T>
static void get_dyn_ptr(Elf64* elf, T* out, Elf64_Sxword tag)
{
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr)
	{
		*out = dyn->d_un.d_ptr;
	}
}

static void LUMEN_SYSV_ABI ProgramExitHandler()
{
	Core::Singleton<RuntimeLinker>::Instance()->StopAllModules();

	printf("exit!!!\n");
}

static SymbolDatabase* CreateRuntimeSymbolDatabase()
{
	auto* symbols = new SymbolDatabase;
	// This callback is passed directly to the guest entry point rather than
	// exported through LIB_FUNC. Register it before Libs::InitAll seals the
	// authoritative host-call set, and recreate the same invariant on reset.
	if (!symbols->RegisterHostCallTarget(reinterpret_cast<uint64_t>(&ProgramExitHandler)))
	{
		delete symbols;
		EXIT("Unable to register the runtime program-exit callback");
		return nullptr;
	}
#if defined(LUMEN_FEXCORE_RUNTIME)
	// The lazy-PLT fallback is a native runtime target too. FEX must route it
	// through fex_host_call, which reports the unresolved symbol and fails the
	// Run without ever decoding this AArch64 function as guest x86-64.
	if (!symbols->RegisterHostCallTarget(
	        reinterpret_cast<uint64_t>(&RelocateHandler)))
	{
		delete symbols;
		EXIT("Unable to register the FEX unresolved-PLT callback");
		return nullptr;
	}
#endif
	return symbols;
}

template <class T>
static void get_dyn_modules(Elf64* elf, T* out, const char* names, Elf64_Sxword tag)
{
	Vector<uint64_t> needed_modules;
	get_dyn_values(elf, &needed_modules, tag);
	for (auto need: needed_modules)
	{
		ModuleId id {};
		// id.id            = static_cast<int>((need >> 48u) & 0xffffu);
		encode_id_64(static_cast<uint16_t>((need >> 48u) & 0xffffu), &id.id);
		id.version_major = static_cast<int>((need >> 40u) & 0xffu);
		id.version_minor = static_cast<int>((need >> 32u) & 0xffu);
		id.name          = names + (need & 0xffffffff);
		out->Add(id);
	}
}

template <class T>
static void get_dyn_libs(Elf64* elf, T* out, const char* names, Elf64_Sxword tag)
{
	Vector<uint64_t> needed_modules;
	get_dyn_values(elf, &needed_modules, tag);
	for (auto need: needed_modules)
	{
		LibraryId id {};
		// id.id      = static_cast<int>((need >> 48u) & 0xffffu);
		encode_id_64(static_cast<uint16_t>((need >> 48u) & 0xffffu), &id.id);
		id.version = static_cast<int>((need >> 32u) & 0xffffu);
		id.name    = names + (need & 0xffffffff);
		out->Add(id);
	}
}

static RelocationInfo GetRelocationInfo(Elf64_Rela* r, Program* program)
{
	LUMEN_PROFILER_FUNCTION();

	// LUMEN_PROFILER_BLOCK("1");

	RelocationInfo ret;
	// SymbolRecord   sr {};

	// LUMEN_PROFILER_END_BLOCK;

	// LUMEN_PROFILER_BLOCK("2");

	auto         type    = r->GetType();
	auto         symbol  = r->GetSymbol();
	Elf64_Sxword addend  = r->r_addend;
	auto*        symbols = program->dynamic_info->symbol_table;
	auto*        names   = program->dynamic_info->str_table;
	ret.base_vaddr       = program->base_vaddr;
	ret.vaddr            = ret.base_vaddr + r->r_offset;
	ret.bind_self        = false;

	// LUMEN_PROFILER_END_BLOCK;

	// LUMEN_PROFILER_BLOCK("3");

	switch (type)
	{
		case R_X86_64_GLOB_DAT:
		case R_X86_64_JUMP_SLOT: addend = 0; [[fallthrough]];
		case R_X86_64_64:
		{
			auto         sym          = symbols[symbol];
			auto         bind         = sym.GetBind();
			auto         sym_type     = sym.GetType();
			uint64_t     symbol_vaddr = 0;
			SymbolRecord sr {};
			switch (sym_type)
			{
				case STT_NOTYPE: ret.type = SymbolType::NoType; break;
				case STT_FUNC: ret.type = SymbolType::Func; break;
				case STT_OBJECT: ret.type = SymbolType::Object; break;
				default: EXIT("unknown symbol type: %d\n", (int)sym_type);
			}
			switch (bind)
			{
				case STB_LOCAL:
					symbol_vaddr = ret.base_vaddr + sym.st_value;
					ret.bind     = BindType::Local;
					break;
				case STB_GLOBAL: ret.bind = BindType::Global; [[fallthrough]];
				case STB_WEAK:
				{
					ret.bind = (ret.bind == BindType::Unknown ? BindType::Weak : ret.bind);
					ret.name = names + sym.st_name;
					program->rt->Resolve(ret.name, ret.type, program, &sr, &ret.bind_self);
					symbol_vaddr = sr.vaddr;
				}
				break;
				default: EXIT("unknown bind: %d\n", (int)bind);
			}
			ret.resolved = (symbol_vaddr != 0);
			ret.value    = (ret.resolved ? symbol_vaddr + addend : 0);
			ret.name     = sr.name;
			ret.dbg_name = sr.dbg_name;
		}
		break;
		case R_X86_64_RELATIVE:
			ret.value    = ret.base_vaddr + addend;
			ret.resolved = true;
			break;
		case R_X86_64_DTPMOD64:
			ret.value    = reinterpret_cast<uint64_t>(program);
			ret.resolved = true;
			ret.type     = SymbolType::TlsModule;
			ret.bind     = BindType::Local;
			ret.dbg_name = program->file_name;
			break;
		default: EXIT("unknown type: %d\n", (int)type);
	}

	// LUMEN_PROFILER_END_BLOCK;

	return ret;
}

enum class UnresolvedRelocationAction: uint8_t
{
	Fail,
	InvalidObject,
	PreserveExistingTarget,
};

static UnresolvedRelocationAction unresolved_relocation_action(
	SymbolType type, bool jmprela_table, bool weak,
	bool defer_unresolved_plt_functions)
{
	if (type == SymbolType::Object && weak)
		return UnresolvedRelocationAction::InvalidObject;
	if (type == SymbolType::Func && jmprela_table &&
	    (weak || defer_unresolved_plt_functions))
	{
		return UnresolvedRelocationAction::PreserveExistingTarget;
	}
	if ((type == SymbolType::Func && !jmprela_table && weak) ||
	    (type == SymbolType::NoType && weak))
	{
		return UnresolvedRelocationAction::PreserveExistingTarget;
	}
	return UnresolvedRelocationAction::Fail;
}

#if defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)
UnresolvedRelocationActionForTest RuntimeLinkerUnresolvedRelocationActionForTest(
	SymbolType type, bool jmprela_table, bool weak,
	bool defer_unresolved_plt_functions)
{
	return static_cast<UnresolvedRelocationActionForTest>(
	    unresolved_relocation_action(type, jmprela_table, weak,
	                                 defer_unresolved_plt_functions));
}
#endif

static bool relocate(uint32_t index, Elf64_Rela* r, Program* program, bool jmprela_table,
                     bool retry_unresolved_only)
{
	LUMEN_PROFILER_FUNCTION();

	auto ri = GetRelocationInfo(r, program);
	if (retry_unresolved_only && !ri.resolved)
	{
		return false;
	}

	[[maybe_unused]] bool patched = false;

	// LUMEN_PROFILER_BLOCK("patch");

	if (ri.resolved)
	{
		patched = Core::VirtualMemory::PatchReplace(ri.vaddr, ri.value);
	} else
	{
		uint64_t value = 0;
		const bool weak =
		    ri.bind == BindType::Weak || !program->fail_if_global_not_resolved;
		const auto action = unresolved_relocation_action(
		    ri.type, jmprela_table, weak,
		    program->defer_unresolved_plt_functions);
		if (action == UnresolvedRelocationAction::InvalidObject)
		{
			value = g_invalid_memory;
		} else if (action == UnresolvedRelocationAction::PreserveExistingTarget)
		{
			if (ri.type == SymbolType::Func && jmprela_table &&
			    program->custom_call_plt_vaddr != 0)
			{
				EXIT_NOT_IMPLEMENTED(index >= program->custom_call_plt_num);
				value = reinterpret_cast<Jit::CallPlt*>(program->custom_call_plt_vaddr)->GetAddr(index);
			} else
			{
				value = RuntimeLinker::ReadFromElf(program, ri.vaddr) + ri.base_vaddr;
			}
		}

		if (value != 0)
		{
			patched = Core::VirtualMemory::PatchReplace(ri.vaddr, value);
		} else
		{
			auto dbg_str = String::FromPrintf("[%016" PRIx64 "] <- %s%016" PRIx64 "%s, %s, %s, %s, %s", ri.vaddr,
			                                  ri.value == 0 ? FG_BRIGHT_RED : FG_BRIGHT_GREEN, ri.value, DEFAULT, ri.name.C_Str(),
			                                  Core::EnumName(ri.type).C_Str(), Core::EnumName(ri.bind).C_Str(), ri.dbg_name.C_Str());

			EXIT("Can't resolve: %s\n", (Log::IsColoredPrintf() ? dbg_str : Log::RemoveColors(dbg_str)).C_Str());
		}
	}

	// LUMEN_PROFILER_END_BLOCK;

	if (program->dbg_print_reloc)
	{
		if (/* !dbg_str.ContainsStr(U"libc_") && */ patched && !ri.bind_self &&
		    (ri.bind == BindType::Global || ri.bind == BindType::Weak || ri.type == SymbolType::TlsModule))
		{
			auto dbg_str = String::FromPrintf("[%016" PRIx64 "] <- %s%016" PRIx64 "%s, %s, %s, %s, %s", ri.vaddr,
			                                  ri.value == 0 ? FG_BRIGHT_RED : FG_BRIGHT_GREEN, ri.value, DEFAULT, ri.name.C_Str(),
			                                  Core::EnumName(ri.type).C_Str(), Core::EnumName(ri.bind).C_Str(), ri.dbg_name.C_Str());

			printf("Relocate: %s\n", dbg_str.C_Str());
		}
	}

	return ri.resolved;
}

#if defined(LUMEN_FEXCORE_RUNTIME)
static bool relocation_patch_epoch_batch_enabled()
{
	static const bool enabled = Android::FexCoreLab::RelocationPatchEpochRequested(
	    std::getenv("LUMEN_EXPERIMENTAL_FEX_RELOCATION_PATCH_EPOCH_BATCH"));
	return enabled;
}
#endif

static void relocate_all(Elf64_Rela* records, uint64_t size, Program* program, bool jmprela_table,
                         Vector<uint32_t>* unresolved_indices)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(unresolved_indices == nullptr);
	unresolved_indices->Clear();
#if defined(LUMEN_FEXCORE_RUNTIME)
	const bool batch_enabled = relocation_patch_epoch_batch_enabled();
	Android::FexCoreLab::RelocationPatchEpoch epoch(batch_enabled);
#endif
	uint32_t index = 0;
	for (auto* r = records; reinterpret_cast<uint8_t*>(r) < reinterpret_cast<uint8_t*>(records) + size; r++, index++)
	{
#if defined(LUMEN_FEXCORE_RUNTIME)
		// Resolve and patch only: neither operation invokes guest code. Keep the
		// existing per-slot VM publication and permission restoration unchanged.
		if (!epoch.AdmitNext()) EXIT("FEX relocation patch epoch admission denied\n");
#endif
		if (!relocate(index, r, program, jmprela_table, false))
		{
			unresolved_indices->Add(index);
		}
	}
#if defined(LUMEN_FEXCORE_RUNTIME)
	epoch.Close();
	if (batch_enabled && epoch.Epochs() != 0)
		std::fprintf(stderr, "[FEX_RELOCATION_PATCH_EPOCH] table=%s retry=0 attempts=%zu epochs=%zu cap=%zu"
		             " slot-publication=retained local-vm-gate=per-slot\n",
		             jmprela_table ? "JMPREL" : "RELA", epoch.Attempts(), epoch.Epochs(), epoch.MaxAttempts);
#endif
}

static void retry_unresolved(Elf64_Rela* records, Program* program, bool jmprela_table,
                             Vector<uint32_t>* unresolved_indices)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(records == nullptr || unresolved_indices == nullptr);
#if defined(LUMEN_FEXCORE_RUNTIME)
	const bool batch_enabled = relocation_patch_epoch_batch_enabled();
	Android::FexCoreLab::RelocationPatchEpoch epoch(batch_enabled);
#endif
	for (uint32_t position = unresolved_indices->Size(); position > 0; --position)
	{
#if defined(LUMEN_FEXCORE_RUNTIME)
		if (!epoch.AdmitNext()) EXIT("FEX unresolved relocation patch epoch admission denied\n");
#endif
		const uint32_t vector_index = position - 1;
		const uint32_t relocation_index = unresolved_indices->At(vector_index);
		if (relocate(relocation_index, records + relocation_index, program, jmprela_table, true))
		{
			EXIT_IF(!unresolved_indices->RemoveAt(vector_index));
		}
	}
#if defined(LUMEN_FEXCORE_RUNTIME)
	epoch.Close();
	if (batch_enabled && epoch.Epochs() != 0)
		std::fprintf(stderr, "[FEX_RELOCATION_PATCH_EPOCH] table=%s retry=1 attempts=%zu epochs=%zu cap=%zu"
		             " slot-publication=retained local-vm-gate=per-slot\n",
		             jmprela_table ? "JMPREL" : "RELA", epoch.Attempts(), epoch.Epochs(), epoch.MaxAttempts);
#endif
}

static LUMEN_SYSV_ABI void RelocateHandler(RelocateHandlerStack s)
{
	auto*  stack     = s.stack;
	auto*  program   = reinterpret_cast<Program*>(stack[-1]);
	auto   rel_index = stack[0];
	String name      = U"<unknown function>";

	if (program != nullptr && program->dynamic_info != nullptr && program->dynamic_info->jmprela_table != nullptr)
	{
		auto ri = GetRelocationInfo(program->dynamic_info->jmprela_table + rel_index, program);

		name = String::FromPrintf(FG_BRIGHT_RED "%s" DEFAULT, ri.name.C_Str());
	}

	// Restore return address (for stack trace)
	stack[-1] = stack[1];

	Core::Singleton<Loader::RuntimeLinker>::Instance()->StackTrace(reinterpret_cast<uint64_t>(s.stack - 2));

	EXIT("=== Unpatched function!!! ===\n[%d]\t%s\n", Core::Thread::GetThreadIdUnique(),
	     (Log::IsColoredPrintf() ? name : Log::RemoveColors(name)).C_Str());
}

static LUMEN_MS_ABI uint8_t* TlsMainGetAddr()
{
	EXIT_IF(g_tls_main_program == nullptr);

	if (memcmp(&g_tls_reg_save_area[XSAVE_BUFFER_SIZE], &XSAVE_CHK_GUARD, sizeof(XSAVE_CHK_GUARD)) != 0)
	{
		EXIT("xsave buffer is too small\n");
	}

	return RuntimeLinker::TlsGetAddr(g_tls_main_program) + g_tls_main_program->tls.image_size;
}

static void PatchProgram(Program* program, uint64_t address, uint64_t size)
{
	EXIT_IF(program == nullptr);
	EXIT_IF(program->elf == nullptr);

	if (!program->elf->IsShared() && program->tls.handler_vaddr != 0)
	{
#if defined(__aarch64__)
		// The ARM64 CPU backend models FS directly and must retain the original
		// guest instruction instead of installing an x86 host thunk.
		(void)address;
		(void)size;
#else
		// Replace:
		//   mov rax, qword ptr fs:[0x00]
		// with:
		//   call <handler>
		//   mov rax,rax
		//   nop
		// TODO() sometimes prefix 666666 is present
		const uint8_t tls_pattern[9] = {0x64, 0x48, 0x8B, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00};

		EXIT_IF(Jit::Call9::GetSize() != sizeof(tls_pattern));

		auto* start_ptr = reinterpret_cast<uint8_t*>(address);
		auto* end_ptr   = start_ptr + size - sizeof(tls_pattern);

		for (auto* ptr = start_ptr; ptr < end_ptr; ptr++)
		{
			if (memcmp(ptr, tls_pattern, sizeof(tls_pattern)) == 0)
			{
				printf("Patch tls at addr: [%016" PRIx64 "]\n", reinterpret_cast<uint64_t>(ptr));

				auto* code = new (ptr) Jit::Call9;
				code->SetFunc(program->tls.handler_vaddr);
			}
		}
#endif
	}
}

uint64_t RuntimeLinker::GetEntry()
{
	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);

	for (const auto* p: m_programs)
	{
		if (p->elf != nullptr && !p->elf->IsShared())
		{
			return p->elf->GetEntry() + p->base_vaddr;
		}
	}
	return 0;
}

uint64_t RuntimeLinker::GetProcParam()
{
	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);

	for (const auto* p: m_programs)
	{
		if (p->elf != nullptr && !p->elf->IsShared())
		{
			return p->proc_param_vaddr;
		}
	}
	return 0;
}

void RuntimeLinker::DbgDump(const String& folder)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);

	for (const auto* p: m_programs)
	{
		auto folder_str = folder.FixDirectorySlash();
		folder_str += p->file_name.FilenameWithoutDirectory();

		EXIT_IF(p->elf == nullptr);

		p->elf->DbgDump(folder_str);

		if (p->dynamic_info != nullptr)
		{
			EXIT_NOT_IMPLEMENTED(p->dynamic_info->symbol_table_entry_size != 0 &&
			                     p->dynamic_info->symbol_table_entry_size != sizeof(Elf64_Sym));
			EXIT_NOT_IMPLEMENTED(p->dynamic_info->rela_table_entry_size != 0 &&
			                     p->dynamic_info->rela_table_entry_size != sizeof(Elf64_Rela));
			// EXIT_NOT_IMPLEMENTED(p->dynamic_info->jmprela_table == nullptr);
			// EXIT_NOT_IMPLEMENTED(p->dynamic_info->rela_table == nullptr);
			// EXIT_NOT_IMPLEMENTED(p->dynamic_info->symbol_table == nullptr);

			if (p->dynamic_info->symbol_table != nullptr)
			{
				dbg_dump_symbols(folder_str, p->dynamic_info->symbol_table, p->dynamic_info->symbol_table_total_size,
				                 p->dynamic_info->str_table);
			}
			if (p->dynamic_info->jmprela_table != nullptr)
			{
				dbg_dump_rela(folder_str, p->dynamic_info->jmprela_table, p->dynamic_info->jmprela_table_size, p->dynamic_info->str_table,
				              "jmprela_table.txt");
			}
			if (p->dynamic_info->rela_table != nullptr)
			{
				dbg_dump_rela(folder_str, p->dynamic_info->rela_table, p->dynamic_info->rela_table_total_size, p->dynamic_info->str_table,
				              "rela_table.txt");
			}
		}

		if (p->export_symbols != nullptr)
		{
			p->export_symbols->DbgDump(folder_str, U"export_symbols.txt");
		}
		if (p->import_symbols != nullptr)
		{
			p->import_symbols->DbgDump(folder_str, U"import_symbols.txt");
		}
	}
}

void RuntimeLinker::RelocateAll()
{
	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);

	for (auto* p: m_programs)
	{
		if (!p->relocations_applied)
		{
			Relocate(p);
		} else
		{
			RetryUnresolvedRelocations(p);
		}
	}

	m_relocated = true;
}

static bool program_contains_address(Program* program, uint64_t address)
{
	if (program == nullptr || program->elf == nullptr) return false;
	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();
	if (ehdr == nullptr || phdr == nullptr) return false;
	for (Elf64_Half index = 0; index < ehdr->e_phnum; ++index)
	{
		const auto& segment = phdr[index];
		if (segment.p_memsz == 0 ||
		    (segment.p_type != PT_LOAD && segment.p_type != PT_OS_RELRO) ||
		    program->base_vaddr > UINT64_MAX - segment.p_vaddr)
		{
			continue;
		}
		const uint64_t begin = program->base_vaddr + segment.p_vaddr;
		const uint64_t size  = get_aligned_size(&segment);
		if (size <= UINT64_MAX - begin && address >= begin &&
		    address < begin + size)
		{
			return true;
		}
	}
	return false;
}

static bool relocation_table_references_program(
	Elf64_Rela* records, uint64_t size, Program* importing_program,
	Program* provider_program)
{
	if (records == nullptr || importing_program == nullptr ||
	    importing_program->dynamic_info == nullptr ||
	    importing_program->dynamic_info->symbol_table == nullptr)
	{
		return false;
	}
	for (auto* record = records;
	     reinterpret_cast<uint8_t*>(record) <
	         reinterpret_cast<uint8_t*>(records) + size;
	     ++record)
	{
		const auto type = record->GetType();
		if (type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT &&
		    type != R_X86_64_64)
		{
			continue;
		}
		const auto symbol = record->GetSymbol();
		const auto bind =
		    importing_program->dynamic_info->symbol_table[symbol].GetBind();
		if (bind != STB_GLOBAL && bind != STB_WEAK) continue;

		uint64_t target = 0;
		const uint64_t slot = importing_program->base_vaddr + record->r_offset;
		std::memcpy(&target, reinterpret_cast<const void*>(slot),
		            sizeof(target));
		if (program_contains_address(provider_program, target))
		{
			return true;
		}
	}
	return false;
}

bool RuntimeLinker::CanUnloadProgramLocked(Program* program)
{
	if (program == nullptr || program->unload_in_progress ||
	    !m_programs.Contains(program))
	{
		return false;
	}

	for (auto* candidate: m_programs)
	{
		if (candidate == program || candidate->dynamic_info == nullptr) continue;
		if (relocation_table_references_program(
		        candidate->dynamic_info->rela_table,
		        candidate->dynamic_info->rela_table_total_size, candidate,
		        program) ||
		    relocation_table_references_program(
		        candidate->dynamic_info->jmprela_table,
		        candidate->dynamic_info->jmprela_table_size, candidate,
		        program))
		{
			return false;
		}
	}
	return true;
}

bool RuntimeLinker::StopUnloadProgram(Program* program, size_t args,
	const void* argp, module_func_t func, int* result)
{
	// Publish the transition while holding the linker lock, then release it
	// before guest DT_FINI so a finalizer can wait for threads using the linker.
	{
		Core::LockGuard lock(m_mutex);
		if (!CanUnloadProgramLocked(program)) return false;
		program->unload_in_progress = true;
	}

	const int stop_result = StopModule(program, args, argp, func);
	if (result != nullptr) *result = stop_result;
	if (stop_result < 0)
	{
		Core::LockGuard lock(m_mutex);
		if (m_programs.Contains(program)) program->unload_in_progress = false;
		return true;
	}

	Core::LockGuard lock(m_mutex);
	const auto index = m_programs.Find(program);
	EXIT_IF(!m_programs.IndexValid(index));
	DeleteProgram(m_programs.At(index));
	m_programs.RemoveAt(index);
	if (m_relocated) RelocateAll();
	return true;
}

static RuntimeLinker* g_runtime_linker = nullptr;

RuntimeLinker::RuntimeLinker(): m_symbols(CreateRuntimeSymbolDatabase())
{
	EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());
	EXIT_NOT_IMPLEMENTED(g_runtime_linker != nullptr);
	g_runtime_linker = this;
#if defined(LUMEN_FEXCORE_RUNTIME)
	// Resolve against the live singleton each time: temporary test databases
	// and session resets must never leave a borrowed metadata pointer behind.
	EXIT_IF(!GuestCpu::RegisterHostCallEffectsQuery(+[](uint64_t target) noexcept -> uint32_t {
		auto* linker = RuntimeLinker::Instance();
		const auto* symbols = linker != nullptr ? linker->Symbols() : nullptr;
		if (symbols == nullptr || !symbols->HostCallRegistrySealed())
			return GuestCpu::EffectMask(GuestCpu::HostCallEffect::Unknown);
		const auto dispatch = symbols->ResolveHostCall(target);
		return GuestCpu::EffectMask(dispatch.classification == HostCallRegistryClassification::Host
		    ? dispatch.effects : GuestCpu::HostCallEffect::Unknown);
	}));
#endif
}

RuntimeLinker::~RuntimeLinker()
{
	Clear();
	g_runtime_linker = nullptr;
}

RuntimeLinker* RuntimeLinker::Instance()
{
	return g_runtime_linker;
}

bool RuntimeLinker::GuestExceptionRoutingReady() const
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
	return std::any_of(m_programs.begin(), m_programs.end(),
	                   [](const Program* program) { return program != nullptr && program->exception_handler_ready; });
#else
	return false;
#endif
}

uint64_t RuntimeLinker::FindExport(const SymbolResolve& symbol)
{
	Core::LockGuard lock(m_mutex);
	for (const auto* program: m_programs)
	{
		if (program->export_symbols != nullptr)
		{
			if (const auto* record = program->export_symbols->Find(symbol); record != nullptr)
			{
				return record->vaddr;
			}
		}
	}
	return 0;
}

bool RuntimeLinker::FindProgramExportByNid(int32_t handle, const String& nid, uint64_t* vaddr)
{
	Core::LockGuard lock(m_mutex);

	Program* program = nullptr;
	if (handle == 0)
	{
		// Handle 0 is the main executable, which is loaded first by every
		// launch flow.
		program = m_programs.IsEmpty() ? nullptr : m_programs.At(0);
	} else
	{
		// Flat scan under the current lock; FindProgramById locks internally.
		for (auto* p: m_programs)
		{
			if (p->unique_id == handle)
			{
				program = p;
				break;
			}
		}
	}
	if (program == nullptr || program->export_symbols == nullptr || vaddr == nullptr)
	{
		return false;
	}
	const auto* record = program->export_symbols->FindByNid(nid);
	if (record == nullptr)
	{
		return false;
	}
	*vaddr = record->vaddr;
	return true;
}

Program* RuntimeLinker::LoadProgram(const String& elf_name, bool defer_unresolved_plt_functions)
{
	Core::LockGuard lock(m_mutex);
	return LoadProgramLocked(elf_name, defer_unresolved_plt_functions);
}

// The console loader keeps one live instance per module file, like dlopen.
// A resolved file that is already mapped is matched case-insensitively so a
// duplicate guest load cannot create a second copy of the same module. The
// comparison follows the loader's own resolved file name rather than the
// guest spelling, and modules mapped by the launcher are found the same way.
Program* RuntimeLinker::FindLoadedProgramByFileNameLocked(const String& elf_name) const
{
	for (auto* program: m_programs)
	{
		if (program == nullptr || program->file_name.IsEmpty()) continue;
		if (program->file_name.Size() != elf_name.Size()) continue;
		if (program->file_name.StartsWith(elf_name, String::Case::Insensitive)) return program;
	}
	return nullptr;
}

RuntimeLinker::ModuleLoadAdmission RuntimeLinker::AcquireModuleLoadReference(const String& elf_name,
                                                                            bool defer_unresolved_plt_functions)
{
	Core::LockGuard lock(m_mutex);

	if (auto* existing = FindLoadedProgramByFileNameLocked(elf_name); existing != nullptr)
	{
		++existing->load_reference_count;
		return { existing, true };
	}

	auto* program = LoadProgramLocked(elf_name, defer_unresolved_plt_functions);
	program->load_reference_count = 1;
	return { program, false };
}

bool RuntimeLinker::ReleaseModuleLoadReference(Program* program)
{
	EXIT_IF(program == nullptr);

	Core::LockGuard lock(m_mutex);

	if (!m_programs.Contains(program)) return true;

	// Modules mapped outside the guest loader hold no guest reference: keep
	// the historical stop-and-unload outcome for them. Otherwise the module
	// stays loaded and started while another guest reference is live; the
	// stored count is left untouched when the stop is not attempted or is
	// refused, so a later unload can retry it.
	if (program->load_reference_count <= 1) return true;

	--program->load_reference_count;
	return false;
}

Program* RuntimeLinker::LoadProgramLocked(const String& elf_name, bool defer_unresolved_plt_functions)
{
	LUMEN_PROFILER_FUNCTION();

	static int32_t id_seq = 0;

	printf("Loading: %s\n", elf_name.C_Str());

	auto* program = new Program;

	program->rt        = this;
	program->file_name = elf_name;
	program->unique_id = ++id_seq;
	program->defer_unresolved_plt_functions = defer_unresolved_plt_functions;

	program->elf = new Elf64;
	program->elf->Open(elf_name);

	if (program->elf->IsValid())
	{
		LoadProgramToMemory(program);
		ParseProgramDynamicInfo(program);
		CreateSymbolDatabase(program);
	} else
	{
		EXIT("elf is not valid: %s\n", elf_name.C_Str());
	}

	m_programs.Add(program);

	if (!program->elf->IsShared())
	{
		program->fail_if_global_not_resolved = false;
		Libs::LibKernel::SetProgName(elf_name.FilenameWithoutDirectory());
	}

	if (/*elf_name.FilenameWithoutExtension().EndsWith(U"libc") || elf_name.FilenameWithoutExtension().EndsWith(U"Fios2") ||
	    elf_name.FilenameWithoutExtension().EndsWith(U"Fios2_debug") || elf_name.FilenameWithoutExtension().EndsWith(U"NpToolkit") ||
	    elf_name.FilenameWithoutExtension().EndsWith(U"NpToolkit2") || elf_name.FilenameWithoutExtension().EndsWith(U"JobManager")*/
	    elf_name.DirectoryWithoutFilename().EndsWith(U"_module/", String::Case::Insensitive))
	{
		program->fail_if_global_not_resolved = false;
	}

	return program;
}

void RuntimeLinker::SaveMainProgram(const String& elf_name)
{
	EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);

	for (const auto* p: m_programs)
	{
		EXIT_IF(p->elf == nullptr);

		if (!p->elf->IsShared())
		{
			p->elf->Save(elf_name);
			break;
		}
	}
}

void RuntimeLinker::SaveProgram(Program* program, const String& elf_name)
{
	EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);

	if (auto index = m_programs.Find(program); m_programs.IndexValid(index))
	{
		EXIT_IF(m_programs.At(index)->elf == nullptr);

		m_programs.At(index)->elf->Save(elf_name);
	} else
	{
		EXIT("program not found");
	}
}

static EntryParams* prepare_entry_params(const char* name)
{
	if (name == nullptr) return nullptr;
	constexpr uint64_t page_size = 16 * 1024;
	const auto name_size = std::strlen(name);
	if (name_size > UINT64_MAX - sizeof(EntryParams) - page_size) return nullptr;
	const auto bytes = sizeof(EntryParams) + name_size + 1;
	const auto allocation_size = (bytes + page_size - 1) & ~(page_size - 1);
	const auto address = Core::VirtualMemory::Alloc(0, allocation_size, Core::VirtualMemory::Mode::ReadWrite);
	if (address == 0) return nullptr;
	auto* params = reinterpret_cast<EntryParams*>(address);
	bool initialized = false;
	{
#if defined(__aarch64__)
		Core::VirtualMemory::GuestDataLease lease;
		if (Core::VirtualMemory::TryAcquireGuestDataLease(address, bytes, Core::VirtualMemory::Mode::Write, &lease))
#endif
		{
			EntryParams initial {};
			initial.argc = 1;
			initial.argv[0] = reinterpret_cast<const char*>(address + sizeof(EntryParams));
			std::memcpy(params, &initial, sizeof(initial));
			std::memcpy(reinterpret_cast<void*>(address + sizeof(EntryParams)), name, name_size + 1);
			initialized = true;
		}
	}
	if (!initialized)
	{
		EXIT_IF(!Core::VirtualMemory::Free(address));
		return nullptr;
	}
	return params;
}

#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
uint64_t PrepareEntryParametersForTest(const char* name)
{
	return reinterpret_cast<uint64_t>(prepare_entry_params(name));
}
#endif

void RuntimeLinker::Execute()
{
	LUMEN_PROFILER_THREAD("Thread_Main");
#if defined(LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY)
	// Phase-6AL/v2 scopes its election to this deterministic Execute-thread
	// TLS stream. Mark it before module initializers or any guest CPU entry can
	// lazily construct the per-thread dynarec cache.
	Cpu::X64MarkA64DynarecPhase6ALPrimaryThread();
#endif
	Lumen::Emulator::StartupTrace::Emit("guest", "execute.begin");
#if defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE)
	Cpu::X64PrepareA64DynarecHostPcProfile();
#endif

	Libs::LibKernel::PthreadInitSelfForMainThread();
	Lumen::Emulator::StartupTrace::Emit("guest", "pthread.ready");

	Lumen::Emulator::StartupTrace::Emit("guest", "relocate.begin");
	RelocateAll();
	Lumen::Emulator::StartupTrace::Emit("guest", "relocate.end");
	Lumen::Emulator::StartupTrace::Emit("guest", "modules.begin");
	StartAllModules();
	Lumen::Emulator::StartupTrace::Emit("guest", "modules.end");

	printf(FG_BRIGHT_YELLOW "---" DEFAULT "\n");
	printf(FG_BRIGHT_YELLOW "--- Execute: " BOLD BG_BLUE "%s" BG_DEFAULT NO_BOLD DEFAULT "\n", "Main");
	printf(FG_BRIGHT_YELLOW "---" DEFAULT "\n");

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	// Reserve some stack. There may be jumps over guard page. To prevent segfault we need to expand committed area.

	size_t expanded_size = 0;
	size_t expanded_max  = static_cast<size_t>(256) * 1024;

	while (expanded_size < expanded_max)
	{
		sys_dbg_stack_info_t s {};
		sys_stack_usage(s);
		*reinterpret_cast<uint32_t*>(s.guard_addr) = 0;
		expanded_size += s.guard_size;
	}
#endif

	if (auto entry = GetEntry(); entry != 0)
	{
		// Every argv cell and its string must be guest-owned memory. Guest
		// threads may retain argv after entry returns, so retire it at session
		// teardown with the other mappings, after all guest roots have drained.
		const auto entry_name = m_programs.At(0)->file_name.utf8_str();
		EXIT_IF(m_entry_params != 0);
		auto* params = prepare_entry_params(entry_name.GetData());
		EXIT_IF(params == nullptr);
		m_entry_params = reinterpret_cast<uint64_t>(params);

		printf("entry_params_addr = %" PRIx64 "\n", m_entry_params);

		Lumen::Emulator::StartupTrace::Emit("guest", "entry.begin");
#if defined(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY)
		Cpu::X64BeginA64DynarecStartupTelemetry();
#endif
#if defined(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
		Cpu::X64BeginA64DynarecPhase6AFTelemetry();
#endif
#if defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE)
		Cpu::X64BeginA64DynarecHostPcProfile();
#endif
		run_entry(entry, params, ProgramExitHandler);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		__android_log_print(ANDROID_LOG_INFO, "LumenGuestCpu",
		    "LUMEN_ANDROID_GUEST_ENTRY_RETURN_V1 nativeTid=%d entry=0x%016" PRIx64 " stopRequested=%u backendCode=%u",
		    static_cast<int>(gettid()), entry, GuestExecutionStopRequested() ? 1u : 0u,
		    GuestExecutionBackendFailure());
#endif
		Lumen::Emulator::StartupTrace::Emit("guest", "entry.end");
	}
	Lumen::Emulator::StartupTrace::Emit("guest", "execute.end");
}

void RuntimeLinker::Clear()
{
	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);
	ClearLocked(false);
}

bool RuntimeLinker::ResetForNextSession()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (!GuestExecutionIsIdle())
	{
		return false;
	}
#endif

	Core::LockGuard lock(m_mutex);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (!GuestExecutionIsIdle())
	{
		return false;
	}
#endif

	ClearLocked(true);
	return true;
}

void RuntimeLinker::ClearLocked(bool rebuild_symbols)
{
	// Invalidate the process-wide TLS owner before unmapping any Program it can
	// reference. The caller is responsible for ensuring no guest callback can
	// enter while this state is being rebuilt.
	g_tls_main_program = nullptr;
	g_tls_spinlock     = 0;
	std::memset(g_tls_reg_save_area, 0, XSAVE_BUFFER_SIZE);
	std::memcpy(&g_tls_reg_save_area[XSAVE_BUFFER_SIZE], &XSAVE_CHK_GUARD, sizeof(XSAVE_CHK_GUARD));

	if (m_entry_params != 0)
	{
		EXIT_IF(!Core::VirtualMemory::Free(m_entry_params));
		m_entry_params = 0;
	}
	for (auto* p: m_programs)
	{
		DeleteProgram(p);
	}
	m_programs.Clear();
	delete m_symbols;
	m_symbols   = rebuild_symbols ? CreateRuntimeSymbolDatabase() : nullptr;
	m_relocated = false;
	m_heap_api  = nullptr;

	// Program mappings are gone, so a following session can request the same
	// deterministic address range. g_invalid_memory intentionally remains a
	// process-lifetime no-access sentinel.
	g_desired_base_addr = SYSTEM_RESERVED + CODE_BASE_OFFSET;
}

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool RuntimeLinkerSessionResetContractTest()
{
	RuntimeLinker linker;
	const uint64_t original_invalid_memory = g_invalid_memory;
	const bool authoritative_registry =
	    AuthoritativeHostCallRegistryEnabled();
#if defined(LUMEN_FEXCORE_RUNTIME)
	// FEX also registers RelocateHandler so an unresolved lazy PLT entry is
	// reported through the host bridge instead of decoded as guest x86-64.
	constexpr uint32_t builtin_host_targets = 2u;
#else
	constexpr uint32_t builtin_host_targets = 1u;
#endif
	const uint32_t initial_host_targets =
	    authoritative_registry ? builtin_host_targets : 0u;
	const uint32_t registered_host_targets =
	    authoritative_registry ? builtin_host_targets + 1u : 0u;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	RequestGuestExecutionStop();
	GuestExecutionActivityBegin();
	const bool accepted_while_guest_active = linker.ResetForNextSession();
	GuestExecutionActivityEnd();
	if (accepted_while_guest_active)
	{
		return false;
	}
#endif

	for (uint64_t cycle = 1; cycle <= 2; cycle++)
	{
		const String marker = String::FromPrintf("runtime-session-%" PRIu64, cycle);
		linker.Symbols()->AddByStdName(marker, cycle);
		const uint64_t host_target = UINT64_C(0xfeed000000000000) + cycle;
		if (linker.Symbols()->FindByStdName(marker) == nullptr ||
		    linker.Symbols()->HostCallTargetCount() != initial_host_targets ||
		    !linker.Symbols()->RegisterHostCallTarget(host_target) ||
		    linker.Symbols()->HostCallTargetCount() != registered_host_targets)
		{
			return false;
		}
		linker.Symbols()->SealHostCallRegistry();
		const auto expected_runtime_target = authoritative_registry
		                                         ? HostCallRegistryClassification::Host
		                                         : HostCallRegistryClassification::Unsealed;
		const auto expected_unknown_target = authoritative_registry
		                                         ? HostCallRegistryClassification::Guest
		                                         : HostCallRegistryClassification::Unsealed;
		if (linker.Symbols()->HostCallRegistrySealed() != authoritative_registry ||
		    linker.Symbols()->ClassifyHostCallTarget(reinterpret_cast<uint64_t>(&ProgramExitHandler)) !=
		        expected_runtime_target ||
		    linker.Symbols()->ClassifyHostCallTarget(host_target) !=
		        expected_runtime_target ||
		    linker.Symbols()->ClassifyHostCallTarget(cycle) !=
		        expected_unknown_target)
		{
			return false;
		}

		g_desired_base_addr += CODE_BASE_INCR * cycle;
		g_tls_main_program = reinterpret_cast<Program*>(static_cast<uintptr_t>(cycle));
		g_tls_spinlock     = static_cast<uint8_t>(cycle);
		g_invalid_memory   = INVALID_MEMORY + cycle;
		std::memset(g_tls_reg_save_area, static_cast<int>(cycle), sizeof(g_tls_reg_save_area));
		linker.m_relocated = true;
		linker.m_heap_api  = reinterpret_cast<HeapAPI*>(static_cast<uintptr_t>(cycle));
		linker.m_entry_params = reinterpret_cast<uint64_t>(prepare_entry_params("runtime-session"));
		if (linker.m_entry_params == 0) return false;
		const auto retired_entry_params = linker.m_entry_params;
		Core::VirtualMemory::ManagedRegion entry_region {};

		if (!linker.ResetForNextSession() || linker.Symbols() == nullptr || linker.Symbols()->FindByStdName(marker) != nullptr ||
		    linker.Symbols()->HostCallRegistrySealed() ||
		    linker.Symbols()->HostCallTargetCount() != initial_host_targets ||
		    linker.Symbols()->ClassifyHostCallTarget(reinterpret_cast<uint64_t>(&ProgramExitHandler)) !=
		        HostCallRegistryClassification::Unsealed ||
		    linker.Symbols()->ClassifyHostCallTarget(host_target) != HostCallRegistryClassification::Unsealed ||
		    g_desired_base_addr != SYSTEM_RESERVED + CODE_BASE_OFFSET || g_tls_main_program != nullptr || g_tls_spinlock != 0 ||
		    g_invalid_memory != INVALID_MEMORY + cycle || linker.m_relocated || linker.m_heap_api != nullptr ||
		    linker.m_entry_params != 0 || Core::VirtualMemory::QueryManagedRegion(retired_entry_params, &entry_region) ==
		        Core::VirtualMemory::ManagedRegionState::Visible ||
		    linker.m_programs.Size() != 0 || g_tls_reg_save_area[0] != 0 || g_tls_reg_save_area[XSAVE_BUFFER_SIZE - 1] != 0 ||
		    std::memcmp(&g_tls_reg_save_area[XSAVE_BUFFER_SIZE], &XSAVE_CHK_GUARD, sizeof(XSAVE_CHK_GUARD)) != 0)
		{
			return false;
		}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		if (!GuestExecutionStopRequested())
		{
			return false;
		}
#endif
	}

	g_invalid_memory = original_invalid_memory;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	ResetGuestExecutionStop();
#endif
	return true;
}
#endif

void RuntimeLinker::SetHeapAPI(void* func[])
{
	EXIT_IF(func == nullptr);
	m_heap_api = reinterpret_cast<HeapAPI*>(func);
}

namespace {

bool LibcMemoryGuestSelector(const char* value)
{
	return value == nullptr || (value[0] == '1' && value[1] == '\0');
}

bool LibcMemsetGuestEnabled()
{
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__aarch64__) && \
	(LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX)
	static const bool enabled = LibcMemoryGuestSelector(std::getenv("LUMEN_EXPERIMENTAL_FEX_LIBC_MEMSET_GUEST"));
	return enabled;
#else
	return false;
#endif
}

bool LibcMemcpyGuestEnabled()
{
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__aarch64__) && \
	(LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX)
	static const bool enabled = LibcMemoryGuestSelector(std::getenv("LUMEN_EXPERIMENTAL_FEX_LIBC_MEMCPY_GUEST"));
	return enabled;
#else
	return false;
#endif
}

bool LibcStringCompareGuestEnabled()
{
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__aarch64__) && \
	(LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX)
	static const bool enabled = LibcMemoryGuestSelector(std::getenv("LUMEN_EXPERIMENTAL_FEX_LIBC_STRING_COMPARE_GUEST"));
	return enabled;
#else
	return false;
#endif
}

bool LibcMemcmpGuestEnabled()
{
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__aarch64__) && \
	(LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX)
	static const bool enabled = LibcMemoryGuestSelector(std::getenv("LUMEN_EXPERIMENTAL_FEX_LIBC_MEMCMP_GUEST"));
	return enabled;
#else
	return false;
#endif
}

bool IsLibcNamespace(const SymbolResolve& symbol)
{
	return symbol.library == U"libc" && symbol.module == U"libc";
}

bool PreferGuestLibcMemset(const SymbolResolve& symbol, bool enabled)
{
	return enabled && symbol.type == SymbolType::Func && symbol.name == U"8zTFvBIAIN8" &&
	       IsLibcNamespace(symbol);
}

bool PreferGuestLibcMemcpy(const SymbolResolve& symbol, bool enabled)
{
	return enabled && symbol.type == SymbolType::Func && symbol.name == U"Q3VBxCXhUHs" &&
	       IsLibcNamespace(symbol);
}

bool PreferGuestLibcStringCompare(const SymbolResolve& symbol, bool enabled)
{
	return enabled && symbol.type == SymbolType::Func &&
	       (symbol.name == U"Ovb2dSJOAuE" || symbol.name == U"aesyjrHVWy4") && IsLibcNamespace(symbol);
}

bool PreferGuestLibcMemcmp(const SymbolResolve& symbol, bool enabled)
{
	return enabled && symbol.type == SymbolType::Func && symbol.name == U"DfivPArhucg" && IsLibcNamespace(symbol);
}

bool PreferGuestLibcMemory(const SymbolResolve& symbol, bool memset_enabled, bool memcpy_enabled,
	                       bool string_compare_enabled, bool memcmp_enabled)
{
	return PreferGuestLibcMemset(symbol, memset_enabled) || PreferGuestLibcMemcpy(symbol, memcpy_enabled) ||
	       PreferGuestLibcStringCompare(symbol, string_compare_enabled) || PreferGuestLibcMemcmp(symbol, memcmp_enabled);
}

struct ResolvedSymbolCandidate
{
	const SymbolRecord* record    = nullptr;
	bool                bind_self = false;
	bool                guest     = false;
};

// Keep the provider lookup lazy: ordinary ARM64 libc HLE hits do not inspect
// loaded modules. Every lookup retains the complete SymbolResolve identity.
template <typename GuestLookup>
ResolvedSymbolCandidate ResolveSymbolProviders(const SymbolResolve& symbol, bool hle_first,
                                               SymbolDatabase* hle, GuestLookup&& find_guest)
{
	if (hle_first && hle != nullptr)
		if (const auto* record = hle->Find(symbol); record != nullptr)
			return {record, false, false};
	const auto guest = find_guest();
	if (guest.record != nullptr) return guest;
	return {hle != nullptr ? hle->Find(symbol) : nullptr, false, false};
}

void TraceLibcMemoryResolution(const SymbolResolve& symbol, const ResolvedSymbolCandidate& result, bool memcpy)
{
	static std::atomic<uint32_t> emitted_memset {0};
	static std::atomic<uint32_t> emitted_memcpy {0};
	auto& emitted = memcpy ? emitted_memcpy : emitted_memset;
	auto count = emitted.load(std::memory_order_relaxed);
	do
	{
		if (count >= 32) return;
	} while (!emitted.compare_exchange_weak(count, count + 1, std::memory_order_relaxed));
	std::fprintf(stderr,
	             "%s namespace=libc/libc nid=%s type=func "
	             "library_version=%d module_major=%d module_minor=%d route=%s bind_self=%u\n",
	             memcpy ? "[FEX_LIBC_MEMCPY_RESOLVE]" : "[FEX_LIBC_MEMSET_RESOLVE]",
	             memcpy ? "Q3VBxCXhUHs" : "8zTFvBIAIN8",
	             symbol.library_version, symbol.module_version_major, symbol.module_version_minor,
	             result.record == nullptr ? "unresolved" : result.guest ? "guest" : "hle_fallback",
	             result.bind_self ? 1U : 0U);
}

void TraceLibcCompareResolution(const SymbolResolve& symbol, const ResolvedSymbolCandidate& result)
{
	const unsigned index = symbol.name == U"DfivPArhucg" ? 2u : symbol.name == U"aesyjrHVWy4" ? 1u : 0u;
	static constexpr std::array<const char*, 3> names {"strcmp", "strncmp", "memcmp"};
	static constexpr std::array<const char*, 3> nids {"Ovb2dSJOAuE", "aesyjrHVWy4", "DfivPArhucg"};
	static std::array<std::atomic<uint32_t>, 3> counts {};
	auto& emitted = counts[index];
	auto count = emitted.load(std::memory_order_relaxed);
	do
	{
		if (count >= 32) return;
	} while (!emitted.compare_exchange_weak(count, count + 1, std::memory_order_relaxed));
	std::fprintf(stderr,
	             "%s function=%s namespace=libc/libc nid=%s type=func "
	             "library_version=%d module_major=%d module_minor=%d route=%s bind_self=%u\n",
	             index == 2 ? "[FEX_LIBC_MEMCMP_RESOLVE]" : "[FEX_LIBC_STRING_COMPARE_RESOLVE]", names[index], nids[index],
	             symbol.library_version, symbol.module_version_major, symbol.module_version_minor,
	             result.record == nullptr ? "unresolved" : result.guest ? "guest" : "hle_fallback",
	             result.bind_self ? 1U : 0U);
}

} // namespace

bool RuntimeLinkerLibcMemsetPreferenceContractTest()
{
	const auto check = [](bool condition, const char* why) {
		if (!condition) std::fprintf(stderr, "LibcMemoryPreference: %s\n", why);
		return condition;
	};
	for (const char* value: std::array<const char*, 5> {"", "0", "true", "01", "1 "})
		if (!check(!LibcMemoryGuestSelector(value), "selector accepted a non-exact value")) return false;
	if (!check(LibcMemoryGuestSelector(nullptr) && LibcMemoryGuestSelector("1"), "selector rejected default or exact1")) return false;
	const bool selected_memset = LibcMemsetGuestEnabled();
	const bool selected_memcpy = LibcMemcpyGuestEnabled();
	const bool selected_string_compare = LibcStringCompareGuestEnabled();
	const bool selected_memcmp = LibcMemcmpGuestEnabled();
	if (const char* expected = std::getenv("LUMEN_EXPECT_FEX_LIBC_MEMSET_GUEST"))
	{
		if (!check((std::strcmp(expected, "0") == 0 || std::strcmp(expected, "1") == 0) &&
		           selected_memset == (std::strcmp(expected, "1") == 0), "unexpected memset platform selection")) return false;
	}
	if (const char* expected = std::getenv("LUMEN_EXPECT_FEX_LIBC_MEMCPY_GUEST"))
	{
		if (!check((std::strcmp(expected, "0") == 0 || std::strcmp(expected, "1") == 0) &&
		           selected_memcpy == (std::strcmp(expected, "1") == 0), "unexpected memcpy platform selection")) return false;
	}
	if (const char* expected = std::getenv("LUMEN_EXPECT_FEX_LIBC_STRING_COMPARE_GUEST"))
	{
		if (!check((std::strcmp(expected, "0") == 0 || std::strcmp(expected, "1") == 0) &&
		           selected_string_compare == (std::strcmp(expected, "1") == 0),
		           "unexpected string comparison platform selection")) return false;
	}
	if (const char* expected = std::getenv("LUMEN_EXPECT_FEX_LIBC_MEMCMP_GUEST"))
	{
		if (!check((std::strcmp(expected, "0") == 0 || std::strcmp(expected, "1") == 0) &&
		           selected_memcmp == (std::strcmp(expected, "1") == 0),
		           "unexpected memcmp platform selection")) return false;
	}
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__aarch64__) && \
	(LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX)
	const auto check_latched = [&](const char* name, bool selected, bool (*is_enabled)()) {
		const char* old_value = std::getenv(name);
		const bool had_value = old_value != nullptr;
		const std::string saved = had_value ? old_value : "";
		if (setenv(name, selected ? "0" : "1", 1) != 0) return false;
		const bool latched = is_enabled() == selected;
		const int restored = had_value ? setenv(name, saved.c_str(), 1) : unsetenv(name);
		return check(latched && restored == 0, "selector failed latching or environment restore");
	};
	if (!check_latched("LUMEN_EXPERIMENTAL_FEX_LIBC_MEMSET_GUEST", selected_memset, LibcMemsetGuestEnabled) ||
	    !check_latched("LUMEN_EXPERIMENTAL_FEX_LIBC_MEMCPY_GUEST", selected_memcpy, LibcMemcpyGuestEnabled) ||
	    !check_latched("LUMEN_EXPERIMENTAL_FEX_LIBC_STRING_COMPARE_GUEST", selected_string_compare,
	                   LibcStringCompareGuestEnabled) ||
	    !check_latched("LUMEN_EXPERIMENTAL_FEX_LIBC_MEMCMP_GUEST", selected_memcmp, LibcMemcmpGuestEnabled)) return false;
#endif

	SymbolResolve symbol {};
	symbol.name = U"8zTFvBIAIN8";
	symbol.library = U"libc";
	symbol.library_version = 1;
	symbol.module = U"libc";
	symbol.module_version_major = 1;
	symbol.module_version_minor = 1;
	symbol.type = SymbolType::Func;
	SymbolDatabase hle, provider, empty;
	constexpr uint64_t hle_address = 0x1000;
	constexpr uint64_t guest_address = 0x2000;
	hle.Add(symbol, hle_address);
	provider.Add(symbol, guest_address);
	unsigned lookups = 0;
	const auto resolve = [&](const SymbolResolve& query, bool memset_enabled, SymbolDatabase* loaded,
	                         SymbolDatabase* fallback, bool self, bool memcpy_enabled = false,
	                         bool string_compare_enabled = false, bool memcmp_enabled = false) {
		// Simulate the existing ARM64 libc preference; provider lookup itself is
		// the real SymbolDatabase::Find, with no code at either synthetic address.
		return ResolveSymbolProviders(query, IsLibcNamespace(query) &&
		                              !PreferGuestLibcMemory(query, memset_enabled, memcpy_enabled,
		                                                    string_compare_enabled, memcmp_enabled), fallback, [&] {
			++lookups;
			const auto* record = loaded != nullptr ? loaded->Find(query) : nullptr;
			return ResolvedSymbolCandidate {record, record != nullptr && self, record != nullptr};
		});
	};
	const auto expect = [&](const ResolvedSymbolCandidate& result, uint64_t address, bool guest, bool self) {
		return check((result.record != nullptr ? result.record->vaddr : 0) == address &&
		             result.guest == guest && result.bind_self == self, "wrong provider or bind_self");
	};
	if (!expect(resolve(symbol, false, &provider, &hle, true), hle_address, false, false) ||
	    !check(lookups == 0, "rollback performed a provider lookup") ||
	    !expect(resolve(symbol, true, &provider, &hle, false), guest_address, true, false) ||
	    !expect(resolve(symbol, true, &provider, &hle, true), guest_address, true, true) ||
	    !expect(resolve(symbol, true, nullptr, &hle, true), hle_address, false, false) ||
	    !expect(resolve(symbol, true, &empty, &hle, true), hle_address, false, false) ||
	    !expect(resolve(symbol, true, nullptr, nullptr, true), 0, false, false) ||
	    !expect(resolve(symbol, true, &provider, nullptr, true), guest_address, true, true)) return false;

	// A wrong version/type/namespace/NID cannot bind either database's base
	// record. Other APIs retain HLE priority even with the selector enabled.
	for (unsigned change = 0; change < 7; ++change)
	{
		auto query = symbol;
		switch (change)
		{
			case 0: ++query.library_version; break;
			case 1: ++query.module_version_major; break;
			case 2: ++query.module_version_minor; break;
			case 3: query.type = SymbolType::Object; break;
			case 4: query.library = U"libSceLibcInternal"; break;
			case 5: query.module = U"libSceLibcInternal"; break;
			case 6: query.name = U"Q3VBxCXhUHs"; break;
		}
		if (!expect(resolve(query, true, &provider, &hle, true), 0, false, false)) return false;
		SymbolDatabase exact_hle, exact_guest;
		exact_hle.Add(query, hle_address);
		exact_guest.Add(query, guest_address);
		if (change < 3)
		{
			// Version fields select the matching export; they are never dropped
			// merely because the experiment recognizes the libc API identity.
			if (!expect(resolve(query, true, &exact_guest, &exact_hle, false), guest_address, true, false) ||
			    !expect(resolve(query, true, &provider, &exact_hle, true), hle_address, false, false)) return false;
		} else if (change == 4 || change == 5)
		{
			// Other namespaces already prefer their loaded module. The selector
			// must not change that order or absorb their similarly named API.
			if (!check(!PreferGuestLibcMemset(query, true), "unrelated namespace selected") ||
			    !expect(resolve(query, false, &exact_guest, &exact_hle, true), guest_address, true, true) ||
			    !expect(resolve(query, true, &exact_guest, &exact_hle, true), guest_address, true, true)) return false;
		} else
		{
			const auto before = lookups;
			if (!expect(resolve(query, true, &exact_guest, &exact_hle, true), hle_address, false, false) ||
			    !check(lookups == before, "unrelated API lost HLE priority")) return false;
		}
	}
	// Provider-first platforms preserve their existing selection independently
	// of the experimental ARM64/FEX policy.
	if (!expect(ResolveSymbolProviders(symbol, false, &hle, [&] {
		return ResolvedSymbolCandidate {provider.Find(symbol), false, true};
	}), guest_address, true, false)) return false;

	// memcpy has an independent selector and the same full-identity provider
	// lookup and HLE fallback. A memcpy rollback must not affect memset.
	auto copy = symbol;
	copy.name = U"Q3VBxCXhUHs";
	SymbolDatabase copy_hle, copy_provider, copy_empty;
	copy_hle.Add(copy, hle_address);
	copy_provider.Add(copy, guest_address);
	const auto copy_resolve = [&](const SymbolResolve& query, bool enabled, SymbolDatabase* loaded,
	                              SymbolDatabase* fallback, bool self) {
		return resolve(query, false, loaded, fallback, self, enabled);
	};
	const auto before_rollback = lookups;
	if (!expect(copy_resolve(copy, false, &copy_provider, &copy_hle, true), hle_address, false, false) ||
	    !check(lookups == before_rollback, "memcpy rollback performed a provider lookup") ||
	    !expect(copy_resolve(copy, true, &copy_provider, &copy_hle, false), guest_address, true, false) ||
	    !expect(copy_resolve(copy, true, &copy_provider, &copy_hle, true), guest_address, true, true) ||
	    !expect(copy_resolve(copy, true, nullptr, &copy_hle, true), hle_address, false, false) ||
	    !expect(copy_resolve(copy, true, &copy_empty, &copy_hle, true), hle_address, false, false) ||
	    !expect(copy_resolve(copy, true, nullptr, nullptr, true), 0, false, false) ||
	    !expect(copy_resolve(copy, true, &copy_provider, nullptr, true), guest_address, true, true) ||
	    !expect(resolve(symbol, true, &provider, &hle, true, false), guest_address, true, true) ||
	    !expect(resolve(symbol, false, &provider, &hle, true, true), hle_address, false, false)) return false;
	for (unsigned change = 0; change < 7; ++change)
	{
		auto query = copy;
		switch (change)
		{
			case 0: ++query.library_version; break;
			case 1: ++query.module_version_major; break;
			case 2: ++query.module_version_minor; break;
			case 3: query.type = SymbolType::Object; break;
			case 4: query.library = U"libSceLibcInternal"; break;
			case 5: query.module = U"libSceLibcInternal"; break;
			case 6: query.name = U"8zTFvBIAIN8"; break;
		}
		if (!expect(copy_resolve(query, true, &copy_provider, &copy_hle, true), 0, false, false)) return false;
		SymbolDatabase exact_hle, exact_guest;
		exact_hle.Add(query, hle_address);
		exact_guest.Add(query, guest_address);
		if (change < 3)
		{
			if (!expect(copy_resolve(query, true, &exact_guest, &exact_hle, false), guest_address, true, false) ||
			    !expect(copy_resolve(query, true, &copy_provider, &exact_hle, true), hle_address, false, false)) return false;
		} else if (change == 4 || change == 5)
		{
			if (!check(!PreferGuestLibcMemcpy(query, true), "unrelated memcpy namespace selected") ||
			    !expect(copy_resolve(query, false, &exact_guest, &exact_hle, true), guest_address, true, true) ||
			    !expect(copy_resolve(query, true, &exact_guest, &exact_hle, true), guest_address, true, true)) return false;
		} else
		{
			const auto before = lookups;
			if (!expect(copy_resolve(query, true, &exact_guest, &exact_hle, true), hle_address, false, false) ||
			    !check(lookups == before, "unrelated API lost HLE priority")) return false;
		}
	}
	// String comparisons share one selector; memcmp remains independently
	// selectable. Exercise the same provider contract for each exact API.
	for (const auto* nid: std::array<const char32_t*, 3> {U"Ovb2dSJOAuE", U"aesyjrHVWy4", U"DfivPArhucg"})
	{
		auto comparison = symbol;
		comparison.name = nid;
		const bool strings = comparison.name != U"DfivPArhucg";
		SymbolDatabase comparison_hle, comparison_provider;
		comparison_hle.Add(comparison, hle_address);
		comparison_provider.Add(comparison, guest_address);
		const auto comparison_resolve = [&](const SymbolResolve& query, bool enabled, SymbolDatabase* loaded,
		                                    SymbolDatabase* fallback, bool self) {
			return resolve(query, false, loaded, fallback, self, false, enabled && strings, enabled && !strings);
		};
		const auto before_disabled = lookups;
		if (!expect(comparison_resolve(comparison, false, &comparison_provider, &comparison_hle, true), hle_address, false, false) ||
		    !check(lookups == before_disabled, "disabled comparison performed a provider lookup") ||
		    !expect(comparison_resolve(comparison, true, &comparison_provider, &comparison_hle, false), guest_address, true, false) ||
		    !expect(comparison_resolve(comparison, true, &comparison_provider, &comparison_hle, true), guest_address, true, true) ||
		    !expect(comparison_resolve(comparison, true, nullptr, &comparison_hle, true), hle_address, false, false) ||
		    !expect(comparison_resolve(comparison, true, &empty, &comparison_hle, true), hle_address, false, false) ||
		    !expect(comparison_resolve(comparison, true, nullptr, nullptr, true), 0, false, false) ||
		    !expect(comparison_resolve(comparison, true, &comparison_provider, nullptr, true), guest_address, true, true) ||
		    !expect(resolve(comparison, true, &comparison_provider, &comparison_hle, true, true, !strings, strings),
		            hle_address, false, false)) return false;
		for (unsigned change = 0; change < 7; ++change)
		{
			auto query = comparison;
			switch (change)
			{
				case 0: ++query.library_version; break;
				case 1: ++query.module_version_major; break;
				case 2: ++query.module_version_minor; break;
				case 3: query.type = SymbolType::Object; break;
				case 4: query.library = U"LibcInternal"; break;
				case 5: query.module = U"LibcInternal"; break;
				case 6: query.name = U"+P6FRGH4LfA"; break;
			}
			if (!expect(comparison_resolve(query, true, &comparison_provider, &comparison_hle, true), 0, false, false)) return false;
			SymbolDatabase exact_hle, exact_guest;
			exact_hle.Add(query, hle_address);
			exact_guest.Add(query, guest_address);
			if (change < 3)
			{
				if (!expect(comparison_resolve(query, true, &exact_guest, &exact_hle, false), guest_address, true, false) ||
				    !expect(comparison_resolve(query, true, &comparison_provider, &exact_hle, true), hle_address, false, false)) return false;
			} else if (change == 4 || change == 5)
			{
				if (!check(!PreferGuestLibcStringCompare(query, true) && !PreferGuestLibcMemcmp(query, true),
				           "comparison selected another namespace") ||
				    !expect(comparison_resolve(query, false, &exact_guest, &exact_hle, true), guest_address, true, true) ||
				    !expect(comparison_resolve(query, true, &exact_guest, &exact_hle, true), guest_address, true, true)) return false;
			} else
			{
				const auto before = lookups;
				if (!expect(comparison_resolve(query, true, &exact_guest, &exact_hle, true), hle_address, false, false) ||
				    !check(lookups == before, "comparison changed another API's HLE priority")) return false;
			}
		}
	}
	if (!expect(resolve(symbol, true, &provider, &hle, true, false, true, true), guest_address, true, true) ||
	    !expect(resolve(symbol, false, &provider, &hle, true, false, true, true), hle_address, false, false) ||
	    !expect(resolve(copy, false, &copy_provider, &copy_hle, true, true, true, true), guest_address, true, true) ||
	    !expect(resolve(copy, false, &copy_provider, &copy_hle, true, false, true, true), hle_address, false, false)) return false;
	return true;
}

void RuntimeLinker::Resolve(const String& name, SymbolType type,
	Program* program, SymbolRecord* out_info, bool* bind_self)
{
	LUMEN_PROFILER_FUNCTION();

	Core::LockGuard lock(m_mutex);

	EXIT_IF(out_info == nullptr);

	auto ids = name.Split(U'#');

	if (bind_self != nullptr)
	{
		*bind_self = false;
	}

	if (ids.Size() == 3)
	{
		const LibraryId* l = FindLibrary(*program, ids.At(1));
		const ModuleId*  m = FindModule(*program, ids.At(2));

		if (l != nullptr && m != nullptr)
		{
			SymbolResolve sr {};
			sr.name                 = ids.At(0);
			sr.library              = l->name;
			sr.library_version      = l->version;
			sr.module               = m->name;
			sr.module_version_major = m->version_major;
			sr.module_version_minor = m->version_minor;
			sr.type                 = type;

			const bool guest_memset = PreferGuestLibcMemset(sr, LibcMemsetGuestEnabled());
			const bool guest_memcpy = PreferGuestLibcMemcpy(sr, LibcMemcpyGuestEnabled());
			const bool guest_string_compare = PreferGuestLibcStringCompare(sr, LibcStringCompareGuestEnabled());
			const bool guest_memcmp = PreferGuestLibcMemcmp(sr, LibcMemcmpGuestEnabled());
			bool hle_first = false;

#if defined(__aarch64__) && \
	(LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || \
	 LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || \
	 LUMEN_PLATFORM == LUMEN_PLATFORM_IOS)
			// Active FEX targets prefer the loaded guest libc exports by default;
			// preserve the ARM64 HLE policy for other APIs and per-API rollback.
			hle_first = IsLibcNamespace(sr) && !(guest_memset || guest_memcpy || guest_string_compare || guest_memcmp);
#endif

			const auto result = ResolveSymbolProviders(sr, hle_first, m_symbols, [&] {
				if (auto* p = FindProgram(*m, *l, program); p != nullptr && p->export_symbols != nullptr)
				{
					if (const auto* record = p->export_symbols->Find(sr); record != nullptr)
						return ResolvedSymbolCandidate {record, p == program, true};
				}
				return ResolvedSymbolCandidate {};
			});
			const auto* rec = result.record;
			if (bind_self != nullptr) *bind_self = result.bind_self;
			if (guest_memset || guest_memcpy) TraceLibcMemoryResolution(sr, result, guest_memcpy);
			if (guest_string_compare || guest_memcmp) TraceLibcCompareResolution(sr, result);

			if (rec != nullptr)
			{
				//*out_vaddr = rec->vaddr;
				*out_info = *rec;
			} else
			{
				out_info->vaddr    = 0;
				out_info->name     = SymbolDatabase::GenerateName(sr);
				out_info->dbg_name = U"";
			}
		} else
		{
			EXIT("l == nullptr || m == nullptr");
		}
	} else
	{
		out_info->vaddr    = 0;
		out_info->name     = name;
		out_info->dbg_name = U"";
	}
}

uint64_t RuntimeLinker::ReadFromElf(Program* program, uint64_t vaddr)
{
	EXIT_IF(program == nullptr);
	EXIT_IF(program->base_vaddr == 0 || program->base_size == 0);
	EXIT_IF(program->elf == nullptr);

	uint64_t ret = 0;

	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();

	EXIT_IF(phdr == nullptr || ehdr == nullptr);

	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++)
	{
		if (phdr[i].p_memsz != 0 && (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO))
		{
			uint64_t segment_addr      = phdr[i].p_vaddr + program->base_vaddr;
			uint64_t segment_file_size = phdr[i].p_filesz;

			if (vaddr >= segment_addr && vaddr < segment_addr + segment_file_size)
			{
				program->elf->LoadSegment(reinterpret_cast<uint64_t>(&ret), phdr[i].p_offset + vaddr - segment_addr, sizeof(ret));
				break;
			}
		}
	}

	return ret;
}

Program* RuntimeLinker::FindProgramById(int32_t id)
{
	Core::LockGuard lock(m_mutex);

	for (auto* p: m_programs)
	{
		if (p->unique_id == id)
		{
			return p;
		}
	}

	return nullptr;
}

Program* RuntimeLinker::FindProgramByAddr(uint64_t vaddr)
{
	Core::LockGuard lock(m_mutex);

	for (auto* p: m_programs)
	{
		if (p->elf == nullptr) continue;
		const auto* ehdr = p->elf->GetEhdr();
		const auto* phdr = p->elf->GetPhdr();

		EXIT_IF(phdr == nullptr || ehdr == nullptr);

		for (Elf64_Half i = 0; i < ehdr->e_phnum; i++)
		{
			if (phdr[i].p_memsz != 0 && (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO))
			{
				uint64_t segment_addr = phdr[i].p_vaddr + p->base_vaddr;
				uint64_t segment_size = get_aligned_size(phdr + i);

				if (vaddr >= segment_addr && vaddr < segment_addr + segment_size)
				{
					return p;
				}
			}
		}
	}

	return nullptr;
}

bool RuntimeLinker::QueryProgramUnwindInfo(
	uint64_t vaddr, ProgramUnwindInfo* info)
{
	if (info == nullptr) return false;
	*info = {};

	Core::LockGuard lock(m_mutex);
	Program* program = nullptr;
	for (auto* candidate: m_programs)
	{
		if (candidate == nullptr || candidate->elf == nullptr) continue;
		const auto* candidate_ehdr = candidate->elf->GetEhdr();
		const auto* candidate_phdr = candidate->elf->GetPhdr();
		if (candidate_ehdr == nullptr || candidate_phdr == nullptr) continue;
		for (Elf64_Half index = 0; index < candidate_ehdr->e_phnum;
		     ++index)
		{
			const auto& segment = candidate_phdr[index];
			if (segment.p_memsz == 0 ||
			    (segment.p_type != PT_LOAD &&
			     segment.p_type != PT_OS_RELRO))
			{
				continue;
			}
			const uint64_t segment_size = get_aligned_size(&segment);
			if (candidate->base_vaddr > UINT64_MAX - segment.p_vaddr ||
			    segment_size > UINT64_MAX -
			                       (candidate->base_vaddr + segment.p_vaddr))
			{
				continue;
			}
			const uint64_t begin = candidate->base_vaddr + segment.p_vaddr;
			if (vaddr >= begin && vaddr < begin + segment_size)
			{
				program = candidate;
				break;
			}
		}
		if (program != nullptr) break;
	}
	if (program == nullptr) return false;

	ProgramUnwindInfo result {};
	auto module_name = program->file_name.FilenameWithoutDirectory();
	if (program->dynamic_info != nullptr &&
	    !program->dynamic_info->export_modules.IsEmpty() &&
	    !program->dynamic_info->export_modules.At(0).name.IsEmpty())
	{
		module_name = String::FromPrintf(
		    "%s.sprx",
		    program->dynamic_info->export_modules.At(0).name.C_Str());
	}
	std::strncpy(result.name, module_name.C_Str(), sizeof(result.name) - 1);

	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();
	const auto aligned_size = [](const Elf64_Phdr& segment,
	                            uint64_t* size)
	{
		if (size == nullptr) return false;
		if (segment.p_align == 0)
		{
			*size = segment.p_memsz;
			return true;
		}
		if ((segment.p_align & (segment.p_align - 1)) != 0 ||
		    segment.p_memsz > UINT64_MAX - (segment.p_align - 1))
		{
			return false;
		}
		*size = (segment.p_memsz + segment.p_align - 1) &
		        ~(segment.p_align - 1);
		return true;
	};
	const auto range_is_loaded = [&](uint64_t offset, uint64_t size)
	{
		if (size == 0 || offset > UINT64_MAX - size) return false;
		const uint64_t end = offset + size;
		for (Elf64_Half index = 0; index < ehdr->e_phnum; ++index)
		{
			const auto& loaded = phdr[index];
			if (loaded.p_memsz == 0 ||
			    (loaded.p_type != PT_LOAD &&
			     loaded.p_type != PT_OS_RELRO))
			{
				continue;
			}
			uint64_t loaded_size = 0;
			if (!aligned_size(loaded, &loaded_size) ||
			    loaded.p_vaddr > UINT64_MAX - loaded_size)
			{
				continue;
			}
			if (offset >= loaded.p_vaddr &&
			    end <= loaded.p_vaddr + loaded_size)
			{
				return true;
			}
		}
		return false;
	};

	for (Elf64_Half index = 0; index < ehdr->e_phnum; ++index)
	{
		const auto& segment = phdr[index];
		if (result.seg0_addr == 0 && segment.p_memsz != 0 &&
		    (segment.p_type == PT_LOAD ||
		     segment.p_type == PT_OS_RELRO))
		{
			uint64_t segment_size = 0;
			if (aligned_size(segment, &segment_size) &&
			    program->base_vaddr <= UINT64_MAX - segment.p_vaddr)
			{
				result.seg0_addr = program->base_vaddr + segment.p_vaddr;
				result.seg0_size = segment_size;
			}
		}
		if (segment.p_type != PT_GNU_EH_FRAME ||
		    segment.p_memsz < 8 ||
		    !range_is_loaded(segment.p_vaddr, segment.p_memsz) ||
		    program->base_vaddr > UINT64_MAX - segment.p_vaddr)
		{
			continue;
		}

		const uint64_t header_address =
		    program->base_vaddr + segment.p_vaddr;
		result.eh_frame_hdr_addr = header_address;
		std::array<uint8_t, 8> header {};
		std::memcpy(header.data(),
		            reinterpret_cast<const void*>(header_address),
		            header.size());
		if (header[0] != 1 || header[1] != 0x1b)
		{
			continue;
		}

		int32_t relative = 0;
		std::memcpy(&relative, header.data() + 4, sizeof(relative));
		const uint64_t encoded_at = header_address + 4;
		const uint64_t distance = relative < 0
		                              ? static_cast<uint64_t>(-
		                                    static_cast<int64_t>(relative))
		                              : static_cast<uint64_t>(relative);
		const bool valid_offset = relative < 0
		                              ? encoded_at >= distance
		                              : encoded_at <= UINT64_MAX - distance;
		if (!valid_offset ||
		    program->base_vaddr > UINT64_MAX - program->base_size_aligned)
		{
			continue;
		}
		const uint64_t frame = relative < 0 ? encoded_at - distance
		                                    : encoded_at + distance;
		const uint64_t module_end =
		    program->base_vaddr + program->base_size_aligned;
		if (frame < program->base_vaddr || frame >= module_end)
		{
			continue;
		}
		result.eh_frame_addr = frame;
		result.eh_frame_size = header_address > frame
		                                ? header_address - frame
		                                : program->base_size_aligned >=
		                                          segment.p_vaddr
		                                      ? program->base_size_aligned -
		                                            segment.p_vaddr
		                                      : 0;
	}

	*info = result;
	return true;
}

Program* RuntimeLinker::FindProgramByName(const char* name)
{
	if (name == nullptr || name[0] == '\0') return nullptr;
	Core::LockGuard lock(m_mutex);
	for (auto* p: m_programs)
	{
		if (p->file_name.FilenameWithoutDirectory().ToLower().Equal(String::FromUtf8(name).ToLower()))
		{
			return p;
		}
	}
	return nullptr;
}

void RuntimeLinker::StackTrace(uint64_t frame_ptr)
{
	void* stack[20];
	int   depth = 20;

	sys_stack_walk_x86(frame_ptr, stack, &depth);

	std::printf("Stack trace [thread = %d]:\n", Core::Thread::GetThreadIdUnique());

	for (int i = 0; i < depth; i++)
	{
		auto  vaddr = reinterpret_cast<uint64_t>(stack[i]);
		auto* p     = FindProgramByAddr(vaddr);
		std::printf("[%d] %016" PRIx64 ", %s\n", i, vaddr, (p == nullptr ? "???" : p->file_name.FilenameWithoutDirectory().C_Str()));
	}
}

void RuntimeLinker::StartAllModules()
{
	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);

	for (auto* p: m_programs)
	{
		if (p->elf->IsShared())
		{
			StartModule(p, 0, nullptr, nullptr);
		}
	}
}

void RuntimeLinker::StopAllModules()
{
	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	Core::LockGuard lock(m_mutex);

	for (auto* p: m_programs)
	{
		if (p->elf->IsShared())
		{
			StopModule(p, 0, nullptr, nullptr);
		}
	}
}

int RuntimeLinker::StartModule(Program* program, size_t args, const void* argp, module_func_t func)
{
	EXIT_IF(program == nullptr);
	EXIT_IF(program->dynamic_info == nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(!program->elf->IsShared());

	EXIT_IF(!m_programs.Contains(program));

	printf(FG_BRIGHT_YELLOW "---" DEFAULT "\n");
	printf(FG_BRIGHT_YELLOW "--- Start module: " BG_BLUE BOLD "%s" BG_DEFAULT NO_BOLD DEFAULT "\n", program->file_name.C_Str());
	printf(FG_BRIGHT_YELLOW "---" DEFAULT "\n");

	// DT_INIT is optional, but a present tag may legitimately contain a zero
	// offset.  In that case the initializer is the first byte of the relocated
	// module (libc.prx relies on this).  Test tag presence instead of using the
	// offset as an absent-value sentinel.
	if (!program->elf->HasDynValue(DT_INIT))
	{
		return 0;
	}
	return run_ini_fini(program->dynamic_info->init_vaddr + program->base_vaddr, args, argp, func);
}

int RuntimeLinker::StopModule(Program* program, size_t args, const void* argp, module_func_t func)
{
	EXIT_IF(program == nullptr);
	EXIT_IF(program->dynamic_info == nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(!program->elf->IsShared());

	EXIT_IF(!m_programs.Contains(program));

	printf(FG_BRIGHT_YELLOW "---" DEFAULT "\n");
	printf(FG_BRIGHT_YELLOW "--- Stop module: " BG_BLUE BOLD "%s" BG_DEFAULT NO_BOLD DEFAULT "\n", program->file_name.C_Str());
	printf(FG_BRIGHT_YELLOW "---" DEFAULT "\n");

	// DT_FINI follows the same presence-versus-zero-offset rule as DT_INIT.
	// Static-object cleanup still belongs to this module when the tag is absent.
	int result = 0;
	if (program->elf->HasDynValue(DT_FINI))
	{
		result = run_ini_fini(program->dynamic_info->fini_vaddr + program->base_vaddr, args, argp, func);
	}

	Libs::LibKernel::PthreadDeleteStaticObjects(program);

	return result;
}

#if defined(LUMEN_MODULE_LOAD_DEDUPE_TEST_HOOK)
Program* RuntimeLinker::RegisterSyntheticProgramForTest(const String& file_name)
{
	Core::LockGuard lock(m_mutex);

	auto* program = new Program;
	program->rt        = this;
	program->file_name = file_name;
	m_programs.Add(program);
	return program;
}
#endif

#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
bool RuntimeLinker::SetupSyntheticTlsForPthreadExitTest()
{
	Core::LockGuard lock(m_mutex);
	if (g_tls_main_program != nullptr || !m_programs.IsEmpty()) return false;

	// A test-owned image supplies the ordinary per-thread TLS allocator without
	// loading an ELF. Program ownership and DeleteTlss/Clear remain unchanged.
	static const uint8_t tls_image[16] = {0x31, 0x72, 0x19, 0xa5};
	auto* program = new Program;
	program->rt = this;
	program->file_name = U"pthread-exit-synthetic-tls";
	program->tls.image_vaddr = reinterpret_cast<uint64_t>(tls_image);
	program->tls.image_size = sizeof(tls_image);
	m_programs.Add(program);
	g_tls_main_program = program;
	g_tls_spinlock = 0;
	std::memset(g_tls_reg_save_area, 0, XSAVE_BUFFER_SIZE);
	std::memcpy(&g_tls_reg_save_area[XSAVE_BUFFER_SIZE], &XSAVE_CHK_GUARD, sizeof(XSAVE_CHK_GUARD));
	return true;
}
#endif

namespace {
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
thread_local RuntimeLinker::TlsRetirementAdmissionTestHook g_tls_retirement_admission_test_hook = nullptr;
#endif
// Keep VM admission outside loader/TLS locks. Core VM operations publish
// their own ranges; this outer epoch only delays readmission until initialized
// TLS is published. No guest callback may execute inside it.
class GuestTlsMutation final
{
public:
	explicit GuestTlsMutation(bool may_retry_retirement = false, int thread_id = 0)
	{
#if defined(LUMEN_FEXCORE_RUNTIME)
		// Only DeleteTlss opts in: it carries a scalar thread ID and has not
		// acquired the loader lock or selected a Program. Other TLS paths may
		// retain a Program* or an outer loader/TLS lock and must fail closed.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		constexpr uint32_t max_attempts = 4096;
		for (uint32_t attempt = 1; ; ++attempt)
		{
			uint32_t retryable = 0;
			m_token = LumenAndroidFexCoreBeginGuestMemoryMutationV3(
			    LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2, &retryable);
			if (m_token != 0) break;
			EXIT_IF(!may_retry_retirement || retryable != 1 || attempt == max_attempts ||
			        std::chrono::steady_clock::now() >= deadline);
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
			if (g_tls_retirement_admission_test_hook != nullptr)
				g_tls_retirement_admission_test_hook(1, thread_id, attempt);
#endif
			{
				ScopedGuestHostWait host_wait;
				EXIT_IF(!host_wait.IsActive());
			}
			// Readmission itself is not timed. This deadline only bounds fresh
			// attempts, matching the existing pthread backing retry contract.
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
			if (g_tls_retirement_admission_test_hook != nullptr)
				g_tls_retirement_admission_test_hook(2, thread_id, attempt);
#endif
			EXIT_IF(std::chrono::steady_clock::now() >= deadline);
		}
#else
		(void)may_retry_retirement;
#endif
		(void)thread_id;
	}
	~GuestTlsMutation()
	{
#if defined(LUMEN_FEXCORE_RUNTIME)
		EXIT_IF(LumenAndroidFexCoreEndGuestMemoryMutationV2(m_token, 0, 0, 0, nullptr, nullptr) == 0);
#endif
	}
	GuestTlsMutation(const GuestTlsMutation&) = delete;
	GuestTlsMutation& operator=(const GuestTlsMutation&) = delete;
private:
#if defined(LUMEN_FEXCORE_RUNTIME)
	uint64_t m_token = 0;
#endif
};
} // namespace

#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
void RuntimeLinker::SetTlsRetirementAdmissionTestHook(TlsRetirementAdmissionTestHook hook)
{
	g_tls_retirement_admission_test_hook = hook;
}

uint64_t RuntimeLinker::AddSyntheticTlsForRetirementTest(int thread_id, uint64_t mutation_token)
{
#if defined(LUMEN_FEXCORE_RUNTIME)
	if (LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(mutation_token) == 0) return 0;
#else
	(void)mutation_token;
#endif
	// The fixture owns this image-less Program and its ordinary Core mapping.
	// Insert it during the competing writer epoch to qualify fresh enumeration
	// after readmission, without loading or borrowing any external image.
	const auto storage = Core::VirtualMemory::Alloc(0, 16 * 1024, Core::VirtualMemory::Mode::ReadWrite);
	if (storage == 0) return 0;
	auto* program = new Program;
	program->rt = this;
	program->file_name = U"pthread-retirement-added-tls";
	program->tls.tlss.GetOrPutDef(thread_id, nullptr) = reinterpret_cast<uint8_t*>(storage);
	Core::LockGuard lock(m_mutex);
	m_programs.Add(program);
	return storage;
}

bool RuntimeLinker::HasTlsForRetirementTest(int thread_id)
{
	Core::LockGuard lock(m_mutex);
	for (auto* program: m_programs)
	{
		Core::LockGuard tls_lock(program->tls.mutex);
		if (program->tls.tlss.Get(thread_id, nullptr) != nullptr) return true;
	}
	return false;
}
#endif

void RuntimeLinker::PrepareCurrentThreadTls()
{
	const int thread_id = Core::Thread::GetThreadIdUnique();
	bool missing = false;
	{
		Core::LockGuard lock(m_mutex);
		for (auto* program: m_programs)
		{
			if (program != g_tls_main_program && program->tls.image_size == 0) continue;
			Core::LockGuard tls_lock(program->tls.mutex);
			if (program->tls.tlss.Get(thread_id, nullptr) == nullptr) { missing = true; break; }
		}
	}
	if (!missing) return;
	GuestTlsMutation mutation;
	Core::LockGuard lock(m_mutex);
	for (auto* program: m_programs)
	{
		if (program == g_tls_main_program || program->tls.image_size != 0) TlsGetAddr(program);
	}
}

uint8_t* RuntimeLinker::TlsGetAddr(Program* program)
{
	EXIT_IF(program == nullptr);

	const int thread_id = Core::Thread::GetThreadIdUnique();
	{
		Core::LockGuard lock(program->tls.mutex);
		if (auto* existing = program->tls.tlss.Get(thread_id, nullptr); existing != nullptr) return existing;
	}
	GuestTlsMutation mutation;
	Core::LockGuard lock(program->tls.mutex);
	auto& tls = program->tls.tlss.GetOrPutDef(thread_id, nullptr);

	if (tls == nullptr)
	{
		// Keep the FreeBSD thread control block after the TLS image, matching the
		// PS4 ABI: FS:[0] is the self pointer (the x86 JIT historically returned
		// this end address from its TLS thunk) and FS:[0x10] is the
		// `tcb_pthread` slot, which guest code reads to reach per-thread state
		// such as the cancel flags. Publish the emulated pthread object (the
		// guest handle) there; the DTV cell at FS:[0x08] stays zero because
		// Lumen serves dynamic TLS through this same accessor.
		constexpr uint64_t tcb_size = 0x18;
		constexpr uint64_t page_size = 16 * 1024;
		EXIT_IF(program->tls.image_size > UINT64_MAX - tcb_size - (page_size - 1));
		const auto alloc_size = (program->tls.image_size + tcb_size + page_size - 1) & ~(page_size - 1);
		auto* storage = reinterpret_cast<uint8_t*>(
		    Core::VirtualMemory::Alloc(0, alloc_size, Core::VirtualMemory::Mode::ReadWrite));
		EXIT_IF(storage == nullptr);
		std::memset(storage, 0, alloc_size);
		if (program->tls.image_size != 0)
			std::memcpy(storage, reinterpret_cast<void*>(program->tls.image_vaddr), program->tls.image_size);
		auto* tcb = storage + program->tls.image_size;
		const auto self = reinterpret_cast<uint64_t>(tcb);
		const auto pthread = Libs::LibKernel::PthreadGuestSelfHandle();
		std::memcpy(tcb, &self, sizeof(self));
		std::memcpy(tcb + 0x10, &pthread, sizeof(pthread));
		tls = storage;
	}
	return tls;
}

void RuntimeLinker::DeleteTls(Program* program, int thread_id)
{
	EXIT_IF(program == nullptr);

	GuestTlsMutation mutation;
	Core::LockGuard lock(program->tls.mutex);
	const auto address = reinterpret_cast<uint64_t>(program->tls.tlss.Get(thread_id, nullptr));
	if (address != 0) EXIT_IF(!Core::VirtualMemory::Free(address));
	program->tls.tlss.Remove(thread_id);
}

static uint64_t loader_code_page_size()
{
#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || defined(__ANDROID__))
	const long size = sysconf(_SC_PAGESIZE);
	EXIT_IF(size <= 0 || (size & (size - 1)) != 0);
	return static_cast<uint64_t>(size);
#else
	return 0x1000;
#endif
}

static uint64_t loader_tls_handler_size()
{
	const uint64_t page_size = loader_code_page_size();
	return (Jit::SafeCall::GetSize() + page_size - 1) & ~(page_size - 1);
}

static bool try_calc_base_size(const Elf64_Phdr* phdr, size_t phnum,
	                           uint64_t* base_size)
{
	if (phdr == nullptr || base_size == nullptr) return false;
	uint64_t size = 0;
	bool has_load_segment = false;
	for (size_t i = 0; i < phnum; i++)
	{
		if (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO)
		{
			uint64_t segment_size = 0;
			if (!try_get_aligned_segment_size(phdr + i, &segment_size))
			{
				return false;
			}
			if (phdr[i].p_memsz == 0) continue;
			has_load_segment = true;
			const uint64_t last_addr = phdr[i].p_vaddr + segment_size;
			if (last_addr > size)
			{
				size = last_addr;
			}
		}
	}
	if (!has_load_segment || size == 0) return false;
	*base_size = size;
	return true;
}

static bool try_advance_module_layout_cursor(uint64_t actual_base,
	                                          uint64_t allocation_size,
	                                          uint64_t* next_cursor)
{
	if (actual_base == 0 || allocation_size == 0 || next_cursor == nullptr)
	{
		return false;
	}
	const uint64_t slots = allocation_size / CODE_BASE_INCR + 1;
	if (slots > UINT64_MAX / CODE_BASE_INCR) return false;
	const uint64_t advance = slots * CODE_BASE_INCR;
	if (actual_base > UINT64_MAX - advance) return false;
	*next_cursor = actual_base + advance;
	return true;
}

#if defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)
bool RuntimeLinkerValidateSegmentLayoutForTest(
	uint64_t vaddr, uint64_t file_offset, uint64_t file_size,
	uint64_t memory_size, uint64_t alignment, uint64_t* aligned_size,
	uint64_t* end)
{
	if (aligned_size == nullptr || end == nullptr) return false;
	*aligned_size = 0;
	*end = 0;
	Elf64_Phdr segment {};
	segment.p_vaddr = vaddr;
	segment.p_offset = file_offset;
	segment.p_filesz = file_size;
	segment.p_memsz = memory_size;
	segment.p_align = alignment;
	if (!try_get_aligned_segment_size(&segment, aligned_size)) return false;
	*end = vaddr + *aligned_size;
	return true;
}

bool RuntimeLinkerCalculateProgramSpanForTest(
	const Elf64_Phdr* phdr, size_t phnum, uint64_t* span)
{
	return try_calc_base_size(phdr, phnum, span);
}

bool RuntimeLinkerAdvanceModuleLayoutCursorForTest(
	uint64_t actual_base, uint64_t allocation_size, uint64_t* next_cursor)
{
	return try_advance_module_layout_cursor(
	    actual_base, allocation_size, next_cursor);
}
#endif

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RuntimeLinker::LoadProgramToMemory(Program* program)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr || program->base_vaddr != 0 || program->base_size != 0 || program->elf == nullptr ||
	        program->exception_handler != nullptr);

	// static uint64_t desired_base_addr = DESIRED_BASE_ADDR;

	bool is_shared = program->elf->IsShared();

	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();

	EXIT_IF(phdr == nullptr || ehdr == nullptr);

	const bool valid_program_span = try_calc_base_size(
	    phdr, static_cast<size_t>(ehdr->e_phnum), &program->base_size);
	if (!valid_program_span) EXIT("Invalid ELF program span\n");
	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++)
	{
		if (phdr[i].p_type == PT_TLS)
		{
			uint64_t tls_size = 0;
			const bool valid_tls =
			    try_get_aligned_segment_size(phdr + i, &tls_size) &&
			    phdr[i].p_vaddr < program->base_size &&
			    tls_size <= program->base_size - phdr[i].p_vaddr;
			if (!valid_tls) EXIT("Invalid ELF TLS layout\n");
		}
		if (phdr[i].p_type == PT_OS_PROCPARAM)
		{
			if (phdr[i].p_vaddr >= program->base_size)
			{
				EXIT("Invalid ELF process-parameter layout\n");
			}
		}
	}
	if (!is_shared && ehdr->e_entry >= program->base_size)
	{
		EXIT("Invalid ELF entrypoint layout\n");
	}
	const uint64_t code_page_size = loader_code_page_size();
	if (code_page_size == 0 ||
	    (code_page_size & (code_page_size - 1)) != 0 ||
	    program->base_size > UINT64_MAX - code_page_size)
	{
		EXIT("Invalid loader page or image size\n");
	}
	// Loader-owned helper code must not share a physical page with writable
	// image data. Guest segment addresses and protections remain unchanged.
	program->base_size_aligned = (program->base_size & ~(code_page_size - 1)) + code_page_size;

	uint64_t exception_handler_size = Core::VirtualMemory::ExceptionHandler::GetSize();
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__ANDROID__)
	// The Android exception-handler reservation is a guest 4 KiB placeholder.
	// Pad it to the host granule so the following TLS helper can become RX
	// without sharing a physical page with the writable reservation.
	if (exception_handler_size > UINT64_MAX - (code_page_size - 1))
	{
		EXIT("Invalid exception-handler size\n");
	}
	exception_handler_size = (exception_handler_size + code_page_size - 1) & ~(code_page_size - 1);
#endif
	uint64_t tls_handler_size       = is_shared ? 0 : loader_tls_handler_size();
	if (program->base_size_aligned > UINT64_MAX - exception_handler_size)
	{
		EXIT("Runtime image allocation overflow\n");
	}
	uint64_t alloc_size = program->base_size_aligned + exception_handler_size;
	if (alloc_size > UINT64_MAX - tls_handler_size)
	{
		EXIT("Runtime TLS allocation overflow\n");
	}
	alloc_size += tls_handler_size;

	const uint64_t requested_base_vaddr = g_desired_base_addr;
	program->base_vaddr = Core::VirtualMemory::Alloc(
	    requested_base_vaddr, alloc_size,
	    Core::VirtualMemory::Mode::ExecuteReadWrite);
	uint64_t next_base_vaddr = 0;
	const bool valid_actual_layout =
	    program->base_vaddr != 0 &&
	    program->base_vaddr <= UINT64_MAX - alloc_size &&
	    program->base_vaddr <= UINT64_MAX - ehdr->e_entry &&
	    (is_shared || program->base_vaddr >= SYSTEM_RESERVED) &&
	    try_advance_module_layout_cursor(
	        program->base_vaddr, alloc_size, &next_base_vaddr);
	if (!valid_actual_layout)
	{
		if (program->base_vaddr != 0)
		{
			if (!Core::VirtualMemory::Free(program->base_vaddr))
			{
				EXIT("Failed to release invalid runtime module layout\n");
			}
			program->base_vaddr = 0;
		}
		EXIT("Invalid runtime module layout\n");
	}
	const char* const layout_trace = std::getenv("LUMEN_RUNTIME_MODULE_LAYOUT_TRACE");
	if (layout_trace != nullptr && layout_trace[0] == '1' &&
	    layout_trace[1] == '\0')
	{
		std::fprintf(stderr,
		             "[RUNTIME_MODULE_LAYOUT] module=%s shared=%u requested=0x%016" PRIx64
		             " actual=0x%016" PRIx64 " imageSize=0x%016" PRIx64
		             " allocationSize=0x%016" PRIx64 "\n",
		             program->file_name.FilenameWithoutDirectory().C_Str(),
		             is_shared ? 1U : 0U, requested_base_vaddr,
		             program->base_vaddr, program->base_size_aligned, alloc_size);
	}

	if (!is_shared)
	{
		program->tls.handler_vaddr = program->base_vaddr + program->base_size_aligned + exception_handler_size;
	}

	g_desired_base_addr = next_base_vaddr;

	EXIT_IF(program->base_size_aligned < program->base_size);

	printf("base_vaddr             = 0x%016" PRIx64 "\n", program->base_vaddr);
	printf("base_size              = 0x%016" PRIx64 "\n", program->base_size);
	printf("base_size_aligned      = 0x%016" PRIx64 "\n", program->base_size_aligned);
	printf("exception_handler_size = 0x%016" PRIx64 "\n", exception_handler_size);
	if (!is_shared)
	{
		printf("tls_handler_size       = 0x%016" PRIx64 "\n", tls_handler_size);
	}

	program->exception_handler = new Core::VirtualMemory::ExceptionHandler;

	if (is_shared)
	{
		program->exception_handler_ready =
		    program->exception_handler->Install(program->base_vaddr, program->base_vaddr + program->base_size_aligned,
		                                        program->base_size_aligned + exception_handler_size + tls_handler_size,
		                                        lumen_exception_handler);
	} else
	{
		program->exception_handler_ready =
		    program->exception_handler->Install(SYSTEM_RESERVED, program->base_vaddr + program->base_size_aligned,
		                                        program->base_vaddr + program->base_size_aligned + exception_handler_size + tls_handler_size -
		                                            SYSTEM_RESERVED,
		                                        lumen_exception_handler);
	}

	// program->elf->SetBaseVAddr(program->base_vaddr);

	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++)
	{
		if (phdr[i].p_memsz != 0 && (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO))
		{
			uint64_t segment_addr        = phdr[i].p_vaddr + program->base_vaddr;
			uint64_t segment_file_size   = phdr[i].p_filesz;
			uint64_t segment_memory_size = get_aligned_size(phdr + i);
			auto     mode                = get_mode(phdr[i].p_flags);

			printf("[%d] addr        = 0x%016" PRIx64 "\n", i, segment_addr);
			printf("[%d] file_size   = %" PRIu64 "\n", i, segment_file_size);
			printf("[%d] memory_size = %" PRIu64 "\n", i, segment_memory_size);
			printf("[%d] mode        = %s\n", i, Core::EnumName(mode).C_Str());

			program->elf->LoadSegment(segment_addr, phdr[i].p_offset, segment_file_size);

			if (Core::VirtualMemory::IsExecute(mode))
			{
				PatchProgram(program, segment_addr, segment_memory_size);
			}

			EXIT_IF(!Core::VirtualMemory::Protect(segment_addr, segment_memory_size, mode));

			if (Core::VirtualMemory::IsExecute(mode))
			{
				EXIT_IF(!Core::VirtualMemory::FlushInstructionCache(segment_addr, segment_memory_size));
			}
		}

		if (phdr[i].p_type == PT_TLS)
		{
			EXIT_IF(phdr[i].p_vaddr >= program->base_size);

			program->tls.image_vaddr = phdr[i].p_vaddr + program->base_vaddr;
			program->tls.image_size  = get_aligned_size(phdr + i);

			printf("tls addr = 0x%016" PRIx64 "\n", program->tls.image_vaddr);
			printf("tls size   = %" PRIu64 "\n", program->tls.image_size);
		}

		if (phdr[i].p_type == PT_OS_PROCPARAM)
		{
			EXIT_IF(program->proc_param_vaddr != 0);
			EXIT_IF(phdr[i].p_vaddr >= program->base_size);

			program->proc_param_vaddr = phdr[i].p_vaddr + program->base_vaddr;
		}
	}

	if (!is_shared)
	{
		SetupTlsHandler(program);
	}

#if defined(_WIN32) && defined(__x86_64__)
	install_native_module_trace(program);
#endif

	printf("entry = 0x%016" PRIx64 "\n", program->elf->GetEntry() + program->base_vaddr);
}

void RuntimeLinker::DeleteProgram(Program* p)
{
	if (p->base_vaddr != 0 || p->base_size != 0)
	{
		Core::VirtualMemory::Free(p->base_vaddr);
	}

	if (p->custom_call_plt_vaddr != 0 || p->custom_call_plt_num != 0)
	{
		Core::VirtualMemory::Free(p->custom_call_plt_vaddr);
	}

	delete p->elf;
	delete p->dynamic_info;
	delete p->exception_handler;
	delete p->export_symbols;
	delete p->import_symbols;

	FOR_HASH (p->tls.tlss)
	{
		EXIT_IF(!Core::VirtualMemory::Free(reinterpret_cast<uint64_t>(p->tls.tlss.Value())));
	}

	delete p;
}

void RuntimeLinker::ParseProgramDynamicInfo(Program* program)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(program->dynamic_info != nullptr);

	program->dynamic_info = new DynamicInfo;

	auto* elf = program->elf;

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_HASH) && elf->HasDynValue(DT_HASH));
	get_dyn_data_os(elf, &program->dynamic_info->hash_table, DT_OS_HASH);
	get_dyn_data(elf, program->base_vaddr, &program->dynamic_info->hash_table, DT_HASH);
	get_dyn_value(elf, &program->dynamic_info->hash_table_size, DT_OS_HASHSZ);

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_STRTAB) && elf->HasDynValue(DT_STRTAB));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_STRSZ) && elf->HasDynValue(DT_STRSZ));
	get_dyn_data_os(elf, &program->dynamic_info->str_table, DT_OS_STRTAB);
	get_dyn_data(elf, program->base_vaddr, &program->dynamic_info->str_table, DT_STRTAB);
	get_dyn_value(elf, &program->dynamic_info->str_table_size, DT_OS_STRSZ);
	get_dyn_value(elf, &program->dynamic_info->str_table_size, DT_STRSZ);

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_SYMTAB) && elf->HasDynValue(DT_SYMTAB));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_SYMENT) && elf->HasDynValue(DT_SYMENT));
	get_dyn_data_os(elf, &program->dynamic_info->symbol_table, DT_OS_SYMTAB);
	get_dyn_data(elf, program->base_vaddr, &program->dynamic_info->symbol_table, DT_SYMTAB);
	get_dyn_value(elf, &program->dynamic_info->symbol_table_total_size, DT_OS_SYMTABSZ);
	get_dyn_value(elf, &program->dynamic_info->symbol_table_entry_size, DT_OS_SYMENT);
	get_dyn_value(elf, &program->dynamic_info->symbol_table_entry_size, DT_SYMENT);

	get_dyn_ptr(elf, &program->dynamic_info->init_vaddr, DT_INIT);
	get_dyn_ptr(elf, &program->dynamic_info->fini_vaddr, DT_FINI);
	get_dyn_ptr(elf, &program->dynamic_info->init_array_vaddr, DT_INIT_ARRAY);
	get_dyn_ptr(elf, &program->dynamic_info->fini_array_vaddr, DT_FINI_ARRAY);
	get_dyn_ptr(elf, &program->dynamic_info->preinit_array_vaddr, DT_PREINIT_ARRAY);
	get_dyn_value(elf, &program->dynamic_info->init_array_size, DT_INIT_ARRAYSZ);
	get_dyn_value(elf, &program->dynamic_info->fini_array_size, DT_FINI_ARRAYSZ);
	get_dyn_value(elf, &program->dynamic_info->preinit_array_size, DT_PREINIT_ARRAYSZ);

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_PLTGOT) && elf->HasDynValue(DT_PLTGOT));
	get_dyn_ptr(elf, &program->dynamic_info->pltgot_vaddr, DT_OS_PLTGOT);
	get_dyn_ptr(elf, &program->dynamic_info->pltgot_vaddr, DT_PLTGOT);

	Elf64_Sxword jmprel_type = 0;
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_PLTREL) && elf->HasDynValue(DT_PLTREL));
	get_dyn_value(elf, &jmprel_type, DT_OS_PLTREL);
	get_dyn_value(elf, &jmprel_type, DT_PLTREL);

	EXIT_NOT_IMPLEMENTED(jmprel_type != DT_RELA);
	if (jmprel_type == DT_RELA)
	{
		EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_JMPREL) && elf->HasDynValue(DT_JMPREL));
		EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_PLTRELSZ) && elf->HasDynValue(DT_PLTRELSZ));
		get_dyn_data_os(elf, &program->dynamic_info->jmprela_table, DT_OS_JMPREL);
		get_dyn_data(elf, program->base_vaddr, &program->dynamic_info->jmprela_table, DT_JMPREL);
		get_dyn_value(elf, &program->dynamic_info->jmprela_table_size, DT_OS_PLTRELSZ);
		get_dyn_value(elf, &program->dynamic_info->jmprela_table_size, DT_PLTRELSZ);
	}

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_RELA) && elf->HasDynValue(DT_RELA));
	get_dyn_data_os(elf, &program->dynamic_info->rela_table, DT_OS_RELA);
	get_dyn_data(elf, program->base_vaddr, &program->dynamic_info->rela_table, DT_RELA);
	get_dyn_value(elf, &program->dynamic_info->rela_table_total_size, DT_OS_RELASZ);
	get_dyn_value(elf, &program->dynamic_info->rela_table_total_size, DT_RELASZ);
	get_dyn_value(elf, &program->dynamic_info->rela_table_entry_size, DT_OS_RELAENT);
	get_dyn_value(elf, &program->dynamic_info->rela_table_entry_size, DT_RELAENT);

	get_dyn_value(elf, &program->dynamic_info->relative_count, DT_RELACOUNT);

	get_dyn_value(elf, &program->dynamic_info->debug, DT_DEBUG);
	get_dyn_value(elf, &program->dynamic_info->flags, DT_FLAGS);
	get_dyn_value(elf, &program->dynamic_info->textrel, DT_TEXTREL);

	EXIT_NOT_IMPLEMENTED(program->dynamic_info->debug != 0);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->textrel != 0);

	Vector<uint64_t> needed;
	get_dyn_values(elf, &needed, DT_NEEDED);
	for (auto need: needed)
	{
		program->dynamic_info->needed.Add(program->dynamic_info->str_table + need);
	}

	uint64_t so_name = 0;
	get_dyn_value(elf, &so_name, DT_SONAME);
	program->dynamic_info->so_name = program->dynamic_info->str_table + so_name;

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_NEEDED_MODULE) && elf->HasDynValue(DT_OS_NEEDED_MODULE_1));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_MODULE_INFO) && elf->HasDynValue(DT_OS_MODULE_INFO_1));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_IMPORT_LIB) && elf->HasDynValue(DT_OS_IMPORT_LIB_1));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_EXPORT_LIB) && elf->HasDynValue(DT_OS_EXPORT_LIB_1));
	get_dyn_modules(elf, &program->dynamic_info->import_modules, program->dynamic_info->str_table, DT_OS_NEEDED_MODULE);
	get_dyn_modules(elf, &program->dynamic_info->import_modules, program->dynamic_info->str_table, DT_OS_NEEDED_MODULE_1);
	get_dyn_modules(elf, &program->dynamic_info->export_modules, program->dynamic_info->str_table, DT_OS_MODULE_INFO);
	get_dyn_modules(elf, &program->dynamic_info->export_modules, program->dynamic_info->str_table, DT_OS_MODULE_INFO_1);
	get_dyn_libs(elf, &program->dynamic_info->import_libs, program->dynamic_info->str_table, DT_OS_IMPORT_LIB);
	get_dyn_libs(elf, &program->dynamic_info->import_libs, program->dynamic_info->str_table, DT_OS_IMPORT_LIB_1);
	get_dyn_libs(elf, &program->dynamic_info->export_libs, program->dynamic_info->str_table, DT_OS_EXPORT_LIB);
	get_dyn_libs(elf, &program->dynamic_info->export_libs, program->dynamic_info->str_table, DT_OS_EXPORT_LIB_1);
}

static void InstallRelocateHandler(Program* program)
{
	LUMEN_PROFILER_FUNCTION();

	uint64_t pltgot_vaddr = program->dynamic_info->pltgot_vaddr + program->base_vaddr;
	void**   pltgot       = reinterpret_cast<void**>(pltgot_vaddr);

	// These are privileged loader writes, not changes to guest data rights.
	// PatchReplace owns the VM/FEX mutation gates and restores the complete
	// physical envelope, including read-only siblings on 16 KiB hosts.
	// Its legacy false result also denotes an unchanged value; skip those
	// explicitly so a failed permission restoration cannot be treated as a
	// successful write merely because the new bytes are already visible.
	if (pltgot[1] != program)
		EXIT_IF(!Core::VirtualMemory::PatchReplace(pltgot_vaddr + sizeof(void*), reinterpret_cast<uint64_t>(program)));
	if (pltgot[2] != reinterpret_cast<void*>(RelocateHandler))
		EXIT_IF(!Core::VirtualMemory::PatchReplace(pltgot_vaddr + 2 * sizeof(void*), reinterpret_cast<uint64_t>(RelocateHandler)));

	// TODO(): check if this table already generated by compiler (sometimes it is missing)
	if (program->custom_call_plt_vaddr == 0)
	{
		program->custom_call_plt_num   = program->dynamic_info->jmprela_table_size / sizeof(Elf64_Rela);
		auto size                      = Jit::CallPlt::GetSize(program->custom_call_plt_num);
		program->custom_call_plt_vaddr = Core::VirtualMemory::Alloc(SYSTEM_RESERVED, size, Core::VirtualMemory::Mode::Write);
		EXIT_NOT_IMPLEMENTED(program->custom_call_plt_vaddr == 0);
		auto* code = new (reinterpret_cast<void*>(program->custom_call_plt_vaddr)) Jit::CallPlt(program->custom_call_plt_num);
		code->SetPltGot(pltgot_vaddr);
		EXIT_IF(!Core::VirtualMemory::Protect(program->custom_call_plt_vaddr, size, Core::VirtualMemory::Mode::Execute));
		EXIT_IF(!Core::VirtualMemory::FlushInstructionCache(program->custom_call_plt_vaddr, size));
	}
}

static void RebindPrelinkedPltForExperiment(Program* program)
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_REBIND_PRELINKED_PLT");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	if (!enabled) return;
	const auto* info = program->dynamic_info;
	if (info == nullptr || info->jmprela_table == nullptr || info->symbol_table == nullptr ||
	    info->jmprela_table_size % sizeof(Elf64_Rela) != 0 ||
	    info->symbol_table_total_size % sizeof(Elf64_Sym) != 0) return;
	const uint64_t count = info->jmprela_table_size / sizeof(Elf64_Rela);
	if (count < 5 || count > 65536) return;
	const auto* phdr = program->elf->GetPhdr();
	const auto phnum = program->elf->GetEhdr()->e_phnum;
	std::vector<uint64_t> slots;
	slots.reserve(static_cast<size_t>(count));
	for (uint64_t i = 0; i < count; ++i)
	{
		const auto& r = info->jmprela_table[i];
		if (r.GetType() != R_X86_64_JUMP_SLOT || r.r_addend != 0 ||
		    r.GetSymbol() >= info->symbol_table_total_size / sizeof(Elf64_Sym)) return;
		const auto& symbol = info->symbol_table[r.GetSymbol()];
		if (symbol.st_shndx != 0 ||
		    (symbol.GetType() != STT_FUNC && symbol.GetType() != STT_NOTYPE) ||
		    (symbol.GetBind() != STB_GLOBAL && symbol.GetBind() != STB_WEAK)) return;
		bool writable_slot = false;
		for (Elf64_Half p = 0; p < phnum; ++p)
		{
			const auto& segment = phdr[p];
			if ((segment.p_type == PT_OS_RELRO ||
			     (segment.p_type == PT_LOAD && (segment.p_flags & PF_W) != 0)) &&
			    segment.p_memsz >= sizeof(uint64_t) && r.r_offset >= segment.p_vaddr &&
			    r.r_offset - segment.p_vaddr <= segment.p_memsz - sizeof(uint64_t)) writable_slot = true;
		}
		if (!writable_slot) return;
		slots.push_back(r.r_offset);
	}
	std::vector<PrelinkedPlt::Repair> repairs;
	bool matched = false;
	for (Elf64_Half p = 0; p < phnum; ++p)
	{
		const auto& segment = phdr[p];
		if (segment.p_type != PT_LOAD || (segment.p_flags & (PF_R | PF_X)) != (PF_R | PF_X) ||
		    segment.p_filesz == 0 || segment.p_filesz > SIZE_MAX) continue;
		const auto plan = PrelinkedPlt::Find(
		    reinterpret_cast<const uint8_t*>(program->base_vaddr + segment.p_vaddr),
		    static_cast<size_t>(segment.p_filesz), segment.p_vaddr, program->base_size, slots);
		if (plan.ambiguous || (matched && plan.matched))
		{
			std::fprintf(stderr, "[RUNTIME_PRELINKED_PLT_REBIND] rejected=ambiguous\n");
			return;
		}
		matched = matched || plan.matched;
		if (!plan.repairs.empty()) repairs = plan.repairs;
	}
	// Bind the complete plan to the bytes recognized by Find before the first
	// write. This does not claim compare-and-swap or a cross-platform VM
	// transaction: PatchReplace remains the publication boundary for each entry.
	for (const auto& repair: repairs)
	{
		if (repair.offset > UINT64_MAX - program->base_vaddr)
		{
			EXIT("Invalid experimental prelinked PLT repair address\n");
		}
		const uint64_t address = program->base_vaddr + repair.offset;
		uint64_t current = 0;
		if (!ReadGuestMemory(address, &current, sizeof(current)))
		{
			std::fprintf(stderr,
			             "[RUNTIME_PRELINKED_PLT_REBIND] rejected=unreadable\n");
			return;
		}
		if (current != repair.expected)
		{
			std::fprintf(stderr,
			             "[RUNTIME_PRELINKED_PLT_REBIND] rejected=stale-plan\n");
			return;
		}
	}
	for (const auto& repair: repairs)
	{
		const uint64_t address = program->base_vaddr + repair.offset;
		uint64_t current = 0;
		if (!ReadGuestMemory(address, &current, sizeof(current)) ||
		    current != repair.expected)
		{
			EXIT("Experimental prelinked PLT changed during apply\n");
		}
		uint8_t replacement[8];
		std::memcpy(replacement, &repair.expected, sizeof(replacement));
		replacement[0] = 0xff;
		replacement[1] = 0x25;
		std::memcpy(replacement + 2, &repair.displacement, sizeof(repair.displacement));
		uint64_t value = 0;
		std::memcpy(&value, replacement, sizeof(value));
		if (!Core::VirtualMemory::PatchReplace(address, value) ||
		    !Core::VirtualMemory::FlushInstructionCache(address, sizeof(value)))
		{
			EXIT("Failed to rebind experimental prelinked PLT\n");
		}
	}
	if (!repairs.empty())
	{
		std::fprintf(stderr, "[RUNTIME_PRELINKED_PLT_REBIND] module=%s repaired=%zu\n",
		             program->file_name.FilenameWithoutDirectory().C_Str(), repairs.size());
	}
}

void RuntimeLinker::Relocate(Program* program)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr || program->relocations_applied);

	if (g_invalid_memory == 0)
	{
		g_invalid_memory = Core::VirtualMemory::Alloc(INVALID_MEMORY, 4096, Core::VirtualMemory::Mode::NoAccess);
		EXIT_NOT_IMPLEMENTED(g_invalid_memory == 0);
	}

	printf("--- Relocate program: " FG_WHITE BOLD "%s" DEFAULT " ---\n", program->file_name.C_Str());

	EXIT_NOT_IMPLEMENTED(program->dynamic_info->symbol_table_entry_size != sizeof(Elf64_Sym));
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->rela_table_entry_size != sizeof(Elf64_Rela));
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->jmprela_table == nullptr);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->rela_table == nullptr);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->symbol_table == nullptr);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->pltgot_vaddr == 0);

	RebindPrelinkedPltForExperiment(program);
	InstallRelocateHandler(program);

	relocate_all(program->dynamic_info->rela_table, program->dynamic_info->rela_table_total_size, program, false,
	             &program->unresolved_rela_indices);
	relocate_all(program->dynamic_info->jmprela_table, program->dynamic_info->jmprela_table_size, program, true,
	             &program->unresolved_jmprela_indices);
	program->relocations_applied = true;
}

void RuntimeLinker::RetryUnresolvedRelocations(Program* program)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr || !program->relocations_applied || program->dynamic_info == nullptr);
	retry_unresolved(program->dynamic_info->rela_table, program, false,
	                 &program->unresolved_rela_indices);
	retry_unresolved(program->dynamic_info->jmprela_table, program, true,
	                 &program->unresolved_jmprela_indices);
}

Program* RuntimeLinker::FindProgram(const ModuleId& m, const LibraryId& l,
	Program* requester)
{
	Core::LockGuard lock(m_mutex);

	for (auto* p: m_programs)
	{
		if (p->unload_in_progress && p != requester) continue;
		const auto& export_libs    = p->dynamic_info->export_libs;
		const auto& export_modules = p->dynamic_info->export_modules;

		if (export_libs.Contains(l) && export_modules.Contains(m))
		{
			return p;
		}
	}
	return nullptr;
}

const ModuleId* RuntimeLinker::FindModule(const Program& program, const String& id)
{
	const auto& import_modules = program.dynamic_info->import_modules;

	if (auto index = import_modules.Find(id, [](auto module, auto str) { return module.id == str; }); import_modules.IndexValid(index))
	{
		return &import_modules.At(index);
	}

	const auto& export_modules = program.dynamic_info->export_modules;

	if (auto index = export_modules.Find(id, [](auto module, auto str) { return module.id == str; }); export_modules.IndexValid(index))
	{
		return &export_modules.At(index);
	}

	return nullptr;
}

// void RuntimeLinker::CreateTls() {}

const LibraryId* RuntimeLinker::FindLibrary(const Program& program, const String& id)
{
	const auto& import_libs = program.dynamic_info->import_libs;

	if (auto index = import_libs.Find(id, [](auto lib, auto str) { return lib.id == str; }); import_libs.IndexValid(index))
	{
		return &import_libs.At(index);
	}

	const auto& export_libs = program.dynamic_info->export_libs;

	if (auto index = export_libs.Find(id, [](auto lib, auto str) { return lib.id == str; }); export_libs.IndexValid(index))
	{
		return &export_libs.At(index);
	}

	return nullptr;
}

void RuntimeLinker::CreateSymbolDatabase(Program* program)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr);
	EXIT_IF(program->export_symbols != nullptr);
	EXIT_IF(program->import_symbols != nullptr);

	program->export_symbols = new SymbolDatabase;
	program->import_symbols = new SymbolDatabase;

	auto syms = [](Program* program, SymbolDatabase* symbols, bool is_export)
	{
		if (program->dynamic_info->symbol_table == nullptr || program->dynamic_info->str_table == nullptr)
		{
			return;
		}

		for (auto* sym = program->dynamic_info->symbol_table;
		     reinterpret_cast<uint8_t*>(sym) <
		     reinterpret_cast<uint8_t*>(program->dynamic_info->symbol_table) + program->dynamic_info->symbol_table_total_size;
		     sym++)
		{
			String id   = String::FromUtf8(program->dynamic_info->str_table + sym->st_name);
			auto   bind = sym->GetBind();
			auto   type = sym->GetType();
			auto   ids  = id.Split(U'#');

			if (ids.Size() == 3)
			{
				const auto* l = FindLibrary(*program, ids.At(1));
				const auto* m = FindModule(*program, ids.At(2));

				if (l != nullptr && m != nullptr && (bind == STB_GLOBAL || bind == STB_WEAK) && (type == STT_FUNC || type == STT_OBJECT) &&
				    is_export == (sym->st_value != 0))
				{
					SymbolResolve sr {};
					sr.name                 = ids.At(0);
					sr.library              = l->name;
					sr.library_version      = l->version;
					sr.module               = m->name;
					sr.module_version_major = m->version_major;
					sr.module_version_minor = m->version_minor;
					switch (type)
					{
						case STT_NOTYPE: sr.type = SymbolType::NoType; break;
						case STT_FUNC: sr.type = SymbolType::Func; break;
						case STT_OBJECT: sr.type = SymbolType::Object; break;
						default: sr.type = SymbolType::Unknown; break;
					}
					symbols->Add(sr, (is_export ? sym->st_value + program->base_vaddr : 0));
				}
			}
		}
	};

	syms(program, program->export_symbols, true);
	syms(program, program->import_symbols, false);
}

void RuntimeLinker::SetupTlsHandler(Program* program)
{
	EXIT_IF(program == nullptr);
	EXIT_IF(g_tls_main_program != nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(program->elf->IsShared());
	EXIT_IF(program->tls.handler_vaddr == 0);

	g_tls_main_program = program;

	auto* code = new (reinterpret_cast<void*>(program->tls.handler_vaddr)) Jit::SafeCall;

	memset(g_tls_reg_save_area, 0, XSAVE_BUFFER_SIZE);
	std::memcpy(&g_tls_reg_save_area[XSAVE_BUFFER_SIZE], &XSAVE_CHK_GUARD, sizeof(XSAVE_CHK_GUARD));

	code->SetFunc(TlsMainGetAddr);
	code->SetRegSaveArea(g_tls_reg_save_area);
	code->SetLockVar(&g_tls_spinlock);

	EXIT_IF(!Core::VirtualMemory::Protect(program->tls.handler_vaddr, loader_tls_handler_size(), Core::VirtualMemory::Mode::Execute));
	EXIT_IF(!Core::VirtualMemory::FlushInstructionCache(program->tls.handler_vaddr, Jit::SafeCall::GetSize()));
}

void RuntimeLinker::DeleteTlss(int thread_id)
{
	// No Program pointer, loader lock, TLS lock or guest data borrow crosses
	// the optional reader donation. Enumerate the current registry only after
	// exclusive admission has been freshly obtained.
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
	if (g_tls_retirement_admission_test_hook != nullptr)
		g_tls_retirement_admission_test_hook(0, thread_id, 0);
#endif
	{
		GuestTlsMutation mutation(true, thread_id);
		Core::LockGuard lock(m_mutex);
		for (auto* p: m_programs) DeleteTls(p, thread_id);
	}
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
	if (g_tls_retirement_admission_test_hook != nullptr)
		g_tls_retirement_admission_test_hook(3, thread_id, 0);
#endif
}

} // namespace Lumen::Loader

#endif // LUMEN_EMU_ENABLED
