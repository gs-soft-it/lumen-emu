// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Lumen::Ps5::DirectFex {

// Records and their veneers are append-only for the lifetime of the process.
// The native target stays private to the bridge; translated SYSCALL uses an ID.
struct HostTargetRecord {
    const std::uint64_t target;
    const std::uint64_t address;
    std::atomic<const void*> binding;

    HostTargetRecord(std::uint64_t target_, std::uint64_t address_, const void* binding_)
        : target(target_), address(address_), binding(binding_) {}

    bool Bind(const void* value) noexcept {
        if (value == nullptr) return true;
        const void* expected = nullptr;
        return binding.compare_exchange_strong(expected, value, std::memory_order_acq_rel,
                                               std::memory_order_acquire) || expected == value;
    }
};

class HostCallTable {
public:
    static constexpr std::size_t Capacity = 65536;

    // The caller fully constructs the immutable record before publishing it.
    // Rejected publication cannot replace an existing operation.
    bool Publish(std::uint64_t operation, const HostTargetRecord* record) noexcept {
        if (operation == 0 || operation >= Capacity || record == nullptr ||
            record->target == 0 || record->address == 0) return false;
        const HostTargetRecord* expected = nullptr;
        return entries[operation].compare_exchange_strong(expected, record,
            std::memory_order_acq_rel, std::memory_order_acquire) || expected == record;
    }

    const HostTargetRecord* Find(std::uint64_t operation) const noexcept {
        if (operation == 0 || operation >= Capacity) return nullptr;
        return entries[operation].load(std::memory_order_acquire);
    }

private:
    std::array<std::atomic<const HostTargetRecord*>, Capacity> entries {};
};

} // namespace Lumen::Ps5::DirectFex
