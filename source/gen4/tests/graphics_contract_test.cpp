// SPDX-License-Identifier: GPL-2.0-only
#include "common/logging/log.h"
#include "common/elf_info.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/emulator_state.h"
#include "core/user_settings.h"
#include "core/memory.h"
#include "core/libraries/libpng/pngenc.h"
#include "core/libraries/libpng/pngdec.h"
#include <algorithm>
#include <xxhash.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/threads/thread_state.h"
#include "input/controller.h"
#include "imgui/renderer/imgui_core.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_driver_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/texture_cache/blit_helper.h"
#include <cstdio>
#include <filesystem>
#include <memory>
#include <semaphore>
#include <thread>
#include <unistd.h>

namespace Core {
// Reuse the existing metadata fixture access without loading a guest image.
class PthreadWaitContractTest final {
public:
    static void InitializeBootMetadata() {
        auto& info = Common::ElfInfo::Instance();
        info.game_serial = "CUSA00000";
        info.title = "Lumen graphics contract";
        info.sdk_ver = Common::ElfInfo::FW_900;
        info.initialized = true;
    }
};
} // namespace Core

static bool CheckStackRangeIsolation(Core::MemoryManager& memory) {
#if defined(__APPLE__) && defined(LUMEN_GEN4_DIRECT_FEX)
    constexpr u64 Payload = 0x40000;
    constexpr u64 Guard = 0x4000;
    auto* state = Libraries::Kernel::ThrState::Instance();
    Libraries::Kernel::PthreadAttr first{};
    first.stacksize_attr = Payload;
    first.guardsize_attr = Guard;
    if (state->CreateStack(&first) != 0) return false;
    const auto first_base = reinterpret_cast<VAddr>(first.stackaddr_attr);
    void* begin{};
    void* end{};
    u32 protection{};
    const auto query_first = [&] {
        return memory.QueryProtection(first_base + Payload - sizeof(u64), &begin, &end,
                                       &protection) == 0 &&
               reinterpret_cast<VAddr>(begin) == first_base &&
               reinterpret_cast<VAddr>(end) == first_base + Payload &&
               protection == static_cast<u32>(Core::MemoryProt::CpuReadWrite);
    };
    if (!query_first()) return false;

    // Hold the exact pre-guard allocation shape deterministically: a second
    // real CreateStack with a guardless 0x44000 payload has the same writable
    // mapping as a neighboring 0x40000 stack before its guard is protected.
    // The first thread's FEX request must never expand across that neighbor.
    Libraries::Kernel::PthreadAttr second{};
    second.stacksize_attr = Payload + Guard;
    if (state->CreateStack(&second) != 0) return false;
    const auto second_base = reinterpret_cast<VAddr>(second.stackaddr_attr);
    if (second_base != first_base + Payload || !query_first()) return false;
    if (memory.Protect(second_base, Guard, Core::MemoryProt::NoAccess) != 0 ||
        !query_first()) return false;
    if (memory.QueryProtection(second_base, &begin, &end, &protection) != 0 ||
        protection != static_cast<u32>(Core::MemoryProt::NoAccess) ||
        reinterpret_cast<VAddr>(end) != second_base + Guard) return false;
    if (memory.QueryProtection(first_base - Guard, &begin, &end, &protection) != 0 ||
        protection != static_cast<u32>(Core::MemoryProt::NoAccess) ||
        reinterpret_cast<VAddr>(end) != first_base) return false;
    if (memory.UnmapMemory(first_base - Guard, Payload + Guard) != 0 ||
        memory.UnmapMemory(second_base, Payload + Guard) != 0) return false;
    std::fprintf(stderr, "GRAPHICS_CONTRACT_STACK_RANGE_READY payload=0x40000 neighbor=0x44000 guards=2\n");
#endif
    return true;
}

static bool CheckPngGpuReadback(Core::MemoryManager& memory, AmdGpu::Liverpool& liverpool,
                                 Vulkan::Rasterizer& rasterizer) {
    namespace Enc = Libraries::PngEnc;
    namespace Dec = Libraries::PngDec;
    constexpr u64 Granule = 16_KB;
    constexpr u32 Bytes = 4096;
    constexpr u32 Pixel = 0x78563412;
    EmulatorSettings.SetReadbacksMode(GpuReadbacksMode::Disabled);
    void* mapping{};
    if (memory.MapMemory(&mapping, 0, Granule, Core::MemoryProt::CpuReadWrite |
                         Core::MemoryProt::GpuReadWrite, Core::MemoryMapFlags::NoFlags,
                         Core::VMAType::Flexible, "PNG explicit readback contract") != 0) return false;
    const auto base = reinterpret_cast<VAddr>(mapping);
    auto* words = static_cast<volatile u32*>(mapping);
    words[0] = 0;
    words[Bytes / sizeof(u32)] = 0xaabbccdd;
    auto& cache = rasterizer.GetBufferCache();
    liverpool.SendCommand<true>([&] {
        const auto [buffer, offset] = cache.ObtainBuffer(base, Bytes, true);
        buffer->Fill(offset, Bytes, Pixel);
    });
    // With global readbacks disabled, CPU bytes are intentionally still stale.
    // The trace-only boundary must leave both bytes and GPU ownership unchanged.
    if (words[0] != 0 || !cache.IsRegionGpuModified(base, Bytes)) return false;
    memory.ReadGpuMemory(base, Bytes, false, true);
    if (words[0] != 0 || !cache.IsRegionGpuModified(base, Bytes)) return false;

    if (setenv("LUMEN_PNG_CPU_READBACK", "1", 1) != 0) return false;
    alignas(8) std::array<u8, 16> enc_work{}, dec_work{};
    Enc::OrbisPngEncCreateParam create_enc{16, 0, 32, 5};
    Dec::OrbisPngDecCreateParam create_dec{12, 0, 32};
    Enc::OrbisPngEncHandle encoder{};
    Dec::OrbisPngDecHandle decoder{};
    if (Enc::scePngEncCreate(&create_enc, enc_work.data(), enc_work.size(), &encoder) != 0 ||
        Dec::scePngDecCreate(&create_dec, dec_work.data(), dec_work.size(), &decoder) != 0) return false;
    std::array<u8, 8192> png{};
    Enc::OrbisPngEncEncodeParam encode{static_cast<const u8*>(mapping), png.data(), Bytes,
        png.size(), 32, 32, 128, Enc::OrbisPngEncPixelFormat::R8G8B8A8,
        Enc::OrbisPngEncColorSpace::RGBA, 8, 0, 0, 1};
    const s32 encoded = Enc::scePngEncEncode(encoder, &encode, nullptr);
    if (encoded <= 0 || words[0] != Pixel || words[Bytes / sizeof(u32)] != 0xaabbccdd ||
        cache.IsRegionGpuModified(base, Bytes) ||
        EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled) return false;
    std::array<u32, Bytes / sizeof(u32)> pixels{};
    Dec::OrbisPngDecDecodeParam decode{png.data(), reinterpret_cast<u8*>(pixels.data()),
        static_cast<u32>(encoded), Bytes, Dec::OrbisPngDecPixelFormat::R8G8B8A8, 255, 0};
    if (Dec::scePngDecDecode(decoder, &decode, nullptr) != 0x00200020 ||
        !std::all_of(pixels.begin(), pixels.end(), [](u32 pixel) { return pixel == Pixel; })) return false;
    if (Enc::scePngEncDelete(encoder) != 0 || Dec::scePngDecDelete(decoder) != 0 ||
        memory.UnmapMemory(base, Granule) != 0) return false;
    unsetenv("LUMEN_PNG_CPU_READBACK");
    EmulatorSettings.SetReadbacksMode(GpuReadbacksMode::Precise);
    std::fprintf(stderr, "GRAPHICS_CONTRACT_PNG_READBACK_READY stale_without_readback=1 trace_unchanged=1 rgba_pixels=1024 neighbor_canary=1 global_readbacks_disabled=1\n");
    return true;
}

