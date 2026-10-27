#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "FexPerformanceTrace.h"
#include "graphics/host_gpu/renderer/gpuSliceTrace.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/renderTiming.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "../../../../../../engine/src/common/memoryBudget.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace Libs::Graphics {

bool BufferContiguousHashEnabled() noexcept {
    static const bool enabled = [] {
        const auto* value = std::getenv("LUMEN_PS5_BUFFER_CONTIGUOUS_HASH");
        if (value == nullptr || std::strcmp(value, "1") == 0) return true;
        if (std::strcmp(value, "0") == 0) return false;
        EXIT("Invalid LUMEN_PS5_BUFFER_CONTIGUOUS_HASH (expected 0 or 1)\n");
        std::abort(); // Keep malformed selectors fail-closed if EXIT ever returns.
    }();
    return enabled;
}

bool BufferPublicationWitnessEnabled() noexcept {
    static const bool enabled = [] {
        const auto* value = std::getenv("LUMEN_PS5_BUFFER_PUBLICATION_WITNESS");
        if (value == nullptr || std::strcmp(value, "0") == 0) return false;
        if (std::strcmp(value, "1") == 0) return true;
        EXIT("Invalid LUMEN_PS5_BUFFER_PUBLICATION_WITNESS (expected 0 or 1)\n");
        std::abort();
    }();
    return enabled;
}

namespace {

constexpr uint64_t MiB           = 1024 * 1024;
constexpr uint64_t GdsBufferSize = 64 * 1024;

void ValidateBackingWrite(uint64_t address, uint64_t size) {
#if defined(LUMEN_PS5_FEXCORE)
    const auto end = address + size;
    for (auto cursor = address; cursor < end;) {
        uint64_t base {}, bytes {};
        uint32_t writable {};
        if (Libs::LibKernel::Memory::QueryGuestExecutable(nullptr, cursor, &base, &bytes, &writable) == 1)
            EXIT("PS5 GPU backing writes to executable guest memory require a FEX mutation barrier; unavailable\n");
        const auto remaining = TRACKER_PAGE_SIZE - (cursor % TRACKER_PAGE_SIZE);
        cursor += std::min(remaining, end - cursor);
    }
#else
    (void)address;
    (void)size;
#endif
}

} // namespace

void BufferCache::WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source,
                                  uint64_t size) {
	auto* bytes = static_cast<const uint8_t*>(source);
	while (size != 0) {
		const auto chunk  = std::min(size, m_staging_buffer.Size());
		// Copy waits for ring reuse and flushes the fresh host data before submission.
		const auto offset = m_staging_buffer.Copy(bytes, chunk, 4);
		const auto destination_offset = buffer.Offset(address);
		EXIT_IF(destination_offset > buffer.Size() || chunk > buffer.Size() - destination_offset);
		m_scheduler.EndRendering();
		const auto command = m_scheduler.Current().Handle();
		vk::BufferMemoryBarrier2 before {};
		before.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		before.srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		before.dstStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		before.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer        = buffer.Handle();
		before.offset        = destination_offset;
		before.size          = chunk;
		vk::DependencyInfo dependency {};
		dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
		dependency.bufferMemoryBarrierCount = 1;
		dependency.pBufferMemoryBarriers    = &before;
		command.pipelineBarrier2(dependency);
		const vk::BufferCopy copy {offset, destination_offset, chunk};
		command.copyBuffer(m_staging_buffer.Handle(), buffer.Handle(), 1, &copy);
		auto after          = before;
		after.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		after.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
		after.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		after.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		dependency.pBufferMemoryBarriers = &after;
		command.pipelineBarrier2(dependency);
		bytes += chunk;
		address += chunk;
		size -= chunk;
	}
}

void BufferCache::Register(BufferId id) {
	ChangeRegister<true>(id);
}

void BufferCache::Unregister(BufferId id, SnapshotDisposition snapshots) {
    const auto& buffer = m_slot_buffers[id];
    if (snapshots == SnapshotDisposition::Forget)
        m_backing_snapshots.Forget(buffer.CpuAddress(), buffer.Size());
	ChangeRegister<false>(id);
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId id) {
	auto& buffer = m_slot_buffers[id];
	if constexpr (!insert) InvalidateUniformMetadata(buffer.CpuAddress(), buffer.Size());
	PageTable::PageRange pages {};
	EXIT_IF(!(GuestRange {buffer.CpuAddress(), buffer.Size()}.Valid()) ||
	        !PageTable::TryGetPageRange(buffer.CpuAddress(), buffer.Size(), pages));
	for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
		if constexpr (insert) {
			m_page_table[page] = id;
		} else {
			m_page_table[page] = {};
		}
	}
	const auto size_pages = pages.last_exclusive - pages.first;
	const auto table_offset = PageIndex(buffer.CpuAddress()) * sizeof(vk::DeviceAddress);
	if constexpr (insert) {
		const auto [it, inserted] = m_buffers.emplace(buffer.CpuAddress(), id);
		(void)it;
		EXIT_IF(!inserted);
		m_total_used_memory += buffer.Size();
		buffer.lru_id = m_lru_cache.Insert(id, m_gc_tick);
		std::vector<vk::DeviceAddress> addresses;
		addresses.reserve(size_pages);
		for (uint64_t i = 0; i < size_pages; ++i) {
			addresses.push_back(buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS));
		}
		WriteDataBuffer(m_bda_pagetable_buffer, table_offset,
		                addresses.data(), addresses.size() * sizeof(vk::DeviceAddress));
	} else {
		const auto found = m_buffers.find(buffer.CpuAddress());
		EXIT_IF(found == m_buffers.end() || found->second != id);
		m_buffers.erase(found);
		EXIT_IF(buffer.Size() > m_total_used_memory);
		m_total_used_memory -= buffer.Size();
		m_lru_cache.Free(buffer.lru_id);
		m_bda_pagetable_buffer.Fill(table_offset,
		                            size_pages * sizeof(vk::DeviceAddress), 0);
		buffer.is_deleted = true;
	}
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
	if (!buffer.is_deleted) {
		m_lru_cache.Touch(buffer.lru_id, m_gc_tick);
	}
}

void BufferCache::DeleteBuffer(BufferId id, SnapshotDisposition snapshots) {
	if (IsBufferInvalid(id)) {
		return;
	}
	Unregister(id, snapshots);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_buffers.erase(id); });
	} else {
		m_slot_buffers.erase(id);
	}
}

