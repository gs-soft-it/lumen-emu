// Lumen, 2026-10-09. SPDX-License-Identifier: GPL-2.0-only
// Bindings are varied only in this isolated executable, without the runtime.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#if __has_warning("-Wmissing-designated-field-initializers")
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#endif
#include "Interface/Context/Context.h"
#include "Interface/Core/JIT/Relocations.h"
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/LumenMemoryHandoffAbi.h>
#include <rpmalloc/rpmalloc.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "FexTranslationCache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using Symbol = FEXCore::CPU::RelocNamedSymbolLiteral::NamedSymbol;
bool providerAvailable {true};
bool ordinaryAlwaysAvailable {};
alignas(64) std::array<std::uint8_t, 1U << 20> guards {};
std::atomic<const std::uint8_t*> directory {guards.data()};
std::atomic<std::uint32_t> progressPending {};
void GuestProgress() noexcept {}
std::uint32_t Ordinary(std::uint64_t, std::uint64_t, std::uint32_t, std::uint64_t) noexcept { return 1; }
#if FEX_LUMEN_HAS_MEMORY_PRESERVE_ALL_ABI
FEX_LUMEN_MEMORY_PRESERVE_ALL_ABI std::uint32_t Preserving(
    std::uint64_t, std::uint64_t, std::uint32_t, std::uint64_t) noexcept { return 1; }
#endif
std::uint32_t Bulk(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t) noexcept { return 1; }

std::uint64_t Binding(Symbol symbol) {
    switch (symbol) {
        case Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_GUARDS: return reinterpret_cast<std::uintptr_t>(guards.data());
        case Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_CHECK: return reinterpret_cast<std::uintptr_t>(Ordinary);
        case Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_BULK: return reinterpret_cast<std::uintptr_t>(Bulk);
        case Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_CHECK_PRESERVE_ALL:
#if FEX_LUMEN_HAS_MEMORY_PRESERVE_ALL_ABI
            return reinterpret_cast<std::uintptr_t>(Preserving);
#else
            return 0;
#endif
        case Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_READ_GUARD_DIRECTORY: return reinterpret_cast<std::uintptr_t>(&directory);
        case Symbol::SYMBOL_LITERAL_LUMEN_GUEST_PROGRESS_FLAG: return reinterpret_cast<std::uintptr_t>(&progressPending);
        case Symbol::SYMBOL_LITERAL_LUMEN_GUEST_PROGRESS_CALLBACK: return reinterpret_cast<std::uintptr_t>(GuestProgress);
        default: return 0;
    }
}

bool FrontendSymbols() {
    // A complete blob with a guest-tail RIP and one additional named literal.
    std::vector<unsigned char> blob(120, 0);
    const auto put = [&blob]<typename T>(std::size_t offset, T value) {
        std::memcpy(blob.data() + offset, &value, sizeof(value));
    };
    const auto append = [&blob]<typename T>(T value) {
        const auto offset = blob.size(); blob.resize(offset + sizeof(value));
        std::memcpy(blob.data() + offset, &value, sizeof(value));
    };
    put(0, UINT32_C(1)); put(4, UINT32_C(80)); put(8, UINT32_C(1));
    put(12, UINT32_C(2)); put(20, UINT32_C(1));
    const unsigned char guest = 0xc3;
    const auto hash = XXH3_128bits(&guest, 1);
    std::memcpy(blob.data() + 24, &hash, 16);
    put(40, UINT32_C(16)); put(56, UINT64_C(80)); put(72, UINT64_C(1));
    put(80, UINT32_C(1)); put(84, UINT32_C(48));
    blob[92] = 1; blob[104] = 0x80; blob[105] = 1;
    append(UINT64_C(0)); append(UINT64_C(0)); append(UINT32_C(4));
    append(UINT32_C(24)); blob.push_back(2); append(UINT64_C(0)); blob.push_back(0);
    append(UINT32_C(8)); blob.push_back(0);
    const auto symbolOffset = blob.size();
    append(UINT64_C(0)); blob.push_back(0); blob.push_back(guest);
    for (std::uint32_t symbol = 0; symbol != 9; ++symbol) {
        put(symbolOffset, symbol);
        if (Lumen::FexCore::TranslationCache::ValidateBlob(blob) != (symbol <= 7)) return false;
    }
    return true;
}
} // namespace

