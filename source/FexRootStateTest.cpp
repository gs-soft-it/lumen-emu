#include "AndroidFexCoreRuntime.h"

#include <array>
#include <atomic>
#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <pthread.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace {
constexpr std::size_t PAGE_SIZE = 16384;
constexpr std::uint64_t FP_RESULT = 0x4000000000000000ULL;
constexpr std::uint64_t CALLBACK_RESULT = 0x8000000000000000ULL;

bool Check(bool condition, const char* message)
{
    if (!condition) std::fprintf(stderr, "FexRootStateTest: %s\n", message);
    return condition;
}

class Mapping {
public:
    explicit Mapping(std::size_t pages = 1) : size(pages * PAGE_SIZE), memory(MAP_FAILED) {
        if (pages == 0 || pages > permissions.size()) return;
        const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (token == 0) return;
        memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (Valid()) {
            permissions.fill(PROT_READ | PROT_WRITE);
            next = first;
            first = this;
        }
        LumenAndroidFexCoreEndGuestMemoryMutationV1(token, Valid() ? Address() : 0, size, Valid());
    }
    ~Mapping() {
        if (!Valid()) return;
        const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (token == 0) std::abort();
        const bool unmapped = munmap(memory, size) == 0;
        auto** entry = &first;
        while (*entry != this) entry = &(*entry)->next;
        *entry = next;
        LumenAndroidFexCoreEndGuestMemoryMutationV1(token, Address(), size, unmapped);
        if (!unmapped) std::abort();
    }
    bool Protect(int mode, std::size_t length = PAGE_SIZE) const {
        if (length == 0 || length > size || length % PAGE_SIZE != 0) return false;
        const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (token == 0) return false;
        const bool changed = mprotect(memory, length, mode) == 0;
        if (changed)
            for (std::size_t page = 0; page < length / PAGE_SIZE; ++page)
                permissions[page] = mode;
        LumenAndroidFexCoreEndGuestMemoryMutationV1(token, Address(), length, changed);
        return changed;
    }
    bool Valid() const { return memory != MAP_FAILED; }
    std::uint64_t Address(std::size_t offset = 0) const {
        return reinterpret_cast<std::uint64_t>(memory) + offset;
    }
    std::uint8_t* Bytes(std::size_t offset = 0) const {
        return reinterpret_cast<std::uint8_t*>(Address(offset));
    }
    static bool Allows(std::uint64_t address, std::uint64_t length, bool write) noexcept {
        if (address == 0 || length == 0 || address > UINT64_MAX - length) return false;
        for (const auto* entry = first; entry != nullptr; entry = entry->next) {
            if (address < entry->Address() || address - entry->Address() >= entry->size ||
                length > entry->size - (address - entry->Address())) continue;
            const int required = PROT_READ | (write ? PROT_WRITE : 0);
            const auto firstPage = (address - entry->Address()) / PAGE_SIZE;
            const auto lastPage = (address + length - 1 - entry->Address()) / PAGE_SIZE;
            for (auto page = firstPage; page <= lastPage; ++page)
                if ((entry->permissions[page] & required) != required) return false;
            return true;
        }
        return false;
    }
    std::size_t size;
    void* memory;
private:
    mutable std::array<int, 2> permissions {};
    Mapping* next {};
    static inline thread_local Mapping* first {};
};

std::uint64_t deniedDataAddress {};
unsigned deniedDataReads {};

std::uint32_t QueryDataAccess(std::uint64_t address, std::uint64_t size,
                              std::uint32_t write) noexcept {
    if (deniedDataAddress != 0 && address == deniedDataAddress && size == 8 && write == 0) {
        ++deniedDataReads;
        return 0;
    }
    return write <= 1 && Mapping::Allows(address, size, write != 0) ? 1U : 0U;
}

class GuestCode {
public:
    void Bytes(std::initializer_list<std::uint8_t> bytes) {
        for (auto byte : bytes) {
            if (size == data.size()) std::abort();
            data[size++] = byte;
        }
    }
    void Word(std::uint64_t word) {
        for (unsigned shift = 0; shift != 64; shift += 8)
            Bytes({static_cast<std::uint8_t>(word >> shift)});
    }
    void Mov(unsigned reg, std::uint64_t word) {
        Bytes({static_cast<std::uint8_t>(reg < 8 ? 0x48 : 0x49),
               static_cast<std::uint8_t>(0xb8 + (reg & 7))});
        Word(word);
    }
    void Vectors(std::uint64_t base, bool store) {
        for (unsigned reg = 0; reg != 16; ++reg) {
            Mov(0, base + reg * 32);
            Bytes({0xc5, static_cast<std::uint8_t>(reg < 8 ? 0xfe : 0x7e),
                   static_cast<std::uint8_t>(store ? 0x7f : 0x6f),
                   static_cast<std::uint8_t>((reg & 7) << 3)});
        }
    }
    bool Publish(const Mapping& map) const {
        std::memcpy(map.memory, data.data(), size);
        return map.Protect(PROT_READ | PROT_EXEC);
    }
private:
    std::array<std::uint8_t, 4096> data {};
    std::size_t size {};
};

struct Context {
    std::uint64_t code;
    unsigned callbacks {};
    bool valid {true};
    std::uint64_t nestedCode {};
    std::uint64_t nestedStack {};
};

void HostTarget() {}

std::uint32_t Query(void* opaque, std::uint64_t address,
                    std::uint64_t* base, std::uint64_t* size,
                    std::uint32_t* writable) noexcept {
    const auto& context = *static_cast<Context*>(opaque);
    if (address < context.code || address - context.code >= PAGE_SIZE) return 0;
    *base = context.code; *size = PAGE_SIZE; *writable = 0;
    return 1;
}

std::uint32_t Callback(void* opaque, std::uint64_t target,
                       std::uint64_t* gprs, LumenAndroidFexCoreSimdViewV2* simd,
                       std::uint64_t* next) noexcept {
    auto& context = *static_cast<Context*>(opaque);
    context.valid = context.valid && target == reinterpret_cast<std::uint64_t>(&HostTarget) &&
        simd != nullptr && simd->registerCount == 16 && simd->registerBytes == 16;
    if (!context.valid) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    ++context.callbacks;
    context.valid = context.valid && std::fegetround() == FE_UPWARD;
    std::fesetround(FE_TOWARDZERO);
    std::feraiseexcept(FE_DIVBYZERO);
    const int nativeExceptions = std::fetestexcept(FE_ALL_EXCEPT);
    LumenAndroidFexCoreGuestStateV3 nested {};
    nested.gp.rip = context.nestedCode;
    nested.gp.gprs[4] = context.nestedStack;
    const auto sentinel = LumenAndroidFexCoreRootReturnAddressV1();
    std::memcpy(reinterpret_cast<void*>(nested.gp.gprs[4]), &sentinel, sizeof(sentinel));
    nested.mxcsr = 0x1f80;
    Context nestedContext {context.nestedCode};
    const auto nestedResult = LumenAndroidFexCoreRunGuestV3(&nested, Callback, Query, &nestedContext);
    context.valid = context.valid && nestedResult == 0 && nested.gp.gprs[0] == 0x515 &&
        std::fegetround() == FE_TOWARDZERO && std::fetestexcept(FE_ALL_EXCEPT) == nativeExceptions;
    std::memcpy(simd->registers, &CALLBACK_RESULT, sizeof(CALLBACK_RESULT));
    std::memcpy(next, reinterpret_cast<void*>(gprs[4]), sizeof(*next));
    gprs[4] += sizeof(*next);
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
}

void Prepare(LumenAndroidFexCoreGuestStateV3* state, const Mapping& code,
             const Mapping& stack) {
    state->gp.rip = code.Address();
    state->gp.gprs[4] = stack.Address(PAGE_SIZE - 8);
    const auto sentinel = LumenAndroidFexCoreRootReturnAddressV1();
    std::memcpy(reinterpret_cast<void*>(state->gp.gprs[4]), &sentinel, sizeof(sentinel));
}

