// Lumen, 2026-10-08. SPDX-License-Identifier: GPL-2.0-only
// Original cache-binding fixture. Link FEXCore without the Android runtime:
// only this executable may vary its synthetic preserving provider.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "Interface/Context/Context.h"
#include "Interface/Core/JIT/Relocations.h"
#include "Interface/Core/JIT/DebugData.h"
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/LumenMemoryHandoffAbi.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/Allocator.h>
#include <rpmalloc/rpmalloc.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include "../../main/cpp/FexTranslationCache.h"

static_assert(FEX_LUMEN_HAS_MEMORY_PRESERVE_ALL_ABI == 1,
    "Android cache fixture requires the configured preserving ABI");

namespace {
bool preservingProviderAvailable;
bool readDirectoryProviderAvailable;
bool progressProviderAvailable;
std::atomic<std::uint32_t> progressPending {};
void GuestProgress() noexcept {}
alignas(64) std::array<std::uint8_t, 1U << 20> inlineGuards {};
std::atomic<const std::uint8_t*> readDirectory {inlineGuards.data()};
std::uint32_t OrdinaryCheck(std::uint64_t, std::uint64_t, std::uint32_t, std::uint64_t) noexcept { return 1; }
FEX_LUMEN_MEMORY_PRESERVE_ALL_ABI std::uint32_t PreservingCheck(
    std::uint64_t, std::uint64_t, std::uint32_t, std::uint64_t) noexcept { return 1; }

class CompileOnlySyscalls final : public FEXCore::HLE::SyscallHandler {
public:
    CompileOnlySyscalls(std::uint64_t base, std::uint64_t bytes) : base_(base), bytes_(bytes) {
        OSABI = FEXCore::HLE::SyscallOSABI::OS_GENERIC;
    }
    std::uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame*, FEXCore::HLE::SyscallArguments*) override { return 0; }
    FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(
        FEXCore::Core::InternalThreadState*, std::uint64_t address) override {
        return address >= base_ && address - base_ < bytes_ ?
            FEXCore::HLE::ExecutableRangeInfo {base_, bytes_, false} : FEXCore::HLE::ExecutableRangeInfo {0, 0, false};
    }
    std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(
        FEXCore::Core::InternalThreadState*, std::uint64_t) override { return std::nullopt; }
private:
    std::uint64_t base_, bytes_;
};

std::uint64_t CompiledFootprint(const FEXCore::HostFeatures& features, std::uint64_t code, std::uint64_t page, bool preserving) {
    preservingProviderAvailable = preserving;
    FEXCore::SignalDelegator signals;
    CompileOnlySyscalls syscalls(code, page);
    auto context = FEXCore::Context::Context::CreateNewContext(features);
    if (!context) return 0;
    auto* impl = static_cast<FEXCore::Context::ContextImpl*>(context.get());
    context->SetSignalDelegator(&signals);
    context->SetSyscallHandler(&syscalls);
    if (!context->InitCore()) return 0;
    auto* thread = context->CreateThread();
    if (!thread) return 0;
    auto& state = thread->CurrentFrame->State;
    state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = state.private_gdt;
    state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = state.private_gdt;
    state.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
    auto* segment = FEXCore::Core::CPUState::GetSegmentFromIndex(state, state.cs_idx);
    FEXCore::Core::CPUState::SetGDTBase(segment, 0);
    FEXCore::Core::CPUState::SetGDTLimit(segment, 0xf'ffffU);
    segment->L = 1; segment->D = 0;
    state.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*segment);
    FEXCore::Allocator::RegisterTLSData(thread);
    const auto compiled = impl->CompileCode(thread, code);
    const auto bytes = compiled.CompiledCode.BlockBegin != nullptr ? compiled.CompiledCode.Size : 0;
    FEXCore::Allocator::UninstallTLSData(thread);
    context->DestroyThread(thread);
    return bytes;
}

