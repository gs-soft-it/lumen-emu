// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "video_core/renderer_vulkan/bounded_draw_diagnostics.h"
#include "video_core/renderer_vulkan/frame_ledger.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "lumen/succi_benchmark.h"
#include "core/libraries/system/systemservice.h"
#include "imgui/notifications_layer.h"
#include "imgui/renderer/imgui_core.h"
#include "imgui/renderer/imgui_impl_vulkan.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/stall_log.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <vector>
#include <imgui.h>
#include <png.h>
#include <vk_mem_alloc.h>

namespace Vulkan {

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

enum class ScreenshotKind : u8 {
    GameOnly,
    WithOverlays,
};

struct ScreenshotReadback {
    ScreenshotKind kind{};
    std::vector<std::filesystem::path> paths{};
    VideoCore::Buffer buffer;
    u32 width{};
    u32 height{};
    vk::Format format{};
    bool hdr_encoded{};

    ScreenshotReadback(const Instance& instance, Scheduler& scheduler, ScreenshotKind kind_,
                       std::vector<std::filesystem::path> paths_, const u32 width_,
                       const u32 height_, const vk::Format format_, const bool hdr_encoded_)
        : kind{kind_}, paths{std::move(paths_)},
          buffer{instance,
                 scheduler,
                 VideoCore::MemoryUsage::Download,
                 0,
                 vk::BufferUsageFlagBits::eTransferDst,
                 static_cast<u64>(width_) * static_cast<u64>(height_) * 4},
          width{width_}, height{height_}, format{format_}, hdr_encoded{hdr_encoded_} {}
};

static std::string SanitizeFilenameComponent(std::string value) {
    for (char& c : value) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-') {
            c = '_';
        }
    }
    if (value.empty()) {
        return "UNKNOWN";
    }
    return value;
}

static std::vector<std::filesystem::path> BuildScreenshotPaths(const ScreenshotKind kind,
                                                               const u32 count) {
    static std::atomic<u64> screenshot_sequence{0};
    std::vector<std::filesystem::path> paths{};
    if (count == 0) {
        return paths;
    }

    const auto& screenshots_dir = Common::FS::GetUserPath(Common::FS::PathType::ScreenshotsDir);
    std::filesystem::create_directories(screenshots_dir);

    const auto game_id =
        SanitizeFilenameComponent(std::string(Common::ElfInfo::Instance().GameSerial()));
    const auto now = std::chrono::system_clock::now();
    const auto now_time = std::chrono::system_clock::to_time_t(now);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000;

    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &now_time);
#else
    localtime_r(&now_time, &local_tm);
#endif

    std::ostringstream stamp;
    stamp << std::put_time(&local_tm, "%Y%m%d_%H%M%S") << '_' << std::setw(3) << std::setfill('0')
          << ms;

    const char* suffix = kind == ScreenshotKind::GameOnly ? "game" : "hud";
    const auto first_sequence = screenshot_sequence.fetch_add(count, std::memory_order_relaxed);

    paths.reserve(count);
    const auto stamp_str = stamp.str();
    for (u32 i = 0; i < count; ++i) {
        paths.emplace_back(screenshots_dir / fmt::format("{}_{}_{}_{:06}.png", game_id, stamp_str,
                                                         suffix, first_sequence + i));
    }

    return paths;
}

static float PqToNits(const float encoded) {
    // ST.2084 inverse EOTF
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;

    const float v = std::clamp(encoded, 0.0f, 1.0f);
    const float vp = std::pow(v, 1.0f / m2);
    const float num = std::max(vp - c1, 0.0f);
    const float den = std::max(c2 - c3 * vp, 1e-6f);
    return 10000.0f * std::pow(num / den, 1.0f / m1);
}

static float ToneMapToSdrLinear(const float nits) {
    // Map absolute HDR luminance into SDR [0,1], preserving 100-nit white.
    constexpr float sdr_white_nits = 100.0f;
    const float x = std::max(nits, 0.0f) / sdr_white_nits;
    const float mapped = (2.0f * x) / (1.0f + x);
    return std::clamp(mapped, 0.0f, 1.0f);
}

static float LinearToSrgb(const float linear) {
    const float x = std::clamp(linear, 0.0f, 1.0f);
    if (x <= 0.0031308f) {
        return 12.92f * x;
    }
    return 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

static const std::array<float, 1024>& GetPqDecodeNitsLut() {
    static const std::array<float, 1024> lut = [] {
        std::array<float, 1024> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = PqToNits(static_cast<float>(i) / 1023.0f);
        }
        return values;
    }();
    return lut;
}

static const std::array<u8, 1024>& GetUnorm10ToU8Lut() {
    static const std::array<u8, 1024> lut = [] {
        std::array<u8, 1024> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = static_cast<u8>((i * 255u + 511u) / 1023u);
        }
        return values;
    }();
    return lut;
}

template <typename Readback>
static void CopyImageToReadback(const vk::CommandBuffer& cmdbuf, const vk::Image image,
                                const vk::ImageLayout layout, Readback& readback) {
    const vk::BufferImageCopy copy_region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {readback.width, readback.height, 1},
    };
    cmdbuf.copyImageToBuffer(image, layout, readback.buffer.Handle(), copy_region);
}

