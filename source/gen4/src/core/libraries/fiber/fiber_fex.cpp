// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "fiber.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "core/guest_cpu/hle_call_adapter.h"
#include "core/libraries/fiber/fiber_error.h"
#include "core/libraries/libs.h"
#include "core/tls.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <limits>

namespace Libraries::Fiber {
namespace {
using Core::GuestCpu::HleCallAdapter;
using Core::GuestCpu::HleCallFailure;
using Core::GuestCpu::HleCallFrame;
using Core::GuestCpu::HleCallResult;

constexpr u32 kFiberSignature0 = 0xdef1649c;
constexpr u32 kFiberSignature1 = 0xb37592a0;
constexpr u32 kFiberOptSignature = 0xbb40e64d;
constexpr u64 kFiberStackSignature = 0x7149f2ca7149f2ca;
constexpr u64 kFiberStackSizeCheck = 0xdeadbeefdeadbeef;
std::atomic<u32> context_size_check{};
u64 entry_return_address{};

struct ThreadFiberState {
    OrbisFiberContext context{};
    SavedFiberState saved{};
    bool active{};
    Core::Tcb* owner{};
};
static_assert(sizeof(OrbisFiberContext) == 0xa0);
static_assert(offsetof(ThreadFiberState, saved) == 0xa0);
static_assert(offsetof(ThreadFiberState, active) == 0x128);
static_assert(offsetof(ThreadFiberState, owner) == 0x130);
static_assert(offsetof(Core::Tcb, tcb_fiber) == 0x30);
static_assert(offsetof(HleCallFrame, callret_sp) == 0x190);
static_assert(offsetof(HleCallFrame, fcw) == 0x1a8);
static_assert(offsetof(HleCallFrame, validate_range) == 0x1c0);
static_assert(sizeof(HleCallFrame) == 0x1e8);
thread_local ThreadFiberState thread_fiber;

struct ContextRegion {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    u64 size{};
};
struct TargetPlan {
    std::uintptr_t stack{};
    bool resume{};
};
struct ControlResult {
    s32 error{};
    bool switched{};
};

s32 ValidateFiber(const OrbisFiber* fiber) {
    if (!fiber) return ORBIS_FIBER_ERROR_NULL;
    if (reinterpret_cast<std::uintptr_t>(fiber) & 7) return ORBIS_FIBER_ERROR_ALIGNMENT;
    if (fiber->magic_start != kFiberSignature0 || fiber->magic_end != kFiberSignature1)
        return ORBIS_FIBER_ERROR_INVALID;
    return ORBIS_OK;
}

s32 ValidateContext(void* address, u64 size, ContextRegion& region) {
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (begin & 15) return ORBIS_FIBER_ERROR_ALIGNMENT;
    if (size && size < ORBIS_FIBER_CONTEXT_MINIMUM_SIZE) return ORBIS_FIBER_ERROR_RANGE;
    if ((size & 15) || !begin || begin >= u64{0} - size)
        return ORBIS_FIBER_ERROR_INVALID;
    region = {begin, begin + size, size};
    return ORBIS_OK;
}

bool ValidateWritable(HleCallFrame& frame, std::uintptr_t address, std::size_t size) {
    return size != 0 && size < std::uintptr_t{0} - address && frame.validate_range &&
           frame.validate_range(frame.validate_context, address, size, true);
}

void ApplyContext(OrbisFiber& fiber, const ContextRegion& region, bool fill_check) {
    fiber.addr_context = reinterpret_cast<void*>(region.begin);
    fiber.size_context = region.size;
    fiber.context_start = reinterpret_cast<void*>(region.begin);
    fiber.context_end = reinterpret_cast<void*>(region.end);
    auto* begin = reinterpret_cast<u64*>(region.begin);
    *begin = kFiberStackSignature;
    if (fill_check) std::fill(begin + 1, reinterpret_cast<u64*>(region.end), kFiberStackSizeCheck);
}

void Save(SavedFiberState& saved, const HleCallFrame& frame, u64* output) {
    std::copy_n(frame.gpr.begin(), 6, saved.gpr.begin());
    std::copy_n(frame.gpr.begin() + 8, 8, saved.gpr.begin() + 6);
    saved.output = output;
    saved.callret_sp = frame.callret_sp;
    saved.mxcsr = frame.mxcsr;
    saved.fcw = frame.fcw;
    saved.valid = true;
}

void Restore(HleCallFrame& frame, const SavedFiberState& saved, u64 argument) {
    if (saved.output) *saved.output = argument;
    std::copy_n(saved.gpr.begin(), 6, frame.gpr.begin());
    std::copy_n(saved.gpr.begin() + 6, 8, frame.gpr.begin() + 8);
    frame.gpr[0] = 0;
    frame.rsp = frame.gpr[4];
    if (frame.callret_stack_base != 0 &&
        frame.callret_stack_base < std::uintptr_t{0} - frame.callret_stack_size) {
        const auto end = frame.callret_stack_base + frame.callret_stack_size;
        frame.callret_sp = saved.callret_sp >= frame.callret_stack_base && saved.callret_sp <= end
                               ? saved.callret_sp
                               : frame.callret_stack_base + frame.callret_stack_size / 4;
    }
    frame.fcw = saved.fcw;
    frame.mxcsr = (saved.mxcsr & ~0x3fu) | (frame.mxcsr & 0x3fu);
}

void Reset(HleCallFrame& frame, OrbisFiber& fiber, u64 argument, std::uintptr_t stack) {
    auto* slots = reinterpret_cast<u64*>(stack);
    slots[0] = reinterpret_cast<u64>(fiber.entry);
    slots[1] = entry_return_address;
    frame.gpr.fill(0);
    frame.gpr[4] = stack;
    frame.gpr[6] = argument;
    frame.gpr[7] = fiber.arg_on_initialize;
    frame.gpr[11] = reinterpret_cast<u64>(fiber.entry);
    frame.rsp = stack;
    frame.xmm = {};
    if (frame.reset_simd) frame.reset_simd(frame.reset_simd_context);
    if (fiber.flags & FiberFlags::SetFpuRegs) {
        frame.fcw = 0x37f;
        frame.mxcsr = 0x9fc0;
    }
}

s32 PrepareTarget(HleCallFrame& frame, OrbisFiber& fiber, const ContextRegion& region,
                  bool attach, std::uintptr_t fallback_top, TargetPlan& plan) {
    if (fiber.saved.valid) {
        if (fiber.saved.output &&
            (!frame.validate_range ||
             !frame.validate_range(frame.validate_context,
                                   reinterpret_cast<std::uintptr_t>(fiber.saved.output), 8, true)))
            return ORBIS_FIBER_ERROR_INVALID;
        plan.resume = true;
        return ORBIS_OK;
    }
    auto top = fallback_top;
    if (attach) {
        top = region.end;
    } else if (fiber.addr_context) {
        ContextRegion existing;
        if (const auto error = ValidateContext(fiber.addr_context, fiber.size_context, existing))
            return error;
        top = existing.end;
    }
    if (top < 16) return ORBIS_FIBER_ERROR_INVALID;
    const auto aligned = top & ~std::uintptr_t{15};
    plan.stack = aligned - 16;
    if (!aligned || !frame.validate_range ||
        !frame.validate_range(frame.validate_context, plan.stack, 16, true))
        return ORBIS_FIBER_ERROR_INVALID;
    return ORBIS_OK;
}

void CommitTarget(HleCallFrame& frame, OrbisFiber& fiber, const ContextRegion& region,
                  bool attach, const TargetPlan& plan, u64 argument) {
    if (attach) ApplyContext(fiber, region, fiber.flags & FiberFlags::ContextSizeCheck);
    if (plan.resume) Restore(frame, fiber.saved, argument);
    else Reset(frame, fiber, argument, plan.stack);
}

s32 CheckCurrentStack(HleCallFrame& frame, const OrbisFiber& fiber) {
    if (!fiber.addr_context) return ORBIS_OK;
    if (!frame.validate_range ||
        !frame.validate_range(frame.validate_context,
                              reinterpret_cast<std::uintptr_t>(fiber.addr_context), 8, true))
        return ORBIS_FIBER_ERROR_INVALID;
    if (*static_cast<const u64*>(fiber.addr_context) != kFiberStackSignature)
        std::abort();
    return ORBIS_OK;
}

ControlResult RunFiber(HleCallFrame& frame, OrbisFiber& fiber, void* address, u64 size,
                       u64 argument, u64* output) {
    ContextRegion region;
    const bool attach = address || size;
    if (attach) {
        if (const auto error = ValidateContext(address, size, region)) return {error};
        if (!frame.validate_range ||
            !frame.validate_range(frame.validate_context, region.begin, region.size, true))
            return {ORBIS_FIBER_ERROR_INVALID};
    }
    if (!frame.rsp || frame.rsp != frame.gpr[4]) return {ORBIS_FIBER_ERROR_INVALID};
    if (thread_fiber.active) return {ORBIS_FIBER_ERROR_PERMISSION};
    FiberState expected = FiberState::Idle;
    if (!fiber.state.compare_exchange_strong(expected, FiberState::Run, std::memory_order_acq_rel))
        return {ORBIS_FIBER_ERROR_STATE};
    TargetPlan plan;
    const auto error = attach && fiber.addr_context
                           ? ORBIS_FIBER_ERROR_INVALID
                           : PrepareTarget(frame, fiber, region, attach, frame.rsp, plan);
    if (error) {
        fiber.state.store(FiberState::Idle, std::memory_order_release);
        return {error};
    }
    Save(thread_fiber.saved, frame, output);
    std::memset(&thread_fiber.context, 0, sizeof(thread_fiber.context));
    thread_fiber.context.rbp = thread_fiber.saved.gpr[5];
    thread_fiber.context.current_fiber = &fiber;
    thread_fiber.active = true;
    CommitTarget(frame, fiber, region, attach, plan, argument);
    return {0, true};
}

ControlResult SwitchFiber(HleCallFrame& frame, OrbisFiber& fiber, void* address, u64 size,
                          u64 argument, u64* output) {
    auto* current = thread_fiber.context.current_fiber;
    if (!thread_fiber.active || !current) return {ORBIS_FIBER_ERROR_PERMISSION};
    if (current == &fiber) return {ORBIS_FIBER_ERROR_STATE};
    ContextRegion region;
    const bool attach = address || size;
    if (attach) {
        if (const auto error = ValidateContext(address, size, region)) return {error};
        if (!frame.validate_range ||
            !frame.validate_range(frame.validate_context, region.begin, region.size, true))
            return {ORBIS_FIBER_ERROR_INVALID};
    }
    if (!frame.rsp || frame.rsp != frame.gpr[4]) return {ORBIS_FIBER_ERROR_INVALID};
    if (const auto error = CheckCurrentStack(frame, *current)) return {error};
    FiberState expected = FiberState::Idle;
    if (!fiber.state.compare_exchange_strong(expected, FiberState::Run, std::memory_order_acq_rel))
        return {ORBIS_FIBER_ERROR_STATE};
    TargetPlan plan;
    const auto error = attach && fiber.addr_context
                           ? ORBIS_FIBER_ERROR_INVALID
                           : PrepareTarget(frame, fiber, region, attach,
                                           thread_fiber.saved.gpr[4], plan);
    if (error) {
        fiber.state.store(FiberState::Idle, std::memory_order_release);
        return {error};
    }
    if (current->addr_context) Save(current->saved, frame, output);
    else std::memset(&current->saved, 0, sizeof(current->saved));
    CommitTarget(frame, fiber, region, attach, plan, argument);
    current->state.store(FiberState::Idle, std::memory_order_release);
    thread_fiber.context.current_fiber = &fiber;
    return {0, true};
}

ControlResult ReturnToThread(HleCallFrame& frame, u64 argument, u64* output) {
    auto* current = thread_fiber.context.current_fiber;
    if (!thread_fiber.active || !current) return {ORBIS_FIBER_ERROR_PERMISSION};
    if (!thread_fiber.saved.valid) return {ORBIS_FIBER_ERROR_INVALID};
    if (output && (!frame.validate_range ||
                   !frame.validate_range(frame.validate_context,
                                         reinterpret_cast<std::uintptr_t>(output), 8, true)))
        return {ORBIS_FIBER_ERROR_INVALID};
    if (thread_fiber.saved.output &&
        (!frame.validate_range ||
         !frame.validate_range(frame.validate_context,
                               reinterpret_cast<std::uintptr_t>(thread_fiber.saved.output), 8, true)))
        return {ORBIS_FIBER_ERROR_INVALID};
    if (!frame.rsp || frame.rsp != frame.gpr[4]) return {ORBIS_FIBER_ERROR_INVALID};
    if (const auto error = CheckCurrentStack(frame, *current)) return {error};
    if (current->addr_context) Save(current->saved, frame, output);
    else std::memset(&current->saved, 0, sizeof(current->saved));
    Restore(frame, thread_fiber.saved, argument);
    current->state.store(FiberState::Idle, std::memory_order_release);
    std::memset(&thread_fiber.context, 0, sizeof(thread_fiber.context));
    std::memset(&thread_fiber.saved, 0, sizeof(thread_fiber.saved));
    thread_fiber.active = false;
    return {0, true};
}

enum class FiberControl : u8 { Run, RunWithContext, Switch, SwitchWithContext, Return };

class FiberControlAdapter final : public HleCallAdapter {
public:
    explicit FiberControlAdapter(FiberControl control_) : control{control_} {}

