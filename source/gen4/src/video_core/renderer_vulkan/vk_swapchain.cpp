// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <limits>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "imgui/renderer/imgui_core.h"
#include "lumen/succi_benchmark.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/renderer_vulkan/vk_swapchain_policy.h"

#ifdef __ANDROID__
#include <SDL3/SDL_vulkan.h>
extern "C" void LumenGen4OnPresentedFrame();
#endif

namespace Vulkan {
namespace {
constexpr bool IsAndroidSwapchain =
#ifdef __ANDROID__
    true;
#else
    false;
#endif
bool RequiresRecreation(vk::Result result) {
    using SwapchainPolicy::Outcome;
    switch(result) {
    case vk::Result::eSuccess:return SwapchainPolicy::MustRecreate(IsAndroidSwapchain,Outcome::Success);
    case vk::Result::eSuboptimalKHR:return SwapchainPolicy::MustRecreate(IsAndroidSwapchain,Outcome::Suboptimal);
    case vk::Result::eErrorOutOfDateKHR:return SwapchainPolicy::MustRecreate(IsAndroidSwapchain,Outcome::OutOfDate);
    case vk::Result::eErrorSurfaceLostKHR:return SwapchainPolicy::MustRecreate(IsAndroidSwapchain,Outcome::SurfaceLost);
    case vk::Result::eErrorDeviceLost:return SwapchainPolicy::MustRecreate(IsAndroidSwapchain,Outcome::DeviceLost);
    default:return true;
    }
}
}

static constexpr vk::SurfaceFormatKHR SURFACE_FORMAT_HDR = {
    .format = vk::Format::eA2B10G10R10UnormPack32,
    .colorSpace = vk::ColorSpaceKHR::eHdr10St2084EXT,
};

Swapchain::Swapchain(const Instance& instance_, const Frontend::WindowSDL& window_)
    : instance{instance_}, window{window_}, surface{CreateSurface(instance.GetInstance(), window)} {
    std::fprintf(stderr, "LUMEN_HOST_SWAPCHAIN_FORMAT_ENTER\n");
    if (!FindPresentFormat()) {
        UNREACHABLE_MSG("Initial presentation surface has no supported format");
    }
    std::fprintf(stderr, "LUMEN_HOST_SWAPCHAIN_FORMAT_READY\n");
    FindPresentMode();
    std::fprintf(stderr, "LUMEN_HOST_SWAPCHAIN_MODE_READY\n");

    Create(window.GetWidth(), window.GetHeight());
    if (needs_recreation) {
        UNREACHABLE_MSG("Initial presentation swapchain is unavailable");
    }
    std::fprintf(stderr, "LUMEN_HOST_SWAPCHAIN_READY\n");
    std::fprintf(stderr, "LUMEN_HOST_IMGUI_ENTER\n");
    ImGui::Core::Initialize(instance, window, image_count, surface_format.format);
    std::fprintf(stderr, "LUMEN_HOST_IMGUI_READY\n");
}

Swapchain::~Swapchain() {
    Destroy();
    instance.GetInstance().destroySurfaceKHR(surface);
}

void Swapchain::Create(u32 width_, u32 height_) {
    width = width_;
    height = height_;
    // A failed recovery stays pending. AcquireNextImage must not index the
    // retired swapchain or its semaphores while Android has no native window.
    needs_recreation = true;

    if (needs_surface_recreation) {
        Destroy();
        if (surface) {
            instance.GetInstance().destroySurfaceKHR(surface);
            surface = nullptr;
        }
#ifdef __ANDROID__
        // SDL obtains the current ANativeWindow. The WindowSystemInfo captured
        // at startup may refer to a window retired during Android pause/resume.
        VkSurfaceKHR replacement{};
        if (!SDL_Vulkan_CreateSurface(window.GetSDLWindow(),
                                      static_cast<VkInstance>(instance.GetInstance()), nullptr,
                                      &replacement)) {
            LOG_DEBUG(Render_Vulkan, "Android surface recovery deferred: {}", SDL_GetError());
            return;
        }
        surface = replacement;
#else
        surface = CreateSurface(instance.GetInstance(), window);
#endif
        const auto [support_result, supported] = instance.GetPhysicalDevice().getSurfaceSupportKHR(
            instance.GetPresentQueueFamilyIndex(), surface);
        if (support_result != vk::Result::eSuccess || !supported) {
            LOG_WARNING(Render_Vulkan, "Replacement surface cannot use present queue: {} supported={}",
                        vk::to_string(support_result), bool(supported));
            return;
        }
        if (!FindPresentFormat()) {
            return;
        }
        FindPresentMode();
        needs_hdr &= supports_hdr;
        // A later capability/create failure can set this again. Keep the new
        // surface when only its extent is temporarily zero or out of date.
        needs_surface_recreation = false;
    }

    Destroy();
    if (!surface || !SetSurfaceProperties()) {
        return;
    }

    const std::array queue_family_indices = {
        instance.GetGraphicsQueueFamilyIndex(),
        instance.GetPresentQueueFamilyIndex(),
    };

    const bool exclusive = queue_family_indices[0] == queue_family_indices[1];
    const u32 queue_family_indices_count = exclusive ? 1u : 2u;
    const vk::SharingMode sharing_mode =
        exclusive ? vk::SharingMode::eExclusive : vk::SharingMode::eConcurrent;
    const auto format = needs_hdr ? SURFACE_FORMAT_HDR : surface_format;
    const vk::SwapchainCreateInfoKHR swapchain_info = {
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
                      vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .imageSharingMode = sharing_mode,
        .queueFamilyIndexCount = queue_family_indices_count,
        .pQueueFamilyIndices = queue_family_indices.data(),
        .preTransform = transform,
        .compositeAlpha = composite_alpha,
        .presentMode = present_mode,
        .clipped = true,
        .oldSwapchain = nullptr,
    };

    auto [swapchain_result, chain] = instance.GetDevice().createSwapchainKHR(swapchain_info);
    if (swapchain_result != vk::Result::eSuccess) {
        needs_surface_recreation |= swapchain_result == vk::Result::eErrorSurfaceLostKHR;
        LOG_WARNING(Render_Vulkan, "Swapchain recreation deferred: {}", vk::to_string(swapchain_result));
        return;
    }
    swapchain = chain;

    SetupImages();
    RefreshSemaphores();
    needs_recreation = false;
}

void Swapchain::Recreate(u32 width_, u32 height_) {
    LOG_DEBUG(Render_Vulkan, "Recreate the swapchain: width={} height={} HDR={}", width_, height_,
              needs_hdr);
    const auto previous_format = needs_hdr ? SURFACE_FORMAT_HDR.format : surface_format.format;
    Create(width_, height_);
    const auto current_format = needs_hdr ? SURFACE_FORMAT_HDR.format : surface_format.format;
    if (!needs_recreation && previous_format != current_format) {
        ImGui::Core::OnSurfaceFormatChange(needs_hdr ? SURFACE_FORMAT_HDR.format
                                                    : surface_format.format);
    }
}

void Swapchain::SetHDR(bool hdr) {
    if (needs_hdr == hdr) {
        return;
    }

    auto result = instance.GetDevice().waitIdle();
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(ImGui, "Failed to wait for Vulkan device idle on mode change: {}",
                    vk::to_string(result));
    }

