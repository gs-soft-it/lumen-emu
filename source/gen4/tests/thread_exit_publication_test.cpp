#ifdef LUMEN_GEN4_DIRECT_FEX
// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/guest_cpu/fex_guest_cpu.h"
#include "core/guest_cpu/fex_hle_bridge.h"
#include "core/thread.h"
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)
namespace {
using namespace Core;
struct Mapping {
    std::size_t size{static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
    u8* data{static_cast<u8*>(mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0))};
    Mapping() { CHECK(data != MAP_FAILED); }
    ~Mapping() { CHECK(munmap(data, size) == 0); }
    std::uintptr_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }
};
struct Fixture {
    std::vector<GuestExecutionRange> ranges;
    static bool Access(void* self, std::uintptr_t address, std::size_t size, bool write) {
        for (const auto& range : static_cast<Fixture*>(self)->ranges)
            if (address >= range.Begin && address - range.Begin < range.Size &&
                size <= range.Size - (address - range.Begin) && (!write || range.Writable)) return true;
        return false;
    }
    static std::optional<GuestExecutionRange> Executable(void* self, std::uintptr_t address) {
        for (const auto& range : static_cast<Fixture*>(self)->ranges)
            if (range.Executable && address >= range.Begin && address - range.Begin < range.Size)
                return range;
        return std::nullopt;
    }
};
FexGuestCpuBackend* backend{};
std::uintptr_t child_entry{};
struct RunContext {
    NativeThread native;
    GuestExecutionRequest request;
    std::atomic_bool ready{};
    std::atomic_uint cleanups{};
    std::atomic_bool after_exit{}, after_callback{}, after_run{};
};
thread_local RunContext* active{};
void InnerCleanup(void* opaque) {
    auto& context = *static_cast<RunContext*>(opaque);
    CHECK(context.cleanups.fetch_add(1) == 0);
}
u64 NativeExit() {
    CHECK(active != nullptr && GuestCpu::IsGuestHleCallActive());
    pthread_cleanup_push(InnerCleanup, active);
    active->native.Exit();
    active->after_exit = true;
    pthread_cleanup_pop(1);
    return 0;
}
u64 NestedExit() {
    const std::array<u64, 0> arguments{};
    const auto result = backend->CallGuest(child_entry, arguments);
    (void)result;
    active->after_callback = true;
    CHECK(false);
    return 0;
}
void OuterCleanup(void* opaque) {
    auto& context = *static_cast<RunContext*>(opaque);
    CHECK(context.cleanups.fetch_add(1) == 1);
    CHECK(!context.after_exit && !context.after_callback && !context.after_run);
    // Observe the native cleanup order only. Thread ownership and teardown
    // belong exclusively to the production backend Run(request) path.
}
void* RunNative(void* opaque) {
    auto& context = *static_cast<RunContext*>(opaque);
    context.ready.wait(false);
    active = &context;
    context.native.Initialize();
    pthread_cleanup_push(OuterCleanup, &context);
    const auto result = backend->Run(context.request);
    (void)result;
    context.after_run = true;
    pthread_cleanup_pop(1);
    CHECK(false);
    return nullptr;
}
}
int main() {
    GuestCpu::HleCallRegistry registry;
    GuestCpu::HleVeneerAllocator veneers;
    const auto allocate = [&](auto function, const char* name) {
        const auto adapter = registry.Register(GuestCpu::MakeHleCallAdapter(function), name);
        CHECK(adapter != nullptr);
        const auto result = veneers.Allocate(*adapter);
        CHECK(std::holds_alternative<u64>(result));
        return std::get<u64>(result);
    };
    const auto direct = allocate(NativeExit, "native-thread-exit");
    const auto nested = allocate(NestedExit, "nested-native-thread-exit");
    Mapping code, stack;
    const u8 program[]{0x48,0x83,0xec,8,0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xd0,0x0f,0x0b};
    for (const auto [offset, veneer] : {std::pair{0, direct}, std::pair{64, nested}}) {
        std::memcpy(code.data + offset, program, sizeof(program));
        std::memcpy(code.data + offset + 6, &veneer, 8);
    }
    CHECK(mprotect(code.data, code.size, PROT_READ) == 0);
    Fixture fixture{veneers.GetExecutableRanges()};
    fixture.ranges.push_back({code.Address(), code.size, true, false});
    fixture.ranges.push_back({stack.Address(), stack.size, false, true});
    GuestCpu::HleGuestBridge bridge{registry, Fixture::Access, &fixture, nullptr, nullptr,
                                    Fixture::Executable, &fixture};
    auto created = FexGuestCpuBackend::Create(bridge);
    CHECK(std::holds_alternative<std::unique_ptr<FexGuestCpuBackend>>(created));
    auto owner = std::move(std::get<std::unique_ptr<FexGuestCpuBackend>>(created));
    backend = owner.get();
    fixture.ranges.push_back(backend->ReturnRange());
    fixture.ranges.push_back(backend->CallbackReturnRange());
    child_entry = code.Address();
    const auto ret = backend->ReturnAddress();
    for (unsigned iteration = 0; iteration < 3; ++iteration) {
        for (const bool callback : {false, true}) {
            RunContext context{};
            context.request.Rip = code.Address() + (callback ? 64 : 0);
            context.request.Rsp = stack.Address() + stack.size - 8;
            context.request.Gpr[4] = context.request.Rsp;
            context.request.Rflags = 2;
            context.request.MappedRanges = fixture.ranges;
            std::memcpy(reinterpret_cast<void*>(context.request.Rsp), &ret, 8);
            CHECK(context.native.Create(RunNative, &context) == 0);
            const auto handle = std::bit_cast<pthread_t>(context.native.GetHandle());
            context.ready = true;
            context.ready.notify_one();
            CHECK(pthread_join(handle, nullptr) == 0);
            CHECK(context.cleanups == 2);
            CHECK(!context.after_exit && !context.after_callback && !context.after_run);
        }
    }
    std::fprintf(stderr, "native-thread-exit=PASS direct+nested production backend; teardown=CHECKING; kernel TryCollect not covered\n");
    // Destruction exercises the production Shutdown check. A retained thread
    // must fail here; the harness must not destroy it on the backend's behalf.
    owner.reset();
    backend = nullptr;
    std::puts("native-thread-exit-teardown=PASS production backend owner released");
}

