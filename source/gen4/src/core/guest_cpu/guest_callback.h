// SPDX-License-Identifier: MIT
#pragma once

#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU

#include <array>
#include <cstdlib>
#include <span>
#include <string_view>
#include <type_traits>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/types.h"
#include "core/linker.h"

namespace Core::GuestCpu {

template <typename T>
u64 EncodeGuestCallbackArgument(T value) {
    using Value = std::remove_cvref_t<T>;
    static_assert(std::is_integral_v<Value> || std::is_enum_v<Value> ||
                  std::is_pointer_v<Value> || std::is_same_v<Value, std::nullptr_t>);
    if constexpr (std::is_pointer_v<Value>) {
        return reinterpret_cast<u64>(value);
    } else if constexpr (std::is_same_v<Value, std::nullptr_t>) {
        return 0;
    } else {
        return static_cast<u64>(value);
    }
}

inline bool IsGuestFunctionAddress(const void* function) {
    auto* linker = Common::Singleton<Linker>::Instance();
    return function != nullptr && linker->FindByAddress(reinterpret_cast<VAddr>(function)) != nullptr;
}

inline u64 RunGuestFunctionOrAbort(const void* function, std::span<const u64> arguments,
                                   std::string_view label, VAddr stack_top = 0
#if !defined(LUMEN_GEN4_DIRECT_FEX) || defined(__APPLE__)
                                   , std::span<const std::array<u64, 2>> xmm = {}
#endif
                                   ) {
    auto* linker = Common::Singleton<Linker>::Instance();
    const auto result = linker->RunGuestFunction(reinterpret_cast<VAddr>(function), arguments,
                                                  stack_top
#if !defined(LUMEN_GEN4_DIRECT_FEX) || defined(__APPLE__)
                                                  , xmm
#endif
                                                  );
    if (const auto* failure = std::get_if<GuestExecutionFailure>(&result)) {
#ifdef LUMEN_GEN4_DIRECT_FEX
        LOG_CRITICAL(Core_Linker, "FEX guest callback {} failed at stage {}: {}", label,
                     static_cast<int>(failure->Stage), failure->Error);
#else
        LOG_CRITICAL(Core_Linker, "FEX guest callback {} failed at stage {}: {} code={} rip={:#x}", label,
                     static_cast<int>(failure->Stage), failure->Error,
                     failure->RuntimeCode, failure->Rip);
#endif
        Common::Log::Flush();
        std::abort();
    }
    return std::get<u64>(result);
}

template <typename... Args>
u64 RunGuestFunctionOrAbort(const void* function, std::string_view label, Args... args) {
    const std::array<u64, sizeof...(Args)> arguments{EncodeGuestCallbackArgument(args)...};
    return RunGuestFunctionOrAbort(function, arguments, label);
}

} // namespace Core::GuestCpu

#endif
