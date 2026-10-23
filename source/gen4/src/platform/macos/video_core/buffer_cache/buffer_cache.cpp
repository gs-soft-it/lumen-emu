// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <vk_mem_alloc.h>

#include <algorithm>
#include <chrono>
#include "common/alignment.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_packet.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/buffer_pending_write_resolve.h"
#include "video_core/buffer_cache/buffer_copy_resolve.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/buffer_cache/vertex_buffer_bind_path.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace VideoCore {

namespace {
void CopyCpuOwnedMemory(Core::MemoryManager* memory, VAddr address, u8* data, u64 size) {
#ifdef LUMEN_GEN4_DIRECT_FEX
    memory->CopySparseMemoryFromBacking(address, data, size);
#else
    memory->CopySparseMemory(address, data, size);
#endif
}

bool CompareCpuOwnedMemory(Core::MemoryManager* memory, VAddr address, const u8* data, u64 size) {
#ifdef LUMEN_GEN4_DIRECT_FEX
    return memory->CompareSparseMemoryFromBacking(address, data, size);
#else
    return memory->CompareSparseMemory(address, data, size);
#endif
}
} // namespace

static constexpr size_t DataShareBufferSize = 64_KB;
static constexpr size_t StagingBufferSize = 512_MB;
static constexpr size_t DownloadBufferSize = 32_MB;
static constexpr size_t UboStreamBufferSize = 64_MB;
static constexpr size_t DeviceBufferSize = 128_MB;

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         AmdGpu::Liverpool* liverpool_, TextureCache& texture_cache_,
                         PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_}, tracker{tracker},
      fault_manager{instance, scheduler, *this, CACHING_PAGEBITS, CACHING_NUMPAGES},
      staging_buffer{instance, scheduler, MemoryUsage::Upload, StagingBufferSize},
      stream_buffer{instance, scheduler, MemoryUsage::Stream, UboStreamBufferSize},
      download_buffer{instance, scheduler, MemoryUsage::Download, DownloadBufferSize},
      device_buffer{instance, scheduler, MemoryUsage::DeviceLocal, DeviceBufferSize},
      gds_buffer{instance, scheduler, MemoryUsage::Stream, 0, AllFlags, DataShareBufferSize},
      bda_pagetable_buffer{instance, scheduler, MemoryUsage::DeviceLocal,
                           0,        AllFlags,  BDA_PAGETABLE_SIZE} {
    (void)ResourcePublicationTrace::Enabled();
    LOG_INFO(Render_Vulkan, "[BUFFER_PENDING_WRITE_RESOLVE] enabled={} default=1",
             BufferPendingWriteResolveEnabled() ? 1 : 0);
    LOG_INFO(Render_Vulkan, "[BUFFER_CONSUMER_BARRIER] enabled={} default=1",
             BufferConsumerBarrierEnabled() ? 1 : 0);
    LOG_INFO(Render_Vulkan, "[BUFFER_SHADER_ACCESS_BARRIER] enabled={} default=0",
             BufferShaderAccessBarrierEnabled() ? 1 : 0);
    LOG_INFO(Render_Vulkan, "[BUFFER_COPY_RESOLVE] enabled={} default=1",
             BufferCopyResolveEnabled() ? 1 : 0);
    Vulkan::SetObjectName(instance.GetDevice(), gds_buffer.Handle(), "GDS Buffer");
    Vulkan::SetObjectName(instance.GetDevice(), bda_pagetable_buffer.Handle(),
                          "BDA Page Table Buffer");

    memory_tracker = std::make_unique<MemoryTracker>(tracker);
    stream_upload_cache.SetEpochMode(
        StreamUploadCache::UseEpochForSerial(Common::ElfInfo::Instance().GameSerial()));

    std::memset(gds_buffer.mapped_data.data(), 0, DataShareBufferSize);

    // Ensure the first slot is used for the null buffer
    const auto null_id =
        slot_buffers.insert(instance, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
    ASSERT(null_id.index == 0);
    const vk::Buffer& null_buffer = slot_buffers[null_id].buffer;
    Vulkan::SetObjectName(instance.GetDevice(), null_buffer, "Null Buffer");

    // Set up garbage collection parameters
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = DEFAULT_TRIGGER_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    trigger_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      DEFAULT_TRIGGER_GC_MEMORY));
    critical_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      DEFAULT_CRITICAL_GC_MEMORY));
}

BufferCache::~BufferCache() = default;

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size) {
    if (!IsRegionRegistered(device_addr, size)) {
        return;
    }
    memory_tracker->InvalidateRegion(
        device_addr, size, [this, device_addr, size] { ReadMemory(device_addr, size, true); });
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write) {
    liverpool->SendCommand<true>([this, device_addr, size, is_write] {
        Buffer& buffer = slot_buffers[FindBuffer(device_addr, size)];
#ifdef LUMEN_GEN4_DIRECT_FEX
        // Use the same synchronous fault readback path as the direct core.
        // It retires a read watch even when its dirty byte range is empty.
        ASSERT_MSG(DownloadBufferMemoryDirect(buffer, device_addr, size, is_write),
                   "GPU fault readback lost its guest backing");
#else
        DownloadBufferMemory(buffer, device_addr, size, is_write);
#endif
    });
}

bool BufferCache::ReadMemoryDirect(VAddr device_addr, u64 size) {
    if (!IsRegionRegistered(device_addr, size)) {
        return false;
    }
    auto& buffer = slot_buffers[FindBuffer(device_addr, size)];
    return DownloadBufferMemoryDirect(buffer, device_addr, size, false);
}

std::optional<bool> BufferCache::ReadSourcePage(VAddr page, u64 generation,
                                               const std::function<void()>& before_read) {
    if (!IsRegionRegistered(page, 4096)) {
        return false;
    }
    const auto write = source_pages.Get(page);
    if (write.generation != generation) {
        return std::nullopt;
    }
    auto& buffer = slot_buffers[FindBuffer(page, 4096)];
    if (write.buffer != std::bit_cast<u64>(VkBuffer(buffer.Handle())) || !buffer.MapReadback()) {
        return std::nullopt;
    }
    return DownloadBufferMemoryDirect(buffer, page, 4096, false, &before_read);
}

bool BufferCache::IsRegionGpuModifiedExact(VAddr addr, u64 size) const {
    return addr + size > addr && gpu_modified_ranges.Intersects(addr, size);
}

