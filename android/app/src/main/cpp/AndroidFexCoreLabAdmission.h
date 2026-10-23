#pragma once

#include <cstdint>

namespace Lumen::Android::FexCoreLab {

inline constexpr std::int32_t kMinimumAndroidApi = 35;
constexpr bool IsSupportedHostPageSize(std::int64_t hostPageSize) {
    return hostPageSize == 4096 || hostPageSize == 16384;
}

struct AdmissionInputs {
    bool fexBuild;
    bool fullRuntimeBuild;
    bool aarch64Host;
    std::int32_t androidApi;
    std::int64_t hostPageSize;
};

struct AdmissionReceipt {
    AdmissionInputs inputs;
    bool admitted;
};

constexpr bool IsAdmissionAllowed(const AdmissionInputs& inputs) {
    return inputs.fexBuild &&
           inputs.fullRuntimeBuild &&
           inputs.aarch64Host &&
           inputs.androidApi >= kMinimumAndroidApi &&
           IsSupportedHostPageSize(inputs.hostPageSize);
}

AdmissionReceipt CollectAdmissionReceipt() noexcept;

} // namespace Lumen::Android::FexCoreLab

extern "C" const char* LumenAndroidFexCoreLabAdmissionReceiptV1() noexcept;
