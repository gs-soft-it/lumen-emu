// SPDX-License-Identifier: GPL-2.0-only
#include "DirectPerformanceOptions.h"
#include "common/hostException.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <array>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace {
using namespace Libs::Graphics::ShaderRecompiler::IR;
unsigned checks = 0;

void Check(bool result, const char* description) {
    ++checks;
    if (!result) throw std::runtime_error(description);
}

struct Fixture {
    ResourcePlan plan;

    Fixture() {
        plan.resource_tracking_complete = true;
        plan.srt_plan_complete = true;
        plan.user_data_base = 8;
    }

    Value Emit(ValueOpcode opcode, std::initializer_list<Value> arguments) {
        auto& inst = plan.value_storage.emplace_back(opcode);
        size_t index = 0;
        for (const auto argument: arguments) inst.SetArg(index++, argument);
        return Value(&inst);
    }

    Value Root(uint32_t offset = 0) {
        const auto low = Emit(ValueOpcode::GetUserData,
                              {Value(static_cast<ScalarReg>(plan.user_data_base + offset))});
        const auto high = Emit(ValueOpcode::GetUserData,
                               {Value(static_cast<ScalarReg>(plan.user_data_base + offset + 1))});
        return Emit(ValueOpcode::GetAddressResource, {low, high});
    }

    Value Read(Value address, int32_t immediate, Value offset = Value(uint32_t{0})) {
        MemoryInfo memory;
        memory.kind = ResourceKind::ScalarAddress;
        memory.offset = static_cast<uint32_t>(immediate);
        const auto index = static_cast<uint32_t>(plan.memory_info.size());
        plan.memory_info.push_back(memory);
        auto read = Emit(ValueOpcode::LoadAddressU32,
                          {address, offset, Value(uint32_t{0}), Value(true)});
        read.ResolveInstruction()->SetFlags(SrtReadFlags{index, 0});
        plan.srt_reads.push_back({read, static_cast<uint32_t>(plan.srt_reads.size())});
        return read;
    }
};

void StorePointer(uint32_t* output, const void* pointer, uint64_t tag = 0) {
    const auto address = reinterpret_cast<uint64_t>(pointer) | tag;
    std::memcpy(output, &address, sizeof(address));
}

struct Reader {
    unsigned calls = 0;
    bool deny = false;
    static bool Read(void* opaque, uint64_t address, std::span<uint32_t> output) {
        auto& reader = *static_cast<Reader*>(opaque);
        ++reader.calls;
        if (reader.deny) return false;
        std::memcpy(output.data(), reinterpret_cast<const void*>(address), output.size_bytes());
        return true;
    }
};

void TestNativeAndFallback(bool enabled) {
    std::vector<uint32_t> parent(6000, 0x11223344);
    alignas(8) std::array<uint32_t, 4> child{0xaabbccdd, 0x87654321, 0, 0};
    alignas(8) std::array<uint32_t, 2> other{0xfedcba98, 0};
    StorePointer(parent.data() + 4, child.data(), 3);
    parent[3] = 0x12345678;
    parent[5004] = 0x76543210;
    alignas(8) std::array<uint32_t, 4> user{};
    StorePointer(user.data(), parent.data() + 4, uint64_t{0xabcd} << 48 | 3);
    StorePointer(user.data() + 2, other.data());

    Fixture fixture;
    const auto root = fixture.Root();
    const auto first = fixture.Read(root, -4);
    const auto lo = fixture.Read(root, 0);
    const auto hi = fixture.Read(root, 4);
    const auto nested = fixture.Emit(ValueOpcode::GetAddressResource, {lo, hi});
    fixture.Read(nested, 4);
    fixture.Read(root, 20000); // exceeds the native scaled-immediate load range
    fixture.Read(fixture.Root(2), 0);
    CompileSrtWalker(fixture.plan);
    Check(bool(fixture.plan.srt_arm64_code) == enabled, "SRT selector/native compilation");

    std::vector<uint32_t> expected, actual;
    Reader reader;
    SrtRuntime checked{.user_data = user, .read_memory = Reader::Read, .userdata = &reader};
    Check(SrtWalker(fixture.plan, checked).RefreshFlatBuffer(expected), "checked reference read");
    Check(reader.calls == 6, "custom reader was bypassed or duplicate reads lost memoization");
    Check(SrtWalker(fixture.plan, {.user_data = user}).RefreshFlatBuffer(actual) && actual == expected,
          "native and evaluator differ for nested/multiple roots, signed/large offsets and masks");
    Check(actual[0] == 0x12345678 && actual[3] == child[1] && actual[4] == parent[5004] &&
          actual[5] == other[0], "resource values differ from independent source oracle");

    {
        SrtWalker walker(fixture.plan, {.user_data = user});
        Check(walker.RefreshFlatBuffer(actual), "native memo refresh");
        parent[3] = 0x2468abcd;
        uint32_t memo = 0;
        Check(walker.Evaluate(first, memo) && memo == 0x12345678, "native read did not seed memo");
    }
    Check(SrtWalker(fixture.plan, {.user_data = user}).RefreshFlatBuffer(actual) &&
          actual[0] == 0x2468abcd, "new session reused stale memo");
    reader.deny = true;
    Check(!SrtWalker(fixture.plan, checked).RefreshFlatBuffer(actual), "denied checked read bypassed");
    Check(!SrtWalker(fixture.plan, {.user_data = std::span(user).first(1)}).RefreshFlatBuffer(actual),
          "short user data accepted");
    auto moved = std::move(fixture.plan);
    Check(SrtWalker(moved, {.user_data = user}).RefreshFlatBuffer(actual) && actual[0] == parent[3],
          "moving resource plan lost native code lifetime");
}

