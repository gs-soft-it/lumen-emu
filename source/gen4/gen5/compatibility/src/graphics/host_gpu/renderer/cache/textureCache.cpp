#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "FexPerformanceTrace.h"
#include "graphics/host_gpu/renderer/gpuSliceTrace.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/renderTiming.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "../../../../../../engine/src/common/memoryBudget.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/image/tiler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cinttypes>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <span>
#include <tuple>
#include <vulkan/vulkan_format_traits.hpp>

namespace Libs::Graphics {

bool ImagePublicationSnapshotEnabled() noexcept {
	static const bool enabled = [] {
		const auto* value = std::getenv("LUMEN_PS5_IMAGE_PUBLICATION_SNAPSHOT");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

bool ImagePublicationBlockSeedEnabled() noexcept {
	static const bool enabled = [] {
		const auto* value = std::getenv("LUMEN_PS5_IMAGE_PUBLICATION_BLOCK_SEED");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

bool ImageUploadSnapshotEnabled() noexcept {
	static const bool enabled = [] {
		const auto* value = std::getenv("LUMEN_PS5_IMAGE_UPLOAD_SNAPSHOT");
		if (value == nullptr || std::strcmp(value, "1") == 0) return true;
		if (std::strcmp(value, "0") == 0) return false;
		EXIT("invalid LUMEN_PS5_IMAGE_UPLOAD_SNAPSHOT: expected 0 or 1\n");
		std::abort();
	}();
	return enabled;
}

namespace {

constexpr uint64_t NumFramesBeforeRemoval = 32;

uint64_t ImageOwnerTraceReservation(bool publication) {
	auto* trace = GpuSliceTrace::current;
	if (trace == nullptr) return 0;
	static const bool enabled = [] {
		const auto* value = std::getenv("LUMEN_PS5_IMAGE_OWNER_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	if (!enabled) return 0;
	static const bool announced = [] {
		std::fprintf(stderr, "PS5_GPU_IMAGE_OWNER_TRACE_V1 enabled=1 publication_limit=128 binding_limit=256 region_limit_per_owner=16 window=gpu_slice_trace coverage=bounded_partial binding_coverage=rt_storage_acquisition_depth_render_state actual_written_ranges=unknown\n");
		return true;
	}();
	(void)announced;
	static std::atomic<uint64_t> owners {0}, bindings {0};
	const auto sample = (publication ? owners : bindings).fetch_add(1, std::memory_order_relaxed) + 1;
	const auto limit = publication ? 128u : 256u;
	if (sample > limit) {
		++(publication ? trace->image_owner_trace_drops : trace->image_binding_trace_drops);
		if (sample == limit + 1)
			std::fprintf(stderr, "PS5_GPU_IMAGE_OWNER_TRACE_V1 event=limit_reached kind=%s limit=%u subsequent=unrecorded overflow_counter=per_slice\n",
			    publication ? "publication" : "binding", limit);
		return 0;
	}
	++(publication ? trace->image_owner_trace_records : trace->image_binding_trace_records);
	return sample;
}

void TraceImageWriteCapableBinding(ImageId id, const Image& image,
                                   const ImageViewInfo& view, const char* kind,
                                   int64_t guest_write_aspects = -1) {
	const auto sample = ImageOwnerTraceReservation(false);
	if (sample == 0) return;
	const auto* trace = GpuSliceTrace::current;
	std::fprintf(stderr, "PS5_GPU_IMAGE_BINDING_V1 sample=%llu slice_sequence=%llu submit_id=%llu epoch_token=%llu owner_index=%u owner_generation=%u publication_revision=%llu kind=%s guest_address=%llu owner_bytes=%llu base_mip=%u mip_count=%u base_layer=%u layer_count=%u aspect=%u view_format=%u guest_write_aspects=%lld native_declared_write_aspects=%u scope=write_capable_binding actual_written=unknown\n",
	    static_cast<unsigned long long>(sample), static_cast<unsigned long long>(trace->Sequence()),
	    static_cast<unsigned long long>(trace->submit_id), static_cast<unsigned long long>(trace->Epoch()),
	    id.index, id.generation, static_cast<unsigned long long>(image.PublicationRevision()), kind,
	    static_cast<unsigned long long>(image.info.data.address), static_cast<unsigned long long>(image.info.data.size),
	    view.base_level, view.level_count, view.base_layer, view.layer_count,
	    static_cast<uint32_t>(view.aspect), static_cast<uint32_t>(view.format),
	    static_cast<long long>(guest_write_aspects), static_cast<uint32_t>(view.aspect));
}

// Per-slice denial_mask bits are an observed set, not counts of attempts:
// 1 policy, 2 image, 4 alias query, 8 dirty buffer, 16 dirty image,
// 32 pending callback, 64 VM borrow, 128 copy alignment, 256 Vulkan import.
bool HostImportDenied(uint64_t bit, const char* reason, bool attempted = false) {
	if (auto* trace = GpuSliceTrace::current; trace != nullptr) {
		trace->image_host_import_denial_mask |= bit;
		if (attempted) ++trace->image_host_import_fallbacks;
	}
	if (attempted) {
		static std::atomic<uint32_t> receipts {0};
		if (receipts.fetch_add(1, std::memory_order_relaxed) < 64)
			std::fprintf(stderr, "PS5_GPU_HOST_IMPORT_V1 outcome=staged reason=%s denial_bit=%llu\n",
			    reason, static_cast<unsigned long long>(bit));
	}
	return false;
}

bool PublicationTraceEnabled() noexcept {
	static const bool enabled = [] {
		const auto* value = std::getenv("LUMEN_PS5_IMAGE_PUBLICATION_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

bool SamePublicationLayout(const ImageInfo& a, const ImageInfo& b) {
	return a.data == b.data && a.stencil == b.stencil && a.extent == b.extent &&
	    a.resources == b.resources && a.pitch == b.pitch && a.bytes_per_block == b.bytes_per_block &&
	    a.samples == b.samples && a.type == b.type && a.tile_mode == b.tile_mode &&
	    a.pixel_format == b.pixel_format && a.guest_format == b.guest_format &&
	    a.bgra16 == b.bgra16 && a.mip_layout == b.mip_layout &&
	    a.metadata.kind == b.metadata.kind && a.metadata.range == b.metadata.range &&
	    a.metadata.compression == b.metadata.compression &&
	    a.metadata.stencil_compressed == b.metadata.stencil_compressed;
}

[[nodiscard]] bool DecodeColorClear(const TextureCache::ImageDesc& desc, uint8_t code,
                                  vk::ClearColorValue& clear) {
	const auto& metadata = desc.info.metadata;
	const auto  format   = desc.view_info.format;
	const bool  cmask    = metadata.kind == ImageMetadataKind::Cmask;
	if (cmask ? code == 0 : code == 0x20) {
		// Register clears belong to the color buffer; the texture pipe cannot decode them.
		return desc.type == TextureCache::BindingType::RenderTarget &&
		       metadata.clear_register_valid &&
		       DecodePackedColorClear(format, metadata.clear_word, clear);
	}
	if (cmask) {
		return false;
	}
	switch (code) {
		case 0x00:
		case 0x40:
		case 0x80:
		case 0xc0: break;
		default: return false;
	}
	clear = {};
	if (code == 0x00) {
		return true;
	}
	switch (format) {
		case vk::Format::eR8Unorm:
		case vk::Format::eR8G8Unorm:
		case vk::Format::eR8G8B8A8Unorm:
		case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2B10G10R10UnormPack32:
		case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eR5G6B5UnormPack16:
		case vk::Format::eA1R5G5B5UnormPack16:
		case vk::Format::eR4G4B4A4UnormPack16:
		case vk::Format::eR16Unorm:
		case vk::Format::eR16G16Unorm:
		case vk::Format::eR16G16B16A16Unorm:
		case vk::Format::eR16Sfloat:
		case vk::Format::eR16G16Sfloat:
		case vk::Format::eR16G16B16A16Sfloat:
		case vk::Format::eR32Sfloat:
		case vk::Format::eR32G32Sfloat:
		case vk::Format::eR32G32B32A32Sfloat:
		case vk::Format::eB10G11R11UfloatPack32: break;
		default: return false;
	}
	const float          rgb   = (code & 0x80u) != 0 ? 1.0f : 0.0f;
	const float          alpha = (code & 0x40u) != 0 ? 1.0f : 0.0f;
	std::array<float, 4> channels {rgb, rgb, rgb, alpha};
	if (!metadata.dcc_alpha_msb) {
		std::swap(channels[0], channels[3]);
	}
	// DCC clear decoding clamps missing lanes before applying the format swizzle.
	const auto components = vk::componentCount(format);
	if (components == 1) {
		channels[0] = channels[3];
	} else if (components == 2) {
		channels[1] = channels[3];
	}
	switch (format) {
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eA1R5G5B5UnormPack16:
		case vk::Format::eR5G6B5UnormPack16: std::swap(channels[0], channels[2]); break;
		case vk::Format::eR4G4B4A4UnormPack16:
			std::reverse(channels.begin(), channels.end());
			break;
		default: break;
	}
	clear.float32 = channels;
	return true;
}

[[nodiscard]] std::vector<vk::BufferImageCopy> BuildDepthCopies(const ImageInfo& info,
                                                              uint64_t slice_stride,
                                                              vk::ImageAspectFlags aspect) {
	std::vector<vk::BufferImageCopy> copies(info.resources.layers);
	for (uint32_t layer = 0; layer < info.resources.layers; ++layer) {
		auto& copy             = copies[layer];
		copy.bufferOffset      = slice_stride * layer;
		copy.bufferRowLength   = info.pitch;
		copy.bufferImageHeight = info.extent.height;
		copy.imageSubresource  = {aspect, 0, layer, 1};
		copy.imageExtent       = {info.extent.width, info.extent.height, 1};
	}
	return copies;
}

[[nodiscard]] std::vector<GpuTileInfo> BuildPublicationTiles(const ImageInfo& info, uint32_t levels);

[[nodiscard]] std::vector<GpuTileInfo> BuildDepthTiles(const ImageInfo& info) {
	return BuildPublicationTiles(info, 0);
}

[[nodiscard]] std::vector<GpuTileInfo> BuildPublicationTiles(const ImageInfo& info,
                                                              uint32_t levels = 0) {
	std::vector<GpuTileInfo> tiles;
	if (!info.IsTiled()) return tiles;
	levels = levels == 0 ? info.resources.levels : levels;
	// Volume and tail footprints conservatively retain their seed; a future proof
	// can extend the existing 2D full-block publication optimization.
	if (info.IsVolume() || levels > info.mip_layout.size()) return tiles;
	for (uint32_t level = 0; level < levels; ++level) {
		const auto& mip = info.mip_layout[level];
		const auto extent = info.MipExtent(level);
		const auto stride = info.linear_slice_stride != 0 ? info.linear_slice_stride : mip.linear_size;
		for (uint32_t layer = 0; layer < extent.depth; ++layer) {
			GpuTileInfo tile {};
			tile.family = info.tiling.block.family;
			tile.bytes_per_element = info.bytes_per_block;
			tile.linear_offset = mip.linear_offset + uint64_t {layer} * stride;
			tile.linear_size = mip.linear_size;
			tile.tiled_offset = mip.offset + uint64_t {layer} * info.tiled_slice_stride;
			tile.tiled_size = mip.size / extent.depth;
			tile.width = (extent.width + info.tiling.texel_width - 1) / info.tiling.texel_width;
			tile.height = (extent.height + info.tiling.texel_height - 1) / info.tiling.texel_height;
			tile.pitch = mip.linear_pitch;
			tile.tail_x = mip.tail_x;
			tile.tail_y = mip.tail_y;
			tile.tail = level >= info.first_tail_level;
			tile.tiled_width = mip.pitch;
			tile.tiled_height = mip.height;
			tile.surface_z = mip.surface_z + layer;
			tiles.push_back(tile);
		}
	}
	return tiles;
}

// Depth and stencil use separate guest surfaces, even when Vulkan stores them
// in one native image. Keep the same one-byte stencil pitch for both directions.
[[nodiscard]] ImageInfo StencilPlaneInfo(const ImageInfo& depth) {
	auto plane = depth;
	plane.data = depth.stencil;
	plane.stencil = {};
	plane.guest_format = Prospero::BufferFormat::k8UInt;
	plane.pixel_format = vk::Format::eS8Uint;
	plane.bytes_per_block = 1;
	if (plane.IsTiled()) plane.pitch = TileGetDepthPitch(plane.extent.width, 1, 0);
	return plane;
}

[[nodiscard]] bool ValidDepthPlaneLayout(const ImageInfo& plane) {
	if (!plane.data.Valid() || plane.samples != 1 || plane.resources.levels != 1 ||
	    plane.resources.layers == 0 || plane.data.size % plane.resources.layers != 0 ||
	    plane.extent.depth != 1 || plane.IsVolume() || plane.extent.width == 0 ||
	    plane.extent.width > 16384 || plane.extent.height == 0 || plane.extent.height > 16384 ||
	    plane.pitch < plane.extent.width || plane.pitch > 16384 ||
	    (plane.bytes_per_block != 1 && plane.bytes_per_block != 2 && plane.bytes_per_block != 4))
		return false;
	const uint64_t stride = plane.data.size / plane.resources.layers;
	const uint64_t linear = static_cast<uint64_t>(plane.pitch) * plane.extent.height *
	                        plane.bytes_per_block;
	// Vulkan depth/stencil buffer-image copies require every bufferOffset to be
	// a multiple of four, including the offset of each array layer.
	if ((stride & 3u) != 0 || linear > stride) return false;
	if (!plane.IsTiled()) return true;
	TileSizeAlign minimum {};
	return plane.tile_mode == Prospero::TileMode::kDepth &&
	       TileGetRenderTargetSize(plane.extent.width, plane.extent.height, plane.pitch,
	                               plane.bytes_per_block, minimum, 0) &&
	       minimum.size <= stride;
}

} // namespace

TextureCache::TextureCache(GraphicContext& graphics, CommandScheduler& scheduler,
                           PageManager& page_manager, BufferCache& buffer_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_page_manager(page_manager),
      m_blit_helper(graphics, scheduler),
      m_tiler(graphics, scheduler, buffer_cache.GetUtilityBuffer(MemoryUsage::Stream)),
      m_buffer_cache(buffer_cache),
      m_readback_linear_images(Config::ReadbackLinearImagesEnabled()) {
	if (m_graphics.CanReportMemoryUsage()) {
		const auto thresholds = MemoryBudget::TextureThresholds(m_graphics.GetTotalMemoryBudget());
		m_trigger_gc_memory = thresholds.trigger;
		m_pressure_gc_memory = thresholds.pressure;
		m_critical_gc_memory = thresholds.critical;
	}
}

TextureCache::~TextureCache() {
	m_slot_images.ForEach([&](ImageId id, const Image& image) {
		if (image.registered) {
			UnregisterImage(id);
		}
	});
}

bool TextureCache::SameBacking(const ImageInfo& cached, const ImageInfo& requested,
                               bool exact_format) {
	if (cached.data.address != requested.data.address) {
		return false;
	}
	if (cached.data.size != requested.data.size) {
		return false;
	}
	if (cached.stencil != requested.stencil) {
		return false;
	}
	if (cached.extent != requested.extent) {
		return false;
	}
	if (cached.resources.levels < requested.resources.levels ||
	    cached.resources.layers < requested.resources.layers) {
		return false;
	}
	if (cached.samples != requested.samples) {
		return false;
	}
	if (cached.bytes_per_block != requested.bytes_per_block) {
		return false;
	}
	if (cached.tile_mode != requested.tile_mode) {
		return false;
	}
	if (cached.GetColorTransform() != requested.GetColorTransform()) {
		return false;
	}
	if (!ImageViewOps::FormatsCompatible(cached.pixel_format, requested.pixel_format) ||
	    cached.type != requested.type) {
		return false;
	}
	if (exact_format && cached.pixel_format != requested.pixel_format) {
		return false;
	}
	return true;
}

TextureCache::BindingType TextureCache::UploadBinding(const Image& image) {
	if (image.info.IsDepth()) {
		if (image.info.tile_mode == Prospero::TileMode::kDepth ||
		    image.info.tile_mode == Prospero::TileMode::kLinear) {
			return BindingType::DepthTarget;
		}
		return BindingType::Texture;
	}
	if (image.usage.render_target) {
		return BindingType::RenderTarget;
	}
	if (image.usage.video_out) {
		return BindingType::VideoOut;
	}
	return image.usage.storage ? BindingType::Storage : BindingType::Texture;
}

ImageId TextureCache::InsertImage(const ImageInfo& info) {
	const auto id = m_slot_images.insert(m_graphics, m_scheduler, info);
	if (!info.data.Empty()) {
		RegisterImage(id);
	}
	return id;
}

void TextureCache::RegisterImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.registered || image.info.data.Empty()) {
		EXIT("TextureCache: invalid image registration\n");
	}
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(image.info.data.address, image.info.data.size, pages)) {
		EXIT("TextureCache: image registration is outside the guest address space\n");
	}
	ForEachPage(image.info.data.address, image.info.data.size, [this, id](uint64_t page) {
		m_image_page_table[page].push_back(id);
	});
	image.registered = true;
	image.lru_id     = m_lru_cache.Insert(id, m_gc_tick);
	m_total_used_memory += image.AccountedSize();
}

void TextureCache::UnregisterImage(ImageId id) {
	ForgetPublicationSnapshot(id);
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	UntrackImage(id);
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(image.info.data.address, image.info.data.size, pages)) {
		EXIT("TextureCache: registered image is outside the guest address space\n");
	}
	ForEachPage(image.info.data.address, image.info.data.size, [this, id](uint64_t page) {
		auto* owners = m_image_page_table.Find(page);
		if (owners == nullptr || !owners->Erase(id)) {
			EXIT("TextureCache: image missing from page owner index\n");
		}
	});
	m_lru_cache.Free(image.lru_id);
	const auto accounted = image.AccountedSize();
	if (accounted > m_total_used_memory) {
		EXIT("TextureCache: image accounting underflow\n");
	}
	m_total_used_memory -= accounted;
	image.registered = false;
}

void TextureCache::DeleteImage(ImageId id) {
	auto* image = m_slot_images.try_get(id);
	if (image == nullptr || !image->registered) {
		return;
	}
	if (!image->depth_id) {
		std::vector<ImageId> associations;
		m_slot_images.ForEach([&](ImageId candidate, const Image& associated) {
			if (associated.depth_id == id) {
				associations.push_back(candidate);
			}
		});
		for (const auto association: associations) {
			FreeImage(association);
		}
	}
	if (image->IsGpuModified()) {
		EXIT("TextureCache: deleting a GPU-modified image without resolving its contents\n");
	}
	m_download_images.erase(id);
	if (image->info.HasMetadata()) {
		const auto metadata = m_surface_metas.find(image->info.metadata.range.address);
		if (metadata != m_surface_metas.end() &&
		    image->info.metadata.kind == ImageMetadataKind::Htile &&
		    metadata->second.type == MetaDataInfo::Type::HTile) {
			// A later binding may have reused this address for another metadata type.
			m_surface_metas.erase(metadata);
		}
	}
	UnregisterImage(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_images.erase(id); });
	} else {
		m_slot_images.erase(id);
	}
}

void TextureCache::FreeImage(ImageId id) {
	const auto* image = m_slot_images.try_get(id);
	if (image == nullptr || !image->registered) return;
	const auto parent = image->depth_id ? image->depth_id : id;
	std::vector<ImageId> planes {parent};
	m_slot_images.ForEach([&](ImageId candidate, const Image& associated) {
		if (associated.registered && associated.depth_id == parent) planes.push_back(candidate);
	});
	// A depth owner and its stencil association share native storage. Resolve
	// both while that storage and both guest ranges are still alive.
	PublishImageWrites(planes);
	DeleteImage(id);
}

void TextureCache::TouchImage(Image& image) {
	if (image.registered) {
		m_lru_cache.Touch(image.lru_id, m_gc_tick);
	}
}

void TextureCache::MarkAsMaybeDirty(ImageId id, Image& image) {
	image.MarkMaybeCpuDirty();
	if (image.NeedsMaybeCpuHash()) {
		image.SetMaybeCpuHash(image.HashGuestEdges());
	}
	UntrackImage(id);
}

void TextureCache::TrackImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_begin = image.info.data.address;
	const auto image_end   = image.info.data.End();
	if (image_begin == image.track_addr && image_end == image.track_addr_end) {
		return;
	}
	if (!image.IsTracked()) {
		image.track_addr     = image_begin;
		image.track_addr_end = image_end;
		m_page_manager.UpdatePageWatchers<true>(image_begin, image.info.data.size);
		return;
	}
	if (image_begin < image.track_addr) {
		TrackImageHead(id);
	}
	if (image.track_addr_end < image_end) {
		TrackImageTail(id);
	}
}

void TextureCache::TrackImageHead(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_begin = image.info.data.address;
	if (image_begin == image.track_addr) {
		return;
	}
	if (!image.IsTracked() || image_begin > image.track_addr) {
		EXIT("TextureCache: invalid image head tracking range\n");
	}
	const auto size  = image.track_addr - image_begin;
	image.track_addr = image_begin;
	m_page_manager.UpdatePageWatchers<true>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.registered) {
		return;
	}
	const auto image_end = image.info.data.End();
	if (image_end == image.track_addr_end) {
		return;
	}
	if (!image.IsTracked() || image.track_addr_end > image_end) {
		EXIT("TextureCache: invalid image tail tracking range\n");
	}
	const auto address   = image.track_addr_end;
	const auto size      = image_end - address;
	image.track_addr_end = image_end;
	m_page_manager.UpdatePageWatchers<true>(address, size);
}

void TextureCache::UntrackImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (!image.IsTracked()) {
		return;
	}
	const auto address   = image.track_addr;
	const auto size      = image.track_addr_end - image.track_addr;
	image.track_addr     = 0;
	image.track_addr_end = 0;
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(address, size);
	}
}

void TextureCache::UntrackImageHead(ImageId id) {
	auto&      image = m_slot_images[id];
	const auto begin = image.info.data.address;
	if (!image.IsTracked() || begin < image.track_addr) {
		return;
	}
	const auto address = Common::AlignDown(begin + TRACKER_PAGE_SIZE, TRACKER_PAGE_SIZE);
	const auto size    = address - begin;
	// Later CPU writes to this opened edge page no longer fault. Preserve the
	// first edge hash while keeping the interior pages watched.
	image.MarkMaybeCpuDirty();
	if (image.NeedsMaybeCpuHash()) {
		image.SetMaybeCpuHash(image.HashGuestEdges());
	}
	image.track_addr   = address;
	if (image.track_addr == image.track_addr_end) {
		MarkAsMaybeDirty(id, image);
	}
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(begin, size);
	}
}

