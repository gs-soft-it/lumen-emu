// Lumen adapter, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#include "loader/hostObject.h"

#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "loader/guestMutationScope.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Loader {
namespace {
using Common::VirtualMemory::Mode;
constexpr std::uint64_t ObjectPage = 16 * 1024;
constexpr std::size_t ProgNameSize = 512;
std::mutex objects_mutex;
std::unordered_map<std::string, std::uint64_t> objects;
std::uint64_t progname_string {};

std::uint64_t CopyObject(const void* bytes, std::size_t size, bool writable, const char* name) {
    if (bytes == nullptr || size == 0 || size > ObjectPage) return 0;
    const auto address = Libs::LibKernel::Memory::AllocateRuntimeMemory(0, ObjectPage, Mode::ReadWrite, name);
    if (address == 0) return 0;
#if defined(LUMEN_PS5_FEXCORE)
    const bool copied = Libs::LibKernel::Memory::WriteGuestMemory(nullptr, address, bytes, size);
#else
    const bool copied = Libs::LibKernel::Memory::TryWriteBacking(address, bytes, size);
#endif
    if (!copied ||
        (!writable && !Libs::LibKernel::Memory::ProtectGuestMemory(address, ObjectPage, Mode::Read))) {
        Libs::LibKernel::Memory::FreeGuestMemory(address, ObjectPage);
        return 0;
    }
    return address;
}
} // namespace

std::uint64_t RefuseHostObject(const char* name) {
    std::fprintf(stderr, "PS5 HLE object ABI unsupported: %s\n", name != nullptr ? name : "<null>");
    return 0;
}

std::uint64_t RegisterHostObject(const char* name, const void* object, std::size_t size) {
    if (name == nullptr || object == nullptr) return 0;
    GuestExecution::GuestMutationScope epoch;
    std::lock_guard lock(objects_mutex);
    const auto previous = objects.find(name);
    if (previous != objects.end()) return previous->second;
    std::uint64_t address {};
    if (std::strcmp(name, "&LibKernel::g_stack_chk_guard") == 0 && size == sizeof(std::uint64_t)) {
        address = CopyObject(object, size, false, "PS5 HLE stack guard");
    } else if (std::strcmp(name, "&LibNet::g_in6addr_any") == 0 && size == 16) {
        address = CopyObject(object, size, false, "PS5 HLE IPv6 any address");
    } else if ((std::strcmp(name, "&LibcInternal::g_need_flag") == 0 ||
                std::strcmp(name, "&LibC::g_need_flag") == 0) && size == sizeof(std::uint32_t)) {
        // These flags have no native readers/writers. Guest-owned storage is
        // their authoritative mutable ABI object for the lifetime of the run.
        address = CopyObject(object, size, true, "PS5 HLE libc need flag");
    } else if (std::strcmp(name, "&LibKernel::g_progname") == 0 && size == sizeof(std::uint64_t)) {
        std::array<char, ProgNameSize> text {};
        const auto* native_text = *static_cast<const char* const*>(object);
        if (native_text != nullptr) std::strncpy(text.data(), native_text, text.size() - 1);
        progname_string = CopyObject(text.data(), text.size(), false, "PS5 HLE program name string");
        if (progname_string != 0)
            address = CopyObject(&progname_string, sizeof(progname_string), false, "PS5 HLE program name pointer");
        if (address == 0 && progname_string != 0) {
            Libs::LibKernel::Memory::FreeGuestMemory(progname_string, ObjectPage);
            progname_string = 0;
        }
    } else return RefuseHostObject(name);
    if (address != 0) objects.emplace(name, address);
    else std::fprintf(stderr, "PS5 HLE object allocation failed: %s\n", name);
    return address;
}