static bool CheckCpuBufferWriteWithoutOwnerWait(Core::MemoryManager& memory,
                                                AmdGpu::Liverpool& liverpool,
                                                Vulkan::Rasterizer& rasterizer) {
    constexpr u64 Bytes = 32_KB;
    constexpr u32 CpuValue = 0xcafebabe;
    EmulatorSettings.SetReadbacksMode(GpuReadbacksMode::Disabled);
    void* mapping{};
    if (memory.MapMemory(&mapping, 0, Bytes, Core::MemoryProt::CpuReadWrite |
                        Core::MemoryProt::GpuReadWrite, Core::MemoryMapFlags::NoFlags,
                        Core::VMAType::Flexible, "CPU buffer write fault contract") != 0) return false;
    const auto address = reinterpret_cast<VAddr>(mapping);
    auto& buffers = rasterizer.GetBufferCache();
    std::memset(mapping, 0, Bytes);
    liverpool.SendCommand<true>([&] {
        (void)buffers.ObtainBuffer(address, Bytes, false);
        // Production leaves a small upload in pending_cpu until this boundary;
        // only then is the CPU-owned buffer protected by a real write watch.
        buffers.ProcessPendingCpuWrites();
    });
    if (buffers.IsRegionCpuModified(address, sizeof(CpuValue))) {
        std::fprintf(stderr, "GRAPHICS_CONTRACT_CPU_BUFFER_FAULT_FAIL still_cpu_dirty_after_upload=1\n");
        return false;
    }

    // Hold the GPU owner at a known point: an ordinary CPU-owned/write-watched
    // buffer must invalidate without waiting for an unrelated owner command.
    std::binary_semaphore owner_parked{0}, release_owner{0}, cpu_written{0};
    liverpool.SendCommand<false>([&] {
        owner_parked.release();
        release_owner.acquire();
    });
    owner_parked.acquire();
    std::thread writer([&] {
        *static_cast<volatile u32*>(mapping) = CpuValue;
        cpu_written.release();
    });
    const bool completed_while_owner_parked = cpu_written.try_acquire_for(std::chrono::seconds{2});
    release_owner.release();
    writer.join();
    liverpool.SendCommand<true>([] {});
    const bool valid = completed_while_owner_parked &&
        *static_cast<volatile u32*>(mapping) == CpuValue &&
        buffers.IsRegionCpuModified(address, sizeof(CpuValue));
    if (!valid) std::fprintf(stderr, "GRAPHICS_CONTRACT_CPU_BUFFER_FAULT_FAIL owner_parked_completed=%d value=%x cpu_dirty=%d\n",
                            completed_while_owner_parked, *static_cast<volatile u32*>(mapping),
                            buffers.IsRegionCpuModified(address, sizeof(CpuValue)));
    if (memory.UnmapMemory(address, Bytes) != 0 || !valid) return false;
    EmulatorSettings.SetReadbacksMode(GpuReadbacksMode::Precise);
    std::fprintf(stderr, "GRAPHICS_CONTRACT_CPU_BUFFER_FAULT_READY owner_parked=1 completed_before_owner_release=1 cpu_dirty_after_real_store=1\n");
    return true;
}

static bool CheckPostWriteInvalidation(Core::MemoryManager& memory,
                                        AmdGpu::Liverpool& liverpool,
                                        Vulkan::Rasterizer& rasterizer) {
    constexpr u64 Bytes = 16_KB;
    constexpr u32 HostWrite = 0xcafebabe;
    EmulatorSettings.SetReadbacksMode(GpuReadbacksMode::Disabled);
    void* mapping{};
    if (memory.MapMemory(&mapping, 0, Bytes, Core::MemoryProt::CpuReadWrite |
                        Core::MemoryProt::GpuReadWrite, Core::MemoryMapFlags::NoFlags,
                        Core::VMAType::Flexible, "Post-write invalidation contract") != 0) return false;
    const auto address = reinterpret_cast<VAddr>(mapping);
    auto& buffers = rasterizer.GetBufferCache();
    liverpool.SendCommand<true>([&] {
        const auto [buffer, offset] = buffers.ObtainBuffer(address, 4096, true);
        buffer->Fill(offset, 4096, 0x11223344);
    });
    // Model an explicit host publication followed by invalidation, which is
    // distinct from a CPU store fault before the write has taken place.
    if (!buffers.IsRegionGpuModified(address, sizeof(HostWrite)) ||
        !memory.TryWriteBacking(mapping, &HostWrite, sizeof(HostWrite))) return false;
    memory.InvalidateMemory(address, sizeof(HostWrite));
    u32 observed{};
    memory.CopySparseMemoryFromBacking(address, reinterpret_cast<u8*>(&observed), sizeof(observed));
    if (observed != HostWrite) return false;
    // Retire the synthetic GPU dirty range before restoring global Precise
    // mode; mode transitions themselves are outside this fixture.
    liverpool.SendCommand<true>([&] { buffers.ReadMemoryDirect(address, 4096); });
    if (memory.UnmapMemory(address, Bytes) != 0) return false;
    EmulatorSettings.SetReadbacksMode(GpuReadbacksMode::Precise);
    std::fprintf(stderr, "GRAPHICS_CONTRACT_POST_WRITE_INVALIDATION_READY host_write_preserved=1 global_readbacks_disabled=1\n");
    return true;
}