bool Run(bool pairedFlush) {
    Mapping first, second, stack, output, nestedCode, nestedStack;
    if (!Check(first.Valid() && second.Valid() && stack.Valid() && output.Valid() && nestedCode.Valid() && nestedStack.Valid(), "allocation")) return false;
    if (!Check(LumenAndroidFexCoreRegisterHostCallTargetV1(
                   reinterpret_cast<std::uint64_t>(&HostTarget)) == 1, "host target")) return false;
    LumenAndroidFexCoreGuestStateV3 state {};
    for (unsigned reg = 0; reg != 16; ++reg)
        for (unsigned byte = 0; byte != 32; ++byte)
            state.vectorRegisters[reg][byte] = static_cast<std::uint8_t>(reg * 37 + byte * 11 + 3);
    std::array<std::uint8_t, 512> expected {};
    std::memcpy(expected.data(), state.vectorRegisters, expected.size());
    const std::uint32_t guestControl = 0x3f80U | (pairedFlush ? 0x8040U : 0U);
    state.mxcsr = guestControl;
    state.x87ControlWord = 0x0b7f;
    state.gp.gprs[0] = 0x72abc123;
    state.gp.gprs[7] = output.Address();
    state.gp.gprs[10] = 0x1122334455667788;
    state.gp.gprs[11] = 0x8877665544332211;

    GuestCode nested;
    nested.Mov(0, 0x515);
    nested.Bytes({0xc3});
    if (!Check(nested.Publish(nestedCode), "nested code publication")) return false;
    const std::uint64_t negativeFraction = 0xbffc000000000000ULL; // -1.75
    std::memcpy(output.Bytes(1080), &negativeFraction, sizeof(negativeFraction));
    GuestCode firstCode;
    firstCode.Bytes({0x4c, 0x89, 0x97, 0x60, 0x04, 0x00, 0x00}); // [rdi+1120] = r10
    firstCode.Bytes({0x4c, 0x89, 0x9f, 0x68, 0x04, 0x00, 0x00}); // [rdi+1128] = r11
    firstCode.Bytes({0x48, 0x89, 0x87, 0x70, 0x04, 0x00, 0x00}); // [rdi+1136] = rax
    firstCode.Bytes({0x48, 0x89, 0xa7, 0x78, 0x04, 0x00, 0x00}); // [rdi+1144] = rsp
    firstCode.Vectors(output.Address(), true);
    firstCode.Mov(0, output.Address(1024));
    firstCode.Bytes({0x0f, 0xae, 0x18}); // stmxcsr [rax]
    firstCode.Mov(0, output.Address(1028));
    firstCode.Bytes({0xd9, 0x38}); // fnstcw [rax]
    firstCode.Mov(0, output.Address(1080));
    firstCode.Bytes({0xf2, 0x48, 0x0f, 0x2d, 0x00});
    firstCode.Mov(10, output.Address(1072));
    firstCode.Bytes({0x49, 0x89, 0x02});
    firstCode.Bytes({0xd9, 0xe8, 0xd9, 0xe8, 0xde, 0xc1}); // fld1; fld1; faddp
    firstCode.Bytes({0x48, 0x83, 0xec, 0x08});
    firstCode.Mov(0, reinterpret_cast<std::uint64_t>(&HostTarget));
    firstCode.Bytes({0xff, 0xd0, 0x48, 0x83, 0xc4, 0x08});
    firstCode.Mov(0, output.Address(1080));
    firstCode.Bytes({0xf2, 0x48, 0x0f, 0x2d, 0x00}); // cvtsd2si rax, qword [rax]
    firstCode.Mov(10, output.Address(1064));
    firstCode.Bytes({0x49, 0x89, 0x02});
    firstCode.Mov(0, output.Address(1080));
    firstCode.Bytes({0xdd, 0x00}); // fld qword [rax]
    firstCode.Mov(0, output.Address(1056));
    firstCode.Bytes({0xdf, 0x38}); // fistp qword [rax]
    firstCode.Bytes({0xd9, 0xe8, 0xd8, 0xd1, 0xdd, 0xd8, 0xdf, 0xe0}); // compare 1 < 2; pop; fnstsw ax
    firstCode.Mov(10, output.Address(1096));
    firstCode.Bytes({0x66, 0x41, 0x89, 0x02});
    // Exercise every arithmetic flag plus direction through the opaque checkpoint.
    firstCode.Bytes({0x68, 0xd7, 0x0c, 0x00, 0x00, 0x9d, 0xc3});
    if (!Check(firstCode.Publish(first), "first code publication")) return false;
    Context context {first.Address()};
    context.nestedCode = nestedCode.Address();
    context.nestedStack = nestedStack.Address(PAGE_SIZE - 8);
    Prepare(&state, first, stack);
    std::fenv_t originalEnvironment {};
    std::fegetenv(&originalEnvironment);
    std::fesetround(FE_UPWARD);
    std::feclearexcept(FE_ALL_EXCEPT);
    std::feraiseexcept(FE_INVALID | FE_INEXACT);
    const int expectedExceptions = std::fetestexcept(FE_ALL_EXCEPT);
    const auto firstResult = LumenAndroidFexCoreRunGuestV3(&state, Callback, Query, &context);
    const bool hostEnvironmentPreserved = std::fegetround() == FE_UPWARD &&
        std::fetestexcept(FE_ALL_EXCEPT) == expectedExceptions;
    std::fesetenv(&originalEnvironment);
    bool passed = Check(hostEnvironmentPreserved, "host FP environment restored") && Check(firstResult == 0, "first execution") &&
        Check(context.valid && context.callbacks == 1, "host callback") &&
        Check(std::memcmp(output.memory, expected.data(), expected.size()) == 0, "all initial YMM lanes") &&
        Check(*reinterpret_cast<std::uint32_t*>(output.Bytes(1024)) == guestControl, "initial MXCSR") &&
        Check(*reinterpret_cast<std::uint16_t*>(output.Bytes(1028)) == 0x0b7f, "initial x87 control");
    if (*reinterpret_cast<std::int64_t*>(output.Bytes(1072)) != -2 ||
        *reinterpret_cast<std::int64_t*>(output.Bytes(1064)) != -2) {
        std::fprintf(stderr, "FexRootStateTest: SSE rounding before=%lld after=%lld mxcsr=%x\n",
                     static_cast<long long>(*reinterpret_cast<std::int64_t*>(output.Bytes(1072))),
                     static_cast<long long>(*reinterpret_cast<std::int64_t*>(output.Bytes(1064))), state.mxcsr);
    }
    passed = Check(*reinterpret_cast<std::uint64_t*>(output.Bytes(1120)) == 0x1122334455667788 &&
                   *reinterpret_cast<std::uint64_t*>(output.Bytes(1128)) == 0x8877665544332211 &&
                   *reinterpret_cast<std::uint64_t*>(output.Bytes(1136)) == 0x72abc123 &&
                   *reinterpret_cast<std::uint64_t*>(output.Bytes(1144)) == stack.Address(PAGE_SIZE - 8),
                   "bootstrap preserves root scratch registers and RSP") && passed;
    passed = Check(*reinterpret_cast<std::int64_t*>(output.Bytes(1072)) == -2,
                   "initial guest SSE rounding") && passed;
    passed = Check(*reinterpret_cast<std::int64_t*>(output.Bytes(1064)) == -2,
                   "guest SSE rounding after native callback and nested root") && passed;
    passed = Check(*reinterpret_cast<std::int64_t*>(output.Bytes(1056)) == -1,
                   "x87 rounding remains independent of SSE after nested callback") && passed;
    std::memcpy(expected.data(), &CALLBACK_RESULT, sizeof(CALLBACK_RESULT));
    passed = Check(std::memcmp(state.vectorRegisters, expected.data(), expected.size()) == 0,
                   "all root result YMM lanes with low XMM0 callback result") && passed;
    passed = Check(state.x87AbridgedTag != 0, "x87 stack exported") && passed;
    if (!passed) return false;

    GuestCode secondCode;
    secondCode.Bytes({0x9c, 0x58}); // pushfq; pop rax
    secondCode.Mov(10, output.Address(1032));
    secondCode.Bytes({0x49, 0x89, 0x02});
    secondCode.Bytes({0xdf, 0xe0}); // fnstsw ax before changing the restored stack
    secondCode.Mov(10, output.Address(1104));
    secondCode.Bytes({0x66, 0x41, 0x89, 0x02});
    secondCode.Vectors(output.Address(512), true);
    secondCode.Mov(0, output.Address(1040));
    secondCode.Bytes({0xdd, 0x18}); // fstp qword [rax]
    secondCode.Mov(0, output.Address(1048));
    secondCode.Bytes({0x0f, 0xae, 0x18});
    secondCode.Mov(0, output.Address(1052));
    secondCode.Bytes({0xd9, 0x38, 0xc3});
    if (!Check(secondCode.Publish(second), "second code publication")) return false;
    context.code = second.Address();
    Prepare(&state, second, stack);
    const auto secondResult = LumenAndroidFexCoreRunGuestV3(&state, Callback, Query, &context);
    passed = Check(secondResult == 0, "second execution") &&
        Check(std::memcmp(output.Bytes(512), expected.data(), expected.size()) == 0, "YMM checkpoint restore") &&
        Check((*reinterpret_cast<std::uint64_t*>(output.Bytes(1032)) & 0xcd5) == 0xcd5,
              "arithmetic and direction flags checkpoint restore") &&
        Check(*reinterpret_cast<std::uint64_t*>(output.Bytes(1040)) == FP_RESULT, "x87 value checkpoint restore") &&
        Check(*reinterpret_cast<std::uint32_t*>(output.Bytes(1048)) == state.mxcsr && (state.mxcsr & 0xffc0U) == guestControl, "MXCSR checkpoint restore") &&
        Check(*reinterpret_cast<std::uint16_t*>(output.Bytes(1052)) == 0x0b7f, "x87 control checkpoint restore");
    passed = Check(state.x87AbridgedTag == 0, "x87 pop exported") && passed;
    const auto originalX87Status = *reinterpret_cast<std::uint16_t*>(output.Bytes(1096));
    passed = Check((originalX87Status & 0x100) != 0 &&
                   originalX87Status == *reinterpret_cast<std::uint16_t*>(output.Bytes(1104)),
                   "x87 status and stack top checkpoint restore") && passed;

    const auto before = state;
    state.abiVersion = 2;
    passed = Check(LumenAndroidFexCoreRunGuestV3(&state, Callback, Query, &context) ==
                   LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1, "wrong ABI rejected") && passed;
    state = before;
    state.mxcsr |= 0x10000;
    passed = Check(LumenAndroidFexCoreRunGuestV3(&state, Callback, Query, &context) ==
                   LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1, "reserved MXCSR rejected") && passed;
    passed = Check(context.callbacks == 1, "rejected state did not execute") && passed;
    return passed;
}
bool RootDenormControls() {
    Mapping code, stack;
    if (!Check(code.Valid() && stack.Valid(), "denorm allocations")) return false;
    GuestCode instructions;
    instructions.Bytes({0xf2, 0x0f, 0x59, 0xc1, 0xc3}); // mulsd xmm0, xmm1; ret
    if (!Check(instructions.Publish(code), "denorm publication")) return false;
    struct Case { std::uint32_t mxcsr; std::uint64_t input, factor, expected; };
    constexpr Case cases[] {
        {0x1f80, 0x1, 0x4000000000000000, 0x2},
        {0x1fc0, 0x1, 0x4000000000000000, 0x0},
        {0x1f80, 0x0010000000000000, 0x3fe0000000000000, 0x0008000000000000},
        {0x9f80, 0x0010000000000000, 0x3fe0000000000000, 0x0},
        {0x9f80, 0x000fffffffffffff, 0x4000000000000000, 0x001ffffffffffffe},
        {0x9fc0, 0x1, 0x4000000000000000, 0x0},
        {0x9fc0, 0x8000000000000001, 0x4000000000000000, 0x8000000000000000},
        {0x9fc0, 0x0010000000000000, 0x3fe0000000000000, 0x0},
        {0x9fc0, 0x8010000000000000, 0x3fe0000000000000, 0x8000000000000000},
        {0x9fc0, 0x3ff0000000000000, 0x4000000000000000, 0x4000000000000000},
    };
    bool passed = true;
    for (const auto& test : cases) {
        LumenAndroidFexCoreGuestStateV3 state {};
        state.mxcsr = test.mxcsr;
        std::memcpy(state.vectorRegisters[0], &test.input, sizeof(test.input));
        std::memcpy(state.vectorRegisters[1], &test.factor, sizeof(test.factor));
        Prepare(&state, code, stack);
        Context context {code.Address()};
        const auto result = LumenAndroidFexCoreRunGuestV3(&state, Callback, Query, &context);
        std::uint64_t actual {};
        std::memcpy(&actual, state.vectorRegisters[0], sizeof(actual));
        if (result != 0 || actual != test.expected) {
            std::fprintf(stderr, "FexRootStateTest: denorm mxcsr=%x result=%u bits=%llx expected=%llx\n",
                         test.mxcsr, result, static_cast<unsigned long long>(actual),
                         static_cast<unsigned long long>(test.expected));
            passed = false;
        }
    }
    return passed;
}

