#include "graphics/guest_gpu/graphicsRun.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/gpuSliceTrace.h"
#include "graphics/host_gpu/renderer/sync.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"
#include "libs/agc.h"
#include "libs/errno.h"
#include "loader/guestHostWait.h"
#include "loader/guestWriterGate.h"
#include "FexPerformanceTrace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <semaphore>
#include <thread>
#include <vector>

namespace Libs::Graphics {

static thread_local CommandProcessor* g_current_processor = nullptr;
static thread_local Pm4Execution*     g_current_execution = nullptr;
static thread_local bool              g_gpu_thread        = false;
static thread_local GuestGpu*         g_gpu_state         = nullptr;

void PublishNativeGuestBackingRead(uint64_t address, uint64_t size, bool require_cpu_readable) {
    if (!GuestAsyncPrivateBackingEnabled() || !GuestGpuWriteOwnershipSupported() ||
        g_gpu_state == nullptr || !GuestRange {address, size}.Valid()) return;
    auto& renderer = g_gpu_state->Renderer();
    // Shader contracts also use native fixture arrays. Only registered guest
    // backing is in this ownership protocol.
    if (!renderer.IsMapped(address, size)) {
        if (renderer.HasMappedMemoryOverlap(address, size))
            EXIT("PS5 native backing reader partially overlaps guest ownership\n");
        return;
    }
    if (require_cpu_readable && !LibKernel::Memory::QueryGuestAccess(
            address, size, Common::VirtualMemory::Mode::Read))
        EXIT("PS5 native raw backing reader lacks CPU-readable guest view\n");
    renderer.PublishGuestWriteDependency(address, size);
}

void PrepareNativeGuestBackingWrite(uint64_t address, uint64_t size) {
    if (!GuestAsyncPrivateBackingEnabled() || !GuestGpuWriteOwnershipSupported() ||
        g_gpu_state == nullptr || !GuestRange {address, size}.Valid()) return;
    auto& renderer = g_gpu_state->Renderer();
    if (!renderer.IsMapped(address, size)) {
        if (renderer.HasMappedMemoryOverlap(address, size))
            EXIT("PS5 native backing writer partially overlaps guest ownership\n");
        return;
    }
    // Raw PM4 stores require a writable native guest view. Such an alias is
    // CPU-visible; complete every overlapping owner before the immediate write.
    if (!LibKernel::Memory::QueryGuestAccess(address, size, Common::VirtualMemory::Mode::Write))
        EXIT("PS5 native raw backing writer lacks CPU-writable guest view\n");
    // Concurrent native stores have no exclusive code invalidation epoch.
    // CPU-only writable mappings are lawful outputs; reject only physical
    // executable aliases (and incomplete backing proof), before any store.
    if (GuestAsyncCpuConcurrentExperimentEnabled() &&
        LibKernel::Memory::QueryGuestBackingHasExecutableAlias(address, size))
        EXIT("PS5 concurrent native backing writer aliases executable guest memory\n");
    renderer.PublishGuestWriteDependency(address, size);
}

void InvalidateNativeGuestBackingWrite(uint64_t address, uint64_t size) {
    if (!GuestAsyncPrivateBackingEnabled() || !GuestGpuWriteOwnershipSupported() ||
        g_gpu_state == nullptr || !GuestRange {address, size}.Valid()) return;
    auto& renderer = g_gpu_state->Renderer();
    if (!renderer.IsMapped(address, size)) {
        if (renderer.HasMappedMemoryOverlap(address, size))
            EXIT("PS5 native backing invalidation partially overlaps guest ownership\n");
        return;
    }
    renderer.InvalidatePhysicalAliases(address, size, true);
}

enum class Pm4BackingBoundary { None, NativeBacking, CpuNotification };

static Pm4BackingBoundary Pm4BackingVisibilityBoundary(uint32_t opcode, uint32_t header,
                                                     uint32_t first_payload) {
    switch (opcode) {
        case Pm4::IT_EVENT_WRITE:
        case Pm4::IT_EVENT_WRITE_EOP:
        case Pm4::IT_EVENT_WRITE_EOS:
        case Pm4::IT_RELEASE_MEM:
        case Pm4::IT_ACQUIRE_MEM:
            return Pm4BackingBoundary::CpuNotification;
        case Pm4::IT_COPY_DATA:
            return Pm4BackingBoundary::NativeBacking;
        case Pm4::IT_NOP:
            switch (KYTY_PM4_R(header)) {
                case Pm4::R_WAIT_FLIP_DONE:
                case Pm4::R_ACQUIRE_MEM:
                case Pm4::R_FLIP:
                case Pm4::R_RELEASE_MEM: return Pm4BackingBoundary::CpuNotification;
                case Pm4::R_WRITE_DATA: return Pm4BackingBoundary::NativeBacking;
                case Pm4::R_ZERO:
                    if ((first_payload & 0xffff0000u) == 0x68750000u) {
                        switch (first_payload & 0xfffu) {
                            case 0x777: return Pm4BackingBoundary::CpuNotification;
                            // These marker forms also store a label directly;
                            // retain the complete pre-store dependency barrier.
                            case 0x778:
                            case 0x781: return Pm4BackingBoundary::NativeBacking;
                            default: break;
                        }
                    }
                    return Pm4BackingBoundary::None;
                default: return Pm4BackingBoundary::None;
            }
        default: return Pm4BackingBoundary::None;
    }
}

struct DrawIndirectArgs {
	uint32_t vertex_count_per_instance;
	uint32_t instance_count;
	uint32_t start_vertex_location;
	uint32_t start_instance_location;
};

struct DrawIndexedIndirectArgs {
	uint32_t index_count_per_instance;
	uint32_t instance_count;
	uint32_t start_index_location;
	uint32_t base_vertex_location;
	uint32_t start_instance_location;
};

static bool ReleaseMemBatchEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("LUMEN_PS5_RELEASE_MEM_BATCH");
        if (value == nullptr || std::strcmp(value, "1") == 0) return true;
        if (std::strcmp(value, "0") == 0) return false;
        std::fprintf(stderr, "LUMEN_PS5_RELEASE_MEM_BATCH must be 0 or 1\n");
        std::abort();
    }();
    return enabled;
}

static bool GraphicsRunDebugDumpEnabled() {
	return Config::GraphicsDebugDumpEnabled() &&
	       Config::GetPrintfDirection() != Config::LogDirection::Silent;
}

GuestGpu::GuestGpu(RenderContext& renderer): m_renderer(renderer) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	GraphicsInitJmpTables();
	m_gfx_cp = std::make_unique<CommandProcessor>(renderer, 0);
	m_gfx_cp->Reset();
	m_thread = std::jthread(ThreadRun, this);
}

GuestGpu::~GuestGpu() {
	Shutdown();
}

void GuestGpu::Shutdown() {
	std::lock_guard shutdown_lock(m_shutdown_mutex);
	if (m_shutdown_complete) {
		return;
	}
	{
		Common::LockGuard lock(m_queue_mutex);
		m_accepting = false;
		m_stopping  = true;
		m_work_available.SignalAll();
	}
	if (m_thread.joinable()) {
		m_thread.join();
	}
	m_shutdown_complete = true;
}

bool GuestGpu::IsStopping() {
	Common::LockGuard lock(m_queue_mutex);
	return m_stopping;
}

void GuestGpu::SendCommand(Common::UniqueFunction<void>&& command) {
	EXIT_IF(!command);
	if (IsGpuThread()) {
		if (g_current_processor != nullptr) g_current_processor->FlushPendingReleaseMem();
		command();
		return;
	}
	Common::LockGuard lock(m_queue_mutex);
	EXIT_IF(!m_accepting);
	m_commands.push_back(std::move(command));
	m_pending_commands.fetch_add(1, std::memory_order_release);
	m_work_available.Signal();
}

void GuestGpu::ProcessCommands() {
	EXIT_IF(!IsGpuThread());
	while (m_pending_commands.load(std::memory_order_acquire) != 0) {
		Common::UniqueFunction<void> command;
		{
			Common::LockGuard lock(m_queue_mutex);
			EXIT_IF(m_commands.empty());
			command = std::move(m_commands.front());
			m_commands.pop_front();
			EXIT_IF(m_pending_commands.fetch_sub(1, std::memory_order_acq_rel) == 0);
		}
		if (g_current_processor != nullptr) g_current_processor->FlushPendingReleaseMem();
		command();
	}
}

void GuestGpu::SendCommandSync(Common::UniqueFunction<void>&& command) {
	EXIT_IF(!command);
	if (IsGpuThread()) {
		if (g_current_processor != nullptr) g_current_processor->FlushPendingReleaseMem();
		command();
		return;
	}
	std::binary_semaphore done {0};
	SendCommand([operation = std::move(command), &done]() mutable {
		operation();
		done.release();
	});
	done.acquire();
}

void GuestGpu::Submit(std::span<const uint32_t> draw_commands,
                      std::span<const uint32_t> constant_commands) {
	if (draw_commands.empty()) {
		return;
	}
	Submission submission;
	submission.type              = SubmissionType::Graphics;
	submission.queue_id          = 0;
	submission.commands          = draw_commands;
	submission.constant_commands = constant_commands;
	Enqueue(std::move(submission));
}

