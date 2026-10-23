// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "AndroidFexCoreRuntime.h"
#include <csignal>

namespace Lumen::Ps5::DirectFex {

// PS5 ABI adaptation around the unchanged Gen4 direct GuestEngine. This API
// deliberately exposes no facade epoch, lease, or generated safepoint claim.
std::uint32_t Probe(LumenAndroidFexCoreRuntimeStatusV1* status = nullptr) noexcept;
const char* Receipt() noexcept;
bool RegisterHostTarget(std::uint64_t target, const void* binding = nullptr) noexcept;
std::uint64_t ResolveHostTarget(std::uint64_t target) noexcept;
// Immutable typed-registry record for this currently active host call only.
// This is a nested-call scope, not a cache; a different target never matches.
const void* ActiveHostTargetBinding(std::uint64_t target) noexcept;
std::uint64_t RootReturn() noexcept;
// Only read-only executable pages allocated by the typed-import bridge.
bool QueryOwnedExecutable(std::uint64_t address, std::uint64_t* base, std::uint64_t* size) noexcept;

std::uint32_t RunGuestV1(LumenAndroidFexCoreGuestStateV1* state,
    LumenAndroidFexCoreHostCallV1 host_call,
    LumenAndroidFexCoreExecutableQueryV1 executable_query, void* opaque) noexcept;
std::uint32_t RunGuestV2(LumenAndroidFexCoreGuestStateV1* state,
    LumenAndroidFexCoreHostCallV2 host_call,
    LumenAndroidFexCoreExecutableQueryV1 executable_query, void* opaque) noexcept;
// The full engine supplies the usable guest stack, excluding guard pages.
// Host mappings may coalesce independent guest stacks, so their extent is not
// a stack boundary. The existing entry points remain available to CPU fixtures.
std::uint32_t RunGuestV2WithStack(LumenAndroidFexCoreGuestStateV1* state,
    LumenAndroidFexCoreHostCallV2 host_call,
    LumenAndroidFexCoreExecutableQueryV1 executable_query, void* opaque,
    std::uint64_t stack_base, std::uint64_t stack_size) noexcept;

std::uint32_t QueryGuestTsc(LumenAndroidFexCoreGuestTscV1* output) noexcept;
bool CaptureCurrentGuestState(LumenAndroidFexCoreGuestStateV1* output) noexcept;
// Diagnostic RIP reconstruction supplied by the original FEX engine. Its own
// fallback is the last spilled RIP when the PC is outside the current block.
bool QueryGuestRipFromHostPc(std::uint64_t host_pc, std::uint64_t* guest_rip) noexcept;
bool IsActive() noexcept;
bool CanCancelFromSignal() noexcept;
// Dispatch only the original translator's recoverable host faults. Returning
// false leaves GPU page-watch and ordinary host fault handling to the embedding.
bool HandleHostSignal(int signal, siginfo_t* info, void* context) noexcept;

// Architectural state at a typed HLE call, after its guest return. This is an
// embedding ABI for fibers; it never captures or restores a native C++ stack.
struct HleContext {
    std::uint64_t gprs[16] {};
    std::uint8_t xmm[16 * 16] {};
    std::uint64_t rip {};
    std::uint64_t callret_sp {};
    std::uint32_t mxcsr {};
    std::uint16_t fcw {};
    bool reset_simd {};
};
bool CaptureHleContext(HleContext* output) noexcept;
bool SwitchHleContext(const HleContext& context) noexcept;

// Acquire before the host mapping/backing mutex and never retain across GPU waits.
// Nested calls on one native thread are supported. This excludes decoder/cache
// mutation, not running guest CPUs.
bool BeginCodeMutation() noexcept;
bool EndCodeMutation(std::uint64_t begin, std::uint64_t size, bool committed) noexcept;

// The PS5 VM owner must serialize its map/protect/unmap operation. This invokes
// the actual direct FEX code-buffer and registered-thread cache invalidation;
// it does not claim to suspend every translated CPU or authorize reclamation.
bool Invalidate(std::uint64_t begin, std::uint64_t size) noexcept;

} // namespace Lumen::Ps5::DirectFex
