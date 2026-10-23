// Lumen, 2026. SPDX-License-Identifier: GPL-2.0-only
#include "AndroidFexCoreRuntime.h"
#include "FexPerformanceTrace.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace {
std::atomic<std::uint64_t> emitterRestarts {};
}
extern "C" void FEXLumenDirectUnalignedTsoRestartV1() noexcept {
    emitterRestarts.fetch_add(1, std::memory_order_relaxed);
}

namespace {
constexpr std::uintptr_t Code = UINT64_C(0x7000000000);
constexpr unsigned char Program[] {
    0x48, 0x8b, 0x47, 0x0f, // mov rax,[rdi+15]
    0x48, 0x89, 0x47, 0x1f, // mov [rdi+31],rax
    0x48, 0x8b, 0x4f, 0x1f, // mov rcx,[rdi+31]
    0xc3,
};
constexpr std::uint64_t Value = UINT64_C(0xe1d2c3b4a5968778);
std::size_t page;
bool readOnlyPage;
constexpr unsigned DenseRepeats = 1000;
constexpr unsigned OrdinaryRepeats = 500;

std::uint32_t QueryCode(void*, std::uint64_t address, std::uint64_t* base,
    std::uint64_t* size, std::uint32_t* writable) noexcept {
    if (address < Code || address - Code >= page) return 0;
    *base = Code; *size = page; *writable = 0;
    return 1;
}
std::uint32_t QueryData(std::uint64_t address, std::uint64_t size, std::uint32_t write) noexcept {
    const auto first = Code + page;
    if (write > 1 || size == 0 || address < first || size > 4 * page ||
        address - first > 4 * page - size) return 0;
    if (write && readOnlyPage && address < Code + 4 * page && address + size > Code + 3 * page) return 0;
    return 1;
}
std::uint32_t HostCall(void*, std::uint64_t, std::uint64_t*, std::uint64_t*) noexcept {
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
}
} // namespace