void GuestGpu::SubmitCompute(uint32_t queue, std::span<const uint32_t> commands) {
	EXIT_IF(commands.empty());

	EXIT_NOT_IMPLEMENTED(queue < ComputeQueueBase || queue >= ComputeQueueBase + ComputeQueueCount);

	const auto compute_queue = queue - ComputeQueueBase;
	Submission submission;
	submission.type     = SubmissionType::Compute;
	submission.queue_id = 1 + compute_queue;
	submission.commands = commands;
	Enqueue(std::move(submission));
}

void GuestGpu::SubmitFlipPreparation(uint64_t request_id) {
	Submission submission;
	submission.type            = SubmissionType::FlipPreparation;
	submission.queue_id        = 0;
	submission.flip_request_id = request_id;
	Enqueue(std::move(submission));
}

void GuestGpu::SuspendPoint() {
	EXIT_IF(IsGpuThread() || CommandScheduler::InDeferredOperation());
	// Do not hold a queue lock while waiting: asynchronous work may be needed to
	// finish the preceding graphics frame. The first point returns immediately.
	{
		Loader::GuestExecution::GuestHostWaitScope wait;
		m_suspend_point_ready->acquire();
	}
	Submission submission;
	submission.type = SubmissionType::SuspendPoint;
	Enqueue(std::move(submission));
	m_done_num++;
}

int GuestGpu::GetFrameNum() const {
	return m_done_num;
}

CommandProcessor& GuestGpu::GetProcessor(uint32_t queue_id) {
	EXIT_IF(queue_id >= QueueCount);
	if (queue_id == 0) {
		return *m_gfx_cp;
	}
	auto& processor = m_compute_cp[queue_id - 1];
	if (processor == nullptr) {
		processor = std::make_unique<CommandProcessor>(m_renderer, ComputeQueueBase + queue_id - 1);
	}
	return *processor;
}

void CommandProcessor::Reset() {
	m_sh_ctx.Reset();
	m_ucfg.Reset();
	m_ctx.Reset();
	m_saved_ctx.Reset();
	m_context_state_pushed             = false;
	m_index_type_and_size              = 0;
	m_index_buffer_size                = 0;
	m_index_base_addr                  = 0;
	m_num_instances                    = 1;
	m_predicate_skip                   = false;
	m_user_data_marker                 = HW::UserSgprType::Unknown;
	m_draw_indirect_args_base_addr     = 0;
	m_dispatch_indirect_args_base_addr = 0;

	std::memset(m_const_ram, 0, sizeof(m_const_ram));
}

void CommandProcessor::ApplyContextStateOperation(ContextStateOperation operation) {
	switch (operation) {
		case ContextStateOperation::Clear: m_ctx.Reset(); break;
		case ContextStateOperation::Push:
			EXIT_IF(m_context_state_pushed);
			m_saved_ctx            = m_ctx;
			m_context_state_pushed = true;
			break;
		case ContextStateOperation::Pop:
			EXIT_IF(!m_context_state_pushed);
			m_ctx                  = m_saved_ctx;
			m_saved_ctx            = {};
			m_context_state_pushed = false;
			break;
		case ContextStateOperation::PushClear:
			EXIT_IF(m_context_state_pushed);
			m_saved_ctx            = m_ctx;
			m_context_state_pushed = true;
			m_ctx.Reset();
			break;
		default: EXIT("unknown context state operation: %u\n", static_cast<uint32_t>(operation));
	}
}

void CommandProcessor::BufferInit() {
	GetScheduler().Begin(m_ctx, m_ucfg, m_sh_ctx);
}

void CommandProcessor::BufferFlush() {
    m_release_mem_batch.Reset();
    GetScheduler().Flush();
}

void CommandProcessor::RequestReleaseMemFlush() {
    if (!ReleaseMemBatchEnabled()) {
        BufferFlush();
        return;
    }
    m_release_mem_batch.Request(GetScheduler().CurrentTick());
}

void CommandProcessor::FlushPendingReleaseMem() {
    if (m_release_mem_batch.IsPending() &&
        m_release_mem_batch.TakePending(GetScheduler().CurrentTick())) {
        GetScheduler().Flush();
    }
}

void CommandProcessor::AdvanceReleaseMemPacket() {
    if (m_release_mem_batch.IsPending() &&
        m_release_mem_batch.ConsumePacket(GetScheduler().CurrentTick())) {
        FlushPendingReleaseMem();
    }
}

void CommandProcessor::BufferWait() {
	BufferInit();
	GetScheduler().Finish();
}

void CommandProcessor::ResetDeCe() {
	m_de_count    = 0;
	m_ce_count    = 0;
	m_ce_complete = false;
}

void CommandProcessor::WaitCe() {
	FlushPendingReleaseMem();
	if (m_ce_count <= m_de_count && !m_ce_complete) {
		SuspendPm4();
	}
}

void CommandProcessor::WaitDeDiff(uint32_t diff) {
	FlushPendingReleaseMem();
	EXIT_IF(m_de_count > m_ce_count);
	if (m_ce_count - m_de_count >= diff) {
		SuspendPm4();
	}
}

void CommandProcessor::WaitForRewind(bool valid) {
	FlushPendingReleaseMem();
	if (!valid) {
		SuspendPm4();
	}
}

void CommandProcessor::IncrementDe() {
	m_de_count++;
}

void CommandProcessor::IncrementCe() {
	m_ce_count++;
}

void CommandProcessor::WriteConstRam(uint32_t offset, const uint32_t* src, uint32_t dw_num) {
	memcpy(m_const_ram + offset / 4, src, static_cast<size_t>(dw_num) * 4);
}

void CommandProcessor::DumpConstRam(uint32_t* dst, uint32_t offset, uint32_t dw_num) {
	if (dw_num == 0) return;
	const uint64_t address = reinterpret_cast<uint64_t>(dst);
	const uint64_t size = uint64_t {dw_num} * sizeof(uint32_t);
	EXIT_IF(offset > sizeof(m_const_ram) || size > sizeof(m_const_ram) - offset);
	if (!GuestRange {address, size}.Valid()) EXIT("PS5 const RAM dump destination range is invalid\n");
	PrepareNativeGuestBackingWrite(address, size);
	memcpy(dst, m_const_ram + offset / 4, static_cast<size_t>(size));
	InvalidateNativeGuestBackingWrite(address, size);
}

bool TestWaitRegMemValue(uint64_t value, uint64_t ref, uint64_t mask, uint32_t func) {
	switch (func) {
		case 0: return true;
		case 1: return (value & mask) < ref;
		case 2: return (value & mask) <= ref;
		case 3: return (value & mask) == ref;
		case 4: return (value & mask) != ref;
		case 5: return (value & mask) >= ref;
		case 6: return (value & mask) > ref;
		default: EXIT("unknown wait compare function: %" PRIu32 "\n", func);
	}

	return false;
}

template <typename T>
void CommandProcessor::WaitRegMem(uint32_t func, const T* addr, T ref, T mask, uint32_t poll,
                                  uint32_t wait_op) {
	EXIT_IF(addr == nullptr);
	if ((wait_op & ~1u) != 0) {
		EXIT("unsupported wait_reg_mem operation: 0x%08" PRIx32 "\n", wait_op);
	}

	(void)poll;
	FlushPendingReleaseMem();
	const auto address = reinterpret_cast<uint64_t>(addr);
	if (!GuestRange {address, sizeof(T)}.Valid()) EXIT("PS5 wait register memory range is invalid\n");
	PublishNativeGuestBackingRead(address, sizeof(T), true);
	if (!TestWaitRegMemValue(*addr, ref, mask, func)) {
		SuspendPm4();
	}
}

template void CommandProcessor::WaitRegMem<uint32_t>(uint32_t, const uint32_t*, uint32_t, uint32_t,
                                                     uint32_t, uint32_t);
template void CommandProcessor::WaitRegMem<uint64_t>(uint32_t, const uint64_t*, uint64_t, uint64_t,
                                                     uint32_t, uint32_t);

void CommandProcessor::WriteData(uint32_t* dst, const uint32_t* src, uint32_t dw_num,
                                 uint32_t write_control) {
	const uint32_t dst_sel = ((write_control >> 30u) & 0x1u) | ((write_control >> 7u) & 0x1eu);
	const bool     write_one_address = ((write_control >> 16u) & 0x1u) != 0;

	switch (dst_sel) {
		case 0:
		case 2:
		case 4:
		case 5:
		case 6: break;
		default: EXIT("unsupported writeData destination selector 0x%02" PRIx32 "\n", dst_sel);
	}
	if (dw_num == 0) {
		return;
	}
	const uint64_t address = reinterpret_cast<uint64_t>(dst);
	const uint64_t size = write_one_address ? sizeof(uint32_t) : uint64_t {dw_num} * sizeof(uint32_t);
	if (!GuestRange {address, size}.Valid()) EXIT("PS5 WRITE_DATA destination range is invalid\n");
	PrepareNativeGuestBackingWrite(address, size);

	if (write_one_address) {
		for (uint32_t i = 0; i < dw_num; i++) {
			dst[0] = src[i];
		}
	} else {
		memcpy(dst, src, static_cast<size_t>(dw_num) * sizeof(uint32_t));
	}
	if (GuestAsyncPrivateBackingEnabled() && GuestGpuWriteOwnershipSupported() &&
	    m_renderer.IsMapped(address, size))
		m_renderer.InvalidatePhysicalAliases(address, size, true);
}

