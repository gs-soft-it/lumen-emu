// SPDX-License-Identifier: MIT
#include "core/fex/fex_guest_engine.h"
#include "core/fex/guest_memory_scope.h"
#ifdef LUMEN_GEN4_DIRECT_FEX
#include "core/fex/host_mapping_snapshot.h"
#include <sstream>
#endif
#include "core/guest_cpu/fex_hle_bridge.h"
#include "core/libraries/kernel/sync/host_wait.h"
#include "core/libraries/kernel/threads/exception.h"
#if !defined(LUMEN_GEN4_DIRECT_FEX) || defined(__APPLE__)
#include "core/libraries/font/font_driver_abi.h"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <thread>
#include <semaphore>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
using namespace Core;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)
#if defined(__APPLE__) && defined(LUMEN_GEN4_DIRECT_FEX)
std::atomic<unsigned> atomic_faults{};
static_assert(std::atomic<unsigned>::is_always_lock_free);
void AtomicFault(int signal, siginfo_t* info, void* context) {
    if (Core::Fex::HandleGuestSignal(signal, info, context)) {
        atomic_faults.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    _exit(125);
}
#endif
struct Fixture {
    std::vector<GuestExecutionRange> ranges;
    std::atomic<std::uintptr_t> revoked{};
    static bool Access(void* self, std::uintptr_t address, std::size_t size, bool write) {
        if (address == static_cast<Fixture*>(self)->revoked.load()) return false;
        for (const auto& r : static_cast<Fixture*>(self)->ranges)
            if (address >= r.Begin && address - r.Begin < r.Size && size <= r.Size - (address-r.Begin) && (!write || r.Writable)) return true;
        return false;
    }
    static std::optional<GuestExecutionRange> Executable(void* self, std::uintptr_t address) {
        for (const auto& r : static_cast<Fixture*>(self)->ranges)
            if (r.Executable && address >= r.Begin && address-r.Begin < r.Size) return r;
        return std::nullopt;
    }
};
struct Mapping {
    std::size_t size{static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
#if defined(LUMEN_GEN4_DIRECT_FEX) && !defined(__APPLE__)
    unsigned char* base{};
    Mapping() {
        static std::atomic<std::uintptr_t> next{0x600000000ULL};
        for (unsigned attempt = 0; attempt < 1024; ++attempt) {
            const auto address = next.fetch_add(size);
            base = static_cast<unsigned char*>(mmap(reinterpret_cast<void*>(address), size,
                PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0));
            if (base != MAP_FAILED) break;
            CHECK(errno == EEXIST);
        }
        CHECK(base != MAP_FAILED);
        CHECK(Address() >= 0x100000000ULL && Address() + size <= 0x900000000ULL);
    }
#else
    unsigned char* base{static_cast<unsigned char*>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0))};
    Mapping() { CHECK(base != MAP_FAILED); }
#endif
    ~Mapping() { CHECK(munmap(base,size)==0); }
    std::uintptr_t Address() const { return reinterpret_cast<std::uintptr_t>(base); }
};
#ifdef LUMEN_GEN4_DIRECT_FEX
void CheckMappingSnapshot() {
    using namespace Fex::detail;
    std::istringstream input{
        "1000-2000 r--p 00000000 00:00 0\n"
        "2000-3000 r-xp 00000000 00:00 0\n"
        "3000-4000 rw-p 00000000 00:00 0\n"
        "5000-6000 rw-s 00000000 00:00 0\n"
        "6000-6000 ---p 00000000 00:00 0\n"
        "6000-7000 --xp 00000000 00:00 0\n"};
    const auto snapshot = ParseHostMappings(input);
    CHECK(snapshot && snapshot->size() == 5);
    CHECK(CheckHostMapping(*snapshot, 0x1000, 0x2000, true, false) == HostMappingResult::Valid);
    CHECK(CheckHostMapping(*snapshot, 0x1000, 0x2001, true, false) == HostMappingResult::CodePermission);
    CHECK(CheckHostMapping(*snapshot, 0x3000, 0x1000, false, true) == HostMappingResult::Valid);
    CHECK(CheckHostMapping(*snapshot, 0x5000, 0x1000, false, true) == HostMappingResult::Valid);
    CHECK(CheckHostMapping(*snapshot, 0x3000, 0x2001, false, true) == HostMappingResult::Gap);
    CHECK(CheckHostMapping(*snapshot, 0x4000, 1, false, false) == HostMappingResult::Gap);
    CHECK(CheckHostMapping(*snapshot, 0x6000, 0x1001, false, false) == HostMappingResult::Gap);
    CHECK(CheckHostMapping(*snapshot, 0x3000, 1, true, false) == HostMappingResult::CodePermission);
    CHECK(CheckHostMapping(*snapshot, 0x6000, 1, true, false) == HostMappingResult::CodePermission);
    CHECK(CheckHostMapping(*snapshot, 0x2000, 1, false, true) == HostMappingResult::WritePermission);
    CHECK(CheckHostMapping(*snapshot, UINTPTR_MAX - 1, 3, false, false) == HostMappingResult::Gap);
    CHECK(CheckHostMapping(*snapshot, 0x1000, 0, true, false) == HostMappingResult::Gap);
    for (const auto* invalid : {
        "", "garbage\n", "1000-2000 rw-p\nmalformed\n",
        "1000-2000 rw-p\n1800-3000 r--p\n", "3000-4000 rw-p\n1000-2000 r--p\n",
        "2000-1000 rw-p\n", "10000000000000000-20000000000000000 rw-p\n",
        "1000-2000 rw-z\n", "1000-2000 r--\n", "1000-1000 r--p\n"}) {
        std::istringstream malformed{invalid};
        CHECK(!ParseHostMappings(malformed));
    }
    std::istringstream failed{"1000-2000 r--p\n"};
    failed.setstate(std::ios::badbit);
    CHECK(!ParseHostMappings(failed));
}
#endif
std::uint64_t Sum(std::uint64_t a,std::uint64_t b,std::uint64_t c,std::uint64_t d,std::uint64_t e,std::uint64_t f,std::uint64_t g) { return a+b+c+d+e+f+g; }
double Scale(double input) { return input*2.0; }
Fex::GuestEngine* engine{};
std::uintptr_t callback{};
std::uintptr_t signal_callback{};
void NativeSignal(int, siginfo_t* info, void* context) {
    Core::Fex::DeliverGuestOrbisSignal(30, info, context, signal_callback);
}
std::atomic<bool> wait_entered{};
std::binary_semaphore wait_release{0};
std::binary_semaphore callback_publication{0};
bool publish_during_callback{};
#ifdef LUMEN_GEN4_DIRECT_FEX
std::uint64_t AwaitSignal(std::uint64_t address) {
    auto* words = reinterpret_cast<std::uint64_t*>(address);
    std::atomic_ref{words[0]}.store(1, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::atomic_ref{words[1]}.load(std::memory_order_acquire) == 0) {
        Core::Fex::FlushPendingGuestOrbisSignal();
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    return 0;
}
#endif
std::uintptr_t unchanged_code_base{};
std::size_t unchanged_code_size{};
std::uint64_t AwaitCallbackPublication() {
    CHECK(Libraries::Kernel::NativeHostWait([] {
        return callback_publication.try_acquire_for(std::chrono::seconds{5});
    }));
    return 71;
}
std::uint64_t WaitAndValidate(std::uint64_t output) {
    CHECK(Core::GuestCpu::ValidateHlePointer(reinterpret_cast<void*>(output), 8, true));
    wait_entered.store(true);
    const bool released = Libraries::Kernel::NativeHostWait([] {
        return wait_release.try_acquire_for(std::chrono::seconds{5});
    });
    CHECK(released);
    CHECK(!Libraries::Kernel::ValidateWaitOutput(reinterpret_cast<void*>(output), 8));
    return 99;
}
std::uint64_t Nested(std::uint64_t input) {
    CHECK(GuestCpu::IsGuestHleCallActive());
    const std::array<std::uint64_t,1> args{input};
    std::thread publisher;
    if (publish_during_callback) {
        publisher = std::thread([] {
            {
                Fex::MemoryMutationScope mutation;
                CHECK(mutation.Valid());
                if (unchanged_code_base != 0) {
                    // The loader may restore host RX without changing guest
                    // access or replacing a suspended continuation's mapping.
                    CHECK(mprotect(reinterpret_cast<void*>(unchanged_code_base),
                                   unchanged_code_size, PROT_READ) == 0);
                    Fex::MemoryMutationScope::PublishCurrent(
                        unchanged_code_base, unchanged_code_size, false);
                }
            }
            callback_publication.release();
        });
    }
    auto result=engine->CallGuest(callback,args);
    CHECK(std::holds_alternative<GuestExecutionState>(result));
    if (publisher.joinable()) publisher.join();
    CHECK(GuestCpu::IsGuestHleCallActive());
    return std::get<GuestExecutionState>(result).Gpr[0];
}
float* mixed_outputs{};
std::uint64_t* stack_output{};
std::uint64_t StackOutputCallback() {
    // The Font open callback's output pointer is its seventh INTEGER argument.
    const std::array<std::uint64_t, 7> args{1, 2, 3, 4, 5, 6,
        reinterpret_cast<std::uintptr_t>(stack_output)};
    const auto result = engine->CallGuest(callback, args);
    CHECK(std::holds_alternative<GuestExecutionState>(result));
    CHECK(*stack_output == 21);
    return std::get<GuestExecutionState>(result).Gpr[0];
}
#if defined(LUMEN_GEN4_DIRECT_FEX) && !defined(__APPLE__)
std::uint64_t CallbackBounds() {
    const std::array<std::uint64_t, 8> arguments{};
    const auto rejected = engine->CallGuest(callback, arguments);
    CHECK(std::holds_alternative<Fex::EngineFailure>(rejected));
    CHECK(std::get<Fex::EngineFailure>(rejected).Stage == Fex::EngineStage::Request);
    CHECK(std::get<Fex::EngineFailure>(rejected).Error == E2BIG);
    return 81;
}
#else
std::uint64_t MixedCallback(std::uint64_t with_dpi) {
    const float x = with_dpi ? 1.25f : -0.0f;
    const float y = with_dpi ? -3.5f : std::bit_cast<float>(std::uint32_t{0x7fc12345});
    const auto args = Libraries::Font::Internal::MakeFontScaleCallbackArguments(
        7, with_dpi != 0, 48, 72, x, y,
        reinterpret_cast<std::uintptr_t>(&mixed_outputs[0]),
        reinterpret_cast<std::uintptr_t>(&mixed_outputs[1]));
    const std::array<std::array<std::uint64_t, 2>, 9> excessive_xmm{};
    const auto rejected = engine->CallGuest(callback, args.Integers(), excessive_xmm);
    CHECK(std::holds_alternative<Fex::EngineFailure>(rejected));
    CHECK(std::get<Fex::EngineFailure>(rejected).Stage == Fex::EngineStage::Request);
    CHECK(std::get<Fex::EngineFailure>(rejected).Error == E2BIG);
    const auto result = engine->CallGuest(callback, args.Integers(), args.xmm);
    CHECK(std::holds_alternative<GuestExecutionState>(result));
    CHECK(std::bit_cast<std::uint32_t>(mixed_outputs[0]) == std::bit_cast<std::uint32_t>(x));
    CHECK(std::bit_cast<std::uint32_t>(mixed_outputs[1]) == std::bit_cast<std::uint32_t>(y));
    return std::get<GuestExecutionState>(result).Gpr[0];
}
#endif
void Append64(std::vector<unsigned char>& code,std::uint64_t value) {
    const auto start=code.size(); code.resize(start+8); std::memcpy(code.data()+start,&value,8);
}
void Call(std::vector<unsigned char>& code,std::uintptr_t target) {
    code.insert(code.end(),{0x48,0xb8}); Append64(code,target); code.insert(code.end(),{0xff,0xd0});
}
}
int main() {
    using namespace Core;
#ifdef LUMEN_GEN4_DIRECT_FEX
    CheckMappingSnapshot();
#endif
    // Original newer registry: bounded raw publication plus shared owning lookup.
    auto registry=std::make_unique<GuestCpu::HleCallRegistry>();
    CHECK(!registry->Find(0) && !registry->Find(UINT64_MAX) && !registry->Find(65536));
    CHECK(!registry->FindRaw(0) && !registry->FindRaw(UINT64_MAX) && !registry->FindRaw(65536));
    auto sum=registry->Register(GuestCpu::MakeHleCallAdapter(Sum),"sum");
    auto scale=registry->Register(GuestCpu::MakeHleCallAdapter(Scale),"scale");
    auto nested=registry->Register(GuestCpu::MakeHleCallAdapter(Nested),"nested");
    auto wait=registry->Register(GuestCpu::MakeHleCallAdapter(WaitAndValidate),"wait-readmission");
    auto publication=registry->Register(GuestCpu::MakeHleCallAdapter(AwaitCallbackPublication),"callback-publication");
#if defined(LUMEN_GEN4_DIRECT_FEX) && !defined(__APPLE__)
    auto mixed=registry->Register(GuestCpu::MakeHleCallAdapter(CallbackBounds),"callback-bounds");
#else
    auto mixed=registry->Register(GuestCpu::MakeHleCallAdapter(MixedCallback),"mixed-callback");
#endif
    auto stackOutput=registry->Register(GuestCpu::MakeHleCallAdapter(StackOutputCallback),"stack-output-callback");
    CHECK(sum && scale && nested && mixed && stackOutput && sum->Operation()==1 && nested->Operation()==3);
    CHECK(!registry->Register({},"null"));
    std::atomic<bool> stop{};
    std::thread reader([&] { while(!stop.load()) { CHECK(registry->Find(1)==sum); CHECK(registry->FindRaw(1)==sum.get()); } });
#ifdef LUMEN_GEN4_DIRECT_FEX
    auto signalWait = registry->Register(GuestCpu::MakeHleCallAdapter(AwaitSignal), "signal-wait");
    CHECK(signalWait);
    constexpr unsigned firstEntry = 9;
#else
    constexpr unsigned firstEntry = 8;
#endif
    for (unsigned id=firstEntry;id<=65535;++id) CHECK(registry->Register(GuestCpu::MakeHleCallAdapter(Scale),"entry")->Operation()==id);
    stop.store(true); reader.join(); CHECK(registry->Find(65535));
    CHECK(registry->FindRaw(65535) == registry->Find(65535).get());
    CHECK(!registry->Register(GuestCpu::MakeHleCallAdapter(Scale), "exhausted"));
    CHECK(!registry->FindRaw(65536));
    CHECK(!registry->Find(65537));
    {
        auto temporary = std::make_unique<GuestCpu::HleCallRegistry>();
        auto registered = temporary->Register(GuestCpu::MakeHleCallAdapter(Scale), "retained");
        // The reference does not reject registering the same adapter twice.
        auto duplicate = temporary->Register(registered, "retained");
        CHECK(duplicate == registered && registered->Operation() == 2);
        CHECK(temporary->FindRaw(1) == registered.get());
        CHECK(temporary->FindRaw(2) == registered.get());
        duplicate.reset();
        std::weak_ptr<GuestCpu::HleCallAdapter> observer = registered;
        auto retained = temporary->Find(registered->Operation());
        registered.reset();
        temporary.reset();
        CHECK(!observer.expired() && retained->Name() == "retained");
        retained.reset();
        CHECK(observer.expired());
    }

    Fixture fixture;
    GuestCpu::HleVeneerAllocator veneers;
    const auto makeVeneer=[&](auto& adapter) {
        const auto result=veneers.Allocate(*adapter); CHECK(std::holds_alternative<u64>(result));
        return std::get<u64>(result);
    };
    const auto mixedVeneer=makeVeneer(mixed);
    const auto stackOutputVeneer=makeVeneer(stackOutput);
    const auto sumVeneer=makeVeneer(sum), scaleVeneer=makeVeneer(scale), nestedVeneer=makeVeneer(nested), waitVeneer=makeVeneer(wait);
    const auto publicationVeneer=makeVeneer(publication);
#ifdef LUMEN_GEN4_DIRECT_FEX
    const auto signalWaitVeneer = makeVeneer(signalWait);
#endif
    fixture.ranges=veneers.GetExecutableRanges();
    Mapping code,stack,tls;
    fixture.ranges.push_back({code.Address(),code.size,true,false});
    fixture.ranges.push_back({stack.Address(),stack.size,false,true});
    fixture.ranges.push_back({tls.Address(),tls.size,false,true});
    GuestCpu::HleGuestBridge bridge{*registry,Fixture::Access,&fixture,nullptr,nullptr,Fixture::Executable,&fixture};
    auto created=Fex::GuestEngine::Create(bridge); CHECK(std::holds_alternative<std::unique_ptr<Fex::GuestEngine>>(created));
    auto owned=std::move(std::get<std::unique_ptr<Fex::GuestEngine>>(created)); engine=owned.get();
#ifdef LUMEN_GEN4_DIRECT_FEX
    fixture.ranges.push_back(engine->ReturnRange());
    fixture.ranges.push_back(engine->CallbackReturnRange());
    // Every request must observe current host protection, including revocation
    // after a successful request. No process-wide snapshot may authorize it.
    GuestExecutionRequest mapping_request{};
    mapping_request.Rip = code.Address();
    mapping_request.Rsp = stack.Address() + stack.size - 8;
    mapping_request.Rflags = 2;
    mapping_request.MappedRanges = fixture.ranges;
    std::reverse(mapping_request.MappedRanges.begin(), mapping_request.MappedRanges.end());
    const auto validate_request = [&](int expected_error) {
        auto result = engine->CreateThread(mapping_request);
        if (expected_error == 0) {
            CHECK(std::holds_alternative<Fex::GuestEngine::Thread*>(result));
            CHECK(std::holds_alternative<bool>(engine->DestroyThread(
                std::get<Fex::GuestEngine::Thread*>(result))));
        } else {
            CHECK(std::holds_alternative<Fex::EngineFailure>(result));
            CHECK(std::get<Fex::EngineFailure>(result).Stage == Fex::EngineStage::Mapping);
            CHECK(std::get<Fex::EngineFailure>(result).Error == expected_error);
        }
    };
    CHECK(mprotect(code.base, code.size, PROT_READ) == 0);
    validate_request(0);
    CHECK(mprotect(code.base, code.size, PROT_READ | PROT_WRITE) == 0);
    validate_request(EACCES);
    CHECK(mprotect(code.base, code.size, PROT_NONE) == 0);
    validate_request(EACCES);
    CHECK(mprotect(code.base, code.size, PROT_READ) == 0);
    CHECK(mprotect(stack.base, stack.size, PROT_READ) == 0);
    validate_request(EACCES);
    CHECK(mprotect(stack.base, stack.size, PROT_READ | PROT_WRITE) == 0);
    validate_request(0);
    mapping_request.MappedRanges.push_back(mapping_request.MappedRanges.front());
    validate_request(EACCES);
    mapping_request.MappedRanges.pop_back();
    validate_request(0);
    std::puts("direct-map-snapshot=PASS parser permissions holes fresh-request overlap");
#endif
    const auto run=[&](std::span<const unsigned char> bytes, std::uint64_t arg=0, double fp=0.0) {
        {
            Fex::MemoryMutationScope mutation; CHECK(mutation.Valid());
            CHECK(mprotect(code.base,code.size,PROT_READ|PROT_WRITE)==0);
            std::memcpy(code.base,bytes.data(),bytes.size());
            CHECK(mprotect(code.base,code.size,PROT_READ)==0);
            mutation.Publish(code.Address(),code.size);
        }
        const auto ret=engine->ReturnAddress();
        GuestExecutionRequest request{}; request.Rip=code.Address(); request.Rsp=stack.Address()+stack.size-8;
        std::memcpy(reinterpret_cast<void*>(request.Rsp),&ret,8);
        request.Gpr[4]=request.Rsp; request.Gpr[7]=arg; request.Gpr[12]=0xfedcba9876543210ULL;
        request.Rflags=2; request.FsBase=tls.Address(); request.MappedRanges=fixture.ranges;
        std::memcpy(request.Xmm[0].data(),&fp,8);
        auto threadResult=engine->CreateThread(request); CHECK(std::holds_alternative<Fex::GuestEngine::Thread*>(threadResult));
        auto* thread=std::get<Fex::GuestEngine::Thread*>(threadResult);
#ifdef LUMEN_GEN4_DIRECT_FEX
        // The native core exposes executable cache invalidation explicitly.
        CHECK(std::holds_alternative<bool>(engine->Invalidate(*thread, code.Address(), code.size)));
#endif
        auto result=engine->Run(*thread);
        CHECK(std::holds_alternative<GuestExecutionState>(result));
        CHECK(std::holds_alternative<bool>(engine->DestroyThread(thread)));
        return std::get<GuestExecutionState>(result);
    };
    std::vector<unsigned char> sumCode{0x6a,0x07,0xbf,1,0,0,0,0xbe,2,0,0,0,0xba,3,0,0,0,0xb9,4,0,0,0,
        0x41,0xb8,5,0,0,0,0x41,0xb9,6,0,0,0};
    Call(sumCode,sumVeneer); sumCode.insert(sumCode.end(),{0x48,0x83,0xc4,8,0xc3});
    const auto sumResult=run(sumCode); CHECK(sumResult.Gpr[0]==28 && sumResult.Gpr[12]==0xfedcba9876543210ULL);
    std::vector<unsigned char> floatCode{0x48,0x83,0xec,8}; Call(floatCode,scaleVeneer);
    floatCode.insert(floatCode.end(),{0x48,0x83,0xc4,8,0xc3});
    const auto floatResult=run(floatCode,0,1.5); double number{}; std::memcpy(&number,floatResult.Xmm[0].data(),8); CHECK(number==3.0);
    // A callback with seven INTEGER arguments reads its final output pointer
    // from [rsp+8], while the first six remain in the SysV argument registers.
    auto* stackOutputWords = reinterpret_cast<std::uint64_t*>(tls.base + 64);
    stackOutputWords[0] = 0x1122334455667788ULL;
    stackOutputWords[1] = 0;
    stackOutputWords[2] = 0x8877665544332211ULL;
    stack_output = &stackOutputWords[1];
    std::vector<unsigned char> stackOutputCode{0x48,0x83,0xec,8};
    Call(stackOutputCode,stackOutputVeneer);
    stackOutputCode.insert(stackOutputCode.end(),{0x48,0x83,0xc4,8,0xc3});
    stackOutputCode.resize(128,0xcc);
    callback=code.Address()+128;
    // r10=[rsp+8]; rax=rdi+rsi+rdx+rcx+r8+r9; [r10]=rax; ret
    stackOutputCode.insert(stackOutputCode.end(),{0x4c,0x8b,0x54,0x24,8,
        0x48,0x89,0xf8,0x48,0x01,0xf0,0x48,0x01,0xd0,0x48,0x01,0xc8,
        0x4c,0x01,0xc0,0x4c,0x01,0xc8,0x49,0x89,0x02,0xc3});
    CHECK(run(stackOutputCode).Gpr[0]==21);
    CHECK(stackOutputWords[0]==0x1122334455667788ULL);
    CHECK(stackOutputWords[2]==0x8877665544332211ULL);
#if defined(LUMEN_GEN4_DIRECT_FEX) && !defined(__APPLE__)
    std::vector<unsigned char> boundsCode{0x48,0x83,0xec,8};
    Call(boundsCode, mixedVeneer);
    boundsCode.insert(boundsCode.end(), {0x48,0x83,0xc4,8,0xc3});
    CHECK(run(boundsCode).Gpr[0] == 81);
#else
    // Fixed mixed SysV arguments use independent INTEGER/SSE positions. Exercise
    // both Font signatures through a real nested translated callback, preserving
    // float32 NaN payload/negative zero and the parent's unrelated XMM state.
    mixed_outputs = reinterpret_cast<float*>(tls.base + 32);
    for (const bool with_dpi : {false, true}) {
        std::vector<unsigned char> mixedCode{0x48,0x83,0xec,8};
        Call(mixedCode, mixedVeneer);
        mixedCode.insert(mixedCode.end(),{0x48,0x83,0xc4,8,0xc3});
        mixedCode.resize(128,0xcc);
        callback=code.Address()+128;
        if (with_dpi) {
            // movss [rcx],xmm0; movss [r8],xmm1; rax=rdi+rsi+rdx; ret
            mixedCode.insert(mixedCode.end(),{0xf3,0x0f,0x11,0x01,0xf3,0x41,0x0f,0x11,0x08,
                0x48,0x89,0xf0,0x48,0x01,0xd0,0x48,0x01,0xf8,0xc3});
        } else {
            // movss [rsi],xmm0; movss [rdx],xmm1; rax=rdi; ret
            mixedCode.insert(mixedCode.end(),{0xf3,0x0f,0x11,0x06,0xf3,0x0f,0x11,0x0a,
                0x48,0x89,0xf8,0xc3});
        }
        const auto result=run(mixedCode,with_dpi,1.5);
        CHECK(result.Gpr[0] == (with_dpi ? 127 : 7));
        double parent_fp{};
        std::memcpy(&parent_fp,result.Xmm[0].data(),sizeof(parent_fp));
        CHECK(parent_fp == 1.5);
    }
#endif
    // The callback resides later in the same immutable executable allocation.
    std::vector<unsigned char> nestedCode{0x48,0x83,0xec,8}; Call(nestedCode,nestedVeneer);
    nestedCode.insert(nestedCode.end(),{0x48,0x83,0xc4,8,0xc3});
    nestedCode.resize(128,0xcc); callback=code.Address()+128;
    nestedCode.insert(nestedCode.end(),{0x48,0x8d,0x47,3,0xc3});
    CHECK(run(nestedCode,10).Gpr[0]==13);
    // The nested callback waits for an independent publisher. Keeping the
    // parent HLE reader would prevent that publisher from ever progressing.
    nestedCode.resize(128);
    nestedCode.insert(nestedCode.end(), {0x48,0x83,0xec,8});
    Call(nestedCode,publicationVeneer);
    nestedCode.insert(nestedCode.end(), {0x48,0x83,0xc4,8,0xc3});
    publish_during_callback = true;
    CHECK(run(nestedCode).Gpr[0]==71);
    unchanged_code_base = code.Address();
    unchanged_code_size = code.size;
    CHECK(run(nestedCode).Gpr[0]==71);
    unchanged_code_base = 0;
    publish_during_callback = false;
    CHECK(!GuestCpu::IsGuestHleCallActive());
    const std::uint64_t tlsValue=0x123456789abcdef0ULL; std::memcpy(tls.base,&tlsValue,8);
    constexpr std::array<unsigned char,10> tlsCode{0x64,0x48,0x8b,0x04,0x25,0,0,0,0,0xc3};
    CHECK(run(tlsCode).Gpr[0]==tlsValue);
    constexpr std::array<unsigned char,11> flagsCode{0x48,0x31,0xc0,0x48,0x83,0xe8,1,0xf9,0x90,0x90,0xc3};
    const auto flags=run(flagsCode); CHECK((flags.Rflags & 0x81)==0x81);
#if defined(__APPLE__) && defined(LUMEN_GEN4_DIRECT_FEX)
    // An actual unaligned LSE compare/exchange enters the Darwin SIGBUS
    // patcher. It must publish patched JIT instructions under per-thread W^X
    // and resume the x86 operation without touching neighboring guest bytes.
    struct sigaction atomic_action{}, previous_bus{};
    atomic_action.sa_sigaction = AtomicFault;
    atomic_action.sa_flags = SA_SIGINFO;
    sigemptyset(&atomic_action.sa_mask);
    CHECK(sigaction(SIGBUS, &atomic_action, &previous_bus) == 0);
    constexpr std::uint64_t atomic_initial = 0x1122334455667788ULL;
    constexpr std::uint64_t atomic_desired = 0x8877665544332211ULL;
    std::vector<unsigned char> atomicCode{0x48, 0xb8}; Append64(atomicCode, atomic_initial);
    atomicCode.insert(atomicCode.end(), {0x48, 0xb9}); Append64(atomicCode, atomic_desired);
    atomicCode.insert(atomicCode.end(), {0xf0, 0x48, 0x0f, 0xb1, 0x0f, 0xc3});
    for (const size_t offset : {size_t{1}, size_t{15}}) {
        std::memset(tls.base, 0x5a, 64);
        std::memcpy(tls.base + offset, &atomic_initial, sizeof(atomic_initial));
        const auto atomic = run(atomicCode, tls.Address() + offset);
        CHECK(atomic.Gpr[0] == atomic_initial && (atomic.Rflags & 0x40) != 0);
        std::uint64_t observed{};
        std::memcpy(&observed, tls.base + offset, sizeof(observed));
        CHECK(observed == atomic_desired);
        for (size_t index = 0; index < 64; ++index)
            if (index < offset || index >= offset + 8) CHECK(tls.base[index] == 0x5a);
    }
    CHECK(atomic_faults.load() >= 1);
    CHECK(sigaction(SIGBUS, &previous_bus, nullptr) == 0);
    std::printf("darwin-unaligned-atomic=PASS same-granule cross-granule jit-wx neighbors faults=%u\n", atomic_faults.load());
#endif
    // A publisher must progress while HLE is blocked over native state. A
    // mapping revoked in that epoch must be rejected before any output write.
    std::thread publisher([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!wait_entered.load()) {
            CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        {
            Fex::MemoryMutationScope mutation; CHECK(mutation.Valid());
            fixture.revoked.store(tls.Address());
            mutation.Publish(tls.Address(), tls.size, true);
        }
        wait_release.release();
    });
    std::vector<unsigned char> waitCode{0x48,0x83,0xec,8}; Call(waitCode,waitVeneer);
    waitCode.insert(waitCode.end(),{0x48,0x83,0xc4,8,0xc3});
    CHECK(run(waitCode,tls.Address()).Gpr[0]==99);
    publisher.join();
    // A synchronous worker shares its owner's epoch and hands publication
    // back before the owner reopens CPU admission.
    Mapping delegated;
    {
        Fex::MemoryMutationScope owner; CHECK(owner.Valid());
        std::thread worker([&] {
            Fex::MemoryMutationScope::SynchronousDelegate delegate{owner};
            Fex::MemoryMutationScope nested; CHECK(nested.Valid());
            delegated.base[0] = 0x7a;
            nested.Publish(delegated.Address(), delegated.size);
        });
        worker.join();
    }
    CHECK(delegated.base[0] == 0x7a);
    // Interrupt a loop with no HLE calls. Delivery must use a reconstructed
    // guest boundary, enter the guest handler, and resume the original GPRs.
    fixture.revoked.store(0);
    std::memset(tls.base, 0, tls.size);
    signal_callback = code.Address() + 256;
    struct sigaction signal_action{}, old_signal_action{};
    signal_action.sa_sigaction = NativeSignal;
    signal_action.sa_flags = SA_SIGINFO;
    sigemptyset(&signal_action.sa_mask);
    CHECK(sigaction(SIGUSR1, &signal_action, &old_signal_action) == 0);
    const auto host_thread = std::bit_cast<std::uintptr_t>(pthread_self());
#ifdef LUMEN_GEN4_DIRECT_FEX
    // This engine delivers signals at HLE boundaries; interruption of a pure
    // translated loop belongs to the shared adapter's separate contract.
    std::vector<unsigned char> signalCode{0x48, 0x83, 0xec, 8, 0x48, 0xbf};
    Append64(signalCode, tls.Address());
    Call(signalCode, signalWaitVeneer);
    signalCode.insert(signalCode.end(), {0x48, 0x83, 0xc4, 8});
#else
    std::vector<unsigned char> signalCode{0x48, 0xb8}; Append64(signalCode, tls.Address());
    signalCode.insert(signalCode.end(), {0x48, 0xc7, 0x00, 1, 0, 0, 0}); // entered=1
    signalCode.insert(signalCode.end(), {0x83, 0x78, 8, 0, 0x74, 0xfa}); // while (!delivered)
#endif
    signalCode.insert(signalCode.end(), {0x48, 0xb8}); Append64(signalCode, 0x1234567);
    signalCode.push_back(0xc3);
    signalCode.resize(256, 0xcc);
    signalCode.insert(signalCode.end(), {0x48, 0xb8}); Append64(signalCode, tls.Address() + 8);
    signalCode.insert(signalCode.end(), {0x48, 0x89, 0x38, 0x48, 0x8b, 0x8e}); // delivered=signal, rcx=context.r12
    const uint32_t r12Offset = offsetof(Libraries::Kernel::Ucontext, uc_mcontext) +
        offsetof(Libraries::Kernel::Mcontext, mc_r12);
    const auto displacement = signalCode.size(); signalCode.resize(displacement + 4);
    std::memcpy(signalCode.data() + displacement, &r12Offset, 4);
    signalCode.insert(signalCode.end(), {0x48, 0x89, 0x48, 8, 0xc3});
    std::thread sender([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        auto& entered = *reinterpret_cast<std::uint64_t*>(tls.base);
        while (std::atomic_ref{entered}.load(std::memory_order_acquire) == 0) {
            CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
#ifdef LUMEN_GEN4_DIRECT_FEX
        CHECK(pthread_kill(std::bit_cast<pthread_t>(host_thread), SIGUSR1) == 0);
#else
        CHECK(Fex::SendGuestOrbisSignal(host_thread, SIGUSR1, 30, signal_callback) == 0);
#endif
    });
    const auto signalResult = run(signalCode);
    sender.join();
    CHECK(sigaction(SIGUSR1, &old_signal_action, nullptr) == 0);
    CHECK(signalResult.Gpr[0] == 0x1234567 && signalResult.Gpr[12] == 0xfedcba9876543210ULL);
    CHECK(*reinterpret_cast<std::uint64_t*>(tls.base + 8) == 30);
#ifdef LUMEN_GEN4_DIRECT_FEX
    CHECK(*reinterpret_cast<std::uint64_t*>(tls.base + 16) == 0);
#else
    CHECK(*reinterpret_cast<std::uint64_t*>(tls.base + 16) == 0xfedcba9876543210ULL);
#endif
    CHECK(std::holds_alternative<bool>(engine->Shutdown()));
#if defined(LUMEN_GEN4_DIRECT_FEX) && defined(__APPLE__)
    std::puts("Gen4GuestRuntimeTest: PASS direct-fex Darwin registry=65535 hle-gp=7 hle-simd=1 nested=1 callback-gp=7 mixed-callback=2 xmm-bounds=1 tls=1 flags=1 invalidation=1 signal-hle-boundary=1; signal-tight-loop=unsupported");
#elif defined(LUMEN_GEN4_DIRECT_FEX)
    std::puts("Gen4GuestRuntimeTest: PASS direct-fex registry=65535 raw-acquire=1 shared-lifetime=1 hle-gp=7 hle-simd=1 nested=1 callback-gp=7 callback-gp8-rejected=1 tls=1 flags=1 invalidation=1 signal-hle-boundary=1 signal-context=1; signal-tight-loop=unsupported");
#else
    std::puts("Gen4GuestRuntimeTest: PASS registry=65535 raw-acquire=1 shared-lifetime=1 hle-gp=7 hle-simd=1 nested=1 callback-gp=7 mixed-callback=2 xmm-bounds=1 tls=1 flags=1 invalidation=1 wait-publication=1 stale-output-denied=1 delegated-publication=1 signal-tight-loop=1 signal-context=1");
#endif
}