void BufferCache::PreserveGuestWrite(VAddr address, u32 size) {
    if (size > 8) {
        return;
    }
    std::scoped_lock lock{preserved_writes_mutex};
    for (auto& entry : preserved_writes) {
        if (entry.address == address) {
            entry.size = std::max(entry.size, size);
            return;
        }
    }
    if (preserved_writes.size() == 512) {
        preserved_writes.erase(preserved_writes.begin());
    }
    preserved_writes.push_back({address, size});
}

bool BufferCache::WriteReadbackCopies(const Buffer& buffer,
                                     std::span<const vk::BufferCopy> copies, const u8* source,
                                     bool source_is_buffer) {
    struct SavedWrite { PreservedWrite range; std::array<u8, 8> bytes; };
    boost::container::small_vector<SavedWrite, 1> saved;
    {
        std::scoped_lock lock{preserved_writes_mutex};
        for (const auto& copy : copies) {
            const VAddr begin = buffer.CpuAddr() + copy.srcOffset;
            const VAddr end = begin + copy.size;
            for (const auto& range : preserved_writes) {
                if (range.address < end && range.address + range.size > begin) {
                    auto& item = saved.emplace_back();
                    item.range = range;
                    CopyCpuOwnedMemory(memory, range.address, item.bytes.data(), range.size);
                }
            }
        }
    }
    bool success = true;
    for (const auto& copy : copies) {
        success &= tracker.PublishBackingWrite(
            buffer.CpuAddr() + copy.srcOffset,
            source + (source_is_buffer ? copy.srcOffset : copy.dstOffset), copy.size);
    }
    for (const auto& item : saved) {
        tracker.PublishBackingWrite(item.range.address, item.bytes.data(),
                                item.range.size);
    }
    return success;
}

bool BufferCache::DownloadBufferMemoryDirect(Buffer& buffer, VAddr device_addr, u64 size,
                                             bool is_write,
                                             const std::function<void()>* before_read) {
    scheduler.BeginReadback();
    SCOPE_EXIT { scheduler.EndReadback(); };
    u8* mapped = before_read ? buffer.MapReadback() : nullptr;
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    memory_tracker->ForEachDownloadRange<false>(
        device_addr, size, [&](VAddr start, u64 length) {
            gpu_modified_ranges.ForEachInRange(start, length, [&](VAddr begin, VAddr end) {
                copies.push_back({begin - buffer.CpuAddr(), 0, end - begin});
            });
        });
    bool success = true;
    const auto download = [&](std::span<const vk::BufferCopy> batch, u64 bytes) {
        if (mapped) {
            (*before_read)();
            buffer.InvalidateReadback();
            success &= WriteReadbackCopies(buffer, batch, mapped, true);
            return;
        }
        auto temporary = std::move(readback_buffer);
        if (!temporary || temporary->SizeBytes() < bytes) {
            temporary = std::make_unique<Buffer>(instance, scheduler, MemoryUsage::Download, 0,
                                                vk::BufferUsageFlagBits::eTransferDst, bytes);
        }
        scheduler.EndRendering();
        const auto cmdbuf = scheduler.CommandBuffer();
        const vk::BufferMemoryBarrier2 before{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .buffer = buffer.Handle(), .offset = 0, .size = buffer.SizeBytes(),
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1,
                                                  .pBufferMemoryBarriers = &before});
        cmdbuf.copyBuffer(buffer.Handle(), temporary->Handle(), batch);
        const vk::BufferMemoryBarrier2 after{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
            .buffer = temporary->Handle(), .offset = 0, .size = bytes,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1,
                                                  .pBufferMemoryBarriers = &after});
        scheduler.Wait(scheduler.CurrentTick());
        temporary->InvalidateReadback();
        success &= WriteReadbackCopies(buffer, batch, temporary->mapped_data.data(), false);
        readback_buffer = std::move(temporary);
    };
    std::vector<vk::BufferCopy> batch;
    batch.reserve(std::min<size_t>(copies.size(), 16));
    u64 bytes = 0;
    for (const auto& copy : copies) {
        for (u64 offset = 0; offset < copy.size;) {
            const u64 length = std::min(copy.size - offset, DownloadBufferSize - bytes);
            batch.push_back({copy.srcOffset + offset, bytes, length});
            bytes += length;
            offset += length;
            if (bytes == DownloadBufferSize) {
                download(batch, bytes);
                batch.clear();
                bytes = 0;
            }
        }
    }
    if (bytes) {
        download(batch, bytes);
    }
    if (success) {
        memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](VAddr start, u64 length) {
            gpu_modified_ranges.Subtract(start, length);
        });
        memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    }
    return success;
}

void BufferCache::PublishGpuWrites() {
    for (auto& buffer : slot_buffers) {
        if (buffer.CpuAddr() != 0 && !buffer.is_deleted)
            DownloadBufferMemory(buffer, buffer.CpuAddr(), buffer.SizeBytes(), false);
    }
}

void BufferCache::DownloadBufferMemory(Buffer& buffer, VAddr device_addr, u64 size, bool is_write) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    memory_tracker->ForEachDownloadRange<false>(
        device_addr, size, [&](u64 device_addr_out, u64 range_size) {
            const VAddr buffer_addr = buffer.CpuAddr();
            const auto add_download = [&](VAddr start, VAddr end) {
                const u64 new_offset = start - buffer_addr;
                const u64 new_size = end - start;
                copies.push_back(vk::BufferCopy{
                    .srcOffset = new_offset,
                    .dstOffset = total_size_bytes,
                    .size = new_size,
                });
                // Align up to avoid cache conflicts
                constexpr u64 align = 64ULL;
                constexpr u64 mask = ~(align - 1ULL);
                total_size_bytes += (new_size + align - 1) & mask;
            };
            gpu_modified_ranges.ForEachInRange(device_addr_out, range_size, add_download);
            gpu_modified_ranges.Subtract(device_addr_out, range_size);
        });
    if (total_size_bytes == 0) {
        return;
    }
    std::unique_ptr<Buffer> large_download;
    u8* download;
    u64 offset;
    if (total_size_bytes > download_buffer.SizeBytes()) {
        large_download = std::make_unique<Buffer>(instance, scheduler, MemoryUsage::Download,
            0, vk::BufferUsageFlagBits::eTransferDst, total_size_bytes);
        download = large_download->mapped_data.data();
        offset = 0;
    } else {
        std::tie(download, offset) = download_buffer.Map(total_size_bytes);
    }
    auto& destination = large_download ? *large_download : static_cast<Buffer&>(download_buffer);
    for (auto& copy : copies) {
        // Modify copies to have the staging offset in mind
        copy.dstOffset += offset;
    }
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    cmdbuf.copyBuffer(buffer.buffer, destination.Handle(), copies);
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    const auto write_data = [&]() {
        auto* memory = Core::Memory::Instance();
        for (const auto& copy : copies) {
            const VAddr copy_device_addr = buffer.CpuAddr() + copy.srcOffset;
            const u64 dst_offset = copy.dstOffset - offset;
            ASSERT_MSG(tracker.PublishBackingWrite(copy_device_addr, download + dst_offset, copy.size),
                       "GPU buffer publication lost its guest backing");
        }
        memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    scheduler.Finish();
    if (!destination.is_coherent)
        vmaInvalidateAllocation(instance.GetAllocator(), destination.buffer.allocation,
                                offset, total_size_bytes);
    write_data();
}

