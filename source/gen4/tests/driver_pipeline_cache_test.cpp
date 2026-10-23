// SPDX-License-Identifier: GPL-2.0-or-later

#include <future>
#include <iostream>
#include <stdexcept>
#include "video_core/renderer_vulkan/vk_driver_pipeline_cache_storage.h"

namespace Cache = Vulkan::DriverCacheStorage;
using namespace std::chrono_literals;

namespace {

void Require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

std::vector<std::uint8_t> Blob(const Cache::Identity& identity) {
    std::vector<std::uint8_t> data(96, 0x56);
    Cache::WriteLE(data, 0, 32, 4);
    Cache::WriteLE(data, 4, 1, 4);
    Cache::WriteLE(data, 8, identity.vendor_id, 4);
    Cache::WriteLE(data, 12, identity.device_id, 4);
    std::copy(identity.uuid.begin(), identity.uuid.end(), data.begin() + 16);
    return data;
}

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void WriteFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    Require(bool(file), "fixture write failed");
}

void TestStorage(const std::filesystem::path& directory) {
    const Cache::Identity identity{0x5143, 0x44050a31, 0x6801000,
                                   {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}};
    const auto path = directory / "driver" / "test.bin";
    const auto data = Blob(identity);
    Require(Cache::Hash({}) == 0xcbf29ce484222325ULL, "FNV offset differs");
    const std::array<std::uint8_t, 5> hello{'h', 'e', 'l', 'l', 'o'};
    Require(Cache::Hash(hello) == 0xa430d84680aabd0bULL, "FNV known vector differs");
    Require(Cache::Load(path, identity).empty(), "missing file accepted");
    Require(Cache::Save(path, identity, data, Cache::Hash(data)), "valid write rejected");
    Require(Cache::Load(path, identity) == data, "round trip mismatch");
    const auto original = ReadFile(path);
    Require(original.size() == 32 + data.size(), "container size mismatch");
    Require(Cache::ReadLE(original, 12, 4) == 0, "reserved field not zero");

    // Each Vulkan header corruption has a matching outer checksum, so this tests
    // the identity/header validation independently of the checksum rejection.
    for (const auto offset : {0u, 8u, 16u, 24u, 32u, 36u, 40u, 44u, 48u, 80u}) {
        auto corrupt = original;
        corrupt[offset] ^= 0x80;
        if (offset >= 32 && offset < 64) {
            Cache::WriteLE(corrupt, 24, Cache::Hash(std::span{corrupt}.subspan(32)), 8);
        }
        WriteFile(path, corrupt);
        Require(Cache::Load(path, identity).empty(), "corrupt cache accepted");
    }
    for (const auto size : {0u, 31u, 32u, 63u, 127u, 129u}) {
        auto corrupt = original;
        corrupt.resize(size);
        WriteFile(path, corrupt);
        Require(Cache::Load(path, identity).empty(), "truncated/extra cache accepted");
    }
    WriteFile(path, original);
    std::filesystem::resize_file(path, Cache::MaxBlobSize + Cache::HeaderSize + 1);
    Require(Cache::Load(path, identity).empty(), "oversized cache accepted");
    WriteFile(path, original);
    auto wrong_driver = identity;
    ++wrong_driver.driver_version;
    Require(Cache::Load(path, wrong_driver).empty(), "different driver accepted");
    Require(!Cache::ValidateBlob({}, identity), "empty Vulkan data accepted");
    Require(!Cache::Save(path, identity, std::span{data}.first(31), 0),
            "undersized Vulkan data saved");
    Require(ReadFile(path) == original, "invalid data damaged old cache");

    // An unwritable temporary path must leave the last complete cache intact.
    auto temporary = path;
    temporary += ".tmp";
    std::filesystem::create_directory(temporary);
    auto updated = data;
    updated.back() ^= 1;
    Require(!Cache::Save(path, identity, updated, Cache::Hash(updated)),
            "failed temporary write accepted");
    Require(ReadFile(path) == original, "failed write damaged old cache");
    std::filesystem::remove(temporary);
    Require(Cache::Save(path, identity, updated, Cache::Hash(updated)),
            "atomic replace failed");
    Require(Cache::Load(path, identity) == updated, "replacement data mismatch");
    const auto blocked = directory / "plain-file";
    WriteFile(blocked, {});
    Require(!Cache::Save(blocked / "cache.bin", identity, data, Cache::Hash(data)),
            "invalid parent accepted");
    const auto target_directory = directory / "existing-directory";
    std::filesystem::create_directory(target_directory);
    Require(!Cache::Save(target_directory, identity, data, Cache::Hash(data)),
            "failed rename accepted");
    Require(std::filesystem::is_directory(target_directory), "failed rename damaged target");
}

