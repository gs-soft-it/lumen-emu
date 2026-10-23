#pragma once

#include <cstdint>

namespace Lumen::MacOS::FexCore {

struct MemoryAccess {
    bool valid {};
    std::uint32_t baseRegister {};
    std::int64_t byteOffset {};
    std::uint32_t width {};
    bool write {};
};

// This deliberately recognizes only the atomic/acquire/release classes
// qualified by original assembler fixtures. An unknown instruction
// must never be sent to a fault-emulation helper on a guessed memory span.
constexpr MemoryAccess DecodeMemoryAccess(std::uint32_t instruction) noexcept {
    const auto size = instruction >> 30;
    const auto base = (instruction >> 5) & 31U;
    const auto scalar = [=](bool write) constexpr {
        return MemoryAccess {true, base, 0, 1U << size, write};
    };

    // LDAR/STLR, including byte and halfword forms. These fixed bits also
    // distinguish them from the less restrictive load/store variants.
    if ((instruction & 0x3ffffc00U) == 0x08dffc00U) return scalar(false);
    if ((instruction & 0x3ffffc00U) == 0x089ffc00U) return scalar(true);

    // LDAPR (RCpc acquire) has no immediate. LDAPUR/STLUR (RCpc2) use
    // an unscaled signed 9-bit byte offset. Signed-extending the field before
    // computing the span is essential for permission checks below the base.
    if ((instruction & 0x3ffffc00U) == 0x38bfc000U) return scalar(false);
    const auto rcpc2Class = instruction & 0x3fe00c00U;
    if (rcpc2Class == 0x19400000U || rcpc2Class == 0x19000000U) {
        const auto rawOffset = static_cast<std::int64_t>((instruction >> 12) & 511U);
        const auto offset = rawOffset >= 256 ? rawOffset - 512 : rawOffset;
        return {true, base, offset, 1U << size, rcpc2Class == 0x19000000U};
    }

    // LDXR/LDAXR and STXR/STLXR: the acquire/release bit and (for stores)
    // status register vary, while the unused second transfer field is fixed.
    if ((instruction & 0x3fff7c00U) == 0x085f7c00U) return scalar(false);
    if ((instruction & 0x3fe07c00U) == 0x08007c00U) return scalar(true);

    // LDXP/LDAXP and STXP/STLXP transfer two 32-bit or two 64-bit values.
    if (size >= 2) {
        if ((instruction & 0x3fff0000U) == 0x087f0000U)
            return {true, base, 0, 2U << size, false};
        if ((instruction & 0x3fe00000U) == 0x08200000U)
            return {true, base, 0, 2U << size, true};
    }

    // CAS{A,L,AL}, and CASP pairs (whose size bit denotes an 8/16-byte span).
    if ((instruction & 0x3fa07c00U) == 0x08a07c00U) return scalar(true);
    if ((instruction & 0xbfa07c00U) == 0x08207c00U &&
        ((instruction >> 16) & 1U) == 0 && (instruction & 1U) == 0)
        return {true, base, 0, 8U << ((instruction >> 30) & 1U), true};

    // LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN/SWP, including
    // acquire/release, byte and halfword variants. Every class writes memory.
    // Other opcodes in the same major group remain unsupported.
    if ((instruction & 0x3f200c00U) == 0x38200000U &&
        ((instruction >> 12) & 15U) <= 8U) return scalar(true);
    return {};
}

// Obtain the complete half-open data span without unsigned wraparound. This
// only computes an address: the caller must still prove guest permissions and
// ownership on every byte before invoking an emulation helper.
constexpr bool ResolveMemoryAccessAddress(const MemoryAccess& access,
                                          std::uintptr_t base,
                                          std::uintptr_t* address) noexcept {
    if (!access.valid || access.width == 0 || address == nullptr) return false;
    constexpr auto maximum = UINTPTR_MAX;
    std::uintptr_t effective {};
    if (access.byteOffset < 0) {
        // This spelling also handles INT64_MIN without signed overflow.
        const auto magnitude = static_cast<std::uint64_t>(-(access.byteOffset + 1)) + 1;
        if (magnitude > base) return false;
        effective = base - static_cast<std::uintptr_t>(magnitude);
    } else {
        const auto offset = static_cast<std::uint64_t>(access.byteOffset);
        if (offset > maximum - base) return false;
        effective = base + static_cast<std::uintptr_t>(offset);
    }
    if (access.width > maximum - effective) return false;
    *address = effective;
    return true;
}

constexpr bool IsDataAlignmentFault(std::uint32_t esr) noexcept {
    // Data abort from EL0, DFSC alignment. Ignore instruction-length and WnR;
    // Darwin's BUS_ADRALN spelling alone also includes permission faults.
    return (esr & 0xfc00003fU) == 0x90000021U;
}

// The FEX scalar acquire/release helper can access aligned surrounding
// granules. Both signal and direct JIT paths must authorize this envelope.
constexpr bool ResolveHelperMemoryEnvelope(std::uintptr_t effective,
    std::uint32_t width, std::uintptr_t* first, std::uintptr_t* length) noexcept {
    constexpr std::uintptr_t granule = 16;
    if (width == 0 || first == nullptr || length == nullptr ||
        effective > UINTPTR_MAX - width ||
        effective + width > UINTPTR_MAX - (granule - 1)) return false;
    const auto begin = effective & ~(granule - 1);
    const auto end = (effective + width + granule - 1) & ~(granule - 1);
    *first = begin;
    *length = end - begin;
    return true;
}

constexpr bool IsDirectUnalignedScalarAccess(std::uintptr_t address,
    std::uint32_t width) noexcept {
    return (width == 2 || width == 4 || width == 8) &&
        (address & 15U) + width > 16;
}

constexpr std::uint32_t DirectUnalignedScalarInstruction(
    std::uint32_t width, bool write) noexcept {
    const auto size = width == 2 ? 1U : width == 4 ? 2U : width == 8 ? 3U : 0U;
    if (size == 0) return 0;
    // x1 is the architectural address; x0 is the transferred value. Use the
    // same acquire/release helper as the original Darwin fault path.
    return (size << 30) | (write ? 0x089ffc00U : 0x08dffc00U) | (1U << 5);
}

constexpr std::uint32_t DirectUnalignedDeniedInstruction =
    0xd4200000U | (0x4c55U << 5); // brk #0x4c55

} // namespace Lumen::MacOS::FexCore
