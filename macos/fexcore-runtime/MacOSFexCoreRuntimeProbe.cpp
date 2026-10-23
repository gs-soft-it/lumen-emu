#include "AndroidFexCoreRuntime.h"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
  const bool expectDenied = argc == 2 &&
    std::strcmp(argv[1], "--expect-admission-denied") == 0;
  const bool expectJitDenied = argc == 2 &&
    std::strcmp(argv[1], "--expect-jit-denied") == 0;
  const bool expectConfigurationDenied = argc == 2 &&
    std::strcmp(argv[1], "--expect-configuration-denied") == 0;
  if (argc > 2 || (argc == 2 && !expectDenied && !expectJitDenied && !expectConfigurationDenied)) {
    std::fprintf(stderr,
      "usage: %s [--expect-admission-denied|--expect-jit-denied|--expect-configuration-denied]\n", argv[0]);
    return 2;
  }
  LumenAndroidFexCoreRuntimeStatusV1 status {};
  const std::uint32_t code = LumenAndroidFexCoreRuntimeProbeV1(&status);
  std::printf(
    "LUMEN_MACOS_FEXCORE_RUNTIME_PROBE_V1 ready=%u code=%u abi=%u "
    "hostPageSize=%u mapJit=%u jitExecuted=%u context=%u signals=%u "
    "bridge=%u route=%u guestExecuted=%u logicalCores=%u "
    "guestResult=0x%016llx fallback=0 nodaRoute=0\n",
    code == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 ? 1U : 0U,
    code,
    status.abiVersion,
    status.hostPageSize,
    status.rwxMapped,
    status.rwxExecuted,
    status.contextInitialized,
    status.signalStopInstalled,
    status.hostBridgeExecuted,
    status.backendRouteReady,
    status.guestExecuted,
    status.logicalCores,
    static_cast<unsigned long long>(status.guestResult));
  if (expectDenied) {
    return code == LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1 ? 0 : 1;
  }
  if (expectJitDenied) {
    return code == LUMEN_ANDROID_FEXCORE_RUNTIME_RWX_DENIED_V1 &&
      status.contextInitialized == 0 && status.rwxExecuted == 0 ? 0 : 1;
  }
  if (expectConfigurationDenied)
    return code == LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1 ? 0 : 1;
  return code == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 ? 0 : 1;
}