#else
// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/fex/fex_guest_engine.h"
#include "core/fex/guest_memory_scope.h"
#include "core/guest_cpu/fex_hle_bridge.h"
#include "core/libraries/kernel/threads.h"
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

namespace Libraries::Kernel { void PS4_SYSV_ABI posix_pthread_testcancel(); }
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)
namespace {
using namespace Core;
using namespace Libraries::Kernel;
struct Mapping {
    std::size_t size{static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
    unsigned char* data{static_cast<unsigned char*>(mmap(nullptr, size,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0))};
    Mapping() { CHECK(data != MAP_FAILED); }
    ~Mapping() { CHECK(munmap(data, size) == 0); }
    std::uintptr_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }
};
struct Fixture {
    std::vector<GuestExecutionRange> ranges;
    static bool Access(void* self, std::uintptr_t address, std::size_t size, bool write) {
        for (const auto& range : static_cast<Fixture*>(self)->ranges)
            if (address >= range.Begin && address - range.Begin < range.Size &&
                size <= range.Size - (address - range.Begin) && (!write || range.Writable)) return true;
        return false;
    }
    static std::optional<GuestExecutionRange> Executable(void* self, std::uintptr_t address) {
        for (const auto& range : static_cast<Fixture*>(self)->ranges)
            if (range.Executable && address >= range.Begin && address - range.Begin < range.Size)
                return range;
        return std::nullopt;
    }
};

Fex::GuestEngine* engine{};
std::uintptr_t childEntry{};
std::atomic<unsigned> cleanups{};
void PS4_SYSV_ABI Cleanup(void*) { ++cleanups; }
std::uint64_t NestedExit(std::uint64_t cancel) {
    const std::array<std::uint64_t, 1> arguments{0x1234};
    const auto result = engine->CallGuest(childEntry + (cancel ? 64 : 0), arguments);
    CHECK(std::holds_alternative<GuestExecutionState>(result));
    CHECK(std::get<GuestExecutionState>(result).StopReason == GuestStopReason::ThreadExited);
    throw Fex::GuestThreadExit{};
}
std::uint64_t ThrowException() { throw std::runtime_error("expected HLE exception"); }
struct RunContext { GuestExecutionRequest request; bool cancel{}, exception{}; };
void* RunNativeThread(void* opaque) {
    auto& context = *static_cast<RunContext*>(opaque);
    // This fixture isolates the real kernel exit/cancel HLE and real FEX
    // teardown; its native pthread ends only after the translated Run ends.
    Pthread kernelThread{};
    g_curthread = &kernelThread;
    kernelThread.cancel_enable = context.cancel;
    kernelThread.cancel_pending = context.cancel;
    PthreadCallbacks().PushCleanup(reinterpret_cast<uintptr_t>(&kernelThread),
        {reinterpret_cast<uintptr_t>(&Cleanup), 0, GuestCpu::CallbackOrigin::Native});
    auto created = engine->CreateThread(context.request);
    CHECK(std::holds_alternative<Fex::GuestEngine::Thread*>(created));
    auto* guest = std::get<Fex::GuestEngine::Thread*>(created);
    const auto result = engine->Run(*guest);
    if (context.exception) {
        CHECK(std::holds_alternative<Fex::EngineFailure>(result));
        CHECK(!kernelThread.cancelling);
        PthreadCallbacks().Clear(reinterpret_cast<uintptr_t>(&kernelThread));
    } else {
        CHECK(std::holds_alternative<GuestExecutionState>(result));
        CHECK(std::get<GuestExecutionState>(result).StopReason == GuestStopReason::ThreadExited);
        CHECK(kernelThread.cancelling && kernelThread.no_cancel);
        CHECK(kernelThread.ret == reinterpret_cast<void*>(context.cancel ? 1 : 0x1234));
        CHECK(!PthreadCallbacks().PopCleanup(reinterpret_cast<uintptr_t>(&kernelThread)));
    }
    CHECK(std::holds_alternative<bool>(engine->DestroyThread(guest)));
    CHECK(!GuestCpu::IsGuestHleCallActive());
#ifndef LUMEN_GEN4_DIRECT_FEX
    CHECK(LumenAndroidFexCoreOwnsGuestMemoryReadLeaseV1() == 0);
#endif
    g_curthread = nullptr;
    pthread_exit(nullptr);
}
}

int main() {
    auto registry = std::make_unique<GuestCpu::HleCallRegistry>();
    GuestCpu::HleVeneerAllocator veneers;
    const auto allocate = [&](auto function, const char* name) {
        const auto adapter = registry->Register(GuestCpu::MakeHleCallAdapter(function), name);
        CHECK(adapter);
        const auto allocated = veneers.Allocate(*adapter);
        CHECK(std::holds_alternative<u64>(allocated));
        return std::get<u64>(allocated);
    };
    const auto exitVeneer = allocate(posix_pthread_exit, "thread-exit");
    const auto cancelVeneer = allocate(posix_pthread_testcancel, "thread-cancel");
    const auto nestedVeneer = allocate(NestedExit, "nested-thread-exit");
    const auto exceptionVeneer = allocate(ThrowException, "thread-exception");
    Mapping code, stack, data;
    Fixture fixture{veneers.GetExecutableRanges()};
    fixture.ranges.push_back({code.Address(), code.size, true, false});
    fixture.ranges.push_back({stack.Address(), stack.size, false, true});
    fixture.ranges.push_back({data.Address(), data.size, false, true});
    GuestCpu::HleGuestBridge bridge{*registry, Fixture::Access, &fixture, nullptr, nullptr,
        Fixture::Executable, &fixture};
    auto created = Fex::GuestEngine::Create(bridge);
    CHECK(std::holds_alternative<std::unique_ptr<Fex::GuestEngine>>(created));
    auto owner = std::move(std::get<std::unique_ptr<Fex::GuestEngine>>(created));
    engine = owner.get();
#ifdef LUMEN_GEN4_DIRECT_FEX
    fixture.ranges.push_back(engine->ReturnRange());
    fixture.ranges.push_back(engine->CallbackReturnRange());
#endif
    const unsigned char program[]{0x48,0x83,0xec,8,0x48,0xb8,0,0,0,0,0,0,0,0,
        0xff,0xd0,0x0f,0x0b}; // UD2 proves guest code after exit is never run.
    {
        Fex::MemoryMutationScope publication;
        CHECK(publication.Valid());
        for (const auto [offset, veneer] : {std::pair{0, exitVeneer},
                std::pair{64, cancelVeneer}, std::pair{128, nestedVeneer},
                std::pair{192, exceptionVeneer}}) {
            std::memcpy(code.data + offset, program, sizeof(program));
            std::memcpy(code.data + offset + 6, &veneer, sizeof(veneer));
        }
        CHECK(mprotect(code.data, code.size, PROT_READ) == 0);
        publication.Publish(code.Address(), code.size);
    }
    childEntry = code.Address();
    const auto ret = engine->ReturnAddress();
    for (unsigned iteration = 0; iteration != 3; ++iteration)
    for (const bool nested : {false, true}) for (const bool cancel : {false, true}) {
        RunContext context{};
        context.cancel = cancel;
        context.request.Rip = code.Address() + (nested ? 128 : (cancel ? 64 : 0));
        context.request.Rsp = stack.Address() + stack.size - 8;
        context.request.Gpr[4] = context.request.Rsp; context.request.Gpr[7] = nested ? cancel : 0x1234;
        context.request.Rflags = 2; context.request.FsBase = data.Address() + 64;
        context.request.MappedRanges = fixture.ranges;
        std::memcpy(reinterpret_cast<void*>(context.request.Rsp), &ret, sizeof(ret));
        const auto before = cleanups.load();
        pthread_t native{};
        CHECK(pthread_create(&native, nullptr, RunNativeThread, &context) == 0);
        CHECK(pthread_join(native, nullptr) == 0);
        CHECK(cleanups.load() == before + 1);
        CHECK(Fex::SendGuestOrbisSignal(std::bit_cast<std::uintptr_t>(native), 1, 1, childEntry) == -1);
        {
            Fex::MemoryMutationScope publication;
            CHECK(publication.Valid()); // No abandoned runtime/reader/TLS after native exit.
            data.data[0] = static_cast<unsigned char>(iteration);
            publication.Publish(data.Address(), 1);
        }
        // Generic HLE exceptions must also leave admission balanced.
        context.exception = true; context.request.Rip = code.Address() + 192;
        CHECK(pthread_create(&native, nullptr, RunNativeThread, &context) == 0);
        CHECK(pthread_join(native, nullptr) == 0);
        Fex::MemoryMutationScope publication;
        CHECK(publication.Valid());
    }
    CHECK(std::holds_alternative<bool>(engine->Shutdown()));
    std::puts("Real FEX pthread exit/cancel, nested callback, cleanup, exception and post-exit publication PASS");
}

#endif