// Original numerical gate: all four controls are changed inside admitted guest
// code. The explicit qualification option remains accepted for existing scripts.
bool DynamicDenormControls() {
    struct Shape {
        const char* name;
        std::initializer_list<std::uint8_t> multiply;
        unsigned elementBytes, activeBytes;
    };
    const Shape shapes[] {
        {"mulss", {0xf3, 0x0f, 0x59, 0xc1}, 4, 4},
        {"mulsd", {0xf2, 0x0f, 0x59, 0xc1}, 8, 8},
        {"mulps", {0x0f, 0x59, 0xc1}, 4, 16},
        {"mulpd", {0x66, 0x0f, 0x59, 0xc1}, 8, 16},
        {"vmulps256", {0xc5, 0xfc, 0x59, 0xc1}, 4, 32},
        {"vmulpd256", {0xc5, 0xfd, 0x59, 0xc1}, 8, 32},
    };
    struct Numbers {
        std::uint64_t input, factor, gradual;
        bool subnormalInput, subnormalOutput;
    };
    constexpr Numbers single[] {
        {1, 0x40000000, 2, true, true},
        {0x007fffff, 0x40000000, 0x00fffffe, true, false},
        {0x00800000, 0x3f000000, 0x00400000, false, true},
        {0x3f800000, 0x40000000, 0x40000000, false, false},
    };
    constexpr Numbers dual[] {
        {1, 0x4000000000000000, 2, true, true},
        {0x000fffffffffffff, 0x4000000000000000, 0x001ffffffffffffe, true, false},
        {0x0010000000000000, 0x3fe0000000000000, 0x0008000000000000, false, true},
        {0x3ff0000000000000, 0x4000000000000000, 0x4000000000000000, false, false},
    };
    bool passed = true;
    unsigned runs = 0, mismatches = 0;
    for (const auto& shape : shapes) {
        Mapping code, stack, controls;
        if (!Check(code.Valid() && stack.Valid() && controls.Valid(), "dynamic denorm allocation")) return false;
        GuestCode instructions;
        // Prime the pass' intra-block control cache before LDMXCSR. The
        // multiply below must observe the newly stored value, not this epoch.
        instructions.Bytes({0xf3, 0x0f, 0x59, 0xd3}); // mulss xmm2, xmm3
        instructions.Bytes({0x0f, 0xae, 0x17}); // ldmxcsr [rdi]
        instructions.Bytes(shape.multiply);
        instructions.Bytes({0x0f, 0xae, 0x5f, 0x04, 0xc3}); // stmxcsr [rdi+4]; ret
        if (!Check(instructions.Publish(code), "dynamic denorm publication")) return false;
        for (std::uint32_t mode : {0x1f80U, 0x9fc0U, 0x1fc0U, 0x9f80U}) {
            const bool daz = (mode & 0x40U) != 0;
            const bool ftz = (mode & 0x8000U) != 0;
            for (unsigned caseIndex = 0; caseIndex != 4; ++caseIndex) {
                const auto& numbers = (shape.elementBytes == 4 ? single : dual)[caseIndex];
                for (unsigned negative = 0; negative != 2; ++negative) {
                    LumenAndroidFexCoreGuestStateV3 state {};
                    std::array<std::uint8_t, 32> expected {};
                    for (unsigned offset = 0; offset != 32; offset += shape.elementBytes) {
                        // Alternate signs across lanes so packed/SIMD lowering
                        // cannot pass by checking only the first positive lane.
                        const auto sign = std::uint64_t((offset / shape.elementBytes + negative) & 1U)
                            << (shape.elementBytes * 8 - 1);
                        const auto input = numbers.input | sign;
                        const auto result = ((daz && numbers.subnormalInput) ||
                                             (ftz && numbers.subnormalOutput)) ? sign : numbers.gradual | sign;
                        std::memcpy(state.vectorRegisters[0] + offset, &input, shape.elementBytes);
                        std::memcpy(state.vectorRegisters[1] + offset, &numbers.factor, shape.elementBytes);
                        std::memcpy(expected.data() + offset,
                                    offset < shape.activeBytes ? &result : &input, shape.elementBytes);
                    }
                    // Every run starts with controls off. Thus this observes
                    // instructions changing MXCSR inside already admitted code.
                    std::memcpy(controls.memory, &mode, sizeof(mode));
                    state.gp.gprs[7] = controls.Address();
                    Prepare(&state, code, stack);
                    Context context {code.Address()};
                    const auto status = LumenAndroidFexCoreRunGuestV3(&state, Callback, Query, &context);
                    std::uint32_t observedControl {};
                    std::memcpy(&observedControl, controls.Bytes(4), sizeof(observedControl));
                    const bool correct = status == 0 && context.callbacks == 0 &&
                        (observedControl & 0xffc0U) == mode && (state.mxcsr & 0xffc0U) == mode &&
                        std::memcmp(state.vectorRegisters[0], expected.data(), expected.size()) == 0;
                    ++runs;
                    if (!correct) {
                        ++mismatches;
                        std::uint64_t actual {}, wanted {};
                        std::memcpy(&actual, state.vectorRegisters[0], shape.elementBytes);
                        std::memcpy(&wanted, expected.data(), shape.elementBytes);
                        std::fprintf(stderr, "FexDenorm: FAIL op=%s mxcsr=%x case=%u sign=%u status=%u actual=%llx expected=%llx\n",
                                     shape.name, mode, caseIndex, negative, status,
                                     static_cast<unsigned long long>(actual), static_cast<unsigned long long>(wanted));
                        passed = false;
                    }
                }
            }
        }
    }
    std::printf("FexDenorm: scope=%s runs=%u mismatches=%u result=%s\n",
                "independent-controls",
                runs, mismatches, passed ? "PASS" : "FAIL");
    return passed;
}

// These bytes describe original synthetic instructions. Expectations are exact
// integer IEEE representations; native host floating-point is never the oracle.
bool DenormOperationCoverage() {
    struct Operation {
        const char* name;
        std::initializer_list<std::uint8_t> bytes;
        unsigned elementBytes, activeBytes;
        std::uint64_t x, y, z;
        std::array<std::uint64_t, 4> expected; // gradual, DAZ, FTZ, both
    };
    constexpr std::uint64_t fmin = 0x00800000, dmin = 0x0010000000000000ULL;
    const Operation operations[] {
        {"addss", {0xf3,0x0f,0x58,0xc1},4,4,1,1,0,{2,0,0,0}},
        {"addsd", {0xf2,0x0f,0x58,0xc1},8,8,1,1,0,{2,0,0,0}},
        {"addps", {0x0f,0x58,0xc1},4,16,1,1,0,{2,0,0,0}},
        {"addpd", {0x66,0x0f,0x58,0xc1},8,16,1,1,0,{2,0,0,0}},
        {"vaddps256", {0xc5,0xfc,0x58,0xc1},4,32,1,1,0,{2,0,0,0}},
        {"vaddpd256", {0xc5,0xfd,0x58,0xc1},8,32,1,1,0,{2,0,0,0}},
        {"subss", {0xf3,0x0f,0x5c,0xc1},4,4,fmin,fmin-1,0,{1,fmin,0,fmin}},
        {"subsd", {0xf2,0x0f,0x5c,0xc1},8,8,dmin,dmin-1,0,{1,dmin,0,dmin}},
        {"subps", {0x0f,0x5c,0xc1},4,16,fmin,fmin-1,0,{1,fmin,0,fmin}},
        {"subpd", {0x66,0x0f,0x5c,0xc1},8,16,dmin,dmin-1,0,{1,dmin,0,dmin}},
        // Tininess uses destination precision with an unbounded exponent.
        // These products are tiny before subnormal-range rounding, which
        // rounds them to the minimum normal; FTZ must still return zero.
        {"mulss-rounds-normal", {0xf3,0x0f,0x59,0xc1},4,4,fmin,0x3f7fffff,0,{fmin,fmin,0,0}},
        {"mulsd-rounds-normal", {0xf2,0x0f,0x59,0xc1},8,8,dmin,0x3fefffffffffffff,0,{dmin,dmin,0,0}},
        {"mulps-rounds-normal", {0x0f,0x59,0xc1},4,16,fmin,0x3f7fffff,0,{fmin,fmin,0,0}},
        {"mulpd-rounds-normal", {0x66,0x0f,0x59,0xc1},8,16,dmin,0x3fefffffffffffff,0,{dmin,dmin,0,0}},
        {"fmaddps-rounds-normal", {0xc4,0xe2,0x71,0x98,0xc2},4,16,fmin,0,0x3f7fffff,{fmin,fmin,0,0}},
        {"fmaddpd-rounds-normal", {0xc4,0xe2,0xf1,0x98,0xc2},8,16,dmin,0,0x3fefffffffffffff,{dmin,dmin,0,0}},
        {"divss-rounds-normal", {0xf3,0x0f,0x5e,0xc1},4,4,0x00ffffff,0x40000000,0,{fmin,fmin,0,0}},
        {"divsd-rounds-normal", {0xf2,0x0f,0x5e,0xc1},8,8,0x001fffffffffffff,0x4000000000000000,0,{dmin,dmin,0,0}},
        {"divps-rounds-normal", {0x0f,0x5e,0xc1},4,16,0x00ffffff,0x40000000,0,{fmin,fmin,0,0}},
        {"divpd-rounds-normal", {0x66,0x0f,0x5e,0xc1},8,16,0x001fffffffffffff,0x4000000000000000,0,{dmin,dmin,0,0}},
        {"divss-input", {0xf3,0x0f,0x5e,0xc1},4,4,fmin-1,0x3f000000,0,{0x00fffffe,0,0x00fffffe,0}},
        {"divsd-input", {0xf2,0x0f,0x5e,0xc1},8,8,dmin-1,0x3fe0000000000000,0,{0x001ffffffffffffe,0,0x001ffffffffffffe,0}},
        {"divps-output", {0x0f,0x5e,0xc1},4,16,fmin,0x40000000,0,{0x00400000,0x00400000,0,0}},
        {"divpd-output", {0x66,0x0f,0x5e,0xc1},8,16,dmin,0x4000000000000000,0,{0x0008000000000000,0x0008000000000000,0,0}},
        {"roundss-up", {0x66,0x0f,0x3a,0x0a,0xc1,2},4,4,1,1,0,{0x3f800000,0,0x3f800000,0}},
        {"roundsd-up", {0x66,0x0f,0x3a,0x0b,0xc1,2},8,8,1,1,0,{0x3ff0000000000000,0,0x3ff0000000000000,0}},
        {"roundps-up", {0x66,0x0f,0x3a,0x08,0xc1,2},4,16,1,1,0,{0x3f800000,0,0x3f800000,0}},
        {"roundpd-up", {0x66,0x0f,0x3a,0x09,0xc1,2},8,16,1,1,0,{0x3ff0000000000000,0,0x3ff0000000000000,0}},
        {"rcpss-subnormal", {0xf3,0x0f,0x53,0xc1},4,4,1,1,0,{0x7f800000,0x7f800000,0x7f800000,0x7f800000}},
        {"rcpps-tiny-output", {0x0f,0x53,0xc1},4,16,1,0x7f7fffff,0,{0,0,0,0}},
        {"rsqrtss-subnormal", {0xf3,0x0f,0x52,0xc1},4,4,1,1,0,{0x7f800000,0x7f800000,0x7f800000,0x7f800000}},
        {"rsqrtps-negative", {0x0f,0x52,0xc1},4,16,1,0x80000001,0,{0xff800000,0xff800000,0xff800000,0xff800000}},
        {"sqrtss", {0xf3,0x0f,0x51,0xc1},4,4,1,2,0,{0x1a800000,0,0x1a800000,0}},
        {"sqrtsd", {0xf2,0x0f,0x51,0xc1},8,8,1,1,0,{0x1e60000000000000,0,0x1e60000000000000,0}},
        {"sqrtps", {0x0f,0x51,0xc1},4,16,1,2,0,{0x1a800000,0,0x1a800000,0}},
        {"sqrtpd", {0x66,0x0f,0x51,0xc1},8,16,1,1,0,{0x1e60000000000000,0,0x1e60000000000000,0}},
        {"minss-negative", {0xf3,0x0f,0x5d,0xc1},4,4,0x80000001,0,0,{0x80000001,0,0x80000001,0}},
        {"minsd-negative", {0xf2,0x0f,0x5d,0xc1},8,8,0x8000000000000001,0,0,{0x8000000000000001,0,0x8000000000000001,0}},
        {"maxps", {0x0f,0x5f,0xc1},4,16,1,0,0,{1,0,1,0}},
        {"maxpd", {0x66,0x0f,0x5f,0xc1},8,16,1,0,0,{1,0,1,0}},
        {"minps-nan", {0x0f,0x5d,0xc1},4,16,0x7fc12345,1,0,{1,0,1,0}},
        {"minpd-nan", {0x66,0x0f,0x5d,0xc1},8,16,0x7ff8123456789abc,1,0,{1,0,1,0}},
        {"cmpeqss", {0xf3,0x0f,0xc2,0xc1,0},4,4,1,0,0,{0,0xffffffff,0,0xffffffff}},
        {"cmpeqsd", {0xf2,0x0f,0xc2,0xc1,0},8,8,1,0,0,{0,~0ULL,0,~0ULL}},
        {"cmpeqps", {0x0f,0xc2,0xc1,0},4,16,1,0,0,{0,0xffffffff,0,0xffffffff}},
        {"cmpeqpd", {0x66,0x0f,0xc2,0xc1,0},8,16,1,0,0,{0,~0ULL,0,~0ULL}},
        {"haddps", {0xf2,0x0f,0x7c,0xc1},4,16,1,1,0,{2,0,0,0}},
        {"haddpd", {0x66,0x0f,0x7c,0xc1},8,16,1,1,0,{2,0,0,0}},
        {"dpps", {0x66,0x0f,0x3a,0x40,0xc1,0xff},4,16,1,0x3f800000,0,{4,0,0,0}},
        {"dppd", {0x66,0x0f,0x3a,0x41,0xc1,0x33},8,16,1,0x3ff0000000000000,0,{2,0,0,0}},
        {"fmaddss", {0xc4,0xe2,0x71,0x99,0xc2},4,4,fmin-1,0,0x40000000,{0x00fffffe,0,0x00fffffe,0}},
        {"fmaddsd", {0xc4,0xe2,0xf1,0x99,0xc2},8,8,dmin-1,0,0x4000000000000000,{0x001ffffffffffffe,0,0x001ffffffffffffe,0}},
        {"fmaddps", {0xc4,0xe2,0x71,0x98,0xc2},4,16,fmin,0,0x3f000000,{0x00400000,0x00400000,0,0}},
        {"fmaddpd", {0xc4,0xe2,0xf1,0x98,0xc2},8,16,dmin,0,0x3fe0000000000000,{0x0008000000000000,0x0008000000000000,0,0}},
        {"fmaddps-addend", {0xc4,0xe2,0x71,0x98,0xc2},4,16,fmin,0x807fffff,0x3f800000,{1,fmin,0,fmin}},
        {"fmaddpd-addend", {0xc4,0xe2,0xf1,0x98,0xc2},8,16,dmin,0x800fffffffffffff,0x3ff0000000000000,{1,dmin,0,dmin}},
        {"fmaddps256", {0xc4,0xe2,0x75,0x98,0xc2},4,32,fmin-1,0,0x40000000,{0x00fffffe,0,0x00fffffe,0}},
        {"fmaddpd256", {0xc4,0xe2,0xf5,0x98,0xc2},8,32,dmin-1,0,0x4000000000000000,{0x001ffffffffffffe,0,0x001ffffffffffffe,0}},
    };
    bool passed = true;
    unsigned runs = 0;
    constexpr std::uint32_t modes[] {0x1f80,0x1fc0,0x9f80,0x9fc0};
    for (const auto& op : operations) {
        Mapping code, stack, controls;
        if (!Check(code.Valid() && stack.Valid() && controls.Valid(), "operation allocations")) return false;
        GuestCode instructions;
        instructions.Bytes({0x0f,0xae,0x17});
        instructions.Bytes(op.bytes);
        instructions.Bytes({0xc3});
        if (!Check(instructions.Publish(code), "operation publication")) return false;
        for (unsigned mode = 0; mode != 4; ++mode) {
            LumenAndroidFexCoreGuestStateV3 state {};
            std::array<std::uint8_t, 32> expected {};
            for (unsigned offset = 0; offset != 32; offset += op.elementBytes) {
                std::memcpy(state.vectorRegisters[0]+offset, &op.x, op.elementBytes);
                std::memcpy(state.vectorRegisters[1]+offset, &op.y, op.elementBytes);
                std::memcpy(state.vectorRegisters[2]+offset, &op.z, op.elementBytes);
                const auto result = offset < op.activeBytes ? op.expected[mode] : op.x;
                std::memcpy(expected.data()+offset, &result, op.elementBytes);
            }
            std::memcpy(controls.memory, &modes[mode], sizeof(modes[mode]));
            state.gp.gprs[7] = controls.Address();
            Prepare(&state,code,stack);
            Context context {code.Address()};
            const auto status = LumenAndroidFexCoreRunGuestV3(&state,Callback,Query,&context);
            // VEX128 instructions zero the upper half of their destination.
            if (op.bytes.begin()[0] == 0xc4 && op.activeBytes != 32)
                std::memset(expected.data()+16,0,16);
            ++runs;
            if (status || std::memcmp(state.vectorRegisters[0],expected.data(),32)) {
                std::uint64_t actual {};
                std::memcpy(&actual,state.vectorRegisters[0],op.elementBytes);
                std::fprintf(stderr,"FexDenorm: FAIL op=%s mxcsr=%x status=%u actual=%llx expected=%llx\n",
                    op.name,modes[mode],status,static_cast<unsigned long long>(actual),
                    static_cast<unsigned long long>(op.expected[mode]));
                passed = false;
            }
        }
    }
    std::printf("FexDenorm: arithmetic-compare-reduction-fma runs=%u result=%s\n",runs,passed?"PASS":"FAIL");
    return passed;
}

