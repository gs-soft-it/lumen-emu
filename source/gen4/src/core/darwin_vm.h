// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <TargetConditionals.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#include <array>
#include <cstdlib>
#include <limits>
#if !TARGET_OS_IPHONE
#include <mach/mach_vm.h>
#endif

// ARM64 iOS exposes the 64-bit VM map through vm_*; mach_vm.h is a macOS
// header. Keep collision rejection and protection semantics identical.
namespace Lumen::DarwinVM {
#if TARGET_OS_IPHONE
using Address = vm_address_t;
using Size = vm_size_t;
inline constexpr auto Region = vm_region_64;
inline constexpr auto Allocate = vm_allocate;
inline constexpr auto Deallocate = vm_deallocate;
inline constexpr auto Protect = vm_protect;
inline constexpr auto Map = vm_map;
inline constexpr auto Remap = vm_remap;
#else
using Address = mach_vm_address_t;
using Size = mach_vm_size_t;
inline constexpr auto Region = mach_vm_region;
inline constexpr auto Allocate = mach_vm_allocate;
inline constexpr auto Deallocate = mach_vm_deallocate;
inline constexpr auto Protect = mach_vm_protect;
inline constexpr auto Map = mach_vm_map;
inline constexpr auto Remap = mach_vm_remap;
#endif
static_assert(sizeof(Address) == 8 && sizeof(Size) == 8);

inline bool IsPageSpan(Address address, Size size) noexcept {
    const Size page = vm_page_size;
    return page != 0 && (page & (page - 1)) == 0 && size != 0 &&
           ((address | size) & (page - 1)) == 0 &&
           size <= std::numeric_limits<Address>::max() - address;
}

// Process-private backing for guest physical memory. Mach remapping gives
// aliases shared storage without sandbox-restricted POSIX shared-memory names
// or the single-object size limit of an extracted Mach memory entry.
// Guest x86 bytes are data to the native CPU: neither the backing nor an alias
// can gain native execute permission.
class AnonymousBacking final {
public:
    AnonymousBacking() = default;
    AnonymousBacking(const AnonymousBacking&) = delete;
    AnonymousBacking& operator=(const AnonymousBacking&) = delete;
    ~AnonymousBacking() { Release(); }

    kern_return_t Create(Size size) noexcept {
        if (size_ != 0 || !IsPageSpan(0, size))
            return KERN_INVALID_ARGUMENT;
        Address address = 0;
        const auto result = Map(mach_task_self(), &address, size, 0, VM_FLAGS_ANYWHERE,
                                MACH_PORT_NULL, 0, false, ReadWrite, ReadWrite, VM_INHERIT_NONE);
        if (result != KERN_SUCCESS) return result;
        address_ = address;
        size_ = size;
        return KERN_SUCCESS;
    }

    [[nodiscard]] bool IsAvailable() const noexcept {
        return size_ != 0;
    }
    [[nodiscard]] void* Base() const noexcept { return reinterpret_cast<void*>(address_); }

    // The caller must own the entire destination reservation before replacing
    // it. FIXED|OVERWRITE keeps the range reserved throughout the operation.
    kern_return_t MapAliasAt(Address address, Size size, Size offset,
                             vm_prot_t protection) const noexcept {
        if (!IsAvailable() || !IsPageSpan(address, size) || !IsPageSpan(offset, size) ||
            offset > size_ || size > size_ - offset || (protection & ~ReadWrite) != 0)
            return KERN_INVALID_ARGUMENT;
        const auto destination = address;
        vm_prot_t current = VM_PROT_NONE;
        vm_prot_t maximum = VM_PROT_NONE;
        auto result = Remap(mach_task_self(), &address, size, 0,
                            VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                            address_ + offset, false, &current, &maximum, VM_INHERIT_NONE);
        if (result != KERN_SUCCESS) return result;
        if (address != destination) std::abort();
        if (current != ReadWrite || maximum != ReadWrite) {
            result = KERN_PROTECTION_FAILURE;
        } else if (protection != current) {
            result = Protect(mach_task_self(), address, size, false, protection);
        }
        if (result != KERN_SUCCESS) {
            // The replacement already owns this destination: never leave a
            // partially admitted alias accessible or report recoverable failure
            // after destroying an older mapping represented by the caller's ledger.
            ReplaceWithAnonymous(destination, size, VM_PROT_NONE);
            std::abort();
        }
        return result;
    }