template <bool async>
bool BufferCache::DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size,
                                       std::shared_ptr<DownloadWitnesses> witnesses) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size     = 0;
	const auto                  buffer_address = buffer.CpuAddress();
	m_memory_tracker.ForEachDownloadRange<false>(
	    vaddr, size, [&](uint64_t address, uint64_t bytes) noexcept {
		    InvalidateUniformMetadata(address, bytes);
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "buffer download");
		    m_gpu_modified_ranges.ForEachInRange(address, bytes, [&](uint64_t start, uint64_t end) {
			    copies.emplace_back(start - buffer_address, total_size, end - start);
			    // Keep packed ranges on separate cache lines, as in shadPS4.
			    total_size += Common::AlignUp(end - start, 64);
		    });
		    m_gpu_modified_ranges.Subtract(address, bytes);
	    });
	if (copies.empty()) {
		return false;
	}
	uint64_t payload_size = 0;
	if constexpr (!async) {
		if (Common::RenderTiming::Enabled()) {
			for (const auto& copy: copies) payload_size += copy.size;
			using Counter = Common::RenderTiming::Counter;
			Common::RenderTiming::Add(Counter::SyncBufferDownloads);
			Common::RenderTiming::Add(Counter::SyncDownloadPayloadBytes, payload_size);
			Common::RenderTiming::Add(Counter::SyncDownloadStagingBytes, total_size);
		}
	}
	if (GpuSliceTrace::current != nullptr)
		for (const auto& copy: copies) GpuSliceTrace::current->buffer_readback_bytes += copy.size;

    if (witnesses) {
        // Never widen a readback to obtain a witness: partial-page padding can
        // be stale natively. One existing copy must contain the ENTIRE page.
        for (const auto& copy: copies) {
            const auto begin = Common::AlignUp(buffer_address + copy.srcOffset, TRACKER_PAGE_SIZE);
            const auto end = Common::AlignDown(buffer_address + copy.srcOffset + copy.size, TRACKER_PAGE_SIZE);
            if (begin >= end) continue;
            DownloadWitnessSpan span;
            span.address = begin;
            span.size = end - begin;
            if (!LibKernel::Memory::QueryGuestBackingRanges(begin, span.size, &span.backing) ||
                std::any_of(span.backing.begin(), span.backing.end(), [](const auto& range) {
                    return range.kind == LibKernel::Memory::GuestBackingKind::Private;
                })) continue;
            span.hashes.resize(span.size / TRACKER_PAGE_SIZE);
            witnesses->push_back(std::move(span));
        }
    }
	auto [mapped, offset] = m_download_buffer.Map(total_size, 64);
	std::unique_ptr<Buffer> temporary;
	if (mapped == nullptr) {
		temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Download, 0,
		                                     vk::BufferUsageFlagBits::eTransferDst, total_size);
		mapped = temporary->Mapped().data();
	} else {
		m_download_buffer.Commit();
	}
	const auto& download = temporary ? *temporary : m_download_buffer;
	for (auto& copy: copies) {
		copy.dstOffset += offset;
	}

	auto& command = m_scheduler.Current();
	command.EndRendering();
	const auto              native = command.Handle();
	vk::BufferMemoryBarrier before {};
	before.srcAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.buffer              = buffer.Handle();
	before.offset              = 0;
	before.size                = buffer.Size();
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &before, 0,
	                       nullptr);
	native.copyBuffer(buffer.Handle(), download.Handle(),
	                  static_cast<uint32_t>(copies.size()), copies.data());

	auto after          = before;
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eHostRead;
	after.buffer        = download.Handle();
	after.offset        = offset;
	after.size          = total_size;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands |
	                           vk::PipelineStageFlagBits::eHost,
	                       {}, 0, nullptr, 1, &after, 0, nullptr);
	auto publish = [this, mapped, offset, total_size, buffer_address,
	                                    copies = std::move(copies), owner = std::move(temporary), witnesses = std::move(witnesses)] {
		(owner ? *owner : m_download_buffer).Invalidate(offset, total_size);
		for (const auto& copy: copies) {
			Libs::LibKernel::Memory::WriteBacking(buffer_address + copy.srcOffset,
			                                      mapped + (copy.dstOffset - offset), copy.size);
		}
        if (witnesses) for (auto& span: *witnesses) {
            const auto copy = std::find_if(copies.begin(), copies.end(), [&](const auto& candidate) {
                const auto begin = buffer_address + candidate.srcOffset;
                return begin <= span.address && span.address + span.size <= begin + candidate.size;
            });
            EXIT_IF(copy == copies.end());
            const auto* bytes = mapped + (copy->dstOffset - offset) +
                (span.address - buffer_address - copy->srcOffset);
            for (size_t page = 0; page < span.hashes.size(); ++page)
                span.hashes[page] = XXH3_64bits(bytes + page * TRACKER_PAGE_SIZE, TRACKER_PAGE_SIZE);
        }
	};
	if constexpr (async) {
		m_scheduler.DeferPriorityOperation(std::move(publish));
	} else {
		const auto tick = m_scheduler.CurrentTick();
		uint64_t native_wait_ns = 0;
		{
			Common::RenderTiming::ScopedSyncDownloadGpuWait download_wait;
			m_scheduler.Wait(tick);
			native_wait_ns = download_wait.ElapsedNs();
		}
		const auto priority_begin = Common::RenderTiming::Begin();
		m_scheduler.WaitPriorityOperations(tick);
		Common::RenderTiming::Finish(
		    Common::RenderTiming::Counter::SyncDownloadPriorityWaitNs, priority_begin);
		publish();
		if (Common::RenderTiming::Enabled()) {
			Common::RenderTiming::RecordReadback({
			    .request_address = vaddr, .request_size = size,
			    .buffer_address = buffer_address, .buffer_size = buffer.Size(),
			    .payload_bytes = payload_size, .staging_bytes = total_size,
			    .native_wait_ns = native_wait_ns,
			    .origin = Common::RenderTiming::CurrentReadbackOrigin()
			});
		}
	}
	return true;
}

void BufferCache::InstallDownloadWitnesses(BufferId id, vk::Buffer native, const DownloadWitnesses& witnesses) {
    const auto* buffer = m_slot_buffers.try_get(id);
    if (buffer == nullptr || buffer->is_deleted || buffer->Handle() != native) return;
    uint64_t installed = 0, cpu_dirty = 0;
    for (const auto& span: witnesses) {
        std::vector<LibKernel::Memory::GuestBackingRange> current;
        if (!buffer->IsInBounds(span.address, span.size) ||
            !LibKernel::Memory::QueryGuestBackingRanges(span.address, span.size, &current) ||
            current.size() != span.backing.size()) continue;
        const bool same = std::equal(current.begin(), current.end(), span.backing.begin(),
            [](const auto& a, const auto& b) {
                return a.address == b.address && a.size == b.size && a.backing_id == b.backing_id &&
                    a.backing_offset == b.backing_offset && a.mapping_id == b.mapping_id &&
                    a.protection == b.protection && a.kind == b.kind;
            });
        if (!same) continue;
        for (size_t page = 0; page < span.hashes.size(); ++page) {
            const auto address = span.address + page * TRACKER_PAGE_SIZE;
            if (m_memory_tracker.IsRegionGpuModified(address, TRACKER_PAGE_SIZE)) continue;
            // Preserve every explicit dirty cause, including another VA's
            // publication of the same physical page. Hash equality never
            // clears that state or grants residency beyond the epoch.
            cpu_dirty += m_memory_tracker.IsRegionCpuModified(address, TRACKER_PAGE_SIZE) ? 1 : 0;
            m_backing_snapshots.PublishCompletedDownloadHash(address, TRACKER_PAGE_SIZE, span.hashes[page]);
            ++installed;
        }
    }
    m_publication_witness_pages += installed;
    m_publication_witness_cpu_dirty_pages += cpu_dirty;
    if (installed != 0 && (++m_publication_witness_receipts <= 5 || GpuSliceTrace::current != nullptr))
        std::fprintf(stderr, "PS5_BUFFER_PUBLICATION_WITNESS_V1 event=install pages=%" PRIu64
            " cpu_dirty_pages=%" PRIu64 " cumulative_pages=%" PRIu64
            " full_page_bytes=%u source=completed_native_copy eager=1\n",
            installed, cpu_dirty, m_publication_witness_pages, unsigned(TRACKER_PAGE_SIZE));
}

BufferCache::BufferCache(GraphicContext& graphics, CommandScheduler& scheduler,
                         PageManager& page_manager, TextureCache& texture_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_fault_manager(graphics, scheduler, *this),
      m_gds_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, GdsBufferSize),
      m_bda_pagetable_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                             BDA_PAGETABLE_SIZE),
      m_memory_tracker(page_manager),
      m_staging_buffer(graphics, scheduler, MemoryUsage::Upload, 512 * MiB),
      m_stream_buffer(graphics, scheduler, MemoryUsage::Stream, 64 * MiB),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 64 * MiB),
      m_device_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 128 * MiB),
      m_texture_cache(texture_cache) {
	std::memset(m_gds_buffer.Mapped().data(), 0, static_cast<size_t>(m_gds_buffer.Size()));
	m_gds_buffer.Flush(0, m_gds_buffer.Size());
	SetVulkanObjectNameF(m_graphics.device, m_bda_pagetable_buffer.Handle(),
	                     "BDA Page Table Buffer");
	const auto null_id =
	    m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
	EXIT_IF(null_id != NULL_BUFFER_ID);
	SetVulkanObjectNameF(m_graphics.device, GetBuffer(null_id).Handle(), "Kyty.NullBuffer");
	if (m_graphics.CanReportMemoryUsage()) {
		const auto thresholds = MemoryBudget::BufferThresholds(m_graphics.GetTotalMemoryBudget());
		m_trigger_gc_memory = thresholds.trigger;
		m_critical_gc_memory = thresholds.critical;
	}
}

