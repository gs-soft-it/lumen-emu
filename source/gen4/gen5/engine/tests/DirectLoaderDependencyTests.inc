// SPDX-License-Identifier: GPL-2.0-only
// Real metadata selection, relocation and FEX-executed module initialization.
void WriteDependencyElf(const std::filesystem::path& path, const std::string& module,
                        const std::vector<std::string>& needed, const std::string& imported,
                        bool executable, uint64_t trace, unsigned digit) {
    using namespace Loader;
    std::vector<uint8_t> image(2 * Page);
    Elf64_Ehdr header {};
    std::memcpy(header.e_ident, "\x7f" "ELF", 4);
    header.e_ident[EI_CLASS] = ELFCLASS64; header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT; header.e_ident[EI_OSABI] = ELFOSABI_FREEBSD;
    header.e_ident[EI_ABIVERSION] = 2;
    header.e_type = executable ? ET_DYNEXEC : ET_DYNAMIC;
    header.e_machine = EM_X86_64; header.e_version = EV_CURRENT; header.e_entry = 0x200;
    header.e_phoff = sizeof(header); header.e_ehsize = sizeof(header);
    header.e_phentsize = sizeof(Elf64_Phdr); header.e_phnum = 3;
    std::string strings(1, '\0');
    const auto str = [&](const std::string& value) {
        const auto offset = strings.size(); strings += value; strings.push_back('\0'); return offset;
    };
    const auto own_name = str(module);
    const auto exported_symbol = str(module + "Symbol#A#A");
    const auto imported_name = str(imported);
    const auto imported_symbol = str(imported + "Symbol#B#B");
    std::vector<Elf64_Dyn> dynamic;
    const auto add = [&](Elf64_Sxword tag, uint64_t value) {
        Elf64_Dyn d {}; d.d_tag = tag; d.d_un.d_val = value; dynamic.push_back(d);
    };
    for (const auto& name: needed) add(DT_NEEDED, str(name));
    add(DT_OS_MODULE_INFO_1, (uint64_t{1} << 40) | (uint64_t{1} << 32) | own_name);
    add(DT_OS_EXPORT_LIB_1, (uint64_t{1} << 32) | own_name);
    if (!imported.empty()) {
        add(DT_OS_NEEDED_MODULE_1, (uint64_t{1} << 48) | (uint64_t{1} << 40) |
                                   (uint64_t{1} << 32) | imported_name);
        add(DT_OS_IMPORT_LIB_1, (uint64_t{1} << 48) | (uint64_t{1} << 32) | imported_name);
    }
    add(DT_STRTAB, Page + 0x600); add(DT_STRSZ, strings.size());
    add(DT_SYMTAB, Page + 0x900); add(DT_SYMENT, sizeof(Elf64_Sym));
    add(DT_OS_SYMTABSZ, 2 * sizeof(Elf64_Sym));
    add(DT_PLTREL, DT_RELA); add(DT_JMPREL, Page + 0xa00); add(DT_PLTRELSZ, 0);
    add(DT_RELA, Page + 0xa00); add(DT_RELASZ, imported.empty() ? 0 : sizeof(Elf64_Rela));
    add(DT_RELAENT, sizeof(Elf64_Rela));
    if (!executable) { add(DT_INIT, 0x400); add(DT_FINI, 0x480); }
    add(DT_NULL, 0);
    Check(dynamic.size() * sizeof(Elf64_Dyn) <= 0x300 && strings.size() <= 0x300,
          "bounded synthetic dependency metadata");
    Elf64_Phdr segments[3] {};
    segments[0].p_type = PT_LOAD; segments[0].p_flags = PF_R | PF_X;
    segments[0].p_filesz = segments[0].p_memsz = Page; segments[0].p_align = Page;
    segments[1].p_type = PT_LOAD; segments[1].p_flags = PF_R | PF_W;
    segments[1].p_offset = segments[1].p_vaddr = Page;
    segments[1].p_filesz = segments[1].p_memsz = Page; segments[1].p_align = Page;
    segments[2].p_type = PT_DYNAMIC; segments[2].p_flags = PF_R;
    segments[2].p_offset = segments[2].p_vaddr = Page + 0x300;
    segments[2].p_filesz = segments[2].p_memsz = dynamic.size() * sizeof(Elf64_Dyn);
    segments[2].p_align = 8;
    Elf64_Sym symbols[2] {};
    symbols[0].st_name = exported_symbol; symbols[0].st_info = (STB_GLOBAL << 4) | STT_FUNC;
    symbols[0].st_shndx = 1; symbols[0].st_value = 0x200; symbols[0].st_size = 6;
    symbols[1].st_name = imported_symbol; symbols[1].st_info = (STB_GLOBAL << 4) | STT_FUNC;
    const Elf64_Rela relocation {Page + 0x1000, (uint64_t{1} << 32) | R_X86_64_64, 0};
    std::memcpy(image.data(), &header, sizeof(header));
    std::memcpy(image.data() + header.e_phoff, segments, sizeof(segments));
    std::memcpy(image.data() + Page + 0x300, dynamic.data(), dynamic.size() * sizeof(Elf64_Dyn));
    std::memcpy(image.data() + Page + 0x600, strings.data(), strings.size());
    std::memcpy(image.data() + Page + 0x900, symbols, sizeof(symbols));
    std::memcpy(image.data() + Page + 0xa00, &relocation, sizeof(relocation));
    const std::array<uint8_t, 6> function {0xb8, 0x5a, 0, 0, 0, 0xc3};
    std::copy(function.begin(), function.end(), image.begin() + 0x200);
    for (unsigned fini = 0; fini < 2; ++fini) {
        // trace = 10*trace + digit; increment this module's init/fini count.
        std::vector<uint8_t> code {0x48, 0xb8};
        const uint64_t address = trace + fini * 8;
        const auto* bytes = reinterpret_cast<const uint8_t*>(&address);
        code.insert(code.end(), bytes, bytes + 8);
        const std::array<uint8_t, 19> body {0x48, 0x8b, 0x08, 0x48, 0x6b, 0xc9, 10,
            0x48, 0x83, 0xc1, static_cast<uint8_t>(digit), 0x48, 0x89, 0x08,
            0x48, 0xff, 0x40, static_cast<uint8_t>(16 + digit * 16), 0x31};
        code.insert(code.end(), body.begin(), body.end()); code.push_back(0xc0); code.push_back(0xc3);
        std::copy(code.begin(), code.end(), image.begin() + (fini ? 0x480 : 0x400));
    }
    auto* file = std::fopen(path.c_str(), "wb");
    Check(file && std::fwrite(image.data(), 1, image.size(), file) == image.size() &&
          std::fclose(file) == 0, "write declared-dependency ELF");
}

