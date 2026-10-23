#ifndef LUMEN_ANDROID_DYNAREC_PROBE_H_
#define LUMEN_ANDROID_DYNAREC_PROBE_H_

#include <cstdint>

enum class LumenAndroidDynarecProbeCodeV1 : uint32_t
{
    ProviderDisabled = 0,
    Passed = 1,
    UnsupportedHost = 2,
    UnsupportedPageSize = 3,
    LoweringFailed = 4,
    CompilationFailed = 5,
    InvalidExecutableMapping = 6,
    ForkFailed = 7,
    ExecutionTimedOut = 8,
    ExecutionFailed = 9,
    TeardownFailed = 10,
};

struct LumenAndroidDynarecProbeStatusV1
{
    LumenAndroidDynarecProbeCodeV1 code =
        LumenAndroidDynarecProbeCodeV1::ProviderDisabled;
    const char* provider = "disabled";
    const char* reason = "provider-disabled";
    uint32_t compiled = 0;
    uint32_t passed = 0;
    uint32_t hostPageSize = 0;
    uint32_t backendStatus = 0;
    uint32_t mappingReadable = 0;
    uint32_t mappingWritable = 0;
    uint32_t mappingExecutable = 0;
    uint32_t targetMappingRxOnly = 0;
    uint32_t childExitCode = 0;
    uint32_t childSignal = 0;
};

LumenAndroidDynarecProbeStatusV1 LumenAndroidDynarecProbeV1();

#endif // LUMEN_ANDROID_DYNAREC_PROBE_H_
