// Original PS5 fiber ABI and state machine, shared with the direct-engine fixture.
namespace Fiber {

LIB_VERSION("Fiber", 1, "Fiber", 1, 1);

constexpr int32_t FIBER_ERROR_NULL       = -2141650943; /* 0x80590001 */
constexpr int32_t FIBER_ERROR_ALIGNMENT  = -2141650942; /* 0x80590002 */
constexpr int32_t FIBER_ERROR_RANGE      = -2141650941; /* 0x80590003 */
constexpr int32_t FIBER_ERROR_INVALID    = -2141650940; /* 0x80590004 */
constexpr int32_t FIBER_ERROR_PERMISSION = -2141650939; /* 0x80590005 */
constexpr int32_t FIBER_ERROR_STATE      = -2141650938; /* 0x80590006 */

constexpr uint32_t FIBER_MAGIC_START       = 0xdef1649c;
constexpr uint32_t FIBER_MAGIC_END         = 0xb37592a0;
constexpr uint32_t FIBER_OPT_MAGIC         = 0xbb40e64d;
constexpr uint64_t FIBER_STACK_MAGIC       = 0x7149f2ca7149f2ca;
constexpr uint64_t FIBER_CONTEXT_MIN_SIZE  = 512;
constexpr size_t   FIBER_MAX_NAME_LENGTH   = 31;
constexpr uint32_t FIBER_STATE_RUNNING     = 1;
constexpr uint32_t FIBER_STATE_IDLE        = 2;
constexpr uint32_t FIBER_STATE_TERMINATED  = 3;
constexpr uint32_t FIBER_FLAG_SET_FPU_REGS = 0x100;

using FiberEntry = KYTY_SYSV_ABI void (*)(uint64_t arg_on_initialize, uint64_t arg_on_run);

struct FiberOptParam {
	uint32_t magic;
};

struct FiberCpuContext {
	uint64_t rbx;
	uint64_t rbp;
	uint64_t rdi;
	uint64_t rsi;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;
	uint64_t rsp;
	uint64_t rip;
};

static_assert(sizeof(FiberCpuContext) == 80);

struct FiberObject {
	uint32_t        magic_start;
	uint32_t        state;
	FiberEntry      entry;
	uint64_t        arg_on_initialize;
	void*           addr_context;
	uint64_t        size_context;
	char            name[FIBER_MAX_NAME_LENGTH + 1];
	void*           context;
	uint32_t        flags;
	uint32_t        padding;
	void*           context_start;
	void*           context_end;
	FiberCpuContext saved_context;
	uint64_t        arg_on_run;
	uint64_t        arg_on_return;
	bool            context_valid;
	uint32_t        magic_end;
};

static_assert(sizeof(FiberObject) <= 256);

struct FiberInfo {
	uint64_t   size;
	FiberEntry entry;
	uint64_t   arg_on_initialize;
	void*      addr_context;
	uint64_t   size_context;
	char       name[FIBER_MAX_NAME_LENGTH + 1];
	uint64_t   size_context_margin;
	uint8_t    padding[48];
};

static_assert(sizeof(FiberInfo) == 128);

static bool FiberIsValid(const FiberObject* fiber) {
	return fiber != nullptr && fiber->magic_start == FIBER_MAGIC_START &&
	       fiber->magic_end == FIBER_MAGIC_END;
}

struct FiberThreadContext {
	FiberObject*    current_fiber;
	FiberObject*    returned_fiber;
	FiberObject*    pending_idle_fiber;
	FiberCpuContext cpu_context;
};

static const auto g_fiber_context_key = [] {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	const auto key = TlsAlloc();
	if (key == TLS_OUT_OF_INDEXES) {
		std::abort();
	}
#else
	pthread_key_t key;
	if (pthread_key_create(&key, nullptr) != 0) {
		std::abort();
	}
#endif
	return key;
}();

// Native lookup must stay fresh when a fiber resumes on another host thread.
static FiberThreadContext* FiberGetThreadContext() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return static_cast<FiberThreadContext*>(TlsGetValue(g_fiber_context_key));
#else
	return static_cast<FiberThreadContext*>(pthread_getspecific(g_fiber_context_key));
#endif
}

