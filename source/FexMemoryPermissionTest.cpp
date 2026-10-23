#include "AndroidFexCoreRuntime.h"
#include "FexRelocationPatchEpoch.h"
#include "Lumen/Core/VirtualMemory.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>

namespace {
namespace VM = Lumen::Core::VirtualMemory;
constexpr std::uint64_t HOST_PAGE = 16384;
constexpr std::uint64_t GUEST_PAGE = 4096;
constexpr std::uint64_t SENTINEL = UINT64_C(0x72657475726e3432);

bool Check(bool condition, const char* label) {
    if (!condition) std::fprintf(stderr, "fex-memory-permission: %s\n", label);
    return condition;
}

class Mapping {
public:
    explicit Mapping(std::size_t pages = 1)
        : size(pages * HOST_PAGE), address(VM::Alloc(0, size, VM::Mode::ReadWrite)) {}
    ~Mapping() {
        if (address != 0 && !VM::Free(address)) std::abort();
    }
    bool Protect(std::size_t offset, std::size_t length, VM::Mode mode) const {
        return address != 0 && VM::Protect(address + offset, length, mode);
    }
    std::uint8_t* Bytes(std::size_t offset = 0) const {
        return reinterpret_cast<std::uint8_t*>(address + offset);
    }
    const std::size_t size;
    const std::uint64_t address;
};

std::uint32_t Query(void*, std::uint64_t address, std::uint64_t* base,
                    std::uint64_t* size, std::uint32_t* writable) noexcept {
    VM::CodeReadLease lease;
    const std::uint64_t page = address & ~(GUEST_PAGE - 1);
    if (!VM::TryAcquireCodeReadLease(page, GUEST_PAGE, &lease)) return 0;
    *base = page;
    *size = GUEST_PAGE;
    *writable = 0;
    VM::ReleaseCodeReadLease(&lease);
    return 1;
}

std::uint32_t DenyHostCall(void*, std::uint64_t, std::uint64_t*, std::uint64_t*) noexcept {
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
}

class Fixture {
public:
    Fixture() : data(3) {}
    bool Initialize() {
        if (code.address == 0 || stack.address == 0 || data.address == 0 || output.address == 0) return false;
        // Original x86-64 probes: scalar load/store; SSE and AVX unaligned
        // copies; atomic xadd. Each operation is isolated from the next run.
        Put(0,   {0x48, 0x8b, 0x07, 0xc3});
        Put(64,  {0x48, 0x89, 0x07, 0xc3});
        Put(128, {0xf3, 0x0f, 0x6f, 0x07, 0xf3, 0x0f, 0x7f, 0x06, 0xc3});
        Put(192, {0xc5, 0xfe, 0x6f, 0x07, 0xc5, 0xfe, 0x7f, 0x06, 0xc3});
        Put(256, {0xf0, 0x48, 0x0f, 0xc1, 0x07, 0xc3});
        std::memset(data.Bytes(), 0x35, data.size);
        // Only the first guest page is executable. R/RX mixed siblings have
        // identical data permissions and must preserve separate execute gates.
        return code.Protect(0, HOST_PAGE, VM::Mode::Read) &&
               code.Protect(0, GUEST_PAGE, VM::Mode::ExecuteRead);
    }

    LumenAndroidFexCoreGuestStateV1 Run(std::size_t program, std::uint64_t source,
                                       std::uint64_t destination, std::uint64_t value,
                                       std::uint32_t* result) {
        // Preserve the last attempted operation if a regression times out
        // inside the signal helper; CTest suppresses these markers on PASS.
        std::fprintf(stderr, "fex-memory-case program=%zu source=0x%llx destination=0x%llx\n", program,
                     static_cast<unsigned long long>(source), static_cast<unsigned long long>(destination));
        LumenAndroidFexCoreGuestStateV1 state {};
        state.structSize = sizeof(state);
        state.abiVersion = 1;
        state.rip = code.address + program;
        state.gprs[0] = value;
        state.gprs[4] = stack.address + HOST_PAGE - 8;
        state.gprs[6] = destination;
        state.gprs[7] = source;
        const auto root_return = LumenAndroidFexCoreRootReturnAddressV1();
        std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &root_return, sizeof(root_return));
        *result = LumenAndroidFexCoreRunGuestV1(&state, DenyHostCall, Query, nullptr);
        return state;
    }

    bool Copy(std::size_t program, std::size_t source_offset, std::size_t width, const char* label) {
        std::memset(output.Bytes(), 0xcc, HOST_PAGE);
        std::uint32_t result = UINT32_MAX;
        Run(program, data.address + source_offset, output.address, 0, &result);
        return Check(result == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
                     std::memcmp(output.Bytes(), data.Bytes(source_offset), width) == 0 &&
                     output.Bytes()[width] == 0xcc, label);
    }

    bool Fault(std::size_t program, std::size_t source_offset,
               std::uint64_t destination, const char* label) {
        std::uint32_t result = UINT32_MAX;
        const auto state = Run(program, data.address + source_offset, destination, SENTINEL, &result);
        return Check(result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
                     state.faultSignal != 0 && state.faultAddress >= data.address &&
                     state.faultAddress < data.address + data.size, label);
    }

    Mapping code, stack, data, output;
private:
    void Put(std::size_t offset, std::initializer_list<std::uint8_t> bytes) {
        std::memcpy(code.Bytes(offset), bytes.begin(), bytes.size());
    }
};