extern "C" FEXLumenMemoryCheckV1 FEXLumenGuestMemoryCheckProviderV1() noexcept {
    return providerAvailable || ordinaryAlwaysAvailable ? Ordinary : nullptr;
}
extern "C" const std::uint8_t* FEXLumenGuestMemoryHandoffGuardProviderV1() noexcept {
    return providerAvailable ? guards.data() : nullptr;
}
extern "C" FEXLumenMemoryPreserveAllCheckV1 FEXLumenGuestMemoryPreserveAllProviderV1() noexcept {
#if FEX_LUMEN_HAS_MEMORY_PRESERVE_ALL_ABI
    return providerAvailable ? Preserving : nullptr;
#else
    return nullptr;
#endif
}
using BulkCheck = std::uint32_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t) noexcept;
extern "C" BulkCheck FEXLumenGuestMemoryBulkProviderV1() noexcept { return providerAvailable ? Bulk : nullptr; }
extern "C" FEXLumenMemoryReadGuardDirectoryV1 FEXLumenGuestMemoryReadGuardProviderV1() noexcept {
    return providerAvailable ? &directory : nullptr;
}

extern "C" const std::uint32_t* FEXLumenGuestProgressFlagProviderV1() noexcept {
    return providerAvailable ? reinterpret_cast<const std::uint32_t*>(&progressPending) : nullptr;
}
extern "C" auto FEXLumenGuestProgressCallbackProviderV1() noexcept -> void (*)() noexcept {
    return providerAvailable ? GuestProgress : nullptr;
}

