#include "AndroidDynarecProbe.h"

#include "CpuA64Dynarec.h"
#include "CpuX64.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <csignal>
#include <ctime>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(LUMEN_ANDROID_DYNAREC_PROVIDER_DISABLED) && \
    defined(LUMEN_ANDROID_DYNAREC_PROVIDER_AARCH64_WX)
#error "Android dynarec providers are mutually exclusive"
#elif !defined(LUMEN_ANDROID_DYNAREC_PROVIDER_DISABLED) && \
    !defined(LUMEN_ANDROID_DYNAREC_PROVIDER_AARCH64_WX)
#error "An Android dynarec provider must be selected explicitly"
#endif
#if defined(LUMEN_ANDROID_DYNAREC_PROVIDER_AARCH64_WX) && \
    !defined(__aarch64__)
#error "The aarch64-wx Android dynarec provider requires an AArch64 host"
#endif

namespace {

#if defined(LUMEN_ANDROID_DYNAREC_PROVIDER_AARCH64_WX)

struct MappingPermissions
{
    bool found = false;
    bool readable = false;
    bool writable = false;
    bool executable = false;
};

MappingPermissions ReadMappingPermissions(const void* address)
{
    MappingPermissions result {};
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr)
    {
        return result;
    }

    const uintptr_t target = reinterpret_cast<uintptr_t>(address);
    char line[512] {};
    while (std::fgets(line, sizeof(line), maps) != nullptr)
    {
        unsigned long long begin = 0;
        unsigned long long end = 0;
        char permissions[5] {};
        if (std::sscanf(line, "%llx-%llx %4s", &begin, &end,
                        permissions) != 3 ||
            target < static_cast<uintptr_t>(begin) ||
            target >= static_cast<uintptr_t>(end))
        {
            continue;
        }
        result.found = true;
        result.readable = permissions[0] == 'r';
        result.writable = permissions[1] == 'w';
        result.executable = permissions[2] == 'x';
        break;
    }
    std::fclose(maps);
    return result;
}

void SetFailure(LumenAndroidDynarecProbeStatusV1* status,
                LumenAndroidDynarecProbeCodeV1 code, const char* reason)
{
    status->code = code;
    status->reason = reason;
}

bool WaitForChild(pid_t child, int* childStatus)
{
    if (child <= 0 || childStatus == nullptr)
    {
        return false;
    }
    constexpr int attempts = 100;
    constexpr timespec interval = {0, 10 * 1000 * 1000};
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        const pid_t waited = waitpid(child, childStatus, WNOHANG);
        if (waited == child)
        {
            return true;
        }
        if (waited < 0 && errno != EINTR)
        {
            return false;
        }
        timespec remaining = interval;
        while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR)
        {
        }
    }

    // The child executes one bounded native operation. Reaching the timeout
    // is a failed canary, not workload latency.
    kill(child, SIGKILL);
    pid_t waited = -1;
    do
    {
        waited = waitpid(child, childStatus, 0);
    } while (waited < 0 && errno == EINTR);
    return false;
}

#endif

} // namespace