BufferCache::~BufferCache() {
	if (!m_gpu_modified_ranges.Empty()) {
		EXIT("BufferCache: destroyed with pending GPU-modified ranges\n");
	}
	for (const auto& [vaddr, id]: m_buffers) {
		(void)vaddr;
		const auto& buffer = m_slot_buffers[id];
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: destroyed with GPU-modified buffer\n");
		}
	}
	m_buffers.clear();
}

void BufferCache::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid memory-invalidation range\n");
	}
	m_memory_tracker.InvalidateRegion(vaddr, size,
	                                  [this, vaddr, size] { ReadMemory(vaddr, size, true); });
}

void BufferCache::RetireBackingSnapshots(uint64_t vaddr, uint64_t size) {
    m_backing_snapshots.Forget(vaddr, size);
}

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write) {
	if (!GuestGpu::IsGpuThread() && CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto caller_origin = Common::RenderTiming::CurrentReadbackOrigin();
	m_scheduler.Context().GetGpu().SendCommandSync([this, vaddr, size, is_write, caller_origin] {
		// The faulting caller may be a guest CPU thread. Carry its diagnostic
		// origin explicitly across the synchronous hand-off to the renderer.
		using Origin = Common::RenderTiming::ReadbackOrigin;
		Common::RenderTiming::ScopedReadbackOrigin origin(
		    is_write && caller_origin == Origin::Other ? Origin::Invalidation : caller_origin);
		if (is_write && !IsRegionRegistered(vaddr, size)) {
			return;
		}
		auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];

		if (DownloadBufferMemory<false>(buffer, vaddr, size)) {
			m_memory_tracker.UnmarkRegionAsGpuModified(vaddr, size);
		}
		if (is_write) {
			m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
		}
	});
}

BufferId BufferCache::FindBuffer(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0) {
		return NULL_BUFFER_ID;
	}
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid buffer discovery request\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			return *owner;
		}
	}
	return CreateBuffer(vaddr, size);
}

BufferCache::OverlapResult BufferCache::ResolveOverlaps(uint64_t vaddr, uint64_t size) {
	static constexpr int      StreamLeapThreshold = 16;
	static constexpr uint64_t StreamLeapSize      = CACHING_PAGESIZE * 128;

	auto       begin      = vaddr;
	auto       end        = vaddr + size;
	const auto find_first = [&](uint64_t address) {
		auto first = m_buffers.lower_bound(address);
		if (first != m_buffers.begin()) {
			const auto  previous = std::prev(first);
			const auto& buffer   = m_slot_buffers[previous->second];
			if (buffer.CpuAddress() + buffer.Size() > address) {
				first = previous;
			}
		}
		return first;
	};
	auto first           = find_first(begin);
	auto last            = first;
	int  stream_score    = 0;
	bool has_stream_leap = false;
	for (; last != m_buffers.end() && last->first < end; ++last) {
		const auto& buffer        = m_slot_buffers[last->second];
		const auto  buffer_begin  = buffer.CpuAddress();
		const auto  buffer_end    = buffer_begin + buffer.Size();
		const bool  expands_left  = buffer_begin < begin;
		const bool  expands_right = buffer_end > end;
		begin                     = std::min(begin, buffer_begin);
		end                       = std::max(end, buffer_end);
		if (!has_stream_leap && (stream_score += buffer.StreamScore()) > StreamLeapThreshold) {
			has_stream_leap = true;
			// Reserve space in the incoming stream's direction of growth.
			// The old buffer extending left of the request predicts growth to the right, and vice versa.
			if (expands_left) {
				end += std::min(StreamLeapSize, (vaddr < LOWER_ADDRESS_SIZE ? LOWER_ADDRESS_SIZE
				                                       : LibKernel::Memory::kExtendedMemoryBase +
				                                             LibKernel::Memory::kExtendedMemorySize) - end);
			}
			if (expands_right) {
				const auto minimum = vaddr < LOWER_ADDRESS_SIZE
				                         ? CACHING_PAGESIZE * 2
				                         : LibKernel::Memory::kExtendedMemoryBase;
				if (begin > minimum) {
					begin -= std::min(StreamLeapSize, begin - minimum);
				}
				first = find_first(begin);
				begin = std::min(begin, first->first);
			}
		}
	}
	return {first, last, begin, end, has_stream_leap};
}

void BufferCache::JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score) {
	auto& new_buffer = m_slot_buffers[new_id];
	auto& overlap    = m_slot_buffers[overlap_id];
	if (accumulate_stream_score) {
		new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
	}
	EXIT_IF(!new_buffer.IsInBounds(overlap.CpuAddress(), overlap.Size()));
	new_buffer.CopyFrom(m_scheduler.Current(), overlap, 0,
	                    overlap.CpuAddress() - new_buffer.CpuAddress(), overlap.Size());
	// This is a native owner transfer over the same guest backing, not a
	// backing retirement. The replacement keeps all old native bytes and
	// VA-keyed dirty pages, so it must keep the original staged snapshots.
	// Freshly hashing backing here would conceal a real concurrent CPU write.
	DeleteBuffer(overlap_id, SnapshotDisposition::Transfer);
}