bool RunTests() {
    Fixture f;
    if (!Check(f.Initialize(), "managed fixture / RX publication")) return false;
    bool ok = true;
    std::uint32_t result = UINT32_MAX;
    for (const std::size_t boundary : {GUEST_PAGE, 2 * GUEST_PAGE, 3 * GUEST_PAGE, HOST_PAGE}) {
        const std::size_t at = boundary - 4;
        auto state = f.Run(0, f.data.address + at, 0, 0, &result);
        ok = Check(result == 0 && state.gprs[0] == UINT64_C(0x3535353535353535),
                   "scalar load crosses guest/host boundary") && ok;
        f.Run(64, f.data.address + at, 0, SENTINEL, &result);
        std::uint64_t stored = 0;
        std::memcpy(&stored, f.data.Bytes(at), sizeof(stored));
        ok = Check(result == 0 && stored == SENTINEL, "scalar store crosses guest/host boundary") && ok;
        std::memset(f.data.Bytes(at), 0x35, sizeof(stored));
        ok = f.Copy(128, boundary - 8, 16, "SSE load crosses guest/host boundary") && ok;
        ok = f.Copy(192, boundary - 16, 32, "AVX load crosses guest/host boundary") && ok;
    }
    auto state = f.Run(256, f.data.address + GUEST_PAGE, 0, 7, &result);
    std::uint64_t atomic_result = 0;
    std::memcpy(&atomic_result, f.data.Bytes(GUEST_PAGE), sizeof(atomic_result));
    ok = Check(result == 0 && state.gprs[0] == UINT64_C(0x3535353535353535) &&
               atomic_result == UINT64_C(0x353535353535353c), "atomic RW xadd") && ok;

    // Uniform host read-only page, with every logical subpage probed by real
    // translated stores/atomics. Faulted operations must not write any byte.
    ok = Check(f.data.Protect(HOST_PAGE, HOST_PAGE, VM::Mode::Read), "protect read-only host page") && ok;
    state = f.Run(0, f.data.address + HOST_PAGE + 4, 0, 0, &result);
    ok = Check(result == 0 && state.gprs[0] == UINT64_C(0x3535353535353535),
               "unaligned scalar load remains admitted on R") && ok;
    for (std::size_t offset = HOST_PAGE; offset < 2 * HOST_PAGE; offset += GUEST_PAGE) {
        ok = f.Fault(64, offset, 0, "scalar store denied on R") && ok;
        ok = f.Fault(64, offset + 4, 0, "unaligned scalar store denied on R") && ok;
        ok = f.Fault(256, offset, 0, "atomic store denied on R") && ok;
        ok = f.Fault(256, offset + 4, 0, "unaligned atomic store denied on R") && ok;
        ok = f.Fault(128, 0, f.data.address + offset, "SSE store denied on R") && ok;
        ok = f.Fault(192, 0, f.data.address + offset, "AVX store denied on R") && ok;
    }
    bool readonly_intact = true;
    for (std::size_t offset = HOST_PAGE; offset < 2 * HOST_PAGE; ++offset)
        readonly_intact = f.data.Bytes()[offset] == 0x35 && readonly_intact;
    ok = Check(readonly_intact, "denied stores leave R contents intact") && ok;

    ok = Check(f.data.Protect(2 * HOST_PAGE, HOST_PAGE, VM::Mode::NoAccess), "protect host guard") && ok;
    ok = f.Copy(128, 2 * HOST_PAGE - 16, 16, "last SSE read before guard") && ok;
    ok = f.Copy(192, 2 * HOST_PAGE - 32, 32, "last AVX read before guard") && ok;
    ok = f.Fault(256, 2 * HOST_PAGE - 4, 0, "unaligned atomic crossing R/guard denied") && ok;
    for (const std::size_t program : {std::size_t{0}, std::size_t{128}, std::size_t{192}}) {
        std::memset(f.output.Bytes(), 0xcc, HOST_PAGE);
        ok = f.Fault(program, 2 * HOST_PAGE - 1, f.output.address, "load crossing guard faults") && ok;
        for (std::size_t byte = 0; byte < 32; byte++)
            ok = Check(f.output.Bytes()[byte] == 0xcc, "faulted load does not publish output") && ok;
    }
    for (std::size_t offset = 2 * HOST_PAGE; offset < 3 * HOST_PAGE; offset += GUEST_PAGE) {
        ok = f.Fault(0, offset, 0, "scalar read denied on guard") && ok;
        ok = f.Fault(64, offset, 0, "scalar write denied on guard") && ok;
    }
    // Failed loads and atomics must not corrupt their original R operand,
    // including the readable prefix of an interval ending in the guard.
    for (std::size_t byte = 2 * HOST_PAGE - 16; byte < 2 * HOST_PAGE; ++byte)
        ok = Check(f.data.Bytes()[byte] == 0x35, "cross-guard fault preserves readable prefix") && ok;

    // A denied mixed 4 KiB request cannot partially protect the valid first
    // host page. Exercise it immediately through translated writes afterward.
    const auto generation = VM::GetMutationGeneration();
    auto old = VM::Mode::ExecuteWrite;
    ok = Check(!VM::Protect(f.data.address, HOST_PAGE + GUEST_PAGE, VM::Mode::NoAccess, &old) &&
               old == VM::Mode::ExecuteWrite && VM::GetMutationGeneration() == generation,
               "mixed guard/R rejected atomically") && ok;
    f.Run(64, f.data.address, 0, SENTINEL, &result);
    std::uint64_t stored = 0;
    std::memcpy(&stored, f.data.Bytes(), sizeof(stored));
    ok = Check(result == 0 && stored == SENTINEL, "rejected protect retains translated write access") && ok;
    VM::CodeReadLease denied_code;
    ok = Check(!VM::TryAcquireCodeReadLease(f.code.address + GUEST_PAGE, 1, &denied_code),
               "R sibling receives no execute lease") && ok;
    VM::ReleaseCodeReadLease(&denied_code);
    return ok;
}

bool ConcurrentPermissionQueries() {
    std::array<Fixture, 2> fixtures;
    for (auto& fixture : fixtures)
        if (!Check(fixture.Initialize(), "concurrent managed fixture")) return false;
    std::atomic<bool> start {false};
    std::atomic<bool> ok {true};
    std::atomic<unsigned> completed {0};
    std::array<std::thread, 2> readers;
    for (std::size_t index = 0; index < readers.size(); ++index) {
        readers[index] = std::thread([&, index] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            auto& fixture = fixtures[index];
            for (unsigned iteration = 0; iteration < 32; ++iteration) {
                std::uint32_t result = UINT32_MAX;
                const auto state = fixture.Run(0, fixture.data.address + HOST_PAGE - 4, 0, 0, &result);
                if (result != 0 || state.gprs[0] != UINT64_C(0x3535353535353535))
                    ok.store(false, std::memory_order_relaxed);
                completed.fetch_add(1, std::memory_order_release);
            }
        });
    }
    start.store(true, std::memory_order_release);
    while (completed.load(std::memory_order_acquire) == 0) std::this_thread::yield();
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
        // Allocation publishes an additive snapshot while readers may still
        // execute. Release additionally exercises the existing exclusive VM
        // safepoint, without changing either reader's own data mapping.
        const auto mapping = VM::Alloc(0, HOST_PAGE, VM::Mode::ReadWrite);
        if (mapping == 0 || !VM::Free(mapping)) ok.store(false, std::memory_order_relaxed);
    }
    for (auto& reader : readers) reader.join();
    return Check(ok.load(std::memory_order_relaxed) && completed.load(std::memory_order_relaxed) == 64,
                 "permission queries survive concurrent publication and VM safepoints");
}

