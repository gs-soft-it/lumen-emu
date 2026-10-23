// Lumen, 2026-10-07. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace Lumen::GuestCpu {

enum class MemoryAccessIntent : std::uint32_t { Read = 1, Write = 2, ReadWrite = 3 };
constexpr bool WritesMemory(MemoryAccessIntent intent) noexcept {
    return (static_cast<std::uint32_t>(intent) & 2U) != 0;
}
constexpr bool ValidMemoryAccessIntent(MemoryAccessIntent intent) noexcept {
    return intent == MemoryAccessIntent::Read || intent == MemoryAccessIntent::Write ||
        intent == MemoryAccessIntent::ReadWrite;
}

struct PhysicalMemoryRange {
    std::uint64_t identity {}, offset {}, size {};
};
inline bool IntersectPhysicalRanges(const PhysicalMemoryRange& first,
    const PhysicalMemoryRange& second, PhysicalMemoryRange* intersection) noexcept {
    if (intersection != nullptr) *intersection = {};
    if (first.identity == 0 || first.identity != second.identity ||
        first.size == 0 || second.size == 0 || first.offset > UINT64_MAX - first.size ||
        second.offset > UINT64_MAX - second.size) return false;
    const auto begin = first.offset > second.offset ? first.offset : second.offset;
    const auto first_end = first.offset + first.size, second_end = second.offset + second.size;
    const auto end = first_end < second_end ? first_end : second_end;
    if (begin >= end) return false;
    if (intersection != nullptr) *intersection = {first.identity, begin, end - begin};
    return true;
}

using MemoryHandoffQuery = bool (*)(std::uint64_t address, std::uint64_t bytes,
    MemoryAccessIntent intent) noexcept;
inline constexpr std::uint32_t MemoryHandoffPageShift = 12;
inline constexpr std::uint32_t MemoryHandoffGuardCount = 1U << 20;
inline constexpr std::uint32_t MemoryHandoffGuardMask = MemoryHandoffGuardCount - 1;

