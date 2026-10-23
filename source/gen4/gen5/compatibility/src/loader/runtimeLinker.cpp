#include "loader/runtimeLinker.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/hostException.h"
#include "common/logging/log.h"
#include "common/platform/sysDbg.h"
#include "common/profiler.h"
#include "common/singleton.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "graphics/host_gpu/pageManager.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "loader/guestCallback.h"
#include "loader/guestMutationScope.h"
#include "loader/elf.h"
#include "loader/gamePatch.h"
#include "loader/jit.h"
#if defined(__x86_64__) || defined(_M_X64)
#include "loader/redZonePatcher.h"
#endif
#include "loader/symbolDatabase.h"
#include "loader/unresolvedImportFallback.h"
#if defined(__x86_64__) || defined(_M_X64)
#include "loader/x64InstructionEmulator.h"
#endif

#if defined(LUMEN_PS5_FEXCORE)
#include "GuestCpuRuntime.h"
#include "loader/guestExecution.h"
#include "loader/hostCall.h"
#if defined(LUMEN_PS5_DIRECT_FEX)
#include "DirectFexRuntime.h"
#endif
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <magic_enum.hpp>
#include <memory>
#include <thread>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#include <sys/uio.h>
#include <unistd.h>
#endif
#endif

namespace Libs::LibKernel {
void SetProgName(const std::string& name);
} // namespace Libs::LibKernel

namespace Loader {

Program::Program() = default;

Program::~Program() = default;

// Snapshot membership under the loader mutex, then keep objects alive without
// holding a loader lock or a translator mutation epoch across guest execution.
class ProgramCallPins {
public:
	ProgramCallPins() = default;
	ProgramCallPins(const ProgramCallPins&) = delete;
	ProgramCallPins& operator=(const ProgramCallPins&) = delete;

	~ProgramCallPins() {
		for (auto* program: m_programs) {
			program->active_guest_calls.fetch_sub(1, std::memory_order_release);
		}
	}

	void Add(Program* program) {
		m_programs.push_back(program);
		if (program->active_guest_calls.fetch_add(1, std::memory_order_acq_rel) == UINT32_MAX) {
			EXIT("PS5 module guest call pin count overflow\n");
		}
	}

	const std::vector<Program*>& Programs() const { return m_programs; }

private:
	std::vector<Program*> m_programs;
};

static void FreeTlsBlock(ThreadLocalStorage::Block* block) {
	if (block == nullptr || block->ptr == nullptr) {
		return;
	}

	if (block->free_func != nullptr) {
		GuestCallback::Call(block->free_func, block->ptr);
	} else if (block->vm_alloc) {
		if (!Libs::LibKernel::Memory::FreeGuestMemory(reinterpret_cast<uint64_t>(block->ptr),
		                                            block->alloc_size)) {
			EXIT("PS5 TLS memory release failed\n");
		}
	} else {
		delete[] block->ptr;
	}

	block->ptr        = nullptr;
	block->free_func  = nullptr;
	block->vm_alloc   = false;
	block->alloc_size = 0;
}

static uint64_t AlignUp(uint64_t value, uint64_t alignment) {
	return alignment != 0 ? (value + alignment - 1) & ~(alignment - 1) : value;
}

ThreadLocalStorage::~ThreadLocalStorage() {
	for (auto& [_, block]: tlss) {
		FreeTlsBlock(&block);
	}
}

static void RejectGuestHeapTlsTeardown(Program* program) {
#if defined(LUMEN_PS5_FEXCORE)
	Common::LockGuard lock(program->tls.mutex);
	for (const auto& [_, block]: program->tls.tlss) {
		if (block.ptr != nullptr && block.free_func != nullptr) {
			EXIT("PS5 module teardown does not support callback-backed TLS; "
			     "guest heap callbacks require a separate lifetime contract\n");
		}
	}
#else
	(void)program;
#endif
}

#pragma pack(1)

struct EntryParams {
	int         argc;
	uint32_t    pad;
	const char* argv[3];
};

#pragma pack()

using atexit_func_t = KYTY_SYSV_ABI void (*)();
using entry_func_t  = KYTY_SYSV_ABI void (*)(EntryParams* params, atexit_func_t atexit_func);
using module_ini_fini_func_t = KYTY_SYSV_ABI int (*)(size_t args, const void* argp,
                                                     module_func_t func);

enum class BindType { Unknown, Local, Global, Weak };

struct RelocationInfo {
	bool        resolved   = false;
	BindType    bind       = BindType::Unknown;
	SymbolType  type       = SymbolType::Unknown;
	uint64_t    value      = 0;
	uint64_t    vaddr      = 0;
	uint64_t    base_vaddr = 0;
	std::string name;
	std::string dbg_name;
	bool        bind_self = false;
};

static bool PatchGuestMemory64(uint64_t vaddr, uint64_t value) {
#if defined(LUMEN_PS5_FEXCORE)
	bool changed = false;
	if (!Libs::LibKernel::Memory::PatchProgramMemory64(vaddr, value, &changed)) {
		EXIT("PS5 relocation write denied at 0x%016" PRIx64 "\n", vaddr);
	}
	return changed;
#else
	auto* ptr     = reinterpret_cast<uint64_t*>(vaddr);
	bool  changed = (*ptr != value);
	std::memcpy(ptr, &value, sizeof(value));
	return changed;
#endif
}

constexpr uint64_t SYSTEM_RESERVED  = 0x800000000u;
constexpr uint64_t CODE_BASE_INCR   = 0x010000000u;
constexpr uint64_t CODE_BASE_OFFSET = 0x100000000u;

static uint64_t g_desired_base_addr = SYSTEM_RESERVED + CODE_BASE_OFFSET;

static Program*              g_tls_main_program        = nullptr;
static thread_local Program* g_tls_cached_main_program = nullptr;
static thread_local uint8_t* g_tls_cached_main_tcb     = nullptr;

static KYTY_SYSV_ABI void RunEntry(uint64_t addr, EntryParams* params, atexit_func_t atexit_func,
                                   void* stack_top) {
#if defined(LUMEN_PS5_FEXCORE)
	std::fprintf(stderr, "PS5_GUEST_ENTRY_ATTEMPT target=%#llx backend=%s\n",
	             static_cast<unsigned long long>(addr),
#if defined(LUMEN_PS5_DIRECT_FEX)
                 "gen4-direct-fex");
#else
                 Lumen::GuestCpu::ProviderName());
#endif
	const auto native_atexit = reinterpret_cast<uint64_t>(atexit_func);
#if defined(LUMEN_PS5_DIRECT_FEX)
    const auto guest_atexit = native_atexit != 0 && IsHostCallTarget(native_atexit)
        ? Lumen::Ps5::DirectFex::ResolveHostTarget(native_atexit) : native_atexit;
#else
    const auto guest_atexit = native_atexit;
#endif
	const uint64_t arguments[] {reinterpret_cast<uint64_t>(params), guest_atexit};
	uint64_t result {};
	if (!GuestExecution::Invoke(addr, arguments, 2, &result,
	                            reinterpret_cast<uint64_t>(stack_top))) {
		EXIT("PS5 guest entry failed\n");
	}
	std::fprintf(stderr, "PS5_GUEST_ENTRY_RETURNED value=%#llx\n",
	             static_cast<unsigned long long>(result));
#elif defined(__x86_64__) || defined(_M_X64)
	auto* func = reinterpret_cast<entry_func_t>(addr);

