// Lumen, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#include "loader/guestExecution.h"
#include "loader/guestWriterGate.h"

#if defined(LUMEN_PS5_FEXCORE)
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/guestBackingPolicy.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvBuilder.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/window.h"
#include "kernel/memory.h"
#include "libs/errno.h"

#include <array>
#include <atomic>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <tuple>
#include <vector>

namespace Libs::Graphics {
struct BufferCacheTestAccess {
    static void Revalidate(BufferCache& cache, uint64_t address, uint64_t size) {
        cache.RevalidateBacking(cache.GetBuffer(cache.FindBuffer(address, size)), address, size);
    }
    static BackingComparison Compare(BufferCache& cache, uint64_t address,
                                      const void* bytes, size_t size) {
        return cache.m_backing_snapshots.Compare(address, bytes, size);
    }
    static uint64_t ContiguousHashPages(const BufferCache& cache) {
        return cache.m_contiguous_hash_pages;
    }
    static std::array<uint64_t, 4> PublicationWitnessStats(const BufferCache& cache) {
        return {cache.m_publication_witness_pages, cache.m_publication_witness_cpu_dirty_pages,
            cache.m_publication_witness_upload_avoided_bytes, cache.m_guest_upload_bytes};
    }
    static bool HasPublicationWitness(BufferCache& cache, uint64_t address) {
        return cache.m_backing_snapshots.IsPublicationWitness(address, TRACKER_PAGE_SIZE);
    }
    static void Retire(BufferCache& cache, uint64_t address, uint64_t size) {
        cache.DeleteBuffer(cache.FindBuffer(address, size));
    }
    static uint64_t SetCollectionTrigger(BufferCache& cache, uint64_t trigger) {
        return std::exchange(cache.m_trigger_gc_memory, trigger);
    }
    static void CollectAgedDirtyBuffers(BufferCache& cache) {
        const auto trigger = std::exchange(cache.m_trigger_gc_memory, uint64_t {0});
        const auto critical = std::exchange(cache.m_critical_gc_memory, uint64_t {0});
        // All resources in this bounded fixture precede the aggressive retirement age.
        cache.m_gc_tick += 200;
        cache.RunGarbageCollector();
        cache.m_trigger_gc_memory = trigger;
        cache.m_critical_gc_memory = critical;
    }
};
struct TextureCacheTestAccess {
    static std::array<uint64_t, 3> UploadSnapshotStats(const TextureCache& cache) {
        return {cache.m_publication_stats.upload_captures, cache.m_publication_stats.upload_hits,
            cache.m_publication_stats.upload_avoided_bytes};
    }
    static uint64_t HostImportBytes(const TextureCache& cache) { return cache.m_host_import_bytes; }
    static std::array<uint64_t, 4> HostImportStats(const TextureCache& cache) {
        return {cache.m_host_import_bytes, cache.m_host_import_batches,
            cache.m_host_import_batch_owners, cache.m_host_import_batch_cap_stops};
    }
    static void PublishOrderedImages(TextureCache& cache, std::span<const ImageId> images) {
        std::scoped_lock lock(cache.m_lock);
        cache.PublishImageWrites(images);
    }
    static bool PublicationBlockSeedProbe() {
        bool pass = true;
        uint32_t cases = 0;
        for (const auto family: {TileBlockFamily::Standard256B, TileBlockFamily::Standard4KB,
                TileBlockFamily::Standard64KB, TileBlockFamily::Prt64KB,
                TileBlockFamily::RenderTarget64KB, TileBlockFamily::Depth64KB}) {
            for (uint32_t bytes = 1; bytes <= 16; bytes *= 2) {
                TileBlockLayout block {};
                if (!TileGetBlockLayout(family, bytes, block)) continue;
                const auto stride = uint64_t {9} * block.block_size + 16;
                GuestRange owner {0x10000, stride * 2 + 16};
                std::array<GpuTileInfo, 2> tiles;
                std::vector<uint8_t> written(owner.size, 0);
                for (uint32_t layer = 0; layer < tiles.size(); ++layer) {
                    auto& tile = tiles[layer];
                    tile.family = family;
                    tile.bytes_per_element = bytes;
                    tile.tiled_offset = 16 + stride * layer;
                    tile.tiled_size = 9ull * block.block_size;
                    tile.width = 2 * block.block_width + std::min(3u, block.block_width - 1);
                    tile.height = 2 * block.block_height + std::min(5u, block.block_height - 1);
                    tile.pitch = 3 * block.block_width;
                    tile.depth = 1;
                    tile.surface_z = layer == 0 ? 1 : 13;
                    for (uint32_t y = 0; y < tile.height; ++y) {
                        for (uint32_t x = 0; x < tile.width; ++x) {
                            uint32_t local = 0, swizzle = 0;
                            pass &= TileGetBlockOffset(block, x % block.block_width,
                                y % block.block_height, 0, local) &&
                                TileGetBlockXor(block, x / block.block_width,
                                    y / block.block_height, tile.surface_z, swizzle);
                            const auto index = uint64_t {y / block.block_height} * 3 +
                                x / block.block_width;
                            const auto offset = tile.tiled_offset + index * block.block_size +
                                (local ^ swizzle);
                            if (offset > written.size() || bytes > written.size() - offset) {
                                pass = false;
                            } else {
                                for (uint32_t lane = 0; lane < bytes; ++lane) written[offset + lane] = 1;
                            }
                        }
                    }
                }
                std::vector<GuestRange> overwritten;
                pass &= TextureCache::BuildPublicationOverwriteBlocks(owner, tiles, overwritten);
                uint64_t skipped = 0;
                for (const auto& range: overwritten) {
                    skipped += range.size;
                    for (uint64_t offset = range.address - owner.address;
                         offset < range.End() - owner.address; ++offset)
                        pass &= offset < written.size() && written[offset] != 0;
                }
                pass &= skipped == 8ull * block.block_size;
                const auto rejects = [&](auto invalid) {
                    overwritten = {{0x1000, 4}};
                    return !TextureCache::BuildPublicationOverwriteBlocks(owner, invalid, overwritten) &&
                        overwritten.empty();
                };
                auto invalid = tiles;
                invalid[0].tail = true;
                pass &= rejects(invalid);
                invalid = tiles;
                invalid[0].depth = 2;
                pass &= rejects(invalid);
                invalid = tiles;
                invalid[1].tiled_offset = invalid[0].tiled_offset;
                pass &= rejects(invalid);
                invalid = tiles;
                invalid[0].tiled_size -= 1;
                pass &= rejects(invalid);
                invalid = tiles;
                invalid[0].tiled_offset += 1;
                pass &= rejects(invalid);
                invalid = tiles;
                invalid[0].pitch = invalid[0].width - 1;
                pass &= rejects(invalid);
                ++cases;
            }
        }
        pass &= cases == 29;
        std::printf("PS5_GPU_TEST case=image-publication-block-seed-coverage result=%s cases=%u"
            " oracle=per-byte-CPU-map full-blocks-per-layer=4 layers=1,13 partial-blocks=preserved tail-volume-overlap=rejected\n",
            pass ? "PASS" : "FAIL", cases);
        return pass;
    }
    static std::pair<uint64_t, uint64_t> BlockSeedBytes(TextureCache& cache) {
        return {cache.m_publication_stats.block_seed_saved, cache.m_publication_stats.block_seed_preserved};
    }
    static size_t PublicationBytes(TextureCache& cache, ImageId image) {
        std::scoped_lock lock(cache.m_lock);
        const auto found = cache.m_publication_snapshots.find(image);
        return found == cache.m_publication_snapshots.end() ? 0 : found->second.bytes.size();
    }
    static bool PublicationMatches(TextureCache& cache, ImageId image) {
        std::scoped_lock lock(cache.m_lock);
        return cache.ReusePublicationSnapshot(image);
    }
    static bool PublicationBudgetBoundaries(TextureCache& cache) {
        constexpr uint64_t mib = 1024 * 1024;
        // Isolate the real allocator/pool from this fixture's native images.
        auto saved_snapshots = std::move(cache.m_publication_snapshots);
        auto saved_pool = std::move(cache.m_publication_pool);
        cache.m_publication_snapshots.clear();
        cache.m_publication_pool.clear();
        const auto saved_live = std::exchange(cache.m_publication_snapshot_bytes, 0);
        const auto saved_allocated = std::exchange(cache.m_publication_allocated_bytes, 0);
        const auto saved_stats = cache.m_publication_stats;
        std::array<TextureCache::PublicationBuffer, 4> owners;
        for (auto& owner: owners)
            owner = cache.ReservePublicationSnapshot(16 * mib);
        bool pass = cache.m_publication_allocated_bytes == 64 * mib &&
            std::all_of(owners.begin(), owners.end(), [](const auto& owner) {
                return owner.data() != nullptr && owner.size() == 16 * mib;
            }) && cache.ReservePublicationSnapshot(1).data() == nullptr &&
            cache.m_publication_snapshot_bytes == 0 && cache.m_publication_pool.empty() &&
            TextureCache::PublicationSnapshotFits(48 * mib, 16 * mib) &&
            !TextureCache::PublicationSnapshotFits(48 * mib + 1, 16 * mib) &&
            !TextureCache::PublicationSnapshotFits(64 * mib, 1) &&
            !TextureCache::PublicationSnapshotFits(0, 16 * mib + 1) &&
            !TextureCache::PublicationSnapshotFits(UINT64_MAX, 1) &&
            !TextureCache::PublicationSnapshotFits(0, 0);
        auto* reused_address = owners[0].data();
        if (reused_address) {
            reused_address[0] = 0x91;
            reused_address[16 * mib - 1] = 0xe7;
        }
        cache.RecyclePublicationBuffer(std::move(owners[0]));
        owners[0] = cache.ReservePublicationSnapshot(16 * mib);
        pass &= owners[0].data() == reused_address && reused_address != nullptr &&
            owners[0].data()[0] == 0x91 && owners[0].data()[16 * mib - 1] == 0xe7 &&
            cache.m_publication_allocated_bytes == 64 * mib;
        cache.RecyclePublicationBuffer(std::move(owners[0]));
        owners[0] = cache.ReservePublicationSnapshot(8 * mib);
        pass &= owners[0].data() != nullptr && owners[0].size() == 8 * mib &&
            cache.m_publication_allocated_bytes == 56 * mib &&
            cache.ReservePublicationSnapshot(16 * mib).data() == nullptr;
        for (auto& owner: owners) cache.RecyclePublicationBuffer(std::move(owner));
        pass &= cache.m_publication_pool.size() == 4 && cache.m_publication_snapshot_bytes == 0;
        cache.m_publication_pool.clear();
        cache.m_publication_snapshots = std::move(saved_snapshots);
        cache.m_publication_pool = std::move(saved_pool);
        cache.m_publication_snapshot_bytes = saved_live;
        cache.m_publication_allocated_bytes = saved_allocated;
        cache.m_publication_stats = saved_stats;
        std::printf("PS5_GPU_TEST case=image-publication-storage-recycle-budget result=%s"
            " reuse=exact-address preserved-sentinel=yes pending-cap=64MiB different-size-trim=56MiB\n",
            pass ? "PASS" : "FAIL");
        return pass;
    }
    static bool PublicationRangeUnionProbe() {
        std::vector<GuestRange> ranges {{0x3000, 0x1000}, {0x1000, 0x1000},
            {0x1800, 0x1000}, {0x2800, 0x800}, {0x6000, 0x1000}, {0x6800, 0x800}};
        const auto original = ranges;
        bool pass = TextureCache::CoalescePublicationRanges(ranges) && ranges.size() == 2 &&
            ranges[0] == GuestRange {0x1000, 0x3000} && ranges[1] == GuestRange {0x6000, 0x1000};
        for (uint64_t address = 0x800; address != 0x7800; ++address) {
            const auto contains = [address](const auto& range) {
                return address >= range.address && address < range.End();
            };
            pass &= std::any_of(original.begin(), original.end(), contains) ==
                std::any_of(ranges.begin(), ranges.end(), contains);
        }
        std::vector<GuestRange> invalid {{0x1000, 0x1000}, {UINT64_MAX - 15, 32}};
        const auto saved = invalid;
        pass &= !TextureCache::CoalescePublicationRanges(invalid) && invalid == saved;
        std::printf("PS5_GPU_TEST case=image-publication-alias-exact-union result=%s"
            " overlap=merged adjacent=merged hole=preserved overflow=rejected\n", pass ? "PASS" : "FAIL");
        return pass;
    }
    static bool FullDownload(TextureCache& cache, ImageId image) {
        return cache.DownloadFullyOverwrites(cache.m_slot_images[image]);
    }
    static std::pair<uint64_t, uint64_t> PrefillBytes(TextureCache& cache) {
        return {cache.m_publication_stats.prefill_saved, cache.m_publication_stats.prefill_preserved};
    }
    static bool PublicationAllocationBounded(TextureCache& cache) {
        uint64_t live = 0, pooled = 0;
        for (const auto& [id, snapshot]: cache.m_publication_snapshots) {
            (void)id;
            live += snapshot.bytes.size();
        }
        for (const auto& bytes: cache.m_publication_pool) pooled += bytes.size();
        return live == cache.m_publication_snapshot_bytes &&
            live + pooled == cache.m_publication_allocated_bytes &&
            cache.m_publication_allocated_bytes <= TextureCache::PublicationSnapshotBudget;
    }
    static uint64_t SetCollectionTrigger(TextureCache& cache, uint64_t trigger) {
        return std::exchange(cache.m_trigger_gc_memory, trigger);
    }
    static void Retire(TextureCache& cache, ImageId image) {
        std::scoped_lock lock(cache.m_lock);
        cache.FreeImage(image);
    }
    static ImageId Insert(TextureCache& cache, const ImageInfo& info) {
        std::scoped_lock lock(cache.m_lock);
        return cache.InsertImage(info);
    }
    static ImageId Stencil(TextureCache& cache, ImageId depth) {
        std::scoped_lock lock(cache.m_lock);
        return cache.AssociateStencil(depth, cache.m_slot_images[depth].info.stencil);
    }
    static bool CompleteDepthPlane(TextureCache& cache, ImageId image) {
        std::scoped_lock lock(cache.m_lock);
        ImageInfo plane;
        return cache.BuildDepthPlane(cache.m_slot_images[image], plane);
    }
    static bool CompleteDepthTiledColor(TextureCache& cache, ImageId image) {
        std::scoped_lock lock(cache.m_lock);
        return cache.DepthTiledColorReadbackSupported(cache.m_slot_images[image]);
    }
    static void ClearStencilLayer(TextureCache& cache, CommandBuffer& command, ImageId image,
                                  uint32_t layer, uint8_t value) {
        std::scoped_lock lock(cache.m_lock);
        vk::ClearValue clear {};
        clear.depthStencil.stencil = value;
        cache.ClearImage(command, image, cache.m_slot_images[image].backing.format,
            {vk::ImageAspectFlagBits::eStencil, 0, 1, layer, 1}, clear);
    }
    static void ClearDepthLayer(TextureCache& cache, CommandBuffer& command, ImageId image,
                                uint32_t layer, float value) {
        std::scoped_lock lock(cache.m_lock);
        vk::ClearValue clear {};
        clear.depthStencil.depth = value;
        cache.ClearImage(command, image, cache.m_slot_images[image].backing.format,
            {vk::ImageAspectFlagBits::eDepth, 0, 1, layer, 1}, clear);
    }
    static bool Registered(TextureCache& cache, ImageId image) {
        std::scoped_lock lock(cache.m_lock);
        const auto* record = cache.m_slot_images.try_get(image);
        return record != nullptr && record->registered;
    }
};
} // namespace Libs::Graphics

namespace Loader::GuestExecution {
namespace {
std::vector<uint32_t> RectProbeShader(bool vertex, bool clip, bool reject) {
    using namespace Libs::Graphics;
    ShaderRecompiler::Spirv::Builder builder;
    builder.RequireCapability(spv::CapabilityShader);
    if (clip) builder.RequireCapability(spv::CapabilityClipDistance);
    builder.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
    const auto void_type = builder.Type(spv::OpTypeVoid);
    const auto uint_type = builder.Type(spv::OpTypeInt, 32u, 0u);
    const auto int_type = builder.Type(spv::OpTypeInt, 32u, 1u);
    const auto float_type = builder.Type(spv::OpTypeFloat, 32u);
    const auto vec4_type = builder.Type(spv::OpTypeVector, float_type, 4u);
    const auto function_type = builder.Type(spv::OpTypeFunction, void_type);
    const auto zero = builder.Constant(spv::OpConstant, uint_type, 0u);
    const auto one = builder.Constant(spv::OpConstant, uint_type, 1u);
    const auto fzero = builder.Constant(spv::OpConstant, float_type, 0u);
    const auto fone = builder.Constant(spv::OpConstant, float_type, std::bit_cast<uint32_t>(1.0f));
    const auto ptr_output_vec4 = builder.Type(spv::OpTypePointer, spv::StorageClassOutput, vec4_type);
    auto output_pointer_type = ptr_output_vec4;
    if (vertex) {
        // Match the real vertex emitter's complete builtin block. A standalone
        // Position fixture would miss VS/TCS block-interface mismatches.
        uint32_t block;
        if (clip) {
            const auto array = builder.Type(spv::OpTypeArray, float_type, one);
            block = builder.DecoratedType(spv::OpTypeStruct,
                {{spv::OpMemberDecorate, {0u, spv::DecorationBuiltIn, spv::BuiltInPosition}},
                 {spv::OpMemberDecorate, {1u, spv::DecorationBuiltIn, spv::BuiltInClipDistance}},
                 {spv::OpDecorate, {spv::DecorationBlock}}}, vec4_type, array);
        } else {
            block = builder.DecoratedType(spv::OpTypeStruct,
                {{spv::OpMemberDecorate, {0u, spv::DecorationBuiltIn, spv::BuiltInPosition}},
                 {spv::OpDecorate, {spv::DecorationBlock}}}, vec4_type);
        }
        output_pointer_type = builder.Type(spv::OpTypePointer, spv::StorageClassOutput, block);
    }
    const auto output = builder.DefineGlobalVariable(output_pointer_type, spv::StorageClassOutput);
    std::vector<uint32_t> interfaces {output};
    uint32_t input = 0, distance = 0, ptr_output_float = 0;
    if (vertex) {
        const auto ptr_input_int = builder.Type(spv::OpTypePointer, spv::StorageClassInput, int_type);
        input = builder.DefineGlobalVariable(ptr_input_int, spv::StorageClassInput);
        builder.AddAnnotation(spv::OpDecorate, input, spv::DecorationBuiltIn, spv::BuiltInVertexIndex);
        interfaces.push_back(input);
        if (clip) {
            ptr_output_float = builder.Type(spv::OpTypePointer, spv::StorageClassOutput, float_type);
            distance = output;
        }
    } else {
        builder.AddAnnotation(spv::OpDecorate, output, spv::DecorationLocation, 0u);
    }
    const auto main = builder.AllocateId();
    builder.AddEntryPoint(vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment,
        main, "main", interfaces);
    if (!vertex) builder.AddExecutionMode(main, spv::ExecutionModeOriginUpperLeft);
    builder.AddFunction(spv::OpFunction, void_type, main, spv::FunctionControlMaskNone, function_type);
    builder.AddFunction(spv::OpLabel, builder.AllocateId());
    const auto result = [&](spv::Op opcode, uint32_t type, auto... operands) {
        const auto id = builder.AllocateId();
        builder.AddFunction(opcode, type, id, operands...);
        return id;
    };
    if (vertex) {
        const auto id = result(spv::OpBitcast, uint_type, result(spv::OpLoad, int_type, input));
        const auto x = result(spv::OpFSub, float_type,
            result(spv::OpConvertUToF, float_type,
                result(spv::OpShiftLeftLogical, uint_type,
                    result(spv::OpBitwiseAnd, uint_type, id, one), one)), fone);
        const auto y = result(spv::OpFSub, float_type,
            result(spv::OpConvertUToF, float_type,
                result(spv::OpShiftLeftLogical, uint_type,
                    result(spv::OpShiftRightLogical, uint_type, id, one), one)), fone);
        builder.AddFunction(spv::OpStore, result(spv::OpAccessChain, ptr_output_vec4, output, zero),
            result(spv::OpCompositeConstruct, vec4_type, x, y, fzero, fone));
        if (clip) {
            const auto value = builder.Constant(spv::OpConstant, float_type,
                std::bit_cast<uint32_t>(reject ? -1.0f : 0.0f));
            builder.AddFunction(spv::OpStore,
                result(spv::OpAccessChain, ptr_output_float, distance, one, zero), value);
        }
    } else {
        const auto red = builder.Constant(spv::OpConstant, float_type, std::bit_cast<uint32_t>(0.25f));
        const auto green = builder.Constant(spv::OpConstant, float_type, std::bit_cast<uint32_t>(0.5f));
        const auto blue = builder.Constant(spv::OpConstant, float_type, std::bit_cast<uint32_t>(0.75f));
        builder.AddFunction(spv::OpStore, output,
            result(spv::OpCompositeConstruct, vec4_type, red, green, blue, fone));
    }
    builder.AddFunction(spv::OpReturn);
    builder.AddFunction(spv::OpFunctionEnd);
    return builder.Build();
}

bool RectDistanceProbe(Libs::Graphics::RenderContext& renderer,
                      Libs::Graphics::HW::Context& hw,
                      Libs::Graphics::HW::UserConfig& user,
                      Libs::Graphics::ImageId image,
                      const Libs::Graphics::TextureCache::ImageDesc& desc) {
    using namespace Libs::Graphics;
    auto& graphics = renderer.GetGraphics();
    auto& scheduler = renderer.GetCommandScheduler();
    auto& textures = renderer.GetTextureCache();
    auto& native_image = textures.GetImage(image);
    RenderColorInfo color;
    color.desc = desc;
    color.image_id = image;
    ShaderRecompiler::IR::ResourceSnapshot snapshot;
    const auto fragment_spirv = RectProbeShader(false, false, false);
    const auto create = [&](const std::vector<uint32_t>& words) {
        return CompileSPV(words, graphics);
    };
    const auto fragment = create(fragment_spirv);
    bool pass = true;
    uint64_t serial = 0;
    for (const auto mode: {0u, 1u, 2u}) {
        const bool clip = mode != 0, reject = mode == 2;
        const auto vertex_spirv = RectProbeShader(true, clip, reject);
        const auto vertex = create(vertex_spirv);
        ShaderRecompiler::IR::CompiledShaderInfo vertex_info;
        // Raw fixture SPIR-V without a cull plane has no vertex-emitter synthetic plane.
        vertex_info.stage = clip ? ShaderType::Vertex : ShaderType::Unknown;
        vertex_info.info.outputs.push_back(
            {ShaderRecompiler::IR::StageOutputKind::Position, 0, 4, "gl_Position"});
        ShaderRecompiler::IR::CompiledShaderInfo pixel_info;
        pixel_info.stage = ShaderType::Pixel;
        ShaderVertexInputInfo vs;
        vs.stage.program = &vertex_info;
        vs.stage.resources = &snapshot;
        ShaderPixelInputInfo ps;
        ps.stage.program = &pixel_info;
        ps.stage.resources = &snapshot;
        hw.SetRenderTargetMask(0xf);
        hw.SetModeControl({});
        hw.SetClipControl({.dx_clip_space = true});
        user.SetPrimitiveType(Prospero::PrimitiveType::kRectList);
        EXIT_IF(!textures.ClearImageFromBuffer(scheduler.Current(), desc.info.data.address,
            desc.info.data.size, 0xff030201u));
        auto& pipeline = renderer.GetPipelineCache().GetGraphicsPipeline(std::span {&color, 1}, {},
            std::span {&vs, 1}, scheduler.Current(), &ps, vk::PrimitiveTopology::ePatchList, false,
            PipelineCache::GraphicsPrograms {{ShaderProgram {0xff00000000ull + ++serial, vertex}},
                ShaderProgram {0xff00000100ull, fragment}});
        (void)textures.FindRenderTarget(image, desc);
        textures.MarkGpuWritten(image);
        native_image.Transit(vk::ImageLayout::eColorAttachmentOptimal,
            vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
            {}, scheduler.Current().Handle());
        RenderState rendering;
        rendering.width = desc.info.extent.width;
        rendering.height = desc.info.extent.height;
        rendering.num_color_attachments = 1;
        rendering.color_attachments[0].image_view = textures.FindRenderTarget(image, desc);
        rendering.color_attachments[0].image_layout = vk::ImageLayout::eColorAttachmentOptimal;
        const auto command = scheduler.Current().Handle();
        const vk::Viewport viewport {0, static_cast<float>(rendering.height),
            static_cast<float>(rendering.width), -static_cast<float>(rendering.height), 0, 1};
        const vk::Rect2D scissor {{0, 0}, {rendering.width, rendering.height}};
        command.setViewportWithCount(1, &viewport);
        command.setScissorWithCount(1, &scissor);
        command.setLineWidth(1);
        command.setDepthTestEnable(false);
        command.setDepthWriteEnable(false);
        command.setDepthCompareOp(vk::CompareOp::eAlways);
        command.setDepthBiasEnable(false);
        command.setStencilTestEnable(false);
        const std::array<float, 4> blend {};
        command.setBlendConstants(blend.data());
#if !defined(__APPLE__)
        command.setDepthBoundsTestEnable(false);
        command.setDepthBounds(0.0f, 1.0f);
        const vk::Bool32 color_write = VK_TRUE;
        command.setColorWriteEnableEXT(1, &color_write);
#endif
        if (graphics.attachment_feedback_loop_enabled)
            command.setAttachmentFeedbackLoopEnableEXT({});
        scheduler.BeginRendering(rendering);
        command.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
        command.draw(3, 1, 0, 0);
        constexpr uint64_t probe_bytes = 32 * 32 * sizeof(uint32_t);
        Buffer readback(graphics, scheduler, MemoryUsage::Download, 0,
            vk::BufferUsageFlagBits::eTransferDst, probe_bytes);
        vk::BufferImageCopy copy;
        copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        copy.imageExtent = {32, 32, 1};
        native_image.Download(std::span {&copy, 1}, readback.Handle(), 0, probe_bytes);
        scheduler.Finish();
        readback.Invalidate(0, probe_bytes);
        bool result = true;
        const auto* pixels = reinterpret_cast<const uint8_t*>(readback.Mapped().data());
        const std::array<uint8_t, 4> expected = reject ? std::array<uint8_t, 4> {1, 2, 3, 255}
            : std::array<uint8_t, 4> {64, 128, 191, 255};
        for (uint64_t i = 0; i < probe_bytes; ++i) result &= pixels[i] == expected[i % 4];
        std::printf("PS5_GPU_TEST case=rect-distance-interface mode=%u result=%s first=%u,%u,%u,%u\n",
            mode, result ? "PASS" : "FAIL", pixels[0], pixels[1], pixels[2], pixels[3]);
        pass &= result;
        graphics.device.destroyShaderModule(vertex);
    }
    graphics.device.destroyShaderModule(fragment);
    return pass;
}

bool ImageUploadAliasBackingProbe(Libs::Graphics::RenderContext& renderer, uint64_t base) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    auto& textures = renderer.GetTextureCache();
    auto& scheduler = renderer.GetCommandScheduler();
    constexpr uint32_t source_width = 16, source_height = 16;
    constexpr uint32_t target_width = 96, target_height = 32;
    const auto source_pitch = TileGetTexturePitch(Prospero::BufferFormat::k8_8_8_8UNorm,
        source_width, Prospero::TileMode::kLinear);
    const auto target_pitch = TileGetTexturePitch(Prospero::BufferFormat::k8_8_8_8UNorm,
        target_width, Prospero::TileMode::kLinear);
    constexpr uint64_t source_size = 0x10000, target_size = 0x20000;
    const auto address = base + 0x1b0000;
    std::vector<uint32_t> expected(target_size / 4, 0xa5a5a5a5u);
    std::vector<uint32_t> pattern(source_width * source_height);
    for (uint32_t y = 0; y < source_height; ++y)
        for (uint32_t x = 0; x < source_width; ++x)
            expected[y * source_pitch + x] = pattern[y * source_width + x] =
                0xff000000u | (x * 11u) | ((y * 13u) << 8u) | ((x ^ y) << 16u);
    TextureCache::ImageDesc source_desc;
    source_desc.type = TextureCache::BindingType::RenderTarget;
    source_desc.info.data = {address, source_size};
    source_desc.info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    source_desc.info.guest_format = Prospero::BufferFormat::k8_8_8_8UNorm;
    source_desc.info.extent = {source_width, source_height, 1};
    source_desc.info.pitch = source_pitch;
    source_desc.info.bytes_per_block = 4;
    source_desc.info.UpdateSize();
    source_desc.view_info.format = source_desc.info.pixel_format;
    source_desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
    source_desc.view_info.usage = vk::ImageUsageFlagBits::eColorAttachment;
    const auto source = textures.FindImage(source_desc);
    (void)textures.FindRenderTarget(source, source_desc);
    Buffer upload(renderer.GetGraphics(), scheduler, MemoryUsage::Upload, 0,
        vk::BufferUsageFlagBits::eTransferSrc, pattern.size() * sizeof(uint32_t));
    std::memcpy(upload.Mapped().data(), pattern.data(), pattern.size() * sizeof(uint32_t));
    upload.Flush(0, pattern.size() * sizeof(uint32_t));
    vk::BufferImageCopy source_copy;
    source_copy.bufferRowLength = source_width;
    source_copy.bufferImageHeight = source_height;
    source_copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    source_copy.imageExtent = {source_width, source_height, 1};
    textures.GetImage(source).Upload(std::span {&source_copy, 1}, upload.Handle(), 0, upload.Size());
    uint32_t unpublished = 0;
    bool pass = source_pitch != target_pitch && textures.GetImage(source).SafeToDownload() &&
        Memory::TryReadBacking(address, &unpublished, sizeof(unpublished)) && unpublished == 0xa5a5a5a5u;

