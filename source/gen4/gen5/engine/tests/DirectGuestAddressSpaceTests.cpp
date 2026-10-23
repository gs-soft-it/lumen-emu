// SPDX-License-Identifier: GPL-2.0-only
// Includes the production address-space implementation to exercise its actual
// shared backing, protection splits and direct FEX invalidation on ARM64.
#include "common/assert.h"
#include "common/uniformMetadata.h"
#include "common/virtualMemory.h"
#include "kernel/directMemoryMutation.h"
#include "kernel/memory.h"
#include "DirectFexRuntime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <semaphore>
#include <shared_mutex>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace Libs::LibKernel::Memory {
namespace VirtualMemory = Common::VirtualMemory;
constexpr int PROT_CPU_READ = 1;
constexpr int PROT_CPU_WRITE = 2;
static bool VirtualRangesOverlap(uint64_t left, uint64_t left_size,
                                 uint64_t right, uint64_t right_size) {
    return left_size != 0 && right_size != 0 && left < right + right_size && right < left + left_size;
}
#define KYTY_BACKING_MUTATION_TESTS 1
static void (*g_test_backing_write_phase)(bool completed) = nullptr;
#include "kernel/memoryAddressSpace.inc"
#undef KYTY_BACKING_MUTATION_TESTS
} // namespace Libs::LibKernel::Memory

