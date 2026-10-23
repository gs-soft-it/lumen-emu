// SPDX-License-Identifier: GPL-2.0-only
#include "loader/guestExecution.h"
#include "GuestCpuRuntime.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace {
constexpr std::uint64_t ExpectedResult = 42;
void SyntheticHleTarget() {}

struct Fixture {
    std::size_t page {static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
    void* code {MAP_FAILED};
    void* guarded_stack {MAP_FAILED};
    std::uint64_t stack {};
    unsigned host_calls {};
    bool executable_allowed {true};
    bool writes_allowed {true};
    bool hle_allowed {true};
    bool stop_at_hle {};
    bool simd_ok {true};
    static inline thread_local Fixture* current {};

    Fixture() {
        if (page == 0 || page == static_cast<std::size_t>(-1)) return;
        code = mmap(nullptr, page, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        guarded_stack = mmap(nullptr, page * 3, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (code == MAP_FAILED || guarded_stack == MAP_FAILED) return;
        if (mprotect(static_cast<char*>(guarded_stack) + page, page,
                     PROT_READ | PROT_WRITE) != 0) return;
        stack = reinterpret_cast<std::uint64_t>(guarded_stack) + page;
        const std::uint64_t tls_value = 3;
        std::memcpy(reinterpret_cast<void*>(stack), &tls_value, sizeof(tls_value));

        // Synthetic x86-64 guest, never a ROM patch: align stack, pass 19 in
        // RDI and XMM0, call an explicitly registered HLE target, then add
        // GS:[0] and the scalar the HLE callback returned in XMM0.
        std::array<std::uint8_t, 49> instructions {
            0x48, 0x83, 0xec, 0x08,             // sub rsp,8
            0xbf, 0x13, 0x00, 0x00, 0x00,       // mov edi,19
            0x66, 0x0f, 0x6e, 0xc7,             // movd xmm0,edi
            0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, // movabs rax,HLE
            0xff, 0xd0,                         // call rax
            0x48, 0x83, 0xc4, 0x08,             // add rsp,8
            0x65, 0x48, 0x8b, 0x14, 0x25, 0, 0, 0, 0, // mov rdx,gs:[0]
            0x48, 0x01, 0xd0,                   // add rax,rdx
            0x66, 0x0f, 0x7e, 0xc2,             // movd edx,xmm0
            0x48, 0x01, 0xd0,                   // add rax,rdx
            0xc3,
        };
        const auto target = reinterpret_cast<std::uint64_t>(&SyntheticHleTarget);
        std::memcpy(instructions.data() + 15, &target, sizeof(target));
        std::memcpy(code, instructions.data(), instructions.size());
        if (mprotect(code, page, PROT_READ) != 0) stack = 0;
        current = this;
    }

    ~Fixture() {
        current = nullptr;
        // Invalidate translated code before releasing its source allocation.
        if (code != MAP_FAILED) {
            const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
            if (token != 0) {
                const bool unmapped = munmap(code, page) == 0;
                LumenAndroidFexCoreEndGuestMemoryMutationV1(token,
                    reinterpret_cast<std::uint64_t>(code), page, unmapped);
            }
        }
        if (guarded_stack != MAP_FAILED) munmap(guarded_stack, page * 3);
    }

    bool ContainsStack(std::uint64_t address, std::size_t size) const noexcept {
        return stack != 0 && size != 0 && size <= page && address >= stack &&
               address - stack <= page - size;
    }
    bool ContainsCode(std::uint64_t address, std::size_t size) const noexcept {
        const auto base = reinterpret_cast<std::uint64_t>(code);
        return code != MAP_FAILED && size != 0 && size <= page && address >= base &&
               address - base <= page - size;
    }
};

std::uint32_t ExecutableQuery(void* opaque, std::uint64_t address,
        std::uint64_t* base, std::uint64_t* size, std::uint32_t* writable) noexcept {
    const auto& fixture = *static_cast<Fixture*>(opaque);
    if (!fixture.executable_allowed || !fixture.ContainsCode(address, 1)) return 0;
    *base = reinterpret_cast<std::uint64_t>(fixture.code);
    *size = fixture.page;
    *writable = 0;
    return 1;
}

bool Read(void* opaque, std::uint64_t address, void* output, std::size_t size) noexcept {
    const auto& fixture = *static_cast<Fixture*>(opaque);
    if (!fixture.ContainsStack(address, size) && !fixture.ContainsCode(address, size)) return false;
    std::memcpy(output, reinterpret_cast<const void*>(address), size);
    return true;
}

bool Write(void* opaque, std::uint64_t address, const void* input, std::size_t size) noexcept {
    const auto& fixture = *static_cast<Fixture*>(opaque);
    if (!fixture.writes_allowed || !fixture.ContainsStack(address, size)) return false;
    std::memcpy(reinterpret_cast<void*>(address), input, size);
    return true;
}

#if defined(__APPLE__)
std::uint32_t DataQuery(std::uint64_t address, std::uint64_t size,
                       std::uint32_t write) noexcept {
    const auto* fixture = Fixture::current;
    return fixture != nullptr && size != 0 && write <= 1 &&
        ((fixture->ContainsStack(address, size) && (!write || fixture->writes_allowed)) ||
         (!write && fixture->ContainsCode(address, size))) ? 1U : 0U;
}
#endif

std::uint32_t HostCall(void* opaque, std::uint64_t target, std::uint64_t* registers,
        LumenAndroidFexCoreSimdViewV2* simd, std::uint64_t* next_rip) noexcept {
    auto& fixture = *static_cast<Fixture*>(opaque);
    ++fixture.host_calls;
    if (!fixture.hle_allowed || target != reinterpret_cast<std::uint64_t>(&SyntheticHleTarget) ||
        registers[7] != 19 || !Read(opaque, registers[4], next_rip, sizeof(*next_rip)))
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    if (fixture.stop_at_hle) return LUMEN_ANDROID_FEXCORE_HOST_CALL_STOP_V1;
    if (simd == nullptr || simd->registers == nullptr || simd->registerBytes != 16 ||
        simd->registerCount != 16 || simd->registerStride < simd->registerBytes)
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    std::uint32_t scalar {};
    std::memcpy(&scalar, simd->registers, sizeof(scalar));
    fixture.simd_ok &= scalar == 19;
    scalar = 1;
    std::memcpy(simd->registers, &scalar, sizeof(scalar));
    registers[0] = registers[7] * 2;
    registers[4] += sizeof(*next_rip);
    return fixture.simd_ok ? LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1 :
                            LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
}

Loader::GuestExecution::RunRequest Request(Fixture& fixture) {
    Loader::GuestExecution::RunRequest request {};
    request.state.structSize = sizeof(request.state);
    request.state.abiVersion = 1;
    request.state.rip = reinterpret_cast<std::uint64_t>(fixture.code);
    request.state.gprs[4] = fixture.stack + fixture.page - sizeof(std::uint64_t);
    request.state.gsBase = fixture.stack;
    request.host_call = HostCall;
    request.executable_query = ExecutableQuery;
    request.opaque = &fixture;
    request.read_memory = Read;
    request.write_memory = Write;
    return request;
}

bool Expect(bool condition, const char* detail) {
    if (!condition) std::fprintf(stderr, "PS5 FEX synthetic test failed: %s\n", detail);
    return condition;
}
} // namespace

int main() {
#if defined(LUMEN_PS5_FEX_C13)
    if (!Expect(LumenAndroidFexCoreConfigureMemoryHandoffV1(
                    LUMEN_FEX_MEMORY_INTERVAL_HANDOFF_V1) == 0,
                "C13 must reject uninstrumented interval publication") ||
        !Expect(LumenAndroidFexCoreConfigureMemoryHandoffV1(
                    LUMEN_FEX_MEMORY_EXCLUSIVE_PUBLICATION_V1) == 1,
                "C13 requires explicit exclusive publication")) return 1;
#endif
    if (!Expect(!Loader::GuestExecution::FullRuntimeReady(),
                "synthetic fixture must not publish PS5 game runtime readiness")) return 1;
#if defined(__APPLE__)
    if (!LumenMacOSFexCoreRegisterDataAccessQueryV1(DataQuery)) return 1;
#endif
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    if (LumenAndroidFexCoreRuntimeProbeV1(&status) != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 ||
        !LumenAndroidFexCoreRegisterHostCallTargetV1(
            reinterpret_cast<std::uint64_t>(&SyntheticHleTarget))) return 1;
    Fixture fixture;
    if (!Expect(fixture.stack != 0, "guarded stack and immutable x86 source allocation")) return 1;
    const auto result = Loader::GuestExecution::Run(Request(fixture));
    bool ok = Expect(result.completed && result.code == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
        result.state.gprs[0] == ExpectedResult &&
        result.state.rip == LumenAndroidFexCoreRootReturnAddressV1() &&
        fixture.host_calls == 1 && fixture.simd_ok,
        "x86-64 HLE, SIMD, GS TLS and root return");
    std::printf("LUMEN_PS5_CPU_SYNTHETIC_V1 backend=%s code=%u completed=%u result=%llu "
        "hleCalls=%u simd=%u hostPageSize=%u commercialGameQualified=0\n",
        Lumen::GuestCpu::ProviderName(), result.code, result.completed,
        static_cast<unsigned long long>(result.state.gprs[0]),
        fixture.host_calls, fixture.simd_ok, status.hostPageSize);

    fixture.executable_allowed = false;
    auto denied = Loader::GuestExecution::Run(Request(fixture));
    ok &= Expect(!denied.completed && denied.code ==
        LUMEN_ANDROID_FEXCORE_RUNTIME_EXECUTABLE_RANGE_DENIED_V1 && fixture.host_calls == 1,
        "unproved executable range denied before HLE");
    fixture.executable_allowed = true;
    fixture.writes_allowed = false;
    denied = Loader::GuestExecution::Run(Request(fixture));
    ok &= Expect(!denied.completed && denied.code != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
        fixture.host_calls == 1, "unproved root stack writes denied before execution");
    fixture.writes_allowed = true;
    fixture.hle_allowed = false;
    denied = Loader::GuestExecution::Run(Request(fixture));
    ok &= Expect(!denied.completed && denied.code ==
        LUMEN_ANDROID_FEXCORE_RUNTIME_HOST_CALL_FAILED_V1 && fixture.host_calls == 2,
        "HLE rejection cannot report successful guest completion");
    fixture.hle_allowed = true;
    fixture.stop_at_hle = true;
    const auto stopped = Loader::GuestExecution::Run(Request(fixture));
    ok &= Expect(stopped.code == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
        stopped.stopped && !stopped.completed && fixture.host_calls == 3,
        "explicit HLE stop remains distinct from guest root return");
    ok &= Expect(!Loader::GuestExecution::FullRuntimeReady(),
        "CPU/HLE coverage cannot qualify the complete PS5 runtime");
    std::printf("LUMEN_PS5_FEX_SYNTHETIC_DENIALS_V1 passed=%u\n", ok);
    return ok ? 0 : 1;
}
