// Lumen modifications, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
// Reads a real executable with the imported loader. It never maps or executes
// guest code, resolves HLE imports, applies instruction patches or starts GPU.
#include "loader/elf.h"
#include "common/file.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json;
constexpr std::uint64_t MaxMetadataBytes = 256U * 1024U * 1024U;

struct Preflight {
    Loader::Elf64 elf;
    std::map<Loader::Elf64_Sxword, std::uint64_t> tags;
    std::uint64_t file_size {};

    explicit Preflight(const std::filesystem::path& path) {
        file_size = std::filesystem::file_size(path);
        ValidateContainer(path);
        elf.Open(path);
        Require(elf.IsValid(), "loader rejected SELF/ELF header");
        const auto* header = elf.GetEhdr();
        Require(header != nullptr && elf.GetPhdr() != nullptr, "missing ELF program headers");
        Require(header->e_phnum != 0 && header->e_phnum <= 1024, "invalid program-header count");
        Require(header->e_ehsize == sizeof(Loader::Elf64_Ehdr), "invalid ELF header size");

        const Loader::Elf64_Phdr* dynamic {};
        bool entry_executable {};
        for (std::uint16_t i = 0; i != header->e_phnum; ++i) {
            const auto& ph = elf.GetPhdr()[i];
            Require(ph.p_filesz <= file_size, "segment larger than source file");
            Require(ph.p_memsz <= std::numeric_limits<std::uint64_t>::max() - ph.p_vaddr,
                    "segment virtual-address overflow");
            Require(ph.p_filesz <= std::numeric_limits<std::uint64_t>::max() - ph.p_offset,
                    "segment file-offset overflow");
            if (ph.p_type == Loader::PT_LOAD || ph.p_type == Loader::PT_OS_RELRO)
                Require(ph.p_filesz <= ph.p_memsz, "load segment file bytes exceed memory size");
            if (ph.p_type == Loader::PT_LOAD && (ph.p_flags & Loader::PF_X) &&
                header->e_entry >= ph.p_vaddr && header->e_entry - ph.p_vaddr < ph.p_memsz)
                entry_executable = true;
            if (ph.p_type == Loader::PT_DYNAMIC) {
                Require(dynamic == nullptr, "duplicate dynamic segment");
                dynamic = &ph;
            }
        }
        Require(entry_executable, "entry is outside executable load segments");
        Require(dynamic != nullptr && dynamic->p_filesz != 0 &&
                dynamic->p_filesz % sizeof(Loader::Elf64_Dyn) == 0 &&
                dynamic->p_filesz <= MaxMetadataBytes && elf.GetDynamic() != nullptr,
                "invalid or missing dynamic table");
        bool terminated {};
        for (std::size_t i = 0; i != dynamic->p_filesz / sizeof(Loader::Elf64_Dyn); ++i) {
            const auto& value = elf.GetDynamic()[i];
            if (value.d_tag == Loader::DT_NULL) {
                terminated = true;
                break;
            }
            // Repeatable module/library/needed records remain available from
            // the bounded dynamic span; only scalar table descriptors use map.
            const bool repeatable = value.d_tag == Loader::DT_NEEDED ||
                value.d_tag == Loader::DT_OS_NEEDED_MODULE ||
                value.d_tag == Loader::DT_OS_NEEDED_MODULE_1 ||
                value.d_tag == Loader::DT_OS_IMPORT_LIB ||
                value.d_tag == Loader::DT_OS_IMPORT_LIB_1 ||
                value.d_tag == Loader::DT_OS_IMPORT_LIB_ATTR;
            if (!repeatable)
                Require(tags.emplace(value.d_tag, value.d_un.d_val).second,
                        "duplicate scalar dynamic tag");
        }
        Require(terminated, "dynamic table lacks a bounded DT_NULL terminator");
    }

    static void Require(bool condition, const char* reason) {
        if (!condition) throw std::runtime_error(reason);
    }

