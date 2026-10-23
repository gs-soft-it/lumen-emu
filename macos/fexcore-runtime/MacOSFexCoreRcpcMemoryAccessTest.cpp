#include "MacOSFexCoreMemoryAccess.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern "C" void LumenRcpcAcquireReads();
extern "C" void LumenRcpcImmediateReads();
extern "C" void LumenRcpcImmediateWrites();
extern "C" void LumenAtomicAcquireReads();
extern "C" void LumenAtomicReleaseWrites();

namespace {
using namespace Lumen::MacOS::FexCore;
using AccessFunction = std::uint64_t (*)(void*, std::uint64_t);
constexpr std::array<int, 5> Offsets {-256, -1, 0, 1, 255};
sigjmp_buf faultJump;
volatile sig_atomic_t faultSignal {};
volatile sig_atomic_t faultCode {};
volatile std::uint32_t faultEsr {};
volatile std::uintptr_t faultPc {};
volatile std::uintptr_t faultBase {};

void FaultHandler(int signal, siginfo_t* info, void* opaque) {
    auto* context = static_cast<ucontext_t*>(opaque);
    faultSignal = signal;
    faultCode = info->si_code;
    faultEsr = context->uc_mcontext->__es.__esr;
    faultPc = context->uc_mcontext->__ss.__pc;
    faultBase = context->uc_mcontext->__ss.__x[0];
    siglongjmp(faultJump, 1);
}

struct Invocation {
    bool completed {};
    std::uint64_t value {};
};

Invocation Invoke(std::uintptr_t code, std::uintptr_t base, std::uint64_t value = 0) {
    faultSignal = 0;
    if (sigsetjmp(faultJump, 1) != 0) return {};
    const auto function = reinterpret_cast<AccessFunction>(code);
    return {true, function(reinterpret_cast<void*>(base), value)};
}

bool Capability(const char* name) {
    int result {};
    std::size_t size = sizeof(result);
    return sysctlbyname(name, &result, &size, nullptr, 0) == 0 &&
        size == sizeof(result) && result == 1;
}

unsigned checks {}, failures {};
unsigned unalignedNative {}, unalignedFaults {};
unsigned baselineUnalignedNative {}, baselineUnalignedFaults {};
void Check(bool success, const char* name) {
    ++checks;
    if (!success) {
        ++failures;
        std::fprintf(stderr, "RCPC memory access: FAIL %s check=%u\n", name, checks);
    }
}

void CheckDecoded(std::uintptr_t code, unsigned width, int offset, bool write) {
    const auto instruction = *reinterpret_cast<const std::uint32_t*>(code);
    const auto decoded = DecodeMemoryAccess(instruction);
    Check(decoded.valid && decoded.width == width && decoded.baseRegister == 0 &&
          decoded.byteOffset == offset && decoded.write == write,
          "assembler footprint");
    // Vary the actual immediate in the independently assembled instruction.
    // Cover every signed value, every base register, and both span boundaries.
    const bool immediate = (instruction & 0x3fe00c00U) ==
        (write ? 0x19000000U : 0x19400000U);
    for (unsigned baseRegister = 0; baseRegister != 32; ++baseRegister) {
        for (int displacement = immediate ? -256 : 0;
             displacement <= (immediate ? 255 : 0); ++displacement) {
            const auto bits = static_cast<unsigned>(displacement) & 511U;
            auto changed = (instruction & ~0x3e0U) | (baseRegister << 5);
            if (immediate) changed = (changed & ~0x1ff000U) | (bits << 12);
            const auto footprint = DecodeMemoryAccess(changed);
            Check(footprint.valid && footprint.width == width &&
                  footprint.baseRegister == baseRegister &&
                  footprint.byteOffset == displacement && footprint.write == write,
                  "signed immediate and base");
        }
    }
    for (std::uintptr_t base : {std::uintptr_t {0}, std::uintptr_t {256},
                              UINTPTR_MAX - 256, UINTPTR_MAX}) {
        std::uintptr_t address = 0xa5a5;
        const bool resolved = ResolveMemoryAccessAddress(decoded, base, &address);
        const auto mathematical = static_cast<__int128>(base) + offset;
        const bool expected = mathematical >= 0 && mathematical + width <= UINTPTR_MAX;
        Check(resolved == expected && (resolved ? address == mathematical : address == 0xa5a5),
              "checked address boundaries");
    }
}

void CheckFault(std::uintptr_t code, std::uintptr_t base,
                std::uintptr_t expectedAddress, unsigned width,
                bool alignment) {
    const auto call = Invoke(code, base, 0x0123456789abcdefULL);
    Check(!call.completed && (faultSignal == SIGBUS || faultSignal == SIGSEGV),
          "native access fault");
    if (call.completed) return;
    Check(faultPc == code && faultBase == base, "fault context belongs to access");
    const auto decoded = DecodeMemoryAccess(*reinterpret_cast<const std::uint32_t*>(code));
    std::uintptr_t address {};
    Check(ResolveMemoryAccessAddress(decoded, faultBase, &address) &&
          address == expectedAddress && decoded.width == width,
          "native fault effective span");
    Check(IsDataAlignmentFault(faultEsr) == alignment, "alignment versus permission ESR");
}

void CheckUnaligned(std::uintptr_t code, std::uintptr_t base,
                    std::uintptr_t address, unsigned width, bool write,
                    bool baseline = false) {
    // Whether the host faults is itself part of the observation. Both outcomes
    // must preserve the complete access; a completed store is never mistaken
    // for an emulated fault with unchanged bytes.
    constexpr std::uint64_t value = 0x8172635445362718ULL;
    std::array<std::uint8_t, 16> expected {};
    std::memcpy(expected.data(), reinterpret_cast<void*>(address - 4), expected.size());
    std::uint64_t expectedLoad {};
    std::memcpy(&expectedLoad, reinterpret_cast<void*>(address), width);
    const auto call = Invoke(code, base, value);
    if (call.completed) {
        baseline ? ++baselineUnalignedNative : ++unalignedNative;
        if (write) std::memcpy(expected.data() + 4, &value, width);
        Check((write || call.value == expectedLoad) && std::memcmp(expected.data(),
              reinterpret_cast<void*>(address - 4), expected.size()) == 0,
              "unaligned native value and complete payload/canaries");
    } else {
        baseline ? ++baselineUnalignedFaults : ++unalignedFaults;
        Check(std::memcmp(expected.data(), reinterpret_cast<void*>(address - 4),
                          expected.size()) == 0, "unaligned fault has no effect");
        Check((faultSignal == SIGBUS || faultSignal == SIGSEGV) &&
              faultPc == code && faultBase == base && IsDataAlignmentFault(faultEsr),
              "unaligned fault context");
        const auto decoded = DecodeMemoryAccess(*reinterpret_cast<const std::uint32_t*>(code));
        std::uintptr_t resolved {};
        Check(ResolveMemoryAccessAddress(decoded, faultBase, &resolved) &&
              resolved == address && decoded.width == width && decoded.write == write,
              "unaligned fault span");
    }
}
} // namespace