    needs_hdr = hdr;
    Recreate(width, height);
    ImGui::Core::OnSurfaceFormatChange(needs_hdr ? SURFACE_FORMAT_HDR.format
                                                 : surface_format.format);
}

void Swapchain::SetUnthrottledPresentation(bool enabled) {
    if (unthrottled_presentation == enabled) return;
    unthrottled_presentation = enabled;
    const auto previous = present_mode;
    FindPresentMode();
    LOG_INFO(Render_Vulkan, "Presentation policy unthrottled={} mode={}", enabled,
             vk::to_string(present_mode));
    if (present_mode != previous) Recreate(width, height);
}

bool Swapchain::AcquireNextImage() {
    if (needs_recreation || !swapchain || image_acquired.empty()) {
        return false;
    }
    vk::Device device = instance.GetDevice();
    vk::Result result =
        device.acquireNextImageKHR(swapchain, std::numeric_limits<u64>::max(),
                                   image_acquired[frame_index], VK_NULL_HANDLE, &image_index);

    switch (result) {
    case vk::Result::eSuccess:
        break;
    case vk::Result::eSuboptimalKHR:
        needs_recreation = RequiresRecreation(result);
        break;
    case vk::Result::eErrorSurfaceLostKHR:
        needs_surface_recreation = true;
        needs_recreation = true;
        break;
    case vk::Result::eErrorOutOfDateKHR:
    case vk::Result::eErrorUnknown:
        needs_recreation = true;
        break;
    case vk::Result::eErrorDeviceLost:
        // Mali/Vortek can report device-lost after heavy first-frame GPU work. Do not
        // UNREACHABLE (exit 133); mark for recreation and let Present skip the frame so
        // session logs flush and the Android layer can observe a clean stop.
        LOG_CRITICAL(Render_Vulkan, "Swapchain acquire returned ErrorDeviceLost");
        {
            // Client-side gate; server dumps alloc map via present_sync / GpuTrack.
            static std::atomic<bool> device_lost_snapshot_logged{false};
            if (!device_lost_snapshot_logged.exchange(true)) {
                LOG_CRITICAL(Render_Vulkan,
                             "DEVICE_LOST_SNAPSHOT where=AcquireNextImage frame_index={} "
                             "image_count={} image_index={} (server dump: LumenHost.Vortek.GpuTrack)",
                             frame_index, image_count, image_index);
            }
        }
        needs_recreation = true;
        break;
    default:
        LOG_CRITICAL(Render_Vulkan, "Swapchain acquire returned unknown result {}",
                     vk::to_string(result));
        UNREACHABLE();
        break;
    }

    return !needs_recreation;
}

