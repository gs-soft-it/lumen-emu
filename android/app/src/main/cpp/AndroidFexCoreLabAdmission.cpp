#include "AndroidFexCoreLabAdmission.h"

#include <android/api-level.h>
#include <unistd.h>

#if !defined(LUMEN_ANDROID_FEXCORE_LAB_BUILD) || \
    LUMEN_ANDROID_FEXCORE_LAB_BUILD != 1
#error "The FEXCore lab admission receipt requires an explicit lab build"
#endif

#if !defined(LUMEN_ANDROID_FULL_RUNTIME_BUILD) || \
    LUMEN_ANDROID_FULL_RUNTIME_BUILD != 1
#error "Android FEX admission requires the full runtime"
#endif

#if defined(LUMEN_ANDROID_FEXCORE_BUILD_DEBUG) == \
    defined(LUMEN_ANDROID_FEXCORE_BUILD_RELEASE)
#error "Android FEX admission requires exactly one Debug or Release build"
#endif

#if defined(LUMEN_ANDROID_FEXCORE_BUILD_DEBUG)
#define LUMEN_ANDROID_FEXCORE_BUILD_LABEL "debug"
#else
#define LUMEN_ANDROID_FEXCORE_BUILD_LABEL "release"
#endif

#if !defined(__aarch64__)
#error "The FEXCore lab admission receipt requires an AArch64 target"
#endif

namespace Lumen::Android::FexCoreLab {
namespace {

constexpr bool kFexBuild = LUMEN_ANDROID_FEXCORE_LAB_BUILD == 1;
constexpr bool kFullRuntimeBuild = LUMEN_ANDROID_FULL_RUNTIME_BUILD == 1;
constexpr bool kAarch64Host = true;

constexpr char kAdmitted4KiBReceipt[] =
    "LUMEN_ANDROID_FEXCORE_LAB_ADMISSION_V1 admitted=1 "
    "lab=1 fullRuntime=1 build=" LUMEN_ANDROID_FEXCORE_BUILD_LABEL
    " aarch64=1 api>=35 pageSize=4096 "
    "backend=FEXCore linked=1 runtimeProbe=required";
constexpr char kAdmitted16KiBReceipt[] =
    "LUMEN_ANDROID_FEXCORE_LAB_ADMISSION_V1 admitted=1 "
    "lab=1 fullRuntime=1 build=" LUMEN_ANDROID_FEXCORE_BUILD_LABEL
    " aarch64=1 api>=35 pageSize=16384 "
    "backend=FEXCore linked=1 runtimeProbe=required";
constexpr char kDeniedReceipt[] =
    "LUMEN_ANDROID_FEXCORE_LAB_ADMISSION_V1 admitted=0 "
    "failClosed=1 backend=FEXCore linked=1 runtimeProbe=required";

static_assert(!IsAdmissionAllowed({false, true, true, 35, 4096}));
static_assert(!IsAdmissionAllowed({true, false, true, 35, 4096}));
static_assert(!IsAdmissionAllowed({true, true, false, 35, 4096}));
static_assert(!IsAdmissionAllowed({true, true, true, 34, 4096}));
static_assert(!IsAdmissionAllowed({true, true, true, 35, 65536}));
static_assert(IsAdmissionAllowed({true, true, true, 35, 4096}));
static_assert(IsAdmissionAllowed({true, true, true, 35, 16384}));

} // namespace

AdmissionReceipt CollectAdmissionReceipt() noexcept {
    const AdmissionInputs inputs {
        kFexBuild,
        kFullRuntimeBuild,
        kAarch64Host,
        android_get_device_api_level(),
        sysconf(_SC_PAGESIZE),
    };
    return {inputs, IsAdmissionAllowed(inputs)};
}

const char* SelectAdmissionReceiptMarker() noexcept {
    const auto receipt = CollectAdmissionReceipt();
    if (!receipt.admitted) {
        return kDeniedReceipt;
    }
    return receipt.inputs.hostPageSize == 16384
        ? kAdmitted16KiBReceipt
        : kAdmitted4KiBReceipt;
}

} // namespace Lumen::Android::FexCoreLab

extern "C" const char* LumenAndroidFexCoreLabAdmissionReceiptV1() noexcept {
    return Lumen::Android::FexCoreLab::SelectAdmissionReceiptMarker();
}
