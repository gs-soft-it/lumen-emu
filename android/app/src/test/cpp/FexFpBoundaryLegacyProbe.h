#pragma once

#include "AndroidFexCoreRuntime.h"
#include "FexFpBoundaryProbePrograms.h"

#include <cfenv>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace Lumen::FexCore::FpBoundaryLegacyProbe {

// These are the existing V1/V2 ABI register slots (x86 architectural order),
// independent of any game. This test helper can inspect an APK built before
// the explicit runtime diagnostic export existed.
inline constexpr unsigned Rbx = 3, Rsp = 4, Rsi = 6, Rdi = 7;

struct Api {
    decltype(&LumenAndroidFexCoreRegisterHostCallTargetV1) registerTarget;
    decltype(&LumenAndroidFexCoreRootReturnAddressV1) rootReturn;
    decltype(&LumenAndroidFexCoreRunGuestV2) run;
    decltype(&LumenAndroidFexCoreBeginGuestMemoryMutationV1) beginMutation;
    decltype(&LumenAndroidFexCoreEndGuestMemoryMutationV2) endMutation;
};

struct Context {
    const Api* api;
    std::uint64_t code, data, page, sentinel, target;
    bool nested {};
    unsigned calls {};
    int entryRounding {};
    std::uint32_t nestedCode {LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1};
};

inline void TargetIdentity() {}

inline std::uint32_t Query(void* opaque, std::uint64_t address,
    std::uint64_t* base, std::uint64_t* size, std::uint32_t* writable) noexcept {
    const auto& context = *static_cast<const Context*>(opaque);
    if (address < context.code || address - context.code >= context.page) return 0;
    *base = context.code; *size = context.page; *writable = 0;
    return 1;
}

inline std::uint32_t HostCall(void*, std::uint64_t, std::uint64_t*,
    LumenAndroidFexCoreSimdViewV2*, std::uint64_t*) noexcept;

inline std::uint32_t Run(Context& context, bool acrossHost, bool nested) noexcept {
    const auto stack = context.data + context.page * (nested ? 2 : 1) - 8;
    std::memcpy(reinterpret_cast<void*>(stack), &context.sentinel, sizeof(context.sentinel));
    LumenAndroidFexCoreGuestStateV1 state {
        sizeof(state), 1, context.code + (acrossHost ? 64 : 0), {}, 0, 0, 0, 0, 0,
    };
    state.gprs[Rsp] = stack; state.gprs[Rdi] = context.data;
    state.gprs[Rsi] = context.data + (nested ? 64 : 32); state.gprs[Rbx] = context.target;
    return context.api->run(&state, &HostCall, &Query, &context);
}

inline std::uint32_t HostCall(void* opaque, std::uint64_t target,
    std::uint64_t* gprs, LumenAndroidFexCoreSimdViewV2*, std::uint64_t* nextRip) noexcept {
    auto& context = *static_cast<Context*>(opaque);
    if (target != context.target || context.calls != 0 || gprs[Rsp] < context.data ||
        gprs[Rsp] - context.data > 2 * context.page - 8)
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    std::uint64_t returnRip;
    std::memcpy(&returnRip, reinterpret_cast<const void*>(gprs[Rsp]), sizeof(returnRip));
    if (returnRip < context.code || returnRip - context.code >= context.page)
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    ++context.calls; context.entryRounding = std::fegetround();
    if (context.nested) {
        context.nestedCode = Run(context, false, true);
        if (context.nestedCode != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1)
            return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    } else if (std::fesetround(FE_UPWARD) != 0) {
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    }
    gprs[Rsp] += 8; *nextRip = returnRip;
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
}