BufferId BufferCache::CreateBuffer(uint64_t vaddr, uint64_t size) {
	EXIT_IF(m_scheduler.Current().IsInvalid());
	const auto end = Common::AlignUp(vaddr + size, CACHING_PAGESIZE);
	vaddr = Common::AlignDown(vaddr, CACHING_PAGESIZE);
	size               = end - vaddr;
	const auto overlap = ResolveOverlaps(vaddr, size);

	const auto id = m_slot_buffers.insert(
	    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, overlap.begin,
	    AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, overlap.end - overlap.begin);
	const auto& buffer = m_slot_buffers[id];
	SetVulkanObjectNameF(m_graphics.device, buffer.Handle(),
	                     "Kyty.GameBuffer[guest=0x{:016x} size=0x{:x}]", overlap.begin,
	                     overlap.end - overlap.begin);
	for (auto it = overlap.first; it != overlap.last;) {
		const auto old_id = (it++)->second;
		JoinOverlap(id, old_id, !overlap.has_stream_leap);
	}
	Register(id);
	return id;
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_written,
                                     bool is_texel_buffer, SynchronizeSource synchronize_source) {
    if (is_written && !GuestGpuWriteOwnershipSupported())
        EXIT("PS5 GPU guest-buffer writes require completed CPU backing publication; unavailable\n");
    if (is_written && !LibKernel::Memory::QueryGpuBackingWrite(vaddr, size))
        EXIT("PS5 GPU buffer destination lacks shared non-executable GPU-write backing\n");
    RangeSet publication_reuse;
    RevalidateBacking(buffer, vaddr, size,
        BufferPublicationWitnessEnabled() ? &publication_reuse : nullptr);
    // Revalidation marks only newly changed pages while the FEX epoch pins
    // their contents. Native execution lacks that pin and retains the old path.
    if (!GuestBufferSnapshotReuseEnabled() || !GuestGpuWriteOwnershipSupported())
	    m_memory_tracker.RevalidateCpuBacking(vaddr, size);
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size = 0;
	vk::Buffer                  source;
	m_memory_tracker.ForEachUploadRange(
	    vaddr, size, is_written,
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    copies.emplace_back(total_size, buffer.Offset(address), bytes);
		    total_size += bytes;
	    },
	    [&]() noexcept { source = UploadCopies(buffer, copies, total_size); });
    uint64_t avoided = 0;
    publication_reuse.ForEach([&](uint64_t begin, uint64_t end) {
        for (auto address = begin; address < end; address += TRACKER_PAGE_SIZE) {
            // Counting only the first real native consumer after publication
            // avoids crediting repeated validations as repeated saved uploads.
            m_backing_snapshots.ConsumePublicationWitness(address);
            avoided += TRACKER_PAGE_SIZE;
        }
    });
    m_publication_witness_upload_avoided_bytes += avoided;
    if (avoided != 0 && (++m_publication_reuse_receipts <= 5 || GpuSliceTrace::current != nullptr))
        std::fprintf(stderr, "PS5_BUFFER_PUBLICATION_WITNESS_V1 event=upload_avoided bytes=%" PRIu64
            " cumulative_bytes=%" PRIu64 " first_consumer=1 fresh_revalidation=1\n",
            avoided, m_publication_witness_upload_avoided_bytes);
	if (source) {
		auto& command = m_scheduler.Current();
		command.EndRendering();
		const auto native = command.Handle();
		vk::BufferMemoryBarrier before {};
		before.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
		                       vk::AccessFlagBits::eTransferRead |
		                       vk::AccessFlagBits::eTransferWrite;
		before.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer              = buffer.Handle();
		before.offset              = 0;
		before.size                = buffer.Size();
		native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                       vk::PipelineStageFlagBits::eTransfer,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &before, 0, nullptr);
		native.copyBuffer(source, buffer.Handle(), static_cast<uint32_t>(copies.size()),
		                  copies.data());
		auto after          = before;
		after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                       vk::PipelineStageFlagBits::eAllCommands,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr);
	}
	if (synchronize_source == SynchronizeSource::BufferConsumer && !is_written &&
	    (is_texel_buffer || m_texture_cache.IsRegionGpuModified(vaddr, size))) {
		const bool synchronized = SynchronizeBufferFromImage(buffer, vaddr, size);
		if (!synchronized && m_texture_cache.IsRegionGpuModified(vaddr, size))
			EXIT("BufferCache: GPU-owned image source has no complete native synchronization\n");
		return synchronized;
	}
	return false;
}

void BufferCache::ReconcileCpuChangedOwnedPage(Buffer& buffer, uint64_t address, const uint8_t* current) {
    std::array<uint8_t, TRACKER_PAGE_SIZE> previous {};
    if (GuestAsyncCpuConcurrentExperimentEnabled() || !GuestGpuWriteOwnershipSupported() ||
        !buffer.IsInBounds(address, TRACKER_PAGE_SIZE) ||
        !m_backing_snapshots.CopyPublishedBytes(address, previous.data(), previous.size()))
        EXIT("PS5 experimental CPU/GPU reconciliation has no complete staged baseline\n");
    // The FEX epoch excludes CPU writers now. Download only this conflicted
    // page, then preserve every CPU byte that changed since the staged copy.
    // This is an experimental delta policy: same-value stores are invisible.
    if (!DownloadBufferMemory<true>(buffer, address, TRACKER_PAGE_SIZE))
        EXIT("PS5 experimental CPU/GPU reconciliation has no dirty byte owner\n");
    const auto tick = m_scheduler.CurrentTick();
    m_scheduler.Finish();
    m_scheduler.WaitPriorityOperations(tick);
    m_memory_tracker.UnmarkRegionAsGpuModified(address, TRACKER_PAGE_SIZE);
    m_backing_snapshots.Forget(address, TRACKER_PAGE_SIZE);
    for (size_t begin = 0; begin < previous.size();) {
        if (previous[begin] == current[begin]) { ++begin; continue; }
        size_t end = begin + 1;
        while (end < previous.size() && previous[end] != current[end]) ++end;
        if (!LibKernel::Memory::QueryGpuBackingWrite(address + begin, end - begin))
            EXIT("PS5 experimental CPU delta lost writable non-executable backing\n");
        LibKernel::Memory::WriteBacking(address + begin, current + begin, end - begin);
        begin = end;
    }
    m_memory_tracker.MarkRegionAsCpuModified(address, TRACKER_PAGE_SIZE);
    m_scheduler.Context().InvalidatePhysicalAliases(address, TRACKER_PAGE_SIZE, false);
}

void BufferCache::RevalidateBacking(Buffer& buffer, uint64_t vaddr, uint64_t size,
                                    RangeSet* publication_reuse) {
    std::array<uint8_t, TRACKER_PAGE_SIZE> snapshot {};
    const bool reuse_unchanged = GuestBufferSnapshotReuseEnabled() &&
        GuestGpuWriteOwnershipSupported();
    const bool hash_selected = BufferContiguousHashEnabled();
    // Async reconciliation consumes actual copied bytes. An epoch token is
    // never a memo key: the fast path scans every byte again on every call.
    const bool hash_admitted = hash_selected && GuestGpuWriteOwnershipSupported() &&
        !GuestAsyncCpuVisibleExperimentEnabled() && !GuestAsyncCpuConcurrentExperimentRequested();
    auto* trace = GpuSliceTrace::current;
    const auto end = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
    for (auto address = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE); address < end;
         address += TRACKER_PAGE_SIZE) {
        BackingComparison comparison;
        bool hashed = false;
        uint32_t denial = 0;
        if (hash_selected) {
            if (trace != nullptr) ++trace->buffer_hash_considered;
            if (!hash_admitted) denial = 1; // Preserve byte-taking/unqualified path.
            else if (!m_backing_snapshots.HasSnapshot(address, snapshot.size())) denial = 2;
            else {
                uint64_t hash = 0;
                if (trace != nullptr) ++trace->buffer_hash_attempts;
                hashed = LibKernel::Memory::TryHashContiguousBacking(address, snapshot.size(), &hash);
                if (hashed) comparison = m_backing_snapshots.CompareHash(address, snapshot.size(), hash);
                else denial = 4; // Entire single shared backing/view not proved.
            }
            if (hashed) {
                ++m_contiguous_hash_pages;
                if (trace != nullptr) {
                    ++trace->buffer_hash_pages;
                    trace->buffer_hash_bytes += snapshot.size();
                    trace->buffer_hash_copy_avoided_bytes += snapshot.size();
                }
            } else {
                ++m_contiguous_hash_fallbacks;
                if (trace != nullptr) {
                    ++trace->buffer_hash_fallbacks;
                    trace->buffer_hash_fallback_mask |= denial;
                }
            }
            const auto receipt = hashed ? m_contiguous_hash_pages : m_contiguous_hash_fallbacks;
            if (receipt <= 5) std::fprintf(stderr,
                "PS5_BUFFER_CONTIGUOUS_HASH_V1 result=%s receipt=%" PRIu64
                " per_cache_result_limit=5 bytes=%zu denial_mask=%u same_hash=xxh3_64"
                " scan=full_span memo=0\n", hashed ? "hashed" : "fallback", receipt,
                snapshot.size(), denial);
        }
        if (!hashed) {
            if (!LibKernel::Memory::TryReadSparseBacking(address, snapshot.data(), snapshot.size()))
                EXIT("BufferCache: failed to snapshot guest backing\n");
            if (trace != nullptr) trace->buffer_revalidation_copy_bytes += snapshot.size();
            comparison = m_backing_snapshots.Compare(address, snapshot.data(), snapshot.size());
        }
        const auto action = ClassifyBackingRevalidation(comparison,
            m_memory_tracker.IsRegionGpuModified(address, TRACKER_PAGE_SIZE), reuse_unchanged);
        if (action == BackingRevalidation::RejectGpuOwner &&
            comparison == BackingComparison::Changed && GuestAsyncCpuVisibleExperimentEnabled() &&
            !GuestAsyncCpuConcurrentExperimentEnabled()) {
            ReconcileCpuChangedOwnedPage(buffer, address, snapshot.data());
            continue;
        }
        // CPU delta reconciliation requires a stopped CPU. The concurrent
        // experiment keeps this conflict as an explicit failure instead of
        // claiming that a moving CPU snapshot can be safely merged.
        if (action == BackingRevalidation::RejectGpuOwner)
            EXIT("BufferCache: GPU-owned backing has no unchanged CPU snapshot; page=0x%016llx consumer=0x%016llx size=0x%llx comparison=%u\n",
                static_cast<unsigned long long>(address), static_cast<unsigned long long>(vaddr),
                static_cast<unsigned long long>(size), static_cast<unsigned>(comparison));
        if (publication_reuse != nullptr && action == BackingRevalidation::KeepCpuState &&
            m_backing_snapshots.IsPublicationWitness(address, TRACKER_PAGE_SIZE) &&
            !m_memory_tracker.IsRegionCpuModified(address, TRACKER_PAGE_SIZE))
            publication_reuse->Add(address, TRACKER_PAGE_SIZE);
        if (action == BackingRevalidation::KeepGpuOwner) {
            if (!GuestGpuWriteOwnershipSupported())
                EXIT("PS5 GPU buffer ownership requires completed CPU backing publication; unavailable\n");
        } else if (reuse_unchanged && action == BackingRevalidation::MarkCpuModified) {
            m_memory_tracker.MarkRegionAsCpuModified(address, TRACKER_PAGE_SIZE);
        }
    }
}