void BufferCache::NoteShaderBufferWriteBinding(const Buffer& buffer, VAddr address, u64 size,
                                               u64 shader_hash, u32 shader_stage) {
    if (!BufferConsumerBarrierEnabled() || !ResourcePublicationTrace::Enabled() || size == 0) {
        return;
    }
    auto& record = shader_buffer_write_bindings[
        shader_buffer_write_binding_count++ % shader_buffer_write_bindings.size()];
    record = {address, size, u64(VkBuffer(buffer.Handle())), shader_hash,
              scheduler.CurrentTick(), shader_stage};
}

void BufferCache::NoteShaderBufferCommandAlias(const Buffer& buffer) {
    if (!BufferShaderAccessBarrierEnabled() || !ResourcePublicationTrace::Enabled()) {
        return;
    }
    const auto& usage = buffer.shader_command_access;
    if (!usage.has_read_only || !usage.has_write) {
        return;
    }
    const u32 capture_epoch = ResourcePublicationTrace::CaptureEpoch();
    if (shader_alias_trace_epoch != capture_epoch) {
        shader_alias_trace_epoch = capture_epoch;
        shader_alias_trace_count = 0;
    }
    const ShaderAliasTraceKey key{u64(VkBuffer(buffer.Handle())), usage.read_shader,
                                  usage.write_shader};
    const auto end = shader_alias_trace_keys.begin() + shader_alias_trace_count;
    if (shader_alias_trace_count == shader_alias_trace_keys.size() ||
        std::find(shader_alias_trace_keys.begin(), end, key) != end) {
        return;
    }
    shader_alias_trace_keys[shader_alias_trace_count++] = key;
    LOG_WARNING(Render_Vulkan,
                "BUFFER_SHADER_ALIAS event={} budget_last={} capture_epoch={} command_id={} "
                "buffer={:#x} buffer_size={:#x} read_shader={:#x} write_shader={:#x} "
                "union_access={:#x} tracked_access={:#x} tick={} binding_metadata_only=1",
                shader_alias_trace_count, shader_alias_trace_count == shader_alias_trace_keys.size(),
                capture_epoch, usage.command_id, key.handle, buffer.SizeBytes(), key.read_shader,
                key.write_shader, u64(VkAccessFlags2(usage.access)),
                u64(VkAccessFlags2(buffer.access_mask)), scheduler.CurrentTick());
}

void BufferCache::PublishBufferConsumer(Buffer& buffer, VAddr address, u64 size, u64 offset,
                                        BufferConsumer consumer) {
    if (!BufferConsumerBarrierEnabled() ||
        !BufferConsumerRangeFits(buffer.SizeBytes(), offset, size)) {
        return;
    }
    const bool gpu_modified = IsRegionGpuModifiedExact(address, size);
    const auto barrier = GetBufferConsumerBarrier(true, gpu_modified, consumer, buffer.Handle(),
                                                   offset, size);
    if (!barrier) {
        return;
    }
    if (ResourcePublicationTrace::Enabled()) {
        const u32 capture_epoch = ResourcePublicationTrace::CaptureEpoch();
        if (consumer_trace_epoch != capture_epoch) {
            consumer_trace_epoch = capture_epoch;
            consumer_trace_counts.fill(0);
        }
        auto& count = consumer_trace_counts[static_cast<size_t>(consumer)];
        if (count < 32) {
            ++count;
            ShaderBufferWriteBinding writer{};
            const u64 handle = u64(VkBuffer(buffer.Handle()));
            const u64 records = std::min<u64>(shader_buffer_write_binding_count,
                                              shader_buffer_write_bindings.size());
            for (u64 i = 0; i < records; ++i) {
                const auto& candidate = shader_buffer_write_bindings[
                    (shader_buffer_write_binding_count - 1 - i) % shader_buffer_write_bindings.size()];
                if (candidate.handle == handle && candidate.address < address + size &&
                    address < candidate.address + candidate.size) {
                    writer = candidate;
                    break;
                }
            }
            // The descriptor binding may belong to the current, not yet emitted
            // command. This is overlap evidence, not proof of GPU execution.
            LOG_WARNING(Render_Vulkan,
                        "BUFFER_CONSUMER_BARRIER kind={} event={} budget_last={} capture_epoch={} addr={:#x} "
                        "size={:#x} buffer={:#x} offset={:#x} tracked_access={:#x} tick={} "
                        "gpu_modified_exact=1 write_binding_shader={:#x} write_binding_stage={} "
                        "write_binding_addr={:#x} write_binding_size={:#x} write_binding_tick={}",
                        BufferConsumerName(consumer), count, count == 32, capture_epoch, address, size, handle,
                        offset, u64(VkAccessFlags2(buffer.access_mask)), scheduler.CurrentTick(),
                        writer.shader_hash, writer.stage, writer.address, writer.size, writer.tick);
        }
    }
    // Never use GetBarrier here: descriptor access state can represent a
    // dependency queued for later emission by Pipeline::BindResources.
    scheduler.EndRendering();
    scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &*barrier,
    });
}

