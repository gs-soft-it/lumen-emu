// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>
#include <semaphore>
#include <span>
#include <thread>
#include <vector>
#include <queue>

#include "common/assert.h"
#if defined(LUMEN_GEN4_ENABLE_FEX_GUEST_CPU) && !defined(LUMEN_GEN4_DIRECT_FEX)
#include "core/fex/guest_memory_scope.h"
#endif
#include "common/slot_vector.h"
#include "common/types.h"
#include "common/unique_function.h"
#include "video_core/amdgpu/cb_db_extent.h"
#include "video_core/amdgpu/command_buffer_copy.h"
#include "video_core/amdgpu/regs.h"

namespace Vulkan {
class Rasterizer;
}

namespace Libraries::VideoOut {
struct VideoOutPort;
}

namespace AmdGpu {

struct Liverpool {
    static constexpr u32 GfxQueueId = 0u;
    static constexpr u32 NumGfxRings = 1u;     // actually 2, but HP is reserved by system software
    static constexpr u32 NumComputePipes = 7u; // actually 8, but #7 is reserved by system software
    static constexpr u32 NumQueuesPerPipe = 8u;
    static constexpr u32 NumComputeRings = NumComputePipes * NumQueuesPerPipe;
    static constexpr u32 NumTotalQueues = NumGfxRings + NumComputeRings;
    static_assert(NumTotalQueues < 64u); // need to fit into u64 bitmap for ffs

    enum ContextRegs : u32 {
        DbZInfo = 0xA010,
        CbColor0Base = 0xA318,
        CbColor1Base = 0xA327,
        CbColor2Base = 0xA336,
        CbColor3Base = 0xA345,
        CbColor4Base = 0xA354,
        CbColor5Base = 0xA363,
        CbColor6Base = 0xA372,
        CbColor7Base = 0xA381,
        CbColor0Cmask = 0xA31F,
        CbColor1Cmask = 0xA32E,
        CbColor2Cmask = 0xA33D,
        CbColor3Cmask = 0xA34C,
        CbColor4Cmask = 0xA35B,
        CbColor5Cmask = 0xA36A,
        CbColor6Cmask = 0xA379,
        CbColor7Cmask = 0xA388,
    };

    Regs regs{};
    std::array<CbDbExtent, NUM_COLOR_BUFFERS> last_cb_extent{};
    CbDbExtent last_db_extent{};

public:
    explicit Liverpool();
    ~Liverpool();

    void SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb);
    void SubmitAsc(u32 gnm_vqid, std::span<const u32> acb);

    void SubmitDone() noexcept {
        std::scoped_lock lk{submit_mutex};
        submit_done = true;
        submit_cv.notify_one();
    }

    void WaitGpuIdle() noexcept {
#if defined(LUMEN_GEN4_ENABLE_FEX_GUEST_CPU) && !defined(LUMEN_GEN4_DIRECT_FEX)
        Core::Fex::QuiescentHostWait wait;
#endif
        std::unique_lock lk{submit_mutex};
        submit_cv.wait(lk, [this] { return num_submits == 0; });
    }

    bool IsGpuIdle() const {
        return num_submits == 0;
    }

    void SetVoPort(Libraries::VideoOut::VideoOutPort* port) {
        vo_port = port;
    }

    void BindRasterizer(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
    }

    bool IsGpuThread() const {
        return std::this_thread::get_id() == gpu_id;
    }

    bool InGfxTask() const {
        return in_gfx_task.load(std::memory_order_relaxed);
    }

    void EnqueueCommand(auto&& func) {
        std::scoped_lock lk{submit_mutex};
        command_queue.emplace([this, callback = std::forward<decltype(func)>(func)]() mutable {
            ExecuteCommand(std::move(callback));
        });
        ++num_commands;
        submit_cv.notify_one();
    }

