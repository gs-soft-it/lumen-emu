#include "AndroidFexCoreRuntime.h"

#include <cfenv>
#include <cstdio>
#include <cstring>
#if !defined(LUMEN_FEX_FP_PROBE_DIRECT_LINK)
#include "FexFpBoundaryLegacyProbe.h"
#include <dlfcn.h>
#endif

namespace {
using Probe = std::uint32_t (*)(LumenAndroidFexCoreFpBoundaryProbeStatusV1*) noexcept;
#if !defined(LUMEN_FEX_FP_PROBE_DIRECT_LINK)
namespace Legacy = Lumen::FexCore::FpBoundaryLegacyProbe;
Legacy::Api legacyApi {};
std::uint32_t LegacyProbe(LumenAndroidFexCoreFpBoundaryProbeStatusV1* status) noexcept {
    return Legacy::Probe(legacyApi, status);
}
#endif

struct NativeControl {
    int rounding;
#if defined(__aarch64__) || defined(__arm64__)
    std::uint64_t fpcr;
    std::uint64_t fpsr;
#endif
};

NativeControl ReadNativeControl() noexcept {
    NativeControl result {std::fegetround()
#if defined(__aarch64__) || defined(__arm64__)
        , 0, 0
#endif
    };
#if defined(__aarch64__) || defined(__arm64__)
    asm volatile("mrs %0, fpcr" : "=r"(result.fpcr));
    asm volatile("mrs %0, fpsr" : "=r"(result.fpsr));
#endif
    return result;
}

bool SameNativeControl(const NativeControl& a, const NativeControl& b) noexcept {
    return a.rounding == b.rounding
#if defined(__aarch64__) || defined(__arm64__)
        && a.fpcr == b.fpcr && a.fpsr == b.fpsr
#endif
        ;
}
} // namespace

int main(int argc, char** argv) {
    bool expectGap = false;
    bool legacy = false;
    Probe probe = nullptr;
#if defined(LUMEN_FEX_FP_PROBE_DIRECT_LINK)
    if (argc == 2 && std::strcmp(argv[1], "--expect-gap") == 0) expectGap = true;
    else if (argc != 1) {
        std::fprintf(stderr, "usage: %s [--expect-gap]\n", argv[0]);
        return 2;
    }
    probe = &LumenAndroidFexCoreFpBoundaryProbeV1;
#else
    if (argc == 3 && std::strcmp(argv[2], "--expect-gap") == 0) expectGap = true;
    else if (argc != 2) {
        std::fprintf(stderr, "usage: %s RUNTIME_LIBRARY [--expect-gap]\n", argv[0]);
        return 2;
    }
    void* const library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        std::fprintf(stderr, "runtime load failed: %s\n", dlerror());
        return 2;
    }
    probe = reinterpret_cast<Probe>(dlsym(library, "LumenAndroidFexCoreFpBoundaryProbeV1"));
    if (probe == nullptr) {
        const auto symbol = [&]<typename T>(const char* name) {
            const auto value = reinterpret_cast<T>(dlsym(library, name));
            if (value == nullptr) std::fprintf(stderr, "legacy export unavailable %s: %s\n", name, dlerror());
            return value;
        };
        legacyApi = {
            symbol.template operator()<decltype(legacyApi.registerTarget)>("LumenAndroidFexCoreRegisterHostCallTargetV1"),
            symbol.template operator()<decltype(legacyApi.rootReturn)>("LumenAndroidFexCoreRootReturnAddressV1"),
            symbol.template operator()<decltype(legacyApi.run)>("LumenAndroidFexCoreRunGuestV2"),
            symbol.template operator()<decltype(legacyApi.beginMutation)>("LumenAndroidFexCoreBeginGuestMemoryMutationV1"),
            symbol.template operator()<decltype(legacyApi.endMutation)>("LumenAndroidFexCoreEndGuestMemoryMutationV2"),
        };
        if (legacyApi.registerTarget == nullptr || legacyApi.rootReturn == nullptr || legacyApi.run == nullptr ||
            legacyApi.beginMutation == nullptr || legacyApi.endMutation == nullptr) return 2;
        probe = &LegacyProbe;
        legacy = true;
    }
    // The FEX runtime owns process signal/allocator state until process exit.
    // Keep its image loaded instead of unloading those registered handlers.
#endif
    const auto nativeBefore = ReadNativeControl();
    LumenAndroidFexCoreFpBoundaryProbeStatusV1 status;
    const auto executed = probe(&status);
    const auto nativeAfter = ReadNativeControl();
    const bool nativeRestored = SameNativeControl(nativeBefore, nativeAfter);
    const bool expected = expectGap ? status.conformanceMask != 31 : status.conformanceMask == 31;
    const bool pass = executed == 1 && status.executedCases == 3 && nativeRestored && expected;
    std::printf("LUMEN_FEX_FP_BOUNDARY_PROBE_V1 source=%s pass=%u executed=%u code=%u cases=%u "
                "conformance=%u expectedGap=%u nativeRestored=%u "
                "root=%d rootMxcsr=%04x rootReturnRound=%d "
                "hleBefore=%d hleAfter=%d hleMxcsrBefore=%04x hleMxcsrAfter=%04x "
                "hleEntryRound=%d hleReturnRound=%d "
                "nested=%d nestedMxcsr=%04x outerBefore=%d outerAfter=%d "
                "outerMxcsrBefore=%04x outerMxcsrAfter=%04x "
                "nestedEntryRound=%d nestedReturnRound=%d\n",
                legacy ? "legacy-v2" : "runtime-api", pass ? 1U : 0U, executed, status.runtimeCode, status.executedCases,
                status.conformanceMask, expectGap ? 1U : 0U, nativeRestored ? 1U : 0U,
                status.rootResult, status.rootMxcsr, status.rootReturnRounding,
                status.hleBefore, status.hleAfter, status.hleMxcsrBefore, status.hleMxcsrAfter,
                status.hleEntryRounding, status.hleReturnRounding,
                status.nestedResult, status.nestedMxcsr, status.nestedOuterBefore, status.nestedOuterAfter,
                status.nestedOuterMxcsrBefore, status.nestedOuterMxcsrAfter,
                status.nestedEntryRounding, status.nestedReturnRounding);
    return pass ? 0 : 1;
}
