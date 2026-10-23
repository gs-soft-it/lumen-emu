// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
#include "QemuProvider.h"
#include "LumenQemuTcg.h"
#include "AndroidFexCoreMutationBarrier.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cfenv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <unordered_set>
#include <unistd.h>
#include <vector>

namespace Lumen::GuestCpu {
namespace {
using namespace Lumen::Android::FexCoreLab;
constexpr auto Ready = LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1;
constexpr auto Denied = LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1;
constexpr auto Invalid = LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1;
GuestMemoryMutationBarrier barrier;
std::once_flag initialize_once;
std::atomic<bool> initialized {};
std::uint32_t initialization_code {LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1};
alignas(4096) const std::array<std::uint8_t, 4096> root_sentinel {};
std::mutex runs_mutex;
std::mutex targets_mutex;
std::atomic<ImmutableCodeQuery> immutable_code_query {};
using Targets = std::unordered_set<std::uint64_t>;
std::shared_ptr<const Targets> targets {std::make_shared<const Targets>()};
std::atomic<LumenAndroidFexCoreBoundaryDeliveryV1> boundary_delivery {};
std::atomic<std::uint64_t> next_interrupt_token {1};

struct MemoryRegionCache {
    std::array<LumenQemuTcgMemoryRegion, 256> regions {};
    std::size_t count {};

    const LumenQemuTcgMemoryRegion* Find(std::uint64_t address, std::uint64_t size) const noexcept {
        std::size_t begin = 0, end = count;
        while (begin < end) {
            const auto middle = begin + (end - begin) / 2;
            if (regions[middle].guest_base <= address) begin = middle + 1;
            else end = middle;
        }
        if (begin == 0) return nullptr;
        const auto& region = regions[begin - 1];
        return address - region.guest_base < region.size &&
            size <= region.size - (address - region.guest_base) ? &region : nullptr;
    }

    void Remember(const LumenQemuTcgMemoryRegion& region) noexcept {
        // This is an optimization, not an ownership store. Deterministic VM
        // proofs recover the same extent after a bounded cache reset.
        if (count == regions.size()) Clear();
        std::size_t begin = 0, end = count;
        while (begin < end) {
            const auto middle = begin + (end - begin) / 2;
            if (regions[middle].guest_base < region.guest_base) begin = middle + 1;
            else end = middle;
        }
        if (begin < count && regions[begin].guest_base == region.guest_base) {
            regions[begin] = region;
            return;
        }
        for (auto index = count; index > begin; --index) regions[index] = regions[index - 1];
        regions[begin] = region;
        ++count;
    }

