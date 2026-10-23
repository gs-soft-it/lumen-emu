#include "AndroidFexCoreRuntime.h"
#include "MacOSFexCoreSignalPolicy.h"
#include "MacOSFexCoreFaultStaticGprs.h"
#include "MacOSFexCoreMemoryAccess.h"
#include <FEXCore/Utils/AllocatorHooks.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cfenv>
#include <cstdio>
#include <cstring>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
constexpr std::size_t Page = 16384;
constexpr std::size_t Workers = 4;
constexpr std::size_t Iterations = 16;
constexpr std::size_t CacheBlocks = 1048576;
constexpr std::size_t BlockBytes = 32;
constexpr std::size_t BlocksPerCallback = 4096;
using WriteScope = FEXCore::Allocator::ScopedJITWriteProtection;

bool Expect(bool condition, const char* detail) {
    if (!condition) std::fprintf(stderr, "JIT gate: FAIL: %s\n", detail);
    return condition;
}

volatile sig_atomic_t signalDepth {-1};
void CaptureWriteDepth(int) {
    signalDepth = static_cast<sig_atomic_t>(FEXCore::Allocator::JITWriteScopeDepth);
}

struct DenialEvidence {
    int signal {};
    int code {};
    int ready {};
    std::uint32_t writeDepth {};
    std::uintptr_t address {};
    std::uintptr_t pc {};
    std::uint32_t esr {};
};
int denialEvidenceFd {-1};
volatile sig_atomic_t denialReady {};

void CaptureDenial(int signal, siginfo_t* info, void* rawContext) {
    const auto* context = static_cast<const ucontext_t*>(rawContext);
    const DenialEvidence evidence {
        signal, info != nullptr ? info->si_code : 0, denialReady,
        FEXCore::Allocator::JITWriteScopeDepth,
        info != nullptr ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0,
        context != nullptr ? context->uc_mcontext->__ss.__pc : 0,
        context != nullptr ? context->uc_mcontext->__es.__esr : 0,
    };
    const auto written = write(denialEvidenceFd, &evidence, sizeof(evidence));
    _exit(written == sizeof(evidence) ? 0 : 97);
}

bool FaultsWhen(bool execute, void* mapping) {
    int evidencePipe[2];
    if (pipe(evidencePipe) != 0) return Expect(false, "create fault evidence pipe");
    const pid_t child = fork();
    if (child < 0) {
        close(evidencePipe[0]); close(evidencePipe[1]);
        return Expect(false, "fork fault probe");
    }
    if (child == 0) {
        close(evidencePipe[0]);
        denialEvidenceFd = evidencePipe[1];
        denialReady = 0;
        const rlimit noCore {0, 0};
        setrlimit(RLIMIT_CORE, &noCore);
        struct sigaction action {};
        action.sa_sigaction = CaptureDenial;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGBUS, &action, nullptr) != 0 ||
            sigaction(SIGSEGV, &action, nullptr) != 0) _exit(96);
        sigset_t faults;
        sigemptyset(&faults);
        sigaddset(&faults, SIGBUS);
        sigaddset(&faults, SIGSEGV);
        sigaddset(&faults, SIGALRM);
        if (pthread_sigmask(SIG_UNBLOCK, &faults, nullptr) != 0) _exit(98);
        signal(SIGALRM, SIG_DFL);
        alarm(5);
        // Establish in this child that the inherited mapping is executable
        // and contains the published result. A fault before this control is
        // evidence of a broken probe, not evidence of the requested denial.
        pthread_jit_write_protect_np(1);
        if (FEXCore::Allocator::JITWriteScopeDepth != 0 ||
            reinterpret_cast<unsigned (*)()>(mapping)() != 42) _exit(95);
        if (execute) {
            WriteScope write;
            *static_cast<volatile std::uint32_t*>(mapping) = 0x52800540U;
            denialReady = 1;
            std::atomic_signal_fence(std::memory_order_seq_cst);
            reinterpret_cast<void (*)()>(mapping)();
        } else {
            denialReady = 1;
            std::atomic_signal_fence(std::memory_order_seq_cst);
            *static_cast<volatile std::uint32_t*>(mapping) = 0;
        }
        _exit(99);
    }
    close(evidencePipe[1]);
    int status {};
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    DenialEvidence evidence {};
    ssize_t received;
    do { received = read(evidencePipe[0], &evidence, sizeof(evidence)); }
    while (received < 0 && errno == EINTR);
    close(evidencePipe[0]);
    const bool denied = waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        received == sizeof(evidence) && evidence.ready == 1 &&
        (evidence.signal == SIGBUS || evidence.signal == SIGSEGV) &&
        evidence.address == reinterpret_cast<std::uintptr_t>(mapping) &&
        evidence.writeDepth == (execute ? 1U : 0U) &&
        (!execute || evidence.pc == reinterpret_cast<std::uintptr_t>(mapping));
    std::fprintf(stderr,
        "JIT denial: operation=%s pid=%d status=0x%x terminationSignal=%d "
        "evidenceBytes=%zd signal=%d code=%d ready=%d depth=%u addressExact=%u "
        "pcAtMapping=%u esr=0x%x expectedFault=%u\n",
        execute ? "execute-in-write" : "write-in-execute", child, status,
        WIFSIGNALED(status) ? WTERMSIG(status) : 0, received, evidence.signal,
        evidence.code, evidence.ready, evidence.writeDepth,
        evidence.address == reinterpret_cast<std::uintptr_t>(mapping),
        evidence.pc == reinterpret_cast<std::uintptr_t>(mapping), evidence.esr, denied);
    return denied;
}

bool CheckWx() {
    if (!pthread_jit_write_protect_supported_np()) return false;
    pthread_jit_write_protect_np(1);
    void* mapping = mmap(nullptr, Page, PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    if (mapping == MAP_FAILED) return false;
    const std::array<std::uint32_t, 2> code {0x52800540U, 0xd65f03c0U}; // mov w0,#42; ret
    struct sigaction action {}, previousAction {};
    action.sa_handler = CaptureWriteDepth;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, &previousAction) != 0) {
        munmap(mapping, Page);
        return false;
    }
    sigset_t observedSignal, previousMask;
    sigemptyset(&observedSignal);
    sigaddset(&observedSignal, SIGUSR1);
    if (pthread_sigmask(SIG_UNBLOCK, &observedSignal, &previousMask) != 0) {
        sigaction(SIGUSR1, &previousAction, nullptr);
        munmap(mapping, Page);
        return Expect(false, "unblock the signal used by the scope probe");
    }
    bool ok = FEXCore::Allocator::JITWriteScopeDepth == 0;
    {
        WriteScope outer;
        ok &= FEXCore::Allocator::JITWriteScopeDepth == 1;
        {
            WriteScope inner;
            ok &= FEXCore::Allocator::JITWriteScopeDepth == 2;
            std::memcpy(mapping, code.data(), sizeof(code));
            ok &= Expect(raise(SIGUSR1) == 0 && signalDepth == 2,
                         "signal delivered inside nested write scope");
        }
        ok &= FEXCore::Allocator::JITWriteScopeDepth == 1;
        sys_icache_invalidate(mapping, sizeof(code));
    }
    ok &= FEXCore::Allocator::JITWriteScopeDepth == 0;
    ok &= Expect(raise(SIGUSR1) == 0 && signalDepth == 0,
                 "signal delivered after write scope closes");
    ok &= pthread_sigmask(SIG_SETMASK, &previousMask, nullptr) == 0;
    ok &= sigaction(SIGUSR1, &previousAction, nullptr) == 0;
    ok &= reinterpret_cast<unsigned (*)()>(mapping)() == 42;
    ok &= Expect(FaultsWhen(false, mapping), "store must fault in execute mode");
    ok &= Expect(FaultsWhen(true, mapping), "instruction fetch must fault in write mode");
    // A writable publisher thread must not revoke execution on another thread.
    // Both threads rendezvous while the publisher scope remains open; the
    // executable bytes remain immutable during this check.
    std::atomic<bool> writable {}, executed {};
    std::atomic<bool> threadOk {true};
    std::thread executor([&] {
        pthread_jit_write_protect_np(1);
        while (!writable.load(std::memory_order_acquire)) std::this_thread::yield();
        threadOk = FEXCore::Allocator::JITWriteScopeDepth == 0 &&
            reinterpret_cast<unsigned (*)()>(mapping)() == 42;
        executed.store(true, std::memory_order_release);
    });
    {
        WriteScope write;
        writable.store(true, std::memory_order_release);
        while (!executed.load(std::memory_order_acquire)) std::this_thread::yield();
        ok &= FEXCore::Allocator::JITWriteScopeDepth == 1;
    }
    executor.join();
    ok &= threadOk && FEXCore::Allocator::JITWriteScopeDepth == 0;
    ok &= munmap(mapping, Page) == 0;
    return ok;
}

struct Stack {
    void* mapping {mmap(nullptr, Page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)};
    std::uint64_t base {};
    Stack* previous {};
    static inline thread_local Stack* current {};
    Stack() {
        if (mapping != MAP_FAILED &&
            mprotect(static_cast<char*>(mapping) + Page, Page, PROT_READ | PROT_WRITE) == 0)
            base = reinterpret_cast<std::uint64_t>(mapping) + Page;
        previous = current;
        current = this;
    }
    ~Stack() {
        current = previous;
        if (mapping != MAP_FAILED) munmap(mapping, Page * 3);
    }
    bool Contains(std::uint64_t address, std::size_t size) const {
        return base != 0 && size <= Page && address >= base && address - base <= Page - size;
    }
};
std::uint32_t QueryDataAccess(std::uint64_t address, std::uint64_t size,
                              std::uint32_t write) noexcept {
    return write <= 1 && size != 0 && Stack::current != nullptr &&
        Stack::current->Contains(address, size) ? 1U : 0U;
}
struct Fixture {
    std::uint64_t code {};
    Stack* stack {};
    unsigned depth {};
    unsigned calls {};
    bool ok {true};
    std::size_t size {Page};
    std::uint64_t previousCache {};
    unsigned cacheChanges {};
    bool countCache {};
    // Optional gate for a holder that needs the target parked inside this
    // host call while another thread arms and cancels a boundary interrupt.
    std::atomic<bool>* hostCallGate {};
    std::atomic<bool>* hostCallEntered {};
    // Reported writability of the fixture's executable range. The immutable-RX
    // policy keeps this zero for every product path; the dedicated console
    // experiment gate sets it to one.
    std::uint32_t writable {};
    bool armBoundaryAfterReturn {};
    bool armBoundaryEveryReturn {};
    unsigned boundaryArmCall {1};
    std::uint64_t boundaryToken {};
};
void HostTarget() {}

std::uint32_t Query(void* opaque, std::uint64_t address, std::uint64_t* base,
                    std::uint64_t* size, std::uint32_t* writable) noexcept {
    const auto* fixture = static_cast<Fixture*>(opaque);
    if (address < fixture->code || address - fixture->code >= fixture->size) return 0;
    *base = fixture->code; *size = fixture->size; *writable = fixture->writable;
    return 1;
}

std::uint32_t HostCall(void*, std::uint64_t, std::uint64_t*, std::uint64_t*) noexcept;

bool Run(Fixture& fixture, std::uint64_t entry, std::uint64_t expected,
         std::uint64_t argument = 0) {
    Stack stack;
    if (!stack.base) return false;
    fixture.stack = &stack;
    LumenAndroidFexCoreGuestStateV1 guest {};
    guest.structSize = sizeof(guest); guest.abiVersion = 1; guest.rip = entry;
    guest.gprs[4] = stack.base + Page - 8;
    guest.gprs[7] = argument;
    const auto root = LumenAndroidFexCoreRootReturnAddressV1();
    std::memcpy(reinterpret_cast<void*>(guest.gprs[4]), &root, sizeof(root));
    std::uint64_t x18Before {}, x18After {};
    asm volatile("mov %0, x18" : "=r"(x18Before));
    const auto result = LumenAndroidFexCoreRunGuestV1(&guest, HostCall, Query, &fixture);
    asm volatile("mov %0, x18" : "=r"(x18After));
    const bool ok = result == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 && guest.gprs[0] == expected &&
        guest.rip == root && fixture.ok && x18Before == x18After &&
        FEXCore::Allocator::JITWriteScopeDepth == 0;
    if (!ok) std::fprintf(stderr,
        "JIT guest: code=%u result=%llu expected=%llu fault=%u address=0x%llx calls=%u\n",
        result, static_cast<unsigned long long>(guest.gprs[0]),
        static_cast<unsigned long long>(expected), guest.faultSignal,
        static_cast<unsigned long long>(guest.faultAddress), fixture.calls);
    return ok;
}

struct GuestExceptionDiagnosticCase {
    const char* name;
    std::array<unsigned char, 2> instruction;
    std::size_t instructionSize;
    int signal;
    const char* route;
    unsigned trapNo;
    unsigned signalCode;
    unsigned errorCode;
};

const char* FindGuestFaultContextRecord(const char* report) {
    const char* record = report;
    while ((record = std::strstr(
                record, "[MACOS_FEXCORE_FAULT_CONTEXT_V4]")) != nullptr) {
        const char* lineEnd = std::strchr(record, '\n');
        const char* guestScope = std::strstr(record, "scope=guest");
        if (guestScope != nullptr &&
            (lineEnd == nullptr || guestScope < lineEnd)) return record;
        ++record;
    }
    return nullptr;
}

bool FaultContextRecordContains(const char* record, const char* token) {
    if (record == nullptr) return false;
    const char* found = std::strstr(record, token);
    const char* lineEnd = std::strchr(record, '\n');
    return found != nullptr && (lineEnd == nullptr || found < lineEnd);
}

