#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionManager.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/image/blitHelper.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/image/tiler.h"

#include <map>
#include <memory>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class Buffer;
class BufferCache;
class CommandBuffer;
class CommandScheduler;
class RenderExecutor;
struct TextureCacheTestAccess;
// Global, process-latched candidate; a live exclusive epoch is checked separately.
[[nodiscard]] bool ImagePublicationSnapshotEnabled() noexcept;
[[nodiscard]] bool ImagePublicationBlockSeedEnabled() noexcept;
[[nodiscard]] bool ImageUploadSnapshotEnabled() noexcept;

class TextureCache {
public:
	enum class BindingType : uint8_t { Texture, Storage, RenderTarget, DepthTarget, VideoOut };

	struct ImageDesc {
		ImageInfo     info;
		ImageViewInfo view_info;
		BindingType   type = BindingType::Texture;
	};

	TextureCache(GraphicContext& graphics, CommandScheduler& scheduler, PageManager& page_manager,
	             BufferCache& buffer_cache);
	~TextureCache();
	KYTY_CLASS_NO_COPY(TextureCache);

	[[nodiscard]] ImageId       FindImage(ImageDesc& desc, bool exact_format = false);
	void                        UpdateImage(ImageId id);
	[[nodiscard]] ImageId       FindImageFromRange(uint64_t address, uint64_t size,
	                                               bool ensure_valid = true);
	[[nodiscard]] vk::ImageView FindTexture(ImageId id, const ImageDesc& desc);
	[[nodiscard]] vk::ImageView FindRenderTarget(ImageId id, const ImageDesc& desc);
	[[nodiscard]] vk::ImageView FindDepthTarget(ImageId id, const ImageDesc& desc);
	[[nodiscard]] Image&        GetImage(ImageId id) {
		auto& image = m_slot_images[id];
		TouchImage(image);
		return image;
	}
	void MarkGpuWritten(ImageId id);

	[[nodiscard]] bool ClearImageFromBuffer(CommandBuffer& command, uint64_t address, uint64_t size,
	                                        uint32_t packed_clear);
	void               InvalidateMemory(uint64_t address, uint64_t size);
	void               InvalidateMemoryFromGPU(uint64_t address, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t address, uint64_t size);
	[[nodiscard]] bool IsRegionRegistered(uint64_t address, uint64_t size);

	[[nodiscard]] bool IsMeta(uint64_t address);
	[[nodiscard]] bool IsMetaCleared(uint64_t address, uint32_t slice);
	[[nodiscard]] bool ClearMeta(uint64_t address);
	[[nodiscard]] bool TouchMeta(uint64_t address, uint32_t slice, bool is_clear);

	void UnmapMemory(uint64_t address, uint64_t size);
	void ProcessDownloadImages();
	void PublishGuestWrites(bool retain_private = false);
    // A dependency on either plane selects the complete depth/stencil family.
    // Query and publication are serialized by the GPU worker, with no scheduler
    // wait while this collection holds the texture lock.
    void CollectGuestWriteDependencies(std::span<const GuestRange> dependencies,
                                       std::vector<GuestRange>& owners);
    [[nodiscard]] bool HasGuestWriteDependency(uint64_t address, uint64_t size);
    void PublishGuestWritesInRanges(std::span<const GuestRange> dependencies);
    [[nodiscard]] bool HasCpuVisibleGpuWrites();
	void RunGarbageCollector();

private:
	enum class TransferDirection { Upload, Download };
	struct TextureTransfer;
	struct ImageDownload;

	struct MetaDataInfo {
		enum class Type : uint8_t { CMask, FMask, HTile };

		Type     type;
		uint32_t clear_mask = UINT32_MAX;
	};

	struct OverlapResult {
		ImageId image;
		int32_t mip   = -1;
		int32_t layer = -1;
	};

	using ImageIds       = InlinePageOwnerList<ImageId, 16>;
	using ImagePageTable = MultiLevelPageTable<ImageIds, 20, 44, 14>;

	// Callers have validated the nonempty 44-bit range with TryGetPageRange.
	template <typename Func>
	static void ForEachPage(uint64_t address, size_t size, Func&& func) {
		using FuncReturn = typename std::invoke_result<Func, uint64_t>::type;
		static constexpr bool RETURNS_BOOL = std::is_same_v<FuncReturn, bool>;
		const uint64_t page_end = (address + size - 1) >> ImagePageTable::kPageBits;
		for (uint64_t page = address >> ImagePageTable::kPageBits; page <= page_end; ++page) {
			if constexpr (RETURNS_BOOL) {
				if (func(page)) {
					break;
				}
			} else {
				func(page);
			}
		}
	}