template <typename Readback>
static bool ConvertReadbackToRgba8(const Readback& readback, std::vector<u8>& out_rgba) {
    const u64 pixel_count = static_cast<u64>(readback.width) * static_cast<u64>(readback.height);
    const u64 byte_size = pixel_count * 4;
    if (readback.buffer.mapped_data.size() < byte_size) {
        LOG_ERROR(Render_Vulkan, "Screenshot readback buffer size mismatch (have {}, need {})",
                  readback.buffer.mapped_data.size(), byte_size);
        return false;
    }

    const auto src =
        std::span<const u8>{readback.buffer.mapped_data.data(), static_cast<size_t>(byte_size)};
    out_rgba.resize(static_cast<size_t>(byte_size));

    switch (readback.format) {
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
        std::memcpy(out_rgba.data(), src.data(), out_rgba.size());
        for (u64 i = 0; i < pixel_count; ++i) {
            out_rgba[static_cast<size_t>(i) * 4 + 3] = 255;
        }
        return true;
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            out_rgba[o + 0] = src[o + 2];
            out_rgba[o + 1] = src[o + 1];
            out_rgba[o + 2] = src[o + 0];
            out_rgba[o + 3] = 255;
        }
        return true;
    case vk::Format::eA2R10G10B10UnormPack32: {
        const auto& pq_decode_lut = GetPqDecodeNitsLut();
        const auto& unorm10_to_u8 = GetUnorm10ToU8Lut();

        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            const u32 packed = static_cast<u32>(src[o + 0]) | (static_cast<u32>(src[o + 1]) << 8) |
                               (static_cast<u32>(src[o + 2]) << 16) |
                               (static_cast<u32>(src[o + 3]) << 24);
            const u32 b = (packed >> 0) & 0x3FF;
            const u32 g = (packed >> 10) & 0x3FF;
            const u32 r = (packed >> 20) & 0x3FF;

            if (readback.hdr_encoded) {
                // Rec.2020 + PQ. Convert to SDR Rec.709 for PNG output.
                const float r2020 = pq_decode_lut[r];
                const float g2020 = pq_decode_lut[g];
                const float b2020 = pq_decode_lut[b];

                const float r709_nits = 1.6605f * r2020 - 0.5876f * g2020 - 0.0728f * b2020;
                const float g709_nits = -0.1246f * r2020 + 1.1329f * g2020 - 0.0083f * b2020;
                const float b709_nits = -0.0182f * r2020 - 0.1006f * g2020 + 1.1187f * b2020;

                const float r_srgb = LinearToSrgb(ToneMapToSdrLinear(r709_nits));
                const float g_srgb = LinearToSrgb(ToneMapToSdrLinear(g709_nits));
                const float b_srgb = LinearToSrgb(ToneMapToSdrLinear(b709_nits));

                out_rgba[o + 0] = static_cast<u8>(std::clamp(r_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 1] = static_cast<u8>(std::clamp(g_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 2] = static_cast<u8>(std::clamp(b_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
            } else {
                out_rgba[o + 0] = unorm10_to_u8[r];
                out_rgba[o + 1] = unorm10_to_u8[g];
                out_rgba[o + 2] = unorm10_to_u8[b];
            }
            out_rgba[o + 3] = 255;
        }
        return true;
    }
    case vk::Format::eA2B10G10R10UnormPack32: {
        const auto& pq_decode_lut = GetPqDecodeNitsLut();
        const auto& unorm10_to_u8 = GetUnorm10ToU8Lut();

        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            const u32 packed = static_cast<u32>(src[o + 0]) | (static_cast<u32>(src[o + 1]) << 8) |
                               (static_cast<u32>(src[o + 2]) << 16) |
                               (static_cast<u32>(src[o + 3]) << 24);
            const u32 r = (packed >> 0) & 0x3FF;
            const u32 g = (packed >> 10) & 0x3FF;
            const u32 b = (packed >> 20) & 0x3FF;

            if (readback.hdr_encoded) {
                // HDR swapchain path is Rec.2020 + PQ. Convert to SDR Rec.709 for PNG output.
                const float r2020 = pq_decode_lut[r];
                const float g2020 = pq_decode_lut[g];
                const float b2020 = pq_decode_lut[b];

                const float r709_nits = 1.6605f * r2020 - 0.5876f * g2020 - 0.0728f * b2020;
                const float g709_nits = -0.1246f * r2020 + 1.1329f * g2020 - 0.0083f * b2020;
                const float b709_nits = -0.0182f * r2020 - 0.1006f * g2020 + 1.1187f * b2020;

                const float r_srgb = LinearToSrgb(ToneMapToSdrLinear(r709_nits));
                const float g_srgb = LinearToSrgb(ToneMapToSdrLinear(g709_nits));
                const float b_srgb = LinearToSrgb(ToneMapToSdrLinear(b709_nits));

                out_rgba[o + 0] = static_cast<u8>(std::clamp(r_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 1] = static_cast<u8>(std::clamp(g_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 2] = static_cast<u8>(std::clamp(b_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
            } else {
                out_rgba[o + 0] = unorm10_to_u8[r];
                out_rgba[o + 1] = unorm10_to_u8[g];
                out_rgba[o + 2] = unorm10_to_u8[b];
            }
            out_rgba[o + 3] = 255;
        }
        return true;
    }
    default:
        LOG_WARNING(Render_Vulkan, "Unsupported screenshot format: {}",
                    vk::to_string(readback.format));
        return false;
    }
}

static bool WritePng(const std::filesystem::path& path, const std::span<const u8> rgba,
                     const u32 width, const u32 height) {
    Common::FS::IOFile file(path, Common::FS::FileAccessMode::Create);
    if (!file.IsOpen()) {
        return false;
    }

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (png_ptr == nullptr) {
        return false;
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == nullptr) {
        png_destroy_write_struct(&png_ptr, nullptr);
        return false;
    }

    if (setjmp(png_jmpbuf(png_ptr)) != 0) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        return false;
    }

    png_init_io(png_ptr, file.file);
    png_set_IHDR(png_ptr, info_ptr, width, height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png_ptr, info_ptr);

    thread_local std::vector<png_bytep> rows;
    rows.resize(height);
    for (u32 y = 0; y < height; ++y) {
        rows[y] = const_cast<png_bytep>(rgba.data() + static_cast<size_t>(y) * width * 4);
    }

    png_write_image(png_ptr, rows.data());
    png_write_end(png_ptr, info_ptr);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    return true;
}

static void SavePendingScreenshots(const std::vector<ScreenshotReadback>& readbacks) {
    for (const auto& readback : readbacks) {
        if (readback.paths.empty()) {
            continue;
        }

        std::vector<u8> rgba;
        if (!ConvertReadbackToRgba8(readback, rgba)) {
            continue;
        }

        const auto& primary_path = readback.paths.front();
        if (!WritePng(primary_path, rgba, readback.width, readback.height)) {
            LOG_ERROR(Render_Vulkan, "Failed saving screenshot to {}", primary_path.string());
            continue;
        }

        LOG_INFO(Render_Vulkan, "Saved screenshot: {}", primary_path.string());

        std::ifstream file(primary_path, std::ios::binary);
        std::vector<u8> imgdata;
        if (file) {
            imgdata = std::vector<u8>(std::istreambuf_iterator<char>(file),
                                      std::istreambuf_iterator<char>());
        }
        shadNotifications::QueueNotification("Saved screenshot:\n" + primary_path.string(), 3.0f,
                                             shadNotifications::position::BottomRight, imgdata);

        for (size_t i = 1; i < readback.paths.size(); ++i) {
            const auto& path = readback.paths[i];
            std::error_code ec{};
            std::filesystem::copy_file(primary_path, path, std::filesystem::copy_options::none, ec);
            if (ec) {
                // Fallback for platforms/filesystems where copy_file can fail for transient
                // reasons.
                if (!WritePng(path, rgba, readback.width, readback.height)) {
                    LOG_ERROR(Render_Vulkan, "Failed saving screenshot to {}", path.string());
                    continue;
                }
            }

            LOG_INFO(Render_Vulkan, "Saved screenshot: {}", path.string());
            std::ifstream file(path, std::ios::binary);
            std::vector<u8> imgdata;
            if (file) {
                imgdata = std::vector<u8>(std::istreambuf_iterator<char>(file),
                                          std::istreambuf_iterator<char>());
            }
            shadNotifications::QueueNotification("Saved screenshot:\n" + path.string(), 3.0f,
                                                 shadNotifications::position::BottomRight, imgdata);
        }
    }
}

#ifdef ANDROID
// Diagnostic resources never alias guest resources. At most four captures can be
// queued; the renderer drops a capture instead of waiting for PNG/file work.
struct FrameLedgerReadbackPool {
    static constexpr u32 Width = 480;
    static constexpr u32 Height = 270;
    static constexpr u64 MaxBytes = 128 * 1024 * 1024;

    struct Readback {
        struct HostBuffer {
            VmaAllocator allocator{};
            VkBuffer buffer{};
            VmaAllocation allocation{};
            std::span<u8> mapped_data;
            const u64 size_bytes = Width * Height * 4;
            ~HostBuffer() {
                if (buffer) vmaDestroyBuffer(allocator, buffer, allocation);
            }
            vk::Buffer Handle() const { return vk::Buffer{buffer}; }
            void InvalidateReadback() {
                vmaInvalidateAllocation(allocator, allocation, 0, size_bytes);
            }
            bool Create(const Instance& instance) {
                allocator = instance.GetAllocator();
                const VkBufferCreateInfo info{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                    .size = size_bytes,
                    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                };
                const VmaAllocationCreateInfo alloc_info{
                    .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT |
                             VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT,
                    .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                    .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                };
                VmaAllocationInfo result{};
                if (vmaCreateBuffer(allocator, &info, &alloc_info, &buffer, &allocation, &result) !=
                    VK_SUCCESS) {
                    buffer = VK_NULL_HANDLE;
                    return false;
                }
                if (!result.pMappedData) {
                    vmaDestroyBuffer(allocator, buffer, allocation);
                    buffer = VK_NULL_HANDLE;
                    return false;
                }
                mapped_data = {static_cast<u8*>(result.pMappedData), size_bytes};
                return true;
            }
        } buffer;
        const u32 width = Width, height = Height;
        vk::Format format{};
        bool hdr_encoded{};
    };

    struct Slot {
        std::atomic<bool> busy{false};
        VmaAllocator allocator{};
        VkImage image{};
        VmaAllocation allocation{};
        vk::Format backing_format{};
        std::unique_ptr<Readback> readback;

        ~Slot() {
            if (image) vmaDestroyImage(allocator, image, allocation);
        }

        bool Prepare(const Instance& instance, vk::Format format,
                     vk::Format view_format, bool hdr_encoded) {
            if (readback && backing_format == format && readback->format == view_format) {
                readback->hdr_encoded = hdr_encoded;
                return true;
            }
            if (image) {
                vmaDestroyImage(allocator, image, allocation);
                image = VK_NULL_HANDLE;
            }
            readback.reset();
            allocator = instance.GetAllocator();
            backing_format = format;
            const vk::ImageCreateInfo image_info{
                .imageType = vk::ImageType::e2D,
                .format = format,
                .extent = {Width, Height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
            };
            const VkImageCreateInfo raw_info = image_info;
            const VmaAllocationCreateInfo alloc_info{
                .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
                .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            };
            if (vmaCreateImage(allocator, &raw_info, &alloc_info, &image, &allocation, nullptr) !=
                VK_SUCCESS) {
                image = VK_NULL_HANDLE;
                return false;
            }
            readback = std::make_unique<Readback>();
            if (!readback->buffer.Create(instance)) {
                readback.reset();
                return false;
            }
            readback->format = view_format;
            readback->hdr_encoded = hdr_encoded;
            return true;
        }
    };

    struct Budget {
        u64 capture_id{};
        u64 written_bytes{}; // Accessed only by the scheduler's serial callback worker.
    };

    std::array<Slot, 4> slots;
    std::shared_ptr<Budget> budget;
    std::filesystem::path directory = Common::FS::GetUserPath(Common::FS::PathType::ScreenshotsDir) /
        fmt::format("frame-ledger-{}", std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    Slot* Acquire() {
        for (auto& slot : slots) {
            bool expected = false;
            if (slot.busy.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
                return &slot;
            }
        }
        return nullptr;
    }
};

static void QueueFrameLedgerImage(const Instance& instance, Scheduler& scheduler,
                                 VideoCore::Image& image, const vk::Extent2D image_size,
                                 VAddr scanout, vk::Format view_format, bool hdr_encoded,
                                 std::shared_ptr<FrameLedgerReadbackPool>& pool) {
    const auto snapshot = FrameLedger::TakeFrame(scanout, scheduler.CurrentTick());
    if (!snapshot) return;
    const auto capture_id = snapshot->burst_id;
    const auto frame_id = snapshot->frame;
    const auto drop = [&](const char* reason) {
        LOG_WARNING(Render_Vulkan, "LUMEN_GEN4_FRAME_LEDGER capture={} frame={} image=missing reason={}",
                    capture_id, frame_id, reason);
    };
    const auto format = image.info.pixel_format;
    const auto features = instance.GetPhysicalDevice().getFormatProperties(format).optimalTilingFeatures;
    if (!(features & vk::FormatFeatureFlagBits::eBlitSrc) ||
        !(features & vk::FormatFeatureFlagBits::eBlitDst)) {
        drop("format_blit_unsupported");
        return;
    }
    if (!pool) pool = std::make_shared<FrameLedgerReadbackPool>();
    if (!pool->budget || pool->budget->capture_id != capture_id) {
        pool->budget = std::make_shared<FrameLedgerReadbackPool::Budget>();
        pool->budget->capture_id = capture_id;
    }
    auto* slot = pool->Acquire();
    if (!slot) {
        drop("readback_pool_busy");
        return;
    }
    bool prepared = false;
    try {
        prepared = slot->Prepare(instance, format, view_format, hdr_encoded);
    } catch (const std::bad_alloc&) {
        // Capturing is optional and must release the slot on allocation failure.
    }
    if (!prepared) {
        slot->busy.store(false, std::memory_order_release);
        drop("image_allocation_failed");
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {}, cmdbuf);
    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = vk::Image{slot->image},
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    cmdbuf.blitImage(image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                    vk::Image{slot->image}, vk::ImageLayout::eTransferDstOptimal,
                    MakeImageBlitStretch(image_size.width, image_size.height,
                                         FrameLedgerReadbackPool::Width, FrameLedgerReadbackPool::Height),
                    vk::Filter::eNearest);
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
    barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    CopyImageToReadback(cmdbuf, vk::Image{slot->image}, vk::ImageLayout::eTransferSrcOptimal,
                        *slot->readback);
    const vk::BufferMemoryBarrier2 host_read{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        .buffer = slot->readback->buffer.Handle(),
        .offset = 0,
        .size = slot->readback->buffer.size_bytes,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &host_read});
    const auto budget = pool->budget;
    scheduler.DeferPriorityOperation([pool, slot, snapshot, budget, image_size, format, view_format]() {
        // Keep pool/metadata alive until the copy's timeline tick has completed.
        struct Release { FrameLedgerReadbackPool::Slot* slot;
            ~Release() { slot->busy.store(false, std::memory_order_release); }
        } release{slot};
        try {
            const auto stem = fmt::format("capture_{}_frame_{:04}", snapshot->burst_id, snapshot->frame);
            const auto png_path = pool->directory / (stem + ".png");
            const auto json_path = pool->directory / (stem + ".json");
            const auto json = fmt::format(
                "{{\"image\":\"{}.png\",\"source_width\":{},\"source_height\":{},"
                "\"capture_width\":{},\"capture_height\":{},\"backing_format\":\"{}\",\"view_format\":\"{}\","
                "\"resampling\":\"nearest\",\"ledger\":{}}}\n",
                stem, image_size.width, image_size.height, FrameLedgerReadbackPool::Width,
                FrameLedgerReadbackPool::Height, vk::to_string(format), vk::to_string(view_format),
                FrameLedger::Serialize(*snapshot));
            // A conservative PNG upper bound, independent of its compression ratio.
            constexpr u64 max_png_bytes = FrameLedgerReadbackPool::Width * FrameLedgerReadbackPool::Height * 5 + 65536;
            if (budget->written_bytes + json.size() + max_png_bytes > FrameLedgerReadbackPool::MaxBytes) {
                LOG_WARNING(Render_Vulkan, "LUMEN_GEN4_FRAME_LEDGER capture={} frame={} image=missing reason=disk_budget",
                            snapshot->burst_id, snapshot->frame);
                return;
            }
            // Reserve before any file operation, including failure paths. The
            // worker is serial; successful writes refund the unused PNG bound.
            budget->written_bytes += json.size() + max_png_bytes;
            std::filesystem::create_directories(pool->directory);
            slot->readback->buffer.InvalidateReadback();
            std::vector<u8> rgba;
            if (!ConvertReadbackToRgba8(*slot->readback, rgba) ||
                !WritePng(png_path, rgba, slot->readback->width, slot->readback->height)) {
                LOG_ERROR(Render_Vulkan, "LUMEN_GEN4_FRAME_LEDGER capture={} frame={} image=missing reason=png_write",
                          snapshot->burst_id, snapshot->frame);
                return;
            }
            std::ofstream file(json_path, std::ios::binary);
            file.write(json.data(), json.size());
            file.close();
            if (!file) {
                LOG_ERROR(Render_Vulkan, "LUMEN_GEN4_FRAME_LEDGER capture={} frame={} metadata=missing reason=json_write",
                          snapshot->burst_id, snapshot->frame);
                return;
            }
            budget->written_bytes -= max_png_bytes - std::min<u64>(max_png_bytes,
                std::filesystem::file_size(png_path));
            LOG_INFO(Render_Vulkan, "LUMEN_GEN4_FRAME_LEDGER capture={} frame={} saved={} bytes={}",
                     snapshot->burst_id, snapshot->frame, png_path.string(), budget->written_bytes);
        } catch (const std::exception& error) {
            LOG_ERROR(Render_Vulkan, "LUMEN_GEN4_FRAME_LEDGER capture={} frame={} save_error={}",
                      snapshot->burst_id, snapshot->frame, error.what());
        }
    });
}
#endif

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation =
        static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);

    fsr_pass.Create(instance, draw_scheduler.GetMasterSemaphore(), device,
                    instance.GetAllocator(), num_images);
    pp_pass.Create(instance, draw_scheduler.GetMasterSemaphore(),
                   swapchain.GetSurfaceFormat().format);

    ImGui::Layer::AddLayer(Common::Singleton<Core::Devtools::Layer>::Instance());
}

Presenter::~Presenter() {
    ImGui::Layer::RemoveLayer(Common::Singleton<Core::Devtools::Layer>::Instance());

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->imgui_texture) {
        ImGui::Vulkan::RemoveTexture(frame->imgui_texture);
    }
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->imgui_texture = ImGui::Vulkan::AddTexture(view, vk::ImageLayout::eShaderReadOnlyOptimal);
    frame->is_hdr = swapchain.GetHDR();
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            // Soft-fail: hard ASSERT became exit 133 on Mali after first game frame.
            // Returning the frame lets Present discover device-lost on acquire/present.
            LOG_CRITICAL(Render_Vulkan, "Device lost during waiting for a frame (PrepareLastFrame)");
            break;
        }
        LOG_ERROR(Render_Vulkan, "Unexpected waitForFences result in PrepareLastFrame: {}",
                  vk::to_string(result));
        break;
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetMasterSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
#ifdef ENABLE_LUMEN_HOST_RUNTIME
    // The embedded X11 server's DRI3 path exposes the guest frame with the opposite R/B
    // memory order from desktop WSI. Compensate in the sampled image view.
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    default:
        break;
    }
#endif
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
    const auto image_id = texture_cache.FindImage(desc);
    texture_cache.UpdateImage(image_id);

    Frame* frame = GetRenderFrame();
    if (!frame) {
        return nullptr;
    }

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    auto& image = texture_cache.GetImage(image_id);
    auto image_view = *image.FindView(view_info).image_view;
    const vk::Extent2D image_size = {image.info.size.width, image.info.size.height};
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    const u32 capture_game_only_count = VideoCore::ConsumeGameOnlyScreenshotRequests();
    std::vector<ScreenshotReadback> pending_screenshots;
    if (capture_game_only_count > 0) {
        pending_screenshots.reserve(1);
        const bool hdr_encoded =
            attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq;
        pending_screenshots.emplace_back(
            instance, draw_scheduler, ScreenshotKind::GameOnly,
            BuildScreenshotPaths(ScreenshotKind::GameOnly, capture_game_only_count),
            image_size.width, image_size.height, view_info.format, hdr_encoded);
        auto& readback = pending_screenshots.back();

        // Capture the guest output before any host-side scaling (FSR/PP) is applied.
        image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
                      cmdbuf);
        CopyImageToReadback(cmdbuf, image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                            readback);
    }