void BufferCache::BindVertexBuffers(const Vulkan::GraphicsPipeline& pipeline) {
    const auto& regs = liverpool->regs;
    const bool trace_publication = ResourcePublicationTrace::Enabled();
    if (trace_publication) publication_bind_context = {};
    Vulkan::VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
    Vulkan::VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
    Vulkan::VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    Vulkan::VertexInputs<AmdGpu::Buffer> guest_buffers;
    pipeline.GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                             regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1);

    if (instance.IsVertexInputDynamicState()) {
        // Update current vertex inputs.
        const auto cmdbuf = scheduler.CommandBuffer();
        cmdbuf.setVertexInputEXT(bindings, attributes);
        if (trace_publication) {
            publication_bind_context.vertex_input_emitted = true;
            publication_bind_context.vertex_input_tick = scheduler.CurrentTick();
        }
    }

    if (bindings.empty()) {
        // If there are no bindings, there is nothing further to do.
        return;
    }

    struct BufferRange {
        VAddr base_address;
        VAddr end_address;
        vk::Buffer vk_buffer;
        u64 offset;

        [[nodiscard]] size_t GetSize() const {
            return end_address - base_address;
        }
    };

    // Build list of ranges covering the requested buffers
    Vulkan::VertexInputs<BufferRange> ranges{};
    for (const auto& buffer : guest_buffers) {
        if (buffer.GetSize() > 0) {
            ranges.emplace_back(buffer.base_address, buffer.base_address + buffer.GetSize());
        }
    }

    // Merge connecting ranges together
    Vulkan::VertexInputs<BufferRange> ranges_merged{};
    if (!ranges.empty()) {
        std::ranges::sort(ranges, [](const BufferRange& lhv, const BufferRange& rhv) {
            return lhv.base_address < rhv.base_address;
        });
        ranges_merged.emplace_back(ranges[0]);
        for (auto range : ranges) {
            auto& prev_range = ranges_merged.back();
            if (prev_range.end_address < range.base_address) {
                ranges_merged.emplace_back(range);
            } else {
                prev_range.end_address = std::max(prev_range.end_address, range.end_address);
            }
        }
    }

    // Map buffers for merged ranges
    for (auto& range : ranges_merged) {
        const u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
        const u64 tick_before = trace_publication ? scheduler.CurrentTick() : 0;
        const auto [buffer, offset] = ObtainBuffer(range.base_address, size, false);
        if (trace_publication) {
            ResourcePublicationTrace::BufferObtain("vertex", range.base_address, size,
                                                   tick_before, scheduler.CurrentTick(),
                                                   publication_bind_context);
        }
        PublishBufferConsumer(*buffer, range.base_address, size, offset, BufferConsumer::Vertex);
        range.vk_buffer = buffer->buffer;
        range.offset = offset;
    }

    // Bind vertex buffers
    Vulkan::VertexInputs<vk::Buffer> host_buffers;
    Vulkan::VertexInputs<vk::DeviceSize> host_offsets;
    Vulkan::VertexInputs<vk::DeviceSize> host_sizes;
    Vulkan::VertexInputs<vk::DeviceSize> host_strides;
    const auto null_buffer =
        instance.IsNullDescriptorSupported() ? VK_NULL_HANDLE : GetBuffer(NULL_BUFFER_ID).Handle();
    for (const auto& buffer : guest_buffers) {
        if (buffer.GetSize() > 0) {
            const auto host_buffer_info =
                std::ranges::find_if(ranges_merged, [&](const BufferRange& range) {
                    return buffer.base_address >= range.base_address &&
                           buffer.base_address < range.end_address;
                });
            ASSERT(host_buffer_info != ranges_merged.cend());
            host_buffers.emplace_back(host_buffer_info->vk_buffer);
            host_offsets.push_back(host_buffer_info->offset + buffer.base_address -
                                   host_buffer_info->base_address);
        } else {
            host_buffers.emplace_back(null_buffer);
            host_offsets.push_back(0);
        }
        host_sizes.push_back(buffer.GetSize());
        host_strides.push_back(buffer.GetStride());
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    const auto num_buffers = guest_buffers.size();

    // setVertexInputEXT() above owns binding strides when vertex input is dynamic.
    // Repeating those strides through vkCmdBindVertexBuffers2 is redundant and can
    // violate VUID-vkCmdBindVertexBuffers2-pStrides-06209 when an unused guest
    // fetch-shader stream has stride zero.
    const auto bind_path =
        SelectVertexBufferBindPath(instance.IsVertexInputDynamicState());
    if (bind_path == VertexBufferBindPath::Core) {
        cmdbuf.bindVertexBuffers(0, num_buffers, host_buffers.data(), host_offsets.data());
    } else {
        cmdbuf.bindVertexBuffers2(0, num_buffers, host_buffers.data(), host_offsets.data(),
                                  host_sizes.data(), host_strides.data());
    }
    if (trace_publication) {
        publication_bind_context.vertex_buffers_emitted = true;
        publication_bind_context.vertex_buffers_tick = scheduler.CurrentTick();
    }
}

void BufferCache::BindIndexBuffer(u32 index_offset) {
    const auto& regs = liverpool->regs;

    // Figure out index type and size.
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const vk::IndexType index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    const VAddr index_address =
        regs.index_base_address.Address<VAddr>() + index_offset * index_size;

    // Bind index buffer.
    const u32 index_buffer_size = regs.num_indices * index_size;
    const bool trace_publication = ResourcePublicationTrace::Enabled();
    const u64 tick_before = trace_publication ? scheduler.CurrentTick() : 0;
    const auto [vk_buffer, offset] = ObtainBuffer(index_address, index_buffer_size, false);
    if (trace_publication) {
        ResourcePublicationTrace::BufferObtain("index", index_address, index_buffer_size,
                                               tick_before, scheduler.CurrentTick(),
                                               publication_bind_context);
    }
    PublishBufferConsumer(*vk_buffer, index_address, index_buffer_size, offset,
                          BufferConsumer::Index);
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindIndexBuffer(vk_buffer->Handle(), offset, index_type);
}

