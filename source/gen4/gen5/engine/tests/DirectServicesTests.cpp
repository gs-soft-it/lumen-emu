// SPDX-License-Identifier: GPL-2.0-only
// Exercises the production ARM64 service adapter using synthetic x86 guests.
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/threads.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/guestCallback.h"
#include "loader/guestExecution.h"
#include "loader/elf.h"
#include "loader/hostCall.h"
#include "loader/hostCallAggregates.h"
#include "loader/hostCallShaderRegister.h"
#include "loader/runtimeLinker.h"
#include "loader/timer.h"
#include "DirectFexRuntime.h"
#include "DirectPerformanceOptions.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {
namespace Kernel = Libs::LibKernel;
namespace Memory = Kernel::Memory;
namespace Direct = Lumen::Ps5::DirectFex;
using Mode = Common::VirtualMemory::Mode;
constexpr uint64_t Page = 0x4000;
std::atomic<bool> ready {};
std::atomic<bool> reacquired {};
Kernel::PthreadMutex mutex {};
Kernel::PthreadCond cond {};
void Check(bool ok, const char* name) {
    std::printf("DIRECT_SERVICES_TEST %s %s\n", ok ? "PASS" : "FAIL", name);
    std::fflush(stdout);
    if (!ok) std::abort();
}
void Ready() { ready.store(true, std::memory_order_release); }
uint64_t Sum8(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
              uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    return a + b + c + d + e + f + g + h;
}
double FloatMix(uint64_t a, double b, uint64_t c, float d) { return a + b + c + d; }
Libs::Graphics::SizeAlign SizeAlignReturn() { return {0x1122334455667788ULL, 0x1000}; }
Libs::Graphics::ShaderRegister ShaderRegisterRoundtrip(Libs::Graphics::ShaderRegister reg) {
    return {reg.offset + 1, reg.value + 2};
}
int PositionArguments(Libs::Audio::AudioOut2::AudioOut2SpeakerArrayHandle handle,
    Libs::Audio::AudioOut2::AudioOut2Position position, float spread, float* coefficients,
    uint32_t count, uint8_t height, float downmix) {
    if (handle != reinterpret_cast<void*>(0x55) || position.x != 1 || position.y != 2 ||
        position.z != 3 || spread != 4 || count != 3 || height != 1 || downmix != 5) return -99;
    coefficients[0] = 9;
    return 77;
}
uint64_t Callback(uint64_t entry, uint64_t argument) {
    const auto target = reinterpret_cast<uint64_t>(&Callback);
    const auto* binding = Direct::ActiveHostTargetBinding(target);
    Check((binding != nullptr) == Lumen::Ps5::DirectPerformance::Get().hle_indexed_dispatch,
          "translated callback receives its typed binding only in indexed mode");
    const auto result = Loader::GuestCallback::Call(reinterpret_cast<uint64_t(*)(uint64_t)>(entry), argument);
    Check(Direct::ActiveHostTargetBinding(target) == binding, "translated nested HLE restores parent binding");
    return result;
}
uint64_t NestedValue(uint64_t value) { return value + 7; }
void* HeapMalloc(uint64_t size) {
    return Common::Singleton<Loader::RuntimeLinker>::Instance()->ApplicationHeapMalloc(size);
}
void* HeapMemalign(uint64_t alignment, uint64_t size) {
    return Common::Singleton<Loader::RuntimeLinker>::Instance()->ApplicationHeapMemalign(alignment, size);
}
void WaitCond() {
    Check(Kernel::PthreadMutexLock(&mutex) == 0, "condition worker locks mutex");
    Ready();
    try {
        Kernel::PthreadCondWait(&cond, &mutex);
    } catch (const Loader::GuestExecution::ThreadExitSignal&) {
        reacquired = Kernel::PthreadMutexUnlock(&mutex) == 0;
        throw;
    }
}
struct Code {
    std::vector<uint8_t> bytes;
    void Op(std::initializer_list<uint8_t> data) { bytes.insert(bytes.end(), data); }
    void Imm(uint64_t value) {
        const auto* p = reinterpret_cast<const uint8_t*>(&value);
        bytes.insert(bytes.end(), p, p + 8);
    }
    void Mov(unsigned reg, uint64_t value) {
        Op({static_cast<uint8_t>(reg < 8 ? 0x48 : 0x49), static_cast<uint8_t>(0xb8 + (reg & 7))}); Imm(value);
    }
    void Call(uint64_t target) { Mov(0, target); Op({0xff,0xd0}); }
    void Xmm(unsigned reg, uint64_t value) {
        Mov(0, value); Op({0x66,0x48,0x0f,0x6e,static_cast<uint8_t>(0xc0 + reg*8)});
    }
    uint64_t Publish() const {
        const auto address = Memory::AllocateProgramMemory(0, Page, Mode::ReadWrite, "service fixture");
        Check(address && bytes.size() < Page, "guest code allocation");
        std::memcpy(reinterpret_cast<void*>(address), bytes.data(), bytes.size());
        Memory::SetProgramMemoryProtection(address, Page, Mode::ExecuteRead);
        return address;
    }
};
template<auto Fn> uint64_t Import(const char* name) {
    Check(Loader::RegisterHostFunction<Fn>(name), "typed import registration");
    const auto address = Direct::ResolveHostTarget(reinterpret_cast<uint64_t>(Fn));
    Check(address != 0, "guest import veneer");
    return address;
}
uint64_t Invoke(uint64_t entry, std::initializer_list<uint64_t> arguments = {}) {
    uint64_t result {};
    Check(Loader::GuestExecution::Invoke(entry, arguments.begin(), arguments.size(), &result), "translated entry returns");
    return result;
}
void CancelWorker(uint64_t entry, const char* name) {
    ready = false;
    Kernel::Pthread worker {};
    Check(Kernel::PthreadCreate(&worker, nullptr,
          reinterpret_cast<Kernel::pthread_entry_func_t>(entry), nullptr, name) == 0, "guest pthread creation");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ready.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    Check(ready.load(), "worker reaches guest service boundary");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(Kernel::PthreadCancel(worker) == 0, "guest cancellation request");
    void* result {};
    Check(Kernel::PthreadJoin(worker, &result) == 0 && result == reinterpret_cast<void*>(1), name);
}
void LoadDlsymFixture(Loader::RuntimeLinker* rt) {
    // A minimal original-format ELF gives KernelDlsym a real module, without
    // bypassing the loader or introducing a production test-only entry point.
    using namespace Loader;
    std::vector<uint8_t> image(0x1000);
    Elf64_Ehdr header {};
    std::memcpy(header.e_ident,"\x7f" "ELF",4);
    header.e_ident[EI_CLASS] = ELFCLASS64; header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT; header.e_ident[EI_OSABI] = ELFOSABI_FREEBSD;
    header.e_ident[EI_ABIVERSION] = 2;
    header.e_type = ET_DYNAMIC; header.e_machine = EM_X86_64; header.e_version = EV_CURRENT;
    header.e_entry = 0x200; header.e_phoff = sizeof(header); header.e_ehsize = sizeof(header);
    header.e_phentsize = sizeof(Elf64_Phdr); header.e_phnum = 2;
    std::vector<Elf64_Dyn> dynamic;
    const auto add = [&](Elf64_Sxword tag,uint64_t value) {
        Elf64_Dyn entry {}; entry.d_tag = tag; entry.d_un.d_val = value; dynamic.push_back(entry);
    };
    add(DT_STRTAB,0x600); add(DT_STRSZ,1); add(DT_SYMTAB,0x680);
    add(DT_SYMENT,sizeof(Elf64_Sym)); add(DT_OS_SYMTABSZ,sizeof(Elf64_Sym));
    add(DT_PLTREL,DT_RELA); add(DT_JMPREL,0x700); add(DT_PLTRELSZ,0);
    add(DT_RELA,0x700); add(DT_RELASZ,0); add(DT_RELAENT,sizeof(Elf64_Rela)); add(DT_NULL,0);
    Elf64_Phdr segments[2] {};
    segments[0].p_type = PT_LOAD; segments[0].p_flags = PF_R | PF_X;
    segments[0].p_filesz = image.size(); segments[0].p_memsz = Page; segments[0].p_align = Page;
    segments[1].p_type = PT_DYNAMIC; segments[1].p_flags = PF_R;
    segments[1].p_offset = segments[1].p_vaddr = 0x300;
    segments[1].p_filesz = segments[1].p_memsz = dynamic.size() * sizeof(Elf64_Dyn);
    segments[1].p_align = 8;
    std::memcpy(image.data(),&header,sizeof(header));
    std::memcpy(image.data()+header.e_phoff,segments,sizeof(segments));
    std::memcpy(image.data()+0x300,dynamic.data(),dynamic.size()*sizeof(Elf64_Dyn));
    image[0x200] = 0xc3;
#if defined(__ANDROID__)
    std::string temporary_root = "/data/local/tmp";
#else
    const char* configured_tmp = std::getenv("TMPDIR");
    std::string temporary_root = configured_tmp != nullptr && *configured_tmp != '\0'
        ? configured_tmp : "/tmp";
#endif
    std::string directory = temporary_root + "/lumen-direct-services-XXXXXX";
    Check(mkdtemp(directory.data()) != nullptr, "isolated temporary module directory");
    std::string path = directory + "/module-XXXXXX";
    const int fd = mkstemp(path.data());
    Check(fd >= 0,"synthetic module file creation");
    auto* file = fdopen(fd,"wb");
    Check(file && std::fwrite(image.data(),1,image.size(),file) == image.size(),"synthetic module write");
    Check(std::fclose(file) == 0,"synthetic module file close");
    const bool loaded = rt->LoadProgram(path) != nullptr;
    Check(std::remove(path.c_str()) == 0 && rmdir(directory.c_str()) == 0,
          "synthetic module file and isolated directory removal");
    Check(loaded,"production loader accepts synthetic module");
}

