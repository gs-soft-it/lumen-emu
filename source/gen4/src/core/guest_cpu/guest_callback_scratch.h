// SPDX-License-Identifier: MIT
#pragma once
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include <cstring>
#include <type_traits>
#include "common/alignment.h"
#include "common/assert.h"
#include "core/fex/guest_memory_scope.h"
#include "core/memory.h"

namespace Core::GuestCpu {
// Callers must hold guest admission or a publication while checking/using the
// pointer. VM metadata, rather than a numeric guest-address window, is authoritative.
inline bool IsMappedCallbackMemory(const void* pointer, std::size_t size, bool writable) {
    const auto address = reinterpret_cast<VAddr>(pointer);
    if (!address || !size || size > UINTPTR_MAX - address) return false;
    const auto required = static_cast<u32>(MemoryProt::CpuRead) |
        (writable ? static_cast<u32>(MemoryProt::CpuWrite) : 0);
    for (auto cursor = address; cursor < address + size;) {
        void* end{}; u32 permissions{};
        if (Memory::Instance()->QueryProtection(cursor, nullptr, &end, &permissions) != 0 ||
            (permissions & required) != required || reinterpret_cast<VAddr>(end) <= cursor) return false;
        cursor = std::min<VAddr>(address + size, reinterpret_cast<VAddr>(end));
    }
    return true;
}

// A synchronous callback borrows this guest mapping. Construction publishes
// initialized bytes and releases the epoch before translated execution begins.
// The callback must not retain Data() after it returns.
template <typename T>
class GuestCallbackScratch final {
    static_assert(std::is_trivially_copyable_v<T>);
    static constexpr u64 MappingSize = Common::AlignUp<u64>(sizeof(T), 16_KB);
public:
    explicit GuestCallbackScratch(const T& input) {
        Fex::MemoryMutationScope publication;
        if (!publication.Valid()) return;
        void* mapped{};
        if (Memory::Instance()->MapMemory(&mapped, 0, MappingSize, MemoryProt::CpuReadWrite,
                MemoryMapFlags::NoCoalesce, VMAType::Flexible, "Guest callback scratch") != 0) return;
        pointer = static_cast<T*>(mapped);
        std::memcpy(pointer, &input, sizeof(T));
        publication.Publish(reinterpret_cast<VAddr>(pointer), MappingSize);
    }
    ~GuestCallbackScratch() {
        if (pointer != nullptr)
            ASSERT_MSG(Memory::Instance()->UnmapMemory(reinterpret_cast<VAddr>(pointer), MappingSize) == 0,
                       "Unable to release guest callback scratch");
    }
    GuestCallbackScratch(const GuestCallbackScratch&) = delete;
    GuestCallbackScratch& operator=(const GuestCallbackScratch&) = delete;
    [[nodiscard]] T* Data() const noexcept { return pointer; }
    [[nodiscard]] bool Read(T& output) const {
        Fex::MemoryMutationScope publication;
        if (!publication.Valid()) return false;
        // Re-check after guest execution, which may have unmapped/protected it.
        if (!IsMappedCallbackMemory(pointer, sizeof(T), false)) return false;
        std::memcpy(&output, pointer, sizeof(T));
        return true;
    }
private:
    T* pointer{};
};
} // namespace Core::GuestCpu
#endif
