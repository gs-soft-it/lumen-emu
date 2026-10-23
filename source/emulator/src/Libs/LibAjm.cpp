#include "Emulator/Common.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include "AjmCore.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("Ajm", 1, "Ajm", 1, 1);

namespace AjmHle {

namespace {

struct TelemetryCounters
{
	std::atomic<uint64_t> calls {0};
	std::atomic<uint64_t> failures {0};
};

struct TelemetryDecision
{
	uint64_t call    = 0;
	uint64_t failure = 0;
	bool     log     = false;
};

bool ShouldLogTelemetry(uint64_t count)
{
	return count != 0 && (count <= 3 || (count & (count - 1u)) == 0);
}

TelemetryDecision GetTelemetryDecision(TelemetryCounters* counters, bool failed)
{
	if (counters == nullptr || std::getenv("LUMEN_CUSTOM_LOG") == nullptr)
	{
		return {};
	}

	TelemetryDecision decision {};
	decision.call = counters->calls.fetch_add(1, std::memory_order_relaxed) + 1;
	if (failed)
	{
		decision.failure = counters->failures.fetch_add(1, std::memory_order_relaxed) + 1;
	}
	decision.log = ShouldLogTelemetry(decision.call) || ShouldLogTelemetry(decision.failure);
	return decision;
}

} // namespace

static int32_t LUMEN_SYSV_ABI sceAjmInitialize(int64_t reserved, uint32_t* context)
{
	PRINT_NAME();
	const int32_t            result = Ajm::GlobalRuntime().Initialize(reserved, context);
	static TelemetryCounters counters;
	const auto               decision = GetTelemetryDecision(&counters, result != Ajm::AJM_OK);
	if (decision.log)
	{
		const uint32_t context_value = result == Ajm::AJM_OK && context != nullptr ? *context : 0;
		std::printf("[AJM] initialize call=%llu reserved=%lld result=0x%08x context=0x%08x\n",
		            static_cast<unsigned long long>(decision.call), static_cast<long long>(reserved), static_cast<uint32_t>(result),
		            context_value);
	}
	return result;
}

static int32_t LUMEN_SYSV_ABI sceAjmModuleRegister(uint32_t context, uint32_t codec_type, int64_t reserved)
{
	PRINT_NAME();
	const int32_t            result = Ajm::GlobalRuntime().ModuleRegister(context, codec_type, reserved);
	static TelemetryCounters counters;
	const auto               decision = GetTelemetryDecision(&counters, result != Ajm::AJM_OK);
	if (decision.log)
	{
		std::printf("[AJM] module_register call=%llu context=0x%08x codec=%u reserved=%lld result=0x%08x\n",
		            static_cast<unsigned long long>(decision.call), context, codec_type, static_cast<long long>(reserved),
		            static_cast<uint32_t>(result));
	}
	return result;
}

static void* LUMEN_SYSV_ABI sceAjmBatchJobControlBufferRa(void* batch, uint32_t instance_id, uint64_t flags, const void* sideband_input,
                                                         size_t sideband_input_size, void* sideband_output, size_t sideband_output_size,
                                                         const void* return_address)
{
	PRINT_NAME();
	void* result = Ajm::BatchJobControlBufferRa(batch, instance_id, flags, sideband_input, sideband_input_size, sideband_output,
	                                            sideband_output_size, return_address);
	static TelemetryCounters counters;
	const auto               decision = GetTelemetryDecision(&counters, result == nullptr);
	if (decision.log)
	{
		std::printf("[AJM] control_builder call=%llu instance=0x%08x flags=0x%016llx side_in=%zu side_out=%zu success=%u\n",
		            static_cast<unsigned long long>(decision.call), instance_id, static_cast<unsigned long long>(flags),
		            sideband_input_size, sideband_output_size, result != nullptr ? 1u : 0u);
	}
	return result;
}

static void* LUMEN_SYSV_ABI sceAjmBatchJobRunBufferRa(void* batch, uint32_t instance_id, uint64_t flags, const void* input,
                                                     size_t input_size, void* output, size_t output_size, void* sideband_output,
                                                     size_t sideband_output_size, const void* return_address)
{
	PRINT_NAME();
	void* result = Ajm::BatchJobRunBufferRa(batch, instance_id, flags, input, input_size, output, output_size, sideband_output,
	                                        sideband_output_size, return_address);
	static TelemetryCounters counters;
	const auto               decision = GetTelemetryDecision(&counters, result == nullptr);
	if (decision.log)
	{
		std::printf("[AJM] run_builder call=%llu instance=0x%08x flags=0x%016llx input=%zu output=%zu side_out=%zu success=%u\n",
		            static_cast<unsigned long long>(decision.call), instance_id, static_cast<unsigned long long>(flags), input_size,
		            output_size, sideband_output_size, result != nullptr ? 1u : 0u);
	}
	return result;
}

// This clean-room implementation currently supports the contiguous-buffer ABI
// used by the title. Split-buffer jobs stay explicit instead of reporting a
// successful no-op.
static void* LUMEN_SYSV_ABI sceAjmBatchJobRunSplitBufferRa(void* /*batch*/, uint32_t /*instance_id*/, uint64_t /*flags*/,
                                                          const void* /*input_buffers*/, size_t /*input_count*/, void* /*output_buffers*/,
                                                          size_t /*output_count*/, void* /*sideband_output*/,
                                                          size_t /*sideband_output_size*/, const void* /*return_address*/)
{
	PRINT_NAME();
	return nullptr;
}

static int32_t LUMEN_SYSV_ABI sceAjmBatchStartBuffer(uint32_t context, uint8_t* batch, uint32_t batch_size, int32_t priority,
                                                    Ajm::BatchError* batch_error, uint32_t* batch_id)
{
	PRINT_NAME();
	const int32_t            result = Ajm::GlobalRuntime().BatchStart(context, batch, batch_size, priority, batch_error, batch_id);
	static TelemetryCounters counters;
	const auto               decision = GetTelemetryDecision(&counters, result != Ajm::AJM_OK);
	if (decision.log)
	{
		const uint32_t id          = result == Ajm::AJM_OK && batch_id != nullptr ? *batch_id : 0;
		const int32_t  error_code  = batch_error != nullptr ? batch_error->error_code : 0;
		const uint32_t command_off = batch_error != nullptr ? batch_error->cmd_offset : 0;
		std::printf(
		    "[AJM] batch_start call=%llu context=0x%08x size=%u priority=%d result=0x%08x batch=0x%08x error=0x%08x command_offset=0x%x\n",
		    static_cast<unsigned long long>(decision.call), context, batch_size, priority, static_cast<uint32_t>(result), id,
		    static_cast<uint32_t>(error_code), command_off);
	}
	return result;
}

static int32_t LUMEN_SYSV_ABI sceAjmBatchWait(uint32_t context, uint32_t batch_id, uint32_t timeout, Ajm::BatchError* batch_error)
{
	PRINT_NAME();
	const int32_t            result = Ajm::GlobalRuntime().BatchWait(context, batch_id, timeout, batch_error);
	static TelemetryCounters counters;
	const auto               decision = GetTelemetryDecision(&counters, result != Ajm::AJM_OK);
	if (decision.log)
	{
		const int32_t  error_code  = batch_error != nullptr ? batch_error->error_code : 0;
		const uint32_t command_off = batch_error != nullptr ? batch_error->cmd_offset : 0;
		std::printf("[AJM] batch_wait call=%llu context=0x%08x batch=0x%08x timeout=%u result=0x%08x error=0x%08x command_offset=0x%x\n",
		            static_cast<unsigned long long>(decision.call), context, batch_id, timeout, static_cast<uint32_t>(result),
		            static_cast<uint32_t>(error_code), command_off);
	}
	return result;
}

static void LUMEN_SYSV_ABI sceAjmBatchErrorDump(const void* batch, const Ajm::BatchError* batch_error)
{
	PRINT_NAME();
	Ajm::BatchErrorDump(batch, batch_error);
}

static int32_t LUMEN_SYSV_ABI sceAjmFinalize(uint32_t context)
{
	PRINT_NAME();
	return Ajm::GlobalRuntime().Finalize(context);
}

static int32_t LUMEN_SYSV_ABI sceAjmInstanceCreate(uint32_t context, uint32_t codec_type, uint64_t flags, uint32_t* instance_id)
{
	PRINT_NAME();
	const int32_t            result = Ajm::GlobalRuntime().InstanceCreate(context, codec_type, flags, instance_id);
	static TelemetryCounters counters;
	const auto               decision = GetTelemetryDecision(&counters, result != Ajm::AJM_OK);
	if (decision.log)
	{
		const uint32_t id = result == Ajm::AJM_OK && instance_id != nullptr ? *instance_id : 0;
		std::printf("[AJM] instance_create call=%llu context=0x%08x codec=%u flags=0x%016llx result=0x%08x instance=0x%08x\n",
		            static_cast<unsigned long long>(decision.call), context, codec_type, static_cast<unsigned long long>(flags),
		            static_cast<uint32_t>(result), id);
	}
	return result;
}

static int32_t LUMEN_SYSV_ABI sceAjmInstanceDestroy(uint32_t context, uint32_t instance_id)
{
	PRINT_NAME();
	return Ajm::GlobalRuntime().InstanceDestroy(context, instance_id);
}

static int32_t LUMEN_SYSV_ABI sceAjmModuleUnregister(uint32_t context, uint32_t codec_type)
{
	PRINT_NAME();
	return Ajm::GlobalRuntime().ModuleUnregister(context, codec_type);
}

} // namespace AjmHle

