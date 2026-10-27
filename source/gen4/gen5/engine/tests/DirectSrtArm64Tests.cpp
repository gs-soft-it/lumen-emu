// SPDX-License-Identifier: GPL-2.0-only
#include "DirectPerformanceOptions.h"
#include "common/hostException.h"
#include "graphics/shader/recompiler/ir/passes/Arm64SrtCode.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <array>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__aarch64__)
// Independent ABI caller: neither the compiler nor the generated walker can
// spill/reload these canaries around the indirect call and hide a violation.
#if defined(__APPLE__)
#define CHECKED_SRT_ABI_SYMBOL "_lumen_test_checked_srt_abi"
#define CHECKED_SRT_FALSE_SYMBOL "_lumen_test_checked_srt_false_upper_bits"
#define CHECKED_SRT_TRUE_SYMBOL "_lumen_test_checked_srt_true_upper_bits"
#else
#define CHECKED_SRT_ABI_SYMBOL "lumen_test_checked_srt_abi"
#define CHECKED_SRT_FALSE_SYMBOL "lumen_test_checked_srt_false_upper_bits"
#define CHECKED_SRT_TRUE_SYMBOL "lumen_test_checked_srt_true_upper_bits"
#endif
asm(
    ".text\n.p2align 2\n.globl " CHECKED_SRT_ABI_SYMBOL "\n"
    CHECKED_SRT_ABI_SYMBOL ":\n"
    "hint #34\n"
    "stp x29, x30, [sp, #-16]!\nmov x29, sp\n"
    "stp x19, x20, [sp, #-16]!\nstp x21, x22, [sp, #-16]!\n"
    "stp x23, x24, [sp, #-16]!\nstp x25, x26, [sp, #-16]!\n"
    "stp x27, x28, [sp, #-16]!\n"
    "mov x16, x0\nmov x0, x1\nmov x1, x2\nmov x2, x3\n"
    "mov x3, x4\nmov x4, x5\nmov x5, x6\nmov x6, x7\n"
    "mov x19, #0x119\nmov x20, #0x120\nmov x21, #0x121\n"
    "mov x22, #0x122\nmov x23, #0x123\nmov x24, #0x124\n"
    "mov x25, #0x125\nmov x26, #0x126\nmov x27, #0x127\nmov x28, #0x128\n"
    "blr x16\nand x0, x0, #1\n"
    "cmp x19, #0x119\nb.ne 1f\ncmp x20, #0x120\nb.ne 1f\n"
    "cmp x21, #0x121\nb.ne 1f\ncmp x22, #0x122\nb.ne 1f\n"
    "cmp x23, #0x123\nb.ne 1f\ncmp x24, #0x124\nb.ne 1f\n"
    "cmp x25, #0x125\nb.ne 1f\ncmp x26, #0x126\nb.ne 1f\n"
    "cmp x27, #0x127\nb.ne 1f\ncmp x28, #0x128\nb.ne 1f\n"
    "add x9, sp, #80\ncmp x29, x9\nb.ne 1f\norr x0, x0, #2\n"
    "1:\nldp x27, x28, [sp], #16\nldp x25, x26, [sp], #16\n"
    "ldp x23, x24, [sp], #16\nldp x21, x22, [sp], #16\n"
    "ldp x19, x20, [sp], #16\nldp x29, x30, [sp], #16\nret\n"
    // AAPCS64 bool occupies one byte; the other return-register bits need not
    // be zero. Keep the valid bool byte while poisoning the unused upper bits.
    ".p2align 2\n.globl " CHECKED_SRT_FALSE_SYMBOL "\n"
    CHECKED_SRT_FALSE_SYMBOL ":\nhint #34\nmov w0, #0x100\nret\n"
    ".p2align 2\n.globl " CHECKED_SRT_TRUE_SYMBOL "\n"
    CHECKED_SRT_TRUE_SYMBOL ":\nhint #34\nmov w9, #0x77\nstr w9, [x2]\n"
    "mov w0, #0x101\nret\n");