namespace {
namespace Direct = Lumen::Ps5::DirectFex;
namespace Memory = Libs::LibKernel::Memory;
using Mode = Common::VirtualMemory::Mode;
constexpr uint64_t Page = 0x4000;
constexpr uint64_t BackingSize = 13824ull * 1024 * 1024;
Memory::GuestAddressSpace* space;
uint64_t watched;
std::atomic<unsigned> faults {};
uint64_t mutate_alias {};
unsigned host_calls {};
bool mutate_native_copy {};
void MutationTarget() {}

void Check(bool value, const char* text) {
    if (!value) {
        std::fprintf(stderr, "DIRECT_MEMORY_TEST FAIL %s\n", text);
        std::abort();
    }
    std::printf("DIRECT_MEMORY_TEST PASS %s\n", text);
}
uint64_t provenance_write_address {};
unsigned provenance_write_phases {};
void CheckWritePhase(bool completed) {
    const auto generation = Memory::ReadBackingMutationGeneration();
    Check(generation != 0 && (generation & 1) != 0,
          completed ? "backing mutation remains odd after memcpy" : "backing mutation is odd before memcpy");
    Check(*reinterpret_cast<const uint8_t*>(provenance_write_address) == (completed ? 0x73 : 0x29),
          completed ? "mutation phase observes copied bytes" : "mutation phase precedes copied bytes");
    ++provenance_write_phases;
}
void CheckBackingProvenance() {
    const auto address = space->ReserveAligned(0x300000000ull, 3 * Page, Page);
    const auto alias = space->ReserveAligned(0x310000000ull, Page, Page);
    Check(address != 0 && alias != 0 &&
          space->MapBacking(address, 2 * Page, 8 * Page, Mode::ReadWrite),
          "provenance fixture unique backing");
    uint64_t generation = 99;
    if (!Common::UniformMetadata::Enabled()) {
        Check(!space->QueryUniqueBackingGeneration(address, Page, &generation) && generation == 0 &&
              Memory::ReadBackingMutationGeneration() == 0,
              "uniform provenance OFF declines without generation state");
        {
            Memory::BackingMutationScope outer;
            Memory::BackingMutationScope inner;
            Check(Memory::g_backing_mutation_state.load() == 0,
                  "uniform provenance OFF performs no mutation atomic writes");
        }
    } else {
        Check(space->QueryUniqueBackingGeneration(address, 2 * Page, &generation) &&
              generation != 0 && (generation & 1) == 0,
              "unique complete backing returns stable even generation");
        uint64_t partial {};
        Check(space->QueryUniqueBackingGeneration(address + 3, Page - 7, &partial) && partial == generation,
              "unaligned subrange retains complete backing proof");
        Check(!space->QueryUniqueBackingGeneration(address + Page, 2 * Page, &partial) && partial == 0 &&
              !space->QueryUniqueBackingGeneration(UINT64_MAX - 3, 8, &partial) && partial == 0 &&
              !space->QueryUniqueBackingGeneration(address, 0, &partial) && partial == 0 &&
              !space->QueryUniqueBackingGeneration(address, Page, nullptr),
              "unmapped partial zero overflow and null-output proof rejection");
        Check(space->MapBacking(address + 2 * Page, Page, 10 * Page, Mode::ReadWrite) &&
              !space->QueryUniqueBackingGeneration(address + Page, 2 * Page, &partial),
              "proof rejects span across distinct committed mappings");
        Check(space->MapBacking(alias, Page, 9 * Page, Mode::ReadWrite) &&
              !space->QueryUniqueBackingGeneration(address, 2 * Page, &partial) &&
              !space->QueryUniqueBackingGeneration(alias, Page, &partial) &&
              space->QueryUniqueBackingGeneration(address, Page, &partial),
              "physical alias denies overlapping slice but preserves disjoint slice");
        const uint8_t alias_value = 0x29;
        Check(space->TryWriteBacking(alias, &alias_value, 1) && space->UnmapBacking(alias, Page) &&
              space->QueryUniqueBackingGeneration(address, 2 * Page, &partial) && partial != generation,
              "alias create write remove invalidates previous unique proof");
        generation = partial;
        provenance_write_address = address + Page;
        Memory::g_test_backing_write_phase = CheckWritePhase;
        const uint8_t replacement = 0x73;
        Check(space->TryWriteBacking(provenance_write_address, &replacement, 1), "instrumented backing write");
        Memory::g_test_backing_write_phase = nullptr;
        Check(provenance_write_phases == 2 &&
              space->QueryUniqueBackingGeneration(address, 2 * Page, &partial) && partial != generation,
              "completed raw write advances to distinct even generation");
        generation = partial;
        Check(!space->TryWriteBacking(address + 3 * Page, &replacement, 1) &&
              Memory::ReadBackingMutationGeneration() != generation &&
              (Memory::ReadBackingMutationGeneration() & 1) == 0,
              "failed raw write also retires proof generation");
        generation = Memory::ReadBackingMutationGeneration();
        Check(space->ZeroBacking(8 * Page, 2 * Page) &&
              *reinterpret_cast<const uint8_t*>(provenance_write_address) == 0 &&
              Memory::ReadBackingMutationGeneration() != generation,
              "physical backing zeroing retires proof generation");
        generation = Memory::ReadBackingMutationGeneration();
        Check(space->Protect(address, Page, Mode::Read) &&
              Memory::ReadBackingMutationGeneration() != generation,
              "semantic guest protection retires proof generation");
        generation = Memory::ReadBackingMutationGeneration();
        Check(space->ProtectTransient(address, Page, Mode::NoAccess) &&
              space->ProtectTransient(address, Page, Mode::Read) &&
              Memory::ReadBackingMutationGeneration() == generation,
              "renderer watch changes preserve provenance generation");
        Check(space->Protect(address, Page, Mode::ReadWrite), "restore provenance semantic protection");
        generation = Memory::ReadBackingMutationGeneration();
        Check(!space->MapBacking(alias, Page, BackingSize, Mode::ReadWrite) &&
              Memory::ReadBackingMutationGeneration() != generation,
              "failed mapping retires proof generation");
        generation = Memory::ReadBackingMutationGeneration();
        Check(!space->UnmapBacking(alias, Page) && Memory::ReadBackingMutationGeneration() != generation,
              "failed unmap retires proof generation");
        {
            Memory::BackingMutationScope outer;
            const auto first = Memory::ReadBackingMutationGeneration();
            Check((first & 1) != 0 &&
                  !space->QueryUniqueBackingGeneration(address, Page, &partial) && partial == 0,
                  "in-progress mutation declines query");
            {
                Memory::BackingMutationScope inner;
                Check((Memory::ReadBackingMutationGeneration() & 1) != 0,
                      "nested mutation stays odd");
            }
            Check((Memory::ReadBackingMutationGeneration() & 1) != 0 &&
                  Memory::ReadBackingMutationGeneration() != first,
                  "nested completion cannot expose stable generation");
        }
        std::binary_semaphore entered {0};
        std::binary_semaphore release {0};
        std::thread concurrent;
        {
            Memory::BackingMutationScope first_writer;
            concurrent = std::thread([&] {
                Memory::BackingMutationScope second_writer;
                entered.release();
                release.acquire();
            });
            entered.acquire();
        }
        Check((Memory::ReadBackingMutationGeneration() & 1) != 0,
              "first concurrent completion keeps second mutation active");
        release.release();
        concurrent.join();
        Check((Memory::ReadBackingMutationGeneration() & 1) == 0,
              "last concurrent completion restores stable generation");
        generation = Memory::ReadBackingMutationGeneration();
        Check(space->UnmapBacking(address, 2 * Page) &&
              space->MapBacking(address, 2 * Page, 12 * Page, Mode::ReadWrite) &&
              space->QueryUniqueBackingGeneration(address, 2 * Page, &partial) && partial != generation,
              "same-VA replacement cannot reuse proof generation");
        Check(space->UnmapBacking(address + 2 * Page, Page), "retire neighboring provenance mapping");
    }
    Check(space->UnmapBacking(address, 2 * Page), "retire provenance backing");
}
uint32_t Query(void*, uint64_t address, uint64_t* base, uint64_t* size, uint32_t* writable) noexcept {
    return space->QueryExecutable(address, base, size, writable) ? 1 : 0;
}
uint32_t Host(void*, uint64_t target, uint64_t* registers, uint64_t*) noexcept {
    if (target != reinterpret_cast<uint64_t>(&MutationTarget) || mutate_alias == 0)
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    const uint8_t value = 45;
    if (mutate_native_copy) {
        std::memcpy(reinterpret_cast<void*>(mutate_alias + 1), &value, 1);
        space->InvalidateCodeAliases(mutate_alias + 1, 1);
    } else if (!space->TryWriteBacking(mutate_alias + 1, &value, 1)) {
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    }
    mutate_alias = 0;
    ++host_calls;
    registers[0] = 0;
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_RETURN_GUEST_V1;
}
uint64_t Run(uint64_t code, uint64_t stack) {
    LumenAndroidFexCoreGuestStateV1 state {sizeof(state), 1, code};
    state.gprs[4] = stack + Page - 24;
    const auto sentinel = Direct::RootReturn();
    std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &sentinel, sizeof(sentinel));
    const auto result = Direct::RunGuestV1(&state, Host, Query, nullptr);
    Check(result == 0 && state.rip == sentinel, "translated guest call/return");
    return state.gprs[0];
}
void Fault(int signal, siginfo_t* info, void* context) {
    if (Direct::HandleHostSignal(signal, info, context)) return;
    const auto address = reinterpret_cast<uint64_t>(info->si_addr);
    if (address >= watched && address - watched < Page &&
        space->ProtectTransient(watched, Page, Mode::ReadWrite)) {
        ++faults;
        return;
    }
    _exit(80);
}
std::vector<uint8_t> MemoryCode(uint64_t address, bool write) {
    std::vector<uint8_t> bytes {0x48, 0xb8};
    const auto* p = reinterpret_cast<const uint8_t*>(&address);
    bytes.insert(bytes.end(), p, p + 8);
    if (write) {
        const std::array<uint8_t, 11> tail {0xc7,0x00,99,0,0,0,0xb8,99,0,0,0};
        bytes.insert(bytes.end(), tail.begin(), tail.end());
    } else {
        bytes.push_back(0x8b); bytes.push_back(0x00);
    }
    bytes.push_back(0xc3);
    return bytes;
}
}

