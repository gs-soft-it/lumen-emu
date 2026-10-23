#include "Emulator/Runtime.h"
#include "Emulator/Graphics/WindowAndroid.h"
#include "AndroidDynarecProbe.h"
#include "AndroidPs4VulkanDriver.h"
#if defined(LUMEN_ANDROID_DYNAREC_POLICY_SELECTIVE_NODA)
#include "CpuA64Dynarec.h"
#endif

#include <android/log.h>
#include <cstddef>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {

constexpr const char* LOG_TAG = "LumenAndroidRuntime";
constexpr const char* ANDROID_FEX_EVENT_POOL_SELECTOR =
    "LUMEN_EXPERIMENTAL_GFX_ANDROID_FEX_EVENT_POOL";
constexpr const char* ANDROID_FEX_PLAY_SELECTORS[] = {
    "LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE",
    "LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE",
    "LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE",
    "LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE",
    "LUMEN_EXPERIMENTAL_RUDP_OFFLINE",
    "LUMEN_EXPERIMENTAL_VOICE_OFFLINE",
    "LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE",
    "LUMEN_EXPERIMENTAL_GFX_DX10_CLAMP_ZERO",
    "LUMEN_EXPERIMENTAL_GFX_ACQUIRE_MEM_08C40000",
    "LUMEN_EXPERIMENTAL_NGS2_WAVEFORM_FRAME_INFO",
    "LUMEN_EXPERIMENTAL_NGS2_VOICE_STATE_FLAGS",
    "LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER",
    "LUMEN_EXPERIMENTAL_ULT_INITIALIZE",
    "LUMEN_EXPERIMENTAL_NETCTL_OFFLINE",
    "LUMEN_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT",
    "LUMEN_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY",
    "LUMEN_EXPERIMENTAL_GFX_RECT_LIST",
    "LUMEN_EXPERIMENTAL_GFX_V_MAC_LEGACY_F32",
    "LUMEN_EXPERIMENTAL_GFX_V_RSQ_CLAMP_F32",
    "LUMEN_EXPERIMENTAL_GFX_ANDROID_FEX_SPIRV_SIZE",
    ANDROID_FEX_EVENT_POOL_SELECTOR,
};
constexpr std::size_t ANDROID_FEX_PLAY_SELECTOR_COUNT =
    sizeof(ANDROID_FEX_PLAY_SELECTORS) /
    sizeof(ANDROID_FEX_PLAY_SELECTORS[0]);
#if defined(LUMEN_ANDROID_ARM_FASTBLOCK_TELEMETRY)
constexpr const char* FAST_BLOCK_TELEMETRY_INTERVAL = "10000000";
constexpr const char* FAST_BLOCK_TELEMETRY_LIMIT = "100000000";
#endif

bool SetEnvironment(const char* name, const char* value)
{
    return name != nullptr && value != nullptr && setenv(name, value, 1) == 0;
}

bool ConfigureCompatibilityExperiments(bool enabled)
{
#if defined(LUMEN_ANDROID_FEXCORE_LAB_BUILD)
    if (!enabled)
    {
        return true;
    }

    for (const char* name : ANDROID_FEX_PLAY_SELECTORS)
    {
        if (!SetEnvironment(name, "1"))
        {
            // This process exits immediately on failure. Do not erase an
            // inherited rollback while trying to tidy the doomed process.
            return false;
        }
    }
    return true;
#else
    return !enabled;
#endif
}

bool ConfigureAndroidFexProductDefaults(
    std::size_t* enabledSelectors, std::size_t* presentBeforeDefaults)
{
    if (enabledSelectors == nullptr || presentBeforeDefaults == nullptr)
    {
        return false;
    }
    *enabledSelectors = 0;
    *presentBeforeDefaults = 0;
    // Play applies the complete Android FEX bundle before the runtime can
    // latch any service or graphics selector. An inherited value remains an
    // exact per-selector rollback: only "1" enables it.
    for (const char* name : ANDROID_FEX_PLAY_SELECTORS)
    {
        const char* value = std::getenv(name);
        if (value != nullptr)
        {
            ++*presentBeforeDefaults;
        } else
        {
            if (setenv(name, "1", 0) != 0)
            {
                return false;
            }
            value = "1";
        }
        if (std::strcmp(value, "1") == 0)
        {
            ++*enabledSelectors;
        }
    }
    return true;
}

