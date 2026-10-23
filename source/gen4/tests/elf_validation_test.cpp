// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdio>
#include <cstdlib>
#include "core/loader/elf_validation.h"

namespace {
void Require(bool condition) {
    if (!condition) {
        std::fputs("ELF validation regression failed\n", stderr);
        std::abort();
    }
}
template <typename F> void Reject(F&& function) {
    try {
        function();
    } catch (const std::runtime_error&) {
        return;
    }
    Require(false);
}
elf_dynamic Entry(s64 tag, u64 value) {
    elf_dynamic entry{};
    entry.d_tag = tag;
    entry.d_un.d_val = value;
    return entry;
}
template <size_t N> auto Bytes(const std::array<elf_dynamic, N>& entries) {
    return std::span<const u8>{reinterpret_cast<const u8*>(entries.data()), sizeof(entries)};
}
}

int main() {
    using namespace Core::Loader;
    constexpr u64 max = std::numeric_limits<u64>::max();
    Require(ContainsFileRange(64, 64, 0));
    Require(!ContainsFileRange(64, max, 1));
    Require(!ContainsFileRange(64, 1, max));

    elf_program_header valid{};
    valid.p_type = PT_LOAD;
    valid.p_offset = 64;
    valid.p_filesz = 64;
    valid.p_memsz = 128;
    valid.p_vaddr = 0x1000;
    valid.p_align = 0x1000;
    const auto check = [](elf_program_header header) {
        return ValidProgramHeaders(std::span{&header, 1}, 4096);
    };
    Require(check(valid));
    auto malformed = valid;
    malformed.p_filesz = 129;
    Require(!check(malformed));
    malformed.p_type = PT_TLS;
    Require(!check(malformed));
    malformed = valid; malformed.p_offset = max;
    Require(!check(malformed));
    malformed = valid; malformed.p_align = 3;
    Require(!check(malformed));
    malformed = valid; malformed.p_memsz = max;
    Require(!check(malformed));
    malformed = valid; malformed.p_vaddr = max - 64;
    Require(!check(malformed));
    malformed = valid; malformed.p_type = PT_TLS; malformed.p_memsz = u64{1} << 32;
    Require(!check(malformed));

    std::array<u8, 32> data{0, 'n', 'a', 'm', 'e', 0};
    elf_symbol symbol{};
    symbol.st_name = 1;
    std::memcpy(data.data() + 8, &symbol, sizeof(symbol));
    const std::array dynamic{Entry(DT_SCE_STRTAB, 0), Entry(DT_SCE_STRSZ, 6),
                             Entry(DT_SCE_SYMTAB, 8), Entry(DT_SCE_SYMTABSZ, sizeof(symbol)),
                             Entry(DT_NULL, 0)};
    const auto bounds = ValidateDynamicTables(Bytes(dynamic), data);
    Require(bounds.entry_count == 4);
    Require(std::strcmp(CheckedDynamicString(data, bounds.strings, 1), "name") == 0);
    Reject([&] { ValidateDynamicTables(Bytes(dynamic).first(sizeof(dynamic) - sizeof(elf_dynamic)), data); });
    Reject([&] { ValidateDynamicTables(Bytes(dynamic).first(sizeof(dynamic) - 1), data); });
    Reject([&] { CheckedDynamicString(data, bounds.strings, 6); });
    auto invalid_dynamic = dynamic;
    invalid_dynamic[2].d_un.d_ptr = max;
    Reject([&] { ValidateDynamicTables(Bytes(invalid_dynamic), data); });
    invalid_dynamic = dynamic;
    invalid_dynamic[3].d_un.d_val = 23;
    Reject([&] { ValidateDynamicTables(Bytes(invalid_dynamic), data); });
    symbol.st_name = 32;
    std::memcpy(data.data() + 8, &symbol, sizeof(symbol));
    Reject([&] { ValidateDynamicTables(Bytes(dynamic), data); });
    symbol.st_name = 1;
    std::memcpy(data.data() + 8, &symbol, sizeof(symbol));
    data[5] = 'x';
    Reject([&] { ValidateDynamicTables(Bytes(dynamic), data); });
    std::puts("ELF startup range validation PASS");
}
