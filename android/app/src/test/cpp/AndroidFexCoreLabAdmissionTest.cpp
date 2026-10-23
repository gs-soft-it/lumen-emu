#include "AndroidFexCoreLabAdmission.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>

int main() {
    using Lumen::Android::FexCoreLab::IsAdmissionAllowed;
    constexpr std::array<std::int64_t, 12> pageSizes {
        -1, 0, 1024, 4095, 4096, 4097, 8192, 16384, 16385, 65536,
        std::numeric_limits<std::int32_t>::max(),
        std::numeric_limits<std::int64_t>::max(),
    };
    constexpr std::array<std::int32_t, 5> apiLevels {-1, 26, 34, 35, 36};
    std::uint32_t cases {};
    for (const auto pageSize : pageSizes) {
        for (const auto api : apiLevels) {
            for (unsigned flags = 0; flags < 8; ++flags) {
                const bool expected = flags == 7 && api >= 35 &&
                    (pageSize == 4096 || pageSize == 16384);
                const bool admitted = IsAdmissionAllowed({
                    (flags & 1u) != 0, (flags & 2u) != 0,
                    (flags & 4u) != 0, api, pageSize,
                });
                if (admitted != expected) {
                    std::fprintf(stderr,
                        "FAIL: FEX admission api=%d pageSize=%lld flags=%u\n",
                        api, static_cast<long long>(pageSize), flags);
                    return 1;
                }
                ++cases;
            }
        }
    }
    std::printf("PASS: FEX lab admission cases=%u hostPages=4K,16K failClosed=1\n", cases);
    return 0;
}
