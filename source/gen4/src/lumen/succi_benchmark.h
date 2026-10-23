// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Lumen::Gen4 {
struct SessionOptions;
namespace SucciBenchmark {

// A frame at the start boundary establishes the baseline; subsequent unique
// guest frames are divided by the actual monotonic interval, without a 60 FPS cap.
struct Measurement {
    enum class Phase { Waiting, Warmup, Measuring, Complete };
    static constexpr std::int64_t WarmupNs = 5'000'000'000;
    static constexpr std::int64_t DurationNs = 30'000'000'000;
    Phase phase{Phase::Waiting};
    std::int64_t first_ns{};
    std::int64_t start_ns{};
    std::int64_t elapsed_ns{};
    std::uint64_t frames{};

    bool Present(std::int64_t now) {
        if (phase == Phase::Complete) return false;
        if (phase == Phase::Waiting) {
            first_ns = now;
            phase = Phase::Warmup;
        } else if (phase == Phase::Warmup && now - first_ns >= WarmupNs) {
            start_ns = now;
            phase = Phase::Measuring;
        } else if (phase == Phase::Measuring) {
            ++frames;
            elapsed_ns = now - start_ns;
            if (elapsed_ns >= DurationNs) {
                phase = Phase::Complete;
                return true;
            }
        }
        return false;
    }
    double Seconds() const { return static_cast<double>(elapsed_ns) / 1e9; }
    double AverageFps() const { return elapsed_ns > 0 ? frames / Seconds() : 0.; }
};

void ValidateLaunch(SessionOptions& options, const std::filesystem::path& private_files);
using Clock = std::chrono::steady_clock;
// The admitted demo starts with ordinary pacing. Only a fresh graphics request
// from its in-game menu can begin a measurement.
void Initialize(const std::filesystem::path& root, const std::filesystem::path& data);
void PollRequest();
void PollRequest(Clock::time_point now);
bool Enabled();
bool Uncapped();
void SetPresentMode(const char* mode);
void OnGuestPresented();
void OnGuestPresented(Clock::time_point now);
void PresentationFailed();
void Cancel();
void SetActive(bool active);
std::string StatusJson();

} // namespace SucciBenchmark
} // namespace Lumen::Gen4
