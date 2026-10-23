// SPDX-License-Identifier: GPL-2.0-only
// Host correctness test for the production typed dispatcher and indexed table.
// The translator entry is replaced by an explicit scoped test harness; real
// translated ABI/callback coverage remains in DirectServicesTests/DirectFexSmoke.
#include "loader/hostCall.h"
#include "DirectFexRuntime.h"
#include "DirectFexHostCallTable.h"
#include "DirectPerformanceOptions.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace {
namespace Direct = Lumen::Ps5::DirectFex;
std::mutex bindings_mutex;
std::unordered_map<std::uint64_t, Direct::HostTargetRecord> bindings;
thread_local const Direct::HostTargetRecord* active {};
std::uint64_t rejected_target {};
std::atomic<unsigned> binding_hits {};

void Check(bool success, const char* name) {
    if (!success) { std::fprintf(stderr, "DIRECT_HLE_DISPATCH_TEST FAIL %s\n", name); std::abort(); }
}

bool Read(void*, std::uint64_t address, void* output, std::size_t size) noexcept {
    if (!address || !output) return false;
    std::memcpy(output, reinterpret_cast<const void*>(address), size);
    return true;
}

bool Dispatch(std::uint64_t target, std::uint64_t* gprs, LumenAndroidFexCoreSimdViewV2* simd) {
    const Direct::HostTargetRecord* record {};
    {
        std::lock_guard lock(bindings_mutex);
        if (const auto found = bindings.find(target); found != bindings.end()) record = &found->second;
    }
    const auto* previous = active;
    active = record;
    const auto result = Loader::DispatchHostCall(target, gprs, simd, Read, nullptr);
    active = previous;
    return result;
}

std::uint64_t Sum8(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                   std::uint64_t e, std::uint64_t f, std::uint64_t g, std::uint64_t h) {
    return a + b + c + d + e + f + g + h;
}
double Mixed(std::uint64_t a, double b, float c) { return a + b + c; }
std::uint64_t Late(std::uint64_t value) { return value + 1; }
std::uint64_t Outer(std::uint64_t value) {
    const auto target = reinterpret_cast<std::uint64_t>(&Outer);
    const auto* previous = Direct::ActiveHostTargetBinding(target);
    Check(Direct::ActiveHostTargetBinding(reinterpret_cast<std::uint64_t>(&Late)) == nullptr,
          "active binding never matches a different target");
    std::uint64_t gprs[16] {};
    gprs[7] = value;
    Check(Dispatch(reinterpret_cast<std::uint64_t>(&Late), gprs, nullptr), "nested typed dispatch");
    Check(Direct::ActiveHostTargetBinding(target) == previous, "parent binding restored");
    return gprs[0] + 1;
}
std::uint64_t Wrong(const std::uint64_t*) { return 99; }

void TableCases() {
    auto table = std::make_unique<Direct::HostCallTable>();
    int binding {}, other_binding {};
    Direct::HostTargetRecord first {0x1000, 0x2000, nullptr};
    Direct::HostTargetRecord replacement {0x3000, 0x4000, nullptr};
    Check(!table->Find(0) && !table->Find(UINT64_MAX) && !table->Find(1), "invalid and unpublished IDs denied");
    Check(!table->Publish(0, &first) && !table->Publish(Direct::HostCallTable::Capacity, &first) &&
          !table->Publish(1, nullptr), "invalid publication denied");
    Check(table->Publish(1, &first) && table->Publish(1, &first) &&
          !table->Publish(1, &replacement) && table->Find(1) == &first, "ID publication is immutable");
    Check(first.Bind(&binding) && first.Bind(&binding) && !first.Bind(&other_binding) &&
          first.binding.load(std::memory_order_acquire) == &binding, "late binding cannot be replaced");
    Check(table->Publish(Direct::HostCallTable::Capacity - 1, &replacement) &&
          table->Find(Direct::HostCallTable::Capacity - 1) == &replacement, "last valid ID accepted");

    constexpr unsigned Count = 128;
    std::array<std::unique_ptr<Direct::HostTargetRecord>, Count> records;
    std::atomic<bool> finished {};
    std::thread reader([&] {
        do {
            for (unsigned i = 0; i < Count; ++i) {
                if (const auto* record = table->Find(i + 2))
                    Check(record->target == i + 100 && record->address == i + 200,
                          "concurrent reader sees a fully initialized record");
            }
        } while (!finished.load(std::memory_order_acquire));
    });
    for (unsigned i = 0; i < Count; ++i) {
        records[i] = std::make_unique<Direct::HostTargetRecord>(i + 100, i + 200, nullptr);
        Check(table->Publish(i + 2, records[i].get()), "late concurrent table publication");
    }
    finished.store(true, std::memory_order_release);
    reader.join();
}
} // namespace