void TextureCache::UntrackImageTail(ImageId id) {
	auto&      image = m_slot_images[id];
	const auto end   = image.info.data.End();
	if (!image.IsTracked() || image.track_addr_end < end) {
		return;
	}
	const auto address   = Common::AlignDown(end, TRACKER_PAGE_SIZE);
	const auto size      = end - address;
	// Later CPU writes to this opened edge page no longer fault. Preserve the
	// first edge hash while keeping the interior pages watched.
	image.MarkMaybeCpuDirty();
	if (image.NeedsMaybeCpuHash()) {
		image.SetMaybeCpuHash(image.HashGuestEdges());
	}
	image.track_addr_end = address;
	if (image.track_addr == image.track_addr_end) {
		MarkAsMaybeDirty(id, image);
	}
	if (size != 0) {
		m_page_manager.UpdatePageWatchers<false>(address, size);
	}
}

void TextureCache::TrackImageDownload(ImageId id, Image& image) {
	if (m_readback_linear_images && !image.info.IsTiled() && !image.info.data.Empty()) {
		if (!image.IsGpuModified()) {
			EXIT("TextureCache: cannot enroll a non-GPU-owned image for download\n");
		}
		m_download_images.insert(id);
	}
}

TextureCache::ImageIds TextureCache::FindImagesInRegion(uint64_t address, uint64_t size,
                                                        bool page_overlap) const {
	ImagePageTable::PageRange pages {};
	if (!ImagePageTable::TryGetPageRange(address, size, pages)) {
		return {};
	}

	uint32_t query_epoch = ++m_image_query_epoch;
	if (query_epoch == 0) {
		m_slot_images.ForEach([](ImageId, const Image& image) { image.query_epoch = 0; });
		query_epoch = ++m_image_query_epoch;
	}

	ImageIds result;
	ForEachPage(address, size, [&](uint64_t page) {
		const auto* owners = m_image_page_table.Find(page);
		if (owners == nullptr) {
			return;
		}
		owners->ForEach([&](ImageId id) {
			auto* image = m_slot_images.try_get(id);
			if (image == nullptr) {
				return;
			}
			if (image->query_epoch == query_epoch) {
				return;
			}
			image->query_epoch = query_epoch;
			if (image->Overlaps(address, size, page_overlap)) {
				result.push_back(id);
			}
		});
	});
	return result;
}

ImageId TextureCache::GetNullImage(const ImageDesc& desc) {
	const auto format = desc.info.pixel_format;
	if (const auto found = m_null_images.find(format); found != m_null_images.end()) {
		return found->second;
	}
	ImageInfo info {};
	info.pixel_format    = desc.info.pixel_format;
	info.guest_format    = desc.info.guest_format;
	info.type            = Prospero::ImageType::kColor2D;
	info.extent          = {1, 1, 1};
	info.resources       = {1, 1};
	info.pitch           = 1;
	info.bytes_per_block = std::max(desc.info.bytes_per_block, 1u);
	info.samples         = 1;
	info.tile_mode       = Prospero::TileMode::kLinear;
	info.mip_layout[0]   = {0, info.bytes_per_block, 1, 1};
	const auto id        = InsertImage(info);
	m_null_images.emplace(format, id);
	return id;
}

void TextureCache::ValidateImageDesc(const ImageDesc& desc) const {
	ImageOps::Validate(desc.info);
	if (desc.view_info.format == vk::Format::eUndefined || desc.view_info.level_count == 0 ||
	    desc.view_info.layer_count == 0 ||
	    desc.view_info.base_level >= desc.info.resources.levels ||
	    desc.view_info.level_count > desc.info.resources.levels - desc.view_info.base_level ||
	    (!desc.info.IsVolume() &&
	     (desc.view_info.base_layer >= desc.info.resources.layers ||
	      desc.view_info.layer_count > desc.info.resources.layers - desc.view_info.base_layer))) {
		EXIT("TextureCache: invalid image view description\n");
	}
	if (desc.type == BindingType::DepthTarget && !IsSupportedDepthTargetFormat(desc.info)) {
		EXIT("TextureCache: unsupported depth image description\n");
	}
	if (desc.type == BindingType::VideoOut && !IsSupportedVideoOutFormat(desc.info)) {
		EXIT("TextureCache: unsupported video-out image description\n");
	}
	if (desc.type == BindingType::VideoOut &&
	    desc.info.metadata.compression == VideoOutCompression::Unsupported) {
		EXIT("TextureCache: unsupported compressed video-out description\n");
	}
}

void TextureCache::PrepareImageCopy(Image& image) {
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
}

void TextureCache::RefreshCopySource(ImageId id) {
	auto& image = m_slot_images[id];
	RefreshImage(id);
	if (image.IsDefinitelyCpuDirty()) {
		EXIT("TextureCache: image copy source remained CPU-dirty after refresh\n");
	}
}

bool TextureCache::CopyD16(Image& destination, Image& source) {
	const bool source_depth      = source.info.IsDepth();
	const bool destination_depth = destination.info.IsDepth();
	if (source_depth == destination_depth) {
		return false;
	}
	auto&      depth          = source_depth ? source : destination;
	auto&      color          = source_depth ? destination : source;
	const auto transfer_bytes = DepthAspectTransferBytes(depth.backing.format);
	if (depth.info.bytes_per_block != sizeof(uint16_t) ||
	    color.info.bytes_per_block != sizeof(uint16_t) || transfer_bytes != sizeof(uint32_t)) {
		return false;
	}
	EXIT_IF(source.backing.samples != 1 || destination.backing.samples != 1 ||
	        source.info.resources.levels != 1 || destination.info.resources.levels != 1 ||
	        source.info.extent != destination.info.extent ||
	        source.info.resources.layers != destination.info.resources.layers);

	const auto     layers = depth.info.resources.layers;
	const uint64_t depth_slice =
	    static_cast<uint64_t>(depth.info.pitch) * depth.info.extent.height * transfer_bytes;
	const uint64_t color_slice =
	    static_cast<uint64_t>(color.info.pitch) * color.info.extent.height * sizeof(uint16_t);
	EXIT_IF(layers == 0 || depth_slice > UINT64_MAX / layers || color_slice > UINT64_MAX / layers);
	const auto                       depth_size = depth_slice * layers;
	const auto                       color_size = color_slice * layers;
	std::vector<vk::BufferImageCopy> depth_copies(layers);
	std::vector<vk::BufferImageCopy> color_copies(layers);
	for (uint32_t layer = 0; layer < layers; layer++) {
		depth_copies[layer].bufferOffset      = depth_slice * layer;
		depth_copies[layer].bufferRowLength   = depth.info.pitch;
		depth_copies[layer].bufferImageHeight = depth.info.extent.height;
		depth_copies[layer].imageSubresource  = {vk::ImageAspectFlagBits::eDepth, 0, layer, 1};
		depth_copies[layer].imageExtent       = depth.info.extent;
		color_copies[layer].bufferOffset      = color_slice * layer;
		color_copies[layer].bufferRowLength   = color.info.pitch;
		color_copies[layer].bufferImageHeight = color.info.extent.height;
		color_copies[layer].imageSubresource  = {vk::ImageAspectFlagBits::eColor, 0, layer, 1};
		color_copies[layer].imageExtent       = color.info.extent;
	}

	auto                         depth_buffer = m_tiler.GetScratchBuffer(depth_size);
	auto                         color_buffer = m_tiler.GetScratchBuffer(color_size, depth_buffer.buffer);
	const TileManager::D16Layout promote_layout {
	    .width               = depth.info.extent.width,
	    .height              = depth.info.extent.height,
	    .layers              = layers,
	    .source_row_stride   = static_cast<uint64_t>(color.info.pitch) * sizeof(uint16_t),
	    .target_row_stride   = static_cast<uint64_t>(depth.info.pitch) * transfer_bytes,
	    .source_slice_stride = color_slice,
	    .target_slice_stride = depth_slice,
	};
	const bool d32 = DepthAspectTransferFormat(depth.backing.format) == vk::Format::eD32Sfloat;
	if (source_depth) {
		source.Download(depth_copies, depth_buffer.buffer, depth_buffer.offset, depth_buffer.size);
		m_tiler.ConvertD16(depth_buffer, color_buffer, TileManager::D16Direction::Demote, d32,
		                   {.width               = promote_layout.width,
		                    .height              = promote_layout.height,
		                    .layers              = promote_layout.layers,
		                    .source_row_stride   = promote_layout.target_row_stride,
		                    .target_row_stride   = promote_layout.source_row_stride,
		                    .source_slice_stride = promote_layout.target_slice_stride,
		                    .target_slice_stride = promote_layout.source_slice_stride});
		destination.Upload(color_copies, color_buffer.buffer, color_buffer.offset,
		                   color_buffer.size);
	} else {
		source.Download(color_copies, color_buffer.buffer, color_buffer.offset, color_buffer.size);
		m_tiler.ConvertD16(color_buffer, depth_buffer, TileManager::D16Direction::Promote, d32,
		                   promote_layout);
		destination.Upload(depth_copies, depth_buffer.buffer, depth_buffer.offset,
		                   depth_buffer.size);
	}
	return true;
}

void TextureCache::CopyImage(ImageId destination_id, ImageId source_id) {
	if (m_slot_images[source_id].IsGpuModified() && !GuestGpuWriteOwnershipSupported())
        EXIT("PS5 GPU-owned image copying requires completed CPU backing publication; unavailable\n");
	RefreshCopySource(source_id);
	auto& destination = m_slot_images[destination_id];
	auto& source      = m_slot_images[source_id];
	if (source.IsGpuModified()) ValidateGpuWrite(destination);
	TrackImage(destination_id);
	if (source.backing.samples != destination.backing.samples) {
		EXIT("TextureCache: cannot issue an unequal-sample image copy\n");
	}
	PrepareImageCopy(destination);
	if (source.IsBufferModified()) {
		if (source.info.data == destination.info.data) {
			destination.MarkBufferModified();
		}
		return;
	}
	const bool source_depth = source.info.IsDepth();
	const bool dest_depth   = destination.info.IsDepth();
	const bool direct_copy =
	    source.info.GetColorTransform() == destination.info.GetColorTransform() &&
	    (source.backing.image_type == destination.backing.image_type ||
	     (source.backing.image_type != vk::ImageType::e1D &&
	      destination.backing.image_type != vk::ImageType::e1D)) &&
	    (source.backing.format == destination.backing.format ||
	     (!source_depth && !dest_depth &&
	      vk::blockSize(source.backing.format) == vk::blockSize(destination.backing.format)));
	if (direct_copy) {
		destination.CopyImage(source);
	} else if (!CopyD16(destination, source)) {
		if (source.backing.samples != 1 || destination.backing.samples != 1) {
			EXIT("TextureCache: cross-format multisample image copy is unsupported\n");
		}
		auto& copy_buffer = m_buffer_cache.GetUtilityBuffer(MemoryUsage::DeviceLocal);
		destination.CopyImageWithBuffer(source, copy_buffer, m_tiler);
	}
	if (source.IsGpuModified()) {
		destination.MarkGpuModified();
	}
	destination.ClearBufferModified();
}

void TextureCache::CopyImageMip(ImageId destination_id, ImageId source_id, uint32_t mip,
                                uint32_t layer) {
	if (m_slot_images[source_id].IsGpuModified() && !GuestGpuWriteOwnershipSupported())
        EXIT("PS5 GPU-owned mip copying requires completed CPU backing publication; unavailable\n");
	RefreshCopySource(source_id);
	auto& destination = m_slot_images[destination_id];
	auto& source      = m_slot_images[source_id];
	if (source.IsGpuModified()) ValidateGpuWrite(destination);
	TrackImage(destination_id);
	if (source.IsBufferModified() || source.backing.samples != destination.backing.samples) {
		EXIT("TextureCache: invalid mip-copy ownership or sample count\n");
	}
	destination.CopyMip(source, mip, layer);
	if (source.IsGpuModified()) {
		destination.MarkGpuModified();
	}
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested, BindingType binding,
                                          ImageId cached_id) {
	auto& cached = m_slot_images[cached_id];
	if ((!cached.info.IsDepth() && !requested.IsDepth()) ||
	    cached.info.tile_mode != requested.tile_mode) {
		return {};
	}
	const bool stencil_match = requested.HasStencil() == cached.info.HasStencil();
	const bool bpp_match     = requested.bytes_per_block == cached.info.bytes_per_block;
	// PPSA04264
	const bool raw_d16_texture =
	    binding == BindingType::Texture && cached.info.IsDepth() &&
	    cached.info.guest_format == Prospero::BufferFormat::k16UNorm &&
	    requested.guest_format == Prospero::BufferFormat::k16UInt &&
	    requested.pixel_format == vk::Format::eR16Uint && cached.backing.samples == 1 &&
	    requested.samples == 1 && requested.data == cached.info.data &&
	    requested.extent == cached.info.extent && requested.resources == cached.info.resources &&
	    requested.type == cached.info.type && requested.pitch == cached.info.pitch &&
	    !requested.HasStencil() && !cached.info.HasStencil() && !requested.HasMetadata() &&
	    !cached.info.HasMetadata();
	// PPSA04264, PPSA04288
	// A partial view retains the entire matching array layout. HTile belongs to
	// the depth source, independently of the data slices copied into color storage.
	const bool retain_cached_layout =
	    requested.samples == 1 && cached.info.samples == 1 && cached.backing.samples == 1 &&
	    requested.bytes_per_block == cached.info.bytes_per_block &&
	    requested.data.address == cached.info.data.address &&
	    requested.data.size < cached.info.data.size && requested.extent == cached.info.extent &&
	    requested.resources.levels == 1 && cached.info.resources.levels == 1 &&
	    requested.resources.layers != 0 && cached.info.resources.layers != 0 &&
	    requested.resources.layers < cached.info.resources.layers &&
	    requested.type == cached.info.type && requested.pitch == cached.info.pitch &&
	    requested.mip_layout[0].offset == 0 &&
	    cached.info.mip_layout[0].offset == 0 &&
	    requested.mip_layout[0].size == requested.data.size &&
	    cached.info.mip_layout[0].size == cached.info.data.size &&
	    requested.data.size % requested.resources.layers == 0 &&
	    cached.info.data.size % cached.info.resources.layers == 0 &&
	    requested.data.size / requested.resources.layers ==
	        cached.info.data.size / cached.info.resources.layers &&
	    !requested.HasStencil() && !cached.info.HasStencil() && !requested.HasMetadata() &&
	    (cached.info.metadata.kind == ImageMetadataKind::None ||
	     cached.info.metadata.kind == ImageMetadataKind::Htile);
	bool recreate = cached.info.resources < requested.resources ||
	                requested.IsVolume() != cached.info.IsVolume();
	switch (binding) {
		case BindingType::Texture:
			recreate |= requested.IsDepth() && !cached.info.IsDepth();
			recreate |= raw_d16_texture;
			break;
		case BindingType::Storage: recreate |= cached.info.IsDepth(); break;
		case BindingType::RenderTarget: recreate |= cached.info.IsDepth(); break;
		case BindingType::DepthTarget:
			recreate |= !cached.info.IsDepth();
			recreate |= cached.info.IsDepth() && !(stencil_match && bpp_match);
			break;
		case BindingType::VideoOut: recreate |= cached.info.IsDepth(); break;
	}
	if (!recreate) {
		return cached_id;
	}
	RefreshImage(cached_id);
	auto info = requested;
	if (retain_cached_layout) {
		info.data       = cached.info.data;
		info.resources  = cached.info.resources;
		info.mip_layout = cached.info.mip_layout;
		info.tiling = cached.info.tiling;
		info.tiled_slice_stride = cached.info.tiled_slice_stride;
		info.linear_slice_stride = cached.info.linear_slice_stride;
		info.first_tail_level = cached.info.first_tail_level;
	}
	info.htile_clear_mask     = 0;
	const auto replacement_id = InsertImage(info);
	auto&      replacement    = m_slot_images[replacement_id];
	replacement.usage         = cached.usage;
	if (cached.binding.is_bound || cached.binding.is_target) {
		cached.binding.needs_rebind = true;
	}
	if (cached.backing.samples == replacement.backing.samples) {
		const bool copy_supported =
		    cached.backing.samples == 1 || cached.backing.format == replacement.backing.format ||
		    (!cached.info.IsDepth() && !replacement.info.IsDepth() &&
		     ImageViewOps::FormatsCompatible(cached.backing.format, replacement.backing.format));
		if (copy_supported) {
			CopyImage(replacement_id, cached_id);
		} else {
			LOGF_COLOR(Log::Color::BrightYellow,
			           "TextureCache: unsupported cross-format multisample depth copy\n");
		}
	} else if (cached.backing.samples == 1 && replacement.backing.samples > 1 &&
	           replacement.info.IsDepth()) {
		RefreshCopySource(cached_id);
		if (cached.IsBufferModified() || cached.IsDefinitelyCpuDirty()) {
			EXIT("TextureCache: multisample depth conversion source is not native-current\n");
		}
		PrepareImageCopy(replacement);
		m_blit_helper.ReinterpretColorAsMsDepth(cached, replacement);
		CommitGpuWrite(replacement);
	} else {
		LOGF_COLOR(Log::Color::BrightYellow,
		           "TextureCache: unsupported unequal-sample depth overlap copy (%u -> %u)\n",
		           cached.backing.samples, replacement.backing.samples);
	}
	FreeImage(cached_id);
	return replacement_id;
}