bool ConfigureAndroidFastBlockTelemetry()
{
#if defined(LUMEN_ANDROID_ARM_FASTBLOCK_TELEMETRY)
    return SetEnvironment("LUMEN_X64_FAST_BLOCK_TELEMETRY", "1") &&
        SetEnvironment("LUMEN_X64_FAST_BLOCK_TELEMETRY_WAIT_FOR_SIGNAL", "0") &&
        SetEnvironment("LUMEN_X64_FAST_BLOCK_TELEMETRY_INTERVAL",
                       FAST_BLOCK_TELEMETRY_INTERVAL);
#else
    return true;
#endif
}

bool ConfigureAndroidDynarec(bool* enabled)
{
    if (enabled == nullptr)
    {
        return false;
    }
    *enabled = false;
    const LumenAndroidDynarecProbeStatusV1 probe =
        LumenAndroidDynarecProbeV1();
    const char* probeOutcome = probe.compiled == 0U
        ? "SKIP"
        : (probe.passed != 0U ? "PASS" : "FAIL");
    __android_log_print(
        probe.compiled == 0U || probe.passed != 0U
            ? ANDROID_LOG_INFO
            : ANDROID_LOG_WARN,
        LOG_TAG,
        "LUMEN_ANDROID_DYNAREC_PROBE_V1 %s provider=%s code=%u "
        "compiled=%u pageSize=%u backend=%u mappingR=%u mappingW=%u "
        "mappingX=%u targetRxOnly=%u isolation=parent-publish-child-execute "
        "childExit=%u childSignal=%u reason=%s",
        probeOutcome, probe.provider, static_cast<unsigned>(probe.code),
        probe.compiled, probe.hostPageSize, probe.backendStatus,
        probe.mappingReadable, probe.mappingWritable,
        probe.mappingExecutable, probe.targetMappingRxOnly,
        probe.childExitCode, probe.childSignal, probe.reason);

#if defined(LUMEN_ANDROID_DYNAREC_POLICY_SELECTIVE_NODA)
    // Publish every restrictive selector before the master enable. A partial
    // environment update, failed W^X probe or failed isolation self-test keeps
    // the master selector at zero and therefore cannot expose native entries.
    bool environmentOk = SetEnvironment("LUMEN_X64_A64_DYNAREC", "0");
    const bool isolationOk = probe.passed != 0U &&
        Lumen::Cpu::A64Dynarec::InitializeAndroidNodaIsolation() &&
        Lumen::Cpu::A64Dynarec::ValidateAndroidNodaIsolation();
    if (probe.passed != 0U && isolationOk && environmentOk)
    {
        environmentOk =
            SetEnvironment("LUMEN_X64_FAST_BLOCKS", "1") &&
            SetEnvironment("LUMEN_X64_FAST_BLOCK_EXTENSIONS", "1") &&
            SetEnvironment("LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION", "0") &&
            SetEnvironment(
                "LUMEN_X64_A64_DYNAREC_PROFITABLE_BLOCK_ADMISSION", "0") &&
            SetEnvironment(
                "LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY", "0") &&
            SetEnvironment(
                "LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION",
                "0") &&
            SetEnvironment(
                "LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY", "0") &&
            SetEnvironment(
                "LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING", "0") &&
            SetEnvironment(
                "LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION", "0") &&
            SetEnvironment(
                "LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_STABILITY_ADMISSION",
                "0") &&
            SetEnvironment(
                "LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE", "0") &&
            SetEnvironment(
                "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION",
                "0") &&
            SetEnvironment(
                "LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE", "0") &&
            SetEnvironment(
                "LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION", "0") &&
            SetEnvironment(
                "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE", "0") &&
            SetEnvironment(
                "LUMEN_X64_A64_DYNAREC_NATIVE_ONLY_ADMISSION", "1");
    }
    if (probe.passed != 0U && isolationOk && environmentOk)
        environmentOk = SetEnvironment("LUMEN_X64_A64_DYNAREC", "1");
    *enabled = probe.passed != 0U && isolationOk && environmentOk;
    if (!*enabled)
    {
        (void)SetEnvironment("LUMEN_X64_A64_DYNAREC", "0");
        Lumen::Cpu::A64Dynarec::InvalidateAndroidNodaIsolation();
    }

    const char* configurationReason = probe.passed == 0U
        ? probe.reason
        : (!isolationOk ? "isolation-self-test-failed"
                        : (!environmentOk ? "environment-failed" : "ready"));
    __android_log_print(
        *enabled ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
        LOG_TAG,
        "LUMEN_ANDROID_DYNAREC_SEMANTIC_V1 policy=selective-noda "
        "route=fastblock-primary-native-only eligibility=direct-basic-only "
        "faultIsolation=signal-altstack-bss-tid pcRange=jit-only "
        "ownership=per-entry fatalChain=platform-siginfo-context "
        "timeout=thread-cpu-10ms "
        "noCommit=shadow oracle=per-entry-fastop-reference fallback=precise "
        "selfTest=%s",
        isolationOk ? "pass" : "fail");
    __android_log_print(
        *enabled ? ANDROID_LOG_INFO : ANDROID_LOG_WARN,
        LOG_TAG,
        "LUMEN_ANDROID_DYNAREC_V1 requested=%u probe=%s enabled=%u "
        "fallback=%u policy=selective-noda guestNative=selective-noda "
        "environment=%s reason=%s",
        probe.compiled, probeOutcome, *enabled ? 1U : 0U,
        probe.compiled != 0U && !*enabled ? 1U : 0U,
        environmentOk ? "ok" : "fail", configurationReason);
    return environmentOk;
#else
    // The W^X publication probe is isolated in a child, but guest-native blocks
    // do not yet have equivalent signal isolation. Keep the real backend linked
    // so publication can be audited while forcing all guest execution through
    // the interpreter. A SIGILL/SIGSEGV/SIGBUS must never be able to turn this
    // correctness canary into a runtime-process crash.
    const bool environmentOk =
        SetEnvironment("LUMEN_X64_A64_DYNAREC", "0");
    const char* configurationReason = probe.passed != 0U
                                          ? "guest-signal-isolation-required"
                                          : probe.reason;

    __android_log_print(
        environmentOk ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
        LOG_TAG,
        "LUMEN_ANDROID_DYNAREC_V1 requested=%u probe=%s enabled=%u "
        "fallback=%u policy=probe-only guestNative=0 "
        "environment=%s reason=%s",
        probe.compiled, probeOutcome, *enabled ? 1U : 0U,
        probe.compiled != 0U && !*enabled ? 1U : 0U,
        environmentOk ? "ok" : "fail", configurationReason);
    return environmentOk;
#endif
}