void CheckGuestMemoryAccess() {
    const auto base = Memory::AllocateRuntimeMemory(0, 3 * Page, Mode::ReadWrite,
                                                   "guest access fixture");
    Check(base != 0, "guest access fixture allocation");
    std::memset(reinterpret_cast<void*>(base), 0x5a, 3 * Page);
    std::array<uint8_t, 8> bytes {};
    const std::array<uint8_t, 8> replacement {1, 2, 3, 4, 5, 6, 7, 8};
    const auto unchanged = [&] {
        return std::all_of(bytes.begin(), bytes.end(), [](uint8_t value) { return value == 0xa5; });
    };
    const auto denied_read = [&](uint64_t address, size_t size) {
        bytes.fill(0xa5);
        return !Memory::ReadGuestMemory(nullptr, address, bytes.data(), size) && unchanged();
    };
    Check(Memory::QueryGuestAccess(base, 3 * Page, Mode::ReadWrite) &&
          Memory::QueryGuestAccess(base, 3 * Page, Mode::NoAccess) &&
          !Memory::QueryGuestAccess(base, 3 * Page, Mode::Execute) &&
          !Memory::QueryGuestAccess(0, 8, Mode::Read) &&
          !Memory::QueryGuestAccess(base, 0, Mode::Read) &&
          !Memory::QueryGuestAccess(UINT64_MAX - 3, 8, Mode::Read) &&
          !Memory::QueryGuestAccess(base, 8, static_cast<Mode>(8)) &&
          !Memory::QueryGuestAccess(base, 8, static_cast<Mode>(-1)),
          "guest access validates complete rights, zero spans, overflow and invalid modes");
    Check(denied_read(0, bytes.size()) && denied_read(base, 0) &&
          denied_read(UINT64_MAX - 3, bytes.size()) &&
          !Memory::ReadGuestMemory(nullptr, base, nullptr, 8) &&
          !Memory::WriteGuestMemory(nullptr, base, nullptr, 8),
          "invalid guest copies reject before changing the destination");
    Check(Memory::ReadGuestMemory(nullptr, base + Page - 4, bytes.data(), bytes.size()) &&
          std::all_of(bytes.begin(), bytes.end(), [](uint8_t value) { return value == 0x5a; }),
          "guest read crosses a mapped page boundary");
    Check(Memory::ProtectGuestMemory(base + Page, Page, Mode::Read),
          "guest access fixture has adjacent different permissions");
    Check(Memory::QueryGuestAccess(base, 3 * Page, Mode::Read) &&
          !Memory::QueryGuestAccess(base, 3 * Page, Mode::ReadWrite) &&
          Memory::ReadGuestMemory(nullptr, base + Page - 4, bytes.data(), bytes.size()) &&
          !Memory::WriteGuestMemory(nullptr, base + Page - 4, replacement.data(), replacement.size()) &&
          std::all_of(reinterpret_cast<const uint8_t*>(base + Page - 4),
                      reinterpret_cast<const uint8_t*>(base + Page + 4),
                      [](uint8_t value) { return value == 0x5a; }),
          "fragmented read passes while partial write denial preserves every destination byte");
    Check(Memory::ProtectGuestMemory(base + Page, Page, Mode::NoAccess) &&
          !Memory::QueryGuestAccess(base, 3 * Page, Mode::Read) &&
          Memory::QueryGuestAccess(base + Page, Page, Mode::NoAccess) &&
          denied_read(base + Page - 4, bytes.size()) &&
          denied_read(base + Page, bytes.size()),
          "guest protection revocation immediately denies full and crossing reads without copying");
    Check(Memory::ProtectGuestMemory(base + Page, Page, Mode::ReadWrite),
          "guest access permission restoration");

    std::atomic<bool> begin {}, failed {};
    std::thread reader([&] {
        while (!begin.load(std::memory_order_acquire)) std::this_thread::yield();
        for (unsigned i = 0; i < 256; ++i) {
            std::array<uint8_t, 8> result;
            result.fill(0xa5);
            const bool read = Memory::ReadGuestMemory(nullptr, base + Page, result.data(), result.size());
            const uint8_t expected = read ? 0x5a : 0xa5;
            if (!std::all_of(result.begin(), result.end(), [expected](uint8_t value) {
                    return value == expected;
                })) failed.store(true, std::memory_order_relaxed);
        }
    });
    begin.store(true, std::memory_order_release);
    for (unsigned i = 0; i < 128; ++i) {
        if (!Memory::ProtectGuestMemory(base + Page, Page, Mode::NoAccess) ||
            !Memory::ProtectGuestMemory(base + Page, Page, Mode::ReadWrite)) failed = true;
    }
    reader.join();
    Check(!failed.load(), "guest reads remain transactional during concurrent protection changes");

    Check(Memory::FreeGuestMemory(base + Page, Page) &&
          !Memory::QueryGuestAccess(base, 3 * Page, Mode::Read) &&
          !Memory::QueryGuestAccess(base + Page, Page, Mode::NoAccess) &&
          Memory::QueryGuestAccess(base, Page, Mode::ReadWrite) &&
          Memory::QueryGuestAccess(base + 2 * Page, Page, Mode::ReadWrite) &&
          denied_read(base + Page - 4, bytes.size()),
          "unmapped interior gap rejects spanning access and preserves its neighbors");
    Check(Memory::AllocateRuntimeMemory(base + Page, Page, Mode::ReadWrite,
                                        "guest access replacement", true) == base + Page &&
          Memory::QueryGuestAccess(base, 3 * Page, Mode::ReadWrite) &&
          Memory::WriteGuestMemory(nullptr, base + Page - 4, replacement.data(), replacement.size()) &&
          Memory::ReadGuestMemory(nullptr, base + Page - 4, bytes.data(), bytes.size()) && bytes == replacement,
          "remap restores cross-range access and copies actual bytes through the guest view");
    Check(Memory::FreeGuestMemory(base, 3 * Page) && denied_read(base, bytes.size()),
          "guest access fixture retirement revokes reads");

    void* reserved = nullptr;
    Check(Memory::KernelReserveVirtualRange(&reserved, Page, 0, Page) == 0 && reserved != nullptr,
          "guest access reservation fixture");
    const auto reservation = reinterpret_cast<uint64_t>(reserved);
    Check(!Memory::QueryGuestAccess(reservation, Page, Mode::Read) &&
          !Memory::QueryGuestAccess(reservation, Page, Mode::NoAccess) &&
          denied_read(reservation, bytes.size()) && Memory::KernelMunmap(reservation, Page) == 0,
          "reserved uncommitted memory never grants CPU access");
}
}

