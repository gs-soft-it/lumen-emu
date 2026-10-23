// Lumen, 2026-10-11. SPDX-License-Identifier: GPL-2.0-only
// Original x86 fixtures exercising the real shared Android/Darwin FEX bridge.
#include "AndroidFexCoreRuntime.h"
#include "MemoryHandoff.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>

namespace {
using Lumen::GuestCpu::MemoryAccessIntent;
constexpr std::uint32_t Iterations = 100000;
constexpr std::uint64_t AlternateResult = 77;
enum class Mode { Return, Continue, Slot, Stack, Stop, Failed, Nested, ProtectDuringWait };
struct Fixture {
    std::uint64_t code {}, data {}, page {}, root {}, redirected {}, revoked {};
    std::uint64_t handoffAddress {}, handoffCalls {}, callbacks {};
    bool denyHandoff {}, publishOnRead {}, handoffAdmitted {true}, valid {true};
    unsigned depth {};
    Mode mode {Mode::Return};
} fixture;

void Target() {}
void Store(std::uint64_t address, std::uint64_t value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
}
bool Contains(std::uint64_t base, std::uint64_t bytes, std::uint64_t address,
              std::uint64_t size) {
    return size != 0 && size <= bytes && address >= base && address - base <= bytes - size;
}
std::uint32_t Query(void*, std::uint64_t address, std::uint64_t* base,
    std::uint64_t* bytes, std::uint32_t* writable) noexcept {
    if (!Contains(fixture.code, fixture.page, address, 1)) return 0;
    *base = fixture.code; *bytes = fixture.page; *writable = 0;
    return 1;
}
#if defined(__APPLE__)
std::uint32_t QueryData(std::uint64_t address, std::uint64_t bytes,
    std::uint32_t write) noexcept {
    if (write > 1 || bytes == 0 || bytes > UINT64_MAX - address) return 0;
    if (fixture.revoked != 0 && address < fixture.revoked + fixture.page &&
        fixture.revoked < address + bytes) return 0;
    if (Contains(fixture.data, 8 * fixture.page, address, bytes)) return 1;
    return write == 0 && Contains(fixture.code, fixture.page, address, bytes);
}
#endif
bool Handoff(std::uint64_t address, std::uint64_t bytes, MemoryAccessIntent intent) noexcept {
    if (!Contains(address, bytes, fixture.handoffAddress, 8)) return true;
    ++fixture.handoffCalls;
    fixture.handoffAdmitted &= intent == MemoryAccessIntent::Read &&
        LumenAndroidFexCoreGuestExecutionReadAdmissionEpochV1() != 0;
    if (fixture.denyHandoff) return false;
    if (fixture.publishOnRead) Store(fixture.handoffAddress, fixture.code + 128);
    return true;
}
std::uint32_t Callback(void*, std::uint64_t, std::uint64_t*, std::uint64_t*) noexcept;
std::uint32_t Run(unsigned offset, std::uint64_t stack, LumenAndroidFexCoreGuestStateV1* state) {
    *state = {};
    state->structSize = sizeof(*state); state->abiVersion = 1;
    state->rip = fixture.code + offset; state->gprs[4] = stack;
    Store(stack, fixture.root);
    return LumenAndroidFexCoreRunGuestV1(state, Callback, Query, nullptr);
}
std::uint32_t Callback(void*, std::uint64_t target, std::uint64_t* gprs,
    std::uint64_t* next) noexcept {
    fixture.valid &= target == reinterpret_cast<std::uint64_t>(&Target);
    ++fixture.callbacks;
    // RAX is a result register, not a SysV integer argument. The Android
    // bridge uses it to preserve RCX across SYSCALL; do not assume an input.
    gprs[0] = 1;
    // RETURN_GUEST must ignore this output, including when it is nonzero.
    *next = UINT64_MAX;
    if (!fixture.valid) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    if (fixture.mode == Mode::Nested && fixture.depth == 0) {
        ++fixture.depth;
        LumenAndroidFexCoreGuestStateV1 nested {};
        fixture.valid &= Run(64, fixture.data + 2 * fixture.page - 32, &nested) == 0 &&
            nested.gprs[0] == 1;
        --fixture.depth;
    }
    if (fixture.mode == Mode::Slot) Store(gprs[4], fixture.code + 128);
    if (fixture.redirected != 0) gprs[4] = fixture.redirected;
    if (fixture.mode == Mode::ProtectDuringWait) {
        const auto wait = LumenAndroidFexCoreBeginQuiescentHostWaitV1();
        fixture.valid &= wait != 0;
        if (wait != 0) {
            bool published = false;
            std::thread publisher([&] {
                const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
                if (token == 0) return;
                const auto page = fixture.redirected & ~(fixture.page - 1);
                const bool protectedPage = mprotect(reinterpret_cast<void*>(page), fixture.page, PROT_NONE) == 0;
                if (protectedPage) fixture.revoked = page;
                published = LumenAndroidFexCoreEndGuestMemoryMutationV2(token, page,
                    fixture.page, protectedPage ? 1U : 0U, nullptr, nullptr) == 1 && protectedPage;
            });
            publisher.join();
            fixture.valid &= published && LumenAndroidFexCoreEndQuiescentHostWaitV1(wait) == 1;
        }
    }
    if (fixture.mode == Mode::Stop) return LUMEN_ANDROID_FEXCORE_HOST_CALL_STOP_V1;
    if (fixture.mode == Mode::Failed || !fixture.valid) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    if (fixture.mode == Mode::Continue) {
        std::memcpy(next, reinterpret_cast<const void*>(gprs[4]), sizeof(*next));
        gprs[4] += 8;
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
    }
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_RETURN_GUEST_V1;
}
bool Protect(std::uint64_t page, bool accessible) {
    const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    if (token == 0) return false;
    const bool changed = mprotect(reinterpret_cast<void*>(page), fixture.page,
        accessible ? PROT_READ | PROT_WRITE : PROT_NONE) == 0;
    if (changed) fixture.revoked = accessible ? 0 : page;
    return LumenAndroidFexCoreEndGuestMemoryMutationV2(token, page, fixture.page,
        changed ? 1U : 0U, nullptr, nullptr) == 1 && changed;
}
bool PublishHandoff(std::uint64_t address) {
    const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    if (token == 0) return false;
    Lumen::GuestCpu::MarkHandoffRange(address, 8, MemoryAccessIntent::Read);
    Lumen::GuestCpu::PublishMemoryHandoffGpuDirty(true);
    return LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 1;
}
} // namespace

