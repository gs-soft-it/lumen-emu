#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/slotVector.h"
#include "common/uniformMetadata.h"
#include "kernel/memory.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/guestBackingPolicy.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/faultManager.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <map>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;
class TextureCache;

// Process-latched selector; absent/1 ON, 0 OFF. Does not enable concurrency.
bool BufferContiguousHashEnabled() noexcept;
// Publication witness remains opt-in; absent/0 OFF, 1 ON.
bool BufferPublicationWitnessEnabled() noexcept;

using BufferId = Common::SlotId;
inline constexpr BufferId NULL_BUFFER_ID {0};

class BufferCache {
public:
	static constexpr uint32_t CACHING_PAGEBITS  = 14;
	static constexpr uint64_t CACHING_PAGESIZE  = uint64_t {1} << CACHING_PAGEBITS;
	static constexpr uint64_t CACHING_NUMPAGES  = (LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemorySize) >> CACHING_PAGEBITS;
	static constexpr uint64_t BDA_PAGETABLE_SIZE =
	    CACHING_NUMPAGES * sizeof(vk::DeviceAddress);

	static constexpr uint64_t PageIndex(uint64_t address) {
		return (address < LOWER_ADDRESS_SIZE
		            ? address
		            : address - LibKernel::Memory::kExtendedMemoryBase + LOWER_ADDRESS_SIZE) >>
		       CACHING_PAGEBITS;
	}
	static constexpr uint64_t GuestAddress(uint64_t offset) {
		return offset < LOWER_ADDRESS_SIZE
		           ? offset
		           : offset - LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemoryBase;
	}

	BufferCache(GraphicContext& graphics, CommandScheduler& scheduler, PageManager& page_manager,
	            TextureCache& texture_cache);
	~BufferCache();
	KYTY_CLASS_NO_COPY(BufferCache);

	void                   InvalidateMemory(uint64_t vaddr, uint64_t size);
	// Called only from the serialized GPU unmap lane (or ordered teardown).
	// Cache reservations may survive unmap, but backing witnesses may not.
	void                   RetireBackingSnapshots(uint64_t vaddr, uint64_t size);
	void                   ReadMemory(uint64_t vaddr, uint64_t size, bool is_write = false);
	[[nodiscard]] Buffer&  GetBuffer(BufferId id) { return m_slot_buffers[id]; }
	[[nodiscard]] BufferId FindBuffer(uint64_t vaddr, uint64_t size);
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBuffer(uint64_t vaddr, uint64_t size,
	                                                        bool     is_written,
	                                                        bool     is_texel_buffer = false,
	                                                        BufferId id              = {});
	[[nodiscard]] StreamBuffer&                GetUtilityBuffer(MemoryUsage usage) noexcept {
		switch (usage) {
			case MemoryUsage::Upload: return m_staging_buffer;
			case MemoryUsage::Stream: return m_stream_buffer;
			case MemoryUsage::Download: return m_download_buffer;
			case MemoryUsage::DeviceLocal: return m_device_buffer;
		}
		EXIT("BufferCache: invalid utility-buffer usage\n");
	}
	[[nodiscard]] const Buffer* GetGdsBuffer() const noexcept { return &m_gds_buffer; }
	[[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept { return &m_bda_pagetable_buffer; }
	[[nodiscard]] Buffer* GetFaultBuffer() noexcept { return m_fault_manager.GetFaultBuffer(); }
	// A capture is produced only for a fresh CPU staging copy. Cached/native
	// buffer sources must never acquire a witness from current guest bytes.
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBufferForImage(uint64_t vaddr, uint64_t size,
	    std::span<uint8_t> capture = {}, bool* captured = nullptr);
	void FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds);
	// Proven uniform GPU writes; ownership and guest backing stay authoritative.
	[[nodiscard]] bool TryFillUniformMetadata(uint64_t vaddr, uint64_t size, uint32_t value);
	[[nodiscard]] bool TryGetUniformMetadata(uint64_t vaddr, uint64_t size,
	                                         uint64_t generation, uint32_t* value);
	// Only for an admitted fill or a completely preflighted metadata conversion.
	void FillUniformMetadataGpu(uint64_t vaddr, uint64_t size, uint32_t value,
	                            uint64_t generation);
	void InvalidateUniformMetadata(uint64_t vaddr, uint64_t size);
	void ClearUniformMetadata();
	void CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
	                bool src_gds);
	// Cache-index and exact dirty-range queries require GPU-thread serialization.
	[[nodiscard]] bool IsRegionRegistered(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	void               ProcessFaultBuffer();
	void               SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size);
	void               RunGarbageCollector();
	void               PublishGuestWrites(bool retain_private = false);
    // The caller holds GPU publication authority and has expanded every
    // selected owner through its physical aliases. Buffer snapshot ownership
    // is page based; touching padding on a dirty page selects that owner too.
    void CollectGuestWriteDependencies(std::span<const GuestRange> dependencies,
                                       std::vector<GuestRange>& owners);
    void PublishGuestWritesInRanges(std::span<const GuestRange> dependencies);
    [[nodiscard]] bool HasCpuVisibleGpuWrites();

private:
	friend struct BufferCacheTestAccess;