#ifdef ANDROID
    if (FrameLedger::Enabled()) {
        QueueFrameLedgerImage(instance, draw_scheduler, image, image_size, cpu_address, view_info.format,
            attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq,
            frame_ledger_readbacks);
    }
#endif

    // Continue with host-side passes that draw the displayed (scaled) frame.
    image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead, {},
                  cmdbuf);

    image_view = fsr_pass.Render(cmdbuf, image_view, image_size, {frame->width, frame->height},
                                 fsr_settings, frame->is_hdr);
    pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings);

    DebugState.game_resolution = {image_size.width, image_size.height};
    DebugState.output_resolution = {frame->width, frame->height};

    std::shared_ptr<std::vector<ScreenshotReadback>> deferred_screenshots{};
    if (!pending_screenshots.empty()) {
        deferred_screenshots =
            std::make_shared<std::vector<ScreenshotReadback>>(std::move(pending_screenshots));
        draw_scheduler.DeferPriorityOperation(
            [deferred_screenshots]() { SavePendingScreenshots(*deferred_screenshots); });
    }

    // Flush frame creation commands.
    frame->ready_semaphore = draw_scheduler.GetMasterSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);
    if (const auto capture = DrawDiagnostic::TakeFlip(cpu_address)) {
        const auto& d = *capture;
        using Outcome = DrawDiagnostic::Outcome;
        const auto count = [&](Outcome outcome) { return d.outcomes[static_cast<unsigned>(outcome)]; };
        LOG_INFO(Render_Vulkan,
                 "LUMEN_GEN4_DRAW_WINDOW flip={}/{} first_interval_partial={} scanout={:#x} "
                 "entries={} indexed={} indirect={} emitted_commands={} pipeline_null={} "
                 "bind_failed={} fast_clear={} fmask={} resolve={} primitive_none={} "
                 "depth_stencil_copy={} zero_count={} zero_instances={} last_primitive={} "
                 "last_color_mode={} last_target_mask={:#x} targets={} target_overflow={} null_gpu={}",
                 d.flip, DrawDiagnostic::MaxFlips, d.flip == 1, d.scanout,
                 d.entries, d.indexed, d.indirect, count(Outcome::Emitted),
                 count(Outcome::PipelineNull), count(Outcome::BindResourcesFailed),
                 count(Outcome::FastClear), count(Outcome::FmaskDecompress), count(Outcome::Resolve),
                 count(Outcome::PrimitiveNone), count(Outcome::DepthStencilCopy),
                 d.zero_count, d.zero_instances, d.last_primitive, d.last_color_mode,
                 d.last_target_mask, d.target_count, d.target_overflow, EmulatorSettings.IsNullGPU());
        for (unsigned i = 0; i < d.publications.size(); ++i) {
            const auto& p = d.publications[i];
            if (p.ends == 0 && p.scans == 0) continue;
            const bool packet = i < DrawDiagnostic::PacketSites * DrawDiagnostic::OpcodeCount;
            const unsigned site = packet ? i / DrawDiagnostic::OpcodeCount
                : i - DrawDiagnostic::PacketSites * DrawDiagnostic::OpcodeCount + DrawDiagnostic::PacketSites;
            const unsigned opcode = packet ? i % DrawDiagnostic::OpcodeCount : 0xff;
            LOG_INFO(Render_Vulkan,
                     "LUMEN_GEN4_PUBLICATION_WINDOW flip={} site={} opcode={:#x} ends={} "
                     "scans={} watched_pages={} watched_bytes={} last_draw_primitive={} "
                     "last_draw_pipeline={:#x} last_draw_vs={:#x} last_draw_ps={:#x}",
                     d.flip, DrawDiagnostic::PublicationSiteNames[site], opcode, p.ends,
                     p.scans, p.watched_pages, p.watched_pages * 4096, p.last_primitive,
                     p.last_pipeline, p.last_vs, p.last_ps);
        }
        for (unsigned kind = 0; kind < d.graphics_nops.size(); ++kind) {
            const auto& p = d.graphics_nops[kind];
            if (p.ends == 0 && p.scans == 0) continue;
            LOG_INFO(Render_Vulkan,
                     "LUMEN_GEN4_PUBLICATION_NOP flip={} kind={} subset=true ends={} scans={} "
                     "watched_pages={} watched_bytes={}",
                     d.flip, DrawDiagnostic::NopKindNames[kind], p.ends, p.scans,
                     p.watched_pages, p.watched_pages * 4096);
        }
        for (unsigned i = 0; i < d.pipeline_count; ++i) {
            const auto& p = d.pipelines[i];
            LOG_INFO(Render_Vulkan,
                     "LUMEN_GEN4_DRAW_PIPELINE flip={} pipeline={:#x} vs={:#x} ps={:#x} "
                     "primitive={} entries={} emitted={} overflow={}",
                     d.flip, p.id, p.vs, p.ps, p.primitive, p.entries, p.emitted, d.pipeline_overflow);
        }
        for (unsigned i = 0; i < d.target_count; ++i) {
            const auto& target = d.targets[i];
            LOG_INFO(Render_Vulkan,
                     "LUMEN_GEN4_DRAW_TARGET flip={} address={:#x} scanout_match={} "
                     "slots={:#x} component_mask={:#x} entries={} emitted_commands={}",
                     d.flip, target.address, target.address == d.scanout, target.slots,
                     target.component_mask, target.entries, target.emitted);
        }
    }
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();
    if (!frame) {
        return nullptr;
    }

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetMasterSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

