#include "Emulator/Libs/Libs.h"

#include "Emulator/Config.h"
#include "Emulator/Log.h"
#include "Emulator/Libs/Ime.h"
#include "Emulator/Libs/ImeDialog.h"
#include "Emulator/Libs/Mouse.h"
#include "Emulator/Libs/Move.h"
#include "Emulator/Libs/Remoteplay.h"
#include "Emulator/Libs/NpRequests.h"
#include "Emulator/Libs/NpInGameMessage.h"
#include "Emulator/Libs/Rudp.h"
#include "Emulator/Libs/Ult.h"
#include "Emulator/Libs/Voice.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include "Lumen/Core/Threads.h"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <cinttypes>
#include <chrono>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

// Ring buffer for tracing last N function calls with return addresses
static constexpr int TRACE_SIZE = 64;

struct TraceEntry {
	const char* name;
	uint64_t    caller_addr;
	uint64_t    timestamp_ms;
};

static TraceEntry g_trace_buffer[TRACE_SIZE] = {};
static std::atomic<int> g_trace_index{0};

// Get current time in milliseconds
static uint64_t GetTimeMs()
{
	static auto start = std::chrono::steady_clock::now();
	auto now = std::chrono::steady_clock::now();
	return std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
}

// Track when Thread 14 sets up shaders
static std::atomic<bool> g_t14_after_vsshader{false};

bool TraceOrLogEnabled()
{
	return Config::DiagnosticsEnabled() || Log::GetDirection() != Log::Direction::Silent;
}

void TraceCallWithAddr(const char* name, uint64_t caller_addr)
{
	if (!Config::DiagnosticsEnabled())
	{
		return;
	}

	int idx = g_trace_index.fetch_add(1) % TRACE_SIZE;
	g_trace_buffer[idx].name = name;
	g_trace_buffer[idx].caller_addr = caller_addr;
	g_trace_buffer[idx].timestamp_ms = GetTimeMs();
	
	// Special Thread 14 tracing - just track in memory, no file I/O
	int tid = Core::Thread::GetThreadIdUnique();
	if (tid == 14)
	{
		// Check if this is the VS shader call
		if (name && strstr(name, "GraphicsSetVsShader"))
		{
			g_t14_after_vsshader.store(true);
		}
	}
}

void TraceCall(const char* name)
{
	TraceCallWithAddr(name, 0);
}

void DumpLastCalls()
{
	printf("=== Last %d API calls (with caller addresses) ===\n", TRACE_SIZE);
	int current = g_trace_index.load() % TRACE_SIZE;
	for (int i = 0; i < TRACE_SIZE; i++)
	{
		int idx = (current + i) % TRACE_SIZE;
		if (g_trace_buffer[idx].name != nullptr)
		{
			if (g_trace_buffer[idx].caller_addr != 0)
			{
				printf("  [%2d] %6" PRIu64 "ms  %-30s  caller: 0x%016" PRIx64 "\n", 
				       i, g_trace_buffer[idx].timestamp_ms, g_trace_buffer[idx].name, 
				       g_trace_buffer[idx].caller_addr);
			}
			else
			{
				printf("  [%2d] %6" PRIu64 "ms  %s\n", i, g_trace_buffer[idx].timestamp_ms, g_trace_buffer[idx].name);
			}
		}
	}
	printf("=================================================\n");
	fflush(stdout);
}

namespace LibcInternal {
LIB_DEFINE(InitLibcInternal_1);
void ResetSessionState();
} // namespace LibcInternal
namespace LibC {
void ResetSessionState();
}
namespace LibcInternalExt {
void ResetSessionState();
}
namespace Bgft {
void ResetSessionState();
}
namespace Fios2 {
void ResetSessionState();
}
namespace LibKernel {
void ResetSessionState();
#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool SessionResetContractTest();
#endif
}
namespace PlayGo {
void ResetSessionState();
}
namespace SaveData {
void ResetSessionState();
}
namespace PngDec {
void ResetSessionState();
}
namespace PngEnc {
void ResetSessionState();
}
namespace Ajm {
bool ResetForNextSession();
#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool SessionResetContractTest();
#endif
} // namespace Ajm
namespace UserService {
bool ResetForNextSession();
#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool SessionResetContractTest();
#endif
} // namespace UserService
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || \
     LUMEN_PLATFORM == LUMEN_PLATFORM_IOS) && defined(__aarch64__)
LIB_DEFINE(InitLibCArm64Hle_1);
#endif

LIB_DEFINE(InitAppContent_1);
LIB_DEFINE(InitAjm_1);
LIB_DEFINE(InitAudio_1);
LIB_DEFINE(InitBgft_1);
LIB_DEFINE(InitCoredump_1);
LIB_DEFINE(InitDbgAddressSanitizer_1);
LIB_DEFINE(InitDebug_1);
LIB_DEFINE(InitDialog_1);
namespace LibImeDialog { LIB_DEFINE(InitDialog_1_ImeDialog); }
LIB_DEFINE(InitDiscMap_1);
LIB_DEFINE(InitFiber_1);
LIB_DEFINE(InitFios2_1);
LIB_DEFINE(InitGraphicsDriver_1);
LIB_DEFINE(InitIme_1);
LIB_DEFINE(InitJobManager_1);
LIB_DEFINE(InitLibKernel_1);
LIB_DEFINE(InitNet_1);
LIB_DEFINE(InitNpToolkit2_1);
LIB_DEFINE(InitMouse_1);
LIB_DEFINE(InitMove_1);
LIB_DEFINE(InitPad_1);
LIB_DEFINE(InitPlayGo_1);
LIB_DEFINE(InitPngDec_1);
LIB_DEFINE(InitPngEnc_1);
LIB_DEFINE(InitRemoteplay_1);
LIB_DEFINE(InitRtc_1);
LIB_DEFINE(InitSaveData_1);
LIB_DEFINE(InitSysmodule_1);
LIB_DEFINE(InitSystemService_1);
LIB_DEFINE(InitUlt_1);
LIB_DEFINE(InitUserService_1);
LIB_DEFINE(InitVideoOut_1);

bool Init(const String& id, Loader::SymbolDatabase* s)
{
	LIB_CHECK(U"libAjm_1", InitAjm_1);
	LIB_CHECK(U"libAppContent_1", InitAppContent_1);
	LIB_CHECK(U"libAudio_1", InitAudio_1);
	LIB_CHECK(U"libBgft_1", InitBgft_1);
	LIB_CHECK(U"libc_internal_1", LibcInternal::InitLibcInternal_1);
	LIB_CHECK(U"libCoredump_1", InitCoredump_1);
	LIB_CHECK(U"libDbgAddressSanitizer_1", InitDbgAddressSanitizer_1);
	LIB_CHECK(U"libDebug_1", InitDebug_1);
	LIB_CHECK(U"libDialog_1", InitDialog_1);
	LIB_CHECK(U"libSceImeDialog_1", LibImeDialog::InitDialog_1_ImeDialog);
	LIB_CHECK(U"libImeDialog_1", LibImeDialog::InitDialog_1_ImeDialog);
	LIB_CHECK(U"libDiscMap_1", InitDiscMap_1);
	LIB_CHECK(U"libFiber_1", InitFiber_1);
	LIB_CHECK(U"libFios2_1", InitFios2_1);
	LIB_CHECK(U"libGraphicsDriver_1", InitGraphicsDriver_1);
	LIB_CHECK(U"libJobManager_1", InitJobManager_1);
	LIB_CHECK(U"libkernel_1", InitLibKernel_1);
	LIB_CHECK(U"libNet_1", InitNet_1);
	LIB_CHECK(U"libSceNpToolkit2_1", InitNpToolkit2_1);
	if (id == U"libSceIme_1")
	{
		if (!Ime::ExperimentalEnabled()) return false;
		InitIme_1(s);
		return true;
	}
	if (id == U"libMouse_1")
	{
		if (!Mouse::ExperimentalEnabled()) return false;
		InitMouse_1(s);
		return true;
	}
	if (id == U"libMove_1")
	{
		if (!Move::ExperimentalEnabled()) return false;
		InitMove_1(s);
		return true;
	}
	LIB_CHECK(U"libPad_1", InitPad_1);
	LIB_CHECK(U"libPlayGo_1", InitPlayGo_1);
	LIB_CHECK(U"libPngDec_1", InitPngDec_1);
	LIB_CHECK(U"libPngEnc_1", InitPngEnc_1);
	if (id == U"libRemoteplay_1")
	{
		if (!Remoteplay::ExperimentalEnabled()) return false;
		InitRemoteplay_1(s);
		return true;
	}
	LIB_CHECK(U"libRtc_1", InitRtc_1);
	LIB_CHECK(U"libSaveData_1", InitSaveData_1);
	LIB_CHECK(U"libSysmodule_1", InitSysmodule_1);
	LIB_CHECK(U"libSystemService_1", InitSystemService_1);
	if (id == U"libUlt_1")
	{
		if (!Ult::ExperimentalEnabled()) return false;
		InitUlt_1(s);
		return true;
	}
	LIB_CHECK(U"libUserService_1", InitUserService_1);
	LIB_CHECK(U"libVideoOut_1", InitVideoOut_1);

	return false;
}

