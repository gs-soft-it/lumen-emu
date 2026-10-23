#include "AndroidFexCoreRuntime.h"
#include "Lumen/Core/VirtualMemory.h"
#include "MemoryHandoff.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <thread>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <unistd.h>

namespace {
namespace VM = Lumen::Core::VirtualMemory;
constexpr std::uint64_t PAGE = 4096;
constexpr std::uint64_t HOST = 16384;
constexpr std::uint64_t VALUE = UINT64_C(0x4142434445464748);

struct AccessDescriptor {
    std::uint64_t address = 0, bytes = 0, rip = 0;
    std::uint32_t write = 0;
};

struct SyntheticPermissions {
    std::uint64_t base = 0;
    std::array<VM::Mode, 4> modes {
        VM::Mode::Read, VM::Mode::ReadWrite, VM::Mode::NoAccess, VM::Mode::ExecuteRead};
    AccessDescriptor denied {};
    std::uint64_t checks = 0;
};

thread_local SyntheticPermissions* current = nullptr;
thread_local std::atomic<std::uint64_t> deniedSignalRip {};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

bool Check(bool condition, const char* label) {
    if (!condition) std::fprintf(stderr, "fex-memory-exact-experiment: %s\n", label);
    return condition;
}

class Mapping {
public:
    Mapping() : address(VM::Alloc(0, HOST, VM::Mode::ReadWrite)) {}
    ~Mapping() { if (address != 0 && !VM::Free(address)) std::abort(); }
    std::uint8_t* Bytes(std::size_t offset = 0) const {
        return reinterpret_cast<std::uint8_t*>(address + offset);
    }
    const std::uint64_t address;
};

std::uint32_t Query(void*, std::uint64_t address, std::uint64_t* base,
                    std::uint64_t* size, std::uint32_t* writable) noexcept {
    VM::CodeReadLease lease;
    const auto page = address & ~(PAGE - 1);
    if (!VM::TryAcquireCodeReadLease(page, PAGE, &lease)) return 0;
    *base = page;
    *size = PAGE;
    *writable = 0;
    VM::ReleaseCodeReadLease(&lease);
    return 1;
}

std::uint32_t DenyHostCall(void*, std::uint64_t, std::uint64_t*, std::uint64_t*) noexcept {
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
}

class Fixture {
public:
    bool Initialize() {
        if (!code.address || !stack.address || !data.address || !output.address) return false;
        Put(0,   {0x48, 0x8b, 0x07, 0xc3});
        Put(64,  {0x48, 0x89, 0x07, 0xc3});
        Put(128, {0x0f, 0xb6, 0x07, 0xc3});
        Put(192, {0x88, 0x07, 0xc3});
        Put(256, {0x0f, 0xb7, 0x07, 0xc3});
        Put(320, {0x66, 0x89, 0x07, 0xc3});
        Put(384, {0x8b, 0x07, 0xc3});
        Put(448, {0x89, 0x07, 0xc3});
        Put(512, {0x48, 0x8b, 0x44, 0x8f, 0x0d, 0xc3});
        Put(576, {0x48, 0x39, 0xd2, 0x48, 0x8b, 0x07, 0x75, 0x01, 0xc3, 0x31, 0xc0, 0xc3});
        Put(640, {0x48, 0x8b, 0x07, 0x48, 0x89, 0x06, 0xc3});
        Put(704, {0xf3, 0x0f, 0x6f, 0x47, 0x40, 0x48, 0x8b, 0x07,
                  0xf3, 0x0f, 0x7f, 0x06, 0xc3});
        Put(768, {0x0f, 0xae, 0x15, 0x79, 0x00, 0x00, 0x00,
                  0x48, 0x8b, 0x07, 0x0f, 0xae, 0x1e, 0xc3});
        const std::uint32_t mxcsr = 0x3f80;
        std::memcpy(code.Bytes(896), &mxcsr, sizeof(mxcsr));
        std::memset(data.Bytes(), 0x35, HOST);
        permissions.base = data.address;
        PutExtendedPrograms();
        return VM::Protect(code.address, HOST, VM::Mode::ExecuteRead);
    }