bool DenormConversionsAndFlags() {
    struct Operation {
        const char* name;
        std::initializer_list<std::uint8_t> bytes;
        unsigned inputBytes, outputBytes;
        std::uint64_t input, expected[4];
        bool flags;
        std::uint32_t rounding;
    };
    const Operation operations[] {
        {"cvtss2sd",{0xf3,0x0f,0x5a,0xc1},4,8,1,{0x36a0000000000000,0,0x36a0000000000000,0},false,0},
        {"cvtsd2ss",{0xf2,0x0f,0x5a,0xc1},8,4,0x36a0000000000000,{1,1,0,0},false,0},
        {"cvtsd2ss-rounds-normal",{0xf2,0x0f,0x5a,0xc1},8,4,0x380fffffe0000000,{0x00800000,0x00800000,0,0},false,0},
        {"cvtss2si-up",{0xf3,0x0f,0x2d,0xc1},4,4,1,{1,0,1,0},true,0x4000},
        {"cvtsd2si-up",{0xf2,0x0f,0x2d,0xc1},8,4,1,{1,0,1,0},true,0x4000},
        {"cvtss2si-down",{0xf3,0x0f,0x2d,0xc1},4,4,0x80000001,{0xffffffff,0,0xffffffff,0},true,0x2000},
        {"cvtsd2si-down",{0xf2,0x0f,0x2d,0xc1},8,4,0x8000000000000001,{0xffffffff,0,0xffffffff,0},true,0x2000},
        {"ucomiss",{0x0f,0x2e,0xc8,0x9c,0x58,0x83,0xe0,0x45},4,4,1,{0,0x40,0,0x40},true,0},
        {"ucomisd",{0x66,0x0f,0x2e,0xc8,0x9c,0x58,0x83,0xe0,0x45},8,4,1,{0,0x40,0,0x40},true,0},
        {"ucomiss-nan",{0x0f,0x2e,0xc8,0x9c,0x58,0x83,0xe0,0x45},4,4,0x7fc12345,{0x45,0x45,0x45,0x45},true,0},
        {"ucomisd-nan",{0x66,0x0f,0x2e,0xc8,0x9c,0x58,0x83,0xe0,0x45},8,4,0x7ff8123456789abc,{0x45,0x45,0x45,0x45},true,0},
    };
    constexpr std::uint32_t modes[] {0x1f80,0x1fc0,0x9f80,0x9fc0};
    bool passed = true;
    unsigned runs = 0;
    for (const auto& op : operations) {
        Mapping code,stack,controls;
        if (!Check(code.Valid()&&stack.Valid()&&controls.Valid(),"conversion allocations")) return false;
        GuestCode instructions;
        instructions.Bytes({0x0f,0xae,0x17});
        instructions.Bytes(op.bytes);
        instructions.Bytes({0xc3});
        if (!Check(instructions.Publish(code),"conversion publication")) return false;
        for (unsigned mode = 0; mode != 4; ++mode) {
            LumenAndroidFexCoreGuestStateV3 state {};
            // The upper scalar source has subnormal canaries of both precisions.
            constexpr std::uint64_t canary = 0x8000000100000001;
            if (!op.flags) for (unsigned offset = 0; offset != 32; offset += 8)
                std::memcpy(state.vectorRegisters[0]+offset,&canary,8);
            std::memcpy(state.vectorRegisters[1],&op.input,op.inputBytes);
            std::array<std::uint8_t,32> expected {};
            std::memcpy(expected.data(),state.vectorRegisters[0],32);
            std::memcpy(expected.data(),&op.expected[mode],op.outputBytes);
            const auto control = modes[mode] | op.rounding;
            std::memcpy(controls.memory,&control,4);
            state.gp.gprs[7] = controls.Address();
            Prepare(&state,code,stack);
            Context context {code.Address()};
            const auto status = LumenAndroidFexCoreRunGuestV3(&state,Callback,Query,&context);
            const bool correct = op.flags ? state.gp.gprs[0] == op.expected[mode] :
                std::memcmp(state.vectorRegisters[0],expected.data(),32) == 0;
            ++runs;
            if (status || !correct) {
                std::fprintf(stderr,"FexDenorm: FAIL op=%s mxcsr=%x status=%u\n",op.name,control,status);
                passed=false;
            }
        }
    }
    std::printf("FexDenorm: conversions-flags runs=%u result=%s\n",runs,passed?"PASS":"FAIL");
    return passed;
}

