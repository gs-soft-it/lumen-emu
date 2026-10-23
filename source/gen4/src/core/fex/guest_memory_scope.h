// SPDX-License-Identifier: MIT
#pragma once

#include "GuestCpuRuntime.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace Core::Fex {

#ifdef LUMEN_GEN4_DIRECT_FEX
// Native core memory ownership is provided by its VM and renderer. These
// compatibility scopes must not initialize the independent shared translator.
class MemoryMutationScope final {
public:
    MemoryMutationScope() noexcept = default;
    bool Valid() const noexcept { return true; }
    static bool Active() noexcept { return true; }
    void Publish(std::uint64_t, std::uint64_t, bool = false) noexcept {}
    static void PublishCurrent(std::uint64_t, std::uint64_t, bool = true) noexcept {}
    class SynchronousDelegate final {
    public:
        explicit SynchronousDelegate(MemoryMutationScope&) noexcept {}
    };
};
class QuiescentHostWait final {};
#else
// Acquire before any engine/renderer mutex. End occurs after those locks have
// been released, so a parked CPU never waits on a lock owned by its publisher.
class MemoryMutationScope final {
public:
    MemoryMutationScope() noexcept : previous{current} {
        // Renderer callbacks can be synchronous children of a VM publication.
        // They share its lifetime; only the outer owner closes CPU admission.
        if (previous == nullptr) {
            for (;;) {
                std::uint32_t retryable = 0;
                token = LumenAndroidFexCoreBeginGuestMemoryMutationV3(
                    LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2, &retryable);
                if (token != 0 || !retryable) break;
                const auto wait = LumenAndroidFexCoreBeginQuiescentHostWaitV1();
                if (wait == 0) break;
                if (!LumenAndroidFexCoreEndQuiescentHostWaitV1(wait)) std::abort();
            }
        }
        if (Valid()) current = this;
    }
    ~MemoryMutationScope() {
        if (!Valid()) return;
        current = previous;
        if (previous != nullptr) {
            if (begin != 0) previous->Publish(begin, end - begin);
            return;
        }
        if (!LumenAndroidFexCoreEndGuestMemoryMutationV2(
                token, begin, end - begin, begin != 0, nullptr, nullptr)) std::abort();
    }
    MemoryMutationScope(const MemoryMutationScope&) = delete;
    MemoryMutationScope& operator=(const MemoryMutationScope&) = delete;
    [[nodiscard]] bool Valid() const noexcept { return token != 0 || previous != nullptr; }
    static bool Active() noexcept { return current != nullptr; }

    // The owner must wait synchronously until this object is destroyed. The
    // worker may only access memory under the same publication; it cannot
    // enter guest code, retain a pointer after completion or close the epoch.
    class SynchronousDelegate final {
    public:
        explicit SynchronousDelegate(MemoryMutationScope& owner) noexcept
            : previous{current} {
            if (!owner.Valid() || current != nullptr) std::abort();
            current = &owner;
        }
        ~SynchronousDelegate() { current = previous; }
        SynchronousDelegate(const SynchronousDelegate&) = delete;
        SynchronousDelegate& operator=(const SynchronousDelegate&) = delete;
    private:
        MemoryMutationScope* previous;
    };
    void Publish(std::uint64_t base, std::uint64_t size, bool mappingChanged = false) noexcept {
        if (base == 0 || size == 0 || size > UINT64_MAX - base) std::abort();
        begin = begin == 0 ? base : std::min(begin, base);
        end = std::max(end, base + size);
        if (mappingChanged) LumenAndroidFexCoreNotifyFreshGuestCallbackMappingChangeV1(base, size);
    }
    static void PublishCurrent(std::uint64_t base, std::uint64_t size,
                               bool mappingChanged = true) noexcept {
        if (current == nullptr) std::abort();
        current->Publish(base, size, mappingChanged);
    }
private:
    inline static thread_local MemoryMutationScope* current{};
    MemoryMutationScope* previous{};
    std::uint64_t token{}, begin{}, end{};
};

// Use only around waits over native state with all guest inputs copied. End
// precedes reacquiring engine locks or accessing/revalidating guest pointers.
class QuiescentHostWait final {
public:
    QuiescentHostWait() noexcept : token{LumenAndroidFexCoreBeginQuiescentHostWaitV1()} {}
    ~QuiescentHostWait() {
        if (token != 0 && !LumenAndroidFexCoreEndQuiescentHostWaitV1(token)) std::abort();
    }
    QuiescentHostWait(const QuiescentHostWait&) = delete;
    QuiescentHostWait& operator=(const QuiescentHostWait&) = delete;
private:
    std::uint64_t token;
};

#endif

// Native address recognized by the shared translator's custom-IR registry.
// Guest veneers tail-jump here; the ARM host body is never executed.
std::uintptr_t HostDispatchAddress() noexcept;

} // namespace Core::Fex
