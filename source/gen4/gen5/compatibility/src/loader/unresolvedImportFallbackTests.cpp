// Lumen, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#include "loader/unresolvedImportFallback.h"

#if defined(LUMEN_PS5_FEXCORE)
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "loader/guestExecution.h"
#include "loader/hostCall.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace Loader {
namespace {
namespace Memory = Libs::LibKernel::Memory;
namespace Vm = Common::VirtualMemory;

std::uint64_t KnownScalarFunction(std::uint64_t value) { return value; }
struct UnsupportedAggregate { std::uint64_t words[2]; };
UnsupportedAggregate KnownUnsupportedFunction() { return {}; }

void Check(const char* name, bool condition, bool& passed) {
    std::printf("PS5_IMPORT_FALLBACK_TEST case=%s result=%s\n", name,
                condition ? "PASS" : "FAIL");
    passed &= condition;
}

bool ReadOnlyExecutable(std::uint64_t address) {
    std::uint64_t base {}, size {};
    std::uint32_t writable {};
    const std::uint8_t forbidden_write = 0xcc;
    return Memory::QueryGuestExecutable(nullptr, address, &base, &size, &writable) == 1 &&
        // Adjacent executable Runtime mappings may share one rights extent.
        base <= address && address - base <= size &&
        0x4000 <= size - (address - base) && writable == 0 &&
        !Memory::WriteGuestMemory(nullptr, address, &forbidden_write, 1);
}
} // namespace