TextureCache::OverlapResult TextureCache::ResolveOverlap(const ImageInfo& requested,
                                                         BindingType binding, ImageId cached_id,
                                                         ImageId merged_id) {
	auto owner = m_slot_images.try_get(cached_id);
	if (owner == nullptr) {
		return {merged_id};
	}
	auto&      cached       = *owner;
	const auto current_tick = m_scheduler.CurrentTick();
	const bool safe_to_delete =
	    current_tick - std::min(current_tick, cached.tick_accessed_last) > NumFramesBeforeRemoval;
	if (cached.info.IsDepth() && requested.IsDepth() && cached.info.data == requested.data &&
	    cached.info.stencil != requested.stencil) {
		// A new stencil allocation is a new native backing identity. Publish the
		// old aspect with its original owner before retiring that whole family;
		// deferred deletion preserves views from already recorded commands.
		if (cached.info.pixel_format != requested.pixel_format ||
		    cached.info.guest_format != requested.guest_format || cached.info.type != requested.type ||
		    cached.info.extent != requested.extent || cached.info.resources != requested.resources ||
		    cached.info.pitch != requested.pitch || cached.info.bytes_per_block != requested.bytes_per_block ||
		    cached.info.tile_mode != requested.tile_mode || cached.info.mip_layout != requested.mip_layout ||
		    cached.info.samples != 1 || requested.samples != 1 ||
		    cached.info.HasMetadata() || requested.HasMetadata() ||
		    !cached.info.HasStencil() || !requested.HasStencil())
			EXIT("TextureCache: stencil retarget requires an unchanged supported depth layout\n");
		const auto old_info = cached.info;
		const auto old_handle = cached.backing.image;
		std::vector<ImageId> family {cached_id};
		m_slot_images.ForEach([&](ImageId candidate, const Image& plane) {
			if (plane.registered && plane.depth_id == cached_id) family.push_back(candidate);
		});
		PublishImageWrites(family);
		const auto* retained = m_slot_images.try_get(cached_id);
		if (retained == nullptr || !retained->registered || retained->depth_id ||
		    retained->backing.image != old_handle || retained->info.data != old_info.data ||
		    retained->info.stencil != old_info.stencil || retained->info.extent != old_info.extent ||
		    retained->info.resources != old_info.resources || retained->info.pitch != old_info.pitch ||
		    retained->info.pixel_format != old_info.pixel_format || retained->info.samples != old_info.samples ||
		    retained->info.tile_mode != old_info.tile_mode || retained->info.mip_layout != old_info.mip_layout)
			EXIT("TextureCache: depth owner changed during stencil retarget publication\n");
		cached.binding.needs_rebind |= cached.binding.is_bound || cached.binding.is_target;
		FreeImage(cached_id);
		return {merged_id};
	}

	const uint32_t requested_block = requested.bytes_per_block * requested.samples;
	const uint32_t cached_block    = cached.info.bytes_per_block * cached.info.samples;
	if (requested.data.address == cached.info.data.address &&
	    requested.BlockExtent() == cached.info.BlockExtent() && requested_block == cached_block) {
		if (const auto depth_id = ResolveDepthOverlap(requested, binding, cached_id)) {
			return {depth_id};
		}
		// Equal pitch does not imply equal mip placement: a changed extent can move
		// a level into or out of the mip tail. These are separate guest layouts.
		if (requested.tile_mode != cached.info.tile_mode ||
		    (requested.resources == cached.info.resources &&
		     requested.mip_layout != cached.info.mip_layout)) {
			if (safe_to_delete) {
				FreeImage(cached_id);
			}
			return {merged_id};
		}
		if (requested.GetColorTransform() != cached.info.GetColorTransform()) {
			return {ExpandImage(requested, cached_id)};
		}
		if (requested.IsBlock() && !cached.info.IsBlock()) {
			return {ExpandImage(requested, cached_id)};
		}
		// Volume depth is not an array-layer count. A larger depth can retain the
		// same block-slice layout while requiring a larger native image.
		if ((requested.IsVolume() || cached.info.IsVolume()) &&
		    (requested.data.size == cached.info.data.size ||
		     (requested.type == cached.info.type && requested.resources == cached.info.resources &&
		      requested.extent.width == cached.info.extent.width &&
		      requested.extent.height == cached.info.extent.height &&
		      requested.extent.depth > cached.info.extent.depth))) {
			return {ExpandImage(requested, cached_id)};
		}
		// PPSA08394
		// A view cannot change the native image type or grow its extent.
		if (requested.data.size == cached.info.data.size &&
		    requested.resources == cached.info.resources &&
		    ImageViewOps::FormatsCompatible(cached.info.pixel_format, requested.pixel_format) &&
		    (requested.type != cached.info.type
		         ? requested.extent == cached.info.extent
		         : requested.extent.width > cached.info.extent.width &&
		               requested.extent.height >= cached.info.extent.height &&
		               requested.extent.depth >= cached.info.extent.depth)) {
			return {ExpandImage(requested, cached_id)};
		}
		// PS5 mip tails can expose more levels without increasing the guest allocation.
		if (requested.pixel_format == cached.info.pixel_format &&
		    requested.type == cached.info.type && requested.resources > cached.info.resources &&
		    (requested.data.size > cached.info.data.size ||
		     (requested.data.size == cached.info.data.size &&
		      requested.extent == cached.info.extent &&
		      cached.info.resources.levels > 1 &&
		      requested.resources.layers == cached.info.resources.layers))) {
			return {ExpandImage(requested, cached_id)};
		}
		if (requested.type != cached.info.type) {
			return {merged_id};
		}
		if (requested.pixel_format != cached.info.pixel_format ||
		    requested.data.size <= cached.info.data.size) {
			const auto result_id = merged_id ? merged_id : cached_id;
			const auto result    = m_slot_images.try_get(result_id);
			return {result != nullptr && ImageViewOps::FormatsCompatible(result->info.pixel_format,
			                                                             requested.pixel_format)
			            ? result_id
			            : ImageId {}};
		}
		EXIT("TextureCache: unresolvable equal-address image overlap, address=0x%016" PRIx64
		     " requested=%ux%u "
		     "cached=%ux%u requested_size=0x%016" PRIx64 " cached_size=0x%016" PRIx64
		     " type=%u/%u tile=%u/%u\n",
		     requested.data.address, requested.resources.levels, requested.resources.layers,
		     cached.info.resources.levels, cached.info.resources.layers, requested.data.size,
		     cached.info.data.size, static_cast<uint32_t>(requested.type),
		     static_cast<uint32_t>(cached.info.type), static_cast<uint32_t>(requested.tile_mode),
		     static_cast<uint32_t>(cached.info.tile_mode));
	}

	const int32_t requested_mip = requested.MipOf(cached.info);
	if (requested_mip >= 0) {
		const int32_t layer = requested.SliceOf(cached.info, requested_mip);
		return {cached_id, requested_mip, layer};
	}

	const int32_t mip = cached.info.MipOf(requested);
	if (mip >= 0) {
		const int32_t layer = cached.info.SliceOf(requested, mip);
		if (!merged_id) {
			return {ExpandImage(requested, cached_id)};
		}
		cached.binding.needs_rebind |= cached.binding.is_bound || cached.binding.is_target;
		m_slot_images[merged_id].binding.is_target |= cached.binding.is_target;
		CopyImageMip(merged_id, cached_id, static_cast<uint32_t>(mip),
		             static_cast<uint32_t>(layer));
		FreeImage(cached_id);
		return {merged_id};
	}
	if (requested.data.address >= cached.info.data.address && safe_to_delete) {
		FreeImage(cached_id);
	}
	return {merged_id};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId source_id) {
	RefreshCopySource(source_id);
	const auto expanded_id = InsertImage(info);
	auto&      source      = m_slot_images[source_id];
	if (source.binding.is_bound || source.binding.is_target) {
		source.binding.needs_rebind = true;
	}
	InitializeImage(expanded_id);
	const int32_t mip = source.info.MipOf(info);
	const int32_t layer = source.info.SliceOf(info, mip);
	if (layer >= 0) {
		CopyImageMip(expanded_id, source_id, static_cast<uint32_t>(mip),
		             static_cast<uint32_t>(layer));
	} else {
		CopyImage(expanded_id, source_id);
	}
	FreeImage(source_id);
	return expanded_id;
}

struct TextureCache::TextureTransfer {
	uint32_t                         levels = 0;
	uint64_t                         linear_size = 0;
	std::vector<vk::BufferImageCopy> regions;
	std::vector<GpuTileInfo>         tiles;
	ColorTransform                   color_transform = ColorTransform::None;
	bool                             valid           = false;

	[[nodiscard]] uint64_t LinearSize() const { return linear_size; }
};

struct TextureCache::ImageDownload {
	TextureTransfer texture;
	bool                depth_target = false;
	bool                valid        = false;
};

bool TextureCache::CoalescePublicationRanges(std::vector<GuestRange>& ranges) {
	// Validate everything before modifying the caller's list. Merging retains
	// exactly the union; it never widens across a hole or a guest address window.
	if (std::any_of(ranges.begin(), ranges.end(), [](const auto& range) { return !range.Valid(); }))
		return false;
	std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) {
		return a.address < b.address;
	});
	size_t count = 0;
	for (const auto range: ranges) {
		if (count != 0 && range.address <= ranges[count - 1].End()) {
			auto& previous = ranges[count - 1];
			previous.size = std::max(previous.End(), range.End()) - previous.address;
		} else {
			ranges[count++] = range;
		}
	}
	ranges.resize(count);
	return true;
}

bool TextureCache::DownloadFullyOverwrites(const Image& image, const ImageDownload& transfer) {
	const auto& info = image.info;
	const auto& texture = transfer.texture;
	// Tilers and numeric conversion shaders have independent padding contracts.
	// Admit only raw linear copies whose written byte union is the whole owner.
	if (!transfer.valid || transfer.depth_target || !texture.valid || !texture.tiles.empty() ||
	    texture.color_transform != ColorTransform::None || !info.data.Valid() || info.tile_mode != Prospero::TileMode::kLinear ||
	    info.IsDepth() || info.HasStencil() || info.IsBlock() || info.bgra16 ||
	    info.samples != 1 || image.backing.samples != 1 || info.bytes_per_block == 0 ||
	    Prospero::NumBytesPerElement(info.guest_format) != info.bytes_per_block ||
	    Prospero::RemapTextureFormat(info.guest_format) != info.guest_format ||
	    VulkanFormat(info.guest_format) != info.pixel_format ||
	    image.backing.format != info.pixel_format || texture.regions.empty()) return false;
	std::vector<GuestRange> written;
	written.reserve(texture.regions.size());
	for (const auto& region: texture.regions) {
		const auto& extent = region.imageExtent;
		if (extent.width == 0 || extent.height == 0 || extent.depth != 1 ||
		    region.imageSubresource.layerCount != 1 ||
		    region.imageSubresource.aspectMask != vk::ImageAspectFlagBits::eColor ||
		    (region.bufferRowLength != 0 && region.bufferRowLength != extent.width) ||
		    (region.bufferImageHeight != 0 && region.bufferImageHeight != extent.height)) return false;
		const uint64_t texels = uint64_t {extent.width} * extent.height;
		if (texels > UINT64_MAX / info.bytes_per_block) return false;
		const auto bytes = texels * info.bytes_per_block;
		if (region.bufferOffset > info.data.size || bytes > info.data.size - region.bufferOffset)
			return false;
		written.push_back({info.data.address + region.bufferOffset, bytes});
	}
	return CoalescePublicationRanges(written) && written.size() == 1 && written.front() == info.data;
}

bool TextureCache::DownloadFullyOverwrites(const Image& image) const {
	return DownloadFullyOverwrites(image, BuildDownload(image));
}

bool TextureCache::BuildPublicationOverwriteBlocks(GuestRange owner,
    std::span<const GpuTileInfo> tiles, std::vector<GuestRange>& overwritten) {
	overwritten.clear();
	if (!owner.Valid() || owner.size > UINT32_MAX || tiles.empty() || tiles.size() > 4096)
		return false;
	std::vector<GuestRange> footprints, complete;
	for (const auto& tile: tiles) {
		TileBlockLayout block {};
		const uint64_t width = tile.tiled_width != 0 ? tile.tiled_width : tile.pitch;
		const uint64_t height = tile.tiled_height != 0 ? tile.tiled_height : tile.height;
		// A whole 2D block is a permutation of all its bytes, including the
		// per-layer XOR. Partial blocks and mip tails retain their entire seed.
		// Volume blocks need a separate coverage proof and are deliberately absent.
		if (!TileGetBlockLayout(tile.family, tile.bytes_per_element, block) ||
		    block.block_depth != 1 || tile.depth != 1 || tile.tail ||
		    tile.width == 0 || tile.height == 0 || tile.pitch < tile.width ||
		    width < tile.width || height < tile.height || (tile.tiled_offset & 3u) != 0 ||
		    tile.tiled_size == 0 || tile.tiled_offset > owner.size ||
		    tile.tiled_size > owner.size - tile.tiled_offset)
			return false;
		const uint64_t columns = (width + block.block_width - 1) / block.block_width;
		const uint64_t rows = (height + block.block_height - 1) / block.block_height;
		if (columns > UINT32_MAX || rows > UINT32_MAX ||
		    columns * rows > tile.tiled_size / block.block_size)
			return false;
		const uint64_t full_columns = tile.width / block.block_width;
		const uint64_t full_rows = tile.height / block.block_height;
		if (full_rows > 4096 || complete.size() + full_rows > 4096) return false;
		const GuestRange footprint {owner.address + tile.tiled_offset, tile.tiled_size};
		for (const auto& other: footprints) {
			if (ImageRangeOverlaps(footprint.address, footprint.size, other.address, other.size))
				return false;
		}
		footprints.push_back(footprint);
		if (full_columns == 0) continue;
		for (uint64_t row = 0; row < full_rows; ++row) {
			complete.push_back({footprint.address + row * columns * block.block_size,
			    full_columns * block.block_size});
		}
	}
	if (complete.empty() || !CoalescePublicationRanges(complete)) return false;
	overwritten = std::move(complete);
	return true;
}

bool TextureCache::BuildPublicationOverwriteBlocks(const Image& image,
    const ImageDownload& transfer, std::vector<GuestRange>& overwritten) const {
	overwritten.clear();
	if (!transfer.valid) return false;
	if (transfer.depth_target) {
		ImageInfo plane;
		if (!BuildDepthPlane(image, plane) || !plane.IsTiled()) return false;
		const auto transfer_bytes = image.depth_id ? 1u : DepthAspectTransferBytes(plane.pixel_format);
		if (transfer_bytes != plane.bytes_per_block) return false;
		return BuildPublicationOverwriteBlocks(image.info.data, BuildDepthTiles(plane), overwritten);
	}
	if (image.info.IsBlock() || transfer.texture.color_transform != ColorTransform::None || !transfer.texture.valid)
		return false;
	return BuildPublicationOverwriteBlocks(image.info.data, transfer.texture.tiles, overwritten);
}

bool TextureCache::DepthTiledColorReadbackSupported(const Image& image) const {
	const auto& info = image.info;
	// A depth surface can be reinterpreted as an ordinary native color image.
	// Its byte layout remains Depth64KB; this is the inverse of the existing
	// raw upload, without a depth-aspect or numeric format conversion.
	if (!image.registered || image.depth_id || image.backing.image == nullptr || info.IsDepth() ||
	    info.tile_mode != Prospero::TileMode::kDepth || info.type != Prospero::ImageType::kColor2D ||
	    image.backing.image_type != vk::ImageType::e2D || info.samples != 1 ||
	    image.backing.samples != 1 || info.resources.levels != 1 ||
	    image.backing.mip_levels != info.resources.levels ||
	    image.backing.layers != info.resources.layers || image.backing.extent != info.extent ||
	    image.backing.format != info.pixel_format || info.HasStencil() || info.HasMetadata() ||
	    !info.metadata.range.Empty() || info.metadata.stencil_compressed ||
	    info.metadata.compression != VideoOutCompression::Uncompressed || info.bgra16 ||
	    info.bytes_per_block != sizeof(uint32_t) ||
	    Prospero::RemapTextureFormat(info.guest_format) != info.guest_format ||
	    VulkanFormat(info.guest_format) != info.pixel_format ||
	    info.pixel_format == vk::Format::eUndefined || !ValidDepthPlaneLayout(info) ||
	    info.mip_layout[0].offset != 0 || info.mip_layout[0].size != info.data.size)
		return false;
	TileTextureElementLayout element {};
	TileSurfaceLayout surface {};
	const TileSurfaceDescription description {info.guest_format, info.tile_mode,
	    TileSurfaceDimension::Dim2D, info.extent.width, info.extent.height, 1, 1,
	    info.resources.layers};
	return TileGetTextureElementLayout(info.guest_format, element) &&
	       element.bytes == info.bytes_per_block && element.texel_width == 1 &&
	       element.texel_height == 1 && TileGetTiledTextureLayout(description, surface) &&
	       surface.texture.block.family == TileBlockFamily::Depth64KB &&
	       surface.block_slice_size <= info.data.size / info.resources.layers &&
	       TileGetTexturePitch(info.guest_format, info.extent.width, info.tile_mode) == info.pitch;
}

