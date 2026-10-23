// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "AndroidFexCoreRuntime.h"
#include "HostCallEffects.h"

#include <cstddef>
#include <type_traits>

namespace Lumen::GuestCpu {

enum class Provider : std::uint32_t { Fex, QemuTcg, Invalid = 0xffffffffU };
enum class SelectionResult : std::uint32_t {
    Selected, UnknownProvider, Unavailable, Locked,
};

// The old exported ABI is retained for the two console engines and VM hooks.
// Each provider implements the complete production surface, including its own
// counted execution/HLE readers, safepoints and translated-code invalidation.
// A populated table does not establish runtime readiness; Probe must do that.
struct ProviderFunctions {
    std::uint32_t structSize {sizeof(ProviderFunctions)};
    std::uint32_t abiVersion {1};
    Provider provider {Provider::Fex};
#define LUMEN_GUEST_CPU_API(requirement, field, symbol, result, parameters, arguments, denied) \
    result (*field) parameters noexcept {};
#include "GuestCpuRuntimeApi.inc"
#undef LUMEN_GUEST_CPU_API
};

// The original ABI v1 table ends before the first appended capability. Both
// layouts and subsequent complete versions are accepted; optional fields
// must be wholly within structSize.
static_assert(std::is_standard_layout_v<ProviderFunctions> &&
              std::is_trivially_copyable_v<ProviderFunctions>);
inline constexpr std::size_t ProviderFunctionsV1Size =
    offsetof(ProviderFunctions, tryBeginNativeDataLeaseV2);
inline constexpr std::size_t ProviderFunctionsV2Size =
    offsetof(ProviderFunctions, beginMemoryMutationV3);
inline constexpr std::size_t ProviderFunctionsV3Size =
    offsetof(ProviderFunctions, configureMemoryHandoff);
inline constexpr std::size_t ProviderFunctionsV4Size =
    offsetof(ProviderFunctions, setHostCallDiagnostic);

// Select before the first ABI call, which permanently locks the provider even
// if its initialization fails. Repeating the same selection is idempotent.
// An unavailable valid choice is recorded: later ABI calls deny it and never
// use FEX as a fallback. An invalid name changes no state.
SelectionResult Select(Provider provider) noexcept;
SelectionResult Select(const char* name) noexcept;
Provider Selected() noexcept;
const char* ProviderName(Provider provider) noexcept;
const char* ProviderName() noexcept;
bool SelectionLocked() noexcept;
bool Available(Provider provider) noexcept;

// Copy a reviewed process-lifetime function table before selection is locked.
// Missing production functions, unknown ABI/provider or replacement tables
// are rejected. Diagnostics may be absent and then return failure/zero.
bool RegisterProvider(const ProviderFunctions& functions) noexcept;

// Logical guest-memory permission proof, registered by the engine's VM owner
// on either host. This authorizes no access by itself: the provider must hold
// its execution/mutation lease and validate the complete requested interval.
using MemoryAccessQuery = std::uint32_t (*)(std::uint64_t address,
    std::uint64_t size, std::uint32_t write) noexcept;
bool RegisterMemoryAccessQuery(MemoryAccessQuery query) noexcept;
MemoryAccessQuery GetMemoryAccessQuery() noexcept;

// The engine's authoritative HLE registry supplies immutable effects. Missing
// metadata, revoked authority or an absent query always select ordinary HLE
// admission. Registration is process-lifetime and cannot replace a query.
using HostCallEffectsQuery = HostCallEffectMask (*)(std::uint64_t target) noexcept;
bool RegisterHostCallEffectsQuery(HostCallEffectsQuery query) noexcept;
HostCallEffectMask QueryHostCallEffects(std::uint64_t target) noexcept;
// One atomic policy snapshot avoids a second registry lookup for effects and
// contract. Missing query preserves the effects ABI but confers no capability.
using HostCallAdmissionPolicyQueryV1 = HostCallAdmissionPolicyV1 (*)(std::uint64_t target) noexcept;
bool RegisterHostCallAdmissionPolicyQueryV1(HostCallAdmissionPolicyQueryV1 query) noexcept;
HostCallAdmissionPolicyV1 QueryHostCallAdmissionPolicyV1(std::uint64_t target) noexcept;
// Thread-local protocol witness only; it grants no pointer permission itself.
bool OwnsShortHostCallAdmission() noexcept;
// Bridges discovering asynchronous guest work downgrade a short admission
// before consuming it. Ordinary admissions and other providers are unchanged.
bool EnsureOrdinaryHostCallAdmission() noexcept;

// Private FEX adapter; obtaining the table never initializes the translator.
const ProviderFunctions& FexProviderFunctions() noexcept;
#if defined(LUMEN_WITH_QEMU_TCG) && LUMEN_WITH_QEMU_TCG
const ProviderFunctions& QemuProviderFunctions() noexcept;
#endif

} // namespace Lumen::GuestCpu