    // Guard container bounds before invoking the inherited parser, whose file
    // reads do not otherwise report every short read to their callers. This
    // precheck does not replace Elf64's format validation or segment reader.
    void ValidateContainer(const std::filesystem::path& path) const {
        std::ifstream source(path, std::ios::binary);
        Require(bool(source), "unable to open source executable");
        const auto read = [&](std::uint64_t offset, void* destination, std::size_t size) {
            Require(offset <= file_size && size <= file_size - offset,
                    "truncated executable container");
            source.seekg(static_cast<std::streamoff>(offset));
            source.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
            Require(bool(source), "short source executable read");
        };
        Loader::SelfHeader self {};
        read(0, &self, sizeof(self));
        const bool is_self = (self.ident[0] == 0x4f && self.ident[1] == 0x15 &&
                              self.ident[2] == 0x3d && self.ident[3] == 0x1d) ||
                             (self.ident[0] == 0x54 && self.ident[1] == 0x14 &&
                              self.ident[2] == 0xf5 && self.ident[3] == 0xee);
        std::uint64_t elf_offset {};
        std::vector<Loader::SelfSegment> self_segments;
        if (is_self) {
            Require(self.segments_num != 0 && self.segments_num <= 1024,
                    "invalid SELF segment count");
            Require(self.file_size <= file_size, "SELF declared size exceeds source file");
            self_segments.resize(self.segments_num);
            read(sizeof(self), self_segments.data(), self_segments.size() * sizeof(Loader::SelfSegment));
            elf_offset = sizeof(self) + self_segments.size() * sizeof(Loader::SelfSegment);
        }
        Loader::Elf64_Ehdr header {};
        read(elf_offset, &header, sizeof(header));
        Require(header.e_phnum != 0 && header.e_phnum <= 1024 &&
                header.e_phentsize == sizeof(Loader::Elf64_Phdr), "invalid program-header shape");
        Require(header.e_phoff >= sizeof(header) && header.e_phoff <= file_size - elf_offset,
                "invalid program-header offset");
        std::vector<Loader::Elf64_Phdr> phdrs(header.e_phnum);
        read(elf_offset + header.e_phoff, phdrs.data(), phdrs.size() * sizeof(Loader::Elf64_Phdr));
        for (const auto& segment : self_segments) {
            Require(segment.offset <= file_size && segment.compressed_size <= file_size - segment.offset,
                    "SELF segment exceeds source file");
            if ((segment.type & 0x800U) != 0) {
                const auto index = (segment.type >> 20U) & 0xfffU;
                Require(index < phdrs.size(), "SELF program-header index out of range");
                Require(segment.decompressed_size == phdrs[index].p_filesz &&
                        segment.compressed_size == segment.decompressed_size,
                        "SELF requires unsupported decompression or segment sizing");
            }
        }
        for (const auto& ph : phdrs) {
            Require(ph.p_filesz <= std::numeric_limits<std::uint64_t>::max() - ph.p_offset,
                    "segment file-offset overflow");
            if (ph.p_type != Loader::PT_DYNAMIC && ph.p_type != Loader::PT_OS_DYNLIBDATA) continue;
            Require(ph.p_filesz <= MaxMetadataBytes, "dynamic metadata exceeds bounded preflight size");
            bool contained = !is_self && ph.p_offset <= file_size && ph.p_filesz <= file_size - ph.p_offset;
            for (const auto& segment : self_segments) {
                if ((segment.type & 0x800U) == 0) continue;
                const auto& backing = phdrs[(segment.type >> 20U) & 0xfffU];
                if (ph.p_offset >= backing.p_offset && ph.p_offset - backing.p_offset < backing.p_filesz &&
                    ph.p_filesz <= backing.p_filesz - (ph.p_offset - backing.p_offset)) contained = true;
            }
            Require(contained, "dynamic metadata has no complete stored segment");
        }
        if (!is_self && header.e_shnum != 0) {
            Require(header.e_shentsize == sizeof(Loader::Elf64_Shdr) && header.e_shoff <= file_size,
                    "invalid section-header shape");
            std::vector<Loader::Elf64_Shdr> sections(header.e_shnum);
            read(header.e_shoff, sections.data(), sections.size() * sizeof(Loader::Elf64_Shdr));
            if (header.e_shstrndx < sections.size()) {
                const auto& strings = sections[header.e_shstrndx];
                Require(strings.sh_offset <= file_size && strings.sh_size <= file_size - strings.sh_offset &&
                        strings.sh_size <= MaxMetadataBytes, "invalid section string-table range");
            }
        }
    }

    std::uint64_t Value(Loader::Elf64_Sxword standard, Loader::Elf64_Sxword os = 0) const {
        const auto a = tags.find(standard);
        const auto b = os == 0 ? tags.end() : tags.find(os);
        Require(a == tags.end() || b == tags.end(), "conflicting standard and OS dynamic tags");
        Require(a != tags.end() || b != tags.end(), "required dynamic tag is absent");
        return a != tags.end() ? a->second : b->second;
    }

    std::vector<std::uint8_t> Table(Loader::Elf64_Sxword standard,
            Loader::Elf64_Sxword os, std::uint64_t size) {
        Require(size != 0 && size <= file_size && size <= MaxMetadataBytes,
                "invalid metadata table size");
        const auto address = Value(standard, os);
        const bool os_relative = tags.contains(os);
        const auto* header = elf.GetEhdr();
        for (std::uint16_t i = 0; i != header->e_phnum; ++i) {
            const auto& ph = elf.GetPhdr()[i];
            if (os_relative && ph.p_type != Loader::PT_OS_DYNLIBDATA) continue;
            if (!os_relative && ph.p_type != Loader::PT_LOAD &&
                ph.p_type != Loader::PT_OS_RELRO) continue;
            const auto origin = os_relative ? 0 : ph.p_vaddr;
            if (address < origin || address - origin > ph.p_filesz ||
                size > ph.p_filesz - (address - origin)) continue;
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            elf.LoadSegment(reinterpret_cast<std::uint64_t>(bytes.data()),
                            ph.p_offset + address - origin, size);
            return bytes;
        }
        throw std::runtime_error("metadata table is outside file-backed load segments");
    }