static void FiberSetThreadContext(FiberThreadContext* context) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (TlsSetValue(g_fiber_context_key, context) == 0) {
#else
	if (pthread_setspecific(g_fiber_context_key, context) != 0) {
#endif
		std::abort();
	}
}

static uint32_t FiberLoadState(const FiberObject* fiber) {
	auto& state = const_cast<uint32_t&>(fiber->state);
	return std::atomic_ref<uint32_t>(state).load(std::memory_order_acquire);
}

static void FiberStoreState(FiberObject* fiber, uint32_t state) {
	std::atomic_ref<uint32_t>(fiber->state).store(state, std::memory_order_release);
}

static void FiberCompleteSwitch(FiberObject* current) {
	auto* context = FiberGetThreadContext();
	context->current_fiber = current;
	// Only publish IDLE after switching away from the departing fiber's stack.
	auto* fiber = context->pending_idle_fiber;
	context->pending_idle_fiber = nullptr;
	if (fiber != nullptr) {
		FiberStoreState(fiber, FIBER_STATE_IDLE);
	}
}

static bool FiberCompareExchangeState(FiberObject* fiber, uint32_t expected, uint32_t desired) {
	return std::atomic_ref<uint32_t>(fiber->state)
	    .compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
	                             std::memory_order_acquire);
}

static void FiberSetContextValid(FiberObject* fiber, bool valid) {
	fiber->context_valid = valid;
	fiber->context       = valid ? &fiber->saved_context : nullptr;
}

#if defined(LUMEN_PS5_DIRECT_FEX)
#include "libs/directFiberRuntime.inc"
#else
#if defined(__x86_64__) || defined(_M_X64)
[[gnu::naked,
  gnu::returns_twice]] static KYTY_SYSV_ABI int FiberSaveContext(FiberCpuContext* /*ctx*/) {
	asm volatile("movq %rdi, %r10\n\t"
	             "movq %rbx, 0(%r10)\n\t"
	             "movq %rbp, 8(%r10)\n\t"
	             "movq %rdi, 16(%r10)\n\t"
	             "movq %rsi, 24(%r10)\n\t"
	             "movq %r12, 32(%r10)\n\t"
	             "movq %r13, 40(%r10)\n\t"
	             "movq %r14, 48(%r10)\n\t"
	             "movq %r15, 56(%r10)\n\t"
	             "leaq 8(%rsp), %r11\n\t"
	             "movq %r11, 64(%r10)\n\t"
	             "movq (%rsp), %r11\n\t"
	             "movq %r11, 72(%r10)\n\t"
	             "xorl %eax, %eax\n\t"
	             "retq\n");
}

[[gnu::naked, gnu::noreturn]] static KYTY_SYSV_ABI void
FiberRestoreContext(FiberCpuContext* /*ctx*/, uint64_t /*ret*/) {
	asm volatile("movq %rdi, %r10\n\t"
	             "movq %rsi, %rax\n\t"
	             "movq 72(%r10), %r11\n\t"
	             "movq 0(%r10), %rbx\n\t"
	             "movq 8(%r10), %rbp\n\t"
	             "movq 16(%r10), %rdi\n\t"
	             "movq 24(%r10), %rsi\n\t"
	             "movq 32(%r10), %r12\n\t"
	             "movq 40(%r10), %r13\n\t"
	             "movq 48(%r10), %r14\n\t"
	             "movq 56(%r10), %r15\n\t"
	             "movq 64(%r10), %rsp\n\t"
	             "jmp *%r11\n");
}
#else
static int FiberSaveContext(FiberCpuContext* ctx) {
	(void)ctx;
	return 0;
}

static void FiberRestoreContext(FiberCpuContext* ctx, uint64_t ret) {
	(void)ctx;
	(void)ret;
	EXIT("Fiber context switching is only implemented on x86_64\n");
}
#endif

[[noreturn]] static KYTY_SYSV_ABI void FiberStartTrampoline(FiberObject* fiber);