TextureCache::TextureTransfer
TextureCache::BuildTextureTransfer(const Image& image, BindingType binding,
                                    TransferDirection direction) const {
	TextureTransfer transfer;
	const auto& info = image.info;
	const bool upload = direction == TransferDirection::Upload;
	if (!info.data.Valid() || info.resources.levels == 0 || info.resources.levels > info.mip_layout.size() ||
	    info.resources.layers == 0 || info.samples != 1 || image.backing.samples != 1 ||
	    info.metadata.compression != VideoOutCompression::Uncompressed ||
	    (info.IsTiled() && Prospero::IsFmaskTextureFormat(info.guest_format))) return transfer;
	if (!upload && info.tile_mode == Prospero::TileMode::kDepth &&
	    !DepthTiledColorReadbackSupported(image)) return transfer;
	if ((binding == BindingType::RenderTarget || binding == BindingType::VideoOut) &&
	    info.data.size % info.resources.layers != 0) return transfer;
	transfer.levels = info.resources.levels;
	transfer.linear_size = info.LinearSize();
	if (transfer.linear_size == 0 || (!info.IsTiled() && transfer.linear_size > info.data.size))
		return transfer;
	transfer.color_transform = info.GetColorTransform();
	transfer.regions = info.BufferCopies();
	transfer.tiles = BuildPublicationTiles(info);
	transfer.valid = !transfer.regions.empty();
	return transfer;
}

TextureCache::ImageDownload TextureCache::BuildDownload(const Image& image) const {
	const auto&  info    = image.info;
	const auto   binding = image.depth_id ? BindingType::DepthTarget : UploadBinding(image);
	ImageDownload transfer {.depth_target = binding == BindingType::DepthTarget};
	if (transfer.depth_target) {
		ImageInfo plane;
		transfer.valid = BuildDepthPlane(image, plane);
		return transfer;
	}
	if (info.samples != 1 || image.backing.samples != 1) return transfer;
	if (info.metadata.compression != VideoOutCompression::Uncompressed) {
		return transfer;
	}
	transfer.texture = BuildTextureTransfer(image, binding, TransferDirection::Download);
	transfer.valid   = transfer.texture.valid;
	return transfer;
}

void TextureCache::TraceImagePublication(ImageId id, const ImageDownload& transfer) const {
	const auto sample = ImageOwnerTraceReservation(true);
	if (sample == 0) return;
	const auto* trace = GpuSliceTrace::current;
	const auto& image = m_slot_images[id];
	const auto* native_owner = image.depth_id ? m_slot_images.try_get(image.depth_id) : &image;
	auto info = image.info;
	const bool plane_valid = !transfer.depth_target || BuildDepthPlane(image, info);
	const auto aspect = image.depth_id ? vk::ImageAspectFlagBits::eStencil :
	    info.IsDepth() ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor;
	const auto region_records = std::min<size_t>(transfer.texture.regions.size(), 16);
	std::fprintf(stderr, "PS5_GPU_IMAGE_OWNER_V1 sample=%llu slice_sequence=%llu submit_id=%llu epoch_token=%llu owner_index=%u owner_generation=%u publication_revision=%llu guest_address=%llu owner_bytes=%llu width=%u height=%u depth=%u layers=%u mip_levels=%u pitch=%u block_bytes=%u format=%u guest_format=%u tile_mode=%u samples=%u image_type=%u depth_plane=%u plane_valid=%u aspect=%u metadata_kind=%u compression=%u depth_owner_index=%u depth_owner_generation=%u native_layers=%u native_mips=%u download_regions=%zu download_region_records=%zu download_tiles=%zu linear_bytes=%llu scope=full_owner_publication actual_written=unknown\n",
	    static_cast<unsigned long long>(sample), static_cast<unsigned long long>(trace->Sequence()),
	    static_cast<unsigned long long>(trace->submit_id), static_cast<unsigned long long>(trace->Epoch()),
	    id.index, id.generation, static_cast<unsigned long long>(image.PublicationRevision()),
	    static_cast<unsigned long long>(info.data.address), static_cast<unsigned long long>(info.data.size),
	    info.extent.width, info.extent.height, info.extent.depth, info.resources.layers, info.resources.levels,
	    info.pitch, info.bytes_per_block, static_cast<uint32_t>(info.pixel_format),
	    static_cast<uint32_t>(info.guest_format), static_cast<uint32_t>(info.tile_mode), info.samples,
	    static_cast<uint32_t>(info.type), transfer.depth_target ? 1u : 0u, plane_valid ? 1u : 0u,
	    static_cast<uint32_t>(aspect), static_cast<uint32_t>(info.metadata.kind),
	    static_cast<uint32_t>(info.metadata.compression), image.depth_id.index, image.depth_id.generation,
	    native_owner != nullptr ? native_owner->backing.layers : 0u,
	    native_owner != nullptr ? native_owner->backing.mip_levels : 0u,
	    transfer.texture.regions.size(), region_records,
	    transfer.texture.tiles.size(), static_cast<unsigned long long>(transfer.depth_target ? 0 : transfer.texture.LinearSize()));
	// Storage metadata and download regions describe publication work, not the
	// set of subresources actually written by draws/dispatches. A depth plane
	// uses DownloadDepth rather than the color region list (linear_bytes=0).
	for (uint32_t mip = 0; mip < std::min<size_t>(info.resources.levels, info.mip_layout.size()); ++mip) {
		const auto& layout = info.mip_layout[mip];
		std::fprintf(stderr, "PS5_GPU_IMAGE_MIP_V1 sample=%llu mip=%u offset=%llu bytes=%llu pitch=%u height=%u scope=storage_metadata\n",
		    static_cast<unsigned long long>(sample), mip, static_cast<unsigned long long>(layout.offset),
		    static_cast<unsigned long long>(layout.size), layout.pitch, layout.height);
	}
	for (size_t i = 0; i < region_records; ++i) {
		const auto& region = transfer.texture.regions[i];
		std::fprintf(stderr, "PS5_GPU_IMAGE_REGION_V1 sample=%llu region=%zu mip=%u base_layer=%u layer_count=%u aspect=%u width=%u height=%u depth=%u buffer_offset=%llu row_length=%u image_height=%u scope=download_region\n",
		    static_cast<unsigned long long>(sample), i, region.imageSubresource.mipLevel,
		    region.imageSubresource.baseArrayLayer, region.imageSubresource.layerCount,
		    static_cast<uint32_t>(region.imageSubresource.aspectMask), region.imageExtent.width,
		    region.imageExtent.height, region.imageExtent.depth,
		    static_cast<unsigned long long>(region.bufferOffset), region.bufferRowLength, region.bufferImageHeight);
	}
}

bool TextureCache::BuildDepthPlane(const Image& image, ImageInfo& plane) const {
	const auto* owner = image.depth_id ? m_slot_images.try_get(image.depth_id) : &image;
	if (owner == nullptr || owner->depth_id || !owner->registered ||
	    owner->backing.image == nullptr || owner->info.samples != 1 || owner->backing.samples != 1 ||
	    owner->backing.layers != owner->info.resources.layers ||
	    owner->backing.mip_levels != owner->info.resources.levels ||
	    owner->backing.extent != owner->info.extent ||
	    owner->backing.format != owner->info.pixel_format ||
	    !IsSupportedDepthPlaneReadback(owner->info) ||
	    owner->info.HasMetadata() ||
	    owner->info.metadata.compression != VideoOutCompression::Uncompressed ||
	    owner->info.metadata.stencil_compressed || !ValidDepthPlaneLayout(owner->info))
		return false;
	if (owner->info.HasStencil()) {
		const auto stencil = StencilPlaneInfo(owner->info);
		if (!ValidDepthPlaneLayout(stencil) ||
		    ImageRangeOverlaps(owner->info.data.address, owner->info.data.size,
		                       stencil.data.address, stencil.data.size) ||
		    !(ImageViewOps::DepthAspectMask(owner->backing.format) & vk::ImageAspectFlagBits::eStencil))
			return false;
	}
	if (image.depth_id) {
		if (!image.registered || !owner->info.HasStencil() || image.info.data != owner->info.stencil ||
		    image.info.extent != owner->info.extent) return false;
		plane = StencilPlaneInfo(owner->info);
	} else {
		plane = owner->info;
	}
	plane.UpdateSize();
	return true;
}

void TextureCache::UploadImage(Image& image, Buffer& source, uint64_t source_offset) {
	auto& destination = image.depth_id ? m_slot_images[image.depth_id] : image;
	auto info = image.depth_id ? StencilPlaneInfo(destination.info) : image.info;
	const auto  binding     = image.depth_id ? BindingType::DepthTarget : UploadBinding(image);
	if (binding != BindingType::DepthTarget) {
		if ((binding == BindingType::RenderTarget || binding == BindingType::VideoOut) &&
		    (info.resources.layers == 0 || info.data.size % info.resources.layers != 0 ||
		     info.samples != 1 || image.backing.samples != 1)) {
			EXIT("TextureCache: invalid color-attachment upload\n");
		}
		if (binding == BindingType::VideoOut &&
		    info.metadata.compression != VideoOutCompression::Uncompressed) {
			EXIT("TextureCache: invalid video-out upload\n");
		}
		const auto linear = m_tiler.Detile(source.Handle(), source_offset, info);
		const auto copies = info.BufferCopies(linear.offset);
		destination.Upload(copies, linear.buffer, linear.offset, linear.size);
		return;
	}

	if (!ValidDepthPlaneLayout(info) || info.samples != 1 || destination.backing.samples != 1 ||
	    info.resources.layers == 0 || info.data.size % info.resources.layers != 0 ||
	    Prospero::NumBytesPerElement(info.guest_format) != info.bytes_per_block) {
		EXIT("TextureCache: invalid depth upload\n");
	}
	if (image.depth_id) info.UpdateSize();
	const auto layers          = info.resources.layers;
	const auto full_slice_size = info.linear_slice_stride;
	auto       linear          = m_tiler.Detile(source.Handle(), source_offset, info);
	auto       copies          = info.BufferCopies(linear.offset);
	if (image.depth_id) {
		for (auto& copy: copies) {
			copy.imageSubresource.baseArrayLayer += image.stencil_subresources.base_layer;
		}
	}
	const auto transfer_bytes = image.depth_id ? 1u : DepthAspectTransferBytes(info.pixel_format);
	if (transfer_bytes != info.bytes_per_block) {
		const uint64_t texels_per_slice = static_cast<uint64_t>(info.pitch) * info.extent.height;
		EXIT_NOT_IMPLEMENTED(info.bytes_per_block != sizeof(uint16_t) ||
		                     transfer_bytes != sizeof(uint32_t) || texels_per_slice > UINT32_MAX ||
		                     texels_per_slice > UINT64_MAX / transfer_bytes);
		const uint64_t transfer_slice = texels_per_slice * transfer_bytes;
		EXIT_NOT_IMPLEMENTED(transfer_slice > UINT64_MAX / layers);
		auto promoted = m_tiler.GetScratchBuffer(transfer_slice * layers, linear.buffer);
		m_tiler.ConvertD16(
		    linear, promoted, TileManager::D16Direction::Promote,
		    info.pixel_format == vk::Format::eD32SfloatS8Uint,
		    {.width               = info.extent.width,
		     .height              = info.extent.height,
		     .layers              = layers,
		     .source_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint16_t),
		     .target_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint32_t),
		     .source_slice_stride = full_slice_size,
		     .target_slice_stride = transfer_slice});
		linear = promoted;
		copies.resize(1);
		copies[0].bufferOffset      = linear.offset;
		copies[0].bufferRowLength   = info.pitch;
		copies[0].bufferImageHeight = info.extent.height;
		copies[0].imageSubresource  = {vk::ImageAspectFlagBits::eDepth, 0, 0, layers};
		copies[0].imageExtent       = info.extent;
	}
	destination.Upload(copies, linear.buffer, linear.offset, linear.size);
}

void TextureCache::InitializeImage(ImageId id) {
	auto& image = m_slot_images[id];
	if (image.info.data.Empty()) {
		return;
	}
	if (ReusePublicationSnapshot(id)) return;
    if (image.IsGpuModified() && !GuestGpuWriteOwnershipSupported())
        EXIT("PS5 GPU image ownership requires completed CPU backing publication; unavailable\n");
    // Without an exact snapshot match, read-only images still consume a fresh
    // complete backing copy. Native page protections are not a write witness.
    if (!image.IsGpuModified())
        image.InvalidateCpuWrite(image.info.data.address, image.info.data.size);
	TrackImage(id);
	if (image.info.metadata.compression != VideoOutCompression::Uncompressed) {
		if (image.IsCpuDirty()) {
			image.RefreshComplete();
		}
		return;
	}
	if (image.info.samples > 1) {
		return;
	}
	PublicationBuffer upload_snapshot;
	bool captured_upload = false;
	const bool upload = image.IsBufferModified() || image.IsCpuDirty();
	if (upload) {
		// Another view can own only part of this larger guest allocation. Complete
		// its native writes before the upload reads backing, retaining every
		// plane in a depth/stencil owner family until that completion.
		std::vector<ImageId> dependencies;
		for (const auto source_id:
		     FindImagesInRegion(image.info.data.address, image.info.data.size, false)) {
			const auto* source = m_slot_images.try_get(source_id);
			if (source == nullptr || !source->registered || !source->IsGpuModified()) continue;
			const auto parent = source->depth_id ? source->depth_id : source_id;
			m_slot_images.ForEach([&](ImageId candidate, const Image& plane) {
				if ((candidate == parent || plane.depth_id == parent) && plane.registered &&
				    plane.IsGpuModified() &&
				    std::ranges::find(dependencies, candidate) == dependencies.end())
					dependencies.push_back(candidate);
			});
		}
		if (!dependencies.empty()) {
			const auto target_range = image.info.data;
			const auto target_depth = image.depth_id;
			const auto target_info = image.info;
			const auto native_id = target_depth ? target_depth : id;
			const auto* native = m_slot_images.try_get(native_id);
			if (native == nullptr || !native->registered || native->backing.image == nullptr)
				EXIT("TextureCache: image upload dependency has no native destination\n");
			const auto native_handle = native->backing.image;
			const auto native_range = native->info.data;
			const auto native_stencil = native->info.stencil;
			PublishImageWrites(dependencies);
			const auto* target = m_slot_images.try_get(id);
			native = m_slot_images.try_get(native_id);
			if (target == nullptr || !target->registered || target->info.data != target_range ||
			    target->depth_id != target_depth || target->info.extent != target_info.extent ||
			    target->info.resources != target_info.resources || target->info.pitch != target_info.pitch ||
			    target->info.tile_mode != target_info.tile_mode ||
			    target->info.guest_format != target_info.guest_format ||
			    target->info.pixel_format != target_info.pixel_format ||
			    target->info.bytes_per_block != target_info.bytes_per_block ||
			    target->info.samples != target_info.samples ||
			    target->info.mip_layout != target_info.mip_layout || native == nullptr ||
			    !native->registered || native->depth_id || native->backing.image != native_handle ||
			    native->info.data != native_range || native->info.stencil != native_stencil)
				EXIT("TextureCache: image upload destination changed during source publication\n");
		}
		if (CanSnapshotUpload(image)) {
			ForgetPublicationSnapshot(id);
			upload_snapshot = ReservePublicationSnapshot(image.info.data.size);
		}
		const auto capture = upload_snapshot.data() == nullptr ? std::span<uint8_t> {} :
		    std::span<uint8_t> {upload_snapshot.data(), static_cast<size_t>(upload_snapshot.size())};
		const auto [source, source_offset] = m_buffer_cache.ObtainBufferForImage(
		    image.info.data.address, image.info.data.size, capture, &captured_upload);
		if (source == nullptr) {
			EXIT("TextureCache: failed to obtain image upload source\n");
		}
		UploadImage(image, *source, source_offset);
		image.ClearBufferModified();
	}
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
	if (captured_upload && CanSnapshotUpload(image) && !image.IsGpuModified() &&
	    !image.IsBufferModified() && !image.IsCpuDirty()) {
		// These are the exact bytes submitted through staging. Queue ordering
		// retains that source until the upload finishes; reuse still compares
		// every guest byte and checks the native owner on each acquisition.
		ForgetPublicationSnapshot(id);
		PublicationSnapshot ready {image.info, image.backing.image, UploadBinding(image),
		    image.PublicationRevision(), std::move(upload_snapshot), m_gc_tick, true};
		m_publication_snapshot_bytes += ready.bytes.size();
		m_publication_snapshots.insert_or_assign(id, std::move(ready));
		++m_publication_stats.upload_captures;
	} else {
		RecyclePublicationBuffer(std::move(upload_snapshot));
	}
}