void UpdateGuestProgName(const char* name) {
    GuestExecution::GuestMutationScope epoch;
    std::lock_guard lock(objects_mutex);
    if (progname_string == 0) return;
    std::array<char, ProgNameSize> text {};
    if (name != nullptr) std::strncpy(text.data(), name, text.size() - 1);
    const bool unlocked = Libs::LibKernel::Memory::ProtectGuestMemory(progname_string, ObjectPage, Mode::ReadWrite);
#if defined(LUMEN_PS5_FEXCORE)
    const bool copied = unlocked && Libs::LibKernel::Memory::WriteGuestMemory(
        nullptr, progname_string, text.data(), text.size());
#else
    const bool copied = unlocked && Libs::LibKernel::Memory::TryWriteBacking(
        progname_string, text.data(), text.size());
#endif
    const bool readonly = unlocked && Libs::LibKernel::Memory::ProtectGuestMemory(
        progname_string, ObjectPage, Mode::Read);
    if (!copied || !readonly) {
        std::fprintf(stderr, "PS5 HLE program name backing update failed\n");
        std::abort();
    }
}

bool HostObjectSelfTest() noexcept {
#if !defined(LUMEN_PS5_FEXCORE)
    return false;
#else
    using namespace Libs::LibKernel::Memory;
    const std::uint64_t guard = 0x1122334455667788;
    const std::uint32_t any[4] {};
    std::uint32_t need = 1;
    const char* name = "before";
    const auto guard_address = RegisterTypedHostObject("&LibKernel::g_stack_chk_guard", &guard);
    const auto any_address = RegisterTypedHostObject("&LibNet::g_in6addr_any", &any);
    const auto need_address = RegisterTypedHostObject("&LibC::g_need_flag", &need);
    const auto name_address = RegisterTypedHostObject("&LibKernel::g_progname", &name);
    std::uint64_t read_guard {}, text_address {};
    std::array<std::uint32_t, 4> read_any {1, 1, 1, 1};
    std::array<char, ProgNameSize> text {};
    std::uint32_t read_need {}, zero {};
    bool pass = guard_address != 0 && any_address != 0 && need_address != 0 && name_address != 0 &&
        guard_address != reinterpret_cast<std::uint64_t>(&guard) &&
        ReadGuestMemory(nullptr, guard_address, &read_guard, sizeof(read_guard)) && read_guard == guard &&
        !QueryGuestAccess(guard_address, sizeof(guard), Mode::Write) &&
        ReadGuestMemory(nullptr, any_address, read_any.data(), sizeof(read_any)) &&
        read_any == std::array<std::uint32_t, 4> {} &&
        !QueryGuestAccess(any_address, sizeof(any), Mode::Write) &&
        ReadGuestMemory(nullptr, need_address, &read_need, sizeof(read_need)) && read_need == 1 &&
        WriteGuestMemory(nullptr, need_address, &zero, sizeof(zero)) &&
        ReadGuestMemory(nullptr, need_address, &read_need, sizeof(read_need)) && read_need == 0 && need == 1 &&
        ReadGuestMemory(nullptr, name_address, &text_address, sizeof(text_address)) && text_address != 0 &&
        text_address != reinterpret_cast<std::uint64_t>(name) &&
        ReadGuestMemory(nullptr, text_address, text.data(), text.size()) &&
        std::strcmp(text.data(), "before") == 0 &&
        !QueryGuestAccess(name_address, sizeof(text_address), Mode::Write) &&
        !QueryGuestAccess(text_address, text.size(), Mode::Write) &&
        RegisterHostObject("stdout", &guard, sizeof(guard)) == 0 &&
        RegisterHostObject("&DbgAddressSanitizer::g_fake_stack", &need, sizeof(need)) == 0;
    UpdateGuestProgName("after");
    pass &= ReadGuestMemory(nullptr, text_address, text.data(), text.size()) &&
        std::strcmp(text.data(), "after") == 0 && !QueryGuestAccess(text_address, text.size(), Mode::Write);
    std::printf("PS5 HLE objects: %s; owned ABI data, mutable guest flags, readonly program-name update, unsupported objects denied\n",
                pass ? "PASS" : "FAIL");
    return pass;
#endif
}
} // namespace Loader
