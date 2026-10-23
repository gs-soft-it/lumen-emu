// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include "core/fex/guest_memory_scope.h"
#endif

#include <algorithm>
#include <mutex>
#include <optional>
#include <span>
#include <variant>
#include <vector>
#include <unordered_map>
#include "core/libraries/kernel/threads.h"
#include "core/module.h"
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/guest_cpu.h"
#endif

namespace Core {

struct DynamicModuleInfo;
union DtvEntry;
class Linker;
class MemoryManager;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
class FexGuestCpuBackend;
namespace GuestCpu {
class HleGuestBridge;
class HleVeneerAllocator;
}
// Shared by Linker runtime and the FEX dynamic executable-range callback.
// HLE veneers are host mmap pages not present in the guest VMM.
struct FexExecutableQueryContext final {
    MemoryManager* memory{};
    GuestCpu::HleVeneerAllocator* veneers{};
};
#endif

struct OrbisKernelMemParam {
    u64 size;
    u64* extended_page_table;
    u64* flexible_memory_size;
    u8* extended_memory_1;
    u64* extended_gpu_page_table;
    u8* extended_memory_2;
    u64* extended_cpu_page_table;
};
static_assert(sizeof(OrbisKernelMemParam) == 0x38);

struct OrbisProcParam {
    u64 size;
    u32 magic;
    u32 entry_count;
    u64 sdk_version;
    char* process_name;
    char* main_thread_name;
    u32* main_thread_prio;
    u32* main_thread_stack_size;
    void* libc_param;
    OrbisKernelMemParam* mem_param;
    void* fs_param;
    u32* process_preload_enable;
    u64 unknown1;
};

using ExitFunc = PS4_SYSV_ABI void (*)();

class Linker;

struct EntryParams {
    int argc;
    u32 padding;
    const char* argv[33];
    VAddr entry_addr;
};

struct HeapAPI {
    PS4_SYSV_ABI void* (*heap_malloc)(u64);
    PS4_SYSV_ABI void (*heap_free)(void*);
    PS4_SYSV_ABI void* (*heap_calloc)(u64, u64);
    PS4_SYSV_ABI void* (*heap_realloc)(void*, u64);
    PS4_SYSV_ABI void* (*heap_memalign)(u64, u64);
    PS4_SYSV_ABI s32 (*heap_posix_memalign)(void**, u64, u64);
    PS4_SYSV_ABI s32 (*heap_reallocalign)(void*, u64, u64);
    PS4_SYSV_ABI s32 (*heap_malloc_stats)(void*);
    PS4_SYSV_ABI s32 (*heap_malloc_stats_fast)(void*);
    PS4_SYSV_ABI u64 (*heap_malloc_usable_size)(void*);
};

using AppHeapAPI = HeapAPI*;

class Linker {
public:
    explicit Linker();
    ~Linker();

    Loader::SymbolsResolver& GetHLESymbols() {
        return m_hle_symbols;
    }

    OrbisProcParam* GetProcParam() const {
        return m_modules[0]->GetProcParam<OrbisProcParam*>();
    }

    Module* GetModule(s32 index) const {
        if (index >= 0 && index < m_modules.size()) {
            return m_modules.at(index).get();
        }
        return nullptr;
    }

    u32 FindByName(const std::filesystem::path& name) const {
        for (u32 i = 0; i < m_modules.size(); i++) {
            if (name == m_modules[i]->file) {
                return i;
            }
        }
        return -1;
    }

    u32 MaxTlsIndex() const {
#ifndef LUMEN_GEN4_DIRECT_FEX
        std::scoped_lock lock{mutex};
#endif
        return max_tls_index;
    }

#ifdef LUMEN_GEN4_DIRECT_FEX
    u32 GenerationCounter() const {
        return dtv_generation_counter;
    }
#else
    std::pair<u32, u32> TlsGeneration() const {
        std::scoped_lock lock{mutex};
        return {max_tls_index, dtv_generation_counter};
    }
#endif

    size_t StaticTlsSize() const noexcept {
#ifdef LUMEN_GEN4_DIRECT_FEX
        return static_tls_size;
#else
        return static_tls_layout.size;
#endif
    }

#ifndef LUMEN_GEN4_DIRECT_FEX
    const StaticTlsLayout& GetStaticTlsLayout() const noexcept {
        return static_tls_layout;
    }
#endif

    void RelocateAnyImports(Module* m) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
        Fex::MemoryMutationScope publication;
        ASSERT_MSG(publication.Valid(), "Module relocation admission denied");
#endif
        std::scoped_lock lk{mutex};

