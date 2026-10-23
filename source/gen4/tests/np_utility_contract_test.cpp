// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include "core/guest_cpu/hle_call_adapter.h"
#include "core/libraries/np/np_utility.h"
#include "core/loader/symbols_resolver.h"

static void Check(bool condition) {
    if (!condition) throw std::runtime_error("NP utility compatibility contract failed");
}

int main() {
    using namespace Core::Loader;
    using namespace Libraries::Np::NpUtility;
    struct Export { const char* nid; s32 (PS4_SYSV_ABI *handler)(); };
    const std::array exports{
        Export{"BYIZGKm6bO4", sceNpBandwidthTestGetStatus},
        Export{"jktww3yJXnc", sceNpBandwidthTestInitStart},
        Export{"pLr1fEQS1z8", sceNpBandwidthTestShutdown},
        Export{"rAOOqDAxBIk", sceNpWordFilterAbortRequest},
        Export{"1dMndqL-QgE", sceNpWordFilterCensorComment},
        Export{"IEB+vgVoQbw", sceNpWordFilterCreateAsyncRequest},
        Export{"iCq5xW5KQW4", sceNpWordFilterCreateRequest},
        Export{"r9BgI0PfJZg", sceNpWordFilterCreateTitleCtx},
        Export{"PYFS1H70bDs", sceNpWordFilterDeleteRequest},
        Export{"t0P5z5yuFPA", sceNpWordFilterDeleteTitleCtx},
        Export{"ur5SShyG0dk", sceNpWordFilterPollAsync},
        Export{"Jj4mkpFO2gE", sceNpWordFilterSanitizeComment},
    };
    SymbolsResolver symbols;
    RegisterLib(&symbols);
    Check(symbols.GetSize() == exports.size());
    std::array<u8, 128> output;
    output.fill(0xa5);
    for (const auto& entry : exports) {
        SymbolResolver key{.name=entry.nid, .library="libSceNpUtility", .library_version=1,
                           .module="libSceNpUtility", .type=SymbolType::Function};
        const auto* registered = symbols.FindSymbol(key);
        Check(registered != nullptr);
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
        Check(registered->hle_adapter && !registered->hle_fallback);
        Core::GuestCpu::HleCallFrame frame{};
        frame.gpr.fill(0xfedcba9876543210ull);
        frame.gpr[7] = reinterpret_cast<u64>(output.data());
        frame.gpr[6] = 0xffffffffffffffffull;
        frame.gpr[2] = reinterpret_cast<u64>(output.data());
        const auto before = frame.gpr;
        const auto result = registered->hle_adapter->Invoke(frame);
        Check(std::holds_alternative<bool>(result));
        Check(frame.gpr[0] == 0);
        Check(std::equal(frame.gpr.begin()+1, frame.gpr.end(), before.begin()+1));
        Check(std::all_of(output.begin(), output.end(), [](u8 value) { return value == 0xa5; }));
#else
        Check(registered->virtual_address == reinterpret_cast<u64>(entry.handler));
        Check(entry.handler() == 0);
#endif
        key.library = "libSceNpManager";
        Check(symbols.FindSymbol(key) == nullptr);
    }
    std::puts("NP utility registrations and exact zero-return/no-output ABI PASS");
}
