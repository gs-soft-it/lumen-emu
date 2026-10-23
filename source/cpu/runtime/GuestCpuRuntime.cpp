// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
#include "GuestCpuRuntime.h"

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <mutex>

namespace Lumen::GuestCpu {
namespace {
constinit std::atomic<Provider> selected {Provider::Fex};
constinit std::atomic<bool> locked {false};
constinit std::atomic<const ProviderFunctions*> active {};
constinit std::atomic<MemoryAccessQuery> memory_access_query {};
constinit std::atomic<HostCallEffectsQuery> host_call_effects_query {};
constinit std::atomic<HostCallAdmissionPolicyQueryV1> host_call_admission_policy_query {};
static_assert(decltype(active)::is_always_lock_free);
static_assert(decltype(memory_access_query)::is_always_lock_free);
static_assert(decltype(host_call_effects_query)::is_always_lock_free);
static_assert(decltype(host_call_admission_policy_query)::is_always_lock_free);

struct Registration {
    std::mutex mutex;
    ProviderFunctions qemu {};
    bool qemu_registered {};
    bool environment_read {};
};
bool Complete(const ProviderFunctions& functions) noexcept;
void CopyFunctions(ProviderFunctions& destination, const ProviderFunctions& source) noexcept {
    destination = {};
    // Complete has validated a known layout. Never read the absent tail of a
    // legacy provider, including when its table ends at an inaccessible page.
    std::memcpy(&destination, &source, source.structSize);
}
Registration& Registry() {
    // VM hooks may run during static initialization and process shutdown.
    // Keep registration state alive independently of translation-unit order.
    static auto* const registry = [] {
        auto* result = new Registration;
#if defined(LUMEN_WITH_QEMU_TCG) && LUMEN_WITH_QEMU_TCG
        const auto& qemu = QemuProviderFunctions();
        if (qemu.provider == Provider::QemuTcg && Complete(qemu)) {
            CopyFunctions(result->qemu, qemu);
            result->qemu_registered = true;
        }
#endif
        return result;
    }();
    return *registry;
}

bool Known(Provider provider) noexcept {
    return provider == Provider::Fex || provider == Provider::QemuTcg;
}
void ResolveEnvironment(Registration& registry) noexcept {
    if (registry.environment_read) return;
    registry.environment_read = true;
    const char* name = std::getenv("LUMEN_CPU_TRANSLATOR");
    if (name == nullptr) return;
    selected.store(std::strcmp(name, "fex") == 0 ? Provider::Fex
        : std::strcmp(name, "qemu-tcg") == 0 ? Provider::QemuTcg
        : Provider::Invalid, std::memory_order_release);
}
bool Complete(const ProviderFunctions& functions) noexcept {
    if ((functions.structSize != ProviderFunctionsV1Size && functions.structSize != ProviderFunctionsV2Size &&
         functions.structSize != ProviderFunctionsV3Size &&
         functions.structSize != ProviderFunctionsV4Size &&
         functions.structSize != sizeof(functions)) ||
        functions.abiVersion != 1 ||
        !Known(functions.provider)) return false;
#define LUMEN_GUEST_CPU_CHECK_Required(field) if (functions.field == nullptr) return false;
#define LUMEN_GUEST_CPU_CHECK_Optional(field)
#define LUMEN_GUEST_CPU_API(requirement, field, symbol, result, parameters, arguments, denied) \
    LUMEN_GUEST_CPU_CHECK_##requirement(field)
#include "GuestCpuRuntimeApi.inc"
#undef LUMEN_GUEST_CPU_API
#undef LUMEN_GUEST_CPU_CHECK_Optional
#undef LUMEN_GUEST_CPU_CHECK_Required
    return true;
}
bool Same(const ProviderFunctions& left, const ProviderFunctions& right) noexcept {
    if (left.structSize != right.structSize || left.abiVersion != right.abiVersion ||
        left.provider != right.provider) return false;
#define LUMEN_GUEST_CPU_API(requirement, field, symbol, result, parameters, arguments, denied) \
    if (left.structSize >= offsetof(ProviderFunctions, field) + sizeof(left.field) && \
        left.field != right.field) return false;
#include "GuestCpuRuntimeApi.inc"
#undef LUMEN_GUEST_CPU_API
    return true;
}

const ProviderFunctions* Acquire() noexcept {
    if (const auto* provider = active.load(std::memory_order_acquire)) return provider;
    auto& registry = Registry();
    std::lock_guard guard(registry.mutex);
    ResolveEnvironment(registry);
    // A failed initialization or unavailable provider cannot reopen selection.
    locked.store(true, std::memory_order_release);
    const auto provider = selected.load(std::memory_order_relaxed);
    const auto* functions = provider == Provider::Fex ? &FexProviderFunctions()
        : provider == Provider::QemuTcg && registry.qemu_registered ? &registry.qemu : nullptr;
    if (functions != nullptr) active.store(functions, std::memory_order_release);
    return functions;
}
} // namespace

SelectionResult Select(Provider provider) noexcept {
    if (!Known(provider)) return SelectionResult::UnknownProvider;
    auto& registry = Registry();
    std::lock_guard guard(registry.mutex);
    if (locked.load(std::memory_order_relaxed)) {
        if (provider != selected.load(std::memory_order_relaxed)) return SelectionResult::Locked;
        return active.load(std::memory_order_acquire) != nullptr
            ? SelectionResult::Selected : SelectionResult::Unavailable;
    }
    registry.environment_read = true;
    selected.store(provider, std::memory_order_release);
    return provider == Provider::Fex || registry.qemu_registered
        ? SelectionResult::Selected : SelectionResult::Unavailable;
}
SelectionResult Select(const char* name) noexcept {
    if (name == nullptr) return SelectionResult::UnknownProvider;
    if (std::strcmp(name, "fex") == 0) return Select(Provider::Fex);
    if (std::strcmp(name, "qemu-tcg") == 0) return Select(Provider::QemuTcg);
    return SelectionResult::UnknownProvider;
}
Provider Selected() noexcept {
    if (locked.load(std::memory_order_acquire)) return selected.load(std::memory_order_acquire);
    auto& registry = Registry();
    std::lock_guard guard(registry.mutex);
    ResolveEnvironment(registry);
    return selected.load(std::memory_order_acquire);
}
const char* ProviderName(Provider provider) noexcept {
    switch (provider) {
        case Provider::Fex: return "fex";
        case Provider::QemuTcg: return "qemu-tcg";
        case Provider::Invalid: return "unknown";
    }
    return "unknown";
}
const char* ProviderName() noexcept { return ProviderName(Selected()); }
bool SelectionLocked() noexcept { return locked.load(std::memory_order_acquire); }
bool Available(Provider provider) noexcept {
    if (provider == Provider::Fex) return true;
    if (provider != Provider::QemuTcg) return false;
    auto& registry = Registry();
    std::lock_guard guard(registry.mutex);
    return registry.qemu_registered;
}
bool RegisterProvider(const ProviderFunctions& functions) noexcept {
    if (!Complete(functions) || functions.provider != Provider::QemuTcg) return false;
    auto& registry = Registry();
    std::lock_guard guard(registry.mutex);
    if (registry.qemu_registered) return Same(registry.qemu, functions);
    if (locked.load(std::memory_order_relaxed)) return false;
    CopyFunctions(registry.qemu, functions);
    registry.qemu_registered = true;
    return true;
}
bool RegisterMemoryAccessQuery(MemoryAccessQuery query) noexcept {
    if (query == nullptr) return false;
    MemoryAccessQuery expected {};
    return memory_access_query.compare_exchange_strong(expected, query,
        std::memory_order_release, std::memory_order_acquire) || expected == query;
}
MemoryAccessQuery GetMemoryAccessQuery() noexcept {
    return memory_access_query.load(std::memory_order_acquire);
}
bool RegisterHostCallEffectsQuery(HostCallEffectsQuery query) noexcept {
    if (query == nullptr) return false;
    HostCallEffectsQuery expected {};
    return host_call_effects_query.compare_exchange_strong(expected, query,
        std::memory_order_release, std::memory_order_acquire) || expected == query;
}
bool RegisterHostCallAdmissionPolicyQueryV1(HostCallAdmissionPolicyQueryV1 query) noexcept {
    if (query == nullptr) return false;
    HostCallAdmissionPolicyQueryV1 expected {};
    return host_call_admission_policy_query.compare_exchange_strong(expected, query,
        std::memory_order_release, std::memory_order_acquire) || expected == query;
}
HostCallAdmissionPolicyV1 QueryHostCallAdmissionPolicyV1(std::uint64_t target) noexcept {
    const auto query = host_call_admission_policy_query.load(std::memory_order_acquire);
    const auto policy = query != nullptr ? query(target) : MakeHostCallAdmissionPolicy(
        static_cast<HostCallEffect>(QueryHostCallEffects(target)));
    return ValidHostCallEffects(AdmissionPolicyEffects(policy)) &&
        ValidHostCallShortContract(AdmissionPolicyContract(policy)) ? policy :
        MakeHostCallAdmissionPolicy(HostCallEffect::Unknown);
}
HostCallEffectMask QueryHostCallEffects(std::uint64_t target) noexcept {
    const auto query = host_call_effects_query.load(std::memory_order_acquire);
    return query != nullptr ? query(target) :
        static_cast<HostCallEffectMask>(HostCallEffect::Unknown);
}
} // namespace Lumen::GuestCpu