[[noreturn]] static KYTY_SYSV_ABI void FiberStartOnGuestStack(FiberObject* fiber) {
	FiberCpuContext ctx {};
	const auto      stack_top = reinterpret_cast<uintptr_t>(fiber->addr_context) +
	                            static_cast<uintptr_t>(fiber->size_context);
	auto            rsp       = (stack_top & ~static_cast<uintptr_t>(0x0f));
	rsp -= sizeof(uint64_t);
	*reinterpret_cast<uint64_t*>(rsp) = 0;

	ctx.rsp = rsp;
	ctx.rip = reinterpret_cast<uint64_t>(&FiberStartTrampoline);
	ctx.rdi = reinterpret_cast<uint64_t>(fiber);

	FiberRestoreContext(&ctx, 1);
}

[[noreturn]] static KYTY_SYSV_ABI void FiberStartTrampoline(FiberObject* fiber) {
	FiberCompleteSwitch(fiber);

	Loader::GuestCallback::Call(fiber->entry, fiber->arg_on_initialize, fiber->arg_on_run);

	FiberStoreState(fiber, FIBER_STATE_TERMINATED);
	FiberSetContextValid(fiber, false);
	fiber->arg_on_return  = 0;
	auto* context = FiberGetThreadContext();
	context->returned_fiber = fiber;
	context->current_fiber = nullptr;

	FiberRestoreContext(&context->cpu_context, 1);
}

#endif // LUMEN_PS5_DIRECT_FEX

int32_t KYTY_SYSV_ABI FiberInitialize(FiberObject* fiber, const char* name, FiberEntry entry,
                                      uint64_t arg_on_initialize, void* addr_context,
                                      uint64_t size_context, const FiberOptParam* opt_param,
                                      uint32_t build_version) {
	PRINT_NAME();

	if (fiber == nullptr || name == nullptr || entry == nullptr) {
		return FIBER_ERROR_NULL;
	}

	if ((reinterpret_cast<uint64_t>(fiber) & 7u) != 0 ||
	    (reinterpret_cast<uint64_t>(addr_context) & 15u) != 0 ||
	    (opt_param != nullptr && (reinterpret_cast<uint64_t>(opt_param) & 7u) != 0)) {
		return FIBER_ERROR_ALIGNMENT;
	}

	if (size_context != 0 && size_context < FIBER_CONTEXT_MIN_SIZE) {
		return FIBER_ERROR_RANGE;
	}

	if ((size_context & 15u) != 0 || (addr_context == nullptr && size_context != 0) ||
	    (addr_context != nullptr && size_context == 0) ||
	    (opt_param != nullptr && opt_param->magic != FIBER_OPT_MAGIC)) {
		return FIBER_ERROR_INVALID;
	}

#if defined(LUMEN_PS5_DIRECT_FEX)
	if (addr_context != nullptr &&
	    (reinterpret_cast<uint64_t>(addr_context) > UINT64_MAX - size_context ||
	     !DirectFiber::Writable(addr_context, size_context))) return FIBER_ERROR_INVALID;
	DirectFiber::Forget(fiber);
#endif
	std::memset(fiber, 0, sizeof(*fiber));
	std::strncpy(fiber->name, name, FIBER_MAX_NAME_LENGTH);
	fiber->name[FIBER_MAX_NAME_LENGTH] = '\0';

	fiber->magic_start       = FIBER_MAGIC_START;
	fiber->state             = FIBER_STATE_IDLE;
	fiber->entry             = entry;
	fiber->arg_on_initialize = arg_on_initialize;
	fiber->addr_context      = addr_context;
	fiber->size_context      = size_context;
	fiber->flags             = (build_version >= 0x03500000 ? FIBER_FLAG_SET_FPU_REGS : 0);
	fiber->context_start     = addr_context;
	fiber->context_end =
	    (addr_context != nullptr ? static_cast<uint8_t*>(addr_context) + size_context : nullptr);
	std::memset(&fiber->saved_context, 0, sizeof(fiber->saved_context));
	FiberSetContextValid(fiber, false);
	fiber->magic_end = FIBER_MAGIC_END;

	if (addr_context != nullptr) {
		*static_cast<uint64_t*>(addr_context) = FIBER_STACK_MAGIC;
	}

	LOGF("\t fiber init: %s, entry = 0x%016" PRIx64 ", context = 0x%016" PRIx64 ", size = %" PRIu64
	     "\n",
	     fiber->name, reinterpret_cast<uint64_t>(entry), reinterpret_cast<uint64_t>(addr_context),
	     size_context);

	return OK;
}