int main() {
    fixture.page = static_cast<std::uint64_t>(getpagesize());
    if (fixture.page != 4096 && fixture.page != 16384) return 2;
    auto* code = static_cast<std::uint8_t*>(mmap(nullptr, fixture.page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    void* data = mmap(nullptr, 8 * fixture.page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED || data == MAP_FAILED) return 2;
    fixture.code = reinterpret_cast<std::uint64_t>(code);
    fixture.data = reinterpret_cast<std::uint64_t>(data);
    // Accumulate each HLE result in callee-saved R13. RAX cannot carry an
    // input across a SysV call (and is bridge scratch on Android).
    // mov r12d,count; xor r13d,r13d; mov rbx,target; call rbx;
    // add r13,rax; dec r12d; jnz call; mov rax,r13; ret
    std::array<std::uint8_t, 33> loop {0x41,0xbc,0,0,0,0,0x45,0x31,0xed,
        0x48,0xbb,0,0,0,0,0,0,0,0,0xff,0xd3,0x49,0x01,0xc5,
        0x41,0xff,0xcc,0x75,0xf6,0x4c,0x89,0xe8,0xc3};
    const auto target = reinterpret_cast<std::uint64_t>(&Target);
    std::memcpy(loop.data() + 2, &Iterations, 4);
    std::memcpy(loop.data() + 11, &target, 8);
    std::memcpy(code, loop.data(), loop.size());
    // mov rbx,target; call rbx; ret
    std::array<std::uint8_t, 13> single {0x48,0xbb,0,0,0,0,0,0,0,0,0xff,0xd3,0xc3};
    std::memcpy(single.data() + 2, &target, 8);
    std::memcpy(code + 64, single.data(), single.size());
    const std::array<std::uint8_t, 6> alternate {0xb8,77,0,0,0,0xc3};
    std::memcpy(code + 128, alternate.data(), alternate.size());
    if (mprotect(code, fixture.page, PROT_READ | PROT_EXEC) != 0) return 2;
#if defined(__APPLE__)
    if (LumenMacOSFexCoreRegisterDataAccessQueryV1(QueryData) != 1) return 2;
#endif
    LumenAndroidFexCoreRuntimeStatusV1 runtime {};
    if (LumenAndroidFexCoreRuntimeProbeV1(&runtime) != 0 ||
        LumenAndroidFexCoreRegisterHostCallTargetV1(target) != 1 ||
        !Lumen::GuestCpu::RegisterMemoryHandoffQuery(Handoff)) return 2;
    fixture.root = LumenAndroidFexCoreRootReturnAddressV1();
    const auto stack = fixture.data + fixture.page - 32;
    unsigned cases {}, passed {};
    const auto check = [&](bool valid, const char* name) {
        ++cases; passed += valid ? 1U : 0U;
        std::printf("%s FexHostCallReturnTest %s\n", valid ? "PASS" : "FAIL", name);
    };
    LumenAndroidFexCoreGuestStateV1 state {};
    for (const auto mode : {Mode::Continue, Mode::Return}) {
        fixture.mode = mode; fixture.callbacks = 0;
        const auto status = Run(0, stack, &state);
        std::printf("FexHostCallReturnLoop disposition=%u status=%u rax=%llu callbacks=%llu rsp_delta=%lld\n",
            mode == Mode::Continue ? 1u : 3u, status,
            static_cast<unsigned long long>(state.gprs[0]), static_cast<unsigned long long>(fixture.callbacks),
            static_cast<long long>(state.gprs[4] - stack));
        check(status == 0 && state.gprs[0] == Iterations &&
            fixture.callbacks == Iterations && state.gprs[4] == stack + 8,
            mode == Mode::Continue ? "continue_loop_balances_shadow_stack" : "guest_return_loop_balances_shadow_stack");
    }
    fixture.mode = Mode::Slot;
    check(Run(64, stack, &state) == 0 && state.gprs[0] == AlternateResult && state.gprs[4] == stack + 8,
        "HLE_replaced_return_slot");
    fixture.mode = Mode::Stack;
    for (const auto redirected : {fixture.data + 3 * fixture.page,
                                  fixture.data + 4 * fixture.page - 4}) {
        fixture.redirected = redirected;
        Store(redirected, fixture.code + 128); Store(redirected + 8, fixture.root);
        check(Run(64, stack, &state) == 0 && state.gprs[0] == AlternateResult &&
            state.gprs[4] == redirected + 16,
            redirected % fixture.page == 0 ? "HLE_replaced_RSP" : "return_slot_crosses_host_page");
    }
    const auto guard = fixture.data + 5 * fixture.page;
    check(Protect(guard, false), "protect_return_page");
    fixture.redirected = guard;
    for (const auto mode : {Mode::Stop, Mode::Failed, Mode::Return}) {
        fixture.mode = mode;
        const auto status = Run(64, stack, &state);
        const auto expected = mode == Mode::Stop ? LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 :
            mode == Mode::Failed ? LUMEN_ANDROID_FEXCORE_RUNTIME_HOST_CALL_FAILED_V1 :
                LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1;
        check(status == expected && state.gprs[4] == guard &&
            (mode == Mode::Return ? state.faultSignal != 0 : state.faultSignal == 0),
            mode == Mode::Stop ? "STOP_never_reads_inaccessible_stack" :
            mode == Mode::Failed ? "FAILED_never_reads_inaccessible_stack" : "RET_fault_preserves_RSP");
    }
    check(Protect(guard, true), "restore_return_page");
    Store(guard, fixture.code + 128); Store(guard + 8, fixture.root);
    fixture.mode = Mode::ProtectDuringWait;
    check(Run(64, stack, &state) == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
        fixture.valid && state.gprs[4] == guard && state.faultSignal != 0,
        "protection_revoked_during_host_wait_is_observed_after_readmission");
    check(Protect(guard, true), "restore_after_host_wait");
    fixture.redirected = 0; fixture.mode = Mode::Nested; fixture.callbacks = 0;
    check(Run(64, stack, &state) == 0 && fixture.valid && fixture.callbacks == 2 &&
        state.gprs[0] == 1 && state.gprs[4] == stack + 8,
        "nested_guest_HLE_return_restores_parent");

    fixture.mode = Mode::Return;
    fixture.redirected = fixture.data + 6 * fixture.page;
    fixture.handoffAddress = fixture.redirected;
    Store(fixture.redirected, fixture.code + 76); // original continuation, replaced by publication below
    Store(fixture.redirected + 8, fixture.root);
    check(LumenAndroidFexCoreMemoryHandoffInstrumentationReadyV1() == 1 &&
        PublishHandoff(fixture.redirected), "enroll_dirty_return_slot");
    fixture.publishOnRead = true;
    check(Run(64, stack, &state) == 0 && state.gprs[0] == AlternateResult &&
        fixture.handoffCalls != 0 && fixture.handoffAdmitted,
        "RET_uses_handoff_published_slot_under_execution_admission");
    fixture.handoffCalls = 0; fixture.denyHandoff = true;
    check(Run(64, stack, &state) == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
        state.gprs[4] == fixture.redirected && fixture.handoffCalls != 0 && fixture.handoffAdmitted,
        "denied_RET_handoff_has_no_stack_effect");
    std::printf("FexHostCallReturnTest: %s (%u/%u)\n", passed == cases ? "PASS" : "FAIL", passed, cases);
    return passed == cases ? 0 : 1;
}
