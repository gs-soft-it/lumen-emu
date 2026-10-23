// SPDX-License-Identifier: GPL-2.0-only
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <source_location>
#include <string_view>
#include <unistd.h>
#include "common/assert.h"
#include "core/loader/compiled_sdk.h"
#include "core/loader/elf.h"

// Keep diagnostics independent of emulator/session initialization. The parser
// and file I/O below are the production implementations.
namespace Common::Log {
std::unordered_map<std::string_view, std::shared_ptr<spdlog::logger>> ALL_LOGGERS;
}
namespace Common {
std::string GetCurrentThreadName() { return "elf-loader-test"; }
}
void assert_fail_impl() { std::abort(); }
[[noreturn]] void unreachable_impl() { std::abort(); }

namespace {
void Require(bool condition, const std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(stderr, "ELF/SELF loader regression failed at %u\n", location.line());
        std::abort();
    }
}
template <typename F> void Reject(F&& function) {
    try { function(); } catch (const std::runtime_error&) { return; }
    Require(false);
}
struct Fixture {
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("lumen-elf-loader-" + std::to_string(getpid()) + "-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { Require(std::filesystem::create_directory(directory)); }
    ~Fixture() { std::filesystem::remove_all(directory); }
    auto Write(const std::vector<u8>& bytes) const {
        const auto path = directory / "fixture.bin";
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        Require(output.good());
        return path;
    }
};
template <typename T> void Put(std::vector<u8>& bytes, size_t offset, const T& object) {
    Require(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset);
    std::memcpy(bytes.data() + offset, &object, sizeof(T));
}

void CheckCompiledSdk(Fixture& fixture, const elf_header& valid_header) {
    auto header = valid_header;
    header.e_phnum = 2;
    header.e_shoff = 0;
    header.e_shnum = 0;
    header.e_shentsize = 0;
    elf_program_header load{};
    load.p_type = PT_LOAD;
    load.p_offset = 0x1000;
    load.p_vaddr = 0x2000;
    load.p_filesz = load.p_memsz = 0x80;
    load.p_align = 0x1000;
    auto proc = load;
    proc.p_type = PT_SCE_PROCPARAM;
    proc.p_offset += 0x10;
    proc.p_vaddr += 0x10;
    proc.p_filesz = proc.p_memsz = 0x40;
    proc.p_align = 8;

    for (const bool self : {false, true}) {
        self_header container{};
        container.magic = self_header::signature;
        container.mode = container.endian = container.category = container.program_type = 1;
        container.attributes = 0x12;
        container.header_size = 0x100;
        container.file_size = 0x280;
        container.segment_count = 1;
        self_segment_header segment{0x800, 0x200, 0x80, 0x80};
        const size_t elf_offset = self ? sizeof(container) + sizeof(segment) : 0;
        const size_t data_offset = self ? segment.file_offset : load.p_offset;
        std::vector<u8> bytes(self ? container.file_size : load.p_offset + load.p_filesz);
        if (self) {
            Put(bytes, 0, container);
            Put(bytes, sizeof(container), segment);
        }
        Put(bytes, elf_offset, header);
        Put(bytes, elf_offset + header.e_phoff, load);
        Put(bytes, elf_offset + header.e_phoff + sizeof(load), proc);
        const size_t proc_offset = data_offset + proc.p_offset - load.p_offset;
        Put(bytes, proc_offset, u64{0x40});
        Put(bytes, proc_offset + 8, u32{0x4942524f});
        Put(bytes, proc_offset + 12, u32{1});
        for (const u64 sdk : {0ULL, 0x1700081ULL, 0x9000081ULL}) {
            Put(bytes, proc_offset + 16, sdk);
            Require(Core::ReadCompiledSdkVersion(fixture.Write(bytes)) == static_cast<s32>(sdk));
        }
        auto absent = bytes;
        auto no_proc = proc;
        no_proc.p_type = PT_NULL;
        Put(absent, elf_offset + header.e_phoff + sizeof(load), no_proc);
        Require(Core::ReadCompiledSdkVersion(fixture.Write(absent)) == 0);
        auto invalid = bytes;
        invalid[elf_offset] = 0;
        Require(Core::ReadCompiledSdkVersion(fixture.Write(invalid)) == 0);
    }
    Require(Core::ReadCompiledSdkVersion(fixture.directory / "missing.bin") == 0);
}
}