bool FrontendNamedSymbolContract() {
    using Symbol = FEXCore::CPU::RelocNamedSymbolLiteral::NamedSymbol;
    static_assert(static_cast<std::uint32_t>(Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_BULK) == 3);
    static_assert(static_cast<std::uint32_t>(Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_CHECK_PRESERVE_ALL) == 4);
    static_assert(static_cast<std::uint32_t>(Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_READ_GUARD_DIRECTORY) == 5);
    static_assert(static_cast<std::uint32_t>(Symbol::SYMBOL_LITERAL_LUMEN_GUEST_PROGRESS_FLAG) == 6);
    static_assert(static_cast<std::uint32_t>(Symbol::SYMBOL_LITERAL_LUMEN_GUEST_PROGRESS_CALLBACK) == 7);
    // A structurally valid one-byte RET blob with both its mandatory tail-RIP
    // relocation and an independent named literal. No native code is executed.
    std::vector<unsigned char> blob(40 + 80, 0);
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
    bool valid = true;
    for (std::uint32_t symbol = 0; symbol != 9; ++symbol) {
        put(symbolOffset, symbol);
        valid &= Lumen::FexCore::TranslationCache::ValidateBlob(blob) == (symbol <= 7);
    }
    std::printf("%s frontend_named_symbol_whitelist accepted=0..7 rejected=8 actual_blob_validator=1\n",
        valid ? "PASS" : "FAIL");
    return valid;
}