    auto target_desc = source_desc;
    target_desc.type = TextureCache::BindingType::Texture;
    target_desc.info.data.size = target_size;
    target_desc.info.extent = {target_width, target_height, 1};
    target_desc.info.pitch = target_pitch;
    target_desc.info.UpdateSize();
    target_desc.view_info.usage = vk::ImageUsageFlagBits::eSampled;
    const auto target = textures.FindImage(target_desc);
    pass &= target != source && textures.GetImage(source).IsGpuModified();
    // The public alias acquisition must complete the smaller native source
    // before uploading the larger surface from its differently pitched backing.
    (void)textures.FindTexture(target, target_desc);
    pass &= !textures.GetImage(source).IsGpuModified() && !textures.GetImage(target).IsGpuModified();
    std::vector<uint32_t> backing(expected.size());
    pass &= Memory::TryReadBacking(address, backing.data(), target_size) && backing == expected;
    for (size_t i = 0; i < backing.size(); ++i) {
        if (backing[i] == expected[i]) continue;
        std::printf("PS5_GPU_TEST alias-mismatch=backing word=%zu actual=%08x expected=%08x source_pitch=%u computed_source_pitch=%u target_pitch=%u computed_target_pitch=%u\n",
            i, backing[i], expected[i], source_pitch,
            TileGetTexturePitch(source_desc.info.guest_format, source_width, Prospero::TileMode::kLinear),
            target_pitch, TileGetTexturePitch(target_desc.info.guest_format, target_width, Prospero::TileMode::kLinear));
        break;
    }
    Buffer readback(renderer.GetGraphics(), scheduler, MemoryUsage::Download, 0,
        vk::BufferUsageFlagBits::eTransferDst, target_width * target_height * sizeof(uint32_t));
    auto target_copy = source_copy;
    target_copy.bufferRowLength = target_width;
    target_copy.bufferImageHeight = target_height;
    target_copy.imageExtent = {target_width, target_height, 1};
    textures.GetImage(target).Download(std::span {&target_copy, 1}, readback.Handle(), 0, readback.Size());
    scheduler.Finish();
    readback.Invalidate(0, readback.Size());
    const auto* native = reinterpret_cast<const uint32_t*>(readback.Mapped().data());
    bool printed_native_mismatch = false;
    for (uint32_t y = 0; y < target_height; ++y)
        for (uint32_t x = 0; x < target_width; ++x) {
            pass &= native[y * target_width + x] == expected[y * target_pitch + x];
            if (!printed_native_mismatch && native[y * target_width + x] != expected[y * target_pitch + x]) {
                std::printf("PS5_GPU_TEST alias-mismatch=native x=%u y=%u actual=%08x expected=%08x\n",
                    x, y, native[y * target_width + x], expected[y * target_pitch + x]);
                printed_native_mismatch = true;
            }
        }
    for (const auto guard_address: {address - 16, address + source_size, address + target_size}) {
        std::array<uint32_t, 4> guard {};
        pass &= Memory::TryReadBacking(guard_address, guard.data(), sizeof(guard));
        for (const auto word: guard) pass &= word == 0xa5a5a5a5u;
    }
    textures.UnmapMemory(address, target_size);
    pass &= !TextureCacheTestAccess::Registered(textures, source) &&
        !TextureCacheTestAccess::Registered(textures, target);
    scheduler.Finish();
    std::printf("PS5_GPU_TEST case=image-upload-alias-completed-source-publication result=%s source_size=%#llx target_size=%#llx\n",
        pass ? "PASS" : "FAIL", static_cast<unsigned long long>(source_size),
        static_cast<unsigned long long>(target_size));
    return pass;
}

bool TypedFragmentExportProbe(Libs::Graphics::RenderContext& renderer,
                              Libs::Graphics::HW::Context& hw,
                              Libs::Graphics::HW::UserConfig& user, uint64_t base) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    auto& graphics = renderer.GetGraphics();
    auto& scheduler = renderer.GetCommandScheduler();
    auto& textures = renderer.GetTextureCache();
    using Class = Prospero::TextureNumericClass;
    constexpr uint32_t width = 4, height = 4;
    constexpr uint64_t plane_size = 0x2000, plane_stride = 0x4000;
    const auto start = base + 0x2a0000;
    std::vector<uint8_t> seed(18 * plane_stride + 32, 0xa5);
    Memory::WriteBacking(start - 16, seed.data(), seed.size());

    // Three actual guest EXP streams cover the observed half transport and raw32 exports.
    const auto make_code = [](uint32_t variant) {
        std::vector<uint32_t> code;
        const auto move = [&](uint32_t reg, uint32_t bits) {
            code.push_back(0x7e0002ffu | (reg << 17u)); // v_mov_b32 literal
            code.push_back(bits);
        };
        const auto exp = [&](uint32_t slot, bool compressed, uint32_t first) {
            code.push_back(0xf800000fu | (slot << 4u) | (compressed ? 0x400u : 0u) |
                (slot == 2 ? 0x800u : 0u) | (slot == 0 ? 0x1000u : 0u));
            code.push_back(first | ((first + 1) << 8u) |
                ((compressed ? first : first + 2) << 16u) |
                ((compressed ? first + 1 : first + 3) << 24u));
        };
        if (variant == 0) {
            move(0, 0x80003c00u); move(1, 0x35557bffu);
            move(2, 0x38003400u); move(3, 0x3c003a00u);
            move(4, 0xc0003c00u); move(5, 0x42003800u);
            exp(0, true, 0); exp(1, true, 2); exp(2, true, 4);
        } else if (variant == 1) {
            move(0, 0xff808000u); move(1, 0xffff7fffu);
            move(2, 0x80000000u); move(3, 0xdeadbeefu);
            move(4, 0x12345678u); move(5, 0x7fffffffu);
            move(6, 0x3e800000u); move(7, 0xbf000000u);
            move(8, 0x40000000u); move(9, 0x3f800000u);
            exp(0, true, 0); exp(1, false, 2); exp(2, false, 6);
        } else {
            move(0, 0x80000000u); move(1, 0xffffff80u);
            move(2, 0x00007fffu); move(3, 0xffffffffu);
            move(4, 0x80000000u); move(5, 0x4000ffffu);
            move(6, 0x80018000u); move(7, 0x7fff0000u);
            exp(0, false, 0); exp(1, true, 4); exp(2, true, 6);
        }
        code.push_back(0xbf810000u);
        return code;
    };
    static const std::array<std::vector<uint32_t>, 3> guest_pixel {
        make_code(0), make_code(1), make_code(2)};
    static const std::array<uint32_t, 3> guest_vertex {
        0xf80008cfu, 0x00000000u, 0xbf810000u}; // EXP POS0; used only for program-cache admission.
    static ShaderUserData vertex_metadata {};
    ShaderMapUserData(reinterpret_cast<uint64_t>(guest_vertex.data()),
        {.type = Prospero::ShaderBinaryType::kGs, .user_data = &vertex_metadata,
         .code_size_bytes = sizeof(guest_vertex)});
    HW::VertexShaderInfo vertex_regs;
    vertex_regs.es_regs.data_addr = reinterpret_cast<uint64_t>(guest_vertex.data());
    const auto vertex_module = CompileSPV(RectProbeShader(true, false, false), graphics);
    ShaderRecompiler::IR::CompiledShaderInfo native_vertex_info;
    native_vertex_info.stage = ShaderType::Unknown;
    native_vertex_info.info.outputs.push_back(
        {ShaderRecompiler::IR::StageOutputKind::Position, 0, 4, "gl_Position"});
    ShaderRecompiler::IR::ResourceSnapshot vertex_snapshot;
    ShaderVertexInputInfo native_vertex;
    native_vertex.stage = {.program = &native_vertex_info, .resources = &vertex_snapshot};
    // CB_TARGET_MASK enables native writes; CB_SHADER_MASK maps the three
    // compact guest export ordinals to physical color attachments 0, 1, and 2.
    hw.SetRenderTargetMask(0xfff);
    hw.SetShaderMask(0xfff);
    hw.SetModeControl({});
    hw.SetClipControl({.dx_clip_space = true});
    user.SetPrimitiveType(Prospero::PrimitiveType::kRectList);
    std::array<uint64_t, 3> class_ids {};
    bool pass = true;
    for (uint32_t phase = 0; phase < 6; ++phase) {
        const auto variant = phase < 4 ? 0u : phase - 3;
        const auto& code = guest_pixel[variant];
        HW::PixelShaderInfo pixel_regs;
        pixel_regs.ps_regs.data_addr = reinterpret_cast<uint64_t>(code.data());
        ShaderMapUserData(pixel_regs.ps_regs.data_addr,
            {.type = Prospero::ShaderBinaryType::kPs,
             .code_size_bytes = static_cast<uint32_t>(code.size() * sizeof(uint32_t))});
        const auto first_type = phase == 1 ? Class::Uint : phase == 2 || phase >= 4 ? Class::Sint : Class::Float;
        const std::array<Prospero::BufferFormat, 3> formats = variant == 0
            ? std::array {first_type == Class::Uint ? Prospero::BufferFormat::k16_16_16_16UInt :
                          first_type == Class::Sint ? Prospero::BufferFormat::k16_16_16_16SInt :
                                                     Prospero::BufferFormat::k16_16_16_16Float,
                          Prospero::BufferFormat::k8_8_8_8UNorm, Prospero::BufferFormat::k16_16_16_16Float}
            : variant == 1
            ? std::array {Prospero::BufferFormat::k32_32_32_32SInt,
                          Prospero::BufferFormat::k32_32_32_32UInt, Prospero::BufferFormat::k32_32_32_32Float}
            : std::array {Prospero::BufferFormat::k32_32_32_32SInt,
                          Prospero::BufferFormat::k16_16_16_16UNorm, Prospero::BufferFormat::k32_32_32_32Float};
        std::array<Prospero::ColorComponentMapping, 8> mappings {};
        // A typed shuffle must preserve the payload and remain identical across cache classes.
        mappings[0] = Prospero::ColorMappingAbgr;
        std::array<Class, 8> classes;
        classes.fill(Class::Float);
        std::array<RenderColorInfo, 3> colors;
        for (uint32_t slot = 0; slot < 3; ++slot) {
            hw.SetTargetOutputMode(slot, variant == 0 ? 4 : variant == 1 ? (slot == 0 ? 4 : 9)
                : slot == 0 ? 9 : slot == 1 ? 5 : 6);
            hw.SetBlendControl(slot, {});
            auto& color = colors[slot];
            color.target_slot = slot;
            color.export_mapping = mappings[slot];
            auto& desc = color.desc;
            desc.type = TextureCache::BindingType::RenderTarget;
            desc.info.data = {start + (phase * 3 + slot) * plane_stride, plane_size};
            desc.info.guest_format = formats[slot];
            desc.info.pixel_format = VulkanFormat(formats[slot]);
            desc.info.extent = {width, height, 1};
            desc.info.pitch = TileGetTexturePitch(formats[slot], width, Prospero::TileMode::kLinear);
            desc.info.bytes_per_block = Prospero::RenderTargetBytesPerElement(formats[slot]);
            desc.info.UpdateSize();
            desc.view_info.format = desc.info.pixel_format;
            desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
            desc.view_info.usage = vk::ImageUsageFlagBits::eColorAttachment;
            classes[slot] = TextureRenderTargetNumericClass(desc.view_info.format);
            color.image_id = textures.FindImage(desc);
        }
        std::array<ShaderVertexInputInfo, 3> prepared_vertex;
        ShaderPixelInputInfo ps;
        auto programs = renderer.GetPipelineCache().GetGraphicsPrograms(vertex_regs, pixel_regs,
            hw.GetShaderRegisters(), hw, user, mappings, true, prepared_vertex, ps, classes);
        if (variant == 0) {
            const auto index = static_cast<uint32_t>(phase == 1 ? 1 : phase == 2 ? 2 : 0);
            if (class_ids[index] != 0) pass &= programs.pixel.id == class_ids[index];
            class_ids[index] = programs.pixel.id;
            if (phase == 3) pass &= class_ids[0] != class_ids[1] && class_ids[0] != class_ids[2] &&
                class_ids[1] != class_ids[2];
        }
        programs.vertex[0] = {0xff00002000ull, vertex_module};
        auto& pipeline = renderer.GetPipelineCache().GetGraphicsPipeline(colors, {},
            std::span {&native_vertex, 1}, scheduler.Current(), &ps,
            vk::PrimitiveTopology::ePatchList, false, programs);
        RenderState rendering;
        rendering.width = width; rendering.height = height; rendering.num_color_attachments = 3;
        for (uint32_t slot = 0; slot < 3; ++slot) {
            const auto& color = colors[slot];
            rendering.color_attachments[slot].image_view = textures.FindRenderTarget(color.image_id, color.desc);
            rendering.color_attachments[slot].image_layout = vk::ImageLayout::eColorAttachmentOptimal;
            textures.GetImage(color.image_id).Transit(vk::ImageLayout::eColorAttachmentOptimal,
                vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
                {}, scheduler.Current().Handle());
        }
        const auto command = scheduler.Current().Handle();
        const vk::Viewport viewport {0, static_cast<float>(height), static_cast<float>(width), -static_cast<float>(height), 0, 1};
        const vk::Rect2D scissor {{0, 0}, {width, height}};
        command.setViewportWithCount(1, &viewport); command.setScissorWithCount(1, &scissor);
        command.setLineWidth(1); command.setDepthTestEnable(false); command.setDepthWriteEnable(false);
        command.setDepthCompareOp(vk::CompareOp::eAlways); command.setDepthBiasEnable(false);
        command.setStencilTestEnable(false);
        const std::array<float, 4> blend {};
        command.setBlendConstants(blend.data());
#if !defined(__APPLE__)
        command.setDepthBoundsTestEnable(false);
        command.setDepthBounds(0.0f, 1.0f);
        const std::array<vk::Bool32, 3> writes {VK_TRUE, VK_TRUE, VK_TRUE};
        command.setColorWriteEnableEXT(writes.size(), writes.data());
#endif
        if (graphics.attachment_feedback_loop_enabled) command.setAttachmentFeedbackLoopEnableEXT({});
        scheduler.BeginRendering(rendering);
        command.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
        command.draw(3, 1, 0, 0);
        renderer.PublishGuestWrites();
        bool phase_pass = true;
        for (uint32_t slot = 0; slot < 3; ++slot) {
            std::vector<uint8_t> pixel;
            const auto words = [&](auto values) {
                pixel.resize(sizeof(values));
                std::memcpy(pixel.data(), values.data(), pixel.size());
            };
            if (variant == 0) {
                if (slot == 0) words(std::array<uint16_t, 4> {0x3555, 0x7bff, 0x8000, 0x3c00});
                if (slot == 1) words(std::array<uint8_t, 4> {64, 128, 191, 255});
                if (slot == 2) words(std::array<uint16_t, 4> {0x3c00, 0xc000, 0x3800, 0x4200});
            } else if (variant == 1) {
                if (slot == 0) words(std::array<uint32_t, 4> {0xffffffff, 0x7fff, 0xffffff80, 0xffff8000});
                if (slot == 1) words(std::array<uint32_t, 4> {0x80000000, 0xdeadbeef, 0x12345678, 0x7fffffff});
                if (slot == 2) words(std::array<uint32_t, 4> {0x3e800000, 0xbf000000, 0x40000000, 0x3f800000});
            } else {
                if (slot == 0) words(std::array<uint32_t, 4> {0xffffffff, 0x7fff, 0xffffff80, 0x80000000});
                if (slot == 1) words(std::array<uint16_t, 4> {0, 0x8000, 0xffff, 0x4000});
                if (slot == 2) words(std::array<uint32_t, 4> {0xbf800000, 0xbf800000, 0, 0x3f800000});
            }
            const auto& info = colors[slot].desc.info;
            std::vector<uint8_t> expected(plane_size, 0xa5), actual(plane_size);
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x)
                    std::memcpy(expected.data() + (y * info.pitch + x) * pixel.size(), pixel.data(), pixel.size());
            phase_pass &= Memory::TryReadBacking(info.data.address, actual.data(), actual.size()) && actual == expected;
            if (actual != expected) {
                const auto offset = static_cast<size_t>(std::mismatch(actual.begin(), actual.end(), expected.begin()).first - actual.begin());
                std::printf("PS5_GPU_TEST typed-export-mismatch phase=%u slot=%u offset=%#zx actual=%02x expected=%02x\n",
                    phase, slot, offset, actual[offset], expected[offset]);
            }
            for (const auto address: {info.data.address - 16, info.data.End()}) {
                std::array<uint8_t, 16> guard;
                phase_pass &= Memory::TryReadBacking(address, guard.data(), guard.size()) &&
                    std::all_of(guard.begin(), guard.end(), [](uint8_t byte) { return byte == 0xa5; });
            }
            textures.UnmapMemory(info.data.address, plane_size);
        }
        scheduler.Finish();
        pass &= phase_pass;
        std::printf("PS5_GPU_TEST case=typed-fragment-mixed-mrt phase=%u PS=%llu class0=%u result=%s\n",
            phase, static_cast<unsigned long long>(programs.pixel.id), static_cast<uint32_t>(classes[0]),
            phase_pass ? "PASS" : "FAIL");
    }
    // A stale blend register for inactive attachment 0 must not redirect a
    // compact export to location 0 when CB_SHADER_MASK selects attachment 3.
    const auto saved_shader_mask = hw.GetShaderRegisters().m_cbShaderMask;
    const auto saved_target_mask = hw.GetRenderTargetMask();
    const auto saved_output_modes = std::to_array(hw.GetShaderRegisters().target_output_mode);
    hw.SetShaderMask(0xf000);
    hw.SetRenderTargetMask(0xf000);
    for (uint32_t index = 0; index < 8; ++index) hw.SetTargetOutputMode(index, index == 0 ? 4 : 0);
    std::array<Prospero::ColorComponentMapping, 8> sparse_mappings {};
    sparse_mappings[3] = Prospero::ColorMappingAbgr;
    std::array<Class, 8> sparse_classes;
    sparse_classes.fill(Class::Float);
    HW::PixelShaderInfo sparse_pixel_regs;
    sparse_pixel_regs.ps_regs.data_addr = reinterpret_cast<uint64_t>(guest_pixel[0].data());
    uint64_t sparse_program_id = 0;
    bool sparse_pass = true;
    for (uint32_t variant = 0; variant < 3; ++variant) {
        HW::BlendControl stale_blend {};
        stale_blend.enable = variant != 0;
        stale_blend.color_srcblend = static_cast<uint8_t>(variant == 2
            ? Prospero::BlendFactor::kSrc1Color : Prospero::BlendFactor::kSrcAlpha);
        stale_blend.color_destblend = static_cast<uint8_t>(Prospero::BlendFactor::kOneMinusSrcAlpha);
        hw.SetBlendControl(0, stale_blend);
        std::array<ShaderVertexInputInfo, 3> prepared_vertex;
        ShaderPixelInputInfo ps;
        const auto programs = renderer.GetPipelineCache().GetGraphicsPrograms(vertex_regs, sparse_pixel_regs,
            hw.GetShaderRegisters(), hw, user, sparse_mappings, true, prepared_vertex, ps, sparse_classes);
        const auto& outputs = ps.stage.program->info.outputs;
        sparse_pass &= !ps.dual_source_blending && ps.alpha_blend_source == ShaderAlphaBlendSource::None &&
            outputs.size() == 1 && outputs[0].kind == ShaderRecompiler::IR::StageOutputKind::Mrt &&
            outputs[0].index == 0 && outputs[0].location == 3;
        if (variant == 0) sparse_program_id = programs.pixel.id;
        sparse_pass &= programs.pixel.id == sparse_program_id;
    }
    hw.SetBlendControl(0, {});
    hw.SetShaderMask(saved_shader_mask);
    hw.SetRenderTargetMask(saved_target_mask);
    for (uint32_t index = 0; index < 8; ++index) hw.SetTargetOutputMode(index, saved_output_modes[index]);
    pass &= sparse_pass;
    std::printf("PS5_GPU_TEST case=sparse-fragment-stale-target-zero-blend result=%s\n",
        sparse_pass ? "PASS" : "FAIL");

    // Exercise the actual native compiler for centroid/sample barycentrics.
    // With one rasterization sample, both pairs equal the pixel-center pair.
    // The interior pixel (0,3) of this 4x4 right triangle has I=J=1/8.
    static const std::array<std::array<uint32_t, 3>, 2> barycentric_code {{
        {0xf800180fu, 0x01000302u, 0xbf810000u}, // EXP centroid v2/v3, center v0/v1
        {0xf800180fu, 0x03020100u, 0xbf810000u}, // EXP sample v0/v1, center v2/v3
    }};
    hw.SetRenderTargetMask(0xf);
    hw.SetShaderMask(0xf);
    hw.SetPsInControl(0);
    for (uint32_t index = 0; index < 8; ++index) hw.SetTargetOutputMode(index, index == 0 ? 9 : 0);
    user.SetPrimitiveType(Prospero::PrimitiveType::kTriList);
    for (uint32_t variant = 0; variant < barycentric_code.size(); ++variant) {
        const auto& code = barycentric_code[variant];
        HW::PixelShaderInfo pixel_regs;
        pixel_regs.ps_regs.data_addr = reinterpret_cast<uint64_t>(code.data());
        ShaderMapUserData(pixel_regs.ps_regs.data_addr,
            {.type = Prospero::ShaderBinaryType::kPs, .code_size_bytes = sizeof(code)});
        hw.SetPsInputEna(variant == 0 ? 6u : 3u);
        hw.SetPsInputAddr(variant == 0 ? 6u : 3u);
        RenderColorInfo color;
        color.target_slot = 0;
        auto& desc = color.desc;
        desc.type = TextureCache::BindingType::RenderTarget;
        desc.info.data = {base + 0x300000, plane_size};
        desc.info.guest_format = Prospero::BufferFormat::k32_32_32_32Float;
        desc.info.pixel_format = VulkanFormat(desc.info.guest_format);
        desc.info.extent = {width, height, 1};
        desc.info.UpdateSize();
        desc.view_info.format = desc.info.pixel_format;
        desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
        desc.view_info.usage = vk::ImageUsageFlagBits::eColorAttachment;
        std::vector<uint8_t> initial(plane_size, 0xa5);
        Memory::WriteBacking(desc.info.data.address, initial.data(), initial.size());
        color.image_id = textures.FindImage(desc);
        std::array<Prospero::ColorComponentMapping, 8> mappings {};
        std::array<ShaderVertexInputInfo, 3> prepared_vertex;
        ShaderPixelInputInfo ps;
        auto programs = renderer.GetPipelineCache().GetGraphicsPrograms(vertex_regs, pixel_regs,
            hw.GetShaderRegisters(), hw, user, mappings, true, prepared_vertex, ps, {},
            desc.info.samples);
        programs.vertex[0] = {0xff00002000ull, vertex_module};
        auto& pipeline = renderer.GetPipelineCache().GetGraphicsPipeline(std::span {&color, 1}, {},
            std::span {&native_vertex, 1}, scheduler.Current(), &ps,
            vk::PrimitiveTopology::eTriangleList, false, programs);
        RenderState rendering;
        rendering.width = width;
        rendering.height = height;
        rendering.num_color_attachments = 1;
        rendering.color_attachments[0].image_view = textures.FindRenderTarget(color.image_id, desc);
        rendering.color_attachments[0].image_layout = vk::ImageLayout::eColorAttachmentOptimal;
        textures.GetImage(color.image_id).Transit(vk::ImageLayout::eColorAttachmentOptimal,
            vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
            {}, scheduler.Current().Handle());
        const auto command = scheduler.Current().Handle();
        const vk::Viewport viewport {0, static_cast<float>(height), static_cast<float>(width),
            -static_cast<float>(height), 0, 1};
        const vk::Rect2D scissor {{0, 0}, {width, height}};
        command.setViewportWithCount(1, &viewport);
        command.setScissorWithCount(1, &scissor);
        command.setLineWidth(1);
        command.setDepthTestEnable(false);
        command.setDepthWriteEnable(false);
        command.setDepthCompareOp(vk::CompareOp::eAlways);
        command.setDepthBiasEnable(false);
        command.setStencilTestEnable(false);
        const std::array<float, 4> blend {};
        command.setBlendConstants(blend.data());
#if !defined(__APPLE__)
        command.setDepthBoundsTestEnable(false);
        command.setDepthBounds(0.0f, 1.0f);
        const vk::Bool32 write = VK_TRUE;
        command.setColorWriteEnableEXT(1, &write);
#endif
        if (graphics.attachment_feedback_loop_enabled) command.setAttachmentFeedbackLoopEnableEXT({});
        scheduler.BeginRendering(rendering);
        command.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
        command.draw(3, 1, 0, 0);
        renderer.PublishGuestWrites();
        std::array<uint32_t, 4> pixel {};
        const std::array<uint32_t, 4> expected {0x3e000000u, 0x3e000000u, 0x3e000000u, 0x3e000000u};
        const bool result = ps.rasterization_samples == 1 &&
            Memory::TryReadBacking(desc.info.data.address + (height - 1) * desc.info.pitch * sizeof(pixel),
                pixel.data(), sizeof(pixel)) && pixel == expected;
        pass &= result;
        std::printf("PS5_GPU_TEST case=fragment-barycentric-%s-one-sample result=%s pixel=%08x,%08x,%08x,%08x\n",
            variant == 0 ? "centroid" : "sample", result ? "PASS" : "FAIL",
            pixel[0], pixel[1], pixel[2], pixel[3]);
        textures.UnmapMemory(desc.info.data.address, plane_size);
        scheduler.Finish();
    }
    hw.SetPsInputEna(0);
    hw.SetPsInputAddr(0);
    graphics.device.destroyShaderModule(vertex_module);
    std::printf("PS5_GPU_TEST case=typed-fragment-cache-float-uint-sint-float result=%s\n", pass ? "PASS" : "FAIL");
    return pass;
}