static bool CheckImageCpuReadback(Core::MemoryManager& memory, AmdGpu::Liverpool& liverpool,
                                    Vulkan::Rasterizer& rasterizer) {
    constexpr u32 Canary = 0x2468ace0;
    constexpr u32 Red = 0xff0000ff;
    constexpr u32 Green = 0xff00ff00;
    auto& textures = rasterizer.GetTextureCache();
    VideoCore::TextureCache::ImageDesc desc{};
    desc.type = VideoCore::TextureCache::BindingType::RenderTarget;
    auto& info = desc.info;
    info.type = AmdGpu::ImageType::Color2D;
    info.size = {64, 64, 1};
    info.pitch = 128;
    info.num_bits = 32;
    info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    info.props.is_tiled = true;
    info.tile_mode = AmdGpu::TileMode::Display2DThin;
    info.array_mode = AmdGpu::GetArrayMode(info.tile_mode);
    info.UpdateSize();
    const auto mapping_size = Common::AlignUp(u64(info.guest_size) + 16_KB, 16_KB);
    void* mapping{};
    if (memory.MapMemory(&mapping, 0, mapping_size, Core::MemoryProt::CpuReadWrite |
                        Core::MemoryProt::GpuReadWrite, Core::MemoryMapFlags::NoFlags,
                        Core::VMAType::Flexible, "Image CPU readback contract") != 0) return false;
    const auto mapping_base = reinterpret_cast<VAddr>(mapping);
    // Exercise notifications beside both ends of an image sharing their pages.
    info.guest_address = mapping_base + 2048;
    std::fill_n(static_cast<u32*>(mapping), mapping_size / sizeof(u32), Canary);
    VideoCore::ImageId id{};
    const auto clear_gpu = [&](bool green) {
        auto& image = textures.GetImage(id);
        textures.UpdateImage(id);
        image.Clear(vk::ClearValue{.color = {.float32 =
                        std::array<float, 4>{green ? 0.f : 1.f, green ? 1.f : 0.f, 0.f, 1.f}}},
                    VideoCore::SubresourceRange{.base = {}, .extent = {1, 1}});
        image.flags |= VideoCore::ImageFlagBits::GpuModified;
        textures.TrackImageReadback(id);
    };
    EmulatorSettings.SetReadbacksMode(GpuReadbacksMode::Disabled);
    liverpool.SendCommand<true>([&] {
        id = textures.FindImage(desc, true);
        clear_gpu(false);
    });
    std::vector<u32> backing(info.guest_size / sizeof(u32));
    memory.CopySparseMemoryFromBacking(info.guest_address, reinterpret_cast<u8*>(backing.data()), info.guest_size);
    if (!std::all_of(backing.begin(), backing.end(), [](u32 value) { return value == Canary; }) ||
        !textures.GetImage(id).cpu_read_watched) return false;
    const auto track_begin = textures.GetImage(id).track_addr;
    const auto track_end = textures.GetImage(id).track_addr_end;
    memory.InvalidateMemory(info.guest_address - sizeof(u32), sizeof(u32));
    memory.InvalidateMemory(info.guest_address + info.guest_size, sizeof(u32));
    if (!textures.GetImage(id).cpu_read_watched ||
        textures.GetImage(id).track_addr != track_begin ||
        textures.GetImage(id).track_addr_end != track_end) return false;
    liverpool.SendCommand<true>([&] {
        rasterizer.GetBufferCache().ObtainBufferForImage(info.guest_address, info.guest_size);
    });
    if (!textures.GetImage(id).cpu_read_watched ||
        memory.HashSparseMemoryFromBacking(info.guest_address, info.guest_size) !=
            XXH3_64bits(backing.data(), info.guest_size)) return false;
    // This ordinary CPU load is the only operation requesting publication.
    const auto first = *reinterpret_cast<volatile u32*>(info.guest_address);
    memory.CopySparseMemoryFromBacking(info.guest_address, reinterpret_cast<u8*>(backing.data()), info.guest_size);
    const auto red_pixels = std::count(backing.begin(), backing.end(), Red);
    const auto untouched = std::count(backing.begin(), backing.end(), Canary);
    if (first != Red || red_pixels != 64 * 64 ||
        red_pixels + untouched != backing.size() || textures.GetImage(id).cpu_read_watched ||
        *reinterpret_cast<volatile u32*>(info.guest_address + info.guest_size) != Canary ||
        EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled) return false;
    // CPU partial writes must first preserve the other texels of a pending GPU
    // image, then retire its watches so later CPU stores are ordinary stores.
    // A registered image remains on the owner path even between read-watch
    // lifetimes. Re-arming after the previous CPU read must protect this store.
    if (textures.InvalidateMemory(info.guest_address, sizeof(u32), true)) return false;
    liverpool.SendCommand<true>([&] { clear_gpu(true); });
    if (textures.InvalidateMemory(info.guest_address, sizeof(u32), true) ||
        !textures.GetImage(id).cpu_read_watched) return false;
    *reinterpret_cast<volatile u32*>(info.guest_address) = Canary;
    memory.CopySparseMemoryFromBacking(info.guest_address, reinterpret_cast<u8*>(backing.data()), info.guest_size);
    if (std::count(backing.begin(), backing.end(), Green) != 64 * 64 - 1 ||
        textures.GetImage(id).cpu_read_watched) return false;
    if (memory.UnmapMemory(mapping_base, mapping_size) != 0) return false;
    EmulatorSettings.SetReadbacksMode(GpuReadbacksMode::Precise);
    std::fprintf(stderr, "GRAPHICS_CONTRACT_IMAGE_READBACK_READY tile=10 rgba_pixels=4096 padding_preserved=1 adjacent_notify_watches_preserved=1 bookkeeping_no_readback=1 lazy_cpu_load=1 partial_write=1 registered_unwatched_image_rejected=1 rearm_partial_write=1 watched_fast_invalidate_rejected=1 global_readbacks_disabled=1\n");
    return true;
}

