#include "cpu/runtime/MemoryHandoff.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace Lumen::GuestCpu {
namespace {

bool CheckClaim(bool condition, const char* reason)
{
    if (!condition) std::fprintf(stderr, "[NATIVE_MEMORY_CLAIMS_TEST] failure=%s\n", reason);
    return condition;
}

template<typename Predicate>
bool ObserveClaimState(Predicate&& predicate)
{
    auto& registry = HandoffDetail::GetRegistry();
    std::unique_lock lock(registry.mutex);
    // A deadlock watchdog, never a throughput or latency measurement.
    return registry.native_claim_ready.wait_for(lock, std::chrono::seconds(10), predicate);
}

struct RestoreDirtyState {
    RestoreDirtyState()
    {
        auto& state = HandoffDetail::GetReadDirectoryState();
        std::lock_guard lock(state.mutex);
        dirty = state.gpu_dirty;
    }
    ~RestoreDirtyState() { PublishMemoryHandoffGpuDirty(dirty); }
    bool dirty = true;
};

} // namespace

bool NativeMemoryClaimsContractTest()
{
    RestoreDirtyState restore;
    constexpr std::uint64_t address = UINT64_C(0x17100000);
    constexpr std::uint64_t alias = UINT64_C(0x17130000);
    constexpr std::uint64_t late_alias = UINT64_C(0x17150000);
    constexpr std::uint64_t disjoint = UINT64_C(0x17200000);
    constexpr std::uint64_t collision = address + (std::uint64_t {MemoryHandoffGuardCount} << MemoryHandoffPageShift);
    constexpr std::uint64_t identity = UINT64_C(0x4017000000000001);
    const PhysicalMemoryRange mapped {identity, 0, 160};
    const auto owner = CreateMemoryHandoffOwner();
    const auto other = CreateMemoryHandoffOwner();
    const auto write_only = CreateMemoryHandoffOwner();
    bool okay = CheckClaim(owner != 0 && other != 0 && write_only != 0 &&
        CurrentMemoryHandoffNativeClaimOwner() == 0, "fresh-owners");
    if (!okay) return false;
    okay = CheckClaim(!PublishMemoryHandoffNativeClaim(0) &&
        !PublishMemoryHandoffNativeClaim(UINT64_MAX) &&
        !PublishMemoryHandoffNativeClaim(other), "invalid-and-empty-publish") && okay;
    okay = CheckClaim(EnrollMemoryHandoffPhysicalRange(write_only, {identity + 1, 0, 4096}, MemoryAccessIntent::Write) &&
        MapMemoryHandoffOwnerPhysicalAlias(write_only, {identity + 1, 0, 4096}, disjoint + 0x10000) &&
        !PublishMemoryHandoffNativeClaim(write_only), "read-fast-path-needs-rw-guards") && okay;
    // Intentionally unsorted intervals: coverage must prove the union, including
    // adjacency, and reject the gap [96,128), regardless of bucket collisions.
    for (const auto range : {PhysicalMemoryRange {identity, 64, 32}, {identity, 0, 32},
                            {identity, 128, 32}, {identity, 32, 32}})
        okay = EnrollMemoryHandoffPhysicalRange(owner, range, MemoryAccessIntent::ReadWrite) && okay;
    okay = MapMemoryHandoffOwnerPhysicalAlias(owner, mapped, address) && okay;
    okay = MapMemoryHandoffOwnerPhysicalAlias(owner, mapped, alias) && okay;
    PublishMemoryHandoffGpuDirty(false);
    okay = CheckClaim(MemoryHandoffReadGuardDirectory()->load(std::memory_order_acquire) == nullptr,
        "clean-directory-before-claim") && okay;
    const bool published = PublishMemoryHandoffNativeClaim(owner);
    okay = CheckClaim(published, "publish-complete-owner") && okay;
    if (!published) {
        ReleaseMemoryHandoffOwner(owner);
        ReleaseMemoryHandoffOwner(other);
        ReleaseMemoryHandoffOwner(write_only);
        return false;
    }
    okay = CheckClaim(!PublishMemoryHandoffNativeClaim(owner) && !PublishMemoryHandoffNativeClaim(other) &&
        !RetireMemoryHandoffNativeClaim(other) && !ReleaseMemoryHandoffOwner(owner) &&
        !EnrollMemoryHandoffPhysicalRange(owner, {identity, 160, 16}, MemoryAccessIntent::ReadWrite),
        "active-owner-is-unique-and-closure-is-immutable") && okay;
    okay = CheckClaim(!NativeClaimCoversPhysical({identity, 0, 32}), "coverage-needs-thread-owner") && okay;
    {
        MemoryHandoffNativeClaimScope scope(owner);
        okay = CheckClaim(scope.IsActive() && CurrentMemoryHandoffNativeClaimOwner() == owner &&
            NativeClaimCoversPhysical({identity, 0, 96}) && NativeClaimCoversPhysical({identity, 8, 80}) &&
            NativeClaimCoversPhysical({identity, 128, 32}) &&
            !NativeClaimCoversPhysical({identity, 0, 160}) && !NativeClaimCoversPhysical({identity, 95, 2}) &&
            !NativeClaimCoversPhysical({identity + 1, 0, 32}) && !NativeClaimCoversPhysical({0, 0, 32}) &&
            !NativeClaimCoversPhysical({identity, 0, 0}) && !NativeClaimCoversPhysical({identity, UINT64_MAX - 1, 4}) &&
            WaitForMemoryHandoffNativeClaim(address, 32), "self-owner-and-exact-physical-coverage") && okay;
        {
            MemoryHandoffNativeClaimScope nested(owner), wrong(other);
            okay = CheckClaim(nested.IsActive() && !wrong.IsActive() &&
                CurrentMemoryHandoffNativeClaimOwner() == owner, "nested-same-owner-only") && okay;
        }
        okay = CheckClaim(CurrentMemoryHandoffNativeClaimOwner() == owner, "nested-restores-owner") && okay;
    }
    okay = CheckClaim(CurrentMemoryHandoffNativeClaimOwner() == 0 &&
        !WaitForMemoryHandoffNativeClaim(0, 1) && !WaitForMemoryHandoffNativeClaim(address, 0) &&
        !WaitForMemoryHandoffNativeClaim(UINT64_MAX - 1, 4), "scope-exit-and-malformed-access") && okay;
    okay = CheckClaim(MapMemoryHandoffPhysicalAlias(mapped, late_alias) &&
        NeedsHandoff(late_alias, 1, MemoryAccessIntent::Read) &&
        HandoffDetail::NativeClaimBucketConflict(late_alias, 1), "new-alias-inherits-active-claim") && okay;

    std::atomic_uint completed {0};
    std::atomic_bool waiter_ok {true};
    std::thread first([&] {
        // Even a generic handoff bypass cannot skip an independent CPU claim.
        MemoryHandoffBypass bypass;
        if (!PrepareCpuMemoryAccess(address, 8, MemoryAccessIntent::Read)) waiter_ok.store(false);
        completed.fetch_add(1);
    });
    std::thread second([&] {
        if (!WaitForMemoryHandoffNativeClaim(late_alias, 8)) waiter_ok.store(false);
        completed.fetch_add(1);
    });
    std::thread third([&] {
        if (!WaitForMemoryHandoffNativeClaim(collision, 8)) waiter_ok.store(false);
        completed.fetch_add(1);
    });
    okay = CheckClaim(ObserveClaimState([] { return HandoffDetail::GetRegistry().native_claim_waiters == 3; }) &&
        completed.load() == 0, "overlap-alias-and-hash-collision-wait") && okay;

    std::atomic_bool unrelated_done {false};
    std::thread unrelated([&] {
        if (!WaitForMemoryHandoffNativeClaim(disjoint, 8)) waiter_ok.store(false);
        unrelated_done.store(true, std::memory_order_release);
        HandoffDetail::GetRegistry().native_claim_ready.notify_all();
    });
    okay = CheckClaim(ObserveClaimState([&] { return unrelated_done.load(std::memory_order_acquire); }) &&
        completed.load() == 0, "disjoint-progress-before-retirement") && okay;

    std::atomic_bool directory_ok {true};
    std::thread dirty_publisher([&] {
        for (unsigned i = 0; i != 512; ++i) {
            PublishMemoryHandoffGpuDirty((i & 1) != 0);
            if (MemoryHandoffReadGuardDirectory()->load(std::memory_order_acquire) != MemoryHandoffReadGuardTable())
                directory_ok.store(false);
        }
        PublishMemoryHandoffGpuDirty(false);
    });
    dirty_publisher.join();
    okay = CheckClaim(directory_ok.load() && completed.load() == 0 &&
        MemoryHandoffReadGuardDirectory()->load(std::memory_order_acquire) == MemoryHandoffReadGuardTable(),
        "clean-ledger-cannot-hide-active-claim") && okay;
    if (!RetireMemoryHandoffNativeClaim(owner)) std::abort();
    first.join(); second.join(); third.join(); unrelated.join();
    okay = CheckClaim(waiter_ok.load() && completed.load() == 3 && unrelated_done.load() &&
        !RetireMemoryHandoffNativeClaim(owner) &&
        MemoryHandoffReadGuardDirectory()->load(std::memory_order_acquire) == nullptr &&
        WaitForMemoryHandoffNativeClaim(address, 8) && NeedsHandoff(address, 8, MemoryAccessIntent::Read),
        "retire-wakes-keeps-owner-and-restores-clean-directory") && okay;
    {
        MemoryHandoffNativeClaimScope retired(owner);
        okay = CheckClaim(!retired.IsActive() && !NativeClaimCoversPhysical({identity, 0, 32}),
            "retired-owner-cannot-authorize-native-bytes") && okay;
    }
    okay = ReleaseMemoryHandoffOwner(owner) && okay;
    okay = ReleaseMemoryHandoffOwner(other) && okay;
    okay = ReleaseMemoryHandoffOwner(write_only) && okay;
    std::fprintf(stderr, "[NATIVE_MEMORY_CLAIMS_TEST] result=%s disjoint=progress overlap/alias/collision=wait "
        "native-coverage=physical-union clean-directory=claim-aware retirement=wake ownership=bounded\n",
        okay ? "PASS" : "FAIL");
    return okay;
}

} // namespace Lumen::GuestCpu