	if (stack_top != nullptr) {
		const auto aligned_stack_top =
		    reinterpret_cast<uintptr_t>(stack_top) & ~static_cast<uintptr_t>(0x0f);
		const auto guest_rsp = aligned_stack_top - 2u * sizeof(uintptr_t);
		const auto guest_rbp = guest_rsp;

		auto* guest_root_frame = reinterpret_cast<uintptr_t*>(guest_rbp);
		guest_root_frame[0]    = 0;
		guest_root_frame[1]    = 0;

#if defined(__APPLE__)
		// Clang on macOS can allocate plain "r" inputs to r12/r13, which the template
		// clobbers before consuming them. Pin the inputs to registers the SysV guest
		// preserves without changing register allocation on Windows or Linux.
		register entry_func_t func_reg asm("rbx")      = func;
		register uintptr_t    guest_rsp_reg asm("r14") = guest_rsp;
		register uintptr_t    guest_rbp_reg asm("r15") = guest_rbp;
#endif

#if defined(__APPLE__)
		asm volatile(
		    "pushq %%r12\n\t"
		    "pushq %%r13\n\t"
		    "movq %%rsp, %%r12\n\t"
		    "movq %%rbp, %%r13\n\t"
		    "movq %[guest_rsp], %%rsp\n\t"
		    "movq %[guest_rbp], %%rbp\n\t"
		    "callq *%[func]\n\t"
		    "movq %%r13, %%rbp\n\t"
		    "movq %%r12, %%rsp\n\t"
		    "popq %%r13\n\t"
		    "popq %%r12\n\t"
		    :
		    : [func] "r"(func_reg), "D"(params),
		      "S"(atexit_func), [guest_rsp] "r"(guest_rsp_reg), [guest_rbp] "r"(guest_rbp_reg)
		    : "cc", "memory", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2",
		      "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",
		      "xmm13", "xmm14", "xmm15");
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		// Windows stack probes use the TEB stack limits during the guest stack switch.
		// bounds, which describe the host stack and are invalid while RSP is in guest memory.
		register entry_func_t func_reg asm("rbx")     = func;
		register uintptr_t    guest_rsp_reg asm("r8") = guest_rsp;
		register uintptr_t    guest_rbp_reg asm("r9") = guest_rbp;
		asm volatile("pushq %%r12\n\t"
		             "pushq %%r13\n\t"
		             "pushq %%r14\n\t"
		             "pushq %%r15\n\t"
		             "movq %%gs:0x08, %%r14\n\t"
		             "movq %%gs:0x10, %%r15\n\t"
		             "xorq %%rcx, %%rcx\n\t"
		             "movq %%rcx, %%gs:0x08\n\t"
		             "movq %%rcx, %%gs:0x10\n\t"
		             "movq %%rsp, %%r12\n\t"
		             "movq %%rbp, %%r13\n\t"
		             "movq %[guest_rsp], %%rsp\n\t"
		             "movq %[guest_rbp], %%rbp\n\t"
		             "callq *%[func]\n\t"
		             "movq %%r13, %%rbp\n\t"
		             "movq %%r12, %%rsp\n\t"
		             "movq %%r14, %%gs:0x08\n\t"
		             "movq %%r15, %%gs:0x10\n\t"
		             "popq %%r15\n\t"
		             "popq %%r14\n\t"
		             "popq %%r13\n\t"
		             "popq %%r12\n\t"
		             : [guest_rsp] "+r"(guest_rsp_reg), [guest_rbp] "+r"(guest_rbp_reg)
		             : [func] "r"(func_reg), "D"(params), "S"(atexit_func)
		             : "cc", "memory", "rax", "rcx", "rdx", "r10", "r11", "xmm0", "xmm1", "xmm2",
		               "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
		               "xmm12", "xmm13", "xmm14", "xmm15");
#else
		// Clobbers prevent inputs from being allocated to r12/r13.
		asm volatile("movq %%rsp, %%r12\n\t"
		             "movq %%rbp, %%r13\n\t"
		             "movq %[guest_rsp], %%rsp\n\t"
		             "movq %[guest_rbp], %%rbp\n\t"
		             "callq *%[func]\n\t"
		             "movq %%r13, %%rbp\n\t"
		             "movq %%r12, %%rsp\n\t"
		             :
		             : [func] "r"(func), "D"(params),
		               "S"(atexit_func), [guest_rsp] "r"(guest_rsp), [guest_rbp] "r"(guest_rbp)
		             : "cc", "memory", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12", "r13",
		               "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8",
		               "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
#endif
		return;
	}

	uintptr_t guest_root_frame[2] = {};

#if defined(__APPLE__)
	register entry_func_t func_reg asm("rbx")      = func;
	register uintptr_t    guest_rbp_reg asm("r14") = reinterpret_cast<uintptr_t>(guest_root_frame);
#endif

#if defined(__APPLE__) || KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	asm volatile("pushq %%r12\n\t"
	             "pushq %%r13\n\t"
	             "movq %%rbp, %%r12\n\t"
	             "movq %[guest_rbp], %%rbp\n\t"
	             "callq *%[func]\n\t"
	             "movq %%r12, %%rbp\n\t"
	             "popq %%r13\n\t"
	             "popq %%r12\n\t"
	             :
#if defined(__APPLE__)
	             : [func] "r"(func_reg), "D"(params),
	               "S"(atexit_func), [guest_rbp] "r"(guest_rbp_reg)
#else
	             : [func] "r"(func), "D"(params),
	               "S"(atexit_func), [guest_rbp] "r"(guest_root_frame)
#endif
	             : "cc", "memory", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "xmm0", "xmm1",
	               "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
	               "xmm12", "xmm13", "xmm14", "xmm15");
#else
	// Keep inputs out of r12.
	asm volatile("movq %%rbp, %%r12\n\t"
	             "movq %[guest_rbp], %%rbp\n\t"
	             "callq *%[func]\n\t"
	             "movq %%r12, %%rbp\n\t"
	             :
	             : [func] "r"(func), "D"(params),
	               "S"(atexit_func), [guest_rbp] "r"(guest_root_frame)
	             : "cc", "memory", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12", "xmm0",
	               "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10",
	               "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
#endif
#else
	(void)addr;
	(void)params;
	(void)atexit_func;
	(void)stack_top;
	EXIT("Native calls to x86-64 guest entry are unavailable on this host; PS5 requires FEX\n");
#endif
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
#if defined(__x86_64__) || defined(_M_X64)
// This fixture calls the native x86 entry thunk. ARM runs translated entry/stack
// coverage through the separate FEX runtime tests instead.
struct MainEntryStackTestState {
	bool      called = false;
	uintptr_t rsp    = 0;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uintptr_t teb_stack_base  = UINTPTR_MAX;
	uintptr_t teb_stack_limit = UINTPTR_MAX;
#endif
};

static KYTY_SYSV_ABI void TestMainEntryStackCallback(EntryParams* params,
                                                     atexit_func_t /*atexit_func*/) {
	auto* state = reinterpret_cast<MainEntryStackTestState*>(const_cast<char*>(params->argv[0]));
	asm volatile("pushq %%r15\n\t"
	             "pushq %%r14\n\t"
	             "popq %%r14\n\t"
	             "popq %%r15\n\t"
	             :
	             :
	             : "memory");
	asm volatile("movq %%rsp, %0" : "=r"(state->rsp) : : "memory");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	asm volatile("movq %%gs:0x08, %0\n\t"
	             "movq %%gs:0x10, %1\n\t"
	             : "=r"(state->teb_stack_base), "=r"(state->teb_stack_limit)
	             :
	             : "memory");
#endif
	state->called = true;
}

bool TestMainEntryUsesGuestStack() {
	constexpr uint64_t stack_size = 0x10000;
	const auto         stack_base = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    0, stack_size, Common::VirtualMemory::Mode::ReadWrite, "main_entry_stack_test");
	if (stack_base == 0) {
		return false;
	}

	MainEntryStackTestState state {};
	EntryParams             params {};
	params.argv[0] = reinterpret_cast<const char*>(&state);
	std::memset(reinterpret_cast<void*>(stack_base), 0xcd, stack_size);
	auto* root_frame = reinterpret_cast<const uintptr_t*>(stack_base + stack_size) - 2;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uintptr_t original_teb_stack_base  = 0;
	uintptr_t original_teb_stack_limit = 0;
	asm volatile("movq %%gs:0x08, %0\n\t"
	             "movq %%gs:0x10, %1\n\t"
	             : "=r"(original_teb_stack_base), "=r"(original_teb_stack_limit)
	             :
	             : "memory");
#endif

	RunEntry(reinterpret_cast<uint64_t>(TestMainEntryStackCallback), &params, nullptr,
	         reinterpret_cast<void*>(stack_base + stack_size));

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uintptr_t restored_teb_stack_base  = 0;
	uintptr_t restored_teb_stack_limit = 0;
	asm volatile("movq %%gs:0x08, %0\n\t"
	             "movq %%gs:0x10, %1\n\t"
	             : "=r"(restored_teb_stack_base), "=r"(restored_teb_stack_limit)
	             :
	             : "memory");
	const bool teb_ok = state.teb_stack_base == 0 && state.teb_stack_limit == 0 &&
	                    restored_teb_stack_base == original_teb_stack_base &&
	                    restored_teb_stack_limit == original_teb_stack_limit;
#else
	constexpr bool teb_ok = true;
#endif

	const bool rsp_ok  = state.rsp >= stack_base && state.rsp < stack_base + stack_size;
	const bool root_ok = root_frame[0] == 0 && root_frame[1] == 0;
	const bool freed   = Libs::LibKernel::Memory::FreeGuestMemory(stack_base, stack_size);
	return state.called && rsp_ok && root_ok && teb_ok && freed;
}
#endif // native x86-64 entry-stack fixture

bool TestModuleRelocationUsesWritableHostMapping() {
	constexpr uint64_t page_size = 0x4000;
	constexpr uint64_t value     = 0x4b59545950415443;
	const auto         base      = Libs::LibKernel::Memory::AllocateProgramMemory(
	    0, page_size, Common::VirtualMemory::Mode::ReadWrite, "host_only_patch_test");
	if (base == 0) {
		return false;
	}
	Libs::LibKernel::Memory::SetProgramMemoryProtection(base, page_size,
	                                                    Common::VirtualMemory::Mode::Read);

	Libs::LibKernel::Memory::VirtualQueryInfo before {};
	Libs::LibKernel::Memory::VirtualQueryInfo after {};
	const bool                                before_ok =
	    Libs::LibKernel::Memory::KernelVirtualQuery(reinterpret_cast<const void*>(base), 0, &before,
	                                                sizeof(before)) == 0;
	const bool changed  = PatchGuestMemory64(base, value);
	const bool after_ok = Libs::LibKernel::Memory::KernelVirtualQuery(
	                          reinterpret_cast<const void*>(base), 0, &after, sizeof(after)) == 0;
	const bool value_ok = *reinterpret_cast<const uint64_t*>(base) == value;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	MEMORY_BASIC_INFORMATION mbi {};
	const bool               host_mode_ok =
	    VirtualQuery(reinterpret_cast<const void*>(base), &mbi, sizeof(mbi)) != 0 &&
	    mbi.Protect == PAGE_READWRITE;
#else
	constexpr bool host_mode_ok = true;
#endif
	const bool freed = Libs::LibKernel::Memory::FreeGuestMemory(base, page_size);

	return before_ok && after_ok && changed && value_ok && host_mode_ok && freed &&
	       before.protection == after.protection;
}
#endif

static uint64_t GetAlignedSize(const Elf64_Phdr* p) {
	return (p->p_align != 0 ? (p->p_memsz + (p->p_align - 1)) & ~(p->p_align - 1) : p->p_memsz);
}

static void DbgDumpSymbols(const std::string& folder, Elf64_Sym* symbols, uint64_t size,
                           const char* names) {
	auto folder_str = Common::FixDirectorySlash(folder);

	Common::File::CreateDirectories(folder_str);

	Common::File f;
	f.Create(folder_str + "symbols.txt");

	for (auto* sym = symbols;
	     reinterpret_cast<uint8_t*>(sym) < reinterpret_cast<uint8_t*>(symbols) + size; sym++) {
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

static void DbgDumpRela(const std::string& folder, Elf64_Rela* records, uint64_t size,
                        const char* /*names*/, const char* file_name) {
	auto folder_str = Common::FixDirectorySlash(folder);

	Common::File::CreateDirectories(folder_str);

	Common::File f;
	f.Create(folder_str + file_name);

	for (auto* r = records;
	     reinterpret_cast<uint8_t*>(r) < reinterpret_cast<uint8_t*>(records) + size; r++) {
		f.Printf("----\n"
		         "r_offset = 0x%016" PRIx64 "\n"
		         "r_info = 0x%016" PRIx64 "\n"
		         "r_addend = %" PRId64 "\n",
		         r->r_offset, r->r_info, r->r_addend);
	}

	f.Close();
}

static Common::VirtualMemory::Mode GetMode(Elf64_Word flags) {
	switch (flags) {
		case PF_R: return Common::VirtualMemory::Mode::Read;
		case PF_W: return Common::VirtualMemory::Mode::Write;
		case PF_R | PF_W: return Common::VirtualMemory::Mode::ReadWrite;
		case PF_X: return Common::VirtualMemory::Mode::Execute;
		case PF_X | PF_R: return Common::VirtualMemory::Mode::ExecuteRead;
		case PF_X | PF_W: return Common::VirtualMemory::Mode::ExecuteWrite;
		case PF_X | PF_W | PF_R: return Common::VirtualMemory::Mode::ExecuteReadWrite;

		default: return Common::VirtualMemory::Mode::NoAccess;
	}
}

struct GuestStackFrame {
	GuestStackFrame* next;
	uintptr_t        return_address;
};

static bool IsReadableRange(uint64_t addr, uint64_t size);

static int WalkGuestStack(uint64_t rbp, uint64_t rsp, void** stack, int capacity) {
	constexpr uintptr_t STACK_SIZE = 1024u * 1024u;
	const uintptr_t     code_start = SYSTEM_RESERVED + CODE_BASE_OFFSET;
	const uintptr_t     stack_end  = rsp + STACK_SIZE;
	if (stack == nullptr || capacity <= 0 || rsp == 0 || rbp < rsp || stack_end < rsp ||
	    g_desired_base_addr <= code_start) {
		return 0;
	}

	auto* frame = reinterpret_cast<GuestStackFrame*>(rbp);

	int depth = 0;
	while (depth < capacity) {
		const auto frame_addr = reinterpret_cast<uintptr_t>(frame);
		if (frame_addr < rsp || frame_addr > stack_end - sizeof(GuestStackFrame) ||
		    !IsReadableRange(frame_addr, sizeof(GuestStackFrame)) ||
		    frame->return_address < code_start || frame->return_address >= g_desired_base_addr) {
			break;
		}
		stack[depth++] = reinterpret_cast<void*>(frame->return_address);
		if (reinterpret_cast<uintptr_t>(frame->next) <= frame_addr) {
			break;
		}
		frame = frame->next;
	}
	return depth;
}

// Probe diagnostic ranges without raising another fault.
static bool IsReadableRange(uint64_t addr, uint64_t size) {
	if (addr == 0 || size == 0) {
		return false;
	}

	const uint64_t end = addr + size;
	if (end < addr) {
		return false;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uint64_t current = addr;
	while (current < end) {
		MEMORY_BASIC_INFORMATION mbi {};
		if (VirtualQuery(reinterpret_cast<const void*>(current), &mbi, sizeof(mbi)) == 0 ||
		    mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
			return false;
		}
		const auto region_end = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
		if (region_end <= current) {
			return false;
		}
		current = std::min(region_end, end);
	}
#elif defined(__APPLE__)
	// Walk the Mach regions covering the range and require read permission. The fatal
	// report dumps memory behind raw register values, and a fault inside the reporter
	// re-enters the signal handler and wedges the reporting thread.
	uint64_t current = addr;
	while (current < end) {
		mach_vm_address_t              region_addr = current;
		mach_vm_size_t                 region_size = 0;
		vm_region_basic_info_data_64_t info {};
		mach_msg_type_number_t         count       = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t                    object_name = MACH_PORT_NULL;
		if (mach_vm_region(mach_task_self(), &region_addr, &region_size, VM_REGION_BASIC_INFO_64,
		                   reinterpret_cast<vm_region_info_t>(&info), &count,
		                   &object_name) != KERN_SUCCESS ||
		    region_addr > current || (info.protection & VM_PROT_READ) == 0) {
			return false;
		}
		current = region_addr + region_size;
	}
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	const auto page_size = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
	if (page_size == 0) {
		return false;
	}

	for (uint64_t current = addr; current < end;) {
		uint8_t probe = 0;

		iovec local {&probe, sizeof(probe)};
		iovec remote {reinterpret_cast<void*>(current), sizeof(probe)};

		if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) !=
		    static_cast<ssize_t>(sizeof(probe))) {
			return false;
		}

		const uint64_t next = (current & ~(page_size - 1)) + page_size;
		if (next <= current) { // wrapped at the top of the address space
			break;
		}
		current = next;
	}
#else
	(void)end;
#endif
	return true;
}

static bool KytyExceptionHandler(const Common::HostException::ExceptionInfo& exception_info) {
	const auto* info = &exception_info;
	if (info->context_architecture !=
	    Common::HostException::ContextArchitecture::NativeX86_64) {
		// FEX owns translated guest faults. A native ARM64 ucontext contains
		// host state and must never be decoded or modified as an x86 context.
		std::fprintf(stderr,
		             "Unhandled native host exception: architecture=%u type=%u pc=0x%016" PRIx64
		             " access=%u address=0x%016" PRIx64 "\n",
		             static_cast<unsigned>(info->context_architecture),
		             static_cast<unsigned>(info->type), info->exception_address,
		             static_cast<unsigned>(info->access_violation_type),
		             info->access_violation_vaddr);
		return false;
	}

#if defined(__x86_64__) || defined(_M_X64)
	if (info->type == Common::HostException::ExceptionType::IllegalInstruction &&
	    Loader::X64InstructionEmulator::TryEmulate(info->native_context)) {
		return true;
	}
#endif

	if (info->type == Common::HostException::ExceptionType::AccessViolation) {
		using CoreAccess = Common::HostException::AccessViolationType;
		using GpuAccess  = Libs::Graphics::PageFaultAccess;
		GpuAccess access;
		switch (info->access_violation_type) {
			case CoreAccess::Read: access = GpuAccess::Read; break;
			case CoreAccess::Write: access = GpuAccess::Write; break;
			case CoreAccess::Execute: access = GpuAccess::Execute; break;
			case CoreAccess::Unknown: return false;
		}
		if (Libs::LibKernel::Memory::HandleGpuFault(access, info->access_violation_vaddr)) {
			return true;
		}
	}
	// Report whatever guest context can be read safely before terminating: which guest thread
	// faulted, the register file, the faulting code bytes and the top of its stack.
	{
		char thread_name[64] = "(host thread)";
		if (auto self = Libs::LibKernel::PthreadSelfOrNull(); self != nullptr) {
			if (Libs::LibKernel::PthreadGetname(self, thread_name) != 0) {
				std::snprintf(thread_name, sizeof(thread_name), "(unnamed guest thread)");
			}
		}
		std::printf("--- Guest fault context ---\n");
		std::printf("thread: %s\n", thread_name);
		std::printf("rax=%016" PRIx64 " rbx=%016" PRIx64 " rcx=%016" PRIx64 " rdx=%016" PRIx64 "\n"
		            "rsi=%016" PRIx64 " rdi=%016" PRIx64 " rbp=%016" PRIx64 " rsp=%016" PRIx64 "\n"
		            "r8 =%016" PRIx64 " r9 =%016" PRIx64 " r10=%016" PRIx64 " r11=%016" PRIx64 "\n"
		            "r12=%016" PRIx64 " r13=%016" PRIx64 " r14=%016" PRIx64 " r15=%016" PRIx64 "\n",
		            info->rax, info->rbx, info->rcx, info->rdx, info->rsi, info->rdi, info->rbp,
		            info->rsp, info->r8, info->r9, info->r10, info->r11, info->r12, info->r13,
		            info->r14, info->r15);
		if (IsReadableRange(info->exception_address - 48, 96)) {
			const auto* code = reinterpret_cast<const uint8_t*>(info->exception_address - 48);
			std::printf("code (pc-48 .. pc+48, fault at byte 48):");
			for (int i = 0; i < 96; i++) {
				std::printf("%s%02x", (i % 16 == 0) ? "\n " : " ", code[i]);
			}
			std::printf("\n");
		}
		if (IsReadableRange(info->rsp, 32 * sizeof(uint64_t))) {
			const auto* stack = reinterpret_cast<const uint64_t*>(info->rsp);
			std::printf("stack:");
			for (int i = 0; i < 32; i++) {
				std::printf("%s %016" PRIx64, (i % 4 == 0) ? "\n " : "", stack[i]);
			}
			std::printf("\n");
		}
		std::fflush(stdout);
	}
	EXIT("Unhandled host exception: type=%u code=%u pc=0x%016" PRIx64
	     " access=%u address=0x%016" PRIx64 "\n",
	     static_cast<unsigned>(info->type), info->native_code, info->exception_address,
	     static_cast<unsigned>(info->access_violation_type), info->access_violation_vaddr);
	__builtin_unreachable();
}

static void EncodeId64(uint16_t in_id, std::string* out_id) {
	static const char* str = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
	if (in_id < 0x40u) {
		*out_id += str[in_id];
	} else {
		if (in_id < 0x1000u) {
			*out_id += str[static_cast<uint16_t>(in_id >> 6u) & 0x3fu];
			*out_id += str[in_id & 0x3fu];
		} else {
			*out_id += str[static_cast<uint16_t>(in_id >> 12u) & 0x3fu];
			*out_id += str[static_cast<uint16_t>(in_id >> 6u) & 0x3fu];
			*out_id += str[in_id & 0x3fu];
		}
	}
}

template <class T>
static void GetDynDataOs(Elf64* elf, T* out, Elf64_Sxword tag) {
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr) {
		*out = elf->GetDynamicData<T>(dyn->d_un.d_ptr);
	}
}

template <class T>
static void GetDynData(Elf64* elf, uint64_t base_vaddr, T* out, Elf64_Sxword tag) {
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr) {
		*out = reinterpret_cast<T>(base_vaddr + dyn->d_un.d_ptr);
	}
}

