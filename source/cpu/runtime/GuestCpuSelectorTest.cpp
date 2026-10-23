// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
// Synthetic function tables test dispatch and ownership only. They execute no
// guest instructions and cannot qualify either translator's runtime.
#include "GuestCpuRuntime.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <type_traits>
#include <vector>
#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
using namespace Lumen::GuestCpu;
std::atomic<unsigned> fex_calls {};
std::atomic<unsigned> qemu_calls {};
HostCallEffectMask EffectsProof(std::uint64_t target) noexcept {
    return EffectMask(target == 0x1000 ? HostCallEffect::None : HostCallEffect::Unknown);
}
HostCallEffectMask DifferentEffectsProof(std::uint64_t) noexcept {
    return EffectMask(HostCallEffect::None);
}

template<class Signature> struct Unused;
template<class Result, class... Arguments>
struct Unused<Result (*)(Arguments...) noexcept> {
    static Result Call(Arguments...) noexcept {
        if constexpr (!std::is_void_v<Result>) return {};
    }
};
std::uint32_t FexProbe(LumenAndroidFexCoreRuntimeStatusV1* status) noexcept {
    ++fex_calls;
    if (status != nullptr) {
        *status = {};
        status->code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1;
    }
    return LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1;
}
std::uint32_t QemuProbe(LumenAndroidFexCoreRuntimeStatusV1* status) noexcept {
    ++qemu_calls;
    if (status != nullptr) {
        *status = {};
        status->code = LUMEN_ANDROID_FEXCORE_RUNTIME_SELF_TEST_FAILED_V1;
    }
    return LUMEN_ANDROID_FEXCORE_RUNTIME_SELF_TEST_FAILED_V1;
}
std::uint32_t QemuRun(LumenAndroidFexCoreGuestStateV1* state,
    LumenAndroidFexCoreHostCallV2, LumenAndroidFexCoreExecutableQueryV1, void*) noexcept {
    ++qemu_calls;
    if (state != nullptr) state->rip += 17;
    return LUMEN_ANDROID_FEXCORE_RUNTIME_SELF_TEST_FAILED_V1;
}
const char* FixtureReceipt() noexcept { return "selector-test fixture guestExecuted=0 ready=0"; }
std::uint64_t NativeV1() noexcept { ++qemu_calls; return 0x2468; }
std::uint64_t NativeV2(std::uint32_t* pending) noexcept {
    ++qemu_calls;
    if (pending != nullptr) *pending = 1;
    return 0;
}
std::uint64_t MutationV2(std::uint32_t kind) noexcept {
    ++qemu_calls;
    return kind == 1 ? 0x3579 : 0;
}
std::uint64_t MutationV3(std::uint32_t kind, std::uint32_t* retryable) noexcept {
    ++qemu_calls;
    if (retryable != nullptr) *retryable = kind == 1 ? 1U : 0U;
    return kind == 0 ? 0x468a : 0;
}
std::uint32_t MemoryProof(std::uint64_t address, std::uint64_t size,
    std::uint32_t write) noexcept {
    return address == 0x1000 && size == 8 && write <= 1 ? 1U : 0U;
}
std::uint32_t DifferentMemoryProof(std::uint64_t, std::uint64_t,
    std::uint32_t) noexcept { return 0; }