void CommandProcessor::WriteReferenceClock(uint64_t dst_address, uint32_t num_bytes) {
	if (dst_address == 0 || (num_bytes != sizeof(uint32_t) && num_bytes != sizeof(uint64_t)) ||
	    (dst_address & (num_bytes - 1u)) != 0) {
		EXIT("invalid reference-clock copy, dst=0x%016" PRIx64 " size=%u\n", dst_address,
		     num_bytes);
	}
	PrepareNativeGuestBackingWrite(dst_address, num_bytes);
	const auto value = Sync::ReadReferenceClock();
	std::memcpy(reinterpret_cast<void*>(dst_address), &value, num_bytes);
	InvalidateNativeGuestBackingWrite(dst_address, num_bytes);
	static std::atomic<uint32_t> clock_log_count {0};
	if (clock_log_count.fetch_add(1) < 64) {
		LOGF("\t copy_data reference clock: dst=0x%016" PRIx64 " value=0x%016" PRIx64
		     " size=%u\n",
		     dst_address, value, num_bytes);
	}
}

void CommandProcessor::DmaData(uint8_t engine, uint8_t dst_sel, uint8_t dst_cache_policy,
                               uint64_t dst_address_or_offset, uint8_t src_sel,
                               uint8_t  src_cache_policy,
                               uint64_t src_address_or_offset_or_immediate, uint32_t num_bytes,
                               uint8_t wait_for_previous, uint8_t write_confirm,
                               uint8_t block_engine) {
	EXIT_NOT_IMPLEMENTED(engine > 1);
	if (num_bytes == 0) {
		return;
	}
	EXIT_NOT_IMPLEMENTED(dst_cache_policy > 3);
	EXIT_NOT_IMPLEMENTED(src_cache_policy > 3);
	EXIT_NOT_IMPLEMENTED(wait_for_previous > 1);
	EXIT_NOT_IMPLEMENTED(write_confirm > 1);
	EXIT_NOT_IMPLEMENTED(block_engine > 1);
	if (static_cast<uint32_t>(dst_address_or_offset) == 0x3022cu) {
		return;
	}
	auto decode_gds = [](uint8_t selector, bool& is_gds) {
		switch (selector) {
			case 0:
			case 3: is_gds = false; return true;
			case 1: is_gds = true; return true;
			default: return false;
		}
	};
	if (dst_sel == 2) {
		// kNowhere discards the GL2 prefetch destination without a guest-visible write.
		if (src_sel != 3) {
			EXIT("unsupported dmaData nowhere source selector 0x%02" PRIx8 "\n", src_sel);
		}
		return;
	}
	bool dst_gds = false;
	if (!decode_gds(dst_sel, dst_gds)) {
		EXIT("unsupported dmaData destination selector 0x%02" PRIx8 "\n", dst_sel);
	}
	auto& buffer_cache = m_renderer.GetBufferCache();
	if (src_sel == 2) {
		buffer_cache.FillBuffer(
		    dst_address_or_offset, num_bytes,
		    static_cast<uint32_t>(src_address_or_offset_or_immediate & 0xffffffffu), dst_gds);
		return;
	}
	bool src_gds = false;
	if (!decode_gds(src_sel, src_gds)) {
		EXIT("unsupported dmaData source selector 0x%02" PRIx8 "\n", src_sel);
	}
	if (src_gds && dst_gds) {
		EXIT("unsupported dmaData GDS-to-GDS copy\n");
	}
	buffer_cache.CopyBuffer(dst_address_or_offset, src_address_or_offset_or_immediate, num_bytes,
	                        dst_gds, src_gds);
}

void GuestGpu::Enqueue(Submission submission) {
	EXIT_IF(submission.queue_id >= QueueCount);
	Common::LockGuard lock(m_queue_mutex);
	EXIT_IF(!m_accepting);
	m_queues[submission.queue_id].push_back(std::move(submission));
	m_submission_count++;
	m_work_available.Signal();
}

void GuestGpu::WaitForIdle() {
	EXIT_IF(IsGpuThread() || CommandScheduler::InDeferredOperation());
	Common::LockGuard lock(m_queue_mutex);
	while (m_processing || !m_commands.empty() || m_submission_count != 0) {
		m_idle.Wait(&m_queue_mutex);
	}
}

void GuestGpu::ThreadRun(void* data) {
	auto* gpu = static_cast<GuestGpu*>(data);
	EXIT_IF(gpu == nullptr);
	KYTY_PROFILER_THREAD("Thread_Gpu");
	g_gpu_thread = true;
	g_gpu_state  = gpu;

	for (;;) {
		Submission                   submission;
		Common::UniqueFunction<void> command;
		bool                         has_submission = false;
		bool                         should_stop    = false;
		{
			Common::LockGuard lock(gpu->m_queue_mutex);
			while (gpu->m_commands.empty() && gpu->m_submission_count == 0 && !gpu->m_stopping) {
				gpu->m_processing = false;
				gpu->m_idle.Signal();
				gpu->m_work_available.Wait(&gpu->m_queue_mutex);
			}
			if (gpu->m_stopping && gpu->m_commands.empty() && gpu->m_submission_count == 0) {
				gpu->m_processing = false;
				gpu->m_idle.SignalAll();
				should_stop = true;
			} else if (!gpu->m_commands.empty()) {
				command = std::move(gpu->m_commands.front());
				gpu->m_commands.pop_front();
				EXIT_IF(gpu->m_pending_commands.fetch_sub(1, std::memory_order_acq_rel) == 0);
				gpu->m_processing = true;
			} else {
				int selected_queue = -1;
				for (uint32_t offset = 0; offset < QueueCount; offset++) {
					const auto id = (gpu->m_next_queue + offset) % QueueCount;
					if (!gpu->m_queues[id].empty() && !gpu->m_queues[id].front().blocked) {
						selected_queue = static_cast<int>(id);
						break;
					}
				}
				if (selected_queue < 0) {
					gpu->m_processing = false;
					gpu->m_work_available.WaitFor(&gpu->m_queue_mutex, 100);
					for (auto& queue: gpu->m_queues) {
						if (!queue.empty()) {
							queue.front().blocked = false;
						}
					}
					continue;
				}
				auto& queue = gpu->m_queues[static_cast<uint32_t>(selected_queue)];
				submission  = std::move(queue.front());
				queue.pop_front();
				gpu->m_submission_count--;
				gpu->m_next_queue = (static_cast<uint32_t>(selected_queue) + 1) % QueueCount;
				gpu->m_processing = true;
				has_submission    = true;
			}
		}
		if (should_stop) {
            if (GuestAsyncPrivateBackingEnabled()) {
                // An empty graphics slice acquires the same FEX authority and
                // drains all retained owners on their sole cache worker.
                Submission drain;
                (void)gpu->Process(drain);
            }
			gpu->m_gfx_cp->BufferWait();
			g_gpu_state  = nullptr;
			g_gpu_thread = false;
			return;
		}

		if (command) {
			EXIT_IF(g_current_processor != nullptr);
			command();

			Common::LockGuard lock(gpu->m_queue_mutex);
			gpu->m_processing = false;
			if (gpu->m_commands.empty() && gpu->m_submission_count == 0) {
				gpu->m_idle.SignalAll();
			}
			continue;
		}

		EXIT_IF(!has_submission);
		const bool complete = gpu->Process(submission);

		Common::LockGuard lock(gpu->m_queue_mutex);
		if (!complete) {
			submission.blocked = true;
			gpu->m_queues[submission.queue_id].push_front(std::move(submission));
			gpu->m_submission_count++;
		} else {
			for (auto& queue: gpu->m_queues) {
				if (!queue.empty()) {
					queue.front().blocked = false;
				}
			}
		}
		gpu->m_processing = false;
		if (gpu->m_commands.empty() && gpu->m_submission_count == 0) {
			gpu->m_idle.SignalAll();
		}
	}
}

