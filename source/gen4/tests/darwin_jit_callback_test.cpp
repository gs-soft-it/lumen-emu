// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/fex/darwin_jit.h"
#include <cstdio>
#include <stdexcept>

int main() {
    const int capability = Core::Fex::ProbeDarwinJit();
    if (capability != 0) {
        std::printf("IOS_FEX_JIT_CALLBACK_V1 available=0 error=%d\n", capability);
        return 77;
    }
    bool nested = false;
    const int value = FEXCore::Allocator::WithJITWriteProtection([&] {
        return FEXCore::Allocator::WithJITWriteProtection([&] {
            nested = true;
            return 42;
        });
    });
    bool caught = false;
    try {
        FEXCore::Allocator::WithJITWriteProtection([] {
            FEXCore::Allocator::WithJITWriteProtection([] {
                throw std::runtime_error("callback scope test");
            });
        });
    } catch (const std::runtime_error&) {
        caught = true;
    }
    // This emits and executes fresh ARM64 code after unwinding. It catches a
    // leaked callback depth or an executable/write-protection state imbalance.
    const bool recovered = Core::Fex::ProbeDarwinJit() == 0;
    std::printf("IOS_FEX_JIT_CALLBACK_V1 available=1 nested=%u value=%d caught=%u recovered=%u\n",
        nested, value, caught, recovered);
    return nested && value == 42 && caught && recovered ? 0 : 1;
}
