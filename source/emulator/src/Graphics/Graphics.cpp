#include "Emulator/Graphics/Graphics.h"

#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/File.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/IndexBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/VertexBuffer.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/RuntimeLinker.h"

#include <algorithm>
#include <atomic>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

LUMEN_SUBSYSTEM_INIT(Graphics)
{
	auto width  = Config::GetScreenWidth();
	auto height = Config::GetScreenHeight();

	WindowInit(width, height);
	VideoOut::VideoOutInit(width, height);
	GraphicsRenderInit();
	GraphicsRunInit();
	GpuMemoryInit();
	LabelInit();
	TileInit();
	IndexBufferInit();
	ShaderInit();
}

LUMEN_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Graphics) {}

LUMEN_SUBSYSTEM_DESTROY(Graphics) {}

bool GraphicsResetGuestObjectsForNextSession()
{
	auto* ctx = WindowGetGraphicContext();
	if (ctx == nullptr || !GpuMemoryResetForNextSession(ctx))
	{
		return false;
	}

	// Index-buffer destruction is intentionally deferred by its GpuMemory
	// callback; consume that queue before guest mappings disappear.
	IndexBufferDeleteAll(ctx);
	const bool dynamic_vertex_finalized = VertexBufferReportDynamicHostVisibleFinal();
	const bool graphics_reset = GraphicsRenderResetForNextSession() && ShaderResetForNextSession();
	if (!dynamic_vertex_finalized || !graphics_reset)
	{
		return false;
	}
	VertexBufferResetDynamicHostVisibleForNextSession();
	return true;
}

void GraphicsDbgDumpDcb(const char* type, uint32_t num_dw, uint32_t* cmd_buffer)
{
	EXIT_IF(type == nullptr);

	static std::atomic_int id = 0;

	if (Config::CommandBufferDumpEnabled() && num_dw > 0 && cmd_buffer != nullptr)
	{
		Core::File f;
		String     file_name = Config::GetCommandBufferDumpFolder().FixDirectorySlash() +
		                   String::FromPrintf("%04d_%04d_buffer_%s.log", GraphicsRunGetFrameNum(), id++, type);
		Core::File::CreateDirectories(file_name.DirectoryWithoutFilename());
		f.Create(file_name);
		if (f.IsInvalid())
		{
			printf(FG_BRIGHT_RED "Can't create file: %s\n" FG_DEFAULT, file_name.C_Str());
			return;
		}
		Pm4::DumpPm4PacketStream(&f, cmd_buffer, 0, num_dw);
		f.Close();
	}
}

namespace Gen4 {

LIB_NAME("GraphicsDriver", "GraphicsDriver");

int LUMEN_SYSV_ABI GraphicsSetVsShader(uint32_t* cmd, uint64_t size, const uint32_t* vs_regs, uint32_t shader_modifier)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < sizeof(HW::VsStageRegisters) / 4 + 2);

	printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size            = %" PRIu64 "\n", size);
	printf("\t shader_modifier = %" PRIu32 "\n", shader_modifier);

	printf("\t m_spiShaderPgmLoVs    = %08" PRIx32 "\n", vs_regs[0]);
	printf("\t m_spiShaderPgmHiVs    = %08" PRIx32 "\n", vs_regs[1]);
	printf("\t m_spiShaderPgmRsrc1Vs = %08" PRIx32 "\n", vs_regs[2]);
	printf("\t m_spiShaderPgmRsrc2Vs = %08" PRIx32 "\n", vs_regs[3]);
	printf("\t m_spiVsOutConfig      = %08" PRIx32 "\n", vs_regs[4]);
	printf("\t m_spiShaderPosFormat  = %08" PRIx32 "\n", vs_regs[5]);
	printf("\t m_paClVsOutCntl       = %08" PRIx32 "\n", vs_regs[6]);

	cmd[0] = LUMEN_PM4(size, Pm4::IT_NOP, Pm4::R_VS);
	cmd[1] = shader_modifier;
	memcpy(&cmd[2], vs_regs, static_cast<size_t>(7) * 4);

	return OK;
}