template <class T>
static void GetDynValue(Elf64* elf, T* out, Elf64_Sxword tag) {
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr) {
		*out = dyn->d_un.d_val;
	}
}

template <class T>
static void GetDynValues(Elf64* elf, T* out, Elf64_Sxword tag) {
	for (const auto* dyn: elf->GetDynList(tag)) {
		out->push_back(dyn->d_un.d_val);
	}
}

template <class T>
static void GetDynPtr(Elf64* elf, T* out, Elf64_Sxword tag) {
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr) {
		*out = dyn->d_un.d_ptr;
	}
}

static void KYTY_SYSV_ABI ProgramExitHandler() {
	Common::Singleton<RuntimeLinker>::Instance()->StopAllModules();

	LOGF("exit!!!\n");
}

template <class T>
static void GetDynModules(Elf64* elf, T* out, const char* names, Elf64_Sxword tag) {
	std::vector<uint64_t> needed_modules;
	GetDynValues(elf, &needed_modules, tag);
	for (auto need: needed_modules) {
		ModuleId id {};
		// id.id            = static_cast<int>((need >> 48u) & 0xffffu);
		EncodeId64(static_cast<uint16_t>((need >> 48u) & 0xffffu), &id.id);
		id.version_major = static_cast<int>((need >> 40u) & 0xffu);
		id.version_minor = static_cast<int>((need >> 32u) & 0xffu);
		id.name          = names + (need & 0xffffffff);
		out->push_back(id);
	}
}

template <class T>
static void GetDynLibs(Elf64* elf, T* out, const char* names, Elf64_Sxword tag) {
	std::vector<uint64_t> needed_modules;
	GetDynValues(elf, &needed_modules, tag);
	for (auto need: needed_modules) {
		LibraryId id {};
		// id.id      = static_cast<int>((need >> 48u) & 0xffffu);
		EncodeId64(static_cast<uint16_t>((need >> 48u) & 0xffffu), &id.id);
		id.version = static_cast<int>((need >> 32u) & 0xffffu);
		id.name    = names + (need & 0xffffffff);
		out->push_back(id);
	}
}

static RelocationInfo GetRelocationInfo(Elf64_Rela* r, Program* program) {
	KYTY_PROFILER_FUNCTION();

	RelocationInfo ret;
	// SymbolRecord   sr {};

	auto         type    = r->GetType();
	auto         symbol  = r->GetSymbol();
	Elf64_Sxword addend  = r->r_addend;
	auto*        symbols = program->dynamic_info->symbol_table;
	auto*        names   = program->dynamic_info->str_table;
	ret.base_vaddr       = program->base_vaddr;
	ret.vaddr            = ret.base_vaddr + r->r_offset;
	ret.bind_self        = false;

	switch (type) {
		case R_X86_64_GLOB_DAT:
		case R_X86_64_JUMP_SLOT: addend = 0; [[fallthrough]];
		case R_X86_64_64: {
			auto         sym          = symbols[symbol];
			auto         bind         = sym.GetBind();
			auto         sym_type     = sym.GetType();
			uint64_t     symbol_vaddr = 0;
			SymbolRecord sr {};
			switch (sym_type) {
				case STT_NOTYPE: ret.type = SymbolType::NoType; break;
				case STT_FUNC: ret.type = SymbolType::Func; break;
				case STT_OBJECT: ret.type = SymbolType::Object; break;
				default: EXIT("unknown symbol type: %d\n", (int)sym_type);
			}
			const auto lookup_type = FunctionImportLookupType(ret.type,
			                                                   type == R_X86_64_JUMP_SLOT);
			switch (bind) {
				case STB_LOCAL:
					symbol_vaddr = ret.base_vaddr + sym.st_value;
					ret.bind     = BindType::Local;
					break;
				case STB_GLOBAL: ret.bind = BindType::Global; [[fallthrough]];
				case STB_WEAK: {
					ret.bind = (ret.bind == BindType::Unknown ? BindType::Weak : ret.bind);
					ret.name = names + sym.st_name;
					program->rt->Resolve(ret.name, lookup_type, program, &sr, &ret.bind_self);
					if (sr.vaddr == 0 && ret.type == SymbolType::NoType &&
					    type == R_X86_64_JUMP_SLOT) {
						SymbolRecord untyped {};
						bool untyped_self = false;
						program->rt->Resolve(ret.name, SymbolType::NoType, program,
						                     &untyped, &untyped_self);
						// Preserve callable untyped guest exports, while never
						// admitting an untyped native object as a host function.
						if (Libs::LibKernel::Memory::QueryGuestAccess(
						        untyped.vaddr, 1, Common::VirtualMemory::Mode::Execute)) {
							sr = untyped;
							ret.bind_self = untyped_self;
						}
					}
					symbol_vaddr = sr.vaddr;
				} break;
				default: EXIT("unknown bind: %d\n", (int)bind);
			}
#if defined(LUMEN_PS5_FEXCORE)
			if (lookup_type == SymbolType::Func &&
			    IsHostCallTarget(symbol_vaddr) &&
			    !IsHostCallSupported(symbol_vaddr)) {
				EXIT("PS5 HLE import requires unsupported ABI: %s (%s)\n",
				     ret.name.c_str(), sr.dbg_name.c_str());
			}
#endif
			ret.resolved = (symbol_vaddr != 0);
			ret.value    = (ret.resolved ? symbol_vaddr + addend : 0);
			ret.name     = sr.name.empty() ? names + sym.st_name : sr.name;
			ret.dbg_name = sr.dbg_name;
		} break;
		case R_X86_64_RELATIVE:
			ret.value    = ret.base_vaddr + addend;
			ret.resolved = true;
			break;
		case R_X86_64_DTPMOD64:
			ret.value    = reinterpret_cast<uint64_t>(program);
			ret.resolved = true;
			ret.type     = SymbolType::TlsModule;
			ret.bind     = BindType::Local;
			ret.dbg_name = Common::PathToString(program->file_name);
			break;
		default: EXIT("unknown type: %d\n", (int)type);
	}

	return ret;
}

static bool RelocateRecord(uint32_t index, Elf64_Rela* r, Program* program, bool jmprela_table) {
	KYTY_PROFILER_FUNCTION();

	const auto ri = GetRelocationInfo(r, program);
	auto value = ri.value;
	if (!ri.resolved && ri.bind != BindType::Weak) {
		if (FunctionFallbackEligible(program->rt->UpstreamFunctionFallbackEnabled(),
		                             ri.type, false, r->GetType() == R_X86_64_JUMP_SLOT,
		                             r->GetType() == R_X86_64_64 ? r->r_addend : 0)) {
			value = RegisterFunctionFallback(program->unique_id, ri.vaddr, ri.name.c_str(),
			    Common::PathToString(program->file_name).c_str(), index, jmprela_table);
		} else {
		EXIT("Unresolved import: symbol=%s type=%s bind=%s program=%s relocation=%u table=%s\n",
		     ri.name.c_str(), magic_enum::enum_name(ri.type).data(),
		     magic_enum::enum_name(ri.bind).data(), Common::PathToString(program->file_name).c_str(),
		     index, jmprela_table ? "PLT" : "non-PLT");
		}
	}
	// ELF weak references retain null. Compatibility thunks remain unresolved
	// in rela_bits so a later module export can replace them.
	const bool patched = PatchGuestMemory64(ri.vaddr, value);

	if (program->dbg_print_reloc && patched && !ri.bind_self &&
	    (ri.bind == BindType::Global || ri.bind == BindType::Weak ||
	     ri.type == SymbolType::TlsModule)) {
		LOGF("Relocate: %s\n",
		     fmt::format("[{:016x}] <- {:016x}, {}, {}, {}, {}", ri.vaddr, value, ri.name,
		                 magic_enum::enum_name(ri.type), magic_enum::enum_name(ri.bind),
		                 ri.dbg_name).c_str());
	}
	return ri.resolved;
}

static void ForEachRelocation(Program* program, auto&& func) {
	const auto& info = *program->dynamic_info;
	for (uint32_t i = 0; i < info.rela_table_total_size / sizeof(Elf64_Rela); ++i) {
		func(info.rela_table + i, i, false);
	}
	for (uint32_t i = 0; i < info.jmprela_table_size / sizeof(Elf64_Rela); ++i) {
		func(info.jmprela_table + i, i, true);
	}
}

static KYTY_MS_ABI uint8_t* TlsMainGetAddr() {
	EXIT_IF(g_tls_main_program == nullptr);

	if (g_tls_cached_main_program == g_tls_main_program && g_tls_cached_main_tcb != nullptr) {
		return g_tls_cached_main_tcb;
	}

	g_tls_cached_main_program = g_tls_main_program;
	g_tls_cached_main_tcb =
	    RuntimeLinker::TlsGetAddr(g_tls_main_program) + g_tls_main_program->tls.tcb_offset;
	return g_tls_cached_main_tcb;
}

static void PatchProgram(Program* program, uint64_t address, uint64_t size) {
	EXIT_IF(program == nullptr);
	EXIT_IF(program->elf == nullptr);

	if (size >= 12) {
		// Replace guest stack-canary/errno stores through fs:[0x28] with nops.
		// Windows x64 cannot host guest FS directly, and an unpatched shared-library access faults
		// at address 0x28.
		const uint8_t fs_store_pattern[8] = {0x64, 0xc7, 0x04, 0x25, 0x28, 0x00, 0x00, 0x00};
		auto*         start_ptr           = reinterpret_cast<uint8_t*>(address);
		auto*         end_ptr             = start_ptr + size - 12;

		for (auto* ptr = start_ptr; ptr <= end_ptr; ptr++) {
			if (memcmp(ptr, fs_store_pattern, sizeof(fs_store_pattern)) == 0) {
				LOGF("Patch fs:[0x28] store at addr: [%016" PRIx64 "]\n",
				     reinterpret_cast<uint64_t>(ptr));
				if (ptr + 16 < start_ptr + size && ptr[12] == 0xcd && ptr[13] == 0x45 &&
				    ptr[14] == 0x90 && ptr[15] == 0x0f && ptr[16] == 0x0b) {
					ptr[0] = 0x5d; // pop rbp
					ptr[1] = 0xc3; // ret
					std::memset(ptr + 2, 0x90, 15);
				} else {
					std::memset(ptr, 0x90, 12);
				}
			}
		}
	}

	if (!program->elf->IsShared() && program->tls.handler_vaddr != 0 &&
	    size >= Jit::Call9::GetSize()) {
		// Replace:
		//   66 66 66
		//   mov <reg>, qword ptr fs:[0x00]
		// with:
		//   call <handler>
		//   mov <reg>,rax
		//   nop ...
		const uint8_t tls_pattern[5]       = {0x64, 0x48, 0x8B, 0x00, 0x25};
		const uint8_t zero_displacement[4] = {};

		EXIT_IF(Jit::Call9::GetSize() != 9);

		auto* start_ptr = reinterpret_cast<uint8_t*>(address);
		auto* end_ptr   = start_ptr + size - Jit::Call9::GetSize();

		for (auto* ptr = start_ptr; ptr <= end_ptr; ptr++) {
			auto*  inst_ptr     = ptr;
			size_t prefix_count = 0;
			while (prefix_count < 3 && inst_ptr < start_ptr + size && *inst_ptr == 0x66) {
				inst_ptr++;
				prefix_count++;
			}

			if (inst_ptr + Jit::Call9::GetSize() > start_ptr + size) {
				break;
			}

			const uint8_t modrm = inst_ptr[3];
			if (memcmp(inst_ptr, tls_pattern, 3) == 0 && (modrm & 0xc7u) == 0x04u &&
			    inst_ptr[4] == tls_pattern[4] &&
			    memcmp(inst_ptr + 5, zero_displacement, sizeof(zero_displacement)) == 0) {
				LOGF("Patch tls at addr: [%016" PRIx64 "]\n", reinterpret_cast<uint64_t>(inst_ptr));

				const auto reg = (modrm >> 3u) & 7u;
				EXIT_NOT_IMPLEMENTED(reg == 4u);

				// A raw scan can encounter a 0x66 in the preceding instruction, so do not
				// overwrite it. Call9 starts with REX.W to neutralize genuine 0x66
				// prefixes on AMD processors (before it could turn E8 into callw 16bit).
				auto* code = new (inst_ptr) Jit::Call9;
				code->SetFunc(reg == 0
				                  ? program->tls.handler_vaddr
				                  : program->tls.handler_vaddr + Jit::TlsRegStub::GetOffset(reg));
				ptr += prefix_count + Jit::Call9::GetSize() - 1;
			}
		}
	}
}

uint64_t RuntimeLinker::GetEntry() {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	for (const auto* p: m_programs) {
		if (p->elf != nullptr && !p->elf->IsShared()) {
			return p->elf->GetEntry() + p->base_vaddr;
		}
	}
	return 0;
}

uint64_t RuntimeLinker::GetProcParam() {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	for (const auto* p: m_programs) {
		if (p->elf != nullptr && !p->elf->IsShared()) {
			return p->proc_param_vaddr;
		}
	}
	return 0;
}

