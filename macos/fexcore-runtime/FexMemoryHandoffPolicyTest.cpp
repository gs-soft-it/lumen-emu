// SPDX-License-Identifier: GPL-2.0-only
// Shared host fixture: original guest bytes, no renderer or game inputs.
#include "AndroidFexCoreRuntime.h"
#include "GuestCpuRuntime.h"
#include "MemoryHandoff.h"
#include <FEXCore/Utils/LumenMemoryHandoffAbi.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>

extern "C" const std::uint32_t* FEXLumenGuestProgressFlagProviderV1() noexcept;
extern "C" auto FEXLumenGuestProgressCallbackProviderV1() noexcept -> void (*)() noexcept;

#define REQUIRE(expression) do { if (!(expression)) { std::fprintf(stderr, "policy failure line=%d\n", __LINE__); return 1; } } while (false)
namespace {
std::uint64_t code {}, data {}, page {};
bool QueryHandoff(std::uint64_t, std::uint64_t, Lumen::GuestCpu::MemoryAccessIntent) noexcept { return true; }
std::uint32_t QueryData(std::uint64_t address, std::uint64_t bytes, std::uint32_t write) noexcept {
    if (write > 1 || bytes == 0) return 0;
    if (address >= data && bytes <= 2 * page && address - data <= 2 * page - bytes) return 1;
    return !write && address >= code && bytes <= page && address - code <= page - bytes;
}
std::uint32_t QueryCode(void*, std::uint64_t address, std::uint64_t* base,
    std::uint64_t* size, std::uint32_t* writable) noexcept {
    if (address < code || address - code >= page) return 0;
    *base = code; *size = page; *writable = 0; return 1;
}
void HleTarget() {}
unsigned hle_calls {};
Lumen::GuestCpu::HostCallAdmissionPolicyV1 QueryHlePolicy(std::uint64_t target) noexcept {
    using namespace Lumen::GuestCpu;
    return target == reinterpret_cast<std::uint64_t>(HleTarget) ?
        MakeHostCallAdmissionPolicy(HostCallEffect::GuestMemory, HostCallShortContract::SynchronousGuestMemory) :
        MakeHostCallAdmissionPolicy(HostCallEffect::Unknown);
}
std::uint32_t HostCall(void*, std::uint64_t target, std::uint64_t* gprs, std::uint64_t* next) noexcept {
    if (target != reinterpret_cast<std::uint64_t>(HleTarget) || gprs == nullptr || next == nullptr ||
        QueryData(gprs[4], 8, 0) == 0) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    std::memcpy(next, reinterpret_cast<const void*>(gprs[4]), 8);
    gprs[4] += 8;
    gprs[0] = Lumen::GuestCpu::OwnsShortHostCallAdmission() ? 1U : 0U;
    ++hle_calls;
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
}
}
int main(int argc, char** argv) {
    REQUIRE(argc == 2);
    setenv("LUMEN_FEX_TRANSLATION_CACHE", "0", 1);
    bool interval = true;
    REQUIRE(!Lumen::GuestCpu::ExclusiveMemoryPublicationFrozen());
    REQUIRE(LumenAndroidFexCoreConfigureMemoryHandoffV1(99) == 0);
    if (std::strcmp(argv[1], "exclusive") == 0 || std::strcmp(argv[1], "interval") == 0) {
        interval = std::strcmp(argv[1], "interval") == 0;
        REQUIRE(LumenAndroidFexCoreConfigureMemoryHandoffV1(interval ? 1 : 0) == 1);
        REQUIRE(LumenAndroidFexCoreConfigureMemoryHandoffV1(interval ? 0 : 1) == 0);
    } else if (std::strcmp(argv[1], "query-first") == 0) {
        REQUIRE(Lumen::GuestCpu::RegisterMemoryHandoffQuery(QueryHandoff));
        REQUIRE(LumenAndroidFexCoreConfigureMemoryHandoffV1(0) == 0);
    } else if (std::strcmp(argv[1], "provider-first") == 0) {
        REQUIRE(FEXLumenGuestMemoryCheckProviderV1() != nullptr);
        REQUIRE(LumenAndroidFexCoreConfigureMemoryHandoffV1(0) == 0);
    } else if (std::strcmp(argv[1], "race") == 0) {
        std::atomic<bool> start {};
        std::array<std::uint32_t, 2> accepted {};
        std::thread first([&] { while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            accepted[0] = LumenAndroidFexCoreConfigureMemoryHandoffV1(0); });
        std::thread second([&] { while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            accepted[1] = LumenAndroidFexCoreConfigureMemoryHandoffV1(1); });
        start.store(true, std::memory_order_release); first.join(); second.join();
        REQUIRE(accepted[0] + accepted[1] == 1);
        interval = accepted[1] != 0;
    } else REQUIRE(std::strcmp(argv[1], "default") == 0);

    page = static_cast<std::uint64_t>(getpagesize());
    code = reinterpret_cast<std::uintptr_t>(mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    data = reinterpret_cast<std::uintptr_t>(mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(code != UINTPTR_MAX && data != UINTPTR_MAX);
    // INC qword [RDI]; DEC RCX; JNZ -8; MOV RAX,[RDI]; RET.
    constexpr std::array<std::uint8_t, 12> bytes {0x48,0xff,0x07,0x48,0xff,0xc9,0x75,0xf8,0x48,0x8b,0x07,0xc3};
    std::memcpy(reinterpret_cast<void*>(code), bytes.data(), bytes.size());
    constexpr std::array<std::uint8_t, 10> hlePrefix {0x48,0x83,0xec,0x08,0x48,0xb8,0,0,0,0};
    constexpr std::array<std::uint8_t, 7> hleTail {0xff,0xd0,0x48,0x83,0xc4,0x08,0xc3};
    const auto target = reinterpret_cast<std::uint64_t>(HleTarget);
    std::memcpy(reinterpret_cast<void*>(code + 128), hlePrefix.data(), 6);
    std::memcpy(reinterpret_cast<void*>(code + 134), &target, 8);
    std::memcpy(reinterpret_cast<void*>(code + 142), hleTail.data(), hleTail.size());
    REQUIRE(mprotect(reinterpret_cast<void*>(code), page, PROT_READ | PROT_EXEC) == 0);
#if defined(__APPLE__)
    REQUIRE(LumenMacOSFexCoreRegisterDataAccessQueryV1(QueryData) == 1);
#else
    REQUIRE(Lumen::GuestCpu::RegisterMemoryAccessQuery(QueryData));
#endif
    REQUIRE(Lumen::GuestCpu::RegisterHostCallAdmissionPolicyQueryV1(QueryHlePolicy));
    LumenAndroidFexCoreRuntimeStatusV1 runtime {};
    REQUIRE(LumenAndroidFexCoreRuntimeProbeV1(&runtime) == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1);
    REQUIRE(LumenAndroidFexCoreMemoryHandoffInstrumentationReadyV1() == (interval ? 1U : 0U));
    REQUIRE(Lumen::GuestCpu::ExclusiveMemoryPublicationFrozen() == !interval);
    REQUIRE((FEXLumenGuestMemoryCheckProviderV1() != nullptr) == interval);
    REQUIRE((FEXLumenGuestMemoryReadGuardProviderV1() != nullptr) == interval);
    REQUIRE((FEXLumenGuestProgressFlagProviderV1() != nullptr) == interval);
    REQUIRE((FEXLumenGuestProgressCallbackProviderV1() != nullptr) == interval);
    REQUIRE(LumenAndroidFexCoreConfigureMemoryHandoffV1(interval ? 0 : 1) == 0);
    REQUIRE(LumenAndroidFexCoreConfigureMemoryHandoffV1(interval ? 1 : 0) == 1);
    REQUIRE(Lumen::GuestCpu::RegisterMemoryHandoffQuery(QueryHandoff) == interval);
    REQUIRE(LumenAndroidFexCoreRegisterHostCallTargetV1(target) == 1);
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        const std::uint64_t zero = 0, root = LumenAndroidFexCoreRootReturnAddressV1();
        REQUIRE(root != 0);
        std::memcpy(reinterpret_cast<void*>(data), &zero, 8);
        LumenAndroidFexCoreGuestStateV1 guest {};
        guest.structSize = sizeof(guest); guest.abiVersion = 1; guest.rip = code;
        guest.gprs[1] = 64; guest.gprs[7] = data; guest.gprs[4] = data + 2 * page - 8;
        std::memcpy(reinterpret_cast<void*>(guest.gprs[4]), &root, 8);
        REQUIRE(LumenAndroidFexCoreRunGuestV1(&guest, HostCall, QueryCode, nullptr) == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1);
        std::uint64_t result {}; std::memcpy(&result, reinterpret_cast<void*>(data), 8);
        REQUIRE(result == 64 && guest.gprs[0] == 64);
    }
    const auto* selector = std::getenv("LUMEN_EXPERIMENTAL_FEX_HLE_SYNC_MEMORY_SHORT");
    const bool expectShort = !interval && (selector == nullptr || std::strcmp(selector, "1") == 0);
    for (unsigned repeat = 0; repeat != 2; ++repeat) {
        const auto root = LumenAndroidFexCoreRootReturnAddressV1();
        LumenAndroidFexCoreGuestStateV1 guest {};
        guest.structSize = sizeof(guest); guest.abiVersion = 1; guest.rip = code + 128;
        guest.gprs[4] = data + 2 * page - 8;
        std::memcpy(reinterpret_cast<void*>(guest.gprs[4]), &root, 8);
        REQUIRE(LumenAndroidFexCoreRunGuestV1(&guest, HostCall, QueryCode, nullptr) == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1);
        REQUIRE(guest.gprs[0] == (expectShort ? 1U : 0U));
    }
    REQUIRE(hle_calls == 2);
    std::printf("PASS memory_handoff_policy mode=%s effective=%u guest_cold_warm=64 immutable=1 sync_memory_short=%u hle_calls=2\n",
        argv[1], interval ? 1U : 0U, expectShort ? 1U : 0U);
    return 0;
}