bool DenormPackedConversions() {
    struct Operation {
        const char* name;
        std::initializer_list<std::uint8_t> bytes;
        unsigned inputBytes, outputBytes, activeBytes;
        std::uint64_t input, expected[4];
        bool vex;
        std::uint32_t rounding;
    };
    const Operation operations[] {
        {"cvtps2pd",{0x0f,0x5a,0xc1},4,8,16,1,{0x36a0000000000000,0,0x36a0000000000000,0},false,0},
        {"vcvtps2pd256",{0xc5,0xfc,0x5a,0xc1},4,8,32,1,{0x36a0000000000000,0,0x36a0000000000000,0},true,0},
        {"cvtpd2ps",{0x66,0x0f,0x5a,0xc1},8,4,8,0x36a0000000000000,{1,1,0,0},false,0},
        {"vcvtpd2ps256",{0xc5,0xfd,0x5a,0xc1},8,4,16,0x36a0000000000000,{1,1,0,0},true,0},
        {"vcvtps2ph",{0xc4,0xe3,0x79,0x1d,0xc8,0},4,2,8,0x33800000,{1,1,1,1},true,0},
        // Intel SDM Vol. 1 DAZ/+infinity rules and Vol. 2 F16C behavior;
        // derivation and source receipts are in FEX_ROOT_STATE_INTERFACE.md.
        {"vcvtps2ph-subnormal-up",{0xc4,0xe3,0x79,0x1d,0xc8,2},4,2,8,1,{1,0,1,0},true,0},
        {"vcvtph2ps",{0xc4,0xe2,0x79,0x13,0xc1},2,4,16,1,{0x33800000,0x33800000,0x33800000,0x33800000},true,0},
        {"cvtps2dq-up",{0x66,0x0f,0x5b,0xc1},4,4,16,1,{1,0,1,0},false,0x4000},
        {"cvtpd2dq-up",{0xf2,0x0f,0xe6,0xc1},8,4,8,1,{1,0,1,0},false,0x4000},
    };
    constexpr std::uint32_t modes[] {0x1f80,0x1fc0,0x9f80,0x9fc0};
    bool passed=true;
    unsigned runs=0;
    for (const auto& op:operations) {
        Mapping code,stack,controls;
        if (!Check(code.Valid()&&stack.Valid()&&controls.Valid(),"packed conversion allocations")) return false;
        GuestCode instructions;
        instructions.Bytes({0x0f,0xae,0x17}); instructions.Bytes(op.bytes); instructions.Bytes({0xc3});
        if (!Check(instructions.Publish(code),"packed conversion publication")) return false;
        for (unsigned mode=0;mode!=4;++mode) {
            LumenAndroidFexCoreGuestStateV3 state {};
            std::memset(state.vectorRegisters[0],0x81,32);
            for (unsigned offset=0;offset!=32;offset+=op.inputBytes)
                std::memcpy(state.vectorRegisters[1]+offset,&op.input,op.inputBytes);
            std::array<std::uint8_t,32> expected{};
            for (unsigned offset=0;offset!=op.activeBytes;offset+=op.outputBytes)
                std::memcpy(expected.data()+offset,&op.expected[mode],op.outputBytes);
            if (!op.vex) std::memset(expected.data()+16,0x81,16);
            const auto control=modes[mode]|op.rounding;
            std::memcpy(controls.memory,&control,4);
            state.gp.gprs[7]=controls.Address(); Prepare(&state,code,stack);
            Context context{code.Address()};
            const auto status=LumenAndroidFexCoreRunGuestV3(&state,Callback,Query,&context);
            ++runs;
            if (status||std::memcmp(state.vectorRegisters[0],expected.data(),32)) {
                std::fprintf(stderr,"FexDenorm: FAIL op=%s mxcsr=%x status=%u\n",op.name,control,status);
                passed=false;
            }
        }
    }
    std::printf("FexDenorm: packed-half-conversions runs=%u result=%s\n",runs,passed?"PASS":"FAIL");
    return passed;
}

bool DenormBoundaryLanes() {
    struct Shape {
        const char* name;
        std::initializer_list<std::uint8_t> bytes;
        unsigned elementBytes, activeBytes;
        bool fma;
    };
    const Shape shapes[] {
        {"mulss",{0xf3,0x0f,0x59,0xc1},4,4,false},
        {"mulsd",{0xf2,0x0f,0x59,0xc1},8,8,false},
        {"mulps",{0x0f,0x59,0xc1},4,16,false},
        {"mulpd",{0x66,0x0f,0x59,0xc1},8,16,false},
        {"vmulps256",{0xc5,0xfc,0x59,0xc1},4,32,false},
        {"vmulpd256",{0xc5,0xfd,0x59,0xc1},8,32,false},
        {"fmaddss",{0xc4,0xe2,0x71,0x99,0xc2},4,4,true},
        {"fmaddsd",{0xc4,0xe2,0xf1,0x99,0xc2},8,8,true},
        {"fmaddps",{0xc4,0xe2,0x71,0x98,0xc2},4,16,true},
        {"fmaddpd",{0xc4,0xe2,0xf1,0x98,0xc2},8,16,true},
        {"fmaddps256",{0xc4,0xe2,0x75,0x98,0xc2},4,32,true},
        {"fmaddpd256",{0xc4,0xe2,0xf5,0x98,0xc2},8,32,true},
    };
    constexpr std::uint32_t modes[] {0x1f80,0x1fc0,0x9f80,0x9fc0};
    bool passed = true;
    unsigned runs = 0, mismatches = 0;
    for (const auto& shape : shapes) {
        Mapping code,stack,controls;
        if (!Check(code.Valid()&&stack.Valid()&&controls.Valid(),"boundary allocation")) return false;
        GuestCode instructions;
        instructions.Bytes({0x0f,0xae,0x17}); instructions.Bytes(shape.bytes); instructions.Bytes({0xc3});
        if (!Check(instructions.Publish(code),"boundary publication")) return false;
        const std::uint64_t minimum = shape.elementBytes == 4 ? 0x00800000 : 0x0010000000000000;
        const std::uint64_t one = shape.elementBytes == 4 ? 0x3f800000 : 0x3ff0000000000000;
        for (auto mode : modes) for (unsigned rc = 0; rc != 4; ++rc) for (unsigned negative = 0; negative != 2; ++negative) {
            LumenAndroidFexCoreGuestStateV3 state {};
            std::array<std::uint8_t,32> expected {};
            for (unsigned offset = 0; offset != 32; offset += shape.elementBytes) {
                const auto lane = offset / shape.elementBytes;
                const bool tiny = (lane & 1U) == 0;
                const bool signBit = ((lane / 2 + negative) & 1U) != 0;
                const std::uint64_t sign = std::uint64_t(signBit) << (shape.elementBytes * 8 - 1);
                const auto input = minimum | sign;
                const auto factor = one - (tiny ? 1 : 0);
                std::memcpy(state.vectorRegisters[0]+offset,&input,shape.elementBytes);
                std::memcpy(state.vectorRegisters[shape.fma ? 2 : 1]+offset,&factor,shape.elementBytes);
                std::uint64_t result = input;
                if (tiny && offset < shape.activeBytes) {
                    // Exact unbounded-precision result is minimum - half a
                    // subnormal ULP. Bounded rounding selects minimum for RN
                    // or rounding away from zero, otherwise the largest subnormal.
                    const bool away = (!signBit && rc == 2) || (signBit && rc == 1);
                    result = ((mode & 0x8000) ? 0 : minimum - ((rc == 0 || away) ? 0 : 1)) | sign;
                }
                std::memcpy(expected.data()+offset,&result,shape.elementBytes);
            }
            if (shape.fma && shape.activeBytes < 32) std::memset(expected.data()+16,0,16);
            const auto control = mode | (rc << 13);
            std::memcpy(controls.memory,&control,4);
            state.gp.gprs[7] = controls.Address(); Prepare(&state,code,stack);
            Context context{code.Address()};
            const auto status = LumenAndroidFexCoreRunGuestV3(&state,Callback,Query,&context);
            ++runs;
            if (status || std::memcmp(state.vectorRegisters[0],expected.data(),32)) {
                ++mismatches;
                std::fprintf(stderr,"FexDenorm: FAIL boundary op=%s mxcsr=%x negative=%u status=%u\n",shape.name,control,negative,status);
                passed = false;
            }
        }
    }
    std::printf("FexDenorm: mixed-boundary-lanes-rounding runs=%u mismatches=%u result=%s\n",runs,mismatches,passed?"PASS":"FAIL");
    return passed;
}

bool DenormDotIntermediates() {
    struct Shape {
        const char* name;
        std::initializer_list<std::uint8_t> bytes;
        unsigned activeBytes;
        bool vex;
    };
    const Shape shapes[] {
        {"dpps",{0x66,0x0f,0x3a,0x40,0xc1,0xff},16,false},
        {"vdpps128",{0xc4,0xe3,0x79,0x40,0xc1,0xff},16,true},
        {"vdpps256",{0xc4,0xe3,0x7d,0x40,0xc1,0xff},32,true},
    };
    constexpr std::uint32_t modes[] {0x1f80,0x1fc0,0x9f80,0x9fc0};
    bool passed = true;
    unsigned runs = 0, mismatches = 0;
    for (const auto& shape : shapes) {
        Mapping code,stack,controls;
        if (!Check(code.Valid()&&stack.Valid()&&controls.Valid(),"dot intermediate allocation")) return false;
        GuestCode instructions;
        instructions.Bytes({0x0f,0xae,0x17}); instructions.Bytes(shape.bytes); instructions.Bytes({0xc3});
        if (!Check(instructions.Publish(code),"dot intermediate publication")) return false;
        for (auto mode : modes) for (unsigned rc = 0; rc != 4; ++rc) for (unsigned negative = 0; negative != 2; ++negative) {
            LumenAndroidFexCoreGuestStateV3 state {};
            std::array<std::uint8_t,32> expected {};
            for (unsigned lane = 0; lane != 8; ++lane) {
                // Each product has a normal input or zero. The first pair
                // cancels to a subnormal, then the second pair restores the
                // minimum normal. Intel's per-operation exception contract
                // requires DAZ/FTZ at these internal addition boundaries.
                constexpr std::uint32_t values[] {0x00800000,0x80800001,0x00800000,0};
                const auto sign = (negative ^ (lane / 4)) << 31;
                const auto input = values[lane % 4] ^ sign;
                const std::uint32_t one = 0x3f800000;
                std::memcpy(state.vectorRegisters[0]+lane*4,&input,4);
                std::memcpy(state.vectorRegisters[1]+lane*4,&one,4);
                const auto result = lane*4 < shape.activeBytes
                    ? ((mode & 0x8040) ? 0x00800000U : 0x007fffffU) | sign
                    : shape.vex ? 0U : input;
                std::memcpy(expected.data()+lane*4,&result,4);
            }
            const auto control = mode | (rc << 13);
            std::memcpy(controls.memory,&control,4);
            state.gp.gprs[7] = controls.Address(); Prepare(&state,code,stack);
            Context context{code.Address()};
            const auto status = LumenAndroidFexCoreRunGuestV3(&state,Callback,Query,&context);
            ++runs;
            if (status || std::memcmp(state.vectorRegisters[0],expected.data(),32)) {
                ++mismatches;
                std::uint32_t actual {}, wanted {};
                std::memcpy(&actual,state.vectorRegisters[0],4); std::memcpy(&wanted,expected.data(),4);
                std::fprintf(stderr,"FexDenorm: FAIL dot-intermediate op=%s mxcsr=%x negative=%u status=%u actual=%x expected=%x\n",
                    shape.name,control,negative,status,actual,wanted);
                passed = false;
            }
        }
    }
    std::printf("FexDenorm: dot-intermediate-controls runs=%u mismatches=%u result=%s\n",runs,mismatches,passed?"PASS":"FAIL");
    return passed;
}

