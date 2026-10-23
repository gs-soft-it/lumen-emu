#pragma once

#include <cstdint>
#include <cstdlib>

namespace Lumen::FexCore {

// These select existing FEX instruction lowerings. Availability must come
// from the host's capability API, never a CPU model or a guest identity.
struct NativeHostFeatures {
    bool atomics {};
    bool flagM {};
    bool flagM2 {};
    bool frintts {};
};

constexpr bool ObservedHostCapability(bool querySucceeded,
                                      std::uint64_t value) noexcept {
    return querySucceeded && value == 1;
}

constexpr bool NativeHostFeaturesRequested(const char* value) noexcept {
    return value == nullptr || (value[0] == '1' && value[1] == '\0');
}

inline bool NativeHostFeaturesEnabled() noexcept {
    static const bool enabled = NativeHostFeaturesRequested(std::getenv(
        "LUMEN_EXPERIMENTAL_FEX_NATIVE_HOST_FEATURES"));
    return enabled;
}

constexpr NativeHostFeatures SelectNativeHostFeatures(
        NativeHostFeatures available, bool enabled) noexcept {
    return enabled ? available : NativeHostFeatures {};
}

} // namespace Lumen::FexCore
