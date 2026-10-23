// Lumen, 2026-10-10. SPDX-License-Identifier: GPL-2.0-only
// VM/epoch contract only: no Vulkan resource or CPU/GPU concurrency is claimed.
bool BackingImportSelfTest() noexcept {
    if (Lumen::GuestCpu::Selected() != Lumen::GuestCpu::Provider::Fex) {
        std::puts("PS5_BACKING_IMPORT_TEST supported=0 reason=requires-fex-exclusive-epoch");
        return true;
    }
    constexpr uint64_t page = 0x4000;
    constexpr int rw = 0x33;
    bool pass = true;
    const auto check = [&](const char* name, bool ok) {
        std::printf("PS5_BACKING_IMPORT_TEST case=%s result=%s\n", name, ok ? "PASS" : "FAIL");
        pass &= ok;
        return ok;
    };
    const auto end = [](uint64_t epoch) {
        return LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0;
    };
    const auto denied = [&](uint64_t address, uint64_t size, uint64_t alignment) {
        const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        Memory::GuestGpuBackingImport imported;
        const bool result = epoch != 0 &&
            !Memory::TryAcquireGuestGpuBackingImport(address, size, alignment, &imported) &&
            !imported && imported.HostPointer() == nullptr && imported.ByteSize() == 0;
        imported.Reset();
        return end(epoch) && result;
    };
    int64_t physical {};
    if (!check("direct-allocate", Memory::KernelAllocateDirectMemory(0,
        Memory::KernelGetDirectMemorySize(), page * 4, page, 0, &physical) == ::OK)) return false;
    void* direct = nullptr;
    if (!check("direct-map", Memory::KernelMapDirectMemory(&direct, page * 4, rw, 0, physical, page) == ::OK)) {
        (void)Memory::KernelCheckedReleaseDirectMemory(physical, page * 4);
        return false;
    }
    const auto base = reinterpret_cast<uint64_t>(direct);
    std::array<uint8_t, page * 4> before {};
    before.fill(0xa5);
    check("seed-oracle", Memory::WriteGuestMemory(nullptr, base, before.data(), before.size()));
    void* alias = nullptr;
    check("alias-map", Memory::KernelMapDirectMemory(&alias, page, rw, 0, physical, page) == ::OK);
    Memory::GuestGpuBackingImport imported;
    check("outside-exclusive-denied", !Memory::TryAcquireGuestGpuBackingImport(base, page, page, &imported));
    const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    // The native alias base need not have the guest 16 KiB page alignment on
    // every host. Discover only under the same epoch, then test a full aligned
    // envelope strictly inside the four-page allocation.
    uint64_t aligned_delta {};
    Memory::GuestGpuBackingImport probe;
    if (check("alias-alignment-probe", epoch != 0 &&
        Memory::TryAcquireGuestGpuBackingImport(base, page, 1, &probe))) {
        aligned_delta = (page - (reinterpret_cast<uintptr_t>(probe.HostPointer()) & (page - 1))) & (page - 1);
        probe.Reset();
    }
    const auto output_offset = aligned_delta + 64;
    if (check("aligned-envelope-borrow", epoch != 0 &&
        Memory::TryAcquireGuestGpuBackingImport(base + output_offset, 64, page, &imported))) {
        check("extent-and-offset", imported.ByteSize() == page && imported.GuestOffset() == 64 &&
            (reinterpret_cast<uintptr_t>(imported.HostPointer()) & (page - 1)) == 0);
        const auto* saved = imported.HostPointer();
        check("live-output-cannot-be-overwritten", !Memory::TryAcquireGuestGpuBackingImport(
            base + page, page, page, &imported) && imported.HostPointer() == saved);
        Memory::GuestGpuBackingImport overlap;
        check("physical-alias-overlap-denied", !Memory::TryAcquireGuestGpuBackingImport(
            reinterpret_cast<uint64_t>(alias), page, 1, &overlap) && !overlap);
        check("close-denied-before-effects", !end(epoch) &&
            LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(epoch) != 0);
        check("nested-vm-denied", LumenAndroidFexCoreBeginGuestMemoryMutationV1() == 0);
        auto moved = std::move(imported);
        check("move-transfers-only-owner", !imported && imported.HostPointer() == nullptr &&
            moved && moved.HostPointer() == saved);
        auto* bytes = static_cast<uint8_t*>(moved.HostPointer());
        bytes[moved.GuestOffset()] = 0x5a; // Emulates the completed native producer.
        std::thread completion([owner = std::move(moved)]() mutable { owner.Reset(); });
        completion.join();
    }
    imported.Reset();
    check("close-after-transferable-release", end(epoch));
    std::array<uint8_t, page * 4> after {};
    before[output_offset] = 0x5a;
    check("direct-publication-preserves-padding", Memory::ReadGuestMemory(nullptr, base,
        after.data(), after.size()) && after == before);
    check("invalid-alignment-denied", denied(base, page, 3));
    check("zero-and-overflow-denied", denied(base, 0, page) && denied(UINT64_MAX - 8, 16, page));
    check("gpu-write-rights-revoked", Memory::KernelMprotect(direct, page, 0x01) == ::OK &&
        denied(base + 64, 64, 1));
    check("gpu-rights-restored", Memory::KernelMprotect(direct, page, rw) == ::OK);
    check("executable-alias-denied", Memory::KernelMprotect(alias, page, 0x07) == ::OK &&
        denied(base, page, 1));
    check("executable-alias-restored", Memory::KernelMprotect(alias, page, rw) == ::OK);
    // Remap the second guest page onto physical page 3. Both pieces are valid
    // and writable, but a host import may not pretend they form one span.
    check("discontinuous-unmap", Memory::KernelMunmap(base + page, page) == ::OK);
    void* replacement = reinterpret_cast<void*>(base + page);
    check("discontinuous-remap", Memory::KernelMapDirectMemory(&replacement,
        page, rw, 0x10, physical + page * 3, page) == ::OK);
    check("physical-discontinuity-denied", denied(base, page * 2, page));
    check("direct-cleanup", Memory::KernelCheckedReleaseDirectMemory(physical, page * 4) == ::OK);
    const auto private_base = Memory::AllocateRuntimeMemory(0, page, Vm::Mode::ReadWrite, "import-private-denial");
    check("private-backing-denied", private_base != 0 && denied(private_base, page, page));
    if (private_base != 0) check("private-cleanup", Memory::FreeGuestMemory(private_base, page));
    void* flexible = nullptr;
    if (check("flexible-map", Memory::KernelMapFlexibleMemory(&flexible, page, rw, 0) == ::OK)) {
        const auto flexible_epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        check("flexible-borrow", flexible_epoch != 0 && Memory::TryAcquireGuestGpuBackingImport(
            reinterpret_cast<uint64_t>(flexible), page, 1, &imported));
        imported.Reset();
        check("flexible-close", end(flexible_epoch));
        check("flexible-cleanup", Memory::KernelMunmap(reinterpret_cast<uint64_t>(flexible), page) == ::OK);
    }
    return pass;
}
