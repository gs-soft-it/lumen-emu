// SPDX-License-Identifier: GPL-2.0-only
// Production NGS2 services with real x86 user-FX callbacks through direct FEX.
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/threads.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/libs.h"
#include "loader/guestExecution.h"
#include "loader/runtimeLinker.h"
#include "loader/timer.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace {
namespace Kernel = Libs::LibKernel;
namespace Memory = Kernel::Memory;
using Mode = Common::VirtualMemory::Mode;
constexpr uint64_t Page = 0x4000;
void Check(bool ok, const char* name) {
    std::printf("DIRECT_NGS2_TEST %s %s\n", ok ? "PASS" : "FAIL", name);
    std::fflush(stdout);
    if (!ok) std::abort();
}
struct Arena {
    uint64_t base = Memory::AllocateRuntimeMemory(0, 1 << 20, Mode::ReadWrite, "NGS2 fixture");
    size_t used = 0;
    void* Bytes(size_t size) {
        used = (used + 15) & ~size_t(15);
        Check(base && used + size < (1 << 20), "guest fixture allocation");
        auto* p = reinterpret_cast<void*>(base + used);
        used += size;
        std::memset(p, 0, size);
        return p;
    }
    template<class T> T* Make() { return static_cast<T*>(Bytes(sizeof(T))); }
};
// Guest ABI mirrors; these describe public records, not native implementation state.
struct SystemOption {
    uint64_t size; char name[64]; uint64_t scheduler[4];
    uint32_t flags, max_grain, grain, sample_rate, max_channels, reserved[5];
};
struct RackOption {
    uint64_t size; char name[64];
    uint32_t flags, max_grain, max_voices, max_delay, max_matrices, max_ports,
             max_channels, max_output_channels, reserved[18];
};
struct ModuleInfo {
    uint64_t option;
    uint32_t module_id, source, extra, destination, state_offset, state_size, reserved[2];
};
struct CustomRackOption {
    RackOption common;
    uint32_t state_size, num_buffers, num_modules, reserved;
    ModuleInfo modules[24];
    uint64_t ports[16];
    uint32_t sampler[6];
};
struct MasterRackOption { RackOption common; uint32_t channels, peak_blocks; };
struct FxOption {
    uint32_t size, padding;
    uint64_t setup, cleanup, control, process, common_size, param_size, work_size, user_data;
};
struct BufferInfo { uint64_t buffer, size, reserved[5], user_data; };
struct Header { uint16_t size; int16_t next; uint32_t id; };
struct Format { Header h; uint32_t type, channels, sample_rate, config, offset, margin; };
struct Block { uint64_t offset, size; uint32_t repeats, skip, samples, reserved; uint64_t user_data; };
struct Blocks { Header h; uint64_t data; uint32_t flags, count; uint64_t blocks; };
struct Param { Header h; uint64_t data, size; };
struct Value { Header h; uint32_t value; };
struct Patch { Header h; uint32_t port, input; uint64_t destination; };
struct Render { uint64_t buffer, size; uint32_t type, channels; };
struct Observation {
    uint64_t setup_context, common, param, work;
    uint64_t process_context, process_common, process_param, process_work, state, channels, channel0, channel1;
    uint64_t cleanup_context, cleanup_common, cleanup_param, cleanup_work;
    uint32_t setups, processes, cleanups, retained_param, flags;
    uint32_t final_common, final_work, source_sample;
};
static_assert(sizeof(SystemOption) == 144 && sizeof(RackOption) == 176);
static_assert(sizeof(ModuleInfo) == 40 && sizeof(CustomRackOption) == 1304);
static_assert(sizeof(FxOption) == 72 && sizeof(BufferInfo) == 64);
static_assert(sizeof(Format) == 32 && sizeof(Block) == 40 && sizeof(Blocks) == 32);