void TestWorker() {
    std::atomic<unsigned> calls{};
    {
        Cache::CheckpointWorker worker{[&] { ++calls; return true; }, 24h};
        worker.Stop();
        Require(calls == 0, "clean cache checkpointed");
    }
    {
        Cache::CheckpointWorker worker{[&] { ++calls; return true; }, 24h};
        worker.MarkDirty();
        worker.Stop();
        Require(calls == 1, "stop lost final checkpoint");
        worker.Stop();
        Require(calls == 1, "second stop repeated checkpoint");
    }

    std::mutex mutex;
    std::condition_variable condition;
    unsigned checkpoints{};
    bool release_first{};
    {
        Cache::CheckpointWorker worker{[&] {
            std::unique_lock lock{mutex};
            ++checkpoints;
            condition.notify_all();
            if (checkpoints == 1) {
                condition.wait(lock, [&] { return release_first; });
            }
            return true;
        }, 1ms};
        worker.MarkDirty();
        {
            std::unique_lock lock{mutex};
            const bool entered = condition.wait_for(lock, 5s, [&] { return checkpoints == 1; });
            if (!entered) {
                release_first = true;
                lock.unlock();
                condition.notify_all();
                Require(false, "periodic checkpoint missing");
            }
            worker.MarkDirty();
            release_first = true;
        }
        condition.notify_all();
        {
            std::unique_lock lock{mutex};
            Require(condition.wait_for(lock, 5s, [&] { return checkpoints == 2; }),
                    "dirty generation during checkpoint was lost");
        }
        worker.Stop();
        Require(checkpoints == 2, "completed generation repeated");
    }
    checkpoints = 0;
    {
        Cache::CheckpointWorker worker{[&] {
            std::scoped_lock lock{mutex};
            ++checkpoints;
            condition.notify_all();
            return checkpoints > 1;
        }, 1ms};
        worker.MarkDirty();
        {
            std::unique_lock lock{mutex};
            Require(condition.wait_for(lock, 5s, [&] { return checkpoints >= 2; }),
                    "failed checkpoint was not retried");
        }
        worker.Stop();
        Require(checkpoints == 2, "successful retry left dirty generation");
    }
}

void TestCacheAccess() {
    Cache::CacheAccess access;
    auto creation_lock = access.Lock();
    std::promise<void> checkpoint_requested;
    std::atomic<bool> snapshot_started{};
    Cache::CheckpointWorker worker{[&] {
        checkpoint_requested.set_value();
        auto snapshot_lock = access.Lock();
        snapshot_started = true;
        return true;
    }, 24h};
    worker.MarkDirty();
    auto stopping = std::async(std::launch::async, [&] { worker.Stop(); });
    const bool requested = checkpoint_requested.get_future().wait_for(5s) ==
                           std::future_status::ready;
    const bool overlapped = snapshot_started;
    creation_lock.unlock();
    Require(requested, "stop did not wake worker promptly");
    Require(!overlapped, "snapshot overlaps pipeline creation");
    Require(stopping.wait_for(5s) == std::future_status::ready, "checkpoint stop did not join");
    stopping.get();
    Require(snapshot_started, "snapshot missing after creation released cache");
}

} // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("lumen-driver-cache-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(directory);
        TestStorage(directory);
        TestWorker();
        TestCacheAccess();
        std::filesystem::remove_all(directory);
        std::cout << "driver pipeline cache storage/worker/exclusion gates PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(directory);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