bool Swapchain::Present() {
    if (needs_recreation || !swapchain || present_ready.empty()) {
        return false;
    }

    const vk::PresentInfoKHR present_info = {
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &present_ready[image_index],
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &image_index,
    };

    auto result = instance.GetPresentQueue().presentKHR(present_info);
#ifdef __ANDROID__
    // Count only presentations accepted by the real swapchain, never guest
    // flips or draw submissions. Suboptimal still presents the current image.
    if (result == vk::Result::eSuccess || result == vk::Result::eSuboptimalKHR) {
        LumenGen4OnPresentedFrame();
    }
#endif
    if (RequiresRecreation(result) && (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR ||
        result == vk::Result::eErrorDeviceLost || result == vk::Result::eErrorSurfaceLostKHR)) {
        static std::atomic_uint32_t logged_recreation_reasons{};
        const u32 reason = result == vk::Result::eSuboptimalKHR ? 1u :
                           result == vk::Result::eErrorOutOfDateKHR ? 2u :
                           result == vk::Result::eErrorSurfaceLostKHR ? 4u : 8u;
        if (!(logged_recreation_reasons.fetch_or(reason, std::memory_order_relaxed) & reason)) {
            LOG_WARNING(Render_Vulkan,
                        "Swapchain present requires recreation: result={} frame_index={} "
                        "image_index={} extent={}x{}",
                        vk::to_string(result), frame_index, image_index, extent.width, extent.height);
        }
        if (result == vk::Result::eErrorDeviceLost) {
            LOG_CRITICAL(Render_Vulkan, "Swapchain present returned ErrorDeviceLost");
            static std::atomic<bool> device_lost_snapshot_logged{false};
            if (!device_lost_snapshot_logged.exchange(true)) {
                LOG_CRITICAL(Render_Vulkan,
                             "DEVICE_LOST_SNAPSHOT where=QueuePresent frame_index={} "
                             "image_count={} image_index={} (server dump: LumenHost.Vortek.GpuTrack)",
                             frame_index, image_count, image_index);
            }
        }
        needs_surface_recreation |= result == vk::Result::eErrorSurfaceLostKHR;
        needs_recreation = true;
    } else {
        ASSERT_MSG(result == vk::Result::eSuccess || result == vk::Result::eSuboptimalKHR,
                   "Swapchain presentation failed: {}",
                   vk::to_string(result));
    }

    frame_index = (frame_index + 1) % image_count;

    return !needs_recreation;
}

