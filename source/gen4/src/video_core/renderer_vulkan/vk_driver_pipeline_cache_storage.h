// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <span>
#include <vector>
#include "common/polyfill_thread.h"

namespace Vulkan::DriverCacheStorage {

constexpr std::size_t HeaderSize = 32;
constexpr std::size_t MaxBlobSize = 128 * 1024 * 1024;
constexpr std::array<std::uint8_t, 8> Magic{'L', 'M', 'N', 'V', 'K', 'C', 1, 0};

struct Identity {
    std::uint32_t vendor_id;
    std::uint32_t device_id;
    std::uint32_t driver_version;
    std::array<std::uint8_t, 16> uuid;
};

inline std::uint64_t ReadLE(std::span<const std::uint8_t> data, std::size_t offset,
                            std::size_t size) {
    std::uint64_t value{};
    for (std::size_t i = 0; i < size; ++i) {
        value |= std::uint64_t{data[offset + i]} << (8 * i);
    }
    return value;
}

inline void WriteLE(std::span<std::uint8_t> data, std::size_t offset, std::uint64_t value,
                    std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        data[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

inline std::uint64_t Hash(std::span<const std::uint8_t> data) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const auto byte : data) {
        hash = (hash ^ byte) * 0x100000001b3ULL;
    }
    return hash;
}

inline bool ValidateBlob(std::span<const std::uint8_t> data, const Identity& identity) {
    return data.size() >= HeaderSize && data.size() <= MaxBlobSize &&
           ReadLE(data, 0, 4) == HeaderSize && ReadLE(data, 4, 4) == 1 &&
           ReadLE(data, 8, 4) == identity.vendor_id &&
           ReadLE(data, 12, 4) == identity.device_id &&
           std::equal(identity.uuid.begin(), identity.uuid.end(), data.begin() + 16);
}

inline std::vector<std::uint8_t> Load(const std::filesystem::path& path,
                                     const Identity& identity) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    const auto length = file.tellg();
    if (!file || length < static_cast<std::streamoff>(2 * HeaderSize) ||
        length > static_cast<std::streamoff>(HeaderSize + MaxBlobSize)) {
        return {};
    }
    file.seekg(0);
    std::array<std::uint8_t, HeaderSize> header{};
    file.read(reinterpret_cast<char*>(header.data()), header.size());
    const auto payload_size = ReadLE(header, 16, 8);
    if (!file || !std::equal(Magic.begin(), Magic.end(), header.begin()) ||
        ReadLE(header, 8, 4) != identity.driver_version ||
        payload_size != static_cast<std::uint64_t>(length) - HeaderSize) {
        return {};
    }
    std::vector<std::uint8_t> data(payload_size);
    file.read(reinterpret_cast<char*>(data.data()), data.size());
    if (!file || !ValidateBlob(data, identity) || Hash(data) != ReadLE(header, 24, 8)) {
        return {};
    }
    return data;
}

inline bool Save(const std::filesystem::path& path, const Identity& identity,
                 std::span<const std::uint8_t> data, std::uint64_t hash) {
    if (!ValidateBlob(data, identity)) {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    std::array<std::uint8_t, HeaderSize> header{};
    std::copy(Magic.begin(), Magic.end(), header.begin());
    WriteLE(header, 8, identity.driver_version, 4);
    WriteLE(header, 16, data.size(), 8);
    WriteLE(header, 24, hash, 8);
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
    file.write(reinterpret_cast<const char*>(header.data()), header.size());
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    file.close();
    if (!file) {
        return false;
    }
    std::filesystem::rename(temporary, path, error);
    return !error;
}

// The cache snapshot and pipeline creation share this lock. File I/O is outside it.
class CacheAccess {
public:
    [[nodiscard]] std::unique_lock<std::mutex> Lock() {
        return std::unique_lock{mutex};
    }

private:
    std::mutex mutex;
};

class CheckpointWorker {
public:
    explicit CheckpointWorker(std::function<bool()> callback_,
                              std::chrono::milliseconds interval_ = std::chrono::milliseconds{5000})
        : callback{std::move(callback_)}, interval{interval_},
          thread{[this](std::stop_token token) { Run(token); }} {}

    ~CheckpointWorker() {
        Stop();
    }

    void MarkDirty() {
        generation.fetch_add(1, std::memory_order_release);
    }

    void Stop() {
        std::scoped_lock stop_lock{stop_mutex};
        if (!thread.joinable()) {
            return;
        }
        {
            std::scoped_lock lock{wait_mutex};
            thread.request_stop();
        }
        condition.notify_all();
        thread.join();
    }

private:
    void CheckpointPending() {
        const auto current = generation.load(std::memory_order_acquire);
        if (current != completed_generation && callback()) {
            completed_generation = current;
        }
    }

    void Run(std::stop_token token) {
        while (!token.stop_requested()) {
            {
                std::unique_lock lock{wait_mutex};
                condition.wait_until(lock, std::chrono::steady_clock::now() + interval,
                                     [&] { return token.stop_requested(); });
            }
            if (!token.stop_requested()) {
                CheckpointPending();
            }
        }
        CheckpointPending();
    }

    std::function<bool()> callback;
    const std::chrono::milliseconds interval;
    std::atomic<std::uint64_t> generation{};
    std::uint64_t completed_generation{};
    std::mutex wait_mutex;
    std::mutex stop_mutex;
    std::condition_variable condition;
    std::jthread thread;
};

} // namespace Vulkan::DriverCacheStorage