    // Also used to restore an owned PROT_NONE placeholder after removing an
    // alias. No deallocate/map gap can expose the address to another allocator.
    static kern_return_t ReplaceWithAnonymous(Address address, Size size,
                                               vm_prot_t protection) noexcept {
        if (!IsPageSpan(address, size) || (protection & ~ReadWrite) != 0)
            return KERN_INVALID_ARGUMENT;
        return Map(mach_task_self(), &address, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                   MACH_PORT_NULL, 0, false, protection, ReadWrite, VM_INHERIT_NONE);
    }

    kern_return_t Release() noexcept {
        kern_return_t result = KERN_SUCCESS;
        if (size_ != 0) {
            const auto released = Deallocate(mach_task_self(), address_, size_);
            if (released == KERN_SUCCESS) {
                address_ = 0;
                size_ = 0;
            }
            if (result == KERN_SUCCESS) result = released;
        }
        return result;
    }

private:
    static constexpr vm_prot_t ReadWrite = VM_PROT_READ | VM_PROT_WRITE;
    Address address_{};
    Size size_{};
};

struct AddressBounds {
    Address begin{};
    Address end{}; // Exclusive, as reported by the current task's VM map.
};

inline kern_return_t QueryAddressBounds(AddressBounds& bounds) noexcept {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_REV2_COUNT;
    const auto result = task_info(mach_task_self(), TASK_VM_INFO,
        reinterpret_cast<task_info_t>(&info), &count);
    if (result != KERN_SUCCESS) return result;
    if (count < TASK_VM_INFO_REV2_COUNT || info.page_size <= 0 ||
        (info.page_size & (info.page_size - 1)) != 0 ||
        info.min_address >= info.max_address ||
        ((info.min_address | info.max_address) & (info.page_size - 1)) != 0) {
        return KERN_INVALID_ARGUMENT;
    }
    bounds = {info.min_address, info.max_address};
    return KERN_SUCCESS;
}

// Preserve native VM capacity while guest placeholders claim free address-space
// gaps. The inventory sees this temporary reservation as host-owned; releasing
// it afterwards leaves room for the full backing alias and native allocations.
class NativeHeadroomReservation final {
public:
    NativeHeadroomReservation() = default;
    NativeHeadroomReservation(const NativeHeadroomReservation&) = delete;
    NativeHeadroomReservation& operator=(const NativeHeadroomReservation&) = delete;
    ~NativeHeadroomReservation() { Release(); }

    kern_return_t ReserveForBacking(Size backing_size, unsigned native_span_count = 1) noexcept {
        if (backing_.size != 0 || !IsPageSpan(0, backing_size) ||
            native_span_count == 0 || native_span_count > native_.size()) {
            return KERN_INVALID_ARGUMENT;
        }

        for (const auto& span : native_) {
            if (span.size != 0) return KERN_INVALID_ARGUMENT;
        }

        const auto backing_result = ReserveSpan(backing_, backing_size);
        if (backing_result != KERN_SUCCESS) return backing_result;

        // The backing needs one contiguous range. Independent native spans
        // can use separate gaps without requiring one much larger hole.
        for (unsigned index = 0; index < native_span_count; ++index) {
            const auto result = ReserveSpan(native_[index], NativeSpanSize);
            if (result != KERN_SUCCESS) {
                Release();
                return result;
            }
        }
        return KERN_SUCCESS;
    }

    kern_return_t ReleaseNativeSpans() noexcept {
        kern_return_t result = KERN_SUCCESS;
        for (auto& span : native_) {
            const auto released = ReleaseSpan(span);
            if (result == KERN_SUCCESS) result = released;
        }
        return result;
    }

    kern_return_t Release() noexcept {
        const auto native_result = ReleaseNativeSpans();
        const auto backing_result = ReleaseSpan(backing_);
        return native_result != KERN_SUCCESS ? native_result : backing_result;
    }

private:
    struct Span {
        Address address{};
        Size size{};
    };

    static kern_return_t ReserveSpan(Span& span, Size size) noexcept {
        Address address = 0;
        const auto result = Map(mach_task_self(), &address, size, 0, VM_FLAGS_ANYWHERE,
                                MACH_PORT_NULL, 0, false, VM_PROT_NONE,
                                VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
        if (result == KERN_SUCCESS) {
            span = {address, size};
        }
        return result;
    }

    static kern_return_t ReleaseSpan(Span& span) noexcept {
        if (span.size == 0) return KERN_SUCCESS;
        const auto result = Deallocate(mach_task_self(), span.address, span.size);
        if (result == KERN_SUCCESS) span = {};
        return result;
    }

    static constexpr Size NativeSpanSize = Size{8} * 1024 * 1024 * 1024;
    Span backing_;
    std::array<Span, 4> native_;
};
}
