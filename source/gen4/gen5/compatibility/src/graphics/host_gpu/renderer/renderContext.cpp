#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/gpuSliceTrace.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/renderTiming.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/guestBackingPolicy.h"
#include "graphics/presentation/videoOut.h"
#include "libs/errno.h"
#include "kernel/memory.h"

#include <algorithm>
#include <cstdio>

namespace Libs::Graphics {

namespace {
void TraceAsyncPrivateDisposition(unsigned disposition) {
    static const bool enabled = [] {
        const auto* value = std::getenv("LUMEN_PS5_GFX_ASYNC_STATS");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    if (!enabled || !GuestAsyncPrivateBackingEnabled()) return;
    static thread_local uint64_t full = 0, deferred = 0, selective = 0, cpu_visible_experiment = 0;
    if (disposition == 0) ++full;
    else if (disposition == 1) ++deferred;
    else if (disposition == 2) ++selective;
    else ++cpu_visible_experiment;
    const auto total = full + deferred + selective + cpu_visible_experiment;
    if (total <= 16 || total % 120 == 0)
        std::fprintf(stderr, "PS5_GPU_ASYNC_PRIVATE_STATS full=%llu deferred=%llu selective=%llu cpu_visible_experiment=%llu\n",
            static_cast<unsigned long long>(full), static_cast<unsigned long long>(deferred),
            static_cast<unsigned long long>(selective),
            static_cast<unsigned long long>(cpu_visible_experiment));
}
} // namespace


RenderContext::RenderContext(GraphicContext& graphics)
    : m_graphics(graphics), m_render_executor(*this), m_command_scheduler(*this, graphics),
      m_descriptor_heap(graphics, m_command_scheduler.GetMasterSemaphore()),
      m_pipeline_cache(graphics), m_sampler_cache(graphics),
      m_buffer_cache(graphics, m_command_scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, m_command_scheduler, m_page_manager, m_buffer_cache) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
}

RenderContext::~RenderContext() {
	ShutdownGpu();
	m_command_scheduler.Shutdown();
}

void RenderContext::InitializeGpu(VideoOut::VideoOutDriver* video_out) {
	EXIT_IF(m_gpu != nullptr);
	if (!GuestCpuGpuConcurrencyCanStart(GuestAsyncCpuConcurrentExperimentRequested()))
		EXIT("PS5 CPU/GPU concurrency admission denied: complete translated JIT/HLE access coverage and coherent physical-alias handoff are unavailable; a native data lease only pins VM lifetime\n");
	m_video_out = video_out;
	m_gpu       = std::make_unique<GuestGpu>(*this);
}

void RenderContext::ShutdownGpu() {
	if (m_gpu != nullptr) {
		m_gpu->Shutdown();
		m_gpu.reset();
	}
	if (m_video_out != nullptr) {
		if (m_command_scheduler.Active()) {
			m_command_scheduler.Finish();
		}
		m_command_scheduler.DrainPriorityOperations();
		m_video_out = nullptr;
	}
}

GuestGpu& RenderContext::GetGpu() const {
	EXIT_IF(m_gpu == nullptr);
	return *m_gpu;
}

VideoOut::VideoOutDriver& RenderContext::GetVideoOut() const {
	EXIT_IF(m_video_out == nullptr);
	return *m_video_out;
}

bool RenderContext::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	// The host reports the faulting byte, not the instruction's access width. Both caches
	// resolve its page; guessing a width can cross the end of a valid guest mapping.
	constexpr uint64_t fault_size = 1;
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}
	using Origin = Common::RenderTiming::ReadbackOrigin;
	Common::RenderTiming::ScopedReadbackOrigin origin(
	    access == PageFaultAccess::Write ? Origin::GuestWriteFault : Origin::GuestReadFault);
	if (access == PageFaultAccess::Write) {
		m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
	} else {
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
	}
	return true;
}

bool RenderContext::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	return true;
}

bool RenderContext::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

bool RenderContext::HasMappedMemoryOverlap(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) return false;
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Intersects(vaddr, size);
}

