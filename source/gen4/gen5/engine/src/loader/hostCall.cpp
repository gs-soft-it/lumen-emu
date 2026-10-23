// SPDX-License-Identifier: GPL-2.0-only
// ARM64 host ABI adapter: every dispatch uses a published typed registry entry.
#include "loader/hostCall.h"
#include "DirectFexRuntime.h"
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
namespace Loader {
namespace {
struct Entry { std::string name; HostCallMetadata metadata; };
std::mutex registry_mutex;
std::unordered_map<std::uint64_t, Entry> registry;
std::atomic<bool> registry_ready {true};
struct ActiveMemory { GuestExecution::MemoryRead read; void* opaque; std::uint64_t rsp; };
thread_local const ActiveMemory* active_memory {};
bool SimdValid(const LumenAndroidFexCoreSimdViewV2* view) {
 return view && view->structSize == sizeof(*view) && view->registers &&
  view->registerCount >= 8 && view->registerStride >= 16 && view->registerBytes >= 16;
}
const Entry* FindDispatchEntry(std::uint64_t target) {
 if (!registry_ready.load(std::memory_order_acquire)) return nullptr;
 // An indexed veneer publishes this immutable node before it can execute.
 // The bridge scopes it to this exact target, restoring it across callbacks.
 // Standalone host dispatch and the default path retain the locked lookup.
 if (const auto* binding = Lumen::Ps5::DirectFex::ActiveHostTargetBinding(target))
  return static_cast<const Entry*>(binding);
 std::lock_guard lock(registry_mutex);
 const auto found = registry.find(target);
 return registry_ready.load(std::memory_order_acquire) && found != registry.end() ? &found->second : nullptr;
}
}
bool RegisterHostCall(std::uint64_t target, const HostCallMetadata& metadata, const char* name) {
 if (!target || !name || metadata.arity > HostCallMaxArguments) return false;
 if (!metadata.invoke && !metadata.invoke_variadic && !metadata.invoke_custom) {
  std::lock_guard lock(registry_mutex);
  registry_ready = false;
  std::fprintf(stderr, "PS5 HLE missing typed adapter: %s\n", name);
  return false;
 }
 if (Lumen::Ps5::DirectFex::Probe() != 0) return false;
 std::lock_guard lock(registry_mutex);
 if (!registry_ready.load(std::memory_order_acquire)) return false;
 const auto previous = registry.find(target);
 if (previous != registry.end()) {
  const auto& old = previous->second.metadata;
  if (old.arity == metadata.arity && old.arguments == metadata.arguments &&
      old.result == metadata.result && bool(old.invoke) == bool(metadata.invoke) &&
      bool(old.invoke_variadic) == bool(metadata.invoke_variadic) &&
      bool(old.invoke_custom) == bool(metadata.invoke_custom)) return true;
  registry_ready = false;
  std::fprintf(stderr, "PS5 HLE conflicting signature: %s\n", name);
  return false;
 }
 // unordered_map rehash preserves node addresses. Nodes are never erased or
 // changed after publication, including on a later registration failure.
 try {
  const auto [entry, inserted] = registry.emplace(target, Entry{name, metadata});
  if (!inserted || !Lumen::Ps5::DirectFex::RegisterHostTarget(target, &entry->second)) {
   registry_ready.store(false, std::memory_order_release);
   return false;
  }
 } catch (...) {
  registry_ready.store(false, std::memory_order_release);
  return false;
 }
 return true;
}
bool IsHostCallTarget(std::uint64_t target) noexcept { return FindDispatchEntry(target) != nullptr; }
bool IsHostCallSupported(std::uint64_t target) noexcept {
 const auto* entry = FindDispatchEntry(target);
 return entry && (entry->metadata.invoke || entry->metadata.invoke_variadic || entry->metadata.invoke_custom);
}
bool HostCallRegistryReady() noexcept { std::lock_guard lock(registry_mutex); return registry_ready.load(std::memory_order_acquire); }
std::size_t HostCallCount() noexcept { std::lock_guard lock(registry_mutex); return registry.size(); }
std::uint64_t ActiveGuestOverflowAddress() noexcept {
 return active_memory && active_memory->rsp <= UINT64_MAX - 8 ? active_memory->rsp + 8 : 0;
}
bool ReadActiveGuestMemory(std::uint64_t address, void* output, std::size_t size) noexcept {
 return active_memory && active_memory->read && active_memory->read(active_memory->opaque,address,output,size);
}
bool DispatchHostCall(std::uint64_t target, std::uint64_t* gprs,
                      LumenAndroidFexCoreSimdViewV2* simd,
                      GuestExecution::MemoryRead read, void* opaque) noexcept {
    if (gprs == nullptr || read == nullptr) return false;
    const auto* found = FindDispatchEntry(target);
    if (found == nullptr) return false;
    const auto& entry = *found;
    const auto& metadata = entry.metadata;
    if (metadata.invoke == nullptr && metadata.invoke_variadic == nullptr && metadata.invoke_custom == nullptr) {
        std::fprintf(stderr, "PS5 HLE unsupported signature: %s\n", entry.name.c_str());
        return false;
    }
    if ((metadata.result == HostScalar::Float || metadata.result == HostScalar::Double) &&
        !SimdValid(simd)) return false;
    const ActiveMemory current {read, opaque, gprs[4]};
    struct Scope {
        const ActiveMemory* previous {active_memory};
        explicit Scope(const ActiveMemory& current) { active_memory = &current; }
        ~Scope() { active_memory = previous; }
    } scope(current);
    std::array<std::uint64_t, HostCallMaxArguments> arguments {};
    constexpr std::size_t gp_registers[] {7, 6, 2, 1, 8, 9};
    std::size_t gp {}, fp {}, spill {};
    try {
        if (metadata.invoke_custom) return metadata.invoke_custom(gprs, simd);
        if (metadata.invoke_variadic != nullptr) {
            if (!SimdValid(simd) || ActiveGuestOverflowAddress() == 0) return false;
        } else {
            for (std::size_t index = 0; index < metadata.arity; ++index) {
                const auto type = metadata.arguments[index];
                const bool is_gp = type == HostScalar::Gp;
                if (!is_gp && type != HostScalar::Float && type != HostScalar::Double) return false;
                const auto width = type == HostScalar::Float ? 4U : 8U;
                if (is_gp && gp < 6) arguments[index] = gprs[gp_registers[gp++]];
                else if (!is_gp && fp < 8) {
                    if (!SimdValid(simd)) return false;
                    std::memcpy(&arguments[index], simd->registers + fp++ * simd->registerStride, width);
                } else {
                    if (gprs[4] > std::numeric_limits<std::uint64_t>::max() - 8 - spill * 8 ||
                        !read(opaque, gprs[4] + 8 + spill++ * 8, &arguments[index], width)) return false;
                }
            }
        }
        const auto result = metadata.invoke_variadic != nullptr ? metadata.invoke_variadic(gprs, simd) :
            metadata.invoke(arguments.data());
        switch (metadata.result) {
        case HostScalar::Void: break;
        case HostScalar::Gp: gprs[0] = result; break;
        case HostScalar::Float:
        case HostScalar::Double:
            if (!SimdValid(simd)) return false;
            std::memcpy(simd->registers, &result, metadata.result == HostScalar::Float ? 4U : 8U);
            break;
        default: return false;
        }
        return true;
    } catch (const GuestExecution::ThreadExitSignal&) {
        return true;
    } catch (...) {
        std::fprintf(stderr, "PS5 HLE call failed: %s\n", entry.name.c_str());
        return false;
    }
}
} // namespace Loader
