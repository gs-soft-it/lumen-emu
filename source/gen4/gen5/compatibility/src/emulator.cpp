#include "emulator.h"

#include "common/abi.h"
#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/loadingTrace.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/singleton.h"
#include "common/stringUtils.h"
#include "common/subsystems.h"
#include "common/systemInfo.h"
#include "common/threads.h"
#include "graphics/presentation/window.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "kytyGitVersion.h"
#include "libs/agc.h"
#include "libs/audio.h"
#include "libs/controller.h"
#include "libs/libs.h"
#include "libs/network.h"
#include "loader/guestExecution.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"
#if defined(LUMEN_PS5_FEXCORE)
#include "GuestCpuRuntime.h"
#endif
#include "loader/timer.h"

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace Emulator {

static void PrintSystemInfo() {
	const Common::SystemInfo info = Common::GetSystemInfo();

#if defined(__APPLE__)
	static constexpr auto platform_name = "macOS";
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	static constexpr auto platform_name = "Windows";
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	static constexpr auto platform_name = "Linux";
#else
	static constexpr auto platform_name = "Unknown";
#endif

	LOGF("Build\n"
	     "  version: %s\n\n"
	     "Host\n"
	     "  os:      %s\n"
	     "  cpu:     %s\n"
	     "  threads: %u\n\n",
	     KYTY_BUILD_LABEL, platform_name, info.ProcessorName.c_str(),
	     std::thread::hardware_concurrency());
}

static void KytyClose() {
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	rt->Clear();

	LOGF("done!\n");

	Common::Subsystems::EmergencyShutdownActive();
}

static void MountOrCreateDir(const std::filesystem::path& dir, const std::string& point) {
	if (!Common::File::IsDirectoryExisting(dir)) {
		Common::File::CreateDirectories(dir);
	}

	EXIT_NOT_IMPLEMENTED(!Common::File::IsDirectoryExisting(dir));

	Libs::LibKernel::FileSystem::Mount(dir, point);
	auto dir_text = Common::PathToString(dir);
	LOGF("Mounted %s -> %s\n", point.c_str(), dir_text.c_str());
}

static void MountSandboxDirs() {
	std::string title_id;
	if (!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) {
		title_id = "UNKNOWN";
	}

	MountOrCreateDir("_DownloadData/" + title_id, "/download0");
	MountOrCreateDir("_TempData/" + title_id, "/temp0");
	MountOrCreateDir("_TempData/" + title_id, "/temp");
}

static bool ClearDirectoryContents(const std::filesystem::path& dir) {
	bool ok = true;

	for (const auto& entry: Common::File::GetDirEntries(dir)) {
		if (entry.name == "." || entry.name == "..") {
			continue;
		}

		auto path = dir / entry.name;

		if (entry.is_file) {
			Common::File::RemoveReadonly(path);
			ok = Common::File::DeleteFile(path) && ok;
		} else {
			ok = ClearDirectoryContents(path) && ok;
			ok = Common::File::DeleteDirectory(path) && ok;
		}
	}

	return ok;
}

static void ClearDebugTextureFolder() {
	const std::string debug_texture_folder = "_Textures";

	if (!Common::File::IsDirectoryExisting(debug_texture_folder)) {
		Common::File::CreateDirectories(debug_texture_folder);
		return;
	}

	if (!ClearDirectoryContents(debug_texture_folder)) {
		LOGF_COLOR(Log::Color::BrightYellow, "TextureDump: failed to completely clear %s\n",
		           debug_texture_folder.c_str());
	}
}

static void Init(const Config::ConfigOptions& cfg, const std::filesystem::path& param_json,
                 Common::Subsystems& subsystems) {
	EXIT_IF(!Common::Thread::IsMainThread());

	subsystems.Initialize<Config::Lifecycle>();
	Config::Load(cfg);
	subsystems.Initialize<Log::Lifecycle>();

	if (Common::File::IsFileExisting(param_json)) {
		Loader::SystemContentLoadParamSfo(param_json);
		if (const auto flexible_memory_size = Loader::SystemContentGetFlexibleMemorySize();
		    flexible_memory_size != 0) {
			Libs::LibKernel::Memory::SetFlexibleMemorySize(flexible_memory_size);
		}
	}

	// Initialization order is explicit; destruction is automatic and reversed.
	subsystems.Initialize<Loader::Timer::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::PthreadLifecycle>();
	subsystems.Initialize<Profiler::Lifecycle>();
	subsystems.Initialize<Libs::Network::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::FileSystem::Lifecycle>();
	subsystems.Initialize<Libs::Controller::Lifecycle>();
	subsystems.Initialize<Libs::Audio::Lifecycle>();
}

static void LoadElf(const std::filesystem::path& elf, bool dbg_print_reloc = false,
                    const std::filesystem::path& save_name = {}) {
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	auto* program = rt->LoadProgram(
	    Libs::LibKernel::FileSystem::GetRealFilename(Common::PathToGenericString(elf)));

	if (dbg_print_reloc) {
		program->dbg_print_reloc = true;
	}

	if (!save_name.empty()) {
		rt->SaveProgram(program, Libs::LibKernel::FileSystem::GetRealFilename(
		                             Common::PathToGenericString(save_name)));
	}
}

static void Execute(const std::filesystem::path& game_patch) {
	auto           patch_path = game_patch;
	Common::LoadingTrace::Phase execute_dispatch("execute_dispatch");
	Common::Thread guest_thread(
	    [](void* param) {
		    auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
		    Common::LoadingTrace::Milestone("guest_execute_begin");
		    rt->Execute(*static_cast<const std::filesystem::path*>(param));
	    },
	    &patch_path);
	execute_dispatch.Finish();
	Libs::Graphics::WindowRun();
	std::quick_exit(0);
}

void Run(const RunOptions& options) {
	Common::LoadingTrace::Initialize();
	if (options.app0_dir.empty()) {
		EXIT("app0 directory is required\n");
	}

	if (options.elf.empty()) {
		EXIT("ELF is required\n");
	}

#if !defined(__x86_64__) && !defined(_M_X64)
#if defined(LUMEN_PS5_FEXCORE)
	Common::LoadingTrace::Phase runtime_initialize("runtime_initialize");
	if (!Loader::GuestExecution::InitializeRuntime()) {
		std::fprintf(stderr, "PS5_RUNTIME_ADMISSION_DENIED: %s\n",
		             Loader::GuestExecution::FullRuntimeLimitation());
		std::fprintf(stderr,
		             "guest_execution=NOT_RUN guest_memory_loaded=false "
		             "graphics_initialized=false audio_initialized=false "
		             "controller_initialized=false\n");
		std::fflush(stderr);
		std::exit(EXIT_FAILURE);
	}
	runtime_initialize.Finish();
#else
	std::fprintf(stderr,
	             "PS5_RUNTIME_ADMISSION_DENIED: an x86-64 guest requires an admitted "
	             "FEX runtime on this host; guest_execution=NOT_RUN\n");
	std::fflush(stderr);
	std::exit(EXIT_FAILURE);
#endif
#endif

	const auto         param_json = options.app0_dir / "sce_sys" / "param.json";
	Common::Subsystems subsystems(true);
	{
		Common::LoadingTrace::Phase phase("subsystems_initialize");
		Init(options.config, param_json, subsystems);
	}

	{
		Common::LoadingTrace::Phase phase("clear_debug_textures");
		ClearDebugTextureFolder();
	}

	PrintSystemInfo();
	std::string title_id;
	if (Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) && !title_id.empty()) {
		Log::WriteToConsoleAndLog(fmt::format("Title ID: {}\n", title_id));
	}

	int ok = atexit(KytyClose);
	EXIT_NOT_IMPLEMENTED(ok != 0);

	// Guest threads are still running, so skip KytyClose() and only flush emergency state.
	ok = at_quick_exit(Common::Subsystems::EmergencyShutdownActive);
	EXIT_NOT_IMPLEMENTED(ok != 0);

	Libs::LibKernel::FileSystem::Mount(options.app0_dir, "/app0");
	Libs::LibKernel::FileSystem::Mount(options.app0_dir, "/hostapp");

	MountSandboxDirs();

	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
	rt->SetUpstreamFunctionFallback(options.upstream_import_fallback);
	Libs::InitAll(rt->Symbols());

#if defined(LUMEN_PS5_FEXCORE)
	Common::LoadingTrace::Phase runtime_prepare("runtime_prepare");
	if (!Loader::GuestExecution::PrepareRuntime()) {
		EXIT("PS5 typed guest runtime initialization failed\n");
	}
	runtime_prepare.Finish();
	std::fprintf(stderr, "PS5_CPU_RUNTIME_INITIALIZED guest_execution=ADMITTED backend=%s\n",
	             Lumen::GuestCpu::ProviderName());
#endif

	{
		Common::LoadingTrace::Phase phase("load_elf");
		LoadElf(options.elf);
	}
	// Resolve the complete extracted module set before device admission. No
	// module initializer or guest entry runs until graphics is ready below.
	{
		Common::LoadingTrace::Phase phase("prepare_programs");
		rt->PreparePrograms();
	}
	{
		Common::LoadingTrace::Phase phase("graphics_initialize");
		subsystems.Initialize<Libs::Graphics::Lifecycle>();
	}

	Execute(options.game_patch);
}

} // namespace Emulator