    LumenAndroidFexCoreGuestStateV1 Run(std::size_t program, std::uint64_t at,
                                       std::uint32_t* result, std::uint64_t destination = 0,
                                       std::uint64_t count = 3) {
        LumenAndroidFexCoreGuestStateV1 state {};
        state.structSize = sizeof(state);
        state.abiVersion = 1;
        state.rip = code.address + program;
        state.gprs[0] = VALUE;
        state.gprs[1] = count;
        state.gprs[2] = UINT64_C(0xfedcba9876543210);
        state.gprs[4] = stack.address + HOST - 8;
        state.gprs[6] = destination == 0 ? output.address : destination;
        state.gprs[7] = at;
        const auto root_return = LumenAndroidFexCoreRootReturnAddressV1();
        std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &root_return, sizeof(root_return));
        permissions.denied = {};
        deniedSignalRip.store(0, std::memory_order_relaxed);
        current = &permissions;
        *result = LumenAndroidFexCoreRunGuestV1(&state, DenyHostCall, Query, nullptr);
        current = nullptr;
        // The registered ownership query carries no RIP. Observe the actual
        // denial BRK's reconstructed guest RIP, never the expected opcode.
        if (permissions.denied.bytes != 0)
            permissions.denied.rip = deniedSignalRip.load(std::memory_order_relaxed);
        return state;
    }

    Mapping code, stack, data, output;
    SyntheticPermissions permissions;
#include "FexGuestMemoryPrograms.inc"
private:
    void Put(std::size_t offset, std::initializer_list<std::uint8_t> bytes) {
        std::memcpy(code.Bytes(offset), bytes.begin(), bytes.size());
    }
};

bool BytesEqual(const std::uint8_t* bytes, std::size_t size, std::uint8_t value) {
    return std::all_of(bytes, bytes + size, [value](std::uint8_t byte) { return byte == value; });
}