    void Clear() noexcept { count = 0; }
};

struct ActiveRun {
    ActiveRun* parent {};
    LumenQemuTcgSession* session {};
    LumenAndroidFexCoreExecutableQueryV1 executable_query {};
    void* opaque {};
    std::uint64_t owned_base {}, owned_size {};
    LumenMacOSFexCoreOwnedStackAccessQueryV1 owned_query {};
    std::shared_ptr<const Targets> host_targets;
    std::uint64_t host_thread {};
    std::uint64_t host_call_target {};
    LumenQemuTcgGuestState* state {};
    std::uint64_t fresh_callback_token {}, fresh_callback_return {};
    std::uint64_t fresh_code_base {}, fresh_code_size {};
    std::uint64_t fresh_stack_base {}, fresh_stack_size {};
    std::uint64_t fresh_tls_base {}, fresh_tls_size {};
    bool fresh_callback_lifetime_revoked {};
    bool fresh_callback_watch_active {};
    bool fresh_callback_restore_failed {};
    std::atomic<std::uint64_t> interrupt_token {};
    MemoryRegionCache memory_regions;
};
thread_local ActiveRun* active_run {};
std::vector<ActiveRun*> runs;

class HostFloatingPointEnvironment {
public:
    HostFloatingPointEnvironment() noexcept : saved(std::fegetenv(&environment) == 0) {}
    ~HostFloatingPointEnvironment() { if (saved) (void)std::fesetenv(&environment); }
    bool Valid() const noexcept { return saved; }
    const std::fenv_t* Snapshot() const noexcept { return &environment; }
    bool Restore() noexcept {
        if (!saved || std::fesetenv(&environment) != 0) return false;
        saved = false;
        return true;
    }
private:
    std::fenv_t environment {};
    bool saved;
};

template<typename Callback>
bool InvokeNativeBoundary(const std::fenv_t* native, Callback&& callback) noexcept {
    HostFloatingPointEnvironment running_environment;
    if (!running_environment.Valid() || std::fesetenv(native) != 0) return false;
    callback();
    return running_environment.Restore();
}

std::uint64_t ReturnAddress() noexcept {
    return reinterpret_cast<std::uintptr_t>(root_sentinel.data());
}

std::uint32_t Initialize() noexcept {
    std::call_once(initialize_once, [] {
        const auto result = LumenQemuTcgInitialize(256);
        initialization_code = result == LUMEN_QEMU_TCG_READY ? Ready :
            LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1;
        initialized.store(initialization_code == Ready, std::memory_order_release);
    });
    return initialization_code;
}

bool Contains(std::uint64_t base, std::uint64_t length,
              std::uint64_t address, std::uint64_t size) noexcept {
    return base != 0 && length != 0 && address >= base && size != 0 &&
        address - base < length && size <= length - (address - base);
}

bool OwnedDataAllowed(ActiveRun& run, std::uint64_t address,
                      std::uint64_t size, bool write) noexcept {
    for (auto* owner = &run; owner != nullptr; owner = owner->parent) {
        if (owner->owned_query != nullptr &&
            Contains(owner->owned_base, owner->owned_size, address, size) &&
            owner->owned_query(address, size, write ? 1U : 0U,
                owner->owned_base, owner->owned_size) != 0) return true;
    }
    return false;
}

bool DataAllowed(ActiveRun& run, std::uint64_t address,
                 std::uint64_t size, bool write) noexcept {
    if (const auto query = GetMemoryAccessQuery(); query != nullptr &&
        query(address, size, write ? 1U : 0U) != 0) return true;
    return OwnedDataAllowed(run, address, size, write);
}

constexpr std::uint64_t guest_page_size = 4096;
constexpr std::uint64_t largest_memory_region = 1024 * 1024;

std::uint32_t GlobalPagePermissions(ActiveRun& run, std::uint64_t page,
                                    MemoryAccessQuery data_query) noexcept {
    std::uint64_t code_base {}, code_size {};
    std::uint32_t writable {};
    const bool code = run.executable_query(run.opaque, page, &code_base,
        &code_size, &writable) == 1 &&
        Contains(code_base, code_size, page, guest_page_size) && writable <= 1;
    const bool read = (data_query != nullptr &&
        data_query(page, guest_page_size, 0) != 0) || code;
    const bool write = data_query != nullptr && data_query(page, guest_page_size, 1) != 0;
    return (read ? LUMEN_QEMU_TCG_READ : 0U) |
        (write ? LUMEN_QEMU_TCG_WRITE : 0U) | (code ? LUMEN_QEMU_TCG_EXECUTE : 0U);
}

bool OverlapsOwnedStack(ActiveRun& run, std::uint64_t base, std::uint64_t size) noexcept {
    for (auto* owner = &run; owner != nullptr; owner = owner->parent)
        if (owner->owned_query != nullptr && base < owner->owned_base + owner->owned_size &&
            owner->owned_base < base + size) return true;
    return false;
}

void CompleteCodeCacheProof(std::uint64_t page, std::uint32_t access,
                           LumenQemuTcgMemoryRegion* region) noexcept {
    if (access != (LUMEN_QEMU_TCG_READ | LUMEN_QEMU_TCG_EXECUTE) ||
        (region->permissions & LUMEN_QEMU_TCG_WRITE) != 0) return;
    const auto query = immutable_code_query.load(std::memory_order_acquire);
    if (query == nullptr || !query(page, guest_page_size)) return;
    // The proof covers one page. Keep the larger canonical memory-region
    // cache intact and narrow only this returned proof to its exact extent.
    region->host_base = static_cast<std::uint8_t*>(region->host_base) + (page - region->guest_base);
    region->guest_base = page;
    region->size = guest_page_size;
    region->reserved = LUMEN_QEMU_TCG_IMMUTABLE_CODE;
}

int MemoryQuery(void* opaque, std::uint64_t address, std::uint64_t size,
                std::uint32_t access, LumenQemuTcgMemoryRegion* region) noexcept {
    auto& run = *static_cast<ActiveRun*>(opaque);
    if (region == nullptr || size == 0 || address > UINT64_MAX - size ||
        access == 0 || (access & ~7U) != 0) return 0;
    // A direct TLB entry authorizes a complete guest page. Mixed/incomplete
    // pages are rejected rather than extending a smaller VM permission proof.
    const auto page = address & ~(guest_page_size - 1);
    if (size > guest_page_size || address - page > guest_page_size - size) return 0;
    if (const auto* cached = run.memory_regions.Find(page, guest_page_size)) {
        if ((cached->permissions & access) != access) return 0;
        *region = *cached;
        CompleteCodeCacheProof(page, access, region);
        return 1;
    }
    const auto data_query = GetMemoryAccessQuery();
    const auto global_permissions = GlobalPagePermissions(run, page, data_query);
    const auto permissions = global_permissions |
        (((global_permissions & LUMEN_QEMU_TCG_READ) == 0 &&
          OwnedDataAllowed(run, page, guest_page_size, false)) ? LUMEN_QEMU_TCG_READ : 0U) |
        (((global_permissions & LUMEN_QEMU_TCG_WRITE) == 0 &&
          OwnedDataAllowed(run, page, guest_page_size, true)) ? LUMEN_QEMU_TCG_WRITE : 0U);
    if ((permissions & access) != access) return 0;
    *region = {page, guest_page_size, reinterpret_cast<void*>(page), permissions, 0};
    // Owned stacks authorize their own complete pages, never adjacent global
    // memory. Their scope can also differ between nested roots.
    if (permissions != global_permissions ||
        OverlapsOwnedStack(run, page, guest_page_size)) return 1;

    // Every candidate lies inside this same aligned dyadic partition. Retain
    // the complete page mask only during this query: rejected larger spans
    // often revisit their pages while proving a smaller extent. The CPU lease
    // fixes these rights, including negative bits; no rejection survives this
    // query, a root boundary or a committed VM writer.
    constexpr std::uint8_t queried = 0x80;
    const auto partition_base = page & ~(largest_memory_region - 1);
    std::array<std::uint8_t, largest_memory_region / guest_page_size> page_masks {};
    page_masks[(page - partition_base) / guest_page_size] =
        static_cast<std::uint8_t>(global_permissions | queried);
    const auto page_permissions = [&](std::uint64_t address) noexcept {
        auto& mask = page_masks[(address - partition_base) / guest_page_size];
        if ((mask & queried) == 0)
            mask = static_cast<std::uint8_t>(GlobalPagePermissions(run, address, data_query) | queried);
        return static_cast<std::uint32_t>(mask & 7U);
    };

    // The VM lease fixes global rights until a committed writer closes the
    // epoch. Aligned dyadic partitions with the same complete R/W/X mask
    // choose identical extents on read, write and roots with unchanged rights.
    // Executable callbacks must describe these same global logical rights;
    // a root-specific code lease may narrow a view, never confer extra rights.
    for (auto span = largest_memory_region; span > guest_page_size; span /= 2) {
        const auto base = page & ~(span - 1);
        if (base > UINT64_MAX - span || OverlapsOwnedStack(run, base, span) ||
            data_query == nullptr || (permissions & LUMEN_QEMU_TCG_READ) == 0 ||
            data_query(base, span, 0) == 0 ||
            ((permissions & LUMEN_QEMU_TCG_WRITE) != 0 && data_query(base, span, 1) == 0))
            continue;
        bool uniform = true;
        for (auto check = base; check < base + span; check += guest_page_size)
            if (page_permissions(check) != permissions) {
                uniform = false; break;
            }
        // Whole-range positive R/W proofs alone cannot distinguish mixed RX
        // and RW pages. Check all negative bits too, before lending a region.
        if (uniform) {
            *region = {base, span, reinterpret_cast<void*>(base), permissions, 0};
            break;
        }
    }
    run.memory_regions.Remember(*region);
    CompleteCodeCacheProof(page, access, region);
    return 1;
}

int HostTargetQuery(void* opaque, std::uint64_t pc) noexcept {
    const auto& run = *static_cast<ActiveRun*>(opaque);
    return run.host_targets->contains(pc) ? 1 : 0;
}

bool ArmSafepoints(std::uint64_t) noexcept {
    std::lock_guard lock(runs_mutex);
    for (auto* run: runs) if (run->session != nullptr) LumenQemuTcgInterrupt(run->session);
    return true;
}
void DisarmSafepoints(std::uint64_t) noexcept {}

void CopyGp(const LumenQemuTcgGuestState& source,
            LumenAndroidFexCoreGuestStateV1& destination) noexcept {
    destination.rip = source.rip;
    std::memcpy(destination.gprs, source.gprs, sizeof(destination.gprs));
    destination.fsBase = source.fs_base;
    destination.gsBase = source.gs_base;
    destination.faultAddress = source.fault_address;
    destination.faultSignal = source.fault_exception == 6 ? SIGILL :
        source.fault_exception == 0 ? SIGFPE : SIGSEGV;
}

std::uint32_t Run(LumenAndroidFexCoreGuestStateV1* gp,
    LumenAndroidFexCoreGuestStateV3* extended,
    LumenAndroidFexCoreHostCallV1 host_v1,
    LumenAndroidFexCoreHostCallV2 host_v2,
    LumenAndroidFexCoreExecutableQueryV1 executable_query, void* opaque,
    std::uint64_t owned_base = 0, std::uint64_t owned_size = 0,
    LumenMacOSFexCoreOwnedStackAccessQueryV1 owned_query = nullptr) noexcept {
    if (gp == nullptr || gp->structSize != sizeof(*gp) || gp->abiVersion != 1 ||
        gp->rip == 0 || executable_query == nullptr || (host_v1 == nullptr && host_v2 == nullptr) ||
        (owned_query == nullptr && (owned_base != 0 || owned_size != 0)) ||
        (owned_query != nullptr && (owned_base == 0 || owned_size == 0 ||
            owned_size > UINT64_MAX - owned_base))) return Invalid;
    if (extended != nullptr && (extended->structSize != sizeof(*extended) ||
        extended->abiVersion != 3 || extended->reserved != 0 ||
        (extended->mxcsr & 0xffff0000U) != 0)) return Invalid;
    if (extended != nullptr) {
        // The legacy V3 root API never authorizes restoring opaque FEX
        // checkpoint bytes into another translator.
        for (const auto byte: extended->flags) if (byte != 0) return Invalid;
        for (const auto& reg: extended->x87Registers)
            for (const auto byte: reg) if (byte != 0) return Invalid;
        if (extended->x87AbridgedTag != 0 || extended->parityRaw != 1 ||
            extended->auxiliaryCarryRaw != 0) return Invalid;
    }
    // TCG keeps architectural guest FP controls in its own CPU state. Native
    // services still borrow the root caller's environment and may change it;
    // isolate each service, nested root and final native return as in FEX.
    HostFloatingPointEnvironment native_environment;
    if (!native_environment.Valid())
        return LUMEN_ANDROID_FEXCORE_RUNTIME_HOST_CALL_FAILED_V1;
    if (Initialize() != Ready) return initialization_code;
    GuestMemoryMutationBarrier::BarrierFailureDiagnostic admission {};
    ExecutionBarrierLease execution(barrier, &admission);
    if (!execution.Acquired()) {
        const char* trace = std::getenv("LUMEN_QEMU_TCG_FAULT_CONTEXT");
        if (trace != nullptr && std::strcmp(trace, "1") == 0) {
            std::fprintf(stderr,
                "[QEMU_ADMISSION_DENIED] rip=0x%llx phase=%u execution=%u host_call=%u "
                "host_readers_owned=%u retained=%u data_leases=%u mutation=%u "
                "host_wait=%u readers=%zu host_readers=%zu retained_total=%zu "
                "parked=%zu waiting=%zu request=%llu writer=%u full_intent=%u\n",
                static_cast<unsigned long long>(gp->rip), static_cast<unsigned>(admission.phase),
                admission.executionDepth, admission.hostCallDepth, admission.hostCallReadersOwned,
                admission.retainedExecutionDepth, admission.guestDataLeases, admission.mutationDepth,
                static_cast<unsigned>(admission.hostWaitActive), admission.readers, admission.hostCallReaders,
                admission.retainedExecutions, admission.parked, admission.waitingEntrants,
                static_cast<unsigned long long>(admission.requestEpoch),
                static_cast<unsigned>(admission.writer), static_cast<unsigned>(admission.fullIntent));
        }
        return Denied;
    }
    // Revalidate after admission: a pending writer may revoke a range while
    // this root waits. A pre-admission positive query is never a CPU lease.
    std::uint64_t code_base {}, code_size {};
    std::uint32_t writable {};
    if (executable_query(opaque, gp->rip, &code_base, &code_size, &writable) != 1 ||
        !Contains(code_base, code_size, gp->rip, 1) || writable > 1)
        return LUMEN_ANDROID_FEXCORE_RUNTIME_EXECUTABLE_RANGE_DENIED_V1;
    ActiveRun run {};
    run.parent = active_run;
    run.executable_query = executable_query;
    run.opaque = opaque;
    run.owned_base = owned_base;
    run.owned_size = owned_size;
    run.owned_query = owned_query;
    if (run.parent != nullptr && run.parent->fresh_callback_token != 0) {
        if (!QemuValidateFreshCallbackParent()) return Denied;
        const auto rsp = gp->gprs[4];
        if (rsp < 16 || rsp > UINT64_MAX - 8 ||
            !DataAllowed(run, rsp - 16, 24, false) ||
            !DataAllowed(run, rsp - 16, 24, true) ||
            (gp->fsBase != 0 && !DataAllowed(run, gp->fsBase, 8, false)) ||
            (gp->gsBase != 0 && !DataAllowed(run, gp->gsBase, 8, false))) return Denied;
    }
    const auto self = pthread_self();
    static_assert(sizeof(self) <= sizeof(run.host_thread));
    std::memcpy(&run.host_thread, &self, sizeof(self));
    { std::lock_guard lock(targets_mutex); run.host_targets = targets; }
    // This query only reads our immutable target set: it cannot change the
    // native thread's MAP_JIT mode or dispatch external code. MemoryQuery has
    // external permission callbacks and deliberately has no such capability.
    const LumenQemuTcgRunConfig config {
        sizeof(config), 2, ReturnAddress(), MemoryQuery, HostTargetQuery, &run,
        LUMEN_QEMU_TCG_HOST_TARGET_PRESERVES_JIT, 0};
    if (LumenQemuTcgOpen(&config, &run.session) != LUMEN_QEMU_TCG_READY) return Denied;
    struct Scope {
        ActiveRun& run;
        explicit Scope(ActiveRun& value): run(value) {
            std::lock_guard lock(runs_mutex);
            runs.push_back(&run);
            active_run = &run;
        }
        ~Scope() {
            {
                std::lock_guard lock(runs_mutex);
                for (auto iterator = runs.begin(); iterator != runs.end(); ++iterator)
                    if (*iterator == &run) { runs.erase(iterator); break; }
                active_run = run.parent;
            }
            LumenQemuTcgClose(run.session);
        }
    } scope(run);
    LumenQemuTcgGuestState state {};
    run.state = &state;
    state.struct_size = sizeof(state);
    state.abi_version = 1;
    state.rip = gp->rip;
    std::memcpy(state.gprs, gp->gprs, sizeof(state.gprs));
    state.fs_base = gp->fsBase;
    state.gs_base = gp->gsBase;
    state.rflags = 2;
    state.mxcsr = extended != nullptr ? extended->mxcsr : 0x1f80;
    state.x87_control = extended != nullptr ? extended->x87ControlWord : 0x037f;
    if (extended != nullptr) std::memcpy(state.ymm, extended->vectorRegisters, sizeof(state.ymm));
    std::uint32_t outcome = Ready;
    for (;;) {
        if (!barrier.CheckpointSafepoint()) { outcome = Denied; break; }
        // QEMU's setjmp/longjmp is entirely below this call's C boundary.
        const auto result = LumenQemuTcgRun(run.session, &state);
        if (result == LUMEN_QEMU_TCG_RETURNED) break;
        if (result == LUMEN_QEMU_TCG_INTERRUPTED) {
            if (!barrier.CheckpointSafepoint()) { outcome = Denied; break; }
            const auto token = run.interrupt_token.exchange(0, std::memory_order_acq_rel);
            if (token != 0) {
                const auto delivery = boundary_delivery.load(std::memory_order_acquire);
                if (delivery == nullptr || !barrier.CheckpointAndSuspendExecutionForHostCall()) {
                    outcome = Denied; break;
                }
                const bool fp_valid = InvokeNativeBoundary(native_environment.Snapshot(), [&] {
                    // Zero means that no installed/queued handler ran. The
                    // token was consumed and must neither fail nor replay.
                    (void)delivery(state.gprs, state.rip);
                });
                if (!barrier.ResumeExecutionAfterHostCallAndCheckpoint() || !fp_valid) {
                    outcome = Denied; break;
                }
            }
            continue;
        }
        if (result == LUMEN_QEMU_TCG_HOST_CALL) {
            if (!barrier.CheckpointAndSuspendExecutionForHostCall()) { outcome = Denied; break; }
            LumenAndroidFexCoreSimdViewV2 simd {
                sizeof(simd), 16, 32, 16, &state.ymm[0][0]};
            auto next_rip = state.rip;
            std::uint32_t disposition = LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
            run.host_call_target = state.boundary_target;
            const bool fp_valid = InvokeNativeBoundary(native_environment.Snapshot(), [&] {
                disposition = host_v2 != nullptr ?
                    host_v2(opaque, state.boundary_target, state.gprs, &simd, &next_rip) :
                    host_v1(opaque, state.boundary_target, state.gprs, &next_rip);
            });
            run.host_call_target = 0;
            state.rip = next_rip;
            if (!barrier.ResumeExecutionAfterHostCallAndCheckpoint()) { outcome = Denied; break; }
            if (run.fresh_callback_restore_failed || run.fresh_callback_token != 0) {
                outcome = Denied; break;
            }
            if (!fp_valid) { outcome = LUMEN_ANDROID_FEXCORE_RUNTIME_HOST_CALL_FAILED_V1; break; }
            if (disposition == LUMEN_ANDROID_FEXCORE_HOST_CALL_STOP_V1) break;
            if (disposition != LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1 || state.rip == 0) {
                outcome = LUMEN_ANDROID_FEXCORE_RUNTIME_HOST_CALL_FAILED_V1; break;
            }
            continue;
        }
        outcome = result == LUMEN_QEMU_TCG_SYSCALL ?
            LUMEN_ANDROID_FEXCORE_RUNTIME_SYSCALL_DENIED_V1 :
            result == LUMEN_QEMU_TCG_GUEST_FAULT ? LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 :
            LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1;
        const char* trace = std::getenv("LUMEN_QEMU_TCG_FAULT_CONTEXT");
        if (result == LUMEN_QEMU_TCG_GUEST_FAULT && trace != nullptr && std::strcmp(trace, "1") == 0) {
            Dl_info native {};
            const bool named = dladdr(reinterpret_cast<const void*>(state.fault_address), &native) != 0;
            std::fprintf(stderr, "QEMU_GUEST_FAULT exception=%u access=%u rip=%#llx address=%#llx "
                "rax=%#llx rcx=%#llx rdx=%#llx rbx=%#llx rsp=%#llx rbp=%#llx rsi=%#llx rdi=%#llx "
                "fs=%#llx gs=%#llx nativeImage=%s nativeOffset=%#llx nativeSymbol=%s\n",
                state.fault_exception, state.fault_access,
                static_cast<unsigned long long>(state.rip), static_cast<unsigned long long>(state.fault_address),
                static_cast<unsigned long long>(state.gprs[0]), static_cast<unsigned long long>(state.gprs[1]),
                static_cast<unsigned long long>(state.gprs[2]), static_cast<unsigned long long>(state.gprs[3]),
                static_cast<unsigned long long>(state.gprs[4]), static_cast<unsigned long long>(state.gprs[5]),
                static_cast<unsigned long long>(state.gprs[6]), static_cast<unsigned long long>(state.gprs[7]),
                static_cast<unsigned long long>(state.fs_base), static_cast<unsigned long long>(state.gs_base),
                named && native.dli_fname ? native.dli_fname : "<none>",
                named ? static_cast<unsigned long long>(state.fault_address - reinterpret_cast<std::uintptr_t>(native.dli_fbase)) : 0ULL,
                named && native.dli_sname ? native.dli_sname : "<none>");
            std::uint64_t base {}, span {}; std::uint32_t writable {};
            if (executable_query(opaque, state.rip, &base, &span, &writable) == 1 &&
                Contains(base, span, state.rip, 16)) {
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(state.rip);
                std::fprintf(stderr, "QEMU_GUEST_FAULT code=");
                for (unsigned index = 0; index < 16; ++index) std::fprintf(stderr, "%02x", bytes[index]);
                std::fprintf(stderr, "\n");
            }
        }
        break;
    }
    CopyGp(state, *gp);
    if (outcome != LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1) {
        gp->faultAddress = 0;
        gp->faultSignal = 0;
    }
    if (extended != nullptr) {
        std::memcpy(extended->vectorRegisters, state.ymm, sizeof(state.ymm));
        extended->mxcsr = state.mxcsr;
        extended->x87ControlWord = state.x87_control;
        // FEX's internal flags/x87 checkpoint bytes are never synthesized.
        std::memset(extended->flags, 0, sizeof(extended->flags));
        std::memset(extended->x87Registers, 0, sizeof(extended->x87Registers));
    }
    return native_environment.Restore() ? outcome :
        LUMEN_ANDROID_FEXCORE_RUNTIME_HOST_CALL_FAILED_V1;
}

std::uint32_t Probe(LumenAndroidFexCoreRuntimeStatusV1* status) noexcept {
    const auto code = Initialize();
    if (status != nullptr) {
        *status = {};
        status->structSize = sizeof(*status);
        status->abiVersion = 1;
        status->code = code;
        status->hostPageSize = static_cast<std::uint32_t>(sysconf(_SC_PAGESIZE));
        status->contextInitialized = code == Ready;
        status->hostBridgeReady = code == Ready;
        status->backendRouteReady = code == Ready;
        std::lock_guard lock(targets_mutex);
        status->registeredHostTargets = static_cast<std::uint32_t>(targets->size());
    }
    return code;
}
std::uint32_t Tsc(LumenAndroidFexCoreGuestTscV1* output) noexcept {
    if (output == nullptr || output->structSize != sizeof(*output) ||
        output->abiVersion != 1 || output->reserved != 0) return Invalid;
    *output = {};
    // Clock queries from native GPU/VideoOut workers must never bootstrap a
    // translator. Probe/root admission publishes the initialized clock.
    if (!initialized.load(std::memory_order_acquire))
        return LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1;
    output->counter = LumenQemuTcgReadTsc();
    output->frequency = LumenQemuTcgTscFrequency();
    output->nativeFrequency = output->frequency;
    return output->frequency != 0 ? Ready : LUMEN_ANDROID_FEXCORE_RUNTIME_CONFIGURATION_FAILED_V1;
}
std::uint32_t RegisterTarget(std::uint64_t target) noexcept {
    if (target == 0 || Initialize() != Ready) return 0;
    std::lock_guard lock(targets_mutex);
    if (targets->contains(target)) return 1;
    auto updated = std::make_shared<Targets>(*targets);
    updated->insert(target);
    targets = std::move(updated);
    return 1;
}
const char* MutationDenialReasonName(
    GuestMemoryMutationBarrier::MutationDenialReason reason) noexcept {
    using Reason = GuestMemoryMutationBarrier::MutationDenialReason;
    switch (reason) {
    case Reason::None: return "none";
    case Reason::HostWaitActive: return "host-wait-active";
    case Reason::GuestDataLeaseActive: return "guest-data-lease-active";
    case Reason::NativeDataLeaseActive: return "native-data-lease-active";
    case Reason::NestedMutationDenied: return "nested-mutation-denied";
    case Reason::ForeignExecutionBarrier: return "foreign-execution-barrier";
    case Reason::ForeignHostCallBarrier: return "foreign-host-call-barrier";
    case Reason::HostReaderOwnershipMismatch: return "host-reader-ownership-mismatch";
    case Reason::NestedExclusiveMutation: return "nested-exclusive-mutation";
    case Reason::CompetingExclusiveMutation: return "competing-exclusive-mutation";
    case Reason::PendingMutationTimeout: return "pending-mutation-timeout";
    case Reason::RetainedExecutionTimeout: return "retained-execution-timeout";
    case Reason::NativeDataLeaseTimeout: return "native-data-lease-timeout";
    case Reason::ArmSafepointsFailed: return "arm-safepoints-failed";
    case Reason::RequestEpochChanged: return "request-epoch-changed";
    case Reason::QuiescenceTimeout: return "quiescence-timeout";
    case Reason::ExecutionUpgradeMismatch: return "execution-upgrade-mismatch";
    case Reason::HostCallUpgradeMismatch: return "host-call-upgrade-mismatch";
    }
    return "unknown";
}

std::uint64_t BeginMutation(std::uint32_t kind) noexcept {
    if (kind > LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_ADDITIVE_MAPPING_V2) return 0;
    const char* trace = std::getenv("LUMEN_QEMU_TCG_FAULT_CONTEXT");
    const bool diagnostics = trace != nullptr && std::strcmp(trace, "1") == 0;
    GuestMemoryMutationBarrier::MutationDenialDiagnostic diagnostic {};
    const auto token = barrier.BeginMutation(kind == 0 ? GuestMemoryMutationScope::Exclusive :
        GuestMemoryMutationScope::AdditiveMapping, ArmSafepoints, DisarmSafepoints,
        std::chrono::seconds(5), diagnostics ? &diagnostic : nullptr);
    static std::atomic<unsigned> denial_records {};
    if (token == 0 && diagnostics && denial_records.fetch_add(1, std::memory_order_relaxed) < 32) {
        const auto target = active_run != nullptr ? active_run->host_call_target : 0;
        const auto outer_target = active_run != nullptr && active_run->parent != nullptr
            ? active_run->parent->host_call_target : 0;
        Dl_info native {}, outer {};
        const bool named = target != 0 && dladdr(reinterpret_cast<const void*>(target), &native) != 0;
        const bool outer_named = outer_target != 0 &&
            dladdr(reinterpret_cast<const void*>(outer_target), &outer) != 0;
        std::fprintf(stderr,
            "QEMU_VM_MUTATION_DENIED scope=%s reason=%s executionDepth=%u hostCallDepth=%u "
            "ownedHostReaders=%u retainedDepth=%u guestDataLeases=%u mutationDepth=%u "
            "mutationScope=%u publishing=%u mutationOwner=%u hostWait=%u executionCounted=%u "
            "readers=%zu hostReaders=%zu retainedExecutions=%zu parked=%zu waiting=%zu "
            "requestEpoch=%llu requestScope=%u writer=%u fullIntent=%u hostThread=%llu "
            "hostTarget=%#llx hostTargetOffset=%#llx hostSymbol=%s "
            "outerHostTarget=%#llx outerHostTargetOffset=%#llx outerHostSymbol=%s\n",
            kind == 0 ? "exclusive" : "additive", MutationDenialReasonName(diagnostic.reason),
            diagnostic.executionDepth, diagnostic.hostCallDepth, diagnostic.hostCallReadersOwned,
            diagnostic.retainedExecutionDepth, diagnostic.guestDataLeases, diagnostic.mutationDepth,
            static_cast<unsigned>(diagnostic.mutationScope), diagnostic.mutationPublishing ? 1U : 0U,
            diagnostic.mutationOwnerMatches ? 1U : 0U, diagnostic.hostWaitActive ? 1U : 0U,
            diagnostic.executionCounted ? 1U : 0U, diagnostic.readers, diagnostic.hostCallReaders,
            diagnostic.retainedExecutions, diagnostic.parked, diagnostic.waitingEntrants,
            static_cast<unsigned long long>(diagnostic.requestEpoch),
            static_cast<unsigned>(diagnostic.requestScope), diagnostic.writer ? 1U : 0U,
            diagnostic.fullIntent ? 1U : 0U,
            static_cast<unsigned long long>(active_run != nullptr ? active_run->host_thread : 0),
            static_cast<unsigned long long>(target),
            static_cast<unsigned long long>(named ? target - reinterpret_cast<std::uint64_t>(native.dli_fbase) : 0),
            named && native.dli_sname != nullptr ? native.dli_sname : "<none>",
            static_cast<unsigned long long>(outer_target),
            static_cast<unsigned long long>(outer_named ? outer_target - reinterpret_cast<std::uint64_t>(outer.dli_fbase) : 0),
            outer_named && outer.dli_sname != nullptr ? outer.dli_sname : "<none>");
    }
    return token;
}
std::uint32_t EndMutation(std::uint64_t token, std::uint64_t base,
    std::uint64_t size, std::uint32_t committed,
    LumenAndroidFexCoreMutationLocalReleaseV2 release, void* opaque) noexcept {
    bool invalidated = true;
    const bool additive = barrier.OwnsAdditiveMutation(token);
    const bool ended = barrier.PublishAndEndMutation(token, [&] {
        if (committed != 0 && size != 0 && initialized.load(std::memory_order_acquire))
            invalidated = (additive ? LumenQemuTcgInvalidateAdditive(base, size) :
                LumenQemuTcgInvalidate(base, size)) == LUMEN_QEMU_TCG_READY;
        if (!invalidated) { std::fputs("QEMU TCG invalidation failed\n", stderr); std::_Exit(74); }
        if (committed != 0) {
            // PublishAndEndMutation has quiesced CPU execution leases. Only
            // MemoryQuery under a CPU lease accesses these caches; suspended
            // HLE readers (which additive writers may retain) never do. Clear
            // their root caches too, before any CPU resumes after a revoke.
            std::lock_guard lock(runs_mutex);
            for (auto* run: runs) run->memory_regions.Clear();
        }
        if (release != nullptr) release(opaque);
    }, DisarmSafepoints);
    return ended && invalidated ? 1U : 0U;
}
std::uint64_t RequestInterrupt(std::uint64_t host_thread) noexcept {
    if (host_thread == 0) return 0;
    std::lock_guard lock(runs_mutex);
    // Most recent nested session belongs to the same host thread.
    for (auto iterator = runs.rbegin(); iterator != runs.rend(); ++iterator) {
        auto* run = *iterator;
        if (run->host_thread != host_thread) continue;
        const auto existing = run->interrupt_token.load(std::memory_order_acquire);
        if (existing != 0) return existing;
        const auto token = next_interrupt_token.fetch_add(1, std::memory_order_relaxed);
        if (token == 0) return 0;
        run->interrupt_token.store(token, std::memory_order_release);
        LumenQemuTcgInterrupt(run->session);
        return token;
    }
    return 0;
}
std::uint32_t CancelInterrupt(std::uint64_t token) noexcept {
    if (token == 0) return 0;
    std::lock_guard lock(runs_mutex);
    for (auto* run: runs) {
        auto expected = token;
        if (run->interrupt_token.compare_exchange_strong(expected, 0)) return 1;
    }
    return 0; // No active run still owns this request, matching the FEX ABI.
}
std::uint32_t RegisterDelivery(LumenAndroidFexCoreBoundaryDeliveryV1 delivery) noexcept {
    if (delivery == nullptr) return 0;
    auto expected = static_cast<LumenAndroidFexCoreBoundaryDeliveryV1>(nullptr);
    return boundary_delivery.compare_exchange_strong(expected, delivery) || expected == delivery;
}
} // namespace

namespace {
bool FreshCallbackParentValid(ActiveRun& run, bool capture_return) noexcept {
    if (run.state == nullptr || run.fresh_callback_lifetime_revoked) return false;
    const auto& state = *run.state;
    const auto rsp = state.gprs[4];
    if (rsp < 128 || rsp > UINT64_MAX - 8 ||
        !Contains(run.fresh_stack_base, run.fresh_stack_size, rsp - 128, 136) ||
        !DataAllowed(run, run.fresh_stack_base, run.fresh_stack_size, false) ||
        !DataAllowed(run, run.fresh_stack_base, run.fresh_stack_size, true)) return false;
    if (capture_return)
        std::memcpy(&run.fresh_callback_return, reinterpret_cast<const void*>(rsp), 8);
    std::uint64_t base {}, size {}; std::uint32_t writable {};
    if (run.executable_query(run.opaque, run.fresh_callback_return, &base, &size, &writable) != 1 ||
        !Contains(base, size, run.fresh_callback_return, 1) ||
        size > UINT64_MAX - base || writable > 1) return false;
    if (capture_return) {
        run.fresh_code_base = base;
        run.fresh_code_size = size;
    }
    if (state.fs_base != 0 &&
        (!Contains(run.fresh_tls_base, run.fresh_tls_size, state.fs_base, 8) ||
         !DataAllowed(run, run.fresh_tls_base, run.fresh_tls_size, false) ||
         !DataAllowed(run, run.fresh_tls_base, run.fresh_tls_size, true))) return false;
    for (const auto tls: {state.fs_base, state.gs_base})
        if (tls != 0 && !DataAllowed(run, tls, 8, false)) return false;
    return true;
}
} // namespace

std::uint64_t QemuBeginFreshCallback(std::uint64_t stack_base,
    std::uint64_t stack_size, std::uint64_t tls_base, std::uint64_t tls_size) noexcept {
    if (active_run == nullptr || active_run->host_call_target == 0 ||
        active_run->fresh_callback_token != 0 || active_run->fresh_callback_restore_failed ||
        active_run->state == nullptr || active_run->state->gs_base != 0 ||
        stack_base == 0 || stack_size == 0 || stack_size > UINT64_MAX - stack_base ||
        (tls_base == 0) != (tls_size == 0) || tls_size > UINT64_MAX - tls_base ||
        !barrier.CurrentThreadOwnsHostMemoryReadLease()) return 0;
    active_run->fresh_stack_base = stack_base;
    active_run->fresh_stack_size = stack_size;
    active_run->fresh_tls_base = tls_base;
    active_run->fresh_tls_size = tls_size;
    if (!FreshCallbackParentValid(*active_run, true)) return 0;
    // Arm before dropping the reader, but never hold runs_mutex while taking
    // the barrier mutex: mutation publication takes those locks in reverse.
    // The independent watch covers the interval before token publication.
    { std::lock_guard lock(runs_mutex); active_run->fresh_callback_watch_active = true; }
    const auto token = barrier.BeginFreshCallback();
    {
        std::lock_guard lock(runs_mutex);
        active_run->fresh_callback_token = token;
        if (token == 0) active_run->fresh_callback_watch_active = false;
    }
    return token;
}

bool QemuEndFreshCallback(std::uint64_t token) noexcept {
    if (active_run == nullptr || token == 0 || active_run->fresh_callback_token != token)
        return false;
    if (!barrier.EndFreshCallback(token)) {
        active_run->fresh_callback_restore_failed = true;
        return false;
    }
    {
        std::lock_guard lock(runs_mutex);
        active_run->fresh_callback_token = 0;
        active_run->fresh_callback_watch_active = false;
    }
    const bool valid = FreshCallbackParentValid(*active_run, false);
    active_run->fresh_callback_restore_failed = !valid;
    return valid;
}

bool QemuValidateFreshCallbackParent() noexcept {
    if (active_run == nullptr || active_run->fresh_callback_token == 0 ||
        !barrier.CurrentThreadOwnsExecutionOrMutationLease()) return false;
    {
        std::lock_guard lock(runs_mutex);
        if (!active_run->fresh_callback_watch_active ||
            active_run->fresh_callback_lifetime_revoked) return false;
    }
    return FreshCallbackParentValid(*active_run, false);
}

void QemuNotifyMappingChange(std::uint64_t base, std::uint64_t size) noexcept {
    std::lock_guard lock(runs_mutex);
    for (auto* run: runs) {
        if (!run->fresh_callback_watch_active) continue;
        const auto overlaps = [base, size](std::uint64_t saved_base,
                                           std::uint64_t saved_size) {
            if (saved_size == 0) return false;
            if (size == 0 || size > UINT64_MAX - base) return true;
            return base < saved_base + saved_size && saved_base < base + size;
        };
        if (overlaps(run->fresh_code_base, run->fresh_code_size) ||
            overlaps(run->fresh_stack_base, run->fresh_stack_size) ||
            overlaps(run->fresh_tls_base, run->fresh_tls_size))
            run->fresh_callback_lifetime_revoked = true;
    }
}

std::uint32_t QemuRunWithOwnedStack(LumenAndroidFexCoreGuestStateV3* state,
    LumenAndroidFexCoreHostCallV2 host_call,
    LumenAndroidFexCoreExecutableQueryV1 executable_query, void* opaque,
    std::uint64_t owned_base, std::uint64_t owned_size,
    LumenMacOSFexCoreOwnedStackAccessQueryV1 owned_query) noexcept {
    return state != nullptr ? Run(&state->gp, state, nullptr, host_call,
        executable_query, opaque, owned_base, owned_size, owned_query) : Invalid;
}

bool QemuRegisterImmutableCodeQuery(ImmutableCodeQuery query) noexcept {
    if (query == nullptr) return false;
    ImmutableCodeQuery expected = nullptr;
    return immutable_code_query.compare_exchange_strong(expected, query,
        std::memory_order_acq_rel, std::memory_order_acquire) || expected == query;
}

const ProviderFunctions& QemuProviderFunctions() noexcept {
    static const ProviderFunctions functions = [] {
        ProviderFunctions table {};
        table.provider = Provider::QemuTcg;
        table.runtimeProbe = Probe;
        table.queryGuestTsc = Tsc;
        table.runtimeReceipt = +[]() noexcept { return LumenQemuTcgReceipt(); };
        table.registerHostCallTarget = RegisterTarget;
        table.rootReturnAddress = +[]() noexcept { return Initialize() == Ready ? ReturnAddress() : 0; };
        table.runGuestV1 = +[](LumenAndroidFexCoreGuestStateV1* state,
            LumenAndroidFexCoreHostCallV1 host,
            LumenAndroidFexCoreExecutableQueryV1 query, void* opaque) noexcept {
            return Run(state, nullptr, host, nullptr, query, opaque); };
        table.runGuestV2 = +[](LumenAndroidFexCoreGuestStateV1* state,
            LumenAndroidFexCoreHostCallV2 host,
            LumenAndroidFexCoreExecutableQueryV1 query, void* opaque) noexcept {
            return Run(state, nullptr, nullptr, host, query, opaque); };
        table.runGuestV3 = +[](LumenAndroidFexCoreGuestStateV3* state,
            LumenAndroidFexCoreHostCallV2 host,
            LumenAndroidFexCoreExecutableQueryV1 query, void* opaque) noexcept -> std::uint32_t {
            return state != nullptr ? Run(&state->gp, state, nullptr, host, query, opaque) : Invalid; };
        table.beginMemoryMutationV1 = +[]() noexcept { return BeginMutation(0); };
        table.beginMemoryMutationV2 = BeginMutation;
        table.endMemoryMutationV1 = +[](std::uint64_t token, std::uint64_t base,
            std::uint64_t size, std::uint32_t committed) noexcept {
            if (EndMutation(token, base, size, committed, nullptr, nullptr) == 0) std::_Exit(74); };
        table.endMemoryMutationV2 = EndMutation;
        table.registerBoundaryDelivery = RegisterDelivery;
        table.requestBoundaryInterrupt = RequestInterrupt;
        table.cancelBoundaryInterrupt = CancelInterrupt;
        table.beginQuiescentHostWait = +[]() noexcept {
            return active_run != nullptr ? barrier.BeginQuiescentHostWait() : 0; };
        table.endQuiescentHostWait = +[](std::uint64_t token) noexcept {
            return active_run != nullptr && barrier.EndQuiescentHostWait(token) ? 1U : 0U; };
        table.retainGuestDataLease = +[]() noexcept { return barrier.RetainGuestDataLease() ? 1U : 0U; };
        table.releaseGuestDataLease = +[]() noexcept { return barrier.ReleaseGuestDataLease() ? 1U : 0U; };
        table.tryBeginNativeDataLease = +[]() noexcept { return barrier.TryBeginNativeGuestDataLease(); };
        table.endNativeDataLease = +[](std::uint64_t token) noexcept {
            return barrier.EndNativeGuestDataLease(token) ? 1U : 0U; };
#if defined(__APPLE__)
        table.registerDataAccessQuery = +[](LumenMacOSFexCoreDataAccessQueryV1 query) noexcept {
            return RegisterMemoryAccessQuery(query) ? 1U : 0U; };
        table.runGuestWithOwnedStackV3 = QemuRunWithOwnedStack;
        table.tryReadOwnedStack = +[](std::uint64_t address, void* destination, std::uint64_t size) noexcept {
            if (active_run == nullptr || destination == nullptr ||
                !barrier.CurrentThreadOwnsHostMemoryReadLease() ||
                !DataAllowed(*active_run, address, size, false)) return 0U;
            bool owned = false;
            for (auto* owner = active_run; owner != nullptr; owner = owner->parent)
                if (Contains(owner->owned_base, owner->owned_size, address, size)) owned = true;
            if (!owned) return 0U;
            std::memcpy(destination, reinterpret_cast<const void*>(address), size);
            return 1U;
        };
#endif
        return table;
    }();
    return functions;
}
} // namespace Lumen::GuestCpu
