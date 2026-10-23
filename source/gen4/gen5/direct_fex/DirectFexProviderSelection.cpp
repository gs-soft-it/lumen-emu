// SPDX-License-Identifier: GPL-2.0-only
#include "DirectFexSelection.h"
#include "GuestCpuRuntime.h"

#include <cstdlib>
#include <cstring>
#include <mutex>

namespace Lumen::GuestCpu {
namespace {
struct Selection {
    std::mutex mutex;
    Provider provider {Provider::Fex};
    bool read_environment {};
    bool locked {};
};
Selection& State() {
    static auto* state = new Selection;
    return *state;
}
void ReadEnvironment(Selection& state) {
    if (state.read_environment) return;
    state.read_environment = true;
    const char* name = std::getenv("LUMEN_CPU_TRANSLATOR");
    if (name != nullptr)
        state.provider = std::strcmp(name, "fex") == 0 ? Provider::Fex
            : std::strcmp(name, "qemu-tcg") == 0 ? Provider::QemuTcg : Provider::Invalid;
}
}
SelectionResult Select(Provider provider) noexcept {
    if (provider != Provider::Fex && provider != Provider::QemuTcg)
        return SelectionResult::UnknownProvider;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    ReadEnvironment(state);
    if (state.locked && state.provider != provider) return SelectionResult::Locked;
    state.provider = provider;
    return provider == Provider::Fex ? SelectionResult::Selected : SelectionResult::Unavailable;
}
SelectionResult Select(const char* name) noexcept {
    if (name == nullptr) return SelectionResult::UnknownProvider;
    if (std::strcmp(name, "fex") == 0) return Select(Provider::Fex);
    if (std::strcmp(name, "qemu-tcg") == 0) return Select(Provider::QemuTcg);
    return SelectionResult::UnknownProvider;
}
Provider Selected() noexcept {
    auto& state = State();
    std::lock_guard lock(state.mutex);
    ReadEnvironment(state);
    return state.provider;
}
const char* ProviderName(Provider provider) noexcept {
    return provider == Provider::Fex ? "fex" : provider == Provider::QemuTcg ? "qemu-tcg" : "invalid";
}
const char* ProviderName() noexcept { return ProviderName(Selected()); }
bool Available(Provider provider) noexcept { return provider == Provider::Fex; }
bool SelectionLocked() noexcept {
    auto& state = State();
    std::lock_guard lock(state.mutex);
    return state.locked;
}
}

namespace Lumen::Ps5::DirectFex {
bool LockSelection() noexcept {
    auto& state = Lumen::GuestCpu::State();
    std::lock_guard lock(state.mutex);
    Lumen::GuestCpu::ReadEnvironment(state);
    state.locked = true;
    return state.provider == Lumen::GuestCpu::Provider::Fex;
}
}