int main() {
    const bool rcpc = Capability("hw.optional.arm.FEAT_LRCPC");
    const bool rcpc2 = Capability("hw.optional.arm.FEAT_LRCPC2");
    if (!rcpc || !rcpc2) {
        std::fprintf(stderr, "RCPC/RCPC2 hardware absent: native effects NON_VERIFIED\n");
        return 77;
    }
    const auto pageSize = static_cast<std::size_t>(getpagesize());
    auto* bytes = static_cast<std::uint8_t*>(mmap(nullptr, 3 * pageSize,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    if (bytes == MAP_FAILED) return 1;
    std::memset(bytes, 0xa5, 3 * pageSize);
    struct sigaction action {}, oldBus {}, oldSegv {};
    action.sa_sigaction = FaultHandler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGBUS, &action, &oldBus) != 0 ||
        sigaction(SIGSEGV, &action, &oldSegv) != 0) return 1;

    const auto acquire = reinterpret_cast<std::uintptr_t>(&LumenRcpcAcquireReads);
    const auto reads = reinterpret_cast<std::uintptr_t>(&LumenRcpcImmediateReads);
    const auto writes = reinterpret_cast<std::uintptr_t>(&LumenRcpcImmediateWrites);
    const auto atomicReads = reinterpret_cast<std::uintptr_t>(&LumenAtomicAcquireReads);
    const auto atomicWrites = reinterpret_cast<std::uintptr_t>(&LumenAtomicReleaseWrites);
    constexpr std::uint64_t payload = 0x8172635445362718ULL;
    const auto target = reinterpret_cast<std::uintptr_t>(bytes + pageSize + 512);
    for (unsigned size = 0; size != 4; ++size) {
        const unsigned width = 1U << size;
        const auto mask = width == 8 ? UINT64_MAX : (1ULL << (8 * width)) - 1;
        const auto code = acquire + 8 * size;
        CheckDecoded(code, width, 0, false);
        std::memcpy(reinterpret_cast<void*>(target), &payload, sizeof(payload));
        const auto loaded = Invoke(code, target);
        Check(loaded.completed && loaded.value == (payload & mask), "LDAPR native value");
        if (width != 1) CheckUnaligned(code, target + 1, target + 1, width, false);
        for (std::size_t offsetIndex = 0; offsetIndex != Offsets.size(); ++offsetIndex) {
            const auto offset = Offsets[offsetIndex];
            const auto base = static_cast<std::uintptr_t>(static_cast<__int128>(target) - offset);
            const auto entry = 8 * (4 * offsetIndex + size);
            CheckDecoded(reads + entry, width, offset, false);
            CheckDecoded(writes + entry, width, offset, true);
            std::memcpy(reinterpret_cast<void*>(target), &payload, sizeof(payload));
            const auto read = Invoke(reads + entry, base);
            Check(read.completed && read.value == (payload & mask), "LDAPUR native value");
            std::array<std::uint8_t, 16> before {};
            std::memset(reinterpret_cast<void*>(target - 4), 0xa5, before.size());
            std::memcpy(before.data(), reinterpret_cast<void*>(target - 4), before.size());
            const auto write = Invoke(writes + entry, base, payload);
            std::memcpy(before.data() + 4, &payload, width);
            Check(write.completed && std::memcmp(before.data(),
                  reinterpret_cast<void*>(target - 4), before.size()) == 0,
                  "STLUR complete payload and canaries");
            if (width != 1) {
                CheckUnaligned(reads + entry, base + 1, target + 1, width, false);
                CheckUnaligned(writes + entry, base + 1, target + 1, width, true);
            }
        }
    }

    // Apple hosts can handle some misalignment natively even with the old
    // LDAR/STLR lowering. Compare the identical addresses, including crossings
    // of 16-byte boundaries, before attributing any reduction in faults to RCpc.
    for (unsigned size = 1; size != 4; ++size) {
        const unsigned width = 1U << size;
        for (unsigned phase = 1; phase != 32; ++phase) {
            const auto address = target + phase;
            std::memcpy(reinterpret_cast<void*>(address), &payload, width);
            CheckUnaligned(atomicReads + 8 * size, address, address, width, false, true);
            CheckUnaligned(atomicWrites + 8 * size, address, address, width, true, true);
            CheckUnaligned(acquire + 8 * size, address, address, width, false);
            // Immediate zero is the third group in the original fixtures.
            CheckUnaligned(reads + 8 * (8 + size), address, address, width, false);
            CheckUnaligned(writes + 8 * (8 + size), address, address, width, true);
        }
    }

    Check(mprotect(bytes + 2 * pageSize, pageSize, PROT_NONE) == 0, "guard page");
    const auto boundary = reinterpret_cast<std::uintptr_t>(bytes + 2 * pageSize - 1);
    for (std::size_t offsetIndex = 0; offsetIndex != Offsets.size(); ++offsetIndex) {
        const auto offset = Offsets[offsetIndex];
        const auto base = static_cast<std::uintptr_t>(static_cast<__int128>(boundary) - offset);
        const auto entry = 8 * (4 * offsetIndex + 3);
        CheckFault(reads + entry, base, boundary, 8, true);
        const auto before = bytes[2 * pageSize - 1];
        CheckFault(writes + entry, base, boundary, 8, true);
        Check(bytes[2 * pageSize - 1] == before, "cross-page store has no effect");
    }
    Check(mprotect(bytes + pageSize, pageSize, PROT_READ) == 0, "read-only page");
    for (std::size_t offsetIndex = 0; offsetIndex != Offsets.size(); ++offsetIndex) {
        const auto base = static_cast<std::uintptr_t>(static_cast<__int128>(target) - Offsets[offsetIndex]);
        const auto entry = 8 * (4 * offsetIndex + 3);
        std::uint64_t before {};
        std::memcpy(&before, reinterpret_cast<void*>(target), sizeof(before));
        CheckFault(writes + entry, base, target, 8, false);
        std::uint64_t after {};
        std::memcpy(&after, reinterpret_cast<void*>(target), sizeof(after));
        Check(before == after, "read-only store has no effect");
    }
    for (const auto instruction : {0xd503201fU, 0xf8bfc4a3U, 0xf94000a3U,
                                   0xd98000a3U, 0x598000a3U, 0x798000a3U})
        Check(!DecodeMemoryAccess(instruction).valid, "reserved/unqualified access");
    std::uintptr_t untouched = 0x1234;
    Check(!ResolveMemoryAccessAddress({}, 1000, &untouched) && untouched == 0x1234,
          "invalid footprint rejects without output");
    Check(!ResolveMemoryAccessAddress({true, 0, INT64_MIN, 8, false}, 0, &untouched),
          "INT64_MIN does not overflow");
    Check(!ResolveMemoryAccessAddress({true, 0, 0, 8, false}, 0, nullptr),
          "null output rejected");

    sigaction(SIGBUS, &oldBus, nullptr);
    sigaction(SIGSEGV, &oldSegv, nullptr);
    munmap(bytes, 3 * pageSize);
    std::printf("LUMEN_MACOS_RCPC_MEMORY_ACCESS_V1 checks=%u failures=%u "
                "native_read_write=1 unaligned_native=%u unaligned_faults=%u "
                "baseline_unaligned_native=%u baseline_unaligned_faults=%u "
                "cross_page_fault=1 permission_fault=1 pass=%u\n", checks, failures,
                unalignedNative, unalignedFaults, baselineUnalignedNative,
                baselineUnalignedFaults, failures == 0);
    return failures != 0;
}
