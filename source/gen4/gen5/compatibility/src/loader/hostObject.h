// Lumen adapter, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#ifndef LUMEN_PS5_HOST_OBJECT_H_
#define LUMEN_PS5_HOST_OBJECT_H_

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Loader {

std::uint64_t RegisterHostObject(const char* name, const void* object, std::size_t size);
std::uint64_t RefuseHostObject(const char* name);
void UpdateGuestProgName(const char* name);
bool HostObjectSelfTest() noexcept;

template <typename T> std::uint64_t RegisterTypedHostObject(const char* name, T* object) {
    using V = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<V, std::uint32_t> || std::is_same_v<V, std::uint64_t> ||
                  std::is_same_v<V, const char*>)
        return RegisterHostObject(name, object, sizeof(T));
    else if constexpr (std::is_array_v<T> && std::extent_v<T> == 4 &&
                       std::is_same_v<std::remove_cv_t<std::remove_extent_t<T>>, std::uint32_t>)
        return RegisterHostObject(name, object, sizeof(T));
    else return RefuseHostObject(name);
}

} // namespace Loader
#endif