namespace HandoffDetail {
// JIT-visible tables combine owner-counted guards with legacy sticky guards.
// Hash collisions add checks, never suppress them. Only explicit owner tokens
// may retire their contribution; legacy callers cannot revoke another owner.
// The C array avoids a million-element std::array constexpr construction and
// is also usable by the engine's C++17 translation units. Static zero
// initialization supplies the unwatched state before any owner is registered.
alignas(64) inline std::atomic<std::uint8_t> guards[MemoryHandoffGuardCount];
// Every read guard also has an ordinary guard, so stores and RMW cannot bypass
// prior GPU publication. Clean GPU inputs need only the ordinary table above.
alignas(64) inline std::atomic<std::uint8_t> read_guard_pages[MemoryHandoffGuardCount];
// Conservative conflict index only. Physical identity/offset coverage below
// remains the authority for native guest access; a bucket collision may wait.
alignas(64) inline std::atomic<std::uint8_t> native_claim_pages[MemoryHandoffGuardCount];
inline std::atomic<std::uint64_t> native_claim_owner {};
inline thread_local std::uint64_t native_claim_scope_owner {};
inline std::uint8_t sticky_guards[MemoryHandoffGuardCount];
inline std::uint8_t sticky_read_guards[MemoryHandoffGuardCount];
inline std::uint32_t guard_owners[MemoryHandoffGuardCount];
inline std::uint32_t read_guard_owners[MemoryHandoffGuardCount];
struct OwnedPhysicalRange {
    PhysicalMemoryRange physical;
    MemoryAccessIntent intent;
};
struct GuardOwner {
    // One contribution per owner/bucket even for overlapping planes, aliases
    // or repeated upgrades. Mapping history is released with this owner.
    std::map<std::uint32_t, std::uint8_t> buckets;
    std::vector<OwnedPhysicalRange> physical;
    std::vector<std::uint64_t> physical_identities;
    bool native_claim_active = false;
};
struct Registry {
    std::mutex mutex;
    std::condition_variable native_claim_ready;
    std::map<std::uint64_t, GuardOwner> owners;
    std::set<std::pair<std::uint64_t, std::uint64_t>> physical_owners;
    std::uint64_t next_owner = 1;
    std::size_t native_claim_waiters = 0;
    bool configured = false, frozen = false, enabled = true;
};
inline Registry& GetRegistry() {
    static Registry registry;
    return registry;
}
inline std::atomic_bool exclusive_publication_frozen {};
inline bool FreezeLocked(Registry& registry) noexcept {
    registry.frozen = true;
    exclusive_publication_frozen.store(registry.configured && !registry.enabled, std::memory_order_release);
    return registry.enabled;
}
inline void IncrementCount(std::uint32_t& count, std::uint8_t& sticky) noexcept {
    // Overflow can only add a permanent conservative guard. Never wrap to an
    // unguarded bucket, and never later decrement an unknown saturated count.
    if (count == UINT32_MAX) sticky = 3;
    else ++count;
}
inline void DecrementCount(std::uint32_t& count) noexcept {
    if (count != 0 && count != UINT32_MAX) --count;
}
inline void PublishBucket(std::uint32_t bucket) noexcept {
    guards[bucket].store(sticky_guards[bucket] != 0 || guard_owners[bucket] != 0 ? 3 : 0,
        std::memory_order_release);
    read_guard_pages[bucket].store(sticky_read_guards[bucket] != 0 || read_guard_owners[bucket] != 0 ? 3 : 0,
        std::memory_order_release);
}
inline void EnrollBucket(GuardOwner& owner, std::uint32_t bucket, MemoryAccessIntent intent) {
    auto& previous = owner.buckets[bucket];
    const auto requested = intent == MemoryAccessIntent::Write ? 2U : 3U;
    if ((previous & 2U) == 0) IncrementCount(guard_owners[bucket], sticky_guards[bucket]);
    if ((requested & 1U) != 0 && (previous & 1U) == 0)
        IncrementCount(read_guard_owners[bucket], sticky_read_guards[bucket]);
    if ((previous | requested) != previous) {
        previous |= requested;
        PublishBucket(bucket);
    }
    // MapDirect can publish another VA for an already claimed physical range.
    // It does so under its exclusive mapping transaction before readmission.
    if (owner.native_claim_active)
        native_claim_pages[bucket].store(1, std::memory_order_release);
}
// The renderer may publish a null read directory only after proving that its
// entire dirty ledger is empty. New GPU ownership restores the conservative
// table before CPU execution is readmitted. This is a global clean proof, not
// a per-page dirty cache: every live write owner remains checked.
inline std::atomic<const std::uint8_t*> read_guards {
    reinterpret_cast<const std::uint8_t*>(read_guard_pages)};
// This mutex is deliberately separate: dirty-ledger publication may happen
// while the renderer owns unrelated locks. The only combined order is
// Registry::mutex -> ReadDirectoryState::mutex; never the reverse.
struct ReadDirectoryState {
    std::mutex mutex;
    bool gpu_dirty = true;
    bool native_claim_active = false;
};
inline ReadDirectoryState& GetReadDirectoryState() {
    static ReadDirectoryState state;
    return state;
}
inline void PublishReadDirectoryLocked(const ReadDirectoryState& state) noexcept {
    read_guards.store(state.gpu_dirty || state.native_claim_active
        ? reinterpret_cast<const std::uint8_t*>(read_guard_pages) : nullptr, std::memory_order_release);
}
inline void SetNativeClaimReadDirectory(bool active) noexcept {
    auto& state = GetReadDirectoryState();
    std::lock_guard lock(state.mutex);
    state.native_claim_active = active;
    PublishReadDirectoryLocked(state);
}
inline std::atomic<MemoryHandoffQuery> query {};
inline thread_local std::uint32_t bypass {};
static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<std::uint8_t>) == 1);
static_assert(std::atomic<MemoryHandoffQuery>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<const std::uint8_t*>::is_always_lock_free);
static_assert(sizeof(std::atomic<const std::uint8_t*>) == sizeof(const std::uint8_t*));
}