	bool IsBufferInvalid(BufferId id) const {
		const auto* buffer = m_slot_buffers.try_get(id);
		return buffer == nullptr || buffer->is_deleted;
	}

	using BufferMap = std::map<uint64_t, BufferId>;
	struct OverlapResult {
		BufferMap::iterator first;
		BufferMap::iterator last;
		uint64_t            begin;
		uint64_t            end;
		bool                has_stream_leap;
	};

	using PageTable = MultiLevelPageTable<BufferId, CACHING_PAGEBITS, 44, 20>;
	static_assert(CACHING_PAGESIZE == (uint64_t {1} << PageTable::kPageBits));
	void WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source, uint64_t size);
	void TouchBuffer(const Buffer& buffer);
	[[nodiscard]] OverlapResult ResolveOverlaps(uint64_t vaddr, uint64_t size);
	void JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score);
	[[nodiscard]] BufferId CreateBuffer(uint64_t vaddr, uint64_t size);
	void                   Register(BufferId id);
	enum class SnapshotDisposition { Forget, Transfer };
	void Unregister(BufferId id, SnapshotDisposition snapshots = SnapshotDisposition::Forget);
	template <bool insert>
	void ChangeRegister(BufferId id);
	void DeleteBuffer(BufferId id, SnapshotDisposition snapshots = SnapshotDisposition::Forget);
	// Image uploads run under the texture lock, after proving that no native
	// image owns unpublished bytes. Their source is current backing or a GPU
	// buffer; they must not recursively query the texture cache.
	enum class SynchronizeSource { BufferConsumer, ImageUpload };
	[[nodiscard]] bool SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                     bool is_written, bool is_texel_buffer,
	                                     SynchronizeSource source = SynchronizeSource::BufferConsumer);
    void RevalidateBacking(Buffer& buffer, uint64_t vaddr, uint64_t size,
                           RangeSet* publication_reuse = nullptr);
    struct DownloadWitnessSpan {
        uint64_t address = 0, size = 0;
        std::vector<LibKernel::Memory::GuestBackingRange> backing;
        std::vector<uint64_t> hashes;
    };
    using DownloadWitnesses = std::vector<DownloadWitnessSpan>;
    void InstallDownloadWitnesses(BufferId id, vk::Buffer native, const DownloadWitnesses& witnesses);
    void ReconcileCpuChangedOwnedPage(Buffer& buffer, uint64_t address, const uint8_t* current);
    void PublishUploadSnapshot(uint64_t address, const uint8_t* copied, uint64_t size);
    [[nodiscard]] bool HasGuestWriteDependency(const Buffer& buffer,
                                               std::span<const GuestRange> dependencies);
    void PublishGuestWrites(bool retain_private, std::span<const GuestRange> dependencies,
                            bool selected_only);
	[[nodiscard]] vk::Buffer UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
	                                      uint64_t total_size);
	[[nodiscard]] bool SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size);
	// Synchronous downloads publish before returning; asynchronous callers wait before reuse.
	template <bool async>
	[[nodiscard]] bool DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size,
        std::shared_ptr<DownloadWitnesses> witnesses = {});

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	FaultManager                                      m_fault_manager;
	Buffer                                            m_gds_buffer;
	Buffer                                            m_bda_pagetable_buffer;
	Common::SlotVector<Buffer>                        m_slot_buffers;
	Common::LeastRecentlyUsedCache<BufferId, uint64_t> m_lru_cache;
	BufferMap                                         m_buffers;
	PageTable                                         m_page_table;
	RangeSet                                          m_gpu_modified_ranges;
	MemoryTracker                                     m_memory_tracker;
	Common::UniformMetadata::KnownRanges               m_uniform_metadata;
    GuestBackingSnapshots                             m_backing_snapshots;
    uint64_t m_contiguous_hash_pages = 0, m_contiguous_hash_fallbacks = 0;
    uint64_t m_publication_witness_pages = 0, m_publication_witness_cpu_dirty_pages = 0;
    uint64_t m_publication_witness_upload_avoided_bytes = 0, m_guest_upload_bytes = 0;
    uint64_t m_publication_witness_receipts = 0, m_publication_reuse_receipts = 0;
	StreamBuffer                                      m_staging_buffer;
	StreamBuffer                                      m_stream_buffer;
	StreamBuffer                                      m_download_buffer;
	StreamBuffer                                      m_device_buffer;
	TextureCache&                                     m_texture_cache;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t m_trigger_gc_memory  = 1ull * 1024 * 1024 * 1024;
	uint64_t m_critical_gc_memory = 2ull * 1024 * 1024 * 1024;
	uint64_t m_gc_tick            = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