bool BorrowedDataPermissions() {
    Mapping data(3);
    if (data.address == 0 || !data.Protect(HOST_PAGE, HOST_PAGE, VM::Mode::Read) ||
        !data.Protect(2 * HOST_PAGE, HOST_PAGE, VM::Mode::NoAccess)) return false;
    bool ok = true;
    {
        VM::GuestDataLease read, write, cross, denied;
        ok = Check(VM::TryAcquireGuestDataLease(data.address + HOST_PAGE, 8, VM::Mode::Read, &read) &&
                   VM::TryAcquireGuestDataLease(data.address, 8, VM::Mode::Write, &write) &&
                   VM::TryAcquireGuestDataLease(data.address + HOST_PAGE - 4, 8, VM::Mode::Read, &cross),
                   "borrow readable spans and writable data") && ok;
        ok = Check(!VM::TryAcquireGuestDataLease(data.address + HOST_PAGE, 1, VM::Mode::Write, &denied) &&
                   !VM::TryAcquireGuestDataLease(data.address + HOST_PAGE - 4, 8, VM::Mode::Write, &denied) &&
                   !VM::TryAcquireGuestDataLease(data.address + 2 * HOST_PAGE - 4, 8, VM::Mode::Read, &denied) &&
                   !VM::TryAcquireGuestDataLease(data.address, 1, VM::Mode::Execute, &denied) &&
                   !VM::TryAcquireGuestDataLease(UINT64_MAX - 3, 8, VM::Mode::Read, &denied) &&
                   !VM::TryAcquireGuestDataLease(data.address, 0, VM::Mode::Read, &denied) &&
                   !denied.IsActive(), "borrow rejects whole denied/overflow/invalid interval") && ok;
        std::uint64_t native = 0;
        VM::GuestDataLease native_buffer;
        ok = Check(VM::TryAcquireGuestDataLease(reinterpret_cast<std::uint64_t>(&native),
                   sizeof(native), VM::Mode::ReadWrite, &native_buffer), "borrow caller-owned native stack") && ok;
    }
    const auto tiny = VM::Alloc(0, 1, VM::Mode::ReadWrite);
    {
        VM::GuestDataLease allowed, denied;
        ok = Check(tiny != 0 && VM::TryAcquireGuestDataLease(tiny, 1, VM::Mode::Read, &allowed) &&
                   !VM::TryAcquireGuestDataLease(tiny, 2, VM::Mode::Read, &denied) &&
                   !VM::TryAcquireGuestDataLease(tiny + 1, 1, VM::Mode::Read, &denied),
                   "borrow rejects allocation padding despite wider host permissions") && ok;
    }
    ok = Check(VM::Free(tiny), "release tiny lease fixture") && ok;

    Mapping code;
    const auto pinned = VM::Alloc(0, HOST_PAGE, VM::Mode::ReadWrite);
    if (pinned == 0 || code.address == 0 || !code.Protect(0, HOST_PAGE, VM::Mode::ExecuteRead)) return false;
    for (const bool free_mapping : {false, true}) {
        VM::GuestDataLease lease;
        if (!VM::TryAcquireGuestDataLease(pinned, 8, VM::Mode::Read, &lease)) return false;
        std::atomic<bool> done {false};
        bool mutation_result = false;
        std::thread mutation([&] {
            mutation_result = free_mapping ? VM::Free(pinned)
                : VM::Protect(pinned, HOST_PAGE, VM::Mode::Read);
            done.store(true, std::memory_order_release);
        });
        bool pending = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            VM::CodeReadLease probe;
            if (!VM::TryAcquireCodeReadLease(code.address, 1, &probe)) {
                pending = true;
                break;
            }
            VM::ReleaseCodeReadLease(&probe);
            std::this_thread::yield();
        }
        ok = Check(pending && !done.load(std::memory_order_acquire),
                   "borrow pins mapping against pending Protect/Free") && ok;
        {
            VM::GuestDataLease nested;
            ok = Check(VM::TryAcquireGuestDataLease(pinned + 8, 8, VM::Mode::Read, &nested),
                       "nested source/destination borrow survives pending mutation") && ok;
        }
        VM::ReleaseGuestDataLease(&lease);
        mutation.join();
        ok = Check(mutation_result && done.load(std::memory_order_acquire),
                   "VM mutation completes after borrowed buffer release") && ok;
    }
    return ok;
}

bool RelocationPatchEpochNesting() {
    using Lumen::Android::FexCoreLab::RelocationPatchEpoch;
    using Lumen::Android::FexCoreLab::RelocationPatchEpochRequested;
    for (const char* value : std::array<const char*, 9> {nullptr, "", "0", "01", "1x", "1 ", " 1", "1\n", "true"})
        if (!Check(!RelocationPatchEpochRequested(value), "relocation epoch selector rejects non-exact-one")) return false;
    if (!RelocationPatchEpochRequested("1")) return false;
    {
        RelocationPatchEpoch disabled(false);
        for (unsigned index = 0; index < 257; ++index)
            if (!disabled.AdmitNext()) return false;
        if (disabled.Attempts() != 0 || disabled.Epochs() != 0) return false;
    }

    Mapping code, probe, stack, control, slots(3);
    if (code.address == 0 || probe.address == 0 || stack.address == 0 ||
        control.address == 0 || slots.address == 0) return false;
    // A real translated peer repeatedly calls the same RX function, records
    // its return value and increments an aligned counter until asked to stop.
    const std::array<std::uint8_t, 36> loop {
        0x48, 0xb9, 0, 0, 0, 0, 0, 0, 0, 0,
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,
        0xff, 0xd0, 0x89, 0x41, 0x10, 0xf0, 0x48, 0xff, 0x01,
        0x80, 0x79, 0x08, 0x00, 0x74, 0xe7, 0xc3};
    std::memcpy(code.Bytes(), loop.data(), loop.size());
    std::memcpy(code.Bytes(2), &control.address, sizeof(control.address));
    std::memcpy(code.Bytes(12), &probe.address, sizeof(probe.address));
    const std::array<std::uint8_t, 8> original {0xb8, 0x11, 0, 0, 0, 0xc3, 0x90, 0x90};
    const std::array<std::uint8_t, 8> replacement {0xb8, 0x22, 0, 0, 0, 0xc3, 0x90, 0x90};
    std::memcpy(probe.Bytes(), original.data(), original.size());
    std::memset(control.Bytes(), 0, HOST_PAGE);
    std::memset(slots.Bytes(), 0, slots.size);
    if (!code.Protect(0, HOST_PAGE, VM::Mode::ExecuteRead) ||
        !probe.Protect(0, HOST_PAGE, VM::Mode::ExecuteRead) ||
        !slots.Protect(0, HOST_PAGE, VM::Mode::Read) ||
        !slots.Protect(HOST_PAGE, HOST_PAGE, VM::Mode::NoAccess)) return false;
    auto* counter = reinterpret_cast<std::uint64_t*>(control.Bytes());
    auto* stop = control.Bytes(8);
    auto* returned = reinterpret_cast<std::uint32_t*>(control.Bytes(16));
    const auto wait_for = [](auto&& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) return true;
            std::this_thread::yield();
        }
        return false;
    };
    std::uint32_t peer_status = UINT32_MAX;
    std::thread peer([&] {
        LumenAndroidFexCoreGuestStateV1 state {};
        state.structSize = sizeof(state);
        state.abiVersion = 1;
        state.rip = code.address;
        state.gprs[4] = stack.address + HOST_PAGE - 8;
        const auto root_return = LumenAndroidFexCoreRootReturnAddressV1();
        std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &root_return, sizeof(root_return));
        peer_status = LumenAndroidFexCoreRunGuestV1(&state, DenyHostCall, Query, nullptr);
    });
    bool ok = Check(wait_for([&] { return __atomic_load_n(counter, __ATOMIC_ACQUIRE) > 100 &&
                                        __atomic_load_n(returned, __ATOMIC_ACQUIRE) == 0x11; }),
                    "translated peer warms the original RX function before batching");
    std::uint64_t replacement_word = 0;
    std::memcpy(&replacement_word, replacement.data(), sizeof(replacement_word));
    const auto generation = VM::GetMutationGeneration();
    std::uint64_t second_epoch_counter = 0;
    {
        RelocationPatchEpoch epoch(true);
        const bool admitted = ok && epoch.AdmitNext();
        ok = Check(admitted, "outer relocation epoch admits beside a running guest") && ok;
        if (admitted) {
            const auto paused_counter = __atomic_load_n(counter, __ATOMIC_ACQUIRE);
            ok = Check(VM::PatchReplace(probe.address, replacement_word), "nested RX patch publishes its own invalidation") && ok;
            for (std::size_t index = 1; index < RelocationPatchEpoch::MaxAttempts; ++index) {
                ok = Check(epoch.AdmitNext() && VM::PatchReplace(slots.address + index * 8, index),
                           "nested read-only slot patch restores permissions") && ok;
                ok = Check(__atomic_load_n(counter, __ATOMIC_ACQUIRE) == paused_counter,
                           "inner publication does not readmit the translated peer") && ok;
            }
            ok = Check(epoch.Attempts() == 256 && epoch.Epochs() == 1 &&
                       VM::GetMutationGeneration() == generation + 256,
                       "one outer epoch preserves every slot generation advance") && ok;
            VM::GuestDataLease write, guard;
            ok = Check(!VM::TryAcquireGuestDataLease(slots.address, 8, VM::Mode::Write, &write) &&
                       !VM::TryAcquireGuestDataLease(slots.address + HOST_PAGE, 1, VM::Mode::Read, &guard) &&
                       !write.IsActive() && !guard.IsActive(), "slot R and neighboring guard remain protected") && ok;
            ok = Check(epoch.AdmitNext() && epoch.Epochs() == 2, "attempt 257 rotates the bounded FEX epoch") && ok;
            second_epoch_counter = __atomic_load_n(counter, __ATOMIC_ACQUIRE);
            ok = Check(!VM::PatchReplace(probe.address, replacement_word), "unchanged inner patch retains its false result") && ok;
            ok = Check(epoch.AdmitNext() && !VM::PatchReplace(UINT64_MAX - 3, 8),
                       "invalid inner patch remains fail-closed") && ok;
            ok = Check(VM::GetMutationGeneration() == generation + 258 &&
                       __atomic_load_n(counter, __ATOMIC_ACQUIRE) == second_epoch_counter,
                       "unchanged and failed inner publishes preserve conservative invalidation without reopening") && ok;
        }
        epoch.Close();
    }
    ok = Check(wait_for([&] { return __atomic_load_n(counter, __ATOMIC_ACQUIRE) > second_epoch_counter &&
                                    __atomic_load_n(returned, __ATOMIC_ACQUIRE) == 0x22; }),
                "final outer close readmits peer with the updated cached RX function") && ok;
    __atomic_store_n(stop, std::uint8_t {1}, __ATOMIC_RELEASE);
    peer.join();
    return Check(peer_status == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1,
                 "translated peer exits normally after batch publication") && ok;
}

struct RelocationEpochHleRun {
    std::uint64_t target, probe, slots, replacement, ownedBase, expectedReturn;
    std::uint64_t* counter;
    std::uint64_t finalParkedCounter {};
    unsigned calls {};
    bool valid {true};
};

void RelocationEpochHostTarget() {}

