#include "MacOSFexCoreSignalPolicy.h"

#include <FEXCore/Debug/InternalThreadState.h>

#include <atomic>
#include <csetjmp>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

namespace {

constexpr std::size_t kInterruptPageOffset =
    offsetof(FEXCore::Core::InternalThreadState, InterruptFaultPage) -
    offsetof(FEXCore::Core::InternalThreadState, BaseFrameState);
constexpr std::uint32_t kInterruptDoorbellStore =
    Lumen::MacOS::FexCore::EncodeInterruptDoorbellStore(
        kInterruptPageOffset);
constexpr std::size_t kNativeDoorbellOffset =
    offsetof(FEXCore::Core::InternalThreadState, InterruptFaultPage);
static_assert(kInterruptPageOffset % sizeof(std::uint64_t) == 0);
static_assert(kInterruptPageOffset <= 32760);
static_assert(kInterruptDoorbellStore != 0);

sigjmp_buf gSignalReturn;
volatile sig_atomic_t gCaughtSignal {};
volatile sig_atomic_t gCaughtCode {};
volatile sig_atomic_t gCaughtBaseRegister {-1};
std::atomic<std::uintptr_t> gCaughtAddress {};
std::atomic<std::uintptr_t> gCaughtPc {};
std::atomic<std::uint32_t> gCaughtInstruction {};
std::atomic<std::uintptr_t> gCaughtBaseValue {};
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

bool ContextRegisterValue(const ucontext_t* context,
                          std::uint32_t registerIndex,
                          std::uintptr_t* value) noexcept {
    if (context == nullptr || value == nullptr) {
        return false;
    }
    if (registerIndex < 29) {
        *value = context->uc_mcontext->__ss.__x[registerIndex];
        return true;
    }
    switch (registerIndex) {
    case 29:
        *value = context->uc_mcontext->__ss.__fp;
        return true;
    case 30:
        *value = context->uc_mcontext->__ss.__lr;
        return true;
    case 31:
        *value = context->uc_mcontext->__ss.__sp;
        return true;
    default:
        return false;
    }
}

void CaptureSignal(int signal, siginfo_t* info, void* rawContext) noexcept {
    gCaughtSignal = signal;
    gCaughtCode = info != nullptr ? info->si_code : 0;
    gCaughtAddress.store(info != nullptr
        ? reinterpret_cast<std::uintptr_t>(info->si_addr)
        : 0, std::memory_order_relaxed);
    if (rawContext != nullptr) {
        const auto* context = static_cast<const ucontext_t*>(rawContext);
        const std::uintptr_t pc = context->uc_mcontext->__ss.__pc;
        gCaughtPc.store(pc, std::memory_order_relaxed);
        if (pc % alignof(std::uint32_t) == 0) {
            const std::uint32_t instruction =
                *reinterpret_cast<const volatile std::uint32_t*>(pc);
            gCaughtInstruction.store(instruction, std::memory_order_relaxed);
            const auto decoded =
                Lumen::MacOS::FexCore::
                    DecodeStoreZeroUnsignedImmediate(instruction);
            std::uintptr_t baseValue = 0;
            if (decoded.valid && ContextRegisterValue(
                    context, decoded.baseRegister, &baseValue)) {
                gCaughtBaseRegister =
                    static_cast<sig_atomic_t>(decoded.baseRegister);
                gCaughtBaseValue.store(
                    baseValue, std::memory_order_relaxed);
            }
        }
    }
    siglongjmp(gSignalReturn, 1);
}

[[gnu::noinline]] void TriggerNativeDoorbellStore(void* state) noexcept {
    auto* const address = static_cast<std::byte*>(state) +
        kNativeDoorbellOffset;
    auto& storage = *reinterpret_cast<std::uint64_t*>(address);
    std::atomic_ref<std::uint64_t>(storage).store(
        0, std::memory_order_relaxed);
}

[[gnu::noinline]] void TriggerInterruptDoorbellStore(
    std::uintptr_t stateAddress) noexcept {
    asm volatile(
        "mov x28, %0\n"
        ".inst %c1\n"
        :
        : "r"(stateAddress), "i"(kInterruptDoorbellStore)
        : "x28", "memory");
}

bool Expect(bool condition, const char* message) noexcept {
    if (!condition) {
        std::fprintf(stderr,
                     "MacOSFexCoreSignalPolicyTest: FAIL: %s\n", message);
    }
    return condition;
}

bool TestPolicyMatrix() noexcept {
    using Lumen::MacOS::FexCore::DecodeStoreZeroUnsignedImmediate;
    using Lumen::MacOS::FexCore::EncodeStoreZeroUnsignedImmediate;
    using Lumen::MacOS::FexCore::ShouldHandleDoorbellFault;
    using Lumen::MacOS::FexCore::StoreZeroTargetsAddress;

    bool passed = true;
    passed &= Expect(ShouldHandleDoorbellFault(
        true, true, false, true),
        "the established non-JIT protection spelling must remain accepted");
    passed &= Expect(ShouldHandleDoorbellFault(
        true, false, true, true),
        "the exact generated BUS_ADRALN doorbell store must be accepted");
    passed &= Expect(!ShouldHandleDoorbellFault(
        false, true, false, true),
        "an established protection fault at another address must fail closed");
    passed &= Expect(!ShouldHandleDoorbellFault(
        true, true, false, false),
        "an exact legacy-code fault without a doorbell store must fail closed");
    passed &= Expect(!ShouldHandleDoorbellFault(
        false, false, true, true),
        "BUS_ADRALN at a different address must fail closed");
    passed &= Expect(!ShouldHandleDoorbellFault(
        true, false, true, false),
        "BUS_ADRALN without a doorbell store must fail closed");
    passed &= Expect(!ShouldHandleDoorbellFault(true, false, false, true),
        "an unrelated signal code must fail closed");

    constexpr std::uintptr_t kThreadBase =
        static_cast<std::uintptr_t>(UINT64_C(0x100000000));
    constexpr std::uintptr_t kDoorbell =
        kThreadBase + kNativeDoorbellOffset;
    constexpr std::uint32_t kNativeStore =
        EncodeStoreZeroUnsignedImmediate(8, kNativeDoorbellOffset);
    const auto decodedNativeStore =
        DecodeStoreZeroUnsignedImmediate(kNativeStore);
    passed &= Expect(decodedNativeStore.valid &&
                     decodedNativeStore.baseRegister == 8 &&
                     decodedNativeStore.byteOffset == kNativeDoorbellOffset,
                     "the native x8-relative store must decode exactly");
    passed &= Expect(StoreZeroTargetsAddress(
        kNativeStore, kThreadBase, kDoorbell),
        "the decoded native store must resolve the doorbell address");
    passed &= Expect(!StoreZeroTargetsAddress(
        kNativeStore, kThreadBase, kDoorbell + sizeof(std::uint64_t)),
        "a decoded store targeting another address must fail closed");
    passed &= Expect(!DecodeStoreZeroUnsignedImmediate(
        kNativeStore & ~UINT32_C(0x1f)).valid,
        "a store from a non-zero register must fail closed");
    passed &= Expect(!DecodeStoreZeroUnsignedImmediate(
        UINT32_C(0xb900001f)).valid,
        "a 32-bit zero store must fail closed");
    passed &= Expect(!DecodeStoreZeroUnsignedImmediate(
        UINT32_C(0xf940001f)).valid,
        "a load must fail closed");
    return passed;
}

bool TestDarwinProtectionSignal(bool nativeStore) noexcept {
    const long rawPageSize = sysconf(_SC_PAGESIZE);
    if (!Expect(rawPageSize > 0, "host page size must be available")) {
        return false;
    }
    const auto pageSize = static_cast<std::size_t>(rawPageSize);
    if (!Expect(pageSize == kNativeDoorbellOffset,
                "the macOS FEX gate requires a 16-KiB host page")) {
        return false;
    }
    const std::size_t mappingSize = nativeStore ? pageSize * 2 : pageSize;
    void* const mapping = mmap(nullptr, mappingSize,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANON, -1, 0);
    if (!Expect(mapping != MAP_FAILED, "doorbell mapping must succeed")) {
        return false;
    }
    void* const doorbell = nativeStore
        ? static_cast<void*>(static_cast<std::byte*>(mapping) + pageSize)
        : mapping;
    *static_cast<std::uint64_t*>(doorbell) = UINT64_C(1);

    gCaughtSignal = 0;
    gCaughtCode = 0;
    gCaughtBaseRegister = -1;
    gCaughtAddress.store(0, std::memory_order_relaxed);
    gCaughtPc.store(0, std::memory_order_relaxed);
    gCaughtInstruction.store(0, std::memory_order_relaxed);
    gCaughtBaseValue.store(0, std::memory_order_relaxed);

    struct sigaction action {};
    struct sigaction oldBus {};
    struct sigaction oldSegv {};
    action.sa_sigaction = CaptureSignal;
    action.sa_flags = SA_SIGINFO;

    bool busInstalled = false;
    bool segvInstalled = false;
    bool pageProtected = false;
    bool storeReturned = false;
    bool passed = true;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGBUS, &action, &oldBus) != 0) {
        passed = Expect(false, "SIGBUS capture must install");
    } else {
        busInstalled = true;
    }
    if (passed && sigaction(SIGSEGV, &action, &oldSegv) != 0) {
        passed = Expect(false, "SIGSEGV capture must install");
    } else if (passed) {
        segvInstalled = true;
    }
    if (passed && mprotect(doorbell, pageSize, PROT_READ) != 0) {
        passed = Expect(false, "doorbell page must become read-only");
    } else if (passed) {
        pageProtected = true;
    }

    if (passed && sigsetjmp(gSignalReturn, 1) == 0) {
        if (nativeStore) {
            TriggerNativeDoorbellStore(mapping);
        } else {
            const std::uintptr_t stateAddress =
                reinterpret_cast<std::uintptr_t>(doorbell) -
                kInterruptPageOffset;
            TriggerInterruptDoorbellStore(stateAddress);
        }
        storeReturned = true;
    }

    if (pageProtected &&
        mprotect(doorbell, pageSize, PROT_READ | PROT_WRITE) != 0) {
        passed = Expect(false, "doorbell page protection must restore") &&
            passed;
    }
    if (segvInstalled && sigaction(SIGSEGV, &oldSegv, nullptr) != 0) {
        passed = Expect(false, "SIGSEGV handler must restore") && passed;
    }
    if (busInstalled && sigaction(SIGBUS, &oldBus, nullptr) != 0) {
        passed = Expect(false, "SIGBUS handler must restore") && passed;
    }

    const bool establishedProtectionFault =
        (gCaughtSignal == SIGSEGV && gCaughtCode == SEGV_ACCERR) ||
        (gCaughtSignal == SIGBUS && gCaughtCode == BUS_ADRERR);
    const bool busAlignmentFault =
        gCaughtSignal == SIGBUS && gCaughtCode == BUS_ADRALN;
    const std::uintptr_t caughtAddress =
        gCaughtAddress.load(std::memory_order_relaxed);
    const std::uintptr_t caughtPc =
        gCaughtPc.load(std::memory_order_relaxed);
    const std::uint32_t caughtInstruction =
        gCaughtInstruction.load(std::memory_order_relaxed);
    const std::uintptr_t caughtBaseValue =
        gCaughtBaseValue.load(std::memory_order_relaxed);
    const auto decodedCapturedStore =
        Lumen::MacOS::FexCore::DecodeStoreZeroUnsignedImmediate(
            caughtInstruction);
    const bool storeZeroTargetsDoorbell =
        gCaughtBaseRegister >= 0 &&
        Lumen::MacOS::FexCore::StoreZeroTargetsAddress(
            caughtInstruction, caughtBaseValue,
            reinterpret_cast<std::uintptr_t>(doorbell));
    passed &= Expect(!storeReturned,
                     "the read-only doorbell store must fault");
    passed &= Expect(
        caughtAddress == reinterpret_cast<std::uintptr_t>(doorbell),
                     "Darwin must report the exact doorbell address");
    passed &= Expect(caughtPc % alignof(std::uint32_t) == 0,
                     "the fault PC must be instruction-aligned");
    if (nativeStore) {
        passed &= Expect(decodedCapturedStore.valid,
                         "the native fault PC must contain a zero store");
        passed &= Expect(gCaughtBaseRegister != 28,
                         "the native C++ probe must exercise a non-x28 form");
    } else {
        passed &= Expect(caughtInstruction == kInterruptDoorbellStore,
                         "the JIT-shaped fault PC must contain its store");
        passed &= Expect(gCaughtBaseRegister == 28,
                         "the generated probe must exercise the x28 form");
    }
    passed &= Expect(storeZeroTargetsDoorbell,
                     "the decoded zero store must target the doorbell");
    passed &= Expect(
        Lumen::MacOS::FexCore::ShouldHandleDoorbellFault(
            caughtAddress == reinterpret_cast<std::uintptr_t>(doorbell),
            establishedProtectionFault, busAlignmentFault,
            storeZeroTargetsDoorbell),
        "the observed Darwin protection signal must be admitted");

    if (munmap(mapping, mappingSize) != 0) {
        passed = Expect(false, "doorbell mapping must release") && passed;
    }
    if (passed) {
        std::printf(
            "MacOSFexCoreSignalPolicyTest: PASS signal=%d code=%d "
            "faultExact=1 form=%s opcode=0x%08x base=x%d offset=%zu\n",
            static_cast<int>(gCaughtSignal),
            static_cast<int>(gCaughtCode),
            nativeStore ? "native-cxx" : "jit-x28",
            static_cast<unsigned>(caughtInstruction),
            static_cast<int>(gCaughtBaseRegister),
            decodedCapturedStore.byteOffset);
    }
    return passed;
}

} // namespace

int main() {
    bool passed = TestPolicyMatrix();
    passed &= TestDarwinProtectionSignal(false);
    passed &= TestDarwinProtectionSignal(true);
    return passed ? 0 : 1;
}
