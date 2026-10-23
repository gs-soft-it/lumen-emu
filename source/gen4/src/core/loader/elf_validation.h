// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include "core/loader/elf.h"

namespace Core::Loader {

constexpr bool ContainsFileRange(u64 file_size, u64 offset, u64 size) {
    return offset <= file_size && size <= file_size - offset;
}

inline bool ValidProgramHeaders(std::span<const elf_program_header> headers, u64 file_size,
                                bool self_file = false) {
    constexpr u64 limit = std::numeric_limits<u64>::max();
    for (const auto& header : headers) {
        if (header.p_type == PT_NULL) {
            continue;
        }
        if (!self_file && header.p_filesz != 0 &&
            !ContainsFileRange(file_size, header.p_offset, header.p_filesz)) {
            return false;
        }
        if (header.p_align != 0 && (header.p_align & (header.p_align - 1)) != 0) {
            return false;
        }
        const u64 padding = header.p_align == 0 ? 0 : header.p_align - 1;
        if (header.p_memsz > limit - padding) {
            return false;
        }
        const u64 aligned_size = (header.p_memsz + padding) & ~padding;
        if (header.p_vaddr > limit - aligned_size) {
            return false;
        }
        if ((header.p_type == PT_LOAD || header.p_type == PT_SCE_RELRO ||
             header.p_type == PT_TLS) && header.p_filesz > header.p_memsz) {
            return false;
        }
        // These fields become u32 in the guest TLS descriptor.
        if (header.p_type == PT_TLS &&
            (aligned_size > std::numeric_limits<u32>::max() ||
             header.p_align > std::numeric_limits<u32>::max())) {
            return false;
        }
    }
    return true;
}

struct DynamicTableRange {
    std::optional<u64> offset;
    u64 size{};
};

struct DynamicTableBounds {
    DynamicTableRange strings, symbols, relocations, jump_relocations, hash;
    size_t entry_count{};
};

inline const char* CheckedDynamicString(std::span<const u8> data,
                                        const DynamicTableRange& strings, u64 offset) {
    if (!strings.offset || offset >= strings.size ||
        !ContainsFileRange(data.size(), *strings.offset, strings.size)) {
        throw std::runtime_error("ELF dynamic string is outside the string table");
    }
    const auto* text = reinterpret_cast<const char*>(data.data() + *strings.offset + offset);
    if (std::memchr(text, 0, strings.size - offset) == nullptr) {
        throw std::runtime_error("ELF dynamic string has no terminator");
    }
    return text;
}

inline DynamicTableBounds ValidateDynamicTables(std::span<const u8> dynamic,
                                                std::span<const u8> data) {
    if (dynamic.empty()) {
        return {};
    }
    if (dynamic.size() % sizeof(elf_dynamic) != 0) {
        throw std::runtime_error("ELF dynamic table has a partial entry");
    }
    DynamicTableBounds bounds;
    bool terminated = false;
    for (size_t offset = 0; offset < dynamic.size(); offset += sizeof(elf_dynamic)) {
        elf_dynamic entry{};
        std::memcpy(&entry, dynamic.data() + offset, sizeof(entry));
        if (entry.d_tag == DT_NULL) {
            terminated = true;
            break;
        }
        ++bounds.entry_count;
        switch (entry.d_tag) {
        case DT_SCE_STRTAB: bounds.strings.offset = entry.d_un.d_ptr; break;
        case DT_SCE_STRSZ: bounds.strings.size = entry.d_un.d_val; break;
        case DT_SCE_SYMTAB: bounds.symbols.offset = entry.d_un.d_ptr; break;
        case DT_SCE_SYMTABSZ: bounds.symbols.size = entry.d_un.d_val; break;
        case DT_SCE_RELA: bounds.relocations.offset = entry.d_un.d_ptr; break;
        case DT_SCE_RELASZ: bounds.relocations.size = entry.d_un.d_val; break;
        case DT_SCE_JMPREL: bounds.jump_relocations.offset = entry.d_un.d_ptr; break;
        case DT_SCE_PLTRELSZ: bounds.jump_relocations.size = entry.d_un.d_val; break;
        case DT_SCE_HASH: bounds.hash.offset = entry.d_un.d_ptr; break;
        case DT_SCE_HASHSZ: bounds.hash.size = entry.d_un.d_val; break;
        case DT_SCE_SYMENT:
            if (entry.d_un.d_val != sizeof(elf_symbol)) {
                throw std::runtime_error("ELF symbol entry size is invalid");
            }
            break;
        case DT_SCE_RELAENT:
            if (entry.d_un.d_val != sizeof(elf_relocation)) {
                throw std::runtime_error("ELF relocation entry size is invalid");
            }
            break;
        default: break;
        }
    }
    if (!terminated) {
        throw std::runtime_error("ELF dynamic table has no DT_NULL terminator");
    }
    const auto validate = [&data](const DynamicTableRange& range, u64 entry_size) {
        if ((!range.offset && range.size != 0) ||
            (range.offset && !ContainsFileRange(data.size(), *range.offset, range.size)) ||
            range.size % entry_size != 0) {
            throw std::runtime_error("ELF dynamic table exceeds its backing segment");
        }
    };
    validate(bounds.strings, 1);
    validate(bounds.symbols, sizeof(elf_symbol));
    validate(bounds.relocations, sizeof(elf_relocation));
    validate(bounds.jump_relocations, sizeof(elf_relocation));
    validate(bounds.hash, 1);
    if ((bounds.symbols.offset && *bounds.symbols.offset % alignof(elf_symbol) != 0) ||
        (bounds.relocations.offset && *bounds.relocations.offset % alignof(elf_relocation) != 0) ||
        (bounds.jump_relocations.offset &&
         *bounds.jump_relocations.offset % alignof(elf_relocation) != 0)) {
        throw std::runtime_error("ELF dynamic table has an unaligned offset");
    }
    for (u64 offset = 0; offset < bounds.symbols.size; offset += sizeof(elf_symbol)) {
        elf_symbol symbol{};
        std::memcpy(&symbol, data.data() + *bounds.symbols.offset + offset, sizeof(symbol));
        CheckedDynamicString(data, bounds.strings, symbol.st_name);
    }
    const auto validate_relocations = [&data, &bounds](const DynamicTableRange& range) {
        for (u64 offset = 0; offset < range.size; offset += sizeof(elf_relocation)) {
            elf_relocation relocation{};
            std::memcpy(&relocation, data.data() + *range.offset + offset, sizeof(relocation));
            const u32 symbol = relocation.GetSymbol();
            if (symbol != 0 && symbol >= bounds.symbols.size / sizeof(elf_symbol)) {
                throw std::runtime_error("ELF relocation references a missing symbol");
            }
        }
    };
    validate_relocations(bounds.relocations);
    validate_relocations(bounds.jump_relocations);
    return bounds;
}

} // namespace Core::Loader
