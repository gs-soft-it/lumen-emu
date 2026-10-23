// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <type_traits>
#include "core/guest_cpu/hle_call_adapter.h"
#include "core/libraries/np/np_callback_registry.h"
static void Check(bool value) {
    if (!value) throw std::runtime_error("NP plus notification compatibility contract failed");
}
int main() {
    using namespace Libraries::Np::NpManager;
    static_assert(std::is_same_v<decltype(&sceNpNotifyPlusFeature),
                                 s32 (PS4_SYSV_ABI *)(void*)>);
    Detail::Callbacks().Reset();
    auto callback = reinterpret_cast<OrbisNpPlusEventCallback>(uintptr_t{0x20000});
    int userdata{};
    Check(sceNpRegisterPlusEventCallback(callback, &userdata) == 0);
    std::array<u8, 64> untouched; untouched.fill(0x5a);
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    Core::GuestCpu::detail::TypedHleCallAdapter<decltype(&sceNpNotifyPlusFeature)>
        adapter{&sceNpNotifyPlusFeature};
    Core::GuestCpu::HleCallFrame frame{};
    for (const auto argument : {u64{0}, ~u64{0}, reinterpret_cast<u64>(untouched.data())}) {
        frame.gpr.fill(0xfedcba9876543210ull);
        frame.gpr[7] = argument;
        const auto before = frame.gpr;
        Check(std::holds_alternative<bool>(adapter.Invoke(frame)));
        Check(frame.gpr[0] == 0);
        Check(std::equal(frame.gpr.begin()+1, frame.gpr.end(), before.begin()+1));
    }
#else
    Check(sceNpNotifyPlusFeature(nullptr) == 0);
    Check(sceNpNotifyPlusFeature(untouched.data()) == 0);
#endif
    Check(std::all_of(untouched.begin(),untouched.end(),[](u8 x){return x==0x5a;}));
    const auto snapshot = Detail::Callbacks().Read();
    Check(snapshot.plus.function == callback && snapshot.plus.userdata == &userdata);
    Detail::Callbacks().Reset();
    std::puts("NP plus notification typed adapter, no-output ABI and unchanged callback state PASS");
}
