// SPDX-License-Identifier: GPL-2.0-only
#include "DirectFexRuntime.h"
#include "DirectFexHostCallTable.h"
#include "DirectPerformanceOptions.h"

#include <array>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace Direct = Lumen::Ps5::DirectFex;
namespace {
constexpr std::size_t Page = 4096;

bool CheckVeneerMapping(std::uint64_t veneer, std::size_t hostPage) {
    std::uint64_t base {}, size {};
    bool ownedRange = veneer % hostPage == 0 &&
        Direct::QueryOwnedExecutable(veneer, &base, &size) && base == veneer && size == Page &&
        Direct::QueryOwnedExecutable(veneer + Page - 1, &base, &size) &&
        !Direct::QueryOwnedExecutable(veneer + Page, &base, &size) &&
        !Direct::QueryOwnedExecutable(veneer, nullptr, &size) &&
        !Direct::QueryOwnedExecutable(veneer, &base, nullptr);
#if defined(__APPLE__)
    mach_vm_address_t address = veneer;
    mach_vm_size_t mappedSize {};
    vm_region_basic_info_data_64_t information {};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const auto result = mach_vm_region(mach_task_self(), &address, &mappedSize,
        VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&information), &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    const bool sealed = result == KERN_SUCCESS && address <= veneer &&
        mappedSize >= hostPage && veneer - address <= mappedSize - hostPage &&
        information.protection == VM_PROT_READ;
    std::printf("PS5_DIRECT_FEX_DARWIN_VENEER_V1 sealed_read_only=%u host_page=%zu guest_page=%zu\n",
        sealed, hostPage, Page);
    ownedRange &= sealed;
#endif
    return ownedRange;
}

struct Fixture {
    std::uint64_t code {}, stack {}, nestedStack {}, target {};
    unsigned calls {}, disposition {LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1};
    std::uint64_t nestedExpected {41};
    bool arguments {}, nested {};
    bool binding {}, restoredBinding {};
    const void* expectedBinding {};
    std::barrier<>* overlap {};
    bool contextChecks {}, innerHost {}, contextValid {};
};
void Target() {}
std::uint32_t Query(void* opaque, std::uint64_t address, std::uint64_t* base,
    std::uint64_t* size, std::uint32_t* writable) noexcept {
    auto& fixture = *static_cast<Fixture*>(opaque);
    if (address < fixture.code || address - fixture.code >= Page) return 0;
    *base = fixture.code; *size = Page; *writable = 0;
    return 1;
}
std::uint32_t Host(void* opaque, std::uint64_t target, std::uint64_t* gprs,
    LumenAndroidFexCoreSimdViewV2*, std::uint64_t* next) noexcept {
    auto& fixture = *static_cast<Fixture*>(opaque);
    if (target != fixture.target) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    ++fixture.calls;
    const auto* binding = Direct::ActiveHostTargetBinding(target);
    fixture.binding = Lumen::Ps5::DirectPerformance::Get().hle_indexed_dispatch
        ? binding == (fixture.expectedBinding ? fixture.expectedBinding : &fixture) : binding == nullptr;
    Direct::HleContext parentContext {};
    if (fixture.contextChecks) {
        fixture.contextValid &= Direct::IsActive() && !Direct::CanCancelFromSignal() &&
            Direct::CaptureHleContext(&parentContext);
        // The nested HLE must temporarily hide the parent's pending switch,
        // then restore it along with the parent's frame and typed binding.
        parentContext.gprs[0] = fixture.innerHost ? fixture.nestedExpected - 1 : fixture.nestedExpected + 1;
        fixture.contextValid &= Direct::SwitchHleContext(parentContext);
        if (fixture.innerHost) return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
        if (fixture.overlap) fixture.overlap->arrive_and_wait();
    }
    fixture.arguments = gprs[0] == 5 && gprs[7] == 7 && gprs[6] == 8;
    LumenAndroidFexCoreGuestStateV1 nested {sizeof(nested), 1, fixture.code + (fixture.contextChecks ? 256 : 128)};
    nested.gprs[4] = fixture.nestedStack + Page - 24;
    const auto sentinel = Direct::RootReturn();
    std::memcpy(reinterpret_cast<void*>(nested.gprs[4]), &sentinel, 8);
    fixture.innerHost = true;
    const auto code = Direct::RunGuestV2(&nested, Host, Query, &fixture);
    fixture.innerHost = false;
    fixture.nested = code == 0 && nested.gprs[0] == fixture.nestedExpected && nested.rip == sentinel;
    fixture.restoredBinding = Direct::ActiveHostTargetBinding(target) == binding;
    gprs[0] = nested.gprs[0] + 1;
    if (fixture.contextChecks) {
        Direct::HleContext restored {};
        fixture.contextValid &= Direct::IsActive() && !Direct::CanCancelFromSignal() &&
            Direct::CaptureHleContext(&restored) && restored.rip == parentContext.rip &&
            restored.gprs[4] == parentContext.gprs[4];
        if (fixture.overlap) fixture.overlap->arrive_and_wait();
        // A lost pending parent context must not accidentally pass through
        // the ordinary register return path with the same expected result.
        gprs[0] = 0xdeadbeef;
    }
    if (fixture.disposition == LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1) {
        std::memcpy(next, reinterpret_cast<const void*>(gprs[4]), 8);
        gprs[4] += 8;
    }
    return fixture.disposition;
}
bool Run(Fixture& fixture) {
    fixture.calls = 0; fixture.arguments = fixture.nested = fixture.binding = fixture.restoredBinding = false;
    fixture.contextValid = true;
    LumenAndroidFexCoreGuestStateV1 state {sizeof(state), 1, fixture.code};
    state.gprs[4] = fixture.stack + Page - 24;
    const auto sentinel = Direct::RootReturn();
    std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &sentinel, 8);
    const auto result = Direct::RunGuestV2(&state, Host, Query, &fixture);
    const bool passed = result == 0 && fixture.calls == (fixture.contextChecks ? 2u : 1u) && fixture.arguments && fixture.nested &&
        fixture.binding && fixture.restoredBinding &&
        fixture.contextValid &&
        state.gprs[0] == fixture.nestedExpected + 2 && state.rip == sentinel;
    if (!passed)
        std::printf("PS5_DIRECT_FEX_SMOKE_FAILURE disposition=%u result=%u calls=%u args=%u nested=%u rax=%llu expected=%llu rip=%llx return=%llx\n",
            fixture.disposition, result, fixture.calls, fixture.arguments, fixture.nested,
            static_cast<unsigned long long>(state.gprs[0]),
            static_cast<unsigned long long>(fixture.nestedExpected + 2),
            static_cast<unsigned long long>(state.rip), static_cast<unsigned long long>(sentinel));
    return passed;
}

