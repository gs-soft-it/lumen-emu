// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace FEXCore::Allocator {

// The debugger route uses one prepared RX arena and a shared RW alias. Code
// pointers remain RX; only the destination of an actual store is translated.
// Initialization is explicit and requires a cooperating development debugger.
int InitializeDebuggerJit();
enum class DebuggerJitProvider : std::uint32_t { LegacyLLDB = 0, Universal = 1 };
int InitializeDebuggerJit(DebuggerJitProvider provider);
// Universal preparation is admitted only by the explicit module entry point.
// Its arena is not ready for ordinary probes until detach and a post-detach
// execution probe have both succeeded.
int DetachUniversalDebuggerJit();
int CompleteUniversalDebuggerJit();
bool IsDebuggerJitReady();
bool IsDebuggerJit();
void* AllocateDebuggerJit(std::size_t size, void* hint = nullptr);
bool FreeDebuggerJit(void* pointer, std::size_t size);
void* JitWritableAddress(const void* pointer);
template<class T> T* JitWritable(T* pointer) {
    return static_cast<T*>(JitWritableAddress(pointer));
}

// The implementation owns one public iOS JIT callback allowlist per image.
// It catches exceptions before returning through pthread's C callback boundary.
void InvokeJitWriteCallback(void (*invoke)(void*), void* context);

template<class Function>
std::invoke_result_t<Function> WithJITWriteProtection(Function&& function) {
    using Result = std::invoke_result_t<Function>;
    if constexpr (std::is_void_v<Result>) {
        InvokeJitWriteCallback([](void* context) {
            (*static_cast<std::remove_reference_t<Function>*>(context))();
        }, &function);
    } else {
        struct Work {
            Function& function;
            std::optional<Result> result;
        } work {function, {}};
        InvokeJitWriteCallback([](void* context) {
            auto& current = *static_cast<Work*>(context);
            current.result.emplace(current.function());
        }, &work);
        return std::move(*work.result);
    }
}

} // namespace FEXCore::Allocator