std::uint32_t RelocationEpochOwnedStackQuery(std::uint64_t address, std::uint64_t size,
    std::uint32_t write, std::uint64_t ownedBase, std::uint64_t ownedSize) noexcept {
    return write <= 1 && VM::CheckOwnedStackAccessUnderFexExecutionBarrier(
        address, size, write != 0, ownedBase, ownedSize) ? 1U : 0U;
}

std::uint32_t RelocationEpochHostCall(void* opaque, std::uint64_t target,
    std::uint64_t* gprs, LumenAndroidFexCoreSimdViewV2*, std::uint64_t* nextRip) noexcept {
    auto& run = *static_cast<RelocationEpochHleRun*>(opaque);
    ++run.calls;
    if (target != run.target || gprs == nullptr || nextRip == nullptr ||
        !RelocationEpochOwnedStackQuery(gprs[4], 8, 0, run.ownedBase, HOST_PAGE)) {
        run.valid = false;
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    }
    // Copy the CALL return slot while this top-level callback still owns its
    // HLE reader. No guest pointer/VM lease remains live across the wait.
    std::uint64_t return_rip = 0;
    std::memcpy(&return_rip, reinterpret_cast<const void*>(gprs[4]), sizeof(return_rip));
    run.valid = Check(return_rip == run.expectedReturn, "HLE epoch copies its actual caller return slot") && run.valid;
    const auto wait = LumenAndroidFexCoreBeginQuiescentHostWaitV1();
    run.valid = Check(wait != 0, "HLE epoch owner starts with a counted top-level reader") && run.valid;
    if (wait == 0 || LumenAndroidFexCoreEndQuiescentHostWaitV1(wait) != 1)
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;

    using Lumen::Android::FexCoreLab::RelocationPatchEpoch;
    const auto generation = VM::GetMutationGeneration();
    RelocationPatchEpoch epoch(true);
    if (!epoch.AdmitNext()) {
        run.valid = false;
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    }
    const auto parked_counter = __atomic_load_n(run.counter, __ATOMIC_ACQUIRE);
    run.valid = Check(LumenAndroidFexCoreBeginQuiescentHostWaitV1() == 0,
                     "HLE reader is upgraded while its outer epoch owns the writer") && run.valid;
    run.valid = Check(VM::PatchReplace(run.probe, run.replacement),
                     "HLE owner invalidates its own and peer's warm RX function") && run.valid;
    for (std::size_t index = 1; index < RelocationPatchEpoch::MaxAttempts; ++index) {
        run.valid = Check(epoch.AdmitNext() && VM::PatchReplace(run.slots + index * 8, index),
                         "HLE nested slot publishes independently") && run.valid;
        run.valid = Check(__atomic_load_n(run.counter, __ATOMIC_ACQUIRE) == parked_counter,
                         "HLE inner publication keeps its peer parked") && run.valid;
    }
    {
        VM::GuestDataLease read, write, guard;
        VM::CodeReadLease rx;
        run.valid = Check(VM::TryAcquireGuestDataLease(run.slots + 8, 8, VM::Mode::Read, &read) &&
                         *reinterpret_cast<const std::uint64_t*>(run.slots + 8) == 1 &&
                         !VM::TryAcquireGuestDataLease(run.slots + 8, 8, VM::Mode::Write, &write) &&
                         !VM::TryAcquireGuestDataLease(run.slots + HOST_PAGE, 1, VM::Mode::Read, &guard) &&
                         VM::TryAcquireCodeReadLease(run.probe, 8, &rx),
                         "HLE slot publishers release local gates and restore R/guard/RX permissions") && run.valid;
        VM::ReleaseCodeReadLease(&rx);
    }
    run.valid = Check(epoch.Attempts() == 256 && epoch.Epochs() == 1 &&
                     VM::GetMutationGeneration() == generation + 256,
                     "HLE first epoch retains every slot generation") && run.valid;
    if (!epoch.AdmitNext()) {
        run.valid = false;
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    }
    const auto second_counter = __atomic_load_n(run.counter, __ATOMIC_ACQUIRE);
    run.finalParkedCounter = second_counter;
    run.valid = Check(!VM::PatchReplace(run.probe, run.replacement) && epoch.AdmitNext() &&
                     !VM::PatchReplace(UINT64_MAX - 3, 8) && epoch.Epochs() == 2 &&
                     epoch.Attempts() == 258 && VM::GetMutationGeneration() == generation + 258 &&
                     __atomic_load_n(run.counter, __ATOMIC_ACQUIRE) == second_counter,
                     "HLE second epoch retains no-op/failed full invalidation without readmission") && run.valid;
    epoch.Close();

    // A fresh quiescent token proves final close restored this HLE reader;
    // its matching end restores it again before returning through the bridge.
    const auto restored = LumenAndroidFexCoreBeginQuiescentHostWaitV1();
    run.valid = Check(restored != 0 && LumenAndroidFexCoreEndQuiescentHostWaitV1(restored) == 1,
                     "HLE reader ownership is restored after final batch close") && run.valid;
    *nextRip = return_rip;
    gprs[4] += sizeof(std::uint64_t);
    gprs[0] = SENTINEL;
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
}

