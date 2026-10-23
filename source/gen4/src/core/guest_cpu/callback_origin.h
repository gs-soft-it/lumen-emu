// SPDX-License-Identifier: MIT
#pragma once
#include <utility>
namespace Core::GuestCpu {
// Provenance is stored in native-owned records at registration. An address not
// belonging to a mapped guest module is never evidence that it is host code.
enum class CallbackOrigin { Guest, Native };
template<typename Guest, typename Native>
decltype(auto) DispatchCallback(CallbackOrigin origin, Guest&& guest, Native&& native) {
    if (origin == CallbackOrigin::Native) return std::forward<Native>(native)();
    return std::forward<Guest>(guest)();
}
} // namespace Core::GuestCpu