bool Swapchain::FindPresentFormat() {
    const auto [formats_result, formats] =
        instance.GetPhysicalDevice().getSurfaceFormatsKHR(surface);
    if (formats_result != vk::Result::eSuccess || formats.empty()) {
        needs_surface_recreation |= formats_result == vk::Result::eErrorSurfaceLostKHR;
        LOG_WARNING(Render_Vulkan, "No presentation surface formats: {}", vk::to_string(formats_result));
        return false;
    }

    // Check if the device supports HDR formats. Here we care of Rec.2020 PQ only as it is expected
    // game output. Other variants as e.g. linear Rec.2020 will require additional color space
    // rotation
    supports_hdr =
        std::find_if(formats.begin(), formats.end(), [](const vk::SurfaceFormatKHR& format) {
            return format == SURFACE_FORMAT_HDR;
        }) != formats.end();
    // Also make sure that user allowed us to use HDR
    supports_hdr &= EmulatorSettings.IsHdrAllowed();

    // If there is a single undefined surface format, the device doesn't care, so we'll just use
    // RGBA sRGB.
    if (formats[0].format == vk::Format::eUndefined) {
        surface_format.format = vk::Format::eR8G8B8A8Unorm;
        surface_format.colorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
        return true;
    }

    // Try to find a suitable format.
    for (const vk::SurfaceFormatKHR& sformat : formats) {
        vk::Format format = sformat.format;
        if (format != vk::Format::eR8G8B8A8Unorm && format != vk::Format::eB8G8R8A8Unorm) {
            continue;
        }

        surface_format.format = format;
        surface_format.colorSpace = sformat.colorSpace;
        return true;
    }

    LOG_ERROR(Render_Vulkan, "Unable to find required swapchain format");
    return false;
}

void Swapchain::FindPresentMode() {
    const auto [modes_result, modes] =
        instance.GetPhysicalDevice().getSurfacePresentModesKHR(surface);
    if (modes_result != vk::Result::eSuccess) {
        LOG_ERROR(Render, "Failed to query available present modes, falling back to Fifo as "
                          "guaranteed supported option.");
        present_mode = vk::PresentModeKHR::eFifo;
        Lumen::Gen4::SucciBenchmark::SetPresentMode("Fifo");
        return;
    }

    if (unthrottled_presentation) {
        // A temporary throughput measurement can prefer a nonblocking mode;
        // disabling it restores the user's ordinary present policy below.
        present_mode = vk::PresentModeKHR::eFifo;
        const char* mode_name = "Fifo";
        if (std::ranges::find(modes, vk::PresentModeKHR::eImmediate) != modes.cend()) {
            present_mode = vk::PresentModeKHR::eImmediate;
            mode_name = "Immediate";
        } else if (std::ranges::find(modes, vk::PresentModeKHR::eMailbox) != modes.cend()) {
            present_mode = vk::PresentModeKHR::eMailbox;
            mode_name = "Mailbox";
        }
        Lumen::Gen4::SucciBenchmark::SetPresentMode(mode_name);
        LOG_INFO(Render_Vulkan, "Succi benchmark present mode: {}", mode_name);
        return;
    }

    const auto requested_mode = EmulatorSettings.GetPresentMode();
    if (requested_mode == "Mailbox") {
        present_mode = vk::PresentModeKHR::eMailbox;
    } else if (requested_mode == "Fifo") {
        present_mode = vk::PresentModeKHR::eFifo;
    } else if (requested_mode == "Immediate") {
        present_mode = vk::PresentModeKHR::eImmediate;
    } else {
        LOG_ERROR(Render_Vulkan, "Unknown present mode {}, defaulting to Mailbox.",
                  EmulatorSettings.GetPresentMode());
        present_mode = vk::PresentModeKHR::eMailbox;
    }

    if (std::ranges::find(modes, present_mode) == modes.cend()) {
        // FIFO is guaranteed to be supported by the Vulkan spec.
        constexpr auto fallback = vk::PresentModeKHR::eFifo;
        LOG_WARNING(Render, "Requested present mode {} is not supported, falling back to {}.",
                    vk::to_string(present_mode), vk::to_string(fallback));
        present_mode = fallback;
    }
}