bool RunConcurrentScopes(Fixture& source, std::size_t hostPage) {
    auto* stacks = static_cast<unsigned char*>(mmap(nullptr, 4 * hostPage,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (stacks == MAP_FAILED) return false;
    std::barrier overlap(2);
    std::array<Fixture, 2> fixtures {source, source};
    std::array<bool, 2> passed {};
    std::array<std::thread, 2> workers;
    for (std::size_t i = 0; i < workers.size(); ++i) {
        auto& fixture = fixtures[i];
        fixture.stack = reinterpret_cast<std::uint64_t>(stacks + 2 * i * hostPage);
        fixture.nestedStack = fixture.stack + hostPage;
        fixture.expectedBinding = &source;
        fixture.contextChecks = true;
        fixture.overlap = &overlap;
        fixture.nestedExpected += 100 * i;
        workers[i] = std::thread([&, i] {
            Direct::HleContext context {};
            const auto clean = [&] {
                return !Direct::IsActive() && !Direct::CanCancelFromSignal() &&
                    !Direct::CaptureHleContext(&context) &&
                    Direct::ActiveHostTargetBinding(fixtures[i].target) == nullptr;
            };
            const bool before = clean();
            const bool run = Run(fixtures[i]);
            passed[i] = before && run && clean();
        });
    }
    for (auto& worker : workers) worker.join();
    const bool unmapped = munmap(stacks, 4 * hostPage) == 0;
    std::printf("PS5_DIRECT_FEX_THREAD_SCOPES_V1 nested_pending_contexts=%u thread_isolation=%u\n",
        fixtures[0].contextValid && fixtures[1].contextValid, passed[0] && passed[1]);
    return passed[0] && passed[1] && unmapped;
}

bool RunStackBounds(Fixture& fixture) {
    const auto pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) return false;
    const auto page = static_cast<std::size_t>(pageSize);
    const auto stackSize = ((256 * 1024 + page - 1) / page) * page;
    const auto mappingSize = stackSize + page;
    auto* mapping = static_cast<unsigned char*>(mmap(nullptr, mappingSize,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (mapping == MAP_FAILED) return false;
    const auto base = reinterpret_cast<std::uint64_t>(mapping);
    const auto sentinel = Direct::RootReturn();
    auto makeState = [&] {
        LumenAndroidFexCoreGuestStateV1 state {sizeof(state), 1, fixture.code + 128};
        state.gprs[4] = base + stackSize - 24;
        std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &sentinel, 8);
        return state;
    };
    auto execute = [&](std::uint64_t stackBase, std::uint64_t size) {
        auto state = makeState();
        state.gprs[4] = stackBase + size - 24;
        std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &sentinel, 8);
        const auto calls = fixture.calls;
        const auto result = Direct::RunGuestV2WithStack(&state, Host, Query, &fixture, stackBase, size);
        const bool passed = result == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
            state.gprs[0] == fixture.nestedExpected && state.rip == sentinel && fixture.calls == calls;
        if (!passed)
            std::printf("PS5_DIRECT_FEX_STACK_FAILURE result=%u rax=%llu expected=%llu rip=%llx return=%llx\n",
                result, static_cast<unsigned long long>(state.gprs[0]),
                static_cast<unsigned long long>(fixture.nestedExpected),
                static_cast<unsigned long long>(state.rip), static_cast<unsigned long long>(sentinel));
        return passed;
    };
    // Both regions start in the same anonymous RW mapping. Changes to the
    // adjacent region must never enlarge or invalidate the supplied stack.
    const bool initial = execute(base, stackSize);
    const bool unaligned = execute(base + 16, stackSize - 32);
    // A 4 KiB guest stack may occupy only part of a 16 KiB Darwin host page.
    // Host-page alignment must not expand the stack's usable guest bounds.
    const bool guestPageStack = execute(base + Page, Page);
    std::atomic<bool> stop {}, protectionFailed {};
    std::atomic<unsigned> changes {};
    std::thread protector {[&] {
        while (!stop.load(std::memory_order_relaxed)) {
            for (const auto protection : {PROT_NONE, PROT_READ | PROT_WRITE}) {
                if (mprotect(mapping + stackSize, page, protection) != 0) {
                    protectionFailed.store(true, std::memory_order_relaxed);
                    return;
                }
                changes.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        }
    }};
    while (changes.load(std::memory_order_relaxed) < 2 &&
        !protectionFailed.load(std::memory_order_relaxed)) std::this_thread::yield();
    const auto changesBefore = changes.load(std::memory_order_relaxed);
    bool concurrent = true;
    for (unsigned i = 0; i < 64; ++i) concurrent &= execute(base, stackSize);
    const auto changesAfter = changes.load(std::memory_order_relaxed);
    stop.store(true, std::memory_order_relaxed);
    protector.join();
    concurrent &= !protectionFailed.load(std::memory_order_relaxed) && changesAfter > changesBefore;

    bool invalidBounds = true;
    auto denied = [&](std::uint64_t stackBase, std::uint64_t size, std::uint64_t rsp) {
        auto state = makeState();
        state.gprs[4] = rsp;
        const auto calls = fixture.calls;
        const auto result = Direct::RunGuestV2WithStack(&state, Host, Query, &fixture, stackBase, size);
        return result == LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1 &&
            state.rip == fixture.code + 128 && state.gprs[4] == rsp && fixture.calls == calls;
    };
    invalidBounds &= denied(base, stackSize, base - 8);
    invalidBounds &= denied(base, stackSize, base + stackSize);
    invalidBounds &= denied(base + 16, stackSize - 32, base + 8);
    invalidBounds &= denied(base + 16, stackSize - 32, base + stackSize - 16);
    invalidBounds &= denied(base + Page, Page, base + Page - 8);
    invalidBounds &= denied(base + Page, Page, base + 2 * Page);
    invalidBounds &= denied(base, 0, base + stackSize - 24);
    invalidBounds &= denied(UINT64_MAX - 7, 16, UINT64_MAX - 3);
    invalidBounds &= denied(0, stackSize, 8);

    // An inaccessible page inside the actual stack must still be rejected,
    // even though the initial RSP and return word are on a writable page.
    bool guardDenied = mprotect(mapping + page, page, PROT_NONE) == 0;
    if (guardDenied) {
        auto state = makeState();
        const auto calls = fixture.calls;
        guardDenied = Direct::RunGuestV2WithStack(&state, Host, Query, &fixture, base, stackSize) ==
            LUMEN_ANDROID_FEXCORE_RUNTIME_EXECUTABLE_RANGE_DENIED_V1 &&
            state.rip == fixture.code + 128 && fixture.calls == calls;
    }
    const bool unmapped = munmap(mapping, mappingSize) == 0;
    std::printf("PS5_DIRECT_FEX_STACK_BOUNDS_V1 initial=%u unaligned=%u concurrent=%u invalid_bounds=%u guard_denied=%u neighbor_changes=%u guest_page_stack=%u\n",
        initial, unaligned, concurrent, invalidBounds, guardDenied, changesAfter - changesBefore, guestPageStack);
    return initial && unaligned && concurrent && invalidBounds && guardDenied && guestPageStack && unmapped;
}
}

int main() {
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    if (Direct::Probe(&status) != 0) { std::puts(Direct::Receipt()); return 1; }
    const auto hostPage = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    if (hostPage != status.hostPageSize || hostPage < Page || hostPage % Page != 0) return 2;
    // Own the rejected boundary as well: the next mapping could otherwise be
    // a valid engine return trampoline, which bypasses the fixture's query.
    const auto codeMappingSize = 2 * hostPage;
    void* code = mmap(nullptr, codeMappingSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* stack = mmap(nullptr, hostPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* nested = mmap(nullptr, hostPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED || stack == MAP_FAILED || nested == MAP_FAILED) return 2;
    Fixture fixture {reinterpret_cast<std::uint64_t>(code), reinterpret_cast<std::uint64_t>(stack),
        reinterpret_cast<std::uint64_t>(nested), reinterpret_cast<std::uint64_t>(&Target)};
    if (!Direct::RegisterHostTarget(fixture.target)) return 3;
    if (!Direct::RegisterHostTarget(fixture.target, &fixture)) return 3;
    const auto veneer = Direct::ResolveHostTarget(fixture.target);
    if (veneer == 0 || veneer == fixture.target) return 4;
    const bool veneerMapping = CheckVeneerMapping(veneer, hostPage);
    // mov eax,5; mov edi,7; mov esi,8; movabs rbx,veneer; call rbx;
    // add rax,1; ret. The callback also runs a real nested guest RET.
    std::array<unsigned char, 32> program {
        0xb8,5,0,0,0, 0xbf,7,0,0,0, 0xbe,8,0,0,0,
        0x48,0xbb,0,0,0,0,0,0,0,0, 0xff,0xd3, 0x48,0x83,0xc0,1, 0xc3};
    std::memcpy(program.data() + 17, &veneer, 8);
    std::memcpy(code, program.data(), program.size());
    std::memcpy(static_cast<unsigned char*>(code) + 256, program.data(), program.size());
    const std::array<unsigned char, 6> nestedCode {0xb8,41,0,0,0,0xc3};
    std::memcpy(static_cast<unsigned char*>(code) + 128, nestedCode.data(), nestedCode.size());
    std::memcpy(static_cast<unsigned char*>(code) + Page, nestedCode.data(), nestedCode.size());
    LumenAndroidFexCoreGuestStateV1 deniedCode {sizeof(deniedCode), 1, fixture.code};
    deniedCode.gprs[4] = fixture.stack + Page - 24;
    const auto deniedReturn = Direct::RootReturn();
    std::memcpy(reinterpret_cast<void*>(deniedCode.gprs[4]), &deniedReturn, sizeof(deniedReturn));
    // A guest-executable query cannot override writable host permissions.
    const bool writableCodeDenied = Direct::RunGuestV2(&deniedCode, Host, Query, &fixture) ==
        LUMEN_ANDROID_FEXCORE_RUNTIME_EXECUTABLE_RANGE_DENIED_V1 && fixture.calls == 0 &&
        deniedCode.rip == fixture.code;
    if (mprotect(code, codeMappingSize, PROT_READ) != 0) return 5;
    deniedCode.rip = fixture.code + Page;
    const bool queryDenied = Direct::RunGuestV2(&deniedCode, Host, Query, &fixture) ==
        LUMEN_ANDROID_FEXCORE_RUNTIME_EXECUTABLE_RANGE_DENIED_V1 && fixture.calls == 0 &&
        deniedCode.rip == fixture.code + Page;
    const bool continuation = Run(fixture);
    fixture.disposition = LUMEN_ANDROID_FEXCORE_HOST_CALL_RETURN_GUEST_V1;
    const bool guestReturn = Run(fixture);
    if (!Direct::BeginCodeMutation()) return 6;
    if (mprotect(code, hostPage, PROT_READ | PROT_WRITE) != 0) return 7;
    static_cast<unsigned char*>(code)[129] = 45;
    if (mprotect(code, hostPage, PROT_READ) != 0 || !Direct::EndCodeMutation(fixture.code + 129, 1, true)) return 8;
    fixture.nestedExpected = 45;
    const bool invalidation = Run(fixture);
    const bool threadScopes = RunConcurrentScopes(fixture, hostPage);
    const bool boundedStack = RunStackBounds(fixture);
    LumenAndroidFexCoreGuestTscV1 tsc;
    const bool clock = Direct::QueryGuestTsc(&tsc) == 0 && tsc.frequency != 0 && tsc.counter != 0;
    const bool bindings = fixture.binding && fixture.restoredBinding &&
        Direct::ActiveHostTargetBinding(fixture.target) == nullptr;
    bool invalidIds = true;
    if (Lumen::Ps5::DirectPerformance::Get().hle_indexed_dispatch) {
        for (const auto operation : {std::uint64_t {0}, std::uint64_t {Direct::HostCallTable::Capacity}, UINT64_MAX}) {
            if (!Direct::BeginCodeMutation() || mprotect(code, hostPage, PROT_READ | PROT_WRITE) != 0) return 11;
            // An invalid raw syscall must stop before a host callback or stack read.
            std::array<unsigned char, 13> denied {0x48,0xb8};
            std::memcpy(denied.data()+2, &operation, 8);
            denied[10]=0x0f; denied[11]=0x05; denied[12]=0xc3;
            std::memcpy(code, denied.data(), denied.size());
            if (mprotect(code, hostPage, PROT_READ) != 0 || !Direct::EndCodeMutation(fixture.code, Page, true)) return 12;
            LumenAndroidFexCoreGuestStateV1 state {sizeof(state),1,fixture.code};
            state.gprs[4] = fixture.stack + Page - 24;
            const auto calls = fixture.calls;
            invalidIds &= Direct::RunGuestV2(&state,Host,Query,&fixture) ==
                LUMEN_ANDROID_FEXCORE_RUNTIME_HOST_CALL_FAILED_V1 && fixture.calls == calls;
        }
    }
    std::printf("PS5_DIRECT_FEX_SMOKE_V1 probe=%u continuation=%u guest_return=%u nested=%u invalidation=%u tsc=%u bindings=%u invalid_ids=%u bounded_stack=%u veneer_mapping=%u writable_code_denied=%u query_denied=%u thread_scopes=%u facade=0 game_qualified=0\n",
        status.guestExecuted, continuation, guestReturn, fixture.nested, invalidation, clock, bindings, invalidIds, boundedStack,
        veneerMapping, writableCodeDenied, queryDenied, threadScopes);
    std::puts(Direct::Receipt());
    if (!Direct::BeginCodeMutation()) return 9;
    const bool unmapped = munmap(code, codeMappingSize) == 0 && munmap(stack, hostPage) == 0 && munmap(nested, hostPage) == 0;
    const bool closed = Direct::EndCodeMutation(fixture.code, codeMappingSize, true);
    return continuation && guestReturn && invalidation && clock && bindings && invalidIds && boundedStack &&
        veneerMapping && writableCodeDenied && queryDenied && threadScopes && unmapped && closed ? 0 : 10;
}