#undef CHECKED_SRT_ABI_SYMBOL
#undef CHECKED_SRT_FALSE_SYMBOL
#undef CHECKED_SRT_TRUE_SYMBOL
extern "C" uint64_t lumen_test_checked_srt_abi(
    Libs::Graphics::ShaderRecompiler::IR::Arm64SrtCode::CheckedWalker,
    const uint32_t*, uint32_t*, void*, uint64_t, size_t,
    Libs::Graphics::ShaderRecompiler::IR::Arm64SrtCode::MemoryReader, void*);
extern "C" bool lumen_test_checked_srt_false_upper_bits(void*, uint64_t, uint32_t*, size_t);
extern "C" bool lumen_test_checked_srt_true_upper_bits(void*, uint64_t, uint32_t*, size_t);
#endif

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

void TestMemoizedRefresh(bool enabled) {
    alignas(8) std::array<uint32_t, 2> first_table{0x11223344u, 0x55667788u};
    alignas(8) std::array<uint32_t, 2> second_table{0xaabbccddu, 0xeeff0011u};
    std::array<uint32_t, 2> user {};
    StorePointer(user.data(), first_table.data());
    Fixture fixture;
    const auto root = fixture.Root();
    const auto first = fixture.Read(root, 0);
    fixture.Read(root, 4);
    DescriptorSource source;
    source.dword_count = 1;
    source.dwords[0] = first;
    fixture.plan.descriptor_sources.push_back(source);
    CompileSrtWalker(fixture.plan);
    Check(bool(fixture.plan.srt_arm64_code) == enabled,
          "memoized refresh retains native eligibility");
    Reader reader;
    SrtRuntime runtime {.user_data = user, .read_memory = Reader::Read, .userdata = &reader};
    DescriptorValue descriptor;
    std::vector<uint32_t> flat;
    {
        SrtWalker walker(fixture.plan, runtime);
        Check(walker.EvaluateDescriptor(0, descriptor) && descriptor.dwords[0] == 0x11223344u &&
              reader.calls == 1, "descriptor-first evaluation reads one scalar");
        first_table[0] = 0x12345678u;
        StorePointer(user.data(), second_table.data());
        Check(walker.RefreshFlatBuffer(flat) &&
              flat == std::vector<uint32_t>{0x11223344u, 0x55667788u} && reader.calls == 2,
              "descriptor-first refresh reuses scalar and root-pointer memos for unread words");
        reader.deny = true;
        Check(walker.RefreshFlatBuffer(flat) &&
              flat == std::vector<uint32_t>{0x11223344u, 0x55667788u} && reader.calls == 2,
              "repeated same-session refresh does not reread memoized scalars");
    }
    reader.deny = false;
    reader.calls = 0;
    {
        SrtWalker walker(fixture.plan, runtime);
        Check(walker.RefreshFlatBuffer(flat) &&
              flat == std::vector<uint32_t>{0xaabbccddu, 0xeeff0011u} && reader.calls == 2,
              "new refresh-first session observes the changed root pointer");
        second_table[0] = 0x87654321u;
        second_table[1] = 0xabcdef01u;
        StorePointer(user.data(), first_table.data());
        Check(walker.RefreshFlatBuffer(flat) &&
              flat == std::vector<uint32_t>{0xaabbccddu, 0xeeff0011u} && reader.calls == 2,
              "native-first repeated refresh retains its scalar snapshot");
        Check(walker.EvaluateDescriptor(0, descriptor) && descriptor.dwords[0] == 0xaabbccddu &&
              reader.calls == 2, "refresh-first descriptor shares the native scalar memo");
    }
    reader.calls = 0;
    Check(SrtWalker(fixture.plan, runtime).RefreshFlatBuffer(flat) &&
          flat == std::vector<uint32_t>{0x12345678u, 0x55667788u} && reader.calls == 2,
          "new generation does not retain either previous session snapshot");
    reader.calls = 0;
    reader.deny = true;
    {
        SrtWalker walker(fixture.plan, runtime);
        Check(!walker.EvaluateDescriptor(0, descriptor) && reader.calls == 1,
              "descriptor-first denied read creates an unknown memo");
        reader.deny = false;
        Check(!walker.RefreshFlatBuffer(flat) && reader.calls == 1,
              "native refresh cannot retry a completed-unknown value within the session");
    }
    Check(SrtWalker(fixture.plan, runtime).RefreshFlatBuffer(flat) &&
          flat == std::vector<uint32_t>{0x12345678u, 0x55667788u} && reader.calls == 3,
          "fresh generation retries an earlier denied scalar read");
}

