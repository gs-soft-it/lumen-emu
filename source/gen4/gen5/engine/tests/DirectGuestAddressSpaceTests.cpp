// SPDX-License-Identifier: GPL-2.0-only
// Includes the production address-space implementation to exercise its actual
// shared backing, protection splits and direct FEX invalidation on ARM64.
#include "common/assert.h"
#include "common/hostException.h"
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
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#endif

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
#if defined(__APPLE__)
uint64_t CreateReusableNativeGuard() {
    // Allocate before the address-space inventory. Even a real GUARD-tagged
    // max-NONE mapping is temporary unless it is an OS permanent reservation.
    mach_vm_address_t address = 0;
    for (uint64_t candidate = 0x7200000000ull; candidate < 0x7210000000ull; candidate += Page) {
        address = candidate;
        if (mach_vm_allocate(mach_task_self(), &address, Page,
                VM_FLAGS_FIXED | VM_MAKE_TAG(VM_MEMORY_GUARD)) == KERN_SUCCESS) break;
        address = 0;
    }
    Check(address != 0 &&
          mach_vm_protect(mach_task_self(), address, Page, true, VM_PROT_NONE) == KERN_SUCCESS,
          "create temporary native GUARD with maximum protection NONE before inventory");
    return address;
}

vm_prot_t NativeProtection(uint64_t address) {
    mach_vm_address_t region = address;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info {};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const auto status = mach_vm_region(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    Check(status == KERN_SUCCESS && region <= address, "query native mapping rights");
    return info.protection;
}

void CheckDarwinPermanentReservationAdmission(uint64_t temporary_guard) {
    uint64_t host = 0;
    Check(NativeProtection(temporary_guard) == VM_PROT_NONE &&
          !Memory::IsGuestNonIdentity(temporary_guard, Page) &&
          !space->Owns(temporary_guard, Page) && !space->ReserveFixed(temporary_guard, Page),
          "temporary max-NONE GUARD remains host-owned and is never a translation arena");
    Check(mach_vm_deallocate(mach_task_self(), temporary_guard, Page) == KERN_SUCCESS,
          "temporary max-NONE guard is deallocatable after inventory");
    mach_vm_address_t reused = temporary_guard;
    Check(mach_vm_allocate(mach_task_self(), &reused, Page, VM_FLAGS_FIXED) == KERN_SUCCESS &&
          reused == temporary_guard &&
          mach_vm_protect(mach_task_self(), reused, Page, false, VM_PROT_READ | VM_PROT_WRITE) == KERN_SUCCESS,
          "temporary guard address can become a readable native mapping");
    *reinterpret_cast<uint64_t*>(reused) = 0x0123456789abcdefull;
    Check(Memory::TranslateGuestMemory(reused, Page, &host) && host == reused &&
          !Memory::IsGuestNonIdentity(reused, Page) &&
          !space->ReserveFixed(reused, Page) && !space->Commit(reused, Page, Mode::ReadWrite) &&
          !space->MapBacking(reused, Page, 0, Mode::ReadWrite) &&
          *reinterpret_cast<uint64_t*>(reused) == 0x0123456789abcdefull &&
          NativeProtection(reused) == (VM_PROT_READ | VM_PROT_WRITE),
          "reused native guard remains unaliased and guest mappings preserve its canary and rights");
    Check(mach_vm_deallocate(mach_task_self(), reused, Page) == KERN_SUCCESS,
          "release reused native guard fixture");
    constexpr uint64_t commpage_pte_begin = 0xffe000000ull;
    constexpr uint64_t commpage_pte_size = 0x2000000ull;
    Check(!Memory::IsGuestNonIdentity(commpage_pte_begin, commpage_pte_size) &&
          !space->OverlapsOwned(commpage_pte_begin, commpage_pte_size) &&
          !space->ReserveFixed(commpage_pte_begin, Page) &&
          !space->ReserveFixed(0xfffffc000ull, Page) &&
          !space->ReserveFixed(0xfffff4000ull, Page) &&
          Memory::TranslateGuestMemory(commpage_pte_begin, commpage_pte_size, &host) &&
          host == commpage_pte_begin,
          "entire native commpage nesting interval including randomized code stays host-owned");
    const auto* arenas = Memory::g_translation_arenas.load();
    Check(arenas != nullptr && arenas->ranges.size() == 2 &&
          Memory::IsGuestNonIdentity(0xfc0000000ull, 0x3e000000ull) &&
          Memory::IsGuestNonIdentity(0x1000000000ull, 0x6000000000ull),
          "only permanent commpage prefix and GPU carveout become translation arenas");
}

void CheckDarwinPageContract() {
    const auto address = space->ReserveAligned(0x320000000ull, 3 * Page, Page);
    const auto middle = address + Page;
    Check(address != 0 && space->Commit(address, Page, Mode::NoAccess) &&
          space->Commit(middle, Page, Mode::ReadWrite) &&
          space->Commit(middle + Page, Page, Mode::NoAccess), "guarded native16K fixture");
    *reinterpret_cast<uint32_t*>(middle) = 0x12345678;
    Check(!space->Protect(middle + 4096, 4096, Mode::Read) &&
          NativeProtection(middle) == (VM_PROT_READ | VM_PROT_WRITE) &&
          NativeProtection(middle + 4096) == (VM_PROT_READ | VM_PROT_WRITE) &&
          *reinterpret_cast<uint32_t*>(middle) == 0x12345678,
          "mixed4K rights denied before native mutation");
    Check(NativeProtection(address) == VM_PROT_NONE && NativeProtection(middle + Page) == VM_PROT_NONE,
          "denied partial protection preserves adjacent guard pages");
    Check(space->Protect(middle, Page, Mode::ExecuteRead), "whole16K executable metadata admitted");
    uint64_t base {}, size {}; uint32_t writable {};
    Check(!space->Protect(middle + 4096, 4096, Mode::ReadWrite) &&
          space->QueryExecutable(middle + 4096, &base, &size, &writable) &&
          base == middle && size == Page && writable == 0 && NativeProtection(middle) == VM_PROT_READ,
          "mixed4K rejection preserves executable metadata and native read-only rights");
    Check(space->ProtectTransient(middle, Page, Mode::ReadWrite) &&
          NativeProtection(middle) == VM_PROT_READ,
          "watch retirement cannot grant write beyond guest semantic rights");
    Check(space->Protect(middle + 4096, 4096, Mode::Read) &&
          !space->QueryExecutable(middle + 4096, &base, &size, &writable) &&
          space->QueryExecutable(middle, &base, &size, &writable) &&
          NativeProtection(middle) == VM_PROT_READ,
          "representable partial metadata split retains homogeneous native data rights");
    Check(space->ReleaseCommitted(address, 3 * Page), "retire guarded native16K fixture");

    // An existing mapping must survive either fixed-allocation API unchanged.
    auto* existing = ::mmap(nullptr, Page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(existing != MAP_FAILED, "native fixed-collision fixture");
    *static_cast<uint32_t*>(existing) = 0xaabbccdd;
    const auto native = reinterpret_cast<uint64_t>(existing);
    Common::VirtualMemory::Init();
    Check(!Common::VirtualMemory::AllocFixed(native, Page, Mode::NoAccess) &&
          !Common::VirtualMemory::ReserveFixed(native, Page) &&
          *static_cast<uint32_t*>(existing) == 0xaabbccdd &&
          NativeProtection(native) == (VM_PROT_READ | VM_PROT_WRITE),
          "fixed allocations reject collision without replacing host data or permissions");
    Check(::munmap(existing, Page) == 0, "release native collision fixture");
}

void CheckDarwinTranslationContract() {
    constexpr uint64_t guest = 0xfd0000000ull;
    constexpr uint64_t offset = 32 * Page;
    uint64_t host = 0, inverse = 0;
    Check(Memory::TranslateGuestMemory(guest, 3 * Page, &host) && host != guest &&
          Memory::IsGuestNonIdentity(guest, 3 * Page) &&
          Memory::TranslateHostMemoryFault(host + 4097, &inverse) && inverse == guest + 4097 &&
          (host % Page) == 0 && NativeProtection(guest) == VM_PROT_NONE,
          "Mach inaccessible guest range has separate immutable aligned host arena");
    Check(!Memory::QueryTranslatedGuestAccess(guest, 1, false) &&
          !Memory::QueryTranslatedGuestAccess(guest, 1, true) &&
          !Memory::QueryTranslatedHostAccess(guest, 1, false) &&
          !Memory::QueryTranslatedHostAccess(guest, 1, true) &&
          !Memory::TranslateGuestMemory(UINT64_MAX - 4, 8, &inverse) &&
          !Memory::TranslateGuestMemory(guest, 0, &inverse) &&
          !Memory::TranslateGuestMemory(guest, Page, nullptr) &&
          !Memory::TranslateHostMemoryFault(guest, &inverse) &&
          !Memory::IsGuestNonIdentity(reinterpret_cast<uint64_t>(&space), 1),
          "translation rejects invalid spans and uncommitted semantic access");
    const auto* arenas = Memory::g_translation_arenas.load();
    const auto* arena = Memory::FindTranslatedGuestRange(guest, Page);
    Check(arenas != nullptr && arena != nullptr &&
          !Memory::TranslateGuestMemory(arena->guest - 1, 2, &inverse) &&
          !Memory::TranslateGuestMemory(arena->guest + arena->size - 1, 2, &inverse),
          "native alias cannot cross either immutable arena boundary");
    Check(space->ReserveAligned(guest, 3 * Page, Page) == guest &&
          space->MapBacking(guest, 3 * Page, offset, Mode::ReadWrite) &&
          Memory::QueryTranslatedGuestAccess(guest, 3 * Page, true) &&
          Memory::QueryTranslatedHostAccess(guest, 3 * Page, true) &&
          NativeProtection(host) == (VM_PROT_READ | VM_PROT_WRITE),
          "requested guest VA maps actual shared backing at protected host alias");
    *reinterpret_cast<uint32_t*>(host) = 0x1234abcd;
    *reinterpret_cast<uint32_t*>(host + Page) = 0x9876abcd;
    uint32_t read = 0;
    Check(space->TryReadBacking(guest, &read, sizeof(read)) && read == 0x1234abcd,
          "guest-keyed backing sees native alias writes");
    const auto identity = space->ReserveAligned(0x340000000ull, Page, Page);
    Check(identity != 0 && !Memory::IsGuestNonIdentity(identity, Page) &&
          space->MapBacking(identity, Page, offset, Mode::ReadWrite) &&
          *reinterpret_cast<uint32_t*>(identity) == 0x1234abcd,
          "identity and nonidentity mappings share the same physical bytes");
    Check(space->Protect(guest, Page, Mode::Read) &&
          Memory::QueryTranslatedGuestAccess(guest, Page, false) &&
          !Memory::QueryTranslatedGuestAccess(guest, Page, true) &&
          Memory::QueryTranslatedHostAccess(guest, Page, false) &&
          !Memory::QueryTranslatedHostAccess(guest, Page, true) &&
          Memory::QueryTranslatedGuestAccess(guest + Page, Page, true) &&
          !Memory::QueryTranslatedGuestAccess(guest, 2 * Page, true) &&
          NativeProtection(host) == VM_PROT_READ,
          "semantic protection splits update lock-free rights and native alias");
    Check(space->ProtectTransient(guest + Page, Page, Mode::NoAccess) &&
          Memory::QueryTranslatedGuestAccess(guest + Page, Page, true) &&
          !Memory::QueryTranslatedHostAccess(guest + Page, Page, false) &&
          !Memory::QueryTranslatedHostAccess(guest + Page, Page, true) &&
          NativeProtection(host + Page) == VM_PROT_NONE &&
          space->ProtectTransient(guest + Page, Page, Mode::ReadWrite) &&
          Memory::QueryTranslatedHostAccess(guest + Page, Page, true),
          "GPU watch protects host alias while semantic guest permission remains intact");
    Check(!space->Protect(guest, Page, Mode::ExecuteRead) &&
          !space->Protect(guest + 4096, 4096, Mode::ReadWrite) &&
          NativeProtection(host) == VM_PROT_READ,
          "nonidentity executable and subpage permission changes fail before mutation");
    std::array<uint8_t, 3 * Page> sparse {};
    Check(space->UnmapBacking(guest + Page, Page) &&
          !Memory::QueryTranslatedGuestAccess(guest + Page, 1, false) &&
          !Memory::QueryTranslatedHostAccess(guest + Page, 1, false) &&
          NativeProtection(host + Page) == VM_PROT_NONE &&
          space->CopySparseMemory<true>(guest, sparse.data(), sparse.size()) &&
          std::memcmp(sparse.data(), &read, sizeof(read)) == 0 &&
          std::all_of(sparse.begin() + Page, sparse.begin() + 2 * Page, [](uint8_t value) { return value == 0; }),
          "partial unmap retires semantic access and sparse read preserves guest holes");
    Check(space->MapBacking(guest + Page, Page, offset + Page, Mode::ReadWrite) &&
          *reinterpret_cast<uint32_t*>(host + Page) == 0x9876abcd &&
          Memory::QueryTranslatedGuestAccess(guest + Page, Page, true),
          "same guest VA remaps preserved physical contents at the same host alias");
    Check(space->UnmapBacking(guest, 3 * Page) && space->UnmapBacking(identity, Page) &&
          !Memory::QueryTranslatedGuestAccess(guest, 1, false) &&
          !Memory::QueryTranslatedHostAccess(guest, 1, false) &&
          Memory::TranslateGuestMemory(guest, Page, &inverse) && inverse == host &&
          NativeProtection(guest) == VM_PROT_NONE && NativeProtection(host) == VM_PROT_NONE,
          "retirement preserves immutable translation and leaves original Mach reservation untouched");
    Check(!space->Commit(guest, Page, Mode::ExecuteRead) &&
          space->Commit(guest, Page, Mode::ReadWrite), "private nonidentity data admitted with executable data denied");
    *reinterpret_cast<uint32_t*>(host) = 0x5678abcd;
    Check(space->CopySparseMemory<true>(guest, &read, sizeof(read)) && read == 0x5678abcd &&
          space->ReleaseCommitted(guest, Page) &&
          !Memory::QueryTranslatedGuestAccess(guest, 1, false) &&
          !Memory::QueryTranslatedHostAccess(guest, 1, false),
          "private nonidentity copy and retirement use native alias correctly");
}
#endif
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
    Check(address != 0 &&
          space->MapBacking(address, 2 * Page, 8 * Page, Mode::ReadWrite),
          "provenance fixture unique backing");
    // A reservation query returns the next available address; Darwin may skip
    // both original hints to the same gap. Select the alias beyond the first
    // fixture's complete span instead of assuming either hint was honored.
    const auto alias = space->ReserveAligned(address + 3 * Page, Page, Page);
    Check(alias >= address + 3 * Page, "provenance fixture distinct alias address");
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
#if defined(__APPLE__)
uint64_t translated_watched {};
bool translated_watch_write {};
unsigned translated_watch_repairs {};
bool close_translated_after_query {};
unsigned translated_injected_watches {};
unsigned translated_nested_faults {};
bool QueryHostAndInjectWatch(uint64_t address, uint64_t size, bool write) noexcept {
    const bool accessible = Memory::QueryTranslatedHostAccess(address, size, write);
    if (accessible && close_translated_after_query && address >= translated_watched &&
        address - translated_watched < Page && write == translated_watch_write) {
        close_translated_after_query = false;
        if (!space->ProtectTransient(translated_watched, Page, write ? Mode::Read : Mode::NoAccess))
            _exit(81);
        ++translated_injected_watches;
        // Deliberately return the observation from before the real mprotect.
        // The next protected operand must take an ordinary nested alias fault.
    }
    return accessible;
}
bool ResolveTranslatedWatch(uint64_t address, bool write) noexcept {
    if (address < translated_watched || address - translated_watched >= Page ||
        write != translated_watch_write) return false;
    if (!space->ProtectTransient(translated_watched, Page, Mode::ReadWrite)) return false;
    ++translated_watch_repairs;
    errno = ENOENT; // The signal dispatcher must preserve the interrupted errno.
    return true;
}
bool HandleTranslatedAliasFault(const Common::HostException::ExceptionInfo& info) {
    using namespace Common::HostException;
    if (info.type != ExceptionType::AccessViolation ||
        info.access_violation_vaddr != translated_watched ||
        !Memory::IsGuestNonIdentity(info.access_violation_vaddr, 1) ||
        info.exception_address == 0 || info.native_context == nullptr ||
        info.access_violation_type != (translated_watch_write ? AccessViolationType::Write : AccessViolationType::Read))
        return false;
    ++translated_nested_faults;
    return ResolveTranslatedWatch(info.access_violation_vaddr, translated_watch_write);
}
extern "C" __attribute__((naked)) uint64_t ReadTranslatedNative(const void*) {
    asm volatile("ldr x0, [x0]\nret\n");
}
extern "C" __attribute__((naked)) void WriteTranslatedNative(void*, uint64_t) {
    asm volatile("str x1, [x0]\nret\n");
}
bool transition_write_expected {};
unsigned transition_before {}, transition_after {}, transition_writes {};
constexpr uint64_t TransitionValue = 0x0123456789abcdefull;
void CheckTranslatedTransition(uint64_t address, uint64_t size, bool native_applied) {
    Check(size == Page && Memory::QueryTranslatedGuestAccess(address, size, false) &&
          Memory::QueryTranslatedGuestAccess(address, size, true) == transition_write_expected &&
          ReadTranslatedNative(reinterpret_cast<const void*>(address)) == TransitionValue,
          "real guest-VA read remains valid during semantic protection/remap transition");
    if (transition_write_expected) {
        WriteTranslatedNative(reinterpret_cast<void*>(address), TransitionValue);
        ++transition_writes;
    }
    ++(native_applied ? transition_after : transition_before);
}
void CheckTranslatedCpuAccess() {
    constexpr uint64_t guest = 0xfd0000000ull;
    constexpr uint64_t initial = 0x1122334455667788ull;
    constexpr uint64_t replacement = 0x8877665544332211ull;
    const auto code = space->ReserveAligned(0x350000000ull, Page, Page);
    Check(code != 0 && space->Commit(code, Page, Mode::ReadWrite), "translated data fixture identity code");
    const auto stack = space->ReserveAligned(code + Page, Page, Page);
    Check(stack != 0 && space->Commit(stack, Page, Mode::ReadWrite) &&
          space->MapBacking(guest, Page, 40 * Page, Mode::ReadWrite),
          "translated data fixture identity stack and shared data");
    uint64_t host = 0;
    Check(Memory::TranslateGuestMemory(guest, Page, &host) && host != guest,
          "translated CPU fixture requires real nonidentity data");
    static const Common::HostException::TranslatedMemoryProvider provider {
        Memory::IsGuestNonIdentity, Memory::TranslateGuestMemory, Memory::TranslateHostMemoryFault,
        Memory::QueryTranslatedGuestAccess, QueryHostAndInjectWatch, ResolveTranslatedWatch};
    Common::HostException::SetTranslatedMemoryProvider(&provider);
    Check(Common::HostException::InitializeThreadSignalStack() &&
          Common::HostException::InstallHandler(HandleTranslatedAliasFault),
          "production HostException translated-memory provider installed");

    // Actual native HLE/libc accesses retain guest pointer values. No per-call
    // alias conversion is used here: the production ARM64 fault path executes
    // each faulting memory instruction against the protected native view.
    void* (*volatile native_memset)(void*, int, size_t) = &std::memset;
    void* (*volatile native_memcpy)(void*, const void*, size_t) = &std::memcpy;
    std::array<uint8_t, 192> native_copy {};
    native_memset(reinterpret_cast<void*>(guest), 0x3a, native_copy.size());
    native_memcpy(native_copy.data(), reinterpret_cast<const void*>(guest), native_copy.size());
    Check(std::all_of(native_copy.begin(), native_copy.end(), [](uint8_t byte) { return byte == 0x3a; }) &&
          std::memcmp(native_copy.data(), reinterpret_cast<const void*>(host), native_copy.size()) == 0,
          "native libc HLE memset/memcpy access original guest VA through fault translation");

    // Inject actual guest-VA accesses in both transition windows: before the
    // native change and after metadata removal but before replacement. This
    // catches transient permission loss deterministically, without scheduling
    // a racing thread or replacing the production signal/resolver path.
    *reinterpret_cast<uint64_t*>(host) = TransitionValue;
    Memory::g_test_translated_mapping_phase = CheckTranslatedTransition;
    transition_write_expected = false;
    Check(space->Protect(guest, Page, Mode::Read) &&
          !Memory::QueryTranslatedGuestAccess(guest, Page, true) &&
          space->Protect(guest, Page, Mode::ReadWrite) &&
          Memory::QueryTranslatedGuestAccess(guest, Page, true),
          "permission intersection preserves reads across downgrade and upgrade");
    transition_write_expected = true;
    Check(space->Protect(guest, Page, Mode::ReadWrite),
          "unchanged RW protection preserves real native guest reads and writes");
    transition_write_expected = false;
    Check(space->MapBacking(guest, Page, 40 * Page, Mode::Read) &&
          !Memory::QueryTranslatedGuestAccess(guest, Page, true),
          "fixed backing replacement preserves common permissions throughout publication");
    Memory::g_test_translated_mapping_phase = nullptr;
    Check(space->Protect(guest, Page, Mode::ReadWrite), "restore translated transition fixture rights");
    Memory::g_test_translated_mapping_phase = CheckTranslatedTransition;
    Check(!space->MapBacking(guest, Page, BackingSize, Mode::Read) &&
          Memory::QueryTranslatedGuestAccess(guest, Page, true) &&
          ReadTranslatedNative(reinterpret_cast<const void*>(guest)) == TransitionValue,
          "failed fixed replacement restores prior guest permissions and contents");
    Memory::g_test_translated_mapping_phase = nullptr;
    Check(transition_before == 5 && transition_after == 4 && transition_writes == 2,
          "all protection and remap transition windows executed through real native faults");

    std::vector<uint8_t> reader {0x48, 0xbb}; // movabs rbx,guest; mov rax,[rbx]; ret
    const auto append64 = [](std::vector<uint8_t>& bytes, uint64_t value) {
        const auto* data = reinterpret_cast<const uint8_t*>(&value);
        bytes.insert(bytes.end(), data, data + sizeof(value));
    };
    append64(reader, guest);
    reader.insert(reader.end(), {0x48, 0x8b, 0x03, 0xc3});
    std::vector<uint8_t> writer {0x48, 0xbb};
    append64(writer, guest);
    writer.insert(writer.end(), {0x48, 0xb8});
    append64(writer, initial);
    writer.insert(writer.end(), {0x48, 0x89, 0x03, 0xc3});
    std::vector<uint8_t> compare_exchange {0x48, 0xbb};
    append64(compare_exchange, guest);
    compare_exchange.insert(compare_exchange.end(), {0x48, 0xb8});
    append64(compare_exchange, initial);
    compare_exchange.insert(compare_exchange.end(), {0x48, 0xb9});
    append64(compare_exchange, replacement);
    compare_exchange.insert(compare_exchange.end(), {
        0xf0, 0x48, 0x0f, 0xb1, 0x0b, // lock cmpxchg qword [rbx],rcx
        0x0f, 0x94, 0xc2, 0x0f, 0xb6, 0xc2, 0xc3}); // sete dl; movzx eax,dl; ret
    std::vector<uint8_t> simd {0x48, 0xbb};
    append64(simd, guest);
    simd.insert(simd.end(), {0xf3, 0x0f, 0x6f, 0x03, // movdqu xmm0,[rbx]
                            0xf3, 0x0f, 0x7f, 0x43, 0x20, // movdqu [rbx+32],xmm0
                            0x66, 0x48, 0x0f, 0x7e, 0xc0, 0xc3}); // movq rax,xmm0; ret
    std::memcpy(reinterpret_cast<void*>(code), reader.data(), reader.size());
    std::memcpy(reinterpret_cast<void*>(code + 128), writer.data(), writer.size());
    std::memcpy(reinterpret_cast<void*>(code + 256), compare_exchange.data(), compare_exchange.size());
    std::memcpy(reinterpret_cast<void*>(code + 384), simd.data(), simd.size());
    Check(space->Protect(code, Page, Mode::ExecuteRead), "translated data fixture x86 code published");
    Check(Run(code + 128, stack) == initial && *reinterpret_cast<uint64_t*>(host) == initial &&
          Run(code, stack) == initial,
          "actual FEX x86 scalar store/load use original nonidentity guest address");
    Check(Run(code + 256, stack) == 1 && *reinterpret_cast<uint64_t*>(host) == replacement &&
          Run(code + 256, stack) == 0 && *reinterpret_cast<uint64_t*>(host) == replacement,
          "actual FEX locked compare-exchange preserves success/failure flags and atomic value");
    std::array<uint8_t, 16> vector {};
    std::memcpy(vector.data(), reinterpret_cast<const void*>(host), vector.size());
    Check(Run(code + 384, stack) == replacement &&
          std::memcmp(reinterpret_cast<const void*>(host + 32), vector.data(), vector.size()) == 0,
          "actual FEX SIMD load/store preserve all 128 bits across guest VA translation");
    translated_watched = guest;
    translated_watch_write = false;
    Check(space->ProtectTransient(guest, Page, Mode::NoAccess) &&
          Run(code, stack) == replacement && translated_watch_repairs == 1,
          "translated FEX read repairs native alias watch before emulated access");
    translated_watch_write = true;
    Check(space->ProtectTransient(guest, Page, Mode::Read) &&
          Run(code + 128, stack) == initial && translated_watch_repairs == 2 &&
          *reinterpret_cast<uint64_t*>(host) == initial,
          "translated FEX write repairs native alias watch without nested host fault");

    const auto neighbor = *reinterpret_cast<uint64_t*>(host + sizeof(uint64_t));
    translated_watch_write = false;
    close_translated_after_query = true;
    errno = EDOM;
    const auto native_value = ReadTranslatedNative(reinterpret_cast<const void*>(guest));
    const auto read_errno = errno;
    Check(native_value == initial && *reinterpret_cast<uint64_t*>(host) == initial &&
          translated_injected_watches == 1 && translated_nested_faults == 1 &&
          translated_watch_repairs == 3 && read_errno == EDOM && !close_translated_after_query,
          "real nested native read fault repairs post-preflight watch and preserves errno/data");
    translated_watch_write = true;
    close_translated_after_query = true;
    errno = EDOM;
    WriteTranslatedNative(reinterpret_cast<void*>(guest), replacement);
    const auto write_errno = errno;
    Check(*reinterpret_cast<uint64_t*>(host) == replacement &&
          *reinterpret_cast<uint64_t*>(host + sizeof(uint64_t)) == neighbor &&
          translated_injected_watches == 2 && translated_nested_faults == 2 &&
          translated_watch_repairs == 4 && write_errno == EDOM && !close_translated_after_query,
          "real nested native write fault repairs post-preflight watch and preserves errno/neighbor");
    translated_watch_write = false;
    close_translated_after_query = true;
    Check(Run(code, stack) == replacement && translated_injected_watches == 3 &&
          translated_nested_faults == 3 && translated_watch_repairs == 5,
          "actual FEX read resumes across deterministic post-preflight native watch race");
    translated_watch_write = true;
    close_translated_after_query = true;
    Check(Run(code + 128, stack) == initial && *reinterpret_cast<uint64_t*>(host) == initial &&
          *reinterpret_cast<uint64_t*>(host + sizeof(uint64_t)) == neighbor &&
          translated_injected_watches == 4 && translated_nested_faults == 4 && translated_watch_repairs == 6,
          "actual FEX write resumes across deterministic post-preflight native watch race");
    translated_watched = 0;
    Common::HostException::SetTranslatedMemoryProvider(nullptr);
    Check(space->UnmapBacking(guest, Page) && space->ReleaseCommitted(code, Page) &&
          space->ReleaseCommitted(stack, Page), "translated CPU fixture retired");
}
#endif
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

void CheckShaderBackingReads() {
    const auto address = space->ReserveAligned(0x330000000ull, 4 * Page, Page);
    Check(address != 0 && space->MapBacking(address, Page, 44 * Page, Mode::ReadWrite) &&
          space->MapBacking(address + Page, Page, 46 * Page, Mode::Read),
          "shader read adjacent mappings with disjoint physical backing");
    const std::array<uint32_t, 2> expected {0x1234abcdu, 0x5678ef01u};
    Check(space->TryWriteBacking(address + Page - 4, expected.data(), sizeof(expected)),
          "shader cross-mapping source");
    std::array<uint32_t, 2> actual {};
    Check(space->CanReadShaderBacking(address + Page - 4, sizeof(actual)) &&
          space->TryReadShaderBacking(address + Page - 4, actual.data(), sizeof(actual)) &&
          actual == expected, "shader complete-span backing copy");
    std::atomic<bool> concurrent_valid {true};
    std::jthread protector([&] {
        for (unsigned i = 0; i < 64; ++i) {
            if (!space->Protect(address + Page, Page, Mode::NoAccess) ||
                !space->Protect(address + Page, Page, Mode::Read)) concurrent_valid = false;
        }
    });
    for (unsigned i = 0; i < 128; ++i) {
        std::array<uint32_t, 2> concurrent {0xeeeeeeeeu, 0xeeeeeeeeu};
        const auto before = concurrent;
        const bool read = space->TryReadShaderBacking(address + Page - 4,
                                                       concurrent.data(), sizeof(concurrent));
        if (read ? concurrent != expected : concurrent != before) concurrent_valid = false;
    }
    protector.join();
    Check(concurrent_valid, "shader concurrent protection retains complete-copy or no-copy outcome");
    Check(space->ProtectTransient(address, Page, Mode::NoAccess) &&
          space->TryReadShaderBacking(address + Page - 4, actual.data(), sizeof(actual)) &&
          actual == expected, "shader logical eligibility ignores temporary GPU read watch");
    Check(space->ProtectTransient(address, Page, Mode::ReadWrite), "restore shader read watch");
    Check(space->CanReadShaderBacking(address + Page, 4) &&
          space->Protect(address + Page, Page, Mode::NoAccess),
          "shader permissions change after eligibility check");
    actual.fill(0xeeeeeeeeu);
    const auto sentinel = actual;
    Check(!space->CanReadShaderBacking(address + Page - 4, sizeof(actual)) &&
          !space->TryReadShaderBacking(address + Page - 4, actual.data(), sizeof(actual)) &&
          actual == sentinel, "shader denied tail leaves entire destination untouched");
    Check(space->Protect(address + Page, Page, Mode::Execute) &&
          space->TryReadShaderBacking(address + Page, actual.data(), 4) &&
          actual[0] == expected[1], "shader executable mapping retains effective host read");
    Check(space->CanReadShaderBacking(address + Page, 4) &&
          space->UnmapBacking(address + Page, Page), "shader unmap after eligibility check");
    actual = sentinel;
    Check(!space->TryReadShaderBacking(address + Page - 4, actual.data(), sizeof(actual)) &&
          actual == sentinel, "shader unmapped tail denied before copying prefix");
    Check(!space->CanReadShaderBacking(address + 2 * Page, 4) &&
          !space->TryReadShaderBacking(address + 2 * Page, actual.data(), 4) &&
          actual == sentinel, "shader reserved uncommitted storage rejected");
    Check(space->Commit(address + 2 * Page, Page, Mode::ReadWrite) &&
          !space->CanReadShaderBacking(address + 2 * Page, 4) &&
          !space->TryReadShaderBacking(address + 2 * Page, actual.data(), 4) &&
          actual == sentinel, "private shader memory remains on watched fallback");
    Check(!space->CanReadShaderBacking(0, 4) &&
          !space->CanReadShaderBacking(address, 0) &&
          !space->CanReadShaderBacking(UINT64_MAX - 1, 4) &&
          !space->TryReadShaderBacking(address, nullptr, 4), "shader invalid spans rejected");
    Check(space->UnmapBacking(address, Page) &&
          space->ReleaseCommitted(address + 2 * Page, Page), "shader read fixture retirement");
}

int main() {
    const auto host_page = sysconf(_SC_PAGESIZE);
    Check(host_page == 4096 || host_page == 16384, "supported 4 or 16 KiB host");
    Check(Direct::Probe() == 0, "real direct FEX bootstrap");
#if defined(__APPLE__)
    const auto temporary_guard = CreateReusableNativeGuard();
#endif
    auto owner = std::make_unique<Memory::GuestAddressSpace>(BackingSize);
    space = owner.get();
    Check(space->GetBackingSize() == BackingSize, "full PS5 13824 MiB backing");
#if defined(__ANDROID__)
    Check(Memory::HOST_USER_MAX == 0x3fffffffffull, "Gen4 Android 256 GiB host user boundary");
#endif
    CheckBackingProvenance();
    CheckShaderBackingReads();
#if defined(__APPLE__)
    CheckDarwinPermanentReservationAdmission(temporary_guard);
    CheckDarwinTranslationContract();
    CheckDarwinPageContract();
#endif
    Memory::g_guest_memory_ready = true;
#if defined(__APPLE__)
    CheckTranslatedCpuAccess();
#endif
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