template<Provider provider> void FreshCall() noexcept {
    if constexpr (provider == Provider::Fex) ++fex_calls;
    else ++qemu_calls;
}
template<Provider provider> std::uint64_t BeginFresh(std::uint64_t stack,
    std::uint64_t stackSize, std::uint64_t tls, std::uint64_t tlsSize) noexcept {
    FreshCall<provider>();
    return stack == 0x1000 && stackSize == 0x2000 && tls == 0x3000 && tlsSize == 0x4000
        ? 0x5678 : 0;
}
template<Provider provider> std::uint32_t EndFresh(std::uint64_t token) noexcept {
    FreshCall<provider>();
    return token == 0x5678 ? 1U : 0U;
}
template<Provider provider> std::uint32_t ValidateFresh() noexcept {
    FreshCall<provider>();
    return 1U;
}
template<Provider provider> void NotifyFresh(std::uint64_t base, std::uint64_t size) noexcept {
    if (base == 0x9000 && size == 0x1000) FreshCall<provider>();
}
template<Provider provider> std::uint32_t RegisterFreshQuery(
    LumenAndroidFexCoreFreshCallbackDataAccessQueryV1 query) noexcept {
    FreshCall<provider>();
    return query == MemoryProof ? 1U : 0U;
}
ProviderFunctions Fixture(Provider provider) {
    ProviderFunctions functions {};
    functions.provider = provider;
#define LUMEN_GUEST_CPU_API(requirement, field, symbol, result, parameters, arguments, denied) \
    functions.field = &Unused<decltype(functions.field)>::Call;
#include "GuestCpuRuntimeApi.inc"
#undef LUMEN_GUEST_CPU_API
    functions.runtimeProbe = provider == Provider::Fex ? FexProbe : QemuProbe;
    functions.runtimeReceipt = FixtureReceipt;
    if (provider == Provider::QemuTcg) functions.runGuestV2 = QemuRun;
    functions.beginFreshGuestCallback = provider == Provider::Fex
        ? BeginFresh<Provider::Fex> : BeginFresh<Provider::QemuTcg>;
    functions.endFreshGuestCallback = provider == Provider::Fex
        ? EndFresh<Provider::Fex> : EndFresh<Provider::QemuTcg>;
    functions.validateFreshGuestCallbackParent = provider == Provider::Fex
        ? ValidateFresh<Provider::Fex> : ValidateFresh<Provider::QemuTcg>;
    functions.notifyFreshGuestCallbackMappingChange = provider == Provider::Fex
        ? NotifyFresh<Provider::Fex> : NotifyFresh<Provider::QemuTcg>;
    functions.registerFreshCallbackDataAccessQuery = provider == Provider::Fex
        ? RegisterFreshQuery<Provider::Fex> : RegisterFreshQuery<Provider::QemuTcg>;
    return functions;
}
bool Check(bool value, const char* expression) {
    if (!value) std::fprintf(stderr, "FAIL: %s\n", expression);
    return value;
}
#define REQUIRE(expression) do { if (!Check(expression, #expression)) return 1; } while (false)
} // namespace