void RuntimeLinker::DbgDump(const std::string& folder) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	for (const auto* p: m_programs) {
		auto folder_str = Common::FixDirectorySlash(folder);
		folder_str += Common::FilenameWithoutDirectory(Common::PathToGenericString(p->file_name));

		EXIT_IF(p->elf == nullptr);

		p->elf->DbgDump(folder_str);

		if (p->dynamic_info != nullptr) {
			EXIT_NOT_IMPLEMENTED(p->dynamic_info->symbol_table_entry_size != 0 &&
			                     p->dynamic_info->symbol_table_entry_size != sizeof(Elf64_Sym));
			EXIT_NOT_IMPLEMENTED(p->dynamic_info->rela_table_entry_size != 0 &&
			                     p->dynamic_info->rela_table_entry_size != sizeof(Elf64_Rela));
			// EXIT_NOT_IMPLEMENTED(p->dynamic_info->jmprela_table == nullptr);
			// EXIT_NOT_IMPLEMENTED(p->dynamic_info->rela_table == nullptr);
			// EXIT_NOT_IMPLEMENTED(p->dynamic_info->symbol_table == nullptr);

			if (p->dynamic_info->symbol_table != nullptr) {
				DbgDumpSymbols(folder_str, p->dynamic_info->symbol_table,
				               p->dynamic_info->symbol_table_total_size,
				               p->dynamic_info->str_table);
			}
			if (p->dynamic_info->jmprela_table != nullptr) {
				DbgDumpRela(folder_str, p->dynamic_info->jmprela_table,
				            p->dynamic_info->jmprela_table_size, p->dynamic_info->str_table,
				            "jmprela_table.txt");
			}
			if (p->dynamic_info->rela_table != nullptr) {
				DbgDumpRela(folder_str, p->dynamic_info->rela_table,
				            p->dynamic_info->rela_table_total_size, p->dynamic_info->str_table,
				            "rela_table.txt");
			}
		}

		if (p->export_symbols != nullptr) {
			p->export_symbols->DbgDump(folder_str, "export_symbols.txt");
		}
		if (p->import_symbols != nullptr) {
			p->import_symbols->DbgDump(folder_str, "import_symbols.txt");
		}
	}
}

void RuntimeLinker::RelocateAll() {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	GuestExecution::GuestMutationScope mutation_scope;
	Common::LockGuard lock(m_mutex);

	for (auto* p: m_programs) {
		Relocate(p);
	}
}

void RuntimeLinker::RelocateProgram(Program* program) {
	GuestExecution::GuestMutationScope mutation_scope;
	Common::LockGuard lock(m_mutex);

	EXIT_IF(program == nullptr);
	EXIT_IF(std::find(m_programs.begin(), m_programs.end(), program) == m_programs.end());

	RelocateAll();
	if (!GamePatch::ApplyPending(program)) {
		EXIT("Failed to apply pending game cheat\n");
	}
}

void RuntimeLinker::UnloadProgram(Program* program) {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	GuestExecution::GuestMutationScope mutation_scope;
	Common::LockGuard lock(m_mutex);

	auto it = std::find(m_programs.begin(), m_programs.end(), program);
	if (it == m_programs.end()) {
		EXIT("PS5 module unload references an unknown module\n");
	}
	if (program->active_guest_calls.load(std::memory_order_acquire) != 0) {
		EXIT("PS5 cannot unload a module while its guest calls are active\n");
	}
	RejectGuestHeapTlsTeardown(program);
	m_programs.erase(it);
	m_programs_prepared = false;

	for (auto* importer: m_programs) {
		const auto& info = *importer->dynamic_info;
		const auto rela_count =
		    static_cast<uint32_t>(info.rela_table_total_size / sizeof(Elf64_Rela));
		ForEachRelocation(importer, [&](Elf64_Rela* r, uint32_t index, bool jmprela) {
			const auto bit_index = (jmprela ? rela_count : 0) + index;
			auto&      bits      = importer->rela_bits[bit_index >> 3u];
			const auto mask      = 1u << (bit_index & 7u);
			const auto type      = r->GetType();
			if ((bits & mask) == 0 || (type != R_X86_64_64 && type != R_X86_64_GLOB_DAT &&
			                           type != R_X86_64_JUMP_SLOT)) {
				return;
			}
			const auto& symbol = info.symbol_table[r->GetSymbol()];
			if (symbol.GetBind() != STB_GLOBAL && symbol.GetBind() != STB_WEAK) {
				return;
			}
			const auto value =
			    *reinterpret_cast<const uint64_t*>(importer->base_vaddr + r->r_offset);
			const auto address = value - (type == R_X86_64_64 ? r->r_addend : 0);
			if (address < program->base_vaddr ||
			    address >= program->base_vaddr + program->mapped_size) {
				return;
			}
			SymbolType symbol_type;
			switch (symbol.GetType()) {
				case STT_NOTYPE: symbol_type = SymbolType::NoType; break;
				case STT_FUNC: symbol_type = SymbolType::Func; break;
				case STT_OBJECT: symbol_type = SymbolType::Object; break;
				default: return;
			}
			const std::string name = info.str_table + symbol.st_name;
			const auto lookup_type = FunctionImportLookupType(symbol_type,
			                                                  type == R_X86_64_JUMP_SLOT);
			const auto nid = name.substr(0, name.find('#'));
			if (program->export_symbols->FindByNid(nid, lookup_type, address) != nullptr ||
			    (symbol_type == SymbolType::NoType && type == R_X86_64_JUMP_SLOT &&
			     program->export_symbols->FindByNid(nid, SymbolType::NoType, address) != nullptr)) {
				bits &= ~mask;
				if (RelocateRecord(index, r, importer, jmprela)) {
					bits |= mask;
				}
			}
		});
	}
	DeleteProgram(program);
}

RuntimeLinker::RuntimeLinker(): m_symbols(std::make_unique<SymbolDatabase>()) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
}

RuntimeLinker::~RuntimeLinker() {
	Clear();
}

Program* RuntimeLinker::LoadProgram(const std::filesystem::path& elf_name) {
	KYTY_PROFILER_FUNCTION();

#if !defined(__x86_64__) && !defined(_M_X64)
#if defined(LUMEN_PS5_FEXCORE)
	if (!GuestExecution::FullRuntimeReady()) {
		EXIT("%s\n", GuestExecution::FullRuntimeLimitation());
	}
#else
	EXIT("PS5 guest loading requires a qualified FEX embedding on ARM64\n");
#endif
#endif

	GuestExecution::GuestMutationScope mutation_scope;
	Common::LockGuard lock(m_mutex);

	static int32_t id_seq = 0;

	LOGF("Loading: %s\n", Common::PathToString(elf_name).c_str());

	auto  program_owner = std::make_unique<Program>();
	auto* program       = program_owner.get();

	program->rt        = this;
	program->file_name = elf_name;
	program->unique_id = ++id_seq;

	program->elf = std::make_unique<Elf64>();
	program->elf->Open(elf_name);

	if (program->elf->IsValid()) {
		LoadProgramToMemory(program);
		ParseProgramDynamicInfo(program);
		CreateSymbolDatabase(program);
	} else {
		EXIT("elf is not valid: %s\n", Common::PathToString(elf_name).c_str());
	}

	m_programs.push_back(program_owner.release());
	m_programs_prepared = false;

	if (!program->elf->IsShared()) {
		Libs::LibKernel::SetProgName(elf_name.filename().string());
	}

	return program;
}

void RuntimeLinker::SaveMainProgram(const std::filesystem::path& elf_name) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	for (const auto* p: m_programs) {
		EXIT_IF(p->elf == nullptr);

		if (!p->elf->IsShared()) {
			p->elf->Save(elf_name);
			break;
		}
	}
}

void RuntimeLinker::SaveProgram(Program* program, const std::filesystem::path& elf_name) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	if (auto it = std::find(m_programs.begin(), m_programs.end(), program);
	    it != m_programs.end()) {
		EXIT_IF((*it)->elf == nullptr);

		(*it)->elf->Save(elf_name);
	} else {
		EXIT("program not found");
	}
}

void RuntimeLinker::PreparePrograms() {
	GuestExecution::GuestMutationScope mutation_scope;
	Common::LockGuard lock(m_mutex);
	if (m_programs_prepared) return;
	PreloadAdjacentPrograms();
	RelocateAll();
	m_programs_prepared = true;
	LOGF("Loader preparation complete: %zu modules; guest entry has not run\n",
	     m_programs.size());
}

void RuntimeLinker::SetUpstreamFunctionFallback(bool enabled) {
	Common::LockGuard lock(m_mutex);
	if (!m_programs.empty()) EXIT("PS5 import mode must be selected before loading modules\n");
	m_upstream_function_fallback = enabled;
	if (enabled && !InitializeFunctionFallbackHostCall())
		EXIT("PS5 import fallback HLE initialization failed\n");
	std::fprintf(stderr, "PS5_IMPORT_MODE mode=%s\n",
	             enabled ? "upstream_function_fallback" : "strict");
}

void RuntimeLinker::Execute(const std::filesystem::path& game_patch) {
	KYTY_PROFILER_THREAD("Thread_Main");

	Libs::LibKernel::PthreadInitSelfForMainThread();
	auto* main_stack_top = Libs::LibKernel::PthreadCreateMainGuestStack();

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Guest code has no Windows stack probes and may jump over the guard page. Module
	// initializers execute on the host stack too, so grow it before calling any guest code.
	size_t expanded_size = 0;
	while (expanded_size < static_cast<size_t>(768) * 1024) {
		sys_dbg_stack_info_t stack {};
		SysStackUsage(stack);
		*reinterpret_cast<uint32_t*>(stack.guard_addr) = 0;
		expanded_size += stack.guard_size;
	}
#endif

	PreparePrograms();

	if (!game_patch.empty()) {
		if (!GamePatch::Apply(game_patch, m_programs.empty() ? nullptr : m_programs.front(),
		                      m_programs)) {
			EXIT("Failed to apply game cheat\n");
		}
	}
	StartAllModules();

	LOGF_COLOR(Log::Color::BrightYellow, "---\n--- Execute: %s\n---\n", "Main");

	if (auto entry = GetEntry(); entry != 0) {
		auto* params = reinterpret_cast<EntryParams*>(
		    (reinterpret_cast<uintptr_t>(main_stack_top) - 0x100u) & ~static_cast<uintptr_t>(0x0f));
		std::memset(params, 0, sizeof(EntryParams));
		params->argc    = 1;
#if defined(LUMEN_PS5_FEXCORE)
		// argv is guest data too: keep its bytes in the owned guest stack.
		auto* name = reinterpret_cast<char*>(params + 1);
		std::memcpy(name, "Lumen", 6);
		params->argv[0] = name;
		if (!RegisterHostFunction<ProgramExitHandler>("ProgramExitHandler")) {
			EXIT("PS5 exit callback registration failed\n");
		}
#else
		params->argv[0] = "KytyEmu";
#endif

		LOGF("stack_addr = %" PRIx64 "\n", reinterpret_cast<uint64_t>(params));

		RunEntry(entry, params, ProgramExitHandler,
		         reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(params) - 0x1000u));
	}
}

void RuntimeLinker::Clear() {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	GuestExecution::GuestMutationScope mutation_scope;
	Common::LockGuard lock(m_mutex);
	for (auto* program: m_programs) {
		if (program->active_guest_calls.load(std::memory_order_acquire) != 0) {
			EXIT("PS5 cannot clear modules while their guest calls are active\n");
		}
	}
	for (auto* program: m_programs) RejectGuestHeapTlsTeardown(program);
	GamePatch::Clear();

	for (auto* p: m_programs) {
		DeleteProgram(p);
	}
	m_programs.clear();
	m_programs_prepared = false;
	g_tls_main_program        = nullptr;
	g_tls_cached_main_program = nullptr;
	g_tls_cached_main_tcb     = nullptr;
	g_desired_base_addr       = SYSTEM_RESERVED + CODE_BASE_OFFSET;
	m_symbols.reset();
}

void RuntimeLinker::Resolve(const std::string& name, SymbolType type, Program* program,
                            SymbolRecord* out_info, bool* bind_self) {
	KYTY_PROFILER_FUNCTION();

	Common::LockGuard lock(m_mutex);

	EXIT_IF(out_info == nullptr);

	auto ids = Common::Split(name, '#');

	if (bind_self != nullptr) {
		*bind_self = false;
	}

	if (ids.size() == 3) {
		const LibraryId* l = FindLibrary(*program, ids.at(1));
		const ModuleId*  m = FindModule(*program, ids.at(2));

		auto resolve_by_nid = [this, type](const std::string& nid, SymbolRecord* out) -> bool {
			EXIT_IF(out == nullptr);

			if (m_symbols != nullptr) {
				if (const auto* rec = m_symbols->FindByNid(nid, type); rec != nullptr) {
					*out = *rec;
					return true;
				}
			}

			for (auto* p: m_programs) {
				if (p != nullptr && p->export_symbols != nullptr) {
					if (const auto* rec = p->export_symbols->FindByNid(nid, type); rec != nullptr) {
						*out = *rec;
						return true;
					}
				}
			}

			return false;
		};

		if (l != nullptr && m != nullptr) {
			SymbolResolve sr {};
			sr.name                 = ids.at(0);
			sr.library              = l->name;
			sr.library_version      = l->version;
			sr.module               = m->name;
			sr.module_version_major = m->version_major;
			sr.module_version_minor = m->version_minor;
			sr.type                 = type;

			const SymbolRecord* rec = nullptr;

			if (m_symbols != nullptr) {
				rec = m_symbols->Find(sr);
			}

			if (rec == nullptr) {
				for (auto* p: m_programs) {
					const auto& libs = p->dynamic_info->export_libs;
					const auto& modules = p->dynamic_info->export_modules;
					if (p->export_symbols == nullptr ||
					    std::find(libs.begin(), libs.end(), *l) == libs.end() ||
					    std::find(modules.begin(), modules.end(), *m) == modules.end()) {
						continue;
					}
					rec = p->export_symbols->Find(sr);
					if (rec != nullptr) {
						if (bind_self != nullptr) {
							*bind_self = (p == program);
						}
						break;
					}
				}
			}

			if (rec == nullptr) {
				if (resolve_by_nid(sr.name, out_info)) {
					LOGF("PS5 NID fallback: %s -> %s\n", sr.name.c_str(), out_info->name.c_str());
					return;
				}
			}

			if (rec != nullptr) {
				*out_info = *rec;
			} else {
				out_info->vaddr    = 0;
				out_info->name     = SymbolDatabase::GenerateName(sr);
				out_info->dbg_name = "";
			}
		} else {
			if (resolve_by_nid(ids.at(0), out_info)) {
				LOGF("PS5 NID fallback: %s -> %s (missing lib/module metadata)\n",
				     ids.at(0).c_str(), out_info->name.c_str());
				return;
			}

			EXIT("l == nullptr || m == nullptr");
		}
	} else {
		out_info->vaddr    = 0;
		out_info->name     = name;
		out_info->dbg_name = "";
	}
}