        Relocate(m);
        const auto exports = m->GetExportModules();
        for (auto& export_mod : exports) {
            for (auto& module : m_modules) {
                const auto imports = module->GetImportModules();
                if (std::ranges::contains(imports, export_mod.name, &ModuleInfo::name)) {
                    Relocate(module.get());
                }
            }
        }
    }

    void RelocateAllImports() {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
        Fex::MemoryMutationScope publication;
        ASSERT_MSG(publication.Valid(), "Module relocation admission denied");
#endif
        std::scoped_lock lk{mutex};
        for (auto& module : m_modules) {
            Relocate(module.get());
        }
    }

    void SetHeapAPI(void* func[]) {
        heap_api = reinterpret_cast<AppHeapAPI>(func);
    }

    void AdvanceGenerationCounter() noexcept {
#ifndef LUMEN_GEN4_DIRECT_FEX
        std::scoped_lock lock{mutex};
#endif
        dtv_generation_counter++;
    }

    void* TlsGetAddr(u64 module_index, u64 offset);
#ifndef LUMEN_GEN4_DIRECT_FEX
    DtvEntry* AllocateDtvTable(u32 count);
    void FreeDtvTable(DtvEntry* table);
#endif
    void* AllocateTlsForThread(bool is_primary);
#ifndef LUMEN_GEN4_DIRECT_FEX
    void FreeTlsForThread(void* base);
#endif
    void FreeTlsForNonPrimaryThread(void* pointer);

    s32 LoadModule(const std::filesystem::path& elf_name, bool is_dynamic = false);
    s32 LoadAndStartModule(const std::filesystem::path& path, u64 args, const void* argp,
                           int* pRes);
    Module* FindByAddress(VAddr address);

    void Relocate(Module* module);
    bool Resolve(const std::string& name, Loader::SymbolType type, Module* module,
                 Loader::SymbolRecord* return_info);
    void Execute(const std::vector<std::string>& args = {});
    void DebugDump();
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    using GuestFunctionResult = std::variant<u64, GuestExecutionFailure>;
    GuestFunctionResult RunGuestFunction(VAddr entry, std::span<const u64> arguments = {},
                                         VAddr stack_top = 0
#if !defined(LUMEN_GEN4_DIRECT_FEX) || defined(__APPLE__)
                                         , std::span<const std::array<u64, 2>> xmm = {}
#endif
                                         );
    GuestFunctionResult RunGuestMain(EntryParams* params);
#endif

private:
    // Caller owns mutex and, for FEX, the publication epoch.
    s32 LoadModuleLocked(const std::filesystem::path& elf_name, bool is_dynamic);
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    std::optional<GuestExecutionFailure> InitializeFexRuntime();
#endif
    void* CallAppHeapMalloc(u64 size);
#ifdef LUMEN_GEN4_DIRECT_FEX
    void CallAppHeapFree(void* pointer);
#else
    void CallAppHeapFree(void* pointer, decltype(HeapAPI::heap_free) callback = nullptr);
#endif
    MemoryManager* memory;
    Libraries::Kernel::Thread main_thread;
    mutable std::mutex mutex;
    u32 dtv_generation_counter{1};
#ifdef LUMEN_GEN4_DIRECT_FEX
    size_t static_tls_size{};
#else
    StaticTlsLayout static_tls_layout;
    bool static_tls_finalized{};
    std::vector<Module*> static_modules;
    struct TlsAllocation {
        void* original;
        u64 mapping_size;
        decltype(HeapAPI::heap_free) heap_free;
    };
    std::unordered_map<void*, TlsAllocation> tls_allocations;
#endif
    u32 max_tls_index{};
    AppHeapAPI heap_api{};
    std::vector<std::unique_ptr<Module>> m_modules;
    Loader::SymbolsResolver m_hle_symbols{};
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    // Must outlive m_fex_bridge; bridge holds a raw pointer into this for
    // dynamic executable-range queries (guest VMM + late HLE veneers).
    FexExecutableQueryContext m_fex_exec_query{};
    std::unique_ptr<GuestCpu::HleVeneerAllocator> m_hle_veneers;
    std::unique_ptr<GuestCpu::HleGuestBridge> m_fex_bridge;
    std::unique_ptr<FexGuestCpuBackend> m_fex_backend;
    std::mutex m_fex_runtime_mutex;
    VAddr m_fex_exit_veneer{};
#ifdef LUMEN_GEN4_DIRECT_FEX
    VAddr m_fex_fiber_return_veneer{};
#endif
#endif
};

} // namespace Core