void RenderContext::MapMemory(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mapped_ranges_mutex);
	m_mapped_ranges.Add(vaddr, size);
}

void RenderContext::UnmapMemory(uint64_t vaddr, uint64_t size) {
	{
		std::shared_lock lock(m_mapped_ranges_mutex);
		// Fresh VM allocations have no GPU mapping to retire. Avoid queuing a
		// synchronous GPU drain while their CPU VM owner holds the FEX epoch.
		// A partially mapped range still takes the complete invalidation path.
		if (!m_mapped_ranges.Intersects(vaddr, size)) return;
	}
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto unmap = [this, vaddr, size] {
		// Check cache ownership on the GPU thread. Guest-memory callbacks can still
		// access a range with no cached data, so they must finish before it is unmapped.
		if (m_command_scheduler.Active() &&
		    (m_buffer_cache.IsRegionRegistered(vaddr, size) ||
		     m_texture_cache.IsRegionRegistered(vaddr, size) ||
		     m_command_scheduler.HasPendingPriorityOperations())) {
			const auto tick = m_command_scheduler.CurrentTick();
			m_command_scheduler.Finish();
			m_command_scheduler.WaitPriorityOperations(tick);
		}
		m_buffer_cache.InvalidateMemory(vaddr, size);
		if (BufferPublicationWitnessEnabled())
			m_buffer_cache.RetireBackingSnapshots(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
	};
	// Shutdown still owns the GPU while queued rendering drains, but its command lane no
	// longer accepts external work. Use the guest GPU's state for the teardown route.
	if (m_gpu == nullptr || m_gpu->IsStopping()) {
		// Stopping is set before join. It is not by itself authority for a
		// foreign thread to mutate the worker's snapshot map. After join the
		// normal ShutdownGpu path resets m_gpu; worker-local retirement is safe.
		if (BufferPublicationWitnessEnabled() && m_gpu != nullptr && !GuestGpu::IsGpuThread())
			EXIT("PS5 backing witness retirement denied during unjoined GPU shutdown\n");
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
}

void RenderContext::PrepareBda() {
	m_buffer_cache.ClearUniformMetadata();
	if (GuestGpuWriteOwnershipSupported())
		EXIT("PS5 BDA access has no verified per-range GPU-write authority\n");
	if (!m_bda_logged) {
		Log::WriteToConsoleAndLog("GPU: using buffer device address (BDA) shader memory access.\n");
		m_bda_logged = true;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	m_mapped_ranges.ForEach([this](uint64_t start, uint64_t end) {
		m_buffer_cache.SynchronizeBuffersInRange(start, end - start);
	});
	m_fault_process_pending = true;
}

void RenderContext::RunGarbageCollector() {
	// The exclusive guest epoch retains surviving GPU owners across GC. Dirty
	// owners selected for retirement publish locally; the submission's final
	// publication drains surviving owners after BufferFlush, before CPU admission.
	if (GuestGpuWriteOwnershipSupported() && !GuestGcPublicationDeferred()) PublishGuestWrites();
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
}

void RenderContext::PublishGuestWrites() {
	if (!GuestGpuWriteOwnershipSupported()) return;
    TraceAsyncPrivateDisposition(0);
	// Finish native producers and all earlier completion callbacks first.
	const auto tick = m_command_scheduler.CurrentTick();
	m_command_scheduler.Finish();
	m_command_scheduler.WaitPriorityOperations(tick);
	m_buffer_cache.PublishGuestWrites();
	m_texture_cache.PublishGuestWrites();
	const auto publication_tick = m_command_scheduler.CurrentTick();
	m_command_scheduler.Finish();
	m_command_scheduler.WaitPriorityOperations(publication_tick);
}

std::vector<GuestRange> RenderContext::GuestWriteDependencyClosure(
    std::span<const GuestRange> dependencies) {
    std::vector<GuestRange> ranges(dependencies.begin(), dependencies.end());
    const auto normalize = [](std::vector<GuestRange>& values) {
        std::vector<std::pair<uint64_t, int64_t>> edges;
        for (const auto& range: values) {
            if (!range.Valid()) EXIT("PS5 backing dependency has invalid guest range\n");
            edges.emplace_back(range.address, 1);
            edges.emplace_back(range.End(), -1);
        }
        std::sort(edges.begin(), edges.end());
        values.clear();
        int64_t coverage = 0;
        uint64_t previous = 0;
        for (size_t edge = 0; edge < edges.size();) {
            const auto address = edges[edge].first;
            if (coverage != 0 && previous < address)
                values.push_back({previous, address - previous});
            do { coverage += edges[edge++].second; }
            while (edge < edges.size() && edges[edge].first == address);
            previous = address;
        }
        // Preserve every input endpoint, including adjacent physical aliases.
        // Joining them would invent a single linear consumer containing the
        // same physical bytes twice, which the VM provenance query rejects.
        if (coverage != 0) EXIT("PS5 backing dependency has unbalanced ranges\n");
    };
    normalize(ranges);
    // Publication widens a selected buffer to its complete dirty owner and an
    // image to its whole depth/stencil family. Repeat alias projection after
    // each widening, including aliases of bytes outside the original access.
    // No cache or VM mutex survives an iteration or a scheduler wait.
    for (unsigned iteration = 0; iteration != 1024; ++iteration) {
        auto expanded = ranges;
        m_buffer_cache.CollectGuestWriteDependencies(ranges, expanded);
        m_texture_cache.CollectGuestWriteDependencies(ranges, expanded);
        normalize(expanded);
        const auto owner_ranges = expanded;
        for (const auto& range: owner_ranges) {
            std::vector<LibKernel::Memory::GuestBackingAlias> aliases;
            if (!LibKernel::Memory::QueryGuestBackingAliases(range.address, range.size, &aliases))
                EXIT("PS5 backing dependency has incomplete physical alias identity\n");
            for (const auto& alias: aliases) expanded.push_back({alias.address, alias.size});
        }
        normalize(expanded);
        if (expanded == ranges) return expanded;
        if (expanded.size() > 65536)
            EXIT("PS5 backing dependency exceeds the bounded physical alias plan\n");
        ranges = std::move(expanded);
    }
    EXIT("PS5 backing dependency physical alias closure did not converge\n");
}

void RenderContext::PublishGuestWriteDependencies(std::span<const GuestRange> dependencies) {
    EXIT_IF(!GuestGpuWriteOwnershipSupported());
    if (dependencies.empty()) return;
    bool dirty = false;
    for (const auto& dependency: dependencies) {
        if (!dependency.Valid()) EXIT("PS5 backing dependency has invalid guest range\n");
        if (m_buffer_cache.IsRegionGpuModified(dependency.address, dependency.size) ||
            m_texture_cache.HasGuestWriteDependency(dependency.address, dependency.size)) {
            dirty = true;
            break;
        }
    }
    // Keep clean alias reads on the indexed page/cache query path. Enumerate
    // native owners only after a real page/family dependency was found.
    if (!dirty) return;
    std::vector<GuestRange> owners;
    m_buffer_cache.CollectGuestWriteDependencies(dependencies, owners);
    m_texture_cache.CollectGuestWriteDependencies(dependencies, owners);
    if (owners.empty()) return;
    // The seed/padding still depends on all preceding priority callbacks. A
    // narrower publication must not turn producer completion into a no-op.
    const auto producer_tick = m_command_scheduler.CurrentTick();
    m_command_scheduler.Finish();
    m_command_scheduler.WaitPriorityOperations(producer_tick);
    const auto closure = GuestWriteDependencyClosure(dependencies);
    m_buffer_cache.PublishGuestWritesInRanges(closure);
    m_texture_cache.PublishGuestWritesInRanges(closure);
    const auto publication_tick = m_command_scheduler.CurrentTick();
    m_command_scheduler.Finish();
    m_command_scheduler.WaitPriorityOperations(publication_tick);
    owners.clear();
    m_buffer_cache.CollectGuestWriteDependencies(closure, owners);
    m_texture_cache.CollectGuestWriteDependencies(closure, owners);
    if (!owners.empty()) EXIT("PS5 backing dependency retained an unpublished owner\n");
}

void RenderContext::PublishGuestWriteDependency(uint64_t vaddr, uint64_t size) {
    if (!GuestRange {vaddr, size}.Valid()) EXIT("PS5 native backing dependency has invalid range\n");
    std::vector<LibKernel::Memory::GuestBackingAlias> aliases;
    if (!LibKernel::Memory::QueryGuestBackingAliases(vaddr, size, &aliases))
        EXIT("PS5 native backing dependency has no complete physical backing identity\n");
    std::vector<GuestRange> dependencies;
    for (const auto& alias: aliases) dependencies.push_back({alias.address, alias.size});
    PublishGuestWriteDependencies(dependencies);
}

void RenderContext::FinishGuestGpuSlice(bool allow_async) {
    if (!GuestGpuWriteOwnershipSupported()) return;
    if (GpuSliceTrace::current != nullptr) {
        // Diagnostic eligibility for the existing strict-private Flush path,
        // not authorization to enable retention or translated CPU concurrency.
        GpuSliceTrace::current->private_eligible = allow_async &&
            !m_command_scheduler.HasPendingOperations() &&
            !m_buffer_cache.HasCpuVisibleGpuWrites() &&
            !m_texture_cache.HasCpuVisibleGpuWrites() ? 1 : 0;
    }
    if (!allow_async || !GuestAsyncPrivateBackingEnabled()) {
        PublishGuestWrites();
        return;
    }
    if (GuestAsyncCpuVisibleExperimentEnabled()) {
        TraceAsyncPrivateDisposition(3);
        // Notifications still complete before CPU readmission. GPU results stay
        // in their native owners even when a CPU alias can observe old backing;
        // explicit consumers and VM retirement remain publication boundaries.
        if (m_command_scheduler.HasPendingOperations()) {
            const auto tick = m_command_scheduler.CurrentTick();
            m_command_scheduler.Finish();
            m_command_scheduler.WaitPriorityOperations(tick);
        } else {
            m_command_scheduler.Flush();
        }
        return;
    }
    // All callback producers are serialized on this worker. No PM4 yield or
    // command-lane service can occur between this proof and the submission.
    if (!m_command_scheduler.HasPendingOperations() &&
        !m_buffer_cache.HasCpuVisibleGpuWrites() &&
        !m_texture_cache.HasCpuVisibleGpuWrites()) {
        TraceAsyncPrivateDisposition(1);
        m_command_scheduler.Flush();
        return;
    }
    TraceAsyncPrivateDisposition(2);
    const auto tick = m_command_scheduler.CurrentTick();
    m_command_scheduler.Finish();
    m_command_scheduler.WaitPriorityOperations(tick);
    m_buffer_cache.PublishGuestWrites(true);
    m_texture_cache.PublishGuestWrites(true);
    const auto publication_tick = m_command_scheduler.CurrentTick();
    m_command_scheduler.Finish();
    m_command_scheduler.WaitPriorityOperations(publication_tick);
    if (m_buffer_cache.HasCpuVisibleGpuWrites() || m_texture_cache.HasCpuVisibleGpuWrites())
        EXIT("PS5 async publication retained CPU-visible GPU backing\n");
}

void RenderContext::PublishRetainedGuestGpuWritesBeforeVmMutation() {
    if (!GuestAsyncPrivateBackingEnabled()) return;
    // The caller already owns the CPU-exclusive FEX mutation, but has acquired
    // no VM mutex. The worker may publish in that authority without attempting
    // a nested epoch or competing with a GPU intent waiter.
    const auto publish = [this] {
        EXIT_IF(GuestGpuPublicationActive);
        if (!m_command_scheduler.Active()) return;
        GuestGpuPublicationActive = true;
        PublishGuestWrites();
        GuestGpuPublicationActive = false;
    };
    // The worker's shutdown slice drains before m_gpu is released. A stopped
    // lane cannot be replaced with a concurrent CPU cache publisher.
    if (m_gpu == nullptr) return;
    if (m_gpu->IsStopping())
        EXIT("PS5 VM mutation attempted during GPU ownership shutdown\n");
    m_gpu->SendCommandSync(publish);
}

void RenderContext::PublishRetainedGuestGpuWritesBeforeCpuDataWrite(uint64_t vaddr, uint64_t size) {
    if (!GuestAsyncCpuVisibleExperimentEnabled() ||
        !GuestRange {vaddr, size}.Valid()) return;
    // The CPU owns the exclusive FEX epoch and has not acquired the VM mutex.
    // Borrow no native cache state on that CPU thread; the sole worker proves
    // the complete destination family and publishes only on a real conflict.
    const auto publish = [this, vaddr, size] {
        EXIT_IF(GuestGpuPublicationActive);
        if (!m_command_scheduler.Active()) return;
        if (!IsMapped(vaddr, size)) {
            if (HasMappedMemoryOverlap(vaddr, size))
                EXIT("PS5 experimental CPU output partially overlaps GPU ownership\n");
            return;
        }
        if (!LibKernel::Memory::QueryGuestAccess(vaddr, size, Common::VirtualMemory::Mode::Write))
            EXIT("PS5 experimental CPU output lost its complete write rights\n");
        GuestGpuPublicationActive = true;
        PublishGuestWriteDependency(vaddr, size);
        GuestGpuPublicationActive = false;
    };
    if (m_gpu == nullptr) return;
    if (m_gpu->IsStopping())
        EXIT("PS5 experimental CPU output attempted during GPU ownership shutdown\n");
    m_gpu->SendCommandSync(publish);
}

void RenderContext::PublishPhysicalAliasDependencies(uint64_t vaddr, uint64_t size) {
    if (!GuestGpuWriteOwnershipSupported() || size == 0)
        return;
    std::vector<LibKernel::Memory::GuestBackingAlias> aliases;
    if (!LibKernel::Memory::QueryGuestBackingAliases(vaddr, size, &aliases))
        EXIT("PS5 async GPU consumer has no complete physical backing identity\n");
    std::vector<GuestRange> dependencies;
    for (const auto& alias: aliases)
        if (!alias.source) dependencies.push_back({alias.address, alias.size});
    // A cache consumer can directly reuse its source VA's native owner. Only
    // another VA demands backing publication before it acquires its cache ID.
    PublishGuestWriteDependencies(dependencies);
}

void RenderContext::InvalidatePhysicalAliases(uint64_t vaddr, uint64_t size,
                                             bool include_source) {
    std::vector<LibKernel::Memory::GuestBackingAlias> aliases;
    if (!LibKernel::Memory::QueryGuestBackingAliases(vaddr, size, &aliases))
        EXIT("PS5 GPU publication lost its physical backing identity\n");
    for (const auto& alias: aliases) {
        if (!include_source && alias.source) continue;
        m_buffer_cache.InvalidateMemory(alias.address, alias.size);
        m_texture_cache.InvalidateMemory(alias.address, alias.size);
    }
}

void RenderContext::AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it != m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.push_back({eq, event_id});
}

void RenderContext::DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it == m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.erase(it);
}

void RenderContext::TriggerInterrupt(int event_id, uint32_t context_id) {
	std::vector<InterruptEqRegistration> registrations;
	{
		Common::LockGuard lock(m_interrupt_mutex);
		for (const auto& registration: m_interrupt_eqs) {
			if (registration.event_id == event_id) {
				registrations.push_back(registration);
			}
		}
	}

	for (const auto& registration: registrations) {
		const auto result = LibKernel::EventQueue::KernelTriggerEvent(
		    registration.eq, static_cast<uintptr_t>(registration.event_id),
		    LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS,
		    reinterpret_cast<void*>(static_cast<uintptr_t>(context_id)));
		if (result == LibKernel::KERNEL_ERROR_EBADF || result == LibKernel::KERNEL_ERROR_ENOENT) {
			DeleteInterruptEq(registration.eq, registration.event_id);
			continue;
		}
		EXIT_NOT_IMPLEMENTED(result != OK);
	}
}

} // namespace Libs::Graphics