// Immutable process capability, selected before translation/query/ownership.
// Repeating the selected mode is harmless; a conflicting or late change is
// denied. The implicit mode remains enabled for PS4 and existing embeddings.
inline bool ConfigureMemoryHandoffRegistration(bool enabled) noexcept {
    auto& registry = HandoffDetail::GetRegistry();
    std::lock_guard lock(registry.mutex);
    if (registry.configured || registry.frozen) return registry.enabled == enabled;
    registry.configured = true;
    registry.enabled = enabled;
    return true;
}
inline bool FreezeMemoryHandoffRegistration() noexcept {
    auto& registry = HandoffDetail::GetRegistry();
    std::lock_guard lock(registry.mutex);
    return HandoffDetail::FreezeLocked(registry);
}

// Explicit configured+frozen protocol proof, not an inference from a missing
// resolver. The configured mode is immutable once any provider freezes it.
inline bool ExclusiveMemoryPublicationFrozen() noexcept {
    return HandoffDetail::exclusive_publication_frozen.load(std::memory_order_acquire);
}
inline const std::uint8_t* MemoryHandoffGuardTable() noexcept {
    return reinterpret_cast<const std::uint8_t*>(HandoffDetail::guards);
}
inline const std::uint8_t* MemoryHandoffReadGuardTable() noexcept {
    return reinterpret_cast<const std::uint8_t*>(HandoffDetail::read_guard_pages);
}
inline const std::atomic<const std::uint8_t*>* MemoryHandoffReadGuardDirectory() noexcept {
    return &HandoffDetail::read_guards;
}
// The caller owns the renderer/tracker transaction and, when adding GPU
// ownership, the exclusive execution epoch. False requires a complete ledger
// proof; retirement of one resource alone cannot authorize it. Initial state
// is deliberately conservative for other embeddings and synthetic providers.
inline void PublishMemoryHandoffGpuDirty(bool dirty) noexcept {
    auto& state = HandoffDetail::GetReadDirectoryState();
    std::lock_guard lock(state.mutex);
    state.gpu_dirty = dirty;
    HandoffDetail::PublishReadDirectoryLocked(state);
}
inline bool RegisterMemoryHandoffQuery(MemoryHandoffQuery query) noexcept {
    if (query == nullptr) return false;
    auto& registry = HandoffDetail::GetRegistry();
    std::lock_guard lock(registry.mutex);
    if (!HandoffDetail::FreezeLocked(registry)) return false;
    MemoryHandoffQuery expected {};
    return HandoffDetail::query.compare_exchange_strong(expected, query,
        std::memory_order_release, std::memory_order_acquire) || expected == query;
}
inline bool MemoryHandoffEnabled() noexcept {
    return HandoffDetail::query.load(std::memory_order_acquire) != nullptr;
}
// The intent describes CPU accesses requiring ownership checks. Read
// enrollment also checks writes/RMW; Write-only enrollment leaves loads fast.
inline void MarkHandoffRange(std::uint64_t address, std::uint64_t bytes,
    MemoryAccessIntent watch_intents, bool watched = true) noexcept {
    if (!watched || bytes == 0 || address > UINT64_MAX - (bytes - 1) ||
        !ValidMemoryAccessIntent(watch_intents)) return;
    auto& registry = HandoffDetail::GetRegistry();
    std::lock_guard lock(registry.mutex);
    if (!HandoffDetail::FreezeLocked(registry)) return;
    const bool reads = watch_intents != MemoryAccessIntent::Write;
    const auto mark_page = [reads](std::uint64_t index) noexcept {
        HandoffDetail::sticky_guards[index] = 3;
        HandoffDetail::guards[index].store(3, std::memory_order_release);
        if (reads) {
            HandoffDetail::sticky_read_guards[index] = 3;
            HandoffDetail::read_guard_pages[index].store(3, std::memory_order_release);
        }
    };
    auto page = address >> MemoryHandoffPageShift;
    const auto last = (address + bytes - 1) >> MemoryHandoffPageShift;
    const auto count = last - page + 1;
    if (count >= MemoryHandoffGuardCount) {
        for (std::uint64_t index = 0; index < MemoryHandoffGuardCount; ++index) mark_page(index);
    } else {
        for (; page <= last; ++page)
            mark_page(page & MemoryHandoffGuardMask);
    }
}
inline void MarkHandoffRange(std::uint64_t address, std::uint64_t bytes, bool watched = true) noexcept {
    MarkHandoffRange(address, bytes, MemoryAccessIntent::ReadWrite, watched);
}