    template <bool wait_done = false>
    void SendCommand(auto&& func) {
        if (std::this_thread::get_id() == gpu_id) {
            ExecuteCommand(std::forward<decltype(func)>(func));
            return;
        }
        if constexpr (wait_done) {
#if defined(LUMEN_GEN4_ENABLE_FEX_GUEST_CPU) && !defined(LUMEN_GEN4_DIRECT_FEX)
            // Acquire before queue locks. The native caller retains the epoch
            // until the worker has published and released every guest pointer.
            Core::Fex::MemoryMutationScope publication;
            ASSERT_MSG(publication.Valid(), "GPU publication admission denied");
#endif
            std::binary_semaphore sem{0};
            {
                std::scoped_lock lk{submit_mutex};
                command_queue.emplace([this, &sem, &func
#if defined(LUMEN_GEN4_ENABLE_FEX_GUEST_CPU) && !defined(LUMEN_GEN4_DIRECT_FEX)
                                       , &publication
#endif
                                      ] {
                    {
#if defined(LUMEN_GEN4_ENABLE_FEX_GUEST_CPU) && !defined(LUMEN_GEN4_DIRECT_FEX)
                        Core::Fex::MemoryMutationScope::SynchronousDelegate delegate{publication};
#endif
                        ExecuteCommand(std::forward<decltype(func)>(func));
                    }
                    sem.release();
                });
                ++num_commands;
                submit_cv.notify_one();
            }
            sem.acquire();
        } else {
            EnqueueCommand(std::forward<decltype(func)>(func));
        }
    }

    void ReserveCopyBufferSpace() {
        // Compatibility with the host initialization API. Copies are task-owned.
    }

    inline ComputeProgram& GetCsRegs() {
        return mapped_queues[curr_qid].cs_state;
    }

    struct AscQueueInfo {
        static constexpr size_t Pm4BufferSize = 1024;
        VAddr map_addr;
        u32* read_addr;
        u32 ring_size_dw;
        u32 pipe_id;
        std::array<u32, Pm4BufferSize> tmp_packet;
        u32 tmp_dwords;
    };
    Common::SlotVector<AscQueueInfo> asc_queues{};

private:
    struct Task {
        struct promise_type {
            std::unique_ptr<CommandBufferCopy> command_buffers;

            auto get_return_object() {
                Task task{};
                task.handle = std::coroutine_handle<promise_type>::from_promise(*this);
                return task;
            }
            static constexpr std::suspend_always initial_suspend() noexcept {
                // We want the task to be suspended at start
                return {};
            }
            static constexpr std::suspend_always final_suspend() noexcept {
                return {};
            }
            void unhandled_exception() {
                try {
                    std::rethrow_exception(std::current_exception());
                } catch (const std::exception& e) {
                    UNREACHABLE_MSG("Unhandled exception: {}", e.what());
                }
            }
            void return_void() {}
            struct empty {};
            std::suspend_always yield_value(empty&&) {
                return {};
            }
        };

        using Handle = std::coroutine_handle<promise_type>;
        Handle handle;
    };

    Task ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb);
    Task ProcessCeUpdate(std::span<const u32> ccb);
    template <bool is_indirect = false>
    Task ProcessCompute(std::span<const u32> acb, u32 vqid);

    void ProcessCommands();
    void ExecuteCommand(Common::UniqueFunction<void> callback);
    u32 publication_depth{};
    void Process(std::stop_token stoken);

    struct GpuQueue {
        std::mutex m_access{};
        std::queue<Task::Handle> submits{};
        ComputeProgram cs_state{};
    };
    std::array<GpuQueue, NumTotalQueues> mapped_queues{};
    u32 num_mapped_queues{1u}; // GFX is always available

    VAddr indirect_args_addr{};
    u32 num_counter_pairs{};
    u64 pixel_counter{};

    struct ConstantEngine {
        void Reset() {
            ce_count = 0;
            de_count = 0;
            ce_compare_count = 0;
        }

        [[nodiscard]] u32 Diff() const {
            ASSERT_MSG(ce_count >= de_count, "DE counter is ahead of CE");
            return ce_count - de_count;
        }

        u32 ce_compare_count{};
        u32 ce_count{};
        u32 de_count{};
        static std::array<u8, 48_KB> constants_heap;
    } cblock{};

    Vulkan::Rasterizer* rasterizer{};
    Libraries::VideoOut::VideoOutPort* vo_port{};
    std::jthread process_thread{};
    std::atomic<u32> num_submits{};
    std::atomic<u32> num_commands{};
    std::atomic<bool> submit_done{};
    std::mutex submit_mutex;
    std::condition_variable_any submit_cv;
    std::queue<Common::UniqueFunction<void>> command_queue{};
    std::thread::id gpu_id;
    std::atomic<bool> in_gfx_task{false};
    s32 curr_qid{-1};
#ifdef ENABLE_LUMEN_HOST_RUNTIME
    u32 debug_last_opcode{};
    u64 debug_resume_count{};
#endif
};

} // namespace AmdGpu