int LUMEN_SYSV_ABI GraphicsUpdateVsShader(uint32_t* cmd, uint64_t size, const uint32_t* vs_regs, uint32_t shader_modifier)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < sizeof(HW::VsStageRegisters) / 4 + 2);

	printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size            = %" PRIu64 "\n", size);
	printf("\t shader_modifier = %" PRIu32 "\n", shader_modifier);

	printf("\t m_spiShaderPgmLoVs    = %08" PRIx32 "\n", vs_regs[0]);
	printf("\t m_spiShaderPgmHiVs    = %08" PRIx32 "\n", vs_regs[1]);
	printf("\t m_spiShaderPgmRsrc1Vs = %08" PRIx32 "\n", vs_regs[2]);
	printf("\t m_spiShaderPgmRsrc2Vs = %08" PRIx32 "\n", vs_regs[3]);
	printf("\t m_spiVsOutConfig      = %08" PRIx32 "\n", vs_regs[4]);
	printf("\t m_spiShaderPosFormat  = %08" PRIx32 "\n", vs_regs[5]);
	printf("\t m_paClVsOutCntl       = %08" PRIx32 "\n", vs_regs[6]);

	cmd[0] = LUMEN_PM4(size, Pm4::IT_NOP, Pm4::R_VS_UPDATE);
	cmd[1] = shader_modifier;
	memcpy(&cmd[2], vs_regs, static_cast<size_t>(7) * 4);

	return OK;
}

int LUMEN_SYSV_ABI GraphicsSetEmbeddedVsShader(uint32_t* cmd, uint64_t size, uint32_t id, uint32_t shader_modifier)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 3);

	printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size            = %" PRIu64 "\n", size);
	printf("\t id              = %" PRIu32 "\n", id);
	printf("\t shader_modifier = %" PRIu32 "\n", shader_modifier);

	cmd[0] = LUMEN_PM4(size, Pm4::IT_NOP, Pm4::R_VS_EMBEDDED);
	cmd[1] = shader_modifier;
	cmd[2] = id;

	return OK;
}

int32_t LUMEN_SYSV_ABI GraphicsSetEmbeddedPsShader(uint32_t* cmd, uint32_t size, int32_t id)
{
	PRINT_NAME();

	// The public API requires 40 DWORDs of command space and defines the
	// predefined shader IDs 0 and 1. Invalid parameters fail without touching
	// the buffer; the measured sibling PS builder returns 0xffffffff for an
	// insufficient capacity and preserves the caller's storage.
	if (cmd == nullptr || size < 40 || id < 0 || id > 1) return -1;

	// Predefined shader 1 ("empty shader exporting 32 bit R and G") has no
	// measured export contract or local host model yet, so it stays fail-closed
	// instead of publishing a command the renderer cannot represent.
	EXIT_NOT_IMPLEMENTED(id != 0);

	printf("\t cmd_buffer = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size       = %" PRIu32 "\n", size);
	printf("\t id         = %" PRId32 "\n", id);

	// The modeled command declares the documented 40-DWORD span and stores the
	// predefined shader ID in its first payload DWORD. Later words stay
	// untouched, matching the native padding that preserves callers' storage.
	const uint32_t packet[2] = {LUMEN_PM4(40, Pm4::IT_NOP, Pm4::R_PS_EMBEDDED), 0};
	bool           written   = false;
#if defined(__aarch64__) || (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	written = Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(cmd), packet, sizeof(packet));
#endif
	if (!written)
	{
		EXIT("Gnm SetEmbeddedPsShader command buffer is not writable");
	}

	return OK;
}

int LUMEN_SYSV_ABI GraphicsSetPsShader(uint32_t* cmd, uint64_t size, const uint32_t* ps_regs)
{
	PRINT_NAME();

	if (ps_regs == nullptr)
	{
		EXIT_NOT_IMPLEMENTED(size < 1 + 1);

		printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
		printf("\t size            = %" PRIu64 "\n", size);
		printf("\t embedded_id     = %d\n", 0);

		cmd[0] = LUMEN_PM4(size, Pm4::IT_NOP, Pm4::R_PS_EMBEDDED);
		cmd[1] = 0;
	} else
	{
		EXIT_NOT_IMPLEMENTED(size < 12 + 1);

		printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
		printf("\t size            = %" PRIu64 "\n", size);

		printf("\t m_spiShaderPgmLoPs    = %08" PRIx32 "\n", ps_regs[0]);
		printf("\t m_spiShaderPgmHiPs    = %08" PRIx32 "\n", ps_regs[1]);
		printf("\t m_spiShaderPgmRsrc1Ps = %08" PRIx32 "\n", ps_regs[2]);
		printf("\t m_spiShaderPgmRsrc2Ps = %08" PRIx32 "\n", ps_regs[3]);
		printf("\t m_spiShaderZFormat    = %08" PRIx32 "\n", ps_regs[4]);
		printf("\t m_spiShaderColFormat  = %08" PRIx32 "\n", ps_regs[5]);
		printf("\t m_spiPsInputEna       = %08" PRIx32 "\n", ps_regs[6]);
		printf("\t m_spiPsInputAddr      = %08" PRIx32 "\n", ps_regs[7]);
		printf("\t m_spiPsInControl      = %08" PRIx32 "\n", ps_regs[8]);
		printf("\t m_spiBarycCntl        = %08" PRIx32 "\n", ps_regs[9]);
		printf("\t m_dbShaderControl     = %08" PRIx32 "\n", ps_regs[10]);
		printf("\t m_cbShaderMask        = %08" PRIx32 "\n", ps_regs[11]);

		cmd[0] = LUMEN_PM4(size, Pm4::IT_NOP, Pm4::R_PS);
		memcpy(&cmd[1], ps_regs, static_cast<size_t>(12) * 4);
	}

	return OK;
}

int LUMEN_SYSV_ABI GraphicsSetPsShader350(uint32_t* cmd, uint64_t size, const uint32_t* ps_regs)
{
	PRINT_NAME();

	if (ps_regs == nullptr)
	{
		EXIT_NOT_IMPLEMENTED(size < 1 + 1);

		printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
		printf("\t size            = %" PRIu64 "\n", size);
		printf("\t embedded_id     = %d\n", 0);

		cmd[0] = LUMEN_PM4(size, Pm4::IT_NOP, Pm4::R_PS_EMBEDDED);
		cmd[1] = 0;
	} else
	{
		EXIT_NOT_IMPLEMENTED(size < 12 + 1);

		printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
		printf("\t size            = %" PRIu64 "\n", size);

		printf("\t m_spiShaderPgmLoPs    = %08" PRIx32 "\n", ps_regs[0]);
		printf("\t m_spiShaderPgmHiPs    = %08" PRIx32 "\n", ps_regs[1]);
		printf("\t m_spiShaderPgmRsrc1Ps = %08" PRIx32 "\n", ps_regs[2]);
		printf("\t m_spiShaderPgmRsrc2Ps = %08" PRIx32 "\n", ps_regs[3]);
		printf("\t m_spiShaderZFormat    = %08" PRIx32 "\n", ps_regs[4]);
		printf("\t m_spiShaderColFormat  = %08" PRIx32 "\n", ps_regs[5]);
		printf("\t m_spiPsInputEna       = %08" PRIx32 "\n", ps_regs[6]);
		printf("\t m_spiPsInputAddr      = %08" PRIx32 "\n", ps_regs[7]);
		printf("\t m_spiPsInControl      = %08" PRIx32 "\n", ps_regs[8]);
		printf("\t m_spiBarycCntl        = %08" PRIx32 "\n", ps_regs[9]);
		printf("\t m_dbShaderControl     = %08" PRIx32 "\n", ps_regs[10]);
		printf("\t m_cbShaderMask        = %08" PRIx32 "\n", ps_regs[11]);

		cmd[0] = LUMEN_PM4(size, Pm4::IT_NOP, Pm4::R_PS);
		memcpy(&cmd[1], ps_regs, static_cast<size_t>(12) * 4);
	}

	// printf("ok\n");

	return OK;
}