namespace Lumen::GuestCpu {
const ProviderFunctions& FexProviderFunctions() noexcept {
    static const auto functions = Fixture(Provider::Fex);
    return functions;
}
#if defined(LUMEN_WITH_QEMU_TCG) && LUMEN_WITH_QEMU_TCG
const ProviderFunctions& QemuProviderFunctions() noexcept {
    static const auto functions = Fixture(Provider::QemuTcg);
    return functions;
}
#endif
} // namespace Lumen::GuestCpu

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const char* mode = argv[1];
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    if (std::strcmp(mode, "default") == 0) {
        unsetenv("LUMEN_CPU_TRANSLATOR");
        REQUIRE(Selected() == Provider::Fex);
#if defined(LUMEN_WITH_QEMU_TCG) && LUMEN_WITH_QEMU_TCG
        REQUIRE(Available(Provider::QemuTcg));
#else
        REQUIRE(!Available(Provider::QemuTcg));
#endif
        REQUIRE(LumenAndroidFexCoreRuntimeProbeV1(&status) ==
            LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1);
        REQUIRE(SelectionLocked() && Selected() == Provider::Fex);
        REQUIRE(fex_calls == 1 && qemu_calls == 0);
        REQUIRE(LumenAndroidFexCoreBeginFreshGuestCallbackV1(0x1000, 0x2000, 0x3000, 0x4000) == 0x5678);
        REQUIRE(LumenAndroidFexCoreEndFreshGuestCallbackV1(0x5678) == 1);
        REQUIRE(LumenAndroidFexCoreValidateFreshGuestCallbackParentV1() == 1);
        LumenAndroidFexCoreNotifyFreshGuestCallbackMappingChangeV1(0x9000, 0x1000);
        REQUIRE(LumenAndroidFexCoreRegisterFreshCallbackDataAccessQueryV1(MemoryProof) == 1);
        REQUIRE(fex_calls == 6 && qemu_calls == 0);
    } else if (std::strcmp(mode, "names") == 0) {
        unsetenv("LUMEN_CPU_TRANSLATOR");
        REQUIRE(Selected() == Provider::Fex);
        REQUIRE(Select(nullptr) == SelectionResult::UnknownProvider);
        REQUIRE(Select("") == SelectionResult::UnknownProvider);
        REQUIRE(Select("FEX") == SelectionResult::UnknownProvider);
        REQUIRE(Select("qemu") == SelectionResult::UnknownProvider);
        REQUIRE(Select(static_cast<Provider>(99)) == SelectionResult::UnknownProvider);
        REQUIRE(Selected() == Provider::Fex && !SelectionLocked());
        REQUIRE(Select("fex") == SelectionResult::Selected);
    } else if (std::strcmp(mode, "unavailable") == 0) {
        REQUIRE(Select("qemu-tcg") == SelectionResult::Unavailable);
        REQUIRE(Selected() == Provider::QemuTcg);
        REQUIRE(!Available(Provider::QemuTcg));
        REQUIRE(LumenAndroidFexCoreRuntimeProbeV1(&status) ==
            LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1);
        REQUIRE(status.backendRouteReady == 0 && status.guestExecuted == 0);
        REQUIRE(LumenAndroidFexCoreBeginGuestMemoryMutationV1() == 0);
        REQUIRE(LumenAndroidFexCoreBeginFreshGuestCallbackV1(0x1000, 0x2000, 0x3000, 0x4000) == 0);
        REQUIRE(LumenAndroidFexCoreEndFreshGuestCallbackV1(0x5678) == 0);
        REQUIRE(LumenAndroidFexCoreValidateFreshGuestCallbackParentV1() == 0);
        LumenAndroidFexCoreNotifyFreshGuestCallbackMappingChangeV1(0x9000, 0x1000);
        REQUIRE(LumenAndroidFexCoreRegisterFreshCallbackDataAccessQueryV1(MemoryProof) == 0);
        std::uint32_t mutation_pending = 99;
        REQUIRE(LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV2(&mutation_pending) == 0 && mutation_pending == 0);
        mutation_pending = 99;
        REQUIRE(LumenAndroidFexCoreBeginGuestMemoryMutationV3(1, &mutation_pending) == 0 && mutation_pending == 0);
        LumenAndroidFexCoreGuestStateV1 state {};
        state.rip = 123;
        REQUIRE(LumenAndroidFexCoreRunGuestV2(&state, nullptr, nullptr, nullptr) ==
            LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1 && state.rip == 123);
        REQUIRE(std::strstr(LumenAndroidFexCoreRuntimeReceiptV1(), "fallback=0") != nullptr);
        REQUIRE(fex_calls == 0 && Select("fex") == SelectionResult::Locked);
        REQUIRE(!RegisterProvider(Fixture(Provider::QemuTcg)));
    } else if (std::strcmp(mode, "registration") == 0) {
        auto functions = Fixture(Provider::QemuTcg);
        // Every production method is required, including callback ownership
        // handoff and lifetime notifications; no provider can partially enroll.
#define LUMEN_GUEST_CPU_CHECK_Required(field) \
        { auto incomplete = functions; incomplete.field = nullptr; REQUIRE(!RegisterProvider(incomplete)); }
#define LUMEN_GUEST_CPU_CHECK_Optional(field)
#define LUMEN_GUEST_CPU_API(requirement, field, symbol, result, parameters, arguments, denied) \
        LUMEN_GUEST_CPU_CHECK_##requirement(field)
#include "GuestCpuRuntimeApi.inc"
#undef LUMEN_GUEST_CPU_API
#undef LUMEN_GUEST_CPU_CHECK_Optional
#undef LUMEN_GUEST_CPU_CHECK_Required
        REQUIRE(!Available(Provider::QemuTcg));
        auto incomplete = functions;
        incomplete.abiVersion = 2;
        REQUIRE(!RegisterProvider(incomplete));
        REQUIRE(RegisterProvider(functions) && RegisterProvider(functions));
        auto replacement = functions;
        replacement.runtimeProbe = FexProbe;
        REQUIRE(!RegisterProvider(replacement));
        REQUIRE(Select("qemu-tcg") == SelectionResult::Selected);
        // Registration copies the table: a caller changing its local storage
        // cannot redirect an already admitted provider back to FEX.
        functions.runtimeProbe = FexProbe;
        REQUIRE(LumenAndroidFexCoreRuntimeProbeV1(&status) ==
            LUMEN_ANDROID_FEXCORE_RUNTIME_SELF_TEST_FAILED_V1);
        LumenAndroidFexCoreGuestStateV1 state {};
        state.rip = 100;
        REQUIRE(LumenAndroidFexCoreRunGuestV2(&state, nullptr, nullptr, nullptr) ==
            LUMEN_ANDROID_FEXCORE_RUNTIME_SELF_TEST_FAILED_V1 && state.rip == 117);
        REQUIRE(fex_calls == 0 && qemu_calls == 2);
        REQUIRE(LumenAndroidFexCoreBeginFreshGuestCallbackV1(0x1000, 0x2000, 0x3000, 0x4000) == 0x5678);
        REQUIRE(LumenAndroidFexCoreEndFreshGuestCallbackV1(0x5678) == 1);
        REQUIRE(LumenAndroidFexCoreValidateFreshGuestCallbackParentV1() == 1);
        LumenAndroidFexCoreNotifyFreshGuestCallbackMappingChangeV1(0x9000, 0x1000);
        REQUIRE(LumenAndroidFexCoreRegisterFreshCallbackDataAccessQueryV1(MemoryProof) == 1);
        REQUIRE(fex_calls == 0 && qemu_calls == 7);
        REQUIRE(Select("fex") == SelectionResult::Locked);
        REQUIRE(Select("qemu-tcg") == SelectionResult::Selected);
    } else if (std::strcmp(mode, "native-legacy") == 0) {
        unsetenv("LUMEN_CPU_TRANSLATOR");
        auto functions = Fixture(Provider::QemuTcg);
        functions.tryBeginNativeDataLease = NativeV1;
        functions.tryBeginNativeDataLeaseV2 = NativeV2;
        functions.structSize = ProviderFunctionsV1Size - 1;
        REQUIRE(!RegisterProvider(functions));
        functions.structSize = ProviderFunctionsV1Size + 1;
        REQUIRE(!RegisterProvider(functions));
        functions.structSize = ProviderFunctionsV1Size;
#if !defined(_WIN32)
        // The legacy table ends exactly at a guard page: validation, copying
        // and repeated registration must never inspect the appended V2 field.
        const long page = sysconf(_SC_PAGESIZE);
        REQUIRE(page > 0 && ProviderFunctionsV1Size < static_cast<size_t>(page));
        void* memory = mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        REQUIRE(memory != MAP_FAILED);
        auto* legacy = reinterpret_cast<ProviderFunctions*>(
            static_cast<char*>(memory) + page - ProviderFunctionsV1Size);
        std::memcpy(legacy, &functions, ProviderFunctionsV1Size);
        REQUIRE(mprotect(static_cast<char*>(memory) + page, page, PROT_NONE) == 0);
        REQUIRE(RegisterProvider(*legacy) && RegisterProvider(*legacy));
        REQUIRE(munmap(memory, 2 * page) == 0);
#else
        REQUIRE(RegisterProvider(functions) && RegisterProvider(functions));
#endif
        REQUIRE(Select("qemu-tcg") == SelectionResult::Selected);
        std::uint32_t pending = 99;
        REQUIRE(LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV2(&pending) == 0x2468 && pending == 0);
        REQUIRE(Selected() == Provider::QemuTcg && SelectionLocked() && fex_calls == 0 && qemu_calls == 1);
        REQUIRE(Select("fex") == SelectionResult::Locked);
        REQUIRE(std::strstr(LumenAndroidFexCoreRuntimeReceiptV1(), "ready=0") != nullptr);
    } else if (std::strcmp(mode, "native-v2") == 0) {
        unsetenv("LUMEN_CPU_TRANSLATOR");
        auto functions = Fixture(Provider::QemuTcg);
        functions.tryBeginNativeDataLease = NativeV1;
        functions.tryBeginNativeDataLeaseV2 = NativeV2;
        REQUIRE(RegisterProvider(functions) && Select("qemu-tcg") == SelectionResult::Selected);
        std::uint32_t pending = 99;
        REQUIRE(LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV2(&pending) == 0 && pending == 1);
        REQUIRE(LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV1() == 0x2468);
        REQUIRE(Selected() == Provider::QemuTcg && SelectionLocked() && fex_calls == 0 && qemu_calls == 2);
    } else if (std::strcmp(mode, "mutation-legacy-v1") == 0 ||
               std::strcmp(mode, "mutation-legacy-v2") == 0 ||
               std::strcmp(mode, "handoff-legacy-v3") == 0 ||
               std::strcmp(mode, "diagnostic-legacy-v4") == 0) {
        unsetenv("LUMEN_CPU_TRANSLATOR");
        auto functions = Fixture(Provider::QemuTcg);
        functions.beginMemoryMutationV2 = MutationV2;
        functions.beginMemoryMutationV3 = MutationV3;
        const bool legacyV3 = std::strcmp(mode, "handoff-legacy-v3") == 0;
        const bool legacyV4 = std::strcmp(mode, "diagnostic-legacy-v4") == 0;
        functions.structSize = legacyV4 ? ProviderFunctionsV4Size : legacyV3 ? ProviderFunctionsV3Size :
            (std::strcmp(mode, "mutation-legacy-v1") == 0 ? ProviderFunctionsV1Size : ProviderFunctionsV2Size);
        if (legacyV3 || legacyV4) functions.beginMemoryMutationV3 = nullptr;
#if !defined(_WIN32)
        const long page = sysconf(_SC_PAGESIZE);
        REQUIRE(page > 0 && functions.structSize < static_cast<size_t>(page));
        void* memory = mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        REQUIRE(memory != MAP_FAILED);
        auto* legacy = reinterpret_cast<ProviderFunctions*>(static_cast<char*>(memory) + page - functions.structSize);
        std::memcpy(legacy, &functions, functions.structSize);
        REQUIRE(mprotect(static_cast<char*>(memory) + page, page, PROT_NONE) == 0);
        REQUIRE(RegisterProvider(*legacy) && RegisterProvider(*legacy));
        REQUIRE(munmap(memory, 2 * page) == 0);
#else
        REQUIRE(RegisterProvider(functions));
#endif
        REQUIRE(Select("qemu-tcg") == SelectionResult::Selected);
        REQUIRE(LumenAndroidFexCoreConfigureMemoryHandoffV1(0) == 0);
        LumenAndroidFexCoreSetHostCallDiagnosticV1(71, "legacy-table-no-diagnostic");
        std::uint32_t retryable = 99;
        REQUIRE(LumenAndroidFexCoreBeginGuestMemoryMutationV3(1, &retryable) == 0x3579 && retryable == 0);
        retryable = 99;
        REQUIRE(LumenAndroidFexCoreBeginGuestMemoryMutationV3(0, &retryable) == 0 && retryable == 0);
        REQUIRE(Selected() == Provider::QemuTcg && fex_calls == 0 && qemu_calls == 2);
    } else if (std::strcmp(mode, "mutation-v3") == 0) {
        unsetenv("LUMEN_CPU_TRANSLATOR");
        auto functions = Fixture(Provider::QemuTcg);
        functions.beginMemoryMutationV2 = MutationV2;
        functions.beginMemoryMutationV3 = MutationV3;
        REQUIRE(RegisterProvider(functions) && Select("qemu-tcg") == SelectionResult::Selected);
        std::uint32_t retryable = 99;
        REQUIRE(LumenAndroidFexCoreBeginGuestMemoryMutationV3(1, &retryable) == 0 && retryable == 1);
        REQUIRE(LumenAndroidFexCoreBeginGuestMemoryMutationV3(0, &retryable) == 0x468a && retryable == 0);
        retryable = 99;
        REQUIRE(LumenAndroidFexCoreBeginGuestMemoryMutationV3(99, &retryable) == 0 && retryable == 0);
        REQUIRE(LumenAndroidFexCoreBeginGuestMemoryMutationV2(1) == 0x3579);
        REQUIRE(Selected() == Provider::QemuTcg && fex_calls == 0 && qemu_calls == 4);
    } else if (std::strcmp(mode, "locked-probe") == 0 ||
               std::strcmp(mode, "locked-registration") == 0 ||
               std::strcmp(mode, "locked-mutation") == 0) {
        unsetenv("LUMEN_CPU_TRANSLATOR");
        REQUIRE(RegisterProvider(Fixture(Provider::QemuTcg)));
        if (std::strcmp(mode, "locked-probe") == 0)
            REQUIRE(LumenAndroidFexCoreRuntimeProbeV1(nullptr) ==
                LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1);
        else if (std::strcmp(mode, "locked-registration") == 0)
            (void)LumenAndroidFexCoreRegisterHostCallTargetV1(0x1234);
        else
            (void)LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        REQUIRE(SelectionLocked() && Select("qemu-tcg") == SelectionResult::Locked);
        REQUIRE(Selected() == Provider::Fex && Select("fex") == SelectionResult::Selected);
    } else if (std::strcmp(mode, "environment") == 0) {
        REQUIRE(setenv("LUMEN_CPU_TRANSLATOR", "qemu-tcg", 1) == 0);
        REQUIRE(RegisterProvider(Fixture(Provider::QemuTcg)));
        REQUIRE(std::strcmp(ProviderName(), "qemu-tcg") == 0);
        REQUIRE(LumenAndroidFexCoreRuntimeProbeV1(nullptr) ==
            LUMEN_ANDROID_FEXCORE_RUNTIME_SELF_TEST_FAILED_V1);
        REQUIRE(setenv("LUMEN_CPU_TRANSLATOR", "fex", 1) == 0);
        REQUIRE(Selected() == Provider::QemuTcg && fex_calls == 0);
    } else if (std::strcmp(mode, "invalid-environment") == 0) {
        REQUIRE(setenv("LUMEN_CPU_TRANSLATOR", "", 1) == 0);
        REQUIRE(LumenAndroidFexCoreRuntimeProbeV1(&status) ==
            LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1);
        REQUIRE(Selected() == Provider::Invalid && fex_calls == 0);
        REQUIRE(std::strstr(LumenAndroidFexCoreRuntimeReceiptV1(), "invalidSelector=1") != nullptr);
        REQUIRE(Select("fex") == SelectionResult::Locked);
    } else if (std::strcmp(mode, "memory-query") == 0) {
        REQUIRE(GetMemoryAccessQuery() == nullptr);
        REQUIRE(!RegisterMemoryAccessQuery(nullptr));
        REQUIRE(RegisterMemoryAccessQuery(MemoryProof));
        REQUIRE(RegisterMemoryAccessQuery(MemoryProof));
        REQUIRE(!RegisterMemoryAccessQuery(DifferentMemoryProof));
        const auto proof = GetMemoryAccessQuery();
        REQUIRE(proof == MemoryProof && proof(0x1000, 8, 0) == 1);
        REQUIRE(proof(0x1001, 8, 0) == 0 && proof(0x1000, 8, 2) == 0);
        REQUIRE(!SelectionLocked());
    } else if (std::strcmp(mode, "effects-query") == 0) {
        REQUIRE(QueryHostCallEffects(0x1000) == EffectMask(HostCallEffect::Unknown));
        REQUIRE(!RegisterHostCallEffectsQuery(nullptr));
        REQUIRE(RegisterHostCallEffectsQuery(EffectsProof));
        REQUIRE(RegisterHostCallEffectsQuery(EffectsProof));
        REQUIRE(!RegisterHostCallEffectsQuery(DifferentEffectsProof));
        REQUIRE(QueryHostCallEffects(0x1000) == EffectMask(HostCallEffect::None));
        REQUIRE(QueryHostCallEffects(0x1001) == EffectMask(HostCallEffect::Unknown));
        REQUIRE(!SelectionLocked() && fex_calls == 0 && qemu_calls == 0);
    } else if (std::strcmp(mode, "concurrent") == 0) {
        unsetenv("LUMEN_CPU_TRANSLATOR");
        REQUIRE(RegisterProvider(Fixture(Provider::QemuTcg)));
        REQUIRE(LumenAndroidFexCoreRuntimeProbeV1(nullptr) ==
            LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1);
        std::atomic<bool> valid {true};
        std::vector<std::thread> peers;
        for (unsigned peer = 0; peer != 8; ++peer) peers.emplace_back([&] {
            for (unsigned call = 0; call != 32; ++call) {
                if (Select("qemu-tcg") != SelectionResult::Locked ||
                    LumenAndroidFexCoreRuntimeProbeV1(nullptr) !=
                        LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1) valid = false;
            }
        });
        for (auto& peer: peers) peer.join();
        REQUIRE(valid && fex_calls == 257 && qemu_calls == 0);
    } else return 2;
    std::printf("guest CPU selector %s: PASS (synthetic dispatch only)\n", mode);
    return 0;
}