bool DenormStateRestoreAndX87() {
    constexpr std::uint32_t modes[] {0x1f80,0x1fc0,0x9f80,0x9fc0};
    bool passed=true;
    unsigned runs=0;
    for (bool extended : {false,true}) {
        Mapping code,stack,controls;
        if (!Check(code.Valid()&&stack.Valid()&&controls.Valid(),"state-restore allocations")) return false;
        GuestCode instructions;
        instructions.Bytes({0x48,0x0f,0xae,0x47,0x10, // fxsave64 [rdi+16]
                            0x8b,0x07,0x89,0x47,0x28}); // image.mxcsr = [rdi]
        if (extended) {
            instructions.Mov(0,3); instructions.Mov(2,0);
            instructions.Bytes({0x48,0x0f,0xae,0x6f,0x10}); // xrstor64 [rdi+16]
        } else {
            instructions.Bytes({0x48,0x0f,0xae,0x4f,0x10}); // fxrstor64 [rdi+16]
        }
        instructions.Bytes({0xf2,0x0f,0x59,0xc1, // mulsd xmm0,xmm1
                            0xdd,0x87,0x00,0x04,0x00,0x00, // fld qword [rdi+1024]
                            0xdc,0x8f,0x08,0x04,0x00,0x00, // fmul qword [rdi+1032]
                            0xdd,0x9f,0x10,0x04,0x00,0x00, // fstp qword [rdi+1040]
                            0x0f,0xae,0x5f,0x04,0xc3}); // stmxcsr [rdi+4];ret
        if (!Check(instructions.Publish(code),"state-restore publication")) return false;
        for (auto control:modes) {
            LumenAndroidFexCoreGuestStateV3 state {};
            const std::uint64_t input=0x000fffffffffffff, factor=0x4000000000000000;
            const std::uint64_t normal=0x001ffffffffffffe;
            std::memcpy(state.vectorRegisters[0],&input,8);
            std::memcpy(state.vectorRegisters[1],&factor,8);
            std::memcpy(controls.memory,&control,4);
            const std::uint64_t components=3;
            std::memcpy(controls.Bytes(528),&components,8);
            std::memcpy(controls.Bytes(1024),&input,8);
            std::memcpy(controls.Bytes(1032),&factor,8);
            state.gp.gprs[7]=controls.Address();
            Prepare(&state,code,stack);
            Context context {code.Address()};
            const auto status=LumenAndroidFexCoreRunGuestV3(&state,Callback,Query,&context);
            std::uint64_t sse{},x87{};
            std::uint32_t observed{};
            std::memcpy(&sse,state.vectorRegisters[0],8);
            std::memcpy(&x87,controls.Bytes(1040),8);
            std::memcpy(&observed,controls.Bytes(4),4);
            const auto expected=(control&0x40)?0:normal;
            ++runs;
            if (status||sse!=expected||x87!=normal||(observed&0xffc0)!=control) {
                std::fprintf(stderr,"FexDenorm: FAIL restore=%s mxcsr=%x status=%u sse=%llx x87=%llx\n",
                    extended?"xrstor":"fxrstor",control,status,
                    static_cast<unsigned long long>(sse),static_cast<unsigned long long>(x87));
                passed=false;
            }
        }
    }
    std::printf("FexDenorm: state-restore-x87 runs=%u result=%s\n",runs,passed?"PASS":"FAIL");
    return passed;
}

bool InaccessibleBootstrapStack() {
    Mapping code, stack(2);
    if (!Check(code.Valid() && stack.Valid(), "guard allocations")) return false;
    GuestCode instructions;
    instructions.Bytes({0xc3});
    if (!Check(instructions.Publish(code) && stack.Protect(PROT_NONE),
               "guard publication")) return false;
    LumenAndroidFexCoreGuestStateV3 state {};
    state.gp.rip = code.Address();
    state.gp.gprs[4] = stack.Address(PAGE_SIZE);
    const auto sentinel = LumenAndroidFexCoreRootReturnAddressV1();
    std::memcpy(reinterpret_cast<void*>(state.gp.gprs[4]), &sentinel, sizeof(sentinel));
    Context context {code.Address()};
    std::fenv_t original {};
    std::fegetenv(&original);
    std::fesetround(FE_UPWARD);
    std::feraiseexcept(FE_INEXACT);
    const int exceptions = std::fetestexcept(FE_ALL_EXCEPT);
    const auto result = LumenAndroidFexCoreRunGuestV3(&state, Callback, Query, &context);
    const bool preserved = std::fegetround() == FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT) == exceptions;
    std::fesetenv(&original);
    return Check(result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
                 state.gp.faultSignal != 0 && context.callbacks == 0 && preserved,
                 "bootstrap guard fault restores native environment without callback");
}

bool RejectedNativeBridgeStackRead() {
    Mapping code, stack;
    if (!Check(code.Valid() && stack.Valid(), "bridge stack allocations")) return false;
    GuestCode instructions;
    instructions.Mov(11, reinterpret_cast<std::uint64_t>(&HostTarget));
    instructions.Bytes({0x41, 0xff, 0xd3, 0xc3}); // call r11; ret
    if (!Check(instructions.Publish(code), "bridge stack code publication")) return false;
    LumenAndroidFexCoreGuestStateV3 state {};
    Prepare(&state, code, stack);
    state.gp.gprs[0] = 0x1732abc45678;
    Context context {code.Address()};
    // CALL pushes the return address and the bridge pushes its private RAX
    // slot. Keep the backing RW but deny the native reader's exact footprint,
    // so this case cannot pass merely because of a hardware protection fault.
    deniedDataAddress = state.gp.gprs[4] - 16;
    deniedDataReads = 0;
    const auto result = LumenAndroidFexCoreRunGuestV3(&state, Callback, Query, &context);
    const auto rejectedReads = deniedDataReads;
    deniedDataAddress = 0;
    return Check(result == LUMEN_ANDROID_FEXCORE_RUNTIME_HOST_CALL_FAILED_V1 &&
                 context.callbacks == 0 && rejectedReads == 1,
                 "native bridge denies unreadable RAX slot before invoking HLE");
}

bool FrontendCallNextBlocks() {
    // Original programs exercise decoded-block lifetime. Both forward targets
    // make the call-to-next and its following POP separate singleton blocks.
    // The controls keep POP in a longer block or leave its start undiscovered.
    struct Program {
        const char* label;
        std::initializer_list<std::uint8_t> bytes;
        std::size_t returnOffset;
        bool skipPath;
    };
    const Program programs[] {
        {"coalesced", {0x85,0xff, 0x74,0x06, 0xe8,0,0,0,0, 0x58,0xc3}, 9, false},
        {"two-instruction-successor",
         {0x85,0xff, 0x74,0x09, 0x85,0xf6, 0x74,0x07, 0xe8,0,0,0,0, 0x58,0x90,0xc3}, 13, true},
        {"singleton-successor",
         {0x85,0xff, 0x74,0x09, 0x85,0xf6, 0x74,0x06, 0xe8,0,0,0,0, 0x58,0xc3}, 13, true},
    };
    unsigned runs = 0;
    for (const auto& program : programs) {
        Mapping code, stack;
        if (!Check(code.Valid() && stack.Valid(), "frontend block allocations")) return false;
        GuestCode instructions;
        instructions.Bytes(program.bytes);
        if (!Check(instructions.Publish(code), "frontend block publication")) return false;
        for (unsigned skip = 0; skip != (program.skipPath ? 2U : 1U); ++skip) {
            std::printf("FexFrontend: case=%s path=%s begin\n", program.label,
                        skip ? "skip-call" : "call-next");
            std::fflush(stdout);
            LumenAndroidFexCoreGuestStateV3 state {};
            Prepare(&state, code, stack);
            state.mxcsr = 0x1f80;
            state.gp.gprs[0] = 0x515;
            state.gp.gprs[7] = 1; // EDI keeps the first conditional fallthrough.
            state.gp.gprs[6] = skip ? 0 : 1; // ESI selects the second conditional.
            Context context {code.Address()};
            const auto result = LumenAndroidFexCoreRunGuestV3(&state, Callback, Query, &context);
            const auto expected = skip ? UINT64_C(0x515) : code.Address(program.returnOffset);
            if (!Check(result == 0 && context.callbacks == 0 && state.gp.gprs[0] == expected &&
                       state.gp.gprs[4] == stack.Address(PAGE_SIZE),
                       "frontend call-next result and balanced stack")) return false;
            ++runs;
        }
    }
    std::printf("FexFrontend: call-next-blocks runs=%u result=PASS\n", runs);
    return true;
}