bool GuestGpu::Process(Submission& submission) {
    GpuSliceTrace slice_trace(submission.queue_id, static_cast<uint32_t>(submission.type));
    if (slice_trace.recording) {
        Common::LockGuard lock(m_queue_mutex);
        slice_trace.queued_submissions = m_submission_count;
        slice_trace.pending_commands = m_pending_commands.load(std::memory_order_acquire);
        for (const auto& queue: m_queues)
            if (!queue.empty() && !queue.front().blocked) ++slice_trace.ready_queue_heads;
    }
#if defined(LUMEN_PS5_DIRECT_FEX)
    // Direct Gen4 uses the PS5 native VM/renderer ownership protocol. This
    // writer lock serializes mapping changes and GPU work; it is not a CPU
    // execution epoch and makes no translated-thread quiescence claim.
    std::unique_lock gpu_intent(Loader::GuestExecution::guest_writer_gate, std::defer_lock);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(16);
    while (!gpu_intent.try_lock()) {
        ProcessCommands();
        if (std::chrono::steady_clock::now() >= deadline)
            EXIT("PS5 direct GPU writer lock timed out\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    constexpr uint64_t epoch = 0;
    constexpr bool native_concurrent = false;
    EXIT_IF(GuestGpuPublicationActive);
    GuestGpuPublicationActive = true;
    slice_trace.Admitted(0, m_renderer.GetCommandScheduler().CurrentTick());
#elif defined(LUMEN_PS5_FEXCORE)
	// No PM4 input has been borrowed and no queue/cache lock is held here.
	// A CPU VM writer can be waiting for a native unmap command; serve that
	// command lane between failed admissions, before doing any PM4 effects.
	uint64_t epoch = 0;
	uint64_t native_data_lease = 0;
	std::unique_lock gpu_intent(Loader::GuestExecution::guest_writer_gate, std::defer_lock);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(16);
	while (!gpu_intent.try_lock()) {
		ProcessCommands();
		if (std::chrono::steady_clock::now() >= deadline)
			EXIT("PS5 GPU writer intent admission timed out\n");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const bool native_concurrent = GuestAsyncCpuConcurrentExperimentEnabled() && !IsStopping();
	if (native_concurrent) {
		// The independent native reader pins existing mappings, without a JIT
		// safepoint request. Failed admission is nonblocking: service a CPU
		// exclusive owner's command lane before retrying, never borrow first.
		while (native_data_lease == 0) {
			ProcessCommands();
			native_data_lease = LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV1();
			if (native_data_lease != 0) break;
			if (std::chrono::steady_clock::now() >= deadline)
				EXIT("PS5 GPU native data lease admission timed out\n");
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	} else {
		for (uint32_t attempt = 0; epoch == 0 && attempt != 8; ++attempt) {
			ProcessCommands();
			epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
		}
		if (epoch == 0) EXIT("PS5 GPU publication epoch admission denied\n");
	}
	EXIT_IF(GuestGpuPublicationActive);
	GuestGpuPublicationActive = true;
	slice_trace.Admitted(epoch, m_renderer.GetCommandScheduler().CurrentTick());
#else
    constexpr bool native_concurrent = false;
    slice_trace.Admitted(0, m_renderer.GetCommandScheduler().CurrentTick());
#endif
	#if defined(LUMEN_PS5_FEXCORE)
	Lumen::Performance::Scope gpu_epoch(Lumen::Performance::Phase::GpuEpoch, epoch);
#else
	Lumen::Performance::Scope gpu_epoch(Lumen::Performance::Phase::GpuEpoch);
#endif
		// Submitted/indirect command spans retain their existing caller lifetime
	// contract until execution completes. The pin covers this active slice;
	// blocked execution releases it and revalidates inputs at its next entry.
	const bool first_slice = !submission.started;
	auto&      cp          = GetProcessor(submission.queue_id);

	if (first_slice) {
		submission.started = true;
		cp.SetSubmitId(++m_submit_id);
		cp.ResetDeCe();
		cp.SetFlip({});
	}
    slice_trace.submit_id = cp.GetSubmitId();

	cp.BufferInit();
	bool complete = true;

	switch (submission.type) {
		case SubmissionType::Graphics: {
			bool progressed = false;
			submission.constant_complete |= submission.constant_commands.empty();
			for (;;) {
				bool round_progress = false;
				if (!submission.constant_complete) {
					submission.constant_complete =
					    cp.Process(submission.constant_execution, submission.constant_commands) ==
					    Pm4ProcessResult::Complete;
					round_progress |= submission.constant_execution.MadeProgress();
				}
				cp.SetCeComplete(submission.constant_complete);
				if (!submission.command_complete) {
					submission.command_complete =
					    cp.Process(submission.command_execution, submission.commands) ==
					    Pm4ProcessResult::Complete;
					round_progress |= submission.command_execution.MadeProgress();
				}
				progressed |= round_progress;
				complete = submission.command_complete && submission.constant_complete;
				if (complete || !round_progress) {
					break;
				}
			}
			if (progressed) {
				if (complete) {
					m_renderer.RunGarbageCollector();
				}
				cp.BufferFlush();
			} else if (complete) {
				m_renderer.RunGarbageCollector();
			}
			break;
		}
		case SubmissionType::Compute: {
			const auto      num_dw = static_cast<uint32_t>(submission.commands.size());
			const auto*     buffer = submission.commands.data();
			static uint32_t compute_batch_log_count = 0;
			if (first_slice && num_dw <= 128 && compute_batch_log_count++ < 32) {
				LOGF("compute direct batch: data=0x%016" PRIx64 ", num_dw=%" PRIu32 "\n",
				     reinterpret_cast<uint64_t>(buffer), num_dw);
				for (uint32_t i = 0; i < std::min<uint32_t>(num_dw, 16); i++) {
					LOGF("\t compute[%02" PRIu32 "] = 0x%08" PRIx32 "\n", i, buffer[i]);
				}
			}
			if (first_slice) {
				GraphicsDbgDumpDcb("cc", num_dw, buffer);
			}
			complete = cp.Process(submission.command_execution, submission.commands) ==
			           Pm4ProcessResult::Complete;
			if (submission.command_execution.MadeProgress()) {
				if (complete) {
					m_renderer.RunGarbageCollector();
				}
				cp.BufferFlush();
			} else if (complete) {
				m_renderer.RunGarbageCollector();
			}
			break;
		}
		case SubmissionType::FlipPreparation:
			m_renderer.RunGarbageCollector();
			cp.PrepareCpuFlip(submission.flip_request_id);
			break;
		case SubmissionType::SuspendPoint:
			cp.EmitGlobalBarrier();
			m_renderer.GetCommandScheduler().DeferPriorityOperation(
			    [ready = m_suspend_point_ready] { ready->release(); });
			cp.BufferFlush();
			cp.Reset();
			break;
	}

    // Strict blocked/suspend slices and shutdown fully drain. A completed CPU flip
    // owns an independent native Frame; its pending CompleteFlip callback forces
    // producer completion before selective publication and CPU readmission.
    // The CPU-visible experiment also retains owners through blocked/suspend
    // readmission; exact native dependencies resolve labels on every resume.
    // A slice admitted concurrently retains through a mid-slice stop request;
    // the final synthetic shutdown slice then takes a real exclusive epoch and
    // fully publishes before the worker ends. Never upgrade a live native pin.
    const bool allow_async = (native_concurrent || !IsStopping()) &&
        (GuestAsyncCpuVisibleExperimentEnabled() ||
         (complete && (submission.type == SubmissionType::Graphics ||
                       submission.type == SubmissionType::Compute ||
                       submission.type == SubmissionType::FlipPreparation)));
    slice_trace.BeginTail();
    m_renderer.FinishGuestGpuSlice(allow_async);
    slice_trace.BeginClose();
#if defined(LUMEN_PS5_FEXCORE)
	GuestGpuPublicationActive = false;
#if !defined(LUMEN_PS5_DIRECT_FEX)
	if (native_data_lease != 0) {
		// Tail completion drains every pointer-bearing deferred callback. No
		// pin crosses the blocked queue, and no lease upgrades into a writer.
		if (LumenAndroidFexCoreEndNativeGuestDataLeaseV1(native_data_lease) == 0)
			EXIT("PS5 GPU native data lease close failed\n");
	} else if (LumenAndroidFexCoreEndGuestMemoryMutationV2(epoch, 0, 0, 0, nullptr, nullptr) == 0) {
		EXIT("PS5 GPU publication epoch close failed\n");
	}
#endif
	static std::atomic<uint32_t> publications {0};
	const auto serial = publications.fetch_add(1, std::memory_order_relaxed) + 1;
	if (serial <= 16 || serial % 120 == 0) {
        if (GuestAsyncPrivateBackingEnabled())
            std::fprintf(stderr, "PS5_GPU_SLICE_CLOSED serial=%u slice_complete=%u private_allowed=%u concurrent_shared=%u\n",
                serial, complete ? 1U : 0U, allow_async ? 1U : 0U, native_concurrent ? 1U : 0U);
        else
            std::fprintf(stderr, "PS5_GPU_BACKING_PUBLICATION_COMPLETED serial=%u slice_complete=%u\n",
                serial, complete ? 1U : 0U);
    }
#endif
	slice_trace.End(complete, m_renderer.GetCommandScheduler().CurrentTick());
	return complete;
}

Pm4ProcessResult CommandProcessor::Process(Pm4Execution&             execution,
                                           std::span<const uint32_t> commands) {
	KYTY_PROFILER_BLOCK("CommandProcessor::Process");
	EXIT_IF(g_current_execution != nullptr);
	EXIT_IF(commands.size() > UINT32_MAX);
	if (execution.m_buffer_stack.empty() && !commands.empty()) {
		execution.m_buffer_stack.push_back({commands});
	}
	execution.m_suspended     = false;
	execution.m_made_progress = false;

	struct ExecutionScope {
		ExecutionScope(CommandProcessor& processor, Pm4Execution& execution)
		    : previous_processor(g_current_processor), previous_execution(g_current_execution) {
			g_current_processor = &processor;
			g_current_execution = &execution;
		}
		~ExecutionScope() {
			g_current_processor->FlushPendingReleaseMem();
			g_current_processor = previous_processor;
			g_current_execution = previous_execution;
		}

		CommandProcessor* previous_processor;
		Pm4Execution*     previous_execution;
	} execution_scope(*this, execution);

	ProcessPm4(execution);
	return execution.m_buffer_stack.empty() ? Pm4ProcessResult::Complete
	                                        : Pm4ProcessResult::Blocked;
}

void CommandProcessor::ProcessIndirectBuffer(std::span<const uint32_t> commands, bool chain) {
	FlushPendingReleaseMem();
    if (!commands.empty() && !GuestRange {reinterpret_cast<uint64_t>(commands.data()),
                                        commands.size_bytes()}.Valid())
        EXIT("PS5 indirect command backing range is invalid\n");
    PublishNativeGuestBackingRead(reinterpret_cast<uint64_t>(commands.data()), commands.size_bytes(), true);
	EXIT_IF(g_current_execution == nullptr);
	EXIT_IF(!g_current_execution->m_next_buffer.empty());
	g_current_execution->m_next_buffer = commands;
	g_current_execution->m_chain       = chain;
}

void CommandProcessor::SuspendPm4() {
	FlushPendingReleaseMem();
	EXIT_IF(g_current_execution == nullptr);
	g_current_execution->m_suspended = true;
}

void CommandProcessor::ProcessPm4(Pm4Execution& execution) {
	while (!execution.m_buffer_stack.empty()) {
		if (g_gpu_state != nullptr) {
			g_gpu_state->ProcessCommands();
		}
		auto& cursor = execution.m_buffer_stack.back();
		EXIT_IF(cursor.offset_dw > cursor.commands.size());
		if (cursor.offset_dw == cursor.commands.size()) {
			execution.m_buffer_stack.pop_back();
			continue;
		}

        PublishNativeGuestBackingRead(
            reinterpret_cast<uint64_t>(cursor.commands.data() + cursor.offset_dw),
            (cursor.commands.size() - cursor.offset_dw) * sizeof(uint32_t), true);
        // Count every fetched packet, including skipped packets and one-word
        // NOPs. An indirect self-chain must not postpone an EOP indefinitely.
        struct PacketScope {
            CommandProcessor& processor;
            ~PacketScope() { processor.AdvanceReleaseMemPacket(); }
        } packet_scope{*this};

		const auto* const packet        = cursor.commands.data() + cursor.offset_dw;
		const auto        total_dw      = static_cast<uint32_t>(cursor.commands.size());
		const auto        remaining_dw  = total_dw - cursor.offset_dw;
		const auto        packet_header = packet[0];
		const auto        opcode        = (packet_header >> 8u) & 0xffu;
		EXIT_NOT_IMPLEMENTED(remaining_dw > total_dw);

		if (packet_header == 0x80000000u || Pm4::PacketSizeDw(packet_header) == 1u) {
			cursor.offset_dw++;
			execution.m_made_progress = true;
			continue;
		}

		EXIT_NOT_IMPLEMENTED(remaining_dw < 2);

		if (GraphicsRunDebugDumpEnabled()) {
			LOGF("CP packet: offset=0x%05" PRIx32 " cmd_id=0x%08" PRIx32 " op=0x%02" PRIx32
			     " len=%" PRIu32 "\n",
			     total_dw - remaining_dw, packet_header, opcode, KYTY_PM4_LEN(packet_header));
		}

		if ((packet_header & 1u) != 0 && ShouldSkipPredicatedPackets()) {
			auto packet_dw = KYTY_PM4_LEN(packet_header);
			EXIT_NOT_IMPLEMENTED(packet_dw == 0 || packet_dw > remaining_dw);
			static std::atomic<uint32_t> skip_log_count {0};
			if (skip_log_count.fetch_add(1) < 2048) {
				LOGF("\t predicated skip: op=0x%02" PRIx32 ", r=0x%02" PRIx32 ", len=%" PRIu32
				     ", packet=0x%016" PRIx64 ", cmd_id=0x%08" PRIx32 "\n",
				     opcode, KYTY_PM4_R(packet_header), packet_dw,
				     reinterpret_cast<uint64_t>(packet), packet_header);
			}
			if (opcode == Pm4::IT_NOP && KYTY_PM4_R(packet_header) == Pm4::R_RELEASE_MEM &&
			    packet_dw >= 7) {
				static std::atomic<uint32_t> log_count {0};
				if (log_count.fetch_add(1) < 128) {
					const auto dst = packet[3] | (static_cast<uint64_t>(packet[4]) << 32u);
					const auto val = packet[5] | (static_cast<uint64_t>(packet[6]) << 32u);
					LOGF("\t predicated skip: R_RELEASE_MEM dst=0x%016" PRIx64
					     ", value=0x%016" PRIx64 ", action=0x%08" PRIx32
					     ", gcr/data/int=0x%08" PRIx32 "\n",
					     dst, val, packet[1], packet[2]);
				}
			}
			cursor.offset_dw += packet_dw;
			execution.m_made_progress = true;
			continue;
		}

        if (GuestAsyncPrivateBackingEnabled()) {
            const auto boundary = Pm4BackingVisibilityBoundary(opcode, packet_header, packet[1]);
            if (boundary == Pm4BackingBoundary::NativeBacking &&
                !GuestAsyncCpuVisibleExperimentEnabled()) {
                // A native raw read or store consumes current backing, before
                // any subsequent private GPU producers acquire their owners.
                m_renderer.PublishGuestWrites();
            }
            // Notifications remain buffered within the exclusive epoch. Their
            // callbacks keep their producer timeline. Strict mode completes
            // CPU-visible publication before readmission; the CPU-visible
            // experiment deliberately skips this global native-backing barrier
            // to expose the remaining consumers that lack exact dependencies.
        }
		auto handler = g_cp_op_func[opcode];

		if (handler == nullptr) {
			const auto offset = total_dw - remaining_dw;
			LOGF("unknown PM4 packet: data=0x%016" PRIx64 ", num_dw=%" PRIu32
			     ", offset=0x%05" PRIx32 ", current=0x%016" PRIx64 "\n",
			     reinterpret_cast<uint64_t>(packet - offset), total_dw, offset,
			     reinterpret_cast<uint64_t>(packet));
			const auto  dump_begin = (offset > 8 ? offset - 8 : 0);
			const auto  dump_end   = std::min<uint32_t>(total_dw, offset + 16);
			auto* const base       = packet - offset;
			for (uint32_t i = dump_begin; i < dump_end; i++) {
				LOGF("\t%05" PRIx32 "%s %08" PRIx32 "\n", i, (i == offset ? ":" : " "), base[i]);
			}
			EXIT("unknown op\n\t%05" PRIx32 ":\n\tcmd_id = %08" PRIx32 "\n",
			     total_dw - remaining_dw, packet_header);
		}

		const auto packet_dw =
		    handler(*this, packet_header & ~1u, packet + 1, remaining_dw, total_dw) + 1;
		EXIT_IF(packet_dw > remaining_dw);
		if (execution.m_suspended) {
			return;
		}
		cursor.offset_dw += packet_dw;
		execution.m_made_progress = true;
		if (!execution.m_next_buffer.empty()) {
			// Chains and taken branches reuse the fetcher; only calls retain a return cursor.
			if (execution.m_chain) {
				cursor = {execution.m_next_buffer};
			} else {
				execution.m_buffer_stack.push_back({execution.m_next_buffer});
			}
			execution.m_next_buffer = {};
		}
	}
}

void CommandProcessor::SetIndexType(uint32_t index_type_and_size) {
	m_index_type_and_size = index_type_and_size & 0x3u;
}

void CommandProcessor::SetIndexBaseAddress(uint64_t index_base_addr) {
	m_index_base_addr = index_base_addr;
}

void CommandProcessor::SetIndexBufferSize(uint32_t index_buffer_size) {
	m_index_buffer_size = index_buffer_size;
}

void CommandProcessor::SetDrawIndirectArgsBaseAddress(uint64_t draw_indirect_args_base_addr) {
	m_draw_indirect_args_base_addr = draw_indirect_args_base_addr;
}

void CommandProcessor::SetDispatchIndirectArgsBaseAddress(
    uint64_t dispatch_indirect_args_base_addr) {
	m_dispatch_indirect_args_base_addr = dispatch_indirect_args_base_addr;
}

void CommandProcessor::SetNumInstances(uint32_t num_instances) {
	if (num_instances == 0) {
		num_instances = 1;
	}

	m_num_instances = num_instances;
}

void CommandProcessor::SetPredication(uint32_t condition, uint32_t op, uint32_t wait_op,
                                      const volatile void* address, uint32_t count_in_dwords) {
	(void)count_in_dwords;
	FlushPendingReleaseMem();
	uint64_t value = 0;

	switch (op) {
		case 0x00:
			m_predicate_skip = false;
			return;
		case 0x01: {
			EXIT_NOT_IMPLEMENTED(address == nullptr);
			const auto counter_address = reinterpret_cast<uint64_t>(address);
			constexpr uint64_t counter_size = 16u * 2u * sizeof(uint64_t);
			if (!GuestRange {counter_address, counter_size}.Valid())
				EXIT("PS5 predication counter range is invalid\n");
			PublishNativeGuestBackingRead(counter_address, counter_size, true);
			// One begin/end pair per DB; bit 63 marks each counter ready.
			constexpr uint64_t ready_bit = 1ull << 63u;
			const auto* results = reinterpret_cast<const volatile uint64_t*>(address);
			for (uint32_t db = 0; db < 16u; db++) {
				const auto begin = results[db * 2u];
				const auto end   = results[db * 2u + 1u];
				if ((begin & end & ready_bit) == 0) {
					if (wait_op == 0) {
						SuspendPm4();
					} else {
						m_predicate_skip = false;
					}
					return;
				}
				value += end - begin;
			}
		} break;
		case 0x03:
			// The wait selector applies only to Z-pass query readiness.
			EXIT_NOT_IMPLEMENTED(address == nullptr);
			if (!GuestRange {reinterpret_cast<uint64_t>(address), sizeof(uint64_t)}.Valid())
				EXIT("PS5 predication boolean range is invalid\n");
			PublishNativeGuestBackingRead(reinterpret_cast<uint64_t>(address), sizeof(uint64_t), true);
			value = *reinterpret_cast<const volatile uint64_t*>(address);
			break;
		default: EXIT("unknown predication op: 0x%08" PRIx32 "\n", op);
	}
	switch (condition) {
		case 0x00: m_predicate_skip = (value != 0); break;
		case 0x01: m_predicate_skip = (value == 0); break;
		default: EXIT("unknown predication condition: 0x%08" PRIx32 "\n", condition);
	}
	if (op == 0x03) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1) < 128) {
			LOGF("\t bool predication: addr=0x%016" PRIx64 ", value=0x%016" PRIx64
			     ", condition=%" PRIu32 ", skip=%u, wait_op=%" PRIu32 "\n",
			     reinterpret_cast<uint64_t>(address), value, condition,
			     m_predicate_skip ? 1u : 0u, wait_op);
		}
	}
}

void CommandProcessor::DrawIndex(DrawIndexArgs args) {
	args.index_type_and_size = m_index_type_and_size;
	if (args.instance_count == 0) {
		args.instance_count = m_num_instances;
	}
	if (GraphicsRunDebugDumpEnabled() && (args.base_vertex != 0 || args.first_instance != 0)) {
		LOGF("\t draw indexed offsets: base_vertex = %" PRId32 ", first_instance = %" PRIu32 "\n",
		     args.base_vertex, args.first_instance);
	}
	m_renderer.GetRenderExecutor().DrawIndex(m_submit_id, CurrentBuffer(), args);
}

void CommandProcessor::DrawIndexOffset(uint32_t index_offset, uint32_t index_count) {
	uint64_t index_size = 0;
	switch (m_index_type_and_size) {
		case 0: index_size = 2; break;
		case 1: index_size = 4; break;
		case 2: index_size = 1; break;
		default: EXIT("unknown index_type_and_size: %u\n", m_index_type_and_size);
	}

	auto* index_addr = reinterpret_cast<const void*>(
	    m_index_base_addr + static_cast<uint64_t>(index_offset) * index_size);

	DrawIndex({.index_count = index_count, .index_addr = index_addr});
}

static void PatchIndirectDrawOffsets(CommandProcessor& cp, IndirectDrawRegisters registers,
                                     uint32_t vertex_offset, uint32_t instance_offset,
                                     uint32_t first_index) {
	const auto write = [&](uint32_t location, uint32_t value) {
		if (location == Pm4::SH_NOP) {
			return;
		}
		EXIT_NOT_IMPLEMENTED(location >= Pm4::SH_NUM ||
		                     g_hw_sh_indirect_func[location] == nullptr);
		g_hw_sh_indirect_func[location](cp, location, value);
	};
	write(registers.vertex_offset, vertex_offset);
	write(registers.instance_offset, instance_offset);
	write(registers.index_offset, first_index);
}

void CommandProcessor::DrawIndirect(uint32_t data_offset, IndirectDrawRegisters registers,
                                    uint32_t draw_initiator, bool indexed) {
	const auto args_size = indexed ? sizeof(DrawIndexedIndirectArgs) : sizeof(DrawIndirectArgs);
	DrawIndirectMulti(data_offset, 1, nullptr, args_size, registers, draw_initiator, indexed);
}

void CommandProcessor::DrawIndirectMulti(uint32_t data_offset, uint32_t max_count_or_count,
                                         const volatile uint32_t* count_addr,
                                         uint32_t stride_in_bytes, IndirectDrawRegisters registers,
                                         uint32_t draw_initiator, bool indexed) {
	EXIT_NOT_IMPLEMENTED((draw_initiator & ~0x20u) != (indexed ? 0u : 2u));
	EXIT_NOT_IMPLEMENTED(m_draw_indirect_args_base_addr == 0);

	uint32_t draw_count = max_count_or_count;
	if (count_addr != nullptr) {
		const auto count_address = reinterpret_cast<uint64_t>(count_addr);
		if (!GuestRange {count_address, sizeof(*count_addr)}.Valid())
			EXIT("PS5 multi indirect draw count range is invalid\n");
		PublishNativeGuestBackingRead(count_address, sizeof(*count_addr), true);
		draw_count = *count_addr;
		if (draw_count > max_count_or_count) {
			draw_count = max_count_or_count;
		}
	}

	if (draw_count == 0) {
		return;
	}

	const auto args_size = indexed ? sizeof(DrawIndexedIndirectArgs) : sizeof(DrawIndirectArgs);
	EXIT_NOT_IMPLEMENTED(stride_in_bytes < args_size);
	EXIT_IF(data_offset > UINT64_MAX - m_draw_indirect_args_base_addr);
	const auto args_begin = m_draw_indirect_args_base_addr + data_offset;
	const uint64_t args_span = uint64_t {draw_count - 1u} * stride_in_bytes + args_size;
	if (!GuestRange {args_begin, args_span}.Valid()) EXIT("PS5 multi indirect draw arguments range is invalid\n");
	PublishNativeGuestBackingRead(args_begin, args_span, true);

	uint64_t index_size = 0;
	if (indexed) {
		switch (m_index_type_and_size) {
			case 0: index_size = 2; break;
			case 1: index_size = 4; break;
			case 2: index_size = 1; break;
			default: EXIT("unknown index_type_and_size: %u\n", m_index_type_and_size);
		}
	}

	for (uint32_t i = 0; i < draw_count; i++) {
		const auto args_addr = args_begin + static_cast<uint64_t>(i) * stride_in_bytes;
		// A preceding draw can write a later argument through a shader binding.
		// Recheck that exact dependency before reading it into native CPU state.
		PublishNativeGuestBackingRead(args_addr, args_size, true);

		if (!indexed) {
			DrawIndirectArgs args {};
			std::memcpy(&args, reinterpret_cast<const void*>(args_addr), sizeof(args));
			PatchIndirectDrawOffsets(*this, registers, args.start_vertex_location,
			                         args.start_instance_location, 0);
			m_num_instances = args.instance_count;
			DrawIndexAuto({.vertex_count   = args.vertex_count_per_instance,
			               .instance_count = args.instance_count,
			               .first_vertex   = args.start_vertex_location,
			               .first_instance = args.start_instance_location,
			               .offset_source  = DrawOffsetSource::IndirectArgs});
			continue;
		}

		DrawIndexedIndirectArgs args {};
		std::memcpy(&args, reinterpret_cast<const void*>(args_addr), sizeof(args));
		PatchIndirectDrawOffsets(*this, registers, args.base_vertex_location,
		                         args.start_instance_location, args.start_index_location);

		auto* index_addr = reinterpret_cast<const void*>(
		    m_index_base_addr + static_cast<uint64_t>(args.start_index_location) * index_size);

		const uint32_t index_count =
		    (m_index_buffer_size != 0
		         ? std::min(args.index_count_per_instance, m_index_buffer_size)
		         : args.index_count_per_instance);
		if (GraphicsRunDebugDumpEnabled() && index_count != args.index_count_per_instance) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 64) {
				LOGF("\t DrawIndexIndirectMulti: clamped index_count from %" PRIu32 " to %" PRIu32
				     " using INDEX_BUFFER_SIZE\n",
				     args.index_count_per_instance, index_count);
			}
		}

		m_num_instances = args.instance_count;
		DrawIndex({.index_count    = index_count,
		           .index_addr     = index_addr,
		           .instance_count = args.instance_count,
		           .base_vertex    = static_cast<int32_t>(args.base_vertex_location),
		           .first_instance = args.start_instance_location,
		           .offset_source  = DrawOffsetSource::IndirectArgs});
	}
}

