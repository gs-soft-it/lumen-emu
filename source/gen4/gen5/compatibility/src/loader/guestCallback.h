// Lumen modifications, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#ifndef LUMEN_PS5_GUEST_CALLBACK_H_
#define LUMEN_PS5_GUEST_CALLBACK_H_

#include "common/assert.h"
#include "kernel/memory.h"
#if defined(LUMEN_PS5_FEXCORE)
#include "loader/guestExecution.h"
#endif

#include <array>
#include <cstring>
#include <type_traits>
#include <vector>

namespace Loader::GuestCallback {

template <typename T> constexpr uint64_t Scalar(T value) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T> || std::is_pointer_v<T> ||
                  std::is_null_pointer_v<T>, "callback arguments require explicit scalar ABI");
    if constexpr (std::is_null_pointer_v<T>) {
        return 0;
    } else if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<uint64_t>(value);
    } else {
        return static_cast<uint64_t>(value);
    }
}

// Every supplied function is a guest address. Internal native callbacks use
// their separate host entry path, never a guest-address classification fallback.
template <bool WithoutGuestBorrow, typename Function, typename... Args>
auto CallImpl(Function function, Args... args) -> std::invoke_result_t<Function, Args...> {
    using Result = std::invoke_result_t<Function, Args...>;
#if defined(LUMEN_PS5_FEXCORE)
    static_assert(std::is_void_v<Result> || std::is_integral_v<Result> ||
                  std::is_enum_v<Result> || std::is_pointer_v<Result>,
                  "callback result requires explicit scalar ABI");
    const std::array<uint64_t, sizeof...(Args)> scalars {Scalar(args)...};
    uint64_t result = 0;
    const bool called = WithoutGuestBorrow ?
        GuestExecution::InvokeWithoutGuestBorrow(reinterpret_cast<uint64_t>(function),
            scalars.data(), scalars.size(), &result) :
        GuestExecution::Invoke(reinterpret_cast<uint64_t>(function), scalars.data(),
            scalars.size(), &result);
    if (!called) {
        EXIT("PS5 guest callback failed: target=0x%016" PRIx64 "\n",
             reinterpret_cast<uint64_t>(function));
    }
    if constexpr (std::is_pointer_v<Result>) {
        return reinterpret_cast<Result>(result);
    } else if constexpr (std::is_same_v<Result, bool>) {
        return (result & 0xffu) != 0;
    } else if constexpr (!std::is_void_v<Result>) {
        return static_cast<Result>(result);
    }
#else
    return function(args...);
#endif
}

template <typename Function, typename... Args>
auto Call(Function function, Args... args) -> std::invoke_result_t<Function, Args...> {
    return CallImpl<false>(function, args...);
}

// Explicit caller promise: all state needed after this call is independently
// live native data, and no guest/VM lease or local mutex spans the callback.
template <typename Function, typename... Args>
auto CallWithoutGuestBorrow(Function function, Args... args)
    -> std::invoke_result_t<Function, Args...> {
    return CallImpl<true>(function, args...);
}

// Host-owned callback records must not be exposed as native stack/heap pointers
// to translated code. This synchronous buffer stays inside the guest VM owner.
class Buffer {
public:
    explicit Buffer(size_t size, const void* initial = nullptr, bool copy_initial = true)
        : size_(size) {
#if defined(LUMEN_PS5_FEXCORE)
        if (size == 0 || size > UINT64_MAX - 0x3fffu) {
            EXIT("PS5 guest callback record has an invalid size\n");
        }
        mapped_size_ = (size + 0x3fffu) & ~uint64_t {0x3fffu};
        address_ = Libs::LibKernel::Memory::AllocateRuntimeMemory(
            0, mapped_size_, Common::VirtualMemory::Mode::ReadWrite, "guest_callback_record");
        if (address_ == 0) {
            EXIT("PS5 guest callback record allocation failed\n");
        }
        const std::vector<uint8_t> zeroes(initial != nullptr && copy_initial ? 0 : size_, 0);
        const void* source = initial != nullptr && copy_initial ? initial : zeroes.data();
        if (!Libs::LibKernel::Memory::WriteGuestMemory(nullptr, address_, source, size_)) {
            EXIT("PS5 guest callback record initialization failed\n");
        }
#else
        (void)copy_initial;
        address_ = reinterpret_cast<uint64_t>(initial);
#endif
    }
    ~Buffer() {
#if defined(LUMEN_PS5_FEXCORE)
        if (!Libs::LibKernel::Memory::FreeGuestMemory(address_, mapped_size_)) {
            EXIT("PS5 guest callback record release failed\n");
        }
#endif
    }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    template <typename T = void> T* Get() const { return reinterpret_cast<T*>(address_); }
    void CopyBack(void* destination) const {
#if defined(LUMEN_PS5_FEXCORE)
        if (!Libs::LibKernel::Memory::ReadGuestMemory(nullptr, address_, destination, size_)) {
            EXIT("PS5 guest callback record copyback failed\n");
        }
#else
        (void)destination;
#endif
    }

private:
    uint64_t address_ = 0;
    uint64_t mapped_size_ = 0;
    size_t size_;
};

template <typename T> class Object: public Buffer {
public:
    explicit Object(T& initial): Buffer(sizeof(T), &initial) {
        static_assert(std::is_trivially_copyable_v<T>, "guest callback records require plain data");
    }
    T* Get() const { return Buffer::Get<T>(); }
    void CopyBack(T& destination) const { Buffer::CopyBack(&destination); }
};

} // namespace Loader::GuestCallback

#endif