// Explicit resource ownership. All enrollment, alias publication and release
// require the caller's exclusive FEX/VM transaction; the mutex only protects
// metadata and cannot make a check/access gap safe on its own.
inline std::uint64_t CreateMemoryHandoffOwner() noexcept {
    auto& registry = HandoffDetail::GetRegistry();
    std::lock_guard lock(registry.mutex);
    if (!HandoffDetail::FreezeLocked(registry) || registry.next_owner == UINT64_MAX) return 0;
    const auto owner = registry.next_owner++;
    registry.owners.emplace(owner, HandoffDetail::GuardOwner {});
    return owner;
}
inline bool EnrollMemoryHandoffPhysicalRange(std::uint64_t owner, const PhysicalMemoryRange& physical,
    MemoryAccessIntent intent) noexcept {
    if (owner == 0 || physical.identity == 0 || physical.size == 0 ||
        physical.offset > UINT64_MAX - physical.size || !ValidMemoryAccessIntent(intent)) return false;
    auto& registry = HandoffDetail::GetRegistry();
    std::lock_guard lock(registry.mutex);
    const auto found = registry.owners.find(owner);
    if (found == registry.owners.end() || !registry.enabled || found->second.native_claim_active) return false;
    bool identity_present = false;
    for (auto& range : found->second.physical) {
        if (range.physical.identity != physical.identity) continue;
        identity_present = true;
        if (range.physical.offset == physical.offset && range.physical.size == physical.size) {
            range.intent = static_cast<MemoryAccessIntent>(static_cast<std::uint32_t>(range.intent) |
                static_cast<std::uint32_t>(intent));
            return true;
        }
    }
    found->second.physical.push_back({physical, intent});
    if (!identity_present) {
        registry.physical_owners.emplace(physical.identity, owner);
        found->second.physical_identities.push_back(physical.identity);
    }
    return true;
}
// Publish every accepted new/remapped physical alias before readmission.
// Only matching live owners are visited; offsets, not VA/hash equality,
// establish the physical overlap. Unmapping may conservatively retain the
// old VA contribution until the resource owner is released.
namespace HandoffDetail {
inline bool ValidPhysicalAlias(const PhysicalMemoryRange& physical, std::uint64_t address) noexcept {
    return physical.identity != 0 && physical.size != 0 && physical.offset <= UINT64_MAX - physical.size &&
        address != 0 && address <= UINT64_MAX - physical.size;
}
inline bool MapOwnerPhysicalAlias(GuardOwner& owner, const PhysicalMemoryRange& physical,
    std::uint64_t address) {
    bool matched = false;
    for (const auto& range : owner.physical) {
        PhysicalMemoryRange overlap;
        if (!IntersectPhysicalRanges(range.physical, physical, &overlap)) continue;
        matched = true;
        const auto begin = address + (overlap.offset - physical.offset);
        const auto first = begin >> MemoryHandoffPageShift;
        const auto last = (begin + overlap.size - 1) >> MemoryHandoffPageShift;
        const auto count = last - first + 1;
        const auto checked = count < MemoryHandoffGuardCount ? count : MemoryHandoffGuardCount;
        for (std::uint64_t index = 0; index < checked; ++index)
            EnrollBucket(owner, static_cast<std::uint32_t>((first + index) & MemoryHandoffGuardMask), range.intent);
    }
    return matched;
}
}
// Enrollment/upgrade only changes this resource. Visiting every owner of a
// shared physical pool here would make N resource births quadratic. A newly
// mapped VA instead uses the all-owner entry below to inherit existing guards.
inline bool MapMemoryHandoffOwnerPhysicalAlias(std::uint64_t owner, const PhysicalMemoryRange& physical,
    std::uint64_t address) noexcept {
    if (owner == 0 || !HandoffDetail::ValidPhysicalAlias(physical, address)) return false;
    auto& registry = HandoffDetail::GetRegistry();
    std::lock_guard lock(registry.mutex);
    if (!HandoffDetail::FreezeLocked(registry)) return false;
    const auto found = registry.owners.find(owner);
    if (found == registry.owners.end()) return false;
    return HandoffDetail::MapOwnerPhysicalAlias(found->second, physical, address);
}
inline bool MapMemoryHandoffPhysicalAlias(const PhysicalMemoryRange& physical, std::uint64_t address) noexcept {
    if (!HandoffDetail::ValidPhysicalAlias(physical, address)) return false;
    auto& registry = HandoffDetail::GetRegistry();
    std::lock_guard lock(registry.mutex);
    if (!HandoffDetail::FreezeLocked(registry)) return false;
    const auto first = registry.physical_owners.lower_bound({physical.identity, 0});
    const auto last = registry.physical_owners.upper_bound({physical.identity, UINT64_MAX});
    for (auto match = first; match != last; ++match) {
        const auto found = registry.owners.find(match->second);
        if (found == registry.owners.end()) return false;
        HandoffDetail::MapOwnerPhysicalAlias(found->second, physical, address);
    }
    return true;
}
inline bool ReleaseMemoryHandoffOwner(std::uint64_t owner) noexcept {
    if (owner == 0) return false;
    auto& registry = HandoffDetail::GetRegistry();
    std::lock_guard lock(registry.mutex);
    const auto found = registry.owners.find(owner);
    if (found == registry.owners.end() || found->second.native_claim_active) return false;
    for (const auto& bucket : found->second.buckets) {
        if ((bucket.second & 2U) != 0) HandoffDetail::DecrementCount(HandoffDetail::guard_owners[bucket.first]);
        if ((bucket.second & 1U) != 0) HandoffDetail::DecrementCount(HandoffDetail::read_guard_owners[bucket.first]);
        HandoffDetail::PublishBucket(bucket.first);
    }
    for (const auto identity : found->second.physical_identities)
        registry.physical_owners.erase({identity, owner});
    registry.owners.erase(found);
    return true;
}