void CommandProcessor::DispatchDirect(uint32_t thread_group_x, uint32_t thread_group_y,
                                      uint32_t thread_group_z, uint32_t mode) {
	m_sh_ctx.SetCsWaveSize(Pm4::ComputeWaveSize(mode));

	uint32_t frame_num = 0;

	{
		frame_num = m_renderer.GetGpu().GetFrameNum();
		if (GraphicsRunDebugDumpEnabled()) {
			static std::atomic<uint32_t> log_count {0};
			if (log_count.fetch_add(1, std::memory_order_relaxed) < 1024) {
				const auto& cs = m_sh_ctx.GetCs().cs_regs;
				const auto& oa = m_ucfg.GetGdsOaCounter(m_ucfg.GetGdsOaState().GetIndex());
				LOGF("QueuePoint DispatchDirect: frame=%u submit=%" PRIu64
				     " groups=%ux%ux%u local=%ux%ux%u mode=0x%08" PRIx32 " wave=%u cs=0x%016" PRIx64
				     " oa_index=%u oa_enabled=%s oa_addr=0x%04" PRIx32 " oa_space=0x%08" PRIx32
				     "\n",
				     frame_num, m_submit_id, thread_group_x, thread_group_y, thread_group_z,
				     std::max(cs.num_thread_x, 1u), std::max(cs.num_thread_y, 1u),
				     std::max(cs.num_thread_z, 1u), mode, static_cast<uint32_t>(cs.wave_size),
				     cs.data_addr, m_ucfg.GetGdsOaState().GetIndex(),
				     oa.IsCounterEnabled() ? "true" : "false", oa.GetAddressBytes(),
				     oa.GetSpaceAvailable());
			}
		}

		m_renderer.GetRenderExecutor().DispatchDirect(m_submit_id, CurrentBuffer(), thread_group_x,
		                                              thread_group_y, thread_group_z, mode, IsAsyncComputeQueue());
	}
}