void TextureCache::MaterializeColorClear(ImageId id, const ImageDesc& desc,
                                       uint32_t metadata_base_layer) {
	if (desc.info.metadata.kind != ImageMetadataKind::Dcc &&
	    desc.info.metadata.kind != ImageMetadataKind::Cmask) {
		return;
	}
	const auto range = desc.info.metadata.range;
	{
		std::scoped_lock lock {m_lock};
		auto& image         = m_slot_images[id];
		image.info.metadata = desc.info.metadata;
		// Native color metadata must not retain a reused HTile/CMask/FMask clear flag.
		m_surface_metas.erase(range.address);
		if (range.size == 0 || desc.info.resources.levels != 1) {
			return;
		}
	}
	const auto layers = desc.info.TransferLayers();
	// These one-mip surfaces use complete 4 KiB color metadata blocks.
	constexpr uint64_t MetadataBlockSize = 0x1000;
	if (!range.Valid() || range.address % MetadataBlockSize != 0 || layers == 0 ||
	    range.size % layers != 0 || (range.size / layers) % MetadataBlockSize != 0) {
		EXIT("TextureCache: color metadata slices must contain aligned 4 KiB blocks\n");
	}
	const auto& view           = desc.view_info;
	const bool  volume_texture = desc.info.IsVolume() && view.type == vk::ImageViewType::e3D;
	const auto  first          = volume_texture ? 0u : metadata_base_layer;
	const auto  image_first    = volume_texture ? 0u : view.base_layer;
	const auto  count          = volume_texture ? desc.info.extent.depth : view.layer_count;
	if (first >= layers || count > layers - first) {
		EXIT("TextureCache: color view exceeds its native metadata slices\n");
	}
	const auto slice_size = range.size / layers;
	if (Common::UniformMetadata::Enabled()) {
		// Preflight every slice before recording any clear or expansion. The
		// final unchanged generation is the logical metadata-read point, not a
		// lease through GPU completion; unordered guest writes remain races.
		std::array<uint8_t, Common::UniformMetadata::KnownRanges::Capacity> codes {};
		const auto generation = LibKernel::Memory::BackingMutationGeneration();
		bool known = count != 0 && count <= codes.size() && generation != 0 &&
		             (generation & 1) == 0;
		for (uint32_t slice = 0; known && slice < count; ++slice) {
			uint32_t value = 0;
			const auto address = range.address + slice_size * (first + slice);
			known = m_buffer_cache.TryGetUniformMetadata(address, slice_size, generation, &value) &&
			        value == (value & 0xffu) * 0x01010101u;
			codes[slice] = static_cast<uint8_t>(value);
		}
		known = known && LibKernel::Memory::BackingMutationGeneration() == generation;
		if (known) {
			Common::RenderTiming::Add(Common::RenderTiming::Counter::UniformMetadataHits);
			for (uint32_t slice = 0; slice < count; ++slice) {
				vk::ClearValue clear {};
				if (!DecodeColorClear(desc, codes[slice], clear.color)) continue;
				{
					std::scoped_lock lock {m_lock};
					const auto& image = m_slot_images[id];
					const bool depth = image.info.IsDepth();
					if (depth) clear.depthStencil = {clear.color.float32[0], 0u};
					ClearImage(m_scheduler.Current(), id, depth ? image.backing.format : view.format,
					           {depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
					            view.base_level, view.level_count, image_first + slice, 1}, clear);
				}
				Common::RenderTiming::Add(Common::RenderTiming::Counter::ColorClearMaterializations);
				if (desc.type == BindingType::VideoOut) {
					Common::RenderTiming::Add(Common::RenderTiming::Counter::VideoOutColorClearMaterializations);
				} else {
					// Preserve the real metadata write and CPU-fault/readback ownership.
					// Never use FillBuffer's immediate CPU publication or ClearMeta path.
					m_buffer_cache.FillUniformMetadataGpu(
					    range.address + slice_size * (first + slice), slice_size, 0xffffffffu, generation);
				}
			}
			return;
		}
		Common::RenderTiming::Add(Common::RenderTiming::Counter::UniformMetadataMisses);
	}
    m_scheduler.Context().PublishPhysicalAliasDependencies(range.address, range.size);
	// Finish native metadata writes before reading backing bytes. This can submit the scheduler,
	// so discovery runs before final draw uploads and never holds the texture lock across it.
	if (m_buffer_cache.IsRegionGpuModified(range.address, range.size)) {
		Common::RenderTiming::ScopedReadbackOrigin origin(
		    Common::RenderTiming::ReadbackOrigin::ColorMetadata);
		m_buffer_cache.ReadMemory(range.address, range.size, false);
	}
	for (uint32_t slice = 0; slice < count; slice++) {
		const auto address = range.address + slice_size * (first + slice);
		uint8_t code = 0;
		if (!LibKernel::Memory::TryReadBacking(address, &code, sizeof(code))) {
			EXIT("TextureCache: failed to read color metadata backing\n");
		}
		vk::ClearValue clear {};
		if (!DecodeColorClear(desc, code, clear.color)) {
			continue;
		}
		std::vector<uint8_t> bytes(slice_size);
		if (!LibKernel::Memory::TryReadBacking(address, bytes.data(), bytes.size())) {
			EXIT("TextureCache: failed to read color metadata slice\n");
		}
		if (!std::all_of(bytes.begin(), bytes.end(), [code](uint8_t byte) { return byte == code; })) {
			continue;
		}
		{
			std::scoped_lock lock {m_lock};
			ClearImage(m_scheduler.Current(), id, view.format,
			           {vk::ImageAspectFlagBits::eColor, view.base_level, view.level_count,
			            image_first + slice, 1}, clear);
		}
		Common::RenderTiming::Add(Common::RenderTiming::Counter::ColorClearMaterializations);
		if (desc.type == BindingType::VideoOut) {
			Common::RenderTiming::Add(Common::RenderTiming::Counter::VideoOutColorClearMaterializations);
		}
		// Publish the conversion's expanded keys without treating them as guest writes
		// to overlapping image data. Invalidate the buffer before updating its backing.
		if (desc.type != BindingType::VideoOut) {
			std::fill(bytes.begin(), bytes.end(), uint8_t {0xff});
			m_buffer_cache.InvalidateMemory(address, slice_size);
			LibKernel::Memory::WriteBacking(address, bytes.data(), bytes.size());
		}
	}
}

void TextureCache::RefreshImage(ImageId id) {
	auto& image = m_slot_images[id];
    if (image.info.data.Empty()) return;
	if (ReusePublicationSnapshot(id)) return;
    if (image.IsGpuModified() && !GuestGpuWriteOwnershipSupported())
        EXIT("PS5 GPU image ownership requires completed CPU backing publication; unavailable\n");
    if (!image.IsGpuModified())
        image.InvalidateCpuWrite(image.info.data.address, image.info.data.size);
	if (image.depth_id &&
	    (m_slot_images[image.depth_id].info.metadata.stencil_compressed ||
	     m_slot_images[image.depth_id].info.samples != 1)) {
		return;
	}
	TrackImage(id);
	if (image.IsMaybeCpuDirty()) {
		const auto hash = image.HashGuestEdges();
		if (image.NeedsMaybeCpuHash()) {
			image.SetMaybeCpuHash(hash);
			return;
		}
		(void)image.ResolveMaybeCpuHash(hash);
	}
	bool cpu_dirty = image.IsBufferModified() || image.IsDefinitelyCpuDirty();
	if (image.info.metadata.compression != VideoOutCompression::Uncompressed) {
		if (cpu_dirty) {
			EXIT("TextureCache: compressed guest image refresh is unsupported\n");
		}
		return;
	}
	if (!cpu_dirty) {
		return;
	}
	InitializeImage(id);
}

ImageId TextureCache::AssociateStencil(ImageId depth_id, GuestRange stencil) {
	if (!stencil.Valid()) {
		EXIT("TextureCache: invalid stencil association range\n");
	}
	auto& depth = m_slot_images[depth_id];
	if (!depth.info.IsDepth() || !depth.info.HasStencil()) {
		EXIT("TextureCache: stencil association requires a depth/stencil image\n");
	}

	ImageId association {};
	for (const auto id: FindImagesInRegion(stencil.address, stencil.size, false)) {
		const auto owner = m_slot_images.try_get(id);
		if (owner != nullptr && owner->info.data == stencil &&
		    owner->info.extent == depth.info.extent) {
			association = id;
		}
	}
	if (!association) {
		ImageInfo info {};
		info.data   = stencil;
		info.extent = depth.info.extent;
		association = InsertImage(info);
	}
	auto& record = m_slot_images[association];
	TouchImage(record);
	if (record.depth_id != depth_id || record.stencil_subresources != depth.stencil_subresources) {
		ImageInfo plane {};
		plane.data         = stencil;
		plane.pixel_format = vk::Format::eS8Uint;
		plane.guest_format = Prospero::BufferFormat::k8UInt;
		plane.type         = depth.info.type;
		plane.extent       = depth.info.extent;
		plane.resources    = {depth.stencil_subresources.level_count,
		                      depth.stencil_subresources.layer_count};
		plane.tile_mode    = depth.info.tile_mode;
		plane.pitch =
		    plane.IsTiled() ? TileGetDepthPitch(plane.extent.width, 1, 0) : depth.info.pitch;
		plane.bytes_per_block = 1;
		plane.UpdateSize();
		record.info                 = plane;
		record.depth_id             = depth_id;
		record.stencil_subresources = depth.stencil_subresources;
	}

	return association;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_format) {
    if (!desc.info.data.Empty())
        m_scheduler.Context().PublishPhysicalAliasDependencies(desc.info.data.address, desc.info.data.size);
    if (desc.info.HasStencil())
        m_scheduler.Context().PublishPhysicalAliasDependencies(desc.info.stencil.address, desc.info.stencil.size);
    if (!desc.info.data.Empty() && !GuestGpuWriteOwnershipSupported() &&
        (desc.type == BindingType::Storage || desc.type == BindingType::RenderTarget ||
         desc.type == BindingType::DepthTarget))
        EXIT("PS5 writable guest images require completed CPU backing publication; unavailable\n");
	auto& command = m_scheduler.Current();
	if (command.IsInvalid()) {
		EXIT("TextureCache: image lookup requires a valid command buffer\n");
	}
	ValidateImageDesc(desc);
	if (desc.info.data.Empty()) {
		std::scoped_lock lock {m_lock};
		return GetNullImage(desc);
	}
	const auto metadata_base_layer = desc.view_info.base_layer;

	ImageId result {};
	{
		std::scoped_lock lock {m_lock};
		const auto       candidates =
		    FindImagesInRegion(desc.info.data.address, desc.info.data.size, false);

		for (const auto id: candidates) {
			const auto& image = m_slot_images[id];
			if (SameBacking(image.info, desc.info, exact_format)) {
				result = id;
			}
		}

		int32_t view_mip   = -1;
		int32_t view_layer = -1;
		if (!result) {
			for (const auto candidate: candidates) {
				view_mip                = -1;
				view_layer              = -1;
				const auto& merged_info = result ? m_slot_images[result].info : desc.info;
				const auto  overlap     = ResolveOverlap(merged_info, desc.type, candidate, result);
				if (overlap.image) {
					result     = overlap.image;
					view_mip   = overlap.mip;
					view_layer = overlap.layer;
				}
			}
		}

		if (result) {
			auto& resolved = m_slot_images[result];
			if (exact_format && resolved.info.pixel_format != desc.info.pixel_format) {
				result = {};
			} else if (resolved.info.resources < desc.info.resources) {
				FreeImage(result);
				result = {};
			}
		}
		if (!result) {
			result         = InsertImage(desc.info);
			auto& inserted = m_slot_images[result];
			if (m_buffer_cache.HasGpuDirtyBytes(inserted.info.data.address,
			                                    inserted.info.data.size)) {
				inserted.MarkBufferModified();
			}
		}
		auto& image = m_slot_images[result];
		if (view_mip >= 0) {
			desc.view_info.base_level = static_cast<uint32_t>(view_mip);
		}
		if (view_layer >= 0) {
			desc.view_info.base_layer = static_cast<uint32_t>(view_layer);
		}
		image.tick_accessed_last = m_scheduler.CurrentTick();
		TouchImage(image);
	}
	MaterializeColorClear(result, desc, metadata_base_layer);
	if (desc.type == BindingType::VideoOut &&
	    desc.info.metadata.compression != VideoOutCompression::Uncompressed) {
		std::scoped_lock lock {m_lock};
		const auto& image = m_slot_images[result];
		const bool guest_dirty = image.IsBufferModified() || image.IsCpuDirty();
		const bool native_current =
		    (image.usage.render_target || image.IsGpuModified()) && !guest_dirty;
		if (!native_current) {
			EXIT("TextureCache: compressed video-out read requires clean native GPU "
			     "contents\n");
		}
	}
	return result;
}

void TextureCache::UpdateImage(ImageId id) {
    const auto info = m_slot_images[id].info;
    if (!info.data.Empty())
        m_scheduler.Context().PublishPhysicalAliasDependencies(info.data.address, info.data.size);
    if (info.HasStencil())
        m_scheduler.Context().PublishPhysicalAliasDependencies(info.stencil.address, info.stencil.size);
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	TouchImage(image);
	RefreshImage(id);
}

ImageId TextureCache::FindImageFromRange(uint64_t address, uint64_t size, bool ensure_valid) {
	if (!GuestRange {address, size}.Valid()) {
		return {};
	}
    m_scheduler.Context().PublishPhysicalAliasDependencies(address, size);
	std::scoped_lock lock {m_lock};
	ImageIds         matches;
	for (const auto id: FindImagesInRegion(address, size, false)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr || !owner->registered || owner->info.data.address > address ||
		    address >= owner->info.data.End() || size > owner->info.data.End() - address) {
			continue;
		}
		if (owner == nullptr || (ensure_valid && !owner->SafeToDownload())) {
			continue;
		}
		matches.push_back(id);
	}
	ImageId selected {};
	if (matches.size() == 1) {
		selected = matches.front();
	} else if (!ensure_valid) {
		for (const auto id: matches) {
			const auto& image = m_slot_images[id];
			if (image.info.data.size == size) {
				selected = id;
				break;
			}
		}
	}
	return selected;
}

vk::ImageView TextureCache::FindTexture(ImageId id, const ImageDesc& desc) {
    if (desc.type == BindingType::Storage && !GuestGpuWriteOwnershipSupported())
        EXIT("PS5 storage guest images require completed CPU backing publication; unavailable\n");
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	TouchImage(image);
	if (!image.info.data.Empty()) {
		if (!image.registered || image.depth_id || image.binding.needs_rebind) {
			EXIT("TextureCache: texture requires rediscovery before final acquisition\n");
		}
	}
	if (desc.type == BindingType::Storage) {
		ValidateGpuWrite(image);
	}
	if (!image.info.data.Empty()) {
		RefreshImage(id);
		if (image.info.HasStencil() &&
		    desc.info.data.address >= image.info.stencil.address &&
		    desc.info.data.End() <= image.info.stencil.End()) {
			for (const auto stencil_id:
			     FindImagesInRegion(image.info.stencil.address, image.info.stencil.size, false)) {
				const auto* stencil = m_slot_images.try_get(stencil_id);
				if (stencil != nullptr && stencil->depth_id == id &&
				    stencil->info.data == image.info.stencil) {
					RefreshImage(stencil_id);
					break;
				}
			}
		}
	}
	switch (desc.type) {
		case BindingType::Texture: break;
		case BindingType::Storage:
			if (!image.info.data.Empty()) {
				CommitGpuWrite(image);
			}
			TrackImageDownload(id, image);
			TraceImageWriteCapableBinding(id, image, desc.view_info, "storage");
			break;
		default: EXIT("TextureCache: invalid texture binding\n");
	}
	return image.FindView(desc.view_info);
}

vk::ImageView TextureCache::FindRenderTarget(ImageId id, const ImageDesc& desc) {
    if (!GuestGpuWriteOwnershipSupported())
        EXIT("PS5 guest color-target writes require completed CPU backing publication; unavailable\n");
	if (desc.type != BindingType::RenderTarget) {
		EXIT("TextureCache: invalid color-target binding\n");
	}
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id || image.binding.needs_rebind) {
		EXIT("TextureCache: color target requires rediscovery before final acquisition\n");
	}
	TouchImage(image);
	image.usage.render_target = true;
	ValidateGpuWrite(image);
	RefreshImage(id);
	CommitGpuWrite(image);
	TrackImageDownload(id, image);
	TraceImageWriteCapableBinding(id, image, desc.view_info, "color_target");
	return image.FindView(desc.view_info);
}

vk::ImageView TextureCache::FindDepthTarget(ImageId id, const ImageDesc& desc) {
    if (!GuestGpuWriteOwnershipSupported())
        EXIT("PS5 guest depth-target writes require completed CPU backing publication; unavailable\n");
	if (desc.type != BindingType::DepthTarget) {
		EXIT("TextureCache: invalid depth-target binding\n");
	}
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id || image.binding.needs_rebind) {
		EXIT("TextureCache: depth target requires rediscovery before final acquisition\n");
	}
	if (image.info.stencil != desc.info.stencil)
		EXIT("TextureCache: stencil backing change requires depth-target rediscovery\n");
	TouchImage(image);
	image.usage.depth_target = true;
	if (desc.info.HasStencil()) {
		const auto layers = image.info.resources.layers;
		EXIT_IF(layers == 0 || image.info.data.size % layers != 0 ||
		        desc.info.data.address < image.info.data.address);
		const auto slice_size = image.info.data.size / layers;
		const auto offset     = desc.info.data.address - image.info.data.address;
		EXIT_IF(slice_size == 0 || offset % slice_size != 0 || offset / slice_size >= layers ||
		        desc.info.resources.layers > layers - offset / slice_size);
		image.stencil_subresources = {0, desc.info.resources.levels,
		                             static_cast<uint32_t>(offset / slice_size),
		                             desc.info.resources.layers};
	}
	image.info.stencil = desc.info.stencil;
	image.info.metadata = desc.info.metadata;
	ValidateGpuWrite(image);
	if (desc.info.HasMetadata()) {
		m_surface_metas.emplace(desc.info.metadata.range.address,
		                        MetaDataInfo {.type       = MetaDataInfo::Type::HTile,
		                                      .clear_mask = image.info.htile_clear_mask});
	}
	RefreshImage(id);
	if (desc.info.HasStencil()) {
		const auto stencil_id = AssociateStencil(id, desc.info.stencil);
		RefreshImage(stencil_id);
		TrackImage(stencil_id);
		CommitGpuWrite(m_slot_images[stencil_id]);
	}
	CommitGpuWrite(image);
	return image.FindView(desc.view_info);
}

void TextureCache::TraceDepthAttachmentBinding(ImageId id, const ImageViewInfo& view,
                                               vk::ImageAspectFlags guest_write_aspects) {
	if (GpuSliceTrace::current == nullptr) return;
	std::scoped_lock lock {m_lock};
	const auto* image = m_slot_images.try_get(id);
	if (image == nullptr || !image->registered) return;
	TraceImageWriteCapableBinding(id, *image, view, "depth_target", static_cast<uint32_t>(guest_write_aspects));
}

void TextureCache::MarkGpuWritten(ImageId id) {
	std::scoped_lock lock {m_lock};
	auto&            image = m_slot_images[id];
	if (!image.registered || image.depth_id) {
		EXIT("TextureCache: cannot mark an unavailable image GPU-written\n");
	}
	ValidateGpuWrite(image);
	TrackImage(id);
	CommitGpuWrite(image);
	if (image.info.HasStencil()) {
		const auto stencil_id = AssociateStencil(id, image.info.stencil);
		TrackImage(stencil_id);
		CommitGpuWrite(m_slot_images[stencil_id]);
	}
}

void TextureCache::CommitGpuWrite(Image& image) {
	m_buffer_cache.InvalidateUniformMetadata(image.info.data.address, image.info.data.size);
    if (!image.info.data.Empty() && !GuestGpuWriteOwnershipSupported())
        EXIT("PS5 GPU guest-image writes require completed CPU backing publication; unavailable\n");
	if (!image.depth_id && image.backing.image == nullptr) {
		EXIT("TextureCache: GPU writes require a native image or stencil association\n");
	}
	ValidateGpuWrite(image);
	image.ClearBufferModified();
	if (image.IsCpuDirty()) {
		image.RefreshComplete();
	}
	image.MarkGpuModified();
}

void TextureCache::ValidateGpuWrite(const Image& image) const {
	if (image.info.data.Empty()) return;
	if (!GuestGpuWriteOwnershipSupported() ||
	    !LibKernel::Memory::QueryGpuBackingWrite(image.info.data.address, image.info.data.size))
		EXIT("PS5 GPU image destination lacks exclusive shared GPU-write backing\n");
	const auto* owner = image.depth_id ? m_slot_images.try_get(image.depth_id) : &image;
	// Validate both complete planes before the first native write, even for a
	// one-aspect clear. A stencil association borrows the parent's native image.
	const bool stencil_backing = owner != nullptr &&
	    (!owner->info.HasStencil() ||
	     (LibKernel::Memory::QueryGpuBackingWrite(owner->info.data.address, owner->info.data.size) &&
	      LibKernel::Memory::QueryGpuBackingWrite(owner->info.stencil.address, owner->info.stencil.size)));
	ImageInfo plane;
	if (!stencil_backing || !BuildDownload(image).valid ||
	    (owner != nullptr && owner->info.HasStencil() && !BuildDepthPlane(image, plane))) {
		const auto& info = image.info;
		EXIT("PS5 GPU image destination has no complete backing readback: "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
		     " format=%u tile=%u extent=%ux%ux%u pitch=%u "
		     "levels=%u layers=%u samples=%u native_samples=%u depth=%u stencil=%u stencil_owner=%u "
		     "host_format=%u stencil_addr=0x%016" PRIx64 " stencil_size=0x%016" PRIx64
		     " metadata=%u stencil_compressed=%u\n",
		     info.data.address, info.data.size, static_cast<uint32_t>(info.guest_format),
		     static_cast<uint32_t>(info.tile_mode), info.extent.width, info.extent.height,
		     info.extent.depth, info.pitch, info.resources.levels, info.resources.layers,
		     info.samples, image.backing.samples, info.IsDepth() ? 1u : 0u,
		     info.HasStencil() ? 1u : 0u, image.depth_id ? 1u : 0u,
		     static_cast<uint32_t>(info.pixel_format), info.stencil.address, info.stencil.size,
		     static_cast<uint32_t>(info.metadata.kind), info.metadata.stencil_compressed ? 1u : 0u);
	}
}

