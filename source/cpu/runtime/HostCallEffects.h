// Lumen, 2026-10-07. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>

namespace Lumen::GuestCpu {

using HostCallEffectMask = std::uint32_t;

// Effects describe the native service body. The bridge still reads the CALL
// return address under its execution lease. Unknown never admits a light path.
enum class HostCallEffect : HostCallEffectMask {
    None = 0,
    GuestMemory = 1U << 0,
    Blocking = 1U << 1,
    GuestCallback = 1U << 2,
    RetainsGuestPointer = 1U << 3,
    VmMutation = 1U << 4,
    Unknown = 1U << 31,
};

constexpr HostCallEffectMask EffectMask(HostCallEffect effect) noexcept {
    return static_cast<HostCallEffectMask>(effect);
}
constexpr HostCallEffect operator|(HostCallEffect left, HostCallEffect right) noexcept {
    return static_cast<HostCallEffect>(EffectMask(left) | EffectMask(right));
}
constexpr bool ValidHostCallEffects(HostCallEffect effects) noexcept {
    constexpr auto known = HostCallEffect::GuestMemory | HostCallEffect::Blocking |
        HostCallEffect::GuestCallback | HostCallEffect::RetainsGuestPointer | HostCallEffect::VmMutation;
    return effects == HostCallEffect::Unknown || (EffectMask(effects) & ~EffectMask(known)) == 0;
}

// Positive native-body attestation, independent of effect classification.
// SynchronousGuestMemory permits retained-execution admission only under the
// explicit EXCLUSIVE publication protocol. It forbids guest waits, callbacks,
// retained pointers and mutation upgrades until explicit ordinary demotion.
enum class HostCallShortContract : std::uint32_t {
    Ordinary = 0,
    SynchronousGuestMemory = 1,
};
using HostCallAdmissionPolicyV1 = std::uint64_t;
constexpr HostCallAdmissionPolicyV1 MakeHostCallAdmissionPolicy(
    HostCallEffect effects, HostCallShortContract contract = HostCallShortContract::Ordinary) noexcept {
    return static_cast<std::uint64_t>(EffectMask(effects)) |
        (static_cast<std::uint64_t>(contract) << 32);
}
constexpr HostCallEffect AdmissionPolicyEffects(HostCallAdmissionPolicyV1 policy) noexcept {
    return static_cast<HostCallEffect>(static_cast<std::uint32_t>(policy));
}
constexpr HostCallShortContract AdmissionPolicyContract(HostCallAdmissionPolicyV1 policy) noexcept {
    return static_cast<HostCallShortContract>(policy >> 32);
}
constexpr bool ValidHostCallShortContract(HostCallShortContract contract) noexcept {
    return contract == HostCallShortContract::Ordinary ||
        contract == HostCallShortContract::SynchronousGuestMemory;
}

} // namespace Lumen::GuestCpu
