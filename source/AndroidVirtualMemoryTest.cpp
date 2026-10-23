#include "Lumen/Core/VirtualMemory.h"
#include "Lumen/Sys/Linux/SysLinuxVirtual.h"
#include "cpu/runtime/MemoryHandoff.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <limits>
#include <memory>
#if defined(__aarch64__)
#include <asm/hwcap.h>
#endif
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID
#error "This test exercises the Android VM backend"
#endif

namespace {

namespace Vm = Lumen::Core::VirtualMemory;
constexpr uint64_t kGuestPage = 4096;
constexpr uint64_t kGuestSystemManagedMin = UINT64_C(0x0000040000);
constexpr uint64_t kGuestSystemManagedMax = UINT64_C(0x07ffffbfff);
constexpr uint64_t kGuestUserMin = UINT64_C(0x1000000000);
constexpr uint64_t kGuestUserMax = UINT64_C(0xfbffffffff);
constexpr uint64_t kGuestAnywhereMin = UINT64_C(0x0000400000);

bool IsGuestAnywhereRange(uint64_t begin, uint64_t size)
{
    if (size == 0 || begin > UINT64_MAX - (size - 1)) return false;
    const uint64_t end = begin + size - 1;
    return (begin >= kGuestSystemManagedMin && end <= kGuestSystemManagedMax) ||
           (begin >= kGuestUserMin && end <= kGuestUserMax);
}

struct PartialProtectFailure
{
    void* address = nullptr;
    size_t length = 0;
    size_t prefix = 0;
    bool armed = false;
    bool fired = false;
};
PartialProtectFailure g_partial_protect_failure;

constexpr uint32_t kNoDeniedFexMutation = UINT32_MAX;
std::atomic<uint32_t> g_denied_fex_mutation {kNoDeniedFexMutation};
std::atomic<uint64_t> g_fex_next_token {1};
std::atomic<uint64_t> g_fex_active {0};
std::atomic<uint64_t> g_fex_begins {0};
std::atomic<uint64_t> g_fex_closes {0};
std::atomic<uint64_t> g_fex_commits {0};
std::atomic<uint64_t> g_fex_last_base {0};
std::atomic<uint64_t> g_fex_last_size {0};
std::atomic<uint64_t> g_fex_fresh_notifications {0};
std::atomic<uint64_t> g_fex_fresh_base {0};
std::atomic<uint64_t> g_fex_fresh_size {0};
std::atomic<uint32_t> g_fex_fresh_kind {UINT32_MAX};
std::atomic<bool> g_fex_fresh_without_mutation {false};
std::atomic<uint32_t> g_fex_last_begin_kind {UINT32_MAX};
thread_local bool g_fex_host_reader = false;
thread_local uint32_t g_fex_guest_data_leases = 0;
thread_local uint32_t g_fex_mutation_depth = 0;
struct ModeledMutationOwner { uint64_t token = 0; uint32_t kind = UINT32_MAX; };
thread_local std::array<ModeledMutationOwner, 64> g_fex_mutation_owners;
thread_local bool g_fex_retryable_conflict = false;
thread_local bool g_fex_last_retryable_conflict = false;
std::atomic<uint64_t> g_fex_data_retains {0};
std::atomic<uint64_t> g_fex_data_releases {0};
std::atomic<bool> g_native_data_admission {false};
std::atomic<bool> g_native_data_mutation_pending {false};
std::atomic<uint64_t> g_native_data_pins {0};
std::atomic<uint64_t> g_native_data_unpins {0};
thread_local uint64_t g_native_data_token = 0;
enum class CopiedReadFailure { None, Denied, Short };
thread_local CopiedReadFailure g_copied_read_failure = CopiedReadFailure::None;
std::atomic<uint64_t> g_copied_read_calls {0};
std::atomic<bool> g_copied_read_without_admission {false};
enum class CopiedWriteFailure { None, Denied, Zero, Short };
thread_local CopiedWriteFailure g_copied_write_failure = CopiedWriteFailure::None;
thread_local uint8_t* g_copied_write_source_to_mutate = nullptr;
std::atomic<uint64_t> g_copied_write_calls {0};
std::atomic<bool> g_copied_write_invalid_contract {false};
enum class CopiedReadCapability { NoMte, Mte, Unavailable };
CopiedReadCapability g_copied_read_capability = CopiedReadCapability::NoMte;
std::atomic<uint64_t> g_copied_read_capability_queries {0};

__attribute__((constructor(101))) void ArmTestTimeout()
{
    // Bound fixture deadlocks independently of the launcher's startup timeout.
    std::signal(SIGALRM, +[](int)
    {
        constexpr char message[] = "FAIL: Android VM fixture exceeded 20 seconds\n";
        syscall(SYS_write, STDERR_FILENO, message, sizeof(message) - 1);
        _exit(124);
    });
    alarm(20);
}

} // namespace

// The capability gate is immutable after its first read. Separate executable
// modes set the modeled capability before admission; other auxv entries remain
// the kernel-provided values, including on host compilers without ARM headers.
extern "C" unsigned long getauxval(unsigned long type)
#if defined(__GLIBC__)
    noexcept
#endif
{
    if (type == AT_HWCAP2)
    {
        g_copied_read_capability_queries.fetch_add(1, std::memory_order_relaxed);
        if (g_copied_read_capability == CopiedReadCapability::Unavailable)
        {
            errno = ENOENT;
            return 0;
        }
        errno = 0;
        // Stable AArch64 UAPI value; non-ARM host tests need no ARM asm header.
        constexpr unsigned long mte = 1UL << 18;
#if defined(HWCAP2_MTE)
        static_assert(HWCAP2_MTE == mte);
#endif
        return g_copied_read_capability == CopiedReadCapability::Mte ? mte : 0;
    }
    // Scudo can call getauxval while initializing malloc. stdio would allocate
    // and recursively lock that initialization, so use only raw syscalls and
    // stack storage here, including when this executes before main.
    int auxv;
    do {
        auxv = static_cast<int>(syscall(SYS_openat, AT_FDCWD, "/proc/self/auxv", O_RDONLY | O_CLOEXEC, 0));
    } while (auxv < 0 && errno == EINTR);
    if (auxv < 0) return 0;
    struct Entry { unsigned long type, value; } entry {};
    for (;;)
    {
        size_t consumed = 0;
        while (consumed < sizeof(entry))
        {
            const auto count = static_cast<ssize_t>(syscall(SYS_read, auxv,
                reinterpret_cast<char*>(&entry) + consumed, sizeof(entry) - consumed));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0)
            {
                const int error = count < 0 ? errno : ENOENT;
                syscall(SYS_close, auxv);
                errno = error;
                return 0;
            }
            consumed += static_cast<size_t>(count);
        }
        if (entry.type == type)
        {
            syscall(SYS_close, auxv);
            return entry.value;
        }
        if (entry.type == 0) break;
    }
    syscall(SYS_close, auxv);
    errno = ENOENT;
    return 0;
}

// Test-owned HLE admission, independent of the Core VM permission checks below.
// The real barrier's yield/reentry/upgrade guards have their own unit tests.
extern "C" uint32_t LumenAndroidFexCoreRetainGuestDataLeaseV1() noexcept
{
    if (!g_fex_host_reader || g_fex_mutation_depth != 0 ||
        g_fex_guest_data_leases == UINT32_MAX) return 0;
    ++g_fex_guest_data_leases;
    g_fex_data_retains.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

extern "C" uint32_t LumenAndroidFexCoreReleaseGuestDataLeaseV1() noexcept
{
    if (!g_fex_host_reader || g_fex_guest_data_leases == 0) return 0;
    --g_fex_guest_data_leases;
    g_fex_data_releases.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

extern "C" uint64_t LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV2(uint32_t* mutation_pending) noexcept
{
    if (mutation_pending != nullptr) *mutation_pending = 0;
    if (g_fex_host_reader || g_fex_mutation_depth != 0 ||
        g_fex_guest_data_leases != 0 || g_native_data_token != 0) return 0;
    if (!g_native_data_admission.load(std::memory_order_acquire)) return 0;
    if (g_native_data_mutation_pending.load(std::memory_order_acquire))
    {
        if (mutation_pending != nullptr) *mutation_pending = 1;
        return 0;
    }
    g_native_data_token = g_fex_next_token.fetch_add(1, std::memory_order_relaxed);
    g_native_data_pins.fetch_add(1, std::memory_order_relaxed);
    return g_native_data_token;
}

extern "C" uint64_t LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV1() noexcept
{
    return LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV2(nullptr);
}

extern "C" uint32_t LumenAndroidFexCoreEndNativeGuestDataLeaseV1(uint64_t token) noexcept
{
    if (token == 0 || token != g_native_data_token) return 0;
    g_native_data_token = 0;
    g_native_data_unpins.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

extern "C" uint64_t LumenAndroidFexCoreBeginGuestMemoryMutationV2(
    uint32_t kind) noexcept
{
    g_fex_last_retryable_conflict = false;
    g_fex_last_begin_kind.store(kind, std::memory_order_relaxed);
    if (g_fex_guest_data_leases != 0 || g_native_data_token != 0) return 0;
    uint32_t denied = kind;
    if (g_denied_fex_mutation.compare_exchange_strong(
            denied, kNoDeniedFexMutation, std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        g_fex_last_retryable_conflict = kind == 1 && g_fex_retryable_conflict && g_fex_host_reader && g_fex_mutation_depth == 0;
        return 0;
    }
    g_fex_begins.fetch_add(1, std::memory_order_relaxed);
    g_fex_active.fetch_add(1, std::memory_order_release);
    if (g_fex_mutation_depth == g_fex_mutation_owners.size()) std::abort();
    const auto token = g_fex_next_token.fetch_add(1, std::memory_order_relaxed);
    g_fex_mutation_owners[g_fex_mutation_depth] = {token, kind};
    ++g_fex_mutation_depth;
    return token;
}

extern "C" uint32_t LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(uint64_t token) noexcept
{
    if (token == 0) return 0;
    for (uint32_t index = 0; index < g_fex_mutation_depth; ++index)
        if (g_fex_mutation_owners[index].token == token && g_fex_mutation_owners[index].kind == 0) return 1;
    return 0;
}

extern "C" uint64_t LumenAndroidFexCoreBeginGuestMemoryMutationV3(
    uint32_t kind, uint32_t* retryable) noexcept
{
    if (retryable != nullptr) *retryable = 0;
    const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(kind);
    if (token == 0 && retryable != nullptr && g_fex_last_retryable_conflict) *retryable = 1;
    return token;
}

extern "C" void LumenAndroidFexCoreEndGuestMemoryMutationV1(
    uint64_t token, uint64_t range_base, uint64_t range_size,
    uint32_t committed) noexcept
{
    if (token == 0) return;
    g_fex_last_base.store(range_base, std::memory_order_relaxed);
    g_fex_last_size.store(range_size, std::memory_order_relaxed);
    if (committed != 0)
        g_fex_commits.fetch_add(1, std::memory_order_relaxed);
    g_fex_closes.fetch_add(1, std::memory_order_relaxed);
    g_fex_active.fetch_sub(1, std::memory_order_release);
    if (g_fex_mutation_depth == 0) std::abort();
    if (g_fex_mutation_owners[g_fex_mutation_depth - 1].token != token) std::abort();
    --g_fex_mutation_depth;
    g_fex_mutation_owners[g_fex_mutation_depth] = {};
}

// Record the actual Core lifetime notification independently of its JIT
// invalidation receipt. The runtime watch implementation is not linked here.
extern "C" void LumenAndroidFexCoreNotifyFreshGuestCallbackMappingChangeV1(
    uint64_t range_base, uint64_t range_size) noexcept
{
    if (g_fex_mutation_depth == 0)
        g_fex_fresh_without_mutation.store(true, std::memory_order_relaxed);
    g_fex_fresh_kind.store(g_fex_mutation_depth == 0 ? UINT32_MAX :
        g_fex_mutation_owners[g_fex_mutation_depth - 1].kind, std::memory_order_relaxed);
    g_fex_fresh_base.store(range_base, std::memory_order_relaxed);
    g_fex_fresh_size.store(range_size, std::memory_order_relaxed);
    g_fex_fresh_notifications.fetch_add(1, std::memory_order_relaxed);
}

// Interpose only in this test executable. Emulate the documented syscall
// failure after a first VMA changed, while every successful operation and
// rollback still uses the real kernel mprotect on the actual host pages.
extern "C" int mprotect(void* address, size_t length, int protection)
{
    auto& failure = g_partial_protect_failure;
    if (failure.armed && address == failure.address && length == failure.length)
    {
        failure.armed = false;
        if (syscall(SYS_mprotect, address, failure.prefix, protection) != 0) return -1;
        failure.fired = true;
        errno = ENOMEM;
        return -1;
    }
    return static_cast<int>(syscall(SYS_mprotect, address, length, protection));
}

// The copied-read path must retain real admission through the kernel access.
// Inject failures only here; ordinary cases exercise the actual self syscall.
extern "C" ssize_t process_vm_readv(pid_t pid, const iovec* local, unsigned long local_count,
    const iovec* remote, unsigned long remote_count, unsigned long flags)
{
    g_copied_read_calls.fetch_add(1, std::memory_order_relaxed);
    if (!((g_fex_host_reader && g_fex_guest_data_leases != 0) ||
          g_native_data_token != 0 || g_fex_mutation_depth != 0))
        g_copied_read_without_admission.store(true, std::memory_order_relaxed);
    const auto failure = g_copied_read_failure;
    g_copied_read_failure = CopiedReadFailure::None;
    if (failure != CopiedReadFailure::None)
    {
        if (local_count != 1 || local[0].iov_len == 0) std::abort();
        // Even bytes written before a short result or error must stay private.
        std::memset(local[0].iov_base, 0xc7, local[0].iov_len);
        if (failure == CopiedReadFailure::Short)
            return static_cast<ssize_t>(local[0].iov_len - 1);
        errno = EPERM;
        return -1;
    }
    return static_cast<ssize_t>(syscall(SYS_process_vm_readv, pid, local, local_count,
                                      remote, remote_count, flags));
}

extern "C" ssize_t process_vm_writev(pid_t pid, const iovec* local, unsigned long local_count,
    const iovec* remote, unsigned long remote_count, unsigned long flags)
{
    g_copied_write_calls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    if (!((g_fex_host_reader && g_fex_guest_data_leases != 0) ||
          g_native_data_token != 0 || g_fex_mutation_depth != 0) ||
        local_count != 1 || remote_count != 1 || flags != 0 || pid != getpid() ||
        local[0].iov_len == 0 || local[0].iov_len > 256 ||
        remote[0].iov_len != local[0].iov_len ||
        (reinterpret_cast<uintptr_t>(local[0].iov_base) & 255) != 0 ||
        local[0].iov_len > page - (reinterpret_cast<uintptr_t>(local[0].iov_base) & (page - 1)) ||
        remote[0].iov_len > page - (reinterpret_cast<uintptr_t>(remote[0].iov_base) & (page - 1)))
    {
        g_copied_write_invalid_contract.store(true, std::memory_order_relaxed);
        errno = EINVAL;
        return -1;
    }
    // The original input can change once dispatch begins; the local iovec must
    // still contain the complete owned snapshot taken before any guest store.
    if (g_copied_write_source_to_mutate != nullptr)
    {
        if (local[0].iov_base == g_copied_write_source_to_mutate)
            g_copied_write_invalid_contract.store(true, std::memory_order_relaxed);
        std::memset(g_copied_write_source_to_mutate, 0xc7, local[0].iov_len);
        g_copied_write_source_to_mutate = nullptr;
    }
    const auto failure = g_copied_write_failure;
    g_copied_write_failure = CopiedWriteFailure::None;
    if (failure == CopiedWriteFailure::Short)
    {
        if (local[0].iov_len < 2) std::abort();
        std::memcpy(remote[0].iov_base, local[0].iov_base, local[0].iov_len - 1);
        return static_cast<ssize_t>(local[0].iov_len - 1);
    }
    if (failure == CopiedWriteFailure::Denied) { errno = EPERM; return -1; }
    if (failure == CopiedWriteFailure::Zero) return 0;
    return static_cast<ssize_t>(syscall(SYS_process_vm_writev, pid, local, local_count,
                                      remote, remote_count, flags));
}

namespace {

#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); return false; \
} } while (false)

int PhysicalProtection(uint64_t address)
{
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return -1;
    char line[1024] {};
    int result = -1;
    while (std::fgets(line, sizeof(line), maps) != nullptr)
    {
        uint64_t begin = 0;
        uint64_t end = 0;
        char rights[5] {};
        if (std::sscanf(line, "%" SCNx64 "-%" SCNx64 " %4s", &begin, &end, rights) == 3 &&
            address >= begin && address < end)
        {
            result = (rights[0] == 'r' ? PROT_READ : 0) |
                     (rights[1] == 'w' ? PROT_WRITE : 0) |
                     (rights[2] == 'x' ? PROT_EXEC : 0);
            break;
        }
    }
    std::fclose(maps);
    return result;
}

bool IsMode(uint64_t address, Vm::Mode mode)
{
    Vm::ManagedRegion region {};
    return Vm::QueryManagedRegion(address, &region) == Vm::ManagedRegionState::Visible &&
           region.begin <= address && region.end > address && region.mode == mode;
}

bool AccessDenied(uint64_t address, bool write)
{
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0)
    {
        struct sigaction action {};
        action.sa_handler = [](int signal) { _exit(signal); };
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGSEGV, &action, nullptr) != 0 ||
            sigaction(SIGBUS, &action, nullptr) != 0) _exit(1);
        auto* byte = reinterpret_cast<volatile uint8_t*>(address);
        if (write) *byte = 0x5a;
        else (void)*byte;
        _exit(0);
    }
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           (WEXITSTATUS(status) == SIGSEGV || WEXITSTATUS(status) == SIGBUS);
}

bool ProtectionMatrix(uint64_t host_page)
{
    // Independent expected data classes; guest X never grants native ARM X.
    constexpr int rights[] = {0, PROT_READ, PROT_READ | PROT_WRITE,
        PROT_READ | PROT_WRITE, PROT_READ, PROT_READ,
        PROT_READ | PROT_WRITE, PROT_READ | PROT_WRITE};
    const uint64_t mapping = Vm::Alloc(0, 2 * host_page, Vm::Mode::ReadWrite);
    CHECK(mapping != 0 && mapping % host_page == 0);
    for (uint32_t initial = 0; initial < 8; initial++)
    {
        for (uint32_t desired = 0; desired < 8; desired++)
        {
            for (uint64_t offset = 0; offset < host_page; offset += kGuestPage)
            {
                const auto before = static_cast<Vm::Mode>(initial);
                const auto after = static_cast<Vm::Mode>(desired);
                CHECK(Vm::Protect(mapping, 2 * host_page, before));
                const uint64_t generation = Vm::GetMutationGeneration();
                auto old = Vm::Mode::ExecuteWrite;
                const bool expected = host_page == kGuestPage || rights[initial] == rights[desired];
                CHECK(Vm::Protect(mapping + offset + 3, 1, after, &old) == expected);
                CHECK(old == (expected ? before : Vm::Mode::ExecuteWrite));
                CHECK(Vm::GetMutationGeneration() == generation + (expected ? 1 : 0));
                for (uint64_t page = 0; page < 2 * host_page; page += kGuestPage)
                {
                    CHECK(IsMode(mapping + page, expected && page == offset ? after : before));
                    CHECK(PhysicalProtection(mapping + page) ==
                          (expected && page < host_page ? rights[desired] : rights[initial]));
                }
            }
        }
    }
    return Vm::Free(mapping);
}

bool AtomicRejectionAndNativeFaults(uint64_t host_page)
{
    const uint64_t mapping = Vm::Alloc(0, 3 * host_page, Vm::Mode::ReadWrite);
    CHECK(mapping != 0);
    std::memset(reinterpret_cast<void*>(mapping), 0x31, 3 * host_page);
    if (host_page > kGuestPage)
    {
        const uint64_t generation = Vm::GetMutationGeneration();
        auto old = Vm::Mode::ExecuteWrite;
        CHECK(!Vm::Protect(mapping, host_page + kGuestPage, Vm::Mode::Read, &old));
        CHECK(old == Vm::Mode::ExecuteWrite && generation == Vm::GetMutationGeneration());
        for (uint64_t offset = 0; offset < 3 * host_page; offset += kGuestPage)
        {
            CHECK(IsMode(mapping + offset, Vm::Mode::ReadWrite));
            CHECK(PhysicalProtection(mapping + offset) == (PROT_READ | PROT_WRITE));
        }
    }
    CHECK(Vm::Protect(mapping, host_page, Vm::Mode::Read));
    CHECK(Vm::Protect(mapping + host_page, host_page, Vm::Mode::NoAccess));
    for (uint64_t offset = 0; offset < host_page; offset += kGuestPage)
    {
        CHECK(*reinterpret_cast<volatile uint8_t*>(mapping + offset) == 0x31);
        CHECK(AccessDenied(mapping + offset, true));
        CHECK(AccessDenied(mapping + host_page + offset, false));
        CHECK(AccessDenied(mapping + host_page + offset, true));
    }
    CHECK(PhysicalProtection(mapping + 2 * host_page) == (PROT_READ | PROT_WRITE));
    return Vm::Free(mapping);
}

