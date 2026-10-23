// Lumen, 2026-10-10. SPDX-License-Identifier: GPL-2.0-only
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
#include "Interface/Core/Frontend.h"
#include "Interface/IR/IREmitter.h"
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
#include "FexTranslationCache.h"


namespace {
bool enabled;
bool progressAvailable {true};
std::atomic<std::uint32_t> pending {1};
void Progress() noexcept {}
alignas(64) std::array<std::uint8_t, 1U << 20> guards {};
std::atomic<const std::uint8_t*> directory {guards.data()};
std::uint32_t Ordinary(std::uint64_t, std::uint64_t, std::uint32_t, std::uint64_t) noexcept { return 1; }
FEX_LUMEN_MEMORY_PRESERVE_ALL_ABI std::uint32_t Preserving(std::uint64_t, std::uint64_t, std::uint32_t, std::uint64_t) noexcept { return 1; }
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

std::uint64_t CompiledFootprint(const FEXCore::HostFeatures& features, std::uint64_t code, std::uint64_t page, unsigned scenario) {
    FEXCore::Allocator::ScopedJITWriteProtection write;
    enabled = false;
    progressAvailable = scenario != 4;
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
    if (scenario < 3) {
        // These are the three no-work shapes emitted by the real frontend:
        // decoded-empty exit, failed SMC validation, and a target-only stub.
        impl->AddCustomIREntrypoint(code, [&, scenario](std::uintptr_t entry, FEXCore::IR::IREmitter* emit) {
            thread->FrontendDecoder->DecodedMinAddress = entry;
            thread->FrontendDecoder->DecodedMaxAddress = entry + 1;
            auto header = emit->_IRHeader(emit->Invalid(), entry, 0, 0, 0, 0);
            auto block = emit->CreateCodeNode(true, 0);
            header.first->Blocks = emit->WrapNode(block);
            emit->SetCurrentCodeBlock(block);
            if (scenario != 2) emit->_GuestOpcode(0);
            if (scenario == 1) emit->_ThreadRemoveCodeEntry();
            emit->_ExitFunction(FEXCore::IR::OpSize::i64Bit,
                emit->_InlineEntrypointOffset(FEXCore::IR::OpSize::i64Bit, 0),
                FEXCore::IR::BranchHint::None, emit->Invalid(), emit->Invalid(), false);
        });
    }
    const auto compiled = impl->CompileCode(thread, code);
    const auto bytes = compiled.CompiledCode.BlockBegin != nullptr ? compiled.CompiledCode.Size : 0;
    unsigned witnesses = 0;
    for (std::size_t offset = 0; offset + sizeof(std::uintptr_t) <= bytes; ++offset) {
        std::uintptr_t literal {};
        std::memcpy(&literal, compiled.CompiledCode.BlockBegin + offset, sizeof(literal));
        if (literal == reinterpret_cast<std::uintptr_t>(Progress)) ++witnesses;
    }
    const bool expected = scenario == 3;
    const bool pass = bytes != 0 && (witnesses != 0) == expected;
    std::printf("%s guest_progress_emission scenario=%s bytes=%llu witnesses=%u\n",
        pass ? "PASS" : "FAIL", scenario == 0 ? "empty" : scenario == 1 ? "smc_abort" :
        scenario == 2 ? "target_stub" : scenario == 3 ? "retired_guest_instruction" : "exclusive_no_hook",
        static_cast<unsigned long long>(bytes), witnesses);
    FEXCore::Allocator::UninstallTLSData(thread);
    context->DestroyThread(thread);
    return pass ? 1 : 0;
}


}
extern "C" FEXLumenMemoryCheckV1 FEXLumenGuestMemoryCheckProviderV1() noexcept { return enabled ? Ordinary : nullptr; }
extern "C" const std::uint8_t* FEXLumenGuestMemoryHandoffGuardProviderV1() noexcept { return enabled ? guards.data() : nullptr; }
extern "C" FEXLumenMemoryPreserveAllCheckV1 FEXLumenGuestMemoryPreserveAllProviderV1() noexcept { return enabled ? Preserving : nullptr; }
extern "C" FEXLumenMemoryReadGuardDirectoryV1 FEXLumenGuestMemoryReadGuardProviderV1() noexcept { return enabled ? &directory : nullptr; }
using BulkCheck = std::uint32_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t) noexcept;
extern "C" BulkCheck FEXLumenGuestMemoryBulkProviderV1() noexcept { return nullptr; }
extern "C" const std::uint32_t* FEXLumenGuestProgressFlagProviderV1() noexcept {
    return progressAvailable ? reinterpret_cast<const std::uint32_t*>(&pending) : nullptr;
}
extern "C" auto FEXLumenGuestProgressCallbackProviderV1() noexcept -> void (*)() noexcept {
    return progressAvailable ? Progress : nullptr;
}
int main() {
 if (rpmalloc_initialize(nullptr) != 0) return 1;
 FEXCore::Allocator::InitializeThread(); FEXCore::Config::Initialize(); FEXCore::Config::Load();
 FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
 FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLETELEMETRY, "1");
 FEXCore::Config::Set(FEXCore::Config::CONFIG_MULTIBLOCK, "1");
 FEXCore::Config::Set(FEXCore::Config::CONFIG_MAXINST, "5000");
 FEXCore::HostFeatures features {}; features.DCacheLineLog2 = 4; features.CPUMIDRs.push_back(0);
 const auto page = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
 auto* code = static_cast<std::uint8_t*>(mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
 if(code == MAP_FAILED) return 2;
 const std::array<std::uint8_t, 4> program {0x48, 0xff, 0x07, 0xc3};
 std::memcpy(code, program.data(), program.size());
 mprotect(code, page, PROT_READ);
 bool pass = true;
 for (unsigned scenario = 0; scenario != 5; ++scenario)
   pass = CompiledFootprint(features, reinterpret_cast<std::uintptr_t>(code), page, scenario) != 0 && pass;
 munmap(code,page); FEXCore::Config::Shutdown(); return pass ? 0 : 3;
}