void BufferCache::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    ASSERT_MSG(address % 4 == 0, "GDS offset must be dword aligned");
    if (!is_gds) {
        texture_cache.ClearMeta(address);
        if (!IsRegionGpuModified(address, num_bytes)) {
            u32* buffer = std::bit_cast<u32*>(address);
            std::fill(buffer, buffer + num_bytes / sizeof(u32), value);
            return;
        }
        if (num_bytes <= 8) {
            const std::array<u32, 2> values{value, value};
            if (!tracker.PublishBackingWrite(address, values.data(), num_bytes)) {
                std::memcpy(std::bit_cast<void*>(address), values.data(), num_bytes);
            }
            PreserveGuestWrite(address, num_bytes);
        }
    }
    Buffer* buffer = [&] {
        if (is_gds) {
            return &gds_buffer;
        }
        const auto [buffer, offset] = ObtainBuffer(address, num_bytes, true);
        return buffer;
    }();
    if (!is_gds) texture_cache.InvalidateMemoryFromGPU(address, num_bytes);
    buffer->Fill(buffer->Offset(address), num_bytes, value);
}

void BufferCache::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    if (!dst_gds && !IsRegionGpuModified(dst, num_bytes)) {
        if (!src_gds && !IsRegionGpuModified(src, num_bytes) &&
            !texture_cache.FindImageFromRange(src, num_bytes)) {
            // Both buffers were not transferred to GPU yet. Can safely copy in host memory.
            memcpy(std::bit_cast<void*>(dst), std::bit_cast<void*>(src), num_bytes);
            return;
        }
        // Without a readback there's nothing we can do with this
        // Fallback to creating dst buffer on GPU to at least have this data there
    }
    texture_cache.InvalidateMemoryFromGPU(dst, num_bytes);
    const bool resolve_copy = BufferCopyResolveEnabled();
    BufferId src_id{}, dst_id{};
    if (resolve_copy && !src_gds && !dst_gds) {
        std::tie(src_id, dst_id) = ResolveBufferCopyIds(
            src, dst, num_bytes, [this](VAddr address, u32 size) {
                return FindBuffer(address, size);
            });
    }
    auto& src_buffer = [&] -> const Buffer& {
        if (src_gds) {
            return gds_buffer;
        }
        const auto buffer_id = src_id ? src_id : FindBuffer(src, num_bytes);
        auto& buffer = slot_buffers[buffer_id];
        SynchronizeBuffer(buffer, src, num_bytes, false, true);
        return buffer;
    }();
    auto& dst_buffer = [&] -> const Buffer& {
        if (dst_gds) {
            return gds_buffer;
        }
        const auto buffer_id = dst_id ? dst_id : FindBuffer(dst, num_bytes);
        auto& buffer = slot_buffers[buffer_id];
        SynchronizeBuffer(buffer, dst, num_bytes, true, true);
        gpu_modified_ranges.Add(dst, num_bytes);
        return buffer;
    }();
    const vk::BufferCopy region = {
        .srcOffset = src_buffer.Offset(src),
        .dstOffset = dst_buffer.Offset(dst),
        .size = num_bytes,
    };
    const vk::BufferMemoryBarrier2 buf_barriers_before[2] = {
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .buffer = dst_buffer.Handle(),
            .offset = dst_buffer.Offset(dst),
            .size = num_bytes,
        },
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .buffer = src_buffer.Handle(),
            .offset = src_buffer.Offset(src),
            .size = num_bytes,
        },
    };
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 2,
        .pBufferMemoryBarriers = buf_barriers_before,
    });
    if (resolve_copy && src_buffer.Handle() == dst_buffer.Handle() &&
        BufferCopyRangesOverlap(region.srcOffset, region.dstOffset, num_bytes)) {
        // Resolving a merge may put both ranges in the same backing. Vulkan
        // forbids overlapping copy regions; preserve the source in a temporary
        // buffer before writing the destination.
        auto scratch = std::make_unique<Buffer>(
            instance, scheduler, MemoryUsage::DeviceLocal, 0,
            vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
            num_bytes);
        cmdbuf.copyBuffer(src_buffer.Handle(), scratch->Handle(),
                         vk::BufferCopy{region.srcOffset, 0, num_bytes});
        const vk::MemoryBarrier2 between{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite |
                             vk::AccessFlagBits2::eTransferRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead |
                             vk::AccessFlagBits2::eTransferWrite,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .memoryBarrierCount = 1, .pMemoryBarriers = &between});
        cmdbuf.copyBuffer(scratch->Handle(), dst_buffer.Handle(),
                         vk::BufferCopy{0, region.dstOffset, num_bytes});
        scheduler.DeferOperation([scratch = std::move(scratch)]() mutable { scratch.reset(); });
    } else {
        cmdbuf.copyBuffer(src_buffer.Handle(), dst_buffer.Handle(), region);
    }
    const vk::BufferMemoryBarrier2 buf_barriers_after[2] = {
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .buffer = dst_buffer.Handle(),
            .offset = dst_buffer.Offset(dst),
            .size = num_bytes,
        },
        {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .buffer = src_buffer.Handle(),
            .offset = src_buffer.Offset(src),
            .size = num_bytes,
        },
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 2,
        .pBufferMemoryBarriers = buf_barriers_after,
    });
}

std::pair<Buffer*, u32> BufferCache::ObtainBuffer(VAddr device_addr, u32 size, bool is_written,
                                                  bool is_texel_buffer, BufferId buffer_id) {
    // Guest VA 0 is a host nullptr. StreamBuffer::Copy memcpy's the address
    // directly; Driveclub 3D entry did that and SIGSEGV'd (Android exit 133).
    if (!AmdGpu::GpuBufferCopySourceUsable(device_addr, size)) {
        LOG_ERROR(Render_Vulkan, "ObtainBuffer rejected addr={:#x} size={}", device_addr, size);
        return {&slot_buffers[NULL_BUFFER_ID], 0};
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= CACHING_PAGESIZE && !IsRegionGpuModified(device_addr, size)) {
        const u64 offset = stream_upload_cache.Copy(
            device_addr, size, scheduler.CurrentTick(),
            [&](VAddr address, u8* destination, u64 amount) {
                CopyCpuOwnedMemory(memory, address, destination, amount);
            },
            [&](VAddr address, const u8* expected, u64 amount) {
                return CompareCpuOwnedMemory(memory, address, expected, amount);
            },
            [&](u64 amount) { return stream_buffer.Map(amount, instance.UniformMinAlignment()); },
            [&] { stream_buffer.Commit(); });
        return {&stream_buffer, offset};
    }
    if (IsBufferInvalid(buffer_id)) {
        buffer_id = FindBuffer(device_addr, size);
    }
    Buffer& buffer = slot_buffers[buffer_id];
    SynchronizeBuffer(buffer, device_addr, size, is_written, is_texel_buffer);
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
    }
    return {&buffer, buffer.Offset(device_addr)};
}