int main() {
    static_assert(static_cast<std::uint32_t>(Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_CHECK) == 2);
    static_assert(static_cast<std::uint32_t>(Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_READ_GUARD_DIRECTORY) == 5);
    if (rpmalloc_initialize(nullptr) != 0) return 1;
    FEXCore::Allocator::InitializeThread();
    FEXCore::Config::Initialize(); FEXCore::Config::Load();
    FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLETELEMETRY, "1");
    FEXCore::HostFeatures features {};
    features.DCacheLineLog2 = 4;
    features.CPUMIDRs.push_back(0);
    auto context = FEXCore::Context::Context::CreateNewContext(features);
    if (!context) return 2;
    auto& cache = static_cast<FEXCore::Context::ContextImpl*>(context.get())->CodeCache;
    unsigned cases {}, passed {};
    for (const bool packed: {false, true}) {
        for (const auto symbol: {Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_GUARDS,
                 Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_CHECK,
                 Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_BULK,
                 Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_CHECK_PRESERVE_ALL,
                 Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_READ_GUARD_DIRECTORY,
                 Symbol::SYMBOL_LITERAL_LUMEN_GUEST_PROGRESS_FLAG,
                 Symbol::SYMBOL_LITERAL_LUMEN_GUEST_PROGRESS_CALLBACK}) {
            for (unsigned scenario = 0; scenario != 4; ++scenario) {
                const bool storage = scenario == 2, clean = scenario == 3;
                providerAvailable = scenario != 1;
                directory.store(clean ? nullptr : guards.data(), std::memory_order_release);
                alignas(8) std::array<std::byte, 32> bytes;
                bytes.fill(std::byte {0x5a}); const auto before = bytes;
                auto raw = FEXCore::CPU::Relocation::Default();
                raw.Header.Type = FEXCore::CPU::RelocationTypes::RELOC_NAMED_SYMBOL_LITERAL;
                raw.Header.Offset = 8; raw.NamedSymbolLiteral.Symbol = symbol;
                FEXCore::DiskCache::BlobSmallRelocation small {};
                small.Offset = 8; small.Type = static_cast<std::uint8_t>(raw.Header.Type);
                small.Named.Symbol = static_cast<std::uint32_t>(symbol);
                const bool applied = packed ? cache.ApplyPackedCodeRelocations(0, bytes, {&small, 1}, {}, storage) :
                    cache.ApplyCodeRelocations(0, bytes, {&raw, 1}, storage);
                std::uint64_t literal {}; std::memcpy(&literal, bytes.data() + 8, 8);
                const auto expected = storage ? 0 : Binding(symbol);
                const bool denied = !storage && (!providerAvailable || expected == 0);
                const bool outside = std::equal(bytes.begin(), bytes.begin() + 8, before.begin()) &&
                    std::equal(bytes.begin() + 16, bytes.end(), before.begin() + 16);
                const bool success = denied ? !applied && bytes == before : applied && literal == expected && outside;
                ++cases; passed += success ? 1U : 0U;
                std::printf("%s macos_handoff_cache format=%s symbol=%u scenario=%s exact_binding=1\n",
                    success ? "PASS" : "FAIL", packed ? "packed" : "regular", static_cast<unsigned>(symbol),
                    scenario == 0 ? "present" : scenario == 1 ? "absent_denied" : storage ? "storage_zero" : "clean_directory");
            }
        }
    }
    for (const bool packed: {false, true}) {
        for (const auto missing: {Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_CHECK_PRESERVE_ALL,
                 Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_READ_GUARD_DIRECTORY,
                 Symbol::SYMBOL_LITERAL_LUMEN_GUEST_PROGRESS_FLAG,
                 Symbol::SYMBOL_LITERAL_LUMEN_GUEST_PROGRESS_CALLBACK}) {
            providerAvailable = false; ordinaryAlwaysAvailable = true;
            alignas(8) std::array<std::byte, 32> bytes;
            bytes.fill(std::byte {0x6b}); const auto before = bytes;
            auto first = FEXCore::CPU::Relocation::Default();
            first.Header.Type = FEXCore::CPU::RelocationTypes::RELOC_NAMED_SYMBOL_LITERAL;
            first.Header.Offset = 0; first.NamedSymbolLiteral.Symbol = Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_CHECK;
            auto second = first; second.Header.Offset = 8; second.NamedSymbolLiteral.Symbol = missing;
            const std::array raw {first, second};
            std::array<FEXCore::DiskCache::BlobSmallRelocation, 2> small {};
            for (unsigned i = 0; i != 2; ++i) {
                small[i].Offset = 8 * i; small[i].Type = static_cast<std::uint8_t>(first.Header.Type);
                small[i].Named.Symbol = static_cast<std::uint32_t>(raw[i].NamedSymbolLiteral.Symbol);
            }
            const bool applied = packed ? cache.ApplyPackedCodeRelocations(0, bytes, small, {}, false) :
                cache.ApplyCodeRelocations(0, bytes, raw, false);
            std::uint64_t literal {}; std::memcpy(&literal, bytes.data(), 8);
            const bool success = !applied && literal == reinterpret_cast<std::uintptr_t>(Ordinary) &&
                std::equal(bytes.begin() + 8, bytes.end(), before.begin() + 8);
            ++cases; passed += success ? 1U : 0U;
            std::printf("%s macos_handoff_cache format=%s missing_symbol=%u preceding_literal_candidate_denied=1\n",
                success ? "PASS" : "FAIL", packed ? "packed" : "regular", static_cast<unsigned>(missing));
        }
    }
    const bool frontend = FrontendSymbols(); ++cases; passed += frontend ? 1U : 0U;
    std::printf("%s macos_handoff_cache frontend_complete_blob_symbols0..7_reject8\n", frontend ? "PASS" : "FAIL");
    context.reset(); FEXCore::Config::Shutdown();
    std::printf("FEX_MACOS_HANDOFF_CACHE cases=%u/%u preserve_all_configured=%u"
        " actual_regular_packed_relocation=1 guest_execution=0\n", passed, cases, FEX_LUMEN_HAS_MEMORY_PRESERVE_ALL_ABI);
    return passed == cases ? 0 : 3;
}
