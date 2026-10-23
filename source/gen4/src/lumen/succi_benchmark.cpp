// SPDX-License-Identifier: GPL-2.0-only
#include "lumen/succi_benchmark.h"
#include "lumen/session.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace Lumen::Gen4::SucciBenchmark {
namespace {
std::atomic_bool enabled{};
std::atomic_bool uncapped{};
std::mutex mutex;
Measurement measurement;
std::filesystem::path result_path, request_path, status_path;
std::string outcome{"unavailable"};
std::string reason{"Benchmark not started"};
std::string present_mode{"unknown"};
std::uint64_t request_sequence{};
Clock::time_point next_poll{};
bool ui_active = true;
bool cancelled_before_initialize{};

struct Request {
    std::uint64_t sequence;
    std::string mode;
};

std::string JsonLocked() {
    const bool complete = outcome == "complete";
    return nlohmann::json{{"version", 1}, {"status", outcome},
                         {"requestSequence", request_sequence},
                         {"level", outcome == "controller" ? 1 : 8},
                         {"warmupSeconds", 5}, {"durationSeconds", 30},
                         {"frames", measurement.frames},
                         {"elapsedSeconds", measurement.Seconds()},
                         {"averageFps", complete ? measurement.AverageFps() : 0.},
                         {"presentMode", present_mode}, {"reason", reason},
                         {"metric", "unique-guest-frames-presented"}}.dump();
}

void WriteJsonLocked(const std::filesystem::path& destination) {
    const auto temporary = destination.string() + ".tmp";
    std::filesystem::remove(temporary);
    // Exclusive creation does not follow links; rename publishes a whole record.
    FILE* file = std::fopen(temporary.c_str(), "wx");
    if (!file) throw std::runtime_error("Cannot create Succi status/result");
    const auto json = JsonLocked();
    const bool written = std::fwrite(json.data(), 1, json.size(), file) == json.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed || std::rename(temporary.c_str(), destination.string().c_str()) != 0) {
        std::remove(temporary.c_str());
        throw std::runtime_error("Cannot publish Succi status/result");
    }
}

void WriteStatusLocked() {
    if (status_path.empty()) return;
    try {
        WriteJsonLocked(status_path);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "LUMEN_SUCCI_STATUS_ERROR %s\n", error.what());
    }
}

std::optional<Request> ReadRequestLocked() {
    namespace fs = std::filesystem;
    if (request_path.empty()) return std::nullopt;
    std::error_code error;
    const auto status = fs::symlink_status(request_path, error);
    if (error || !fs::is_regular_file(status)) return std::nullopt;
    // The admitted guest publishes via rename. Bound the read independently of
    // stat size; invalid, linked, stale and truncated commands never unlock FPS.
    std::ifstream input(request_path, std::ios::binary);
    std::array<char, 65> buffer{};
    input.read(buffer.data(), buffer.size());
    const auto count = input.gcount();
    if (input.bad() || count <= 0 || count >= static_cast<std::streamsize>(buffer.size()))
        return std::nullopt;
    std::string_view text(buffer.data(), static_cast<std::size_t>(count));
    const auto space = text.find(' ');
    if (space == std::string_view::npos || space == 0 || text.back() != '\n')
        return std::nullopt;
    std::uint64_t sequence{};
    const auto parsed = std::from_chars(text.data(), text.data() + space, sequence);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + space || !sequence)
        return std::nullopt;
    const auto mode = text.substr(space + 1, text.size() - space - 2);
    if (mode != "graphics" && mode != "controller" && mode != "menu") return std::nullopt;
    return Request{sequence, std::string(mode)};
}

void CancelMeasurementLocked() {
    if (outcome != "warmup" && outcome != "measuring") return;
    outcome = "cancelled";
    reason = "Benchmark interrupted before completion";
    uncapped.store(false, std::memory_order_release);
    WriteStatusLocked();
}

void ApplyRequestLocked(const Request& request, bool allow_graphics) {
    request_sequence = request.sequence;
    measurement = {};
    reason.clear();
    uncapped.store(false, std::memory_order_release);
    if (request.mode == "graphics") {
        try {
            std::filesystem::remove(result_path);
            std::filesystem::remove(result_path.string() + ".tmp");
            // Keep the last selected mode until the presenter changes policy.
            // A direct retry may already use the same unthrottled swapchain.
            outcome = allow_graphics && ui_active ? "warmup" : "cancelled";
            if (outcome == "warmup") uncapped.store(true, std::memory_order_release);
            else reason = "Benchmark request interrupted before presentation";
        } catch (const std::exception& error) {
            outcome = "unavailable";
            reason = error.what();
        }
    } else {
        outcome = request.mode;
    }
    std::fprintf(stderr, "LUMEN_SUCCI_TEST_REQUEST sequence=%llu mode=%s status=%s\n",
                 static_cast<unsigned long long>(request_sequence), request.mode.c_str(),
                 outcome.c_str());
    WriteStatusLocked();
}

void ConsumeRequestLocked(bool allow_graphics) {
    const auto request = ReadRequestLocked();
    if (request && request->sequence > request_sequence)
        ApplyRequestLocked(*request, allow_graphics);
}
} // namespace