bool TextureCache::ClearImageFromBuffer(CommandBuffer& command, uint64_t address, uint64_t size,
                                        uint32_t packed_clear) {
	if (command.IsInvalid() || !GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid image clear\n");
	}
    m_scheduler.Context().PublishPhysicalAliasDependencies(address, size);
	std::scoped_lock      lock {m_lock};
	ImageId               selected {};
	ImageId               stencil_id {};
	vk::ImageAspectFlags  aspect {};
	ImageSubresourceRange subresources;
	for (const auto id: FindImagesInRegion(address, size, false)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		vk::ImageAspectFlags candidate {};
		ImageId              candidate_id = id;
		ImageSubresourceRange candidate_subresources {
		    0, owner->info.resources.levels, 0, owner->info.TransferLayers()};
		if (owner->depth_id && owner->info.data.address == address &&
		    owner->info.data.size == size) {
			candidate              = vk::ImageAspectFlagBits::eStencil;
			candidate_id           = owner->depth_id;
			candidate_subresources = owner->stencil_subresources;
			owner        = m_slot_images.try_get(candidate_id);
			if (owner == nullptr || owner->backing.image == nullptr || !owner->info.HasStencil()) {
				continue;
			}
		} else if (!owner->depth_id && owner->info.data.address == address &&
		           owner->info.data.size == size) {
			candidate = owner->info.IsDepth() ? vk::ImageAspectFlagBits::eDepth
			                                  : vk::ImageAspectFlagBits::eColor;
		}
		if (!candidate) {
			continue;
		}
		if (selected && (selected != candidate_id || subresources != candidate_subresources)) {
			return false;
		}
		selected     = candidate_id;
		stencil_id   = candidate == vk::ImageAspectFlagBits::eStencil ? id : ImageId {};
		aspect       = candidate;
		subresources = candidate_subresources;
	}
	if (!selected) {
		return false;
	}
	auto&          image = m_slot_images[selected];
	vk::ClearValue clear {};
	if (aspect == vk::ImageAspectFlagBits::eColor) {
		if (!DecodeColorDwordFill(image.info.pixel_format, packed_clear, clear.color)) {
			return false;
		}
	} else {
		uint8_t stencil_clear = 0;
		if ((aspect == vk::ImageAspectFlagBits::eDepth &&
		     !DecodePackedDepthClear(image.info.pixel_format, packed_clear, clear.depthStencil.depth)) ||
		    (aspect == vk::ImageAspectFlagBits::eStencil &&
		     !DecodePackedStencilClear(packed_clear, stencil_clear))) {
			return false;
		}
		clear.depthStencil.stencil = stencil_clear;
	}
	ClearImage(command, selected, image.backing.format,
	           {aspect, subresources.base_level, subresources.level_count,
	            subresources.base_layer, subresources.layer_count}, clear);
	if (stencil_id) {
		TrackImage(stencil_id);
		CommitGpuWrite(m_slot_images[stencil_id]);
	}
	return true;
}

void TextureCache::ClearImage(CommandBuffer& command, ImageId id, vk::Format format,
                              const vk::ImageSubresourceRange& range, const vk::ClearValue& clear) {
	auto& image = m_slot_images[id];
    if (!image.info.data.Empty() && !GuestGpuWriteOwnershipSupported())
        EXIT("PS5 GPU guest-image clears require completed CPU backing publication; unavailable\n");
	ValidateGpuWrite(image);
	const auto aspects = image.info.IsDepth() ? ImageViewOps::DepthAspectMask(image.backing.format)
	                                          : vk::ImageAspectFlagBits::eColor;
	EXIT_IF(range.baseMipLevel >= image.info.resources.levels);
	const auto layers = image.info.IsVolume()
	                        ? std::max(image.info.extent.depth >> range.baseMipLevel, 1u)
	                        : image.backing.layers;
	EXIT_IF(command.IsInvalid() || image.depth_id || !range.aspectMask || range.levelCount == 0 ||
	        range.levelCount > image.info.resources.levels - range.baseMipLevel ||
	        range.layerCount == 0 || range.baseArrayLayer >= layers ||
	        range.layerCount > layers - range.baseArrayLayer ||
	        (range.aspectMask & aspects) != range.aspectMask);
	const bool full_subresources = range.baseMipLevel == 0 &&
	                               range.levelCount == image.info.resources.levels &&
	                               range.baseArrayLayer == 0 && range.layerCount == layers;
	const bool full_image = range.aspectMask == aspects && full_subresources;
	TrackImage(id);
	if (!full_image && (image.IsBufferModified() || image.IsCpuDirty())) {
		InitializeImage(id);
		if (image.info.samples == 1 && (image.IsBufferModified() || image.IsCpuDirty())) {
			EXIT("TextureCache: image clear retained guest ownership\n");
		}
	}
	if (image.info.HasStencil() && (range.aspectMask & vk::ImageAspectFlagBits::eStencil)) {
		const auto stencil_id = AssociateStencil(id, image.info.stencil);
		if (!full_subresources) {
			RefreshImage(stencil_id);
		}
		TrackImage(stencil_id);
		CommitGpuWrite(m_slot_images[stencil_id]);
	}
	command.EndRendering();
	// Transfer clears use the backing format; aliased clears must encode through their view.
	if (format != image.backing.format || (image.info.IsVolume() && !full_image)) {
		EXIT_NOT_IMPLEMENTED(range.aspectMask != vk::ImageAspectFlagBits::eColor ||
		                     range.levelCount != 1);
		ImageViewInfo view {};
		view.format = format;
		view.type   = range.layerCount == 1 ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray;
		view.base_level  = range.baseMipLevel;
		view.base_layer  = range.baseArrayLayer;
		view.layer_count = range.layerCount;
		view.usage       = vk::ImageUsageFlagBits::eColorAttachment;
		image.Transit(vk::ImageLayout::eColorAttachmentOptimal,
		              vk::AccessFlagBits2::eColorAttachmentWrite, {}, command.Handle());
		vk::RenderingAttachmentInfo attachment {};
		attachment.imageView   = image.FindView(view);
		attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
		attachment.loadOp      = vk::AttachmentLoadOp::eClear;
		attachment.storeOp     = vk::AttachmentStoreOp::eStore;
		attachment.clearValue  = clear;
		vk::RenderingInfo rendering {};
		rendering.renderArea.extent = {
		    std::max(image.info.extent.width >> range.baseMipLevel, 1u),
		    std::max(image.info.extent.height >> range.baseMipLevel, 1u)};
		rendering.layerCount           = range.layerCount;
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachments    = &attachment;
		command.Handle().beginRendering(&rendering);
		command.Handle().endRendering();
		CommitGpuWrite(image);
		return;
	}
	image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {},
	              command.Handle());
	auto native_range = range;
	if (image.info.IsVolume()) {
		native_range.baseArrayLayer = 0;
		native_range.layerCount     = 1;
	}
	if (range.aspectMask == vk::ImageAspectFlagBits::eColor) {
		command.Handle().clearColorImage(image.backing.image, vk::ImageLayout::eTransferDstOptimal,
		                                 &clear.color, 1, &native_range);
	} else {
		command.Handle().clearDepthStencilImage(image.backing.image,
		                                        vk::ImageLayout::eTransferDstOptimal,
		                                        &clear.depthStencil, 1, &native_range);
	}
	if (!image.info.IsDepth() || (range.aspectMask & vk::ImageAspectFlagBits::eDepth))
		CommitGpuWrite(image);
}

void TextureCache::InvalidateMemory(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid memory-invalidation range\n");
	}
	std::scoped_lock lock {m_lock};
	InvalidateCpuAliases(address, size);
}

void TextureCache::DownloadDepth(Image& image, Buffer& destination, uint64_t destination_offset) {
	ImageInfo info;
	EXIT_NOT_IMPLEMENTED(!BuildDepthPlane(image, info));
	auto& native = image.depth_id ? m_slot_images[image.depth_id] : image;
	const auto     layers           = info.resources.layers;
	const auto     full_slice_size  = info.linear_slice_stride;
	const auto     transfer_bytes   = image.depth_id ? 1u : DepthAspectTransferBytes(info.pixel_format);
	const uint64_t texels_per_slice = static_cast<uint64_t>(info.pitch) * info.extent.height;
	EXIT_NOT_IMPLEMENTED(transfer_bytes == 0 || texels_per_slice > UINT32_MAX ||
	                     texels_per_slice > UINT64_MAX / transfer_bytes ||
	                     texels_per_slice > UINT64_MAX / info.bytes_per_block);
	const uint64_t transfer_slice = texels_per_slice * transfer_bytes;
	const uint64_t guest_slice    = texels_per_slice * info.bytes_per_block;
	EXIT_NOT_IMPLEMENTED(transfer_slice > UINT64_MAX / layers);
	const uint64_t transfer_size = transfer_slice * layers;
	EXIT_NOT_IMPLEMENTED(guest_slice > full_slice_size);
	auto copies = BuildDepthCopies(info, full_slice_size, image.depth_id
	                                                        ? vk::ImageAspectFlagBits::eStencil
	                                                        : vk::ImageAspectFlagBits::eDepth);
	EXIT_NOT_IMPLEMENTED((destination_offset & 3u) != 0);
	if (transfer_bytes == info.bytes_per_block) {
		if (!info.IsTiled()) {
			for (auto& copy: copies) {
				copy.bufferOffset += destination_offset;
			}
			native.Download(copies, destination.Handle(), destination_offset, info.data.size);
			return;
		}
		m_tiler.TileImage(native, copies, destination.Handle(), destination_offset, info.data.size, info);
		return;
	}
	EXIT_NOT_IMPLEMENTED(info.bytes_per_block != sizeof(uint16_t) ||
	                     transfer_bytes != sizeof(uint32_t));
	for (uint32_t layer = 0; layer < layers; layer++) {
		copies[layer].bufferOffset = transfer_slice * layer;
	}
	auto host_linear = m_tiler.GetScratchBuffer(transfer_size);
	native.Download(copies, host_linear.buffer, 0, host_linear.size);
	const bool tiled        = info.IsTiled();
	auto       guest_linear = tiled ? m_tiler.GetScratchBuffer(info.data.size, host_linear.buffer)
	                                : TileManager::Result {destination.Handle(), destination_offset,
	                                                       destination.Size() - destination_offset};
	m_tiler.ConvertD16(host_linear, guest_linear, TileManager::D16Direction::Demote,
	                   DepthAspectTransferFormat(info.pixel_format) == vk::Format::eD32Sfloat,
	                   {.width               = info.extent.width,
	                    .height              = info.extent.height,
	                    .layers              = layers,
	                    .source_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint32_t),
	                    .target_row_stride   = static_cast<uint64_t>(info.pitch) * sizeof(uint16_t),
	                    .source_slice_stride = transfer_slice,
	                    .target_slice_stride = full_slice_size});
	if (!tiled) {
		return;
	}
	m_tiler.Tile(guest_linear.buffer, guest_linear.offset, info.LinearSize(), destination.Handle(),
	             destination_offset, info.data.size, info);
}

void TextureCache::DownloadImage(Image& image, Buffer& destination, uint64_t destination_offset,
                                     uint64_t destination_size, ImageDownload transfer) {
	if (!transfer.valid) {
		EXIT("TextureCache: invalid image download transfer\n");
	}
	if (transfer.depth_target) {
		if (destination_size != image.info.data.size) {
			EXIT("TextureCache: partial depth image download is unsupported\n");
		}
		DownloadDepth(image, destination, destination_offset);
		return;
	}

	auto&      texture   = transfer.texture;
	const auto transform = texture.color_transform;
	if (!image.info.IsTiled()) {
		if (transform != ColorTransform::None) {
			auto linear = m_tiler.GetScratchBuffer(destination_size);
			image.Download(texture.regions, linear.buffer, 0, linear.size);
			m_tiler.TransformColor(linear,
			                       {destination.Handle(), destination_offset, destination_size},
			                       transform, false);
			return;
		}
		for (auto& copy: texture.regions) {
			copy.bufferOffset += destination_offset;
		}
		image.Download(texture.regions, destination.Handle(), destination_offset, destination_size);
		return;
	}

	m_tiler.TileImage(image, texture.regions, destination.Handle(), destination_offset,
	                  destination_size, texture.levels);
}

bool BufferCache::SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	const auto selected = m_texture_cache.FindImageFromRange(vaddr, size);
	if (!selected) {
		return false;
	}

	std::scoped_lock lock {m_texture_cache.m_lock};
	auto& image = m_texture_cache.m_slot_images[selected];
	// The GPU thread owns image retirement; CPU invalidation can dirty this image after lookup.
	if (!image.SafeToDownload()) {
		return false;
	}
	if (!buffer.IsInBounds(image.info.data.address, 1)) {
		return false;
	}
	const auto buf_offset = buffer.Offset(image.info.data.address);
	const auto available  = buffer.Size() - buf_offset;
	uint32_t   levels     = 0;
	uint64_t   copy_size  = 0;
	if (buffer.IsInBounds(image.info.data.address, image.info.data.size)) {
		// A stencil placeholder has no mip layout; its actual geometry belongs to
		// the depth owner. Both stencil and volume synchronization require the
		// complete guest surface, including padding, inside this buffer.
		if (!buffer.IsInBounds(image.info.data.address, image.info.data.size)) {
			return false;
		}
		levels    = image.info.resources.levels;
		copy_size = image.info.data.size;
	} else if (image.info.IsVolume() || image.info.resources.layers > 1) {
		// Array and volume slices are strided across the full mip chain.
		return false;
	} else {
		for (; levels < image.info.resources.levels; ++levels) {
			const auto& mip = image.info.mip_layout[levels];
			if (mip.size == 0 || mip.offset > available || mip.size > available - mip.offset) {
				break;
			}
			copy_size = std::max(copy_size, mip.offset + mip.size);
		}
	}
	if (copy_size == 0) {
		return false;
	}
	auto transfer = m_texture_cache.BuildDownload(image);
	if (!transfer.valid) {
		return false;
	}
	if (transfer.depth_target && copy_size != image.info.data.size) {
		return false;
	}
	if (!transfer.depth_target && levels < image.info.resources.levels) {
		auto& texture = transfer.texture;
		std::erase_if(texture.regions, [levels](const vk::BufferImageCopy& region) {
			return region.imageSubresource.mipLevel >= levels;
		});
		if (texture.regions.empty()) {
			return false;
		}
		texture.levels = levels;
		texture.linear_size = image.info.LinearSize(levels);
		texture.tiles = BuildPublicationTiles(image.info, levels);
	}
	m_texture_cache.DownloadImage(image, buffer, buf_offset, copy_size, std::move(transfer));
	return true;
}

bool TextureCache::HasDisjointHostImportBacking(ImageId id) {
	if (!m_graphics.external_memory_host_enabled || !GuestGpuWriteOwnershipSupported() ||
	    GuestAsyncCpuConcurrentExperimentEnabled() || ImagePublicationSnapshotEnabled() ||
	    Config::GraphicsDebugDumpEnabled()) return HostImportDenied(1, "policy");
	const auto* image = m_slot_images.try_get(id);
	if (image == nullptr || !image->registered || !image->IsGpuModified() ||
	    !image->SafeToDownload() || !image->info.data.Valid() ||
	    ((image->info.data.address | image->info.data.size) & 3u) != 0)
		return HostImportDenied(2, "image_owner");
	// Direct GPU stores must never become visible to another dirty native owner
	// through a different VA. Check the complete physical alias closure, not just
	// the image's source address. The final VM borrow proves the aligned envelope.
	std::vector<LibKernel::Memory::GuestBackingAlias> aliases;
	if (!LibKernel::Memory::QueryGuestBackingAliases(image->info.data.address,
	        image->info.data.size, &aliases)) return HostImportDenied(4, "physical_alias_query");
	for (const auto& alias: aliases) {
		if (m_buffer_cache.IsRegionGpuModified(alias.address, alias.size) ||
		    m_buffer_cache.HasGpuDirtyBytes(alias.address, alias.size))
			return HostImportDenied(8, "physical_dirty_buffer");
		bool conflict = false;
		m_slot_images.ForEach([&](ImageId other_id, const Image& other) {
			if (other_id != id && other.registered && other.IsGpuModified() &&
			    other.Overlaps(alias.address, alias.size)) conflict = true;
		});
		if (conflict) return HostImportDenied(16, "physical_dirty_image");
	}
	return true;
}