void CheckLoaderDependencies(const std::filesystem::path& temporary) {
    using Mode = Common::VirtualMemory::Mode;
    const auto directory = (temporary / "dependency-fixture").lexically_normal();
    std::filesystem::create_directories(directory / "sce_module");
    const auto trace = Memory::AllocateRuntimeMemory(0, Page, Mode::ReadWrite, "module init trace");
    std::array<uint64_t, 12> counters {};
    Check(trace && Memory::WriteGuestMemory(nullptr, trace, counters.data(), sizeof(counters)),
          "guest module initializer trace");
    WriteDependencyElf(directory / "eboot.bin", "main", {"A.prx", "A.prx"}, "A", true, trace, 0);
    WriteDependencyElf(directory / "A.prx", "A", {"B.prx"}, "B", false, trace, 1);
    WriteDependencyElf(directory / "sce_module/B.prx", "B", {}, "", false, trace, 2);
    WriteDependencyElf(directory / "unused.prx", "unused", {}, "", false, trace, 3);
    Fs::Mount(directory, "/app0");
    auto* linker = Common::Singleton<Loader::RuntimeLinker>::Instance();
    auto* main = linker->LoadProgram(directory / "eboot.bin");
    linker->PreparePrograms();
    auto* a = linker->FindProgramByFileName(directory / "A.prx");
    auto* b = linker->FindProgramByFileName(directory / "sce_module/B.prx");
    Check(a && b && !linker->FindProgramByFileName(directory / "unused.prx") &&
          a->dependency_count == 1 && b->dependency_count == 1,
          "load only declared dependency closure and deduplicate repeated DT_NEEDED");
    uint64_t main_import {}, a_import {};
    Check(Memory::ReadGuestMemory(nullptr, main->base_vaddr + Page + 0x1000, &main_import, 8) &&
          Memory::ReadGuestMemory(nullptr, a->base_vaddr + Page + 0x1000, &a_import, 8) &&
          main_import == a->base_vaddr + 0x200 && a_import == b->base_vaddr + 0x200,
          "strict relocations bind the actual transitive PRX exports");
    Check(Loader::GuestExecution::InitializeRuntime(), "initialize real FEX module callback runtime");
    Kernel::PthreadCreateMainGuestStack();
    linker->StartAllModules();
    Check(Memory::ReadGuestMemory(nullptr, trace, counters.data(), sizeof(counters)) &&
          counters[0] == 21 && counters[4] == 1 && counters[6] == 1 && counters[8] == 0,
          "FEX executes dependency B before A once; undeclared module never initializes");
    int result = -1;
    const auto handle = Kernel::KernelLoadStartModule("/app0/A.prx", 0, nullptr, 0, nullptr, &result);
    Check(handle == a->unique_id && result == 0 && a->load_count == 1,
          "loadstart reuses the already initialized static dependency");
    Check(Kernel::KernelStopUnloadModule(handle, 0, nullptr, 0, nullptr, &result) == 0 &&
          a->load_count == 0 && linker->FindProgramByFileName(directory / "A.prx") == a,
          "releasing explicit reference retains the statically imported module");
    linker->StartAllModules();
    Check(Memory::ReadGuestMemory(nullptr, trace, counters.data(), sizeof(counters)) && counters[0] == 21,
          "repeat startup does not invoke initializers again");
    linker->StopAllModules();
    Check(Memory::ReadGuestMemory(nullptr, trace, counters.data(), sizeof(counters)) &&
          counters[1] == 12 && counters[5] == 1 && counters[7] == 1,
          "FEX executes finalizers in reverse initialization order");
    linker->Clear();
    Fs::Umount("/app0");
    Check(Memory::FreeGuestMemory(trace, Page), "release module initializer trace");
    Check(std::filesystem::remove_all(directory) == 6, "remove dependency fixture files");
}
