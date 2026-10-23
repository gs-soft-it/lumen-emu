// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/fex/fex_guest_engine.h"
#include "core/fex/guest_memory_scope.h"
#include "core/guest_cpu/fex_hle_bridge.h"
#include "core/libraries/kernel/threads/pthread.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <semaphore>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)
namespace {
using namespace Core;
using namespace Libraries::Kernel;
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
PthreadRwlock rwlock{};
std::binary_semaphore hleEntered{0};
std::atomic<bool> hleCompleted{};
std::uint64_t WaitRwlock(std::uint64_t write, const OrbisKernelTimespec* deadline) {
    Pthread thread{};
    const auto previous = g_curthread;
    g_curthread = &thread;
    hleEntered.release();
    const int result = write ? rwlock.Wrlock(deadline) : rwlock.Rdlock(deadline);
    if (result == 0) {
        if (write) {
            CHECK(rwlock.owner == &thread);
            rwlock.owner = nullptr;
            rwlock.lock.unlock();
        } else {
            CHECK(thread.rdlock_count == 1);
            rwlock.lock.unlock_shared();
        }
    }
    g_curthread = previous;
    hleCompleted.store(true, std::memory_order_release);
    return result;
}
}

int main() {
    auto registry = std::make_unique<GuestCpu::HleCallRegistry>();
    const auto adapter = registry->Register(GuestCpu::MakeHleCallAdapter(WaitRwlock), "rwlock-publication");
    CHECK(adapter != nullptr);
    GuestCpu::HleVeneerAllocator veneers;
    const auto allocated = veneers.Allocate(*adapter);
    CHECK(std::holds_alternative<u64>(allocated));
    const auto veneer = std::get<u64>(allocated);
    Mapping code, stack, data;
    Fixture fixture{veneers.GetExecutableRanges()};
    fixture.ranges.push_back({code.Address(), code.size, true, false});
    fixture.ranges.push_back({stack.Address(), stack.size, false, true});
    fixture.ranges.push_back({data.Address(), data.size, false, true});
    GuestCpu::HleGuestBridge bridge{*registry, Fixture::Access, &fixture, nullptr, nullptr,
        Fixture::Executable, &fixture};
    auto created = Fex::GuestEngine::Create(bridge);
    CHECK(std::holds_alternative<std::unique_ptr<Fex::GuestEngine>>(created));
    auto engine = std::move(std::get<std::unique_ptr<Fex::GuestEngine>>(created));
    // SUB RSP,8; MOV RAX,veneer; CALL RAX; ADD RSP,8; RET.
    const unsigned char program[]{0x48,0x83,0xec,8,0x48,0xb8,0,0,0,0,0,0,0,0,
        0xff,0xd0,0x48,0x83,0xc4,8,0xc3};
    {
        Fex::MemoryMutationScope publication;
        CHECK(publication.Valid());
        std::memcpy(code.data, program, sizeof(program));
        std::memcpy(code.data + 6, &veneer, sizeof(veneer));
        CHECK(mprotect(code.data, code.size, PROT_READ) == 0);
        publication.Publish(code.Address(), code.size);
    }
    const auto ret = engine->ReturnAddress();
    for (const bool write : {false, true}) for (const bool timed : {false, true}) {
        hleCompleted.store(false);
        auto* deadline = reinterpret_cast<OrbisKernelTimespec*>(data.data);
        const auto future = std::chrono::system_clock::now().time_since_epoch() + 30s;
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(future);
        *deadline = {seconds.count(), std::chrono::duration_cast<std::chrono::nanoseconds>(future-seconds).count()};
        std::binary_semaphore lockHeld{0};
        bool published{};
        std::thread publisher([&] {
            rwlock.lock.lock();
            lockHeld.release();
            CHECK(hleEntered.try_acquire_for(5s));
            {
                Fex::MemoryMutationScope publication;
                published = publication.Valid();
                if (published) {
                    // The actual blocking HLE must have donated its reader.
                    // Its deadline is already copied before the publication.
                    CHECK(!hleCompleted.load(std::memory_order_acquire));
                    *deadline = {-1, -1};
                    publication.Publish(data.Address(), sizeof(*deadline));
                }
            }
            rwlock.lock.unlock();
        });
        CHECK(lockHeld.try_acquire_for(5s));
        GuestExecutionRequest request{};
        request.Rip = code.Address(); request.Rsp = stack.Address() + stack.size - 8;
        request.Gpr[4] = request.Rsp; request.Gpr[7] = write;
        request.Gpr[6] = timed ? data.Address() : 0;
        request.Rflags = 2; request.FsBase = data.Address() + 64;
        request.MappedRanges = fixture.ranges;
        std::memcpy(reinterpret_cast<void*>(request.Rsp), &ret, sizeof(ret));
        auto thread = engine->CreateThread(request);
        CHECK(std::holds_alternative<Fex::GuestEngine::Thread*>(thread));
        auto* guest = std::get<Fex::GuestEngine::Thread*>(thread);
        const auto result = engine->Run(*guest);
        publisher.join();
        CHECK(published);
        CHECK(std::holds_alternative<GuestExecutionState>(result));
        CHECK(std::get<GuestExecutionState>(result).Gpr[0] == 0);
        CHECK(std::holds_alternative<bool>(engine->DestroyThread(guest)));
    }
    CHECK(std::holds_alternative<bool>(engine->Shutdown()));
    std::puts("Real FEX HLE rwlock read/write, timed/untimed, publication and owned deadline PASS");
}