	[[nodiscard]] ImageId     InsertImage(const ImageInfo& info);
	[[nodiscard]] ImageId     GetNullImage(const ImageDesc& desc);
	void                      RegisterImage(ImageId id);
	void                      UnregisterImage(ImageId id);
	void                      DeleteImage(ImageId id);
	void                      FreeImage(ImageId id);
	void                      TouchImage(Image& image);
	void                      TrackImage(ImageId id);
	void                      TrackImageHead(ImageId id);
	void                      TrackImageTail(ImageId id);
	void                      UntrackImage(ImageId id);
	void                      UntrackImageHead(ImageId id);
	void                      UntrackImageTail(ImageId id);
	void                      MarkAsMaybeDirty(ImageId id, Image& image);
	void                      TrackImageDownload(ImageId id, Image& image);
	[[nodiscard]] static bool SameBacking(const ImageInfo& cached, const ImageInfo& requested,
	                                      bool exact_format);
	[[nodiscard]] static BindingType UploadBinding(const Image& image);

	// Caller holds m_lock; it also serializes the per-image query epoch.
	[[nodiscard]] ImageIds      FindImagesInRegion(uint64_t address, uint64_t size,
	                                               bool page_overlap) const;
	[[nodiscard]] OverlapResult ResolveOverlap(const ImageInfo& requested, BindingType binding,
	                                           ImageId cached, ImageId merged);
	[[nodiscard]] ImageId       ResolveDepthOverlap(const ImageInfo& requested, BindingType binding,
	                                                ImageId cached);
	[[nodiscard]] ImageId       ExpandImage(const ImageInfo& info, ImageId source);
	void                        RefreshImage(ImageId id);
	void                        MaterializeColorClear(ImageId id, const ImageDesc& desc,
	                                                uint32_t metadata_base_layer);
	void                        InitializeImage(ImageId id);
	[[nodiscard]] TextureTransfer
	BuildTextureTransfer(const Image& image, BindingType binding, TransferDirection direction) const;
	[[nodiscard]] ImageDownload BuildDownload(const Image& image) const;
	void TraceImagePublication(ImageId id, const ImageDownload& transfer) const;
	[[nodiscard]] bool BuildDepthPlane(const Image& image, ImageInfo& plane) const;
	[[nodiscard]] bool DepthTiledColorReadbackSupported(const Image& image) const;
	void UploadImage(Image& image, Buffer& source, uint64_t source_offset);
	void DownloadImage(Image& image, Buffer& destination, uint64_t destination_offset,
	                       uint64_t destination_size, ImageDownload transfer);
	void DownloadDepth(Image& image, Buffer& destination, uint64_t destination_offset);
	void CommitGpuWrite(Image& image);
	void ValidateGpuWrite(const Image& image) const;
	// Caller holds m_lock. Volume layer ranges select depth slices.
	void ClearImage(CommandBuffer& command, ImageId id, vk::Format format,
	                const vk::ImageSubresourceRange& range, const vk::ClearValue& clear);
	void PrepareImageCopy(Image& image);
	void RefreshCopySource(ImageId id);
	[[nodiscard]] bool CopyD16(Image& destination, Image& source);
	void               CopyImage(ImageId destination, ImageId source);
	[[nodiscard]] ImageId AssociateStencil(ImageId depth, GuestRange stencil);
	void CopyImageMip(ImageId destination, ImageId source, uint32_t mip, uint32_t layer);
	void ValidateImageDesc(const ImageDesc& desc) const;