bool DepthTiledColorBackingProbe(Libs::Graphics::RenderContext& renderer, uint64_t base) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    auto& scheduler = renderer.GetCommandScheduler();
    auto& textures = renderer.GetTextureCache();
    constexpr uint32_t width = 259, height = 137, layers = 2;
    const auto pitch = TileGetDepthPitch(width, 4);
    TileSizeAlign layout {};
    TileBlockLayout block {};
    if (!TileGetRenderTargetSize(width, height, pitch, 4, layout) ||
        !TileGetBlockLayout(TileBlockFamily::Depth64KB, 4, block)) return false;
    const uint64_t size = static_cast<uint64_t>(layout.size) * layers;
    const uint64_t address = base + 0x310000;
    std::vector<uint8_t> initial(size, 0xcd), expected = initial;
    const uint64_t native_slice = static_cast<uint64_t>(width) * height * sizeof(uint32_t);
    std::vector<uint32_t> native(width * height * layers);
    for (uint32_t layer = 0; layer < layers; ++layer)
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                uint32_t local = 0, swizzle = 0;
                EXIT_IF(!TileGetBlockOffset(block, x % block.block_width, y % block.block_height,
                                           0, local) ||
                        !TileGetBlockXor(block, x / block.block_width, y / block.block_height,
                                        layer, swizzle));
                const uint64_t tile = static_cast<uint64_t>(y / block.block_height) *
                    (pitch / block.block_width) + x / block.block_width;
                const uint64_t offset = layout.size * static_cast<uint64_t>(layer) +
                    tile * block.block_size + (local ^ swizzle);
                const auto initial_word = std::bit_cast<uint32_t>(0.125f);
                const auto word = std::bit_cast<uint32_t>(static_cast<float>(
                    1u + (x * 13u + y * 79u + layer * 997u) % 65534u) / 65536.0f);
                std::memcpy(initial.data() + offset, &initial_word, sizeof(initial_word));
                std::memcpy(expected.data() + offset, &word, sizeof(word));
                native[(static_cast<uint64_t>(layer) * height + y) * width + x] = word;
            }
    Memory::WriteBacking(address, initial.data(), size);
    TextureCache::ImageDesc desc;
    desc.type = TextureCache::BindingType::DepthTarget;
    desc.info.data = {address, size};
    desc.info.pixel_format = vk::Format::eD32Sfloat;
    desc.info.guest_format = Prospero::BufferFormat::k32Float;
    desc.info.extent = {width, height, 1};
    desc.info.resources = {1, layers};
    desc.info.pitch = pitch;
    desc.info.bytes_per_block = 4;
    desc.info.tile_mode = Prospero::TileMode::kDepth;
    desc.info.UpdateSize();
    desc.view_info.format = desc.info.pixel_format;
    desc.view_info.type = vk::ImageViewType::e2DArray;
    desc.view_info.layer_count = layers;
    desc.view_info.aspect = vk::ImageAspectFlagBits::eDepth;
    desc.view_info.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
    const auto depth = textures.FindImage(desc);
    (void)textures.FindDepthTarget(depth, desc);
    renderer.PublishGuestWrites();
    bool pass = !textures.GetImage(depth).IsGpuModified();
    // Native color reinterpretation uses the same public overlap/copy path as
    // storage acquisition. Publish the old depth authority before changing it.
    desc.type = TextureCache::BindingType::Storage;
    desc.info.pixel_format = vk::Format::eR32Sfloat;
    desc.view_info.format = desc.info.pixel_format;
    desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
    desc.view_info.usage = vk::ImageUsageFlagBits::eStorage;
    const auto color = textures.FindImage(desc);
    pass &= color != depth && !TextureCacheTestAccess::Registered(textures, depth) &&
        textures.GetImage(color).backing.format == vk::Format::eR32Sfloat &&
        TextureCacheTestAccess::CompleteDepthTiledColor(textures, color);
    const auto saved = textures.GetImage(color).info;
    const auto reject = [&] {
        pass &= !TextureCacheTestAccess::CompleteDepthTiledColor(textures, color);
        textures.GetImage(color).info = saved;
    };
    textures.GetImage(color).info.metadata.kind = ImageMetadataKind::Htile;
    textures.GetImage(color).info.metadata.range = {base + 0x1e0000, 0x8000};
    reject();
    textures.GetImage(color).info.metadata.stencil_compressed = true;
    reject();
    textures.GetImage(color).info.stencil = saved.data;
    reject();
    textures.GetImage(color).info.samples = 2;
    reject();
    textures.GetImage(color).info.resources.levels = 2;
    reject();
    textures.GetImage(color).info.pitch = pitch - 1;
    reject();
    textures.GetImage(color).info.data.size -= 4;
    textures.GetImage(color).info.mip_layout[0].size -= 4;
    reject();
    textures.GetImage(color).info.guest_format = Prospero::BufferFormat::k16UNorm;
    reject();
    (void)textures.FindTexture(color, desc);
    Buffer upload(renderer.GetGraphics(), scheduler, MemoryUsage::Upload, 0,
        vk::BufferUsageFlagBits::eTransferSrc, native.size() * sizeof(uint32_t));
    std::memcpy(upload.Mapped().data(), native.data(), upload.Size());
    upload.Flush(0, upload.Size());
    std::array<vk::BufferImageCopy, layers> copies;
    for (uint32_t layer = 0; layer < layers; ++layer) {
        copies[layer].bufferOffset = layer * native_slice;
        copies[layer].bufferRowLength = width;
        copies[layer].bufferImageHeight = height;
        copies[layer].imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, layer, 1};
        copies[layer].imageExtent = {width, height, 1};
    }
    textures.GetImage(color).Upload(copies, upload.Handle(), 0, upload.Size());
    std::vector<uint8_t> actual(size);
    pass &= textures.GetImage(color).SafeToDownload() &&
        Memory::TryReadBacking(address, actual.data(), size) && actual == initial && initial != expected;
    renderer.PublishGuestWrites();
    pass &= !textures.GetImage(color).IsGpuModified() &&
        Memory::TryReadBacking(address, actual.data(), size) && actual == expected;
    for (size_t i = 0; i < actual.size(); ++i) {
        if (actual[i] == expected[i]) continue;
        std::printf("PS5_GPU_TEST mismatch=depth-tiled-color offset=%#zx actual=%02x expected=%02x\n",
            i, actual[i], expected[i]);
        break;
    }
    // Re-upload from the published tiled backing and check the native color
    // aspect. Complete owner comparisons above retain all untargeted padding.
    textures.UpdateImage(color);
    Buffer readback(renderer.GetGraphics(), scheduler, MemoryUsage::Download, 0,
        vk::BufferUsageFlagBits::eTransferDst, upload.Size());
    textures.GetImage(color).Download(copies, readback.Handle(), 0, readback.Size());
    scheduler.Finish();
    readback.Invalidate(0, readback.Size());
    pass &= std::memcmp(readback.Mapped().data(), native.data(), readback.Size()) == 0;
    for (const auto guard_address: {address - 16, address + size}) {
        std::array<uint32_t, 4> guard {};
        pass &= Memory::TryReadBacking(guard_address, guard.data(), sizeof(guard));
        for (const auto word: guard) pass &= word == 0xa5a5a5a5u;
    }
    textures.UnmapMemory(address, size);
    pass &= !TextureCacheTestAccess::Registered(textures, color);
    scheduler.Finish();
    std::printf("PS5_GPU_TEST case=depth-tiled-native-color-inverse-publication result=%s extent=%ux%u pitch=%u layers=%u bytes=%llu\n",
        pass ? "PASS" : "FAIL", width, height, pitch, layers, static_cast<unsigned long long>(size));
    return pass;
}

bool DepthStencilBackingProbe(Libs::Graphics::RenderContext& renderer, uint64_t base, bool tiled) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    auto& scheduler = renderer.GetCommandScheduler();
    auto& textures = renderer.GetTextureCache();
    constexpr uint32_t layers = 2;
    // Include complete 256x256 stencil blocks as well as partial right/bottom
    // blocks, so the seed candidate is tested on both native depth aspects.
    const uint32_t width = tiled ? 259 : 7, height = tiled ? 269 : 5;
    const uint32_t depth_pitch = tiled ? TileGetDepthPitch(width, 4) : 12;
    const uint32_t stencil_pitch = tiled ? TileGetDepthPitch(width, 1) : depth_pitch;
    TileSizeAlign depth_layout {256, 4}, stencil_layout {64, 4};
    if (tiled && (!TileGetRenderTargetSize(width, height, depth_pitch, 4, depth_layout) ||
                  !TileGetRenderTargetSize(width, height, stencil_pitch, 1, stencil_layout)))
        return false;
    const uint64_t depth_size = static_cast<uint64_t>(depth_layout.size) * layers;
    const uint64_t stencil_size = static_cast<uint64_t>(stencil_layout.size) * layers;
    TextureCache::ImageDesc desc;
    desc.type = TextureCache::BindingType::DepthTarget;
    desc.info.data = {base + (tiled ? 0x80000 : 0x1a1000), depth_size};
    desc.info.stencil = {base + (tiled ? 0x280000 : 0x1a2000), stencil_size};
    desc.info.pixel_format = vk::Format::eD32SfloatS8Uint;
    desc.info.guest_format = Prospero::BufferFormat::k32Float;
    desc.info.type = Prospero::ImageType::kColor2D;
    desc.info.extent = {width, height, 1};
    desc.info.resources = {1, layers};
    desc.info.pitch = depth_pitch;
    desc.info.bytes_per_block = 4;
    desc.info.tile_mode = tiled ? Prospero::TileMode::kDepth : Prospero::TileMode::kLinear;
    desc.info.UpdateSize();
    desc.view_info.format = desc.info.pixel_format;
    desc.view_info.type = vk::ImageViewType::e2DArray;
    desc.view_info.layer_count = layers;
    desc.view_info.aspect = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
    desc.view_info.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
    TileBlockLayout depth_block, stencil_block;
    if (!TileGetBlockLayout(TileBlockFamily::Depth64KB, 4, depth_block) ||
        !TileGetBlockLayout(TileBlockFamily::Depth64KB, 1, stencil_block)) return false;
    const auto guest_offset = [&](bool stencil, uint32_t x, uint32_t y, uint32_t layer) {
        const auto& block = stencil ? stencil_block : depth_block;
        const uint32_t pitch = stencil ? stencil_pitch : depth_pitch;
        const uint64_t stride = stencil ? stencil_layout.size : depth_layout.size;
        if (!tiled) return stride * layer + (static_cast<uint64_t>(y) * pitch + x) * block.bytes_per_element;
        uint32_t local = 0, swizzle = 0;
        EXIT_IF(!TileGetBlockOffset(block, x % block.block_width, y % block.block_height, 0, local) ||
                !TileGetBlockXor(block, x / block.block_width, y / block.block_height, layer, swizzle));
        const uint64_t tile = static_cast<uint64_t>(y / block.block_height) *
            (pitch / block.block_width) + x / block.block_width;
        return stride * layer + tile * block.block_size + (local ^ swizzle);
    };
    std::vector<uint8_t> expected_depth(depth_size, 0xcd), expected_stencil(stencil_size, 0x6c);
    auto initial_depth = expected_depth, initial_stencil = expected_stencil;
    const uint64_t depth_transfer_slice = static_cast<uint64_t>(width) * height * sizeof(uint32_t);
    const uint64_t stencil_transfer_slice = (static_cast<uint64_t>(width) * height + 3u) & ~uint64_t {3};
    std::vector<uint8_t> native_depth(depth_transfer_slice * layers), native_stencil(stencil_transfer_slice * layers);
    for (uint32_t layer = 0; layer < layers; ++layer)
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                const auto value = std::bit_cast<uint32_t>(
                    static_cast<float>(1 + (x * 13u + y * 79u + layer * 997u) % 65534u) / 65536.0f);
                const auto depth_offset = guest_offset(false, x, y, layer);
                const auto stencil_offset = guest_offset(true, x, y, layer);
                const auto stencil_value = static_cast<uint8_t>(x * 5u + y * 17u + layer * 47u);
                const auto initial_value = std::bit_cast<uint32_t>(0.125f);
                std::memcpy(expected_depth.data() + depth_offset, &value, sizeof(value));
                std::memcpy(initial_depth.data() + depth_offset, &initial_value, sizeof(initial_value));
                expected_stencil[stencil_offset] = stencil_value;
                initial_stencil[stencil_offset] = 0x21;
                std::memcpy(native_depth.data() + depth_transfer_slice * layer +
                    (static_cast<uint64_t>(y) * width + x) * sizeof(value), &value, sizeof(value));
                native_stencil[stencil_transfer_slice * layer + static_cast<uint64_t>(y) * width + x] = stencil_value;
            }
    Memory::WriteBacking(desc.info.data.address, initial_depth.data(), depth_size);
    Memory::WriteBacking(desc.info.stencil.address, initial_stencil.data(), stencil_size);
    auto image = textures.FindImage(desc);
    (void)textures.FindDepthTarget(image, desc);
    auto stencil = TextureCacheTestAccess::Stencil(textures, image);
    bool pass = TextureCacheTestAccess::CompleteDepthPlane(textures, image) &&
        TextureCacheTestAccess::CompleteDepthPlane(textures, stencil);
    // Reject incomplete contracts before any Vulkan effects or guest publication.
    const auto saved_info = textures.GetImage(image).info;
    textures.GetImage(image).info.metadata.kind = ImageMetadataKind::Htile;
    textures.GetImage(image).info.metadata.range = {base + 0x1e0000, 0x8000};
    pass &= !TextureCacheTestAccess::CompleteDepthPlane(textures, image) &&
        !TextureCacheTestAccess::CompleteDepthPlane(textures, stencil);
    textures.GetImage(image).info = saved_info;
    textures.GetImage(image).info.metadata.stencil_compressed = true;
    pass &= !TextureCacheTestAccess::CompleteDepthPlane(textures, image);
    textures.GetImage(image).info = saved_info;
    textures.GetImage(image).info.samples = 2;
    pass &= !TextureCacheTestAccess::CompleteDepthPlane(textures, image);
    textures.GetImage(image).info = saved_info;
    textures.GetImage(image).info.stencil = saved_info.data;
    pass &= !TextureCacheTestAccess::CompleteDepthPlane(textures, image);
    textures.GetImage(image).info = saved_info;
    textures.GetImage(image).info.stencil.size -= 1;
    pass &= !TextureCacheTestAccess::CompleteDepthPlane(textures, image);
    textures.GetImage(image).info = saved_info;

    Buffer depth_upload(renderer.GetGraphics(), scheduler, MemoryUsage::Upload, 0,
        vk::BufferUsageFlagBits::eTransferSrc, native_depth.size());
    Buffer stencil_upload(renderer.GetGraphics(), scheduler, MemoryUsage::Upload, 0,
        vk::BufferUsageFlagBits::eTransferSrc, native_stencil.size());
    std::memcpy(depth_upload.Mapped().data(), native_depth.data(), native_depth.size());
    std::memcpy(stencil_upload.Mapped().data(), native_stencil.data(), native_stencil.size());
    depth_upload.Flush(0, native_depth.size());
    stencil_upload.Flush(0, native_stencil.size());
    const auto copies = [&](bool stencil_plane) {
        std::array<vk::BufferImageCopy, layers> regions;
        for (uint32_t layer = 0; layer < layers; ++layer) {
            regions[layer].bufferOffset = layer * (stencil_plane ? stencil_transfer_slice : depth_transfer_slice);
            regions[layer].bufferRowLength = width;
            regions[layer].bufferImageHeight = height;
            regions[layer].imageSubresource = {stencil_plane ? vk::ImageAspectFlagBits::eStencil :
                vk::ImageAspectFlagBits::eDepth, 0, layer, 1};
            regions[layer].imageExtent = {width, height, 1};
        }
        return regions;
    };
    const auto depth_copies = copies(false), stencil_copies = copies(true);
    textures.GetImage(image).Upload(depth_copies, depth_upload.Handle(), 0, native_depth.size());
    textures.GetImage(image).Upload(stencil_copies, stencil_upload.Handle(), 0, native_stencil.size());
    // Normal render acquisition must own both aspects without a later manual mark.
    pass &= textures.GetImage(image).IsGpuModified() && textures.GetImage(stencil).IsGpuModified();
    const auto backing_matches = [&] {
        std::vector<uint8_t> depth(depth_size), stencil_bytes(stencil_size);
        bool valid = Memory::TryReadBacking(desc.info.data.address, depth.data(), depth.size()) &&
            Memory::TryReadBacking(desc.info.stencil.address, stencil_bytes.data(), stencil_bytes.size()) &&
            depth == expected_depth && stencil_bytes == expected_stencil;
        const auto mismatch = [&](const char* plane, const std::vector<uint8_t>& actual,
                                  const std::vector<uint8_t>& expected) {
            for (size_t i = 0; i < actual.size(); ++i) {
                if (actual[i] == expected[i]) continue;
                std::printf("PS5_GPU_TEST mismatch=%s mode=%s offset=%#zx actual=%02x expected=%02x\n",
                    plane, tiled ? "tiled" : "linear", i, actual[i], expected[i]);
                break;
            }
        };
        mismatch("depth", depth, expected_depth);
        mismatch("stencil", stencil_bytes, expected_stencil);
        for (const auto range: {desc.info.data, desc.info.stencil}) {
            std::array<uint32_t, 4> before {}, after {};
            valid &= Memory::TryReadBacking(range.address - sizeof(before), before.data(), sizeof(before)) &&
                Memory::TryReadBacking(range.End(), after.data(), sizeof(after));
            for (size_t i = 0; i < before.size(); ++i) {
                valid &= before[i] == 0xa5a5a5a5u && after[i] == 0xa5a5a5a5u;
                if (before[i] != 0xa5a5a5a5u || after[i] != 0xa5a5a5a5u)
                    std::printf("PS5_GPU_TEST mismatch=guard mode=%s range=%#llx word=%zu before=%08x after=%08x expected=a5a5a5a5\n",
                        tiled ? "tiled" : "linear", static_cast<unsigned long long>(range.address),
                        i, before[i], after[i]);
            }
        }
        return valid;
    };
    const auto seed_before = TextureCacheTestAccess::BlockSeedBytes(textures);
    renderer.PublishGuestWrites();
    const auto seed_after = TextureCacheTestAccess::BlockSeedBytes(textures);
    const auto* seed_trace = std::getenv("LUMEN_PS5_IMAGE_PUBLICATION_TRACE");
    if (tiled && seed_trace != nullptr && std::strcmp(seed_trace, "1") == 0) {
        pass &= ImagePublicationBlockSeedEnabled()
            ? seed_after.first > seed_before.first && seed_after.second > seed_before.second
            : seed_after == seed_before;
    }
    pass &= backing_matches() && !textures.GetImage(image).IsGpuModified() &&
        !textures.GetImage(stencil).IsGpuModified();
    std::printf("PS5_GPU_TEST case=%s-depth-stencil-nonuniform-publication result=%s depth_pitch=%u stencil_pitch=%u layers=%u\n",
        tiled ? "tiled" : "linear", pass ? "PASS" : "FAIL", depth_pitch, stencil_pitch, layers);

    // Consume the published guest planes again, then compare both native aspects.
    (void)textures.FindDepthTarget(image, desc);
    Buffer depth_read(renderer.GetGraphics(), scheduler, MemoryUsage::Download, 0,
        vk::BufferUsageFlagBits::eTransferDst, native_depth.size());
    Buffer stencil_read(renderer.GetGraphics(), scheduler, MemoryUsage::Download, 0,
        vk::BufferUsageFlagBits::eTransferDst, native_stencil.size());
    textures.GetImage(image).Download(depth_copies, depth_read.Handle(), 0, native_depth.size());
    textures.GetImage(image).Download(stencil_copies, stencil_read.Handle(), 0, native_stencil.size());
    scheduler.Finish();
    depth_read.Invalidate(0, native_depth.size());
    stencil_read.Invalidate(0, native_stencil.size());
    pass &= std::memcmp(depth_read.Mapped().data(), native_depth.data(), native_depth.size()) == 0;
    for (uint32_t layer = 0; layer < layers; ++layer)
        pass &= std::memcmp(stencil_read.Mapped().data() + layer * stencil_transfer_slice,
            native_stencil.data() + layer * stencil_transfer_slice, static_cast<size_t>(width) * height) == 0;
    renderer.PublishGuestWrites();

    (void)textures.FindDepthTarget(image, desc);
    textures.GetImage(image).Transit(vk::ImageLayout::eTransferDstOptimal,
        vk::AccessFlagBits2::eTransferWrite, {}, scheduler.Current().Handle());
    vk::ClearDepthStencilValue native_clear {0.0f, 0x69};
    const vk::ImageSubresourceRange stencil_layer {vk::ImageAspectFlagBits::eStencil, 0, 1, 0, 1};
    scheduler.Current().Handle().clearDepthStencilImage(textures.GetImage(image).backing.image,
        vk::ImageLayout::eTransferDstOptimal, &native_clear, 1, &stencil_layer);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) expected_stencil[guest_offset(true, x, y, 0)] = 0x69;
    pass &= textures.GetImage(image).IsGpuModified() && textures.GetImage(stencil).IsGpuModified();
    renderer.PublishGuestWrites();
    pass &= backing_matches();
    std::printf("PS5_GPU_TEST case=%s-depth-target-acquisition-stencil-publication result=%s\n",
        tiled ? "tiled" : "linear", pass ? "PASS" : "FAIL");

    TextureCacheTestAccess::ClearStencilLayer(textures, scheduler.Current(), image, 1, 0x37);
    pass &= !textures.GetImage(image).IsGpuModified() && textures.GetImage(stencil).IsGpuModified();
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) expected_stencil[guest_offset(true, x, y, 1)] = 0x37;

    std::array<uint8_t, 32> clean_read {};
    const uint64_t source_offset = static_cast<uint64_t>(stencil_layout.size) + 16;
    const auto source_address = desc.info.stencil.address + source_offset;
    pass &= textures.IsRegionGpuModified(source_address, clean_read.size()) &&
        !Memory::TryReadGpuCleanBacking(source_address, clean_read.data(), clean_read.size());
    // This first buffer request is partial, non-texel, and has no preallocated
    // complete image owner. It must acquire and synchronize full owner storage.
    auto& buffers = renderer.GetBufferCache();
    auto [partial, partial_offset] = buffers.ObtainBuffer(source_address, clean_read.size(), false, false);
    Buffer partial_read(renderer.GetGraphics(), scheduler, MemoryUsage::Download, 0,
        vk::BufferUsageFlagBits::eTransferDst, clean_read.size());
    const vk::BufferCopy partial_copy {partial_offset, 0, clean_read.size()};
    scheduler.Current().Handle().copyBuffer(partial->Handle(), partial_read.Handle(), 1, &partial_copy);
    scheduler.Finish();
    partial_read.Invalidate(0, clean_read.size());
    pass &= std::memcmp(partial_read.Mapped().data(), expected_stencil.data() + source_offset,
        clean_read.size()) == 0;
    const uint64_t copy_address = base + (tiled ? 0x3e2000 : 0x1f1000);
    buffers.CopyBuffer(copy_address, source_address, clean_read.size(), false, false);
    renderer.PublishGuestWrites();
    std::array<uint8_t, 32> copied {};
    pass &= Memory::TryReadGpuCleanBacking(source_address, clean_read.data(), clean_read.size()) &&
        std::memcmp(clean_read.data(), expected_stencil.data() + source_offset, clean_read.size()) == 0 &&
        Memory::TryReadBacking(copy_address, copied.data(), copied.size()) && copied == clean_read;
    std::array<uint32_t, 4> copy_before {}, copy_after {};
    pass &= Memory::TryReadBacking(copy_address - sizeof(copy_before), copy_before.data(), sizeof(copy_before)) &&
        Memory::TryReadBacking(copy_address + copied.size(), copy_after.data(), sizeof(copy_after));
    for (size_t i = 0; i < copy_before.size(); ++i)
        pass &= copy_before[i] == 0xa5a5a5a5u && copy_after[i] == 0xa5a5a5a5u;
    pass &= backing_matches();
    std::printf("PS5_GPU_TEST case=%s-dirty-stencil-partial-nontexel-read-copy result=%s\n",
        tiled ? "tiled" : "linear", pass ? "PASS" : "FAIL");
    std::printf("PS5_GPU_TEST case=%s-stencil-layer-clear-preserves-depth-padding result=%s\n",
        tiled ? "tiled" : "linear", pass ? "PASS" : "FAIL");

    const auto depth_clear = std::bit_cast<uint32_t>(0.75f);
    pass &= textures.ClearImageFromBuffer(scheduler.Current(), desc.info.data.address, depth_size, depth_clear);
    pass &= textures.GetImage(image).IsGpuModified() && !textures.GetImage(stencil).IsGpuModified();
    for (uint32_t layer = 0; layer < layers; ++layer)
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
                std::memcpy(expected_depth.data() + guest_offset(false, x, y, layer), &depth_clear, sizeof(depth_clear));
    renderer.PublishGuestWrites();
    pass &= backing_matches();
    std::printf("PS5_GPU_TEST case=%s-depth-clear-preserves-stencil-padding result=%s\n",
        tiled ? "tiled" : "linear", pass ? "PASS" : "FAIL");

    // Retirement must resolve an entire dirty owner/association pair before deleting either.
    (void)textures.FindDepthTarget(image, desc);
    textures.GetImage(image).Upload(depth_copies, depth_upload.Handle(), 0, native_depth.size());
    textures.GetImage(image).Upload(stencil_copies, stencil_upload.Handle(), 0, native_stencil.size());
    pass &= textures.GetImage(image).IsGpuModified() && textures.GetImage(stencil).IsGpuModified();
    for (uint32_t layer = 0; layer < layers; ++layer)
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                std::memcpy(expected_depth.data() + guest_offset(false, x, y, layer),
                    native_depth.data() + depth_transfer_slice * layer + (static_cast<uint64_t>(y) * width + x) * 4, 4);
                expected_stencil[guest_offset(true, x, y, layer)] =
                    native_stencil[stencil_transfer_slice * layer + static_cast<uint64_t>(y) * width + x];
            }
    const auto old_image = image, old_stencil = stencil;
    const auto old_stencil_range = desc.info.stencil;
    const auto old_native = textures.GetImage(image).backing.image;
    const auto old_expected_stencil = expected_stencil;
    auto retarget_desc = desc;
    retarget_desc.info.stencil.address = base + (tiled ? 0x340000 : 0x210000);
    std::vector<uint8_t> next_stencil(stencil_size, 0x9e);
    for (uint32_t layer = 0; layer < layers; ++layer)
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
                next_stencil[guest_offset(true, x, y, layer)] =
                    static_cast<uint8_t>(0x40 + x * 19 + y * 23 + layer * 31);
    Memory::WriteBacking(retarget_desc.info.stencil.address, next_stencil.data(), stencil_size);
    const auto next_image = textures.FindImage(retarget_desc);
    // Discovery must complete the old family with its original stencil range
    // before any new stencil upload. It creates a distinct native parent so
    // already prepared old aspect views cannot change to the new allocation.
    pass &= next_image != old_image && textures.GetImage(next_image).backing.image != old_native &&
        !TextureCacheTestAccess::Registered(textures, old_image) &&
        !TextureCacheTestAccess::Registered(textures, old_stencil) && backing_matches();
    (void)textures.FindDepthTarget(next_image, retarget_desc);
    const auto next_association = TextureCacheTestAccess::Stencil(textures, next_image);
    textures.GetImage(next_image).Download(depth_copies, depth_read.Handle(), 0, native_depth.size());
    textures.GetImage(next_image).Download(stencil_copies, stencil_read.Handle(), 0, native_stencil.size());
    scheduler.Finish();
    depth_read.Invalidate(0, native_depth.size());
    stencil_read.Invalidate(0, native_stencil.size());
    pass &= std::memcmp(depth_read.Mapped().data(), native_depth.data(), native_depth.size()) == 0;
    for (uint32_t layer = 0; layer < layers; ++layer)
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
                pass &= static_cast<uint8_t>(stencil_read.Mapped()[stencil_transfer_slice * layer +
                    static_cast<uint64_t>(y) * width + x]) == next_stencil[guest_offset(true, x, y, layer)];
    image = next_image;
    stencil = next_association;
    desc = retarget_desc;
    expected_stencil = next_stencil;
    std::printf("PS5_GPU_TEST case=%s-stencil-retarget-publishes-old-family-before-new-upload result=%s\n",
        tiled ? "tiled" : "linear", pass ? "PASS" : "FAIL");
    textures.UnmapMemory(desc.info.data.address, desc.info.data.size);
    pass &= backing_matches() && !TextureCacheTestAccess::Registered(textures, image) &&
        !TextureCacheTestAccess::Registered(textures, stencil);
    std::vector<uint8_t> old_stencil_backing(stencil_size);
    pass &= Memory::TryReadBacking(old_stencil_range.address, old_stencil_backing.data(), stencil_size) &&
        old_stencil_backing == old_expected_stencil;
    std::printf("PS5_GPU_TEST case=%s-depth-stencil-retirement result=%s\n",
        tiled ? "tiled" : "linear", pass ? "PASS" : "FAIL");
    scheduler.Finish();
    return pass;
}
} // namespace


bool BufferBackingRevalidationPolicyProbe() {
    using namespace Libs::Graphics;
    std::array<uint8_t, TRACKER_PAGE_SIZE> staged {};
    for (size_t i = 0; i < staged.size(); ++i) staged[i] = static_cast<uint8_t>(i * 17u + 3u);
    auto current = staged;
    constexpr uint64_t address = 0x10000;
    GuestBackingSnapshots snapshots;
    bool pass = snapshots.Compare(address, current.data(), current.size()) == BackingComparison::Missing;
    snapshots.Publish(address, staged.data(), staged.size());
    pass &= snapshots.Compare(address, current.data(), current.size()) == BackingComparison::Unchanged;
    // Matching endpoints cannot hide a guest edit in the middle of a page.
    current[current.size() / 2] ^= 0x80;
    pass &= snapshots.Compare(address, current.data(), current.size()) == BackingComparison::Changed;
    for (const auto comparison: {BackingComparison::Missing, BackingComparison::Changed,
                                 BackingComparison::Unchanged}) {
        for (const bool reuse: {false, true}) {
            const auto cpu = ClassifyBackingRevalidation(comparison, false, reuse);
            pass &= cpu == (reuse && comparison == BackingComparison::Unchanged
                ? BackingRevalidation::KeepCpuState : BackingRevalidation::MarkCpuModified);
            const auto gpu = ClassifyBackingRevalidation(comparison, true, reuse);
            pass &= gpu == (comparison == BackingComparison::Unchanged
                ? BackingRevalidation::KeepGpuOwner : BackingRevalidation::RejectGpuOwner);
        }
    }
    snapshots.Publish(address + TRACKER_PAGE_SIZE, staged.data(), staged.size());
    snapshots.Forget(address + TRACKER_PAGE_SIZE / 2, 1);
    pass &= snapshots.Compare(address, staged.data(), staged.size()) == BackingComparison::Missing &&
        snapshots.Compare(address + TRACKER_PAGE_SIZE, staged.data(), staged.size()) == BackingComparison::Unchanged;
    // A retired/reused range requires a new staged witness even for identical bytes.
    snapshots.Forget(address + TRACKER_PAGE_SIZE, TRACKER_PAGE_SIZE);
    pass &= snapshots.Compare(address + TRACKER_PAGE_SIZE, staged.data(), staged.size()) == BackingComparison::Missing;
    std::printf("PS5_GPU_TEST case=buffer-backing-current-page-policy result=%s full-page-central-edit=checked"
                " cpu-explicit-state=retained gpu-missing-changed=refused\n", pass ? "PASS" : "FAIL");
    return pass;
}

bool BufferPublicationWitnessProbe(Libs::Graphics::RenderContext& renderer, uint64_t base) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    auto& buffers = renderer.GetBufferCache();
    auto& scheduler = renderer.GetCommandScheduler();
    const auto* batch_selector = std::getenv("LUMEN_PS5_GFX_BUFFER_PUBLICATION_BATCH");
    const bool enabled = BufferPublicationWitnessEnabled() && GuestBufferSnapshotReuseEnabled() &&
        !GuestAsyncPrivateBackingEnabled() &&
        (batch_selector == nullptr || std::strcmp(batch_selector, "1") == 0);
    constexpr uint64_t size = 0x8000;
    const auto address = base + 0x380000;
    std::vector<uint32_t> expected(size / 4, 0x8a765432u);
    Memory::WriteBacking(address, expected.data(), size);
    auto fill = [&](uint32_t value, uint64_t offset = 0, uint64_t bytes = 0x8000) {
        auto [buffer, native_offset] = buffers.ObtainBuffer(address + offset, bytes, true);
        buffer->Fill(native_offset, bytes, value);
        std::fill(expected.begin() + offset / 4, expected.begin() + (offset + bytes) / 4, value);
    };
    auto native_equal = [&] {
        auto [buffer, offset] = buffers.ObtainBuffer(address, size, false);
        Buffer download(renderer.GetGraphics(), scheduler, MemoryUsage::Download, 0,
            vk::BufferUsageFlagBits::eTransferDst, size);
        download.CopyFrom(scheduler.Current(), *buffer, offset, 0, size);
        scheduler.Finish();
        download.Invalidate(0, size);
        return std::memcmp(download.Mapped().data(), expected.data(), size) == 0;
    };
    bool pass = true;
    fill(0x12345678u);
    const auto before = BufferCacheTestAccess::PublicationWitnessStats(buffers);
    renderer.PublishGuestWrites();
    const auto published = BufferCacheTestAccess::PublicationWitnessStats(buffers);
    pass &= published[0] - before[0] == (enabled ? size / TRACKER_PAGE_SIZE : 0) &&
        Memory::TryEqualBacking(address, expected.data(), size) &&
        !buffers.IsRegionCpuModified(address, size);
    if (!GuestBufferSnapshotReuseEnabled()) {
        pass &= published[0] == before[0] && native_equal();
        std::printf("PS5_GPU_TEST case=publication-witness-reuse-disabled result=%s witnesses=0 native-bytes=checked\n", pass ? "PASS" : "FAIL");
        BufferCacheTestAccess::Retire(buffers, address, size);
        scheduler.Finish();
        return pass;
    }
    auto [short_buffer, short_offset] = buffers.ObtainBuffer(address + 64, 32, false);
    const auto short_consumer = BufferCacheTestAccess::PublicationWitnessStats(buffers);
    pass &= short_consumer[3] - published[3] == (enabled ? 0 : TRACKER_PAGE_SIZE) &&
        short_consumer[2] - published[2] == (enabled ? TRACKER_PAGE_SIZE : 0);
    Buffer short_download(renderer.GetGraphics(), scheduler, MemoryUsage::Download, 0,
        vk::BufferUsageFlagBits::eTransferDst, 32);
    short_download.CopyFrom(scheduler.Current(), *short_buffer, short_offset, 0, 32);
    scheduler.Finish();
    short_download.Invalidate(0, 32);
    pass &= std::memcmp(short_download.Mapped().data(),
        reinterpret_cast<const uint8_t*>(expected.data()) + 64, 32) == 0;
    (void)buffers.ObtainBuffer(address, size, false);
    const auto consumed = BufferCacheTestAccess::PublicationWitnessStats(buffers);
    pass &= consumed[3] - published[3] == (enabled ? 0 : size) &&
        consumed[2] - published[2] == (enabled ? size : 0);
    (void)buffers.ObtainBuffer(address, size, false);
    pass &= BufferCacheTestAccess::PublicationWitnessStats(buffers)[2] == consumed[2] && native_equal();
    std::printf("PS5_GPU_TEST case=publication-witness-native-page-reuse result=%s enabled=%u actual_upload_bytes=%llu witness_pages=%llu padding=full-page first-consumer-only=checked short-consumer=32 baseline-page=4096\n",
        pass ? "PASS" : "FAIL", enabled ? 1u : 0u,
        static_cast<unsigned long long>(consumed[3] - published[3]),
        static_cast<unsigned long long>(published[0] - before[0]));

    // A CPU edit in padding outside a later 32-byte consumer must still cause
    // upload of the entire changed page, in this same exclusive epoch.
    expected[TRACKER_PAGE_SIZE / 4 - 1] ^= 0x80008000u;
    Memory::WriteBacking(address + TRACKER_PAGE_SIZE - 4, &expected[TRACKER_PAGE_SIZE / 4 - 1], 4);
    BufferCacheTestAccess::Revalidate(buffers, address + 64, 32);
    pass &= buffers.IsRegionCpuModified(address, TRACKER_PAGE_SIZE);
    const auto cpu_changed = BufferCacheTestAccess::PublicationWitnessStats(buffers);
    pass &= native_equal();
    pass &= BufferCacheTestAccess::PublicationWitnessStats(buffers)[3] - cpu_changed[3] == TRACKER_PAGE_SIZE;

    fill(0x87654321u);
    BufferCacheTestAccess::Revalidate(buffers, address, size);
    pass &= buffers.IsRegionGpuModified(address, size);
    renderer.PublishGuestWrites();
    pass &= Memory::TryEqualBacking(address, expected.data(), size) && native_equal();
    std::printf("PS5_GPU_TEST case=publication-witness-cpu-padding-and-gpu-redirty result=%s same-epoch=checked native-bytes=checked\n", pass ? "PASS" : "FAIL");

    fill(0xabcdef01u, 16, 64);
    renderer.PublishGuestWrites();
    pass &= !BufferCacheTestAccess::HasPublicationWitness(buffers, address) &&
        Memory::TryEqualBacking(address, expected.data(), size) && native_equal();
    std::printf("PS5_GPU_TEST case=publication-witness-partial-page-denied result=%s clean-padding=preserved\n", pass ? "PASS" : "FAIL");

    fill(0x1357a9bdu, 16, TRACKER_PAGE_SIZE * 2);
    fill(0x4680ce12u, TRACKER_PAGE_SIZE * 4 + 32, TRACKER_PAGE_SIZE * 2);
    const auto interior_before = BufferCacheTestAccess::PublicationWitnessStats(buffers);
    renderer.PublishGuestWrites();
    pass &= BufferCacheTestAccess::PublicationWitnessStats(buffers)[0] - interior_before[0] ==
        (enabled ? 2 : 0) && !BufferCacheTestAccess::HasPublicationWitness(buffers, address) &&
        BufferCacheTestAccess::HasPublicationWitness(buffers, address + TRACKER_PAGE_SIZE) == enabled &&
        BufferCacheTestAccess::HasPublicationWitness(buffers, address + 5 * TRACKER_PAGE_SIZE) == enabled &&
        !BufferCacheTestAccess::HasPublicationWitness(buffers, address + 2 * TRACKER_PAGE_SIZE) &&
        !BufferCacheTestAccess::HasPublicationWitness(buffers, address + 4 * TRACKER_PAGE_SIZE) &&
        !BufferCacheTestAccess::HasPublicationWitness(buffers, address + 6 * TRACKER_PAGE_SIZE) &&
        Memory::TryEqualBacking(address, expected.data(), size) && native_equal();
    std::printf("PS5_GPU_TEST case=publication-witness-interior-full-page result=%s unaligned-copies=2 packed-bytes=16384 interior-pages=%u boundary-padding=preserved\n",
        pass ? "PASS" : "FAIL", enabled ? 2u : 0u);

    fill(0x514a93e7u);
    renderer.PublishGuestWrites();
    buffers.InvalidateMemory(address, TRACKER_PAGE_SIZE);
    BufferCacheTestAccess::Revalidate(buffers, address, TRACKER_PAGE_SIZE);
    pass &= buffers.IsRegionCpuModified(address, TRACKER_PAGE_SIZE) && native_equal();
    fill(0x8b82c014u);
    scheduler.DeferOperation([] {});
    const auto pending = BufferCacheTestAccess::PublicationWitnessStats(buffers);
    buffers.PublishGuestWrites(); // No outer drain: the pending callback denies capture.
    pass &= BufferCacheTestAccess::PublicationWitnessStats(buffers)[0] == pending[0] &&
        Memory::TryEqualBacking(address, expected.data(), size) && native_equal();
    std::printf("PS5_GPU_TEST case=publication-witness-dirty-and-callback-denial result=%s explicit-dirty=retained pending-callback=no-witness\n", pass ? "PASS" : "FAIL");
    BufferCacheTestAccess::Retire(buffers, address, size);
    scheduler.Finish();
    return pass;
}

bool BufferBackingRevalidationProbe(Libs::Graphics::RenderContext& renderer, uint64_t base) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    auto& buffers = renderer.GetBufferCache();
    auto& scheduler = renderer.GetCommandScheduler();
    constexpr uint64_t size = 0x8000; // Larger than the one-off streaming shortcut.
    const uint64_t source_address = base + 0x2e0000;
    const uint64_t target_address = source_address + size;
    std::vector<uint32_t> expected(size / sizeof(uint32_t));
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = 0x13570000u + i;
    Memory::WriteBacking(source_address, expected.data(), size);
    auto [source, source_offset] = buffers.ObtainBuffer(source_address, size, false);
    bool pass = !buffers.IsRegionCpuModified(source_address, size) &&
        BufferCacheTestAccess::Compare(buffers, source_address, expected.data(), TRACKER_PAGE_SIZE) == BackingComparison::Unchanged;
    const auto hash_pages_before = BufferCacheTestAccess::ContiguousHashPages(buffers);
    BufferCacheTestAccess::Revalidate(buffers, source_address, size);
    pass &= !buffers.IsRegionCpuModified(source_address, size);
    const bool direct_hash = BufferContiguousHashEnabled() && GuestGpuWriteOwnershipSupported() &&
        !GuestAsyncCpuVisibleExperimentEnabled() && !GuestAsyncCpuConcurrentExperimentRequested();
    pass &= BufferCacheTestAccess::ContiguousHashPages(buffers) - hash_pages_before ==
        (direct_hash ? size / TRACKER_PAGE_SIZE : 0);

    // A short consumer still validates its entire tracking page, including
    // padding far outside that consumer. Repeated calls share this GPU epoch.
    const auto padding_word = TRACKER_PAGE_SIZE / sizeof(uint32_t) - 1;
    expected[padding_word] ^= 0x00080000u;
    Memory::WriteBacking(source_address + padding_word * sizeof(uint32_t), &expected[padding_word], sizeof(uint32_t));
    BufferCacheTestAccess::Revalidate(buffers, source_address + 128, 32);
    if (GuestBufferSnapshotReuseEnabled()) pass &= buffers.IsRegionCpuModified(source_address, TRACKER_PAGE_SIZE);
    std::tie(source, source_offset) = buffers.ObtainBuffer(source_address, size, false);

    const auto changed_page = source_address + size / 2;
    expected[expected.size() / 2 + 31] ^= 0x80000000u;
    Memory::WriteBacking(source_address, expected.data(), size);
    BufferCacheTestAccess::Revalidate(buffers, source_address, size);
    if (GuestBufferSnapshotReuseEnabled()) {
        pass &= buffers.IsRegionCpuModified(changed_page, TRACKER_PAGE_SIZE) &&
            !buffers.IsRegionCpuModified(source_address, TRACKER_PAGE_SIZE);
    }
    std::tie(source, source_offset) = buffers.ObtainBuffer(source_address, size, false);
    auto [target, target_offset] = buffers.ObtainBuffer(target_address, size, true);
    target->CopyFrom(scheduler.Current(), *source, source_offset, target_offset, size);
    renderer.PublishGuestWrites();
    std::vector<uint32_t> actual(expected.size());
    pass &= Memory::TryReadBacking(target_address, actual.data(), size) && actual == expected;

    buffers.InvalidateMemory(source_address, TRACKER_PAGE_SIZE);
    pass &= buffers.IsRegionCpuModified(source_address, TRACKER_PAGE_SIZE);
    BufferCacheTestAccess::Revalidate(buffers, source_address, TRACKER_PAGE_SIZE);
    pass &= buffers.IsRegionCpuModified(source_address, TRACKER_PAGE_SIZE);
    (void)buffers.ObtainBuffer(source_address, size, false);
    pass &= !buffers.IsRegionCpuModified(source_address, size);
    BufferCacheTestAccess::Retire(buffers, source_address, size);
    pass &= BufferCacheTestAccess::Compare(buffers, source_address, expected.data(), TRACKER_PAGE_SIZE) == BackingComparison::Missing;
    (void)buffers.ObtainBuffer(source_address, size, false);
    pass &= BufferCacheTestAccess::Compare(buffers, source_address, expected.data(), TRACKER_PAGE_SIZE) == BackingComparison::Unchanged;
    renderer.PublishGuestWrites();
    std::printf("PS5_GPU_TEST case=buffer-backing-revalidation-native-copy result=%s reuse=%u"
                " contiguous_hash=%u hashed_pages=%llu full-page-padding=checked same-epoch-write=checked"
                " changed-page=checked explicit-cpu-dirty=retained retire-reacquire=fresh\n",
                pass ? "PASS" : "FAIL", GuestBufferSnapshotReuseEnabled() ? 1u : 0u,
                direct_hash ? 1u : 0u, static_cast<unsigned long long>(
                    BufferCacheTestAccess::ContiguousHashPages(buffers) - hash_pages_before));
    return pass;
}

bool BufferPublicationBatchProbe(Libs::Graphics::RenderContext& renderer, uint64_t base) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    auto& buffers = renderer.GetBufferCache();
    auto& scheduler = renderer.GetCommandScheduler();
    constexpr uint32_t canary = 0xa5a5a5a5u;
    constexpr uint64_t written_offset = 16, written_bytes = 64;
    const std::array<uint64_t, 2> addresses {base + 0x2c0000, base + 0x2c8000};
    const std::array<uint32_t, 2> values {0x12345678u, 0x89abcdefu};
    std::array<uint32_t, TRACKER_PAGE_SIZE / sizeof(uint32_t)> seed;
    seed.fill(canary);
    for (size_t i = 0; i < addresses.size(); ++i) {
        Memory::WriteBacking(addresses[i], seed.data(), sizeof(seed));
        auto [buffer, offset] = buffers.ObtainBuffer(addresses[i] + written_offset, written_bytes, true);
        buffer->Fill(offset, written_bytes, values[i]);
    }
    const auto* selector = std::getenv("LUMEN_PS5_GFX_BUFFER_PUBLICATION_BATCH");
    const bool batch = selector == nullptr || std::strcmp(selector, "1") == 0;
    const auto before = scheduler.CurrentTick();
    buffers.PublishGuestWrites();
    const auto submitted = scheduler.CurrentTick() - before;
    bool pass = submitted == (batch ? 1u : 2u);
    for (size_t i = 0; i < addresses.size(); ++i) {
        std::array<uint32_t, 24> actual {};
        pass &= Memory::TryReadBacking(addresses[i], actual.data(), sizeof(actual)) &&
            !buffers.HasGpuDirtyBytes(addresses[i], TRACKER_PAGE_SIZE) &&
            !buffers.IsRegionGpuModified(addresses[i], TRACKER_PAGE_SIZE);
        for (size_t word = 0; word < actual.size(); ++word)
            pass &= actual[word] == (word >= written_offset / sizeof(uint32_t) &&
                word < (written_offset + written_bytes) / sizeof(uint32_t) ? values[i] : canary);
    }
    std::printf("PS5_GPU_TEST case=two-buffer-publication-batch result=%s batch=%u submits=%llu"
                " exact-dirty-bytes=checked padding=preserved owners=released-after-completion\n",
                pass ? "PASS" : "FAIL", batch ? 1u : 0u, static_cast<unsigned long long>(submitted));

    // Repeated clean publication must neither submit work nor consume a later
    // writer. The renderer still completes both callback lanes with no dirty
    // buffers, before admitting CPU access to this backing.
    const auto clean_tick = scheduler.CurrentTick();
    for (unsigned repeat = 0; repeat != 3; ++repeat) buffers.PublishGuestWrites();
    pass &= scheduler.CurrentTick() == clean_tick && !buffers.HasCpuVisibleGpuWrites();
    std::atomic<unsigned> callbacks {0};
    scheduler.DeferPriorityOperation([&] { callbacks.fetch_or(1u, std::memory_order_release); });
    scheduler.DeferOperation([&] { callbacks.fetch_or(2u, std::memory_order_release); });
    renderer.PublishGuestWrites();
    pass &= callbacks.load(std::memory_order_acquire) == 3u;

    constexpr uint64_t rewritten_offset = 8, rewritten_bytes = 16;
    constexpr uint32_t rewritten_value = 0x76543210u;
    auto [rewritten, offset] = buffers.ObtainBuffer(addresses[0] + rewritten_offset,
                                                   rewritten_bytes, true);
    rewritten->Fill(offset, rewritten_bytes, rewritten_value);
    pass &= buffers.HasGpuDirtyBytes(addresses[0] + rewritten_offset, rewritten_bytes) &&
        buffers.IsRegionGpuModified(addresses[0], TRACKER_PAGE_SIZE) &&
        !buffers.HasGpuDirtyBytes(addresses[1], TRACKER_PAGE_SIZE);
    buffers.PublishGuestWrites();
    for (size_t i = 0; i < addresses.size(); ++i) {
        std::array<uint32_t, 24> actual {};
        pass &= Memory::TryReadBacking(addresses[i], actual.data(), sizeof(actual)) &&
            !buffers.HasGpuDirtyBytes(addresses[i], TRACKER_PAGE_SIZE) &&
            !buffers.IsRegionGpuModified(addresses[i], TRACKER_PAGE_SIZE);
        for (size_t word = 0; word < actual.size(); ++word) {
            const auto byte = word * sizeof(uint32_t);
            const auto expected = i == 0 && byte >= rewritten_offset &&
                byte < rewritten_offset + rewritten_bytes ? rewritten_value :
                byte >= written_offset && byte < written_offset + written_bytes ? values[i] : canary;
            pass &= actual[word] == expected;
        }
    }
    std::printf("PS5_GPU_TEST case=clean-buffer-publication-redirty result=%s"
                " clean-repeats=3 priority-and-ordinary-callbacks=completed"
                " later-writer=published peer-and-padding=preserved\n", pass ? "PASS" : "FAIL");
    return pass;
}

bool CompletionTransactionProbe(Libs::Graphics::RenderContext& renderer) {
    using namespace Libs::Graphics;
    auto& scheduler = renderer.GetCommandScheduler();
    scheduler.Finish();
    scheduler.DrainPriorityOperations();
    const auto tick = scheduler.CurrentTick();
    std::atomic<unsigned> priority {0};
    unsigned ordinary = 0, nested = 0;
    scheduler.DeferPriorityOperation([&] { priority.fetch_add(1, std::memory_order_release); });
    scheduler.DeferOperation([&] {
        ++ordinary;
        // Completion must have restored recording before it permits resource
        // callbacks to enqueue retirement on the new native timeline tick.
        (void)scheduler.Current();
        scheduler.DeferOperation([&] { ++nested; });
    });
    auto prepared = scheduler.PrepareCompletion();
    bool pass = prepared.Valid() && prepared.Tick() == tick && ordinary == 0 && nested == 0 &&
        scheduler.CurrentTick() == tick + 1 && GuestGpuWriteOwnershipSupported();
    auto completion = std::move(prepared);
    pass &= !prepared.Valid() && completion.Valid();
    scheduler.WaitNativeCompletion(completion);
    // A repeated native wait is harmless and still cannot perform ordinary
    // callback retirement. The independent priority lane remains explicit.
    scheduler.WaitNativeCompletion(completion);
    scheduler.WaitPriorityOperations(completion.Tick());
    pass &= scheduler.IsFree(tick) && ordinary == 0 && nested == 0 &&
        priority.load(std::memory_order_acquire) == 1 && GuestGpuWriteOwnershipSupported();
    scheduler.CompleteDeferredCompletion(std::move(completion));
    pass &= !completion.Valid() && ordinary == 1 && nested == 0 &&
        scheduler.CurrentTick() == tick + 1;
    // The nested retirement belongs to the new recording, not to the earlier
    // completed ticket. A subsequent normal Finish preserves that boundary.
    scheduler.Finish();
    pass &= ordinary == 1 && nested == 1 &&
        priority.load(std::memory_order_acquire) == 1 && GuestGpuWriteOwnershipSupported();
    std::printf("PS5_GPU_TEST case=native-completion-transaction result=%s"
                " native-wait=ordinary-callback-free priority-lane=explicit"
                " nested-retirement=next-tick guest-authority=retained\n",
                pass ? "PASS" : "FAIL");
    return pass;
}

bool GarbageCollectionPublicationProbe(Libs::Graphics::RenderContext& renderer, uint64_t base) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    auto& buffers = renderer.GetBufferCache();
    auto& textures = renderer.GetTextureCache();
    auto& scheduler = renderer.GetCommandScheduler();
    constexpr uint32_t canary = 0xa5a5a5a5u;
    constexpr uint64_t image_size = 8192, written_offset = 16, written_bytes = 64;
    // Keep this fixture beyond the depth/stencil retarget and color inverse ranges,
    // whose before/after guards intentionally retain the mapping's initial canary.
    const uint64_t image_address = base + 0x3e0000;
    const uint64_t buffer_address = base + 0x3e8000;
    std::array<uint32_t, image_size / sizeof(uint32_t)> seed;
    seed.fill(canary);
    Memory::WriteBacking(image_address, seed.data(), sizeof(seed));
    Memory::WriteBacking(buffer_address, seed.data(), sizeof(seed));

    TextureCache::ImageDesc desc;
    desc.type = TextureCache::BindingType::RenderTarget;
    desc.info.data = {image_address, image_size};
    desc.info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    desc.info.guest_format = Prospero::BufferFormat::k8_8_8_8UNorm;
    desc.info.type = Prospero::ImageType::kColor2D;
    desc.info.extent = {32, 32, 1};
    desc.info.resources = {1, 1};
    desc.info.pitch = 64;
    desc.info.bytes_per_block = 4;
    desc.info.UpdateSize();
    desc.view_info.format = desc.info.pixel_format;
    desc.view_info.type = vk::ImageViewType::e2D;
    desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
    desc.view_info.usage = vk::ImageUsageFlagBits::eColorAttachment;
    const auto image = textures.FindImage(desc);
    (void)textures.FindRenderTarget(image, desc);
    bool pass = textures.ClearImageFromBuffer(scheduler.Current(), image_address, image_size,
                                              0xff0000ffu);
    auto [buffer, offset] = buffers.ObtainBuffer(buffer_address + written_offset, written_bytes, true);
    buffer->Fill(offset, written_bytes, 0x12345678u);
    bool sentinel = false;
    scheduler.DeferPriorityOperation([&] { sentinel = true; });

    const auto old_buffer_trigger = BufferCacheTestAccess::SetCollectionTrigger(
        buffers, std::numeric_limits<uint64_t>::max());
    const auto old_image_trigger = TextureCacheTestAccess::SetCollectionTrigger(
        textures, std::numeric_limits<uint64_t>::max());
    const auto before_gc = scheduler.CurrentTick();
    renderer.RunGarbageCollector();
    BufferCacheTestAccess::SetCollectionTrigger(buffers, old_buffer_trigger);
    TextureCacheTestAccess::SetCollectionTrigger(textures, old_image_trigger);

    std::array<uint32_t, image_size / sizeof(uint32_t)> pixels {};
    std::array<uint32_t, 24> actual {};
    const auto backing_matches = [&](uint32_t color, uint32_t fill) {
        bool matches = Memory::TryReadBacking(image_address, pixels.data(), sizeof(pixels)) &&
            Memory::TryReadBacking(buffer_address, actual.data(), sizeof(actual));
        for (size_t word = 0; word < pixels.size(); ++word)
            matches &= pixels[word] == (word % 64 < 32 ? color : canary);
        for (size_t word = 0; word < actual.size(); ++word)
            matches &= actual[word] == (word >= written_offset / sizeof(uint32_t) &&
                word < (written_offset + written_bytes) / sizeof(uint32_t) ? fill : canary);
        return matches;
    };
    const bool deferred = GuestGcPublicationDeferred();
    if (deferred) {
        pass &= scheduler.CurrentTick() == before_gc && !sentinel &&
            buffers.HasGpuDirtyBytes(buffer_address, TRACKER_PAGE_SIZE) &&
            buffers.IsRegionGpuModified(buffer_address, TRACKER_PAGE_SIZE) &&
            textures.GetImage(image).IsGpuModified() && backing_matches(canary, canary);
    } else {
        pass &= sentinel && scheduler.CurrentTick() > before_gc &&
            !buffers.HasGpuDirtyBytes(buffer_address, TRACKER_PAGE_SIZE) &&
            !textures.GetImage(image).IsGpuModified() && backing_matches(0xff0000ffu, 0x12345678u);
    }
    renderer.PublishGuestWrites();
    pass &= sentinel && !buffers.HasGpuDirtyBytes(buffer_address, TRACKER_PAGE_SIZE) &&
        !buffers.IsRegionGpuModified(buffer_address, TRACKER_PAGE_SIZE) &&
        !textures.GetImage(image).IsGpuModified() && backing_matches(0xff0000ffu, 0x12345678u);
    std::printf("PS5_GPU_TEST case=gc-retained-owner-publication result=%s deferred=%u"
                " low-pressure=checked priority-drain=checked exact-bytes-padding=checked\n",
                pass ? "PASS" : "FAIL", deferred ? 1u : 0u);

    // Retiring a selected dirty owner must publish even when the blanket GC drain is deferred.
    pass &= textures.ClearImageFromBuffer(scheduler.Current(), image_address, image_size,
                                          0xff00ff00u);
    TextureCacheTestAccess::Retire(textures, image);
    pass &= !TextureCacheTestAccess::Registered(textures, image) &&
        backing_matches(0xff00ff00u, 0x12345678u);
    std::tie(buffer, offset) = buffers.ObtainBuffer(buffer_address + written_offset, written_bytes, true);
    buffer->Fill(offset, written_bytes, 0x89abcdefu);
    BufferCacheTestAccess::CollectAgedDirtyBuffers(buffers);
    pass &= !buffers.IsRegionRegistered(buffer_address, TRACKER_PAGE_SIZE) &&
        !buffers.HasGpuDirtyBytes(buffer_address, TRACKER_PAGE_SIZE) &&
        !buffers.IsRegionGpuModified(buffer_address, TRACKER_PAGE_SIZE) &&
        backing_matches(0xff00ff00u, 0x89abcdefu);
    renderer.PublishGuestWrites();
    std::printf("PS5_GPU_TEST case=gc-selected-dirty-owner-retirement result=%s"
                " image-free=completed buffer-collection=completed padding=preserved\n",
                pass ? "PASS" : "FAIL");
    return pass;
}