bool PaddingAndAllocation(uint64_t host_page)
{
    const uint64_t explicit_low = Vm::Alloc(kGuestSystemManagedMin, host_page,
                                             Vm::Mode::ReadWrite);
    CHECK(explicit_low == kGuestSystemManagedMin);
    *reinterpret_cast<volatile uint8_t*>(explicit_low) = 0x2a;
    CHECK(Vm::Free(explicit_low));

    const uint64_t small = Vm::Alloc(0, 1, Vm::Mode::ReadWrite);
    CHECK(small >= kGuestAnywhereMin && IsGuestAnywhereRange(small, 1));
    Vm::ManagedRegion padding {};
    CHECK(Vm::QueryManagedRegion(small + 1, &padding) == Vm::ManagedRegionState::Reserved);
    CHECK(padding.begin == small + 1 && padding.end == small + host_page);
    CHECK(!Vm::IsManagedAddress(small + 1));
    CHECK(!Vm::Protect(small + 1, 1, Vm::Mode::Read));
    CHECK(Vm::Protect(small, 1, Vm::Mode::NoAccess));
    CHECK(AccessDenied(small, false));
    CHECK(Vm::Free(small));
    CHECK(Vm::QueryManagedRegion(small + 1, &padding) == Vm::ManagedRegionState::Unmanaged);

    constexpr uint64_t direct_memory_alignment = UINT64_C(0x200000);
    const uint64_t aligned = Vm::AllocAligned(0, host_page + 1, Vm::Mode::ReadWrite,
                                              direct_memory_alignment);
    CHECK(aligned >= kGuestAnywhereMin && aligned % direct_memory_alignment == 0 &&
          IsGuestAnywhereRange(aligned, host_page + 1));
    CHECK(Vm::Protect(aligned + host_page, 1, Vm::Mode::Read));
    CHECK(PhysicalProtection(aligned) == (PROT_READ | PROT_WRITE));
    CHECK(PhysicalProtection(aligned + host_page) == PROT_READ);
    CHECK(Vm::QueryManagedRegion(aligned + host_page + 1, &padding) == Vm::ManagedRegionState::Reserved);
    CHECK(Vm::Free(aligned));
    CHECK(Vm::AllocFixed(aligned, host_page, Vm::Mode::ReadWrite));
    *reinterpret_cast<volatile uint8_t*>(aligned) = 0x42;
    CHECK(!Vm::AllocFixed(aligned, host_page, Vm::Mode::NoAccess));
    CHECK(*reinterpret_cast<volatile uint8_t*>(aligned) == 0x42);
    if (host_page > kGuestPage)
        CHECK(!Vm::AllocFixed(aligned + kGuestPage, kGuestPage, Vm::Mode::ReadWrite));
    CHECK(Vm::Free(aligned));
    return true;
}

bool GuestWindowCollisionPlacement(uint64_t host_page)
{
    constexpr uint64_t direct_memory_alignment = UINT64_C(0x200000);
    const uint64_t blocker = Vm::Alloc(0, host_page, Vm::Mode::ReadWrite);
    CHECK(blocker != 0 && IsGuestAnywhereRange(blocker, host_page));

    const uint64_t first = Vm::AllocAligned(0, 2 * host_page, Vm::Mode::ReadWrite,
                                            direct_memory_alignment);
    CHECK(first != 0 && first % direct_memory_alignment == 0 &&
          IsGuestAnywhereRange(first, 2 * host_page));
    const uint64_t second = Vm::AllocAligned(0, 2 * host_page, Vm::Mode::ReadWrite,
                                             direct_memory_alignment);
    CHECK(second != 0 && second != first && second % direct_memory_alignment == 0 &&
          IsGuestAnywhereRange(second, 2 * host_page));

    constexpr uint64_t preferred_hint = UINT64_C(0x200000000);
    const uint64_t preferred = Vm::Alloc(preferred_hint, host_page, Vm::Mode::ReadWrite);
    CHECK(preferred == preferred_hint);
    *reinterpret_cast<volatile uint8_t*>(preferred) = 0x5a;
    const uint64_t relocated = Vm::Alloc(preferred_hint, host_page, Vm::Mode::ReadWrite);
    CHECK(relocated > preferred && IsGuestAnywhereRange(relocated, host_page));
    CHECK(*reinterpret_cast<volatile uint8_t*>(preferred) == 0x5a);

    const size_t foreign_size = static_cast<size_t>(4097 * host_page);
    constexpr uint64_t foreign_hint = UINT64_C(0x300000000);
    const uint64_t foreign_base = Vm::Alloc(foreign_hint, foreign_size,
                                             Vm::Mode::NoAccess);
    CHECK(foreign_base >= foreign_hint &&
          IsGuestAnywhereRange(foreign_base, foreign_size));
    CHECK(Vm::Free(foreign_base));
    void* const foreign = mmap(reinterpret_cast<void*>(foreign_base), foreign_size,
                               PROT_NONE,
                               MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE,
                               -1, 0);
    CHECK(foreign == reinterpret_cast<void*>(foreign_base));
    const uint64_t after_foreign = Vm::Alloc(foreign_base, host_page,
                                              Vm::Mode::ReadWrite);
    CHECK(after_foreign >= foreign_base + foreign_size &&
          IsGuestAnywhereRange(after_foreign, host_page));
    CHECK(munmap(foreign, foreign_size) == 0);

    CHECK(Vm::Free(preferred));
    const uint64_t reused = Vm::Alloc(preferred_hint, host_page, Vm::Mode::ReadWrite);
    CHECK(reused == preferred_hint);
    CHECK(Vm::Free(blocker));
    const uint64_t reused_anywhere = Vm::Alloc(0, host_page, Vm::Mode::ReadWrite);
    CHECK(reused_anywhere == blocker);

    return Vm::Free(reused_anywhere) && Vm::Free(reused) &&
           Vm::Free(after_foreign) && Vm::Free(relocated) && Vm::Free(second) &&
           Vm::Free(first);
}

bool PartialSyscallRollback(uint64_t host_page)
{
    const uint64_t mapping = Vm::Alloc(0, 2 * host_page, Vm::Mode::ReadWrite);
    CHECK(mapping != 0);
    CHECK(Vm::Protect(mapping, host_page, Vm::Mode::Read));
    CHECK(Vm::Protect(mapping + host_page, host_page, Vm::Mode::NoAccess));
    const uint64_t generation = Vm::GetMutationGeneration();
    auto old = Vm::Mode::ExecuteWrite;
    g_partial_protect_failure = {
        reinterpret_cast<void*>(mapping), 2 * host_page, host_page, true, false};
    const bool result = Vm::Protect(mapping, 2 * host_page, Vm::Mode::ReadWrite, &old);
    g_partial_protect_failure.armed = false;
    CHECK(g_partial_protect_failure.fired && !result);
    CHECK(old == Vm::Mode::ExecuteWrite && generation == Vm::GetMutationGeneration());
    for (uint64_t offset = 0; offset < host_page; offset += kGuestPage)
    {
        CHECK(IsMode(mapping + offset, Vm::Mode::Read));
        CHECK(IsMode(mapping + host_page + offset, Vm::Mode::NoAccess));
        CHECK(PhysicalProtection(mapping + offset) == PROT_READ);
        CHECK(PhysicalProtection(mapping + host_page + offset) == PROT_NONE);
    }
    CHECK(AccessDenied(mapping, true));
    CHECK(AccessDenied(mapping + host_page, false));
    return Vm::Free(mapping);
}