int main() {
    Common::InitializeThreads();
    Config::Initialize();
    Config::ConfigOptions options;
    options.printf_direction = Config::LogDirection::Silent;
    Config::Load(options);
    Log::Initialize();
    Loader::Timer::Start();
    Kernel::Initialize();
    Memory::Initialize();
    Kernel::PthreadInitSelfForMainThread();
    Kernel::PthreadCreateMainGuestStack();
    Check(Loader::GuestExecution::InitializeRuntime(), "production direct engine bootstrap");
    auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
    Libs::InitAll(rt->Symbols());
    Check(Loader::GuestExecution::PrepareRuntime(), "complete service import registration");
    CheckGuestMemoryAccess();
    const auto self = Kernel::PthreadSelf();
    Check(!Kernel::PthreadHasAnyPendingSignal(self) && !Kernel::PthreadHasAnyPendingSignal(nullptr),
          "signal mask initially empty and null thread denied");
    Kernel::PthreadQueuePendingSignal(self,5);
    Kernel::PthreadQueuePendingSignal(self,30);
    Kernel::PthreadQueuePendingSignal(self,64);
    Check(Kernel::PthreadHasAnyPendingSignal(self) && Kernel::PthreadTakePendingSignal(self,30) &&
          Kernel::PthreadHasAnyPendingSignal(self) && Kernel::PthreadTakePendingSignal(self,5) &&
          !Kernel::PthreadHasAnyPendingSignal(self), "signal mask preserves other bits and rejects invalid signal");
    const auto aggregate_return = Import<SizeAlignReturn>("test INTEGER pair return");
    const auto aggregate_position = Import<PositionArguments>("test SSE pair argument");
    const auto shader_register = Import<ShaderRegisterRoundtrip>("test ShaderRegister INTEGER eightbyte");
    const auto sum = Import<Sum8>("test Sum8");
    const auto callback = Import<Callback>("test callback");
    const auto heap_malloc = Import<HeapMalloc>("test application heap malloc");
    const auto heap_memalign = Import<HeapMemalign>("test application heap memalign");
    const auto float_mix = Import<FloatMix>("test mixed floating arguments");
    const auto mark_ready = Import<Ready>("test ready");
    const auto cancel_type = Import<Kernel::PthreadSetcanceltype>("test cancel type");
    const auto test_cancel = Import<Kernel::PthreadTestcancel>("test cancel point");
    const auto wait_cond = Import<WaitCond>("test cond wait");

    Code eight;
    eight.Op({0x48,0x83,0xec,0x08,0x6a,8,0x6a,7});
    constexpr unsigned args[] {7,6,2,1,8,9};
    for (unsigned i=0;i<6;++i) eight.Mov(args[i],i+1);
    eight.Call(sum);
    eight.Op({0x48,0x83,0xc4,0x18,0xc3});
    Check(Invoke(eight.Publish()) == 36, "SysV INTEGER registers and stack spills");
    Code mixed; mixed.Op({0x48,0x83,0xec,8}); mixed.Mov(7,2); mixed.Mov(6,4);
    mixed.Mov(0,0x3ff4000000000000ULL); mixed.Op({0x66,0x48,0x0f,0x6e,0xc0});
    mixed.Mov(0,0x40600000); mixed.Op({0x66,0x0f,0x6e,0xc8}); mixed.Call(float_mix);
    mixed.Op({0x66,0x48,0x0f,0x7e,0xc0,0x48,0x83,0xc4,8,0xc3});
    Check(Invoke(mixed.Publish()) == 0x4025800000000000ULL,
          "mixed SysV INTEGER SSE arguments and double return");
    const auto nested_value = Import<NestedValue>("test late nested HLE");
    Code child; child.Op({0x48,0x83,0xec,8}); child.Call(nested_value);
    child.Op({0x48,0x83,0xc4,8,0xc3});
    Code parent; parent.Op({0x48,0x83,0xec,8});
    parent.Mov(7,child.Publish()); parent.Mov(6,35); parent.Call(callback);
    parent.Op({0x48,0x83,0xc4,8,0xc3});
    Check(Invoke(parent.Publish()) == 42, "nested guest callback returns through original frame");

    const auto heap_data = Memory::AllocateRuntimeMemory(0,Page,Mode::ReadWrite,"heap callback fixture");
    Code malloc_code; malloc_code.Mov(0,heap_data); malloc_code.Op({0x48,0x01,0xf8,0xc3});
    Code free_code; free_code.Op({0xc3});
    Code align_code; align_code.Mov(0,heap_data);
    align_code.Op({0x48,0x01,0xf0,0x48,0x01,0xd0,0x48,0x89,0x07,0x31,0xc0,0xc3});
    void* heap_api[10] {};
    heap_api[0] = reinterpret_cast<void*>(malloc_code.Publish());
    heap_api[1] = reinterpret_cast<void*>(free_code.Publish());
    heap_api[6] = reinterpret_cast<void*>(align_code.Publish());
    rt->SetApplicationHeapApi(heap_api);
    Check(Invoke(heap_malloc,{0x80}) == heap_data + 0x80,
          "application heap malloc uses translated guest callback");
    Check(Invoke(heap_memalign,{0x100,0x80}) == heap_data + 0x180,
          "application heap memalign uses guest-owned output pointer and copyback");
    LoadDlsymFixture(rt);
    std::strcpy(reinterpret_cast<char*>(heap_data+0x1000),"scriptingGetMem");
    const auto* dlsym = rt->Symbols()->FindByNid("LwG8g3niqwA",Loader::SymbolType::Func);
    Check(dlsym && Invoke(dlsym->vaddr,{0,heap_data+0x1000,heap_data+0x1100}) == 0,
          "production dlsym resolves application allocator fallback");
    const auto scripting_get_mem = *reinterpret_cast<const uint64_t*>(heap_data+0x1100);
    uint64_t veneer_base {}, veneer_size {};
    Check(Direct::QueryOwnedExecutable(scripting_get_mem,&veneer_base,&veneer_size) &&
          Invoke(scripting_get_mem,{0x100,0x80}) == heap_data+0x180,
          "manual dlsym export is a guest RX veneer and calls guest heap callback");
    rt->SetApplicationHeapApi(nullptr);

    const auto aggregate_data = Memory::AllocateRuntimeMemory(0,Page,Mode::ReadWrite,"aggregate fixture");
    auto call_pair = [&](uint64_t target) {
        Code code; code.Op({0x48,0x83,0xec,8}); code.Call(target);
        code.Mov(1,aggregate_data); code.Op({0x48,0x89,0x01,0x48,0x89,0x51,8,0x48,0x83,0xc4,8,0xc3});
        Invoke(code.Publish());
    };
    call_pair(aggregate_return);
    const auto* pair = reinterpret_cast<const uint64_t*>(aggregate_data);
    Check(pair[0] == 0x1122334455667788ULL && pair[1] == 0x1000,
          "16-byte INTEGER aggregate return uses RAX and RDX");
    for (const char* nid : {"U-CnbmeyYaA", "-C2nkoEYOnU"}) {
        const auto* query = rt->Symbols()->FindByNid(nid,Loader::SymbolType::Func);
        Check(query && query->vaddr, "Hmd2 aggregate export resolves to veneer");
        call_pair(query->vaddr);
        Check(pair[0] != 0 && pair[1] != 0 && (pair[1] & (pair[1]-1)) == 0,
              "production Hmd2 aggregate size and alignment return");
    }
    auto call_position = [&](uint64_t target) {
        Code code; code.Op({0x48,0x83,0xec,8});
        code.Mov(7,0x55); code.Mov(6,aggregate_data); code.Mov(2,3); code.Mov(1,1);
        code.Xmm(0,0x400000003f800000ULL); code.Xmm(1,0xdeadbeef40400000ULL);
        code.Xmm(2,0x40800000); code.Xmm(3,0x40a00000); code.Call(target);
        code.Op({0x48,0x83,0xc4,8,0xc3}); return Invoke(code.Publish());
    };
    Check(call_position(aggregate_position) == 77 && *reinterpret_cast<float*>(aggregate_data) == 9,
          "12-byte float aggregate uses two SSE registers independently of GP arguments");
    const auto* coefficients = rt->Symbols()->FindByNid("4BlZurolOAo",Loader::SymbolType::Func);
    Check(coefficients && coefficients->vaddr && call_position(coefficients->vaddr) == 0 &&
          reinterpret_cast<float*>(aggregate_data)[0] == 1 &&
          reinterpret_cast<float*>(aggregate_data)[1] == 1 &&
          reinterpret_cast<float*>(aggregate_data)[2] == 0,
          "production audio aggregate export executes original coefficient service");
    Code shader; shader.Op({0x48,0x83,0xec,8}); shader.Mov(7,0x000000140000000aULL);
    shader.Call(shader_register); shader.Op({0x48,0x83,0xc4,8,0xc3});
    Check(Invoke(shader.Publish()) == 0x000000160000000bULL,
          "verified ShaderRegister INTEGER eightbyte argument and return");

    Loader::Program tls_program;
    tls_program.tls.image_size = 32;
    tls_program.tls.init_image = {1,2,3,4};
    auto* main_tls = Loader::RuntimeLinker::TlsGetAddr(&tls_program);
    Check(main_tls && main_tls[0] == 1 && main_tls[4] == 0 &&
          *reinterpret_cast<uint64_t*>(main_tls + 32) == reinterpret_cast<uint64_t>(main_tls + 32),
          "original TLS template zero fill and self pointer");
    std::atomic<bool> tls_isolated {};
    std::thread tls_thread([&] {
        auto* other = Loader::RuntimeLinker::TlsGetAddr(&tls_program);
        tls_isolated = other && other != main_tls && other[0] == 1;
        Loader::RuntimeLinker::DeleteTls(&tls_program, Common::Thread::GetThreadIdUnique());
    });
    tls_thread.join();
    Check(tls_isolated, "original TLS allocation isolated per native guest thread");
    Loader::RuntimeLinker::DeleteTls(&tls_program, Common::Thread::GetThreadIdUnique());

    const auto* stdout_symbol = rt->Symbols()->FindByNid("2sWzhYqFH4E", Loader::SymbolType::Object);
    const auto* fflush_symbol = rt->Symbols()->FindByNid("MUjC4lbHrK4", Loader::SymbolType::Func);
    Check(stdout_symbol && stdout_symbol->vaddr && fflush_symbol &&
          Invoke(fflush_symbol->vaddr,{stdout_symbol->vaddr}) == 0, "guest stdout identity maps to native fflush");

    const auto data = Memory::AllocateRuntimeMemory(0,Page,Mode::ReadWrite,"signal fixture");
    Code handler; handler.Mov(0,data); handler.Op({0x48,0xff,0x00,0x48,0x8b,0x96,0xf8,0,0,0,0x48,0x89,0x50,8,0xc3});
    const auto* install = rt->Symbols()->FindByNid("WkwEd3N7w0Y", Loader::SymbolType::Func);
    const auto* raise = rt->Symbols()->FindByNid("il03nluKfMk", Loader::SymbolType::Func);
    Check(install && raise && Invoke(install->vaddr,{30,handler.Publish()}) == 0,
          "guest exception callback registration");
    Check(Invoke(raise->vaddr,{reinterpret_cast<uint64_t>(Kernel::PthreadSelf()),30}) == 0 &&
          *reinterpret_cast<uint64_t*>(data) == 1 && *reinterpret_cast<uint64_t*>(data+8) != 0,
          "deferred guest signal callback receives guest stack and returns");
    std::thread signal_publisher([self] { Kernel::PthreadQueuePendingSignal(self,30); });
    signal_publisher.join();
    Check(Kernel::PthreadHasAnyPendingSignal(self), "signal published from peer remains pending");
    Invoke(mark_ready);
    Check(*reinterpret_cast<uint64_t*>(data) == 2 && !Kernel::PthreadHasAnyPendingSignal(self),
          "next HLE boundary delivers peer signal once");
    Invoke(mark_ready);
    Check(*reinterpret_cast<uint64_t*>(data) == 2, "empty HLE boundary does not redeliver signal");

    Code deferred; deferred.Op({0x48,0x83,0xec,8}); deferred.Call(mark_ready);
    const auto loop = deferred.bytes.size(); deferred.Call(test_cancel);
    deferred.Op({0xeb,static_cast<uint8_t>(-static_cast<int>(deferred.bytes.size()+2-loop))});
    CancelWorker(deferred.Publish(),"deferred cancellation joins with guest canceled value");
    Code asynchronous; asynchronous.Op({0x48,0x83,0xec,8});
    asynchronous.Mov(7,2); asynchronous.Mov(6,0); asynchronous.Call(cancel_type);
    asynchronous.Call(mark_ready); asynchronous.Op({0xeb,0xfe});
    const auto asynchronous_entry = asynchronous.Publish();
    CancelWorker(asynchronous_entry,"asynchronous cancellation interrupts pure translated loop");
    Code nested_cancel; nested_cancel.Op({0x48,0x83,0xec,8});
    nested_cancel.Mov(7,asynchronous_entry); nested_cancel.Mov(6,0); nested_cancel.Call(callback);
    nested_cancel.Op({0x48,0x83,0xc4,8,0xc3});
    CancelWorker(nested_cancel.Publish(),"asynchronous cancellation cleans nested guest callback root");

    Check(Kernel::PthreadMutexInit(&mutex,nullptr,"test cond mutex") == 0 &&
          Kernel::PthreadCondInit(&cond,nullptr,"test cond") == 0, "condition fixture initialization");
    Code waiter; waiter.Op({0x48,0x83,0xec,8}); waiter.Call(wait_cond); waiter.Op({0x48,0x83,0xc4,8,0xc3});
    CancelWorker(waiter.Publish(),"condition wait cancellation joins");
    Check(reacquired, "condition cancellation reacquires guest mutex before cleanup");
    Kernel::PthreadCondDestroy(&cond); Kernel::PthreadMutexDestroy(&mutex);
    std::printf("DIRECT_SERVICES_TEST PASS all\n");
    std::fflush(stdout);
    std::quick_exit(0);
}