static bool CheckLargeLinearImageCpuReadback(Core::MemoryManager& memory,
                                            AmdGpu::Liverpool& liverpool,
                                            Vulkan::Rasterizer& rasterizer) {
    constexpr u32 Width = 8192;
    constexpr u32 Height = 4096;
    constexpr u64 Bytes = u64(Width) * Height * sizeof(u32);
    constexpr u32 Canary = 0x2468ace0;
    constexpr u32 Green = 0xff00ff00;
    auto& textures = rasterizer.GetTextureCache();
    VideoCore::TextureCache::ImageDesc desc{};
    desc.type = VideoCore::TextureCache::BindingType::RenderTarget;
    auto& info = desc.info;
    info.type = AmdGpu::ImageType::Color2D;
    info.size = {Width, Height, 1};
    info.pitch = Width;
    info.num_bits = 32;
    info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    info.props.is_tiled = false;
    info.tile_mode = AmdGpu::TileMode::DisplayLinearGeneral;
    info.array_mode = AmdGpu::GetArrayMode(info.tile_mode);
    info.UpdateSize();
    if (info.guest_size != Bytes || info.mips_layout[0].pitch != Width ||
        info.mips_layout[0].height != Height || info.mips_layout[0].offset != 0 ||
        info.IsTiled()) return false;
    void* mapping{};
    if (memory.MapMemory(&mapping, 0, Bytes + 16_KB, Core::MemoryProt::CpuReadWrite |
                        Core::MemoryProt::GpuReadWrite, Core::MemoryMapFlags::NoFlags,
                        Core::VMAType::Flexible, "Large linear CPU readback contract") != 0)
        return false;
    info.guest_address = reinterpret_cast<VAddr>(mapping);
    auto* words = static_cast<u32*>(mapping);
    std::fill_n(words, (Bytes + 16_KB) / sizeof(u32), Canary);
    const auto initial_hash = XXH3_64bits(mapping, Bytes);
    EmulatorSettings.SetReadbacksMode(GpuReadbacksMode::Disabled);
    VideoCore::ImageId id{};
    liverpool.SendCommand<true>([&] {
        id = textures.FindImage(desc, true);
        textures.UpdateImage(id);
        auto& image = textures.GetImage(id);
        image.Clear(vk::ClearValue{.color = {.float32 =
                        std::array<float, 4>{0.f, 1.f, 0.f, 1.f}}},
                    VideoCore::SubresourceRange{.base = {}, .extent = {1, 1}});
        image.flags |= VideoCore::ImageFlagBits::GpuModified;
        textures.TrackImageReadback(id);
    });
    // The renderer has the green image, while the complete physical backing
    // remains unchanged until an ordinary CPU access requests publication.
    if (!textures.GetImage(id).cpu_read_watched ||
        memory.HashSparseMemoryFromBacking(info.guest_address, Bytes) != initial_hash)
        return false;
    const auto first = *static_cast<volatile u32*>(mapping);
    const bool all_green = std::all_of(words, words + Bytes / sizeof(u32),
                                     [](u32 value) { return value == Green; });
    const bool guard_intact = std::all_of(words + Bytes / sizeof(u32),
                                         words + (Bytes + 16_KB) / sizeof(u32),
                                         [](u32 value) { return value == Canary; });
    if (first != Green || !all_green || !guard_intact ||
        textures.GetImage(id).cpu_read_watched ||
        EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled) return false;
    if (memory.UnmapMemory(info.guest_address, Bytes + 16_KB) != 0) return false;
    EmulatorSettings.SetReadbacksMode(GpuReadbacksMode::Precise);
    std::fprintf(stderr, "GRAPHICS_CONTRACT_LARGE_LINEAR_READBACK_READY bytes=134217728 width=8192 height=4096 all_pixels=33554432 stale_before_cpu_load=1 guard16k_intact=1 global_readbacks_disabled=1\n");
    return true;
}