bool SparseReservationLifecycle(uint64_t host_page)
{
    const uint64_t alignment = host_page < UINT64_C(0x4000)
                                   ? UINT64_C(0x4000) : host_page;
    const uint64_t reservation_size = 8 * host_page;
    const uint64_t output_mapping =
        Vm::Alloc(0, host_page, Vm::Mode::ReadWrite);
    CHECK(output_mapping != 0);
    Vm::ReservationHandle handle {1, 1, 1};
    Vm::MutationTransaction transaction;

    CHECK(!Vm::BeginReserveAligned(0, 0, alignment, &handle, &transaction));
    CHECK(!handle.IsValid() && !transaction.IsActive());
    CHECK(!Vm::BeginReserveAligned(1, reservation_size, alignment,
                                   &handle, &transaction));
    CHECK(!handle.IsValid() && !transaction.IsActive());
    CHECK(!Vm::BeginReserveAligned(
        alignment, reservation_size, alignment, &handle, &transaction,
        Vm::ReservationPlacement::Exact));
    CHECK(!handle.IsValid() && !transaction.IsActive());
    CHECK(Vm::BeginReserveAligned(
        alignment, reservation_size, alignment, &handle, &transaction,
        Vm::ReservationPlacement::PreferAddress));
    CHECK(handle.base >= kGuestAnywhereMin && handle.base != alignment);
    transaction.Commit();
    CHECK(Vm::ReleaseReservation(handle));
    CHECK(!Vm::BeginReserveAligned(0, reservation_size, 3,
                                   &handle, &transaction));
    CHECK(!handle.IsValid() && !transaction.IsActive());
    CHECK(!Vm::BeginReserveAligned(
        0, reservation_size, alignment, &handle, &transaction,
        static_cast<Vm::ReservationPlacement>(UINT32_MAX)));
    CHECK(!handle.IsValid() && !transaction.IsActive());

    Vm::ReservationHandle explicit_low {};
    Vm::MutationTransaction explicit_low_transaction;
    CHECK(Vm::BeginReserveAligned(
        kGuestSystemManagedMin, reservation_size, alignment, &explicit_low,
        &explicit_low_transaction, Vm::ReservationPlacement::Exact));
    CHECK(explicit_low.base == kGuestSystemManagedMin);
    explicit_low_transaction.Commit();
    CHECK(Vm::ReleaseReservation(explicit_low));

    Vm::ReservationHandle relocated_low {};
    Vm::MutationTransaction relocated_low_transaction;
    CHECK(Vm::BeginReserveAligned(
        kGuestSystemManagedMin, reservation_size, alignment, &relocated_low,
        &relocated_low_transaction, Vm::ReservationPlacement::PreferAddress));
    CHECK(relocated_low.base != kGuestSystemManagedMin &&
          relocated_low.base >= kGuestAnywhereMin &&
          g_fex_last_begin_kind.load(std::memory_order_relaxed) == 1);
    relocated_low_transaction.Commit();
    CHECK(Vm::ReleaseReservation(relocated_low));

    const uint64_t denied_generation = Vm::GetMutationGeneration();
    g_denied_fex_mutation.store(1, std::memory_order_release);
    CHECK(!Vm::BeginReserveAligned(0, reservation_size, alignment,
                                   &handle, &transaction));
    CHECK(!handle.IsValid() && !transaction.IsActive() &&
          Vm::GetMutationGeneration() == denied_generation &&
          g_fex_active.load(std::memory_order_acquire) == 0);

    const uint64_t before_abort = Vm::GetMutationGeneration();
    const uint64_t begins_before_abort =
        g_fex_begins.load(std::memory_order_relaxed);
    CHECK(Vm::BeginReserveAligned(0, reservation_size, alignment,
                                  &handle, &transaction));
    CHECK(handle.IsValid() && handle.base >= kGuestAnywhereMin &&
          handle.base % alignment == 0 &&
          transaction.IsActive() &&
          g_fex_last_begin_kind.load(std::memory_order_relaxed) == 1 &&
          g_fex_begins.load(std::memory_order_relaxed) ==
              begins_before_abort + 1 &&
          g_fex_active.load(std::memory_order_acquire) == 1 &&
          Vm::GetMutationGeneration() == before_abort + 1 &&
          PhysicalProtection(handle.base) == PROT_NONE);
    Vm::ManagedRegion region {};
    CHECK(Vm::QueryManagedRegion(output_mapping, &region) ==
              Vm::ManagedRegionState::Visible &&
          region.begin == output_mapping &&
          region.end == output_mapping + host_page &&
          region.mode == Vm::Mode::ReadWrite &&
          Vm::IsManagedAddress(output_mapping));
    {
        // HLE final-output publication occurs inside the admitted Core
        // transaction. Taking a second native pin here would be invalid.
        Vm::GuestDataLease output;
        CHECK(Vm::TryAcquireGuestDataLease(output_mapping, sizeof(uint64_t),
                                          Vm::Mode::Write, &output));
        CHECK(g_fex_guest_data_leases == 0 && g_native_data_token == 0);
        const uint64_t result = handle.base;
        std::memcpy(reinterpret_cast<void*>(output.HostBegin()), &result, sizeof(result));
        Vm::GuestDataLease hole;
        CHECK(!Vm::TryAcquireGuestDataLease(handle.base, 1, Vm::Mode::Write, &hole));
    }
    CHECK(*reinterpret_cast<const uint64_t*>(output_mapping) == handle.base);
    CHECK(Vm::QueryManagedRegion(handle.base, &region) ==
              Vm::ManagedRegionState::Reserved &&
          region.begin == handle.base &&
          region.end == handle.base + handle.size &&
          !Vm::IsManagedAddress(handle.base));
    const Vm::ReservationHandle aborted = handle;
    CHECK(transaction.Abort() && !transaction.IsActive() &&
          g_fex_active.load(std::memory_order_acquire) == 0 &&
          Vm::GetMutationGeneration() == before_abort + 2 &&
          Vm::QueryManagedRegion(aborted.base, &region) ==
              Vm::ManagedRegionState::Unmanaged &&
          !Vm::ReleaseReservation(aborted));

    const uint64_t commits_before =
        g_fex_commits.load(std::memory_order_relaxed);
    CHECK(Vm::BeginReserveAligned(aborted.base, aborted.size, alignment,
                                  &handle, &transaction));
    CHECK(handle.base == aborted.base && handle.identity != aborted.identity);
    transaction.Commit();
    CHECK(!transaction.IsActive() &&
          g_fex_active.load(std::memory_order_acquire) == 0 &&
          g_fex_commits.load(std::memory_order_relaxed) == commits_before + 1 &&
          g_fex_last_base.load(std::memory_order_relaxed) == handle.base &&
          g_fex_last_size.load(std::memory_order_relaxed) == handle.size &&
          Vm::QueryManagedRegion(handle.base, &region) ==
              Vm::ManagedRegionState::Reserved &&
          region.begin == handle.base &&
          region.end == handle.base + handle.size &&
          !Vm::IsManagedAddress(handle.base) &&
          Vm::HasReservationOverlap(handle.base, handle.size) &&
          !Vm::HasReservationOverlap(handle.base + handle.size, host_page));

    Vm::ReservationHandle exact_collision {1, 1, 1};
    Vm::MutationTransaction exact_collision_transaction;
    const uint64_t before_exact_collision = Vm::GetMutationGeneration();
    CHECK(!Vm::BeginReserveAligned(
        handle.base, handle.size, alignment, &exact_collision,
        &exact_collision_transaction, Vm::ReservationPlacement::Exact));
    CHECK(!exact_collision.IsValid() &&
          !exact_collision_transaction.IsActive() &&
          Vm::GetMutationGeneration() == before_exact_collision);

    Vm::ReservationHandle relocated {};
    Vm::MutationTransaction relocate_transaction;
    CHECK(Vm::BeginReserveAligned(
        handle.base, handle.size, alignment, &relocated,
        &relocate_transaction, Vm::ReservationPlacement::PreferAddress));
    CHECK(relocated.IsValid() && relocated.base != handle.base &&
          relocated.base >= kGuestAnywhereMin &&
          relocated.base % alignment == 0);
    relocate_transaction.Commit();
    CHECK(Vm::ReleaseReservation(relocated));

    Vm::MutationTransaction outside_transaction;
    CHECK(Vm::BeginAllocFixedInReservation(
              handle.base + handle.size, host_page, Vm::Mode::ReadWrite,
              &outside_transaction) == Vm::ReservedFixedResult::NotReserved &&
          !outside_transaction.IsActive());

    const uint64_t denied_child = handle.base + 3 * host_page;
    const uint64_t before_denied_child = Vm::GetMutationGeneration();
    Vm::MutationTransaction denied_child_transaction;
    g_denied_fex_mutation.store(0, std::memory_order_release);
    CHECK(Vm::BeginAllocFixedInReservation(
              denied_child, host_page, Vm::Mode::ReadWrite,
              &denied_child_transaction) == Vm::ReservedFixedResult::Failed &&
          !denied_child_transaction.IsActive() &&
          g_fex_last_begin_kind.load(std::memory_order_relaxed) == 0 &&
          Vm::GetMutationGeneration() == before_denied_child &&
          Vm::QueryManagedRegion(denied_child, &region) ==
              Vm::ManagedRegionState::Reserved &&
          PhysicalProtection(denied_child) == PROT_NONE);

    const uint64_t failed_activation = handle.base + 3 * host_page;
    const uint64_t before_failed_activation = Vm::GetMutationGeneration();
    Vm::MutationTransaction failed_activation_transaction;
    g_partial_protect_failure = {
        reinterpret_cast<void*>(failed_activation), 2 * host_page,
        host_page, true, false};
    CHECK(Vm::BeginAllocFixedInReservation(
              failed_activation, 2 * host_page, Vm::Mode::ReadWrite,
              &failed_activation_transaction) ==
              Vm::ReservedFixedResult::Failed);
    g_partial_protect_failure.armed = false;
    CHECK(g_partial_protect_failure.fired &&
          !failed_activation_transaction.IsActive() &&
          Vm::GetMutationGeneration() == before_failed_activation &&
          Vm::QueryManagedRegion(failed_activation, &region) ==
              Vm::ManagedRegionState::Reserved &&
          PhysicalProtection(failed_activation) == PROT_NONE &&
          PhysicalProtection(failed_activation + host_page) == PROT_NONE);

    const uint64_t child = handle.base + host_page;
    const uint64_t child_size = 2 * host_page;
    const uint64_t before_child = Vm::GetMutationGeneration();
    Vm::MutationTransaction child_transaction;
    CHECK(Vm::BeginAllocFixedInReservation(
              child, child_size, Vm::Mode::ReadWrite, &child_transaction) ==
          Vm::ReservedFixedResult::Begun);
    CHECK(child_transaction.IsActive() &&
          g_fex_last_begin_kind.load(std::memory_order_relaxed) == 0 &&
          g_fex_active.load(std::memory_order_acquire) == 1 &&
          Vm::GetMutationGeneration() == before_child + 1 &&
          PhysicalProtection(child) == (PROT_READ | PROT_WRITE));
    *reinterpret_cast<volatile uint8_t*>(child) = 0x61;
    *reinterpret_cast<volatile uint8_t*>(child + host_page) = 0x62;
    child_transaction.Commit();
    CHECK(g_fex_active.load(std::memory_order_acquire) == 0 &&
          Vm::QueryManagedRegion(child, &region) ==
              Vm::ManagedRegionState::Visible &&
          region.begin == child && region.end == child + child_size &&
          region.mode == Vm::Mode::ReadWrite && Vm::IsManagedAddress(child));

    CHECK(Vm::QueryManagedRegion(handle.base, &region) ==
              Vm::ManagedRegionState::Reserved &&
          region.begin == handle.base && region.end == child);
    const uint64_t before_overlap = Vm::GetMutationGeneration();
    Vm::MutationTransaction overlap_transaction;
    CHECK(Vm::BeginAllocFixedInReservation(
              child + host_page, host_page, Vm::Mode::ReadWrite,
              &overlap_transaction) == Vm::ReservedFixedResult::Failed &&
          !overlap_transaction.IsActive());
    CHECK(Vm::BeginAllocFixedInReservation(
              handle.base, 2 * host_page, Vm::Mode::ReadWrite,
              &overlap_transaction) == Vm::ReservedFixedResult::Failed &&
          !overlap_transaction.IsActive() &&
          Vm::GetMutationGeneration() == before_overlap);

    if (host_page > kGuestPage)
    {
        auto old = Vm::Mode::ExecuteWrite;
        const uint64_t before_mixed = Vm::GetMutationGeneration();
        CHECK(!Vm::Protect(child + 3, 1, Vm::Mode::Read, &old));
        CHECK(old == Vm::Mode::ExecuteWrite &&
              Vm::GetMutationGeneration() == before_mixed &&
              IsMode(child, Vm::Mode::ReadWrite) &&
              PhysicalProtection(child) == (PROT_READ | PROT_WRITE));
    }

    auto old = Vm::Mode::ExecuteWrite;
    CHECK(Vm::Protect(child, host_page, Vm::Mode::Read, &old));
    CHECK(old == Vm::Mode::ReadWrite && PhysicalProtection(child) == PROT_READ &&
          PhysicalProtection(child + host_page) == (PROT_READ | PROT_WRITE));
    CHECK(Vm::Protect(child, host_page, Vm::Mode::ExecuteReadWrite, &old));
    CHECK(old == Vm::Mode::Read &&
          PhysicalProtection(child) == (PROT_READ | PROT_WRITE) &&
          (PhysicalProtection(child) & PROT_EXEC) == 0 &&
          IsMode(child, Vm::Mode::ExecuteReadWrite));
    CHECK(Vm::Protect(child, host_page, Vm::Mode::Read, &old));
    CHECK(Vm::Protect(child + host_page, host_page, Vm::Mode::NoAccess, &old));

    const uint64_t before_partial = Vm::GetMutationGeneration();
    old = Vm::Mode::ExecuteWrite;
    g_partial_protect_failure = {
        reinterpret_cast<void*>(child), 2 * host_page, host_page, true, false};
    const bool partial_result =
        Vm::Protect(child, 2 * host_page, Vm::Mode::ReadWrite, &old);
    g_partial_protect_failure.armed = false;
    CHECK(g_partial_protect_failure.fired && !partial_result &&
          old == Vm::Mode::ExecuteWrite &&
          Vm::GetMutationGeneration() == before_partial &&
          IsMode(child, Vm::Mode::Read) &&
          IsMode(child + host_page, Vm::Mode::NoAccess) &&
          PhysicalProtection(child) == PROT_READ &&
          PhysicalProtection(child + host_page) == PROT_NONE);
    CHECK(Vm::Protect(child, 2 * host_page, Vm::Mode::ReadWrite));

    const uint64_t aborted_child = handle.base + 4 * host_page;
    const uint64_t before_child_abort = Vm::GetMutationGeneration();
    {
        Vm::MutationTransaction abort_child_transaction;
        CHECK(Vm::BeginAllocFixedInReservation(
                  aborted_child, host_page, Vm::Mode::ReadWrite,
                  &abort_child_transaction) == Vm::ReservedFixedResult::Begun);
        *reinterpret_cast<volatile uint8_t*>(aborted_child) = 0x7b;
        CHECK(abort_child_transaction.Abort());
    }
    CHECK(Vm::GetMutationGeneration() == before_child_abort + 2 &&
          Vm::QueryManagedRegion(aborted_child, &region) ==
              Vm::ManagedRegionState::Reserved &&
          !Vm::IsManagedAddress(aborted_child) &&
          AccessDenied(aborted_child, false));

    const uint64_t abandoned_child = handle.base + 5 * host_page;
    const uint64_t before_abandoned_child = Vm::GetMutationGeneration();
    {
        Vm::MutationTransaction abandoned_transaction;
        CHECK(Vm::BeginAllocFixedInReservation(
                  abandoned_child, host_page, Vm::Mode::ReadWrite,
                  &abandoned_transaction) == Vm::ReservedFixedResult::Begun);
    }
    CHECK(Vm::GetMutationGeneration() == before_abandoned_child + 2 &&
          Vm::QueryManagedRegion(abandoned_child, &region) ==
              Vm::ManagedRegionState::Reserved &&
          g_fex_active.load(std::memory_order_acquire) == 0);

    *reinterpret_cast<volatile uint8_t*>(child) = 0x33;
    const uint64_t before_decommit = Vm::GetMutationGeneration();
    g_partial_protect_failure = {
        reinterpret_cast<void*>(child), child_size, host_page, true, false};
    CHECK(!Vm::Free(child));
    g_partial_protect_failure.armed = false;
    CHECK(g_partial_protect_failure.fired &&
          Vm::GetMutationGeneration() == before_decommit &&
          Vm::QueryManagedRegion(child, &region) ==
              Vm::ManagedRegionState::Visible &&
          region.begin == child && region.end == child + child_size &&
          region.mode == Vm::Mode::ReadWrite &&
          PhysicalProtection(child) == (PROT_READ | PROT_WRITE) &&
          PhysicalProtection(child + host_page) == (PROT_READ | PROT_WRITE) &&
          *reinterpret_cast<volatile uint8_t*>(child) == 0x33);
    CHECK(Vm::Free(child) && Vm::GetMutationGeneration() == before_decommit + 1 &&
          Vm::QueryManagedRegion(child, &region) ==
              Vm::ManagedRegionState::Reserved &&
          !Vm::IsManagedAddress(child) && AccessDenied(child, false));
    CHECK(Vm::AllocFixed(child, child_size, Vm::Mode::ReadWrite));
    CHECK(*reinterpret_cast<volatile uint8_t*>(child) == 0 &&
          *reinterpret_cast<volatile uint8_t*>(child + host_page) == 0);
    CHECK(Vm::Free(child));

    std::atomic<bool> race_start {false};
    std::atomic<uint32_t> race_winners {0};
    std::atomic<uint32_t> race_unexpected {0};
    const auto race_activate = [&]
    {
        while (!race_start.load(std::memory_order_acquire))
            std::this_thread::yield();
        Vm::MutationTransaction race_transaction;
        const auto result = Vm::BeginAllocFixedInReservation(
            child, child_size, Vm::Mode::ReadWrite, &race_transaction);
        if (result == Vm::ReservedFixedResult::Begun)
        {
            race_winners.fetch_add(1, std::memory_order_relaxed);
            race_transaction.Commit();
        } else if (result != Vm::ReservedFixedResult::Failed)
        {
            race_unexpected.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread first_racer(race_activate);
    std::thread second_racer(race_activate);
    race_start.store(true, std::memory_order_release);
    first_racer.join();
    second_racer.join();
    CHECK(race_winners.load(std::memory_order_relaxed) == 1 &&
          race_unexpected.load(std::memory_order_relaxed) == 0 &&
          g_fex_active.load(std::memory_order_acquire) == 0 &&
          IsMode(child, Vm::Mode::ReadWrite));
    CHECK(Vm::Free(child));

    const uint64_t released_child = handle.base + 6 * host_page;
    CHECK(Vm::AllocFixed(released_child, host_page,
                         Vm::Mode::ExecuteReadWrite));
    CHECK(IsMode(released_child, Vm::Mode::ExecuteReadWrite) &&
          PhysicalProtection(released_child) == (PROT_READ | PROT_WRITE) &&
          (PhysicalProtection(released_child) & PROT_EXEC) == 0);

    const uint64_t before_release_denial = Vm::GetMutationGeneration();
    g_denied_fex_mutation.store(0, std::memory_order_release);
    CHECK(!Vm::ReleaseReservation(handle) &&
          Vm::GetMutationGeneration() == before_release_denial &&
          Vm::QueryManagedRegion(released_child, &region) ==
              Vm::ManagedRegionState::Visible);
    const Vm::ReservationHandle stale = handle;
    CHECK(Vm::ReleaseReservation(handle) &&
          Vm::QueryManagedRegion(stale.base, &region) ==
              Vm::ManagedRegionState::Unmanaged &&
          !Vm::IsManagedAddress(stale.base));

    Vm::ReservationHandle replacement {};
    Vm::MutationTransaction replacement_transaction;
    CHECK(Vm::BeginReserveAligned(stale.base, stale.size, alignment,
                                  &replacement, &replacement_transaction));
    replacement_transaction.Commit();
    CHECK(replacement.base == stale.base && replacement.identity != stale.identity &&
          !Vm::ReleaseReservation(stale) &&
          Vm::ReleaseReservation(replacement));
    CHECK(g_fex_active.load(std::memory_order_acquire) == 0 &&
          g_fex_begins.load(std::memory_order_relaxed) ==
              g_fex_closes.load(std::memory_order_relaxed));
    return Vm::Free(output_mapping);
}

bool PatchAcrossPhysicalPages(uint64_t host_page)
{
    const uint64_t mapping = Vm::Alloc(0, 2 * host_page, Vm::Mode::ReadWrite);
    CHECK(mapping != 0);
    std::memset(reinterpret_cast<void*>(mapping), 0, 2 * host_page);
    CHECK(Vm::Protect(mapping, 2 * host_page, Vm::Mode::Read));
    CHECK(Vm::Protect(mapping, kGuestPage, Vm::Mode::ExecuteRead));
    constexpr uint64_t value = UINT64_C(0x1122334455667788);
    g_partial_protect_failure = {
        reinterpret_cast<void*>(mapping), 2 * host_page, host_page, true, false};
    const bool failed_patch = Vm::PatchReplace(mapping + host_page - 4, value);
    g_partial_protect_failure.armed = false;
    CHECK(g_partial_protect_failure.fired && !failed_patch);
    uint64_t actual = value;
    std::memcpy(&actual, reinterpret_cast<const void*>(mapping + host_page - 4), sizeof(actual));
    CHECK(actual == 0 && PhysicalProtection(mapping) == PROT_READ &&
          PhysicalProtection(mapping + host_page) == PROT_READ);
    CHECK(Vm::PatchReplace(mapping + host_page - 4, value));
    std::memcpy(&actual, reinterpret_cast<const void*>(mapping + host_page - 4), sizeof(actual));
    CHECK(actual == value);
    CHECK(!Vm::PatchReplace(mapping + host_page - 4, value));
    CHECK(Vm::PatchReplace(mapping + kGuestPage + 8, ~value));
    for (uint64_t offset = 0; offset < 2 * host_page; offset += kGuestPage)
    {
        CHECK(PhysicalProtection(mapping + offset) == PROT_READ);
        CHECK(IsMode(mapping + offset, offset == 0 ? Vm::Mode::ExecuteRead : Vm::Mode::Read));
    }
    CHECK(!Vm::PatchReplace(mapping + 2 * host_page - 4, value));
    return Vm::Free(mapping);
}

bool AllocationFailureContract(uint64_t host_page)
{
    using Failure = Vm::AllocationFailure;
    CHECK(!g_fex_host_reader && g_fex_guest_data_leases == 0 && g_fex_mutation_depth == 0);
    Vm::AllocationDenialMetadata denial {Failure::AdmissionDenied, true};
    const uint64_t address = Vm::Alloc(0, host_page, Vm::Mode::ReadWrite, &denial);
    CHECK(address != 0 && denial.reason == Failure::None && !denial.retryable && Vm::Free(address));
    const uint64_t generation = Vm::GetMutationGeneration();
    const uint64_t begins = g_fex_begins.load();
    for (const auto request: {std::array<uint64_t, 3>{0, 0, 3}, {UINT64_MAX, host_page, 3}, {0, host_page, 8}})
    {
        denial = {Failure::AdmissionDenied, true};
        CHECK(Vm::Alloc(request[0], request[1], static_cast<Vm::Mode>(request[2]), &denial) == 0 &&
              denial.reason == Failure::InvalidArgument && !denial.retryable);
    }
    CHECK(g_fex_begins.load() == begins && Vm::GetMutationGeneration() == generation);
    // Only a modeled top-level HLE admission conflict grants retry. Both
    // declines leave the physical mapping/registry and mutation epoch intact.
    g_fex_host_reader = true;
    for (const bool eligible: {false, true})
    {
        g_fex_retryable_conflict = eligible;
        g_denied_fex_mutation.store(1);
        CHECK(Vm::Alloc(address, host_page, Vm::Mode::ReadWrite, &denial) == 0 &&
              denial.reason == Failure::AdmissionDenied && denial.retryable == eligible);
        CHECK(!Vm::IsManagedAddress(address) && g_fex_begins.load() == begins &&
              Vm::GetMutationGeneration() == generation && g_fex_active.load() == 0);
    }
    // Model the caller's completed host-wait/readmission before its next
    // attempt; Core itself never sleeps or retries the mapping operation.
    g_fex_host_reader = false;
    g_fex_retryable_conflict = false;
    g_fex_host_reader = true;
    const uint64_t recovered = Vm::Alloc(address, host_page, Vm::Mode::ReadWrite, &denial);
    CHECK(recovered != 0 && denial.reason == Failure::None && !denial.retryable &&
          Vm::GetMutationGeneration() == generation + 1);
    {
        Vm::GuestDataLease lease;
        CHECK(Vm::TryAcquireGuestDataLease(recovered, host_page, Vm::Mode::Read, &lease));
        CHECK(Vm::Alloc(0, host_page, Vm::Mode::ReadWrite, &denial) == 0 &&
              denial.reason == Failure::AdmissionDenied && !denial.retryable);
    }
    // This representable request exceeds every guest allocation window.
    // It proves an allocator API failure, not physical-memory exhaustion.
    CHECK(Vm::Alloc(0, UINT64_C(1) << 44, Vm::Mode::ReadWrite, &denial) == 0 &&
          denial.reason == Failure::HostAllocationFailed && !denial.retryable);
    g_fex_host_reader = false;
    CHECK(Vm::Free(recovered));
    return true;
}

bool InvalidRequests(uint64_t host_page)
{
    const uint64_t mapping = Vm::Alloc(0, host_page, Vm::Mode::ReadWrite);
    CHECK(mapping != 0);
    const uint64_t generation = Vm::GetMutationGeneration();
    for (auto invalid : {UINT32_C(8), UINT32_C(0x100), UINT32_MAX})
    {
        const auto mode = static_cast<Vm::Mode>(invalid);
        auto old = Vm::Mode::ExecuteWrite;
        CHECK(Vm::Alloc(0, host_page, mode) == 0);
        CHECK(Vm::AllocAligned(0, host_page, mode, host_page) == 0);
        CHECK(!Vm::AllocFixed(mapping, host_page, mode));
        CHECK(!Vm::Protect(mapping, host_page, mode, &old));
        CHECK(!Vm::Protect(mapping, 0, mode, &old));
        CHECK(old == Vm::Mode::ExecuteWrite);
    }
    CHECK(Vm::Alloc(0, 0, Vm::Mode::ReadWrite) == 0);
    CHECK(Vm::Alloc(0, UINT64_MAX, Vm::Mode::ReadWrite) == 0);
    CHECK(Vm::AllocAligned(0, host_page, Vm::Mode::ReadWrite, 3) == 0);
    CHECK(!Vm::Protect(mapping, 0, Vm::Mode::Read));
    CHECK(!Vm::Protect(UINT64_MAX - 3, 8, Vm::Mode::Read));
    CHECK(!Vm::Protect(mapping, 2 * host_page, Vm::Mode::Read));
    CHECK(generation == Vm::GetMutationGeneration());
    CHECK(PhysicalProtection(mapping) == (PROT_READ | PROT_WRITE));
    return Vm::Free(mapping);
}

bool GuestDataLeaseContract(uint64_t host_page)
{
    CHECK(!g_fex_host_reader && g_fex_guest_data_leases == 0 &&
          g_fex_mutation_depth == 0);
    const uint64_t retains_before = g_fex_data_retains.load(std::memory_order_relaxed);
    const uint64_t releases_before = g_fex_data_releases.load(std::memory_order_relaxed);
    const uint64_t mapping = Vm::Alloc(0, 3 * host_page, Vm::Mode::ReadWrite);
    const uint64_t small = Vm::Alloc(0, 1, Vm::Mode::ReadWrite);
    CHECK(mapping != 0 && small != 0);
    std::memset(reinterpret_cast<void*>(mapping), 0x4c, 3 * host_page);
    *reinterpret_cast<uint8_t*>(small) = 0x6d;
    Vm::GuestDataLeaseDenialMetadata denial {};
    denial.reason = Vm::GuestDataLeaseDenialReason::PhysicalQueryOrRightsDenied;
    Vm::GuestDataLease absent_reader;
    CHECK(!Vm::TryAcquireGuestDataLease(mapping, 8, Vm::Mode::Read,
                                       &absent_reader, false, &denial));
    CHECK(!absent_reader.IsActive() && g_fex_guest_data_leases == 0 &&
          denial.reason == Vm::GuestDataLeaseDenialReason::MappingLeaseCapabilityUnavailable &&
          denial.address == mapping && denial.bytes == 8 &&
          denial.required == Vm::Mode::Read);

    struct HostReaderScope
    {
        HostReaderScope() { g_fex_host_reader = true; }
        ~HostReaderScope()
        {
            if (g_fex_guest_data_leases != 0) std::abort();
            g_fex_host_reader = false;
        }
    } reader;
    const auto reject = [&](uint64_t address, uint64_t bytes, Vm::Mode mode)
    {
        const uint32_t retained = g_fex_guest_data_leases;
        Vm::GuestDataLease lease;
        Vm::GuestDataLeaseDenialMetadata metadata {};
        metadata.reason = Vm::GuestDataLeaseDenialReason::PhysicalQueryOrRightsDenied;
        return !Vm::TryAcquireGuestDataLease(address, bytes, mode, &lease, false, &metadata) &&
            !lease.IsActive() && g_fex_guest_data_leases == retained &&
            metadata.reason != Vm::GuestDataLeaseDenialReason::None &&
            metadata.address == address && metadata.bytes == bytes && metadata.required == mode;
    };

    for (const auto mode: {Vm::Mode::Read, Vm::Mode::Write, Vm::Mode::ReadWrite})
    {
        Vm::GuestDataLease lease;
        denial.reason = Vm::GuestDataLeaseDenialReason::PhysicalQueryOrRightsDenied;
        CHECK(Vm::TryAcquireGuestDataLease(mapping + 3, 48, mode, &lease, false, &denial));
        CHECK(lease.IsActive() && lease.HostBegin() == mapping + 3 &&
              lease.ByteCount() == 48 &&
              lease.MutationGeneration() == Vm::GetMutationGeneration() &&
              denial.reason == Vm::GuestDataLeaseDenialReason::None &&
              g_fex_guest_data_leases == 1);
        if (mode != Vm::Mode::Read)
            std::memset(reinterpret_cast<void*>(lease.HostBegin()), 0x5e, 48);
        CHECK(*reinterpret_cast<uint8_t*>(mapping + 2) == 0x4c &&
              *reinterpret_cast<uint8_t*>(mapping + 51) == 0x4c);
        Vm::ReleaseGuestDataLease(&lease);
        CHECK(!lease.IsActive() && lease.HostBegin() == 0 && lease.ByteCount() == 0 &&
              lease.MutationGeneration() == 0 && g_fex_guest_data_leases == 0);
        Vm::ReleaseGuestDataLease(&lease);
        CHECK(g_fex_guest_data_leases == 0);
    }
    {
        Vm::GuestDataLease outer;
        Vm::GuestDataLease inner;
        CHECK(Vm::TryAcquireGuestDataLease(mapping, 8, Vm::Mode::Read, &outer));
        CHECK(Vm::TryAcquireGuestDataLease(mapping + host_page - 4,
                                          host_page + 8, Vm::Mode::ReadWrite, &inner));
        CHECK(g_fex_guest_data_leases == 2 && inner.ByteCount() == host_page + 8);
        CHECK(!Vm::TryAcquireGuestDataLease(mapping + 8, 8, Vm::Mode::Read,
                                           &outer, false, &denial));
        CHECK(outer.HostBegin() == mapping && outer.ByteCount() == 8 &&
              g_fex_guest_data_leases == 2);
        const uint64_t generation = Vm::GetMutationGeneration();
        CHECK(!Vm::Protect(mapping, host_page, Vm::Mode::NoAccess) &&
              !Vm::Free(mapping) && Vm::Alloc(0, host_page, Vm::Mode::ReadWrite) == 0);
        CHECK(Vm::GetMutationGeneration() == generation && IsMode(mapping, Vm::Mode::ReadWrite) &&
              PhysicalProtection(mapping) == (PROT_READ | PROT_WRITE));
    }
    CHECK(g_fex_guest_data_leases == 0);
    CHECK(reject(0, 8, Vm::Mode::Read) && reject(mapping, 0, Vm::Mode::Read) &&
          reject(UINT64_MAX - 3, 8, Vm::Mode::Read) &&
          reject(mapping, UINT64_MAX, Vm::Mode::Read));
    for (const auto mode: {Vm::Mode::NoAccess, Vm::Mode::Execute, Vm::Mode::ExecuteRead,
                           static_cast<Vm::Mode>(UINT32_MAX)})
        CHECK(reject(mapping, 8, mode));
    CHECK(!Vm::TryAcquireGuestDataLease(mapping, 8, Vm::Mode::Read, nullptr, false, &denial));
    CHECK(g_fex_guest_data_leases == 0);

    std::atomic<bool> foreign_denied {false};
    std::thread foreign([&]
    {
        Vm::GuestDataLease lease;
        foreign_denied.store(!Vm::TryAcquireGuestDataLease(mapping, 8, Vm::Mode::Read, &lease) &&
                             !lease.IsActive() && g_fex_guest_data_leases == 0,
                             std::memory_order_release);
    });
    foreign.join();
    CHECK(foreign_denied.load(std::memory_order_acquire));

    // x86 data reads are allowed by every non-NoAccess guest mode, including
    // Write-only and Execute-only metadata. Native ARM execute remains off.
    for (const auto mode: {Vm::Mode::Write, Vm::Mode::Execute, Vm::Mode::ExecuteWrite})
    {
        CHECK(Vm::Protect(mapping, host_page, mode));
        Vm::GuestDataLease lease;
        CHECK(Vm::TryAcquireGuestDataLease(mapping + 17, 48, Vm::Mode::Read, &lease));
        CHECK((PhysicalProtection(mapping) & PROT_EXEC) == 0);
    }
    CHECK(Vm::Protect(mapping, host_page, Vm::Mode::Execute));
    CHECK(reject(mapping + 17, 48, Vm::Mode::Write));
    CHECK(Vm::Protect(mapping, host_page, Vm::Mode::Read));
    CHECK(reject(mapping + 17, 48, Vm::Mode::Write));
    {
        Vm::GuestDataLease lease;
        CHECK(Vm::TryAcquireGuestDataLease(mapping + 17, 48, Vm::Mode::Read, &lease));
    }
    CHECK(Vm::Protect(mapping + host_page, host_page, Vm::Mode::NoAccess));
    CHECK(reject(mapping + host_page, 8, Vm::Mode::Read) &&
          reject(mapping + host_page - 4, 8, Vm::Mode::Read));
    {
        Vm::GuestDataLease lease;
        CHECK(Vm::TryAcquireGuestDataLease(mapping + host_page - 48, 48,
                                          Vm::Mode::Read, &lease));
    }
    CHECK(Vm::Protect(mapping, 2 * host_page, Vm::Mode::ReadWrite));
    {
        Vm::GuestDataLease lease;
        CHECK(Vm::TryAcquireGuestDataLease(small, 1, Vm::Mode::ReadWrite, &lease));
    }
    CHECK(reject(small, 2, Vm::Mode::Read) && reject(small + 1, 1, Vm::Mode::Read));

    // Native storage is owned by this synchronous caller. Tagged addresses
    // must retain their tag for the eventual ARM access, while permissions use
    // the untagged kernel address. Do not dereference a tag on the test host.
    std::array<uint8_t, 96> native {};
    const uint64_t native_address = reinterpret_cast<uint64_t>(native.data());
    CHECK((native_address & UINT64_C(0xff00000000000000)) == 0);
    const uint64_t tagged = native_address | UINT64_C(0xa500000000000000);
    {
        Vm::GuestDataLease native_lease;
        Vm::GuestDataLease tagged_lease;
        CHECK(Vm::TryAcquireGuestDataLease(native_address + 1, 76,
                                          Vm::Mode::ReadWrite, &native_lease));
        CHECK(Vm::TryAcquireGuestDataLease(tagged + 1, 76,
                                          Vm::Mode::ReadWrite, &tagged_lease));
        CHECK(native_lease.HostBegin() == native_address + 1 &&
              tagged_lease.HostBegin() == tagged + 1 && g_fex_guest_data_leases == 2);
    }
    void* const physical = mmap(nullptr, 2 * host_page, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(physical != MAP_FAILED);
    const uint64_t physical_address = reinterpret_cast<uint64_t>(physical);
    CHECK(mprotect(physical, host_page, PROT_READ) == 0);
    CHECK(mprotect(reinterpret_cast<void*>(physical_address + host_page), host_page, PROT_NONE) == 0);
    CHECK(reject(physical_address, 8, Vm::Mode::Write) &&
          reject(physical_address + host_page, 8, Vm::Mode::Read) &&
          reject(physical_address + host_page - 4, 8, Vm::Mode::Read));
    CHECK(munmap(physical, 2 * host_page) == 0);

    Vm::ReservationHandle reservation {};
    Vm::MutationTransaction reserve;
    CHECK(Vm::BeginReserveAligned(0, 4 * host_page, host_page, &reservation, &reserve));
    reserve.Commit();
    CHECK(Vm::AllocFixed(reservation.base, host_page, Vm::Mode::ReadWrite));
    CHECK(Vm::AllocFixed(reservation.base + host_page, host_page, Vm::Mode::ReadWrite));
    CHECK(Vm::AllocFixed(reservation.base + 3 * host_page, host_page, Vm::Mode::ReadWrite));
    {
        Vm::GuestDataLease adjacent;
        CHECK(Vm::TryAcquireGuestDataLease(reservation.base + host_page - 4, 8,
                                          Vm::Mode::ReadWrite, &adjacent));
    }
    const uint64_t hole = reservation.base + 2 * host_page;
    // Deliberately create an inconsistent physical view: a writable kernel VMA
    // must never override the logical reserved hole or its crossing ranges.
    CHECK(mprotect(reinterpret_cast<void*>(hole), host_page, PROT_READ | PROT_WRITE) == 0);
    CHECK(reject(hole, 8, Vm::Mode::Read) &&
          reject(hole - 4, host_page + 8, Vm::Mode::ReadWrite));
    CHECK(mprotect(reinterpret_cast<void*>(hole), host_page, PROT_NONE) == 0);
    CHECK(Vm::ReleaseReservation(reservation));
    CHECK(reject(reservation.base, 8, Vm::Mode::Read));
    CHECK(Vm::Free(mapping) && Vm::Free(small));
    CHECK(g_fex_guest_data_leases == 0 &&
          g_fex_data_retains.load(std::memory_order_relaxed) - retains_before ==
              g_fex_data_releases.load(std::memory_order_relaxed) - releases_before);
    return true;
}

bool GuestDataReadContract(uint64_t host_page)
{
    CHECK(!g_fex_host_reader && g_fex_guest_data_leases == 0 && g_native_data_token == 0 &&
          !g_native_data_admission.load(std::memory_order_acquire));
    const uint64_t mapping = Vm::Alloc(0, 2 * host_page, Vm::Mode::ReadWrite);
    const uint64_t small = Vm::Alloc(0, 8, Vm::Mode::ReadWrite);
    CHECK(mapping != 0 && small != 0);
    std::memset(reinterpret_cast<void*>(mapping), 0x69, 2 * host_page);
    std::memset(reinterpret_cast<void*>(small), 0x51, 8);
    std::array<uint8_t, 257> native;
    native.fill(0x37);
    const uint64_t native_address = reinterpret_cast<uint64_t>(native.data());
    CHECK((native_address & UINT64_C(0xff00000000000000)) == 0);
    std::array<uint8_t, 257> destination;
    destination.fill(0xa3);
    const auto untouched = destination;
    const auto calls = [] { return g_copied_read_calls.load(std::memory_order_relaxed); };
    const auto reject = [&](uint64_t address, uint64_t size)
    {
        destination = untouched;
        return !Vm::TryReadGuestData(address, destination.data(), size) && destination == untouched;
    };
    uint64_t before = calls();
    CHECK(reject(mapping, 8) && reject(native_address, 8) && calls() == before);

    const uint64_t retains = g_fex_data_retains.load(std::memory_order_relaxed);
    const uint64_t releases = g_fex_data_releases.load(std::memory_order_relaxed);
    {
        struct HostReaderScope
        {
            HostReaderScope() { g_fex_host_reader = true; }
            ~HostReaderScope()
            {
                if (g_fex_guest_data_leases != 0) std::abort();
                g_fex_host_reader = false;
            }
        } reader;
        CHECK(Vm::TryReadGuestData(mapping + 3, destination.data(), 256) && calls() == before + 1);
        for (size_t i = 0; i < 256; ++i) CHECK(destination[i] == 0x69);
        CHECK(destination[256] == untouched[256] && g_fex_guest_data_leases == 0);
        CHECK(Vm::TryReadGuestData(native_address, destination.data(), 8) && calls() == before + 2);
        CHECK(std::memcmp(destination.data(), native.data(), 8) == 0);

        // Larger reads retain the normal lease, with no kernel-copy dispatch.
        before = calls();
        CHECK(Vm::TryReadGuestData(native_address, destination.data(), native.size()) &&
              destination == native && calls() == before);
        CHECK(reject(0, 8) && reject(mapping, 0) && reject(UINT64_MAX - 3, 8) &&
              reject(mapping, UINT64_MAX) &&
              reject(UINT64_C(0x00fffffffffffffc), 8) &&
              !Vm::TryReadGuestData(mapping, nullptr, 8) && calls() == before);

        // Being in a host call is insufficient when retaining its capability
        // fails. Native admission must not turn that failure into a read.
        g_fex_guest_data_leases = UINT32_MAX;
        const bool negative_capability = reject(mapping, 8);
        g_fex_guest_data_leases = 0;
        CHECK(negative_capability && calls() == before);
        std::atomic<bool> foreign_denied {false};
        std::thread foreign([&]
        {
            uint64_t value = UINT64_C(0x123456789abcdef0);
            foreign_denied.store(!Vm::TryReadGuestData(mapping, &value, sizeof(value)) &&
                value == UINT64_C(0x123456789abcdef0), std::memory_order_release);
        });
        foreign.join();
        CHECK(foreign_denied.load(std::memory_order_acquire) && calls() == before);

        // Neither a denied syscall nor a short copy may retry via the valid
        // source pointer or publish the private bytes written by the fixture.
        g_copied_read_failure = CopiedReadFailure::Denied;
        CHECK(reject(mapping, 8) && calls() == before + 1 && g_fex_guest_data_leases == 0);
        g_copied_read_failure = CopiedReadFailure::Short;
        CHECK(reject(mapping, 8) && calls() == before + 2 && g_fex_guest_data_leases == 0);

        for (const auto mode: {Vm::Mode::Write, Vm::Mode::Execute, Vm::Mode::Read})
        {
            CHECK(Vm::Protect(mapping, host_page, mode));
            CHECK(Vm::TryReadGuestData(mapping + 7, destination.data(), 8) && destination[0] == 0x69);
        }
        CHECK(Vm::Protect(mapping, host_page, Vm::Mode::ReadWrite));
        CHECK(Vm::Protect(mapping + host_page, host_page, Vm::Mode::NoAccess));
        // Logical guards and allocation padding deny before the syscall even
        // when the corresponding physical page is readable.
        CHECK(mprotect(reinterpret_cast<void*>(mapping + host_page), host_page, PROT_READ) == 0);
        before = calls();
        CHECK(reject(mapping + host_page, 8) && reject(mapping + host_page - 4, 8) &&
              reject(small, 9) && reject(small + 8, 1) && calls() == before);
        CHECK(mprotect(reinterpret_cast<void*>(mapping + host_page), host_page, PROT_NONE) == 0);
        CHECK(Vm::Protect(mapping + host_page, host_page, Vm::Mode::ReadWrite));

        // Changes made outside Core must be checked live, without an epoch or
        // cached permissions. The kernel can return a prefix across a page.
        CHECK(mprotect(reinterpret_cast<void*>(mapping + host_page), host_page, PROT_NONE) == 0);
        before = calls();
        CHECK(reject(mapping + host_page - 4, 8) && calls() == before + 1);
        CHECK(reject(mapping + host_page, 8) && calls() == before + 2);
        // A tag stays on the ordinary lease route. Logical rights permit this
        // address, so only its physical query can reject it without readv.
        const uint64_t tagged_guard = (mapping + host_page) | UINT64_C(0xa500000000000000);
        const uint64_t tagged_retains = g_fex_data_retains.load(std::memory_order_relaxed);
        before = calls();
        CHECK(reject(tagged_guard, 8) && calls() == before &&
              g_fex_data_retains.load(std::memory_order_relaxed) == tagged_retains + 1);
        CHECK(mprotect(reinterpret_cast<void*>(mapping + host_page), host_page,
                       PROT_READ | PROT_WRITE) == 0);
        CHECK(Vm::TryReadGuestData(mapping + host_page - 4, destination.data(), 8));

        void* const physical = mmap(nullptr, 2 * host_page, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(physical != MAP_FAILED);
        const uint64_t physical_address = reinterpret_cast<uint64_t>(physical);
        CHECK(mprotect(reinterpret_cast<void*>(physical_address + host_page), host_page, PROT_NONE) == 0);
        before = calls();
        CHECK(reject(physical_address + host_page - 4, 8) && calls() == before + 1);
        CHECK(munmap(physical, 2 * host_page) == 0);

        Vm::ReservationHandle reservation;
        {
            Vm::MutationTransaction transaction;
            CHECK(Vm::BeginReserveAligned(0, host_page, host_page, &reservation, &transaction));
            transaction.Commit();
        }
        CHECK(mprotect(reinterpret_cast<void*>(reservation.base), host_page, PROT_READ) == 0);
        before = calls();
        CHECK(reject(reservation.base, 8) && calls() == before);
        CHECK(Vm::ReleaseReservation(reservation));
    }
    CHECK(g_fex_data_retains.load(std::memory_order_relaxed) - retains ==
          g_fex_data_releases.load(std::memory_order_relaxed) - releases);
    const uint64_t pins = g_native_data_pins.load(std::memory_order_relaxed);
    const uint64_t unpins = g_native_data_unpins.load(std::memory_order_relaxed);
    g_native_data_admission.store(true, std::memory_order_release);
    CHECK(Vm::TryReadGuestData(native_address, destination.data(), 8) &&
          std::memcmp(destination.data(), native.data(), 8) == 0 && g_native_data_token == 0 &&
          g_native_data_pins.load(std::memory_order_relaxed) == pins + 1 &&
          g_native_data_unpins.load(std::memory_order_relaxed) == unpins + 1);
    g_native_data_admission.store(false, std::memory_order_release);
    before = calls();
    CHECK(reject(native_address, 8) && calls() == before &&
          !g_copied_read_without_admission.load(std::memory_order_relaxed));
    return Vm::Free(mapping) && Vm::Free(small);
}

bool GuestDataPrivateScratchContract(uint64_t host_page)
{
    using Result = Vm::PrivateGuestDataCopyResult;
    CHECK(!g_fex_host_reader && g_fex_guest_data_leases == 0 && g_native_data_token == 0);
    constexpr size_t limit = Vm::PRIVATE_GUEST_DATA_COPY_LIMIT;
    static_assert(limit == 2 * 1024 * 1024);
    const uint64_t mapping = Vm::Alloc(0, limit, Vm::Mode::ReadWrite);
    const uint64_t small = Vm::Alloc(0, 8, Vm::Mode::ReadWrite);
    CHECK(mapping != 0 && small != 0 && 2 * host_page <= limit);
    std::memset(reinterpret_cast<void*>(mapping), 0x69, limit);
    std::unique_ptr<uint8_t[]> scratch(new uint8_t[limit]);
    std::memset(scratch.get(), 0xa3, limit);
    Vm::GuestDataLeaseDenialMetadata denial;
    const auto calls = [] { return g_copied_read_calls.load(std::memory_order_relaxed); };
    const auto copy = [&](uint64_t address, uint64_t bytes) {
        return Vm::CopyGuestDataToPrivateScratch(address, scratch.get(), bytes, &denial);
    };
    uint64_t before = calls();
    CHECK(copy(mapping, 1024) == Result::Denied && calls() == before && scratch[0] == 0xa3 &&
          denial.reason == Vm::GuestDataLeaseDenialReason::MappingLeaseCapabilityUnavailable);
    const uint64_t retains = g_fex_data_retains.load(std::memory_order_relaxed);
    const uint64_t releases = g_fex_data_releases.load(std::memory_order_relaxed);
    {
        struct HostReaderScope
        {
            HostReaderScope() { g_fex_host_reader = true; }
            ~HostReaderScope()
            {
                if (g_fex_guest_data_leases != 0) std::abort();
                g_fex_host_reader = false;
            }
        } reader;
        CHECK(copy(mapping + 3, 1024) == Result::Copied && calls() == before + 1 &&
              std::memcmp(scratch.get(), reinterpret_cast<const void*>(mapping + 3), 1024) == 0 &&
              scratch[1024] == 0xa3 && g_fex_guest_data_leases == 0);
        CHECK(copy(mapping, limit) == Result::Copied && calls() == before + 2 &&
              std::memcmp(scratch.get(), reinterpret_cast<const void*>(mapping), limit) == 0);
        // Nested full-copy admission must preserve the caller's prefix lease.
        {
            Vm::GuestDataLease prefix;
            CHECK(Vm::TryAcquireGuestDataLease(mapping, 8, Vm::Mode::Read, &prefix));
            CHECK(g_fex_guest_data_leases == 1);
            CHECK(copy(mapping, 1024) == Result::Copied && prefix.IsActive() &&
                  g_fex_guest_data_leases == 1);
        }
        before = calls();
        scratch[0] = 0xa3;
        CHECK(copy(mapping, limit + 1) == Result::Unsupported &&
              copy(mapping | UINT64_C(0xa500000000000000), 1024) == Result::Unsupported &&
              scratch[0] == 0xa3 && calls() == before && g_fex_guest_data_leases == 0);
        CHECK(copy(0, 1024) == Result::Denied && copy(mapping, 0) == Result::Denied &&
              copy(UINT64_MAX - 3, 1024) == Result::Denied &&
              copy(UINT64_C(0x00ffffffffffff00), 1024) == Result::Denied &&
              Vm::CopyGuestDataToPrivateScratch(mapping, nullptr, 1024) == Result::Denied && calls() == before);
        CHECK(copy(small, 1024) == Result::Denied && calls() == before &&
              denial.reason == Vm::GuestDataLeaseDenialReason::LogicalRegistryDenied);
        // The private API may overwrite output on failure. It returns Denied
        // exactly once, even though the original source remains fully readable.
        for (const auto failure: {CopiedReadFailure::Denied, CopiedReadFailure::Short})
        {
            g_copied_read_failure = failure;
            before = calls();
            CHECK(copy(mapping, 1024) == Result::Denied && calls() == before + 1 &&
                  scratch[0] == 0xc7 && scratch[1023] == 0xc7 && g_fex_guest_data_leases == 0 &&
                  denial.reason == Vm::GuestDataLeaseDenialReason::PhysicalQueryOrRightsDenied);
            CHECK(copy(mapping, 1024) == Result::Copied && scratch[0] == 0x69 && scratch[1023] == 0x69);
        }
        // A readable kernel VMA cannot override the logical guard.
        CHECK(Vm::Protect(mapping + host_page, host_page, Vm::Mode::NoAccess));
        CHECK(mprotect(reinterpret_cast<void*>(mapping + host_page), host_page, PROT_READ) == 0);
        before = calls();
        CHECK(copy(mapping + host_page - 512, 1024) == Result::Denied && calls() == before &&
              denial.reason == Vm::GuestDataLeaseDenialReason::LogicalRegistryDenied);
        CHECK(mprotect(reinterpret_cast<void*>(mapping + host_page), host_page, PROT_NONE) == 0);
        CHECK(Vm::Protect(mapping + host_page, host_page, Vm::Mode::ReadWrite));
        // Live physical changes outside Core must revoke the next full copy,
        // including a real short read with an accessible prefix >256 bytes.
        CHECK(mprotect(reinterpret_cast<void*>(mapping + host_page), host_page, PROT_NONE) == 0);
        before = calls();
        CHECK(copy(mapping + host_page, 1024) == Result::Denied && calls() == before + 1);
        CHECK(copy(mapping + host_page - 512, 1024) == Result::Denied && calls() == before + 2 &&
              denial.reason == Vm::GuestDataLeaseDenialReason::PhysicalQueryOrRightsDenied &&
              g_fex_guest_data_leases == 0);
        CHECK(mprotect(reinterpret_cast<void*>(mapping + host_page), host_page, PROT_READ | PROT_WRITE) == 0);
        CHECK(copy(mapping + host_page - 512, 1024) == Result::Copied &&
              std::memcmp(scratch.get(), reinterpret_cast<const void*>(mapping + host_page - 512), 1024) == 0);
        // Reusing scratch must still import fresh interior and tail bytes.
        reinterpret_cast<uint8_t*>(mapping)[513] ^= 1;
        reinterpret_cast<uint8_t*>(mapping)[1023] ^= 1;
        CHECK(copy(mapping, 1024) == Result::Copied && scratch[513] == 0x68 && scratch[1023] == 0x68);
    }
    CHECK(g_fex_data_retains.load(std::memory_order_relaxed) - retains ==
          g_fex_data_releases.load(std::memory_order_relaxed) - releases);
    g_native_data_admission.store(true, std::memory_order_release);
    CHECK(copy(mapping, 1024) == Result::Copied && g_native_data_token == 0);
    g_native_data_admission.store(false, std::memory_order_release);
    CHECK(!g_copied_read_without_admission.load(std::memory_order_relaxed));
    return Vm::Free(mapping) && Vm::Free(small);
}

bool GuestDataBoundedReadLeaseContract(uint64_t host_page)
{
    const uint64_t mapping = Vm::Alloc(0, 2 * host_page, Vm::Mode::ReadWrite);
    CHECK(mapping != 0 && (mapping & UINT64_C(0xff00000000000000)) == 0);
    std::memset(reinterpret_cast<void*>(mapping), 0x69, 2 * host_page);
    const auto calls = [] { return g_copied_read_calls.load(std::memory_order_relaxed); };
    Vm::GuestDataLeaseDenialMetadata denial;
    const auto denied = [&](uint64_t address, uint64_t bytes)
    {
        Vm::GuestDataLease lease;
        return !Vm::TryAcquireGuestDataLease(address, bytes, Vm::Mode::Read, &lease, true, &denial) &&
            !lease.IsActive() && g_fex_guest_data_leases == 0;
    };
    const uint64_t before_admission = calls();
    CHECK(denied(mapping, 8) && calls() == before_admission &&
          denial.reason == Vm::GuestDataLeaseDenialReason::MappingLeaseCapabilityUnavailable);
    g_fex_host_reader = true;
    {
        const uint64_t before = calls();
        Vm::GuestDataLease lease;
        CHECK(Vm::TryAcquireGuestDataLease(mapping + 3, 256, Vm::Mode::Read, &lease));
        CHECK(calls() == before + 1 && lease.IsActive() && g_fex_guest_data_leases == 1 &&
              *reinterpret_cast<const uint8_t*>(lease.HostBegin()) == 0x69);
        const uint64_t generation = Vm::GetMutationGeneration();
        CHECK(!Vm::Protect(mapping, host_page, Vm::Mode::NoAccess) && !Vm::Free(mapping) &&
              Vm::GetMutationGeneration() == generation && g_fex_guest_data_leases == 1);
    }
    CHECK(g_fex_guest_data_leases == 0);
    for (const auto failure: {CopiedReadFailure::Denied, CopiedReadFailure::Short})
    {
        const uint64_t before = calls();
        g_copied_read_failure = failure;
        CHECK(denied(mapping, 8) && calls() == before + 1 &&
              denial.reason == Vm::GuestDataLeaseDenialReason::PhysicalQueryOrRightsDenied &&
              *reinterpret_cast<const uint8_t*>(mapping) == 0x69);
    }
    CHECK(mprotect(reinterpret_cast<void*>(mapping), host_page, PROT_READ) == 0);
    {
        Vm::GuestDataLease lease;
        CHECK(Vm::TryAcquireGuestDataLease(mapping, 8, Vm::Mode::Read, &lease));
    }
    CHECK(mprotect(reinterpret_cast<void*>(mapping), host_page, PROT_NONE) == 0);
    CHECK(denied(mapping, 8) && denial.reason == Vm::GuestDataLeaseDenialReason::PhysicalQueryOrRightsDenied);
    CHECK(mprotect(reinterpret_cast<void*>(mapping), host_page, PROT_READ | PROT_WRITE) == 0);
    CHECK(mprotect(reinterpret_cast<void*>(mapping + host_page), host_page, PROT_NONE) == 0);
    // The kernel can read a prefix of this physically split range. That must
    // not create a lease, fall back to a raw read, or change guest bytes.
    const uint64_t before_guard = calls();
    CHECK(denied(mapping + host_page - 4, 8) && calls() == before_guard + 1 &&
          denial.reason == Vm::GuestDataLeaseDenialReason::PhysicalQueryOrRightsDenied);
    for (size_t index = 0; index < 4; ++index)
        CHECK(*reinterpret_cast<const uint8_t*>(mapping + host_page - 4 + index) == 0x69);
    CHECK(mprotect(reinterpret_cast<void*>(mapping + host_page), host_page, PROT_READ | PROT_WRITE) == 0);
    {
        const uint64_t before = calls();
        Vm::GuestDataLease large, tagged, writable;
        CHECK(Vm::TryAcquireGuestDataLease(mapping, 257, Vm::Mode::Read, &large));
        CHECK(Vm::TryAcquireGuestDataLease(mapping | UINT64_C(0xa500000000000000), 8, Vm::Mode::Read, &tagged));
        CHECK(Vm::TryAcquireGuestDataLease(mapping, 8, Vm::Mode::ReadWrite, &writable));
        CHECK(calls() == before && g_fex_guest_data_leases == 3);
    }
    CHECK(!g_copied_read_without_admission.load(std::memory_order_relaxed));
    g_fex_host_reader = false;
    return Vm::Free(mapping);
}

bool GuestDataWriteContract(uint64_t host_page)
{
    CHECK(!g_fex_host_reader && g_fex_guest_data_leases == 0 && g_native_data_token == 0 &&
          !g_native_data_admission.load(std::memory_order_acquire));
    const uint64_t mapping = Vm::Alloc(0, 2 * host_page, Vm::Mode::ReadWrite);
    const uint64_t small = Vm::Alloc(0, 8, Vm::Mode::ReadWrite);
    CHECK(mapping != 0 && small != 0);
    auto* const mapped = reinterpret_cast<uint8_t*>(mapping);
    std::memset(mapped, 0x69, 2 * host_page);
    std::memset(reinterpret_cast<void*>(small), 0x51, 8);
    alignas(256) std::array<uint8_t, 512> native;
    native.fill(0x69);
    const uint64_t native_address = reinterpret_cast<uint64_t>(native.data());
    CHECK((native_address & UINT64_C(0xff00000000000000)) == 0);
    std::array<uint8_t, 257> source;
    source.fill(0x37);
    const auto calls = [] { return g_copied_write_calls.load(std::memory_order_relaxed); };
    const auto reject = [&](uint64_t address, uint64_t size)
    {
        return !Vm::TryWriteGuestData(address, source.data(), size);
    };
    uint64_t before = calls();
    CHECK(reject(mapping, 8) && reject(native_address, 8) && calls() == before &&
          mapped[0] == 0x69 && native[0] == 0x69);
    const uint64_t retains = g_fex_data_retains.load(std::memory_order_relaxed);
    const uint64_t releases = g_fex_data_releases.load(std::memory_order_relaxed);
    {
        struct HostReaderScope
        {
            HostReaderScope() { g_fex_host_reader = true; }
            ~HostReaderScope()
            {
                if (g_fex_guest_data_leases != 0) std::abort();
                g_fex_host_reader = false;
            }
        } reader;
        CHECK(Vm::TryWriteGuestData(mapping + 3, source.data(), 256) && calls() == before + 1);
        CHECK(std::memcmp(mapped + 3, source.data(), 256) == 0 &&
              mapped[2] == 0x69 && mapped[259] == 0x69 && g_fex_guest_data_leases == 0);
        CHECK(Vm::TryWriteGuestData(native_address, source.data(), 8) && calls() == before + 2 &&
              std::memcmp(native.data(), source.data(), 8) == 0 && native[8] == 0x69);
        CHECK(Vm::TryWriteGuestData(mapping + host_page - 1, source.data(), 1) &&
              calls() == before + 3 && mapped[host_page - 1] == 0x37 && mapped[host_page] == 0x69);

        // The syscall receives an owned, page-contained snapshot of the input.
        const auto saved_source = source;
        g_copied_write_source_to_mutate = source.data();
        CHECK(Vm::TryWriteGuestData(mapping + 512, source.data(), 256));
        CHECK(source[0] == 0xc7 && source[255] == 0xc7 && source[256] == 0x37 &&
              std::memcmp(mapped + 512, saved_source.data(), 256) == 0);
        source = saved_source;

        // Large writes and small writes spanning host pages retain the full
        // normal lease and never dispatch a kernel copy.
        before = calls();
        CHECK(Vm::TryWriteGuestData(native_address, source.data(), source.size()) && calls() == before &&
              std::memcmp(native.data(), source.data(), source.size()) == 0 && native[257] == 0x69);
        CHECK(Vm::TryWriteGuestData(mapping + host_page - 4, source.data(), 8) && calls() == before &&
              std::memcmp(mapped + host_page - 4, source.data(), 8) == 0);
        CHECK(reject(0, 8) && reject(mapping, 0) && reject(UINT64_MAX - 3, 8) &&
              reject(mapping, UINT64_MAX) && reject(UINT64_C(0x00fffffffffffffc), 8) &&
              !Vm::TryWriteGuestData(mapping, nullptr, 8) && calls() == before);

        g_fex_guest_data_leases = UINT32_MAX;
        const bool negative_capability = reject(mapping, 8);
        g_fex_guest_data_leases = 0;
        CHECK(negative_capability && calls() == before);
        std::atomic<bool> foreign_denied {false};
        std::thread foreign([&] { foreign_denied.store(reject(mapping, 8), std::memory_order_release); });
        foreign.join();
        CHECK(foreign_denied.load(std::memory_order_acquire) && calls() == before);

        std::array<uint8_t, 8> original;
        std::memcpy(original.data(), mapped, original.size());
        for (const auto failure: {CopiedWriteFailure::Denied, CopiedWriteFailure::Zero})
        {
            g_copied_write_failure = failure;
            CHECK(reject(mapping, original.size()) && calls() == ++before &&
                  std::memcmp(mapped, original.data(), original.size()) == 0 && g_fex_guest_data_leases == 0);
        }
        for (const auto mode: {Vm::Mode::Read, Vm::Mode::Execute, Vm::Mode::NoAccess})
        {
            CHECK(Vm::Protect(mapping, host_page, mode));
            // A physically writable page must not bypass logical permissions.
            CHECK(mprotect(mapped, host_page, PROT_READ | PROT_WRITE) == 0);
            CHECK(reject(mapping, 8) && calls() == before &&
                  std::memcmp(mapped, original.data(), original.size()) == 0);
        }
        CHECK(Vm::Protect(mapping, host_page, Vm::Mode::Write));
        CHECK(Vm::TryWriteGuestData(mapping, source.data(), 8) && calls() == before + 1);
        CHECK(Vm::Protect(mapping, host_page, Vm::Mode::ReadWrite));

        // External permission changes are checked live by the real kernel.
        // With one remote page, denial must leave the entire destination alone.
        for (const int protection: {PROT_READ, PROT_NONE})
        {
            std::memset(mapped, 0x69, 16);
            CHECK(mprotect(mapped, host_page, protection) == 0);
            before = calls();
            CHECK(reject(mapping + 3, 8) && calls() == before + 1);
            CHECK(mprotect(mapped, host_page, PROT_READ | PROT_WRITE) == 0);
            for (size_t i = 0; i < 16; ++i) CHECK(mapped[i] == 0x69);
        }
        // An ineligible cross-page write proves the whole physical span before
        // its ordinary memcpy; a writable first page is not sufficient.
        std::memset(mapped + host_page - 4, 0x69, 8);
        CHECK(mprotect(mapped + host_page, host_page, PROT_READ) == 0);
        before = calls();
        CHECK(reject(mapping + host_page - 4, 8) && calls() == before);
        for (size_t i = 0; i < 8; ++i) CHECK(mapped[host_page - 4 + i] == 0x69);
        const uint64_t tagged_guard = (mapping + host_page) | UINT64_C(0xa500000000000000);
        const uint64_t tagged_retains = g_fex_data_retains.load(std::memory_order_relaxed);
        CHECK(reject(tagged_guard, 8) && calls() == before &&
              g_fex_data_retains.load(std::memory_order_relaxed) == tagged_retains + 1);
        CHECK(mprotect(mapped + host_page, host_page, PROT_READ | PROT_WRITE) == 0);
        CHECK(reject(small, 9) && reject(small + 8, 1) && calls() == before &&
              *reinterpret_cast<const uint8_t*>(small) == 0x51);

        Vm::ReservationHandle reservation;
        {
            Vm::MutationTransaction transaction;
            CHECK(Vm::BeginReserveAligned(0, host_page, host_page, &reservation, &transaction));
            transaction.Commit();
        }
        CHECK(mprotect(reinterpret_cast<void*>(reservation.base), host_page, PROT_READ | PROT_WRITE) == 0);
        CHECK(reject(reservation.base, 8) && calls() == before);
        CHECK(Vm::ReleaseReservation(reservation));
    }
    CHECK(g_fex_data_retains.load(std::memory_order_relaxed) - retains ==
          g_fex_data_releases.load(std::memory_order_relaxed) - releases);
    const uint64_t pins = g_native_data_pins.load(std::memory_order_relaxed);
    const uint64_t unpins = g_native_data_unpins.load(std::memory_order_relaxed);
    g_native_data_admission.store(true, std::memory_order_release);
    CHECK(Vm::TryWriteGuestData(native_address, source.data(), 8) && g_native_data_token == 0 &&
          g_native_data_pins.load(std::memory_order_relaxed) == pins + 1 &&
          g_native_data_unpins.load(std::memory_order_relaxed) == unpins + 1);
    g_native_data_admission.store(false, std::memory_order_release);
    before = calls();
    CHECK(reject(native_address, 8) && calls() == before &&
          !g_copied_write_invalid_contract.load(std::memory_order_relaxed));

    // A positive short write may have changed guest bytes. Observe a real
    // prefix in shared memory after the child fail-stops, with no retry,
    // rollback, or success reported to its caller.
    void* const shared = mmap(nullptr, host_page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    CHECK(shared != MAP_FAILED);
    std::memset(shared, 0x69, host_page);
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0)
    {
        alarm(20);
        signal(SIGABRT, +[](int signal_number) { _exit(signal_number); });
        g_fex_host_reader = true;
        g_copied_write_failure = CopiedWriteFailure::Short;
        Vm::TryWriteGuestData(reinterpret_cast<uint64_t>(shared), source.data(), 8);
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == SIGABRT);
    const auto* shared_bytes = static_cast<const uint8_t*>(shared);
    for (size_t i = 0; i < 7; ++i) CHECK(shared_bytes[i] == 0x37);
    CHECK(shared_bytes[7] == 0x69 && shared_bytes[8] == 0x69);
    CHECK(munmap(shared, host_page) == 0);
    return Vm::Free(mapping) && Vm::Free(small);
}

bool GuestDataNativeRouteContract(uint64_t host_page)
{
    const uint64_t capability_queries = g_copied_read_capability_queries.load(std::memory_order_relaxed);
    const uint64_t mapping = Vm::Alloc(0, host_page, Vm::Mode::ReadWrite);
    CHECK(mapping != 0 && (mapping & UINT64_C(0xff00000000000000)) == 0);
    std::memset(reinterpret_cast<void*>(mapping), 0x69, host_page);
    std::array<uint8_t, 257> destination;
    destination.fill(0xa3);
    const auto untouched = destination;
    const uint64_t calls = g_copied_read_calls.load(std::memory_order_relaxed);
    const uint64_t write_calls = g_copied_write_calls.load(std::memory_order_relaxed);
    const uint64_t retains = g_fex_data_retains.load(std::memory_order_relaxed);
    const uint64_t releases = g_fex_data_releases.load(std::memory_order_relaxed);
    CHECK(!Vm::TryReadGuestData(mapping, destination.data(), 8) && destination == untouched);
    CHECK(!Vm::TryWriteGuestData(mapping, untouched.data(), 8));
    {
        struct HostReaderScope
        {
            HostReaderScope() { g_fex_host_reader = true; }
            ~HostReaderScope()
            {
                if (g_fex_guest_data_leases != 0) std::abort();
                g_fex_host_reader = false;
            }
        } reader;
        // Logical tag zero still retains a real lease and bypasses kernel copy
        // when MTE is available (or capability discovery cannot be trusted).
        CHECK(Vm::CopyGuestDataToPrivateScratch(mapping, destination.data(), destination.size()) ==
              Vm::PrivateGuestDataCopyResult::Unsupported && destination == untouched &&
              g_fex_data_retains.load(std::memory_order_relaxed) == retains &&
              g_copied_read_calls.load(std::memory_order_relaxed) == calls);
        CHECK(Vm::TryReadGuestData(mapping + 3, destination.data(), 256));
        for (size_t i = 0; i < 256; ++i) CHECK(destination[i] == 0x69);
        CHECK(destination[256] == untouched[256] && g_fex_guest_data_leases == 0);
        CHECK(g_fex_data_retains.load(std::memory_order_relaxed) == retains + 1 &&
              g_fex_data_releases.load(std::memory_order_relaxed) == releases + 1);
        CHECK(mprotect(reinterpret_cast<void*>(mapping), host_page, PROT_NONE) == 0);
        destination = untouched;
        CHECK(!Vm::TryReadGuestData(mapping, destination.data(), 8) && destination == untouched);
        CHECK(mprotect(reinterpret_cast<void*>(mapping), host_page, PROT_READ | PROT_WRITE) == 0);
        CHECK(g_fex_data_retains.load(std::memory_order_relaxed) == retains + 2 &&
              g_fex_data_releases.load(std::memory_order_relaxed) == releases + 2);
        CHECK(Vm::TryWriteGuestData(mapping + 3, untouched.data(), 256));
        CHECK(std::memcmp(reinterpret_cast<const void*>(mapping + 3), untouched.data(), 256) == 0 &&
              *reinterpret_cast<const uint8_t*>(mapping + 2) == 0x69 &&
              *reinterpret_cast<const uint8_t*>(mapping + 259) == 0x69);
        CHECK(mprotect(reinterpret_cast<void*>(mapping), host_page, PROT_READ) == 0);
        CHECK(!Vm::TryWriteGuestData(mapping, untouched.data(), 8) &&
              *reinterpret_cast<const uint8_t*>(mapping) == 0x69);
        CHECK(mprotect(reinterpret_cast<void*>(mapping), host_page, PROT_READ | PROT_WRITE) == 0);
        CHECK(g_fex_data_retains.load(std::memory_order_relaxed) == retains + 4 &&
              g_fex_data_releases.load(std::memory_order_relaxed) == releases + 4);
    }
    CHECK(g_copied_read_calls.load(std::memory_order_relaxed) == calls &&
          g_copied_write_calls.load(std::memory_order_relaxed) == write_calls);
#if defined(__aarch64__) && defined(HWCAP2_MTE)
    CHECK(g_copied_read_capability_queries.load(std::memory_order_relaxed) == capability_queries + 1);
#else
    (void)capability_queries;
#endif
    return Vm::Free(mapping);
}

bool GuestExecutionLogicalRightsContract(uint64_t host_page)
{
    const uint64_t mapping = Vm::Alloc(0, host_page, Vm::Mode::Write);
    const uint64_t small = Vm::Alloc(0, 8, Vm::Mode::ReadWrite);
    CHECK(mapping != 0 && small != 0);
    CHECK(IsMode(mapping, Vm::Mode::Write) &&
          PhysicalProtection(mapping) == (PROT_READ | PROT_WRITE));
    const uint64_t marker = UINT64_C(0x123456789abcdef0);
    *reinterpret_cast<volatile uint64_t*>(mapping) = marker;
    CHECK(*reinterpret_cast<const volatile uint64_t*>(mapping) == marker);
    // The embedding's execution barrier is modeled by this single-threaded
    // fixture. These helpers prove rights only, without creating any reader.
    CHECK(Vm::CheckGuestDataAccessUnderFexExecutionBarrier(mapping, 8, false));
    CHECK(Vm::CheckGuestDataAccessUnderFexExecutionBarrier(mapping, 8, true));
    CHECK(Vm::CheckOwnedStackAccessUnderFexExecutionBarrier(mapping, 8, false, mapping, host_page));
    CHECK(Vm::CheckOwnedStackAccessUnderFexExecutionBarrier(mapping, 8, true, mapping, host_page));
    Vm::ManagedRegion region;
    CHECK(Vm::QueryManagedRegion(mapping, &region) == Vm::ManagedRegionState::Visible &&
          (static_cast<uint32_t>(region.mode) & static_cast<uint32_t>(Vm::Mode::Execute)) == 0 &&
          (PhysicalProtection(mapping) & PROT_EXEC) == 0);
    CHECK(Vm::Protect(mapping, host_page, Vm::Mode::Read));
    CHECK(Vm::CheckGuestDataAccessUnderFexExecutionBarrier(mapping, 8, false) &&
          !Vm::CheckGuestDataAccessUnderFexExecutionBarrier(mapping, 8, true) &&
          !Vm::CheckOwnedStackAccessUnderFexExecutionBarrier(mapping, 8, true, mapping, host_page));
    CHECK(AccessDenied(mapping, true));
    CHECK(Vm::Protect(mapping, host_page, Vm::Mode::Execute));
    CHECK(Vm::CheckGuestDataAccessUnderFexExecutionBarrier(mapping, 8, false) &&
          !Vm::CheckGuestDataAccessUnderFexExecutionBarrier(mapping, 8, true) &&
          (PhysicalProtection(mapping) & PROT_EXEC) == 0);
    CHECK(Vm::Protect(mapping, host_page, Vm::Mode::NoAccess));
    CHECK(!Vm::CheckGuestDataAccessUnderFexExecutionBarrier(mapping, 8, false) &&
          !Vm::CheckGuestDataAccessUnderFexExecutionBarrier(mapping, 8, true));
    CHECK(PhysicalProtection(small + 8) == (PROT_READ | PROT_WRITE));
    CHECK(!Vm::CheckGuestDataAccessUnderFexExecutionBarrier(small, 9, false) &&
          !Vm::CheckGuestDataAccessUnderFexExecutionBarrier(small + 8, 1, true) &&
          !Vm::CheckOwnedStackAccessUnderFexExecutionBarrier(small + 8, 1, true, small, host_page));
    std::array<uint8_t, 16> native {};
    CHECK(!Vm::CheckGuestDataAccessUnderFexExecutionBarrier(reinterpret_cast<uint64_t>(native.data()),
                                                          native.size(), false));
    return Vm::Free(mapping) && Vm::Free(small);
}

// Compare the bounded data preflight against the public region query, whose
// historical extent contract must remain unchanged. This reference checks
// every byte and uses the independent x86 data-rights rule.
bool PublicRegionDataAccess(uint64_t address, uint64_t bytes, bool write)
{
    if (address == 0 || bytes == 0 || bytes > UINT64_MAX - address) return false;
    const uint64_t end = address + bytes;
    while (address < end)
    {
        Vm::ManagedRegion region {};
        if (Vm::QueryManagedRegion(address, &region) != Vm::ManagedRegionState::Visible ||
            region.begin > address || region.end <= address || region.mode == Vm::Mode::NoAccess ||
            (write && (static_cast<uint32_t>(region.mode) & static_cast<uint32_t>(Vm::Mode::Write)) == 0))
            return false;
        address = region.end < end ? region.end : end;
    }
    return true;
}

bool GuestDataBoundedRangeContract(uint64_t host_page)
{
    constexpr uint64_t large_size = 2 * 1024 * 1024;
    const uint64_t regular = Vm::Alloc(0, large_size, Vm::Mode::ReadWrite);
    Vm::ReservationHandle reservation {};
    Vm::MutationTransaction reserve;
    CHECK(regular != 0 && Vm::BeginReserveAligned(0, large_size + 2 * host_page, host_page,
                                                  &reservation, &reserve));
    reserve.Commit();
    const uint64_t reserved = reservation.base + host_page;
    CHECK(Vm::AllocFixed(reserved, large_size, Vm::Mode::ReadWrite));
    std::array<uint8_t, 257> source;
    source.fill(0x6d);
    std::array<uint8_t, 257> output;
    const auto differential = [&](uint64_t address, uint64_t bytes)
    {
        for (const bool write: {false, true})
        {
            const bool expected = PublicRegionDataAccess(address, bytes, write);
            if (Vm::CheckGuestDataAccessUnderFexExecutionBarrier(address, bytes, write) != expected ||
                Vm::CheckOwnedStackAccessUnderFexExecutionBarrier(address, bytes, write,
                                                                  address, bytes) != expected)
                return false;
        }
        return true;
    };
    g_fex_host_reader = true;
    for (const uint64_t base: {regular, reserved})
    {
        const uint64_t center = base + large_size / 2;
        Vm::ManagedRegion before {}, after {};
        CHECK(Vm::QueryManagedRegion(center + 3, &before) == Vm::ManagedRegionState::Visible &&
              before.begin < center && before.end > center + host_page);
        for (const uint64_t bytes: {uint64_t{1}, uint64_t{8}, uint64_t{256}, uint64_t{257}, host_page + 8})
            CHECK(differential(center + 3, bytes));
        CHECK(Vm::TryReadGuestData(center + 3, output.data(), 256) &&
              Vm::TryWriteGuestData(center + 3, source.data(), 256) &&
              Vm::TryReadGuestData(center + 3, output.data(), 256) &&
              std::memcmp(output.data(), source.data(), 256) == 0 && g_fex_guest_data_leases == 0);
        CHECK(Vm::QueryManagedRegion(center + 3, &after) == Vm::ManagedRegionState::Visible &&
              before.begin == after.begin && before.end == after.end && before.mode == after.mode);

        for (uint32_t bits = 0; bits < 8; ++bits)
        {
            const auto mode = static_cast<Vm::Mode>(bits);
            CHECK(Vm::Protect(center, host_page, mode));
            // Live physical rights deliberately disagree with the logical
            // guard/read-only page: a data query cannot grant those writes.
            CHECK(mprotect(reinterpret_cast<void*>(center), host_page, PROT_READ | PROT_WRITE) == 0);
            for (const uint64_t start: {center - 4, center + 3, center + host_page - 4})
                for (const uint64_t bytes: {uint64_t{1}, uint64_t{8}, uint64_t{256}, uint64_t{257}})
                    CHECK(differential(start, bytes));
            output.fill(0xa3);
            const auto untouched = output;
            const uint64_t reads_before = g_copied_read_calls.load(std::memory_order_relaxed);
            const bool readable = bits != 0;
            CHECK(Vm::TryReadGuestData(center + 3, output.data(), 8) == readable);
            if (!readable) CHECK(output == untouched && g_copied_read_calls.load(std::memory_order_relaxed) == reads_before);
            const uint64_t writes_before = g_copied_write_calls.load(std::memory_order_relaxed);
            const bool writable = (bits & static_cast<uint32_t>(Vm::Mode::Write)) != 0;
            CHECK(Vm::TryWriteGuestData(center + 3, source.data(), 8) == writable);
            if (!writable) CHECK(g_copied_write_calls.load(std::memory_order_relaxed) == writes_before);
            CHECK(g_fex_guest_data_leases == 0);
        }
        CHECK(Vm::Protect(center, host_page, Vm::Mode::ReadWrite));
        // A denial elsewhere in the same large mapping must not deny the
        // scalar range; touching it, including as the last byte, must deny.
        CHECK(Vm::Protect(center + host_page, host_page, Vm::Mode::NoAccess));
        CHECK(differential(center + 3, 8) && differential(center + host_page - 4, 8) &&
              Vm::TryReadGuestData(center + 3, output.data(), 8));
        output.fill(0xa3);
        const auto untouched = output;
        CHECK(!Vm::TryReadGuestData(center + host_page - 4, output.data(), 8) && output == untouched);
        CHECK(Vm::Protect(center + host_page, host_page, Vm::Mode::ReadWrite));
        // The logical proof cannot cache a physical protection observation.
        CHECK(mprotect(reinterpret_cast<void*>(center), host_page, PROT_READ) == 0);
        CHECK(!Vm::TryWriteGuestData(center + 3, source.data(), 8));
        CHECK(mprotect(reinterpret_cast<void*>(center), host_page, PROT_NONE) == 0);
        output.fill(0xa3);
        CHECK(!Vm::TryReadGuestData(center + 3, output.data(), 8) && output == untouched);
        CHECK(mprotect(reinterpret_cast<void*>(center), host_page, PROT_READ | PROT_WRITE) == 0);
        CHECK(Vm::TryReadGuestData(center + 3, output.data(), 8));
    }
    // Reservation holes remain denied even with deliberately readable backing.
    CHECK(mprotect(reinterpret_cast<void*>(reservation.base), host_page, PROT_READ | PROT_WRITE) == 0);
    CHECK(differential(reservation.base, 8) && differential(reserved - 4, 8) &&
          !Vm::TryReadGuestData(reserved - 4, output.data(), 8));
    CHECK(mprotect(reinterpret_cast<void*>(reservation.base), host_page, PROT_NONE) == 0);
    g_fex_host_reader = false;

    const uint64_t center = regular + large_size / 2 + 3;
    Vm::ManagedRegion bounded {};
    CHECK(Lumen::Core::sys_virtual_query_managed_data_region(center, center + 8, &bounded) ==
              Vm::ManagedRegionState::Visible && bounded.begin == center && bounded.end == center + 8);
    CHECK(Lumen::Core::sys_virtual_query_managed_data_region(center, center, &bounded) ==
              Vm::ManagedRegionState::Unmanaged && bounded.begin == 0 && bounded.end == 0);
    CHECK(Lumen::Core::sys_virtual_query_managed_data_region(center, 0, &bounded) ==
              Vm::ManagedRegionState::Unmanaged && bounded.begin == 0 && bounded.end == 0);
    CHECK(Vm::Free(regular) && Vm::Free(reserved));
    CHECK(!Vm::CheckGuestDataAccessUnderFexExecutionBarrier(reserved + large_size / 2, 8, false));
    CHECK(Vm::AllocFixed(reserved, large_size, Vm::Mode::Read));
    CHECK(Vm::CheckGuestDataAccessUnderFexExecutionBarrier(reserved + large_size / 2, 8, false) &&
          !Vm::CheckGuestDataAccessUnderFexExecutionBarrier(reserved + large_size / 2, 8, true));
    CHECK(Vm::ReleaseReservation(reservation) && g_fex_guest_data_leases == 0);
    return true;
}

bool GuestDataQueryBenchmark(uint64_t host_page)
{
    constexpr uint64_t large_size = 2 * 1024 * 1024;
    constexpr uint64_t iterations = 1000000;
    const uint64_t regular = Vm::Alloc(0, large_size, Vm::Mode::ReadWrite);
    Vm::ReservationHandle reservation {};
    Vm::MutationTransaction reserve;
    CHECK(regular != 0 && Vm::BeginReserveAligned(0, large_size, host_page, &reservation, &reserve));
    reserve.Commit();
    CHECK(Vm::AllocFixed(reservation.base, large_size, Vm::Mode::ReadWrite));
    for (const uint64_t base: {regular, reservation.base})
    {
        const uint64_t address = base + large_size / 2 + 3;
        for (const bool bounded: {false, true})
        {
            timespec start {}, end {};
            CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
            uint64_t accepted = 0;
            for (uint64_t iteration = 0; iteration < iterations; ++iteration)
                accepted += bounded ? Vm::CheckGuestDataAccessUnderFexExecutionBarrier(address, 8, false) :
                                      PublicRegionDataAccess(address, 8, false);
            CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0 && accepted == iterations);
            const int64_t elapsed_ns = (end.tv_sec - start.tv_sec) * INT64_C(1000000000) + end.tv_nsec - start.tv_nsec;
            std::printf("guest-data-query-benchmark mapping=%s route=%s snapshot=%u iterations=%" PRIu64 " ns_per_check=%.1f\n",
                        base == regular ? "regular" : "reserved", bounded ? "bounded" : "public-reference",
                        Vm::IsImmutableRegionSnapshotEnabled(), iterations, static_cast<double>(elapsed_ns) / iterations);
        }
    }
    return Vm::Free(regular) && Vm::ReleaseReservation(reservation);
}

bool NativeGuestDataLeaseContract(uint64_t host_page)
{
    CHECK(!g_fex_host_reader && g_fex_guest_data_leases == 0 && g_native_data_token == 0 &&
          !Vm::CurrentThreadHasMemoryLease());
    const uint64_t small = Vm::Alloc(0, 8, Vm::Mode::ReadWrite);
    const uint64_t mapping = Vm::Alloc(0, host_page, Vm::Mode::ReadWrite);
    CHECK(small != 0 && mapping != 0);
    std::memset(reinterpret_cast<void*>(small), 0x51, static_cast<size_t>(host_page));
    g_native_data_admission.store(true, std::memory_order_release);
    const uint64_t pins = g_native_data_pins.load(std::memory_order_relaxed);
    const uint64_t unpins = g_native_data_unpins.load(std::memory_order_relaxed);
    {
        Vm::GuestDataLease first, second, padding;
        CHECK(Vm::TryAcquireGuestDataLease(small, 8, Vm::Mode::ReadWrite, &first));
        CHECK(Vm::CurrentThreadHasMemoryLease());
        const uint64_t token = g_native_data_token;
        CHECK(token != 0 && g_fex_guest_data_leases == 0 &&
              g_native_data_pins.load(std::memory_order_relaxed) == pins + 1);
        // Padding is physically RW in this same VMA. Both a complete crossing
        // range and a byte wholly inside padding must fail logically.
        Vm::GuestDataLeaseDenialMetadata denial;
        CHECK(PhysicalProtection(small + 8) == (PROT_READ | PROT_WRITE));
        CHECK(!Vm::TryAcquireGuestDataLease(small, 9, Vm::Mode::Read, &padding, false, &denial) &&
              denial.reason == Vm::GuestDataLeaseDenialReason::LogicalRegistryDenied);
        CHECK(!Vm::TryAcquireGuestDataLease(small + 8, 1, Vm::Mode::Write, &padding));
        CHECK(g_native_data_token == token && !padding.IsActive());
        CHECK(Vm::TryAcquireGuestDataLease(mapping + 17, 8, Vm::Mode::ReadWrite, &second));
        const uint64_t generation = Vm::GetMutationGeneration();
        CHECK(!Vm::Protect(small, 8, Vm::Mode::NoAccess) && !Vm::Free(small) &&
              Vm::GetMutationGeneration() == generation);
        // Release order need not be the inverse of acquisition order.
        Vm::ReleaseGuestDataLease(&first);
        CHECK(g_native_data_token == token && second.IsActive() && Vm::CurrentThreadHasMemoryLease());
        const uint64_t marker = UINT64_C(0x9876543210abcdef);
        std::memcpy(reinterpret_cast<void*>(second.HostBegin()), &marker, sizeof(marker));
        Vm::ReleaseGuestDataLease(&second);
        CHECK(g_native_data_token == 0 && !Vm::CurrentThreadHasMemoryLease() &&
              g_native_data_unpins.load(std::memory_order_relaxed) == unpins + 1);
    }
    // With native admission denied, permissions alone never grant a pointer.
    g_native_data_admission.store(false, std::memory_order_release);
    {
        Vm::GuestDataLease denied;
        CHECK(!Vm::TryAcquireGuestDataLease(small, 8, Vm::Mode::Read, &denied) && !denied.IsActive());
    }
    CHECK(*reinterpret_cast<const uint8_t*>(small + 8) == 0x51);
    return Vm::Free(mapping) && Vm::Free(small);
}

bool GuestDataRetryMetadataContract(uint64_t host_page)
{
    CHECK(!g_fex_host_reader && g_fex_guest_data_leases == 0 && g_native_data_token == 0);
    const uint64_t mapping = Vm::Alloc(0, host_page, Vm::Mode::ReadWrite);
    const uint64_t small = Vm::Alloc(0, 8, Vm::Mode::ReadWrite);
    CHECK(mapping != 0 && small != 0);
    std::memset(reinterpret_cast<void*>(mapping), 0x69, host_page);
    std::array<uint8_t, 257> destination;
    destination.fill(0xa3);
    const auto untouched = destination;
    Vm::GuestDataLeaseDenialMetadata denial;
    const uint64_t calls = g_copied_read_calls.load(std::memory_order_relaxed);
    const uint64_t pins = g_native_data_pins.load(std::memory_order_relaxed);
    g_native_data_admission.store(true, std::memory_order_release);
    g_native_data_mutation_pending.store(true, std::memory_order_release);
    {
        Vm::GuestDataLease lease;
        CHECK(!Vm::TryAcquireGuestDataLease(mapping, 8, Vm::Mode::Read, &lease, false, &denial) &&
              denial.retryable && denial.reason == Vm::GuestDataLeaseDenialReason::GateWriterPending &&
              !lease.IsActive() && g_native_data_token == 0);
        // Both copied and ordinary-lease reads carry the exact admission result.
        for (const size_t bytes: {size_t{8}, destination.size()})
            CHECK(!Vm::TryReadGuestData(mapping, destination.data(), bytes, &denial) &&
                  denial.retryable && denial.reason == Vm::GuestDataLeaseDenialReason::GateWriterPending &&
                  destination == untouched);
        CHECK(g_copied_read_calls.load(std::memory_order_relaxed) == calls &&
              g_native_data_pins.load(std::memory_order_relaxed) == pins);
        CHECK(!Vm::TryReadGuestData(mapping, nullptr, 8, &denial) && !denial.retryable &&
              denial.reason == Vm::GuestDataLeaseDenialReason::LeaseOrRangeInvalid);
        CHECK(!Vm::TryAcquireGuestDataLease(mapping, 0, Vm::Mode::Read, &lease, false, &denial) &&
              !denial.retryable && denial.reason == Vm::GuestDataLeaseDenialReason::LeaseOrRangeInvalid);

        // Context failure has priority over a simultaneous pending mutation.
        g_fex_host_reader = true;
        g_fex_guest_data_leases = UINT32_MAX;
        const bool invalid_context = !Vm::TryReadGuestData(mapping, destination.data(), 8, &denial) &&
            !denial.retryable && denial.reason == Vm::GuestDataLeaseDenialReason::MappingLeaseCapabilityUnavailable;
        g_fex_guest_data_leases = 0;
        g_fex_host_reader = false;
        CHECK(invalid_context && destination == untouched);
    }
    g_native_data_mutation_pending.store(false, std::memory_order_release);
    {
        Vm::GuestDataLease prefix, invalid;
        CHECK(Vm::TryAcquireGuestDataLease(small, 8, Vm::Mode::Read, &prefix, false, &denial) &&
              !denial.retryable && denial.reason == Vm::GuestDataLeaseDenialReason::None);
        // A held native pin admits a nested read during full intent. Its own
        // logical/physical denial must never inherit the unrelated busy flag.
        g_native_data_mutation_pending.store(true, std::memory_order_release);
        CHECK(!Vm::TryAcquireGuestDataLease(small, 9, Vm::Mode::Read, &invalid, false, &denial) &&
              !denial.retryable && denial.reason == Vm::GuestDataLeaseDenialReason::LogicalRegistryDenied);
        CHECK(!Vm::TryReadGuestData(small + 8, destination.data(), 1, &denial) &&
              !denial.retryable && denial.reason == Vm::GuestDataLeaseDenialReason::LogicalRegistryDenied &&
              destination == untouched);
        g_copied_read_failure = CopiedReadFailure::Denied;
        CHECK(!Vm::TryReadGuestData(mapping, destination.data(), 8, &denial) &&
              !denial.retryable && denial.reason == Vm::GuestDataLeaseDenialReason::PhysicalQueryOrRightsDenied &&
              destination == untouched);
        CHECK(mprotect(reinterpret_cast<void*>(mapping), host_page, PROT_NONE) == 0);
        CHECK(!Vm::TryAcquireGuestDataLease(mapping, 8, Vm::Mode::Read, &invalid, false, &denial) &&
              !denial.retryable && denial.reason == Vm::GuestDataLeaseDenialReason::PhysicalQueryOrRightsDenied);
        CHECK(mprotect(reinterpret_cast<void*>(mapping), host_page, PROT_READ | PROT_WRITE) == 0);
        CHECK(Vm::TryReadGuestData(mapping, destination.data(), 8, &denial) &&
              !denial.retryable && denial.reason == Vm::GuestDataLeaseDenialReason::None && destination[0] == 0x69);
        g_native_data_mutation_pending.store(false, std::memory_order_release);
    }
    CHECK(g_native_data_token == 0);
    g_native_data_admission.store(false, std::memory_order_release);
    CHECK(!Vm::TryReadGuestData(mapping, destination.data(), 8, &denial) && !denial.retryable &&
          denial.reason == Vm::GuestDataLeaseDenialReason::MappingLeaseCapabilityUnavailable);
    return Vm::Free(mapping) && Vm::Free(small);
}

bool GuestMemoryMutationOwnerContract(uint64_t host_page)
{
    CHECK(!g_fex_host_reader && g_fex_mutation_depth == 0 && g_native_data_token == 0 &&
          !g_native_data_admission.load() && !Vm::CurrentThreadHasMemoryLease());
    const uint64_t mapping = Vm::Alloc(0, host_page, Vm::Mode::ReadWrite);
    CHECK(mapping != 0);
    const auto denied_without_adoption = [&] {
        Vm::GuestDataLease lease;
        return !Vm::TryAcquireGuestDataLease(mapping + 17, 8, Vm::Mode::ReadWrite, &lease, false) &&
            !lease.IsActive() && !Vm::CurrentThreadHasMemoryLease();
    };
    {
        Vm::GuestMemoryMutationOwnerScope absent(0);
        CHECK(!absent.IsActive() && denied_without_adoption());
    }
    uint64_t stale = 0;
    const uint64_t pins = g_native_data_pins.load();
    const uint64_t retains = g_fex_data_retains.load();
    {
        const uint64_t token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(0);
        CHECK(token != 0 && LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(token) == 1);
        struct EndToken {
            uint64_t token;
            ~EndToken() { LumenAndroidFexCoreEndGuestMemoryMutationV1(token, 0, 0, 0); }
        } end {token};
        stale = token;
        {
            Vm::GuestMemoryMutationOwnerScope wrong(token + 1);
            CHECK(!wrong.IsActive() && denied_without_adoption());
        }
        {
            Vm::GuestMemoryMutationOwnerScope owner(token);
            CHECK(owner.IsActive());
            Vm::GuestDataLease first, nested;
            CHECK(Vm::TryAcquireGuestDataLease(mapping + 17, 8, Vm::Mode::ReadWrite, &first, false));
            CHECK(Vm::TryAcquireGuestDataLease(mapping + 21, 4, Vm::Mode::Read, &nested, false));
            CHECK(first.IsActive() && nested.IsActive() && Vm::CurrentThreadHasMemoryLease());
            const uint64_t value = UINT64_C(0x123456789abcdef0);
            std::memcpy(reinterpret_cast<void*>(first.HostBegin()), &value, sizeof(value));
            uint64_t observed = 0;
            CHECK(Vm::TryReadGuestData(mapping + 17, &observed, sizeof(observed)) && observed == value);
            std::array<uint8_t, 1024> private_copy;
            CHECK(Vm::CopyGuestDataToPrivateScratch(mapping, private_copy.data(), private_copy.size()) ==
                  Vm::PrivateGuestDataCopyResult::Copied &&
                  std::memcmp(private_copy.data(), reinterpret_cast<const void*>(mapping), private_copy.size()) == 0 &&
                  first.IsActive() && nested.IsActive() && Vm::CurrentThreadHasMemoryLease());
            Vm::GuestMemoryMutationOwnerScope duplicate(token);
            CHECK(!duplicate.IsActive());
            std::atomic<bool> foreign_denied {false};
            std::thread foreign([&] {
                Vm::GuestMemoryMutationOwnerScope foreign_owner(token);
                foreign_denied.store(!foreign_owner.IsActive() && denied_without_adoption());
            });
            foreign.join();
            CHECK(foreign_denied.load());
            Vm::ReleaseGuestDataLease(&first);
            CHECK(nested.IsActive() && Vm::CurrentThreadHasMemoryLease());
            Vm::ReleaseGuestDataLease(&nested);
            CHECK(!Vm::CurrentThreadHasMemoryLease());
        }
        CHECK(denied_without_adoption() && g_native_data_pins.load() == pins && g_fex_data_retains.load() == retains);
    }
    {
        Vm::GuestMemoryMutationOwnerScope ended(stale);
        CHECK(!ended.IsActive() && LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(stale) == 0 &&
              denied_without_adoption());
    }
    {
        const uint64_t additive = LumenAndroidFexCoreBeginGuestMemoryMutationV2(1);
        CHECK(additive != 0);
        struct EndToken {
            uint64_t token;
            ~EndToken() { LumenAndroidFexCoreEndGuestMemoryMutationV1(token, 0, 0, 0); }
        } end {additive};
        Vm::GuestMemoryMutationOwnerScope nonexclusive(additive);
        CHECK(!nonexclusive.IsActive() && LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(additive) == 0 &&
              denied_without_adoption());
    }
    CHECK(g_fex_mutation_depth == 0 && g_fex_active.load() == 0 && Vm::Free(mapping));
    return true;
}

bool FreeAllocationInvalidationContract(uint64_t host_page)
{
    const uint64_t bytes = 3 * host_page;
    const uint64_t mapping = Vm::Alloc(0, bytes, Vm::Mode::ReadWrite);
    const uint64_t unrelated = Vm::Alloc(0, host_page, Vm::Mode::ReadWrite);
    CHECK(mapping != 0 && unrelated != 0);
    CHECK(Vm::Protect(mapping, host_page, Vm::Mode::Read));
    CHECK(Vm::Protect(mapping + 2 * host_page, host_page, Vm::Mode::ExecuteReadWrite));
    const Vm::Mode modes[] = {Vm::Mode::Read, Vm::Mode::ReadWrite, Vm::Mode::ExecuteReadWrite};
    for (uint64_t run = 0; run < 3; ++run)
    {
        Vm::ManagedRegion region {};
        CHECK(Vm::QueryManagedRegion(mapping + run * host_page, &region) ==
              Vm::ManagedRegionState::Visible);
        CHECK(region.begin == mapping + run * host_page &&
              region.end == mapping + (run + 1) * host_page && region.mode == modes[run]);
    }
    uint64_t allocation_begin = 0, allocation_bytes = 0;
    CHECK(Lumen::Core::sys_virtual_query_allocation_range(
              mapping + 3, &allocation_begin, &allocation_bytes));
    CHECK(allocation_begin == mapping && allocation_bytes == bytes);

    // An interior protection run is not an allocation owner. Its failed query
    // and failed free must close admission without notifying or committing an
    // invalidation. The real backend remains responsible for the refusal.
    allocation_begin = allocation_bytes = UINT64_MAX;
    CHECK(!Lumen::Core::sys_virtual_query_allocation_range(
              mapping + host_page, &allocation_begin, &allocation_bytes));
    CHECK(allocation_begin == 0 && allocation_bytes == 0);
    const uint64_t failed_generation = Vm::GetMutationGeneration();
    const uint64_t failed_commits = g_fex_commits.load();
    const uint64_t failed_closes = g_fex_closes.load();
    const uint64_t failed_notifications = g_fex_fresh_notifications.load();
    CHECK(!Vm::Free(mapping + host_page));
    CHECK(Vm::GetMutationGeneration() == failed_generation &&
          g_fex_commits.load() == failed_commits && g_fex_closes.load() == failed_closes + 1 &&
          g_fex_fresh_notifications.load() == failed_notifications &&
          g_fex_last_base.load() == 0 && g_fex_last_size.load() == 0 &&
          g_fex_active.load() == 0 && g_fex_mutation_depth == 0);
    CHECK(IsMode(mapping, modes[0]) && IsMode(mapping + host_page, modes[1]) &&
          IsMode(mapping + 2 * host_page, modes[2]));

    Vm::GuestDataRangeSeal freed_seal, unrelated_seal;
    const auto capture = [](uint64_t address, Vm::GuestDataRangeSeal* seal) {
        g_fex_host_reader = true;
        bool captured = false;
        {
            Vm::GuestDataLease lease;
            captured = Vm::TryAcquireGuestDataLease(address, 16, Vm::Mode::ReadWrite, &lease) &&
                Vm::CaptureGuestDataRangeSeal(lease, seal);
        }
        g_fex_host_reader = false;
        return captured;
    };
    const auto validate = [](uint64_t address, const Vm::GuestDataRangeSeal& seal) {
        g_fex_host_reader = true;
        bool valid = false;
        {
            Vm::GuestDataLease lease;
            valid = Vm::TryAcquireGuestDataLease(address, 16, Vm::Mode::ReadWrite, &lease) &&
                Vm::ValidateGuestDataRangeSeal(lease, seal);
        }
        g_fex_host_reader = false;
        return valid;
    };
    CHECK(capture(mapping + 2 * host_page + 17, &freed_seal));
    CHECK(capture(unrelated + 17, &unrelated_seal));
    const uint64_t generation = Vm::GetMutationGeneration();
    const uint64_t commits = g_fex_commits.load();
    const uint64_t notifications = g_fex_fresh_notifications.load();
    CHECK(Vm::Free(mapping));
    CHECK(Vm::GetMutationGeneration() == generation + 1 && g_fex_commits.load() == commits + 1 &&
          g_fex_last_begin_kind.load() == 0 && g_fex_active.load() == 0 && g_fex_mutation_depth == 0);
    CHECK(g_fex_last_base.load() == mapping && g_fex_last_size.load() == bytes);
    CHECK(g_fex_fresh_notifications.load() == notifications + 1 &&
          g_fex_fresh_base.load() == mapping && g_fex_fresh_size.load() == bytes &&
          g_fex_fresh_kind.load() == 0 && !g_fex_fresh_without_mutation.load());
    for (uint64_t run = 0; run < 3; ++run) CHECK(!Vm::IsManagedAddress(mapping + run * host_page));
    CHECK(validate(unrelated + 17, unrelated_seal));
    CHECK(Vm::AllocFixed(mapping, bytes, Vm::Mode::ReadWrite));
    CHECK(!validate(mapping + 2 * host_page + 17, freed_seal) &&
          validate(unrelated + 17, unrelated_seal));
    Vm::ReleaseGuestDataRangeSeal(&freed_seal);
    Vm::ReleaseGuestDataRangeSeal(&unrelated_seal);
    CHECK(Vm::Free(mapping) && Vm::Free(unrelated));

    // Logical allocation bytes, rather than host-page padding, remain the
    // exact extent; FEX itself rounds the invalidation to guest code pages.
    const uint64_t small = Vm::Alloc(0, 1, Vm::Mode::ReadWrite);
    CHECK(small != 0 && Vm::Free(small));
    CHECK(g_fex_last_base.load() == small && g_fex_last_size.load() == 1 &&
          g_fex_fresh_base.load() == small && g_fex_fresh_size.load() == 1);
    std::printf("ANDROID_VM_FREE_INVALIDATION_TEST allocationRuns=3 exactBytes=%" PRIu64
                " interiorQueryRefused=1 failedFreeNoCommit=1 lifetimeNotifyExact=1 unrelatedSealRetained=1 reusedSealRevoked=1 paddingBytes=1 admission=modeled\n", bytes);
    return true;
}

bool GuestDataRangeSealContract(uint64_t host_page)
{
    const uint64_t mapping = Vm::Alloc(0, 3 * host_page, Vm::Mode::ReadWrite);
    const uint64_t unrelated = Vm::Alloc(0, 2 * host_page, Vm::Mode::ReadWrite);
    CHECK(mapping != 0 && unrelated != 0);
    struct HostReaderScope
    {
        HostReaderScope() { g_fex_host_reader = true; }
        ~HostReaderScope()
        {
            if (g_fex_guest_data_leases != 0) std::abort();
            g_fex_host_reader = false;
        }
    } reader;
    const uint64_t begin = mapping + 17;
    constexpr uint64_t bytes = 48;
    Vm::GuestDataRangeSeal seal;
    const auto capture = [&](uint64_t address, Vm::GuestDataRangeSeal* target) {
        Vm::GuestDataLease lease;
        return Vm::TryAcquireGuestDataLease(address, bytes, Vm::Mode::ReadWrite, &lease) &&
            Vm::CaptureGuestDataRangeSeal(lease, target);
    };
    const auto validate = [&](uint64_t address, const Vm::GuestDataRangeSeal& target) {
        Vm::GuestDataLease lease;
        return Vm::TryAcquireGuestDataLease(address, bytes, Vm::Mode::ReadWrite, &lease) &&
            Vm::ValidateGuestDataRangeSeal(lease, target);
    };
    Vm::GuestDataLease inactive;
    CHECK(!Vm::CaptureGuestDataRangeSeal(inactive, &seal) && !seal.IsActive());
    CHECK(capture(begin, &seal) && g_fex_guest_data_leases == 0);
    CHECK(!Vm::ValidateGuestDataRangeSeal(inactive, seal) && validate(begin, seal));
    CHECK(!validate(begin + 1, seal));
    {
        Vm::GuestDataLease lease;
        CHECK(Vm::TryAcquireGuestDataLease(begin, bytes, Vm::Mode::ReadWrite, &lease));
        CHECK(!Vm::CaptureGuestDataRangeSeal(lease, &seal));
        std::atomic<bool> rejected {false};
        std::thread foreign([&] {
            Vm::GuestDataRangeSeal foreign_seal;
            rejected.store(!Vm::CaptureGuestDataRangeSeal(lease, &foreign_seal) &&
                           !Vm::ValidateGuestDataRangeSeal(lease, seal));
        });
        foreign.join();
        CHECK(rejected.load());
    }
    const uint64_t generation = Vm::GetMutationGeneration();
    CHECK(Vm::Protect(unrelated, host_page, Vm::Mode::Read));
    CHECK(Vm::Protect(unrelated, host_page, Vm::Mode::ReadWrite));
    CHECK(Vm::Free(unrelated) && Vm::GetMutationGeneration() > generation && validate(begin, seal));
    CHECK(Vm::Protect(mapping + 2 * host_page, host_page, Vm::Mode::Read));
    CHECK(Vm::Protect(mapping + 2 * host_page, host_page, Vm::Mode::ReadWrite) && validate(begin, seal));

    Vm::GuestDataRangeSeal sibling;
    CHECK(capture(mapping + kGuestPage + 17, &sibling));
    CHECK(Vm::Protect(begin, bytes, Vm::Mode::ReadWrite));
    CHECK(!validate(begin, seal));
    CHECK(validate(mapping + kGuestPage + 17, sibling) == (host_page == kGuestPage));
    Vm::ReleaseGuestDataRangeSeal(&sibling);
    Vm::ReleaseGuestDataRangeSeal(&seal);
    CHECK(!seal.IsActive() && !validate(begin, seal));
    Vm::ReleaseGuestDataRangeSeal(&seal);

    // Real label caches may retain more than one registry block of detached
    // lifetime seals. Grow without revoking older tickets or leaking admission.
    std::array<Vm::GuestDataRangeSeal, 4097> capacity;
    {
        Vm::GuestDataLease lease;
        CHECK(Vm::TryAcquireGuestDataLease(begin, bytes, Vm::Mode::ReadWrite, &lease));
        for (auto& ticket: capacity) CHECK(Vm::CaptureGuestDataRangeSeal(lease, &ticket));
        CHECK(Vm::CaptureGuestDataRangeSeal(lease, &seal) && seal.IsActive());
        for (const auto& ticket: capacity) CHECK(Vm::ValidateGuestDataRangeSeal(lease, ticket));
        Vm::ReleaseGuestDataRangeSeal(&capacity[31]);
        CHECK(Vm::CaptureGuestDataRangeSeal(lease, &capacity[31]));
        CHECK(!Vm::CaptureGuestDataRangeSeal(lease, &seal));
        // Recycle interleaved slots in reverse order while their neighbours
        // remain live. A double release must never add the same slot twice.
        for (size_t index = 1; index < capacity.size(); index += 2) {
            Vm::ReleaseGuestDataRangeSeal(&capacity[index]);
            Vm::ReleaseGuestDataRangeSeal(&capacity[index]);
        }
        for (size_t index = 0; index < capacity.size(); index += 2)
            CHECK(Vm::ValidateGuestDataRangeSeal(lease, capacity[index]));
        for (size_t index = capacity.size() - 2;; index -= 2) {
            CHECK(Vm::CaptureGuestDataRangeSeal(lease, &capacity[index]));
            if (index == 1) break;
        }
        for (const auto& ticket: capacity) CHECK(Vm::ValidateGuestDataRangeSeal(lease, ticket));
    }
    for (auto& ticket: capacity) Vm::ReleaseGuestDataRangeSeal(&ticket);
    CHECK(validate(begin, seal));
    CHECK(Vm::Protect(mapping, host_page, Vm::Mode::Read));
    CHECK(Vm::Protect(mapping, host_page, Vm::Mode::ReadWrite) && !validate(begin, seal));
    // Revocation keeps the old ticket owned until Release. A new capture of
    // the same range must survive releasing and recapturing that old owner.
    CHECK(seal.IsActive() && !capture(begin, &seal));
    Vm::GuestDataRangeSeal renewed;
    CHECK(capture(begin, &renewed) && validate(begin, renewed));
    Vm::ReleaseGuestDataRangeSeal(&seal);
    CHECK(validate(begin, renewed) && capture(begin, &seal));
    CHECK(validate(begin, seal) && validate(begin, renewed));
    Vm::ReleaseGuestDataRangeSeal(&renewed);

    const uint64_t before_failure = Vm::GetMutationGeneration();
    g_partial_protect_failure = {
        reinterpret_cast<void*>(mapping), static_cast<size_t>(2 * host_page),
        static_cast<size_t>(host_page), true, false};
    CHECK(!Vm::Protect(mapping, 2 * host_page, Vm::Mode::Read));
    CHECK(g_partial_protect_failure.fired && Vm::GetMutationGeneration() == before_failure &&
          IsMode(mapping, Vm::Mode::ReadWrite) &&
          PhysicalProtection(mapping) == (PROT_READ | PROT_WRITE) && !validate(begin, seal));
    Vm::ReleaseGuestDataRangeSeal(&seal);
    CHECK(capture(begin, &seal));
    CHECK(Vm::Free(mapping) && Vm::AllocFixed(mapping, 3 * host_page, Vm::Mode::ReadWrite));
    CHECK(!validate(begin, seal));
    Vm::ReleaseGuestDataRangeSeal(&seal);
    CHECK(capture(begin, &seal));
    CHECK(Vm::PatchReplace(mapping + 24, UINT64_C(0xaabbccddeeff0011)) && !validate(begin, seal));
    Vm::ReleaseGuestDataRangeSeal(&seal);
    CHECK(Vm::Free(mapping));

    std::array<uint8_t, 64> native {};
    {
        Vm::GuestDataLease lease;
        CHECK(Vm::TryAcquireGuestDataLease(reinterpret_cast<uint64_t>(native.data()), native.size(),
                                          Vm::Mode::ReadWrite, &lease));
        CHECK(!Vm::CaptureGuestDataRangeSeal(lease, &seal));
    }
    Vm::ReservationHandle reservation;
    {
        Vm::MutationTransaction transaction;
        CHECK(Vm::BeginReserveAligned(0, 2 * host_page, host_page, &reservation, &transaction));
        transaction.Commit();
    }
    const uint64_t child = reservation.base;
    CHECK(Vm::AllocFixed(child, 2 * host_page, Vm::Mode::ReadWrite));
    CHECK(capture(child + 17, &seal));
    CHECK(Vm::Free(child) && Vm::AllocFixed(child, 2 * host_page, Vm::Mode::ReadWrite));
    CHECK(!validate(child + 17, seal));
    Vm::ReleaseGuestDataRangeSeal(&seal);
    CHECK(capture(child + 17, &seal));
    const uint64_t before_reserved_failure = Vm::GetMutationGeneration();
    g_partial_protect_failure = {
        reinterpret_cast<void*>(child), static_cast<size_t>(2 * host_page),
        static_cast<size_t>(host_page), true, false};
    CHECK(!Vm::Protect(child, 2 * host_page, Vm::Mode::Read));
    CHECK(g_partial_protect_failure.fired && Vm::GetMutationGeneration() == before_reserved_failure &&
          IsMode(child, Vm::Mode::ReadWrite) && !validate(child + 17, seal));
    Vm::ReleaseGuestDataRangeSeal(&seal);
    CHECK(capture(child + 17, &seal));
    CHECK(Vm::ReleaseReservation(reservation));
    {
        Vm::ReservationHandle replacement;
        Vm::MutationTransaction transaction;
        CHECK(Vm::BeginReserveAligned(child, 2 * host_page, host_page, &replacement, &transaction,
                                      Vm::ReservationPlacement::Exact));
        transaction.Commit();
        CHECK(Vm::AllocFixed(child, 2 * host_page, Vm::Mode::ReadWrite));
        CHECK(!validate(child + 17, seal) && Vm::ReleaseReservation(replacement));
    }
    Vm::ReleaseGuestDataRangeSeal(&seal);
    CHECK(g_fex_guest_data_leases == 0);
    return true;
}

struct SharedBackingFixture
{
    uint64_t first = 0, second = 0, bytes = 0;
    int fd = -1;
    uint64_t identity = UINT64_C(0x400000000000001f);
    uint64_t reads = 0, writes = 0;
    bool permit = true;
};
SharedBackingFixture g_shared_backing;

bool SharedBackingQuery(uint64_t address, uint64_t maximum, Vm::GuestBackingSpan* span) noexcept
{
    for (auto base : {g_shared_backing.first, g_shared_backing.second})
    {
        if (base == 0 || address < base || address - base >= g_shared_backing.bytes) continue;
        *span = {g_shared_backing.identity, address - base, address,
            std::min(maximum, g_shared_backing.bytes - (address - base)), Vm::GetMutationGeneration()};
        return true;
    }
    return false;
}
bool SharedBackingMark(uint64_t address, uint64_t bytes, uint32_t watch_intents) noexcept
{
    if (watch_intents < 1u || watch_intents > 3u) return false;
    Vm::GuestBackingSpan span;
    if (!SharedBackingQuery(address, bytes, &span) || span.size != bytes) return false;
    for (auto base : {g_shared_backing.first, g_shared_backing.second})
        Lumen::GuestCpu::MarkHandoffRange(base + span.offset, span.size,
            static_cast<Lumen::GuestCpu::MemoryAccessIntent>(watch_intents));
    return true;
}
Vm::GuestBackingPresence SharedBackingPresence(uint64_t identity, uint64_t offset, uint64_t bytes) noexcept
{
    using Presence = Vm::GuestBackingPresence;
    if (identity != g_shared_backing.identity || bytes == 0 || offset > UINT64_MAX - bytes)
        return Presence::Unknown;
    for (auto base : {g_shared_backing.first, g_shared_backing.second})
    {
        if (base == 0 || offset >= g_shared_backing.bytes) continue;
        const uint64_t end = base + std::min(g_shared_backing.bytes, offset + bytes);
        uint64_t address = base + offset;
        while (address < end)
        {
            Vm::ManagedRegion region {};
            if (Vm::QueryManagedRegion(address, &region) == Vm::ManagedRegionState::Visible)
                return region.begin <= address && region.end > address ? Presence::Live : Presence::Unknown;
            address += std::min(end - address, UINT64_C(4096) - address % UINT64_C(4096));
        }
    }
    return Presence::Retired;
}
bool SharedBackingAccess(uint64_t address, uint64_t bytes, Lumen::GuestCpu::MemoryAccessIntent intent) noexcept
{
    Vm::GuestBackingSpan span;
    // Conservative hash collisions must not confer an alias relationship.
    if (!SharedBackingQuery(address, bytes, &span)) return true;
    if (Lumen::GuestCpu::WritesMemory(intent)) ++g_shared_backing.writes;
    else ++g_shared_backing.reads;
    return g_shared_backing.permit;
}
bool SharedBackingGpuCopy(uint64_t address, void* buffer, uint64_t bytes, bool write) noexcept
{
    // Model only the producer's existing caller admission, independently of
    // CPU VA permissions. Kernel ownership keeps this fd live through a copy.
    if ((!g_fex_host_reader && g_fex_mutation_depth == 0 && g_native_data_token == 0) ||
        g_shared_backing.fd < 0 || buffer == nullptr || bytes == 0 || address > UINT64_MAX - bytes) return false;
    Vm::GuestBackingSpan span;
    if (!SharedBackingQuery(address, bytes, &span) || span.size != bytes ||
        span.offset > static_cast<uint64_t>((std::numeric_limits<off_t>::max)())) return false;
    for (uint64_t cursor = address; cursor < address + bytes;)
    {
        Vm::ManagedRegion region {};
        if (Vm::QueryManagedRegion(cursor, &region) != Vm::ManagedRegionState::Visible ||
            region.begin > cursor || region.end <= cursor) return false;
        cursor = std::min(address + bytes, region.end);
    }
    size_t copied = 0;
    while (copied != bytes)
    {
        const ssize_t result = write
            ? pwrite(g_shared_backing.fd, static_cast<const uint8_t*>(buffer) + copied,
                     static_cast<size_t>(bytes - copied), static_cast<off_t>(span.offset + copied))
            : pread(g_shared_backing.fd, static_cast<uint8_t*>(buffer) + copied,
                    static_cast<size_t>(bytes - copied), static_cast<off_t>(span.offset + copied));
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return false;
        copied += static_cast<size_t>(result);
    }
    return true;
}

bool MemoryHandoffBackingContract(uint64_t host_page)
{
    using namespace Lumen::GuestCpu;
    {
        Vm::ReservationHandle reservation;
        Vm::MutationTransaction transaction;
        CHECK(Vm::BeginReserveAligned(0, 2 * host_page, host_page, &reservation, &transaction,
            Vm::ReservationPlacement::PreferAddress));
        transaction.Commit();
        const int reserved_fd = static_cast<int>(syscall(SYS_memfd_create, "lumen-handoff-reserved", 0));
        CHECK(reserved_fd >= 0 && ftruncate(reserved_fd, static_cast<off_t>(2 * host_page)) == 0);
        Vm::MutationTransaction mapping_transaction;
        CHECK(Vm::BeginAllocFixedInReservation(reservation.base, 2 * host_page, Vm::Mode::ReadWrite,
            &mapping_transaction) == Vm::ReservedFixedResult::Begun);
        CHECK(Vm::BindFreshGuestMappingToSharedBacking(reservation.base, 2 * host_page, reserved_fd, 0, Vm::Mode::ReadWrite));
        const uint64_t reserved_value = UINT64_C(0xfeedbeef00112233);
        CHECK(pwrite(reserved_fd, &reserved_value, sizeof(reserved_value), 32) == sizeof(reserved_value));
        mapping_transaction.Commit();
        Vm::GuestBackingSpan reserved;
        CHECK(Vm::QueryGuestBackingSpan(reservation.base + 17, 8, &reserved));
        CHECK((reserved.identity & (UINT64_C(1) << 63)) != 0);
        CHECK(Vm::QueryGuestBackingPresence(reserved.identity, 17, 8) == Vm::GuestBackingPresence::Live);
        CHECK(Vm::Free(reservation.base));
        CHECK(Vm::QueryGuestBackingPresence(reserved.identity, 17, 8) == Vm::GuestBackingPresence::Retired);
        uint64_t reserved_read = 0;
        CHECK(pread(reserved_fd, &reserved_read, sizeof(reserved_read), 32) == sizeof(reserved_read) && reserved_read == reserved_value);
        // A retired reserved child becomes an anonymous hole, not an orphaned
        // shared VMA that preserves the old physical file after fd retirement.
        CHECK(Vm::AllocFixed(reservation.base, 2 * host_page, Vm::Mode::ReadWrite));
        g_fex_host_reader = true;
        CHECK(Vm::TryReadGuestData(reservation.base + 32, &reserved_read, sizeof(reserved_read)) && reserved_read == 0);
        g_fex_host_reader = false;
        CHECK(Vm::Free(reservation.base));
        CHECK(Vm::ReleaseReservation(reservation));
        close(reserved_fd);
    }
    const uint64_t first = Vm::Alloc(0, 2 * host_page, Vm::Mode::ReadWrite);
    const uint64_t second = Vm::Alloc(0, 2 * host_page, Vm::Mode::ReadWrite);
    CHECK(first != 0 && second != 0);
    Vm::GuestBackingSpan original, split, unrelated, retired;
    CHECK(Vm::QueryGuestBackingSpan(first + 17, 2 * host_page, &original));
    CHECK(original.offset == 17 && original.size == 2 * host_page - 17 && original.identity != 0);
    CHECK(Vm::QueryGuestBackingPresence(original.identity, 17, 8) == Vm::GuestBackingPresence::Live);
    CHECK(Vm::QueryGuestBackingPresence(0, 0, 8) == Vm::GuestBackingPresence::Unknown);
    CHECK(Vm::QueryGuestBackingPresence(UINT64_MAX, 0, 8) == Vm::GuestBackingPresence::Unknown);
    CHECK(Vm::QueryGuestBackingPresence(original.identity, UINT64_MAX - 2, 8) == Vm::GuestBackingPresence::Unknown);
    CHECK(Vm::QueryGuestBackingSpan(second + 17, 7, &unrelated) && unrelated.identity != original.identity);
    CHECK(Vm::Protect(first + host_page, host_page, Vm::Mode::Read));
    CHECK(Vm::QueryGuestBackingSpan(first + host_page + 23, 8, &split) &&
        split.identity == original.identity && split.offset == host_page + 23);
    CHECK(Vm::Protect(first + host_page, host_page, Vm::Mode::ReadWrite));
    CHECK(!Vm::QueryGuestBackingSpan(UINT64_MAX - 2, 8, &retired) && retired.identity == 0);

    const int fd = static_cast<int>(syscall(SYS_memfd_create, "lumen-handoff-fixture", 0));
    CHECK(fd >= 0 && ftruncate(fd, static_cast<off_t>(2 * host_page)) == 0);
    CHECK(Vm::BindFreshGuestMappingToSharedBacking(first, 2 * host_page, fd, 0, Vm::Mode::ReadWrite));
    CHECK(Vm::BindFreshGuestMappingToSharedBacking(second, 2 * host_page, fd, 0, Vm::Mode::ReadWrite));
    g_shared_backing.first = first;
    g_shared_backing.second = second;
    g_shared_backing.bytes = 2 * host_page;
    g_shared_backing.fd = fd;
    CHECK(Vm::RegisterGuestBackingOwner(SharedBackingQuery, SharedBackingMark, SharedBackingPresence, SharedBackingGpuCopy));
    CHECK(Vm::RegisterGuestBackingOwner(SharedBackingQuery, SharedBackingMark, SharedBackingPresence, SharedBackingGpuCopy));
    CHECK(!Vm::RegisterGuestBackingOwner(SharedBackingQuery, SharedBackingMark, SharedBackingPresence));
    CHECK(!Vm::RegisterGuestBackingOwner(SharedBackingQuery, SharedBackingMark)); // Immutable callback set.
    CHECK(RegisterMemoryHandoffQuery(SharedBackingAccess));
    CHECK(Vm::MarkGuestBackingHandoffRange(first + 32, 8));
    CHECK(NeedsHandoff(first + 32, 8) && NeedsHandoff(second + 32, 8));
    CHECK(Vm::QueryGuestBackingSpan(second + 32, 8, &split) && split.identity == g_shared_backing.identity && split.offset == 32);
    g_fex_host_reader = true;
    uint64_t value = UINT64_C(0xa1302948675cbdf0), read = 0;
    CHECK(Vm::TryWriteGuestData(first + 32, &value, sizeof(value)));
    CHECK(Vm::TryWriteGuestData(first + 32, &value, sizeof(value)));
    CHECK(g_shared_backing.writes == 2); // Identical bytes still constitute a write.
    CHECK(Vm::TryReadGuestData(second + 32, &read, sizeof(read)) && read == value && g_shared_backing.reads == 1);
    {
        Vm::GuestDataLease persistent;
        CHECK(Vm::TryAcquireGuestDataLease(second + 32, 8, Vm::Mode::ReadWrite, &persistent));
        CHECK(persistent.IsActive() && g_fex_guest_data_leases == 1 && g_shared_backing.writes == 3);
        CHECK(*reinterpret_cast<uint64_t*>(persistent.HostBegin()) == value);
    }
    CHECK(g_fex_guest_data_leases == 0);
    g_shared_backing.permit = false;
    read = 71;
    Vm::GuestDataLeaseDenialMetadata denial;
    CHECK(!Vm::TryReadGuestData(second + 32, &read, 8, &denial) && read == 71 &&
        denial.reason == Vm::GuestDataLeaseDenialReason::DataHandoffDenied);
    Vm::GuestDataLease denied;
    CHECK(!Vm::TryAcquireGuestDataLease(second + 32, 8, Vm::Mode::Read, &denied, true, &denial) &&
        !denied.IsActive() && g_fex_guest_data_leases == 0 && denial.reason == Vm::GuestDataLeaseDenialReason::DataHandoffDenied);
    std::array<uint8_t, 1024> private_copy;
    private_copy.fill(0xc7);
    const auto before_private_denial = g_copied_read_calls.load(std::memory_order_relaxed);
    CHECK(Vm::CopyGuestDataToPrivateScratch(second + 32, private_copy.data(), private_copy.size(), &denial) ==
          Vm::PrivateGuestDataCopyResult::Denied && denial.reason == Vm::GuestDataLeaseDenialReason::DataHandoffDenied &&
          g_copied_read_calls.load(std::memory_order_relaxed) == before_private_denial && g_fex_guest_data_leases == 0 &&
          std::all_of(private_copy.begin(), private_copy.end(), [](uint8_t value) { return value == 0xc7; }));
    g_fex_host_reader = false;
    g_shared_backing.permit = true;
    std::array<uint8_t, 48> sentinels;
    sentinels.fill(0xa5);
    CHECK(pwrite(fd, sentinels.data(), sentinels.size(), static_cast<off_t>(host_page + 16)) ==
          static_cast<ssize_t>(sentinels.size()));
    CHECK(Vm::Protect(first, 2 * host_page, Vm::Mode::Read));
    CHECK(Vm::Protect(second, 2 * host_page, Vm::Mode::NoAccess));
    const uint64_t protected_generation = Vm::GetMutationGeneration();
    const uint64_t cpu_writes = g_shared_backing.writes, cpu_reads = g_shared_backing.reads;
    std::array<uint8_t, 11> producer, copied;
    producer.fill(0x5b);
    copied.fill(0xc7);
    CHECK(!Vm::CopyGuestBackingDataForGpu(first + host_page + 29, producer.data(), producer.size(), true));
    g_fex_host_reader = true;
    CHECK(!Vm::TryWriteGuestData(first + host_page + 29, producer.data(), producer.size()));
    CHECK(!Vm::TryReadGuestData(second + host_page + 29, copied.data(), copied.size()));
    CHECK(Vm::CopyGuestBackingDataForGpu(first + host_page + 29, producer.data(), producer.size(), true));
    CHECK(Vm::CopyGuestBackingDataForGpu(second + host_page + 29, copied.data(), copied.size(), false));
    CHECK(copied == producer && g_fex_guest_data_leases == 0 &&
          g_shared_backing.writes == cpu_writes && g_shared_backing.reads == cpu_reads);
    g_shared_backing.fd = -1; // Fallback checks exact canonical CPU rights.
    CHECK(!Vm::CopyGuestBackingDataForGpu(first + host_page + 29, producer.data(), producer.size(), true));
    copied.fill(0xc7);
    CHECK(!Vm::CopyGuestBackingDataForGpu(second + host_page + 29, copied.data(), copied.size(), false));
    CHECK(std::all_of(copied.begin(), copied.end(), [](uint8_t byte) { return byte == 0xc7; }));
    g_shared_backing.fd = fd;
    CHECK(!Vm::CopyGuestBackingDataForGpu(0, producer.data(), producer.size(), true));
    CHECK(!Vm::CopyGuestBackingDataForGpu(first, nullptr, producer.size(), true));
    CHECK(!Vm::CopyGuestBackingDataForGpu(first, producer.data(), 0, true));
    CHECK(!Vm::CopyGuestBackingDataForGpu(UINT64_MAX - 3, producer.data(), producer.size(), true));
    g_fex_host_reader = false;
    CHECK(pread(fd, sentinels.data(), sentinels.size(), static_cast<off_t>(host_page + 16)) ==
          static_cast<ssize_t>(sentinels.size()));
    for (size_t index = 0; index < sentinels.size(); ++index)
        CHECK(sentinels[index] == (index >= 13 && index < 24 ? 0x5b : 0xa5));
    Vm::ManagedRegion readonly, inaccessible;
    CHECK(Vm::GetMutationGeneration() == protected_generation &&
          Vm::QueryManagedRegion(first, &readonly) == Vm::ManagedRegionState::Visible && readonly.mode == Vm::Mode::Read &&
          Vm::QueryManagedRegion(second, &inaccessible) == Vm::ManagedRegionState::Visible && inaccessible.mode == Vm::Mode::NoAccess &&
          PhysicalProtection(first) == PROT_READ && PhysicalProtection(second) == PROT_NONE);
    const auto shared_identity = g_shared_backing.identity;
    CHECK(Vm::QueryGuestBackingPresence(shared_identity, 32, 8) == Vm::GuestBackingPresence::Live);
    CHECK(Vm::Free(first));
    CHECK(Vm::QueryGuestBackingPresence(shared_identity, 32, 8) == Vm::GuestBackingPresence::Live);
    CHECK(Vm::QueryGuestBackingPresence(original.identity, 17, 8) == Vm::GuestBackingPresence::Retired);
    CHECK(Vm::Free(second));
    CHECK(Vm::QueryGuestBackingPresence(shared_identity, 32, 8) == Vm::GuestBackingPresence::Retired);
    g_shared_backing = {};
    CHECK(!Vm::QueryGuestBackingSpan(first, 8, &retired) && retired.identity == 0);
    CHECK(Vm::AllocFixed(first, 2 * host_page, Vm::Mode::ReadWrite));
    CHECK(Vm::BindFreshGuestMappingToSharedBacking(first, 2 * host_page, fd, 0, Vm::Mode::ReadWrite));
    CHECK(Vm::QueryGuestBackingSpan(first + 17, 8, &retired) && retired.identity != original.identity && retired.offset == 17);
    CHECK(pread(fd, &read, sizeof(read), 32) == sizeof(read) && read == value);
    read = 0;
    g_fex_host_reader = true;
    CHECK(Vm::TryReadGuestData(first + 32, &read, sizeof(read)) && read == value);
    g_fex_host_reader = false;
    CHECK(Vm::Free(first));
    const uint64_t partial_value = UINT64_C(0x0123456789abcdef);
    CHECK(pwrite(fd, &partial_value, sizeof(partial_value), static_cast<off_t>(host_page + 32)) == sizeof(partial_value));
    const auto partial = Vm::Alloc(0, host_page, Vm::Mode::Read);
    CHECK(partial != 0);
    CHECK(Vm::BindFreshGuestMappingToSharedBacking(partial, host_page, fd, host_page, Vm::Mode::Read));
    CHECK(!Vm::BindFreshGuestMappingToSharedBacking(partial, host_page, fd, host_page, Vm::Mode::ReadWrite));
    CHECK(!Vm::BindFreshGuestMappingToSharedBacking(partial + 1, host_page, fd, host_page, Vm::Mode::Read));
    read = 0;
    g_fex_host_reader = true;
    CHECK(Vm::TryReadGuestData(partial + 32, &read, sizeof(read)) && read == partial_value);
    g_fex_host_reader = false;
    Vm::ManagedRegion partial_region {};
    CHECK(Vm::QueryManagedRegion(partial, &partial_region) == Vm::ManagedRegionState::Visible && partial_region.mode == Vm::Mode::Read);
    CHECK(Vm::Free(partial));
    close(fd);
    return true;
}

bool MemoryHandoffUnknownOwnerContract(uint64_t host_page)
{
    const auto address = Vm::Alloc(0, host_page, Vm::Mode::ReadWrite);
    CHECK(address != 0);
    g_shared_backing.first = address;
    g_shared_backing.bytes = host_page;
    // The legacy two-callback registration is still accepted, but it provides
    // no retirement authority even when its canonical address disappears.
    CHECK(Vm::RegisterGuestBackingOwner(SharedBackingQuery, SharedBackingMark));
    CHECK(Vm::QueryGuestBackingPresence(g_shared_backing.identity, 0, 8) == Vm::GuestBackingPresence::Unknown);
    CHECK(Vm::Free(address));
    CHECK(Vm::QueryGuestBackingPresence(g_shared_backing.identity, 0, 8) == Vm::GuestBackingPresence::Unknown);
    g_shared_backing = {};
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    const bool mte_route = argc == 3 && std::strcmp(argv[2], "--guest-data-read-mte-only") == 0;
    const bool unknown_capability_route = argc == 3 &&
        std::strcmp(argv[2], "--guest-data-read-no-hwcap-only") == 0;
    if (mte_route) g_copied_read_capability = CopiedReadCapability::Mte;
    if (unknown_capability_route) g_copied_read_capability = CopiedReadCapability::Unavailable;
    const long detected = sysconf(_SC_PAGESIZE);
    if ((detected != 4096 && detected != 16384) ||
        (argc >= 2 && std::strtol(argv[1], nullptr, 10) != detected))
    {
        std::fprintf(stderr, "FAIL: unexpected host page size %ld\n", detected);
        return 1;
    }
    Vm::Init();
    const auto host_page = static_cast<uint64_t>(detected);
    if (argc == 3 && std::strcmp(argv[2], "--mutation-owner-only") == 0)
    {
        const bool pass = GuestMemoryMutationOwnerContract(host_page);
        std::printf("%s: Android Core exclusive owner adoption exact/foreign/stale/additive/unadopted hostPage=%ld admission=modeled\n",
            pass ? "PASS" : "FAIL", detected);
        return pass ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[2], "--range-seal-only") == 0)
    {
        const bool pass = GuestDataRangeSealContract(host_page);
        std::printf("%s: Android exact range seals simultaneous=4097 reusable/double-release/interleaved/protect/remap/foreign/rights hostPage=%ld admission=modeled\n",
            pass ? "PASS" : "FAIL", detected);
        return pass ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[2], "--free-invalidation-only") == 0)
    {
        const bool pass = FreeAllocationInvalidationContract(host_page);
        std::printf("%s: Android complete-allocation free invalidation hostPage=%ld admission=modeled\n",
                    pass ? "PASS" : "FAIL", detected);
        return pass ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[2], "--memory-handoff-owner-unknown-only") == 0)
    {
        const bool pass = MemoryHandoffUnknownOwnerContract(host_page);
        std::printf("%s: Android unknown backing owner cannot prove retirement hostPage=%ld\n",
            pass ? "PASS" : "FAIL", detected);
        return pass ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[2], "--memory-handoff-only") == 0)
    {
        const bool pass = MemoryHandoffBackingContract(host_page);
        std::printf("%s: Android physical backing aliases/reuse/split/same-value/HLE/GPU-owner-copy RO/NONE/exact-interval handoff hostPage=%ld\n",
            pass ? "PASS" : "FAIL", detected);
        return pass ? 0 : 1;
    }
    if (mte_route || unknown_capability_route)
    {
        const bool pass = GuestDataNativeRouteContract(host_page);
        std::printf("%s: Android guest native read/write hostPage=%ld capability=%s logicalTag=0 kernelCopy=0 admission=modeled\n",
                    pass ? "PASS" : "FAIL", detected, mte_route ? "mte-modeled" : "unavailable-modeled");
        return pass ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[2], "--guest-data-query-only") == 0)
    {
        const bool pass = GuestExecutionLogicalRightsContract(host_page) && GuestDataBoundedRangeContract(host_page);
        std::printf("%s: Android x86 data rights hostPage=%ld admission=modeled nativeExecute=0\n",
                    pass ? "PASS" : "FAIL", detected);
        return pass ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[2], "--guest-data-query-benchmark") == 0)
    {
        const bool pass = GuestDataBoundedRangeContract(host_page) && GuestDataQueryBenchmark(host_page);
        return pass ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[2], "--guest-data-read-only") == 0)
    {
        const bool pass = GuestDataReadContract(host_page) && GuestDataPrivateScratchContract(host_page);
        std::printf("%s: Android guest copied read hostPage=%ld admission=modeled kernelCopy=self privateMax=2MiB partial/protection/crosspage/prefix\n",
                    pass ? "PASS" : "FAIL", detected);
        return pass ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[2], "--guest-data-read-lease-only") == 0)
    {
        const bool pass = GuestDataBoundedReadLeaseContract(host_page);
        std::printf("%s: Android bounded read leases hostPage=%ld untaggedMax=256 physicalProof=self"
                    " revoke/partial/guard/held-admission/tagged/large/RW admission=modeled\n",
                    pass ? "PASS" : "FAIL", detected);
        return pass ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[2], "--guest-data-write-only") == 0)
    {
        const bool pass = GuestDataWriteContract(host_page);
        std::printf("%s: Android guest copied write hostPage=%ld admission=modeled kernelCopy=self partialWrite=fail-stop\n",
                    pass ? "PASS" : "FAIL", detected);
        return pass ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[2], "--guest-data-retry-only") == 0)
    {
        const bool pass = GuestDataRetryMetadataContract(host_page);
        std::printf("%s: Android guest data retry hostPage=%ld admission=modeled retryable=mutation-pending-only\n",
                    pass ? "PASS" : "FAIL", detected);
        return pass ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[2], "--allocation-retry-only") == 0)
    {
        const bool pass = AllocationFailureContract(host_page);
        std::printf("%s: Android allocation metadata hostPage=%ld admission=modeled decline=no-effects retry=caller-owned permanent=no-retry\n",
                    pass ? "PASS" : "FAIL", detected);
        return pass ? 0 : 1;
    }
    const bool ok = Vm::RequiresUniformHostPageDataProtection() == (host_page > kGuestPage) &&
        ProtectionMatrix(host_page) && AtomicRejectionAndNativeFaults(host_page) &&
        PaddingAndAllocation(host_page) && GuestWindowCollisionPlacement(host_page) &&
        PartialSyscallRollback(host_page) &&
        SparseReservationLifecycle(host_page) &&
        PatchAcrossPhysicalPages(host_page) &&
        InvalidRequests(host_page) && AllocationFailureContract(host_page) && GuestDataLeaseContract(host_page) && GuestDataReadContract(host_page) &&
        GuestDataBoundedReadLeaseContract(host_page) && GuestDataPrivateScratchContract(host_page) &&
        GuestDataWriteContract(host_page) &&
        GuestExecutionLogicalRightsContract(host_page) && GuestDataBoundedRangeContract(host_page) && NativeGuestDataLeaseContract(host_page) &&
        GuestDataRetryMetadataContract(host_page) && FreeAllocationInvalidationContract(host_page) &&
        GuestDataRangeSealContract(host_page) && GuestMemoryMutationOwnerContract(host_page);
    std::printf("%s: Android VM hostPage=%ld guestPage=4096 snapshot=%u mixed4k=%s nativeExecute=0 sparseReservation=1 guestDataLease=1 copiedRead=1 copiedWrite=1 rangeSeal=1 fexAdmission=modeled\n",
        ok ? "PASS" : "FAIL", detected, Vm::IsImmutableRegionSnapshotEnabled(),
        host_page == kGuestPage ? "supported" : "rejected");
    return ok ? 0 : 1;
}
