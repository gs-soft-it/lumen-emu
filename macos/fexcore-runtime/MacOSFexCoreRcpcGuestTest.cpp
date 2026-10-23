// Lumen, 2026. SPDX-License-Identifier: GPL-2.0-only
#include "AndroidFexCoreRuntime.h"
#include "FexPerformanceTrace.h"
#include "MacOSFexCoreMemoryAccess.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using Lumen::MacOS::FexCore::DecodeMemoryAccess;
using Lumen::MacOS::FexCore::ResolveMemoryAccessAddress;
std::size_t page;
unsigned checks;
bool Check(bool passed, const char* message) {
    ++checks;
    if (!passed) std::fprintf(stderr, "RCPC guest: FAIL: %s\n", message);
    return passed;
}

bool DirectTraceBalanced(bool expectCall) {
    using namespace Lumen::Performance;
    const auto& phase = data.phases[static_cast<unsigned>(Phase::DirectUnaligned)];
    const auto calls = data.directUnalignedCalls.load();
    const auto handled = data.directUnalignedHandled.load();
    const auto events = phase.events.load();
    if (!Enabled()) return calls == 0 && handled == 0 && events == 0 &&
        phase.wall.load() == 0 && phase.cpu.load() == 0 && data.openScopes.load() == 0 && ledger.Depth() == 0;
    const bool passed = (!expectCall || calls != 0) && events == calls && handled <= calls &&
        data.openScopes.load() == 0 && ledger.Depth() == 0 &&
        data.overflow.load() == 0 && data.clockFailures.load() == 0;
    std::fprintf(stderr, "FEX_DIRECT_UNALIGNED_TRACE_TEST pid=%u events=%llu calls=%llu handled=%llu "
        "wall_ns=%llu cpu_ns=%llu open_scopes=%llu result=%s\n",
        static_cast<unsigned>(getpid()), static_cast<unsigned long long>(events),
        static_cast<unsigned long long>(calls), static_cast<unsigned long long>(handled),
        static_cast<unsigned long long>(phase.wall.load()), static_cast<unsigned long long>(phase.cpu.load()),
        static_cast<unsigned long long>(data.openScopes.load()), passed ? "PASS" : "FAIL");
    return passed;
}

class Mapping {
public:
    explicit Mapping(unsigned pages = 1) : size_(page * pages),
        data_(mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) {}
    ~Mapping() { if (Valid()) munmap(data_, size_); }
    bool Valid() const { return data_ != MAP_FAILED; }
    void* Data() const { return data_; }
    std::uintptr_t Address() const { return reinterpret_cast<std::uintptr_t>(data_); }
    bool Protect(int permission, std::size_t offset = 0, std::size_t size = 0) {
        return mprotect(static_cast<char*>(data_) + offset, size != 0 ? size : size_, permission) == 0;
    }
    bool Contains(std::uint64_t address, std::uint64_t size) const {
        return Valid() && size != 0 && size <= size_ && address >= Address() && address - Address() <= size_ - size;
    }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
private:
    std::size_t size_;
    void* data_;
};

struct Fixture {
    const Mapping* code;
    const Mapping* stack;
    const Mapping* data;
    bool dataAllowed {true};
    bool dataWritable {true};
    std::size_t dataLength;
    std::uintptr_t dataStart {};
};
thread_local const Fixture* active;

std::uint32_t QueryCode(void* opaque, std::uint64_t address,
    std::uint64_t* base, std::uint64_t* size, std::uint32_t* writable) noexcept {
    const auto& fixture = *static_cast<const Fixture*>(opaque);
    if (!fixture.code->Contains(address, 1)) return 0;
    *base = fixture.code->Address(); *size = page; *writable = 0;
    return 1;
}
std::uint32_t QueryData(std::uint64_t address, std::uint64_t size, std::uint32_t write) noexcept {
    if (active == nullptr || write > 1 || size == 0) return 0;
    if (active->stack->Contains(address, size)) return 1;
    const auto begin = active->dataStart != 0 ? active->dataStart : active->data->Address();
    return active->dataAllowed && (!write || active->dataWritable) &&
        address >= begin && size <= active->dataLength &&
        address - begin <= active->dataLength - size ? 1U : 0U;
}
std::uint32_t HostCall(void*, std::uint64_t, std::uint64_t*, std::uint64_t*) noexcept {
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
}