    HleCallResult Invoke(HleCallFrame& frame) const override {
        auto complete = [&](s32 error) -> HleCallResult {
            frame.gpr[0] = static_cast<u64>(static_cast<s64>(error));
            return true;
        };
        OrbisFiber* fiber{};
        void* address{};
        u64 size{};
        u64 argument{};
        u64* output{};
        const bool attach = control == FiberControl::RunWithContext ||
                            control == FiberControl::SwitchWithContext;
        const bool run = control == FiberControl::Run || control == FiberControl::RunWithContext;
        if (control == FiberControl::Return) {
            argument = frame.gpr[7];
            output = reinterpret_cast<u64*>(frame.gpr[6]);
        } else {
            fiber = reinterpret_cast<OrbisFiber*>(frame.gpr[7]);
            if (!fiber) return complete(ORBIS_FIBER_ERROR_NULL);
            if (attach) {
                address = reinterpret_cast<void*>(frame.gpr[6]);
                size = frame.gpr[2];
                argument = frame.gpr[1];
                output = reinterpret_cast<u64*>(frame.gpr[8]);
            } else {
                argument = frame.gpr[6];
                output = reinterpret_cast<u64*>(frame.gpr[2]);
            }
            if ((reinterpret_cast<std::uintptr_t>(fiber) & 7) ||
                (attach && (reinterpret_cast<std::uintptr_t>(address) & 15)))
                return complete(ORBIS_FIBER_ERROR_ALIGNMENT);
            if (!ValidateWritable(frame, reinterpret_cast<std::uintptr_t>(fiber), sizeof(*fiber)))
                return HleCallFailure{EFAULT, Name()};
            if (const auto error = ValidateFiber(fiber)) return complete(error);
            if (attach) {
                ContextRegion region;
                if (const auto error = ValidateContext(address, size, region)) return complete(error);
                if (!ValidateWritable(frame, region.begin, region.size))
                    return HleCallFailure{EFAULT, Name()};
            }
        }
        if (output && !ValidateWritable(frame, reinterpret_cast<std::uintptr_t>(output), 8))
            return HleCallFailure{EFAULT, Name()};
        ControlResult result;
        if (run) {
            Core::Tcb* owner{};
            if (!thread_fiber.active) {
                owner = Core::GetTcbBase();
                if (!owner || owner->tcb_fiber) return complete(ORBIS_FIBER_ERROR_PERMISSION);
            }
            result = RunFiber(frame, *fiber, address, size, argument, output);
            if (result.switched) {
                thread_fiber.owner = owner;
                owner->tcb_fiber = &thread_fiber.context;
            }
        } else if (control == FiberControl::Return) {
            if (thread_fiber.active && !thread_fiber.owner)
                return complete(ORBIS_FIBER_ERROR_PERMISSION);
            result = ReturnToThread(frame, argument, output);
            if (result.switched) {
                auto* owner = thread_fiber.owner;
                thread_fiber.owner = nullptr;
                owner->tcb_fiber = nullptr;
            }
        } else {
            result = SwitchFiber(frame, *fiber, address, size, argument, output);
        }
        if (!result.switched) return complete(result.error);
        return true;
    }

private:
    FiberControl control;
};

class FiberEntryReturnAdapter final : public HleCallAdapter {
public:
    bool ReturnsNormally() const override { return false; }
    HleCallResult Invoke(HleCallFrame&) const override { return HleCallFailure{EPROTO, Name()}; }
};

void RegisterControl(Core::Loader::SymbolsResolver* sym, const char* nid, FiberControl control) {
    Core::Loader::SymbolResolver symbol{};
    symbol.name = nid;
    symbol.library = "libSceFiber";
    symbol.library_version = 1;
    symbol.module = "libSceFiber";
    symbol.type = Core::Loader::SymbolType::Function;
    sym->AddFunction(symbol, std::make_shared<FiberControlAdapter>(control));
}
} // namespace

std::shared_ptr<Core::GuestCpu::HleCallAdapter> MakeFiberEntryReturnAdapter() {
    return std::make_shared<FiberEntryReturnAdapter>();
}

void SetFiberEntryReturnAddress(u64 address) { entry_return_address = address; }

OrbisFiberContext* GetFiberContext() { return Core::GetTcbBase()->tcb_fiber; }

s32 PS4_SYSV_ABI sceFiberInitializeImpl(OrbisFiber* fiber, const char* name, OrbisFiberEntry entry,
                                      u64 arg_on_initialize, void* addr_context, u64 size_context,
                                      const OrbisFiberOptParam* opt_param, u32 flags, u32 build_ver) {
    if (!fiber || !name || !entry) return ORBIS_FIBER_ERROR_NULL;
    if ((reinterpret_cast<u64>(fiber) & 7) || (reinterpret_cast<u64>(addr_context) & 15) ||
        (reinterpret_cast<u64>(opt_param) & 7)) return ORBIS_FIBER_ERROR_ALIGNMENT;
    ContextRegion region;
    const bool has_context = addr_context || size_context;
    if (has_context) {
        if (const auto error = ValidateContext(addr_context, size_context, region)) return error;
    }
    if (opt_param && opt_param->magic != kFiberOptSignature) return ORBIS_FIBER_ERROR_INVALID;
    u32 user_flags = flags;
    if ((build_ver >> 20) > 0x34) user_flags |= FiberFlags::SetFpuRegs;
    if (context_size_check.load(std::memory_order_acquire)) user_flags |= FiberFlags::ContextSizeCheck;
    if (size_context && size_context <= 4096)
        LOG_WARNING(Lib_Fiber, "Fiber initialized with small stack area.");
    std::strncpy(fiber->name, name, ORBIS_FIBER_MAX_NAME_LENGTH);
    fiber->entry = entry;
    fiber->arg_on_initialize = arg_on_initialize;
    fiber->context = nullptr;
    fiber->addr_context = nullptr;
    fiber->size_context = 0;
    fiber->flags = user_flags;
    std::memset(&fiber->saved, 0, sizeof(fiber->saved));
    if (has_context) ApplyContext(*fiber, region, flags & FiberFlags::ContextSizeCheck);
    fiber->magic_start = kFiberSignature0;
    fiber->magic_end = kFiberSignature1;
    fiber->state.store(FiberState::Idle, std::memory_order_release);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFiberOptParamInitialize(OrbisFiberOptParam* opt_param) {
    if (!opt_param) {
        return ORBIS_FIBER_ERROR_NULL;
    }
    if ((u64)opt_param & 7) {
        return ORBIS_FIBER_ERROR_ALIGNMENT;
    }

    opt_param->magic = kFiberOptSignature;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFiberFinalize(OrbisFiber* fiber) {
    if (!fiber) {
        return ORBIS_FIBER_ERROR_NULL;
    }
    if ((u64)fiber & 7) {
        return ORBIS_FIBER_ERROR_ALIGNMENT;
    }
    if (fiber->magic_start != kFiberSignature0 || fiber->magic_end != kFiberSignature1) {
        return ORBIS_FIBER_ERROR_INVALID;
    }

    FiberState expected = FiberState::Idle;
    if (!fiber->state.compare_exchange_strong(expected, FiberState::Terminated)) {
        return ORBIS_FIBER_ERROR_STATE;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFiberGetSelf(OrbisFiber** fiber) {
    if (!fiber) {
        return ORBIS_FIBER_ERROR_NULL;
    }

    OrbisFiberContext* g_ctx = GetFiberContext();
    if (!g_ctx) {
        return ORBIS_FIBER_ERROR_PERMISSION;
    }

    *fiber = g_ctx->current_fiber;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFiberGetInfo(OrbisFiber* fiber, OrbisFiberInfo* fiber_info) {
    if (!fiber || !fiber_info) {
        return ORBIS_FIBER_ERROR_NULL;
    }
    if ((u64)fiber & 7 || (u64)fiber_info & 7) {
        return ORBIS_FIBER_ERROR_ALIGNMENT;
    }
    if (fiber_info->size != sizeof(OrbisFiberInfo)) {
        return ORBIS_FIBER_ERROR_INVALID;
    }
    if (fiber->magic_start != kFiberSignature0 || fiber->magic_end != kFiberSignature1) {
        return ORBIS_FIBER_ERROR_INVALID;
    }

    fiber_info->entry = fiber->entry;
    fiber_info->arg_on_initialize = fiber->arg_on_initialize;
    fiber_info->addr_context = fiber->addr_context;
    fiber_info->size_context = fiber->size_context;
    strncpy(fiber_info->name, fiber->name, ORBIS_FIBER_MAX_NAME_LENGTH);

    fiber_info->size_context_margin = -1;
    if (fiber->flags & FiberFlags::ContextSizeCheck && fiber->addr_context != nullptr) {
        u64 stack_margin = 0;
        u64* stack_start = reinterpret_cast<u64*>(fiber->context_start);
        u64* stack_end = reinterpret_cast<u64*>(fiber->context_end);

        if (*stack_start == kFiberStackSignature) {
            u64* stack_ptr = stack_start + 1;
            while (stack_ptr < stack_end) {
                if (*stack_ptr == kFiberStackSizeCheck) {
                    stack_ptr++;
                }
            }

            stack_margin =
                reinterpret_cast<u64>(stack_ptr) - reinterpret_cast<u64>(stack_start + 1);
        }

        fiber_info->size_context_margin = stack_margin;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFiberStartContextSizeCheck(u32 flags) {
    if (flags != 0) {
        return ORBIS_FIBER_ERROR_INVALID;
    }

    u32 expected = 0;
    if (!context_size_check.compare_exchange_strong(expected, 1u)) {
        return ORBIS_FIBER_ERROR_STATE;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFiberStopContextSizeCheck() {
    u32 expected = 1;
    if (!context_size_check.compare_exchange_strong(expected, 0u)) {
        return ORBIS_FIBER_ERROR_STATE;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFiberRename(OrbisFiber* fiber, const char* name) {
    if (!fiber || !name) {
        return ORBIS_FIBER_ERROR_NULL;
    }
    if ((u64)fiber & 7) {
        return ORBIS_FIBER_ERROR_ALIGNMENT;
    }
    if (fiber->magic_start != kFiberSignature0 || fiber->magic_end != kFiberSignature1) {
        return ORBIS_FIBER_ERROR_INVALID;
    }

    strncpy(fiber->name, name, ORBIS_FIBER_MAX_NAME_LENGTH);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFiberGetThreadFramePointerAddress(u64* addr_frame_pointer) {
    if (!addr_frame_pointer) {
        return ORBIS_FIBER_ERROR_NULL;
    }

    OrbisFiberContext* g_ctx = GetFiberContext();
    if (!g_ctx) {
        return ORBIS_FIBER_ERROR_PERMISSION;
    }

    *addr_frame_pointer = g_ctx->rbp;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceFiberInitialize(OrbisFiber* fiber, const char* name, OrbisFiberEntry entry,
                                    u64 arg_on_initialize, void* addr_context, u64 size_context,
                                    const OrbisFiberOptParam* opt_param, u32 build_ver) {
    return sceFiberInitializeImpl(fiber, name, entry, arg_on_initialize, addr_context, size_context,
                                  opt_param, 0, build_ver);
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("hVYD7Ou2pCQ", "libSceFiber", 1, "libSceFiber", sceFiberInitialize);
    LIB_FUNCTION("7+OJIpko9RY", "libSceFiber", 1, "libSceFiber",
                 sceFiberInitializeImpl); // _sceFiberInitializeWithInternalOptionImpl
    LIB_FUNCTION("asjUJJ+aa8s", "libSceFiber", 1, "libSceFiber", sceFiberOptParamInitialize);
    LIB_FUNCTION("JeNX5F-NzQU", "libSceFiber", 1, "libSceFiber", sceFiberFinalize);

    RegisterControl(sym, "a0LLrZWac0M", FiberControl::Run);
    RegisterControl(sym, "PFT2S-tJ7Uk", FiberControl::Switch);
    LIB_FUNCTION("p+zLIOg27zU", "libSceFiber", 1, "libSceFiber", sceFiberGetSelf);
    RegisterControl(sym, "B0ZX2hx9DMw", FiberControl::Return);

    RegisterControl(sym, "avfGJ94g36Q", FiberControl::RunWithContext);
    RegisterControl(sym, "ZqhZFuzKT6U", FiberControl::SwitchWithContext);

    LIB_FUNCTION("uq2Y5BFz0PE", "libSceFiber", 1, "libSceFiber", sceFiberGetInfo);
    LIB_FUNCTION("Lcqty+QNWFc", "libSceFiber", 1, "libSceFiber", sceFiberStartContextSizeCheck);
    LIB_FUNCTION("Kj4nXMpnM8Y", "libSceFiber", 1, "libSceFiber", sceFiberStopContextSizeCheck);
    LIB_FUNCTION("JzyT91ucGDc", "libSceFiber", 1, "libSceFiber", sceFiberRename);

    LIB_FUNCTION("0dy4JtMUcMQ", "libSceFiber", 1, "libSceFiber",
                 sceFiberGetThreadFramePointerAddress);
}

} // namespace Libraries::Fiber