struct Code {
    std::vector<uint8_t> bytes;
    void Op(std::initializer_list<uint8_t> data) { bytes.insert(bytes.end(), data); }
    void Dword(uint32_t n) {
        const auto* p = reinterpret_cast<const uint8_t*>(&n); bytes.insert(bytes.end(), p, p + 4);
    }
    void Rex(bool wide, unsigned reg, unsigned base) {
        const uint8_t rex = 0x40 | (wide ? 8 : 0) | (reg >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0);
        if (rex != 0x40) Op({rex});
    }
    void Load(unsigned reg, unsigned base, uint32_t offset, bool wide = true) {
        Rex(wide, reg, base); Op({0x8b, static_cast<uint8_t>(0x80 | (reg & 7) * 8 | (base & 7))}); Dword(offset);
    }
    void Store(unsigned base, uint32_t offset, unsigned reg, bool wide = true) {
        Rex(wide, reg, base); Op({0x89, static_cast<uint8_t>(0x80 | (reg & 7) * 8 | (base & 7))}); Dword(offset);
    }
    void Set(unsigned base, uint32_t offset, uint32_t value) {
        Rex(false, 0, base); Op({0xc7, static_cast<uint8_t>(0x80 | (base & 7))}); Dword(offset); Dword(value);
    }
    void Increment(unsigned base, uint32_t offset) {
        Rex(false, 0, base); Op({0x83, static_cast<uint8_t>(0x80 | (base & 7))}); Dword(offset); Op({1});
    }
    void Record(uint32_t context_offset, uint32_t observation_offset) {
        Load(0, 7, context_offset); Store(8, observation_offset, 0);
    }
    uint64_t Publish() {
        Op({0x31,0xc0,0xc3});
        const auto p = Memory::AllocateProgramMemory(0, Page, Mode::ReadWrite, "NGS2 x86 callback");
        Check(p && bytes.size() < Page, "x86 callback allocation");
        std::memcpy(reinterpret_cast<void*>(p), bytes.data(), bytes.size());
        Memory::SetProgramMemoryProtection(p, Page, Mode::ExecuteRead);
        return p;
    }
};
#define FIELD(name) static_cast<uint32_t>(offsetof(Observation, name))
uint64_t SetupCode(bool empty = false) {
    Code c; c.Load(8,7,24); c.Store(8,FIELD(setup_context),7);
    c.Record(0,FIELD(common)); c.Record(8,FIELD(param)); c.Record(16,FIELD(work));
    c.Increment(8,FIELD(setups));
    if (!empty) {
        c.Load(0,7,0); c.Set(0,0,11);
        c.Load(1,7,8); c.Set(1,0,22);
        c.Load(0,7,16); c.Set(0,0,33); c.Store(0,8,1);
    }
    return c.Publish();
}
uint64_t ProcessCode() {
    Code c; c.Load(8,7,40); c.Store(8,FIELD(process_context),7);
    c.Record(8,FIELD(process_common)); c.Record(16,FIELD(process_param));
    c.Record(24,FIELD(process_work)); c.Record(32,FIELD(state)); c.Record(0,FIELD(channels));
    c.Increment(8,FIELD(processes));
    c.Load(0,7,48,false); c.Store(8,FIELD(flags),0,false);
    c.Load(0,7,8); c.Increment(0,0);
    c.Load(0,7,32); c.Increment(0,0);
    c.Load(0,7,24); c.Increment(0,0); c.Load(1,0,8); c.Load(2,1,0,false);
    c.Store(8,FIELD(retained_param),2,false);
    c.Load(0,7,0); c.Load(1,0,0); c.Store(8,FIELD(channel0),1);
    c.Load(2,1,0,false); c.Store(8,FIELD(source_sample),2,false);
    // movss xmm0,[rcx]; mov eax,1.0f; movd xmm1,eax; addss xmm0,xmm1; movss [rcx],xmm0.
    c.Op({0xf3,0x0f,0x10,0x01,0xb8,0,0,0x80,0x3f,0x66,0x0f,0x6e,0xc8,
          0xf3,0x0f,0x58,0xc1,0xf3,0x0f,0x11,0x01});
    c.Load(0,7,0); c.Load(1,0,8); c.Store(8,FIELD(channel1),1); c.Set(1,12,0x3f000000);
    return c.Publish();
}
uint64_t CleanupCode(bool empty = false) {
    Code c; c.Load(8,7,24); c.Store(8,FIELD(cleanup_context),7);
    c.Record(0,FIELD(cleanup_common)); c.Record(8,FIELD(cleanup_param)); c.Record(16,FIELD(cleanup_work));
    c.Increment(8,FIELD(cleanups));
    if (!empty) {
        c.Load(0,7,0); c.Load(1,0,0,false); c.Store(8,FIELD(final_common),1,false);
        c.Load(0,7,16); c.Load(1,0,0,false); c.Store(8,FIELD(final_work),1,false);
    }
    return c.Publish();
}
#undef FIELD
uint64_t Function(const char* nid) {
    const auto* sym = Common::Singleton<Loader::RuntimeLinker>::Instance()->Symbols()->FindByNid(nid,Loader::SymbolType::Func);
    Check(sym != nullptr, "production service NID"); return sym->vaddr;
}
uint64_t Call(const char* nid, std::initializer_list<uint64_t> args) {
    uint64_t result = ~uint64_t{};
    Check(Loader::GuestExecution::Invoke(Function(nid),args.begin(),args.size(),&result), "translated service returns");
    Check(static_cast<int32_t>(result) == 0, "production service succeeds"); return result;
}
uint64_t Address(const void* p) { return reinterpret_cast<uint64_t>(p); }
bool Owned(uint64_t address) {
    return address != 0 && Memory::QueryGuestAccess(address, 4, Mode::ReadWrite);
}
void Control(Arena& arena, uint64_t voice, const void* value, size_t size) {
    void* p = arena.Bytes(size); std::memcpy(p,value,size);
    Call("uu94irFOGpA",{voice,Address(p)});
}
}