std::pair<Buffer*, u32> BufferCache::ObtainBufferForImage(VAddr gpu_addr, u32 size) {
    // Check if any buffer contains the full requested range.
    const BufferId buffer_id = page_table[gpu_addr >> CACHING_PAGEBITS].buffer_id;
    if (buffer_id) {
        if (Buffer& buffer = slot_buffers[buffer_id]; buffer.IsInBounds(gpu_addr, size)) {
            SynchronizeBuffer(buffer, gpu_addr, size, false, false);
            return {&buffer, buffer.Offset(gpu_addr)};
        }
    }
    // If some buffer within was GPU modified create a full buffer to avoid losing GPU data.
    if (IsRegionGpuModified(gpu_addr, size)) {
        return ObtainBuffer(gpu_addr, size, false, false);
    }

    // Baseline: CPU copy into shared staging StreamBuffer ring.
    const auto [data, offset] = staging_buffer.Map(size, 16);
    CopyCpuOwnedMemory(memory, gpu_addr, data, size);
    staging_buffer.Commit();
    return {&staging_buffer, offset};
}

bool BufferCache::IsRegionRegistered(VAddr addr, size_t size) {
    // Check if we are missing some edge case here
    return buffer_ranges.Intersects(addr, size);
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

BufferId BufferCache::FindBuffer(VAddr device_addr, u32 size) {
    if (device_addr == 0) {
        return NULL_BUFFER_ID;
    }
    const u64 page = device_addr >> CACHING_PAGEBITS;
    const BufferId buffer_id = page_table[page].buffer_id;
    if (!buffer_id) {
        return CreateBuffer(device_addr, size);
    }
    const Buffer& buffer = slot_buffers[buffer_id];
    if (buffer.IsInBounds(device_addr, size)) {
        return buffer_id;
    }
    return CreateBuffer(device_addr, size);
}

BufferCache::OverlapResult BufferCache::ResolveOverlaps(VAddr device_addr, u32 wanted_size) {
    static constexpr int STREAM_LEAP_THRESHOLD = 16;
    boost::container::small_vector<BufferId, 16> overlap_ids;
    VAddr begin = device_addr;
    VAddr end = device_addr + wanted_size;
    int stream_score = 0;
    bool has_stream_leap = false;
    const auto expand_begin = [&](VAddr add_value) {
        static constexpr VAddr min_page = CACHING_PAGESIZE + DEVICE_PAGESIZE;
        if (add_value > begin - min_page) {
            begin = min_page;
            device_addr = DEVICE_PAGESIZE;
            return;
        }
        begin -= add_value;
        device_addr = begin - CACHING_PAGESIZE;
    };
    const auto expand_end = [&](VAddr add_value) {
        static constexpr VAddr max_page = 1ULL << MemoryTracker::MAX_CPU_PAGE_BITS;
        if (add_value > max_page - end) {
            end = max_page;
            return;
        }
        end += add_value;
    };
    if (begin == 0) {
        return OverlapResult{
            .ids = std::move(overlap_ids),
            .begin = begin,
            .end = end,
            .has_stream_leap = has_stream_leap,
        };
    }
    for (; device_addr >> CACHING_PAGEBITS < Common::DivCeil(end, CACHING_PAGESIZE);
         device_addr += CACHING_PAGESIZE) {
        const BufferId overlap_id = page_table[device_addr >> CACHING_PAGEBITS].buffer_id;
        if (!overlap_id) {
            continue;
        }
        Buffer& overlap = slot_buffers[overlap_id];
        if (overlap.is_picked) {
            continue;
        }
        overlap_ids.push_back(overlap_id);
        overlap.is_picked = true;
        const VAddr overlap_device_addr = overlap.CpuAddr();
        const bool expands_left = overlap_device_addr < begin;
        if (expands_left) {
            begin = overlap_device_addr;
        }
        const VAddr overlap_end = overlap_device_addr + overlap.SizeBytes();
        const bool expands_right = overlap_end > end;
        if (overlap_end > end) {
            end = overlap_end;
        }
        stream_score += overlap.StreamScore();
        if (stream_score > STREAM_LEAP_THRESHOLD && !has_stream_leap) {
            // When this memory region has been joined a bunch of times, we assume it's being used
            // as a stream buffer. Increase the size to skip constantly recreating buffers.
            has_stream_leap = true;
            if (expands_right) {
                expand_end(CACHING_PAGESIZE * 128);
            }
            if (expands_left) {
                expand_begin(CACHING_PAGESIZE * 128);
            }
        }
    }
    return OverlapResult{
        .ids = std::move(overlap_ids),
        .begin = begin,
        .end = end,
        .has_stream_leap = has_stream_leap,
    };
}

void BufferCache::JoinOverlap(BufferId new_buffer_id, BufferId overlap_id,
                              bool accumulate_stream_score) {
    Buffer& new_buffer = slot_buffers[new_buffer_id];
    Buffer& overlap = slot_buffers[overlap_id];
    if (accumulate_stream_score) {
        new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
    }
    const size_t dst_base_offset = overlap.CpuAddr() - new_buffer.CpuAddr();
    const vk::BufferCopy copy = {
        .srcOffset = 0,
        .dstOffset = dst_base_offset,
        .size = overlap.SizeBytes(),
    };
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    boost::container::static_vector<vk::BufferMemoryBarrier2, 2> pre_barriers{};
    if (auto src_barrier = overlap.GetBarrier(vk::AccessFlagBits2::eTransferRead,
                                              vk::PipelineStageFlagBits2::eTransfer)) {
        pre_barriers.push_back(*src_barrier);
    }
    if (auto dst_barrier =
            new_buffer.GetBarrier(vk::AccessFlagBits2::eTransferWrite,
                                  vk::PipelineStageFlagBits2::eTransfer, dst_base_offset)) {
        pre_barriers.push_back(*dst_barrier);
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = static_cast<u32>(pre_barriers.size()),
        .pBufferMemoryBarriers = pre_barriers.data(),
    });

    cmdbuf.copyBuffer(overlap.Handle(), new_buffer.Handle(), copy);

    boost::container::static_vector<vk::BufferMemoryBarrier2, 2> post_barriers{};
    if (auto src_barrier =
            overlap.GetBarrier(vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                               vk::PipelineStageFlagBits2::eAllCommands)) {
        post_barriers.push_back(*src_barrier);
    }
    if (auto dst_barrier = new_buffer.GetBarrier(
            vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            vk::PipelineStageFlagBits2::eAllCommands, dst_base_offset)) {
        post_barriers.push_back(*dst_barrier);
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = static_cast<u32>(post_barriers.size()),
        .pBufferMemoryBarriers = post_barriers.data(),
    });
    DeleteBuffer(overlap_id);
}

BufferId BufferCache::CreateBuffer(VAddr device_addr, u32 wanted_size) {
    const VAddr device_addr_end = Common::AlignUp(device_addr + wanted_size, CACHING_PAGESIZE);
    device_addr = Common::AlignDown(device_addr, CACHING_PAGESIZE);
    wanted_size = static_cast<u32>(device_addr_end - device_addr);
    const OverlapResult overlap = ResolveOverlaps(device_addr, wanted_size);
    const u32 size = static_cast<u32>(overlap.end - overlap.begin);
    const BufferId new_buffer_id =
        slot_buffers.insert(instance, scheduler, MemoryUsage::DeviceLocal, overlap.begin,
                            AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, size);
    auto& new_buffer = slot_buffers[new_buffer_id];
    for (const BufferId overlap_id : overlap.ids) {
        JoinOverlap(new_buffer_id, overlap_id, !overlap.has_stream_leap);
    }
    Register(new_buffer_id);
    return new_buffer_id;
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager.ProcessFaultBuffer();
}

void BufferCache::ProcessPendingCpuWrites() {
    memory_tracker->ProcessPendingCpuWrites();
    stream_upload_cache.AdvanceEpoch();
}

void BufferCache::Register(BufferId buffer_id) {
    ChangeRegister<true>(buffer_id);
    const auto& buffer = slot_buffers[buffer_id];
    CpuWriteJournal::Instance().Record(buffer.CpuAddr(), buffer.SizeBytes());
}

void BufferCache::Unregister(BufferId buffer_id) {
    ChangeRegister<false>(buffer_id);
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId buffer_id) {
    Buffer& buffer = slot_buffers[buffer_id];
    const auto size = buffer.SizeBytes();
    const VAddr device_addr_begin = buffer.CpuAddr();
    const VAddr device_addr_end = device_addr_begin + size;
    const u64 page_begin = device_addr_begin / CACHING_PAGESIZE;
    const u64 page_end = Common::DivCeil(device_addr_end, CACHING_PAGESIZE);
    const u64 size_pages = page_end - page_begin;
    for (u64 page = page_begin; page != page_end; ++page) {
        if constexpr (insert) {
            page_table[page].buffer_id = buffer_id;
        } else {
            page_table[page].buffer_id = BufferId{};
        }
    }
    if constexpr (insert) {
        total_used_memory += Common::AlignUp(size, CACHING_PAGESIZE);
        buffer.SetLRUId(lru_cache.Insert(buffer_id, gc_tick));
        boost::container::small_vector<vk::DeviceAddress, 128> bda_addrs;
        bda_addrs.reserve(size_pages);
        for (u64 i = 0; i < size_pages; ++i) {
            vk::DeviceAddress addr = buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS);
            bda_addrs.push_back(addr);
        }
        WriteDataBuffer(bda_pagetable_buffer, page_begin * sizeof(vk::DeviceAddress),
                        bda_addrs.data(), bda_addrs.size() * sizeof(vk::DeviceAddress));
        buffer_ranges.Add(buffer.CpuAddr(), buffer.SizeBytes(), buffer_id);
    } else {
        total_used_memory -= Common::AlignUp(size, CACHING_PAGESIZE);
        lru_cache.Free(buffer.LRUId());
        const u64 offset = bda_pagetable_buffer.Offset(page_begin * sizeof(vk::DeviceAddress));
        bda_pagetable_buffer.Fill(offset, size_pages * sizeof(vk::DeviceAddress), 0);
        buffer_ranges.Subtract(buffer.CpuAddr(), buffer.SizeBytes());
    }
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, VAddr device_addr, u32 size, bool is_written,
                                    bool is_texel_buffer) {
    if (BufferPendingWriteResolveEnabled()) {
        const u32 changed_regions = memory_tracker->ResolvePendingCpuWrites(device_addr, size);
        if (changed_regions != 0 && ResourcePublicationTrace::Enabled()) {
            static u32 traces = 0;
            static u32 last_capture_epoch = 0;
            const u32 capture_epoch = ResourcePublicationTrace::CaptureEpoch();
            if (last_capture_epoch != capture_epoch) {
                last_capture_epoch = capture_epoch;
                traces = 0;
            }
            if (traces < 32) {
                ++traces;
                LOG_WARNING(Render_Vulkan,
                            "BUFFER_PENDING_WRITE_RESOLVED event={} budget_last={} capture_epoch={} addr={:#x} "
                            "size={:#x} changed_regions={} is_written={} tick={}",
                            traces, traces == 32, capture_epoch, device_addr, size, changed_regions, is_written,
                            scheduler.CurrentTick());
            }
        }
    }
    if (is_written) {
        buffer.upload_cache.Clear();
        source_pages.Mark(device_addr, size, std::bit_cast<u64>(VkBuffer(buffer.Handle())));
    } else if (const auto generations = buffer.upload_cache.Find(device_addr, size);
               generations && !generations->empty() &&
               memory_tracker->AreRegionGenerationsCurrent(device_addr, size, *generations)) {
        return is_texel_buffer ? SynchronizeBufferFromImage(buffer, device_addr, size) : false;
    }
    scheduler.BeginReadback();
    SCOPE_EXIT { scheduler.EndReadback(); };
    BufferUploadCache::Generations generations;

    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes = 0;
    VAddr buffer_start = buffer.CpuAddr();
    vk::Buffer src_buffer = VK_NULL_HANDLE;
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 device_addr_out, u64 range_size) {
            copies.emplace_back(total_size_bytes, device_addr_out - buffer_start, range_size);
            total_size_bytes += range_size;
        },
        [&] { src_buffer = UploadCopies(buffer, copies, total_size_bytes); },
        is_written ? nullptr : &generations);
    if (!is_written) {
        buffer.upload_cache.Store(device_addr, size, std::move(generations));
    }

    if (src_buffer) {
        scheduler.EndRendering();
        const auto cmdbuf = scheduler.CommandBuffer();
        const vk::BufferMemoryBarrier2 pre_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
                             vk::AccessFlagBits2::eTransferRead |
                             vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .buffer = buffer.Handle(),
            .offset = 0,
            .size = buffer.SizeBytes(),
        };
        const vk::BufferMemoryBarrier2 post_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .buffer = buffer.Handle(),
            .offset = 0,
            .size = buffer.SizeBytes(),
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &pre_barrier,
        });
        cmdbuf.copyBuffer(src_buffer, buffer.buffer, copies);
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &post_barrier,
        });
        TouchBuffer(buffer);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeBufferFromImage(buffer, device_addr, size);
    }
    return false;
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     size_t total_size_bytes) {
    if (copies.empty()) {
        return VK_NULL_HANDLE;
    }
    const auto [staging, offset] = staging_buffer.Map(total_size_bytes);
    if (staging) {
        for (auto& copy : copies) {
            u8* const src_pointer = staging + copy.srcOffset;
            const VAddr device_addr = buffer.CpuAddr() + copy.dstOffset;
            CopyCpuOwnedMemory(memory, device_addr, src_pointer, copy.size);
            // Apply the staging offset
            copy.srcOffset += offset;
        }
        staging_buffer.Commit();
        return staging_buffer.Handle();
    } else {
        // For large one time transfers use a temporary host buffer.
        auto temp_buffer =
            std::make_unique<Buffer>(instance, scheduler, MemoryUsage::Upload, 0,
                                     vk::BufferUsageFlagBits::eTransferSrc, total_size_bytes);
        const vk::Buffer src_buffer = temp_buffer->Handle();
        u8* const staging = temp_buffer->mapped_data.data();
        for (const auto& copy : copies) {
            u8* const src_pointer = staging + copy.srcOffset;
            const VAddr device_addr = buffer.CpuAddr() + copy.dstOffset;
            CopyCpuOwnedMemory(memory, device_addr, src_pointer, copy.size);
        }
        scheduler.DeferOperation([buffer = std::move(temp_buffer)]() mutable { buffer.reset(); });
        return src_buffer;
    }
}

