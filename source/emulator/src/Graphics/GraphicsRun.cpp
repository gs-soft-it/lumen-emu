#include "Emulator/Graphics/GraphicsRun.h"

#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/LinkList.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Vector.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/AsyncJob.h"
#include "Emulator/Graphics/DepthSurfaceImport.h"
#include "Emulator/Graphics/EopCpuProofResume.h"
#include "Lumen/Core/MagicEnum.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/HardwareColorMetadata.h"
#include "Emulator/Graphics/GuestWriteDestination.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/HostPolling.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderSourceRetry.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Profiler.h"
#include "../../../cpu/runtime/MemoryHandoff.h"
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && defined(LUMEN_ANDROID_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME))
#include "../../../cpu/runtime/GuestCpuRuntime.h"
#endif
#include "../../../../android/app/src/main/cpp/CriticalPathTrace.h"

#include <cstdlib>
#include "Emulator/Config.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <vector>

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
#include "../../../../android/app/src/main/cpp/AndroidFexCoreMutationBarrier.h"
#endif

#ifdef LUMEN_EMU_ENABLED

#define LUMEN_HW_CTX_PARSER_ARGS                                                                                                            \
	[[maybe_unused]] CommandProcessor *cp, uint32_t cmd_id, [[maybe_unused]] uint32_t cmd_offset, const uint32_t *buffer,                  \
	    [[maybe_unused]] uint32_t dw
#define LUMEN_HW_UC_PARSER_ARGS                                                                                                             \
	[[maybe_unused]] CommandProcessor *cp, uint32_t cmd_id, [[maybe_unused]] uint32_t cmd_offset, const uint32_t *buffer,                  \
	    [[maybe_unused]] uint32_t dw
#define LUMEN_HW_SH_PARSER_ARGS                                                                                                             \
	[[maybe_unused]] CommandProcessor *cp, uint32_t cmd_id, [[maybe_unused]] uint32_t cmd_offset, const uint32_t *buffer,                  \
	    [[maybe_unused]] uint32_t dw
#define LUMEN_HW_CTX_INDIRECT_ARGS CommandProcessor *cp, [[maybe_unused]] uint32_t cmd_offset, uint32_t value
#define LUMEN_HW_UC_INDIRECT_ARGS  CommandProcessor *cp, [[maybe_unused]] uint32_t cmd_offset, uint32_t value
#define LUMEN_HW_SH_INDIRECT_ARGS  CommandProcessor *cp, [[maybe_unused]] uint32_t cmd_offset, uint32_t value

#define LUMEN_HW_CTX_PARSER(f) static uint32_t f(LUMEN_HW_CTX_PARSER_ARGS)
#define LUMEN_HW_UC_PARSER(f)  static uint32_t f(LUMEN_HW_UC_PARSER_ARGS)
#define LUMEN_HW_SH_PARSER(f)  static uint32_t f(LUMEN_HW_SH_PARSER_ARGS)

#define LUMEN_CP_OP_PARSER_ARGS                                                                                                             \
	[[maybe_unused]] CommandProcessor *cp, [[maybe_unused]] uint32_t cmd_id, [[maybe_unused]] const uint32_t *buffer,                      \
	    [[maybe_unused]] uint32_t dw, [[maybe_unused]] uint32_t num_dw
#define LUMEN_CP_OP_PARSER(f) static uint32_t f(LUMEN_CP_OP_PARSER_ARGS)

namespace Lumen::Libs::Graphics {

enum class CommandStreamKind : uint8_t
{
	Unknown,
	Draw,
	Const,
	Compute,
	Indirect,
};

enum class ShaderUserStage : uint8_t
{
	Vertex,
	Pixel,
	Compute,
	Geometry,
};

static bool command_provenance_enabled()
{
	static const bool enabled = []()
	{
		const char* value = std::getenv("LUMEN_GFX_COMMAND_PROVENANCE");
		return value != nullptr && value[0] != '\0' && value[0] != '0';
	}();
	return enabled;
}

static bool exact_one_environment_value(const char* value)
{
	return value != nullptr && value[0] == '1' && value[1] == '\0';
}

static bool gpu_owned_preparation_enabled()
{
	static const bool enabled = [] {
		const bool selected = exact_one_environment_value(std::getenv("LUMEN_GPU_OWNED_PREPARATION"));
		if (selected) std::fputs("[GPU_OWNED_PREPARATION_CONFIG] selected=1 owned-analysis-only=1 per-range-claims=0\n", stderr);
		return selected;
	}();
	return enabled;
}

static bool gpu_range_execution_enabled()
{
	static const bool enabled = [] {
		const bool selected = exact_one_environment_value(std::getenv("LUMEN_GPU_RANGE_EXECUTION"));
		if (selected) std::fputs("[GPU_RANGE_EXECUTION_CONFIG] selected=1 physical-claims=1 native-mapping-pin=1\n", stderr);
		return selected;
	}();
	return enabled;
}

static bool gpu_ready_counter_wait_enabled()
{
	static const bool enabled = [] {
		const bool selected = exact_one_environment_value(std::getenv("LUMEN_GPU_READY_COUNTER_WAIT"));
		if (selected) std::fputs("[GPU_READY_COUNTER_WAIT_CONFIG] selected=1 predicate-before-handoff=1\n", stderr);
		return selected;
	}();
	return enabled;
}

static bool gpu_ready_memory_wait_enabled()
{
	static const bool enabled = [] {
		const bool selected = exact_one_environment_value(std::getenv("LUMEN_GPU_READY_MEMORY_WAIT"));
		if (selected) std::fputs("[GPU_READY_MEMORY_WAIT_CONFIG] selected=1 exclusive/clean/lease=required\n", stderr);
		return selected;
	}();
	return enabled;
}

static constexpr bool hle_d32_default_for_target()
{
#if ((LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID) && \
     LUMEN_ABI == LUMEN_ABI_ARM64_V8A) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	return true;
#else
	return false;
#endif
}

static bool hle_d32_default_environment_value(const char* value, bool default_enabled)
{
	return value == nullptr ? default_enabled : exact_one_environment_value(value);
}

static bool experimental_hle_d32_default_enabled()
{
	// Keep the existing selector as an explicit rollback of the adopted default.
	static const bool enabled = hle_d32_default_environment_value(std::getenv("LUMEN_EXPERIMENTAL_GFX_HLE_D32_DEFAULT"),
	                                                            hle_d32_default_for_target());
	return enabled;
}

static bool experimental_acquire_mem_08c40000_enabled()
{
	static const bool enabled = exact_one_environment_value(std::getenv("LUMEN_EXPERIMENTAL_GFX_ACQUIRE_MEM_08C40000"));
	return enabled;
}

static bool experimental_acquire_mem_db_only_enabled()
{
	static const bool enabled = exact_one_environment_value(std::getenv("LUMEN_EXPERIMENTAL_GFX_ACQUIRE_MEM_DB_ONLY"));
	return enabled;
}

static bool experimental_cb_meta_event_enabled()
{
	static const bool enabled = exact_one_environment_value(std::getenv("LUMEN_EXPERIMENTAL_GFX_CB_META_EVENT"));
	return enabled;
}

static bool increment_de_gpu_wait_enabled()
{
	// Keep the legacy drain by default until the asynchronous DE path has a
	// comparable runtime qualification. "0" advances DE after submission;
	// explicit DE-diff waits, EOP and label completion retain their fences.
	static const bool enabled = [] {
		const bool value = hle_d32_default_environment_value(std::getenv("LUMEN_GFX_INCREMENT_DE_WAIT"), true);
		std::fprintf(stderr, "[GFX_INCREMENT_DE_WAIT] enabled=%u processor_buffers=%u\n", value ? 1u : 0u,
		    VulkanCommandPool::PROCESSOR_BUFFER_COUNT);
		return value;
	}();
	return enabled;
}

static bool HasUnsupportedColorMetadata(const HW::Context& ctx, uint32_t* first_slot = nullptr)
{
	const auto verdict = HW::ClassifyColorMetadata(ctx);
	if (verdict.profile != HW::ColorMetadataProfile::Unsupported) return false;
	if (first_slot != nullptr) *first_slot = verdict.first_slot;
	return true;
}

static bool pm4_cb_meta_event_supported(uint32_t control, bool candidate_enabled, uint32_t interrupt_event_id,
                                        const HW::Context& ctx)
{
	return candidate_enabled && interrupt_event_id == 64 && control == 0x0000072eu && !HasUnsupportedColorMetadata(ctx);
}

static constexpr uint32_t hle_draw_reset_polygon_offset_format(bool enabled)
{
	// Diagnostic hypothesis for the HLE draw initializer, not a proven console default.
	return enabled ? 0x1e9u : 0u;
}

static void apply_hle_draw_reset_polygon_offset_format(HW::Context* ctx, bool enabled)
{
	if (enabled) ctx->SetPolygonOffsetRegister(0, hle_draw_reset_polygon_offset_format(enabled));
}

static bool root_command_ownership_enabled()
{
	static const bool enabled = exact_one_environment_value(std::getenv("LUMEN_GFX_ROOT_COMMAND_OWNERSHIP"));
	return enabled;
}

static bool root_command_ownership_configuration_valid(bool ownership, bool provenance)
{
	return !ownership || !provenance;
}

static constexpr uint32_t ROOT_COMMAND_OWNERSHIP_MAX_DWORDS = 1024u * 1024u;
static constexpr uint64_t ROOT_COMMAND_OWNERSHIP_MAX_TOTAL_DWORDS = 8u * 1024u * 1024u;
static constexpr uint32_t PM4_DISPATCH_DIRECT_COMPUTE_HEADER      = LUMEN_PM4(5, Pm4::IT_DISPATCH_DIRECT, 0) | 2u;

static bool pm4_context_control_packet_supported(uint32_t cmd_id, const uint32_t* body, uint32_t available_dw)
{
	return cmd_id == LUMEN_PM4(3, Pm4::IT_CONTEXT_CONTROL, 0) && body != nullptr && available_dw >= 3 && body[0] == 0x80000000u &&
	       body[1] == 0x80000000u;
}

static bool pm4_clear_state_packet_supported(uint32_t cmd_id, const uint32_t* body, uint32_t available_dw)
{
	return cmd_id == LUMEN_PM4(2, Pm4::IT_CLEAR_STATE, 0) && body != nullptr && available_dw >= 2 && body[0] == 0;
}

enum class Pm4AcquireMemGlobalWriteBackProfile : uint8_t
{
	Unsupported,
	Base,
	Observed08c40000,
};

static constexpr Pm4AcquireMemGlobalWriteBackProfile pm4_acquire_mem_global_writeback_profile(uint32_t cache_action)
{
	switch (cache_action)
	{
		case 0x00c40000u: return Pm4AcquireMemGlobalWriteBackProfile::Base;
		case 0x08c40000u: return Pm4AcquireMemGlobalWriteBackProfile::Observed08c40000;
		default: return Pm4AcquireMemGlobalWriteBackProfile::Unsupported;
	}
}

struct Pm4AcquireMemGlobalWriteBackPacket
{
	uint32_t cmd_id          = 0;
	uint32_t engine_selector = 0;
	uint32_t cache_action    = 0;
	uint64_t size_lo         = 0;
	uint32_t size_hi         = 0;
	uint64_t base_lo         = 0;
	uint32_t base_hi         = 0;
	uint32_t poll            = 0;
	uint32_t gcr_cntl        = 0;
};

static bool pm4_decode_acquire_mem(uint32_t cmd_id, const uint32_t* body, uint32_t available_dw,
                                   Pm4AcquireMemGlobalWriteBackPacket* packet)
{
	if (packet == nullptr || body == nullptr || (cmd_id != 0xc0055800u && cmd_id != 0xc0061050u) ||
	    available_dw < LUMEN_PM4_LEN(cmd_id)) return false;
	const Pm4AcquireMemGlobalWriteBackPacket decoded = {
	    cmd_id, body[0] >> 31u, body[0] & 0x7fffffffu, body[1], body[2], body[3], body[4], body[5],
	    cmd_id == 0xc0061050u ? body[6] : 0u,
	};
	*packet = decoded;
	return true;
}

static constexpr bool pm4_acquire_mem_db_only_supported(const Pm4AcquireMemGlobalWriteBackPacket& packet,
                                                        bool candidate_enabled, uint32_t interrupt_event_id)
{
	if (!candidate_enabled || interrupt_event_id != 64 || packet.cmd_id != 0xc0055800u || packet.engine_selector != 1 ||
	    packet.cache_action != 0x04004000u || packet.poll != 10 || packet.gcr_cntl != 0 || packet.base_hi != 0 ||
	    packet.size_hi != 0 || packet.base_lo == 0 || packet.base_lo > UINT32_MAX || packet.size_lo == 0 ||
	    packet.size_lo > UINT32_MAX) return false;
	const uint64_t base = packet.base_lo << 8u;
	const uint64_t size = packet.size_lo << 8u;
	return size <= (uint64_t {1} << 40u) - base;
}

static constexpr bool pm4_acquire_mem_global_writeback_supported(const Pm4AcquireMemGlobalWriteBackPacket& packet,
                                                                 bool observed_profile_enabled)
{
	const auto profile = pm4_acquire_mem_global_writeback_profile(packet.cache_action);
	if (profile == Pm4AcquireMemGlobalWriteBackProfile::Unsupported ||
	    (packet.cmd_id != 0xc0055800u && packet.cmd_id != 0xc0061050u) ||
	    (profile == Pm4AcquireMemGlobalWriteBackProfile::Observed08c40000 &&
	     (!observed_profile_enabled || packet.cmd_id != 0xc0055800u || packet.engine_selector != 1)))
	{
		return false;
	}

	const uint32_t target_mask     = packet.cache_action & 0x00007fc0u;
	const uint32_t extended_action = packet.cache_action & 0x2e000000u;
	const uint32_t action =
	    ((packet.cache_action & 0x00c00000u) >> 0x12u) | ((packet.cache_action & 0x00058000u) >> 0xfu);
	const uint32_t expected_extended_action =
	    profile == Pm4AcquireMemGlobalWriteBackProfile::Base ? 0x00000000u : 0x08000000u;
	return target_mask == 0 && extended_action == expected_extended_action && action == 0x38u && packet.size_lo == 1 &&
	       packet.size_hi == 0 && packet.base_lo == 0 && packet.base_hi == 0 && packet.poll == 10;
}

enum class Pm4AcquireMemGlobalWriteBackStep : uint8_t
{
	Unsupported,
	MemoryBarrier,
	WriteBack,
};

struct Pm4AcquireMemGlobalWriteBackPlan
{
	Pm4AcquireMemGlobalWriteBackProfile profile = Pm4AcquireMemGlobalWriteBackProfile::Unsupported;
	std::array<Pm4AcquireMemGlobalWriteBackStep, 2> steps = {
	    Pm4AcquireMemGlobalWriteBackStep::Unsupported,
	    Pm4AcquireMemGlobalWriteBackStep::Unsupported,
	};
};

static constexpr Pm4AcquireMemGlobalWriteBackPlan pm4_acquire_mem_global_writeback_plan(
    const Pm4AcquireMemGlobalWriteBackPacket& packet, bool observed_profile_enabled)
{
	if (!pm4_acquire_mem_global_writeback_supported(packet, observed_profile_enabled))
	{
		return {};
	}
	return {
	    pm4_acquire_mem_global_writeback_profile(packet.cache_action),
	    {Pm4AcquireMemGlobalWriteBackStep::MemoryBarrier, Pm4AcquireMemGlobalWriteBackStep::WriteBack},
	};
}

struct Pm4PixelDoneWrite32
{
	uint64_t address = 0;
	uint32_t value = 0;
};

// Only the documented graphics PS_DONE immediate-data profile is new here.
// Validate raw words so reserved EOS bits cannot become legacy EOP fields.
static bool pm4_decode_pixel_done_write32(uint32_t header, const uint32_t* body, uint32_t available_dw,
	                                     bool graphics_processor, Pm4PixelDoneWrite32* output)
{
	if (!graphics_processor || body == nullptr || output == nullptr || available_dw < 5 || header != 0xc0034800u ||
	    body[0] != 0x00000630u || (body[2] & 0xffff0000u) != 0x40000000u || (body[1] & 3u) != 0) return false;
	const uint64_t address = body[1] | (static_cast<uint64_t>(body[2] & 0xffffu) << 32u);
	if (address == 0) return false;
	*output = {address, body[3]};
	return true;
}

enum class Pm4EventWriteEopCompletionProfile : uint8_t
{
	Unsupported,
	InterruptOnly,
	ReleaseMemWriteBack32Interrupt,
	ReleaseMemWriteBackClock64Interrupt,
};

struct Pm4EventWriteEopPacket
{
	uint32_t packet_id             = 0;
	uint32_t cache_policy          = 0;
	uint32_t event_write_dest      = 0;
	uint32_t event_type            = 0;
	uint32_t cache_action          = 0;
	uint32_t event_index           = 0;
	uint32_t int_selector          = 0;
	uint32_t data_selector         = 0;
	uint32_t control_selector_byte = 0;
	uint32_t event_control_word    = 0;
	uint32_t address_control_word  = 0;
};

static constexpr Pm4EventWriteEopCompletionProfile pm4_event_write_eop_completion_profile(const Pm4EventWriteEopPacket& packet)
{
	if (packet.packet_id == 0xc0044700u && packet.cache_policy == 0 && packet.event_write_dest == 0 && packet.event_type == 4 &&
	    packet.cache_action == 0 && packet.event_index == 5 && packet.int_selector == 1 && packet.data_selector == 0 &&
	    packet.control_selector_byte == 0x01u && packet.event_control_word == 0x00000504u &&
	    (packet.address_control_word & 0xffff0000u) == 0x01000000u)
	{
		return Pm4EventWriteEopCompletionProfile::InterruptOnly;
	}
	// Native RELEASE_MEM cache-flush/invalidate timestamp profiles observed on
	// PS4. The exact data selector distinguishes a 32-bit value from a 64-bit clock.
	if (packet.packet_id == 0xc0054902u && packet.cache_policy == 0 && packet.event_write_dest == 0 &&
	    packet.event_type == 0x14 && packet.cache_action == 0 && packet.event_index == 5 && packet.int_selector == 2 &&
	    packet.event_control_word == 0x00000514u)
	{
		if (packet.data_selector == 1 && packet.control_selector_byte == 0x22u && packet.address_control_word == 0x22000000u)
			return Pm4EventWriteEopCompletionProfile::ReleaseMemWriteBack32Interrupt;
		if (packet.data_selector == 3 && packet.control_selector_byte == 0x62u && packet.address_control_word == 0x62000000u)
			return Pm4EventWriteEopCompletionProfile::ReleaseMemWriteBackClock64Interrupt;
	}
	return Pm4EventWriteEopCompletionProfile::Unsupported;
}

static constexpr bool pm4_event_write_eop_virtual_clock_profile(const Pm4EventWriteEopPacket& packet,
    uint64_t destination, bool graphics_cp)
{
	return graphics_cp && packet.packet_id == 0xc0044700u && packet.event_control_word == 0x00038504u &&
	       (packet.address_control_word & 0xffff0000u) == 0x80000000u && packet.cache_policy == 0 &&
	       packet.event_write_dest == 0 && packet.event_type == 4 && packet.cache_action == 0x38 &&
	       packet.event_index == 5 && packet.int_selector == 0 && packet.data_selector == 4 &&
	       packet.control_selector_byte == 0x80 && destination != 0 && destination % 8 == 0;
}

static constexpr bool pm4_event_write_eop_immediate32_checkpoint_profile(const Pm4EventWriteEopPacket& packet,
    uint64_t destination, bool graphics_cp)
{
	// An explicitly selected local completion model, not general hardware
	// cache/TCL1 qualification. DATA1 is immediate32 and INT0 requests no IRQ.
	const bool exact_event = (packet.event_control_word == 0x00038528u && packet.event_type == 0x28) ||
	                        (packet.event_control_word == 0x00038504u && packet.event_type == 4);
	return graphics_cp && packet.packet_id == 0xc0044700u && exact_event &&
	       (packet.address_control_word & 0xffff0000u) == 0x20000000u && packet.cache_policy == 0 &&
	       packet.event_write_dest == 0 && packet.cache_action == 0x38 &&
	       packet.event_index == 5 && packet.int_selector == 0 && packet.data_selector == 1 &&
	       packet.control_selector_byte == 0x20 && destination != 0 && destination <= 0x0000ffffffffffffull && destination % 4 == 0;
}

static bool eop_immediate32_checkpoint_selector_enabled(const char* value)
{
	return value != nullptr && std::strcmp(value, "1") == 0;
}

static bool experimental_eop_immediate32_checkpoint_enabled()
{
	static const bool enabled = eop_immediate32_checkpoint_selector_enabled(std::getenv("LUMEN_EXPERIMENTAL_GFX_EOP_IMMEDIATE32_CHECKPOINT"));
	return enabled;
}

static bool eop_virtual_clock_selector_enabled(const char* value)
{
	return value != nullptr && std::strcmp(value, "1") == 0;
}

static bool experimental_eop_virtual_clock_enabled()
{
	static const bool enabled = eop_virtual_clock_selector_enabled(std::getenv("LUMEN_EXPERIMENTAL_GFX_EOP_VIRTUAL_CLOCK"));
	return enabled;
}

enum class Pm4NopMarkerDecision : uint8_t
{
	OrdinaryNop,
	EmbeddedData,
	UserDataVsharp,
	UserDataRegion,
	Flip,
	FlipWithWrite,
	FlipWithInterrupt,
	Malformed,
};

static Pm4NopMarkerDecision pm4_nop_marker_decision(const uint32_t* body, uint32_t packet_dw, uint32_t available_dw)
{
	if (body == nullptr || packet_dw < 2 || packet_dw > available_dw)
	{
		return Pm4NopMarkerDecision::Malformed;
	}
	if ((body[0] & 0xffff0000u) != 0x68750000u)
	{
		return Pm4NopMarkerDecision::OrdinaryNop;
	}

	switch (body[0] & 0xfffu)
	{
		case 0x0u: return Pm4NopMarkerDecision::EmbeddedData;
		case 0x4u: return Pm4NopMarkerDecision::UserDataVsharp;
		case 0xdu: return Pm4NopMarkerDecision::UserDataRegion;
		case 0x777u: return Pm4NopMarkerDecision::Flip;
		case 0x778u: return packet_dw >= 5 ? Pm4NopMarkerDecision::FlipWithWrite : Pm4NopMarkerDecision::Malformed;
		case 0x781u: return packet_dw >= 7 ? Pm4NopMarkerDecision::FlipWithInterrupt : Pm4NopMarkerDecision::Malformed;
		default: return Pm4NopMarkerDecision::OrdinaryNop;
	}
}

struct Pm4DispatchDirectPacket
{
	uint32_t thread_group_x = 0;
	uint32_t thread_group_y = 0;
	uint32_t thread_group_z = 0;
	uint32_t renderer_mode  = 0;
	uint32_t consumed       = 0;
};

static bool pm4_decode_dispatch_direct(uint32_t cmd_id, const uint32_t* body, uint32_t available_dw, Pm4DispatchDirectPacket* decoded)
{
	if (body == nullptr || decoded == nullptr)
	{
		return false;
	}
	if (cmd_id == PM4_DISPATCH_DIRECT_COMPUTE_HEADER && available_dw >= 5 && body[3] == 1)
	{
		// The standard PM4 control word selects compute dispatch; it is not the
		// renderer-mode argument carried by Lumen's separate custom packet.
		*decoded = {body[0], body[1], body[2], 0, 4};
		return true;
	}
	if (cmd_id == LUMEN_PM4(9, Pm4::IT_NOP, Pm4::R_DISPATCH_DIRECT) && available_dw >= 9)
	{
		*decoded = {body[0], body[1], body[2], body[3], 8};
		return true;
	}
	return false;
}

struct Pm4DmaDataMemoryCopyPacket
{
	uint64_t source_address      = 0;
	uint64_t destination_address = 0;
	uint32_t byte_count          = 0;
};

static constexpr uint32_t PM4_DMA_DATA_MEMORY_COPY_HEADER  = 0xc0055000u;
static constexpr uint32_t PM4_DMA_DATA_MEMORY_COPY_CONTROL = 0x84004000u;
static constexpr uint32_t PM4_DMA_DATA_BYTE_COUNT_MASK     = 0x001fffffu;

static bool pm4_decode_dma_data_memory_copy(uint32_t cmd_id, const uint32_t* body, uint32_t available_dw,
	                                          Pm4DmaDataMemoryCopyPacket* decoded)
{
	if (cmd_id != PM4_DMA_DATA_MEMORY_COPY_HEADER || body == nullptr || available_dw < 7 || decoded == nullptr ||
	    body[0] != PM4_DMA_DATA_MEMORY_COPY_CONTROL || (body[5] & ~PM4_DMA_DATA_BYTE_COUNT_MASK) != 0)
	{
		return false;
	}

	*decoded = {
	    body[1] | (static_cast<uint64_t>(body[2]) << 32u),
	    body[3] | (static_cast<uint64_t>(body[4]) << 32u),
	    body[5] & PM4_DMA_DATA_BYTE_COUNT_MASK,
	};
	return true;
}

static bool pm4_decode_dma_data_gds_read(uint32_t cmd_id, const uint32_t* body, uint32_t available_dw,
                                       Pm4DmaDataMemoryCopyPacket* decoded)
{
	if (cmd_id != PM4_DMA_DATA_MEMORY_COPY_HEADER || body == nullptr || available_dw < 7 || decoded == nullptr ||
	    body[0] != 0xa4004000u || (body[5] & ~PM4_DMA_DATA_BYTE_COUNT_MASK) != 0) return false;
	const uint64_t source = body[1] | (static_cast<uint64_t>(body[2]) << 32u);
	const uint64_t destination = body[3] | (static_cast<uint64_t>(body[4]) << 32u);
	if (!GraphicsRenderGdsReadRangeIsValid(destination, source, body[5])) return false;
	*decoded = {source, destination, body[5]};
	return true;
}

static bool dma_data_memory_copy_ranges_supported(uint64_t source_address, uint64_t destination_address, uint32_t byte_count)
{
	if (byte_count == 0)
	{
		return true;
	}

	constexpr uint64_t pointer_max = std::numeric_limits<uintptr_t>::max();
	const uint64_t     copy_size   = byte_count;
	if (source_address == 0 || destination_address == 0 || source_address > pointer_max || destination_address > pointer_max ||
	    copy_size > pointer_max - source_address || copy_size > pointer_max - destination_address)
	{
		return false;
	}

	const uint64_t source_end      = source_address + copy_size;
	const uint64_t destination_end = destination_address + copy_size;
	return source_address >= destination_end || destination_address >= source_end;
}

[[maybe_unused]] static bool dma_data_memory_copy_bytes(uint64_t source_address, uint64_t destination_address, uint32_t byte_count)
{
	if (!dma_data_memory_copy_ranges_supported(source_address, destination_address, byte_count))
	{
		return false;
	}
	if (byte_count != 0)
	{
		std::memcpy(reinterpret_cast<void*>(destination_address), reinterpret_cast<const void*>(source_address), byte_count);
	}
	return true;
}

enum class RootCommandOwnershipDecision : uint8_t
{
	Eligible,
	NotQuiescent,
	ConstBuffer,
	Empty,
	Capacity,
	Malformed,
	Unsupported,
	Indirect,
	WaitDependency,
	CeDe,
	WriteOverlap,
	Count,
};

static bool root_command_write_overlaps(const uint32_t* root, uint32_t root_num_dw, uint64_t write_address,
	                                      uint64_t write_size)
{
	if (write_size == 0)
	{
		return false;
	}
	const uint64_t root_begin = reinterpret_cast<uint64_t>(root);
	const uint64_t root_size  = static_cast<uint64_t>(root_num_dw) * sizeof(uint32_t);
	if (root_begin > UINT64_MAX - root_size || write_address > UINT64_MAX - write_size)
	{
		return true;
	}
	const uint64_t root_end  = root_begin + root_size;
	const uint64_t write_end = write_address + write_size;
	return write_address < root_end && root_begin < write_end;
}

static RootCommandOwnershipDecision root_command_packet_decision(const uint32_t* guest_root, uint32_t root_num_dw,
	                                                               const uint32_t* packet, uint32_t packet_dw)
{
	const uint32_t cmd_id = packet[0];
	const uint32_t op     = (cmd_id >> 8u) & 0xffu;
	const uint32_t* body  = packet + 1;
	auto write_decision = [guest_root, root_num_dw](uint64_t address, uint64_t size)
	{
		return root_command_write_overlaps(guest_root, root_num_dw, address, size) ? RootCommandOwnershipDecision::WriteOverlap
		                                                                              : RootCommandOwnershipDecision::Eligible;
	};

	if (op == Pm4::IT_NOP)
	{
		const uint32_t custom = LUMEN_PM4_R(cmd_id);
		if (custom == Pm4::R_ZERO)
		{
			if (packet_dw < 2)
			{
				return RootCommandOwnershipDecision::Malformed;
			}
			if (cmd_id != LUMEN_PM4(packet_dw, Pm4::IT_NOP, Pm4::R_ZERO))
			{
				return RootCommandOwnershipDecision::Unsupported;
			}
			const auto marker = pm4_nop_marker_decision(body, packet_dw, packet_dw);
			if (marker == Pm4NopMarkerDecision::Malformed)
			{
				return RootCommandOwnershipDecision::Malformed;
			}
			if (marker == Pm4NopMarkerDecision::FlipWithWrite || marker == Pm4NopMarkerDecision::FlipWithInterrupt)
			{
				const uint64_t address = body[1] | (static_cast<uint64_t>(body[2]) << 32u);
				return write_decision(address, sizeof(uint32_t));
			}
			return RootCommandOwnershipDecision::Eligible;
		}
		if (custom == Pm4::R_SH_REGS_INDIRECT || custom == Pm4::R_CX_REGS_INDIRECT || custom == Pm4::R_UC_REGS_INDIRECT)
		{
			return RootCommandOwnershipDecision::Indirect;
		}
		if (custom == Pm4::R_WAIT_MEM_32 || custom == Pm4::R_WAIT_MEM_64 || custom == Pm4::R_WAIT_FLIP_DONE)
		{
			return RootCommandOwnershipDecision::WaitDependency;
		}
		if (custom == Pm4::R_WRITE_DATA)
		{
			if (packet_dw < 4)
			{
				return RootCommandOwnershipDecision::Malformed;
			}
			const uint64_t address = body[1] | (static_cast<uint64_t>(body[2]) << 32u);
			return write_decision(address, static_cast<uint64_t>(packet_dw - 4u) * sizeof(uint32_t));
		}
		if (custom == Pm4::R_RELEASE_MEM)
		{
			if (packet_dw < 7)
			{
				return RootCommandOwnershipDecision::Malformed;
			}
			const uint64_t address = body[2] | (static_cast<uint64_t>(body[3]) << 32u);
			return write_decision(address, sizeof(uint64_t));
		}
		if (custom == Pm4::R_DISPATCH_DIRECT)
		{
			if (packet_dw != 9)
			{
				return RootCommandOwnershipDecision::Malformed;
			}
			Pm4DispatchDirectPacket decoded;
			return pm4_decode_dispatch_direct(cmd_id, body, packet_dw, &decoded) && decoded.consumed == 8
			           ? RootCommandOwnershipDecision::Eligible
			           : RootCommandOwnershipDecision::Unsupported;
		}
		switch (custom)
		{
			case Pm4::R_VS:
			case Pm4::R_PS:
			case Pm4::R_DRAW_INDEX:
			case Pm4::R_DRAW_INDEX_AUTO:
			case Pm4::R_DRAW_RESET:
			case Pm4::R_CS:
			case Pm4::R_DISPATCH_RESET:
			case Pm4::R_PUSH_MARKER:
			case Pm4::R_POP_MARKER:
			case Pm4::R_VS_EMBEDDED:
			case Pm4::R_PS_EMBEDDED:
			case Pm4::R_VS_UPDATE:
			case Pm4::R_PS_UPDATE:
			case Pm4::R_ACQUIRE_MEM:
			case Pm4::R_FLIP: return RootCommandOwnershipDecision::Eligible;
			default: return RootCommandOwnershipDecision::Unsupported;
		}
	}

	switch (op)
	{
		case Pm4::IT_CLEAR_STATE:
			if (packet_dw != 2)
			{
				return RootCommandOwnershipDecision::Malformed;
			}
			return pm4_clear_state_packet_supported(cmd_id, body, packet_dw) ? RootCommandOwnershipDecision::Eligible
			                                                                 : RootCommandOwnershipDecision::Unsupported;
		case Pm4::IT_CONTEXT_CONTROL:
			if (packet_dw != 3)
			{
				return RootCommandOwnershipDecision::Malformed;
			}
			return pm4_context_control_packet_supported(cmd_id, body, packet_dw) ? RootCommandOwnershipDecision::Eligible
			                                                                     : RootCommandOwnershipDecision::Unsupported;
		case Pm4::IT_DISPATCH_DIRECT:
			if (packet_dw != 5)
			{
				return RootCommandOwnershipDecision::Malformed;
			}
			{
				Pm4DispatchDirectPacket decoded;
				return pm4_decode_dispatch_direct(cmd_id, body, packet_dw, &decoded) && decoded.consumed == 4
				           ? RootCommandOwnershipDecision::Eligible
				           : RootCommandOwnershipDecision::Unsupported;
			}
		case Pm4::IT_DRAW_INDEX_2:
		case Pm4::IT_INDEX_TYPE:
		case Pm4::IT_NUM_INSTANCES:
		case Pm4::IT_DRAW_INDEX_AUTO:
		case Pm4::IT_EVENT_WRITE:
		case Pm4::IT_ACQUIRE_MEM:
		case Pm4::IT_SET_CONTEXT_REG:
		case Pm4::IT_SET_SH_REG:
		case Pm4::IT_SET_UCONFIG_REG: return RootCommandOwnershipDecision::Eligible;
		case Pm4::IT_INDIRECT_BUFFER: return RootCommandOwnershipDecision::Indirect;
		case Pm4::IT_WAIT_REG_MEM: return RootCommandOwnershipDecision::WaitDependency;
		case Pm4::IT_WRITE_CONST_RAM:
		case Pm4::IT_DUMP_CONST_RAM:
		case Pm4::IT_INCREMENT_CE_COUNTER:
		case Pm4::IT_INCREMENT_DE_COUNTER:
		case Pm4::IT_WAIT_ON_CE_COUNTER:
		case Pm4::IT_WAIT_ON_DE_COUNTER_DIFF: return RootCommandOwnershipDecision::CeDe;
		case Pm4::IT_DMA_DATA: return RootCommandOwnershipDecision::Unsupported;
		case Pm4::IT_WRITE_DATA:
		{
			if (packet_dw < 4)
			{
				return RootCommandOwnershipDecision::Malformed;
			}
			const uint64_t address = body[1] | (static_cast<uint64_t>(body[2]) << 32u);
			return write_decision(address, static_cast<uint64_t>(packet_dw - 4u) * sizeof(uint32_t));
		}
		case Pm4::IT_EVENT_WRITE_EOP:
		{
			if (packet_dw < 6)
			{
				return RootCommandOwnershipDecision::Malformed;
			}
			const uint64_t address = body[1] | (static_cast<uint64_t>(body[2] & 0xffffu) << 32u);
			return write_decision(address, sizeof(uint64_t));
		}
		case Pm4::IT_EVENT_WRITE_EOS:
		{
			if (packet_dw < 5)
			{
				return RootCommandOwnershipDecision::Malformed;
			}
			const uint64_t address = body[1] | (static_cast<uint64_t>(body[2] & 0xffffu) << 32u);
			return write_decision(address, sizeof(uint32_t));
		}
		case Pm4::IT_RELEASE_MEM:
		{
			if (packet_dw < 7)
			{
				return RootCommandOwnershipDecision::Malformed;
			}
			const uint64_t address = body[2] | (static_cast<uint64_t>(body[3]) << 32u);
			return write_decision(address, sizeof(uint64_t));
		}
		default: return RootCommandOwnershipDecision::Unsupported;
	}
}

static RootCommandOwnershipDecision root_command_ownership_decision(const uint32_t* draw, uint32_t draw_num_dw,
	                                                                  const uint32_t* constant, uint32_t constant_num_dw,
	                                                                  bool quiescent, const uint32_t* guest_root = nullptr)
{
	if (!quiescent)
	{
		return RootCommandOwnershipDecision::NotQuiescent;
	}
	if (constant != nullptr || constant_num_dw != 0)
	{
		return RootCommandOwnershipDecision::ConstBuffer;
	}
	if (draw == nullptr || draw_num_dw == 0)
	{
		return RootCommandOwnershipDecision::Empty;
	}
	if (draw_num_dw > ROOT_COMMAND_OWNERSHIP_MAX_DWORDS)
	{
		return RootCommandOwnershipDecision::Capacity;
	}

	uint32_t offset = 0;
	while (offset < draw_num_dw)
	{
		const uint32_t cmd_id = draw[offset];
		if ((cmd_id & 0xc0000000u) != 0xc0000000u)
		{
			return RootCommandOwnershipDecision::Malformed;
		}
		const uint32_t packet_dw = LUMEN_PM4_LEN(cmd_id);
		if (packet_dw < 2 || packet_dw > draw_num_dw - offset)
		{
			return RootCommandOwnershipDecision::Malformed;
		}
		const auto decision = root_command_packet_decision(guest_root == nullptr ? draw : guest_root, draw_num_dw, draw + offset, packet_dw);
		if (decision != RootCommandOwnershipDecision::Eligible)
		{
			return decision;
		}
		offset += packet_dw;
	}
	return RootCommandOwnershipDecision::Eligible;
}

static bool root_command_ownership_storage_available(uint64_t retained_dw, uint32_t requested_dw)
{
	return retained_dw <= ROOT_COMMAND_OWNERSHIP_MAX_TOTAL_DWORDS &&
	       requested_dw <= ROOT_COMMAND_OWNERSHIP_MAX_TOTAL_DWORDS - retained_dw;
}

static bool root_command_ownership_snapshot_valid(RootCommandOwnershipDecision copied_decision, const uint32_t* owned,
	                                                const uint32_t* guest, uint32_t num_dw)
{
	return copied_decision == RootCommandOwnershipDecision::Eligible && owned != nullptr && guest != nullptr && num_dw != 0 &&
	       std::memcmp(owned, guest, static_cast<size_t>(num_dw) * sizeof(uint32_t)) == 0;
}

static const char* command_stream_kind_name(CommandStreamKind kind)
{
	switch (kind)
	{
		case CommandStreamKind::Draw: return "draw";
		case CommandStreamKind::Const: return "const";
		case CommandStreamKind::Compute: return "compute";
		case CommandStreamKind::Indirect: return "indirect";
		default: return "unknown";
	}
}

struct EopInitialTargetIdentity
{
	const void* processor = nullptr;
	const void* buffer = nullptr;
	const void* parent = nullptr;
	const void* pool = nullptr;
	int current_index = -1, processor_queue = -1, buffer_queue = -1;
	uint32_t pool_index = UINT32_MAX;
	uint64_t submit_id = 0, processor_epoch = 0, assigned_serial = 0;
	uint64_t recording = 0, session = 0, serial = 0, ordinal = 0, submitted_cutoff = 0, completed_cutoff = 0;
	bool exclusive = false, valid = false;

	bool Matches(const EopInitialTargetIdentity& other) const
	{
		return valid && other.valid &&
		    std::tie(processor, buffer, parent, pool, current_index, processor_queue, buffer_queue, pool_index,
		             submit_id, processor_epoch, assigned_serial, recording, session, serial, ordinal,
		             submitted_cutoff, completed_cutoff, exclusive) ==
		    std::tie(other.processor, other.buffer, other.parent, other.pool, other.current_index, other.processor_queue,
		             other.buffer_queue, other.pool_index, other.submit_id, other.processor_epoch, other.assigned_serial,
		             other.recording, other.session, other.serial, other.ordinal, other.submitted_cutoff,
		             other.completed_cutoff, other.exclusive);
	}
};

static bool GraphicsRecordingIdentityValid(const EopInitialTargetIdentity& identity,
                                          const void* processor, const void* buffer)
{
	return processor != nullptr && buffer != nullptr && identity.valid &&
	    identity.processor == processor && identity.parent == processor && identity.buffer == buffer &&
	    identity.pool != nullptr && identity.pool_index != UINT32_MAX && identity.current_index >= 0 &&
	    identity.processor_queue == GraphicContext::QUEUE_GFX && identity.buffer_queue == GraphicContext::QUEUE_GFX &&
	    identity.recording != 0 && identity.session != 0 && identity.session == identity.processor_epoch &&
	    identity.serial != 0 && identity.serial == identity.assigned_serial && identity.submitted_cutoff < identity.serial &&
	    identity.completed_cutoff <= identity.submitted_cutoff;
}

static bool ExclusiveGraphicsRecordingIdentityValid(const EopInitialTargetIdentity& identity,
                                                     const void* processor, const void* buffer)
{
	return identity.exclusive && GraphicsRecordingIdentityValid(identity, processor, buffer);
}

struct RetainedProcessorRecordingWitness
{
	CommandProcessor* processor;
	CommandBuffer* buffer;
	uint64_t unlock_epoch;
	uint32_t depth;
	RetainedProcessorRecordingWitness* previous;
	bool revoked = false;
};
static thread_local RetainedProcessorRecordingWitness* g_retained_processor_recording = nullptr;

class RetainedProcessorRecordingScope final
{
public:
	RetainedProcessorRecordingScope(CommandProcessor* processor, CommandBuffer* buffer, uint64_t unlock_epoch):
	    m_witness {processor, buffer, unlock_epoch,
	        g_retained_processor_recording == nullptr ? 1u : g_retained_processor_recording->depth + 1u,
	        g_retained_processor_recording}
	{
		g_retained_processor_recording = &m_witness;
	}
	~RetainedProcessorRecordingScope() { g_retained_processor_recording = m_witness.previous; }
	LUMEN_CLASS_NO_COPY(RetainedProcessorRecordingScope);
private:
	RetainedProcessorRecordingWitness m_witness;
};

static bool RetainedProcessorScopeQualified(CommandProcessor* cp, CommandBuffer* buffer, uint64_t unlock_epoch,
                                            bool run_owned, bool parser_owned)
{
	const auto* witness = g_retained_processor_recording;
	return witness != nullptr && !witness->revoked && witness->depth == 1 && witness->processor == cp && witness->buffer == buffer &&
	    cp != nullptr && buffer != nullptr && witness->unlock_epoch == unlock_epoch && unlock_epoch != UINT64_MAX &&
	    run_owned && parser_owned;
}

static bool DeferredLabelCaptureScopeQualified(const EopInitialTargetIdentity& identity, const void* processor,
                                              bool run_lock_held, bool parser_owned)
{
	return run_lock_held && parser_owned && GraphicsRecordingIdentityValid(identity, processor, identity.buffer);
}

static bool ScalarLabelCaptureScopeQualified(const EopInitialTargetIdentity& identity, const void* processor,
                                            bool run_lock_held, bool parser_owned)
{
	return run_lock_held && parser_owned && ExclusiveGraphicsRecordingIdentityValid(identity, processor, identity.buffer);
}

static LabelScalarRecordingIdentity ScalarLabelRecordingIdentity(const EopInitialTargetIdentity& identity)
{
	return {identity.processor, identity.buffer, identity.parent, identity.pool,
	    identity.current_index, identity.processor_queue, identity.buffer_queue, identity.pool_index,
	    identity.submit_id, identity.processor_epoch, identity.assigned_serial, identity.recording,
	    identity.session, identity.serial, identity.ordinal, identity.submitted_cutoff, identity.completed_cutoff,
	    identity.exclusive, identity.valid};
}

struct EopInitialTargetCaptureResult
{
	bool captured = false;
	GraphicsGuestLeaseResult acquisition;
	uint64_t original_generation = UINT64_MAX;
	uint32_t rounds = 0;
};

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
static bool graphics_run_render_full_preflight_retry_test(CommandProcessor* cp);
static bool graphics_run_retained_recording_scope_test(CommandProcessor* cp);
#endif

static constexpr uint32_t CONST_RAM_DWORDS = 0x3000;

// Minted only by the graphics coordinator after both CE/DE jobs have returned,
// while RunLock still excludes the next batch. This grants no EOP authority.
class JoinedGraphicsParserAuthority final
{
public:
	LUMEN_CLASS_NO_COPY(JoinedGraphicsParserAuthority);
	bool IsFor(const CommandProcessor* processor) const { return m_processor == processor; }
private:
	friend class GraphicsRing;
	explicit JoinedGraphicsParserAuthority(const CommandProcessor* processor): m_processor(processor) {}
	const CommandProcessor* m_processor;
};

class CommandProcessor
{
public:
	struct FlipInfo
	{
		int     handle    = 0;
		int     index     = 0;
		int     flip_mode = 0;
		int64_t flip_arg  = 0;
	};

	struct IndexBufferSizeState
	{
		uint32_t index_count = 0;
		bool     configured  = false;
	};

	explicit CommandProcessor(uint32_t interrupt_event_id): m_interrupt_event_id(interrupt_event_id) {}
	virtual ~CommandProcessor() { LUMEN_NOT_IMPLEMENTED; }

	LUMEN_CLASS_NO_COPY(CommandProcessor);

	void Reset();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	bool ResetIOSRuntimeSession();
	void ReleaseIOSRuntimeSessionRetiredResources();
#endif

	void BufferInit();
	void BufferFlush(bool host_read = false);
	void BufferFlushPending();
	void RetireCompletedReadOnlyStorage(const JoinedGraphicsParserAuthority& joined);
	void BufferWait(Lumen::CriticalPath::Reason reason = Lumen::CriticalPath::Reason::GpuBufferWait);
	bool BufferWaitForPrefix(int queue, uint64_t epoch, uint64_t serial,
	    Lumen::CriticalPath::Reason reason = Lumen::CriticalPath::Reason::GpuDependencyWait);
	void BufferFlushAndWaitForPrefix(Lumen::CriticalPath::Reason reason);
	void SetEopCheckpointExclusiveStream(bool exclusive)
	{
		Core::LockGuard lock(m_mutex);
		m_eop_checkpoint_exclusive_stream = exclusive;
	}
	bool EopResumeValid(int queue, uint64_t epoch, uint64_t serial) const
	{
		return m_eop_checkpoint_exclusive_stream && HasCompletedTrackedCommandBuffer(queue, epoch, serial);
	}
	bool ExclusiveRecordingValid(CommandBuffer* buffer)
	{
		// Do not dereference a stale or foreign pointer supplied by a caller.
		if (buffer == nullptr || GetCurrentBufferForCpuCopy() != buffer) return false;
		return ExclusiveGraphicsRecordingIdentityValid(GetEopInitialTargetIdentity(), this, buffer);
	}
	bool RetainedRecordingValid(CommandBuffer* buffer);
	LabelScalarRecordingIdentity ScalarLabelIdentity(CommandBuffer* buffer);
	LabelScalarRecordingIdentity DeferredLabelIdentity(CommandBuffer* buffer);
	GpuMemoryEopCheckpointResult TryEopHostCheckpoint(const GuestWriteDestination* publication_destination = nullptr,
	    GpuMemoryEopPublicationKind publication_kind = GpuMemoryEopPublicationKind::VirtualClock64, uint32_t immediate32 = 0);
	GpuMemoryDbCacheProofResult TryDbCacheBarrierWithLeaseRetry(bool admission_enabled);

	void RunLock() { m_run_mutex.Lock(); m_run_lock_held.store(true, std::memory_order_release); }
	void RunUnlock() { m_run_lock_held.store(false, std::memory_order_release); m_run_mutex.Unlock(); }
	void Lock() { m_mutex.Lock(); }
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	bool TryLockForTest() { return m_mutex.TryLock(); }
#endif
	void Unlock()
	{
		// A draw witness cannot survive a CP release/reacquire, even if another
		// stream happens to restore all of the visible recording metadata.
		if (m_retained_recording_unlock_epoch != UINT64_MAX) ++m_retained_recording_unlock_epoch;
		for (auto* witness = g_retained_processor_recording; witness != nullptr; witness = witness->previous)
			if (witness->processor == this) witness->revoked = true;
		m_mutex.Unlock();
	}
	bool TryLock() { return m_mutex.TryLock(); }
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	friend bool graphics_run_render_full_preflight_retry_test(CommandProcessor* cp);
	friend bool graphics_run_retained_recording_scope_test(CommandProcessor* cp);
	void               SetHeadlessTestMode() { m_headless_test_mode = true; }
	[[nodiscard]] bool IsHeadlessTestMode() const { return m_headless_test_mode; }
	bool               TryRunLockForTest() { return m_run_mutex.TryLock(); }
	bool               IsIOSRuntimeSessionResetForTest();
#endif

	HW::Context*    GetCtx() { return &m_ctx; }
	HW::UserConfig* GetUcfg() { return &m_ucfg; }
	HW::Shader*     GetShCtx() { return &m_sh_ctx; }
	CommandBuffer*  GetCurrentBufferForCpuCopy()
	{
		return m_current_buffer >= 0 && m_current_buffer < VK_BUFFERS_NUM ? m_buffer[m_current_buffer] : nullptr;
	}

	void SetIndexType(uint32_t index_type_and_size);
	void SetIndexBufferSize(uint32_t index_count);
	[[nodiscard]] IndexBufferSizeState GetIndexBufferSizeState();
	void SetNumInstances(uint32_t num_instances);
	void DrawIndex(uint32_t index_count, const void* index_addr, uint32_t flags, uint32_t type, uint32_t consumed_packet_dw = 0);
	void DrawIndexAuto(uint32_t index_count, uint32_t flags, uint32_t consumed_packet_dw = 0);
	bool PrepareOwnedDraw(GraphicsRenderOwnedPreparation* preparation, uint32_t consumed_packet_dw);
	bool DrawWithRangeOwnership(uint32_t index_count, const void* index_addr, uint32_t flags, uint32_t type,
	                            uint32_t consumed_packet_dw, bool indexed);
	void PrepareVertexSource(ShaderVertexSourceSnapshot* source);
	void WriteAtEndOfPixelShader32(uint32_t* destination, uint32_t value);
	void WriteAtEndOfPipe32(uint32_t cache_policy, uint32_t event_write_dest, uint32_t eop_event_type, uint32_t cache_action,
	                        uint32_t event_index, uint32_t event_write_source, void* dst_gpu_addr, uint32_t value,
	                        uint32_t interrupt_selector);
	void WriteAtEndOfPipe64(uint32_t cache_policy, uint32_t event_write_dest, uint32_t eop_event_type, uint32_t cache_action,
	                        uint32_t event_index, uint32_t event_write_source, void* dst_gpu_addr, uint64_t value,
	                        uint32_t legacy_interrupt_selector, uint32_t control_selector_byte, uint32_t packet_id,
	                        uint32_t event_control_word, uint32_t address_control_word);
	void Flip();
	void Flip(void* dst_gpu_addr, uint32_t value);
	void FlipWithInterrupt(uint32_t eop_event_type, uint32_t cache_action, void* dst_gpu_addr, uint32_t value);
	void WriteBack();
	void WriteBack(uint64_t vaddr, uint64_t size);
	void MemoryBarrier();
	void FullMemoryBarrier();
	void RenderTextureBarrier(uint64_t vaddr, uint64_t size);
	void DepthStencilBarrier(uint64_t vaddr, uint64_t size);
	void DispatchDirect(uint32_t thread_group_x, uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode);
	void WaitFlipDone(uint32_t video_out_handle, uint32_t display_buffer_index);
	void TriggerEvent(uint32_t event_type, uint32_t event_index);

	void                           SetUserDataMarker(HW::UserSgprType type);
	[[nodiscard]] HW::UserSgprType GetUserDataMarker();
	void                           SetEmbeddedDataMarker(const uint32_t* buffer, uint32_t num_dw, uint32_t align) {}
	void                           PushMarker(const char* str) {}
	void                           PopMarker() {}

	void PrefetchL2(void* addr, uint32_t size) {}
	void ClearGds(uint64_t dw_offset, uint32_t dw_num, uint32_t clear_value);
	void ReadGds(uint64_t destination_address, uint64_t source_byte_offset, uint32_t byte_count);
	void CopyDmaData(uint64_t source_address, uint64_t destination_address, uint32_t byte_count);

	void ResetDeCe();
	bool CeWaitReady();
	bool DeDiffWaitReady(uint32_t diff);
	void WaitCe();
	void WaitDeDiff(uint32_t diff);
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	bool ReadyCounterWaitContractTest();
#endif
	void IncremenetDe();
	void IncremenetCe();

	void WriteConstRam(uint32_t offset, const uint32_t* src, uint32_t dw_num);
	void DumpConstRam(uint32_t* dst, uint32_t offset, uint32_t dw_num);

	void WaitRegMem32(uint32_t func, const uint32_t* addr, uint32_t ref, uint32_t mask, uint32_t poll);
	void WaitRegMem64(uint32_t func, const uint64_t* addr, uint64_t ref, uint64_t mask, uint32_t poll);
	void WriteData(uint32_t* dst, const uint32_t* src, uint32_t dw_num, uint32_t write_control, bool custom);

	void Run(uint32_t* parse_data, uint32_t num_dw, const uint32_t* submitted_data = nullptr,
	         CommandStreamKind stream_kind = CommandStreamKind::Unknown, uint32_t* guest_data = nullptr);
	void RecordUserSgprWrite(ShaderUserStage stage, uint32_t slot, const uint32_t* values, uint32_t count,
	                         HW::UserSgprType type, uint32_t cmd_id, uint32_t cmd_offset, uint32_t payload_index = 0);
	void DumpDescriptorProvenance(ShaderUserStage stage, uint32_t start_register, uint32_t register_count);

	void SetQueue(int queue) { m_queue = queue; }
	[[nodiscard]] uint32_t GetInterruptEventId() const { return m_interrupt_event_id; }
	[[nodiscard]] bool BeginTrackedCommandBufferSubmission(int queue, uint64_t* session_epoch,
	                                                       uint64_t* recording_submit_serial,
	                                                       uint64_t* begin_submit_serial_cutoff,
	                                                       uint64_t* begin_completed_serial_cutoff);
	[[nodiscard]] bool RecordTrackedCommandBufferSubmission(int queue, uint64_t session_epoch,
	                                                        uint64_t recording_submit_serial);
	void RecordTrackedCommandBufferCompletion(int queue, uint64_t session_epoch, uint64_t recording_submit_serial);
	[[nodiscard]] bool HasSubmittedTrackedCommandBuffer(int queue, uint64_t session_epoch,
	                                                   uint64_t recording_submit_serial) const;
	[[nodiscard]] bool HasCompletedTrackedCommandBuffer(int queue, uint64_t session_epoch,
	                                                   uint64_t recording_submit_serial) const;
	void ResetTrackedCommandBufferSubmissionTimeline();

	[[nodiscard]] FlipInfo GetFlip()
	{
		Core::LockGuard lock(m_mutex);
		return m_flip;
	}
	void SetFlip(const FlipInfo& flip)
	{
		(void)VideoOut::VideoOutPrepareEventPayload(static_cast<uint64_t>(flip.flip_arg));
		Core::LockGuard lock(m_mutex);
		m_flip = flip;
	}

	[[nodiscard]] uint64_t GetSumbitId()
	{
		Core::LockGuard lock(m_mutex);
		return m_sumbit_id;
	}
	void SetSumbitId(uint64_t sumbit_id)
	{
		Core::LockGuard lock(m_mutex);
		m_sumbit_id = sumbit_id;
	}

private:
	// A deeper ring keeps more submissions in flight before the reuse fence
	// wait in BufferFlush; four serialized the parser against the GPU.
	static constexpr int VK_BUFFERS_NUM = VulkanCommandPool::PROCESSOR_BUFFER_COUNT;
	static constexpr int SHADER_USER_STAGE_NUM = 4;
	EopInitialTargetIdentity GetEopInitialTargetIdentity(bool require_exclusive = true);
	EopInitialTargetCaptureResult CaptureEopInitialTarget(GuestWriteDestination* destination, uint64_t address,
	                                                    uint64_t bytes, uint64_t alignment);
	LabelScalarCaptureTicket CaptureScalarLabelDestination(uint64_t address, uint64_t bytes, uint64_t value);

	struct UserSgprWriter
	{
		uint64_t          sequence             = 0;
		uint64_t          submit_id            = 0;
		uint64_t          stream_address       = 0;
		uint32_t          stream_num_dw        = 0;
		uint32_t          packet_offset_dw     = 0;
		uint32_t          packet_num_dw        = 0;
		uint32_t          depth                = 0;
		uint32_t          parsed_value         = 0;
		uint32_t          submitted_value      = 0;
		uint32_t          parsed_cmd_id        = 0;
		uint32_t          submitted_cmd_id     = 0;
		uint32_t          parsed_cmd_offset    = 0;
		uint32_t          submitted_cmd_offset = 0;
		CommandStreamKind stream_kind          = CommandStreamKind::Unknown;
		HW::UserSgprType  type                 = HW::UserSgprType::Unknown;
		bool              submitted_available   = false;
		bool              submitted_layout_same = false;
		bool              valid                 = false;
	};

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
public:
	struct UserSgprWriterForTest
	{
		uint64_t          sequence             = 0;
		uint64_t          submit_id            = 0;
		uint64_t          stream_address       = 0;
		uint32_t          stream_num_dw        = 0;
		uint32_t          packet_offset_dw     = 0;
		uint32_t          packet_num_dw        = 0;
		uint32_t          depth                = 0;
		uint32_t          parsed_value         = 0;
		uint32_t          submitted_value      = 0;
		uint32_t          parsed_cmd_id        = 0;
		uint32_t          submitted_cmd_id     = 0;
		uint32_t          parsed_cmd_offset    = 0;
		uint32_t          submitted_cmd_offset = 0;
		CommandStreamKind stream_kind          = CommandStreamKind::Unknown;
		HW::UserSgprType  type                 = HW::UserSgprType::Unknown;
		bool              submitted_available   = false;
		bool              submitted_layout_same = false;
		bool              valid                 = false;
	};
	UserSgprWriterForTest GetUserSgprWriterForTest(ShaderUserStage stage, uint32_t slot);

private:
#endif

	struct Counter
	{
		Core::Mutex   mutex;
		Core::CondVar cond_var;
		uint32_t      value = 0;
	};

	HW::Context      m_ctx;
	HW::UserConfig   m_ucfg;
	HW::Shader       m_sh_ctx;
	uint32_t         m_index_type_and_size = 0;
	uint32_t         m_num_instances       = 1;
	IndexBufferSizeState m_index_buffer_size;

	Core::Mutex m_mutex;
	Core::Mutex m_run_mutex;
	std::atomic_bool m_run_lock_held {false};
	bool m_eop_checkpoint_exclusive_stream = false;
	uint64_t m_retained_recording_unlock_epoch = 0;

	CommandBuffer* m_buffer[VK_BUFFERS_NUM] = {};
	int            m_current_buffer         = -1;
	int            m_queue                  = -1;
	const uint32_t m_interrupt_event_id;
	std::atomic_uint64_t m_tracked_submission_session_epoch {1};
	std::atomic_uint64_t m_tracked_submission_assigned_serial {0};
	std::atomic_uint64_t m_tracked_submission_submitted_serial {0};
	std::atomic_uint64_t m_tracked_submission_completed_serial {0};
	std::atomic_bool     m_tracked_submission_timeline_valid {true};

	Counter m_de_counter;
	Counter m_ce_counter;

	uint32_t m_const_ram[CONST_RAM_DWORDS] = {0};

	FlipInfo m_flip;
	uint64_t m_sumbit_id = 0;
	uint64_t m_user_sgpr_write_sequence = 0;
	UserSgprWriter m_user_sgpr_writers[SHADER_USER_STAGE_NUM][HW::UserSgprInfo::SGPRS_MAX] = {};
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	bool m_headless_test_mode = false;
	const EopInitialTargetIdentity* m_retained_identity_for_test = nullptr;
#endif
};

class CommandProcessorStateLock final
{
public:
	explicit CommandProcessorStateLock(CommandProcessor* cp): m_cp(cp)
	{
		EXIT_IF(m_cp == nullptr);
		m_cp->Lock();
	}
	~CommandProcessorStateLock() { m_cp->Unlock(); }

	LUMEN_CLASS_NO_COPY(CommandProcessorStateLock);

private:
	CommandProcessor* m_cp;
};

struct CommandProcessorParserThreadState
{
	CommandProcessor*                  owner    = nullptr;
	HW::UserSgprType                   marker   = HW::UserSgprType::Unknown;
	CommandProcessorParserThreadState* previous = nullptr;
};

static thread_local CommandProcessorParserThreadState* g_command_processor_parser_state = nullptr;

struct CommandProcessorStreamThreadState
{
	CommandProcessor*                  owner          = nullptr;
	uint32_t*                          live_data      = nullptr;
	const uint32_t*                    submitted_data = nullptr;
	uint32_t                           num_dw         = 0;
	uint32_t                           packet_offset  = 0;
	uint32_t                           packet_num_dw  = 0;
	uint32_t                           depth          = 0;
	CommandStreamKind                  kind           = CommandStreamKind::Unknown;
	bool                               root_owned     = false;
	CommandProcessorStreamThreadState* previous       = nullptr;
};

static thread_local CommandProcessorStreamThreadState* g_command_processor_stream_state = nullptr;

static bool command_provenance_recording()
{
	return g_command_processor_stream_state != nullptr &&
	       (command_provenance_enabled() || g_command_processor_stream_state->submitted_data != nullptr);
}

class CommandProcessorStreamStateScope final
{
public:
	CommandProcessorStreamStateScope(CommandProcessor* cp, uint32_t* live_data, const uint32_t* submitted_data, uint32_t num_dw,
	                                 CommandStreamKind kind, bool root_owned)
	{
		EXIT_IF(cp == nullptr);
		// Handoff pauses need the complete active chain, including indirect
		// ancestors, independently of optional descriptor diagnostics.
		m_active = GpuMemoryCpuHandoffEnabled() || command_provenance_enabled() || submitted_data != nullptr || root_owned;
		if (!m_active)
		{
			return;
		}
		m_state.owner          = cp;
		m_state.live_data      = live_data;
		m_state.submitted_data = submitted_data;
		m_state.num_dw         = num_dw;
		m_state.kind           = kind;
		m_state.root_owned     = root_owned;
		m_state.previous       = g_command_processor_stream_state;
		m_state.depth          = (m_state.previous != nullptr && m_state.previous->owner == cp ? m_state.previous->depth + 1u : 0u);
		g_command_processor_stream_state = &m_state;
	}

	~CommandProcessorStreamStateScope()
	{
		if (!m_active)
		{
			return;
		}
		EXIT_IF(g_command_processor_stream_state != &m_state);
		g_command_processor_stream_state = m_state.previous;
	}

	void SetPacket(uint32_t offset, uint32_t num_dw)
	{
		if (!m_active)
		{
			return;
		}
		m_state.packet_offset = offset;
		m_state.packet_num_dw = num_dw;
	}

	LUMEN_CLASS_NO_COPY(CommandProcessorStreamStateScope);

private:
	CommandProcessorStreamThreadState m_state;
	bool                              m_active = false;
};

class CommandProcessorRunStateScope final
{
public:
	explicit CommandProcessorRunStateScope(CommandProcessor* cp): m_cp(cp)
	{
		EXIT_IF(m_cp == nullptr);
		if (g_command_processor_parser_state == nullptr || g_command_processor_parser_state->owner != m_cp)
		{
			m_state.owner    = m_cp;
			m_state.marker   = HW::UserSgprType::Unknown;
			m_state.previous = g_command_processor_parser_state;
			g_command_processor_parser_state = &m_state;
			m_owns_state = true;
		}
	}

	~CommandProcessorRunStateScope()
	{
		if (m_owns_state)
		{
			EXIT_IF(g_command_processor_parser_state != &m_state || m_state.owner != m_cp);
			g_command_processor_parser_state = m_state.previous;
		}
	}

	LUMEN_CLASS_NO_COPY(CommandProcessorRunStateScope);

private:
	CommandProcessor*                  m_cp;
	CommandProcessorParserThreadState  m_state;
	bool                               m_owns_state = false;
};

void CommandProcessor::SetUserDataMarker(HW::UserSgprType type)
{
	EXIT_NOT_IMPLEMENTED(g_command_processor_parser_state == nullptr || g_command_processor_parser_state->owner != this);
	g_command_processor_parser_state->marker = type;
}

HW::UserSgprType CommandProcessor::GetUserDataMarker()
{
	EXIT_NOT_IMPLEMENTED(g_command_processor_parser_state == nullptr || g_command_processor_parser_state->owner != this);
	return g_command_processor_parser_state->marker;
}

void CommandProcessor::RecordUserSgprWrite(ShaderUserStage stage, uint32_t slot, const uint32_t* values, uint32_t count,
	                                        HW::UserSgprType type, uint32_t cmd_id, uint32_t cmd_offset, uint32_t payload_index)
{
	auto* stream = g_command_processor_stream_state;
	if (!command_provenance_enabled() && (stream == nullptr || stream->submitted_data == nullptr))
	{
		return;
	}
	EXIT_IF(values == nullptr);
	EXIT_IF(slot >= HW::UserSgprInfo::SGPRS_MAX || count > HW::UserSgprInfo::SGPRS_MAX - slot);

	EXIT_IF(stream == nullptr || stream->owner != this);
	const auto stage_index = static_cast<uint32_t>(stage);
	EXIT_IF(stage_index >= SHADER_USER_STAGE_NUM);

	for (uint32_t i = 0; i < count; ++i)
	{
		auto& writer                  = m_user_sgpr_writers[stage_index][slot + i];
		writer.sequence               = ++m_user_sgpr_write_sequence;
		writer.submit_id              = m_sumbit_id;
		writer.stream_address         = reinterpret_cast<uint64_t>(stream->live_data);
		writer.stream_num_dw          = stream->num_dw;
		writer.packet_offset_dw       = stream->packet_offset;
		writer.packet_num_dw          = stream->packet_num_dw;
		writer.depth                  = stream->depth;
		writer.parsed_value           = values[i];
		writer.parsed_cmd_id          = cmd_id;
		writer.parsed_cmd_offset      = cmd_offset;
		writer.stream_kind            = stream->kind;
		writer.type                   = type;
		writer.submitted_available    = false;
		writer.valid                  = true;

		// SET_SH_REG layout is header, register offset, then values. The
		// submitted shadow is diagnostic only; parsing continues from live data.
		const uint32_t submitted_index = stream->packet_offset + 2u + payload_index + i;
		if (stream->submitted_data != nullptr && submitted_index < stream->num_dw)
		{
			writer.submitted_value     = stream->submitted_data[submitted_index];
			writer.submitted_cmd_id    = stream->submitted_data[stream->packet_offset];
			writer.submitted_cmd_offset = stream->submitted_data[stream->packet_offset + 1u];
			writer.submitted_available = true;
			writer.submitted_layout_same = writer.submitted_cmd_id == writer.parsed_cmd_id &&
			                               writer.submitted_cmd_offset == writer.parsed_cmd_offset &&
			                               LUMEN_PM4_LEN(writer.submitted_cmd_id) == stream->packet_num_dw;
		}
	}
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
CommandProcessor::UserSgprWriterForTest CommandProcessor::GetUserSgprWriterForTest(ShaderUserStage stage, uint32_t slot)
{
	Core::LockGuard lock(m_mutex);
	const auto      stage_index = static_cast<uint32_t>(stage);
	EXIT_IF(stage_index >= SHADER_USER_STAGE_NUM || slot >= HW::UserSgprInfo::SGPRS_MAX);
	const auto& writer = m_user_sgpr_writers[stage_index][slot];
	return UserSgprWriterForTest {writer.sequence,
	                              writer.submit_id,
	                              writer.stream_address,
	                              writer.stream_num_dw,
	                              writer.packet_offset_dw,
	                              writer.packet_num_dw,
	                              writer.depth,
	                              writer.parsed_value,
	                              writer.submitted_value,
	                              writer.parsed_cmd_id,
	                              writer.submitted_cmd_id,
	                              writer.parsed_cmd_offset,
	                              writer.submitted_cmd_offset,
	                              writer.stream_kind,
	                              writer.type,
	                              writer.submitted_available,
	                              writer.submitted_layout_same,
	                              writer.valid};
}
#endif

void CommandProcessor::DumpDescriptorProvenance(ShaderUserStage stage, uint32_t start_register, uint32_t register_count)
{
	if (!command_provenance_enabled())
	{
		return;
	}
	const auto stage_index = static_cast<uint32_t>(stage);
	EXIT_IF(stage_index >= SHADER_USER_STAGE_NUM);
	EXIT_IF(start_register >= HW::UserSgprInfo::SGPRS_MAX || register_count > HW::UserSgprInfo::SGPRS_MAX - start_register);

	for (uint32_t i = 0; i < register_count; ++i)
	{
		const auto& writer = m_user_sgpr_writers[stage_index][start_register + i];
		if (!writer.valid)
		{
			dbg_printf("[GFX_CMD_PROVENANCE] stage=%u sgpr=%u writer=none\n", stage_index, start_register + i);
			continue;
		}
		dbg_printf("[GFX_CMD_PROVENANCE] stage=%u sgpr=%u seq=%" PRIu64 " submit=%" PRIu64
		           " kind=%s stream=0x%016" PRIx64 " dw=%u depth=%u packet=%u packet_dw=%u type=%u parsed=%08" PRIx32,
		           stage_index, start_register + i, writer.sequence, writer.submit_id, command_stream_kind_name(writer.stream_kind),
		           writer.stream_address, writer.stream_num_dw, writer.depth, writer.packet_offset_dw, writer.packet_num_dw,
		           static_cast<unsigned>(writer.type), writer.parsed_value);
		if (writer.submitted_available)
		{
			dbg_printf(" submitted=%08" PRIx32 " layout_same=%u live_header=%08" PRIx32 " submitted_header=%08" PRIx32
			           " live_offset=%08" PRIx32 " submitted_offset=%08" PRIx32 " changed=%s\n",
			           writer.submitted_value, writer.submitted_layout_same ? 1u : 0u, writer.parsed_cmd_id, writer.submitted_cmd_id,
			           writer.parsed_cmd_offset, writer.submitted_cmd_offset,
			           !writer.submitted_layout_same ? "unknown" : (writer.submitted_value != writer.parsed_value ? "1" : "0"));
		} else
		{
			dbg_printf(" submitted=unavailable changed=unknown\n");
		}
	}
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
struct SubmissionAdmissionLockProbe
{
	Core::Mutex* mutex           = nullptr;
	bool         observed_locked = false;
	uint32_t     calls           = 0;
	bool         reader_active   = true;
};

static thread_local SubmissionAdmissionLockProbe* g_submission_admission_lock_probe = nullptr;

struct GpuHostWaitTestContext
{
	Android::FexCoreLab::GuestMemoryMutationBarrier* barrier = nullptr;
	void (*readmitted)(void*) = nullptr;
	void* data = nullptr;
	std::atomic_uint32_t waits = 0;
};
static thread_local GpuHostWaitTestContext* g_gpu_host_wait_test = nullptr;

enum class CommandEffectProbeValue : uint8_t
{
	MemoryBarrier,
	FullMemoryBarrier,
	WriteBack,
	QueueEndOfPipeInterrupt,
	WriteBack32Interrupt,
	WriteBackClock64Interrupt,
	QueuePixelDoneWrite32,
	ReadGds,
	EopVirtualClock,
	EopImmediate32Checkpoint,
};

struct CommandEffectProbe
{
	CommandProcessor* owner = nullptr;
	std::array<CommandEffectProbeValue, 16> effects = {};
	uint32_t count    = 0;
	bool     overflow = false;
	uint32_t interrupt_event_id = UINT32_MAX;
	uint32_t* destination32 = nullptr;
	uint64_t* destination64 = nullptr;
	uint32_t value32 = 0;
	uint64_t source_byte_offset = 0;
	uint32_t byte_count = 0;
	bool virtual_clock_enabled = false;
	bool immediate32_checkpoint_enabled = false;
	bool db_cache_proof_enabled = false;
	bool db_cache_admission_enabled = false;
	GpuMemoryDbCacheProofStatus db_cache_proof_status = GpuMemoryDbCacheProofStatus::Proven;
	uint32_t db_cache_proofs = 0;

	void Record(CommandEffectProbeValue effect)
	{
		if (count >= effects.size())
		{
			overflow = true;
			return;
		}
		effects[count++] = effect;
	}
};

static thread_local CommandEffectProbe* g_command_effect_probe = nullptr;

class CommandEffectProbeScope final
{
public:
	explicit CommandEffectProbeScope(CommandEffectProbe* probe): m_previous(g_command_effect_probe)
	{
		g_command_effect_probe = probe;
	}
	~CommandEffectProbeScope() { g_command_effect_probe = m_previous; }

	CommandEffectProbeScope(const CommandEffectProbeScope&)            = delete;
	CommandEffectProbeScope& operator=(const CommandEffectProbeScope&) = delete;

private:
	CommandEffectProbe* m_previous = nullptr;
};

static bool record_command_effect(CommandProcessor* owner, CommandEffectProbeValue effect)
{
	if (g_command_effect_probe == nullptr || g_command_effect_probe->owner != owner)
	{
		return false;
	}
	g_command_effect_probe->Record(effect);
	return true;
}

static bool command_effect_probe_active(CommandProcessor* owner)
{
	return g_command_effect_probe != nullptr && g_command_effect_probe->owner == owner;
}
#endif

class GraphicsRing
{
public:
	GraphicsRing(): m_job1("Thread_Gfx_Draw"), m_job2("Thread_Gfx_Const") { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~GraphicsRing() { LUMEN_NOT_IMPLEMENTED; }

	LUMEN_CLASS_NO_COPY(GraphicsRing);

	void Submit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw, int handle, int index,
	            int flip_mode, int64_t flip_arg);
	void Done();
	void WaitForIdle();
	bool IsSubmitReady();
	void WaitForSubmitReady();
	bool IsIdle();
	bool ReportRootOwnershipFinal();
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	void SetSubmitBusyForTest(bool busy)
	{
		Core::LockGuard lock(m_mutex);
		m_done = true;
		m_idle = !busy;
		if (!busy) m_idle_cond_var.SignalAll();
	}
	bool SubmitLockAvailableForTest()
	{
		if (!m_mutex.TryLock()) return false;
		m_mutex.Unlock();
		return true;
	}
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	void ResetIOSRuntimeSession();
	bool IsDormantIOSRuntimeSession();
#endif

	void SetCp(CommandProcessor* cp)
	{
		m_cp = cp;
		Start();
	}

private:
	void Start()
	{
		Core::Thread t(ThreadBatchRun, this);
		t.Detach();
	}

	struct CmdBuffer
	{
		uint32_t*                         data   = nullptr;
		uint32_t                          num_dw = 0;
		bool                              owned  = false;
		std::shared_ptr<Vector<uint32_t>> submitted;
	};
	static_assert(sizeof(CmdBuffer) == 32, "CmdBuffer ownership flag must stay in the existing 64-bit layout padding");

	struct CmdBatch
	{
		CmdBuffer draw_buffer;
		CmdBuffer const_buffer;

		CommandProcessor::FlipInfo flip;
	};

	static void ThreadBatchRun(void* data);

	CmdBatch GetCmdBatch();
	void     ReleaseProvenance(CmdBatch* batch);

	Core::Mutex          m_mutex;
	Core::CondVar        m_cond_var;
	Core::CondVar        m_idle_cond_var;
	Core::List<CmdBatch> m_cmd_batches;
	bool                 m_done = true;
	bool                 m_idle = true;
	uint64_t             m_provenance_dwords         = 0;
	bool                 m_provenance_config_reported = false;
	bool                 m_provenance_cap_reported    = false;
	uint64_t             m_root_ownership_decisions[static_cast<size_t>(RootCommandOwnershipDecision::Count)] = {};
	bool                 m_root_ownership_config_reported = false;
	uint64_t             m_root_ownership_dwords = 0;
	bool                 m_root_ownership_final_reported = false;
	bool                 m_root_ownership_final_clean = false;
	bool                 m_root_ownership_admission_closed = false;
	uint64_t             m_root_ownership_rejected = 0;
	uint64_t             m_root_ownership_capacity_failures = 0;
	std::shared_ptr<Vector<uint32_t>> m_root_ownership_storage;

	AsyncJob m_job1;
	AsyncJob m_job2;

	CommandProcessor* m_cp = nullptr;
};

class ComputeRing
{
public:
	ComputeRing() = default;
	virtual ~ComputeRing() { LUMEN_NOT_IMPLEMENTED; }

	LUMEN_CLASS_NO_COPY(ComputeRing);

	void MapComputeQueue(uint32_t* ring_addr, uint32_t ring_size_dw, uint32_t* read_ptr_addr);
	void DingDong(uint32_t offset_dw);
	void Done();
	void WaitForIdle();
	bool IsIdle();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	void ResetIOSRuntimeSession();
	bool CanResetIOSRuntimeSession();
	bool IsDormantIOSRuntimeSession();
#endif

	void SetCp(CommandProcessor* cp)
	{
		m_cp = cp;
		Start();
	}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	[[nodiscard]] CommandProcessor* GetCpForTest() const { return m_cp; }
#endif
	[[nodiscard]] int GetQueueId() const { return m_queue_id; }
	void              SetQueueId(int id) { m_queue_id = id; }

	void SetActive(bool flag);
	bool IsActive();

private:
	void Start()
	{
		Core::Thread t(ThreadRun, this);
		t.Detach();
	}

	static void ThreadRun(void* data);

	Core::Mutex   m_mutex;
	Core::CondVar m_cond_var;
	Core::CondVar m_idle_cond_var;
	bool          m_done = true;
	bool          m_idle = true;

	CommandProcessor* m_cp       = nullptr;
	int               m_queue_id = -1;

	bool      m_active                  = false;
	uint32_t* m_ring_addr               = nullptr;
	uint32_t* m_read_ptr_addr           = nullptr;
	uint32_t* m_internal_buffer         = nullptr;
	uint32_t  m_ring_size_dw            = 0;
	uint32_t  m_run_offset_dw           = 0;
	uint32_t  m_last_dingdong_offset_dw = 0;
};

class Gpu
{
public:
	Gpu()
	{
		EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());
		Init();
	}
	virtual ~Gpu() { LUMEN_NOT_IMPLEMENTED; }

	LUMEN_CLASS_NO_COPY(Gpu);

	void     Submit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw);
	void     SubmitAndFlip(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw, int handle,
	                       int index, int flip_mode, int64_t flip_arg);
	uint32_t MapComputeQueue(uint32_t pipe_id, uint32_t queue_id, uint32_t* ring_addr, uint32_t ring_size_dw, uint32_t* read_ptr_addr);
	void     Unmap(uint32_t ring_id);
	void     DingDong(uint32_t ring_id, uint32_t offset_dw);
	void     Done();
	[[nodiscard]] bool Wait();
	bool     ReportRootOwnershipFinal();
	int      GetFrameNum();
	bool     AreSubmitsAllowed();
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	bool SubmissionAdmissionSerializationForTest();
	bool InterruptRoutingForTest();
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	bool BeginIOSRuntimeSession();
	bool ResetIOSRuntimeSession();
	bool CanSkipCommandProcessorWaitAfterIOSReset();
#endif

private:
	void Init();
	bool WaitForWindowReady();

	ComputeRing* GetRing(uint32_t ring_id);

	Core::Mutex m_mutex;

	CommandProcessor* m_gfx_cp   = nullptr;
	GraphicsRing*     m_gfx_ring = nullptr;

	CommandProcessor* m_compute_cp[8]    = {};
	ComputeRing*      m_compute_ring[64] = {};

	std::atomic_int m_done_num = 0;
	std::atomic_bool m_window_ready = false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	bool m_ios_runtime_session_active                 = true;
	bool m_ios_runtime_session_resetting              = false;
	bool m_ios_runtime_device_idle_callbacks_safe     = false;
#endif
};

using hw_ctx_parser_func_t   = uint32_t (*)(LUMEN_HW_CTX_PARSER_ARGS);
using hw_uc_parser_func_t    = uint32_t (*)(LUMEN_HW_UC_PARSER_ARGS);
using hw_sh_parser_func_t    = uint32_t (*)(LUMEN_HW_SH_PARSER_ARGS);
using cp_op_parser_func_t    = uint32_t (*)(LUMEN_CP_OP_PARSER_ARGS);
using hw_ctx_indirect_func_t = void (*)(LUMEN_HW_CTX_INDIRECT_ARGS);
using hw_uc_indirect_func_t  = void (*)(LUMEN_HW_UC_INDIRECT_ARGS);
using hw_sh_indirect_func_t  = void (*)(LUMEN_HW_SH_INDIRECT_ARGS);

static hw_ctx_parser_func_t   g_hw_ctx_func[Pm4::CX_NUM]          = {};
static hw_ctx_indirect_func_t g_hw_ctx_indirect_func[Pm4::CX_NUM] = {};
static hw_sh_parser_func_t    g_hw_sh_func[Pm4::SH_NUM]           = {};
static hw_sh_indirect_func_t  g_hw_sh_indirect_func[Pm4::SH_NUM]  = {};
static hw_uc_parser_func_t    g_hw_uc_func[Pm4::UC_NUM]           = {};
static hw_uc_indirect_func_t  g_hw_uc_indirect_func[Pm4::UC_NUM]  = {};
static hw_sh_parser_func_t    g_hw_sh_custom_func[Pm4::R_NUM]     = {};
static cp_op_parser_func_t    g_cp_op_func[256]                   = {};
static cp_op_parser_func_t    g_cp_op_custom_func[Pm4::R_NUM]     = {};

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
static std::atomic_bool g_graphics_state_test_pause_requested = false;
static std::atomic_bool g_graphics_state_test_pause_reached   = false;
static std::atomic_bool g_graphics_state_test_pause_release   = false;
#endif

static Gpu* g_gpu = nullptr;

// A native wait may retain only process-owned GPU/ring objects and scalar
// guest addresses. Its restoration (including guest exception delivery) runs
// after every GPU/ring lock has been released.
class GpuNativeWaitScope final
{
public:
	GpuNativeWaitScope()
	{
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		m_test = g_gpu_host_wait_test;
		if (m_test != nullptr)
		{
			m_token = m_test->barrier->BeginQuiescentHostWait();
			EXIT_IF(m_token == 0);
			++m_test->waits;
			if (g_submission_admission_lock_probe != nullptr) g_submission_admission_lock_probe->reader_active = false;
			return;
		}
#endif
		m_wait.emplace();
	}
	~GpuNativeWaitScope()
	{
		m_wait.reset();
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		if (m_test != nullptr)
		{
			EXIT_IF(!m_test->barrier->EndQuiescentHostWait(m_token));
			if (g_submission_admission_lock_probe != nullptr) g_submission_admission_lock_probe->reader_active = true;
			if (m_test->readmitted != nullptr) m_test->readmitted(m_test->data);
		}
#endif
	}
	LUMEN_CLASS_NO_COPY(GpuNativeWaitScope);
private:
	std::optional<Loader::ScopedGuestHostWait> m_wait;
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	GpuHostWaitTestContext* m_test = nullptr;
	uint64_t m_token = 0;
#endif
};

// Never block on the outer GPU mutex while retaining an HLE reader: its owner
// can be waiting for a parser that needs a VM mutation to complete. Acquiring
// and releasing it during quiescence is only a wait, never lock ownership
// transferred across readmission. Reentrant submissions therefore remain safe.
class GpuAdmissionLock final
{
public:
	explicit GpuAdmissionLock(Core::Mutex& mutex): m_mutex(mutex)
	{
		while (!m_mutex.TryLock())
		{
			GpuNativeWaitScope host_wait;
			{ Core::LockGuard wait(m_mutex); }
			m_resumed = true;
		}
	}
	~GpuAdmissionLock() { m_mutex.Unlock(); }
	bool Resumed() const { return m_resumed; }
	LUMEN_CLASS_NO_COPY(GpuAdmissionLock);
private:
	Core::Mutex& m_mutex;
	bool m_resumed = false;
};

static bool gpu_submit_span_valid(const uint32_t* address, uint32_t words)
{
	if (words == 0) return true;
	const uint64_t bytes = static_cast<uint64_t>(words) * sizeof(uint32_t);
	return address != nullptr && bytes <= SIZE_MAX &&
	    Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(address), static_cast<size_t>(bytes), false);
}

template <typename Ready, typename NativeWait, typename Commit>
static void submit_with_gpu_readmission(Core::Mutex& mutex, Ready ready, NativeWait native_wait, Commit commit)
{
	bool resumed = false;
	for (;;)
	{
		{
			GpuAdmissionLock lock(mutex);
			resumed = resumed || lock.Resumed();
			if (ready())
			{
				commit(resumed);
				return;
			}
		}
		{
			GpuNativeWaitScope host_wait;
			Core::LockGuard lock(mutex);
			native_wait();
		}
		resumed = true;
	}
}

static void graphics_init_jmp_tables();

void GraphicsRunInit()
{
	EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (g_gpu != nullptr)
	{
		EXIT_IF(!g_gpu->BeginIOSRuntimeSession());
		return;
	}
#else
	EXIT_IF(g_gpu != nullptr);
#endif

	graphics_init_jmp_tables();

	g_gpu = new Gpu;
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
bool GraphicsRunResetIOSRuntimeSession()
{
	if (g_gpu == nullptr || !Loader::GuestExecutionIsIdle())
	{
		return false;
	}
	return g_gpu->ResetIOSRuntimeSession();
}
#endif

bool Gpu::WaitForWindowReady()
{
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	if (g_submission_admission_lock_probe != nullptr) return false;
#endif
	if (m_window_ready.load(std::memory_order_acquire)) return false;
	{
		GpuNativeWaitScope host_wait;
		WindowWaitForGraphicInitialized();
	}
	m_window_ready.store(true, std::memory_order_release);
	return true;
}

void Gpu::Submit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw)
{
	const bool window_waited = WaitForWindowReady();
	submit_with_gpu_readmission(m_mutex, [&]
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		EXIT_IF(!m_ios_runtime_session_active);
#endif
		return m_gfx_ring->IsSubmitReady();
	}, [&] { m_gfx_ring->WaitForSubmitReady(); }, [&](bool resumed)
	{
		EXIT_IF((resumed || window_waited) && (!gpu_submit_span_valid(cmd_draw_buffer, num_draw_dw) ||
		                    !gpu_submit_span_valid(cmd_const_buffer, num_const_dw)));
		m_gfx_ring->Submit(cmd_draw_buffer, num_draw_dw, cmd_const_buffer, num_const_dw, 0, 0, 0, 0);
	});
}

void Gpu::SubmitAndFlip(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw, int handle,
                        int index, int flip_mode, int64_t flip_arg)
{
	(void)VideoOut::VideoOutPrepareEventPayload(static_cast<uint64_t>(flip_arg));
	const bool window_waited = WaitForWindowReady();
	// Wait holds this admission lock through the all-queue retirement cutoff.
	// Flip submissions must obey the same GPU -> ring order as ordinary submits.
	submit_with_gpu_readmission(m_mutex, [&]
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		EXIT_IF(!m_ios_runtime_session_active);
#endif
		return m_gfx_ring->IsSubmitReady();
	}, [&] { m_gfx_ring->WaitForSubmitReady(); }, [&](bool resumed)
	{
		EXIT_IF((resumed || window_waited) && (!gpu_submit_span_valid(cmd_draw_buffer, num_draw_dw) ||
		                    !gpu_submit_span_valid(cmd_const_buffer, num_const_dw)));
		m_gfx_ring->Submit(cmd_draw_buffer, num_draw_dw, cmd_const_buffer, num_const_dw, handle, index, flip_mode, flip_arg);
	});
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool Gpu::SubmissionAdmissionSerializationForTest()
{
	EXIT_IF(g_submission_admission_lock_probe != nullptr);
	SubmissionAdmissionLockProbe probe {&m_mutex};
	g_submission_admission_lock_probe = &probe;
	Submit(nullptr, 0, nullptr, 0);
	const bool ordinary_locked = probe.calls == 1 && probe.observed_locked;
	probe.observed_locked = false;
	SubmitAndFlip(nullptr, 0, nullptr, 0, 0, 0, 0, 0);
	g_submission_admission_lock_probe = nullptr;
	if (!ordinary_locked || probe.calls != 2 || !probe.observed_locked) return false;

	using Barrier = Android::FexCoreLab::GuestMemoryMutationBarrier;
	uint32_t cases = 0;
	for (const bool mutex_contention: {true, false})
	{
		for (const bool reentrant_submit: {false, true})
		{
			Barrier barrier;
			struct RestoreContext
			{
				Gpu* gpu;
				bool reenter;
				bool locks_free = true;
				bool reentered = false;
			} restored {this, reentrant_submit};
			GpuHostWaitTestContext wait_context;
			wait_context.barrier = &barrier;
			wait_context.data = &restored;
			wait_context.readmitted = [](void* opaque)
			{
				auto& state = *static_cast<RestoreContext*>(opaque);
				bool available = false;
				std::thread probe_locks([&]
				{
					available = state.gpu->m_mutex.TryLock();
					if (available)
					{
						// The real ring mutex must also be available at restoration.
						available = state.gpu->m_gfx_ring->SubmitLockAvailableForTest();
						state.gpu->m_mutex.Unlock();
					}
				});
				probe_locks.join();
				state.locks_free = state.locks_free && available;
				if (state.reenter && !state.reentered)
				{
					state.reentered = true;
					state.gpu->Submit(nullptr, 0, nullptr, 0);
					// A headless submission has no Vulkan worker. Publish its pending
					// batch condition through the actual ring state/condition variable.
					state.gpu->m_gfx_ring->SetSubmitBusyForTest(true);
				}
			};
			std::atomic_bool started = false;
			bool submit_ok = false;
			SubmissionAdmissionLockProbe submitted {&m_mutex};
			if (mutex_contention) m_mutex.Lock();
			else m_gfx_ring->SetSubmitBusyForTest(true);
			std::thread submitter([&]
			{
				const bool admitted = barrier.EnterExecution() && barrier.SuspendExecutionForHostCall();
				g_gpu_host_wait_test = &wait_context;
				g_submission_admission_lock_probe = &submitted;
				started.store(true, std::memory_order_release);
				if (admitted) SubmitAndFlip(nullptr, 0, nullptr, 0, 0, 0, 0, 0);
				g_submission_admission_lock_probe = nullptr;
				g_gpu_host_wait_test = nullptr;
				submit_ok = admitted && submitted.reader_active && submitted.observed_locked &&
				    submitted.calls == (reentrant_submit ? 2u : 1u) && barrier.ResumeExecutionAfterHostCall();
				barrier.LeaveExecution();
			});
			auto wait_until = [](auto predicate)
			{
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
				while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
				return predicate();
			};
			bool valid = wait_until([&] { return started.load(std::memory_order_acquire); });
			auto mutate = [&]
			{
				const auto token = barrier.BeginMutation([](uint64_t) { return true; }, [](uint64_t) {},
				    std::chrono::milliseconds(500));
				return token != 0 && barrier.EndMutation(token);
			};
			valid = mutate() && valid;
			if (mutex_contention) m_mutex.Unlock();
			else m_gfx_ring->SetSubmitBusyForTest(false);
			if (reentrant_submit)
			{
				valid = wait_until([&] { return wait_context.waits.load() >= 2; }) && valid;
				valid = mutate() && valid;
				m_gfx_ring->SetSubmitBusyForTest(false);
			}
			submitter.join();
			m_gfx_ring->SetSubmitBusyForTest(false);
			if (!valid || !submit_ok || !restored.locks_free || restored.reentered != reentrant_submit ||
			    wait_context.waits.load() != (reentrant_submit ? 2u : 1u))
			{
				std::fprintf(stderr, "[GFX_SUBMIT_READMISSION_TEST] failed mutex=%u reenter=%u valid=%u submit=%u waits=%u\n",
				    mutex_contention, reentrant_submit, valid, submit_ok, wait_context.waits.load());
				return false;
			}
			++cases;
		}
	}
	if (!gpu_submit_span_valid(nullptr, 0) || gpu_submit_span_valid(nullptr, 1) ||
	    gpu_submit_span_valid(reinterpret_cast<uint32_t*>(UINT64_MAX - 3), 2)) return false;
	std::fprintf(stderr, "[GFX_SUBMIT_READMISSION_TEST] cases=%u gpu/ring-contention=real writer=FEX-barrier"
	    " readmission-locks=free reentrant-submit=rechecked guest-span=validated\n", cases);
	return true;
}
#endif

uint32_t Gpu::MapComputeQueue(uint32_t pipe_id, uint32_t queue_id, uint32_t* ring_addr, uint32_t ring_size_dw, uint32_t* read_ptr_addr)
{
	EXIT_NOT_IMPLEMENTED(ring_addr == nullptr);
	EXIT_NOT_IMPLEMENTED(ring_size_dw == 0);
	EXIT_NOT_IMPLEMENTED(read_ptr_addr == nullptr);

	GpuAdmissionLock lock(m_mutex);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	EXIT_IF(!m_ios_runtime_session_active);
#endif
	EXIT_IF(lock.Resumed() && (!gpu_submit_span_valid(ring_addr, ring_size_dw) ||
	    !Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(read_ptr_addr), sizeof(*read_ptr_addr), true)));

	auto v = pipe_id * 8 + queue_id;

	EXIT_NOT_IMPLEMENTED(v >= 64);

	//	for (int i = 0; i < 8; i++)
	//	{
	//		EXIT_NOT_IMPLEMENTED(m_compute_ring[pipe_id * 8 + i] != nullptr && m_compute_ring[pipe_id * 8 + i]->IsActive());
	//	}

	auto* ring = GetRing(v + 1);

	ring->MapComputeQueue(ring_addr, ring_size_dw, read_ptr_addr);

	EXIT_IF(!ring->IsActive());

	*read_ptr_addr = 0;

	return v + 1;
}

void Gpu::Unmap(uint32_t ring_id)
{
	GpuAdmissionLock lock(m_mutex);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	EXIT_IF(!m_ios_runtime_session_active);
#endif

	EXIT_NOT_IMPLEMENTED(ring_id < 1 || ring_id > 64);

	auto* ring = GetRing(ring_id);

	EXIT_NOT_IMPLEMENTED(!ring->IsActive());

	ring->SetActive(false);
}

void Gpu::DingDong(uint32_t ring_id, uint32_t offset_dw)
{
	(void)WaitForWindowReady();
	// No guest data or retirement callbacks: only queue IDs, native queue
	// counters and the window/context readiness wait. Resolve the current
	// mapping under the GPU mutex and keep it stable through the offset update.
	GpuNativeWaitScope host_wait;
	Core::LockGuard lock(m_mutex);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	EXIT_IF(!m_ios_runtime_session_active);
#endif

	EXIT_NOT_IMPLEMENTED(ring_id < 1 || ring_id > 64);
	EXIT_NOT_IMPLEMENTED(offset_dw == 0);

	auto* ring = GetRing(ring_id);

	EXIT_NOT_IMPLEMENTED(!ring->IsActive());

	ring->DingDong(offset_dw);
}

void Gpu::Done()
{
	// Ring Done only waits for native idle flags and sets native done flags.
	GpuNativeWaitScope host_wait;
	Core::LockGuard lock(m_mutex);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	EXIT_IF(!m_ios_runtime_session_active);
#endif

	m_gfx_ring->Done();
	for (auto& cr: m_compute_ring)
	{
		if (cr != nullptr)
		{
			cr->Done();
		}
	}

	m_done_num++;
}

bool Gpu::ReportRootOwnershipFinal()
{
	EXIT_IF(m_gfx_ring == nullptr);
	return m_gfx_ring->ReportRootOwnershipFinal();
}

bool Gpu::AreSubmitsAllowed()
{
	GpuAdmissionLock lock(m_mutex);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (!m_ios_runtime_session_active)
	{
		return true;
	}
#endif

	if (m_gfx_ring->IsIdle())
	{
		for (auto& cr: m_compute_ring)
		{
			if (cr != nullptr)
			{
				if (!cr->IsIdle())
				{
					return false;
				}
			}
		}
		return true;
	}
	return false;
}

int Gpu::GetFrameNum()
{
	// Core::LockGuard lock(m_mutex);

	return m_done_num;
}

// The parser may need a VM writer to finish a guest snapshot. Keep the host-call
// reader quiescent only while waiting for parser-owned rings, never while
// BufferWait runs retirement callbacks. Readmission can deliver a guest
// exception, so it must also happen outside the GPU and ring mutexes.
template <typename QuiescentWait, typename SessionActive, typename WaitRings, typename RingsIdle, typename Complete>
static bool wait_for_gpu_idle_with_readmission(Core::Mutex& mutex, QuiescentWait quiescent_wait, SessionActive session_active,
                                             WaitRings wait_rings, RingsIdle rings_idle, Complete complete)
{
	for (;;)
	{
		bool active = false;
		quiescent_wait([&]
		{
			Core::LockGuard lock(mutex);
			active = session_active();
			if (active) wait_rings();
		});
		if (!active) return false;

		GpuAdmissionLock lock(mutex);
		if (!session_active()) return false;
		// A submission (including from a delivered exception) may enter between
		// the pure wait and readmission. It revokes the earlier idle observation.
		// Repeat the synchronous wait without inventing a success or retry limit.
		if (!rings_idle()) continue;
		complete();
		return true;
	}
}

bool Gpu::Wait()
{
	auto session_active = [&]
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
		// A reset session is already idle. Its memory teardown must not lazily
		// allocate or touch Vulkan command buffers in the dormant interval.
		return m_ios_runtime_session_active;
#else
		return true;
#endif
	};
	return wait_for_gpu_idle_with_readmission(m_mutex,
	    [](auto wait_rings)
	    {
		    Loader::ScopedGuestHostWait host_wait;
		    wait_rings();
	    }, session_active,
	    [&]
	    {
		    m_gfx_ring->WaitForIdle();
		    for (auto* ring: m_compute_ring)
		    {
			    if (ring != nullptr) ring->WaitForIdle();
		    }
	    },
	    [&]
	    {
		    if (!m_gfx_ring->IsIdle()) return false;
		    for (auto* ring: m_compute_ring)
		    {
			    if (ring != nullptr && !ring->IsIdle()) return false;
		    }
		    return true;
	    },
	    [&]
	    {
		    m_gfx_cp->BufferWait();
		    for (auto* cp: m_compute_cp)
		    {
			    if (cp != nullptr) cp->BufferWait();
		    }
		    // Only this rechecked all-queue idle observation establishes the
		    // retirement cutoff, still excluding Submit/DingDong with m_mutex.
		    GpuMemoryMarkRetiredSampledTexturesReadyAfterAllQueuesIdle();
	    });
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
static bool graphics_run_gpu_wait_readmission_test()
{
	enum class Scenario { Idle, SubmitAtReadmission, RepeatedSubmits, DormantBeforeWait, DormantAtReadmission };
	uint32_t cases = 0;
	for (const auto scenario: {Scenario::Idle, Scenario::SubmitAtReadmission, Scenario::RepeatedSubmits,
	                          Scenario::DormantBeforeWait, Scenario::DormantAtReadmission})
	{
		Core::Mutex gpu_mutex, ring_mutex, reader_mutex;
		bool active = scenario != Scenario::DormantBeforeWait;
		bool reader_active = true, valid = true;
		std::array<bool, 3> queued {true, true, false};
		uint32_t waits = 0, readmissions = 0, idle_checks = 0, completions = 0;
		const uint32_t submissions = scenario == Scenario::SubmitAtReadmission ? 1u :
		    scenario == Scenario::RepeatedSubmits ? 20u : 0u;
		uint32_t submissions_remaining = submissions, submitted_generation = 1, drained_generation = 0, cutoff = 0;
		std::atomic_bool writer_progress = false, stop_writer = false;
		reader_mutex.Lock();
		// Model a mutation writer that cannot progress while the waiting HLE
		// retains its reader. The bounded polling makes a broken ordering fail
		// the fixture instead of hanging the test executable.
		std::thread writer([&]
		{
			while (!stop_writer.load(std::memory_order_acquire))
			{
				if (reader_mutex.TryLock())
				{
					reader_mutex.Unlock();
					writer_progress.store(true, std::memory_order_release);
					return;
				}
				std::this_thread::yield();
			}
		});
		auto probe_locks = [&](bool gpu_available)
		{
			bool observed = false;
			std::thread probe([&]
			{
				const bool gpu = gpu_mutex.TryLock();
				const bool ring = ring_mutex.TryLock();
				observed = gpu == gpu_available && ring;
				if (ring) ring_mutex.Unlock();
				if (gpu) gpu_mutex.Unlock();
			});
			probe.join();
			valid = valid && observed;
		};
		const bool result = wait_for_gpu_idle_with_readmission(gpu_mutex,
		    [&](auto wait_rings)
		    {
			    probe_locks(true);
			    valid = valid && reader_active;
			    reader_active = false;
			    reader_mutex.Unlock();
			    wait_rings();
			    // A restored HLE can deliver an exception which re-enters Submit.
			    // Its admission and ring locks must both be free before restoration.
			    probe_locks(true);
			    reader_mutex.Lock();
			    reader_active = true;
			    ++readmissions;
			    if (scenario == Scenario::DormantAtReadmission) active = false;
			    if (submissions_remaining != 0)
			    {
				    Core::LockGuard gpu_lock(gpu_mutex);
				    Core::LockGuard ring_lock(ring_mutex);
				    // The newly active compute ring was absent from the first wait.
				    queued[2] = true;
				    ++submitted_generation;
				    --submissions_remaining;
			    }
		    },
		    [&] { return active; },
		    [&]
		    {
			    ++waits;
			    probe_locks(false);
			    valid = valid && !reader_active && completions == 0;
			    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			    while (!writer_progress.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
			    {
				    std::this_thread::yield();
			    }
			    valid = valid && writer_progress.load(std::memory_order_acquire);
			    Core::LockGuard ring_lock(ring_mutex);
			    queued.fill(false);
			    drained_generation = submitted_generation;
		    },
		    [&]
		    {
			    ++idle_checks;
			    valid = valid && reader_active;
			    Core::LockGuard ring_lock(ring_mutex);
			    return std::none_of(queued.begin(), queued.end(), [](bool pending) { return pending; });
		    },
		    [&]
		    {
			    probe_locks(false);
			    // BufferWait and its retirement callbacks retain ordinary admission;
			    // the authoritative cutoff includes the final restored submission.
			    valid = valid && reader_active && writer_progress.load(std::memory_order_acquire) &&
			        drained_generation == submitted_generation && submissions_remaining == 0 && idle_checks == waits;
			    ++completions;
			    cutoff = drained_generation;
		    });
		reader_mutex.Unlock();
		stop_writer.store(true, std::memory_order_release);
		writer.join();
		const bool dormant = scenario == Scenario::DormantBeforeWait || scenario == Scenario::DormantAtReadmission;
		const uint32_t expected_waits = scenario == Scenario::DormantBeforeWait ? 0u : 1u + submissions;
		if (!valid || result == dormant || !reader_active || waits != expected_waits || readmissions != 1u + submissions ||
		    idle_checks != (dormant ? 0u : expected_waits) || completions != (dormant ? 0u : 1u) ||
		    cutoff != (dormant ? 0u : submitted_generation))
		{
			std::fprintf(stderr, "[GFX_GPU_WAIT_READMISSION_TEST] case=%u valid=%u result=%u waits=%u restores=%u"
			    " checks=%u completions=%u cutoff=%u submitted=%u\n", static_cast<unsigned>(scenario), valid ? 1u : 0u,
			    result ? 1u : 0u, waits, readmissions, idle_checks, completions, cutoff, submitted_generation);
			return false;
		}
		++cases;
	}
	std::fprintf(stderr, "[GFX_GPU_WAIT_READMISSION_TEST] cases=%u writer-progress=modeled restore-gpu/ring-locks=free"
	    " new-compute-submit=revokes-idle repeated-submits=20 callbacks-reader=retained cutoff=after-final-recheck\n", cases);
	return true;
}
#endif


void Gpu::Init()
{
	EXIT_IF(m_gfx_cp != nullptr);
	EXIT_IF(m_gfx_ring != nullptr);

	m_gfx_cp   = new CommandProcessor(64);
	m_gfx_ring = new GraphicsRing;
	m_gfx_cp->SetQueue(GraphicContext::QUEUE_GFX);
	m_gfx_ring->SetCp(m_gfx_cp);

	EXIT_IF(GraphicContext::QUEUE_COMPUTE_NUM < 8);
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
bool Gpu::BeginIOSRuntimeSession()
{
	Core::LockGuard lock(m_mutex);
	if (m_ios_runtime_session_active || m_ios_runtime_session_resetting || !m_gfx_ring->IsDormantIOSRuntimeSession())
	{
		return false;
	}
	for (auto* ring: m_compute_ring)
	{
		if (ring != nullptr && !ring->IsDormantIOSRuntimeSession())
		{
			return false;
		}
	}
	m_ios_runtime_device_idle_callbacks_safe = false;
	m_window_ready.store(false, std::memory_order_release);
	m_ios_runtime_session_active = true;
	return true;
}

bool Gpu::ResetIOSRuntimeSession()
{
	m_mutex.Lock();
	if (!m_ios_runtime_session_active || m_ios_runtime_session_resetting || !m_gfx_ring->IsIdle())
	{
		m_mutex.Unlock();
		return false;
	}
	for (auto* ring: m_compute_ring)
	{
		if (ring != nullptr && !ring->CanResetIOSRuntimeSession())
		{
			m_mutex.Unlock();
			return false;
		}
	}

	// The caller has already joined the guest and waited the Vulkan device idle.
	// Mark the manager dormant before releasing its mutex: retired-resource
	// callbacks may call back into GraphicsRun, and must never run under this
	// outer lock. A failed reset leaves resetting set and the manager fail-closed.
	m_ios_runtime_session_resetting = true;
	m_ios_runtime_session_active    = false;
	m_ios_runtime_device_idle_callbacks_safe = false;
	m_gfx_ring->ResetIOSRuntimeSession();
	for (auto* ring: m_compute_ring)
	{
		if (ring != nullptr)
		{
			ring->ResetIOSRuntimeSession();
		}
	}
	m_mutex.Unlock();

	bool reset_ok = m_gfx_cp->ResetIOSRuntimeSession();
	if (reset_ok)
	{
		for (auto* cp: m_compute_cp)
		{
			if (cp != nullptr && !cp->ResetIOSRuntimeSession())
			{
				reset_ok = false;
				break;
			}
		}
	}
	if (reset_ok)
	{
		// Every command buffer in every processor is now reset after the proven
		// device-idle barrier. Retired callbacks can safely bypass BufferWait even
		// while the reset transaction itself is still in progress.
		m_mutex.Lock();
		m_ios_runtime_device_idle_callbacks_safe = true;
		m_mutex.Unlock();
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		reset_ok = GpuMemoryRetiredStorageWaitContractTest(m_gfx_cp);
#endif
	}
	if (reset_ok)
	{
		m_gfx_cp->ReleaseIOSRuntimeSessionRetiredResources();
		for (auto* cp: m_compute_cp)
		{
			if (cp != nullptr)
			{
				cp->ReleaseIOSRuntimeSessionRetiredResources();
			}
		}
	}

	m_mutex.Lock();
	if (reset_ok)
	{
		m_done_num.store(0, std::memory_order_relaxed);
		m_ios_runtime_session_resetting = false;
	}
	m_mutex.Unlock();
	return reset_ok;
}

bool Gpu::CanSkipCommandProcessorWaitAfterIOSReset()
{
	Core::LockGuard lock(m_mutex);
	if (m_ios_runtime_session_active || !m_ios_runtime_device_idle_callbacks_safe || !m_gfx_ring->IsDormantIOSRuntimeSession())
	{
		return false;
	}
	for (auto* ring: m_compute_ring)
	{
		if (ring != nullptr && !ring->IsDormantIOSRuntimeSession())
		{
			return false;
		}
	}
	return true;
}
#endif

ComputeRing* Gpu::GetRing(uint32_t ring_id)
{
	int v        = static_cast<int>(ring_id - 1);
	int pipe_id  = v / 8;
	int queue_id = v % 8;

	if (m_compute_cp[pipe_id] == nullptr)
	{
		// One processor is shared by all queues of this pipe. The event identity
		// is independent of the backend queue and remains stable across resets.
		m_compute_cp[pipe_id] = new CommandProcessor(pipe_id);
		m_compute_cp[pipe_id]->SetQueue(GraphicContext::QUEUE_COMPUTE_START + pipe_id);
	}

	if (m_compute_ring[v] == nullptr)
	{
		m_compute_ring[v] = new ComputeRing;
		m_compute_ring[v]->SetQueueId(queue_id);
		m_compute_ring[v]->SetActive(false);
		m_compute_ring[v]->SetCp(m_compute_cp[pipe_id]);
	}

	return m_compute_ring[v];
}

bool CommandProcessor::BeginTrackedCommandBufferSubmission(int queue, uint64_t* session_epoch,
	                                                         uint64_t* recording_submit_serial,
	                                                         uint64_t* begin_submit_serial_cutoff,
	                                                         uint64_t* begin_completed_serial_cutoff)
{
	if (session_epoch != nullptr)
	{
		*session_epoch = 0;
	}
	if (recording_submit_serial != nullptr)
	{
		*recording_submit_serial = 0;
	}
	if (begin_submit_serial_cutoff != nullptr)
	{
		*begin_submit_serial_cutoff = 0;
	}
	if (begin_completed_serial_cutoff != nullptr)
	{
		*begin_completed_serial_cutoff = 0;
	}
	if (queue != m_queue || session_epoch == nullptr || recording_submit_serial == nullptr ||
	    begin_submit_serial_cutoff == nullptr || begin_completed_serial_cutoff == nullptr ||
	    !m_tracked_submission_timeline_valid.load(std::memory_order_acquire))
	{
		return false;
	}

	const uint64_t epoch     = m_tracked_submission_session_epoch.load(std::memory_order_acquire);
	const uint64_t submitted = m_tracked_submission_submitted_serial.load(std::memory_order_acquire);
	const uint64_t completed = m_tracked_submission_completed_serial.load(std::memory_order_acquire);
	if (epoch == 0 || submitted == UINT64_MAX || completed > submitted)
	{
		m_tracked_submission_timeline_valid.store(false, std::memory_order_release);
		return false;
	}

	// A command processor records and submits one buffer at a time. A gap means
	// that a prior recording was discarded, so the diagnostic timeline stays
	// fail-closed until the next proven session reset.
	uint64_t expected_assigned = submitted;
	const uint64_t candidate   = submitted + 1u;
	if (!m_tracked_submission_assigned_serial.compare_exchange_strong(expected_assigned, candidate,
	                                                                 std::memory_order_acq_rel,
	                                                                 std::memory_order_acquire) ||
	    m_tracked_submission_session_epoch.load(std::memory_order_acquire) != epoch ||
	    !m_tracked_submission_timeline_valid.load(std::memory_order_acquire))
	{
		m_tracked_submission_timeline_valid.store(false, std::memory_order_release);
		return false;
	}

	*session_epoch                  = epoch;
	*recording_submit_serial        = candidate;
	*begin_submit_serial_cutoff     = submitted;
	*begin_completed_serial_cutoff  = completed;
	return true;
}

bool CommandProcessor::RecordTrackedCommandBufferSubmission(int queue, uint64_t session_epoch,
	                                                          uint64_t recording_submit_serial)
{
	if (!m_tracked_submission_timeline_valid.load(std::memory_order_acquire))
	{
		return false;
	}
	if (queue != m_queue || session_epoch == 0 || recording_submit_serial == 0 ||
	    m_tracked_submission_session_epoch.load(std::memory_order_acquire) != session_epoch)
	{
		// This hook is reached only after vkQueueSubmit succeeds. Losing that
		// submission would make every later cutoff optimistic, so poison the
		// diagnostic epoch immediately rather than waiting for the next Begin.
		m_tracked_submission_timeline_valid.store(false, std::memory_order_release);
		return false;
	}

	const uint64_t assigned  = m_tracked_submission_assigned_serial.load(std::memory_order_acquire);
	const uint64_t submitted = m_tracked_submission_submitted_serial.load(std::memory_order_acquire);
	if (recording_submit_serial != assigned || submitted == UINT64_MAX || recording_submit_serial != submitted + 1u)
	{
		m_tracked_submission_timeline_valid.store(false, std::memory_order_release);
		return false;
	}

	m_tracked_submission_submitted_serial.store(recording_submit_serial, std::memory_order_release);
	return true;
}

void CommandProcessor::RecordTrackedCommandBufferCompletion(int queue, uint64_t session_epoch,
	                                                          uint64_t recording_submit_serial)
{
	if (queue != m_queue || session_epoch == 0 || recording_submit_serial == 0 ||
	    !m_tracked_submission_timeline_valid.load(std::memory_order_acquire) ||
	    m_tracked_submission_session_epoch.load(std::memory_order_acquire) != session_epoch)
	{
		return;
	}

	const uint64_t submitted = m_tracked_submission_submitted_serial.load(std::memory_order_acquire);
	if (recording_submit_serial > submitted)
	{
		return;
	}
	uint64_t completed = m_tracked_submission_completed_serial.load(std::memory_order_acquire);
	while (completed < recording_submit_serial &&
	       !m_tracked_submission_completed_serial.compare_exchange_weak(completed, recording_submit_serial,
	                                                                    std::memory_order_acq_rel,
	                                                                    std::memory_order_acquire))
	{
	}
}

bool CommandProcessor::HasSubmittedTrackedCommandBuffer(int queue, uint64_t session_epoch,
                                                       uint64_t recording_submit_serial) const
{
	// The caller owns m_mutex, excluding queue/session changes and submissions.
	// RecordTrackedCommandBufferSubmission advances this contiguous prefix only
	// after native submission succeeds; Begin merely assigns the next serial.
	// This proves submission on the same queue, never completion or content.
	if (queue < 0 || queue >= GraphicContext::QUEUES_NUM || queue != m_queue ||
	    session_epoch == 0 || recording_submit_serial == 0 ||
	    !m_tracked_submission_timeline_valid.load(std::memory_order_acquire) ||
	    m_tracked_submission_session_epoch.load(std::memory_order_acquire) != session_epoch) return false;
	const uint64_t completed = m_tracked_submission_completed_serial.load(std::memory_order_acquire);
	const uint64_t submitted = m_tracked_submission_submitted_serial.load(std::memory_order_acquire);
	return recording_submit_serial <= submitted && completed <= submitted &&
	       m_tracked_submission_session_epoch.load(std::memory_order_acquire) == session_epoch &&
	       m_tracked_submission_timeline_valid.load(std::memory_order_acquire);
}

bool CommandProcessor::HasCompletedTrackedCommandBuffer(int queue, uint64_t session_epoch,
                                                       uint64_t recording_submit_serial) const
{
	// The caller owns m_mutex, excluding queue/session changes and submissions.
	// Completion may advance asynchronously; a conservative older value merely
	// rejects the request. Invalid or replaced timelines never prove completion.
	if (queue < 0 || queue != m_queue || session_epoch == 0 || recording_submit_serial == 0 ||
	    !m_tracked_submission_timeline_valid.load(std::memory_order_acquire) ||
	    m_tracked_submission_session_epoch.load(std::memory_order_acquire) != session_epoch) return false;
	const uint64_t completed = m_tracked_submission_completed_serial.load(std::memory_order_acquire);
	const uint64_t submitted = m_tracked_submission_submitted_serial.load(std::memory_order_acquire);
	return recording_submit_serial <= completed && completed <= submitted &&
	       m_tracked_submission_session_epoch.load(std::memory_order_acquire) == session_epoch &&
	       m_tracked_submission_timeline_valid.load(std::memory_order_acquire);
}

void CommandProcessor::ResetTrackedCommandBufferSubmissionTimeline()
{
	m_tracked_submission_timeline_valid.store(false, std::memory_order_release);
	m_tracked_submission_assigned_serial.store(0, std::memory_order_relaxed);
	m_tracked_submission_submitted_serial.store(0, std::memory_order_relaxed);
	m_tracked_submission_completed_serial.store(0, std::memory_order_relaxed);
	const uint64_t epoch = m_tracked_submission_session_epoch.load(std::memory_order_relaxed);
	if (epoch == UINT64_MAX)
	{
		m_tracked_submission_session_epoch.store(0, std::memory_order_release);
		return;
	}
	m_tracked_submission_session_epoch.store(epoch + 1u, std::memory_order_release);
	m_tracked_submission_timeline_valid.store(true, std::memory_order_release);
}

void CommandProcessor::Reset()
{
	Core::LockGuard lock(m_mutex);

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	if (!m_headless_test_mode)
#endif
	{
		BufferWait();

		GraphicsRenderDeleteIndexBuffers();
	}

	m_sh_ctx.Reset();
	m_ucfg.Reset();
	m_ctx.Reset();
	if (g_command_processor_parser_state != nullptr && g_command_processor_parser_state->owner == this)
	{
		g_command_processor_parser_state->marker = HW::UserSgprType::Unknown;
	}
	m_index_type_and_size = 0;
	m_index_buffer_size = IndexBufferSizeState {};
	m_user_sgpr_write_sequence = 0;
	std::memset(m_user_sgpr_writers, 0, sizeof(m_user_sgpr_writers));

	std::memset(m_const_ram, 0, sizeof(m_const_ram));
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
bool CommandProcessor::ResetIOSRuntimeSession()
{
	// Ring idleness guarantees no parser owns this processor. Taking the run
	// lock makes that contract fail-safe against an unexpected late worker.
	bool reset_ok = true;
	RunLock();
	{
		Core::LockGuard lock(m_mutex);
		for (auto* buffer: m_buffer)
		{
			if (buffer != nullptr && !buffer->ResetAfterDeviceIdle())
			{
				reset_ok = false;
				break;
			}
		}
		if (reset_ok)
		{
			if (GpuMemoryAccessTrackingEnabled())
			{
				ResetTrackedCommandBufferSubmissionTimeline();
			}
			m_current_buffer = -1;
			m_sh_ctx.Reset();
			m_ucfg.Reset();
			m_ctx.Reset();
			m_index_type_and_size = 0;
			m_index_buffer_size   = IndexBufferSizeState {};
			m_num_instances       = 1;
			m_flip                = FlipInfo {};
			m_sumbit_id           = 0;
			m_user_sgpr_write_sequence = 0;
			std::memset(m_user_sgpr_writers, 0, sizeof(m_user_sgpr_writers));
			std::memset(m_const_ram, 0, sizeof(m_const_ram));
		}
	}
	if (reset_ok)
	{
		ResetDeCe();
	}
	RunUnlock();
	if (!reset_ok)
	{
		return false;
	}

	return true;
}

void CommandProcessor::ReleaseIOSRuntimeSessionRetiredResources()
{
	// The GPU coordinator invokes this only after every command processor has
	// reset successfully and exposes the device-idle callback-safe phase. Run
	// deletion callbacks after both command-processor locks are gone.
	for (auto* buffer: m_buffer)
	{
		if (buffer != nullptr)
		{
			buffer->ReleaseRetiredAfterDeviceIdle();
		}
	}
}
#endif

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool CommandProcessor::IsIOSRuntimeSessionResetForTest()
{
	Core::LockGuard lock(m_mutex);
	bool            const_ram_clear = true;
	for (const auto value: m_const_ram)
	{
		const_ram_clear = const_ram_clear && value == 0;
	}
	return m_current_buffer == -1 && m_index_type_and_size == 0 && m_index_buffer_size.index_count == 0 &&
	       !m_index_buffer_size.configured && m_num_instances == 1 && m_flip.handle == 0 && m_flip.index == 0 &&
	       m_flip.flip_mode == 0 && m_flip.flip_arg == 0 && m_sumbit_id == 0 && const_ram_clear;
}
#endif

void CommandProcessor::BufferInit()
{
	Core::LockGuard lock(m_mutex);

	if (m_current_buffer < 0)
	{
		VulkanCommandPool* processor_pool = nullptr;
		for (auto& buf: m_buffer)
		{
			if (buf == nullptr)
			{
				buf = new CommandBuffer(m_queue);
				buf->SetParent(this);
			} else
			{
				EXIT_IF(buf->IsInvalid());
			}
			// Initial allocation runs before parser jobs and fills one thread-local
			// pool. Utility buffers spill into separate chunks after this ring.
			if (processor_pool == nullptr) processor_pool = buf->GetPool();
			EXIT_IF(buf->GetPool() != processor_pool);
		}

		m_current_buffer = 0;
		m_buffer[m_current_buffer]->Begin();
	}
}

void CommandProcessor::BufferFlush(bool host_read)
{
	Lumen::CriticalPath::Scope trace(Lumen::CriticalPath::Reason::GpuBufferFlush, static_cast<uint64_t>(m_queue + 1));
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);
	EXIT_IF(m_buffer[m_current_buffer] == nullptr);

	if (host_read)
	{
		// Coherent mappings still need shader writes published to the host.
		// Record on this processor's queue so the following submission/fence
		// also covers its previously submitted work. ALL_COMMANDS includes
		// every supported shader stage on graphics and compute-only queues.
		auto* command = m_buffer[m_current_buffer];
		VkMemoryBarrier barrier {};
		barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vkCmdPipelineBarrier(command->GetBufferOutsideRenderPass(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		                     VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
	}

	m_buffer[m_current_buffer]->End();
	m_buffer[m_current_buffer]->Execute();

	m_current_buffer = (m_current_buffer + 1) % VK_BUFFERS_NUM;

	EXIT_IF(m_buffer[m_current_buffer] == nullptr);

	{
		Lumen::CriticalPath::Scope reuse(Lumen::CriticalPath::Reason::GpuFenceReuseWait, static_cast<uint64_t>(m_queue + 1));
		m_buffer[m_current_buffer]->WaitForFenceAndReset();
	}
	m_buffer[m_current_buffer]->Begin();
}

void CommandProcessor::BufferFlushPending()
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM || m_buffer[m_current_buffer] == nullptr);
	// A release can already have submitted all native work and begun an empty
	// recording. Preserve that recording's identity instead of submitting it
	// again at the outer batch boundary. Explicit dependency/host-read flushes
	// still use BufferFlush and cannot lose their fence or visibility operation.
	if (m_buffer[m_current_buffer]->HasPendingSubmissionWork()) BufferFlush();
	else Lumen::CriticalPath::Event(Lumen::CriticalPath::Reason::GpuEmptyFlushSkipped,
	    m_buffer[m_current_buffer]->GetRecordingId(), 0, static_cast<uint64_t>(m_queue + 1));
}

void CommandProcessor::RetireCompletedReadOnlyStorage(const JoinedGraphicsParserAuthority& joined)
{
	if (!GpuMemoryReadOnlyStorageRetirementEnabled()) return;
	const auto receipt = [this](uint32_t bit, const char* outcome) {
		static std::atomic<uint32_t> reported {0};
		if ((reported.fetch_or(bit, std::memory_order_relaxed) & bit) == 0)
			std::fprintf(stderr, "[GPU_READONLY_STORAGE_RETIRE_SCOPE] cp=%p outcome=%s policy=joined-graphics-parsers once=1\n",
			    static_cast<void*>(this), outcome);
	};
	if (!m_run_lock_held.load(std::memory_order_acquire)) { receipt(1u, "skip-no-run-lock"); return; }
	if (!joined.IsFor(this)) { receipt(4u, "skip-foreign-joined-parsers"); return; }
	Core::LockGuard processor_lock(m_mutex);
	// Both parser jobs have returned: neither CE nor DE retains packet-local
	// pointers. Their EOP exclusive-stream flag remains unchanged and unused here.
	if (m_queue != GraphicContext::QUEUE_GFX) { receipt(2u, "skip-non-graphics"); return; }
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	if (m_headless_test_mode) { receipt(8u, "skip-headless-test"); return; }
#endif
	auto* renderer_mutex = GraphicsRenderMemoryMutex();
	auto* buffer = GetCurrentBufferForCpuCopy();
	if (renderer_mutex == nullptr || buffer == nullptr) { receipt(16u, "skip-no-renderer-or-buffer"); return; }
	Core::LockGuard renderer_lock(*renderer_mutex);
	receipt(32u, "entered-graphics-jobs-joined");
	GpuMemoryRetireCompletedReadOnlyStorage(WindowGetGraphicContext(), this, buffer);
}

void CommandProcessor::BufferWait(Lumen::CriticalPath::Reason reason)
{
	Lumen::CriticalPath::Scope trace(reason, static_cast<uint64_t>(m_queue + 1));
	BufferInit();

	Core::LockGuard lock(m_mutex);

	for (auto& buf: m_buffer)
	{
		EXIT_IF(buf == nullptr);

		buf->WaitForFenceAndReset();
	}
}

struct PendingSubmissionPrefixSlot
{
	int queue = -1;
	uint64_t epoch = 0, serial = 0;
	bool pending = false;
};

struct SubmissionPrefixWaitPlan
{
	uint32_t slots = 0;
	bool valid = false;
};

template <size_t Count>
static SubmissionPrefixWaitPlan PlanSubmissionPrefixWait(const std::array<PendingSubmissionPrefixSlot, Count>& slots,
    int queue, uint64_t epoch, uint64_t serial, int processor_queue, uint64_t processor_epoch,
    uint64_t submitted, uint64_t completed, bool timeline_valid)
{
	static_assert(Count <= 32);
	SubmissionPrefixWaitPlan plan;
	if (!timeline_valid || queue < 0 || queue >= GraphicContext::QUEUES_NUM || queue != processor_queue ||
	    epoch == 0 || epoch != processor_epoch || serial == 0 || serial > submitted || completed > submitted)
		return plan;
	bool covered = serial <= completed;
	for (size_t index = 0; index != Count; ++index)
	{
		const auto& slot = slots[index];
		if (!slot.pending) continue;
		// Every pending buffer must retain its accepted submission identity. An
		// unknown slot cannot be classified as an independent later dependency.
		if (slot.queue != queue || slot.epoch != epoch || slot.serial == 0 || slot.serial > submitted) return {};
		for (size_t earlier = 0; earlier != index; ++earlier)
			if (slots[earlier].pending && slots[earlier].serial == slot.serial) return {};
		if (slot.serial <= serial) plan.slots |= uint32_t {1} << index;
		if (slot.serial == serial) covered = true;
	}
	plan.valid = covered;
	return plan;
}

template <size_t Count, typename Read, typename Completed, typename Wait>
static bool CompleteSubmissionPrefixPlan(const SubmissionPrefixWaitPlan& plan,
    const std::array<PendingSubmissionPrefixSlot, Count>& frozen, Read read, Completed completed, Wait wait)
{
	if (!plan.valid) return false;
	for (size_t index = 0; index != Count; ++index)
	{
		if ((plan.slots & (uint32_t {1} << index)) == 0) continue;
		const auto live = read(index);
		if (!live.pending || live.queue != frozen[index].queue || live.epoch != frozen[index].epoch ||
		    live.serial != frozen[index].serial)
		{
			// A nested wait may have completed and reused this persistent slot.
			// The replacement never inherits membership of the frozen prefix.
			if (!completed(frozen[index])) return false;
			continue;
		}
		wait(index);
	}
	return true;
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
static bool graphics_run_submission_prefix_wait_test()
{
	const int queue = GraphicContext::QUEUE_GFX;
	// Ring wrap changes slot order. Select the native prefix in the original
	// pool traversal, excluding both a later submission and the next recording.
	std::array<PendingSubmissionPrefixSlot, 4> slots {{{queue, 7, 12, true}, {queue, 7, 9, true},
	    {queue, 7, 10, true}, {queue, 0, 0, false}}};
	const auto plan = [&](uint64_t cutoff, uint64_t completed = 8, bool valid = true) {
		return PlanSubmissionPrefixWait(slots, queue, 7, cutoff, queue, 7, 12, completed, valid);
	};
	if (!plan(10).valid || plan(10).slots != 6 || !plan(12).valid || plan(12).slots != 7 ||
	    plan(11).valid || plan(10, 8, false).valid || plan(13).valid || plan(0).valid || plan(10, 13).valid) return false;
	// A reused slot may no longer contain the old producer. Its original
	// completion covers it; an assigned/unsubmitted serial never does.
	slots[2] = {queue, 7, 13, true};
	if (plan(10).valid || plan(10, 10).valid) return false; // Pending serial beyond submitted poisons selection.
	slots[2] = {queue, 0, 0, false};
	if (plan(10).valid || !plan(10, 10).valid || plan(10, 10).slots != 2) return false;
	slots[2] = {queue, 7, 10, true};
	const auto original = slots;
	for (unsigned bad = 0; bad != 5; ++bad)
	{
		slots = original;
		switch (bad)
		{
			case 0: slots[0].serial = 0; break;
			case 1: slots[0].epoch = 8; break;
			case 2: slots[0].queue = GraphicContext::QUEUE_COMPUTE_START; break;
			case 3: slots[0].serial = slots[1].serial; break;
			case 4: slots[2].epoch = 0; break;
		}
		if (plan(10).valid) return false;
	}
	slots = original;
	if (PlanSubmissionPrefixWait(slots, queue, 8, 10, queue, 7, 12, 8, true).valid ||
	    PlanSubmissionPrefixWait(slots, GraphicContext::QUEUE_COMPUTE_START, 7, 10, queue, 7, 12, 8, true).valid) return false;
	// Retirement of the earlier slot can reenter and complete/reuse another
	// selected slot. Never wait its replacement above the original cutoff.
	for (const bool original_completed: {false, true})
	{
		slots = original;
		uint32_t waits = 0;
		const auto selected = plan(10);
		const bool result = CompleteSubmissionPrefixPlan(selected, original, [&](size_t index) { return slots[index]; },
		    [&](const PendingSubmissionPrefixSlot& stopped) { return original_completed && stopped.serial == 10; },
		    [&](size_t index) {
			    ++waits;
			    if (index == 1) slots[2] = {queue, 7, 13, true};
		    });
		if (result != original_completed || waits != 1 || slots[0].serial != 12 || slots[2].serial != 13) return false;
	}
	std::fprintf(stderr, "[GFX_SUBMISSION_PREFIX_WAIT_TEST] wrap=pass cutoff10/12=pass later-submission/current-recording=retained"
	    " reentrant-slot-reuse=original-completion-required unknown/foreign/duplicate/stale/invalid/unsubmitted=fallback native-fences=modeled\n");
	return true;
}
#endif

bool CommandProcessor::BufferWaitForPrefix(int queue, uint64_t epoch, uint64_t serial, Lumen::CriticalPath::Reason reason)
{
	Lumen::CriticalPath::Scope trace(reason, static_cast<uint64_t>(m_queue + 1));
	Core::LockGuard lock(m_mutex);
	std::array<PendingSubmissionPrefixSlot, VK_BUFFERS_NUM> slots;
	for (int index = 0; index != VK_BUFFERS_NUM; ++index)
	{
		auto* buffer = m_buffer[index];
		if (buffer == nullptr) return false;
		slots[index] = {buffer->GetQueue(), buffer->GetPendingSubmissionSessionEpoch(),
		    buffer->GetPendingSubmitSerial(), buffer->IsExecute()};
	}
	const auto plan = PlanSubmissionPrefixWait(slots, queue, epoch, serial, m_queue,
	    m_tracked_submission_session_epoch.load(std::memory_order_acquire),
	    m_tracked_submission_submitted_serial.load(std::memory_order_acquire),
	    m_tracked_submission_completed_serial.load(std::memory_order_acquire),
	    m_tracked_submission_timeline_valid.load(std::memory_order_acquire));
	if (!plan.valid) return false;
	// Keep the ordinary pool traversal and per-buffer retirement order. Freeze
	// selection before callbacks: a reentrant callback may submit more work, but
	// that later work never acquires membership of this dependency prefix.
	if (!CompleteSubmissionPrefixPlan(plan, slots, [&](size_t index) {
		    auto* buffer = m_buffer[index];
		    return PendingSubmissionPrefixSlot {buffer->GetQueue(), buffer->GetPendingSubmissionSessionEpoch(),
		        buffer->GetPendingSubmitSerial(), buffer->IsExecute()};
	    }, [&](const PendingSubmissionPrefixSlot& stopped) {
		    return HasCompletedTrackedCommandBuffer(stopped.queue, stopped.epoch, stopped.serial);
	    }, [&](size_t index) {
		    // A retirement callback may have begun another recording in this
		    // slot. Avoid resetting the command after callbacks; the pool's
		    // RESET_COMMAND_BUFFER flag permits Begin's ordinary implicit reset.
		    m_buffer[index]->WaitForFence();
	    })) return false;
	return HasCompletedTrackedCommandBuffer(queue, epoch, serial);
}

namespace {
class GpuHandoffClaimScope;
thread_local GpuHandoffClaimScope* g_gpu_handoff_claim = nullptr;
// A waiting parser must not enqueue a global VM writer while another parser
// owns a native draw. Admission precedes every CP/renderer lock and is released
// at ordinary parser waits, so it cannot hold the CPU stopped behind a draw.
static std::mutex g_gpu_native_admission;

// Minted before a draw has acquired resources or emitted native commands.
// A previous, fully recorded prefix can be submitted by CompleteHandoffProducer
// while the parser is paused; all processor/renderer locks must be released.
// This is deliberately distinct from "already submitted" wait authority.
class IndependentlyCompletableGpuPrefix final
{
public:
	bool IsFor(const CommandProcessor* processor) const { return m_processor == processor; }
private:
	friend class ::Lumen::Libs::Graphics::CommandProcessor;
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	friend bool ::Lumen::Libs::Graphics::GraphicsRunOwnedPreparationPauseTest(void (*)(void*), void*);
#endif
	explicit IndependentlyCompletableGpuPrefix(const CommandProcessor* processor): m_processor(processor) {}
	const CommandProcessor* m_processor;
};

// A parser worker (or the coordinator after both workers return) owns this
// epoch. Ring RunLock is only a scheduling guard; no execution reader needs it
// to complete a CPU memory dependency.
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
static thread_local void (*g_native_recording_before_renderer_test)(void*) = nullptr;
static thread_local void* g_native_recording_lock_context_test = nullptr;
#endif

// CPU publication also takes renderer -> producer CP. Both production draws
// and the concurrent dirty-publication gate use this exact acquisition scope.
class NativeGpuRecordingLocks final
{
public:
	explicit NativeGpuRecordingLocks(CommandProcessor* cp): m_cp(cp), m_renderer(GraphicsRenderMemoryMutex())
	{
		EXIT_IF(m_cp == nullptr || m_renderer == nullptr);
		LockRenderer();
		GraphicsRunCommandProcessorLock(m_cp);
	}
	~NativeGpuRecordingLocks()
	{
		GraphicsRunCommandProcessorUnlock(m_cp);
		m_renderer->Unlock();
	}
	LUMEN_CLASS_NO_COPY(NativeGpuRecordingLocks);
private:
	void LockRenderer()
	{
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		if (g_native_recording_before_renderer_test != nullptr)
			g_native_recording_before_renderer_test(g_native_recording_lock_context_test);
#endif
		m_renderer->Lock();
	}
	CommandProcessor* m_cp;
	Core::Mutex* m_renderer;
};

class GpuHandoffClaimScope
{
public:
	explicit GpuHandoffClaimScope(CommandProcessor* cp, bool active = true): m_cp(cp)
	{
		if (!active || !GpuMemoryCpuHandoffEnabled() || g_gpu_handoff_claim != nullptr) return;
		m_owner = true;
		g_gpu_handoff_claim = this;
		Acquire();
	}
	~GpuHandoffClaimScope()
	{
		if (!m_owner) return;
		EXIT_IF(m_native_token != 0);
		Release();
		g_gpu_handoff_claim = nullptr;
	}
	void Release([[maybe_unused]] bool producers_published = false)
	{
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && defined(LUMEN_ANDROID_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME))
		if (m_native_token != 0) return; // Native fence waits retain the range claim.
		if (m_token == 0) return;
		Lumen::CriticalPath::Scope release(Lumen::CriticalPath::Reason::GpuHandoffRelease,
		    reinterpret_cast<uintptr_t>(m_cp), m_token);
		// CPU readmission must observe submitted native producers. It can then
		// complete just that prefix without asking a paused parser to progress.
		if (m_cp != nullptr && !producers_published) m_cp->BufferFlushPending();
		EndEpoch();
#endif
	}
	bool ReleaseOwnedPreparation(const IndependentlyCompletableGpuPrefix& prefix)
	{
		if (!OwnsEpoch() || !prefix.IsFor(m_cp) || Core::VirtualMemory::CurrentThreadHasMemoryLease() ||
		    g_retained_processor_recording != nullptr) return false;
		// No unconditional flush: an accessing CPU/GPU worker can acquire all
		// native locks and complete the previous prefix without this parser.
		EndEpoch();
		return !OwnsEpoch();
	}
	void Acquire()
	{
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && defined(LUMEN_ANDROID_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME))
		if (m_token != 0 || m_native_token != 0) return;
		Lumen::CriticalPath::Scope admission(Lumen::CriticalPath::Reason::GpuHandoffAdmission,
		    reinterpret_cast<uintptr_t>(m_cp));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		do
		{
			if (gpu_range_execution_enabled() && !m_admission.owns_lock() && !m_admission.try_lock())
			{
				// End the quiescent wait BEFORE acquiring native admission. An HLE
				// must never readmit its CPU reader while holding this mutex.
				WaitForAdmission();
				continue;
			}
			uint32_t retryable = 0;
			m_token = m_cp == nullptr
			    ? LumenAndroidFexCoreBeginGuestMemoryMutationV3(LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2, &retryable)
			    : LumenAndroidFexCoreBeginGuestMemoryMutationV2(LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_EXCLUSIVE_V2);
			if (m_token != 0)
			{
				m_core_owner.emplace(m_token);
				EXIT_IF(!m_core_owner->IsActive());
				m_bypass.emplace();
				admission.End();
				m_held.emplace(Lumen::CriticalPath::Reason::GpuHandoffHeld,
				    reinterpret_cast<uintptr_t>(m_cp), m_token);
				return;
			}
			// A competing CE/DE or compute parser releases its epoch at a wait.
			// Own no CP/renderer/tracker lock while waiting for its claim.
			if (m_admission.owns_lock()) m_admission.unlock();
			if (retryable == 1)
			{
				// The CPU refresh entry owns no borrowed guest data or GPU lock.
				// Drop its HLE reader while the competing writer finishes.
				WaitForAdmission();
			} else Core::Thread::SleepMicro(100);
		} while (std::chrono::steady_clock::now() < deadline);
		EXIT("[GPU_MEMORY_HANDOFF] exclusive claim unavailable before processor locks\n");
#endif
	}
	bool OwnsEpochFor(const CommandProcessor* cp) const { return m_token != 0 && m_cp == cp; }
	bool OwnsEpoch() const { return m_token != 0; }
	bool OwnsRecordingFor(const CommandProcessor* cp) const
	{
		return m_cp == cp && (m_token != 0 || (m_native_token != 0 && m_native_core && m_native_core->IsActive()));
	}
	bool OwnsNativePhase() const { return m_native_token != 0; }
	bool BeginRangePhase(uint64_t owner)
	{
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && defined(LUMEN_ANDROID_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME))
		if (!gpu_range_execution_enabled() || !OwnsEpoch() || !m_admission.owns_lock() ||
		    Core::VirtualMemory::CurrentThreadHasMemoryLease() || g_retained_processor_recording != nullptr ||
		    !GuestCpu::PublishMemoryHandoffNativeClaim(owner)) return false;
		m_core_owner.reset();
		m_native_token = LumenAndroidFexCoreDowngradeExclusiveToNativeGuestDataLeaseV1(m_token);
		if (m_native_token == 0)
		{
			EXIT_IF(!GuestCpu::RetireMemoryHandoffNativeClaim(owner));
			m_core_owner.emplace(m_token);
			EXIT_IF(!m_core_owner->IsActive());
			return false;
		}
		m_token = 0;
		m_held.reset();
		m_native_owner = owner;
		m_native_claim.emplace(owner);
		EXIT_IF(!m_native_claim->IsActive());
		m_native_core.emplace(m_native_token);
		// A local VM gate can conservatively deny adoption. The caller still
		// owns a mapping pin and must retire its plan before readmission.
		return m_native_core->IsActive();
#else
		(void)owner;
		return false;
#endif
	}
	void EndRangePhase(GraphicsRenderRangePreparation* preparation)
	{
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && defined(LUMEN_ANDROID_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME))
		EXIT_IF(m_native_token == 0 || Core::VirtualMemory::CurrentThreadHasMemoryLease() ||
		    g_retained_processor_recording != nullptr);
		m_native_core.reset();
		m_native_claim.reset();
		EXIT_IF(!GuestCpu::RetireMemoryHandoffNativeClaim(m_native_owner));
		// Remove temporary guards while the mapping pin still protects aliases.
		if (preparation != nullptr) preparation->Reset();
		m_native_owner = 0;
		const auto token = m_native_token;
		m_native_token = 0;
		m_bypass.reset();
		EXIT_IF(LumenAndroidFexCoreEndNativeGuestDataLeaseV1(token) == 0);
		if (m_admission.owns_lock()) m_admission.unlock();
		Acquire(); // All CP/renderer locks have already been released.
#else
		(void)preparation;
#endif
	}

private:
	void WaitForAdmission()
	{
		EXIT_IF(m_token != 0 || m_native_token != 0 || m_admission.owns_lock() ||
		    m_core_owner.has_value() || m_bypass.has_value() || g_gpu_handoff_claim != this);
		// Readmission may deliver a guest exception whose HLE reenters graphics.
		// An unowned placeholder must not suppress that inner claim. Keep TLS
		// empty through the wait destructor and restore it only after delivery.
		struct Restore
		{
			GpuHandoffClaimScope* claim;
			~Restore() { EXIT_IF(g_gpu_handoff_claim != nullptr); g_gpu_handoff_claim = claim; }
		} restore {this};
		g_gpu_handoff_claim = nullptr;
		Loader::ScopedGuestHostWait wait;
		Core::Thread::SleepMicro(100);
	}
	void EndEpoch()
	{
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && defined(LUMEN_ANDROID_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME))
		const auto token = m_token;
		EXIT_IF(token == 0);
		m_token = 0;
		m_core_owner.reset();
		m_bypass.reset();
		EXIT_IF(LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 0);
		m_held.reset();
		if (m_admission.owns_lock()) m_admission.unlock();
#endif
	}
	[[maybe_unused]] CommandProcessor* m_cp;
	uint64_t m_token = 0;
	uint64_t m_native_token = 0, m_native_owner = 0;
	bool m_owner = false;
	std::unique_lock<std::mutex> m_admission {g_gpu_native_admission, std::defer_lock};
	std::optional<Core::VirtualMemory::GuestMemoryMutationOwnerScope> m_core_owner;
	std::optional<Core::VirtualMemory::GuestNativeMemoryOwnerScope> m_native_core;
	std::optional<GuestCpu::MemoryHandoffNativeClaimScope> m_native_claim;
	std::optional<GuestCpu::MemoryHandoffBypass> m_bypass;
	std::optional<Lumen::CriticalPath::Scope> m_held;
};

template <typename Analyze>
bool AnalyzeOutsideGpuEpoch(const IndependentlyCompletableGpuPrefix& prefix, Analyze&& analyze)
{
	auto* claim = g_gpu_handoff_claim;
	if (claim == nullptr || !claim->ReleaseOwnedPreparation(prefix)) return false;
	struct Readmission
	{
		GpuHandoffClaimScope* claim;
		~Readmission() { claim->Acquire(); }
	} readmission {claim};
	analyze();
	return true;
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
struct GpuCounterWaitTestProbe
{
	void* context = nullptr;
	void (*handoff)(void*, bool) = nullptr;
	void (*waiting)(void*) = nullptr;
};
static thread_local GpuCounterWaitTestProbe* g_gpu_counter_wait_test_probe = nullptr;
#endif

class GpuHandoffWaitScope
{
public:
	explicit GpuHandoffWaitScope(bool producers_published = false): m_claim(g_gpu_handoff_claim)
	{
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		if (auto* probe = g_gpu_counter_wait_test_probe; probe != nullptr && probe->handoff != nullptr)
			probe->handoff(probe->context, true);
#endif
		if (m_claim != nullptr) m_claim->Release(producers_published);
	}
	~GpuHandoffWaitScope()
	{
		if (m_claim != nullptr) m_claim->Acquire();
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		if (auto* probe = g_gpu_counter_wait_test_probe; probe != nullptr && probe->handoff != nullptr)
			probe->handoff(probe->context, false);
#endif
	}
private:
	GpuHandoffClaimScope* m_claim;
};

// A seal preserves mapping identity, not byte content or permission. Reacquire
// both and compare the stopped packets before any parser effect after a native
// wait. Future packets remain live and are read only after readmission. Owned
// root buffers stay alive in the synchronous Run/submit stack across this pause.
class GpuParserPauseProof final
{
	struct Stream
	{
		CommandProcessorStreamThreadState* state = nullptr;
		uint32_t* data = nullptr;
		uint32_t words = 0, offset = 0, header_words = 0;
		bool owned = false;
		Core::VirtualMemory::GuestDataRangeSeal seal;
		std::vector<uint32_t> packet;
	};
public:
	bool Capture(CommandProcessor* cp, uint32_t consumed_packet_dw = 0)
	{
		for (auto* state = g_command_processor_stream_state; state != nullptr; state = state->previous)
		{
			const uint32_t captured_words = state == g_command_processor_stream_state && consumed_packet_dw != 0
			    ? consumed_packet_dw : state->packet_num_dw;
			if (state->owner != cp || state->live_data == nullptr || state->num_dw == 0 ||
			    state->packet_num_dw == 0 || state->packet_offset >= state->num_dw ||
			    captured_words < state->packet_num_dw || captured_words > state->num_dw - state->packet_offset) return false;
			auto stream = std::make_unique<Stream>();
			stream->state = state; stream->data = state->live_data; stream->words = state->num_dw;
			stream->offset = state->packet_offset; stream->owned = state->root_owned; stream->header_words = state->packet_num_dw;
			Core::VirtualMemory::GuestDataLease lease;
			if (!stream->owned &&
			    (!Core::VirtualMemory::TryAcquireGuestDataLease(reinterpret_cast<uint64_t>(stream->data),
			        uint64_t(stream->words) * 4, Core::VirtualMemory::Mode::Read, &lease, false) ||
			     !Core::VirtualMemory::CaptureGuestDataRangeSeal(lease, &stream->seal))) return false;
			// Some draw opcodes consume a companion NOP beyond their own header.
			// Preserve every consumed word, including ignored companion payload.
			stream->packet.assign(stream->data + stream->offset, stream->data + stream->offset + captured_words);
			streams.push_back(std::move(stream));
		}
		return !streams.empty();
	}
	bool Validate() const
	{
		auto* active = g_command_processor_stream_state;
		for (const auto& stream: streams)
		{
			if (active != stream->state || active->live_data != stream->data || active->num_dw != stream->words ||
			    active->packet_offset != stream->offset || active->packet_num_dw != stream->header_words ||
			    active->root_owned != stream->owned) return false;
			Core::VirtualMemory::GuestDataLease lease;
			if (!stream->owned &&
			    (!Core::VirtualMemory::TryAcquireGuestDataLease(reinterpret_cast<uint64_t>(stream->data),
			        uint64_t(stream->words) * 4, Core::VirtualMemory::Mode::Read, &lease, false) ||
			     !Core::VirtualMemory::ValidateGuestDataRangeSeal(lease, stream->seal))) return false;
			if (std::memcmp(stream->data + stream->offset, stream->packet.data(), stream->packet.size() * 4) != 0) return false;
			active = active->previous;
		}
		return active == nullptr;
	}
	void AppendClaimRanges(std::vector<GpuMemoryClaimRange>* ranges) const
	{
		for (const auto& stream: streams)
			if (!stream->owned)
				ranges->push_back({reinterpret_cast<uint64_t>(stream->data + stream->offset),
				    uint64_t(stream->packet.size()) * 4});
	}
	bool RefreshGpuProducers() const
	{
		// Other GPU queues may have become the newest producer while the CPU
		// epoch was released. Mapping continuity alone cannot publish their bytes.
		for (const auto& stream: streams)
			if (!stream->owned && !GraphicsRenderPrepareCpuMemoryAccess(reinterpret_cast<uint64_t>(stream->data),
			        uint64_t(stream->words) * 4, GuestCpu::MemoryAccessIntent::Read)) return false;
		return true;
	}
private:
	std::vector<std::unique_ptr<Stream>> streams;
};

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
static bool graphics_run_parser_pause_proof_test(CommandProcessor* cp)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	Init();
	const auto address = Alloc(0, 65536, Mode::ReadWrite);
	if (address == 0) return false;
	struct Owner { uint64_t address; ~Owner() { Free(address); } } owner {address};
	auto* packet = reinterpret_cast<uint32_t*>(address);
	std::fill_n(packet, 32, 0x12345678u);
	uint32_t root[16] {};
	CommandProcessorStreamThreadState outer;
	outer.owner = cp; outer.live_data = root; outer.num_dw = 16;
	outer.packet_offset = 2; outer.packet_num_dw = 4; outer.root_owned = true;
	CommandProcessorStreamThreadState inner;
	inner.owner = cp; inner.live_data = packet; inner.num_dw = 32;
	inner.packet_offset = 4; inner.packet_num_dw = 8; inner.previous = &outer;
	struct Restore { CommandProcessorStreamThreadState* old; ~Restore() { g_command_processor_stream_state = old; } }
	    restore {g_command_processor_stream_state};
	g_command_processor_stream_state = &inner;
	GpuParserPauseProof proof;
	if (!proof.Capture(cp) || !proof.Validate()) return false;
	GpuParserPauseProof with_companion;
	if (!with_companion.Capture(cp, 10) || !with_companion.Validate()) return false;
	packet[13] ^= 1; // Last consumed NOP word lies beyond the draw header.
	if (with_companion.Validate() || !proof.Validate()) return false;
	packet[13] ^= 1;
	GpuParserPauseProof short_companion, long_companion;
	if (short_companion.Capture(cp, 7) || long_companion.Capture(cp, 29)) return false;
	packet[20] ^= 1; // Future packets remain live; no stale snapshot execution.
	if (!proof.Validate() || !with_companion.Validate()) return false;
	packet[5] ^= 1;
	if (proof.Validate()) return false;
	packet[5] ^= 1;
	root[3] ^= 1;
	if (proof.Validate()) return false;
	root[3] ^= 1;
	inner.packet_offset++;
	if (proof.Validate()) return false;
	inner.packet_offset--;
	inner.previous = nullptr;
	if (proof.Validate()) return false;
	inner.previous = &outer;
	const auto other = Alloc(0, 65536, Mode::ReadWrite);
	if (other == 0 || !Free(other) || !proof.Validate()) return false;
	if (!Protect(address, 65536, Mode::NoAccess) || proof.Validate()) return false;
	if (!Protect(address, 65536, Mode::ReadWrite)) return false;
	GpuParserPauseProof remap;
	if (!remap.Capture(cp) || !Free(address) || !AllocFixed(address, 65536, Mode::ReadWrite)) return false;
	std::fill_n(packet, 32, 0x12345678u);
	if (remap.Validate()) return false; // Identical bytes cannot revive an old mapping.
	inner.packet_num_dw = 33;
	GpuParserPauseProof malformed;
	if (malformed.Capture(cp)) return false;
	std::fprintf(stderr, "[GFX_PARSER_PAUSE_PROOF_TEST] managed-VM=real nested/owned=pass current-packet-change=reject"
	    " consumed-NOP-change=reject future-packet=live unrelated-map=accepted permission-loss/remap/shape=reject\n");
#else
	(void)cp;
#endif
	return true;
}
#endif

template <typename T> bool GpuHandoffPollMatches(const T* address, T reference, T mask)
{
	Core::VirtualMemory::GuestDataLease lease;
	if (GpuMemoryCpuHandoffEnabled())
	{
		Core::VirtualMemory::GuestDataLeaseDenialMetadata denial;
		if (!Core::VirtualMemory::TryAcquireGuestDataLease(reinterpret_cast<uint64_t>(address), sizeof(T),
		        Core::VirtualMemory::Mode::Read, &lease, false, &denial))
		{
			if (!denial.retryable)
				EXIT("[GPU_HANDOFF_POLL_DENIED] reason=%s phase=%s address=0x%016" PRIx64
				    " bytes=%zu required=%u reference=0x%016" PRIx64 " mask=0x%016" PRIx64
				    " generation=%" PRIu64 " gate_observed=%u mutation_active=%u waiting_mutations=%" PRIu64
				    " code_readers=%" PRIu64 " physical_observed=%u physical_result=%d physical_protection=%u\n",
				    Core::VirtualMemory::GuestDataLeaseDenialReasonName(denial.reason),
				    Core::VirtualMemory::GuestDataLeaseDenialPhaseName(denial.reason),
				    reinterpret_cast<uint64_t>(address), sizeof(T), static_cast<unsigned>(denial.required),
				    static_cast<uint64_t>(reference), static_cast<uint64_t>(mask), denial.generation,
				    denial.gate_observed ? 1u : 0u, denial.mutation_active ? 1u : 0u, denial.waiting_mutations,
				    denial.code_readers, denial.physical_observed ? 1u : 0u, denial.physical_result, denial.physical_protection);
			return false;
		}
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	return (__atomic_load_n(address, __ATOMIC_ACQUIRE) & mask) == reference;
#else
	return (*address & mask) == reference;
#endif
}
} // namespace

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool GraphicsRunParserPauseProofContractTest()
{
	// CommandProcessor instances intentionally have process lifetime, as in
	// the full state-synchronization fixture. This fixture needs no GPU device.
	auto* cp = new CommandProcessor(64);
	cp->SetHeadlessTestMode();
	return graphics_run_parser_pause_proof_test(cp);
}

CommandProcessor* GraphicsRunCreateHeadlessTestProcessor()
{
	auto* cp = new CommandProcessor(64);
	cp->SetHeadlessTestMode();
	return cp;
}

bool GraphicsRunTryProcessorLockForTest(CommandProcessor* cp)
{
	return cp != nullptr && cp->TryLockForTest();
}

bool GraphicsRunNativeRecordingLocksTest(CommandProcessor* cp, void (*before_renderer)(void*), void* context,
    bool (*locked)(void*))
{
	if (cp == nullptr || before_renderer == nullptr || locked == nullptr ||
	    g_native_recording_before_renderer_test != nullptr) return false;
	struct Restore
	{
		~Restore() { g_native_recording_before_renderer_test = nullptr; g_native_recording_lock_context_test = nullptr; }
	} restore;
	g_native_recording_before_renderer_test = before_renderer;
	g_native_recording_lock_context_test = context;
	NativeGpuRecordingLocks locks(cp);
	return locked(context);
}

bool GraphicsRunOwnedPreparationPauseTest(void (*paused)(void*), void* context)
{
	if (paused == nullptr || g_gpu_handoff_claim != nullptr || g_retained_processor_recording != nullptr ||
	    Core::VirtualMemory::CurrentThreadHasMemoryLease()) return false;
	// No command processor or recording exists in this gate, so its native
	// prefix is empty. Admission/release/readmission are the production path.
	GpuHandoffClaimScope claim(nullptr);
	if (!claim.OwnsEpoch()) return false;
	bool callback_valid = false;
	const bool analyzed = AnalyzeOutsideGpuEpoch(IndependentlyCompletableGpuPrefix(nullptr), [&] {
		callback_valid = !claim.OwnsEpoch() && !Core::VirtualMemory::CurrentThreadHasMemoryLease();
		if (callback_valid) paused(context);
		callback_valid = callback_valid && !claim.OwnsEpoch() && !Core::VirtualMemory::CurrentThreadHasMemoryLease();
	});
	return analyzed && callback_valid && claim.OwnsEpoch();
}

bool GraphicsRunNativeRangePhaseTest(uint64_t address, uint64_t bytes, void (*native)(void*), void* context)
{
	if (native == nullptr || !gpu_range_execution_enabled() || g_gpu_handoff_claim != nullptr ||
	    Core::VirtualMemory::CurrentThreadHasMemoryLease()) return false;
	GpuHandoffClaimScope claim(nullptr);
	if (!claim.OwnsEpoch()) return false;
	const auto owner = Core::VirtualMemory::CreateGuestBackingHandoffOwner();
	if (owner == 0) return false;
	bool okay = Core::VirtualMemory::MarkGuestBackingHandoffRangeOwned(owner, address, bytes, 3u);
	if (okay) okay = claim.BeginRangePhase(owner);
	if (okay)
	{
		// This hook has an empty native prefix. The production wait scope must
		// retain the active ranges and pin instead of reacquiring the VM writer.
		GpuHandoffWaitScope wait(true);
		okay = !claim.OwnsEpoch() && claim.OwnsNativePhase() && claim.OwnsRecordingFor(nullptr);
		if (okay) native(context);
	}
	if (claim.OwnsNativePhase()) claim.EndRangePhase(nullptr);
	return Core::VirtualMemory::ReleaseGuestBackingHandoffOwner(owner) && okay && claim.OwnsEpoch();
}
#endif

void CommandProcessor::BufferFlushAndWaitForPrefix(Lumen::CriticalPath::Reason reason)
{
	int queue;
	uint64_t epoch, serial;
	std::array<CommandBuffer::NativeFenceWait, VK_BUFFERS_NUM> waits;
	GpuParserPauseProof parser;
	bool native_pause = g_gpu_handoff_claim != nullptr && g_gpu_handoff_claim->OwnsEpochFor(this) && parser.Capture(this);
	{
		Core::LockGuard lock(m_mutex);
		EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM || m_buffer[m_current_buffer] == nullptr);
		const auto* buffer = m_buffer[m_current_buffer];
		queue = buffer->GetQueue(); epoch = buffer->GetSubmissionSessionEpoch(); serial = buffer->GetRecordingSubmitSerial();
		BufferFlush();
		// A published empty successor permits release without another flush.
		// Flushing after pin capture could otherwise rotate into our own pin.
		native_pause = native_pause && !m_buffer[m_current_buffer]->HasPendingSubmissionWork();
		if (native_pause)
		{
			std::array<PendingSubmissionPrefixSlot, VK_BUFFERS_NUM> slots;
			for (int index = 0; index != VK_BUFFERS_NUM; ++index)
			{
				const auto* live = m_buffer[index];
				slots[index] = {live->GetQueue(), live->GetPendingSubmissionSessionEpoch(), live->GetPendingSubmitSerial(), live->IsExecute()};
			}
			const auto plan = PlanSubmissionPrefixWait(slots, queue, epoch, serial, m_queue,
			    m_tracked_submission_session_epoch.load(std::memory_order_acquire),
			    m_tracked_submission_submitted_serial.load(std::memory_order_acquire),
			    m_tracked_submission_completed_serial.load(std::memory_order_acquire),
			    m_tracked_submission_timeline_valid.load(std::memory_order_acquire));
			native_pause = plan.valid && plan.slots != 0;
			for (int index = 0; native_pause && index != VK_BUFFERS_NUM; ++index)
				if ((plan.slots & (uint32_t {1} << index)) != 0)
					native_pause = m_buffer[index]->CaptureNativeFenceWait(&waits[index]);
		}
	}
	if (native_pause)
	{
		VkResult result = VK_SUCCESS;
		{
			// No processor/renderer lock or guest-data lease crosses this wait.
			GpuHandoffWaitScope released(true);
			for (auto& wait: waits)
				if (wait.IsActive()) { const auto observed = wait.Wait(); if (observed != VK_SUCCESS) result = observed; }
		}
		EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
		EXIT_IF(!parser.RefreshGpuProducers() || !parser.Validate());
	} else
	{
		// Partial capture grants no pause. Drop its native pins before the
		// conservative completion path can reset any of those same fences.
		for (auto& wait: waits) wait = CommandBuffer::NativeFenceWait {};
	}
	// Another reader may have completed/reused slots during the native wait.
	// The prefix helper checks the original queue/session/serial and never
	// consumes a replacement beyond that cutoff. Callbacks still own the epoch.
	if (!BufferWaitForPrefix(queue, epoch, serial, reason)) BufferWait(reason);
}

void CommandProcessor::ResetDeCe()
{
	m_de_counter.mutex.Lock();
	m_de_counter.value = 0;
	m_de_counter.cond_var.Signal();
	m_de_counter.mutex.Unlock();
	m_ce_counter.mutex.Lock();
	m_ce_counter.value = 0;
	m_ce_counter.cond_var.Signal();
	m_ce_counter.mutex.Unlock();
}

static void record_ready_counter_wait(CommandProcessor* cp, bool ce, bool ready)
{
	struct Counts { uint64_t checks = 0, ready = 0, slow = 0; };
	static thread_local Counts ce_counts, de_counts;
	auto& counts = ce ? ce_counts : de_counts;
	++counts.checks;
	if (ready) ++counts.ready; else ++counts.slow;
	if (counts.checks != 0 && (counts.checks & (counts.checks - 1)) == 0)
		std::fprintf(stderr, "[GPU_READY_COUNTER_WAIT] cp=%p thread=%d engine=%s checks=%" PRIu64
		    " ready=%" PRIu64 " slow=%" PRIu64 "\n", static_cast<void*>(cp), Core::Thread::GetThreadIdUnique(),
		    ce ? "CE" : "DE", counts.checks, counts.ready, counts.slow);
}

bool CommandProcessor::CeWaitReady()
{
	uint32_t de_value;
	{
		Core::LockGuard lock(m_de_counter.mutex);
		de_value = m_de_counter.value;
	}
	Core::LockGuard lock(m_ce_counter.mutex);
	return m_ce_counter.value > de_value;
}

bool CommandProcessor::DeDiffWaitReady(uint32_t diff)
{
	uint32_t ce_value;
	{
		Core::LockGuard lock(m_ce_counter.mutex);
		ce_value = m_ce_counter.value;
	}
	Core::LockGuard lock(m_de_counter.mutex);
	return ce_value - m_de_counter.value < diff;
}

void CommandProcessor::WaitCe()
{
	// Counter synchronization alone can satisfy this packet. Retaining the
	// epoch exposes no producer to CPU readers and needs no release flush.
	// An unsatisfied observation grants no pause: keep the original slow path,
	// including fresh snapshots after release, with no counter lock crossing it.
	if (gpu_ready_counter_wait_enabled())
	{
		const bool ready = CeWaitReady();
		record_ready_counter_wait(this, true, ready);
		if (ready) return;
	}
	GpuHandoffWaitScope handoff_wait;
	Lumen::CriticalPath::Scope trace(Lumen::CriticalPath::Reason::GpuCeWait, static_cast<uint64_t>(m_queue + 1));
	m_de_counter.mutex.Lock();
	auto de_value = m_de_counter.value;
	m_de_counter.mutex.Unlock();

	m_ce_counter.mutex.Lock();
	while (!(m_ce_counter.value > de_value)
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	       && !Loader::GuestExecutionStopRequested()
#endif
	)
	{
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		if (auto* probe = g_gpu_counter_wait_test_probe; probe != nullptr && probe->waiting != nullptr)
			probe->waiting(probe->context);
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		m_ce_counter.cond_var.WaitFor(&m_ce_counter.mutex, 10000);
#else
		m_ce_counter.cond_var.Wait(&m_ce_counter.mutex);
#endif
	}
	m_ce_counter.mutex.Unlock();
}

void CommandProcessor::WaitDeDiff(uint32_t diff)
{
	if (gpu_ready_counter_wait_enabled())
	{
		const bool ready = DeDiffWaitReady(diff);
		record_ready_counter_wait(this, false, ready);
		if (ready) return;
	}
	GpuHandoffWaitScope handoff_wait;
	Lumen::CriticalPath::Scope trace(Lumen::CriticalPath::Reason::GpuDeWait, static_cast<uint64_t>(m_queue + 1));
	m_ce_counter.mutex.Lock();
	auto ce_value = m_ce_counter.value;
	m_ce_counter.mutex.Unlock();

	m_de_counter.mutex.Lock();
	while (!(ce_value - m_de_counter.value < diff)
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	       && !Loader::GuestExecutionStopRequested()
#endif
	)
	{
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		if (auto* probe = g_gpu_counter_wait_test_probe; probe != nullptr && probe->waiting != nullptr)
			probe->waiting(probe->context);
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		m_de_counter.cond_var.WaitFor(&m_de_counter.mutex, 10000);
#else
		m_de_counter.cond_var.Wait(&m_de_counter.mutex);
#endif
	}
	m_de_counter.mutex.Unlock();
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool CommandProcessor::ReadyCounterWaitContractTest()
{
	const bool enabled = gpu_ready_counter_wait_enabled();
	if (enabled != exact_one_environment_value(std::getenv("LUMEN_GPU_READY_COUNTER_WAIT"))) return false;
	struct Case
	{
		bool ce;
		uint32_t ce_value, de_value, diff;
		bool ready;
		uint32_t next_ce, next_de;
	};
	constexpr Case scenarios[] = {
	    {true, 1, 0, 0, true, 1, 0},
	    {true, UINT32_MAX, UINT32_MAX - 1, 0, true, UINT32_MAX, UINT32_MAX - 1},
	    {true, 0, 0, 0, false, 1, 0},
	    {true, UINT32_MAX - 1, UINT32_MAX - 1, 0, false, UINT32_MAX, UINT32_MAX - 1},
	    {false, 0, 0, 1, true, 0, 0},
	    {false, 0, UINT32_MAX, 2, true, 0, UINT32_MAX},
	    {false, UINT32_MAX, UINT32_MAX, 1, true, UINT32_MAX, UINT32_MAX},
	    {false, 1, 0, 1, false, 1, 1},
	    {false, 0, UINT32_MAX, 1, false, 0, 0},
	    {false, UINT32_MAX, 0, UINT32_MAX, false, UINT32_MAX, 1},
	    {false, 0, 1, UINT32_MAX, false, 0, 2},
	};
	uint32_t cases = 0;
	for (const auto& scenario: scenarios)
	{
		for (const bool during_wait: {false, true})
		{
			if (scenario.ready && during_wait) continue;
			{
				Core::LockGuard lock(m_ce_counter.mutex);
				m_ce_counter.value = scenario.ce_value;
			}
			{
				Core::LockGuard lock(m_de_counter.mutex);
				m_de_counter.value = scenario.de_value;
			}
			if ((scenario.ce ? CeWaitReady() : DeDiffWaitReady(scenario.diff)) != scenario.ready ||
			    DeDiffWaitReady(0)) return false;
			struct Observation
			{
				CommandProcessor* cp;
				const Case* scenario;
				bool during_wait;
				bool locks_available = true;
				uint32_t releases = 0, readmissions = 0;
				std::atomic_bool waiting = false, finished = false, produced = false, producer_timeout = false;
			} observed {this, &scenario, during_wait};
			GpuCounterWaitTestProbe probe;
			probe.context = &observed;
			probe.handoff = [](void* context, bool release) {
				auto& state = *static_cast<Observation*>(context);
				if (release) ++state.releases; else ++state.readmissions;
				// A different host thread must be able to take both counter locks
				// at the actual handoff scope boundaries. This also publishes the
				// early producer after a false precheck, before native wait setup.
				std::thread producer([&] {
					const bool ce = state.cp->m_ce_counter.mutex.TryLock();
					const bool de = state.cp->m_de_counter.mutex.TryLock();
					state.locks_available = state.locks_available && ce && de;
					if (ce && de && release && !state.scenario->ready && !state.during_wait)
					{
						state.cp->m_ce_counter.value = state.scenario->next_ce;
						state.cp->m_de_counter.value = state.scenario->next_de;
						state.cp->m_ce_counter.cond_var.Signal();
						state.cp->m_de_counter.cond_var.Signal();
						state.produced = true;
					}
					if (de) state.cp->m_de_counter.mutex.Unlock();
					if (ce) state.cp->m_ce_counter.mutex.Unlock();
				});
				producer.join();
			};
			probe.waiting = [](void* context) {
				static_cast<Observation*>(context)->waiting.store(true, std::memory_order_release);
			};
			std::thread producer;
			if (during_wait)
			{
				producer = std::thread([&] {
					const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
					while (!observed.waiting.load(std::memory_order_acquire) && !observed.finished.load() &&
					       std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
					if (!observed.waiting.load(std::memory_order_acquire)) observed.producer_timeout = true;
					// The waiting callback runs while the watched mutex is still
					// locked. Publishing under it can proceed only when the real CV
					// wait atomically releases that mutex, closing the lost-wake gap.
					auto& counter = scenario.ce ? m_ce_counter : m_de_counter;
					Core::LockGuard lock(counter.mutex);
					counter.value = scenario.ce ? scenario.next_ce : scenario.next_de;
					counter.cond_var.Signal();
					observed.produced = true;
				});
			}
			auto* previous = g_gpu_counter_wait_test_probe;
			g_gpu_counter_wait_test_probe = &probe;
			if (scenario.ce) WaitCe(); else WaitDeDiff(scenario.diff);
			g_gpu_counter_wait_test_probe = previous;
			observed.finished = true;
			if (producer.joinable()) producer.join();
			const uint32_t expected_handoffs = enabled && scenario.ready ? 0u : 1u;
			if (!observed.locks_available || observed.releases != expected_handoffs ||
			    observed.readmissions != expected_handoffs || observed.producer_timeout ||
			    observed.waiting != during_wait || observed.produced != !scenario.ready) return false;
			++cases;
		}
	}
	// The CE comparison is numeric, not a modulo-distance comparison. A CE
	// wrap to zero must not make it greater than the maximum DE snapshot.
	m_ce_counter.mutex.Lock(); m_ce_counter.value = 0; m_ce_counter.mutex.Unlock();
	m_de_counter.mutex.Lock(); m_de_counter.value = UINT32_MAX; m_de_counter.mutex.Unlock();
	if (CeWaitReady()) return false;
	ResetDeCe();
	std::fprintf(stderr, "[GFX_READY_COUNTER_WAIT_TEST] selector=%u cases=%u counters/mutex/CV=real ready-handoff=%u"
	    " false-precheck/producer-before-wait/producer-during-wait=pass uint32-wrap/equality/diff-zero=pass"
	    " counter-locks-at-handoff=free native-GPU-handoff=modeled\n", enabled ? 1u : 0u, cases, enabled ? 0u : 1u);
	return true;
}
#endif

void CommandProcessor::IncremenetDe()
{
	if (increment_de_gpu_wait_enabled())
	{
		BufferFlushAndWaitForPrefix(Lumen::CriticalPath::Reason::GpuIncrementDe);
	}
	else
	{
		if (g_gpu_handoff_claim == nullptr)
		{
			BufferFlushPending();
		}
		else
		{
			// Even an immediate release/readmission permits guest VM changes.
			// Preserve every active PM4 stream before yielding its ownership.
			GpuParserPauseProof parser;
			if (!g_gpu_handoff_claim->OwnsEpochFor(this) || !parser.Capture(this))
			{
				// No proof grants no pause: retain the existing conservative path.
				BufferFlushAndWaitForPrefix(Lumen::CriticalPath::Reason::GpuIncrementDe);
			}
			else
			{
				{
					// Release submits the pending prefix without draining the GPU
					// or submitting an empty recording. No lock or lease crosses it.
					GpuHandoffWaitScope released;
				}
				// The epoch is owned again. Re-publish any intervening GPU
				// producers and validate the live streams before advancing DE.
				EXIT_IF(!parser.RefreshGpuProducers() || !parser.Validate());
			}
		}
	}

	m_de_counter.mutex.Lock();
	m_de_counter.value++;
	const auto counter = m_de_counter.value;
	m_de_counter.cond_var.Signal();
	m_de_counter.mutex.Unlock();
	Lumen::CriticalPath::Event(Lumen::CriticalPath::Reason::GpuDeProgress, static_cast<uint64_t>(m_queue + 1), 0, counter);
}

void CommandProcessor::IncremenetCe()
{
	m_ce_counter.mutex.Lock();
	m_ce_counter.value++;
	const auto counter = m_ce_counter.value;
	m_ce_counter.cond_var.Signal();
	m_ce_counter.mutex.Unlock();
	Lumen::CriticalPath::Event(Lumen::CriticalPath::Reason::GpuCeProgress, static_cast<uint64_t>(m_queue + 1), 0, counter);
}

static const char* const_ram_range_rejection(uint32_t offset, uint32_t dw_num, size_t capacity_dw)
{
	// Retain the existing count/offset profile, then validate the combined span.
	// Subtraction follows the offset check, so neither it nor a count sum can wrap.
	if (dw_num >= capacity_dw) return "count";
	if (offset / sizeof(uint32_t) >= capacity_dw) return "offset";
	if ((offset & (sizeof(uint32_t) - 1u)) != 0) return "alignment";
	if (dw_num > capacity_dw - offset / sizeof(uint32_t)) return "range";
	return nullptr;
}

template <size_t Capacity>
static const char* const_ram_write(uint32_t (&ram)[Capacity], uint32_t offset, const uint32_t* src, uint32_t dw_num)
{
	if (const auto* reason = const_ram_range_rejection(offset, dw_num, Capacity)) return reason;
	if (dw_num == 0) return nullptr;
	if (src == nullptr) return "source";
	std::memcpy(ram + offset / sizeof(uint32_t), src, static_cast<size_t>(dw_num) * sizeof(uint32_t));
	return nullptr;
}

template <size_t Capacity>
static const char* const_ram_read(const uint32_t (&ram)[Capacity], uint32_t* dst, uint32_t offset, uint32_t dw_num)
{
	if (const auto* reason = const_ram_range_rejection(offset, dw_num, Capacity)) return reason;
	if (dw_num == 0) return nullptr;
	if (dst == nullptr) return "destination";
	std::memcpy(dst, ram + offset / sizeof(uint32_t), static_cast<size_t>(dw_num) * sizeof(uint32_t));
	return nullptr;
}

static void const_ram_reject(const char* operation, const char* reason, uint32_t offset, uint32_t dw_num)
{
	// span is the exclusive byte end, widened before either arithmetic operation.
	const uint64_t span = static_cast<uint64_t>(offset) + static_cast<uint64_t>(dw_num) * sizeof(uint32_t);
	EXIT("[GFX_CONST_RAM_REJECT] op=%s reason=%s offset=%u count=%u span=%" PRIu64 "\n", operation, reason, offset, dw_num, span);
}

void CommandProcessor::WriteConstRam(uint32_t offset, const uint32_t* src, uint32_t dw_num)
{
	if (dw_num != 0) EXIT_IF(!GraphicsRenderPrepareCpuMemoryAccess(reinterpret_cast<uint64_t>(src), uint64_t(dw_num) * 4, GuestCpu::MemoryAccessIntent::Read));
	Core::LockGuard lock(m_mutex);

	if (const auto* reason = const_ram_write(m_const_ram, offset, src, dw_num))
		const_ram_reject("write", reason, offset, dw_num);
}

void CommandProcessor::DumpConstRam(uint32_t* dst, uint32_t offset, uint32_t dw_num)
{
	if (dw_num != 0) EXIT_IF(!GraphicsRenderPrepareCpuMemoryAccess(reinterpret_cast<uint64_t>(dst), uint64_t(dw_num) * 4, GuestCpu::MemoryAccessIntent::Write));
	Core::LockGuard lock(m_mutex);

	if (const auto* reason = const_ram_read(m_const_ram, dst, offset, dw_num))
		const_ram_reject("dump", reason, offset, dw_num);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);
	GraphicsRenderMemoryFlush(m_buffer[m_current_buffer], reinterpret_cast<uint64_t>(dst), static_cast<size_t>(dw_num) * 4);
}

static bool WaitRegMemTraceEnabled()
{
	static const bool enabled = exact_one_environment_value(std::getenv("LUMEN_GFX_WAIT_REG_MEM_TRACE"));
	return enabled;
}

struct WaitRegMemTraceWindow
{
	uint32_t records = 0;
	uint64_t calls = 0, calls32 = 0, calls64 = 0;
	uint64_t flush_us = 0, polls = 0, sleep_us = 0, wait_us = 0, max_sleep_us = 0;
	std::chrono::steady_clock::time_point start {};
};

// One window per worker covers both packet widths. Disabled/exhausted tracing
// performs no clock reads; the guest predicate and requested sleep stay intact.
class WaitRegMemTrace
{
public:
	explicit WaitRegMemTrace(uint32_t width)
	{
		if (!WaitRegMemTraceEnabled()) return;
		static thread_local WaitRegMemTraceWindow window;
		if (window.records >= 64) return;
		m_window = &window;
		m_flush_start = Clock::now();
		if (window.calls == 0) window.start = m_flush_start;
		++window.calls;
		if (width == 32) ++window.calls32;
		else ++window.calls64;
	}
	void Flushed()
	{
		if (m_window == nullptr) return;
		m_wait_start = Clock::now();
		m_window->flush_us += Micros(m_wait_start - m_flush_start);
	}
	void Sleep()
	{
		if (m_window == nullptr)
		{
			GraphicsHostPollSleep(10);
			return;
		}
		const auto start = Clock::now();
		GraphicsHostPollSleep(10);
		const auto elapsed = Micros(Clock::now() - start);
		++m_window->polls;
		m_window->sleep_us += elapsed;
		if (elapsed > m_window->max_sleep_us) m_window->max_sleep_us = elapsed;
	}
	void Finish()
	{
		if (m_window == nullptr) return;
		const auto end = Clock::now();
		auto& window = *m_window;
		window.wait_us += Micros(end - m_wait_start);
		if (window.records >= 3 && end - window.start < std::chrono::seconds(5)) return;
		++window.records;
		std::fprintf(stderr,
		             "[GFX_WAIT_REG_MEM] worker=%p record=%u window_us=%" PRIu64 " calls=%" PRIu64
		             " calls32=%" PRIu64 " calls64=%" PRIu64 " flush_us=%" PRIu64 " polls=%" PRIu64
		             " sleep_us=%" PRIu64 " wait_us=%" PRIu64 " max_sleep_us=%" PRIu64
		             " requested_sleep_us=10 scope=completed-calls\n",
		             static_cast<void*>(&window), window.records, Micros(end - window.start), window.calls,
		             window.calls32, window.calls64, window.flush_us, window.polls, window.sleep_us, window.wait_us,
		             window.max_sleep_us);
		std::fflush(stderr);
		const auto records = window.records;
		window = {};
		window.records = records;
	}

private:
	using Clock = std::chrono::steady_clock;
	static uint64_t Micros(Clock::duration value)
	{
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(value).count());
	}
	WaitRegMemTraceWindow* m_window = nullptr;
	Clock::time_point m_flush_start {};
	Clock::time_point m_wait_start {};
};

template <typename T, typename CleanProof>
static bool GpuHandoffReadyMemoryWaitMatches(bool owns_epoch, const T* address, T reference, T mask, CleanProof&& clean)
{
	if (!owns_epoch) return false;
	// The parser's mutation-owner bypass alone does not publish GPU writes.
	// Retain mapping authority and require a fresh all-alias clean proof before
	// observing bytes. Any uncertainty keeps the original released wait path.
	Core::VirtualMemory::GuestDataLease lease;
	if (!Core::VirtualMemory::TryAcquireGuestDataLease(reinterpret_cast<uint64_t>(address), sizeof(T),
	        Core::VirtualMemory::Mode::Read, &lease, false) ||
	    !clean(reinterpret_cast<uint64_t>(address), sizeof(T))) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	return (__atomic_load_n(address, __ATOMIC_ACQUIRE) & mask) == reference;
#else
	return (*address & mask) == reference;
#endif
}

static void record_ready_memory_wait(CommandProcessor* cp, uint32_t width, bool ready)
{
	struct Counts { uint64_t checks = 0, ready = 0, slow = 0; };
	static thread_local Counts counts32, counts64;
	auto& counts = width == 32 ? counts32 : counts64;
	++counts.checks;
	if (ready) ++counts.ready; else ++counts.slow;
	if (counts.checks != 0 && (counts.checks & (counts.checks - 1)) == 0)
		std::fprintf(stderr, "[GPU_READY_MEMORY_WAIT] cp=%p thread=%d width=%u checks=%" PRIu64
		    " ready=%" PRIu64 " slow=%" PRIu64 "\n", static_cast<void*>(cp), Core::Thread::GetThreadIdUnique(),
		    width, counts.checks, counts.ready, counts.slow);
}

template <typename T, typename CleanProof>
static void GpuHandoffMemoryWaitAfterFlush(CommandProcessor* cp, uint64_t queue_identity, bool owns_epoch,
    const T* address, T reference, T mask, WaitRegMemTrace& trace, CleanProof&& clean)
{
	if (gpu_ready_memory_wait_enabled())
	{
		const bool ready = GpuHandoffReadyMemoryWaitMatches(owns_epoch, address, reference, mask, clean);
		record_ready_memory_wait(cp, sizeof(T) * 8u, ready);
		if (ready) { trace.Finish(); return; }
	}
	GpuHandoffWaitScope handoff_wait;
	Lumen::CriticalPath::Scope critical_wait(Lumen::CriticalPath::Reason::GpuRegisterWait, queue_identity);
	while (!GpuHandoffPollMatches(address, reference, mask)
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	       && !Loader::GuestExecutionStopRequested()
#endif
	)
	{
		trace.Sleep();
	}
	trace.Finish();
}

void CommandProcessor::WaitRegMem32(uint32_t func, const uint32_t* addr, uint32_t ref, uint32_t mask, uint32_t poll)
{
	EXIT_NOT_IMPLEMENTED(func != 3);
	EXIT_NOT_IMPLEMENTED(poll != 10);

	WaitRegMemTrace trace(32);
	BufferFlush();
	trace.Flushed();
	const bool owns_epoch = g_gpu_handoff_claim != nullptr && g_gpu_handoff_claim->OwnsEpochFor(this);
	GpuHandoffMemoryWaitAfterFlush(this, static_cast<uint64_t>(m_queue + 1), owns_epoch, addr, ref, mask, trace,
	    [](uint64_t address, uint64_t bytes) {
		    return GpuMemoryCpuMemoryAccessNeedsNoRenderer(address, bytes, GuestCpu::MemoryAccessIntent::Read);
	    });
}

void CommandProcessor::WaitRegMem64(uint32_t func, const uint64_t* addr, uint64_t ref, uint64_t mask, uint32_t poll)
{
	EXIT_NOT_IMPLEMENTED(func != 3);
	EXIT_NOT_IMPLEMENTED(poll != 10);

	WaitRegMemTrace trace(64);
	BufferFlush();
	trace.Flushed();
	const bool owns_epoch = g_gpu_handoff_claim != nullptr && g_gpu_handoff_claim->OwnsEpochFor(this);
	GpuHandoffMemoryWaitAfterFlush(this, static_cast<uint64_t>(m_queue + 1), owns_epoch, addr, ref, mask, trace,
	    [](uint64_t address, uint64_t bytes) {
		    return GpuMemoryCpuMemoryAccessNeedsNoRenderer(address, bytes, GuestCpu::MemoryAccessIntent::Read);
	    });
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
static bool graphics_run_ready_memory_wait_test(CommandProcessor* cp)
{
	const bool enabled = gpu_ready_memory_wait_enabled();
	if (enabled != exact_one_environment_value(std::getenv("LUMEN_GPU_READY_MEMORY_WAIT"))) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	namespace Vm = Core::VirtualMemory;
	Vm::Init();
	constexpr uint64_t page = 65536;
	const uint64_t allocation = Vm::Alloc(0, page, Vm::Mode::ReadWrite);
	if (allocation == 0) return false;
	struct Owner { uint64_t address; ~Owner() { Vm::Free(address); } } owner {allocation};
	enum class Case { Ready, NotReady, Masked, NoOwner, Revoked, ReadOnly, Remapped };
	uint32_t cases = 0;
	const auto check_width = [&](auto reference) {
		using T = decltype(reference);
		auto* address = reinterpret_cast<T*>(allocation);
		for (const auto scenario: {Case::Ready, Case::NotReady, Case::Masked, Case::NoOwner,
		    Case::Revoked, Case::ReadOnly, Case::Remapped})
		{
			if (!Vm::Protect(allocation, page, Vm::Mode::ReadWrite)) return false;
			*address = reference;
			const T mask = scenario == Case::Masked ? T(0x7fffffff) : ~T(0);
			if (scenario == Case::NotReady) *address = 0;
			if (scenario == Case::Masked) *address |= T(0x80000000);
			if (scenario == Case::Revoked && !Vm::Protect(allocation, page, Vm::Mode::NoAccess)) return false;
			if (scenario == Case::ReadOnly && !Vm::Protect(allocation, page, Vm::Mode::Read)) return false;
			if (scenario == Case::Remapped)
			{
				// Same VA, new mapping and different bytes: no prior ready result
				// or permission observation may survive into this request.
				if (!Vm::Free(allocation) || !Vm::AllocFixed(allocation, page, Vm::Mode::ReadWrite)) return false;
				*address = 0;
			}
			struct Observation
			{
				uint64_t allocation;
				T reference;
				bool valid = true;
				uint32_t releases = 0, readmissions = 0, proofs = 0;
			} observed {allocation, reference};
			GpuCounterWaitTestProbe probe;
			probe.context = &observed;
			probe.handoff = [](void* context, bool release) {
				auto& state = *static_cast<Observation*>(context);
				state.valid &= !Vm::CurrentThreadHasMemoryLease();
				if (!release) { ++state.readmissions; return; }
				++state.releases;
				// The slow scope must release every short lease before a native
				// producer can mutate the mapping and publish the watched value.
				std::thread producer([&] {
					state.valid &= Vm::Protect(state.allocation, page, Vm::Mode::ReadWrite);
					if (state.valid) *reinterpret_cast<T*>(state.allocation) = state.reference;
				});
				producer.join();
			};
			auto* previous = g_gpu_counter_wait_test_probe;
			g_gpu_counter_wait_test_probe = &probe;
			WaitRegMemTrace trace(sizeof(T) * 8u);
			trace.Flushed();
			GpuHandoffMemoryWaitAfterFlush(cp, 1, scenario != Case::NoOwner, address, reference, mask, trace,
			    [&](uint64_t start, uint64_t bytes) {
				    ++observed.proofs;
				    observed.valid &= Vm::CurrentThreadHasMemoryLease() && start == allocation && bytes == sizeof(T);
				    return true;
			    });
			g_gpu_counter_wait_test_probe = previous;
			const bool ready = scenario == Case::Ready || scenario == Case::Masked || scenario == Case::ReadOnly;
			const uint32_t handoffs = enabled && ready ? 0 : 1;
			const uint32_t proofs = enabled && scenario != Case::NoOwner && scenario != Case::Revoked ? 1 : 0;
			if (!observed.valid || observed.releases != handoffs || observed.readmissions != handoffs ||
			    observed.proofs != proofs || Vm::CurrentThreadHasMemoryLease()) return false;
			++cases;
		}
		return true;
	};
	if (!check_width(uint32_t(0x12345678)) || !check_width(uint64_t(0x12345678))) return false;
	struct FixtureContext { CommandProcessor* cp; bool enabled; uint32_t cases = 0; } fixture {cp, enabled};
	if (!GpuMemoryReadyMemoryWaitContractTest([](void* context, uint64_t address, uint64_t bytes, bool clean_expected,
	    bool (*clean)(void*, uint64_t, uint64_t), void* memory) {
		auto& fixture = *static_cast<FixtureContext*>(context);
		struct Observation { bool valid = true; uint32_t releases = 0, readmissions = 0, proofs = 0; } observed;
		GpuCounterWaitTestProbe probe;
		probe.context = &observed;
		probe.handoff = [](void* context, bool release) {
			auto& state = *static_cast<Observation*>(context);
			state.valid &= !Vm::CurrentThreadHasMemoryLease();
			if (release) ++state.releases; else ++state.readmissions;
		};
		auto* previous = g_gpu_counter_wait_test_probe;
		g_gpu_counter_wait_test_probe = &probe;
		const auto run = [&](auto reference) {
			using T = decltype(reference);
			WaitRegMemTrace trace(sizeof(T) * 8u);
			trace.Flushed();
			GpuHandoffMemoryWaitAfterFlush(fixture.cp, 1, true, reinterpret_cast<const T*>(address),
			    reference, ~T(0), trace, [&](uint64_t start, uint64_t count) {
				    ++observed.proofs;
				    observed.valid &= Vm::CurrentThreadHasMemoryLease() && start == address && count == bytes;
				    return clean(memory, start, count);
			    });
		};
		if (bytes == 4) run(uint32_t(0x12345678)); else if (bytes == 8) run(uint64_t(0x12345678)); else return false;
		g_gpu_counter_wait_test_probe = previous;
		const uint32_t handoffs = fixture.enabled && clean_expected ? 0 : 1;
		++fixture.cases;
		return observed.valid && observed.releases == handoffs && observed.readmissions == handoffs &&
		    observed.proofs == (fixture.enabled ? 1u : 0u) && !Vm::CurrentThreadHasMemoryLease();
	}, &fixture)) return false;
	std::fprintf(stderr, "[GFX_READY_MEMORY_WAIT_TEST] selector=%u widths=32/64 VM-cases=%u tracker-cases=%u"
	    " clean-ready-release=%u dirty/stale/unknown-alias=slow revoke/remap=pass lease-before-proof=real"
	    " lease-across-release=0 mapping/producer/tracker=real epoch/submission=modeled\n",
	    enabled ? 1u : 0u, cases, fixture.cases, enabled ? 0u : 1u);
#else
	(void)cp;
#endif
	return true;
}
#endif

void CommandProcessor::WriteData(uint32_t* dst, const uint32_t* src, uint32_t dw_num, uint32_t write_control, bool custom)
{
	if (dw_num != 0)
	{
		EXIT_IF(!GraphicsRenderPrepareCpuMemoryAccess(reinterpret_cast<uint64_t>(src), uint64_t(dw_num) * 4, GuestCpu::MemoryAccessIntent::Read));
		EXIT_IF(!GraphicsRenderPrepareCpuMemoryAccess(reinterpret_cast<uint64_t>(dst), uint64_t(dw_num) * 4, GuestCpu::MemoryAccessIntent::Write));
	}
	Core::LockGuard lock(m_mutex);

	EXIT_NOT_IMPLEMENTED(!custom && write_control != 0x04100500);
	EXIT_NOT_IMPLEMENTED(custom && write_control != 0x01000004);

	memcpy(dst, src, static_cast<size_t>(dw_num) * 4);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);
	GraphicsRenderMemoryFlush(m_buffer[m_current_buffer], reinterpret_cast<uint64_t>(dst), static_cast<size_t>(dw_num) * 4);
}

void CommandProcessor::CopyDmaData(uint64_t source_address, uint64_t destination_address, uint32_t byte_count)
{
	if (byte_count == 0)
	{
		// Native-only prefix waiting must begin outside every CP lock level.
		BufferFlushAndWaitForPrefix(Lumen::CriticalPath::Reason::GpuBufferWait);
		return;
	}
	if (byte_count != 0)
	{
		EXIT_IF(!GraphicsRenderPrepareCpuMemoryAccess(source_address, byte_count, GuestCpu::MemoryAccessIntent::Read));
		EXIT_IF(!GraphicsRenderPrepareCpuMemoryAccess(destination_address, byte_count, GuestCpu::MemoryAccessIntent::Write));
	}
	Core::LockGuard lock(m_mutex);

	EXIT_NOT_IMPLEMENTED(!dma_data_memory_copy_ranges_supported(source_address, destination_address, byte_count));
	EXIT_NOT_IMPLEMENTED(!GpuMemoryContainsAllocatedRange(source_address, byte_count));
	EXIT_NOT_IMPLEMENTED(!GpuMemoryContainsAllocatedRange(destination_address, byte_count));

	EXIT_NOT_IMPLEMENTED(!GraphicsRenderCopyDmaData(this, source_address, destination_address, byte_count));
}

void GraphicsRing::Submit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw, int handle,
                          int index, int flip_mode, int64_t flip_arg)
{
	EXIT_IF(m_cp == nullptr);

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	if (g_submission_admission_lock_probe != nullptr)
	{
		// Exercise the real Gpu submission entry points without initializing a
		// window or Vulkan. A distinct thread tests ownership at the ring boundary;
		// a recursive mutex probe on the submitting thread would be inconclusive.
		auto& probe = *g_submission_admission_lock_probe;
		std::thread contender([&probe]()
		{
			const bool acquired = probe.mutex->TryLock();
			if (acquired)
			{
				probe.mutex->Unlock();
			}
			probe.observed_locked = !acquired && probe.reader_active;
		});
		contender.join();
		probe.calls++;
		return;
	}
#endif

	if (num_draw_dw != 0) EXIT_IF(!GraphicsRenderPrepareCpuMemoryAccess(reinterpret_cast<uint64_t>(cmd_draw_buffer), uint64_t(num_draw_dw) * 4, GuestCpu::MemoryAccessIntent::Read));
	if (num_const_dw != 0) EXIT_IF(!GraphicsRenderPrepareCpuMemoryAccess(reinterpret_cast<uint64_t>(cmd_const_buffer), uint64_t(num_const_dw) * 4, GuestCpu::MemoryAccessIntent::Read));
	Core::LockGuard lock(m_mutex);

	GraphicsRenderCreateContext();

	if (m_done)
	{
		// Gpu admission performed this wait without an HLE reader and then
		// rechecked it under the outer mutex. No submit/done can invalidate it.
		EXIT_IF(!m_idle);
		m_done = false;

		m_cp->Reset();
	}

	CmdBatch buf {};
	buf.draw_buffer.data    = cmd_draw_buffer;
	buf.draw_buffer.num_dw  = num_draw_dw;
	buf.const_buffer.data   = cmd_const_buffer;
	buf.const_buffer.num_dw = num_const_dw;
	const bool root_ownership = root_command_ownership_enabled();
	const bool provenance     = command_provenance_enabled();
	if (!root_command_ownership_configuration_valid(root_ownership, provenance))
	{
		EXIT("LUMEN_GFX_ROOT_COMMAND_OWNERSHIP conflicts with LUMEN_GFX_COMMAND_PROVENANCE\n");
	}
	if (!m_root_ownership_config_reported)
	{
		dbg_printf("GFX command configuration: root_command_ownership=%u root_command_scope=dcb_safe_only"
		           " root_command_max_batch_dw=%" PRIu32 " root_command_max_total_dw=%" PRIu64 "\n",
		           root_ownership ? 1u : 0u, ROOT_COMMAND_OWNERSHIP_MAX_DWORDS, ROOT_COMMAND_OWNERSHIP_MAX_TOTAL_DWORDS);
		m_root_ownership_config_reported = true;
	}
	if (root_ownership && !m_root_ownership_admission_closed)
	{
		const auto initial_decision = root_command_ownership_decision(cmd_draw_buffer, num_draw_dw, cmd_const_buffer, num_const_dw,
		                                                               m_idle && m_cmd_batches.Size() == 0);
		auto decision = initial_decision;
		if (initial_decision == RootCommandOwnershipDecision::Eligible &&
		    !root_command_ownership_storage_available(m_root_ownership_dwords, num_draw_dw))
		{
			decision = RootCommandOwnershipDecision::Capacity;
		}
		if (decision == RootCommandOwnershipDecision::Capacity)
		{
			m_root_ownership_capacity_failures++;
			EXIT("root command ownership capacity exceeded: requested_dw=%" PRIu32 " retained_dw=%" PRIu64 "\n", num_draw_dw,
			     m_root_ownership_dwords);
		}
		if (initial_decision == RootCommandOwnershipDecision::Eligible)
		{
			if (m_root_ownership_storage == nullptr)
			{
				m_root_ownership_storage = std::make_shared<Vector<uint32_t>>();
			} else
			{
				EXIT_IF(m_root_ownership_storage.use_count() != 1);
				m_root_ownership_storage->Clear();
			}
			m_root_ownership_storage->Add(cmd_draw_buffer, num_draw_dw);
			buf.draw_buffer.submitted = m_root_ownership_storage;
			const auto copied_decision = root_command_ownership_decision(buf.draw_buffer.submitted->GetDataConst(), num_draw_dw, nullptr, 0,
			                                                               true, cmd_draw_buffer);
			const bool snapshot_valid = root_command_ownership_snapshot_valid(copied_decision, buf.draw_buffer.submitted->GetDataConst(),
			                                                                  cmd_draw_buffer, num_draw_dw);
			if (!snapshot_valid)
			{
				buf.draw_buffer.submitted.reset();
				m_root_ownership_rejected++;
				EXIT("root command ownership copy changed during submit: copied_decision=%u\n", static_cast<unsigned>(copied_decision));
			}
			buf.draw_buffer.owned = true;
			m_root_ownership_dwords += num_draw_dw;
		}
		m_root_ownership_decisions[static_cast<size_t>(initial_decision)]++;
	}
	if (provenance)
	{
		static constexpr uint64_t MAX_PROVENANCE_BATCH_DWORDS = 1024u * 1024u;
		static constexpr uint64_t MAX_PROVENANCE_TOTAL_DWORDS = 8u * 1024u * 1024u;
		const uint64_t requested = (cmd_draw_buffer != nullptr ? static_cast<uint64_t>(num_draw_dw) : 0u) +
		                           (cmd_const_buffer != nullptr ? static_cast<uint64_t>(num_const_dw) : 0u);
		if (!m_provenance_config_reported)
		{
			dbg_printf("[GFX_CMD_PROVENANCE] configuration enabled=1 max_batch_dw=%" PRIu64 " max_total_dw=%" PRIu64 "\n",
			           MAX_PROVENANCE_BATCH_DWORDS, MAX_PROVENANCE_TOTAL_DWORDS);
			m_provenance_config_reported = true;
		}
		if (requested <= MAX_PROVENANCE_BATCH_DWORDS && requested <= MAX_PROVENANCE_TOTAL_DWORDS - m_provenance_dwords)
		{
			if (cmd_draw_buffer != nullptr && num_draw_dw != 0)
			{
				buf.draw_buffer.submitted = std::make_shared<Vector<uint32_t>>();
				buf.draw_buffer.submitted->Add(cmd_draw_buffer, num_draw_dw);
			}
			if (cmd_const_buffer != nullptr && num_const_dw != 0)
			{
				buf.const_buffer.submitted = std::make_shared<Vector<uint32_t>>();
				buf.const_buffer.submitted->Add(cmd_const_buffer, num_const_dw);
			}
			m_provenance_dwords += requested;
		} else if (!m_provenance_cap_reported)
		{
			dbg_printf("[GFX_CMD_PROVENANCE] shadow=unavailable reason=bounded-cap requested_dw=%" PRIu64
			           " retained_dw=%" PRIu64 "\n",
			           requested, m_provenance_dwords);
			m_provenance_cap_reported = true;
		}
	}
	buf.flip.handle         = handle;
	buf.flip.index          = index;
	buf.flip.flip_mode      = flip_mode;
	buf.flip.flip_arg       = flip_arg;

	m_cmd_batches.Add(buf);
	m_idle = false;

	m_cond_var.Signal();
}

bool GraphicsRing::IsSubmitReady()
{
	Core::LockGuard lock(m_mutex);
	return !m_done || m_idle;
}

void GraphicsRing::WaitForSubmitReady()
{
	Core::LockGuard lock(m_mutex);
	while (m_done && !m_idle) m_idle_cond_var.Wait(&m_mutex);
}

void GraphicsRing::Done()
{
	Core::LockGuard lock(m_mutex);
	if (m_done)
	{
		while (!m_idle)
		{
			m_idle_cond_var.Wait(&m_mutex);
		}
	}
	m_done = true;
}

void GraphicsRing::WaitForIdle()
{
	Core::LockGuard lock(m_mutex);
	while (!m_idle || m_cmd_batches.Size() != 0)
	{
		m_idle_cond_var.Wait(&m_mutex);
	}
}

bool GraphicsRing::IsIdle()
{
	Core::LockGuard lock(m_mutex);
	return m_idle && m_cmd_batches.Size() == 0;
}

bool GraphicsRing::ReportRootOwnershipFinal()
{
	if (!root_command_ownership_enabled())
	{
		return true;
	}
	Core::LockGuard lock(m_mutex);
	if (m_root_ownership_final_reported)
	{
		return m_root_ownership_final_clean;
	}
	m_root_ownership_admission_closed = true;
	m_root_ownership_final_reported = true;
	uint64_t decisions = 0;
	for (const auto count: m_root_ownership_decisions)
	{
		decisions += count;
	}
	const uint64_t owned    = m_root_ownership_decisions[static_cast<size_t>(RootCommandOwnershipDecision::Eligible)];
	const uint64_t rejected = m_root_ownership_rejected;
	const uint64_t legacy   = decisions - owned;
	const uint64_t batches  = decisions + rejected;
	const uint64_t queued   = m_cmd_batches.Size();
	const uint64_t active   = m_idle ? 0u : 1u;
	const bool clean        = rejected == 0 && m_root_ownership_capacity_failures == 0 && owned > 0;
	auto decision_count = [this](RootCommandOwnershipDecision decision)
	{
		return m_root_ownership_decisions[static_cast<size_t>(decision)];
	};
	m_root_ownership_final_clean = clean;
	std::fprintf(stderr,
	             "[GFX_ROOT_COMMAND_FINAL] batches=%" PRIu64 " owned_dcb=%" PRIu64 " legacy_dcb=%" PRIu64
	             " not_quiescent=%" PRIu64 " const_buffer=%" PRIu64 " empty=%" PRIu64
	             " capacity=%" PRIu64 " malformed=%" PRIu64 " unsupported=%" PRIu64
	             " indirect=%" PRIu64 " wait=%" PRIu64 " cede=%" PRIu64 " write_overlap=%" PRIu64
	             " rejected=%" PRIu64 " cap_fail=%" PRIu64 " admission_closed=1 queued=%" PRIu64
	             " active=%" PRIu64 " outstanding_dw=%" PRIu64 " status=%s\n",
	             batches, owned, legacy,
	             decision_count(RootCommandOwnershipDecision::NotQuiescent),
	             decision_count(RootCommandOwnershipDecision::ConstBuffer),
	             decision_count(RootCommandOwnershipDecision::Empty),
	             decision_count(RootCommandOwnershipDecision::Capacity),
	             decision_count(RootCommandOwnershipDecision::Malformed),
	             decision_count(RootCommandOwnershipDecision::Unsupported),
	             decision_count(RootCommandOwnershipDecision::Indirect),
	             decision_count(RootCommandOwnershipDecision::WaitDependency),
	             decision_count(RootCommandOwnershipDecision::CeDe),
	             decision_count(RootCommandOwnershipDecision::WriteOverlap), rejected,
	             m_root_ownership_capacity_failures, queued, active, m_root_ownership_dwords,
	             clean ? "clean" : "unhealthy");
	std::fflush(stderr);
	return clean;
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
void GraphicsRing::ResetIOSRuntimeSession()
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(!m_idle || m_cmd_batches.Size() != 0);
	EXIT_IF(m_root_ownership_dwords != 0 || (m_root_ownership_storage != nullptr && m_root_ownership_storage.use_count() != 1));
	m_root_ownership_storage.reset();
	m_done = true;
}

bool GraphicsRing::IsDormantIOSRuntimeSession()
{
	Core::LockGuard lock(m_mutex);
	return m_done && m_idle && m_cmd_batches.Size() == 0;
}
#endif

GraphicsRing::CmdBatch GraphicsRing::GetCmdBatch()
{
	Core::LockGuard lock(m_mutex);

	while (m_cmd_batches.Size() == 0)
	{
		m_idle = true;
		m_idle_cond_var.Signal();

		m_cond_var.Wait(&m_mutex);
	}

	m_idle = false;

	auto first = m_cmd_batches.First();

	CmdBatch buf = m_cmd_batches.At(first);

	m_cmd_batches.Remove(first);

	return buf;
}

void GraphicsRing::ReleaseProvenance(CmdBatch* batch)
{
	EXIT_IF(batch == nullptr);
	const uint64_t provenance_retained =
	    (!batch->draw_buffer.owned && batch->draw_buffer.submitted != nullptr ? batch->draw_buffer.submitted->Size() : 0u) +
	    (!batch->const_buffer.owned && batch->const_buffer.submitted != nullptr ? batch->const_buffer.submitted->Size() : 0u);
	const uint64_t ownership_retained =
	    (batch->draw_buffer.owned && batch->draw_buffer.submitted != nullptr ? batch->draw_buffer.submitted->Size() : 0u) +
	    (batch->const_buffer.owned && batch->const_buffer.submitted != nullptr ? batch->const_buffer.submitted->Size() : 0u);
	batch->draw_buffer.submitted.reset();
	batch->const_buffer.submitted.reset();
	batch->draw_buffer.owned = false;
	batch->const_buffer.owned = false;
	if (provenance_retained == 0 && ownership_retained == 0)
	{
		return;
	}
	Core::LockGuard lock(m_mutex);
	EXIT_IF(provenance_retained > m_provenance_dwords || ownership_retained > m_root_ownership_dwords);
	m_provenance_dwords -= provenance_retained;
	m_root_ownership_dwords -= ownership_retained;
}

void GraphicsRing::ThreadBatchRun(void* data)
{
	EXIT_IF(data == nullptr);

	static std::atomic_uint64_t seq = 0;

	auto* ring = static_cast<GraphicsRing*>(data);
	auto* cp   = ring->m_cp;

	EXIT_IF(ring == nullptr);
	EXIT_IF(cp == nullptr);
	uint64_t diagnostic_batches  = 0;
	uint64_t diagnostic_us       = 0;
	uint64_t diagnostic_jobs_us  = 0;
	uint64_t diagnostic_flush_us = 0;
	uint64_t diagnostic_draw_us  = 0;
	uint64_t diagnostic_const_us = 0;
	static const bool frame_pipeline_trace = []()
	{
		const char* value = std::getenv("LUMEN_FRAME_PIPELINE_TRACE");
		return value != nullptr && value[0] != '\0' && value[0] != '0';
	}();
	const bool diagnostics_enabled = Config::DiagnosticsEnabled() || frame_pipeline_trace;

	for (;;)
	{
		CmdBatch buf = ring->GetCmdBatch();
		std::chrono::steady_clock::time_point diagnostic_start;
		if (diagnostics_enabled)
		{
			diagnostic_start = std::chrono::steady_clock::now();
		}
		uint64_t batch_jobs_us     = 0;
		uint64_t batch_flush_us    = 0;
		uint64_t batch_draw_us     = 0;
		uint64_t batch_const_us    = 0;

		cp->RunLock();
		{
			{
			// Coordinator work can retire native resources and lock producers.
			// It must join the same admission order as the parser workers.
			GpuHandoffClaimScope setup_claim(cp, gpu_range_execution_enabled());
			cp->BufferInit();
			cp->ResetDeCe();
			cp->SetFlip(buf.flip);
			cp->SetSumbitId(++seq);
			cp->SetEopCheckpointExclusiveStream(buf.const_buffer.num_dw == 0);
			}

			std::chrono::steady_clock::time_point jobs_start;
			if (diagnostics_enabled)
			{
				jobs_start = std::chrono::steady_clock::now();
			}
			ring->m_job1.Execute([cp, buf, &batch_draw_us, diagnostics_enabled](void* /*unused*/)
			{
				std::chrono::steady_clock::time_point start;
				if (diagnostics_enabled)
				{
					start = std::chrono::steady_clock::now();
				}
				EXIT_IF(buf.draw_buffer.owned && buf.draw_buffer.submitted == nullptr);
				auto* parse_data = !buf.draw_buffer.owned ? buf.draw_buffer.data : buf.draw_buffer.submitted->GetData();
				cp->Run(parse_data, buf.draw_buffer.num_dw,
				        buf.draw_buffer.owned || buf.draw_buffer.submitted == nullptr ? nullptr : buf.draw_buffer.submitted->GetDataConst(),
				        CommandStreamKind::Draw,
				        buf.draw_buffer.data);
				if (diagnostics_enabled)
				{
					batch_draw_us = static_cast<uint64_t>(
					    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
				}
			});
			ring->m_job2.Execute([cp, buf, &batch_const_us, diagnostics_enabled](void* /*unused*/)
			{
				std::chrono::steady_clock::time_point start;
				if (diagnostics_enabled)
				{
					start = std::chrono::steady_clock::now();
				}
				EXIT_IF(buf.const_buffer.owned && buf.const_buffer.submitted == nullptr);
				auto* parse_data = !buf.const_buffer.owned ? buf.const_buffer.data : buf.const_buffer.submitted->GetData();
				cp->Run(parse_data, buf.const_buffer.num_dw,
				        buf.const_buffer.owned || buf.const_buffer.submitted == nullptr ? nullptr : buf.const_buffer.submitted->GetDataConst(),
				        CommandStreamKind::Const,
				        buf.const_buffer.data);
				if (diagnostics_enabled)
				{
					batch_const_us = static_cast<uint64_t>(
					    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
				}
			});
			ring->m_job1.Wait();
			ring->m_job2.Wait();
			std::chrono::steady_clock::time_point jobs_end;
			if (diagnostics_enabled)
			{
				jobs_end = std::chrono::steady_clock::now();
			}

			// ACQUIRE_MEM barriers remain ordered in the Vulkan command buffer. Make
			// their results visible to the CPU once at the PS4 batch boundary instead
			// of submitting and waiting after every individual cache operation.
			if (!GpuMemoryCpuHandoffEnabled())
			{
				cp->WriteBack();
				cp->BufferFlush();
			} else if (GpuMemoryReadOnlyStorageRetirementDue())
			{
				// Wait() above joins and destroys both parser callbacks. Acquire the
				// new epoch before taking any CP/renderer/tracker lock; RunLock keeps
				// the joined boundary stable. Due() released its tracker lock already
				// and the collector rechecks the frame/pressure after admission.
				GpuHandoffClaimScope claim(cp);
				cp->BufferFlushPending();
				if (claim.OwnsEpoch())
				{
					const JoinedGraphicsParserAuthority joined(cp);
					cp->RetireCompletedReadOnlyStorage(joined);
				}
			} else
			{
				GpuHandoffClaimScope claim(cp, gpu_range_execution_enabled());
				cp->BufferFlushPending();
			}
			if (diagnostics_enabled)
			{
				batch_jobs_us = static_cast<uint64_t>(
				    std::chrono::duration_cast<std::chrono::microseconds>(jobs_end - jobs_start).count());
				batch_flush_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
				                                                 std::chrono::steady_clock::now() - jobs_end)
				                                                 .count());
			}
		}
		cp->RunUnlock();
		ring->ReleaseProvenance(&buf);

		if (diagnostics_enabled)
		{
			diagnostic_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			                                                std::chrono::steady_clock::now() - diagnostic_start)
			                                                .count());
			diagnostic_jobs_us += batch_jobs_us;
			diagnostic_flush_us += batch_flush_us;
			diagnostic_draw_us += batch_draw_us;
			diagnostic_const_us += batch_const_us;
			diagnostic_batches++;
			if ((diagnostic_batches % 120) == 0)
			{
				dbg_printf("[PERF] command batches=%" PRIu64 " avg=%.3f ms\n", diagnostic_batches,
				           static_cast<double>(diagnostic_us) / static_cast<double>(diagnostic_batches) / 1000.0);
				dbg_printf("[PERF] command jobs=%.3f ms flush=%.3f ms\n",
				           static_cast<double>(diagnostic_jobs_us) / static_cast<double>(diagnostic_batches) / 1000.0,
				           static_cast<double>(diagnostic_flush_us) / static_cast<double>(diagnostic_batches) / 1000.0);
				dbg_printf("[PERF] command draw=%.3f ms const=%.3f ms\n",
				           static_cast<double>(diagnostic_draw_us) / static_cast<double>(diagnostic_batches) / 1000.0,
				           static_cast<double>(diagnostic_const_us) / static_cast<double>(diagnostic_batches) / 1000.0);
				diagnostic_us       = 0;
				diagnostic_jobs_us  = 0;
				diagnostic_flush_us = 0;
				diagnostic_draw_us  = 0;
				diagnostic_const_us = 0;
				diagnostic_batches  = 0;
			}
		}
	}
}

void ComputeRing::ThreadRun(void* data)
{
	EXIT_IF(data == nullptr);

	auto* ring = static_cast<ComputeRing*>(data);
	auto* cp   = ring->m_cp;

	EXIT_IF(ring == nullptr);
	EXIT_IF(cp == nullptr);

	uint32_t* buffer = nullptr;

	LUMEN_PROFILER_THREAD("Thread_Compute");

	ring->m_mutex.Lock();

	for (;;)
	{
		while (!(ring->m_active && ring->m_run_offset_dw > 0))
		{
			ring->m_idle = true;
			ring->m_idle_cond_var.Signal();
			ring->m_cond_var.Wait(&ring->m_mutex);
		}

		ring->m_idle = false;
		ring->m_mutex.Unlock();
		{
		GpuHandoffClaimScope claim(cp);
		ring->m_mutex.Lock();
		EXIT_IF(!ring->m_active || ring->m_run_offset_dw == 0);
		EXIT_IF(!GraphicsRenderPrepareCpuMemoryAccess(reinterpret_cast<uint64_t>(ring->m_read_ptr_addr), 4, GuestCpu::MemoryAccessIntent::Read));

		auto  pos       = *ring->m_read_ptr_addr;
		auto  num_dw    = ring->m_run_offset_dw;
		auto  next_pos  = pos + num_dw;
		auto  ring_size = ring->m_ring_size_dw;
		auto* ring_addr = ring->m_ring_addr;

		if (next_pos <= ring_size)
		{
			buffer = ring_addr + pos;
			EXIT_IF(!GraphicsRenderPrepareCpuMemoryAccess(reinterpret_cast<uint64_t>(buffer), uint64_t(num_dw) * 4, GuestCpu::MemoryAccessIntent::Read));
		} else
		{
			auto d1 = next_pos - ring_size;
			auto d2 = num_dw - d1;
			buffer  = ring->m_internal_buffer;
			EXIT_IF(!GraphicsRenderPrepareCpuMemoryAccess(reinterpret_cast<uint64_t>(ring_addr + pos), uint64_t(d2) * 4, GuestCpu::MemoryAccessIntent::Read));
			EXIT_IF(!GraphicsRenderPrepareCpuMemoryAccess(reinterpret_cast<uint64_t>(ring_addr), uint64_t(d1) * 4, GuestCpu::MemoryAccessIntent::Read));
			memcpy(buffer, ring_addr + pos, uint64_t(d2) * 4);
			memcpy(buffer + d2, ring_addr, uint64_t(d1) * 4);
		}

		ring->m_mutex.Unlock();

		cp->RunLock();
		{
			cp->BufferInit();
			cp->ResetDeCe();

			GraphicsDbgDumpDcb("cc", num_dw, buffer);
			cp->SetEopCheckpointExclusiveStream(true);

			cp->Run(buffer, num_dw, nullptr, CommandStreamKind::Compute);

			cp->BufferFlush();
		}
		cp->RunUnlock();

		ring->m_mutex.Lock();

		ring->m_run_offset_dw -= num_dw;
		EXIT_IF(!GraphicsRenderPrepareCpuMemoryAccess(reinterpret_cast<uint64_t>(ring->m_read_ptr_addr), 4, GuestCpu::MemoryAccessIntent::Write));
		*ring->m_read_ptr_addr = next_pos % ring_size;
		ring->m_mutex.Unlock();
		}
		ring->m_mutex.Lock();
	}
}

void ComputeRing::MapComputeQueue(uint32_t* ring_addr, uint32_t ring_size_dw, uint32_t* read_ptr_addr)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_active);

	if (m_ring_size_dw != ring_size_dw)
	{
		delete[] m_internal_buffer;
		m_internal_buffer = new uint32_t[ring_size_dw];
	}

	m_ring_addr               = ring_addr;
	m_ring_size_dw            = ring_size_dw;
	m_read_ptr_addr           = read_ptr_addr;
	m_run_offset_dw           = 0;
	m_last_dingdong_offset_dw = 0;

	*m_read_ptr_addr = 0;
	m_active         = true;

	m_cond_var.Signal();
}

void ComputeRing::DingDong(uint32_t offset_dw)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(!m_active);

	GraphicsRenderCreateContext();

	if (m_done)
	{
		while (!m_idle)
		{
			m_idle_cond_var.Wait(&m_mutex);
		}
		m_done = false;
		/*m_cp->Reset();*/
	}

	uint32_t advance = (offset_dw + (offset_dw < m_last_dingdong_offset_dw ? m_ring_size_dw : 0)) - m_last_dingdong_offset_dw;

	m_run_offset_dw += advance;
	if (advance > 0)
	{
		m_idle = false;
	}

	m_last_dingdong_offset_dw = offset_dw;

	m_cond_var.Signal();
}

void ComputeRing::Done()
{
	Core::LockGuard lock(m_mutex);
	if (m_done)
	{
		while (!m_idle)
		{
			m_idle_cond_var.Wait(&m_mutex);
		}
	}
	m_done = true;
}

void ComputeRing::WaitForIdle()
{
	Core::LockGuard lock(m_mutex);
	while (!m_idle || (m_active && m_run_offset_dw > 0))
	{
		m_idle_cond_var.Wait(&m_mutex);
	}
}

bool ComputeRing::IsIdle()
{
	Core::LockGuard lock(m_mutex);
	return m_idle && (!m_active || m_run_offset_dw == 0);
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
bool ComputeRing::CanResetIOSRuntimeSession()
{
	Core::LockGuard lock(m_mutex);
	return m_idle && m_run_offset_dw == 0;
}

void ComputeRing::ResetIOSRuntimeSession()
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(!m_idle || m_run_offset_dw != 0);

	m_done   = true;
	m_active = false;
	delete[] m_internal_buffer;
	m_internal_buffer         = nullptr;
	m_ring_addr               = nullptr;
	m_read_ptr_addr           = nullptr;
	m_ring_size_dw            = 0;
	m_run_offset_dw           = 0;
	m_last_dingdong_offset_dw = 0;
	m_cond_var.Signal();
}

bool ComputeRing::IsDormantIOSRuntimeSession()
{
	Core::LockGuard lock(m_mutex);
	return m_done && m_idle && !m_active && m_ring_addr == nullptr && m_read_ptr_addr == nullptr && m_ring_size_dw == 0 &&
	       m_run_offset_dw == 0 && m_last_dingdong_offset_dw == 0;
}
#endif

void ComputeRing::SetActive(bool flag)
{
	Core::LockGuard lock(m_mutex);

	m_active = flag;

	m_cond_var.Signal();
}

bool ComputeRing::IsActive()
{
	Core::LockGuard lock(m_mutex);

	return m_active;
}

static uint32_t* command_processor_guest_data(uint32_t* parse_data, uint32_t* guest_data)
{
	return guest_data == nullptr ? parse_data : guest_data;
}

void CommandProcessor::Run(uint32_t* parse_data, uint32_t num_dw, const uint32_t* submitted_data, CommandStreamKind stream_kind,
	                       uint32_t* guest_data)
{
	// Include admission and the final producer submission, not only the packet
	// body between them. Separate handoff spans expose those nested costs.
	Lumen::CriticalPath::Scope critical_run(Lumen::CriticalPath::Reason::GpuCommandRun, static_cast<uint64_t>(m_queue + 1));
	GpuHandoffClaimScope handoff_claim(this, num_dw != 0);
	if (GpuMemoryCpuHandoffEnabled() && num_dw != 0)
		EXIT_IF(!GraphicsRenderPrepareCpuMemoryAccess(reinterpret_cast<uint64_t>(parse_data), uint64_t(num_dw) * 4,
		    GuestCpu::MemoryAccessIntent::Read));
	LUMEN_PROFILER_BLOCK("CommandProcessor::Run");
	CommandProcessorRunStateScope parser_state_scope(this);
	CommandProcessorStreamStateScope stream_state_scope(this, parse_data, submitted_data, num_dw, stream_kind,
	                                                    guest_data != nullptr && parse_data != guest_data);
	thread_local uint64_t diagnostic_op_us[256]    = {};
	thread_local uint64_t diagnostic_op_count[256] = {};
	thread_local uint64_t diagnostic_runs          = 0;
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	const bool diagnostics_enabled = !m_headless_test_mode && Config::DiagnosticsEnabled();
#else
	const bool diagnostics_enabled = Config::DiagnosticsEnabled();
#endif

	if (num_dw > 0)
	{
		Core::LockGuard lock(m_mutex);
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		if (!m_headless_test_mode)
#endif
		{
			EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);
			GraphicsRenderMemoryFree(m_buffer[m_current_buffer], reinterpret_cast<uint64_t>(command_processor_guest_data(parse_data, guest_data)),
			                         static_cast<size_t>(num_dw) * 4);
		}
	}

	auto* cmd = parse_data;
	auto  dw  = num_dw;
	for (;;)
	{
		if (dw == 0)
		{
			break;
		}

		EXIT_NOT_IMPLEMENTED(dw < 2);
		EXIT_NOT_IMPLEMENTED(dw > num_dw);

		auto cmd_id = *cmd++;
		const auto packet_dw = LUMEN_PM4_LEN(cmd_id);
		EXIT_NOT_IMPLEMENTED(packet_dw > dw);
		stream_state_scope.SetPacket(num_dw - dw, packet_dw);

		auto op = (cmd_id >> 8u) & 0xffu;

		auto pfunc = g_cp_op_func[op];

		if (pfunc == nullptr)
		{
			EXIT("unknown op\n\t%05" PRIx32 ":\n\tcmd_id = %08" PRIx32 "\n\tfirst_payload = %08" PRIx32 "\n",
			     num_dw - dw, cmd_id, cmd[0]);
		}

		uint32_t s = 0;
		if (diagnostics_enabled)
		{
			auto start = std::chrono::steady_clock::now();
			s          = pfunc(this, cmd_id, cmd, dw, num_dw);
			diagnostic_op_us[op] += static_cast<uint64_t>(
			    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
			diagnostic_op_count[op]++;
		} else
		{
			s = pfunc(this, cmd_id, cmd, dw, num_dw);
		}
		EXIT_NOT_IMPLEMENTED(s >= dw);

		// printf("\t %05" PRIx32 ": %u\n", num_dw - dw, s);

		cmd += s;
		dw -= s + 1;
	}

	if (diagnostics_enabled && (++diagnostic_runs % 120) == 0)
	{
		for (int rank = 0; rank < 8; ++rank)
		{
			int best = -1;
			for (int op = 0; op < 256; ++op)
			{
				if (diagnostic_op_count[op] != 0 && (best < 0 || diagnostic_op_us[op] > diagnostic_op_us[best]))
				{
					best = op;
				}
			}
			if (best < 0)
			{
				break;
			}
			dbg_printf("[PERF] opcode=0x%02x total=%.3f ms count=%" PRIu64 " avg=%.3f us\n", best,
			           static_cast<double>(diagnostic_op_us[best]) / 1000.0, diagnostic_op_count[best],
			           static_cast<double>(diagnostic_op_us[best]) / static_cast<double>(diagnostic_op_count[best]));
			diagnostic_op_count[best] = 0;
		}
		for (int op = 0; op < 256; ++op)
		{
			diagnostic_op_us[op]    = 0;
			diagnostic_op_count[op] = 0;
		}
	}
}

void CommandProcessor::SetIndexType(uint32_t index_type_and_size)
{
	Core::LockGuard lock(m_mutex);

	m_index_type_and_size = index_type_and_size;
}

void CommandProcessor::SetIndexBufferSize(uint32_t index_count)
{
	Core::LockGuard lock(m_mutex);

	m_index_buffer_size = {index_count, true};
}

CommandProcessor::IndexBufferSizeState CommandProcessor::GetIndexBufferSizeState()
{
	Core::LockGuard lock(m_mutex);

	return m_index_buffer_size;
}

void CommandProcessor::SetNumInstances(uint32_t num_instances)
{
	Core::LockGuard lock(m_mutex);

	if (num_instances == 0)
	{
		num_instances = 1;
	}

	m_num_instances = num_instances;

	EXIT_NOT_IMPLEMENTED(m_num_instances != 1);
}

void CommandProcessor::PrepareVertexSource(ShaderVertexSourceSnapshot* source)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && defined(LUMEN_FEXCORE_RUNTIME)
	EXIT_IF(source == nullptr);
	if (m_sh_ctx.GetVs().vs_embedded) return;
	const uint64_t address = m_sh_ctx.GetVs().vs_regs.data_addr;
	EopInitialTargetIdentity identity;
	const uint32_t index_type_and_size = m_index_type_and_size;
	const uint32_t num_instances = m_num_instances;
	const auto index_buffer_size = m_index_buffer_size;
	std::array<uint8_t, sizeof(HW::Context)> context_bytes;
	std::array<uint8_t, sizeof(HW::UserConfig)> user_bytes;
	std::array<uint8_t, sizeof(HW::Shader)> shader_bytes;
	bool saved = false;
	const auto qualified = [&]() -> const char* {
		const auto current = GetEopInitialTargetIdentity(false);
		if (const char* reason = ShaderSourceRetryContextRejection(
		    Core::VirtualMemory::CurrentThreadHasMemoryLease(), m_run_lock_held.load(std::memory_order_acquire),
		    g_command_processor_parser_state != nullptr && g_command_processor_parser_state->owner == this,
		    GraphicsRecordingIdentityValid(current, this, current.buffer))) return reason;
		if (!saved)
		{
			identity = current;
			std::memcpy(context_bytes.data(), reinterpret_cast<const uint8_t*>(&m_ctx), sizeof(m_ctx));
			std::memcpy(user_bytes.data(), reinterpret_cast<const uint8_t*>(&m_ucfg), sizeof(m_ucfg));
			std::memcpy(shader_bytes.data(), reinterpret_cast<const uint8_t*>(&m_sh_ctx), sizeof(m_sh_ctx));
			saved = true;
		}
		if (!identity.Matches(current)) return "source-recording-changed";
		if (index_type_and_size != m_index_type_and_size || num_instances != m_num_instances ||
		    index_buffer_size.index_count != m_index_buffer_size.index_count ||
		    index_buffer_size.configured != m_index_buffer_size.configured) return "source-index-state-changed";
		if (std::memcmp(context_bytes.data(), reinterpret_cast<const uint8_t*>(&m_ctx), sizeof(m_ctx)) != 0 ||
		    std::memcmp(user_bytes.data(), reinterpret_cast<const uint8_t*>(&m_ucfg), sizeof(m_ucfg)) != 0 ||
		    std::memcmp(shader_bytes.data(), reinterpret_cast<const uint8_t*>(&m_sh_ctx), sizeof(m_sh_ctx)) != 0)
			return "source-draw-state-changed";
		return nullptr;
	};
	const auto result = CaptureShaderSourceWithRetry(
	    [&](bool* retryable) { return source->Capture(address, retryable); }, qualified, [&] {
		    // This is before GraphicsRender, framebuffer lookup, command emission
		    // or resource effects. Only CP1 is held; RunLock keeps this batch live.
		    // CCB activity may proceed, but any changed draw state/recording is
		    // rejected after the pause instead of consuming a stale snapshot.
		    struct Pause
		    {
			    Core::Mutex& mutex;
			    explicit Pause(Core::Mutex& value): mutex(value) { mutex.Unlock(); }
			    ~Pause() { mutex.Lock(); }
		    } pause(m_mutex);
		    Core::Thread::SleepMicro(1000);
	    }, qualified);
	if (!result.captured)
		EXIT("Vertex source preflight refused: %s attempts=%u elapsed_ms=%" PRIu64 "\n",
		     result.reason, result.attempts, result.elapsed_ms);
#else
	(void)source;
#endif
}

bool CommandProcessor::PrepareOwnedDraw(GraphicsRenderOwnedPreparation* preparation, uint32_t consumed_packet_dw)
{
	if (!gpu_owned_preparation_enabled()) return false;
	struct Counts
	{
		uint64_t attempts = 0, outside = 0, validated = 0, fallback = 0;
		uint64_t preconditions = 0, capture_failed = 0, release_failed = 0, after_release_invalid = 0;
	};
	static thread_local Counts counts;
	struct Receipt
	{
		enum class Outcome { Preconditions, Capture, Release, Revalidate, Validated } outcome = Outcome::Preconditions;
		Counts& counts;
		const CommandProcessor* processor;
		Receipt(Counts& value, const CommandProcessor* cp): counts(value), processor(cp) { ++counts.attempts; }
		~Receipt()
		{
			if (outcome == Outcome::Validated) ++counts.validated;
			else
			{
				++counts.fallback;
				switch (outcome)
				{
					case Outcome::Preconditions: ++counts.preconditions; break;
					case Outcome::Capture: ++counts.capture_failed; break;
					case Outcome::Release: ++counts.release_failed; break;
					case Outcome::Revalidate: ++counts.after_release_invalid; break;
					case Outcome::Validated: break;
				}
			}
			if ((counts.attempts & (counts.attempts - 1)) == 0)
				std::fprintf(stderr, "[GPU_OWNED_PREPARATION_COUNTS] cp=%p attempts=%" PRIu64 " outside=%" PRIu64
				    " validated=%" PRIu64 " fallback=%" PRIu64 " preconditions=%" PRIu64 " capture_failed=%" PRIu64
				    " release_failed=%" PRIu64 " after_release_invalid=%" PRIu64 " per-thread=1 powers-of-two=1\n",
				    static_cast<const void*>(processor), counts.attempts, counts.outside, counts.validated, counts.fallback,
				    counts.preconditions, counts.capture_failed, counts.release_failed, counts.after_release_invalid);
		}
	} receipt(counts, this);
	if (preparation == nullptr || consumed_packet_dw == 0 ||
	    g_gpu_handoff_claim == nullptr || !g_gpu_handoff_claim->OwnsEpochFor(this) ||
	    Core::VirtualMemory::CurrentThreadHasMemoryLease() || g_retained_processor_recording != nullptr ||
	    !m_run_lock_held.load(std::memory_order_acquire) || g_command_processor_parser_state == nullptr ||
	    g_command_processor_parser_state->owner != this) return false;

	// Only the draw parser calls this entry, before DrawIndex/Auto takes CP.
	// Custom and indirect dispatch also enter without a CP or renderer lock.
	// No recursive lock is retained while the memory epoch is readmitted.
	GpuParserPauseProof parser;
	EopInitialTargetIdentity original;
	std::array<uint8_t, sizeof(HW::UserConfig)> user_bytes;
	uint32_t index_type = 0, instances = 0;
	IndexBufferSizeState index_size;
	receipt.outcome = Receipt::Outcome::Capture;
	{
		Core::LockGuard lock(m_mutex);
		original = GetEopInitialTargetIdentity(false);
		if (!GraphicsRecordingIdentityValid(original, this, original.buffer) ||
		    !m_buffer[m_current_buffer]->CanPauseOwnedPreparation() || !parser.Capture(this, consumed_packet_dw) ||
		    !preparation->Capture(&m_ctx, &m_sh_ctx)) return false;
		std::memcpy(user_bytes.data(), reinterpret_cast<const uint8_t*>(&m_ucfg), sizeof(m_ucfg));
		index_type = m_index_type_and_size; instances = m_num_instances; index_size = m_index_buffer_size;
		// Capture may publish older shader producers and rotate the recording.
		// Re-prove the complete prefix immediately before minting release authority.
		const auto current = GetEopInitialTargetIdentity(false);
		if (!GraphicsRecordingIdentityValid(current, this, current.buffer) || current.session != original.session ||
		    current.submit_id != original.submit_id || !m_buffer[m_current_buffer]->CanPauseOwnedPreparation() ||
		    Core::VirtualMemory::CurrentThreadHasMemoryLease()) return false;
	}
	receipt.outcome = Receipt::Outcome::Release;
	if (!AnalyzeOutsideGpuEpoch(IndependentlyCompletableGpuPrefix(this), [&] {
		++counts.outside;
		preparation->Analyze();
	})) return false;
	receipt.outcome = Receipt::Outcome::Revalidate;

	// The global epoch has already been reacquired, without any processor lock.
	// Re-publish changed GPU producers before reading any live guest packet.
	// A changed active packet cannot safely resume its already-parsed scalars.
	EXIT_IF(!parser.RefreshGpuProducers() || !parser.Validate());
	Core::LockGuard lock(m_mutex);
	const auto current = GetEopInitialTargetIdentity(false);
	EXIT_IF(!GraphicsRecordingIdentityValid(current, this, current.buffer) || current.session != original.session ||
	    current.processor_queue != original.processor_queue || current.submit_id != original.submit_id ||
	    !m_buffer[m_current_buffer]->CanPauseOwnedPreparation());
	// Prior producers may have been submitted by an independent CPU access.
	// Use the current recording; no saved CommandBuffer pointer is dereferenced.
	if (index_type != m_index_type_and_size || instances != m_num_instances ||
	    index_size.index_count != m_index_buffer_size.index_count || index_size.configured != m_index_buffer_size.configured ||
	    std::memcmp(user_bytes.data(), reinterpret_cast<const uint8_t*>(&m_ucfg), sizeof(m_ucfg)) != 0 ||
	    !preparation->Validate(&m_ctx, &m_sh_ctx)) return false;
	static std::atomic<bool> reported {false};
	if (!reported.exchange(true, std::memory_order_relaxed))
		std::fputs("[GPU_OWNED_PREPARATION] applied=1 analysis-outside-epoch=1 parser-and-sources-revalidated=1 forced-flush=0\n", stderr);
	receipt.outcome = Receipt::Outcome::Validated;
	return true;
}

bool CommandProcessor::DrawWithRangeOwnership(uint32_t index_count, const void* index_addr, uint32_t flags, uint32_t type,
                                             uint32_t consumed_packet_dw, bool indexed)
{
	if (!gpu_range_execution_enabled()) return false;
	struct Counts { uint64_t attempts = 0, completed = 0, rejected = 0, admission_failed = 0; };
	static thread_local Counts counts;
	struct Receipt
	{
		Counts& counts;
		~Receipt()
		{
			if ((counts.attempts & (counts.attempts - 1)) == 0)
				std::fprintf(stderr, "[GPU_RANGE_EXECUTION_COUNTS] attempts=%" PRIu64 " completed=%" PRIu64
				    " rejected=%" PRIu64 " admission_failed=%" PRIu64 " whole-native-draw=1 per-thread=1\n",
				    counts.attempts, counts.completed, counts.rejected, counts.admission_failed);
		}
	} receipt {counts};
	++counts.attempts;
	++counts.rejected;
	auto* claim = g_gpu_handoff_claim;
	if (consumed_packet_dw == 0 || claim == nullptr || !claim->OwnsEpochFor(this) ||
	    Core::VirtualMemory::CurrentThreadHasMemoryLease() || g_retained_processor_recording != nullptr ||
	    !m_run_lock_held.load(std::memory_order_acquire) || g_command_processor_parser_state == nullptr ||
	    g_command_processor_parser_state->owner != this) return false;

	GraphicsRenderRangePreparation preparation;
	GraphicsRenderPreparedInputs prepared;
	GpuParserPauseProof parser;
	EopInitialTargetIdentity original;
	{
		Core::LockGuard lock(m_mutex);
		original = GetEopInitialTargetIdentity(false);
		if (!GraphicsRecordingIdentityValid(original, this, original.buffer) ||
		    !m_buffer[m_current_buffer]->CanPauseOwnedPreparation() || !parser.Capture(this, consumed_packet_dw)) return false;
		EXIT_IF(!parser.RefreshGpuProducers() || !parser.Validate());
		std::vector<GpuMemoryClaimRange> parser_ranges;
		parser.AppendClaimRanges(&parser_ranges);
		if (!preparation.Capture(&m_ctx, &m_sh_ctx, indexed ? reinterpret_cast<uint64_t>(index_addr) : 0,
		        indexed ? uint64_t(index_count) * (m_index_type_and_size == 0 ? 2u : 4u) : 0, &prepared, parser_ranges)) return false;
		// Resource preflight may publish a previous GPU producer into PM4.
		// Never emit effects using scalars parsed before such a publication.
		EXIT_IF(!parser.Validate());
		const auto current = GetEopInitialTargetIdentity(false);
		if (!GraphicsRecordingIdentityValid(current, this, current.buffer) || current.session != original.session ||
		    current.submit_id != original.submit_id || !m_buffer[m_current_buffer]->CanPauseOwnedPreparation() ||
		    Core::VirtualMemory::CurrentThreadHasMemoryLease()) return false;
	}
	--counts.rejected;
	if (!claim->BeginRangePhase(preparation.Owner()))
	{
		++counts.admission_failed;
		if (claim->OwnsNativePhase())
		{
			claim->EndRangePhase(&preparation);
			EXIT_IF(!parser.RefreshGpuProducers() || !parser.Validate());
		}
		return false;
	}
	{
		// CPU publication takes renderer -> producer CP. Use the same order
		// now that CPU callbacks can run; the draw's internal renderer lock is
		// recursive. Taking CP first would deadlock on an unrelated old output.
		NativeGpuRecordingLocks locks(this);
		EXIT_IF(claim->OwnsEpoch() || !claim->OwnsRecordingFor(this));
		ScopedShaderVertexSource vertex_scope(preparation.VertexSource());
		RetainedProcessorRecordingScope recording_scope(this, m_buffer[m_current_buffer], m_retained_recording_unlock_epoch);
		if (indexed)
			GraphicsRenderDrawIndex(m_sumbit_id, m_buffer[m_current_buffer], &m_ctx, &m_ucfg, &m_sh_ctx,
			    m_index_type_and_size, index_count, index_addr, flags, type, &prepared);
		else
			GraphicsRenderDrawIndexAuto(m_sumbit_id, m_buffer[m_current_buffer], &m_ctx, &m_ucfg, &m_sh_ctx,
			    index_count, flags, &prepared);
	}
	// The draw has enrolled its ordinary producer guards. A CPU dependency can
	// complete this recorded prefix independently after the temporary claim ends.
	claim->EndRangePhase(&preparation);
	EXIT_IF(!parser.RefreshGpuProducers() || !parser.Validate());
	{
		Core::LockGuard lock(m_mutex);
		const auto current = GetEopInitialTargetIdentity(false);
		EXIT_IF(!GraphicsRecordingIdentityValid(current, this, current.buffer) || current.session != original.session ||
		    current.processor_queue != original.processor_queue || current.submit_id != original.submit_id);
	}
	++counts.completed;
	return true;
}

void CommandProcessor::DrawIndex(uint32_t index_count, const void* index_addr, uint32_t flags, uint32_t type,
                                 uint32_t consumed_packet_dw)
{
	if (DrawWithRangeOwnership(index_count, index_addr, flags, type, consumed_packet_dw, true)) return;
	std::optional<GraphicsRenderOwnedPreparation> owned;
	if (gpu_owned_preparation_enabled()) owned.emplace();
	const bool owned_ready = owned.has_value() && PrepareOwnedDraw(&*owned, consumed_packet_dw);
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && defined(LUMEN_FEXCORE_RUNTIME)
	ShaderVertexSourceSnapshot vertex_source;
	if (!owned_ready) PrepareVertexSource(&vertex_source);
	ScopedShaderVertexSource vertex_scope(owned_ready ? owned->VertexSource() : vertex_source.Data() != nullptr ? &vertex_source : nullptr);
#endif
	GraphicsRenderPreparedInputs prepared;
	GraphicsRenderPrepareResourceBacking(&m_ctx, &m_sh_ctx, false, reinterpret_cast<uint64_t>(index_addr),
	    uint64_t(index_count) * (m_index_type_and_size == 0 ? 2u : 4u), &prepared, owned_ready ? &*owned : nullptr);
	RetainedProcessorRecordingScope recording_scope(this, m_buffer[m_current_buffer], m_retained_recording_unlock_epoch);

	GraphicsRenderDrawIndex(m_sumbit_id, m_buffer[m_current_buffer], &m_ctx, &m_ucfg, &m_sh_ctx, m_index_type_and_size, index_count,
	                        index_addr, flags, type, &prepared);
}

void CommandProcessor::DispatchDirect(uint32_t thread_group_x, uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);
	GraphicsRenderPreparedInputs prepared;
	GraphicsRenderPrepareResourceBacking(&m_ctx, &m_sh_ctx, true, 0, 0, &prepared);
	RetainedProcessorRecordingScope recording_scope(this, m_buffer[m_current_buffer], m_retained_recording_unlock_epoch);

	GraphicsRenderDispatchDirect(m_sumbit_id, m_buffer[m_current_buffer], &m_ctx, &m_sh_ctx, thread_group_x, thread_group_y, thread_group_z,
	                             mode, &prepared);
}

void CommandProcessor::DrawIndexAuto(uint32_t index_count, uint32_t flags, uint32_t consumed_packet_dw)
{
	if (DrawWithRangeOwnership(index_count, nullptr, flags, 0, consumed_packet_dw, false)) return;
	std::optional<GraphicsRenderOwnedPreparation> owned;
	if (gpu_owned_preparation_enabled()) owned.emplace();
	const bool owned_ready = owned.has_value() && PrepareOwnedDraw(&*owned, consumed_packet_dw);
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && defined(LUMEN_FEXCORE_RUNTIME)
	ShaderVertexSourceSnapshot vertex_source;
	if (!owned_ready) PrepareVertexSource(&vertex_source);
	ScopedShaderVertexSource vertex_scope(owned_ready ? owned->VertexSource() : vertex_source.Data() != nullptr ? &vertex_source : nullptr);
#endif
	GraphicsRenderPreparedInputs prepared;
	GraphicsRenderPrepareResourceBacking(&m_ctx, &m_sh_ctx, false, 0, 0, &prepared, owned_ready ? &*owned : nullptr);
	RetainedProcessorRecordingScope recording_scope(this, m_buffer[m_current_buffer], m_retained_recording_unlock_epoch);

	GraphicsRenderDrawIndexAuto(m_sumbit_id, m_buffer[m_current_buffer], &m_ctx, &m_ucfg, &m_sh_ctx, index_count, flags, &prepared);
}

void CommandProcessor::ClearGds(uint64_t dw_offset, uint32_t dw_num, uint32_t clear_value)
{
	Core::LockGuard lock(m_mutex);

	GraphicsRenderClearGds(dw_offset, dw_num, clear_value);
}

void CommandProcessor::ReadGds(uint64_t destination_address, uint64_t source_byte_offset, uint32_t byte_count)
{
	if (byte_count != 0) EXIT_IF(!GraphicsRenderPrepareCpuMemoryAccess(destination_address, byte_count, GuestCpu::MemoryAccessIntent::Write));
	Core::LockGuard lock(m_mutex);
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	if (record_command_effect(this, CommandEffectProbeValue::ReadGds))
	{
		g_command_effect_probe->destination32 = reinterpret_cast<uint32_t*>(destination_address);
		g_command_effect_probe->source_byte_offset = source_byte_offset;
		g_command_effect_probe->byte_count = byte_count;
		return;
	}
#endif
	EXIT_NOT_IMPLEMENTED(!GraphicsRenderReadGds(destination_address, source_byte_offset, byte_count));
}

void CommandProcessor::WaitFlipDone(uint32_t video_out_handle, uint32_t display_buffer_index)
{
	BufferFlush();
	GpuHandoffWaitScope handoff_wait;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (Loader::GuestExecutionStopRequested())
	{
		return;
	}
#endif
	VideoOut::VideoOutWaitFlipDone(static_cast<int>(video_out_handle), static_cast<int>(display_buffer_index));
}

void CommandProcessor::WriteAtEndOfPixelShader32(uint32_t* destination, uint32_t value)
{
	Core::LockGuard lock(m_mutex);
	EXIT_IF(m_interrupt_event_id != 64);
	EXIT_IF(destination == nullptr || (reinterpret_cast<uint64_t>(destination) & 3u) != 0);
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	if (record_command_effect(this, CommandEffectProbeValue::QueuePixelDoneWrite32))
	{
		g_command_effect_probe->destination32 = destination;
		g_command_effect_probe->value32 = value;
		return;
	}
#endif
	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);
	if (Config::GetPrintfDirection() != Log::Direction::Silent)
		printf("CommandProcessor::WriteAtEndOfPixelShader32(): queued completion write\n");
	// The label publishes only after GPU completion, without cache writeback,
	// GDS reads or an interrupt. No guest write occurs during packet parsing.
	const auto capture = CaptureScalarLabelDestination(reinterpret_cast<uint64_t>(destination), 4, value);
	GraphicsRenderWriteAtEndOfPipe32(m_sumbit_id, m_buffer[m_current_buffer], destination, value,
	    capture.IsCaptured() ? &capture : nullptr);
}

void CommandProcessor::WriteAtEndOfPipe32(uint32_t cache_policy, uint32_t event_write_dest, uint32_t eop_event_type, uint32_t cache_action,
                                          uint32_t event_index, uint32_t event_write_source, void* dst_gpu_addr, uint32_t value,
                                          uint32_t interrupt_selector)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);

	if (Config::GetPrintfDirection() != Log::Direction::Silent)
	{
		printf("CommandProcessor::WriteAtEndOfPipe32()\n");
		printf("\t cache_policy        = 0x%08" PRIx32 "\n", cache_policy);
		printf("\t event_write_dest    = 0x%08" PRIx32 "\n", event_write_dest);
		printf("\t eop_event_type      = 0x%08" PRIx32 "\n", eop_event_type);
		printf("\t cache_action        = 0x%08" PRIx32 "\n", cache_action);
		printf("\t event_index         = 0x%08" PRIx32 "\n", event_index);
		printf("\t event_write_source  = 0x%08" PRIx32 "\n", event_write_source);
		printf("\t interrupt_selector  = 0x%08" PRIx32 "\n", interrupt_selector);
		printf("\t dst_gpu_addr        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(dst_gpu_addr));
		printf("\t value               = 0x%08" PRIx32 "\n", value);
	}

	EXIT_NOT_IMPLEMENTED(cache_policy != 0x00000000);
	EXIT_NOT_IMPLEMENTED(event_write_dest != 0x00000000);
	EXIT_NOT_IMPLEMENTED(interrupt_selector != 0x0);

	if (event_write_source == 0x00000002 && eop_event_type == 0x0000002f && cache_action == 0x00000000 && event_index == 0x00000006)
	{
		const auto capture = CaptureScalarLabelDestination(reinterpret_cast<uint64_t>(dst_gpu_addr), 4, value);
		GraphicsRenderWriteAtEndOfPipe32(m_sumbit_id, m_buffer[m_current_buffer], static_cast<uint32_t*>(dst_gpu_addr), value,
		    capture.IsCaptured() ? &capture : nullptr);
	} else if (event_write_source == 0x00000001 && eop_event_type == 0x0000002f && cache_action == 0x00000000 && event_index == 0x00000006)
	{
		GraphicsRenderWriteAtEndOfPipeGds32(m_sumbit_id, m_buffer[m_current_buffer], static_cast<uint32_t*>(dst_gpu_addr), value & 0xffffu,
		                                    value >> 16u);
	} else
	{
		EXIT("unknown event type\n");
	}
}

template <typename Capture, typename Revalidate, typename Backoff, typename LeaseHeld>
static EopInitialTargetCaptureResult EopCaptureInitialTargetWithLeaseRetry(CommandProcessor* cp, uint64_t original_generation,
    Capture capture, Revalidate revalidate, Backoff backoff, LeaseHeld lease_held)
{
	EopInitialTargetCaptureResult result;
	result.original_generation = original_generation;
	const auto reject = [&](const char* reason) {
		result.acquisition.reason = reason;
		return result;
	};
	// Capture's default sentinel would create a new ticket. Never pass it to
	// the first or any subsequent borrow in this fixed-token operation.
	if (original_generation == UINT64_MAX) return reject("mapping-generation-unavailable");
	if (const char* reason = revalidate(); reason != nullptr) return reject(reason);
	for (uint32_t round = 1; round <= 16; ++round)
	{
		if (lease_held()) return reject("retry-memory-lease-held");
		result.rounds = round;
		result.acquisition = {};
		if (capture(original_generation, &result.acquisition))
		{
			result.captured = true;
			return result;
		}
		if (result.acquisition.reason == nullptr ||
		    std::strcmp(result.acquisition.reason, "lease-acquisition-retry-exhausted") != 0 ||
		    result.acquisition.attempts != 16 || round == 16) return result;
		if (lease_held()) return reject("retry-memory-lease-held");
		if (const char* reason = revalidate(); reason != nullptr) return reject(reason);
		{
			// Both EOP packet parsers call WriteAtEndOfPipe64 directly. Its
			// guard owns one CP level; Run releases its setup guard before the
			// parser. The ring's RunLock stays held on its owning thread.
			struct ReleaseProcessorLock
			{
				CommandProcessor* cp;
				explicit ReleaseProcessorLock(CommandProcessor* value): cp(value) { cp->Unlock(); }
				~ReleaseProcessorLock() { cp->Lock(); }
			} release(cp);
			backoff();
		}
		// Re-read the current recording before touching a saved buffer. No
		// prefix flush, GPU operation, callback or guest store has occurred.
		if (const char* reason = revalidate(); reason != nullptr) return reject(reason);
	}
	return result;
}

EopInitialTargetIdentity CommandProcessor::GetEopInitialTargetIdentity(bool require_exclusive)
{
	EopInitialTargetIdentity identity;
	identity.processor = this;
	identity.current_index = m_current_buffer;
	identity.processor_queue = m_queue;
	identity.submit_id = m_sumbit_id;
	identity.exclusive = m_eop_checkpoint_exclusive_stream;
	identity.processor_epoch = m_tracked_submission_session_epoch.load(std::memory_order_acquire);
	identity.assigned_serial = m_tracked_submission_assigned_serial.load(std::memory_order_acquire);
	// Only dereference the current pointer read while CP is held, including
	// after every yield. Completed serial may legitimately advance in a worker.
	if (m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM) return identity;
	auto* current = m_buffer[m_current_buffer];
	if (current == nullptr) return identity;
	identity.buffer = current;
	identity.parent = current->GetParent();
	identity.pool = current->GetPool();
	identity.pool_index = current->GetIndex();
	identity.buffer_queue = current->GetQueue();
	identity.recording = current->GetRecordingId();
	identity.session = current->GetSubmissionSessionEpoch();
	identity.serial = current->GetRecordingSubmitSerial();
	identity.ordinal = current->GetTrackedCommandOrdinal();
	identity.submitted_cutoff = current->GetBeginSubmitSerialCutoff();
	identity.completed_cutoff = current->GetBeginCompletedSerialCutoff();
	identity.valid = (!require_exclusive || identity.exclusive) && identity.parent == this && identity.pool != nullptr &&
	    identity.pool_index != UINT32_MAX && identity.processor_queue >= 0 &&
	    identity.buffer_queue == identity.processor_queue && identity.recording != 0 && !current->IsExecute() &&
	    identity.session != 0 && identity.session == identity.processor_epoch && identity.serial != 0 &&
	    identity.serial == identity.assigned_serial && identity.submitted_cutoff < identity.serial &&
	    identity.completed_cutoff <= identity.submitted_cutoff &&
	    m_tracked_submission_timeline_valid.load(std::memory_order_acquire);
	return identity;
}

EopInitialTargetCaptureResult CommandProcessor::CaptureEopInitialTarget(GuestWriteDestination* destination,
    uint64_t address, uint64_t bytes, uint64_t alignment)
{
	const uint64_t original_generation = Core::VirtualMemory::GetMutationGeneration();
	const auto identity = GetEopInitialTargetIdentity();
	return EopCaptureInitialTargetWithLeaseRetry(this, original_generation,
	    [destination, address, bytes, alignment](uint64_t original, GraphicsGuestLeaseResult* acquisition) {
		    return destination->Capture(address, bytes, alignment, acquisition, original);
	    }, [&]() -> const char* {
		    if (!m_eop_checkpoint_exclusive_stream) return "concurrent-command-stream-during-target-capture";
		    return identity.Matches(GetEopInitialTargetIdentity()) ? nullptr : "recording-changed-during-target-capture";
	    }, [] { Core::Thread::SleepMicro(1000); }, [] {
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		    return Core::VirtualMemory::CurrentThreadHasMemoryLease();
#else
		    return false;
#endif
	    });
}

LabelScalarCaptureTicket CommandProcessor::CaptureScalarLabelDestination(uint64_t address, uint64_t bytes, uint64_t value)
{
	LabelScalarCaptureTicket ticket;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	const auto identity = GetEopInitialTargetIdentity();
	const auto qualified = [&](const EopInitialTargetIdentity& current) {
		return ScalarLabelCaptureScopeQualified(current, this, m_run_lock_held.load(std::memory_order_acquire),
		    g_command_processor_parser_state != nullptr && g_command_processor_parser_state->owner == this);
	};
	// Generic renderer calls and a stream outside the ring's exclusive parser
	// retain the existing no-pause capture in LabelSet.
	if (!qualified(identity)) return ticket;
	GuestWriteDestination destination;
	const uint64_t original_generation = Core::VirtualMemory::GetMutationGeneration();
	const auto captured = EopCaptureInitialTargetWithLeaseRetry(this, original_generation,
	    [&](uint64_t original, GraphicsGuestLeaseResult* acquisition) {
		    return destination.Capture(address, bytes, bytes, acquisition, original);
	    }, [&]() -> const char* {
		    const auto current = GetEopInitialTargetIdentity();
		    return qualified(current) && identity.Matches(current) ? nullptr : "scalar-label-recording-changed";
	    }, [] { Core::Thread::SleepMicro(1000); }, [] { return Core::VirtualMemory::CurrentThreadHasMemoryLease(); });
	if (!captured.captured || captured.rounds > 1)
		std::fprintf(stderr, "[GFX_LABEL_SCALAR_INITIAL_CAPTURE] bytes=%" PRIu64 " rounds=%u local_attempts=%u captured=%u"
		    " reason=%s original_generation=%" PRIu64 " enrollment=0 gpu_operations=0 callbacks=0 guest_stores=0\n",
		    bytes, captured.rounds, captured.acquisition.attempts, captured.captured ? 1u : 0u,
		    captured.acquisition.reason != nullptr ? captured.acquisition.reason : "none", original_generation);
	if (!captured.captured) EXIT("Scalar Label initial destination rejected before enrollment\n");
	ticket.m_address = destination.Address(); ticket.m_size = destination.Size();
	ticket.m_generation = destination.Generation(); ticket.m_value = bytes == 4 ? static_cast<uint32_t>(value) : value;
	ticket.m_recording = ScalarLabelRecordingIdentity(identity);
#else
	(void)address; (void)bytes; (void)value;
#endif
	return ticket;
}

LabelScalarRecordingIdentity CommandProcessor::ScalarLabelIdentity(CommandBuffer* buffer)
{
	const auto identity = GetEopInitialTargetIdentity();
	if (buffer == nullptr || identity.buffer != buffer ||
	    !ScalarLabelCaptureScopeQualified(identity, this, m_run_lock_held.load(std::memory_order_acquire),
	        g_command_processor_parser_state != nullptr && g_command_processor_parser_state->owner == this)) return {};
	return ScalarLabelRecordingIdentity(identity);
}

bool CommandProcessor::RetainedRecordingValid(CommandBuffer* buffer)
{
	// TLS qualification comes before reading processor or buffer state. A
	// foreign thread or arbitrary caller cannot manufacture lock ownership.
	const auto* witness = g_retained_processor_recording;
	if (witness == nullptr || witness->revoked || witness->depth != 1 || witness->processor != this || witness->buffer != buffer) return false;
	if (!RetainedProcessorScopeQualified(this, buffer, m_retained_recording_unlock_epoch,
	        m_run_lock_held.load(std::memory_order_acquire),
	        g_command_processor_parser_state != nullptr && g_command_processor_parser_state->owner == this)) return false;
	// Pointer equality is checked before the supplied buffer is dereferenced.
	if (GetCurrentBufferForCpuCopy() != buffer) return false;
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	// Headless fixtures provide only Vulkan recording metadata. They still
	// exercise this public path's real TLS, CP slot, run lock and parser gates.
	if (m_headless_test_mode && m_retained_identity_for_test != nullptr)
		return GraphicsRecordingIdentityValid(*m_retained_identity_for_test, this, buffer);
#endif
	return GraphicsRecordingIdentityValid(GetEopInitialTargetIdentity(false), this, buffer);
}

LabelScalarRecordingIdentity CommandProcessor::DeferredLabelIdentity(CommandBuffer* buffer)
{
	const auto identity = GetEopInitialTargetIdentity(false);
	if (buffer == nullptr || identity.buffer != buffer ||
	    !DeferredLabelCaptureScopeQualified(identity, this, m_run_lock_held.load(std::memory_order_acquire),
	        g_command_processor_parser_state != nullptr && g_command_processor_parser_state->owner == this)) return {};
	return ScalarLabelRecordingIdentity(identity);
}

void CommandProcessor::WriteAtEndOfPipe64(uint32_t cache_policy, uint32_t event_write_dest, uint32_t eop_event_type, uint32_t cache_action,
                                          uint32_t event_index, uint32_t event_write_source, void* dst_gpu_addr, uint64_t value,
                                          uint32_t legacy_interrupt_selector, uint32_t control_selector_byte, uint32_t packet_id,
                                          uint32_t event_control_word, uint32_t address_control_word)
{
	Core::LockGuard lock(m_mutex);

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	if (!command_effect_probe_active(this))
#endif
	{
		EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);
	}

	if (Config::GetPrintfDirection() != Log::Direction::Silent)
	{
		printf("CommandProcessor::WriteAtEndOfPipe64()\n");
		printf("\t cache_policy        = 0x%08" PRIx32 "\n", cache_policy);
		printf("\t event_write_dest    = 0x%08" PRIx32 "\n", event_write_dest);
		printf("\t eop_event_type      = 0x%08" PRIx32 "\n", eop_event_type);
		printf("\t cache_action        = 0x%08" PRIx32 "\n", cache_action);
		printf("\t event_index         = 0x%08" PRIx32 "\n", event_index);
		printf("\t event_write_source  = 0x%08" PRIx32 "\n", event_write_source);
		printf("\t interrupt_selector  = 0x%08" PRIx32 "\n", legacy_interrupt_selector);
		printf("\t dst_gpu_addr        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(dst_gpu_addr));
		printf("\t value               = 0x%016" PRIx64 "\n", value);
	}

	EXIT_NOT_IMPLEMENTED(cache_policy != 0x00000000);
	EXIT_NOT_IMPLEMENTED(event_write_dest != 0x00000000);

	const Pm4EventWriteEopPacket packet = {
	    packet_id,          cache_policy,       event_write_dest,      eop_event_type, cache_action,
	    event_index,        control_selector_byte & 0x3u, event_write_source, control_selector_byte,
	    event_control_word, address_control_word,
	};
	if (pm4_event_write_eop_completion_profile(packet) == Pm4EventWriteEopCompletionProfile::InterruptOnly)
	{
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		if (record_command_effect(this, CommandEffectProbeValue::QueueEndOfPipeInterrupt))
		{
			g_command_effect_probe->interrupt_event_id = m_interrupt_event_id;
			return;
		}
#endif
		GraphicsRenderQueueEndOfPipeInterrupt(m_buffer[m_current_buffer], m_interrupt_event_id);
		return;
	}

	const auto release_profile = pm4_event_write_eop_completion_profile(packet);
	if (release_profile == Pm4EventWriteEopCompletionProfile::ReleaseMemWriteBack32Interrupt ||
	    release_profile == Pm4EventWriteEopCompletionProfile::ReleaseMemWriteBackClock64Interrupt)
	{
		// The timestamp event itself requests flush/invalidate even with cache_action=0.
		// Order GPU memory first, then read it back before the signal and IRQ.
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		if (!record_command_effect(this, CommandEffectProbeValue::MemoryBarrier))
#endif
			GraphicsRenderMemoryBarrier(m_buffer[m_current_buffer]);
		if (release_profile == Pm4EventWriteEopCompletionProfile::ReleaseMemWriteBackClock64Interrupt)
		{
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
			if (record_command_effect(this, CommandEffectProbeValue::WriteBackClock64Interrupt))
			{
				g_command_effect_probe->interrupt_event_id = m_interrupt_event_id;
				g_command_effect_probe->destination64 = static_cast<uint64_t*>(dst_gpu_addr);
				return;
			}
#endif
			GraphicsRenderWriteAtEndOfPipeClockCounterWithInterruptWriteBack(m_sumbit_id, m_buffer[m_current_buffer],
			    static_cast<uint64_t*>(dst_gpu_addr), m_interrupt_event_id);
			return;
		}
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		if (record_command_effect(this, CommandEffectProbeValue::WriteBack32Interrupt))
		{
			g_command_effect_probe->interrupt_event_id = m_interrupt_event_id;
			g_command_effect_probe->destination32 = static_cast<uint32_t*>(dst_gpu_addr);
			g_command_effect_probe->value32 = static_cast<uint32_t>(value);
			return;
		}
#endif
		GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBack32(m_sumbit_id, m_buffer[m_current_buffer],
		    static_cast<uint32_t*>(dst_gpu_addr), static_cast<uint32_t>(value), m_interrupt_event_id);
		return;
	}

	bool virtual_clock_enabled = experimental_eop_virtual_clock_enabled();
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	if (command_effect_probe_active(this)) virtual_clock_enabled = g_command_effect_probe->virtual_clock_enabled;
#endif
	if (virtual_clock_enabled && pm4_event_write_eop_virtual_clock_profile(packet,
	        reinterpret_cast<uint64_t>(dst_gpu_addr), m_interrupt_event_id == 64))
	{
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		if (record_command_effect(this, CommandEffectProbeValue::EopVirtualClock))
		{
			g_command_effect_probe->destination64 = static_cast<uint64_t*>(dst_gpu_addr);
			return;
		}
#endif
		GuestWriteDestination destination;
		// DATA4 ignores packet payload and publishes Lumen's virtual clock.
		// Capture its natural 64-bit target before any prefix flush or callback wait.
		const auto captured = CaptureEopInitialTarget(&destination, reinterpret_cast<uint64_t>(dst_gpu_addr), sizeof(uint64_t), 8);
		if (!captured.captured)
		{
			std::fprintf(stderr, "[GFX_EOP_VIRTUAL_CLOCK] reason=%s phase=before-checkpoint published=0"
			                     " capture_rounds=%u local_attempts=%u original_generation=%" PRIu64 "\n",
			    captured.acquisition.reason != nullptr ? captured.acquisition.reason : "target-capture-refused",
			    captured.rounds, captured.acquisition.attempts, captured.original_generation);
			EXIT("virtual EOP clock target is not qualified\n");
		}
		const auto result = TryEopHostCheckpoint(&destination);
		if (result.status != GpuMemoryEopCheckpointStatus::Published)
			EXIT("virtual EOP clock rejected: %s\n", result.reason);
		return;
	}

	bool immediate32_enabled = experimental_eop_immediate32_checkpoint_enabled();
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	if (command_effect_probe_active(this)) immediate32_enabled = g_command_effect_probe->immediate32_checkpoint_enabled;
#endif
	if (immediate32_enabled && pm4_event_write_eop_immediate32_checkpoint_profile(packet,
	        reinterpret_cast<uint64_t>(dst_gpu_addr), m_interrupt_event_id == 64))
	{
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
		if (record_command_effect(this, CommandEffectProbeValue::EopImmediate32Checkpoint))
		{
			g_command_effect_probe->destination32 = static_cast<uint32_t*>(dst_gpu_addr);
			g_command_effect_probe->value32 = static_cast<uint32_t>(value);
			return;
		}
#endif
		GuestWriteDestination destination;
		// Preserve the low payload word before any flush/wait. The high payload
		// word is ignored by DATA1; exactly four destination bytes are published.
		const uint32_t immediate = static_cast<uint32_t>(value);
		const auto captured = CaptureEopInitialTarget(&destination, reinterpret_cast<uint64_t>(dst_gpu_addr), sizeof(uint32_t), 4);
		if (!captured.captured)
		{
			std::fprintf(stderr, "[GFX_EOP_IMMEDIATE32_CHECKPOINT] reason=%s phase=before-checkpoint published=0 irq=0"
			                     " capture_rounds=%u local_attempts=%u original_generation=%" PRIu64 "\n",
			    captured.acquisition.reason != nullptr ? captured.acquisition.reason : "target-capture-refused",
			    captured.rounds, captured.acquisition.attempts, captured.original_generation);
			EXIT("EOP immediate32 checkpoint target is not qualified\n");
		}
		const auto result = TryEopHostCheckpoint(&destination, GpuMemoryEopPublicationKind::Immediate32, immediate);
		// Record each admitted packet profile once, after its real checkpoint.
		// This diagnostic does not expose the destination or change admission.
		static const bool trace_profile = [] {
			const char* value = std::getenv("LUMEN_GFX_EOP_RESOURCE_TRACE");
			return value != nullptr && std::strcmp(value, "1") == 0;
		}();
		static std::atomic<uint32_t> traced_profiles {0};
		const uint32_t profile_bit = eop_event_type == 4 ? 1u : 2u;
		if (trace_profile && (traced_profiles.fetch_or(profile_bit, std::memory_order_relaxed) & profile_bit) == 0)
			std::fprintf(stderr, "[GFX_EOP_IMMEDIATE32_PROFILE] event=%08x type=0x%02x published=%u bytes=4 irq=0 native_cache_claim=0\n",
			             event_control_word, eop_event_type, result.status == GpuMemoryEopCheckpointStatus::Published ? 1u : 0u);
		if (result.status != GpuMemoryEopCheckpointStatus::Published)
			EXIT("EOP immediate32 checkpoint rejected: %s\n", result.reason);
		return;
	}

	bool with_interrupt = false;
	bool source64       = (event_write_source == 0x02);
	bool source32       = (event_write_source == 0x01);
	bool source_counter = (event_write_source == 0x04);

	switch (legacy_interrupt_selector)
	{
		case 0x00:
		case 0x03: with_interrupt = false; break;
		case 0x02: with_interrupt = true; break;
		default:
			EXIT("unknown interrupt_selector: 0x%08" PRIx32
			     " (packet=0x%08" PRIx32 ", event_type=0x%08" PRIx32 ", event_index=0x%08" PRIx32
			     ", data_sel=0x%08" PRIx32 ", control_byte=0x%02" PRIx32 ", cache_action=0x%08" PRIx32 ")\n",
			     legacy_interrupt_selector, packet_id, eop_event_type, event_index, event_write_source, control_selector_byte, cache_action);
	}

	if (eop_event_type == 0x04 && cache_action == 0x00 && event_index == 0x05 && source64 && !with_interrupt)
	{
		const auto capture = CaptureScalarLabelDestination(reinterpret_cast<uint64_t>(dst_gpu_addr), 8, value);
		GraphicsRenderWriteAtEndOfPipe64(m_sumbit_id, m_buffer[m_current_buffer], static_cast<uint64_t*>(dst_gpu_addr), value,
		    capture.IsCaptured() ? &capture : nullptr);
	} else if (eop_event_type == 0x04 && cache_action == 0x00 && event_index == 0x05 && source32 && !with_interrupt)
	{
		const auto capture = CaptureScalarLabelDestination(reinterpret_cast<uint64_t>(dst_gpu_addr), 4, value);
		GraphicsRenderWriteAtEndOfPipe32(m_sumbit_id, m_buffer[m_current_buffer], static_cast<uint32_t*>(dst_gpu_addr), value,
		    capture.IsCaptured() ? &capture : nullptr);
	} else if (((eop_event_type == 0x04 && event_index == 0x05) || (eop_event_type == 0x28 && event_index == 0x05) ||
	            (eop_event_type == 0x2f && event_index == 0x06) || (eop_event_type == 0x14 && event_index == 0x00) ||
	            (eop_event_type == 0x28 && event_index == 0x00)) &&
	           cache_action == 0x38 && source64 && !with_interrupt)
	{
		GraphicsRenderWriteAtEndOfPipeWithWriteBack64(m_sumbit_id, m_buffer[m_current_buffer], static_cast<uint64_t*>(dst_gpu_addr), value);
	} else if (((eop_event_type == 0x04 && event_index == 0x05) || (eop_event_type == 0x28 && event_index == 0x00)) &&
	           cache_action == 0x00 && source_counter && !with_interrupt)
	{
		GraphicsRenderWriteAtEndOfPipeClockCounter(m_sumbit_id, m_buffer[m_current_buffer], static_cast<uint64_t*>(dst_gpu_addr));
	} else if ((eop_event_type == 0x04 && event_index == 0x05) && cache_action == 0x00 && source64 && with_interrupt)
	{
		GraphicsRenderWriteAtEndOfPipeWithInterrupt64(m_sumbit_id, m_buffer[m_current_buffer], static_cast<uint64_t*>(dst_gpu_addr), value, m_interrupt_event_id);
	} else if ((eop_event_type == 0x04 && event_index == 0x05) && cache_action == 0x00 && source32 && with_interrupt)
	{
		GraphicsRenderWriteAtEndOfPipeWithInterrupt32(m_sumbit_id, m_buffer[m_current_buffer], static_cast<uint32_t*>(dst_gpu_addr), value, m_interrupt_event_id);
	} else if ((eop_event_type == 0x04 && event_index == 0x05) && cache_action == 0x3b && source64 && with_interrupt)
	{
		GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBack64(m_sumbit_id, m_buffer[m_current_buffer],
		                                                       static_cast<uint64_t*>(dst_gpu_addr), value, m_interrupt_event_id);
	} else
	{
		// Preserve the rejected controls without exposing the destination's high
		// address bits or payload. Decoded fields alone can hide reserved bits.
		GpuMemoryTraceEopResources(this, packet_id, event_control_word, address_control_word);
		if (virtual_clock_enabled)
			std::fprintf(stderr, "[GFX_EOP_VIRTUAL_CLOCK] reason=packet-profile-excluded phase=before-checkpoint published=0"
			                     " packet_shape=%u payload_ignored=1 target_nonzero=%u target_aligned8=%u graphics_cp=%u\n",
			             pm4_event_write_eop_virtual_clock_profile(packet, 8, true) ? 1u : 0u,
			             dst_gpu_addr != nullptr ? 1u : 0u,
			             reinterpret_cast<uint64_t>(dst_gpu_addr) % 8 == 0 ? 1u : 0u, m_interrupt_event_id == 64 ? 1u : 0u);
		TryEopHostCheckpoint();
		EXIT("unknown EOP event: packet=%08" PRIx32 " event=%08" PRIx32 " address_control=%08" PRIx32
		     " type=%02" PRIx32 " index=%u cache_action=%02" PRIx32 " data_sel=%u int_sel=%u\n",
		     packet_id, event_control_word, address_control_word & 0xffff0000u,
		     eop_event_type, event_index, cache_action, event_write_source, control_selector_byte & 0x3u);
	}
}

// The completed prefix and destination snapshot belong to the caller. This
// helper retries only the whole materialization batch, never preparation or
// publication on its own. Materialize must return with tracker/leases released.
struct EopContinuityProbe
{
	bool unchanged = false;
	bool unavailable = false;
};

static EopContinuityProbe EopContinuityAcquisitionRefusal(const GraphicsGuestLeaseResult& result)
{
	// An opaque failed borrow admits no effect. Only bounded exhaustion may
	// defer this pure preflight; known rights/history/shape failures are final.
	return {false, result.reason != nullptr && std::strcmp(result.reason, "lease-acquisition-retry-exhausted") == 0};
}

struct EopGlobalGenerationProof
{
	EopContinuityProbe operator()(uint64_t original) const { return {Core::VirtualMemory::GetMutationGeneration() == original, false}; }
};

template <typename Materialize, typename Revalidate, typename Backoff, typename Continuity = EopGlobalGenerationProof>
static GpuMemoryEopCheckpointResult EopMaterializeWithLeaseRetry(CommandProcessor* cp, uint64_t memory_generation,
    Materialize materialize, Revalidate revalidate, Backoff backoff, uint32_t* attempts, Continuity continuity = {})
{
	*attempts = 0;
	const auto reject = [](const char* reason) { return GpuMemoryEopCheckpointResult {GpuMemoryEopCheckpointStatus::Rejected, reason}; };
	bool deferred = false;
	for (;;)
	{
		++*attempts;
		// Deferred rounds consume the same cap without restarting the batch or
		// reading bytes while the original range proof remains unavailable.
		const auto result = deferred ? GpuMemoryEopCheckpointResult {GpuMemoryEopCheckpointStatus::Rejected,
		    "continuity-lease-unavailable", true} : materialize();
		if (result.status != GpuMemoryEopCheckpointStatus::Rejected || !result.retryable) return result;
		const auto before = continuity(memory_generation);
		if (memory_generation == UINT64_MAX || (!before.unchanged && !before.unavailable)) return reject("mapping-changed-before-retry");
		if (!GpuMemoryEopCheckpointRetryAllowed(result, *attempts))
		{
			std::fprintf(stderr, "[GFX_EOP_MATERIALIZE_RETRY] exhausted=1 attempts=%u reason=%s published=0\n", *attempts, result.reason);
			return reject("materialization-retry-exhausted");
		}
		if (const char* reason = revalidate(false); reason != nullptr) return reject(reason);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		if (Core::VirtualMemory::CurrentThreadHasMemoryLease()) return reject("retry-memory-lease-held");
#endif
		{
			struct ReleaseProcessorLock
			{
				CommandProcessor* cp;
				explicit ReleaseProcessorLock(CommandProcessor* value): cp(value) { cp->Unlock(); }
				~ReleaseProcessorLock() { cp->Lock(); }
			} release(cp);
			backoff();
		}
		const auto after = continuity(memory_generation);
		if (!after.unchanged && !after.unavailable) return reject("mapping-changed-during-retry");
		deferred = !after.unchanged;
		if (const char* reason = revalidate(true); reason != nullptr) return reject(reason);
	}
}

GpuMemoryEopCheckpointResult CommandProcessor::TryEopHostCheckpoint(const GuestWriteDestination* publication_destination,
    GpuMemoryEopPublicationKind publication_kind, uint32_t immediate32)
{
	static const bool enabled = [] {
		const auto* value = std::getenv("LUMEN_EXPERIMENTAL_GFX_EOP_HOST_CHECKPOINT");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	if (!enabled && publication_destination == nullptr) return {GpuMemoryEopCheckpointStatus::Rejected, "checkpoint-disabled"};
	uint32_t materialize_attempts = 0;
	const auto report = [publication_destination, publication_kind, &materialize_attempts](GpuMemoryEopCheckpointResult result, bool prefix, bool labels, uint64_t publications) {
		const bool published = result.status == GpuMemoryEopCheckpointStatus::Published;
		std::fprintf(stderr, "[GFX_EOP_HOST_CHECKPOINT] reason=%s prefix_completed=%u label_prefix_completed=%u"
		                     " publications=%" PRIu64 " native_profile_admitted=0 unchanged_rejection=%u"
		                     " materialize_attempts=%u retryable=%u\n",
		             result.reason, prefix ? 1u : 0u, labels ? 1u : 0u, publications, published ? 0u : 1u,
		             materialize_attempts, result.retryable ? 1u : 0u);
		if (publication_destination != nullptr && publication_kind == GpuMemoryEopPublicationKind::VirtualClock64)
			std::fprintf(stderr, "[GFX_EOP_VIRTUAL_CLOCK] reason=%s phase=%s published=%u irq=0 physical_clock_claim=0\n",
			             result.reason, prefix ? "after-prefix" : "before-checkpoint", published ? 1u : 0u);
		else if (publication_destination != nullptr && publication_kind == GpuMemoryEopPublicationKind::Immediate32)
			std::fprintf(stderr, "[GFX_EOP_IMMEDIATE32_CHECKPOINT] reason=%s phase=%s published=%u bytes=4 irq=0 native_cache_claim=0\n",
			             result.reason, prefix ? "after-prefix" : "before-checkpoint", published ? 1u : 0u);
		std::fflush(stderr);
		return result;
	};
	const auto reject = [](const char* reason) { return GpuMemoryEopCheckpointResult {GpuMemoryEopCheckpointStatus::Rejected, reason}; };
	const uint64_t publication_bytes = GpuMemoryEopPublicationBytes(publication_kind);
	if (publication_bytes == 0 || (publication_destination == nullptr && publication_kind != GpuMemoryEopPublicationKind::VirtualClock64) ||
	    (publication_destination != nullptr && (publication_destination->Size() != publication_bytes || publication_destination->Address() % publication_bytes != 0)))
		return report(reject("publication-profile-invalid"), false, false, 0);
#if LUMEN_PLATFORM != LUMEN_PLATFORM_OSX
	// Other VM backends have no managed GuestDataLease capability yet.
	return report(reject("mapping-lease-capability-unavailable"), false, false, 0);
#endif
	// RunLock excludes a later batch. A nonempty CCB could still run beside
	// this DCB, so it is outside the first checkpoint profile.
	if (!m_eop_checkpoint_exclusive_stream) return report(reject("concurrent-command-stream"), false, false, 0);
	auto* buffer = m_buffer[m_current_buffer];
	const int queue = buffer->GetQueue();
	const uint64_t epoch = buffer->GetSubmissionSessionEpoch();
	const uint64_t serial = buffer->GetRecordingSubmitSerial();
	const uint64_t memory_generation = publication_destination != nullptr ? publication_destination->Generation() :
	    Core::VirtualMemory::GetMutationGeneration();
	if (memory_generation == UINT64_MAX) return report(reject("mapping-generation-unavailable"), false, false, 0);
	if (publication_destination != nullptr)
	{
		// Both packet parsers call WriteAtEndOfPipe64 directly after Run's
		// setup guard is released. Only that EOP's CP1 is held here; renderer,
		// Label manager and GPU tracker locks have not been entered. RunLock
		// remains owned by the ring coordinator across a failed borrow's pause.
		const auto identity = GetEopInitialTargetIdentity();
		const auto validated = EopCaptureInitialTargetWithLeaseRetry(this, memory_generation,
		    [publication_destination](uint64_t /*original*/, GraphicsGuestLeaseResult* acquisition) {
			    // Validate borrows against the immutable destination's captured
			    // generation. Never Capture a new ticket after releasing CP1.
			    return publication_destination->Validate(acquisition);
		    }, [&]() -> const char* {
			    const auto current = GetEopInitialTargetIdentity();
			    return ScalarLabelCaptureScopeQualified(current, this, m_run_lock_held.load(std::memory_order_acquire),
			        g_command_processor_parser_state != nullptr && g_command_processor_parser_state->owner == this) &&
			        identity.Matches(current) ? nullptr : "recording-changed-before-checkpoint";
		    }, [] { Core::Thread::SleepMicro(1000); }, [] {
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
			    return Core::VirtualMemory::CurrentThreadHasMemoryLease();
#else
			    return false;
#endif
		    });
		if (!validated.captured || validated.rounds > 1)
			std::fprintf(stderr, "[GFX_EOP_INITIAL_TARGET_REVALIDATE] rounds=%u local_attempts=%u validated=%u reason=%s"
			    " original_generation=%" PRIu64 " prefix_flushes=0 gpu_operations=0 callbacks=0 guest_stores=0\n",
			    validated.rounds, validated.acquisition.attempts, validated.captured ? 1u : 0u,
			    validated.acquisition.reason != nullptr ? validated.acquisition.reason : "none", memory_generation);
		if (!validated.captured) return report(reject(validated.acquisition.reason != nullptr ?
		    validated.acquisition.reason : "target-validation-refused"), false, false, 0);
		GpuMemoryEopTargetRangeResult target;
		if (!GpuMemoryEopTargetRangeAvailable(publication_destination->Address(), publication_destination->Size(), &target))
		{
			std::fprintf(stderr, "[GFX_EOP_TARGET_RANGE_REJECT] caller=before-prefix reason=%s heaps=%" PRIu64
			                     " proof_slots=%" PRIu64 " live_examined=%" PRIu64 " heap=%d object=%d plane=%d type=%s\n",
			    target.reason, target.heap_count, target.slot_count, target.live_count, target.heap, target.object, target.plane,
			    Core::EnumName(target.type).C_Str());
			return report(reject(target.reason), false, false, 0);
		}
	}
	BufferFlush(true);
	if (!BufferWaitForPrefix(queue, epoch, serial, Lumen::CriticalPath::Reason::GpuEopWait))
		BufferWait(Lumen::CriticalPath::Reason::GpuEopWait);
	if (!HasCompletedTrackedCommandBuffer(queue, epoch, serial))
		return report(reject("prefix-completion-unproven"), false, false, 0);
	// WriteAtEndOfPipe64 owns exactly one processor lock at this call site.
	// Worker callbacks may acquire it for their own writeback. Waiting with it
	// held would deadlock even when every Vulkan fence is already signalled.
	uint64_t publications = 0;
	bool labels = false;
	{
		struct ReleaseProcessorLock
		{
			Core::Mutex& mutex;
			explicit ReleaseProcessorLock(Core::Mutex& value): mutex(value) { mutex.Unlock(); }
			~ReleaseProcessorLock() { mutex.Lock(); }
		} release(m_mutex);
		Lumen::CriticalPath::Scope publication(Lumen::CriticalPath::Reason::GpuEopPublicationWait,
		                                    static_cast<uint64_t>(m_queue + 1));
		labels = LabelWaitForPublicationPrefix(this, queue, epoch, serial, &publications);
	}
	if (!labels) return report(reject("label-prefix-incomplete"), true, false, publications);
	// The materializer checks the original destination under CP + tracker before
	// any resource effects. Keep its typed lease refusal: a boolean check here
	// would turn a transient failed borrow into a claimed target mutation and
	// prevent the existing pure-preflight continuation from running.
	const auto result = EopMaterializeWithLeaseRetry(this, memory_generation, [&] {
		return GpuMemoryEopCheckpointMaterialize(WindowGetGraphicContext(), this, queue, epoch, serial,
		    memory_generation, publication_destination, publication_kind, immediate32);
	}, [&](bool after_wait) -> const char* {
		// RunLock stays held across the short processor-lock yield. Preserve the
		// original queue/epoch/serial and never flush or wait the prefix again.
		if (!m_eop_checkpoint_exclusive_stream) return "concurrent-command-stream-during-retry";
		if (!HasCompletedTrackedCommandBuffer(queue, epoch, serial)) return "prefix-changed-during-retry";
		(void)after_wait; // Exact target and footprint leases are checked by the continuity probe.
		return nullptr;
	}, [] { Core::Thread::SleepMicro(1000); }, &materialize_attempts, [&](uint64_t original) -> EopContinuityProbe {
		GraphicsGuestLeaseResult acquisition;
		if (publication_destination != nullptr && !publication_destination->Validate(&acquisition))
			return EopContinuityAcquisitionRefusal(acquisition);
		if (!GpuMemoryEopMappingsUnchanged(original, &acquisition))
			return EopContinuityAcquisitionRefusal(acquisition);
		return {true, false};
	});
	return report(result, true, true, publications);
}

void CommandProcessor::MemoryBarrier()
{
	Core::LockGuard lock(m_mutex);

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	if (record_command_effect(this, CommandEffectProbeValue::MemoryBarrier)) return;
#endif
	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);

	GraphicsRenderMemoryBarrier(m_buffer[m_current_buffer]);
}

void CommandProcessor::FullMemoryBarrier()
{
	Core::LockGuard lock(m_mutex);

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	if (record_command_effect(this, CommandEffectProbeValue::FullMemoryBarrier)) return;
#endif
	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);
	GraphicsRenderFullMemoryBarrier(m_buffer[m_current_buffer]);
}

void CommandProcessor::RenderTextureBarrier(uint64_t vaddr, uint64_t size)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);

	GraphicsRenderRenderTextureBarrier(m_buffer[m_current_buffer], vaddr, size);
}

void CommandProcessor::DepthStencilBarrier(uint64_t vaddr, uint64_t size)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);

	GraphicsRenderDepthStencilBarrier(m_buffer[m_current_buffer], vaddr, size);
}

void CommandProcessor::TriggerEvent(uint32_t event_type, uint32_t event_index)
{
	Core::LockGuard lock(m_mutex);

	if (Config::GetPrintfDirection() != Log::Direction::Silent)
	{
		printf("CommandProcessor::TriggerEvent()\n");
		printf("\t event_type  = 0x%08" PRIx32 "\n", event_type);
		printf("\t event_index = 0x%08" PRIx32 "\n", event_index);
	}

	if ((event_type == 0x00000016 || event_type == 0x00000031) && event_index == 0x00000007)
	{
		// CacheFlushAndInvEvent
		// FlushAndInvalidateCbPixelData
		MemoryBarrier();
	} else if ((event_type == 0x0000002c) && event_index == 0x00000007)
	{ // NOLINT
	  // FlushAndInvalidateDbMeta
	} else if ((event_type == 0x00000010) && event_index == 0x00000000)
	{
		// PsPartialFlush
	} else
	{
		EXIT("unknown event type: 0x%08" PRIx32 ", 0x%08" PRIx32 "\n", event_type, event_index);
	}
}

void CommandProcessor::Flip()
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);

	if (Config::GetPrintfDirection() != Log::Direction::Silent)
	{
		printf("CommandProcessor::Flip()\n");
	}

	GraphicsRenderWriteAtEndOfPipeOnlyFlip(m_sumbit_id, m_buffer[m_current_buffer], m_flip.handle, m_flip.index, m_flip.flip_mode,
	                                       m_flip.flip_arg);
}

void CommandProcessor::Flip(void* dst_gpu_addr, uint32_t value)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);

	if (Config::GetPrintfDirection() != Log::Direction::Silent)
	{
		printf("CommandProcessor::Flip()\n");
		printf("\t dst_gpu_addr = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(dst_gpu_addr));
		printf("\t value        = 0x%08" PRIx32 "\n", value);
	}

	GraphicsRenderWriteAtEndOfPipeWithFlip32(m_sumbit_id, m_buffer[m_current_buffer], static_cast<uint32_t*>(dst_gpu_addr), value,
	                                         m_flip.handle, m_flip.index, m_flip.flip_mode, m_flip.flip_arg);
}

void CommandProcessor::FlipWithInterrupt(uint32_t eop_event_type, uint32_t cache_action, void* dst_gpu_addr, uint32_t value)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_current_buffer < 0 || m_current_buffer >= VK_BUFFERS_NUM);

	if (Config::GetPrintfDirection() != Log::Direction::Silent)
	{
		printf("CommandProcessor::FlipWithInterrupt()\n");
		printf("\t eop_event_type      = 0x%08" PRIx32 "\n", eop_event_type);
		printf("\t cache_action        = 0x%08" PRIx32 "\n", cache_action);
		printf("\t dst_gpu_addr        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(dst_gpu_addr));
		printf("\t value               = 0x%08" PRIx32 "\n", value);
	}

	if (eop_event_type == 0x00000004 && cache_action == 0x00000038)
	{
		GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBackFlip32(m_sumbit_id, m_buffer[m_current_buffer],
		                                                           static_cast<uint32_t*>(dst_gpu_addr), value, m_flip.handle, m_flip.index,
		                                                           m_flip.flip_mode, m_flip.flip_arg, m_interrupt_event_id);
	} else
	{
		EXIT("unknown event type\n");
	}
}

template <typename Attempt, typename Backoff, typename LeaseHeld>
static GpuMemoryRenderFullWriteBackResult RenderFullWriteBackWithPreflightRetry(Attempt attempt, Backoff backoff,
                                                                             LeaseHeld lease_held, uint32_t* attempts)
{
	*attempts = 0;
	for (;;)
	{
		++*attempts;
		const auto result = attempt();
		// Every caller scope has unwound before this boundary. A successful or
		// effects-started attempt can never be replayed by the helper.
		if (result.complete || !result.retryable) return result;
		if (lease_held()) return {false, false, "retry-memory-lease-held"};
		if (*attempts >= 16) return {false, false, "render-full-preflight-retry-exhausted"};
		backoff();
	}
}

void CommandProcessor::WriteBack()
{
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	{
		Core::LockGuard lock(m_mutex);
		if (record_command_effect(this, CommandEffectProbeValue::WriteBack)) return;
	}
#endif
	GpuMemoryRenderFullPreflightTicket ticket(Core::VirtualMemory::GetMutationGeneration());
	uint32_t attempts = 0;
	const auto result = RenderFullWriteBackWithPreflightRetry([&] {
		// RunLock is retained by the ring coordinator. Each failed pure
		// preflight unwinds CP2, renderer1 and tracker2 normally.
		Core::LockGuard lock(m_mutex);
		return GraphicsRenderTryWriteBackRenderFull(this, &ticket, m_eop_checkpoint_exclusive_stream);
	}, [] { Core::Thread::SleepMicro(1000); }, [] {
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		return Core::VirtualMemory::CurrentThreadHasMemoryLease();
#else
		return true;
#endif
	}, &attempts);
	if (attempts > 1)
		std::fprintf(stderr, "[GFX_RENDER_FULL_PREFLIGHT_RETRY] attempts=%u complete=%u reason=%s"
		                     " original_generation=%" PRIu64 " gpu_replays=0 callback_replays=0\n",
		    attempts, result.complete ? 1u : 0u, result.reason != nullptr ? result.reason : "none", ticket.Generation());
	if (!result.complete) EXIT("[GFX_RENDER_FULL_WRITEBACK_TERMINAL] reason=%s attempts=%u original_generation=%" PRIu64 "\n",
	    result.reason, attempts, ticket.Generation());
}

void CommandProcessor::WriteBack(uint64_t vaddr, uint64_t size)
{
	Core::LockGuard lock(m_mutex);

	GraphicsRenderWriteBack(this, vaddr, size);
}

// void CommandBuffer::CommandProcessorWait()
//{
//	EXIT_IF(m_parent == nullptr);
//
//	m_parent->BufferWait();
//}

void GraphicsRunSubmit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw)
{
	EXIT_IF(cmd_draw_buffer == nullptr);
	EXIT_IF(num_draw_dw == 0);
	EXIT_IF(g_gpu == nullptr);

	g_gpu->Submit(cmd_draw_buffer, num_draw_dw, cmd_const_buffer, num_const_dw);
}

void GraphicsRunSubmitAndFlip(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer, uint32_t num_const_dw,
                              int handle, int index, int flip_mode, int64_t flip_arg)
{
	EXIT_IF(cmd_draw_buffer == nullptr);
	EXIT_IF(num_draw_dw == 0);
	EXIT_IF(g_gpu == nullptr);

	g_gpu->SubmitAndFlip(cmd_draw_buffer, num_draw_dw, cmd_const_buffer, num_const_dw, handle, index, flip_mode, flip_arg);
}

void GraphicsRunDumpDescriptorProvenance(const char* stage, uint32_t start_register, uint32_t register_count)
{
	if (!command_provenance_enabled())
	{
		return;
	}
	auto* stream = g_command_processor_stream_state;
	if (stream == nullptr || stream->owner == nullptr || stage == nullptr)
	{
		dbg_printf("[GFX_CMD_PROVENANCE] stage=%s writer_context=none\n", stage == nullptr ? "unknown" : stage);
		return;
	}

	ShaderUserStage shader_stage = ShaderUserStage::Pixel;
	if (std::strcmp(stage, "vs") == 0)
	{
		shader_stage = ShaderUserStage::Vertex;
	} else if (std::strcmp(stage, "ps") == 0)
	{
		shader_stage = ShaderUserStage::Pixel;
	} else if (std::strcmp(stage, "cs") == 0)
	{
		shader_stage = ShaderUserStage::Compute;
	} else if (std::strcmp(stage, "gs") == 0)
	{
		shader_stage = ShaderUserStage::Geometry;
	} else
	{
		dbg_printf("[GFX_CMD_PROVENANCE] stage=%s writer_context=unknown_stage\n", stage);
		return;
	}

	stream->owner->DumpDescriptorProvenance(shader_stage, start_register, register_count);
}

bool GraphicsRunCurrentCommandRootOwned()
{
	return g_command_processor_stream_state != nullptr && g_command_processor_stream_state->root_owned;
}

void GraphicsRunDingDong(uint32_t ring_id, uint32_t offset_dw)
{
	EXIT_IF(g_gpu == nullptr);

	g_gpu->DingDong(ring_id, offset_dw);
}

uint32_t GraphicsRunMapComputeQueue(uint32_t pipe_id, uint32_t queue_id, uint32_t* ring_addr, uint32_t ring_size_dw,
                                    uint32_t* read_ptr_addr)
{
	EXIT_IF(g_gpu == nullptr);

	return g_gpu->MapComputeQueue(pipe_id, queue_id, ring_addr, ring_size_dw, read_ptr_addr);
}

void GraphicsRunUnmapComputeQueue(uint32_t id)
{
	EXIT_IF(g_gpu == nullptr);

	g_gpu->Unmap(id);
}

void GraphicsRunWait()
{
	EXIT_IF(g_gpu == nullptr);

	if (g_gpu->Wait())
	{
		// Wait has completed every graphics/compute ring and command processor.
		// Its lock is released on return, so cross-queue texture destruction cannot
		// recurse while the GPU manager mutex is held.
		GpuMemoryDrainReadyRetiredSampledTextures(WindowGetGraphicContext());
	}
}

void GraphicsRunFlushMemory(GraphicContext* ctx)
{
	// Complete existing parser work before taking an epoch it may need.
	// Upload refresh can rearm CPU write tracking, so it must exclude counted
	// CPU readers until all native snapshots and tracker state are published.
	GraphicsRunWait();
	GpuHandoffClaimScope refresh(nullptr);
	GpuMemoryFlushAll(ctx);
}

void GraphicsRunDone()
{
	EXIT_IF(g_gpu == nullptr);

	g_gpu->Done();
}

bool GraphicsRunReportRootOwnershipFinal()
{
	if (g_gpu != nullptr)
	{
		return g_gpu->ReportRootOwnershipFinal();
	}
	return true;
}

bool GraphicsRunAreSubmitsAllowed()
{
	EXIT_IF(g_gpu == nullptr);

	return g_gpu->AreSubmitsAllowed();
}

int GraphicsRunGetFrameNum()
{
	EXIT_IF(g_gpu == nullptr);

	return g_gpu->GetFrameNum();
}

bool GraphicsRunBeginTrackedCommandBufferSubmission(CommandProcessor* cp, int queue, uint64_t* session_epoch,
	                                                  uint64_t* recording_submit_serial,
	                                                  uint64_t* begin_submit_serial_cutoff,
	                                                  uint64_t* begin_completed_serial_cutoff)
{
	return cp != nullptr && cp->BeginTrackedCommandBufferSubmission(queue, session_epoch, recording_submit_serial,
	                                                               begin_submit_serial_cutoff,
	                                                               begin_completed_serial_cutoff);
}

bool GraphicsRunRecordTrackedCommandBufferSubmission(CommandProcessor* cp, int queue, uint64_t session_epoch,
	                                                   uint64_t recording_submit_serial)
{
	return cp != nullptr && cp->RecordTrackedCommandBufferSubmission(queue, session_epoch, recording_submit_serial);
}

void GraphicsRunRecordTrackedCommandBufferCompletion(CommandProcessor* cp, int queue, uint64_t session_epoch,
	                                                  uint64_t recording_submit_serial)
{
	if (cp != nullptr)
	{
		cp->RecordTrackedCommandBufferCompletion(queue, session_epoch, recording_submit_serial);
	}
}

void GraphicsRunCommandProcessorFlush(CommandProcessor* cp)
{
	EXIT_IF(cp == nullptr);

	cp->BufferFlush();
}

void GraphicsRunCommandProcessorFlushForHostRead(CommandProcessor* cp)
{
	EXIT_IF(cp == nullptr);

	cp->BufferFlush(true);
}

void GraphicsRunCommandProcessorWait(CommandProcessor* cp)
{
	EXIT_IF(cp == nullptr);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_GRAPHICS_STATE_TEST_HOOK)
	// Guest-object destruction runs after the proven device-idle transition.
	// Do not let it resurrect a recording command buffer in the dormant gap.
	// Failed or pre-barrier resets keep callbacks-safe clear, so they still take
	// the ordinary fail-closed wait path instead of silently bypassing it.
	if (g_gpu != nullptr && g_gpu->CanSkipCommandProcessorWaitAfterIOSReset())
	{
		return;
	}
#endif
	cp->BufferWait();
}

bool GraphicsRunCommandProcessorWaitForPrefix(CommandProcessor* cp, int queue, uint64_t session_epoch,
                                             uint64_t recording_submit_serial)
{
	return cp != nullptr && cp->BufferWaitForPrefix(queue, session_epoch, recording_submit_serial,
	    Lumen::CriticalPath::Reason::GpuDependencyWait);
}

bool GraphicsRunCommandProcessorSubmitted(CommandProcessor* cp, int queue, uint64_t session_epoch,
                                         uint64_t recording_submit_serial)
{
	return cp != nullptr && cp->HasSubmittedTrackedCommandBuffer(queue, session_epoch, recording_submit_serial);
}

bool GraphicsRunCommandProcessorCompleted(CommandProcessor* cp, int queue, uint64_t session_epoch,
                                         uint64_t recording_submit_serial)
{
	return cp != nullptr && cp->HasCompletedTrackedCommandBuffer(queue, session_epoch, recording_submit_serial);
}

bool GraphicsRunCommandProcessorEopResumeValid(CommandProcessor* cp, int queue, uint64_t session_epoch,
                                              uint64_t recording_submit_serial)
{
	return cp != nullptr && cp->EopResumeValid(queue, session_epoch, recording_submit_serial);
}

bool GraphicsRunCommandProcessorExclusiveRecordingValid(CommandProcessor* cp, CommandBuffer* buffer)
{
	return cp != nullptr && cp->ExclusiveRecordingValid(buffer);
}

bool GraphicsRunDrawAccessReservationScopeValid(CommandProcessor* cp, CommandBuffer* buffer)
{
	return GraphicsRunCommandProcessorRetainedRecordingValid(cp, buffer) &&
	       (!GpuMemoryCpuHandoffEnabled() || (g_gpu_handoff_claim != nullptr && g_gpu_handoff_claim->OwnsRecordingFor(cp)));
}

bool GraphicsRunCommandProcessorRetainedRecordingValid(CommandProcessor* cp, CommandBuffer* buffer)
{
	return cp != nullptr && cp->RetainedRecordingValid(buffer);
}

LabelScalarRecordingIdentity GraphicsRunCommandProcessorScalarLabelIdentity(CommandProcessor* cp, CommandBuffer* buffer)
{
	return cp != nullptr ? cp->ScalarLabelIdentity(buffer) : LabelScalarRecordingIdentity {};
}

LabelScalarRecordingIdentity GraphicsRunCommandProcessorDeferredLabelIdentity(CommandProcessor* cp, CommandBuffer* buffer)
{
	return cp != nullptr ? cp->DeferredLabelIdentity(buffer) : LabelScalarRecordingIdentity {};
}

CommandBuffer* GraphicsRunCommandProcessorGetCurrentBuffer(CommandProcessor* cp)
{
	EXIT_IF(cp == nullptr);
	return cp->GetCurrentBufferForCpuCopy();
}

void GraphicsRunCommandProcessorLock(CommandProcessor* cp)
{
	EXIT_IF(cp == nullptr);

	cp->Lock();
}

void GraphicsRunCommandProcessorUnlock(CommandProcessor* cp)
{
	EXIT_IF(cp == nullptr);

	cp->Unlock();
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_aa_config)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::PA_SC_AA_CONFIG);

	HW::AaConfig r;

	r.msaa_num_samples      = LUMEN_PM4_GET(buffer[0], PA_SC_AA_CONFIG, MSAA_NUM_SAMPLES);
	r.aa_mask_centroid_dtmn = LUMEN_PM4_GET(buffer[0], PA_SC_AA_CONFIG, AA_MASK_CENTROID_DTMN) != 0;
	r.max_sample_dist       = LUMEN_PM4_GET(buffer[0], PA_SC_AA_CONFIG, MAX_SAMPLE_DIST);
	r.msaa_exposed_samples  = LUMEN_PM4_GET(buffer[0], PA_SC_AA_CONFIG, MSAA_EXPOSED_SAMPLES);

	cp->GetCtx()->SetAaConfig(r);

	return 1;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_aa_sample_control)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0106900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0);

	uint32_t count = 1;

	if (dw >= 20 && buffer[16] == 0xc0026900 && buffer[17] == Pm4::PA_SC_CENTROID_PRIORITY_0)
	{
		count = 20;

		HW::AaSampleControl r;

		memcpy(r.locations, buffer, static_cast<size_t>(16) * 4);

		r.centroid_priority = static_cast<uint64_t>(buffer[18]) | (static_cast<uint64_t>(buffer[19]) << 32u);

		cp->GetCtx()->SetAaSampleControl(r);
	} else
	{
		LUMEN_NOT_IMPLEMENTED;
	}

	return count;
}

static uint32_t hw_ctx_update_blend_color(HW::Context* ctx, uint32_t cmd_id, uint32_t cmd_offset,
                                        const uint32_t* values, uint32_t dw)
{
	// This interface supports the complete four-register packet only. Validate
	// the available packet length before reading or changing any component.
	if (ctx == nullptr || values == nullptr || cmd_id != LUMEN_PM4(6, Pm4::IT_SET_CONTEXT_REG, 0) ||
	    cmd_offset != Pm4::CB_BLEND_RED || dw < 6) return 0;

	HW::BlendColor r;
	std::memcpy(&r.red, &values[0], sizeof(r.red));
	std::memcpy(&r.green, &values[1], sizeof(r.green));
	std::memcpy(&r.blue, &values[2], sizeof(r.blue));
	std::memcpy(&r.alpha, &values[3], sizeof(r.alpha));
	ctx->SetBlendColor(r);
	return 4;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_blend_color)
{
	const uint32_t count = hw_ctx_update_blend_color(cp->GetCtx(), cmd_id, cmd_offset, buffer, dw);
	EXIT_NOT_IMPLEMENTED(count == 0);
	return count;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_blend_control)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);

	uint32_t param = (cmd_offset - Pm4::CB_BLEND0_CONTROL) / 1;

	HW::BlendControl r;

	r.color_srcblend  = (buffer[0] >> Pm4::CB_BLEND0_CONTROL_COLOR_SRCBLEND_SHIFT) & Pm4::CB_BLEND0_CONTROL_COLOR_SRCBLEND_MASK;
	r.color_comb_fcn  = (buffer[0] >> Pm4::CB_BLEND0_CONTROL_COLOR_COMB_FCN_SHIFT) & Pm4::CB_BLEND0_CONTROL_COLOR_COMB_FCN_MASK;
	r.color_destblend = (buffer[0] >> Pm4::CB_BLEND0_CONTROL_COLOR_DESTBLEND_SHIFT) & Pm4::CB_BLEND0_CONTROL_COLOR_DESTBLEND_MASK;
	r.alpha_srcblend  = (buffer[0] >> Pm4::CB_BLEND0_CONTROL_ALPHA_SRCBLEND_SHIFT) & Pm4::CB_BLEND0_CONTROL_ALPHA_SRCBLEND_MASK;
	r.alpha_comb_fcn  = (buffer[0] >> Pm4::CB_BLEND0_CONTROL_ALPHA_COMB_FCN_SHIFT) & Pm4::CB_BLEND0_CONTROL_ALPHA_COMB_FCN_MASK;
	r.alpha_destblend = (buffer[0] >> Pm4::CB_BLEND0_CONTROL_ALPHA_DESTBLEND_SHIFT) & Pm4::CB_BLEND0_CONTROL_ALPHA_DESTBLEND_MASK;
	r.separate_alpha_blend =
	    ((buffer[0] >> Pm4::CB_BLEND0_CONTROL_SEPARATE_ALPHA_BLEND_SHIFT) & Pm4::CB_BLEND0_CONTROL_SEPARATE_ALPHA_BLEND_MASK) != 0;
	r.enable = ((buffer[0] >> Pm4::CB_BLEND0_CONTROL_ENABLE_SHIFT) & Pm4::CB_BLEND0_CONTROL_ENABLE_MASK) != 0;

	cp->GetCtx()->SetBlendControl(param, r);

	return 1;
}

static HW::ClipControl decode_clip_control(uint32_t value)
{
	HW::ClipControl r {};

	//	r.user_clip_planes                    = buffer[0] & 0x3fu;
	//	r.user_clip_plane_mode                = (buffer[0] >> 14u) & 0x3u;
	//	r.clip_space                          = (buffer[0] >> 19u) & 0x1u;
	//	r.vertex_kill_mode                    = (buffer[0] >> 21u) & 0x1u;
	//	r.min_z_clip_enable                   = (buffer[0] >> 26u) & 0x1u;
	//	r.max_z_clip_enable                   = (buffer[0] >> 27u) & 0x1u;
	//	r.user_clip_plane_negate_y            = (buffer[0] & 0x00002000u) != 0;
	//	r.clip_enable                         = (buffer[0] & 0x00010000u) != 0;
	//	r.user_clip_plane_cull_only           = (buffer[0] & 0x00020000u) != 0;
	//	r.cull_on_clipping_error_disable      = (buffer[0] & 0x00100000u) != 0;
	//	r.linear_attribute_clip_enable        = (buffer[0] & 0x01000000u) != 0;
	//	r.force_viewport_index_from_vs_enable = (buffer[0] & 0x02000000u) != 0;

	r.user_clip_planes                    = LUMEN_PM4_GET(value, PA_CL_CLIP_CNTL, UCP_ENA);
	r.user_clip_plane_mode                = LUMEN_PM4_GET(value, PA_CL_CLIP_CNTL, PS_UCP_MODE);
	r.dx_clip_space                       = LUMEN_PM4_GET(value, PA_CL_CLIP_CNTL, DX_CLIP_SPACE_DEF) != 0;
	r.vertex_kill_any                     = LUMEN_PM4_GET(value, PA_CL_CLIP_CNTL, VTX_KILL_OR) != 0;
	r.min_z_clip_disable                  = LUMEN_PM4_GET(value, PA_CL_CLIP_CNTL, ZCLIP_NEAR_DISABLE) != 0;
	r.max_z_clip_disable                  = LUMEN_PM4_GET(value, PA_CL_CLIP_CNTL, ZCLIP_FAR_DISABLE) != 0;
	r.user_clip_plane_negate_y            = LUMEN_PM4_GET(value, PA_CL_CLIP_CNTL, PS_UCP_Y_SCALE_NEG) != 0;
	r.clip_disable                        = LUMEN_PM4_GET(value, PA_CL_CLIP_CNTL, CLIP_DISABLE) != 0;
	r.user_clip_plane_cull_only           = LUMEN_PM4_GET(value, PA_CL_CLIP_CNTL, UCP_CULL_ONLY_ENA) != 0;
	r.cull_on_clipping_error_disable      = LUMEN_PM4_GET(value, PA_CL_CLIP_CNTL, DIS_CLIP_ERR_DETECT) != 0;
	r.linear_attribute_clip_enable        = LUMEN_PM4_GET(value, PA_CL_CLIP_CNTL, DX_LINEAR_ATTR_CLIP_ENA) != 0;
	r.force_viewport_index_from_vs_enable = LUMEN_PM4_GET(value, PA_CL_CLIP_CNTL, VTE_VPORT_PROVOKE_DISABLE) != 0;
	return r;

}

static uint32_t hw_ctx_update_clip_control(HW::Context* ctx, uint32_t cmd_id, uint32_t cmd_offset, const uint32_t* buffer, uint32_t dw)
{
	if (ctx == nullptr || buffer == nullptr || cmd_id != LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0) ||
	    cmd_offset != Pm4::PA_CL_CLIP_CNTL || dw < 3)
	{
		return 0;
	}

	ctx->SetClipControl(decode_clip_control(buffer[0]));
	return 1;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_clip_control)
{
	const auto count = hw_ctx_update_clip_control(cp->GetCtx(), cmd_id, cmd_offset, buffer, dw);
	EXIT_NOT_IMPLEMENTED(count == 0);

	return count;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_color_control)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::CB_COLOR_CONTROL);

	HW::ColorControl r;

	r.mode = LUMEN_PM4_GET(buffer[0], CB_COLOR_CONTROL, MODE);
	r.op   = LUMEN_PM4_GET(buffer[0], CB_COLOR_CONTROL, ROP3);

	cp->GetCtx()->SetColorControl(r);

	return 1;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_depth_clear)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::DB_DEPTH_CLEAR);

	cp->GetCtx()->SetDepthClearValue(*reinterpret_cast<const float*>(buffer));

	return 1;
}

static constexpr uint32_t depth_control_modeled_mask =
    (Pm4::DB_DEPTH_CONTROL_STENCIL_ENABLE_MASK << Pm4::DB_DEPTH_CONTROL_STENCIL_ENABLE_SHIFT) |
    (Pm4::DB_DEPTH_CONTROL_Z_ENABLE_MASK << Pm4::DB_DEPTH_CONTROL_Z_ENABLE_SHIFT) |
    (Pm4::DB_DEPTH_CONTROL_Z_WRITE_ENABLE_MASK << Pm4::DB_DEPTH_CONTROL_Z_WRITE_ENABLE_SHIFT) |
    (Pm4::DB_DEPTH_CONTROL_DEPTH_BOUNDS_ENABLE_MASK << Pm4::DB_DEPTH_CONTROL_DEPTH_BOUNDS_ENABLE_SHIFT) |
    (Pm4::DB_DEPTH_CONTROL_ZFUNC_MASK << Pm4::DB_DEPTH_CONTROL_ZFUNC_SHIFT) |
    (Pm4::DB_DEPTH_CONTROL_BACKFACE_ENABLE_MASK << Pm4::DB_DEPTH_CONTROL_BACKFACE_ENABLE_SHIFT) |
    (Pm4::DB_DEPTH_CONTROL_STENCILFUNC_MASK << Pm4::DB_DEPTH_CONTROL_STENCILFUNC_SHIFT) |
    (Pm4::DB_DEPTH_CONTROL_STENCILFUNC_BF_MASK << Pm4::DB_DEPTH_CONTROL_STENCILFUNC_BF_SHIFT) |
    (Pm4::DB_DEPTH_CONTROL_ENABLE_COLOR_WRITES_ON_DEPTH_FAIL_MASK << Pm4::DB_DEPTH_CONTROL_ENABLE_COLOR_WRITES_ON_DEPTH_FAIL_SHIFT) |
    (Pm4::DB_DEPTH_CONTROL_DISABLE_COLOR_WRITES_ON_DEPTH_PASS_MASK << Pm4::DB_DEPTH_CONTROL_DISABLE_COLOR_WRITES_ON_DEPTH_PASS_SHIFT);

LUMEN_HW_CTX_PARSER(hw_ctx_set_depth_control)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::DB_DEPTH_CONTROL);

	HW::DepthControl r;

	r.stencil_enable                     = LUMEN_PM4_GET(buffer[0], DB_DEPTH_CONTROL, STENCIL_ENABLE) != 0;
	r.z_enable                           = LUMEN_PM4_GET(buffer[0], DB_DEPTH_CONTROL, Z_ENABLE) != 0;
	r.z_write_enable                     = LUMEN_PM4_GET(buffer[0], DB_DEPTH_CONTROL, Z_WRITE_ENABLE) != 0;
	r.depth_bounds_enable                = LUMEN_PM4_GET(buffer[0], DB_DEPTH_CONTROL, DEPTH_BOUNDS_ENABLE) != 0;
	r.zfunc                              = LUMEN_PM4_GET(buffer[0], DB_DEPTH_CONTROL, ZFUNC);
	r.backface_enable                    = LUMEN_PM4_GET(buffer[0], DB_DEPTH_CONTROL, BACKFACE_ENABLE) != 0;
	r.stencilfunc                        = LUMEN_PM4_GET(buffer[0], DB_DEPTH_CONTROL, STENCILFUNC);
	r.stencilfunc_bf                     = LUMEN_PM4_GET(buffer[0], DB_DEPTH_CONTROL, STENCILFUNC_BF);
	r.color_writes_on_depth_fail_enable  = LUMEN_PM4_GET(buffer[0], DB_DEPTH_CONTROL, ENABLE_COLOR_WRITES_ON_DEPTH_FAIL) != 0;
	r.color_writes_on_depth_pass_disable = LUMEN_PM4_GET(buffer[0], DB_DEPTH_CONTROL, DISABLE_COLOR_WRITES_ON_DEPTH_PASS) != 0;
	r.raw                                = buffer[0];
	r.unsupported_bits                   = buffer[0] & ~depth_control_modeled_mask;

	cp->GetCtx()->SetDepthControl(r);

	return 1;
}

// Retain every bit outside the fields represented by the hardware context.
// A register overwrite replaces this evidence, including a write of zero.
static constexpr uint32_t depth_z_info_modeled_mask =
    (Pm4::DB_Z_INFO_FORMAT_MASK << Pm4::DB_Z_INFO_FORMAT_SHIFT) |
    (Pm4::DB_Z_INFO_NUM_SAMPLES_MASK << Pm4::DB_Z_INFO_NUM_SAMPLES_SHIFT) |
    (Pm4::DB_Z_INFO_ITERATE_FLUSH_MASK << Pm4::DB_Z_INFO_ITERATE_FLUSH_SHIFT) |
    (Pm4::DB_Z_INFO_PARTIALLY_RESIDENT_MASK << Pm4::DB_Z_INFO_PARTIALLY_RESIDENT_SHIFT) |
    (Pm4::DB_Z_INFO_MAXMIP_MASK << Pm4::DB_Z_INFO_MAXMIP_SHIFT) |
    (Pm4::DB_Z_INFO_TILE_MODE_INDEX_MASK << Pm4::DB_Z_INFO_TILE_MODE_INDEX_SHIFT) |
    (Pm4::DB_Z_INFO_DECOMPRESS_ON_N_ZPLANES_MASK << Pm4::DB_Z_INFO_DECOMPRESS_ON_N_ZPLANES_SHIFT) |
    (Pm4::DB_Z_INFO_ALLOW_EXPCLEAR_MASK << Pm4::DB_Z_INFO_ALLOW_EXPCLEAR_SHIFT) |
    (Pm4::DB_Z_INFO_TILE_SURFACE_ENABLE_MASK << Pm4::DB_Z_INFO_TILE_SURFACE_ENABLE_SHIFT) |
    (Pm4::DB_Z_INFO_ZRANGE_PRECISION_MASK << Pm4::DB_Z_INFO_ZRANGE_PRECISION_SHIFT);
static constexpr uint32_t depth_stencil_info_modeled_mask =
    (Pm4::DB_STENCIL_INFO_FORMAT_MASK << Pm4::DB_STENCIL_INFO_FORMAT_SHIFT) |
    (Pm4::DB_STENCIL_INFO_ITERATE_FLUSH_MASK << Pm4::DB_STENCIL_INFO_ITERATE_FLUSH_SHIFT) |
    (Pm4::DB_STENCIL_INFO_PARTIALLY_RESIDENT_MASK << Pm4::DB_STENCIL_INFO_PARTIALLY_RESIDENT_SHIFT) |
    (Pm4::DB_STENCIL_INFO_RESERVED_FIELD_1_MASK << Pm4::DB_STENCIL_INFO_RESERVED_FIELD_1_SHIFT) |
    (Pm4::DB_STENCIL_INFO_TILE_MODE_INDEX_MASK << Pm4::DB_STENCIL_INFO_TILE_MODE_INDEX_SHIFT) |
    (Pm4::DB_STENCIL_INFO_ALLOW_EXPCLEAR_MASK << Pm4::DB_STENCIL_INFO_ALLOW_EXPCLEAR_SHIFT) |
    (Pm4::DB_STENCIL_INFO_TILE_STENCIL_DISABLE_MASK << Pm4::DB_STENCIL_INFO_TILE_STENCIL_DISABLE_SHIFT);
static constexpr uint32_t depth_info_modeled_mask =
    (Pm4::DB_DEPTH_INFO_ADDR5_SWIZZLE_MASK_MASK << Pm4::DB_DEPTH_INFO_ADDR5_SWIZZLE_MASK_SHIFT) |
    (Pm4::DB_DEPTH_INFO_ARRAY_MODE_MASK << Pm4::DB_DEPTH_INFO_ARRAY_MODE_SHIFT) |
    (Pm4::DB_DEPTH_INFO_PIPE_CONFIG_MASK << Pm4::DB_DEPTH_INFO_PIPE_CONFIG_SHIFT) |
    (Pm4::DB_DEPTH_INFO_BANK_WIDTH_MASK << Pm4::DB_DEPTH_INFO_BANK_WIDTH_SHIFT) |
    (Pm4::DB_DEPTH_INFO_BANK_HEIGHT_MASK << Pm4::DB_DEPTH_INFO_BANK_HEIGHT_SHIFT) |
    (Pm4::DB_DEPTH_INFO_MACRO_TILE_ASPECT_MASK << Pm4::DB_DEPTH_INFO_MACRO_TILE_ASPECT_SHIFT) |
    (Pm4::DB_DEPTH_INFO_NUM_BANKS_MASK << Pm4::DB_DEPTH_INFO_NUM_BANKS_SHIFT);
static constexpr uint32_t depth_size_modeled_mask =
    (Pm4::DB_DEPTH_SIZE_PITCH_TILE_MAX_MASK << Pm4::DB_DEPTH_SIZE_PITCH_TILE_MAX_SHIFT) |
    (Pm4::DB_DEPTH_SIZE_HEIGHT_TILE_MAX_MASK << Pm4::DB_DEPTH_SIZE_HEIGHT_TILE_MAX_SHIFT);
static constexpr uint32_t depth_slice_modeled_mask =
    Pm4::DB_DEPTH_SLICE_SLICE_TILE_MAX_MASK << Pm4::DB_DEPTH_SLICE_SLICE_TILE_MAX_SHIFT;

LUMEN_HW_CTX_PARSER(hw_ctx_set_depth_render_target)
{
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::DB_Z_INFO);

	uint32_t count = 1;

	if (cmd_id == 0xC0016900)
	{
		HW::DepthZInfo r;

		//	r.expclear_enabled = (buffer[0] & 0x08000000u) != 0;
		//	r.format              = (buffer[0] >> Pm4::DB_Z_INFO_FORMAT_SHIFT) & Pm4::DB_Z_INFO_FORMAT_MASK;
		//	r.num_samples         = (buffer[0] >> Pm4::DB_Z_INFO_NUM_SAMPLES_SHIFT) & Pm4::DB_Z_INFO_NUM_SAMPLES_MASK;
		//	r.tile_mode_index     = (buffer[0] >> Pm4::DB_Z_INFO_TILE_MODE_INDEX_SHIFT) & Pm4::DB_Z_INFO_TILE_MODE_INDEX_MASK;
		//	r.tile_surface_enable = ((buffer[0] >> Pm4::DB_Z_INFO_TILE_SURFACE_ENABLE_SHIFT) & Pm4::DB_Z_INFO_TILE_SURFACE_ENABLE_MASK) !=0
		// 	r.zrange_precision    = (buffer[0] >> Pm4::DB_Z_INFO_ZRANGE_PRECISION_SHIFT) & Pm4::DB_Z_INFO_ZRANGE_PRECISION_MASK;

		r.format                    = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, FORMAT);
		r.num_samples               = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, NUM_SAMPLES);
		r.embedded_sample_locations = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, ITERATE_FLUSH) != 0;
		r.partially_resident        = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, PARTIALLY_RESIDENT) != 0;
		r.num_mip_levels            = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, MAXMIP);
		r.tile_mode_index           = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, TILE_MODE_INDEX);
		r.plane_compression         = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, DECOMPRESS_ON_N_ZPLANES);
		r.expclear_enabled          = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, ALLOW_EXPCLEAR) != 0;
		r.tile_surface_enable       = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, TILE_SURFACE_ENABLE) != 0;
		r.zrange_precision          = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, ZRANGE_PRECISION);
		r.unsupported_bits          = buffer[0] & ~depth_z_info_modeled_mask;

		cp->GetCtx()->SetDepthZInfo(r);
	} else if (cmd_id == 0xC0086900)
	{
		if (dw >= 22 && buffer[8] == 0xC0016900 && buffer[9] == Pm4::DB_DEPTH_INFO && buffer[11] == 0xC0016900 &&
		    buffer[12] == Pm4::DB_DEPTH_VIEW && buffer[14] == 0xC0016900 && buffer[15] == Pm4::DB_HTILE_DATA_BASE &&
		    buffer[17] == 0xC0016900 && buffer[18] == Pm4::DB_HTILE_SURFACE && buffer[20] == 0xC0001000)
		{
			count = 22;

			HW::DepthRenderTarget z;

			//			z.z_info.expclear_enabled    = (buffer[0] & 0x08000000u) != 0;
			//			z.z_info.format              = (buffer[0] >> Pm4::DB_Z_INFO_FORMAT_SHIFT) & Pm4::DB_Z_INFO_FORMAT_MASK;
			//			z.z_info.num_samples         = (buffer[0] >> Pm4::DB_Z_INFO_NUM_SAMPLES_SHIFT) & Pm4::DB_Z_INFO_NUM_SAMPLES_MASK;
			//			z.z_info.tile_mode_index     = (buffer[0] >> Pm4::DB_Z_INFO_TILE_MODE_INDEX_SHIFT) &
			// Pm4::DB_Z_INFO_TILE_MODE_INDEX_MASK; 			z.z_info.tile_surface_enable = LUMEN_PM4_GET(buffer[0], DB_Z_INFO,
			// TILE_SURFACE_ENABLE) != 0; 			z.z_info.zrange_precision    = (buffer[0] >> Pm4::DB_Z_INFO_ZRANGE_PRECISION_SHIFT) &
			// Pm4::DB_Z_INFO_ZRANGE_PRECISION_MASK;

			z.z_info.format                    = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, FORMAT);
			z.z_info.num_samples               = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, NUM_SAMPLES);
			z.z_info.embedded_sample_locations = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, ITERATE_FLUSH) != 0;
			z.z_info.partially_resident        = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, PARTIALLY_RESIDENT) != 0;
			z.z_info.num_mip_levels            = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, MAXMIP);
			z.z_info.tile_mode_index           = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, TILE_MODE_INDEX);
			z.z_info.plane_compression         = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, DECOMPRESS_ON_N_ZPLANES);
			z.z_info.expclear_enabled          = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, ALLOW_EXPCLEAR) != 0;
			z.z_info.tile_surface_enable       = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, TILE_SURFACE_ENABLE) != 0;
			z.z_info.zrange_precision          = LUMEN_PM4_GET(buffer[0], DB_Z_INFO, ZRANGE_PRECISION);
			z.z_info.unsupported_bits          = buffer[0] & ~depth_z_info_modeled_mask;

			//			z.stencil_info.expclear_enabled     = (buffer[1] & 0x08000000u) != 0;
			//			z.stencil_info.tile_split           = (buffer[1] >> 13u) & 0x7u;
			//			z.stencil_info.format               = LUMEN_PM4_GET(buffer[1], DB_STENCIL_INFO, FORMAT);
			//			z.stencil_info.tile_mode_index      = LUMEN_PM4_GET(buffer[1], DB_STENCIL_INFO, TILE_MODE_INDEX);
			//			z.stencil_info.tile_stencil_disable = LUMEN_PM4_GET(buffer[1], DB_STENCIL_INFO, TILE_STENCIL_DISABLE);
			z.stencil_info.format                     = LUMEN_PM4_GET(buffer[1], DB_STENCIL_INFO, FORMAT);
			z.stencil_info.texture_compatible_stencil = LUMEN_PM4_GET(buffer[1], DB_STENCIL_INFO, ITERATE_FLUSH) != 0;
			z.stencil_info.partially_resident         = LUMEN_PM4_GET(buffer[1], DB_STENCIL_INFO, PARTIALLY_RESIDENT) != 0;
			z.stencil_info.tile_split                 = LUMEN_PM4_GET(buffer[1], DB_STENCIL_INFO, RESERVED_FIELD_1);
			z.stencil_info.tile_mode_index            = LUMEN_PM4_GET(buffer[1], DB_STENCIL_INFO, TILE_MODE_INDEX);
			z.stencil_info.expclear_enabled           = LUMEN_PM4_GET(buffer[1], DB_STENCIL_INFO, ALLOW_EXPCLEAR) != 0;
			z.stencil_info.tile_stencil_disable       = LUMEN_PM4_GET(buffer[1], DB_STENCIL_INFO, TILE_STENCIL_DISABLE) != 0;
			z.stencil_info.unsupported_bits           = buffer[1] & ~depth_stencil_info_modeled_mask;

			z.z_read_base_addr        = static_cast<uint64_t>(buffer[2]) << 8u;
			z.stencil_read_base_addr  = static_cast<uint64_t>(buffer[3]) << 8u;
			z.z_write_base_addr       = static_cast<uint64_t>(buffer[4]) << 8u;
			z.stencil_write_base_addr = static_cast<uint64_t>(buffer[5]) << 8u;

			// DB_DEPTH_SIZE
			z.pitch_div8_minus1  = (buffer[6] >> Pm4::DB_DEPTH_SIZE_PITCH_TILE_MAX_SHIFT) & Pm4::DB_DEPTH_SIZE_PITCH_TILE_MAX_MASK;
			z.height_div8_minus1 = (buffer[6] >> Pm4::DB_DEPTH_SIZE_HEIGHT_TILE_MAX_SHIFT) & Pm4::DB_DEPTH_SIZE_HEIGHT_TILE_MAX_MASK;
			z.depth_size_unsupported_bits = buffer[6] & ~depth_size_modeled_mask;

			// DB_DEPTH_SLICE
			z.slice_div64_minus1 = (buffer[7] >> Pm4::DB_DEPTH_SLICE_SLICE_TILE_MAX_SHIFT) & Pm4::DB_DEPTH_SLICE_SLICE_TILE_MAX_MASK;
			z.depth_slice_unsupported_bits = buffer[7] & ~depth_slice_modeled_mask;

			z.depth_info.addr5_swizzle_mask = LUMEN_PM4_GET(buffer[10], DB_DEPTH_INFO, ADDR5_SWIZZLE_MASK);
			z.depth_info.array_mode         = (buffer[10] >> Pm4::DB_DEPTH_INFO_ARRAY_MODE_SHIFT) & Pm4::DB_DEPTH_INFO_ARRAY_MODE_MASK;
			z.depth_info.pipe_config        = (buffer[10] >> Pm4::DB_DEPTH_INFO_PIPE_CONFIG_SHIFT) & Pm4::DB_DEPTH_INFO_PIPE_CONFIG_MASK;
			z.depth_info.bank_width         = (buffer[10] >> Pm4::DB_DEPTH_INFO_BANK_WIDTH_SHIFT) & Pm4::DB_DEPTH_INFO_BANK_WIDTH_MASK;
			z.depth_info.bank_height        = (buffer[10] >> Pm4::DB_DEPTH_INFO_BANK_HEIGHT_SHIFT) & Pm4::DB_DEPTH_INFO_BANK_HEIGHT_MASK;
			z.depth_info.macro_tile_aspect  = LUMEN_PM4_GET(buffer[10], DB_DEPTH_INFO, MACRO_TILE_ASPECT);
			z.depth_info.num_banks          = (buffer[10] >> Pm4::DB_DEPTH_INFO_NUM_BANKS_SHIFT) & Pm4::DB_DEPTH_INFO_NUM_BANKS_MASK;
			z.depth_info.unsupported_bits   = buffer[10] & ~depth_info_modeled_mask;

			// z.depth_view.slice_start = (buffer[13] >> Pm4::DB_DEPTH_VIEW_SLICE_START_SHIFT) & Pm4::DB_DEPTH_VIEW_SLICE_START_MASK;
			// z.depth_view.slice_max   = (buffer[13] >> Pm4::DB_DEPTH_VIEW_SLICE_MAX_SHIFT) & Pm4::DB_DEPTH_VIEW_SLICE_MAX_MASK;

			z.depth_view.slice_start =
			    LUMEN_PM4_GET(buffer[13], DB_DEPTH_VIEW, SLICE_START) + (LUMEN_PM4_GET(buffer[13], DB_DEPTH_VIEW, SLICE_START_HI) << 11u);
			z.depth_view.slice_max =
			    LUMEN_PM4_GET(buffer[13], DB_DEPTH_VIEW, SLICE_MAX) + (LUMEN_PM4_GET(buffer[13], DB_DEPTH_VIEW, SLICE_MAX_HI) << 11u);
			z.depth_view.depth_write_disable   = LUMEN_PM4_GET(buffer[13], DB_DEPTH_VIEW, Z_READ_ONLY) != 0;
			z.depth_view.stencil_write_disable = LUMEN_PM4_GET(buffer[13], DB_DEPTH_VIEW, STENCIL_READ_ONLY) != 0;
			z.depth_view.current_mip_level     = LUMEN_PM4_GET(buffer[13], DB_DEPTH_VIEW, MIPID);

			z.htile_data_base_addr = static_cast<uint64_t>(buffer[16]) << 8u;

			z.htile_surface.raw        = buffer[19];
			z.htile_surface.linear     = (buffer[19] >> Pm4::DB_HTILE_SURFACE_LINEAR_SHIFT) & Pm4::DB_HTILE_SURFACE_LINEAR_MASK;
			z.htile_surface.full_cache = (buffer[19] >> Pm4::DB_HTILE_SURFACE_FULL_CACHE_SHIFT) & Pm4::DB_HTILE_SURFACE_FULL_CACHE_MASK;
			z.htile_surface.htile_uses_preload_win = LUMEN_PM4_GET(buffer[19], DB_HTILE_SURFACE, HTILE_USES_PRELOAD_WIN);
			z.htile_surface.preload         = (buffer[19] >> Pm4::DB_HTILE_SURFACE_PRELOAD_SHIFT) & Pm4::DB_HTILE_SURFACE_PRELOAD_MASK;
			z.htile_surface.prefetch_width  = LUMEN_PM4_GET(buffer[19], DB_HTILE_SURFACE, PREFETCH_WIDTH);
			z.htile_surface.prefetch_height = LUMEN_PM4_GET(buffer[19], DB_HTILE_SURFACE, PREFETCH_HEIGHT);
			z.htile_surface.dst_outside_zero_to_one = LUMEN_PM4_GET(buffer[19], DB_HTILE_SURFACE, DST_OUTSIDE_ZERO_TO_ONE);

			z.width  = (buffer[21] >> 0u) & 0xffffu;
			z.height = (buffer[21] >> 16u) & 0xffffu;

			cp->GetCtx()->SetDepthRenderTarget(z);
		} else
		{
			LUMEN_NOT_IMPLEMENTED;
		}
	} else
	{
		LUMEN_NOT_IMPLEMENTED;
	}

	return count;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_eqaa_control)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::DB_EQAA);

	HW::EqaaControl r;

	r.max_anchor_samples         = LUMEN_PM4_GET(buffer[0], DB_EQAA, MAX_ANCHOR_SAMPLES);
	r.ps_iter_samples            = LUMEN_PM4_GET(buffer[0], DB_EQAA, PS_ITER_SAMPLES);
	r.mask_export_num_samples    = LUMEN_PM4_GET(buffer[0], DB_EQAA, MASK_EXPORT_NUM_SAMPLES);
	r.alpha_to_mask_num_samples  = LUMEN_PM4_GET(buffer[0], DB_EQAA, ALPHA_TO_MASK_NUM_SAMPLES);
	r.high_quality_intersections = LUMEN_PM4_GET(buffer[0], DB_EQAA, HIGH_QUALITY_INTERSECTIONS) != 0;
	r.incoherent_eqaa_reads      = LUMEN_PM4_GET(buffer[0], DB_EQAA, INCOHERENT_EQAA_READS) != 0;
	r.interpolate_comp_z         = LUMEN_PM4_GET(buffer[0], DB_EQAA, INTERPOLATE_COMP_Z) != 0;
	r.static_anchor_associations = LUMEN_PM4_GET(buffer[0], DB_EQAA, STATIC_ANCHOR_ASSOCIATIONS) != 0;

	cp->GetCtx()->SetEqaaControl(r);

	return 1;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_generic_scissor)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0026900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::PA_SC_GENERIC_SCISSOR_TL);

	int left   = static_cast<int16_t>(static_cast<uint16_t>(LUMEN_PM4_GET(buffer[0], PA_SC_GENERIC_SCISSOR_TL, TL_X)));
	int top    = static_cast<int16_t>(static_cast<uint16_t>(LUMEN_PM4_GET(buffer[0], PA_SC_GENERIC_SCISSOR_TL, TL_Y)));
	int right  = static_cast<int16_t>(static_cast<uint16_t>(LUMEN_PM4_GET(buffer[1], PA_SC_GENERIC_SCISSOR_BR, BR_X)));
	int bottom = static_cast<int16_t>(static_cast<uint16_t>(LUMEN_PM4_GET(buffer[1], PA_SC_GENERIC_SCISSOR_BR, BR_Y)));

	bool window_offset_disable = LUMEN_PM4_GET(buffer[0], PA_SC_GENERIC_SCISSOR_TL, WINDOW_OFFSET_DISABLE) != 0;

	cp->GetCtx()->SetGenericScissor(left, top, right, bottom, !window_offset_disable);

	return 2;
}

static bool hw_ctx_write_viewport_scissor(HW::Context* ctx, uint32_t cmd_offset, uint32_t value)
{
	if (ctx == nullptr || cmd_offset < Pm4::PA_SC_VPORT_SCISSOR_0_TL || cmd_offset > Pm4::PA_SC_VPORT_SCISSOR_15_BR)
		return false;
	const uint32_t index = cmd_offset - Pm4::PA_SC_VPORT_SCISSOR_0_TL;
	if ((index & 1u) == 0)
	{
		const int left = static_cast<int16_t>(static_cast<uint16_t>(LUMEN_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_TL, TL_X)));
		const int top = static_cast<int16_t>(static_cast<uint16_t>(LUMEN_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_TL, TL_Y)));
		const bool window_offset_disable = LUMEN_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_TL, WINDOW_OFFSET_DISABLE) != 0;
		ctx->SetViewportScissorTL(index / 2u, left, top, !window_offset_disable);
	} else
	{
		const int right = static_cast<int16_t>(static_cast<uint16_t>(LUMEN_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_BR, BR_X)));
		const int bottom = static_cast<int16_t>(static_cast<uint16_t>(LUMEN_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_BR, BR_Y)));
		ctx->SetViewportScissorBR(index / 2u, right, bottom);
	}
	return true;
}

static uint32_t hw_ctx_update_viewport_scissor(HW::Context* ctx, uint32_t cmd_id, uint32_t cmd_offset,
                                              const uint32_t* buffer, uint32_t dw)
{
	const uint32_t packet_dw = LUMEN_PM4_LEN(cmd_id);
	if (ctx == nullptr || buffer == nullptr || packet_dw < 3 || packet_dw > dw ||
	    cmd_id != LUMEN_PM4(packet_dw, Pm4::IT_SET_CONTEXT_REG, 0) ||
	    cmd_offset < Pm4::PA_SC_VPORT_SCISSOR_0_TL || cmd_offset > Pm4::PA_SC_VPORT_SCISSOR_15_BR)
		return 0;
	const uint32_t count = packet_dw - 2u;
	if (count > Pm4::PA_SC_VPORT_SCISSOR_15_BR - cmd_offset + 1u) return 0;
	// Validate the entire packet before modifying either half of any viewport.
	for (uint32_t index = 0; index < count; ++index)
		EXIT_IF(!hw_ctx_write_viewport_scissor(ctx, cmd_offset + index, buffer[index]));
	return count;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_viewport_scissor)
{
	const auto count = hw_ctx_update_viewport_scissor(cp->GetCtx(), cmd_id, cmd_offset, buffer, dw);
	EXIT_NOT_IMPLEMENTED(count == 0);
	return count;
}

static uint32_t hw_ctx_update_guard_bands(HW::Context* ctx, uint32_t cmd_id, uint32_t cmd_offset, const uint32_t* buffer,
                                          uint32_t dw)
{
	const uint32_t packet_dw = LUMEN_PM4_LEN(cmd_id);
	const uint32_t count     = packet_dw - 2u;
	if (ctx == nullptr || buffer == nullptr || cmd_id != LUMEN_PM4(packet_dw, Pm4::IT_SET_CONTEXT_REG, 0) ||
	    packet_dw > dw || count == 0 || count > 4 || cmd_offset < Pm4::PA_CL_GB_VERT_CLIP_ADJ ||
	    cmd_offset > Pm4::PA_CL_GB_HORZ_DISC_ADJ || count > Pm4::PA_CL_GB_HORZ_DISC_ADJ - cmd_offset + 1u)
	{
		return 0;
	}

	const auto& viewport = ctx->GetScreenViewport();
	float values[] = {viewport.guard_band_vert_clip, viewport.guard_band_vert_discard,
	                  viewport.guard_band_horz_clip, viewport.guard_band_horz_discard};
	const uint32_t first = cmd_offset - Pm4::PA_CL_GB_VERT_CLIP_ADJ;
	static_assert(sizeof(float) == sizeof(uint32_t));
	for (uint32_t i = 0; i < count; ++i)
	{
		std::memcpy(&values[first + i], buffer + i, sizeof(float));
	}

	ctx->SetGuardBands(values[2], values[0], values[3], values[1]);
	return count;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_guard_bands)
{
	const auto count = hw_ctx_update_guard_bands(cp->GetCtx(), cmd_id, cmd_offset, buffer, dw);
	EXIT_NOT_IMPLEMENTED(count == 0);
	return count;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_hardware_screen_offset)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::PA_SU_HARDWARE_SCREEN_OFFSET);

	// uint32_t x = static_cast<uint16_t>(buffer[0] & 0xffffu);
	// uint32_t y = static_cast<uint16_t>((buffer[0] >> 16u) & 0xffffu);

	uint32_t x = LUMEN_PM4_GET(buffer[0], PA_SU_HARDWARE_SCREEN_OFFSET, HW_SCREEN_OFFSET_X);
	uint32_t y = LUMEN_PM4_GET(buffer[0], PA_SU_HARDWARE_SCREEN_OFFSET, HW_SCREEN_OFFSET_Y);

	cp->GetCtx()->SetHardwareScreenOffset(x, y);

	return 1;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_line_control)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::PA_SU_LINE_CNTL);

	auto line_width = LUMEN_PM4_GET(buffer[0], PA_SU_LINE_CNTL, WIDTH);

	if (line_width == 8)
	{
		cp->GetCtx()->SetLineWidth(1.0f);
	} else
	{
		cp->GetCtx()->SetLineWidth(static_cast<float>(line_width) / 8.0f);
	}

	return 1;
}

static void hw_ctx_observe_mode_control(HW::Context* ctx, uint32_t first_register, const uint32_t* values, uint32_t count)
{
	if (ctx == nullptr || values == nullptr || count == 0) return;
	if (first_register <= Pm4::PA_SU_SC_MODE_CNTL && Pm4::PA_SU_SC_MODE_CNTL - first_register < count)
	{
		ctx->ObservePolygonOffsetModeControl(values[Pm4::PA_SU_SC_MODE_CNTL - first_register]);
	}
}

static uint32_t hw_ctx_update_polygon_offset(HW::Context* ctx, uint32_t cmd_id, uint32_t cmd_offset,
                                           const uint32_t* values, uint32_t dw)
{
	const uint32_t packet_dw = LUMEN_PM4_LEN(cmd_id);
	if (ctx == nullptr || values == nullptr || packet_dw < 3 || packet_dw > dw ||
	    cmd_id != LUMEN_PM4(packet_dw, Pm4::IT_SET_CONTEXT_REG, 0) ||
	    cmd_offset < Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL || cmd_offset > Pm4::PA_SU_POLY_OFFSET_BACK_OFFSET) return 0;
	const uint32_t count = packet_dw - 2;
	if (count > Pm4::PA_SU_POLY_OFFSET_BACK_OFFSET - cmd_offset + 1) return 0;
	const uint32_t first = cmd_offset - Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL;
	for (uint32_t index = 0; index < count; ++index) ctx->SetPolygonOffsetRegister(first + index, values[index]);
	return count;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_polygon_offset)
{
	const uint32_t count = hw_ctx_update_polygon_offset(cp->GetCtx(), cmd_id, cmd_offset, buffer, dw);
	EXIT_NOT_IMPLEMENTED(count == 0);
	return count;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_mode_control)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::PA_SU_SC_MODE_CNTL);

	HW::ModeControl r;

	r.cull_front               = LUMEN_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, CULL_FRONT) != 0;
	r.cull_back                = LUMEN_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, CULL_BACK) != 0;
	r.face                     = LUMEN_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, FACE) != 0;
	r.poly_mode                = LUMEN_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, POLY_MODE);
	r.polymode_front_ptype     = LUMEN_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, POLYMODE_FRONT_PTYPE);
	r.polymode_back_ptype      = LUMEN_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, POLYMODE_BACK_PTYPE);
	r.poly_offset_front_enable = LUMEN_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, POLY_OFFSET_FRONT_ENABLE) != 0;
	r.poly_offset_back_enable  = LUMEN_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, POLY_OFFSET_BACK_ENABLE) != 0;
	r.vtx_window_offset_enable = LUMEN_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, VTX_WINDOW_OFFSET_ENABLE) != 0;
	r.provoking_vtx_last       = LUMEN_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, PROVOKING_VTX_LAST) != 0;
	r.persp_corr_dis           = LUMEN_PM4_GET(buffer[0], PA_SU_SC_MODE_CNTL, PERSP_CORR_DIS) != 0;

	cp->GetCtx()->SetModeControl(r);

	return 1;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_ps_input)
{
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::SPI_PS_INPUT_CNTL_0);

	uint32_t count = (cmd_id >> 16u) & 0x3fffu;

	EXIT_NOT_IMPLEMENTED(count == 0);
	EXIT_NOT_IMPLEMENTED(count > 32);

	for (uint32_t i = 0; i < count; i++)
	{
		cp->GetCtx()->SetPsInputSettings(i, buffer[i]);
	}

	return count;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_render_control)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::DB_RENDER_CONTROL);

	HW::RenderControl r;

	r.depth_clear_enable       = LUMEN_PM4_GET(buffer[0], DB_RENDER_CONTROL, DEPTH_CLEAR_ENABLE) != 0;
	r.stencil_clear_enable     = LUMEN_PM4_GET(buffer[0], DB_RENDER_CONTROL, STENCIL_CLEAR_ENABLE) != 0;
	r.resummarize_enable       = LUMEN_PM4_GET(buffer[0], DB_RENDER_CONTROL, RESUMMARIZE_ENABLE) != 0;
	r.stencil_compress_disable = LUMEN_PM4_GET(buffer[0], DB_RENDER_CONTROL, STENCIL_COMPRESS_DISABLE) != 0;
	r.depth_compress_disable   = LUMEN_PM4_GET(buffer[0], DB_RENDER_CONTROL, DEPTH_COMPRESS_DISABLE) != 0;
	r.copy_centroid            = LUMEN_PM4_GET(buffer[0], DB_RENDER_CONTROL, COPY_CENTROID) != 0;
	r.copy_sample              = LUMEN_PM4_GET(buffer[0], DB_RENDER_CONTROL, COPY_SAMPLE);
	r.raw                      = buffer[0];

	cp->GetCtx()->SetRenderControl(r);

	return 1;
}

static uint32_t hw_ctx_update_render_target(HW::Context* ctx, uint32_t cmd_id, uint32_t cmd_offset, const uint32_t* buffer,
                                             uint32_t dw)
{
	const uint32_t packet_dw = LUMEN_PM4_LEN(cmd_id);
	const uint32_t count     = packet_dw - 2u;
	if (ctx == nullptr || buffer == nullptr || cmd_id != LUMEN_PM4(packet_dw, Pm4::IT_SET_CONTEXT_REG, 0) ||
	    packet_dw > dw || count == 0 || count > 14 || cmd_offset < Pm4::CB_COLOR0_BASE ||
	    cmd_offset >= Pm4::CB_COLOR0_BASE + 8u * 15u)
	{
		return 0;
	}
	const uint32_t slot  = (cmd_offset - Pm4::CB_COLOR0_BASE) / 15u;
	const uint32_t first = (cmd_offset - Pm4::CB_COLOR0_BASE) % 15u;
	if (first >= 14 || count > 14u - first)
	{
		return 0;
	}

	bool has_size = false;
	HW::ColorSize size;
	if ((first + count == 11 || first + count == 14) && dw > packet_dw && buffer[count] == 0xC0001000u)
	{
		if (dw - packet_dw < 2)
		{
			return 0;
		}
		size.width  = buffer[count + 1] & 0xffffu;
		size.height = buffer[count + 1] >> 16u;
		has_size    = true;
	}

	for (uint32_t i = 0; i < count; ++i)
	{
		const uint32_t value = buffer[i];
		switch (first + i)
		{
			case 0: ctx->SetColorBase(slot, {static_cast<uint64_t>(value) << 8u}); break;
			case 1: ctx->SetColorPitch(slot, {value & 0x7ffu, (value >> 20u) & 0x7ffu}); break;
			case 2: ctx->SetColorSlice(slot, {value & 0x3fffffu}); break;
			case 3:
			{
				HW::ColorView view;
				view.base_array_slice_index = LUMEN_PM4_GET(value, CB_COLOR0_VIEW, SLICE_START);
				view.last_array_slice_index = LUMEN_PM4_GET(value, CB_COLOR0_VIEW, SLICE_MAX);
				view.current_mip_level      = LUMEN_PM4_GET(value, CB_COLOR0_VIEW, MIP_LEVEL);
				ctx->SetColorView(slot, view);
				break;
			}
			case 4:
			{
				HW::ColorInfo info;
				info.format                         = LUMEN_PM4_GET(value, CB_COLOR0_INFO, FORMAT);
				info.channel_type                   = LUMEN_PM4_GET(value, CB_COLOR0_INFO, NUMBER_TYPE);
				info.channel_order                  = LUMEN_PM4_GET(value, CB_COLOR0_INFO, COMP_SWAP);
				info.cmask_fast_clear_enable        = LUMEN_PM4_GET(value, CB_COLOR0_INFO, FAST_CLEAR) != 0;
				info.fmask_compression_enable       = LUMEN_PM4_GET(value, CB_COLOR0_INFO, COMPRESSION) != 0;
				info.blend_clamp                    = LUMEN_PM4_GET(value, CB_COLOR0_INFO, BLEND_CLAMP) != 0;
				info.blend_bypass                   = LUMEN_PM4_GET(value, CB_COLOR0_INFO, BLEND_BYPASS) != 0;
				info.round_mode                     = LUMEN_PM4_GET(value, CB_COLOR0_INFO, ROUND_MODE) != 0;
				info.cmask_tile_mode                = LUMEN_PM4_GET(value, CB_COLOR0_INFO, CMASK_IS_LINEAR);
				info.fmask_data_compression_disable = LUMEN_PM4_GET(value, CB_COLOR0_INFO, FMASK_COMPRESSION_DISABLE) != 0;
				info.fmask_one_frag_mode            = LUMEN_PM4_GET(value, CB_COLOR0_INFO, FMASK_COMPRESS_1FRAG_ONLY) != 0;
				info.dcc_compression_enable         = LUMEN_PM4_GET(value, CB_COLOR0_INFO, DCC_ENABLE) != 0;
				info.cmask_tile_mode_neo            = LUMEN_PM4_GET(value, CB_COLOR0_INFO, CMASK_ADDR_TYPE);
				info.neo_mode                       = LUMEN_PM4_GET(value, CB_COLOR0_INFO, ALT_TILE_MODE) != 0;
				ctx->SetColorInfo(slot, info);
				break;
			}
			case 5:
			{
				HW::ColorAttrib attrib;
				attrib.force_dest_alpha_to_one = LUMEN_PM4_GET(value, CB_COLOR0_ATTRIB, FORCE_DST_ALPHA_1) != 0;
				attrib.tile_mode               = LUMEN_PM4_GET(value, CB_COLOR0_ATTRIB, TILE_MODE_INDEX);
				attrib.fmask_tile_mode         = LUMEN_PM4_GET(value, CB_COLOR0_ATTRIB, FMASK_TILE_MODE_INDEX);
				attrib.num_samples             = LUMEN_PM4_GET(value, CB_COLOR0_ATTRIB, NUM_SAMPLES);
				attrib.num_fragments           = LUMEN_PM4_GET(value, CB_COLOR0_ATTRIB, NUM_FRAGMENTS);
				ctx->SetColorAttrib(slot, attrib);
				break;
			}
			case 6:
			{
				HW::ColorDccControl dcc;
				dcc.overwrite_combiner_disable    = LUMEN_PM4_GET(value, CB_COLOR0_DCC_CONTROL, OVERWRITE_COMBINER_DISABLE) != 0;
				dcc.dcc_clear_key_enable          = LUMEN_PM4_GET(value, CB_COLOR0_DCC_CONTROL, KEY_CLEAR_ENABLE) != 0;
				dcc.max_uncompressed_block_size   = LUMEN_PM4_GET(value, CB_COLOR0_DCC_CONTROL, MAX_UNCOMPRESSED_BLOCK_SIZE);
				dcc.min_compressed_block_size     = LUMEN_PM4_GET(value, CB_COLOR0_DCC_CONTROL, MIN_COMPRESSED_BLOCK_SIZE);
				dcc.max_compressed_block_size     = LUMEN_PM4_GET(value, CB_COLOR0_DCC_CONTROL, MAX_COMPRESSED_BLOCK_SIZE);
				dcc.color_transform              = LUMEN_PM4_GET(value, CB_COLOR0_DCC_CONTROL, COLOR_TRANSFORM);
				dcc.independent_64b_blocks        = LUMEN_PM4_GET(value, CB_COLOR0_DCC_CONTROL, INDEPENDENT_64B_BLOCKS) != 0;
				dcc.data_write_on_dcc_clear_to_reg = LUMEN_PM4_GET(value, CB_COLOR0_DCC_CONTROL, ENABLE_CONSTANT_ENCODE_REG_WRITE) != 0;
				dcc.independent_128b_blocks       = LUMEN_PM4_GET(value, CB_COLOR0_DCC_CONTROL, INDEPENDENT_128B_BLOCKS) != 0;
				ctx->SetColorDccControl(slot, dcc);
				break;
			}
			case 7: ctx->SetColorCmask(slot, {static_cast<uint64_t>(value) << 8u}); break;
			case 8: ctx->SetColorCmaskSlice(slot, {value & 0x3fffu}); break;
			case 9: ctx->SetColorFmask(slot, {static_cast<uint64_t>(value) << 8u}); break;
			case 10: ctx->SetColorFmaskSlice(slot, {value & 0x3fffffu}); break;
			case 11: ctx->SetColorClearWord0(slot, {value}); break;
			case 12: ctx->SetColorClearWord1(slot, {value}); break;
			case 13: ctx->SetColorDccAddr(slot, {static_cast<uint64_t>(value) << 8u}); break;
		}
	}
	if (has_size)
	{
		ctx->SetColorSize(slot, size);
	}
	return count + (has_size ? 2u : 0u);
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_render_target)
{
	const auto count = hw_ctx_update_render_target(cp->GetCtx(), cmd_id, cmd_offset, buffer, dw);
	EXIT_NOT_IMPLEMENTED(count == 0);
	return count;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_render_target_mask)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::CB_TARGET_MASK);

	cp->GetCtx()->SetRenderTargetMask(*buffer);

	return 1;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_scan_mode_control)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::PA_SC_MODE_CNTL_0);

	HW::ScanModeControl r;

	r.msaa_enable          = LUMEN_PM4_GET(buffer[0], PA_SC_MODE_CNTL_0, MSAA_ENABLE) != 0;
	r.vport_scissor_enable = LUMEN_PM4_GET(buffer[0], PA_SC_MODE_CNTL_0, VPORT_SCISSOR_ENABLE) != 0;
	r.line_stipple_enable  = LUMEN_PM4_GET(buffer[0], PA_SC_MODE_CNTL_0, LINE_STIPPLE_ENABLE) != 0;

	cp->GetCtx()->SetScanModeControl(r);

	return 1;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_screen_scissor)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0026900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::PA_SC_SCREEN_SCISSOR_TL);

	int left   = static_cast<int16_t>(static_cast<uint16_t>(LUMEN_PM4_GET(buffer[0], PA_SC_SCREEN_SCISSOR_TL, TL_X)));
	int top    = static_cast<int16_t>(static_cast<uint16_t>(LUMEN_PM4_GET(buffer[0], PA_SC_SCREEN_SCISSOR_TL, TL_Y)));
	int right  = static_cast<int16_t>(static_cast<uint16_t>(LUMEN_PM4_GET(buffer[1], PA_SC_SCREEN_SCISSOR_BR, BR_X)));
	int bottom = static_cast<int16_t>(static_cast<uint16_t>(LUMEN_PM4_GET(buffer[1], PA_SC_SCREEN_SCISSOR_BR, BR_Y)));

	cp->GetCtx()->SetScreenScissor(left, top, right, bottom);

	return 2;
}

static uint32_t hw_ctx_update_index_offset(HW::Context* ctx, uint32_t cmd_id, uint32_t cmd_offset, const uint32_t* buffer,
                                           uint32_t dw)
{
	if (ctx == nullptr || buffer == nullptr || cmd_id != LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0) ||
	    cmd_offset != Pm4::VGT_INDX_OFFSET || dw < 3)
	{
		return 0;
	}

	ctx->SetIndexOffset(buffer[0]);
	return 1;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_index_offset)
{
	const auto count = hw_ctx_update_index_offset(cp->GetCtx(), cmd_id, cmd_offset, buffer, dw);
	EXIT_NOT_IMPLEMENTED(count == 0);
	return count;
}

static uint32_t hw_ctx_update_geometry_streamout_state(HW::Context* ctx, uint32_t cmd_id, uint32_t cmd_offset,
                                                       const uint32_t* buffer, uint32_t dw)
{
	if (ctx == nullptr || buffer == nullptr || cmd_id != LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0) || dw < 3)
	{
		return 0;
	}

	switch (cmd_offset)
	{
		case Pm4::VGT_GS_MODE: ctx->SetGsMode(buffer[0]); break;
		case Pm4::VGT_STRMOUT_CONFIG: ctx->SetStreamoutConfig(buffer[0]); break;
		default: return 0;
	}
	return 1;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_geometry_streamout_state)
{
	const auto count = hw_ctx_update_geometry_streamout_state(cp->GetCtx(), cmd_id, cmd_offset, buffer, dw);
	EXIT_NOT_IMPLEMENTED(count == 0);
	return count;
}

static uint32_t hw_ctx_update_instance_step_rate(HW::Context* ctx, uint32_t cmd_id, uint32_t cmd_offset,
                                                 const uint32_t* buffer, uint32_t dw)
{
	const uint32_t packet_dw = LUMEN_PM4_LEN(cmd_id);
	if (ctx == nullptr || buffer == nullptr || cmd_id != LUMEN_PM4(packet_dw, Pm4::IT_SET_CONTEXT_REG, 0) || packet_dw < 3 ||
	    dw < packet_dw || cmd_offset < Pm4::VGT_INSTANCE_STEP_RATE_0 || cmd_offset > Pm4::VGT_INSTANCE_STEP_RATE_1)
	{
		return 0;
	}

	const uint32_t values = packet_dw - 2u;
	if (values > Pm4::VGT_INSTANCE_STEP_RATE_1 - cmd_offset + 1u) return 0;

	for (uint32_t i = 0; i < values; ++i)
	{
		ctx->SetInstanceStepRate(cmd_offset - Pm4::VGT_INSTANCE_STEP_RATE_0 + i, buffer[i]);
	}
	return values;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_instance_step_rate)
{
	const auto count = hw_ctx_update_instance_step_rate(cp->GetCtx(), cmd_id, cmd_offset, buffer, dw);
	EXIT_NOT_IMPLEMENTED(count == 0);
	return count;
}

static bool hw_ctx_write_draw_control(HW::Context* ctx, uint32_t cmd_offset, uint32_t value)
{
	if (ctx == nullptr) return false;
	switch (cmd_offset)
	{
		case Pm4::DB_ALPHA_TO_MASK: ctx->SetAlphaToMask(value); break;
		case Pm4::IA_MULTI_VGT_PARAM: ctx->SetMultiVgtParam(value); break;
		case Pm4::PA_SC_MODE_CNTL_1: ctx->SetScanModeControl1(value); break;
		default: return false;
	}
	return true;
}

static uint32_t hw_ctx_update_draw_control(HW::Context* ctx, uint32_t cmd_id, uint32_t cmd_offset,
                                           const uint32_t* buffer, uint32_t dw)
{
	if (buffer == nullptr || cmd_id != LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0) || dw < 3) return 0;
	return hw_ctx_write_draw_control(ctx, cmd_offset, buffer[0]) ? 1u : 0u;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_draw_control)
{
	const auto count = hw_ctx_update_draw_control(cp->GetCtx(), cmd_id, cmd_offset, buffer, dw);
	EXIT_NOT_IMPLEMENTED(count == 0);
	return count;
}

static bool hw_ctx_write_aa_mask(HW::Context* ctx, uint32_t cmd_offset, uint32_t value)
{
	if (ctx == nullptr || cmd_offset < Pm4::PA_SC_AA_MASK_X0Y0_X1Y0 || cmd_offset > Pm4::PA_SC_AA_MASK_X0Y1_X1Y1) return false;
	ctx->SetAaMask(cmd_offset - Pm4::PA_SC_AA_MASK_X0Y0_X1Y0, value);
	return true;
}

static uint32_t hw_ctx_update_aa_mask(HW::Context* ctx, uint32_t cmd_id, uint32_t cmd_offset,
                                     const uint32_t* values, uint32_t dw)
{
	const uint32_t packet_dw = LUMEN_PM4_LEN(cmd_id);
	if (ctx == nullptr || values == nullptr || packet_dw < 3 || packet_dw > dw ||
	    cmd_id != LUMEN_PM4(packet_dw, Pm4::IT_SET_CONTEXT_REG, 0) ||
	    cmd_offset < Pm4::PA_SC_AA_MASK_X0Y0_X1Y0 || cmd_offset > Pm4::PA_SC_AA_MASK_X0Y1_X1Y1) return 0;
	const uint32_t count = packet_dw - 2;
	if (count > Pm4::PA_SC_AA_MASK_X0Y1_X1Y1 - cmd_offset + 1) return 0;
	for (uint32_t index = 0; index < count; ++index) hw_ctx_write_aa_mask(ctx, cmd_offset + index, values[index]);
	return count;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_aa_mask)
{
	const auto count = hw_ctx_update_aa_mask(cp->GetCtx(), cmd_id, cmd_offset, buffer, dw);
	EXIT_NOT_IMPLEMENTED(count == 0);
	return count;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_shader_stages)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::VGT_SHADER_STAGES_EN);

	cp->GetCtx()->SetShaderStages(buffer[0]);

	return 1;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_stencil_clear)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::DB_STENCIL_CLEAR);

	cp->GetCtx()->SetStencilClearValue(LUMEN_PM4_GET(buffer[0], DB_STENCIL_CLEAR, CLEAR));

	return 1;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_stencil_control)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::DB_STENCIL_CONTROL);

	HW::StencilControl r;

	r.stencil_fail     = LUMEN_PM4_GET(buffer[0], DB_STENCIL_CONTROL, STENCILFAIL);
	r.stencil_zpass    = LUMEN_PM4_GET(buffer[0], DB_STENCIL_CONTROL, STENCILZPASS);
	r.stencil_zfail    = LUMEN_PM4_GET(buffer[0], DB_STENCIL_CONTROL, STENCILZFAIL);
	r.stencil_fail_bf  = LUMEN_PM4_GET(buffer[0], DB_STENCIL_CONTROL, STENCILFAIL_BF);
	r.stencil_zpass_bf = LUMEN_PM4_GET(buffer[0], DB_STENCIL_CONTROL, STENCILZPASS_BF);
	r.stencil_zfail_bf = LUMEN_PM4_GET(buffer[0], DB_STENCIL_CONTROL, STENCILZFAIL_BF);

	cp->GetCtx()->SetStencilControl(r);

	return 1;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_stencil_info)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::DB_STENCIL_INFO);

	HW::DepthStencilInfo r;

	//	r.expclear_enabled = (buffer[0] & 0x08000000u) != 0;
	//	r.tile_split       = (buffer[0] >> 13u) & 0x7u;
	//	r.format          = (buffer[0] >> Pm4::DB_STENCIL_INFO_FORMAT_SHIFT) & Pm4::DB_STENCIL_INFO_FORMAT_MASK;
	//	r.tile_mode_index = (buffer[0] >> Pm4::DB_STENCIL_INFO_TILE_MODE_INDEX_SHIFT) & Pm4::DB_STENCIL_INFO_TILE_MODE_INDEX_MASK;
	//	r.tile_stencil_disable =
	//	    ((buffer[0] >> Pm4::DB_STENCIL_INFO_TILE_STENCIL_DISABLE_SHIFT) & Pm4::DB_STENCIL_INFO_TILE_STENCIL_DISABLE_MASK) != 0;

	r.format                     = LUMEN_PM4_GET(buffer[0], DB_STENCIL_INFO, FORMAT);
	r.texture_compatible_stencil = LUMEN_PM4_GET(buffer[0], DB_STENCIL_INFO, ITERATE_FLUSH) != 0;
	r.partially_resident         = LUMEN_PM4_GET(buffer[0], DB_STENCIL_INFO, PARTIALLY_RESIDENT) != 0;
	r.tile_split                 = LUMEN_PM4_GET(buffer[0], DB_STENCIL_INFO, RESERVED_FIELD_1);
	r.tile_mode_index            = LUMEN_PM4_GET(buffer[0], DB_STENCIL_INFO, TILE_MODE_INDEX);
	r.expclear_enabled           = LUMEN_PM4_GET(buffer[0], DB_STENCIL_INFO, ALLOW_EXPCLEAR) != 0;
	r.tile_stencil_disable       = LUMEN_PM4_GET(buffer[0], DB_STENCIL_INFO, TILE_STENCIL_DISABLE) != 0;
	r.unsupported_bits           = buffer[0] & ~depth_stencil_info_modeled_mask;

	cp->GetCtx()->SetDepthStencilInfo(r);

	return 1;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_stencil_mask)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0026900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::DB_STENCILREFMASK);

	HW::StencilMask r;

	r.stencil_testval      = LUMEN_PM4_GET(buffer[0], DB_STENCILREFMASK, STENCILTESTVAL);
	r.stencil_mask         = LUMEN_PM4_GET(buffer[0], DB_STENCILREFMASK, STENCILMASK);
	r.stencil_writemask    = LUMEN_PM4_GET(buffer[0], DB_STENCILREFMASK, STENCILWRITEMASK);
	r.stencil_opval        = LUMEN_PM4_GET(buffer[0], DB_STENCILREFMASK, STENCILOPVAL);
	r.stencil_testval_bf   = LUMEN_PM4_GET(buffer[1], DB_STENCILREFMASK_BF, STENCILTESTVAL_BF);
	r.stencil_mask_bf      = LUMEN_PM4_GET(buffer[1], DB_STENCILREFMASK_BF, STENCILMASK_BF);
	r.stencil_writemask_bf = LUMEN_PM4_GET(buffer[1], DB_STENCILREFMASK_BF, STENCILWRITEMASK_BF);
	r.stencil_opval_bf     = LUMEN_PM4_GET(buffer[1], DB_STENCILREFMASK_BF, STENCILOPVAL_BF);

	cp->GetCtx()->SetStencilMask(r);

	return 2;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_viewport_scale_offset)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0066900);

	uint32_t param = (cmd_offset - Pm4::PA_CL_VPORT_XSCALE) / 6;

	EXIT_NOT_IMPLEMENTED(param != 0);

	cp->GetCtx()->SetViewportScaleOffset(param, *reinterpret_cast<const float*>(buffer), *reinterpret_cast<const float*>(buffer + 1),
	                                     *reinterpret_cast<const float*>(buffer + 2), *reinterpret_cast<const float*>(buffer + 3),
	                                     *reinterpret_cast<const float*>(buffer + 4), *reinterpret_cast<const float*>(buffer + 5));

	return 6;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_viewport_transform_control)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0016900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::PA_CL_VTE_CNTL);

	cp->GetCtx()->SetViewportTransformControl(*buffer);

	return 1;
}

LUMEN_HW_CTX_PARSER(hw_ctx_set_viewport_z)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0026900);

	uint32_t param = (cmd_offset - Pm4::PA_SC_VPORT_ZMIN_0) / 2;

	EXIT_NOT_IMPLEMENTED(param != 0);

	cp->GetCtx()->SetViewportZ(param, *reinterpret_cast<const float*>(buffer), *reinterpret_cast<const float*>(buffer + 1));

	return 2;
}

LUMEN_HW_SH_PARSER(hw_sh_set_cs_shader)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC017101C);

	auto shader_modifier = buffer[0];

	HW::CsStageRegisters r {};

	r.data_addr      = (static_cast<uint64_t>(buffer[1]) << 8u) | (static_cast<uint64_t>(buffer[2]) << 40u);
	r.vgprs          = (buffer[3] >> Pm4::COMPUTE_PGM_RSRC1_VGPRS_SHIFT) & Pm4::COMPUTE_PGM_RSRC1_VGPRS_MASK;
	r.sgprs          = (buffer[3] >> Pm4::COMPUTE_PGM_RSRC1_SGPRS_SHIFT) & Pm4::COMPUTE_PGM_RSRC1_SGPRS_MASK;
	r.bulky          = (buffer[3] >> Pm4::COMPUTE_PGM_RSRC1_BULKY_SHIFT) & Pm4::COMPUTE_PGM_RSRC1_BULKY_MASK;
	r.scratch_en     = (buffer[4] >> Pm4::COMPUTE_PGM_RSRC2_SCRATCH_EN_SHIFT) & Pm4::COMPUTE_PGM_RSRC2_SCRATCH_EN_MASK;
	r.user_sgpr      = (buffer[4] >> Pm4::COMPUTE_PGM_RSRC2_USER_SGPR_SHIFT) & Pm4::COMPUTE_PGM_RSRC2_USER_SGPR_MASK;
	r.tgid_x_en      = (buffer[4] >> Pm4::COMPUTE_PGM_RSRC2_TGID_X_EN_SHIFT) & Pm4::COMPUTE_PGM_RSRC2_TGID_X_EN_MASK;
	r.tgid_y_en      = (buffer[4] >> Pm4::COMPUTE_PGM_RSRC2_TGID_Y_EN_SHIFT) & Pm4::COMPUTE_PGM_RSRC2_TGID_Y_EN_MASK;
	r.tgid_z_en      = (buffer[4] >> Pm4::COMPUTE_PGM_RSRC2_TGID_Z_EN_SHIFT) & Pm4::COMPUTE_PGM_RSRC2_TGID_Z_EN_MASK;
	r.tg_size_en     = (buffer[4] >> Pm4::COMPUTE_PGM_RSRC2_TG_SIZE_EN_SHIFT) & Pm4::COMPUTE_PGM_RSRC2_TG_SIZE_EN_MASK;
	r.tidig_comp_cnt = (buffer[4] >> Pm4::COMPUTE_PGM_RSRC2_TIDIG_COMP_CNT_SHIFT) & Pm4::COMPUTE_PGM_RSRC2_TIDIG_COMP_CNT_MASK;
	r.lds_size       = (buffer[4] >> Pm4::COMPUTE_PGM_RSRC2_LDS_SIZE_SHIFT) & Pm4::COMPUTE_PGM_RSRC2_LDS_SIZE_MASK;
	r.num_thread_x   = buffer[5];
	r.num_thread_y   = buffer[6];
	r.num_thread_z   = buffer[7];

	cp->GetShCtx()->SetCsShader(r, shader_modifier);

	return 24;
}

LUMEN_HW_SH_PARSER(hw_sh_set_cs_user_sgpr)
{
	EXIT_NOT_IMPLEMENTED(!(cmd_offset >= Pm4::COMPUTE_USER_DATA_0 && cmd_offset <= Pm4::COMPUTE_USER_DATA_15));

	uint32_t slot = (cmd_offset - Pm4::COMPUTE_USER_DATA_0) / 1;

	auto reg_num = (cmd_id >> 16u) & 0x3fffu;

	EXIT_NOT_IMPLEMENTED(reg_num >= HW::UserSgprInfo::SGPRS_MAX);
	EXIT_NOT_IMPLEMENTED(reg_num > HW::UserSgprInfo::SGPRS_MAX - slot);
	const auto marker = cp->GetUserDataMarker();

	if (command_provenance_recording())
	{
		for (uint32_t i = 0; i < reg_num; i++)
		{
			const uint32_t actual = buffer[i];
			cp->GetShCtx()->SetCsUserSgpr(slot + i, actual, marker);
			cp->RecordUserSgprWrite(ShaderUserStage::Compute, slot + i, &actual, 1, marker, cmd_id, cmd_offset, i);
		}
	} else
	{
		for (uint32_t i = 0; i < reg_num; i++)
		{
			cp->GetShCtx()->SetCsUserSgpr(slot + i, buffer[i], marker);
		}
	}
	cp->SetUserDataMarker(HW::UserSgprType::Unknown);

	return reg_num;
}

LUMEN_HW_SH_PARSER(hw_sh_set_ps_embedded)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0261038);

	auto id = buffer[0];

	cp->GetShCtx()->SetPsEmbedded(id);

	return 39;
}

LUMEN_HW_SH_PARSER(hw_sh_set_ps_shader)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0261008);

	// HW::PsStageRegisters r {};
	HW::PsShaderResource1 r1;
	HW::PsShaderResource2 r2;

	//	r.data_addr   = (static_cast<uint64_t>(buffer[0]) << 8u) | (static_cast<uint64_t>(buffer[1]) << 40u);
	//	r.vgprs       = (buffer[2] >> Pm4::SPI_SHADER_PGM_RSRC1_PS_VGPRS_SHIFT) & Pm4::SPI_SHADER_PGM_RSRC1_PS_VGPRS_MASK;
	//	r.sgprs       = (buffer[2] >> Pm4::SPI_SHADER_PGM_RSRC1_PS_SGPRS_SHIFT) & Pm4::SPI_SHADER_PGM_RSRC1_PS_SGPRS_MASK;
	//	r.scratch_en  = (buffer[3] >> Pm4::SPI_SHADER_PGM_RSRC2_PS_SCRATCH_EN_SHIFT) & Pm4::SPI_SHADER_PGM_RSRC2_PS_SCRATCH_EN_MASK;
	//	r.user_sgpr   = (buffer[3] >> Pm4::SPI_SHADER_PGM_RSRC2_PS_USER_SGPR_SHIFT) & Pm4::SPI_SHADER_PGM_RSRC2_PS_USER_SGPR_MASK;
	//	r.wave_cnt_en = (buffer[3] >> Pm4::SPI_SHADER_PGM_RSRC2_PS_WAVE_CNT_EN_SHIFT) & Pm4::SPI_SHADER_PGM_RSRC2_PS_WAVE_CNT_EN_MASK;

	uint64_t addr = (static_cast<uint64_t>(buffer[0]) << 8u) | (static_cast<uint64_t>(buffer[1] & 0xffu) << 40u);

	r1.vgprs                    = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, VGPRS);
	r1.sgprs                    = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, SGPRS);
	r1.priority                 = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, PRIORITY);
	r1.float_mode               = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, FLOAT_MODE);
	r1.dx10_clamp               = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, DX10_CLAMP) != 0;
	r1.debug_mode               = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, DEBUG_MODE) != 0;
	r1.ieee_mode                = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, IEEE_MODE) != 0;
	r1.cu_group_disable         = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, CU_GROUP_DISABLE) != 0;
	r1.require_forward_progress = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, FWD_PROGRESS) != 0;
	r1.fp16_overflow            = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, FP16_OVFL) != 0;

	r2.scratch_en = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, SCRATCH_EN);
	r2.user_sgpr  = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, USER_SGPR) +
	               (LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, USER_SGPR_MSB) << 5u);
	r2.wave_cnt_en            = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, WAVE_CNT_EN);
	r2.extra_lds_size         = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, EXTRA_LDS_SIZE);
	r2.raster_ordered_shading = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, LOAD_INTRAWAVE_COLLISION);
	r2.shared_vgprs           = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, SHARED_VGPR_CNT);

	cp->GetCtx()->SetShaderZFormat(buffer[4]);

	for (uint32_t i = 0; i < 8; i++)
	{
		cp->GetCtx()->SetTargetOutputMode(i, (buffer[5] >> (i * 4)) & 0xFu);
	}

	cp->GetCtx()->SetPsInputEna(buffer[6]);
	cp->GetCtx()->SetPsInputAddr(buffer[7]);
	cp->GetCtx()->SetPsInControl(buffer[8]);
	cp->GetCtx()->SetBarycCntl(buffer[9]);

	HW::DepthShaderControl db_shader_control {};

	//	db_shader_control.conservative_z_export_value = (buffer[10] >> 13u) & 0x3u;
	//	db_shader_control.shader_z_behavior           = (buffer[10] >> 4u) & 0x3u;
	//	db_shader_control.shader_kill_enable          = (buffer[10] & 0x40u) != 0;
	//	db_shader_control.shader_z_export_enable      = (buffer[10] & 0x1u) != 0;
	//	db_shader_control.shader_execute_on_noop      = (buffer[10] & 0x400u) != 0;
	db_shader_control.other_bits                  = buffer[10] & 0xFFFF9B8Eu;
	db_shader_control.conservative_z_export_value = LUMEN_PM4_GET(buffer[10], DB_SHADER_CONTROL, CONSERVATIVE_Z_EXPORT);
	db_shader_control.shader_z_behavior           = LUMEN_PM4_GET(buffer[10], DB_SHADER_CONTROL, Z_ORDER);
	db_shader_control.shader_kill_enable          = LUMEN_PM4_GET(buffer[10], DB_SHADER_CONTROL, KILL_ENABLE) != 0;
	db_shader_control.shader_z_export_enable      = LUMEN_PM4_GET(buffer[10], DB_SHADER_CONTROL, Z_EXPORT_ENABLE) != 0;
	db_shader_control.shader_execute_on_noop      = LUMEN_PM4_GET(buffer[10], DB_SHADER_CONTROL, EXEC_ON_NOOP) != 0;

	cp->GetCtx()->SetDepthShaderControl(db_shader_control);
	cp->GetCtx()->SetShaderMask(buffer[11]);

	// cp->GetShCtx()->SetPsShader(r);
	cp->GetShCtx()->SetPsShaderBase(addr);
	cp->GetShCtx()->SetPsShaderResource1(r1);
	cp->GetShCtx()->SetPsShaderResource2(r2);

	return 39;
}

LUMEN_HW_SH_PARSER(hw_sh_set_ps_user_sgpr)
{
	EXIT_NOT_IMPLEMENTED(!(cmd_offset >= Pm4::SPI_SHADER_USER_DATA_PS_0 && cmd_offset <= Pm4::SPI_SHADER_USER_DATA_PS_15));

	uint32_t slot = (cmd_offset - Pm4::SPI_SHADER_USER_DATA_PS_0) / 1;

	auto reg_num = (cmd_id >> 16u) & 0x3fffu;

	EXIT_NOT_IMPLEMENTED(reg_num >= HW::UserSgprInfo::SGPRS_MAX);
	EXIT_NOT_IMPLEMENTED(reg_num > HW::UserSgprInfo::SGPRS_MAX - slot);
	const auto marker = cp->GetUserDataMarker();

	if (command_provenance_recording())
	{
		for (uint32_t i = 0; i < reg_num; i++)
		{
			const uint32_t actual = buffer[i];
			cp->GetShCtx()->SetPsUserSgpr(slot + i, actual, marker);
			cp->RecordUserSgprWrite(ShaderUserStage::Pixel, slot + i, &actual, 1, marker, cmd_id, cmd_offset, i);
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
			if (i == 3 && g_graphics_state_test_pause_requested.load(std::memory_order_relaxed))
			{
				g_graphics_state_test_pause_reached.store(true, std::memory_order_release);
				while (!g_graphics_state_test_pause_release.load(std::memory_order_acquire))
				{
					std::this_thread::yield();
				}
			}
#endif
		}
	} else
	{
		for (uint32_t i = 0; i < reg_num; i++)
		{
			cp->GetShCtx()->SetPsUserSgpr(slot + i, buffer[i], marker);
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
			if (i == 3 && g_graphics_state_test_pause_requested.load(std::memory_order_relaxed))
			{
				g_graphics_state_test_pause_reached.store(true, std::memory_order_release);
				while (!g_graphics_state_test_pause_release.load(std::memory_order_acquire))
				{
					std::this_thread::yield();
				}
			}
#endif
		}
	}
	cp->SetUserDataMarker(HW::UserSgprType::Unknown);

	return reg_num;
}

LUMEN_HW_SH_PARSER(hw_sh_set_vs_embedded)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc01b1034);

	auto shader_modifier = buffer[0];
	auto id              = buffer[1];

	cp->GetShCtx()->SetVsEmbedded(id, shader_modifier);

	return 28;
}

LUMEN_HW_SH_PARSER(hw_sh_set_vs_shader)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC01B1004);

	auto shader_modifier = buffer[0];

	// HW::VsStageRegisters r {};

	uint64_t addr = (static_cast<uint64_t>(buffer[1]) << 8u) | (static_cast<uint64_t>(buffer[2] & 0xffu) << 40u);

	HW::VsShaderResource1 r1;
	HW::VsShaderResource2 r2;

	// r.m_spiShaderPgmLoVs    = buffer[1];
	// r.m_spiShaderPgmHiVs    = buffer[2];
	// r.m_spiShaderPgmRsrc1Vs = buffer[3];
	// r.m_spiShaderPgmRsrc2Vs = buffer[4];

	r1.vgprs                    = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, VGPRS);
	r1.sgprs                    = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, SGPRS);
	r1.priority                 = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, PRIORITY);
	r1.float_mode               = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, FLOAT_MODE);
	r1.dx10_clamp               = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, DX10_CLAMP) != 0;
	r1.ieee_mode                = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, IEEE_MODE) != 0;
	r1.vgpr_component_count     = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT);
	r1.cu_group_enable          = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, CU_GROUP_ENABLE) != 0;
	r1.require_forward_progress = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, FWD_PROGRESS) != 0;
	r1.fp16_overflow            = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, FP16_OVFL) != 0;

	r2.scratch_en = LUMEN_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, SCRATCH_EN) != 0;
	r2.user_sgpr  = LUMEN_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, USER_SGPR) +
	               (LUMEN_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, USER_SGPR_MSB) << 5u);
	r2.offchip_lds       = LUMEN_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, OC_LDS_EN) != 0;
	r2.streamout_enabled = LUMEN_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, SO_EN) != 0;
	r2.shared_vgprs      = LUMEN_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, SHARED_VGPR_CNT);

	uint32_t m_spi_vs_out_config     = buffer[5];
	uint32_t m_spi_shader_pos_format = buffer[6];
	uint32_t m_pa_cl_vs_out_cntl     = buffer[7];

	// cp->GetShCtx()->SetVsShader(r, shader_modifier);
	cp->GetShCtx()->SetVsShaderBase(addr);
	cp->GetShCtx()->SetVsShaderModifier(shader_modifier);
	cp->GetShCtx()->SetVsShaderResource1(r1);
	cp->GetShCtx()->SetVsShaderResource2(r2);

	cp->GetCtx()->SetVsOutConfig(m_spi_vs_out_config);
	cp->GetCtx()->SetShaderPosFormat(m_spi_shader_pos_format);
	cp->GetCtx()->SetClVsOutCntl(m_pa_cl_vs_out_cntl);

	return 28;
}

LUMEN_HW_SH_PARSER(hw_sh_set_vs_user_sgpr)
{
	EXIT_NOT_IMPLEMENTED(!(cmd_offset >= Pm4::SPI_SHADER_USER_DATA_VS_0 && cmd_offset <= Pm4::SPI_SHADER_USER_DATA_VS_15));

	uint32_t slot = (cmd_offset - Pm4::SPI_SHADER_USER_DATA_VS_0) / 1;

	auto reg_num = (cmd_id >> 16u) & 0x3fffu;

	EXIT_NOT_IMPLEMENTED(reg_num >= HW::UserSgprInfo::SGPRS_MAX);
	EXIT_NOT_IMPLEMENTED(reg_num > HW::UserSgprInfo::SGPRS_MAX - slot);
	const auto marker = cp->GetUserDataMarker();

	if (command_provenance_recording())
	{
		for (uint32_t i = 0; i < reg_num; i++)
		{
			const uint32_t actual = buffer[i];
			cp->GetShCtx()->SetVsUserSgpr(slot + i, actual, marker);
			cp->RecordUserSgprWrite(ShaderUserStage::Vertex, slot + i, &actual, 1, marker, cmd_id, cmd_offset, i);
		}
	} else
	{
		for (uint32_t i = 0; i < reg_num; i++)
		{
			cp->GetShCtx()->SetVsUserSgpr(slot + i, buffer[i], marker);
		}
	}
	cp->SetUserDataMarker(HW::UserSgprType::Unknown);

	return reg_num;
}

LUMEN_HW_SH_PARSER(hw_sh_set_gs_user_sgpr)
{
	EXIT_NOT_IMPLEMENTED(!(cmd_offset >= Pm4::SPI_SHADER_USER_DATA_GS_0 && cmd_offset <= Pm4::SPI_SHADER_USER_DATA_GS_15));

	uint32_t slot = (cmd_offset - Pm4::SPI_SHADER_USER_DATA_GS_0) / 1;

	auto reg_num = (cmd_id >> 16u) & 0x3fffu;

	EXIT_NOT_IMPLEMENTED(reg_num >= HW::UserSgprInfo::SGPRS_MAX);
	EXIT_NOT_IMPLEMENTED(reg_num > HW::UserSgprInfo::SGPRS_MAX - slot);
	const auto marker = cp->GetUserDataMarker();

	if (command_provenance_recording())
	{
		for (uint32_t i = 0; i < reg_num; i++)
		{
			const uint32_t actual = buffer[i];
			cp->GetShCtx()->SetGsUserSgpr(slot + i, actual, marker);
			cp->RecordUserSgprWrite(ShaderUserStage::Geometry, slot + i, &actual, 1, marker, cmd_id, cmd_offset, i);
		}
	} else
	{
		for (uint32_t i = 0; i < reg_num; i++)
		{
			cp->GetShCtx()->SetGsUserSgpr(slot + i, buffer[i], marker);
		}
	}
	cp->SetUserDataMarker(HW::UserSgprType::Unknown);

	return reg_num;
}

LUMEN_HW_SH_PARSER(hw_sh_update_ps_shader)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0261040);

	HW::PsShaderResource1 r1;
	HW::PsShaderResource2 r2;

	uint64_t addr = (static_cast<uint64_t>(buffer[0]) << 8u) | (static_cast<uint64_t>(buffer[1] & 0xffu) << 40u);

	r1.vgprs                    = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, VGPRS);
	r1.sgprs                    = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, SGPRS);
	r1.priority                 = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, PRIORITY);
	r1.float_mode               = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, FLOAT_MODE);
	r1.dx10_clamp               = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, DX10_CLAMP) != 0;
	r1.debug_mode               = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, DEBUG_MODE) != 0;
	r1.ieee_mode                = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, IEEE_MODE) != 0;
	r1.cu_group_disable         = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, CU_GROUP_DISABLE) != 0;
	r1.require_forward_progress = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, FWD_PROGRESS) != 0;
	r1.fp16_overflow            = LUMEN_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, FP16_OVFL) != 0;

	r2.scratch_en = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, SCRATCH_EN);
	r2.user_sgpr  = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, USER_SGPR) +
	               (LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, USER_SGPR_MSB) << 5u);
	r2.wave_cnt_en            = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, WAVE_CNT_EN);
	r2.extra_lds_size         = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, EXTRA_LDS_SIZE);
	r2.raster_ordered_shading = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, LOAD_INTRAWAVE_COLLISION);
	r2.shared_vgprs           = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, SHARED_VGPR_CNT);

	cp->GetCtx()->SetShaderZFormat(buffer[4]);

	// cp->GetShCtx()->UpdatePsShader(r);
	cp->GetShCtx()->SetPsShaderBase(addr);
	cp->GetShCtx()->SetPsShaderResource1(r1);
	cp->GetShCtx()->SetPsShaderResource2(r2);

	return 39;
}

LUMEN_HW_SH_PARSER(hw_sh_update_vs_shader)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc01b103c);

	auto shader_modifier = buffer[0];

	uint64_t addr = (static_cast<uint64_t>(buffer[1]) << 8u) | (static_cast<uint64_t>(buffer[2] & 0xffu) << 40u);

	HW::VsShaderResource1 r1;
	HW::VsShaderResource2 r2;

	r1.vgprs                    = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, VGPRS);
	r1.sgprs                    = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, SGPRS);
	r1.priority                 = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, PRIORITY);
	r1.float_mode               = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, FLOAT_MODE);
	r1.dx10_clamp               = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, DX10_CLAMP) != 0;
	r1.ieee_mode                = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, IEEE_MODE) != 0;
	r1.vgpr_component_count     = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT);
	r1.cu_group_enable          = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, CU_GROUP_ENABLE) != 0;
	r1.require_forward_progress = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, FWD_PROGRESS) != 0;
	r1.fp16_overflow            = LUMEN_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, FP16_OVFL) != 0;

	r2.scratch_en = LUMEN_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, SCRATCH_EN) != 0;
	r2.user_sgpr  = LUMEN_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, USER_SGPR) +
	               (LUMEN_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, USER_SGPR_MSB) << 5u);
	r2.offchip_lds       = LUMEN_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, OC_LDS_EN) != 0;
	r2.streamout_enabled = LUMEN_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, SO_EN) != 0;
	r2.shared_vgprs      = LUMEN_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, SHARED_VGPR_CNT);

	cp->GetShCtx()->SetVsShaderBase(addr);
	cp->GetShCtx()->SetVsShaderModifier(shader_modifier);
	cp->GetShCtx()->SetVsShaderResource1(r1);
	cp->GetShCtx()->SetVsShaderResource2(r2);

	return 28;
}

LUMEN_HW_UC_PARSER(hw_uc_set_primitive_type)
{
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0017900);
	EXIT_NOT_IMPLEMENTED(cmd_offset != Pm4::VGT_PRIMITIVE_TYPE);

	uint32_t prim_type = LUMEN_PM4_GET(buffer[0], VGT_PRIMITIVE_TYPE, PRIM_TYPE);

	cp->GetUcfg()->SetPrimitiveType(prim_type);

	return 1;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static bool execute_acquire_mem_db_only(CommandProcessor* cp, const Pm4AcquireMemGlobalWriteBackPacket& packet,
                                         bool candidate_enabled)
{
	if (cp == nullptr || !pm4_acquire_mem_db_only_supported(packet, candidate_enabled, cp->GetInterruptEventId())) return false;
	// Conservative GPU visibility for the admitted range; no CPU write-back or
	// inferred image layout transition. This remains an experimental HLE mapping.
	cp->FullMemoryBarrier();
	return true;
}

LUMEN_CP_OP_PARSER(cp_op_acquire_mem)
{
	LUMEN_PROFILER_FUNCTION();

	Pm4AcquireMemGlobalWriteBackPacket decoded;
	EXIT_NOT_IMPLEMENTED(!pm4_decode_acquire_mem(cmd_id, buffer, dw, &decoded));

	bool custom = (cmd_id == 0xc0061050);

	uint32_t                  engine_selector = decoded.engine_selector;
	uint32_t                  cache_action    = decoded.cache_action;
	uint64_t                  size_lo         = decoded.size_lo;
	uint32_t                  size_hi         = decoded.size_hi;
	uint64_t                  base_lo         = decoded.base_lo;
	uint32_t                  base_hi         = decoded.base_hi;
	uint32_t                  poll            = decoded.poll;
	[[maybe_unused]] uint32_t gcr_cntl        = decoded.gcr_cntl;

	uint32_t target_mask     = cache_action & 0x00007FC0u;
	uint32_t extended_action = cache_action & 0x2E000000u;
	uint32_t action          = ((cache_action & 0x00C00000u) >> 0x12u) | ((cache_action & 0x00058000u) >> 0xfu);
	thread_local uint64_t diagnostic_acquire_count[9] = {};
	thread_local uint64_t diagnostic_acquire_total    = 0;
	int                   diagnostic_action_index     = -1;
	switch (cache_action)
	{
		case 0x02c40040: diagnostic_action_index = 0; break;
		case 0x02c43fc0: diagnostic_action_index = 1; break;
		case 0x02c47fc0: diagnostic_action_index = 2; break;
		case 0x02003fc0: diagnostic_action_index = 3; break;
		case 0x00C40000: diagnostic_action_index = 4; break;
		case 0x00400000: diagnostic_action_index = 5; break;
		case 0x04c44000: diagnostic_action_index = 6; break;
		case 0x06007fc0: diagnostic_action_index = 7; break;
		case 0x08c40000: diagnostic_action_index = 8; break;
		default: break;
	}
	if (Config::DiagnosticsEnabled())
	{
		if (diagnostic_action_index >= 0)
		{
			diagnostic_acquire_count[diagnostic_action_index]++;
		}
		diagnostic_acquire_total++;
		if (diagnostic_acquire_total == 2880)
		{
			dbg_printf("[PERF] acquire actions 02c40040=%" PRIu64 " 02c43fc0=%" PRIu64 " 02c47fc0=%" PRIu64
			           " 02003fc0=%" PRIu64 " 00c40000=%" PRIu64 " 00400000=%" PRIu64 " 04c44000=%" PRIu64
			           " 06007fc0=%" PRIu64 " 08c40000=%" PRIu64 "\n",
			           diagnostic_acquire_count[0], diagnostic_acquire_count[1], diagnostic_acquire_count[2], diagnostic_acquire_count[3],
			           diagnostic_acquire_count[4], diagnostic_acquire_count[5], diagnostic_acquire_count[6], diagnostic_acquire_count[7],
			           diagnostic_acquire_count[8]);
		}
	}

	if (cache_action != 0x04004000u)
	{
		EXIT_NOT_IMPLEMENTED(size_hi != 0);
		EXIT_NOT_IMPLEMENTED(base_hi != 0);
		EXIT_NOT_IMPLEMENTED(poll != 10);
	}

	switch (cache_action)
	{
		case 0x02c40040:
		case 0x02c43fc0:
		case 0x02c47fc0:
		{
			// target_mask:     0x00000040 (rt0), 0x00003fc0 (all rt), 0x00007fc0 (all rt and depth)
			// extended_action: 0x02000000 (FlushAndInvalidateCbCache)
			// action:          0x38 (WriteBackAndInvalidateL1andL2)
			EXIT_IF(target_mask != 0x00000040 && target_mask != 0x00003FC0 && target_mask != 0x00007FC0);
			EXIT_IF(extended_action != 0x02000000);
			EXIT_IF(action != 0x38);
			EXIT_NOT_IMPLEMENTED(size_lo == 0);
			EXIT_NOT_IMPLEMENTED(base_lo == 0);

			cp->RenderTextureBarrier(base_lo << 8u, size_lo << 8u);
		}
		break;
		case 0x02003fc0:
		{
			// target_mask:     0x00003FC0 (all rt)
			// extended_action: 0x02000000 (FlushAndInvalidateCbCache)
			// action:          0x00 (none)
			EXIT_IF(target_mask != 0x00003FC0);
			EXIT_IF(extended_action != 0x02000000);
			EXIT_IF(action != 0x00);
			EXIT_NOT_IMPLEMENTED(size_lo == 0);
			EXIT_NOT_IMPLEMENTED(base_lo == 0);

			cp->RenderTextureBarrier(base_lo << 8u, size_lo << 8u);
		}
		break;
		case 0x00C40000:
		case 0x08C40000:
		{
			// target_mask:     0x00000000 (none)
			// extended_action: 0x00000000 (base) or observed SH_KCACHE_ACTION_ENA (0x08000000)
			// action:          0x38 (WriteBackAndInvalidateL1andL2)
			const Pm4AcquireMemGlobalWriteBackPacket packet = {
			    cmd_id, engine_selector, cache_action, size_lo, size_hi, base_lo, base_hi, poll, gcr_cntl,
			};
			const auto plan = pm4_acquire_mem_global_writeback_plan(packet, experimental_acquire_mem_08c40000_enabled());
			if (plan.profile == Pm4AcquireMemGlobalWriteBackProfile::Unsupported)
			{
				EXIT("Unsupported ACQUIRE_MEM profile: header=%08x engine=%u control=%08x size=%08x:%08" PRIx64
				     " base=%08x:%08" PRIx64 " poll=%u gcr=%08x extended_candidate=%u\n",
				     cmd_id, engine_selector, cache_action, size_hi, size_lo, base_hi, base_lo, poll, gcr_cntl,
				     static_cast<unsigned>(experimental_acquire_mem_08c40000_enabled()));
			}
			for (const auto step: plan.steps)
			{
				switch (step)
				{
					case Pm4AcquireMemGlobalWriteBackStep::MemoryBarrier: cp->MemoryBarrier(); break;
					case Pm4AcquireMemGlobalWriteBackStep::WriteBack: cp->WriteBack(); break;
					default: EXIT_NOT_IMPLEMENTED(true);
				}
			}
		}
		break;
		case 0x00400000:
		{
			// target_mask:     0x00000000 (none)
			// extended_action: 0x00000000 (none)
			// action:          0x10 (InvalidateL1)
			EXIT_IF(target_mask != 0x00000000);
			EXIT_IF(extended_action != 0x00000000);
			EXIT_IF(action != 0x10);
			EXIT_NOT_IMPLEMENTED(size_lo != 1);
			EXIT_NOT_IMPLEMENTED(base_lo != 0);

			cp->MemoryBarrier();
		}
		break;
		case 0x04004000:
		{
			if (!execute_acquire_mem_db_only(cp, decoded, experimental_acquire_mem_db_only_enabled()))
			{
				EXIT("Unsupported DB-only ACQUIRE_MEM: header=%08x engine=%u control=%08x size=%08x:%08" PRIx64
				     " base=%08x:%08" PRIx64 " poll=%u gcr=%08x graphics_cp=%u candidate=%u\n",
				     cmd_id, engine_selector, cache_action, size_hi, size_lo, base_hi, base_lo, poll, gcr_cntl,
				     static_cast<unsigned>(cp->GetInterruptEventId() == 64),
				     static_cast<unsigned>(experimental_acquire_mem_db_only_enabled()));
			}
		}
		break;
		case 0x04c44000:
		{
			// target_mask:     0x00004000 (Depth Target)
			// extended_action: 0x04000000 (FlushAndInvalidateDbCache)
			// action:          0x38 (WriteBackAndInvalidateL1andL2)
			EXIT_IF(target_mask != 0x00004000);
			EXIT_IF(extended_action != 0x04000000);
			EXIT_IF(action != 0x38);

			cp->DepthStencilBarrier(base_lo << 8u, size_lo << 8u);
		}
		break;

		case 0x06007fc0:
		{
			// target_mask:     0x00007fc0 (all rt and depth)
			// extended_action: 0x06000000 (Flush Cb & Db)
			// action:          0x00 (none)
			EXIT_NOT_IMPLEMENTED(gcr_cntl != 0x280); // kGl0ScalarInvalidate & kGl1Invalidate
			EXIT_NOT_IMPLEMENTED(size_lo != 0);
			EXIT_NOT_IMPLEMENTED(base_lo != 0);

			EXIT_IF(target_mask != 0x00007fc0);
			EXIT_IF(extended_action != 0x06000000);
			EXIT_IF(action != 0x00);

			cp->MemoryBarrier();
		}
		break;

		default:
			EXIT("Unsupported ACQUIRE_MEM barrier: header=%08" PRIx32 " engine=%u control=%08" PRIx32
			     " size=%08" PRIx32 ":%08" PRIx64 " base=%08" PRIx32 ":%08" PRIx64
			     " poll=%u gcr=%08" PRIx32 " target=%08" PRIx32 " extended=%08" PRIx32 " action=%08" PRIx32 "\n",
			     cmd_id, engine_selector, cache_action, size_hi, size_lo, base_hi, base_lo, poll, gcr_cntl,
			     target_mask, extended_action, action);
	}

	(void)engine_selector;

	return (custom ? 7 : 6);
}

LUMEN_CP_OP_PARSER(cp_op_dispatch_direct)
{
	LUMEN_PROFILER_FUNCTION();

	Pm4DispatchDirectPacket decoded;
	EXIT_NOT_IMPLEMENTED(!pm4_decode_dispatch_direct(cmd_id, buffer, dw, &decoded));

	cp->DispatchDirect(decoded.thread_group_x, decoded.thread_group_y, decoded.thread_group_z, decoded.renderer_mode);

	return decoded.consumed;
}

LUMEN_CP_OP_PARSER(cp_op_dispatch_reset)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0001024);

	cp->Reset();

	return 1;
}

LUMEN_CP_OP_PARSER(cp_op_dma_data)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0055000 || buffer == nullptr || dw < 7);
	Pm4DmaDataMemoryCopyPacket memory_copy;

	uint32_t type  = buffer[0];
	uint32_t type2 = (buffer[5] >> 21u);
	uint64_t src   = (buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
	uint64_t dst   = (buffer[3] | (static_cast<uint64_t>(buffer[4]) << 32u));
	uint32_t size  = buffer[5] & 0x1fffffu;

	if (pm4_decode_dma_data_memory_copy(cmd_id, buffer, dw, &memory_copy))
	{
		cp->CopyDmaData(memory_copy.source_address, memory_copy.destination_address, memory_copy.byte_count);
	} else if (type == 0x60000000 && dst == 0x0003022C && type2 == 0x141u)
	{
		auto* addr = reinterpret_cast<void*>(src);

		cp->PrefetchL2(addr, size);
	} else if (type == 0xc4104000 && type2 == 0x0u)
	{
		EXIT_NOT_IMPLEMENTED((dst & 0x3u) != 0);
		EXIT_NOT_IMPLEMENTED((size & 0x3u) != 0);

		cp->ClearGds(dst / 4, size / 4, static_cast<uint32_t>(src));
	} else if (type == 0xa4004000 && type2 == 0x0u)
	{
		Pm4DmaDataMemoryCopyPacket gds_read;
		EXIT_NOT_IMPLEMENTED(!pm4_decode_dma_data_gds_read(cmd_id, buffer, dw, &gds_read));
		cp->ReadGds(gds_read.destination_address, gds_read.source_address, gds_read.byte_count);
	} else
	{
		EXIT("unknown dma: type=0x%08" PRIx32 ", type2=0x%03" PRIx32 ", src=0x%016" PRIx64
		     ", dst=0x%016" PRIx64 ", size=0x%06" PRIx32 ", command=0x%08" PRIx32 "\n",
		     type, type2, src, dst, size, buffer[5]);
	}

	return 6;
}

LUMEN_CP_OP_PARSER(cp_op_draw_index)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC008100C && cmd_id != 0xc0042700);

	if (cmd_id == 0xC008100C)
	{
		uint32_t index_count = buffer[0];
		auto*    index_addr  = reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
		uint32_t flags       = buffer[3];
		uint32_t type        = buffer[4];

		cp->DrawIndex(index_count, index_addr, flags, type, 10);

		return 9;
	}

	if (cmd_id == 0xc0042700)
	{
		EXIT_NOT_IMPLEMENTED(dw < 8);

		uint32_t index_count = buffer[0];
		auto*    index_addr  = reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));

		EXIT_NOT_IMPLEMENTED(buffer[3] != index_count);
		EXIT_NOT_IMPLEMENTED(buffer[4] != 0);

		if (buffer[5] == 0xc0001000)
		{
			EXIT_NOT_IMPLEMENTED(buffer[6] != 0);
			cp->DrawIndex(index_count, index_addr, 0, 1, 8);

			return 7;
		}

		if (buffer[5] == 0xc0021000)
		{
			EXIT_NOT_IMPLEMENTED(dw < 10);
			EXIT_NOT_IMPLEMENTED(buffer[6] != 0);
			cp->DrawIndex(index_count, index_addr, 0, 1, 10);

			return 9;
		}

		EXIT("invalid draw_index\n");
	}

	return 1;
}

LUMEN_CP_OP_PARSER(cp_op_draw_index_auto)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0051010 && cmd_id != 0xc0012d00);

	if (cmd_id == 0xC0051010)
	{
		uint32_t index_count = buffer[0];
		uint32_t flags       = buffer[1];

		cp->DrawIndexAuto(index_count, flags, 7);

		return 6;
	}

	if (cmd_id == 0xc0012d00)
	{
		EXIT_NOT_IMPLEMENTED(dw < 5);

		uint32_t index_count = buffer[0];
		uint32_t flags       = 0;

		EXIT_NOT_IMPLEMENTED(buffer[1] != 2);

		if (buffer[2] == 0xc0001000)
		{
			EXIT_NOT_IMPLEMENTED(buffer[3] != 0);
			cp->DrawIndexAuto(index_count, flags, 5);

			return 4;
		}

		if (buffer[2] == 0xc0021000)
		{
			EXIT_NOT_IMPLEMENTED(dw < 7);
			EXIT_NOT_IMPLEMENTED(buffer[3] != 0);
			cp->DrawIndexAuto(index_count, flags, 7);

			return 6;
		}

		EXIT("invalid draw_index_auto\n");
	}

	return 1;
}

LUMEN_CP_OP_PARSER(cp_op_draw_reset)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0001014);

	CommandProcessorStateLock state_lock(cp);
	cp->Reset();
	if (experimental_hle_d32_default_enabled())
	{
		apply_hle_draw_reset_polygon_offset_format(cp->GetCtx(), true);
		static std::atomic_bool logged {false};
		if (!logged.exchange(true, std::memory_order_relaxed))
			dbg_printf("[GFX_HLE_D32_DEFAULT] experimental=1 R_DRAW_RESET DB_FMT_CNTL=0x1e9; diagnostic hypothesis, console default unproven\n");
	}

	return 1;
}

struct Pm4ConstRamDumpPacket
{
	uint32_t offset = 0;
	uint32_t count = 0;
	uint32_t* destination = nullptr;
};

static bool pm4_decode_dump_const_ram(uint32_t cmd_id, const uint32_t* body, uint32_t available_dw,
                                      Pm4ConstRamDumpPacket* decoded)
{
	// available_dw includes the header. Read none of the four body words until
	// the complete fixed packet is present, including for direct parser callers.
	if (cmd_id != 0xC0038300u || body == nullptr || decoded == nullptr || available_dw < 5) return false;
	*decoded = {body[0], body[1], reinterpret_cast<uint32_t*>(body[2] | (static_cast<uint64_t>(body[3]) << 32u))};
	return true;
}

LUMEN_CP_OP_PARSER(cp_op_dump_const_ram)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0038300);
	Pm4ConstRamDumpPacket decoded;
	if (!pm4_decode_dump_const_ram(cmd_id, buffer, dw, &decoded))
		const_ram_reject("dump", "packet-span", 0, 0);

	cp->DumpConstRam(decoded.destination, decoded.offset, decoded.count);

	return 4;
}

static bool pm4_decode_event_write(uint32_t cmd_id, const uint32_t* body, uint32_t available_dw, uint32_t* control)
{
	if (cmd_id != 0xc0004600u || body == nullptr || available_dw < 2 || control == nullptr) return false;
	*control = body[0];
	return true;
}

static bool pm4_shader_partial_flush_supported(uint32_t cmd_id, uint32_t control, uint32_t available_dw, uint32_t interrupt_event_id)
{
	return cmd_id == 0xc0004600u && available_dw >= 2 && (control == 0x410u || control == 0x407u) && interrupt_event_id == 64;
}

static bool execute_shader_partial_flush_event(CommandProcessor* cp, uint32_t cmd_id, uint32_t control, uint32_t available_dw)
{
	if (cp == nullptr) return false;
	CommandProcessorStateLock state_lock(cp);
	if (!pm4_shader_partial_flush_supported(cmd_id, control, available_dw, cp->GetInterruptEventId())) return false;

	// The verified non-query PS/CS partial-flush profiles order prior shader
	// work. Use the common in-queue barrier conservatively; this does not
	// publish guest memory, flush PS4 caches, signal EOP or wait on the CPU.
	cp->FullMemoryBarrier();
	static std::atomic_uint traced_profiles {0};
	const uint32_t profile_bit = control == 0x410u ? 1u : 2u;
	if ((traced_profiles.fetch_or(profile_bit, std::memory_order_relaxed) & profile_bit) == 0)
	{
		std::fprintf(stderr, "[GFX_SHADER_PARTIAL_FLUSH] header=%08" PRIx32 " control=%08" PRIx32 " shader=%s type=%02" PRIx32
		                     " graphics_cp=1 effect=full-memory-barrier guest-write=0 irq=0 readback=0 hardware-cache-claim=0\n",
		             cmd_id, control, control == 0x410u ? "ps" : "cs", control & 0x3fu);
	}
	return true;
}

static bool execute_cb_meta_event(CommandProcessor* cp, uint32_t control, bool candidate_enabled)
{
	if (cp == nullptr) return false;
	CommandProcessorStateLock state_lock(cp);
	if (!pm4_cb_meta_event_supported(control, candidate_enabled, cp->GetInterruptEventId(), *cp->GetCtx())) return false;

	// Experimental ordering only: this renderer keeps every color surface
	// uncompressed and fully materialized, so a metadata flush has no host
	// cache to publish and only needs the ordering barrier. Do not infer CPU
	// writes or interrupts. The bounded report documents the first accepted
	// fast-clear register states once per process; the renderer never reads or
	// writes the guest metadata bytes.
	if (candidate_enabled)
	{
		static std::atomic_int traced = 0;
		if (traced.fetch_add(1) < 8)
		{
			const auto verdict = HW::ClassifyColorMetadata(*cp->GetCtx());
			if (verdict.profile == HW::ColorMetadataProfile::FastClearSingleSample)
			{
				const auto& rt = cp->GetCtx()->GetRenderTarget(verdict.first_slot);
				std::fprintf(stderr, "[CB_META] admitted fast-clear slot=%u cmask=%016" PRIx64 " fmask=%016" PRIx64
				                     " clear0=%08" PRIx32 " clear1=%08" PRIx32 "\n",
				             verdict.first_slot, rt.cmask.addr, rt.fmask.addr, rt.clear_word0.word0, rt.clear_word1.word1);
			}
		}
	}
	cp->FullMemoryBarrier();
	return true;
}

static bool pm4_db_cache_event_supported(uint32_t header, uint32_t control, uint32_t available_dw, uint32_t interrupt_event_id)
{
	return header == 0xc0004600u && control == 0x72au && available_dw >= 2 && interrupt_event_id == 64;
}

static bool db_cache_event_admission_enabled(CommandProcessor* cp)
{
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	if (command_effect_probe_active(cp) && g_command_effect_probe->db_cache_proof_enabled)
		return g_command_effect_probe->db_cache_admission_enabled;
#endif
	(void)cp;
	// Raw profile and the complete already-synchronized depth proof are both
	// mandatory. Backends without mapping-continuity capability still reject.
	return true;
}

struct DbCacheRetryScopeSnapshot
{
	EopInitialTargetIdentity identity;
	const HW::Context* context_owner = nullptr;
	std::array<uint8_t, sizeof(HW::Context)> context_bytes;
	uint64_t operation_generation = UINT64_MAX;
	uint64_t proof_generation = UINT64_MAX;
	uint32_t rounds = 0;
	uint32_t pauses = 0;
};

static const char* DbCacheRetryScopeRejection(const EopInitialTargetIdentity& original,
    const EopInitialTargetIdentity& current, const void* processor, bool run_lock_held, bool parser_owned,
    const HW::Context* context_owner, const std::array<uint8_t, sizeof(HW::Context)>& original_context,
    const HW::Context& current_context)
{
	if (!ScalarLabelCaptureScopeQualified(current, processor, run_lock_held, parser_owned))
		return "db-cache-retry-scope-unqualified";
	if (!original.Matches(current)) return "db-cache-recording-changed-during-retry";
	if (context_owner != &current_context || std::memcmp(original_context.data(),
	    reinterpret_cast<const unsigned char*>(&current_context), sizeof(current_context)) != 0)
		return "db-cache-context-changed-during-retry";
	return nullptr;
}

template <typename Attempt, typename Revalidate, typename Backoff, typename LeaseHeld, typename RetrySafe>
static GpuMemoryDbCacheProofResult DbCacheBarrierWithLeaseRetry(CommandProcessor* cp, uint64_t operation_generation,
    Attempt attempt, Revalidate revalidate, Backoff backoff, LeaseHeld lease_held, RetrySafe retry_safe,
    uint32_t* rounds = nullptr, uint32_t* pauses = nullptr)
{
	GpuMemoryDbCacheProofResult result {GpuMemoryDbCacheProofStatus::Rejected, "db-cache-preflight-not-attempted"};
	if (rounds != nullptr) *rounds = 0;
	if (pauses != nullptr) *pauses = 0;
	const auto reject = [&](const char* reason) {
		result.status = GpuMemoryDbCacheProofStatus::Rejected;
		result.reason = reason;
		result.retryable = false;
		return result;
	};
	if (operation_generation == UINT64_MAX) return reject("mapping-generation-unavailable");
	if (const char* reason = revalidate(); reason != nullptr) return reject(reason);
	for (uint32_t round = 1; round <= 16; ++round)
	{
		if (lease_held()) return reject("db-cache-retry-memory-lease-held");
		if (rounds != nullptr) *rounds = round;
		result = attempt();
		// The renderer and tracker guards, and every local lease, must have
		// unwound before either returning or releasing the sole CP level.
		if (lease_held()) return reject("db-cache-retry-memory-lease-held");
		if (result.status != GpuMemoryDbCacheProofStatus::Rejected || !result.retryable || result.reason == nullptr ||
		    std::strcmp(result.reason, "lease-set-retry-exhausted") != 0)
		{
			result.retryable = false;
			return result;
		}
		if (!retry_safe(result)) return reject("db-cache-preflight-retry-ticket-invalid");
		if (round == 16) return reject("db-cache-lease-set-retry-exhausted");
		if (const char* reason = revalidate(); reason != nullptr) return reject(reason);
		{
			// execute_db_cache_event owns CP1. The parser takes its diagnostic
			// guard only after this returns; Run's setup guard is already gone.
			// The ring owner keeps RunLock throughout this pre-effects pause.
			struct ReleaseProcessorLock
			{
				CommandProcessor* cp;
				explicit ReleaseProcessorLock(CommandProcessor* value): cp(value) { cp->Unlock(); }
				~ReleaseProcessorLock() { cp->Lock(); }
			} release(cp);
			if (pauses != nullptr) ++*pauses;
			backoff();
		}
		// Re-read the current recording and context under CP before touching
		// any saved buffer. The ticket retains its original proof and token.
		if (const char* reason = revalidate(); reason != nullptr) return reject(reason);
	}
	return result;
}

GpuMemoryDbCacheProofResult CommandProcessor::TryDbCacheBarrierWithLeaseRetry(bool admission_enabled)
{
	DbCacheRetryScopeSnapshot scope;
	scope.identity = GetEopInitialTargetIdentity();
	scope.context_owner = &m_ctx;
	// Compare bytes of this exact CP-owned object, including its original
	// padding. Do not reconstruct or memcpy a polymorphic Context object.
	std::memcpy(scope.context_bytes.data(), reinterpret_cast<const unsigned char*>(&m_ctx), sizeof(m_ctx));
	scope.operation_generation = Core::VirtualMemory::GetMutationGeneration();
	GpuMemoryDbCachePreflightTicket ticket(scope.operation_generation);
	const auto attempt = [&] {
		return GraphicsRenderDbCacheAlreadySynchronizedBarrier(GetCurrentBufferForCpuCopy(), admission_enabled, &ticket);
	};
	const auto revalidate = [&]() -> const char* {
		return DbCacheRetryScopeRejection(scope.identity, GetEopInitialTargetIdentity(), this,
		    m_run_lock_held.load(std::memory_order_acquire),
		    g_command_processor_parser_state != nullptr && g_command_processor_parser_state->owner == this,
		    scope.context_owner, scope.context_bytes, m_ctx);
	};
	const auto backoff = [] { Core::Thread::SleepMicro(1000); };
	const auto lease_held = [] {
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		return Core::VirtualMemory::CurrentThreadHasMemoryLease();
#else
		return false;
#endif
	};
	const auto retry_safe = [&](const GpuMemoryDbCacheProofResult& proof) {
		if (!admission_enabled || !ticket.IsPrepared() || ticket.EffectsStarted()) return false;
		const auto generation = ticket.OriginalGeneration();
		if (generation == UINT64_MAX || generation > scope.operation_generation || generation != proof.original_generation) return false;
		if (scope.proof_generation == UINT64_MAX) scope.proof_generation = generation;
		return generation == scope.proof_generation;
	};
	// The tracker charges this declared reserve before allocating its retained
	// proof. Include the caller snapshot, closures and transient identity/result
	// copies within that same aggregate 4 MiB accounting, rather than a second
	// uncharged scope budget.
	static_assert(sizeof(scope) + sizeof(ticket) +
	    2 * (sizeof(attempt) + sizeof(revalidate) + sizeof(backoff) + sizeof(lease_held) + sizeof(retry_safe)) +
	    sizeof(EopInitialTargetIdentity) + 2 * sizeof(GpuMemoryDbCacheProofResult) +
	    8 * sizeof(void*) + 3 * sizeof(uint64_t) + 3 * sizeof(uint32_t) <= GpuMemoryDbCachePreflightTicket::MaxScopeMetadataBytes);
	const auto result = DbCacheBarrierWithLeaseRetry(this, scope.operation_generation, attempt, revalidate,
	    backoff, lease_held, retry_safe, &scope.rounds, &scope.pauses);
	if (scope.pauses != 0)
	{
		static std::atomic<uint32_t> receipts {0};
		if (receipts.fetch_add(1, std::memory_order_relaxed) < 16)
			std::fprintf(stderr, "[GFX_DB_CACHE_LEASE_RETRY] rounds=%u pauses=%u operation_generation=%" PRIu64
			    " original_generation=%" PRIu64 " proof_status=%u first_cause=%s barrier=%u guest-write=0 irq=0 readback=0"
			    " local-leases=released local-renderer=released local-tracker=released cp-level=1 run-lock=retained"
			    " original-proof=retained cap-rounds=16 cap-pauses=15\n", scope.rounds, scope.pauses, scope.operation_generation,
			    result.original_generation, static_cast<unsigned>(result.status), result.reason,
			    result.status == GpuMemoryDbCacheProofStatus::Recorded ? 1u : 0u);
	}
	return result;
}

static GpuMemoryDbCacheProofResult execute_db_cache_event(CommandProcessor* cp, uint32_t header, uint32_t control,
    uint32_t available_dw, bool admission_enabled)
{
	if (cp == nullptr) return {GpuMemoryDbCacheProofStatus::Rejected, "db-cache-command-processor-missing"};
	CommandProcessorStateLock state_lock(cp);
	if (!pm4_db_cache_event_supported(header, control, available_dw, cp->GetInterruptEventId()))
		return {GpuMemoryDbCacheProofStatus::Rejected, "db-cache-raw-profile-unproved"};
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	if (command_effect_probe_active(cp) && g_command_effect_probe->db_cache_proof_enabled)
	{
		// Only native resources/proof authority are modeled in this PM4 fixture.
		// The tracker fixture independently exercises real complete VM lease sets.
		++g_command_effect_probe->db_cache_proofs;
		if (g_command_effect_probe->db_cache_proof_status == GpuMemoryDbCacheProofStatus::Rejected)
			return {GpuMemoryDbCacheProofStatus::Rejected, "fixture-depth-proof-rejected"};
		if (admission_enabled) cp->FullMemoryBarrier();
		return {admission_enabled ? GpuMemoryDbCacheProofStatus::Recorded : GpuMemoryDbCacheProofStatus::Proven,
		    "fixture-depth-proof-modeled"};
	}
#endif
	return cp->TryDbCacheBarrierWithLeaseRetry(admission_enabled);
}

static void trace_db_cache_guard(const GpuMemoryDbCacheGuardDiagnostic& guard)
{
	if (!guard.rejected) return;
	// Called only after the proof has returned and the caller-lease guard below.
	static std::atomic<uint32_t> records {0};
	auto record = records.load(std::memory_order_relaxed);
	while (record < 16 && !records.compare_exchange_weak(record, record + 1, std::memory_order_relaxed)) {}
	if (record >= 16) return;
	std::fprintf(stderr, "[GFX_DB_CACHE_DEPTH_GUARD] record=%u heap=%d object=%d lifetime=%" PRIu64 " planes=%d"
	    " synchronized=%u synchronized_generation=%" PRIu64 " synchronized_from_readback=%u host_content_pending=%u"
	    " initial_use_pending=%u reuse_pending=%u metadata_write_pending=%u read_only_barrier=%u barrier_generation=%" PRIu64
	    " operation_generation=%" PRIu64 " accumulated_prefix_generation=%" PRIu64 " evaluation_mask=%u"
	    " latest_follows=%u latest_completed=%u layout_follows=%u layout_completed=%u unevaluated_bits=unknown"
	    " evaluation_mask_scope=strict-prefix retained_readback_attempted=%u producer_completion_observed=%u producer_completed=%u"
	    " guest_reads=0 diagnostic_queries_added=0 diagnostic_receipt_queries_added=0 local_leases=released local_tracker=released"
	    " metadata_only=1 admission=unchanged cap=16\n",
	    record, guard.heap, guard.object, guard.lifetime, guard.plane_count, guard.synchronized ? 1u : 0u,
	    guard.synchronized_generation, guard.synchronized_from_readback ? 1u : 0u, guard.host_content_pending ? 1u : 0u,
	    guard.initial_use_pending ? 1u : 0u, guard.reuse_pending ? 1u : 0u, guard.metadata_write_pending ? 1u : 0u,
	    guard.read_only_barrier ? 1u : 0u, guard.barrier_generation, guard.operation_generation,
	    guard.accumulated_prefix_generation, guard.evaluation_mask, guard.latest_follows ? 1u : 0u,
	    guard.latest_completed ? 1u : 0u, guard.layout_follows ? 1u : 0u, guard.layout_completed ? 1u : 0u,
	    guard.retained_readback_attempted ? 1u : 0u, guard.producer_completion_observed ? 1u : 0u, guard.producer_completed ? 1u : 0u);
	for (int plane = 0; plane < 3; ++plane)
		std::fprintf(stderr, "[GFX_DB_CACHE_DEPTH_GUARD_PLANE] record=%u plane=%d address=%016" PRIx64 " bytes=%" PRIu64 "\n",
		    record, plane, guard.addresses[plane], guard.sizes[plane]);
	const std::pair<const char*, const GpuMemoryDbCacheGuardIdentity*> identities[] = {
	    {"current", &guard.current}, {"latest", &guard.latest}, {"layout", &guard.layout},
	    {"synchronized_producer", &guard.synchronized_producer}};
	for (const auto& named: identities)
	{
		const auto& identity = *named.second;
		std::fprintf(stderr, "[GFX_DB_CACHE_DEPTH_GUARD_IDENTITY] record=%u role=%s cp=%p pool=%p queue=%d session=%" PRIu64
		    " serial=%" PRIu64 " recording=%" PRIu64 " ordinal=%" PRIu64 " submitted_cutoff=%" PRIu64
		    " completed_cutoff=%" PRIu64 " metadata_only=1 identity_dereference=0\n",
		    record, named.first, static_cast<void*>(identity.cp), static_cast<void*>(identity.pool), identity.queue,
		    identity.submission_session_epoch, identity.recording_submit_serial, identity.recording_id,
		    identity.command_ordinal, identity.begin_submit_serial_cutoff, identity.begin_completed_serial_cutoff);
	}
}

static void trace_db_cache_event(CommandProcessor* cp, uint32_t header, uint32_t control, uint32_t available_dw,
    bool admission_enabled, const GpuMemoryDbCacheProofResult& proof)
{
	// The proof's local mapping pins are released before this bounded receipt.
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	// A rejected proof does not release a lease owned by its caller.
	if (Core::VirtualMemory::CurrentThreadHasMemoryLease()) return;
#endif
	trace_db_cache_guard(proof.guard_diagnostic);
	static std::atomic<uint32_t> accepted {0}, rejected {0};
	const bool recorded = proof.status == GpuMemoryDbCacheProofStatus::Recorded;
	auto& counter = recorded ? accepted : rejected;
	const bool ordinary_receipt = counter.fetch_add(1, std::memory_order_relaxed) < 4;
	// Earlier strict DB events may consume their ordinary receipt budget. Keep
	// a separate bounded witness for the completed-readback/submitted-read-only
	// authority, without changing either existing event counter or admission.
	static const bool trace_retained = [] {
		const char* value = std::getenv("LUMEN_GFX_EOP_RESOURCE_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	static std::atomic<uint32_t> retained_records {0};
	bool retained_receipt = false;
	if (trace_retained && proof.status != GpuMemoryDbCacheProofStatus::Rejected && proof.retained_readback_pairs != 0)
	{
		auto retained_record = retained_records.load(std::memory_order_relaxed);
		while (retained_record < 16 &&
		       !retained_records.compare_exchange_weak(retained_record, retained_record + 1, std::memory_order_relaxed)) {}
		retained_receipt = retained_record < 16;
	}
	if (!ordinary_receipt && !retained_receipt) return;
	const auto& target = cp->GetCtx()->GetDepthRenderTarget();
	const auto& depth = cp->GetCtx()->GetDepthControl();
	const auto& render = cp->GetCtx()->GetRenderControl();
	std::fprintf(stderr, "[GFX_DB_CACHE_EVENT_%s] header=%08" PRIx32 " control=%08" PRIx32
	    " index_raw=%u available_dw=%u graphics_cp=%u admission=%u first_cause=%s proof_status=%u"
	    " live_objects=%u depth_pairs=%u initial_uniform_pairs=%u readback_pairs=%u retained_readback_pairs=%u"
	    " submitted_read_only_pairs=%u original_generation=%" PRIu64 " rejected_plane=%d"
	    " z_format=%u stencil_format=%u z_read=%016" PRIx64 " z_write=%016" PRIx64
	    " stencil_read=%016" PRIx64 " stencil_write=%016" PRIx64 " htile=%016" PRIx64
	    " htile_raw=%08" PRIx32 " depth_control=%08" PRIx32 " render_control=%08" PRIx32
	    " z_enable=%u z_write_enable=%u stencil_enable=%u barrier=%u guest-write=0 irq=0 readback=0 layout-mutation=0"
	    " submitted_prefix_completion_claim=0 retained_trace=%u retained_trace_cap=16 retry_certificate_reason=%s\n",
	    recorded ? "RECORDED" : "BLOCK", header, control, (control >> 8u) & 0xfu, available_dw,
	    static_cast<unsigned>(cp->GetInterruptEventId() == 64), admission_enabled ? 1u : 0u,
	    !admission_enabled && proof.status == GpuMemoryDbCacheProofStatus::Proven ? "admission-disabled" : proof.reason,
	    static_cast<unsigned>(proof.status), proof.live_objects, proof.depth_pairs, proof.initial_uniform_pairs,
	    proof.readback_pairs, proof.retained_readback_pairs, proof.submitted_read_only_pairs,
	    proof.original_generation, proof.rejected_plane, target.z_info.format, target.stencil_info.format,
	    target.z_read_base_addr, target.z_write_base_addr, target.stencil_read_base_addr, target.stencil_write_base_addr,
	    target.htile_data_base_addr, target.htile_surface.raw, depth.raw, render.raw, static_cast<unsigned>(depth.z_enable),
	    static_cast<unsigned>(depth.z_write_enable), static_cast<unsigned>(depth.stencil_enable), recorded ? 1u : 0u,
	    retained_receipt ? 1u : 0u,
	    proof.retry_certificate_reason != nullptr ? proof.retry_certificate_reason : "db-cache-preflight-reason-unavailable");
}

LUMEN_CP_OP_PARSER(cp_op_event_write)
{
	LUMEN_PROFILER_FUNCTION();

	uint32_t control = 0;
	EXIT_NOT_IMPLEMENTED(!pm4_decode_event_write(cmd_id, buffer, dw, &control));

	uint32_t event_index = (control >> 8u) & 0x7u;
	uint32_t event_type  = control & 0x3fu;

	if (event_type == 0x2eu)
	{
		if (!execute_cb_meta_event(cp, control, experimental_cb_meta_event_enabled()))
		{
			if (experimental_cb_meta_event_enabled() && control == 0x72eu && cp->GetInterruptEventId() == 64)
			{
				CommandProcessorStateLock state_lock(cp);
				uint32_t slot = 0;
				if (HasUnsupportedColorMetadata(*cp->GetCtx(), &slot))
				{
					const auto& rt = cp->GetCtx()->GetRenderTarget(slot);
					std::fprintf(stderr, "[CB_META] unsupported metadata slot=%u cmask=%016" PRIx64 " fmask=%016" PRIx64
					             " dcc=%016" PRIx64 " fmask_compression=%u fmask_data_disable=%u one_frag=%u fast_clear=%u"
					             " dcc_compression=%u dcc_clear_key=%u samples=%u fragments=%u clear0=%08" PRIx32
					             " clear1=%08" PRIx32 "\n", slot, rt.cmask.addr, rt.fmask.addr, rt.dcc_addr.addr,
					             static_cast<unsigned>(rt.info.fmask_compression_enable), static_cast<unsigned>(rt.info.fmask_data_compression_disable),
					             static_cast<unsigned>(rt.info.fmask_one_frag_mode), static_cast<unsigned>(rt.info.cmask_fast_clear_enable),
					             static_cast<unsigned>(rt.info.dcc_compression_enable), static_cast<unsigned>(rt.dcc.dcc_clear_key_enable),
					             rt.attrib.num_samples, rt.attrib.num_fragments, rt.clear_word0.word0, rt.clear_word1.word1);
					const auto& color = cp->GetCtx()->GetColorControl();
					std::fprintf(stderr, "[CB_META] target-state slot=%u base=%016" PRIx64
					             " format=%u channel_type=%u channel_order=%u width=%u height=%u tile=%u"
					             " pitch_div8_minus1=%u slice_div64_minus1=%u cmask_slice_minus1=%u"
					             " cmask_tile=%u cmask_tile_neo=%u fmask_pitch_div8_minus1=%u"
					             " fmask_slice_minus1=%u fmask_tile=%u array_first=%u array_last=%u mip=%u"
					             " color_mode=%u color_op=%u target_mask=%08" PRIx32 "\n",
					             slot, rt.base.addr, rt.info.format, rt.info.channel_type, rt.info.channel_order,
					             rt.size.width, rt.size.height, rt.attrib.tile_mode, rt.pitch.pitch_div8_minus1,
					             rt.slice.slice_div64_minus1, rt.cmask_slice.slice_minus1, rt.info.cmask_tile_mode,
					             rt.info.cmask_tile_mode_neo, rt.pitch.fmask_pitch_div8_minus1,
					             rt.fmask_slice.slice_minus1, rt.attrib.fmask_tile_mode,
					             rt.view.base_array_slice_index, rt.view.last_array_slice_index,
					             rt.view.current_mip_level, color.mode, color.op, cp->GetCtx()->GetRenderTargetMask());
				}
			}
			EXIT("Unsupported CB metadata EVENT_WRITE: control=%08" PRIx32 " graphics_cp=%u candidate=%u\n", control,
			     static_cast<unsigned>(cp->GetInterruptEventId() == 64), static_cast<unsigned>(experimental_cb_meta_event_enabled()));
		}
	} else if ((event_type == 0x10u || event_type == 0x07u) && event_index == 4u)
	{
		// Keep the legacy index-zero route separate. Validate the complete raw
		// word here so index12 and reserved bits cannot alias the new profile.
		if (!execute_shader_partial_flush_event(cp, cmd_id, control, dw))
		{
			EXIT("Unsupported shader partial-flush EVENT_WRITE: header=%08" PRIx32 " control=%08" PRIx32
			     " graphics_cp=%u index_raw=%u\n", cmd_id, control,
			     static_cast<unsigned>(cp->GetInterruptEventId() == 64), (control >> 8u) & 0xfu);
		}
	} else if (event_type == 0x2au && event_index == 7u)
	{
		const bool admission_enabled = db_cache_event_admission_enabled(cp);
		const auto proof = execute_db_cache_event(cp, cmd_id, control, dw, admission_enabled);
		CommandProcessorStateLock state_lock(cp);
		trace_db_cache_event(cp, cmd_id, control, dw, admission_enabled, proof);
		if (proof.status != GpuMemoryDbCacheProofStatus::Recorded)
			EXIT("Unsupported DB cache EVENT_WRITE: header=%08" PRIx32 " control=%08" PRIx32
			     " graphics_cp=%u index_raw=%u first_cause=%s\n", cmd_id, control,
			     static_cast<unsigned>(cp->GetInterruptEventId() == 64), (control >> 8u) & 0xfu,
			     !admission_enabled && proof.status == GpuMemoryDbCacheProofStatus::Proven ? "admission-disabled" : proof.reason);
	} else
	{
		cp->TriggerEvent(event_type, event_index);
	}

	return 1;
}

LUMEN_CP_OP_PARSER(cp_op_event_write_eop)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0044700);

	uint32_t cache_policy       = (buffer[0] >> 25u) & 0x3u;
	uint32_t event_write_dest   = ((buffer[0] >> 23u) & 0x10u) | ((buffer[2] >> 16u) & 0x01u);
	uint32_t eop_event_type     = (buffer[0]) & 0x3fu;
	uint32_t cache_action       = (buffer[0] >> 12u) & 0x3fu;
	uint32_t event_index        = (buffer[0] >> 8u) & 0x7u;
	uint32_t event_write_source = ((buffer[2] >> 29u) & 0x7u);
	uint32_t legacy_interrupt_selector = (buffer[2] >> 24u) & 0x7u;
	uint32_t control_selector_byte      = (buffer[2] >> 24u) & 0xffu;
	auto*    dst_gpu_addr               = reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2] & 0xffffu) << 32u));
	uint64_t value                      = (buffer[3] | (static_cast<uint64_t>(buffer[4]) << 32u));

	cp->WriteAtEndOfPipe64(cache_policy, event_write_dest, eop_event_type, cache_action, event_index, event_write_source, dst_gpu_addr,
	                       value, legacy_interrupt_selector, control_selector_byte, cmd_id, buffer[0], buffer[2]);

	return 5;
}

LUMEN_CP_OP_PARSER(cp_op_event_write_eos)
{
	LUMEN_PROFILER_FUNCTION();

	Pm4PixelDoneWrite32 pixel_done;
	if (pm4_decode_pixel_done_write32(cmd_id, buffer, dw, cp->GetInterruptEventId() == 64, &pixel_done))
	{
		cp->WriteAtEndOfPixelShader32(reinterpret_cast<uint32_t*>(pixel_done.address), pixel_done.value);
		return 4;
	}
	if (cmd_id != 0xC0034802)
	{
		std::fprintf(stderr, "[GFX_EOS_UNSUPPORTED_HEADER] header=%08" PRIx32 " packet_dw=%u\n", cmd_id, LUMEN_PM4_LEN(cmd_id));
		if (LUMEN_PM4_LEN(cmd_id) == 5 && dw >= 5)
		{
			std::fprintf(stderr, "[GFX_EOS_UNSUPPORTED_CONTROL] event=%08" PRIx32 " control=%08" PRIx32 "\n",
			             buffer[0], buffer[2] & 0xffff0000u);
		}
	}
	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0034802);

	uint32_t cache_policy       = (buffer[0] >> 25u) & 0x3u;
	uint32_t event_write_dest   = 0;
	uint32_t eop_event_type     = (buffer[0]) & 0x3fu;
	uint32_t cache_action       = (buffer[0] >> 12u) & 0x3fu;
	uint32_t event_index        = (buffer[0] >> 8u) & 0x7u;
	uint32_t event_write_source = ((buffer[2] >> 29u) & 0x7u);
	uint32_t interrupt_selector = (buffer[2] >> 24u) & 0x7u;

	auto*    dst_gpu_addr = reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2] & 0xffffu) << 32u));
	uint32_t value        = buffer[3];

	cp->WriteAtEndOfPipe32(cache_policy, event_write_dest, eop_event_type, cache_action, event_index, event_write_source, dst_gpu_addr,
	                       value, interrupt_selector);

	return 4;
}

LUMEN_CP_OP_PARSER(cp_op_flip)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc004105c);

	CommandProcessor::FlipInfo f;

	f.handle    = static_cast<int>(buffer[0]);
	f.index     = static_cast<int>(buffer[1]);
	f.flip_mode = static_cast<int>(buffer[2]);
	f.flip_arg  = static_cast<int64_t>(buffer[3] | (static_cast<uint64_t>(buffer[4]) << 32u));

	CommandProcessorStateLock state_lock(cp);
	cp->SetFlip(f);
	cp->Flip();

	return 5;
}

LUMEN_CP_OP_PARSER(cp_op_increment_ce_counter)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0008400);
	EXIT_NOT_IMPLEMENTED(buffer[0] != 1);

	cp->IncremenetCe();

	return 1;
}

LUMEN_CP_OP_PARSER(cp_op_increment_de_counter)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0008500);
	EXIT_NOT_IMPLEMENTED(buffer[0] != 0);

	cp->IncremenetDe();

	return 1;
}

static uint32_t cp_update_index_buffer_size(CommandProcessor* cp, uint32_t cmd_id, const uint32_t* buffer, uint32_t dw)
{
	if (cp == nullptr || buffer == nullptr || cmd_id != LUMEN_PM4(2, Pm4::IT_INDEX_BUFFER_SIZE, 0) || dw < 2)
	{
		return 0;
	}

	cp->SetIndexBufferSize(buffer[0]);
	return 1;
}

LUMEN_CP_OP_PARSER(cp_op_index_buffer_size)
{
	const auto count = cp_update_index_buffer_size(cp, cmd_id, buffer, dw);
	EXIT_NOT_IMPLEMENTED(count == 0);
	return count;
}

LUMEN_CP_OP_PARSER(cp_op_index_type)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0002A00);

	cp->SetIndexType(buffer[0]);

	return 1;
}

LUMEN_CP_OP_PARSER(cp_op_indirect_buffer)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0023f02);

	EXIT_NOT_IMPLEMENTED((buffer[2] & 0xff00000u) != 0x1800000u);

	auto*    indirect_buffer = reinterpret_cast<uint32_t*>(buffer[0] | (static_cast<uint64_t>(buffer[1] & 0xffffu) << 32u));
	uint32_t indirect_num_dw = buffer[2] & 0xfffffu;

	EXIT_NOT_IMPLEMENTED(indirect_buffer == nullptr);
	EXIT_NOT_IMPLEMENTED(indirect_num_dw == 0);
	EXIT_IF(!GraphicsRenderPrepareCpuMemoryAccess(reinterpret_cast<uint64_t>(indirect_buffer), uint64_t(indirect_num_dw) * 4, GuestCpu::MemoryAccessIntent::Read));

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	if (!cp->IsHeadlessTestMode())
#endif
	{
		GraphicsDbgDumpDcb("ci", indirect_num_dw, indirect_buffer);
	}

	cp->Run(indirect_buffer, indirect_num_dw, nullptr, CommandStreamKind::Indirect);

	return 3;
}

LUMEN_CP_OP_PARSER(cp_op_indirect_cx_regs)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0021048);
	EXIT_NOT_IMPLEMENTED(dw < 4);

	auto*    indirect_buffer = reinterpret_cast<uint32_t*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
	uint32_t indirect_num_dw = buffer[0];

	EXIT_NOT_IMPLEMENTED(indirect_buffer == nullptr);
	EXIT_NOT_IMPLEMENTED(indirect_num_dw == 0);
	EXIT_IF(!GraphicsRenderPrepareCpuMemoryAccess(reinterpret_cast<uint64_t>(indirect_buffer), uint64_t(indirect_num_dw) * 8, GuestCpu::MemoryAccessIntent::Read));
	CommandProcessorStateLock state_lock(cp);

	for (uint32_t i = 0; i < indirect_num_dw; i++, indirect_buffer += 2)
	{
		auto cmd_offset = indirect_buffer[0];
		auto value      = indirect_buffer[1];

		EXIT_NOT_IMPLEMENTED(cmd_offset >= Pm4::CX_NUM);

		hw_ctx_observe_mode_control(cp->GetCtx(), cmd_offset, &value, 1);
		auto pfunc = g_hw_ctx_indirect_func[cmd_offset & (Pm4::CX_NUM - 1)];

		if (pfunc == nullptr)
		{
			EXIT("unknown cx reg at %05" PRIx32 ": 0x%" PRIx32 "\n", num_dw - dw, cmd_offset);
		}

		pfunc(cp, cmd_offset, value);
	}

	return 3;
}

LUMEN_CP_OP_PARSER(cp_op_indirect_sh_regs)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0021044);

	auto*    indirect_buffer = reinterpret_cast<uint32_t*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
	uint32_t indirect_num_dw = buffer[0];

	EXIT_NOT_IMPLEMENTED(indirect_buffer == nullptr);
	EXIT_NOT_IMPLEMENTED(indirect_num_dw == 0);
	EXIT_IF(!GraphicsRenderPrepareCpuMemoryAccess(reinterpret_cast<uint64_t>(indirect_buffer), uint64_t(indirect_num_dw) * 8, GuestCpu::MemoryAccessIntent::Read));
	CommandProcessorStateLock state_lock(cp);

	for (uint32_t i = 0; i < indirect_num_dw; i++, indirect_buffer += 2)
	{
		auto cmd_offset = indirect_buffer[0];
		auto value      = indirect_buffer[1];

		EXIT_NOT_IMPLEMENTED(cmd_offset >= Pm4::SH_NUM);

		auto pfunc = g_hw_sh_indirect_func[cmd_offset & (Pm4::SH_NUM - 1)];

		if (pfunc == nullptr)
		{
			EXIT("unknown sh reg at %05" PRIx32 ": 0x%" PRIx32 "\n", num_dw - dw, cmd_offset);
		}

		pfunc(cp, cmd_offset, value);
	}

	return 3;
}

LUMEN_CP_OP_PARSER(cp_op_indirect_uc_regs)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc002104c);

	auto*    indirect_buffer = reinterpret_cast<uint32_t*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
	uint32_t indirect_num_dw = buffer[0];

	EXIT_NOT_IMPLEMENTED(indirect_buffer == nullptr);
	EXIT_NOT_IMPLEMENTED(indirect_num_dw == 0);
	EXIT_IF(!GraphicsRenderPrepareCpuMemoryAccess(reinterpret_cast<uint64_t>(indirect_buffer), uint64_t(indirect_num_dw) * 8, GuestCpu::MemoryAccessIntent::Read));
	CommandProcessorStateLock state_lock(cp);

	for (uint32_t i = 0; i < indirect_num_dw; i++, indirect_buffer += 2)
	{
		auto cmd_offset = indirect_buffer[0];
		auto value      = indirect_buffer[1];

		EXIT_NOT_IMPLEMENTED(cmd_offset >= Pm4::UC_NUM);

		auto pfunc = g_hw_uc_indirect_func[cmd_offset & (Pm4::UC_NUM - 1)];

		if (pfunc == nullptr)
		{
			EXIT("unknown uc reg at %05" PRIx32 ": 0x%" PRIx32 "\n", num_dw - dw, cmd_offset);
		}

		pfunc(cp, cmd_offset, value);
	}

	return 3;
}

static uint32_t cp_op_marker(CommandProcessor* cp, uint32_t cmd_id, const uint32_t* buffer, Pm4NopMarkerDecision marker)
{
	LUMEN_PROFILER_FUNCTION();

	const uint32_t packet_dw = LUMEN_PM4_LEN(cmd_id);
	switch (marker)
	{
		case Pm4NopMarkerDecision::EmbeddedData:
			cp->SetEmbeddedDataMarker(buffer + 1, packet_dw - 2u, (buffer[0] >> 12u) & 0xfu);
			break;
		case Pm4NopMarkerDecision::UserDataVsharp: cp->SetUserDataMarker(HW::UserSgprType::Vsharp); break;
		case Pm4NopMarkerDecision::UserDataRegion: cp->SetUserDataMarker(HW::UserSgprType::Region); break;
		case Pm4NopMarkerDecision::Flip:
		{
			cp->Flip();
			break;
		}
		case Pm4NopMarkerDecision::FlipWithWrite:
		{
			auto*    addr  = reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
			uint32_t value = buffer[3];
			cp->Flip(addr, value);
			break;
		}
		case Pm4NopMarkerDecision::FlipWithInterrupt:
		{
			auto*    addr           = reinterpret_cast<void*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
			uint32_t value          = buffer[3];
			uint32_t eop_event_type = buffer[4];
			uint32_t cache_action   = buffer[5];
			cp->FlipWithInterrupt(eop_event_type, cache_action, addr, value);
			break;
		}
		default: EXIT_NOT_IMPLEMENTED(true); break;
	}

	return packet_dw - 1u;
}

LUMEN_CP_OP_PARSER(cp_op_nop)
{
	LUMEN_PROFILER_FUNCTION();

	auto r = LUMEN_PM4_R(cmd_id);

	if (r == Pm4::R_ZERO)
	{
		const uint32_t packet_dw = LUMEN_PM4_LEN(cmd_id);
		EXIT_NOT_IMPLEMENTED(cmd_id != LUMEN_PM4(packet_dw, Pm4::IT_NOP, Pm4::R_ZERO));
		const auto marker = pm4_nop_marker_decision(buffer, packet_dw, dw);
		EXIT_NOT_IMPLEMENTED(marker == Pm4NopMarkerDecision::Malformed);
		if (marker != Pm4NopMarkerDecision::OrdinaryNop)
		{
			return cp_op_marker(cp, cmd_id, buffer, marker);
		}
		return packet_dw - 1u;
	}

	auto hw_ctx = g_hw_sh_custom_func[r];
	auto cp_op  = g_cp_op_custom_func[r];

	if (hw_ctx != nullptr && cp_op == nullptr)
	{
		CommandProcessorStateLock state_lock(cp);
		return hw_ctx(cp, cmd_id, 0, buffer, dw);
	}

	if (hw_ctx == nullptr && cp_op != nullptr)
	{
		return cp_op(cp, cmd_id, buffer, dw, num_dw);
	}

	EXIT("unknown custom code at 0x%05" PRIx32 ": 0x%02" PRIx32 "\n", num_dw - dw, r);

	return 0;
}

LUMEN_CP_OP_PARSER(cp_op_clear_state)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(!pm4_clear_state_packet_supported(cmd_id, buffer, dw));
	CommandProcessorStateLock state_lock(cp);
	cp->GetCtx()->Reset();

	return 1;
}

LUMEN_CP_OP_PARSER(cp_op_context_control)
{
	LUMEN_PROFILER_FUNCTION();

	// The local state model has no represented effect for this exact observed
	// administrative form. Other headers and payloads remain fail-closed.
	EXIT_NOT_IMPLEMENTED(!pm4_context_control_packet_supported(cmd_id, buffer, dw));

	return 2;
}

LUMEN_CP_OP_PARSER(cp_op_num_instances)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0002f00);

	cp->SetNumInstances(buffer[0]);

	return 1;
}

LUMEN_CP_OP_PARSER(cp_op_pop_marker)
{
	LUMEN_PROFILER_FUNCTION();

	auto dw_num = (cmd_id >> 16u) & 0x3fffu;

	if (Config::GetPrintfDirection() != Log::Direction::Silent)
	{
		printf("Pop marker\n");
	}

	cp->PopMarker();

	return dw_num + 1;
}

LUMEN_CP_OP_PARSER(cp_op_push_marker)
{
	LUMEN_PROFILER_FUNCTION();

	auto dw_num = (cmd_id >> 16u) & 0x3fffu;

	const char* str = reinterpret_cast<const char*>(buffer);

	if (Config::GetPrintfDirection() != Log::Direction::Silent)
	{
		const auto max_length = static_cast<int>((dw_num + 1u) * sizeof(uint32_t));
		printf("Push marker: %.*s\n", max_length, str);
	}

	cp->PushMarker(str);

	return dw_num + 1;
}

LUMEN_CP_OP_PARSER(cp_op_release_mem)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0054902 && cmd_id != 0xc0051060);

	bool custom = (cmd_id == 0xc0051060);

	uint32_t cache_policy       = (buffer[0] >> 25u) & 0x3u;
	uint32_t cache_action       = (buffer[0] >> 12u) & 0x3fu;
	uint32_t eop_event_type     = (buffer[0]) & 0x3fu;
	uint32_t event_index        = (buffer[0] >> 8u) & 0x7u;
	uint32_t event_write_dest   = ((buffer[0] >> 23u) & 0x10u) | ((buffer[1] >> 16u) & 0x01u);
	uint32_t event_write_source = (buffer[1] >> 29u) & 0x7u;
	uint32_t legacy_interrupt_selector = (buffer[1] >> 24u) & 0x7u;
	uint32_t control_selector_byte      = (buffer[1] >> 24u) & 0xffu;
	auto*    dst_gpu_addr               = reinterpret_cast<void*>(buffer[2] | (static_cast<uint64_t>(buffer[3]) << 32u));
	uint64_t value                      = (buffer[4] | (static_cast<uint64_t>(buffer[5]) << 32u));

	if (custom)
	{
		uint32_t gcr_cntl = buffer[1] & 0xffffu;
		uint32_t data_sel = (buffer[1] >> 16u) & 0xffu;

		EXIT_NOT_IMPLEMENTED(data_sel != 2 && data_sel != 3);

		if (data_sel == 2)
		{
			EXIT_NOT_IMPLEMENTED(gcr_cntl != 0x0200);
			EXIT_NOT_IMPLEMENTED((buffer[0] >> 8u) != 0x3);

			if (gcr_cntl == 0x200)
			{
				cache_action = 0x38;
			}

			cache_policy       = 0;
			event_index        = 0;
			event_write_dest   = 0;
			event_write_source = 2;
			legacy_interrupt_selector = 0;
		} else if (data_sel == 3)
		{
			EXIT_NOT_IMPLEMENTED(gcr_cntl != 0x0000);
			EXIT_NOT_IMPLEMENTED((buffer[0] >> 8u) != 0x0);

			cache_action = 0x00;

			cache_policy       = 0;
			event_index        = 0;
			event_write_dest   = 0;
			event_write_source = 4;
			legacy_interrupt_selector = 0;
		}
	}

	cp->WriteAtEndOfPipe64(cache_policy, event_write_dest, eop_event_type, cache_action, event_index, event_write_source, dst_gpu_addr,
	                       value, legacy_interrupt_selector, control_selector_byte, cmd_id, buffer[0], buffer[1]);

	return 6;
}

LUMEN_CP_OP_PARSER(cp_op_set_context_reg)
{
	LUMEN_PROFILER_FUNCTION();

	auto cmd_offset = buffer[0];

	EXIT_NOT_IMPLEMENTED(cmd_offset >= Pm4::CX_NUM);

	CommandProcessorStateLock state_lock(cp);
	const uint32_t packet_dw = LUMEN_PM4_LEN(cmd_id);
	// The scissor handler validates its whole span before any effects. A valid
	// scissor span cannot include mode-control registers; do not let a malformed
	// oversized span mutate them through this observer before the handler rejects it.
	const bool viewport_scissor = cmd_offset >= Pm4::PA_SC_VPORT_SCISSOR_0_TL && cmd_offset <= Pm4::PA_SC_VPORT_SCISSOR_15_BR;
	if (!viewport_scissor && packet_dw >= 3 && packet_dw <= dw && cmd_id == LUMEN_PM4(packet_dw, Pm4::IT_SET_CONTEXT_REG, 0))
	{
		hw_ctx_observe_mode_control(cp->GetCtx(), cmd_offset, buffer + 1, packet_dw - 2);
	}
	auto pfunc = g_hw_ctx_func[cmd_offset & (Pm4::CX_NUM - 1)];

	if (pfunc == nullptr)
	{
		const bool has_value = dw >= 3 && LUMEN_PM4_LEN(cmd_id) >= 3;
		EXIT("unknown context register\n\t%05" PRIx32 ":\n\tcmd_id = %08" PRIx32 "\n\tcmd_offset = %08" PRIx32
		     "\n\tfirst_value_available = %u\n\tfirst_value = %08" PRIx32 "\n",
		     num_dw - dw, cmd_id, cmd_offset, (has_value ? 1u : 0u), (has_value ? buffer[1] : 0u));
	}

	auto s = pfunc(cp, cmd_id, cmd_offset, buffer + 1, dw);

	EXIT_IF(s == 0);

	return s + 1;
}

LUMEN_CP_OP_PARSER(cp_op_set_shader_reg)
{
	LUMEN_PROFILER_FUNCTION();

	auto cmd_offset = buffer[0];

	EXIT_NOT_IMPLEMENTED(cmd_offset >= Pm4::SH_NUM);

	auto pfunc = g_hw_sh_func[cmd_offset & (Pm4::SH_NUM - 1)];

	if (pfunc == nullptr)
	{
		EXIT("unknown shader register\n\t%05" PRIx32 ":\n\tcmd_id = %08" PRIx32 "\n\tcmd_offset = %08" PRIx32 "\n", num_dw - dw, cmd_id,
		     cmd_offset);
	}

	CommandProcessorStateLock state_lock(cp);
	auto                      s = pfunc(cp, cmd_id, cmd_offset, buffer + 1, dw);
	EXIT_IF(s == 0);
	return s + 1;
}

LUMEN_CP_OP_PARSER(cp_op_set_uconfig_reg)
{
	LUMEN_PROFILER_FUNCTION();

	auto cmd_offset = buffer[0] & 0xEFFFFFFFu; // ignore neo bit

	EXIT_NOT_IMPLEMENTED(cmd_offset >= Pm4::UC_NUM);

	auto pfunc = g_hw_uc_func[cmd_offset & (Pm4::UC_NUM - 1)];

	if (pfunc == nullptr)
	{
		EXIT("unknown user config register\n\t%05" PRIx32 ":\n\tcmd_id = %08" PRIx32 "\n\tcmd_offset = %08" PRIx32 "\n", num_dw - dw,
		     cmd_id, cmd_offset);
	}

	CommandProcessorStateLock state_lock(cp);
	auto                      s = pfunc(cp, cmd_id, cmd_offset, buffer + 1, dw);
	EXIT_IF(s == 0);
	return s + 1;
}

LUMEN_CP_OP_PARSER(cp_op_wait_flip_done)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0051018);

	auto video_out_handle     = buffer[0];
	auto display_buffer_index = buffer[1];

	cp->WaitFlipDone(video_out_handle, display_buffer_index);

	return 6;
}

LUMEN_CP_OP_PARSER(cp_op_wait_reg_mem_32)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC00C1028);

	auto* addr = reinterpret_cast<uint32_t*>(buffer[0] | (static_cast<uint64_t>(buffer[1]) << 32u));
	auto  mask = buffer[2];
	auto  func = buffer[3];
	auto  ref  = buffer[4];
	auto  poll = 10;

	cp->WaitRegMem32(func, addr, ref, mask, poll);

	return 13;
}

LUMEN_CP_OP_PARSER(cp_op_wait_reg_mem_64)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xc0071058);

	auto* addr = reinterpret_cast<uint64_t*>(buffer[0] | (static_cast<uint64_t>(buffer[1]) << 32u));
	auto  mask = buffer[2] | (static_cast<uint64_t>(buffer[3]) << 32u);
	auto  ref  = buffer[4] | (static_cast<uint64_t>(buffer[5]) << 32u);
	auto  func = buffer[6];
	auto  poll = buffer[7];

	cp->WaitRegMem64(func, addr, ref, mask, poll);

	return 8;
}

LUMEN_CP_OP_PARSER(cp_op_wait_on_ce_counter)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0008600);
	EXIT_NOT_IMPLEMENTED(buffer[0] != 1);

	cp->WaitCe();

	return 1;
}

LUMEN_CP_OP_PARSER(cp_op_wait_on_de_counter_diff)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0008800);

	cp->WaitDeDiff(buffer[0]);

	return 1;
}

LUMEN_CP_OP_PARSER(cp_op_wait_reg_mem)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(cmd_id != 0xC0053C00);

	auto  func = buffer[0] & 0x7u;
	bool  me   = (buffer[0] & 0x100u) == 0;
	bool  mem  = (buffer[0] & 0x10u) != 0;
	auto* addr = reinterpret_cast<uint32_t*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));
	auto  ref  = buffer[3];
	auto  mask = buffer[4];
	auto  poll = buffer[5];

	EXIT_NOT_IMPLEMENTED(!me);
	EXIT_NOT_IMPLEMENTED(!mem);

	cp->WaitRegMem32(func, addr, ref, mask, poll);

	return 6;
}

struct Pm4ConstRamWritePacket
{
	uint32_t offset = 0;
	uint32_t count = 0;
	const uint32_t* source = nullptr;
};

static bool pm4_decode_write_const_ram(uint32_t cmd_id, const uint32_t* body, uint32_t available_dw,
                                       Pm4ConstRamWritePacket* decoded)
{
	// The offset is one body word and the header count describes the following
	// source words. Validate both before even reading the offset.
	const uint32_t count = (cmd_id >> 16u) & 0x3fffu;
	if (body == nullptr || decoded == nullptr || available_dw < count + 2u) return false;
	*decoded = {body[0], count, body + 1};
	return true;
}

LUMEN_CP_OP_PARSER(cp_op_write_const_ram)
{
	LUMEN_PROFILER_FUNCTION();

	Pm4ConstRamWritePacket decoded;
	if (!pm4_decode_write_const_ram(cmd_id, buffer, dw, &decoded))
		const_ram_reject("write", "packet-span", 0, (cmd_id >> 16u) & 0x3fffu);

	cp->WriteConstRam(decoded.offset, decoded.source, decoded.count);

	return 1 + decoded.count;
}

LUMEN_CP_OP_PARSER(cp_op_write_data)
{
	LUMEN_PROFILER_FUNCTION();

	auto op = (cmd_id >> 8u) & 0xffu;

	EXIT_NOT_IMPLEMENTED(op != Pm4::IT_WRITE_DATA && op != Pm4::IT_NOP);

	bool custom = (op == Pm4::IT_NOP);

	auto dw_num = (cmd_id >> 16u) & 0x3fffu;
	EXIT_NOT_IMPLEMENTED(dw_num < 2);

	auto  write_control = buffer[0];
	auto* dst           = reinterpret_cast<uint32_t*>(buffer[1] | (static_cast<uint64_t>(buffer[2]) << 32u));

	cp->WriteData(dst, buffer + 3, dw_num - 2, write_control, custom);

	return 1 + dw_num;
}

static void graphics_init_jmp_tables_cx_indirect()
{
	for (auto& func: g_hw_ctx_indirect_func)
	{
		func = nullptr;
	}
	for (const auto reg: {Pm4::DB_ALPHA_TO_MASK, Pm4::IA_MULTI_VGT_PARAM, Pm4::PA_SC_MODE_CNTL_1})
	{
		g_hw_ctx_indirect_func[reg] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			EXIT_NOT_IMPLEMENTED(!hw_ctx_write_draw_control(cp->GetCtx(), cmd_offset, value));
		};
	}
	for (const auto reg: {Pm4::PA_SC_AA_MASK_X0Y0_X1Y0, Pm4::PA_SC_AA_MASK_X0Y1_X1Y1})
	{
		g_hw_ctx_indirect_func[reg] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			EXIT_NOT_IMPLEMENTED(!hw_ctx_write_aa_mask(cp->GetCtx(), cmd_offset, value));
		};
	}
	for (uint32_t reg = Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL; reg <= Pm4::PA_SU_POLY_OFFSET_BACK_OFFSET; ++reg)
	{
		g_hw_ctx_indirect_func[reg] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			EXIT_NOT_IMPLEMENTED(cmd_offset < Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL || cmd_offset > Pm4::PA_SU_POLY_OFFSET_BACK_OFFSET);
			cp->GetCtx()->SetPolygonOffsetRegister(cmd_offset - Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL, value);
		};
	}

	for (auto cmd_offset = Pm4::SPI_PS_INPUT_CNTL_0; cmd_offset <= Pm4::SPI_PS_INPUT_CNTL_31; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = cmd_offset - Pm4::SPI_PS_INPUT_CNTL_0;
			cp->GetCtx()->SetPsInputSettings(slot, value);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_BASE; cmd_offset <= Pm4::CB_COLOR7_BASE; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_BASE) / 15;
			auto     base = cp->GetCtx()->GetRenderTarget(slot).base;
			base.addr &= 0xFFFFFF00000000FFull;
			base.addr |= static_cast<uint64_t>(value) << 8u;
			cp->GetCtx()->SetColorBase(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_BASE_EXT; cmd_offset <= Pm4::CB_COLOR7_BASE_EXT; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_BASE_EXT);
			auto     base = cp->GetCtx()->GetRenderTarget(slot).base;
			base.addr &= 0xFFFF00FFFFFFFFFFull;
			base.addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			cp->GetCtx()->SetColorBase(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_VIEW; cmd_offset <= Pm4::CB_COLOR7_VIEW; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t      slot = (cmd_offset - Pm4::CB_COLOR0_VIEW) / 15;
			HW::ColorView view;
			view.base_array_slice_index = LUMEN_PM4_GET(value, CB_COLOR0_VIEW, SLICE_START);
			view.last_array_slice_index = LUMEN_PM4_GET(value, CB_COLOR0_VIEW, SLICE_MAX);
			view.current_mip_level      = LUMEN_PM4_GET(value, CB_COLOR0_VIEW, MIP_LEVEL);
			cp->GetCtx()->SetColorView(slot, view);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_INFO; cmd_offset <= Pm4::CB_COLOR7_INFO; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t      slot = (cmd_offset - Pm4::CB_COLOR0_INFO) / 15;
			HW::ColorInfo info;
			info.format                         = LUMEN_PM4_GET(value, CB_COLOR0_INFO, FORMAT);
			info.channel_type                   = LUMEN_PM4_GET(value, CB_COLOR0_INFO, NUMBER_TYPE);
			info.channel_order                  = LUMEN_PM4_GET(value, CB_COLOR0_INFO, COMP_SWAP);
			info.cmask_fast_clear_enable        = LUMEN_PM4_GET(value, CB_COLOR0_INFO, FAST_CLEAR) != 0;
			info.fmask_compression_enable       = LUMEN_PM4_GET(value, CB_COLOR0_INFO, COMPRESSION) != 0;
			info.blend_clamp                    = LUMEN_PM4_GET(value, CB_COLOR0_INFO, BLEND_CLAMP) != 0;
			info.blend_bypass                   = LUMEN_PM4_GET(value, CB_COLOR0_INFO, BLEND_BYPASS) != 0;
			info.round_mode                     = LUMEN_PM4_GET(value, CB_COLOR0_INFO, ROUND_MODE) != 0;
			info.cmask_tile_mode                = LUMEN_PM4_GET(value, CB_COLOR0_INFO, CMASK_IS_LINEAR);
			info.fmask_data_compression_disable = LUMEN_PM4_GET(value, CB_COLOR0_INFO, FMASK_COMPRESSION_DISABLE) != 0;
			info.fmask_one_frag_mode            = LUMEN_PM4_GET(value, CB_COLOR0_INFO, FMASK_COMPRESS_1FRAG_ONLY) != 0;
			info.dcc_compression_enable         = LUMEN_PM4_GET(value, CB_COLOR0_INFO, DCC_ENABLE) != 0;
			info.cmask_tile_mode_neo            = LUMEN_PM4_GET(value, CB_COLOR0_INFO, CMASK_ADDR_TYPE);
			info.neo_mode                       = LUMEN_PM4_GET(value, CB_COLOR0_INFO, ALT_TILE_MODE) != 0;
			cp->GetCtx()->SetColorInfo(slot, info);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_ATTRIB; cmd_offset <= Pm4::CB_COLOR7_ATTRIB; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t        slot = (cmd_offset - Pm4::CB_COLOR0_ATTRIB) / 15;
			HW::ColorAttrib attrib;
			attrib.force_dest_alpha_to_one = LUMEN_PM4_GET(value, CB_COLOR0_ATTRIB, FORCE_DST_ALPHA_1) != 0;
			attrib.tile_mode               = LUMEN_PM4_GET(value, CB_COLOR0_ATTRIB, TILE_MODE_INDEX);
			attrib.fmask_tile_mode         = LUMEN_PM4_GET(value, CB_COLOR0_ATTRIB, FMASK_TILE_MODE_INDEX);
			attrib.num_samples             = LUMEN_PM4_GET(value, CB_COLOR0_ATTRIB, NUM_SAMPLES);
			attrib.num_fragments           = LUMEN_PM4_GET(value, CB_COLOR0_ATTRIB, NUM_FRAGMENTS);
			cp->GetCtx()->SetColorAttrib(slot, attrib);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_DCC_CONTROL; cmd_offset <= Pm4::CB_COLOR7_DCC_CONTROL; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t            slot = (cmd_offset - Pm4::CB_COLOR0_DCC_CONTROL) / 15;
			HW::ColorDccControl dcc;
			dcc.overwrite_combiner_disable     = LUMEN_PM4_GET(value, CB_COLOR0_DCC_CONTROL, OVERWRITE_COMBINER_DISABLE) != 0;
			dcc.dcc_clear_key_enable           = LUMEN_PM4_GET(value, CB_COLOR0_DCC_CONTROL, KEY_CLEAR_ENABLE) != 0;
			dcc.max_uncompressed_block_size    = LUMEN_PM4_GET(value, CB_COLOR0_DCC_CONTROL, MAX_UNCOMPRESSED_BLOCK_SIZE);
			dcc.min_compressed_block_size      = LUMEN_PM4_GET(value, CB_COLOR0_DCC_CONTROL, MIN_COMPRESSED_BLOCK_SIZE);
			dcc.max_compressed_block_size      = LUMEN_PM4_GET(value, CB_COLOR0_DCC_CONTROL, MAX_COMPRESSED_BLOCK_SIZE);
			dcc.color_transform                = LUMEN_PM4_GET(value, CB_COLOR0_DCC_CONTROL, COLOR_TRANSFORM);
			dcc.independent_64b_blocks         = LUMEN_PM4_GET(value, CB_COLOR0_DCC_CONTROL, INDEPENDENT_64B_BLOCKS) != 0;
			dcc.data_write_on_dcc_clear_to_reg = LUMEN_PM4_GET(value, CB_COLOR0_DCC_CONTROL, ENABLE_CONSTANT_ENCODE_REG_WRITE) != 0;
			dcc.independent_128b_blocks        = LUMEN_PM4_GET(value, CB_COLOR0_DCC_CONTROL, INDEPENDENT_128B_BLOCKS) != 0;
			cp->GetCtx()->SetColorDccControl(slot, dcc);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_CMASK; cmd_offset <= Pm4::CB_COLOR7_CMASK; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_CMASK) / 15;
			auto     base = cp->GetCtx()->GetRenderTarget(slot).cmask;
			base.addr &= 0xFFFFFF00000000FFull;
			base.addr |= static_cast<uint64_t>(value) << 8u;
			cp->GetCtx()->SetColorCmask(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_CMASK_BASE_EXT; cmd_offset <= Pm4::CB_COLOR7_CMASK_BASE_EXT; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_CMASK_BASE_EXT);
			auto     base = cp->GetCtx()->GetRenderTarget(slot).cmask;
			base.addr &= 0xFFFF00FFFFFFFFFFull;
			base.addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			cp->GetCtx()->SetColorCmask(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_FMASK; cmd_offset <= Pm4::CB_COLOR7_FMASK; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_FMASK) / 15;
			auto     base = cp->GetCtx()->GetRenderTarget(slot).fmask;
			base.addr &= 0xFFFFFF00000000FFull;
			base.addr |= static_cast<uint64_t>(value) << 8u;
			cp->GetCtx()->SetColorFmask(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_FMASK_BASE_EXT; cmd_offset <= Pm4::CB_COLOR7_FMASK_BASE_EXT; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_FMASK_BASE_EXT);
			auto     base = cp->GetCtx()->GetRenderTarget(slot).fmask;
			base.addr &= 0xFFFF00FFFFFFFFFFull;
			base.addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			cp->GetCtx()->SetColorFmask(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_CLEAR_WORD0; cmd_offset <= Pm4::CB_COLOR7_CLEAR_WORD0; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			HW::ColorClearWord0 clear_word0;
			clear_word0.word0 = value;
			uint32_t slot     = (cmd_offset - Pm4::CB_COLOR0_CLEAR_WORD0) / 15;
			cp->GetCtx()->SetColorClearWord0(slot, clear_word0);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_CLEAR_WORD1; cmd_offset <= Pm4::CB_COLOR7_CLEAR_WORD1; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			HW::ColorClearWord1 clear_word1;
			clear_word1.word1 = value;
			uint32_t slot     = (cmd_offset - Pm4::CB_COLOR0_CLEAR_WORD1) / 15;
			cp->GetCtx()->SetColorClearWord1(slot, clear_word1);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_DCC_BASE; cmd_offset <= Pm4::CB_COLOR7_DCC_BASE; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_DCC_BASE) / 15;
			auto     base = cp->GetCtx()->GetRenderTarget(slot).dcc_addr;
			base.addr &= 0xFFFFFF00000000FFull;
			base.addr |= static_cast<uint64_t>(value) << 8u;
			cp->GetCtx()->SetColorDccAddr(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_DCC_BASE_EXT; cmd_offset <= Pm4::CB_COLOR7_DCC_BASE_EXT; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_DCC_BASE_EXT);
			auto     base = cp->GetCtx()->GetRenderTarget(slot).dcc_addr;
			base.addr &= 0xFFFF00FFFFFFFFFFull;
			base.addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			cp->GetCtx()->SetColorDccAddr(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_ATTRIB2; cmd_offset <= Pm4::CB_COLOR7_ATTRIB2; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t         slot = (cmd_offset - Pm4::CB_COLOR0_ATTRIB2);
			HW::ColorAttrib2 attrib2;
			attrib2.height         = LUMEN_PM4_GET(value, CB_COLOR0_ATTRIB2, MIP0_HEIGHT);
			attrib2.width          = LUMEN_PM4_GET(value, CB_COLOR0_ATTRIB2, MIP0_WIDTH);
			attrib2.num_mip_levels = LUMEN_PM4_GET(value, CB_COLOR0_ATTRIB2, MAX_MIP);
			cp->GetCtx()->SetColorAttrib2(slot, attrib2);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_ATTRIB3; cmd_offset <= Pm4::CB_COLOR7_ATTRIB3; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t         slot = (cmd_offset - Pm4::CB_COLOR0_ATTRIB3);
			HW::ColorAttrib3 attrib3;
			attrib3.depth              = LUMEN_PM4_GET(value, CB_COLOR0_ATTRIB3, MIP0_DEPTH);
			attrib3.tile_mode          = LUMEN_PM4_GET(value, CB_COLOR0_ATTRIB3, COLOR_SW_MODE);
			attrib3.dimension          = LUMEN_PM4_GET(value, CB_COLOR0_ATTRIB3, RESOURCE_TYPE);
			attrib3.cmask_pipe_aligned = LUMEN_PM4_GET(value, CB_COLOR0_ATTRIB3, CMASK_PIPE_ALIGNED);
			attrib3.dcc_pipe_aligned   = LUMEN_PM4_GET(value, CB_COLOR0_ATTRIB3, DCC_PIPE_ALIGNED);
			cp->GetCtx()->SetColorAttrib3(slot, attrib3);
		};
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_XSCALE; cmd_offset <= Pm4::PA_CL_VPORT_XSCALE_15; cmd_offset += 6)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportXScale((cmd_offset - Pm4::PA_CL_VPORT_XSCALE) / 6, *reinterpret_cast<const float*>(&value)); };
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_XOFFSET; cmd_offset <= Pm4::PA_CL_VPORT_XOFFSET_15; cmd_offset += 6)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportXOffset((cmd_offset - Pm4::PA_CL_VPORT_XOFFSET) / 6, *reinterpret_cast<const float*>(&value)); };
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_YSCALE; cmd_offset <= Pm4::PA_CL_VPORT_YSCALE_15; cmd_offset += 6)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportYScale((cmd_offset - Pm4::PA_CL_VPORT_YSCALE) / 6, *reinterpret_cast<const float*>(&value)); };
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_YOFFSET; cmd_offset <= Pm4::PA_CL_VPORT_YOFFSET_15; cmd_offset += 6)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportYOffset((cmd_offset - Pm4::PA_CL_VPORT_YOFFSET) / 6, *reinterpret_cast<const float*>(&value)); };
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_ZSCALE; cmd_offset <= Pm4::PA_CL_VPORT_ZSCALE_15; cmd_offset += 6)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportZScale((cmd_offset - Pm4::PA_CL_VPORT_ZSCALE) / 6, *reinterpret_cast<const float*>(&value)); };
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_ZOFFSET; cmd_offset <= Pm4::PA_CL_VPORT_ZOFFSET_15; cmd_offset += 6)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportZOffset((cmd_offset - Pm4::PA_CL_VPORT_ZOFFSET) / 6, *reinterpret_cast<const float*>(&value)); };
	}

	for (auto cmd_offset = Pm4::PA_SC_VPORT_SCISSOR_0_TL; cmd_offset <= Pm4::PA_SC_VPORT_SCISSOR_15_BR; ++cmd_offset)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{ EXIT_IF(!hw_ctx_write_viewport_scissor(cp->GetCtx(), cmd_offset, value)); };
	}

	for (auto cmd_offset = Pm4::PA_SC_VPORT_ZMIN_0; cmd_offset <= Pm4::PA_SC_VPORT_ZMIN_15; cmd_offset += 2)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportZMin((cmd_offset - Pm4::PA_SC_VPORT_ZMIN_0) / 2, *reinterpret_cast<const float*>(&value)); };
	}

	for (auto cmd_offset = Pm4::PA_SC_VPORT_ZMAX_0; cmd_offset <= Pm4::PA_SC_VPORT_ZMAX_15; cmd_offset += 2)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportZMax((cmd_offset - Pm4::PA_SC_VPORT_ZMAX_0) / 2, *reinterpret_cast<const float*>(&value)); };
	}

	g_hw_ctx_indirect_func[Pm4::SPI_VS_OUT_CONFIG] = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetVsOutConfig(value); };

	g_hw_ctx_indirect_func[Pm4::SPI_SHADER_POS_FORMAT] = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetShaderPosFormat(value); };
	g_hw_ctx_indirect_func[Pm4::SPI_SHADER_IDX_FORMAT] = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetShaderIdxFormat(value); };
	g_hw_ctx_indirect_func[Pm4::PA_CL_VS_OUT_CNTL]     = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetClVsOutCntl(value); };
	g_hw_ctx_indirect_func[Pm4::GE_NGG_SUBGRP_CNTL]    = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetNggSubgrpCntl(value); };
	g_hw_ctx_indirect_func[Pm4::VGT_GS_INSTANCE_CNT]   = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetGsInstanceCnt(value); };
	g_hw_ctx_indirect_func[Pm4::VGT_GS_ONCHIP_CNTL]    = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetGsOnchipCntl(value); };
	g_hw_ctx_indirect_func[Pm4::VGT_GS_MODE]           = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetGsMode(value); };
	g_hw_ctx_indirect_func[Pm4::VGT_STRMOUT_CONFIG]    = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetStreamoutConfig(value); };

	g_hw_ctx_indirect_func[Pm4::GE_MAX_OUTPUT_PER_SUBGROUP] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{ cp->GetCtx()->SetMaxOutputPerSubgroup(value); };

	for (uint32_t reg = Pm4::VGT_INSTANCE_STEP_RATE_0; reg <= Pm4::VGT_INSTANCE_STEP_RATE_1; ++reg)
	{
		g_hw_ctx_indirect_func[reg] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetInstanceStepRate(cmd_offset - Pm4::VGT_INSTANCE_STEP_RATE_0, value); };
	}

	g_hw_ctx_indirect_func[Pm4::VGT_ESGS_RING_ITEMSIZE] = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetEsgsRingItemsize(value); };
	g_hw_ctx_indirect_func[Pm4::VGT_GS_MAX_VERT_OUT]    = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetGsMaxVertOut(value); };
	g_hw_ctx_indirect_func[Pm4::VGT_SHADER_STAGES_EN]   = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetShaderStages(value); };
	g_hw_ctx_indirect_func[Pm4::VGT_GS_OUT_PRIM_TYPE]   = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetGsOutPrimType(value); };
	g_hw_ctx_indirect_func[Pm4::SPI_SHADER_Z_FORMAT]    = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetShaderZFormat(value); };

	g_hw_ctx_indirect_func[Pm4::SPI_SHADER_COL_FORMAT] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{
		for (uint32_t i = 0; i < 8; i++)
		{
			cp->GetCtx()->SetTargetOutputMode(i, (value >> (i * 4)) & 0xFu);
		}
	};

	g_hw_ctx_indirect_func[Pm4::SPI_PS_INPUT_ENA]  = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetPsInputEna(value); };
	g_hw_ctx_indirect_func[Pm4::SPI_PS_INPUT_ADDR] = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetPsInputAddr(value); };
	g_hw_ctx_indirect_func[Pm4::SPI_PS_IN_CONTROL] = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetPsInControl(value); };
	g_hw_ctx_indirect_func[Pm4::SPI_BARYC_CNTL]    = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetBarycCntl(value); };

	g_hw_ctx_indirect_func[Pm4::DB_SHADER_CONTROL] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{
		HW::DepthShaderControl db_shader_control {};
		db_shader_control.other_bits                  = value & 0xFFFF9B8Eu;
		db_shader_control.conservative_z_export_value = LUMEN_PM4_GET(value, DB_SHADER_CONTROL, CONSERVATIVE_Z_EXPORT);
		db_shader_control.shader_z_behavior           = LUMEN_PM4_GET(value, DB_SHADER_CONTROL, Z_ORDER);
		db_shader_control.shader_kill_enable          = LUMEN_PM4_GET(value, DB_SHADER_CONTROL, KILL_ENABLE) != 0;
		db_shader_control.shader_z_export_enable      = LUMEN_PM4_GET(value, DB_SHADER_CONTROL, Z_EXPORT_ENABLE) != 0;
		db_shader_control.shader_execute_on_noop      = LUMEN_PM4_GET(value, DB_SHADER_CONTROL, EXEC_ON_NOOP) != 0;
		cp->GetCtx()->SetDepthShaderControl(db_shader_control);
	};

	g_hw_ctx_indirect_func[Pm4::CB_SHADER_MASK]       = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetShaderMask(value); };
	g_hw_ctx_indirect_func[Pm4::PA_SC_SHADER_CONTROL] = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetScShaderControl(value); };
	g_hw_ctx_indirect_func[Pm4::CB_TARGET_MASK]       = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetRenderTargetMask(value); };
	g_hw_ctx_indirect_func[Pm4::PA_CL_CLIP_CNTL]      = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{ cp->GetCtx()->SetClipControl(decode_clip_control(value)); };

	g_hw_ctx_indirect_func[Pm4::DB_Z_INFO] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{
		HW::DepthZInfo r;
		r.format                    = LUMEN_PM4_GET(value, DB_Z_INFO, FORMAT);
		r.num_samples               = LUMEN_PM4_GET(value, DB_Z_INFO, NUM_SAMPLES);
		r.embedded_sample_locations = LUMEN_PM4_GET(value, DB_Z_INFO, ITERATE_FLUSH) != 0;
		r.partially_resident        = LUMEN_PM4_GET(value, DB_Z_INFO, PARTIALLY_RESIDENT) != 0;
		r.num_mip_levels            = LUMEN_PM4_GET(value, DB_Z_INFO, MAXMIP);
		r.tile_mode_index           = LUMEN_PM4_GET(value, DB_Z_INFO, TILE_MODE_INDEX);
		r.plane_compression         = LUMEN_PM4_GET(value, DB_Z_INFO, DECOMPRESS_ON_N_ZPLANES);
		r.expclear_enabled          = LUMEN_PM4_GET(value, DB_Z_INFO, ALLOW_EXPCLEAR) != 0;
		r.tile_surface_enable       = LUMEN_PM4_GET(value, DB_Z_INFO, TILE_SURFACE_ENABLE) != 0;
		r.zrange_precision          = LUMEN_PM4_GET(value, DB_Z_INFO, ZRANGE_PRECISION);
		r.unsupported_bits          = value & ~depth_z_info_modeled_mask;
		cp->GetCtx()->SetDepthZInfo(r);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_INFO] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{
		HW::DepthStencilInfo r;
		r.format                     = LUMEN_PM4_GET(value, DB_STENCIL_INFO, FORMAT);
		r.texture_compatible_stencil = LUMEN_PM4_GET(value, DB_STENCIL_INFO, ITERATE_FLUSH) != 0;
		r.partially_resident         = LUMEN_PM4_GET(value, DB_STENCIL_INFO, PARTIALLY_RESIDENT) != 0;
		r.tile_split                 = LUMEN_PM4_GET(value, DB_STENCIL_INFO, RESERVED_FIELD_1);
		r.tile_mode_index            = LUMEN_PM4_GET(value, DB_STENCIL_INFO, TILE_MODE_INDEX);
		r.expclear_enabled           = LUMEN_PM4_GET(value, DB_STENCIL_INFO, ALLOW_EXPCLEAR) != 0;
		r.tile_stencil_disable       = LUMEN_PM4_GET(value, DB_STENCIL_INFO, TILE_STENCIL_DISABLE) != 0;
		r.unsupported_bits           = value & ~depth_stencil_info_modeled_mask;
		cp->GetCtx()->SetDepthStencilInfo(r);
	};

	g_hw_ctx_indirect_func[Pm4::DB_Z_READ_BASE] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().z_read_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthZReadBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_Z_READ_BASE_HI] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().z_read_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthZReadBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_READ_BASE] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().stencil_read_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthStencilReadBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_READ_BASE_HI] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().stencil_read_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthStencilReadBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_Z_WRITE_BASE] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().z_write_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthZWriteBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_Z_WRITE_BASE_HI] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().z_write_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthZWriteBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_WRITE_BASE] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().stencil_write_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthStencilWriteBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_WRITE_BASE_HI] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().stencil_write_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthStencilWriteBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_HTILE_DATA_BASE] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().htile_data_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthHTileDataBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_HTILE_DATA_BASE_HI] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().htile_data_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthHTileDataBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_VIEW] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{
		HW::DepthDepthView r;
		r.slice_start = LUMEN_PM4_GET(value, DB_DEPTH_VIEW, SLICE_START) + (LUMEN_PM4_GET(value, DB_DEPTH_VIEW, SLICE_START_HI) << 11u);
		r.slice_max   = LUMEN_PM4_GET(value, DB_DEPTH_VIEW, SLICE_MAX) + (LUMEN_PM4_GET(value, DB_DEPTH_VIEW, SLICE_MAX_HI) << 11u);
		r.depth_write_disable   = LUMEN_PM4_GET(value, DB_DEPTH_VIEW, Z_READ_ONLY) != 0;
		r.stencil_write_disable = LUMEN_PM4_GET(value, DB_DEPTH_VIEW, STENCIL_READ_ONLY) != 0;
		r.current_mip_level     = LUMEN_PM4_GET(value, DB_DEPTH_VIEW, MIPID);
		cp->GetCtx()->SetDepthDepthView(r);
	};

	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_SIZE_XY] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{
		HW::DepthDepthSizeXY r;
		r.x_max = LUMEN_PM4_GET(value, DB_DEPTH_SIZE_XY, X_MAX);
		r.y_max = LUMEN_PM4_GET(value, DB_DEPTH_SIZE_XY, Y_MAX);
		cp->GetCtx()->SetDepthDepthSizeXY(r);
	};

	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_CLEAR] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{ cp->GetCtx()->SetDepthClearValue(*reinterpret_cast<const float*>(&value)); };
	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_CLEAR] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{ cp->GetCtx()->SetStencilClearValue(LUMEN_PM4_GET(value, DB_STENCIL_CLEAR, CLEAR)); };
	g_hw_ctx_indirect_func[Pm4::PA_CL_VTE_CNTL] = [](LUMEN_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetViewportTransformControl(value); };

	g_hw_ctx_indirect_func[Pm4::PA_SC_MODE_CNTL_0] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{
		HW::ScanModeControl r;
		r.msaa_enable          = LUMEN_PM4_GET(value, PA_SC_MODE_CNTL_0, MSAA_ENABLE) != 0;
		r.vport_scissor_enable = LUMEN_PM4_GET(value, PA_SC_MODE_CNTL_0, VPORT_SCISSOR_ENABLE) != 0;
		r.line_stipple_enable  = LUMEN_PM4_GET(value, PA_SC_MODE_CNTL_0, LINE_STIPPLE_ENABLE) != 0;
		cp->GetCtx()->SetScanModeControl(r);
	};

	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_CONTROL] = [](LUMEN_HW_CTX_INDIRECT_ARGS)
	{
		HW::DepthControl r;
		r.stencil_enable                     = LUMEN_PM4_GET(value, DB_DEPTH_CONTROL, STENCIL_ENABLE) != 0;
		r.z_enable                           = LUMEN_PM4_GET(value, DB_DEPTH_CONTROL, Z_ENABLE) != 0;
		r.z_write_enable                     = LUMEN_PM4_GET(value, DB_DEPTH_CONTROL, Z_WRITE_ENABLE) != 0;
		r.depth_bounds_enable                = LUMEN_PM4_GET(value, DB_DEPTH_CONTROL, DEPTH_BOUNDS_ENABLE) != 0;
		r.zfunc                              = LUMEN_PM4_GET(value, DB_DEPTH_CONTROL, ZFUNC);
		r.backface_enable                    = LUMEN_PM4_GET(value, DB_DEPTH_CONTROL, BACKFACE_ENABLE) != 0;
		r.stencilfunc                        = LUMEN_PM4_GET(value, DB_DEPTH_CONTROL, STENCILFUNC);
		r.stencilfunc_bf                     = LUMEN_PM4_GET(value, DB_DEPTH_CONTROL, STENCILFUNC_BF);
		r.color_writes_on_depth_fail_enable  = LUMEN_PM4_GET(value, DB_DEPTH_CONTROL, ENABLE_COLOR_WRITES_ON_DEPTH_FAIL) != 0;
		r.color_writes_on_depth_pass_disable = LUMEN_PM4_GET(value, DB_DEPTH_CONTROL, DISABLE_COLOR_WRITES_ON_DEPTH_PASS) != 0;
		r.raw                                = value;
		r.unsupported_bits                   = value & ~depth_control_modeled_mask;
		cp->GetCtx()->SetDepthControl(r);
	};
}

static void graphics_init_jmp_tables_sh_indirect()
{
	for (auto& func: g_hw_sh_indirect_func)
	{
		func = nullptr;
	}

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_LO_ES] = [](LUMEN_HW_SH_INDIRECT_ARGS)
	{
		auto base = cp->GetShCtx()->GetVs().es_regs.data_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetShCtx()->SetEsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_HI_ES] = [](LUMEN_HW_SH_INDIRECT_ARGS)
	{
		auto base = cp->GetShCtx()->GetVs().es_regs.data_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetShCtx()->SetEsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_CHKSUM_GS] = [](LUMEN_HW_SH_INDIRECT_ARGS) { cp->GetShCtx()->SetGsShaderChksum(value); };

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC1_GS] = [](LUMEN_HW_SH_INDIRECT_ARGS)
	{
		HW::GsShaderResource1 r1;
		r1.vgprs                    = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, VGPRS);
		r1.sgprs                    = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, SGPRS);
		r1.priority                 = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, PRIORITY);
		r1.float_mode               = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, FLOAT_MODE);
		r1.dx10_clamp               = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, DX10_CLAMP) != 0;
		r1.debug_mode               = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, DEBUG_MODE) != 0;
		r1.ieee_mode                = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, IEEE_MODE) != 0;
		r1.cu_group_enable          = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, CU_GROUP_ENABLE) != 0;
		r1.require_forward_progress = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, FWD_PROGRESS) != 0;
		r1.lds_configuration        = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, WGP_MODE) != 0;
		r1.gs_vgpr_component_count  = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, GS_VGPR_COMP_CNT);
		r1.fp16_overflow            = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, FP16_OVFL) != 0;
		cp->GetShCtx()->SetGsShaderResource1(r1);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC2_GS] = [](LUMEN_HW_SH_INDIRECT_ARGS)
	{
		HW::GsShaderResource2 r2;
		r2.scratch_en = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, SCRATCH_EN) != 0;
		r2.user_sgpr =
		    LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR) + (LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB) << 5u);
		r2.es_vgpr_component_count = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, ES_VGPR_COMP_CNT);
		r2.offchip_lds             = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, OC_LDS_EN) != 0;
		r2.lds_size                = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, LDS_SIZE);
		r2.shared_vgprs            = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, SHARED_VGPR_CNT);
		cp->GetShCtx()->SetGsShaderResource2(r2);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_LO_PS] = [](LUMEN_HW_SH_INDIRECT_ARGS)
	{
		auto base = cp->GetShCtx()->GetPs().ps_regs.data_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetShCtx()->SetPsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_HI_PS] = [](LUMEN_HW_SH_INDIRECT_ARGS)
	{
		auto base = cp->GetShCtx()->GetPs().ps_regs.data_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetShCtx()->SetPsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_CHKSUM_PS] = [](LUMEN_HW_SH_INDIRECT_ARGS) { cp->GetShCtx()->SetPsShaderChksum(value); };

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC1_PS] = [](LUMEN_HW_SH_INDIRECT_ARGS)
	{
		HW::PsShaderResource1 r1;
		r1.vgprs                    = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, VGPRS);
		r1.sgprs                    = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, SGPRS);
		r1.priority                 = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, PRIORITY);
		r1.float_mode               = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, FLOAT_MODE);
		r1.dx10_clamp               = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, DX10_CLAMP) != 0;
		r1.debug_mode               = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, DEBUG_MODE) != 0;
		r1.ieee_mode                = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, IEEE_MODE) != 0;
		r1.cu_group_disable         = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, CU_GROUP_DISABLE) != 0;
		r1.require_forward_progress = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, FWD_PROGRESS) != 0;
		r1.fp16_overflow            = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, FP16_OVFL) != 0;
		cp->GetShCtx()->SetPsShaderResource1(r1);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC2_PS] = [](LUMEN_HW_SH_INDIRECT_ARGS)
	{
		HW::PsShaderResource2 r2;
		r2.scratch_en = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, SCRATCH_EN);
		r2.user_sgpr =
		    LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR) + (LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR_MSB) << 5u);
		r2.wave_cnt_en            = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, WAVE_CNT_EN);
		r2.extra_lds_size         = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, EXTRA_LDS_SIZE);
		r2.raster_ordered_shading = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, LOAD_INTRAWAVE_COLLISION);
		r2.shared_vgprs           = LUMEN_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, SHARED_VGPR_CNT);
		cp->GetShCtx()->SetPsShaderResource2(r2);
	};
}

static void graphics_init_jmp_tables_uc_indirect()
{
	for (auto& func: g_hw_uc_indirect_func)
	{
		func = nullptr;
	}

	g_hw_uc_indirect_func[Pm4::GE_CNTL] = [](LUMEN_HW_UC_INDIRECT_ARGS)
	{
		HW::GeControl r;
		r.primitive_group_size = LUMEN_PM4_GET(value, GE_CNTL, PRIM_GRP_SIZE);
		r.vertex_group_size    = LUMEN_PM4_GET(value, GE_CNTL, VERT_GRP_SIZE);
		cp->GetUcfg()->SetGeControl(r);
	};

	g_hw_uc_indirect_func[Pm4::GE_USER_VGPR_EN] = [](LUMEN_HW_UC_INDIRECT_ARGS)
	{
		HW::GeUserVgprEn r;
		r.vgpr1 = LUMEN_PM4_GET(value, GE_USER_VGPR_EN, EN_USER_VGPR1) != 0;
		r.vgpr2 = LUMEN_PM4_GET(value, GE_USER_VGPR_EN, EN_USER_VGPR2) != 0;
		r.vgpr3 = LUMEN_PM4_GET(value, GE_USER_VGPR_EN, EN_USER_VGPR3) != 0;
		cp->GetUcfg()->SetGeUserVgprEn(r);
	};

	g_hw_uc_indirect_func[Pm4::VGT_PRIMITIVE_TYPE] = [](LUMEN_HW_UC_INDIRECT_ARGS)
	{
		uint32_t prim_type = LUMEN_PM4_GET(value, VGT_PRIMITIVE_TYPE, PRIM_TYPE);
		cp->GetUcfg()->SetPrimitiveType(prim_type);
	};
}

static void graphics_init_jmp_tables()
{
	for (auto& func: g_hw_ctx_func)
	{
		func = nullptr;
	}

	g_hw_ctx_func[Pm4::DB_RENDER_CONTROL]                 = hw_ctx_set_render_control;
	g_hw_ctx_func[Pm4::DB_STENCIL_CLEAR]                  = hw_ctx_set_stencil_clear;
	g_hw_ctx_func[Pm4::DB_DEPTH_CLEAR]                    = hw_ctx_set_depth_clear;
	g_hw_ctx_func[Pm4::PA_SC_SCREEN_SCISSOR_TL]           = hw_ctx_set_screen_scissor;
	g_hw_ctx_func[Pm4::DB_Z_INFO]                         = hw_ctx_set_depth_render_target;
	g_hw_ctx_func[Pm4::DB_STENCIL_INFO]                   = hw_ctx_set_stencil_info;
	g_hw_ctx_func[Pm4::PA_SU_HARDWARE_SCREEN_OFFSET]      = hw_ctx_set_hardware_screen_offset;
	g_hw_ctx_func[Pm4::CB_TARGET_MASK]                    = hw_ctx_set_render_target_mask;
	g_hw_ctx_func[Pm4::PA_SC_GENERIC_SCISSOR_TL]          = hw_ctx_set_generic_scissor;
	g_hw_ctx_func[Pm4::CB_BLEND_RED]                      = hw_ctx_set_blend_color;
	g_hw_ctx_func[Pm4::DB_STENCIL_CONTROL]                = hw_ctx_set_stencil_control;
	g_hw_ctx_func[Pm4::DB_STENCILREFMASK]                 = hw_ctx_set_stencil_mask;
	g_hw_ctx_func[Pm4::SPI_PS_INPUT_CNTL_0]               = hw_ctx_set_ps_input;
	g_hw_ctx_func[Pm4::DB_DEPTH_CONTROL]                  = hw_ctx_set_depth_control;
	g_hw_ctx_func[Pm4::DB_EQAA]                           = hw_ctx_set_eqaa_control;
	g_hw_ctx_func[Pm4::CB_COLOR_CONTROL]                  = hw_ctx_set_color_control;
	g_hw_ctx_func[Pm4::PA_CL_CLIP_CNTL]                   = hw_ctx_set_clip_control;
	g_hw_ctx_func[Pm4::PA_SU_SC_MODE_CNTL]                = hw_ctx_set_mode_control;
	g_hw_ctx_func[Pm4::PA_CL_VTE_CNTL]                    = hw_ctx_set_viewport_transform_control;
	g_hw_ctx_func[Pm4::PA_SU_LINE_CNTL]                   = hw_ctx_set_line_control;
	g_hw_ctx_func[Pm4::PA_SC_MODE_CNTL_0]                 = hw_ctx_set_scan_mode_control;
	g_hw_ctx_func[Pm4::PA_SC_AA_CONFIG]                   = hw_ctx_set_aa_config;
	g_hw_ctx_func[Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0] = hw_ctx_set_aa_sample_control;
	g_hw_ctx_func[Pm4::VGT_SHADER_STAGES_EN]              = hw_ctx_set_shader_stages;
	g_hw_ctx_func[Pm4::VGT_INDX_OFFSET]                   = hw_ctx_set_index_offset;
	g_hw_ctx_func[Pm4::VGT_GS_MODE]                       = hw_ctx_set_geometry_streamout_state;
	g_hw_ctx_func[Pm4::VGT_STRMOUT_CONFIG]                = hw_ctx_set_geometry_streamout_state;
	g_hw_ctx_func[Pm4::VGT_INSTANCE_STEP_RATE_0]          = hw_ctx_set_instance_step_rate;
	g_hw_ctx_func[Pm4::VGT_INSTANCE_STEP_RATE_1]          = hw_ctx_set_instance_step_rate;
	g_hw_ctx_func[Pm4::DB_ALPHA_TO_MASK]                 = hw_ctx_set_draw_control;
	g_hw_ctx_func[Pm4::IA_MULTI_VGT_PARAM]               = hw_ctx_set_draw_control;
	g_hw_ctx_func[Pm4::PA_SC_MODE_CNTL_1]                = hw_ctx_set_draw_control;
	g_hw_ctx_func[Pm4::PA_SC_AA_MASK_X0Y0_X1Y0]          = hw_ctx_set_aa_mask;
	g_hw_ctx_func[Pm4::PA_SC_AA_MASK_X0Y1_X1Y1]          = hw_ctx_set_aa_mask;
	g_hw_ctx_func[Pm4::PA_CL_GB_VERT_CLIP_ADJ]            = hw_ctx_set_guard_bands;
	g_hw_ctx_func[Pm4::PA_CL_GB_VERT_DISC_ADJ]            = hw_ctx_set_guard_bands;
	g_hw_ctx_func[Pm4::PA_CL_GB_HORZ_CLIP_ADJ]            = hw_ctx_set_guard_bands;
	g_hw_ctx_func[Pm4::PA_CL_GB_HORZ_DISC_ADJ]            = hw_ctx_set_guard_bands;
	for (uint32_t reg = Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL; reg <= Pm4::PA_SU_POLY_OFFSET_BACK_OFFSET; ++reg)
	{
		g_hw_ctx_func[reg] = hw_ctx_set_polygon_offset;
	}

	for (uint32_t slot = 0; slot < 8; slot++)
	{
		for (uint32_t reg = 0; reg < 14; ++reg)
		{
			g_hw_ctx_func[Pm4::CB_COLOR0_BASE + slot * 15 + reg] = hw_ctx_set_render_target;
		}

		g_hw_ctx_func[Pm4::CB_BLEND0_CONTROL + slot * 1] = hw_ctx_set_blend_control;
	}

	for (uint32_t reg = Pm4::PA_SC_VPORT_SCISSOR_0_TL; reg <= Pm4::PA_SC_VPORT_SCISSOR_15_BR; ++reg)
		g_hw_ctx_func[reg] = hw_ctx_set_viewport_scissor;

	for (uint32_t viewport = 0; viewport < 16; viewport++)
	{
		g_hw_ctx_func[Pm4::PA_SC_VPORT_ZMIN_0 + viewport * 2] = hw_ctx_set_viewport_z;
		g_hw_ctx_func[Pm4::PA_CL_VPORT_XSCALE + viewport * 6] = hw_ctx_set_viewport_scale_offset;
	}

	for (auto& func: g_hw_sh_func)
	{
		func = nullptr;
	}

	for (uint32_t slot = 0; slot < 16; slot++)
	{
		g_hw_sh_func[Pm4::SPI_SHADER_USER_DATA_VS_0 + slot * 1] = hw_sh_set_vs_user_sgpr;
		g_hw_sh_func[Pm4::SPI_SHADER_USER_DATA_PS_0 + slot * 1] = hw_sh_set_ps_user_sgpr;
		g_hw_sh_func[Pm4::COMPUTE_USER_DATA_0 + slot * 1]       = hw_sh_set_cs_user_sgpr;
		g_hw_sh_func[Pm4::SPI_SHADER_USER_DATA_GS_0 + slot * 1] = hw_sh_set_gs_user_sgpr;
	}

	for (auto& func: g_hw_uc_func)
	{
		func = nullptr;
	}

	g_hw_uc_func[Pm4::VGT_PRIMITIVE_TYPE] = hw_uc_set_primitive_type;

	for (auto& func: g_hw_sh_custom_func)
	{
		func = nullptr;
	}

	g_hw_sh_custom_func[Pm4::R_VS]          = hw_sh_set_vs_shader;
	g_hw_sh_custom_func[Pm4::R_PS]          = hw_sh_set_ps_shader;
	g_hw_sh_custom_func[Pm4::R_CS]          = hw_sh_set_cs_shader;
	g_hw_sh_custom_func[Pm4::R_VS_EMBEDDED] = hw_sh_set_vs_embedded;
	g_hw_sh_custom_func[Pm4::R_PS_EMBEDDED] = hw_sh_set_ps_embedded;
	g_hw_sh_custom_func[Pm4::R_VS_UPDATE]   = hw_sh_update_vs_shader;
	g_hw_sh_custom_func[Pm4::R_PS_UPDATE]   = hw_sh_update_ps_shader;

	for (auto& func: g_cp_op_func)
	{
		func = nullptr;
	}

	g_cp_op_func[Pm4::IT_NOP]                     = cp_op_nop;
	g_cp_op_func[Pm4::IT_CLEAR_STATE]             = cp_op_clear_state;
	g_cp_op_func[Pm4::IT_CONTEXT_CONTROL]         = cp_op_context_control;
	g_cp_op_func[Pm4::IT_DISPATCH_DIRECT]         = cp_op_dispatch_direct;
	g_cp_op_func[Pm4::IT_DRAW_INDEX_2]            = cp_op_draw_index;
	g_cp_op_func[Pm4::IT_INDEX_TYPE]              = cp_op_index_type;
	g_cp_op_func[Pm4::IT_INDEX_BUFFER_SIZE]       = cp_op_index_buffer_size;
	g_cp_op_func[Pm4::IT_NUM_INSTANCES]           = cp_op_num_instances;
	g_cp_op_func[Pm4::IT_DRAW_INDEX_AUTO]         = cp_op_draw_index_auto;
	g_cp_op_func[Pm4::IT_WAIT_REG_MEM]            = cp_op_wait_reg_mem;
	g_cp_op_func[Pm4::IT_WRITE_DATA]              = cp_op_write_data;
	g_cp_op_func[Pm4::IT_INDIRECT_BUFFER]         = cp_op_indirect_buffer;
	g_cp_op_func[Pm4::IT_EVENT_WRITE]             = cp_op_event_write;
	g_cp_op_func[Pm4::IT_EVENT_WRITE_EOP]         = cp_op_event_write_eop;
	g_cp_op_func[Pm4::IT_EVENT_WRITE_EOS]         = cp_op_event_write_eos;
	g_cp_op_func[Pm4::IT_RELEASE_MEM]             = cp_op_release_mem;
	g_cp_op_func[Pm4::IT_DMA_DATA]                = cp_op_dma_data;
	g_cp_op_func[Pm4::IT_ACQUIRE_MEM]             = cp_op_acquire_mem;
	g_cp_op_func[Pm4::IT_SET_CONTEXT_REG]         = cp_op_set_context_reg;
	g_cp_op_func[Pm4::IT_SET_SH_REG]              = cp_op_set_shader_reg;
	g_cp_op_func[Pm4::IT_SET_UCONFIG_REG]         = cp_op_set_uconfig_reg;
	g_cp_op_func[Pm4::IT_WRITE_CONST_RAM]         = cp_op_write_const_ram;
	g_cp_op_func[Pm4::IT_DUMP_CONST_RAM]          = cp_op_dump_const_ram;
	g_cp_op_func[Pm4::IT_INCREMENT_CE_COUNTER]    = cp_op_increment_ce_counter;
	g_cp_op_func[Pm4::IT_INCREMENT_DE_COUNTER]    = cp_op_increment_de_counter;
	g_cp_op_func[Pm4::IT_WAIT_ON_CE_COUNTER]      = cp_op_wait_on_ce_counter;
	g_cp_op_func[Pm4::IT_WAIT_ON_DE_COUNTER_DIFF] = cp_op_wait_on_de_counter_diff;

	for (auto& func: g_cp_op_custom_func)
	{
		func = nullptr;
	}

	g_cp_op_custom_func[Pm4::R_DRAW_INDEX]       = cp_op_draw_index;
	g_cp_op_custom_func[Pm4::R_DRAW_INDEX_AUTO]  = cp_op_draw_index_auto;
	g_cp_op_custom_func[Pm4::R_DISPATCH_DIRECT]  = cp_op_dispatch_direct;
	g_cp_op_custom_func[Pm4::R_DISPATCH_RESET]   = cp_op_dispatch_reset;
	g_cp_op_custom_func[Pm4::R_WAIT_MEM_32]      = cp_op_wait_reg_mem_32;
	g_cp_op_custom_func[Pm4::R_DRAW_RESET]       = cp_op_draw_reset;
	g_cp_op_custom_func[Pm4::R_WAIT_FLIP_DONE]   = cp_op_wait_flip_done;
	g_cp_op_custom_func[Pm4::R_PUSH_MARKER]      = cp_op_push_marker;
	g_cp_op_custom_func[Pm4::R_POP_MARKER]       = cp_op_pop_marker;
	g_cp_op_custom_func[Pm4::R_CX_REGS_INDIRECT] = cp_op_indirect_cx_regs;
	g_cp_op_custom_func[Pm4::R_SH_REGS_INDIRECT] = cp_op_indirect_sh_regs;
	g_cp_op_custom_func[Pm4::R_UC_REGS_INDIRECT] = cp_op_indirect_uc_regs;
	g_cp_op_custom_func[Pm4::R_ACQUIRE_MEM]      = cp_op_acquire_mem;
	g_cp_op_custom_func[Pm4::R_WRITE_DATA]       = cp_op_write_data;
	g_cp_op_custom_func[Pm4::R_WAIT_MEM_64]      = cp_op_wait_reg_mem_64;
	g_cp_op_custom_func[Pm4::R_FLIP]             = cp_op_flip;
	g_cp_op_custom_func[Pm4::R_RELEASE_MEM]      = cp_op_release_mem;

	graphics_init_jmp_tables_cx_indirect();
	graphics_init_jmp_tables_sh_indirect();
	graphics_init_jmp_tables_uc_indirect();
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
static bool graphics_run_pixel_done_write32_test(CommandProcessor* cp)
{
	constexpr uint32_t header = 0xc0034800u;
	const std::array<uint32_t, 4> body {0x630u, 0x1000u, 0x40000012u, 0xfedcba98u};
	auto rejects = [&](uint32_t candidate_header, const std::array<uint32_t, 4>& candidate,
	                   uint32_t available = 5, bool graphics = true)
	{
		Pm4PixelDoneWrite32 output {0xabcdefu, 0x12345678u};
		return !pm4_decode_pixel_done_write32(candidate_header, candidate.data(), available, graphics, &output) &&
		       output.address == 0xabcdefu && output.value == 0x12345678u;
	};
	for (uint32_t available = 0; available < 5; ++available)
		if (!rejects(header, body, available)) return false;
	if (!rejects(header, body, 5, false) || pm4_decode_pixel_done_write32(header, nullptr, 5, true, nullptr) ||
	    pm4_decode_pixel_done_write32(header, body.data(), 5, true, nullptr)) return false;
	Pm4PixelDoneWrite32 decoded;
	if (pm4_decode_pixel_done_write32(header, nullptr, 5, true, &decoded)) return false;
	for (uint32_t bit = 0; bit < 32; ++bit)
	{
		if (!rejects(header ^ (uint32_t {1} << bit), body)) return false;
		auto changed = body;
		changed[0] ^= uint32_t {1} << bit;
		if (!rejects(header, changed)) return false;
	}
	for (uint32_t bit = 16; bit < 32; ++bit)
	{
		auto changed = body;
		changed[2] ^= uint32_t {1} << bit;
		if (!rejects(header, changed)) return false;
	}
	for (uint32_t command = 0; command < 8; ++command)
	{
		auto changed = body;
		changed[2] = (body[2] & 0xffffu) | (command << 29u);
		if (command != 2 && !rejects(header, changed)) return false;
	}
	auto changed = body;
	changed[1] = 0; changed[2] = 0x40000000u;
	if (!rejects(header, changed)) return false;
	for (uint32_t offset = 1; offset <= 3; ++offset)
	{
		changed = body; changed[1] += offset;
		if (!rejects(header, changed)) return false;
	}
	for (uint32_t high: {0u, 1u, 0x8000u, 0xffffu})
		for (uint32_t value: {0u, 1u, 0x80000000u, 0xffffffffu})
		{
			changed = body; changed[2] = 0x40000000u | high; changed[3] = value;
			if (!pm4_decode_pixel_done_write32(header, changed.data(), 6, true, &decoded) ||
			    decoded.address != (static_cast<uint64_t>(high) << 32u | body[1]) || decoded.value != value) return false;
		}

	std::array<uint32_t, 3> sentinels {0x11223344u, 0x55667788u, 0x99aabbccu};
	const auto destination = reinterpret_cast<uint64_t>(&sentinels[1]);
	if ((destination >> 48u) != 0) return false;
	uint32_t stream[] = {header, 0x630u, static_cast<uint32_t>(destination),
	                     0x40000000u | static_cast<uint32_t>(destination >> 32u), body[3]};
	CommandEffectProbe probe;
	probe.owner = cp;
	{
		CommandEffectProbeScope scope(&probe);
		if (cp_op_event_write_eos(cp, stream[0], stream + 1, 5, 5) != 4) return false;
	}
	if (probe.overflow || probe.count != 1 || probe.effects[0] != CommandEffectProbeValue::QueuePixelDoneWrite32 ||
	    probe.destination32 != &sentinels[1] || probe.value32 != body[3] || probe.interrupt_event_id != UINT32_MAX ||
	    sentinels[1] != 0x55667788u) return false;
	probe = {}; probe.owner = cp;
	{
		CommandEffectProbeScope scope(&probe);
		cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
	}
	if (probe.overflow || probe.count != 1 || probe.effects[0] != CommandEffectProbeValue::QueuePixelDoneWrite32 ||
	    probe.destination32 != &sentinels[1] || probe.value32 != body[3] || probe.interrupt_event_id != UINT32_MAX ||
	    sentinels[1] != 0x55667788u) return false;
	const LabelGpuObject label(body[3], nullptr, nullptr);
	const bool completion = LabelCompletionContractTest(label, &sentinels[1], [](void* state, bool completed)
	{
		const auto& values = *static_cast<std::array<uint32_t, 3>*>(state);
		return values[0] == 0x11223344u && values[2] == 0x99aabbccu &&
		       values[1] == (completed ? 0xfedcba98u : 0x55667788u);
	}, &sentinels);
	if (completion) std::fprintf(stderr, "[GFX_PIXEL_DONE_WRITE32_TEST] raw-parser=pass reserved-rejection=pass deferred-write=pass\n");
	return completion;
}

static bool graphics_run_event_write_eop_immediate32_checkpoint_profile_test(CommandProcessor* cp)
{
	constexpr std::array<Pm4EventWriteEopPacket, 2> profiles = {{
	    {0xc0044700u, 0, 0, 0x28, 0x38, 5, 0, 1, 0x20, 0x00038528u, 0x20000000u},
	    {0xc0044700u, 0, 0, 4, 0x38, 5, 0, 1, 0x20, 0x00038504u, 0x20000000u},
	}};
	for (const char* value: {static_cast<const char*>(nullptr), "", "0", "01", "10", "true", "1 ", " 1", "1x"})
		if (eop_immediate32_checkpoint_selector_enabled(value)) return false;
	if (!eop_immediate32_checkpoint_selector_enabled("1") ||
	    experimental_eop_immediate32_checkpoint_enabled() !=
	        eop_immediate32_checkpoint_selector_enabled(std::getenv("LUMEN_EXPERIMENTAL_GFX_EOP_IMMEDIATE32_CHECKPOINT"))) return false;
	for (const auto& exact: profiles)
	{
		if (!pm4_event_write_eop_immediate32_checkpoint_profile(exact, 4, true) ||
		    pm4_event_write_eop_immediate32_checkpoint_profile(exact, 4, false)) return false;
		for (uint64_t address: std::array<uint64_t, 8>{0, 1, 2, 3, 6, 14, 0x1000000000000ull, UINT64_MAX})
			if (pm4_event_write_eop_immediate32_checkpoint_profile(exact, address, true)) return false;
		for (uint64_t address: std::array<uint64_t, 8>{4, 8, 12, 20, 28, 60, 68, 0xfffffffffffcull})
			if (!pm4_event_write_eop_immediate32_checkpoint_profile(exact, address, true)) return false;
		for (uint32_t bit = 0; bit != 32; ++bit)
		{
			auto changed = exact; changed.packet_id ^= 1u << bit;
			if (pm4_event_write_eop_immediate32_checkpoint_profile(changed, 4, true)) return false;
			changed = exact; changed.event_control_word ^= 1u << bit;
			if (pm4_event_write_eop_immediate32_checkpoint_profile(changed, 4, true)) return false;
			changed = exact; changed.address_control_word ^= 1u << bit;
			if (pm4_event_write_eop_immediate32_checkpoint_profile(changed, 4, true) != (bit < 16)) return false;
		}
		std::array<Pm4EventWriteEopPacket, 8> changed;
		changed.fill(exact);
		changed[0].cache_policy = 1; changed[1].event_write_dest = 1; changed[2].event_type = exact.event_type == 4 ? 0x28 : 4;
		changed[3].cache_action = 0; changed[4].event_index = 4; changed[5].int_selector = 1;
		changed[6].data_selector = 2; changed[7].control_selector_byte = 0x21;
		for (const auto& packet: changed)
			if (pm4_event_write_eop_immediate32_checkpoint_profile(packet, 4, true)) return false;
		alignas(64) std::array<uint32_t, 32> sentinel;
		sentinel.fill(0xa5a5a5a5u);
		const auto address = reinterpret_cast<uint64_t>(&sentinel[17]);
		if ((address >> 48) != 0 || address % 8 != 4) return false;
		uint32_t stream[] = {0xc0044700u, 0x00038528u, static_cast<uint32_t>(address),
		    0x20000000u | static_cast<uint32_t>(address >> 32), 0, 0,
		    0xc0044700u, 0x00000504u, static_cast<uint32_t>(address),
		    0x01000000u | static_cast<uint32_t>(address >> 32), 0, 0};
		stream[1] = exact.event_control_word;
		for (const uint64_t payload: std::array<uint64_t, 6>{0, 1, UINT32_MAX, 0x1122334455667788ull,
		                                                  0xffffffffffffffffull, 0xdeadbeef00000000ull})
		{
			stream[4] = static_cast<uint32_t>(payload); stream[5] = static_cast<uint32_t>(payload >> 32);
			CommandEffectProbe probe;
			probe.owner = cp; probe.immediate32_checkpoint_enabled = true;
			{
				CommandEffectProbeScope scope(&probe);
				cp->Run(stream, 6);
			}
			if (probe.overflow || probe.count != 1 || probe.effects[0] != CommandEffectProbeValue::EopImmediate32Checkpoint ||
			    probe.destination32 != &sentinel[17] || probe.value32 != static_cast<uint32_t>(payload) ||
			    probe.interrupt_event_id != UINT32_MAX || probe.destination64 != nullptr) return false;
		}
		CommandEffectProbe probe;
		probe.owner = cp; probe.immediate32_checkpoint_enabled = true;
		{
			CommandEffectProbeScope scope(&probe);
			cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
		}
		if (probe.overflow || probe.count != 2 || probe.effects[0] != CommandEffectProbeValue::EopImmediate32Checkpoint ||
		    probe.effects[1] != CommandEffectProbeValue::QueueEndOfPipeInterrupt) return false;
		for (const auto value: sentinel) if (value != 0xa5a5a5a5u) return false;
	}
	std::fprintf(stderr, "[GFX_EOP_IMMEDIATE32_CHECKPOINT_PARSER_TEST] exact-selector=pass event4/event28=pass raw-parser=pass reserved/control/stage=reject"
	                     " alignment4=pass low-payload32=retained high-payload32=ignored parse-store=none irq=none next-packet=pass\n");
	return true;
}

static bool graphics_run_event_write_eop_virtual_clock_profile_test(CommandProcessor* cp)
{
	constexpr Pm4EventWriteEopPacket exact = {0xc0044700u, 0, 0, 4, 0x38, 5, 0, 4, 0x80, 0x00038504u, 0x80000000u};
	for (const char* value: {static_cast<const char*>(nullptr), "", "0", "01", "10", "true", "1 ", " 1", "1x"})
		if (eop_virtual_clock_selector_enabled(value)) return false;
	if (!eop_virtual_clock_selector_enabled("1")) return false;
	if (!pm4_event_write_eop_virtual_clock_profile(exact, 8, true) ||
	    pm4_event_write_eop_virtual_clock_profile(exact, 8, false)) return false;
	for (uint64_t address: std::array<uint64_t, 9>{0, 1, 4, 12, 20, 60, 68, 76, UINT64_MAX})
		if (pm4_event_write_eop_virtual_clock_profile(exact, address, true)) return false;
	for (uint64_t address: std::array<uint64_t, 9>{8, 16, 24, 32, 40, 48, 56, 64, 72})
		if (!pm4_event_write_eop_virtual_clock_profile(exact, address, true)) return false;
	for (uint32_t bit = 0; bit != 32; ++bit)
	{
		auto changed = exact;
		changed.packet_id ^= 1u << bit;
		if (pm4_event_write_eop_virtual_clock_profile(changed, 8, true)) return false;
		changed = exact; changed.event_control_word ^= 1u << bit;
		if (pm4_event_write_eop_virtual_clock_profile(changed, 8, true)) return false;
		changed = exact; changed.address_control_word ^= 1u << bit;
		if (pm4_event_write_eop_virtual_clock_profile(changed, 8, true) != (bit < 16)) return false;
	}
	std::array<Pm4EventWriteEopPacket, 8> changed;
	changed.fill(exact);
	changed[0].cache_policy = 1; changed[1].event_write_dest = 1; changed[2].event_type = 5;
	changed[3].cache_action = 0; changed[4].event_index = 4; changed[5].int_selector = 1;
	changed[6].data_selector = 3; changed[7].control_selector_byte = 0x81;
	for (const auto& packet: changed)
		if (pm4_event_write_eop_virtual_clock_profile(packet, 8, true)) return false;
	alignas(64) std::array<uint64_t, 16> sentinel;
	sentinel.fill(0x0123456789abcdefull);
	const auto address = reinterpret_cast<uint64_t>(&sentinel[9]);
	if ((address >> 48) != 0 || address % 64 != 8) return false;
	uint32_t stream[] = {0xc0044700u, 0x00038504u, static_cast<uint32_t>(address),
	    0x80000000u | static_cast<uint32_t>(address >> 32), 0, 0,
	    0xc0044700u, 0x00000504u, static_cast<uint32_t>(address),
	    0x01000000u | static_cast<uint32_t>(address >> 32), 0, 0};
	for (const auto payload: std::array<uint64_t, 4>{0, 1, 0x1122334455667788ull, UINT64_MAX})
	{
		stream[4] = static_cast<uint32_t>(payload);
		stream[5] = static_cast<uint32_t>(payload >> 32);
		CommandEffectProbe probe;
		probe.owner = cp;
		probe.virtual_clock_enabled = true;
		{
			CommandEffectProbeScope scope(&probe);
			cp->Run(stream, 6);
		}
		if (probe.overflow || probe.count != 1 || probe.effects[0] != CommandEffectProbeValue::EopVirtualClock ||
		    probe.destination64 != &sentinel[9] || probe.interrupt_event_id != UINT32_MAX) return false;
	}
	CommandEffectProbe probe;
	probe.owner = cp; probe.virtual_clock_enabled = true;
	{
		CommandEffectProbeScope scope(&probe);
		cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
	}
	if (probe.overflow || probe.count != 2 || probe.effects[0] != CommandEffectProbeValue::EopVirtualClock ||
	    probe.effects[1] != CommandEffectProbeValue::QueueEndOfPipeInterrupt) return false;
	for (auto value: sentinel) if (value != 0x0123456789abcdefull) return false;
	std::fprintf(stderr, "[GFX_EOP_VIRTUAL_CLOCK_PARSER_TEST] exact-selector=pass raw-parser=pass reserved-rejection=pass"
	                     " alignment8=pass payload=ignored parse-store=none clock-irq=none next-packet=pass\n");
	return true;
}

static bool graphics_run_event_write_eop_completion_profile_test(CommandProcessor* cp)
{
	using Profile = Pm4EventWriteEopCompletionProfile;
	if (cp == nullptr)
	{
		return false;
	}
	constexpr Pm4EventWriteEopPacket exact = {
	    0xc0044700u, 0, 0, 4, 0, 5, 1, 0, 0x01, 0x00000504u, 0x01000000u,
	};
	if (pm4_event_write_eop_completion_profile(exact) != Profile::InterruptOnly)
	{
		std::fprintf(stderr, "exact EVENT_WRITE_EOP interrupt-only profile was not admitted\n");
		return false;
	}

	uint32_t admitted_control_bytes = 0;
	for (uint32_t control_byte = 0; control_byte <= 0xffu; control_byte++)
	{
		auto candidate                    = exact;
		candidate.control_selector_byte   = control_byte;
		candidate.int_selector            = control_byte & 0x3u;
		candidate.data_selector           = (control_byte >> 5u) & 0x7u;
		candidate.address_control_word     = control_byte << 24u;
		if (pm4_event_write_eop_completion_profile(candidate) != Profile::Unsupported)
		{
			admitted_control_bytes++;
			if (control_byte != 0x01u)
			{
				std::fprintf(stderr, "nonexact EVENT_WRITE_EOP control byte was admitted\n");
				return false;
			}
		}
	}
	if (admitted_control_bytes != 1)
	{
		std::fprintf(stderr, "EVENT_WRITE_EOP control-byte gate was not exact\n");
		return false;
	}
	for (const uint32_t legacy_control_byte: {0x00u, 0x02u, 0x03u})
	{
		auto legacy_profile                  = exact;
		legacy_profile.control_selector_byte = legacy_control_byte;
		legacy_profile.int_selector          = legacy_control_byte & 0x3u;
		legacy_profile.data_selector         = (legacy_control_byte >> 5u) & 0x7u;
		legacy_profile.address_control_word  = legacy_control_byte << 24u;
		if (pm4_event_write_eop_completion_profile(legacy_profile) != Profile::Unsupported)
		{
			std::fprintf(stderr, "legacy EOP selector was absorbed by the new completion profile\n");
			return false;
		}
	}

	std::array<Pm4EventWriteEopPacket, 13> unsupported = {};
	unsupported.fill(exact);
	unsupported[0].packet_id          = 0xc0054902u;
	unsupported[1].packet_id          = 0xc0051060u;
	unsupported[2].cache_policy       = 1;
	unsupported[3].event_write_dest   = 1;
	unsupported[4].event_type         = 5;
	unsupported[5].cache_action       = 1;
	unsupported[6].event_index        = 4;
	unsupported[7].int_selector       = 0;
	unsupported[8].data_selector      = 1;
	unsupported[9].packet_id          = 0xc0044701u;
	unsupported[10].event_control_word |= 1u << 6u;
	unsupported[11].address_control_word |= 1u << 17u;
	unsupported[12].address_control_word = 0x02000000u;
	for (const auto& packet: unsupported)
	{
		if (pm4_event_write_eop_completion_profile(packet) != Profile::Unsupported)
		{
			std::fprintf(stderr, "nonexact EVENT_WRITE_EOP packet was admitted\n");
			return false;
		}
	}

	uint64_t sentinel = 0x0123456789abcdefull;
	const auto destination = reinterpret_cast<uint64_t>(&sentinel);
	if ((destination >> 48u) != 0)
	{
		std::fprintf(stderr, "EVENT_WRITE_EOP test destination does not fit the packet address field\n");
		return false;
	}
	uint32_t stream[] = {
	    0xc0044700u,
	    0x00000504u,
	    static_cast<uint32_t>(destination),
	    static_cast<uint32_t>((destination >> 32u) & 0xffffu) | 0x01000000u,
	    0x76543210u,
	    0xfedcba98u,
	};
	CommandEffectProbe probe;
	probe.owner = cp;
	{
		CommandEffectProbeScope scope(&probe);
		cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
	}
	if (probe.overflow || probe.count != 1 || probe.effects[0] != CommandEffectProbeValue::QueueEndOfPipeInterrupt ||
	    sentinel != 0x0123456789abcdefull)
	{
		std::fprintf(stderr, "EVENT_WRITE_EOP raw packet did not route to one interrupt-only effect\n");
		return false;
	}
	return graphics_run_event_write_eop_virtual_clock_profile_test(cp) &&
	       graphics_run_event_write_eop_immediate32_checkpoint_profile_test(cp);
}

bool Gpu::InterruptRoutingForTest()
{
	using Profile = Pm4EventWriteEopCompletionProfile;
	constexpr Pm4EventWriteEopPacket profiles[] = {
	    {0xc0054902u, 0, 0, 0x14, 0, 5, 2, 1, 0x22, 0x00000514u, 0x22000000u},
	    {0xc0054902u, 0, 0, 0x14, 0, 5, 2, 3, 0x62, 0x00000514u, 0x62000000u},
	};
	for (const auto& exact: profiles)
	{
		const auto expected = exact.data_selector == 1 ? Profile::ReleaseMemWriteBack32Interrupt :
		    Profile::ReleaseMemWriteBackClock64Interrupt;
		if (pm4_event_write_eop_completion_profile(exact) != expected) return false;
		std::array<Pm4EventWriteEopPacket, 11> rejected;
		rejected.fill(exact);
		rejected[0].packet_id ^= 1;
		rejected[1].cache_policy = 1;
		rejected[2].event_write_dest = 1;
		rejected[3].event_type = 4;
		rejected[4].cache_action = 1;
		rejected[5].event_index = 0;
		rejected[6].int_selector = 1;
		rejected[7].data_selector = 2;
		rejected[8].control_selector_byte ^= 1;
		rejected[9].event_control_word |= 1u << 6u;
		rejected[10].address_control_word |= 1u << 17u;
		for (const auto& packet: rejected)
			if (pm4_event_write_eop_completion_profile(packet) != Profile::Unsupported) return false;
	}

	auto* queue9 = GetRing(9)->GetCpForTest();
	auto* queue10 = GetRing(10)->GetCpForTest();
	auto* queue17 = GetRing(17)->GetCpForTest();
	if (queue9 != queue10 || queue9 == queue17 || m_gfx_cp == queue9 || m_gfx_cp->GetInterruptEventId() != 64 ||
	    queue9->GetInterruptEventId() != 1 || queue17->GetInterruptEventId() != 2) return false;
	for (auto* cp: {m_gfx_cp, queue9, queue10, queue17})
	{
		cp->SetHeadlessTestMode();
		const uint32_t identity = cp->GetInterruptEventId();
		cp->Reset();
		if (cp->GetInterruptEventId() != identity) return false;
		for (const uint32_t token: {0x11223344u, 0xaabbccddu})
		{
			uint32_t destination[3] = {0x12345678, 0x87654321, 0xfedcba98};
			const auto address = reinterpret_cast<uint64_t>(&destination[1]);
			uint32_t stream[] = {0xc0054902u, 0x00000514u, 0x22000000u,
			    static_cast<uint32_t>(address), static_cast<uint32_t>(address >> 32u), token, 0};
			CommandEffectProbe probe;
			probe.owner = cp;
			{
				CommandEffectProbeScope scope(&probe);
				cp->Run(stream, static_cast<uint32_t>(std::size(stream)), nullptr,
				    identity == 64 ? CommandStreamKind::Draw : CommandStreamKind::Compute);
			}
			if (probe.overflow || probe.count != 2 || probe.effects[0] != CommandEffectProbeValue::MemoryBarrier ||
			    probe.effects[1] != CommandEffectProbeValue::WriteBack32Interrupt || probe.interrupt_event_id != identity ||
			    probe.destination32 != &destination[1] || probe.value32 != token || destination[0] != 0x12345678 ||
			    destination[1] != 0x87654321 || destination[2] != 0xfedcba98) return false;

			uint64_t clock_destination[3] = {UINT64_C(0x12345678abcdef01), UINT64_C(0xfedcba9876543210), UINT64_C(0x87654321abcdef01)};
			const auto clock_address = reinterpret_cast<uint64_t>(&clock_destination[1]);
			uint32_t clock_stream[] = {0xc0054902u, 0x00000514u, 0x62000000u,
			    static_cast<uint32_t>(clock_address), static_cast<uint32_t>(clock_address >> 32u), 0, 0};
			CommandEffectProbe clock_probe;
			clock_probe.owner = cp;
			{
				CommandEffectProbeScope scope(&clock_probe);
				cp->Run(clock_stream, static_cast<uint32_t>(std::size(clock_stream)), nullptr,
				    identity == 64 ? CommandStreamKind::Draw : CommandStreamKind::Compute);
			}
			if (clock_probe.overflow || clock_probe.count != 2 || clock_probe.effects[0] != CommandEffectProbeValue::MemoryBarrier ||
			    clock_probe.effects[1] != CommandEffectProbeValue::WriteBackClock64Interrupt || clock_probe.interrupt_event_id != identity ||
			    clock_probe.destination64 != &clock_destination[1] || clock_probe.destination32 != nullptr ||
			    clock_destination[0] != UINT64_C(0x12345678abcdef01) || clock_destination[1] != UINT64_C(0xfedcba9876543210) ||
			    clock_destination[2] != UINT64_C(0x87654321abcdef01)) return false;
		}
	}
	return true;
}

static bool graphics_run_acquire_mem_global_writeback_profile_test(CommandProcessor* cp)
{
	using Profile = Pm4AcquireMemGlobalWriteBackProfile;
	if (cp == nullptr)
	{
		return false;
	}
	if (!exact_one_environment_value("1") || exact_one_environment_value(nullptr) || exact_one_environment_value("") ||
	    exact_one_environment_value("0") || exact_one_environment_value("01") || exact_one_environment_value("true") ||
	    exact_one_environment_value("2") || exact_one_environment_value("1x"))
	{
		std::fprintf(stderr, "ACQUIRE_MEM experimental selector parser was not exact\n");
		return false;
	}
	const bool observed_profile_enabled = experimental_acquire_mem_08c40000_enabled();
	if (observed_profile_enabled !=
	    exact_one_environment_value(std::getenv("LUMEN_EXPERIMENTAL_GFX_ACQUIRE_MEM_08C40000")))
	{
		std::fprintf(stderr, "ACQUIRE_MEM experimental selector did not reflect the exact process value\n");
		return false;
	}
	if (pm4_acquire_mem_global_writeback_profile(0x00c40000u) != Profile::Base ||
	    pm4_acquire_mem_global_writeback_profile(0x08c40000u) != Profile::Observed08c40000)
	{
		std::fprintf(stderr, "ACQUIRE_MEM global write-back profiles were not classified exactly\n");
		return false;
	}

	constexpr Pm4AcquireMemGlobalWriteBackPacket base = {
	    0xc0055800u, 0, 0x00c40000u, 1, 0, 0, 0, 10, 0,
	};
	auto extended            = base;
	extended.engine_selector = 1;
	extended.cache_action    = 0x08c40000u;
	if (!pm4_acquire_mem_global_writeback_supported(base, false) ||
	    !pm4_acquire_mem_global_writeback_supported(base, true) ||
	    pm4_acquire_mem_global_writeback_supported(extended, false) ||
	    pm4_acquire_mem_global_writeback_plan(extended, false).profile != Profile::Unsupported ||
	    !pm4_acquire_mem_global_writeback_supported(extended, true))
	{
		std::fprintf(stderr, "ACQUIRE_MEM profiles did not preserve the default fail-closed gate\n");
		return false;
	}
	const auto base_plan     = pm4_acquire_mem_global_writeback_plan(base, false);
	const auto extended_plan = pm4_acquire_mem_global_writeback_plan(extended, true);
	if (base_plan.profile != Profile::Base || extended_plan.profile != Profile::Observed08c40000 ||
	    base_plan.steps[0] != Pm4AcquireMemGlobalWriteBackStep::MemoryBarrier ||
	    base_plan.steps[1] != Pm4AcquireMemGlobalWriteBackStep::WriteBack || base_plan.steps != extended_plan.steps)
	{
		std::fprintf(stderr, "ACQUIRE_MEM global write-back execution plan changed\n");
		return false;
	}

	auto custom_base     = base;
	custom_base.cmd_id   = 0xc0061050u;
	custom_base.gcr_cntl = 0xffffffffu;
	if (!pm4_acquire_mem_global_writeback_supported(custom_base, false))
	{
		std::fprintf(stderr, "existing custom ACQUIRE_MEM base profile changed\n");
		return false;
	}
	const auto custom_plan = pm4_acquire_mem_global_writeback_plan(custom_base, false);
	if (custom_plan.profile != Profile::Base || custom_plan.steps != base_plan.steps)
	{
		std::fprintf(stderr, "existing custom ACQUIRE_MEM base execution plan changed\n");
		return false;
	}

	std::array<Pm4AcquireMemGlobalWriteBackPacket, 14> unsupported = {};
	unsupported.fill(base);
	unsupported[0].cmd_id       = 0;
	unsupported[1].cache_action = 0x08000000u;
	unsupported[2].cache_action = 0x08c00000u;
	unsupported[3].cache_action = 0x08c40040u;
	unsupported[4].cache_action = 0x09c40000u;
	unsupported[5].cache_action = 0x0ac40000u;
	unsupported[6].cache_action = 0x28c40000u;
	unsupported[7].size_lo      = 0;
	unsupported[8].size_hi      = 1;
	unsupported[9].base_lo      = 1;
	unsupported[10].base_hi     = 1;
	unsupported[11].poll        = 9;
	unsupported[12]             = extended;
	unsupported[12].cmd_id      = 0xc0061050u;
	unsupported[13] = extended;
	unsupported[13].engine_selector = 0;
	for (const auto& packet: unsupported)
	{
		if (pm4_acquire_mem_global_writeback_supported(packet, true) ||
		    pm4_acquire_mem_global_writeback_plan(packet, true).profile != Profile::Unsupported)
		{
			std::fprintf(stderr, "unsupported ACQUIRE_MEM global write-back packet was admitted\n");
			return false;
		}
	}

	uint32_t base_stream[] = {
	    0xc0055800u, 0x00c40000u, 1, 0, 0, 0, 10,
	    0xc0061050u, 0x00c40000u, 1, 0, 0, 0, 10, 0xffffffffu,
	};
	uint32_t observed_stream[] = {
	    0xc0055800u, 0x88c40000u, 1, 0, 0, 0, 10,
	};
	CommandEffectProbe probe;
	probe.owner = cp;
	{
		CommandEffectProbeScope scope(&probe);
		cp->Run(base_stream, static_cast<uint32_t>(std::size(base_stream)));
		if (observed_profile_enabled)
		{
			cp->Run(observed_stream, static_cast<uint32_t>(std::size(observed_stream)));
		}
	}
	const uint32_t expected_count = observed_profile_enabled ? 6 : 4;
	if (probe.overflow || probe.count != expected_count)
	{
		std::fprintf(stderr, "ACQUIRE_MEM raw packet stream produced the wrong number of effects\n");
		return false;
	}
	for (uint32_t index = 0; index < probe.count; index++)
	{
		const auto expected = (index % 2u) == 0 ? CommandEffectProbeValue::MemoryBarrier : CommandEffectProbeValue::WriteBack;
		if (probe.effects[index] != expected)
		{
			std::fprintf(stderr, "ACQUIRE_MEM raw packet stream changed barrier/write-back order\n");
			return false;
		}
	}
	return true;
}

static bool graphics_run_acquire_mem_db_only_test(CommandProcessor* cp)
{
	if (cp == nullptr || cp->GetInterruptEventId() != 64) return false;
	if (!exact_one_environment_value("1") || exact_one_environment_value(nullptr) || exact_one_environment_value("") ||
	    exact_one_environment_value("0") || exact_one_environment_value("01") || exact_one_environment_value("true") ||
	    exact_one_environment_value("1 ") || exact_one_environment_value("1x")) return false;
	const bool enabled = experimental_acquire_mem_db_only_enabled();
	if (enabled != exact_one_environment_value(std::getenv("LUMEN_EXPERIMENTAL_GFX_ACQUIRE_MEM_DB_ONLY"))) return false;
	constexpr Pm4AcquireMemGlobalWriteBackPacket supported = {0xc0055800u, 1, 0x04004000u, 0x123u, 0, 0x456u, 0, 10, 0};
	if (!pm4_acquire_mem_db_only_supported(supported, true, 64) || pm4_acquire_mem_db_only_supported(supported, false, 64))
		return false;
	for (const uint32_t queue: {0u, 1u, 2u, 63u, 65u, UINT32_MAX})
	{
		if (pm4_acquire_mem_db_only_supported(supported, true, queue)) return false;
	}
	struct RangeCase { uint64_t base; uint64_t size; bool admitted; };
	constexpr RangeCase ranges[] = {
	    {1, 1, true}, {0x234567u, 0x765432u, true}, {UINT32_MAX, 1, true}, {1, UINT32_MAX, true},
	    {0x80000000u, 0x80000000u, true}, {UINT32_MAX, 2, false}, {0x80000000u, 0x80000001u, false},
	    {0, 1, false}, {1, 0, false}, {uint64_t {1} << 32u, 1, false}, {1, uint64_t {1} << 32u, false},
	    {UINT64_MAX, 1, false}, {1, UINT64_MAX, false},
	};
	CommandEffectProbe rejected_probe;
	rejected_probe.owner = cp;
	{
		CommandEffectProbeScope scope(&rejected_probe);
		if (execute_acquire_mem_db_only(cp, supported, false) || execute_acquire_mem_db_only(nullptr, supported, true)) return false;
		for (const auto& range: ranges)
		{
			auto packet = supported;
			packet.base_lo = range.base;
			packet.size_lo = range.size;
			if (pm4_acquire_mem_db_only_supported(packet, true, 64) != range.admitted ||
			    (!range.admitted && execute_acquire_mem_db_only(cp, packet, true))) return false;
		}
		std::array<Pm4AcquireMemGlobalWriteBackPacket, 12> invalid;
		invalid.fill(supported);
		invalid[0].cmd_id = 0xc0061050u;
		invalid[1].cmd_id = 0xc0055801u;
		invalid[2].engine_selector = 0;
		invalid[3].engine_selector = 2;
		invalid[4].cache_action = 0x04c44000u;
		invalid[5].cache_action = 0x08c40000u;
		invalid[6].size_hi = 1;
		invalid[7].base_hi = 1;
		invalid[8].poll = 9;
		invalid[9].poll = 11;
		invalid[10].gcr_cntl = 1;
		invalid[11].gcr_cntl = 0x280u;
		for (const auto& packet: invalid)
		{
			if (pm4_acquire_mem_db_only_supported(packet, true, 64) || execute_acquire_mem_db_only(cp, packet, true)) return false;
		}
		for (uint32_t bit = 0; bit < 32; ++bit)
		{
			auto packet = supported;
			packet.cache_action ^= uint32_t {1} << bit;
			if (pm4_acquire_mem_db_only_supported(packet, true, 64) || execute_acquire_mem_db_only(cp, packet, true)) return false;
		}
	}
	if (rejected_probe.overflow || rejected_probe.count != 0) return false;

	// Decode only complete declared bodies and leave output unchanged on errors.
	uint32_t body[] = {0x84004000u, 0x123u, 0, 0x456u, 0, 10, 0xdeadbeefu};
	auto decoded = supported;
	auto unchanged = [&]()
	{
		return decoded.cmd_id == supported.cmd_id && decoded.engine_selector == supported.engine_selector &&
		       decoded.cache_action == supported.cache_action && decoded.size_lo == supported.size_lo &&
		       decoded.size_hi == supported.size_hi && decoded.base_lo == supported.base_lo &&
		       decoded.base_hi == supported.base_hi && decoded.poll == supported.poll && decoded.gcr_cntl == supported.gcr_cntl;
	};
	for (const uint32_t header: {0u, 0xc0045800u, 0xc0065800u, 0xc0055801u, 0xc0055802u, 0xc0051000u})
	{
		if (pm4_decode_acquire_mem(header, body, 8, &decoded) || !unchanged()) return false;
	}
	for (const uint32_t header: {0xc0055800u, 0xc0061050u})
	{
		for (uint32_t available = 0; available < LUMEN_PM4_LEN(header); ++available)
		{
			if (pm4_decode_acquire_mem(header, body, available, &decoded) || !unchanged()) return false;
		}
	}
	if (pm4_decode_acquire_mem(0xc0055800u, nullptr, 7, &decoded) || !unchanged() ||
	    pm4_decode_acquire_mem(0xc0055800u, body, 7, nullptr) ||
	    !pm4_decode_acquire_mem(0xc0055800u, body, 7, &decoded) || !unchanged()) return false;
	if (!pm4_decode_acquire_mem(0xc0061050u, body, 8, &decoded) || decoded.gcr_cntl != 0xdeadbeefu ||
	    pm4_acquire_mem_db_only_supported(decoded, true, 64)) return false;

	auto* compute = new CommandProcessor(1);
	compute->SetHeadlessTestMode();
	CommandEffectProbe compute_probe;
	compute_probe.owner = compute;
	{
		CommandEffectProbeScope scope(&compute_probe);
		if (execute_acquire_mem_db_only(compute, supported, true)) return false;
	}
	if (compute_probe.overflow || compute_probe.count != 0) return false;

	// The actual parser emits exactly one full GPU barrier, with no WriteBack,
	// only in the separate process where the exact-one selector is enabled.
	CommandEffectProbe accepted_probe;
	accepted_probe.owner = cp;
	{
		CommandEffectProbeScope scope(&accepted_probe);
		if (enabled)
		{
			uint32_t stream[] = {0xc0055800u, 0x84004000u, 0x123u, 0, 0x456u, 0, 10};
			cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
		}
	}
	return !accepted_probe.overflow && accepted_probe.count == (enabled ? 1u : 0u) &&
	       (!enabled || accepted_probe.effects[0] == CommandEffectProbeValue::FullMemoryBarrier);
}

static bool graphics_run_shader_partial_flush_test(CommandProcessor* cp)
{
	if (cp == nullptr || cp->GetInterruptEventId() != 64) return false;
	constexpr uint32_t header = 0xc0004600u;
	constexpr uint32_t bodies[] = {0x410u, 0x407u};
	CommandEffectProbe rejected_probe;
	rejected_probe.owner = cp;
	{
		CommandEffectProbeScope scope(&rejected_probe);
		for (const uint32_t body: bodies)
		{
			uint32_t decoded = 0xdeadbeefu;
			if (!pm4_decode_event_write(header, &body, 2, &decoded) || decoded != body ||
			    !pm4_shader_partial_flush_supported(header, decoded, 2, 64) ||
			    execute_shader_partial_flush_event(nullptr, header, body, 2)) return false;
			for (const uint32_t available: {0u, 1u})
			{
				decoded = 0xdeadbeefu;
				if (pm4_decode_event_write(header, &body, available, &decoded) || decoded != 0xdeadbeefu ||
				    pm4_shader_partial_flush_supported(header, body, available, 64) ||
				    execute_shader_partial_flush_event(cp, header, body, available)) return false;
			}
			decoded = 0xdeadbeefu;
			if (pm4_decode_event_write(header, nullptr, 2, &decoded) || decoded != 0xdeadbeefu ||
			    pm4_decode_event_write(header, &body, 2, nullptr)) return false;
			for (uint32_t bit = 0; bit < 32; ++bit)
			{
				const uint32_t wrong_header = header ^ (uint32_t {1} << bit);
				const uint32_t wrong_control = body ^ (uint32_t {1} << bit);
				decoded = 0xdeadbeefu;
				if (pm4_decode_event_write(wrong_header, &body, 2, &decoded) || decoded != 0xdeadbeefu ||
				    pm4_shader_partial_flush_supported(wrong_header, body, 2, 64) ||
				    execute_shader_partial_flush_event(cp, wrong_header, body, 2) ||
				    pm4_shader_partial_flush_supported(header, wrong_control, 2, 64) ||
				    execute_shader_partial_flush_event(cp, header, wrong_control, 2)) return false;
			}
			for (uint32_t index = 0; index < 16; ++index)
			{
				if (index != 4 && execute_shader_partial_flush_event(cp, header, (index << 8u) | (body & 0x3fu), 2)) return false;
			}
			for (const uint32_t wrong_header: {0xc0004700u, 0xc0004800u, 0xc0004900u, 0xc0014600u})
			{
				if (pm4_shader_partial_flush_supported(wrong_header, body, 2, 64) ||
				    execute_shader_partial_flush_event(cp, wrong_header, body, 2)) return false;
			}
			for (const uint32_t queue: {0u, 1u, 2u, 63u, 65u, UINT32_MAX})
			{
				if (pm4_shader_partial_flush_supported(header, body, 2, queue)) return false;
			}
		}
		if (pm4_shader_partial_flush_supported(header, 0x40fu, 2, 64) ||
		    execute_shader_partial_flush_event(cp, header, 0x40fu, 2)) return false;
	}
	if (rejected_probe.overflow || rejected_probe.count != 0) return false;

	auto* compute = new CommandProcessor(65);
	compute->SetHeadlessTestMode();
	CommandEffectProbe compute_probe;
	compute_probe.owner = compute;
	{
		CommandEffectProbeScope scope(&compute_probe);
		for (const uint32_t body: bodies)
		{
			if (execute_shader_partial_flush_event(compute, header, body, 2)) return false;
		}
	}
	if (compute_probe.overflow || compute_probe.count != 0) return false;

	// Real parser dispatch must consume only the control DWORD, record one
	// barrier and preserve the packet bytes and a following packet boundary.
	// Every writeback/interrupt effect would be a distinct probe entry.
	for (const uint32_t body: bodies)
	{
		uint32_t stream[] = {header, body, LUMEN_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO), 0};
		std::array<uint32_t, std::size(stream)> unchanged {};
		std::copy(std::begin(stream), std::end(stream), unchanged.begin());
		uint64_t guest_sentinel = 0x0123456789abcdefull;
		CommandEffectProbe parser_probe;
		parser_probe.owner = cp;
		parser_probe.destination64 = &guest_sentinel;
		{
			CommandEffectProbeScope scope(&parser_probe);
			if (cp_op_event_write(cp, header, stream + 1, 2, 2) != 1) return false;
		}
		if (parser_probe.overflow || parser_probe.count != 1 ||
		    parser_probe.effects[0] != CommandEffectProbeValue::FullMemoryBarrier) return false;
		CommandEffectProbe accepted_probe;
		accepted_probe.owner = cp;
		accepted_probe.destination64 = &guest_sentinel;
		{
			CommandEffectProbeScope scope(&accepted_probe);
			cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
		}
		if (accepted_probe.overflow || accepted_probe.count != 1 ||
		    accepted_probe.effects[0] != CommandEffectProbeValue::FullMemoryBarrier ||
		    !std::equal(std::begin(stream), std::end(stream), unchanged.begin()) || guest_sentinel != 0x0123456789abcdefull) return false;
	}
	return true;
}

static bool graphics_run_db_cache_event_test(CommandProcessor* cp)
{
	constexpr uint32_t header = 0xc0004600u, control = 0x72au;
	if (cp == nullptr || cp->GetInterruptEventId() != 64) return false;
	CommandEffectProbe rejected;
	rejected.owner = cp;
	rejected.db_cache_proof_enabled = true;
	{
		CommandEffectProbeScope scope(&rejected);
		if (!pm4_db_cache_event_supported(header, control, 2, 64) ||
		    execute_db_cache_event(nullptr, header, control, 2, true).status != GpuMemoryDbCacheProofStatus::Rejected) return false;
		uint32_t decoded = 0;
		if (!pm4_decode_event_write(header, &control, 2, &decoded) || decoded != control ||
		    pm4_decode_event_write(header, nullptr, 2, &decoded) || pm4_decode_event_write(header, &control, 2, nullptr)) return false;
		for (uint32_t bit = 0; bit < 32; ++bit)
		{
			const uint32_t mutated_header = header ^ (uint32_t {1} << bit);
			const uint32_t mutated_control = control ^ (uint32_t {1} << bit);
			if (execute_db_cache_event(cp, mutated_header, control, 2, true).status != GpuMemoryDbCacheProofStatus::Rejected ||
			    execute_db_cache_event(cp, header, mutated_control, 2, true).status != GpuMemoryDbCacheProofStatus::Rejected) return false;
		}
		for (const uint32_t available: {0u, 1u})
			if (pm4_decode_event_write(header, &control, available, &decoded) ||
			    execute_db_cache_event(cp, header, control, available, true).status != GpuMemoryDbCacheProofStatus::Rejected) return false;
		for (uint32_t index = 0; index < 16; ++index)
			if (index != 7 && execute_db_cache_event(cp, header, (index << 8u) | 0x2au, 2, true).status != GpuMemoryDbCacheProofStatus::Rejected) return false;
		for (const uint32_t wrong_control: {0x72bu, 0x72cu, 0x72eu, 0x407u, 0x410u})
			if (execute_db_cache_event(cp, header, wrong_control, 2, true).status != GpuMemoryDbCacheProofStatus::Rejected) return false;
		for (const uint32_t cp_id: {0u, 63u, 65u, UINT32_MAX})
			if (pm4_db_cache_event_supported(header, control, 2, cp_id)) return false;
	}
	if (rejected.count != 0 || rejected.overflow || rejected.db_cache_proofs != 0) return false;
	auto* compute = new CommandProcessor(65);
	compute->SetHeadlessTestMode();
	CommandEffectProbe compute_probe;
	compute_probe.owner = compute;
	compute_probe.db_cache_proof_enabled = true;
	{
		CommandEffectProbeScope scope(&compute_probe);
		if (execute_db_cache_event(compute, header, control, 2, true).status != GpuMemoryDbCacheProofStatus::Rejected) return false;
	}
	if (compute_probe.count != 0 || compute_probe.db_cache_proofs != 0) return false;
	for (const bool admission: {false, true})
	{
		CommandEffectProbe probe;
		probe.owner = cp;
		probe.db_cache_proof_enabled = true;
		{
			CommandEffectProbeScope scope(&probe);
			const auto result = execute_db_cache_event(cp, header, control, 2, admission);
			if (result.status != (admission ? GpuMemoryDbCacheProofStatus::Recorded : GpuMemoryDbCacheProofStatus::Proven)) return false;
		}
		if (probe.db_cache_proofs != 1 || probe.count != (admission ? 1u : 0u) || probe.overflow ||
		    (admission && probe.effects[0] != CommandEffectProbeValue::FullMemoryBarrier)) return false;
	}
	CommandEffectProbe unproved;
	unproved.owner = cp;
	unproved.db_cache_proof_enabled = true;
	unproved.db_cache_proof_status = GpuMemoryDbCacheProofStatus::Rejected;
	{
		CommandEffectProbeScope scope(&unproved);
		if (execute_db_cache_event(cp, header, control, 2, true).status != GpuMemoryDbCacheProofStatus::Rejected) return false;
	}
	if (unproved.db_cache_proofs != 1 || unproved.count != 0) return false;
	uint32_t stream[] = {header, control, LUMEN_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO), 0};
	std::array<uint32_t, std::size(stream)> saved {};
	std::copy(std::begin(stream), std::end(stream), saved.begin());
	uint64_t sentinel = 0x0123456789abcdefull;
	for (const bool direct_parser: {true, false})
	{
		CommandEffectProbe probe;
		probe.owner = cp;
		probe.destination64 = &sentinel;
		probe.db_cache_proof_enabled = true;
		probe.db_cache_admission_enabled = true;
		{
			CommandEffectProbeScope scope(&probe);
			if (direct_parser) { if (cp_op_event_write(cp, header, stream + 1, 2, 2) != 1) return false; }
			else cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
		}
		if (probe.db_cache_proofs != 1 || probe.count != 1 || probe.overflow ||
		    probe.effects[0] != CommandEffectProbeValue::FullMemoryBarrier || sentinel != 0x0123456789abcdefull ||
		    !std::equal(std::begin(stream), std::end(stream), saved.begin())) return false;
	}
	std::fprintf(stderr, "[GFX_DB_CACHE_PM4_TEST] exact-raw=pass all32header/control/index15/2b/2c/CP65=reject"
	    " admission-off/unproved=no-barrier parser/Run=one-barrier packet/sentinel=preserved native-proof=modeled\n");
	return true;
}

static bool graphics_run_cb_meta_event_test(CommandProcessor* cp)
{
	if (cp == nullptr || cp->GetInterruptEventId() != 64) return false;
	auto* ctx = cp->GetCtx();
	const auto saved = *ctx;
	const bool result = [&]()
	{
		if (!exact_one_environment_value("1") || exact_one_environment_value(nullptr) || exact_one_environment_value("") ||
		    exact_one_environment_value("0") || exact_one_environment_value("01") || exact_one_environment_value("true") ||
		    exact_one_environment_value("1 ") || exact_one_environment_value("1x")) return false;
		const bool enabled = experimental_cb_meta_event_enabled();
		if (enabled != exact_one_environment_value(std::getenv("LUMEN_EXPERIMENTAL_GFX_CB_META_EVENT"))) return false;
		HW::Context clean;
		clean.SetRenderTargetMask(0x13579bdfu);
		clean.SetShaderStages(0x2468ace0u);
		*ctx = clean;
		if (HasUnsupportedColorMetadata(*ctx) || !pm4_cb_meta_event_supported(0x72eu, true, 64, *ctx) ||
		    pm4_cb_meta_event_supported(0x72eu, false, 64, *ctx)) return false;
		for (const uint32_t queue: {0u, 1u, 63u, 65u, UINT32_MAX})
		{
			if (pm4_cb_meta_event_supported(0x72eu, true, queue, *ctx)) return false;
		}
		// DCC configuration is retained even when DCC has no base or enable.
		// Exercise every configurable field together without treating those
		// values as authority to access or materialize DCC metadata.
		const auto apply_configured_dcc_control = [](HW::Context* state, uint32_t slot)
		{
			auto dcc = state->GetRenderTarget(slot).dcc;
			dcc.max_uncompressed_block_size = 3;
			dcc.max_compressed_block_size = 3;
			dcc.min_compressed_block_size = 1;
			dcc.color_transform = 3;
			dcc.overwrite_combiner_disable = true;
			dcc.independent_64b_blocks = true;
			dcc.independent_128b_blocks = true;
			dcc.data_write_on_dcc_clear_to_reg = true;
			state->SetColorDccControl(slot, dcc);
		};
		for (uint32_t slot = 0; slot < 8; ++slot)
		{
			*ctx = clean;
			apply_configured_dcc_control(ctx, slot);
			for (const bool admission: {false, true})
			{
				const auto slot_verdict = HW::ClassifyColorMetadataSlot(ctx->GetRenderTarget(slot));
				const auto verdict = HW::ClassifyColorMetadata(*ctx);
				if (slot_verdict.profile != HW::ColorMetadataProfile::None || verdict.profile != HW::ColorMetadataProfile::None ||
				    HasUnsupportedColorMetadata(*ctx) || pm4_cb_meta_event_supported(0x72eu, admission, 64, *ctx) != admission) return false;
				CommandEffectProbe configured_probe;
				configured_probe.owner = cp;
				{
					CommandEffectProbeScope scope(&configured_probe);
					if (execute_cb_meta_event(cp, 0x72eu, admission) != admission) return false;
				}
				if (configured_probe.overflow || configured_probe.count != (admission ? 1u : 0u) ||
				    (admission && configured_probe.effects[0] != CommandEffectProbeValue::FullMemoryBarrier) ||
				    HW::ClassifyColorMetadata(*ctx).profile != HW::ColorMetadataProfile::None ||
				    ctx->GetRenderTargetMask() != 0x13579bdfu || ctx->GetShaderStages() != 0x2468ace0u) return false;
			}
		}
		*ctx = clean;

		uint32_t control = 0xdeadbeefu;
		const uint32_t body = 0x72eu;
		for (const uint32_t header: {0u, 0xc0014600u, 0xc0004601u, 0xc0004602u, 0xc0004700u, 0x80004600u})
		{
			if (pm4_decode_event_write(header, &body, 3, &control) || control != 0xdeadbeefu) return false;
		}
		for (const uint32_t available: {0u, 1u})
		{
			if (pm4_decode_event_write(0xc0004600u, &body, available, &control) || control != 0xdeadbeefu) return false;
		}
		if (pm4_decode_event_write(0xc0004600u, nullptr, 2, &control) || control != 0xdeadbeefu ||
		    pm4_decode_event_write(0xc0004600u, &body, 2, nullptr) ||
		    !pm4_decode_event_write(0xc0004600u, &body, 2, &control) || control != body) return false;

		CommandEffectProbe rejected_probe;
		rejected_probe.owner = cp;
		{
			CommandEffectProbeScope scope(&rejected_probe);
			if (execute_cb_meta_event(cp, 0x72eu, false) || execute_cb_meta_event(nullptr, 0x72eu, true)) return false;
			// Every altered control bit is rejected, including the index's fourth
			// bit11 and all control bits that the legacy event path does not decode.
			for (uint32_t bit = 0; bit < 32; ++bit)
			{
				const uint32_t wrong = 0x72eu ^ (uint32_t {1} << bit);
				if (pm4_cb_meta_event_supported(wrong, true, 64, *ctx) || execute_cb_meta_event(cp, wrong, true)) return false;
			}
			for (uint32_t index = 0; index < 16; ++index)
			{
				if (index != 7 && execute_cb_meta_event(cp, (index << 8u) | 0x2eu, true)) return false;
			}
			for (const uint32_t wrong: {0x716u, 0x731u, 0x72cu, 0x10u, 0xffffffffu})
			{
				if (execute_cb_meta_event(cp, wrong, true)) return false;
			}

			using MetadataMutation = void (*)(HW::Context*, uint32_t);
			// Profiles with no renderer representation stay fail-closed: DCC,
			// FMASK compression, multisample encoding, linear CMASK addressing,
			// pipe-aligned metadata and CMASK without fast clear.
			constexpr MetadataMutation unsupported_mutations[] = {
			    [](HW::Context* state, uint32_t slot) { HW::ColorCmask v; v.addr = 0x100u; state->SetColorCmask(slot, v); },
			    [](HW::Context* state, uint32_t slot) { state->SetColorDccAddr(slot, {0x300u}); },
			    [](HW::Context* state, uint32_t slot) { HW::ColorInfo v; v.fmask_compression_enable = true; state->SetColorInfo(slot, v); },
			    [](HW::Context* state, uint32_t slot) { HW::ColorInfo v; v.fmask_data_compression_disable = true; state->SetColorInfo(slot, v); },
			    [](HW::Context* state, uint32_t slot) { HW::ColorInfo v; v.fmask_one_frag_mode = true; state->SetColorInfo(slot, v); },
			    [](HW::Context* state, uint32_t slot) { HW::ColorInfo v; v.dcc_compression_enable = true; state->SetColorInfo(slot, v); },
			    [](HW::Context* state, uint32_t slot) { HW::ColorDccControl v; v.dcc_clear_key_enable = true; state->SetColorDccControl(slot, v); },
			    [](HW::Context* state, uint32_t slot) { HW::ColorAttrib v; v.num_samples = 1; state->SetColorAttrib(slot, v); },
			    [](HW::Context* state, uint32_t slot) { HW::ColorAttrib v; v.num_fragments = 1; state->SetColorAttrib(slot, v); },
			    [](HW::Context* state, uint32_t slot) { HW::ColorInfo v; v.cmask_tile_mode = 1; state->SetColorInfo(slot, v); },
			    [](HW::Context* state, uint32_t slot) { HW::ColorInfo v; v.cmask_tile_mode_neo = 2; state->SetColorInfo(slot, v); },
			    [](HW::Context* state, uint32_t slot) { HW::ColorAttrib3 v; v.cmask_pipe_aligned = true; state->SetColorAttrib3(slot, v); },
			};
			for (uint32_t slot = 0; slot < 8; ++slot)
			{
				for (const auto mutate: unsupported_mutations)
				{
					for (const bool configured_dcc: {false, true})
					{
						*ctx = clean;
						mutate(ctx, slot);
						if (configured_dcc) apply_configured_dcc_control(ctx, slot);
						uint32_t first_slot = UINT32_MAX;
						const auto& rt = ctx->GetRenderTarget(slot);
						const auto slot_verdict = HW::ClassifyColorMetadataSlot(rt);
						if (slot_verdict.profile != HW::ColorMetadataProfile::Unsupported ||
						    ((rt.dcc_addr.addr != 0 || rt.info.dcc_compression_enable || rt.dcc.dcc_clear_key_enable) &&
						     std::strcmp(slot_verdict.cause, "dcc") != 0)) return false;
						if (!HasUnsupportedColorMetadata(*ctx, &first_slot) || first_slot != slot ||
						    pm4_cb_meta_event_supported(0x72eu, true, 64, *ctx) ||
						    execute_cb_meta_event(cp, 0x72eu, true) || !HasUnsupportedColorMetadata(*ctx) ||
						    ctx->GetRenderTargetMask() != 0x13579bdfu || ctx->GetShaderStages() != 0x2468ace0u) return false;
					}
				}
			}
		}
		if (rejected_probe.overflow || rejected_probe.count != 0) return false;

		// The adopted contract is the single-sample CMASK fast-clear profile
		// that active titles exercise: FAST_CLEAR enabled, a CMASK surface, the
		// clear words and the observed FMASK alias of the color base. The
		// renderer keeps the surface uncompressed and materialized, so the
		// metadata only selects the ordering barrier.
		const auto apply_fast_clear_profile = [](HW::Context* state, uint32_t slot)
		{
			HW::ColorInfo info;
			info.cmask_fast_clear_enable = true;
			state->SetColorInfo(slot, info);
			HW::ColorSlice slice;
			slice.slice_div64_minus1 = 0x23ffu;
			state->SetColorSlice(slot, slice);
			state->SetColorCmask(slot, {0x719be0e0u});
			HW::ColorCmaskSlice cmask_slice;
			cmask_slice.slice_minus1 = 0x2fu;
			state->SetColorCmaskSlice(slot, cmask_slice);
			state->SetColorFmask(slot, {0x71cbc400u});
			HW::ColorFmaskSlice fmask_slice;
			fmask_slice.slice_minus1 = 0x23ffu;
			state->SetColorFmaskSlice(slot, fmask_slice);
			state->SetColorClearWord0(slot, {0xff000000u});
		};
		for (uint32_t slot = 0; slot < 8; ++slot)
		{
			for (const bool configured_dcc: {false, true})
			{
				*ctx = clean;
				apply_fast_clear_profile(ctx, slot);
				if (configured_dcc) apply_configured_dcc_control(ctx, slot);
				const auto slot_verdict = HW::ClassifyColorMetadataSlot(ctx->GetRenderTarget(slot));
				const auto verdict = HW::ClassifyColorMetadata(*ctx);
				uint32_t first_slot = UINT32_MAX;
				if (slot_verdict.profile != HW::ColorMetadataProfile::FastClearSingleSample ||
				    verdict.profile != HW::ColorMetadataProfile::FastClearSingleSample || verdict.first_slot != slot ||
				    HasUnsupportedColorMetadata(*ctx, &first_slot) ||
				    !pm4_cb_meta_event_supported(0x72eu, true, 64, *ctx) ||
				    pm4_cb_meta_event_supported(0x72eu, false, 64, *ctx)) return false;
				CommandEffectProbe fast_clear_probe;
				fast_clear_probe.owner = cp;
				{
					CommandEffectProbeScope scope(&fast_clear_probe);
					if (!execute_cb_meta_event(cp, 0x72eu, true)) return false;
				}
				if (fast_clear_probe.overflow || fast_clear_probe.count != 1 ||
				    fast_clear_probe.effects[0] != CommandEffectProbeValue::FullMemoryBarrier) return false;
			}
		}
		// A prior admitted slot must not replace the identity/cause of a later
		// rejected slot. Exercise both scan orders and retain zero effects on
		// every mixed-profile rejection.
		CommandEffectProbe mixed_profile_probe;
		mixed_profile_probe.owner = cp;
		{
			CommandEffectProbeScope scope(&mixed_profile_probe);
			for (uint32_t admitted_slot = 0; admitted_slot < 8; ++admitted_slot)
			{
				for (uint32_t rejected_slot = 0; rejected_slot < 8; ++rejected_slot)
				{
					if (admitted_slot == rejected_slot) continue;
					*ctx = clean;
					apply_fast_clear_profile(ctx, admitted_slot);
					apply_configured_dcc_control(ctx, admitted_slot);
					apply_configured_dcc_control(ctx, rejected_slot);
					ctx->SetColorDccAddr(rejected_slot, {0x300u});
					const auto verdict = HW::ClassifyColorMetadata(*ctx);
					uint32_t first_slot = UINT32_MAX;
					if (verdict.profile != HW::ColorMetadataProfile::Unsupported || verdict.first_slot != rejected_slot ||
					    std::strcmp(verdict.cause, "dcc") != 0 ||
					    !HasUnsupportedColorMetadata(*ctx, &first_slot) || first_slot != rejected_slot ||
					    pm4_cb_meta_event_supported(0x72eu, true, 64, *ctx) || execute_cb_meta_event(cp, 0x72eu, true) ||
					    ctx->GetRenderTargetMask() != 0x13579bdfu || ctx->GetShaderStages() != 0x2468ace0u) return false;
				}
			}
		}
		if (mixed_profile_probe.overflow || mixed_profile_probe.count != 0) return false;
		*ctx = clean;
		auto* compute = new CommandProcessor(1);
		compute->SetHeadlessTestMode();
		CommandEffectProbe compute_probe;
		compute_probe.owner = compute;
		{
			CommandEffectProbeScope scope(&compute_probe);
			if (execute_cb_meta_event(compute, 0x72eu, true)) return false;
		}
		if (compute_probe.overflow || compute_probe.count != 0) return false;

		CommandEffectProbe prior_probe;
		prior_probe.owner = cp;
		{
			CommandEffectProbeScope scope(&prior_probe);
			uint32_t prior[] = {0xc0004600u, 0x716u, 0xc0004600u, 0x731u, 0xc0004600u, 0x72cu, 0xc0004600u, 0x10u};
			cp->Run(prior, static_cast<uint32_t>(std::size(prior)));
		}
		if (prior_probe.overflow || prior_probe.count != 2 || prior_probe.effects[0] != CommandEffectProbeValue::MemoryBarrier ||
		    prior_probe.effects[1] != CommandEffectProbeValue::MemoryBarrier) return false;
		CommandEffectProbe accepted_probe;
		accepted_probe.owner = cp;
		{
			CommandEffectProbeScope scope(&accepted_probe);
			if (enabled)
			{
				uint32_t stream[] = {0xc0004600u, 0x72eu};
				cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
			}
		}
		return !accepted_probe.overflow && accepted_probe.count == (enabled ? 1u : 0u) &&
		       (!enabled || accepted_probe.effects[0] == CommandEffectProbeValue::FullMemoryBarrier) &&
		       !HasUnsupportedColorMetadata(*ctx) && ctx->GetRenderTargetMask() == 0x13579bdfu && ctx->GetShaderStages() == 0x2468ace0u;
	}();
	*ctx = saved;
	if (!result) std::fprintf(stderr, "CB metadata event selector, decoder, metadata admission or effect contract failed\n");
	return result;
}

static bool graphics_run_index_buffer_size_test(CommandProcessor* cp)
{
	auto state_matches = [cp](uint32_t index_count, bool configured)
	{
		const auto state = cp->GetIndexBufferSizeState();
		return state.index_count == index_count && state.configured == configured;
	};
	if (!state_matches(0, false))
	{
		std::fprintf(stderr, "index-buffer size did not start unset\n");
		return false;
	}
	cp->GetCtx()->SetIndexOffset(0x12345678u);
	constexpr uint32_t writes[] = {0u, 0x80000000u, 0xffffffffu, 7u};
	for (const auto value: writes)
	{
		if (cp_op_index_buffer_size(cp, 0xc0001300u, &value, 2, 2) != 1 || !state_matches(value, true) ||
		    cp->GetCtx()->GetIndexOffset() != 0x12345678u)
		{
			std::fprintf(stderr, "index-buffer size write lost state or changed the index offset\n");
			return false;
		}
	}

	struct InvalidPacket
	{
		uint32_t header;
		uint32_t available;
	};
	constexpr InvalidPacket invalid[] = {
	    {0xc0001300u, 0}, {0xc0001300u, 1}, {0xc0011300u, 3},
	    {0xc0001200u, 2}, {0x80001300u, 2}, {0xc0001301u, 2}, {0xc0001302u, 2}, {0xc0001304u, 2},
	};
	const uint32_t rejected_value = 19;
	for (const auto& packet: invalid)
	{
		if (cp_update_index_buffer_size(cp, packet.header, &rejected_value, packet.available) != 0 ||
		    !state_matches(7, true) || cp->GetCtx()->GetIndexOffset() != 0x12345678u)
		{
			std::fprintf(stderr, "malformed index-buffer size packet changed state\n");
			return false;
		}
	}
	if (cp_update_index_buffer_size(cp, 0xc0001300u, nullptr, 2) != 0 ||
	    cp_update_index_buffer_size(nullptr, 0xc0001300u, &rejected_value, 2) != 0 || !state_matches(7, true))
	{
		return false;
	}

	uint32_t stream[] = {
	    0xc0001300u, 0xffffffffu,
	    0xc0016900u, Pm4::CB_TARGET_MASK, 0x87654321u,
	    0xc0001300u, 0u,
	};
	cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
	if (!state_matches(0, true) || cp->GetCtx()->GetRenderTargetMask() != 0x87654321u ||
	    cp->GetCtx()->GetIndexOffset() != 0x12345678u)
	{
		std::fprintf(stderr, "index-buffer size packet stream lost alignment or state\n");
		return false;
	}
	cp->Reset();
	if (!state_matches(0, false))
	{
		std::fprintf(stderr, "index-buffer size reset retained configured state\n");
		return false;
	}
	return true;
}

static bool graphics_run_index_offset_test(CommandProcessor* cp)
{
	auto* ctx = cp->GetCtx();
	if (ctx->GetIndexOffset() != 0 || !ctx->IsIndexOffsetSupportedForDraw())
	{
		std::fprintf(stderr, "index-offset initial state is not the local zero policy\n");
		return false;
	}

	ctx->SetRenderTargetMask(0x12345678u);
	ctx->SetShaderStages(0x87654321u);
	ctx->SetGuardBands(1.25f, 2.5f, 3.75f, 5.0f);
	auto unrelated_state_preserved = [ctx]()
	{
		const auto& viewport = ctx->GetScreenViewport();
		return ctx->GetRenderTargetMask() == 0x12345678u && ctx->GetShaderStages() == 0x87654321u &&
		       viewport.guard_band_horz_clip == 1.25f && viewport.guard_band_vert_clip == 2.5f &&
		       viewport.guard_band_horz_discard == 3.75f && viewport.guard_band_vert_discard == 5.0f;
	};
	constexpr uint32_t writes[] = {1u, 0x80000000u, 0xffffffffu, 0u};
	for (const auto value: writes)
	{
		uint32_t packet[] = {Pm4::VGT_INDX_OFFSET, value};
		if (cp_op_set_context_reg(cp, 0xc0016900u, packet, 3, 3) != 2 || ctx->GetIndexOffset() != value ||
		    ctx->IsIndexOffsetSupportedForDraw() != (value == 0) || !unrelated_state_preserved())
		{
			std::fprintf(stderr, "index-offset write lost bits or changed unrelated state\n");
			return false;
		}
	}

	ctx->SetIndexOffset(0xabcdef01u);
	struct InvalidPacket
	{
		uint32_t header;
		uint32_t offset;
		uint32_t available;
	};
	constexpr InvalidPacket invalid[] = {
	    {0xc0006900u, Pm4::VGT_INDX_OFFSET, 2},
	    {0xc0026900u, Pm4::VGT_INDX_OFFSET, 4},
	    {0xc0016800u, Pm4::VGT_INDX_OFFSET, 3},
	    {0x80016900u, Pm4::VGT_INDX_OFFSET, 3},
	    {0xc0016901u, Pm4::VGT_INDX_OFFSET, 3},
	    {0xc0016902u, Pm4::VGT_INDX_OFFSET, 3},
	    {0xc0016904u, Pm4::VGT_INDX_OFFSET, 3},
	    {0xc0016900u, Pm4::VGT_INDX_OFFSET - 1u, 3},
	    {0xc0016900u, Pm4::VGT_INDX_OFFSET + 1u, 3},
	    {0xc0016900u, Pm4::VGT_INDX_OFFSET, 0},
	    {0xc0016900u, Pm4::VGT_INDX_OFFSET, 1},
	    {0xc0016900u, Pm4::VGT_INDX_OFFSET, 2},
	};
	const uint32_t rejected_value = 0x13579bdfu;
	for (const auto& packet: invalid)
	{
		if (hw_ctx_update_index_offset(ctx, packet.header, packet.offset, &rejected_value, packet.available) != 0 ||
		    ctx->GetIndexOffset() != 0xabcdef01u || ctx->IsIndexOffsetSupportedForDraw() || !unrelated_state_preserved())
		{
			std::fprintf(stderr, "malformed index-offset packet was accepted or changed state\n");
			return false;
		}
	}
	if (hw_ctx_update_index_offset(ctx, 0xc0016900u, Pm4::VGT_INDX_OFFSET, nullptr, 3) != 0 ||
	    hw_ctx_update_index_offset(nullptr, 0xc0016900u, Pm4::VGT_INDX_OFFSET, &rejected_value, 3) != 0 ||
	    ctx->GetIndexOffset() != 0xabcdef01u || ctx->IsIndexOffsetSupportedForDraw() || !unrelated_state_preserved())
	{
		std::fprintf(stderr, "index-offset null input changed state\n");
		return false;
	}

	uint32_t stream[] = {
	    0xc0016900u, Pm4::VGT_INDX_OFFSET, 0xffffffffu,
	    0xc0016900u, Pm4::CB_TARGET_MASK, 0x12345678u,
	    0xc0016900u, Pm4::VGT_SHADER_STAGES_EN, 0x87654321u,
	};
	ctx->SetRenderTargetMask(0);
	ctx->SetShaderStages(0);
	cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
	if (ctx->GetIndexOffset() != 0xffffffffu || ctx->IsIndexOffsetSupportedForDraw() || !unrelated_state_preserved())
	{
		std::fprintf(stderr, "index-offset packet stream lost alignment or state\n");
		return false;
	}
	cp->Reset();
	if (ctx->GetIndexOffset() != 0 || !ctx->IsIndexOffsetSupportedForDraw())
	{
		std::fprintf(stderr, "index-offset command processor reset retained stale state\n");
		return false;
	}
	return true;
}

static bool graphics_run_instance_step_rate_test(CommandProcessor* cp)
{
	auto* ctx = cp->GetCtx();
	if (ctx->GetInstanceStepRate(0) != 0 || ctx->GetInstanceStepRate(1) != 0)
	{
		std::fprintf(stderr, "instance-step-rate initial state is not the local zero policy\n");
		return false;
	}

	ctx->SetRenderTargetMask(0x12345678u);
	ctx->SetShaderStages(0x87654321u);
	ctx->SetGuardBands(1.25f, 2.5f, 3.75f, 5.0f);
	auto unrelated_state_preserved = [ctx]()
	{
		const auto& viewport = ctx->GetScreenViewport();
		return ctx->GetRenderTargetMask() == 0x12345678u && ctx->GetShaderStages() == 0x87654321u &&
		       viewport.guard_band_horz_clip == 1.25f && viewport.guard_band_vert_clip == 2.5f &&
		       viewport.guard_band_horz_discard == 3.75f && viewport.guard_band_vert_discard == 5.0f;
	};

	// Both words retain every raw value; the pair span observed in the game
	// reaches the second word through one canonical packet.
	constexpr uint32_t writes[] = {1u, 0x80000000u, 0xffffffffu, 0u};
	for (const auto value: writes)
	{
		const uint32_t preserved_second = ctx->GetInstanceStepRate(1);
		uint32_t       first[]          = {Pm4::VGT_INSTANCE_STEP_RATE_0, value};
		if (cp_op_set_context_reg(cp, 0xc0016900u, first, 3, 3) != 2 || ctx->GetInstanceStepRate(0) != value ||
		    ctx->GetInstanceStepRate(1) != preserved_second || !unrelated_state_preserved())
		{
			std::fprintf(stderr, "instance-step-rate first-word write lost bits or changed unrelated state\n");
			return false;
		}
		uint32_t second[] = {Pm4::VGT_INSTANCE_STEP_RATE_1, value};
		if (cp_op_set_context_reg(cp, 0xc0016900u, second, 3, 3) != 2 || ctx->GetInstanceStepRate(0) != value ||
		    ctx->GetInstanceStepRate(1) != value || !unrelated_state_preserved())
		{
			std::fprintf(stderr, "instance-step-rate second-word write lost bits or changed unrelated state\n");
			return false;
		}
	}
	const uint32_t pair_values[] = {0x00000001u, 0x00000003u};
	uint32_t       pair[]        = {Pm4::VGT_INSTANCE_STEP_RATE_0, pair_values[0], pair_values[1]};
	if (cp_op_set_context_reg(cp, 0xc0026900u, pair, 4, 4) != 3 || ctx->GetInstanceStepRate(0) != pair_values[0] ||
	    ctx->GetInstanceStepRate(1) != pair_values[1] || !unrelated_state_preserved())
	{
		std::fprintf(stderr, "instance-step-rate pair span lost bits or changed unrelated state\n");
		return false;
	}

	uint32_t indirect_values[] = {Pm4::VGT_INSTANCE_STEP_RATE_0, 0x11223344u, Pm4::VGT_INSTANCE_STEP_RATE_1, 0x55667788u};
	const auto indirect_addr   = reinterpret_cast<uint64_t>(indirect_values);
	uint32_t indirect_body[] = {2u, static_cast<uint32_t>(indirect_addr), static_cast<uint32_t>(indirect_addr >> 32u)};
	if (cp_op_indirect_cx_regs(cp, 0xc0021048u, indirect_body, 4, 4) != 3 || ctx->GetInstanceStepRate(0) != 0x11223344u ||
	    ctx->GetInstanceStepRate(1) != 0x55667788u || !unrelated_state_preserved())
	{
		std::fprintf(stderr, "instance-step-rate indirect write lost bits or changed unrelated state\n");
		return false;
	}

	ctx->SetInstanceStepRate(0, 0xabcdef01u);
	ctx->SetInstanceStepRate(1, 0xabcdef02u);
	struct InvalidPacket
	{
		uint32_t header;
		uint32_t offset;
		uint32_t available;
	};
	constexpr InvalidPacket invalid[] = {
	    {0xc0006900u, Pm4::VGT_INSTANCE_STEP_RATE_0, 2},
	    {0xc0026900u, Pm4::VGT_INSTANCE_STEP_RATE_1, 4},
	    {0xc0036900u, Pm4::VGT_INSTANCE_STEP_RATE_0, 5},
	    {0xc0016800u, Pm4::VGT_INSTANCE_STEP_RATE_0, 3},
	    {0x80016900u, Pm4::VGT_INSTANCE_STEP_RATE_0, 3},
	    {0xc0016901u, Pm4::VGT_INSTANCE_STEP_RATE_0, 3},
	    {0xc0016902u, Pm4::VGT_INSTANCE_STEP_RATE_0, 3},
	    {0xc0016904u, Pm4::VGT_INSTANCE_STEP_RATE_0, 3},
	    {0xc0016900u, Pm4::VGT_INSTANCE_STEP_RATE_0 - 2u, 3},
	    {0xc0016900u, Pm4::VGT_INSTANCE_STEP_RATE_0 - 1u, 3},
	    {0xc0016900u, Pm4::VGT_INSTANCE_STEP_RATE_1 + 1u, 3},
	    {0xc0016900u, Pm4::VGT_INSTANCE_STEP_RATE_0, 0},
	    {0xc0016900u, Pm4::VGT_INSTANCE_STEP_RATE_0, 1},
	    {0xc0016900u, Pm4::VGT_INSTANCE_STEP_RATE_0, 2},
	    {0xc0026900u, Pm4::VGT_INSTANCE_STEP_RATE_0, 3},
	};
	const uint32_t rejected_value = 0x13579bdfu;
	for (const auto& packet: invalid)
	{
		if (hw_ctx_update_instance_step_rate(ctx, packet.header, packet.offset, &rejected_value, packet.available) != 0 ||
		    ctx->GetInstanceStepRate(0) != 0xabcdef01u || ctx->GetInstanceStepRate(1) != 0xabcdef02u ||
		    !unrelated_state_preserved())
		{
			std::fprintf(stderr, "malformed instance-step-rate packet was accepted or changed state\n");
			return false;
		}
	}
	if (hw_ctx_update_instance_step_rate(ctx, 0xc0016900u, Pm4::VGT_INSTANCE_STEP_RATE_0, nullptr, 3) != 0 ||
	    hw_ctx_update_instance_step_rate(nullptr, 0xc0016900u, Pm4::VGT_INSTANCE_STEP_RATE_0, &rejected_value, 3) != 0 ||
	    ctx->GetInstanceStepRate(0) != 0xabcdef01u || ctx->GetInstanceStepRate(1) != 0xabcdef02u ||
	    !unrelated_state_preserved())
	{
		std::fprintf(stderr, "instance-step-rate null input changed state\n");
		return false;
	}

	// CLEAR_STATE wipes the whole context; only the step-rate words are asserted here.
	uint32_t clear[] = {LUMEN_PM4(2, Pm4::IT_CLEAR_STATE, 0), 0};
	cp->Run(clear, static_cast<uint32_t>(std::size(clear)));
	if (ctx->GetInstanceStepRate(0) != 0 || ctx->GetInstanceStepRate(1) != 0)
	{
		std::fprintf(stderr, "instance-step-rate clear state did not restore the local zero policy\n");
		return false;
	}

	ctx->SetRenderTargetMask(0);
	ctx->SetShaderStages(0x87654321u);
	ctx->SetGuardBands(1.25f, 2.5f, 3.75f, 5.0f);
	uint32_t stream[] = {
	    0xc0026900u, Pm4::VGT_INSTANCE_STEP_RATE_0, 0x00000001u, 0x00000003u,
	    0xc0016900u, Pm4::VGT_INSTANCE_STEP_RATE_1, 0x00000005u,
	    0xc0016900u, Pm4::CB_TARGET_MASK, 0x12345678u,
	};
	cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
	if (ctx->GetInstanceStepRate(0) != 1u || ctx->GetInstanceStepRate(1) != 5u || !unrelated_state_preserved())
	{
		std::fprintf(stderr, "instance-step-rate packet stream lost alignment or state\n");
		return false;
	}
	cp->Reset();
	if (ctx->GetInstanceStepRate(0) != 0 || ctx->GetInstanceStepRate(1) != 0)
	{
		std::fprintf(stderr, "instance-step-rate command processor reset retained stale state\n");
		return false;
	}
	return true;
}

static bool graphics_run_geometry_streamout_state_test(CommandProcessor* cp)
{
	auto* ctx = cp->GetCtx();
	if (ctx->GetGsMode() != 0 || ctx->IsGsModeConfigured() || ctx->GetStreamoutConfig() != 0 ||
	    ctx->IsStreamoutConfigConfigured() || !ctx->IsGeometryStreamoutSupportedForDraw())
	{
		std::fprintf(stderr, "geometry/stream-out initial state is not the local unset policy\n");
		return false;
	}

	ctx->SetRenderTargetMask(0x12345678u);
	ctx->SetShaderStages(0x87654321u);
	ctx->SetIndexOffset(0);
	auto unrelated_state_preserved = [ctx]()
	{
		return ctx->GetRenderTargetMask() == 0x12345678u && ctx->GetShaderStages() == 0x87654321u &&
		       ctx->GetIndexOffset() == 0;
	};

	constexpr uint32_t offsets[] = {Pm4::VGT_GS_MODE, Pm4::VGT_STRMOUT_CONFIG};
	constexpr uint32_t writes[]  = {1u, 0x80000000u, 0xffffffffu, 0u};
	for (const auto offset: offsets)
	{
		for (const auto value: writes)
		{
			uint32_t packet[] = {offset, value};
			if (cp_op_set_context_reg(cp, LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), packet, 3, 3) != 2 ||
			    !unrelated_state_preserved())
			{
				std::fprintf(stderr, "geometry/stream-out write changed unrelated state\n");
				return false;
			}
			const bool expected_configured = (value == 0);
			if ((offset == Pm4::VGT_GS_MODE &&
			     (ctx->GetGsMode() != value || ctx->IsGsModeConfigured() != expected_configured)) ||
			    (offset == Pm4::VGT_STRMOUT_CONFIG &&
			     (ctx->GetStreamoutConfig() != value || ctx->IsStreamoutConfigConfigured() != expected_configured)) ||
			    ctx->IsGeometryStreamoutSupportedForDraw() != expected_configured)
			{
				std::fprintf(stderr, "geometry/stream-out write lost bits or violated the zero-only policy\n");
				return false;
			}
		}
	}

	ctx->SetGsMode(0xabcdef01u);
	ctx->SetStreamoutConfig(0x13579bdfu);
	struct InvalidPacket
	{
		uint32_t header;
		uint32_t offset;
		uint32_t available;
	};
	constexpr InvalidPacket invalid[] = {
	    {LUMEN_PM4(2, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::VGT_GS_MODE, 2},
	    {LUMEN_PM4(4, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::VGT_GS_MODE, 4},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONFIG_REG, 0), Pm4::VGT_GS_MODE, 3},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0) | 1u, Pm4::VGT_GS_MODE, 3},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::VGT_GS_MODE - 1u, 3},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::VGT_GS_MODE + 1u, 3},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::VGT_STRMOUT_CONFIG - 1u, 3},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::VGT_STRMOUT_CONFIG + 1u, 3},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::VGT_STRMOUT_CONFIG, 0},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::VGT_STRMOUT_CONFIG, 1},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::VGT_STRMOUT_CONFIG, 2},
	};
	const uint32_t rejected_value = 0;
	for (const auto& packet: invalid)
	{
		if (hw_ctx_update_geometry_streamout_state(ctx, packet.header, packet.offset, &rejected_value, packet.available) != 0 ||
		    ctx->GetGsMode() != 0xabcdef01u || ctx->IsGsModeConfigured() || ctx->GetStreamoutConfig() != 0x13579bdfu ||
		    ctx->IsStreamoutConfigConfigured() || ctx->IsGeometryStreamoutSupportedForDraw() || !unrelated_state_preserved())
		{
			std::fprintf(stderr, "malformed geometry/stream-out packet was accepted or changed state\n");
			return false;
		}
	}
	if (hw_ctx_update_geometry_streamout_state(ctx, LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::VGT_GS_MODE, nullptr, 3) != 0 ||
	    hw_ctx_update_geometry_streamout_state(nullptr, LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::VGT_GS_MODE,
	                                           &rejected_value, 3) != 0 ||
	    ctx->GetGsMode() != 0xabcdef01u || ctx->GetStreamoutConfig() != 0x13579bdfu || !unrelated_state_preserved())
	{
		std::fprintf(stderr, "geometry/stream-out null input changed state\n");
		return false;
	}

	uint32_t stream[] = {
	    LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::VGT_GS_MODE, 0u,
	    LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::CB_TARGET_MASK, 0x12345678u,
	    LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::VGT_STRMOUT_CONFIG, 0u,
	};
	cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
	if (ctx->GetGsMode() != 0 || !ctx->IsGsModeConfigured() || ctx->GetStreamoutConfig() != 0 ||
	    !ctx->IsStreamoutConfigConfigured() || !ctx->IsGeometryStreamoutSupportedForDraw() || !unrelated_state_preserved())
	{
		std::fprintf(stderr, "geometry/stream-out packet stream lost alignment or state\n");
		return false;
	}

	uint32_t indirect_values[] = {Pm4::VGT_GS_MODE, 0xffffffffu, Pm4::VGT_STRMOUT_CONFIG, 0u};
	auto     indirect_addr     = reinterpret_cast<uint64_t>(indirect_values);
	uint32_t indirect_body[]   = {2u, static_cast<uint32_t>(indirect_addr), static_cast<uint32_t>(indirect_addr >> 32u)};
	if (cp_op_indirect_cx_regs(cp, 0xc0021048u, indirect_body, 4, 4) != 3 || ctx->GetGsMode() != 0xffffffffu ||
	    ctx->IsGsModeConfigured() || ctx->GetStreamoutConfig() != 0 || !ctx->IsStreamoutConfigConfigured() ||
	    ctx->IsGeometryStreamoutSupportedForDraw() || !unrelated_state_preserved())
	{
		std::fprintf(stderr, "indirect geometry/stream-out update diverged from direct state\n");
		return false;
	}
	indirect_values[1] = 0;
	indirect_values[3] = 0x80000000u;
	if (cp_op_indirect_cx_regs(cp, 0xc0021048u, indirect_body, 4, 4) != 3 || ctx->GetGsMode() != 0 ||
	    !ctx->IsGsModeConfigured() || ctx->GetStreamoutConfig() != 0x80000000u || ctx->IsStreamoutConfigConfigured() ||
	    ctx->IsGeometryStreamoutSupportedForDraw() || !unrelated_state_preserved())
	{
		std::fprintf(stderr, "indirect stream-out nonzero transition was not preserved\n");
		return false;
	}

	uint32_t indirect_reverse[] = {
	    Pm4::VGT_STRMOUT_CONFIG, 0u,
	    Pm4::VGT_GS_MODE, 0u,
	    Pm4::CB_TARGET_MASK, 0x2468ace0u,
	};
	indirect_addr    = reinterpret_cast<uint64_t>(indirect_reverse);
	indirect_body[0] = 3u;
	indirect_body[1] = static_cast<uint32_t>(indirect_addr);
	indirect_body[2] = static_cast<uint32_t>(indirect_addr >> 32u);
	if (cp_op_indirect_cx_regs(cp, 0xc0021048u, indirect_body, 4, 4) != 3 || ctx->GetGsMode() != 0 ||
	    !ctx->IsGsModeConfigured() || ctx->GetStreamoutConfig() != 0 || !ctx->IsStreamoutConfigConfigured() ||
	    !ctx->IsGeometryStreamoutSupportedForDraw() || ctx->GetRenderTargetMask() != 0x2468ace0u ||
	    ctx->GetShaderStages() != 0x87654321u || ctx->GetIndexOffset() != 0)
	{
		std::fprintf(stderr, "reverse indirect geometry/stream-out zero transition lost state or alignment\n");
		return false;
	}

	uint32_t indirect_forward[] = {
	    Pm4::CB_TARGET_MASK, 0x13579bdfu,
	    Pm4::VGT_GS_MODE, 0u,
	    Pm4::VGT_STRMOUT_CONFIG, 0u,
	};
	indirect_addr    = reinterpret_cast<uint64_t>(indirect_forward);
	indirect_body[1] = static_cast<uint32_t>(indirect_addr);
	indirect_body[2] = static_cast<uint32_t>(indirect_addr >> 32u);
	if (cp_op_indirect_cx_regs(cp, 0xc0021048u, indirect_body, 4, 4) != 3 || ctx->GetGsMode() != 0 ||
	    !ctx->IsGsModeConfigured() || ctx->GetStreamoutConfig() != 0 || !ctx->IsStreamoutConfigConfigured() ||
	    !ctx->IsGeometryStreamoutSupportedForDraw() || ctx->GetRenderTargetMask() != 0x13579bdfu ||
	    ctx->GetShaderStages() != 0x87654321u || ctx->GetIndexOffset() != 0)
	{
		std::fprintf(stderr, "forward indirect geometry/stream-out zero transition lost state or alignment\n");
		return false;
	}

	cp->Reset();
	if (ctx->GetGsMode() != 0 || ctx->IsGsModeConfigured() || ctx->GetStreamoutConfig() != 0 ||
	    ctx->IsStreamoutConfigConfigured() || !ctx->IsGeometryStreamoutSupportedForDraw())
	{
		std::fprintf(stderr, "geometry/stream-out reset retained stale state\n");
		return false;
	}
	return true;
}

static bool graphics_run_alpha_to_mask_test(CommandProcessor* cp)
{
	auto* ctx = cp->GetCtx();
	const auto saved = *ctx;
	const bool result = [&]()
	{
		HW::Context independent;
		if (independent.GetAlphaToMask() != 0 || !independent.IsAlphaToMaskSupportedForDraw()) return false;
		independent.SetAlphaToMask(0x8000aa01u);
		ctx->SetRenderTargetMask(0x13579bdfu);
		ctx->SetShaderStages(0x2468ace0u);
		ctx->SetPolygonOffsetRegister(0, 0x12345678u);
		ctx->ObservePolygonOffsetModeControl(0x87654321u);
		ctx->SetStreamoutConfig(0xdeadbeefu);
		auto unrelated = [&]()
		{
			return ctx->GetRenderTargetMask() == 0x13579bdfu && ctx->GetShaderStages() == 0x2468ace0u &&
			       ctx->GetPolygonOffsetRegisters().values[0] == 0x12345678u &&
			       ctx->GetPolygonOffsetRegisters().mode_control_raw == 0x87654321u && ctx->GetStreamoutConfig() == 0xdeadbeefu &&
			       independent.GetAlphaToMask() == 0x8000aa01u && !independent.IsAlphaToMaskSupportedForDraw();
		};
		uint32_t indirect_values[] = {Pm4::DB_ALPHA_TO_MASK, 0};
		const auto indirect_addr = reinterpret_cast<uint64_t>(indirect_values);
		uint32_t indirect_body[] = {1u, static_cast<uint32_t>(indirect_addr), static_cast<uint32_t>(indirect_addr >> 32u)};
		auto check_write = [&](uint32_t value, bool supported)
		{
			uint32_t packet[] = {Pm4::DB_ALPHA_TO_MASK, value};
			if (cp_op_set_context_reg(cp, 0xc0016900u, packet, 3, 3) != 2 || ctx->GetAlphaToMask() != value ||
			    ctx->IsAlphaToMaskSupportedForDraw() != supported || !unrelated()) return false;
			ctx->SetAlphaToMask(~value);
			indirect_values[1] = value;
			return cp_op_indirect_cx_regs(cp, 0xc0021048u, indirect_body, 4, 4) == 3 && ctx->GetAlphaToMask() == value &&
			       ctx->IsAlphaToMaskSupportedForDraw() == supported && unrelated();
		};
		// All four 2-bit offsets and the round bit remain represented while disabled.
		for (uint32_t fields = 0; fields < 512; ++fields)
		{
			if (!check_write(fields << 8u, true)) return false;
		}
		// Every unqualified bit, including enable, remains visible and blocks draws.
		for (uint32_t bit = 0; bit < 32; ++bit)
		{
			if (bit >= 8 && bit <= 16) continue;
			if (!check_write(0x0000aa00u | (uint32_t {1} << bit), false)) return false;
		}
		if (!check_write(0xffffffffu, false) || !check_write(0x0000aa01u, false) ||
		    !check_write(0x0000aa00u, true)) return false;

		struct InvalidPacket { uint32_t header; uint32_t offset; uint32_t available; };
		constexpr InvalidPacket invalid[] = {
		    {0xc0006900u, Pm4::DB_ALPHA_TO_MASK, 2}, {0xc0026900u, Pm4::DB_ALPHA_TO_MASK, 4},
		    {0xc0016800u, Pm4::DB_ALPHA_TO_MASK, 3}, {0xc0016901u, Pm4::DB_ALPHA_TO_MASK, 3},
		    {0xc0016902u, Pm4::DB_ALPHA_TO_MASK, 3}, {0x80016900u, Pm4::DB_ALPHA_TO_MASK, 3},
		    {0xc0016900u, Pm4::DB_ALPHA_TO_MASK - 1u, 3}, {0xc0016900u, Pm4::DB_ALPHA_TO_MASK + 1u, 3},
		    {0xc0016900u, Pm4::DB_ALPHA_TO_MASK, 0}, {0xc0016900u, Pm4::DB_ALPHA_TO_MASK, 1},
		    {0xc0016900u, Pm4::DB_ALPHA_TO_MASK, 2},
		};
		const uint32_t rejected[] = {0xffffffffu, 0xffffffffu};
		for (const auto& bad: invalid)
		{
			if (hw_ctx_update_draw_control(ctx, bad.header, bad.offset, rejected, bad.available) != 0 ||
			    ctx->GetAlphaToMask() != 0xaa00u || !unrelated()) return false;
		}
		if (hw_ctx_update_draw_control(nullptr, 0xc0016900u, Pm4::DB_ALPHA_TO_MASK, rejected, 3) != 0 ||
		    hw_ctx_update_draw_control(ctx, 0xc0016900u, Pm4::DB_ALPHA_TO_MASK, nullptr, 3) != 0 ||
		    hw_ctx_write_draw_control(ctx, Pm4::DB_ALPHA_TO_MASK + 1u, rejected[0]) ||
		    ctx->GetAlphaToMask() != 0xaa00u || !unrelated()) return false;

		uint32_t stream[] = {
		    0xc0016900u, Pm4::DB_ALPHA_TO_MASK, 0x1ff01u,
		    0xc0016900u, Pm4::CB_TARGET_MASK, 0x13579bdfu,
		    0xc0016900u, Pm4::DB_ALPHA_TO_MASK, 0x1ff00u,
		};
		cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
		if (ctx->GetAlphaToMask() != 0x1ff00u || !ctx->IsAlphaToMaskSupportedForDraw() || !unrelated()) return false;

		ctx->SetAlphaToMask(0xffffffffu);
		uint32_t clear_stream[] = {LUMEN_PM4(2, Pm4::IT_CLEAR_STATE, 0), 0};
		cp->Run(clear_stream, static_cast<uint32_t>(std::size(clear_stream)));
		if (ctx->GetAlphaToMask() != 0 || !ctx->IsAlphaToMaskSupportedForDraw() || independent.GetAlphaToMask() != 0x8000aa01u)
			return false;
		ctx->SetAlphaToMask(0x0000aa01u);
		cp->Reset();
		if (ctx->GetAlphaToMask() != 0 || !ctx->IsAlphaToMaskSupportedForDraw() || independent.GetAlphaToMask() != 0x8000aa01u)
			return false;
		independent.Reset();
		return independent.GetAlphaToMask() == 0 && independent.IsAlphaToMaskSupportedForDraw();
	}();
	*ctx = saved;
	if (!result) std::fprintf(stderr, "alpha-to-mask raw state, decoder, draw admission or reset contract failed\n");
	return result;
}

static bool graphics_run_multi_vgt_param_test(CommandProcessor* cp)
{
	auto* ctx = cp->GetCtx();
	const auto saved = *ctx;
	const bool result = [&]()
	{
		HW::Context independent;
		if (independent.GetMultiVgtParam() != 0xffu || !independent.IsMultiVgtParamSupportedForDraw()) return false;
		independent.SetMultiVgtParam(0x12345678u);
		ctx->SetAlphaToMask(0x1ff00u);
		ctx->SetRenderTargetMask(0x2468ace0u);
		ctx->SetShaderStages(0x13579bdfu);
		auto unrelated = [&]()
		{
			return ctx->GetAlphaToMask() == 0x1ff00u && ctx->IsAlphaToMaskSupportedForDraw() &&
			       ctx->GetRenderTargetMask() == 0x2468ace0u && ctx->GetShaderStages() == 0x13579bdfu &&
			       independent.GetMultiVgtParam() == 0x12345678u && !independent.IsMultiVgtParamSupportedForDraw();
		};
		uint32_t indirect_values[] = {Pm4::IA_MULTI_VGT_PARAM, 0};
		const auto addr = reinterpret_cast<uint64_t>(indirect_values);
		uint32_t indirect_body[] = {1u, static_cast<uint32_t>(addr), static_cast<uint32_t>(addr >> 32u)};
		constexpr uint32_t values[] = {0u, 1u, 0xfeu, 0x100u, 0x100ffu, 0x200ffu, 0x400ffu, 0x800ffu, 0x1000ffu,
		                               0x800000ffu, 0xffffffffu, 0xffu};
		for (const auto value: values)
		{
			const bool supported = value == 0xffu;
			uint32_t packet[] = {Pm4::IA_MULTI_VGT_PARAM, value};
			if (cp_op_set_context_reg(cp, 0xc0016900u, packet, 3, 3) != 2 || ctx->GetMultiVgtParam() != value ||
			    ctx->IsMultiVgtParamSupportedForDraw() != supported || !unrelated()) return false;
			ctx->SetMultiVgtParam(~value);
			indirect_values[1] = value;
			if (cp_op_indirect_cx_regs(cp, 0xc0021048u, indirect_body, 4, 4) != 3 || ctx->GetMultiVgtParam() != value ||
			    ctx->IsMultiVgtParamSupportedForDraw() != supported || !unrelated()) return false;
		}
		const uint32_t rejected[] = {0xdeadbeefu, 0x87654321u};
		constexpr uint32_t bad_headers[] = {0xc0006900u, 0xc0026900u, 0xc0016800u, 0xc0016901u, 0xc0016902u, 0x80016900u};
		for (const auto header: bad_headers)
		{
			if (hw_ctx_update_draw_control(ctx, header, Pm4::IA_MULTI_VGT_PARAM, rejected, 4) != 0 ||
			    ctx->GetMultiVgtParam() != 0xffu || !unrelated()) return false;
		}
		for (uint32_t dw = 0; dw < 3; ++dw)
		{
			if (hw_ctx_update_draw_control(ctx, 0xc0016900u, Pm4::IA_MULTI_VGT_PARAM, rejected, dw) != 0 ||
			    ctx->GetMultiVgtParam() != 0xffu || !unrelated()) return false;
		}
		for (const auto offset: {Pm4::IA_MULTI_VGT_PARAM - 1u, Pm4::IA_MULTI_VGT_PARAM + 1u})
		{
			if (hw_ctx_update_draw_control(ctx, 0xc0016900u, offset, rejected, 3) != 0 ||
			    hw_ctx_write_draw_control(ctx, offset, rejected[0]) || ctx->GetMultiVgtParam() != 0xffu || !unrelated()) return false;
		}
		if (hw_ctx_update_draw_control(nullptr, 0xc0016900u, Pm4::IA_MULTI_VGT_PARAM, rejected, 3) != 0 ||
		    hw_ctx_update_draw_control(ctx, 0xc0016900u, Pm4::IA_MULTI_VGT_PARAM, nullptr, 3) != 0 || !unrelated()) return false;
		uint32_t stream[] = {
		    0xc0016900u, Pm4::IA_MULTI_VGT_PARAM, 0x100ffu,
		    0xc0016900u, Pm4::CB_TARGET_MASK, 0x2468ace0u,
		    0xc0016900u, Pm4::IA_MULTI_VGT_PARAM, 0xffu,
		};
		cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
		if (ctx->GetMultiVgtParam() != 0xffu || !ctx->IsMultiVgtParamSupportedForDraw() || !unrelated()) return false;
		ctx->SetMultiVgtParam(0xffffffffu);
		uint32_t clear[] = {LUMEN_PM4(2, Pm4::IT_CLEAR_STATE, 0), 0};
		cp->Run(clear, static_cast<uint32_t>(std::size(clear)));
		if (ctx->GetMultiVgtParam() != 0xffu || !ctx->IsMultiVgtParamSupportedForDraw()) return false;
		ctx->SetMultiVgtParam(0x100ffu);
		cp->Reset();
		if (ctx->GetMultiVgtParam() != 0xffu || !ctx->IsMultiVgtParamSupportedForDraw() ||
		    independent.GetMultiVgtParam() != 0x12345678u) return false;
		independent.Reset();
		return independent.GetMultiVgtParam() == 0xffu && independent.IsMultiVgtParamSupportedForDraw();
	}();
	*ctx = saved;
	if (!result) std::fprintf(stderr, "multi-VGT raw state, decoder, draw admission or reset contract failed\n");
	return result;
}

static bool graphics_run_scan_mode_control_1_test(CommandProcessor* cp)
{
	auto* ctx = cp->GetCtx();
	const auto saved = *ctx;
	const bool result = [&]()
	{
		HW::Context independent;
		if (independent.GetScanModeControl1() != 0 || !independent.IsScanModeControl1SupportedForDraw()) return false;
		independent.SetScanModeControl1(0x80000000u);
		ctx->SetRenderTargetMask(0x13579bdfu);
		ctx->SetShaderStages(0x2468ace0u);
		ctx->SetAlphaToMask(0x1ff00u);
		ctx->SetMultiVgtParam(0xffu);
		HW::ScanModeControl scan_mode {};
		scan_mode.msaa_enable = true;
		scan_mode.vport_scissor_enable = true;
		ctx->SetScanModeControl(scan_mode);
		auto unrelated = [&]()
		{
			const auto& scan = ctx->GetScanModeControl();
			return ctx->GetRenderTargetMask() == 0x13579bdfu && ctx->GetShaderStages() == 0x2468ace0u &&
			       ctx->GetAlphaToMask() == 0x1ff00u && ctx->GetMultiVgtParam() == 0xffu &&
			       scan.msaa_enable && scan.vport_scissor_enable && !scan.line_stipple_enable &&
			       independent.GetScanModeControl1() == 0x80000000u && !independent.IsScanModeControl1SupportedForDraw();
		};
		uint32_t indirect_values[] = {Pm4::PA_SC_MODE_CNTL_1, 0};
		const auto addr = reinterpret_cast<uint64_t>(indirect_values);
		uint32_t indirect_body[] = {1u, static_cast<uint32_t>(addr), static_cast<uint32_t>(addr >> 32u)};
		auto check_write = [&](uint32_t value, bool supported)
		{
			uint32_t packet[] = {Pm4::PA_SC_MODE_CNTL_1, value};
			if (cp_op_set_context_reg(cp, 0xc0016900u, packet, 3, 3) != 2 || ctx->GetScanModeControl1() != value ||
			    ctx->IsScanModeControl1SupportedForDraw() != supported || !unrelated()) return false;
			ctx->SetScanModeControl1(~value);
			indirect_values[1] = value;
			return cp_op_indirect_cx_regs(cp, 0xc0021048u, indirect_body, 4, 4) == 3 && ctx->GetScanModeControl1() == value &&
			       ctx->IsScanModeControl1SupportedForDraw() == supported && unrelated();
		};
		// Every combination of the three qualified scheduling controls is admitted.
		constexpr uint32_t supported_values[] = {
		    0u, 0x00020000u, 0x02000000u, 0x04000000u, 0x02020000u, 0x04020000u, 0x06000000u, 0x06020000u,
		};
		for (const auto value: supported_values)
		{
			if (!check_write(value, true)) return false;
		}
		// Preserve all single bits, including sample shading, primitive reordering
		// and unnamed bits, and reject every unqualified control on the draw path.
		for (uint32_t bit = 0; bit < 32; ++bit)
		{
			const bool supported = bit == 17 || bit == 25 || bit == 26;
			const uint32_t value = uint32_t {1} << bit;
			if (!check_write(value, supported) || (!supported && !check_write(0x06020000u | value, false))) return false;
		}
		if (!check_write(0xffffffffu, false) || !check_write(0u, true) || !check_write(0x06020000u, true)) return false;

		const uint32_t rejected[] = {0xdeadbeefu, 0x87654321u};
		constexpr uint32_t bad_headers[] = {0xc0006900u, 0xc0026900u, 0xc0016800u, 0xc0016901u, 0xc0016902u, 0x80016900u};
		for (const auto header: bad_headers)
		{
			if (hw_ctx_update_draw_control(ctx, header, Pm4::PA_SC_MODE_CNTL_1, rejected, 4) != 0 ||
			    ctx->GetScanModeControl1() != 0x06020000u || !unrelated()) return false;
		}
		for (uint32_t dw = 0; dw < 3; ++dw)
		{
			if (hw_ctx_update_draw_control(ctx, 0xc0016900u, Pm4::PA_SC_MODE_CNTL_1, rejected, dw) != 0 ||
			    ctx->GetScanModeControl1() != 0x06020000u || !unrelated()) return false;
		}
		for (const auto offset: {Pm4::PA_SC_MODE_CNTL_1 - 1u, Pm4::PA_SC_MODE_CNTL_1 + 1u})
		{
			if (hw_ctx_update_draw_control(ctx, 0xc0016900u, offset, rejected, 3) != 0 ||
			    hw_ctx_write_draw_control(ctx, offset, rejected[0]) || ctx->GetScanModeControl1() != 0x06020000u || !unrelated())
				return false;
		}
		if (hw_ctx_update_draw_control(nullptr, 0xc0016900u, Pm4::PA_SC_MODE_CNTL_1, rejected, 3) != 0 ||
		    hw_ctx_update_draw_control(ctx, 0xc0016900u, Pm4::PA_SC_MODE_CNTL_1, nullptr, 3) != 0 ||
		    ctx->GetScanModeControl1() != 0x06020000u || !unrelated()) return false;

		uint32_t stream[] = {
		    0xc0016900u, Pm4::PA_SC_MODE_CNTL_1, 0xffffffffu,
		    0xc0016900u, Pm4::CB_TARGET_MASK, 0x13579bdfu,
		    0xc0016900u, Pm4::PA_SC_MODE_CNTL_1, 0x06020000u,
		};
		cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
		if (ctx->GetScanModeControl1() != 0x06020000u || !ctx->IsScanModeControl1SupportedForDraw() || !unrelated()) return false;
		uint32_t indirect_stream[] = {
		    Pm4::PA_SC_MODE_CNTL_1, 0xffffffffu, Pm4::CB_TARGET_MASK, 0x13579bdfu, Pm4::PA_SC_MODE_CNTL_1, 0u,
		};
		const auto stream_addr = reinterpret_cast<uint64_t>(indirect_stream);
		uint32_t stream_body[] = {3u, static_cast<uint32_t>(stream_addr), static_cast<uint32_t>(stream_addr >> 32u)};
		if (cp_op_indirect_cx_regs(cp, 0xc0021048u, stream_body, 4, 4) != 3 || ctx->GetScanModeControl1() != 0 ||
		    !ctx->IsScanModeControl1SupportedForDraw() || !unrelated()) return false;

		ctx->SetScanModeControl1(0xffffffffu);
		uint32_t clear[] = {LUMEN_PM4(2, Pm4::IT_CLEAR_STATE, 0), 0};
		cp->Run(clear, static_cast<uint32_t>(std::size(clear)));
		if (ctx->GetScanModeControl1() != 0 || !ctx->IsScanModeControl1SupportedForDraw() ||
		    independent.GetScanModeControl1() != 0x80000000u) return false;
		ctx->SetScanModeControl1(0x06020000u);
		cp->Reset();
		if (ctx->GetScanModeControl1() != 0 || !ctx->IsScanModeControl1SupportedForDraw() ||
		    independent.GetScanModeControl1() != 0x80000000u) return false;
		independent.Reset();
		return independent.GetScanModeControl1() == 0 && independent.IsScanModeControl1SupportedForDraw();
	}();
	*ctx = saved;
	if (!result) std::fprintf(stderr, "scan-mode control 1 raw state, decoder, draw admission or reset contract failed\n");
	return result;
}

static bool graphics_run_aa_mask_test(CommandProcessor* cp)
{
	auto* ctx = cp->GetCtx();
	const auto saved = *ctx;
	const bool result = [&]()
	{
		HW::Context independent;
		if (independent.GetAaMask(0) != 0xffffffffu || independent.GetAaMask(1) != 0xffffffffu ||
		    !independent.IsAaMaskSupportedForDraw()) return false;
		independent.SetAaMask(0, 0x12345678u);
		independent.SetAaMask(1, 0x87654321u);
		ctx->SetRenderTargetMask(0x13579bdfu);
		ctx->SetShaderStages(0x2468ace0u);
		ctx->SetAlphaToMask(0x1ff00u);
		ctx->SetMultiVgtParam(0xffu);
		ctx->SetScanModeControl1(0x06020000u);
		HW::AaConfig aa_config {};
		aa_config.msaa_num_samples = 3;
		ctx->SetAaConfig(aa_config);
		auto unrelated = [&]()
		{
			return ctx->GetRenderTargetMask() == 0x13579bdfu && ctx->GetShaderStages() == 0x2468ace0u &&
			       ctx->GetAlphaToMask() == 0x1ff00u && ctx->GetMultiVgtParam() == 0xffu &&
			       ctx->GetScanModeControl1() == 0x06020000u && ctx->GetAaConfig().msaa_num_samples == 3 &&
			       independent.GetAaMask(0) == 0x12345678u && independent.GetAaMask(1) == 0x87654321u &&
			       !independent.IsAaMaskSupportedForDraw();
		};
		auto check_state = [&](uint32_t first, uint32_t second)
		{
			return ctx->GetAaMask(0) == first && ctx->GetAaMask(1) == second &&
			       ctx->IsAaMaskSupportedForDraw() == (first == 0xffffffffu && second == 0xffffffffu) && unrelated();
		};
		auto check_single = [&](uint32_t index, uint32_t value, uint32_t other)
		{
			const uint32_t offset = Pm4::PA_SC_AA_MASK_X0Y0_X1Y0 + index;
			ctx->SetAaMask(index, ~value);
			ctx->SetAaMask(1u - index, other);
			uint32_t packet[] = {offset, value};
			if (cp_op_set_context_reg(cp, 0xc0016900u, packet, 3, 3) != 2 ||
			    !check_state(index == 0 ? value : other, index == 1 ? value : other)) return false;
			ctx->SetAaMask(index, ~value);
			const auto addr = reinterpret_cast<uint64_t>(packet);
			uint32_t body[] = {1u, static_cast<uint32_t>(addr), static_cast<uint32_t>(addr >> 32u)};
			return cp_op_indirect_cx_regs(cp, 0xc0021048u, body, 4, 4) == 3 &&
			       check_state(index == 0 ? value : other, index == 1 ? value : other);
		};
		for (uint32_t index = 0; index < 2; ++index)
		{
			for (uint32_t bit = 0; bit < 32; ++bit)
			{
				if (!check_single(index, ~(uint32_t {1} << bit), 0xffffffffu)) return false;
			}
			for (const uint32_t value: {0u, 0x00010001u, 0xaaaaaaaau, 0x55555555u, 0xffffffffu})
			{
				if (!check_single(index, value, 0xffffffffu) || !check_single(index, value, 0x89abcdefu)) return false;
			}
		}
		uint32_t pair[] = {Pm4::PA_SC_AA_MASK_X0Y0_X1Y0, 0x12345678u, 0x87654321u};
		if (cp_op_set_context_reg(cp, 0xc0026900u, pair, 4, 4) != 3 || !check_state(pair[1], pair[2])) return false;
		uint32_t indirect_pair[] = {Pm4::PA_SC_AA_MASK_X0Y1_X1Y1, 0xffffffffu, Pm4::PA_SC_AA_MASK_X0Y0_X1Y0, 0xffffffffu};
		const auto pair_addr = reinterpret_cast<uint64_t>(indirect_pair);
		uint32_t pair_body[] = {2u, static_cast<uint32_t>(pair_addr), static_cast<uint32_t>(pair_addr >> 32u)};
		if (cp_op_indirect_cx_regs(cp, 0xc0021048u, pair_body, 4, 4) != 3 || !check_state(0xffffffffu, 0xffffffffu)) return false;

		ctx->SetAaMask(0, 0x12345678u);
		ctx->SetAaMask(1, 0x87654321u);
		struct InvalidPacket { uint32_t header; uint32_t offset; uint32_t available; };
		constexpr InvalidPacket invalid[] = {
		    {0xc0006900u, Pm4::PA_SC_AA_MASK_X0Y0_X1Y0, 2},
		    {0xc0036900u, Pm4::PA_SC_AA_MASK_X0Y0_X1Y0, 5},
		    {0xc0026900u, Pm4::PA_SC_AA_MASK_X0Y1_X1Y1, 4},
		    {0xc0016800u, Pm4::PA_SC_AA_MASK_X0Y0_X1Y0, 3},
		    {0xc0026800u, Pm4::PA_SC_AA_MASK_X0Y0_X1Y0, 4},
		    {0xc0016901u, Pm4::PA_SC_AA_MASK_X0Y0_X1Y0, 3},
		    {0xc0026902u, Pm4::PA_SC_AA_MASK_X0Y0_X1Y0, 4},
		    {0x80026900u, Pm4::PA_SC_AA_MASK_X0Y0_X1Y0, 4},
		    {0xc0026900u, Pm4::PA_SC_AA_MASK_X0Y0_X1Y0 - 1u, 4},
		    {0xc0016900u, Pm4::PA_SC_AA_MASK_X0Y1_X1Y1 + 1u, 3},
		};
		const uint32_t rejected[] = {0xffffffffu, 0xffffffffu, 0xffffffffu};
		for (const auto& bad: invalid)
		{
			if (hw_ctx_update_aa_mask(ctx, bad.header, bad.offset, rejected, bad.available) != 0 ||
			    !check_state(0x12345678u, 0x87654321u)) return false;
		}
		for (const uint32_t packet_dw: {3u, 4u})
		{
			for (uint32_t available = 0; available < packet_dw; ++available)
			{
				if (hw_ctx_update_aa_mask(ctx, LUMEN_PM4(packet_dw, Pm4::IT_SET_CONTEXT_REG, 0),
				                          Pm4::PA_SC_AA_MASK_X0Y0_X1Y0, rejected, available) != 0 ||
				    !check_state(0x12345678u, 0x87654321u)) return false;
			}
		}
		if (hw_ctx_update_aa_mask(nullptr, 0xc0026900u, Pm4::PA_SC_AA_MASK_X0Y0_X1Y0, rejected, 4) != 0 ||
		    hw_ctx_update_aa_mask(ctx, 0xc0026900u, Pm4::PA_SC_AA_MASK_X0Y0_X1Y0, nullptr, 4) != 0 ||
		    hw_ctx_write_aa_mask(ctx, Pm4::PA_SC_AA_MASK_X0Y0_X1Y0 - 1u, 0) ||
		    hw_ctx_write_aa_mask(ctx, Pm4::PA_SC_AA_MASK_X0Y1_X1Y1 + 1u, 0) ||
		    !check_state(0x12345678u, 0x87654321u)) return false;

		uint32_t stream[] = {
		    0xc0026900u, Pm4::PA_SC_AA_MASK_X0Y0_X1Y0, 0u, 0x00010001u,
		    0xc0016900u, Pm4::CB_TARGET_MASK, 0x13579bdfu,
		    0xc0016900u, Pm4::PA_SC_AA_MASK_X0Y1_X1Y1, 0xffffffffu,
		    0xc0016900u, Pm4::PA_SC_AA_MASK_X0Y0_X1Y0, 0xffffffffu,
		};
		cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
		if (!check_state(0xffffffffu, 0xffffffffu)) return false;
		ctx->SetAaMask(0, 0u);
		ctx->SetAaMask(1, 0x00010001u);
		uint32_t clear[] = {LUMEN_PM4(2, Pm4::IT_CLEAR_STATE, 0), 0};
		cp->Run(clear, static_cast<uint32_t>(std::size(clear)));
		if (ctx->GetAaMask(0) != 0xffffffffu || ctx->GetAaMask(1) != 0xffffffffu || !ctx->IsAaMaskSupportedForDraw()) return false;
		ctx->SetAaMask(0, 0x55555555u);
		ctx->SetAaMask(1, 0xaaaaaaaau);
		cp->Reset();
		if (ctx->GetAaMask(0) != 0xffffffffu || ctx->GetAaMask(1) != 0xffffffffu || !ctx->IsAaMaskSupportedForDraw() ||
		    independent.GetAaMask(0) != 0x12345678u || independent.GetAaMask(1) != 0x87654321u) return false;
		independent.Reset();
		return independent.GetAaMask(0) == 0xffffffffu && independent.GetAaMask(1) == 0xffffffffu &&
		       independent.IsAaMaskSupportedForDraw();
	}();
	*ctx = saved;
	if (!result) std::fprintf(stderr, "AA-mask raw state, decoder, draw admission or reset contract failed\n");
	return result;
}

static bool graphics_run_depth_control_raw_state_test(CommandProcessor* cp)
{
	auto* ctx = cp->GetCtx();
	const HW::Context saved = *ctx;
	const bool result = [&]()
	{
		// Independent literal complement of the ten fields retained by the
		// existing decoder: bits 0..10, 20..22, 30 and 31 are modeled.
		constexpr uint32_t omitted = 0x3f8ff800u;
		const auto matches = [&](uint32_t value)
		{
			const auto& d = ctx->GetDepthControl();
			return d.raw == value && d.unsupported_bits == (value & omitted) &&
			       d.stencil_enable == ((value & 0x1u) != 0) &&
			       d.z_enable == ((value & 0x2u) != 0) && d.z_write_enable == ((value & 0x4u) != 0) &&
			       d.depth_bounds_enable == ((value & 0x8u) != 0) && d.zfunc == ((value >> 4u) & 7u) &&
			       d.backface_enable == ((value & 0x80u) != 0) && d.stencilfunc == ((value >> 8u) & 7u) &&
			       d.stencilfunc_bf == ((value >> 20u) & 7u) &&
			       d.color_writes_on_depth_fail_enable == ((value & 0x40000000u) != 0) &&
			       d.color_writes_on_depth_pass_disable == ((value & 0x80000000u) != 0);
		};
		ctx->SetRenderTargetMask(0x13579bdfu);
		const auto direct = [&](uint32_t value)
		{
			// The following word is outside the one-register packet. It must
			// neither replace the payload nor consume the next packet's state.
			uint32_t packet[] = {Pm4::DB_DEPTH_CONTROL, value, 0xa5a5a5a5u};
			return cp_op_set_context_reg(cp, 0xc0016900u, packet, 3, 3) == 2 && matches(value) &&
			       ctx->GetRenderTargetMask() == 0x13579bdfu;
		};
		const auto indirect = [&](uint32_t value)
		{
			uint32_t pairs[] = {Pm4::DB_DEPTH_CONTROL, value, Pm4::CB_TARGET_MASK, 0xa5a5a5a5u};
			const auto address = reinterpret_cast<uint64_t>(pairs);
			uint32_t body[] = {1u, static_cast<uint32_t>(address), static_cast<uint32_t>(address >> 32u)};
			return cp_op_indirect_cx_regs(cp, 0xc0021048u, body, 4, 4) == 3 && matches(value) &&
			       ctx->GetRenderTargetMask() == 0x13579bdfu;
		};
		for (uint32_t bit = 0; bit < 32; ++bit)
		{
			for (const uint32_t value: {uint32_t {1} << bit, ~(uint32_t {1} << bit), 0u, 0xffffffffu})
			{
				if (!direct(~value) || !direct(value) || !direct(~value) || !indirect(value)) return false;
			}
		}
		if (!direct(0u) || !indirect(0xffffffffu) || !indirect(0u)) return false;

		HW::Context independent;
		if (independent.GetDepthControl().raw != 0 || independent.GetDepthControl().unsupported_bits != 0) return false;
		independent.SetDepthControl(ctx->GetDepthControl());
		if (!direct(0xffffffffu)) return false;
		independent.SetDepthControl(ctx->GetDepthControl());
		independent.Reset();
		if (independent.GetDepthControl().raw != 0 || independent.GetDepthControl().unsupported_bits != 0 ||
		    !matches(0xffffffffu)) return false;

		uint32_t clear[] = {LUMEN_PM4(2, Pm4::IT_CLEAR_STATE, 0), 0};
		cp->Run(clear, static_cast<uint32_t>(std::size(clear)));
		if (!matches(0u)) return false;
		ctx->SetRenderTargetMask(0x13579bdfu);
		if (!direct(0xffffffffu)) return false;
		cp->Reset();
		return matches(0u);
	}();
	*ctx = saved;
	if (!result)
	{
		std::fprintf(stderr, "depth-control raw, omitted bits, decoded fields or overwrite/reset contract failed\n");
		return false;
	}
	std::fprintf(stderr, "[GFX_DEPTH_CONTROL_RAW_RETENTION_TEST] direct/indirect=checked single-bits=32"
	                     " complements=checked payload-sentinel=checked overwrite-clears=checked"
	                     " context/CP/clear-state-reset=checked active-decoding=unchanged\n");
	return true;
}

static bool graphics_run_htile_preload_state_test(CommandProcessor* cp)
{
	struct StateCase
	{
		uint32_t raw;
		uint32_t preload;
		bool     tile_surface_enable;
		uint64_t base;
		bool     supported;
	};
	constexpr StateCase cases[] = {
	    {0x00000008u, 1u, false, 0u, true},
	    {0x00000008u, 1u, true, 0u, true},
	    {0x80000008u, 1u, true, 0x100u, true},
	    {0x00000000u, 0u, false, 0u, true},
	    {0x80000000u, 0u, false, 0u, false},
	    {0x00000000u, 0u, true, 0u, false},
	    {0x00000000u, 0u, false, 0x100u, false},
	    {0x00000000u, 0u, true, 0x100u, false},
	    {0x00000000u, 2u, false, 0u, false},
	};
	for (const auto& test: cases)
	{
		HW::DepthRenderTarget z {};
		z.htile_surface.raw          = test.raw;
		z.htile_surface.preload      = test.preload;
		z.z_info.tile_surface_enable = test.tile_surface_enable;
		z.htile_data_base_addr       = test.base;
		if (z.IsHtilePreloadStateSupported() != test.supported)
		{
			std::fprintf(stderr, "HTILE preload support matrix failed: raw=%08x preload=%u enabled=%u base=%" PRIx64 "\n",
			             test.raw, test.preload, test.tile_surface_enable ? 1u : 0u, test.base);
			return false;
		}
	}

	HW::DepthRenderTarget transition {};
	transition.z_info.tile_surface_enable = true;
	transition.htile_data_base_addr       = 0x100u;
	if (transition.IsHtilePreloadStateSupported())
	{
		std::fprintf(stderr, "active zero-preload HTILE state was accepted\n");
		return false;
	}
	transition.z_info.tile_surface_enable = false;
	if (transition.IsHtilePreloadStateSupported())
	{
		std::fprintf(stderr, "zero-preload HTILE state retained a metadata base\n");
		return false;
	}
	transition.htile_data_base_addr = 0;
	if (!transition.IsHtilePreloadStateSupported())
	{
		std::fprintf(stderr, "exact inert HTILE state was rejected\n");
		return false;
	}
	transition.z_info.tile_surface_enable = true;
	if (transition.IsHtilePreloadStateSupported())
	{
		std::fprintf(stderr, "reenabled zero-preload HTILE state was accepted\n");
		return false;
	}

	uint32_t composite[22] = {};
	composite[0] = (3u << Pm4::DB_Z_INFO_FORMAT_SHIFT) | (1u << Pm4::DB_Z_INFO_ZRANGE_PRECISION_SHIFT);
	composite[8]  = 0xc0016900u;
	composite[9]  = Pm4::DB_DEPTH_INFO;
	composite[11] = 0xc0016900u;
	composite[12] = Pm4::DB_DEPTH_VIEW;
	composite[14] = 0xc0016900u;
	composite[15] = Pm4::DB_HTILE_DATA_BASE;
	composite[17] = 0xc0016900u;
	composite[18] = Pm4::DB_HTILE_SURFACE;
	composite[20] = 0xc0001000u;
	composite[21] = (1080u << 16u) | 1920u;
	auto* ctx = cp->GetCtx();
	ctx->SetRenderTargetMask(0x13579bdfu);
	composite[19] = 0x80000008u;
	if (hw_ctx_set_depth_render_target(cp, 0xc0086900u, Pm4::DB_Z_INFO, composite, 22) != 22 ||
	    ctx->GetDepthRenderTarget().htile_surface.raw != 0x80000008u ||
	    ctx->GetDepthRenderTarget().htile_surface.preload != 1u ||
	    !ctx->GetDepthRenderTarget().IsHtilePreloadStateSupported() || ctx->GetRenderTargetMask() != 0x13579bdfu)
	{
		std::fprintf(stderr, "legacy HTILE surface raw state was not preserved\n");
		return false;
	}
	composite[19] = 0;
	if (hw_ctx_set_depth_render_target(cp, 0xc0086900u, Pm4::DB_Z_INFO, composite, 22) != 22 ||
	    ctx->GetDepthRenderTarget().htile_surface.raw != 0u || ctx->GetDepthRenderTarget().htile_surface.preload != 0u ||
	    ctx->GetDepthRenderTarget().z_info.tile_surface_enable || ctx->GetDepthRenderTarget().htile_data_base_addr != 0u ||
	    !ctx->GetDepthRenderTarget().IsHtilePreloadStateSupported() || ctx->GetRenderTargetMask() != 0x13579bdfu)
	{
		std::fprintf(stderr, "inert HTILE surface raw state was not preserved\n");
		return false;
	}

	const bool depth_raw_retained = [&]()
	{
		// Fixed raw words exercise actual register writes. Their independently
		// enumerated complements include every bit the current context omits.
		constexpr uint32_t z_word = 0xa0000003u;
		constexpr uint32_t stencil_word = 0x20000001u;
		constexpr uint32_t info_word = 0x001d0c40u;
		constexpr uint32_t size_word = 127u | (79u << 11u);
		constexpr uint32_t slice_word = 10239u;
		composite[0] = z_word;
		composite[1] = stencil_word;
		composite[6] = size_word;
		composite[7] = slice_word;
		composite[10] = info_word;
		const auto check_composite = [&]()
		{
			if (hw_ctx_set_depth_render_target(cp, 0xc0086900u, Pm4::DB_Z_INFO, composite, 22) != 22) return false;
			const auto& z = ctx->GetDepthRenderTarget();
			return z.z_info.unsupported_bits == (composite[0] & 0x5000e7f0u) &&
			       z.stencil_info.unsupported_bits == (composite[1] & 0xd78f07feu) &&
			       z.depth_info.unsupported_bits == (composite[10] & 0xffe00000u) &&
			       z.depth_size_unsupported_bits == (composite[6] & 0xffc00000u) &&
			       z.depth_slice_unsupported_bits == (composite[7] & 0xffc00000u) &&
			       z.z_info.format == 3 && z.stencil_info.format == 1 &&
			       ctx->GetRenderTargetMask() == 0x13579bdfu;
		};
		for (uint32_t bit = 0; bit < 32; ++bit)
		{
			const uint32_t value = uint32_t {1} << bit;
			composite[0] = z_word | value;
			composite[1] = stencil_word | value;
			composite[6] = size_word | value;
			composite[7] = slice_word | value;
			composite[10] = info_word | value;
			if (!check_composite()) return false;
			composite[0] = z_word;
			composite[1] = stencil_word;
			composite[6] = size_word;
			composite[7] = slice_word;
			composite[10] = info_word;
			if (!check_composite()) return false;
		}

		for (const bool stencil: {false, true})
		{
			const uint32_t reg = stencil ? Pm4::DB_STENCIL_INFO : Pm4::DB_Z_INFO;
			const uint32_t raw = stencil ? stencil_word : z_word;
			const uint32_t omitted = stencil ? 0xd78f07feu : 0x5000e7f0u;
			const auto check_single = [&](uint32_t value)
			{
				const auto& z = ctx->GetDepthRenderTarget();
				return (stencil ? z.stencil_info.unsupported_bits : z.z_info.unsupported_bits) == (value & omitted) &&
				       z.z_info.format == 3 && z.stencil_info.format == 1 &&
				       (stencil ? z.z_info.unsupported_bits : z.stencil_info.unsupported_bits) == 0 &&
				       z.depth_info.unsupported_bits == 0 && z.depth_size_unsupported_bits == 0 &&
				       z.depth_slice_unsupported_bits == 0 && ctx->GetRenderTargetMask() == 0x13579bdfu;
			};
			for (uint32_t bit = 0; bit < 32; ++bit)
			{
				for (const uint32_t value: {raw | (uint32_t {1} << bit), raw})
				{
					// The last word is outside the single-register packet. A decoder
					// must consume its payload, regardless of the following word.
					uint32_t packet[] = {reg, value, 0x00000002u};
					if (cp_op_set_context_reg(cp, 0xc0016900u, packet, 3, 3) != 2 || !check_single(value)) return false;
					// Force the opposite evidence before the indirect overwrite.
					packet[1] = raw | (~value & omitted);
					if (cp_op_set_context_reg(cp, 0xc0016900u, packet, 3, 3) != 2) return false;
					packet[1] = value;
					const auto addr = reinterpret_cast<uint64_t>(packet);
					uint32_t body[] = {1u, static_cast<uint32_t>(addr), static_cast<uint32_t>(addr >> 32u)};
					if (cp_op_indirect_cx_regs(cp, 0xc0021048u, body, 4, 4) != 3 || !check_single(value)) return false;
				}
			}
		}
		return true;
	}();
	if (!depth_raw_retained)
	{
		std::fprintf(stderr, "depth raw omitted bits, register payload or overwrite transitions were not preserved\n");
		return false;
	}
	std::fprintf(stderr, "[GFX_DEPTH_RAW_RETENTION_TEST] composite-five-words=checked direct-z/stencil=checked"
	                     " indirect-z/stencil=checked single-bits=32 payload-sentinel=checked overwrite-clears=checked\n");

	cp->Reset();
	const auto& reset_depth = ctx->GetDepthRenderTarget();
	if (!reset_depth.IsHtilePreloadStateSupported() || reset_depth.htile_surface.raw != 0u ||
	    reset_depth.z_info.unsupported_bits != 0 || reset_depth.stencil_info.unsupported_bits != 0 ||
	    reset_depth.depth_info.unsupported_bits != 0 || reset_depth.depth_size_unsupported_bits != 0 ||
	    reset_depth.depth_slice_unsupported_bits != 0)
	{
		std::fprintf(stderr, "HTILE preload reset did not restore the inert local state\n");
		return false;
	}
	return true;
}

static bool graphics_run_clip_control_test(CommandProcessor* cp)
{
	auto only_dx_clip_space = [](const HW::ClipControl& c, bool expected)
	{
		return c.dx_clip_space == expected && c.user_clip_planes == 0 && c.user_clip_plane_mode == 0 && !c.vertex_kill_any &&
		       !c.min_z_clip_disable && !c.max_z_clip_disable && !c.user_clip_plane_negate_y && !c.clip_disable &&
		       !c.user_clip_plane_cull_only && !c.cull_on_clipping_error_disable && !c.linear_attribute_clip_enable &&
		       !c.force_viewport_index_from_vs_enable;
	};

	for (const bool dx_clip_space: {false, true})
	{
		HW::ClipControl c {};
		c.dx_clip_space = dx_clip_space;
		if (!c.IsSupportedForDraw())
		{
			std::fprintf(stderr, "native clip-space state was rejected: dx=%u\n", dx_clip_space ? 1u : 0u);
			return false;
		}
		c.user_clip_planes = 1;
		if (c.IsSupportedForDraw()) return false;
		c = {};
		c.dx_clip_space        = dx_clip_space;
		c.user_clip_plane_mode = 1;
		if (c.IsSupportedForDraw()) return false;
		c = {};
		c.dx_clip_space   = dx_clip_space;
		c.vertex_kill_any = true;
		if (c.IsSupportedForDraw()) return false;
		c = {};
		c.dx_clip_space      = dx_clip_space;
		c.min_z_clip_disable = true;
		if (c.IsSupportedForDraw()) return false;
		c = {};
		c.dx_clip_space      = dx_clip_space;
		c.max_z_clip_disable = true;
		if (c.IsSupportedForDraw()) return false;
		c = {};
		c.dx_clip_space            = dx_clip_space;
		c.user_clip_plane_negate_y = true;
		if (c.IsSupportedForDraw()) return false;
		c = {};
		c.dx_clip_space = dx_clip_space;
		c.clip_disable  = true;
		if (c.IsSupportedForDraw()) return false;
		c = {};
		c.dx_clip_space             = dx_clip_space;
		c.user_clip_plane_cull_only = true;
		if (c.IsSupportedForDraw()) return false;
		c = {};
		c.dx_clip_space                  = dx_clip_space;
		c.cull_on_clipping_error_disable = true;
		if (c.IsSupportedForDraw()) return false;
		c = {};
		c.dx_clip_space                = dx_clip_space;
		c.linear_attribute_clip_enable = true;
		if (c.IsSupportedForDraw()) return false;
		c = {};
		c.dx_clip_space                       = dx_clip_space;
		c.force_viewport_index_from_vs_enable = true;
		if (c.IsSupportedForDraw()) return false;
	}

	constexpr uint32_t dx_word = 1u << Pm4::PA_CL_CLIP_CNTL_DX_CLIP_SPACE_DEF_SHIFT;
	auto*              ctx     = cp->GetCtx();
	ctx->SetRenderTargetMask(0x13579bdfu);
	uint32_t direct[] = {Pm4::PA_CL_CLIP_CNTL, dx_word};
	if (cp_op_set_context_reg(cp, LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), direct, 3, 3) != 2 ||
	    !only_dx_clip_space(ctx->GetClipControl(), true) || !ctx->GetClipControl().IsSupportedForDraw() ||
	    ctx->GetRenderTargetMask() != 0x13579bdfu)
	{
		std::fprintf(stderr, "direct DX clip-space write failed\n");
		return false;
	}
	direct[1] = 0;
	if (cp_op_set_context_reg(cp, LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), direct, 3, 3) != 2 ||
	    !only_dx_clip_space(ctx->GetClipControl(), false) || !ctx->GetClipControl().IsSupportedForDraw())
	{
		std::fprintf(stderr, "direct legacy clip-space transition failed\n");
		return false;
	}

	ctx->SetClipControl(decode_clip_control(dx_word));
	struct InvalidPacket
	{
		uint32_t header;
		uint32_t offset;
		uint32_t available;
	};
	constexpr InvalidPacket invalid[] = {
	    {LUMEN_PM4(2, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::PA_CL_CLIP_CNTL, 2},
	    {LUMEN_PM4(4, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::PA_CL_CLIP_CNTL, 4},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONFIG_REG, 0), Pm4::PA_CL_CLIP_CNTL, 3},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0) | 1u, Pm4::PA_CL_CLIP_CNTL, 3},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::PA_CL_CLIP_CNTL - 1u, 3},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::PA_CL_CLIP_CNTL + 1u, 3},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::PA_CL_CLIP_CNTL, 0},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::PA_CL_CLIP_CNTL, 1},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::PA_CL_CLIP_CNTL, 2},
	};
	const uint32_t rejected_value = 0;
	for (const auto& packet: invalid)
	{
		if (hw_ctx_update_clip_control(ctx, packet.header, packet.offset, &rejected_value, packet.available) != 0 ||
		    !only_dx_clip_space(ctx->GetClipControl(), true) || ctx->GetRenderTargetMask() != 0x13579bdfu)
		{
			std::fprintf(stderr, "malformed clip-control packet changed state\n");
			return false;
		}
	}
	if (hw_ctx_update_clip_control(ctx, LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::PA_CL_CLIP_CNTL, nullptr, 3) != 0 ||
	    hw_ctx_update_clip_control(nullptr, LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::PA_CL_CLIP_CNTL,
	                               &rejected_value, 3) != 0 ||
	    !only_dx_clip_space(ctx->GetClipControl(), true))
	{
		std::fprintf(stderr, "null clip-control input changed state\n");
		return false;
	}

	uint32_t stream[] = {
	    LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::PA_CL_CLIP_CNTL, dx_word,
	    LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::CB_TARGET_MASK, 0x2468ace0u,
	};
	cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
	if (!only_dx_clip_space(ctx->GetClipControl(), true) || ctx->GetRenderTargetMask() != 0x2468ace0u)
	{
		std::fprintf(stderr, "clip-control command stream lost alignment\n");
		return false;
	}

	uint32_t indirect_values[] = {Pm4::PA_CL_CLIP_CNTL, 0u, Pm4::PA_CL_CLIP_CNTL, dx_word};
	const auto indirect_addr   = reinterpret_cast<uint64_t>(indirect_values);
	uint32_t indirect_body[]   = {2u, static_cast<uint32_t>(indirect_addr), static_cast<uint32_t>(indirect_addr >> 32u)};
	if (cp_op_indirect_cx_regs(cp, 0xc0021048u, indirect_body, 4, 4) != 3 ||
	    !only_dx_clip_space(ctx->GetClipControl(), true) || !ctx->GetClipControl().IsSupportedForDraw())
	{
		std::fprintf(stderr, "indirect clip-control updates diverged from direct state\n");
		return false;
	}

	cp->Reset();
	if (!only_dx_clip_space(ctx->GetClipControl(), false) || !ctx->GetClipControl().IsSupportedForDraw())
	{
		std::fprintf(stderr, "clip-control reset did not restore the local legacy state\n");
		return false;
	}
	return true;
}

static bool graphics_run_color_target_test(CommandProcessor* cp)
{
	auto* ctx = cp->GetCtx();
	auto values = [](const HW::RenderTarget& t)
	{
		return std::array<uint64_t, 52> {
		    t.base.addr, t.pitch.pitch_div8_minus1, t.pitch.fmask_pitch_div8_minus1, t.slice.slice_div64_minus1,
		    t.view.base_array_slice_index, t.view.last_array_slice_index, t.view.current_mip_level,
		    t.info.fmask_compression_enable, t.info.fmask_data_compression_disable, t.info.fmask_one_frag_mode,
		    t.info.cmask_fast_clear_enable, t.info.dcc_compression_enable, t.info.neo_mode, t.info.blend_clamp,
		    t.info.blend_bypass, t.info.round_mode, t.info.cmask_tile_mode, t.info.cmask_tile_mode_neo,
		    t.info.format, t.info.channel_type, t.info.channel_order,
		    t.attrib.force_dest_alpha_to_one, t.attrib.tile_mode, t.attrib.fmask_tile_mode, t.attrib.num_samples, t.attrib.num_fragments,
		    t.attrib2.height, t.attrib2.width, t.attrib2.num_mip_levels,
		    t.attrib3.depth, t.attrib3.tile_mode, t.attrib3.dimension, t.attrib3.cmask_pipe_aligned, t.attrib3.dcc_pipe_aligned,
		    t.dcc.max_uncompressed_block_size, t.dcc.max_compressed_block_size, t.dcc.min_compressed_block_size, t.dcc.color_transform,
		    t.dcc.dcc_clear_key_enable, t.dcc.overwrite_combiner_disable, t.dcc.independent_64b_blocks,
		    t.dcc.independent_128b_blocks, t.dcc.data_write_on_dcc_clear_to_reg,
		    t.cmask.addr, t.cmask_slice.slice_minus1, t.fmask.addr, t.fmask_slice.slice_minus1,
		    t.clear_word0.word0, t.clear_word1.word1, t.dcc_addr.addr, t.size.width, t.size.height,
		};
	};
	auto set_target = [ctx](uint32_t slot, const HW::RenderTarget& t)
	{
		ctx->SetColorBase(slot, t.base);
		ctx->SetColorPitch(slot, t.pitch);
		ctx->SetColorSlice(slot, t.slice);
		ctx->SetColorView(slot, t.view);
		ctx->SetColorInfo(slot, t.info);
		ctx->SetColorAttrib(slot, t.attrib);
		ctx->SetColorAttrib2(slot, t.attrib2);
		ctx->SetColorAttrib3(slot, t.attrib3);
		ctx->SetColorDccControl(slot, t.dcc);
		ctx->SetColorCmask(slot, t.cmask);
		ctx->SetColorCmaskSlice(slot, t.cmask_slice);
		ctx->SetColorFmask(slot, t.fmask);
		ctx->SetColorFmaskSlice(slot, t.fmask_slice);
		ctx->SetColorClearWord0(slot, t.clear_word0);
		ctx->SetColorClearWord1(slot, t.clear_word1);
		ctx->SetColorDccAddr(slot, t.dcc_addr);
		ctx->SetColorSize(slot, t.size);
	};
	constexpr uint32_t words[] = {
	    0x10203040u, 0x137000cbu, 0xffc12345u, 0x18072017u, 0xdc0df344u, 0x00033169u, 0x0018033bu,
	    0x02030405u, 0xffff0123u, 0x03040506u, 0xffd23456u, 0xa55a1234u, 0x5aa54321u, 0x04050607u,
	};
	HW::RenderTarget written;
	written.base.addr   = 0x1020304000ull;
	written.pitch       = {203, 311};
	written.slice       = {0x12345};
	written.view        = {23, 57, 6};
	written.info.fmask_compression_enable       = true;
	written.info.fmask_data_compression_disable = true;
	written.info.fmask_one_frag_mode            = true;
	written.info.cmask_fast_clear_enable        = true;
	written.info.dcc_compression_enable         = true;
	written.info.neo_mode                       = true;
	written.info.blend_clamp                    = true;
	written.info.blend_bypass                   = true;
	written.info.round_mode                     = true;
	written.info.cmask_tile_mode                = 1;
	written.info.cmask_tile_mode_neo            = 2;
	written.info.format                         = 17;
	written.info.channel_type                   = 3;
	written.info.channel_order                  = 2;
	written.attrib       = {true, 9, 11, 3, 2};
	written.attrib2      = {41, 53, 3};
	written.attrib3      = {7, 9, 2, true, false};
	written.dcc          = {2, 1, 1, 2, true, true, true, true, true};
	written.cmask.addr   = 0x0203040500ull;
	written.cmask_slice  = {0x123};
	written.fmask.addr   = 0x0304050600ull;
	written.fmask_slice  = {0x123456};
	written.clear_word0  = {0xa55a1234u};
	written.clear_word1  = {0x5aa54321u};
	written.dcc_addr.addr = 0x0405060700ull;
	written.size         = {641, 479};
	std::array<HW::RenderTarget, 8> seeds;
	for (uint32_t slot = 0; slot < 8; ++slot)
	{
		seeds[slot] = written;
		seeds[slot].base.addr += (slot + 1u) * 0x100u;
		seeds[slot].size = {641 + slot, 479 + slot};
		set_target(slot, seeds[slot]);
	}
	auto other_slots_preserved = [&](uint32_t updated_slot)
	{
		for (uint32_t slot = 0; slot < 8; ++slot)
		{
			if (slot != updated_slot && values(ctx->GetRenderTarget(slot)) != values(seeds[slot]))
			{
				return false;
			}
		}
		return true;
	};
	auto clear_range = [](HW::RenderTarget& t, uint32_t first, uint32_t count)
	{
		auto includes = [=](uint32_t reg) { return first <= reg && reg < first + count; };
		if (includes(0)) t.base = {};
		if (includes(1)) t.pitch = {};
		if (includes(2)) t.slice = {};
		if (includes(3)) t.view = {};
		if (includes(4)) t.info = {};
		if (includes(5)) t.attrib = {};
		if (includes(6)) t.dcc = {};
		if (includes(7)) t.cmask = {};
		if (includes(8)) t.cmask_slice = {};
		if (includes(9)) t.fmask = {};
		if (includes(10)) t.fmask_slice = {};
		if (includes(11)) t.clear_word0 = {};
		if (includes(12)) t.clear_word1 = {};
		if (includes(13)) t.dcc_addr = {};
	};

	for (uint32_t slot = 0; slot < 8; ++slot)
	{
		uint32_t packet[15] = {Pm4::CB_COLOR0_BASE + slot * 15};
		std::memcpy(packet + 1, words, sizeof(words));
		auto expected = written;
		expected.size = seeds[slot].size;
		if (cp_op_set_context_reg(cp, 0xc00e6900u, packet, 16, 16) != 15 ||
		    values(ctx->GetRenderTarget(slot)) != values(expected) || !other_slots_preserved(slot))
		{
			std::fprintf(stderr, "color-target nonzero register decode failed: slot=%u\n", slot);
			return false;
		}
		set_target(slot, seeds[slot]);
		for (uint32_t first = 0; first < 14; ++first)
		{
			for (uint32_t count = 1; count <= 14 - first; ++count)
			{
				uint32_t partial[15] = {Pm4::CB_COLOR0_BASE + slot * 15 + first};
				auto partial_expected = seeds[slot];
				clear_range(partial_expected, first, count);
				const uint32_t packet_dw = count + 2;
				if (cp_op_set_context_reg(cp, LUMEN_PM4(packet_dw, Pm4::IT_SET_CONTEXT_REG, 0), partial, packet_dw, packet_dw) != count + 1 ||
				    values(ctx->GetRenderTarget(slot)) != values(partial_expected) || !other_slots_preserved(slot))
				{
					std::fprintf(stderr, "color-target partial write failed: slot=%u first=%u count=%u\n", slot, first, count);
					return false;
				}
				set_target(slot, seeds[slot]);
			}
		}
	}

	struct SizeMetadataPacket { uint32_t first; uint32_t count; };
	constexpr SizeMetadataPacket size_packets[] = {{0, 11}, {0, 14}, {7, 4}, {10, 1}, {13, 1}};
	for (const auto& metadata: size_packets)
	{
		const uint32_t count = metadata.count;
		uint32_t packet[17] = {Pm4::CB_COLOR0_BASE + 7 * 15 + metadata.first};
		std::memcpy(packet + 1, words + metadata.first, count * sizeof(uint32_t));
		packet[count + 1] = 0xc0001000u;
		packet[count + 2] = (654u << 16u) | 321u;
		auto expected = seeds[7];
		if (metadata.first == 0)
		{
			expected.base = written.base;
		}
		expected.size = {321, 654};
		if (cp_op_set_context_reg(cp, LUMEN_PM4(count + 2, Pm4::IT_SET_CONTEXT_REG, 0), packet, count + 4, count + 4) != count + 3 ||
		    values(ctx->GetRenderTarget(7)) != values(expected) || !other_slots_preserved(7))
		{
			std::fprintf(stderr, "color-target size metadata failed: first=%u count=%u\n", metadata.first, count);
			return false;
		}
		set_target(7, seeds[7]);
	}

	struct InvalidPacket { uint32_t header; uint32_t offset; uint32_t available; };
	constexpr InvalidPacket invalid[] = {
	    {0xc0006900u, Pm4::CB_COLOR0_BASE, 2}, {0xc00f6900u, Pm4::CB_COLOR0_BASE, 17},
	    {0xc0016800u, Pm4::CB_COLOR0_BASE, 3}, {0x80016900u, Pm4::CB_COLOR0_BASE, 3},
	    {0xc0016901u, Pm4::CB_COLOR0_BASE, 3}, {0xc0016904u, Pm4::CB_COLOR0_BASE, 3},
	    {0xc0016900u, Pm4::CB_COLOR0_BASE - 1, 3}, {0xc0016900u, Pm4::CB_COLOR0_BASE + 8 * 15, 3},
	    {0xc0016900u, Pm4::CB_COLOR0_BASE + 14, 3}, {0xc0026900u, Pm4::CB_COLOR0_BASE + 13, 4},
	    {0xc0016900u, Pm4::CB_COLOR0_BASE, 0}, {0xc0016900u, Pm4::CB_COLOR0_BASE, 2},
	    {0xc00e6900u, Pm4::CB_COLOR0_BASE, 15},
	};
	for (const auto& packet: invalid)
	{
		if (hw_ctx_update_render_target(ctx, packet.header, packet.offset, words, packet.available) != 0 ||
		    values(ctx->GetRenderTarget(0)) != values(seeds[0]) || !other_slots_preserved(0))
		{
			std::fprintf(stderr, "malformed color-target packet was accepted or changed state\n");
			return false;
		}
	}
	for (const auto& metadata: size_packets)
	{
		const uint32_t count = metadata.count;
		uint32_t truncated[15] = {};
		truncated[count] = 0xc0001000u;
		if (hw_ctx_update_render_target(ctx, LUMEN_PM4(count + 2, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::CB_COLOR0_BASE + metadata.first,
		                                truncated, count + 3) != 0 || values(ctx->GetRenderTarget(0)) != values(seeds[0]) ||
		    !other_slots_preserved(0))
		{
			std::fprintf(stderr, "truncated color-target NOP metadata changed state\n");
			return false;
		}
	}
	if (hw_ctx_update_render_target(ctx, 0xc0016900u, Pm4::CB_COLOR0_BASE, nullptr, 3) != 0)
	{
		return false;
	}

	uint32_t stream[] = {
	    LUMEN_PM4(8, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::CB_COLOR0_BASE, 0, 0, 0, 0, 0, 0,
	    LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::PA_CL_GB_HORZ_DISC_ADJ, 0x40000000u,
	    LUMEN_PM4(4, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::CB_COLOR0_BASE + 7 * 15 + 12, 0, 0,
	    LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::CB_TARGET_MASK, 0x13579bdfu,
	};
	cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
	auto first_expected = seeds[0];
	auto last_expected = seeds[7];
	clear_range(first_expected, 0, 6);
	clear_range(last_expected, 12, 2);
	if (values(ctx->GetRenderTarget(0)) != values(first_expected) || values(ctx->GetRenderTarget(7)) != values(last_expected) ||
	    ctx->GetRenderTargetMask() != 0x13579bdfu || ctx->GetScreenViewport().guard_band_horz_discard != 2.0f)
	{
		std::fprintf(stderr, "mixed color-target command stream lost alignment\n");
		return false;
	}
	return true;
}

static bool graphics_run_viewport_scissor_test(CommandProcessor* cp)
{
	auto* ctx = cp->GetCtx();
	const auto saved = *ctx;
	const bool result = [&]()
	{
		HW::Context baseline;
		baseline.SetRenderTargetMask(0x12345678u);
		baseline.SetShaderStages(0x87654321u);
		baseline.SetIndexOffset(0x2468u);
		baseline.SetViewportTransformControl(0x13579bdfu);
		baseline.SetScreenScissor(3, 5, 701, 503);
		baseline.SetGenericScissor(7, 11, 613, 419, true);
		baseline.SetHardwareScreenOffset(17, 19);
		baseline.SetGuardBands(1.25f, 2.5f, 3.75f, 4.5f);
		static_assert(sizeof(HW::ScreenViewport::viewports) == 16 * sizeof(HW::Viewport));
		for (uint32_t slot = 0; slot < 16; ++slot)
		{
			baseline.SetViewportScissor(slot, slot + 1, slot + 17, slot + 33, slot + 49, (slot & 1u) != 0);
			baseline.SetViewportZ(slot, 0.125f, 0.875f);
			baseline.SetViewportScaleOffset(slot, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f);
		}
		const auto same_screen = [](const HW::ScreenViewport& a, const HW::ScreenViewport& b)
		{
			if (a.transform_control != b.transform_control || a.screen_scissor_left != b.screen_scissor_left ||
			    a.screen_scissor_top != b.screen_scissor_top || a.screen_scissor_right != b.screen_scissor_right ||
			    a.screen_scissor_bottom != b.screen_scissor_bottom || a.generic_scissor_left != b.generic_scissor_left ||
			    a.generic_scissor_top != b.generic_scissor_top || a.generic_scissor_right != b.generic_scissor_right ||
			    a.generic_scissor_bottom != b.generic_scissor_bottom || a.generic_scissor_window_offset_enable != b.generic_scissor_window_offset_enable ||
			    a.hw_offset_x != b.hw_offset_x || a.hw_offset_y != b.hw_offset_y || a.guard_band_horz_clip != b.guard_band_horz_clip ||
			    a.guard_band_vert_clip != b.guard_band_vert_clip || a.guard_band_horz_discard != b.guard_band_horz_discard ||
			    a.guard_band_vert_discard != b.guard_band_vert_discard) return false;
			for (size_t slot = 0; slot < std::size(a.viewports); ++slot)
			{
				const auto& x = a.viewports[slot];
				const auto& y = b.viewports[slot];
				if (x.zmin != y.zmin || x.zmax != y.zmax || x.xscale != y.xscale || x.xoffset != y.xoffset ||
				    x.yscale != y.yscale || x.yoffset != y.yoffset || x.zscale != y.zscale || x.zoffset != y.zoffset ||
				    x.viewport_scissor_left != y.viewport_scissor_left || x.viewport_scissor_top != y.viewport_scissor_top ||
				    x.viewport_scissor_right != y.viewport_scissor_right || x.viewport_scissor_bottom != y.viewport_scissor_bottom ||
				    x.viewport_scissor_window_offset_enable != y.viewport_scissor_window_offset_enable) return false;
			}
			return true;
		};
		const auto unrelated = [&]()
		{
			return ctx->GetRenderTargetMask() == baseline.GetRenderTargetMask() && ctx->GetShaderStages() == baseline.GetShaderStages() &&
			    ctx->GetIndexOffset() == baseline.GetIndexOffset();
		};
		constexpr uint32_t first_reg = Pm4::PA_SC_VPORT_SCISSOR_0_TL;
		constexpr uint32_t values[] = {0x80000000u, 0xffffffffu, 0x00010002u, 0x7fff7fffu};
		for (uint32_t first = 0; first < 32; ++first)
		{
			for (uint32_t count = 1; count <= 32 - first; ++count)
			{
				*ctx = baseline;
				uint32_t packet[33] = {first_reg + first};
				uint32_t indirect_values[64] = {};
				for (uint32_t i = 0; i < count; ++i)
				{
					packet[i + 1] = values[i % std::size(values)];
					indirect_values[i * 2] = first_reg + first + i;
					indirect_values[i * 2 + 1] = packet[i + 1];
				}
				const uint32_t dw = count + 2;
				if (cp_op_set_context_reg(cp, LUMEN_PM4(dw, Pm4::IT_SET_CONTEXT_REG, 0), packet, dw, dw) != count + 1 || !unrelated()) return false;
				const auto direct = ctx->GetScreenViewport();
				auto untouched = direct;
				for (uint32_t reg = first; reg < first + count; ++reg)
				{
					auto& a = untouched.viewports[reg / 2];
					const auto& b = baseline.GetScreenViewport().viewports[reg / 2];
					if ((reg & 1u) == 0)
					{
						a.viewport_scissor_left = b.viewport_scissor_left;
						a.viewport_scissor_top = b.viewport_scissor_top;
						a.viewport_scissor_window_offset_enable = b.viewport_scissor_window_offset_enable;
					} else
					{
						a.viewport_scissor_right = b.viewport_scissor_right;
						a.viewport_scissor_bottom = b.viewport_scissor_bottom;
					}
				}
				if (!same_screen(untouched, baseline.GetScreenViewport())) return false;
				*ctx = baseline;
				const auto address = reinterpret_cast<uint64_t>(indirect_values);
				uint32_t indirect[] = {count, static_cast<uint32_t>(address), static_cast<uint32_t>(address >> 32u)};
				if (cp_op_indirect_cx_regs(cp, 0xc0021048u, indirect, 4, 4) != 3 || !unrelated() ||
				    !same_screen(direct, ctx->GetScreenViewport())) return false;
			}
		}
		*ctx = baseline;
		struct Invalid { uint32_t header; uint32_t offset; uint32_t available; };
		constexpr Invalid invalid[] = {
		    {0xc0006900u, first_reg, 2}, {0xc0016800u, first_reg, 3}, {0x80016900u, first_reg, 3},
		    {0xc0016901u, first_reg, 3}, {0xc0016902u, first_reg, 3}, {0xc0016904u, first_reg, 3},
		    {0xc0016900u, first_reg - 1, 3}, {0xc0016900u, first_reg + 32, 3}, {0xc0016900u, UINT32_MAX, 3},
		    {0xc0016900u, first_reg, 0}, {0xc0016900u, first_reg, 2}, {0xc0026900u, first_reg, 3},
		    {0xc0026900u, first_reg + 31, 4}, {0xc0216900u, first_reg, 35}, {0xffff6900u, first_reg, UINT32_MAX},
		};
		for (const auto& bad: invalid)
			if (hw_ctx_update_viewport_scissor(ctx, bad.header, bad.offset, values, bad.available) != 0 || !unrelated() ||
			    !same_screen(ctx->GetScreenViewport(), baseline.GetScreenViewport())) return false;
		if (hw_ctx_update_viewport_scissor(nullptr, 0xc0016900u, first_reg, values, 3) != 0 ||
		    hw_ctx_update_viewport_scissor(ctx, 0xc0016900u, first_reg, nullptr, 3) != 0 || !unrelated() ||
		    !same_screen(ctx->GetScreenViewport(), baseline.GetScreenViewport())) return false;

		uint32_t stream[] = {0xc0026900u, first_reg, 0x80000000u, (1080u << 16u) | 1920u,
		    0xc0016900u, first_reg + 31, (719u << 16u) | 1279u,
		    0xc0016900u, Pm4::CB_TARGET_MASK, 0xabcdef01u};
		cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
		const auto& viewport = ctx->GetScreenViewport();
		if (viewport.viewports[0].viewport_scissor_left != 0 || viewport.viewports[0].viewport_scissor_top != 0 ||
		    viewport.viewports[0].viewport_scissor_window_offset_enable || viewport.viewports[0].viewport_scissor_right != 1920 ||
		    viewport.viewports[0].viewport_scissor_bottom != 1080 || viewport.viewports[15].viewport_scissor_right != 1279 ||
		    viewport.viewports[15].viewport_scissor_bottom != 719 || viewport.transform_control != 0x13579bdfu ||
		    ctx->GetRenderTargetMask() != 0xabcdef01u) return false;
		cp->Reset();
		return same_screen(ctx->GetScreenViewport(), HW::Context().GetScreenViewport());
	}();
	*ctx = saved;
	if (!result) std::fprintf(stderr, "viewport-scissor direct/indirect contract failed\n");
	return result;
}

static bool graphics_run_guard_band_test(CommandProcessor* cp)
{
	auto* ctx = cp->GetCtx();
	ctx->SetViewportTransformControl(0x1357u);
	ctx->SetScreenScissor(3, 5, 701, 503);
	ctx->SetGenericScissor(7, 11, 613, 419, true);
	ctx->SetHardwareScreenOffset(17, 19);
	ctx->SetViewportZ(3, 0.125f, 0.875f);
	ctx->SetViewportScaleOffset(3, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f);
	ctx->SetViewportScissor(3, 23, 29, 307, 211, true);
	const auto baseline = ctx->GetScreenViewport();
	auto unrelated_state_preserved = [&baseline](const HW::ScreenViewport& current)
	{
		if (current.transform_control != baseline.transform_control || current.screen_scissor_left != baseline.screen_scissor_left ||
		    current.screen_scissor_top != baseline.screen_scissor_top || current.screen_scissor_right != baseline.screen_scissor_right ||
		    current.screen_scissor_bottom != baseline.screen_scissor_bottom || current.generic_scissor_left != baseline.generic_scissor_left ||
		    current.generic_scissor_top != baseline.generic_scissor_top || current.generic_scissor_right != baseline.generic_scissor_right ||
		    current.generic_scissor_bottom != baseline.generic_scissor_bottom ||
		    current.generic_scissor_window_offset_enable != baseline.generic_scissor_window_offset_enable ||
		    current.hw_offset_x != baseline.hw_offset_x || current.hw_offset_y != baseline.hw_offset_y)
		{
			return false;
		}
		for (size_t i = 0; i < std::size(current.viewports); ++i)
		{
			const auto& a = current.viewports[i];
			const auto& b = baseline.viewports[i];
			if (a.zmin != b.zmin || a.zmax != b.zmax || a.xscale != b.xscale || a.xoffset != b.xoffset ||
			    a.yscale != b.yscale || a.yoffset != b.yoffset || a.zscale != b.zscale || a.zoffset != b.zoffset ||
			    a.viewport_scissor_left != b.viewport_scissor_left || a.viewport_scissor_top != b.viewport_scissor_top ||
			    a.viewport_scissor_right != b.viewport_scissor_right || a.viewport_scissor_bottom != b.viewport_scissor_bottom ||
			    a.viewport_scissor_window_offset_enable != b.viewport_scissor_window_offset_enable)
			{
				return false;
			}
		}
		return true;
	};
	auto guard_bands_match = [ctx](const float* expected)
	{
		const auto& current = ctx->GetScreenViewport();
		return current.guard_band_vert_clip == expected[0] && current.guard_band_vert_discard == expected[1] &&
		       current.guard_band_horz_clip == expected[2] && current.guard_band_horz_discard == expected[3];
	};
	constexpr float initial[] = {11.25f, 12.5f, 13.75f, 15.0f};
	constexpr float updates[] = {1.25f, 2.5f, 3.75f, 5.0f};
	uint32_t update_words[4] = {};
	std::memcpy(update_words, updates, sizeof(update_words));
	for (uint32_t first = 0; first < 4; ++first)
	{
		for (uint32_t count = 1; count <= 4 - first; ++count)
		{
			ctx->SetGuardBands(initial[2], initial[0], initial[3], initial[1]);
			float expected[4];
			std::memcpy(expected, initial, sizeof(expected));
			for (uint32_t i = 0; i < count; ++i)
			{
				expected[first + i] = updates[i];
			}
			uint32_t packet[5] = {Pm4::PA_CL_GB_VERT_CLIP_ADJ + first};
			std::memcpy(packet + 1, update_words, count * sizeof(uint32_t));
			const uint32_t packet_dw = count + 2;
			if (cp_op_set_context_reg(cp, LUMEN_PM4(packet_dw, Pm4::IT_SET_CONTEXT_REG, 0), packet, packet_dw, packet_dw) != count + 1 ||
			    !guard_bands_match(expected) || !unrelated_state_preserved(ctx->GetScreenViewport()))
			{
				std::fprintf(stderr, "guard-band partial write failed: first=%u count=%u\n", first, count);
				return false;
			}
		}
	}

	ctx->SetGuardBands(initial[2], initial[0], initial[3], initial[1]);
	struct InvalidPacket
	{
		uint32_t header;
		uint32_t offset;
		uint32_t available;
	};
	constexpr InvalidPacket invalid[] = {
	    {0xc0006900u, Pm4::PA_CL_GB_VERT_CLIP_ADJ, 2},
	    {0xc0056900u, Pm4::PA_CL_GB_VERT_CLIP_ADJ, 7},
	    {0xc0016800u, Pm4::PA_CL_GB_VERT_CLIP_ADJ, 3},
	    {0x80016900u, Pm4::PA_CL_GB_VERT_CLIP_ADJ, 3},
	    {0xc0016901u, Pm4::PA_CL_GB_VERT_CLIP_ADJ, 3},
	    {0xc0016904u, Pm4::PA_CL_GB_VERT_CLIP_ADJ, 3},
	    {0xc0016900u, Pm4::PA_CL_GB_VERT_CLIP_ADJ - 1, 3},
	    {0xc0016900u, Pm4::PA_CL_GB_HORZ_DISC_ADJ + 1, 3},
	    {0xc0046900u, Pm4::PA_CL_GB_VERT_DISC_ADJ, 6},
	    {0xc0036900u, Pm4::PA_CL_GB_HORZ_CLIP_ADJ, 5},
	    {0xc0026900u, Pm4::PA_CL_GB_HORZ_DISC_ADJ, 4},
	    {0xc0016900u, Pm4::PA_CL_GB_VERT_CLIP_ADJ, 0},
	    {0xc0016900u, Pm4::PA_CL_GB_VERT_CLIP_ADJ, 2},
	    {0xc0046900u, Pm4::PA_CL_GB_VERT_CLIP_ADJ, 5},
	};
	for (const auto& packet: invalid)
	{
		if (hw_ctx_update_guard_bands(ctx, packet.header, packet.offset, update_words, packet.available) != 0 ||
		    !guard_bands_match(initial) || !unrelated_state_preserved(ctx->GetScreenViewport()))
		{
			std::fprintf(stderr, "malformed guard-band packet was accepted or changed state\n");
			return false;
		}
	}
	if (hw_ctx_update_guard_bands(ctx, 0xc0016900u, Pm4::PA_CL_GB_VERT_CLIP_ADJ, nullptr, 3) != 0 ||
	    !guard_bands_match(initial) || !unrelated_state_preserved(ctx->GetScreenViewport()))
	{
		return false;
	}

	uint32_t stream[] = {
	    LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::PA_CL_GB_VERT_DISC_ADJ, update_words[0],
	    LUMEN_PM4(4, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::PA_CL_GB_HORZ_CLIP_ADJ, update_words[1], update_words[2],
	    LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::PA_CL_GB_VERT_CLIP_ADJ, update_words[3],
	};
	cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
	const float expected_stream[] = {updates[3], updates[0], updates[1], updates[2]};
	if (!guard_bands_match(expected_stream) || !unrelated_state_preserved(ctx->GetScreenViewport()))
	{
		std::fprintf(stderr, "guard-band packet stream lost alignment or state\n");
		return false;
	}
	return true;
}

static bool graphics_run_administrative_packet_test(CommandProcessor* cp)
{
	constexpr uint32_t context_header = LUMEN_PM4(3, Pm4::IT_CONTEXT_CONTROL, 0);
	uint32_t           context_body[] = {0x80000000u, 0x80000000u};
	if (!pm4_context_control_packet_supported(context_header, context_body, 3) ||
	    pm4_context_control_packet_supported(LUMEN_PM4(2, Pm4::IT_CONTEXT_CONTROL, 0), context_body, 3) ||
	    pm4_context_control_packet_supported(context_header | 1u, context_body, 3) ||
	    pm4_context_control_packet_supported(context_header, context_body, 2) ||
	    pm4_context_control_packet_supported(context_header, nullptr, 3))
	{
		std::fprintf(stderr, "context-control packet shape validation failed\n");
		return false;
	}
	context_body[0]                           = 0;
	const bool rejects_first_payload_variant  = !pm4_context_control_packet_supported(context_header, context_body, 3);
	context_body[0]                           = 0x80000000u;
	context_body[1]                           = 0;
	const bool rejects_second_payload_variant = !pm4_context_control_packet_supported(context_header, context_body, 3);
	context_body[1]                           = 0x80000000u;
	if (!rejects_first_payload_variant || !rejects_second_payload_variant)
	{
		std::fprintf(stderr, "unsupported context-control payload was accepted\n");
		return false;
	}
	uint32_t clear_body = 0;
	if (!pm4_clear_state_packet_supported(LUMEN_PM4(2, Pm4::IT_CLEAR_STATE, 0), &clear_body, 2) ||
	    pm4_clear_state_packet_supported(LUMEN_PM4(3, Pm4::IT_CLEAR_STATE, 0), &clear_body, 3) ||
	    pm4_clear_state_packet_supported(LUMEN_PM4(2, Pm4::IT_CLEAR_STATE, 0) | 1u, &clear_body, 2) ||
	    pm4_clear_state_packet_supported(LUMEN_PM4(2, Pm4::IT_CLEAR_STATE, 0), &clear_body, 1) ||
	    pm4_clear_state_packet_supported(LUMEN_PM4(2, Pm4::IT_CLEAR_STATE, 0), nullptr, 2))
	{
		std::fprintf(stderr, "clear-state packet shape validation failed\n");
		return false;
	}
	clear_body = 1;
	if (pm4_clear_state_packet_supported(LUMEN_PM4(2, Pm4::IT_CLEAR_STATE, 0), &clear_body, 2))
	{
		std::fprintf(stderr, "unsupported clear-state payload was accepted\n");
		return false;
	}

	auto marker_decision = [](uint32_t marker, uint32_t packet_dw, uint32_t available_dw)
	{
		return pm4_nop_marker_decision(&marker, packet_dw, available_dw);
	};
	if (marker_decision(0x68750000u, 2, 2) != Pm4NopMarkerDecision::EmbeddedData ||
	    marker_decision(0x6875f000u, 7, 7) != Pm4NopMarkerDecision::EmbeddedData ||
	    marker_decision(0x68750004u, 2, 2) != Pm4NopMarkerDecision::UserDataVsharp ||
	    marker_decision(0x68750004u, 3, 3) != Pm4NopMarkerDecision::UserDataVsharp ||
	    marker_decision(0x6875000du, 2, 2) != Pm4NopMarkerDecision::UserDataRegion ||
	    marker_decision(0x6875000du, 3, 3) != Pm4NopMarkerDecision::UserDataRegion ||
	    marker_decision(0x68750777u, 2, 2) != Pm4NopMarkerDecision::Flip ||
	    marker_decision(0x68750777u, 3, 3) != Pm4NopMarkerDecision::Flip ||
	    marker_decision(0x68750778u, 5, 5) != Pm4NopMarkerDecision::FlipWithWrite ||
	    marker_decision(0x68750778u, 4, 4) != Pm4NopMarkerDecision::Malformed ||
	    marker_decision(0x68750778u, 6, 6) != Pm4NopMarkerDecision::FlipWithWrite ||
	    marker_decision(0x68750781u, 7, 7) != Pm4NopMarkerDecision::FlipWithInterrupt ||
	    marker_decision(0x68750781u, 6, 6) != Pm4NopMarkerDecision::Malformed ||
	    marker_decision(0x68750781u, 8, 8) != Pm4NopMarkerDecision::FlipWithInterrupt ||
	    marker_decision(0x68750781u, 7, 6) != Pm4NopMarkerDecision::Malformed ||
	    marker_decision(0x68750abcu, 2, 2) != Pm4NopMarkerDecision::OrdinaryNop ||
	    marker_decision(0x68750abcu, 4, 4) != Pm4NopMarkerDecision::OrdinaryNop ||
	    marker_decision(0x13579bdfu, 2, 2) != Pm4NopMarkerDecision::OrdinaryNop ||
	    pm4_nop_marker_decision(nullptr, 2, 2) != Pm4NopMarkerDecision::Malformed)
	{
		std::fprintf(stderr, "NOP marker packet shape validation failed\n");
		return false;
	}

	uint32_t ordinary_nop[]        = {LUMEN_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO), 0};
	uint32_t padded_nop[]          = {LUMEN_PM4(3, Pm4::IT_NOP, Pm4::R_ZERO), 0x13579bdfu, 0x2468ace0u};
	uint32_t colliding_nop[]       = {LUMEN_PM4(3, Pm4::IT_NOP, Pm4::R_ZERO), 0x68750abcu, 0x2468ace0u};
	uint32_t embedded_marker[]     = {LUMEN_PM4(4, Pm4::IT_NOP, Pm4::R_ZERO), 0x68753000u, 0x13579bdfu, 0x2468ace0u};
	uint32_t vsharp_marker[]       = {LUMEN_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO), 0x68750004u};
	uint32_t region_marker[]       = {LUMEN_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO), 0x6875000du};
	uint32_t flip_marker[]         = {LUMEN_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO), 0x68750777u};
	uint32_t marker_write_target   = 0;
	const auto marker_write_addr  = reinterpret_cast<uint64_t>(&marker_write_target);
	uint32_t flip_write_marker[]   = {LUMEN_PM4(5, Pm4::IT_NOP, Pm4::R_ZERO), 0x68750778u,
	                                  static_cast<uint32_t>(marker_write_addr), static_cast<uint32_t>(marker_write_addr >> 32u),
	                                  0x13579bdfu};
	uint32_t flip_eop_marker[]     = {LUMEN_PM4(7, Pm4::IT_NOP, Pm4::R_ZERO), 0x68750781u,
	                                  static_cast<uint32_t>(marker_write_addr), static_cast<uint32_t>(marker_write_addr >> 32u),
	                                  0x2468ace0u, 0x11223344u, 0x55667788u};
	uint32_t padded_region_marker[] = {LUMEN_PM4(3, Pm4::IT_NOP, Pm4::R_ZERO), 0x6875000du, 0};
	uint32_t short_flip_write[] = {LUMEN_PM4(4, Pm4::IT_NOP, Pm4::R_ZERO), 0x68750778u,
	                               static_cast<uint32_t>(marker_write_addr), static_cast<uint32_t>(marker_write_addr >> 32u)};
	uint32_t padded_flip_eop[] = {LUMEN_PM4(8, Pm4::IT_NOP, Pm4::R_ZERO), 0x68750781u,
	                              static_cast<uint32_t>(marker_write_addr), static_cast<uint32_t>(marker_write_addr >> 32u),
	                              0x2468ace0u, 0x11223344u, 0x55667788u, 0};
	uint32_t context_packet[]      = {context_header, 0x80000000u, 0x80000000u};
	uint32_t malformed_context[]   = {LUMEN_PM4(2, Pm4::IT_CONTEXT_CONTROL, 0), 0x80000000u};
	uint32_t unsupported_context[] = {context_header, 0x80000000u, 0};
	uint32_t clear_packet[]        = {LUMEN_PM4(2, Pm4::IT_CLEAR_STATE, 0), 0};
	uint32_t malformed_clear[]     = {LUMEN_PM4(3, Pm4::IT_CLEAR_STATE, 0), 0, 0};
	uint32_t unsupported_clear[]   = {LUMEN_PM4(2, Pm4::IT_CLEAR_STATE, 0), 1};
	if (root_command_ownership_decision(ordinary_nop, static_cast<uint32_t>(std::size(ordinary_nop)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Eligible ||
	    root_command_ownership_decision(padded_nop, static_cast<uint32_t>(std::size(padded_nop)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Eligible ||
	    root_command_ownership_decision(colliding_nop, static_cast<uint32_t>(std::size(colliding_nop)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Eligible ||
	    root_command_ownership_decision(embedded_marker, static_cast<uint32_t>(std::size(embedded_marker)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Eligible ||
	    root_command_ownership_decision(vsharp_marker, static_cast<uint32_t>(std::size(vsharp_marker)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Eligible ||
	    root_command_ownership_decision(region_marker, static_cast<uint32_t>(std::size(region_marker)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Eligible ||
	    root_command_ownership_decision(flip_marker, static_cast<uint32_t>(std::size(flip_marker)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Eligible ||
	    root_command_ownership_decision(flip_write_marker, static_cast<uint32_t>(std::size(flip_write_marker)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Eligible ||
	    root_command_ownership_decision(flip_eop_marker, static_cast<uint32_t>(std::size(flip_eop_marker)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Eligible ||
	    root_command_ownership_decision(padded_region_marker, static_cast<uint32_t>(std::size(padded_region_marker)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Eligible ||
	    root_command_ownership_decision(short_flip_write, static_cast<uint32_t>(std::size(short_flip_write)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Malformed ||
	    root_command_ownership_decision(padded_flip_eop, static_cast<uint32_t>(std::size(padded_flip_eop)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Eligible ||
	    root_command_ownership_decision(context_packet, static_cast<uint32_t>(std::size(context_packet)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Eligible ||
	    root_command_ownership_decision(malformed_context, static_cast<uint32_t>(std::size(malformed_context)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Malformed ||
	    root_command_ownership_decision(unsupported_context, static_cast<uint32_t>(std::size(unsupported_context)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Unsupported ||
	    root_command_ownership_decision(clear_packet, static_cast<uint32_t>(std::size(clear_packet)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Eligible ||
	    root_command_ownership_decision(malformed_clear, static_cast<uint32_t>(std::size(malformed_clear)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Malformed ||
	    root_command_ownership_decision(unsupported_clear, static_cast<uint32_t>(std::size(unsupported_clear)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Unsupported)
	{
		std::fprintf(stderr, "administrative packet ownership classification failed\n");
		return false;
	}

	auto* ctx = cp->GetCtx();
	ctx->SetRenderTargetMask(0xaaaaaaaa);
	ctx->SetShaderStages(0x55555555);
	ctx->SetIndexOffset(0x12345678);
	ctx->SetGsMode(0x11111111u);
	ctx->SetStreamoutConfig(0x22222222u);
	uint32_t collision_stream[] = {
	    LUMEN_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO),
	    0x6875000du,
	    LUMEN_PM4(3, Pm4::IT_NOP, Pm4::R_ZERO),
	    0x68750abcu,
	    0x2468ace0u,
	    LUMEN_PM4(3, Pm4::IT_SET_SH_REG, 0),
	    Pm4::SPI_SHADER_USER_DATA_PS_0 + 15,
	    0x0abc0defu,
	};
	cp->Run(collision_stream, static_cast<uint32_t>(std::size(collision_stream)));
	const auto& collision_ps_sgprs = cp->GetShCtx()->GetPs().ps_user_sgpr;
	if (ctx->GetRenderTargetMask() != 0xaaaaaaaau || ctx->GetShaderStages() != 0x55555555u ||
	    ctx->GetIndexOffset() != 0x12345678u || collision_ps_sgprs.value[15] != 0x0abc0defu ||
	    collision_ps_sgprs.type[15] != HW::UserSgprType::Region || ctx->GetGsMode() != 0x11111111u ||
	    ctx->GetStreamoutConfig() != 0x22222222u)
	{
		std::fprintf(stderr, "unknown NOP marker collision changed state or lost stream alignment\n");
		return false;
	}

	ctx->SetRenderTargetMask(0xaaaaaaaa);
	ctx->SetShaderStages(0x55555555);
	ctx->SetIndexOffset(0x12345678);
	ctx->SetGsMode(0x11111111u);
	ctx->SetStreamoutConfig(0x22222222u);
	uint32_t stream[] = {
	    LUMEN_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO),
	    0,
	    LUMEN_PM4(3, Pm4::IT_NOP, Pm4::R_ZERO),
	    0x13579bdfu,
	    0x2468ace0u,
	    context_header,
	    0x80000000u,
	    0x80000000u,
	    LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0),
	    Pm4::CB_TARGET_MASK,
	    0x87654321u,
	};
	cp->Run(stream, static_cast<uint32_t>(std::size(stream)));
	if (ctx->GetRenderTargetMask() != 0x87654321u || ctx->GetShaderStages() != 0x55555555u ||
	    ctx->GetIndexOffset() != 0x12345678u || ctx->GetGsMode() != 0x11111111u || ctx->GetStreamoutConfig() != 0x22222222u)
	{
		std::fprintf(stderr, "administrative packet stream lost alignment or changed graphics state\n");
		return false;
	}
	ctx->SetRenderTargetMask(0xaaaaaaaa);
	ctx->SetShaderStages(0x55555555);
	ctx->SetIndexOffset(0x12345678);
	ctx->SetGsMode(0x11111111u);
	ctx->SetStreamoutConfig(0x22222222u);
	cp->GetUcfg()->SetPrimitiveType(0x13579bdfu);
	HW::CsStageRegisters cs_sentinel;
	cs_sentinel.data_addr    = 0x1020304000ull;
	cs_sentinel.num_thread_x = 7;
	cp->GetShCtx()->SetCsShader(cs_sentinel, 0x89abcdefu);
	cp->GetShCtx()->SetCsUserSgpr(3, 0x31415926u, HW::UserSgprType::Region);
	uint32_t clear_stream[] = {
	    LUMEN_PM4(2, Pm4::IT_CLEAR_STATE, 0), 0, LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::CB_TARGET_MASK, 0x2468ace0u,
	};
	cp->Run(clear_stream, static_cast<uint32_t>(std::size(clear_stream)));
	const auto& preserved_cs = cp->GetShCtx()->GetCs();
	if (ctx->GetRenderTargetMask() != 0x2468ace0u || ctx->GetShaderStages() != 0 || ctx->GetIndexOffset() != 0 ||
	    ctx->GetGsMode() != 0 || ctx->IsGsModeConfigured() || ctx->GetStreamoutConfig() != 0 ||
	    ctx->IsStreamoutConfigConfigured() || !ctx->IsGeometryStreamoutSupportedForDraw() ||
	    cp->GetUcfg()->GetPrimType() != 0x13579bdfu || preserved_cs.cs_regs.data_addr != cs_sentinel.data_addr ||
	    preserved_cs.cs_regs.num_thread_x != cs_sentinel.num_thread_x || preserved_cs.cs_shader_modifier != 0x89abcdefu ||
	    preserved_cs.cs_user_sgpr.value[3] != 0x31415926u || preserved_cs.cs_user_sgpr.type[3] != HW::UserSgprType::Region)
	{
		std::fprintf(stderr, "clear-state packet did not reset only context state or lost stream alignment\n");
		return false;
	}
	return true;
}

static bool graphics_run_dispatch_direct_packet_test()
{
	uint32_t                standard_body[4] = {2, 3, 5, 1};
	uint32_t                custom_body[8]   = {7, 11, 13, 0x89abcdefu};
	constexpr uint32_t      standard_header  = PM4_DISPATCH_DIRECT_COMPUTE_HEADER;
	constexpr uint32_t      custom_header    = LUMEN_PM4(9, Pm4::IT_NOP, Pm4::R_DISPATCH_DIRECT);
	Pm4DispatchDirectPacket standard;
	Pm4DispatchDirectPacket custom;
	if (standard_header != 0xc0031502u || custom_header != 0xc0071020u ||
	    !pm4_decode_dispatch_direct(standard_header, standard_body, 5, &standard) || standard.thread_group_x != 2 ||
	    standard.thread_group_y != 3 || standard.thread_group_z != 5 || standard.renderer_mode != 0 || standard.consumed != 4 ||
	    !pm4_decode_dispatch_direct(custom_header, custom_body, 9, &custom) || custom.thread_group_x != 7 || custom.thread_group_y != 11 ||
	    custom.thread_group_z != 13 || custom.renderer_mode != 0x89abcdefu || custom.consumed != 8 ||
	    pm4_decode_dispatch_direct(standard_header, standard_body, 4, &standard) ||
	    pm4_decode_dispatch_direct(custom_header, custom_body, 8, &custom) ||
	    pm4_decode_dispatch_direct(standard_header | 1u, standard_body, 5, &standard) ||
	    pm4_decode_dispatch_direct(standard_header & ~2u, standard_body, 5, &standard) ||
	    pm4_decode_dispatch_direct(standard_header, nullptr, 5, &standard) ||
	    pm4_decode_dispatch_direct(standard_header, standard_body, 5, nullptr))
	{
		std::fprintf(stderr, "dispatch-direct packet shape validation failed\n");
		return false;
	}
	standard_body[3]                         = 0;
	const bool rejects_standard_control_zero = !pm4_decode_dispatch_direct(standard_header, standard_body, 5, &standard);
	standard_body[3]                         = 2;
	const bool rejects_standard_control_two  = !pm4_decode_dispatch_direct(standard_header, standard_body, 5, &standard);
	standard_body[3]                         = 1;
	if (!rejects_standard_control_zero || !rejects_standard_control_two)
	{
		std::fprintf(stderr, "unsupported standard dispatch-direct control was accepted\n");
		return false;
	}

	uint32_t standard_stream[] = {standard_header, 2, 3, 5, 1, LUMEN_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO), 0};
	if (!pm4_decode_dispatch_direct(standard_stream[0], standard_stream + 1, static_cast<uint32_t>(std::size(standard_stream)),
	                                &standard) ||
	    standard_stream[1 + standard.consumed] != LUMEN_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO))
	{
		std::fprintf(stderr, "dispatch-direct decode lost the following packet boundary\n");
		return false;
	}
	uint32_t standard_packet[]    = {standard_header, 2, 3, 5, 1};
	uint32_t custom_packet[]      = {custom_header, 7, 11, 13, 0x89abcdefu, 0, 0, 0, 0};
	uint32_t malformed_custom[]   = {LUMEN_PM4(8, Pm4::IT_NOP, Pm4::R_DISPATCH_DIRECT), 7, 11, 13, 0, 0, 0, 0};
	uint32_t malformed_packet[]   = {LUMEN_PM4(4, Pm4::IT_DISPATCH_DIRECT, 0) | 2u, 2, 3, 5};
	uint32_t unsupported_packet[] = {standard_header | 1u, 2, 3, 5, 1};
	if (root_command_ownership_decision(standard_packet, static_cast<uint32_t>(std::size(standard_packet)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Eligible ||
	    root_command_ownership_decision(custom_packet, static_cast<uint32_t>(std::size(custom_packet)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Eligible ||
	    root_command_ownership_decision(malformed_custom, static_cast<uint32_t>(std::size(malformed_custom)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Malformed ||
	    root_command_ownership_decision(malformed_packet, static_cast<uint32_t>(std::size(malformed_packet)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Malformed ||
	    root_command_ownership_decision(unsupported_packet, static_cast<uint32_t>(std::size(unsupported_packet)), nullptr, 0, true) !=
	        RootCommandOwnershipDecision::Unsupported)
	{
		std::fprintf(stderr, "dispatch-direct ownership classification failed\n");
		return false;
	}
	return true;
}

bool GraphicsRunDx10ClampBitExtractionContractTest()
{
	constexpr uint32_t vs_bit = 1u << Pm4::SPI_SHADER_PGM_RSRC1_VS_DX10_CLAMP_SHIFT;
	constexpr uint32_t ps_bit = 1u << Pm4::SPI_SHADER_PGM_RSRC1_PS_DX10_CLAMP_SHIFT;
	constexpr uint32_t gs_bit = 1u << Pm4::SPI_SHADER_PGM_RSRC1_GS_DX10_CLAMP_SHIFT;
	return LUMEN_PM4_GET(0u, SPI_SHADER_PGM_RSRC1_VS, DX10_CLAMP) == 0u &&
	       LUMEN_PM4_GET(vs_bit, SPI_SHADER_PGM_RSRC1_VS, DX10_CLAMP) == 1u &&
	       LUMEN_PM4_GET(0u, SPI_SHADER_PGM_RSRC1_PS, DX10_CLAMP) == 0u &&
	       LUMEN_PM4_GET(ps_bit, SPI_SHADER_PGM_RSRC1_PS, DX10_CLAMP) == 1u &&
	       LUMEN_PM4_GET(0u, SPI_SHADER_PGM_RSRC1_GS, DX10_CLAMP) == 0u && LUMEN_PM4_GET(gs_bit, SPI_SHADER_PGM_RSRC1_GS, DX10_CLAMP) == 1u;
}

bool GraphicsRunConstRamBoundsContractTest()
{
	struct GuardedRam
	{
		uint32_t before = 0x12345678u;
		uint32_t words[CONST_RAM_DWORDS] = {};
		uint32_t after = 0x87654321u;
	} ram;
	std::fill(std::begin(ram.words), std::end(ram.words), 0xa5a5a5a5u);
	const uint32_t last_offset = (CONST_RAM_DWORDS - 1u) * sizeof(uint32_t);
	const uint32_t source[] = {0x10203040u, 0x50607080u};
	std::array<uint32_t, 4> destination {0x13572468u, 0xa5a5a5a5u, 0xa5a5a5a5u, 0x24681357u};
	if (const_ram_write(ram.words, last_offset, source, 1) != nullptr ||
	    ram.words[CONST_RAM_DWORDS - 1u] != source[0] ||
	    const_ram_read(ram.words, destination.data() + 1, last_offset, 1) != nullptr ||
	    destination != std::array<uint32_t, 4> {0x13572468u, source[0], 0xa5a5a5a5u, 0x24681357u} ||
	    ram.before != 0x12345678u || ram.after != 0x87654321u) return false;
	for (size_t index = 0; index + 1 < CONST_RAM_DWORDS; ++index)
		if (ram.words[index] != 0xa5a5a5a5u) return false;

	const auto unchanged_ram = ram;
	const auto unchanged_destination = destination;
	const std::array<std::pair<uint32_t, uint32_t>, 7> rejected {{
	    {last_offset, 2u}, {last_offset - 4u, 3u}, {0u, CONST_RAM_DWORDS},
	    {0u, UINT32_MAX}, {UINT32_MAX, 1u}, {last_offset + 4u, 0u}, {last_offset - 1u, 1u}}};
	for (const auto& [offset, count]: rejected)
	{
		if (const_ram_write(ram.words, offset, source, count) == nullptr ||
		    const_ram_read(ram.words, destination.data() + 1, offset, count) == nullptr ||
		    std::memcmp(ram.words, unchanged_ram.words, sizeof(ram.words)) != 0 ||
		    ram.before != unchanged_ram.before || ram.after != unchanged_ram.after ||
		    destination != unchanged_destination) return false;
	}
	// Preserve zero-count admission without calling memcpy on null pointers.
	if (const_ram_write(ram.words, last_offset, nullptr, 0) != nullptr ||
	    const_ram_read(ram.words, nullptr, last_offset, 0) != nullptr ||
	    const_ram_write(ram.words, 0, nullptr, 1) == nullptr ||
	    const_ram_read(ram.words, nullptr, 0, 1) == nullptr ||
	    const_ram_range_rejection(4, CONST_RAM_DWORDS - 1u, CONST_RAM_DWORDS) != nullptr ||
	    std::memcmp(ram.words, unchanged_ram.words, sizeof(ram.words)) != 0 ||
	    ram.before != unchanged_ram.before || ram.after != unchanged_ram.after) return false;

	const uint32_t write_header = LUMEN_PM4(4, Pm4::IT_WRITE_CONST_RAM, 0);
	uint32_t write_body[] = {last_offset, source[0], source[1]};
	Pm4ConstRamWritePacket write_decoded;
	if (!pm4_decode_write_const_ram(write_header, write_body, 4, &write_decoded) ||
	    write_decoded.offset != last_offset || write_decoded.count != 2 || write_decoded.source != write_body + 1) return false;
	const auto unchanged_write = write_decoded;
	for (uint32_t available_dw = 0; available_dw < 4; ++available_dw)
	{
		if (pm4_decode_write_const_ram(write_header, nullptr, available_dw, &write_decoded) ||
		    pm4_decode_write_const_ram(write_header, write_body, available_dw, &write_decoded) ||
		    write_decoded.offset != unchanged_write.offset || write_decoded.count != unchanged_write.count ||
		    write_decoded.source != unchanged_write.source) return false;
	}
	uint32_t zero_body[] = {last_offset};
	if (!pm4_decode_write_const_ram(LUMEN_PM4(2, Pm4::IT_WRITE_CONST_RAM, 0), zero_body, 2, &write_decoded) ||
	    write_decoded.count != 0 || write_decoded.offset != last_offset ||
	    const_ram_write(ram.words, write_decoded.offset, write_decoded.source, write_decoded.count) != nullptr ||
	    pm4_decode_write_const_ram(write_header, nullptr, 4, &write_decoded) ||
	    pm4_decode_write_const_ram(write_header, write_body, 4, nullptr)) return false;

	const uint64_t destination_address = reinterpret_cast<uint64_t>(destination.data() + 1);
	uint32_t dump_body[] = {last_offset, 1, static_cast<uint32_t>(destination_address),
	                        static_cast<uint32_t>(destination_address >> 32u)};
	Pm4ConstRamDumpPacket dump_decoded;
	if (!pm4_decode_dump_const_ram(0xC0038300u, dump_body, 5, &dump_decoded) ||
	    dump_decoded.offset != last_offset || dump_decoded.count != 1 ||
	    dump_decoded.destination != destination.data() + 1) return false;
	const auto unchanged_dump = dump_decoded;
	for (uint32_t available_dw = 0; available_dw < 5; ++available_dw)
	{
		if (pm4_decode_dump_const_ram(0xC0038300u, nullptr, available_dw, &dump_decoded) ||
		    pm4_decode_dump_const_ram(0xC0038300u, dump_body, available_dw, &dump_decoded) ||
		    dump_decoded.offset != unchanged_dump.offset || dump_decoded.count != unchanged_dump.count ||
		    dump_decoded.destination != unchanged_dump.destination) return false;
	}
	return !pm4_decode_dump_const_ram(0xC0038300u, nullptr, 5, &dump_decoded) &&
	       !pm4_decode_dump_const_ram(0xC0028300u, dump_body, 5, &dump_decoded) &&
	       !pm4_decode_dump_const_ram(0xC0038300u, dump_body, 5, nullptr);
}

bool GraphicsRunDmaDataGdsReadContractTest()
{
	std::array<uint32_t, 4> sentinels {0x12345678u, 0xa5a5a5a5u, 0xa5a5a5a5u, 0x87654321u};
	const uint64_t destination = reinterpret_cast<uint64_t>(&sentinels[1]);
	std::array<uint32_t, 6> body {0xa4004000u, 49144u, 0u, static_cast<uint32_t>(destination),
	                             static_cast<uint32_t>(destination >> 32u), 8u};
	Pm4DmaDataMemoryCopyPacket decoded;
	if (!pm4_decode_dma_data_gds_read(PM4_DMA_DATA_MEMORY_COPY_HEADER, body.data(), 7, &decoded) ||
	    decoded.source_address != 49144 || decoded.destination_address != destination || decoded.byte_count != 8) return false;
	for (uint32_t count = 0; count != 7; ++count)
		if (pm4_decode_dma_data_gds_read(PM4_DMA_DATA_MEMORY_COPY_HEADER, body.data(), count, &decoded)) return false;
	for (uint32_t bit = 0; bit != 32; ++bit)
	{
		if (pm4_decode_dma_data_gds_read(PM4_DMA_DATA_MEMORY_COPY_HEADER ^ (1u << bit), body.data(), 7, &decoded)) return false;
		auto changed = body;
		changed[0] ^= 1u << bit;
		if (pm4_decode_dma_data_gds_read(PM4_DMA_DATA_MEMORY_COPY_HEADER, changed.data(), 7, &decoded)) return false;
		if (bit >= 21)
		{
			changed = body; changed[5] |= 1u << bit;
			if (pm4_decode_dma_data_gds_read(PM4_DMA_DATA_MEMORY_COPY_HEADER, changed.data(), 7, &decoded)) return false;
		}
	}
	for (uint64_t source: {UINT64_C(1), UINT64_C(49148), UINT64_C(49152), UINT64_C(0x400000000), UINT64_MAX - 3})
	{
		auto changed = body;
		changed[1] = static_cast<uint32_t>(source); changed[2] = static_cast<uint32_t>(source >> 32u);
		if (pm4_decode_dma_data_gds_read(PM4_DMA_DATA_MEMORY_COPY_HEADER, changed.data(), 7, &decoded)) return false;
	}
	for (uint64_t address: {UINT64_C(0), destination + 1, destination + 2, destination + 3, UINT64_MAX - 3})
	{
		auto changed = body;
		changed[3] = static_cast<uint32_t>(address); changed[4] = static_cast<uint32_t>(address >> 32u);
		if (pm4_decode_dma_data_gds_read(PM4_DMA_DATA_MEMORY_COPY_HEADER, changed.data(), 7, &decoded)) return false;
	}
	for (uint32_t size: {1u, 2u, 3u, 12u, 49156u})
	{
		auto changed = body; changed[5] = size;
		if (pm4_decode_dma_data_gds_read(PM4_DMA_DATA_MEMORY_COPY_HEADER, changed.data(), 7, &decoded)) return false;
	}
	// Both packet dispatch entry points preserve byte offset/count and leave
	// memory untouched before the production renderer performs publication.
	Core::ThreadsSubsystem::Instance()->Init(nullptr);
	graphics_init_jmp_tables();
	auto* cp = new CommandProcessor(64); // Production processors have process lifetime.
	cp->SetHeadlessTestMode();
	uint32_t stream[7] = {PM4_DMA_DATA_MEMORY_COPY_HEADER};
	std::copy(body.begin(), body.end(), stream + 1);
	CommandEffectProbe probe;
	probe.owner = cp;
	{
		CommandEffectProbeScope scope(&probe);
		if (cp_op_dma_data(cp, stream[0], stream + 1, 7, 7) != 6) return false;
		cp->Run(stream, 7);
	}
	return !probe.overflow && probe.count == 2 && probe.effects[0] == CommandEffectProbeValue::ReadGds &&
	       probe.effects[1] == CommandEffectProbeValue::ReadGds && probe.destination32 == &sentinels[1] &&
	       probe.source_byte_offset == 49144 && probe.byte_count == 8 &&
	       sentinels == std::array<uint32_t, 4> {0x12345678u, 0xa5a5a5a5u, 0xa5a5a5a5u, 0x87654321u};
}

bool GraphicsRunDmaDataMemoryCopyContractTest()
{
	constexpr uint64_t source_address      = 0x0000000160610730ull;
	constexpr uint64_t destination_address = 0x00000005b0fb9400ull;
	uint32_t body[6] = {
	    PM4_DMA_DATA_MEMORY_COPY_CONTROL,
	    static_cast<uint32_t>(source_address),
	    static_cast<uint32_t>(source_address >> 32u),
	    static_cast<uint32_t>(destination_address),
	    static_cast<uint32_t>(destination_address >> 32u),
	    0x0000015eu,
	};
	Pm4DmaDataMemoryCopyPacket decoded;
	if (!pm4_decode_dma_data_memory_copy(PM4_DMA_DATA_MEMORY_COPY_HEADER, body, 7, &decoded) ||
	    decoded.source_address != source_address || decoded.destination_address != destination_address || decoded.byte_count != 350 ||
	    pm4_decode_dma_data_memory_copy(PM4_DMA_DATA_MEMORY_COPY_HEADER, body, 6, &decoded) ||
	    pm4_decode_dma_data_memory_copy(PM4_DMA_DATA_MEMORY_COPY_HEADER ^ 1u, body, 7, &decoded) ||
	    pm4_decode_dma_data_memory_copy(PM4_DMA_DATA_MEMORY_COPY_HEADER, nullptr, 7, &decoded) ||
	    pm4_decode_dma_data_memory_copy(PM4_DMA_DATA_MEMORY_COPY_HEADER, body, 7, nullptr))
	{
		return false;
	}

	const uint32_t observed_control = body[0];
	for (uint32_t bit = 0; bit < 32; ++bit)
	{
		body[0] = observed_control ^ (1u << bit);
		if (pm4_decode_dma_data_memory_copy(PM4_DMA_DATA_MEMORY_COPY_HEADER, body, 7, &decoded))
		{
			return false;
		}
	}
	body[0] = observed_control;
	const uint32_t observed_command = body[5];
	for (uint32_t bit = 21; bit < 32; ++bit)
	{
		body[5] = observed_command | (1u << bit);
		if (pm4_decode_dma_data_memory_copy(PM4_DMA_DATA_MEMORY_COPY_HEADER, body, 7, &decoded))
		{
			return false;
		}
	}
	body[5] = PM4_DMA_DATA_BYTE_COUNT_MASK;
	if (!pm4_decode_dma_data_memory_copy(PM4_DMA_DATA_MEMORY_COPY_HEADER, body, 7, &decoded) ||
	    decoded.byte_count != PM4_DMA_DATA_BYTE_COUNT_MASK)
	{
		return false;
	}
	body[5] = 0;
	if (!pm4_decode_dma_data_memory_copy(PM4_DMA_DATA_MEMORY_COPY_HEADER, body, 7, &decoded) || decoded.byte_count != 0 ||
	    !dma_data_memory_copy_bytes(0, 0, 0))
	{
		return false;
	}

	std::array<uint8_t, 384> source {};
	std::array<uint8_t, 384> destination {};
	for (size_t index = 0; index < source.size(); ++index)
	{
		source[index] = static_cast<uint8_t>((index * 37u + 11u) & 0xffu);
	}
	destination.fill(0xa5u);
	constexpr size_t source_offset      = 9;
	constexpr size_t destination_offset = 17;
	constexpr uint32_t copy_size        = 350;
	if (!dma_data_memory_copy_bytes(reinterpret_cast<uint64_t>(source.data() + source_offset),
	                                reinterpret_cast<uint64_t>(destination.data() + destination_offset), copy_size))
	{
		return false;
	}
	for (size_t index = 0; index < destination.size(); ++index)
	{
		const bool in_copy = index >= destination_offset && index < destination_offset + copy_size;
		const uint8_t expected = in_copy ? source[source_offset + index - destination_offset] : 0xa5u;
		if (destination[index] != expected)
		{
			return false;
		}
	}

	std::array<uint8_t, 32> overlap {};
	const uint64_t overlap_base = reinterpret_cast<uint64_t>(overlap.data());
	if (dma_data_memory_copy_ranges_supported(0, overlap_base, 1) || dma_data_memory_copy_ranges_supported(overlap_base, 0, 1) ||
	    dma_data_memory_copy_ranges_supported(UINT64_MAX - 3u, overlap_base, 4) ||
	    dma_data_memory_copy_ranges_supported(overlap_base, UINT64_MAX - 3u, 4) ||
	    dma_data_memory_copy_ranges_supported(overlap_base, overlap_base, 1) ||
	    dma_data_memory_copy_ranges_supported(overlap_base, overlap_base + 1u, 8) ||
	    dma_data_memory_copy_ranges_supported(overlap_base + 1u, overlap_base, 8))
	{
		return false;
	}

	uint32_t root_packet[7] = {PM4_DMA_DATA_MEMORY_COPY_HEADER, observed_control,
	                           static_cast<uint32_t>(source_address), static_cast<uint32_t>(source_address >> 32u),
	                           static_cast<uint32_t>(destination_address), static_cast<uint32_t>(destination_address >> 32u),
	                           observed_command};
	return root_command_ownership_decision(root_packet, static_cast<uint32_t>(std::size(root_packet)), nullptr, 0, true) ==
	       RootCommandOwnershipDecision::Unsupported;
}

static bool graphics_run_tracked_submission_timeline_test(CommandProcessor* cp)
{
	if (cp == nullptr)
	{
		return false;
	}
	CommandProcessorStateLock state_lock(cp);
	uint32_t submitted_cases = 0;
	const auto expect_submitted = [&](CommandProcessor* processor, int queue, uint64_t epoch,
	                                  uint64_t serial, bool expected) {
		++submitted_cases;
		return GraphicsRunCommandProcessorSubmitted(processor, queue, epoch, serial) == expected;
	};
	cp->SetQueue(GraphicContext::QUEUE_GFX);
	cp->ResetTrackedCommandBufferSubmissionTimeline();

	uint64_t first_epoch      = 0;
	uint64_t first_serial     = 0;
	uint64_t submitted_cutoff = UINT64_MAX;
	uint64_t completed_cutoff = UINT64_MAX;
	if (!GraphicsRunBeginTrackedCommandBufferSubmission(cp, GraphicContext::QUEUE_GFX, &first_epoch, &first_serial,
	                                                    &submitted_cutoff, &completed_cutoff) ||
	    first_epoch == 0 || first_serial != 1 || submitted_cutoff != 0 || completed_cutoff != 0)
	{
		return false;
	}
	if (GraphicsRunCommandProcessorCompleted(cp, GraphicContext::QUEUE_GFX, first_epoch, first_serial) ||
	    GraphicsRunCommandProcessorCompleted(nullptr, GraphicContext::QUEUE_GFX, first_epoch, first_serial) ||
	    !expect_submitted(cp, GraphicContext::QUEUE_GFX, first_epoch, first_serial, false) ||
	    !expect_submitted(nullptr, GraphicContext::QUEUE_GFX, first_epoch, first_serial, false)) return false;
	// Completion of a merely assigned recording cannot create submission proof.
	GraphicsRunRecordTrackedCommandBufferCompletion(cp, GraphicContext::QUEUE_GFX, first_epoch, first_serial);
	if (!expect_submitted(cp, GraphicContext::QUEUE_GFX, first_epoch, first_serial, false) ||
	    GraphicsRunCommandProcessorCompleted(cp, GraphicContext::QUEUE_GFX, first_epoch, first_serial)) return false;

	uint64_t rejected_epoch     = 1;
	uint64_t rejected_serial    = 1;
	uint64_t rejected_submitted = 1;
	uint64_t rejected_completed = 1;
	if (GraphicsRunBeginTrackedCommandBufferSubmission(cp, GraphicContext::QUEUE_GFX + 1, &rejected_epoch,
	                                                   &rejected_serial, &rejected_submitted, &rejected_completed) ||
	    rejected_epoch != 0 || rejected_serial != 0 || rejected_submitted != 0 || rejected_completed != 0 ||
	    !GraphicsRunRecordTrackedCommandBufferSubmission(cp, GraphicContext::QUEUE_GFX, first_epoch, first_serial))
	{
		return false;
	}
	if (GraphicsRunCommandProcessorCompleted(cp, GraphicContext::QUEUE_GFX, first_epoch, first_serial) ||
	    !expect_submitted(cp, GraphicContext::QUEUE_GFX, first_epoch, first_serial, true)) return false;
	for (const auto& invalid: {std::tuple {GraphicContext::QUEUE_GFX + 1, first_epoch, first_serial},
	                          std::tuple {-1, first_epoch, first_serial},
	                          std::tuple {GraphicContext::QUEUES_NUM, first_epoch, first_serial},
	                          std::tuple {GraphicContext::QUEUE_GFX, first_epoch + 1u, first_serial},
	                          std::tuple {GraphicContext::QUEUE_GFX, uint64_t {0}, first_serial},
	                          std::tuple {GraphicContext::QUEUE_GFX, first_epoch, uint64_t {0}},
	                          std::tuple {GraphicContext::QUEUE_GFX, first_epoch, first_serial + 1u},
	                          std::tuple {GraphicContext::QUEUE_GFX, first_epoch, uint64_t {UINT64_MAX}}})
		if (!expect_submitted(cp, std::get<0>(invalid), std::get<1>(invalid), std::get<2>(invalid), false)) return false;
	// Rejected queries do not poison or advance the valid native prefix.
	if (!expect_submitted(cp, GraphicContext::QUEUE_GFX, first_epoch, first_serial, true) ||
	    GraphicsRunCommandProcessorCompleted(cp, GraphicContext::QUEUE_GFX, first_epoch, first_serial)) return false;
	GraphicsRunRecordTrackedCommandBufferCompletion(cp, GraphicContext::QUEUE_GFX, first_epoch, first_serial);
	if (!GraphicsRunCommandProcessorCompleted(cp, GraphicContext::QUEUE_GFX, first_epoch, first_serial) ||
	    GraphicsRunCommandProcessorCompleted(cp, GraphicContext::QUEUE_GFX + 1, first_epoch, first_serial) ||
	    GraphicsRunCommandProcessorCompleted(cp, -1, first_epoch, first_serial) ||
	    GraphicsRunCommandProcessorCompleted(cp, GraphicContext::QUEUE_GFX, first_epoch + 1, first_serial) ||
	    GraphicsRunCommandProcessorCompleted(cp, GraphicContext::QUEUE_GFX, 0, first_serial) ||
	    GraphicsRunCommandProcessorCompleted(cp, GraphicContext::QUEUE_GFX, first_epoch, 0) ||
	    GraphicsRunCommandProcessorCompleted(cp, GraphicContext::QUEUE_GFX, first_epoch, first_serial + 1) ||
	    !expect_submitted(cp, GraphicContext::QUEUE_GFX, first_epoch, first_serial, true)) return false;

	uint64_t second_epoch     = 0;
	uint64_t second_serial    = 0;
	uint64_t second_submitted = 0;
	uint64_t second_completed = 0;
	if (!GraphicsRunBeginTrackedCommandBufferSubmission(cp, GraphicContext::QUEUE_GFX, &second_epoch, &second_serial,
	                                                    &second_submitted, &second_completed) ||
	    second_epoch != first_epoch || second_serial != 2 || second_submitted != 1 || second_completed != 1 ||
	    !expect_submitted(cp, GraphicContext::QUEUE_GFX, first_epoch, first_serial, true) ||
	    !expect_submitted(cp, GraphicContext::QUEUE_GFX, second_epoch, second_serial, false) ||
	    !GraphicsRunRecordTrackedCommandBufferSubmission(cp, GraphicContext::QUEUE_GFX, second_epoch, second_serial))
	{
		return false;
	}
	if (GraphicsRunCommandProcessorCompleted(cp, GraphicContext::QUEUE_GFX, second_epoch, second_serial) ||
	    !expect_submitted(cp, GraphicContext::QUEUE_GFX, second_epoch, second_serial, true)) return false;
	GraphicsRunRecordTrackedCommandBufferCompletion(cp, GraphicContext::QUEUE_GFX, second_epoch, second_serial);
	// A later wait can complete work after Begin captured its immutable cutoff.
	if (second_completed != 1 ||
	    !GraphicsRunCommandProcessorCompleted(cp, GraphicContext::QUEUE_GFX, second_epoch, second_serial) ||
	    !expect_submitted(cp, GraphicContext::QUEUE_GFX, second_epoch, second_serial, true)) return false;

	// A second Begin without submitting the preceding recording creates a gap.
	// It must poison only this diagnostic epoch, then recover at session reset.
	uint64_t gap_epoch     = 0;
	uint64_t gap_serial    = 0;
	uint64_t gap_submitted = 0;
	uint64_t gap_completed = 0;
	if (!GraphicsRunBeginTrackedCommandBufferSubmission(cp, GraphicContext::QUEUE_GFX, &gap_epoch, &gap_serial,
	                                                    &gap_submitted, &gap_completed) ||
	    gap_epoch != first_epoch || gap_serial != 3 || gap_submitted != 2 || gap_completed != 2 ||
	    !expect_submitted(cp, GraphicContext::QUEUE_GFX, gap_epoch, gap_serial, false) ||
	    !expect_submitted(cp, GraphicContext::QUEUE_GFX, second_epoch, second_serial, true) ||
	    GraphicsRunBeginTrackedCommandBufferSubmission(cp, GraphicContext::QUEUE_GFX, &rejected_epoch, &rejected_serial,
	                                                   &rejected_submitted, &rejected_completed) ||
	    GraphicsRunRecordTrackedCommandBufferSubmission(cp, GraphicContext::QUEUE_GFX, gap_epoch, gap_serial))
	{
		return false;
	}
	if (GraphicsRunCommandProcessorCompleted(cp, GraphicContext::QUEUE_GFX, first_epoch, first_serial) ||
	    !expect_submitted(cp, GraphicContext::QUEUE_GFX, first_epoch, first_serial, false) ||
	    !expect_submitted(cp, GraphicContext::QUEUE_GFX, gap_epoch, gap_serial, false)) return false;

	cp->ResetTrackedCommandBufferSubmissionTimeline();
	if (GraphicsRunCommandProcessorCompleted(cp, GraphicContext::QUEUE_GFX, first_epoch, first_serial) ||
	    !expect_submitted(cp, GraphicContext::QUEUE_GFX, first_epoch, first_serial, false)) return false;
	uint64_t reset_epoch     = 0;
	uint64_t reset_serial    = 0;
	uint64_t reset_submitted = UINT64_MAX;
	uint64_t reset_completed = UINT64_MAX;
	if (!GraphicsRunBeginTrackedCommandBufferSubmission(cp, GraphicContext::QUEUE_GFX, &reset_epoch, &reset_serial,
	                                                    &reset_submitted, &reset_completed) ||
	    reset_epoch != first_epoch + 1u || reset_serial != 1 || reset_submitted != 0 || reset_completed != 0 ||
	    !expect_submitted(cp, GraphicContext::QUEUE_GFX, reset_epoch, reset_serial, false) ||
	    GraphicsRunRecordTrackedCommandBufferSubmission(cp, GraphicContext::QUEUE_GFX, reset_epoch + 1u, reset_serial) ||
	    GraphicsRunRecordTrackedCommandBufferSubmission(cp, GraphicContext::QUEUE_GFX, reset_epoch, reset_serial) ||
	    !expect_submitted(cp, GraphicContext::QUEUE_GFX, reset_epoch, reset_serial, false))
	{
		return false;
	}

	cp->ResetTrackedCommandBufferSubmissionTimeline();
	uint64_t final_epoch     = 0;
	uint64_t final_serial    = 0;
	uint64_t final_submitted = UINT64_MAX;
	uint64_t final_completed = UINT64_MAX;
	if (!GraphicsRunBeginTrackedCommandBufferSubmission(cp, GraphicContext::QUEUE_GFX, &final_epoch, &final_serial,
	                                                   &final_submitted, &final_completed) ||
	    final_epoch != reset_epoch + 1u || final_serial != 1 || final_submitted != 0 || final_completed != 0 ||
	    !expect_submitted(cp, GraphicContext::QUEUE_GFX, final_epoch, final_serial, false) ||
	    !GraphicsRunRecordTrackedCommandBufferSubmission(cp, GraphicContext::QUEUE_GFX, final_epoch, final_serial) ||
	    !expect_submitted(cp, GraphicContext::QUEUE_GFX, final_epoch, final_serial, true) ||
	    GraphicsRunCommandProcessorCompleted(cp, GraphicContext::QUEUE_GFX, final_epoch, final_serial)) return false;
	std::fprintf(stderr, "[GFX_SUBMITTED_PREFIX_TEST] cases=%u assigned-only=reject successful-prefix/incomplete=distinct"
	    " future/queue/epoch/reset/gap/poison=reject query-no-effect=pass processor=real native-submission-success=modeled"
	    " native-GPU-execution=not-exercised\n", submitted_cases);
	return true;
}

static bool graphics_run_blend_color_test(CommandProcessor* cp)
{
	auto* ctx = cp->GetCtx();
	const HW::Context saved = *ctx;
	const auto words = [](const HW::Context& value) {
		const auto& color = value.GetBlendColor();
		const float components[] = {color.red, color.green, color.blue, color.alpha};
		std::array<uint32_t, 4> result;
		std::memcpy(result.data(), components, sizeof(components));
		return result;
	};
	const bool result = [&]() {
		ctx->Reset();
		if (words(*ctx) != std::array<uint32_t, 4> {}) return false;
		ctx->SetRenderTargetMask(0xa5a5u);
		const uint32_t packet[] = {Pm4::CB_BLEND_RED, 0x3e800000u, 0x3f000000u, 0x3f400000u, 0x3f800000u};
		const std::array<uint32_t, 4> expected = {packet[1], packet[2], packet[3], packet[4]};
		if (cp_op_set_context_reg(cp, LUMEN_PM4(6, Pm4::IT_SET_CONTEXT_REG, 0), packet, 6, 6) != 5 ||
		    words(*ctx) != expected || ctx->GetRenderTargetMask() != 0xa5a5u) return false;
		const HW::Context copy = *ctx;
		// Parsing preserves every bit; range/nonfinite admission happens at draw.
		const uint32_t raw[] = {0x80000000u, 0x7fc01234u, 0x7f800000u, 0xbf800000u};
		if (hw_ctx_update_blend_color(ctx, LUMEN_PM4(6, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::CB_BLEND_RED, raw, 7) != 4 ||
		    words(*ctx) != std::array<uint32_t, 4> {raw[0], raw[1], raw[2], raw[3]} || words(copy) != expected) return false;
		const auto before = words(*ctx);
		struct Invalid { uint32_t header; uint32_t offset; uint32_t available; };
		const Invalid invalid[] = {
		    {LUMEN_PM4(5, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::CB_BLEND_RED, 6},
		    {LUMEN_PM4(7, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::CB_BLEND_RED, 7},
		    {LUMEN_PM4(6, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::CB_BLEND_GREEN, 6},
		    {LUMEN_PM4(6, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::CB_BLEND_RED - 1, 6},
		    {LUMEN_PM4(6, Pm4::IT_SET_SH_REG, 0), Pm4::CB_BLEND_RED, 6},
		    {LUMEN_PM4(6, Pm4::IT_SET_CONTEXT_REG, 0) | 1u, Pm4::CB_BLEND_RED, 6},
		};
		for (const auto& bad: invalid)
			if (hw_ctx_update_blend_color(ctx, bad.header, bad.offset, raw, bad.available) != 0 || words(*ctx) != before)
				return false;
		for (uint32_t available = 0; available < 6; ++available)
			if (hw_ctx_update_blend_color(ctx, LUMEN_PM4(6, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::CB_BLEND_RED, raw, available) != 0 ||
			    words(*ctx) != before) return false;
		if (hw_ctx_update_blend_color(ctx, LUMEN_PM4(6, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::CB_BLEND_RED, nullptr, 6) != 0 ||
		    hw_ctx_update_blend_color(nullptr, LUMEN_PM4(6, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::CB_BLEND_RED, raw, 6) != 0 ||
		    words(*ctx) != before) return false;
		uint32_t clear = 0;
		return cp_op_clear_state(cp, LUMEN_PM4(2, Pm4::IT_CLEAR_STATE, 0), &clear, 2, 2) == 1 &&
		       words(*ctx) == std::array<uint32_t, 4> {} && words(copy) == expected;
	}();
	*ctx = saved;
	return result;
}

static bool graphics_run_polygon_offset_test(CommandProcessor* cp)
{
	auto* ctx = cp->GetCtx();
	const HW::Context saved = *ctx;
	const auto reset_matches = [](const HW::Context& value) {
		const auto& registers = value.GetPolygonOffsetRegisters();
		if (registers.written_mask != 0 || registers.mode_control_written) return false;
		for (uint32_t word: registers.values) if (word != 0) return false;
		return true;
	};
	ctx->Reset();
	if (!reset_matches(*ctx)) return false;
	for (uint32_t first = 0; first < 6; ++first)
	{
		for (uint32_t count = 1; count <= 6 - first; ++count)
		{
			ctx->Reset();
			ctx->SetRenderTargetMask(0xa5a5u);
			uint32_t packet[7] = {Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL + first};
			for (uint32_t index = 0; index < count; ++index) packet[index + 1] = 0x3f800000u + index;
			const uint32_t dw = count + 2;
			if (cp_op_set_context_reg(cp, LUMEN_PM4(dw, Pm4::IT_SET_CONTEXT_REG, 0), packet, dw, dw) != count + 1 ||
			    ctx->GetRenderTargetMask() != 0xa5a5u) return false;
			const auto& registers = ctx->GetPolygonOffsetRegisters();
			if (registers.written_mask != (((1u << count) - 1u) << first)) return false;
			for (uint32_t index = 0; index < 6; ++index)
			{
				const uint32_t expected = index >= first && index < first + count ? packet[index - first + 1] : 0;
				if (registers.values[index] != expected) return false;
			}
		}
	}
	HW::Context local;
	local.SetPolygonOffsetRegister(2, 0x41000000u);
	const auto before = local.GetPolygonOffsetRegisters();
	const uint32_t words[6] = {1, 2, 3, 4, 5, 6};
	struct Invalid { uint32_t header; uint32_t offset; uint32_t available; };
	const Invalid invalid[] = {
	    {LUMEN_PM4(2, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL, 2},
	    {LUMEN_PM4(8, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::PA_SU_POLY_OFFSET_CLAMP, 8},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL - 1, 3},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::PA_SU_POLY_OFFSET_BACK_OFFSET + 1, 3},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0), Pm4::PA_SU_POLY_OFFSET_CLAMP, 2},
	    {LUMEN_PM4(3, Pm4::IT_SET_SH_REG, 0), Pm4::PA_SU_POLY_OFFSET_CLAMP, 3},
	    {LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0) | 1u, Pm4::PA_SU_POLY_OFFSET_CLAMP, 3},
	};
	for (const auto& bad: invalid)
	{
		if (hw_ctx_update_polygon_offset(&local, bad.header, bad.offset, words, bad.available) != 0 ||
		    local.GetPolygonOffsetRegisters().written_mask != before.written_mask ||
		    std::memcmp(local.GetPolygonOffsetRegisters().values, before.values, sizeof(before.values)) != 0) return false;
	}
	if (hw_ctx_update_polygon_offset(&local, LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0),
	                                 Pm4::PA_SU_POLY_OFFSET_CLAMP, nullptr, 3) != 0) return false;
	ctx->Reset();
	uint32_t pairs[12];
	for (uint32_t index = 0; index < 6; ++index)
	{
		pairs[index * 2] = Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL + index;
		pairs[index * 2 + 1] = words[index];
	}
	const uint64_t address = reinterpret_cast<uint64_t>(pairs);
	const uint32_t indirect[] = {6, static_cast<uint32_t>(address), static_cast<uint32_t>(address >> 32u)};
	if (cp_op_indirect_cx_regs(cp, 0xc0021048u, indirect, 4, 4) != 3 ||
	    ctx->GetPolygonOffsetRegisters().written_mask != 0x3fu ||
	    std::memcmp(ctx->GetPolygonOffsetRegisters().values, words, sizeof(words)) != 0) return false;
	const HW::Context copy = *ctx;
	ctx->SetPolygonOffsetRegister(3, 0);
	if (copy.GetPolygonOffsetRegisters().values[3] != words[3] || copy.GetPolygonOffsetRegisters().written_mask != 0x3fu) return false;
	uint32_t clear = 0;
	if (cp_op_clear_state(cp, LUMEN_PM4(2, Pm4::IT_CLEAR_STATE, 0), &clear, 2, 2) != 1 || !reset_matches(*ctx)) return false;
	*ctx = saved;
	return true;
}

static bool graphics_run_experimental_hle_d32_default_test(CommandProcessor* cp)
{
	const auto format_matches = [](const HW::Context& ctx, uint32_t format, uint8_t mask) {
		const auto& registers = ctx.GetPolygonOffsetRegisters();
		if (registers.values[0] != format || registers.written_mask != mask ||
		    registers.mode_control_raw != 0 || registers.mode_control_written) return false;
		for (uint32_t index = 1; index < 6; ++index) if (registers.values[index] != 0) return false;
		return true;
	};
	if (hle_draw_reset_polygon_offset_format(false) != 0 || hle_draw_reset_polygon_offset_format(true) != 0x1e9u) return false;
	const char* disabled_values[] = {nullptr, "", "0", "01", "10", "true", " 1", "1 ", "+1"};
	for (const char* value: disabled_values) if (exact_one_environment_value(value)) return false;
	if (!exact_one_environment_value("1")) return false;
	for (bool default_enabled: {false, true})
	{
		if (hle_d32_default_environment_value(nullptr, default_enabled) != default_enabled ||
		    !hle_d32_default_environment_value("1", default_enabled)) return false;
		for (const char* value: disabled_values)
			if (value != nullptr && hle_d32_default_environment_value(value, default_enabled)) return false;
	}
	for (bool enabled: {false, true})
	{
		HW::Context local;
		if (!format_matches(local, 0, 0)) return false;
		apply_hle_draw_reset_polygon_offset_format(&local, enabled);
		if (!format_matches(local, enabled ? 0x1e9u : 0u, enabled ? 1u : 0u)) return false;
		for (uint32_t explicit_format: {0u, 0x123u})
		{
			if (hw_ctx_update_polygon_offset(&local, LUMEN_PM4(3, Pm4::IT_SET_CONTEXT_REG, 0),
			                                 Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL, &explicit_format, 3) != 1 ||
			    !format_matches(local, explicit_format, 1)) return false;
			local.Reset();
			if (!format_matches(local, 0, 0)) return false;
			apply_hle_draw_reset_polygon_offset_format(&local, enabled);
			if (!format_matches(local, enabled ? 0x1e9u : 0u, enabled ? 1u : 0u)) return false;
		}
	}
	auto* ctx = cp->GetCtx();
	const HW::Context saved = *ctx;
	const bool result = [&]() {
		uint32_t payload = 0;
		const bool enabled = hle_d32_default_environment_value(std::getenv("LUMEN_EXPERIMENTAL_GFX_HLE_D32_DEFAULT"),
		                                                      hle_d32_default_for_target());
		if (experimental_hle_d32_default_enabled() != enabled) return false;
		if (cp_op_draw_reset(cp, 0xc0001014u, &payload, 2, 2) != 1 ||
		    !format_matches(*ctx, enabled ? 0x1e9u : 0u, enabled ? 1u : 0u)) return false;
		if (cp_op_clear_state(cp, LUMEN_PM4(2, Pm4::IT_CLEAR_STATE, 0), &payload, 2, 2) != 1 ||
		    !format_matches(*ctx, 0, 0)) return false;
		ctx->SetPolygonOffsetRegister(0, 0x1e9u);
		return cp_op_dispatch_reset(cp, 0xc0001024u, &payload, 2, 2) == 1 && format_matches(*ctx, 0, 0);
	}();
	*ctx = saved;
	return result;
}

static bool graphics_run_exclusive_recording_test(CommandProcessor* cp)
{
	// Headless fixtures cannot begin a Vulkan recording. These opaque keys
	// exercise the production predicate; the public bridge also rejects a
	// foreign pointer before any attempted CommandBuffer access.
	int buffer_key = 0, foreign_key = 0, pool_key = 0;
	if (GraphicsRunCommandProcessorExclusiveRecordingValid(nullptr, nullptr) ||
	    GraphicsRunCommandProcessorExclusiveRecordingValid(cp, nullptr) ||
	    GraphicsRunCommandProcessorExclusiveRecordingValid(cp, reinterpret_cast<CommandBuffer*>(&buffer_key))) return false;
	EopInitialTargetIdentity active;
	active.processor = active.parent = cp; active.buffer = &buffer_key; active.pool = &pool_key;
	active.current_index = 0; active.pool_index = 0;
	active.processor_queue = active.buffer_queue = GraphicContext::QUEUE_GFX;
	active.processor_epoch = active.session = 3;
	active.assigned_serial = active.serial = 5; active.recording = 7;
	active.submitted_cutoff = 4; active.completed_cutoff = 2;
	active.exclusive = active.valid = true;
	if (!ExclusiveGraphicsRecordingIdentityValid(active, cp, &buffer_key) ||
	    ExclusiveGraphicsRecordingIdentityValid(active, cp, &foreign_key)) return false;
	for (uint32_t scenario = 0; scenario != 10; ++scenario)
	{
		auto changed = active;
		switch (scenario)
		{
			case 0: changed.exclusive = false; break;
			case 1: changed.buffer = &foreign_key; break;
			case 2: changed.parent = &foreign_key; break;
			case 3: changed.processor_queue = changed.buffer_queue = GraphicContext::QUEUE_COMPUTE_START; break;
			case 4: ++changed.processor_epoch; break;
			case 5: ++changed.assigned_serial; break;
			case 6: changed.recording = 0; break;
			case 7: changed.valid = false; break;
			case 8: changed.pool = nullptr; break;
			case 9: changed.submitted_cutoff = changed.serial; break;
		}
		if (ExclusiveGraphicsRecordingIdentityValid(changed, cp, &buffer_key)) return false;
		unsigned captures = 0, pauses = 0;
		auto live = active;
		const auto retried = CaptureShaderSourceWithRetry([&](bool* retryable) {
			++captures; *retryable = true; return false;
		}, []() -> const char* { return nullptr; }, [&] { ++pauses; live = changed; },
		    [&]() -> const char* { return active.Matches(live) ? nullptr : "source-recording-changed"; });
		if (retried.captured || retried.reason == nullptr || std::strcmp(retried.reason, "source-recording-changed") != 0 ||
		    captures != 1 || pauses != 1) return false;
	}
	std::fprintf(stderr, "[GFX_EXCLUSIVE_RECORDING_TEST] active-unsent=admitted completion=not-required"
	                     " nonexclusive/wrong-buffer/foreign-parent/compute/stale-session/stale-serial/inactive/pool/timeline=reject"
	                     " bridge-foreign-pointer=dereference-free\n");
	return true;
}

bool GraphicsRunDeferredLabelCaptureScopeContractTest()
{
	int processor_key = 0, buffer_key = 0, pool_key = 0, foreign_key = 0;
	for (const bool exclusive: {false, true})
	{
		EopInitialTargetIdentity active;
		active.processor = active.parent = &processor_key;
		active.buffer = &buffer_key; active.pool = &pool_key;
		active.current_index = 0; active.pool_index = 0;
		active.processor_queue = active.buffer_queue = GraphicContext::QUEUE_GFX;
		active.processor_epoch = active.session = 3;
		active.assigned_serial = active.serial = 5; active.recording = 7;
		active.submitted_cutoff = 4; active.completed_cutoff = 2;
		active.exclusive = exclusive; active.valid = true;
		if (!DeferredLabelCaptureScopeQualified(active, &processor_key, true, true) ||
		    DeferredLabelCaptureScopeQualified(active, &processor_key, false, true) ||
		    DeferredLabelCaptureScopeQualified(active, &processor_key, true, false) ||
		    DeferredLabelCaptureScopeQualified(active, &foreign_key, true, true) ||
		    ScalarLabelCaptureScopeQualified(active, &processor_key, true, true) != exclusive ||
		    ExclusiveGraphicsRecordingIdentityValid(active, &processor_key, &buffer_key) != exclusive) return false;
		for (uint32_t field = 0; field != 10; ++field)
		{
			auto changed = active;
			switch (field)
			{
				case 0: changed.processor = nullptr; break;
				case 1: changed.buffer = nullptr; break;
				case 2: changed.parent = &foreign_key; break;
				case 3: changed.processor_queue = changed.buffer_queue = GraphicContext::QUEUE_COMPUTE_START; break;
				case 4: ++changed.processor_epoch; break;
				case 5: ++changed.assigned_serial; break;
				case 6: changed.recording = 0; break;
				case 7: changed.valid = false; break;
				case 8: changed.pool = nullptr; break;
				case 9: changed.submitted_cutoff = changed.serial; break;
			}
			if (DeferredLabelCaptureScopeQualified(changed, &processor_key, true, true)) return false;
		}
	}
	std::fprintf(stderr, "[GFX_LABEL_CAPTURE_SCOPE_TEST] shared/exclusive-parser=pass RunLock/parser/owner/recording=required"
	    " EOP/scalar-exclusive-authority=unchanged identity=modeled\n");
	return true;
}

static bool graphics_run_eop_initial_target_capture_test(CommandProcessor* cp)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	Init();
	// Headless tests cannot begin a Vulkan recording. Exercise the production
	// equality predicate for every identity component with opaque fixture keys;
	// the borrow, CP locking, RunLock exclusion and VM mutations below are real.
	int buffer_key = 0, parent_key = 0, pool_key = 0;
	EopInitialTargetIdentity frozen;
	frozen.processor = cp; frozen.buffer = &buffer_key; frozen.parent = cp; frozen.pool = &pool_key;
	frozen.current_index = 0; frozen.processor_queue = frozen.buffer_queue = GraphicContext::QUEUE_GFX;
	frozen.pool_index = 0; frozen.submit_id = 9; frozen.processor_epoch = frozen.session = 3;
	frozen.assigned_serial = frozen.serial = 5; frozen.recording = 7; frozen.ordinal = 11;
	frozen.submitted_cutoff = 4; frozen.completed_cutoff = 2; frozen.exclusive = frozen.valid = true;
	for (const bool run_held: {false, true})
	for (const bool parser_owned: {false, true})
	{
		if (ScalarLabelCaptureScopeQualified(frozen, cp, run_held, parser_owned) != (run_held && parser_owned)) return false;
		auto foreign = frozen; foreign.exclusive = false;
		if (ScalarLabelCaptureScopeQualified(foreign, cp, run_held, parser_owned)) return false;
		foreign = frozen; foreign.processor_queue = foreign.buffer_queue = GraphicContext::QUEUE_COMPUTE_START;
		if (ScalarLabelCaptureScopeQualified(foreign, cp, run_held, parser_owned)) return false;
	}
	for (unsigned field = 0; field < 19; ++field)
	{
		auto changed = frozen;
		switch (field)
		{
			case 0: changed.processor = &parent_key; break;
			case 1: changed.buffer = &parent_key; break;
			case 2: changed.parent = &parent_key; break;
			case 3: changed.pool = &parent_key; break;
			case 4: ++changed.current_index; break;
			case 5: ++changed.processor_queue; break;
			case 6: ++changed.buffer_queue; break;
			case 7: ++changed.pool_index; break;
			case 8: ++changed.submit_id; break;
			case 9: ++changed.processor_epoch; break;
			case 10: ++changed.assigned_serial; break;
			case 11: ++changed.recording; break;
			case 12: ++changed.session; break;
			case 13: ++changed.serial; break;
			case 14: ++changed.ordinal; break;
			case 15: ++changed.submitted_cutoff; break;
			case 16: ++changed.completed_cutoff; break;
			case 17: changed.exclusive = false; break;
			case 18: changed.valid = false; break;
		}
		if (!frozen.Matches(frozen) || frozen.Matches(changed) || changed.Matches(frozen)) return false;
	}
	enum class Scenario { MutationAbort, Exhaust, ProtectRevert, Remap, Unrelated, Queue, Epoch, Recording,
	                      Exclusive, HeldLease, Shape, Execute, KnownRights, ShortExhaust, UnknownReason, TokenUnavailable,
	                      InitialNonexclusive };
	for (const bool validation_only: {false, true})
	for (const uint64_t bytes: {uint64_t {4}, uint64_t {8}})
	for (const auto scenario: {Scenario::MutationAbort, Scenario::Exhaust, Scenario::ProtectRevert, Scenario::Remap,
	                          Scenario::Unrelated, Scenario::Queue, Scenario::Epoch, Scenario::Recording, Scenario::Exclusive,
	                          Scenario::HeldLease, Scenario::Shape, Scenario::Execute, Scenario::KnownRights,
	                          Scenario::ShortExhaust, Scenario::UnknownReason, Scenario::TokenUnavailable, Scenario::InitialNonexclusive})
	{
		const uint64_t owned = Alloc(0, 65536, Mode::ReadWrite);
		if (owned == 0) return false;
		struct Owner { uint64_t address; ~Owner() { Free(address); } } owner {owned};
		std::memset(reinterpret_cast<void*>(owned), 0xa5, 65536);
		GuestWriteDestination destination;
		const uint64_t captured_generation = GetMutationGeneration();
		// The second mode validates an already captured, immutable ticket. A
		// malformed ticket stays empty; no retry may repair it with Capture.
		if (validation_only && scenario != Scenario::Shape &&
		    !destination.Capture(owned + 128, bytes, bytes, nullptr, captured_generation)) return false;
		const uint64_t captured_address = destination.Address(), captured_size = destination.Size();
		if (scenario == Scenario::Execute && !Protect(owned, 65536, Mode::ExecuteReadWrite)) return false;
		const uint64_t generation = scenario == Scenario::TokenUnavailable ? UINT64_MAX :
		    validation_only ? captured_generation : GetMutationGeneration();
		struct ExclusiveProcessor
		{
			CommandProcessor* cp;
			explicit ExclusiveProcessor(CommandProcessor* value): cp(value) { cp->RunLock(); cp->Lock(); }
			~ExclusiveProcessor() { cp->Unlock(); cp->RunUnlock(); }
		} exclusive(cp);
		cp->SetQueue(GraphicContext::QUEUE_GFX);
		cp->SetEopCheckpointExclusiveStream(true);
		cp->ResetTrackedCommandBufferSubmissionTimeline();
		uint64_t epoch = 0, serial = 0, submitted = 0, completed = 0;
		if (!cp->BeginTrackedCommandBufferSubmission(GraphicContext::QUEUE_GFX, &epoch, &serial, &submitted, &completed)) return false;
		auto current = frozen;
		if (scenario == Scenario::InitialNonexclusive)
		{
			cp->SetEopCheckpointExclusiveStream(false);
			current.exclusive = false;
		}
		GuestDataLease held;
		if (scenario == Scenario::HeldLease && !TryAcquireGuestDataLease(owned, 8, Mode::Read, &held, false)) return false;
		ReservationHandle reservation;
		MutationTransaction mutation;
		const bool deferred = scenario <= Scenario::Exclusive;
		if (deferred && !BeginReserveAligned(0, 65536, 65536, &reservation, &mutation)) return false;
		uint32_t calls = 0, waits = 0, flushes = 0, gpu_proofs = 0, callbacks = 0, stores = 0;
		bool valid = true;
		const uint64_t address = owned + 128 + (scenario == Scenario::Shape ? 1u : 0u);
		const auto result = EopCaptureInitialTargetWithLeaseRetry(cp, generation,
		    [&](uint64_t original, GraphicsGuestLeaseResult* acquisition) {
			    ++calls;
			    valid = valid && original == generation && flushes == 0 && gpu_proofs == 0 && callbacks == 0 && stores == 0;
			    if (scenario == Scenario::KnownRights || scenario == Scenario::ShortExhaust || scenario == Scenario::UnknownReason)
			    {
				    *acquisition = {scenario == Scenario::KnownRights ? "destination-protection" :
				        scenario == Scenario::ShortExhaust ? "lease-acquisition-retry-exhausted" : nullptr,
				        generation, scenario == Scenario::KnownRights ? 1u : scenario == Scenario::ShortExhaust ? 15u : 16u};
				    return false;
			    }
			    return validation_only ? destination.Validate(acquisition) : destination.Capture(address, bytes, bytes, acquisition, original);
		    }, [&]() -> const char* {
			    return frozen.Matches(current) ? nullptr : "recording-changed-during-target-capture";
		    }, [&] {
			    ++waits;
			    valid = valid && !CurrentThreadHasMemoryLease();
			    if (scenario != Scenario::Exhaust && (scenario != Scenario::MutationAbort || waits == 2))
				    valid = mutation.Abort() && valid;
			    std::thread worker([&] {
				    const bool processor_available = cp->TryLock();
				    const bool run_available = cp->TryRunLockForTest();
				    valid = valid && processor_available && !run_available;
				    if (run_available) cp->RunUnlock();
				    if (processor_available)
				    {
					    if (scenario == Scenario::ProtectRevert)
						    valid = Protect(owned, 65536, Mode::Read) && Protect(owned, 65536, Mode::ReadWrite) && valid;
					    if (scenario == Scenario::Remap)
					    {
						    valid = Free(owned) && AllocFixed(owned, 65536, Mode::ReadWrite) && valid;
						    if (valid) std::memset(reinterpret_cast<void*>(owned), 0xa5, 65536);
					    }
					    if (scenario == Scenario::Unrelated)
					    {
						    const uint64_t unrelated = Alloc(0, 4096, Mode::ReadWrite);
						    valid = unrelated != 0 && valid;
						    if (unrelated != 0) valid = Free(unrelated) && valid;
					    }
					    if (scenario == Scenario::Queue)
					    {
						    cp->SetQueue(GraphicContext::QUEUE_COMPUTE_START);
						    current.processor_queue = GraphicContext::QUEUE_COMPUTE_START;
					    }
					    if (scenario == Scenario::Epoch)
					    {
						    cp->ResetTrackedCommandBufferSubmissionTimeline();
						    ++current.processor_epoch;
					    }
					    if (scenario == Scenario::Recording) ++current.recording;
					    if (scenario == Scenario::Exclusive)
					    {
						    cp->SetEopCheckpointExclusiveStream(false);
						    current.exclusive = false;
					    }
					    cp->Unlock();
				    }
			    });
			    worker.join();
		    }, [] { return CurrentThreadHasMemoryLease(); });
		if (scenario == Scenario::HeldLease) valid = held.IsActive() && CurrentThreadHasMemoryLease() && valid;
		ReleaseGuestDataLease(&held);
		valid = mutation.Abort() && valid;
		std::thread retained([&] {
			const bool processor_available = cp->TryLock();
			const bool run_available = cp->TryRunLockForTest();
			valid = valid && !processor_available && !run_available;
			if (processor_available) cp->Unlock();
			if (run_available) cp->RunUnlock();
		});
		retained.join();
		uint64_t before = 0, observed = 0, after = 0;
		std::memcpy(&before, reinterpret_cast<const void*>(owned + 120), 8);
		std::memcpy(&observed, reinterpret_cast<const void*>(owned + 128), 8);
		std::memcpy(&after, reinterpret_cast<const void*>(owned + 136), 8);
		valid = valid && before == 0xa5a5a5a5a5a5a5a5ull && observed == before && after == before;
		const bool success = scenario == Scenario::MutationAbort || scenario == Scenario::Unrelated;
		if (validation_only)
			valid = valid && destination.Address() == captured_address && destination.Size() == captured_size &&
			    destination.Generation() == (scenario == Scenario::Shape ? 0u : captured_generation);
		if (result.captured)
		{
			valid = valid && destination.Generation() == generation && destination.Address() == address && destination.Size() == bytes;
			constexpr uint64_t value = 0x1122334455667788ull;
			valid = destination.Publish(&value) && valid;
			++stores; ++callbacks;
		}
		std::memcpy(&observed, reinterpret_cast<const void*>(owned + 128), 8);
		std::memcpy(&before, reinterpret_cast<const void*>(owned + 120), 8);
		std::memcpy(&after, reinterpret_cast<const void*>(owned + 136), 8);
		const uint64_t expected_value = bytes == 4 ? 0xa5a5a5a555667788ull : 0x1122334455667788ull;
		const uint32_t expected_rounds = scenario == Scenario::HeldLease || scenario == Scenario::TokenUnavailable ||
		    scenario == Scenario::InitialNonexclusive ? 0u :
		    scenario == Scenario::MutationAbort ? 3u : scenario == Scenario::Exhaust ? 16u :
		    (scenario == Scenario::ProtectRevert || scenario == Scenario::Remap || scenario == Scenario::Unrelated) ? 2u : 1u;
		const uint32_t expected_waits = scenario == Scenario::MutationAbort ? 2u : scenario == Scenario::Exhaust ? 15u : deferred ? 1u : 0u;
		const uint32_t expected_local_attempts = expected_rounds == 0 || scenario == Scenario::Shape ? 0u :
		    scenario == Scenario::ShortExhaust ? 15u : success || scenario == Scenario::ProtectRevert ||
		    scenario == Scenario::Remap || scenario == Scenario::Execute || scenario == Scenario::KnownRights ? 1u : 16u;
		const char* expected_reason = success || scenario == Scenario::UnknownReason ? nullptr :
		    scenario == Scenario::HeldLease ? "retry-memory-lease-held" : scenario == Scenario::TokenUnavailable ? "mapping-generation-unavailable" :
		    scenario == Scenario::Shape ? "destination-shape" : scenario == Scenario::Execute ? "destination-executable" :
		    scenario == Scenario::KnownRights ? "destination-protection" :
		    (scenario == Scenario::Exhaust || scenario == Scenario::ShortExhaust) ? "lease-acquisition-retry-exhausted" :
		    (scenario == Scenario::ProtectRevert || scenario == Scenario::Remap) ? "mapping-continuity-changed" :
		    "recording-changed-during-target-capture";
		if (!valid || result.captured != success || result.original_generation != generation || result.rounds != expected_rounds ||
		    calls != expected_rounds || waits != expected_waits || result.acquisition.attempts != expected_local_attempts ||
		    flushes != 0 || gpu_proofs != 0 || before != 0xa5a5a5a5a5a5a5a5ull || after != before ||
		    callbacks != (success ? 1u : 0u) || stores != (success ? 1u : 0u) || CurrentThreadHasMemoryLease() ||
		    observed != (success ? expected_value : 0xa5a5a5a5a5a5a5a5ull) ||
		    (expected_reason == nullptr ? result.acquisition.reason != nullptr :
		        result.acquisition.reason == nullptr || std::strcmp(result.acquisition.reason, expected_reason) != 0))
		{
			std::fprintf(stderr, "[GFX_EOP_INITIAL_TARGET_CAPTURE_TEST] failed_scenario=%u validation_only=%u bytes=%" PRIu64
			                     " valid=%u rounds=%u calls=%u waits=%u captured=%u reason=%s local_attempts=%u\n",
			    static_cast<unsigned>(scenario), validation_only ? 1u : 0u, bytes, valid ? 1u : 0u, result.rounds, calls, waits, result.captured ? 1u : 0u,
			    result.acquisition.reason != nullptr ? result.acquisition.reason : "none", result.acquisition.attempts);
			return false;
		}
	}
	cp->SetQueue(GraphicContext::QUEUE_GFX);
	cp->SetEopCheckpointExclusiveStream(false);
	std::fprintf(stderr, "[GFX_EOP_INITIAL_TARGET_CAPTURE_TEST] actual-VM-borrow-denial=checked mutation-abort=recover"
	                     " protect/revert/remap=reject unrelated-mutation=accept original-token=retained cp-depth1=released/restored"
	                     " run-lock=held identity-all-fields=reject rounds-max16=pass waits-max15=pass held-lease/known-reason=terminal"
	                     " no-prefix/GPU/callback-before-capture=pass store4/8-once=pass guards=preserved\n");
	std::fprintf(stderr, "[GFX_EOP_INITIAL_TARGET_REVALIDATE_TEST] actual-VM-already-captured-target=checked"
	                     " validate-only=pass original-range/token=immutable abort/unrelated=recover protect/revert/remap=reject"
	                     " identity-all-fields=reject parser/run-lock/exclusive=required cp1=released/restored run-lock=held"
	                     " exhausted16=only-retry rounds-max16=pass waits-max15=pass prefix/GPU/Label-completion=modeled"
	                     " flush/GPU/callback-before-validation=none store4/8-once=pass guards=preserved\n");
#else
	(void)cp;
#endif
	return true;
}

static bool graphics_run_eop_cpu_proof_resume_test(CommandProcessor* cp)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	Init();
	enum class Scenario { Recover, Exhaust, KnownContent, Inventory, Queue, Epoch, Exclusive, HeldLease,
	                      ProtectRevert, Remap, DeferredRecover, DeferredExhaust, SharedBudget };
	for (const auto scenario: {Scenario::Recover, Scenario::Exhaust, Scenario::KnownContent, Scenario::Inventory,
	                          Scenario::Queue, Scenario::Epoch, Scenario::Exclusive, Scenario::HeldLease,
	                          Scenario::ProtectRevert, Scenario::Remap, Scenario::DeferredRecover,
	                          Scenario::DeferredExhaust, Scenario::SharedBudget})
	{
		const uint64_t owned = Alloc(0, 4096, Mode::ReadWrite);
		if (owned == 0) return false;
		struct Owner { uint64_t address; ~Owner() { Free(address); } } owner {owned};
		std::memset(reinterpret_cast<void*>(owned), 0, 4096);
		std::memset(reinterpret_cast<void*>(owned + 504), 0xa5, 24);
		const uint64_t planes[] {owned + 64, owned + 128, owned + 192};
		const uint64_t sizes[] {32, 32, 32};
		GuestWriteDestination destination;
		if (!destination.Capture(owned + 512, 8, 8)) return false;
		const uint64_t generation = destination.Generation();
		Core::Mutex tracker;
		struct ExclusiveProcessorAndTracker
		{
			CommandProcessor* cp;
			Core::Mutex& tracker;
			ExclusiveProcessorAndTracker(CommandProcessor* value, Core::Mutex& mutex): cp(value), tracker(mutex)
			{
				cp->RunLock(); cp->Lock(); cp->Lock(); tracker.Lock(); tracker.Lock();
			}
			~ExclusiveProcessorAndTracker()
			{
				tracker.Unlock(); tracker.Unlock(); cp->Unlock(); cp->Unlock(); cp->RunUnlock();
			}
		} exclusive(cp, tracker);
		cp->SetQueue(GraphicContext::QUEUE_GFX);
		cp->SetEopCheckpointExclusiveStream(true);
		cp->ResetTrackedCommandBufferSubmissionTimeline();
		uint64_t epoch = 0, serial = 0, submitted = 0, completed = 0;
		if (!cp->BeginTrackedCommandBufferSubmission(GraphicContext::QUEUE_GFX, &epoch, &serial, &submitted, &completed) ||
		    !cp->RecordTrackedCommandBufferSubmission(GraphicContext::QUEUE_GFX, epoch, serial)) return false;
		cp->RecordTrackedCommandBufferCompletion(GraphicContext::QUEUE_GFX, epoch, serial);
		if (!GraphicsRunCommandProcessorEopResumeValid(cp, GraphicContext::QUEUE_GFX, epoch, serial) ||
		    GraphicsRunCommandProcessorEopResumeValid(nullptr, GraphicContext::QUEUE_GFX, epoch, serial) ||
		    GraphicsRunCommandProcessorEopResumeValid(cp, GraphicContext::QUEUE_COMPUTE_START, epoch, serial) ||
		    GraphicsRunCommandProcessorEopResumeValid(cp, GraphicContext::QUEUE_GFX, epoch + 1, serial) ||
		    GraphicsRunCommandProcessorEopResumeValid(cp, GraphicContext::QUEUE_GFX, epoch, serial + 1)) return false;
		GuestDataLease held;
		if (scenario == Scenario::HeldLease && !TryAcquireGuestDataLease(owned, 8, Mode::Read, &held, false)) return false;
		if (scenario == Scenario::KnownContent) *reinterpret_cast<uint8_t*>(planes[1]) = 1;
		ReservationHandle reservation;
		MutationTransaction mutation;
		const bool deferred = scenario == Scenario::DeferredRecover || scenario == Scenario::DeferredExhaust;
		if (deferred && !BeginReserveAligned(0, 65536, 65536, &reservation, &mutation)) return false;
		uint32_t backoffs = scenario == Scenario::SharedBudget ? 14u : 0u;
		uint32_t proofs = 0, waits = 0, unavailable_validations = 0;
		uint64_t inventory = 7;
		const uint64_t frozen_inventory = inventory;
		// Model a cursor reached after one GPU proof and one prior callback. The
		// actual depth/VM probes below may repeat, but these effects never do.
		uint32_t gpu_proofs = 1, prior_callbacks = 1, callback_cursor = 1, current_callbacks = 0;
		bool valid = true;
		const auto proof = [&]() -> EopCpuProofResult {
			++proofs;
			valid = valid && gpu_proofs == 1 && prior_callbacks == 1 && callback_cursor == 1 && current_callbacks == 0;
			if (scenario != Scenario::KnownContent && !deferred &&
			    (scenario != Scenario::Recover || proofs < 3))
				return {false, true, "lease-set-retry-exhausted"};
			const char* reason = nullptr;
			const bool complete = DepthSurfaceUniformZeroBytes(planes, sizes, 3, generation, &reason);
			return {complete, !complete && reason != nullptr && std::strcmp(reason, "lease-set-retry-exhausted") == 0, reason};
		};
		const auto validate = [&]() -> EopCpuProofResult {
			if (!GraphicsRunCommandProcessorEopResumeValid(cp, GraphicContext::QUEUE_GFX, epoch, serial))
				return {false, false, "fixture-prefix-changed"};
			if (inventory != frozen_inventory) return {false, false, "fixture-inventory-changed"};
			GraphicsGuestLeaseResult acquisition;
			if (destination.Validate(&acquisition)) return {true, false, nullptr};
			const bool unavailable = acquisition.reason != nullptr &&
			    std::strcmp(acquisition.reason, "lease-acquisition-retry-exhausted") == 0;
			if (unavailable) ++unavailable_validations;
			return {false, unavailable, unavailable ? "continuity-lease-unavailable" : "fixture-original-token-changed"};
		};
		const auto yield = [&] {
			WithEopCpuProofLocksReleased(tracker, [cp] { GraphicsRunCommandProcessorUnlock(cp); },
			    [cp] { GraphicsRunCommandProcessorLock(cp); }, [&] {
				++waits;
				valid = valid && !CurrentThreadHasMemoryLease();
				std::thread worker([&] {
					const bool processor_available = cp->TryLock();
					const bool tracker_available = tracker.TryLock();
					const bool run_available = cp->TryRunLockForTest();
					valid = valid && processor_available && tracker_available && !run_available;
					if (run_available) cp->RunUnlock();
					if (processor_available && tracker_available)
					{
						if (scenario == Scenario::Recover && waits == 1)
						{
							const uint64_t unrelated = Alloc(0, 4096, Mode::ReadWrite);
							valid = unrelated != 0 && valid;
							if (unrelated != 0) valid = Free(unrelated) && valid;
						}
						if (scenario == Scenario::Inventory) ++inventory;
						if (scenario == Scenario::Queue) cp->SetQueue(GraphicContext::QUEUE_COMPUTE_START);
						if (scenario == Scenario::Epoch) cp->ResetTrackedCommandBufferSubmissionTimeline();
						if (scenario == Scenario::Exclusive) cp->SetEopCheckpointExclusiveStream(false);
						if (scenario == Scenario::ProtectRevert)
							valid = Protect(owned, 4096, Mode::Read) && Protect(owned, 4096, Mode::ReadWrite) && valid;
						if (scenario == Scenario::Remap)
						{
							valid = Free(owned) && AllocFixed(owned, 4096, Mode::ReadWrite) && valid;
							if (valid)
							{
								std::memset(reinterpret_cast<void*>(owned), 0, 4096);
								std::memset(reinterpret_cast<void*>(owned + 504), 0xa5, 24);
							}
						}
					}
					if (tracker_available) tracker.Unlock();
					if (processor_available) cp->Unlock();
				});
				worker.join();
				if (scenario == Scenario::DeferredRecover && waits == 2) valid = mutation.Abort() && valid;
			});
		};
		const auto result = ResumeEopCpuProof(proof, validate, yield, [] { return CurrentThreadHasMemoryLease(); }, &backoffs);
		if (scenario == Scenario::HeldLease) valid = valid && held.IsActive() && CurrentThreadHasMemoryLease();
		ReleaseGuestDataLease(&held);
		valid = mutation.Abort() && valid;
		const bool success = scenario == Scenario::Recover || scenario == Scenario::DeferredRecover;
		if (result.complete)
		{
			constexpr uint64_t value = 0x1122334455667788ull;
			valid = destination.Publish(&value) && valid;
			++current_callbacks;
			++callback_cursor;
		}
		// Confirm both levels were reacquired, then peel only the inner level.
		// A single reacquisition would let the second worker enter too early.
		const auto locks_retained = [&] {
			std::thread worker([&] {
				const bool processor_available = cp->TryLock();
				const bool tracker_available = tracker.TryLock();
				const bool run_available = cp->TryRunLockForTest();
				valid = valid && !processor_available && !tracker_available && !run_available;
				if (run_available) cp->RunUnlock();
				if (tracker_available) tracker.Unlock();
				if (processor_available) cp->Unlock();
			});
			worker.join();
		};
		locks_retained();
		tracker.Unlock(); cp->Unlock();
		locks_retained();
		cp->Lock(); tracker.Lock();
		if (scenario == Scenario::SharedBudget)
		{
			const auto next = ResumeEopCpuProof(proof, validate, yield, [] { return false; }, &backoffs);
			const auto already_complete = ResumeEopCpuProof([] { return EopCpuProofResult {true, false, nullptr}; },
			    validate, yield, [] { return false; }, &backoffs);
			valid = valid && !next.complete && next.reason != nullptr &&
			    std::strcmp(next.reason, "cpu-proof-resume-exhausted") == 0 && already_complete.complete;
		}
		uint64_t left = 0, observed = 0, right = 0;
		std::memcpy(&left, reinterpret_cast<void*>(owned + 504), 8);
		std::memcpy(&observed, reinterpret_cast<void*>(owned + 512), 8);
		std::memcpy(&right, reinterpret_cast<void*>(owned + 520), 8);
		const uint32_t expected_proofs = scenario == Scenario::Recover ? 3u : scenario == Scenario::Exhaust ? 16u :
		    scenario == Scenario::DeferredRecover ? 2u : scenario == Scenario::SharedBudget ? 3u : 1u;
		const uint32_t expected_waits = success ? 2u :
		    (scenario == Scenario::Exhaust || scenario == Scenario::DeferredExhaust) ? 15u :
		    (scenario == Scenario::KnownContent || scenario == Scenario::HeldLease) ? 0u : 1u;
		const char* expected_reason = success ? nullptr : scenario == Scenario::KnownContent ? "nonzero-content" :
		    scenario == Scenario::HeldLease ? "retry-memory-lease-held" : scenario == Scenario::Inventory ? "fixture-inventory-changed" :
		    (scenario == Scenario::Queue || scenario == Scenario::Epoch || scenario == Scenario::Exclusive) ? "fixture-prefix-changed" :
		    (scenario == Scenario::ProtectRevert || scenario == Scenario::Remap) ? "fixture-original-token-changed" : "cpu-proof-resume-exhausted";
		if (!valid || proofs != expected_proofs || waits != expected_waits || result.complete != success || result.unavailable ||
		    (expected_reason != nullptr && (result.reason == nullptr || std::strcmp(result.reason, expected_reason) != 0)) ||
		    backoffs != (scenario == Scenario::SharedBudget ? 15u : expected_waits) ||
		    (deferred && unavailable_validations == 0) || destination.Generation() != generation || CurrentThreadHasMemoryLease() ||
		    gpu_proofs != 1 || prior_callbacks != 1 || current_callbacks != (success ? 1u : 0u) || callback_cursor != (success ? 2u : 1u) ||
		    observed != (success ? 0x1122334455667788ull : 0xa5a5a5a5a5a5a5a5ull) ||
		    left != 0xa5a5a5a5a5a5a5a5ull || right != 0xa5a5a5a5a5a5a5a5ull)
		{
			std::fprintf(stderr, "[GFX_EOP_CPU_PROOF_RESUME_TEST] failed_scenario=%u valid=%u proofs=%u waits=%u backoffs=%u"
			                     " complete=%u unavailable=%u reason=%s unavailable_validations=%u\n",
			    static_cast<unsigned>(scenario), valid ? 1u : 0u, proofs, waits, backoffs, result.complete ? 1u : 0u,
			    result.unavailable ? 1u : 0u, result.reason != nullptr ? result.reason : "none", unavailable_validations);
			return false;
		}
	}
	cp->SetQueue(GraphicContext::QUEUE_GFX);
	cp->SetEopCheckpointExclusiveStream(false);
	std::fprintf(stderr, "[GFX_EOP_CPU_PROOF_RESUME_TEST] cp/tracker-depth2=released/restored run-lock=held"
	                     " actual-depth/VM-proof=checked real-worker-protect/revert/remap=reject queue/epoch/exclusive=reject"
	                     " inventory-guard=reject original-token=retained deferred-proof=none effects/cursor=once"
	                     " held-lease/content=terminal shared-backoffs-max15=pass attempts-max16=pass guards=preserved\n");
#else
	(void)cp;
#endif
	return true;
}

static bool graphics_run_eop_materialized_cpu_resume_test(CommandProcessor* cp)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	Init();
	enum class Scenario { Recover, ProtectRevert, Queue, Epoch, Exclusive, Inventory, Content, Exhaust, SharedBudget };
	for (const auto scenario: {Scenario::Recover, Scenario::ProtectRevert, Scenario::Queue, Scenario::Epoch,
	                          Scenario::Exclusive, Scenario::Inventory, Scenario::Content, Scenario::Exhaust, Scenario::SharedBudget})
	{
		const uint64_t owned = Alloc(0, 4096, Mode::ReadWrite);
		if (owned == 0) return false;
		struct Owner { uint64_t address; ~Owner() { Free(address); } } owner {owned};
		std::memset(reinterpret_cast<void*>(owned), 0, 4096);
		std::memset(reinterpret_cast<void*>(owned + 504), 0xa5, 24);
		if (scenario == Scenario::Content) *reinterpret_cast<uint8_t*>(owned + 64) = 1;
		const uint64_t original_generation = GetMutationGeneration();
		Core::Mutex tracker;
		struct MaterializerOwner
		{
			CommandProcessor* cp;
			Core::Mutex& tracker;
			MaterializerOwner(CommandProcessor* value, Core::Mutex& mutex): cp(value), tracker(mutex)
			{ cp->RunLock(); cp->Lock(); tracker.Lock(); }
			~MaterializerOwner() { tracker.Unlock(); cp->Unlock(); cp->RunUnlock(); }
		} materializer_owner(cp, tracker);
		cp->SetQueue(GraphicContext::QUEUE_GFX);
		cp->SetEopCheckpointExclusiveStream(true);
		cp->ResetTrackedCommandBufferSubmissionTimeline();
		uint64_t epoch = 0, serial = 0, submitted = 0, completed = 0;
		if (!cp->BeginTrackedCommandBufferSubmission(GraphicContext::QUEUE_GFX, &epoch, &serial, &submitted, &completed) ||
		    !cp->RecordTrackedCommandBufferSubmission(GraphicContext::QUEUE_GFX, epoch, serial)) return false;
		cp->RecordTrackedCommandBufferCompletion(GraphicContext::QUEUE_GFX, epoch, serial);
		ReservationHandle reservation;
		MutationTransaction mutation;
		if (scenario != Scenario::Content && !BeginReserveAligned(0, 65536, 65536, &reservation, &mutation)) return false;
		uint64_t inventory = 7;
		const uint64_t frozen_inventory = inventory;
		uint32_t proofs = 0, waits = 0, observations = 0;
		uint32_t backoffs = scenario == Scenario::SharedBudget ? 14u : 0u;
		// These already-completed effects belong to the enclosing materializer.
		// Only the failed current CPU proof can repeat after a lock pause.
		uint32_t materializations = 1, callbacks = 1, prior_successful_proofs = 1;
		bool valid = true;
		const auto proof = [&]() -> EopCpuProofResult {
			++proofs;
			GuestDataLease lease;
			GraphicsGuestLeaseResult acquisition;
			if (!TryAcquireGraphicsGuestContinuityLease(owned + 64, 32, Mode::Read,
			        original_generation, &lease, &acquisition))
				return {false, acquisition.reason != nullptr && acquisition.attempts == 16 &&
				    std::strcmp(acquisition.reason, "lease-acquisition-retry-exhausted") == 0, acquisition.reason};
			++observations;
			valid = valid && CurrentThreadHasMemoryLease() && materializations == 1 && callbacks == 1 && prior_successful_proofs == 1;
			const auto* bytes = reinterpret_cast<const uint8_t*>(owned + 64);
			const bool equal = std::all_of(bytes, bytes + 32, [](uint8_t value) { return value == 0; });
			return {equal, false, equal ? nullptr : "fixture-content-changed"};
		};
		const auto validate = [&]() -> EopCpuProofResult {
			if (!GraphicsRunCommandProcessorEopResumeValid(cp, GraphicContext::QUEUE_GFX, epoch, serial))
				return {false, false, "fixture-prefix-changed"};
			if (inventory != frozen_inventory) return {false, false, "fixture-inventory-changed"};
			GuestDataLease lease;
			GraphicsGuestLeaseResult acquisition;
			if (TryAcquireGraphicsGuestContinuityLease(owned + 64, 32, Mode::Read,
			        original_generation, &lease, &acquisition)) return {true, false, nullptr};
			return {false, acquisition.reason != nullptr && acquisition.attempts == 16 &&
				    std::strcmp(acquisition.reason, "lease-acquisition-retry-exhausted") == 0, acquisition.reason};
		};
		const auto yield = [&] {
			WithEopMaterializedCpuProofLocksReleased(tracker, [cp] { GraphicsRunCommandProcessorUnlock(cp); },
			    [cp] { GraphicsRunCommandProcessorLock(cp); }, [&] {
				++waits;
				valid = valid && !CurrentThreadHasMemoryLease();
				std::thread worker([&] {
					const bool processor_available = cp->TryLock(), tracker_available = tracker.TryLock();
					const bool run_available = cp->TryRunLockForTest();
					valid = valid && processor_available && tracker_available && !run_available;
					if (run_available) cp->RunUnlock();
					if (processor_available && tracker_available)
					{
						if (scenario == Scenario::Queue) cp->SetQueue(GraphicContext::QUEUE_COMPUTE_START);
						if (scenario == Scenario::Epoch) cp->ResetTrackedCommandBufferSubmissionTimeline();
						if (scenario == Scenario::Exclusive) cp->SetEopCheckpointExclusiveStream(false);
						if (scenario == Scenario::Inventory) ++inventory;
					}
					if (tracker_available) tracker.Unlock();
					if (processor_available) cp->Unlock();
				});
				worker.join();
				if (scenario != Scenario::Exhaust && scenario != Scenario::SharedBudget)
					valid = mutation.Abort() && valid;
				if (scenario == Scenario::ProtectRevert)
					valid = Protect(owned, 4096, Mode::Read) && Protect(owned, 4096, Mode::ReadWrite) && valid;
			});
		};
		const auto result = ResumeEopCpuProof(proof, validate, yield, [] { return CurrentThreadHasMemoryLease(); }, &backoffs);
		valid = mutation.Abort() && valid;
		const auto probe_locks = [&](bool expected_available) {
			std::thread worker([&] {
				const bool processor_available = cp->TryLock(), tracker_available = tracker.TryLock();
				const bool run_available = cp->TryRunLockForTest();
				valid = valid && processor_available == expected_available && tracker_available == expected_available && !run_available;
				if (run_available) cp->RunUnlock();
				if (tracker_available) tracker.Unlock();
				if (processor_available) cp->Unlock();
			});
			worker.join();
		};
		probe_locks(false);
		tracker.Unlock(); cp->Unlock();
		probe_locks(true);
		cp->Lock(); tracker.Lock();
		uint64_t left = 0, target = 0, right = 0;
		std::memcpy(&left, reinterpret_cast<const void*>(owned + 504), 8);
		std::memcpy(&target, reinterpret_cast<const void*>(owned + 512), 8);
		std::memcpy(&right, reinterpret_cast<const void*>(owned + 520), 8);
		const bool success = scenario == Scenario::Recover;
		const uint32_t expected_waits = scenario == Scenario::Content ? 0u : scenario == Scenario::Exhaust ? 15u : 1u;
		const char* expected_reason = success ? nullptr : scenario == Scenario::Content ? "fixture-content-changed" :
		    scenario == Scenario::ProtectRevert ? "mapping-continuity-changed" : scenario == Scenario::Inventory ? "fixture-inventory-changed" :
		    (scenario == Scenario::Exhaust || scenario == Scenario::SharedBudget) ? "cpu-proof-resume-exhausted" : "fixture-prefix-changed";
		if (!valid || result.complete != success || proofs != (success ? 2u : 1u) || waits != expected_waits ||
		    observations != (success || scenario == Scenario::Content ? 1u : 0u) ||
		    backoffs != (scenario == Scenario::SharedBudget ? 15u : expected_waits) || CurrentThreadHasMemoryLease() ||
		    materializations != 1 || callbacks != 1 || prior_successful_proofs != 1 ||
		    left != 0xa5a5a5a5a5a5a5a5ull || target != left || right != left ||
		    (expected_reason == nullptr ? result.reason != nullptr : result.reason == nullptr || std::strcmp(result.reason, expected_reason) != 0))
			return false;
	}
	cp->SetQueue(GraphicContext::QUEUE_GFX);
	cp->SetEopCheckpointExclusiveStream(false);
	std::fprintf(stderr, "[GFX_EOP_MATERIALIZED_CPU_RESUME_TEST] cp-depth1/tracker-depth1=released/restored run-lock=held"
	                     " real-VM-mutation-abort=recover protect/revert=reject prefix/queue/epoch/exclusive/inventory=checked"
	                     " materialization/callback/prior-proof=once current-byte-observation=once-after-lease"
	                     " content=terminal backoffs-max15/shared-budget=pass target/canaries=untouched\n");
#else
	(void)cp;
#endif
	return true;
}

static bool graphics_run_retained_recording_scope_test(CommandProcessor* cp)
{
	// Only Vulkan handles/recording metadata are opaque in this headless test;
	// the public accessor, CP slot, TLS witness, parser/run ownership and mutex
	// exclusion are the production objects and operations.
	int buffer_key = 0, pool_key = 0, foreign_key = 0;
	auto* other_cp = new CommandProcessor(64); // Process-owned, like production processors.
	auto* buffer = reinterpret_cast<CommandBuffer*>(&buffer_key);
	auto* foreign = reinterpret_cast<CommandBuffer*>(&foreign_key);
	if (GraphicsRunCommandProcessorRetainedRecordingValid(cp, buffer)) return false;
	const int saved_index = cp->m_current_buffer;
	auto* saved_buffer = cp->m_buffer[0];
	const int saved_queue = cp->m_queue;
	const bool saved_exclusive = cp->m_eop_checkpoint_exclusive_stream;
	cp->RunLock();
	cp->Lock();
	struct Restore
	{
		CommandProcessor* cp;
		int index, queue;
		CommandBuffer* buffer;
		bool exclusive;
		~Restore()
		{
			cp->m_retained_identity_for_test = nullptr;
			cp->m_current_buffer = index; cp->m_buffer[0] = buffer; cp->m_queue = queue;
			cp->m_eop_checkpoint_exclusive_stream = exclusive;
			cp->Unlock(); cp->RunUnlock();
		}
	} restore {cp, saved_index, saved_queue, saved_buffer, saved_exclusive};
	cp->m_current_buffer = 0; cp->m_buffer[0] = buffer; cp->m_queue = GraphicContext::QUEUE_GFX;
	cp->m_eop_checkpoint_exclusive_stream = false;
	EopInitialTargetIdentity identity;
	identity.processor = identity.parent = cp; identity.buffer = buffer; identity.pool = &pool_key;
	identity.current_index = identity.pool_index = 0;
	identity.processor_queue = identity.buffer_queue = GraphicContext::QUEUE_GFX;
	identity.submit_id = 5; identity.processor_epoch = identity.session = 7;
	identity.assigned_serial = identity.serial = 9; identity.recording = 11; identity.ordinal = 13;
	identity.submitted_cutoff = 8; identity.completed_cutoff = 6; identity.valid = true;
	cp->m_retained_identity_for_test = &identity;
	const auto valid = [&] { return GraphicsRunCommandProcessorRetainedRecordingValid(cp, buffer); };
	CommandProcessorRunStateScope parser(cp);
	uint32_t rejected = 0;
	{
		RetainedProcessorRecordingScope retained(cp, buffer, cp->m_retained_recording_unlock_epoch);
		if (!valid()) return false; // A stable double-stream draw is admitted.
		bool thread_rejected = false, mutex_excluded = false;
		std::thread ce([&]
		{
			thread_rejected = !valid();
			mutex_excluded = !cp->TryLock();
			if (!mutex_excluded) cp->Unlock();
		});
		ce.join();
		if (!thread_rejected || !mutex_excluded) return false;
		rejected += 2;
		if (GraphicsRunCommandProcessorRetainedRecordingValid(cp, foreign)) return false;
		if (GraphicsRunCommandProcessorRetainedRecordingValid(other_cp, buffer)) return false;
		++rejected;
		{
			RetainedProcessorRecordingScope nested(cp, buffer, cp->m_retained_recording_unlock_epoch);
			if (valid()) return false;
			++rejected;
		}
		if (!valid()) return false;
		const auto original = identity;
		for (unsigned field = 0; field != 12; ++field)
		{
			identity = original;
			switch (field)
			{
				case 0: identity.processor = &foreign_key; break;
				case 1: identity.parent = &foreign_key; break;
				case 2: identity.buffer = foreign; break;
				case 3: identity.pool = nullptr; break;
				case 4: identity.pool_index = UINT32_MAX; break;
				case 5: identity.current_index = -1; break;
				case 6: identity.processor_queue = GraphicContext::QUEUE_COMPUTE_START; break;
				case 7: identity.buffer_queue = GraphicContext::QUEUE_COMPUTE_START; break;
				case 8: ++identity.session; break;
				case 9: ++identity.assigned_serial; break;
				case 10: identity.completed_cutoff = identity.submitted_cutoff + 1; break;
				case 11: identity.valid = false; break;
			}
			if (valid()) return false;
			++rejected;
		}
		identity = original;
		cp->m_buffer[0] = foreign;
		if (valid()) return false;
		cp->m_buffer[0] = buffer;
		++rejected;
		{
			CommandProcessorRunStateScope wrong_parser(other_cp);
			if (valid()) return false;
			++rejected;
		}
		cp->RunUnlock();
		if (valid()) return false;
		cp->RunLock();
		++rejected;
		// Even an identical restored recording cannot revive an old witness.
		cp->Unlock();
		if (valid()) { cp->Lock(); return false; }
		cp->Lock();
		if (valid()) return false;
		++rejected;
	}
	if (valid()) return false;
	++rejected;
	{
		RetainedProcessorRecordingScope next_draw(cp, buffer, cp->m_retained_recording_unlock_epoch);
		if (!valid()) return false;
	}
	std::fprintf(stderr, "[GFX_RETAINED_RECORDING_TEST] double-stream=accepted rejected=%u scope/thread/CP/parser/run=checked"
	    " current-slot/identity=checked peer-CE-mutex=excluded CP-release=revokes Vulkan-metadata=headless\n", rejected);
	return true;
}

static bool graphics_run_render_full_preflight_retry_test(CommandProcessor* cp)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	Init();
	enum class Scenario { Recover, Exhaust, ProtectRestore, Unrelated, Remap, Inventory, Label, Recording,
	                      Queue, Epoch, Exclusive, InitialNonexclusive, HeldLease, EffectsStarted, Content };
	for (const auto scenario: {Scenario::Recover, Scenario::Exhaust, Scenario::ProtectRestore, Scenario::Unrelated,
	        Scenario::Remap, Scenario::Inventory, Scenario::Label, Scenario::Recording, Scenario::Queue, Scenario::Epoch,
	        Scenario::Exclusive, Scenario::InitialNonexclusive, Scenario::HeldLease, Scenario::EffectsStarted, Scenario::Content})
	{
		const uint64_t owned = Alloc(0, 65536, Mode::ReadWrite);
		if (owned == 0) return false;
		struct Owner { uint64_t address; ~Owner() { if (address != 0) Free(address); } } owner {owned};
		std::memset(reinterpret_cast<void*>(owned), 0, 65536);
		std::memset(reinterpret_cast<void*>(owned + 504), 0xa5, 24);
		const uint64_t planes[] {owned + 64, owned + 128, owned + 192};
		const uint64_t sizes[] {32, 32, 32};
		GuestWriteDestination destination;
		if (!destination.Capture(owned + 512, 8, 8)) return false;
		const uint64_t original_generation = destination.Generation();
		Core::Mutex renderer, tracker;
		struct RunOwner
		{
			CommandProcessor* cp;
			explicit RunOwner(CommandProcessor* value): cp(value) { cp->RunLock(); }
			~RunOwner() { cp->RunUnlock(); }
		} run_owner(cp);
		cp->SetQueue(GraphicContext::QUEUE_GFX);
		cp->SetEopCheckpointExclusiveStream(scenario != Scenario::InitialNonexclusive);
		cp->ResetTrackedCommandBufferSubmissionTimeline();
		uint64_t epoch = 0, serial = 0, submitted = 0, completed = 0;
		if (!cp->BeginTrackedCommandBufferSubmission(GraphicContext::QUEUE_GFX, &epoch, &serial, &submitted, &completed))
			return false;
		// No Vulkan buffer exists in this fixture. Preserve a modeled recording
		// alongside the real processor queue, epoch, serial and exclusive state.
		uint64_t recording = 7, inventory = 11;
		const uint64_t original_inventory = inventory;
		auto current_identity = [&] {
			auto identity = cp->GetEopInitialTargetIdentity();
			identity.valid = true;
			identity.recording = recording;
			return identity;
		};
		const auto original_identity = current_identity();
		LabelPublicationSnapshot label;
		label.instance_id = 19; label.queued = 1; label.pending = 1; label.cp = cp;
		label.queue = GraphicContext::QUEUE_GFX; label.session_epoch = epoch; label.submit_serial = serial;
		const auto original_label = label;
		const auto label_matches = [&] {
			return std::tie(label.instance_id, label.queued, label.completed, label.cancelled, label.pending, label.cp,
			                label.queue, label.session_epoch, label.submit_serial, label.identity_ambiguous) ==
			       std::tie(original_label.instance_id, original_label.queued, original_label.completed, original_label.cancelled,
			                original_label.pending, original_label.cp, original_label.queue, original_label.session_epoch,
			                original_label.submit_serial, original_label.identity_ambiguous);
		};
		GuestDataLease external;
		if (scenario == Scenario::HeldLease && !TryAcquireGuestDataLease(owned, 8, Mode::Read, &external, false)) return false;
		if (scenario == Scenario::Content) *reinterpret_cast<uint8_t*>(planes[1] + sizes[1] - 1) = 1;
		ReservationHandle reservation;
		MutationTransaction mutation;
		const bool mutation_case = scenario != Scenario::InitialNonexclusive && scenario != Scenario::HeldLease &&
		                           scenario != Scenario::EffectsStarted && scenario != Scenario::Content;
		if (mutation_case && !BeginReserveAligned(0, 65536, 65536, &reservation, &mutation)) return false;
		uint32_t attempts = 0, calls = 0, waits = 0, flushes = 0, callbacks = 0, stores = 0;
		// The parser's preceding MemoryBarrier is already recorded. This helper
		// retries only WriteBack's pure preflight, never its packet or plan.
		uint32_t preceding_packet_effects = 1;
		bool valid = true;
		const auto attempt = [&]() -> GpuMemoryRenderFullWriteBackResult {
			CommandProcessorStateLock outer(cp);
			Core::LockGuard render_lock(renderer);
			CommandProcessorStateLock inner(cp);
			// The ordinary ticket keeps its tracker scope through validation,
			// the inner WriteBack and initial refusal capture.
			Core::LockGuard tracker_lock1(tracker), tracker_lock2(tracker);
			++calls;
			valid = valid && destination.Generation() == original_generation && preceding_packet_effects == 1;
			bool processor_available = true, render_available = true, tracker_available = true, run_available = true;
			std::thread observer([&] {
				processor_available = cp->TryLock(); if (processor_available) cp->Unlock();
				render_available = renderer.TryLock(); if (render_available) renderer.Unlock();
				tracker_available = tracker.TryLock(); if (tracker_available) tracker.Unlock();
				run_available = cp->TryRunLockForTest(); if (run_available) cp->RunUnlock();
			});
			observer.join();
			valid = valid && !processor_available && !render_available && !tracker_available && !run_available;
			if (!current_identity().exclusive) return {false, false, "fixture-nonexclusive"};
			if (!original_identity.Matches(current_identity())) return {false, false, "fixture-recording-changed"};
			if (inventory != original_inventory) return {false, false, "fixture-inventory-changed"};
			if (!label_matches()) return {false, false, "fixture-label-changed"};
			if (scenario == Scenario::HeldLease) return {false, true, "depth-preflight-lease-set-unavailable"};
			if (scenario == Scenario::EffectsStarted)
			{
				++flushes; ++callbacks;
				// Once effects have started, even a lease-like refusal is terminal.
				return {false, false, "fixture-after-effects-lease-unavailable"};
			}
			const char* reason = nullptr;
			const bool complete = DepthSurfaceUniformZeroBytes(planes, sizes, 3, original_generation, &reason);
			if (!complete)
				return {false, reason != nullptr && std::strcmp(reason, "lease-set-retry-exhausted") == 0, reason};
			// Only an admitted attempt can perform this modeled fence/callback and
			// the real guarded guest publication. No effect belongs to a backoff.
			++flushes; ++callbacks;
			constexpr uint64_t value = 0x1122334455667788ull;
			if (!destination.Publish(&value)) return {false, false, "fixture-publication-refused"};
			++stores;
			return {true, false, nullptr};
		};
		const auto backoff = [&] {
			++waits;
			valid = valid && !CurrentThreadHasMemoryLease() && preceding_packet_effects == 1 &&
			        flushes == 0 && callbacks == 0 && stores == 0;
			if (scenario != Scenario::Exhaust) valid = mutation.Abort() && valid;
			std::thread observer([&] {
				const bool processor_available = cp->TryLock();
				const bool render_available = renderer.TryLock();
				const bool tracker_available = tracker.TryLock();
				const bool run_available = cp->TryRunLockForTest();
				valid = valid && processor_available && render_available && tracker_available && !run_available;
				if (run_available) cp->RunUnlock();
				if (processor_available && render_available && tracker_available)
				{
					if (scenario == Scenario::ProtectRestore)
						valid = Protect(owned, 65536, Mode::Read) && Protect(owned, 65536, Mode::ReadWrite) && valid;
					if (scenario == Scenario::Unrelated)
					{
						const uint64_t unrelated = Alloc(0, 4096, Mode::ReadWrite);
						valid = unrelated != 0 && valid;
						if (unrelated != 0) valid = Free(unrelated) && valid;
					}
					if (scenario == Scenario::Remap)
					{
						valid = Free(owned) && valid;
						owner.address = 0;
						valid = AllocFixed(owned, 65536, Mode::ReadWrite) && valid;
						if (valid) { owner.address = owned; std::memset(reinterpret_cast<void*>(owned), 0, 65536);
						             std::memset(reinterpret_cast<void*>(owned + 504), 0xa5, 24); }
					}
					if (scenario == Scenario::Inventory) ++inventory;
					if (scenario == Scenario::Label) { ++label.completed; --label.pending; }
					if (scenario == Scenario::Recording) ++recording;
					if (scenario == Scenario::Queue) cp->SetQueue(GraphicContext::QUEUE_COMPUTE_START);
					if (scenario == Scenario::Epoch) cp->ResetTrackedCommandBufferSubmissionTimeline();
					if (scenario == Scenario::Exclusive) cp->SetEopCheckpointExclusiveStream(false);
				}
				if (tracker_available) tracker.Unlock();
				if (render_available) renderer.Unlock();
				if (processor_available) cp->Unlock();
			});
			observer.join();
		};
		const auto result = RenderFullWriteBackWithPreflightRetry(attempt, backoff,
		    [] { return CurrentThreadHasMemoryLease(); }, &attempts);
		if (scenario == Scenario::HeldLease) valid = external.IsActive() && CurrentThreadHasMemoryLease() && valid;
		ReleaseGuestDataLease(&external);
		valid = mutation.Abort() && valid;
		const bool success = scenario == Scenario::Recover || scenario == Scenario::Unrelated;
		const uint32_t expected_calls = scenario == Scenario::Exhaust ? 16u : mutation_case ? 2u : 1u;
		const uint32_t expected_waits = scenario == Scenario::Exhaust ? 15u : mutation_case ? 1u : 0u;
		const uint32_t expected_effects = success || scenario == Scenario::EffectsStarted ? 1u : 0u;
		uint64_t left = 0, observed = 0, right = 0;
		if (owner.address == 0) return false;
		std::memcpy(&left, reinterpret_cast<void*>(owned + 504), 8);
		std::memcpy(&observed, reinterpret_cast<void*>(owned + 512), 8);
		std::memcpy(&right, reinterpret_cast<void*>(owned + 520), 8);
		if (!valid || attempts != calls || calls != expected_calls || waits != expected_waits || result.complete != success ||
		    result.retryable || destination.Generation() != original_generation || CurrentThreadHasMemoryLease() ||
		    preceding_packet_effects != 1 || flushes != expected_effects || callbacks != expected_effects || stores != (success ? 1u : 0u) ||
		    observed != (success ? 0x1122334455667788ull : 0xa5a5a5a5a5a5a5a5ull) ||
		    left != 0xa5a5a5a5a5a5a5a5ull || right != left ||
		    (!success && result.reason == nullptr) ||
		    (scenario == Scenario::Exhaust && std::strcmp(result.reason, "render-full-preflight-retry-exhausted") != 0))
		{
			std::fprintf(stderr, "[GFX_RENDER_FULL_PREFLIGHT_RETRY_TEST] failed_scenario=%u valid=%u attempts=%u calls=%u waits=%u"
			                     " complete=%u retryable=%u reason=%s effects=%u/%u/%u\n", static_cast<unsigned>(scenario),
			    valid ? 1u : 0u, attempts, calls, waits, result.complete ? 1u : 0u, result.retryable ? 1u : 0u,
			    result.reason != nullptr ? result.reason : "none", flushes, callbacks, stores);
			return false;
		}
	}
	cp->SetQueue(GraphicContext::QUEUE_GFX);
	cp->SetEopCheckpointExclusiveStream(false);
	std::fprintf(stderr, "[GFX_RENDER_FULL_PREFLIGHT_RETRY_TEST] actual-depth/VM-denial=checked cp-depth2/render1/tracker-depth2=fully-unwound"
	                     " run-lock=held original-token=retained recover/unrelated=pass protect/restore/remap=reject"
	                     " recording/queue/epoch/exclusive/inventory/label=reject preflight-only=pass effects/packet/store=once"
	                     " attempts-max16=pass backoffs-max15=pass held-lease/content=terminal guards=preserved\n");
#else
	(void)cp;
#endif
	return true;
}

static bool graphics_run_eop_lease_retry_test(CommandProcessor* cp)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	Init();
	enum class Scenario { Recover, Nonretryable, Exhaust, ChangedBeforeWait, ProtectDuringWait, RemapDuringWait,
	                      QueueDuringWait, EpochDuringWait, LeaseHeld };
	for (const auto publication_kind: {GpuMemoryEopPublicationKind::VirtualClock64, GpuMemoryEopPublicationKind::Immediate32})
	for (const auto scenario: {Scenario::Recover, Scenario::Nonretryable, Scenario::Exhaust, Scenario::ChangedBeforeWait,
	                          Scenario::ProtectDuringWait, Scenario::RemapDuringWait, Scenario::QueueDuringWait,
	                          Scenario::EpochDuringWait, Scenario::LeaseHeld})
	{
		const uint64_t owned = Alloc(0, 4096, Mode::ReadWrite);
		if (owned == 0) return false;
		struct Owner { uint64_t address; ~Owner() { Free(address); } } owner {owned};
		std::memset(reinterpret_cast<void*>(owned), 0xa5, 4096);
		GuestWriteDestination destination;
		const uint64_t publication_bytes = GpuMemoryEopPublicationBytes(publication_kind);
		if (!destination.Capture(owned + 128, publication_bytes, publication_bytes)) return false;
		const uint64_t memory_generation = destination.Generation();
		struct ExclusiveProcessor
		{
			CommandProcessor* cp;
			explicit ExclusiveProcessor(CommandProcessor* value): cp(value) { cp->RunLock(); cp->Lock(); }
			~ExclusiveProcessor() { cp->Unlock(); cp->RunUnlock(); }
		} exclusive(cp);
		cp->SetQueue(GraphicContext::QUEUE_GFX);
		cp->ResetTrackedCommandBufferSubmissionTimeline();
		uint64_t epoch = 0, serial = 0, submitted = 0, completed = 0;
		if (!cp->BeginTrackedCommandBufferSubmission(GraphicContext::QUEUE_GFX, &epoch, &serial, &submitted, &completed) ||
		    !cp->RecordTrackedCommandBufferSubmission(GraphicContext::QUEUE_GFX, epoch, serial)) return false;
		cp->RecordTrackedCommandBufferCompletion(GraphicContext::QUEUE_GFX, epoch, serial);
		const int original_queue = GraphicContext::QUEUE_GFX;
		const uint64_t original_epoch = epoch, original_serial = serial;
		uint32_t attempts = 0, calls = 0, waits = 0;
		bool locks_proven = true, mutations_ok = true;
		GuestDataLease held;
		if (scenario == Scenario::LeaseHeld && !TryAcquireGuestDataLease(owned, 8, Mode::Read, &held, false)) return false;
		constexpr uint64_t value = 0x1122334455667788ull;
		const uint64_t expected_value = publication_kind == GpuMemoryEopPublicationKind::Immediate32
		    ? 0xa5a5a5a555667788ull : value;
		const auto result = EopMaterializeWithLeaseRetry(cp, memory_generation, [&] {
			++calls;
			if (!cp->HasCompletedTrackedCommandBuffer(original_queue, original_epoch, original_serial) ||
			    destination.Generation() != memory_generation)
				return GpuMemoryEopCheckpointResult {GpuMemoryEopCheckpointStatus::Rejected, "fixture-original-proof-changed"};
			if (scenario == Scenario::Nonretryable)
				return GpuMemoryEopCheckpointResult {GpuMemoryEopCheckpointStatus::Rejected, "materialization-rejected"};
			if (scenario == Scenario::ChangedBeforeWait) mutations_ok &= Protect(owned, 4096, Mode::Read);
			if (scenario != Scenario::Recover || calls < 3)
				return GpuMemoryEopCheckpointResult {GpuMemoryEopCheckpointStatus::Rejected, "depth-equivalence-unproven", true};
			// Model only the completed host batch's final publication. No GPU
			// command is bypassed: this closure substitutes the batch in the test.
			if (!destination.Publish(&value))
				return GpuMemoryEopCheckpointResult {GpuMemoryEopCheckpointStatus::Rejected, "fixture-target-rejected"};
			return GpuMemoryEopCheckpointResult {GpuMemoryEopCheckpointStatus::Published, "fixture-published"};
		}, [&](bool after_wait) -> const char* {
			if (!cp->HasCompletedTrackedCommandBuffer(original_queue, original_epoch, original_serial))
				return "prefix-changed-during-retry";
			if (after_wait && !destination.Validate()) return "target-changed-during-retry";
			return nullptr;
		}, [&] {
			++waits;
			locks_proven &= !CurrentThreadHasMemoryLease();
			std::thread worker([&] {
				const bool processor_available = cp->TryLock();
				const bool run_available = cp->TryRunLockForTest();
				locks_proven &= processor_available && !run_available;
				if (run_available) cp->RunUnlock();
				if (scenario == Scenario::ProtectDuringWait) mutations_ok &= Protect(owned, 4096, Mode::Read);
				if (scenario == Scenario::RemapDuringWait)
				{
					mutations_ok &= Free(owned) && AllocFixed(owned, 4096, Mode::ReadWrite);
					if (mutations_ok) std::memset(reinterpret_cast<void*>(owned), 0xa5, 4096);
				}
				if (processor_available)
				{
					if (scenario == Scenario::QueueDuringWait) cp->SetQueue(GraphicContext::QUEUE_COMPUTE_START);
					if (scenario == Scenario::EpochDuringWait) cp->ResetTrackedCommandBufferSubmissionTimeline();
					cp->Unlock();
				}
			});
			worker.join();
		}, &attempts);
		std::thread retained_locks([&] {
			const bool processor_available = cp->TryLock();
			const bool run_available = cp->TryRunLockForTest();
			locks_proven &= !processor_available && !run_available;
			if (processor_available) cp->Unlock();
			if (run_available) cp->RunUnlock();
		});
		retained_locks.join();
		if (!locks_proven || !mutations_ok || attempts != calls || destination.Generation() != memory_generation) return false;
		const uint32_t expected_calls = scenario == Scenario::Recover ? 3 :
		    scenario == Scenario::Exhaust ? GpuMemoryEopCheckpointMaxAttempts : 1;
		const uint32_t expected_waits = scenario == Scenario::Recover ? 2 :
		    scenario == Scenario::Exhaust ? GpuMemoryEopCheckpointMaxAttempts - 1 :
		    (scenario == Scenario::Nonretryable || scenario == Scenario::ChangedBeforeWait || scenario == Scenario::LeaseHeld) ? 0 : 1;
		const char* expected_reason = scenario == Scenario::Recover ? "fixture-published" :
		    scenario == Scenario::Nonretryable ? "materialization-rejected" :
		    scenario == Scenario::Exhaust ? "materialization-retry-exhausted" :
		    scenario == Scenario::ChangedBeforeWait ? "mapping-changed-before-retry" :
		    scenario == Scenario::LeaseHeld ? "retry-memory-lease-held" :
		    (scenario == Scenario::QueueDuringWait || scenario == Scenario::EpochDuringWait) ? "prefix-changed-during-retry" :
		    "mapping-changed-during-retry";
		uint64_t observed = 0, before = 0, after = 0;
		std::memcpy(&before, reinterpret_cast<const void*>(owned + 120), 8);
		std::memcpy(&observed, reinterpret_cast<const void*>(owned + 128), 8);
		std::memcpy(&after, reinterpret_cast<const void*>(owned + 136), 8);
		if (calls != expected_calls || waits != expected_waits ||
		    result.status != (scenario == Scenario::Recover ? GpuMemoryEopCheckpointStatus::Published : GpuMemoryEopCheckpointStatus::Rejected) ||
		    result.retryable || std::strcmp(result.reason, expected_reason) != 0 ||
		    observed != (scenario == Scenario::Recover ? expected_value : 0xa5a5a5a5a5a5a5a5ull) ||
		    before != 0xa5a5a5a5a5a5a5a5ull || after != 0xa5a5a5a5a5a5a5a5ull) return false;
	}
	// A real active mutation can make continuity borrowing unavailable. Deferred
	// rounds must never restart even the pure batch until a checked lease proves
	// the original token, and must share the existing attempt/backoff budget.
	for (const auto publication_kind: {GpuMemoryEopPublicationKind::VirtualClock64, GpuMemoryEopPublicationKind::Immediate32})
	for (const unsigned scenario: {0u, 1u, 2u}) // recover after two waits, exhaust, relevant Protect/revert
	{
		const uint64_t owned = Alloc(0, 65536, Mode::ReadWrite);
		if (owned == 0) return false;
		struct Owner { uint64_t address; ~Owner() { Free(address); } } owner {owned};
		std::memset(reinterpret_cast<void*>(owned), 0xa5, 65536);
		GuestWriteDestination destination;
		const uint64_t bytes = GpuMemoryEopPublicationBytes(publication_kind);
		if (!destination.Capture(owned + 128, bytes, bytes)) return false;
		const auto generation = destination.Generation();
		struct ExclusiveProcessor
		{
			CommandProcessor* cp;
			explicit ExclusiveProcessor(CommandProcessor* value): cp(value) { cp->RunLock(); cp->Lock(); }
			~ExclusiveProcessor() { cp->Unlock(); cp->RunUnlock(); }
		} exclusive(cp);
		cp->SetQueue(GraphicContext::QUEUE_GFX);
		cp->ResetTrackedCommandBufferSubmissionTimeline();
		uint64_t epoch = 0, serial = 0, submitted = 0, completed = 0;
		if (!cp->BeginTrackedCommandBufferSubmission(GraphicContext::QUEUE_GFX, &epoch, &serial, &submitted, &completed) ||
		    !cp->RecordTrackedCommandBufferSubmission(GraphicContext::QUEUE_GFX, epoch, serial)) return false;
		cp->RecordTrackedCommandBufferCompletion(GraphicContext::QUEUE_GFX, epoch, serial);
		ReservationHandle reservation;
		MutationTransaction mutation;
		if (!BeginReserveAligned(0, 65536, 65536, &reservation, &mutation)) return false;
		uint32_t attempts = 0, batches = 0, waits = 0, stores = 0, unavailable = 0;
		bool valid = true;
		constexpr uint64_t value = 0x1122334455667788ull;
		const auto result = EopMaterializeWithLeaseRetry(cp, generation, [&] {
			++batches;
			GraphicsGuestLeaseResult acquisition;
			if (!destination.Validate(&acquisition))
			{
				const auto refusal = EopContinuityAcquisitionRefusal(acquisition);
				return GpuMemoryEopCheckpointResult {GpuMemoryEopCheckpointStatus::Rejected,
				    refusal.unavailable ? "target-preflight-lease-unavailable" : "target-changed-before-materialization",
				    refusal.unavailable};
			}
			if (!destination.Publish(&value)) return GpuMemoryEopCheckpointResult {GpuMemoryEopCheckpointStatus::Rejected, "fixture-target-refused"};
			++stores;
			return GpuMemoryEopCheckpointResult {GpuMemoryEopCheckpointStatus::Published, "fixture-published"};
		}, [&](bool) -> const char* {
			return cp->HasCompletedTrackedCommandBuffer(GraphicContext::QUEUE_GFX, epoch, serial) ? nullptr : "fixture-prefix-changed";
		}, [&] {
			++waits;
			valid = valid && !CurrentThreadHasMemoryLease();
			if ((scenario == 0 && waits == 2) || (scenario == 2 && waits == 1))
			{
				valid = mutation.Abort() && valid;
				if (scenario == 2) valid = Protect(owned, 65536, Mode::Read) && Protect(owned, 65536, Mode::ReadWrite) && valid;
			}
		}, &attempts, [&](uint64_t original) -> EopContinuityProbe {
			valid = valid && original == generation && destination.Generation() == generation;
			GraphicsGuestLeaseResult acquisition;
			if (destination.Validate(&acquisition)) return {true, false};
			const auto probe = EopContinuityAcquisitionRefusal(acquisition);
			if (probe.unavailable) ++unavailable;
			return probe;
		});
		valid = mutation.Abort() && valid;
		uint64_t observed = 0, left = 0, right = 0;
		std::memcpy(&observed, reinterpret_cast<const void*>(owned + 128), 8);
		std::memcpy(&left, reinterpret_cast<const void*>(owned + 120), 8);
		std::memcpy(&right, reinterpret_cast<const void*>(owned + 136), 8);
		const bool success = scenario == 0;
		const uint64_t expected = publication_kind == GpuMemoryEopPublicationKind::Immediate32 ? 0xa5a5a5a555667788ull : value;
		if (!valid || unavailable == 0 || batches != (success ? 2u : 1u) || stores != (success ? 1u : 0u) ||
		    attempts != (success ? 3u : scenario == 1 ? 16u : 1u) || waits != (success ? 2u : scenario == 1 ? 15u : 1u) ||
		    result.status != (success ? GpuMemoryEopCheckpointStatus::Published : GpuMemoryEopCheckpointStatus::Rejected) ||
		    result.retryable || std::strcmp(result.reason, success ? "fixture-published" :
		        scenario == 1 ? "materialization-retry-exhausted" : "mapping-changed-during-retry") != 0 ||
		    observed != (success ? expected : 0xa5a5a5a5a5a5a5a5ull) || left != 0xa5a5a5a5a5a5a5a5ull || right != 0xa5a5a5a5a5a5a5a5ull ||
		    CurrentThreadHasMemoryLease()) return false;
	}
	std::fprintf(stderr, "[GFX_EOP_CONTINUITY_DEFER_TEST] actual-mutation-borrow-denial=checked deferred-batch-restart=none"
	                     " recover-round3=pass exhaust-round16=reject waits-max15=pass original-token/prefix=retained"
	                     " protect/revert=reject store4/8-once=pass guards=preserved\n");
	std::fprintf(stderr, "[GFX_EOP_LEASE_RETRY_TEST] original-prefix=retained original-target=retained"
	                     " cp-mutex=released run-lock=held leases=none wait-cap=15 attempt-cap=16"
	                     " generation-change=reject remap=reject queue-or-epoch-change=reject nonretryable=reject clock64/immediate32=checked\n");
#else
	(void)cp;
#endif
	return true;
}

static bool graphics_run_db_cache_lease_retry_test(CommandProcessor* cp)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	Init();
	enum class Scenario { Recover, Exhaust, ProtectRevert, Remap, Unrelated, Context, ContextOwner, Parser, RunLock,
	                      Inventory, Census, Nonretryable, UnknownReason, FlagFalse, TicketAbsent, EffectsStarted,
	                      TokenUnavailable, HeldLease, InitialNonexclusive, ProveOnly, LeaseReturned, TicketGeneration,
	                      Identity };
	uint32_t cases = 0;
	for (unsigned index = 0; index != static_cast<unsigned>(Scenario::Identity) + 19; ++index)
	{
		const auto scenario = index < static_cast<unsigned>(Scenario::Identity) ? static_cast<Scenario>(index) : Scenario::Identity;
		const unsigned field = index - static_cast<unsigned>(Scenario::Identity);
		const uint64_t owned = Alloc(0, 65536, Mode::ReadWrite);
		if (owned == 0) return false;
		struct Owner { uint64_t address; ~Owner() { if (address != 0) Free(address); } } owner {owned};
		std::memset(reinterpret_cast<void*>(owned), 0, 65536);
		std::memset(reinterpret_cast<void*>(owned + 504), 0xa5, 24);
		const uint64_t planes[] {owned + 64, owned + 128, owned + 192};
		const uint64_t sizes[] {32, 32, 32};
		const uint64_t original_generation = GetMutationGeneration();
		const uint64_t operation_generation = scenario == Scenario::TokenUnavailable ? UINT64_MAX : original_generation;
		Core::Mutex renderer, tracker;
		struct ExclusiveProcessor
		{
			CommandProcessor* cp;
			explicit ExclusiveProcessor(CommandProcessor* value): cp(value) { cp->RunLock(); cp->Lock(); }
			~ExclusiveProcessor() { cp->Unlock(); cp->RunUnlock(); }
		} exclusive(cp);
		CommandProcessorRunStateScope parser_scope(cp);
		int buffer_key = 0, pool_key = 0, foreign_key = 0;
		EopInitialTargetIdentity original;
		original.processor = original.parent = cp; original.buffer = &buffer_key; original.pool = &pool_key;
		original.current_index = 0; original.pool_index = 0;
		original.processor_queue = original.buffer_queue = GraphicContext::QUEUE_GFX;
		original.submit_id = 9; original.processor_epoch = original.session = 3;
		original.assigned_serial = original.serial = 5; original.recording = 7; original.ordinal = 11;
		original.submitted_cutoff = 4; original.completed_cutoff = 2; original.exclusive = original.valid = true;
		auto current = original;
		if (scenario == Scenario::InitialNonexclusive) current.exclusive = false;
		auto* context = cp->GetCtx();
		const auto saved_context = *context;
		std::array<uint8_t, sizeof(HW::Context)> context_bytes;
		std::memcpy(context_bytes.data(), reinterpret_cast<const unsigned char*>(context), sizeof(*context));
		const HW::Context* context_owner = context;
		bool parser_owned = true, run_held = true, prepared = true, effects = false;
		uint64_t ticket_generation = original_generation;
		uint64_t inventory = 19, census = 23;
		GuestDataLease external;
		if (scenario == Scenario::HeldLease && !TryAcquireGuestDataLease(owned, 8, Mode::Read, &external, false)) return false;
		ReservationHandle reservation;
		MutationTransaction mutation;
		const bool deferred = scenario <= Scenario::Census || scenario == Scenario::Identity;
		if (deferred && !BeginReserveAligned(0, 65536, 65536, &reservation, &mutation)) return false;
		uint32_t rounds = 0, pauses = 0, calls = 0, waits = 0, barriers = 0, preparations = 0;
		bool valid = true;
		const auto result = DbCacheBarrierWithLeaseRetry(cp, operation_generation, [&]() -> GpuMemoryDbCacheProofResult {
			Core::LockGuard renderer_lock(renderer), tracker_lock(tracker);
			++calls;
			// Native authority and the retained inventory/census are modeled in
			// this driver fixture. Mapping sets, locking and byte scans are real.
			if (calls == 1) ++preparations;
			valid = valid && preparations == 1 && barriers == 0;
			GpuMemoryDbCacheProofResult proof {GpuMemoryDbCacheProofStatus::Rejected, "lease-set-retry-exhausted"};
			proof.original_generation = original_generation;
			proof.retryable = true;
			if (inventory != 19) { proof.reason = "fixture-original-inventory-changed"; proof.retryable = false; return proof; }
			if (census != 23) { proof.reason = "fixture-original-census-changed"; proof.retryable = false; return proof; }
			if (scenario == Scenario::Nonretryable) { proof.reason = "pair-or-mapping-changed"; return proof; }
			if (scenario == Scenario::UnknownReason) { proof.reason = nullptr; return proof; }
			if (scenario == Scenario::FlagFalse) { proof.retryable = false; return proof; }
			if (scenario == Scenario::TicketAbsent) { prepared = false; return proof; }
			if (scenario == Scenario::TicketGeneration) { ++ticket_generation; return proof; }
			if (scenario == Scenario::EffectsStarted) { effects = true; ++barriers; return proof; }
			if (scenario == Scenario::LeaseReturned)
			{
				valid = TryAcquireGuestDataLease(owned, 8, Mode::Read, &external, false) && valid;
				return proof;
			}
			const char* reason = nullptr;
			if (!DepthSurfaceUniformZeroBytes(planes, sizes, 3, original_generation, &reason))
			{
				proof.reason = reason;
				proof.retryable = reason != nullptr && std::strcmp(reason, "lease-set-retry-exhausted") == 0;
				return proof;
			}
			proof.status = scenario == Scenario::ProveOnly ? GpuMemoryDbCacheProofStatus::Proven : GpuMemoryDbCacheProofStatus::Recorded;
			proof.reason = "fixture-native-record-modeled";
			proof.retryable = false;
			if (scenario != Scenario::ProveOnly) { effects = true; ++barriers; }
			return proof;
		}, [&]() -> const char* {
			return DbCacheRetryScopeRejection(original, current, cp, run_held, parser_owned &&
			    g_command_processor_parser_state != nullptr && g_command_processor_parser_state->owner == cp,
			    context_owner, context_bytes, *context);
		}, [&] {
			++waits;
			valid = valid && !CurrentThreadHasMemoryLease() && barriers == 0 && preparations == 1;
			if (scenario != Scenario::Exhaust && (scenario != Scenario::Recover || waits == 2)) valid = mutation.Abort() && valid;
			std::thread observer([&] {
				const bool processor_available = cp->TryLock(), renderer_available = renderer.TryLock(), tracker_available = tracker.TryLock();
				const bool run_available = cp->TryRunLockForTest();
				valid = valid && processor_available && renderer_available && tracker_available && !run_available;
				if (run_available) cp->RunUnlock();
				if (processor_available && renderer_available && tracker_available)
				{
					if (scenario == Scenario::ProtectRevert)
						valid = Protect(owned, 65536, Mode::Read) && Protect(owned, 65536, Mode::ReadWrite) && valid;
					if (scenario == Scenario::Remap)
					{
						valid = Free(owned) && valid; owner.address = 0;
						valid = AllocFixed(owned, 65536, Mode::ReadWrite) && valid;
						if (valid) { owner.address = owned; std::memset(reinterpret_cast<void*>(owned), 0, 65536);
						             std::memset(reinterpret_cast<void*>(owned + 504), 0xa5, 24); }
					}
					if (scenario == Scenario::Unrelated)
					{
						const uint64_t unrelated = Alloc(0, 4096, Mode::ReadWrite);
						valid = unrelated != 0 && valid;
						if (unrelated != 0) valid = Free(unrelated) && valid;
					}
					if (scenario == Scenario::Context)
					{
						auto depth = context->GetDepthControl(); depth.raw ^= 1u; context->SetDepthControl(depth);
					}
					if (scenario == Scenario::ContextOwner) context_owner = &saved_context;
					if (scenario == Scenario::Parser) parser_owned = false;
					if (scenario == Scenario::RunLock) run_held = false;
					if (scenario == Scenario::Inventory) ++inventory;
					if (scenario == Scenario::Census) ++census;
					if (scenario == Scenario::Identity)
					{
						switch (field)
						{
							case 0: current.processor = &foreign_key; break;
							case 1: current.buffer = &foreign_key; break;
							case 2: current.parent = &foreign_key; break;
							case 3: current.pool = &foreign_key; break;
							case 4: ++current.current_index; break;
							case 5: ++current.processor_queue; break;
							case 6: ++current.buffer_queue; break;
							case 7: ++current.pool_index; break;
							case 8: ++current.submit_id; break;
							case 9: ++current.processor_epoch; break;
							case 10: ++current.assigned_serial; break;
							case 11: ++current.recording; break;
							case 12: ++current.session; break;
							case 13: ++current.serial; break;
							case 14: ++current.ordinal; break;
							case 15: ++current.submitted_cutoff; break;
							case 16: ++current.completed_cutoff; break;
							case 17: current.exclusive = false; break;
							case 18: current.valid = false; break;
						}
					}
				}
				if (tracker_available) tracker.Unlock();
				if (renderer_available) renderer.Unlock();
				if (processor_available) cp->Unlock();
			});
			observer.join();
		}, [] { return CurrentThreadHasMemoryLease(); }, [&](const GpuMemoryDbCacheProofResult& proof) {
			return prepared && !effects && ticket_generation == original_generation && proof.original_generation == ticket_generation;
		}, &rounds, &pauses);
		if (scenario == Scenario::HeldLease || scenario == Scenario::LeaseReturned)
			valid = external.IsActive() && CurrentThreadHasMemoryLease() && valid;
		ReleaseGuestDataLease(&external);
		valid = mutation.Abort() && valid;
		std::thread retained([&] {
			const bool processor_available = cp->TryLock(), renderer_available = renderer.TryLock(), tracker_available = tracker.TryLock();
			const bool run_available = cp->TryRunLockForTest();
			valid = valid && !processor_available && renderer_available && tracker_available && !run_available;
			if (run_available) cp->RunUnlock();
			if (tracker_available) tracker.Unlock();
			if (renderer_available) renderer.Unlock();
			if (processor_available) cp->Unlock();
		});
		retained.join();
		uint64_t left = 0, target = 0, right = 0;
		if (owner.address == 0) return false;
		std::memcpy(&left, reinterpret_cast<const void*>(owned + 504), 8);
		std::memcpy(&target, reinterpret_cast<const void*>(owned + 512), 8);
		std::memcpy(&right, reinterpret_cast<const void*>(owned + 520), 8);
		*context = saved_context;
		const bool success = scenario == Scenario::Recover || scenario == Scenario::Unrelated || scenario == Scenario::ProveOnly;
		const bool no_call = scenario == Scenario::TokenUnavailable || scenario == Scenario::HeldLease || scenario == Scenario::InitialNonexclusive;
		const uint32_t expected_calls = no_call ? 0u : scenario == Scenario::Recover ? 3u : scenario == Scenario::Exhaust ? 16u :
		    scenario == Scenario::ProtectRevert || scenario == Scenario::Remap || scenario == Scenario::Unrelated ||
		    scenario == Scenario::Inventory || scenario == Scenario::Census ? 2u : 1u;
		const uint32_t expected_waits = scenario == Scenario::Recover ? 2u : scenario == Scenario::Exhaust ? 15u : deferred ? 1u : 0u;
		const uint32_t expected_barriers = scenario == Scenario::Recover || scenario == Scenario::Unrelated || scenario == Scenario::EffectsStarted ? 1u : 0u;
		if (!valid || calls != expected_calls || rounds != calls || waits != expected_waits || pauses != waits ||
		    preparations != (no_call ? 0u : 1u) || barriers != expected_barriers || result.retryable ||
		    result.status != (success ? scenario == Scenario::ProveOnly ? GpuMemoryDbCacheProofStatus::Proven :
		        GpuMemoryDbCacheProofStatus::Recorded : GpuMemoryDbCacheProofStatus::Rejected) || CurrentThreadHasMemoryLease() ||
		    left != 0xa5a5a5a5a5a5a5a5ull || target != left || right != left ||
		    (scenario == Scenario::Exhaust && std::strcmp(result.reason, "db-cache-lease-set-retry-exhausted") != 0) ||
		    (scenario == Scenario::Nonretryable && std::strcmp(result.reason, "pair-or-mapping-changed") != 0) ||
		    (scenario == Scenario::UnknownReason && result.reason != nullptr))
		{
			std::fprintf(stderr, "[GFX_DB_CACHE_LEASE_RETRY_TEST] failed_case=%u field=%u valid=%u calls=%u rounds=%u waits=%u pauses=%u"
			    " preparations=%u barriers=%u status=%u retryable=%u first_cause=%s\n", index, field, valid ? 1u : 0u,
			    calls, rounds, waits, pauses, preparations, barriers, static_cast<unsigned>(result.status), result.retryable ? 1u : 0u,
			    result.reason != nullptr ? result.reason : "none");
			return false;
		}
		++cases;
	}
	std::fprintf(stderr, "[GFX_DB_CACHE_LEASE_RETRY_TEST] cases=%u real-VM/depth-set=checked cp-depth1=released/restored"
	    " renderer/tracker=unwound run-lock=held original-token=retained attempts-max16=pass pauses-max15=pass"
	    " identity19/context-object-bytes/parser/RunLock=revocation-checked preparation=once barrier-modeled-once"
	    " inventory/census/native-authority=modeled guest-write/irq/readback=none guards=preserved\n", cases);
#else
	(void)cp;
#endif
	return true;
}

bool GraphicsRunStateSynchronizationTest()
{
	// This focused executable does not construct the production subsystem graph,
	// but its dormant-GPU contract uses the same process-owned worker classes.
	Core::ThreadsSubsystem::Instance()->Init(nullptr);
	graphics_init_jmp_tables();

	// The production command processors intentionally live for the process lifetime,
	// and their destructor is fail-closed. Match that ownership in this short test.
	auto* cp = new CommandProcessor(64);
	cp->SetHeadlessTestMode();
	const bool exclusive_recording = graphics_run_exclusive_recording_test(cp);
	const bool submission_timeline = graphics_run_tracked_submission_timeline_test(cp);
	const bool submission_prefix_wait = graphics_run_submission_prefix_wait_test();
	const bool parser_pause_proof = graphics_run_parser_pause_proof_test(cp);
	const bool eop_lease_retry = graphics_run_eop_lease_retry_test(cp);
	const bool eop_cpu_proof_resume = graphics_run_eop_cpu_proof_resume_test(cp);
	const bool eop_materialized_cpu_resume = graphics_run_eop_materialized_cpu_resume_test(cp);
	const bool render_full_preflight_retry = graphics_run_render_full_preflight_retry_test(cp);
	const bool retained_recording_scope = graphics_run_retained_recording_scope_test(cp);
	const bool eop_initial_target_capture = graphics_run_eop_initial_target_capture_test(cp);
	const bool index_buffer_size = graphics_run_index_buffer_size_test(cp);
	const bool index_offset = graphics_run_index_offset_test(cp);
	const bool instance_step_rate = graphics_run_instance_step_rate_test(cp);
	const bool geometry_streamout_state = graphics_run_geometry_streamout_state_test(cp);
	const bool alpha_to_mask = graphics_run_alpha_to_mask_test(cp);
	const bool multi_vgt_param = graphics_run_multi_vgt_param_test(cp);
	const bool scan_mode_control_1 = graphics_run_scan_mode_control_1_test(cp);
	const bool aa_mask = graphics_run_aa_mask_test(cp);
	const bool depth_control_raw_state = graphics_run_depth_control_raw_state_test(cp);
	const bool htile_preload_state = graphics_run_htile_preload_state_test(cp);
	const bool clip_control = graphics_run_clip_control_test(cp);
	const bool guard_bands = graphics_run_guard_band_test(cp);
	const bool viewport_scissors = graphics_run_viewport_scissor_test(cp);
	const bool color_targets = graphics_run_color_target_test(cp);
	const bool administrative_packets = graphics_run_administrative_packet_test(cp);
	const bool polygon_offset = graphics_run_polygon_offset_test(cp);
	const bool experimental_hle_d32_default = graphics_run_experimental_hle_d32_default_test(cp);
	const bool blend_color = graphics_run_blend_color_test(cp);
	const bool dispatch_direct_packet = graphics_run_dispatch_direct_packet_test();
	const bool acquire_mem_global_writeback = graphics_run_acquire_mem_global_writeback_profile_test(cp);
	const bool acquire_mem_db_only = graphics_run_acquire_mem_db_only_test(cp);
	const bool shader_partial_flush = graphics_run_shader_partial_flush_test(cp);
	const bool db_cache_event = graphics_run_db_cache_event_test(cp);
	const bool db_cache_lease_retry = graphics_run_db_cache_lease_retry_test(cp);
	const bool gpu_wait_readmission = graphics_run_gpu_wait_readmission_test();
	const bool ready_counter_wait = cp->ReadyCounterWaitContractTest();
	const bool ready_memory_wait = graphics_run_ready_memory_wait_test(cp);
	const bool cb_meta_event = graphics_run_cb_meta_event_test(cp);
	const bool event_write_eop_completion = graphics_run_event_write_eop_completion_profile_test(cp);
	const bool pixel_done_write32 = graphics_run_pixel_done_write32_test(cp);

	auto wait_for = [](const std::atomic_bool& value)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!value.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
		{
			std::this_thread::yield();
		}
		return value.load(std::memory_order_acquire);
	};

	auto set_one_sgpr = [cp](uint32_t offset, uint32_t value)
	{
		uint32_t packet[2] = {offset, value};
		return cp_op_set_shader_reg(cp, LUMEN_PM4(3, Pm4::IT_SET_SH_REG, 0), packet, 3, 3) == 2;
	};

	const uint32_t descriptor[8] = {0x01020304u, 0x05060708u, 0x090a0b0cu, 0x91234567u,
	                                0x11121314u, 0x15161718u, 0x191a1b1cu, 0x1d1e1f20u};
	uint32_t       packet[9]     = {Pm4::SPI_SHADER_USER_DATA_PS_0};
	std::memcpy(packet + 1, descriptor, sizeof(descriptor));

	g_graphics_state_test_pause_requested.store(true, std::memory_order_relaxed);
	g_graphics_state_test_pause_reached.store(false, std::memory_order_relaxed);
	g_graphics_state_test_pause_release.store(false, std::memory_order_relaxed);
	std::atomic_bool writer_ok = false;
	std::thread writer(
	    [cp, &packet, &writer_ok]()
	    {
		    CommandProcessorRunStateScope parser_scope(cp);
		    cp->SetUserDataMarker(HW::UserSgprType::Region);
		    writer_ok.store(cp_op_set_shader_reg(cp, LUMEN_PM4(10, Pm4::IT_SET_SH_REG, 0), packet, 10, 10) == 9,
		                    std::memory_order_release);
	    });

	const bool pause_reached = wait_for(g_graphics_state_test_pause_reached);
	bool       packet_lock_held = false;
	if (pause_reached)
	{
		packet_lock_held = !cp->TryLock();
		if (!packet_lock_held)
		{
			cp->Unlock();
		}
	}
	g_graphics_state_test_pause_release.store(true, std::memory_order_release);
	writer.join();
	g_graphics_state_test_pause_requested.store(false, std::memory_order_relaxed);

	bool descriptor_complete = writer_ok.load(std::memory_order_acquire);
	cp->Lock();
	const auto& ps_sgprs = cp->GetShCtx()->GetPs().ps_user_sgpr;
	for (uint32_t i = 0; i < 8; i++)
	{
		descriptor_complete = descriptor_complete && ps_sgprs.value[i] == descriptor[i] &&
		                      ps_sgprs.type[i] == HW::UserSgprType::Region;
	}
	descriptor_complete = descriptor_complete && ps_sgprs.count >= 8;
	cp->Unlock();

	// Reproduce the observed failure shape without a worker: the packet layout
	// and Region marker remain stable while only the live SET_SH payload is
	// recycled to zero after the diagnostic submit shadow was captured.
	uint32_t submitted_descriptor_packet[] = {
	    LUMEN_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO),
	    0x6875000du,
	    LUMEN_PM4(10, Pm4::IT_SET_SH_REG, 0),
	    Pm4::SPI_SHADER_USER_DATA_PS_0,
	    descriptor[0],
	    descriptor[1],
	    descriptor[2],
	    descriptor[3],
	    descriptor[4],
	    descriptor[5],
	    descriptor[6],
	    descriptor[7],
	};
	uint32_t live_descriptor_packet[std::size(submitted_descriptor_packet)] = {};
	std::memcpy(live_descriptor_packet, submitted_descriptor_packet, sizeof(submitted_descriptor_packet));
	std::memset(live_descriptor_packet + 4, 0, sizeof(descriptor));
	cp->Run(live_descriptor_packet, static_cast<uint32_t>(std::size(live_descriptor_packet)), submitted_descriptor_packet,
	        CommandStreamKind::Draw);

	bool provenance_shadow = true;
	for (uint32_t i = 0; i < 8; ++i)
	{
		const auto provenance = cp->GetUserSgprWriterForTest(ShaderUserStage::Pixel, i);
		provenance_shadow = provenance_shadow && provenance.valid && provenance.parsed_value == 0 &&
		                    provenance.submitted_available && provenance.submitted_layout_same &&
		                    provenance.submitted_value == descriptor[i] && provenance.stream_kind == CommandStreamKind::Draw &&
		                    provenance.type == HW::UserSgprType::Region && provenance.packet_offset_dw == 2 &&
		                    provenance.packet_num_dw == 10 && provenance.depth == 0 &&
		                    provenance.stream_num_dw == std::size(live_descriptor_packet) &&
		                    provenance.stream_address == reinterpret_cast<uint64_t>(live_descriptor_packet);
	}
	cp->Lock();
	const auto& zero_ps_sgprs = cp->GetShCtx()->GetPs().ps_user_sgpr;
	for (uint32_t i = 0; i < 8; ++i)
	{
		provenance_shadow = provenance_shadow && zero_ps_sgprs.value[i] == 0 && zero_ps_sgprs.type[i] == HW::UserSgprType::Region;
	}
	cp->Unlock();

	const bool root_ownership_selector = exact_one_environment_value("1") && !exact_one_environment_value(nullptr) &&
	                                     !exact_one_environment_value("") && !exact_one_environment_value("0") &&
	                                     !exact_one_environment_value("01") && !exact_one_environment_value("10") &&
	                                     !exact_one_environment_value("true") &&
	                                     root_command_ownership_configuration_valid(false, false) &&
	                                     root_command_ownership_configuration_valid(true, false) &&
	                                     root_command_ownership_configuration_valid(false, true) &&
	                                     !root_command_ownership_configuration_valid(true, true);
	const bool root_ownership_direct =
	    root_command_ownership_decision(submitted_descriptor_packet, static_cast<uint32_t>(std::size(submitted_descriptor_packet)), nullptr,
	                                    0, true) == RootCommandOwnershipDecision::Eligible;
	uint32_t ccb_packet[] = {LUMEN_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO), 0x6875000du};
	const bool root_ownership_dcb_only =
	    root_command_ownership_decision(submitted_descriptor_packet, static_cast<uint32_t>(std::size(submitted_descriptor_packet)), ccb_packet,
	                                    static_cast<uint32_t>(std::size(ccb_packet)), true) ==
	    RootCommandOwnershipDecision::ConstBuffer;
	const bool root_ownership_quiescent =
	    root_command_ownership_decision(submitted_descriptor_packet, static_cast<uint32_t>(std::size(submitted_descriptor_packet)), nullptr,
	                                    0, false) == RootCommandOwnershipDecision::NotQuiescent;
	uint32_t indirect_packet[] = {LUMEN_PM4(4, Pm4::IT_INDIRECT_BUFFER, 0), 0, 0, 0};
	uint32_t ce_packet[]       = {LUMEN_PM4(2, Pm4::IT_INCREMENT_CE_COUNTER, 0), 1};
	uint32_t wait_packet[]     = {LUMEN_PM4(7, Pm4::IT_WAIT_REG_MEM, 0), 0, 0, 0, 0, 0, 0};
	uint32_t custom_wait_32[14] = {LUMEN_PM4(14, Pm4::IT_NOP, Pm4::R_WAIT_MEM_32)};
	uint32_t custom_wait_64[9]  = {LUMEN_PM4(9, Pm4::IT_NOP, Pm4::R_WAIT_MEM_64)};
	uint32_t wait_flip[7]        = {LUMEN_PM4(7, Pm4::IT_NOP, Pm4::R_WAIT_FLIP_DONE)};
	uint32_t malformed_packet  = LUMEN_PM4(2, Pm4::IT_SET_SH_REG, 0);
	uint32_t unsupported_packet[] = {LUMEN_PM4(7, Pm4::IT_DMA_DATA, 0), 0, 0, 0, 0, 0, 0};
	const bool root_ownership_rejects_dependencies =
	    root_command_ownership_decision(indirect_packet, static_cast<uint32_t>(std::size(indirect_packet)), nullptr, 0, true) ==
	        RootCommandOwnershipDecision::Indirect &&
	    root_command_ownership_decision(ce_packet, static_cast<uint32_t>(std::size(ce_packet)), nullptr, 0, true) ==
	        RootCommandOwnershipDecision::CeDe &&
	    root_command_ownership_decision(wait_packet, static_cast<uint32_t>(std::size(wait_packet)), nullptr, 0, true) ==
	        RootCommandOwnershipDecision::WaitDependency &&
	    root_command_ownership_decision(custom_wait_32, static_cast<uint32_t>(std::size(custom_wait_32)), nullptr, 0, true) ==
	        RootCommandOwnershipDecision::WaitDependency &&
	    root_command_ownership_decision(custom_wait_64, static_cast<uint32_t>(std::size(custom_wait_64)), nullptr, 0, true) ==
	        RootCommandOwnershipDecision::WaitDependency &&
	    root_command_ownership_decision(wait_flip, static_cast<uint32_t>(std::size(wait_flip)), nullptr, 0, true) ==
	        RootCommandOwnershipDecision::WaitDependency &&
	    root_command_ownership_decision(&malformed_packet, 1, nullptr, 0, true) == RootCommandOwnershipDecision::Malformed &&
	    root_command_ownership_decision(unsupported_packet, static_cast<uint32_t>(std::size(unsupported_packet)), nullptr, 0, true) ==
	        RootCommandOwnershipDecision::Unsupported &&
	    root_command_ownership_decision(&malformed_packet, ROOT_COMMAND_OWNERSHIP_MAX_DWORDS + 1u, nullptr, 0, true) ==
	        RootCommandOwnershipDecision::Capacity;

	uint32_t overlap_write[] = {LUMEN_PM4(5, Pm4::IT_WRITE_DATA, 0), 0x04100500u, 0, 0, 0xaabbccddu};
	const uint64_t overlap_address = reinterpret_cast<uint64_t>(overlap_write + 4);
	overlap_write[2]               = static_cast<uint32_t>(overlap_address);
	overlap_write[3]               = static_cast<uint32_t>(overlap_address >> 32u);
	uint32_t non_overlap_destination = 0;
	uint32_t non_overlap_write[] = {LUMEN_PM4(5, Pm4::IT_WRITE_DATA, 0), 0x04100500u, 0, 0, 0x11223344u};
	const uint64_t non_overlap_address = reinterpret_cast<uint64_t>(&non_overlap_destination);
	non_overlap_write[2]               = static_cast<uint32_t>(non_overlap_address);
	non_overlap_write[3]               = static_cast<uint32_t>(non_overlap_address >> 32u);
	Vector<uint32_t> copied_overlap_write;
	copied_overlap_write.Add(overlap_write, static_cast<uint32_t>(std::size(overlap_write)));
	const bool root_ownership_write_overlap =
	    root_command_ownership_decision(overlap_write, static_cast<uint32_t>(std::size(overlap_write)), nullptr, 0, true) ==
	        RootCommandOwnershipDecision::WriteOverlap &&
	    root_command_ownership_decision(copied_overlap_write.GetDataConst(), copied_overlap_write.Size(), nullptr, 0, true,
	                                    overlap_write) == RootCommandOwnershipDecision::WriteOverlap &&
	    root_command_ownership_decision(non_overlap_write, static_cast<uint32_t>(std::size(non_overlap_write)), nullptr, 0, true) ==
	        RootCommandOwnershipDecision::Eligible;
	const bool root_ownership_storage =
	    root_command_ownership_storage_available(0, ROOT_COMMAND_OWNERSHIP_MAX_DWORDS) &&
	    root_command_ownership_storage_available(ROOT_COMMAND_OWNERSHIP_MAX_TOTAL_DWORDS - ROOT_COMMAND_OWNERSHIP_MAX_DWORDS,
	                                             ROOT_COMMAND_OWNERSHIP_MAX_DWORDS) &&
	    !root_command_ownership_storage_available(ROOT_COMMAND_OWNERSHIP_MAX_TOTAL_DWORDS, 1) &&
	    !root_command_ownership_storage_available(ROOT_COMMAND_OWNERSHIP_MAX_TOTAL_DWORDS + 1u, 0);
	Vector<uint32_t> changed_but_eligible_guest;
	changed_but_eligible_guest.Add(submitted_descriptor_packet, static_cast<uint32_t>(std::size(submitted_descriptor_packet)));
	std::memset(changed_but_eligible_guest.GetData() + 4, 0, sizeof(descriptor));
	const auto changed_but_eligible_decision =
	    root_command_ownership_decision(changed_but_eligible_guest.GetDataConst(), changed_but_eligible_guest.Size(), nullptr, 0, true,
	                                    submitted_descriptor_packet);
	const bool root_ownership_copy_validation =
	    changed_but_eligible_decision == RootCommandOwnershipDecision::Eligible &&
	    !root_command_ownership_snapshot_valid(changed_but_eligible_decision, submitted_descriptor_packet,
	                                           changed_but_eligible_guest.GetDataConst(), changed_but_eligible_guest.Size()) &&
	    !root_command_ownership_snapshot_valid(RootCommandOwnershipDecision::Indirect, submitted_descriptor_packet,
	                                           submitted_descriptor_packet, static_cast<uint32_t>(std::size(submitted_descriptor_packet))) &&
	    root_command_ownership_snapshot_valid(RootCommandOwnershipDecision::Eligible, submitted_descriptor_packet,
	                                          submitted_descriptor_packet, static_cast<uint32_t>(std::size(submitted_descriptor_packet)));

	Vector<uint32_t> owned_descriptor_packet;
	owned_descriptor_packet.Add(submitted_descriptor_packet, static_cast<uint32_t>(std::size(submitted_descriptor_packet)));
	cp->Run(owned_descriptor_packet.GetData(), owned_descriptor_packet.Size(), nullptr, CommandStreamKind::Draw, live_descriptor_packet);
	bool root_ownership_preserves_payload =
	    command_processor_guest_data(owned_descriptor_packet.GetData(), live_descriptor_packet) == live_descriptor_packet;
	cp->Lock();
	const auto& owned_ps_sgprs = cp->GetShCtx()->GetPs().ps_user_sgpr;
	for (uint32_t i = 0; i < 8; ++i)
	{
		root_ownership_preserves_payload = root_ownership_preserves_payload && owned_ps_sgprs.value[i] == descriptor[i] &&
		                                   owned_ps_sgprs.type[i] == HW::UserSgprType::Region;
	}
	cp->Unlock();
	bool root_ownership_current_stream = false;
	{
		CommandProcessorStreamStateScope owned_scope(cp, owned_descriptor_packet.GetData(), nullptr, owned_descriptor_packet.Size(),
		                                             CommandStreamKind::Draw, true);
		root_ownership_current_stream = GraphicsRunCurrentCommandRootOwned();
	}
	root_ownership_current_stream = root_ownership_current_stream && !GraphicsRunCurrentCommandRootOwned();

	std::atomic_uint marker_ready = 0;
	std::atomic_bool marker_go    = false;
	std::atomic_bool ps_marker_ok = false;
	std::atomic_bool vs_marker_ok = false;
	std::thread ps_writer(
	    [cp, &marker_ready, &marker_go, &ps_marker_ok, &set_one_sgpr]()
	    {
		    CommandProcessorRunStateScope parser_scope(cp);
		    cp->SetUserDataMarker(HW::UserSgprType::Region);
		    marker_ready.fetch_add(1, std::memory_order_release);
		    while (!marker_go.load(std::memory_order_acquire))
		    {
			    std::this_thread::yield();
		    }
		    ps_marker_ok.store(set_one_sgpr(Pm4::SPI_SHADER_USER_DATA_PS_0 + 12, 0xa5a5a5a5u), std::memory_order_release);
	    });
	std::thread vs_writer(
	    [cp, &marker_ready, &marker_go, &vs_marker_ok, &set_one_sgpr]()
	    {
		    CommandProcessorRunStateScope parser_scope(cp);
		    cp->SetUserDataMarker(HW::UserSgprType::Vsharp);
		    marker_ready.fetch_add(1, std::memory_order_release);
		    while (!marker_go.load(std::memory_order_acquire))
		    {
			    std::this_thread::yield();
		    }
		    vs_marker_ok.store(set_one_sgpr(Pm4::SPI_SHADER_USER_DATA_VS_0 + 12, 0x5a5a5a5au), std::memory_order_release);
	    });
	const auto marker_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (marker_ready.load(std::memory_order_acquire) != 2 && std::chrono::steady_clock::now() < marker_deadline)
	{
		std::this_thread::yield();
	}
	const bool marker_ready_complete = marker_ready.load(std::memory_order_acquire) == 2;
	marker_go.store(true, std::memory_order_release);
	ps_writer.join();
	vs_writer.join();

	bool marker_isolated = marker_ready_complete && ps_marker_ok.load(std::memory_order_acquire) && vs_marker_ok.load(std::memory_order_acquire);
	cp->Lock();
	marker_isolated = marker_isolated && cp->GetShCtx()->GetPs().ps_user_sgpr.value[12] == 0xa5a5a5a5u &&
	                  cp->GetShCtx()->GetPs().ps_user_sgpr.type[12] == HW::UserSgprType::Region &&
	                  cp->GetShCtx()->GetVs().vs_user_sgpr.value[12] == 0x5a5a5a5au &&
	                  cp->GetShCtx()->GetVs().vs_user_sgpr.type[12] == HW::UserSgprType::Vsharp;
	cp->Unlock();

	bool nested_marker = false;
	{
		CommandProcessorRunStateScope outer_scope(cp);
		cp->SetUserDataMarker(HW::UserSgprType::Region);
		{
			CommandProcessorRunStateScope nested_scope(cp);
		}
		nested_marker = set_one_sgpr(Pm4::SPI_SHADER_USER_DATA_VS_0 + 13, 0x13579bdfu);
		cp->SetUserDataMarker(HW::UserSgprType::Vsharp);
		{
			CommandProcessorRunStateScope nested_scope(cp);
			cp->SetUserDataMarker(HW::UserSgprType::Region);
		}
		nested_marker = nested_marker && set_one_sgpr(Pm4::SPI_SHADER_USER_DATA_PS_0 + 13, 0x2468ace0u);
	}
	cp->Lock();
	nested_marker = nested_marker && cp->GetShCtx()->GetVs().vs_user_sgpr.type[13] == HW::UserSgprType::Region &&
	                cp->GetShCtx()->GetPs().ps_user_sgpr.type[13] == HW::UserSgprType::Region;
	cp->Unlock();

	bool reset_clears_marker = false;
	{
		CommandProcessorRunStateScope parser_scope(cp);
		cp->SetUserDataMarker(HW::UserSgprType::Vsharp);
		cp->Reset();
		reset_clears_marker = set_one_sgpr(Pm4::SPI_SHADER_USER_DATA_PS_0 + 14, 0x55aa55aau);
	}
	cp->Lock();
	reset_clears_marker = reset_clears_marker && cp->GetShCtx()->GetPs().ps_user_sgpr.type[14] == HW::UserSgprType::Unknown;
	cp->Unlock();

	uint32_t nested_set[] = {LUMEN_PM4(3, Pm4::IT_SET_SH_REG, 0), Pm4::SPI_SHADER_USER_DATA_PS_0 + 15, 0x0badc0deu};
	const auto nested_set_addr = reinterpret_cast<uint64_t>(nested_set);
	uint32_t outer_marker_then_indirect[] = {
	    LUMEN_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO),
	    0x6875000du,
	    0xc0023f02u,
	    static_cast<uint32_t>(nested_set_addr),
	    static_cast<uint32_t>(nested_set_addr >> 32u),
	    0x01800000u | static_cast<uint32_t>(std::size(nested_set)),
	};
	cp->Run(outer_marker_then_indirect, static_cast<uint32_t>(std::size(outer_marker_then_indirect)));

	uint32_t nested_marker_packet[] = {LUMEN_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO), 0x68750004u};
	const auto nested_marker_addr = reinterpret_cast<uint64_t>(nested_marker_packet);
	uint32_t outer_indirect_then_set[] = {
	    0xc0023f02u,
	    static_cast<uint32_t>(nested_marker_addr),
	    static_cast<uint32_t>(nested_marker_addr >> 32u),
	    0x01800000u | static_cast<uint32_t>(std::size(nested_marker_packet)),
	    LUMEN_PM4(3, Pm4::IT_SET_SH_REG, 0),
	    Pm4::SPI_SHADER_USER_DATA_VS_0 + 15,
	    0xc001d00du,
	};
	cp->Run(outer_indirect_then_set, static_cast<uint32_t>(std::size(outer_indirect_then_set)));
	cp->Lock();
	const bool indirect_marker_flow = cp->GetShCtx()->GetPs().ps_user_sgpr.value[15] == 0x0badc0deu &&
	                                  cp->GetShCtx()->GetPs().ps_user_sgpr.type[15] == HW::UserSgprType::Region &&
	                                  cp->GetShCtx()->GetVs().vs_user_sgpr.value[15] == 0xc001d00du &&
	                                  cp->GetShCtx()->GetVs().vs_user_sgpr.type[15] == HW::UserSgprType::Vsharp;
	cp->Unlock();

	cp->ResetDeCe();
	std::atomic_bool wait_started = false;
	std::atomic_bool wait_done    = false;
	cp->Lock();
	std::thread counter_waiter(
	    [cp, &wait_started, &wait_done]()
	    {
		    wait_started.store(true, std::memory_order_release);
		    cp->WaitCe();
		    wait_done.store(true, std::memory_order_release);
	    });
	const bool counter_started = wait_for(wait_started);
	cp->IncremenetCe();
	const bool wait_releases_state_lock = counter_started && wait_for(wait_done);
	cp->Unlock();
	counter_waiter.join();

	CommandProcessor::FlipInfo stale_flip {};
	stale_flip.handle    = 7;
	stale_flip.index     = 3;
	stale_flip.flip_mode = 2;
	stale_flip.flip_arg  = 99;
	cp->SetFlip(stale_flip);
	cp->SetSumbitId(42);
	cp->SetIndexType(1);
	const uint32_t stale_const_ram = 0xfeedbeefu;
	cp->WriteConstRam(0, &stale_const_ram, 1);
	cp->GetCtx()->SetIndexOffset(0xffffffffu);
	cp->GetCtx()->SetGsMode(0xffffffffu);
	cp->GetCtx()->SetStreamoutConfig(0x80000000u);
	cp->GetCtx()->SetAlphaToMask(0xffffffffu);
	cp->GetCtx()->SetMultiVgtParam(0x100ffu);
	cp->GetCtx()->SetScanModeControl1(0xffffffffu);
	cp->GetCtx()->SetAaMask(0, 0x00010001u);
	cp->GetCtx()->SetAaMask(1, 0u);
	cp->GetCtx()->SetInstanceStepRate(0, 0x11111111u);
	cp->GetCtx()->SetInstanceStepRate(1, 0x22222222u);
	cp->SetIndexBufferSize(0);
	const bool command_processor_session_reset = cp->ResetIOSRuntimeSession() && cp->IsIOSRuntimeSessionResetForTest() &&
	                                             cp->GetCtx()->GetIndexOffset() == 0 && cp->GetCtx()->IsIndexOffsetSupportedForDraw() &&
	                                             cp->GetCtx()->GetInstanceStepRate(0) == 0 && cp->GetCtx()->GetInstanceStepRate(1) == 0 &&
	                                             cp->GetCtx()->GetGsMode() == 0 && !cp->GetCtx()->IsGsModeConfigured() &&
	                                             cp->GetCtx()->GetStreamoutConfig() == 0 &&
	                                             !cp->GetCtx()->IsStreamoutConfigConfigured() &&
	                                             cp->GetCtx()->IsGeometryStreamoutSupportedForDraw();
	const bool draw_controls_session_reset = cp->GetCtx()->GetAlphaToMask() == 0 && cp->GetCtx()->IsAlphaToMaskSupportedForDraw() &&
	                                         cp->GetCtx()->GetMultiVgtParam() == 0xffu && cp->GetCtx()->IsMultiVgtParamSupportedForDraw() &&
	                                         cp->GetCtx()->GetScanModeControl1() == 0 && cp->GetCtx()->IsScanModeControl1SupportedForDraw() &&
	                                         cp->GetCtx()->GetAaMask(0) == 0xffffffffu && cp->GetCtx()->GetAaMask(1) == 0xffffffffu &&
	                                         cp->GetCtx()->IsAaMaskSupportedForDraw();
	cp->GetCtx()->SetIndexOffset(0x80000000u);
	cp->GetCtx()->SetGsMode(0x80000000u);
	cp->GetCtx()->SetStreamoutConfig(0xffffffffu);
	cp->SetIndexBufferSize(0xffffffffu);
	const bool command_processor_second_reset  = cp->ResetIOSRuntimeSession() && cp->IsIOSRuntimeSessionResetForTest() &&
	                                             cp->GetCtx()->GetIndexOffset() == 0 && cp->GetCtx()->IsIndexOffsetSupportedForDraw() &&
	                                             cp->GetCtx()->GetGsMode() == 0 && !cp->GetCtx()->IsGsModeConfigured() &&
	                                             cp->GetCtx()->GetStreamoutConfig() == 0 &&
	                                             !cp->GetCtx()->IsStreamoutConfigConfigured() &&
	                                             cp->GetCtx()->IsGeometryStreamoutSupportedForDraw();
	std::atomic_bool reset_locks_released       = false;
	std::thread reset_lock_probe(
	    [cp, &reset_locks_released]()
	    {
		    const bool state_lock = cp->TryLock();
		    if (state_lock)
		    {
			    cp->Unlock();
		    }
		    const bool run_lock = cp->TryRunLockForTest();
		    if (run_lock)
		    {
			    cp->RunUnlock();
		    }
		    reset_locks_released.store(state_lock && run_lock, std::memory_order_release);
	    });
	reset_lock_probe.join();

	// Model the post-vkDeviceWaitIdle dormant state without allocating a Vulkan
	// command buffer. The public wait wrapper must preserve m_current_buffer=-1;
	// otherwise BufferWait would attempt to initialize the headless pool here.
	const bool gpu_slot_available = g_gpu == nullptr;
	auto*      lifecycle_gpu      = gpu_slot_available ? new Gpu : nullptr;
	if (lifecycle_gpu != nullptr)
	{
		g_gpu = lifecycle_gpu;
	}
	const bool submission_admission_serialized =
	    lifecycle_gpu != nullptr && lifecycle_gpu->SubmissionAdmissionSerializationForTest();
	// ResetIOSRuntimeSession also sends a storage-style RetiredGpuObject through
	// GraphicsRunCommandProcessorWait while resetting is still true but every CP
	// has crossed ResetAfterDeviceIdle.
	const bool interrupt_routing = lifecycle_gpu != nullptr && lifecycle_gpu->InterruptRoutingForTest();
	const bool lifecycle_gpu_reset          = lifecycle_gpu != nullptr && lifecycle_gpu->ResetIOSRuntimeSession();
	bool       dormant_wait_preserves_reset = false;
	if (lifecycle_gpu_reset)
	{
		GraphicsRunWait();
		GraphicsRunCommandProcessorWait(cp);
		dormant_wait_preserves_reset = cp->IsIOSRuntimeSessionResetForTest();
	}

	// Compute-ring managers and their waiting workers are process-owned. Exercise
	// two guest mapping cycles without starting a worker or touching Vulkan.
	auto* compute_ring = new ComputeRing;
	uint32_t ring_data[8] = {};
	uint32_t read_ptr     = 0;
	const bool compute_initially_dormant = compute_ring->IsDormantIOSRuntimeSession();
	compute_ring->MapComputeQueue(ring_data, static_cast<uint32_t>(std::size(ring_data)), &read_ptr);
	const bool compute_first_active = compute_ring->CanResetIOSRuntimeSession() && !compute_ring->IsDormantIOSRuntimeSession();
	compute_ring->ResetIOSRuntimeSession();
	const bool compute_first_reset = compute_ring->IsDormantIOSRuntimeSession();
	compute_ring->MapComputeQueue(ring_data, static_cast<uint32_t>(std::size(ring_data)), &read_ptr);
	compute_ring->ResetIOSRuntimeSession();
	const bool compute_second_reset = compute_ring->IsDormantIOSRuntimeSession();

	return exclusive_recording && submission_timeline && submission_prefix_wait && parser_pause_proof && eop_lease_retry && eop_cpu_proof_resume && eop_materialized_cpu_resume && render_full_preflight_retry && retained_recording_scope && eop_initial_target_capture && index_buffer_size && index_offset && instance_step_rate && geometry_streamout_state && alpha_to_mask &&
	       multi_vgt_param &&
	       scan_mode_control_1 && aa_mask && draw_controls_session_reset && depth_control_raw_state && htile_preload_state && clip_control && guard_bands && viewport_scissors &&
	       color_targets && administrative_packets && polygon_offset && experimental_hle_d32_default && blend_color &&
	       dispatch_direct_packet && acquire_mem_global_writeback && acquire_mem_db_only && shader_partial_flush && db_cache_event && db_cache_lease_retry && cb_meta_event &&
	       event_write_eop_completion && pixel_done_write32 && interrupt_routing && gpu_wait_readmission &&
	       pause_reached && packet_lock_held && descriptor_complete && provenance_shadow && root_ownership_selector &&
	       root_ownership_direct && root_ownership_dcb_only && root_ownership_quiescent && root_ownership_rejects_dependencies &&
	       root_ownership_write_overlap && root_ownership_storage && root_ownership_copy_validation && root_ownership_preserves_payload &&
	       marker_isolated && nested_marker && root_ownership_current_stream && reset_clears_marker && indirect_marker_flow &&
	       wait_releases_state_lock && wait_done.load(std::memory_order_acquire) && ready_counter_wait && ready_memory_wait &&
	       command_processor_session_reset &&
	       command_processor_second_reset && reset_locks_released.load(std::memory_order_acquire) && gpu_slot_available &&
	       submission_admission_serialized && lifecycle_gpu_reset && dormant_wait_preserves_reset && compute_initially_dormant &&
	       compute_first_active && compute_first_reset && compute_second_reset;
}
#endif

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED
