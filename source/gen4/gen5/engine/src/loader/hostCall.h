// Lumen adapter, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#ifndef LUMEN_PS5_HOST_CALL_H_
#define LUMEN_PS5_HOST_CALL_H_

#include "common/abi.h"
#include "loader/guestExecution.h"

#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Loader {

enum class HostScalar : std::uint8_t { Void, Gp, Float, Double, Custom, Unsupported };
inline constexpr std::size_t HostCallMaxArguments = 32;
using HostScalarInvoker = std::uint64_t (*)(const std::uint64_t*);
using HostVariadicInvoker = std::uint64_t (*)(const std::uint64_t*,
                                            const LumenAndroidFexCoreSimdViewV2*);

using HostCustomInvoker = bool (*)(std::uint64_t*, const LumenAndroidFexCoreSimdViewV2*);

struct HostCallMetadata {
    std::size_t arity {};
    std::array<HostScalar, HostCallMaxArguments> arguments {};
    HostScalar result {HostScalar::Unsupported};
    HostScalarInvoker invoke {};
    HostVariadicInvoker invoke_variadic {};
    HostCustomInvoker invoke_custom {};
};

// Only native functions supplied by typed registration enter this registry.
// Guest export addresses must never be registered here.
bool RegisterHostCall(std::uint64_t target, const HostCallMetadata& metadata,
                      const char* name);
bool IsHostCallTarget(std::uint64_t target) noexcept;
bool IsHostCallSupported(std::uint64_t target) noexcept;
bool HostCallRegistryReady() noexcept;
std::size_t HostCallCount() noexcept;
bool DispatchHostCall(std::uint64_t target, std::uint64_t* gprs,
                      LumenAndroidFexCoreSimdViewV2* simd,
                      GuestExecution::MemoryRead read, void* opaque) noexcept;

// VA_ARGS wrappers receive architectural XMM values and use this checked
// overflow area instead of the ARM64 caller's stack.
std::uint64_t ActiveGuestOverflowAddress() noexcept;
bool ReadActiveGuestMemory(std::uint64_t address, void* output, std::size_t size) noexcept;
struct HostCallMemoryError {};

namespace HostCallDetail {

// Explicit ABI declarations only. Trivial/standard-layout aggregates alone
// do not prove a SysV INTEGER class (e.g. a double has the SSE class).
template <typename T> struct IntegerEightbyte : std::false_type {};

template <typename T> constexpr HostScalar Scalar() {
    using V = std::remove_cv_t<T>;
    if constexpr (std::is_void_v<V>) return HostScalar::Void;
    else if constexpr (std::is_pointer_v<V>) return HostScalar::Gp;
    else if constexpr (std::is_integral_v<V> || std::is_enum_v<V>)
        return sizeof(V) <= 8 ? HostScalar::Gp : HostScalar::Unsupported;
    else if constexpr (IntegerEightbyte<V>::value) {
        static_assert(sizeof(V) == 8 && alignof(V) <= 8 &&
            std::is_trivially_copyable_v<V> && std::is_standard_layout_v<V>,
            "Declared SysV INTEGER eightbyte must retain its verified layout");
        return HostScalar::Gp;
    }
    else if constexpr (std::is_same_v<V, float>) return HostScalar::Float;
    else if constexpr (std::is_same_v<V, double>) return HostScalar::Double;
    else return HostScalar::Unsupported;
}

template <typename T> T FromWord(std::uint64_t word) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(word);
    else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) return static_cast<T>(word);
    else {
        T value {};
        std::memcpy(&value, &word, sizeof(value));
        return value;
    }
}

template <typename T> std::uint64_t ToWord(T value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<std::uint64_t>(value);
    else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>)
        return static_cast<std::uint64_t>(value);
    else {
        std::uint64_t word {};
        std::memcpy(&word, &value, sizeof(value));
        return word;
    }
}

// Aggregate conventions are admitted only by an explicit complete-layout adapter.
template <typename Function> struct CustomSignature { static constexpr bool supported = false; };

template <typename Function> struct Signature;
template <typename R, typename... A> struct Signature<KYTY_SYSV_ABI R (*)(A...)> {
    static constexpr bool supported = sizeof...(A) <= HostCallMaxArguments &&
        Scalar<R>() != HostScalar::Unsupported &&
        ((Scalar<A>() != HostScalar::Unsupported && Scalar<A>() != HostScalar::Void) && ...);
    static constexpr bool variadic = false;
    static constexpr std::size_t arity = sizeof...(A);
    using Return = R;
    using Arguments = std::tuple<A...>;
    static constexpr HostCallMetadata Describe() {
        return {arity, {Scalar<A>()...}, Scalar<R>(), nullptr, nullptr};
    }
};