inline std::uint32_t Probe(const Api& api,
    LumenAndroidFexCoreFpBoundaryProbeStatusV1* output) noexcept {
    *output = {};
    std::fenv_t nativeEnvironment;
    if (std::fegetenv(&nativeEnvironment) != 0) return 0;
    struct Restore {
        std::fenv_t* environment;
        ~Restore() { (void)std::fesetenv(environment); }
    } restore {&nativeEnvironment};
    const auto page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return 0;
    Context context {&api, 0, 0, static_cast<std::uint64_t>(page), api.rootReturn(),
                     reinterpret_cast<std::uintptr_t>(&TargetIdentity)};
    if (context.sentinel == 0 || api.registerTarget(context.target) == 0) return 0;
    void* const code = mmap(nullptr, context.page, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) return 0;
    void* const data = mmap(nullptr, context.page * 2, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED) { munmap(code, context.page); return 0; }
    context.code = reinterpret_cast<std::uintptr_t>(code);
    context.data = reinterpret_cast<std::uintptr_t>(data);
    namespace Programs = Lumen::FexCore::FpBoundaryProbe;
    std::memcpy(code, Programs::RootConversion.data(), Programs::RootConversion.size());
    std::memcpy(static_cast<std::uint8_t*>(code) + 64,
                Programs::AcrossHostCall.data(), Programs::AcrossHostCall.size());
    if (mprotect(code, context.page, PROT_READ) != 0) {
        munmap(code, context.page); munmap(data, context.page * 2); return 0;
    }
    auto* words = static_cast<std::uint32_t*>(data);
    words[0] = Programs::InputBits; words[1] = Programs::GuestDownwardMxcsr;
    bool executed = true;
    for (unsigned test = 0; test != 3 && executed; ++test) {
        std::memset(words + 8, 0, 48); context.calls = 0; context.nested = test == 2;
        if (std::fesetround(test == 0 ? FE_DOWNWARD : FE_TOWARDZERO) != 0) {
            output->runtimeCode = LUMEN_ANDROID_FEXCORE_RUNTIME_THREAD_SETUP_FAILED_V1;
            executed = false; break;
        }
        output->runtimeCode = Run(context, test != 0, false);
        executed = output->runtimeCode == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
            (test == 0 || context.calls == 1) && (test != 2 || context.nestedCode == 0);
        if (!executed) break;
        ++output->executedCases;
        if (test == 0) {
            output->rootResult = static_cast<std::int32_t>(words[8]);
            output->rootMxcsr = words[9]; output->rootReturnRounding = std::fegetround();
        } else if (test == 1) {
            output->hleBefore = static_cast<std::int32_t>(words[8]);
            output->hleAfter = static_cast<std::int32_t>(words[10]);
            output->hleMxcsrBefore = words[9]; output->hleMxcsrAfter = words[11];
            output->hleEntryRounding = context.entryRounding;
            output->hleReturnRounding = std::fegetround();
        } else {
            output->nestedOuterBefore = static_cast<std::int32_t>(words[8]);
            output->nestedOuterAfter = static_cast<std::int32_t>(words[10]);
            output->nestedResult = static_cast<std::int32_t>(words[16]);
            output->nestedOuterMxcsrBefore = words[9]; output->nestedOuterMxcsrAfter = words[11];
            output->nestedMxcsr = words[17]; output->nestedEntryRounding = context.entryRounding;
            output->nestedReturnRounding = std::fegetround();
        }
    }
    const auto mxcsrIs = [](std::uint32_t value, std::uint32_t expected) {
        return (value & 0xffc0U) == expected;
    };
    if (executed) {
        if (output->rootResult == 2 && mxcsrIs(output->rootMxcsr, Programs::DefaultMxcsr))
            output->conformanceMask |= 1;
        if (output->hleBefore == 1 && output->hleAfter == 1 &&
            mxcsrIs(output->hleMxcsrBefore, Programs::GuestDownwardMxcsr) &&
            mxcsrIs(output->hleMxcsrAfter, Programs::GuestDownwardMxcsr)) output->conformanceMask |= 2;
        if (output->nestedResult == 2 && output->nestedOuterBefore == 1 && output->nestedOuterAfter == 1 &&
            mxcsrIs(output->nestedMxcsr, Programs::DefaultMxcsr) &&
            mxcsrIs(output->nestedOuterMxcsrBefore, Programs::GuestDownwardMxcsr) &&
            mxcsrIs(output->nestedOuterMxcsrAfter, Programs::GuestDownwardMxcsr)) output->conformanceMask |= 4;
        if (output->hleEntryRounding == FE_TOWARDZERO && output->nestedEntryRounding == FE_TOWARDZERO)
            output->conformanceMask |= 8;
        if (output->rootReturnRounding == FE_DOWNWARD && output->hleReturnRounding == FE_TOWARDZERO &&
            output->nestedReturnRounding == FE_TOWARDZERO) output->conformanceMask |= 16;
    }
    const auto token = api.beginMutation();
    if (token == 0) return 0; // Preserve storage if close is denied.
    bool closed = false;
    struct Close { Context* context; bool* closed; } close {&context, &closed};
    const bool ended = api.endMutation(token, context.code, context.page, 1,
        +[](void* opaque) noexcept {
            auto& close = *static_cast<Close*>(opaque);
            const bool a = munmap(reinterpret_cast<void*>(close.context->code), close.context->page) == 0;
            const bool b = munmap(reinterpret_cast<void*>(close.context->data), close.context->page * 2) == 0;
            *close.closed = a && b;
        }, &close) != 0;
    const bool restored = std::fesetenv(&nativeEnvironment) == 0;
    return executed && ended && closed && restored ? 1U : 0U;
}

} // namespace Lumen::FexCore::FpBoundaryLegacyProbe
