#include "graphics/host_gpu/renderer/drawCapture.h"

#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fmt/format.h>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace Libs::Graphics {
namespace {

constexpr uint64_t MaximumBytes = 32u * 1024u * 1024u;
constexpr uint32_t MaximumCopies = 512;
std::atomic<uint64_t> outstanding_bytes {0};

struct CaptureState {
	std::filesystem::path shader_folder;
	std::filesystem::path output_folder;
	double center_u = 0.5;
	double center_v = 0.5;
	uint32_t width = 64;
	uint32_t height = 64;
	uint32_t requested = 0;
	uint32_t copied = 0;
	uint32_t skipped = 0;
	uint32_t frames = 0;
	uint64_t skip_draws = 0;
	uint64_t eligible_draws = 0;
	uint64_t pixel_hash = 0;
	uint64_t bytes = 0;
	uint64_t draw_serial = 0;
	int last_frame = std::numeric_limits<int>::min();
	bool armed = false;
};

void WriteText(const std::filesystem::path& path, const std::string& text) {
	Common::File file(path);
	if (file.IsInvalid()) {
		LOGF("DrawCapture: cannot create %s\n", Common::PathToString(path).c_str());
		return;
	}
	file.Write(text.data(), static_cast<uint32_t>(text.size()));
}

void Complete(CaptureState& state, const char* reason) {
	WriteText(state.output_folder / "status.txt",
	          fmt::format("DrawCaptureV1 status={} copies={} requested={} bytes={} skipped={} "
	                      "pending_bytes={}\n", reason, state.copied, state.requested,
	                      state.bytes, state.skipped, outstanding_bytes.load()));
	state.requested = 0;
	state.armed = false;
}

template <typename T>
bool ParseNumber(std::string_view token, T& value) {
	const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
	return result.ec == std::errc() && result.ptr == token.data() + token.size();
}

bool ParseHash(std::string_view token, uint64_t& value) {
	if (token.starts_with("0x") || token.starts_with("0X")) token.remove_prefix(2);
	const auto result = std::from_chars(token.data(), token.data() + token.size(), value, 16);
	return result.ec == std::errc() && result.ptr == token.data() + token.size();
}

void PollRequest(CaptureState& state, int frame) {
	const auto& folder = Config::GetShaderLogFolder();
	if (state.shader_folder != folder) {
		state = {};
		state.shader_folder = folder;
	}
	if (state.last_frame == frame) return;
	state.last_frame = frame;
	state.draw_serial = 0;
	if (state.requested != 0) {
		state.armed = false;
		if (++state.frames >= 64) Complete(state, "frame-limit");
		return;
	}
	const auto request = folder / "draw-capture.request";
	std::error_code error;
	if (!std::filesystem::is_regular_file(request, error) || error) return;
	const auto serial = std::chrono::duration_cast<std::chrono::nanoseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count();
	const auto output = folder / fmt::format("draw-capture-{}", serial);
	if (!std::filesystem::create_directory(output, error) || error) return;
	// Rename consumes only this diagnostic request and preserves its exact bytes.
	std::filesystem::rename(request, output / "request.txt", error);
	if (error) return;
	Common::File input(output / "request.txt", Common::File::Mode::Read);
	std::array<char, 257> text {};
	double u = 0.0, v = 0.0;
	uint32_t width = 0, height = 0, count = 0;
	uint64_t skip_draws = 0, pixel_hash = 0;
	bool valid = !input.IsInvalid() && input.Size() != 0 && input.Size() <= 256;
	if (valid) {
		const auto size = static_cast<uint32_t>(input.Size());
		uint32_t read = 0;
		input.Read(text.data(), size, &read);
		std::istringstream stream(std::string(text.data(), size));
		std::array<std::string, 8> tokens;
		size_t fields = 0;
		while (fields < tokens.size() && stream >> tokens[fields]) ++fields;
		valid = read == size && fields >= 5 && fields <= 7 &&
		        ParseNumber(tokens[0], u) && ParseNumber(tokens[1], v) &&
		        ParseNumber(tokens[2], width) && ParseNumber(tokens[3], height) &&
		        ParseNumber(tokens[4], count) &&
		        (fields < 6 || ParseNumber(tokens[5], skip_draws)) &&
		        (fields < 7 || ParseHash(tokens[6], pixel_hash)) && skip_draws <= 1000000 &&
		        std::isfinite(u) && std::isfinite(v) && u >= 0.0 && u <= 1.0 &&
		        v >= 0.0 && v <= 1.0 && width >= 1 && width <= 256 &&
		        height >= 1 && height <= 256 && count >= 1 && count <= MaximumCopies;
	}
	if (!valid) {
		WriteText(output / "status.txt", "DrawCaptureV1 status=invalid-request\n");
		return;
	}
	state.output_folder = output;
	state.center_u = u;
	state.center_v = v;
	state.width = width;
	state.height = height;
	state.requested = count;
	state.copied = state.skipped = state.frames = 0;
	state.bytes = 0;
	state.skip_draws = skip_draws;
	state.pixel_hash = pixel_hash;
	state.eligible_draws = 0;
	state.armed = true;
	WriteText(output / "status.txt", "DrawCaptureV1 status=armed-next-frame\n");
}

uint32_t PixelBytes(vk::Format format) {
	switch (format) {
		case vk::Format::eR8G8B8A8Unorm:
		case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eR32Sfloat:
		case vk::Format::eR32Uint:
		case vk::Format::eB10G11R11UfloatPack32:
		case vk::Format::eA2B10G10R10UnormPack32: return 4;
		case vk::Format::eR16Sfloat: return 2;
		case vk::Format::eR16G16Sfloat: return 4;
		case vk::Format::eR16G16B16A16Sfloat:
		case vk::Format::eR32G32Sfloat: return 8;
		case vk::Format::eR32G32B32A32Sfloat: return 16;
		default: return 0;
	}
}

void EmitImageBarriers(vk::CommandBuffer command, const Image::Barriers& barriers) {
	if (barriers.empty()) return;
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
	dependency.pImageMemoryBarriers = barriers.data();
	command.pipelineBarrier2(dependency);
}

} // namespace