template <typename R, typename... A> struct Signature<KYTY_SYSV_ABI R (*)(A..., ...)> {
    using Return = R;
    using Arguments = std::tuple<A...>;
    static constexpr std::size_t arity = sizeof...(A);
    static constexpr bool variadic = true;
    template <std::size_t I> static constexpr bool VaArgument() {
        using V = std::tuple_element_t<I, Arguments>;
        if constexpr (I < 7) return std::is_same_v<V, std::uint64_t>;
        else return sizeof(V) == 16 && alignof(V) == 16 && std::is_trivially_copyable_v<V>;
    }
    template <std::size_t... I> static constexpr bool VaSignature(std::index_sequence<I...>) {
        return (VaArgument<I>() && ...);
    }
    // Kyty's explicit VA_ARGS wrapper is the sole admitted variadic signature.
    // Ordinary native ellipsis functions require their own guest adapter.
    static constexpr bool supported = arity == 15 &&
        Scalar<R>() != HostScalar::Unsupported && VaSignature(std::make_index_sequence<arity> {});
    static constexpr HostCallMetadata Describe() {
        HostCallMetadata result {};
        result.arity = arity;
        result.result = Scalar<R>();
        return result;
    }
};

template <typename R, typename... A>
struct Signature<KYTY_SYSV_ABI R (*)(A...) noexcept> : Signature<KYTY_SYSV_ABI R (*)(A...)> {};
template <typename R, typename... A>
struct Signature<KYTY_SYSV_ABI R (*)(A..., ...) noexcept> : Signature<KYTY_SYSV_ABI R (*)(A..., ...)> {};

template <auto Function, std::size_t... I>
std::uint64_t InvokeScalar(const std::uint64_t* words, std::index_sequence<I...>) {
    using S = Signature<decltype(Function)>;
    if constexpr (std::is_void_v<typename S::Return>) {
        Function(FromWord<std::tuple_element_t<I, typename S::Arguments>>(words[I])...);
        return 0;
    } else return ToWord(Function(
        FromWord<std::tuple_element_t<I, typename S::Arguments>>(words[I])...));
}

template <auto Function> std::uint64_t InvokeBound(const std::uint64_t* words) {
    return InvokeScalar<Function>(words, std::make_index_sequence<Signature<decltype(Function)>::arity> {});
}

template <typename T, std::size_t I> T VaValue(const std::uint64_t* gprs,
                                             const LumenAndroidFexCoreSimdViewV2* simd) {
    constexpr std::size_t registers[] {7, 6, 2, 1, 8, 9};
    if constexpr (I < 6) return FromWord<T>(gprs[registers[I]]);
    else if constexpr (I == 6) return FromWord<T>(0);
    else {
        T value {};
        std::memcpy(&value, simd->registers + (I - 7) * simd->registerStride, 16);
        return value;
    }
}

template <auto Function, std::size_t... I>
std::uint64_t InvokeVariadic(const std::uint64_t* gprs,
                            const LumenAndroidFexCoreSimdViewV2* simd,
                            std::index_sequence<I...>) {
    using S = Signature<decltype(Function)>;
    if constexpr (std::is_void_v<typename S::Return>) {
        Function(VaValue<std::tuple_element_t<I, typename S::Arguments>, I>(gprs, simd)...);
        return 0;
    } else return ToWord(Function(
        VaValue<std::tuple_element_t<I, typename S::Arguments>, I>(gprs, simd)...));
}

template <auto Function> std::uint64_t InvokeVariadicBound(
    const std::uint64_t* gprs, const LumenAndroidFexCoreSimdViewV2* simd) {
    return InvokeVariadic<Function>(gprs, simd,
        std::make_index_sequence<Signature<decltype(Function)>::arity> {});
}

} // namespace HostCallDetail

template <auto Function> bool RegisterHostFunction(const char* name) {
    using Custom = HostCallDetail::CustomSignature<decltype(Function)>;
    if constexpr (Custom::supported) {
        return RegisterHostCall(reinterpret_cast<std::uint64_t>(Function),
            Custom::template Describe<Function>(), name);
    } else {
        using S = HostCallDetail::Signature<decltype(Function)>;
        static_assert(S::supported,
            "Every PS5 export requires a verified SysV scalar, variadic or aggregate adapter");
        auto metadata = S::Describe();
        if constexpr (S::variadic) metadata.invoke_variadic = &HostCallDetail::InvokeVariadicBound<Function>;
        else metadata.invoke = &HostCallDetail::InvokeBound<Function>;
        return RegisterHostCall(reinterpret_cast<std::uint64_t>(Function), metadata, name);
    }
}

} // namespace Loader
#endif