int main() {
    Check(sysconf(_SC_PAGESIZE) == 4096, "Android 4 KiB host");
    Check(Direct::Probe() == 0, "real direct FEX bootstrap");
    auto owner = std::make_unique<Memory::GuestAddressSpace>(BackingSize);
    space = owner.get();
    Check(space->GetBackingSize() == BackingSize && Memory::HOST_USER_MAX == 0x3fffffffffull,
          "full PS5 13824 MiB backing with Gen4 Android 256 GiB host user boundary");
    CheckBackingProvenance();
    Memory::g_guest_memory_ready = true;
    const auto base = space->ReserveAligned(0x200000000ull, 3 * Page, Page);
    Check(base != 0 && space->MapBacking(base, 3 * Page, 0, Mode::ReadWrite), "shared backing map");
    const auto alias = space->ReserveAligned(base + 3 * Page, 3 * Page, Page);
    Check(alias != 0 && space->MapBacking(alias, 3 * Page, 0, Mode::ReadWrite), "physical alias map");
    const auto stack = space->ReserveAligned(alias + 3 * Page, Page, Page);
    Check(stack != 0 && space->Commit(stack, Page, Mode::ReadWrite), "private stack commit");
    std::array<uint8_t, 6> program {0xb8,41,0,0,0,0xc3};
    Check(space->TryWriteBacking(alias, program.data(), program.size()), "write through backing alias");
    Check(std::memcmp(reinterpret_cast<void*>(base), program.data(), program.size()) == 0, "aliases share bytes");
    Check(space->Protect(base, Page, Mode::ExecuteRead), "partial executable protection");
    uint64_t queried_base {}, queried_size {}; uint32_t writable {};
    Check(space->QueryExecutable(base, &queried_base, &queried_size, &writable) &&
          queried_base == base && queried_size == Page && writable == 0 &&
          !space->QueryExecutable(base + Page, &queried_base, &queried_size, &writable),
          "semantic protection split preserves neighboring data");
    Check(Run(base, stack) == 41, "translated original bytes");
    const auto caller = space->ReserveAligned(stack + Page, Page, Page);
    Check(caller != 0 && space->Commit(caller, Page, Mode::ReadWrite), "same-thread caller allocation");
    const auto target = reinterpret_cast<uint64_t>(&MutationTarget);
    Check(Direct::RegisterHostTarget(target), "same-thread mutation host call registration");
    const auto veneer = Direct::ResolveHostTarget(target);
    Check(veneer != 0, "same-thread mutation veneer");
    std::vector<uint8_t> caller_code;
    auto call = [&](uint64_t address) {
        caller_code.insert(caller_code.end(), {0x48, 0xbb});
        const auto* bytes = reinterpret_cast<const uint8_t*>(&address);
        caller_code.insert(caller_code.end(), bytes, bytes + 8);
        caller_code.insert(caller_code.end(), {0xff, 0xd3});
    };
    call(base);
    caller_code.insert(caller_code.end(), {0x41, 0x89, 0xc4}); // mov r12d,eax
    call(veneer);
    call(base);
    caller_code.insert(caller_code.end(), {0x44, 0x01, 0xe0, 0xc3}); // add eax,r12d; ret
    std::memcpy(reinterpret_cast<void*>(caller), caller_code.data(), caller_code.size());
    Check(space->Protect(caller, Page, Mode::ExecuteRead), "same-thread caller executable");
    mutate_alias = alias;
    Check(Run(caller, stack) == 86 && host_calls == 1,
          "physical alias invalidation re-enters modified code in the same guest thread/run");
    Check(Run(base, stack) == 45, "all physical aliases invalidate shared context code");
    Check(space->TryWriteBacking(alias, program.data(), program.size()), "reset native-copy test bytes");
    mutate_alias = alias; mutate_native_copy = true;
    Check(Run(caller, stack) == 86 && host_calls == 2,
          "watched native-copy alias invalidation in the same guest thread/run");
    program[1] = 67;
    Check(space->TryWriteBacking(alias + Page, program.data(), program.size()), "prepare replacement backing");
    Check(space->UnmapBacking(base, Page) &&
          !space->QueryExecutable(base, &queried_base, &queried_size, &writable), "retired mapping stops executable query");
    Check(space->MapBacking(base, Page, Page, Mode::ExecuteRead), "same VA replacement mapping");
    Check(Run(base, stack) == 67, "same VA replacement invalidates old translation");

    watched = alias + 2 * Page;
    *reinterpret_cast<uint32_t*>(watched) = 42;
    struct sigaction action {};
    action.sa_sigaction = Fault; action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    Check(sigaction(SIGSEGV, &action, nullptr) == 0 && sigaction(SIGBUS, &action, nullptr) == 0,
          "composed direct FEX and page-watch signal handler");
    auto read = MemoryCode(watched, false);
    Check(space->TryWriteBacking(alias + Page, read.data(), read.size()) &&
          space->ProtectTransient(watched, Page, Mode::NoAccess), "read watch installed with actual mprotect");
    Check(Run(base, stack) == 42 && faults == 1, "translated load faults and resumes original PC");
    auto write = MemoryCode(watched, true);
    Check(space->TryWriteBacking(alias + Page, write.data(), write.size()) &&
          space->ProtectTransient(watched, Page, Mode::Read), "write watch installed with actual mprotect");
    Check(Run(base, stack) == 99 && *reinterpret_cast<uint32_t*>(watched) == 99 && faults == 2,
          "translated store faults and resumes original PC");
    Check(space->ProtectTransient(base, Page, Mode::NoAccess) &&
          space->QueryExecutable(base, &queried_base, &queried_size, &writable) && writable == 0,
          "watch protection does not rewrite executable metadata");
    Check(space->ProtectTransient(base, Page, Mode::ExecuteRead), "restore code host protection");
    Check(!space->MapBacking(base + 3 * Page, Page, BackingSize, Mode::ReadWrite), "out-of-bounds physical backing denied");
    Check(!space->UnmapBacking(stack, Page), "shared unmap rejects private allocation");
    Check(space->ReleaseCommitted(stack, Page) && space->ReleaseCommitted(caller, Page), "private stack/code retirement");
    Check(space->UnmapBacking(base, 3 * Page) && space->UnmapBacking(alias, 3 * Page), "alias retirement");
    Memory::g_guest_memory_ready = false;
    std::puts("DIRECT_MEMORY_TEST ALL_PASS actual_address_space=1 actual_fex=1 guest_load_store_faults=2 gpu_renderer_tested=0 global_cpu_suspension=0");
}
