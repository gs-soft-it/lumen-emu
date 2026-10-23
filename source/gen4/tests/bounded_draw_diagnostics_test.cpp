// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/bounded_draw_diagnostics.h"
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <thread>
using namespace Vulkan::DrawDiagnostic;
int main() {
    Collector collector;
    assert(!collector.Arm());
    assert(!collector.TakeFlip(0x1000));
    collector.Configure(true);
    assert(collector.Arm());
    assert(!collector.Arm());
    // Count ends independently from executed scans: the guarded draw retains
    // End/readback but can avoid the full snapshot comparison.
    constexpr PublicationContext draw{PublicationSite::Graphics, 0x2d};
    constexpr PublicationContext nop{PublicationSite::Graphics, 0x10, NopKind::Marker};
    constexpr PublicationContext flip_nop{PublicationSite::Graphics, 0x10, NopKind::PatchedFlip};
    constexpr PublicationContext begin{PublicationSite::OuterBegin};
    assert(ClassifyNopPayload(0x68750776) == NopKind::PatchedFlip);
    assert(ClassifyNopPayload(0x68750001) == NopKind::Marker);
    assert(ClassifyNopPayload(0x12345678) == NopKind::Other);
    collector.Publication(draw, false);
    collector.Publication(nop, false);
    collector.Publication(nop, true, 32);
    collector.Publication(flip_nop, false);
    collector.Publication(flip_nop, true, 48);
    collector.Publication(begin, true, 16);
    assert(publication_context.site == PublicationSite::OuterCommand);
    {
        PublicationScope task{{PublicationSite::OuterTaskDone}, true};
        {
            const PublicationScope packet{{PublicationSite::Compute, 0x15}, true};
            collector.Publication(publication_context, false);
            collector.Publication(publication_context, true, 4);
        }
        assert(publication_context.site == PublicationSite::OuterTaskDone);
        task.SetSite(PublicationSite::OuterTaskYield);
        collector.Publication(publication_context, false);
        collector.Publication(publication_context, true, 8);
    }
    assert(publication_context.site == PublicationSite::OuterCommand);
    {
        const PublicationScope disabled{{PublicationSite::Graphics, 0xff}, false};
        assert(publication_context.site == PublicationSite::OuterCommand);
    }
    Targets targets{};
    targets[0] = {0x1000, 15, true};
    targets[1] = {0x2000, 0, true};
    collector.Entry(false, false, 3, 1, 4, 1, 15, targets);
    auto bound = targets;
    bound[1].active = false;
    collector.Pipeline(0x11, 0x94720a3237573b18, 0x22);
    collector.Result(Outcome::Emitted, bound);
    collector.Publication({PublicationSite::Graphics, 0x27}, false);
    collector.Entry(true, true, 0, 0, 4, 1, 15, targets);
    collector.Result(Outcome::PipelineNull);
    auto first = collector.TakeFlip(0x1000);
    assert(first && first->flip == 1 && first->entries == 2);
    assert(first->publications[PublicationIndex(draw)].ends == 1);
    assert(first->publications[PublicationIndex(draw)].scans == 0);
    const auto& nop_counts = first->publications[PublicationIndex(nop)];
    assert(nop_counts.ends == 2 && nop_counts.scans == 2 && nop_counts.watched_pages == 80);
    assert(first->graphics_nops[static_cast<unsigned>(NopKind::Marker)].watched_pages == 32);
    assert(first->graphics_nops[static_cast<unsigned>(NopKind::PatchedFlip)].watched_pages == 48);
    assert(first->publications[PublicationIndex(begin)].ends == 0);
    assert(first->publications[PublicationIndex(begin)].scans == 1);
    const auto& yield = first->publications[PublicationIndex({PublicationSite::OuterTaskYield})];
    assert(yield.ends == 1 && yield.scans == 1 && yield.watched_pages == 8);
    const auto& compute = first->publications[PublicationIndex({PublicationSite::Compute, 0x15})];
    assert(compute.ends == 1 && compute.scans == 1 && compute.watched_pages == 4);
    assert(first->indexed == 1 && first->indirect == 1);
    assert(first->zero_count == 0 && first->zero_instances == 0);
    assert(first->outcomes[static_cast<unsigned>(Outcome::Emitted)] == 1);
    assert(first->outcomes[static_cast<unsigned>(Outcome::PipelineNull)] == 1);
    assert(first->targets[0].address == first->scanout && first->targets[0].emitted == 1);
    assert(first->targets[1].emitted == 0);
    assert(first->pipeline_count == 1 && first->pipeline_overflow == 0);
    assert(first->pipelines[0].id == 0x11 && first->pipelines[0].vs == 0x94720a3237573b18);
    assert(first->pipelines[0].entries == 1 && first->pipelines[0].emitted == 1);
    const auto& draw_pipeline = first->publications[PublicationIndex({PublicationSite::Graphics, 0x27})];
    assert(draw_pipeline.last_pipeline == 0x11 && draw_pipeline.last_primitive == 4);
    // Recording from a worker and arm/flip control are synchronized native state.
    std::thread worker([&] {
        for (unsigned i = 0; i < 20; ++i) {
            targets[0].address = 0x3000 + i * 0x1000;
            collector.Entry(false, false, 0, 0, 0, 0, 0, targets);
            collector.Pipeline(0x100 + i, 0x200 + i, 0x300 + i);
            collector.Result(Outcome::PrimitiveNone);
        }
    });
    worker.join();
    const auto second = collector.TakeFlip(0x9000);
    assert(second && second->flip == 2 && second->entries == 20);
    for (const auto& p : second->publications)
        assert(p.ends == 0 && p.scans == 0 && p.watched_pages == 0);
    for (const auto& p : second->graphics_nops)
        assert(p.ends == 0 && p.scans == 0 && p.watched_pages == 0);
    assert(second->pipeline_count == MaxPipelines && second->pipeline_overflow == 4);
    assert(second->zero_count == 20 && second->zero_instances == 20);
    assert(second->target_count == MaxTargets && second->target_overflow > 0);
    for (unsigned flip = 3; flip <= MaxFlips; ++flip) {
        const auto frame = collector.TakeFlip(0x1000);
        assert(frame && frame->flip == flip && frame->entries == 0);
    }
    assert(!collector.Active() && !collector.Arm());
    collector.Configure(true);
    assert(!collector.Arm());
    collector.Entry(false, false, 3, 1, 4, 1, 15, targets);
    collector.Result(Outcome::Emitted, targets);
    assert(!collector.TakeFlip(0x1000));
    std::puts("BoundedDrawDiagnostics: PASS default-off once-only 8-flip-cap outcomes targets overflow worker publication-opcodes scans pages NOP-subsets outer-yield context-restore");
}