	void               InvalidateCpuAliases(uint64_t address, uint64_t size);
	// Caller holds m_lock; used by internal image uploads to avoid recursion.
	[[nodiscard]] bool IsRegionGpuModifiedLocked(uint64_t address, uint64_t size) const;
    [[nodiscard]] bool HasGuestWriteDependency(const Image& image,
                                               std::span<const GuestRange> dependencies) const;
	// A nonempty snapshot span remains owned by PublishImageWrites until its
	// scheduler and every priority callback complete. No staging pointer is retained.
	[[nodiscard]] bool DownloadImageMemory(ImageId id,
	    std::span<uint8_t> publication = {});
	[[nodiscard]] bool HasDisjointHostImportBacking(ImageId id);
	[[nodiscard]] size_t TryDownloadImageHostBatch(
	    std::span<const std::pair<ImageId, GuestRange>> candidates);
	uint64_t m_host_import_bytes = 0;
	uint64_t m_host_import_batches = 0, m_host_import_batch_owners = 0, m_host_import_batch_cap_stops = 0;
	[[nodiscard]] static bool DownloadFullyOverwrites(const Image& image, const ImageDownload& transfer);
	[[nodiscard]] bool DownloadFullyOverwrites(const Image& image) const;
	[[nodiscard]] static bool BuildPublicationOverwriteBlocks(GuestRange owner,
	    std::span<const GpuTileInfo> tiles, std::vector<GuestRange>& overwritten);
	[[nodiscard]] bool BuildPublicationOverwriteBlocks(const Image& image,
	    const ImageDownload& transfer, std::vector<GuestRange>& overwritten) const;
	[[nodiscard]] static bool CoalescePublicationRanges(std::vector<GuestRange>& ranges);
	// Caller holds m_lock. Complete every dirty plane before clearing ownership.
	void PublishImageWrites(std::span<const ImageId> images);
    [[nodiscard]] bool IsGpuOwnerCpuVisible(const Image& image) const;
	struct PublicationBuffer {
		std::unique_ptr<uint8_t[]> storage;
		uint64_t length = 0;
		[[nodiscard]] uint8_t* data() const noexcept { return storage.get(); }
		[[nodiscard]] uint64_t size() const noexcept { return length; }
	};
	struct PublicationSnapshot {
		ImageInfo info;
		vk::Image native;
		BindingType binding;
		uint64_t revision = 0;
		PublicationBuffer bytes;
		uint64_t last_used = 0;
		bool from_upload = false;
	};
	static constexpr uint64_t PublicationSnapshotBudget = 64ull * 1024 * 1024;
	static constexpr uint64_t PublicationSnapshotOwnerLimit = 16ull * 1024 * 1024;
	[[nodiscard]] static bool PublicationSnapshotFits(uint64_t used, uint64_t size) noexcept;
	[[nodiscard]] PublicationBuffer ReservePublicationSnapshot(uint64_t size);
	void RecyclePublicationBuffer(PublicationBuffer bytes);
	[[nodiscard]] bool CanSnapshotPublication(const Image& image) const;
	[[nodiscard]] bool CanSnapshotImage(const Image& image) const;
	[[nodiscard]] bool CanSnapshotUpload(const Image& image) const;
	[[nodiscard]] bool ReusePublicationSnapshot(ImageId id);
	void ForgetPublicationSnapshot(ImageId id);
	void PrunePublicationSnapshots();
	std::unordered_map<ImageId, PublicationSnapshot> m_publication_snapshots;
	std::vector<PublicationBuffer> m_publication_pool;
	uint64_t m_publication_snapshot_bytes = 0;
	uint64_t m_publication_allocated_bytes = 0;
	struct PublicationStats {
		uint64_t batches = 0, hits = 0, misses = 0, compared_bytes = 0, copied_bytes = 0;
		uint64_t allocations = 0, reused_buffers = 0, reused_bytes = 0, evictions = 0;
		uint64_t budget_misses = 0, allocation_failures = 0;
		uint64_t prefill_saved = 0, prefill_preserved = 0, aliases_before = 0, aliases_after = 0;
		uint64_t block_seed_saved = 0, block_seed_preserved = 0, block_seed_plans = 0;
		uint64_t upload_captures = 0, upload_hits = 0, upload_avoided_bytes = 0;
	} m_publication_stats;
	void LogPublicationStats();

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	TrackingSpinLock                                  m_lock;
	PageManager&                                      m_page_manager;
	BlitHelper                                        m_blit_helper;
	TileManager                                       m_tiler;
	BufferCache&                                      m_buffer_cache;
	Common::SlotVector<Image>                         m_slot_images;
	ImagePageTable                                    m_image_page_table;
	std::unordered_map<vk::Format, ImageId>           m_null_images;
	Common::LeastRecentlyUsedCache<ImageId, uint64_t> m_lru_cache;
	std::unordered_set<ImageId>                       m_download_images;
	std::map<uint64_t, MetaDataInfo>                  m_surface_metas;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t                                          m_trigger_gc_memory  = 0;
	uint64_t                                          m_pressure_gc_memory = 1536ull * 1024 * 1024;
	uint64_t         m_critical_gc_memory     = 3ull * 1024 * 1024 * 1024;
	uint64_t         m_gc_tick                = 0;
	mutable uint32_t m_image_query_epoch      = 0;
	bool             m_readback_linear_images = false;

	friend struct TextureCacheTestAccess;
	friend class BufferCache;
	friend class RenderExecutor;
	// Bounded diagnostic only; guest tests/clear writes differ from native STORE.
	void TraceDepthAttachmentBinding(ImageId id, const ImageViewInfo& view,
	                                 vk::ImageAspectFlags guest_write_aspects);
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
