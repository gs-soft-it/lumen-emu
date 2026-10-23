// Lumen, 2026-10-09. SPDX-License-Identifier: GPL-2.0-only
// Original synthetic x86 programs; this gate executes the real Darwin FEX JIT.
#include "AndroidFexCoreRuntime.h"
#include "MemoryHandoff.h"
#include "MemoryHandoffPageCache.h"
#include <FEXCore/Utils/LumenMemoryHandoffAbi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace {
using Lumen::GuestCpu::MemoryAccessIntent;
struct Access { std::uint64_t address, bytes; MemoryAccessIntent intent; };
struct Probe {
    std::uint64_t code {}, data {}, page {}, root {};
    std::vector<Access> accesses;
    bool deny {};
    bool cache_enabled {}, cache_dirty {}, cache_pending {}, cache_shape_valid {true};
    std::uint64_t cache_epoch {}, page_proofs {};
    std::uint64_t bulk_allowed {}, bulk_denied {};
    Lumen::GuestCpu::MemoryHandoffPageCache cache;
    std::atomic<bool> pause {}, entered {}, release {};
} probe;

bool Handoff(std::uint64_t address, std::uint64_t bytes, MemoryAccessIntent intent) noexcept {
    if (address < probe.data || address - probe.data >= 2 * probe.page) return true;
    probe.accesses.push_back({address, bytes, intent});
    if (probe.cache_enabled) {
        const auto epoch = LumenAndroidFexCoreGuestExecutionReadAdmissionEpochV1();
        probe.cache_shape_valid &= epoch != 0;
        probe.cache_epoch = epoch;
        if (probe.cache.Try(epoch, address, bytes, intent,
                [](std::uint64_t page, std::uint64_t size, MemoryAccessIntent access) noexcept {
                    ++probe.page_proofs;
                    probe.cache_shape_valid &= page == probe.data && size == 4096;
                    return !probe.cache_dirty &&
                        (!Lumen::GuestCpu::WritesMemory(access) || probe.cache_pending);
                })) return true;
        // Model the fresh resolver, including invalidation before the store.
        // A denied dirty read must still reach this path after readmission.
        if (!probe.deny && Lumen::GuestCpu::WritesMemory(intent)) probe.cache_pending = true;
    }
    if (probe.pause.load(std::memory_order_acquire)) {
        probe.entered.store(true, std::memory_order_release);
        while (!probe.release.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    return !probe.deny;
}
std::uint32_t QueryCode(void*, std::uint64_t address, std::uint64_t* base,
    std::uint64_t* bytes, std::uint32_t* writable) noexcept {
    if (address < probe.code || address - probe.code >= probe.page) return 0;
    *base = probe.code; *bytes = probe.page; *writable = 0;
    return 1;
}
std::uint32_t QueryData(std::uint64_t address, std::uint64_t bytes, std::uint32_t write) noexcept {
    if (write > 1 || bytes == 0) return 0;
    if (address >= probe.data && bytes <= 6 * probe.page && address - probe.data <= 6 * probe.page - bytes) return 1;
    return !write && address >= probe.code && bytes <= probe.page && address - probe.code <= probe.page - bytes;
}
std::uint32_t HostCall(void*, std::uint64_t, std::uint64_t*, std::uint64_t*) noexcept {
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
}
std::uint32_t Run(unsigned offset, std::uint64_t destination, std::uint64_t source,
    std::uint64_t value, std::uint64_t count, LumenAndroidFexCoreGuestStateV1* result) {
    probe.accesses.clear();
    probe.bulk_allowed = probe.bulk_denied = 0;
    *result = {};
    result->structSize = sizeof(*result); result->abiVersion = 1;
    result->rip = probe.code + offset;
    result->gprs[0] = value; result->gprs[1] = count;
    result->gprs[4] = probe.data + 5 * probe.page - 8;
    result->gprs[6] = source; result->gprs[7] = destination;
    std::memcpy(reinterpret_cast<void*>(result->gprs[4]), &probe.root, 8);
    return LumenAndroidFexCoreRunGuestV1(result, HostCall, QueryCode, nullptr);
}
bool PublishRange(bool dirty, std::uint64_t address, std::uint64_t bytes, MemoryAccessIntent intent) {
    const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
        LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
    if (token == 0) return false;
    const bool owns = LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(token) == 1;
    if (owns) {
        if (bytes != 0) Lumen::GuestCpu::MarkHandoffRange(address, bytes, intent);
        Lumen::GuestCpu::PublishMemoryHandoffGpuDirty(dirty);
    }
    return LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 1 && owns;
}
bool Publish(bool dirty, bool enroll = false, MemoryAccessIntent intent = MemoryAccessIntent::ReadWrite) {
    return PublishRange(dirty, probe.data, enroll ? 2 * probe.page : 0, intent);
}
bool Covers(std::uint64_t address, std::uint64_t bytes, bool write) {
    for (std::uint64_t byte = 0; byte < bytes; ++byte)
        if (std::none_of(probe.accesses.begin(), probe.accesses.end(), [&](const auto& access) {
            return address + byte >= access.address && address + byte - access.address < access.bytes &&
                Lumen::GuestCpu::WritesMemory(access.intent) == write;
        })) return false;
    return true;
}
bool OnlyWrites() {
    return !probe.accesses.empty() && std::all_of(probe.accesses.begin(), probe.accesses.end(), [](const auto& access) {
        return Lumen::GuestCpu::WritesMemory(access.intent);
    });
}
bool OwnerContract() {
    if (LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(0) != 0) return false;
    const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
        LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
    if (token == 0) return false;
    bool foreign = false;
    std::thread other([&] { foreign = LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(token) == 0; });
    other.join();
    bool valid = foreign && LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(token) == 1 &&
        LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(token + 1) == 0;
    valid &= LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 1;
    valid &= LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(token) == 0;
    const auto additive = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
        LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_ADDITIVE_MAPPING_V2);
    if (additive == 0) return false;
    valid &= LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(additive) == 0;
    valid &= LumenAndroidFexCoreEndGuestMemoryMutationV2(additive, 0, 0, 0, nullptr, nullptr) == 1;
    return valid;
}
bool ConcurrentEpoch(std::uint64_t address, std::uint64_t before, std::uint64_t after) {
    if (!Publish(true)) return false;
    std::memcpy(reinterpret_cast<void*>(address), &before, 8);
    probe.entered = false; probe.release = false; probe.pause = true;
    LumenAndroidFexCoreGuestStateV1 result {};
    std::uint32_t status {};
    std::atomic<bool> attempted {}, acquired {}, closed {};
    std::thread guest([&] { status = Run(0, address, 0, 0, 0, &result); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!probe.entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const bool entered = probe.entered.load(std::memory_order_acquire);
    std::thread writer([&] {
        attempted.store(true, std::memory_order_release);
        const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
            LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
        if (token == 0) return;
        acquired.store(true, std::memory_order_release);
        if (LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(token) == 1) {
            std::memcpy(reinterpret_cast<void*>(address), &after, 8);
            Lumen::GuestCpu::PublishMemoryHandoffGpuDirty(true);
        }
        closed.store(LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 1,
            std::memory_order_release);
    });
    while (!attempted.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool blocked = !acquired.load(std::memory_order_acquire);
    probe.release.store(true, std::memory_order_release);
    guest.join(); writer.join(); probe.pause = false;
    std::uint64_t stored {}; std::memcpy(&stored, reinterpret_cast<const void*>(address), 8);
    return entered && blocked && acquired && closed && status == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
        result.gprs[0] == before && stored == after;
}
} // namespace

extern "C" void FEXLumenGuestMemoryBulkDecisionV1(std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint32_t allowed) noexcept {
    if (allowed != 0) ++probe.bulk_allowed; else ++probe.bulk_denied;
}

int main() {
    probe.page = static_cast<std::uint64_t>(getpagesize());
    if (probe.page != 16384) return 2;
    auto* code = static_cast<std::uint8_t*>(mmap(nullptr, probe.page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    auto* data = static_cast<std::uint8_t*>(mmap(nullptr, 6 * probe.page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (code == MAP_FAILED || data == MAP_FAILED) return 2;
    probe.code = reinterpret_cast<std::uintptr_t>(code); probe.data = reinterpret_cast<std::uintptr_t>(data);
    const auto install = [&](unsigned offset, std::initializer_list<std::uint8_t> bytes) {
        std::copy(bytes.begin(), bytes.end(), code + offset);
    };
    install(0, {0x48,0x8b,0x07,0xc3}); // mov rax,[rdi]; ret
    install(32, {0x48,0x89,0x07,0xc3}); // mov [rdi],rax; ret
    install(64, {0xf0,0x48,0x0f,0xb1,0x0f,0xc3}); // lock cmpxchg [rdi],rcx; ret
    install(96, {0xf3,0x0f,0x6f,0x06,0xf3,0x0f,0x7f,0x07,0xc3}); // movdqu xmm0,[rsi]; movdqu [rdi],xmm0
    install(128, {0xfc,0xf3,0xa4,0xc3}); // cld; rep movsb; ret
    install(160, {0xdb,0x2e,0xdb,0x3f,0xc3}); // fld tbyte[rsi]; fstp tbyte[rdi]; ret
    install(224, {0x49,0xc7,0xc0,0x11,0,0,0,0x49,0xc7,0xc1,0x22,0,0,0,
        0xf9,0x48,0x8b,0x07,0x4d,0x11,0xc8,0x4c,0x89,0x06,0xc3}); // live r8/r9/carry across read
    install(288, {0x48,0x89,0x07,0x48,0xff,0xc9,0x75,0xf8,0xc3}); // repeated same-value stores
    if (mprotect(code, probe.page, PROT_READ | PROT_EXEC) != 0 ||
        LumenMacOSFexCoreRegisterDataAccessQueryV1(QueryData) != 1) return 2;
    LumenAndroidFexCoreRuntimeStatusV1 runtime {};
    if (LumenAndroidFexCoreRuntimeProbeV1(&runtime) != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) return 2;
    probe.root = LumenAndroidFexCoreRootReturnAddressV1();
    unsigned cases {}, passed {};
    const auto check = [&](bool success, const char* label) {
        ++cases; passed += success ? 1U : 0U;
        std::printf("%s macos_memory_handoff %s\n", success ? "PASS" : "FAIL", label);
    };
    check(LumenAndroidFexCoreMemoryHandoffInstrumentationReadyV1() == 1, "real_JIT_instrumentation_ready");
    if (passed != cases) return 3;
    check(FEXLumenGuestMemoryCheckProviderV1() != nullptr &&
        FEXLumenGuestMemoryReadGuardProviderV1() == Lumen::GuestCpu::MemoryHandoffReadGuardDirectory() &&
        (FEX_LUMEN_HAS_MEMORY_PRESERVE_ALL_ABI == 0 || FEXLumenGuestMemoryPreserveAllProviderV1() != nullptr),
        "configured_typed_callback_and_read_directory_providers");
    check(OwnerContract(), "real_exclusive_owner_foreign_stale_additive_denied");
    check(Lumen::GuestCpu::RegisterMemoryHandoffQuery(Handoff), "runtime_query_registered");
    constexpr std::uint64_t value = UINT64_C(0x8123456789abcdef);
    const auto crossing = probe.data + 4096 - 4;
    std::memcpy(reinterpret_cast<void*>(crossing), &value, 8);
    LumenAndroidFexCoreGuestStateV1 result {};
    const auto ready = [&](unsigned offset, std::uint64_t dst, std::uint64_t src = 0,
        std::uint64_t input = 0, std::uint64_t count = 0) {
        return Run(offset, dst, src, input, count, &result) == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1;
    };
    check(ready(0, crossing) && result.gprs[0] == value && probe.accesses.empty(), "unwatched_direct_access");
    // Exercise the new tables before legacy enrollment makes the input's READ
    // bits sticky. A different page models a dirty GPU writer throughout.
    check(PublishRange(true, probe.data + 2 * probe.page, 4096, MemoryAccessIntent::ReadWrite) &&
        Publish(true, true, MemoryAccessIntent::Write) &&
        Lumen::GuestCpu::MemoryHandoffReadGuardDirectory()->load(std::memory_order_acquire) ==
            Lumen::GuestCpu::MemoryHandoffReadGuardTable() &&
        !Lumen::GuestCpu::NeedsHandoff(crossing, 8, MemoryAccessIntent::Read) &&
        Lumen::GuestCpu::NeedsHandoff(crossing, 8, MemoryAccessIntent::Write),
        "split_write_only_input_with_unrelated_dirty_writer");
    probe.deny = true;
    check(ready(0, crossing) && result.gprs[0] == value && probe.accesses.empty(),
        "split_same_translation_cross_page_scalar_read_skips_resolver");
    probe.deny = false;
    check(ready(32, crossing, 0, value) && Covers(crossing, 8, true) && OnlyWrites(),
        "split_write_only_input_store_still_checked");
    const auto aligned = probe.data + 256;
    std::memcpy(reinterpret_cast<void*>(aligned), &value, 8);
    check(ready(64, aligned, 0, ~value, value) && result.gprs[0] == value &&
        *reinterpret_cast<const std::uint64_t*>(data + 256) == value && Covers(aligned, 8, true) && OnlyWrites(),
        "split_failed_CAS_still_has_write_intent");
    for (const unsigned offset: {32u, 64u}) {
        probe.deny = true;
        const auto status = Run(offset, aligned, 0, ~value, value, &result);
        probe.deny = false;
        check(status == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 && result.rip == probe.code + offset &&
            *reinterpret_cast<const std::uint64_t*>(data + 256) == value && Covers(aligned, 8, true),
            offset == 32 ? "split_denied_store_has_no_effect" : "split_denied_failed_CAS_has_no_effect");
    }
    const auto split_source = probe.data + 8192 - 8;
    for (unsigned byte = 0; byte < 16; ++byte)
        reinterpret_cast<std::uint8_t*>(split_source)[byte] = static_cast<std::uint8_t>(byte + 0x60);
    check(ready(96, probe.data + 128, split_source) &&
        std::memcmp(reinterpret_cast<const void*>(split_source), data + 128, 16) == 0 &&
        Covers(probe.data + 128, 16, true) && OnlyWrites(), "split_cross_page_SIMD_reads_skip_stores_checked");
    check(ready(128, probe.data + 160, split_source, 0, 13) &&
        std::memcmp(reinterpret_cast<const void*>(split_source), data + 160, 13) == 0 && result.gprs[1] == 0 &&
        result.gprs[6] == split_source + 13 && result.gprs[7] == probe.data + 173 &&
        Covers(probe.data + 160, 13, true) && OnlyWrites(), "split_cross_page_REP_reads_skip_stores_checked");
    const auto bulk_destination = probe.data + 3 * probe.page;
    check(!Lumen::GuestCpu::NeedsHandoff(bulk_destination, 13) &&
        ready(128, bulk_destination, split_source, 0, 13) &&
        std::memcmp(reinterpret_cast<const void*>(split_source), reinterpret_cast<const void*>(bulk_destination), 13) == 0 &&
        result.gprs[1] == 0 && result.gprs[6] == split_source + 13 && result.gprs[7] == bulk_destination + 13 &&
        probe.bulk_allowed == 1 && probe.bulk_denied == 0 && probe.accesses.empty(),
        "rep_bulk_READ_source_write_only_watch_cross_page_allowed");
    check(Publish(true, true, MemoryAccessIntent::Read) && ready(0, crossing) && result.gprs[0] == value &&
        Covers(crossing, 8, false), "split_read_upgrade_reuses_translation_and_enters_resolver");
    check(ready(128, bulk_destination, split_source, 0, 13) &&
        std::memcmp(reinterpret_cast<const void*>(split_source), reinterpret_cast<const void*>(bulk_destination), 13) == 0 &&
        probe.bulk_allowed == 0 && probe.bulk_denied == 1 && Covers(split_source, 13, false),
        "rep_bulk_same_translation_READ_upgrade_denies_bulk_before_effects");
    check(ready(96, probe.data + 128, split_source) && Covers(split_source, 16, false) &&
        Covers(probe.data + 128, 16, true), "split_upgraded_cross_page_SIMD_reads_checked");
    check(ready(128, probe.data + 160, split_source, 0, 13) && Covers(split_source, 13, false) &&
        Covers(probe.data + 160, 13, true), "split_upgraded_cross_page_REP_reads_checked");
    check(Publish(true, true) && ready(0, crossing) && result.gprs[0] == value &&
        Covers(crossing, 8, false), "dirty_read_cross_guest4KiB_on_host16KiB");
    check(Publish(false), "publish_clean_under_real_exclusive_epoch");
    probe.deny = true;
    check(ready(128, bulk_destination, split_source, 0, 13) &&
        std::memcmp(reinterpret_cast<const void*>(split_source), reinterpret_cast<const void*>(bulk_destination), 13) == 0 &&
        probe.bulk_allowed == 1 && probe.bulk_denied == 0 && probe.accesses.empty(),
        "rep_bulk_clean_ledger_skips_enrolled_source_READ");
    probe.deny = false;
    check(ready(128, split_source, split_source, 0, 13) &&
        probe.bulk_allowed == 0 && probe.bulk_denied == 1 && Covers(split_source, 13, true) && OnlyWrites(),
        "rep_bulk_clean_alias_retains_destination_WRITE_guard");
    check(Publish(true) && ready(128, bulk_destination, split_source, 0, 13) &&
        probe.bulk_allowed == 0 && probe.bulk_denied == 1 && Covers(split_source, 13, false),
        "rep_bulk_clean_to_dirty_same_translation_rechecks_source");
    std::array<std::uint8_t, 13> before_bulk {};
    std::memcpy(before_bulk.data(), reinterpret_cast<const void*>(bulk_destination), before_bulk.size());
    probe.deny = true;
    const auto overflow_bulk = Run(128, bulk_destination, split_source, 0, UINT64_MAX, &result);
    probe.deny = false;
    check(overflow_bulk == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
        probe.bulk_allowed == 0 && probe.bulk_denied == 1 && result.gprs[1] == UINT64_MAX &&
        std::memcmp(before_bulk.data(), reinterpret_cast<const void*>(bulk_destination), before_bulk.size()) == 0,
        "rep_bulk_overflow_denied_before_first_copy_effect");
    check(Publish(false), "rep_bulk_restore_clean_ledger");
    probe.deny = true;
    check(ready(0, crossing) && result.gprs[0] == value && probe.accesses.empty(), "same_translation_clean_read_no_callback");
    probe.deny = false;
    check(ready(32, crossing, 0, value) && Covers(crossing, 8, true) && OnlyWrites(), "clean_same_value_store_keeps_write_intent");
    std::memcpy(reinterpret_cast<void*>(aligned), &value, 8);
    check(ready(64, aligned, 0, value, value) && result.gprs[0] == value && Covers(aligned, 8, true),
        "clean_successful_same_value_CAS_keeps_write_intent");
    check(ready(64, aligned, 0, ~value, value) && result.gprs[0] == value && Covers(aligned, 8, true),
        "clean_failed_CAS_keeps_write_intent");
    for (unsigned byte = 0; byte < 32; ++byte) data[64 + byte] = static_cast<std::uint8_t>(byte + 0x80);
    check(ready(96, probe.data + 128, probe.data + 64) && std::memcmp(data + 64, data + 128, 16) == 0 &&
        Covers(probe.data + 128, 16, true) && OnlyWrites(), "clean_SIMD_read_skip_store_checked");
    check(ready(128, probe.data + 160, probe.data + 64, 0, 13) && std::memcmp(data + 64, data + 160, 13) == 0 &&
        result.gprs[1] == 0 && result.gprs[6] == probe.data + 77 && result.gprs[7] == probe.data + 173 &&
        Covers(probe.data + 160, 13, true) && OnlyWrites(), "clean_REP_scratch_restored_stores_checked");
    const std::array<std::uint8_t, 10> one {0,0,0,0,0,0,0,0x80,0xff,0x3f};
    std::memcpy(data + 320, one.data(), one.size());
    check(ready(160, probe.data + 352, probe.data + 320) && std::memcmp(data + 352, one.data(), one.size()) == 0 &&
        Covers(probe.data + 352, 10, true) && OnlyWrites(), "clean_x87_full80bit_footprint");
    check(Publish(true) && ready(224, crossing, probe.data + 384) && result.gprs[0] == value &&
        *reinterpret_cast<const std::uint64_t*>(data + 384) == 0x34 && Covers(crossing, 8, false),
        "configured_ABI_live_GPR_and_carry_preserved");
    check(ready(96, probe.data + 128, probe.data + 64) && std::memcmp(data + 64, data + 128, 16) == 0 &&
        Covers(probe.data + 64, 16, false) && Covers(probe.data + 128, 16, true),
        "configured_ABI_live_SIMD_across_watched_store");
    const auto denied = [&](unsigned offset, bool dirty) {
        if (!Publish(dirty)) return false;
        std::array<std::uint8_t, 16> before {};
        std::memcpy(before.data(), reinterpret_cast<const void*>(crossing - 4), before.size());
        probe.deny = true;
        const auto status = Run(offset, crossing, 0, ~value, 0, &result);
        probe.deny = false;
        return status == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 && result.rip == probe.code + offset &&
            Covers(crossing, 8, offset != 0) && probe.accesses.size() == 1 &&
            std::memcmp(before.data(), reinterpret_cast<const void*>(crossing - 4), before.size()) == 0;
    };
    check(denied(32, false), "clean_denied_store_precise_RIP_no_access");
    check(denied(0, true), "republished_dirty_same_translation_denied_read_precise_RIP");
    check(ConcurrentEpoch(aligned, value, ~value), "real_writer_waits_through_callback_and_guest_load");
    check(ready(0, aligned) && result.gprs[0] == ~value && Covers(aligned, 8, false),
        "same_translation_observes_published_writer");
    probe.cache_enabled = true;
    probe.cache_dirty = false;
    probe.cache_pending = false;
    check(Publish(false) && ready(32, aligned, 0, value) && probe.page_proofs == 1 &&
        probe.cache_pending && probe.cache.GetCounters().hits == 0 && probe.cache_shape_valid,
        "real_execution_first_store_uses_fresh_resolver");
    const auto first_epoch = probe.cache_epoch;
    const auto proofs_before = probe.page_proofs;
    check(ready(288, aligned, 0, value, 64) && probe.accesses.size() == 64 && result.gprs[1] == 0 &&
        *reinterpret_cast<const std::uint64_t*>(data + 256) == value &&
        probe.cache_epoch != first_epoch && probe.page_proofs == proofs_before + 1 &&
        probe.cache.GetCounters().hits == 63 && probe.cache_shape_valid,
        "real_execution_64_same_value_stores_avoid_63_collectors");
    const auto previous_epoch = probe.cache_epoch;
    const auto previous_hits = probe.cache.GetCounters().hits;
    // Real exclusive ownership/readmission; only the physical GPU peers are
    // modeled. Equal-byte upload still revokes the earlier positive proof.
    check(Publish(false), "real_execution_cache_upload_exclusive");
    probe.cache_pending = false;
    const auto upload_proofs = probe.page_proofs;
    check(ready(32, aligned, 0, value) && probe.cache_epoch != previous_epoch &&
        probe.page_proofs == upload_proofs + 1 && probe.cache.GetCounters().hits == previous_hits,
        "real_execution_equal_upload_rearms_first_store");
    check(Publish(true), "real_execution_cache_dirty_exclusive");
    probe.cache_dirty = true;
    probe.deny = true;
    const auto dirty_proofs = probe.page_proofs;
    const auto dirty_status = Run(0, aligned, 0, 0, 0, &result);
    check(dirty_status == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 && result.rip == probe.code &&
        probe.page_proofs == dirty_proofs + 1 && probe.cache.GetCounters().hits == previous_hits &&
        probe.cache_shape_valid && LumenAndroidFexCoreGuestExecutionReadAdmissionEpochV1() == 0,
        "real_execution_dirty_readmission_cannot_reuse_clean_proof");
    probe.deny = false;
    probe.cache_enabled = false;
    std::printf("FEX_MACOS_MEMORY_HANDOFF cases=%u/%u host_page=%llu guest_page=4096 guest_execution=1"
        " preserve_all_configured=%u tracker_GPU=modeled mixed_4KiB_permissions=not_claimed\n", passed, cases,
        static_cast<unsigned long long>(probe.page), FEX_LUMEN_HAS_MEMORY_PRESERVE_ALL_ABI);
    return passed == cases ? 0 : 3;
}