bool RunExtendedTests() {
    Fixture f;
    if (!Check(f.Initialize(), "extended memory fixture")) return false;
    bool ok = true;
    std::uint32_t result = UINT32_MAX;
    struct AccessCase { const char* name; std::size_t program, width; bool write, cross; };
    const AccessCase cases[] {
        {"SSE load", f.kSseLoad, 16, false, true}, {"SSE store", f.kSseStore, 16, true, true},
        {"AVX load", f.kAvxLoad, 32, false, true}, {"AVX store", f.kAvxStore, 32, true, true},
        {"non-temporal store", f.kNonTemporal, 16, true, false},
        {"vector element load", f.kElementLoad, 4, false, true},
        {"vector element store", f.kElementStore, 4, true, true},
        {"broadcast", f.kBroadcast, 4, false, true},
        {"push", f.kPush, 8, true, true}, {"pop", f.kPop, 8, false, true},
        {"push pair", f.kPushPair, 16, true, false}, {"pop pair", f.kPopPair, 16, false, false},
        {"atomic xadd8", f.kXadd8, 1, true, false}, {"atomic xadd16", f.kXadd16, 2, true, true},
        {"atomic xadd32", f.kXadd32, 4, true, true}, {"atomic xadd64", f.kXadd64, 8, true, true},
        {"atomic swap", f.kAtomicSwap, 8, true, true}, {"atomic add", f.kAtomicAdd, 8, true, true},
        {"atomic sub", f.kAtomicSub, 8, true, true}, {"atomic and", f.kAtomicAnd, 8, true, true},
        {"atomic or", f.kAtomicOr, 8, true, true}, {"atomic xor", f.kAtomicXor, 8, true, true},
        {"atomic neg", f.kAtomicNeg, 8, true, true}, {"CAS", f.kCas, 8, true, true},
        {"CAS pair", f.kCasPair, 16, true, false},
        {"x87 f80 load", f.kX87Load, 10, false, true}, {"x87 f80 store", f.kX87Store, 10, true, true},
        {"x87 f64 load", f.kX87DoubleLoad, 8, false, true}, {"x87 f64 store", f.kX87DoubleStore, 8, true, true},
    };
    for (const auto& test : cases) {
        for (std::size_t page = 0; page != 4; ++page) {
            std::fprintf(stderr, "checked-memory family=%s page=%zu\n", test.name, page);
            std::memset(f.data.Bytes(), 0x35, HOST);
            std::memset(f.output.Bytes(), 0xcc, HOST);
            std::memset(f.output.Bytes(64), 0x59, 32);
            const auto at = f.data.address + page * PAGE + 64;
            const auto state = f.Run(test.program, at, &result);
            const bool allowed = test.write ? page == 1 : page != 2;
            if (allowed) {
                ok = Check(result == 0, test.name) && ok;
                if (test.write) ok = Check(!BytesEqual(f.data.Bytes(page * PAGE + 64), test.width, 0x35),
                                          "allowed operation actually writes its destination") && ok;
                else if (test.program == f.kPop || test.program == f.kPopPair)
                    ok = Check(state.gprs[0] == UINT64_C(0x3535353535353535), "pop returns loaded value") && ok;
                else ok = Check(!BytesEqual(f.output.Bytes(), 16, 0xcc), "allowed load publishes result") && ok;
            } else {
                const auto& denied = f.permissions.denied;
                ok = Check(result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
                           denied.address >= at && denied.address < at + test.width &&
                           denied.bytes != 0 && denied.bytes <= test.width && denied.write == test.write &&
                           denied.rip >= f.code.address + test.program && denied.rip < f.code.address + test.program + 128,
                           "denied family records its guest footprint and opcode") && ok;
                ok = Check(BytesEqual(f.data.Bytes(page * PAGE + 64), test.width, 0x35) &&
                           BytesEqual(f.output.Bytes(), 32, 0xcc), "denied family preserves memory and output") && ok;
            }
        }
        if (test.cross) {
            std::memset(f.data.Bytes(), 0x35, HOST);
            std::memset(f.output.Bytes(), 0xcc, 64);
            const auto offset = 2 * PAGE - test.width / 2;
            std::fprintf(stderr, "checked-memory family=%s cross-guard\n", test.name);
            f.Run(test.program, f.data.address + offset, &result);
            ok = Check(result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
                       BytesEqual(f.data.Bytes(offset), test.width, 0x35) &&
                       BytesEqual(f.output.Bytes(), 32, 0xcc), "cross-guard operation has no partial store/output") && ok;
        }
    }

    for (const auto program : {f.kMaskedLoad, f.kMaskedStore, f.kMaskedAvxStore}) {
        const bool write = program != f.kMaskedLoad;
        std::fprintf(stderr, "checked-memory masked program=%zu\n", program);
        const auto offset = 2 * PAGE - 8;
        std::array<std::int32_t, 8> masks {INT32_MIN, INT32_MIN, 0, 0, 0, 0, 0, 0};
        std::memset(f.data.Bytes(), 0x35, HOST);
        std::memset(f.output.Bytes(), 0xcc, HOST);
        std::memset(f.output.Bytes(64), 0x59, 32);
        std::memcpy(f.output.Bytes(128), masks.data(), sizeof(masks));
        f.Run(program, f.data.address + offset, &result);
        ok = Check(result == 0 && BytesEqual(f.data.Bytes(2 * PAGE), 24, 0x35),
                   "inactive masked lanes in guard never access memory") && ok;
        if (write) ok = Check(BytesEqual(f.data.Bytes(offset), 8, 0x59), "active masked stores write") && ok;
        else ok = Check(BytesEqual(f.output.Bytes(), 8, 0x35) && BytesEqual(f.output.Bytes(8), 8, 0),
                        "masked load zeroes inactive lanes") && ok;
        // A later active forbidden lane must be checked before earlier stores.
        masks[program == f.kMaskedAvxStore ? 7 : 3] = INT32_MIN;
        std::memset(f.data.Bytes(), 0x35, HOST);
        std::memset(f.output.Bytes(), 0xcc, 32);
        std::memcpy(f.output.Bytes(128), masks.data(), sizeof(masks));
        f.Run(program, f.data.address + offset, &result);
        ok = Check(result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
                   BytesEqual(f.data.Bytes(offset), 32, 0x35) && BytesEqual(f.output.Bytes(), 32, 0xcc),
                   "masked access preflights later active lane before any effect") && ok;
        masks.fill(0);
        std::memcpy(f.output.Bytes(128), masks.data(), sizeof(masks));
        f.Run(program, UINT64_MAX - 4, &result);
        ok = Check(result == 0, "zero mask ignores even invalid base address") && ok;
    }

    for (const auto program : {f.kGather, f.kGatherQ}) {
        std::fprintf(stderr, "checked-memory gather program=%zu\n", program);
        std::memset(f.output.Bytes(), 0xcc, HOST);
        std::memset(f.output.Bytes(64), 0x59, 16);
        std::memset(f.data.Bytes(), 0x35, HOST);
        std::array<std::int32_t, 4> masks {INT32_MIN, 0, INT32_MIN, 0};
        const std::array<std::int32_t, 4> indexes {16, 1024, -1, 1024};
        const std::array<std::int64_t, 2> qindexes {-1, 1024};
        std::memcpy(f.output.Bytes(128), masks.data(), sizeof(masks));
        if (program == f.kGather) std::memcpy(f.output.Bytes(192), indexes.data(), sizeof(indexes));
        else std::memcpy(f.output.Bytes(192), qindexes.data(), sizeof(qindexes));
        f.Run(program, f.data.address + PAGE + 64, &result);
        ok = Check(result == 0 && BytesEqual(f.output.Bytes(), 4, 0x35) &&
                   BytesEqual(f.output.Bytes(4), 4, 0x59), "gather signed indexes and inactive guard lane") && ok;
        masks[1] = INT32_MIN;
        std::memcpy(f.output.Bytes(128), masks.data(), sizeof(masks));
        std::memset(f.output.Bytes(), 0xcc, 16);
        f.Run(program, f.data.address + PAGE + 64, &result);
        ok = Check(result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
                   f.permissions.denied.address == f.data.address + 2 * PAGE + 64 &&
                   f.permissions.denied.bytes == 4 && BytesEqual(f.output.Bytes(), 16, 0xcc),
                   "gather denies active guard lane with precise data address") && ok;
    }

    for (const auto program : {f.kRepMovsb, f.kRepMovsq, f.kRepMovsbBack,
                               f.kRepStosb, f.kRepStosq, f.kRepStosbBack}) {
        const bool copy = program == f.kRepMovsb || program == f.kRepMovsq || program == f.kRepMovsbBack;
        const bool back = program == f.kRepMovsbBack || program == f.kRepStosbBack;
        const std::size_t width = program == f.kRepMovsq || program == f.kRepStosq ? 8 : 1;
        std::fprintf(stderr, "checked-memory REP program=%zu\n", program);
        std::memset(f.data.Bytes(), 0x35, HOST);
        std::memset(f.output.Bytes(), 0xcc, HOST);
        const auto at = f.data.address + PAGE + 64;
        auto state = f.Run(program, at, &result, f.output.address + 64, 16);
        ok = Check(result == 0 && state.gprs[1] == 0 &&
                   state.gprs[7] == (copy ? f.output.address + 64 : at) + (back ? -16 : 16 * width),
                   "REP count and final destination on successful completion") && ok;
        if (copy) ok = Check(BytesEqual(f.output.Bytes(back ? 49 : 64), 16 * width, 0x35), "REP copies all elements") && ok;
        else ok = Check(!BytesEqual(f.data.Bytes(PAGE + (back ? 49 : 64)), 16 * width, 0x35), "REP sets all elements") && ok;
        // The failing element has no effects. Earlier completed elements keep
        // their memory effects; resumable CPU checkpoints remain a separate gate.
        std::memset(f.data.Bytes(), 0x35, HOST);
        std::memset(f.output.Bytes(), 0xcc, HOST);
        const std::size_t start = back ? PAGE + 3 : 2 * PAGE - 4 * width;
        if (copy && back) f.permissions.modes[0] = VM::Mode::NoAccess;
        f.Run(program, f.data.address + start, &result, f.output.address + 64, 8);
        const auto done_begin = back ? 61 : 64;
        ok = Check(result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
                   f.permissions.denied.bytes == width, "REP checks the first forbidden element") && ok;
        if (copy) ok = Check(BytesEqual(f.output.Bytes(done_begin), 4 * width, 0x35) &&
                             BytesEqual(f.output.Bytes(back ? 60 : 64 + 4 * width), width, 0xcc),
                             "REP preserves prefix but not forbidden source element") && ok;
        else {
            const auto changed_begin = back ? PAGE : start;
            ok = Check(!BytesEqual(f.data.Bytes(changed_begin), 4 * width, 0x35) &&
                       BytesEqual(f.data.Bytes(back ? PAGE - 1 : 2 * PAGE), width, 0x35),
                       "REP never writes the forbidden destination element") && ok;
        }
        f.permissions.modes[0] = VM::Mode::Read;
        f.Run(program, f.data.address + 2 * PAGE, &result, f.output.address, 0);
        ok = Check(result == 0, "REP zero count ignores inaccessible pointers") && ok;
    }
    const auto cache_features = f.Run(f.kCacheFeatures, f.data.address, &result);
    ok = Check(result == 0, "cache-feature CPUID query") && ok;
    const bool has_clzero = (cache_features.gprs[0] & 1) != 0;
    for (const std::size_t page : {std::size_t{0}, std::size_t{1}, std::size_t{2}}) {
        std::memset(f.data.Bytes(), 0x35, HOST);
        std::memset(f.output.Bytes(64), 0x59, 32);
        f.Run(f.kAvxNegativeStore, f.data.address + page * PAGE + 96, &result);
        ok = Check(page == 1 ? result == 0 && BytesEqual(f.data.Bytes(PAGE + 64), 32, 0x59) :
                              result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
                              f.permissions.denied.address == f.data.address + page * PAGE + 64 &&
                              BytesEqual(f.data.Bytes(page * PAGE + 64), 32, 0x35),
                   "paired SIMD signed negative displacement") && ok;
        f.Run(f.kCacheFlush, f.data.address + page * PAGE + 65, &result);
        ok = Check(page == 2 ? result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 : result == 0,
                   "cache flush checks guest read permission even with host barrier fallback") && ok;
        std::memset(f.data.Bytes(page * PAGE + 64), 0x35, 64);
        const auto zero_state = f.Run(f.kCacheZero, f.data.address + page * PAGE + 77, &result);
        if (has_clzero) {
            ok = Check(page == 1 ? result == 0 && BytesEqual(f.data.Bytes(PAGE + 64), 64, 0) :
                                  result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
                                  f.permissions.denied.bytes == 64 && BytesEqual(f.data.Bytes(page * PAGE + 64), 64, 0x35),
                       "cache-line zero checks its complete aligned write footprint") && ok;
        } else {
            ok = Check(result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 && zero_state.faultSignal == SIGILL &&
                       f.permissions.denied.bytes == 0 && BytesEqual(f.data.Bytes(page * PAGE + 64), 64, 0x35),
                       "unexposed CLZERO raises illegal opcode before accessing guest memory") && ok;
        }
    }
    std::printf("fex-memory-clzero exposed=%u runtimeFootprint=%s\n", has_clzero, has_clzero ? "checked" : "NOT_EXERCISED");
    f.Run(f.kPrefetch, f.data.address + 2 * PAGE, &result);
    ok = Check(result == 0, "prefetch remains non-faulting on a logical guard") && ok;

    // Two independently admitted Runs update one checked RW guest location.
    // Permission callbacks must stay outside the exclusive sequence.
    std::uint64_t zero = 0;
    std::memcpy(f.data.Bytes(PAGE + 64), &zero, sizeof(zero));
    std::atomic<unsigned> ready {0};
    std::atomic<bool> start {false}, threads_ok {true};
    auto worker = [&] {
        Fixture local;
        const bool initialized = local.Initialize();
        local.permissions.base = f.data.address;
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        std::uint32_t code = UINT32_MAX;
        if (initialized) local.Run(local.kAtomicLoop, f.data.address + PAGE + 64, &code);
        if (!initialized || code != 0) threads_ok.store(false, std::memory_order_relaxed);
    };
    std::thread first(worker), second(worker);
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    start.store(true, std::memory_order_release);
    first.join(); second.join();
    std::uint64_t total = 0;
    std::memcpy(&total, f.data.Bytes(PAGE + 64), sizeof(total));
    ok = Check(threads_ok.load() && total == 256, "checked atomics retain atomicity across two Runs") && ok;
    std::printf("fex-memory-family-checks fixed/simd/stack/atomic/masked/gather/rep/x87=%s\n", ok ? "PASS" : "FAIL");
    return ok;
}