void TestBounds(bool enabled) {
    for (const int32_t offset: {-4, 4}) {
        Fixture fixture;
        fixture.Read(fixture.Root(), offset);
        CompileSrtWalker(fixture.plan);
        Check(bool(fixture.plan.srt_arm64_code) == enabled, "bounds plan was not compiled");
        const uint64_t pointer = offset < 0 ? 0 : 0x0000fffffffffffcull;
        std::array<uint32_t, 2> user{static_cast<uint32_t>(pointer), static_cast<uint32_t>(pointer >> 32)};
        std::vector<uint32_t> flat;
        Check(!SrtWalker(fixture.plan, {.user_data = user}).RefreshFlatBuffer(flat),
              "overflow/underflow must fail before dereferencing");
    }
}

void TestEligibility() {
    for (unsigned kind = 0; kind < 4; ++kind) {
        Fixture fixture;
        const auto offset = kind == 0 ? fixture.Emit(ValueOpcode::GetUserData,
            {Value(static_cast<ScalarReg>(10))}) : Value(uint32_t{0});
        fixture.Read(fixture.Root(), 0, offset);
        if (kind == 1) fixture.plan.capture_specialization_reads = true;
        if (kind == 2) fixture.plan.control_flow.emplace_back();
        if (kind == 3) fixture.plan.resource_tracking_complete = false;
        CompileSrtWalker(fixture.plan);
        Check(!fixture.plan.srt_arm64_code, "unsupported plan incorrectly selected native path");
    }
    Fixture dynamic;
    dynamic.Read(dynamic.Root(), 0, dynamic.Emit(ValueOpcode::GetUserData,
                  {Value(static_cast<ScalarReg>(10))}));
    CompileSrtWalker(dynamic.plan);
    alignas(8) std::array<uint32_t, 2> table{7, 91};
    std::array<uint32_t, 3> user{};
    StorePointer(user.data(), table.data());
    user[2] = 4;
    std::vector<uint32_t> flat;
    Check(SrtWalker(dynamic.plan, {.user_data = user}).RefreshFlatBuffer(flat) && flat[0] == 91,
          "unsupported dynamic offset lost portable fallback");
}

void* watched = MAP_FAILED;
size_t watched_size = 0;
volatile sig_atomic_t fault_count = 0;
bool RepairFault(const Common::HostException::ExceptionInfo& info) {
    using namespace Common::HostException;
    if (info.type != ExceptionType::AccessViolation ||
        info.access_violation_type != AccessViolationType::Read ||
        info.access_violation_vaddr != reinterpret_cast<uint64_t>(watched)) return false;
    fault_count = fault_count + 1;
    return ::mprotect(watched, watched_size, PROT_READ) == 0;
}

void TestFaultRetry(bool enabled) {
    const auto page = ::sysconf(_SC_PAGESIZE);
    Check(page > 0, "host page size");
    watched_size = static_cast<size_t>(page);
    watched = ::mmap(nullptr, watched_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(watched != MAP_FAILED, "fault backing allocation");
    *static_cast<uint32_t*>(watched) = 0xfeedbeef;
    Fixture fixture;
    fixture.Read(fixture.Root(), 0);
    CompileSrtWalker(fixture.plan);
    Check(bool(fixture.plan.srt_arm64_code) == enabled, "fault plan was not compiled");
    std::array<uint32_t, 2> user{};
    StorePointer(user.data(), watched);
    Check(Common::HostException::InitializeThreadSignalStack() &&
          Common::HostException::InstallHandler(RepairFault), "production host fault handler");
    Check(::mprotect(watched, watched_size, PROT_NONE) == 0, "protect read source");
    std::vector<uint32_t> flat;
    Check(SrtWalker(fixture.plan, {.user_data = user}).RefreshFlatBuffer(flat) &&
          flat[0] == 0xfeedbeef && fault_count == 1, "native fault repair/retry");
    Check(::munmap(watched, watched_size) == 0, "release fault backing");
}
} // namespace

int main() {
    try {
        const auto& options = Lumen::Ps5::DirectPerformance::Get();
        Check(options.valid, "malformed performance selector");
        TestNativeAndFallback(options.srt_arm64);
        TestBounds(options.srt_arm64);
        TestEligibility();
        TestFaultRetry(options.srt_arm64);
        std::printf("PS5_DIRECT_SRT_ARM64_V1 enabled=%u assertions=%u result=PASS "
                    "native_codegen=%u checked_reader_preserved=1 host_fault_retry=1 "
                    "renderer_readback_qualified=0\n", options.srt_arm64, checks, options.srt_arm64);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "PS5_DIRECT_SRT_ARM64_V1 assertion=%u failed=%s\n", checks, error.what());
        return 1;
    }
}