bool Presenter::Present(Frame* frame, bool is_reusing_frame) {
    static std::atomic_uint32_t present_traces{};
    const u32 trace_id = present_traces.fetch_add(1, std::memory_order_relaxed);

    // LumenHost diagnostics: dump the guest output buffer every N presents when
    // the file <UserDir>/auto_shot_every exists (device black-screen triage;
    // env vars cannot be injected into the app-spawned runtime).
    static const u32 shot_every = [] {
        std::error_code ec;
        const auto shot_flag =
            Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "auto_shot_every";
        if (std::filesystem::exists(shot_flag, ec)) {
            Common::FS::IOFile f(shot_flag, Common::FS::FileAccessMode::Read);
            std::array<char, 16> buf{};
            const auto n = f.ReadRaw<char>(buf.data(), buf.size() - 1);
            buf[n] = '\0';
            const u32 v = std::strtoul(buf.data(), nullptr, 10);
            LOG_INFO(Render_Vulkan, "LUMEN_HOST_AUTO_SHOT every={} presents", v);
            return v;
        }
        return 0u;
    }();
    if (shot_every > 0 && trace_id > 0 && trace_id % shot_every == 0) {
        VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
        VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::WithOverlays);
    }
    // Log first 64, then every 16th, always around suspected 32-present boundary.
    const bool trace = trace_id < 64 || (trace_id % 16u) == 0u ||
                       (trace_id >= 28 && trace_id <= 40);
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };

    // The in-game demo menu/controller use normal pacing. Reevaluate the
    // swapchain policy on benchmark start, completion or cancellation.
    swapchain.SetUnthrottledPresentation(Lumen::Gen4::SucciBenchmark::Uncapped());

    // Recreate the swapchain if the window was resized.
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

    if (!swapchain.AcquireNextImage()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            free_frame();
            return false;
        }
    }
    if (trace) {
        LOG_INFO(Render_Vulkan, "LUMEN_HOST_PRESENT_TRACE id={} stage=acquire_done", trace_id);
        LOG_INFO(Render_Vulkan,
                 "FRAME_SLOT_ACQUIRE presentId={} slot={} frameW={} frameH={} "
                 "swapchainFrameIndex={} swapchainImageCount={} readyTick={}",
                 trace_id, frame ? int(frame->id) : -1, frame ? frame->width : 0,
                 frame ? frame->height : 0, swapchain.GetFrameIndex(), swapchain.GetImageCount(),
                 frame ? frame->ready_tick : 0);
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    if (reset_result == vk::Result::eErrorDeviceLost) {
        LOG_CRITICAL(Render_Vulkan, "Device lost while resetting present done fence");
        static std::atomic<bool> device_lost_snapshot_logged{false};
        if (!device_lost_snapshot_logged.exchange(true)) {
            LOG_CRITICAL(Render_Vulkan,
                         "DEVICE_LOST_SNAPSHOT where=resetPresentFence presentId={} slot={}",
                         trace_id, frame ? int(frame->id) : -1);
        }
        free_frame();
        return false;
    }
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    ImGuiID dockId = ImGui::Core::NewFrame(is_reusing_frame);

    const vk::Image swapchain_image = swapchain.Image();
    const vk::ImageView swapchain_image_view = swapchain.ImageView();

    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();
    const u32 capture_with_overlays_count = VideoCore::ConsumeWithOverlaysScreenshotRequests();
    std::vector<ScreenshotReadback> pending_screenshots;
    if (capture_with_overlays_count > 0) {
        pending_screenshots.reserve(1);
    }

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        auto* profiler_ctx = instance.GetProfilerContext();
        TracyVkNamedZoneC(profiler_ctx, renderer_gpu_zone, cmdbuf, "Host frame",
                          MarkersPalette::GpuMarkerColor, profiler_ctx != nullptr);

        const vk::Extent2D extent = swapchain.GetExtent();
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };

        bool swapchain_copied_for_screenshot = false;

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                               vk::PipelineStageFlagBits::eColorAttachmentOutput,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barriers);

        { // Draw the game
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
            ImGui::SetNextWindowDockID(dockId, ImGuiCond_Once);
            if (ImGui::Begin("Display##game_display", nullptr, ImGuiWindowFlags_NoNav)) {
                auto game_texture = frame->imgui_texture;
                auto game_width = frame->width;
                auto game_height = frame->height;

                if (Libraries::SystemService::IsSplashVisible()) { // draw splash
                    if (!splash_img.has_value()) {
                        splash_img.emplace();
                        auto splash_path = Common::ElfInfo::Instance().GetSplashPath();
                        if (!splash_path.empty()) {
                            splash_img = ImGui::RefCountedTexture::DecodePngFile(splash_path);
                        }
                    }
                    if (auto& splash_image = this->splash_img.value()) {
                        auto [im_id, width, height] = splash_image.GetTexture();
                        game_texture = im_id;
                        game_width = width;
                        game_height = height;
                    }
                }

                ImVec2 contentArea = ImGui::GetContentRegionAvail();
                SetExpectedGameSize((s32)contentArea.x, (s32)contentArea.y);

                const auto imgRect =
                    FitImage(game_width, game_height, (s32)contentArea.x, (s32)contentArea.y);
                ImVec2 offset{
                    static_cast<float>(imgRect.offset.x),
                    static_cast<float>(imgRect.offset.y),
                };
                ImVec2 size{
                    static_cast<float>(imgRect.extent.width),
                    static_cast<float>(imgRect.extent.height),
                };

                ImGui::SetCursorPos(ImGui::GetCursorStartPos() + offset);
                ImGui::Image(game_texture, size);

                if (EmulatorSettings.IsNullGPU()) {
                    Core::Devtools::Layer::DrawNullGpuNotice();
                }
            }
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor();
        }
        ImGui::Core::Render(cmdbuf, swapchain_image_view, swapchain.GetExtent());

        if (capture_with_overlays_count > 0) {
            pending_screenshots.emplace_back(
                instance, scheduler, ScreenshotKind::WithOverlays,
                BuildScreenshotPaths(ScreenshotKind::WithOverlays, capture_with_overlays_count),
                extent.width, extent.height,
                swapchain.GetHDR() ? vk::Format::eA2B10G10R10UnormPack32
                                   : swapchain.GetSurfaceFormat().format,
                swapchain.GetHDR());
            auto& readback = pending_screenshots.back();

            const vk::ImageMemoryBarrier to_transfer{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_transfer);
            CopyImageToReadback(cmdbuf, swapchain_image, vk::ImageLayout::eTransferSrcOptimal,
                                readback);
            swapchain_copied_for_screenshot = true;
        }

        const vk::AccessFlags post_src_access_mask =
            swapchain_copied_for_screenshot ? vk::AccessFlagBits::eTransferRead
                                            : vk::AccessFlagBits::eColorAttachmentWrite;
        const vk::ImageLayout post_old_layout = swapchain_copied_for_screenshot
                                                    ? vk::ImageLayout::eTransferSrcOptimal
                                                    : vk::ImageLayout::eColorAttachmentOptimal;
        const vk::ImageMemoryBarrier post_barrier{
            .srcAccessMask = post_src_access_mask,
            .dstAccessMask = vk::AccessFlagBits::eNone,
            .oldLayout = post_old_layout,
            .newLayout = vk::ImageLayout::ePresentSrcKHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);

        if (profiler_ctx) {
            TracyVkCollect(profiler_ctx, cmdbuf);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.
    std::shared_ptr<std::vector<ScreenshotReadback>> deferred_screenshots{};
    if (!pending_screenshots.empty()) {
        deferred_screenshots =
            std::make_shared<std::vector<ScreenshotReadback>>(std::move(pending_screenshots));
        scheduler.DeferPriorityOperation(
            [deferred_screenshots]() { SavePendingScreenshots(*deferred_screenshots); });
    }

    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
    scheduler.Flush(info);
    if (trace) {
        LOG_INFO(Render_Vulkan, "LUMEN_HOST_PRESENT_TRACE id={} stage=flush_done", trace_id);
    }

    // Present to swapchain. Only an accepted present can count as a benchmark
    // frame; callers that redraw a cached frame do not feed the benchmark.
    bool presented = false;
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        presented = swapchain.Present();
        if (trace) {
            LOG_INFO(Render_Vulkan, "LUMEN_HOST_PRESENT_TRACE id={} stage=present_done ok={}", trace_id,
                     presented);
        }
        if (!presented) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
    }

    free_frame();
    if (!is_reusing_frame) {
        DebugState.IncFlipFrameNum();
    }
    return presented;
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    const bool nonblock =
        liverpool && liverpool->IsGpuThread() && liverpool->InGfxTask();
    {
        std::unique_lock lock{free_mutex};
        if (nonblock) {
            if (free_queue.empty()) {
                return nullptr;
            }
        } else {
            free_cv.wait(lock, [this] { return !free_queue.empty(); });
        }
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    static std::atomic_uint32_t get_frame_traces{};
    const u32 get_id = get_frame_traces.fetch_add(1, std::memory_order_relaxed);
    if (get_id < 64 || (get_id % 16u) == 0u || (get_id >= 28 && get_id <= 40)) {
        LOG_INFO(Render_Vulkan,
                 "GET_RENDER_FRAME_WAIT frameGetId={} slot={} poolSize={} freeRemaining={} "
                 "readyTick={} (before waitForFences)",
                 get_id, frame ? int(frame->id) : -1, present_frames.size(), free_queue.size(),
                 frame ? frame->ready_tick : 0);
    }

    const u64 timeout_ns = AmdGpu::PresentFenceTimeoutNs(
        liverpool && liverpool->IsGpuThread(), liverpool && liverpool->InGfxTask());
    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, timeout_ns);
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        if (result == vk::Result::eTimeout) {
            if (nonblock) {
                static std::atomic<u32> defer_count{};
                if (defer_count.fetch_add(1, std::memory_order_relaxed) % 100 == 0) {
                    LOG_WARNING(Render_Vulkan,
                                "GetRenderFrame: present fence busy on GpuComm; deferring flip");
                }
                std::scoped_lock relock{free_mutex};
                free_queue.push(frame);
                free_cv.notify_one();
                return nullptr;
            }
            continue;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            // Soft-fail on Mali/Vortek: ASSERT_MSG here was exit 133 after the first
            // real EOP flip. Return the frame so Present can soft-fail acquire/present
            // and the session can stop without an immediate trap.
            // Client-side snapshot header; server dumps live alloc map via
            // DEVICE_LOST_SNAPSHOT in liblumen_host_vortek_server (logcat LumenHost.Vortek.GpuTrack).
            static std::atomic<bool> device_lost_snapshot_logged{false};
            const u32 flip_num = DebugState.GetFrameNum();
            LOG_CRITICAL(Render_Vulkan, "Device lost during waiting for a frame (GetRenderFrame)");
            if (!device_lost_snapshot_logged.exchange(true)) {
                LOG_CRITICAL(Render_Vulkan,
                             "DEVICE_LOST_SNAPSHOT where=GetRenderFrame lastPresent={} "
                             "currentFrameId={} frameSize={}x{} hdr={} "
                             "flipFrameNum={} (server alloc dump in logcat tag "
                             "LumenHost.Vortek.GpuTrack)",
                             flip_num, frame ? int(frame->id) : -1, frame ? frame->width : 0,
                             frame ? frame->height : 0, frame && frame->is_hdr ? 1 : 0, flip_num);
            }
            break;
        }
        LOG_ERROR(Render_Vulkan, "Unexpected waitForFences result in GetRenderFrame: {}",
                  vk::to_string(result));
        break;
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan
