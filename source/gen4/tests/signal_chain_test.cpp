// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/fex/guest_runtime_setup.h"
#include "core/fex/guest_memory_scope.h"
#include "core/guest_cpu/fex_hle_bridge.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)
namespace {
using namespace Core;
using namespace std::chrono_literals;
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

volatile sig_atomic_t expectedNativeSignal{};
volatile sig_atomic_t nativeSignalsHandled{};
bool NativeIllegalInstruction(void*) {
    if (expectedNativeSignal != SIGILL) return false;
    nativeSignalsHandled = nativeSignalsHandled + 1;
    return true;
}
bool NativeAccessViolation(void*, void*) {
    if (expectedNativeSignal != SIGSEGV && expectedNativeSignal != SIGBUS) return false;
    nativeSignalsHandled = nativeSignalsHandled + 1;
    return true;
}
constexpr std::array<int, 3> signals{SIGILL, SIGSEGV, SIGBUS};
auto ReadActions() {
    std::array<struct sigaction, signals.size()> actions{};
    for (std::size_t i = 0; i != signals.size(); ++i)
        CHECK(sigaction(signals[i], nullptr, &actions[i]) == 0);
    return actions;
}
}
int main(int argc, char** argv) {
    const bool reproduceLateInitialization = argc == 2 &&
        std::strcmp(argv[1], "--reproduce-late-init") == 0;
    GuestCpu::HleCallRegistry registry;
    Mapping code, stack, data;
    Fixture fixture{{{code.Address(), code.size, true, false},
        {stack.Address(), stack.size, false, true},
        {data.Address(), data.size, false, true}}};
    GuestCpu::HleGuestBridge bridge{registry, Fixture::Access, &fixture, nullptr, nullptr,
        Fixture::Executable, &fixture};
    // The normal route is the same full-core factory used by Linker. The
    // optional diagnostic route exercises the previous initialization order.
    auto created = reproduceLateInitialization ? FexGuestCpuBackend::Create(bridge)
                                               : Fex::CreateGuestBackend(bridge);
    CHECK(std::holds_alternative<std::unique_ptr<FexGuestCpuBackend>>(created));
    auto backend = std::move(std::get<std::unique_ptr<FexGuestCpuBackend>>(created));
    const auto before = ReadActions();
    for (const auto& action : before) {
#ifdef LUMEN_GEN4_DIRECT_FEX
        // Direct FEX delegates guest faults from the core handler; it does not
        // install an outer POSIX handler like the legacy runtime wrapper.
        if (!reproduceLateInitialization) {
            CHECK((action.sa_flags & SA_SIGINFO) != 0);
            CHECK(action.sa_sigaction == Core::SignalHandler);
        } else {
            CHECK(action.sa_sigaction != Core::SignalHandler);
        }
#else
        CHECK((action.sa_flags & SA_SIGINFO) != 0);
        CHECK(action.sa_sigaction != Core::SignalHandler);
#endif
    }
    // Match first SRT-walker compilation: obtain the core singleton and add
    // native fault handlers only after the translator runtime is active.
    auto* dispatcher = Core::Signals::Instance();
    dispatcher->RegisterAccessViolationHandler(NativeAccessViolation, 1);
    dispatcher->RegisterIllegalInstructionHandler(NativeIllegalInstruction, 1);
    const auto after = ReadActions();
    for (std::size_t i = 0; i != signals.size(); ++i) {
#ifdef LUMEN_GEN4_DIRECT_FEX
        CHECK((after[i].sa_flags & SA_SIGINFO) != 0);
        CHECK(after[i].sa_sigaction == Core::SignalHandler);
#endif
        if (reproduceLateInitialization) {
            CHECK(after[i].sa_sigaction == Core::SignalHandler);
            CHECK(after[i].sa_sigaction != before[i].sa_sigaction);
        } else {
            CHECK(after[i].sa_sigaction == before[i].sa_sigaction);
            CHECK(after[i].sa_flags == before[i].sa_flags);
        }
    }
#ifndef LUMEN_GEN4_DIRECT_FEX
    if (reproduceLateInitialization) {
        std::puts("REPRODUCED: late core initialization replaces FEX SIGILL/SIGSEGV/SIGBUS handlers");
        return 0;
    }
#endif
    // A native signal outside translated execution must still reach the
    // core dispatcher, rather than being swallowed by the runtime.
    for (const int signal : signals) {
        expectedNativeSignal = signal;
        CHECK(raise(signal) == 0);
    }
    expectedNativeSignal = 0;
    CHECK(nativeSignalsHandled == signals.size());
    // MOV RAX,data; MOV [RAX],1; CMP [RAX+8],0; JE back; MOV EAX,42; RET.
    unsigned char program[]{0x48,0xb8,0,0,0,0,0,0,0,0,
        0xc7,0x00,1,0,0,0,0x83,0x78,8,0,0x74,0xfa,0xb8,42,0,0,0,0xc3};
    const auto address = data.Address();
    std::memcpy(program + 2, &address, sizeof(address));
    {
        Fex::MemoryMutationScope publication;
        CHECK(publication.Valid());
        std::memcpy(code.data, program, sizeof(program));
        CHECK(mprotect(code.data, code.size, PROT_READ) == 0);
        publication.Publish(code.Address(), code.size);
    }
    GuestExecutionRequest request{};
    request.Rip = code.Address(); request.Rsp = stack.Address() + stack.size - 8;
    request.Gpr[4] = request.Rsp; request.Rflags = 2; request.FsBase = data.Address() + 64;
    request.MappedRanges = fixture.ranges;
    const auto ret = backend->ReturnAddress();
    std::memcpy(reinterpret_cast<void*>(request.Rsp), &ret, sizeof(ret));
    std::atomic_ref<std::uint32_t> entered{*reinterpret_cast<std::uint32_t*>(data.data)};
    bool published{};
    std::thread publisher([&] {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (entered.load(std::memory_order_acquire) == 0) {
            CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        Fex::MemoryMutationScope publication;
        published = publication.Valid();
        CHECK(published);
        *reinterpret_cast<std::uint32_t*>(data.data + 8) = 1;
        publication.Publish(data.Address(), 12);
    });
    const auto result = backend->Run(request);
    publisher.join();
    CHECK(published);
    CHECK(std::holds_alternative<GuestExecutionState>(result));
    CHECK(std::get<GuestExecutionState>(result).Gpr[0] == 42);
#ifndef LUMEN_GEN4_DIRECT_FEX
    CHECK(LumenAndroidFexCoreOwnsGuestMemoryReadLeaseV1() == 0);
#endif
    std::puts("Full-core signal initialization, late renderer registration, native chaining and real FEX safepoint PASS");
}
