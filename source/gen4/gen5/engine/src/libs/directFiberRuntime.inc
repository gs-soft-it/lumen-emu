// Included inside Libs::Fiber. A control call returns through the
// ordinary typed HLE bridge; the bridge applies the pending guest CPU context.
// No native C++ continuation or stack address is retained across a switch.
namespace DirectFiber {
namespace Runtime = Lumen::Ps5::DirectFex;

struct Saved {
	Runtime::HleContext cpu {};
	uint64_t* output {};
};

struct ThreadState {
	FiberThreadContext context {};
	Saved parent {};
};

static std::mutex mutex;
static std::unordered_map<FiberObject*, Saved> saved;
static thread_local ThreadState thread;

static bool Writable(const void* address, uint64_t size) {
	return Libs::LibKernel::Memory::QueryGuestAccess(reinterpret_cast<uint64_t>(address), size,
	                                Common::VirtualMemory::Mode::Write);
}

static bool OutputValid(const uint64_t* output) {
	return output == nullptr || Writable(output, sizeof(*output));
}

static void Forget(FiberObject* fiber) {
	std::lock_guard lock(mutex);
	saved.erase(fiber);
}

static void Save(FiberObject* fiber, const Saved& state) {
	// A stackless fiber borrows the Run caller's guest stack. Once that caller
	// resumes, its frames can overwrite the borrowed stack, so reentry starts
	// the fiber again as in the direct Gen4 context adapter.
	if (fiber->addr_context == nullptr) {
		saved.erase(fiber);
		FiberSetContextValid(fiber, false);
		return;
	}
	saved.insert_or_assign(fiber, state);
	const auto& gpr = state.cpu.gprs;
	fiber->saved_context = {gpr[3], gpr[5], gpr[7], gpr[6], gpr[12], gpr[13],
	                        gpr[14], gpr[15], gpr[4], state.cpu.rip};
	FiberSetContextValid(fiber, true);
}

static uint64_t EntryReturnAddress();

// Called with the target owned in RUNNING state and the saved-state lock held.
// Validate all writes before publishing a new context or another IDLE fiber.
static bool Prepare(FiberObject* fiber, const Runtime::HleContext& source,
	                uint64_t argument, uint64_t fallback_top, Saved& target) {
	if (fiber->context_valid) {
		const auto found = saved.find(fiber);
		if (found == saved.end() || !OutputValid(found->second.output)) return false;
		target = found->second;
		target.cpu.gprs[0] = OK;
		return true;
	}
	const auto base = reinterpret_cast<uint64_t>(fiber->addr_context);
	if (base != 0 && (fiber->size_context < FIBER_CONTEXT_MIN_SIZE ||
	                 base > UINT64_MAX - fiber->size_context)) return false;
	const auto top = base != 0 ? base + fiber->size_context : fallback_top;
	const auto aligned = top & ~uint64_t{15};
	if (aligned < 24 || !Writable(reinterpret_cast<void*>(aligned - 24), 24)) return false;
	const auto return_address = EntryReturnAddress();
	if (return_address == 0) return false;
	target = {};
	target.cpu.reset_simd = true;
	target.cpu.rip = reinterpret_cast<uint64_t>(fiber->entry);
	target.cpu.gprs[4] = aligned - 24;
	target.cpu.gprs[7] = fiber->arg_on_initialize;
	target.cpu.gprs[6] = argument;
	target.cpu.callret_sp = source.callret_sp;
	target.cpu.fcw = (fiber->flags & FIBER_FLAG_SET_FPU_REGS) ? 0x37f : source.fcw;
	target.cpu.mxcsr = (fiber->flags & FIBER_FLAG_SET_FPU_REGS) ? 0x9fc0 : source.mxcsr;
	// Entry sees the normal SysV RSP % 16 == 8. After its RET the veneer has
	// another valid return slot, required by the typed HLE dispatcher even
	// though completion transfers directly back to the suspended Run call.
	auto* slots = reinterpret_cast<uint64_t*>(target.cpu.gprs[4]);
	slots[0] = return_address;
	slots[1] = Runtime::RootReturn();
	slots[2] = 0;
	return true;
}

static int32_t Run(FiberObject* fiber, uint64_t argument, uint64_t* output) {
	if (!FiberIsValid(fiber)) return FIBER_ERROR_INVALID;
	if (FiberGetThreadContext() != nullptr) return FIBER_ERROR_PERMISSION;
	Saved parent {};
	parent.output = output;
	if (!OutputValid(output) || !Runtime::CaptureHleContext(&parent.cpu)) return FIBER_ERROR_INVALID;
	if (!FiberCompareExchangeState(fiber, FIBER_STATE_IDLE, FIBER_STATE_RUNNING))
		return FIBER_ERROR_STATE;
	std::lock_guard lock(mutex);
	Saved target {};
	if (!Prepare(fiber, parent.cpu, argument, parent.cpu.gprs[4], target) ||
	    !Runtime::SwitchHleContext(target.cpu)) {
		FiberStoreState(fiber, FIBER_STATE_IDLE);
		return FIBER_ERROR_INVALID;
	}
	thread = {};
	thread.parent = parent;
	thread.context.current_fiber = fiber;
	FiberSetThreadContext(&thread.context);
	fiber->arg_on_run = argument;
	fiber->arg_on_return = 0;
	if (target.output != nullptr) *target.output = argument;
	return OK;
}

static int32_t Switch(FiberObject* fiber, uint64_t argument, uint64_t* output) {
	if (!FiberIsValid(fiber)) return FIBER_ERROR_INVALID;
	auto* context = FiberGetThreadContext();
	if (context == nullptr || context->current_fiber == nullptr) return FIBER_ERROR_PERMISSION;
	Saved caller {};
	caller.output = output;
	if (!OutputValid(output) || !Runtime::CaptureHleContext(&caller.cpu)) return FIBER_ERROR_INVALID;
	if (!FiberCompareExchangeState(fiber, FIBER_STATE_IDLE, FIBER_STATE_RUNNING))
		return FIBER_ERROR_STATE;
	std::lock_guard lock(mutex);
	Saved target {};
	if (!Prepare(fiber, caller.cpu, argument, thread.parent.cpu.gprs[4], target)) {
		FiberStoreState(fiber, FIBER_STATE_IDLE);
		return FIBER_ERROR_INVALID;
	}
	auto* departing = context->current_fiber;
	try { Save(departing, caller); }
	catch (...) {
		FiberStoreState(fiber, FIBER_STATE_IDLE);
		return FIBER_ERROR_STATE;
	}
	if (!Runtime::SwitchHleContext(target.cpu)) {
		FiberStoreState(fiber, FIBER_STATE_IDLE);
		return FIBER_ERROR_INVALID;
	}
	fiber->arg_on_run = argument;
	fiber->arg_on_return = 0;
	if (target.output != nullptr) *target.output = argument;
	context->current_fiber = fiber;
	// The guest stack is already inactive: this service executes on the host
	// stack. Publish IDLE only after saving every guest register and output.
	FiberStoreState(departing, FIBER_STATE_IDLE);
	return OK;
}

static int32_t Return(uint64_t argument, uint64_t* output) {
	auto* context = FiberGetThreadContext();
	if (context == nullptr || context->current_fiber == nullptr) return FIBER_ERROR_PERMISSION;
	Saved caller {};
	caller.output = output;
	if (!OutputValid(output) || !OutputValid(thread.parent.output) ||
	    !Runtime::CaptureHleContext(&caller.cpu)) return FIBER_ERROR_INVALID;
	std::lock_guard lock(mutex);
	auto* departing = context->current_fiber;
	try { Save(departing, caller); }
	catch (...) { return FIBER_ERROR_STATE; }
	if (!Runtime::SwitchHleContext(thread.parent.cpu)) return FIBER_ERROR_INVALID;
	if (thread.parent.output != nullptr) *thread.parent.output = argument;
	departing->arg_on_return = argument;
	thread = {};
	FiberSetThreadContext(nullptr);
	FiberStoreState(departing, FIBER_STATE_IDLE);
	return OK;
}

static int32_t KYTY_SYSV_ABI EntryReturned() {
	auto* context = FiberGetThreadContext();
	if (context == nullptr || context->current_fiber == nullptr) return FIBER_ERROR_PERMISSION;
	if (!OutputValid(thread.parent.output)) return FIBER_ERROR_INVALID;
	std::lock_guard lock(mutex);
	auto parent = thread.parent.cpu;
	parent.gprs[0] = static_cast<uint64_t>(static_cast<int64_t>(FIBER_ERROR_STATE));
	if (!Runtime::SwitchHleContext(parent)) return FIBER_ERROR_INVALID;
	auto* departing = context->current_fiber;
	if (thread.parent.output != nullptr) *thread.parent.output = 0;
	departing->arg_on_return = 0;
	FiberSetContextValid(departing, false);
	saved.erase(departing);
	thread = {};
	FiberSetThreadContext(nullptr);
	FiberStoreState(departing, FIBER_STATE_TERMINATED);
	return FIBER_ERROR_STATE;
}

static uint64_t EntryReturnAddress() {
	static const uint64_t address = [] {
		if (!Loader::RegisterHostFunction<EntryReturned>("FiberEntryReturned")) return uint64_t{0};
		return Runtime::ResolveHostTarget(reinterpret_cast<uint64_t>(&EntryReturned));
	}();
	return address;
}
} // namespace DirectFiber