bool RelocationPatchEpochHleReturn() {
    Mapping peer_code, owner_code, probe, peer_stack, control, output, slots(2);
    if (peer_code.address == 0 || owner_code.address == 0 || probe.address == 0 ||
        peer_stack.address == 0 || control.address == 0 || output.address == 0 || slots.address == 0) return false;
    const auto target = reinterpret_cast<std::uint64_t>(&RelocationEpochHostTarget);
    // Register before either real Run starts: registration while a peer is
    // executing is forbidden by the ordinary immutable target contract.
    if (!LumenAndroidFexCoreRegisterHostCallTargetV1(target)) return false;
    const std::array<std::uint8_t, 36> peer_loop {
        0x48, 0xb9, 0, 0, 0, 0, 0, 0, 0, 0,
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,
        0xff, 0xd0, 0x89, 0x41, 0x10, 0xf0, 0x48, 0xff, 0x01,
        0x80, 0x79, 0x08, 0x00, 0x74, 0xe7, 0xc3};
    std::memcpy(peer_code.Bytes(), peer_loop.data(), peer_loop.size());
    std::memcpy(peer_code.Bytes(2), &control.address, 8);
    std::memcpy(peer_code.Bytes(12), &probe.address, 8);
    // Warm RX; call HLE; retain its RAX; call the same RX again after HLE
    // returns. A STOP-only callback could not publish the final two outputs.
    const std::array<std::uint8_t, 76> owner_program {
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0,
        0x48, 0xbf, 0, 0, 0, 0, 0, 0, 0, 0, 0x89, 0x07,
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0,
        0x48, 0xbf, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x89, 0x47, 0x08,
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0,
        0x48, 0xbf, 0, 0, 0, 0, 0, 0, 0, 0, 0x89, 0x47, 0x10, 0xc3};
    std::memcpy(owner_code.Bytes(), owner_program.data(), owner_program.size());
    for (const auto offset : {2, 52}) std::memcpy(owner_code.Bytes(offset), &probe.address, 8);
    for (const auto offset : {14, 38, 64}) std::memcpy(owner_code.Bytes(offset), &output.address, 8);
    std::memcpy(owner_code.Bytes(26), &target, 8);
    const std::array<std::uint8_t, 8> original {0xb8, 0x11, 0, 0, 0, 0xc3, 0x90, 0x90};
    const std::array<std::uint8_t, 8> replacement {0xb8, 0x22, 0, 0, 0, 0xc3, 0x90, 0x90};
    std::memcpy(probe.Bytes(), original.data(), original.size());
    std::memset(control.Bytes(), 0, HOST_PAGE);
    std::memset(output.Bytes(), 0, HOST_PAGE);
    std::memset(slots.Bytes(), 0, slots.size);
    if (!peer_code.Protect(0, HOST_PAGE, VM::Mode::ExecuteRead) ||
        !owner_code.Protect(0, HOST_PAGE, VM::Mode::ExecuteRead) ||
        !probe.Protect(0, HOST_PAGE, VM::Mode::ExecuteRead) ||
        !slots.Protect(0, HOST_PAGE, VM::Mode::Read) ||
        !slots.Protect(HOST_PAGE, HOST_PAGE, VM::Mode::NoAccess)) return false;
    void* owned_stack = mmap(nullptr, HOST_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (owned_stack == MAP_FAILED) return false;
    auto* counter = reinterpret_cast<std::uint64_t*>(control.Bytes());
    auto* stop = control.Bytes(8);
    auto* returned = reinterpret_cast<std::uint32_t*>(control.Bytes(16));
    const auto wait_for = [](auto&& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) return true;
            std::this_thread::yield();
        }
        return false;
    };
    std::uint32_t peer_status = UINT32_MAX;
    std::thread peer([&] {
        LumenAndroidFexCoreGuestStateV1 state {};
        state.structSize = sizeof(state);
        state.abiVersion = 1;
        state.rip = peer_code.address;
        state.gprs[4] = peer_stack.address + HOST_PAGE - 8;
        const auto root_return = LumenAndroidFexCoreRootReturnAddressV1();
        std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &root_return, 8);
        peer_status = LumenAndroidFexCoreRunGuestV1(&state, DenyHostCall, Query, nullptr);
    });
    bool ok = Check(wait_for([&] { return __atomic_load_n(counter, __ATOMIC_ACQUIRE) > 100 &&
                                        __atomic_load_n(returned, __ATOMIC_ACQUIRE) == 0x11; }),
                    "HLE batch peer warms RX before owner enters HLE");
    std::uint64_t replacement_word = 0;
    std::memcpy(&replacement_word, replacement.data(), 8);
    RelocationEpochHleRun run {target, probe.address, slots.address, replacement_word,
        reinterpret_cast<std::uint64_t>(owned_stack), owner_code.address + 36, counter};
    LumenAndroidFexCoreGuestStateV3 state {};
    state.gp.rip = owner_code.address;
    state.gp.gprs[4] = run.ownedBase + HOST_PAGE - 8;
    state.gp.gprs[3] = SENTINEL;
    state.gp.gprs[12] = ~SENTINEL;
    const auto root_return = LumenAndroidFexCoreRootReturnAddressV1();
    std::memcpy(reinterpret_cast<void*>(state.gp.gprs[4]), &root_return, 8);
    const auto result = ok ? LumenMacOSFexCoreRunGuestWithOwnedStackV3(&state,
        RelocationEpochHostCall, Query, &run, run.ownedBase, HOST_PAGE, RelocationEpochOwnedStackQuery) : UINT32_MAX;
    std::uint64_t hle_return = 0;
    std::uint32_t original_output = 0, patched_output = 0;
    std::memcpy(&original_output, output.Bytes(), sizeof(original_output));
    std::memcpy(&hle_return, output.Bytes(8), 8);
    std::memcpy(&patched_output, output.Bytes(16), sizeof(patched_output));
    ok = Check(result == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 && run.calls == 1 && run.valid &&
               original_output == 0x11 && patched_output == 0x22 && hle_return == SENTINEL &&
               state.gp.gprs[0] == 0x22 && state.gp.gprs[3] == SENTINEL && state.gp.gprs[12] == ~SENTINEL,
               "owner returns through HLE bridge with its RAX and fresh RX code, preserving nonvolatile registers") && ok;
    ok = Check(wait_for([&] { return __atomic_load_n(counter, __ATOMIC_ACQUIRE) > run.finalParkedCounter &&
                                    __atomic_load_n(returned, __ATOMIC_ACQUIRE) == 0x22; }),
               "HLE batch peer executes fresh RX after final reader readmission") && ok;
    __atomic_store_n(stop, std::uint8_t {1}, __ATOMIC_RELEASE);
    peer.join();
    ok = Check(peer_status == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 && munmap(owned_stack, HOST_PAGE) == 0,
               "HLE epoch owner and peer finish with no retained writer") && ok;
    std::fprintf(stderr, "fex-relocation-hle-batch attempts=258 epochs=2 owner=%u peer=%u result=%s\n",
                 result, peer_status, ok ? "PASS" : "FAIL");
    return ok;
}