void CommandProcessor::DispatchIndirect(uint64_t args_addr, uint32_t mode) {
	EXIT_NOT_IMPLEMENTED(args_addr == 0 || (args_addr & 3u) != 0);
	if ((mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0) {
		if (!GuestRange {args_addr, sizeof(vk::DispatchIndirectCommand)}.Valid())
			EXIT("PS5 indirect dispatch arguments range is invalid\n");
		PublishNativeGuestBackingRead(args_addr, sizeof(vk::DispatchIndirectCommand), true);
		vk::DispatchIndirectCommand args {};
		std::memcpy(&args, reinterpret_cast<const void*>(args_addr), sizeof(args));
		DispatchDirect(args.x, args.y, args.z, mode);
		return;
	}
	m_sh_ctx.SetCsWaveSize(Pm4::ComputeWaveSize(mode));
	m_renderer.GetRenderExecutor().DispatchIndirect(m_submit_id, CurrentBuffer(), args_addr, mode,
	                                                IsAsyncComputeQueue());
}

void CommandProcessor::DrawIndexAuto(DrawAutoArgs args) {
	if (args.instance_count == 0) {
		args.instance_count = m_num_instances;
	}
	m_renderer.GetRenderExecutor().DrawAuto(m_submit_id, CurrentBuffer(), args);
}

void CommandProcessor::WaitFlipDone(uint32_t video_out_handle, uint32_t display_buffer_index) {
	BufferFlush();

	m_renderer.GetVideoOut().WaitFlipDone(static_cast<int>(video_out_handle),
	                                      static_cast<int>(display_buffer_index));
}

template <typename T>
void CommandProcessor::WriteAtEndOfPipe(uint32_t cache_policy, uint32_t event_write_dest,
                                        uint32_t eop_event_type, uint32_t cache_action,
                                        uint32_t event_index, uint32_t event_write_source,
                                        void* dst_gpu_addr, T value, uint32_t interrupt_selector,
                                        uint32_t interrupt_context_id) {
	static_assert(sizeof(T) == sizeof(uint32_t) || sizeof(T) == sizeof(uint64_t));

	auto& command = CurrentBuffer();

	if (GraphicsRunDebugDumpEnabled()) {
		const auto bits      = static_cast<unsigned>(sizeof(T) * 8u);
		const auto log_width = static_cast<int>(sizeof(T) * 2u);

		LOGF("CommandProcessor::WriteAtEndOfPipe%u()\n"
		     "\t cache_policy        = 0x%08" PRIx32 "\n"
		     "\t event_write_dest    = 0x%08" PRIx32 "\n"
		     "\t eop_event_type      = 0x%08" PRIx32 "\n"
		     "\t cache_action        = 0x%08" PRIx32 "\n"
		     "\t event_index         = 0x%08" PRIx32 "\n"
		     "\t event_write_source  = 0x%08" PRIx32 "\n"
		     "\t interrupt_selector  = 0x%08" PRIx32 "\n"
		     "\t interrupt_context   = 0x%08" PRIx32 "\n"
		     "\t dst_gpu_addr        = 0x%016" PRIx64 "\n"
		     "\t value               = 0x%0*" PRIx64 "\n",
		     bits, cache_policy, event_write_dest, eop_event_type, cache_action, event_index,
		     event_write_source, interrupt_selector, interrupt_context_id,
		     reinterpret_cast<uint64_t>(dst_gpu_addr), log_width, static_cast<uint64_t>(value));
	}

	EXIT_NOT_IMPLEMENTED(cache_policy != 0x00000000);
	EXIT_NOT_IMPLEMENTED(event_write_dest != 0x00000000);

	bool with_interrupt = false;
	switch (interrupt_selector) {
		case 0x00:
		case 0x03: with_interrupt = false; break;
		case 0x01:
			if (!IsAsyncComputeQueue()) {
				Sync::TriggerEopEventAtEndOfPipe(command, m_interrupt_event_id,
				                                 interrupt_context_id);
				return;
			}
			with_interrupt = true;
			break;
		case 0x02: with_interrupt = true; break;
		default: EXIT("unknown interrupt_selector\n");
	}

	auto write32 = [&](bool with_writeback) {
        PrepareNativeGuestBackingWrite(reinterpret_cast<uint64_t>(dst_gpu_addr), sizeof(uint32_t));
		auto* dst  = static_cast<uint32_t*>(dst_gpu_addr);
		auto  data = static_cast<uint32_t>(value);
		std::memcpy(dst, &data, sizeof(data));

		if (with_interrupt) {
			if (with_writeback) {
				Sync::WriteAtEndOfPipeWithInterruptWriteBack32(m_submit_id, command, dst,
				                                               data, m_interrupt_event_id,
				                                               interrupt_context_id);
			} else {
				Sync::WriteAtEndOfPipeWithInterrupt32(m_submit_id, command, dst, data,
				                                      m_interrupt_event_id, interrupt_context_id);
			}
		} else if (with_writeback) {
			Sync::WriteAtEndOfPipeWithWriteBack32(m_submit_id, command, dst, data);
		} else {
			Sync::WriteAtEndOfPipe32(m_submit_id, command, dst, data);
		}
	};

	switch (event_write_source) {
		case 0x01:
			if constexpr (sizeof(T) == sizeof(uint32_t)) {
				if (eop_event_type == 0x2f && cache_action == 0x00 && event_index == 0x06) {
					const uint64_t gds_size = uint64_t {value >> 16u} * sizeof(uint32_t);
					if (gds_size != 0) {
						const GuestRange destination {reinterpret_cast<uint64_t>(dst_gpu_addr), gds_size};
						if (!destination.Valid()) EXIT("PS5 GDS backing destination range is invalid\n");
						PrepareNativeGuestBackingWrite(destination.address, destination.size);
					}
					auto* dst = static_cast<uint32_t*>(dst_gpu_addr);
					SynchronizeGpu();
					Sync::ReadGds(*m_renderer.GetBufferCache().GetGdsBuffer(), dst, value & 0xffffu,
					              value >> 16u);
					Sync::WriteAtEndOfPipeGds32(m_submit_id, command, dst, value & 0xffffu,
					                            value >> 16u);
					if (with_interrupt) {
						m_renderer.TriggerInterrupt(m_interrupt_event_id, interrupt_context_id);
					}
					return;
				}
			} else if (eop_event_type == 0x04 && cache_action == 0x00 && event_index == 0x05) {
				write32(false);
				return;
			}
			break;
		case 0x02:
		case 0x04:
			if constexpr (sizeof(T) == sizeof(uint32_t)) {
				if (event_write_source == 0x02 && eop_event_type == 0x2f && event_index == 0x06) {
					switch (cache_action) {
						case 0x00: write32(false); return;
						case 0x38: write32(true); return;
						default: break;
					}
				}
			} else {
				if (event_write_source == 0x04) {
					value = Sync::ReadReferenceClock();
				}
				auto write64 = [&](bool with_writeback) {
                    PrepareNativeGuestBackingWrite(reinterpret_cast<uint64_t>(dst_gpu_addr), sizeof(uint64_t));
					auto* dst = static_cast<uint64_t*>(dst_gpu_addr);
					std::memcpy(dst, &value, sizeof(value));

					if (with_interrupt) {
						if (with_writeback) {
							Sync::WriteAtEndOfPipeWithInterruptWriteBack64(
							    m_submit_id, command, dst, value, m_interrupt_event_id,
							    interrupt_context_id);
						} else {
							Sync::WriteAtEndOfPipeWithInterrupt64(m_submit_id, command, dst,
							                                      value, m_interrupt_event_id,
							                                      interrupt_context_id);
						}
					} else if (with_writeback) {
						Sync::WriteAtEndOfPipeWithWriteBack64(m_submit_id, command, dst,
						                                      value);
					} else {
						Sync::WriteAtEndOfPipe64(m_submit_id, command, dst, value);
					}
				};

				switch (cache_action) {
					case 0x00:
						switch (eop_event_type) {
							case 0x04:
								if (event_index == 0x05) {
									write64(false);
									return;
								}
								break;
							case 0x14:
							case 0x28:
							case 0x2f:
								if (event_index == 0x00) {
									write64(false);
									return;
								}
								break;
							case 0x2b:
							case 0x2d:
							case 0x30:
								if (event_index == 0x00 && !with_interrupt) {
									write64(false);
									return;
								}
								break;
							default: break;
						}
						break;
					case 0x38:
						switch (eop_event_type) {
							case 0x04:
							case 0x14:
							case 0x28:
								if (((eop_event_type == 0x04 || eop_event_type == 0x28) &&
								     event_index == 0x05) ||
								    (event_index == 0x00)) {
									write64(true);
									return;
								}
								break;
							case 0x2b:
							case 0x2d:
								if (event_index == 0x00 && !with_interrupt) {
									write64(true);
									return;
								}
								break;
							case 0x2f:
								if (event_index == 0x06 && !with_interrupt) {
									write64(true);
									return;
								}
								break;
							default: break;
						}
						break;
					case 0x3b:
						if (eop_event_type == 0x04 && event_index == 0x05 && with_interrupt) {
							write64(true);
							return;
						}
						break;
					default: break;
				}
			}
			break;
		default: break;
	}

	EXIT("unknown event type\n");
}

void CommandProcessor::WriteAtEndOfPipe32(uint32_t cache_policy, uint32_t event_write_dest,
                                          uint32_t eop_event_type, uint32_t cache_action,
                                          uint32_t event_index, uint32_t event_write_source,
                                          void* dst_gpu_addr, uint32_t value,
                                          uint32_t interrupt_selector,
                                          uint32_t interrupt_context_id) {
	WriteAtEndOfPipe(cache_policy, event_write_dest, eop_event_type, cache_action, event_index,
	                 event_write_source, dst_gpu_addr, value, interrupt_selector,
	                 interrupt_context_id);
}

void CommandProcessor::WriteAtEndOfPipe64(uint32_t cache_policy, uint32_t event_write_dest,
                                          uint32_t eop_event_type, uint32_t cache_action,
                                          uint32_t event_index, uint32_t event_write_source,
                                          void* dst_gpu_addr, uint64_t value,
                                          uint32_t interrupt_selector,
                                          uint32_t interrupt_context_id) {
	WriteAtEndOfPipe(cache_policy, event_write_dest, eop_event_type, cache_action, event_index,
	                 event_write_source, dst_gpu_addr, value, interrupt_selector,
	                 interrupt_context_id);
}

void CommandProcessor::EmitGlobalBarrier() {
	Common::LockGuard lock(m_renderer.GetMutex());

	vk::MemoryBarrier2 barrier {};
	barrier.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
	barrier.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;

	vk::DependencyInfo dependency {};
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers    = &barrier;
	GetScheduler().EndRendering();
	CurrentBuffer().Handle().pipelineBarrier2(dependency);
}

void CommandProcessor::TriggerEopEventAtEndOfPipe(uint32_t interrupt_context_id) {
	Sync::TriggerEopEventAtEndOfPipe(CurrentBuffer(), m_interrupt_event_id, interrupt_context_id);
}

void CommandProcessor::TriggerEvent(uint32_t event_type, uint32_t event_index,
                                    uint64_t event_address) {
	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::TriggerEvent()\n"
		     "\t event_type  = 0x%08" PRIx32 "\n"
		     "\t event_index = 0x%08" PRIx32 "\n"
		     "\t address     = 0x%016" PRIx64 "\n",
		     event_type, event_index, event_address);
	}

	const auto valid_cache_event_index = event_index == 0x00000000 || event_index == 0x00000007;
	switch (event_type) {
		// CsPartialFlush, GsPartialFlush, PsPartialFlush.
		case 0x00000007:
		case 0x0000000f:
		case 0x00000010: EmitGlobalBarrier(); break;
		// CbDbDataWritebackInvalidate, CbDataWritebackInvalidate.
		case 0x00000016:
		case 0x00000031:
			if (!valid_cache_event_index) {
				EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type,
				     event_index);
			}
			EmitGlobalBarrier();
			break;
		// DbDataWritebackInvalidate, DbMetadataWritebackInvalidate, CbMetadataWritebackInvalidate.
		case 0x0000002a:
		case 0x0000002c:
		case 0x0000002e:
			if (!valid_cache_event_index) {
				EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type,
				     event_index);
			}
			EmitGlobalBarrier();
			break;
		case 0x0000000d:
		case 0x0000000e:
		case 0x00000012:
		case 0x00000017:
		case 0x00000018:
		case 0x00000019:
		case 0x0000001a:
		case 0x0000001b:
		case 0x00000038:
		case 0x0000003a:
			LOGF("\t temporary: ignoring unsupported event_write type 0x%08" PRIx32
			     ", index 0x%08" PRIx32 "\n",
			     event_type, event_index);
			break;
		case 0x00000039: {
			if (event_index != 0x00000001 || event_address == 0 || (event_address & 0x7u) != 0) {
				EXIT("invalid occlusion-counter dump: index=0x%08" PRIx32 ", address=0x%016" PRIx64
				     "\n",
				     event_index, event_address);
			}
			static std::once_flag warning_once;
			std::call_once(warning_once, [] {
				std::printf("Warning: game uses occlusion queries, which are currently treated as "
				            "always visible; GPU usage may be higher and FPS may be lower.\n");
			});

			// Until host occlusion queries are implemented, publish an always-visible result. The
			// PS5 layout contains one interleaved begin/end pair per DB, and bit 63 marks a result
			// ready.
			constexpr uint64_t ready_bit    = 1ull << 63u;
			constexpr uint64_t counter_mask = ready_bit - 1u;
            PrepareNativeGuestBackingWrite(event_address, 32 * sizeof(uint64_t));
			auto*              results      = reinterpret_cast<volatile uint64_t*>(event_address);
			const auto         value        = ready_bit | m_synthetic_occlusion_counter;
			for (uint32_t db = 0; db < 16u; db++) {
				results[db * 2u] = value;
			}
			m_synthetic_occlusion_counter = (m_synthetic_occlusion_counter + 1u) & counter_mask;
			break;
		}
		default:
			EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type, event_index);
	}
}