LIB_DEFINE(InitAjm_1)
{
	LIB_FUNC("dl+4eHSzUu4", AjmHle::sceAjmInitialize);
	LIB_FUNC("Q3dyFuwGn64", AjmHle::sceAjmModuleRegister);
	LIB_FUNC("dmDybN--Fn8", AjmHle::sceAjmBatchJobControlBufferRa);
	LIB_FUNC_GP_ARITY("ElslOCpOIns", AjmHle::sceAjmBatchJobRunBufferRa, 10);
	LIB_FUNC_GP_ARITY("7jdAXK+2fMo", AjmHle::sceAjmBatchJobRunSplitBufferRa, 10);
	LIB_FUNC("fFFkk0xfGWs", AjmHle::sceAjmBatchStartBuffer);
	LIB_FUNC("-qLsfDAywIY", AjmHle::sceAjmBatchWait);
	LIB_FUNC("WfAiBW8Wcek", AjmHle::sceAjmBatchErrorDump);
	LIB_FUNC("MHur6qCsUus", AjmHle::sceAjmFinalize);
	LIB_FUNC("AxoDrINp4J8", AjmHle::sceAjmInstanceCreate);
	LIB_FUNC("RbLbuKv8zho", AjmHle::sceAjmInstanceDestroy);
	LIB_FUNC("Wi7DtlLV+KI", AjmHle::sceAjmModuleUnregister);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