int LUMEN_SYSV_ABI GraphicsUpdatePsShader(uint32_t* cmd, uint64_t size, const uint32_t* ps_regs)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(ps_regs == nullptr);
	EXIT_NOT_IMPLEMENTED(size < 12 + 1);

	printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size            = %" PRIu64 "\n", size);

	printf("\t m_spiShaderPgmLoPs    = %08" PRIx32 "\n", ps_regs[0]);
	printf("\t m_spiShaderPgmHiPs    = %08" PRIx32 "\n", ps_regs[1]);
	printf("\t m_spiShaderPgmRsrc1Ps = %08" PRIx32 "\n", ps_regs[2]);
	printf("\t m_spiShaderPgmRsrc2Ps = %08" PRIx32 "\n", ps_regs[3]);
	printf("\t m_spiShaderZFormat    = %08" PRIx32 "\n", ps_regs[4]);
	printf("\t m_spiShaderColFormat  = %08" PRIx32 "\n", ps_regs[5]);
	printf("\t m_spiPsInputEna       = %08" PRIx32 "\n", ps_regs[6]);
	printf("\t m_spiPsInputAddr      = %08" PRIx32 "\n", ps_regs[7]);
	printf("\t m_spiPsInControl      = %08" PRIx32 "\n", ps_regs[8]);
	printf("\t m_spiBarycCntl        = %08" PRIx32 "\n", ps_regs[9]);
	printf("\t m_dbShaderControl     = %08" PRIx32 "\n", ps_regs[10]);
	printf("\t m_cbShaderMask        = %08" PRIx32 "\n", ps_regs[11]);

	cmd[0] = LUMEN_PM4(size, Pm4::IT_NOP, Pm4::R_PS_UPDATE);
	memcpy(&cmd[1], ps_regs, static_cast<size_t>(12) * 4);

	return OK;
}

int LUMEN_SYSV_ABI GraphicsUpdatePsShader350(uint32_t* cmd, uint64_t size, const uint32_t* ps_regs)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(ps_regs == nullptr);
	EXIT_NOT_IMPLEMENTED(size < 12 + 1);

	printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size            = %" PRIu64 "\n", size);

	printf("\t m_spiShaderPgmLoPs    = %08" PRIx32 "\n", ps_regs[0]);
	printf("\t m_spiShaderPgmHiPs    = %08" PRIx32 "\n", ps_regs[1]);
	printf("\t m_spiShaderPgmRsrc1Ps = %08" PRIx32 "\n", ps_regs[2]);
	printf("\t m_spiShaderPgmRsrc2Ps = %08" PRIx32 "\n", ps_regs[3]);
	printf("\t m_spiShaderZFormat    = %08" PRIx32 "\n", ps_regs[4]);
	printf("\t m_spiShaderColFormat  = %08" PRIx32 "\n", ps_regs[5]);
	printf("\t m_spiPsInputEna       = %08" PRIx32 "\n", ps_regs[6]);
	printf("\t m_spiPsInputAddr      = %08" PRIx32 "\n", ps_regs[7]);
	printf("\t m_spiPsInControl      = %08" PRIx32 "\n", ps_regs[8]);
	printf("\t m_spiBarycCntl        = %08" PRIx32 "\n", ps_regs[9]);
	printf("\t m_dbShaderControl     = %08" PRIx32 "\n", ps_regs[10]);
	printf("\t m_cbShaderMask        = %08" PRIx32 "\n", ps_regs[11]);

	cmd[0] = LUMEN_PM4(size, Pm4::IT_NOP, Pm4::R_PS_UPDATE);
	memcpy(&cmd[1], ps_regs, static_cast<size_t>(12) * 4);

	return OK;
}

static int GraphicsSetCsShaderPacket(uint32_t* cmd, uint64_t size, const uint32_t* cs_regs, uint32_t shader_modifier)
{
	constexpr uint64_t packet_dwords = 25;
	EXIT_NOT_IMPLEMENTED(size != packet_dwords);

	printf("\t cmd_buffer      = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size            = %" PRIu64 "\n", size);
	printf("\t shader_modifier = %" PRIu32 "\n", shader_modifier);

	printf("\t m_computePgmLo      = %08" PRIx32 "\n", cs_regs[0]);
	printf("\t m_computePgmHi      = %08" PRIx32 "\n", cs_regs[1]);
	printf("\t m_computePgmRsrc1   = %08" PRIx32 "\n", cs_regs[2]);
	printf("\t m_computePgmRsrc2   = %08" PRIx32 "\n", cs_regs[3]);
	printf("\t m_computeNumThreadX = %08" PRIx32 "\n", cs_regs[4]);
	printf("\t m_computeNumThreadY = %08" PRIx32 "\n", cs_regs[5]);
	printf("\t m_computeNumThreadZ = %08" PRIx32 "\n", cs_regs[6]);

	cmd[0] = LUMEN_PM4(packet_dwords, Pm4::IT_NOP, Pm4::R_CS);
	cmd[1] = shader_modifier;
	memcpy(&cmd[2], cs_regs, static_cast<size_t>(7) * 4);

	return OK;
}

int LUMEN_SYSV_ABI GraphicsSetCsShader(uint32_t* cmd, uint64_t size, const uint32_t* cs_regs)
{
	PRINT_NAME();

	return GraphicsSetCsShaderPacket(cmd, size, cs_regs, 0);
}

int LUMEN_SYSV_ABI GraphicsSetCsShaderWithModifier(uint32_t* cmd, uint64_t size, const uint32_t* cs_regs, uint32_t shader_modifier)
{
	PRINT_NAME();

	return GraphicsSetCsShaderPacket(cmd, size, cs_regs, shader_modifier);
}

int LUMEN_SYSV_ABI GraphicsDrawIndex(uint32_t* cmd, uint64_t size, uint32_t index_count, const void* index_addr, uint32_t flags,
                                    uint32_t type)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 6);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);
	printf("\t index_count = %" PRIu32 "\n", index_count);
	printf("\t index_addr  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(index_addr));
	printf("\t flags       = %08" PRIx32 "\n", flags);
	printf("\t type        = %" PRIu32 "\n", type);

	cmd[0] = LUMEN_PM4(size, Pm4::IT_NOP, Pm4::R_DRAW_INDEX);
	cmd[1] = index_count;
	cmd[2] = static_cast<uint32_t>(reinterpret_cast<uint64_t>(index_addr) & 0xffffffffu);
	cmd[3] = static_cast<uint32_t>((reinterpret_cast<uint64_t>(index_addr) >> 32u) & 0xffffffffu);
	cmd[4] = flags;
	cmd[5] = type;

	return OK;
}

int LUMEN_SYSV_ABI GraphicsDrawIndexAuto(uint32_t* cmd, uint64_t size, uint32_t index_count, uint32_t flags)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 3);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);
	printf("\t index_count = %" PRIu32 "\n", index_count);
	printf("\t flags       = %08" PRIx32 "\n", flags);

	cmd[0] = LUMEN_PM4(size, Pm4::IT_NOP, Pm4::R_DRAW_INDEX_AUTO);
	cmd[1] = index_count;
	cmd[2] = flags;

	return OK;
}

int LUMEN_SYSV_ABI GraphicsInsertWaitFlipDone(uint32_t* cmd, uint64_t size, uint32_t video_out_handle, uint32_t display_buffer_index)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 3);

	printf("\t cmd_buffer           = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size                 = %" PRIu64 "\n", size);
	printf("\t video_out_handle     = %" PRIu32 "\n", video_out_handle);
	printf("\t display_buffer_index = %" PRIu32 "\n", display_buffer_index);

	cmd[0] = LUMEN_PM4(size, Pm4::IT_NOP, Pm4::R_WAIT_FLIP_DONE);
	cmd[1] = video_out_handle;
	cmd[2] = display_buffer_index;

	return OK;
}

int LUMEN_SYSV_ABI GraphicsDispatchDirect(uint32_t* cmd, uint64_t size, uint32_t thread_group_x, uint32_t thread_group_y,
                                         uint32_t thread_group_z, uint32_t mode)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 5);

	printf("\t cmd_buffer     = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size           = %" PRIu64 "\n", size);
	printf("\t thread_group_x = %" PRIu32 "\n", thread_group_x);
	printf("\t thread_group_y = %" PRIu32 "\n", thread_group_y);
	printf("\t thread_group_z = %" PRIu32 "\n", thread_group_z);
	printf("\t mode           = %" PRIu32 "\n", mode);

	cmd[0] = LUMEN_PM4(size, Pm4::IT_NOP, Pm4::R_DISPATCH_DIRECT);
	cmd[1] = thread_group_x;
	cmd[2] = thread_group_y;
	cmd[3] = thread_group_z;
	cmd[4] = mode;

	return OK;
}

uint32_t LUMEN_SYSV_ABI GraphicsDrawInitDefaultHardwareState(uint32_t* cmd, uint64_t size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 2);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);

	cmd[0] = LUMEN_PM4(2, Pm4::IT_NOP, Pm4::R_DRAW_RESET);

	return 2;
}

uint32_t LUMEN_SYSV_ABI GraphicsDrawInitDefaultHardwareState175(uint32_t* cmd, uint64_t size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 2);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);

	cmd[0] = LUMEN_PM4(2, Pm4::IT_NOP, Pm4::R_DRAW_RESET);

	return 2;
}

uint32_t LUMEN_SYSV_ABI GraphicsDrawInitDefaultHardwareState200(uint32_t* cmd, uint64_t size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 2);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);

	cmd[0] = LUMEN_PM4(2, Pm4::IT_NOP, Pm4::R_DRAW_RESET);

	return 2;
}

uint32_t LUMEN_SYSV_ABI GraphicsDrawInitDefaultHardwareState350(uint32_t* cmd, uint64_t size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 2);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);

	cmd[0] = LUMEN_PM4(2, Pm4::IT_NOP, Pm4::R_DRAW_RESET);

	return 2;
}

uint32_t LUMEN_SYSV_ABI GraphicsDispatchInitDefaultHardwareState(uint32_t* cmd, uint64_t size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 2);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);

	cmd[0] = LUMEN_PM4(2, Pm4::IT_NOP, Pm4::R_DISPATCH_RESET);

	return 2;
}

int LUMEN_SYSV_ABI GraphicsSubmitCommandBuffers(uint32_t count, void* dcb_gpu_addrs[], const uint32_t* dcb_sizes_in_bytes,
                                               void* ccb_gpu_addrs[], const uint32_t* ccb_sizes_in_bytes)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(count != 1);

	auto* dcb      = (dcb_gpu_addrs == nullptr ? nullptr : static_cast<uint32_t*>(dcb_gpu_addrs[0]));
	auto  dcb_size = (dcb_sizes_in_bytes == nullptr ? 0 : dcb_sizes_in_bytes[0] / 4);
	auto* ccb      = (ccb_gpu_addrs == nullptr ? nullptr : static_cast<uint32_t*>(ccb_gpu_addrs[0]));
	auto  ccb_size = (ccb_sizes_in_bytes == nullptr ? 0 : ccb_sizes_in_bytes[0] / 4);

	GraphicsDbgDumpDcb("d", dcb_size, dcb);
	GraphicsDbgDumpDcb("c", ccb_size, ccb);

	GraphicsRunSubmit(dcb, dcb_size, ccb, ccb_size);

	return OK;
}

int LUMEN_SYSV_ABI GraphicsSubmitAndFlipCommandBuffers(uint32_t count, void* dcb_gpu_addrs[], const uint32_t* dcb_sizes_in_bytes,
                                                      void* ccb_gpu_addrs[], const uint32_t* ccb_sizes_in_bytes, int handle, int index,
                                                      int flip_mode, int64_t flip_arg)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(count != 1);

	auto* dcb      = (dcb_gpu_addrs == nullptr ? nullptr : static_cast<uint32_t*>(dcb_gpu_addrs[0]));
	auto  dcb_size = (dcb_sizes_in_bytes == nullptr ? 0 : dcb_sizes_in_bytes[0] / 4);
	auto* ccb      = (ccb_gpu_addrs == nullptr ? nullptr : static_cast<uint32_t*>(ccb_gpu_addrs[0]));
	auto  ccb_size = (ccb_sizes_in_bytes == nullptr ? 0 : ccb_sizes_in_bytes[0] / 4);

	GraphicsDbgDumpDcb("d", dcb_size, dcb);
	GraphicsDbgDumpDcb("c", ccb_size, ccb);

	printf("\t handle    = %" PRId32 "\n", handle);
	printf("\t index     = %" PRId32 "\n", index);
	printf("\t flip_mode = %" PRId32 "\n", flip_mode);
	printf("\t flip_arg  = %" PRId64 "\n", flip_arg);

	GraphicsRunSubmitAndFlip(dcb, dcb_size, ccb, ccb_size, handle, index, flip_mode, flip_arg);

	return OK;
}