bool ScalarSourceCursorLoop() {
    // Original two-level loop: copy the source cursor into R9 before ESI
    // becomes a scalar counter, then carry the advanced cursor across blocks.
    // This deliberately gives static-register coalescing two distinct roles
    // for RSI while every source address stays within one live allocation.
    constexpr unsigned outerCount = 256, innerCount = 2;
    constexpr std::size_t dataOffset = 128;
    constexpr std::size_t samples = outerCount * innerCount;
    constexpr std::size_t dataBytes = samples * sizeof(std::uint32_t);
    constexpr std::array<std::uint32_t, 8> inputs {
        0x3f800000, 0xbf800000, 0x00800000, 0x80800000,
        0x00000002, 0x80000002, 0x40000000, 0xc0000000,
    };
    constexpr std::array<std::uint32_t, 8> halves {
        0x3f000000, 0xbf000000, 0x00400000, 0x80400000,
        0x00000001, 0x80000001, 0x3f800000, 0xbf800000,
    };
    Mapping code, stack, source, output;
    if (!Check(code.Valid() && stack.Valid() && source.Valid() && output.Valid(),
               "source cursor loop allocations")) return false;
    GuestCode instructions;
    instructions.Bytes({0x48,0x89,0xf3, 0x31,0xff}); // mov rbx,rsi; xor edi,edi
    // Outer loop begins here. No pointer is reloaded from guest memory.
    instructions.Bytes({0x49,0x89,0xf1, 0x44,0x89,0xee}); // mov r9,rsi; mov esi,r13d
    instructions.Bytes({0xc4,0xc1,0x72,0x59,0x11}); // vmulss xmm2,xmm1,[r9]
    instructions.Bytes({0xc4,0xc1,0x7a,0x11,0x10}); // vmovss [r8],xmm2
    instructions.Bytes({0x49,0x83,0xc1,0x04, 0x49,0x83,0xc0,0x04}); // advance source/dest
    instructions.Bytes({0xff,0xce, 0x75,0xea}); // dec esi; jne inner loop (-22)
    instructions.Bytes({0x4c,0x89,0xce, 0xff,0xc7}); // mov rsi,r9; inc edi
    instructions.Bytes({0x44,0x39,0xe7, 0x72,0xda, 0xc3}); // cmp edi,r12d; jb outer (-38); ret
    if (!Check(instructions.Publish(code), "source cursor loop publication")) return false;
    std::array<std::uint8_t, PAGE_SIZE> expectedSource, expectedOutput;
    expectedSource.fill(0xa5);
    for (std::size_t index = 0; index != samples; ++index)
        std::memcpy(expectedSource.data() + dataOffset + index * sizeof(std::uint32_t),
                    &inputs[index % inputs.size()], sizeof(std::uint32_t));
    std::memcpy(source.memory, expectedSource.data(), expectedSource.size());
    if (!Check(source.Protect(PROT_READ), "source cursor loop read-only input")) return false;
    bool passed = true;
    unsigned runs = 0;
    for (const std::uint32_t control : {0x1f80U,0x1fc0U,0x9f80U,0x9fc0U}) {
        expectedOutput.fill(0xcc);
        std::memset(output.memory, 0xcc, PAGE_SIZE);
        for (std::size_t index = 0; index != samples; ++index) {
            const auto slot = index % inputs.size();
            const bool flush = ((control & 0x40U) && (slot == 4 || slot == 5)) ||
                               ((control & 0x8000U) && slot >= 2 && slot <= 5);
            const std::uint32_t expected = flush ? inputs[slot] & 0x80000000U : halves[slot];
            std::memcpy(expectedOutput.data() + dataOffset + index * sizeof(expected),
                        &expected, sizeof(expected));
        }
        LumenAndroidFexCoreGuestStateV3 state {};
        state.mxcsr = control;
        for (unsigned reg = 0; reg != 16; ++reg) {
            state.gp.gprs[reg] = UINT64_C(0x1234567800000000) + reg;
            for (unsigned byte = 0; byte != 32; ++byte)
                state.vectorRegisters[reg][byte] = static_cast<std::uint8_t>(reg * 17 + byte * 7 + 1);
        }
        const std::uint32_t factor = 0x3f000000; // Exact multiplication by one half.
        std::memcpy(state.vectorRegisters[1], &factor, sizeof(factor));
        state.gp.gprs[6] = source.Address(dataOffset);
        state.gp.gprs[8] = output.Address(dataOffset);
        state.gp.gprs[12] = outerCount;
        state.gp.gprs[13] = innerCount;
        Prepare(&state, code, stack);
        std::array<std::uint64_t, 16> expectedGprs;
        std::memcpy(expectedGprs.data(), state.gp.gprs, sizeof(state.gp.gprs));
        expectedGprs[3] = source.Address(dataOffset);
        expectedGprs[4] = stack.Address(PAGE_SIZE);
        expectedGprs[6] = expectedGprs[9] = source.Address(dataOffset + dataBytes);
        expectedGprs[7] = outerCount;
        expectedGprs[8] = output.Address(dataOffset + dataBytes);
        std::array<std::uint8_t, 16 * 32> expectedVectors;
        std::memcpy(expectedVectors.data(), state.vectorRegisters, expectedVectors.size());
        auto* expectedResult = expectedVectors.data() + 2 * 32;
        std::memcpy(expectedResult, &halves.back(), sizeof(std::uint32_t));
        std::memcpy(expectedResult + 4, state.vectorRegisters[1] + 4, 12);
        std::memset(expectedResult + 16, 0, 16);
        Context context {code.Address()};
        const auto status = LumenAndroidFexCoreRunGuestV3(&state, Callback, Query, &context);
        ++runs;
        const bool gprsCorrect = std::memcmp(state.gp.gprs, expectedGprs.data(), sizeof(state.gp.gprs)) == 0;
        const bool vectorsCorrect = std::memcmp(state.vectorRegisters, expectedVectors.data(), expectedVectors.size()) == 0;
        const bool sourceCorrect = std::memcmp(source.memory, expectedSource.data(), expectedSource.size()) == 0;
        const bool outputCorrect = std::memcmp(output.memory, expectedOutput.data(), expectedOutput.size()) == 0;
        const bool correct = status == 0 && context.callbacks == 0 &&
            (state.mxcsr & 0xffc0U) == control && gprsCorrect && vectorsCorrect && sourceCorrect && outputCorrect;
        if (!correct) {
            std::fprintf(stderr, "FexRootStateTest: source cursor loop mxcsr=%x status=%u gprs=%u vectors=%u source=%u output=%u\n",
                         control, status, gprsCorrect, vectorsCorrect, sourceCorrect, outputCorrect);
            passed = false;
        }
    }
    std::printf("FexSourceCursor: inner=%u outer=%u samples=%zu runs=%u sentinels=full-page result=%s\n",
                innerCount, outerCount, samples, runs, passed ? "PASS" : "FAIL");
    return passed;
}

struct SourceDispatchBoundaryContext {
    std::uint64_t code {}, stack {};
    unsigned deliveries {}, loopDeliveries {}, carryDeliveries {};
    bool valid {true};
};

thread_local SourceDispatchBoundaryContext* sourceDispatchBoundary {};

std::uint32_t CaptureSourceDispatchBoundary(const std::uint64_t* gprs,
                                             std::uint64_t rip) noexcept {
    auto* const context = sourceDispatchBoundary;
    if (context == nullptr) return 0;
    ++context->deliveries;
    // The original dispatch corpus is 213 bytes. Offsets 49..208 contain
    // the outer carry, dispatch, vector loop and scalar loop; an interrupt
    // confined to the prologue/epilogue does not exercise the cursor carry.
    const bool inCode = rip >= context->code && rip - context->code < 213;
    // The owned V3 adapter enters its immutable MXCSR bootstrap at offset
    // 256 in the same private bridge page whose root HLT is at offset 16.
    // Only those exact internal boundaries are outside this fixture's code;
    // they never count as a cursor-loop interruption.
    const auto rootReturn = LumenAndroidFexCoreRootReturnAddressV1();
    const bool atInternalBoundary = rip == rootReturn ||
        (rootReturn <= UINT64_MAX - (256 - 16) && rip == rootReturn + (256 - 16));
    // These are the decoded leaders derived from the original program's
    // conditional/unconditional edges. In particular offset 49 is the
    // source-cursor carry; offset 47 is the initial jump and cannot substitute
    // for that completed-loop boundary.
    constexpr unsigned leaders[] {0,49,58,63,69,84,91,157,167,169,175,181,204,209};
    bool atCodeBoundary = false;
    for (const auto leader : leaders)
        atCodeBoundary = atCodeBoundary || (inCode && rip - context->code == leader);
    const bool onStack = gprs != nullptr && gprs[4] >= context->stack &&
                         gprs[4] - context->stack <= PAGE_SIZE;
    if ((!(atCodeBoundary || atInternalBoundary) || !onStack) && context->valid) {
        std::fprintf(stderr, "FexSourceDispatchBoundary: invalid rip=0x%llx code=0x%llx offset=0x%llx rsp=0x%llx stack=0x%llx inCode=%u onStack=%u rootReturn=0x%llx\n",
                     static_cast<unsigned long long>(rip),
                     static_cast<unsigned long long>(context->code),
                     static_cast<unsigned long long>(rip - context->code),
                     static_cast<unsigned long long>(gprs != nullptr ? gprs[4] : 0),
                     static_cast<unsigned long long>(context->stack), inCode, onStack,
                     static_cast<unsigned long long>(LumenAndroidFexCoreRootReturnAddressV1()));
    }
    context->valid = context->valid && (atCodeBoundary || atInternalBoundary) && onStack;
    if (inCode && rip - context->code >= 49 && rip - context->code < 209)
        ++context->loopDeliveries;
    if (inCode && rip - context->code == 49) ++context->carryDeliveries;
    return 1;
}