void BufferCache::PublishUploadSnapshot(uint64_t address, const uint8_t* copied, uint64_t size) {
    if ((address % TRACKER_PAGE_SIZE) != 0 || (size % TRACKER_PAGE_SIZE) != 0)
        EXIT("BufferCache: upload snapshot must contain complete tracking pages\n");
    for (uint64_t offset = 0; offset < size; offset += TRACKER_PAGE_SIZE)
        m_backing_snapshots.Publish(address + offset, copied + offset, TRACKER_PAGE_SIZE);
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     uint64_t total_size) {
	if (copies.empty()) {
		return nullptr;
	}
    m_guest_upload_bytes += total_size;

	auto [mapped, base_offset] = m_staging_buffer.Map(total_size, 4);
	if (mapped != nullptr) {
		for (auto& copy: copies) {
			const auto address = buffer.CpuAddress() + copy.dstOffset;
            if (!Libs::LibKernel::Memory::TryReadSparseBacking(address, mapped + copy.srcOffset, copy.size))
                EXIT("BufferCache: failed to copy guest upload backing\n");
            PublishUploadSnapshot(address, mapped + copy.srcOffset, copy.size);
			copy.srcOffset += base_offset;
		}
		m_staging_buffer.Commit();
		return m_staging_buffer.Handle();
	}

	auto temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Upload, 0,
	                                         vk::BufferUsageFlagBits::eTransferSrc, total_size);
	for (const auto& copy: copies) {
		const auto address = buffer.CpuAddress() + copy.dstOffset;
        auto* copied = temporary->Mapped().data() + copy.srcOffset;
        if (!Libs::LibKernel::Memory::TryReadSparseBacking(address, copied, copy.size))
            EXIT("BufferCache: failed to copy temporary guest upload backing\n");
        PublishUploadSnapshot(address, copied, copy.size);
	}
	temporary->Flush(0, total_size);
	const auto handle = temporary->Handle();
	m_scheduler.DeferOperation([owner = std::move(temporary)]() mutable { owner.reset(); });
	return handle;
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer(uint64_t vaddr, uint64_t size,
                                                       bool is_written, bool is_texel_buffer,
                                                       BufferId id) {
	if (is_written) InvalidateUniformMetadata(vaddr, size);
    if (is_written && !GuestGpuWriteOwnershipSupported())
        EXIT("PS5 GPU guest-buffer writes require completed CPU backing publication; unavailable\n");
    m_scheduler.Context().PublishPhysicalAliasDependencies(vaddr, size);
	auto& command = m_scheduler.Current();
	if (command.IsInvalid() || !GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: buffer request requires a recording command buffer\n");
	}
	const bool image_source = !is_written && m_texture_cache.IsRegionGpuModified(vaddr, size);
	GuestRange synchronize {vaddr, size};
	if (image_source) {
		const auto image = m_texture_cache.FindImageFromRange(vaddr, size);
		if (!image) EXIT("BufferCache: GPU-owned image source has no unique authoritative owner\n");
		synchronize = m_texture_cache.GetImage(image).info.data;
		// A request for a few stencil bytes must not allocate only those bytes
		// and then consume the old CPU backing. Download the complete owner into
		// a full-sized native buffer before returning the requested subrange.
		if (IsBufferInvalid(id) || !m_slot_buffers[id].IsInBounds(synchronize.address, synchronize.size))
			id = FindBuffer(synchronize.address, synchronize.size);
	}

	if (!is_written && !image_source && size <= CACHING_PAGESIZE &&
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
	    m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
		const auto alignment = std::max<uint64_t>(
		    m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 1);
		auto [mapped, offset] = m_stream_buffer.Map(size, alignment, false);
		if (mapped != nullptr) {
            if (!Libs::LibKernel::Memory::TryReadSparseBacking(vaddr, mapped, size))
                EXIT("BufferCache: failed to read streamed guest backing\n");
			m_stream_buffer.Commit();
			return {&m_stream_buffer, offset};
		}
	}

	if (IsBufferInvalid(id) || !m_slot_buffers[id].IsInBounds(vaddr, size)) {
		id = FindBuffer(vaddr, size);
	}
	auto& buffer = m_slot_buffers[id];
	TouchBuffer(buffer);
	(void)SynchronizeBuffer(buffer, synchronize.address, synchronize.size, is_written, is_texel_buffer);
	if (is_written) {
		m_gpu_modified_ranges.Add(vaddr, size);
	}
	return {&buffer, buffer.Offset(vaddr)};
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size,
                                                            std::span<uint8_t> capture,
                                                            bool* captured) {
	if (captured != nullptr) *captured = false;
	EXIT_IF(!capture.empty() && (capture.size() != size || captured == nullptr));
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid image source\n");
	}
	// InitializeImage owns the texture lock. Never feed an upload old CPU
	// bytes while another native image/association still owns this surface.
	if (m_texture_cache.IsRegionGpuModifiedLocked(vaddr, size)) {
		for (const auto id: m_texture_cache.FindImagesInRegion(vaddr, size, false)) {
			const auto* owner = m_texture_cache.m_slot_images.try_get(id);
			if (owner == nullptr || !owner->registered || !owner->IsGpuModified()) continue;
			const auto& info = owner->info;
			std::printf("PS5_IMAGE_UPLOAD_DEPENDENCY owner=0x%016" PRIx64 " size=0x%016" PRIx64
			            " format=%u host_format=%u tile=%u extent=%ux%ux%u pitch=%u levels=%u"
			            " layers=%u samples=%u native_samples=%u depth=%u association=%u"
			            " cpu_dirty=%u buffer_dirty=%u safe_download=%u metadata=%u"
			            " stencil=0x%016" PRIx64 " stencil_size=0x%016" PRIx64 "\n",
			            info.data.address, info.data.size, static_cast<uint32_t>(info.guest_format),
			            static_cast<uint32_t>(info.pixel_format), static_cast<uint32_t>(info.tile_mode),
			            info.extent.width, info.extent.height, info.extent.depth, info.pitch,
			            info.resources.levels, info.resources.layers, info.samples, owner->backing.samples,
			            info.IsDepth() ? 1u : 0u, owner->depth_id ? 1u : 0u,
			            owner->IsCpuDirty() ? 1u : 0u, owner->IsBufferModified() ? 1u : 0u,
			            owner->SafeToDownload() ? 1u : 0u, static_cast<uint32_t>(info.metadata.kind),
			            info.stencil.address, info.stencil.size);
		}
		std::fflush(stdout);
		EXIT("BufferCache: image upload source retains unpublished native image ownership:"
		     " request=0x%016" PRIx64 " size=0x%016" PRIx64 "\n", vaddr, size);
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			TouchBuffer(buffer);
			(void)SynchronizeBuffer(buffer, vaddr, size, false, false, SynchronizeSource::ImageUpload);
			return {&buffer, buffer.Offset(vaddr)};
		}
	}
	if (IsRegionGpuModified(vaddr, size)) {
		const auto id = FindBuffer(vaddr, size);
		auto& buffer = m_slot_buffers[id];
		TouchBuffer(buffer);
		(void)SynchronizeBuffer(buffer, vaddr, size, false, false, SynchronizeSource::ImageUpload);
		return {&buffer, buffer.Offset(vaddr)};
	}

	auto [staging, stage_offset] = m_staging_buffer.Map(size, 16);
	if (staging == nullptr || !Libs::LibKernel::Memory::TryReadSparseBacking(vaddr, staging, size)) {
		EXIT("BufferCache: failed to read mapped guest image backing\n");
	}
	if (!capture.empty()) {
		std::memcpy(capture.data(), staging, capture.size());
		*captured = true;
	}
	m_staging_buffer.Commit();
	return {&m_staging_buffer, stage_offset};
}

