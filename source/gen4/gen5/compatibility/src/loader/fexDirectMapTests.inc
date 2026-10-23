// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
// Included by fexRuntimeTests.cpp after its common fixture helpers.
namespace {
constexpr std::uint64_t direct_map_page = 0x4000;
constexpr std::uint64_t direct_map_guard = 0x6469726563746d70ULL;
constexpr std::uint64_t direct_map_marker = 0x1029384756abcdefULL;
struct DirectMapObservation {
    std::array<std::uint64_t, 4> frame {};
    bool captured {};
    bool translated_access {};
};
std::array<std::array<std::uint64_t, 6>, 3> direct_map_arguments {};
std::array<DirectMapObservation, 3> direct_map_observations {};
DirectMapObservation direct_map_nested_busy {};
bool direct_map_exclusive_denied {};
bool direct_map_busy_lease_preserved {};

std::uint64_t NativeDirectMapBusy(void** output, std::size_t len, int prot, int flags,
    std::int64_t physical, std::size_t alignment) {
    if (LumenAndroidFexCoreRetainGuestDataLeaseV1() == 0) return UINT64_MAX;
    const int result = Memory::KernelMapDirectMemory(output, len, prot, flags, physical, alignment);
    // A failed additive scope must not surrender the original HLE reader,
    // publish/unlock an unowned scope, or consume this independently held borrow.
    const bool retained = LumenAndroidFexCoreRetainGuestDataLeaseV1() != 0;
    const bool extra_released = retained && LumenAndroidFexCoreReleaseGuestDataLeaseV1() != 0;
    const bool original_released = LumenAndroidFexCoreReleaseGuestDataLeaseV1() != 0;
    direct_map_busy_lease_preserved = retained && extra_released && original_released;
    return static_cast<std::uint32_t>(result);
}

std::uint64_t NativeDirectMapExclusiveProbe() {
    const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    if (token != 0) LumenAndroidFexCoreEndGuestMemoryMutationV1(token, 0, 0, 0);
    return token == 0 ? 1 : 0;
}

std::uint64_t NativeDirectMapOuter(std::uint64_t map_callback,
    std::uint64_t rw_callback, std::uint64_t write_callback, std::uint64_t exclusive_callback,
    std::uint64_t busy_callback) {
    bool pass = true;
    std::uint64_t busy_frame {};
    direct_map_nested_busy.captured = Invoke(busy_callback, direct_map_arguments[0].data(),
        direct_map_arguments[0].size(), &busy_frame) && Memory::ReadGuestMemory(nullptr,
            busy_frame, direct_map_nested_busy.frame.data(), sizeof(direct_map_nested_busy.frame));
    pass &= direct_map_nested_busy.captured && direct_map_busy_lease_preserved &&
        static_cast<std::uint32_t>(direct_map_nested_busy.frame[3]) ==
            static_cast<std::uint32_t>(Libs::LibKernel::KERNEL_ERROR_EBUSY);
    for (std::size_t index = 0; index < direct_map_arguments.size(); ++index) {
        std::uint64_t frame {};
        auto& observation = direct_map_observations[index];
        observation.captured = Invoke(map_callback, direct_map_arguments[index].data(),
            direct_map_arguments[index].size(), &frame) && Memory::ReadGuestMemory(nullptr,
                frame, observation.frame.data(), sizeof(observation.frame));
        if (observation.captured && static_cast<std::uint32_t>(observation.frame[3]) == ::OK) {
            const std::array<std::uint64_t, 2> access_arguments {
                observation.frame[1], direct_map_marker + index};
            std::uint64_t value {};
            observation.translated_access = Invoke(index == 1 ? write_callback : rw_callback,
                access_arguments.data(), access_arguments.size(), &value) &&
                value == direct_map_marker + index;
        }
        pass &= observation.captured && observation.translated_access;
    }
    std::uint64_t denied {};
    direct_map_exclusive_denied = Invoke(exclusive_callback, nullptr, 0, &denied) && denied == 1;
    return pass ? 1 : 0;
}
} // namespace