static bool CheckNativeFramePublication(Core::MemoryManager& memory, AmdGpu::Liverpool& liverpool,
                                         Vulkan::Rasterizer& rasterizer) {
    constexpr u64 Granule = 16_KB;
    constexpr u32 Bytes = 8192;
    void* mapping{};
    if (memory.MapMemory(&mapping, 0, Granule, Core::MemoryProt::CpuReadWrite |
                        Core::MemoryProt::GpuReadWrite, Core::MemoryMapFlags::NoFlags,
                        Core::VMAType::Flexible, "Native frame publication contract") != 0) return false;
    const auto base = reinterpret_cast<VAddr>(mapping);
    auto* pixels = static_cast<u8*>(mapping);
    std::memset(pixels, 0x3c, Bytes);
    auto& cache = rasterizer.GetBufferCache();
    std::thread::id owner;
    liverpool.SendCommand<true>([&] {
        owner = std::this_thread::get_id();
        // A real GPU-owned/write-watched destination exercises reentry when
        // the first CPU store faults inside the publication callback.
        const auto [buffer, offset] = cache.ObtainBuffer(base, Bytes, true);
        buffer->Fill(offset, Bytes, 0x11223344);
    });
    if (!cache.IsRegionGpuModified(base, Bytes)) return false;

    std::binary_semaphore half_written{0}, finish_write{0};
    bool writer_on_owner{}, reader_after_chroma{}, reader_saw_dirty{}, intact{};
    bool chroma_complete{};
    std::thread producer([&] {
        memory.PublishGpuMemory(base, Bytes, [&] {
            writer_on_owner = std::this_thread::get_id() == owner;
            std::memset(pixels, 0, Bytes);
            std::memset(pixels, 0x61, Bytes / 2);
            half_written.release();
            finish_write.acquire();
            std::memset(pixels + Bytes / 2, 0xb7, Bytes / 2);
            chroma_complete = true;
        });
    });
    half_written.acquire();
    if (!writer_on_owner) {
        // Fail deterministically if publication reverts to a producer-thread
        // write; do not create an intentional C++ read/write data race.
        finish_write.release();
        producer.join();
        memory.UnmapMemory(base, Granule);
        return false;
    }
    // Queue the real renderer's next upload while the native producer has
    // luma but zero chroma. It must not execute in the middle of publication.
    liverpool.SendCommand<false>([&] {
        reader_after_chroma = chroma_complete;
        reader_saw_dirty = cache.IsRegionCpuModified(base, Bytes);
        cache.ObtainBufferForImage(base, Bytes);
        std::array<u8, Bytes> bytes{};
        memory.CopySparseMemoryFromBacking(base, bytes.data(), bytes.size());
        intact = std::all_of(bytes.begin(), bytes.begin() + Bytes / 2,
                            [](u8 value) { return value == 0x61; }) &&
                 std::all_of(bytes.begin() + Bytes / 2, bytes.end(),
                            [](u8 value) { return value == 0xb7; });
    });
    finish_write.release();
    producer.join();
    liverpool.SendCommand<true>([] {});
    const bool valid = writer_on_owner && reader_after_chroma && reader_saw_dirty && intact;
    if (memory.UnmapMemory(base, Granule) != 0 || !valid) return false;
    std::fprintf(stderr, "GRAPHICS_CONTRACT_NATIVE_FRAME_PUBLICATION_READY owner=1 ordered_planes=1 write_watch_reentry=1 upload_after_invalidate=1\n");
    return true;
}