void CommandProcessor::Flip() {
	FlushPendingReleaseMem();
	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::Flip()\n");
	}

	auto& command = CurrentBuffer();
	auto request = Sync::PrepareVideoOutFlip(command, m_flip.handle, m_flip.index, m_flip.flip_mode,
	                                         m_flip.flip_arg);
	Sync::WriteAtEndOfPipeOnlyFlip(m_submit_id, command, m_flip.handle, m_flip.index,
	                               m_flip.flip_mode, m_flip.flip_arg, request);
	GetScheduler().Flush();
}

void CommandProcessor::Flip(void* dst_gpu_addr, uint32_t value) {
	FlushPendingReleaseMem();
	auto& command = CurrentBuffer();

	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::Flip()\n"
		     "\t dst_gpu_addr = 0x%016" PRIx64 "\n"
		     "\t value        = 0x%08" PRIx32 "\n",
		     reinterpret_cast<uint64_t>(dst_gpu_addr), value);
	}

	PrepareNativeGuestBackingWrite(reinterpret_cast<uint64_t>(dst_gpu_addr), sizeof(value));
	std::memcpy(dst_gpu_addr, &value, sizeof(value));
	auto request = Sync::PrepareVideoOutFlip(command, m_flip.handle, m_flip.index, m_flip.flip_mode,
	                                         m_flip.flip_arg);
	Sync::WriteAtEndOfPipeWithFlip32(m_submit_id, command, static_cast<uint32_t*>(dst_gpu_addr),
	                                 value, m_flip.handle, m_flip.index, m_flip.flip_mode,
	                                 m_flip.flip_arg, request);
	GetScheduler().Flush();
}

