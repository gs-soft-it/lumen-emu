// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

namespace Vulkan::DrawDiagnostic {

// One manually requested capture window. No timers, sampling, guest memory
// reads, or changes to command submission. All records stop after eight flips.
inline constexpr unsigned MaxFlips = 8;
inline constexpr unsigned MaxTargets = 8;
enum class Outcome : unsigned {
    FastClear, FmaskDecompress, Resolve, PrimitiveNone, DepthStencilCopy,
    PipelineNull, BindResourcesFailed, Emitted, Count
};
struct TargetInput {
    std::uint64_t address{};
    std::uint32_t component_mask{};
    bool active{};
};
using Targets = std::array<TargetInput, 8>;
struct TargetRecord {
    std::uint64_t address{}, entries{}, emitted{};
    std::uint32_t slots{}, component_mask{};
};
enum class PublicationSite : unsigned {
    Graphics, Constant, Compute, OuterCommand, OuterTaskDone, OuterTaskYield, OuterBegin, Count
};
enum class NopKind : unsigned { Empty, Marker, Other, PatchedFlip, Count };
inline constexpr unsigned PacketSites = 3;
inline constexpr unsigned OpcodeCount = 256;
inline constexpr unsigned PublicationSlots = PacketSites * OpcodeCount +
    static_cast<unsigned>(PublicationSite::Count) - PacketSites;
struct PublicationContext {
    PublicationSite site{PublicationSite::OuterCommand};
    std::uint8_t opcode{};
    NopKind nop_kind{NopKind::Other};
};
struct PublicationRecord {
    std::uint64_t ends{}, scans{}, watched_pages{};
    std::uint64_t last_pipeline{}, last_vs{}, last_ps{};
    std::uint32_t last_primitive{};
};
constexpr unsigned PublicationIndex(PublicationContext context) {
    const auto site = static_cast<unsigned>(context.site);
    return site < PacketSites ? site * OpcodeCount + context.opcode
                              : PacketSites * OpcodeCount + site - PacketSites;
}
constexpr NopKind ClassifyNopPayload(std::uint32_t payload) {
    switch (payload) {
    case 0x68750776u: return NopKind::PatchedFlip;
    case 0x68750001u:
    case 0x68750002u:
    case 0x6875000eu: return NopKind::Marker;
    default: return NopKind::Other;
    }
}
inline constexpr std::array PublicationSiteNames{
    "gfx", "ce", "compute", "outer_command", "outer_task_done", "outer_task_yield", "outer_begin"};
inline constexpr std::array NopKindNames{"empty", "marker", "other", "patched_flip"};
inline constexpr unsigned MaxPipelines = 16;
struct PipelineRecord {
    std::uint64_t id{}, vs{}, ps{}, entries{}, emitted{};
    std::uint32_t primitive{};
};
struct Snapshot {
    unsigned flip{};
    std::array<PublicationRecord, PublicationSlots> publications{};
    // Subset of the Graphics/NOP row; never add these twice to total scans.
    std::array<PublicationRecord, static_cast<unsigned>(NopKind::Count)> graphics_nops{};
    std::array<PipelineRecord, MaxPipelines> pipelines{};
    unsigned pipeline_count{};
    std::uint64_t pipeline_overflow{}, last_pipeline{}, last_vs{}, last_ps{};
    std::uint64_t scanout{}, entries{}, indexed{}, indirect{}, zero_count{}, zero_instances{};
    std::array<std::uint64_t, static_cast<unsigned>(Outcome::Count)> outcomes{};
    std::array<TargetRecord, MaxTargets> targets{};
    unsigned target_count{};
    std::uint64_t target_overflow{};
    std::uint32_t last_primitive{}, last_color_mode{}, last_target_mask{};
};

class Collector final {
public:
    void Configure(bool enabled_) {
        std::lock_guard lock{mutex};
        enabled = enabled_;
    }
    bool Arm() {
        std::lock_guard lock{mutex};
        if (!enabled || armed) return false;
        armed = true;
        active.store(true, std::memory_order_release);
        return true;
    }
    bool Active() const { return active.load(std::memory_order_acquire); }
    void Entry(bool is_indexed, bool is_indirect, std::uint32_t count,
               std::uint32_t instances, std::uint32_t primitive,
               std::uint32_t color_mode, std::uint32_t target_mask,
               const Targets& targets) {
        if (!Active()) return;
        std::lock_guard lock{mutex};
        if (!Active()) return;
        ++current.entries;
        current.last_pipeline = current.last_vs = current.last_ps = 0;
        current.indexed += is_indexed;
        current.indirect += is_indirect;
        // Indirect counts are GPU input, deliberately never dereferenced here.
        current.zero_count += !is_indirect && count == 0;
        current.zero_instances += !is_indirect && instances == 0;
        current.last_primitive = primitive;
        current.last_color_mode = color_mode;
        current.last_target_mask = target_mask;
        RecordTargets(targets, false);
    }
    void Pipeline(std::uint64_t id, std::uint64_t vs, std::uint64_t ps) {
        if (!Active()) return;
        std::lock_guard lock{mutex};
        if (!Active()) return;
        current.last_pipeline = id;
        current.last_vs = vs;
        current.last_ps = ps;
        unsigned index{};
        while (index < current.pipeline_count &&
               (current.pipelines[index].id != id ||
                current.pipelines[index].primitive != current.last_primitive)) ++index;
        if (index == current.pipeline_count) {
            if (current.pipeline_count == MaxPipelines) { ++current.pipeline_overflow; return; }
            ++current.pipeline_count;
            current.pipelines[index] = {id, vs, ps, 0, 0, current.last_primitive};
        }
        ++current.pipelines[index].entries;
    }
    void Result(Outcome outcome, const Targets& targets = {}) {
        if (!Active()) return;
        std::lock_guard lock{mutex};
        if (!Active()) return;
        ++current.outcomes[static_cast<unsigned>(outcome)];
        if (outcome == Outcome::Emitted) {
            RecordTargets(targets, true);
            for (unsigned i = 0; i < current.pipeline_count; ++i) {
                auto& pipeline = current.pipelines[i];
                if (pipeline.id == current.last_pipeline &&
                    pipeline.primitive == current.last_primitive) { ++pipeline.emitted; break; }
            }
        }
    }
    void Publication(PublicationContext context, bool scan, std::uint64_t watched_pages = 0) {
        if (!Active()) return;
        std::lock_guard lock{mutex};
        if (!Active()) return;
        const auto update = [&](PublicationRecord& record) {
            record.ends += !scan;
            record.scans += scan;
            if (scan) record.watched_pages += watched_pages;
            record.last_pipeline = current.last_pipeline;
            record.last_vs = current.last_vs;
            record.last_ps = current.last_ps;
            record.last_primitive = current.last_primitive;
        };
        update(current.publications[PublicationIndex(context)]);
        if (context.site == PublicationSite::Graphics && context.opcode == 0x10)
            update(current.graphics_nops[static_cast<unsigned>(context.nop_kind)]);
    }
    std::optional<Snapshot> TakeFlip(std::uint64_t scanout) {
        if (!Active()) return std::nullopt;
        std::lock_guard lock{mutex};
        if (!Active()) return std::nullopt;
        current.flip = ++completed;
        current.scanout = scanout;
        auto result = current;
        current = {};
        if (completed == MaxFlips) active.store(false, std::memory_order_release);
        return result;
    }
private:
    void RecordTargets(const Targets& targets, bool emitted) {
        for (unsigned slot = 0; slot < targets.size(); ++slot) {
            const auto& input = targets[slot];
            if (!input.active || input.address == 0) continue;
            unsigned index{};
            while (index < current.target_count &&
                   current.targets[index].address != input.address) ++index;
            if (index == current.target_count) {
                if (current.target_count == MaxTargets) {
                    ++current.target_overflow;
                    continue;
                }
                ++current.target_count;
                current.targets[index].address = input.address;
            }
            auto& target = current.targets[index];
            target.slots |= 1U << slot;
            target.component_mask |= input.component_mask;
            if (emitted) ++target.emitted;
            else ++target.entries;
        }
    }
    std::mutex mutex;
    std::atomic_bool active{};
    bool enabled{}, armed{};
    unsigned completed{};
    Snapshot current{};
};

inline Collector& Session() {
    static Collector collector;
    return collector;
}
inline bool Active() {
#ifdef __ANDROID__
    return Session().Active();
#else
    return false;
#endif
}
inline thread_local PublicationContext publication_context{};
// Scopes surround synchronous publication calls or one task.resume() invocation,
// never a coroutine-local region that could suspend across another task.
class PublicationScope final {
public:
    explicit PublicationScope(PublicationContext context, bool enabled = Active())
        : enabled{enabled} {
        if (!enabled) return;
        previous = publication_context;
        publication_context = context;
    }
    ~PublicationScope() { if (enabled) publication_context = previous; }
    PublicationScope(const PublicationScope&) = delete;
    PublicationScope& operator=(const PublicationScope&) = delete;
    void SetSite(PublicationSite site) { if (enabled) publication_context.site = site; }
private:
    bool enabled;
    PublicationContext previous{};
};
inline void PublicationEnd() {
#ifdef __ANDROID__
    Session().Publication(publication_context, false);
#endif
}
inline void PublicationScan(std::uint64_t watched_pages) {
#ifdef __ANDROID__
    Session().Publication(publication_context, true, watched_pages);
#endif
}
inline void Pipeline(std::uint64_t id, std::uint64_t vs, std::uint64_t ps) {
#ifdef __ANDROID__
    Session().Pipeline(id, vs, ps);
#endif
}
inline void Configure(bool enabled) {
#ifdef __ANDROID__
    Session().Configure(enabled);
#endif
}
inline bool Arm() {
#ifdef __ANDROID__
    return Session().Arm();
#else
    return false;
#endif
}
inline void Result(Outcome outcome) {
#ifdef __ANDROID__
    Session().Result(outcome);
#endif
}
template <typename Registers>
inline void Entry(const Registers& regs, bool indexed, bool indirect) {
#ifdef __ANDROID__
    if (!Session().Active()) return;
    Targets targets{};
    for (unsigned slot = 0; slot < targets.size(); ++slot) {
        const auto& cb = regs.color_buffers[slot];
        targets[slot] = {cb.Address(), regs.color_target_mask.GetMask(slot), bool(cb)};
    }
    Session().Entry(indexed, indirect, regs.num_indices, regs.num_instances.NumInstances(),
                    static_cast<std::uint32_t>(regs.primitive_type),
                    static_cast<std::uint32_t>(regs.color_control.mode),
                    regs.color_target_mask.raw, targets);
#endif
}
template <typename Registers, typename RenderState>
inline void Emitted(const Registers& regs, const RenderState& state) {
#ifdef __ANDROID__
    if (!Session().Active()) return;
    Targets targets{};
    for (unsigned slot = 0; slot < targets.size(); ++slot) {
        const auto& cb = regs.color_buffers[slot];
        targets[slot] = {cb.Address(), regs.color_target_mask.GetMask(slot),
                         bool(state.color_attachments[slot].image_view)};
    }
    // This counts recorded Vulkan draw commands, not nonblack pixels or
    // indirect draw invocations whose count is resolved later by the GPU.
    Session().Result(Outcome::Emitted, targets);
#endif
}
inline std::optional<Snapshot> TakeFlip(std::uint64_t scanout) {
#ifdef __ANDROID__
    return Session().TakeFlip(scanout);
#else
    return std::nullopt;
#endif
}
} // namespace Vulkan::DrawDiagnostic
