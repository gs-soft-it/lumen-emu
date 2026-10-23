// Lumen PS5 integration modifications, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#include "AndroidBootstrap.h"
#include "AndroidGamePreflight.h"

#if defined(LUMEN_ANDROID_FEXCORE_LAB_BUILD)
#include "AndroidFexCoreRuntime.h"
#include "AndroidRuntimeReceipt.h"
#endif

#if defined(LUMEN_ANDROID_FULL_RUNTIME)
#include "Emulator/Runtime.h"
#endif

#include <android/log.h>
#include <cstddef>
#include <cinttypes>
#include <cstdio>
#include <jni.h>

namespace {

constexpr const char* LOG_TAG = "LumenAndroidSpike";

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_it_gssoft_lumenemu_spike_MainActivity_nativePs5RuntimeStatus(
    JNIEnv* environment, jclass)
{
#if defined(LUMEN_ANDROID_FULL_RUNTIME) && defined(LUMEN_ANDROID_FEXCORE_LAB_BUILD)
    // Loading the PS5 DSO here would conflate the two process-local FEX/SDL
    // owners. Its complete runtime is initialized only by Ps5RuntimeActivity.
    return environment->NewStringUTF("LUMEN_ANDROID_PS5_RUNTIME_V1 ready=0 "
        "engine=full process=ps5runtime initialization=deferred ps4Route=denied");
#else
    return environment->NewStringUTF("LUMEN_ANDROID_PS5_RUNTIME_V1 ready=0 linked=0 ps4Route=denied");
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_it_gssoft_lumenemu_spike_MainActivity_nativeFexCoreRuntimeStatus(
    JNIEnv* environment, jclass)
{
#if defined(LUMEN_ANDROID_FEXCORE_LAB_BUILD)
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    std::uint32_t code = LumenAndroidFexCoreRuntimeProbeV1(&status);
    std::uint32_t maximumBlockInstructions = 0;
    if (code == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) {
        const char* receipt = LumenAndroidFexCoreRuntimeReceiptV1();
        maximumBlockInstructions = receipt != nullptr
            ? Lumen::FexCore::MaximumBlockInstructionsFromReceipt(receipt) : 0;
        if (maximumBlockInstructions == 0)
            code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
    }
    char report[512] {};
    const int written = std::snprintf(
        report, sizeof(report),
        "LUMEN_ANDROID_FEXCORE_RUNTIME_V1 ready=%u code=%u abi=%u "
        "pageSize=%u rwxMapped=%u rwxExecuted=%u context=%u "
        "signalStop=%u hostBridge=%u hostBridgeExecuted=%u "
        "backendRoute=%u hostTargets=%u guestExecuted=%u "
        "opt=O2 maxInst=%u smc=mtrack-rx monoHacks=0 mutationBarrier=1 "
        "mutationSafepoint=1 mutationScopes=additive-exclusive "
        "customIRPrecompile=1 customIRRegenerate=1 rolloverProbe=1 "
        "logicalCores=%u "
        "guestResult=0x%016" PRIx64
        " pin=e17fdf9",
        code == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 ? 1u : 0u,
        static_cast<unsigned>(code),
        static_cast<unsigned>(status.abiVersion),
        static_cast<unsigned>(status.hostPageSize),
        static_cast<unsigned>(status.rwxMapped),
        static_cast<unsigned>(status.rwxExecuted),
        static_cast<unsigned>(status.contextInitialized),
        static_cast<unsigned>(status.signalStopInstalled),
        static_cast<unsigned>(status.hostBridgeReady),
        static_cast<unsigned>(status.hostBridgeExecuted),
        static_cast<unsigned>(status.backendRouteReady),
        status.registeredHostTargets != 0 ? 1u : 0u,
        static_cast<unsigned>(status.guestExecuted),
        static_cast<unsigned>(maximumBlockInstructions),
        static_cast<unsigned>(status.logicalCores), status.guestResult);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(report)) {
        return environment->NewStringUTF(
            "LUMEN_ANDROID_FEXCORE_RUNTIME_V1 ready=0 report=overflow");
    }
    __android_log_write(
        code == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1
            ? ANDROID_LOG_INFO
            : ANDROID_LOG_ERROR,
        LOG_TAG, report);
    return environment->NewStringUTF(report);
#else
    return environment->NewStringUTF(
        "LUMEN_ANDROID_FEXCORE_RUNTIME_V1 ready=0 linked=0");
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_it_gssoft_lumenemu_spike_MainActivity_nativeRunSmoke(JNIEnv* environment,
                                                          jclass)
{
    LumenAndroidBootstrapStatusV1 status {};
    const auto code = LumenAndroidBootstrapProbeV1(&status);
    char report[320] {};
    const int written = std::snprintf(
        report, sizeof(report),
        "LUMEN_ANDROID_SPIKE_V1 %s code=%u runtimeAbi=%u abi=arm64-v8a "
        "pointerBits=%u pageSize=%u cpu=%s virtual-memory=%s jit=%u "
        "romLaunch=%u",
        code == LUMEN_ANDROID_BOOTSTRAP_OK_V1 ? "PASS" : "FAIL",
        static_cast<unsigned>(code),
        static_cast<unsigned>(status.runtimeAbiVersion),
        static_cast<unsigned>(status.pointerBits),
        static_cast<unsigned>(status.hostPageSize),
        status.cpuOk != 0u ? "ok" : "fail",
        status.virtualMemoryOk != 0u ? "ok" : "fail",
        static_cast<unsigned>(status.jitEnabled),
        static_cast<unsigned>(status.romLaunchAvailable));
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(report))
    {
        return environment->NewStringUTF("LUMEN_ANDROID_SPIKE_V1 FAIL report=overflow");
    }
    __android_log_write(code == LUMEN_ANDROID_BOOTSTRAP_OK_V1
                            ? ANDROID_LOG_INFO
                            : ANDROID_LOG_ERROR,
                        LOG_TAG, report);
    return environment->NewStringUTF(report);
}

extern "C" JNIEXPORT jstring JNICALL
Java_it_gssoft_lumenemu_spike_MainActivity_nativePreflightGame(
    JNIEnv* environment, jclass, jstring gameDirectory)
{
    if (gameDirectory == nullptr)
    {
        return environment->NewStringUTF(
            "LUMEN_ANDROID_GAME_PREFLIGHT_V1 FAIL code=1");
    }

    const char* gameDirectoryChars =
        environment->GetStringUTFChars(gameDirectory, nullptr);
    if (gameDirectoryChars == nullptr)
    {
        return environment->NewStringUTF(
            "LUMEN_ANDROID_GAME_PREFLIGHT_V1 FAIL code=1");
    }

    LumenAndroidGamePreflightStatusV1 status {};
    const uint32_t code =
        LumenAndroidGamePreflightV1(gameDirectoryChars, &status);
    environment->ReleaseStringUTFChars(gameDirectory, gameDirectoryChars);

    char report[512] {};
    const int written = code == LUMEN_ANDROID_GAME_PREFLIGHT_OK_V1
        ? std::snprintf(
              report, sizeof(report),
              "LUMEN_ANDROID_GAME_PREFLIGHT_V1 PASS code=%u abi=%u "
              "profile=known-system files=%u bytes=%" PRIu64 " titleId=%s "
              "appVersion=%s contentId=%s",
              static_cast<unsigned>(code),
              static_cast<unsigned>(status.abiVersion),
              static_cast<unsigned>(status.requiredFileCount),
              status.requiredFileBytes, status.titleId, status.appVersion,
              status.contentId)
        : code == LUMEN_ANDROID_GAME_PREFLIGHT_PS5_REQUIRES_SEPARATE_RUNTIME_V1
        ? std::snprintf(report, sizeof(report),
                        "LUMEN_ANDROID_GAME_PREFLIGHT_V1 FAIL code=%u system=PS5 ps4Route=denied runtime=unavailable",
                        static_cast<unsigned>(code))
        : std::snprintf(report, sizeof(report),
                        "LUMEN_ANDROID_GAME_PREFLIGHT_V1 FAIL code=%u",
                        static_cast<unsigned>(code));
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(report))
    {
        return environment->NewStringUTF(
            "LUMEN_ANDROID_GAME_PREFLIGHT_V1 FAIL report=overflow");
    }