bool BufferCache::SynchronizeBufferFromImage(Buffer& buffer, VAddr device_addr, u32 size) {
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        return false;
    }
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u32 buf_offset = buffer.Offset(image.info.guest_address);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    u32 copy_size = 0;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (buf_offset + mip_info.offset + mip_info.size > buffer.SizeBytes()) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
        copy_size += mip_info.size;
    }
    if (copy_size == 0) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, buffer.Handle(), buf_offset, copy_size);
    return true;
}

void BufferCache::SynchronizeBuffersInRange(VAddr device_addr, u64 size) {
    const VAddr device_addr_end = device_addr + size;
    ForEachBufferInRange(device_addr, size, [&](BufferId buffer_id, Buffer& buffer) {
        RENDERER_TRACE;
        VAddr start = std::max(buffer.CpuAddr(), device_addr);
        VAddr end = std::min(buffer.CpuAddr() + buffer.SizeBytes(), device_addr_end);
        u32 size = static_cast<u32>(end - start);
        SynchronizeBuffer(buffer, start, size, false, false);
    });
}

void BufferCache::WriteDataBuffer(Buffer& buffer, VAddr address, const void* value, u32 num_bytes) {
    vk::BufferCopy copy = {
        .srcOffset = 0,
        .dstOffset = buffer.Offset(address),
        .size = num_bytes,
    };
    vk::Buffer src_buffer = staging_buffer.Handle();
    if (num_bytes < StagingBufferSize) {
        const auto [staging, offset] = staging_buffer.Map(num_bytes);
        std::memcpy(staging, value, num_bytes);
        copy.srcOffset = offset;
        staging_buffer.Commit();
    } else {
        // For large one time transfers use a temporary host buffer.
        // RenderDoc can lag quite a bit if the stream buffer is too large.
        Buffer temp_buffer{
            instance, scheduler, MemoryUsage::Upload, 0, vk::BufferUsageFlagBits::eTransferSrc,
            num_bytes};
        src_buffer = temp_buffer.Handle();
        u8* const staging = temp_buffer.mapped_data.data();
        std::memcpy(staging, value, num_bytes);
        scheduler.DeferOperation([buffer = std::move(temp_buffer)]() mutable {});
    }
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer.Handle(),
        .offset = buffer.Offset(address),
        .size = num_bytes,
    };
    const vk::BufferMemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .buffer = buffer.Handle(),
        .offset = buffer.Offset(address),
        .size = num_bytes,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
    });
    cmdbuf.copyBuffer(src_buffer, buffer.Handle(), copy);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
}

void BufferCache::RunGarbageCollector() {
    SCOPE_EXIT {
        ++gc_tick;
    };
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
    }
    if (total_used_memory < trigger_gc_memory) {
        return;
    }
    const bool aggressive = total_used_memory >= critical_gc_memory;
    const u64 ticks_to_destroy = std::min<u64>(aggressive ? 80 : 160, gc_tick);
    int max_deletions = aggressive ? 64 : 32;
    const auto clean_up = [&](BufferId buffer_id) {
        if (max_deletions == 0) {
            return;
        }
        --max_deletions;
        Buffer& buffer = slot_buffers[buffer_id];
        // InvalidateMemory(buffer.CpuAddr(), buffer.SizeBytes());
        DownloadBufferMemory(buffer, buffer.CpuAddr(), buffer.SizeBytes(), true);
        DeleteBuffer(buffer_id);
    };
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
    lru_cache.Touch(buffer.LRUId(), gc_tick);
}

void BufferCache::DeleteBuffer(BufferId buffer_id) {
    Buffer& buffer = slot_buffers[buffer_id];
    Unregister(buffer_id);
    scheduler.DeferOperation([this, buffer_id] { slot_buffers.erase(buffer_id); });
    buffer.is_deleted = true;
}

} // namespace VideoCore