int main(int argc, char** argv) {
    page = static_cast<std::size_t>(getpagesize());
    const bool dense = argc == 2 && (std::strcmp(argv[1], "--dense") == 0 ||
        std::strcmp(argv[1], "--dense-ro") == 0);
    const bool denied = argc == 2 && std::strcmp(argv[1], "--dense-ro") == 0;
    if (argc > 2 || (argc == 2 && !dense)) return 2;
    const auto* selector = std::getenv("LUMEN_FEX_MACOS_DIRECT_UNALIGNED");
    if ((selector != nullptr && std::strcmp(selector, "0") != 0 && std::strcmp(selector, "1") != 0) ||
        !Lumen::Performance::Enabled()) return 2;
    const bool direct = selector == nullptr || std::strcmp(selector, "1") == 0;
    if (dense && !direct) return 2;
    // A hint admits only a new owned mapping, never overwriting another range.
    void* mapping = mmap(reinterpret_cast<void*>(Code), 5 * page,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED || reinterpret_cast<std::uintptr_t>(mapping) != Code) {
        if (mapping != MAP_FAILED) munmap(mapping, 5 * page);
        std::fprintf(stderr, "Direct cache fixture could not reserve its fresh stable arena\n");
        return 1;
    }
    std::memset(mapping, 0xcc, page);
    std::vector<unsigned char> program;
    for (unsigned n = 0; n != (dense ? DenseRepeats : 1); ++n)
        program.insert(program.end(), std::begin(Program), std::end(Program) - 1);
    if (dense) {
        // Preserve every byte read as an input to the accumulating r11 result,
        // preventing the optimizer from eliminating the ordinary non-helper tail.
        constexpr unsigned char ordinary[] {0x44, 0x0f, 0xb6, 0x16, 0x4d, 0x01, 0xd3};
        for (unsigned n = 0; n != OrdinaryRepeats; ++n)
            program.insert(program.end(), std::begin(ordinary), std::end(ordinary));
    }
    const auto faultRip = Code + program.size();
    if (denied) {
        // Width-one store retains native STLRB lowering; its fault must recover
        // RIP from the regenerated metadata rather than the helper's explicit RIP.
        constexpr unsigned char store[] {0xc6, 0x06, 0x7f};
        program.insert(program.end(), std::begin(store), std::end(store));
    }
    program.push_back(0xc3);
    if (program.size() > page) return 2;
    std::memcpy(mapping, program.data(), program.size());
    auto* payload = reinterpret_cast<unsigned char*>(Code + page);
    std::memset(payload, 0xa5, page);
    std::memcpy(payload + 15, &Value, sizeof(Value));
    readOnlyPage = dense;
    auto* protectedPayload = reinterpret_cast<unsigned char*>(Code + 3 * page);
    std::memset(protectedPayload, 0xa5, page);
    bool passed = mprotect(mapping, page, PROT_READ | PROT_EXEC) == 0 &&
        (!readOnlyPage || mprotect(protectedPayload, page, PROT_READ) == 0);
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    passed = passed && LumenMacOSFexCoreRegisterDataAccessQueryV1(QueryData) == 1 &&
        LumenAndroidFexCoreRuntimeProbeV1(&status) == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1;
    const auto signalBefore = Lumen::Performance::data.alignmentFaults.load();
    const auto directBefore = Lumen::Performance::data.directUnalignedCalls.load();
    const auto handledBefore = Lumen::Performance::data.directUnalignedHandled.load();
    const auto restartBefore = emitterRestarts.load();
    LumenAndroidFexCoreGuestStateV1 guest {};
    guest.structSize = sizeof(guest); guest.abiVersion = 1; guest.rip = Code;
    guest.gprs[4] = Code + 5 * page - 8; guest.gprs[7] = Code + page;
    guest.gprs[6] = Code + 3 * page;
    const auto root = LumenAndroidFexCoreRootReturnAddressV1();
    std::memcpy(reinterpret_cast<void*>(guest.gprs[4]), &root, sizeof(root));
    const auto result = passed ? LumenAndroidFexCoreRunGuestV1(&guest, HostCall, QueryCode, nullptr)
        : LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
    bool canaries = true;
    const auto* value = reinterpret_cast<const unsigned char*>(&Value);
    for (std::size_t byte = 0; byte != page; ++byte) {
        const auto expected = byte >= 15 && byte < 23 ? value[byte - 15] :
            byte >= 31 && byte < 39 ? value[byte - 31] : 0xa5;
        canaries &= payload[byte] == expected;
    }
    const auto signals = Lumen::Performance::data.alignmentFaults.load() - signalBefore;
    const auto calls = Lumen::Performance::data.directUnalignedCalls.load() - directBefore;
    const auto handled = Lumen::Performance::data.directUnalignedHandled.load() - handledBefore;
    const auto restarts = emitterRestarts.load() - restartBefore;
    bool protectedCanaries = true;
    for (std::size_t byte = 0; byte != page; ++byte) protectedCanaries &= protectedPayload[byte] == 0xa5;
    const auto accesses = 3U * (dense ? DenseRepeats : 1U);
    const bool completion = denied ? result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
        guest.faultAddress == Code + 3 * page && guest.gprs[4] == Code + 5 * page - 8 :
        result == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 && guest.rip == root && guest.gprs[4] == Code + 5 * page;
    passed = passed && completion && guest.gprs[0] == Value && guest.gprs[1] == Value &&
        (!dense || guest.gprs[11] == OrdinaryRepeats * 0xa5U) && canaries && protectedCanaries &&
        (direct ? (signals == 0 && calls == accesses && handled == accesses) : (signals == accesses && calls == 0));
    std::printf("{\"schema\":\"LUMEN_FEX_DIRECT_UNALIGNED_CACHE_V1\",\"pass\":%s,"
        "\"direct\":%u,\"selector_absent\":%s,\"signal_faults\":%llu,\"direct_calls\":%llu,\"direct_handled\":%llu,"
        "\"payload_canaries\":%s,\"protected_canaries\":%s,\"dense\":%s,\"denied\":%s,"
        "\"emitter_restarts\":%llu,\"expected_fault_rip\":\"0x%016llx\","
        "\"guest_status\":%u,\"guest_result\":\"0x%016llx\",\"gameplay_qualified\":false}\n",
        passed ? "true" : "false", direct ? 1U : 0U, selector == nullptr ? "true" : "false",
        static_cast<unsigned long long>(signals), static_cast<unsigned long long>(calls),
        static_cast<unsigned long long>(handled), canaries ? "true" : "false",
        protectedCanaries ? "true" : "false", dense ? "true" : "false", denied ? "true" : "false",
        static_cast<unsigned long long>(restarts), static_cast<unsigned long long>(faultRip), result,
        static_cast<unsigned long long>(guest.gprs[0]));
    munmap(mapping, 5 * page);
    return passed ? 0 : 1;
}
