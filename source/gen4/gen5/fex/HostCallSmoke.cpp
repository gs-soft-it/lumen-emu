// Lumen typed-HLE fixture, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#include "loader/hostCall.h"
#include "loader/hostCallShaderRegister.h"
#include "libs/vaContext.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
constexpr std::uint64_t Stack = 0x1000;
std::array<std::uint8_t, 256> stack {};
std::uint32_t calls {};
bool Read(void*, std::uint64_t address, void* destination, std::size_t size) noexcept {
    if (address < Stack || address - Stack > stack.size() || size > stack.size() - (address - Stack))
        return false;
    std::memcpy(destination, stack.data() + address - Stack, size);
    return true;
}
template <typename T> void Store(std::size_t offset, T value) {
    std::memcpy(stack.data() + offset, &value, sizeof(value));
}
std::uint64_t KYTY_SYSV_ABI Gp8(std::uint64_t a, std::uint64_t b, std::uint64_t c,
    std::uint64_t d, std::uint64_t e, std::uint64_t f, std::uint64_t g, std::uint64_t h) {
    ++calls;
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;
}
double KYTY_SYSV_ABI Mixed(std::uint64_t a, double x, std::uint64_t b, float y,
    std::uint64_t c, double z, std::uint64_t d, double p, std::uint64_t e, double q,
    std::uint64_t f, double r, std::uint64_t g, double s, double t, double u) {
    ++calls;
    return static_cast<double>(a + b + c + d + e + f + g) + x + y + z + p + q + r + s + t + u;
}
float KYTY_SYSV_ABI FloatResult(float value) { ++calls; return value * 3; }
std::int32_t KYTY_SYSV_ABI Narrow(std::int8_t a, std::uint8_t b, std::uint32_t c) {
    ++calls;
    return a + b - static_cast<std::int32_t>(c);
}
enum class Mode : std::uint32_t { Test = 4 };
void* KYTY_SYSV_ABI Pointer(void* pointer, Mode mode) {
    ++calls;
    return mode == Mode::Test ? pointer : nullptr;
}
void KYTY_SYSV_ABI VoidReturn() { ++calls; }
struct Aggregate { std::uint64_t word[2]; };
Aggregate KYTY_SYSV_ABI Unsupported(Aggregate value) { ++calls; return value; }
struct UndeclaredEightbyte { std::uint32_t low, high; };
UndeclaredEightbyte KYTY_SYSV_ABI UnsupportedSmall(UndeclaredEightbyte value) {
    ++calls;
    return value;
}
using ShaderRegister = Libs::Graphics::ShaderRegister;
ShaderRegister seen_register {};
ShaderRegister KYTY_SYSV_ABI RegisterGp(std::uint64_t tag, ShaderRegister reg) {
    ++calls;
    seen_register = reg;
    return {reg.value ^ static_cast<std::uint32_t>(tag), reg.offset + 3};
}
std::uint64_t KYTY_SYSV_ABI RegisterSpill(std::uint64_t a, std::uint64_t b,
    std::uint64_t c, std::uint64_t d, std::uint64_t e, std::uint64_t f,
    ShaderRegister reg, std::uint64_t tail) {
    ++calls;
    seen_register = reg;
    return a + 2*b + 3*c + 4*d + 5*e + 6*f +
        7*static_cast<std::uint64_t>(reg.offset) +
        11*static_cast<std::uint64_t>(reg.value) + 13*tail;
}
int KYTY_SYSV_ABI NativeVariadic(const char*, ...) { ++calls; return 0; }
std::uint64_t KYTY_SYSV_ABI GuestVariadic(VA_ARGS) {
    using Libs::VaContext;
    using Libs::VaRegSave;
    ++calls;
    VA_CONTEXT(context);
    std::uint64_t result {};
    for (int i = 0; i != 8; ++i) result += Libs::VaArg_long_long(&context.va_list);
    result += static_cast<std::uint64_t>(Libs::VaArg_double(&context.va_list));
    return result;
}
template <auto Function> bool Dispatch(std::uint64_t* gprs, LumenAndroidFexCoreSimdViewV2* simd) {
    return Loader::DispatchHostCall(reinterpret_cast<std::uint64_t>(Function), gprs, simd, Read, nullptr);
}

