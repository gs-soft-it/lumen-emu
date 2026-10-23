#include "AndroidFexCoreRuntime.h"
#include "MacOSFexCoreSignalPolicy.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
struct Fixture {
    std::uint64_t code;
    std::size_t pageSize;
};
std::uint32_t Query(void* opaque, std::uint64_t address,
                    std::uint64_t* base, std::uint64_t* size,
                    std::uint32_t* writable) noexcept {
    const auto& fixture = *static_cast<Fixture*>(opaque);
    if (address < fixture.code || address - fixture.code >= fixture.pageSize) return 0;
    *base = fixture.code;
    *size = fixture.pageSize;
    *writable = 0;
    return 1;
}
std::uint32_t NoHostCall(void*, std::uint64_t, std::uint64_t*, std::uint64_t*) noexcept {
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
}
struct Handshake {
    std::atomic<std::uint32_t> value {0};
    std::atomic<std::uint32_t> finish {0};
};

struct ScalarHandshake {
    std::uint64_t target {};
    std::uintptr_t stackBase {};
    std::size_t stackSize {};
    std::uint8_t expected[16] {};
    std::uint8_t replacement[16] {};
    std::uint32_t callCount {};
};

void ScalarTargetIdentity() noexcept {
}

std::uint32_t ScalarHostCall(
    void* opaque, std::uint64_t target, std::uint64_t* gprs,
    LumenAndroidFexCoreSimdViewV2* simd,
    std::uint64_t* nextRip) noexcept {
    auto* handshake = static_cast<ScalarHandshake*>(opaque);
    if (handshake == nullptr || gprs == nullptr || nextRip == nullptr ||
        simd == nullptr || target != handshake->target ||
        simd->structSize != sizeof(*simd) || simd->registers == nullptr ||
        simd->registerCount != 16 || simd->registerBytes != 16 ||
        simd->registerStride != 16 ||
        std::memcmp(simd->registers, handshake->expected, 16) != 0) {
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    }
    const std::uint64_t rsp = gprs[4];
    if (rsp < handshake->stackBase ||
        rsp > handshake->stackBase + handshake->stackSize - sizeof(*nextRip)) {
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    }
    std::memcpy(nextRip, reinterpret_cast<const void*>(rsp), sizeof(*nextRip));
    gprs[4] += sizeof(*nextRip);
    std::memcpy(simd->registers, handshake->replacement, 16);
    ++handshake->callCount;
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
}
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<std::uint32_t>) == 4);
static_assert(offsetof(Handshake, finish) == 4);

[[noreturn]] void Fail(int code, const char* stage, int error = 0) {
    std::fprintf(stderr, "FAIL: FEX page-size stage=%s code=%d errno=%d\n", stage, code, error);
    std::fflush(stderr);
    // Failure may leave guest threads blocked. Exit the bounded test process
    // directly rather than destroying a joinable thread or waiting forever.
    std::_Exit(code);
}
}