size_t TextureCache::TryDownloadImageHostBatch(
    std::span<const std::pair<ImageId, GuestRange>> candidates) {
	if (candidates.empty()) return 0;
	constexpr size_t import_limit = 64;
	struct ExpectedOwner {
		ImageId id;
		GuestRange range;
		ImageInfo info;
		vk::Image native;
		BindingType binding;
	};
	std::vector<ExpectedOwner> expected;
	expected.reserve(std::min(candidates.size(), import_limit));
	for (size_t i = 0; i < std::min(candidates.size(), import_limit); ++i) {
		const auto& [id, range] = candidates[i];
		const auto& image = m_slot_images[id];
		expected.push_back({id, range, image.info, image.backing.image, UploadBinding(image)});
	}
	auto* trace = GpuSliceTrace::current;
	// The entire preceding staged prefix and every old priority/ordinary
	// callback complete before the first borrow. No callbacks run with borrows.
	struct UnlockForCompletion {
		TrackingSpinLock& lock;
		explicit UnlockForCompletion(TrackingSpinLock& held): lock(held) { lock.unlock(); }
		~UnlockForCompletion() { lock.lock(); }
	};
	const auto drain_start = trace != nullptr ? GpuSliceTrace::Now() : 0;
	{
		UnlockForCompletion unlocked(m_lock);
		const auto tick = m_scheduler.CurrentTick();
		m_scheduler.Finish();
		m_scheduler.WaitPriorityOperations(tick);
	}
	if (trace != nullptr) trace->image_host_import_drain_ns += GpuSliceTrace::Now() - drain_start;
	if (m_scheduler.HasPendingOperations()) {
		(void)HostImportDenied(32, "pending_callback");
		return 0;
	}
	struct ImportTiming {
		GpuSliceTrace* trace;
		uint64_t start;
		~ImportTiming() { if (trace != nullptr) trace->image_host_import_ns += GpuSliceTrace::Now() - start; }
	} timing {trace, trace != nullptr ? GpuSliceTrace::Now() : 0};
	struct PreparedImport {
		// Destruction order is deliberate: Vulkan buffer/memory before VM borrow.
		LibKernel::Memory::GuestGpuBackingImport backing;
		std::unique_ptr<Buffer> buffer;
		ImageId id;
		GuestRange range;
		ImageDownload transfer;
	};
	std::vector<PreparedImport> prepared;
	prepared.reserve(expected.size());
	for (const auto& owner: expected) {
		if (trace != nullptr) {
			if (!prepared.empty()) ++trace->image_host_import_considered;
			++trace->image_host_import_attempts;
		}
		const auto* image = m_slot_images.try_get(owner.id);
		if (image == nullptr || !image->registered || image->info.data != owner.range ||
		    image->backing.image != owner.native || !SamePublicationLayout(image->info, owner.info) ||
		    UploadBinding(*image) != owner.binding || !image->SafeToDownload())
			EXIT("PS5 GPU image owner changed during host import batch drain\n");
		if (!HasDisjointHostImportBacking(owner.id)) {
			(void)HostImportDenied(2, "owner_after_drain", true);
			break;
		}
		PreparedImport item;
		item.id = owner.id;
		item.range = owner.range;
		item.transfer = BuildDownload(*image);
		if (!item.transfer.valid) {
			(void)HostImportDenied(2, "download_plan", true);
			break;
		}
		if (!LibKernel::Memory::TryAcquireGuestGpuBackingImport(owner.range.address, owner.range.size,
		        m_graphics.min_imported_host_pointer_alignment, &item.backing)) {
			(void)HostImportDenied(64, "vm_borrow", true);
			break;
		}
		const auto offset = item.backing.GuestOffset();
		const auto copy_alignment = std::max<uint64_t>(image->info.bytes_per_block, 4);
		if (offset % copy_alignment != 0 || offset > item.backing.ByteSize() ||
		    owner.range.size > item.backing.ByteSize() - offset) {
			(void)HostImportDenied(128, "copy_alignment", true);
			break;
		}
		const char* failure = nullptr;
		item.buffer = Buffer::TryImportHostMemory(m_graphics, m_scheduler,
		    item.backing.HostPointer(), item.backing.ByteSize(), &failure);
		if (!item.buffer) {
			(void)HostImportDenied(256, failure, true);
			break;
		}
		prepared.push_back(std::move(item));
	}
	if (prepared.empty()) return 0;
	const bool cap_stop = prepared.size() == import_limit && candidates.size() > import_limit;
	uint64_t imported_bytes = 0;
	// All fallible imports are resolved before the first command. A denied
	// owner ends this contiguous batch; its staging starts only after ALL
	// native work, Vulkan destruction and borrow release below have completed.
	for (auto& item: prepared) {
		TraceImagePublication(item.id, item.transfer);
		const auto offset = item.backing.GuestOffset();
		vk::BufferMemoryBarrier barrier {};
		barrier.srcAccessMask = vk::AccessFlagBits::eHostWrite;
		barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite |
		                        vk::AccessFlagBits::eTransferWrite;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.buffer = item.buffer->Handle();
		barrier.offset = offset;
		barrier.size = item.range.size;
		m_scheduler.EndRendering();
		m_scheduler.Current().Handle().pipelineBarrier(vk::PipelineStageFlagBits::eHost,
		    vk::PipelineStageFlagBits::eAllCommands, {}, 0, nullptr, 1, &barrier, 0, nullptr);
		DownloadImage(m_slot_images[item.id], *item.buffer, offset, item.range.size, std::move(item.transfer));
		barrier.srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eTransferWrite |
		                        vk::AccessFlagBits::eShaderWrite;
		barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
		m_scheduler.EndRendering();
		m_scheduler.Current().Handle().pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		    vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1, &barrier, 0, nullptr);
		imported_bytes += item.range.size;
	}
	// Tiler stream wrap may submit/wait, but does not drain ordinary callbacks.
	// DownloadImage/tiler enqueue only ordinary resource retirement. The final
	// tick covers every earlier submit on this queue. No staged priority
	// copyout is admitted while any of these imports remains live.
	auto ticket = m_scheduler.PrepareCompletion();
	m_scheduler.WaitNativeCompletion(ticket);
	const auto imported_owners = prepared.size();
	prepared.clear();
	{
		UnlockForCompletion unlocked(m_lock);
		m_scheduler.CompleteDeferredCompletion(std::move(ticket));
	}
	m_host_import_bytes += imported_bytes;
	++m_host_import_batches;
	m_host_import_batch_owners += imported_owners;
	m_host_import_batch_cap_stops += cap_stop ? 1u : 0u;
	if (PublicationTraceEnabled()) m_publication_stats.prefill_saved += imported_bytes;
	static std::atomic<uint32_t> import_receipts {0};
	if (import_receipts.fetch_add(1, std::memory_order_relaxed) < 16)
		std::fprintf(stderr, "PS5_GPU_HOST_IMPORT_V1 outcome=imported owner_bytes=%llu owners=%zu cap_stop=%u cpu_seed_bytes=0 cpu_copyout_bytes=0 completion=native_before_release\n",
		    static_cast<unsigned long long>(imported_bytes), imported_owners, cap_stop ? 1u : 0u);
	if (trace != nullptr) {
		trace->image_readback_bytes += imported_bytes;
		trace->image_host_import_bytes += imported_bytes;
		++trace->image_host_import_batches;
		trace->image_host_import_batch_owners += imported_owners;
		trace->image_host_import_batch_cap_stops += cap_stop ? 1u : 0u;
	}
	return imported_owners;
}

bool TextureCache::DownloadImageMemory(ImageId id,
                                      std::span<uint8_t> publication) {
	auto transfer = BuildDownload(m_slot_images[id]);
	if (!transfer.valid || !m_slot_images[id].SafeToDownload()) {
		return false;
	}
	TraceImagePublication(id, transfer);
	auto& image = m_slot_images[id];
	const auto range    = image.info.data;
	auto&      download = m_buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
	auto [mapped, offset] =
	    download.Map(range.size, std::max<uint64_t>(image.info.bytes_per_block, 4));
	if (mapped == nullptr) {
		EXIT("TextureCache: failed to map reusable download buffer\n");
	}
	download.Commit();
	const bool full_overwrite = ImagePublicationSnapshotEnabled() && GuestGpuWriteOwnershipSupported() &&
	    !GuestAsyncCpuConcurrentExperimentEnabled() && DownloadFullyOverwrites(image, transfer);
	std::vector<GuestRange> overwritten;
	const bool block_seed = !full_overwrite && ImagePublicationBlockSeedEnabled() &&
	    GuestGpuWriteOwnershipSupported() && !GuestAsyncCpuConcurrentExperimentEnabled() &&
	    BuildPublicationOverwriteBlocks(image, transfer, overwritten);
	uint64_t seeded = 0;
	if (!full_overwrite) {
		if (block_seed) {
			// Validate the complete backing and its physical aliases even though
			// only padding-bearing spans are read. The exclusive epoch remains
			// held through download, full copyout, and ownership retirement.
			if (!LibKernel::Memory::QueryGpuBackingWrite(range.address, range.size)) return false;
			auto cursor = range.address;
			for (const auto& complete: overwritten) {
				const auto bytes = complete.address - cursor;
				if (bytes != 0 && !LibKernel::Memory::TryReadBacking(cursor,
				        mapped + (cursor - range.address), bytes)) return false;
				seeded += bytes;
				cursor = complete.End();
			}
			const auto bytes = range.End() - cursor;
			if (bytes != 0 && !LibKernel::Memory::TryReadBacking(cursor,
			        mapped + (cursor - range.address), bytes)) return false;
			seeded += bytes;
		} else {
			if (!LibKernel::Memory::TryReadBacking(range.address, mapped, range.size)) return false;
			seeded = range.size;
		}
		download.Flush(offset, range.size);
	}
	if (PublicationTraceEnabled()) {
		m_publication_stats.prefill_saved += range.size - seeded;
		m_publication_stats.prefill_preserved += seeded;
		if (block_seed) {
			m_publication_stats.block_seed_saved += range.size - seeded;
			m_publication_stats.block_seed_preserved += seeded;
			++m_publication_stats.block_seed_plans;
		}
	}

	if (GpuSliceTrace::current != nullptr) {
		GpuSliceTrace::current->image_readback_bytes += range.size;
		GpuSliceTrace::current->image_seed_bytes += seeded;
		GpuSliceTrace::current->image_cpu_copyout_bytes += range.size;
	}
	DownloadImage(image, download, offset, range.size, std::move(transfer));
	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eTransferWrite |
	                        vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = download.Handle();
	barrier.offset              = offset;
	barrier.size                = range.size;
	m_scheduler.EndRendering();
	m_scheduler.Current().Handle().pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                                               vk::PipelineStageFlagBits::eHost, {}, 0, nullptr,
	                                               1, &barrier, 0, nullptr);
	const auto format = image.info.pixel_format;
	m_scheduler.DeferPriorityOperation([&download, range, mapped, offset, format, publication] {
		download.Invalidate(offset, range.size);
		LibKernel::Memory::WriteBacking(range.address, mapped, range.size);
		// The snapshot records the completed copyout, including preserved padding.
		// It is not a second read of backing that could mask a later CPU write.
		if (!publication.empty()) std::memcpy(publication.data(), mapped, publication.size());
		if (Config::GraphicsDebugDumpEnabled()) {
			static std::atomic<uint32_t> receipts {0};
			const auto serial = receipts.fetch_add(1, std::memory_order_relaxed);
			if (serial < 16 || (serial < 3840 && serial % 120 == 0)) {
				uint64_t nonzero = 0, colored = 0;
				const auto* bytes = static_cast<const uint8_t*>(mapped);
				for (uint64_t i = 0; i < range.size; ++i) nonzero += bytes[i] != 0;
				const bool rgba8 = format == vk::Format::eR8G8B8A8Unorm ||
				    format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eR8G8B8A8Srgb ||
				    format == vk::Format::eB8G8R8A8Srgb;
				if (rgba8) for (uint64_t i = 0; i + 4 <= range.size; i += 4)
					colored += bytes[i] != 0 || bytes[i + 1] != 0 || bytes[i + 2] != 0;
				std::fprintf(stderr, "PS5_GPU_IMAGE_PUBLICATION serial=%u addr=%#llx bytes=%llu format=%u nonzero_bytes=%llu rgba8=%u colored_words=%llu\n",
				    serial, static_cast<unsigned long long>(range.address),
				    static_cast<unsigned long long>(range.size), static_cast<unsigned>(format),
				    static_cast<unsigned long long>(nonzero), rgba8,
				    static_cast<unsigned long long>(colored));
			}
		}
	});
	return true;
}

void TextureCache::InvalidateMemoryFromGPU(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		return;
	}
	std::scoped_lock lock {m_lock};
	for (const auto id: FindImagesInRegion(address, size, true)) {
		auto& image = m_slot_images[id];
		if (image.info.data.address != address) {
			continue;
		}
		if (image.IsGpuModified()) {
			image.ClearGpuModified();
		}
		image.MarkBufferModified();
	}
}

bool TextureCache::IsRegionGpuModified(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		return false;
	}
	std::scoped_lock lock {m_lock};
	return IsRegionGpuModifiedLocked(address, size);
}

bool TextureCache::IsRegionGpuModifiedLocked(uint64_t address, uint64_t size) const {
	for (const auto id: FindImagesInRegion(address, size, false)) {
		const auto& image = m_slot_images[id];
		// A separate stencil association retains GPU ownership independently of
		// depth. Conflicting CPU dirtiness cannot make unpublished GPU bytes clean.
		if (image.registered && image.IsGpuModified()) {
			return true;
		}
	}
	return false;
}

void TextureCache::InvalidateCpuAliases(uint64_t address, uint64_t size) {
	const auto page_begin = Common::AlignDown(address, TRACKER_PAGE_SIZE);
	const auto page_end   = Common::AlignUp(address + size, TRACKER_PAGE_SIZE);
	for (const auto id: FindImagesInRegion(address, size, true)) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		if (owner->Overlaps(address, size)) {
			owner->InvalidateCpuWrite(address, size);
			UntrackImage(id);
			continue;
		}
		const auto image_begin = owner->info.data.address;
		const auto image_end   = owner->info.data.End();
		if (page_end < image_end) {
			UntrackImageHead(id);
		} else if (image_begin < page_begin) {
			UntrackImageTail(id);
		} else {
			MarkAsMaybeDirty(id, *owner);
		}
	}
}

bool TextureCache::IsMeta(uint64_t address) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	return found != m_surface_metas.end();
}

bool TextureCache::IsMetaCleared(uint64_t address, uint32_t slice) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end() || slice >= 32) {
		return false;
	}
	return (found->second.clear_mask & (1u << slice)) != 0;
}

bool TextureCache::ClearMeta(uint64_t address) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end()) {
		return false;
	}
	found->second.clear_mask = UINT32_MAX;
	return true;
}

bool TextureCache::TouchMeta(uint64_t address, uint32_t slice, bool is_clear) {
	std::scoped_lock lock {m_lock};
	const auto       found = m_surface_metas.find(address);
	if (found == m_surface_metas.end() || slice >= 32) {
		return false;
	}
	if (is_clear) {
		found->second.clear_mask |= 1u << slice;
	} else {
		found->second.clear_mask &= ~(1u << slice);
	}
	return true;
}

bool TextureCache::IsRegionRegistered(uint64_t address, uint64_t size) {
	std::scoped_lock lock {m_lock};
	return !FindImagesInRegion(address, size, false).empty();
}

void TextureCache::UnmapMemory(uint64_t address, uint64_t size) {
	if (!GuestRange {address, size}.Valid()) {
		EXIT("TextureCache: invalid unmap range\n");
	}
	std::scoped_lock lock {m_lock};
	for (auto metadata = m_surface_metas.begin(); metadata != m_surface_metas.end();) {
		const auto base = metadata->first;
		if (base >= address && base < address + size) {
			metadata = m_surface_metas.erase(metadata);
		} else {
			++metadata;
		}
	}
	auto images = FindImagesInRegion(address, size, false);
	for (const auto id: images) {
		auto owner = m_slot_images.try_get(id);
		if (owner == nullptr) {
			continue;
		}
		FreeImage(id);
	}
}

void TextureCache::RunGarbageCollector() {
	std::scoped_lock lock {m_lock};
	const uint64_t   tick = m_gc_tick++;
	if (m_graphics.CanReportMemoryUsage()) {
		// Texture collection precedes buffer collection at each cache-maintenance point.
		m_graphics.RefreshMemoryBudget(static_cast<uint32_t>(tick));
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
		// Android/UMA budgets can shrink while a session is running.
		const auto thresholds = MemoryBudget::TextureThresholds(m_graphics.GetTotalMemoryBudget());
		m_trigger_gc_memory = thresholds.trigger;
		m_pressure_gc_memory = thresholds.pressure;
		m_critical_gc_memory = thresholds.critical;
	}
	if (m_total_used_memory < m_trigger_gc_memory) {
		return;
	}
	const auto collect = [&](bool allow_aggressive) {
		bool           pressured  = m_total_used_memory >= m_pressure_gc_memory;
		bool           aggressive = allow_aggressive && m_total_used_memory >= m_critical_gc_memory;
		const auto cutoff = MemoryBudget::CollectionCutoff(tick, aggressive ? 160 : pressured ? 80 : 16);
		if (!cutoff) return;
		size_t         deletions = aggressive ? 40 : pressured ? 20 : 10;
		std::vector<ImageId> candidates;
		candidates.reserve(deletions);
		// Deleting depth recursively deletes its stencil association, so finish LRU traversal
		// first.
		m_lru_cache.ForEachItemBelow(*cutoff, [&](ImageId id) {
			candidates.push_back(id);
			return candidates.size() == deletions;
		});
		for (const auto id: candidates) {
			if (deletions == 0) {
				break;
			}
			--deletions;
			auto owner = m_slot_images.try_get(id);
			if (owner == nullptr || !owner->registered || owner->depth_id) {
				continue;
			}
			if (owner->IsGpuModified()) {
				const bool safe = owner->SafeToDownload();
				if (safe && owner->info.IsTiled()) {
					continue;
				}
				if (safe && !pressured) {
					continue;
				}
			}
			FreeImage(id);
			if (m_total_used_memory < m_critical_gc_memory && aggressive) {
				deletions >>= 2;
				aggressive = false;
			}
			if (m_total_used_memory < m_pressure_gc_memory && pressured) {
				deletions >>= 1;
				pressured = false;
			}
		}
	};
	collect(false);
	if (m_total_used_memory >= m_critical_gc_memory) {
		collect(true);
	}
}

void TextureCache::ProcessDownloadImages() {
	// Synchronous epoch publication owns these downloads and their retirement.
	if (GuestGpuWriteOwnershipSupported()) return;
	std::scoped_lock lock {m_lock};
	for (const auto id: m_download_images) {
		const auto owner = m_slot_images.try_get(id);
		if (owner != nullptr && owner->registered && owner->IsGpuModified()) {
			(void)DownloadImageMemory(id);
		}
	}
	m_download_images.clear();
}