void CommandProcessor::FlipWithInterrupt(uint32_t eop_event_type, uint32_t cache_action,
                                         void* dst_gpu_addr, uint32_t value) {
	FlushPendingReleaseMem();
	auto& command = CurrentBuffer();

	if (GraphicsRunDebugDumpEnabled()) {
		LOGF("CommandProcessor::FlipWithInterrupt()\n"
		     "\t eop_event_type      = 0x%08" PRIx32 "\n"
		     "\t cache_action        = 0x%08" PRIx32 "\n"
		     "\t dst_gpu_addr        = 0x%016" PRIx64 "\n"
		     "\t value               = 0x%08" PRIx32 "\n",
		     eop_event_type, cache_action, reinterpret_cast<uint64_t>(dst_gpu_addr), value);
	}

	if (eop_event_type != 0x00000004 || cache_action != 0x00000038) {
		EXIT("unknown event type\n");
	}
	PrepareNativeGuestBackingWrite(reinterpret_cast<uint64_t>(dst_gpu_addr), sizeof(value));
	std::memcpy(dst_gpu_addr, &value, sizeof(value));
	auto request = Sync::PrepareVideoOutFlip(command, m_flip.handle, m_flip.index, m_flip.flip_mode,
	                                         m_flip.flip_arg);
	Sync::WriteAtEndOfPipeWithInterruptWriteBackFlip32(
	    m_submit_id, command, static_cast<uint32_t*>(dst_gpu_addr), value, m_flip.handle,
	    m_flip.index, m_flip.flip_mode, m_flip.flip_arg, request, m_interrupt_event_id);
	GetScheduler().Flush();
}

void CommandProcessor::PrepareCpuFlip(uint64_t request_id) {
	FlushPendingReleaseMem();
	auto& command = CurrentBuffer();
	if (g_current_processor != nullptr) {
		EXIT("invalid graphics-thread CPU flip preparation\n");
	}
	struct ProcessorScope {
		explicit ProcessorScope(CommandProcessor& processor) { g_current_processor = &processor; }
		~ProcessorScope() { g_current_processor = nullptr; }
	};
	ProcessorScope processor_scope(*this);

	m_renderer.GetVideoOut().PrepareFlip(request_id, command);
	GetScheduler().DeferPriorityOperation(
	    [this, request_id] { m_renderer.GetVideoOut().CompleteFlip(request_id); });
	GetScheduler().Flush();
}

void CommandProcessor::SynchronizeGpu() {
	GetScheduler().Finish();
}

bool GuestGpu::IsGpuThread() noexcept {
	return g_gpu_thread;
}

} // namespace Libs::Graphics
