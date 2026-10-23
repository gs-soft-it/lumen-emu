#include "AndroidBootstrap.h"

#include "CpuX64.h"
#include "Emulator/Runtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#if defined(LUMEN_ANDROID_DYNAREC_PROVIDER_DISABLED) && \
    defined(LUMEN_ANDROID_DYNAREC_PROVIDER_AARCH64_WX)
#error "Android dynarec providers are mutually exclusive"
#elif !defined(LUMEN_ANDROID_DYNAREC_PROVIDER_DISABLED) && \
    !defined(LUMEN_ANDROID_DYNAREC_PROVIDER_AARCH64_WX)
#error "An Android dynarec provider must be selected explicitly"
#endif

namespace {

static_assert(sizeof(void*) == 8, "the Android spike requires a 64-bit process");
static_assert(sizeof(LumenRuntimeCapabilitiesV1) == 64, "runtime capability ABI drift");
static_assert(sizeof(LumenRuntimeResultV1) == 524, "runtime result ABI drift");
static_assert(sizeof(LumenRuntimeEventV1) == 212, "runtime event ABI drift");
static_assert(sizeof(LumenRuntimeLaunchRequestV1) == 56, "runtime launch ABI drift");
static_assert(sizeof(LumenAndroidBootstrapStatusV1) == 64, "Android bootstrap ABI drift");

bool IsSupportedHost()
{
#if defined(__aarch64__) && defined(__LP64__) && defined(__BYTE_ORDER__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return true;
#else
    return false;
#endif
}

bool IsPowerOfTwo(uint64_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

} // namespace

extern "C" LumenAndroidBootstrapResultCodeV1 LumenAndroidBootstrapProbeV1(
    LumenAndroidBootstrapStatusV1* status)
{
    if (status == nullptr)
    {
        return LUMEN_ANDROID_BOOTSTRAP_INVALID_ARGUMENT_V1;
    }

    *status                      = {};
    status->structSize           = sizeof(*status);
    status->abiVersion           = LUMEN_ANDROID_BOOTSTRAP_ABI_VERSION_V1;
    status->runtimeAbiVersion    = LUMEN_RUNTIME_ABI_VERSION_V1;
    status->pointerBits          = sizeof(void*) * 8u;
    status->romLaunchAvailable   = 0u;
    status->jitEnabled           = 0u;

    if (!IsSupportedHost())
    {
        status->code = LUMEN_ANDROID_BOOTSTRAP_UNSUPPORTED_HOST_V1;
        return status->code;
    }
    const long native_page_size = sysconf(_SC_PAGESIZE);
    if (native_page_size < 4096 ||
        !IsPowerOfTwo(static_cast<uint64_t>(native_page_size)))
    {
        status->code = LUMEN_ANDROID_BOOTSTRAP_VM_FAILED_V1;
        return status->code;
    }
    status->hostPageSize = static_cast<uint32_t>(native_page_size);

    void* mapping = mmap(nullptr, static_cast<size_t>(native_page_size),
                         PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                         -1, 0);
    if (mapping == MAP_FAILED)
    {
        status->code = LUMEN_ANDROID_BOOTSTRAP_VM_FAILED_V1;
        return status->code;
    }

    // MOV RAX, 0x1122334455667788; RET
    constexpr std::array<uint8_t, 11> guest_code = {
        0x48, 0xb8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xc3,
    };
    constexpr uint64_t expected_result = 0x1122334455667788ull;
    std::memcpy(mapping, guest_code.data(), guest_code.size());

    const bool protected_read_only =
        mprotect(mapping, static_cast<size_t>(native_page_size), PROT_READ) == 0;

    uint64_t return_address = 0;
    Lumen::Cpu::X64State state {};
    state.rip    = reinterpret_cast<uint64_t>(mapping);
    state.gpr[4] = reinterpret_cast<uint64_t>(&return_address);
    const Lumen::Cpu::X64RunResult run_result =
        protected_read_only
            ? Lumen::Cpu::X64Interpreter {}.Run(&state, 4)
            : Lumen::Cpu::X64RunResult::MemoryFault;
    const bool cpu_ok = run_result == Lumen::Cpu::X64RunResult::Returned &&
                        state.rip == 0 && state.gpr[0] == expected_result;

    const bool restored =
        mprotect(mapping, static_cast<size_t>(native_page_size),
                 PROT_READ | PROT_WRITE) == 0;
    const bool freed =
        munmap(mapping, static_cast<size_t>(native_page_size)) == 0;

    status->cpuOk           = cpu_ok ? 1u : 0u;
    status->virtualMemoryOk = protected_read_only && restored && freed ? 1u : 0u;
    if (status->virtualMemoryOk == 0u)
    {
        status->code = LUMEN_ANDROID_BOOTSTRAP_VM_FAILED_V1;
    } else if (status->cpuOk == 0u)
    {
        status->code = LUMEN_ANDROID_BOOTSTRAP_CPU_FAILED_V1;
    } else
    {
        status->code = LUMEN_ANDROID_BOOTSTRAP_OK_V1;
    }
    return status->code;
}