int LUMEN_SYSV_ABI GraphicsSubmitDone()
{
	PRINT_NAME();

	GraphicsRunDone();

	return OK;
}

int LUMEN_SYSV_ABI GraphicsAreSubmitsAllowed()
{
	return GraphicsRunAreSubmitsAllowed() ? 1 : 0;
}

void LUMEN_SYSV_ABI GraphicsFlushMemory()
{
	PRINT_NAME();

	GraphicsRunFlushMemory(WindowGetGraphicContext());
}

int LUMEN_SYSV_ABI GraphicsAddEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id, void* udata)
{
	PRINT_NAME();

	if (eq == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EBADF;
	}

	return GraphicsRenderAddEqEvent(eq, id, udata);
}

int LUMEN_SYSV_ABI GraphicsDeleteEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id)
{
	PRINT_NAME();

	if (eq == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EBADF;
	}

	return GraphicsRenderDeleteEqEvent(eq, id);
}

uint32_t LUMEN_SYSV_ABI GraphicsMapComputeQueue(uint32_t pipe_id, uint32_t queue_id, uint32_t* ring_addr, uint32_t ring_size_dw,
                                               uint32_t* read_ptr_addr)
{
	PRINT_NAME();

	printf("\t pipe_id       = %" PRIu32 "\n", pipe_id);
	printf("\t queue_id      = %" PRIu32 "\n", queue_id);
	printf("\t ring_addr     = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(ring_addr));
	printf("\t ring_size_dw  = %" PRIu32 "\n", ring_size_dw);
	printf("\t read_ptr_addr = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(read_ptr_addr));

	uint32_t id = GraphicsRunMapComputeQueue(pipe_id, queue_id, ring_addr, ring_size_dw, read_ptr_addr);

	printf("\t queue         = %" PRIu32 "\n", id);

	return id;
}

void LUMEN_SYSV_ABI GraphicsUnmapComputeQueue(uint32_t id)
{
	PRINT_NAME();

	printf("\t id = %" PRIu32 "\n", id);

	GraphicsRunUnmapComputeQueue(id);
}

int LUMEN_SYSV_ABI GraphicsComputeWaitOnAddress(uint32_t* cmd, uint64_t size, uint32_t* gpu_addr, uint32_t mask, uint32_t func, uint32_t ref)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 6);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);
	printf("\t gpu_addr    = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(gpu_addr));
	printf("\t mask        = %08" PRIx32 "\n", mask);
	printf("\t func        = %" PRIu32 "\n", func);
	printf("\t ref         = %08" PRIx32 "\n", ref);

	cmd[0] = LUMEN_PM4(size, Pm4::IT_NOP, Pm4::R_WAIT_MEM_32);
	cmd[1] = static_cast<uint32_t>(reinterpret_cast<uint64_t>(gpu_addr) & 0xffffffffu);
	cmd[2] = static_cast<uint32_t>((reinterpret_cast<uint64_t>(gpu_addr) >> 32u) & 0xffffffffu);
	cmd[3] = mask;
	cmd[4] = func;
	cmd[5] = ref;

	return OK;
}

void LUMEN_SYSV_ABI GraphicsDingDong(uint32_t ring_id, uint32_t offset_dw)
{
	if (ring_id == 0) return;

	PRINT_NAME();

	printf("\t ring_id   = %" PRIu32 "\n", ring_id);
	printf("\t offset_dw = %" PRIu32 "\n", offset_dw);

	GraphicsRunDingDong(ring_id, offset_dw);
}

int LUMEN_SYSV_ABI GraphicsInsertPushMarker(uint32_t* cmd, uint64_t size, const char* str)
{
	PRINT_NAME();

	auto len = strlen(str) + 1;

	EXIT_NOT_IMPLEMENTED(size * 4 < len + 1);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);
	printf("\t str         = %s\n", str);

	cmd[0] = LUMEN_PM4(size, Pm4::IT_NOP, Pm4::R_PUSH_MARKER);

	memcpy(cmd + 1, str, len);

	return OK;
}

