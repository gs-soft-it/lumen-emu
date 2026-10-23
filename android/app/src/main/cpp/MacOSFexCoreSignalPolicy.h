#pragma once

#include <cstddef>
#include <cstdint>

namespace Lumen::MacOS::FexCore {

struct StoreZeroUnsignedImmediate {
    bool valid {};
    std::uint32_t baseRegister {};
    std::size_t byteOffset {};
};

constexpr std::uint32_t EncodeStoreZeroUnsignedImmediate(
    std::uint32_t baseRegister, std::size_t byteOffset) noexcept {
    constexpr std::size_t kStoreScale = sizeof(std::uint64_t);
    constexpr std::size_t kMaximumImmediate = UINT32_C(0xfff);
    constexpr std::uint32_t kStoreXUnsignedImmediate = UINT32_C(0xf9000000);
    constexpr std::uint32_t kZeroRegister = 31;

    if (baseRegister > 31 || byteOffset % kStoreScale != 0 ||
        byteOffset / kStoreScale > kMaximumImmediate) {
        return 0;
    }
    return kStoreXUnsignedImmediate |
        (static_cast<std::uint32_t>(byteOffset / kStoreScale) << 10) |
        (baseRegister << 5) | kZeroRegister;
}

constexpr std::uint32_t EncodeInterruptDoorbellStore(
    std::size_t byteOffset) noexcept {
    constexpr std::uint32_t kStateRegister = 28;
    return EncodeStoreZeroUnsignedImmediate(
        kStateRegister, byteOffset);
}

constexpr StoreZeroUnsignedImmediate DecodeStoreZeroUnsignedImmediate(
    std::uint32_t instruction) noexcept {
    constexpr std::uint32_t kOpcodeAndTargetMask = UINT32_C(0xffc0001f);
    constexpr std::uint32_t kStoreXZeroUnsignedImmediate =
        UINT32_C(0xf900001f);
    if ((instruction & kOpcodeAndTargetMask) !=
        kStoreXZeroUnsignedImmediate) {
        return {};
    }
    return {
        true,
        (instruction >> 5) & UINT32_C(0x1f),
        static_cast<std::size_t>(
            (instruction >> 10) & UINT32_C(0xfff)) *
            sizeof(std::uint64_t),
    };
}

constexpr bool StoreZeroTargetsAddress(
    std::uint32_t instruction, std::uintptr_t baseRegisterValue,
    std::uintptr_t targetAddress) noexcept {
    const auto decoded = DecodeStoreZeroUnsignedImmediate(instruction);
    return decoded.valid && targetAddress >= baseRegisterValue &&
        targetAddress - baseRegisterValue == decoded.byteOffset;
}

constexpr bool ShouldHandleDoorbellFault(
    bool exactAddress, bool establishedProtectionFault,
    bool busAlignmentFault, bool storeZeroTargetsDoorbell) noexcept {
    return exactAddress && storeZeroTargetsDoorbell &&
        (establishedProtectionFault || busAlignmentFault);
}

enum class BoundaryPauseReturnDisposition {
    Reject,
    Resume,
    Stop,
};

// The pause stub has already spilled the guest frame. A failed active run
// may stop only at this existing SIGILL return site; no other signal or PC
// becomes a native stop route.
constexpr BoundaryPauseReturnDisposition ClassifyBoundaryPauseReturn(
    bool illegalInstructionSignal, bool inFlight, bool matchingThread,
    std::uint64_t hostPc, std::uint64_t pauseReturnPc,
    bool failedRun) noexcept {
    if (!illegalInstructionSignal || !inFlight || !matchingThread ||
        pauseReturnPc == 0 || hostPc != pauseReturnPc) {
        return BoundaryPauseReturnDisposition::Reject;
    }
    return failedRun ? BoundaryPauseReturnDisposition::Stop
                     : BoundaryPauseReturnDisposition::Resume;
}

} // namespace Lumen::MacOS::FexCore