// These fixtures cross a real FEX epoch boundary before granting CPU access.
// A cached owner must keep its native payload and publish every byte first.
bool AsyncPrivateBackingProbe(Libs::Graphics::RenderContext& renderer) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    constexpr uint64_t size = 0x10000;
    constexpr uint32_t seed = 0xa5a5a5a5u;
    constexpr uint32_t fill = 0x24681357u;
    void* mapping = nullptr;
    if (Memory::KernelMapFlexibleMemory(&mapping, size, 0x32, 0) != OK) return false;
    const auto base = reinterpret_cast<uint64_t>(mapping);
    void* visible_mapping = nullptr;
    if (Memory::KernelMapFlexibleMemory(&visible_mapping, 0x4000, 0x33, 0) != OK) return false;
    const auto visible_base = reinterpret_cast<uint64_t>(visible_mapping);
    std::array<uint32_t, 0x4000 / 4> visible_seed {};
    visible_seed.fill(seed);
    Memory::WriteBacking(visible_base, visible_seed.data(), sizeof(visible_seed));
    std::vector<uint32_t> expected(size / 4, seed);
    Memory::WriteBacking(base, expected.data(), size);
    bool pass = Memory::KernelMprotect(mapping, size, 0x30) == OK;
    HW::Context hw;
    HW::UserConfig user;
    HW::Shader shader;
    const char* selector = std::getenv("LUMEN_PS5_GFX_ASYNC_PRIVATE_BACKING");
    const bool enabled = selector != nullptr && std::strcmp(selector, "1") == 0;
    renderer.GetGpu().SendCommandSync([&] {
        GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
        const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (epoch == 0) EXIT("private backing fixture epoch denied\n");
        GuestGpuPublicationActive = true;
        auto& scheduler = renderer.GetCommandScheduler();
        scheduler.Begin(hw, user, shader);
        auto& buffers = renderer.GetBufferCache();
        auto [buffer, offset] = buffers.ObtainBuffer(base + 16, 64, true);
        buffer->Fill(offset, 64, fill);
        TextureCache::ImageDesc desc;
        desc.type = TextureCache::BindingType::RenderTarget;
        desc.info.data = {base + 0x8000, 8192};
        desc.info.pixel_format = vk::Format::eR8G8B8A8Unorm;
        desc.info.guest_format = Prospero::BufferFormat::k8_8_8_8UNorm;
        desc.info.type = Prospero::ImageType::kColor2D;
        desc.info.extent = {32, 32, 1};
        desc.info.resources = {1, 1};
        desc.info.pitch = 64;
        desc.info.bytes_per_block = 4;
        desc.info.UpdateSize();
        desc.view_info.format = desc.info.pixel_format;
        desc.view_info.type = vk::ImageViewType::e2D;
        desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
        desc.view_info.usage = vk::ImageUsageFlagBits::eColorAttachment;
        auto& textures = renderer.GetTextureCache();
        const auto image = textures.FindImage(desc);
        (void)textures.FindRenderTarget(image, desc);
        pass &= textures.ClearImageFromBuffer(scheduler.Current(), desc.info.data.address,
            desc.info.data.size, 0xff123456u);
        auto [visible_buffer, visible_offset] = buffers.ObtainBuffer(visible_base + 16, 64, true);
        visible_buffer->Fill(visible_offset, 64, fill);
        std::atomic<unsigned> callbacks {0};
        scheduler.DeferPriorityOperation([&] { callbacks.fetch_or(1u, std::memory_order_release); });
        scheduler.DeferOperation([&] { callbacks.fetch_or(2u, std::memory_order_release); });
        renderer.FinishGuestGpuSlice(true);
        pass &= callbacks.load(std::memory_order_acquire) == 3 &&
            !buffers.HasGpuDirtyBytes(visible_base, 0x4000) &&
            buffers.HasGpuDirtyBytes(base, 0x4000) == enabled &&
            textures.GetImage(image).IsGpuModified() == enabled;
        std::array<uint32_t, 0x4000 / 4> visible_actual {};
        pass &= Memory::ReadGuestMemory(nullptr, visible_base, visible_actual.data(), sizeof(visible_actual));
        for (size_t i = 0; i < visible_actual.size(); ++i)
            pass &= visible_actual[i] == (i >= 4 && i < 20 ? fill : seed);
        // Readmission does not grant CPU permission to unpublished private pages.
        uint32_t forbidden = 0;
        pass &= !Memory::ReadGuestMemory(nullptr, base + 16, &forbidden, sizeof(forbidden));
        GuestGpuPublicationActive = false;
        pass &= LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0;
    });
    // A CPU-data output cannot overlap an owner which denies every physical
    // alias's CPU rights. It must neither drain unrelated private GPU work nor
    // let a rejected private write publish it as a side effect.
    constexpr uint32_t cpu_output = 0x10293847u;
    pass &= Memory::WriteGuestMemory(nullptr, visible_base + 80, &cpu_output, sizeof(cpu_output));
    uint32_t cpu_actual = 0;
    pass &= Memory::ReadGuestMemory(nullptr, visible_base + 80, &cpu_actual, sizeof(cpu_actual)) &&
        cpu_actual == cpu_output;
    pass &= !Memory::WriteGuestMemory(nullptr, base + 16, &cpu_output, sizeof(cpu_output));
    renderer.GetGpu().SendCommandSync([&] {
        pass &= renderer.GetBufferCache().HasGpuDirtyBytes(base, 0x4000) == enabled &&
            renderer.GetTextureCache().IsRegionGpuModified(base + 0x8000, 8192) == enabled;
        uint32_t backing = 0;
        pass &= Memory::TryReadBacking(base + 16, &backing, sizeof(backing)) &&
            backing == (enabled ? seed : fill);
    });
    for (size_t i = 4; i < 20; ++i) expected[i] = fill;
    for (size_t i = 0; i < 2048; ++i)
        if (i % 64 < 32) expected[0x8000 / 4 + i] = 0xff123456u;
    // The VM hook must complete both native downloads before publishing Read.
    pass &= Memory::KernelMprotect(mapping, size, 0x31) == OK;
    std::vector<uint32_t> actual(expected.size());
    pass &= Memory::ReadGuestMemory(nullptr, base, actual.data(), size) && actual == expected;
    pass &= Memory::KernelMunmap(base, size) == OK;
    pass &= Memory::KernelMunmap(visible_base, 0x4000) == OK;
    std::printf("PS5_GPU_TEST case=private-buffer-color-epoch-mprotect result=%s async=%u"
        " full-bytes-padding=checked cpu-before-grant=denied mixed-visible-callbacks=completed"
        " unrelated-cpu-output=preserves-private denied-write=preserves-private\n", pass ? "PASS" : "FAIL", enabled ? 1u : 0u);
    return pass;
}

bool BufferOverlapSnapshotTransferProbe(Libs::Graphics::RenderContext& renderer) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    constexpr uint64_t page_size = 0x4000, size = 3 * page_size;
    constexpr uint64_t first_offset = 32, first_bytes = 128;
    constexpr uint64_t last_offset = 2 * page_size + 96, last_bytes = 256;
    int64_t physical = -1;
    if (Memory::KernelAllocateMainDirectMemory(size, page_size, 0, &physical) != OK) return false;
    // A distinct virtual alignment avoids reacquiring clean cache entries from
    // preceding fixtures. Every native owner below must fit this exact mapping.
    void* mapping = nullptr;
    if (Memory::KernelMapDirectMemory(&mapping, size, 0x30, 0, physical, 0x100000) != OK) {
        (void)Memory::KernelCheckedReleaseDirectMemory(physical, size);
        return false;
    }
    const auto base = reinterpret_cast<uint64_t>(mapping);
    std::vector<uint32_t> expected(size / 4);
    for (size_t i = 0; i < expected.size(); ++i)
        expected[i] = 0x60000000u + static_cast<uint32_t>(i);
    Memory::WriteBacking(base, expected.data(), size);
    HW::Context hw;
    HW::UserConfig user;
    HW::Shader shader;
    bool pass = true;
    std::array<bool, 8> phases;
    phases.fill(true);
    const auto check = [&](size_t phase, bool result) {
        phases[phase] &= result;
        pass &= result;
    };
    const bool enabled = GuestAsyncPrivateBackingEnabled();
    const auto set_expected = [&](uint32_t first, uint32_t last) {
        std::fill(expected.begin() + first_offset / 4,
            expected.begin() + (first_offset + first_bytes) / 4, first);
        std::fill(expected.begin() + last_offset / 4,
            expected.begin() + (last_offset + last_bytes) / 4, last);
    };
    renderer.GetGpu().SendCommandSync([&] {
        GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
        const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (epoch == 0) EXIT("buffer snapshot transfer fixture epoch denied\n");
        GuestGpuPublicationActive = true;
        auto& scheduler = renderer.GetCommandScheduler();
        scheduler.Begin(hw, user, shader);
        auto& buffers = renderer.GetBufferCache();
        // VM release preserves clean cache registrations. Retire an overlap
        // from preceding fixtures before asserting exactly two native owners
        // and an uncached gap. Mapping admission already published old writes.
        if (buffers.IsRegionRegistered(base, size)) {
            if (buffers.HasGpuDirtyBytes(base, size) || buffers.IsRegionGpuModified(base, size))
                EXIT("buffer snapshot transfer fixture reused a dirty cache owner\n");
            BufferCacheTestAccess::Retire(buffers, base, size);
            scheduler.Finish();
            scheduler.WaitPriorityOperations(scheduler.CurrentTick() - 1);
            check(0, !buffers.IsRegionRegistered(base, size));
            std::printf("PS5_GPU_TEST case=buffer-dirty-overlap-snapshot-transfer"
                " diagnostic=retired-previous-clean-cache bytes=49152\n");
        }
        auto [first, first_native_offset] = buffers.ObtainBuffer(base + first_offset, first_bytes, true);
        const auto first_base = first->CpuAddress(), first_size = first->Size();
        const bool first_bounds = first_base == base && first_size == page_size;
        check(0, first_bounds);
        first->Fill(first_native_offset, first_bytes, 0x10203040u);
        const auto first_handle = first->Handle();
        auto [last, last_native_offset] = buffers.ObtainBuffer(base + last_offset, last_bytes, true);
        const bool last_bounds = last->CpuAddress() == base + 2 * page_size && last->Size() == page_size;
        check(0, last_bounds);
        last->Fill(last_native_offset, last_bytes, 0x50607080u);
        const bool distinct = first_handle != last->Handle();
        const bool first_dirty = buffers.HasGpuDirtyBytes(base, page_size);
        const bool last_dirty = buffers.HasGpuDirtyBytes(base + 2 * page_size, page_size);
        const bool gap_empty = !buffers.IsRegionRegistered(base + page_size, page_size);
        check(0, distinct && first_dirty && last_dirty && gap_empty);
        if (!phases[0])
            std::printf("PS5_GPU_TEST case=buffer-dirty-overlap-snapshot-transfer diagnostic=phase0"
                " first-bounds=%u last-bounds=%u distinct=%u first-dirty=%u last-dirty=%u gap-empty=%u"
                " base=0x%016llx first=0x%016llx:%llu last=0x%016llx:%llu\n",
                first_bounds ? 1u : 0u, last_bounds ? 1u : 0u, distinct ? 1u : 0u,
                first_dirty ? 1u : 0u, last_dirty ? 1u : 0u, gap_empty ? 1u : 0u,
                static_cast<unsigned long long>(base), static_cast<unsigned long long>(first_base),
                static_cast<unsigned long long>(first_size), static_cast<unsigned long long>(last->CpuAddress()),
                static_cast<unsigned long long>(last->Size()));
        renderer.FinishGuestGpuSlice(true);
        check(1, buffers.HasGpuDirtyBytes(base, page_size) == enabled &&
            buffers.HasGpuDirtyBytes(base + 2 * page_size, page_size) == enabled);
        GuestGpuPublicationActive = false;
        check(1, LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0);
    });
    set_expected(0x90a0b0c0u, 0xd0e0f001u);
    renderer.GetGpu().SendCommandSync([&] {
        GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
        const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (epoch == 0) EXIT("buffer snapshot replacement fixture epoch denied\n");
        GuestGpuPublicationActive = true;
        auto& scheduler = renderer.GetCommandScheduler();
        scheduler.Begin(hw, user, shader);
        auto& buffers = renderer.GetBufferCache();
        // Rewrite both old buffers so this transfer is GPU-dirty with ON and OFF.
        auto [first, first_native_offset] = buffers.ObtainBuffer(base + first_offset, first_bytes, true);
        first->Fill(first_native_offset, first_bytes, 0x90a0b0c0u);
        const auto first_handle = first->Handle();
        auto [last, last_native_offset] = buffers.ObtainBuffer(base + last_offset, last_bytes, true);
        last->Fill(last_native_offset, last_bytes, 0xd0e0f001u);
        const auto last_handle = last->Handle();
        check(2, first_handle != last_handle &&
            buffers.HasGpuDirtyBytes(base, page_size) &&
            buffers.HasGpuDirtyBytes(base + 2 * page_size, page_size));
        // Joining both native owners must not forget their CPU baselines before
        // SynchronizeBuffer checks the larger request and uploads the clean gap.
        auto [combined, combined_offset] = buffers.ObtainBuffer(base, size, false);
        check(2, combined->Handle() != first_handle && combined->Handle() != last_handle &&
            combined->CpuAddress() == base && combined->Size() == size && combined_offset == 0);
        std::array<uint8_t, TRACKER_PAGE_SIZE> snapshot {};
        for (const auto address: {base, base + 2 * page_size}) {
            check(3, Memory::TryReadBacking(address, snapshot.data(), snapshot.size()) &&
                BufferCacheTestAccess::Compare(buffers, address, snapshot.data(), snapshot.size()) ==
                    BackingComparison::Unchanged);
        }
        Buffer readback(renderer.GetGraphics(), scheduler, MemoryUsage::Download, 0,
            vk::BufferUsageFlagBits::eTransferDst, size);
        readback.CopyFrom(scheduler.Current(), *combined, combined_offset, 0, size);
        scheduler.Finish();
        scheduler.WaitPriorityOperations(scheduler.CurrentTick() - 1);
        readback.Invalidate(0, size);
        const bool native_matches = std::memcmp(readback.Mapped().data(), expected.data(), size) == 0;
        check(4, native_matches);
        if (!native_matches) {
            const auto* native = readback.Mapped().data();
            const auto* oracle = reinterpret_cast<const uint8_t*>(expected.data());
            size_t first_difference = 0;
            while (first_difference < size && native[first_difference] == oracle[first_difference])
                ++first_difference;
            std::printf("PS5_GPU_TEST case=buffer-dirty-overlap-snapshot-transfer diagnostic=native-byte"
                " offset=%zu actual=%02x expected=%02x\n", first_difference,
                static_cast<unsigned>(native[first_difference]), static_cast<unsigned>(oracle[first_difference]));
        }
        renderer.PublishGuestWrites();
        std::vector<uint32_t> actual(expected.size());
        check(5, Memory::TryReadBacking(base, actual.data(), size) && actual == expected &&
            !buffers.HasGpuDirtyBytes(base, size) && !buffers.IsRegionGpuModified(base, size));
        // Remove this clean cache identity before its guest backing is released;
        // following fixtures must not inherit an expanded native owner.
        BufferCacheTestAccess::Retire(buffers, base, size);
        scheduler.Finish();
        scheduler.WaitPriorityOperations(scheduler.CurrentTick() - 1);
        check(6, !buffers.IsRegionRegistered(base, size));
        GuestGpuPublicationActive = false;
        check(6, LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0);
    });
    check(7, Memory::KernelMprotect(mapping, size, 0x31) == OK);
    std::vector<uint32_t> actual(expected.size());
    check(7, Memory::ReadGuestMemory(nullptr, base, actual.data(), size) && actual == expected);
    check(7, Memory::KernelMunmap(base, size) == OK);
    check(7, Memory::KernelCheckedReleaseDirectMemory(physical, size) == OK);
    std::printf("PS5_GPU_TEST case=buffer-dirty-overlap-snapshot-transfer result=%s async=%u"
        " owners=2 native-replacement=checked staged-baselines=preserved"
        " full-bytes-gap-padding=checked bytes=49152 alignment=1048576"
        " phases=%u,%u,%u,%u,%u,%u,%u,%u\n", pass ? "PASS" : "FAIL", enabled ? 1u : 0u,
        phases[0] ? 1u : 0u, phases[1] ? 1u : 0u, phases[2] ? 1u : 0u, phases[3] ? 1u : 0u,
        phases[4] ? 1u : 0u, phases[5] ? 1u : 0u, phases[6] ? 1u : 0u, phases[7] ? 1u : 0u);
    return pass;
}

bool AggressiveCpuDeltaProbe(Libs::Graphics::RenderContext& renderer) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    if (!GuestAsyncCpuVisibleExperimentEnabled()) {
        std::printf("PS5_GPU_TEST case=experimental-cpu-delta-buffer-private-color result=SKIP"
            " cpu-visible-experiment=0\n");
        return true;
    }
    constexpr uint64_t mapping_size = 0x4000;
    constexpr uint64_t fill_offset = 16, fill_bytes = 64;
    constexpr uint32_t fill = 0x21436587u, color_fill = 0xffabcde1u;
    int64_t physical = -1, private_physical = -1;
    if (Memory::KernelAllocateMainDirectMemory(mapping_size, mapping_size, 0, &physical) != OK)
        return false;
    if (Memory::KernelAllocateMainDirectMemory(mapping_size, mapping_size, 0, &private_physical) != OK) {
        (void)Memory::KernelCheckedReleaseDirectMemory(physical, mapping_size);
        return false;
    }
    void* mapping = nullptr;
    void* read_mapping = nullptr;
    void* private_mapping = nullptr;
    if (Memory::KernelMapDirectMemory(&mapping, mapping_size, 0x33, 0, physical, 0x100000) != OK ||
        Memory::KernelMapDirectMemory(&read_mapping, mapping_size, 0x31, 0, physical, 0x100000) != OK ||
        Memory::KernelMapDirectMemory(&private_mapping, mapping_size, 0x30, 0,
            private_physical, 0x100000) != OK) {
        (void)Memory::KernelCheckedReleaseDirectMemory(physical, mapping_size);
        (void)Memory::KernelCheckedReleaseDirectMemory(private_physical, mapping_size);
        return false;
    }
    const auto base = reinterpret_cast<uint64_t>(mapping);
    const auto read_alias = reinterpret_cast<uint64_t>(read_mapping);
    const auto private_base = reinterpret_cast<uint64_t>(private_mapping);
    std::vector<uint8_t> seed(mapping_size);
    for (size_t i = 0; i < seed.size(); ++i)
        seed[i] = static_cast<uint8_t>((i * 29 + 7) ^ ((i >> 4) * 3));
    auto expected = seed;
    for (size_t offset = fill_offset; offset < fill_offset + fill_bytes; offset += sizeof(fill))
        std::memcpy(expected.data() + offset, &fill, sizeof(fill));
    std::vector<uint32_t> private_expected(mapping_size / 4);
    for (size_t i = 0; i < private_expected.size(); ++i)
        private_expected[i] = 0x76540000u + static_cast<uint32_t>(i);
    Memory::WriteBacking(base, seed.data(), seed.size());
    Memory::WriteBacking(private_base, private_expected.data(), mapping_size);
    TextureCache::ImageDesc desc;
    desc.type = TextureCache::BindingType::RenderTarget;
    desc.info.data = {private_base, 8192};
    desc.info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    desc.info.guest_format = Prospero::BufferFormat::k8_8_8_8UNorm;
    desc.info.type = Prospero::ImageType::kColor2D;
    desc.info.extent = {32, 32, 1};
    desc.info.resources = {1, 1};
    desc.info.pitch = 64;
    desc.info.bytes_per_block = 4;
    desc.info.UpdateSize();
    desc.view_info.format = desc.info.pixel_format;
    desc.view_info.type = vk::ImageViewType::e2D;
    desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
    desc.view_info.usage = vk::ImageUsageFlagBits::eColorAttachment;
    HW::Context hw;
    HW::UserConfig user;
    HW::Shader shader;
    ImageId private_image {};
    bool pass = true;
    std::array<bool, 6> phases;
    phases.fill(true);
    const auto check = [&](size_t phase, bool result) {
        phases[phase] &= result;
        pass &= result;
    };
    renderer.GetGpu().SendCommandSync([&] {
        GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
        const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (epoch == 0) EXIT("experimental CPU delta producer epoch denied\n");
        GuestGpuPublicationActive = true;
        auto& scheduler = renderer.GetCommandScheduler();
        scheduler.Begin(hw, user, shader);
        auto& buffers = renderer.GetBufferCache();
        auto [buffer, offset] = buffers.ObtainBuffer(base + fill_offset, fill_bytes, true);
        check(0, buffer->CpuAddress() == base && buffer->Size() == mapping_size);
        buffer->Fill(offset, fill_bytes, fill);
        auto& textures = renderer.GetTextureCache();
        private_image = textures.FindImage(desc);
        (void)textures.FindRenderTarget(private_image, desc);
        check(0, textures.ClearImageFromBuffer(scheduler.Current(), private_base, 8192, color_fill));
        renderer.FinishGuestGpuSlice(true);
        std::array<uint8_t, TRACKER_PAGE_SIZE> backing {};
        check(1, buffers.HasGpuDirtyBytes(base, TRACKER_PAGE_SIZE) &&
            buffers.IsRegionGpuModified(base, TRACKER_PAGE_SIZE) &&
            textures.GetImage(private_image).IsGpuModified() &&
            Memory::TryReadBacking(base, backing.data(), backing.size()) &&
            std::memcmp(backing.data(), seed.data(), backing.size()) == 0 &&
            BufferCacheTestAccess::Compare(buffers, base, backing.data(), backing.size()) ==
                BackingComparison::Unchanged);
        GuestGpuPublicationActive = false;
        check(1, LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0);
    });
    // Deliberately reproduce uninstrumented guest stores between FEX epochs.
    // The HLE WriteGuestMemory preflight would publish this owner before writing.
    // All values differ from the staged seed; same-value stores are unobservable.
    constexpr std::array<size_t, 3> offsets {17, 37, 1025};
    constexpr std::array<uint8_t, 3> deltas {0x55, 0xaa, 0x6b};
    for (size_t i = 0; i < offsets.size(); ++i) {
        expected[offsets[i]] = seed[offsets[i]] ^ deltas[i];
        std::memcpy(reinterpret_cast<uint8_t*>(mapping) + offsets[i],
            expected.data() + offsets[i], 1);
    }
    renderer.GetGpu().SendCommandSync([&] {
        GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
        const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (epoch == 0) EXIT("experimental CPU delta consumer epoch denied\n");
        GuestGpuPublicationActive = true;
        auto& scheduler = renderer.GetCommandScheduler();
        scheduler.Begin(hw, user, shader);
        auto& buffers = renderer.GetBufferCache();
        auto& textures = renderer.GetTextureCache();
        std::array<uint8_t, TRACKER_PAGE_SIZE> before {};
        check(2, Memory::TryReadBacking(base, before.data(), before.size()) &&
            BufferCacheTestAccess::Compare(buffers, base, before.data(), before.size()) ==
                BackingComparison::Changed && buffers.HasGpuDirtyBytes(base, TRACKER_PAGE_SIZE) &&
            textures.GetImage(private_image).IsGpuModified());
        // Only the conflicted tracking page is downloaded; synchronization then
        // uploads its merged bytes and initializes the other three clean pages.
        auto [buffer, offset] = buffers.ObtainBuffer(base, mapping_size, false);
        check(3, !buffers.HasGpuDirtyBytes(base, mapping_size) &&
            !buffers.IsRegionGpuModified(base, mapping_size) &&
            textures.GetImage(private_image).IsGpuModified());
        Buffer readback(renderer.GetGraphics(), scheduler, MemoryUsage::Download, 0,
            vk::BufferUsageFlagBits::eTransferDst, mapping_size);
        readback.CopyFrom(scheduler.Current(), *buffer, offset, 0, mapping_size);
        scheduler.Finish();
        scheduler.WaitPriorityOperations(scheduler.CurrentTick() - 1);
        readback.Invalidate(0, mapping_size);
        check(4, std::memcmp(readback.Mapped().data(), expected.data(), mapping_size) == 0);
        std::vector<uint8_t> actual(mapping_size);
        check(4, Memory::TryReadBacking(base, actual.data(), actual.size()) && actual == expected);
        renderer.FinishGuestGpuSlice(true);
        check(4, textures.GetImage(private_image).IsGpuModified() &&
            textures.IsRegionGpuModified(private_base, 8192));
        BufferCacheTestAccess::Retire(buffers, base, mapping_size);
        scheduler.Finish();
        scheduler.WaitPriorityOperations(scheduler.CurrentTick() - 1);
        GuestGpuPublicationActive = false;
        check(4, LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0);
    });
    std::vector<uint8_t> alias_actual(mapping_size);
    check(5, Memory::ReadGuestMemory(nullptr, read_alias, alias_actual.data(), alias_actual.size()) &&
        alias_actual == expected);
    // Only this explicit VM transition publishes the unrelated private image.
    check(5, Memory::KernelMprotect(private_mapping, mapping_size, 0x31) == OK);
    for (size_t i = 0; i < 2048; ++i)
        if (i % 64 < 32) private_expected[i] = color_fill;
    std::vector<uint32_t> private_actual(private_expected.size());
    check(5, Memory::ReadGuestMemory(nullptr, private_base, private_actual.data(), mapping_size) &&
        private_actual == private_expected);
    check(5, Memory::KernelMunmap(base, mapping_size) == OK);
    check(5, Memory::KernelMunmap(read_alias, mapping_size) == OK);
    check(5, Memory::KernelMunmap(private_base, mapping_size) == OK);
    check(5, Memory::KernelCheckedReleaseDirectMemory(physical, mapping_size) == OK);
    check(5, Memory::KernelCheckedReleaseDirectMemory(private_physical, mapping_size) == OK);
    std::printf("PS5_GPU_TEST case=experimental-cpu-delta-buffer-private-color result=%s"
        " cpu-visible-experiment=1 direct-cpu-delta-bytes=3 dirty-range=16:64"
        " inside-and-outside-dirty=checked native-and-read-alias-full-padding=checked"
        " unrelated-private-color=retained-until-vm-transition"
        " phases=%u,%u,%u,%u,%u,%u\n", pass ? "PASS" : "FAIL",
        phases[0] ? 1u : 0u, phases[1] ? 1u : 0u, phases[2] ? 1u : 0u,
        phases[3] ? 1u : 0u, phases[4] ? 1u : 0u, phases[5] ? 1u : 0u);
    return pass;
}

