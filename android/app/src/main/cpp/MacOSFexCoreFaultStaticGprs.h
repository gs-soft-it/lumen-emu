#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace Lumen::MacOS::FexCore {

struct RawStaticGprSnapshot {
    bool valid {};
    std::array<std::uint64_t, 16> values {};
};

template <std::size_t Before, std::size_t After>
struct RawHostCodeWindowSnapshot {
    static_assert(Before <= 512 && After <= 128);
    static constexpr std::size_t BeforeWords = Before;
    static constexpr std::size_t AfterWords = After;
    bool faultInstructionValid {};
    std::array<std::uint32_t, Before + After + 1> instructions {};
    std::array<bool, Before + After + 1> available {};
};

inline constexpr std::size_t RawHostCodeRadius = 16;
using RawHostCodeSnapshot = RawHostCodeWindowSnapshot<RawHostCodeRadius, RawHostCodeRadius>;
using RawAndroidHostCodeSnapshot = RawHostCodeWindowSnapshot<256, 16>;

// Read only aligned, complete instruction words owned by the active thread's
// code buffer. A rejected fault PC admits no neighbouring reads. Neighbours
// outside the owner are omitted, and arithmetic never wraps at either end.
template <std::size_t Before, std::size_t After,
          typename IsOwnedAddress, typename ReadInstruction>
RawHostCodeWindowSnapshot<Before, After> CaptureRawHostCodeWindow(std::uint64_t pc,
        IsOwnedAddress&& isOwnedAddress, ReadInstruction&& readInstruction) noexcept {
    RawHostCodeWindowSnapshot<Before, After> snapshot {};
    constexpr auto WordBytes = sizeof(std::uint32_t);
    const auto ownedWord = [&](std::uint64_t address) noexcept {
        if (address > UINT64_MAX - (WordBytes - 1)) return false;
        for (std::size_t byte = 0; byte != WordBytes; ++byte) {
            if (!isOwnedAddress(address + byte)) return false;
        }
        return true;
    };
    if (pc % WordBytes != 0 || !ownedWord(pc)) return snapshot;
    snapshot.faultInstructionValid = true;
    snapshot.instructions[Before] = readInstruction(pc);
    snapshot.available[Before] = true;
    for (std::size_t distance = 1; distance <= Before; ++distance) {
        const auto delta = distance * WordBytes;
        if (pc >= delta && ownedWord(pc - delta)) {
            snapshot.instructions[Before - distance] = readInstruction(pc - delta);
            snapshot.available[Before - distance] = true;
        }
    }
    for (std::size_t distance = 1; distance <= After; ++distance) {
        const auto delta = distance * WordBytes;
        if (pc <= UINT64_MAX - delta && ownedWord(pc + delta)) {
            snapshot.instructions[Before + distance] = readInstruction(pc + delta);
            snapshot.available[Before + distance] = true;
        }
    }
    return snapshot;
}

template <typename IsOwnedAddress, typename ReadInstruction>
RawHostCodeSnapshot CaptureRawHostCode(std::uint64_t pc,
        IsOwnedAddress&& isOwnedAddress, ReadInstruction&& readInstruction) noexcept {
    return CaptureRawHostCodeWindow<RawHostCodeRadius, RawHostCodeRadius>(
        pc, isOwnedAddress, readInstruction);
}

inline constexpr std::size_t RawExtendedHostCodeBefore = 512;
inline constexpr std::size_t RawExtendedHostCodeAfter = 128;
struct RawExtendedHostCodeSnapshot {
    bool faultInstructionValid {};
    std::uint64_t pageFirst {}, pageLast {};
    std::uint16_t before {}, after {}, pageClampedBefore {}, pageClampedAfter {}, ownerOmitted {};
    std::array<std::uint32_t, RawExtendedHostCodeBefore + RawExtendedHostCodeAfter + 1> instructions {};
    std::array<bool, RawExtendedHostCodeBefore + RawExtendedHostCodeAfter + 1> available {};
};

constexpr bool RawExtendedHostCodeEnabled(bool context, bool staticGprs, const char* value) noexcept {
    return context && staticGprs && value != nullptr && value[0] == '1' && value[1] == '\0';
}