int32_t KYTY_SYSV_ABI FiberInitializeInternal(FiberObject* fiber, const char* name,
                                              FiberEntry entry, uint64_t arg_on_initialize,
                                              void* addr_context, uint64_t size_context,
                                              const FiberOptParam* opt_param, uint32_t flags,
                                              uint32_t build_version) {
	auto ret = FiberInitialize(fiber, name, entry, arg_on_initialize, addr_context, size_context,
	                           opt_param, build_version);
	if (ret == OK && fiber != nullptr) {
		fiber->flags |= flags;
	}
	return ret;
}

int32_t KYTY_SYSV_ABI FiberOptParamInitialize(FiberOptParam* opt_param) {
	PRINT_NAME();

	if (opt_param == nullptr) {
		return FIBER_ERROR_NULL;
	}
	if ((reinterpret_cast<uint64_t>(opt_param) & 7u) != 0) {
		return FIBER_ERROR_ALIGNMENT;
	}

	std::memset(opt_param, 0, 128);
	opt_param->magic = FIBER_OPT_MAGIC;

	return OK;
}

int32_t KYTY_SYSV_ABI FiberFinalize(FiberObject* fiber) {
	PRINT_NAME();

	if (fiber == nullptr) {
		return FIBER_ERROR_NULL;
	}
	if (!FiberIsValid(fiber)) {
		return FIBER_ERROR_INVALID;
	}
	if (!FiberCompareExchangeState(fiber, FIBER_STATE_IDLE, FIBER_STATE_TERMINATED)) {
		return FIBER_ERROR_STATE;
	}

#if defined(LUMEN_PS5_DIRECT_FEX)
	DirectFiber::Forget(fiber);
#endif
	return OK;
}

int32_t KYTY_SYSV_ABI FiberRun(FiberObject* fiber, uint64_t arg_on_run, uint64_t* arg_on_return) {
	PRINT_NAME();
#if defined(LUMEN_PS5_DIRECT_FEX)
	return DirectFiber::Run(fiber, arg_on_run, arg_on_return);
#else

	if (!FiberIsValid(fiber)) {
		return FIBER_ERROR_INVALID;
	}
	if (FiberGetThreadContext() != nullptr) {
		return FIBER_ERROR_PERMISSION;
	}
	if (!FiberCompareExchangeState(fiber, FIBER_STATE_IDLE, FIBER_STATE_RUNNING)) {
		return FIBER_ERROR_STATE;
	}

	fiber->arg_on_run    = arg_on_run;
	fiber->arg_on_return = 0;
	// Register on the host stack; switching fibers only reads the native TLS slot.
	FiberThreadContext context {};
	FiberSetThreadContext(&context);

	if (FiberSaveContext(&context.cpu_context) == 0) {
		if (fiber->context_valid) {
			FiberRestoreContext(&fiber->saved_context, 1);
		}
		FiberStartOnGuestStack(fiber);
	}

	auto* returned_fiber  = (context.returned_fiber != nullptr ? context.returned_fiber : fiber);
	const auto return_value = returned_fiber->arg_on_return;
	const auto return_code =
	    FiberLoadState(returned_fiber) == FIBER_STATE_TERMINATED ? FIBER_ERROR_STATE : OK;
	// Copy results before another thread can acquire and resume the fiber.
	FiberCompleteSwitch(nullptr);
	FiberSetThreadContext(nullptr);

	if (arg_on_return != nullptr) {
		*arg_on_return = return_value;
	}

	return return_code;
#endif
}