void InitAll(Loader::SymbolDatabase* s)
{
	LIB_LOAD(InitAjm_1);
	LIB_LOAD(InitAudio_1);
	LIB_LOAD(InitBgft_1);
	LIB_LOAD(InitAppContent_1);
	LIB_LOAD(InitCoredump_1);
	LIB_LOAD(LibcInternal::InitLibcInternal_1);
	#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || \
	     LUMEN_PLATFORM == LUMEN_PLATFORM_IOS) && defined(__aarch64__)
	LIB_LOAD(InitLibCArm64Hle_1);
#endif
	LIB_LOAD(InitDbgAddressSanitizer_1);
	LIB_LOAD(InitDebug_1);
	LIB_LOAD(InitDialog_1);
	LIB_LOAD(InitDiscMap_1);
	LIB_LOAD(InitFiber_1);
	LIB_LOAD(InitFios2_1);
	LIB_LOAD(InitGraphicsDriver_1);
	LIB_LOAD(InitIme_1);
	LIB_LOAD(InitJobManager_1);
	LIB_LOAD(InitLibKernel_1);
	LIB_LOAD(InitNet_1);
	LIB_LOAD(InitNpToolkit2_1);
	LIB_LOAD(InitMouse_1);
	LIB_LOAD(InitMove_1);
	LIB_LOAD(InitPad_1);
	LIB_LOAD(InitPlayGo_1);
	LIB_LOAD(InitPngDec_1);
	LIB_LOAD(InitPngEnc_1);
	LIB_LOAD(InitRemoteplay_1);
	LIB_LOAD(InitRtc_1);
	LIB_LOAD(InitSaveData_1);
	LIB_LOAD(InitSysmodule_1);
	LIB_LOAD(InitSystemService_1);
	LIB_LOAD(InitUlt_1);
	LIB_LOAD(InitUserService_1);
	LIB_LOAD(InitVideoOut_1);

	// Diagnostic: verify RTTI symbols were registered
	{
		Loader::SymbolResolve test_sr {};
		test_sr.name                 = "pZ9WXcClPO8";
		test_sr.library              = "libc";
		test_sr.library_version      = 1;
		test_sr.module               = "libc";
		test_sr.module_version_major = 1;
		test_sr.module_version_minor = 1;
		test_sr.type                 = Loader::SymbolType::Object;
		auto key = Loader::SymbolDatabase::GenerateName(test_sr);
		auto* rec = s->Find(test_sr);
		printf("[INIT_DIAG] After InitAll: looking for key=%s\n", key.C_Str());
		printf("[INIT_DIAG]   Find result: %p\n", (void*)rec);
		if (rec) printf("[INIT_DIAG]   vaddr=0x%llX name=%s\n", (unsigned long long)rec->vaddr, rec->name.C_Str());
		else printf("[INIT_DIAG]   *** NOT FOUND — RTTI registration failed! ***\n");
		fflush(stdout);
	}

	// Only the exhaustive initializer closes the exact HLE universe. Partial
	// per-library initialization intentionally leaves the registry unsealed so
	// RuntimeLinker retains its conservative native-image fallback.
	s->SealHostCallRegistry();
}

bool ResetSessionState()
{
	// The caller has already stopped and joined every guest execution path, so
	// none of these callback slots can be entered while they are invalidated.
	std::memset(g_trace_buffer, 0, sizeof(g_trace_buffer));
	g_trace_index.store(0, std::memory_order_release);
	g_t14_after_vsshader.store(false, std::memory_order_release);

	// AJM owns decoder resources, so release it before resetting libc callback
	// slots. UserService only owns the per-session login-event latch.
	bool ok = Ajm::ResetForNextSession();
	if (!UserService::ResetForNextSession())
	{
		ok = false;
	}
	Fios2::ResetSessionState();
	LibC::ResetSessionState();
	LibcInternalExt::ResetSessionState();
	LibcInternal::ResetSessionState();
	LibKernel::ResetSessionState();
	Bgft::ResetSessionState();
	PlayGo::ResetSessionState();
	SaveData::ResetSessionState();
	PngDec::ResetSessionState();
	PngEnc::ResetSessionState();
	Remoteplay::ResetSessionState();
	NpRequests::ResetSessionState();
	NpInGameMessage::ResetSessionState();
	Rudp::ResetSessionState();
	Ult::ResetSessionState();
	Voice::ResetSessionState();
	Ime::ResetSessionState();
	Dialog::ImeDialog::ResetSessionState();
	Mouse::ResetSessionState();
	Move::ResetSessionState();
	return ok;
}

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool HleSessionResetContractTest()
{
	return Ajm::SessionResetContractTest() && UserService::SessionResetContractTest() &&
	       LibKernel::SessionResetContractTest();
}
#endif

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