uint64_t RuntimeLinker::ReadFromElf(Program* program, uint64_t vaddr) {
	EXIT_IF(program == nullptr);
	EXIT_IF(program->base_vaddr == 0 || program->base_size == 0);
	EXIT_IF(program->elf == nullptr);

	uint64_t ret = 0;

	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();

	EXIT_IF(phdr == nullptr || ehdr == nullptr);

	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_memsz != 0 && (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO)) {
			uint64_t segment_addr      = phdr[i].p_vaddr + program->base_vaddr;
			uint64_t segment_file_size = phdr[i].p_filesz;

			if (vaddr >= segment_addr && vaddr < segment_addr + segment_file_size) {
				program->elf->LoadSegment(reinterpret_cast<uint64_t>(&ret),
				                          phdr[i].p_offset + vaddr - segment_addr, sizeof(ret));
				break;
			}
		}
	}

	return ret;
}

Program* RuntimeLinker::FindProgramById(int32_t id) {
	Common::LockGuard lock(m_mutex);

	// Id 0 is reserved for main program
	if (id == 0 && !m_programs.empty()) {
		return m_programs.front();
	}

	for (auto* p: m_programs) {
		if (p->unique_id == id) {
			return p;
		}
	}

	return nullptr;
}

Program* RuntimeLinker::FindProgramByFileName(const std::filesystem::path& elf_name) {
	Common::LockGuard lock(m_mutex);

	auto fixed_name = Common::FixFilenameSlash(Common::PathToGenericString(elf_name));
	for (auto* p: m_programs) {
		if (Common::EqualNoCase(Common::FixFilenameSlash(Common::PathToGenericString(p->file_name)),
		                        fixed_name)) {
			return p;
		}
	}

	return nullptr;
}

Program* RuntimeLinker::FindProgramByAddr(uint64_t vaddr) {
	Common::LockGuard lock(m_mutex);

	for (auto* p: m_programs) {
		const auto* ehdr = p->elf->GetEhdr();
		const auto* phdr = p->elf->GetPhdr();

		EXIT_IF(phdr == nullptr || ehdr == nullptr);

		for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
			if (phdr[i].p_memsz != 0 &&
			    (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO)) {
				uint64_t segment_addr = phdr[i].p_vaddr + p->base_vaddr;
				uint64_t segment_size = GetAlignedSize(phdr + i);

				if (vaddr >= segment_addr && vaddr < segment_addr + segment_size) {
					return p;
				}
			}
		}
	}

	return nullptr;
}

void RuntimeLinker::StackTrace(uint64_t frame_ptr, uint64_t stack_ptr) {
	void* stack[20];
	int   depth = WalkGuestStack(frame_ptr, stack_ptr, stack, static_cast<int>(std::size(stack)));

	LOGF("Stack trace [thread = %d]:\n", Common::Thread::GetThreadIdUnique());

	for (int i = 0; i < depth; i++) {
		auto  vaddr = reinterpret_cast<uint64_t>(stack[i]);
		auto* p     = FindProgramByAddr(vaddr);
		LOGF("[%d] %016" PRIx64 ", off=%016" PRIx64 ", %s\n", i, vaddr,
		     (p == nullptr ? 0 : vaddr - p->base_vaddr),
		     (p == nullptr
		          ? "???"
		          : Common::FilenameWithoutDirectory(Common::PathToGenericString(p->file_name))
		                .c_str()));
	}
}

static std::string GetProgramModuleName(const Program* program) {
	EXIT_IF(program == nullptr);

	if (program->dynamic_info != nullptr && program->dynamic_info->so_name != nullptr &&
	    program->dynamic_info->so_name[0] != '\0') {
		return std::string(program->dynamic_info->so_name);
	}

	return Common::FilenameWithoutDirectory(Common::PathToGenericString(program->file_name));
}

static bool ModuleStartDependenciesSatisfied(const Program*               program,
                                             const std::vector<Program*>& programs,
                                             const std::vector<Program*>& started) {
	EXIT_IF(program == nullptr);
	EXIT_IF(program->dynamic_info == nullptr);

	for (const auto* needed: program->dynamic_info->needed) {
		if (needed == nullptr || needed[0] == '\0') {
			continue;
		}

		const auto needed_name = std::string(needed);

		for (auto* dependency: programs) {
			if (dependency == nullptr || dependency == program || dependency->elf == nullptr ||
			    !dependency->elf->IsShared()) {
				continue;
			}

			const auto dependency_name = GetProgramModuleName(dependency);
			if (Common::EqualNoCase(dependency_name, needed_name) ||
			    Common::EqualNoCase(Common::FilenameWithoutDirectory(
			                            Common::PathToGenericString(dependency->file_name)),
			                        needed_name)) {
				if (std::find(started.begin(), started.end(), dependency) == started.end()) {
					return false;
				}
				break;
			}
		}
	}

	return true;
}

void RuntimeLinker::StartAllModules() {
	ProgramCallPins pins;
	{
		Common::LockGuard lock(m_mutex);
		for (auto* program: m_programs) pins.Add(program);
	}
	const auto& programs = pins.Programs();

	std::vector<Program*> started;

	for (;;) {
		bool progressed = false;

		for (auto* p: programs) {
			if (p->elf->IsShared() && p->dynamic_info->init_vaddr != 0 &&
			    std::find(started.begin(), started.end(), p) == started.end() &&
			    ModuleStartDependenciesSatisfied(p, programs, started)) {
				StartModule(p, 0, nullptr, nullptr);
				started.push_back(p);
				progressed = true;
			}
		}

		if (!progressed) {
			break;
		}
	}

	for (auto* p: programs) {
		if (p->elf->IsShared() && p->dynamic_info->init_vaddr != 0 &&
		    std::find(started.begin(), started.end(), p) == started.end()) {
			StartModule(p, 0, nullptr, nullptr);
			started.push_back(p);
		}
	}
}

void RuntimeLinker::StopAllModules() {
	ProgramCallPins pins;
	{
		Common::LockGuard lock(m_mutex);
		for (auto* program: m_programs) pins.Add(program);
	}

	for (auto* p: pins.Programs()) {
		if (p->elf->IsShared() && p->dynamic_info->fini_vaddr != 0) {
			StopModule(p, 0, nullptr, nullptr);
		}
	}
}

static bool IsAdjacentModuleFile(const std::string& name) {
	auto lower = Common::ToLower(name);
	return lower.ends_with(".prx") || lower.ends_with(".sprx");
}

static bool SkipAdjacentModuleFile(const std::string& name) {
	auto lower = Common::ToLower(name);
	return lower == "eboot.bin" || lower == "libkernel.prx" || lower == "libkernel_sys.prx";
}

void RuntimeLinker::PreloadAdjacentPrograms() {
	if (m_programs.empty()) {
		return;
	}

	std::vector<std::filesystem::path> module_paths;

	auto is_loaded = [this](const std::filesystem::path& path) {
		auto fixed_path = Common::FixFilenameSlash(Common::PathToGenericString(path));
		for (auto* program: m_programs) {
			if (Common::EqualNoCase(
			        Common::FixFilenameSlash(Common::PathToGenericString(program->file_name)),
			        fixed_path)) {
				return true;
			}
		}
		return false;
	};

	auto add_path = [&module_paths, &is_loaded](const std::filesystem::path& path) {
		if (is_loaded(path)) {
			return;
		}
		for (const auto& p: module_paths) {
			if (Common::EqualNoCase(Common::PathToGenericString(p),
			                        Common::PathToGenericString(path))) {
				return;
			}
		}
		module_paths.push_back(path);
	};

	auto add_dir = [&add_path](const std::filesystem::path& dir) {
		if (!Common::File::IsDirectoryExisting(dir)) {
			return;
		}
		for (const auto& entry: Common::File::GetDirEntries(dir)) {
			if (entry.is_file && IsAdjacentModuleFile(entry.name) &&
			    !SkipAdjacentModuleFile(entry.name)) {
				add_path(dir / entry.name);
			}
		}
	};

	auto root = m_programs.at(0)->file_name.parent_path();
	if (root.empty()) {
		return;
	}

	add_dir(root);
	add_dir(root / "sce_module");
	add_dir(root / "sce_modules");

	for (const auto& path: module_paths) {
		LoadProgram(path);
	}
}