// Original x86-64 MOV/MOVZX, using disp32 so the address-mode pass must decide
// whether to retain the signed displacement in the generated TSO instruction.
std::vector<unsigned char> Program(unsigned width, int offset, bool write, bool stack = false) {
    std::vector<unsigned char> code;
    if (width == 8) code.push_back(0x48);
    if (write) {
        if (width == 2) code.push_back(0x66);
        code.push_back(width == 1 ? 0x88 : 0x89);
    } else {
        if (width <= 2) { code.push_back(0x0f); code.push_back(width == 1 ? 0xb6 : 0xb7); }
        else code.push_back(0x8b);
    }
    code.push_back(stack ? 0x84 : 0x87); // [rsp+disp32] or [rdi+disp32]
    if (stack) code.push_back(0x24);
    const auto displacement = static_cast<std::uint32_t>(offset);
    for (unsigned shift = 0; shift != 32; shift += 8) code.push_back(displacement >> shift);
    code.push_back(0xc3);
    return code;
}

bool Initialize() {
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    return LumenMacOSFexCoreRegisterDataAccessQueryV1(QueryData) == 1 &&
        LumenAndroidFexCoreRuntimeProbeV1(&status) == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1;
}
constexpr std::uint64_t RegisterCanary(unsigned index) {
    return UINT64_C(0x1011121314151617) + index * UINT64_C(0x0101010101010101);
}
std::uint32_t Run(Fixture& fixture, unsigned entry, std::uintptr_t base, std::uint64_t input,
    LumenAndroidFexCoreGuestStateV1* guest, std::uintptr_t simdInput = 0,
    std::uintptr_t simdOutput = 0, bool seedCanaries = false) {
    *guest = {};
    guest->structSize = sizeof(*guest); guest->abiVersion = 1;
    guest->rip = fixture.code->Address() + entry;
    if (seedCanaries) for (unsigned index = 0; index != 16; ++index)
        guest->gprs[index] = RegisterCanary(index);
    guest->gprs[4] = fixture.stack->Address() + page - 8;
    guest->gprs[7] = base; guest->gprs[0] = input;
    guest->gprs[6] = simdInput; guest->gprs[2] = simdOutput;
    const auto root = LumenAndroidFexCoreRootReturnAddressV1();
    std::memcpy(reinterpret_cast<void*>(guest->gprs[4]), &root, sizeof(root));
    active = &fixture;
    const auto status = LumenAndroidFexCoreRunGuestV1(guest, HostCall, QueryCode, &fixture);
    active = nullptr;
    return status;
}

enum class Negative { AlignmentDenied, HelperGranuleDenied, ReadOnly, PageCross, Inaccessible };

bool RecordContains(const char* record, const char* token) {
    if (record == nullptr) return false;
    const auto* end = std::strchr(record, '\n');
    const auto* match = std::strstr(record, token);
    return match != nullptr && (end == nullptr || match < end);
}
bool Hex(const char* record, const char* name, unsigned long long* value) {
    const auto* field = record != nullptr ? std::strstr(record, name) : nullptr;
    return field != nullptr && RecordContains(record, name) &&
        std::sscanf(field + std::strlen(name), "%llx", value) == 1;
}