void TextureCache::PublishImageWrites(std::span<const ImageId> images) {
    Lumen::Performance::Scope publication(Lumen::Performance::Phase::GpuPublishTextures);
	std::vector<std::pair<ImageId, GuestRange>> dirty;
	for (const auto id: images) {
		const auto* image = m_slot_images.try_get(id);
		if (image == nullptr || !image->registered || image->info.data.Empty() ||
		    !image->IsGpuModified()) continue;
		ValidateGpuWrite(*image);
		if (!image->SafeToDownload())
			EXIT("PS5 GPU image publication has conflicting CPU/buffer ownership\n");
		// Retirement can select only a depth/stencil pair. It must still reject
		// another live GPU owner overlapping either selected guest plane.
		m_slot_images.ForEach([&](ImageId other_id, const Image& other) {
			if (other_id != id && other.registered && other.IsGpuModified() &&
			    other.Overlaps(image->info.data.address, image->info.data.size))
				EXIT("PS5 GPU publication has overlapping image owners\n");
		});
		for (const auto& [other_id, range]: dirty) {
			(void)other_id;
			if (ImageRangeOverlaps(range.address, range.size, image->info.data.address, image->info.data.size))
				EXIT("PS5 GPU publication has overlapping image owners\n");
		}
		dirty.emplace_back(id, image->info.data);
	}
	if (dirty.empty()) return;
	PrunePublicationSnapshots();
	struct PendingSnapshot {
		ImageId id;
		ImageInfo info;
		vk::Image native;
		BindingType binding;
		PublicationBuffer bytes;
	};
	std::vector<PendingSnapshot> snapshots;
	// At most one contiguous import batch per publication, preserving owner
	// order. No subsequent staged seed/copyout starts until the batch returns
	// with all Vulkan resources and VM borrows destroyed after native completion.
	bool import_attempted = false;
	for (size_t position = 0; position < dirty.size();) {
		const auto& [id, range] = dirty[position];
		if (!import_attempted && m_graphics.external_memory_host_enabled) {
			if (GpuSliceTrace::current != nullptr) ++GpuSliceTrace::current->image_host_import_considered;
			if (HasDisjointHostImportBacking(id)) {
				import_attempted = true;
				const auto completed = TryDownloadImageHostBatch(
				    std::span<const std::pair<ImageId, GuestRange>>(dirty).subspan(position));
				if (completed != 0) {
					position += completed;
					continue;
				}
			}
		}
		PublicationBuffer bytes;
		const auto& image = m_slot_images[id];
		if (CanSnapshotPublication(image)) {
			bytes = ReservePublicationSnapshot(range.size);
		}
		std::span<uint8_t> capture;
		if (bytes.data() != nullptr) {
			capture = {bytes.data(), static_cast<size_t>(bytes.size())};
			snapshots.push_back({id, image.info, image.backing.image, UploadBinding(image), std::move(bytes)});
		}
		if (!DownloadImageMemory(id, capture))
			EXIT("PS5 GPU image publication has no authoritative download\n");
		++position;
	}
	const auto tick = m_scheduler.CurrentTick();
	// Completion callbacks and buffer invalidation may enter cache code. The
	// exclusive guest epoch, and the single GPU worker, retain these owners while
	// the texture lock is released. Always restore the caller's lock on exit.
	struct UnlockForCompletion {
		TrackingSpinLock& lock;
		explicit UnlockForCompletion(TrackingSpinLock& held): lock(held) { lock.unlock(); }
		~UnlockForCompletion() { lock.lock(); }
	};
	{
		UnlockForCompletion unlocked(m_lock);
		m_scheduler.Finish();
		m_scheduler.WaitPriorityOperations(tick);
	}
	// Do not release one aspect's ownership while another aspect is unpublished.
	for (const auto& [id, range]: dirty) {
		auto* image = m_slot_images.try_get(id);
		if (image == nullptr || !image->registered || image->info.data != range ||
		    !image->SafeToDownload())
			EXIT("PS5 GPU image owner changed during backing publication\n");
		image->ClearGpuModified();
	}
    std::vector<GuestRange> aliases;
	for (const auto& [id, range]: dirty) {
		(void)id;
        std::vector<LibKernel::Memory::GuestBackingAlias> physical;
        if (!LibKernel::Memory::QueryGuestBackingAliases(range.address, range.size, &physical))
            EXIT("PS5 image publication lost physical backing identity\n");
		for (const auto& alias: physical) aliases.push_back({alias.address, alias.size});
	}
	const auto aliases_before = aliases.size();
	if (!CoalescePublicationRanges(aliases)) EXIT("PS5 publication has invalid physical alias ranges\n");
	if (PublicationTraceEnabled()) {
		m_publication_stats.aliases_before += aliases_before;
		m_publication_stats.aliases_after += aliases.size();
	}
    for (const auto& alias: aliases) InvalidateCpuAliases(alias.address, alias.size);
	{
		UnlockForCompletion unlocked(m_lock);
        for (const auto& alias: aliases)
            m_buffer_cache.InvalidateMemory(alias.address, alias.size);
	}
	// The entire publication batch and its alias invalidations are complete.
	// Only the exact native owner gets a byte witness; other views stay dirty.
	for (auto& snapshot: snapshots) {
		if (PublicationTraceEnabled()) m_publication_stats.copied_bytes += snapshot.bytes.size();
		auto* image = m_slot_images.try_get(snapshot.id);
		if (image == nullptr || !CanSnapshotPublication(*image) || image->IsGpuModified() ||
		    image->IsBufferModified() || image->backing.image != snapshot.native ||
		    UploadBinding(*image) != snapshot.binding ||
		    !SamePublicationLayout(image->info, snapshot.info)) {
			RecyclePublicationBuffer(std::move(snapshot.bytes));
			continue;
		}
		ForgetPublicationSnapshot(snapshot.id);
		if (image->IsCpuDirty()) image->RefreshComplete();
		PublicationSnapshot ready {snapshot.info, snapshot.native, snapshot.binding,
		    image->PublicationRevision(), std::move(snapshot.bytes), m_gc_tick};
		const auto size = ready.bytes.size();
		m_publication_snapshots.insert_or_assign(snapshot.id, std::move(ready));
		m_publication_snapshot_bytes += size;
	}
	if (PublicationTraceEnabled()) LogPublicationStats();
}

bool TextureCache::PublicationSnapshotFits(uint64_t used, uint64_t size) noexcept {
	return size != 0 && size <= PublicationSnapshotOwnerLimit &&
	    used <= PublicationSnapshotBudget && size <= PublicationSnapshotBudget - used;
}

TextureCache::PublicationBuffer TextureCache::ReservePublicationSnapshot(uint64_t size) {
	if (!PublicationSnapshotFits(0, size)) {
		if (PublicationTraceEnabled()) ++m_publication_stats.budget_misses;
		return {};
	}
	for (;;) {
		const auto reusable = std::find_if(m_publication_pool.begin(), m_publication_pool.end(),
		    [size](const auto& bytes) { return bytes.size() == size; });
		if (reusable != m_publication_pool.end()) {
			auto bytes = std::move(*reusable);
			m_publication_pool.erase(reusable);
			if (PublicationTraceEnabled()) {
				++m_publication_stats.reused_buffers;
				m_publication_stats.reused_bytes += size;
			}
			return bytes;
		}
		if (PublicationSnapshotFits(m_publication_allocated_bytes, size)) {
			// Default initialization of uint8_t leaves the payload untouched. Its
			// first reader runs only after the completed download writes every byte.
			PublicationBuffer bytes {std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[size]), size};
			if (!bytes.data()) {
				if (PublicationTraceEnabled()) ++m_publication_stats.allocation_failures;
				return {};
			}
			m_publication_allocated_bytes += size;
			if (PublicationTraceEnabled()) ++m_publication_stats.allocations;
			return bytes;
		}
		if (!m_publication_pool.empty()) {
			m_publication_allocated_bytes -= m_publication_pool.back().size();
			m_publication_pool.pop_back();
			continue;
		}
		if (m_publication_snapshots.empty()) {
			// The remaining allocation is pending copyout: never recycle an owner
			// retained by a callback just to admit another image in the batch.
			if (PublicationTraceEnabled()) ++m_publication_stats.budget_misses;
			return {};
		}
		const auto oldest = std::min_element(m_publication_snapshots.begin(), m_publication_snapshots.end(),
		    [](const auto& a, const auto& b) { return a.second.last_used < b.second.last_used; });
		const auto id = oldest->first;
		ForgetPublicationSnapshot(id);
		if (PublicationTraceEnabled()) ++m_publication_stats.evictions;
	}
}

void TextureCache::RecyclePublicationBuffer(PublicationBuffer bytes) {
	if (!bytes.data()) return;
	EXIT_IF(bytes.size() > m_publication_allocated_bytes);
	// Bound idle bookkeeping as well as payload bytes. Installed/pending owners
	// have independent lifetimes and are never exposed through this pool.
	if (m_publication_pool.size() >= 64) {
		m_publication_allocated_bytes -= bytes.size();
		return;
	}
	m_publication_pool.push_back(std::move(bytes));
}

bool TextureCache::CanSnapshotPublication(const Image& image) const {
	return ImagePublicationSnapshotEnabled() && CanSnapshotImage(image);
}

bool TextureCache::CanSnapshotUpload(const Image& image) const {
	return ImageUploadSnapshotEnabled() && CanSnapshotImage(image) &&
	    !GuestAsyncCpuVisibleExperimentEnabled() && !GuestAsyncCpuConcurrentExperimentRequested();
}

bool TextureCache::CanSnapshotImage(const Image& image) const {
	const auto& info = image.info;
	// Without exclusivity, full equality has no stable meaning until native use.
	// Conversion formats need a separate round-trip proof (notably D16/D32).
	return GuestGpuWriteOwnershipSupported() &&
	    !GuestAsyncCpuConcurrentExperimentEnabled() && image.registered && !image.depth_id &&
	    image.backing.image != nullptr && info.data.Valid() && !info.IsDepth() &&
	    !info.IsBlock() && !info.HasStencil() && !info.HasMetadata() &&
	    info.metadata.range.Empty() && !info.metadata.stencil_compressed &&
	    info.metadata.compression == VideoOutCompression::Uncompressed && !info.bgra16 &&
	    info.samples == 1 && image.backing.samples == 1 &&
	    info.pixel_format == image.backing.format && info.extent == image.backing.extent &&
	    info.resources.levels == image.backing.mip_levels &&
	    (info.IsVolume() ? 1u : info.resources.layers) == image.backing.layers &&
	    Prospero::RemapTextureFormat(info.guest_format) == info.guest_format &&
	    VulkanFormat(info.guest_format) == info.pixel_format;
}

void TextureCache::ForgetPublicationSnapshot(ImageId id) {
	const auto found = m_publication_snapshots.find(id);
	if (found == m_publication_snapshots.end()) return;
	EXIT_IF(found->second.bytes.size() > m_publication_snapshot_bytes);
	m_publication_snapshot_bytes -= found->second.bytes.size();
	RecyclePublicationBuffer(std::move(found->second.bytes));
	m_publication_snapshots.erase(found);
}

void TextureCache::PrunePublicationSnapshots() {
	for (auto it = m_publication_snapshots.begin(); it != m_publication_snapshots.end();) {
		const auto* image = m_slot_images.try_get(it->first);
		if (image != nullptr && image->registered &&
		    image->PublicationRevision() == it->second.revision) {
			++it;
			continue;
		}
		m_publication_snapshot_bytes -= it->second.bytes.size();
		RecyclePublicationBuffer(std::move(it->second.bytes));
		it = m_publication_snapshots.erase(it);
	}
}

bool TextureCache::ReusePublicationSnapshot(ImageId id) {
	if (!ImagePublicationSnapshotEnabled() && !ImageUploadSnapshotEnabled()) return false;
	const auto found = m_publication_snapshots.find(id);
	if (found == m_publication_snapshots.end()) {
		if (PublicationTraceEnabled()) ++m_publication_stats.misses;
		return false;
	}
	const auto& image = m_slot_images[id];
	auto& snapshot = found->second;
	const bool admitted = snapshot.from_upload ? CanSnapshotUpload(image) : CanSnapshotPublication(image);
	if (admitted && !image.IsGpuModified() && !image.IsCpuDirty() &&
	    !image.IsBufferModified() && image.PublicationRevision() == snapshot.revision &&
	    // A writer inside this allocation may own a different buffer/image ID.
	    // Equal CPU bytes cannot supersede its unpublished native contents.
	    !m_buffer_cache.HasGpuDirtyBytes(image.info.data.address, image.info.data.size) &&
	    !IsRegionGpuModifiedLocked(image.info.data.address, image.info.data.size) &&
	    UploadBinding(image) == snapshot.binding &&
	    image.backing.image == snapshot.native && SamePublicationLayout(image.info, snapshot.info) &&
	    snapshot.bytes.size() == image.info.data.size) {
		if (PublicationTraceEnabled()) m_publication_stats.compared_bytes += snapshot.bytes.size();
		if (LibKernel::Memory::TryEqualBacking(image.info.data.address,
		        snapshot.bytes.data(), snapshot.bytes.size())) {
			snapshot.last_used = m_gc_tick;
			if (PublicationTraceEnabled()) ++m_publication_stats.hits;
			if (snapshot.from_upload) {
				++m_publication_stats.upload_hits;
				m_publication_stats.upload_avoided_bytes += snapshot.bytes.size();
				if (m_publication_stats.upload_hits <= 5)
					std::fprintf(stderr, "PS5_IMAGE_UPLOAD_SNAPSHOT_V1 event=reuse hits=%" PRIu64
					    " bytes=%" PRIu64 " avoided_bytes=%" PRIu64
					    " fresh_comparison=full_span source=staging epoch_memo=0\n",
					    m_publication_stats.upload_hits, snapshot.bytes.size(),
					    m_publication_stats.upload_avoided_bytes);
			}
			return true;
		}
	}
	if (PublicationTraceEnabled()) ++m_publication_stats.misses;
	ForgetPublicationSnapshot(id);
	return false;
}

void TextureCache::LogPublicationStats() {
	auto& s = m_publication_stats;
	if (++s.batches > 8 && s.batches % 120 != 0) return;
	std::fprintf(stderr, "PS5_IMAGE_PUBLICATION_STATS batches=%llu snapshot_enabled=%u hits=%llu misses=%llu"
	    " compared_bytes=%llu copied_bytes=%llu allocations=%llu reused_buffers=%llu reused_bytes=%llu"
	    " evictions=%llu budget_misses=%llu allocation_failures=%llu live_bytes=%llu allocated_bytes=%llu"
	    " pooled_buffers=%zu prefill_saved_bytes=%llu prefill_preserved_bytes=%llu aliases_before=%llu aliases_after=%llu"
	    " block_seed_enabled=%u block_seed_plans=%llu block_seed_saved_bytes=%llu block_seed_preserved_bytes=%llu"
	    " upload_snapshot_enabled=%u upload_captures=%llu upload_hits=%llu upload_avoided_bytes=%llu\n",
	    static_cast<unsigned long long>(s.batches), ImagePublicationSnapshotEnabled() ? 1u : 0u,
	    static_cast<unsigned long long>(s.hits), static_cast<unsigned long long>(s.misses),
	    static_cast<unsigned long long>(s.compared_bytes), static_cast<unsigned long long>(s.copied_bytes),
	    static_cast<unsigned long long>(s.allocations), static_cast<unsigned long long>(s.reused_buffers),
	    static_cast<unsigned long long>(s.reused_bytes), static_cast<unsigned long long>(s.evictions),
	    static_cast<unsigned long long>(s.budget_misses), static_cast<unsigned long long>(s.allocation_failures),
	    static_cast<unsigned long long>(m_publication_snapshot_bytes),
	    static_cast<unsigned long long>(m_publication_allocated_bytes), m_publication_pool.size(),
	    static_cast<unsigned long long>(s.prefill_saved), static_cast<unsigned long long>(s.prefill_preserved),
	    static_cast<unsigned long long>(s.aliases_before), static_cast<unsigned long long>(s.aliases_after),
	    ImagePublicationBlockSeedEnabled() ? 1u : 0u,
	    static_cast<unsigned long long>(s.block_seed_plans),
	    static_cast<unsigned long long>(s.block_seed_saved),
	    static_cast<unsigned long long>(s.block_seed_preserved), ImageUploadSnapshotEnabled() ? 1u : 0u,
	    static_cast<unsigned long long>(s.upload_captures),
	    static_cast<unsigned long long>(s.upload_hits),
	    static_cast<unsigned long long>(s.upload_avoided_bytes));
}

bool TextureCache::IsGpuOwnerCpuVisible(const Image& image) const {
    const auto* parent = image.depth_id ? m_slot_images.try_get(image.depth_id) : &image;
    if (parent == nullptr || parent->info.data.Empty()) return true;
    if (LibKernel::Memory::QueryGuestBackingCpuVisible(parent->info.data.address, parent->info.data.size))
        return true;
    return parent->info.HasStencil() && LibKernel::Memory::QueryGuestBackingCpuVisible(
        parent->info.stencil.address, parent->info.stencil.size);
}

bool TextureCache::HasCpuVisibleGpuWrites() {
    std::scoped_lock lock {m_lock};
    bool visible = false;
    m_slot_images.ForEach([&](ImageId, const Image& image) {
        if (!image.info.data.Empty() && image.IsGpuModified() && IsGpuOwnerCpuVisible(image))
            visible = true;
    });
    return visible;
}

bool TextureCache::HasGuestWriteDependency(const Image& image,
                                          std::span<const GuestRange> dependencies) const {
    const auto* parent = image.depth_id ? m_slot_images.try_get(image.depth_id) : &image;
    if (parent == nullptr || !parent->registered || !parent->info.data.Valid())
        EXIT("PS5 texture dependency has no live family owner\n");
    for (const auto& dependency: dependencies) {
        if (!dependency.Valid()) EXIT("PS5 texture dependency has invalid guest range\n");
        if (ImageRangeOverlaps(dependency.address, dependency.size,
                              parent->info.data.address, parent->info.data.size) ||
            (parent->info.HasStencil() && ImageRangeOverlaps(dependency.address, dependency.size,
                 parent->info.stencil.address, parent->info.stencil.size))) return true;
    }
    return false;
}

bool TextureCache::HasGuestWriteDependency(uint64_t address, uint64_t size) {
    if (!GuestRange {address, size}.Valid()) EXIT("PS5 texture dependency has invalid guest range\n");
    std::scoped_lock lock {m_lock};
    for (const auto id: FindImagesInRegion(address, size, false)) {
        const auto* image = m_slot_images.try_get(id);
        if (image == nullptr || !image->registered) continue;
        const auto parent_id = image->depth_id ? image->depth_id : id;
        const auto* parent = m_slot_images.try_get(parent_id);
        if (parent == nullptr || !parent->registered)
            EXIT("PS5 texture dependency lost its indexed family owner\n");
        if (parent->IsGpuModified()) return true;
        bool dirty = false;
        m_slot_images.ForEach([&](ImageId, const Image& plane) {
            if (plane.registered && plane.depth_id == parent_id && plane.IsGpuModified()) dirty = true;
        });
        if (dirty) return true;
    }
    return false;
}

void TextureCache::CollectGuestWriteDependencies(std::span<const GuestRange> dependencies,
                                                std::vector<GuestRange>& owners) {
    EXIT_IF(!GuestGpuWriteOwnershipSupported());
    std::scoped_lock lock {m_lock};
    m_slot_images.ForEach([&](ImageId, const Image& image) {
        if (!image.registered || image.info.data.Empty() || !image.IsGpuModified() ||
            !HasGuestWriteDependency(image, dependencies)) return;
        owners.push_back(image.info.data);
    });
}

void TextureCache::PublishGuestWritesInRanges(std::span<const GuestRange> dependencies) {
    EXIT_IF(!GuestGpuWriteOwnershipSupported());
    if (dependencies.empty()) return;
    std::scoped_lock lock {m_lock};
    std::vector<ImageId> dirty;
    m_slot_images.ForEach([&](ImageId id, const Image& image) {
        if (image.registered && !image.info.data.Empty() && image.IsGpuModified() &&
            HasGuestWriteDependency(image, dependencies)) dirty.push_back(id);
    });
    PublishImageWrites(dirty);
    // Retained unrelated owners keep their pending download registrations.
    for (const auto id: dirty) m_download_images.erase(id);
}

void TextureCache::PublishGuestWrites(bool retain_private) {
	EXIT_IF(!GuestGpuWriteOwnershipSupported());
	std::scoped_lock lock {m_lock};
	std::vector<ImageId> dirty;
	m_slot_images.ForEach([&](ImageId id, Image& image) {
		if (!image.info.data.Empty() && image.IsGpuModified() &&
            (!retain_private || IsGpuOwnerCpuVisible(image))) dirty.push_back(id);
	});
	PublishImageWrites(dirty);
	m_download_images.clear();
}

} // namespace Libs::Graphics