struct OwnedPermissionCase {
    std::uint64_t address, size, ownedBase, ownedSize;
    bool write, expected;
};
bool ownedStackReadTest {};
bool ownedStackReadEnabled {};
struct OwnedReadCase {
    std::uint64_t address, size;
    bool expected;
};
struct OwnedPermissionRun {
    std::array<OwnedPermissionCase, 10> cases {};
    std::array<OwnedReadCase, 10> reads {};
    unsigned queries {};
    unsigned calls {};
    bool valid {true};
    bool hasOwnedProof {true};
};
thread_local OwnedPermissionRun* ownedPermissionRun {};

void OwnedHostTarget() {}

std::uint32_t OwnedPermissionQuery(std::uint64_t address, std::uint64_t size, std::uint32_t write,
                                   std::uint64_t ownedBase, std::uint64_t ownedSize) noexcept {
    auto& run = *ownedPermissionRun;
    ++run.queries;
    // The bridge calls this under its execution token; the owned-stack HLE
    // reader calls it under its retained host-memory token. Both exclude
    // destructive mutations. These cases inspect metadata without reading bytes.
    for (const auto& item : run.cases)
        run.valid = run.valid && VM::CheckOwnedStackAccessUnderFexExecutionBarrier(
            item.address, item.size, item.write, item.ownedBase, item.ownedSize) == item.expected;
    return write <= 1 && VM::CheckOwnedStackAccessUnderFexExecutionBarrier(
        address, size, write != 0, ownedBase, ownedSize) ? 1U : 0U;
}

std::uint32_t OwnedHostCall(void*, std::uint64_t, std::uint64_t*,
                           LumenAndroidFexCoreSimdViewV2*, std::uint64_t*) noexcept {
    auto& run = *ownedPermissionRun;
    ++run.calls;
    if (ownedStackReadTest) {
        for (const auto& item : run.reads) {
            std::array<std::uint8_t, 16> output;
            output.fill(0xa7);
            auto expected = output;
            const bool permitted = ownedStackReadEnabled && run.hasOwnedProof && item.expected;
            if (permitted) std::memcpy(expected.data(), reinterpret_cast<const void*>(item.address), item.size);
            const auto result = LumenMacOSFexCoreTryReadOwnedStackForHostCallV1(item.address, output.data(), item.size);
            run.valid = result == (permitted ? 1U : 0U) && output == expected && run.valid;
        }
        std::uint64_t foreign = SENTINEL;
        const auto result = LumenMacOSFexCoreTryReadOwnedStackForHostCallV1(
            reinterpret_cast<std::uint64_t>(&foreign), &foreign, sizeof(foreign));
        run.valid = result == 0 && foreign == SENTINEL && run.valid;
        if (run.hasOwnedProof) {
            const auto source = run.reads[0].address;
            run.valid = LumenMacOSFexCoreTryReadOwnedStackForHostCallV1(source, nullptr, 8) == 0 && run.valid;
            run.valid = LumenMacOSFexCoreTryReadOwnedStackForHostCallV1(
                source, reinterpret_cast<void*>(UINTPTR_MAX - 3), 8) == 0 && run.valid;
            const auto read_in_context = [&](bool reader_present) {
                std::uint64_t output = SENTINEL;
                std::uint64_t expected = SENTINEL;
                const bool permitted = reader_present && ownedStackReadEnabled;
                if (permitted) std::memcpy(&expected, reinterpret_cast<const void*>(source), sizeof(expected));
                const auto copied = LumenMacOSFexCoreTryReadOwnedStackForHostCallV1(source, &output, sizeof(output));
                run.valid = copied == (permitted ? 1U : 0U) && output == expected && run.valid;
            };
            // No Core data leases remain active here. A real host wait drops
            // this callback's memory reader, then restores it before returning.
            const auto wait = LumenAndroidFexCoreBeginQuiescentHostWaitV1();
            run.valid = wait != 0 && run.valid;
            if (wait != 0) {
                read_in_context(false);
                Lumen::Android::FexCoreLab::RelocationPatchEpoch denied(true);
                run.valid = !denied.AdmitNext() && !denied.AdmitNext() &&
                    denied.Attempts() == 0 && denied.Epochs() == 0 && run.valid;
                const bool ended = LumenAndroidFexCoreEndQuiescentHostWaitV1(wait) == 1;
                run.valid = ended && run.valid;
                if (ended) read_in_context(true);
            }
            const auto mutation = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
                LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
            run.valid = mutation != 0 && run.valid;
            if (mutation != 0) {
                read_in_context(false);
                // This probes ownership only; no mapping or bytes are changed.
                LumenAndroidFexCoreEndGuestMemoryMutationV1(mutation, 0, 0, 0);
                read_in_context(true);
            }
        }
    }
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_STOP_V1;
}

