// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>

#include "common/native_clock.h"
#include "common/rdtsc.h"
#include "common/uint128.h"

namespace {

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #condition); \
    std::abort(); \
} } while (false)

void CheckRates() {
    struct Rate {
        u64 frequency;
        u32 shift;
        u64 scaled;
    };
    constexpr std::array rates{
        Rate{0, 0, 0}, Rate{1, 30, 1'073'741'824},
        Rate{19'200'000, 6, 1'228'800'000}, Rate{24'000'000, 6, 1'536'000'000},
        Rate{31'250'000, 5, 1'000'000'000}, Rate{100'000'000, 4, 1'600'000'000},
        Rate{499'999'999, 2, 1'999'999'996}, Rate{500'000'000, 1, 1'000'000'000},
        Rate{999'999'999, 1, 1'999'999'998}, Rate{1'000'000'000, 0, 1'000'000'000},
        Rate{2'400'000'000, 0, 2'400'000'000},
        Rate{std::numeric_limits<u64>::max(), 0, std::numeric_limits<u64>::max()},
    };
    for (const auto& rate : rates) {
        CHECK(Common::Arm64CounterShift(rate.frequency) == rate.shift);
        CHECK((rate.frequency << rate.shift) == rate.scaled);
        if (rate.scaled == 0 || rate.scaled == std::numeric_limits<u64>::max()) continue;
        for (const u64 units : {u64{1'000}, u64{1'000'000}, u64{1'000'000'000}}) {
            const u64 converted = Common::MultiplyHigh(
                rate.scaled, Common::GetFixedPoint64Factor(units, rate.scaled));
            if (units == rate.scaled) {
                // Preserve the existing fixed-point representation at its exact boundary.
                CHECK(converted == 0);
            } else {
                CHECK(converted <= units && converted + 1 >= units);
            }
        }
    }
}

#ifdef ARCH_ARM64
u64 ReadRawCounter() {
    u64 value;
    asm volatile("isb\n\tmrs %0, cntvct_el0\n\tisb" : "=r"(value) :: "memory");
    return value;
}

u64 ReadRawFrequency() {
    u64 value;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}

void CheckCounterDomain(const Common::NativeClock& clock) {
    const u64 frequency = ReadRawFrequency();
    CHECK(frequency != 0);
    const u32 shift = Common::Arm64CounterShift(frequency);
    const u64 expected_frequency = frequency << shift;
    const u64 measured_frequency = clock.GetTscFrequency();
    const u64 difference = measured_frequency > expected_frequency
                               ? measured_frequency - expected_frequency
                               : expected_frequency - measured_frequency;
    CHECK(difference <= expected_frequency / 100 + 100'000);
    for (unsigned i = 0; i != 128; ++i) {
        const u64 before = ReadRawCounter();
        const u64 scaled = Common::FencedRDTSC();
        const u64 after = ReadRawCounter();
        CHECK((scaled >> shift) >= before && (scaled >> shift) <= after);
        const u64 uptime_before = ReadRawCounter();
        const u64 uptime = clock.GetUptime();
        const u64 uptime_after = ReadRawCounter();
        CHECK((uptime >> shift) >= uptime_before && (uptime >> shift) <= uptime_after);
    }
    std::printf("ARM counter raw_frequency=%llu shift=%u expected_frequency=%llu measured_frequency=%llu\n",
                static_cast<unsigned long long>(frequency), shift,
                static_cast<unsigned long long>(expected_frequency),
                static_cast<unsigned long long>(measured_frequency));
}
#endif

void CheckElapsed(const Common::NativeClock& clock) {
    const auto start = std::chrono::steady_clock::now();
    const u64 base = clock.GetUptime();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    const u64 ns = clock.GetTimeNS(base);
    const u64 us = clock.GetTimeUS(base);
    const u64 ms = clock.GetTimeMS(base);
    const u64 elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    const u64 upper_ns = elapsed_ns + elapsed_ns / 20 + 2'000'000;
    if (Common::GetFixedPoint64Factor(1'000'000'000, clock.GetTscFrequency()) == 0) {
        CHECK(ns == 0);
    } else {
        CHECK(ns >= 15'000'000 && ns <= upper_ns);
    }
    CHECK(us >= 15'000 && us <= upper_ns / 1'000);
    CHECK(ms >= 15 && ms <= upper_ns / 1'000'000);
}

} // namespace

int main() {
    CheckRates();
    const Common::NativeClock clock;
    CHECK(clock.GetTscFrequency() != 0);
#ifdef ARCH_ARM64
    CheckCounterDomain(clock);
#endif
    CheckElapsed(clock);
    std::puts("PASS counter scale: 12 rates, raw counter brackets, production frequency and elapsed NS/US/MS");
}
