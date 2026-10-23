// Lumen, 2026-10-10. SPDX-License-Identifier: GPL-2.0-only
// Included in the native FEX fixture namespace. Each comparison retains the
// same real exclusive epoch for the copied oracle and locked direct hash.
bool BackingHashSelfTest() noexcept {
    constexpr uint64_t page = 0x4000, tracker = 0x1000;
    constexpr uint64_t poison = 0x55aa7711ddcc3399ull;
    bool pass = true;
    const auto check = [&](const char* name, bool ok) {
        std::printf("PS5_BACKING_HASH_TEST case=%s result=%s\n", name, ok ? "PASS" : "FAIL");
        pass &= ok;
        return ok;
    };
    const auto matches = [&](uint64_t address, uint64_t size, uint64_t* observed = nullptr) {
        const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (epoch == 0) return false;
        std::vector<uint8_t> copy(size, 0xa5);
        uint64_t hash = poison;
        const bool ok = Memory::TryReadSparseBacking(address, copy.data(), copy.size()) &&
            Memory::TryHashContiguousBacking(address, size, &hash) &&
            hash == XXH3_64bits(copy.data(), copy.size());
        if (observed != nullptr) *observed = hash;
        const bool ended = LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0;
        return ok && ended;
    };
    const auto denied = [&](uint64_t address, uint64_t size) {
        uint64_t hash = poison;
        return !Memory::TryHashContiguousBacking(address, size, &hash) && hash == poison;
    };
    check("invalid-output-preserved", denied(0, tracker) && denied(UINT64_MAX - 7, 16) &&
        denied(page, 0) && !Memory::TryHashContiguousBacking(page, tracker, nullptr));
    int64_t physical = 0;
    if (!check("direct-allocate", Memory::KernelAllocateDirectMemory(
            0, Memory::KernelGetDirectMemorySize(), page * 3, page, 0, &physical) == ::OK)) return false;
    void* mapping = nullptr;
    if (!check("direct-map", Memory::KernelMapDirectMemory(&mapping, page * 3, 0x33,
            0, physical, page) == ::OK)) {
        (void)Memory::KernelCheckedReleaseDirectMemory(physical, page * 3);
        return false;
    }
    const auto base = reinterpret_cast<uint64_t>(mapping);
    std::vector<uint8_t> bytes(page * 3);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>((i * 37) ^ (i >> 7) ^ 0xd3);
    Memory::WriteBacking(base, bytes.data(), bytes.size());
    uint64_t initial = 0, changed = 0;
    check("whole-tracker-page", matches(base, tracker, &initial));
    check("unaligned-exact-span", matches(base + 17, tracker + 31));
    check("last-byte-and-padding-included", matches(base, bytes.size()));

    void* alias = nullptr;
    if (check("direct-alias", Memory::KernelMapDirectMemory(&alias, page, 0x33,
            0, physical, page) == ::OK)) {
        const auto alias_base = reinterpret_cast<uint64_t>(alias);
        uint64_t alias_hash = 0;
        check("alias-identical-bytes", matches(alias_base, tracker, &alias_hash) && alias_hash == initial);
        const uint8_t last_byte = static_cast<uint8_t>(bytes[tracker - 1] ^ 0xff);
        const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        uint64_t before = poison;
        std::array<uint8_t, tracker> oracle {};
        bool repeated = epoch != 0 && Memory::TryHashContiguousBacking(base, tracker, &before);
        if (epoch != 0) {
            Memory::WriteBacking(alias_base + tracker - 1, &last_byte, 1);
            bytes[tracker - 1] = last_byte;
            repeated &= Memory::TryHashContiguousBacking(base, tracker, &changed) &&
                Memory::TryReadSparseBacking(base, oracle.data(), oracle.size()) &&
                changed == XXH3_64bits(oracle.data(), oracle.size()) && changed != before && before == initial;
            repeated &= LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0;
        }
        check("same-epoch-not-memoized", repeated);
        check("alias-mutation-visible", matches(alias_base, tracker, &alias_hash) && alias_hash == changed);
        check("alias-unmap", Memory::KernelMunmap(alias_base, page) == ::OK && denied(alias_base, tracker));
    }
    check("middle-unmap", Memory::KernelMunmap(base + page, page) == ::OK);
    std::vector<uint8_t> sparse(bytes.size(), 0xa5);
    const bool sparse_read = Memory::TryReadSparseBacking(base, sparse.data(), sparse.size());
    std::fill(bytes.begin() + page, bytes.begin() + page * 2, 0);
    check("hole-fallback-zeroes-only-hole", denied(base, bytes.size()) &&
        denied(base + page - 16, 32) && sparse_read && sparse == bytes);
    check("split-survivors-remain-eligible", matches(base, tracker) && matches(base + page * 2, tracker));
    void* middle = reinterpret_cast<void*>(base + page);
    check("remap-middle", Memory::KernelMapDirectMemory(&middle, page, 0x33,
        0x10, physical + page, page) == ::OK);
    std::vector<uint8_t> replacement(page, 0x69);
    Memory::WriteBacking(base + page, replacement.data(), replacement.size());
    std::copy(replacement.begin(), replacement.end(), bytes.begin() + page);
    check("adjacent-views-fall-back-with-exact-bytes", denied(base, bytes.size()) &&
        Memory::TryReadSparseBacking(base, sparse.data(), sparse.size()) && sparse == bytes &&
        matches(base + page, tracker));
    check("release-invalidates-hash-lifetime", Memory::KernelCheckedReleaseDirectMemory(
        physical, page * 3) == ::OK && denied(base, tracker));
    int64_t reused = -1;
    if (check("reuse-physical", Memory::KernelAllocateDirectMemory(physical, physical + page,
            page, page, 0, &reused) == ::OK && reused == physical)) {
        mapping = reinterpret_cast<void*>(base);
        if (check("reuse-virtual", Memory::KernelMapDirectMemory(&mapping, page, 0x33,
                0x10, reused, page) == ::OK)) {
            Memory::WriteBacking(base, replacement.data(), replacement.size());
            uint64_t reused_hash = 0;
            check("reused-view-hashes-new-content", matches(base, tracker, &reused_hash) &&
                reused_hash != initial && reused_hash != changed);
        }
        check("reuse-cleanup", Memory::KernelCheckedReleaseDirectMemory(reused, page) == ::OK);
    }
    void* flexible = nullptr;
    if (check("flexible-map", Memory::KernelMapFlexibleMemory(&flexible, page, 0x33, 0) == ::OK)) {
        const auto flex = reinterpret_cast<uint64_t>(flexible);
        Memory::WriteBacking(flex, replacement.data(), replacement.size());
        check("flexible-oracle", matches(flex + 31, tracker));
        check("guest-read-rights-do-not-change-backing-reader", Memory::KernelMprotect(flexible,
            page, 0x30) == ::OK && matches(flex, tracker));
        check("flexible-cleanup", Memory::KernelMunmap(flex, page) == ::OK);
    }
    const auto private_base = Memory::AllocateRuntimeMemory(0, page, Vm::Mode::ReadWrite, "backing-hash-private");
    if (check("private-map", private_base != 0)) {
        std::array<uint8_t, tracker> copy;
        copy.fill(0xa5);
        check("private-denied-reader-unchanged", denied(private_base, tracker) &&
            !Memory::TryReadSparseBacking(private_base, copy.data(), copy.size()) &&
            std::all_of(copy.begin(), copy.end(), [](auto byte) { return byte == 0xa5; }));
        check("private-cleanup", Memory::FreeGuestMemory(private_base, page));
    }
    int64_t pool_physical = 0;
    constexpr uint64_t pool_page = 0x10000, pool_reservation = 0x200000;
    void* pool = nullptr;
    if (check("pool-expand", Memory::KernelMemoryPoolExpand(0, Memory::KernelGetDirectMemorySize(),
            pool_page, pool_page, &pool_physical) == ::OK) &&
        check("pool-reserve", Memory::KernelMemoryPoolReserve(nullptr, pool_reservation,
            pool_reservation, 0, &pool) == ::OK)) {
        const auto pool_base = reinterpret_cast<uint64_t>(pool);
        if (check("pool-commit", Memory::KernelMemoryPoolCommit(pool, pool_page, 0, 0x33, 0) == ::OK)) {
            Memory::WriteBacking(pool_base, replacement.data(), replacement.size());
            std::array<uint8_t, tracker> copy;
            check("pool-fallback-keeps-bytes", denied(pool_base, tracker) &&
                Memory::TryReadSparseBacking(pool_base, copy.data(), copy.size()) &&
                std::equal(copy.begin(), copy.end(), replacement.begin()));
            check("pool-decommit", Memory::KernelMemoryPoolDecommit(pool, pool_page, 0) == ::OK);
        }
        check("pool-cleanup", Memory::KernelMunmap(pool_base, pool_reservation) == ::OK);
        // Expanded physical pages remain in the existing pool until shutdown.
    }
    return pass;
}