void TestPartialNativeFailure(bool checked_enabled) {
    if (!checked_enabled) return;
    alignas(8) std::array<uint32_t, 2> first_table{0x11223344u, 0x55667788u};
    alignas(8) std::array<uint32_t, 2> second_table{0xaabbccddu, 0xeeff0011u};
    std::array<uint32_t, 2> user {};
    StorePointer(user.data(), first_table.data());
    Fixture fixture;
    const auto root = fixture.Root();
    const auto first = fixture.Read(root, 0);
    fixture.Read(root, 4);
    CompileSrtWalker(fixture.plan);
    Check(fixture.plan.srt_arm64_code && fixture.plan.srt_arm64_code->CheckedFunction(),
          "partial failure fixture has checked native entry");
    struct PartialReader {
        unsigned calls = 0;
        unsigned deny_at = 2;
        static bool Read(void* opaque, uint64_t address, std::span<uint32_t> output) {
            auto& self = *static_cast<PartialReader*>(opaque);
            if (++self.calls == self.deny_at) return false;
            std::memcpy(output.data(), reinterpret_cast<const void*>(address), output.size_bytes());
            return true;
        }
    } reader;
    SrtRuntime runtime {.user_data = user, .read_memory = PartialReader::Read, .userdata = &reader};
    std::vector<uint32_t> flat;
    {
        SrtWalker walker(fixture.plan, runtime);
        Check(!walker.RefreshFlatBuffer(flat) && reader.calls == 2 && flat[0] == first_table[0],
              "native partial failure occurs after the first scalar was memoized");
        StorePointer(user.data(), second_table.data());
        reader.deny_at = 0;
        Check(!walker.RefreshFlatBuffer(flat) && reader.calls == 2,
              "failed native session cannot retry with a different root pointer");
        uint32_t value = 0xeeeeeeeeu;
        Check(!walker.Evaluate(first, value) && reader.calls == 2 && value == 0xeeeeeeeeu,
              "failed native session cannot expose a partial memo as a complete evaluation");
    }
    Check(SrtWalker(fixture.plan, runtime).RefreshFlatBuffer(flat) && reader.calls == 4 &&
          flat == std::vector<uint32_t>{0xaabbccddu, 0xeeff0011u},
          "fresh session recovers after partial native failure using the new root");
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
#if defined(__aarch64__)
        if (enabled && Lumen::Ps5::DirectPerformance::Get().srt_exact_readback) {
            const auto checked = fixture.plan.srt_arm64_code->CheckedFunction();
            Check(checked != nullptr, "bounds checked entry is present");
            std::vector<ResourcePlan::EvaluationContext::Entry> memo(fixture.plan.evaluation_value_count);
            flat.resize(fixture.plan.srt_reads.size());
            Reader reader;
            const auto thunk = +[](void* opaque, uint64_t address, uint32_t* output, size_t count) {
                return Reader::Read(opaque, address, {output, count});
            };
            Check(lumen_test_checked_srt_abi(checked, user.data(), flat.data(), memo.data(),
                      4, user.size(), thunk, &reader) == 2 && reader.calls == 0,
                  "checked bounds failure precedes callbacks and restores callee registers");
        }
#endif
    }
}