bool OwnedStackPermissions() {
    Mapping code;
    Mapping managed_stack;
    void* allocation = mmap(nullptr, 3 * HOST_PAGE, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code.address == 0 || managed_stack.address == 0 || allocation == MAP_FAILED) return false;
    const auto native = reinterpret_cast<std::uint64_t>(allocation);
    auto* bytes = static_cast<std::uint8_t*>(allocation);
    bytes[0] = 0x73;
    bool ok = Check(!VM::AllocFixed(native, HOST_PAGE, VM::Mode::ReadWrite) && bytes[0] == 0x73,
                    "fixed additive mapping cannot replace an owned raw stack");
    if (munmap(bytes + HOST_PAGE, HOST_PAGE) != 0 ||
        !VM::AllocFixed(native + HOST_PAGE, HOST_PAGE, VM::Mode::Read)) return false;
    const auto target = reinterpret_cast<std::uint64_t>(&OwnedHostTarget);
    const std::array<std::uint8_t, 13> instructions {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0, 0xc3};
    std::memcpy(code.Bytes(), instructions.data(), instructions.size());
    std::memcpy(code.Bytes(2), &target, sizeof(target));
    if (!code.Protect(0, HOST_PAGE, VM::Mode::ExecuteRead) ||
        !LumenAndroidFexCoreRegisterHostCallTargetV1(target)) return false;
    VM::ReservationHandle reserved {};
    for (unsigned phase = 0; phase < 4; ++phase) {
        if (phase == 1 && !VM::Protect(native + HOST_PAGE, HOST_PAGE, VM::Mode::NoAccess)) return false;
        if (phase == 2 && (!VM::Free(native + HOST_PAGE) ||
                          !VM::AllocFixed(native + HOST_PAGE, 1, VM::Mode::ReadWrite))) return false;
        if (phase == 3) {
            VM::MutationTransaction transaction;
            if (!VM::Free(native + HOST_PAGE) ||
                !VM::BeginReserveAligned(native + HOST_PAGE, HOST_PAGE, HOST_PAGE, &reserved, &transaction)) return false;
            transaction.Commit();
        }
        OwnedPermissionRun run;
        run.cases = {{
            {native, 8, native, HOST_PAGE, false, true},
            {native, 8, native + 1, HOST_PAGE - 1, false, false},
            {native + HOST_PAGE - 4, 8, native, 3 * HOST_PAGE, true, false},
            {native + HOST_PAGE - 4, 8, native, 3 * HOST_PAGE, false, phase == 0},
            {native + HOST_PAGE, 1, native, 3 * HOST_PAGE, true, phase == 2},
            {native + HOST_PAGE + 1, 1, native, 3 * HOST_PAGE, false, phase == 0},
            {native + HOST_PAGE, 1, native, HOST_PAGE, false, false},
            {native, 0, native, HOST_PAGE, false, false},
            {UINT64_MAX - 3, 8, native, HOST_PAGE, false, false},
            {native, 8, native, UINT64_MAX, false, false},
        }};
        run.reads = {{
            {native, 8, true},
            {native + HOST_PAGE - 4, 8, phase == 0},
            {native + HOST_PAGE, 1, phase == 0 || phase == 2},
            {native + HOST_PAGE + 1, 1, phase == 0},
            {native + 3 * HOST_PAGE - 8, 8, true},
            {native + 3 * HOST_PAGE - 4, 8, false},
            {native - 1, 2, false},
            {native + 3 * HOST_PAGE, 1, false},
            {native, 0, false},
            {UINT64_MAX - 3, 8, false},
        }};
        ownedPermissionRun = &run;
        const auto prepare = [&] {
            LumenAndroidFexCoreGuestStateV3 state {};
            state.gp.rip = code.address;
            state.gp.gprs[4] = native + HOST_PAGE - 8;
            const auto sentinel = LumenAndroidFexCoreRootReturnAddressV1();
            std::memcpy(reinterpret_cast<void*>(state.gp.gprs[4]), &sentinel, sizeof(sentinel));
            return state;
        };
        auto state = prepare();
        const auto status = LumenMacOSFexCoreRunGuestWithOwnedStackV3(
            &state, OwnedHostCall, Query, nullptr, native,
            ownedStackReadTest ? 3 * HOST_PAGE : HOST_PAGE, OwnedPermissionQuery);
        const bool queries_valid = ownedStackReadTest && ownedStackReadEnabled ? run.queries > 1 : run.queries == 1;
        ok = Check(status == 0 && queries_valid && run.calls == 1 && run.valid,
                   "owned stack proof preserves bounds and managed rights across an unmanaged gap") && ok;
        run.queries = run.calls = 0;
        state = prepare();
        const auto narrowed = LumenMacOSFexCoreRunGuestWithOwnedStackV3(
            &state, OwnedHostCall, Query, nullptr, native + HOST_PAGE - 16, 16, OwnedPermissionQuery);
        ok = Check(narrowed == LUMEN_ANDROID_FEXCORE_RUNTIME_HOST_CALL_FAILED_V1 &&
                   run.queries == 0 && run.calls == 0,
                   "native bridge rejects a slot outside explicit ownership before callback") && ok;
        state = prepare();
        const auto strict = LumenAndroidFexCoreRunGuestV3(&state, OwnedHostCall, Query, nullptr);
        ok = Check(strict == LUMEN_ANDROID_FEXCORE_RUNTIME_HOST_CALL_FAILED_V1 && run.calls == 0,
                   "ordinary V3 does not inherit another Run's owned stack") && ok;
        if (ownedStackReadTest) {
            // The ordinary managed stack admits the callback through the full
            // guest-memory path, without granting this API any ownership proof.
            run.hasOwnedProof = false;
            run.queries = run.calls = 0;
            state = prepare();
            state.gp.gprs[4] = managed_stack.address + HOST_PAGE - 8;
            const auto sentinel = LumenAndroidFexCoreRootReturnAddressV1();
            std::memcpy(reinterpret_cast<void*>(state.gp.gprs[4]), &sentinel, sizeof(sentinel));
            const auto missing_proof = LumenAndroidFexCoreRunGuestV3(&state, OwnedHostCall, Query, nullptr);
            ok = Check(missing_proof == 0 && run.calls == 1 && run.queries == 0 && run.valid,
                       "host read without owned proof leaves every output unchanged") && ok;
        }
        ownedPermissionRun = nullptr;
    }
    ok = Check(VM::ReleaseReservation(reserved) && munmap(allocation, HOST_PAGE) == 0 &&
               munmap(bytes + 2 * HOST_PAGE, HOST_PAGE) == 0, "owned stack fixture release") && ok;
    return ok;
}
} // namespace

int main(int argc, char** argv) {
    ownedStackReadEnabled = argc == 2 && std::strcmp(argv[1], "--owned-stack-read-enabled") == 0;
    ownedStackReadTest = ownedStackReadEnabled ||
        (argc == 2 && std::strcmp(argv[1], "--owned-stack-read-disabled") == 0);
    if (argc != 1 && !ownedStackReadTest) return 2;
    if (sysconf(_SC_PAGESIZE) != static_cast<long>(HOST_PAGE) || !VM::RequiresUniformHostPageDataProtection()) return 2;
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    status.structSize = sizeof(status);
    status.abiVersion = 1;
    if (LumenAndroidFexCoreRuntimeProbeV1(&status) != 0) return 3;
    const bool ok = RunTests() && ConcurrentPermissionQueries() && BorrowedDataPermissions() &&
        RelocationPatchEpochNesting() && RelocationPatchEpochHleReturn() && OwnedStackPermissions();
    std::printf("fex-memory-permission scalar/sse/avx/atomic/guard/rollback/concurrent/borrow=%s mixed4k=denied exact4k=unimplemented\n",
                ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