static bool CheckGpuPageWatches(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler) {
#if defined(__APPLE__) && defined(LUMEN_GEN4_DIRECT_FEX)
    constexpr u64 Granule = 16 * 1024;
    constexpr u64 Subpage = 4096;
    if (setenv("LUMEN_IMAGE_CPU_READBACK", "1", 1) != 0) return false;
    const auto saved_readbacks = EmulatorSettings.GetReadbacksMode();
    EmulatorSettings.SetReadbacksMode(GpuReadbacksMode::Precise);
    AmdGpu::Liverpool liverpool;
    Vulkan::Rasterizer rasterizer(instance, scheduler, &liverpool);
    auto* memory = Core::Memory::Instance();
    void* mapping{};
    if (memory->MapMemory(&mapping, 0, Granule, Core::MemoryProt::CpuReadWrite |
                         Core::MemoryProt::GpuReadWrite, Core::MemoryMapFlags::NoFlags,
                         Core::VMAType::Flexible, "GPU host-page contract") != 0) return false;
    const auto base = reinterpret_cast<VAddr>(mapping);
    auto* words = static_cast<volatile u32*>(mapping);
    auto& cache = rasterizer.GetBufferCache();
    const auto fill_gpu = [&](VAddr address, u32 value) {
        // Establish GPU ownership first: FillBuffer alone deliberately uses
        // a CPU shortcut for a range that has not reached the GPU yet.
        const auto [buffer, offset] = cache.ObtainBuffer(address, Subpage, true);
        buffer->Fill(offset, Subpage, value);
    };
    words[Subpage / sizeof(u32)] = 0x12345678;
    liverpool.SendCommand<true>([&] {
        fill_gpu(base, 0x11223344);
        fill_gpu(base + 2 * Subpage, 0x55667788);
    });
    if (!cache.IsRegionGpuModified(base, Subpage) ||
        !cache.IsRegionGpuModified(base + 2 * Subpage, Subpage)) return false;

    // This untracked neighboring subpage shares the same native protection.
    // Its read must drain both real GPU writes before resuming the instruction.
    const u32 neighbor = words[Subpage / sizeof(u32)];
    if (neighbor != 0x12345678 || words[0] != 0x11223344 ||
        words[2 * Subpage / sizeof(u32)] != 0x55667788 ||
        cache.IsRegionGpuModified(base, Subpage) ||
        cache.IsRegionGpuModified(base + 2 * Subpage, Subpage)) return false;
    words[Subpage / sizeof(u32)] = 0xabcdef01;
    if (!cache.IsRegionCpuModified(base, Subpage) ||
        !cache.IsRegionCpuModified(base + 2 * Subpage, Subpage)) return false;

    // Unmap while a fresh GPU read watch is active. The normal VM/cache
    // teardown must finish readback and retire protection without a snapshot.
    liverpool.SendCommand<true>([&] {
        fill_gpu(base, 0x778899aa);
    });
    if (memory->UnmapMemory(base, Granule) != 0) return false;

    if (!CheckPngGpuReadback(*memory, liverpool, rasterizer) ||
        !CheckCpuBufferWriteWithoutOwnerWait(*memory, liverpool, rasterizer) ||
        !CheckImageCpuReadback(*memory, liverpool, rasterizer) ||
        !CheckLargeLinearImageCpuReadback(*memory, liverpool, rasterizer) ||
        !CheckPostWriteInvalidation(*memory, liverpool, rasterizer) ||
        !CheckNativeFramePublication(*memory, liverpool, rasterizer)) return false;

    // A sparse PRT range can already have a GPU write watch when a pool
    // commit supplies its physical pages. The real OnMap notification must
    // arm that previously unavailable host page before the CPU reads it.
    constexpr u64 PoolSize = 64 * 1024;
    const auto physical = memory->PoolExpand(PoolSize, memory->GetTotalDirectSize(),
                                             2 * PoolSize, PoolSize);
    if (physical == static_cast<PAddr>(-1)) return false;
    void* pooled_mapping{};
    if (memory->MapMemory(&pooled_mapping, 0, PoolSize, Core::MemoryProt::NoAccess,
                         Core::MemoryMapFlags::NoFlags, Core::VMAType::PoolReserved,
                         "GPU sparse-page contract", false, -1, PoolSize) != 0) return false;
    const auto pooled = reinterpret_cast<VAddr>(pooled_mapping);
    memory->SetPrtArea(0, pooled, PoolSize);
    liverpool.SendCommand<true>([&] { fill_gpu(pooled, 0x99887766); });
    if (!cache.IsRegionGpuModified(pooled, Subpage)) return false;
    if (memory->PoolCommit(pooled, PoolSize, Core::MemoryProt::CpuReadWrite |
                           Core::MemoryProt::GpuReadWrite, 3) != 0) return false;
    const auto committed = *static_cast<volatile u32*>(pooled_mapping);
    if (committed != 0x99887766 || cache.IsRegionGpuModified(pooled, Subpage)) return false;
    if (memory->PoolDecommit(pooled, PoolSize) != 0 ||
        memory->UnmapMemory(pooled, PoolSize) != 0) return false;
    memory->SetPrtArea(0, 0, 0);
    if (memory->Free(physical, 2 * PoolSize, false) != 0) return false;
    if (!CheckStackRangeIsolation(*memory)) return false;
    scheduler.Finish();
    EmulatorSettings.SetReadbacksMode(saved_readbacks);
    std::fprintf(stderr, "GRAPHICS_CONTRACT_HOST_PAGE_WATCH_READY readback=2 invalidation=2 unmap=1 sparse_rearm=1\n");
#endif
    return true;
}