int RuntimeLinker::StartModule(Program* program, size_t args, const void* argp,
                               module_func_t func) {
#if !defined(LUMEN_PS5_FEXCORE) && !defined(__x86_64__) && !defined(_M_X64)
	EXIT("PS5 guest module calls require the complete FEX embedding\n");
	__builtin_unreachable();
#else
	ProgramCallPins pins;
	{
		Common::LockGuard lock(m_mutex);
		if (program == nullptr ||
		    std::find(m_programs.begin(), m_programs.end(), program) == m_programs.end()) {
			EXIT("PS5 module initializer references an unknown module\n");
		}
		pins.Add(program);
	}
	EXIT_IF(program->dynamic_info == nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(!program->elf->IsShared());

	LOGF_COLOR(Log::Color::BrightYellow, "---\n--- Start module: %s\n---\n",
	           Common::PathToString(program->file_name).c_str());

#if defined(LUMEN_PS5_FEXCORE)
	std::fprintf(stderr, "PS5_GUEST_MODULE_INIT_ATTEMPT owner=%d module=%s\n",
	             program->unique_id, Common::PathToString(program->file_name).c_str());
	const uint64_t arguments[] {args, reinterpret_cast<uint64_t>(argp),
	                            reinterpret_cast<uint64_t>(func)};
	uint64_t result {};
	if (!GuestExecution::Invoke(program->dynamic_info->init_vaddr + program->base_vaddr,
	                            arguments, 3, &result)) {
		EXIT("PS5 guest module initializer failed\n");
	}
	std::fprintf(stderr, "PS5_GUEST_MODULE_INIT_RETURNED owner=%d value=%#llx\n",
	             program->unique_id, static_cast<unsigned long long>(result));
	return static_cast<int>(result);
#else
	return reinterpret_cast<module_ini_fini_func_t>(program->dynamic_info->init_vaddr +
	                                                program->base_vaddr)(args, argp, func);
#endif
#endif
}

int RuntimeLinker::StopModule(Program* program, size_t args, const void* argp, module_func_t func) {
#if !defined(LUMEN_PS5_FEXCORE) && !defined(__x86_64__) && !defined(_M_X64)
	EXIT("PS5 guest module calls require the complete FEX embedding\n");
	__builtin_unreachable();
#else
	ProgramCallPins pins;
	{
		Common::LockGuard lock(m_mutex);
		if (program == nullptr ||
		    std::find(m_programs.begin(), m_programs.end(), program) == m_programs.end()) {
			EXIT("PS5 module finalizer references an unknown module\n");
		}
		pins.Add(program);
	}
	EXIT_IF(program->dynamic_info == nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(!program->elf->IsShared());

	LOGF_COLOR(Log::Color::BrightYellow, "---\n--- Stop module: %s\n---\n",
	           Common::PathToString(program->file_name).c_str());

#if defined(LUMEN_PS5_FEXCORE)
	const uint64_t arguments[] {args, reinterpret_cast<uint64_t>(argp),
	                            reinterpret_cast<uint64_t>(func)};
	uint64_t value {};
	if (!GuestExecution::Invoke(program->dynamic_info->fini_vaddr + program->base_vaddr,
	                            arguments, 3, &value)) {
		EXIT("PS5 guest module finalizer failed\n");
	}
	int result = static_cast<int>(value);
#else
	int result = reinterpret_cast<module_ini_fini_func_t>(program->dynamic_info->fini_vaddr +
	                                                      program->base_vaddr)(args, argp, func);
#endif

	Libs::LibKernel::PthreadDeleteStaticObjects(program);

	return result;
#endif
}

uint8_t* RuntimeLinker::TlsGetAddr(Program* program, bool native_root_entry) {
	EXIT_IF(program == nullptr);

#if defined(LUMEN_PS5_FEXCORE)
	// The common cached lookup does not mutate mappings or enter an epoch.
	// A missing block is rechecked after exclusive admission and TLS locking.
	{
		Common::LockGuard lock(program->tls.mutex);
		const auto existing = program->tls.tlss.find(Common::Thread::GetThreadIdUnique());
		if (existing != program->tls.tlss.end() && existing->second.ptr != nullptr)
			return existing->second.ptr;
	}
#endif
	GuestExecution::GuestMutationScope mutation_scope(
	    native_root_entry ? GuestExecution::GuestMutationScope::Admission::NativeRootEntry
	                      : GuestExecution::GuestMutationScope::Admission::Strict);
	Common::LockGuard lock(program->tls.mutex);

	auto& tls = program->tls.tlss[Common::Thread::GetThreadIdUnique()];

	if (tls.ptr == nullptr) {
		constexpr uint64_t TCB_SIZE  = 0x40;
		constexpr uint64_t TCB_ALIGN = 0x20;

		const auto tcb_offset =
		    program->tls.tcb_offset != 0 ? program->tls.tcb_offset : program->tls.image_size;
		if (tcb_offset < program->tls.image_size ||
		    tcb_offset > UINT64_MAX - (TCB_ALIGN - 1) - TCB_SIZE ||
		    program->tls.init_size > program->tls.image_size ||
		    program->tls.init_image.size() > program->tls.image_size) {
			EXIT("invalid PS5 static TLS image or TCB layout\n");
		}
		const auto alloc_size = AlignUp(tcb_offset, TCB_ALIGN) + TCB_SIZE;
		tls.ptr        = reinterpret_cast<uint8_t*>(Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    0, alloc_size, Common::VirtualMemory::Mode::ReadWrite, "thread_local_storage"));
		tls.free_func  = nullptr;
		tls.vm_alloc   = true;
		tls.alloc_size = alloc_size;

		if (tls.ptr == nullptr) {
			EXIT("PS5 static TLS allocation failed\n");
		}

		std::memset(tls.ptr, 0, alloc_size);

		if (!program->tls.init_image.empty()) {
			std::memcpy(tls.ptr, program->tls.init_image.data(), program->tls.init_image.size());
		} else {
			std::memcpy(tls.ptr, reinterpret_cast<void*>(program->tls.image_vaddr),
			            program->tls.init_size);
		}

		const auto self = reinterpret_cast<uint64_t>(tls.ptr + tcb_offset);
		// x86 allows an unaligned FS base. Preserve the imported image/TCB
		// offset without an unaligned typed store on the native ARM64 host.
		std::memcpy(tls.ptr + tcb_offset, &self, sizeof(self));
	}

	return tls.ptr;
}

#if defined(LUMEN_PS5_FEXCORE)
void RuntimeLinker::PrepareCurrentThreadTls() {
	// This lexical native root owns no execution/HLE lease. Admit before the
	// loader lock; nested allocation epochs inherit this writer, never a reader.
	GuestExecution::GuestMutationScope mutation_scope(
	    GuestExecution::GuestMutationScope::Admission::NativeRootEntry);
	Common::LockGuard lock(m_mutex);
	for (auto* program: m_programs) {
		if (program->tls.image_size != 0) TlsGetAddr(program, true);
	}
}
#endif

uint64_t RuntimeLinker::CurrentThreadTlsBase(bool native_root_entry) {
#if defined(LUMEN_PS5_FEXCORE)
	if (native_root_entry) Libs::Posix::PrepareCurrentThreadError();
#endif
	if (g_tls_main_program == nullptr) {
		return 0;
	}
#if defined(LUMEN_PS5_FEXCORE)
	if (native_root_entry) {
		EXIT_IF(g_tls_main_program->rt == nullptr);
		g_tls_main_program->rt->PrepareCurrentThreadTls();
	}
#endif
	const auto offset = g_tls_main_program->tls.tcb_offset != 0
	                        ? g_tls_main_program->tls.tcb_offset
	                        : g_tls_main_program->tls.image_size;
	return reinterpret_cast<uint64_t>(TlsGetAddr(g_tls_main_program, native_root_entry)) + offset;
}

bool RuntimeLinker::CurrentThreadTlsExtent(uint64_t* base, uint64_t* size) {
	if (base == nullptr || size == nullptr) return false;
	*base = 0;
	*size = 0;
	if (g_tls_main_program == nullptr) return true;
	Common::LockGuard lock(g_tls_main_program->tls.mutex);
	const auto existing = g_tls_main_program->tls.tlss.find(Common::Thread::GetThreadIdUnique());
	if (existing == g_tls_main_program->tls.tlss.end() || existing->second.ptr == nullptr ||
	    !existing->second.vm_alloc || existing->second.alloc_size == 0) return false;
	*base = reinterpret_cast<uint64_t>(existing->second.ptr);
	*size = existing->second.alloc_size;
	return *size <= UINT64_MAX - *base;
}

uint8_t* RuntimeLinker::GetThreadTlsAddress(Program* program, uint64_t offset) {
#if defined(LUMEN_PS5_FEXCORE) && !defined(LUMEN_PS5_DIRECT_FEX)
	if (LumenAndroidFexCoreRetainGuestDataLeaseV1() != 0) {
		struct ReaderLease {
			~ReaderLease() {
				if (LumenAndroidFexCoreReleaseGuestDataLeaseV1() == 0)
					EXIT("PS5 dynamic TLS reader release failed\n");
			}
		} lease;
		Common::LockGuard lock(m_mutex);
		if (std::find(m_programs.begin(), m_programs.end(), program) == m_programs.end())
			EXIT("PS5 dynamic TLS descriptor references an unknown module\n");
		if (offset >= program->tls.image_size)
			EXIT("PS5 dynamic TLS descriptor offset exceeds its module image\n");
		Common::LockGuard tls_lock(program->tls.mutex);
		const auto existing = program->tls.tlss.find(Common::Thread::GetThreadIdUnique());
		if (existing != program->tls.tlss.end() && existing->second.ptr != nullptr)
			return existing->second.ptr + offset;
	}
	// A miss changes VM ownership. Release both local locks and the temporary
	// borrow before the unchanged strict epoch; retained outer HLE still denies it.
#endif
	GuestExecution::GuestMutationScope mutation_scope;
	Common::LockGuard lock(m_mutex);
	if (std::find(m_programs.begin(), m_programs.end(), program) == m_programs.end()) {
		EXIT("PS5 dynamic TLS descriptor references an unknown module\n");
	}
	if (offset >= program->tls.image_size) {
		EXIT("PS5 dynamic TLS descriptor offset exceeds its module image\n");
	}
	return TlsGetAddr(program) + offset;
}

#if defined(LUMEN_PS5_FEXCORE) && !defined(LUMEN_PS5_DIRECT_FEX)
namespace {
bool nested_tls_mutation_denied {};
uint64_t nested_tls_address {};

uint64_t NestedTlsLeaf(Program* program, uint64_t offset) {
	const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
	nested_tls_mutation_denied = token == 0;
	if (token != 0) LumenAndroidFexCoreEndGuestMemoryMutationV1(token, 0, 0, 0);
	auto* rt = Common::Singleton<RuntimeLinker>::Instance();
	nested_tls_address = reinterpret_cast<uint64_t>(rt->GetThreadTlsAddress(program, offset));
	return nested_tls_address;
}

uint64_t NestedTlsInner(uint64_t leaf, Program* program, uint64_t offset) {
	const uint64_t arguments[] {reinterpret_cast<uint64_t>(program), offset};
	uint64_t result {};
	return GuestExecution::Invoke(leaf, arguments, 2, &result) ? result : UINT64_MAX;
}

uint64_t NestedTlsOuter(uint64_t inner, uint64_t leaf, Program* program, uint64_t offset) {
	const uint64_t arguments[] {leaf, reinterpret_cast<uint64_t>(program), offset};
	uint64_t result {};
	return GuestExecution::Invoke(inner, arguments, 3, &result) ? result : UINT64_MAX;
}
} // namespace

bool TestNestedThreadTlsLookup(uint64_t stack_top) {
	auto* rt = Common::Singleton<RuntimeLinker>::Instance();
	{
		Common::LockGuard lock(rt->m_mutex);
		if (!rt->m_programs.empty() || g_tls_main_program != nullptr) return false;
	}
	if (!RegisterHostFunction<NestedTlsOuter>("self-test-nested-tls-outer") ||
	    !RegisterHostFunction<NestedTlsInner>("self-test-nested-tls-inner") ||
	    !RegisterHostFunction<NestedTlsLeaf>("self-test-nested-tls-leaf")) return false;
	const auto code_base = Libs::LibKernel::Memory::AllocateProgramMemory(0, 0x4000,
	    Common::VirtualMemory::Mode::ReadWrite, "nested-tls-test-code");
	if (code_base == 0) return false;
	std::array<uint8_t, 192> code {};
	const auto relay = [&](size_t offset, uint64_t target, bool read_tls) {
		constexpr uint8_t prefix[] {0x48, 0x83, 0xec, 0x08, 0x48, 0xb8};
		constexpr uint8_t tail[] {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x08};
		std::memcpy(code.data() + offset, prefix, sizeof(prefix));
		std::memcpy(code.data() + offset + sizeof(prefix), &target, sizeof(target));
		std::memcpy(code.data() + offset + 14, tail, sizeof(tail));
		if (read_tls) {
			// Use the pointer in translated code after the HLE lookup returns.
			constexpr uint8_t load_and_return[] {0x0f, 0xb6, 0x00, 0xc3};
			std::memcpy(code.data() + offset + 20, load_and_return, sizeof(load_and_return));
		} else {
			code[offset + 20] = 0xc3;
		}
	};
	relay(0, reinterpret_cast<uint64_t>(NestedTlsOuter), false);
	relay(64, reinterpret_cast<uint64_t>(NestedTlsInner), false);
	relay(128, reinterpret_cast<uint64_t>(NestedTlsLeaf), true);
	bool pass = Libs::LibKernel::Memory::WriteGuestMemory(nullptr, code_base, code.data(), code.size());
	Libs::LibKernel::Memory::SetProgramMemoryProtection(code_base, 0x4000,
	    Common::VirtualMemory::Mode::ExecuteRead);

	Program main_program, module, late_module;
	const auto initialize = [rt](Program& program, std::initializer_list<uint8_t> image) {
		program.rt = rt;
		program.tls.image_size = 0x20;
		program.tls.tcb_offset = 0x20;
		program.tls.init_image = image;
		program.tls.init_size = program.tls.init_image.size();
	};
	initialize(main_program, {0x11, 0x22});
	initialize(module, {0x55, 0x66});
	initialize(late_module, {0x88, 0x99});
	{
		GuestExecution::GuestMutationScope mutation(
		    GuestExecution::GuestMutationScope::Admission::NativeRootEntry);
		Common::LockGuard lock(rt->m_mutex);
		rt->m_programs = {&main_program, &module};
		g_tls_main_program = &main_program;
	}
	const auto cached_address = [](Program& program, int thread_id) {
		Common::LockGuard lock(program.tls.mutex);
		const auto block = program.tls.tlss.find(thread_id);
		return block != program.tls.tlss.end() ? reinterpret_cast<uint64_t>(block->second.ptr) : 0;
	};
	const int main_id = Common::Thread::GetThreadIdUnique();
	nested_tls_mutation_denied = false;
	nested_tls_address = 0;
	const uint64_t arguments[] {code_base + 64, code_base + 128,
	                           reinterpret_cast<uint64_t>(&module), 1};
	uint64_t result {};
	const bool invoked = GuestExecution::Invoke(code_base, arguments, 4, &result, stack_top);
	const auto main_tls = cached_address(main_program, main_id);
	const auto module_tls = cached_address(module, main_id);
	pass = invoked && result == 0x66 && nested_tls_mutation_denied &&
	    main_tls != 0 && module_tls != 0 && main_tls != module_tls &&
	    nested_tls_address == module_tls + 1 && pass;
	const auto first_tcb = RuntimeLinker::CurrentThreadTlsBase(true);
	pass = first_tcb == main_tls + 0x20 &&
	    RuntimeLinker::CurrentThreadTlsBase(true) == first_tcb &&
	    cached_address(module, main_id) == module_tls && pass;
	{
		GuestExecution::GuestMutationScope mutation(
		    GuestExecution::GuestMutationScope::Admission::NativeRootEntry);
		Common::LockGuard lock(rt->m_mutex);
		rt->m_programs.push_back(&late_module);
	}
	pass = cached_address(late_module, main_id) == 0 && pass;
	RuntimeLinker::CurrentThreadTlsBase(true);
	const auto late_tls = cached_address(late_module, main_id);
	pass = late_tls != 0 && late_tls != main_tls && late_tls != module_tls && pass;
	uint8_t late_image {};
	pass = Libs::LibKernel::Memory::ReadGuestMemory(nullptr, late_tls + 1, &late_image, 1) &&
	    late_image == 0x99 && pass;
	bool child_valid = false;
	std::thread child([&] {
		const auto child_tcb = RuntimeLinker::CurrentThreadTlsBase(true);
		const int child_id = Common::Thread::GetThreadIdUnique();
		const auto child_main = cached_address(main_program, child_id);
		const auto child_module = cached_address(module, child_id);
		const auto child_late = cached_address(late_module, child_id);
		uint8_t image {};
		const uint8_t changed = 0x7a;
		child_valid = child_main != 0 && child_module != 0 && child_late != 0 &&
		    child_main != main_tls && child_module != module_tls && child_late != late_tls &&
		    child_main != child_module && child_module != child_late && child_main != child_late &&
		    child_tcb == child_main + 0x20 &&
		    Libs::LibKernel::Memory::ReadGuestMemory(nullptr, child_module, &image, 1) && image == 0x55 &&
		    Libs::LibKernel::Memory::WriteGuestMemory(nullptr, child_module, &changed, 1);
		rt->DeleteTlss(child_id);
	});
	child.join();
	uint8_t main_image {};
	pass = child_valid &&
	    Libs::LibKernel::Memory::ReadGuestMemory(nullptr, module_tls, &main_image, 1) &&
	    main_image == 0x55 && cached_address(main_program, main_id) == main_tls &&
	    cached_address(module, main_id) == module_tls && pass;
	rt->DeleteTlss(main_id);
	{
		GuestExecution::GuestMutationScope mutation(
		    GuestExecution::GuestMutationScope::Admission::NativeRootEntry);
		Common::LockGuard lock(rt->m_mutex);
		rt->m_programs.clear();
		g_tls_main_program = nullptr;
		g_tls_cached_main_program = nullptr;
		g_tls_cached_main_tcb = nullptr;
	}
	pass = Libs::LibKernel::Memory::FreeGuestMemory(code_base, 0x4000) && pass;
	std::puts(pass ? "PS5 prepared nested module TLS: PASS" : "PS5 prepared nested module TLS: FAIL");
	return pass;
}

bool TestThreadTlsIsolation() {
	Program program;
	program.tls.init_image = {0x11, 0x22, 0x33, 0x44};
	program.tls.init_size = program.tls.init_image.size();
	program.tls.image_size = 0x20;
	program.tls.tcb_offset = 0x20;
	const auto main_tls = reinterpret_cast<uint64_t>(RuntimeLinker::TlsGetAddr(&program));
	uint64_t child_tls = 0;
	bool child_valid = false;
	std::thread child([&] {
		child_tls = reinterpret_cast<uint64_t>(RuntimeLinker::TlsGetAddr(&program));
		std::array<uint8_t, 4> image {};
		uint64_t self = 0;
		const uint8_t changed = 0x7a;
		child_valid = Libs::LibKernel::Memory::ReadGuestMemory(nullptr, child_tls,
		    image.data(), image.size()) &&
		    image == std::array<uint8_t, 4> {0x11, 0x22, 0x33, 0x44} &&
		    Libs::LibKernel::Memory::ReadGuestMemory(nullptr, child_tls + 0x20, &self, 8) &&
		    self == child_tls + 0x20 &&
		    Libs::LibKernel::Memory::WriteGuestMemory(nullptr, child_tls, &changed, 1);
		RuntimeLinker::DeleteTls(&program, Common::Thread::GetThreadIdUnique());
	});
	child.join();
	std::array<uint8_t, 4> image {};
	uint64_t self = 0;
	const bool pass = child_valid && main_tls != child_tls &&
	    Libs::LibKernel::Memory::ReadGuestMemory(nullptr, main_tls, image.data(), image.size()) &&
	    image == std::array<uint8_t, 4> {0x11, 0x22, 0x33, 0x44} &&
	    Libs::LibKernel::Memory::ReadGuestMemory(nullptr, main_tls + 0x20, &self, 8) &&
	    self == main_tls + 0x20 &&
	    reinterpret_cast<uint64_t>(RuntimeLinker::TlsGetAddr(&program)) == main_tls;
	RuntimeLinker::DeleteTls(&program, Common::Thread::GetThreadIdUnique());
	std::puts(pass ? "PS5 module TLS thread isolation: PASS" : "PS5 module TLS thread isolation: FAIL");
	return pass;
}
#endif

void RuntimeLinker::DeleteTls(Program* program, int thread_id) {
	EXIT_IF(program == nullptr);

	if (thread_id == Common::Thread::GetThreadIdUnique() && g_tls_cached_main_program == program) {
		g_tls_cached_main_program = nullptr;
		g_tls_cached_main_tcb     = nullptr;
	}

#if defined(LUMEN_PS5_FEXCORE)
	ThreadLocalStorage::Block retired {};
	{
		GuestExecution::GuestMutationScope mutation_scope;
		Common::LockGuard lock(program->tls.mutex);
		if (auto it = program->tls.tlss.find(thread_id); it != program->tls.tlss.end()) {
			retired = it->second;
			program->tls.tlss.erase(it);
		}
	}
	// Callback-backed blocks must invoke the guest after both locks and the
	// translator mutation epoch have closed. Ordinary VM frees take their own epoch.
	FreeTlsBlock(&retired);
#else
	Common::LockGuard lock(program->tls.mutex);
	if (auto it = program->tls.tlss.find(thread_id); it != program->tls.tlss.end()) {
		FreeTlsBlock(&it->second);
		program->tls.tlss.erase(it);
	}
#endif
}

static uint64_t CalcBaseSize(const Elf64_Ehdr* ehdr, const Elf64_Phdr* phdr) {
	uint64_t base_size = 0;
	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_memsz != 0 && (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO)) {
			uint64_t last_addr = phdr[i].p_vaddr + GetAlignedSize(phdr + i);
			if (last_addr > base_size) {
				base_size = last_addr;
			}
		}
	}
	return base_size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RuntimeLinker::LoadProgramToMemory(Program* program) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr || program->base_vaddr != 0 || program->base_size != 0 ||
	        program->elf == nullptr);

	// static uint64_t desired_base_addr = DESIRED_BASE_ADDR;

	bool is_shared   = program->elf->IsShared();
	bool is_next_gen = program->elf->IsNextGen();

	EXIT_NOT_IMPLEMENTED(!is_shared && !is_next_gen);

	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();

	EXIT_IF(phdr == nullptr || ehdr == nullptr);

	program->base_size                 = CalcBaseSize(ehdr, phdr);
	constexpr uint64_t GUEST_PAGE_SIZE = 0x4000;
	EXIT_IF(program->base_size > UINT64_MAX - (GUEST_PAGE_SIZE - 1));
	program->base_size_aligned = AlignUp(program->base_size, GUEST_PAGE_SIZE);

#if defined(LUMEN_PS5_FEXCORE)
	// FEX supplies architectural FS/GS bases; no guest instruction rewriting.
	uint64_t tls_handler_size = 0;
#else
	uint64_t tls_handler_size = is_shared ? 0 : Jit::SafeCall::GetSize();
#endif
	EXIT_IF(tls_handler_size > UINT64_MAX - program->base_size_aligned);
	program->mapped_size = program->base_size_aligned + tls_handler_size;
	const bool emulate_rsqrt = Config::AmdCpuEnabled();

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	const bool         protect_memory_faults   = Config::RedZoneProtectionEnabled();
	const bool         use_red_zone_protection = protect_memory_faults || emulate_rsqrt;
	constexpr uint64_t RED_ZONE_TRAMPOLINE_SIZE = 8u * 1024u * 1024u;
	if (use_red_zone_protection) {
		EXIT_IF(RED_ZONE_TRAMPOLINE_SIZE > UINT64_MAX - program->mapped_size);
		program->mapped_size += RED_ZONE_TRAMPOLINE_SIZE;
	}
#endif

	program->base_vaddr = Libs::LibKernel::Memory::AllocateProgramMemory(
	    g_desired_base_addr, program->mapped_size, Common::VirtualMemory::Mode::ExecuteReadWrite,
	    Common::PathToString(program->file_name.filename()).c_str());
	EXIT_IF(program->base_vaddr == 0);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (use_red_zone_protection) {
		program->red_zone_trampoline_vaddr = program->base_vaddr + program->base_size_aligned;
		program->red_zone_trampoline_size  = RED_ZONE_TRAMPOLINE_SIZE;
		RegisterRedZonePatchModule(reinterpret_cast<void*>(program->base_vaddr),
		                           program->base_size_aligned,
		                           reinterpret_cast<void*>(program->red_zone_trampoline_vaddr),
		                           program->red_zone_trampoline_size);
	}
#endif
	if (!is_shared && tls_handler_size != 0) {
		program->tls.handler_vaddr = program->base_vaddr + program->base_size_aligned;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		program->tls.handler_vaddr += program->red_zone_trampoline_size;
#endif
	}

	g_desired_base_addr += CODE_BASE_INCR * (1 + program->mapped_size / CODE_BASE_INCR);

	EXIT_IF(program->base_size_aligned < program->base_size);
	LOGF("base_vaddr             = 0x%016" PRIx64 "\n"
	     "base_size              = 0x%016" PRIx64 "\n"
	     "base_size_aligned      = 0x%016" PRIx64 "\n"
	     "mapped_size            = 0x%016" PRIx64 "\n",
	     program->base_vaddr, program->base_size, program->base_size_aligned, program->mapped_size);
	if (!is_shared) {
		LOGF("tls_handler_size       = 0x%016" PRIx64 "\n", tls_handler_size);
	}

#if !defined(LUMEN_PS5_FEXCORE)
	if (!Common::HostException::InstallHandler(KytyExceptionHandler)) {
		EXIT("Failed to install the required vectored exception handler\n");
	}
#endif

	std::vector<std::pair<uint64_t, uint64_t>> executable_segments;
#if defined(LUMEN_PS5_FEXCORE)
	struct SegmentProtection {
		uint64_t address;
		uint64_t size;
		Common::VirtualMemory::Mode mode;
	};
	std::vector<SegmentProtection> segment_protections;
#endif
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uint64_t                                   eh_frame_header_addr = 0;
	uint64_t                                   eh_frame_header_size = 0;
#endif

	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_memsz != 0 && (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO)) {
			uint64_t segment_addr        = phdr[i].p_vaddr + program->base_vaddr;
			uint64_t segment_file_size   = phdr[i].p_filesz;
			uint64_t segment_memory_size = GetAlignedSize(phdr + i);
			auto     mode                = GetMode(phdr[i].p_flags);

			LOGF("[%d] addr        = 0x%016" PRIx64 "\n"
			     "[%d] file_size   = %" PRIu64 "\n"
			     "[%d] memory_size = %" PRIu64 "\n"
			     "[%d] mode        = %s\n",
			     i, segment_addr, i, segment_file_size, i, segment_memory_size, i,
			     magic_enum::enum_name(mode));

			program->elf->LoadSegment(segment_addr, phdr[i].p_offset, segment_file_size);

			bool skip_protect = (phdr[i].p_type == PT_LOAD && is_next_gen &&
			                     mode == Common::VirtualMemory::Mode::NoAccess);

			if (Common::VirtualMemory::IsExecute(mode)) {
#if !defined(LUMEN_PS5_FEXCORE)
				PatchProgram(program, segment_addr, segment_memory_size);
#endif
				executable_segments.emplace_back(segment_addr, segment_file_size);
			}

			if (!skip_protect) {
#if defined(LUMEN_PS5_FEXCORE)
				segment_protections.push_back({segment_addr, segment_memory_size, mode});
#else
				Libs::LibKernel::Memory::SetProgramMemoryProtection(segment_addr,
				                                                    segment_memory_size, mode);

				if (Common::VirtualMemory::IsExecute(mode)) {
					Common::VirtualMemory::FlushInstructionCache(segment_addr, segment_memory_size);
				}
#endif
			}
		}

		if (phdr[i].p_type == PT_TLS) {
			EXIT_IF(phdr[i].p_vaddr >= program->base_size);

			program->tls.image_vaddr = phdr[i].p_vaddr + program->base_vaddr;
			program->tls.init_size   = std::min(phdr[i].p_filesz, GetAlignedSize(phdr + i));
			program->tls.image_size  = GetAlignedSize(phdr + i);
			program->tls.tcb_offset  = program->tls.image_size;

			LOGF("tls addr = 0x%016" PRIx64 "\n"
			     "tls init   = %" PRIu64 "\n"
			     "tls size   = %" PRIu64 "\n"
			     "tls offset = %" PRIu64 "\n",
			     program->tls.image_vaddr, program->tls.init_size, program->tls.image_size,
			     program->tls.tcb_offset);
		}

		if (phdr[i].p_type == PT_OS_PROCPARAM) {
			EXIT_IF(program->proc_param_vaddr != 0);
			EXIT_IF(phdr[i].p_vaddr >= program->base_size);

			program->proc_param_vaddr = phdr[i].p_vaddr + program->base_vaddr;
		}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (use_red_zone_protection && phdr[i].p_type == PT_GNU_EH_FRAME) {
			eh_frame_header_addr = phdr[i].p_vaddr + program->base_vaddr;
			eh_frame_header_size = phdr[i].p_memsz;
		}
#endif
	}

#if defined(LUMEN_PS5_FEXCORE)
	// RELRO and PT_LOAD may overlap. Populate the complete image before
	// installing native read-only permissions on any of those pages.
	for (const auto& protection: segment_protections) {
		Libs::LibKernel::Memory::SetProgramMemoryProtection(
		    protection.address, protection.size, protection.mode);
	}
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	std::vector<uintptr_t> function_starts;
	if (use_red_zone_protection) {
		if (!DecodeEhFrameFunctionStarts(eh_frame_header_addr, eh_frame_header_size,
		                                 &function_starts)) {
			LOGF("Windows guest red-zone patching could not decode function boundaries for %s\n",
			     Common::PathToString(program->file_name).c_str());
		}
	}
#endif
#if !defined(LUMEN_PS5_FEXCORE) && (defined(__x86_64__) || defined(_M_X64))
	for (const auto& [segment_addr, segment_size]: executable_segments) {
		uint64_t reciprocal_sqrt_count = 0;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (use_red_zone_protection) {
			const auto result =
			    PatchGuestInstructions(segment_addr, segment_size, function_starts,
			                           protect_memory_faults, emulate_rsqrt);
			LOGF("Windows guest red-zone patching: %s, functions=%" PRIu64 ", red_zone=%" PRIu64
			     ", memory=%" PRIu64 ", patched=%" PRIu64 ", short=%" PRIu64 ", stack=%" PRIu64
			     ", control=%" PRIu64 ", unrelocatable=%" PRIu64 "\n",
			     Common::PathToString(program->file_name.filename()).c_str(), result.function_count,
			     result.red_zone_function_count, result.memory_instruction_count,
			     result.patched_memory_instruction_count, result.short_memory_instruction_count,
			     result.stack_dependent_memory_instruction_count,
			     result.control_flow_memory_instruction_count,
			     result.unrelocatable_memory_instruction_count);
			reciprocal_sqrt_count = result.reciprocal_sqrt_instruction_count;
		}
#else
		if (emulate_rsqrt) {
			reciprocal_sqrt_count =
			    X64InstructionEmulator::PatchReciprocalSquareRoots(segment_addr, segment_size);
			Common::VirtualMemory::FlushInstructionCache(segment_addr, segment_size);
		}
#endif
		if (reciprocal_sqrt_count != 0) {
			LOGF("Guest VRSQRTPS emulation: %s, instructions=%" PRIu64 "\n",
			     Common::PathToString(program->file_name.filename()).c_str(), reciprocal_sqrt_count);
		}
	}

	if (!is_shared) {
		SetupTlsHandler(program);
	}
#else
	if (!is_shared) {
		EXIT_IF(g_tls_main_program != nullptr);
		g_tls_main_program = program;
	}
#endif

	LOGF("entry = 0x%016" PRIx64 "\n", program->elf->GetEntry() + program->base_vaddr);
}

void RuntimeLinker::DeleteProgram(Program* p) {
	RejectGuestHeapTlsTeardown(p);
	ReleaseFunctionFallbacks(p->unique_id);
	auto program = std::unique_ptr<Program>(p);
	if (g_tls_main_program == program.get()) {
		g_tls_main_program = nullptr;
	}
	if (g_tls_cached_main_program == program.get()) {
		g_tls_cached_main_program = nullptr;
		g_tls_cached_main_tcb     = nullptr;
	}

	if (program->base_vaddr != 0 || program->mapped_size != 0) {
		EXIT_IF(program->base_vaddr == 0 || program->mapped_size == 0);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (program->red_zone_trampoline_size != 0) {
			UnregisterRedZonePatchModule(reinterpret_cast<void*>(program->base_vaddr));
		}
#endif
#if defined(LUMEN_PS5_FEXCORE)
		if (!Libs::LibKernel::Memory::FreeGuestMemory(program->base_vaddr, program->mapped_size))
			EXIT("PS5 program memory release failed\n");
#else
		EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(program->base_vaddr, program->mapped_size));
#endif
	}
}

void RuntimeLinker::ParseProgramDynamicInfo(Program* program) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(program->dynamic_info != nullptr);

	program->dynamic_info = std::make_unique<DynamicInfo>();

	auto* elf = program->elf.get();

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_HASH) && elf->HasDynValue(DT_HASH));
	GetDynDataOs(elf, &program->dynamic_info->hash_table, DT_OS_HASH);
	GetDynData(elf, program->base_vaddr, &program->dynamic_info->hash_table, DT_HASH);
	GetDynValue(elf, &program->dynamic_info->hash_table_size, DT_OS_HASHSZ);

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_STRTAB) && elf->HasDynValue(DT_STRTAB));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_STRSZ) && elf->HasDynValue(DT_STRSZ));
	GetDynDataOs(elf, &program->dynamic_info->str_table, DT_OS_STRTAB);
	GetDynData(elf, program->base_vaddr, &program->dynamic_info->str_table, DT_STRTAB);
	GetDynValue(elf, &program->dynamic_info->str_table_size, DT_OS_STRSZ);
	GetDynValue(elf, &program->dynamic_info->str_table_size, DT_STRSZ);

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_SYMTAB) && elf->HasDynValue(DT_SYMTAB));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_SYMENT) && elf->HasDynValue(DT_SYMENT));
	GetDynDataOs(elf, &program->dynamic_info->symbol_table, DT_OS_SYMTAB);
	GetDynData(elf, program->base_vaddr, &program->dynamic_info->symbol_table, DT_SYMTAB);
	GetDynValue(elf, &program->dynamic_info->symbol_table_total_size, DT_OS_SYMTABSZ);
	GetDynValue(elf, &program->dynamic_info->symbol_table_entry_size, DT_OS_SYMENT);
	GetDynValue(elf, &program->dynamic_info->symbol_table_entry_size, DT_SYMENT);

	GetDynPtr(elf, &program->dynamic_info->init_vaddr, DT_INIT);
	GetDynPtr(elf, &program->dynamic_info->fini_vaddr, DT_FINI);
	GetDynPtr(elf, &program->dynamic_info->init_array_vaddr, DT_INIT_ARRAY);
	GetDynPtr(elf, &program->dynamic_info->fini_array_vaddr, DT_FINI_ARRAY);
	GetDynPtr(elf, &program->dynamic_info->preinit_array_vaddr, DT_PREINIT_ARRAY);
	GetDynValue(elf, &program->dynamic_info->init_array_size, DT_INIT_ARRAYSZ);
	GetDynValue(elf, &program->dynamic_info->fini_array_size, DT_FINI_ARRAYSZ);
	GetDynValue(elf, &program->dynamic_info->preinit_array_size, DT_PREINIT_ARRAYSZ);

	Elf64_Sxword jmprel_type = 0;
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_PLTREL) && elf->HasDynValue(DT_PLTREL));
	GetDynValue(elf, &jmprel_type, DT_OS_PLTREL);
	GetDynValue(elf, &jmprel_type, DT_PLTREL);

	EXIT_NOT_IMPLEMENTED(jmprel_type != DT_RELA);
	if (jmprel_type == DT_RELA) {
		EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_JMPREL) && elf->HasDynValue(DT_JMPREL));
		EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_PLTRELSZ) && elf->HasDynValue(DT_PLTRELSZ));
		GetDynDataOs(elf, &program->dynamic_info->jmprela_table, DT_OS_JMPREL);
		GetDynData(elf, program->base_vaddr, &program->dynamic_info->jmprela_table, DT_JMPREL);
		GetDynValue(elf, &program->dynamic_info->jmprela_table_size, DT_OS_PLTRELSZ);
		GetDynValue(elf, &program->dynamic_info->jmprela_table_size, DT_PLTRELSZ);
	}

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_RELA) && elf->HasDynValue(DT_RELA));
	GetDynDataOs(elf, &program->dynamic_info->rela_table, DT_OS_RELA);
	GetDynData(elf, program->base_vaddr, &program->dynamic_info->rela_table, DT_RELA);
	GetDynValue(elf, &program->dynamic_info->rela_table_total_size, DT_OS_RELASZ);
	GetDynValue(elf, &program->dynamic_info->rela_table_total_size, DT_RELASZ);
	GetDynValue(elf, &program->dynamic_info->rela_table_entry_size, DT_OS_RELAENT);
	GetDynValue(elf, &program->dynamic_info->rela_table_entry_size, DT_RELAENT);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->rela_table_total_size % sizeof(Elf64_Rela) != 0 ||
	                     program->dynamic_info->jmprela_table_size % sizeof(Elf64_Rela) != 0);
	const auto rela_count = program->dynamic_info->rela_table_total_size / sizeof(Elf64_Rela);
	const auto jmprela_count = program->dynamic_info->jmprela_table_size / sizeof(Elf64_Rela);
	program->rela_bits.resize((rela_count + jmprela_count + 7u) / 8u);

	GetDynValue(elf, &program->dynamic_info->relative_count, DT_RELACOUNT);

	GetDynValue(elf, &program->dynamic_info->debug, DT_DEBUG);
	GetDynValue(elf, &program->dynamic_info->flags, DT_FLAGS);
	GetDynValue(elf, &program->dynamic_info->textrel, DT_TEXTREL);

	EXIT_NOT_IMPLEMENTED(program->dynamic_info->debug != 0);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->textrel != 0);

	std::vector<uint64_t> needed;
	GetDynValues(elf, &needed, DT_NEEDED);
	for (auto need: needed) {
		program->dynamic_info->needed.push_back(program->dynamic_info->str_table + need);
	}

	uint64_t so_name = 0;
	GetDynValue(elf, &so_name, DT_SONAME);
	program->dynamic_info->so_name = program->dynamic_info->str_table + so_name;

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_NEEDED_MODULE) &&
	                     elf->HasDynValue(DT_OS_NEEDED_MODULE_1));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_MODULE_INFO) &&
	                     elf->HasDynValue(DT_OS_MODULE_INFO_1));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_IMPORT_LIB) &&
	                     elf->HasDynValue(DT_OS_IMPORT_LIB_1));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_EXPORT_LIB) &&
	                     elf->HasDynValue(DT_OS_EXPORT_LIB_1));
	GetDynModules(elf, &program->dynamic_info->import_modules, program->dynamic_info->str_table,
	              DT_OS_NEEDED_MODULE);
	GetDynModules(elf, &program->dynamic_info->import_modules, program->dynamic_info->str_table,
	              DT_OS_NEEDED_MODULE_1);
	GetDynModules(elf, &program->dynamic_info->export_modules, program->dynamic_info->str_table,
	              DT_OS_MODULE_INFO);
	GetDynModules(elf, &program->dynamic_info->export_modules, program->dynamic_info->str_table,
	              DT_OS_MODULE_INFO_1);
	GetDynLibs(elf, &program->dynamic_info->import_libs, program->dynamic_info->str_table,
	           DT_OS_IMPORT_LIB);
	GetDynLibs(elf, &program->dynamic_info->import_libs, program->dynamic_info->str_table,
	           DT_OS_IMPORT_LIB_1);
	GetDynLibs(elf, &program->dynamic_info->export_libs, program->dynamic_info->str_table,
	           DT_OS_EXPORT_LIB);
	GetDynLibs(elf, &program->dynamic_info->export_libs, program->dynamic_info->str_table,
	           DT_OS_EXPORT_LIB_1);
}

void RuntimeLinker::Relocate(Program* program) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr);



	LOGF_COLOR(Log::Color::White, "--- Relocate program: %s ---\n",
	           Common::PathToString(program->file_name).c_str());

	EXIT_NOT_IMPLEMENTED(program->dynamic_info->symbol_table_entry_size != sizeof(Elf64_Sym));
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->rela_table_entry_size != sizeof(Elf64_Rela));
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->jmprela_table == nullptr);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->rela_table == nullptr);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->symbol_table == nullptr);

	const auto rela_count = static_cast<uint32_t>(
	    program->dynamic_info->rela_table_total_size / sizeof(Elf64_Rela));

	ForEachRelocation(program, [&](Elf64_Rela* r, uint32_t index, bool jmprela) {
		const auto bit_index = (jmprela ? rela_count : 0) + index;
		if ((program->rela_bits[bit_index >> 3u] & (1u << (bit_index & 7u))) == 0 &&
		    RelocateRecord(index, r, program, jmprela)) {
			program->rela_bits[bit_index >> 3u] |= 1u << (bit_index & 7u);
		}
	});

	if (program->tls.image_vaddr != 0 && program->tls.init_size != 0 &&
	    program->tls.init_image.empty()) {
		const auto* src = reinterpret_cast<const uint8_t*>(program->tls.image_vaddr);
		program->tls.init_image.assign(src, src + program->tls.init_size);
	}
}

const ModuleId* RuntimeLinker::FindModule(const Program& program, const std::string& id) {
	const auto& import_modules = program.dynamic_info->import_modules;

	if (auto it = std::find_if(import_modules.begin(), import_modules.end(),
	                           [&id](const auto& module) { return module.id == id; });
	    it != import_modules.end()) {
		return &(*it);
	}

	const auto& export_modules = program.dynamic_info->export_modules;

	if (auto it = std::find_if(export_modules.begin(), export_modules.end(),
	                           [&id](const auto& module) { return module.id == id; });
	    it != export_modules.end()) {
		return &(*it);
	}

	return nullptr;
}

const LibraryId* RuntimeLinker::FindLibrary(const Program& program, const std::string& id) {
	const auto& import_libs = program.dynamic_info->import_libs;

	if (auto it = std::find_if(import_libs.begin(), import_libs.end(),
	                           [&id](const auto& lib) { return lib.id == id; });
	    it != import_libs.end()) {
		return &(*it);
	}

	const auto& export_libs = program.dynamic_info->export_libs;

	if (auto it = std::find_if(export_libs.begin(), export_libs.end(),
	                           [&id](const auto& lib) { return lib.id == id; });
	    it != export_libs.end()) {
		return &(*it);
	}

	return nullptr;
}

void RuntimeLinker::CreateSymbolDatabase(Program* program) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr);
	EXIT_IF(program->export_symbols != nullptr);
	EXIT_IF(program->import_symbols != nullptr);

	program->export_symbols = std::make_unique<SymbolDatabase>();
	program->import_symbols = std::make_unique<SymbolDatabase>();

	auto syms = [](Program* program, SymbolDatabase* symbols, bool is_export) {
		if (program->dynamic_info->symbol_table == nullptr ||
		    program->dynamic_info->str_table == nullptr) {
			return;
		}

		for (auto* sym = program->dynamic_info->symbol_table;
		     reinterpret_cast<uint8_t*>(sym) <
		     reinterpret_cast<uint8_t*>(program->dynamic_info->symbol_table) +
		         program->dynamic_info->symbol_table_total_size;
		     sym++) {
			std::string id   = std::string(program->dynamic_info->str_table + sym->st_name);
			auto        bind = sym->GetBind();
			auto        type = sym->GetType();
			auto        ids  = Common::Split(id, '#');

			if (ids.size() == 3) {
				const auto* l = FindLibrary(*program, ids.at(1));
				const auto* m = FindModule(*program, ids.at(2));

				if (l != nullptr && m != nullptr && (bind == STB_GLOBAL || bind == STB_WEAK) &&
				    (type == STT_FUNC || type == STT_OBJECT || type == STT_NOTYPE) &&
				    is_export == (sym->st_value != 0)) {
					SymbolResolve sr {};
					sr.name                 = ids.at(0);
					sr.library              = l->name;
					sr.library_version      = l->version;
					sr.module               = m->name;
					sr.module_version_major = m->version_major;
					sr.module_version_minor = m->version_minor;
					switch (type) {
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

	syms(program, program->export_symbols.get(), true);
	syms(program, program->import_symbols.get(), false);
}

void RuntimeLinker::SetupTlsHandler(Program* program) {
	EXIT_IF(program == nullptr);
	EXIT_IF(g_tls_main_program != nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(program->elf->IsShared());
	EXIT_IF(program->tls.handler_vaddr == 0);

	g_tls_main_program = program;

	auto* code = new (reinterpret_cast<void*>(program->tls.handler_vaddr)) Jit::SafeCall;

	code->SetFunc(TlsMainGetAddr);

	for (uint8_t reg = 1; reg < 8; reg++) {
		if (reg == 4) {
			continue;
		}

		auto* stub = new (reinterpret_cast<void*>(program->tls.handler_vaddr +
		                                          Jit::TlsRegStub::GetOffset(reg))) Jit::TlsRegStub;
		stub->SetFunc(program->tls.handler_vaddr);
		stub->SetOutputReg(reg);
	}

	EXIT_IF(!Libs::LibKernel::Memory::ProtectGuestMemory(program->tls.handler_vaddr,
	                                                     Jit::SafeCall::GetSize(),
	                                                     Common::VirtualMemory::Mode::Execute));
	Common::VirtualMemory::FlushInstructionCache(program->tls.handler_vaddr,
	                                             Jit::SafeCall::GetSize());
}

void RuntimeLinker::DeleteTlss(int thread_id) {
#if defined(LUMEN_PS5_FEXCORE)
	std::vector<ThreadLocalStorage::Block> retired;
	{
		// CleanupThread runs after its guest root and destructor callbacks have
		// returned. Native snapshots are the only retained state at this boundary.
		GuestExecution::GuestMutationScope mutation_scope(
		    GuestExecution::GuestMutationScope::Admission::NativeRootEntry);
		Common::LockGuard lock(m_mutex);
		for (auto* program: m_programs) {
			Common::LockGuard tls_lock(program->tls.mutex);
			if (auto it = program->tls.tlss.find(thread_id); it != program->tls.tlss.end()) {
				retired.push_back(it->second);
				program->tls.tlss.erase(it);
			}
			if (thread_id == Common::Thread::GetThreadIdUnique() &&
			    g_tls_cached_main_program == program) {
				g_tls_cached_main_program = nullptr;
				g_tls_cached_main_tcb = nullptr;
			}
		}
	}
	for (auto& block: retired) FreeTlsBlock(&block);
#else
	Common::LockGuard lock(m_mutex);
	for (auto* program: m_programs) DeleteTls(program, thread_id);
#endif
}

void RuntimeLinker::SetApplicationHeapApi(void* const api[10]) {
	Common::LockGuard lock(m_mutex);

	if (api == nullptr || api[0] == nullptr || api[1] == nullptr) {
		m_application_heap_malloc         = nullptr;
		m_application_heap_free           = nullptr;
		m_application_heap_posix_memalign = nullptr;
		return;
	}

	m_application_heap_malloc = reinterpret_cast<application_heap_malloc_func_t>(api[0]);
	m_application_heap_free   = reinterpret_cast<application_heap_free_func_t>(api[1]);
	m_application_heap_posix_memalign =
	    reinterpret_cast<application_heap_posix_memalign_func_t>(api[6]);
}

void* RuntimeLinker::ApplicationHeapMemalign(uint64_t alignment, uint64_t size) {
	ProgramCallPins pins;
	application_heap_posix_memalign_func_t callback = nullptr;
	{
		Common::LockGuard lock(m_mutex);
		callback = m_application_heap_posix_memalign;
		if (callback != nullptr) {
			if (auto* program = FindProgramByAddr(reinterpret_cast<uint64_t>(callback))) {
				pins.Add(program);
			}
		}
	}

	if (callback != nullptr) {
		void* ptr = nullptr;
		GuestCallback::Object<void*> output(ptr);
		const auto result = GuestCallback::Call(callback,
		                                       output.Get(), alignment, size);
		output.CopyBack(ptr);
		return result == 0 ? ptr : nullptr;
	}

	return nullptr;
}

void* RuntimeLinker::ApplicationHeapMalloc(uint64_t size) {
	ProgramCallPins pins;
	application_heap_malloc_func_t callback = nullptr;
	{
		Common::LockGuard lock(m_mutex);
		callback = m_application_heap_malloc;
		if (callback != nullptr) {
			if (auto* program = FindProgramByAddr(reinterpret_cast<uint64_t>(callback))) {
				pins.Add(program);
			}
		}
	}

	return callback != nullptr ? GuestCallback::Call(callback, size) : nullptr;
}

} // namespace Loader