int main() {
#if defined(LUMEN_PS5_FEX_C13)
    if (LumenAndroidFexCoreConfigureMemoryHandoffV1(
            LUMEN_FEX_MEMORY_EXCLUSIVE_PUBLICATION_V1) != 1) return 2;
#endif
    // Bound failure even if an interrupt-page regression strands a guest.
    alarm(30);
    using Lumen::Android::FexCoreLab::IsDispatcherMapErrorDoorbellFault;
    if (!IsDispatcherMapErrorDoorbellFault(true, true, true, true, true) ||
        IsDispatcherMapErrorDoorbellFault(false, true, true, true, true) ||
        IsDispatcherMapErrorDoorbellFault(true, false, true, true, true) ||
        IsDispatcherMapErrorDoorbellFault(true, true, false, true, true) ||
        IsDispatcherMapErrorDoorbellFault(true, true, true, false, true) ||
        IsDispatcherMapErrorDoorbellFault(true, true, true, true, false)) {
        Fail(1, "dispatcher-doorbell-maperr-admission");
    }
    using DoorbellDisposition =
        Lumen::Android::FexCoreLab::VendorMapErrorDoorbellDisposition;
    using Lumen::Android::FexCoreLab::
        ClassifyVendorMapErrorDoorbellFault;
    // A MAPERR already raised by the exact JIT doorbell store may be delivered
    // after Disarm or rollback publishes epoch zero.  It must reach the
    // runtime's retry branch; an ordinary or insufficiently proven MAPERR
    // remains rejected.
    if (ClassifyVendorMapErrorDoorbellFault(
            0, true, true, true, true, true, true) !=
            DoorbellDisposition::RetryLateDelivery ||
        ClassifyVendorMapErrorDoorbellFault(
            7, true, true, true, true, true, true) !=
            DoorbellDisposition::HandleArmed ||
        ClassifyVendorMapErrorDoorbellFault(
            0, true, true, true, true, false, false) !=
            DoorbellDisposition::Reject ||
        ClassifyVendorMapErrorDoorbellFault(
            0, false, true, true, true, true, true) !=
            DoorbellDisposition::Reject) {
        Fail(1, "safepoint-doorbell-late-maperr-classifier");
    }
    // The Xiaomi Tutorial crash's native FEX guard instruction is a 64-bit
    // zero store through x19, not the JIT's x28-relative doorbell opcode.
    // Verify the shared decoder accepts only the exact effective address.
    constexpr std::uint32_t kNativeGuardStore = UINT32_C(0xf920027f);
    constexpr std::uintptr_t kNativeThreadBase = UINT64_C(0x100000000);
    constexpr std::uintptr_t kNativeDoorbell =
        kNativeThreadBase + UINT32_C(0x4000);
    constexpr auto decodedNativeGuard =
        Lumen::MacOS::FexCore::DecodeStoreZeroUnsignedImmediate(
            kNativeGuardStore);
    if (!decodedNativeGuard.valid ||
        decodedNativeGuard.baseRegister != 19 ||
        decodedNativeGuard.byteOffset != UINT32_C(0x4000) ||
        !Lumen::MacOS::FexCore::StoreZeroTargetsAddress(
            kNativeGuardStore, kNativeThreadBase, kNativeDoorbell) ||
        Lumen::MacOS::FexCore::StoreZeroTargetsAddress(
            kNativeGuardStore, kNativeThreadBase, kNativeDoorbell + 8) ||
        Lumen::MacOS::FexCore::StoreZeroTargetsAddress(
            kNativeGuardStore ^ 1, kNativeThreadBase, kNativeDoorbell) ||
        Lumen::MacOS::FexCore::StoreZeroTargetsAddress(
            UINT32_C(0xb920027f), kNativeThreadBase, kNativeDoorbell)) {
        Fail(1, "native-safepoint-doorbell-store");
    }
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize != 4096 && pageSize != 16384) Fail(1, "host-page-size");
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    if (LumenAndroidFexCoreRuntimeProbeV1(&status) != 0 ||
        status.hostPageSize != static_cast<std::uint32_t>(pageSize) ||
        !status.guestExecuted || !status.hostBridgeExecuted ||
        status.guestResult != 0x12345678) {
        std::fprintf(stderr, "FEX probe failed: code=%u page=%u\n", status.code, status.hostPageSize);
        Fail(2, "runtime-probe");
    }
    // Both peers repeatedly execute the same translated instruction. Mutating
    // its immediate while they are parked must invalidate each live code cache.
    // loop: mov eax,42; mov [rdi],eax; cmp [rdi+4],0; je loop; ret
    constexpr std::uint8_t program[] = {
        0xb8, 42, 0, 0, 0, 0x89, 0x07, 0x83, 0x7f, 4, 0,
        0x74, 0xf3, 0xc3,
    };
    void* code = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) Fail(3, "map-code", errno);
    void* data = mmap(nullptr, pageSize * 4, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED) Fail(3, "map-data", errno);
    std::memcpy(code, program, sizeof(program));
    // Guest RX is host-readable data, not native executable code.
    if (mprotect(code, pageSize, PROT_READ) != 0) Fail(4, "protect-code", errno);
    Fixture fixture {reinterpret_cast<std::uintptr_t>(code), static_cast<std::size_t>(pageSize)};
    LumenAndroidFexCoreGuestStateV1 guests[2] {};
    Handshake* handshakes[2] {};
    std::uint32_t results[2] {99, 99};
    std::uint64_t initialRsp[2] {};
    std::atomic<unsigned> completed {0};
    const auto root = LumenAndroidFexCoreRootReturnAddressV1();
    if (root == 0) Fail(4, "root-sentinel");
    for (unsigned i = 0; i != 2; ++i) {
        auto* region = static_cast<std::uint8_t*>(data) + i * 2 * pageSize;
        handshakes[i] = new (region) Handshake;
        initialRsp[i] = reinterpret_cast<std::uintptr_t>(region + 2 * pageSize - 8);
        guests[i].structSize = sizeof(guests[i]);
        guests[i].abiVersion = 1;
        guests[i].rip = fixture.code;
        guests[i].gprs[4] = initialRsp[i];
        guests[i].gprs[7] = reinterpret_cast<std::uintptr_t>(handshakes[i]);
        std::memcpy(reinterpret_cast<void*>(initialRsp[i]), &root, sizeof(root));
    }
    std::thread peers[2];
    for (unsigned i = 0; i != 2; ++i) {
        peers[i] = std::thread([&, i] {
            results[i] = LumenAndroidFexCoreRunGuestV1(&guests[i], NoHostCall, Query, &fixture);
            completed.fetch_add(1);
        });
    }
    const auto waitForValue = [&](std::uint32_t expected) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (handshakes[0]->value.load() == expected &&
                handshakes[1]->value.load() == expected) return true;
            if (completed.load() != 0) return false;
            std::this_thread::yield();
        }
        return false;
    };
    if (!waitForValue(42)) Fail(5, "peers-entered-translated-loop");
    const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    if (token == 0) Fail(5, "park-two-peers");
    if (mprotect(code, pageSize, PROT_READ | PROT_WRITE) != 0) Fail(6, "live-edit-rw", errno);
    static_cast<std::uint8_t*>(code)[1] = 43;
    if (mprotect(code, pageSize, PROT_READ) != 0) Fail(7, "live-edit-rx", errno);
    LumenAndroidFexCoreEndGuestMemoryMutationV1(token, fixture.code, pageSize, 1);
    const bool retranslated = waitForValue(43);
    handshakes[0]->finish.store(1);
    handshakes[1]->finish.store(1);
    for (auto& peer : peers) peer.join();
    const auto doorbellSignalCode =
        LumenAndroidFexCoreLastSafepointSignalCodeV1();
    if (doorbellSignalCode != SEGV_MAPERR &&
        doorbellSignalCode != SEGV_ACCERR) {
        std::fprintf(stderr,
            "FEX safepoint doorbell signalCode=%u expected=%d-or-%d\n",
            doorbellSignalCode, SEGV_MAPERR, SEGV_ACCERR);
        Fail(8, "safepoint-doorbell-signal-code");
    }
    for (unsigned i = 0; i != 2; ++i) {
        if (!retranslated || results[i] != 0 || guests[i].gprs[0] != 43 ||
            guests[i].gprs[4] != initialRsp[i] + sizeof(root)) {
            std::fprintf(stderr,
                "FEX peer=%u retranslated=%d value=%u code=%u rax=%llu rsp=%llx expectedRsp=%llx\n",
                i, retranslated, handshakes[i]->value.load(), results[i],
                static_cast<unsigned long long>(guests[i].gprs[0]),
                static_cast<unsigned long long>(guests[i].gprs[4]),
                static_cast<unsigned long long>(initialRsp[i] + sizeof(root)));
            Fail(8, "live-retranslation-and-root-return");
        }
    }
    // Reuse the same address in a fresh run as well. Seed the root frame
    // explicitly rather than adjusting an unchecked returned guest RSP.
    const auto edit = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    if (edit == 0) Fail(9, "fresh-edit-barrier");
    if (mprotect(code, pageSize, PROT_READ | PROT_WRITE) != 0) Fail(9, "fresh-edit-rw", errno);
    constexpr std::uint8_t replacement[] = {0xb8, 44, 0, 0, 0, 0xc3};
    std::memcpy(code, replacement, sizeof(replacement));
    if (mprotect(code, pageSize, PROT_READ) != 0) Fail(9, "fresh-edit-rx", errno);
    LumenAndroidFexCoreEndGuestMemoryMutationV1(edit, fixture.code, pageSize, 1);
    guests[0].rip = fixture.code;
    guests[0].gprs[4] = initialRsp[0];
    std::memcpy(reinterpret_cast<void*>(initialRsp[0]), &root, sizeof(root));
    const auto freshResult = LumenAndroidFexCoreRunGuestV1(&guests[0], NoHostCall, Query, &fixture);
    if (freshResult != 0 || guests[0].gprs[0] != 44 ||
        guests[0].gprs[4] != initialRsp[0] + sizeof(root)) {
        std::fprintf(stderr, "FEX fresh-run code=%u rax=%llu\n", freshResult,
            static_cast<unsigned long long>(guests[0].gprs[0]));
        Fail(10, "fresh-run-and-root-return");
    }

    const std::uint64_t scalarTarget =
        reinterpret_cast<std::uintptr_t>(&ScalarTargetIdentity);
    if (LumenAndroidFexCoreRegisterHostCallTargetV1(scalarTarget) == 0) {
        Fail(11, "register-scalar-host-target");
    }
    const auto scalarEdit = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    if (scalarEdit == 0) Fail(11, "scalar-edit-barrier");
    if (mprotect(code, pageSize, PROT_READ | PROT_WRITE) != 0) {
        Fail(11, "scalar-edit-rw", errno);
    }
    std::uint8_t scalarProgram[] = {
        0xf3, 0x0f, 0x6f, 0x07,             // movdqu xmm0, [rdi]
        0x48, 0xbb, 0, 0, 0, 0, 0, 0, 0, 0, // movabs rbx, target
        0xff, 0xd3,                         // call rbx
        0xf3, 0x0f, 0x7f, 0x06,             // movdqu [rsi], xmm0
        0xc3,                               // ret
    };
    std::memcpy(scalarProgram + 6, &scalarTarget, sizeof(scalarTarget));
    std::memcpy(code, scalarProgram, sizeof(scalarProgram));
    if (mprotect(code, pageSize, PROT_READ) != 0) {
        Fail(11, "scalar-edit-rx", errno);
    }
    LumenAndroidFexCoreEndGuestMemoryMutationV1(
        scalarEdit, fixture.code, pageSize, 1);

    auto* scalarInput = static_cast<std::uint8_t*>(data);
    auto* scalarOutput = scalarInput + 32;
    ScalarHandshake scalar {
        scalarTarget,
        reinterpret_cast<std::uintptr_t>(data),
        static_cast<std::size_t>(pageSize * 4),
        {},
        {},
        0,
    };
    for (std::size_t index = 0; index != 16; ++index) {
        scalar.expected[index] = static_cast<std::uint8_t>(0x10 + index);
        scalar.replacement[index] = static_cast<std::uint8_t>(0xa0 + index);
    }
    std::memcpy(scalarInput, scalar.expected, sizeof(scalar.expected));
    std::memset(scalarOutput, 0, sizeof(scalar.replacement));
    LumenAndroidFexCoreGuestStateV1 scalarGuest {
        sizeof(LumenAndroidFexCoreGuestStateV1), 1, fixture.code, {},
        0, 0, 0, 0, 0,
    };
    scalarGuest.gprs[4] = initialRsp[0];
    scalarGuest.gprs[7] = reinterpret_cast<std::uintptr_t>(scalarInput);
    scalarGuest.gprs[6] = reinterpret_cast<std::uintptr_t>(scalarOutput);
    std::memcpy(reinterpret_cast<void*>(initialRsp[0]), &root, sizeof(root));
    const auto scalarResult = LumenAndroidFexCoreRunGuestV2(
        &scalarGuest, ScalarHostCall, Query, &scalar);
    if (scalarResult != 0 || scalar.callCount != 1 ||
        scalarGuest.gprs[4] != initialRsp[0] + sizeof(root) ||
        std::memcmp(scalarOutput, scalar.replacement,
                    sizeof(scalar.replacement)) != 0) {
        std::fprintf(stderr,
            "FEX scalar-v2 code=%u calls=%u rsp=%llx expectedRsp=%llx\n",
            scalarResult, scalar.callCount,
            static_cast<unsigned long long>(scalarGuest.gprs[4]),
            static_cast<unsigned long long>(initialRsp[0] + sizeof(root)));
        Fail(11, "scalar-v2-round-trip");
    }
    const auto cleanup = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    if (cleanup == 0) Fail(12, "cleanup-barrier");
    if (munmap(code, pageSize) != 0 || munmap(data, pageSize * 4) != 0) Fail(12, "unmap", errno);
    LumenAndroidFexCoreEndGuestMemoryMutationV1(cleanup, fixture.code, pageSize, 1);
    alarm(0);
    std::printf("PASS: Android FEX hostPage=%ld guestPage=4096 bridge=1 scalarV2=1 rollover=1 traps=1 peers=2 safepoint=1 retranslation=1 doorbellSignalCode=%u\n",
                pageSize, doorbellSignalCode);
    return 0;
}