bool AsyncNativeBackingDependencyProbe(Libs::Graphics::RenderContext& renderer) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    constexpr uint64_t page_size = 0x4000;
    constexpr uint64_t private_offset = 64, private_bytes = 192;
    void* private_mapping = nullptr;
    if (Memory::KernelMapFlexibleMemory(&private_mapping, page_size, 0x30, 0) != OK) return false;
    const auto private_base = reinterpret_cast<uint64_t>(private_mapping);
    int64_t physical = -1;
    if (Memory::KernelAllocateMainDirectMemory(page_size, page_size, 0, &physical) != OK) {
        (void)Memory::KernelMunmap(private_base, page_size);
        return false;
    }
    void* gpu_mapping = nullptr;
    void* cpu_mapping = nullptr;
    if (Memory::KernelMapDirectMemory(&gpu_mapping, page_size, 0x30, 0, physical, page_size) != OK ||
        Memory::KernelMapDirectMemory(&cpu_mapping, page_size, 0x33, 0, physical, page_size) != OK) {
        (void)Memory::KernelReleaseDirectMemory(physical, page_size);
        (void)Memory::KernelMunmap(private_base, page_size);
        return false;
    }
    const auto gpu_base = reinterpret_cast<uint64_t>(gpu_mapping);
    const auto cpu_base = reinterpret_cast<uint64_t>(cpu_mapping);
    std::vector<uint32_t> private_expected(page_size / 4), target_expected(page_size / 4);
    for (size_t i = 0; i < private_expected.size(); ++i) {
        private_expected[i] = 0x50200000u + static_cast<uint32_t>(i);
        target_expected[i] = 0x70100000u + static_cast<uint32_t>(i);
    }
    const auto private_seed = private_expected;
    Memory::WriteBacking(private_base, private_expected.data(), page_size);
    Memory::WriteBacking(gpu_base, target_expected.data(), page_size);
    const bool enabled = GuestAsyncPrivateBackingEnabled();
    bool pass = Memory::QueryGuestBackingCpuVisible(gpu_base, page_size) &&
        !Memory::QueryGuestBackingCpuVisible(private_base, page_size);
    HW::Context hw;
    HW::UserConfig user;
    HW::Shader shader;
    const auto update_private = [&](uint32_t value) {
        std::fill(private_expected.begin() + private_offset / 4,
            private_expected.begin() + (private_offset + private_bytes) / 4, value);
    };
    const auto backing_matches = [&](uint64_t address, const std::vector<uint32_t>& expected) {
        std::vector<uint32_t> actual(expected.size());
        return Memory::TryReadBacking(address, actual.data(), page_size) && actual == expected;
    };
    const auto owned_epoch = [&](auto&& operation) {
        renderer.GetGpu().SendCommandSync([&] {
            GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
            const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
            if (epoch == 0) EXIT("native backing dependency fixture epoch denied\n");
            GuestGpuPublicationActive = true;
            renderer.GetCommandScheduler().Begin(hw, user, shader);
            operation();
            renderer.FinishGuestGpuSlice(true);
            GuestGpuPublicationActive = false;
            pass &= LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0;
        });
    };
    update_private(0x13579bdfu);
    owned_epoch([&] {
        auto& buffers = renderer.GetBufferCache();
        auto [private_buffer, private_native_offset] =
            buffers.ObtainBuffer(private_base + private_offset, private_bytes, true);
        private_buffer->Fill(private_native_offset, private_bytes, 0x13579bdfu);
        // Clean CPU targets cannot force unrelated private ownership to publish.
        PublishNativeGuestBackingRead(cpu_base + 1024, 64, true);
        std::array<uint32_t, 16> read {};
        std::memcpy(read.data(), reinterpret_cast<const void*>(cpu_base + 1024), sizeof(read));
        pass &= std::memcmp(read.data(), target_expected.data() + 1024 / 4, sizeof(read)) == 0 &&
            buffers.HasGpuDirtyBytes(private_base, page_size) &&
            backing_matches(private_base, private_seed);
        constexpr uint32_t raw_value = 0xa1b2c3d4u;
        PrepareNativeGuestBackingWrite(cpu_base + 1028, sizeof(raw_value));
        std::memcpy(reinterpret_cast<void*>(cpu_base + 1028), &raw_value, sizeof(raw_value));
        target_expected[1028 / 4] = raw_value;
        renderer.InvalidatePhysicalAliases(cpu_base + 1028, sizeof(raw_value), true);
        pass &= buffers.HasGpuDirtyBytes(private_base, page_size) &&
            backing_matches(private_base, private_seed) &&
            std::memcmp(cpu_mapping, target_expected.data(), page_size) == 0;
    });
    renderer.GetGpu().SendCommandSync([&] {
        pass &= renderer.GetBufferCache().HasGpuDirtyBytes(private_base, page_size) == enabled &&
            backing_matches(private_base, enabled ? private_seed : private_expected);
    });
    update_private(0x99aabbccu);
    owned_epoch([&] {
        auto& buffers = renderer.GetBufferCache();
        auto [private_buffer, private_native_offset] =
            buffers.ObtainBuffer(private_base + private_offset, private_bytes, true);
        private_buffer->Fill(private_native_offset, private_bytes, 0x99aabbccu);
        auto [target, target_offset] = buffers.ObtainBuffer(gpu_base + 128, 128, true);
        target->Fill(target_offset, 128, 0x24681357u);
        std::fill(target_expected.begin() + 128 / 4, target_expected.begin() + 256 / 4,
            0x24681357u);
        auto [far_target, far_offset] = buffers.ObtainBuffer(gpu_base + 8192, 64, true);
        far_target->Fill(far_offset, 64, 0x7badc0deu);
        std::fill(target_expected.begin() + 8192 / 4, target_expected.begin() + 8256 / 4,
            0x7badc0deu);
        pass &= buffers.HasGpuDirtyBytes(gpu_base, page_size) &&
            !buffers.HasGpuDirtyBytes(cpu_base, page_size);
        std::atomic<unsigned> callbacks {0};
        auto& scheduler = renderer.GetCommandScheduler();
        scheduler.DeferPriorityOperation([&] {
            constexpr uint32_t padding_value = 0x76543210u;
            Memory::WriteBacking(gpu_base + 1536, &padding_value, sizeof(padding_value));
            target_expected[1536 / 4] = padding_value;
            callbacks.fetch_or(1u, std::memory_order_release);
        });
        scheduler.DeferOperation([&] { callbacks.fetch_or(2u, std::memory_order_release); });
        // The same-epoch owner is at another VA. The raw read needs its exact
        // physical dependency even though no CPU readmission has occurred yet.
        PublishNativeGuestBackingRead(cpu_base + 128, 128, true);
        if (!enabled) {
            // OFF keeps the completed baseline policy; its helper is a no-op.
            pass &= buffers.HasGpuDirtyBytes(gpu_base, page_size);
            renderer.FinishGuestGpuSlice(false);
        }
        pass &= !buffers.HasGpuDirtyBytes(gpu_base, page_size) &&
            buffers.HasGpuDirtyBytes(private_base, page_size) == enabled &&
            std::memcmp(cpu_mapping, target_expected.data(), page_size) == 0 &&
            callbacks.load(std::memory_order_acquire) == 3 &&
            backing_matches(private_base, enabled ? private_seed : private_expected);
    });
    update_private(0x0f1e2d3cu);
    owned_epoch([&] {
        auto& buffers = renderer.GetBufferCache();
        auto [private_buffer, private_native_offset] =
            buffers.ObtainBuffer(private_base + private_offset, private_bytes, true);
        private_buffer->Fill(private_native_offset, private_bytes, 0x0f1e2d3cu);
        auto [target, target_offset] = buffers.ObtainBuffer(gpu_base + 512, 128, true);
        target->Fill(target_offset, 128, 0x55667788u);
        std::fill(target_expected.begin() + 512 / 4, target_expected.begin() + 640 / 4,
            0x55667788u);
        constexpr uint32_t raw_value = 0xdeadbeefu;
        // This store is outside the exact dirty bytes, on the same staged
        // buffer page. Resolving only the byte overlap would leave a Changed
        // GPU-owned witness after the store.
        PrepareNativeGuestBackingWrite(cpu_base + 700, sizeof(raw_value));
        if (!enabled) {
            pass &= buffers.HasGpuDirtyBytes(gpu_base, page_size);
            renderer.FinishGuestGpuSlice(false);
        }
        pass &= !buffers.HasGpuDirtyBytes(gpu_base, page_size) &&
            buffers.HasGpuDirtyBytes(private_base, page_size) == enabled &&
            std::memcmp(cpu_mapping, target_expected.data(), page_size) == 0 &&
            backing_matches(private_base, enabled ? private_seed : private_expected);
        // Publication must precede this raw store so it cannot overwrite it.
        std::memcpy(reinterpret_cast<void*>(cpu_base + 700), &raw_value, sizeof(raw_value));
        target_expected[700 / 4] = raw_value;
        renderer.InvalidatePhysicalAliases(cpu_base + 700, sizeof(raw_value), true);
        pass &= std::memcmp(cpu_mapping, target_expected.data(), page_size) == 0;
    });
    std::vector<uint32_t> target_actual(target_expected.size());
    pass &= Memory::ReadGuestMemory(nullptr, cpu_base, target_actual.data(), page_size) &&
        target_actual == target_expected &&
        backing_matches(private_base, enabled ? private_seed : private_expected);
    pass &= Memory::KernelReleaseDirectMemory(physical, page_size) == OK;
    // Destructive VM effects still drain every retained owner before reuse.
    pass &= backing_matches(private_base, private_expected);
    pass &= Memory::KernelMunmap(private_base, page_size) == OK;
    std::printf("PS5_GPU_TEST case=native-backing-exact-dependencies result=%s async=%u"
        " clean-and-dirty-target=preserves-unrelated-private physical-alias-read-write=completed"
        " same-page-store-outside-dirty=completed callbacks-and-padding=checked"
        " selector-off=completed-baseline-policy\n",
        pass ? "PASS" : "FAIL", enabled ? 1u : 0u);
    return pass;
}

bool AdjacentPhysicalAliasDependencyProbe(Libs::Graphics::RenderContext& renderer) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    constexpr uint64_t page = 0x4000;
    int64_t physical = -1;
    if (Memory::KernelAllocateMainDirectMemory(page, page, 0, &physical) != OK) return false;
    void* reservation = nullptr;
    if (Memory::KernelReserveVirtualRange(&reservation, 3 * page, 0, page) != OK) {
        (void)Memory::KernelReleaseDirectMemory(physical, page);
        return false;
    }
    const auto base = reinterpret_cast<uint64_t>(reservation);
    void* gpu = reservation;
    void* adjacent_gpu = reinterpret_cast<void*>(base + page);
    void* cpu = reinterpret_cast<void*>(base + 2 * page);
    bool pass = Memory::KernelMapDirectMemory(&gpu, page, 0x30, 0x10, physical, page) == OK &&
        Memory::KernelMapDirectMemory(&adjacent_gpu, page, 0x30, 0x10, physical, page) == OK &&
        Memory::KernelMapDirectMemory(&cpu, page, 0x33, 0x10, physical, page) == OK;
    if (!pass) {
        (void)Memory::KernelReleaseDirectMemory(physical, page);
        return false;
    }
    std::vector<uint32_t> expected(page / 4, 0x13572468u);
    Memory::WriteBacking(base, expected.data(), page);
    HW::Context hw;
    HW::UserConfig user;
    HW::Shader shader;
    renderer.GetGpu().SendCommandSync([&] {
        GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
        const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (epoch == 0) EXIT("adjacent physical alias dependency epoch denied\n");
        GuestGpuPublicationActive = true;
        auto& scheduler = renderer.GetCommandScheduler();
        scheduler.Begin(hw, user, shader);
        auto& buffers = renderer.GetBufferCache();
        auto [owner, offset] = buffers.ObtainBuffer(base, page, true);
        owner->Fill(offset, page, 0x89abcdefu);
        std::fill(expected.begin(), expected.end(), 0x89abcdefu);
        std::vector<Memory::GuestBackingAlias> aliases;
        pass &= Memory::QueryGuestBackingAliases(base, page, &aliases);
        // These are three views of the same bytes, not one linear consumer
        // spanning that backing three times. Re-project each mapping segment.
        for (const auto& alias: aliases) {
            std::vector<Memory::GuestBackingAlias> projected;
            pass &= alias.size <= page &&
                Memory::QueryGuestBackingAliases(alias.address, alias.size, &projected);
        }
        renderer.PublishGuestWriteDependency(base + 2 * page + 128, 1);
        std::vector<uint32_t> actual(expected.size());
        pass &= !buffers.HasGpuDirtyBytes(base, page) &&
            Memory::TryReadBacking(base, actual.data(), page) && actual == expected &&
            std::memcmp(cpu, expected.data(), page) == 0;
        renderer.FinishGuestGpuSlice(false);
        GuestGpuPublicationActive = false;
        pass &= LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0;
    });
    pass &= Memory::KernelReleaseDirectMemory(physical, page) == OK;
    std::printf("PS5_GPU_TEST case=adjacent-physical-alias-dependency result=%s"
        " mappings=3 duplicate-physical-spans=separate one-byte-consumer=complete-owner\n",
        pass ? "PASS" : "FAIL");
    return pass;
}

bool AsyncPrivateDepthStencilProbe(Libs::Graphics::RenderContext& renderer) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    constexpr uint64_t mapping_size = 0xc000;
    constexpr uint32_t width = 7, height = 5, pitch = 12, layers = 2;
    constexpr uint64_t depth_stride = 256, stencil_stride = 64;
    constexpr uint64_t depth_size = depth_stride * layers, stencil_size = stencil_stride * layers;
    constexpr uint32_t depth_value = std::bit_cast<uint32_t>(0.75f);
    constexpr uint8_t stencil_value = 0x69;
    void* mapping = nullptr;
    if (Memory::KernelMapFlexibleMemory(&mapping, mapping_size, 0x32, 0) != OK) return false;
    const auto base = reinterpret_cast<uint64_t>(mapping);
    std::vector<uint8_t> expected(mapping_size, 0xa5);
    Memory::WriteBacking(base, expected.data(), expected.size());
    for (uint32_t layer = 0; layer < layers; ++layer)
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                const auto texel = static_cast<uint64_t>(y) * pitch + x;
                std::memcpy(expected.data() + 0x4000 + depth_stride * layer + texel * 4,
                    &depth_value, sizeof(depth_value));
                expected[0x8000 + stencil_stride * layer + texel] = stencil_value;
            }
    bool pass = Memory::KernelMprotect(mapping, mapping_size, 0x30) == OK;
    const bool enabled = GuestAsyncPrivateBackingEnabled();
    HW::Context hw;
    HW::UserConfig user;
    HW::Shader shader;
    TextureCache::ImageDesc desc;
    desc.type = TextureCache::BindingType::DepthTarget;
    desc.info.data = {base + 0x4000, depth_size};
    desc.info.stencil = {base + 0x8000, stencil_size};
    desc.info.pixel_format = vk::Format::eD32SfloatS8Uint;
    desc.info.guest_format = Prospero::BufferFormat::k32Float;
    desc.info.type = Prospero::ImageType::kColor2D;
    desc.info.extent = {width, height, 1};
    desc.info.resources = {1, layers};
    desc.info.pitch = pitch;
    desc.info.bytes_per_block = 4;
    desc.info.tile_mode = Prospero::TileMode::kLinear;
    desc.info.UpdateSize();
    desc.view_info.format = desc.info.pixel_format;
    desc.view_info.type = vk::ImageViewType::e2DArray;
    desc.view_info.layer_count = layers;
    desc.view_info.aspect = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
    desc.view_info.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
    ImageId image {}, stencil {};
    renderer.GetGpu().SendCommandSync([&] {
        GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
        const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (epoch == 0) EXIT("private depth/stencil fixture epoch denied\n");
        GuestGpuPublicationActive = true;
        auto& scheduler = renderer.GetCommandScheduler();
        scheduler.Begin(hw, user, shader);
        auto& textures = renderer.GetTextureCache();
        image = textures.FindImage(desc);
        (void)textures.FindDepthTarget(image, desc);
        stencil = TextureCacheTestAccess::Stencil(textures, image);
        pass &= TextureCacheTestAccess::CompleteDepthPlane(textures, image) &&
            TextureCacheTestAccess::CompleteDepthPlane(textures, stencil);
        auto& native = textures.GetImage(image);
        native.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
            {}, scheduler.Current().Handle());
        const vk::ClearDepthStencilValue clear {0.75f, stencil_value};
        const vk::ImageSubresourceRange range {desc.view_info.aspect, 0, 1, 0, layers};
        scheduler.Current().Handle().clearDepthStencilImage(native.backing.image,
            vk::ImageLayout::eTransferDstOptimal, &clear, 1, &range);
        std::atomic<unsigned> callbacks {0};
        scheduler.DeferPriorityOperation([&] { callbacks.fetch_or(1u, std::memory_order_release); });
        scheduler.DeferOperation([&] { callbacks.fetch_or(2u, std::memory_order_release); });
        renderer.FinishGuestGpuSlice(true);
        pass &= callbacks.load(std::memory_order_acquire) == 3 &&
            textures.GetImage(image).IsGpuModified() == enabled &&
            textures.GetImage(stencil).IsGpuModified() == enabled;
        GuestGpuPublicationActive = false;
        pass &= LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0;
    });
    // Reacquisition in a second epoch must reuse both native aspects instead of
    // uploading the still-unpublished private backing over either plane.
    renderer.GetGpu().SendCommandSync([&] {
        GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
        const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (epoch == 0) EXIT("private depth/stencil reuse epoch denied\n");
        GuestGpuPublicationActive = true;
        auto& scheduler = renderer.GetCommandScheduler();
        scheduler.Begin(hw, user, shader);
        auto& textures = renderer.GetTextureCache();
        pass &= textures.FindImage(desc) == image;
        (void)textures.FindDepthTarget(image, desc);
        constexpr uint64_t unrelated_offset = 64, unrelated_size = 48;
        constexpr uint32_t unrelated_value = 0x5a697887u;
        auto& buffers = renderer.GetBufferCache();
        auto [unrelated, unrelated_native_offset] =
            buffers.ObtainBuffer(base + unrelated_offset, unrelated_size, true);
        unrelated->Fill(unrelated_native_offset, unrelated_size, unrelated_value);
        for (uint64_t offset = unrelated_offset; offset < unrelated_offset + unrelated_size; offset += 4)
            std::memcpy(expected.data() + offset, &unrelated_value, sizeof(unrelated_value));
        constexpr uint64_t depth_transfer_stride = width * height * 4;
        constexpr uint64_t stencil_transfer_stride = (width * height + 3u) & ~uint64_t {3};
        Buffer depth_read(renderer.GetGraphics(), scheduler, MemoryUsage::Download, 0,
            vk::BufferUsageFlagBits::eTransferDst, depth_transfer_stride * layers);
        Buffer stencil_read(renderer.GetGraphics(), scheduler, MemoryUsage::Download, 0,
            vk::BufferUsageFlagBits::eTransferDst, stencil_transfer_stride * layers);
        std::array<vk::BufferImageCopy, layers> depth_copies {}, stencil_copies {};
        for (uint32_t layer = 0; layer < layers; ++layer) {
            depth_copies[layer].bufferOffset = depth_transfer_stride * layer;
            depth_copies[layer].bufferRowLength = width;
            depth_copies[layer].bufferImageHeight = height;
            depth_copies[layer].imageSubresource = {vk::ImageAspectFlagBits::eDepth, 0, layer, 1};
            depth_copies[layer].imageExtent = {width, height, 1};
            stencil_copies[layer] = depth_copies[layer];
            stencil_copies[layer].bufferOffset = stencil_transfer_stride * layer;
            stencil_copies[layer].imageSubresource.aspectMask = vk::ImageAspectFlagBits::eStencil;
        }
        textures.GetImage(image).Download(depth_copies, depth_read.Handle(), 0,
            depth_transfer_stride * layers);
        textures.GetImage(image).Download(stencil_copies, stencil_read.Handle(), 0,
            stencil_transfer_stride * layers);
        scheduler.Finish();
        scheduler.WaitPriorityOperations(scheduler.CurrentTick() - 1);
        depth_read.Invalidate(0, depth_transfer_stride * layers);
        stencil_read.Invalidate(0, stencil_transfer_stride * layers);
        for (uint32_t layer = 0; layer < layers; ++layer)
            for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
                uint32_t actual_depth = 0;
                std::memcpy(&actual_depth, depth_read.Mapped().data() +
                    depth_transfer_stride * layer + pixel * 4, sizeof(actual_depth));
                pass &= actual_depth == depth_value &&
                    static_cast<uint8_t>(stencil_read.Mapped()[stencil_transfer_stride * layer + pixel]) ==
                        stencil_value;
            }
        // A one-byte dependency on the stencil aspect must complete both
        // guest planes and all padding, retaining an unrelated buffer owner.
        renderer.PublishGuestWriteDependency(desc.info.stencil.address + 1, 1);
        std::vector<uint8_t> published(mapping_size);
        pass &= Memory::TryReadBacking(base, published.data(), published.size()) &&
            std::memcmp(published.data() + 0x4000, expected.data() + 0x4000,
                mapping_size - 0x4000) == 0 &&
            std::all_of(published.begin(), published.begin() + 0x4000,
                [](uint8_t value) { return value == 0xa5; }) &&
            !textures.GetImage(image).IsGpuModified() &&
            !textures.GetImage(stencil).IsGpuModified() &&
            buffers.HasGpuDirtyBytes(base + unrelated_offset, unrelated_size);
        TextureCacheTestAccess::ClearDepthLayer(textures, scheduler.Current(), image, 1, 0.25f);
        const auto depth_only_value = std::bit_cast<uint32_t>(0.25f);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                const auto texel = static_cast<uint64_t>(y) * pitch + x;
                std::memcpy(expected.data() + 0x4000 + depth_stride + texel * 4,
                    &depth_only_value, sizeof(depth_only_value));
            }
        pass &= textures.GetImage(image).IsGpuModified() &&
            !textures.GetImage(stencil).IsGpuModified();
        // The requested stencil plane is clean, while its native depth sibling
        // is dirty. The indexed fast path must still resolve the family.
        renderer.PublishGuestWriteDependency(desc.info.stencil.address + 2, 1);
        pass &= !textures.GetImage(image).IsGpuModified() &&
            !textures.GetImage(stencil).IsGpuModified() &&
            buffers.HasGpuDirtyBytes(base + unrelated_offset, unrelated_size);
        TextureCacheTestAccess::ClearStencilLayer(textures, scheduler.Current(), image, 0, 0x3c);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
                expected[0x8000 + (static_cast<uint64_t>(y) * pitch + x)] = 0x3c;
        pass &= !textures.GetImage(image).IsGpuModified() &&
            textures.GetImage(stencil).IsGpuModified();
        renderer.PublishGuestWriteDependency(desc.info.data.address + 2, 1);
        pass &= Memory::TryReadBacking(base, published.data(), published.size()) &&
            std::memcmp(published.data() + 0x4000, expected.data() + 0x4000,
                mapping_size - 0x4000) == 0 &&
            std::all_of(published.begin(), published.begin() + 0x4000,
                [](uint8_t value) { return value == 0xa5; }) &&
            !textures.GetImage(image).IsGpuModified() &&
            !textures.GetImage(stencil).IsGpuModified() &&
            buffers.HasGpuDirtyBytes(base + unrelated_offset, unrelated_size);
        renderer.FinishGuestGpuSlice(true);
        pass &= buffers.HasGpuDirtyBytes(base + unrelated_offset, unrelated_size) == enabled;
        GuestGpuPublicationActive = false;
        pass &= LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0;
    });
    uint32_t forbidden = 0;
    pass &= !Memory::ReadGuestMemory(nullptr, desc.info.data.address, &forbidden, sizeof(forbidden)) &&
        !Memory::ReadGuestMemory(nullptr, desc.info.stencil.address, &forbidden, sizeof(forbidden));
    // Granting only stencil CPU access drains the entire association first.
    pass &= Memory::KernelMprotect(reinterpret_cast<void*>(base + 0x8000), 0x4000, 0x31) == OK;
    std::array<uint8_t, 0x4000> stencil_page {};
    pass &= Memory::ReadGuestMemory(nullptr, base + 0x8000, stencil_page.data(), stencil_page.size()) &&
        std::memcmp(stencil_page.data(), expected.data() + 0x8000, stencil_page.size()) == 0 &&
        !Memory::ReadGuestMemory(nullptr, desc.info.data.address, &forbidden, sizeof(forbidden));
    renderer.GetGpu().SendCommandSync([&] {
        pass &= !renderer.GetTextureCache().GetImage(image).IsGpuModified() &&
            !renderer.GetTextureCache().GetImage(stencil).IsGpuModified();
    });
    pass &= Memory::KernelMprotect(mapping, mapping_size, 0x31) == OK;
    std::vector<uint8_t> actual(mapping_size);
    pass &= Memory::ReadGuestMemory(nullptr, base, actual.data(), actual.size()) && actual == expected;
    pass &= Memory::KernelMunmap(base, mapping_size) == OK;
    std::printf("PS5_GPU_TEST case=private-depth-stencil-cross-epoch-mprotect result=%s async=%u"
        " layers=2 native-reuse=checked one-byte-stencil-dependency=family-published"
        " clean-plane-dirty-sibling=depth-and-stencil-checked"
        " unrelated-buffer=retained stencil-grant=family-published full-bytes-padding=checked\n",
        pass ? "PASS" : "FAIL", enabled ? 1u : 0u);
    return pass;
}

bool AsyncDirectAliasProbe(Libs::Graphics::RenderContext& renderer) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    constexpr uint64_t size = 0x8000;
    int64_t physical = -1;
    if (Memory::KernelAllocateMainDirectMemory(size, 0x4000, 0, &physical) != OK) return false;
    void* gpu_mapping = nullptr;
    if (Memory::KernelMapDirectMemory(&gpu_mapping, size, 0x30, 0, physical, 0x4000) != OK) return false;
    const auto base = reinterpret_cast<uint64_t>(gpu_mapping);
    void* private_alias_mapping = nullptr;
    if (Memory::KernelMapDirectMemory(&private_alias_mapping, size, 0x30, 0, physical, 0x4000) != OK) return false;
    const auto private_alias = reinterpret_cast<uint64_t>(private_alias_mapping);
    std::vector<uint32_t> expected(size / 4, 0x31415926u);
    Memory::WriteBacking(base, expected.data(), size);
    HW::Context hw;
    HW::UserConfig user;
    HW::Shader shader;
    bool pass = true;
    const char* selector = std::getenv("LUMEN_PS5_GFX_ASYNC_PRIVATE_BACKING");
    const bool enabled = selector != nullptr && std::strcmp(selector, "1") == 0;
    auto fill_buffer = [&](uint32_t value, bool must_retain) {
        renderer.GetGpu().SendCommandSync([&] {
            GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
            const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
            if (epoch == 0) EXIT("direct alias fixture epoch denied\n");
            GuestGpuPublicationActive = true;
            auto& scheduler = renderer.GetCommandScheduler();
            scheduler.Begin(hw, user, shader);
            auto& buffers = renderer.GetBufferCache();
            auto [buffer, offset] = buffers.ObtainBuffer(base, size, true);
            buffer->Fill(offset, size, value);
            renderer.FinishGuestGpuSlice(true);
            pass &= buffers.HasGpuDirtyBytes(base, size) == (enabled && must_retain);
            GuestGpuPublicationActive = false;
            pass &= LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0;
        });
        std::fill(expected.begin(), expected.end(), value);
    };
    fill_buffer(0x01020304u, true);
    renderer.GetGpu().SendCommandSync([&] {
        GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
        const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (epoch == 0) EXIT("private GPU alias dependency epoch denied\n");
        GuestGpuPublicationActive = true;
        auto& scheduler = renderer.GetCommandScheduler();
        scheduler.Begin(hw, user, shader);
        auto [source, source_offset] = renderer.GetBufferCache().ObtainBuffer(private_alias, size, false);
        Buffer readback(renderer.GetGraphics(), scheduler, MemoryUsage::Download, 0,
            vk::BufferUsageFlagBits::eTransferDst, size);
        readback.CopyFrom(scheduler.Current(), *source, source_offset, 0, size);
        scheduler.Finish();
        scheduler.WaitPriorityOperations(scheduler.CurrentTick() - 1);
        readback.Invalidate(0, size);
        pass &= std::memcmp(readback.Mapped().data(), expected.data(), size) == 0;
        renderer.PublishGuestWrites();
        GuestGpuPublicationActive = false;
        pass &= LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0;
    });
    std::printf("PS5_GPU_TEST case=private-physical-alias-native-consumer result=%s async=%u\n",
        pass ? "PASS" : "FAIL", enabled ? 1u : 0u);
    renderer.GetGpu().SendCommandSync([&] {
        GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
        const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (epoch == 0) EXIT("overlapping physical alias copy epoch denied\n");
        GuestGpuPublicationActive = true;
        auto& scheduler = renderer.GetCommandScheduler();
        scheduler.Begin(hw, user, shader);
        for (size_t i = 0; i < expected.size(); ++i) expected[i] = 0x24350000u + i;
        Memory::WriteBacking(base, expected.data(), size);
        auto& buffers = renderer.GetBufferCache();
        buffers.InvalidateMemory(base, size);
        buffers.InvalidateMemory(private_alias, size);
        // VA ordering requires forward copy, physical ordering requires backward
        // copy. More than one scratch chunk catches accidental virtual memmove.
        std::memmove(reinterpret_cast<uint8_t*>(expected.data()) + 8, expected.data(), 0x6000);
        buffers.CopyBuffer(base + 8, private_alias, 0x6000, false, false);
        renderer.PublishGuestWrites();
        std::vector<uint32_t> actual(expected.size());
        pass &= Memory::TryReadBacking(base, actual.data(), size) && actual == expected;
        GuestGpuPublicationActive = false;
        pass &= LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0;
    });
    std::printf("PS5_GPU_TEST case=overlapping-physical-alias-copy result=%s bytes=24576\n", pass ? "PASS" : "FAIL");
    fill_buffer(0x10203040u, true);
    // A newly mapped CPU alias must see the retained owner, including bytes far
    // beyond either edge. Mapping is a physical visibility transition.
    void* cpu_mapping = nullptr;
    pass &= Memory::KernelMapDirectMemory(&cpu_mapping, size, 0x31, 0, physical, 0x4000) == OK;
    const auto alias = reinterpret_cast<uint64_t>(cpu_mapping);
    std::vector<uint32_t> actual(expected.size());
    pass &= Memory::ReadGuestMemory(nullptr, alias, actual.data(), size) && actual == expected;
    // Pre-existing readable alias prevents private retention of the GPU VA.
    fill_buffer(0x55667788u, false);
    pass &= Memory::ReadGuestMemory(nullptr, alias, actual.data(), size) && actual == expected;
    pass &= Memory::KernelMunmap(alias, size) == OK;
    // Retirement/release also drains owners even if no CPU alias remains.
    fill_buffer(0x99aabbccu, true);
    pass &= Memory::KernelMunmap(base, size) == OK;
    pass &= Memory::KernelMunmap(private_alias, size) == OK;
    pass &= Memory::KernelCheckedReleaseDirectMemory(physical, size) == OK;
    std::printf("PS5_GPU_TEST case=direct-late-readable-alias-retirement result=%s async=%u"
        " new-alias=published existing-alias=not-retained release=completed\n", pass ? "PASS" : "FAIL", enabled ? 1u : 0u);
    return pass;
}