bool CheckGuestExceptionDiagnosticCase(
        bool enabled, const GuestExceptionDiagnosticCase& testCase) {
    void* mapping = mmap(nullptr, Page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return false;
    std::memcpy(mapping, testCase.instruction.data(),
                testCase.instructionSize);
    if (mprotect(mapping, Page, PROT_READ) != 0) {
        munmap(mapping, Page);
        return false;
    }

    int reportPipe[2];
    if (pipe(reportPipe) != 0) {
        munmap(mapping, Page);
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        close(reportPipe[0]);
        close(reportPipe[1]);
        munmap(mapping, Page);
        return false;
    }
    if (child == 0) {
        close(reportPipe[0]);
        const int environmentResult = enabled
            ? setenv("LUMEN_MACOS_FEXCORE_FAULT_CONTEXT", "1", 1)
            : unsetenv("LUMEN_MACOS_FEXCORE_FAULT_CONTEXT");
        if (dup2(reportPipe[1], STDERR_FILENO) < 0 ||
            close(reportPipe[1]) != 0 || environmentResult != 0) {
            _exit(96);
        }
        alarm(5);
        Stack stack;
        if (!stack.base) _exit(92);
        Fixture fixture {reinterpret_cast<std::uint64_t>(mapping)};
        LumenAndroidFexCoreGuestStateV1 guest {};
        guest.structSize = sizeof(guest);
        guest.abiVersion = 1;
        guest.rip = fixture.code;
        guest.gprs[4] = stack.base + Page - sizeof(std::uint64_t);
        const auto root = LumenAndroidFexCoreRootReturnAddressV1();
        std::memcpy(reinterpret_cast<void*>(guest.gprs[4]), &root,
                    sizeof(root));
        const auto result = LumenAndroidFexCoreRunGuestV1(
            &guest, HostCall, Query, &fixture);
        const bool expectedFault =
            result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
            guest.faultSignal ==
                static_cast<std::uint32_t>(testCase.signal);
        _exit(expectedFault ? 0 : 91);
    }

    close(reportPipe[1]);
    std::array<char, 8192> report {};
    std::size_t total {};
    while (total < report.size() - 1) {
        const auto received = read(reportPipe[0], report.data() + total,
                                   report.size() - 1 - total);
        if (received > 0) {
            total += static_cast<std::size_t>(received);
            continue;
        }
        if (received < 0 && errno == EINTR) continue;
        break;
    }
    close(reportPipe[0]);
    int status {};
    pid_t waited;
    do { waited = waitpid(child, &status, 0); }
    while (waited < 0 && errno == EINTR);

    const char* const record = FindGuestFaultContextRecord(report.data());
    std::array<char, 48> expectedSignal {};
    std::array<char, 48> expectedRoute {};
    std::array<char, 48> expectedTrap {};
    std::array<char, 48> expectedSignalCode {};
    std::array<char, 64> expectedErrorCode {};
    std::snprintf(expectedSignal.data(), expectedSignal.size(),
        "syncFaultSignal=%d", testCase.signal);
    std::snprintf(expectedRoute.data(), expectedRoute.size(),
        "guestSignalRoute=%s", testCase.route);
    std::snprintf(expectedTrap.data(), expectedTrap.size(),
        "syncFaultTrapNo=%u", testCase.trapNo);
    std::snprintf(expectedSignalCode.data(), expectedSignalCode.size(),
        "syncFaultSiCode=%u", testCase.signalCode);
    std::snprintf(expectedErrorCode.data(), expectedErrorCode.size(),
        "syncFaultErrorCode=0x%04x", testCase.errorCode);
    const bool recordValid = enabled
        ? record != nullptr &&
            FaultContextRecordContains(record, expectedRoute.data()) &&
            FaultContextRecordContains(record, "syncFaultGenerated=1") &&
            FaultContextRecordContains(record, "syncFaultMetadataValid=1") &&
            FaultContextRecordContains(record, expectedSignal.data()) &&
            FaultContextRecordContains(record, expectedTrap.data()) &&
            FaultContextRecordContains(record, expectedSignalCode.data()) &&
            FaultContextRecordContains(record, expectedErrorCode.data()) &&
            FaultContextRecordContains(record, "guestCsIndex=0x0030") &&
            FaultContextRecordContains(record, "guestIoplBit12=0")
        : std::strstr(report.data(),
              "[MACOS_FEXCORE_FAULT_CONTEXT_V4]") == nullptr;
    const bool valid = waited == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0 && recordValid;
    if (!valid) {
        std::fprintf(stderr,
            "guest exception diagnostic: case=%s enabled=%u status=0x%x "
            "bytes=%zu report=%s\n",
            testCase.name, static_cast<unsigned>(enabled), status, total,
            report.data());
    }
    return munmap(mapping, Page) == 0 && valid;
}

bool CheckGuestExceptionDiagnostic() {
    constexpr GuestExceptionDiagnosticCase invalidOpcode {
        "invalid-opcode", {0x0f, 0x0b}, 2, SIGILL, "ill", 6, 2, 0,
    };
    constexpr GuestExceptionDiagnosticCase privilegedHalt {
        "privileged-halt", {0xf4, 0x90}, 1, SIGSEGV, "segv", 13, 0x80, 0,
    };
    return CheckGuestExceptionDiagnosticCase(false, invalidOpcode) &&
        CheckGuestExceptionDiagnosticCase(true, invalidOpcode) &&
        CheckGuestExceptionDiagnosticCase(true, privilegedHalt);
}

bool CheckRawStaticGprMapValidation() {
    std::array<std::uint64_t, 31> registers {};
    std::array<std::uint8_t, 16> mapping {};
    for (std::size_t index = 0; index != registers.size(); ++index)
        registers[index] = UINT64_C(0x1234567800000000) + index;
    for (std::size_t index = 0; index != mapping.size(); ++index)
        mapping[index] = static_cast<std::uint8_t>(30 - index);
    const auto snapshot = Lumen::MacOS::FexCore::CaptureRawStaticGprs(16, mapping, registers);
    bool ok = snapshot.valid;
    for (std::size_t index = 0; index != mapping.size(); ++index)
        ok &= snapshot.values[index] == registers[mapping[index]];
    const auto rejected = [&](std::uint16_t count, const auto& map) {
        const auto result = Lumen::MacOS::FexCore::CaptureRawStaticGprs(count, map, registers);
        return !result.valid && result.values == std::array<std::uint64_t, 16> {};
    };
    ok &= rejected(15, mapping) && rejected(17, mapping);
    mapping[15] = mapping[0];
    ok &= rejected(16, mapping);
    mapping[15] = 31;
    ok &= rejected(16, mapping);
    return ok;
}

bool CheckRawHostCodeValidation() {
    using namespace Lumen::MacOS::FexCore;
    std::uint64_t begin = 0x1000, end = 0x1084, hole = UINT64_MAX;
    std::size_t reads {};
    bool unsafeRead {};
    const auto owned = [&](std::uint64_t address) {
        return address >= begin && address < end && address != hole;
    };
    const auto readWord = [&](std::uint64_t address) {
        ++reads;
        for (std::size_t byte = 0; byte != 4; ++byte)
            unsafeRead |= !owned(address + byte);
        return UINT32_C(0xa5a50000) + static_cast<std::uint32_t>((address - begin) / 4);
    };
    const auto empty = [](const RawHostCodeSnapshot& snapshot) {
        return !snapshot.faultInstructionValid &&
            snapshot.instructions == decltype(snapshot.instructions) {} &&
            snapshot.available == decltype(snapshot.available) {};
    };
    auto snapshot = CaptureRawHostCode(0x1040, owned, readWord);
    bool ok = snapshot.faultInstructionValid && reads == 33 && !unsafeRead &&
        snapshot.instructions[RawHostCodeRadius] == UINT32_C(0xa5a50010) &&
        std::all_of(snapshot.available.begin(), snapshot.available.end(), [](bool value) { return value; });
    reads = 0;
    ok &= empty(CaptureRawHostCode(0x1084, owned, readWord)) && reads == 0;
    ok &= empty(CaptureRawHostCode(0x1042, owned, readWord)) && reads == 0;
    end = 0x1043;
    ok &= empty(CaptureRawHostCode(0x1040, owned, readWord)) && reads == 0;
    end = 0x1084; hole = 0x1046;
    snapshot = CaptureRawHostCode(0x1040, owned, readWord);
    ok &= snapshot.faultInstructionValid && reads == 32 && !unsafeRead &&
        !snapshot.available[RawHostCodeRadius + 1];
    reads = 0; hole = UINT64_MAX;
    snapshot = CaptureRawHostCode(0x1000, owned, readWord);
    ok &= snapshot.faultInstructionValid && reads == 17 && !unsafeRead &&
        !snapshot.available[RawHostCodeRadius - 1];
    reads = 0;
    snapshot = CaptureRawHostCode(UINT64_MAX - 3,
        [](std::uint64_t address) { return address >= UINT64_MAX - 3; },
        [&](std::uint64_t address) { ++reads; unsafeRead |= address != UINT64_MAX - 3; return UINT32_C(0x1234); });
    ok &= snapshot.faultInstructionValid && reads == 1 && !unsafeRead;
    reads = 0;
    snapshot = CaptureRawHostCode(0,
        [](std::uint64_t address) { return address < 4; },
        [&](std::uint64_t address) { ++reads; unsafeRead |= address != 0; return UINT32_C(0x5678); });
    ok &= snapshot.faultInstructionValid && reads == 1 && !unsafeRead;
    return Expect(ok, "bounded owned host-code snapshot, alignment, partial words and address limits");
}

bool CheckRawExtendedHostCodeValidation() {
    using namespace Lumen::MacOS::FexCore;
    std::uint64_t begin = 0x4000, end = 0x8000, hole = UINT64_MAX;
    std::size_t reads {}, ownershipQueries {};
    bool unsafeRead {};
    const auto owned = [&](std::uint64_t address) {
        ++ownershipQueries;
        return address >= begin && address < end && address != hole;
    };
    const auto readWord = [&](std::uint64_t address) {
        ++reads;
        unsafeRead |= address < 0x4000 || address > 0x7ffc || address % 4 != 0;
        for (std::size_t byte = 0; byte != 4; ++byte)
            unsafeRead |= !owned(address + byte);
        return UINT32_C(0xa5a50000) + static_cast<std::uint32_t>((address - begin) / 4);
    };
    const auto empty = [](const RawExtendedHostCodeSnapshot& s) {
        return !s.faultInstructionValid && s.pageFirst == 0 && s.pageLast == 0 &&
            s.before == 0 && s.after == 0 && s.ownerOmitted == 0 &&
            s.pageClampedBefore == 0 && s.pageClampedAfter == 0 &&
            s.instructions == decltype(s.instructions) {} && s.available == decltype(s.available) {};
    };
    auto s = CaptureRawExtendedHostCode(true, 0x6000, Page, owned, readWord);
    bool ok = s.faultInstructionValid && reads == 641 && !unsafeRead &&
        s.before == 512 && s.after == 128 && s.ownerOmitted == 0 &&
        s.pageFirst == 0x4000 && s.pageLast == 0x7fff &&
        s.pageClampedBefore == 0 && s.pageClampedAfter == 0 &&
        s.instructions[RawExtendedHostCodeBefore] == UINT32_C(0xa5a50800) &&
        std::all_of(s.available.begin(), s.available.end(), [](bool value) { return value; });
    reads = ownershipQueries = 0;
    ok &= empty(CaptureRawExtendedHostCode(false, 0x6000, Page, owned, readWord)) &&
        reads == 0 && ownershipQueries == 0;
    for (const std::uint64_t invalidPage : {UINT64_C(0), UINT64_C(3), UINT64_C(12)}) {
        reads = ownershipQueries = 0;
        ok &= empty(CaptureRawExtendedHostCode(true, 0x6000, invalidPage, owned, readWord)) &&
            reads == 0 && ownershipQueries == 0;
    }
    for (const std::uint64_t invalidPc : {UINT64_C(0x6002), UINT64_C(0x8000), UINT64_MAX}) {
        reads = 0;
        ok &= empty(CaptureRawExtendedHostCode(true, invalidPc, Page, owned, readWord)) && reads == 0;
    }
    hole = 0x6002; reads = 0;
    ok &= empty(CaptureRawExtendedHostCode(true, 0x6000, Page, owned, readWord)) && reads == 0;
    hole = 0x6006; reads = 0;
    s = CaptureRawExtendedHostCode(true, 0x6000, Page, owned, readWord);
    ok &= s.faultInstructionValid && reads == 640 && !unsafeRead && s.before == 512 &&
        s.after == 127 && s.ownerOmitted == 1 && !s.available[RawExtendedHostCodeBefore + 1] &&
        s.available[RawExtendedHostCodeBefore + 2];
    hole = UINT64_MAX; end = 0x6003; reads = 0;
    ok &= empty(CaptureRawExtendedHostCode(true, 0x6000, Page, owned, readWord)) && reads == 0;
    // Even an owner extending across pages cannot authorize foreign-page reads.
    begin = 0; end = UINT64_MAX; reads = 0;
    s = CaptureRawExtendedHostCode(true, 0x4000, Page, owned, readWord);
    ok &= s.faultInstructionValid && reads == 129 && !unsafeRead && s.before == 0 && s.after == 128 &&
        s.pageClampedBefore == 512 && s.pageClampedAfter == 0 && s.ownerOmitted == 0;
    reads = 0;
    s = CaptureRawExtendedHostCode(true, 0x7ffc, Page, owned, readWord);
    ok &= s.faultInstructionValid && reads == 513 && !unsafeRead && s.before == 512 && s.after == 0 &&
        s.pageClampedBefore == 0 && s.pageClampedAfter == 128 && s.ownerOmitted == 0;
    reads = 0;
    s = CaptureRawExtendedHostCode(true, UINT64_MAX - 3, Page,
        [](std::uint64_t address) { return address >= UINT64_MAX - 3; },
        [&](std::uint64_t address) { ++reads; unsafeRead |= address != UINT64_MAX - 3; return UINT32_C(0x1234); });
    ok &= s.faultInstructionValid && reads == 1 && !unsafeRead && s.pageLast == UINT64_MAX &&
        s.pageClampedAfter == 128 && s.ownerOmitted == 512;
    reads = 0;
    s = CaptureRawExtendedHostCode(true, 0, Page,
        [](std::uint64_t address) { return address < 4; },
        [&](std::uint64_t address) { ++reads; unsafeRead |= address != 0; return UINT32_C(0x5678); });
    ok &= s.faultInstructionValid && reads == 1 && !unsafeRead && s.pageFirst == 0 &&
        s.pageClampedBefore == 512 && s.ownerOmitted == 128;
    for (const char* value : std::array<const char*, 6> {nullptr, "", "0", "1x", "11", " 1"})
        ok &= !RawExtendedHostCodeEnabled(true, true, value);
    ok &= RawExtendedHostCodeEnabled(true, true, "1") &&
        !RawExtendedHostCodeEnabled(false, true, "1") && !RawExtendedHostCodeEnabled(true, false, "1");
    return Expect(ok, "extended host-code exact selector, same-page bounds, owned full words, holes and zero bad-PC reads");
}

bool CheckRawStaticGprFaultCase(bool contextEnabled, const char* selector, const char* extendedSelector = nullptr) {
    void* mapping = mmap(nullptr, Page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return false;
    // Original synthetic sequence: change two static GPRs, then fault on
    // a store through a null RAX. No guest calls or architectural state
    // reconstruction assumption is needed to assess these raw bank values.
    constexpr std::array<unsigned char, 10> code {
        0x49, 0x83, 0xc4, 0x10, // add r12, 16
        0x49, 0x83, 0xc5, 0x20, // add r13, 32
        0x89, 0x18,             // mov [rax], ebx
    };
    std::memcpy(mapping, code.data(), code.size());
    if (mprotect(mapping, Page, PROT_READ) != 0) {
        munmap(mapping, Page);
        return false;
    }
    Stack stack;
    if (!stack.base) { munmap(mapping, Page); return false; }
    std::array<std::uint64_t, 16> expected {};
    for (std::size_t index = 1; index != expected.size(); ++index)
        expected[index] = UINT64_C(0x1122334400000000) + index * UINT64_C(0x01010101);
    expected[4] = stack.base + Page - sizeof(std::uint64_t);
    int reportPipe[2];
    if (pipe(reportPipe) != 0) { munmap(mapping, Page); return false; }
    const pid_t child = fork();
    if (child < 0) {
        close(reportPipe[0]); close(reportPipe[1]);
        munmap(mapping, Page); return false;
    }
    if (child == 0) {
        close(reportPipe[0]);
        const int contextResult = contextEnabled
            ? setenv("LUMEN_MACOS_FEXCORE_FAULT_CONTEXT", "1", 1)
            : unsetenv("LUMEN_MACOS_FEXCORE_FAULT_CONTEXT");
        const int selectorResult = selector != nullptr
            ? setenv("LUMEN_MACOS_FEXCORE_FAULT_STATIC_GPRS", selector, 1)
            : unsetenv("LUMEN_MACOS_FEXCORE_FAULT_STATIC_GPRS");
        const int extendedResult = extendedSelector != nullptr
            ? setenv("LUMEN_MACOS_FEXCORE_FAULT_HOST_CODE_EXTENDED", extendedSelector, 1)
            : unsetenv("LUMEN_MACOS_FEXCORE_FAULT_HOST_CODE_EXTENDED");
        if (dup2(reportPipe[1], STDERR_FILENO) < 0 || close(reportPipe[1]) != 0 ||
            contextResult != 0 || selectorResult != 0 || extendedResult != 0) _exit(96);
        alarm(5);
        Fixture fixture {reinterpret_cast<std::uint64_t>(mapping)};
        LumenAndroidFexCoreGuestStateV1 guest {};
        guest.structSize = sizeof(guest); guest.abiVersion = 1; guest.rip = fixture.code;
        std::copy(expected.begin(), expected.end(), guest.gprs);
        const auto result = LumenAndroidFexCoreRunGuestV1(&guest, HostCall, Query, &fixture);
        _exit(result == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
              guest.faultSignal == SIGSEGV && guest.faultAddress == 0 ? 0 : 91);
    }
    close(reportPipe[1]);
    std::array<char, 256 * 1024> report {};
    std::size_t total {};
    while (total < report.size() - 1) {
        const auto received = read(reportPipe[0], report.data() + total, report.size() - 1 - total);
        if (received > 0) { total += static_cast<std::size_t>(received); continue; }
        if (received < 0 && errno == EINTR) continue;
        break;
    }
    close(reportPipe[0]);
    int status {};
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    const bool enabled = contextEnabled && selector != nullptr && std::strcmp(selector, "1") == 0;
    const char* rawRecord = report.data();
    while ((rawRecord = std::strstr(rawRecord, "[MACOS_FEXCORE_FAULT_STATIC_GPRS_V1]")) != nullptr &&
           !FaultContextRecordContains(rawRecord, "scope=guest")) ++rawRecord;
    bool valid = waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (enabled) {
        valid &= rawRecord != nullptr && FaultContextRecordContains(rawRecord, "rawOnly=1") &&
            FaultContextRecordContains(rawRecord, "staticMapValid=1 count=16 inJit=1");
        expected[12] += 16; expected[13] += 32;
        constexpr std::array<const char*, 16> names {
            "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
            "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
        };
        for (std::size_t index = 0; index != names.size(); ++index) {
            std::array<char, 48> token {};
            std::snprintf(token.data(), token.size(), "%s=%016llx", names[index],
                          static_cast<unsigned long long>(expected[index]));
            valid &= FaultContextRecordContains(rawRecord, token.data());
        }
        const auto* v4 = FindGuestFaultContextRecord(report.data());
        valid &= FaultContextRecordContains(v4, "capturedGprsValid=0 inJit=1") &&
            FaultContextRecordContains(v4, "activeHostCall=0");
        const char* host = report.data();
        while ((host = std::strstr(host, "[MACOS_FEXCORE_FAULT_HOST_CONTEXT_V1]")) != nullptr &&
               !FaultContextRecordContains(host, "scope=guest")) ++host;
        valid &= FaultContextRecordContains(host, "rawOnly=1 hostRegistersValid=1 count=31 inJit=1") &&
            FaultContextRecordContains(host, "faultInstructionValid=1");
        const auto parseHex = [&](const char* record, const char* name,
                                  unsigned long long& value) {
            const auto* field = record != nullptr ? std::strstr(record, name) : nullptr;
            return field != nullptr && FaultContextRecordContains(record, name) &&
                std::sscanf(field + std::strlen(name), "%llx", &value) == 1;
        };
        std::array<unsigned long long, 31> hostRegisters {};
        for (std::size_t index = 0; index != hostRegisters.size(); ++index) {
            std::array<char, 16> name {};
            std::snprintf(name.data(), name.size(), " x%zu=", index);
            valid &= parseHex(host, name.data(), hostRegisters[index]);
        }
        unsigned long long hostPc {}, hostInstruction {}, v4Pc {};
        valid &= parseHex(host, "hostPc=", hostPc) && parseHex(v4, "hostPc=", v4Pc) &&
            hostPc == v4Pc && hostPc % 4 == 0 &&
            parseHex(host, "faultInstruction=", hostInstruction);
        auto access = Lumen::MacOS::FexCore::DecodeMemoryAccess(
            static_cast<std::uint32_t>(hostInstruction));
        // The original null-store fixture may use STLR or ordinary STR W.
        // Decode the emitted base field, never a fixed register allocation.
        if (!access.valid && (hostInstruction & UINT32_C(0xffc00000)) == UINT32_C(0xb9000000)) {
            access = {true, static_cast<std::uint32_t>((hostInstruction >> 5) & 31),
                static_cast<std::int64_t>((hostInstruction >> 10) & 0xfff) * 4, 4, true};
        }
        valid &= access.valid && access.write && access.width == 4 && access.byteOffset == 0 &&
            access.baseRegister < hostRegisters.size() && hostRegisters[access.baseRegister] == 0;
        const char* codeRecord = report.data();
        std::size_t codeRecords {};
        bool centerFound {};
        while ((codeRecord = std::strstr(codeRecord, "[MACOS_FEXCORE_FAULT_HOST_CODE_V1]")) != nullptr) {
            if (FaultContextRecordContains(codeRecord, "scope=guest")) {
                ++codeRecords;
                const auto* offsetField = std::strstr(codeRecord, "offset=");
                int offset = 99;
                unsigned long long address {}, word {};
                valid &= offsetField != nullptr && std::sscanf(offsetField, "offset=%d", &offset) == 1 &&
                    offset >= -16 && offset <= 16 &&
                    parseHex(codeRecord, "address=", address) && parseHex(codeRecord, "instruction=", word);
                if (offset == 0) centerFound = address == hostPc && word == hostInstruction;
            }
            ++codeRecord;
        }
        valid &= centerFound && codeRecords >= 1 && codeRecords <= 33;
        const bool extendedEnabled = Lumen::MacOS::FexCore::RawExtendedHostCodeEnabled(
            contextEnabled, enabled, extendedSelector);
        if (extendedEnabled) {
            const char* summary = report.data();
            while ((summary = std::strstr(summary, "[MACOS_FEXCORE_FAULT_HOST_CODE_EXTENDED_V1]")) != nullptr &&
                   !FaultContextRecordContains(summary, "scope=guest")) ++summary;
            valid &= FaultContextRecordContains(summary, "faultInstructionValid=1 maximum_before=512 maximum_after=128") &&
                FaultContextRecordContains(summary, "native_page_size=16384");
            const auto parseCount = [&](const char* name, unsigned& value) {
                const char* field = summary != nullptr ? std::strstr(summary, name) : nullptr;
                return field != nullptr && FaultContextRecordContains(summary, name) &&
                    std::sscanf(field + std::strlen(name), "%u", &value) == 1;
            };
            unsigned captured {}, before {}, after {}, clampedBefore {}, clampedAfter {}, omitted {};
            unsigned long long first {}, last {};
            valid &= parseCount(" captured=", captured) && parseCount(" before=", before) &&
                parseCount(" after=", after) && parseCount(" page_clamped_before=", clampedBefore) &&
                parseCount(" page_clamped_after=", clampedAfter) && parseCount(" owner_omitted=", omitted) &&
                parseHex(summary, "page_first=", first) && parseHex(summary, "page_last=", last) &&
                first == (hostPc & ~(Page - 1)) && last == (first | (Page - 1)) &&
                captured == before + after + 1 && captured <= 641 && before <= 512 && after <= 128 &&
                captured + clampedBefore + clampedAfter + omitted == 641;
            const char* wordRecord = report.data();
            std::array<bool, 641> offsets {};
            std::size_t wordCount {}, beforeCount {}, afterCount {};
            bool extendedCenter {};
            while ((wordRecord = std::strstr(wordRecord, "[MACOS_FEXCORE_FAULT_HOST_CODE_EXTENDED_WORD_V1]")) != nullptr) {
                if (FaultContextRecordContains(wordRecord, "scope=guest")) {
                    ++wordCount;
                    const char* offsetField = std::strstr(wordRecord, "offset=");
                    int offset = 9999;
                    unsigned long long address {}, instruction {};
                    const bool wordValid = offsetField != nullptr && std::sscanf(offsetField, "offset=%d", &offset) == 1 &&
                        offset >= -512 && offset <= 128 && parseHex(wordRecord, "address=", address) &&
                        parseHex(wordRecord, "instruction=", instruction) && address >= first && address <= last - 3;
                    valid &= wordValid;
                    if (wordValid) {
                        const auto expectedAddress = offset < 0 ? hostPc - static_cast<unsigned>(-offset) * 4
                            : hostPc + static_cast<unsigned>(offset) * 4;
                        valid &= address == expectedAddress && !offsets[static_cast<std::size_t>(offset + 512)];
                        offsets[static_cast<std::size_t>(offset + 512)] = true;
                        beforeCount += offset < 0; afterCount += offset > 0;
                        if (offset == 0) extendedCenter = address == hostPc && instruction == hostInstruction;
                    }
                }
                ++wordRecord;
            }
            valid &= extendedCenter && wordCount == captured && beforeCount == before && afterCount == after;
        } else {
            valid &= std::strstr(report.data(), "[MACOS_FEXCORE_FAULT_HOST_CODE_EXTENDED_V1]") == nullptr &&
                std::strstr(report.data(), "[MACOS_FEXCORE_FAULT_HOST_CODE_EXTENDED_WORD_V1]") == nullptr;
        }
    } else {
        valid &= std::strstr(report.data(), "[MACOS_FEXCORE_FAULT_STATIC_GPRS_V1]") == nullptr &&
            std::strstr(report.data(), "[MACOS_FEXCORE_FAULT_HOST_CONTEXT_V1]") == nullptr &&
            std::strstr(report.data(), "[MACOS_FEXCORE_FAULT_HOST_CODE_V1]") == nullptr &&
            std::strstr(report.data(), "[MACOS_FEXCORE_FAULT_HOST_CODE_EXTENDED_V1]") == nullptr &&
            std::strstr(report.data(), "[MACOS_FEXCORE_FAULT_HOST_CODE_EXTENDED_WORD_V1]") == nullptr;
    }
    if (!valid) std::fprintf(stderr, "raw static GPR diagnostic: context=%u selector=%s status=0x%x report=%s\n",
        contextEnabled, selector != nullptr ? selector : "absent", status, report.data());
    return munmap(mapping, Page) == 0 && valid;
}

bool CheckRawStaticGprDiagnostic() {
    const bool ok = CheckRawStaticGprMapValidation() && CheckRawHostCodeValidation() && CheckRawExtendedHostCodeValidation() &&
        CheckRawStaticGprFaultCase(true, nullptr) &&
        CheckRawStaticGprFaultCase(true, "0") &&
        CheckRawStaticGprFaultCase(true, "1x") &&
        CheckRawStaticGprFaultCase(false, "1") &&
        CheckRawStaticGprFaultCase(true, "1") &&
        CheckRawStaticGprFaultCase(true, "1", "0") && CheckRawStaticGprFaultCase(true, "1", "1x") &&
        CheckRawStaticGprFaultCase(false, "1", "1") && CheckRawStaticGprFaultCase(true, "0", "1") &&
        CheckRawStaticGprFaultCase(true, "1", "1");
    std::printf("LUMEN_MACOS_FEXCORE_FAULT_STATIC_GPRS_TEST_V1 pass=%u faultCases=10 mapNegatives=4 hostCodeCases=8 rawOnly=1\n", ok);
    std::printf("LUMEN_MACOS_FEXCORE_FAULT_HOST_CODE_EXTENDED_TEST_V1 pass=%u maximumBefore=512 maximumAfter=128"
                " samePage/ownerHoles/limits=checked selector/dependencies=checked badPcReads=0 default33=preserved rawOnly=1\n", ok);
    return ok;
}

bool CheckDeniedTransitionDiagnostic() {
    void* reservation = mmap(nullptr, Page * 3, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reservation == MAP_FAILED) return false;
    const auto code = reinterpret_cast<std::uint64_t>(reservation) + Page;
    const auto deniedTarget = reinterpret_cast<std::uint64_t>(reservation) + 64;
    if (mprotect(reinterpret_cast<void*>(code), Page,
                 PROT_READ | PROT_WRITE) != 0) {
        munmap(reservation, Page * 3);
        return false;
    }
    // The source compilation unit has a distinct entry and transfer
    // instruction. The diagnostic must report the unit root, without
    // claiming an exact predecessor or branch source that FEX does not expose.
    std::array<unsigned char, 14> indirectCall {
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,
        0x90, 0xff, 0xd0, 0xc3,
    };
    std::memcpy(indirectCall.data() + 2, &deniedTarget,
                sizeof(deniedTarget));
    std::memcpy(reinterpret_cast<void*>(code), indirectCall.data(),
                indirectCall.size());
    if (mprotect(reinterpret_cast<void*>(code), Page, PROT_READ) != 0) {
        munmap(reservation, Page * 3);
        return false;
    }
    int reportPipe[2];
    if (pipe(reportPipe) != 0) {
        munmap(reservation, Page * 3);
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        close(reportPipe[0]); close(reportPipe[1]);
        munmap(reservation, Page * 3);
        return false;
    }
    if (child == 0) {
        close(reportPipe[0]);
        if (dup2(reportPipe[1], STDERR_FILENO) < 0 ||
            close(reportPipe[1]) != 0 ||
            setenv("LUMEN_MACOS_FEXCORE_FAULT_CONTEXT", "1", 1) != 0) {
            _exit(96);
        }
        Stack stack;
        if (!stack.base) _exit(92);
        Fixture fixture {code};
        LumenAndroidFexCoreGuestStateV1 guest {};
        guest.structSize = sizeof(guest);
        guest.abiVersion = 1;
        guest.rip = code;
        guest.gprs[4] = stack.base + Page - sizeof(std::uint64_t);
        const auto root = LumenAndroidFexCoreRootReturnAddressV1();
        std::memcpy(reinterpret_cast<void*>(guest.gprs[4]), &root,
                    sizeof(root));
        const auto result = LumenAndroidFexCoreRunGuestV1(
            &guest, HostCall, Query, &fixture);
        _exit(result ==
            LUMEN_ANDROID_FEXCORE_RUNTIME_EXECUTABLE_RANGE_DENIED_V1 ? 0 : 91);
    }

    close(reportPipe[1]);
    std::array<char, 8192> report {};
    std::size_t total {};
    while (total < report.size() - 1) {
        const auto received = read(reportPipe[0], report.data() + total,
                                   report.size() - 1 - total);
        if (received > 0) {
            total += static_cast<std::size_t>(received);
            continue;
        }
        if (received < 0 && errno == EINTR) continue;
        break;
    }
    close(reportPipe[0]);
    int status {};
    pid_t waited;
    do { waited = waitpid(child, &status, 0); }
    while (waited < 0 && errno == EINTR);

    const char* record = FindGuestFaultContextRecord(report.data());
    std::array<char, 64> expectedSource {};
    std::array<char, 64> expectedTarget {};
    std::array<char, 48> expectedSignal {};
    std::snprintf(expectedSource.data(), expectedSource.size(),
        "sourceCompilationEntry=0x%016llx",
        static_cast<unsigned long long>(code));
    std::snprintf(expectedTarget.data(), expectedTarget.size(),
        "transitionTarget=0x%016llx",
        static_cast<unsigned long long>(deniedTarget));
    std::snprintf(expectedSignal.data(), expectedSignal.size(),
        "syncFaultSignal=%d", SIGSEGV);
    const bool valid = waited == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0 && record != nullptr &&
        FaultContextRecordContains(record, "guestSignalRoute=segv") &&
        FaultContextRecordContains(record, "syncFaultGenerated=1") &&
        FaultContextRecordContains(record, "syncFaultMetadataValid=1") &&
        FaultContextRecordContains(record, expectedSignal.data()) &&
        FaultContextRecordContains(record, "syncFaultTrapNo=14") &&
        FaultContextRecordContains(record, "syncFaultSiCode=2") &&
        FaultContextRecordContains(record, "syncFaultErrorCode=0x0015") &&
        FaultContextRecordContains(record, "guestCsIndex=0x0030") &&
        FaultContextRecordContains(record, "guestIoplBit12=0") &&
        FaultContextRecordContains(record,
            "sourceCompilationEntryValid=1") &&
        FaultContextRecordContains(record, expectedSource.data()) &&
        FaultContextRecordContains(record, expectedTarget.data());
    if (!valid) {
        std::fprintf(stderr,
            "denied transition diagnostic: status=0x%x bytes=%zu report=%s\n",
            status, total, report.data());
    }
    return munmap(reservation, Page * 3) == 0 && valid;
}

std::uint32_t HostCall(void* opaque, std::uint64_t target, std::uint64_t* gprs,
                       std::uint64_t* nextRip) noexcept {
    auto& fixture = *static_cast<Fixture*>(opaque);
    if (target != reinterpret_cast<std::uint64_t>(&HostTarget) ||
        FEXCore::Allocator::JITWriteScopeDepth != 0 || !fixture.stack->Contains(gprs[4], 8))
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    ++fixture.calls;
    if (fixture.hostCallGate != nullptr) {
        if (fixture.hostCallEntered != nullptr)
            fixture.hostCallEntered->store(true, std::memory_order_release);
        while (!fixture.hostCallGate->load(std::memory_order_acquire))
            std::this_thread::yield();
    }
    if (fixture.countCache) {
        const auto cache = LumenMacOSFexCoreProbeActiveCodeCacheV1();
        fixture.ok &= cache != 0;
        if (fixture.previousCache != 0 && cache != fixture.previousCache)
            ++fixture.cacheChanges;
        fixture.previousCache = cache;
    } else if (fixture.depth == 0) {
        Fixture nested {fixture.code, nullptr, 1};
        fixture.ok &= Run(nested, fixture.code + 32, 73) && nested.calls == 0;
    }
    std::memcpy(nextRip, reinterpret_cast<void*>(gprs[4]), sizeof(*nextRip));
    gprs[4] += 8;
    gprs[0] = 42;
    if (fixture.armBoundaryAfterReturn &&
        (fixture.armBoundaryEveryReturn || fixture.calls == fixture.boundaryArmCall)) {
        fixture.boundaryToken = LumenAndroidFexCoreRequestGuestBoundaryInterruptV1(
            reinterpret_cast<std::uint64_t>(pthread_self()));
        fixture.ok &= fixture.boundaryToken != 0;
    }
    return fixture.ok ? LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1 :
        LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
}

bool ReleaseGuestCode(void* mapping, std::size_t size) {
    const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    if (!token) return false;
    const bool released = munmap(mapping, size) == 0;
    LumenAndroidFexCoreEndGuestMemoryMutationV1(token,
        reinterpret_cast<std::uint64_t>(mapping), size, released);
    return released;
}

bool CheckConcurrent() {
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    if (LumenAndroidFexCoreRuntimeProbeV1(&status) != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1)
        return false;
    const auto target = reinterpret_cast<std::uint64_t>(&HostTarget);
    if (!LumenAndroidFexCoreRegisterHostCallTargetV1(target)) return false;
    void* mapping = mmap(nullptr, Page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return false;
    // Original synthetic x86-64 corpus: a native callback and a nested leaf.
    std::array<unsigned char, 13> outer {0x48, 0xb8, 0,0,0,0,0,0,0,0, 0xff,0xd0,0xc3};
    std::memcpy(outer.data() + 2, &target, sizeof(target));
    std::memcpy(mapping, outer.data(), outer.size());
    const unsigned char leaf[] {0xb8,73,0,0,0,0xc3};
    std::memcpy(static_cast<char*>(mapping) + 32, leaf, sizeof(leaf));
    if (mprotect(mapping, Page, PROT_READ) != 0) { munmap(mapping, Page); return false; }
    std::atomic<unsigned> ready {}, completed {};
    std::atomic<bool> start {}, ok {true};
    std::array<std::thread, Workers> threads;
    for (auto& thread : threads) {
        thread = std::thread([&] {
            ++ready;
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (std::size_t iteration = 0; iteration < Iterations; ++iteration) {
                Fixture fixture {reinterpret_cast<std::uint64_t>(mapping)};
                if (!Run(fixture, fixture.code, 42) || fixture.calls != 1) ok = false;
                ++completed;
            }
        });
    }
    while (ready.load() != Workers) std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    const bool result = ok && completed == Workers * Iterations;
    return ReleaseGuestCode(mapping, Page) && result;
}

struct MutationCloseFixture {
    std::uint64_t token {};
    std::uint32_t kind {};
    std::thread::id owner {std::this_thread::get_id()};
    unsigned calls {};
    unsigned reentrantCalls {};
    bool ownerMatched {true};
    bool reentryDenied {true};
};

void CountReentrantClose(void* opaque) noexcept {
    ++static_cast<MutationCloseFixture*>(opaque)->reentrantCalls;
}

void ReleaseMutationLocalGate(void* opaque) noexcept {
    auto& fixture = *static_cast<MutationCloseFixture*>(opaque);
    ++fixture.calls;
    fixture.ownerMatched &= std::this_thread::get_id() == fixture.owner;
    fixture.reentryDenied &= LumenAndroidFexCoreEndGuestMemoryMutationV2(
        fixture.token, 0, 0, 0, CountReentrantClose, &fixture) == 0;
    // BeginMutation also remains closed to the publishing owner, so the
    // callback cannot accidentally start another publication mid-close.
    fixture.reentryDenied &= LumenAndroidFexCoreBeginGuestMemoryMutationV2(
        fixture.kind) == 0;
}

bool CheckMutationCloseCallbacks() {
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    if (LumenAndroidFexCoreRuntimeProbeV1(&status) !=
        LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) return false;
    for (const auto kind : {
             LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2,
             LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_ADDITIVE_MAPPING_V2}) {
        for (const auto committed : {0U, 1U}) {
            for (const auto size : {std::uint64_t {0}, std::uint64_t {Page}}) {
                MutationCloseFixture fixture;
                fixture.kind = kind;
                fixture.token = LumenAndroidFexCoreBeginGuestMemoryMutationV2(kind);
                if (fixture.token == 0) return false;
                bool ok = LumenAndroidFexCoreEndGuestMemoryMutationV2(
                    0, 0, size, committed, ReleaseMutationLocalGate, &fixture) == 0;
                const auto wrongToken = fixture.token ^ (UINT64_C(1) << 40U);
                ok &= LumenAndroidFexCoreEndGuestMemoryMutationV2(
                    wrongToken, 0, size, committed,
                    ReleaseMutationLocalGate, &fixture) == 0;
                std::uint32_t crossThreadResult = 1;
                std::thread wrongOwner([&] {
                    crossThreadResult = LumenAndroidFexCoreEndGuestMemoryMutationV2(
                        fixture.token, 0, size, committed,
                        ReleaseMutationLocalGate, &fixture);
                });
                wrongOwner.join();
                ok &= crossThreadResult == 0 && fixture.calls == 0;
                ok &= LumenAndroidFexCoreEndGuestMemoryMutationV2(
                    fixture.token, 0, size, committed,
                    ReleaseMutationLocalGate, &fixture) == 1;
                ok &= fixture.calls == 1 && fixture.ownerMatched &&
                    fixture.reentryDenied && fixture.reentrantCalls == 0;
                ok &= LumenAndroidFexCoreEndGuestMemoryMutationV2(
                    fixture.token, 0, size, committed,
                    ReleaseMutationLocalGate, &fixture) == 0 && fixture.calls == 1;
                if (!ok) return false;
            }
        }
    }
    // Null callbacks are supported, and the stable V1 path still closes a
    // mutation sufficiently for another one to begin on the same thread.
    auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    if (token == 0) return false;
    if (LumenAndroidFexCoreEndGuestMemoryMutationV2(
            token, 0, 0, 0, nullptr, nullptr) != 1) return false;
    token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    if (token == 0) return false;
    LumenAndroidFexCoreEndGuestMemoryMutationV1(token, 0, 0, 0);
    token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    if (token == 0) return false;
    return LumenAndroidFexCoreEndGuestMemoryMutationV2(
        token, 0, 0, 0, nullptr, nullptr) == 1;
}

bool CheckBranchInvalidation() {
    void* mapping = mmap(nullptr, Page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return false;
    // Both conditional branch destinations are warmed, then an executable
    // range mutation changes the two results at the same guest addresses.
    const unsigned char branch[] {
        0x85,0xff,             // test edi,edi
        0x74,0x06,             // jz second
        0xb8,73,0,0,0,0xc3,   // mov eax,73; ret
        0xb8,42,0,0,0,0xc3,   // mov eax,42; ret
    };
    std::memcpy(mapping, branch, sizeof(branch));
    bool ok = mprotect(mapping, Page, PROT_READ) == 0;
    for (unsigned generation = 0; ok && generation < 8; ++generation) {
        for (unsigned repeat = 0; ok && repeat < 4; ++repeat) {
            Fixture first {reinterpret_cast<std::uint64_t>(mapping)};
            Fixture second {reinterpret_cast<std::uint64_t>(mapping)};
            ok &= Run(first, first.code, 42 + generation, 0);
            ok &= Run(second, second.code, 73 + generation, 1);
        }
        if (generation == 7 || !ok) break;
        const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (!token) { ok = false; break; }
        const bool writable = mprotect(mapping, Page, PROT_READ | PROT_WRITE) == 0;
        if (writable) {
            static_cast<unsigned char*>(mapping)[5] = 74 + generation;
            static_cast<unsigned char*>(mapping)[11] = 43 + generation;
        }
        const bool published = writable && mprotect(mapping, Page, PROT_READ) == 0;
        MutationCloseFixture close;
        close.token = token;
        close.kind = LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2;
        ok &= LumenAndroidFexCoreEndGuestMemoryMutationV2(token,
            reinterpret_cast<std::uint64_t>(mapping), Page, writable,
            ReleaseMutationLocalGate, &close) == 1;
        ok &= published && close.calls == 1 && close.ownerMatched &&
            close.reentryDenied && close.reentrantCalls == 0;
    }
    return ReleaseGuestCode(mapping, Page) && ok;
}

bool CheckCacheExhaustion(unsigned* changes, unsigned* calls) {
    constexpr std::size_t size = CacheBlocks * BlockBytes;
    void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return false;
    const auto target = reinterpret_cast<std::uint64_t>(&HostTarget);
    std::memset(mapping, 0x90, size);
    // Original x86-64 corpus. Every block has its own indirect destination,
    // forcing lazy compilation of new guest entries. Host callbacks sample
    // the active cache identity throughout the same continuous guest run.
    // No ClearCodeCache hook is invoked: an observed generation change must
    // follow normal compilation pressure.
    for (std::size_t index = 0; index < CacheBlocks; ++index) {
        auto* cursor = static_cast<unsigned char*>(mapping) + index * BlockBytes;
        if (index % BlocksPerCallback == 0 || index == CacheBlocks - 1) {
            *cursor++ = 0x48; *cursor++ = 0xb8;
            std::memcpy(cursor, &target, sizeof(target)); cursor += sizeof(target);
            *cursor++ = 0xff; *cursor++ = 0xd0;
        }
        *cursor++ = 0x41; *cursor++ = 0x83; *cursor++ = 0xc4; *cursor++ = 1; // add r12d,1
        if (index == CacheBlocks - 1) {
            *cursor++ = 0x4c; *cursor++ = 0x89; *cursor++ = 0xe0; // mov rax,r12
            *cursor++ = 0xc3;
        } else {
            *cursor++ = 0x49; *cursor++ = 0xbb; // movabs r11,next
            const auto next = reinterpret_cast<std::uint64_t>(mapping) + (index + 1) * BlockBytes;
            std::memcpy(cursor, &next, sizeof(next)); cursor += sizeof(next);
            *cursor++ = 0x41; *cursor++ = 0xff; *cursor++ = 0xe3; // jmp r11
        }
    }
    bool ok = mprotect(mapping, size, PROT_READ) == 0;
    Fixture fixture {reinterpret_cast<std::uint64_t>(mapping)};
    fixture.size = size;
    fixture.countCache = true;
    if (ok) ok = Run(fixture, fixture.code, CacheBlocks);
    *changes = fixture.cacheChanges;
    *calls = fixture.calls;
    ok &= Expect(fixture.calls == CacheBlocks / BlocksPerCallback + 1,
                 "all cache sampling host calls must return");
    ok &= Expect(fixture.cacheChanges >= 2, "at least two natural code cache changes required");
    ok &= Expect(LumenMacOSFexCoreProbeActiveCodeCacheV1() == 0,
                 "cache probe must expire outside an active guest run");
    return ReleaseGuestCode(mapping, size) && ok;
}

struct NullIndirectEvidence {
    std::uint32_t result {UINT32_MAX};
    std::uint32_t guestSignal {};
    std::uint64_t guestRip {};
    int hostSignal {};
    std::uint64_t hostPc {};
    std::uint64_t hostLr {};
    std::uint64_t hostX0 {};
    std::uint64_t hostX1 {};
};
int nullIndirectEvidenceFd {-1};

void CaptureNullIndirectFault(int signal, siginfo_t*, void* rawContext) {
    const auto* context = static_cast<const ucontext_t*>(rawContext);
    NullIndirectEvidence evidence;
    evidence.hostSignal = signal;
    if (context != nullptr) {
        evidence.hostPc = context->uc_mcontext->__ss.__pc;
        evidence.hostLr = context->uc_mcontext->__ss.__lr;
        evidence.hostX0 = context->uc_mcontext->__ss.__x[0];
        evidence.hostX1 = context->uc_mcontext->__ss.__x[1];
    }
    const auto count = write(nullIndirectEvidenceFd, &evidence, sizeof(evidence));
    _exit(count == sizeof(evidence) ? 3 : 97);
}

// Manual diagnostic mode only: it is not part of the ordinary JIT gate while
// the zero-target behavior is still under investigation. Each child starts a
// fresh runtime and must return an executable-range denial, not a host fault.
bool CheckNullIndirectTransfer(bool call) {
    int evidencePipe[2];
    if (pipe(evidencePipe) != 0) return false;
    const auto child = fork();
    if (child < 0) {
        close(evidencePipe[0]); close(evidencePipe[1]);
        return false;
    }
    if (child == 0) {
        close(evidencePipe[0]);
        nullIndirectEvidenceFd = evidencePipe[1];
        const rlimit noCore {0, 0};
        setrlimit(RLIMIT_CORE, &noCore);
        struct sigaction action {};
        action.sa_sigaction = CaptureNullIndirectFault;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGSEGV, &action, nullptr) != 0 ||
            sigaction(SIGBUS, &action, nullptr) != 0) _exit(96);
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGSEGV);
        sigaddset(&signals, SIGBUS);
        sigaddset(&signals, SIGALRM);
        if (pthread_sigmask(SIG_UNBLOCK, &signals, nullptr) != 0) _exit(98);
        signal(SIGALRM, SIG_DFL);
        alarm(15);
        void* mapping = mmap(nullptr, Page, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) _exit(95);
        // Original x86 corpus: CALL RDI / JMP RDI; UD2 if it unexpectedly
        // returns. RDI is supplied as zero in the initial runtime state.
        const unsigned char code[] {0xff, static_cast<unsigned char>(call ? 0xd7 : 0xe7), 0x0f, 0x0b};
        std::memcpy(mapping, code, sizeof(code));
        if (mprotect(mapping, Page, PROT_READ) != 0) _exit(94);
        Stack stack;
        if (!stack.base) _exit(93);
        Fixture fixture {reinterpret_cast<std::uint64_t>(mapping), &stack};
        LumenAndroidFexCoreGuestStateV1 guest {};
        guest.structSize = sizeof(guest);
        guest.abiVersion = 1;
        guest.rip = fixture.code;
        guest.gprs[4] = stack.base + Page - sizeof(std::uint64_t);
        const auto root = LumenAndroidFexCoreRootReturnAddressV1();
        std::memcpy(reinterpret_cast<void*>(guest.gprs[4]), &root, sizeof(root));
        NullIndirectEvidence evidence;
        evidence.result = LumenAndroidFexCoreRunGuestV1(&guest, HostCall, Query, &fixture);
        evidence.guestSignal = guest.faultSignal;
        evidence.guestRip = guest.rip;
        const auto count = write(evidencePipe[1], &evidence, sizeof(evidence));
        _exit(count == sizeof(evidence) ? 0 : 97);
    }
    close(evidencePipe[1]);
    int status {};
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    NullIndirectEvidence evidence;
    ssize_t count;
    do { count = read(evidencePipe[0], &evidence, sizeof(evidence)); } while (count < 0 && errno == EINTR);
    close(evidencePipe[0]);
    const bool passed = waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        count == sizeof(evidence) && evidence.hostSignal == 0 &&
        evidence.result == LUMEN_ANDROID_FEXCORE_RUNTIME_EXECUTABLE_RANGE_DENIED_V1 &&
        evidence.guestRip == 0 && evidence.guestSignal == SIGSEGV;
    std::printf("LUMEN_MACOS_FEXCORE_NULL_INDIRECT_DIAGNOSTIC_V1 operation=%s pass=%u "
        "status=0x%x evidenceBytes=%zd result=%u guestSignal=%u guestRip=0x%llx "
        "hostSignal=%d hostPc=0x%llx hostLr=0x%llx hostX0=0x%llx hostX1=0x%llx\n",
        call ? "call" : "jump", passed, status, count, evidence.result, evidence.guestSignal,
        static_cast<unsigned long long>(evidence.guestRip), evidence.hostSignal,
        static_cast<unsigned long long>(evidence.hostPc), static_cast<unsigned long long>(evidence.hostLr),
        static_cast<unsigned long long>(evidence.hostX0), static_cast<unsigned long long>(evidence.hostX1));
    return passed;
}

// --- Generated-code boundary interrupt ---------------------------------------
//
// The exception delivery reuses the mutation doorbell: the requester arms the
// interrupt page of a peer's active run and the peer delivers at its next
// block boundary. The synthetic corpus covers both shapes: a tight loop whose
// only backedge is a taken conditional branch (unreachable until the branch
// itself carries the interrupt store) and a request cancelled while the
// target is inside a host call (page restored, no delivery, run completes).

std::atomic<unsigned> g_boundaryDeliveries {0};
std::atomic<std::uint64_t> g_boundaryRip {0};
std::atomic<std::uint64_t> g_boundaryRsp {0};
std::atomic<std::uint64_t> g_boundaryCounter {0};
std::atomic<bool> g_boundaryStateInCode {false};
std::atomic<bool> g_boundaryStateOnStack {false};
std::atomic<unsigned> g_boundaryNested {0};
std::atomic<std::uint64_t> g_boundaryCodeBase {0};
std::atomic<std::uint64_t> g_boundaryCodeSize {0};
std::atomic<std::uint64_t> g_boundaryStackBase {0};
std::atomic<std::uint64_t> g_boundaryStackSize {0};
std::atomic<std::uint64_t> g_boundaryNestedCode {0};
std::atomic<std::uint64_t> g_boundaryNestedHostCode {0};
std::atomic<unsigned> g_boundaryNestedHostCalls {0};
std::atomic<bool> g_boundaryRearm {false};
std::atomic<std::uint64_t> g_boundaryRearmedToken {0};
std::atomic<bool> g_boundaryGprsPreserved {true};
std::atomic<bool> g_boundaryHostFpPreserved {true};

struct HostReturnBoundaryEvidence {
    unsigned deliveries {};
    std::uint64_t rip {};
    std::uint64_t rsp {};
    bool stop {};
};
thread_local HostReturnBoundaryEvidence* g_hostReturnBoundaryEvidence {};

bool CheckBoundaryClosedStop() {
    using Lumen::MacOS::FexCore::BoundaryPauseReturnDisposition;
    using Lumen::MacOS::FexCore::ClassifyBoundaryPauseReturn;
    constexpr std::uint64_t pausePc = 0x1000;
    bool ok = true;
    unsigned cases = 0;
    // Reject every missing proof, including when an error has already been
    // reported. A failed flag alone never admits a native signal as a stop.
    for (const bool illegalSignal : {false, true})
        for (const bool inFlight : {false, true})
            for (const bool sameThread : {false, true})
                for (const auto hostPc : {std::uint64_t {0}, pausePc, pausePc + 4})
                    for (const bool failed : {false, true}) {
                        const auto expected = illegalSignal && inFlight && sameThread && hostPc == pausePc
                            ? (failed ? BoundaryPauseReturnDisposition::Stop : BoundaryPauseReturnDisposition::Resume)
                            : BoundaryPauseReturnDisposition::Reject;
                        ok &= ClassifyBoundaryPauseReturn(illegalSignal, inFlight, sameThread,
                            hostPc, pausePc, failed) == expected;
                        ++cases;
                    }
    ok &= ClassifyBoundaryPauseReturn(true, true, true, 0, 0, true) ==
        BoundaryPauseReturnDisposition::Reject;
    std::printf("LUMEN_MACOS_FEXCORE_BOUNDARY_CLOSED_STOP_TEST_V1 pass=%u cases=%u"
                " failed-exact=stop healthy-exact=resume other-signal-pc-thread=reject zero-pause=reject\n", ok, cases + 1);
    return Expect(ok, "boundary closed stop admission policy");
}

std::uint32_t BoundaryHostCallV2(void* opaque, std::uint64_t target,
    std::uint64_t* gprs, LumenAndroidFexCoreSimdViewV2*,
    std::uint64_t* nextRip) noexcept {
    return HostCall(opaque, target, gprs, nextRip);
}

std::uint32_t CaptureBoundaryDelivery(const std::uint64_t* gprs,
                                      std::uint64_t rip) noexcept {
    if (gprs == nullptr) return 0;
    if (g_hostReturnBoundaryEvidence != nullptr) {
        ++g_hostReturnBoundaryEvidence->deliveries;
        g_hostReturnBoundaryEvidence->rip = rip;
        g_hostReturnBoundaryEvidence->rsp = gprs[4];
        return g_hostReturnBoundaryEvidence->stop
            ? LUMEN_ANDROID_FEXCORE_BOUNDARY_STOP_V1
            : LUMEN_ANDROID_FEXCORE_BOUNDARY_IDLE_V1;
    }
    std::array<std::uint64_t, 16> interruptedGprs;
    std::memcpy(interruptedGprs.data(), gprs, sizeof(interruptedGprs));
    g_boundaryDeliveries.fetch_add(1, std::memory_order_relaxed);
    g_boundaryRip.store(rip, std::memory_order_relaxed);
    g_boundaryRsp.store(gprs[4], std::memory_order_relaxed);
    g_boundaryCounter.store(gprs[1], std::memory_order_relaxed);
    const auto codeBase = g_boundaryCodeBase.load(std::memory_order_relaxed);
    const auto codeSize = g_boundaryCodeSize.load(std::memory_order_relaxed);
    const auto stackBase = g_boundaryStackBase.load(std::memory_order_relaxed);
    const auto stackSize = g_boundaryStackSize.load(std::memory_order_relaxed);
    g_boundaryStateInCode.store(
        rip >= codeBase && rip - codeBase < codeSize,
        std::memory_order_relaxed);
    g_boundaryStateOnStack.store(
        gprs[4] >= stackBase && gprs[4] - stackBase <= stackSize,
        std::memory_order_relaxed);
    // V3 root entry has native downward rounding and guest nearest rounding.
    // Delivery must see the original host environment and may change it;
    // the outer guest and the caller must retain their own environments.
    if (std::fegetround() != FE_DOWNWARD || std::fesetround(FE_UPWARD) != 0)
        g_boundaryHostFpPreserved.store(false);
    const bool rearm = g_boundaryRearm.exchange(false);
    if (rearm) {
        g_boundaryRearmedToken.store(LumenAndroidFexCoreRequestGuestBoundaryInterruptV1(
            reinterpret_cast<std::uint64_t>(pthread_self())));
    }
    // A real handler is an ordinary nested guest call; run one here too.
    const std::uint64_t nestedCode =
        g_boundaryNestedCode.load(std::memory_order_relaxed);
    if (nestedCode != 0) {
        Fixture nested {nestedCode, nullptr};
        nested.size = 6;
        if (Run(nested, nestedCode, 7))
            g_boundaryNested.store(7, std::memory_order_relaxed);
    }
    const std::uint64_t nestedHostCode = g_boundaryNestedHostCode.load(std::memory_order_relaxed);
    if (nestedHostCode != 0) {
        Fixture nested {nestedHostCode, nullptr, 1};
        nested.size = 13;
        if (Run(nested, nestedHostCode, 42) && nested.calls == 1)
            g_boundaryNestedHostCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (std::memcmp(interruptedGprs.data(), gprs, sizeof(interruptedGprs)) != 0)
        g_boundaryGprsPreserved.store(false);
    // The second token also covers the valid no-handler-result contract.
    return rearm ? 1U : 0U;
}

bool CheckHostReturnBoundaryCase(bool stop) {
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    if (LumenAndroidFexCoreRuntimeProbeV1(&status) != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 ||
        !LumenAndroidFexCoreRegisterBoundaryDeliveryV1(&CaptureBoundaryDelivery) ||
        !LumenAndroidFexCoreRegisterHostCallTargetV1(reinterpret_cast<std::uint64_t>(&HostTarget))) return false;
    void* mapping = mmap(nullptr, Page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return false;
    auto* code = static_cast<unsigned char*>(mapping);
    const unsigned char leaf[] {
        0x48, 0x85, 0xff,                         // test rdi,rdi
        0x74, 0x0d,                               // jz done
        0x48, 0xff, 0x07,                         // inc qword [rdi]
        0x48, 0x83, 0x3f, 0x02,                   // cmp qword [rdi],2
        0x0f, 0x82, 0x2e, 0x00, 0x00, 0x00,       // jb caller at +64
        0xb8, 73, 0, 0, 0, 0xc3,                  // done: mov eax,73; ret
    };
    std::memcpy(code, leaf, sizeof(leaf));
    // Warm the leaf first. The later JMP can then link backward into a
    // different compilation unit immediately after an entry doorbell.
    // The armed run loops twice through the same host call: the first return
    // fills this thread's L1 entry, so the second return keeps State.rip at
    // the completed SYSCALL instead of a dispatcher miss updating it.
    std::array<unsigned char, 17> caller {
        0x48, 0xb8, 0,0,0,0,0,0,0,0, // movabs rax,HostTarget
        0xff, 0xd0,                   // call rax
        0xe9, 0,0,0,0,               // jmp leaf
    };
    const auto target = reinterpret_cast<std::uint64_t>(&HostTarget);
    const std::int32_t displacement = -81; // caller at +64, next RIP at +81
    std::memcpy(caller.data() + 2, &target, sizeof(target));
    std::memcpy(caller.data() + 13, &displacement, sizeof(displacement));
    std::memcpy(code + 64, caller.data(), caller.size());
    if (mprotect(mapping, Page, PROT_READ) != 0) {
        munmap(mapping, Page);
        return false;
    }
    const auto base = reinterpret_cast<std::uint64_t>(mapping);
    Fixture fixture {base, nullptr, 1};
    bool ok = Run(fixture, base, 73);
    ok = Run(fixture, base + 64, 73) && fixture.calls == 1 && ok;
    fixture.calls = 0;
    fixture.armBoundaryAfterReturn = true;
    fixture.boundaryArmCall = stop ? 1 : 2;
    Stack stack;
    if (stack.base == 0) {
        ReleaseGuestCode(mapping, Page);
        return false;
    }
    fixture.stack = &stack;
    LumenAndroidFexCoreGuestStateV1 guest {};
    guest.structSize = sizeof(guest); guest.abiVersion = 1; guest.rip = base + 64;
    const auto initialRsp = stack.base + Page - 32;
    const auto counterAddress = stack.base + Page - 64;
    guest.gprs[4] = initialRsp;
    guest.gprs[7] = counterAddress;
    const std::uint64_t initialCounter = 0;
    std::memcpy(reinterpret_cast<void*>(guest.gprs[7]), &initialCounter, sizeof(initialCounter));
    const std::array<std::uint64_t, 3> frame {
        LumenAndroidFexCoreRootReturnAddressV1(), UINT64_C(0xd1c2b3a4978655e6), UINT64_C(0xfedcba9876543210)};
    std::memcpy(reinterpret_cast<void*>(initialRsp), frame.data(), sizeof(frame));
    HostReturnBoundaryEvidence evidence;
    evidence.stop = stop;
    g_hostReturnBoundaryEvidence = &evidence;
    const auto result = LumenAndroidFexCoreRunGuestV1(&guest, HostCall, Query, &fixture);
    g_hostReturnBoundaryEvidence = nullptr;
    const bool canaries = std::memcmp(reinterpret_cast<void*>(initialRsp), frame.data(), sizeof(frame)) == 0;
    std::uint64_t iterations {};
    std::memcpy(&iterations, reinterpret_cast<const void*>(counterAddress), sizeof(iterations));
    ok = result == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 && (stop || guest.gprs[0] == 73) &&
        guest.rip == frame[0] && guest.gprs[4] == initialRsp + (stop ? 0 : 8) &&
        guest.gprs[7] == counterAddress && fixture.calls == (stop ? 1 : 2) &&
        fixture.boundaryToken != 0 && evidence.deliveries == 1 && evidence.rip == base + 76 &&
        evidence.rsp == initialRsp && iterations == (stop ? 0 : 2) && canaries && fixture.ok && ok;
    ok = LumenAndroidFexCoreOwnsGuestMemoryReadLeaseV1() == 0 && ok;
    if (fixture.boundaryToken != 0)
        ok = LumenAndroidFexCoreCancelGuestBoundaryInterruptV1(fixture.boundaryToken) == 0 && ok;
    ok = ReleaseGuestCode(mapping, Page) && ok;
    std::printf("LUMEN_MACOS_FEXCORE_HOST_RETURN_BOUNDARY_TEST_V1 pass=%u stop=%u result=%u calls=%u "
        "deliveries=%u deliveryRip=0x%llx expectedRip=0x%llx deliveryRsp=0x%llx "
        "expectedRsp=0x%llx finalRsp=0x%llx iterations=%llu canaries=%u\n", ok, stop, result, fixture.calls,
        evidence.deliveries, static_cast<unsigned long long>(evidence.rip),
        static_cast<unsigned long long>(base + 76), static_cast<unsigned long long>(evidence.rsp),
        static_cast<unsigned long long>(initialRsp), static_cast<unsigned long long>(guest.gprs[4]),
        static_cast<unsigned long long>(iterations), canaries);
    return Expect(ok, "host return entry boundary must not replay the completed call");
}

bool CheckHostReturnBoundary() {
    return CheckHostReturnBoundaryCase(false) && CheckHostReturnBoundaryCase(true);
}

// Console-semantics experiment gate: the same binary reports the immutable-RX
// denial for every environment except an exact "1", which admits the writable
// range and executes it. The product default never admits one.
bool CheckWritableExecution() {
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    if (LumenAndroidFexCoreRuntimeProbeV1(&status) !=
        LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) return false;
    const char* const value = std::getenv(
        "LUMEN_EXPERIMENTAL_FEX_WRITABLE_EXECUTION");
    const bool admitted =
        value != nullptr && value[0] == '1' && value[1] == '\0';
    void* mapping = mmap(nullptr, Page, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return Expect(false, "writable execution mapping");
    // Original synthetic leaf: mov eax,55; ret
    const unsigned char code[] {0xb8, 0x37, 0x00, 0x00, 0x00, 0xc3};
    std::memcpy(mapping, code, sizeof(code));
    Stack stack;
    Fixture fixture {reinterpret_cast<std::uint64_t>(mapping), &stack};
    fixture.size = Page;
    fixture.writable = 1;
    LumenAndroidFexCoreGuestStateV1 guest {};
    guest.structSize = sizeof(guest);
    guest.abiVersion = 1;
    guest.rip = fixture.code;
    guest.gprs[4] = stack.base + Page - sizeof(std::uint64_t);
    const auto root = LumenAndroidFexCoreRootReturnAddressV1();
    std::memcpy(reinterpret_cast<void*>(guest.gprs[4]), &root, sizeof(root));
    const auto result = LumenAndroidFexCoreRunGuestV1(
        &guest, HostCall, Query, &fixture);
    const bool ok = admitted
        ? result == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
              guest.gprs[0] == 55 && guest.rip == root
        : result == LUMEN_ANDROID_FEXCORE_RUNTIME_EXECUTABLE_RANGE_DENIED_V1;
    if (!ok) {
        std::fprintf(stderr,
            "writable execution gate: admitted=%u result=%u rax=%llu rip=0x%llx\n",
            static_cast<unsigned>(admitted), result,
            static_cast<unsigned long long>(guest.gprs[0]),
            static_cast<unsigned long long>(guest.rip));
    }
    const int released = munmap(mapping, Page);
    std::printf(
        "LUMEN_MACOS_FEXCORE_WRITABLE_EXECUTION_TEST_V1 pass=%u admitted=%u "
        "result=%u rax=%llu\n",
        ok && released == 0, static_cast<unsigned>(admitted), result,
        static_cast<unsigned long long>(guest.gprs[0]));
    return ok && released == 0;
}

bool CheckBoundaryInterrupt() {
    if (!CheckBoundaryClosedStop()) return false;
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    if (LumenAndroidFexCoreRuntimeProbeV1(&status) !=
        LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) return false;
    if (!Expect(LumenAndroidFexCoreRegisterBoundaryDeliveryV1(
                    &CaptureBoundaryDelivery) == 1,
                "boundary delivery registration")) return false;
    if (!Expect(LumenAndroidFexCoreRegisterHostCallTargetV1(
                    reinterpret_cast<std::uint64_t>(&HostTarget)) == 1,
                "boundary nested host target registration")) return false;

    void* memory = mmap(nullptr, Page * 2, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return Expect(false, "boundary fixture mapping");
    // The first callback deterministically arms a second token while the
    // outer run is paused. Cleanup must preserve its page protection.
    auto* control = static_cast<std::uint64_t*>(memory);
    auto* code = static_cast<unsigned char*>(memory) + Page;
    // Original synthetic loop. [rdi]=started, [rdi+8]=limit, [rdi+16]=stop;
    // the only backedge is the taken conditional `jb loop`.
    const std::array<unsigned char, 35> loop {
        0x48, 0xc7, 0x07, 0x01, 0x00, 0x00, 0x00, // mov qword [rdi],1
        0x31, 0xc9,                               // xor ecx,ecx
        0x48, 0x8b, 0x47, 0x10,                   // mov rax,[rdi+0x10]
        0x48, 0x85, 0xc0,                         // test rax,rax
        0x75, 0x0d,                               // jne done
        0x48, 0x83, 0xc1, 0x01,                   // add rcx,1
        0x48, 0x8b, 0x47, 0x08,                   // mov rax,[rdi+8]
        0x48, 0x39, 0xc1,                         // cmp rcx,rax
        0x72, 0xea,                               // jb loop
        0x48, 0x89, 0xc8,                         // mov rax,rcx
        0xc3,                                     // ret
    };
    // Nested handler fixture: mov eax,7; ret
    const std::array<unsigned char, 6> nested {
        0xb8, 0x07, 0x00, 0x00, 0x00, 0xc3};
    std::memcpy(code, loop.data(), loop.size());
    std::memcpy(code + 64, nested.data(), nested.size());
    // Original nested handler with a real registered HLE call: movabs rax,
    // HostTarget; call rax; ret. A pure guest leaf cannot expose a missing
    // execution-to-host-reader transition around the outer delivery callback.
    std::array<unsigned char, 13> nestedHost {0x48, 0xb8, 0,0,0,0,0,0,0,0, 0xff,0xd0,0xc3};
    const auto hostTarget = reinterpret_cast<std::uint64_t>(&HostTarget);
    std::memcpy(nestedHost.data() + 2, &hostTarget, sizeof(hostTarget));
    std::memcpy(code + 80, nestedHost.data(), nestedHost.size());
    control[0] = 0;
    control[1] = 200000000;
    control[2] = 0;
    if (mprotect(code, Page, PROT_READ) != 0) {
        munmap(memory, Page * 2);
        return Expect(false, "boundary code protection");
    }
    g_boundaryCodeBase.store(reinterpret_cast<std::uint64_t>(code));
    g_boundaryCodeSize.store(loop.size());
    g_boundaryNestedCode.store(reinterpret_cast<std::uint64_t>(code + 64));
    g_boundaryNestedHostCode.store(reinterpret_cast<std::uint64_t>(code + 80));
    g_boundaryNestedHostCalls.store(0);
    g_boundaryRearm.store(true);
    g_boundaryRearmedToken.store(0);
    g_boundaryGprsPreserved.store(true);
    g_boundaryHostFpPreserved.store(true);

    std::atomic<std::uint64_t> hostIdentity {0};
    std::atomic<std::uint32_t> runnerResult {UINT32_MAX};
    std::atomic<std::uint64_t> runnerRip {0};
    std::atomic<std::uint64_t> runnerIterations {0};
    std::atomic<bool> runnerStatePreserved {false};
    const unsigned deliveriesBefore = g_boundaryDeliveries.load();

    std::thread runner([&] {
        Stack stack;
        if (!stack.base) {
            runnerResult.store(99);
            return;
        }
        g_boundaryStackBase.store(stack.base);
        g_boundaryStackSize.store(Page);
        hostIdentity.store(reinterpret_cast<std::uint64_t>(pthread_self()));
        Fixture fixture {reinterpret_cast<std::uint64_t>(code), &stack};
        fixture.size = loop.size();
        LumenAndroidFexCoreGuestStateV3 checkpoint;
        auto& guest = checkpoint.gp;
        for (unsigned reg = 0; reg != 16; ++reg) {
            guest.gprs[reg] = UINT64_C(0x1234000056780000) + reg;
            for (unsigned byte = 0; byte != 32; ++byte)
                checkpoint.vectorRegisters[reg][byte] = static_cast<std::uint8_t>(reg * 37 + byte);
        }
        guest.rip = fixture.code;
        guest.gprs[4] = stack.base + Page - sizeof(std::uint64_t);
        guest.gprs[7] = reinterpret_cast<std::uint64_t>(control);
        const auto root = LumenAndroidFexCoreRootReturnAddressV1();
        std::memcpy(reinterpret_cast<void*>(guest.gprs[4]), &root,
                    sizeof(root));
        const auto initial = checkpoint;
        const int previousRounding = std::fegetround();
        const bool hostFpReady = previousRounding != -1 && std::fesetround(FE_DOWNWARD) == 0;
        const auto result = hostFpReady
            ? LumenAndroidFexCoreRunGuestV3(&checkpoint, BoundaryHostCallV2, Query, &fixture)
            : LUMEN_ANDROID_FEXCORE_RUNTIME_THREAD_SETUP_FAILED_V1;
        bool preserved = std::fegetround() == FE_DOWNWARD &&
            checkpoint.mxcsr == initial.mxcsr &&
            checkpoint.x87ControlWord == initial.x87ControlWord &&
            std::memcmp(checkpoint.vectorRegisters, initial.vectorRegisters,
                        sizeof(checkpoint.vectorRegisters)) == 0;
        for (unsigned reg = 2; reg != 16; ++reg)
            preserved &= guest.gprs[reg] == initial.gp.gprs[reg] + (reg == 4 ? 8U : 0U);
        if (previousRounding != -1) preserved &= std::fesetround(previousRounding) == 0;
        runnerStatePreserved.store(preserved);
        runnerResult.store(result);
        runnerRip.store(guest.rip);
        runnerIterations.store(guest.gprs[0]);
    });

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (std::atomic_ref<std::uint64_t>(control[0]).load(
               std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    bool ok = Expect(std::atomic_ref<std::uint64_t>(control[0]).load(
                         std::memory_order_acquire) == 1,
                     "boundary fixture loop did not start");
    const std::uint64_t token = ok
        ? LumenAndroidFexCoreRequestGuestBoundaryInterruptV1(
              hostIdentity.load(std::memory_order_acquire))
        : 0;
    ok = Expect(token != 0,
                "no active FEX run matched the requesting host") && ok;
    while (g_boundaryDeliveries.load() < deliveriesBefore + 2 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ok = Expect(g_boundaryDeliveries.load() == deliveriesBefore + 2,
                "boundary interrupt or its callback-rearmed token did not deliver inside the loop") && ok;
    std::atomic_ref<std::uint64_t>(control[2]).store(
        1, std::memory_order_release);
    runner.join();
    ok = Expect(runnerResult.load() == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
                    runnerRip.load() == LumenAndroidFexCoreRootReturnAddressV1() &&
                    runnerIterations.load() != 0,
                "boundary fixture loop run did not complete") && ok;
    ok = Expect(g_boundaryStateInCode.load() && g_boundaryStateOnStack.load(),
                "boundary state was outside the guest code or stack") && ok;
    ok = Expect(g_boundaryCounter.load() < control[1],
                "boundary delivered after the loop reached its limit") && ok;
    ok = Expect(g_boundaryNested.load() == 7,
                "nested guest call inside the boundary delivery did not run") && ok;
    ok = Expect(g_boundaryNestedHostCalls.load() == 2,
                "nested HLE call did not run exactly once per delivery") && ok;
    ok = Expect(runnerStatePreserved.load() && g_boundaryGprsPreserved.load() &&
                    g_boundaryHostFpPreserved.load(),
                "boundary delivery changed outer GP/vector/FP state or did not restore the native FP environment") && ok;
    const auto rearmedToken = g_boundaryRearmedToken.load();
    ok = Expect(rearmedToken != 0 && rearmedToken != token &&
                    LumenAndroidFexCoreCancelGuestBoundaryInterruptV1(rearmedToken) == 0,
                "callback-rearmed token was lost, reused or delivered more than once") && ok;
    if (token != 0) {
        ok = Expect(LumenAndroidFexCoreCancelGuestBoundaryInterruptV1(token) == 0,
                    "an already delivered boundary was still cancellable") && ok;
    }
    ok = Expect(LumenAndroidFexCoreRequestGuestBoundaryInterruptV1(
                    reinterpret_cast<std::uint64_t>(pthread_self())) == 0,
                "armed a host thread without an active run") && ok;

    // A request cancelled before any boundary must not deliver, and the
    // resumed run must continue with the interrupt page writable again.
    if (!Expect(LumenAndroidFexCoreRegisterHostCallTargetV1(
                    reinterpret_cast<std::uint64_t>(&HostTarget)) != 0,
                "boundary cancel fixture host target registration")) {
        munmap(memory, Page * 2);
        return false;
    }
    void* cancelMemory = mmap(nullptr, Page * 2, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cancelMemory == MAP_FAILED) {
        munmap(memory, Page * 2);
        return Expect(false, "boundary cancel fixture mapping");
    }
    auto* cancelCode = static_cast<unsigned char*>(cancelMemory) + Page;
    std::array<unsigned char, 18> callCode {
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, // movabs rax,target
        0xff, 0xd0,                         // call rax
        0xb8, 0x05, 0x00, 0x00, 0x00,       // mov eax,5
        0xc3,                               // ret
    };
    const std::uint64_t target = reinterpret_cast<std::uint64_t>(&HostTarget);
    std::memcpy(callCode.data() + 2, &target, sizeof(target));
    std::memcpy(cancelCode, callCode.data(), callCode.size());
    if (mprotect(cancelCode, Page, PROT_READ) != 0) {
        munmap(cancelMemory, Page * 2);
        munmap(memory, Page * 2);
        return Expect(false, "boundary cancel code protection");
    }
    std::atomic<bool> gate {false};
    std::atomic<bool> entered {false};
    std::atomic<std::uint64_t> cancelIdentity {0};
    std::atomic<std::uint32_t> cancelResult {UINT32_MAX};
    std::atomic<std::uint64_t> cancelValue {0};
    std::thread cancelRunner([&] {
        Stack stack;
        if (!stack.base) {
            cancelResult.store(99);
            return;
        }
        cancelIdentity.store(reinterpret_cast<std::uint64_t>(pthread_self()));
        Fixture fixture {reinterpret_cast<std::uint64_t>(cancelCode), &stack};
        fixture.size = callCode.size();
        fixture.countCache = true;
        fixture.hostCallGate = &gate;
        fixture.hostCallEntered = &entered;
        LumenAndroidFexCoreGuestStateV1 guest {};
        guest.structSize = sizeof(guest);
        guest.abiVersion = 1;
        guest.rip = fixture.code;
        guest.gprs[4] = stack.base + Page - sizeof(std::uint64_t);
        const auto root = LumenAndroidFexCoreRootReturnAddressV1();
        std::memcpy(reinterpret_cast<void*>(guest.gprs[4]), &root,
                    sizeof(root));
        const auto result = LumenAndroidFexCoreRunGuestV1(
            &guest, HostCall, Query, &fixture);
        cancelResult.store(result);
        cancelValue.store(guest.gprs[0]);
    });
    const auto cancelDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (!entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < cancelDeadline) {
        std::this_thread::yield();
    }
    ok = Expect(entered.load(std::memory_order_acquire),
                "boundary cancel fixture did not reach its host call") && ok;
    const unsigned deliveriesBeforeCancel = g_boundaryDeliveries.load();
    const std::uint64_t cancelToken =
        LumenAndroidFexCoreRequestGuestBoundaryInterruptV1(
            cancelIdentity.load(std::memory_order_acquire));
    ok = Expect(cancelToken != 0,
                "no active FEX run matched the cancelled request") && ok;
    if (cancelToken != 0) {
        // The target is parked inside its host call, so no boundary can fire
        // between these two requests: the second one must coalesce.
        ok = Expect(LumenAndroidFexCoreRequestGuestBoundaryInterruptV1(
                        cancelIdentity.load(std::memory_order_acquire)) ==
                        cancelToken,
                    "repeated boundary request did not coalesce") && ok;
        ok = Expect(LumenAndroidFexCoreCancelGuestBoundaryInterruptV1(
                        cancelToken) == 1,
                    "boundary cancel was not accepted") && ok;
        ok = Expect(LumenAndroidFexCoreCancelGuestBoundaryInterruptV1(
                        cancelToken) == 0,
                    "boundary cancel was not idempotent") && ok;
    }
    gate.store(true, std::memory_order_release);
    cancelRunner.join();
    ok = Expect(cancelResult.load() == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
                    cancelValue.load() == 5,
                "cancelled boundary run did not complete") && ok;
    ok = Expect(g_boundaryDeliveries.load() == deliveriesBeforeCancel,
                "a cancelled boundary still delivered") && ok;
    g_boundaryNestedCode.store(0);
    g_boundaryNestedHostCode.store(0);
    ok = Expect(munmap(cancelMemory, Page * 2) == 0,
                "boundary cancel fixture release") && ok;
    ok = Expect(munmap(memory, Page * 2) == 0, "boundary fixture release") && ok;
    std::printf(
        "LUMEN_MACOS_FEXCORE_BOUNDARY_INTERRUPT_TEST_V1 pass=%u deliveries=%u "
        "rip=0x%llx rsp=0x%llx counter=%llu nested=%u nestedHle=%u state=%u rearm=%u cancel=%u\n",
        ok, g_boundaryDeliveries.load(),
        static_cast<unsigned long long>(g_boundaryRip.load()),
        static_cast<unsigned long long>(g_boundaryRsp.load()),
        static_cast<unsigned long long>(g_boundaryCounter.load()),
        g_boundaryNested.load(), g_boundaryNestedHostCalls.load(), runnerStatePreserved.load(),
        rearmedToken != 0, cancelToken != 0 &&
            cancelResult.load() == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1);
    return ok;
}

// The late CALL return is an entry inside the original compilation unit.
// Memory instrumentation makes that entry exceed ADR's one-MiB range; the
// shared code allocator then relocates the unit without page alignment.
bool RunLargeUnitHeaderChild() {
    if (setenv("LUMEN_MACOS_FEXCORE_FAULT_CONTEXT", "1", 1) != 0) return false;
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    if (LumenAndroidFexCoreRuntimeProbeV1(&status) != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 ||
        !LumenAndroidFexCoreRegisterBoundaryDeliveryV1(&CaptureBoundaryDelivery) ||
        !LumenAndroidFexCoreRegisterHostCallTargetV1(reinterpret_cast<std::uint64_t>(&HostTarget))) return false;
    constexpr unsigned Operations = 4900;
    std::vector<unsigned char> program;
    const auto target = reinterpret_cast<std::uint64_t>(&HostTarget);
    program.insert(program.end(), {0x48, 0xb8, 0,0,0,0,0,0,0,0, 0xff, 0xd0});
    std::memcpy(program.data() + 2, &target, sizeof(target));
    for (unsigned i = 0; i < Operations; ++i)
        program.insert(program.end(), {0xf0, 0x48, 0xff, 0x07}); // lock inc qword [rdi]
    const auto callOffset = program.size();
    program.insert(program.end(), {0x48, 0xb8, 0,0,0,0,0,0,0,0, 0xff, 0xd0, 0xb8, 73,0,0,0, 0xc3});
    std::memcpy(program.data() + callOffset + 2, &target, sizeof(target));
    const auto bytes = (program.size() + Page - 1) & ~(Page - 1);
    void* mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return false;
    std::memcpy(mapping, program.data(), program.size());
    if (mprotect(mapping, bytes, PROT_READ) != 0) { munmap(mapping, bytes); return false; }
    const auto base = reinterpret_cast<std::uint64_t>(mapping);
    bool ok = true;
    for (unsigned iteration = 0; iteration < 2; ++iteration) {
        Stack stack;
        if (stack.base == 0) { ok = false; break; }
        Fixture fixture {base, &stack, 1};
        fixture.size = bytes;
        fixture.armBoundaryAfterReturn = true;
        fixture.armBoundaryEveryReturn = true;
        HostReturnBoundaryEvidence evidence;
        g_hostReturnBoundaryEvidence = &evidence;
        LumenAndroidFexCoreGuestStateV1 guest {};
        guest.structSize = sizeof(guest); guest.abiVersion = 1; guest.rip = base;
        guest.gprs[4] = stack.base + Page - 32;
        guest.gprs[7] = stack.base + 128;
        const std::array<std::uint64_t, 3> frame {
            LumenAndroidFexCoreRootReturnAddressV1(), UINT64_C(0xd1c2b3a4978655e6), UINT64_C(0xfedcba9876543210)};
        std::memcpy(reinterpret_cast<void*>(guest.gprs[4]), frame.data(), sizeof(frame));
        const auto result = LumenAndroidFexCoreRunGuestV1(&guest, HostCall, Query, &fixture);
        g_hostReturnBoundaryEvidence = nullptr;
        std::uint64_t increments {};
        std::memcpy(&increments, reinterpret_cast<void*>(stack.base + 128), sizeof(increments));
        std::array<std::uint64_t, 3> after;
        std::memcpy(after.data(), reinterpret_cast<void*>(stack.base + Page - 32), sizeof(after));
        const bool pass = result == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 && fixture.ok &&
            fixture.calls == 2 && guest.gprs[0] == 73 && guest.rip == frame[0] &&
            guest.gprs[4] == stack.base + Page - 24 && increments == Operations && after == frame &&
            fixture.boundaryToken != 0 && evidence.deliveries == 2 &&
            evidence.rip == base + callOffset + 12 && evidence.rsp == stack.base + Page - 32;
        std::fprintf(stderr, "LUMEN_MACOS_FEXCORE_LARGE_UNIT_HEADER_V1 iteration=%u pass=%u result=%u "
            "increments=%llu deliveries=%u returnRip=0x%llx expectedRip=0x%llx\n", iteration, pass, result,
            static_cast<unsigned long long>(increments), evidence.deliveries,
            static_cast<unsigned long long>(evidence.rip), static_cast<unsigned long long>(base + callOffset + 12));
        ok &= pass;
        if (!ok) break;
    }
    ok = ReleaseGuestCode(mapping, bytes) && ok;
    return ok;
}

bool CheckLargeUnitHeader() {
    int reportPipe[2];
    if (pipe(reportPipe) != 0) return false;
    const auto child = fork();
    if (child < 0) { close(reportPipe[0]); close(reportPipe[1]); return false; }
    if (child == 0) {
        close(reportPipe[0]);
        if (dup2(reportPipe[1], STDERR_FILENO) < 0 || close(reportPipe[1]) != 0) _exit(96);
        alarm(30);
        _exit(RunLargeUnitHeaderChild() ? 0 : 1);
    }
    close(reportPipe[1]);
    std::array<char, 32768> report {};
    std::size_t total {};
    // Drain completely even if diagnostics exceed the retained prefix.
    std::array<char, 4096> chunk {};
    for (;;) {
        const auto count = read(reportPipe[0], chunk.data(), chunk.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        const auto keep = std::min(static_cast<std::size_t>(count), report.size() - 1 - total);
        std::memcpy(report.data() + total, chunk.data(), keep); total += keep;
    }
    close(reportPipe[0]);
    int status {};
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    unsigned qualified {};
    unsigned validNear {};
    unsigned long long hostPc {};
    unsigned long long firstUnit {};
    bool sameUnit = true;
    const char* record = report.data();
    while (*record != '\0') {
        if (std::strncmp(record, "[MACOS_FEXCORE_SAFEPOINT_RESUME_V1]", 35) == 0) {
            const auto* pc = std::strstr(record, "hostPc=");
            if (pc == nullptr || std::sscanf(pc, "hostPc=0x%llx", &hostPc) != 1) hostPc = 0;
        } else if (std::strncmp(record, "[MACOS_FEXCORE_SAFEPOINT_CODE_WITNESS_V1]", 41) == 0) {
            unsigned long long begin {}, size {};
            const auto* beginField = std::strstr(record, "unitBegin=");
            const auto* sizeField = std::strstr(record, "unitSize=");
            if (FaultContextRecordContains(record, "scope=guest") &&
                FaultContextRecordContains(record, "unitValid=1") &&
                FaultContextRecordContains(record, "pcInUnit=1") &&
                beginField != nullptr && sizeField != nullptr &&
                std::sscanf(beginField, "unitBegin=0x%llx", &begin) == 1 &&
                std::sscanf(sizeField, "unitSize=%llu", &size) == 1 && size > (1U << 20) &&
                (begin & 4095) != 0 && hostPc >= begin && hostPc - begin < size) {
                if (firstUnit == 0) firstUnit = begin;
                sameUnit &= firstUnit == begin;
                if (hostPc - begin > (1U << 20) + 32) ++qualified;
                else ++validNear;
            }
        }
        const auto* next = std::strchr(record, '\n');
        if (next == nullptr) break;
        record = next + 1;
    }
    const bool ok = waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        qualified == 2 && validNear == 2 && sameUnit;
    std::printf("LUMEN_MACOS_FEXCORE_LARGE_UNIT_HEADER_TEST_V1 pass=%u status=0x%x "
        "largeRelocatedSafepoints=%u coldWarmRuns=2\n", ok, status, qualified);
    std::fwrite(report.data(), 1, total, ok ? stdout : stderr);
    return ok;
}
}

int main(int argc, char** argv) {
    const bool blockedSignalCase = argc == 2 && std::strcmp(argv[1], "--blocked-usr1") == 0;
    const bool mutationCloseCase = argc == 2 && std::strcmp(argv[1], "--mutation-close") == 0;
    const bool nullIndirectCase = argc == 2 && std::strcmp(argv[1], "--null-indirect-diagnostic") == 0;
    const bool boundaryInterruptCase = argc == 2 && std::strcmp(argv[1], "--boundary-interrupt") == 0;
    const bool hostReturnBoundaryCase = argc == 2 && std::strcmp(argv[1], "--host-return-boundary") == 0;
    const bool largeUnitHeaderCase = argc == 2 && std::strcmp(argv[1], "--large-unit-header") == 0;
    const bool writableExecutionCase = argc == 2 && std::strcmp(argv[1], "--writable-execution") == 0;
    const bool staticGprsDiagnosticCase = argc == 2 && std::strcmp(argv[1], "--static-gprs-diagnostic") == 0;
    if (argc != 1 && !blockedSignalCase && !mutationCloseCase && !nullIndirectCase &&
        !boundaryInterruptCase && !hostReturnBoundaryCase && !largeUnitHeaderCase && !writableExecutionCase && !staticGprsDiagnosticCase) return 2;
    if (!Expect(LumenMacOSFexCoreRegisterDataAccessQueryV1(QueryDataAccess) == 1,
                "fixture data access query")) return 1;
    if (writableExecutionCase) {
        return CheckWritableExecution() ? 0 : 1;
    }
    if (staticGprsDiagnosticCase) return CheckRawStaticGprDiagnostic() ? 0 : 1;
    if (hostReturnBoundaryCase) return CheckHostReturnBoundary() ? 0 : 1;
    if (largeUnitHeaderCase) return CheckLargeUnitHeader() ? 0 : 1;
    if (boundaryInterruptCase) {
        return CheckBoundaryInterrupt() ? 0 : 1;
    }
    if (nullIndirectCase) {
        const bool call = CheckNullIndirectTransfer(true);
        const bool jump = CheckNullIndirectTransfer(false);
        return call && jump ? 0 : 1;
    }
    if (mutationCloseCase) {
        const bool callbacks = Expect(CheckMutationCloseCallbacks(),
            "mutation close callback ownership, rejection and empty publication");
        const bool branch = callbacks && Expect(CheckBranchInvalidation(),
            "mutation close callback and warmed branch invalidation");
        std::printf("LUMEN_MACOS_FEXCORE_MUTATION_CLOSE_TEST_V2 pass=%u "
                    "callbacks=%u branchInvalidation=%u generations=8\n",
                    callbacks && branch, callbacks, branch);
        return callbacks && branch ? 0 : 1;
    }
    sigset_t originalMask;
    if (blockedSignalCase) {
        // Reproduce a launcher inheriting a blocked SIGUSR1. The scope probe
        // must obtain delivery temporarily and restore that inherited mask.
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGUSR1);
        if (pthread_sigmask(SIG_BLOCK, &blocked, &originalMask) != 0) return 1;
    }
    const bool wx = Expect(CheckWx(), "W^X, signal scope and per-thread isolation");
    if (blockedSignalCase) {
        sigset_t restored;
        const bool blocked = pthread_sigmask(SIG_SETMASK, nullptr, &restored) == 0 &&
            sigismember(&restored, SIGUSR1) == 1;
        const bool maskRestored = pthread_sigmask(SIG_SETMASK, &originalMask, nullptr) == 0;
        std::printf("LUMEN_MACOS_FEXCORE_WX_BLOCKED_SIGNAL_TEST_V1 pass=%u wx=%u "
                    "blockedSignalRestored=%u\n", wx && blocked && maskRestored, wx, blocked);
        return wx && blocked && maskRestored ? 0 : 1;
    }
    const bool guestExceptionDiagnostic = wx && Expect(
        CheckGuestExceptionDiagnostic(),
        "guest exception route and generated-fault diagnostic");
    const bool staticGprsDiagnostic = guestExceptionDiagnostic && Expect(
        CheckRawStaticGprDiagnostic(), "raw runtime static GPR fault diagnostic");
    const bool deniedTransition = staticGprsDiagnostic && Expect(
        CheckDeniedTransitionDiagnostic(),
        "denied transition source compilation unit");
    const bool concurrent = deniedTransition && Expect(
        CheckConcurrent(), "concurrent nested guest calls");
    const bool mutationClose = concurrent && Expect(CheckMutationCloseCallbacks(),
        "mutation close callback contract");
    const bool branch = mutationClose && Expect(CheckBranchInvalidation(), "branch invalidation");
    const bool boundary = branch && Expect(CheckBoundaryInterrupt(),
        "generated-code boundary interrupt delivery") && CheckHostReturnBoundary();
    unsigned cacheChanges {}, cacheCalls {};
    const bool exhaustion = boundary && Expect(CheckCacheExhaustion(&cacheChanges, &cacheCalls),
                                             "natural cache exhaustion and host target regeneration");
    const bool passed = wx && guestExceptionDiagnostic && deniedTransition &&
        concurrent && mutationClose && branch && boundary && exhaustion;
    std::printf("LUMEN_MACOS_FEXCORE_JIT_TEST_V2 pass=%u wx=%u nestedScopes=%u "
        "writeDeniedInExecute=%u executeDeniedInWrite=%u concurrent=%u threads=%zu "
        "runsExpected=%zu nestedGuest=%u x18Preserved=%u signalScope=%u threadWxIsolation=%u "
        "guestExceptionDiagnostic=%u deniedTransition=%u mutationClose=%u branchInvalidation=%u "
        "boundaryInterrupt=%u generations=8 lazyBlocks=%zu cacheExhaustion=%u "
        "cacheChanges=%u cacheHostCalls=%u publisher=thread-toggle callbackPublisher=0 "
        "qualifiedRuntimeReady=0\n", passed, wx, wx, wx, wx, concurrent,
        Workers, Workers * Iterations, concurrent, concurrent, wx, wx,
        guestExceptionDiagnostic, deniedTransition, mutationClose, branch,
        boundary, CacheBlocks, exhaustion, cacheChanges, cacheCalls);
    return passed ? 0 : 1;
}