    __android_log_write(code == LUMEN_ANDROID_GAME_PREFLIGHT_OK_V1
                            ? ANDROID_LOG_INFO
                            : ANDROID_LOG_ERROR,
                        LOG_TAG, report);
    return environment->NewStringUTF(report);
}

extern "C" JNIEXPORT jstring JNICALL
Java_it_gssoft_lumenemu_spike_MainActivity_nativeRuntimePreflight(
    JNIEnv* environment, jclass, jstring gameDirectory,
    jstring dataDirectory)
{
#if defined(LUMEN_ANDROID_FULL_RUNTIME)
    if (gameDirectory == nullptr || dataDirectory == nullptr)
    {
        return environment->NewStringUTF(
            "LUMEN_ANDROID_RUNTIME_PREFLIGHT_V1 FAIL code=1");
    }

    const char* gameDirectoryChars =
        environment->GetStringUTFChars(gameDirectory, nullptr);
    if (gameDirectoryChars == nullptr)
    {
        return environment->NewStringUTF(
            "LUMEN_ANDROID_RUNTIME_PREFLIGHT_V1 FAIL code=1");
    }
    const char* dataDirectoryChars =
        environment->GetStringUTFChars(dataDirectory, nullptr);
    if (dataDirectoryChars == nullptr)
    {
        environment->ReleaseStringUTFChars(gameDirectory,
                                           gameDirectoryChars);
        return environment->NewStringUTF(
            "LUMEN_ANDROID_RUNTIME_PREFLIGHT_V1 FAIL code=1");
    }

    LumenRuntimeLaunchRequestV1 request {};
    request.structSize = sizeof(request);
    request.abiVersion = LUMEN_RUNTIME_ABI_VERSION_V1;
    request.installPath = gameDirectoryChars;
    request.dataPath = dataDirectoryChars;
    request.profile = LUMEN_RUNTIME_PROFILE_KNOWN_SYSTEM_V1;
    request.width = 1920;
    request.height = 1080;

    LumenRuntimeResultV1 result {};
    const auto code = LumenRuntimePreflightV1(&request, &result);
    const auto capabilities = LumenRuntimeGetCapabilitiesV1();

    environment->ReleaseStringUTFChars(dataDirectory, dataDirectoryChars);
    environment->ReleaseStringUTFChars(gameDirectory, gameDirectoryChars);

    char report[768] {};
    const int written = std::snprintf(
        report, sizeof(report),
        "LUMEN_ANDROID_RUNTIME_PREFLIGHT_V1 %s code=%u abi=%u "
        "romLaunch=%u jit=%u loader=%u graphics=%u message=%s",
        code == LUMEN_RUNTIME_RESULT_OK_V1 ? "PASS" : "FAIL",
        static_cast<unsigned>(code),
        static_cast<unsigned>(capabilities.abiVersion),
        static_cast<unsigned>(capabilities.romLaunchAvailable),
        static_cast<unsigned>(capabilities.jitEnabled),
        static_cast<unsigned>(capabilities.loaderAvailable),
        static_cast<unsigned>(capabilities.graphicsAvailable), result.message);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(report))
    {
        return environment->NewStringUTF(
            "LUMEN_ANDROID_RUNTIME_PREFLIGHT_V1 FAIL report=overflow");
    }
    __android_log_write(code == LUMEN_RUNTIME_RESULT_OK_V1
                            ? ANDROID_LOG_INFO
                            : ANDROID_LOG_ERROR,
                        LOG_TAG, report);
    return environment->NewStringUTF(report);
#else
    (void)gameDirectory;
    (void)dataDirectory;
    return environment->NewStringUTF(
        "LUMEN_ANDROID_RUNTIME_PREFLIGHT_V1 UNAVAILABLE fullRuntime=0");
#endif
}
