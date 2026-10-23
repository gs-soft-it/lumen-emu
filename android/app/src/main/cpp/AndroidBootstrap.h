#ifndef LUMEN_ANDROID_BOOTSTRAP_H_
#define LUMEN_ANDROID_BOOTSTRAP_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LUMEN_ANDROID_BOOTSTRAP_ABI_VERSION_V1 1u

typedef uint32_t LumenAndroidBootstrapResultCodeV1;
enum
{
    LUMEN_ANDROID_BOOTSTRAP_OK_V1               = 0u,
    LUMEN_ANDROID_BOOTSTRAP_INVALID_ARGUMENT_V1 = 1u,
    LUMEN_ANDROID_BOOTSTRAP_UNSUPPORTED_HOST_V1 = 2u,
    LUMEN_ANDROID_BOOTSTRAP_VM_FAILED_V1        = 3u,
    LUMEN_ANDROID_BOOTSTRAP_CPU_FAILED_V1       = 4u,
    LUMEN_ANDROID_BOOTSTRAP_JIT_ENABLED_V1      = 5u,
};

typedef struct LumenAndroidBootstrapStatusV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    LumenAndroidBootstrapResultCodeV1 code;
    uint32_t runtimeAbiVersion;
    uint32_t pointerBits;
    uint32_t hostPageSize;
    uint32_t cpuOk;
    uint32_t virtualMemoryOk;
    uint32_t jitEnabled;
    uint32_t romLaunchAvailable;
    uint32_t reserved[6];
} LumenAndroidBootstrapStatusV1;

LumenAndroidBootstrapResultCodeV1 LumenAndroidBootstrapProbeV1(
    LumenAndroidBootstrapStatusV1* status);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LUMEN_ANDROID_BOOTSTRAP_H_