bool RunTests() {
    Fixture f;
    if (!Check(f.Initialize(), "original scalar fixture")) return false;
    bool ok = true;
    std::uint32_t result = UINT32_MAX;
    const std::array<std::size_t, 4> loads {128, 256, 384, 0};
    const std::array<std::size_t, 4> stores {192, 320, 448, 64};
    const std::array<std::size_t, 4> widths {1, 2, 4, 8};
    for (std::size_t index = 0; index < widths.size(); ++index) {
        const auto width = widths[index];
        const auto mask = width == 8 ? UINT64_MAX : (UINT64_C(1) << (width * 8)) - 1;
        for (const std::size_t page : {std::size_t{0}, std::size_t{1}, std::size_t{3}}) {
            const auto state = f.Run(loads[index], f.data.address + page * PAGE + 64, &result);
            ok = Check(result == 0 && (state.gprs[0] & mask) == (UINT64_C(0x3535353535353535) & mask),
                       "scalar reads on synthetic R/RW/RX") && ok;
        }
        f.Run(stores[index], f.data.address + PAGE + 64, &result);
        std::uint64_t written = 0;
        std::memcpy(&written, f.data.Bytes(PAGE + 64), width);
        ok = Check(result == 0 && written == (VALUE & mask), "scalar write on synthetic RW") && ok;
        std::memset(f.data.Bytes(PAGE + 64), 0x35, width);
        for (const std::size_t page : {std::size_t{0}, std::size_t{2}, std::size_t{3}}) {
            const auto at = f.data.address + page * PAGE + 64;
            f.Run(stores[index], at, &result);
            const auto& denied = f.permissions.denied;
            ok = Check(result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
                       denied.address == at && denied.bytes == width && denied.write == 1 &&
                       denied.rip == f.code.address + stores[index],
                       "denied scalar store descriptor with precise RIP") && ok;
            for (std::size_t byte = 0; byte < width; ++byte)
                ok = Check(f.data.Bytes(page * PAGE + 64)[byte] == 0x35,
                           "denied store has no partial effects") && ok;
        }
    }
    for (const std::size_t boundary : {PAGE, 2 * PAGE, 3 * PAGE}) {
        const auto at = f.data.address + boundary - 4;
        std::memset(f.output.Bytes(), 0xcc, 8);
        const auto state = f.Run(640, at, &result);
        if (boundary == PAGE) {
            ok = Check(result == 0 && state.gprs[0] == UINT64_C(0x3535353535353535),
                       "cross-page read spans R and RW") && ok;
        } else {
            ok = Check(result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
                       f.permissions.denied.address == at && f.permissions.denied.bytes == 8 &&
                       f.permissions.denied.rip == f.code.address + 640 &&
                       *reinterpret_cast<const std::uint64_t*>(f.output.Bytes()) == UINT64_C(0xcccccccccccccccc),
                       "cross-page denied load publishes no output") && ok;
        }
        f.Run(64, at, &result);
        ok = Check(result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
                   f.permissions.denied.address == at && f.permissions.denied.write == 1,
                   "cross-page store checks both pages before effects") && ok;
        for (std::size_t byte = 0; byte < 8; ++byte)
            ok = Check(f.data.Bytes(boundary - 4)[byte] == 0x35, "cross-page store preserves prefix and suffix") && ok;
    }
    auto state = f.Run(512, f.data.address + PAGE + 64 - 25, &result);
    ok = Check(result == 0 && state.gprs[0] == UINT64_C(0x3535353535353535),
               "scaled indexed operand checks effective address") && ok;
    f.Run(512, f.data.address + 2 * PAGE + 64 - 25, &result);
    ok = Check(result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
               f.permissions.denied.address == f.data.address + 2 * PAGE + 64,
               "scaled indexed descriptor identifies guard") && ok;
    state = f.Run(576, f.data.address + 64, &result);
    ok = Check(result == 0 && state.gprs[0] == UINT64_C(0x3535353535353535) &&
               state.gprs[2] == UINT64_C(0xfedcba9876543210),
               "callback preserves flags and live GPRs") && ok;
    f.Run(576, f.data.address + 2 * PAGE + 64, &result);
    ok = Check(result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
               f.permissions.denied.rip == f.code.address + 579,
               "descriptor RIP follows the actual opcode after earlier instructions") && ok;
    f.Run(704, f.data.address + 64, &result);
    ok = Check(result == 0 && std::memcmp(f.output.Bytes(), f.data.Bytes(128), 16) == 0,
               "scalar callback preserves live 128-bit vector despite explicit host clobber") && ok;
    f.Run(768, f.data.address + 64, &result);
    std::uint32_t mxcsr = 0;
    std::memcpy(&mxcsr, f.output.Bytes(), sizeof(mxcsr));
    ok = Check(result == 0 && mxcsr == 0x3f80,
               "scalar callback restores FP control despite explicit host clobber") && ok;
    ok = Check(f.permissions.checks != 0 && VM::RequiresUniformHostPageDataProtection() &&
               !VM::Protect(f.data.address, PAGE, VM::Mode::Read),
               "experiment does not admit mixed mappings in product VM") && ok;
    return ok;
}
} // namespace