// The caller holds the exclusive execution epoch and has closed the complete
// physical resource/alias set before publishing. A process-wide native GPU
// permit allows only one active claim. Publication alone grants no guest bytes.
inline bool PublishMemoryHandoffNativeClaim(std::uint64_t owner) noexcept {
    if (owner == 0) return false;
    auto& registry = HandoffDetail::GetRegistry();
    std::lock_guard lock(registry.mutex);
    const auto found = registry.owners.find(owner);
    if (!registry.enabled || found == registry.owners.end() ||
        HandoffDetail::native_claim_owner.load(std::memory_order_relaxed) != 0 ||
        found->second.native_claim_active || found->second.physical.empty() || found->second.buckets.empty()) return false;
    // A write-only ordinary guard would allow the JIT's read fast path to
    // miss the claim entirely. The native phase conservatively excludes both.
    for (const auto& bucket : found->second.buckets)
        if (bucket.second != 3) return false;
    for (const auto& range : found->second.physical)
        if (range.intent != MemoryAccessIntent::ReadWrite) return false;
    found->second.native_claim_active = true;
    for (const auto& bucket : found->second.buckets)
        HandoffDetail::native_claim_pages[bucket.first].store(1, std::memory_order_release);
    HandoffDetail::SetNativeClaimReadDirectory(true);
    HandoffDetail::native_claim_owner.store(owner, std::memory_order_release);
    return true;
}

// Native work and its final producer ledger must be complete before retiring.
// No global epoch is required here: this only removes conflicts and wakes CPU
// readers. The plan still owns the guard owner and mapping pin until cleanup.
inline bool RetireMemoryHandoffNativeClaim(std::uint64_t owner) noexcept {
    if (owner == 0) return false;
    auto& registry = HandoffDetail::GetRegistry();
    {
        std::lock_guard lock(registry.mutex);
        const auto found = registry.owners.find(owner);
        if (found == registry.owners.end() || !found->second.native_claim_active ||
            HandoffDetail::native_claim_owner.load(std::memory_order_relaxed) != owner) return false;
        found->second.native_claim_active = false;
        for (const auto& bucket : found->second.buckets)
            HandoffDetail::native_claim_pages[bucket.first].store(0, std::memory_order_release);
        HandoffDetail::native_claim_owner.store(0, std::memory_order_release);
        HandoffDetail::SetNativeClaimReadDirectory(false);
    }
    registry.native_claim_ready.notify_all();
    return true;
}

inline std::uint64_t CurrentMemoryHandoffNativeClaimOwner() noexcept {
    const auto owner = HandoffDetail::native_claim_scope_owner;
    return owner != 0 && HandoffDetail::native_claim_owner.load(std::memory_order_acquire) == owner ? owner : 0;
}

// The native GPU worker adopts the already-published owner on this thread.
// Nested helpers may adopt that same owner; a different or retired owner is
// rejected. Core must additionally prove exact physical coverage for each borrow.
class MemoryHandoffNativeClaimScope final {
public:
    explicit MemoryHandoffNativeClaimScope(std::uint64_t owner) noexcept {
        if (owner == 0 || (HandoffDetail::native_claim_scope_owner != 0 &&
                          HandoffDetail::native_claim_scope_owner != owner)) return;
        auto& registry = HandoffDetail::GetRegistry();
        std::lock_guard lock(registry.mutex);
        if (HandoffDetail::native_claim_owner.load(std::memory_order_relaxed) != owner) return;
        previous_ = HandoffDetail::native_claim_scope_owner;
        owner_ = owner;
        thread_ = &HandoffDetail::native_claim_scope_owner;
        HandoffDetail::native_claim_scope_owner = owner;
    }
    ~MemoryHandoffNativeClaimScope() {
        if (owner_ == 0) return;
        if (thread_ != &HandoffDetail::native_claim_scope_owner ||
            HandoffDetail::native_claim_scope_owner != owner_) std::abort();
        HandoffDetail::native_claim_scope_owner = previous_;
    }
    bool IsActive() const noexcept { return owner_ != 0 && CurrentMemoryHandoffNativeClaimOwner() == owner_; }
    MemoryHandoffNativeClaimScope(const MemoryHandoffNativeClaimScope&) = delete;
    MemoryHandoffNativeClaimScope& operator=(const MemoryHandoffNativeClaimScope&) = delete;
private:
    std::uint64_t owner_ = 0, previous_ = 0;
    const std::uint64_t* thread_ = nullptr;
};

inline bool NativeClaimCoversPhysical(const PhysicalMemoryRange& physical) noexcept {
    const auto owner = CurrentMemoryHandoffNativeClaimOwner();
    if (owner == 0 || physical.identity == 0 || physical.size == 0 ||
        physical.offset > UINT64_MAX - physical.size) return false;
    auto& registry = HandoffDetail::GetRegistry();
    std::lock_guard lock(registry.mutex);
    const auto found = registry.owners.find(owner);
    if (found == registry.owners.end() || !found->second.native_claim_active ||
        HandoffDetail::native_claim_owner.load(std::memory_order_relaxed) != owner) return false;
    auto cursor = physical.offset;
    const auto end = cursor + physical.size;
    // Exact union coverage permits adjacent or overlapping captured planes,
    // but never a gap or a mere hash/VA match. No registry pointer escapes.
    while (cursor < end) {
        auto covered_end = cursor;
        for (const auto& owned : found->second.physical) {
            const auto& range = owned.physical;
            if (range.identity != physical.identity || range.offset > cursor || range.size == 0 ||
                range.offset > UINT64_MAX - range.size) continue;
            const auto range_end = range.offset + range.size;
            if (range_end > covered_end) covered_end = range_end;
        }
        if (covered_end == cursor) return false;
        cursor = covered_end < end ? covered_end : end;
    }
    return true;
}

namespace HandoffDetail {
inline bool NativeClaimBucketConflict(std::uint64_t address, std::uint64_t bytes) noexcept {
    const auto first = address >> MemoryHandoffPageShift;
    const auto last = (address + bytes - 1) >> MemoryHandoffPageShift;
    const auto count = last - first + 1;
    const auto checked = count < MemoryHandoffGuardCount ? count : MemoryHandoffGuardCount;
    for (std::uint64_t index = 0; index < checked; ++index)
        if (native_claim_pages[(first + index) & MemoryHandoffGuardMask].load(std::memory_order_acquire) != 0) return true;
    return false;
}
}