bool NegativeAndCodegen(unsigned width, int offset, bool write, Negative kind, bool rcpc) {
    Mapping code, data(2), stack;
    if (!code.Valid() || !data.Valid() || !stack.Valid()) return Check(false, "negative mappings");
    const auto bytes = Program(width, offset, write);
    std::memcpy(code.Data(), bytes.data(), bytes.size());
    std::memset(data.Data(), 0xa5, page);
    std::uintptr_t effective = data.Address() + 511; // crossing a 16-byte granule
    if (kind == Negative::ReadOnly) effective = data.Address() + 512;
    if (kind == Negative::PageCross) effective = data.Address() + page - 3;
    if (kind == Negative::Inaccessible) effective = data.Address() + page + 32;
    const auto base = static_cast<std::uintptr_t>(static_cast<std::int64_t>(effective) - offset);
    const auto* directSelector = std::getenv("LUMEN_FEX_MACOS_DIRECT_UNALIGNED");
    const auto* traceSelector = std::getenv("LUMEN_FEX_PERFORMANCE_TRACE");
    const bool traceRequested = traceSelector != nullptr && std::strcmp(traceSelector, "1") == 0;
    const bool direct = (directSelector == nullptr || std::strcmp(directSelector, "1") == 0) &&
        Lumen::MacOS::FexCore::IsDirectUnalignedScalarAccess(effective, width);
    if (!code.Protect(PROT_READ) ||
        (kind == Negative::ReadOnly && !data.Protect(PROT_READ)) ||
        ((kind == Negative::PageCross || kind == Negative::Inaccessible) && !data.Protect(PROT_NONE, page, page)))
        return Check(false, "negative page permissions");
    int pipeFd[2];
    if (pipe(pipeFd) != 0) return Check(false, "negative pipe");
    // The parent has not initialized FEX yet: each witness gets fresh runtime
    // configuration and avoids forking live translator threads/locks.
    const auto child = fork();
    if (child < 0) { close(pipeFd[0]); close(pipeFd[1]); return Check(false, "negative fork"); }
    if (child == 0) {
        close(pipeFd[0]);
        if (dup2(pipeFd[1], STDERR_FILENO) < 0 || close(pipeFd[1]) != 0 ||
            setenv("LUMEN_MACOS_FEXCORE_FAULT_CONTEXT", "1", 1) != 0 ||
            setenv("LUMEN_MACOS_FEXCORE_FAULT_STATIC_GPRS", "1", 1) != 0 ||
            setenv("LUMEN_FEX_PERFORMANCE_TRACE", traceRequested ? "1" : "0", 1) != 0) _exit(96);
        alarm(8);
        Fixture fixture {&code, &stack, &data, kind != Negative::AlignmentDenied,
            kind != Negative::ReadOnly, page};
        if (kind == Negative::HelperGranuleDenied) {
            fixture.dataStart = effective;
            fixture.dataLength = width;
        }
        if (!Initialize()) _exit(95);
        LumenAndroidFexCoreGuestStateV1 guest {};
        const auto result = Run(fixture, 0, base, UINT64_C(0x1122334455667788), &guest);
        bool unchanged = true;
        const auto* payload = static_cast<const unsigned char*>(data.Data());
        for (std::size_t i = 0; i != page; ++i) unchanged &= payload[i] == 0xa5;
        const bool precise = !direct || (guest.faultSignal == SIGSEGV &&
            guest.faultAddress == effective && guest.rip == code.Address());
        if (direct) std::fprintf(stderr,
            "[FEX_DIRECT_UNALIGNED_DENIED_V1] precise=%u unchanged=%u\n",
            precise ? 1U : 0U, unchanged ? 1U : 0U);
        const bool traceBalanced = DirectTraceBalanced(direct);
        _exit(result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 && unchanged && precise && traceBalanced ? 0 : 94);
    }
    close(pipeFd[1]);
    std::string report;
    std::array<char, 4096> buffer;
    for (;;) {
        const auto received = read(pipeFd[0], buffer.data(), buffer.size());
        if (received > 0) { report.append(buffer.data(), static_cast<std::size_t>(received)); continue; }
        if (received < 0 && errno == EINTR) continue;
        break;
    }
    close(pipeFd[0]);
    int status {};
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    bool passed = waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (traceRequested) {
        const auto* trace = std::strstr(report.c_str(), "FEX_DIRECT_UNALIGNED_TRACE_TEST ");
        passed &= RecordContains(trace, "result=PASS");
        if (trace != nullptr) {
            const auto* end = std::strchr(trace, '\n');
            std::fprintf(stderr, "%.*s\n", static_cast<int>(end != nullptr ? end - trace : std::strlen(trace)), trace);
        }
    }
    const char* host = report.c_str();
    while ((host = std::strstr(host, "[MACOS_FEXCORE_FAULT_HOST_CONTEXT_V1]")) != nullptr &&
        !RecordContains(host, "scope=guest")) ++host;
    unsigned long long instruction {};
    passed &= RecordContains(host, "inJit=1") && RecordContains(host, "faultInstructionValid=1") &&
        Hex(host, "faultInstruction=", &instruction);
    if (direct) {
        passed &= instruction == Lumen::MacOS::FexCore::DirectUnalignedDeniedInstruction &&
            report.find("[FEX_DIRECT_UNALIGNED_DENIED_V1] precise=1 unchanged=1") != std::string::npos;
        if (!passed) std::fprintf(stderr, "Direct guest denial width=%u offset=%d write=%u negative=%u child=%d\n%s",
            width, offset, write ? 1U : 0U, static_cast<unsigned>(kind), status, report.c_str());
        return Check(passed, "direct helper rejects before effects with precise guest RIP/address");
    }
    const auto access = DecodeMemoryAccess(static_cast<std::uint32_t>(instruction));
    unsigned long long hostBase {};
    std::array<char, 16> baseName {};
    if (access.baseRegister == 31) std::snprintf(baseName.data(), baseName.size(), "hostSp=");
    else std::snprintf(baseName.data(), baseName.size(), " x%u=", access.baseRegister);
    std::uintptr_t resolved {};
    const auto expectedClass = write ? 0x19000000U : 0x19400000U;
    const auto baselineClass = write ? 0x089ffc00U : 0x08dffc00U;
    passed &= access.valid && access.width == width && access.write == write &&
        access.byteOffset == (rcpc ? offset : 0) && Hex(host, baseName.data(), &hostBase) &&
        ResolveMemoryAccessAddress(access, hostBase, &resolved) && resolved == effective &&
        (rcpc ? (instruction & 0x3fe00c00U) == expectedClass :
                (instruction & 0x3ffffc00U) == baselineClass);
    if (!passed) std::fprintf(stderr, "RCPC guest witness width=%u offset=%d write=%u negative=%u child=%d\n%s",
        width, offset, write ? 1U : 0U, static_cast<unsigned>(kind), status, report.c_str());
    return Check(passed, "generated TSO opcode, signed effective address and denied fault effects");
}