void RuntimeEvent(const LumenRuntimeEventV1* event, void*)
{
    if (event == nullptr)
    {
        return;
    }
    __android_log_print(
        event->code == LUMEN_RUNTIME_RESULT_OK_V1
            ? ANDROID_LOG_INFO
            : ANDROID_LOG_ERROR,
        LOG_TAG,
        "LUMEN_ANDROID_RUNTIME_EVENT_V1 type=%u progress=%u code=%u "
        "message=%s",
        static_cast<unsigned>(event->type),
        static_cast<unsigned>(event->progressPermille),
        static_cast<unsigned>(event->code), event->message);
    Lumen::Libs::Graphics::WindowAndroidRuntimeProgress(
        event->progressPermille, event->message);
}

bool ParseDimension(const char* value, uint32_t* dimension)
{
    if (value == nullptr || dimension == nullptr)
    {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX)
    {
        return false;
    }
    *dimension = static_cast<uint32_t>(parsed);
    return true;
}

bool ParseDiagnostics(const char* value, bool* enabled)
{
    if (value == nullptr || enabled == nullptr)
    {
        return false;
    }
    if (std::strcmp(value, "0") == 0)
    {
        *enabled = false;
        return true;
    }
    if (std::strcmp(value, "1") == 0)
    {
        *enabled = true;
        return true;
    }
    return false;
}