    Json Inspect() {
        const auto* header = elf.GetEhdr();
        const auto string_size = Value(Loader::DT_STRSZ, Loader::DT_OS_STRSZ);
        auto strings = Table(Loader::DT_STRTAB, Loader::DT_OS_STRTAB, string_size);
        const auto symbol_size = Value(Loader::DT_OS_SYMTABSZ);
        Require(Value(Loader::DT_SYMENT, Loader::DT_OS_SYMENT) == sizeof(Loader::Elf64_Sym) &&
                symbol_size % sizeof(Loader::Elf64_Sym) == 0, "invalid symbol entry/table size");
        auto symbols = Table(Loader::DT_SYMTAB, Loader::DT_OS_SYMTAB, symbol_size);
        const auto symbol_count = symbol_size / sizeof(Loader::Elf64_Sym);
        Json imports = Json::array();
        for (std::size_t i = 0; i != symbol_count; ++i) {
            Loader::Elf64_Sym symbol {};
            std::memcpy(&symbol, symbols.data() + i * sizeof(symbol), sizeof(symbol));
            Require(symbol.st_name < strings.size(), "symbol name lies outside string table");
            const auto* start = reinterpret_cast<const char*>(strings.data() + symbol.st_name);
            const auto* end = static_cast<const char*>(
                std::memchr(start, 0, strings.size() - symbol.st_name));
            Require(end != nullptr, "symbol name lacks a bounded terminator");
            if (symbol.st_shndx == 0 && symbol.GetBind() != Loader::STB_LOCAL) {
                imports.push_back({{"symbol_index", i}, {"name", std::string(start, end)},
                    {"bind", symbol.GetBind()}, {"type", symbol.GetType()},
                    {"resolution", "NOT_RUN"}});
            }
        }
        std::size_t relocations {};
        std::size_t plt_relocations {};
        for (const bool plt : {false, true}) {
            const auto size = plt ? Value(Loader::DT_PLTRELSZ, Loader::DT_OS_PLTRELSZ) :
                                    Value(Loader::DT_RELASZ, Loader::DT_OS_RELASZ);
            Require(size % sizeof(Loader::Elf64_Rela) == 0, "invalid relocation table size");
            if (plt)
                Require(Value(Loader::DT_PLTREL, Loader::DT_OS_PLTREL) == Loader::DT_RELA,
                        "unsupported PLT relocation encoding");
            else
                Require(Value(Loader::DT_RELAENT, Loader::DT_OS_RELAENT) ==
                        sizeof(Loader::Elf64_Rela), "unsupported relocation entry size");
            if (size == 0) continue;
            auto bytes = Table(plt ? Loader::DT_JMPREL : Loader::DT_RELA,
                               plt ? Loader::DT_OS_JMPREL : Loader::DT_OS_RELA, size);
            for (std::size_t i = 0; i != size / sizeof(Loader::Elf64_Rela); ++i) {
                Loader::Elf64_Rela relocation {};
                std::memcpy(&relocation, bytes.data() + i * sizeof(relocation), sizeof(relocation));
                Require(relocation.GetSymbol() < symbol_count, "relocation symbol index out of range");
                const auto type = relocation.GetType();
                Require(type == Loader::R_X86_64_64 || type == Loader::R_X86_64_GLOB_DAT ||
                        type == Loader::R_X86_64_JUMP_SLOT || type == Loader::R_X86_64_RELATIVE ||
                        type == Loader::R_X86_64_DTPMOD64, "unsupported relocation type");
                bool destination {};
                for (std::uint16_t j = 0; j != header->e_phnum; ++j) {
                    const auto& ph = elf.GetPhdr()[j];
                    if (ph.p_type != Loader::PT_LOAD && ph.p_type != Loader::PT_OS_RELRO) continue;
                    if (ph.p_memsz >= sizeof(std::uint64_t) && relocation.r_offset >= ph.p_vaddr &&
                        relocation.r_offset - ph.p_vaddr <= ph.p_memsz - sizeof(std::uint64_t))
                        destination = true;
                }
                Require(destination, "relocation destination outside mapped program segments");
            }
            (plt ? plt_relocations : relocations) = size / sizeof(Loader::Elf64_Rela);
        }
        Json segments = Json::array();
        Json tls = Json::array();
        for (std::uint16_t i = 0; i != header->e_phnum; ++i) {
            const auto& ph = elf.GetPhdr()[i];
            segments.push_back({{"index", i}, {"type", ph.p_type}, {"flags", ph.p_flags},
                {"virtual_address", ph.p_vaddr}, {"file_size", ph.p_filesz}, {"memory_size", ph.p_memsz}});
            if (ph.p_type == Loader::PT_TLS)
                tls.push_back({{"file_size", ph.p_filesz}, {"memory_size", ph.p_memsz},
                               {"alignment", ph.p_align}});
        }
        return {{"format", "LUMEN_PS5_REAL_LOADER_PREFLIGHT_V1"}, {"loader", "Lumen Gen5 Elf64"},
            {"self", elf.IsSelf()}, {"next_gen", elf.IsNextGen()}, {"file_size", file_size},
            {"machine", header->e_machine}, {"entry", header->e_entry},
            {"program_header_count", header->e_phnum}, {"symbols", symbol_count},
            {"relocations", relocations}, {"plt_relocations", plt_relocations},
            {"imports", imports}, {"segments", segments}, {"tls", tls},
            {"import_resolution", "NOT_RUN"}, {"guest_execution", false},
            {"commercial_game_qualified", false}};
    }
};
} // namespace

int main(int argc, char** argv) {
    if (argc != 2 && (argc != 4 || std::strcmp(argv[2], "--output") != 0)) {
        std::fprintf(stderr, "Usage: lumen_ps5_elf_preflight executable [--output metadata.json]\n");
        return 2;
    }
    try {
        Preflight preflight(argv[1]);
        const auto metadata = preflight.Inspect();
        if (argc == 4) {
            std::ofstream output(argv[3], std::ios::trunc);
            output << metadata.dump(2) << '\n';
            if (!output) throw std::runtime_error("unable to write metadata receipt");
        }
        std::printf("LUMEN_PS5_REAL_LOADER_PREFLIGHT_V1 self=%u nextGen=%u machine=%u phdrs=%u "
            "symbols=%zu imports=%zu relocations=%zu pltRelocations=%zu "
            "importResolution=NOT_RUN guestExecuted=0 commercialGameQualified=0\n",
            preflight.elf.IsSelf(), preflight.elf.IsNextGen(), preflight.elf.GetEhdr()->e_machine,
            preflight.elf.GetEhdr()->e_phnum, metadata["symbols"].get<std::size_t>(),
            metadata["imports"].size(), metadata["relocations"].get<std::size_t>(),
            metadata["plt_relocations"].get<std::size_t>());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "PS5 real-loader preflight rejected: %s\n", error.what());
        return 1;
    }
}
