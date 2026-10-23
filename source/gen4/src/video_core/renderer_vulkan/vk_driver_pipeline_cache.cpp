// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "video_core/renderer_vulkan/vk_driver_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace Vulkan {

DriverPipelineCache::DriverPipelineCache(const Instance& instance)
    : device{instance.GetDevice()},
      identity{instance.GetVendorID(), instance.GetDeviceID(), instance.GetDriverVersion(), {}} {
    const auto uuid = instance.GetPipelineCacheUUID();
    std::copy(uuid.begin(), uuid.end(), identity.uuid.begin());
    std::vector<std::uint8_t> initial_data;
    if (EmulatorSettings.IsPipelineCacheEnabled()) {
        auto filename = fmt::format("{:x}-{:x}-{:x}-", identity.vendor_id, identity.device_id,
                                    identity.driver_version);
        for (const auto byte : identity.uuid) {
            filename += fmt::format("{:02x}", byte);
        }
        path = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
               Common::ElfInfo::Instance().GameSerial() / "driver" / (filename + ".bin");
        try {
            initial_data = DriverCacheStorage::Load(path, identity);
        } catch (const std::exception& error) {
            LOG_WARNING(Render_Vulkan, "Driver pipeline cache load failed: {}", error.what());
        }
    }

    vk::PipelineCacheCreateInfo info{
        .initialDataSize = initial_data.size(),
        .pInitialData = initial_data.data(),
    };
    auto [result, created_cache] = device.createPipelineCacheUnique(info);
    if (result != vk::Result::eSuccess && !initial_data.empty()) {
        LOG_WARNING(Render_Vulkan, "Driver rejected pipeline cache; retrying empty: {}",
                    vk::to_string(result));
        auto [retry_result, retry_cache] = device.createPipelineCacheUnique({});
        result = retry_result;
        created_cache = std::move(retry_cache);
        initial_data.clear();
    }
    ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(result));
    cache = std::move(created_cache);
    last_size = initial_data.size();
    last_hash = DriverCacheStorage::Hash(initial_data);
    if (!path.empty()) {
        LOG_INFO(Render_Vulkan, "Driver pipeline cache loaded bytes={} path={}", last_size,
                 path.string());
        worker = std::make_unique<DriverCacheStorage::CheckpointWorker>([this] {
            Common::SetCurrentThreadName("Lumen:DriverCacheIO");
            return Checkpoint();
        });
    }
}

DriverPipelineCache::~DriverPipelineCache() {
    Stop();
}

void DriverPipelineCache::MarkDirty() {
    if (worker) {
        worker->MarkDirty();
    }
}

void DriverPipelineCache::Stop() {
    if (worker) {
        worker->Stop();
    }
}

bool DriverPipelineCache::Checkpoint() noexcept {
    try {
        const auto start = std::chrono::steady_clock::now();
        std::vector<std::uint8_t> data;
        vk::Result result;
        {
            auto lock = Lock();
            std::size_t size{};
            result = device.getPipelineCacheData(*cache, &size, nullptr);
            if (result != vk::Result::eSuccess || size < DriverCacheStorage::HeaderSize ||
                size > DriverCacheStorage::MaxBlobSize) {
                return false;
            }
            data.resize(size);
            result = device.getPipelineCacheData(*cache, &size, data.data());
            if ((result != vk::Result::eSuccess && result != vk::Result::eIncomplete) ||
                size > data.size()) {
                return false;
            }
            data.resize(size);
        }
        const auto hash = DriverCacheStorage::Hash(data);
        if (data.size() != last_size || hash != last_hash) {
            if (!DriverCacheStorage::Save(path, identity, data, hash)) {
                LOG_WARNING(Render_Vulkan, "Driver pipeline cache save failed path={}",
                            path.string());
                return false;
            }
            last_size = data.size();
            last_hash = hash;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            LOG_INFO(Render_Vulkan, "Driver pipeline cache saved bytes={} elapsed_ms={}",
                     last_size, elapsed.count());
        }
        return result == vk::Result::eSuccess;
    } catch (const std::exception& error) {
        LOG_WARNING(Render_Vulkan, "Driver pipeline cache checkpoint failed: {}", error.what());
        return false;
    }
}

} // namespace Vulkan
