// Lumen adapter, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#include "loader/hostCall.h"
#if defined(LUMEN_PS5_DIRECT_FEX)
#include "DirectFexRuntime.h"
#endif
#include "FexPerformanceTrace.h"
#if defined(LUMEN_PS5_FEXCORE)
#include "GuestCpuRuntime.h"
#endif

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Loader {
namespace {
struct Entry {
    HostCallMetadata metadata;
    std::string name;
    std::atomic<Lumen::GuestCpu::HostCallAdmissionPolicyV1> policy;
    bool contract_specified;
    Entry(const HostCallMetadata& description, const char* symbol)
        : metadata(description), name(symbol), policy(Lumen::GuestCpu::MakeHostCallAdmissionPolicy(
            description.effects, description.short_contract.value_or(
                Lumen::GuestCpu::HostCallShortContract::Ordinary))),
          contract_specified(description.short_contract.has_value()) {}
    static_assert(decltype(policy)::is_always_lock_free);
};
std::mutex registry_mutex;
std::unordered_map<std::uint64_t, Entry> registry;
std::atomic<bool> registry_ready {true};
// Registrations only append immutable nodes; aliases never replace an entry.
// unordered_map rehash preserves node addresses, and the registry has no erase
// path. Each thread can therefore retain a bounded set of metadata references
// without taking the process-wide registry mutex at every HLE boundary.
struct CachedEntry { std::uint64_t target {}; const Entry* entry {}; };
thread_local std::array<CachedEntry, 32> dispatch_cache {};

const Entry* FindDispatchEntry(std::uint64_t target) {
    if (!registry_ready.load(std::memory_order_acquire)) return nullptr;
    auto& cached = dispatch_cache[((target >> 2) ^ (target >> 10)) & (dispatch_cache.size() - 1)];
    if (cached.entry != nullptr && cached.target == target) return cached.entry;
    std::lock_guard lock(registry_mutex);
    const auto found = registry.find(target);
    if (!registry_ready.load(std::memory_order_acquire) || found == registry.end()) return nullptr;
    cached = {target, &found->second};
    return cached.entry;
}
struct ActiveMemory { GuestExecution::MemoryRead read; void* opaque; std::uint64_t rsp; };
thread_local const ActiveMemory* active_memory {};

bool SimdValid(const LumenAndroidFexCoreSimdViewV2* view) {
    return view != nullptr && view->structSize == sizeof(*view) && view->registers != nullptr &&
        view->registerCount >= 8 && view->registerStride >= 16 && view->registerBytes >= 16;
}
bool SameMetadata(const HostCallMetadata& a, const HostCallMetadata& b) {
    return a.arity == b.arity && a.arguments == b.arguments && a.result == b.result &&
        a.invoke == b.invoke && a.invoke_variadic == b.invoke_variadic;
}

void TraceTargetName(std::uint64_t target, const std::string& name) {
    if (!Lumen::Performance::Enabled()) return;
    // Keep one parseable token even if a registered symbol contains whitespace
    // or control characters. This runs once per canonical target, never in HLE.
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':') {
            encoded += static_cast<char>(c);
        } else {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 15];
        }
    }
#if defined(__APPLE__) || defined(__linux__)
    const auto pid = static_cast<unsigned long long>(getpid());
#else
    const auto pid = 0ULL;
#endif
    std::fprintf(stderr, "PS5_HLE_TARGET_NAME_V1 pid=%llu target=%#llx name=%s encoding=percent canonical=1\n",
        pid, static_cast<unsigned long long>(target), encoded.c_str());
}
} // namespace

bool RegisterHostCall(std::uint64_t target, const HostCallMetadata& metadata, const char* name) {
    using Effect = Lumen::GuestCpu::HostCallEffect;
    const bool light_signature = metadata.invoke != nullptr && metadata.invoke_variadic == nullptr &&
        metadata.arity <= 6 && (metadata.result == HostScalar::Gp || metadata.result == HostScalar::Void) &&
        std::all_of(metadata.arguments.begin(), metadata.arguments.begin() + metadata.arity,
            [](HostScalar argument) { return argument == HostScalar::Gp; });
    if (target == 0 || metadata.arity > HostCallMaxArguments || name == nullptr ||
        !Lumen::GuestCpu::ValidHostCallEffects(metadata.effects) ||
        (metadata.effects == Effect::None && !light_signature) ||
        (metadata.short_contract &&
         (!Lumen::GuestCpu::ValidHostCallShortContract(*metadata.short_contract) ||
          (*metadata.short_contract == Lumen::GuestCpu::HostCallShortContract::SynchronousGuestMemory &&
           (metadata.effects != Effect::GuestMemory || !light_signature))))) {
        std::lock_guard lock(registry_mutex);
        registry_ready = false;
        return false;
    }
#if defined(LUMEN_PS5_DIRECT_FEX)
    if (Lumen::Ps5::DirectFex::Probe() != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) {
        std::lock_guard lock(registry_mutex);
        registry_ready = false;
        return false;
    }
#elif defined(LUMEN_PS5_FEXCORE)
    // Cold runtime initialization executes its own guest bridge probes. With
    // short admission those probes query effects, including unknown targets,
    // and may re-enter this registry. Never hold registry_mutex across them.
    if (!Lumen::GuestCpu::RegisterHostCallEffectsQuery(HostCallEffectsFor) ||
        !Lumen::GuestCpu::RegisterHostCallAdmissionPolicyQueryV1(HostCallAdmissionPolicyFor) ||
        LumenAndroidFexCoreRuntimeProbeV1(nullptr) != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) {
        std::lock_guard lock(registry_mutex);
        registry_ready = false;
        return false;
    }
#endif
    std::lock_guard lock(registry_mutex);
    const auto previous = registry.find(target);
    if (previous != registry.end()) {
        const auto policy = previous->second.policy.load(std::memory_order_acquire);
        const auto effects = Lumen::GuestCpu::AdmissionPolicyEffects(policy);
        const auto contract = Lumen::GuestCpu::AdmissionPolicyContract(policy);
        if (metadata.short_contract && previous->second.contract_specified &&
            *metadata.short_contract != contract) {
            std::fprintf(stderr, "PS5 HLE conflicting short contract: %s\n", name);
            registry_ready = false;
            return false;
        }
        if (effects != Effect::Unknown && metadata.effects != Effect::Unknown && effects != metadata.effects) {
            std::fprintf(stderr, "PS5 HLE conflicting effects: %s\n", name);
            registry_ready = false;
            return false;
        }
        // Different template invokers are legitimate aliases only when both
        // describe the same ABI. Their bound native target is identical.
        const auto& m = previous->second.metadata;
        if (SameMetadata(m, metadata) || (m.arity == metadata.arity && m.arguments == metadata.arguments && m.result == metadata.result &&
            bool(m.invoke) == bool(metadata.invoke) && bool(m.invoke_variadic) == bool(metadata.invoke_variadic))) {
            // Unclassified aliases express no new effect claim. A later
            // explicit classification is atomically published to cached nodes.
            const auto merged_effects = metadata.effects == Effect::Unknown ? effects : metadata.effects;
            const auto merged_contract = metadata.short_contract.value_or(contract);
            previous->second.policy.store(Lumen::GuestCpu::MakeHostCallAdmissionPolicy(
                merged_effects, merged_contract), std::memory_order_release);
            previous->second.contract_specified |= metadata.short_contract.has_value();
            return true;
        }
        std::fprintf(stderr, "PS5 HLE conflicting signature: %s\n", name);
        registry_ready = false;
        return false;
    }
#if defined(LUMEN_PS5_DIRECT_FEX)
    if (!Lumen::Ps5::DirectFex::RegisterHostTarget(target)) {
        registry_ready = false;
        return false;
    }
#elif defined(LUMEN_PS5_FEXCORE)
    // Initialization has completed outside the lock; target publication does
    // not execute guest code. Keep it serialized with metadata insertion.
    if (LumenAndroidFexCoreRegisterHostCallTargetV1(target) != 1) {
        registry_ready = false;
        return false;
    }
#endif
    const auto [entry, inserted] = registry.try_emplace(target, metadata, name);
    if (inserted) TraceTargetName(target, entry->second.name);
    return true;
}

Lumen::GuestCpu::HostCallAdmissionPolicyV1 HostCallAdmissionPolicyFor(std::uint64_t target) noexcept {
    const auto* entry = FindDispatchEntry(target);
    return entry != nullptr ? entry->policy.load(std::memory_order_acquire) :
        Lumen::GuestCpu::MakeHostCallAdmissionPolicy(Lumen::GuestCpu::HostCallEffect::Unknown);
}
std::uint32_t HostCallEffectsFor(std::uint64_t target) noexcept {
    return Lumen::GuestCpu::EffectMask(Lumen::GuestCpu::AdmissionPolicyEffects(
        HostCallAdmissionPolicyFor(target)));
}

bool IsHostCallTarget(std::uint64_t target) noexcept {
    std::lock_guard lock(registry_mutex);
    return registry.contains(target);
}
bool IsHostCallSupported(std::uint64_t target) noexcept {
    std::lock_guard lock(registry_mutex);
    const auto found = registry.find(target);
    return found != registry.end() &&
        (found->second.metadata.invoke != nullptr || found->second.metadata.invoke_variadic != nullptr);
}
bool HostCallRegistryReady() noexcept {
    std::lock_guard lock(registry_mutex);
    if (!registry_ready) return false;
    for (const auto& [target, entry]: registry)
        if (entry.metadata.invoke != nullptr || entry.metadata.invoke_variadic != nullptr) return true;
    return false;
}
std::size_t HostCallCount() noexcept {
    std::lock_guard lock(registry_mutex);
    return registry.size();
}

std::uint64_t ActiveGuestOverflowAddress() noexcept {
    return active_memory != nullptr && active_memory->rsp <= std::numeric_limits<std::uint64_t>::max() - 8
        ? active_memory->rsp + 8 : 0;
}
bool ReadActiveGuestMemory(std::uint64_t address, void* output, std::size_t size) noexcept {
    return active_memory != nullptr && active_memory->read != nullptr &&
        active_memory->read(active_memory->opaque, address, output, size);
}

bool DispatchHostCall(std::uint64_t target, std::uint64_t* gprs,
                      LumenAndroidFexCoreSimdViewV2* simd,
                      GuestExecution::MemoryRead read, void* opaque) noexcept {
    if (gprs == nullptr || read == nullptr) return false;
    const auto* found = FindDispatchEntry(target);
    if (found == nullptr) return false;
    const auto& entry = *found;
    const auto& metadata = entry.metadata;
    if (metadata.invoke == nullptr && metadata.invoke_variadic == nullptr) {
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
    } catch (...) {
        std::fprintf(stderr, "PS5 HLE call failed: %s\n", entry.name.c_str());
        return false;
    }
}
} // namespace Loader