bool ImageUploadSnapshotProbe(Libs::Graphics::RenderContext& renderer) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    constexpr uint64_t mapping_size = 0xc000, image_size = 8192;
    constexpr uint32_t width = 32, height = 32, pitch = 64;
    int64_t physical = -1;
    void* mapping = nullptr;
    void* alias_mapping = nullptr;
    if (Memory::KernelAllocateMainDirectMemory(mapping_size, 0x4000, 0, &physical) != OK ||
        Memory::KernelMapDirectMemory(&mapping, mapping_size, 0x33, 0, physical, 0x4000) != OK ||
        Memory::KernelMapDirectMemory(&alias_mapping, mapping_size, 0x33, 0, physical, 0x4000) != OK)
        return false;
    const auto base = reinterpret_cast<uint64_t>(mapping);
    const auto alias = reinterpret_cast<uint64_t>(alias_mapping);
    std::vector<uint32_t> expected(image_size / sizeof(uint32_t));
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = 0xff000000u + uint32_t(i);
    Memory::WriteBacking(base, expected.data(), image_size);
    TextureCache::ImageDesc desc;
    desc.info.data = {base, image_size};
    desc.info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    desc.info.guest_format = Prospero::BufferFormat::k8_8_8_8UNorm;
    desc.info.extent = {width, height, 1};
    desc.info.pitch = pitch;
    desc.info.bytes_per_block = 4;
    desc.info.UpdateSize();
    desc.view_info.format = desc.info.pixel_format;
    desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
    auto& textures = renderer.GetTextureCache();
    auto& buffers = renderer.GetBufferCache();
    auto& scheduler = renderer.GetCommandScheduler();
    const bool enabled = ImageUploadSnapshotEnabled() &&
        !GuestAsyncCpuVisibleExperimentEnabled() && !GuestAsyncCpuConcurrentExperimentRequested();
    HW::Context hw;
    HW::UserConfig user;
    HW::Shader shader;
    ImageId image;
    bool pass = true;
    const auto check = [&](const char* phase, bool value) {
        pass &= value;
        if (!value) std::printf("PS5_GPU_TEST case=image-upload-snapshot-check phase=%s result=FAIL\n", phase);
    };
    const auto in_epoch = [&](auto&& operation) {
        renderer.GetGpu().SendCommandSync([&] {
            GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
            const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
            if (epoch == 0) EXIT("image upload snapshot fixture epoch denied\n");
            GuestGpuPublicationActive = true;
            scheduler.Begin(hw, user, shader);
            operation();
            GuestGpuPublicationActive = false;
            pass &= LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0;
        });
    };
    const auto native_matches = [&] {
        Buffer readback(renderer.GetGraphics(), scheduler, MemoryUsage::Download, 0,
            vk::BufferUsageFlagBits::eTransferDst, width * height * sizeof(uint32_t));
        vk::BufferImageCopy copy {};
        copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        textures.GetImage(image).Download(std::span(&copy, 1), readback.Handle(), 0, readback.Size());
        scheduler.Finish();
        readback.Invalidate(0, readback.Size());
        const auto* actual = reinterpret_cast<const uint32_t*>(readback.Mapped().data());
        bool equal = true;
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
                equal &= actual[y * width + x] == expected[y * pitch + x];
        return equal;
    };
    in_epoch([&] {
        // Prior fixtures may leave a clean native reservation at this newly
        // mapped VA. Force the CPU staging route this fixture is qualifying.
        if (buffers.IsRegionRegistered(base, image_size)) {
            pass &= !buffers.IsRegionGpuModified(base, image_size);
            BufferCacheTestAccess::Retire(buffers, base, image_size);
            scheduler.Finish();
        }
        const auto before = TextureCacheTestAccess::UploadSnapshotStats(textures);
        image = textures.FindImage(desc);
        textures.UpdateImage(image);
        const auto revision = textures.GetImage(image).PublicationRevision();
        textures.UpdateImage(image);
        const auto after = TextureCacheTestAccess::UploadSnapshotStats(textures);
        pass &= (textures.GetImage(image).PublicationRevision() == revision) == enabled;
        pass &= after[0] == before[0] + (enabled ? 1 : 0) &&
            after[1] == before[1] + (enabled ? 1 : 0) &&
            after[2] == before[2] + (enabled ? image_size : 0);
        std::printf("PS5_GPU_TEST case=image-upload-snapshot-route captures=%llu hits=%llu avoided_bytes=%llu enabled=%u\n",
            static_cast<unsigned long long>(after[0] - before[0]),
            static_cast<unsigned long long>(after[1] - before[1]),
            static_cast<unsigned long long>(after[2] - before[2]), enabled ? 1u : 0u);
        check("initial-native", native_matches());
        // Mutate an interior physical alias during the SAME epoch, without
        // dirty metadata. Neither the epoch nor unmodified edges may hit.
        for (const size_t changed: {size_t(16 * pitch + 16), size_t(16 * pitch + 48)}) {
            const auto hits = TextureCacheTestAccess::UploadSnapshotStats(textures)[1];
            expected[changed] ^= 0x00abcde1u;
            Memory::WriteBacking(alias + changed * sizeof(uint32_t), &expected[changed], sizeof(uint32_t));
            textures.UpdateImage(image);
            check("alias-revalidation", TextureCacheTestAccess::UploadSnapshotStats(textures)[1] == hits);
            check("alias-native", native_matches());
        }
        const auto hits = TextureCacheTestAccess::UploadSnapshotStats(textures)[1];
        textures.InvalidateMemory(base + 4, 4);
        textures.UpdateImage(image);
        check("explicit-dirty", TextureCacheTestAccess::UploadSnapshotStats(textures)[1] == hits);
        check("explicit-native", native_matches());
        check("allocation-budget", TextureCacheTestAccess::PublicationAllocationBounded(textures));
        // The witness exists BEFORE an interior native buffer write. The
        // legacy buffer invalidation only marks images with the same base.
        constexpr size_t produced_pixel = 9 * pitch + 11;
        const auto produced_address = base + produced_pixel * sizeof(uint32_t);
        textures.InvalidateMemoryFromGPU(produced_address, sizeof(uint32_t));
        auto [producer, offset] = buffers.ObtainBuffer(produced_address, sizeof(uint32_t), true);
        producer->Fill(offset, sizeof(uint32_t), 0xff998877u);
        check("interior-native-dirty", buffers.HasGpuDirtyBytes(produced_address, sizeof(uint32_t)));
        check("interior-native-denies-existing-witness",
            !TextureCacheTestAccess::PublicationMatches(textures, image));
        expected[produced_pixel] = 0xff998877u;
        textures.UpdateImage(image);
        check("interior-native-pixels", native_matches());
        renderer.PublishGuestWrites();
        BufferCacheTestAccess::Retire(buffers, base, image_size);
        scheduler.Finish();
        textures.UpdateImage(image);
        // A different image ID can likewise own a strict subset of the same
        // VA. Exercise the real native producer before reacquiring the parent.
        auto partial = desc.info;
        partial.extent.height = height / 2;
        partial.data.size = image_size / 2;
        partial.UpdateSize();
        const auto partial_image = TextureCacheTestAccess::Insert(textures, partial);
        constexpr uint32_t partial_color = 0xff3a5b7cu;
        std::vector<uint32_t> partial_pixels(width * height / 2, partial_color);
        Buffer partial_upload(renderer.GetGraphics(), scheduler, MemoryUsage::Upload, 0,
            vk::BufferUsageFlagBits::eTransferSrc, partial_pixels.size() * sizeof(uint32_t));
        std::memcpy(partial_upload.Mapped().data(), partial_pixels.data(), partial_upload.Size());
        partial_upload.Flush(0, partial_upload.Size());
        vk::BufferImageCopy partial_copy {};
        partial_copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        partial_copy.imageExtent = partial.extent;
        textures.GetImage(partial_image).Upload(std::span(&partial_copy, 1),
            partial_upload.Handle(), 0, partial_upload.Size());
        textures.MarkGpuWritten(partial_image);
        check("overlapping-image-denies-existing-witness",
            !TextureCacheTestAccess::PublicationMatches(textures, image));
        for (uint32_t y = 0; y < height / 2; ++y)
            std::fill_n(expected.begin() + y * pitch, width, partial_color);
        textures.UpdateImage(image);
        check("overlapping-image-native-pixels", native_matches());
        TextureCacheTestAccess::Retire(textures, partial_image);
        scheduler.Finish();
    });
    // Ordinary callers have no publication authority even with equal bytes.
    check("ordinary-authority", !TextureCacheTestAccess::PublicationMatches(textures, image) &&
        TextureCacheTestAccess::PublicationBytes(textures, image) == 0);
    in_epoch([&] {
        textures.UpdateImage(image);
        check("next-epoch-native", native_matches());
        TextureCacheTestAccess::Retire(textures, image);
        pass &= TextureCacheTestAccess::PublicationBytes(textures, image) == 0;
        scheduler.Finish();
        // A GPU buffer producer supplies bytes different from CPU backing.
        // Its native upload must not get a CPU-staging snapshot.
        desc.info.data.address = base + 0x4000;
        Memory::WriteBacking(desc.info.data.address, expected.data(), image_size);
        constexpr uint32_t gpu_color = 0xff443322u;
        auto [producer, offset] = buffers.ObtainBuffer(desc.info.data.address, image_size, true);
        producer->Fill(offset, image_size, gpu_color);
        check("native-source-dirty", buffers.IsRegionGpuModified(desc.info.data.address, image_size));
        image = textures.FindImage(desc);
        const auto captures = TextureCacheTestAccess::UploadSnapshotStats(textures)[0];
        textures.UpdateImage(image);
        check("native-source-no-witness", TextureCacheTestAccess::UploadSnapshotStats(textures)[0] == captures &&
            TextureCacheTestAccess::PublicationBytes(textures, image) == 0);
        std::fill(expected.begin(), expected.end(), gpu_color);
        check("native-source-pixels", native_matches());
        renderer.PublishGuestWrites();
        TextureCacheTestAccess::Retire(textures, image);
        scheduler.Finish();
        pass &= TextureCacheTestAccess::PublicationAllocationBounded(textures);
    });
    pass &= Memory::KernelMunmap(base, mapping_size) == OK;
    pass &= Memory::KernelMunmap(alias, mapping_size) == OK;
    pass &= Memory::KernelCheckedReleaseDirectMemory(physical, mapping_size) == OK;
    std::printf("PS5_GPU_TEST case=image-upload-snapshot-exact-staging result=%s enabled=%u"
        " alias-interior-padding=same-epoch explicit-dirty=retained native-buffer=fallback retirement=checked\n",
        pass ? "PASS" : "FAIL", enabled ? 1u : 0u);
    return pass;
}

bool ImagePublicationSnapshotProbe(Libs::Graphics::RenderContext& renderer) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    constexpr uint64_t mapping_size = 0x8000, image_size = 8192;
    constexpr uint32_t width = 32, height = 32, pitch = 64;
    constexpr uint32_t first_color = 0xff345678u, second_color = 0xffa1b2c3u;
    int64_t physical = -1;
    void* mapping = nullptr;
    void* alias_mapping = nullptr;
    if (Memory::KernelAllocateMainDirectMemory(mapping_size, 0x4000, 0, &physical) != OK ||
        Memory::KernelMapDirectMemory(&mapping, mapping_size, 0x33, 0, physical, 0x4000) != OK ||
        Memory::KernelMapDirectMemory(&alias_mapping, mapping_size, 0x33, 0, physical, 0x4000) != OK)
        return false;
    const auto base = reinterpret_cast<uint64_t>(mapping);
    const auto alias = reinterpret_cast<uint64_t>(alias_mapping);
    std::vector<uint32_t> expected(image_size / sizeof(uint32_t), 0x5a6b7c8du);
    Memory::WriteBacking(base, expected.data(), image_size);
    TextureCache::ImageDesc desc;
    desc.type = TextureCache::BindingType::RenderTarget;
    desc.info.data = {base, image_size};
    desc.info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    desc.info.guest_format = Prospero::BufferFormat::k8_8_8_8UNorm;
    desc.info.extent = {width, height, 1};
    desc.info.pitch = pitch;
    desc.info.bytes_per_block = 4;
    desc.info.UpdateSize();
    desc.view_info.format = desc.info.pixel_format;
    desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
    desc.view_info.usage = vk::ImageUsageFlagBits::eColorAttachment;
    HW::Context hw;
    HW::UserConfig user;
    HW::Shader shader;
    ImageId image;
    auto& textures = renderer.GetTextureCache();
    auto& scheduler = renderer.GetCommandScheduler();
    const bool enabled = ImagePublicationSnapshotEnabled() &&
        !GuestAsyncCpuConcurrentExperimentEnabled();
    const bool host_import = renderer.GetGraphics().external_memory_host_enabled &&
        !ImagePublicationSnapshotEnabled() && !GuestAsyncCpuConcurrentExperimentEnabled() &&
        !Config::GraphicsDebugDumpEnabled();
    const auto* trace_value = std::getenv("LUMEN_PS5_IMAGE_PUBLICATION_TRACE");
    const bool traced = trace_value != nullptr && std::strcmp(trace_value, "1") == 0;
    bool pass = true;
    const auto native_matches = [&] {
        Buffer readback(renderer.GetGraphics(), scheduler, MemoryUsage::Download, 0,
            vk::BufferUsageFlagBits::eTransferDst, width * height * sizeof(uint32_t));
        vk::BufferImageCopy copy {};
        copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        textures.GetImage(image).Download(std::span(&copy, 1), readback.Handle(), 0, readback.Size());
        scheduler.Finish();
        readback.Invalidate(0, readback.Size());
        const auto* actual = reinterpret_cast<const uint32_t*>(readback.Mapped().data());
        bool equal = true;
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
                equal &= actual[y * width + x] == expected[y * pitch + x];
        return equal;
    };
    const auto in_epoch = [&](auto&& operation) {
        renderer.GetGpu().SendCommandSync([&] {
            GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
            const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
            if (epoch == 0) EXIT("image publication snapshot fixture epoch denied\n");
            GuestGpuPublicationActive = true;
            scheduler.Begin(hw, user, shader);
            operation();
            GuestGpuPublicationActive = false;
            pass &= LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0;
        });
    };
    const auto republish = [&] {
        textures.MarkGpuWritten(image);
        renderer.PublishGuestWrites();
        pass &= TextureCacheTestAccess::PublicationBytes(textures, image) == (enabled ? image_size : 0);
        pass &= Memory::TryEqualBacking(base, expected.data(), image_size);
        pass &= TextureCacheTestAccess::PublicationAllocationBounded(textures);
    };
    in_epoch([&] {
        image = textures.FindImage(desc);
        (void)textures.FindRenderTarget(image, desc);
        pass &= textures.ClearImageFromBuffer(scheduler.Current(), base, image_size, first_color);
        for (uint32_t y = 0; y < height; ++y)
            std::fill_n(expected.begin() + y * pitch, width, first_color);
        const auto before_prefill = TextureCacheTestAccess::PrefillBytes(textures);
        pass &= !TextureCacheTestAccess::FullDownload(textures, image);
        renderer.PublishGuestWrites();
        const auto after_prefill = TextureCacheTestAccess::PrefillBytes(textures);
        pass &= !traced || (after_prefill.first == before_prefill.first + (host_import ? image_size : 0) &&
            after_prefill.second == before_prefill.second + (host_import ? 0 : image_size));
        pass &= Memory::TryEqualBacking(base, expected.data(), image_size) &&
            Memory::TryEqualBacking(alias, expected.data(), image_size);
        pass &= TextureCacheTestAccess::PublicationBytes(textures, image) == (enabled ? image_size : 0);
        const auto revision = textures.GetImage(image).PublicationRevision();
        textures.UpdateImage(image);
        pass &= (textures.GetImage(image).PublicationRevision() == revision) == enabled;
        pass &= native_matches();
        pass &= TextureCacheTestAccess::PublicationBudgetBoundaries(textures);
        pass &= TextureCacheTestAccess::PublicationRangeUnionProbe();
        pass &= TextureCacheTestAccess::PublicationAllocationBounded(textures);
    });
    std::printf("PS5_GPU_TEST case=image-publication-snapshot-unchanged-padding result=%s enabled=%u\n",
        pass ? "PASS" : "FAIL", enabled ? 1u : 0u);
    // A native pin or ordinary caller is not proof of CPU quiescence.
    pass &= !TextureCacheTestAccess::PublicationMatches(textures, image) &&
        TextureCacheTestAccess::PublicationBytes(textures, image) == 0;
    in_epoch(republish);

    // A second real image covers every byte: tightly packed color rows, no
    // missing layer/mip/row padding. OFF must retain the original prefill.
    in_epoch([&] {
        auto tight = desc;
        tight.info.data.address = base + 0x4000;
        tight.info.extent.width = pitch;
        Memory::WriteBacking(tight.info.data.address, expected.data(), image_size);
        const auto tight_image = textures.FindImage(tight);
        (void)textures.FindRenderTarget(tight_image, tight);
        pass &= TextureCacheTestAccess::FullDownload(textures, tight_image);
        pass &= textures.ClearImageFromBuffer(scheduler.Current(), tight.info.data.address, image_size, first_color);
        const auto before = TextureCacheTestAccess::PrefillBytes(textures);
        renderer.PublishGuestWrites();
        const auto after = TextureCacheTestAccess::PrefillBytes(textures);
        pass &= !traced || (after.first == before.first + (enabled || host_import ? image_size : 0) &&
            after.second == before.second + (enabled || host_import ? 0 : image_size));
        std::vector<uint32_t> tight_expected(image_size / sizeof(uint32_t), first_color);
        pass &= Memory::TryEqualBacking(tight.info.data.address, tight_expected.data(), image_size);
        textures.UpdateImage(tight_image);
        pass &= TextureCacheTestAccess::PublicationAllocationBounded(textures);
        TextureCacheTestAccess::Retire(textures, tight_image);
        scheduler.Finish();
        pass &= TextureCacheTestAccess::PublicationAllocationBounded(textures);
    });
    std::printf("PS5_GPU_TEST case=image-publication-prefill-full-versus-padding result=%s"
        " snapshot_enabled=%u trace_enabled=%u full_bytes=8192 padded_preserved_bytes=8192\n",
        pass ? "PASS" : "FAIL", enabled ? 1u : 0u, traced ? 1u : 0u);

    // A translated store can change a different physical alias without dirty
    // metadata. Use the real backing path, with no cache invalidation shortcut.
    constexpr size_t middle_pixel = 16 * pitch + 16, middle_padding = 16 * pitch + 48;
    expected[middle_pixel] = 0xffeeddccu;
    Memory::WriteBacking(alias + middle_pixel * 4, &expected[middle_pixel], 4);
    in_epoch([&] {
        pass &= !TextureCacheTestAccess::PublicationMatches(textures, image);
        textures.UpdateImage(image);
        pass &= native_matches();
        republish();
    });
    expected[middle_padding] = 0x1234abcdu;
    Memory::WriteBacking(alias + middle_padding * 4, &expected[middle_padding], 4);
    in_epoch([&] {
        pass &= !TextureCacheTestAccess::PublicationMatches(textures, image);
        textures.UpdateImage(image);
        pass &= native_matches();
        republish();
        // Explicit invalidation must remain authoritative even for equal bytes.
        textures.InvalidateMemory(base + middle_pixel * 4, 4);
        pass &= !TextureCacheTestAccess::PublicationMatches(textures, image);
        textures.UpdateImage(image);
        pass &= native_matches();
        republish();
        // Layout mismatch cannot preserve a witness merely because bytes agree.
        auto& owner = textures.GetImage(image);
        owner.info.pitch++;
        pass &= !TextureCacheTestAccess::PublicationMatches(textures, image);
        owner.info.pitch--;
        textures.UpdateImage(image);
        republish();
        // Conversion and metadata capabilities always retain the original path.
        owner.info.bgra16 = true;
        pass &= !TextureCacheTestAccess::PublicationMatches(textures, image);
        owner.info.bgra16 = false;
        textures.UpdateImage(image);
        republish();
        const auto render_target = owner.usage.render_target;
        owner.usage.render_target = false;
        pass &= !TextureCacheTestAccess::PublicationMatches(textures, image);
        owner.usage.render_target = render_target;
        textures.UpdateImage(image);
        republish();
        pass &= textures.ClearImageFromBuffer(scheduler.Current(), base, image_size, second_color);
        pass &= !TextureCacheTestAccess::PublicationMatches(textures, image);
        for (uint32_t y = 0; y < height; ++y)
            std::fill_n(expected.begin() + y * pitch, width, second_color);
        renderer.PublishGuestWrites();
        textures.UpdateImage(image);
        pass &= native_matches() && Memory::TryEqualBacking(base, expected.data(), image_size);
        // Retirement must remove the witness before a slot/native owner is reused.
        TextureCacheTestAccess::Retire(textures, image);
        pass &= TextureCacheTestAccess::PublicationBytes(textures, image) == 0;
        scheduler.Finish();
        image = textures.FindImage(desc);
        pass &= TextureCacheTestAccess::PublicationBytes(textures, image) == 0;
        textures.UpdateImage(image);
        pass &= native_matches();
        republish();
    });
    std::printf("PS5_GPU_TEST case=image-publication-snapshot-alias-interior-retirement result=%s enabled=%u\n",
        pass ? "PASS" : "FAIL", enabled ? 1u : 0u);
    pass &= Memory::KernelMunmap(base, mapping_size) == OK;
    pass &= !Memory::TryEqualBacking(base, expected.data(), image_size) &&
        Memory::TryEqualBacking(alias, expected.data(), image_size);
    pass &= TextureCacheTestAccess::PublicationBytes(textures, image) == 0;
    pass &= Memory::KernelMunmap(alias, mapping_size) == OK;
    pass &= Memory::KernelCheckedReleaseDirectMemory(physical, mapping_size) == OK;
    std::printf("PS5_GPU_TEST case=image-publication-snapshot-revoked-range-budget result=%s enabled=%u\n",
        pass ? "PASS" : "FAIL", enabled ? 1u : 0u);
    return pass;
}

bool HostImportRetileAliasProbe(Libs::Graphics::RenderContext& renderer) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    auto& graphics = renderer.GetGraphics();
    if (!graphics.external_memory_host_enabled || ImagePublicationSnapshotEnabled() ||
        GuestAsyncCpuConcurrentExperimentEnabled() || Config::GraphicsDebugDumpEnabled()) {
        std::printf("PS5_GPU_TEST case=host-import-retile-alias-padding result=SKIP reason=disabled-or-incompatible\n");
        return true;
    }
    constexpr uint32_t width = 131, height = 137, layers = 13;
    constexpr uint64_t prefix = 256;
    constexpr auto format = Prospero::BufferFormat::k8_8_8_8UNorm;
    constexpr auto mode = Prospero::TileMode::kRenderTarget;
    TileSizeAlign slice {};
    TileGetTextureTotalSize(format, width, height, 1, 1, mode, false, slice);
    TileBlockLayout block {};
    if (!TileGetBlockLayout(TileBlockFamily::RenderTarget64KB, 4, block)) return false;
    const auto pitch = TileGetTexturePitch(format, width, mode);
    const uint64_t image_size = static_cast<uint64_t>(slice.size) * layers;
    const auto alignment = std::max<uint64_t>(0x4000, graphics.min_imported_host_pointer_alignment);
    const auto mapping_size = (prefix + image_size + alignment * 2 - 1) & ~(alignment - 1);
    int64_t physical = -1;
    void* mapping = nullptr;
    void* alias_mapping = nullptr;
    if (Memory::KernelAllocateMainDirectMemory(mapping_size, alignment, 0, &physical) != OK ||
        Memory::KernelMapDirectMemory(&mapping, mapping_size, 0x33, 0, physical, alignment) != OK ||
        Memory::KernelMapDirectMemory(&alias_mapping, mapping_size, 0x33, 0, physical, alignment) != OK)
        return false;
    const auto base = reinterpret_cast<uint64_t>(mapping);
    const auto alias = reinterpret_cast<uint64_t>(alias_mapping);
    std::vector<uint8_t> initial(mapping_size, 0xcd), expected = initial;
    std::vector<uint32_t> native(static_cast<uint64_t>(width) * height * layers);
    for (uint32_t layer = 0; layer < layers; ++layer)
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                uint32_t local = 0, swizzle = 0;
                if (!TileGetBlockOffset(block, x % block.block_width, y % block.block_height, 0, local) ||
                    !TileGetBlockXor(block, x / block.block_width, y / block.block_height, layer, swizzle))
                    return false;
                const auto tile = static_cast<uint64_t>(y / block.block_height) *
                    (pitch / block.block_width) + x / block.block_width;
                const auto offset = prefix + static_cast<uint64_t>(slice.size) * layer +
                    tile * block.block_size + (local ^ swizzle);
                const auto word = 0xff000000u | ((x * 13u + y * 79u + layer * 997u) & 0xffffffu);
                std::memcpy(expected.data() + offset, &word, sizeof(word));
                native[(static_cast<uint64_t>(layer) * height + y) * width + x] = word;
            }
    TextureCache::ImageDesc desc;
    desc.type = TextureCache::BindingType::RenderTarget;
    desc.info.data = {base + prefix, image_size};
    desc.info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    desc.info.guest_format = format;
    desc.info.extent = {width, height, 1};
    desc.info.resources = {1, layers};
    desc.info.pitch = pitch;
    desc.info.bytes_per_block = 4;
    desc.info.tile_mode = mode;
    desc.info.UpdateSize();
    desc.view_info.format = desc.info.pixel_format;
    desc.view_info.type = vk::ImageViewType::e2DArray;
    desc.view_info.layer_count = layers;
    desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
    desc.view_info.usage = vk::ImageUsageFlagBits::eColorAttachment;
    auto& scheduler = renderer.GetCommandScheduler();
    auto& textures = renderer.GetTextureCache();
    HW::Context hw;
    HW::UserConfig user;
    HW::Shader shader;
    bool pass = true;
    uint64_t imported_bytes = 0;
    // Run the identical nonuniform upload through staged publication first,
    // then import, comparing the entire backing envelope through both aliases.
    for (const bool imported: {false, true}) {
        Memory::WriteBacking(base, initial.data(), mapping_size);
        renderer.GetGpu().SendCommandSync([&] {
            GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
            const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
            if (epoch == 0) EXIT("host import fixture epoch denied\n");
            GuestGpuPublicationActive = true;
            scheduler.Begin(hw, user, shader);
            graphics.external_memory_host_enabled = imported;
            const auto image = textures.FindImage(desc);
            (void)textures.FindRenderTarget(image, desc);
            Buffer upload(graphics, scheduler, MemoryUsage::Upload, 0,
                vk::BufferUsageFlagBits::eTransferSrc, native.size() * sizeof(uint32_t));
            std::memcpy(upload.Mapped().data(), native.data(), upload.Size());
            upload.Flush(0, upload.Size());
            std::array<vk::BufferImageCopy, layers> copies {};
            for (uint32_t layer = 0; layer < layers; ++layer) {
                copies[layer].bufferOffset = static_cast<uint64_t>(layer) * width * height * sizeof(uint32_t);
                copies[layer].bufferRowLength = width;
                copies[layer].bufferImageHeight = height;
                copies[layer].imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, layer, 1};
                copies[layer].imageExtent = {width, height, 1};
            }
            textures.GetImage(image).Upload(copies, upload.Handle(), 0, upload.Size());
            textures.MarkGpuWritten(image);
            // An earlier priority reader must finish before direct GPU writes.
            bool earlier_reader = false;
            scheduler.DeferPriorityOperation([&] {
                earlier_reader = Memory::TryEqualBacking(alias, initial.data(), mapping_size);
            });
            const auto before = TextureCacheTestAccess::HostImportBytes(textures);
            renderer.PublishGuestWrites();
            const auto delta = TextureCacheTestAccess::HostImportBytes(textures) - before;
            pass &= earlier_reader && delta == (imported ? image_size : 0) &&
                Memory::TryEqualBacking(base, expected.data(), mapping_size) &&
                Memory::TryEqualBacking(alias, expected.data(), mapping_size);
            imported_bytes += delta;
            textures.UnmapMemory(desc.info.data.address, image_size);
            scheduler.Finish();
            graphics.external_memory_host_enabled = true;
            GuestGpuPublicationActive = false;
            pass &= LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0;
        });
    }
    pass &= Memory::KernelMunmap(base, mapping_size) == OK;
    pass &= Memory::KernelMunmap(alias, mapping_size) == OK;
    pass &= Memory::KernelCheckedReleaseDirectMemory(physical, mapping_size) == OK;
    std::printf("PS5_GPU_TEST case=host-import-retile-alias-padding result=%s layers=%u imported_bytes=%llu prefix=%llu envelope_bytes=%llu\n",
        pass ? "PASS" : "FAIL", layers, static_cast<unsigned long long>(imported_bytes),
        static_cast<unsigned long long>(prefix), static_cast<unsigned long long>(mapping_size));
    return pass;
}