// Defined only by this executable. The product's typed providers and JIT ABI
// remain intact; this registered query adds a synthetic permission oracle.
static std::uint32_t SyntheticMemoryCheck(std::uint64_t address, std::uint64_t bytes,
                                       std::uint32_t write) noexcept {
    // Startup's separate native-stack probe is outside this synthetic mapping
    // experiment. The product never registers this synthetic oracle.
    if (current == nullptr) return 1;
    if (address == 0 || bytes == 0 || address > UINT64_MAX - bytes || write > 1) return 0;
    const auto end = address + bytes;
    if (current != nullptr) {
        ++current->checks;
        const auto begin = current->base;
        const auto limit = begin + HOST;
        if (address < limit && end > begin) {
            bool permitted = address >= begin && end <= limit;
            if (permitted) {
                const auto first = (address - begin) / PAGE;
                const auto last = (end - 1 - begin) / PAGE;
                for (auto page = first; page <= last; ++page) {
                    const auto mode = current->modes[page];
                    permitted = mode != VM::Mode::NoAccess &&
                        (write == 0 || mode == VM::Mode::ReadWrite) && permitted;
                }
            }
            if (!permitted) {
                current->denied = {address, bytes, 0, write};
                return 0;
            }
        }
    }
    // The existing ExecutionBarrierLease pins the mapping through the generated
    // access. A callback running in the JIT must not wait for a VM mutator that
    // may itself be waiting for this executor to reach its next safepoint.
    const bool permitted = VM::CheckGuestDataAccessUnderFexExecutionBarrier(address, bytes, write != 0);
    // Deliberately disturb host status and caller-saved vectors so the bridge's
    // preservation is tested even when the C++ permission lookup uses no SIMD.
    const std::uint64_t flags = UINT64_C(0xf0000000), status = 0x9f;
    asm volatile("msr nzcv, %0\n\tmsr fpcr, xzr\n\tmsr fpsr, %1\n\t"
                 "movi v0.16b, #0\n\tmovi v1.16b, #0\n\tmovi v2.16b, #0\n\tmovi v3.16b, #0\n\t"
                 "movi v4.16b, #0\n\tmovi v5.16b, #0\n\tmovi v6.16b, #0\n\tmovi v7.16b, #0\n\t"
                 "movi v16.16b, #0\n\tmovi v17.16b, #0\n\tmovi v18.16b, #0\n\tmovi v19.16b, #0\n\t"
                 "movi v20.16b, #0\n\tmovi v21.16b, #0\n\tmovi v22.16b, #0\n\tmovi v23.16b, #0\n\t"
                 "movi v24.16b, #0\n\tmovi v25.16b, #0\n\tmovi v26.16b, #0\n\tmovi v27.16b, #0\n\t"
                 "movi v28.16b, #0\n\tmovi v29.16b, #0\n\tmovi v30.16b, #0\n\tmovi v31.16b, #0"
                 : : "r"(flags), "r"(status) : "cc", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
                   "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
                   "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
    return permitted ? 1 : 0;
}

static bool SyntheticHandoff(std::uint64_t address, std::uint64_t bytes,
                             Lumen::GuestCpu::MemoryAccessIntent intent) noexcept {
    return SyntheticMemoryCheck(address, bytes, Lumen::GuestCpu::WritesMemory(intent) ? 1U : 0U) != 0;
}

extern "C" void FEXLumenGuestMemoryHandoffDeniedSignalV1(std::uint64_t rip) noexcept {
    // Signal-context witness: only a lock-free store to already-used TLS.
    deniedSignalRip.store(rip, std::memory_order_relaxed);
}

int main() {
    if (sysconf(_SC_PAGESIZE) != static_cast<long>(HOST)) return 2;
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    status.structSize = sizeof(status);
    status.abiVersion = 1;
    if (LumenAndroidFexCoreRuntimeProbeV1(&status) != 0) return 3;
    if (LumenAndroidFexCoreMemoryHandoffInstrumentationReadyV1() != 1 ||
        !Lumen::GuestCpu::RegisterMemoryHandoffQuery(SyntheticHandoff)) return 3;
    const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
        LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
    if (token == 0 || LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(token) != 1) return 3;
    // All generated data accesses keep the original fixture's check coverage,
    // including stack, instruction data, REP helpers and synthetic subpages.
    Lumen::GuestCpu::MarkHandoffRange(0, UINT64_C(1) << 32);
    Lumen::GuestCpu::PublishMemoryHandoffGpuDirty(true);
    if (LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) != 1) return 3;
    const bool scalars = RunTests();
    const bool extended = RunExtendedTests();
    const bool ok = scalars && extended;
    std::printf("fex-memory-exact-experiment scalar8/16/32/64/cross-page/descriptor/token=%s "
                "synthetic4k=checked productMixed4k=denied generalMemoryCoverage=0"
                " product_typed_provider=retained denied_RIP=actual_signal_witness\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
