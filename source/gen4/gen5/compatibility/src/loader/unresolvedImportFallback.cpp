// Lumen adapter, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
// The x86 thunk and zero-return behavior derive from KytyPS5
// b3e419ff1101999525fa2d061ada1d102cf788b1 runtimeLinker.cpp (GPL-2.0-only).
#include "loader/unresolvedImportFallback.h"
#include "common/abi.h"
#include "common/assert.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "loader/guestMutationScope.h"
#if defined(LUMEN_PS5_FEXCORE)
#include "loader/hostCall.h"
#endif

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Loader {
namespace {
namespace Memory = Libs::LibKernel::Memory;
namespace Vm = Common::VirtualMemory;
constexpr std::uint64_t page_size = 0x4000;
struct Record {
    std::int32_t owner;
    std::uint64_t patch, thunk, id;
    std::string symbol, program;
    std::atomic<std::uint64_t> calls {0};
};
std::mutex records_mutex;
std::map<std::pair<std::int32_t, std::uint64_t>, std::shared_ptr<Record>> records;
std::map<std::uint64_t, std::shared_ptr<Record>> by_id;
std::uint64_t next_id = 0;

std::uint64_t KYTY_SYSV_ABI UnresolvedFunctionReturnZero(std::uint64_t id) {
    std::shared_ptr<Record> record;
    std::uint64_t count;
    {
        std::lock_guard lock(records_mutex);
        const auto found = by_id.find(id);
        if (found == by_id.end()) EXIT("PS5 invalid or retired import fallback record\n");
        record = found->second;
        count = record->calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 0) EXIT("PS5 import fallback call counter exhausted\n");
    }
    // Keep complete counters per import; bound repeated console output while
    // preserving the first call of every symbol and later growth checkpoints.
    if (count <= 16 || (count & (count - 1)) == 0) {
        std::fprintf(stderr,
            "PS5_IMPORT_FALLBACK_CALLED record=%llu owner=%d symbol=%s calls=%llu return=0 api_implemented=false\n",
            static_cast<unsigned long long>(id), record->owner, record->symbol.c_str(),
            static_cast<unsigned long long>(count));
        std::fflush(stderr);
    }
    return 0;
}
} // namespace

bool InitializeFunctionFallbackHostCall() {
#if defined(LUMEN_PS5_FEXCORE)
    // Register before the first guest Run: the FEX target registry cannot
    // admit a new native target during a retained guest/HLE invocation.
    return RegisterHostFunction<UnresolvedFunctionReturnZero>(
        "upstream-unresolved-function-return-zero");
#else
    return true;
#endif
}

std::uint64_t RegisterFunctionFallback(std::int32_t owner, std::uint64_t patch_address,
    const char* symbol, const char* program, std::uint32_t relocation, bool plt) {
    if (owner < 0 || patch_address == 0 || symbol == nullptr || program == nullptr)
        EXIT("PS5 invalid import fallback registration\n");
    GuestExecution::GuestMutationScope mutation;
    std::lock_guard lock(records_mutex);
    const auto key = std::make_pair(owner, patch_address);
    if (const auto existing = records.find(key); existing != records.end()) {
        if (existing->second->symbol != symbol)
            EXIT("PS5 import fallback relocation changed identity\n");
        return existing->second->thunk;
    }
#if defined(LUMEN_PS5_FEXCORE)
    if (!InitializeFunctionFallbackHostCall())
        EXIT("PS5 typed import fallback registration failed\n");
#endif
    if (next_id == UINT64_MAX) EXIT("PS5 import fallback record ID exhausted\n");
    const auto id = ++next_id;
    const auto address = Memory::AllocateRuntimeMemory(0, page_size, Vm::Mode::ReadWrite,
                                                       "upstream-function-fallback");
    if (address == 0) EXIT("PS5 import fallback allocation failed\n");
    // SysV stack alignment and RAX/XMM0 match the upstream thunk. The native
    // target is an explicitly registered HLE function when running through FEX.
    std::array<std::uint8_t, 34> bytes {
        0x48,0x83,0xec,0x08, 0x48,0xbf,0,0,0,0,0,0,0,0,
        0x48,0xb8,0,0,0,0,0,0,0,0, 0xff,0xd0,
        0x48,0x83,0xc4,0x08, 0x0f,0x57,0xc0, 0xc3
    };
    const auto target = reinterpret_cast<std::uint64_t>(UnresolvedFunctionReturnZero);
    std::memcpy(bytes.data() + 6, &id, 8);
    std::memcpy(bytes.data() + 16, &target, 8);
    const bool written = Memory::WriteGuestMemory(nullptr, address, bytes.data(), bytes.size());
    if (!written || !Memory::ProtectGuestMemory(address, page_size, Vm::Mode::ExecuteRead))
        EXIT("PS5 import fallback code publication failed\n");
    Vm::FlushInstructionCache(address, bytes.size());
    auto record = std::make_shared<Record>();
    record->owner = owner; record->patch = patch_address; record->thunk = address;
    record->id = id; record->symbol = symbol; record->program = program;
    records.emplace(key, record);
    by_id.emplace(id, record);
    std::fprintf(stderr,
        "PS5_IMPORT_FALLBACK_REGISTERED record=%llu owner=%d symbol=%s relocation=%u table=%s thunk=%#llx api_implemented=false\n",
        static_cast<unsigned long long>(id), owner, symbol, relocation, plt ? "PLT" : "RELA",
        static_cast<unsigned long long>(address));
    return address;
}

void ReleaseFunctionFallbacks(std::int32_t owner) {
    GuestExecution::GuestMutationScope mutation;
    std::vector<std::shared_ptr<Record>> retired;
    {
        std::lock_guard lock(records_mutex);
        for (auto it = records.begin(); it != records.end();) {
            if (it->first.first != owner) { ++it; continue; }
            retired.push_back(it->second);
            by_id.erase(it->second->id);
            it = records.erase(it);
        }
    }
    for (const auto& record: retired) {
        std::fprintf(stderr,
            "PS5_IMPORT_FALLBACK_RETIRED record=%llu owner=%d symbol=%s calls=%llu\n",
            static_cast<unsigned long long>(record->id), owner, record->symbol.c_str(),
            static_cast<unsigned long long>(record->calls.load(std::memory_order_relaxed)));
#if defined(LUMEN_PS5_FEXCORE)
        if (!Memory::FreeGuestMemory(record->thunk, page_size))
            EXIT("PS5 import fallback release failed\n");
#else
        // Native guest calls do not yet have a quiescence epoch. A helper
        // already in flight must return to RX code that still exists. Retire
        // the callable record (later calls fail closed), but retain its VM
        // backing until process teardown instead of freeing a live return PC.
        std::fprintf(stderr,
            "PS5_IMPORT_FALLBACK_STORAGE_RETAINED record=%llu owner=%d thunk=%#llx reason=native-quiescence-unqualified\n",
            static_cast<unsigned long long>(record->id), owner,
            static_cast<unsigned long long>(record->thunk));
#endif
    }
}

std::uint64_t FunctionFallbackCallCount(std::int32_t owner, std::uint64_t patch_address) {
    std::lock_guard lock(records_mutex);
    const auto found = records.find({owner, patch_address});
    return found == records.end() ? 0 : found->second->calls.load(std::memory_order_relaxed);
}
} // namespace Loader