bool RedirectDiagnostics(const char* path)
{
    if (path == nullptr)
    {
        errno = EINVAL;
        return false;
    }

    std::fflush(stdout);
    std::fflush(stderr);
    const int diagnosticFd = open(
        path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (diagnosticFd < 0)
    {
        return false;
    }

    const bool redirected =
        dup2(diagnosticFd, STDOUT_FILENO) >= 0 &&
        dup2(diagnosticFd, STDERR_FILENO) >= 0;
    const int redirectErrno = errno;
    close(diagnosticFd);
    if (!redirected)
    {
        errno = redirectErrno;
        return false;
    }

    std::clearerr(stdout);
    std::clearerr(stderr);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    return true;
}

[[noreturn]] void ExitRuntimeProcess(LumenRuntimeResultCodeV1 code)
{
    std::fflush(stdout);
    std::fflush(stderr);
    // RuntimeActivity always lives in the dedicated :runtime process. End
    // every entry path, including preflight failure, so no one-shot native
    // state can survive in a cached child process.
    std::_Exit(code == LUMEN_RUNTIME_RESULT_OK_V1 ? 0
                                                  : static_cast<int>(code));
}

} // namespace

extern "C" __attribute__((visibility("default"), used))
int LumenAndroidSdlMainV1(int argc, char** argv)
{
    __android_log_write(ANDROID_LOG_INFO, LOG_TAG,
                        "LUMEN_ANDROID_RUNTIME_LAUNCH_V1 START");
    if (!ConfigureAndroidFastBlockTelemetry())
    {
        __android_log_print(
            ANDROID_LOG_ERROR, LOG_TAG,
            "LUMEN_ANDROID_FASTBLOCK_TELEMETRY_V1 FAIL environment errno=%d",
            errno);
        ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1);
    }
    bool dynarecEnabled = false;
    if (!ConfigureAndroidDynarec(&dynarecEnabled))
    {
        __android_log_write(
            ANDROID_LOG_ERROR, LOG_TAG,
            "LUMEN_ANDROID_RUNTIME_LAUNCH_V1 FAIL dynarec-environment");
        ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1);
    }
    const LumenRuntimeCapabilitiesV1 runtimeCapabilities =
        LumenRuntimeGetCapabilitiesV1();
    if ((runtimeCapabilities.jitEnabled != 0U) != dynarecEnabled)
    {
        __android_log_write(
            ANDROID_LOG_ERROR, LOG_TAG,
            "LUMEN_ANDROID_RUNTIME_LAUNCH_V1 FAIL dynarec-capability");
        ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1);
    }
    __android_log_print(
        ANDROID_LOG_INFO, LOG_TAG,
#if defined(LUMEN_ANDROID_DYNAREC_POLICY_SELECTIVE_NODA)
        "LUMEN_ANDROID_RUNTIME_CPU_V1 jit=%u guestNative=%s",
        runtimeCapabilities.jitEnabled,
        dynarecEnabled ? "selective-noda" : "disabled");
#else
        "LUMEN_ANDROID_RUNTIME_CPU_V1 jit=%u guestNative=disabled",
        runtimeCapabilities.jitEnabled);