bool TestDirectMapNested(std::uint64_t stack_top) {
    if (!RegisterHostFunction<Memory::KernelMapDirectMemory>("self-test-direct-map") ||
        !RegisterHostFunction<NativeDirectMapBusy>("self-test-direct-map-busy") ||
        !RegisterHostFunction<NativeDirectMapOuter>("self-test-direct-map-outer") ||
        !RegisterHostFunction<NativeDirectMapExclusiveProbe>("self-test-direct-map-exclusive"))
        return false;

    std::array<std::int64_t, 3> physical {-1, -1, -1};
    std::array<void*, 3> reserved {};
    std::uint64_t code_base {};
    const auto cleanup = [&]() {
        bool cleaned = true;
        for (auto address: reserved) {
            if (address == nullptr) continue;
            Memory::VirtualQueryInfo info {};
            if (Memory::KernelVirtualQuery(address, 0, &info, sizeof(info)) == ::OK)
                cleaned &= Memory::KernelMunmap(reinterpret_cast<std::uint64_t>(address),
                                               direct_map_page) == ::OK;
        }
        // Release also retires the nonfixed mapping and any remaining aliases.
        for (auto address: physical)
            if (address >= 0) cleaned &= Memory::KernelCheckedReleaseDirectMemory(
                address, direct_map_page) == ::OK;
        if (code_base != 0) cleaned &= Memory::FreeGuestMemory(code_base, 0x4000);
        return cleaned;
    };
    const auto physical_size = Memory::KernelGetDirectMemorySize();
    if (physical_size > INT64_MAX) return false;
    for (std::size_t index = 0; index < physical.size(); ++index) {
        if (Memory::KernelAllocateDirectMemory(0, static_cast<std::int64_t>(physical_size),
                direct_map_page, direct_map_page, 3, &physical[index]) != ::OK ||
            Memory::KernelReserveVirtualRange(&reserved[index], direct_map_page, 0,
                                              direct_map_page) != ::OK) {
            cleanup();
            return false;
        }
    }
    code_base = Memory::AllocateProgramMemory(0, 0x4000, Vm::Mode::ReadWrite, "fex-direct-map-code");
    if (code_base == 0) { cleanup(); return false; }

    std::array<std::uint8_t, 1024> code {};
    const auto relay = [&](std::size_t offset, std::uint64_t target) {
        constexpr std::uint8_t prefix[] {0x48,0x83,0xec,0x08,0x48,0xb8};
        constexpr std::uint8_t tail[] {0xff,0xd0,0x48,0x83,0xc4,0x08,0xc3};
        std::memcpy(code.data() + offset, prefix, sizeof(prefix));
        Put64(code, offset + sizeof(prefix), target);
        std::memcpy(code.data() + offset + sizeof(prefix) + 8, tail, sizeof(tail));
    };
    relay(0, reinterpret_cast<std::uint64_t>(NativeDirectMapOuter));
    // The first argument is an initial hint. Put it in an owned stack slot,
    // replace RDI with that slot's address, and preserve the other five args.
    constexpr std::uint8_t map_prefix[] {0x48,0x83,0xec,0x28,0x48,0xb8};
    constexpr std::uint8_t map_middle[] {
        0x48,0x89,0x04,0x24,0x48,0x89,0x7c,0x24,0x08,
        0x48,0x89,0x44,0x24,0x10,0x48,0x8d,0x7c,0x24,0x08,0x48,0xb8
    };
    constexpr std::uint8_t map_tail[] {
        0xff,0xd0,0x48,0x89,0x44,0x24,0x18,
        0x48,0x8d,0x04,0x24,0x48,0x83,0xc4,0x28,0xc3
    };
    std::memcpy(code.data() + 64, map_prefix, sizeof(map_prefix));
    Put64(code, 64 + sizeof(map_prefix), direct_map_guard);
    std::memcpy(code.data() + 78, map_middle, sizeof(map_middle));
    Put64(code, 78 + sizeof(map_middle),
          reinterpret_cast<std::uint64_t>(Memory::KernelMapDirectMemory));
    std::memcpy(code.data() + 86 + sizeof(map_middle), map_tail, sizeof(map_tail));
    std::memcpy(code.data() + 320, code.data() + 64,
                22 + sizeof(map_middle) + sizeof(map_tail));
    Put64(code, 320 + 14 + sizeof(map_middle),
          reinterpret_cast<std::uint64_t>(NativeDirectMapBusy));
    constexpr std::uint8_t rw[] {0x48,0x89,0x37,0x48,0x8b,0x07,0xc3}; // [RDI]=RSI; RAX=[RDI]
    constexpr std::uint8_t write[] {0x48,0x89,0x37,0x48,0x89,0xf0,0xc3}; // [RDI]=RSI; RAX=RSI
    std::memcpy(code.data() + 192, rw, sizeof(rw));
    std::memcpy(code.data() + 224, write, sizeof(write));
    relay(256, reinterpret_cast<std::uint64_t>(NativeDirectMapExclusiveProbe));
    bool pass = Memory::WriteGuestMemory(nullptr, code_base, code.data(), code.size());
    Memory::SetProgramMemoryProtection(code_base, 0x4000, Vm::Mode::ExecuteRead);
    direct_map_observations = {};
    direct_map_nested_busy = {};
    direct_map_exclusive_denied = false;
    direct_map_busy_lease_preserved = false;
    direct_map_arguments = {{
        {reinterpret_cast<std::uint64_t>(reserved[0]), direct_map_page, 3, 0x400010,
         static_cast<std::uint64_t>(physical[0]), direct_map_page},
        {reinterpret_cast<std::uint64_t>(reserved[1]), direct_map_page, 2, 0x400010,
         static_cast<std::uint64_t>(physical[1]), direct_map_page},
        {0, direct_map_page, 3, 0, static_cast<std::uint64_t>(physical[2]), direct_map_page}
    }};
    CheckCase("direct-map-reserved-targets-start-without-cpu-rights",
        !Memory::QueryGuestAccess(reinterpret_cast<std::uint64_t>(reserved[0]), 8, Vm::Mode::Read) &&
        !Memory::QueryGuestAccess(reinterpret_cast<std::uint64_t>(reserved[0]), 8, Vm::Mode::Write) &&
        !Memory::QueryGuestAccess(reinterpret_cast<std::uint64_t>(reserved[1]), 8, Vm::Mode::Read) &&
        !Memory::QueryGuestAccess(reinterpret_cast<std::uint64_t>(reserved[1]), 8, Vm::Mode::Write), pass);
    // Deterministic denial: a real HLE callback holds a separate data borrow
    // through the otherwise eligible map. No scheduler timing or sleeps are used.
    std::uint64_t busy_frame {};
    std::array<std::uint64_t, 4> busy_observation {};
    const bool busy_invoked = Invoke(code_base + 320, direct_map_arguments[0].data(),
        direct_map_arguments[0].size(), &busy_frame, stack_top) &&
        Memory::ReadGuestMemory(nullptr, busy_frame, busy_observation.data(), sizeof(busy_observation));
    CheckCase("direct-map-denied-additive-returns-busy-without-changing-output",
        busy_invoked && busy_observation[0] == direct_map_guard &&
        busy_observation[1] == reinterpret_cast<std::uint64_t>(reserved[0]) &&
        busy_observation[2] == direct_map_guard &&
        static_cast<std::uint32_t>(busy_observation[3]) ==
            static_cast<std::uint32_t>(Libs::LibKernel::KERNEL_ERROR_EBUSY) &&
        direct_map_busy_lease_preserved, pass);
    Memory::VirtualQueryInfo busy_info {};
    std::uint64_t busy_witness = direct_map_guard;
    CheckCase("direct-map-denied-additive-keeps-reservation-without-backing",
        Memory::KernelVirtualQuery(reserved[0], 0, &busy_info, sizeof(busy_info)) == ::OK &&
        busy_info.protection == 0 && !busy_info.is_direct && !busy_info.is_committed &&
        !Memory::QueryGuestAccess(reinterpret_cast<std::uint64_t>(reserved[0]), 8, Vm::Mode::Read) &&
        !Memory::QueryGuestAccess(reinterpret_cast<std::uint64_t>(reserved[0]), 8, Vm::Mode::Write) &&
        !Memory::TryReadBacking(reinterpret_cast<std::uint64_t>(reserved[0]),
                                &busy_witness, sizeof(busy_witness)) &&
        busy_witness == direct_map_guard, pass);
    const std::array<std::uint64_t, 5> callbacks {code_base + 64, code_base + 192,
                                                code_base + 224, code_base + 256, code_base + 320};
    std::uint64_t value {};
    const bool invoked = Invoke(code_base, callbacks.data(), callbacks.size(), &value, stack_top);
    CheckCase("direct-map-real-wrapper-at-two-hle-levels", invoked && value == 1, pass);
    CheckCase("direct-map-nested-borrow-denial-keeps-output-and-outer-reader",
        invoked && direct_map_nested_busy.captured && direct_map_busy_lease_preserved &&
        direct_map_nested_busy.frame[0] == direct_map_guard &&
        direct_map_nested_busy.frame[1] == reinterpret_cast<std::uint64_t>(reserved[0]) &&
        direct_map_nested_busy.frame[2] == direct_map_guard &&
        static_cast<std::uint32_t>(direct_map_nested_busy.frame[3]) ==
            static_cast<std::uint32_t>(Libs::LibKernel::KERNEL_ERROR_EBUSY), pass);
    CheckCase("direct-map-same-physical-and-target-remain-eligible-after-busy",
        invoked && direct_map_observations[0].captured &&
        static_cast<std::uint32_t>(direct_map_observations[0].frame[3]) == ::OK &&
        direct_map_observations[0].frame[1] == busy_observation[1], pass);
    for (std::size_t index = 0; index < direct_map_observations.size(); ++index) {
        const auto& observation = direct_map_observations[index];
        const auto address = observation.frame[1];
        const bool right_address = index < 2
            ? address == reinterpret_cast<std::uint64_t>(reserved[index])
            : address != 0 && (address & (direct_map_page - 1)) == 0;
        const std::array<const char*, 3> names {
            "direct-map-reserved-rw-preserves-output-guards",
            "direct-map-reserved-write-only-preserves-output-guards",
            "direct-map-free-hint-preserves-output-guards"
        };
        CheckCase(names[index], observation.captured && right_address &&
            observation.frame[0] == direct_map_guard && observation.frame[2] == direct_map_guard &&
            static_cast<std::uint32_t>(observation.frame[3]) == ::OK, pass);
    }
    CheckCase("direct-map-new-rw-views-work-in-translated-guest",
        direct_map_observations[0].translated_access && direct_map_observations[2].translated_access, pass);
    CheckCase("direct-map-write-only-view-accepts-translated-store",
              direct_map_observations[1].translated_access, pass);
    const auto write_only = reinterpret_cast<std::uint64_t>(reserved[1]);
    auto witness = direct_map_guard;
    // The existing guest-rights policy normalizes CPU_WRITE to CPU_READ too.
    CheckCase("direct-map-protection-two-keeps-write-implies-read-policy",
        Memory::QueryGuestAccess(write_only, 8, Vm::Mode::ReadWrite) &&
        Memory::ReadGuestMemory(nullptr, write_only, &witness, sizeof(witness)) &&
        witness == direct_map_marker + 1, pass);
    CheckCase("direct-map-write-only-store-persists-after-root-protect",
        Memory::KernelMprotect(reserved[1], direct_map_page, 3) == ::OK &&
        Memory::ReadGuestMemory(nullptr, write_only, &witness, sizeof(witness)) &&
        witness == direct_map_marker + 1, pass);
    CheckCase("direct-map-keeps-nested-destructive-mutation-denied",
              invoked && direct_map_exclusive_denied, pass);

    // Noneligible calls retain their legacy exclusive entry. Exercise API
    // errors and aliases from root, without calling a fatal nested route.
    void* overlap = reserved[0];
    const auto overlap_result = Memory::KernelMapDirectMemory(&overlap, direct_map_page,
        3, 0x90, physical[2], direct_map_page);
    CheckCase("direct-map-root-no-overwrite-keeps-existing-view", overlap_result ==
        Libs::LibKernel::KERNEL_ERROR_ENOMEM && overlap == reserved[0] &&
        Memory::ReadGuestMemory(nullptr, reinterpret_cast<std::uint64_t>(overlap),
            &witness, sizeof(witness)) && witness == direct_map_marker, pass);
    void* invalid = reserved[2];
    CheckCase("direct-map-root-invalid-length-keeps-reservation", Memory::KernelMapDirectMemory(
        &invalid, direct_map_page + 1, 3, 0x10, physical[0], direct_map_page) ==
        Libs::LibKernel::KERNEL_ERROR_EINVAL && invalid == reserved[2], pass);
    CheckCase("direct-map-root-executable-protection-keeps-reservation", Memory::KernelMapDirectMemory(
        &invalid, direct_map_page, 7, 0x10, physical[0], direct_map_page) ==
        Libs::LibKernel::KERNEL_ERROR_EACCES && invalid == reserved[2] &&
        !Memory::QueryGuestAccess(reinterpret_cast<std::uint64_t>(invalid), 8, Vm::Mode::Write), pass);
    void* alias = reserved[2];
    const bool aliased = Memory::KernelMapDirectMemory(&alias, direct_map_page, 3, 0x10,
        physical[0], direct_map_page) == ::OK;
    CheckCase("direct-map-root-physical-alias-sees-same-backing", aliased && alias == reserved[2] &&
        alias != reserved[0] && Memory::ReadGuestMemory(nullptr,
            reinterpret_cast<std::uint64_t>(alias), &witness, sizeof(witness)) &&
        witness == direct_map_marker, pass);
    const auto alias_marker = direct_map_marker ^ 0x00ff00ff00ff00ffULL;
    CheckCase("direct-map-root-physical-alias-writes-reach-original", aliased &&
        Memory::WriteGuestMemory(nullptr, reinterpret_cast<std::uint64_t>(alias),
            &alias_marker, sizeof(alias_marker)) && Memory::ReadGuestMemory(nullptr,
            reinterpret_cast<std::uint64_t>(reserved[0]), &witness, sizeof(witness)) &&
        witness == alias_marker, pass);
    CheckCase("direct-map-root-cleans-all-views-and-reservations", cleanup(), pass);
    return pass;
}