bool UnresolvedFallbackSelfTest(std::uint64_t stack_top) noexcept {
    bool passed = true;
    const bool known_registered = RegisterHostFunction<KnownScalarFunction>(
        "fallback-fixture-known-scalar") &&
        RegisterHostFunction<KnownUnsupportedFunction>("fallback-fixture-known-unsupported");
    SymbolDatabase symbols;
    SymbolResolve known {};
    known.name = "known-function";
    known.library = "fixture";
    known.library_version = 1;
    known.module = "fixture";
    known.module_version_major = 1;
    known.module_version_minor = 1;
    known.type = SymbolType::Func;
    const auto known_target = reinterpret_cast<std::uint64_t>(KnownScalarFunction);
    symbols.Add(known, known_target);
    auto import = known;
    import.type = FunctionImportLookupType(SymbolType::NoType, true);
    const auto* callable = symbols.Find(import);
    Check("notype-jump-slot-binds-known-function", known_registered && callable != nullptr &&
          callable->vaddr == known_target && IsHostCallTarget(callable->vaddr) &&
          IsHostCallSupported(callable->vaddr), passed);
    import.type = FunctionImportLookupType(SymbolType::NoType, false);
    Check("notype-data-does-not-bind-function", import.type == SymbolType::NoType &&
          symbols.Find(import) == nullptr, passed);
    known.name = "known-unsupported-function";
    const auto unsupported_target = reinterpret_cast<std::uint64_t>(KnownUnsupportedFunction);
    symbols.Add(known, unsupported_target);
    import = known;
    import.type = FunctionImportLookupType(SymbolType::NoType, true);
    const auto* unsupported = symbols.Find(import);
    Check("notype-call-keeps-known-unsupported-abi-rejection", known_registered &&
          unsupported != nullptr && unsupported->vaddr == unsupported_target &&
          import.type == SymbolType::Func && IsHostCallTarget(unsupported->vaddr) &&
          !IsHostCallSupported(unsupported->vaddr), passed);
    Check("strict-rejects-function", !FunctionFallbackEligible(false, SymbolType::Func,
          false, true, 0), passed);
    Check("function-explicitly-admitted", FunctionFallbackEligible(true, SymbolType::Func,
          false, false, 0), passed);
    Check("notype-jump-slot-admitted", FunctionFallbackEligible(true, SymbolType::NoType,
          false, true, 0), passed);
    Check("object-notype-data-weak-and-addend-rejected",
          !FunctionFallbackEligible(true, SymbolType::Object, false, true, 0) &&
          !FunctionFallbackEligible(true, SymbolType::NoType, false, false, 0) &&
          !FunctionFallbackEligible(true, SymbolType::Func, true, true, 0) &&
          !FunctionFallbackEligible(true, SymbolType::Func, false, false, 1) &&
          !FunctionFallbackEligible(true, SymbolType::Func, false, false, -1), passed);

    // These keys identify test records, not addresses patched in a ROM.
    constexpr std::int32_t owner = 0;
    constexpr std::int32_t other_owner = 2;
    constexpr std::uint64_t patch = 1;
    const auto thunk = RegisterFunctionFallback(owner, patch, "self-test-missing-function",
                                                "synthetic", 0, true);
    const auto duplicate = RegisterFunctionFallback(owner, patch, "self-test-missing-function",
                                                    "synthetic", 0, true);
    const auto other_thunk = RegisterFunctionFallback(other_owner, patch,
        "self-test-missing-function", "synthetic-other-owner", 0, true);
    Check("deduplicated-owner-key", thunk != 0 && thunk == duplicate &&
          other_thunk != 0 && other_thunk != thunk, passed);
    Check("fallback-thunks-are-rx", ReadOnlyExecutable(thunk) &&
          ReadOnlyExecutable(other_thunk), passed);

    const auto code = Memory::AllocateProgramMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                    "fallback-abi-test-code");
    const auto witness = Memory::AllocateRuntimeMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                       "fallback-abi-test-witness");
    if (code == 0 || witness == 0) {
        if (code != 0) Memory::FreeGuestMemory(code, 0x4000);
        if (witness != 0) Memory::FreeGuestMemory(witness, 0x4000);
        ReleaseFunctionFallbacks(other_owner);
        ReleaseFunctionFallbacks(owner);
        return false;
    }

    // The guest wrapper preserves RBX, seeds both return/adjacent SIMD
    // registers, then calls the actual published thunk. Its owned witness
    // records RAX, all 128 bits of XMM0/XMM1 and RSP around the call.
    std::array<std::uint8_t, 76> bytes {
        0x53,                                      // push rbx (align CALL stack)
        0x48,0xbb,0,0,0,0,0,0,0,0,                 // mov rbx, witness
        0x48,0x89,0x63,0x28,                        // mov [rbx+40], rsp
        0x48,0xb8,0,0,0,0,0,0,0,0,                 // mov rax, XMM0 seed
        0x66,0x48,0x0f,0x6e,0xc0,                   // movq xmm0, rax
        0x48,0xb8,0,0,0,0,0,0,0,0,                 // mov rax, XMM1 seed
        0x66,0x48,0x0f,0x6e,0xc8,                   // movq xmm1, rax
        0x48,0xb8,0,0,0,0,0,0,0,0,                 // mov rax, thunk
        0xff,0xd0,                                 // call rax
        0x48,0x89,0x03,                             // mov [rbx], rax
        0xf3,0x0f,0x7f,0x43,0x08,                   // movdqu [rbx+8], xmm0
        0xf3,0x0f,0x7f,0x4b,0x18,                   // movdqu [rbx+24], xmm1
        0x48,0x89,0x63,0x30,                        // mov [rbx+48], rsp
        0x5b,0xc3,                                 // pop rbx; ret
    };
    constexpr std::uint64_t xmm0_seed = 0x1122334455667788ULL;
    constexpr std::uint64_t xmm1_seed = 0x8877665544332211ULL;
    std::memcpy(bytes.data() + 3, &witness, 8);
    std::memcpy(bytes.data() + 17, &xmm0_seed, 8);
    std::memcpy(bytes.data() + 32, &xmm1_seed, 8);
    std::memcpy(bytes.data() + 47, &thunk, 8);
    std::array<std::uint64_t, 7> observed;
    observed.fill(UINT64_MAX);
    passed &= Memory::WriteGuestMemory(nullptr, witness, observed.data(), sizeof(observed));
    passed &= Memory::WriteGuestMemory(nullptr, code, bytes.data(), bytes.size());
    Memory::SetProgramMemoryProtection(code, 0x4000, Vm::Mode::ExecuteRead);
    std::uint64_t result = UINT64_MAX;
    const bool invoked = GuestExecution::Invoke(code, nullptr, 0, &result, stack_top);
    const bool read = Memory::ReadGuestMemory(nullptr, witness, observed.data(), sizeof(observed));
    Check("translated-zero-return-simd-and-stack", invoked && result == 0 && read &&
          observed[0] == 0 && observed[1] == 0 && observed[2] == 0 &&
          observed[3] == xmm1_seed && observed[4] == 0 && observed[5] != 0 &&
          observed[5] == observed[6] && (observed[5] & 15) == 0, passed);
    const bool invoked_again = GuestExecution::Invoke(code, nullptr, 0, &result, stack_top);
    const bool other_invoked = GuestExecution::Invoke(other_thunk, nullptr, 0, &result, stack_top);
    Check("per-owner-real-call-counts", invoked_again && other_invoked && result == 0 &&
          FunctionFallbackCallCount(owner, patch) == 2 &&
          FunctionFallbackCallCount(other_owner, patch) == 1, passed);

    ReleaseFunctionFallbacks(other_owner);
    Check("retirement-does-not-remove-other-owner",
          !GuestExecution::IsGuestExecutable(other_thunk) &&
          FunctionFallbackCallCount(other_owner, patch) == 0 &&
          GuestExecution::IsGuestExecutable(thunk), passed);
    ReleaseFunctionFallbacks(owner);
    Check("retired-thunk-no-longer-executable", !GuestExecution::IsGuestExecutable(thunk) &&
          FunctionFallbackCallCount(owner, patch) == 0, passed);
    passed &= Memory::FreeGuestMemory(code, 0x4000);
    passed &= Memory::FreeGuestMemory(witness, 0x4000);
    return passed;
}
} // namespace Loader
#endif
