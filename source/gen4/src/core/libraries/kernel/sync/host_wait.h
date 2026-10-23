// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <utility>
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include "core/fex/guest_memory_scope.h"
#include "core/guest_cpu/hle_call_adapter.h"
#endif

namespace Libraries::Kernel {
// The callable may touch native-owned state only. Its native locks must be
// released before it returns, unless they are guest synchronization objects
// never acquired by a VM/GPU publisher. Copy guest inputs before calling.
template <typename Function>
decltype(auto) NativeHostWait(Function&& function) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    Core::Fex::QuiescentHostWait wait;
#endif
    return std::forward<Function>(function)();
}

inline bool ValidateWaitOutput(const void* pointer, std::size_t bytes) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    return Core::GuestCpu::ValidateHlePointer(pointer, bytes, true);
#else
    return pointer != nullptr;
#endif
}
} // namespace Libraries::Kernel