bool Swapchain::SetSurfaceProperties() {
    const auto [capabilities_result, capabilities] =
        instance.GetPhysicalDevice().getSurfaceCapabilitiesKHR(surface);
    if (capabilities_result != vk::Result::eSuccess) {
        needs_surface_recreation |= capabilities_result == vk::Result::eErrorSurfaceLostKHR;
        LOG_WARNING(Render_Vulkan, "Surface capabilities unavailable: {}", vk::to_string(capabilities_result));
        return false;
    }
    constexpr auto required_usage = vk::ImageUsageFlagBits::eColorAttachment |
                                    vk::ImageUsageFlagBits::eTransferSrc |
                                    vk::ImageUsageFlagBits::eTransferDst;
    if ((capabilities.supportedUsageFlags & required_usage) != required_usage) {
        LOG_ERROR(Render_Vulkan, "Surface lacks required presentation image usage");
        return false;
    }

    extent = capabilities.currentExtent;
    if (capabilities.currentExtent.width == std::numeric_limits<u32>::max()) {
        extent.width = std::max(capabilities.minImageExtent.width,
                                std::min(capabilities.maxImageExtent.width, width));
        extent.height = std::max(capabilities.minImageExtent.height,
                                 std::min(capabilities.maxImageExtent.height, height));
    } else {
        // A fixed surface extent may still reflect Android's initial inset
        // window. Track the size actually selected so the presenter's resize
        // check can detect the later fullscreen surface. For variable extents,
        // retain the request above to avoid recreating forever after clamping.
        width = extent.width;
        height = extent.height;
    }

    if (!extent.width || !extent.height) {
        return false;
    }

    // Select number of images in swap chain, we prefer one buffer in the background to work on
    image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0) {
        image_count = std::min(image_count, capabilities.maxImageCount);
    }

    // Prefer identity transform if possible
    transform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    if (!(capabilities.supportedTransforms & transform)) {
        transform = capabilities.currentTransform;
    }

    // Android surfaces may change their compositing support after recreation.
    for (const auto candidate : {vk::CompositeAlphaFlagBitsKHR::eOpaque,
                                 vk::CompositeAlphaFlagBitsKHR::ePreMultiplied,
                                 vk::CompositeAlphaFlagBitsKHR::ePostMultiplied,
                                 vk::CompositeAlphaFlagBitsKHR::eInherit}) {
        if (capabilities.supportedCompositeAlpha & candidate) {
            composite_alpha = candidate;
            return image_count != 0;
        }
    }
    return false;
}

void Swapchain::Destroy() {
    if (!swapchain && images_view.empty() && image_acquired.empty() && present_ready.empty()) {
        return;
    }
    vk::Device device = instance.GetDevice();
    const auto wait_result = device.waitIdle();
    if (wait_result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Failed to wait for device to become idle: {}",
                    vk::to_string(wait_result));
    }

    for (auto& image_view : images_view) {
        device.destroyImageView(image_view);
    }
    images_view.clear();

    if (swapchain) {
        device.destroySwapchainKHR(swapchain);
        swapchain = nullptr;
    }

    for (const auto& sem : image_acquired) {
        device.destroySemaphore(sem);
    }
    for (const auto& sem : present_ready) {
        device.destroySemaphore(sem);
    }

    image_acquired.clear();
    present_ready.clear();
    images.clear();
    image_count = 0;
    frame_index = 0;
    image_index = 0;
}

void Swapchain::RefreshSemaphores() {
    const vk::Device device = instance.GetDevice();
    image_acquired.resize(image_count);
    present_ready.resize(image_count);

    for (vk::Semaphore& semaphore : image_acquired) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create image acquired semaphore: {}",
                   vk::to_string(semaphore_result));
        semaphore = sem;
    }
    for (vk::Semaphore& semaphore : present_ready) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create present ready semaphore: {}", vk::to_string(semaphore_result));
        semaphore = sem;
    }

    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, image_acquired[i], "Swapchain Semaphore: image_acquired {}", i);
        SetObjectName(device, present_ready[i], "Swapchain Semaphore: present_ready {}", i);
    }
}

void Swapchain::SetupImages() {
    vk::Device device = instance.GetDevice();
    auto [images_result, imgs] = device.getSwapchainImagesKHR(swapchain);
    ASSERT_MSG(images_result == vk::Result::eSuccess, "Failed to create swapchain images: {}",
               vk::to_string(images_result));
    images = std::move(imgs);
    image_count = static_cast<u32>(images.size());
    images_view.resize(image_count);
    for (u32 i = 0; i < image_count; ++i) {
        if (images_view[i]) {
            device.destroyImageView(images_view[i]);
        }
        auto [im_view_result, im_view] = device.createImageView(vk::ImageViewCreateInfo{
            .image = images[i],
            .viewType = vk::ImageViewType::e2D,
            .format = needs_hdr ? SURFACE_FORMAT_HDR.format : surface_format.format,
            .subresourceRange =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        });
        ASSERT_MSG(im_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
                   vk::to_string(im_view_result));
        images_view[i] = im_view;
    }

    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, images[i], "Swapchain Image {}", i);
        SetObjectName(device, images_view[i], "Swapchain ImageView {}", i);
    }
}

} // namespace Vulkan