int main() {
    std::fprintf(stderr,"GRAPHICS_CONTRACT_START\n");
    auto path = std::filesystem::temp_directory_path() / ("lumen-graphics-contract-" + std::to_string(getpid()));
    Common::FS::InitializeUserPaths(path);
    std::fprintf(stderr,"GRAPHICS_CONTRACT_PATHS\n");
    EmulatorState::SetInstance(std::make_shared<EmulatorState>());
    EmulatorSettingsImpl::SetInstance(std::make_shared<EmulatorSettingsImpl>());
    Core::PthreadWaitContractTest::InitializeBootMetadata();
    Common::Log::Setup("graphics-contract.log");
    std::fprintf(stderr,"GRAPHICS_CONTRACT_LOG\n");
    UserSettings.Load();
    for (auto& [name, logger] : Common::Log::ALL_LOGGERS) logger->set_level(spdlog::level::info);

    {
        Input::GameControllers controllers;
        Frontend::WindowSDL window(320,240,&controllers,"Lumen graphics contract","graphics-contract");
        Vulkan::Instance instance(window,-1,false,false);
        std::fprintf(stderr,"GRAPHICS_CONTRACT_DEVICE_READY\n");
        EmulatorSettings.SetPipelineCacheEnabled(true);
        {
            Vulkan::DriverPipelineCache cache(instance);
            {
                auto lock = cache.Lock();
                size_t size{};
                if (!*cache || instance.GetDevice().getPipelineCacheData(*cache, &size, nullptr) != vk::Result::eSuccess ||
                    size < Vulkan::DriverCacheStorage::HeaderSize) return 5;
            }
            cache.MarkDirty();
            cache.Stop(); // Joins the worker after its pending production checkpoint.
        }
        bool checkpointFound = false;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 Common::FS::GetUserPath(Common::FS::PathType::CacheDir))) {
            if (entry.is_regular_file() && entry.path().extension() == ".bin" &&
                entry.path().parent_path().filename() == "driver") checkpointFound = true;
        }
        if (!checkpointFound) return 6;
        {
            Vulkan::DriverPipelineCache restoredCache(instance);
            auto lock = restoredCache.Lock();
            if (!*restoredCache) return 7;
        }
        std::fprintf(stderr,"GRAPHICS_CONTRACT_PIPELINE_CACHE_READY\n");
        Vulkan::Scheduler scheduler(instance);
        if (!CheckGpuPageWatches(instance, scheduler)) return 8;
        VideoCore::BlitHelper blit(instance,scheduler);
        Vulkan::Swapchain swapchain(instance,window);
        std::fprintf(stderr,"GRAPHICS_CONTRACT_SWAPCHAIN_READY\n");
        Vulkan::HostPasses::FsrPass fsr;
        fsr.Create(instance, scheduler.GetMasterSemaphore(), instance.GetDevice(),
                   instance.GetAllocator(), swapchain.GetImageCount());
        std::fprintf(stderr,"GRAPHICS_CONTRACT_FSR_READY\n");
        Vulkan::HostPasses::PostProcessingPass postProcessing;
        postProcessing.Create(instance, scheduler.GetMasterSemaphore(), swapchain.GetSurfaceFormat().format);
        std::fprintf(stderr,"GRAPHICS_CONTRACT_POST_PROCESSING_READY\n");
        // Swapchain already initialized the ImGui context and renderer.
        for (unsigned frame=0;frame<2;++frame) {
            if (!swapchain.AcquireNextImage()) return 3;
            ImGui::Core::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(10,10),ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(250,120),ImGuiCond_Always);
            ImGui::Begin("Graphics contract");
            ImGui::Text("Presentation frame %u",frame);
            ImGui::End();
            const auto command=scheduler.CommandBuffer();
            // Match Present's legacy barriers and real ImGui dynamic rendering.
            vk::ImageMemoryBarrier barrier{
                .dstAccessMask=vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout=vk::ImageLayout::eUndefined,
                .newLayout=vk::ImageLayout::eColorAttachmentOptimal,
                .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                .image=swapchain.Image(),
                .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,VK_REMAINING_ARRAY_LAYERS},
            };
            command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                    vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                    vk::DependencyFlagBits::eByRegion,{}, {},barrier);
            ImGui::Core::Render(command,swapchain.ImageView(),swapchain.GetExtent());
            barrier.srcAccessMask=vk::AccessFlagBits::eColorAttachmentWrite;
            barrier.dstAccessMask={};
            barrier.oldLayout=vk::ImageLayout::eColorAttachmentOptimal;
            barrier.newLayout=vk::ImageLayout::ePresentSrcKHR;
            command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                    vk::PipelineStageFlagBits::eAllCommands,
                                    vk::DependencyFlagBits::eByRegion,{}, {},barrier);
            Vulkan::SubmitInfo submit{};submit.AddWait(swapchain.GetImageAcquiredSemaphore());submit.AddSignal(swapchain.GetPresentReadySemaphore());
            scheduler.Flush(submit);
            if (!swapchain.Present()) return 4;
            scheduler.Finish();
        }
        ImGui::Core::Shutdown(instance.GetDevice());
        std::fprintf(stderr,"GRAPHICS_CONTRACT_PRESENT_READY\n");
    }
    UserSettingsImpl::SetInstance(nullptr);
    Common::Log::Shutdown();
    std::filesystem::remove_all(path);
    std::puts("Full renderer instance, driver pipeline cache checkpoint/reload, VMA, scheduler, compiled host shaders, swapchain and real presentation PASS");
    return 0;
}