// Optional wider observation, never a CFG reconstruction: only complete owned
// words in the fault PC's native host page can be read. Gaps remain explicit.
template <typename IsOwnedAddress, typename ReadInstruction>
RawExtendedHostCodeSnapshot CaptureRawExtendedHostCode(bool enabled, std::uint64_t pc,
        std::uint64_t pageSize, IsOwnedAddress&& isOwnedAddress,
        ReadInstruction&& readInstruction) noexcept {
    RawExtendedHostCodeSnapshot snapshot {};
    constexpr auto WordBytes = sizeof(std::uint32_t);
    if (!enabled || pageSize < WordBytes || (pageSize & (pageSize - 1)) != 0 ||
        pc % WordBytes != 0 || pc > UINT64_MAX - (WordBytes - 1)) return snapshot;
    const auto first = pc & ~(pageSize - 1);
    const auto last = first | (pageSize - 1); // inclusive, including the top page
    const auto ownedWord = [&](std::uint64_t address) noexcept {
        if (address < first || address > last - (WordBytes - 1)) return false;
        for (std::size_t byte = 0; byte != WordBytes; ++byte)
            if (!isOwnedAddress(address + byte)) return false;
        return true;
    };
    if (!ownedWord(pc)) return snapshot; // bad PC admits no neighbouring reads
    snapshot.faultInstructionValid = true;
    snapshot.pageFirst = first; snapshot.pageLast = last;
    const auto before = std::min<std::uint64_t>(RawExtendedHostCodeBefore, (pc - first) / WordBytes);
    const auto after = std::min<std::uint64_t>(RawExtendedHostCodeAfter, (last - pc - (WordBytes - 1)) / WordBytes);
    snapshot.pageClampedBefore = static_cast<std::uint16_t>(RawExtendedHostCodeBefore - before);
    snapshot.pageClampedAfter = static_cast<std::uint16_t>(RawExtendedHostCodeAfter - after);
    snapshot.instructions[RawExtendedHostCodeBefore] = readInstruction(pc);
    snapshot.available[RawExtendedHostCodeBefore] = true;
    for (std::uint64_t distance = 1; distance <= before; ++distance) {
        const auto address = pc - distance * WordBytes;
        if (!ownedWord(address)) { ++snapshot.ownerOmitted; continue; }
        const auto index = RawExtendedHostCodeBefore - distance;
        snapshot.instructions[index] = readInstruction(address);
        snapshot.available[index] = true;
        ++snapshot.before;
    }
    for (std::uint64_t distance = 1; distance <= after; ++distance) {
        const auto address = pc + distance * WordBytes;
        if (!ownedWord(address)) { ++snapshot.ownerOmitted; continue; }
        const auto index = RawExtendedHostCodeBefore + distance;
        snapshot.instructions[index] = readInstruction(address);
        snapshot.available[index] = true;
        ++snapshot.after;
    }
    return snapshot;
}

// This is a raw observation of the runtime's static register bank, not a
// reconstruction of precise architectural state at an arbitrary host PC.
// The admitted backend may coalesce a guest register store into an earlier
// host operation. Keep that distinction even when a synthetic case agrees.
constexpr RawStaticGprSnapshot CaptureRawStaticGprs(
        std::uint16_t count,
        const std::array<std::uint8_t, 16>& mapping,
        const std::array<std::uint64_t, 31>& hostRegisters) noexcept {
    RawStaticGprSnapshot snapshot {};
    if (count != snapshot.values.size()) return snapshot;
    std::uint32_t observed {};
    for (const auto index : mapping) {
        if (index >= hostRegisters.size() ||
            (observed & (UINT32_C(1) << index)) != 0) return snapshot;
        observed |= UINT32_C(1) << index;
    }
    for (std::size_t index = 0; index != snapshot.values.size(); ++index) {
        snapshot.values[index] = hostRegisters[mapping[index]];
    }
    snapshot.valid = true;
    return snapshot;
}

} // namespace Lumen::MacOS::FexCore