void ValidateLaunch(SessionOptions& options, const std::filesystem::path& private_files) {
    namespace fs = std::filesystem;
    options.succi_benchmark = false;
    if (private_files.empty() || !private_files.is_absolute())
        throw std::runtime_error("Missing private demo storage");
    const auto private_root = fs::canonical(private_files);
    const auto expected = private_root / "benchmarks/succi-v1/roms/SUCC00001";
    if (!fs::is_directory(fs::symlink_status(expected)) || fs::canonical(expected) != expected ||
        fs::canonical(options.root) != expected)
        throw std::runtime_error("The Succi demo is restricted to its bundled private payload");
    const auto expected_data = private_root / "RuntimeData/succi-benchmark-v1";
    if (!fs::is_directory(fs::symlink_status(expected_data)) ||
        fs::canonical(expected_data) != expected_data || fs::canonical(options.data) != expected_data)
        throw std::runtime_error("The Succi demo requires its isolated private runtime data");
    if (fs::exists(fs::symlink_status(expected / "succi-controller-test")))
        throw std::runtime_error("Legacy Succi controller marker is not supported");
    const auto marker_path = expected / "succi-benchmark.json";
    if (!fs::is_regular_file(fs::symlink_status(marker_path)) || fs::file_size(marker_path) > 4096)
        throw std::runtime_error("Missing or invalid Succi demo marker");
    std::ifstream stream(marker_path);
    const auto marker = nlohmann::json::parse(stream);
    if (marker.at("version") != 2 || marker.at("mode") != "succi-demo-v2" ||
        marker.at("level") != 8 || marker.at("controllerLevel") != 1 ||
        marker.at("benchmarkOnly") != true || marker.at("warmupSeconds") != 5 ||
        marker.at("measurementSeconds") != 30)
        throw std::runtime_error("Unsupported Succi demo protocol");
    options.eboot_digest = marker.at("ebootSha256").get<std::string>();
    if (options.eboot_digest.size() != 64 ||
        !std::all_of(options.eboot_digest.begin(), options.eboot_digest.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
        throw std::runtime_error("Invalid Succi demo executable digest");
    options.succi_benchmark = true;
}

void Initialize(const std::filesystem::path& root, const std::filesystem::path& data) {
    std::scoped_lock lock{mutex};
    // /app0 stays read-only for the guest. Its writable /data mount is the
    // session's GameDataDir (data/data), not a title-ID subdirectory.
    const auto guest_data = std::filesystem::canonical(data) / "data";
    const auto existing = std::filesystem::symlink_status(guest_data);
    if (std::filesystem::exists(existing) && !std::filesystem::is_directory(existing))
        throw std::runtime_error("Invalid Succi guest data directory");
    std::filesystem::create_directories(guest_data);
    if (std::filesystem::canonical(guest_data) != guest_data)
        throw std::runtime_error("Linked Succi guest data directory");
    result_path = root / "succi-benchmark-result.json";
    request_path = guest_data / "succi-test-request";
    status_path = root / "succi-test-status.json";
    for (const auto& path : {result_path, request_path, status_path}) {
        std::filesystem::remove(path);
        std::filesystem::remove(path.string() + ".tmp");
    }
    measurement = {};
    request_sequence = 0;
    next_poll = {};
    present_mode = "unknown";
    if (cancelled_before_initialize) ui_active = false;
    outcome = "menu";
    reason.clear();
    uncapped.store(false, std::memory_order_release);
    enabled.store(true, std::memory_order_release);
    WriteStatusLocked();
}

bool Enabled() { return enabled.load(std::memory_order_acquire); }
bool Uncapped() { return uncapped.load(std::memory_order_acquire); }

void PollRequest() {
    if (Enabled()) PollRequest(Clock::now());
}

void PollRequest(Clock::time_point now) {
    if (!Enabled()) return;
    std::scoped_lock lock{mutex};
    if (now < next_poll) return;
    next_poll = now + std::chrono::milliseconds(100);
    ConsumeRequestLocked(true);
}

void SetPresentMode(const char* mode) {
    if (!Uncapped()) return;
    std::scoped_lock lock{mutex};
    if (Uncapped()) present_mode = mode;
}

void OnGuestPresented() {
    if (Uncapped()) OnGuestPresented(Clock::now());
}

void OnGuestPresented(Clock::time_point now) {
    if (!Uncapped()) return;
    std::scoped_lock lock{mutex};
    if (!Uncapped()) return;
    const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               now.time_since_epoch()).count();
    const bool complete = measurement.Present(timestamp);
    if (measurement.phase == Measurement::Phase::Measuring) outcome = "measuring";
    if (complete) {
        outcome = "complete";
        try {
            WriteJsonLocked(result_path);
            std::fprintf(stderr, "LUMEN_SUCCI_BENCHMARK_RESULT %s\n", JsonLocked().c_str());
        } catch (const std::exception& error) {
            outcome = "unavailable";
            reason = error.what();
        }
        uncapped.store(false, std::memory_order_release);
        WriteStatusLocked();
    }
}

void PresentationFailed() {
    if (!Uncapped()) return;
    std::scoped_lock lock{mutex};
    if (!Uncapped()) return;
    outcome = "unavailable";
    reason = "A guest frame could not be presented; run the benchmark again";
    uncapped.store(false, std::memory_order_release);
    WriteStatusLocked();
}

void Cancel() {
    std::scoped_lock lock{mutex};
    if (!Enabled()) {
        cancelled_before_initialize = true;
        return;
    }
    // Consume a graphics command that the guest published before cancellation,
    // even when the present loop has not polled it yet. Later choices may retry.
    ConsumeRequestLocked(false);
    CancelMeasurementLocked();
}

void SetActive(bool active) {
    std::scoped_lock lock{mutex};
    if (ui_active == active) return;
    ui_active = active;
    if (!Enabled()) return;
    // On resume also discard the last command issued while the UI was inactive.
    ConsumeRequestLocked(false);
    CancelMeasurementLocked();
}

std::string StatusJson() {
    std::scoped_lock lock{mutex};
    return JsonLocked();
}
} // namespace Lumen::Gen4::SucciBenchmark