int LUMEN_SYSV_ABI GraphicsInsertPopMarker(uint32_t* cmd, uint64_t size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(size < 2);

	printf("\t cmd_buffer  = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	printf("\t size        = %" PRIu64 "\n", size);

	cmd[0] = LUMEN_PM4(size, Pm4::IT_NOP, Pm4::R_POP_MARKER);

	return OK;
}

int32_t LUMEN_SYSV_ABI GraphicsSetVgtControl(uint32_t* command, uint32_t words, uint32_t a, uint32_t b, uint32_t c)
{
	if (command == nullptr || words != 3 || a > 255 || b > 1 || c > 1) return -1;

	bool written = false;
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	const uint32_t packet[3] = {0xc0016900u, 0x000002aau, a | (b << 16)};
	written = Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(command), packet, sizeof(packet));
#endif
	if (!written)
	{
		EXIT("Gnm SetVgtControl command buffer is not writable");
	}
	return 0;
}

uint64_t LUMEN_SYSV_ABI GraphicsGetGpuCoreClockFrequency()
{
	return LibKernel::KernelGetTscFrequency();
}

int LUMEN_SYSV_ABI GraphicsIsUserPaEnabled()
{
	return 0;
}

void* LUMEN_SYSV_ABI GraphicsGetTheTessellationFactorRingBufferBaseAddress()
{
	PRINT_NAME();

	auto addr = Core::VirtualMemory::AllocAligned(0, 0x20000, Core::VirtualMemory::Mode::ReadWrite, 256);
	Core::VirtualMemory::Free(addr);
	bool again = Core::VirtualMemory::AllocFixed(addr, 0x20000, Core::VirtualMemory::Mode::ReadWrite);
	EXIT_NOT_IMPLEMENTED(!again);
	Core::VirtualMemory::Free(addr);

	printf("\t addr = %016" PRIx64 "\n", addr);

	return reinterpret_cast<void*>(addr);
}

int LUMEN_SYSV_ABI GraphicsRegisterOwner(uint32_t* owner_handle, const char* name)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(owner_handle == nullptr);
	EXIT_NOT_IMPLEMENTED(name == nullptr);

	printf("\t RegisterOwner: %s\n", name);

	GpuMemoryRegisterOwner(owner_handle, name);

	printf("\t handler: %" PRIu32 "\n", *owner_handle);

	return OK;
}

int LUMEN_SYSV_ABI GraphicsRegisterResource(uint32_t* resource_handle, uint32_t owner_handle, const void* memory, size_t size,
                                           const char* name, uint32_t type, uint64_t user_data)
{
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(resource_handle == nullptr);
	EXIT_NOT_IMPLEMENTED(memory == nullptr);
	EXIT_NOT_IMPLEMENTED(name == nullptr);

	printf("\t RegisterResource: %s\n", name);
	printf("\t owner_handle:     %" PRIu32 "\n", owner_handle);
	printf("\t addr:             %016" PRIx64 "\n", reinterpret_cast<uint64_t>(memory));
	printf("\t size:             %" PRIu64 "\n", size);
	printf("\t type:             %" PRIu32 "\n", type);
	printf("\t user_data:        %" PRIu64 "\n", user_data);

	uint32_t rhandle = 0;

	GpuMemoryRegisterResource(&rhandle, owner_handle, memory, size, name, type, user_data);

	printf("\t handler: %" PRIu32 "\n", rhandle);

	if (resource_handle != nullptr)
	{
		*resource_handle = rhandle;
	}

	return OK;
}

int LUMEN_SYSV_ABI GraphicsUnregisterAllResourcesForOwner(uint32_t owner_handle)
{
	PRINT_NAME();

	printf("\t owner_handle:     %" PRIu32 "\n", owner_handle);

	GpuMemoryUnregisterAllResourcesForOwner(owner_handle);

	return OK;
}

int LUMEN_SYSV_ABI GraphicsUnregisterOwnerAndResources(uint32_t owner_handle)
{
	PRINT_NAME();

	printf("\t owner_handle:     %" PRIu32 "\n", owner_handle);

	GpuMemoryUnregisterOwnerAndResources(owner_handle);

	return OK;
}

int LUMEN_SYSV_ABI GraphicsUnregisterResource(uint32_t resource_handle)
{
	PRINT_NAME();

	printf("\t resource_handle:     %" PRIu32 "\n", resource_handle);

	GpuMemoryUnregisterResource(resource_handle);

	return OK;
}

} // namespace Gen4


} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED
