#include "Emulator/Common.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("Fiber", 1, "Fiber", 1, 1);

namespace Fiber {

// --- Constants ---
[[maybe_unused]] static constexpr uint32_t FIBER_MAGIC_START    = 0xdef1649c;
[[maybe_unused]] static constexpr uint32_t FIBER_MAGIC_END      = 0xb37592a0;
[[maybe_unused]] static constexpr uint32_t FIBER_STATE_IDLE      = 1;
[[maybe_unused]] static constexpr uint32_t FIBER_STATE_RUN       = 2;
[[maybe_unused]] static constexpr uint32_t FIBER_STATE_TERMINATED = 3;
[[maybe_unused]] static constexpr uint64_t FIBER_CONTEXT_MINIMUM = 4096;

// PS4 error codes
static constexpr int SCE_FIBER_OK                     = 0;
[[maybe_unused]] static constexpr int SCE_FIBER_ERROR_NULL              = 0x80980001;
[[maybe_unused]] static constexpr int SCE_FIBER_ERROR_ALIGNMENT         = 0x80980002;
[[maybe_unused]] static constexpr int SCE_FIBER_ERROR_RANGE             = 0x80980003;
static constexpr int SCE_FIBER_ERROR_INVALID           = 0x80980004;
static constexpr int SCE_FIBER_ERROR_PERMISSION        = 0x80980005;
[[maybe_unused]] static constexpr int SCE_FIBER_ERROR_STATE             = 0x80980006;

// --- Fiber state structure (must fit in 128 bytes — the opaque SceFiber) ---
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
struct OrbisFiber
{
	uint32_t magic_start;        // 0x00: 0xdef1649c
	uint32_t state;              // 0x04: FIBER_STATE_*
	void*    win_fiber;          // 0x08: Windows fiber handle
	uint64_t entry;              // 0x10: PS4 entry function (sysv_abi)
	uint64_t arg_on_initialize;  // 0x18: arg passed at creation
	uint64_t arg_on_run_to;      // 0x20: arg passed to Run/Switch
	uint64_t* p_arg_on_return;   // 0x28: where to write return arg
	void*    addr_context;       // 0x30: game-provided context memory
	uint64_t size_context;       // 0x38: context memory size
	char     name[32];           // 0x40: fiber name (31 chars + null)
	uint32_t magic_end;          // 0x60: 0xb37592a0
	uint32_t pad;                // 0x64: padding
	// Total: 0x68 = 104 bytes (fits in 128-byte SceFiber)
};
static_assert(sizeof(OrbisFiber) <= 128, "OrbisFiber must fit in 128-byte SceFiber");

// --- Thread-local fiber context ---
struct FiberThreadContext
{
	void*       thread_fiber;   // ConvertThreadToFiber result
	OrbisFiber* current_fiber;  // Currently running fiber (null if on thread)
	uint64_t    arg_on_return;  // Return argument from fiber
	bool        initialized;    // Has this thread been converted?
};
static thread_local FiberThreadContext t_fiber_ctx = {};

// --- Logging counters ---
static std::atomic<int> g_fiber_init_count{0};
static std::atomic<int> g_fiber_run_count{0};
static std::atomic<int> g_fiber_switch_count{0};
static std::atomic<int> g_fiber_return_count{0};
static constexpr int    LOG_LIMIT = 50;

// --- Ensure current thread is fiber-capable ---
static void EnsureThreadFiber()
{
	if (!t_fiber_ctx.initialized)
	{
		// Check if already a fiber (e.g., from another ConvertThreadToFiber call)
		// IsThreadAFiber() is not declared by some MinGW Windows headers.
		// GetCurrentFiber() provides the same check and is available there.
		if (GetCurrentFiber() != nullptr)
		{
			t_fiber_ctx.thread_fiber = GetCurrentFiber();
		}
		else
		{
			t_fiber_ctx.thread_fiber = ConvertThreadToFiber(nullptr);
		}
		t_fiber_ctx.initialized = true;
		if (!t_fiber_ctx.thread_fiber)
		{
			printf("[FIBER_ERROR] ConvertThreadToFiber failed: %lu\n", GetLastError());
		}
	}
}

// --- PS4 fiber entry function type (System V ABI) ---
using SceFiberEntry = void (LUMEN_SYSV_ABI*)(uint64_t arg_on_initialize, uint64_t arg_on_run);

// --- Windows fiber entry wrapper ---
static void WINAPI FiberEntryWrapper(LPVOID param)
{
	auto* fiber = static_cast<OrbisFiber*>(param);

	for (;;)
	{
		// Call the PS4 entry function with System V calling convention
		auto entry = reinterpret_cast<SceFiberEntry>(fiber->entry);
		entry(fiber->arg_on_initialize, fiber->arg_on_run_to);

		// If the entry function returns without calling sceFiberReturnToThread,
		// treat it as an implicit return-to-thread with arg=0
		fiber->state = FIBER_STATE_IDLE;
		t_fiber_ctx.current_fiber = nullptr;

		if (t_fiber_ctx.thread_fiber)
		{
			SwitchToFiber(t_fiber_ctx.thread_fiber);
		}
		else
		{
			printf("[FIBER_ERROR] FiberEntryWrapper: no thread_fiber to return to!\n");
			break;
		}
		// If we get switched back in (re-run), loop and call entry again
	}
}

// --- sceFiberInitialize (NID: hVYD7Ou2pCQ) ---
static int LUMEN_SYSV_ABI sceFiberInitialize(OrbisFiber* fiber, const char* name,
                                             uint64_t entry, uint64_t arg_on_initialize,
                                             void* addr_context, uint64_t size_context,
                                             void* opt_param)
{
	if (!fiber || !name || !entry)
	{
		return SCE_FIBER_ERROR_NULL;
	}

	if (reinterpret_cast<uintptr_t>(fiber) % 8 != 0)
	{
		return SCE_FIBER_ERROR_ALIGNMENT;
	}

	if (size_context < FIBER_CONTEXT_MINIMUM)
	{
		return SCE_FIBER_ERROR_RANGE;
	}

	// Create Windows fiber with game-specified stack size
	void* win_fiber = CreateFiber(static_cast<SIZE_T>(size_context), FiberEntryWrapper, fiber);
	if (!win_fiber)
	{
		printf("[FIBER_ERROR] CreateFiber failed for '%s': %lu\n", name, GetLastError());
		return SCE_FIBER_ERROR_INVALID;
	}

	// Populate fiber structure
	fiber->magic_start       = FIBER_MAGIC_START;
	fiber->state             = FIBER_STATE_IDLE;
	fiber->win_fiber         = win_fiber;
	fiber->entry             = entry;
	fiber->arg_on_initialize = arg_on_initialize;
	fiber->arg_on_run_to     = 0;
	fiber->p_arg_on_return   = nullptr;
	fiber->addr_context      = addr_context;
	fiber->size_context      = size_context;
	std::strncpy(fiber->name, name, 31);
	fiber->name[31] = '\0';
	fiber->magic_end = FIBER_MAGIC_END;
	fiber->pad       = 0;

	int count = g_fiber_init_count.fetch_add(1) + 1;
	if (count <= LOG_LIMIT)
	{
		printf("[FIBER_INIT #%d] name=\"%s\" entry=0x%llx ctx_size=%lluKB\n",
		       count, name, static_cast<unsigned long long>(entry),
		       static_cast<unsigned long long>(size_context / 1024));
	}

	return SCE_FIBER_OK;
}

// --- sceFiberRun (NID: a0LLrZWac0M) ---
static int LUMEN_SYSV_ABI sceFiberRun(OrbisFiber* fiber, uint64_t arg_on_run_to, uint64_t* arg_on_return)
{
	if (!fiber)
	{
		return SCE_FIBER_ERROR_NULL;
	}

	if (fiber->magic_start != FIBER_MAGIC_START || fiber->magic_end != FIBER_MAGIC_END)
	{
		return SCE_FIBER_ERROR_INVALID;
	}

	if (fiber->state != FIBER_STATE_IDLE)
	{
		return SCE_FIBER_ERROR_STATE;
	}

	if (t_fiber_ctx.current_fiber != nullptr)
	{
		// Already running a fiber — can't Run, must Switch
		return SCE_FIBER_ERROR_PERMISSION;
	}

	EnsureThreadFiber();

	fiber->arg_on_run_to   = arg_on_run_to;
	fiber->p_arg_on_return = arg_on_return;
	fiber->state           = FIBER_STATE_RUN;
	t_fiber_ctx.current_fiber = fiber;
	t_fiber_ctx.arg_on_return = 0;

	int count = g_fiber_run_count.fetch_add(1) + 1;
	if (count <= LOG_LIMIT)
	{
		printf("[FIBER_RUN #%d] name=\"%s\" arg=0x%llx\n",
		       count, fiber->name, static_cast<unsigned long long>(arg_on_run_to));
	}

	// Switch to the fiber — blocks until fiber yields back
	SwitchToFiber(fiber->win_fiber);

	// Fiber has returned to us
	if (arg_on_return)
	{
		*arg_on_return = t_fiber_ctx.arg_on_return;
	}

	return SCE_FIBER_OK;
}

// --- sceFiberSwitch (NID: PFT2S-tJ7Uk) ---
static int LUMEN_SYSV_ABI sceFiberSwitch(OrbisFiber* fiber, uint64_t arg_on_run_to, uint64_t* arg_on_return)
{
	if (!fiber)
	{
		return SCE_FIBER_ERROR_NULL;
	}

	if (fiber->magic_start != FIBER_MAGIC_START || fiber->magic_end != FIBER_MAGIC_END)
	{
		return SCE_FIBER_ERROR_INVALID;
	}

	if (fiber->state != FIBER_STATE_IDLE)
	{
		return SCE_FIBER_ERROR_STATE;
	}

	OrbisFiber* prev = t_fiber_ctx.current_fiber;
	if (!prev)
	{
		// Not currently in a fiber — use Run instead
		return SCE_FIBER_ERROR_PERMISSION;
	}

	// Save previous fiber state
	prev->state = FIBER_STATE_IDLE;

	// Set up target fiber
	fiber->arg_on_run_to   = arg_on_run_to;
	fiber->p_arg_on_return = arg_on_return;
	fiber->state           = FIBER_STATE_RUN;
	t_fiber_ctx.current_fiber = fiber;

	int count = g_fiber_switch_count.fetch_add(1) + 1;
	if (count <= LOG_LIMIT)
	{
		printf("[FIBER_SWITCH #%d] from=\"%s\" to=\"%s\" arg=0x%llx\n",
		       count, prev->name, fiber->name, static_cast<unsigned long long>(arg_on_run_to));
	}

	// Switch to target fiber
	SwitchToFiber(fiber->win_fiber);

	// We've been switched back to
	if (arg_on_return)
	{
		*arg_on_return = t_fiber_ctx.arg_on_return;
	}

	return SCE_FIBER_OK;
}

// --- sceFiberReturnToThread (NID: B0ZX2hx9DMw) ---
static int LUMEN_SYSV_ABI sceFiberReturnToThread(uint64_t arg_on_return, uint64_t* arg_on_run)
{
	OrbisFiber* fiber = t_fiber_ctx.current_fiber;
	if (!fiber)
	{
		return SCE_FIBER_ERROR_PERMISSION;
	}

	int count = g_fiber_return_count.fetch_add(1) + 1;
	if (count <= LOG_LIMIT)
	{
		printf("[FIBER_RETURN #%d] name=\"%s\" arg=0x%llx\n",
		       count, fiber->name, static_cast<unsigned long long>(arg_on_return));
	}

	// Store return argument for the thread to pick up
	t_fiber_ctx.arg_on_return = arg_on_return;

	// If fiber wants to know next run arg, store pointer
	// (will be populated on next Run/Switch)

	fiber->state = FIBER_STATE_IDLE;
	t_fiber_ctx.current_fiber = nullptr;

	if (t_fiber_ctx.thread_fiber)
	{
		SwitchToFiber(t_fiber_ctx.thread_fiber);
	}

	// We've been re-entered (another Run/Switch to this fiber)
	// Update the arg_on_run pointer if provided
	if (arg_on_run)
	{
		*arg_on_run = fiber->arg_on_run_to;
	}

	return SCE_FIBER_OK;
}

// --- sceFiberFinalize (NID: JeNX5F-NzQU) ---
static int LUMEN_SYSV_ABI sceFiberFinalize(OrbisFiber* fiber)
{
	if (!fiber)
	{
		return SCE_FIBER_ERROR_NULL;
	}

	if (fiber->magic_start != FIBER_MAGIC_START || fiber->magic_end != FIBER_MAGIC_END)
	{
		return SCE_FIBER_ERROR_INVALID;
	}

	if (fiber->state == FIBER_STATE_RUN)
	{
		return SCE_FIBER_ERROR_STATE;
	}

	if (fiber->win_fiber)
	{
		DeleteFiber(fiber->win_fiber);
		fiber->win_fiber = nullptr;
	}

	fiber->state       = FIBER_STATE_TERMINATED;
	fiber->magic_start = 0;
	fiber->magic_end   = 0;

	return SCE_FIBER_OK;
}

// --- sceFiberGetSelf (NID: p+zLIOg27zU) ---
static int LUMEN_SYSV_ABI sceFiberGetSelf(OrbisFiber** fiber)
{
	if (!fiber)
	{
		return SCE_FIBER_ERROR_NULL;
	}

	*fiber = t_fiber_ctx.current_fiber;

	return SCE_FIBER_OK;
}

#else

// The original backend uses the Windows fiber API. Keep the symbols available
// on POSIX platforms until a native context-switching backend is introduced.
struct OrbisFiber {};

static int LUMEN_SYSV_ABI sceFiberInitialize(OrbisFiber*, const char*, uint64_t,
                                             uint64_t, void*, uint64_t, void*)
{
	return SCE_FIBER_ERROR_INVALID;
}

static int LUMEN_SYSV_ABI sceFiberRun(OrbisFiber*, uint64_t, uint64_t*)
{
	return SCE_FIBER_ERROR_INVALID;
}

static int LUMEN_SYSV_ABI sceFiberSwitch(OrbisFiber*, uint64_t, uint64_t*)
{
	return SCE_FIBER_ERROR_INVALID;
}

static int LUMEN_SYSV_ABI sceFiberReturnToThread(uint64_t, uint64_t*)
{
	return SCE_FIBER_ERROR_PERMISSION;
}

static int LUMEN_SYSV_ABI sceFiberFinalize(OrbisFiber*)
{
	return SCE_FIBER_ERROR_INVALID;
}

static int LUMEN_SYSV_ABI sceFiberGetSelf(OrbisFiber** fiber)
{
	if (fiber != nullptr)
	{
		*fiber = nullptr;
	}
	return SCE_FIBER_OK;
}

#endif

} // namespace Fiber

LIB_DEFINE(InitFiber_1)
{
	LIB_FUNC("JeNX5F-NzQU", Fiber::sceFiberFinalize);
	LIB_FUNC("p+zLIOg27zU", Fiber::sceFiberGetSelf);
	LIB_FUNC("B0ZX2hx9DMw", Fiber::sceFiberReturnToThread);
	LIB_FUNC("a0LLrZWac0M", Fiber::sceFiberRun);
	LIB_FUNC("PFT2S-tJ7Uk", Fiber::sceFiberSwitch);
	LIB_FUNC("hVYD7Ou2pCQ", Fiber::sceFiberInitialize);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