bool Payloads() {
    Mapping code, data, stack;
    if (!code.Valid() || !data.Valid() || !stack.Valid()) return Check(false, "payload mappings");
    constexpr std::array<unsigned, 4> widths {1, 2, 4, 8};
    constexpr std::array<int, 5> offsets {-256, -1, 0, 1, 255};
    constexpr std::array<unsigned, 4> phases {0, 1, 9, 15};
    unsigned entry {};
    for (const auto width : widths) for (const auto offset : offsets) for (bool write : {false, true}) {
        const auto bytes = Program(width, offset, write);
        std::memcpy(static_cast<unsigned char*>(code.Data()) + entry, bytes.data(), bytes.size());
        entry += 32;
    }
    const auto stackWrite = Program(8, -255, true, true);
    const auto stackRead = Program(8, -255, false, true);
    std::memcpy(static_cast<unsigned char*>(code.Data()) + entry, stackWrite.data(), stackWrite.size());
    std::memcpy(static_cast<unsigned char*>(code.Data()) + entry + 32, stackRead.data(), stackRead.size());
    if (!code.Protect(PROT_READ)) return Check(false, "payload code immutable");
    Fixture fixture {&code, &stack, &data, true, true, page};
    bool passed = true;
    unsigned position {};
    constexpr std::uint64_t value = UINT64_C(0xf1e2d3c4b5a69788);
    for (const auto width : widths) for (const auto offset : offsets) {
        const auto mask = width == 8 ? UINT64_MAX : (UINT64_C(1) << (width * 8)) - 1;
        for (const auto phase : phases) {
            const auto effective = data.Address() + 512 + phase;
            const auto base = static_cast<std::uintptr_t>(static_cast<std::int64_t>(effective) - offset);
            std::memset(data.Data(), 0xa5, page);
            std::memcpy(reinterpret_cast<void*>(effective), &value, width);
            LumenAndroidFexCoreGuestStateV1 guest {};
            const auto loadStatus = Run(fixture, position, base, 0, &guest);
            passed = Check(loadStatus == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
                guest.rip == LumenAndroidFexCoreRootReturnAddressV1() && guest.gprs[0] == (value & mask) &&
                guest.gprs[4] == stack.Address() + page, "translated load payload and guest RSP") && passed;
            std::memset(data.Data(), 0xa5, page);
            const auto storeStatus = Run(fixture, position + 32, base, value, &guest);
            bool payload = true;
            const auto* actual = static_cast<const unsigned char*>(data.Data());
            const auto* desired = reinterpret_cast<const unsigned char*>(&value);
            for (std::size_t byte = 0; byte != page; ++byte) {
                const auto start = effective - data.Address();
                const auto expected = byte >= start && byte - start < width ? desired[byte - start] : 0xa5;
                payload &= actual[byte] == expected;
            }
            passed = Check(storeStatus == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
                guest.rip == LumenAndroidFexCoreRootReturnAddressV1() && payload &&
                guest.gprs[4] == stack.Address() + page, "translated store full payload/canaries and guest RSP") && passed;
        }
        position += 64;
    }
    LumenAndroidFexCoreGuestStateV1 guest {};
    passed = Check(Run(fixture, entry, 0, value, &guest) == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
        guest.rip == LumenAndroidFexCoreRootReturnAddressV1() &&
        guest.gprs[4] == stack.Address() + page, "guest RSP-relative disaligned store completes") && passed;
    passed = Check(Run(fixture, entry + 32, 0, 0, &guest) == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
        guest.rip == LumenAndroidFexCoreRootReturnAddressV1() &&
        guest.gprs[0] == value && guest.gprs[4] == stack.Address() + page,
        "guest RSP-relative disaligned load and return preserve guest RSP") && passed;
    return passed;
}

bool StatePreservation() {
    Mapping code, data, stack;
    if (!code.Valid() || !data.Valid() || !stack.Valid()) return Check(false, "preservation mappings");
    const auto publication = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    if (publication == 0) return Check(false, "preservation code publication barrier");
    unsigned entry {};
    for (bool write : {false, true}) {
        std::vector<unsigned char> program;
        const auto simd = [&](unsigned reg, bool store) {
            program.push_back(0xf3);
            if (reg >= 8) program.push_back(0x44); // REX.R for xmm8..15
            program.push_back(0x0f);
            program.push_back(store ? 0x7f : 0x6f);
            program.push_back(0x80 | ((reg & 7) << 3) | (store ? 2 : 6)); // rdx/rsi+disp32
            const auto displacement = reg * 16;
            for (unsigned shift = 0; shift != 32; shift += 8)
                program.push_back(displacement >> shift);
        };
        for (unsigned reg = 0; reg != 16; ++reg) simd(reg, false);
        const unsigned char setup[] {
            0x0f, 0xae, 0x96, 0x00, 0x01, 0x00, 0x00, // ldmxcsr [rsi+256]
            0x48, 0x39, 0xdb, 0xf9,             // cmp rbx,rbx; stc
        };
        program.insert(program.end(), std::begin(setup), std::end(setup));
        auto access = Program(8, -1, write);
        access.pop_back();
        program.insert(program.end(), access.begin(), access.end());
        program.push_back(0x9c); program.push_back(0x59); // pushfq; pop rcx
        for (unsigned reg = 0; reg != 16; ++reg) simd(reg, true);
        const unsigned char tail[] {
            0x0f, 0xae, 0x9a, 0x00, 0x01, 0x00, 0x00, 0xc3, // stmxcsr [rdx+256]; ret
        };
        program.insert(program.end(), std::begin(tail), std::end(tail));
        std::memcpy(static_cast<unsigned char*>(code.Data()) + entry, program.data(), program.size());
        entry += 512;
    }
    const bool published = code.Protect(PROT_READ);
    // Prior payload fixtures may have occupied the same virtual address. A
    // fresh mapping must invalidate those old translations before admission.
    LumenAndroidFexCoreEndGuestMemoryMutationV1(publication, code.Address(), page, published ? 1U : 0U);
    if (!published) return Check(false, "preservation code immutable");
    Fixture fixture {&code, &stack, &data, true, true, page};
    const auto effective = data.Address() + 1535;
    constexpr std::uint64_t value = UINT64_C(0x7766554433221100);
    constexpr std::uint32_t mxcsr = 0x5f80; // round toward +infinity, exceptions masked
    bool passed = true;
    for (bool write : {false, true}) {
        std::memset(data.Data(), 0xa5, page);
        auto* payload = static_cast<unsigned char*>(data.Data());
        for (unsigned byte = 0; byte != 256; ++byte) payload[512 + byte] = byte * 7 + 1;
        std::memcpy(payload + 768, &mxcsr, sizeof(mxcsr));
        std::memcpy(reinterpret_cast<void*>(effective), &value, sizeof(value));
        LumenAndroidFexCoreGuestStateV1 guest {};
        const auto result = Run(fixture, write ? 512 : 0, effective + 1, value, &guest,
            data.Address() + 512, data.Address() + 1024, true);
        std::uint32_t observedMxcsr {};
        std::memcpy(&observedMxcsr, payload + 1280, sizeof(observedMxcsr));
        bool registers = guest.gprs[0] == value && guest.gprs[2] == data.Address() + 1024 &&
            guest.gprs[4] == stack.Address() + page && guest.gprs[6] == data.Address() + 512 &&
            guest.gprs[7] == effective + 1;
        for (unsigned index : {3U, 5U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U})
            registers &= guest.gprs[index] == RegisterCanary(index);
        constexpr std::uint64_t arithmeticFlags = 0x8d5; // OF/SF/ZF/AF/PF/CF
        if (result != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 || !registers ||
            (guest.gprs[1] & arithmeticFlags) != 0x45 ||
            std::memcmp(payload + 512, payload + 1024, 256) != 0 || observedMxcsr != mxcsr) {
            std::fprintf(stderr, "preservation write=%u status=%u registers=%u flags=%llx simd=%u mxcsr=%x expected=%x\n",
                write ? 1U : 0U, result, registers ? 1U : 0U,
                static_cast<unsigned long long>(guest.gprs[1]),
                std::memcmp(payload + 512, payload + 1024, 256) == 0, observedMxcsr, mxcsr);
            for (unsigned index = 0; index != 16; ++index)
                std::fprintf(stderr, "r%u=%llx ", index, static_cast<unsigned long long>(guest.gprs[index]));
            std::fputc('\n', stderr);
        }
        passed = Check(result == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
            guest.rip == LumenAndroidFexCoreRootReturnAddressV1() && registers &&
            (guest.gprs[1] & arithmeticFlags) == 0x45 &&
            std::memcmp(payload + 512, payload + 1024, 256) == 0 && observedMxcsr == mxcsr,
            "split-granule scalar callback preserves nonzero GPRs, arithmetic flags, all 16 XMM registers and MXCSR") && passed;
    }
    return passed;
}
bool Capability(const char* name) {
    std::uint64_t value {};
    auto size = sizeof(value);
    return sysctlbyname(name, &value, &size, nullptr, 0) == 0 && value == 1;
}
} // namespace