int32_t KYTY_SYSV_ABI FiberSwitch(FiberObject* fiber, uint64_t arg_on_run,
                                  uint64_t* arg_on_return) {
	PRINT_NAME();
#if defined(LUMEN_PS5_DIRECT_FEX)
	return DirectFiber::Switch(fiber, arg_on_run, arg_on_return);
#else

	if (!FiberIsValid(fiber)) {
		return FIBER_ERROR_INVALID;
	}
	auto* context = FiberGetThreadContext();
	if (context == nullptr) {
		return FIBER_ERROR_PERMISSION;
	}

	if (!FiberCompareExchangeState(fiber, FIBER_STATE_IDLE, FIBER_STATE_RUNNING)) {
		return FIBER_ERROR_STATE;
	}

	auto* caller = context->current_fiber;

	fiber->arg_on_run    = arg_on_run;
	fiber->arg_on_return = 0;

	if (FiberSaveContext(&caller->saved_context) == 0) {
		FiberSetContextValid(caller, true);
		context->pending_idle_fiber = caller;
		if (fiber->context_valid) {
			FiberRestoreContext(&fiber->saved_context, 1);
		}
		FiberStartOnGuestStack(fiber);
	}

	FiberCompleteSwitch(caller);

	if (arg_on_return != nullptr) {
		*arg_on_return = caller->arg_on_run;
	}

	return OK;
#endif
}

int32_t KYTY_SYSV_ABI FiberGetSelf(FiberObject** fiber) {
	PRINT_NAME();

	if (fiber == nullptr) {
		return FIBER_ERROR_NULL;
	}

	const auto* context = FiberGetThreadContext();
	*fiber = context != nullptr ? context->current_fiber : nullptr;

	return OK;
}

int32_t KYTY_SYSV_ABI FiberReturnToThread(uint64_t arg_on_return, uint64_t* arg_on_run) {
	PRINT_NAME();
#if defined(LUMEN_PS5_DIRECT_FEX)
	return DirectFiber::Return(arg_on_return, arg_on_run);
#else

	auto* context = FiberGetThreadContext();
	if (context == nullptr) {
		return FIBER_ERROR_PERMISSION;
	}

	auto* fiber          = context->current_fiber;
	fiber->arg_on_return = arg_on_return;

	if (FiberSaveContext(&fiber->saved_context) == 0) {
		FiberSetContextValid(fiber, true);
		context->returned_fiber = fiber;
		context->pending_idle_fiber = fiber;
		FiberRestoreContext(&context->cpu_context, 1);
	}

	FiberCompleteSwitch(fiber);
	if (arg_on_run != nullptr) {
		*arg_on_run = fiber->arg_on_run;
	}

	return OK;
#endif
}

int32_t KYTY_SYSV_ABI FiberGetInfo(FiberObject* fiber, FiberInfo* fiber_info) {
	PRINT_NAME();

	if (fiber == nullptr || fiber_info == nullptr) {
		return FIBER_ERROR_NULL;
	}
	if (!FiberIsValid(fiber)) {
		return FIBER_ERROR_INVALID;
	}

	std::memset(fiber_info, 0, sizeof(*fiber_info));
	fiber_info->size              = sizeof(*fiber_info);
	fiber_info->entry             = fiber->entry;
	fiber_info->arg_on_initialize = fiber->arg_on_initialize;
	fiber_info->addr_context      = fiber->addr_context;
	fiber_info->size_context      = fiber->size_context;
	std::strncpy(fiber_info->name, fiber->name, FIBER_MAX_NAME_LENGTH);

	return OK;
}

int32_t KYTY_SYSV_ABI FiberStartContextSizeCheck(uint32_t flags) {
	PRINT_NAME();
	LOGF("\t flags = 0x%08" PRIx32 "\n", flags);
	return OK;
}

int32_t KYTY_SYSV_ABI FiberStopContextSizeCheck() {
	PRINT_NAME();
	return OK;
}

int32_t KYTY_SYSV_ABI FiberRename(FiberObject* fiber, const char* name) {
	PRINT_NAME();

	if (fiber == nullptr || name == nullptr) {
		return FIBER_ERROR_NULL;
	}
	if (!FiberIsValid(fiber)) {
		return FIBER_ERROR_INVALID;
	}

	std::strncpy(fiber->name, name, FIBER_MAX_NAME_LENGTH);
	fiber->name[FIBER_MAX_NAME_LENGTH] = '\0';

	return OK;
}

int32_t KYTY_SYSV_ABI FiberGetThreadFramePointerAddress(uint64_t* addr_frame_pointer) {
	PRINT_NAME();

	if (addr_frame_pointer == nullptr) {
		return FIBER_ERROR_NULL;
	}

	*addr_frame_pointer = 0;

	return OK;
}

} // namespace Fiber
