// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "GuestCpuRuntime.h"

namespace Lumen::GuestCpu {
const ProviderFunctions& QemuProviderFunctions() noexcept;

// Optional memory-owner proof for the persistent IR path. It must cover all
// physical aliases and native/GPU writers under the ordinary execution lease.
// Missing/negative proof preserves ordinary JIT for that code range.
using ImmutableCodeQuery = bool (*)(std::uint64_t address, std::uint64_t size) noexcept;
bool QemuRegisterImmutableCodeQuery(ImmutableCodeQuery query) noexcept;

// Only an audited HLE caller with native snapshots/scalars and no borrowed
// guest pointers or local locks may use this synchronous fresh-root handoff.
// Failure never authorizes a callback. End readmits and revalidates the saved
// parent's continuation, stack and TLS; a failed restore denies CPU resume.
std::uint64_t QemuBeginFreshCallback(std::uint64_t stack_base,
    std::uint64_t stack_size, std::uint64_t tls_base = 0,
    std::uint64_t tls_size = 0) noexcept;
bool QemuEndFreshCallback(std::uint64_t token) noexcept;
// Recheck under an acquired writer before stack preparation and again under
// execution admission before the child CPU starts. Captured mapping lifetime
// must still exist; a replacement at the same VA cannot receive frame writes.
bool QemuValidateFreshCallbackParent() noexcept;
// VM owner calls after successful structural mapping/rights effects, while
// its epoch still excludes CPU execution. Ordinary CPU data writes do not
// revoke mapping identity. Executable writes retire the affected code lifetime;
// proven NX data-only writes need no TB/TLB or memory-view invalidation.
void QemuNotifyMappingChange(std::uint64_t base, std::uint64_t size) noexcept;

// Android's old GP-only entry does not carry an allocator-owned stack proof.
// The common adapter makes that proof explicit for TCG on either host.
std::uint32_t QemuRunWithOwnedStack(LumenAndroidFexCoreGuestStateV3* state,
    LumenAndroidFexCoreHostCallV2 host_call,
    LumenAndroidFexCoreExecutableQueryV1 executable_query, void* opaque,
    std::uint64_t owned_base, std::uint64_t owned_size,
    LumenMacOSFexCoreOwnedStackAccessQueryV1 owned_query) noexcept;
}