template <std::uint64_t Index>
std::uint64_t KYTY_SYSV_ABI CacheTarget(std::uint64_t value) { return value + Index; }

template <std::size_t... Index>
bool RegisterCacheTargets(std::index_sequence<Index...>) {
    return (Loader::RegisterHostFunction<CacheTarget<Index + 1>>(
        "late-registered-HLE-metadata-cache-target") && ...);
}

bool TestConcurrentDispatchRegistration() {
    if (!Loader::RegisterHostFunction<CacheTarget<0>>("cached-HLE-target")) return false;
    std::array<std::uint64_t, 16> initial {};
    initial[4] = Stack;
    initial[7] = 37;
    if (!Dispatch<CacheTarget<0>>(initial.data(), nullptr) || initial[0] != 37) return false;
    using Effect = Lumen::GuestCpu::HostCallEffect;
    const auto target = reinterpret_cast<std::uint64_t>(CacheTarget<0>);
    if (Loader::HostCallEffectsFor(target) != Lumen::GuestCpu::EffectMask(Effect::Unknown) ||
        !Loader::RegisterHostFunction<CacheTarget<0>>("audited-cached-HLE-target", Effect::None) ||
        !Loader::RegisterHostFunction<CacheTarget<0>>("unclassified-alias") ||
        Loader::HostCallEffectsFor(target) != Lumen::GuestCpu::EffectMask(Effect::None)) return false;
    std::atomic<bool> start {false}, valid {true};
    std::atomic<unsigned> ready {};
    std::array<std::thread, 4> workers;
    for (auto& worker: workers) worker = std::thread([&] {
        std::array<std::uint64_t, 16> gprs {};
        gprs[4] = Stack;
        gprs[7] = 53;
        ++ready;
        while (!start.load()) std::this_thread::yield();
        for (unsigned i = 0; i != 4096; ++i)
            if (!Dispatch<CacheTarget<0>>(gprs.data(), nullptr) || gprs[0] != 53 ||
                Loader::HostCallEffectsFor(target) != Lumen::GuestCpu::EffectMask(Effect::None))
                valid.store(false);
    });
    while (ready.load() != workers.size()) std::this_thread::yield();
    start.store(true);
    // Exercise insertion/rehash while other threads retain immutable nodes.
    const bool registered = RegisterCacheTargets(std::make_index_sequence<64> {});
    for (auto& worker: workers) worker.join();
    return registered && valid.load() &&
        Dispatch<CacheTarget<0>>(initial.data(), nullptr) && initial[0] == 37;
}
bool TestConcurrentAdmissionPolicyPublication() {
    using namespace Lumen::GuestCpu;
    constexpr auto ordinary = MakeHostCallAdmissionPolicy(HostCallEffect::Unknown);
    constexpr auto audited = MakeHostCallAdmissionPolicy(HostCallEffect::GuestMemory,
        HostCallShortContract::SynchronousGuestMemory);
    const auto target = reinterpret_cast<std::uint64_t>(CacheTarget<66>);
    if (!Loader::RegisterHostFunction<CacheTarget<66>>("policy-before-attestation") ||
        Loader::HostCallAdmissionPolicyFor(target) != ordinary) return false;
    std::atomic<bool> start {}, valid {true};
    std::atomic<unsigned> ready {};
    std::array<std::thread, 4> readers;
    for (auto& reader: readers) reader = std::thread([&] {
        ++ready;
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (unsigned i = 0; i != 4096; ++i) {
            const auto policy = Loader::HostCallAdmissionPolicyFor(target);
            if (policy != ordinary && policy != audited) valid.store(false);
        }
    });
    while (ready.load() != readers.size()) std::this_thread::yield();
    start.store(true, std::memory_order_release);
    const bool registered = Loader::RegisterHostFunction<CacheTarget<66>>("policy-atomic-attestation",
        HostCallEffect::GuestMemory, HostCallShortContract::SynchronousGuestMemory);
    for (auto& reader: readers) reader.join();
    return registered && valid.load() && Loader::HostCallAdmissionPolicyFor(target) == audited;
}
} // namespace

