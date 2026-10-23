// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_driver_pipeline_cache_storage.h"

namespace Vulkan {

class Instance;

class DriverPipelineCache {
public:
    explicit DriverPipelineCache(const Instance& instance);
    ~DriverPipelineCache();

    vk::PipelineCache operator*() const {
        return *cache;
    }

    [[nodiscard]] auto Lock() {
        return access.Lock();
    }

    void MarkDirty();
    void Stop();

private:
    bool Checkpoint() noexcept;

    vk::Device device;
    vk::UniquePipelineCache cache;
    DriverCacheStorage::Identity identity;
    std::filesystem::path path;
    std::size_t last_size{};
    std::uint64_t last_hash{};
    DriverCacheStorage::CacheAccess access;
    std::unique_ptr<DriverCacheStorage::CheckpointWorker> worker;
};

} // namespace Vulkan