#endif
    if (argc != 10 || argv == nullptr)
    {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                            "LUMEN_ANDROID_RUNTIME_LAUNCH_V1 FAIL argc=%d",
                            argc);
        ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INVALID_ARGUMENT_V1);
    }
    for (int index = 1; index < argc; ++index)
    {
        if (argv[index] == nullptr)
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                                "LUMEN_ANDROID_RUNTIME_LAUNCH_V1 FAIL null-argument=%d",
                                index);
            ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INVALID_ARGUMENT_V1);
        }
    }

    if (!LumenAndroidPs4ConfigureVulkanDriver(argv[8], argv[9]))
    {
        __android_log_write(ANDROID_LOG_ERROR, LOG_TAG,
            "LUMEN_ANDROID_RUNTIME_LAUNCH_V1 FAIL vulkan-driver fallback=denied");
        ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INVALID_ARGUMENT_V1);
    }

    uint32_t width = 0;
    uint32_t height = 0;
    bool diagnostics = false;
    bool compatibilityExperiments = false;
    if (!ParseDimension(argv[3], &width) ||
        !ParseDimension(argv[4], &height) ||
        !ParseDiagnostics(argv[5], &diagnostics) ||
        !ParseDiagnostics(argv[6], &compatibilityExperiments))
    {
        __android_log_write(ANDROID_LOG_ERROR, LOG_TAG,
                            "LUMEN_ANDROID_RUNTIME_LAUNCH_V1 FAIL dimensions");
        ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INVALID_ARGUMENT_V1);
    }

    LumenRuntimeProfileV1 profile = LUMEN_RUNTIME_PROFILE_KNOWN_SYSTEM_V1;
    if (std::strcmp(argv[7], "eboot-only") == 0)
    {
        profile = LUMEN_RUNTIME_PROFILE_EBOOT_ONLY_V1;
    } else if (std::strcmp(argv[7], "known-system") != 0)
    {
        __android_log_write(ANDROID_LOG_ERROR, LOG_TAG,
                            "LUMEN_ANDROID_RUNTIME_LAUNCH_V1 FAIL profile");
        ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INVALID_ARGUMENT_V1);
    }
    if (compatibilityExperiments &&
        profile != LUMEN_RUNTIME_PROFILE_KNOWN_SYSTEM_V1)
    {
        __android_log_write(
            ANDROID_LOG_ERROR, LOG_TAG,
            "LUMEN_ANDROID_COMPATIBILITY_EXPERIMENTS_V1 FAIL profile");
        ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INVALID_ARGUMENT_V1);
    }
    if (!ConfigureCompatibilityExperiments(compatibilityExperiments))
    {
        __android_log_print(
            ANDROID_LOG_ERROR, LOG_TAG,
            "LUMEN_ANDROID_COMPATIBILITY_EXPERIMENTS_V1 FAIL "
            "forceKnownSystem=%u errno=%d",
            compatibilityExperiments ? 1U : 0U, errno);
        ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1);
    }
    std::size_t enabledProductSelectors = 0;
    std::size_t presentBeforeProductDefaults = 0;
    if (!ConfigureAndroidFexProductDefaults(
            &enabledProductSelectors, &presentBeforeProductDefaults))
    {
        __android_log_print(
            ANDROID_LOG_ERROR, LOG_TAG,
            "LUMEN_ANDROID_FEX_PRODUCT_DEFAULTS_V1 FAIL errno=%d", errno);
        ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1);
    }
    __android_log_print(
        ANDROID_LOG_INFO, LOG_TAG,
        "LUMEN_ANDROID_FEX_PRODUCT_DEFAULTS_V1 PASS "
        "enabledSelectors=%zu presentBeforeDefaults=%zu catalogSelectors=%zu "
        "eventPool=%u "
        "exactReusePolicy=android-default-with-rollback",
        enabledProductSelectors, presentBeforeProductDefaults,
        ANDROID_FEX_PLAY_SELECTOR_COUNT,
        std::strcmp(std::getenv(ANDROID_FEX_EVENT_POOL_SELECTOR), "1") == 0
            ? 1U : 0U);
    __android_log_print(
        ANDROID_LOG_INFO, LOG_TAG,
        "LUMEN_ANDROID_COMPATIBILITY_EXPERIMENTS_V1 PASS "
        "forceKnownSystem=%u harnessSelectors=%zu catalogSelectors=%zu",
        compatibilityExperiments ? 1U : 0U,
        compatibilityExperiments
            ? ANDROID_FEX_PLAY_SELECTOR_COUNT : 0U,
        ANDROID_FEX_PLAY_SELECTOR_COUNT);
    const char* performanceProfileSelector =
        std::getenv("LUMEN_ANDROID_RUNTIME_PERFORMANCE_PROFILE");
    const bool performanceProfile = performanceProfileSelector != nullptr &&
        std::strcmp(performanceProfileSelector, "1") == 0;
    if (performanceProfile)
    {
#if !defined(LUMEN_ANDROID_FEXCORE_LAB_BUILD) || defined(NDEBUG)
        __android_log_write(
            ANDROID_LOG_ERROR, LOG_TAG,
            "LUMEN_ANDROID_RUNTIME_PERFORMANCE_PROFILE_V1 FAIL reason=unavailable");
        ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INVALID_ARGUMENT_V1);