namespace Lumen::Ps5::DirectFex {
std::uint32_t Probe(LumenAndroidFexCoreRuntimeStatusV1*) noexcept { return 0; }
bool RegisterHostTarget(std::uint64_t target, const void* binding) noexcept {
    std::lock_guard lock(bindings_mutex);
    if (target == rejected_target) return false;
    const auto [entry, inserted] = bindings.try_emplace(target, target, target, binding);
    return inserted || entry->second.Bind(binding);
}
const void* ActiveHostTargetBinding(std::uint64_t target) noexcept {
    if (!DirectPerformance::Get().hle_indexed_dispatch || !active || active->target != target) return nullptr;
    const auto* result = active->binding.load(std::memory_order_acquire);
    if (result) binding_hits.fetch_add(1, std::memory_order_relaxed);
    return result;
}
} // namespace Lumen::Ps5::DirectFex

int main(int argc, char** argv) {
    TableCases();
    Check(Lumen::Ps5::DirectPerformance::Get().valid, "valid opt-in configuration");
    const auto sum = reinterpret_cast<std::uint64_t>(&Sum8);
    Check(Direct::RegisterHostTarget(sum), "untyped registration precedes typed binding");
    Check(Loader::RegisterHostFunction<Sum8>("sum") && Loader::RegisterHostFunction<Sum8>("sum alias"),
          "late typed binding and duplicate registration");
    Check(Loader::RegisterHostFunction<Mixed>("mixed") && Loader::RegisterHostFunction<Outer>("outer"),
          "typed registry setup");
    std::uint64_t gprs[16] {};
    std::uint64_t stack[] {0, 7, 8};
    std::uint8_t xmm[16 * 16] {};
    LumenAndroidFexCoreSimdViewV2 simd {sizeof(simd),16,16,16,xmm};
    gprs[4] = reinterpret_cast<std::uint64_t>(stack);
    constexpr unsigned registers[] {7,6,2,1,8,9};
    for (unsigned i = 0; i < 6; ++i) gprs[registers[i]] = i + 1;
    Check(Dispatch(sum,gprs,&simd) && gprs[0] == 36, "integer register and stack ABI");
    const double d = 1.25;
    const float f = 3.5f;
    std::memcpy(xmm,&d,sizeof(d)); std::memcpy(xmm+16,&f,sizeof(f)); gprs[7] = 2;
    Check(Dispatch(reinterpret_cast<std::uint64_t>(&Mixed),gprs,&simd), "mixed SSE ABI");
    double result {}; std::memcpy(&result,xmm,sizeof(result));
    Check(result == 6.75, "floating result");
    Check(!Dispatch(reinterpret_cast<std::uint64_t>(&Mixed),gprs,nullptr), "missing SIMD denied");

    // Registration after live dispatch must not invalidate earlier nodes.
    Check(Loader::RegisterHostFunction<Late>("late"), "late registry extension");
    gprs[7] = 40;
    Check(Dispatch(reinterpret_cast<std::uint64_t>(&Outer),gprs,&simd) && gprs[0] == 42,
          "nested callback binding restoration");
    Check(Direct::ActiveHostTargetBinding(sum) == nullptr, "binding does not escape dispatch scope");
    Check(!Dispatch(0xfedcba9876543210ULL,gprs,&simd), "unknown target denied");
    Check((binding_hits.load() != 0) == Lumen::Ps5::DirectPerformance::Get().hle_indexed_dispatch,
          "indexed path used only when enabled");

    auto metadata = Loader::HostCallDetail::Signature<decltype(&Sum8)>::Describe();
    metadata.invoke = Wrong;
    if (argc > 1 && std::strcmp(argv[1],"publication-failure") == 0) {
        rejected_target = 0xffee;
        Check(!Loader::RegisterHostCall(rejected_target,metadata,"rejected veneer"), "failed bridge publication denied");
        Check(!bindings.contains(rejected_target), "failed publication has no binding");
    } else {
        metadata.result = Loader::HostScalar::Double;
        Check(!Loader::RegisterHostCall(sum,metadata,"conflicting result ABI"), "late conflicting adapter rejected");
    }
    Check(!Loader::HostCallRegistryReady() && !Dispatch(sum,gprs,&simd) &&
          !Loader::RegisterHostFunction<Late>("late alias"), "failure poisons both indexed and fallback dispatch");
    std::puts("DIRECT_HLE_DISPATCH_TEST PASS benchmark=0");
}