namespace {
std::uint32_t DeniedProbe(LumenAndroidFexCoreRuntimeStatusV1* status) noexcept {
    if (status != nullptr) {
        *status = {};
        status->structSize = sizeof(*status);
        status->abiVersion = 1;
        status->code = LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1;
    }
    return LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1;
}
std::uint32_t DeniedTsc(LumenAndroidFexCoreGuestTscV1* output) noexcept {
    if (output != nullptr) *output = {};
    return LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1;
}
std::uint32_t DeniedFp(LumenAndroidFexCoreFpBoundaryProbeStatusV1* status) noexcept {
    if (status != nullptr) {
        *status = {};
        status->runtimeCode = LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1;
    }
    return 0;
}
const char* DeniedReceipt() noexcept {
    switch (Lumen::GuestCpu::Selected()) {
        case Lumen::GuestCpu::Provider::QemuTcg:
            return "LUMEN_GUEST_CPU_RUNTIME_V1 backend=qemu-tcg ready=0 linked=0 failClosed=1 fallback=0";
        case Lumen::GuestCpu::Provider::Fex:
            return "LUMEN_GUEST_CPU_RUNTIME_V1 backend=fex ready=0 linked=0 failClosed=1 fallback=0";
        default:
            return "LUMEN_GUEST_CPU_RUNTIME_V1 backend=unknown ready=0 invalidSelector=1 failClosed=1 fallback=0";
    }
}
} // namespace

#define LUMEN_GUEST_CPU_PRESENT_Required(field) true
#define LUMEN_GUEST_CPU_PRESENT_Optional(field) \
    (provider->structSize >= offsetof(Lumen::GuestCpu::ProviderFunctions, field) + sizeof(provider->field))
#define LUMEN_GUEST_CPU_API(requirement, field, symbol, result, parameters, arguments, denied) \
    extern "C" result symbol parameters noexcept { \
        const auto* provider = Lumen::GuestCpu::Acquire(); \
        if (provider != nullptr && LUMEN_GUEST_CPU_PRESENT_##requirement(field) && \
            provider->field != nullptr) return provider->field arguments; \
        return denied; \
    }
#include "GuestCpuRuntimeApi.inc"
#undef LUMEN_GUEST_CPU_API
#undef LUMEN_GUEST_CPU_PRESENT_Optional
#undef LUMEN_GUEST_CPU_PRESENT_Required
