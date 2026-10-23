// SPDX-License-Identifier: GPL-2.0-only
#include "lumen/session.h"
#include "common/sha256.h"
#include "lumen/path_boundary.h"
#include "core/loader/elf.h"
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
template <typename T> void Store(std::string& bytes, size_t offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
template <typename T> T Load(const std::string& bytes, size_t offset) {
    T value{}; std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value;
}
std::string Executable() {
    std::string bytes(0x110, '\0');
    elf_header header{};
    header.e_ident = {{ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3}, ELF_CLASS_64,
        ELF_DATA_2LSB, ELF_VERSION_CURRENT, ELF_OSABI_FREEBSD, ELF_ABI_VERSION_AMDGPU_HSA_V2, {}};
    header.e_type = ET_SCE_DYNEXEC; header.e_machine = EM_X86_64; header.e_version = EV_CURRENT;
    header.e_ehsize = sizeof(header); header.e_phoff = sizeof(header);
    header.e_phentsize = sizeof(elf_program_header); header.e_phnum = 1; header.e_entry = 0x4000;
    elf_program_header program{};
    program.p_type = PT_LOAD; program.p_flags = static_cast<elf_program_flags>(PF_READ | PF_EXEC);
    program.p_offset = 0x100; program.p_vaddr = 0x4000;
    program.p_filesz = 16; program.p_memsz = 32; program.p_align = 0x1000;
    Store(bytes, 0, header); Store(bytes, sizeof(header), program);
    bytes[0x100] = static_cast<char>(0xc3);
    return bytes;
}
std::string SelfExecutable() {
    auto raw = Executable();
    auto header = Load<elf_header>(raw, 0);
    auto program = Load<elf_program_header>(raw, sizeof(header));
    // Original ELF layout deliberately differs from SELF physical placement.
    program.p_offset = 0x4000;
    header.e_shoff = 0x9000; header.e_shnum = 3; header.e_shentsize = sizeof(elf_section_header);
    std::string bytes(0x110, '\0');
    self_header self{};
    self.magic = self_header::signature; self.mode = 1; self.endian = 1; self.attributes = 0x12;
    self.category = 1; self.program_type = 1; self.segment_count = 1;
    self.header_size = 0xc0; self.meta_size = 0x40; self.file_size = bytes.size();
    self_segment_header segment{0x800, 0x100, 16, 16};
    Store(bytes, 0, self); Store(bytes, sizeof(self), segment);
    Store(bytes, 0x40, header); Store(bytes, 0x80, program);
    bytes[0x100] = static_cast<char>(0xc3);
    return bytes;
}
}

