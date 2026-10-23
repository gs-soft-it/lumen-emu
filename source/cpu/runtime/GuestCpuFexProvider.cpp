// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
#include "GuestCpuFexPrivateSymbols.h"
#include "GuestCpuRuntime.h"

// The private diagnostic is implemented on both hosts; its legacy public
// declaration is gated by consumer build definitions in the existing header.
extern "C" std::uint32_t LumenAndroidFexCoreHostTargetSnapshotContractTestV1() noexcept;

namespace Lumen::GuestCpu {
const ProviderFunctions& FexProviderFunctions() noexcept {
    static const ProviderFunctions functions {
        .provider = Provider::Fex,
#define LUMEN_GUEST_CPU_API(requirement, field, symbol, result, parameters, arguments, denied) \
        .field = &symbol,
#include "GuestCpuRuntimeApi.inc"
#undef LUMEN_GUEST_CPU_API
    };
    return functions;
}
} // namespace Lumen::GuestCpu