int main(int argc, char** argv) {
    Fixture fixture;
    elf_header header{};
    const std::array<u8, 4> magic{0x7f, 'E', 'L', 'F'};
    std::memcpy(header.e_ident.magic, magic.data(), magic.size());
    header.e_ident.ei_class = ELF_CLASS_64;
    header.e_ident.ei_data = ELF_DATA_2LSB;
    header.e_ident.ei_version = ELF_VERSION_CURRENT;
    header.e_ident.ei_osabi = ELF_OSABI_FREEBSD;
    header.e_type = ET_SCE_DYNEXEC;
    header.e_machine = EM_X86_64;
    header.e_version = EV_CURRENT;
    header.e_ehsize = sizeof(header);
    header.e_phoff = sizeof(header);
    header.e_phentsize = sizeof(elf_program_header);
    header.e_phnum = 1;
    header.e_shoff = 1ULL << 30;
    header.e_shentsize = sizeof(elf_section_header);
    header.e_shnum = 2;
    CheckCompiledSdk(fixture, header);

    elf_program_header program{};
    program.p_type = PT_LOAD;
    program.p_offset = 0x1000;
    program.p_vaddr = 0x2000;
    program.p_filesz = 16;
    program.p_memsz = 32;
    program.p_align = 0x1000;

    self_header container{};
    container.magic = self_header::signature;
    container.mode = container.endian = container.category = container.program_type = 1;
    container.attributes = 0x12;
    container.header_size = 0x100;
    container.file_size = 0x280;
    container.segment_count = 1;
    self_segment_header segment{0x800, 0x200, 16, 16};
    constexpr size_t elf_offset = sizeof(container) + sizeof(segment);
    std::vector<u8> bytes(container.file_size);
    Put(bytes, 0, container);
    Put(bytes, sizeof(container), segment);
    Put(bytes, elf_offset, header);
    Put(bytes, elf_offset + header.e_phoff, program);
    for (size_t i = 0; i < program.p_filesz; ++i) bytes[segment.file_offset + i] = 0xa0 + i;

    Core::Loader::Elf elf;
    elf.Open(fixture.Write(bytes));
    Require(elf.IsElfFile() && elf.GetProgramHeader().size() == 1);
    std::array<u8, 10> output{};
    output.fill(0x55);
    elf.LoadSegment(reinterpret_cast<u64>(output.data() + 1), program.p_offset + 4, 8);
    Require(output.front() == 0x55 && output.back() == 0x55);
    for (size_t i = 0; i < 8; ++i) Require(output[i + 1] == 0xa4 + i);
    Reject([&] { elf.LoadSegment(reinterpret_cast<u64>(output.data()), program.p_offset + 15, 2); });

    for (const u64 unsupported : {2ULL, 8ULL}) {
        auto invalid = bytes;
        auto bad_segment = segment;
        bad_segment.flags |= unsupported;
        Put(invalid, sizeof(container), bad_segment);
        elf.Open(fixture.Write(invalid));
        Require(!elf.IsElfFile() && elf.GetProgramHeader().empty() && elf.GetSegmentHeader().empty());
    }
    auto invalid = bytes;
    auto bad_segment = segment;
    bad_segment.file_offset = bytes.size() - 1;
    Put(invalid, sizeof(container), bad_segment);
    elf.Open(fixture.Write(invalid));
    Require(!elf.IsElfFile());

    // The same absent section table is invalid for a raw ELF, whose offsets
    // are physical. A resident section table remains accepted.
    std::vector<u8> raw(bytes.size());
    program.p_offset = 0x200;
    Put(raw, 0, header);
    Put(raw, header.e_phoff, program);
    elf.Open(fixture.Write(raw));
    Require(!elf.IsElfFile());
    header.e_shoff = 0x100;
    Put(raw, 0, header);
    elf.Open(fixture.Write(raw));
    Require(elf.IsElfFile());
    header.e_phoff = raw.size() - 1;
    Put(raw, 0, header);
    elf.Open(fixture.Write(raw));
    Require(!elf.IsElfFile());

    // Optional local metadata-only checks never read or print guest payloads.
    std::optional<u32> expected_sdk;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        if (argument.starts_with("--sdk=")) {
            expected_sdk = static_cast<u32>(std::stoul(std::string(argument.substr(6)), nullptr, 0));
            continue;
        }
        elf.Open(argv[i]);
        Require(elf.IsElfFile());
        const auto sdk = static_cast<u32>(Core::ReadCompiledSdkVersion(argv[i]));
        if (expected_sdk) {
            Require(sdk == *expected_sdk);
        }
        std::printf("ELF/SELF SDK metadata: %#x\n", sdk);
    }
    std::puts("ELF/SELF physical loader and compiled SDK regression PASS");
}
