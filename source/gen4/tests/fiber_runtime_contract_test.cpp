// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/fex/fex_guest_engine.h"
#include "core/guest_cpu/fex_hle_bridge.h"
#include "core/libraries/fiber/fiber.h"
#include "core/loader/symbols_resolver.h"
#include "core/tls.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)

namespace {
using namespace Core;
using namespace Core::GuestCpu;
using namespace Libraries::Fiber;

struct Mapping {
    const std::size_t size;
    u8* const data;
    explicit Mapping(std::size_t size_)
        : size(size_), data(static_cast<u8*>(mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0))) {
        CHECK(data != MAP_FAILED);
    }
    ~Mapping() { CHECK(munmap(data, size) == 0); }
    std::uintptr_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }
};

struct Data {
    alignas(16) OrbisFiber a{};
    alignas(16) OrbisFiber b{};
    u64 main_output{}, a_output{}, b_output{};
    u16 main_fcw{0x37f}, a_fcw{0x27f}, b_fcw{0xb7f};
    u32 main_mxcsr{0x1f80}, a_mxcsr{0x5fc0}, b_mxcsr{0x3fc0};
    u16 observed_fcw{};
    u32 observed_mxcsr{};
};
Data* data{};
std::size_t observed{};
constexpr std::array<u64, 4> expected_returns{0xaa, 0xbb, 0xcc, 0xdd};
constexpr std::array<u64, 10> expected_phases{0, 10, 1, 11, 20, 2, 12, 3, 21, 4};

class Observe final : public HleCallAdapter {
public:
    HleCallResult Invoke(HleCallFrame& frame) const override {
        const auto phase = frame.gpr[7];
        CHECK(observed < expected_phases.size() && phase == expected_phases[observed++]);
        CHECK(frame.callret_stack_base != 0 && frame.callret_stack_size == 0x400000);
        CHECK(frame.callret_sp >= frame.callret_stack_base);
        CHECK(frame.callret_sp <= frame.callret_stack_base + frame.callret_stack_size);
        CHECK(frame.fcw == data->observed_fcw);
        CHECK(frame.mxcsr == data->observed_mxcsr);
        if (phase < 10) {
            CHECK(frame.fcw == data->main_fcw && frame.mxcsr == data->main_mxcsr);
            CHECK(Core::GetTcbBase()->tcb_fiber == nullptr);
            if (phase) CHECK(data->main_output == expected_returns[phase - 1]);
        } else {
            CHECK(Core::GetTcbBase()->tcb_fiber != nullptr);
            if (phase == 10) {
                CHECK(frame.gpr[12] == 0xa001 && frame.gpr[13] == 11);
                CHECK(frame.fcw == 0x37f && frame.mxcsr == 0x9fc0);
                for (const auto& reg : frame.xmm) CHECK(reg[0] == 0 && reg[1] == 0);
            } else if (phase == 11 || phase == 12) {
                CHECK(frame.fcw == data->a_fcw && frame.mxcsr == data->a_mxcsr);
                CHECK(data->a_output == (phase == 11 ? 22 : 44));
            } else if (phase == 20) {
                CHECK(frame.gpr[12] == 0xb001 && frame.gpr[13] == 33);
                CHECK(frame.fcw == data->a_fcw && frame.mxcsr == data->a_mxcsr);
            } else if (phase == 21) {
                CHECK(frame.fcw == data->b_fcw && frame.mxcsr == data->b_mxcsr);
                CHECK(data->b_output == 55);
            }
        }
        frame.gpr[0] = 0;
        return true;
    }
};

struct Fixture {
    std::vector<GuestExecutionRange> ranges;
    static bool Access(void* opaque, std::uintptr_t address, std::size_t size, bool write) {
        for (const auto& range : static_cast<Fixture*>(opaque)->ranges) {
            if (address >= range.Begin && address - range.Begin < range.Size &&
                size <= range.Size - (address - range.Begin) && (!write || range.Writable)) return true;
        }
        return false;
    }
    static std::optional<GuestExecutionRange> Executable(void* opaque, std::uintptr_t address) {
        for (const auto& range : static_cast<Fixture*>(opaque)->ranges)
            if (range.Executable && address >= range.Begin && address - range.Begin < range.Size)
                return range;
        return std::nullopt;
    }
};

