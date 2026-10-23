// SPDX-License-Identifier: GPL-2.0-only
#include "lumen/session.h"
#include "common/sha256.h"
#include "core/loader/elf_validation.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>

namespace Lumen::Gen4 {
namespace fs = std::filesystem;
namespace {
bool Digest(std::string& value) {
    if (value.size() != 64) return false;
    for (auto& c : value) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return true;
}
bool Dimension(const std::string& text, unsigned min, unsigned max, unsigned& value) {
    const auto [end, status] = std::from_chars(text.data(), text.data() + text.size(), value);
    return status == std::errc{} && end == text.data() + text.size() && value >= min && value <= max;
}
bool Regular(const fs::path& file) {
    return fs::is_regular_file(fs::symlink_status(file)) && fs::file_size(file) > 0;
}
std::string Hash(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    sha256::SHA256 hash;
    std::array<char, 65536> bytes;
    while (stream.read(bytes.data(), bytes.size()) || stream.gcount() > 0)
        hash.update(bytes.data(), static_cast<size_t>(stream.gcount()));
    if (!stream.eof()) throw std::runtime_error("Cannot read ROM content");
    return sha256::SHA256::Hex(hash.final());
}

void ValidateExecutable(const fs::path& file) {
    using Core::Loader::ContainsFileRange;
    const auto size = fs::file_size(file);
    if (size > static_cast<u64>(std::numeric_limits<std::streamoff>::max()))
        throw std::runtime_error("Executable file is too large");
    std::ifstream binary(file, std::ios::binary);
    const auto read = [&](u64 offset, auto& value) {
        if (!ContainsFileRange(size, offset, sizeof(value)) ||
            !binary.seekg(static_cast<std::streamoff>(offset)) ||
            !binary.read(reinterpret_cast<char*>(&value), sizeof(value)))
            throw std::runtime_error("Executable header is truncated or outside its file");
    };
    self_header self{};
    read(0, self);
    const bool is_self = self.magic == self_header::signature;
    std::vector<self_segment_header> segments;
    u64 elf_offset{};
    if (is_self) {
        if (self.version != 0 || self.mode != 1 || self.endian != 1 ||
            self.attributes != 0x12 || self.category != 1 || self.program_type != 1 ||
            self.segment_count == 0)
            throw std::runtime_error("Unsupported SELF container header");
        elf_offset = sizeof(self_header) + u64{self.segment_count} * sizeof(self_segment_header);
        if (!ContainsFileRange(self.header_size, elf_offset, sizeof(elf_header)) ||
            !ContainsFileRange(self.file_size, self.header_size, self.meta_size) ||
            self.file_size > size)
            throw std::runtime_error("SELF headers exceed the container file");
        segments.resize(self.segment_count);
        for (size_t i = 0; i < segments.size(); ++i) {
            auto& segment = segments[i];
            read(sizeof(self_header) + i * sizeof(segment), segment);
            if (segment.IsEncrypted() || segment.IsCompressed())
                throw std::runtime_error("SELF segments must already be decrypted and decompressed");
            if (!ContainsFileRange(self.file_size, segment.file_offset, segment.file_size) ||
                (segment.file_size != 0 &&
                 segment.file_offset < u64{self.header_size} + self.meta_size))
                throw std::runtime_error("SELF segment exceeds its container data");
        }
    }
    elf_header header{};
    read(elf_offset, header);
    const auto& ident = header.e_ident;
    if (ident.magic[0] != ELFMAG0 || ident.magic[1] != ELFMAG1 ||
        ident.magic[2] != ELFMAG2 || ident.magic[3] != ELFMAG3 ||
        ident.ei_class != ELF_CLASS_64 || ident.ei_data != ELF_DATA_2LSB ||
        ident.ei_version != ELF_VERSION_CURRENT || ident.ei_osabi != ELF_OSABI_FREEBSD ||
        ident.ei_abiversion != ELF_ABI_VERSION_AMDGPU_HSA_V2 ||
        header.e_machine != EM_X86_64 || header.e_version != EV_CURRENT ||
        (header.e_type != ET_SCE_EXEC && header.e_type != ET_SCE_DYNEXEC &&
         header.e_type != ET_SCE_DYNAMIC) || header.e_ehsize != sizeof(elf_header) ||
        header.e_phentsize != sizeof(elf_program_header) || header.e_phnum == 0 ||
        ((header.e_shnum != 0 || header.e_shentsize != 0) &&
         header.e_shentsize != sizeof(elf_section_header)))
        throw std::runtime_error("PS4 launch requires an x86-64 PS4 ELF or decrypted SELF executable");
    const auto header_limit = is_self ? u64{self.header_size} : size;
    const auto table_fits = [&](u64 offset, u64 bytes) {
        return ContainsFileRange(header_limit, elf_offset, offset) &&
               ContainsFileRange(header_limit, elf_offset + offset, bytes);
    };
    if (header.e_phoff < sizeof(elf_header) ||
        !table_fits(header.e_phoff, u64{header.e_phnum} * sizeof(elf_program_header)))
        throw std::runtime_error("ELF program header table exceeds its file");
    // SELF section offsets describe the original ELF, not the container.
    // Runtime loading uses its resident program headers and segment backing.
    if (!is_self && header.e_shnum != 0 &&
        (header.e_shoff < sizeof(elf_header) ||
         !table_fits(header.e_shoff, u64{header.e_shnum} * sizeof(elf_section_header))))
        throw std::runtime_error("ELF section header table exceeds its file");
    std::vector<elf_program_header> programs(header.e_phnum);
    for (size_t i = 0; i < programs.size(); ++i)
        read(elf_offset + header.e_phoff + i * sizeof(elf_program_header), programs[i]);
    if (!Core::Loader::ValidProgramHeaders(programs, size, is_self))
        throw std::runtime_error("ELF program segment exceeds its file or memory range");
    for (const auto& segment : segments) {
        if (segment.IsBlocked() && (segment.GetId() >= programs.size() ||
            segment.file_size != segment.memory_size))
            throw std::runtime_error("SELF backing segment has an invalid program index or size");
    }
    bool executable_entry = false;
    for (const auto& program : programs) {
        if (!ContainsFileRange(std::numeric_limits<u64>::max(), program.p_offset, program.p_filesz))
            throw std::runtime_error("ELF program file range overflows");
        if (program.p_type == PT_LOAD && (program.p_flags & PF_EXEC) &&
            header.e_entry >= program.p_vaddr &&
            header.e_entry - program.p_vaddr < program.p_filesz)
            executable_entry = true;
        if (!is_self || program.p_filesz == 0 ||
            (program.p_type != PT_LOAD && program.p_type != PT_SCE_RELRO &&
             program.p_type != PT_DYNAMIC && program.p_type != PT_SCE_DYNLIBDATA)) continue;
        bool backed = false;
        for (const auto& segment : segments) {
            if (!segment.IsBlocked()) continue;
            const auto& source = programs[segment.GetId()];
            if (program.p_offset < source.p_offset ||
                !ContainsFileRange(source.p_filesz, program.p_offset - source.p_offset,
                                   program.p_filesz)) continue;
            // Match the loader's first containing backing segment.
            backed = ContainsFileRange(segment.file_size, program.p_offset - source.p_offset,
                                       program.p_filesz);
            break;
        }
        if (!backed) throw std::runtime_error("SELF program segment has no complete backing data");
    }
    if (!executable_entry) throw std::runtime_error("ELF entry is outside executable file-backed memory");
}
}
bool ParseCommand(int argc, char** argv, SessionOptions& options, std::string& error) {
    if (argc < 3 || argv == nullptr || (std::string_view(argv[1]) != "--launch-managed" &&
        std::string_view(argv[1]) != "--preflight-managed")) {
        error = "Use --launch-managed <absolute ROM directory> with the dashboard launch options";
        return false;
    }
    options.preflight = std::string_view(argv[1]) == "--preflight-managed";
    options.root = fs::u8path(argv[2]);
    options.executable = fs::absolute(fs::u8path(argv[0]));
    options.data = fs::current_path();
    std::set<std::string> seen;
    for (int i = 3; i < argc; i += 2) {
        if (i + 1 >= argc || (std::string_view(argv[i]) != "--guest-arg" && !seen.insert(argv[i]).second)) {
            error = "Missing value or repeated launch option";
            return false;
        }
        const std::string key(argv[i]), value(argv[i + 1]);
        bool valid = true;
        if (key == "--eboot-sha256") { options.eboot_digest = value; valid = Digest(options.eboot_digest); }
        else if (key == "--param-sha256") { options.param_digest = value; valid = Digest(options.param_digest); }
        else if (key == "--module-policy") {
            valid = value == "eboot-only" || value == "known-system";
            options.known_system = value == "known-system";
        } else if (key == "--width") valid = Dimension(value, 320, 7680, options.width);
        else if (key == "--height") valid = Dimension(value, 240, 4320, options.height);
        else if (key == "--event-token") {
            valid = !value.empty() && value.size() <= 80 && std::all_of(value.begin(), value.end(),
                [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.'; });
            if (valid) options.event_token = value;
        } else if (key == "--data-path") options.data = fs::u8path(value);
        else if (key == "--entry") options.entry = fs::u8path(value);
        else if (key == "--guest-arg") {
            valid = value.size() <= 4096 && options.guest_args.size() < 33;
            if (valid) options.guest_args.push_back(value);
        }
        else valid = false;
        if (!valid) { error = "Invalid launch option: " + key; return false; }
    }
    for (const auto* key : {"--eboot-sha256", "--param-sha256", "--module-policy", "--width", "--height", "--event-token"}) {
        if (!seen.contains(key)) { error = std::string("Missing launch option: ") + key; return false; }
    }
    return true;
}
bool ValidateEntry(const SessionOptions& options, const fs::path& entry, std::string& error) {
    try {
        if (entry.empty() || entry.is_absolute() || std::ranges::any_of(entry, [](const auto& part) { return part == ".."; }))
            throw std::runtime_error("Executable must be relative to the selected ROM root");
        auto file = options.root;
        for (const auto& part : entry) {
            file /= part;
            if (fs::is_symlink(fs::symlink_status(file))) throw std::runtime_error("Linked executable path is not allowed");
        }
        if (!Regular(file)) throw std::runtime_error("Executable is missing");
        ValidateExecutable(file);
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}
std::vector<std::string> ManagedArguments(const SessionOptions& options) {
    auto utf8 = [](const fs::path& path) { const auto text = path.u8string(); return std::string(text.begin(), text.end()); };
    std::vector<std::string> result{utf8(options.executable), "--launch-managed", utf8(options.root),
        "--eboot-sha256", options.eboot_digest, "--param-sha256", options.param_digest,
        "--module-policy", options.known_system ? "known-system" : "eboot-only", "--width", std::to_string(options.width),
        "--height", std::to_string(options.height), "--event-token", options.event_token, "--data-path", utf8(options.data),
        "--entry", utf8(options.entry)};
    for (const auto& argument : options.guest_args) { result.emplace_back("--guest-arg"); result.push_back(argument); }
    return result;
}
bool ValidateContent(SessionOptions& options, std::string& error) {
    try {
        if (!options.root.is_absolute() || !fs::is_directory(fs::symlink_status(options.root)))
            throw std::runtime_error("ROM root must be an absolute, existing directory without a symbolic link");
        options.root = fs::canonical(options.root);
        if (!ValidateEntry(options, options.entry, error)) return false;
        const auto eboot = options.root / "eboot.bin";
        const auto sys = options.root / "sce_sys";
        const auto param = sys / "param.sfo";
        if (!fs::is_directory(fs::symlink_status(sys)) || !Regular(eboot) || !Regular(param))
            throw std::runtime_error("Missing or linked eboot.bin / sce_sys/param.sfo");
        if (fs::exists(sys / "param.json")) throw std::runtime_error("PS5 content requires the separate PS5 runtime");
        if ((!options.eboot_digest.empty() && Hash(eboot) != options.eboot_digest) ||
            (!options.param_digest.empty() && Hash(param) != options.param_digest))
            throw std::runtime_error("Critical ROM files changed after dashboard validation");
        if (options.entry != fs::path{"eboot.bin"} && !ValidateEntry(options, "eboot.bin", error))
            return false;
        std::array<unsigned char, 20> header{};
        std::ifstream metadata(param, std::ios::binary);
        if (!metadata.read(reinterpret_cast<char*>(header.data()), 20) ||
            header[0] != 0 || header[1] != 'P' || header[2] != 'S' || header[3] != 'F')
            throw std::runtime_error("Invalid PS4 param.sfo");
        options.startup_modules.clear();
        if (options.known_system) {
            const auto modules = options.root / "sce_module";
            if (!fs::is_directory(fs::symlink_status(modules)))
                throw std::runtime_error("Missing or linked sce_module directory");
            for (const auto* name : {"libc.prx", "libSceFios2.prx"}) {
                const auto path = modules / name;
                if (fs::exists(path) && Regular(path)) options.startup_modules.push_back(path);
            }
            if (options.startup_modules.empty()) throw std::runtime_error("No supported modules for known-system policy");
        }
        if (!options.data.is_absolute() || !fs::is_directory(fs::symlink_status(options.data)))
            throw std::runtime_error("Runtime data must be an absolute existing directory without a symbolic link");
        options.data = fs::canonical(options.data);
        if (options.data == options.root || options.data.string().starts_with(options.root.string() + fs::path::preferred_separator))
            throw std::runtime_error("Runtime data must be outside the ROM root");
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}
void EmitEvent(const SessionOptions& options, const char* state) {
    if (options.event_token.empty()) return;
    std::printf("LUMEN_RUNTIME_EVENT token=%s state=%s\n", options.event_token.c_str(), state);
    std::fflush(stdout);
}
}