#endif
        if (diagnostics)
        {
            __android_log_write(
                ANDROID_LOG_ERROR, LOG_TAG,
                "LUMEN_ANDROID_RUNTIME_PERFORMANCE_PROFILE_V1 FAIL "
                "reason=runtime-diagnostics-incompatible");
            ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INVALID_ARGUMENT_V1);
        }
#if defined(LUMEN_ANDROID_ARM_FASTBLOCK_TELEMETRY)
        __android_log_write(
            ANDROID_LOG_ERROR, LOG_TAG,
            "LUMEN_ANDROID_RUNTIME_PERFORMANCE_PROFILE_V1 FAIL "
            "reason=fastblock-telemetry-incompatible");
        ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INVALID_ARGUMENT_V1);
#endif
    }
#if defined(LUMEN_ANDROID_ARM_FASTBLOCK_TELEMETRY)
    if (diagnostics)
    {
        __android_log_write(
            ANDROID_LOG_ERROR, LOG_TAG,
            "LUMEN_ANDROID_FASTBLOCK_TELEMETRY_V1 FAIL "
            "reason=runtime-diagnostics-incompatible");
        ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INVALID_ARGUMENT_V1);
    }
#endif
    bool diagnosticOutputRedirected = false;
    const auto selectorEnabled = [](const char* name) {
        const char* value = std::getenv(name);
        return value != nullptr && std::strcmp(value, "1") == 0;
    };
    const bool criticalPathTrace = selectorEnabled("LUMEN_CRITICAL_PATH_TRACE");
    const bool framePipelineTrace = selectorEnabled("LUMEN_FRAME_PIPELINE_TRACE");
    const bool criticalPathDeferred = criticalPathTrace &&
        selectorEnabled("LUMEN_CRITICAL_PATH_TRACE_DEFERRED");
    if (criticalPathDeferred)
    {
        // Arm only after entering the measured scene. The marker belongs to
        // this runtime's working directory, never to the ROM or save data.
        const std::string armPath = std::string(argv[2]) + "/critical-path.arm";
        if ((unlink(armPath.c_str()) != 0 && errno != ENOENT) ||
            setenv("LUMEN_CRITICAL_PATH_ARM_FILE", armPath.c_str(), 1) != 0)
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                "LUMEN_ANDROID_FRAME_PATH_V1 FAIL arm-setup errno=%d", errno);
            ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1);
        }
    }
    if ((criticalPathTrace || framePipelineTrace) && !diagnostics && !performanceProfile)
    {
        // Keep frame-only measurement separate from the diagnostic readbacks
        // and high-volume graphics/FEX traces. Each new runtime owns its log.
        const std::string measurementLogPath = std::string(argv[2]) +
            (criticalPathTrace ? "/android-runtime-critical-path.log" : "/android-runtime-frames.log");
        if (!RedirectDiagnostics(measurementLogPath.c_str()))
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                "LUMEN_ANDROID_FRAME_PATH_V1 FAIL redirect errno=%d", errno);
            ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1);
        }
        diagnosticOutputRedirected = true;
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
            "LUMEN_ANDROID_FRAME_PATH_V1 criticalPath=%u framePipeline=%u deferred=%u "
            "runtimeDiagnostics=0 performanceProfile=0 output=%s",
            criticalPathTrace ? 1U : 0U, framePipelineTrace ? 1U : 0U,
            criticalPathDeferred ? 1U : 0U,
            measurementLogPath.c_str());
    }