bool HostImportBatchProbe(Libs::Graphics::RenderContext& renderer) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    auto& graphics = renderer.GetGraphics();
    if (!graphics.external_memory_host_enabled || ImagePublicationSnapshotEnabled() ||
        GuestAsyncCpuConcurrentExperimentEnabled() || Config::GraphicsDebugDumpEnabled()) {
        std::printf("PS5_GPU_TEST case=host-import-batch result=SKIP reason=disabled-or-incompatible\n");
        return true;
    }
    auto& scheduler = renderer.GetCommandScheduler();
    auto& textures = renderer.GetTextureCache();
    HW::Context hw;
    HW::UserConfig user;
    HW::Shader shader;
    constexpr uint64_t image_size = 65536;
    const auto alignment = std::max<uint64_t>(0x4000, graphics.min_imported_host_pointer_alignment);
    const auto stride = ((image_size + alignment - 1) & ~(alignment - 1)) + alignment;
    bool pass = true;
    for (const auto count: {3u, 4u, 65u}) {
        const bool middle_failure = count == 4;
        const uint64_t imported_owners = middle_failure ? 2 : std::min(count, 64u);
        const uint64_t mapping_size = (count + 2) * stride;
        int64_t physical = -1;
        void* mapping = nullptr;
        void* alias_mapping = nullptr;
        if (Memory::KernelAllocateMainDirectMemory(mapping_size, alignment, 0, &physical) != OK ||
            Memory::KernelMapDirectMemory(&mapping, mapping_size, 0x33, 0, physical, alignment) != OK ||
            Memory::KernelMapDirectMemory(&alias_mapping, mapping_size, 0x33, 0, physical, alignment) != OK)
            return false;
        const auto base = reinterpret_cast<uint64_t>(mapping);
        const auto alias = reinterpret_cast<uint64_t>(alias_mapping);
        uint64_t prefix = 256;
        renderer.GetGpu().SendCommandSync([&] {
            GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
            const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
            if (epoch == 0) EXIT("host import batch fixture alignment epoch denied\n");
            Memory::GuestGpuBackingImport descriptor;
            const bool acquired = Memory::TryAcquireGuestGpuBackingImport(base, 1, 1, &descriptor);
            pass &= acquired;
            if (acquired) prefix += (alignment - (reinterpret_cast<uintptr_t>(descriptor.HostPointer()) &
                (alignment - 1))) & (alignment - 1);
            descriptor.Reset();
            pass &= LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0;
        });
        std::vector<uint8_t> initial(mapping_size, 0xcd), expected = initial;
        std::vector<uint64_t> offsets(count);
        for (uint32_t i = 0; i < count; ++i) {
            offsets[i] = prefix + i * stride;
            // Payloads are disjoint, but owners 1 and 2 share one aligned
            // import envelope page. Owner 2 must stage after both imports die.
            if (middle_failure && i == 2) offsets[i] = offsets[1] + image_size;
            const auto color = 0xff000000u | (i + 1) * 0x010307u;
            for (uint64_t offset = 0; offset < image_size; offset += sizeof(color))
                std::memcpy(expected.data() + offsets[i] + offset, &color, sizeof(color));
        }
        Memory::WriteBacking(base, initial.data(), mapping_size);
        bool phase_pass = true;
        renderer.GetGpu().SendCommandSync([&] {
            GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
            const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
            if (epoch == 0) EXIT("host import batch fixture epoch denied\n");
            GuestGpuPublicationActive = true;
            scheduler.Begin(hw, user, shader);
            std::vector<ImageId> images;
            for (uint32_t i = 0; i < count; ++i) {
                TextureCache::ImageDesc desc;
                desc.type = TextureCache::BindingType::RenderTarget;
                desc.info.data = {base + offsets[i], image_size};
                desc.info.pixel_format = vk::Format::eR8G8B8A8Unorm;
                desc.info.guest_format = Prospero::BufferFormat::k8_8_8_8UNorm;
                desc.info.extent = {128, 128, 1};
                desc.info.pitch = 128;
                desc.info.bytes_per_block = 4;
                desc.info.tile_mode = Prospero::TileMode::kRenderTarget;
                desc.info.UpdateSize();
                desc.view_info.format = desc.info.pixel_format;
                desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
                desc.view_info.usage = vk::ImageUsageFlagBits::eColorAttachment;
                const auto image = textures.FindImage(desc);
                (void)textures.FindRenderTarget(image, desc);
                phase_pass &= textures.ClearImageFromBuffer(scheduler.Current(), desc.info.data.address,
                    image_size, 0xff000000u | (i + 1) * 0x010307u);
                images.push_back(image);
            }
            bool earlier_reader = false;
            scheduler.DeferPriorityOperation([&] {
                earlier_reader = Memory::TryEqualBacking(alias, initial.data(), mapping_size);
            });
            const auto before = TextureCacheTestAccess::HostImportStats(textures);
            TextureCacheTestAccess::PublishOrderedImages(textures, images);
            const auto after = TextureCacheTestAccess::HostImportStats(textures);
            phase_pass &= earlier_reader && after[0] - before[0] == imported_owners * image_size &&
                after[1] - before[1] == 1 && after[2] - before[2] == imported_owners &&
                after[3] - before[3] == (count > 64 ? 1u : 0u) &&
                Memory::TryEqualBacking(base, expected.data(), mapping_size) &&
                Memory::TryEqualBacking(alias, expected.data(), mapping_size);
            for (const auto id: images) phase_pass &= !textures.GetImage(id).IsGpuModified();
            textures.UnmapMemory(base, mapping_size);
            scheduler.Finish();
            GuestGpuPublicationActive = false;
            phase_pass &= LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0;
        });
        pass &= phase_pass;
        pass &= Memory::KernelMunmap(base, mapping_size) == OK;
        pass &= Memory::KernelMunmap(alias, mapping_size) == OK;
        pass &= Memory::KernelCheckedReleaseDirectMemory(physical, mapping_size) == OK;
        std::printf("PS5_GPU_TEST case=host-import-batch result=%s owners=%u imported_owners=%llu staged_owners=%llu envelope_overlap=%u cap_stop=%u padding=checked aliases=2 prior_priority=checked\n",
            phase_pass ? "PASS" : "FAIL", count, static_cast<unsigned long long>(imported_owners),
            static_cast<unsigned long long>(count - imported_owners), middle_failure ? 1u : 0u, count > 64 ? 1u : 0u);
    }
    return pass;
}

bool HostImportDeferredVmProbe(Libs::Graphics::RenderContext& renderer) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    auto& graphics = renderer.GetGraphics();
    if (!graphics.external_memory_host_enabled) return true;
    const auto alignment = std::max<uint64_t>(0x4000, graphics.min_imported_host_pointer_alignment);
    const auto mapping_size = alignment * 6;
    void* mapping = nullptr;
    if (Memory::KernelMapFlexibleMemory(&mapping, mapping_size, 0x33, 0) != OK) return false;
    const auto base = reinterpret_cast<uint64_t>(mapping);
    std::vector<uint8_t> expected(mapping_size, 0xcd);
    Memory::WriteBacking(base, expected.data(), mapping_size);
    auto& scheduler = renderer.GetCommandScheduler();
    HW::Context hw;
    HW::UserConfig user;
    HW::Shader shader;
    bool pass = true;
    bool callback_ran = false;
    renderer.GetGpu().SendCommandSync([&] {
        GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
        const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (epoch == 0) EXIT("host import deferred VM fixture epoch denied\n");
        GuestGpuPublicationActive = true;
        scheduler.Begin(hw, user, shader);
        const auto prior_tick = scheduler.CurrentTick();
        scheduler.Finish();
        scheduler.WaitPriorityOperations(prior_tick);
        Memory::GuestGpuBackingImport origin;
        const bool acquired = Memory::TryAcquireGuestGpuBackingImport(base, 1, 1, &origin);
        pass &= acquired;
        uint64_t prefix = 0;
        if (acquired) prefix = (alignment - (reinterpret_cast<uintptr_t>(origin.HostPointer()) &
            (alignment - 1))) & (alignment - 1);
        origin.Reset();
        struct Imported {
            Memory::GuestGpuBackingImport backing;
            std::unique_ptr<Buffer> buffer;
        };
        std::array<Imported, 2> imports;
        bool ready = acquired;
        for (size_t i = 0; i < imports.size() && ready; ++i) {
            const auto address = base + prefix + i * alignment * 2;
            ready = Memory::TryAcquireGuestGpuBackingImport(address, alignment,
                graphics.min_imported_host_pointer_alignment, &imports[i].backing);
            if (ready) imports[i].buffer = Buffer::TryImportHostMemory(graphics, scheduler,
                imports[i].backing.HostPointer(), imports[i].backing.ByteSize());
            ready &= imports[i].buffer != nullptr;
        }
        pass &= ready;
        if (ready) {
            for (size_t i = 0; i < imports.size(); ++i) {
                const auto color = 0x12345678u + static_cast<uint32_t>(i);
                imports[i].buffer->Fill(imports[i].backing.GuestOffset(), alignment, color);
                for (uint64_t offset = 0; offset < alignment; offset += sizeof(color))
                    std::memcpy(expected.data() + prefix + i * alignment * 2 + offset, &color, sizeof(color));
                vk::BufferMemoryBarrier barrier {};
                barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
                barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.buffer = imports[i].buffer->Handle();
                barrier.offset = imports[i].backing.GuestOffset();
                barrier.size = alignment;
                scheduler.Current().Handle().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                    vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1, &barrier, 0, nullptr);
            }
            scheduler.DeferOperation([&] {
                callback_ran = true;
                const auto nested = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
                pass &= nested != 0;
                if (nested != 0)
                    pass &= LumenAndroidFexCoreEndGuestMemoryMutationV2(nested, 0, 0, 0, nullptr, nullptr) != 0;
                pass &= Memory::KernelMprotect(mapping, mapping_size, 0x31) == OK;
                pass &= Memory::TryEqualBacking(base, expected.data(), mapping_size);
            });
            auto ticket = scheduler.PrepareCompletion();
            scheduler.WaitNativeCompletion(ticket);
            pass &= !callback_ran;
            for (auto& imported: imports) {
                imported.buffer.reset();
                imported.backing.Reset();
            }
            scheduler.CompleteDeferredCompletion(std::move(ticket));
        } else {
            for (auto& imported: imports) {
                imported.buffer.reset();
                imported.backing.Reset();
            }
        }
        pass &= callback_ran;
        scheduler.Finish();
        GuestGpuPublicationActive = false;
        pass &= LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0;
    });
    pass &= Memory::KernelMunmap(base, mapping_size) == OK;
    std::printf("PS5_GPU_TEST case=host-import-batch-ordinary-vm-after-release result=%s imports=2 nested-mutation=checked mprotect=checked bytes-and-guards=checked\n",
        pass ? "PASS" : "FAIL");
    return pass;
}

bool BufferPublicationWitnessAliasRemapProbe(Libs::Graphics::RenderContext& renderer) {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    constexpr uint64_t size = 0x8000;
    constexpr uint64_t alignment = 0x4000;
    int64_t physical = -1, replacement_physical = -1;
    void* mapping = nullptr;
    void* alias_mapping = nullptr;
    if (Memory::KernelAllocateMainDirectMemory(size, alignment, 0, &physical) != OK ||
        Memory::KernelAllocateMainDirectMemory(size, alignment, 0, &replacement_physical) != OK ||
        Memory::KernelMapDirectMemory(&mapping, size, 0x33, 0, physical, alignment) != OK ||
        Memory::KernelMapDirectMemory(&alias_mapping, size, 0x33, 0, physical, alignment) != OK)
        return false;
    const auto base = reinterpret_cast<uint64_t>(mapping);
    const auto alias = reinterpret_cast<uint64_t>(alias_mapping);
    const auto* batch_selector = std::getenv("LUMEN_PS5_GFX_BUFFER_PUBLICATION_BATCH");
    const bool enabled = BufferPublicationWitnessEnabled() && GuestBufferSnapshotReuseEnabled() &&
        !GuestAsyncPrivateBackingEnabled() &&
        (batch_selector == nullptr || std::strcmp(batch_selector, "1") == 0);
    auto& buffers = renderer.GetBufferCache();
    auto& scheduler = renderer.GetCommandScheduler();
    HW::Context hw;
    HW::UserConfig user;
    HW::Shader shader;
    std::vector<uint32_t> expected(size / 4, 0x789a2468u);
    Memory::WriteBacking(base, expected.data(), size);
    bool pass = true;
    uint64_t previous_mapping_id = 0;
    auto on_gpu = [&](auto&& operation) {
        renderer.GetGpu().SendCommandSync([&] {
            GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
            const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
            if (epoch == 0) EXIT("publication witness alias/remap epoch denied\n");
            GuestGpuPublicationActive = true;
            scheduler.Begin(hw, user, shader);
            operation();
            scheduler.Finish();
            GuestGpuPublicationActive = false;
            pass &= LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0;
        });
    };
    auto native_equal = [&](uint64_t address, const std::vector<uint32_t>& bytes) {
        auto [buffer, offset] = buffers.ObtainBuffer(address, size, false);
        Buffer download(renderer.GetGraphics(), scheduler, MemoryUsage::Download, 0,
            vk::BufferUsageFlagBits::eTransferDst, size);
        download.CopyFrom(scheduler.Current(), *buffer, offset, 0, size);
        scheduler.Finish();
        download.Invalidate(0, size);
        return std::memcmp(download.Mapped().data(), bytes.data(), size) == 0;
    };
    on_gpu([&] {
        // Establish a separate, clean native alias before publishing the source.
        pass &= native_equal(alias, expected);
        auto [buffer, offset] = buffers.ObtainBuffer(base, size, true);
        buffer->Fill(offset, size, 0x11224488u);
        std::fill(expected.begin(), expected.end(), 0x11224488u);
        renderer.PublishGuestWrites();
        pass &= !buffers.IsRegionCpuModified(base, size) && buffers.IsRegionCpuModified(alias, size) &&
            Memory::TryEqualBacking(base, expected.data(), size) &&
            Memory::TryEqualBacking(alias, expected.data(), size);
        auto before = BufferCacheTestAccess::PublicationWitnessStats(buffers);
        pass &= native_equal(base, expected);
        auto after = BufferCacheTestAccess::PublicationWitnessStats(buffers);
        pass &= after[3] - before[3] == (enabled ? 0 : size);
        before = after;
        pass &= native_equal(alias, expected);
        after = BufferCacheTestAccess::PublicationWitnessStats(buffers);
        pass &= after[3] - before[3] == size; // Alias invalidation is never forgiven.
        expected[TRACKER_PAGE_SIZE / 4 + 17] ^= 0x01008000u;
        Memory::WriteBacking(alias + TRACKER_PAGE_SIZE + 17 * 4,
            &expected[TRACKER_PAGE_SIZE / 4 + 17], 4);
        pass &= native_equal(base, expected);
        std::tie(buffer, offset) = buffers.ObtainBuffer(base, size, true);
        buffer->Fill(offset, size, 0x9053c7a1u);
        std::fill(expected.begin(), expected.end(), 0x9053c7a1u);
        renderer.PublishGuestWrites();
        std::vector<Memory::GuestBackingRange> ranges;
        pass &= Memory::QueryGuestBackingRanges(base, size, &ranges) && ranges.size() == 1;
        if (!ranges.empty()) previous_mapping_id = ranges.front().mapping_id;
    });
    std::printf("PS5_GPU_TEST case=publication-witness-physical-alias result=%s source-clean=checked alias-dirty=retained cpu-alias-edit=native-checked\n", pass ? "PASS" : "FAIL");
    pass &= Memory::KernelMunmap(base, size) == OK;
    void* replacement = reinterpret_cast<void*>(base);
    pass &= Memory::KernelMapDirectMemory(&replacement, size, 0x33, 0x10,
        replacement_physical, alignment) == OK && reinterpret_cast<uint64_t>(replacement) == base;
    std::vector<uint32_t> replacement_bytes(size / 4, 0xdead7246u);
    Memory::WriteBacking(base, replacement_bytes.data(), size);
    on_gpu([&] {
        std::vector<Memory::GuestBackingRange> ranges;
        const bool identity = Memory::QueryGuestBackingRanges(base, size, &ranges) &&
            ranges.size() == 1 && ranges.front().mapping_id != previous_mapping_id;
        const bool forgotten = BufferCacheTestAccess::Compare(buffers, base,
            replacement_bytes.data(), TRACKER_PAGE_SIZE) == BackingComparison::Missing;
        const auto before = BufferCacheTestAccess::PublicationWitnessStats(buffers);
        const bool replacement_native = native_equal(base, replacement_bytes);
        const bool alias_native = native_equal(alias, expected);
        const auto uploaded = BufferCacheTestAccess::PublicationWitnessStats(buffers)[3] - before[3];
        pass &= identity && forgotten && replacement_native && alias_native && uploaded >= size;
        std::printf("PS5_GPU_TEST case=publication-witness-remap-details result=%s identity=%u forgotten=%u replacement_native=%u alias_native=%u actual_upload_bytes=%llu\n",
            pass ? "PASS" : "FAIL", identity ? 1u : 0u, forgotten ? 1u : 0u,
            replacement_native ? 1u : 0u, alias_native ? 1u : 0u,
            static_cast<unsigned long long>(uploaded));
    });
    pass &= Memory::KernelMunmap(base, size) == OK;
    pass &= Memory::KernelMunmap(alias, size) == OK;
    pass &= Memory::KernelCheckedReleaseDirectMemory(physical, size) == OK;
    pass &= Memory::KernelCheckedReleaseDirectMemory(replacement_physical, size) == OK;
    std::printf("PS5_GPU_TEST case=publication-witness-remap result=%s same-va=new-identity old-witness=forgotten replacement-native-bytes=checked old-alias=preserved\n", pass ? "PASS" : "FAIL");
    return pass;
}

bool GpuBackingSelfTest() noexcept {
    using namespace Libs::Graphics;
    namespace Memory = Libs::LibKernel::Memory;
    Common::InitializeThreads();
    if (!InitializeRuntime()) return false;
    Common::Subsystems subsystems;
    subsystems.Initialize<Config::Lifecycle>();
    Config::ConfigOptions options;
    options.printf_direction = Config::LogDirection::Silent;
    Config::Load(options);
    subsystems.Initialize<Log::Lifecycle>();
    subsystems.Initialize<Memory::Lifecycle>();
    if (!Memory::InitializeFexMemoryAccess()) return false;
    void* memory = nullptr;
    constexpr uint64_t memory_size = 0x400000;
    if (Memory::KernelMapFlexibleMemory(&memory, memory_size, 0x32, 0) != OK) return false;
    const auto base = reinterpret_cast<uint64_t>(memory);
    std::vector<uint32_t> bytes(memory_size / 4, 0xa5a5a5a5u);
    Memory::WriteBacking(base, bytes.data(), memory_size);
    bool pass = TextureCacheTestAccess::PublicationBlockSeedProbe() &&
        BufferBackingRevalidationPolicyProbe() && !GuestGpuWriteOwnershipSupported() &&
        !GuestGcPublicationDeferred() &&
        Memory::QueryGpuBackingWrite(base, memory_size) &&
        !Memory::QueryGpuBackingWrite(base + 0x4000, memory_size);
    // GPU rights are independent of CPU view rights.
    pass &= Memory::KernelMprotect(memory, memory_size, 0x20) == OK;
    pass &= !Memory::QueryGuestAccess(base, 4, Common::VirtualMemory::Mode::Read) &&
        Memory::QueryGpuBackingWrite(base, memory_size);
    std::printf("PS5_GPU_TEST case=write-rights-cpu-noaccess result=%s\n", pass ? "PASS" : "FAIL");
    auto& renderer = WindowInit(128, 128).Renderer();
    renderer.InitializeGpu(nullptr);
    ShaderInit();
    HW::Context hw;
    HW::UserConfig user;
    HW::Shader shader;
    renderer.GetGpu().SendCommandSync([&] {
        GuestWriterGate intent(GuestWriterGate::Admission::NativeRootEntry);
        const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (epoch == 0) EXIT("GPU backing test epoch denied\n");
        GuestGpuPublicationActive = true;
        auto& scheduler = renderer.GetCommandScheduler();
        scheduler.Begin(hw, user, shader);
        auto& buffers = renderer.GetBufferCache();
        auto [buffer, offset] = buffers.ObtainBuffer(base + 16, 64, true);
        buffer->Fill(offset, 64, 0x12345678u);
        renderer.PublishGuestWrites();
        pass &= !buffers.HasGpuDirtyBytes(base, 0x4000) &&
            !buffers.IsRegionGpuModified(base, 0x4000);
        std::array<uint32_t, 20> actual {};
        pass &= Memory::TryReadBacking(base, actual.data(), sizeof(actual));
        for (size_t i = 0; i < actual.size(); ++i)
            pass &= actual[i] == (i >= 4 ? 0x12345678u : 0xa5a5a5a5u);
        std::printf("PS5_GPU_TEST case=exact-buffer-publication result=%s first=%08x written=%08x\n",
                    pass ? "PASS" : "FAIL", actual[0], actual[4]);

        // Exercise the guest upload path and a real native copy, avoiding the CPU-copy shortcut.
        std::array<uint32_t, 32> upload;
        for (size_t i = 0; i < upload.size(); ++i) upload[i] = 0x13570000u + i;
        Memory::WriteBacking(base + 0x2000, upload.data(), sizeof(upload));
        auto [source, source_offset] = buffers.ObtainBuffer(base + 0x2000, sizeof(upload), false);
        auto [target, target_offset] = buffers.ObtainBuffer(base + 0x3000, sizeof(upload), true);
        target->CopyFrom(scheduler.Current(), *source, source_offset, target_offset, sizeof(upload));
        renderer.PublishGuestWrites();
        std::array<uint32_t, 32> uploaded {};
        pass &= Memory::TryReadBacking(base + 0x3000, uploaded.data(), sizeof(uploaded));
        pass &= uploaded == upload;
        std::printf("PS5_GPU_TEST case=guest-buffer-upload-native-copy result=%s first=%08x last=%08x\n",
            pass ? "PASS" : "FAIL", uploaded.front(), uploaded.back());

        pass &= BufferPublicationWitnessProbe(renderer, base);
        pass &= BufferBackingRevalidationProbe(renderer, base);
        pass &= BufferPublicationBatchProbe(renderer, base);
        pass &= CompletionTransactionProbe(renderer);
        pass &= GarbageCollectionPublicationProbe(renderer, base);

        TextureCache::ImageDesc desc;
        desc.type = TextureCache::BindingType::RenderTarget;
        desc.info.data = {base + 0x4000, 8192};
        desc.info.pixel_format = vk::Format::eR8G8B8A8Unorm;
        desc.info.guest_format = Prospero::BufferFormat::k8_8_8_8UNorm;
        desc.info.type = Prospero::ImageType::kColor2D;
        desc.info.extent = {32, 32, 1};
        desc.info.resources = {1, 1};
        desc.info.pitch = 64;
        desc.info.bytes_per_block = 4;
        desc.info.UpdateSize();
        desc.view_info.format = desc.info.pixel_format;
        desc.view_info.type = vk::ImageViewType::e2D;
        desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
        desc.view_info.usage = vk::ImageUsageFlagBits::eColorAttachment;
        auto& textures = renderer.GetTextureCache();
        const auto image = textures.FindImage(desc);
        (void)textures.FindRenderTarget(image, desc);
        // The native image starts with the guest payload. Read it back before any clear.
        textures.MarkGpuWritten(image);
        renderer.PublishGuestWrites();
        std::array<uint32_t, 2048> initial_pixels {};
        pass &= Memory::TryReadBacking(base + 0x4000, initial_pixels.data(), sizeof(initial_pixels));
        for (const auto pixel: initial_pixels) pass &= pixel == 0xa5a5a5a5u;
        std::printf("PS5_GPU_TEST case=guest-color-image-upload result=%s first=%08x last=%08x\n",
                    pass ? "PASS" : "FAIL", initial_pixels.front(), initial_pixels.back());
        pass &= textures.ClearImageFromBuffer(scheduler.Current(), base + 0x4000, 8192,
                                               0xff0000ffu);
        renderer.PublishGuestWrites();
        std::array<uint32_t, 2048> pixels {};
        pass &= Memory::TryReadBacking(base + 0x4000, pixels.data(), sizeof(pixels));
        for (size_t i = 0; i < pixels.size(); ++i)
            pass &= pixels[i] == (i % 64 < 32 ? 0xff0000ffu : 0xa5a5a5a5u);
        pass &= !textures.GetImage(image).IsGpuModified();
        std::printf("PS5_GPU_TEST case=color-image-publication result=%s first=%08x last=%08x\n",
                    pass ? "PASS" : "FAIL", pixels.front(), pixels.back());
        pass &= RectDistanceProbe(renderer, hw, user, image, desc);

        TileSizeAlign tiled_size {};
        desc.info.extent = {128, 128, 1};
        desc.info.tile_mode = Prospero::TileMode::kRenderTarget;
        TileGetTextureTotalSize(desc.info.guest_format, 128, 128, 1, 1,
            desc.info.tile_mode, false, tiled_size);
        desc.info.data = {base + 0x10000, tiled_size.size};
        desc.info.pitch = TileGetTexturePitch(desc.info.guest_format, 128, desc.info.tile_mode);
        desc.info.UpdateSize();
        const auto tiled_image = textures.FindImage(desc);
        (void)textures.FindRenderTarget(tiled_image, desc);
        pass &= textures.ClearImageFromBuffer(scheduler.Current(), desc.info.data.address,
            desc.info.data.size, 0xff0000ffu);
        renderer.PublishGuestWrites();
        std::vector<uint32_t> tiled_pixels(tiled_size.size / 4);
        pass &= Memory::TryReadBacking(desc.info.data.address, tiled_pixels.data(), tiled_size.size);
        for (const auto pixel: tiled_pixels) pass &= pixel == 0xff0000ffu;
        pass &= !textures.GetImage(tiled_image).IsGpuModified();
        std::printf("PS5_GPU_TEST case=tiled-color-publication result=%s bytes=%u first=%08x last=%08x\n",
            pass ? "PASS" : "FAIL", tiled_size.size, tiled_pixels.front(), tiled_pixels.back());

        // Nonuniform multi-block data detects coordinate permutations hidden by a uniform clear.
        constexpr uint32_t pattern_width = 256, pattern_height = 192;
        std::vector<uint32_t> pattern(pattern_width * pattern_height);
        for (uint32_t y = 0; y < pattern_height; ++y)
            for (uint32_t x = 0; x < pattern_width; ++x)
                pattern[y * pattern_width + x] = 0xff000000u | x | (y << 8) | ((x ^ y) << 16);
        desc.info.extent = {pattern_width, pattern_height, 1};
        TileGetTextureTotalSize(desc.info.guest_format, pattern_width, pattern_height, 1, 1,
            desc.info.tile_mode, false, tiled_size);
        desc.info.data = {base + 0x40000, tiled_size.size};
        desc.info.pitch = TileGetTexturePitch(desc.info.guest_format, pattern_width, desc.info.tile_mode);
        desc.info.UpdateSize();
        const auto pattern_image = textures.FindImage(desc);
        (void)textures.FindRenderTarget(pattern_image, desc);
        const auto pattern_bytes = pattern.size() * sizeof(uint32_t);
        Buffer pattern_upload(renderer.GetGraphics(), scheduler, MemoryUsage::Upload, 0,
            vk::BufferUsageFlagBits::eTransferSrc, pattern_bytes);
        std::memcpy(pattern_upload.Mapped().data(), pattern.data(), pattern_bytes);
        pattern_upload.Flush(0, pattern_bytes);
        vk::BufferImageCopy region {};
        region.bufferRowLength = pattern_width;
        region.bufferImageHeight = pattern_height;
        region.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        region.imageExtent = {pattern_width, pattern_height, 1};
        textures.GetImage(pattern_image).Upload(std::span(&region, 1), pattern_upload.Handle(), 0, pattern_bytes);
        textures.MarkGpuWritten(pattern_image);
        renderer.PublishGuestWrites();
        textures.UpdateImage(pattern_image);
        Buffer pattern_readback(renderer.GetGraphics(), scheduler, MemoryUsage::Download, 0,
            vk::BufferUsageFlagBits::eTransferDst, pattern_bytes);
        textures.GetImage(pattern_image).Download(std::span(&region, 1), pattern_readback.Handle(), 0, pattern_bytes);
        scheduler.Finish();
        pattern_readback.Invalidate(0, pattern_bytes);
        const bool pattern_pass = std::memcmp(pattern_readback.Mapped().data(), pattern.data(), pattern_bytes) == 0;
        pass &= pattern_pass;
        std::printf("PS5_GPU_TEST case=nonuniform-tiled-publication-reupload result=%s extent=%ux%u bytes=%u\n",
            pattern_pass ? "PASS" : "FAIL", pattern_width, pattern_height, tiled_size.size);
        pass &= DepthStencilBackingProbe(renderer, base, false);
        pass &= DepthStencilBackingProbe(renderer, base, true);
        pass &= ImageUploadAliasBackingProbe(renderer, base);
        pass &= TypedFragmentExportProbe(renderer, hw, user, base);
        pass &= DepthTiledColorBackingProbe(renderer, base);
        GuestGpuPublicationActive = false;
        pass &= LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) != 0;
    });
    pass &= BufferPublicationWitnessAliasRemapProbe(renderer);
    pass &= HostImportRetileAliasProbe(renderer);
    pass &= HostImportBatchProbe(renderer);
    pass &= HostImportDeferredVmProbe(renderer);
    pass &= ImagePublicationSnapshotProbe(renderer);
    pass &= ImageUploadSnapshotProbe(renderer);
    const auto* delta_only = std::getenv("LUMEN_PS5_TEST_CPU_DELTA_ONLY");
    if (delta_only != nullptr && std::strcmp(delta_only, "1") == 0) {
        pass &= GuestAsyncCpuVisibleExperimentEnabled() &&
            !GuestAsyncCpuConcurrentExperimentEnabled() && AggressiveCpuDeltaProbe(renderer);
    } else {
        pass &= AsyncPrivateBackingProbe(renderer);
        pass &= AsyncNativeBackingDependencyProbe(renderer);
        pass &= AdjacentPhysicalAliasDependencyProbe(renderer);
        pass &= BufferOverlapSnapshotTransferProbe(renderer);
        pass &= AggressiveCpuDeltaProbe(renderer);
        pass &= AsyncPrivateDepthStencilProbe(renderer);
        pass &= AsyncDirectAliasProbe(renderer);
    }
    pass &= Memory::KernelMprotect(memory, memory_size, 0x02) == OK;
    pass &= !Memory::QueryGpuBackingWrite(base, 4);
    pass &= Memory::KernelMunmap(base, memory_size) == OK;
    std::printf("PS5_GPU_TEST case=write-rights-retirement result=%s\n", pass ? "PASS" : "FAIL");
    renderer.ShutdownGpu();
    WindowShutdown();
    std::printf("PS5 GPU buffer/image completed backing publication: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}
} // namespace Loader::GuestExecution
#endif