int main() {
    using Effect = Lumen::GuestCpu::HostCallEffect;
    bool ok = Loader::RegisterHostFunction<Gp8>("Gp8") && Loader::RegisterHostFunction<Mixed>("Mixed") &&
        Loader::RegisterHostFunction<FloatResult>("FloatResult") &&
        Loader::RegisterHostFunction<Unsupported>("Unsupported") &&
        Loader::RegisterHostFunction<NativeVariadic>("NativeVariadic") &&
        Loader::RegisterHostFunction<GuestVariadic>("GuestVariadic") &&
        Loader::RegisterHostFunction<Narrow>("Narrow", Effect::None) &&
        Loader::RegisterHostFunction<Pointer>("Pointer", Effect::RetainsGuestPointer) &&
        Loader::RegisterHostFunction<VoidReturn>("VoidReturn", Effect::None) &&
        Loader::RegisterHostFunction<RegisterGp>("RegisterGp") &&
        Loader::RegisterHostFunction<RegisterSpill>("RegisterSpill") &&
        Loader::RegisterHostFunction<UnsupportedSmall>("UnsupportedSmall");
    ok &= Loader::HostCallRegistryReady() && Loader::HostCallCount() == 12 &&
        !Loader::IsHostCallSupported(reinterpret_cast<std::uint64_t>(Unsupported)) &&
        !Loader::IsHostCallSupported(reinterpret_cast<std::uint64_t>(NativeVariadic)) &&
        !Loader::IsHostCallSupported(reinterpret_cast<std::uint64_t>(UnsupportedSmall)) &&
        Loader::IsHostCallSupported(reinterpret_cast<std::uint64_t>(RegisterGp)) &&
        Loader::IsHostCallSupported(reinterpret_cast<std::uint64_t>(RegisterSpill));
    ok &= Loader::HostCallEffectsFor(0x1234) == Lumen::GuestCpu::EffectMask(Effect::Unknown) &&
        Loader::HostCallEffectsFor(reinterpret_cast<std::uint64_t>(Gp8)) == Lumen::GuestCpu::EffectMask(Effect::Unknown) &&
        Loader::HostCallEffectsFor(reinterpret_cast<std::uint64_t>(Narrow)) == Lumen::GuestCpu::EffectMask(Effect::None) &&
        Loader::HostCallEffectsFor(reinterpret_cast<std::uint64_t>(Pointer)) == Lumen::GuestCpu::EffectMask(Effect::RetainsGuestPointer);
    std::array<std::uint64_t, 16> gprs {};
    gprs[4] = Stack;
    constexpr std::size_t registers[] {7, 6, 2, 1, 8, 9};
    for (std::size_t i = 0; i != 6; ++i) gprs[registers[i]] = i + 1;
    Store(8, std::uint64_t {7});
    Store(16, std::uint64_t {8});
    alignas(16) std::array<std::uint8_t, 16 * 16> vectors {};
    LumenAndroidFexCoreSimdViewV2 simd {sizeof(simd), 16, 16, 16, vectors.data()};
    ok &= Dispatch<Gp8>(gprs.data(), &simd) && gprs[0] == 204 && calls == 1;
    for (std::size_t i = 0; i != 8; ++i) {
        const double value = static_cast<double>(i + 1);
        std::memcpy(vectors.data() + i * 16, &value, sizeof(value));
    }
    const float second = 2;
    std::memcpy(vectors.data() + 16, &second, sizeof(second));
    Store(16, double {9});
    double mixed {};
    ok &= Dispatch<Mixed>(gprs.data(), &simd);
    std::memcpy(&mixed, vectors.data(), sizeof(mixed));
    ok &= mixed == 73 && calls == 2;
    const float input = 2.5F;
    std::memcpy(vectors.data(), &input, sizeof(input));
    float output {};
    ok &= Dispatch<FloatResult>(gprs.data(), &simd);
    std::memcpy(&output, vectors.data(), sizeof(output));
    ok &= output == 7.5F && calls == 3;
    const auto before = calls;
    gprs[4] = UINT64_MAX - 4;
    ok &= !Dispatch<Gp8>(gprs.data(), &simd) && calls == before;
    gprs[4] = Stack + stack.size() - 8;
    ok &= !Dispatch<Gp8>(gprs.data(), &simd) && calls == before;
    gprs[4] = Stack;
    ok &= !Dispatch<FloatResult>(gprs.data(), nullptr) && calls == before;
    ok &= !Dispatch<Unsupported>(gprs.data(), &simd) && calls == before;
    ok &= !Dispatch<NativeVariadic>(gprs.data(), &simd) && calls == before;
    ok &= !Loader::DispatchHostCall(0x1234, gprs.data(), &simd, Read, nullptr) && calls == before;
    const double floating = 5;
    std::memcpy(vectors.data(), &floating, sizeof(floating));
    Store(16, std::uint64_t {8});
    ok &= Dispatch<GuestVariadic>(gprs.data(), &simd) && gprs[0] == 41 && calls == before + 1;
    gprs[7] = UINT64_MAX;
    gprs[6] = 0x102;
    gprs[2] = 0x100000004;
    ok &= Dispatch<Narrow>(gprs.data(), &simd) && gprs[0] == static_cast<std::uint64_t>(-3);
    gprs[7] = 0x12345678;
    gprs[6] = 4;
    ok &= Dispatch<Pointer>(gprs.data(), &simd) && gprs[0] == 0x12345678;
    gprs[0] = 0xABCDEF;
    ok &= Dispatch<VoidReturn>(gprs.data(), &simd) && gprs[0] == 0xABCDEF;
    const auto aggregate_before = calls;
    const ShaderRegister register_input {0x10203040, 0x8090a0b0};
    std::uint64_t register_word {};
    std::memcpy(&register_word, &register_input, sizeof(register_input));
    gprs[7] = 0x55667788;
    gprs[6] = register_word;
    ShaderRegister register_output {};
    ok &= Dispatch<RegisterGp>(gprs.data(), &simd);
    std::memcpy(&register_output, &gprs[0], sizeof(register_output));
    ok &= seen_register.offset == register_input.offset &&
          seen_register.value == register_input.value &&
          register_output.offset == (register_input.value ^ 0x55667788) &&
          register_output.value == register_input.offset + 3 && calls == aggregate_before + 1;
    for (std::size_t i = 0; i != 6; ++i) gprs[registers[i]] = i + 1;
    const ShaderRegister spilled_register {0xfedcba98, 0x76543210};
    Store(8, spilled_register);
    Store(16, std::uint64_t {19});
    ok &= Dispatch<RegisterSpill>(gprs.data(), &simd) &&
          gprs[0] == 91 + 7*static_cast<std::uint64_t>(spilled_register.offset) +
              11*static_cast<std::uint64_t>(spilled_register.value) + 13*19 &&
          seen_register.offset == spilled_register.offset &&
          seen_register.value == spilled_register.value && calls == aggregate_before + 2;
    gprs[4] = Stack + stack.size() - 8;
    ok &= !Dispatch<RegisterSpill>(gprs.data(), &simd) && calls == aggregate_before + 2;
    gprs[4] = Stack;
    ok &= !Dispatch<UnsupportedSmall>(gprs.data(), &simd) && calls == aggregate_before + 2;
    ok &= TestConcurrentDispatchRegistration() && TestConcurrentAdmissionPolicyPublication();
    using Contract = Lumen::GuestCpu::HostCallShortContract;
    const auto contract_target = reinterpret_cast<std::uint64_t>(CacheTarget<65>);
    ok &= Loader::RegisterHostFunction<CacheTarget<65>>("unclassified-contract-target") &&
        Lumen::GuestCpu::AdmissionPolicyContract(Loader::HostCallAdmissionPolicyFor(contract_target)) == Contract::Ordinary &&
        Loader::RegisterHostFunction<CacheTarget<65>>("audited-memory-contract-target", Effect::GuestMemory,
            Contract::SynchronousGuestMemory) &&
        Loader::RegisterHostFunction<CacheTarget<65>>("unclassified-contract-alias") &&
        Loader::HostCallAdmissionPolicyFor(contract_target) == Lumen::GuestCpu::MakeHostCallAdmissionPolicy(
            Effect::GuestMemory, Contract::SynchronousGuestMemory);
    // Start from a live, warmed certified node: the conflict itself must revoke
    // cached policy and dispatch. Later successful registration cannot revive it.
    ok &= Dispatch<CacheTarget<65>>(gprs.data(), &simd);
    const auto certified_result = gprs[0];
    ok &= !Loader::RegisterHostFunction<CacheTarget<65>>("first-conflicting-memory-contract",
            Effect::GuestMemory, Contract::Ordinary) &&
        Loader::HostCallAdmissionPolicyFor(contract_target) ==
            Lumen::GuestCpu::MakeHostCallAdmissionPolicy(Effect::Unknown, Contract::Ordinary) &&
        !Dispatch<CacheTarget<65>>(gprs.data(), &simd) && gprs[0] == certified_result &&
        Loader::RegisterHostFunction<CacheTarget<65>>("late-unclassified-alias-after-poison") &&
        Loader::RegisterHostFunction<CacheTarget<67>>("late-new-target-after-poison") &&
        !Loader::HostCallRegistryReady() && !Dispatch<CacheTarget<65>>(gprs.data(), &simd) &&
        Loader::HostCallAdmissionPolicyFor(contract_target) ==
            Lumen::GuestCpu::MakeHostCallAdmissionPolicy(Effect::Unknown, Contract::Ordinary);
    // Other malformed metadata remains independently rejected after poisoning.
    ok &= !Dispatch<VoidReturn>(gprs.data(), &simd);
    const auto poisoned_calls = calls;
    auto conflict = Loader::HostCallDetail::Signature<decltype(&VoidReturn)>::Describe();
    conflict.effects = Effect::Blocking;
    conflict.invoke = &Loader::HostCallDetail::InvokeBound<VoidReturn>;
    ok &= !Loader::RegisterHostCall(reinterpret_cast<std::uint64_t>(VoidReturn), conflict,
                                   "conflicting-cached-HLE-effects") &&
        !Loader::HostCallRegistryReady() && !Dispatch<VoidReturn>(gprs.data(), &simd) &&
        Loader::HostCallEffectsFor(reinterpret_cast<std::uint64_t>(VoidReturn)) == Lumen::GuestCpu::EffectMask(Effect::Unknown) &&
        calls == poisoned_calls;
    conflict.effects = Effect::Unknown;
    conflict.result = Loader::HostScalar::Gp;
    ok &= !Loader::RegisterHostCall(reinterpret_cast<std::uint64_t>(VoidReturn), conflict,
                                   "conflicting-cached-HLE-signature") &&
        !Loader::RegisterHostFunction<Gp8>("invalid-light-spill", Effect::None) &&
        !Loader::RegisterHostFunction<FloatResult>("invalid-light-fp", Effect::None) &&
        !Loader::RegisterHostFunction<Unsupported>("invalid-light-aggregate", Effect::None) &&
        !Loader::RegisterHostFunction<VoidReturn>("invalid-effect-mask", static_cast<Effect>(1U << 20)) &&
        !Loader::RegisterHostFunction<Gp8>("invalid-memory-short-spill", Effect::GuestMemory, Contract::SynchronousGuestMemory) &&
        !Loader::RegisterHostFunction<FloatResult>("invalid-memory-short-fp", Effect::GuestMemory, Contract::SynchronousGuestMemory) &&
        !Loader::RegisterHostFunction<VoidReturn>("invalid-memory-short-unknown", Effect::Unknown, Contract::SynchronousGuestMemory) &&
        !Loader::RegisterHostFunction<VoidReturn>("invalid-memory-short-none", Effect::None, Contract::SynchronousGuestMemory) &&
        !Loader::RegisterHostFunction<VoidReturn>("invalid-memory-short-blocking", Effect::GuestMemory | Effect::Blocking,
            Contract::SynchronousGuestMemory) &&
        !Loader::RegisterHostFunction<CacheTarget<65>>("conflicting-memory-short-contract", Effect::GuestMemory, Contract::Ordinary) &&
        Lumen::GuestCpu::AdmissionPolicyContract(Loader::HostCallAdmissionPolicyFor(contract_target)) == Contract::Ordinary;
    std::printf("PS5 typed HLE: %s; mixed GP/SSE, stack spills, explicit INTEGER aggregate, variadic overflow, concurrent metadata publication, cached authority revocation, admission negatives\n",
                ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
