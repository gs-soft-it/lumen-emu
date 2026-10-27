// SPDX-License-Identifier: GPL-2.0-only
// Included by the direct engine I/O fixture after its common initialization.
void CheckLoaderSentinel(const std::filesystem::path& directory) {
    using namespace Loader;
    using Mode = Common::VirtualMemory::Mode;
    constexpr uint64_t Preferred = 0x840000000;
    constexpr uint64_t Canary = 0x87d42e1305c9ab6f;

    // Occupy the historical loader hint if it belongs to the guest reservation.
    // On Darwin the host may already own it; never replace that host mapping.
    const auto collision = Memory::AllocateRuntimeMemory(Preferred, 4096, Mode::ReadWrite,
                                                         "sentinel preferred collision", true);
    Check(collision == 0 || collision == Preferred, "fixed collision fixture cannot relocate");
    if (collision) Check(Memory::WriteGuestMemory(nullptr, collision, &Canary, sizeof(Canary)),
                         "initialize preferred-address collision canary");

    // Also prove a deliberate, readable collision independently of host ASLR.
    const auto occupied = Memory::AllocateRuntimeMemory(0, 4096, Mode::ReadWrite,
                                                       "sentinel allocator collision");
    Check(occupied != 0 && Memory::WriteGuestMemory(nullptr, occupied, &Canary, sizeof(Canary)),
          "allocate readable collision fixture");
    const auto relocated = Memory::AllocateRuntimeMemory(occupied, 4096, Mode::NoAccess,
                                                        "sentinel allocator fallback");
    uint64_t observed {};
    Check(relocated != 0 && relocated != occupied &&
          Memory::ReadGuestMemory(nullptr, occupied, &observed, sizeof(observed)) && observed == Canary,
          "relocatable allocation preserves occupied bytes and permissions");
    Check(Memory::FreeGuestMemory(relocated, 4096) && Memory::FreeGuestMemory(occupied, 4096),
          "release rounded allocation collision fixtures");

    // An ELF containing two unresolved weak objects exercises the production
    // loader and relocation consumer, without an exported test-only sentinel API.
    std::vector<uint8_t> image(0x1000);
    Elf64_Ehdr header {};
    std::memcpy(header.e_ident, "\x7f" "ELF", 4);
    header.e_ident[EI_CLASS] = ELFCLASS64;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_ident[EI_OSABI] = ELFOSABI_FREEBSD;
    header.e_ident[EI_ABIVERSION] = 2;
    header.e_type = ET_DYNAMIC;
    header.e_machine = EM_X86_64;
    header.e_version = EV_CURRENT;
    header.e_entry = 0x200;
    header.e_phoff = sizeof(header);
    header.e_ehsize = sizeof(header);
    header.e_phentsize = sizeof(Elf64_Phdr);
    header.e_phnum = 2;
    constexpr char Name[] = "missing-fixture-object";
    std::vector<Elf64_Dyn> dynamic;
    const auto add = [&](Elf64_Sxword tag, uint64_t value) {
        Elf64_Dyn entry {}; entry.d_tag = tag; entry.d_un.d_val = value; dynamic.push_back(entry);
    };
    add(DT_STRTAB, 0x600); add(DT_STRSZ, sizeof(Name)); add(DT_SYMTAB, 0x680);
    add(DT_SYMENT, sizeof(Elf64_Sym)); add(DT_OS_SYMTABSZ, sizeof(Elf64_Sym));
    add(DT_PLTREL, DT_RELA); add(DT_JMPREL, 0x700); add(DT_PLTRELSZ, 0);
    add(DT_RELA, 0x700); add(DT_RELASZ, 2 * sizeof(Elf64_Rela));
    add(DT_RELAENT, sizeof(Elf64_Rela)); add(DT_NULL, 0);
    Elf64_Phdr segments[2] {};
    segments[0].p_type = PT_LOAD; segments[0].p_flags = PF_R | PF_W;
    segments[0].p_filesz = image.size(); segments[0].p_memsz = Page; segments[0].p_align = Page;
    segments[1].p_type = PT_DYNAMIC; segments[1].p_flags = PF_R;
    segments[1].p_offset = segments[1].p_vaddr = 0x300;
    segments[1].p_filesz = segments[1].p_memsz = dynamic.size() * sizeof(Elf64_Dyn);
    segments[1].p_align = 8;
    Elf64_Sym symbol {};
    symbol.st_info = (STB_WEAK << 4) | STT_OBJECT;
    const Elf64_Rela relocations[2] {{0x780, R_X86_64_64, 0}, {0x788, R_X86_64_64, 0}};
    std::memcpy(image.data(), &header, sizeof(header));
    std::memcpy(image.data() + header.e_phoff, segments, sizeof(segments));
    std::memcpy(image.data() + 0x300, dynamic.data(), dynamic.size() * sizeof(Elf64_Dyn));
    std::memcpy(image.data() + 0x600, Name, sizeof(Name));
    std::memcpy(image.data() + 0x680, &symbol, sizeof(symbol));
    std::memcpy(image.data() + 0x700, relocations, sizeof(relocations));
    const auto path = directory / "sentinel.elf";
    auto* file = std::fopen(path.c_str(), "wb");
    Check(file && std::fwrite(image.data(), 1, image.size(), file) == image.size() &&
          std::fclose(file) == 0, "write original-format weak-object fixture");
    {
        RuntimeLinker linker;
        auto* program = linker.LoadProgram(path);
        Check(program != nullptr, "production loader accepts weak-object fixture");
        linker.RelocateProgram(program);
        std::array<uint64_t, 2> values {};
        Check(Memory::ReadGuestMemory(nullptr, program->base_vaddr + 0x780, values.data(), sizeof(values)) &&
              values[0] != 0 && values[0] != Preferred && values[0] == values[1],
              "both weak objects receive the actual relocated sentinel");
        const auto sentinel = values[0];
        Check(!Memory::QueryGuestAccess(sentinel, 1, Mode::Read) &&
              !Memory::QueryGuestAccess(sentinel, 1, Mode::Write) &&
              !Memory::QueryGuestAccess(sentinel, 1, Mode::Execute), "sentinel denies all guest access");
        mach_vm_address_t native = 0;
        Check(Memory::TranslateGuestMemory(sentinel, Page, &native), "translate complete rounded sentinel page");
        const auto expected_native = native;
        mach_vm_size_t size {};
        vm_region_basic_info_data_64_t info {};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        const auto result = mach_vm_region(mach_task_self(), &native, &size, VM_REGION_BASIC_INFO_64,
            reinterpret_cast<vm_region_info_t>(&info), &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        Check(result == KERN_SUCCESS && native <= expected_native &&
              expected_native + Page <= native + size && info.protection == VM_PROT_NONE,
              "all 16KiB of the sentinel are natively inaccessible");
        linker.RelocateProgram(program);
        Check(Memory::ReadGuestMemory(nullptr, program->base_vaddr + 0x780, values.data(), sizeof(values)) &&
              values[0] == sentinel && values[1] == sentinel, "repeat relocation reuses the sentinel");
    } // Production Clear releases the actual sentinel and the loaded image.
    Check(std::filesystem::remove(path), "remove synthetic weak-object ELF");
    if (collision) {
        Check(Memory::ReadGuestMemory(nullptr, collision, &observed, sizeof(observed)) && observed == Canary,
              "loader relocation preserves preferred-address collision");
        Check(Memory::FreeGuestMemory(collision, 4096), "release preferred-address collision");
    }
}
