// Lumen, 2026-10-10. SPDX-License-Identifier: GPL-2.0-only
// Included after the common actual-x86/HLE fixture helpers.
namespace {
struct ShortUnlockWitness { bool before {}, after {}; unsigned calls {}; } short_unlock_witness;
int KYTY_SYSV_ABI NativeAuditedUnlock(Libs::LibKernel::PthreadMutex* slot) {
    short_unlock_witness.before = Lumen::GuestCpu::OwnsShortHostCallAdmission();
    ++short_unlock_witness.calls;
    const int result = Libs::LibKernel::PthreadMutexUnlock(slot);
    short_unlock_witness.after = Lumen::GuestCpu::OwnsShortHostCallAdmission();
    return result;
}
}

bool TestShortGuestMemoryUnlock(std::uint64_t stack_top) {
    namespace P = Libs::LibKernel;
    using namespace Lumen::GuestCpu;
    Loader::SymbolDatabase symbols;
    Libs::InitLibKernel_1_Pthread(&symbols);
    const auto* kernel = symbols.FindByNid("tn3VlD0hG60", Loader::SymbolType::Func);
    const auto* posix = symbols.FindByNid("2Z+PpY6CaJg", Loader::SymbolType::Func);
    bool pass = true;
    const auto audited = MakeHostCallAdmissionPolicy(HostCallEffect::GuestMemory,
        HostCallShortContract::SynchronousGuestMemory);
    std::fprintf(stderr, "PS5_SYNC_SHORT_IMPORT_POLICY kernel=%#llx kernel_policy=%#llx "
        "posix=%#llx posix_policy=%#llx expected=%#llx registry_ready=%u\n",
        static_cast<unsigned long long>(kernel != nullptr ? kernel->vaddr : 0),
        static_cast<unsigned long long>(kernel != nullptr ? HostCallAdmissionPolicyFor(kernel->vaddr) : 0),
        static_cast<unsigned long long>(posix != nullptr ? posix->vaddr : 0),
        static_cast<unsigned long long>(posix != nullptr ? HostCallAdmissionPolicyFor(posix->vaddr) : 0),
        static_cast<unsigned long long>(audited), HostCallRegistryReady() ? 1U : 0U);
    CheckCase("sync-short-production-unlock-imports-retain-guest-memory-effects",
        kernel != nullptr && posix != nullptr && kernel->vaddr != posix->vaddr &&
        HostCallAdmissionPolicyFor(kernel->vaddr) == audited &&
        HostCallAdmissionPolicyFor(posix->vaddr) == audited, pass);
    if (!pass || !RegisterHostFunction<NativeAuditedUnlock>("self-test-audited-unlock",
            HostCallEffect::GuestMemory, HostCallShortContract::SynchronousGuestMemory)) return false;
    const char* selected = std::getenv("LUMEN_EXPERIMENTAL_FEX_HLE_SYNC_MEMORY_SHORT");
    const bool expect_short = Selected() == Provider::Fex &&
        (selected == nullptr || std::strcmp(selected, "1") == 0) && ExclusiveMemoryPublicationFrozen();
    const std::array<std::uint64_t, 3> targets {kernel->vaddr, posix->vaddr,
        reinterpret_cast<std::uint64_t>(NativeAuditedUnlock)};
    constexpr std::uint64_t page = 0x4000;
    const auto code_base = Memory::AllocateProgramMemory(0, page, Vm::Mode::ReadWrite,
        "fex-short-memory-code");
    const auto records = Memory::AllocateRuntimeMemory(0, page, Vm::Mode::ReadWrite,
        "fex-short-memory-slots");
    if (code_base == 0 || records == 0) return false;
    std::array<std::uint8_t, 1024> code {};
    constexpr std::uint8_t prefix[] {0x48,0x83,0xec,0x08,0x48,0xb8};
    constexpr std::uint8_t tail[] {0xff,0xd0,0x48,0x83,0xc4,0x08,0xc3};
    for (std::size_t index = 0; index != targets.size(); ++index) {
        std::memcpy(code.data() + index * 32, prefix, sizeof(prefix));
        Put64(code, index * 32 + sizeof(prefix), targets[index]);
        std::memcpy(code.data() + index * 32 + sizeof(prefix) + 8, tail, sizeof(tail));
    }
    pass &= Memory::WriteGuestMemory(nullptr, code_base, code.data(), code.size());
    Memory::SetProgramMemoryProtection(code_base, page, Vm::Mode::ExecuteRead);
    const auto slot = records + 8;
    const auto put = [&](std::uint64_t value) {
        return Memory::WriteGuestMemory(nullptr, slot, &value, sizeof(value));
    };
    const auto call = [&](unsigned target, std::uint64_t address, int expected) {
        std::uint64_t result {};
        return Invoke(code_base + target * 32, &address, 1, &result, stack_top) &&
            static_cast<std::uint32_t>(result) == static_cast<std::uint32_t>(expected);
    };
    for (unsigned target = 0; target != targets.size(); ++target) {
        P::PthreadMutex mutex {};
        const bool initialized = P::PthreadMutexInit(&mutex, nullptr, nullptr) == ::OK;
        bool valid = initialized && P::PthreadMutexLock(&mutex) == ::OK &&
            put(reinterpret_cast<std::uint64_t>(mutex));
        short_unlock_witness = {};
        valid = valid && call(target, slot, ::OK) && P::PthreadMutexTrylock(&mutex) == ::OK;
        if (target == 2) valid = valid && short_unlock_witness.calls == 1 &&
            short_unlock_witness.before == expect_short && short_unlock_witness.after == expect_short;
        valid = valid && P::PthreadMutexUnlock(&mutex) == ::OK;
        if (initialized) valid = P::PthreadMutexDestroy(&mutex) == ::OK && valid;
        CheckCase(target == 0 ? "sync-short-kernel-live-unlock" : target == 1 ?
            "sync-short-posix-live-unlock" : "sync-short-live-body-keeps-execution-reader", valid, pass);
    }
    for (const auto sentinel: {std::uint64_t {0}, std::uint64_t {1}}) {
        bool valid = true;
        for (unsigned target = 0; target != targets.size(); ++target) {
            short_unlock_witness = {};
            bool one = put(sentinel) && call(target, slot,
                target == 1 ? Libs::Posix::POSIX_EPERM : P::KERNEL_ERROR_EPERM);
            if (target == 2) one = one && short_unlock_witness.calls == 1 &&
                short_unlock_witness.before == expect_short && !short_unlock_witness.after;
            // Outside Run the fixture owns this slot; no pointer survived a yield.
            auto* native_slot = reinterpret_cast<P::PthreadMutex*>(slot);
            one = P::PthreadMutexDestroy(native_slot) == ::OK && one;
            valid = one && valid;
        }
        CheckCase(sentinel == 0 ? "sync-short-kernel-posix-cold-zero-demotes-before-init" :
            "sync-short-kernel-posix-cold-one-demotes-before-init", valid, pass);
    }
    P::PthreadCond condition {};
    const bool cond_live = P::PthreadCondInit(&condition, nullptr, nullptr) == ::OK;
    bool invalid = cond_live && put(reinterpret_cast<std::uint64_t>(condition)) &&
        call(0, slot, P::KERNEL_ERROR_EINVAL) && call(1, slot, Libs::Posix::POSIX_EINVAL) &&
        call(0, 0, P::KERNEL_ERROR_EINVAL) && call(0, slot + 1, P::KERNEL_ERROR_EINVAL);
    if (cond_live) invalid = P::PthreadCondDestroy(&condition) == ::OK && invalid;
    CheckCase("sync-short-kernel-posix-wrong-type-null-and-alignment", invalid, pass);
    if (expect_short) {
        bool rejected = call(0, UINT64_MAX - 7, P::KERNEL_ERROR_EINVAL) && put(0) &&
            Memory::ProtectGuestMemory(records, page, Vm::Mode::Read);
        short_unlock_witness = {};
        rejected = rejected && call(2, slot, P::KERNEL_ERROR_EINVAL) &&
            short_unlock_witness.before && !short_unlock_witness.after;
        rejected = Memory::ProtectGuestMemory(records, page, Vm::Mode::NoAccess) && rejected;
        rejected = call(0, slot, P::KERNEL_ERROR_EINVAL) && rejected;
        rejected = Memory::ProtectGuestMemory(records, page, Vm::Mode::ReadWrite) && rejected;
        CheckCase("sync-short-invalid-slot-and-cold-read-only-fail-before-effects", rejected, pass);
    } else {
        std::fputs("SKIP: sync-short-invalid-slot-proofs-require-selected-contract\n", stderr);
    }
    CheckCase("sync-short-lifetime-destroy-busy-stale-race", P::TestMutexResolvePinLifetime(), pass);
    pass = Memory::FreeGuestMemory(code_base, page) && pass;
    pass = Memory::FreeGuestMemory(records, page) && pass;
    return pass;
}
