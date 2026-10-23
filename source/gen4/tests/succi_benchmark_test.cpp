// SPDX-License-Identifier: GPL-2.0-only
#include "lumen/succi_benchmark.h"
#include "lumen/session.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>

using namespace Lumen::Gen4;
using namespace Lumen::Gen4::SucciBenchmark;
namespace fs = std::filesystem;

int main() {
    const auto require = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    };
    const auto temporary = fs::temp_directory_path() / ("lumen-succi-benchmark-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(temporary);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{temporary};
    try {
        Measurement fast;
        fast.Present(0);
        for (std::int64_t i = 1; i <= 600; ++i) fast.Present(i * 1'000'000'000 / 120);
        require(fast.phase == Measurement::Phase::Measuring && fast.frames == 0,
                "Warmup frames must not enter the measurement");
        for (std::int64_t i = 1; i <= 3600; ++i)
            fast.Present(Measurement::WarmupNs + i * 1'000'000'000 / 120);
        require(fast.phase == Measurement::Phase::Complete && fast.frames == 3600 &&
                fast.Seconds() == 30. && std::abs(fast.AverageFps() - 120.) < 0.0001,
                "A real 120 FPS trace must remain 120 FPS");
        fast.Present(100'000'000'000);
        require(fast.frames == 3600 && fast.Seconds() == 30., "Completed result must stay fixed");

        Measurement uneven;
        uneven.Present(100);
        uneven.Present(Measurement::WarmupNs + 100);
        uneven.Present(Measurement::WarmupNs + Measurement::DurationNs - 1);
        require(uneven.phase == Measurement::Phase::Measuring, "Short run must not finish");
        uneven.Present(Measurement::WarmupNs + Measurement::DurationNs + 10'000'000'100);
        require(uneven.phase == Measurement::Phase::Complete && uneven.Seconds() == 40. &&
                std::abs(uneven.AverageFps() - 0.05) < 0.0001,
                "Average must use actual elapsed time, including stalls");

        const auto root = temporary / "benchmarks/succi-v1/roms/SUCC00001";
        const auto data = temporary / "RuntimeData/succi-benchmark-v1";
        fs::create_directories(root);
        fs::create_directories(data);
        const auto marker = root / "succi-benchmark.json";
        const nlohmann::json manifest{{"version",2},{"mode","succi-demo-v2"},
            {"level",8},{"controllerLevel",1},{"benchmarkOnly",true},{"warmupSeconds",5},{"measurementSeconds",30},
            {"ebootSha256",std::string(64,'a')}};
        std::ofstream(marker) << manifest.dump();
        SessionOptions options;
        options.root = root;
        options.data = data;
        ValidateLaunch(options, temporary);
        require(options.succi_benchmark, "Private bundled benchmark must pass admission");
        auto rejects = [&](const fs::path& candidate) {
            options.root = candidate;
            try { ValidateLaunch(options, temporary); return false; }
            catch (const std::exception&) { return true; }
        };
        options.data = root;
        require(rejects(root), "The demo must not use an arbitrary runtime data directory");
        options.data = data;
        const auto controller_marker = root / "succi-controller-test";
        std::ofstream(controller_marker) << "v1\n";
        require(rejects(root), "Legacy controller payload must not be admitted");
        fs::remove(controller_marker);
        fs::create_symlink(root / "missing-controller-target", controller_marker);
        require(rejects(root), "Dangling legacy controller markers must not be admitted");
        fs::remove(controller_marker);
        fs::create_directory(temporary / "external-game");
        require(rejects(temporary / "external-game"), "Ordinary games must not unlock");
        auto wrong = manifest;
        wrong["level"] = 2;
        std::ofstream(marker) << wrong.dump();
        require(rejects(root), "Other levels must not unlock");
        wrong = manifest;
        wrong["version"] = 1;
        wrong["mode"] = "succi-benchmark-v1";
        std::ofstream(marker) << wrong.dump();
        require(rejects(root), "Legacy auto-start payload must not be admitted");
        std::ofstream(temporary / "external-marker") << manifest.dump();
        fs::remove(marker);
        fs::create_symlink(temporary / "external-marker", marker);
        require(rejects(root), "Linked markers must not unlock");

        fs::remove(marker);
        std::ofstream(marker) << manifest.dump();
        const auto result = root / "succi-benchmark-result.json";
        const auto request = data / "data/succi-test-request";
        const auto status_file = root / "succi-test-status.json";
        fs::create_directories(request.parent_path());
        std::ofstream(result) << "stale";
        std::ofstream(request) << "999 graphics\n";
        SetActive(false);
        SetActive(true);
        Initialize(root, data);
        require(!fs::exists(result) && !fs::exists(request) && !Uncapped(),
                "Demo initialization must discard stale state without unlocking FPS");
        const auto at = [](std::int64_t ns) { return Clock::time_point(std::chrono::nanoseconds(ns)); };
        const auto status = [] { return nlohmann::json::parse(StatusJson()); };
        auto poll_time = std::int64_t{};
        const auto publish = [&](const std::string& text) {
            std::ofstream(request.string() + ".tmp") << text;
            fs::rename(request.string() + ".tmp", request);
        };
        const auto poll = [&] {
            poll_time += 100'000'000;
            PollRequest(at(poll_time));
        };
        require(status()["status"] == "menu", "The demo must begin in its in-game menu");
        std::ofstream(root / "succi-test-request") << "1 graphics\n";
        poll();
        require(!Uncapped(), "Commands must come from writable /data, never guest read-only /app0");
        fs::remove(root / "succi-test-request");
        for (const auto* invalid : {"0 graphics\n", "1 unknown\n", "1 graphics", "1 graphics\nextra",
                                    "18446744073709551616 graphics\n", "1  graphics\n"}) {
            publish(invalid);
            poll();
            require(!Uncapped() && status()["requestSequence"] == 0,
                    "Invalid commands must not unlock or advance the request sequence");
        }
        fs::remove(request);
        std::ofstream(root / "external-request") << "1 graphics\n";
        fs::create_symlink(root / "external-request", request);
        poll();
        require(!Uncapped(), "Linked requests must not unlock FPS");
        fs::remove(request);
        publish("1 controller\n");
        poll();
        require(!Uncapped() && status()["status"] == "controller" && status()["level"] == 1,
                "Manual controller mode must use ordinary pacing and level one");
        publish("2 graphics\n");
        PollRequest(at(poll_time + 50'000'000));
        require(!Uncapped(), "Filesystem requests must be polled at most every 100 ms");
        poll();
        require(Uncapped() && status()["status"] == "warmup" && status()["level"] == 8,
                "Only a fresh graphics choice may start the level-eight benchmark");
        SetPresentMode("Mailbox");
        OnGuestPresented(at(0));
        OnGuestPresented(at(Measurement::WarmupNs));
        for (std::int64_t i = 1; i <= 3600; ++i)
            OnGuestPresented(at(Measurement::WarmupNs + i * 1'000'000'000 / 120));
        auto completed = status();
        require(!Uncapped() && completed["status"] == "complete" &&
                completed["requestSequence"] == 2 && completed["frames"] == 3600 &&
                completed["averageFps"] == 120. && completed["level"] == 8,
                "Completed graphics result must preserve its sequence, level and uncapped FPS");
        std::ifstream result_stream(result);
        require(nlohmann::json::parse(result_stream) == completed,
                "The guest and JNI must receive the same complete result");
        Cancel();
        SetPresentMode("Fifo");
        require(status() == completed, "Cancellation or restored pacing must preserve a completed result");
        publish("3 menu\n");
        poll();
        require(!Uncapped() && status()["status"] == "menu", "Returning to menu must keep normal pacing");
        publish("4 graphics\n");
        poll();
        require(Uncapped() && !fs::exists(result) && status()["frames"] == 0,
                "A repeated graphics test must reset all prior measurement state");
        publish("3 controller\n");
        poll();
        require(Uncapped() && status()["requestSequence"] == 4, "Stale commands must be ignored");
        OnGuestPresented(at(100'000'000'000));
        PresentationFailed();
        require(!Uncapped() && status()["status"] == "unavailable" && !fs::exists(result),
                "Failed presentation must invalidate rather than publish a result");
        std::ifstream failure_stream(status_file);
        require(nlohmann::json::parse(failure_stream)["status"] == "unavailable",
                "The guest must receive a failure acknowledgement instead of waiting forever");
        publish("5 controller\n");
        poll();
        Cancel();
        require(status()["status"] == "controller", "Cancel must not alter manual controller mode");
        publish("6 graphics\n");
        Cancel();
        poll();
        require(!Uncapped() && status()["status"] == "cancelled" && status()["requestSequence"] == 6,
                "Cancellation must consume a graphics command not yet polled");
        publish("7 graphics\n");
        poll();
        require(Uncapped(), "A later explicit choice may retry after cancellation");
        SetActive(false);
        require(!Uncapped() && status()["status"] == "cancelled", "Backgrounding must cancel an active run");
        publish("8 graphics\n");
        poll();
        require(!Uncapped() && status()["requestSequence"] == 8, "Background requests cannot unlock FPS");
        publish("9 graphics\n");
        SetActive(true);
        poll();
        require(!Uncapped() && status()["requestSequence"] == 9,
                "Resume must discard a request published while inactive");
        publish("10 graphics\n");
        poll();
        SetActive(true);
        require(Uncapped(), "Repeated focus callbacks must not cancel a newly chosen test");
        Cancel();
        std::ifstream cancelled_stream(status_file);
        require(!fs::exists(result) && nlohmann::json::parse(cancelled_stream)["status"] == "cancelled",
                "Cancellation must publish only status, never a benchmark result");
        SetActive(false);
        Initialize(root, data);
        publish("1 graphics\n");
        poll();
        require(!Uncapped(), "An inactive UI before initialization must not permit a later unlock");
        std::puts("Succi demo: 120 FPS, warmup, duration, stalls, admission, deferred menu/controller, repeated runs and lifecycle races passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