void CaptureDrawOutputs(RenderContext& context, CommandBuffer& command,
                        std::span<const RenderColorInfo> colors, const DrawCaptureInfo& draw) {
	if (Config::GetShaderLogDirection() != Config::LogDirection::File) return;
	// Called only by the serialized renderer owner; completion callbacks retain
	// buffer/value copies, never references into this state or the image cache.
	static CaptureState state;
	const auto frame = context.GetGpu().GetFrameNum();
	PollRequest(state, frame);
	const auto draw_serial = state.draw_serial++;
	if (state.requested == 0 || state.armed) return;
	if (state.pixel_hash != 0 && draw.pixel_hash != state.pixel_hash) return;
	// Splitting a pass with persistent explicit clear flags would replay its
	// load clears on the next otherwise identical draw. Leave that pass intact.
	if (draw.attachment_clear) {
		if (state.skipped++ < 32) {
			LOGF("DrawCapture: skipped frame=%d draw=%" PRIu64 " reason=attachment-clear\n",
			     frame, draw_serial);
		}
		return;
	}
	auto& scheduler = context.GetCommandScheduler();
	bool eligible_draw = false;
	for (const auto& color: colors) {
		if (!color.image_id) continue;
		auto& image = context.GetTextureCache().GetImage(color.image_id);
		const auto& view = color.desc.view_info;
		const auto pixel_bytes = PixelBytes(image.backing.format);
		if (pixel_bytes == 0 || image.backing.image_type != vk::ImageType::e2D ||
		    image.backing.samples != 1 || view.layer_count != 1 ||
		    view.aspect != vk::ImageAspectFlagBits::eColor ||
		    view.base_level >= image.backing.mip_levels || view.base_layer >= image.backing.layers ||
		    !(image.backing.usage & vk::ImageUsageFlagBits::eTransferSrc)) {
			if (state.skipped++ < 32) {
				LOGF("DrawCapture: skipped frame=%d draw=%" PRIu64 " slot=%u format=%u "
				     "samples=%u type=%u layers=%u\n", frame, draw_serial, color.target_slot,
				     static_cast<uint32_t>(image.backing.format), image.backing.samples,
				     static_cast<uint32_t>(image.backing.image_type), view.layer_count);
			}
			continue;
		}
		const auto extent = color.Extent();
		const auto mip_extent = image.info.MipExtent(view.base_level);
		const uint32_t target_width = std::min(extent.width, mip_extent.width);
		const uint32_t target_height = std::min(extent.height, mip_extent.height);
		if (target_width == 0 || target_height == 0) continue;
		if (!eligible_draw) {
			eligible_draw = true;
			if (state.eligible_draws++ < state.skip_draws) return;
		}
		const uint32_t width = std::min(state.width, target_width);
		const uint32_t height = std::min(state.height, target_height);
		const auto origin = [](double center, uint32_t size, uint32_t length) {
			const auto position = static_cast<int64_t>(center * static_cast<double>(size - 1u));
			return static_cast<uint32_t>(std::clamp<int64_t>(position - length / 2u, 0, size - length));
		};
		const uint32_t x = origin(state.center_u, target_width, width);
		const uint32_t y = origin(state.center_v, target_height, height);
		const uint64_t bytes = static_cast<uint64_t>(width) * height * pixel_bytes;
		if (bytes > MaximumBytes - state.bytes ||
		    bytes > MaximumBytes - outstanding_bytes.load(std::memory_order_relaxed)) {
			Complete(state, "byte-limit");
			return;
		}
		auto download = std::make_unique<Buffer>(context.GetGraphics(), scheduler,
		    MemoryUsage::Download, 0, vk::BufferUsageFlagBits::eTransferDst, bytes);
		if (download->Mapped().size() < bytes) {
			Complete(state, "mapping-unavailable");
			return;
		}
		const auto original_global = image.backing.state;
		const bool had_subresources = !image.backing.subresource_states.empty();
		const auto subresource_index = view.base_level * image.info.resources.layers + view.base_layer;
		if (had_subresources && subresource_index >= image.backing.subresource_states.size()) {
			Complete(state, "invalid-subresource-state");
			return;
		}
		const auto original = had_subresources
		    ? image.backing.subresource_states[subresource_index] : original_global;
		const ImageSubresourceRange range {view.base_level, 1, view.base_layer, 1};
		scheduler.EndRendering();
		const auto native = command.Handle();
		EmitImageBarriers(native, image.GetBarriers(vk::ImageLayout::eTransferSrcOptimal,
		    vk::AccessFlagBits2::eTransferRead, vk::PipelineStageFlagBits2::eCopy, range));
		vk::BufferImageCopy copy {};
		copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, view.base_level, view.base_layer, 1};
		copy.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
		copy.imageExtent = {width, height, 1};
		native.copyImageToBuffer(image.backing.image, vk::ImageLayout::eTransferSrcOptimal,
		                         download->Handle(), 1, &copy);
		EmitImageBarriers(native, image.GetBarriers(original.layout, original.access_mask,
		                                          original.pl_stage, range));
		// Restore the original bookkeeping representation after the native reverse
		// transition; unrelated mip/layer states were never changed.
		image.backing.state = original_global;
		if (!had_subresources) image.backing.subresource_states.clear();
		vk::BufferMemoryBarrier2 host {};
		host.srcStageMask = vk::PipelineStageFlagBits2::eCopy;
		host.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
		host.dstStageMask = vk::PipelineStageFlagBits2::eHost;
		host.dstAccessMask = vk::AccessFlagBits2::eHostRead;
		host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		host.buffer = download->Handle();
		host.size = bytes;
		vk::DependencyInfo dependency {};
		dependency.bufferMemoryBarrierCount = 1;
		dependency.pBufferMemoryBarriers = &host;
		native.pipelineBarrier2(dependency);
		const auto stem = fmt::format("{:04d}", state.copied);
		const auto output_folder = state.output_folder;
		const auto metadata = fmt::format(
		    "DrawCaptureV1 frame={} request_frame={} draw={} eligible_draw={} submit={} output={} "
		    "slot={} vs_id={} vs_hash=0x{:016x} "
		    "ps_id={} ps_hash=0x{:016x} indexed={} count={} instances={} address=0x{:x} "
		    "backing_format={} view_format={} export_mapping=0x{:02x} view_mapping={},{},{},{} "
		    "mip={} layer={} target_width={} target_height={} x={} y={} width={} height={} "
		    "pixel_bytes={} row_bytes={} bytes={}\n", frame, state.frames, draw_serial,
		    state.eligible_draws - 1, draw.submit_id,
		    state.copied, color.target_slot, draw.vertex_id, draw.vertex_hash, draw.pixel_id,
		    draw.pixel_hash, draw.indexed, draw.count, draw.instances, image.info.data.address,
		    static_cast<uint32_t>(image.backing.format), static_cast<uint32_t>(view.format),
		    color.export_mapping.packed, static_cast<uint32_t>(view.mapping.r),
		    static_cast<uint32_t>(view.mapping.g), static_cast<uint32_t>(view.mapping.b),
		    static_cast<uint32_t>(view.mapping.a), view.base_level, view.base_layer,
		    target_width, target_height, x, y, width, height, pixel_bytes, width * pixel_bytes, bytes);
		outstanding_bytes.fetch_add(bytes, std::memory_order_relaxed);
		scheduler.DeferOperation([owner = std::move(download), output_folder, stem, metadata, bytes] {
			owner->Invalidate(0, bytes);
			Common::File raw(output_folder / (stem + ".raw"));
			uint32_t written = 0;
			if (!raw.IsInvalid()) raw.Write(owner->Mapped().data(), static_cast<uint32_t>(bytes), &written);
			WriteText(output_folder / (stem + ".txt"), metadata +
			          fmt::format("written_bytes={} status={}\n", written,
			                      written == bytes ? "complete" : "write-failed"));
			outstanding_bytes.fetch_sub(bytes, std::memory_order_relaxed);
		});
		state.bytes += bytes;
		if (++state.copied == state.requested) {
			Complete(state, "queued");
			return;
		}
	}
}

} // namespace Libs::Graphics