LumenAndroidDynarecProbeStatusV1 LumenAndroidDynarecProbeV1()
{
    LumenAndroidDynarecProbeStatusV1 result {};

#if defined(LUMEN_ANDROID_DYNAREC_PROVIDER_DISABLED)
    return result;
#else
    result.provider = "aarch64-wx";
    result.reason = "probe-not-run";
    result.compiled = 1;

    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize > 0 && pageSize <= UINT32_MAX)
    {
        result.hostPageSize = static_cast<uint32_t>(pageSize);
    }
    if (pageSize != 4096)
    {
        SetFailure(&result,
                   LumenAndroidDynarecProbeCodeV1::UnsupportedPageSize,
                   "page-size");
        return result;
    }
    if (!Lumen::Cpu::A64Dynarec::IsHostSupported())
    {
        SetFailure(&result,
                   LumenAndroidDynarecProbeCodeV1::UnsupportedHost,
                   "host-unsupported");
        return result;
    }

    constexpr uint64_t guestRip = UINT64_C(0x100000000);
    constexpr uint64_t expectedResult = UINT64_C(0x5a5a);
    Lumen::Cpu::X64Fast::FastOp operation {};
    operation.kind = Lumen::Cpu::X64Fast::FastOpKind::MovImmReg64;
    operation.dst = 0;
    operation.target = expectedResult;
    operation.length = 10;
    operation.rip = guestRip;

    Lumen::Cpu::A64Dynarec::IrBlock block {};
    auto backendStatus = Lumen::Cpu::A64Dynarec::LowerFastOps(
        &operation, 1, &block);
    result.backendStatus = static_cast<uint32_t>(backendStatus);
    if (backendStatus != Lumen::Cpu::A64Dynarec::Status::Ok)
    {
        SetFailure(&result,
                   LumenAndroidDynarecProbeCodeV1::LoweringFailed,
                   "semantic-lowering");
        return result;
    }

    const void* codeAddress = nullptr;
    {
        Lumen::Cpu::A64Dynarec::CompiledBlock compiled;
        backendStatus = compiled.Compile(
            block,
            Lumen::Cpu::A64Dynarec::EligibilityPolicy::DirectBasicBlockOnly);
        result.backendStatus = static_cast<uint32_t>(backendStatus);
        if (backendStatus != Lumen::Cpu::A64Dynarec::Status::Ok)
        {
            SetFailure(&result,
                       LumenAndroidDynarecProbeCodeV1::CompilationFailed,
                       "semantic-compile");
            return result;
        }

        codeAddress = compiled.CodeAddress();
        const MappingPermissions permissions =
            ReadMappingPermissions(codeAddress);
        result.mappingReadable = permissions.readable ? 1U : 0U;
        result.mappingWritable = permissions.writable ? 1U : 0U;
        result.mappingExecutable = permissions.executable ? 1U : 0U;
        result.targetMappingRxOnly =
            permissions.found && permissions.readable &&
                    !permissions.writable && permissions.executable
                ? 1U
                : 0U;
        if (codeAddress == nullptr || result.targetMappingRxOnly == 0U)
        {
            SetFailure(
                &result,
                LumenAndroidDynarecProbeCodeV1::InvalidExecutableMapping,
                "mapping-not-rx");
            return result;
        }

        // The parent :runtime process lowers and publishes the real semantic
        // artifact. A short-lived child inherits that exact RX page and
        // executes it, turning SIGILL/SIGSEGV/SIGBUS into a normal fallback.
        const pid_t child = fork();
        if (child < 0)
        {
            SetFailure(&result,
                       LumenAndroidDynarecProbeCodeV1::ForkFailed,
                       "fork");
            return result;
        }
        if (child == 0)
        {
            Lumen::Cpu::X64State state {};
            state.rip = guestRip;
            Lumen::Cpu::A64Dynarec::EntryContext context {};
            context.state = &state;
            context.cache_epoch = compiled.Epoch();
            context.resume_rip = guestRip;
            context.remaining_steps = 1;
            const uint32_t steps = compiled.Execute(&context);
            const bool valid =
                steps == 1 && context.executed_steps == 1 &&
                context.direct_steps == 1 &&
                context.exit_reason ==
                    Lumen::Cpu::A64Dynarec::ExitReason::Completed &&
                context.resume_rip == block.fallthrough_rip &&
                state.rip == block.fallthrough_rip &&
                state.gpr[0] == expectedResult;
            _exit(valid ? 0 : 113);
        }

        int childStatus = 0;
        if (!WaitForChild(child, &childStatus))
        {
            result.childSignal = WIFSIGNALED(childStatus)
                ? static_cast<uint32_t>(WTERMSIG(childStatus))
                : 0U;
            SetFailure(&result,
                       LumenAndroidDynarecProbeCodeV1::ExecutionTimedOut,
                       "child-timeout-or-wait");
            return result;
        }
        if (WIFEXITED(childStatus))
        {
            result.childExitCode =
                static_cast<uint32_t>(WEXITSTATUS(childStatus));
        } else if (WIFSIGNALED(childStatus))
        {
            result.childSignal =
                static_cast<uint32_t>(WTERMSIG(childStatus));
        }
        if (!WIFEXITED(childStatus) || WEXITSTATUS(childStatus) != 0)
        {
            SetFailure(&result,
                       LumenAndroidDynarecProbeCodeV1::ExecutionFailed,
                       "semantic-execute");
            return result;
        }

        if (!compiled.ResetCodeForAndroidProbe())
        {
            SetFailure(&result,
                       LumenAndroidDynarecProbeCodeV1::TeardownFailed,
                       "munmap");
            return result;
        }
    }

    result.code = LumenAndroidDynarecProbeCodeV1::Passed;
    result.reason = "ok";
    result.passed = 1;
    return result;
#endif
}