#if defined(LUMEN_ANDROID_ARM_FASTBLOCK_TELEMETRY)
    const std::string fastBlockTelemetryLogPath =
        std::string(argv[2]) + "/android-fastblock-telemetry.log";
    if (!RedirectDiagnostics(fastBlockTelemetryLogPath.c_str()))
    {
        __android_log_print(
            ANDROID_LOG_ERROR, LOG_TAG,
            "LUMEN_ANDROID_FASTBLOCK_TELEMETRY_V1 FAIL redirect errno=%d",
            errno);
        ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1);
    }
    diagnosticOutputRedirected = true;
    __android_log_print(
        ANDROID_LOG_INFO, LOG_TAG,
        "LUMEN_ANDROID_FASTBLOCK_TELEMETRY_V1 PASS activation=immediate "
        "interval=%s limit=%s output=%s runtimeDiagnostics=%u",
        FAST_BLOCK_TELEMETRY_INTERVAL, FAST_BLOCK_TELEMETRY_LIMIT,
        fastBlockTelemetryLogPath.c_str(), diagnostics ? 1U : 0U);
#endif
    if (performanceProfile)
    {
        const std::string performanceLogPath =
            std::string(argv[2]) + "/android-runtime-performance.log";
        if (!RedirectDiagnostics(performanceLogPath.c_str()))
        {
            __android_log_print(
                ANDROID_LOG_ERROR, LOG_TAG,
                "LUMEN_ANDROID_RUNTIME_PERFORMANCE_PROFILE_V1 FAIL redirect errno=%d",
                errno);
            ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1);
        }
        if (!SetEnvironment("LUMEN_GFX_TIMING_TRACE", "1") ||
            !SetEnvironment("LUMEN_GFX_READBACK_PROFILE", "1"))
        {
            __android_log_print(
                ANDROID_LOG_ERROR, LOG_TAG,
                "LUMEN_ANDROID_RUNTIME_PERFORMANCE_PROFILE_V1 FAIL environment errno=%d",
                errno);
            ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1);
        }
        __android_log_print(
            ANDROID_LOG_INFO, LOG_TAG,
            "LUMEN_ANDROID_RUNTIME_PERFORMANCE_PROFILE_V1 enabled=1 output=%s "
            "gfxTiming=1 gpuReadbackProfile=1 runtimeDiagnostics=0",
            performanceLogPath.c_str());
    }
    if (diagnostics)
    {
        const std::string diagnosticLogPath =
            std::string(argv[2]) + "/android-runtime-diagnostics.log";
        if (!diagnosticOutputRedirected &&
            !RedirectDiagnostics(diagnosticLogPath.c_str()))
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                                "LUMEN_ANDROID_RUNTIME_LAUNCH_V1 FAIL diagnostic-log errno=%d",
                                errno);
            ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1);
        }
        // A single bounded source-image readback distinguishes guest-black
        // output from a Surface/compositor problem. It is opt-in because the
        // target frame performs a synchronous GPU wait and a temporary copy.
        if (setenv("LUMEN_STARTUP_TRACE", "1", 1) != 0 ||
            setenv("LUMEN_VIDEOOUT_READBACK_FRAME", "300", 1) != 0 ||
            setenv("LUMEN_VIDEOOUT_READBACK_INTERVAL", "300", 1) != 0 ||
            setenv("LUMEN_VIDEOOUT_READBACK_MAX_FRAME", "30000", 1) != 0 ||
            setenv("LUMEN_FRAME_PIPELINE_TRACE", "1", 1) != 0 ||
            // A diagnostic wrapper may explicitly disable timing while keeping
            // readback/logging active to isolate allocator corruption.
            setenv("LUMEN_GFX_TIMING_TRACE", "1", 0) != 0 ||
            setenv("LUMEN_AUDIO_OUTPUT_TRACE", "1", 1) != 0)
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                                "LUMEN_ANDROID_RUNTIME_LAUNCH_V1 FAIL diagnostics errno=%d",
                                errno);
            ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1);
        }
    }
    if (criticalPathTrace)
    {
        // All diagnostic paths above redirect fd2 to an app-owned file. Keep
        // bounded CP records there so logcat transport loss cannot erase them.
        if (!SetEnvironment("LUMEN_CRITICAL_PATH_STDERR", "1"))
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                "LUMEN_ANDROID_FRAME_PATH_V1 FAIL sink-setup errno=%d", errno);
            ExitRuntimeProcess(LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1);
        }
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
            "LUMEN_ANDROID_FRAME_PATH_V1 criticalPathSink=stderr appOwned=1");
    }
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                        "LUMEN_ANDROID_RUNTIME_DIAGNOSTICS_V1 enabled=%u "
                        "readbackStart=%u readbackInterval=%u readbackMax=%u gfxTiming=%s",
                        diagnostics ? 1U : 0U, diagnostics ? 300U : 0U,
                        diagnostics ? 300U : 0U, diagnostics ? 30000U : 0U,
                        getenv("LUMEN_GFX_TIMING_TRACE") == nullptr ? "0"
                                                                  : getenv("LUMEN_GFX_TIMING_TRACE"));

    LumenRuntimeLaunchRequestV1 request {};
    request.structSize = sizeof(request);
    request.abiVersion = LUMEN_RUNTIME_ABI_VERSION_V1;
    request.installPath = argv[1];
    request.dataPath = argv[2];
    request.profile = profile;
    request.width = width;
    request.height = height;
    request.callback = RuntimeEvent;

    LumenRuntimeResultV1 result {};
    auto code = LumenRuntimePreflightV1(&request, &result);
    __android_log_print(
        code == LUMEN_RUNTIME_RESULT_OK_V1 ? ANDROID_LOG_INFO
                                           : ANDROID_LOG_ERROR,
        LOG_TAG,
        "LUMEN_ANDROID_RUNTIME_NATIVE_PREFLIGHT_V1 %s code=%u message=%s",
        code == LUMEN_RUNTIME_RESULT_OK_V1 ? "PASS" : "FAIL",
        static_cast<unsigned>(code), result.message);
    if (code != LUMEN_RUNTIME_RESULT_OK_V1)
    {
        ExitRuntimeProcess(code);
    }

    code = LumenRuntimeLaunchV1(&request, &result);
    Lumen::Libs::Graphics::WindowAndroidRuntimeInputReady(false);
    __android_log_print(
        code == LUMEN_RUNTIME_RESULT_OK_V1 ? ANDROID_LOG_INFO
                                           : ANDROID_LOG_ERROR,
        LOG_TAG, "LUMEN_ANDROID_RUNTIME_LAUNCH_V1 %s code=%u message=%s",
        code == LUMEN_RUNTIME_RESULT_OK_V1 ? "PASS" : "FAIL",
        static_cast<unsigned>(code), result.message);
    // Success reaches this point only after cooperative guest stop and full
    // subsystem teardown; then reveal the persistent dashboard process.
    ExitRuntimeProcess(code);
}