void BufferCache::FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds) {
    if (!is_gds && !GuestGpuWriteOwnershipSupported())
        EXIT("PS5 GPU guest-memory fills require write rights and completed CPU backing publication; unavailable\n");
    if (!is_gds && !LibKernel::Memory::QueryGpuBackingWrite(vaddr, size))
        EXIT("PS5 GPU fill destination lacks GPU-write backing\n");
	if ((vaddr & 3u) != 0 || size == 0 || (size & 3u) != 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: fill range must be dword aligned\n");
	}
	if (is_gds) {
		if (vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - vaddr) {
			EXIT("BufferCache: GDS fill range is out of bounds\n");
		}
		m_gds_buffer.Fill(vaddr, size, value);
		return;
	}
	if (vaddr == 0) {
		EXIT("BufferCache: invalid fill memory address\n");
	}
    m_scheduler.Context().PublishPhysicalAliasDependencies(vaddr, size);
	if (m_texture_cache.IsRegionGpuModified(vaddr, size))
		EXIT("BufferCache: fill destination retains unpublished image ownership\n");
	(void)m_texture_cache.ClearMeta(vaddr);
	if (!IsRegionGpuModified(vaddr, size)) {
        ValidateBackingWrite(vaddr, size);
        // A synchronous backing mutation needs no native GPU ownership. Use
        // the owned backing alias even when CPU rights deny the guest view.
        std::array<uint32_t, 1024> copied;
        copied.fill(value);
        for (uint64_t offset = 0; offset < size; offset += sizeof(copied)) {
            const auto bytes = std::min<uint64_t>(sizeof(copied), size - offset);
            if (!Libs::LibKernel::Memory::TryWriteBacking(vaddr + offset, copied.data(), bytes))
                EXIT("BufferCache: failed to fill guest backing\n");
        }
        m_scheduler.Context().InvalidatePhysicalAliases(vaddr, size, true);
		return;
	}

	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true);
	dst->Fill(dst_offset, size, value);
}

void BufferCache::InvalidateUniformMetadata(uint64_t vaddr, uint64_t size) {
	if (Common::UniformMetadata::Enabled()) m_uniform_metadata.Invalidate(vaddr, size);
}

void BufferCache::ClearUniformMetadata() {
	if (Common::UniformMetadata::Enabled()) m_uniform_metadata.Clear();
}

bool BufferCache::TryGetUniformMetadata(uint64_t vaddr, uint64_t size,
                                        uint64_t generation, uint32_t* value) {
	return Common::UniformMetadata::Enabled() && generation != 0 && (generation & 1) == 0 &&
	       LibKernel::Memory::BackingMutationGeneration() == generation &&
	       !m_texture_cache.IsRegionRegistered(vaddr, size) &&
	       m_uniform_metadata.Lookup(vaddr, size, generation, value);
}

bool BufferCache::TryFillUniformMetadata(uint64_t vaddr, uint64_t size, uint32_t value) {
	if (!Common::UniformMetadata::Enabled() || !GuestRange {vaddr, size}.Valid() ||
	    ((vaddr | size) & 3u) != 0 || m_texture_cache.IsRegionRegistered(vaddr, size)) return false;
	uint64_t generation = 0;
	if (!LibKernel::Memory::QueryUniqueBackingGeneration(vaddr, size, &generation)) return false;
	FillUniformMetadataGpu(vaddr, size, value, generation);
	return true;
}

void BufferCache::FillUniformMetadataGpu(uint64_t vaddr, uint64_t size, uint32_t value,
                                        uint64_t generation) {
	if (!GuestRange {vaddr, size}.Valid() || ((vaddr | size) & 3u) != 0) {
		EXIT("BufferCache: GPU metadata fill must be valid and dword aligned\n");
	}
	if (!GuestGpuWriteOwnershipSupported())
		EXIT("PS5 GPU guest-memory fills require completed CPU backing publication; unavailable\n");
	if (!LibKernel::Memory::QueryGpuBackingWrite(vaddr, size))
		EXIT("PS5 GPU fill destination lacks GPU-write backing\n");
	m_scheduler.Context().PublishPhysicalAliasDependencies(vaddr, size);
	if (m_texture_cache.IsRegionGpuModified(vaddr, size))
		EXIT("BufferCache: fill destination retains unpublished image ownership\n");
	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true);
	dst->Fill(dst_offset, size, value);
	// Only remember after recording the real write. A changed VM snapshot
	// discards this optimization; it never permits early CPU publication.
	if (Common::UniformMetadata::Enabled() && generation != 0 && (generation & 1) == 0 &&
	    LibKernel::Memory::BackingMutationGeneration() == generation) {
		m_uniform_metadata.Remember(vaddr, size, value, generation);
	}
}

void BufferCache::CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
                             bool src_gds) {
	const bool dst_memory = !dst_gds;
	const bool src_memory = !src_gds;
    if (dst_memory && !GuestGpuWriteOwnershipSupported())
        EXIT("PS5 GPU guest-memory copies require write rights and completed CPU backing publication; unavailable\n");
    if (dst_memory && !LibKernel::Memory::QueryGpuBackingWrite(dst_vaddr, size))
        EXIT("PS5 GPU copy destination lacks GPU-write backing\n");
	if ((dst_memory && dst_vaddr == 0) || (src_memory && src_vaddr == 0) || size == 0 ||
	    ((dst_gds || src_gds) && ((dst_vaddr | src_vaddr | size) & 3u) != 0) ||
	    size > UINT64_MAX - dst_vaddr || size > UINT64_MAX - src_vaddr || (dst_gds && src_gds) ||
	    (dst_gds && (dst_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - dst_vaddr)) ||
	    (src_gds && (src_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - src_vaddr))) {
		EXIT("BufferCache: invalid copy range, src=0x%016" PRIx64 " dst=0x%016" PRIx64
		     " size=0x%016" PRIx64 " src_gds=%d dst_gds=%d\n",
		     src_vaddr, dst_vaddr, size, static_cast<int>(src_gds), static_cast<int>(dst_gds));
	}
    bool physical_overlap = false;
    if (src_memory && dst_memory) {
        std::vector<LibKernel::Memory::GuestBackingAlias> aliases;
        if (!LibKernel::Memory::QueryGuestBackingAliases(src_vaddr, size, &aliases))
            EXIT("BufferCache: copy source has no complete physical backing identity\n");
        for (const auto& alias: aliases) {
            if (!alias.source && alias.address < dst_vaddr + size && dst_vaddr < alias.address + alias.size)
                physical_overlap = true;
        }
    }
    if (src_memory) m_scheduler.Context().PublishPhysicalAliasDependencies(src_vaddr, size);
    if (dst_memory) m_scheduler.Context().PublishPhysicalAliasDependencies(dst_vaddr, size);
	if (dst_memory && m_texture_cache.IsRegionGpuModified(dst_vaddr, size))
		EXIT("BufferCache: copy destination retains unpublished image ownership\n");
	if (src_memory && dst_memory && !physical_overlap && !IsRegionGpuModified(dst_vaddr, size) &&
	    !IsRegionGpuModified(src_vaddr, size) &&
	    !m_texture_cache.IsRegionGpuModified(src_vaddr, size) &&
	    !m_texture_cache.FindImageFromRange(src_vaddr, size)) {
        ValidateBackingWrite(dst_vaddr, size);
        std::array<uint8_t, 4096> copied;
        const bool backwards = dst_vaddr > src_vaddr && dst_vaddr - src_vaddr < size;
        for (uint64_t remaining = size; remaining != 0;) {
            const auto bytes = std::min<uint64_t>(copied.size(), remaining);
            const auto offset = backwards ? remaining - bytes : size - remaining;
            if (!Libs::LibKernel::Memory::TryReadBacking(src_vaddr + offset, copied.data(), bytes) ||
                !Libs::LibKernel::Memory::TryWriteBacking(dst_vaddr + offset, copied.data(), bytes))
                EXIT("BufferCache: failed to copy guest backing\n");
            remaining -= bytes;
        }
        m_scheduler.Context().InvalidatePhysicalAliases(dst_vaddr, size, true);
		return;
	}

	auto& command = m_scheduler.Current();
	if (dst_memory) {
		m_texture_cache.InvalidateMemoryFromGPU(dst_vaddr, size);
	}
	const auto src_id      = src_memory ? FindBuffer(src_vaddr, size) : BufferId {};
	const auto dst_id      = dst_memory ? FindBuffer(dst_vaddr, size) : BufferId {};
	auto [src, src_offset] = src_memory ? ObtainBuffer(src_vaddr, size, false, true, src_id)
	                                    : std::pair {&m_gds_buffer, src_vaddr};
	auto [dst, dst_offset] = dst_memory ? ObtainBuffer(dst_vaddr, size, true, true, dst_id)
	                                    : std::pair {&m_gds_buffer, dst_vaddr};
    if (physical_overlap && src->Handle() == dst->Handle())
        EXIT("BufferCache: physically overlapping copy requires distinct native snapshots\n");
	dst->CopyFrom(command, *src, src_offset, dst_offset, size);
}

bool BufferCache::IsRegionRegistered(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid registered-region query\n");
	}
	// Cached buffers are ordered and non-overlapping. The last buffer beginning before the query
	// end is therefore the only possible intersection.
	const auto candidate = m_buffers.lower_bound(vaddr + size);
	if (candidate == m_buffers.begin()) {
		return false;
	}
	const auto& [address, id] = *std::prev(candidate);
	return address + m_slot_buffers[id].Size() > vaddr;
}

bool BufferCache::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionGpuModified(vaddr, size);
}

bool BufferCache::HasGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	return m_gpu_modified_ranges.Intersects(vaddr, size);
}

bool BufferCache::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionCpuModified(vaddr, size);
}

void BufferCache::RunGarbageCollector() {
	const auto tick = m_gc_tick++;
	if (m_graphics.CanReportMemoryUsage()) {
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
		// Android/UMA budgets can shrink while a session is running.
		const auto thresholds = MemoryBudget::BufferThresholds(m_graphics.GetTotalMemoryBudget());
		m_trigger_gc_memory = thresholds.trigger;
		m_critical_gc_memory = thresholds.critical;
	}
	if (m_total_used_memory < m_trigger_gc_memory) {
		return;
	}

	const bool     aggressive = m_total_used_memory >= m_critical_gc_memory;
	const uint64_t age        = std::min<uint64_t>(aggressive ? 80 : 160, tick);
	const size_t   limit      = aggressive ? 64 : 32;

	// Native reservations may cover cached unmapped holes. Preserve the exact
	// written spans before downloads consume them; only those bytes are published.
	const auto dirty_ranges = m_gpu_modified_ranges;
	std::vector<BufferId> dirty_buffers;
	size_t                retire_count = 0;
	m_lru_cache.ForEachItemBelow(tick - age, [&](BufferId id) {
		auto& buffer = m_slot_buffers[id];
		EXIT_IF(buffer.is_deleted);
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, buffer.CpuAddress(),
		                                           buffer.Size(), "garbage collection");
		const bool dirty = m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size());
		if (dirty && !aggressive) {
			return false;
		}
		if (dirty) {
			EXIT_NOT_IMPLEMENTED(!DownloadBufferMemory<true>(buffer, buffer.CpuAddress(), buffer.Size()));
			dirty_buffers.push_back(id);
		} else {
			m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
			DeleteBuffer(id);
		}
		return ++retire_count == limit;
	});
	if (dirty_buffers.empty()) {
		return;
	}

	// Publish all queued downloads before releasing their tracked pages and owners.
	const auto completion_tick = m_scheduler.CurrentTick();
	m_scheduler.Wait(completion_tick);
	m_scheduler.WaitPriorityOperations(completion_tick);
	for (const auto id: dirty_buffers) {
		auto& buffer = m_slot_buffers[id];
		m_memory_tracker.UnmarkRegionAsGpuModified(buffer.CpuAddress(), buffer.Size());
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size()) ||
		    m_gpu_modified_ranges.Intersects(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: garbage collection retained GPU ownership\n");
		}
        dirty_ranges.ForEachInRange(buffer.CpuAddress(), buffer.Size(),
            [&](uint64_t begin, uint64_t end) {
                m_scheduler.Context().InvalidatePhysicalAliases(begin, end - begin, false);
            });
		m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
		Unregister(id);
		m_slot_buffers.erase(id);
	}
}

void BufferCache::ProcessFaultBuffer() {
	ClearUniformMetadata();
	m_fault_manager.ProcessFaultBuffer();
}

void BufferCache::SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size) {
	ClearUniformMetadata();
	const auto end = vaddr + size;
	auto       it  = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		--it;
	}
	for (; it != m_buffers.end() && it->first < end; ++it) {
		auto&      buffer = m_slot_buffers[it->second];
		const auto start  = std::max(buffer.CpuAddress(), vaddr);
		const auto finish = std::min(buffer.CpuAddress() + buffer.Size(), end);
		if (start < finish) {
			(void)SynchronizeBuffer(buffer, start, finish - start, false, false);
		}
	}
}

bool BufferCache::HasCpuVisibleGpuWrites() {
    if (m_gpu_modified_ranges.Empty()) return false;
    for (const auto& [address, id]: m_buffers) {
        const auto& buffer = m_slot_buffers[id];
        if (m_gpu_modified_ranges.Intersects(address, buffer.Size()) &&
            LibKernel::Memory::QueryGuestBackingCpuVisible(address, buffer.Size())) return true;
    }
    return false;
}

void BufferCache::PublishGuestWrites(bool retain_private) {
	PublishGuestWrites(retain_private, {}, false);
}