int main() {
    page = static_cast<std::size_t>(getpagesize());
    const auto* selected = std::getenv("LUMEN_FEX_MACOS_RCPC");
    if (selected == nullptr || (std::strcmp(selected, "0") != 0 && std::strcmp(selected, "1") != 0)) {
        std::fprintf(stderr, "RCPC guest test requires LUMEN_FEX_MACOS_RCPC=0|1\n"); return 1;
    }
    const bool rcpc = std::strcmp(selected, "1") == 0;
    if (rcpc && (!Capability("hw.optional.arm.FEAT_LRCPC") || !Capability("hw.optional.arm.FEAT_LRCPC2"))) return 77;
    bool passed = true;
    for (const auto offset : {-256, -1, 0, 1, 255}) for (bool write : {false, true})
        passed = NegativeAndCodegen(8, offset, write, Negative::AlignmentDenied, rcpc) && passed;
    for (const auto width : {2U, 4U}) for (bool write : {false, true})
        passed = NegativeAndCodegen(width, -1, write, Negative::AlignmentDenied, rcpc) && passed;
    for (const auto offset : {-1, 1}) {
        passed = NegativeAndCodegen(8, offset, true, Negative::ReadOnly, rcpc) && passed;
        passed = NegativeAndCodegen(8, offset, true, Negative::PageCross, rcpc) && passed;
    }
    for (bool write : {false, true})
        passed = NegativeAndCodegen(8, -1, write, Negative::HelperGranuleDenied, rcpc) && passed;
    passed = NegativeAndCodegen(1, -1, true, Negative::ReadOnly, rcpc) && passed;
    passed = NegativeAndCodegen(1, -1, false, Negative::Inaccessible, rcpc) && passed;
    const bool codeWitnesses = passed;
    passed = Check(Initialize(), "product runtime initializes after isolated fault witnesses") && passed;
    if (passed) passed = Payloads() && StatePreservation() && passed;
    const auto* directSelector = std::getenv("LUMEN_FEX_MACOS_DIRECT_UNALIGNED");
    const bool direct = directSelector == nullptr || std::strcmp(directSelector, "1") == 0;
    passed = Check(DirectTraceBalanced(direct), "real helper trace balances completed calls and stays inert while OFF") && passed;
    std::printf("MacOSFexCoreRcpcGuestTest: %s rcpc=%u direct_unaligned=%u checks=%u hardware_tso=0 generated_code_witness=%u gameplay_qualified=0\n",
        passed ? "PASS" : "FAIL", rcpc ? 1U : 0U, direct ? 1U : 0U, checks, codeWitnesses ? 1U : 0U);
    return passed ? 0 : 1;
}