bool FootprintContract(const FEXCore::HostFeatures& features) {
    const auto page = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    auto* code = static_cast<std::uint8_t*>(mmap(nullptr, page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (code == MAP_FAILED) return false;
    // Original scalar/SSE instructions, compiled twice but never executed.
    // Both variants query exactly this owned read-only guest page.
    std::vector<std::uint8_t> program {0x66, 0x0f, 0xef, 0xc0};
    const std::array<std::uint8_t, 14> accesses {
        0x48, 0x8b, 0x07, 0xf3, 0x0f, 0x6f, 0x0e,
        0x48, 0x89, 0x07, 0xf3, 0x0f, 0x7f, 0x06};
    for (unsigned repeat = 0; repeat != 24; ++repeat)
        program.insert(program.end(), accesses.begin(), accesses.end());
    program.push_back(0xc3);
    if (program.size() > page) { munmap(code, page); return false; }
    std::memcpy(code, program.data(), program.size());
    if (mprotect(code, page, PROT_READ) != 0) { munmap(code, page); return false; }
    const auto base = reinterpret_cast<std::uintptr_t>(code);
    const auto ordinary = CompiledFootprint(features, base, page, false);
    const auto preserving = CompiledFootprint(features, base, page, true);
    munmap(code, page);
    const bool success = preserving != 0 && preserving < ordinary;
    std::printf("%s preserve_all_emitted_footprint ordinary_bytes=%llu preserving_bytes=%llu"
        " same_program=1 inline_guards=1 guest_execution=0 fps_attribution=0\n", success ? "PASS" : "FAIL",
        static_cast<unsigned long long>(ordinary), static_cast<unsigned long long>(preserving));
    return success;
}
}

extern "C" FEXLumenMemoryCheckV1 FEXLumenGuestMemoryCheckProviderV1() noexcept { return OrdinaryCheck; }
extern "C" const std::uint8_t* FEXLumenGuestMemoryHandoffGuardProviderV1() noexcept { return inlineGuards.data(); }
extern "C" FEXLumenMemoryPreserveAllCheckV1 FEXLumenGuestMemoryPreserveAllProviderV1() noexcept {
    return preservingProviderAvailable ? PreservingCheck : nullptr;
}
extern "C" FEXLumenMemoryReadGuardDirectoryV1 FEXLumenGuestMemoryReadGuardProviderV1() noexcept {
    return readDirectoryProviderAvailable ? &readDirectory : nullptr;
}

extern "C" const std::uint32_t* FEXLumenGuestProgressFlagProviderV1() noexcept {
    return progressProviderAvailable ? reinterpret_cast<const std::uint32_t*>(&progressPending) : nullptr;
}
extern "C" auto FEXLumenGuestProgressCallbackProviderV1() noexcept -> void (*)() noexcept {
    return progressProviderAvailable ? GuestProgress : nullptr;
}

int main() {
    if (!FEX_LUMEN_HAS_MEMORY_PRESERVE_ALL_ABI || rpmalloc_initialize(nullptr) != 0) return 1;
    FEXCore::Allocator::InitializeThread();
    FEXCore::Config::Initialize();
    FEXCore::Config::Load();
    FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLETELEMETRY, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_MULTIBLOCK, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_MAXINST, "5000");
    FEXCore::HostFeatures features {};
    features.DCacheLineLog2 = 4;
    features.HostType = FEXCore::HostFeatures::HostTypeEnum::Linux;
    features.CPUMIDRs.push_back(0);
    auto context = FEXCore::Context::Context::CreateNewContext(features);
    if (!context) return 2;
    auto& cache = static_cast<FEXCore::Context::ContextImpl*>(context.get())->CodeCache;
    using Symbol = FEXCore::CPU::RelocNamedSymbolLiteral::NamedSymbol;
    unsigned passed {};
    for (const bool packed : {false, true}) {
        for (unsigned scenario = 0; scenario != 5; ++scenario) {
            preservingProviderAvailable = scenario == 0;
            const bool storage = scenario == 2, ordinary = scenario == 3;
            const auto symbol = ordinary ? Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_CHECK :
                Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_CHECK_PRESERVE_ALL;
            alignas(8) std::array<std::byte, 32> bytes;
            bytes.fill(std::byte {0x5a});
            const auto before = bytes;
            auto raw = FEXCore::CPU::Relocation::Default();
            raw.Header.Type = FEXCore::CPU::RelocationTypes::RELOC_NAMED_SYMBOL_LITERAL;
            raw.Header.Offset = 8;
            raw.NamedSymbolLiteral.Symbol = symbol;
            FEXCore::DiskCache::BlobSmallRelocation small {};
            small.Offset = 8;
            small.Type = static_cast<std::uint8_t>(raw.Header.Type);
            small.Named.Symbol = static_cast<std::uint32_t>(symbol);
            std::array<FEXCore::CPU::Relocation, 2> rawRelocations {raw, raw};
            std::array<FEXCore::DiskCache::BlobSmallRelocation, 2> packedRelocations {small, small};
            const bool preceding = scenario == 4;
            if (preceding) {
                rawRelocations[0].Header.Offset = 0;
                rawRelocations[0].NamedSymbolLiteral.Symbol = Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_CHECK;
                packedRelocations[0].Offset = 0;
                packedRelocations[0].Named.Symbol = static_cast<std::uint32_t>(Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_CHECK);
            }
            const auto count = preceding ? 2U : 1U;
            const bool applied = packed ? cache.ApplyPackedCodeRelocations(0, bytes, {packedRelocations.data(), count}, {}, storage) :
                cache.ApplyCodeRelocations(0, bytes, {rawRelocations.data(), count}, storage);
            std::uint64_t literal {};
            std::memcpy(&literal, bytes.data() + 8, sizeof(literal));
            const auto expected = storage ? 0 : ordinary ? reinterpret_cast<std::uintptr_t>(OrdinaryCheck) :
                reinterpret_cast<std::uintptr_t>(PreservingCheck);
            const bool outsideUntouched = std::equal(bytes.begin(), bytes.begin() + 8, before.begin()) &&
                std::equal(bytes.begin() + 16, bytes.end(), before.begin() + 16);
            const bool denied = scenario == 1 || preceding;
            std::uint64_t precedingLiteral {};
            std::memcpy(&precedingLiteral, bytes.data(), 8);
            const bool deniedCanary = preceding ?
                precedingLiteral == reinterpret_cast<std::uintptr_t>(OrdinaryCheck) &&
                    std::equal(bytes.begin() + 8, bytes.end(), before.begin() + 8) : bytes == before;
            const bool success = denied ? !applied && deniedCanary : applied && literal == expected && outsideUntouched;
            std::printf("%s preserve_all_cache format=%s scenario=%s exact_binding=%u canary=%u\n",
                success ? "PASS" : "FAIL", packed ? "packed" : "regular",
                scenario == 0 ? "present" : scenario == 1 ? "absent_denied" : storage ? "storage_zero" :
                    preceding ? "preceding_literal_candidate_denied" : "ordinary_fallback",
                success ? 1U : 0U, (denied ? deniedCanary : outsideUntouched) ? 1U : 0U);
            passed += success ? 1U : 0U;
        }
    }
    for (const bool packed : {false, true}) {
        for (unsigned scenario = 0; scenario != 5; ++scenario) {
            const bool storage = scenario == 3, preceding = scenario == 4;
            readDirectoryProviderAvailable = scenario < 2;
            readDirectory.store(scenario == 1 ? nullptr : inlineGuards.data(), std::memory_order_release);
            alignas(8) std::array<std::byte, 32> bytes;
            bytes.fill(std::byte {0x6b});
            const auto before = bytes;
            auto raw = FEXCore::CPU::Relocation::Default();
            raw.Header.Type = FEXCore::CPU::RelocationTypes::RELOC_NAMED_SYMBOL_LITERAL;
            raw.Header.Offset = 8;
            raw.NamedSymbolLiteral.Symbol = Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_READ_GUARD_DIRECTORY;
            FEXCore::DiskCache::BlobSmallRelocation small {};
            small.Offset = 8;
            small.Type = static_cast<std::uint8_t>(raw.Header.Type);
            small.Named.Symbol = static_cast<std::uint32_t>(raw.NamedSymbolLiteral.Symbol);
            std::array<FEXCore::CPU::Relocation, 2> rawRelocations {raw, raw};
            std::array<FEXCore::DiskCache::BlobSmallRelocation, 2> packedRelocations {small, small};
            if (preceding) {
                rawRelocations[0].Header.Offset = 0;
                rawRelocations[0].NamedSymbolLiteral.Symbol = Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_CHECK;
                packedRelocations[0].Offset = 0;
                packedRelocations[0].Named.Symbol = static_cast<std::uint32_t>(Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_CHECK);
            }
            const auto count = preceding ? 2U : 1U;
            const bool applied = packed ? cache.ApplyPackedCodeRelocations(0, bytes, {packedRelocations.data(), count}, {}, storage) :
                cache.ApplyCodeRelocations(0, bytes, {rawRelocations.data(), count}, storage);
            std::uint64_t literal {}, precedingLiteral {};
            std::memcpy(&literal, bytes.data() + 8, sizeof(literal));
            std::memcpy(&precedingLiteral, bytes.data(), sizeof(precedingLiteral));
            const bool outsideUntouched = std::equal(bytes.begin(), bytes.begin() + 8, before.begin()) &&
                std::equal(bytes.begin() + 16, bytes.end(), before.begin() + 16);
            const bool denied = scenario == 2 || preceding;
            const bool deniedCanary = preceding ? precedingLiteral == reinterpret_cast<std::uintptr_t>(OrdinaryCheck) &&
                std::equal(bytes.begin() + 8, bytes.end(), before.begin() + 8) : bytes == before;
            const bool success = denied ? !applied && deniedCanary : applied && outsideUntouched &&
                literal == (storage ? 0 : reinterpret_cast<std::uintptr_t>(&readDirectory));
            std::printf("%s read_directory_cache format=%s scenario=%s directory_binding=1\n", success ? "PASS" : "FAIL",
                packed ? "packed" : "regular", scenario == 0 ? "dirty" : scenario == 1 ? "clean_null_table" :
                scenario == 2 ? "absent_denied" : storage ? "storage_zero" : "preceding_literal_candidate_denied");
            passed += success ? 1U : 0U;
        }
    }
    for (const bool packed : {false, true}) {
        for (const auto symbol : {Symbol::SYMBOL_LITERAL_LUMEN_GUEST_PROGRESS_FLAG,
                                 Symbol::SYMBOL_LITERAL_LUMEN_GUEST_PROGRESS_CALLBACK}) {
            for (unsigned scenario = 0; scenario != 5; ++scenario) {
                const bool storage = scenario == 3, preceding = scenario == 4;
                progressProviderAvailable = scenario < 2;
                progressPending.store(scenario == 1 ? 0U : 1U, std::memory_order_release);
                alignas(8) std::array<std::byte, 32> bytes;
                bytes.fill(std::byte {0x7c});
                const auto before = bytes;
                auto raw = FEXCore::CPU::Relocation::Default();
                raw.Header.Type = FEXCore::CPU::RelocationTypes::RELOC_NAMED_SYMBOL_LITERAL;
                raw.Header.Offset = 8;
                raw.NamedSymbolLiteral.Symbol = symbol;
                FEXCore::DiskCache::BlobSmallRelocation small {};
                small.Offset = 8;
                small.Type = static_cast<std::uint8_t>(raw.Header.Type);
                small.Named.Symbol = static_cast<std::uint32_t>(symbol);
                std::array<FEXCore::CPU::Relocation, 2> rawRelocations {raw, raw};
                std::array<FEXCore::DiskCache::BlobSmallRelocation, 2> packedRelocations {small, small};
                if (preceding) {
                    rawRelocations[0].Header.Offset = 0;
                    rawRelocations[0].NamedSymbolLiteral.Symbol = Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_CHECK;
                    packedRelocations[0].Offset = 0;
                    packedRelocations[0].Named.Symbol = static_cast<std::uint32_t>(Symbol::SYMBOL_LITERAL_LUMEN_MEMORY_HANDOFF_CHECK);
                }
                const auto count = preceding ? 2U : 1U;
                const bool applied = packed ? cache.ApplyPackedCodeRelocations(0, bytes, {packedRelocations.data(), count}, {}, storage) :
                    cache.ApplyCodeRelocations(0, bytes, {rawRelocations.data(), count}, storage);
                std::uint64_t literal {}, precedingLiteral {};
                std::memcpy(&literal, bytes.data() + 8, sizeof(literal));
                std::memcpy(&precedingLiteral, bytes.data(), sizeof(precedingLiteral));
                const bool denied = scenario == 2 || preceding;
                const bool deniedCanary = preceding ? precedingLiteral == reinterpret_cast<std::uintptr_t>(OrdinaryCheck) &&
                    std::equal(bytes.begin() + 8, bytes.end(), before.begin() + 8) : bytes == before;
                const bool outsideUntouched = std::equal(bytes.begin(), bytes.begin() + 8, before.begin()) &&
                    std::equal(bytes.begin() + 16, bytes.end(), before.begin() + 16);
                const auto binding = symbol == Symbol::SYMBOL_LITERAL_LUMEN_GUEST_PROGRESS_FLAG ?
                    reinterpret_cast<std::uintptr_t>(&progressPending) : reinterpret_cast<std::uintptr_t>(GuestProgress);
                const bool success = denied ? !applied && deniedCanary : applied && outsideUntouched &&
                    literal == (storage ? 0 : binding);
                std::printf("%s guest_progress_cache format=%s symbol=%u scenario=%s exact_binding=1\n",
                    success ? "PASS" : "FAIL", packed ? "packed" : "regular", static_cast<unsigned>(symbol),
                    scenario == 0 ? "pending" : scenario == 1 ? "clear_flag" : scenario == 2 ? "absent_denied" :
                    storage ? "storage_zero" : "preceding_literal_candidate_denied");
                passed += success ? 1U : 0U;
            }
        }
    }
    passed += FrontendNamedSymbolContract() ? 1U : 0U;
    context.reset();
    readDirectoryProviderAvailable = true;
    passed += FootprintContract(features) ? 1U : 0U;
    FEXCore::Config::Shutdown();
    std::printf("FEX_PRESERVE_ALL_CACHE cases=%u/42 actual_relocation_methods=1 guest_execution=0\n", passed);
    return passed == 42 ? 0 : 3;
}