struct Code {
    u8* cursor;
    void Bytes(std::initializer_list<u8> values) {
        for (const auto value : values) *cursor++ = value;
    }
    void Imm64(u64 value) { std::memcpy(cursor, &value, 8); cursor += 8; }
    void Move(unsigned reg, u64 value) {
        Bytes({static_cast<u8>(reg >= 8 ? 0x49 : 0x48), static_cast<u8>(0xb8 + (reg & 7))});
        Imm64(value);
    }
    void Call(u64 address) { Move(0, address); Bytes({0xff, 0xd0}); }
    void Fp(const u16* fcw, const u32* mxcsr) {
        Move(0, reinterpret_cast<u64>(fcw)); Bytes({0xd9, 0x28});
        Move(0, reinterpret_cast<u64>(mxcsr)); Bytes({0x0f, 0xae, 0x10});
    }
    void ObserveAt(u64 phase, u64 observer) {
        Move(0, reinterpret_cast<u64>(&data->observed_fcw)); Bytes({0xd9, 0x38});
        Move(0, reinterpret_cast<u64>(&data->observed_mxcsr)); Bytes({0x0f, 0xae, 0x18});
        Move(7, phase); Call(observer);
    }
    void Control(u64 veneer, OrbisFiber* fiber, u64 argument, u64* output) {
        Move(7, reinterpret_cast<u64>(fiber)); Move(6, argument);
        Move(2, reinterpret_cast<u64>(output)); Call(veneer);
    }
    void Return(u64 veneer, u64 argument, u64* output) {
        Move(7, argument); Move(6, reinterpret_cast<u64>(output)); Call(veneer);
    }
    void SaveEntryArgs() { Bytes({0x49, 0x89, 0xfc, 0x49, 0x89, 0xf5}); }
};
} // namespace