class SourceDispatchInterrupts {
public:
    explicit SourceDispatchInterrupts(SourceDispatchBoundaryContext& context)
        : hostThread(reinterpret_cast<std::uint64_t>(pthread_self())) {
        sourceDispatchBoundary = &context;
        requester = std::thread([this] {
            ready.store(true, std::memory_order_release);
            while (running.load(std::memory_order_acquire)) {
                // A zero result simply means the next root run is not active
                // yet. Repeated requests use the production token coalescing;
                // each root run owns cancellation and page cleanup on exit.
                LumenAndroidFexCoreRequestGuestBoundaryInterruptV1(hostThread);
                std::this_thread::yield();
            }
        });
        while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    ~SourceDispatchInterrupts() {
        running.store(false, std::memory_order_release);
        requester.join();
        sourceDispatchBoundary = nullptr;
    }
private:
    const std::uint64_t hostThread;
    std::atomic<bool> running {true}, ready {false};
    std::thread requester;
};

bool SourceCursorDispatchLoop() {
    // Original programs join a scalar preheader and a vector-tail edge. The
    // preheader copies RSI into R9 before ESI becomes a counter; the tail skips
    // that copy because R9 already points past the vector prefix. The outer
    // edge then restores RSI from R9. Neither program contains guest-ROM bytes.
    struct Shape { unsigned inner, outer; };
    constexpr Shape shapes[] {{2,256}, {31,16}, {32,16}, {33,16}, {34,16}, {64,8}};
    constexpr std::size_t dataOffset = 128, factorOffset = 64;
    constexpr std::uint32_t factor = 0x3f000000;
    constexpr std::array<std::uint32_t, 8> inputs {
        0x3f800000, 0xbf800000, 0x00800000, 0x80800000,
        0x00000002, 0x80000002, 0x40000000, 0xc0000000,
    };
    constexpr std::array<std::uint32_t, 8> halves {
        0x3f000000, 0xbf000000, 0x00400000, 0x80400000,
        0x00000001, 0x80000001, 0x3f800000, 0xbf800000,
    };
    if (!Check(LumenAndroidFexCoreRegisterBoundaryDeliveryV1(CaptureSourceDispatchBoundary) == 1,
               "source dispatch boundary registration")) return false;
    SourceDispatchBoundaryContext boundary;
    SourceDispatchInterrupts interrupts(boundary);
    unsigned runs = 0;
    bool passed = true;
    for (unsigned pressure = 0; pressure != 2; ++pressure) {
        Mapping code, stack;
        if (!Check(code.Valid() && stack.Valid(), "source dispatch code allocations")) return false;
        GuestCode instructions;
        // Save RBP/R14, load and broadcast the RO half factor, derive the
        // 32-element vector prefix, and dispatch on count and pointer distance.
        instructions.Bytes({
            0x55,0x48,0x89,0xe5,0x41,0x56,0x45,0x89,0xce,0xc4,0xc1,0x7a,0x10,0x0c,0x24,0xc5,
            0xf0,0xc6,0xc1,0x00,0xc4,0xe3,0x7d,0x18,0xc0,0x01,0x4d,0x89,0xf8,0x44,0x89,0xf0,
            0x89,0xc1,0x83,0xe1,0xe0,0x48,0x8d,0x14,0x8d,0x00,0x00,0x00,0x00,0x31,0xff,0xeb,
            0x0e,0x4c,0x89,0xce,0xff,0xc7,0x39,0xdf,0x72,0x05,0xe9,0x92,0x00,0x00,0x00,0x41,
            0x83,0xfe,0x20,0x72,0x64,0x4d,0x89,0xc1,0x49,0x29,0xf1,0x49,0x81,0xf9,0x80,0x00,
            0x00,0x00,0x72,0x55,0x4c,0x8d,0x0c,0x16,0x45,0x31,0xd2,
        });
        if (pressure == 0) {
            // YMM2..5: store each product before loading the next vector.
            instructions.Bytes({
                0xc4,0xa1,0x7c,0x59,0x14,0x16,0xc4,0x81,0x7c,0x11,0x14,0x10,0xc4,0xa1,0x7c,0x59,
                0x5c,0x16,0x20,0xc4,0x81,0x7c,0x11,0x5c,0x10,0x20,0xc4,0xa1,0x7c,0x59,0x64,0x16,
                0x40,0xc4,0x81,0x7c,0x11,0x64,0x10,0x40,0xc4,0xa1,0x7c,0x59,0x6c,0x16,0x60,0xc4,
                0x81,0x7c,0x11,0x6c,0x10,0x60,
            });
        } else {
            // YMM10..13: keep four products live before storing any vector.
            instructions.Bytes({
                0xc4,0x21,0x7c,0x59,0x14,0x16,0xc4,0x21,0x7c,0x59,0x5c,0x16,0x20,0xc4,0x21,0x7c,
                0x59,0x64,0x16,0x40,0xc4,0x21,0x7c,0x59,0x6c,0x16,0x60,0xc4,0x01,0x7c,0x11,0x14,
                0x10,0xc4,0x01,0x7c,0x11,0x5c,0x10,0x20,0xc4,0x01,0x7c,0x11,0x64,0x10,0x40,0xc4,
                0x01,0x7c,0x11,0x6c,0x10,0x60,
            });
        }
        // Vector loop, tail edge into the scalar counter, scalar preheader,
        // scalar loop, outer cursor carry, and balanced saved-register return.
        instructions.Bytes({
            0x49,0x81,0xc2,0x80,0x00,0x00,0x00,0x49,0x39,0xd2,0x72,0xbe,0x49,0x01,0xd0,0x39,
            0xc8,0x41,0x89,0xca,0x75,0x08,0xeb,0x88,0x49,0x89,0xf1,0x45,0x31,0xd2,0x44,0x89,
            0xf6,0x44,0x29,0xd6,0xc4,0xc1,0x72,0x59,0x11,0xc4,0xc1,0x7a,0x11,0x10,0x4d,0x8d,
            0x49,0x04,0x4d,0x8d,0x40,0x04,0x83,0xee,0x01,0x75,0xe9,0xe9,0x60,0xff,0xff,0xff,
            0x41,0x5e,0x5d,0xc3,
        });
        if (!Check(instructions.Publish(code), "source dispatch publication")) return false;
        for (const auto shape : shapes) {
            Mapping source, output;
            if (!Check(source.Valid() && output.Valid(), "source dispatch data allocations")) return false;
            const std::size_t samples = shape.inner * shape.outer;
            const std::size_t dataBytes = samples * sizeof(std::uint32_t);
            const unsigned prefix = shape.inner & ~31U;
            if (!Check(dataOffset + dataBytes <= PAGE_SIZE &&
                       output.Address(dataOffset) - source.Address(dataOffset) >= 128,
                       "source dispatch footprint and disjoint vector admission")) return false;
            std::array<std::uint8_t, PAGE_SIZE> expectedSource, expectedOutput;
            expectedSource.fill(0xa5);
            std::memcpy(expectedSource.data() + factorOffset, &factor, sizeof(factor));
            for (std::size_t index = 0; index != samples; ++index) {
                // Distinct even mantissas make displacement errors observable
                // while multiplication by one half remains exactly representable.
                const std::uint32_t input = inputs[index % inputs.size()] +
                                            static_cast<std::uint32_t>(index / inputs.size()) * 2;
                std::memcpy(expectedSource.data() + dataOffset + index * sizeof(std::uint32_t),
                            &input, sizeof(input));
            }
            std::memcpy(source.memory, expectedSource.data(), expectedSource.size());
            if (!Check(source.Protect(PROT_READ), "source dispatch read-only input")) return false;
            for (const std::uint32_t control : {0x1f80U,0x1fc0U,0x9f80U,0x9fc0U}) {
                expectedOutput.fill(0xcc);
                std::memset(output.memory, 0xcc, PAGE_SIZE);
                for (std::size_t index = 0; index != samples; ++index) {
                    const auto slot = index % inputs.size();
                    const bool flush = ((control & 0x40U) && (slot == 4 || slot == 5)) ||
                                       ((control & 0x8000U) && slot >= 2 && slot <= 5);
                    const auto sequence = static_cast<std::uint32_t>(index / inputs.size());
                    const std::uint32_t product = halves[slot] + sequence * (slot >= 2 && slot <= 5 ? 1 : 2);
                    const std::uint32_t expected = flush ? inputs[slot] & 0x80000000U : product;
                    std::memcpy(expectedOutput.data() + dataOffset + index * sizeof(expected),
                                &expected, sizeof(expected));
                }
                LumenAndroidFexCoreGuestStateV3 state {};
                state.mxcsr = control;
                for (unsigned reg = 0; reg != 16; ++reg) {
                    state.gp.gprs[reg] = UINT64_C(0x1234567800000000) + reg;
                    for (unsigned byte = 0; byte != 32; ++byte)
                        state.vectorRegisters[reg][byte] = static_cast<std::uint8_t>(reg * 17 + byte * 7 + 1);
                }
                state.gp.gprs[3] = shape.outer;
                state.gp.gprs[6] = source.Address(dataOffset);
                state.gp.gprs[9] = shape.inner;
                state.gp.gprs[12] = source.Address(factorOffset);
                state.gp.gprs[15] = output.Address(dataOffset);
                Prepare(&state, code, stack);
                std::array<std::uint64_t, 16> expectedGprs;
                std::memcpy(expectedGprs.data(), state.gp.gprs, sizeof(state.gp.gprs));
                expectedGprs[0] = shape.inner;
                expectedGprs[1] = prefix;
                expectedGprs[2] = prefix * sizeof(std::uint32_t);
                expectedGprs[4] = stack.Address(PAGE_SIZE);
                expectedGprs[6] = expectedGprs[9] = source.Address(dataOffset + dataBytes);
                expectedGprs[7] = shape.outer;
                expectedGprs[8] = output.Address(dataOffset + dataBytes);
                expectedGprs[10] = prefix;
                std::array<std::uint8_t, 16 * 32> expectedVectors;
                std::memcpy(expectedVectors.data(), state.vectorRegisters, expectedVectors.size());
                for (unsigned lane = 0; lane != 8; ++lane)
                    std::memcpy(expectedVectors.data() + lane * sizeof(factor), &factor, sizeof(factor));
                std::memset(expectedVectors.data() + 32, 0, 32);
                std::memcpy(expectedVectors.data() + 32, &factor, sizeof(factor));
                if (prefix != 0) {
                    const auto lastVector = (shape.outer - 1) * shape.inner + prefix - 32;
                    for (unsigned group = 0; group != 4; ++group)
                        std::memcpy(expectedVectors.data() + (pressure ? 10 + group : 2 + group) * 32,
                                    expectedOutput.data() + dataOffset + (lastVector + group * 8) * 4, 32);
                }
                if (prefix == 0 || prefix != shape.inner) {
                    std::memset(expectedVectors.data() + 2 * 32, 0, 32);
                    std::memcpy(expectedVectors.data() + 2 * 32,
                                expectedOutput.data() + dataOffset + dataBytes - 4, 4);
                }
                const auto initialState = state;
                boundary.code = code.Address();
                boundary.stack = stack.Address();
                const unsigned carryDeliveriesBefore = boundary.carryDeliveries;
                // These finite loops can finish before the peer is scheduled.
                // Every attempt retains the complete result checks, and a
                // variant passes only after an interrupt at the exact cursor
                // carry. Bound retries without depending on peer scheduling.
                for (unsigned attempt = 0; attempt != 1024; ++attempt) {
                    state = initialState;
                    std::memset(output.memory, 0xcc, PAGE_SIZE);
                    Context context {code.Address()};
                    const auto status = LumenAndroidFexCoreRunGuestV3(&state, Callback, Query, &context);
                    ++runs;
                    const bool gprsCorrect = std::memcmp(state.gp.gprs, expectedGprs.data(), sizeof(state.gp.gprs)) == 0;
                    const bool vectorsCorrect = std::memcmp(state.vectorRegisters, expectedVectors.data(), expectedVectors.size()) == 0;
                    const bool sourceCorrect = std::memcmp(source.memory, expectedSource.data(), expectedSource.size()) == 0;
                    const bool outputCorrect = std::memcmp(output.memory, expectedOutput.data(), expectedOutput.size()) == 0;
                    const bool correct = status == 0 && context.callbacks == 0 && boundary.valid &&
                        (state.mxcsr & 0xffc0U) == control && gprsCorrect && vectorsCorrect && sourceCorrect && outputCorrect;
                    if (!correct) {
                        std::fprintf(stderr, "FexRootStateTest: source dispatch inner=%u outer=%u pressure=%u mxcsr=%x attempt=%u status=%u gprs=%u vectors=%u source=%u output=%u boundary=%u rip=0x%llx faultSignal=%u faultAddress=0x%llx code=0x%llx\n",
                                     shape.inner, shape.outer, pressure, control, attempt, status,
                                     gprsCorrect, vectorsCorrect, sourceCorrect, outputCorrect, boundary.valid,
                                     static_cast<unsigned long long>(state.gp.rip), state.gp.faultSignal,
                                     static_cast<unsigned long long>(state.gp.faultAddress),
                                     static_cast<unsigned long long>(code.Address()));
                        passed = false;
                        break;
                    }
                    if (boundary.carryDeliveries != carryDeliveriesBefore) break;
                }
                if (boundary.carryDeliveries == carryDeliveriesBefore) {
                    std::fprintf(stderr, "FexRootStateTest: source dispatch exact carry interrupt not exercised after 1024 attempts inner=%u outer=%u pressure=%u mxcsr=%x\n",
                                 shape.inner, shape.outer, pressure, control);
                    passed = false;
                }
            }
        }
    }
    std::printf("FexSourceDispatch: shapes=6 vector-register-profiles=2 variants=48 runs=%u interrupts=%u loop-interrupts=%u carry-interrupts=%u sentinels=full-page result=%s\n",
                runs, boundary.deliveries, boundary.loopDeliveries, boundary.carryDeliveries, passed ? "PASS" : "FAIL");
    return passed;
}

} // namespace

int main(int argc, char** argv) {
    alarm(30);
    const bool requireGeneral = argc == 2 && std::strcmp(argv[1], "--require-general-denormals") == 0;
    // Opt-in reproducer of the documented unresolved frontend assertion.
    // Keep the ordinary regression suite usable until its fix is verified.
    const bool frontendOnly = argc == 2 && std::strcmp(argv[1], "--frontend-call-next") == 0;
    if (argc > 1 && !requireGeneral && !frontendOnly) return 2;
    if (!Check(LumenMacOSFexCoreRegisterDataAccessQueryV1(QueryDataAccess) == 1,
               "fixture data access query")) return 1;
    if (frontendOnly) return FrontendCallNextBlocks() ? 0 : 1;
    bool passed = Run(false);
    passed = Run(true) && passed;
    passed = RootDenormControls() && passed;
    passed = DynamicDenormControls() && passed;
    passed = DenormOperationCoverage() && passed;
    passed = DenormConversionsAndFlags() && passed;
    passed = DenormPackedConversions() && passed;
    passed = DenormBoundaryLanes() && passed;
    passed = DenormDotIntermediates() && passed;
    passed = DenormStateRestoreAndX87() && passed;
    passed = InaccessibleBootstrapStack() && passed;
    passed = RejectedNativeBridgeStackRead() && passed;
    passed = ScalarSourceCursorLoop() && passed;
    passed = SourceCursorDispatchLoop() && passed;
    if (!passed) return 1;
    std::puts("FexRootStateTest: PASS");
    return 0;
}
