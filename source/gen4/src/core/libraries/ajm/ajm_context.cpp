// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/libraries/ajm/ajm.h"
#include "core/libraries/ajm/ajm_at9.h"
#include "core/libraries/ajm/ajm_aac.h"
#include "core/libraries/ajm/ajm_context.h"
#include "core/libraries/ajm/ajm_error.h"
#include "core/libraries/ajm/ajm_instance.h"
#include "core/libraries/ajm/ajm_instance_statistics.h"
#include "core/libraries/ajm/ajm_mp3.h"
#include "core/libraries/error_codes.h"

#include <span>
#include <utility>
#include "core/libraries/kernel/sync/host_wait.h"
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/guest_callback_scratch.h"
#include "core/fex/fex_guest_engine.h"
#endif

namespace Libraries::Ajm {

constexpr u32 ORBIS_AJM_WAIT_INFINITE = -1;
constexpr int INSTANCE_ID_MASK = 0x3FFF;

AjmContext::AjmContext() {
    worker_thread = std::jthread([this](std::stop_token stop) { this->WorkerThread(stop); });
}

AjmContext::~AjmContext() { Shutdown(); }

void AjmContext::Shutdown() {
    // No other shutdown caller may return while the worker still accesses members.
    Kernel::NativeHostWait([&] {
        std::scoped_lock shutdown{shutdown_mutex};
        if (closed.exchange(true)) return;
        worker_thread.request_stop();
        if (worker_thread.joinable()) worker_thread.join();
        std::unique_lock lock{batches_mutex};
        for (u32 id = 1; id < MaxBatches; ++id) {
            if (auto* batch = batches.Get(id)) {
                (*batch)->canceled = true;
                (*batch)->Complete();
            }
        }
    });
}

bool AjmContext::IsRegistered(AjmCodecType type) const {
    return registered_codecs[std::to_underlying(type)];
}

s32 AjmContext::BatchCancel(const u32 batch_id) {
    if (closed.load()) return ORBIS_AJM_ERROR_INVALID_CONTEXT;
    if (batch_id == 0 || batch_id >= MaxBatches) return ORBIS_AJM_ERROR_INVALID_BATCH;
    std::shared_ptr<AjmBatch> batch{};
    {
        std::shared_lock guard(batches_mutex);
        const auto p_batch = batches.Get(batch_id);
        if (p_batch == nullptr) {
            return ORBIS_AJM_ERROR_INVALID_BATCH;
        }
        batch = *p_batch;
    }

    if (batch->processed) {
        return ORBIS_OK;
    }

    bool expected = false;
    batch->canceled.compare_exchange_strong(expected, true);
    return ORBIS_OK;
}

s32 AjmContext::ModuleRegister(AjmCodecType type) {
    std::unique_lock lock{instances_mutex};
    if (closed.load()) return ORBIS_AJM_ERROR_INVALID_CONTEXT;
    if (type != AjmCodecType::At9Dec && type != AjmCodecType::Mp3Dec && type != AjmCodecType::M4aacDec)
        return ORBIS_AJM_ERROR_CODEC_NOT_SUPPORTED;
    if (std::to_underlying(type) >= NumAjmCodecs) {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }
    if (IsRegistered(type)) {
        return ORBIS_AJM_ERROR_CODEC_ALREADY_REGISTERED;
    }
    registered_codecs[std::to_underlying(type)] = true;
    return ORBIS_OK;
}

s32 AjmContext::ModuleUnregister(AjmCodecType type) {
    std::unique_lock lock{instances_mutex};
    if (closed.load()) return ORBIS_AJM_ERROR_INVALID_CONTEXT;
    if (type != AjmCodecType::At9Dec && type != AjmCodecType::Mp3Dec && type != AjmCodecType::M4aacDec)
        return ORBIS_AJM_ERROR_CODEC_NOT_SUPPORTED;
    const auto index = std::to_underlying(type);
    if (!registered_codecs[index]) return ORBIS_AJM_ERROR_CODEC_NOT_REGISTERED;
    if (live_instances[index] != 0) return ORBIS_AJM_ERROR_BUSY;
    registered_codecs[index] = false;
    return ORBIS_OK;
}

void AjmContext::WorkerThread(std::stop_token stop) {
    Common::SetCurrentThreadName("Lumen:AjmWorker");
    while (!stop.stop_requested()) {
        auto batch = batch_queue.PopWait(stop);
        if (batch != nullptr) {
            bool expected = false;
            batch->processed.compare_exchange_strong(expected, true);
            if (!batch->canceled) batch->error = ProcessBatch(batch->id, batch->jobs);
            batch->Complete();
        }
    }
}

s32 AjmContext::ProcessBatch(u32 id, std::span<AjmJob> jobs) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    Core::Fex::MemoryMutationScope publication;
    if (!publication.Valid()) return ORBIS_AJM_ERROR_INVALID_ADDRESS;
    const auto output = [&](const void* pointer, size_t size) {
        if (!pointer) return true;
        if (!Core::GuestCpu::IsMappedCallbackMemory(pointer, size, true)) return false;
        publication.Publish(reinterpret_cast<uintptr_t>(pointer), size);
        return true;
    };
    for (const auto& job : jobs) {
        const auto& p = job.output;
        for (const auto buffer : p.buffers)
            if (!buffer.empty() && !output(buffer.data(), buffer.size())) return ORBIS_AJM_ERROR_INVALID_ADDRESS;
        if (!p.p_result || !output(p.p_result, sizeof(*p.p_result)) ||
            !output(p.p_stream, sizeof(*p.p_stream)) || !output(p.p_format, sizeof(*p.p_format)) ||
            !output(p.p_memory, sizeof(*p.p_memory)) || !output(p.p_engine_per_codec, sizeof(*p.p_engine_per_codec)) ||
            !output(p.p_engine, sizeof(*p.p_engine)) || !output(p.p_gapless_decode, sizeof(*p.p_gapless_decode)) ||
            !output(p.p_mframe, sizeof(*p.p_mframe)) || (p.p_codec_info &&
                (!p.codec_info_size || !output(p.p_codec_info, p.codec_info_size))))
            return ORBIS_AJM_ERROR_INVALID_ADDRESS;
    }
#endif
    // Perform operation requested by control flags.
    for (auto& job : jobs) {
        LOG_TRACE(Lib_Ajm, "Processing job {} for instance {}. flags = {:#x}", id, job.instance_id,
                  job.flags.raw);

        if (job.instance_id == AJM_INSTANCE_STATISTICS) {
            AjmInstanceStatistics::Getinstance().ExecuteJob(job);
        } else {
            if (!job.instance) return ORBIS_AJM_ERROR_INVALID_INSTANCE;
            job.instance->ExecuteJob(job);
        }
    }
    return ORBIS_OK;
}

s32 AjmContext::BatchWait(const u32 batch_id, const u32 timeout, AjmBatchError* const batch_error) {
    if (closed.load()) return ORBIS_AJM_ERROR_INVALID_CONTEXT;
    if (batch_id == 0 || batch_id >= MaxBatches) return ORBIS_AJM_ERROR_INVALID_BATCH;
    std::shared_ptr<AjmBatch> batch{};
    {
        std::shared_lock guard(batches_mutex);
        const auto p_batch = batches.Get(batch_id);
        if (p_batch == nullptr) {
            return ORBIS_AJM_ERROR_INVALID_BATCH;
        }
        batch = *p_batch;
    }

    bool expected = false;
    if (!batch->waiting.compare_exchange_strong(expected, true)) {
        return ORBIS_AJM_ERROR_BUSY;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        const auto slice = timeout == ORBIS_AJM_WAIT_INFINITE ? std::chrono::milliseconds(25) :
            std::min(std::chrono::milliseconds(25), std::chrono::duration_cast<std::chrono::milliseconds>(std::max(deadline - now, std::chrono::steady_clock::duration::zero())));
        const bool finished = Kernel::NativeHostWait([&] { return batch->finished.try_acquire_for(slice); });
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
        Core::Fex::FlushPendingGuestOrbisSignal();
#endif
        if (finished) break;
        if (timeout != ORBIS_AJM_WAIT_INFINITE && std::chrono::steady_clock::now() >= deadline) {
            batch->waiting = false;
            return ORBIS_AJM_ERROR_IN_PROGRESS;
        }
    }

    {
        std::unique_lock guard(batches_mutex);
        batches.Destroy(batch_id);
    }

    if (batch_error) {
        if (!Kernel::ValidateWaitOutput(batch_error, sizeof(*batch_error))) return ORBIS_AJM_ERROR_INVALID_ADDRESS;
        *batch_error = AjmBatchError{.error_code = batch->error};
    }
    if (batch->canceled) return ORBIS_AJM_ERROR_CANCELLED;
    return batch->error;
}

int AjmContext::BatchStartBuffer(u8* p_batch, u32 batch_size, const int priority,
                                 AjmBatchError* batch_error, u32* out_batch_id) {
    if (closed.load()) return ORBIS_AJM_ERROR_INVALID_CONTEXT;
    if (!out_batch_id || !Kernel::ValidateWaitOutput(out_batch_id, sizeof(*out_batch_id)))
        return ORBIS_AJM_ERROR_INVALID_ADDRESS;
    if (!p_batch && batch_size != 0) return ORBIS_AJM_ERROR_INVALID_ADDRESS;
    if ((batch_size & 7) != 0) {
        LOG_ERROR(Lib_Ajm, "ORBIS_AJM_ERROR_MALFORMED_BATCH");
        return ORBIS_AJM_ERROR_MALFORMED_BATCH;
    }

    const auto batch_info = AjmBatch::FromBatchBuffer({p_batch, batch_size});
    {
        std::shared_lock lock{instances_mutex};
        for (auto& job : batch_info->jobs) {
            if (job.instance_id == AJM_INSTANCE_STATISTICS) continue;
            const auto index = job.instance_id & INSTANCE_ID_MASK;
            if (index == 0 || index >= MaxInstances) return ORBIS_AJM_ERROR_INVALID_INSTANCE;
            auto* instance = instances.Get(index);
            if (!instance || instance_codecs[index] != static_cast<AjmCodecType>(job.instance_id >> 14))
                return ORBIS_AJM_ERROR_INVALID_INSTANCE;
            if (job.output.p_codec_info) {
                size_t minimum{};
                switch (instance_codecs[index]) {
                case AjmCodecType::At9Dec: minimum = sizeof(AjmSidebandDecAt9CodecInfo); break;
                case AjmCodecType::Mp3Dec: minimum = sizeof(AjmSidebandDecMp3CodecInfo); break;
                case AjmCodecType::M4aacDec: minimum = sizeof(AjmSidebandDecM4aacCodecInfo); break;
                default: return ORBIS_AJM_ERROR_CODEC_NOT_SUPPORTED;
                }
                if (job.output.codec_info_size < minimum) return ORBIS_AJM_ERROR_MALFORMED_BATCH;
            }
            job.instance = *instance;
        }
    }
    std::optional<u32> batch_id;
    {
        std::unique_lock guard(batches_mutex);
        if (closed.load()) return ORBIS_AJM_ERROR_INVALID_CONTEXT;
        batch_id = batches.Create(batch_info);
    }
    if (!batch_id.has_value()) {
        return ORBIS_AJM_ERROR_OUT_OF_MEMORY;
    }
    batch_info->id = batch_id.value();

    if (!batch_info->jobs.empty()) {
        std::unique_lock guard{batches_mutex};
        if (closed.load() || !batch_queue.TryEmplace(batch_info)) {
            batches.Destroy(batch_id.value());
            return closed.load() ? ORBIS_AJM_ERROR_INVALID_CONTEXT : ORBIS_AJM_ERROR_OUT_OF_RESOURCES;
        }
    } else {
        // Empty batches are not submitted to the processor and are marked as finished
        batch_info->Complete();
    }

    *out_batch_id = batch_id.value();
    return ORBIS_OK;
}

s32 AjmContext::InstanceCreate(AjmCodecType codec_type, AjmInstanceFlags flags, u32* out_instance) {
    std::unique_lock lock{instances_mutex};
    if (closed.load()) return ORBIS_AJM_ERROR_INVALID_CONTEXT;
    if (!out_instance || !Kernel::ValidateWaitOutput(out_instance, sizeof(*out_instance)))
        return ORBIS_AJM_ERROR_INVALID_ADDRESS;
    if (codec_type >= AjmCodecType::Max) {
        return ORBIS_AJM_ERROR_INVALID_PARAMETER;
    }
    if (flags.version == 0) {
        return ORBIS_AJM_ERROR_WRONG_REVISION_FLAG;
    }
    if (!IsRegistered(codec_type)) {
        return ORBIS_AJM_ERROR_CODEC_NOT_REGISTERED;
    }
    std::optional<u32> opt_index;
    {
        opt_index = instances.Create(std::move(std::make_unique<AjmInstance>(codec_type, flags)));
    }
    if (!opt_index.has_value()) {
        return ORBIS_AJM_ERROR_OUT_OF_RESOURCES;
    }
    ++live_instances[std::to_underlying(codec_type)];
    instance_codecs[opt_index.value()] = codec_type;
    *out_instance = opt_index.value() | (static_cast<u32>(codec_type) << 14);

    LOG_INFO(Lib_Ajm, "instance = {}", *out_instance);
    return ORBIS_OK;
}

s32 AjmContext::InstanceDestroy(u32 instance_id) {
    std::unique_lock lock(instances_mutex);
    if (closed.load()) return ORBIS_AJM_ERROR_INVALID_CONTEXT;
    const u32 index = instance_id & INSTANCE_ID_MASK;
    if (index == 0 || index >= MaxInstances || !instances.Get(index) ||
        instance_codecs[index] != static_cast<AjmCodecType>(instance_id >> 14)) {
        return ORBIS_AJM_ERROR_INVALID_INSTANCE;
    }
    if (instances.Get(index)->use_count() > 1) return ORBIS_AJM_ERROR_BUSY;
    instances.Destroy(index);
    --live_instances[std::to_underlying(instance_codecs[index])];
    return ORBIS_OK;
}

} // namespace Libraries::Ajm