// Call before every clean/guard/bypass shortcut. The CPU keeps its execution
// or HLE reader and all existing guest leases throughout this wait. Therefore
// the native owner must finish and retire without another exclusive admission
// or any dependency on the waiting CPU. Retaining that reader also closes the
// successful-check -> actual-access gap against a subsequent claim publisher.
inline bool WaitForMemoryHandoffNativeClaim(std::uint64_t address, std::uint64_t bytes) noexcept {
    // Preserve the existing caller's validation/bypass semantics when this
    // optional protocol is inactive. Active claims fail closed on bad ranges.
    if (HandoffDetail::native_claim_owner.load(std::memory_order_acquire) == 0) return true;
    if (address == 0 || bytes == 0 || address > UINT64_MAX - bytes) return false;
    if (CurrentMemoryHandoffNativeClaimOwner() != 0 ||
        !HandoffDetail::NativeClaimBucketConflict(address, bytes)) return true;
    auto& registry = HandoffDetail::GetRegistry();
    std::unique_lock lock(registry.mutex);
    const auto ready = [&] {
        return HandoffDetail::native_claim_owner.load(std::memory_order_relaxed) == 0 ||
            !HandoffDetail::NativeClaimBucketConflict(address, bytes);
    };
    if (!ready()) {
        ++registry.native_claim_waiters;
        registry.native_claim_ready.notify_all();
        registry.native_claim_ready.wait(lock, ready);
        --registry.native_claim_waiters;
    }
    return true;
}

inline bool NeedsHandoff(std::uint64_t address, std::uint64_t bytes,
    MemoryAccessIntent intent = MemoryAccessIntent::ReadWrite) noexcept {
    if (bytes == 0 || address > UINT64_MAX - (bytes - 1) || !ValidMemoryAccessIntent(intent)) return true;
    const auto* table = intent == MemoryAccessIntent::Read
        ? HandoffDetail::read_guard_pages : HandoffDetail::guards;
    auto page = address >> MemoryHandoffPageShift;
    const auto last = (address + bytes - 1) >> MemoryHandoffPageShift;
    // At most one table traversal is needed even for a large REP/HLE range.
    const auto count = last - page + 1;
    const auto checked = count < MemoryHandoffGuardCount ? count : MemoryHandoffGuardCount;
    for (std::uint64_t index = 0; index < checked; ++index)
        if (table[(page + index) & MemoryHandoffGuardMask].load(std::memory_order_acquire) != 0)
            return true;
    return false;
}
inline bool PrepareCpuMemoryAccess(std::uint64_t address, std::uint64_t bytes,
    MemoryAccessIntent intent) noexcept {
    if (bytes == 0 || address > UINT64_MAX - bytes || !ValidMemoryAccessIntent(intent)) return false;
    if (!WaitForMemoryHandoffNativeClaim(address, bytes)) return false;
    if (intent == MemoryAccessIntent::Read &&
        HandoffDetail::read_guards.load(std::memory_order_acquire) == nullptr) return true;
    if (HandoffDetail::bypass != 0 || !NeedsHandoff(address, bytes, intent)) return true;
    const auto query = HandoffDetail::query.load(std::memory_order_acquire);
    // A callback failure never grants permission to perform the access.
    return query == nullptr || query(address, bytes, intent);
}

// Graphics publication accesses backing under its own producer proof. It must
// not recursively treat its readback stores as independent CPU write intent.
class MemoryHandoffBypass final {
public:
    MemoryHandoffBypass() noexcept { ++HandoffDetail::bypass; }
    ~MemoryHandoffBypass() { --HandoffDetail::bypass; }
    MemoryHandoffBypass(const MemoryHandoffBypass&) = delete;
    MemoryHandoffBypass& operator=(const MemoryHandoffBypass&) = delete;
};

} // namespace Lumen::GuestCpu
