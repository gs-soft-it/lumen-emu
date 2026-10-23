// Lumen, 2026-10-10. SPDX-License-Identifier: GPL-2.0-only
// Included inside the FEX runtime test namespace. No concurrent guest execution
// is admitted: these tests exercise identity, not interval publication coverage.
bool BackingIdentitySelfTest() noexcept {
    constexpr uint64_t page = 0x4000;
    constexpr int rw = 0x33;
    constexpr int fixed = 0x10;
    bool pass = true;
    const auto check = [&](const char* name, bool ok) {
        std::printf("PS5_BACKING_IDENTITY_TEST case=%s result=%s\n", name, ok ? "PASS" : "FAIL");
        pass &= ok;
        return ok;
    };
    std::vector<Memory::GuestBackingRange> ranges;
    const auto one = [&](uint64_t address, uint64_t bytes, Memory::GuestBackingRange* out) {
        const bool ok = Memory::QueryGuestBackingRanges(address, bytes, &ranges) && ranges.size() == 1;
        if (ok) *out = ranges.front();
        return ok;
    };
    const auto same = [](const auto& left, const auto& right) {
        return left.backing_id == right.backing_id && left.backing_offset == right.backing_offset &&
               left.mapping_id == right.mapping_id;
    };
    ranges.push_back({});
    check("overflow-clears-result", !Memory::QueryGuestBackingRanges(UINT64_MAX - 8, 16, &ranges) && ranges.empty());
    check("zero-size-rejected", !Memory::QueryGuestBackingRanges(page, 0, &ranges) && ranges.empty());
    check("null-result-rejected", !Memory::QueryGuestBackingRanges(page, page, nullptr));

    int64_t physical = 0;
    if (!check("direct-allocate", Memory::KernelAllocateDirectMemory(
            0, Memory::KernelGetDirectMemorySize(), page * 3, page, 0, &physical) == ::OK)) return false;
    void* direct = nullptr;
    if (!check("direct-map", Memory::KernelMapDirectMemory(&direct, page * 3, rw, 0, physical, page) == ::OK)) {
        (void)Memory::KernelCheckedReleaseDirectMemory(physical, page * 3);
        return false;
    }
    const auto base = reinterpret_cast<uint64_t>(direct);
    Memory::GuestBackingRange original {}, left {}, right {}, alias_range {}, current {};
    check("direct-clipped-identity", one(base + page + 8, 32, &original) && original.backing_id != 0 &&
        original.mapping_id != 0 && original.backing_offset == page + 8 &&
        original.kind == Memory::GuestBackingKind::Direct);
    check("direct-edge-identities", one(base + 8, 32, &left) && one(base + page * 2 + 8, 32, &right));
    void* alias = nullptr;
    if (!check("direct-alias-map", Memory::KernelMapDirectMemory(&alias, page, rw, 0, physical + page, page) == ::OK)) {
        (void)Memory::KernelCheckedReleaseDirectMemory(physical, page * 3);
        return false;
    }
    const auto alias_base = reinterpret_cast<uint64_t>(alias);
    check("alias-shares-backing-distinct-view", one(alias_base + 8, 32, &alias_range) &&
        alias_range.backing_id == original.backing_id && alias_range.backing_offset == original.backing_offset &&
        alias_range.mapping_id != original.mapping_id);
    check("protection-split-retains-identities", Memory::KernelMprotect(
        reinterpret_cast<void*>(base + page), page, 0x01) == ::OK &&
        one(base + page + 8, 32, &current) && same(current, original) && current.protection == 0x01);
    check("alias-unmap-invalidates-view", Memory::KernelMunmap(alias_base, page) == ::OK &&
        !Memory::QueryGuestBackingRanges(alias_base, page, &ranges) && ranges.empty());
    check("alias-remap-new-view-same-backing", Memory::KernelMapDirectMemory(
        &alias, page, rw, fixed, physical + page, page) == ::OK &&
        one(alias_base + 8, 32, &current) && current.backing_id == alias_range.backing_id &&
        current.backing_offset == alias_range.backing_offset && current.mapping_id != alias_range.mapping_id);
    if (!check("partial-physical-release", Memory::KernelCheckedReleaseDirectMemory(physical + page, page) == ::OK)) {
        (void)Memory::KernelCheckedReleaseDirectMemory(physical, page * 3);
        return false;
    }
    check("hole-clears-entire-result", !Memory::QueryGuestBackingRanges(base, page * 3, &ranges) && ranges.empty());
    check("release-invalidates-all-aliases", !Memory::QueryGuestBackingRanges(alias_base, page, &ranges) && ranges.empty());
    check("split-survivors-retain-offset-and-view", one(base + 8, 32, &current) && same(current, left) &&
        one(base + page * 2 + 8, 32, &current) && same(current, right));
    int64_t replacement = 0;
    if (!check("reuse-exact-physical-offset", Memory::KernelAllocateDirectMemory(
            physical + page, physical + page * 2, page, page, 0, &replacement) == ::OK && replacement == physical + page)) {
        (void)Memory::KernelCheckedReleaseDirectMemory(physical, page);
        (void)Memory::KernelCheckedReleaseDirectMemory(physical + page * 2, page);
        return false;
    }
    void* middle = reinterpret_cast<void*>(base + page);
    check("physical-reuse-new-backing", Memory::KernelMapDirectMemory(
        &middle, page, rw, fixed, replacement, page) == ::OK && one(base + page + 8, 32, &current) &&
        current.backing_id != original.backing_id && current.mapping_id != original.mapping_id && current.backing_offset == 8);
    check("multi-allocation-range-keeps-distinct-owners", Memory::QueryGuestBackingRanges(base, page * 3, &ranges) &&
        ranges.size() == 3 && ranges[0].backing_id == original.backing_id &&
        ranges[1].backing_id != original.backing_id && ranges[2].backing_id == original.backing_id &&
        ranges[2].backing_offset == page * 2);
    void* combined_alias = nullptr;
    check("single-view-resolves-multiple-physical-owners", Memory::KernelMapDirectMemory(
        &combined_alias, page * 3, rw, 0, physical, page) == ::OK &&
        Memory::QueryGuestBackingRanges(reinterpret_cast<uint64_t>(combined_alias), page * 3, &ranges) &&
        ranges.size() == 3 && ranges[0].backing_id == original.backing_id &&
        ranges[1].backing_id != original.backing_id && ranges[2].backing_id == original.backing_id &&
        ranges[0].mapping_id == ranges[1].mapping_id && ranges[1].mapping_id == ranges[2].mapping_id);
    check("direct-cleanup", Memory::KernelCheckedReleaseDirectMemory(physical, page * 3) == ::OK);

    void* flexible = nullptr;
    if (!check("flexible-map", Memory::KernelMapFlexibleMemory(&flexible, page * 3, rw, 0) == ::OK)) return false;
    const auto flex_base = reinterpret_cast<uint64_t>(flexible);
    check("flexible-identities", one(flex_base + 8, 32, &left) && one(flex_base + page + 8, 32, &original) &&
        one(flex_base + page * 2 + 8, 32, &right) && original.kind == Memory::GuestBackingKind::Flexible);
    check("flexible-protect-retains-identities", Memory::KernelMprotect(
        reinterpret_cast<void*>(flex_base + page), page, 0x01) == ::OK &&
        one(flex_base + page + 8, 32, &current) && same(current, original) && current.protection == 0x01);
    check("flexible-unmap-hole", Memory::KernelMunmap(flex_base + page, page) == ::OK &&
        !Memory::QueryGuestBackingRanges(flex_base, page * 3, &ranges) && ranges.empty());
    check("flexible-survivors-retain-identities", one(flex_base + 8, 32, &current) && same(current, left) &&
        one(flex_base + page * 2 + 8, 32, &current) && same(current, right));
    middle = reinterpret_cast<void*>(flex_base + page);
    check("flexible-reuse-new-identities", Memory::KernelMapFlexibleMemory(&middle, page, rw, fixed) == ::OK &&
        one(flex_base + page + 8, 32, &current) && current.backing_id != original.backing_id &&
        current.mapping_id != original.mapping_id);
    check("flexible-cleanup", Memory::KernelMunmap(flex_base, page * 3) == ::OK);

    const auto private_base = Memory::AllocateRuntimeMemory(0, page * 3, Vm::Mode::ReadWrite, "backing-identity-test");
    if (!check("private-map", private_base != 0)) return false;
    check("private-identities", one(private_base + 8, 32, &left) &&
        one(private_base + page + 8, 32, &original) && one(private_base + page * 2 + 8, 32, &right) &&
        original.kind == Memory::GuestBackingKind::Private && original.backing_offset == page + 8);
    check("private-protect-retains-identities", Memory::ProtectGuestMemory(
        private_base + page, page, Vm::Mode::Read) && one(private_base + page + 8, 32, &current) && same(current, original));
    check("private-unmap-hole", Memory::FreeGuestMemory(private_base + page, page) &&
        !Memory::QueryGuestBackingRanges(private_base, page * 3, &ranges) && ranges.empty());
    check("private-survivors-retain-identities", one(private_base + 8, 32, &current) && same(current, left) &&
        one(private_base + page * 2 + 8, 32, &current) && same(current, right));
    check("private-reuse-new-identities", Memory::AllocateRuntimeMemory(
        private_base + page, page, Vm::Mode::ReadWrite, "backing-identity-test", true) == private_base + page &&
        one(private_base + page + 8, 32, &current) && current.backing_id != original.backing_id &&
        current.mapping_id != original.mapping_id && current.backing_offset == 8);
    check("merged-private-va-keeps-allocation-identities", Memory::QueryGuestBackingRanges(private_base, page * 3, &ranges) &&
        ranges.size() == 3 && ranges[0].backing_id == original.backing_id &&
        ranges[1].backing_id != original.backing_id && ranges[2].backing_id == original.backing_id &&
        ranges[2].backing_offset == page * 2);
    check("private-cleanup", Memory::FreeGuestMemory(private_base, page * 3));

    const auto stack = Memory::AllocateGuestStackMemory(0, page, Vm::Mode::ReadWrite, "backing-stack-identity-test");
    const auto code = Memory::AllocateProgramMemory(0, page, Vm::Mode::ReadWrite, "backing-code-identity-test");
    check("stack-code-distinct-private-identities", stack != 0 && code != 0 && one(stack + 8, 32, &left) &&
        one(code + 8, 32, &right) && left.kind == Memory::GuestBackingKind::Private &&
        right.kind == Memory::GuestBackingKind::Private && left.backing_id != right.backing_id);
    if (stack != 0) check("stack-cleanup", Memory::KernelMunmap(stack, page) == ::OK);
    if (code != 0) check("code-cleanup", Memory::FreeGuestMemory(code, page));

    void* reservation = nullptr;
    if (check("reserve-map", Memory::KernelReserveVirtualRange(&reservation, page, 0, page) == ::OK)) {
        check("reservation-has-no-backing", !Memory::QueryGuestBackingRanges(
            reinterpret_cast<uint64_t>(reservation), page, &ranges) && ranges.empty());
        check("reserve-cleanup", Memory::KernelMunmap(reinterpret_cast<uint64_t>(reservation), page) == ::OK);
    }
    return pass;
}