bool BufferCache::HasGuestWriteDependency(const Buffer& buffer,
                                         std::span<const GuestRange> dependencies) {
    for (const auto& dependency: dependencies) {
        if (!dependency.Valid()) EXIT("PS5 buffer dependency has invalid guest range\n");
        const auto begin = std::max(dependency.address, buffer.CpuAddress());
        const auto end = std::min(dependency.End(), buffer.CpuAddress() + buffer.Size());
        // A native CPU store outside the exact GPU-written bytes can still
        // change a full-page staged witness. Resolve that page's owner first.
        if (begin < end && m_memory_tracker.IsRegionGpuModified(begin, end - begin)) return true;
    }
    return false;
}

void BufferCache::CollectGuestWriteDependencies(std::span<const GuestRange> dependencies,
                                               std::vector<GuestRange>& owners) {
    EXIT_IF(!GuestGpuWriteOwnershipSupported());
    for (const auto& [address, id]: m_buffers) {
        const auto& buffer = m_slot_buffers[id];
        if (!HasGuestWriteDependency(buffer, dependencies)) continue;
        if (!m_gpu_modified_ranges.Intersects(address, buffer.Size()))
            EXIT("PS5 buffer dependency has GPU page ownership without dirty bytes\n");
        m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, address, buffer.Size(),
                                                   "buffer dependency collection");
        // Publish this complete native buffer owner, but expand physical
        // identity only over its mapped dirty bytes. Cache reservations can
        // include clean unmapped holes after retirement.
        m_gpu_modified_ranges.ForEachInRange(address, buffer.Size(),
            [&](uint64_t begin, uint64_t end) { owners.push_back({begin, end - begin}); });
    }
}

void BufferCache::PublishGuestWritesInRanges(std::span<const GuestRange> dependencies) {
    if (dependencies.empty()) return;
    PublishGuestWrites(false, dependencies, true);
}

void BufferCache::PublishGuestWrites(bool retain_private,
                                    std::span<const GuestRange> dependencies,
                                    bool selected_only) {
    Lumen::Performance::Scope publication(Lumen::Performance::Phase::GpuPublishBuffers);
	EXIT_IF(!GuestGpuWriteOwnershipSupported());
    // This exact ledger is maintained by every native buffer writer, including
    // overlap transfers. A clean cache needs no owner scan. Producer/callback
    // completion remains the caller's responsibility even on this path.
    if (m_gpu_modified_ranges.Empty()) return;
	static const bool batch_enabled = [] {
		const auto* value = std::getenv("LUMEN_PS5_GFX_BUFFER_PUBLICATION_BATCH");
		return value == nullptr || std::strcmp(value, "1") == 0;
	}();
    // No ordinary/foreign priority callback may change a native producer
    // between the downloaded witness and its install. Downloads below queue
    // only their own copyout callbacks; stream wraps never pop ordinary work.
    const bool witness_admitted = BufferPublicationWitnessEnabled() && batch_enabled &&
        GuestBufferSnapshotReuseEnabled() && !GuestAsyncPrivateBackingEnabled() &&
        !GuestAsyncCpuConcurrentExperimentRequested() && !m_scheduler.HasPendingOperations();
	if (batch_enabled) {
		struct Publication {
			BufferId id;
			uint64_t address;
			uint64_t size;
			vk::Buffer native;
            std::shared_ptr<DownloadWitnesses> witnesses;
		};
		std::vector<Publication> publications;
		// Downloads consume byte ranges when queued. Retain their ownership proof
		// until all callbacks complete, while the GPU tracker remains dirty.
		const auto dirty_ranges = m_gpu_modified_ranges;
			for (const auto& [address, id]: m_buffers) {
				auto& buffer = m_slot_buffers[id];
				if (!dirty_ranges.Intersects(buffer.CpuAddress(), buffer.Size())) continue;
                if (selected_only && !HasGuestWriteDependency(buffer, dependencies)) continue;
            if (retain_private && !LibKernel::Memory::QueryGuestBackingCpuVisible(
                    buffer.CpuAddress(), buffer.Size())) continue;
			EXIT_IF(buffer.is_deleted || address != buffer.CpuAddress());
			m_memory_tracker.ValidateGpuDirtyOwnership(dirty_ranges, address, buffer.Size(),
			                                           "batched buffer publication prepare");
            auto witnesses = witness_admitted ? std::make_shared<DownloadWitnesses>() : nullptr;
			publications.push_back({id, address, buffer.Size(), buffer.Handle(), witnesses});
			if (!DownloadBufferMemory<true>(buffer, address, buffer.Size(), std::move(witnesses)))
				EXIT("PS5 GPU buffer publication has dirty bytes without a download\n");
		}
			if (!retain_private && !selected_only && !m_gpu_modified_ranges.Empty())
			EXIT("PS5 GPU buffer publication left unowned dirty bytes\n");
		if (publications.empty()) return;
		const auto tick = m_scheduler.CurrentTick();
		m_scheduler.Finish();
		m_scheduler.WaitPriorityOperations(tick);
		// Deferred resource retirement can run during Finish. Revalidate the whole
		// retained set before releasing any owner's GPU-dirty state.
		for (const auto& publication: publications) {
			const auto* buffer = m_slot_buffers.try_get(publication.id);
			const auto found = m_buffers.find(publication.address);
			if (buffer == nullptr || buffer->is_deleted ||
			    buffer->CpuAddress() != publication.address || buffer->Size() != publication.size ||
			    buffer->Handle() != publication.native || found == m_buffers.end() ||
			    found->second != publication.id)
				EXIT("PS5 GPU buffer owner changed during backing publication\n");
			m_memory_tracker.ValidateGpuDirtyOwnership(dirty_ranges, publication.address,
			                                           publication.size,
			                                           "batched buffer publication completion");
		}
		for (const auto& publication: publications) {
			m_memory_tracker.UnmarkRegionAsGpuModified(publication.address, publication.size);
			m_backing_snapshots.Forget(publication.address, publication.size);
		}
        // Ownership is over written guest bytes. The native cache reservation
        // can additionally contain clean unmapped holes after VM retirement.
        for (const auto& publication: publications)
            dirty_ranges.ForEachInRange(publication.address, publication.size,
                [&](uint64_t begin, uint64_t end) {
                    m_scheduler.Context().InvalidatePhysicalAliases(begin, end - begin, false);
                });
        for (const auto& publication: publications)
            if (publication.witnesses) InstallDownloadWitnesses(publication.id, publication.native, *publication.witnesses);
		return;
	}
	// Cache indexes cannot change while the sole GPU worker owns the epoch.
	// Each download retains exact dirty bytes; wait before retiring page owners.
	const auto dirty_ranges = m_gpu_modified_ranges;
	for (const auto& [address, id]: m_buffers) {
		(void)address;
		auto& buffer = m_slot_buffers[id];
		if (!m_gpu_modified_ranges.Intersects(buffer.CpuAddress(), buffer.Size())) continue;
        if (selected_only && !HasGuestWriteDependency(buffer, dependencies)) continue;
        if (retain_private && !LibKernel::Memory::QueryGuestBackingCpuVisible(
                buffer.CpuAddress(), buffer.Size())) continue;
		if (!DownloadBufferMemory<true>(buffer, buffer.CpuAddress(), buffer.Size()))
			EXIT("PS5 GPU buffer publication has dirty bytes without a download\n");
		const auto tick = m_scheduler.CurrentTick();
		m_scheduler.Finish();
		m_scheduler.WaitPriorityOperations(tick);
		m_memory_tracker.UnmarkRegionAsGpuModified(buffer.CpuAddress(), buffer.Size());
		m_backing_snapshots.Forget(buffer.CpuAddress(), buffer.Size());
        dirty_ranges.ForEachInRange(buffer.CpuAddress(), buffer.Size(),
            [&](uint64_t begin, uint64_t end) {
                m_scheduler.Context().InvalidatePhysicalAliases(begin, end - begin, false);
            });
	}
	if (!retain_private && !selected_only && !m_gpu_modified_ranges.Empty())
		EXIT("PS5 GPU buffer publication left unowned dirty bytes\n");
}

} // namespace Libs::Graphics