int main() {
    const auto host_page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    CHECK(host_page == 4096 || host_page == 16384);
    Mapping code{host_page}, storage{host_page}, stack{65536}, a_stack{65536}, b_stack{65536};
    data = std::construct_at(reinterpret_cast<Data*>(storage.data));
    Core::Tcb tcb{};
    Core::SetTcbBase(&tcb);
    Core::Loader::SymbolsResolver symbols;
    Libraries::Fiber::RegisterLib(&symbols);
    auto& registry = symbols.GetHleCallRegistry();
    HleVeneerAllocator veneers;
    const auto allocate = [&](const std::shared_ptr<HleCallAdapter>& adapter) {
        CHECK(adapter != nullptr);
        auto result = veneers.Allocate(*adapter);
        CHECK(std::holds_alternative<u64>(result));
        return std::get<u64>(result);
    };
    const auto find = [&](const char* nid) {
        for (const auto& symbol : symbols.GetSymbols())
            if (symbol.name.starts_with(nid)) return allocate(symbol.hle_adapter);
        CHECK(false);
        return u64{};
    };
    const auto run = find("a0LLrZWac0M");
    const auto switch_to = find("PFT2S-tJ7Uk");
    const auto return_to = find("B0ZX2hx9DMw");
    const auto observer = allocate(registry.Register(std::make_shared<Observe>(), "observe"));
    SetFiberEntryReturnAddress(allocate(registry.Register(MakeFiberEntryReturnAdapter(), "entry-return")));
    const auto a_entry = code.Address() + 1024;
    const auto b_entry = code.Address() + 2048;
    CHECK(sceFiberInitialize(&data->a, "a", reinterpret_cast<OrbisFiberEntry>(a_entry), 0xa001,
                             a_stack.data, a_stack.size, nullptr, 0x3500000) == 0);
    CHECK(sceFiberInitialize(&data->b, "b", reinterpret_cast<OrbisFiberEntry>(b_entry), 0xb001,
                             b_stack.data, b_stack.size, nullptr, 0) == 0);
    Code main{code.data};
    main.Fp(&data->main_fcw, &data->main_mxcsr);
    main.ObserveAt(0, observer);
    main.Control(run, &data->a, 11, &data->main_output); main.ObserveAt(1, observer);
    main.Control(run, &data->a, 22, &data->main_output); main.ObserveAt(2, observer);
    main.Control(run, &data->a, 44, &data->main_output); main.ObserveAt(3, observer);
    main.Control(run, &data->b, 55, &data->main_output); main.ObserveAt(4, observer);
    main.Bytes({0xc3});
    CHECK(main.cursor < code.data + 1024);
    Code a{code.data + 1024};
    a.SaveEntryArgs(); a.ObserveAt(10, observer);
    a.Fp(&data->a_fcw, &data->a_mxcsr);
    a.Return(return_to, 0xaa, &data->a_output); a.ObserveAt(11, observer);
    a.Control(switch_to, &data->b, 33, &data->a_output); a.ObserveAt(12, observer);
    a.Return(return_to, 0xcc, &data->a_output); a.Bytes({0x0f, 0x0b});
    CHECK(a.cursor < code.data + 2048);
    Code b{code.data + 2048};
    b.SaveEntryArgs(); b.ObserveAt(20, observer);
    b.Fp(&data->b_fcw, &data->b_mxcsr);
    b.Return(return_to, 0xbb, &data->b_output); b.ObserveAt(21, observer);
    b.Return(return_to, 0xdd, &data->b_output); b.Bytes({0x0f, 0x0b});
    CHECK(b.cursor < code.data + code.size);
    CHECK(mprotect(code.data, code.size, PROT_READ) == 0);
    Fixture fixture;
    fixture.ranges = veneers.GetExecutableRanges();
    fixture.ranges.push_back({code.Address(), code.size, true, false});
    for (const auto* mapping : {&storage, &stack, &a_stack, &b_stack})
        fixture.ranges.push_back({mapping->Address(), mapping->size, false, true});
    HleGuestBridge bridge{registry, Fixture::Access, &fixture, nullptr, nullptr,
                          Fixture::Executable, &fixture};
    auto created = Fex::GuestEngine::Create(bridge);
    CHECK(std::holds_alternative<std::unique_ptr<Fex::GuestEngine>>(created));
    auto engine = std::move(std::get<std::unique_ptr<Fex::GuestEngine>>(created));
    fixture.ranges.push_back(engine->ReturnRange());
    fixture.ranges.push_back(engine->CallbackReturnRange());
    GuestExecutionRequest request{};
    request.Rip = code.Address(); request.Rsp = stack.Address() + stack.size - 8;
    request.Gpr[4] = request.Rsp; request.Rflags = 2;
    request.MappedRanges = fixture.ranges;
    const auto return_address = engine->ReturnAddress();
    std::memcpy(reinterpret_cast<void*>(request.Rsp), &return_address, 8);
    request.Xmm[0] = {0x1122334455667788, 0x99aabbccddeeff00};
    auto created_thread = engine->CreateThread(request);
    CHECK(std::holds_alternative<Fex::GuestEngine::Thread*>(created_thread));
    auto* thread = std::get<Fex::GuestEngine::Thread*>(created_thread);
    const auto result = engine->Run(*thread);
    if (const auto* failure = std::get_if<Fex::EngineFailure>(&result))
        std::fprintf(stderr, "engine failure stage=%d error=%d\n", static_cast<int>(failure->Stage), failure->Error);
    CHECK(std::holds_alternative<GuestExecutionState>(result));
    CHECK(observed == expected_phases.size());
    CHECK(tcb.tcb_fiber == nullptr);
    CHECK(data->a.state == FiberState::Idle && data->b.state == FiberState::Idle);
    CHECK(data->a.saved.valid && data->b.saved.valid);
    CHECK(std::holds_alternative<bool>(engine->DestroyThread(thread)));
    CHECK(std::holds_alternative<bool>(engine->Shutdown()));
    Core::SetTcbBase(nullptr);
    std::puts("fiber-runtime=PASS entry return resume switch FCW MXCSR callret SIMD-reset");
}