int main(int argc, char** external_arguments) {
    namespace fs = std::filesystem;
    using namespace Lumen::Gen4;
    const auto base = fs::temp_directory_path() / ("lumen-admission-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(base / "rom/sce_sys");
    fs::create_directory(base / "data");
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code error; fs::remove_all(root, error); } } cleanup{base};
    auto require = [](bool condition, const char* text) { if (!condition) throw std::runtime_error(text); };
    auto write = [](const fs::path& path, const std::string& data) { std::ofstream(path, std::ios::binary).write(data.data(), data.size()); };
    try {
        const std::string elf = Executable();
        std::string sfo(20, '\0'); sfo[1] = 'P'; sfo[2] = 'S'; sfo[3] = 'F';
        write(base / "rom/eboot.bin", elf); write(base / "rom/sce_sys/param.sfo", sfo);
        SessionOptions options; options.root = base / "rom"; options.data = base / "data";
        options.eboot_digest = sha256::SHA256::Hex(elf); options.param_digest = sha256::SHA256::Hex(sfo);
        std::string error;
        require(ValidateContent(options, error), error.c_str());
        const auto self = SelfExecutable();
        auto format_options = options;
        format_options.eboot_digest.clear();
        const auto accepts = [&](const std::string& bytes) {
            write(base / "rom/eboot.bin", bytes);
            return ValidateContent(format_options, error);
        };
        const auto reject = [&](const std::string& bytes, const char* message) {
            require(!accepts(bytes), message);
        };
        require(accepts(self), error.c_str());
        require(accepts(self + "container trailer"), error.c_str());
        reject(self.substr(0, 20), "truncated SELF header admitted");
        reject(self.substr(0, 0x90), "truncated SELF tables admitted");
        reject(self.substr(0, self.size() - 1), "truncated SELF backing admitted");
        auto malformed = self;
        auto container = Load<self_header>(self, 0);
        container.segment_count = 0xffff; Store(malformed, 0, container);
        reject(malformed, "SELF segment table overflow admitted");
        malformed = self; container = Load<self_header>(self, 0);
        container.header_size = 0x90; Store(malformed, 0, container);
        reject(malformed, "SELF program table outside header admitted");
        for (const u64 unsupported : {u64{2}, u64{8}}) {
            malformed = self; auto segment = Load<self_segment_header>(self, 0x20);
            segment.flags |= unsupported; Store(malformed, 0x20, segment);
            reject(malformed, "encrypted or compressed SELF admitted");
        }
        malformed = self; auto segment = Load<self_segment_header>(self, 0x20);
        segment.file_offset = UINT64_MAX; Store(malformed, 0x20, segment);
        reject(malformed, "SELF backing offset overflow admitted");
        malformed = self; segment = Load<self_segment_header>(self, 0x20);
        segment.file_size = segment.memory_size = 17; Store(malformed, 0x20, segment);
        reject(malformed, "SELF backing extends beyond file");
        malformed = self; segment = Load<self_segment_header>(self, 0x20);
        segment.flags |= u64{1} << 20; Store(malformed, 0x20, segment);
        reject(malformed, "SELF invalid program index admitted");
        malformed = self; segment = Load<self_segment_header>(self, 0x20);
        segment.flags = 0; Store(malformed, 0x20, segment);
        reject(malformed, "SELF without load backing admitted");
        malformed = self; auto embedded = Load<elf_header>(self, 0x40);
        embedded.e_machine = static_cast<e_machine_es>(183); Store(malformed, 0x40, embedded);
        reject(malformed, "non-x86 SELF admitted");
        malformed = self; embedded = Load<elf_header>(self, 0x40);
        embedded.e_phoff = UINT64_MAX; Store(malformed, 0x40, embedded);
        reject(malformed, "SELF program offset overflow admitted");
        malformed = self; auto program = Load<elf_program_header>(self, 0x80);
        program.p_filesz = 17; Store(malformed, 0x80, program);
        reject(malformed, "SELF backing shorter than load admitted");
        malformed = self; program = Load<elf_program_header>(self, 0x80);
        program.p_memsz = 15; Store(malformed, 0x80, program);
        reject(malformed, "SELF load exceeds memory admitted");
        malformed = elf; embedded = Load<elf_header>(elf, 0);
        embedded.e_shoff = UINT64_MAX; embedded.e_shnum = 1;
        embedded.e_shentsize = sizeof(elf_section_header); Store(malformed, 0, embedded);
        reject(malformed, "raw ELF section offset overflow admitted");
        malformed = elf; program = Load<elf_program_header>(elf, sizeof(elf_header));
        program.p_offset = UINT64_MAX; Store(malformed, sizeof(elf_header), program);
        reject(malformed, "raw ELF load offset overflow admitted");
        malformed = elf; embedded = Load<elf_header>(elf, 0);
        embedded.e_entry = 0x8000; Store(malformed, 0, embedded);
        reject(malformed, "entry outside executable memory admitted");
        write(base / "rom/restart.elf", elf);
        format_options.entry = "restart.elf";
        reject(self.substr(0, 20), "restart bypassed invalid root executable");
        require(accepts(self), error.c_str());
        format_options.entry = "eboot.bin";
        fs::remove(base / "rom/restart.elf");
        write(base / "rom/eboot.bin", elf);
        write(base / "rom/eboot.bin", elf + "changed");
        require(!ValidateContent(options, error), "changed executable admitted");
        write(base / "rom/eboot.bin", elf);
        write(base / "rom/sce_sys/param.json", "{}");
        require(!ValidateContent(options, error), "PS5 content admitted into PS4");
        fs::remove(base / "rom/sce_sys/param.json");
        fs::create_directory_symlink(base / "rom", base / "linked");
        auto linked = options; linked.root = base / "linked";
        require(!ValidateContent(linked, error), "linked root admitted");
        options.known_system = true;
        require(!ValidateContent(options, error), "missing known system modules admitted");
        fs::create_directory(base / "rom/sce_module");
        fs::create_symlink(base / "rom/eboot.bin", base / "rom/sce_module/libc.prx");
        require(!ValidateContent(options, error), "linked module admitted");
        fs::remove(base / "rom/sce_module/libc.prx");
        write(base / "rom/sce_module/libc.prx", elf);
        require(ValidateContent(options, error), error.c_str());
        require(options.startup_modules.size() == 1, "module policy not preserved");
        auto writable_rom = options; writable_rom.data = base / "rom";
        require(!ValidateContent(writable_rom, error), "ROM used for writable runtime data");
        std::vector<std::string> args = {"runtime", "--preflight-managed", options.root.string(),
            "--eboot-sha256", options.eboot_digest, "--param-sha256", options.param_digest,
            "--module-policy", "eboot-only", "--width", "1280", "--height", "720", "--event-token", "test-token"};
        std::vector<char*> argv; for (auto& value : args) argv.push_back(value.data());
        SessionOptions parsed;
        require(ParseCommand(argv.size(), argv.data(), parsed, error), error.c_str());
        require(parsed.preflight && parsed.width == 1280 && !parsed.known_system, "CLI protocol changed");
        auto restarted = options;
        restarted.executable = fs::absolute("runtime"); restarted.event_token = "test-token";
        restarted.entry = "next/app.elf"; restarted.guest_args = {"--literal", "argument with spaces"};
        fs::create_directory(base / "rom/next"); write(base / "rom/next/app.elf", elf);
        require(ValidateEntry(restarted, restarted.entry, error), error.c_str());
        require(!ValidateEntry(restarted, "../data/outside", error), "restart escaped ROM root");
        require(!ValidateEntry(restarted, fs::absolute("outside"), error), "absolute restart admitted");
        auto restart_args = ManagedArguments(restarted);
        std::vector<char*> restart_argv; for (auto& value : restart_args) restart_argv.push_back(value.data());
        SessionOptions restart_parsed;
        require(ParseCommand(restart_argv.size(), restart_argv.data(), restart_parsed, error), error.c_str());
        require(restart_parsed.entry == restarted.entry && restart_parsed.guest_args == restarted.guest_args &&
                restart_parsed.data == restarted.data && restart_parsed.known_system, "restart lost managed configuration");
        restarted.guest_args.assign(34, "argument");
        auto overflow_args = ManagedArguments(restarted);
        std::vector<char*> overflow_argv; for (auto& value : overflow_args) overflow_argv.push_back(value.data());
        SessionOptions overflow_parsed;
        require(!ParseCommand(overflow_argv.size(), overflow_argv.data(), overflow_parsed, error),
                "arguments silently truncated by the guest entry ABI");
        fs::create_directory_symlink(base / "data", base / "rom/escape");
        require(!PathWithinRoot(base / "rom", base / "rom/escape/new"), "symlink escaped mount");
        require(PathWithinRoot(base / "rom", base / "rom/new/file"), "new confined path rejected");
        require(!PathWithinRoot(base / "rom", base / "rom-sibling/file"), "sibling prefix admitted");
        require(!ValidateEntry(restarted, "escape/app.elf", error), "linked restart path admitted");
        // Optional read-only validation of an externally supplied ROM directory.
        // No content from that directory is written or executed by this test.
        if (argc == 2) {
            SessionOptions external;
            external.root = fs::absolute(fs::path{external_arguments[1]});
            external.data = base / "data";
            require(ValidateContent(external, error), error.c_str());
            std::cout << "External content admission PASS\n";
        }
        args[13] = "--width"; argv[13] = args[13].data();
        require(!ParseCommand(argv.size(), argv.data(), parsed, error), "duplicate option admitted");
        std::cout << "Gen4 admission contract PASS\n";
    } catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
}