void TestCallbackOnlyPointers(bool enabled) {
    const auto page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    auto* source = static_cast<uint32_t*>(::mmap(nullptr, page, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Check(source != MAP_FAILED, "callback-only watched source");
    struct Snapshot {
        uint64_t base;
        std::array<uint32_t, 8> words {};
        unsigned calls = 0;
        static bool Read(void* opaque, uint64_t address, std::span<uint32_t> output) {
            auto& self = *static_cast<Snapshot*>(opaque);
            if (address < self.base || address - self.base > sizeof(self.words) ||
                output.size_bytes() > sizeof(self.words) - (address - self.base)) return false;
            ++self.calls;
            std::memcpy(output.data(), reinterpret_cast<const uint8_t*>(self.words.data()) +
                address - self.base, output.size_bytes());
            return true;
        }
    } snapshot {reinterpret_cast<uint64_t>(source)};
    StorePointer(snapshot.words.data(), source + 4);
    snapshot.words[4] = 0x12345678;
    snapshot.words[5] = 0x87654321;
    std::array<uint32_t, 3> user {};
    StorePointer(user.data(), source);
    Fixture fixture;
    const auto low = fixture.Read(fixture.Root(), 0);
    const auto high = fixture.Read(fixture.Root(), 4);
    fixture.Read(fixture.Emit(ValueOpcode::GetAddressResource, {low, high}), 0);
    CompileSrtWalker(fixture.plan);
    Check(bool(fixture.plan.srt_arm64_code) == enabled, "callback plan can otherwise use JIT");
    SrtRuntime runtime {.user_data = user, .read_memory = Snapshot::Read, .userdata = &snapshot};
    std::vector<uint32_t> flat;
    Check(SrtWalker(fixture.plan, runtime).RefreshFlatBuffer(flat) && flat.back() == snapshot.words[4] &&
          snapshot.calls == 3, "callback covers nested pointers without touching PROT_NONE view");
    Fixture dynamic;
    dynamic.Read(dynamic.Root(), 0, dynamic.Emit(ValueOpcode::GetUserData,
        {Value(static_cast<ScalarReg>(10))}));
    user[2] = 5 * sizeof(uint32_t);
    Check(SrtWalker(dynamic.plan, runtime).RefreshFlatBuffer(flat) && flat.back() == snapshot.words[5],
          "callback covers dynamic pointer offset");
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    SrtReadCapture capture(runtime, ranges);
    const auto observed = capture.ObservedRuntime();
    std::array<uint32_t, 2> pair {};
    Check(observed.read_memory(observed.userdata, snapshot.base + 4 * sizeof(uint32_t), pair) &&
          pair[0] == snapshot.words[4] && pair[1] == snapshot.words[5] &&
          ranges == std::vector<std::pair<uint64_t, uint64_t>>{{snapshot.base + 16, sizeof(pair)}},
          "captured ordinary callback forwards complete multiword span");
    Check(::munmap(source, page) == 0, "callback-only source retirement");
}

void TestCheckedAbi(bool enabled, bool checked_enabled) {
#if defined(__aarch64__)
    using Entry = ResourcePlan::EvaluationContext::Entry;
    const auto page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    auto* inaccessible = static_cast<uint32_t*>(::mmap(nullptr, page, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Check(inaccessible != MAP_FAILED, "checked ABI inaccessible guest view");
    struct Callback {
        uint64_t base;
        std::array<uint32_t, 8> words {};
        std::array<uint64_t, 8> addresses {};
        unsigned calls = 0;
        unsigned deny_at = 0;
        bool aligned = true;
        bool throw_on_read = false;
        static bool Read(void* opaque, uint64_t address, uint32_t* output, size_t count) {
            auto& self = *static_cast<Callback*>(opaque);
            uintptr_t stack;
            asm volatile("mov %0, sp" : "=r"(stack));
            self.aligned &= (stack & 15u) == 0;
            ++self.calls;
            if (self.calls <= self.addresses.size()) self.addresses[self.calls - 1] = address;
            if (self.throw_on_read) throw std::runtime_error("checked callback exception");
            const bool allowed = self.calls != self.deny_at && count == 1 &&
                address >= self.base && address - self.base < sizeof(self.words) &&
                (address - self.base) % sizeof(uint32_t) == 0;
            if (allowed) *output = self.words[(address - self.base) / sizeof(uint32_t)];
            // Caller-saved state must survive only in the checked walker's own
            // preserved frame/registers. x18 is platform-reserved and untouched.
            asm volatile(
                "mov x0, #0\nmov x1, #1\nmov x2, #2\nmov x3, #3\n"
                "mov x4, #4\nmov x5, #5\nmov x6, #6\nmov x7, #7\n"
                "mov x8, #8\nmov x9, #9\nmov x10, #10\nmov x11, #11\n"
                "mov x12, #12\nmov x13, #13\nmov x14, #14\nmov x15, #15\n"
                "mov x16, #16\nmov x17, #17\ncmp x0, x1\n"
                : : : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8",
                        "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "cc");
            return allowed;
        }
        static bool SpanRead(void* opaque, uint64_t address, std::span<uint32_t> output) {
            return Read(opaque, address, output.data(), output.size());
        }
    } callback {reinterpret_cast<uint64_t>(inaccessible)};
    StorePointer(callback.words.data(), inaccessible + 4, (uint64_t{0xabcd} << 48) | 3u);
    callback.words[4] = 0x89abcdefu;
    callback.words[5] = 0x76543210u;
    std::array<uint32_t, 2> user {};
    StorePointer(user.data(), inaccessible);
    Fixture fixture;
    const auto root = fixture.Root();
    const auto low = fixture.Read(root, 0);
    const auto high = fixture.Read(root, 4);
    const auto child = fixture.Emit(ValueOpcode::GetAddressResource, {low, high});
    const auto leaf = fixture.Read(child, 0);
    fixture.Read(child, 4);
    CompileSrtWalker(fixture.plan);
    Check(bool(fixture.plan.srt_arm64_code) == enabled, "checked ABI plan compilation");
    Check(!enabled || bool(fixture.plan.srt_arm64_code->CheckedFunction()) == checked_enabled,
          "checked native entry follows exact-readback selector");
    if (checked_enabled) {
        const auto checked = fixture.plan.srt_arm64_code->CheckedFunction();
        Check(checked != nullptr, "checked native entry is present");
        constexpr uint64_t generation = 0x1234567890ull;
        std::vector<Entry> memo(fixture.plan.evaluation_value_count);
        std::vector<uint32_t> flat(fixture.plan.srt_reads.size(), 0xeeeeeeeeu);
        const auto invoke = [&](size_t user_count) {
            return lumen_test_checked_srt_abi(checked, user.data(), flat.data(), memo.data(),
                generation, user_count, Callback::Read, &callback);
        };
        Check(invoke(user.size()) == 3 && callback.aligned && callback.calls == 4,
              "checked native call preserves callee registers, frame and callback count");
        Check(callback.addresses[0] == callback.base && callback.addresses[1] == callback.base + 4 &&
              callback.addresses[2] == callback.base + 16 && callback.addresses[3] == callback.base + 20,
              "checked nested pointer comes from parent DWORD memo without raw or duplicate loads");
        Check(flat == std::vector<uint32_t>{callback.words[0], callback.words[1],
                                           0x89abcdefu, 0x76543210u},
              "checked ABI output matches independent byte oracle");
        for (const auto& read : fixture.plan.srt_reads) {
            const auto index = read.value.ResolveInstruction()->EvaluationIndex(fixture.plan.evaluation_value_count);
            Check(memo[index].generation == generation && memo[index].value == flat[read.flat_offset],
                  "checked ABI DWORD value and full generation are committed to memo");
        }
        for (unsigned denied : {1u, 3u, 4u}) {
            callback.calls = 0;
            callback.deny_at = denied;
            std::fill(flat.begin(), flat.end(), 0xeeeeeeeeu);
            std::fill(memo.begin(), memo.end(), Entry {});
            Check(invoke(user.size()) == 2 && callback.calls == denied && callback.aligned,
                  "checked denial unwinds nested frames and preserves callee registers");
            const auto& failed_read = fixture.plan.srt_reads[denied - 1];
            const auto failed_index = failed_read.value.ResolveInstruction()->EvaluationIndex(
                fixture.plan.evaluation_value_count);
            Check(flat[failed_read.flat_offset] == 0xeeeeeeeeu && memo[failed_index].generation == 0,
                  "failed checked read never publishes output or memo generation");
        }
        callback.calls = 0;
        callback.deny_at = 0;
        Check(invoke(1) == 2 && callback.calls == 0, "checked short userdata denies before callback");

        Fixture return_fixture;
        return_fixture.Read(return_fixture.Root(), 0);
        CompileSrtWalker(return_fixture.plan);
        Check(return_fixture.plan.srt_arm64_code && return_fixture.plan.srt_arm64_code->CheckedFunction(),
              "callback bool ABI fixture has checked native entry");
        const auto return_checked = return_fixture.plan.srt_arm64_code->CheckedFunction();
        std::vector<Entry> return_memo(return_fixture.plan.evaluation_value_count);
        uint32_t return_flat = 0xeeeeeeeeu;
        Check(lumen_test_checked_srt_abi(return_checked, user.data(), &return_flat, return_memo.data(),
                  generation, user.size(), lumen_test_checked_srt_false_upper_bits, nullptr) == 2 &&
              return_flat == 0xeeeeeeeeu,
              "checked callback false ignores unspecified upper return-register bits");
        Check(lumen_test_checked_srt_abi(return_checked, user.data(), &return_flat, return_memo.data(),
                  generation, user.size(), lumen_test_checked_srt_true_upper_bits, nullptr) == 3 &&
              return_flat == 0x77,
              "checked callback true accepts valid bool byte with nonzero upper register bits");
        Check(lumen_test_checked_srt_abi(return_checked, user.data(), &return_flat, return_memo.data(),
                  generation, user.size(), nullptr, nullptr) == 2,
              "checked null reader fails through preserved native frame");

        SrtRuntime runtime {.user_data = user, .read_memory = Callback::SpanRead, .userdata = &callback};
        callback.calls = 0;
        {
            SrtWalker walker(fixture.plan, runtime);
            Check(walker.RefreshFlatBuffer(flat), "checked dispatcher refresh");
            callback.words[4] ^= 0xffffffffu;
            uint32_t value = 0;
            Check(walker.Evaluate(leaf, value) && value == 0x89abcdefu && callback.calls == 4,
                  "checked dispatcher seeds same-session memo without another callback");
        }
        Check(SrtWalker(fixture.plan, runtime).RefreshFlatBuffer(flat) && flat[2] == callback.words[4],
              "checked dispatcher refreshes a new session instead of retaining old memo");
        callback.throw_on_read = true;
        Check(!SrtWalker(fixture.plan, runtime).RefreshFlatBuffer(flat),
              "checked thunk contains callback exceptions without unwinding generated code");

        Fixture clean;
        auto strict = clean.Read(clean.Root(), 0);
        auto flags = strict.ResolveInstruction()->Flags<SrtReadFlags>();
        flags.clean = 1;
        strict.ResolveInstruction()->SetFlags(flags);
        CompileSrtWalker(clean.plan);
        Check(!clean.plan.srt_arm64_code || !clean.plan.srt_arm64_code->CheckedFunction(),
              "clean read cannot use ordinary checked callback in place of strict reader");
    }
    Check(::munmap(inaccessible, page) == 0, "checked ABI guest view retirement");
#else
    (void)enabled;
    (void)checked_enabled;
#endif
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
        TestMemoizedRefresh(options.srt_arm64);
        TestPartialNativeFailure(options.srt_arm64 && options.srt_exact_readback);
        TestBounds(options.srt_arm64);
        TestCallbackOnlyPointers(options.srt_arm64);
        TestCheckedAbi(options.srt_arm64, options.srt_arm64 && options.srt_exact_readback);
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
