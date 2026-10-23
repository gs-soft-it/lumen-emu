// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
// Included by fexRuntimeTests.cpp after its common fixture helpers.
bool TestSyncHandles(std::uint64_t stack_top) {
    namespace P = Libs::LibKernel;
    enum Call : std::size_t {
        MutexInit, MutexLock, MutexTry, MutexTimed, MutexUnlock, MutexDestroy,
        CondInit, CondSignal, CondBroadcast, CondSignalTo, CondWait, CondTimed,
        CondAbs, CondDestroy, RwInit, RwRead, RwWrite, RwTryRead, RwTryWrite,
        RwTimedRead, RwTimedWrite, RwUnlock, RwDestroy, Count
    };
    std::array<std::uint64_t, Count> targets {};
    bool registered = true;
#define SYNC_TARGET(index, function) \
    targets[index] = reinterpret_cast<std::uint64_t>(P::function); \
    registered &= RegisterHostFunction<P::function>("self-test-sync-" #function)
    SYNC_TARGET(MutexInit, PthreadMutexInit);
    SYNC_TARGET(MutexLock, PthreadMutexLock);
    SYNC_TARGET(MutexTry, PthreadMutexTrylock);
    SYNC_TARGET(MutexTimed, PthreadMutexTimedlock);
    SYNC_TARGET(MutexUnlock, PthreadMutexUnlock);
    SYNC_TARGET(MutexDestroy, PthreadMutexDestroy);
    SYNC_TARGET(CondInit, PthreadCondInit);
    SYNC_TARGET(CondSignal, PthreadCondSignal);
    SYNC_TARGET(CondBroadcast, PthreadCondBroadcast);
    SYNC_TARGET(CondSignalTo, PthreadCondSignalto);
    SYNC_TARGET(CondWait, PthreadCondWait);
    SYNC_TARGET(CondTimed, PthreadCondTimedwait);
    SYNC_TARGET(CondAbs, PthreadCondTimedwaitAbs);
    SYNC_TARGET(CondDestroy, PthreadCondDestroy);
    SYNC_TARGET(RwInit, PthreadRwlockInit);
    SYNC_TARGET(RwRead, PthreadRwlockRdlock);
    SYNC_TARGET(RwWrite, PthreadRwlockWrlock);
    SYNC_TARGET(RwTryRead, PthreadRwlockTryrdlock);
    SYNC_TARGET(RwTryWrite, PthreadRwlockTrywrlock);
    SYNC_TARGET(RwTimedRead, PthreadRwlockTimedrdlock);
    SYNC_TARGET(RwTimedWrite, PthreadRwlockTimedwrlock);
    SYNC_TARGET(RwUnlock, PthreadRwlockUnlock);
    SYNC_TARGET(RwDestroy, PthreadRwlockDestroy);
#undef SYNC_TARGET
    if (!registered) return false;

    const auto code_base = Memory::AllocateProgramMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                         "fex-sync-handle-code");
    const auto records = Memory::AllocateRuntimeMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                       "fex-sync-handle-records");
    if (code_base == 0 || records == 0) return false;
    std::array<std::uint8_t, 1024> code {};
    // Each real x86 call preserves all SysV GP arguments and calls one typed HLE.
    constexpr std::uint8_t prefix[] {0x48,0x83,0xec,0x08,0x48,0xb8};
    constexpr std::uint8_t tail[] {0xff,0xd0,0x48,0x83,0xc4,0x08,0xc3};
    for (std::size_t index = 0; index < Count; ++index) {
        std::memcpy(code.data() + index * 32, prefix, sizeof(prefix));
        Put64(code, index * 32 + sizeof(prefix), targets[index]);
        std::memcpy(code.data() + index * 32 + sizeof(prefix) + 8, tail, sizeof(tail));
    }
    bool pass = Memory::WriteGuestMemory(nullptr, code_base, code.data(), code.size());
    Memory::SetProgramMemoryProtection(code_base, 0x4000, Vm::Mode::ExecuteRead);
    constexpr std::uint64_t guard = 0x73796e6368616e64ULL;
    const auto slot = records + 8;
    const auto peer = records + 40;
    const auto put = [&](std::uint64_t address, std::uint64_t handle) {
        const std::array<std::uint64_t, 3> frame {guard, handle, guard};
        return Memory::WriteGuestMemory(nullptr, address - 8, frame.data(), sizeof(frame));
    };
    const auto unchanged = [&](std::uint64_t address, std::uint64_t handle) {
        std::array<std::uint64_t, 3> frame {};
        return Memory::ReadGuestMemory(nullptr, address - 8, frame.data(), sizeof(frame)) &&
            frame == std::array<std::uint64_t, 3> {guard, handle, guard};
    };
    const auto bits = [&](std::uint64_t address) {
        std::uint64_t handle {};
        if (!Memory::ReadGuestMemory(nullptr, address, &handle, sizeof(handle))) return UINT64_MAX;
        return handle;
    };
    const auto call = [&](Call operation, std::uint64_t first, std::uint64_t second,
                          std::uint64_t third, int expected) {
        const std::array<std::uint64_t, 3> arguments {first, second, third};
        std::uint64_t result {};
        return Invoke(code_base + operation * 32, arguments.data(), arguments.size(),
                      &result, stack_top) &&
            static_cast<std::uint32_t>(result) == static_cast<std::uint32_t>(expected);
    };
    const auto invalid_family = [&](std::uint64_t handle, unsigned family) {
        bool valid = put(slot, handle) && put(peer, handle);
        const auto check = [&](Call operation) {
            return call(operation, slot, peer, 0, P::KERNEL_ERROR_EINVAL) &&
                   unchanged(slot, handle) && unchanged(peer, handle);
        };
        if (family == 0) {
            for (const auto operation: {MutexLock, MutexTry, MutexTimed, MutexUnlock, MutexDestroy})
                valid &= check(operation);
        } else if (family == 1) {
            for (const auto operation: {CondSignal, CondBroadcast, CondSignalTo, CondWait,
                                       CondTimed, CondAbs, CondDestroy}) valid &= check(operation);
        } else {
            for (const auto operation: {RwRead, RwWrite, RwTryRead, RwTryWrite,
                                       RwTimedRead, RwTimedWrite, RwUnlock, RwDestroy})
                valid &= check(operation);
        }
        return valid;
    };

    std::uint64_t native_witness = guard;
    const auto unowned = reinterpret_cast<std::uint64_t>(&native_witness);
    CheckCase("sync-unowned-native-address-rejected-all-families",
        invalid_family(unowned, 0) && invalid_family(unowned, 1) && invalid_family(unowned, 2) &&
        native_witness == guard, pass);
    CheckCase("sync-unknown-noncanonical-handle-rejected-before-dereference",
        invalid_family(UINT64_MAX, 0) && invalid_family(UINT64_MAX, 1) &&
        invalid_family(UINT64_MAX, 2), pass);
    bool invalid_slots = put(slot, guard);
    for (const auto operation: {MutexLock, MutexTry, MutexUnlock}) {
        invalid_slots &= call(operation, 0, 0, 0, P::KERNEL_ERROR_EINVAL) &&
            call(operation, slot + 1, 0, 0, P::KERNEL_ERROR_EINVAL) && unchanged(slot, guard);
    }
    CheckCase("sync-mutex-resolver-null-misaligned-slots-unchanged", invalid_slots, pass);

    P::PthreadMutex native_mutex {};
    P::PthreadCond native_cond {};
    P::PthreadRwlock native_rwlock {};
    bool live = P::PthreadMutexInit(&native_mutex, nullptr, nullptr) == ::OK &&
        P::PthreadCondInit(&native_cond, nullptr, nullptr) == ::OK &&
        P::PthreadRwlockInit(&native_rwlock, nullptr, nullptr) == ::OK;
    CheckCase("sync-foreign-live-family-rejected",
        live && invalid_family(reinterpret_cast<std::uint64_t>(native_cond), 0) &&
        invalid_family(reinterpret_cast<std::uint64_t>(native_mutex), 1) &&
        invalid_family(reinterpret_cast<std::uint64_t>(native_mutex), 2), pass);
    const auto stale_mutex = reinterpret_cast<std::uint64_t>(native_mutex);
    const auto stale_cond = reinterpret_cast<std::uint64_t>(native_cond);
    const auto stale_rwlock = reinterpret_cast<std::uint64_t>(native_rwlock);
    live &= P::PthreadMutexDestroy(&native_mutex) == ::OK &&
        P::PthreadCondDestroy(&native_cond) == ::OK && P::PthreadRwlockDestroy(&native_rwlock) == ::OK;
    // Recreate the same kinds before exercising the stale values: retirement
    // must prevent allocation identity reuse, not just reject a missing entry.
    live &= P::PthreadMutexInit(&native_mutex, nullptr, nullptr) == ::OK &&
        P::PthreadCondInit(&native_cond, nullptr, nullptr) == ::OK &&
        P::PthreadRwlockInit(&native_rwlock, nullptr, nullptr) == ::OK;
    CheckCase("sync-retirement-prevents-address-reuse-and-stale-handles",
        live && reinterpret_cast<std::uint64_t>(native_mutex) != stale_mutex &&
        reinterpret_cast<std::uint64_t>(native_cond) != stale_cond &&
        reinterpret_cast<std::uint64_t>(native_rwlock) != stale_rwlock &&
        invalid_family(stale_mutex, 0) && invalid_family(stale_cond, 1) &&
        invalid_family(stale_rwlock, 2), pass);
    if (native_mutex) pass &= P::PthreadMutexDestroy(&native_mutex) == ::OK;
    if (native_cond) pass &= P::PthreadCondDestroy(&native_cond) == ::OK;
    if (native_rwlock) pass &= P::PthreadRwlockDestroy(&native_rwlock) == ::OK;

    CheckCase("sync-invalid-cond-pair-preserves-both-lazy-slots",
        put(slot, 0) && put(peer, unowned) &&
        call(CondWait, slot, peer, 0, P::KERNEL_ERROR_EINVAL) && unchanged(slot, 0) &&
        unchanged(peer, unowned) && put(slot, unowned) && put(peer, 0) &&
        call(CondTimed, slot, peer, 0, P::KERNEL_ERROR_EINVAL) &&
        unchanged(slot, unowned) && unchanged(peer, 0), pass);

    bool normal = put(slot, 0) && call(MutexInit, slot, 0, 0, ::OK);
    const auto normal_handle = bits(slot);
    normal &= normal_handle != 0 && unchanged(slot, normal_handle) &&
        call(MutexLock, slot, 0, 0, ::OK) &&
        call(MutexDestroy, slot, 0, 0, P::KERNEL_ERROR_EBUSY) && unchanged(slot, normal_handle) &&
        call(MutexLock, slot, 0, 0, P::KERNEL_ERROR_EDEADLK) &&
        call(MutexUnlock, slot, 0, 0, ::OK) && call(MutexDestroy, slot, 0, 0, ::OK) && unchanged(slot, 0);
    CheckCase("sync-real-mutex-init-lock-destroy-busy-and-retire", normal, pass);

    P::PthreadMutexattr recursive_attr {};
    bool recursive = P::PthreadMutexattrInit(&recursive_attr) == ::OK &&
        P::PthreadMutexattrSettype(&recursive_attr, 2) == ::OK && put(slot, 0) &&
        call(MutexInit, slot, reinterpret_cast<std::uint64_t>(&recursive_attr), 0, ::OK) &&
        call(MutexLock, slot, 0, 0, ::OK) && call(MutexLock, slot, 0, 0, ::OK) &&
        call(MutexUnlock, slot, 0, 0, ::OK) &&
        call(MutexDestroy, slot, 0, 0, P::KERNEL_ERROR_EBUSY) &&
        call(MutexUnlock, slot, 0, 0, ::OK) && call(MutexDestroy, slot, 0, 0, ::OK);
    CheckCase("sync-real-recursive-mutex-keeps-recursion", recursive, pass);
    if (recursive_attr) pass &= P::PthreadMutexattrDestroy(&recursive_attr) == ::OK;

    CheckCase("sync-known-adaptive-static-initializer", put(slot, 1) &&
        call(MutexLock, slot, 0, 0, ::OK) && call(MutexUnlock, slot, 0, 0, ::OK) &&
        call(MutexDestroy, slot, 0, 0, ::OK) && unchanged(slot, 0), pass);
    CheckCase("sync-mutex-resolver-lazy-zero-trylock", put(slot, 0) &&
        call(MutexTry, slot, 0, 0, ::OK) && call(MutexUnlock, slot, 0, 0, ::OK) &&
        call(MutexDestroy, slot, 0, 0, ::OK) && unchanged(slot, 0), pass);
    CheckCase("sync-mutex-resolver-pin-concurrent-retirement", P::TestMutexResolvePinLifetime(), pass);
    CheckCase("sync-real-condition-timeout-reacquires-mutex", put(slot, 0) && put(peer, 0) &&
        call(MutexInit, peer, 0, 0, ::OK) && call(MutexLock, peer, 0, 0, ::OK) &&
        call(CondTimed, slot, peer, 0, P::KERNEL_ERROR_ETIMEDOUT) &&
        call(MutexDestroy, peer, 0, 0, P::KERNEL_ERROR_EBUSY) &&
        call(MutexUnlock, peer, 0, 0, ::OK) && call(MutexDestroy, peer, 0, 0, ::OK) &&
        call(CondDestroy, slot, 0, 0, ::OK) && unchanged(slot, 0) && unchanged(peer, 0), pass);
    CheckCase("sync-real-rwlock-static-read-write-and-destroy-busy", put(slot, 0) &&
        call(RwTryWrite, slot, 0, 0, ::OK) &&
        call(RwDestroy, slot, 0, 0, P::KERNEL_ERROR_EBUSY) &&
        call(RwUnlock, slot, 0, 0, ::OK) && call(RwRead, slot, 0, 0, ::OK) &&
        call(RwDestroy, slot, 0, 0, P::KERNEL_ERROR_EBUSY) &&
        call(RwUnlock, slot, 0, 0, ::OK) && call(RwDestroy, slot, 0, 0, ::OK) &&
        unchanged(slot, 0), pass);

    pass &= Memory::FreeGuestMemory(records, 0x4000);
    pass &= Memory::FreeGuestMemory(code_base, 0x4000);
    return pass;
}