int main() {
    Common::InitializeThreads(); Config::Initialize();
    Config::ConfigOptions config; config.printf_direction = Config::LogDirection::Silent;
    Config::Load(config); Log::Initialize(); Loader::Timer::Start();
    Kernel::Initialize(); Memory::Initialize(); Kernel::PthreadInitSelfForMainThread();
    Kernel::PthreadCreateMainGuestStack();
    Check(Loader::GuestExecution::InitializeRuntime(), "production direct bootstrap");
    auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance(); Libs::InitAll(rt->Symbols());
    Check(Loader::GuestExecution::PrepareRuntime(), "typed service registry");
    Arena arena;
    auto* system = arena.Make<SystemOption>(); Call("AQkj7C0f3PY",{Address(system)});
    system->grain = 4;
    auto* system_buffer = arena.Make<BufferInfo>();
    Call("pgFAiLR5qT4",{Address(system),Address(system_buffer)});
    system_buffer->buffer = Address(arena.Bytes(system_buffer->size));
    auto* system_handle = arena.Make<uint64_t>();
    Call("koBbCMvOKWw",{Address(system),Address(system_buffer),Address(system_handle)});
    auto* observation = arena.Make<Observation>();
    auto* fx = arena.Make<FxOption>();
    *fx = {sizeof(FxOption),0,SetupCode(),CleanupCode(),0,ProcessCode(),32,32,32,Address(observation)};
    auto* custom = arena.Make<CustomRackOption>();
    custom->common.size = sizeof(*custom); custom->common.max_grain = 4;
    custom->common.max_voices = 1; custom->common.max_ports = 1;
    custom->common.max_channels = custom->common.max_output_channels = 2;
    custom->num_buffers = 1; custom->num_modules = 1;
    custom->modules[0] = {Address(fx),0x1f,0,0,0,0,32,{}};
    custom->sampler[0] = 2; custom->sampler[1] = 1;
    auto MakeRack = [&](uint32_t id, void* options) {
        auto* info = arena.Make<BufferInfo>(); auto* handle = arena.Make<uint64_t>();
        Call("0eFLVCfWVds",{id,Address(options),Address(info)});
        info->buffer = Address(arena.Bytes(info->size));
        Call("cLV4aiT9JpA",{*system_handle,id,Address(options),Address(info),Address(handle)});
        return *handle;
    };
    auto Voice = [&](uint64_t rack) {
        auto* voice = arena.Make<uint64_t>(); Call("MwmHz8pAdAo",{rack,0,Address(voice)}); return *voice;
    };
    const auto source_rack = MakeRack(0x4001,custom), source = Voice(source_rack);
    Check(observation->setups == 1 && Owned(observation->common) && Owned(observation->param) && Owned(observation->work),
          "setup receives persistent guest-owned common parameter and work buffers");
    Check(*reinterpret_cast<uint32_t*>(observation->common) == 11 &&
          *reinterpret_cast<uint32_t*>(observation->work) == 33, "guest setup mutations persist");
    auto* master = arena.Make<MasterRackOption>(); master->common.size = sizeof(*master);
    master->common.max_voices = 1; master->common.max_grain = 4; master->channels = 2;
    const auto master_rack = MakeRack(0x3000,master), output_voice = Voice(master_rack);
    const Value master_channels {{sizeof(Value),0,0x30000000},2};
    Control(arena,output_voice,&master_channels,sizeof(master_channels));
    const Format format {{sizeof(Format),0,0x40010000},0x12,2,48000,0,0,0};
    Control(arena,source,&format,sizeof(format));
    const Patch patch {{sizeof(Patch),0,5},0,0,output_voice}; Control(arena,source,&patch,sizeof(patch));
    auto* pcm = static_cast<int16_t*>(arena.Bytes(32));
    for (unsigned i=0;i<16;++i) pcm[i] = i % 2 ? -8192 : 8192;
    auto* block = arena.Make<Block>(); *block = {0,32,0,0,8,0,0};
    const Blocks blocks {{sizeof(Blocks),0,0x40010001},Address(pcm),1,1,Address(block)};
    Control(arena,source,&blocks,sizeof(blocks));
    auto* parameter = static_cast<uint32_t*>(arena.Bytes(32)); *parameter = 44;
    const Param param {{sizeof(Param),0,0x40001f00},Address(parameter),32};
    Control(arena,source,&param,sizeof(param));
    const Value play {{sizeof(Value),0,6},1};
    Control(arena,source,&play,sizeof(play)); Control(arena,output_voice,&play,sizeof(play));
    auto* output = static_cast<float*>(arena.Bytes(8 * sizeof(float)));
    auto* render = arena.Make<Render>(); *render = {Address(output),8*sizeof(float),0x18,2};
    for (unsigned pass=0;pass<2;++pass) {
        Call("i0VnXM-C9fc",{*system_handle,Address(render),1});
        const std::array<float,8> expected {1.25f,-0.25f,0.25f,-0.25f,0.25f,-0.25f,0.25f,0.5f};
        Check(std::memcmp(output,expected.data(),sizeof(expected)) == 0, "guest PCM changes reach production mastering output");
        Check(observation->processes == pass+1 && observation->retained_param == 44 &&
              observation->process_common == observation->common && observation->process_param == observation->param &&
              observation->process_work == observation->work && Owned(observation->state) &&
              *reinterpret_cast<uint32_t*>(observation->state) == pass+1,
              "process retains pointers and shares common work parameter and state contents");
        Check(observation->source_sample == 0x3e800000 && observation->channel1-observation->channel0 == 4*sizeof(float) &&
              observation->flags == (pass == 0 ? 3u : 0u), "deep channel graph input and callback flags");
    }
    Call("lCqD7oycmIM",{source_rack,0});
    Check(observation->cleanups == 1 && observation->cleanup_common == observation->common &&
          observation->cleanup_param == observation->param && observation->cleanup_work == observation->work &&
          observation->final_common == 13 && observation->final_work == 35,
          "cleanup observes same buffers and final guest mutations");
    Check(!Owned(observation->common) && !Owned(observation->param) && !Owned(observation->work) && !Owned(observation->state),
          "rack destruction releases persistent guest mappings");
    auto* empty_observation = arena.Make<Observation>();
    *fx = {sizeof(FxOption),0,SetupCode(true),CleanupCode(true),0,0,0,0,0,Address(empty_observation)};
    custom->modules[0].state_size = 0;
    const auto empty_rack = MakeRack(0x4001,custom);
    Call("lCqD7oycmIM",{empty_rack,0});
    Check(empty_observation->setups == 1 && empty_observation->cleanups == 1 &&
          empty_observation->common == 0 && empty_observation->param == 0 && empty_observation->work == 0 &&
          empty_observation->cleanup_common == 0 && empty_observation->cleanup_param == 0 && empty_observation->cleanup_work == 0,
          "zero-sized user FX buffers remain null through setup and cleanup");
    Call("lCqD7oycmIM",{master_rack,0});
    Call("u-WrYDaJA3k",{*system_handle,0});
    std::puts("PS5_DIRECT_NGS2_V1 setup=1 process=2 cleanup=1 deep_buffers=1 retained_pointers=1 pcm_copyback=1 zero_buffers=1 benchmark=0");
}
