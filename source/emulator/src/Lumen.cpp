#include "Lumen/Core/Core.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/MagicEnum.h"
#include "Lumen/Core/SDLSubsystem.h"
#include "Lumen/Core/Singleton.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Subsystems.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Vector.h"
#include "Lumen/Core/VirtualMemory.h"
#include "Lumen/Scripts/Scripts.h"
#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID && LUMEN_PLATFORM != LUMEN_PLATFORM_IOS
#include "Lumen/UnitTest.h"
#endif

#include "Emulator/Audio.h"
#include "Emulator/Common.h"
#include "Emulator/Config.h"
#include "Emulator/Controller.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Kernel/EventFlag.h"
#include "Emulator/Kernel/EventQueue.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Kernel/Semaphore.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/SystemContent.h"
#include "Emulator/Loader/Timer.h"
#include "Emulator/Network.h"
#include "Emulator/Profiler.h"
#include "Emulator/StartupTrace.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
#include <pthread.h>
#endif

namespace Lumen::Emulator {

#ifdef LUMEN_EMU_ENABLED

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
struct IOSGuestThreadState
{
	std::atomic_bool started    = false;
	std::atomic_bool finished   = false;
	std::atomic_uint references = 2;
};

static std::atomic_bool g_ios_presentation_teardown_safe = true;

static void ReleaseIOSGuestThreadState(IOSGuestThreadState* state)
{
	if (state != nullptr && state->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
	{
		delete state;
	}
}

static void MarkIOSGuestThreadFinished(void* data)
{
	auto* state = static_cast<IOSGuestThreadState*>(data);
	// A guest may return from its entry point (or execute raw guest exit) without
	// calling an HLE exit shim. Make every authoritative guest-thread completion
	// close the presentation loop as well as publishing the idle state.
	(void)Loader::RequestGuestSessionExit();
	Libs::LibKernel::PthreadFinishSelfForMainThread();
	Loader::GuestExecutionActivityEndAllForCurrentThread();
	state->finished.store(true, std::memory_order_release);
	ReleaseIOSGuestThreadState(state);
}

static void* RunIOSGuestThread(void* data)
{
	auto* state = static_cast<IOSGuestThreadState*>(data);
	pthread_cleanup_push(MarkIOSGuestThreadFinished, state);
	state->started.store(true, std::memory_order_release);
	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();
	rt->Execute();
	pthread_cleanup_pop(1);
	return nullptr;
}
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
struct AndroidGuestThreadState
{
	std::atomic_bool started    = false;
	std::atomic_bool finished   = false;
	std::atomic_uint references = 2;
};

static void ReleaseAndroidGuestThreadState(AndroidGuestThreadState* state)
{
	if (state != nullptr && state->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
	{
		delete state;
	}
}

static void MarkAndroidGuestThreadFinished(void* data)
{
	auto* state = static_cast<AndroidGuestThreadState*>(data);
	// A natural guest return must also wake the SDL presentation loop.
	(void)Loader::RequestGuestSessionExit();
	Loader::GuestExecutionActivityEndAllForCurrentThread();
	state->finished.store(true, std::memory_order_release);
	ReleaseAndroidGuestThreadState(state);
}

static void* RunAndroidGuestThread(void* data)
{
	auto* state = static_cast<AndroidGuestThreadState*>(data);
	pthread_cleanup_push(MarkAndroidGuestThreadFinished, state);
	state->started.store(true, std::memory_order_release);
	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();
	rt->Execute();
	pthread_cleanup_pop(1);
	return nullptr;
}
#endif

namespace LuaFunc {

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
static bool g_advanced_subsystems_initialized = false;
static bool g_atexit_registered               = false;
#endif

static void load_symbols(const String& id, Loader::RuntimeLinker* rt)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(rt == nullptr);
	if (!Libs::Init(id, rt->Symbols()))
	{
		EXIT("Unknown library: %s\n", id.C_Str());
	}
}

static void load_symbols_all(Loader::RuntimeLinker* rt)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(rt == nullptr);

	Libs::InitAll(rt->Symbols());
}

static void print_system_info()
{
	Core::SystemInfo info = Core::GetSystemInfo();

	printf("ProcessorName = %s\n", info.ProcessorName.C_Str());
}

static void lumen_close()
{
	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();

	rt->Clear();

	printf("done!\n");

	Core::SubsystemsListSingleton::Instance()->ShutdownAll();
}

static void Init(const Scripts::ScriptVar& cfg)
{
	EXIT_IF(!Core::Thread::IsMainThread());

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (g_advanced_subsystems_initialized)
	{
		Config::BeginSession();
		Config::Load(cfg);
		const auto width  = Config::GetScreenWidth();
		const auto height = Config::GetScreenHeight();
		Libs::Graphics::WindowInit(width, height);
		Libs::VideoOut::VideoOutInit(width, height);
		Libs::Graphics::GraphicsRunInit();
		Loader::Timer::Start();
		return;
	}
#endif

	auto* slist = Core::SubsystemsList::Instance();

	auto* audio       = Libs::Audio::AudioSubsystem::Instance();
	auto* config      = Config::ConfigSubsystem::Instance();
	auto* controller  = Libs::Controller::ControllerSubsystem::Instance();
	auto* core        = Core::CoreSubsystem::Instance();
	auto* file_system = Libs::LibKernel::FileSystem::FileSystemSubsystem::Instance();
	auto* graphics    = Libs::Graphics::GraphicsSubsystem::Instance();
	auto* log         = Log::LogSubsystem::Instance();
	auto* memory      = Libs::LibKernel::Memory::MemorySubsystem::Instance();
	auto* network     = Libs::Network::NetworkSubsystem::Instance();
	auto* profiler    = Profiler::ProfilerSubsystem::Instance();
	auto* pthread     = Libs::LibKernel::PthreadSubsystem::Instance();
	auto* sdl         = Core::SDLSubsystem::Instance();
	auto* scripts     = Scripts::ScriptsSubsystem::Instance();
	auto* timer       = Loader::Timer::TimerSubsystem::Instance();

	slist->Add(config, {core, scripts});
	slist->InitAll(true);

	Config::BeginSession();
	Config::Load(cfg);

	slist->Add(audio, {core, log, pthread, memory, sdl});
	slist->Add(controller, {core, log, config});
	slist->Add(file_system, {core, log, pthread});
	slist->Add(graphics, {core, log, pthread, memory, config, profiler, controller});
	slist->Add(log, {core, config});
	slist->Add(memory, {core, log});
	slist->Add(network, {core, log, pthread});
	slist->Add(profiler, {core, config});
	slist->Add(pthread, {core, log, timer});
	slist->Add(timer, {core, log});

	slist->InitAll(true);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	g_advanced_subsystems_initialized = true;
#endif
}

LUMEN_SCRIPT_FUNC(lumen_load_cfg_func)
{
	if (Scripts::ArgGetVarCount() != 1)
	{
		EXIT("invalid args\n");
	}

	Scripts::ScriptVar cfg = Scripts::ArgGetVar(0);

	Config::Load(cfg);

	return 0;
}

LUMEN_SCRIPT_FUNC(lumen_init_func)
{
	if (Scripts::ArgGetVarCount() != 1)
	{
		EXIT("invalid args\n");
	}
	StartupTrace::Emit("runtime", "init.begin");

	Scripts::ScriptVar cfg = Scripts::ArgGetVar(0);

	Init(cfg);

	print_system_info();

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
	if (!g_atexit_registered)
	{
		int ok = atexit(lumen_close);
		EXIT_NOT_IMPLEMENTED(ok != 0);
		g_atexit_registered = true;
	}
#else
	int ok = atexit(lumen_close);
	EXIT_NOT_IMPLEMENTED(ok != 0);
#endif
	StartupTrace::Emit("runtime", "init.end");

	return 0;
}

LUMEN_SCRIPT_FUNC(lumen_load_elf_func)
{
	if (Scripts::ArgGetVarCount() != 1 && Scripts::ArgGetVarCount() != 2 && Scripts::ArgGetVarCount() != 3)
	{
		EXIT("invalid args\n");
	}
	StartupTrace::Emit("runtime", "load_elf.begin");

	Scripts::ScriptVar elf = Scripts::ArgGetVar(0);

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();

	auto* program = rt->LoadProgram(Libs::LibKernel::FileSystem::GetRealFilename(elf.ToString()));

	if (Scripts::ArgGetVarCount() >= 2)
	{
		if (Scripts::ArgGetVar(1).ToInteger() == 1)
		{
			program->dbg_print_reloc = true;
		}
	}

	if (Scripts::ArgGetVarCount() >= 3)
	{
		auto save_name = Scripts::ArgGetVar(2).ToString();

		rt->SaveProgram(program, Libs::LibKernel::FileSystem::GetRealFilename(save_name));
	}
	StartupTrace::Emit("runtime", "load_elf.end");

	return 0;
}

LUMEN_SCRIPT_FUNC(lumen_save_main_elf_func)
{
	if (Scripts::ArgGetVarCount() != 1)
	{
		EXIT("invalid args\n");
	}

	Scripts::ScriptVar elf = Scripts::ArgGetVar(0);

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();

	rt->SaveMainProgram(Libs::LibKernel::FileSystem::GetRealFilename(elf.ToString()));

	return 0;
}

LUMEN_SCRIPT_FUNC(lumen_load_symbols_func)
{
	auto count = Scripts::ArgGetVarCount();

	if (count < 1)
	{
		EXIT("invalid args\n");
	}

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();

	for (int i = 0; i < count; i++)
	{
		Scripts::ScriptVar id = Scripts::ArgGetVar(i);
		load_symbols(id.ToString(), rt);
	}

	return 0;
}

LUMEN_SCRIPT_FUNC(lumen_load_symbols_all_func)
{
	auto count = Scripts::ArgGetVarCount();

	if (count != 0)
	{
		EXIT("invalid args\n");
	}
	StartupTrace::Emit("runtime", "load_symbols_all.begin");

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();

	load_symbols_all(rt);
	StartupTrace::Emit("runtime", "load_symbols_all.end");

	return 0;
}

LUMEN_SCRIPT_FUNC(lumen_load_param_sfo_func)
{
	auto count = Scripts::ArgGetVarCount();

	if (count != 1)
	{
		EXIT("invalid args\n");
	}
	StartupTrace::Emit("runtime", "load_param_sfo.begin");

	auto file_name = Scripts::ArgGetVar(0).ToString();

	if (!file_name.IsEmpty())
	{
		Loader::SystemContentLoadParamSfo(Scripts::ArgGetVar(0).ToString());
	}
	StartupTrace::Emit("runtime", "load_param_sfo.end");

	return 0;
}

LUMEN_SCRIPT_FUNC(lumen_dbg_dump_func)
{
	if (Scripts::ArgGetVarCount() != 1)
	{
		EXIT("invalid args\n");
	}

	Scripts::ScriptVar dbg_dir = Scripts::ArgGetVar(0);

	auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();

	rt->DbgDump(dbg_dir.ToString());

	return 0;
}

LUMEN_SCRIPT_FUNC(lumen_execute_func)
{
	if (Scripts::ArgGetVarCount() != 0)
	{
		EXIT("invalid args\n");
	}
	StartupTrace::Emit("runtime", "execute.begin");

	int thread_model = 1;

	if (thread_model == 0)
	{
		Core::Thread t([](void* /*unused*/) { Libs::Graphics::WindowRun(); }, nullptr);
		t.Detach();
		auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();
		rt->Execute();
	} else
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		Loader::ResetGuestExecutionStop();
		g_ios_presentation_teardown_safe.store(false, std::memory_order_release);
		auto* guest_thread_state = new IOSGuestThreadState;
		pthread_t guest_thread {};
		if (pthread_create(&guest_thread, nullptr, RunIOSGuestThread, guest_thread_state) != 0)
		{
			delete guest_thread_state;
			std::fprintf(stderr, "iOS runtime could not create the guest execution thread\n");
			std::fflush(stderr);
			return 0;
		}

		constexpr auto guest_stop_timeout = std::chrono::seconds(10);
		auto           guest_deadline = std::chrono::steady_clock::now() + guest_stop_timeout;
		while (!guest_thread_state->started.load(std::memory_order_acquire))
		{
			if (std::chrono::steady_clock::now() >= guest_deadline)
			{
				Loader::RequestGuestExecutionStop();
				(void)pthread_detach(guest_thread);
				ReleaseIOSGuestThreadState(guest_thread_state);
				std::fprintf(stderr, "iOS runtime guest thread did not start before the lifecycle deadline\n");
				std::fflush(stderr);
				return 0;
			}
			Core::Thread::SleepMicro(1000);
		}
		Libs::Graphics::WindowRun();
		guest_deadline = std::chrono::steady_clock::now() + guest_stop_timeout;
		while (!guest_thread_state->finished.load(std::memory_order_acquire) || !Loader::GuestExecutionIsIdle())
		{
			if (std::chrono::steady_clock::now() >= guest_deadline)
			{
				const bool dashboard_restored = Libs::Graphics::WindowAbortIOSRuntimeSession();
				const int  detach_result      = pthread_detach(guest_thread);
				ReleaseIOSGuestThreadState(guest_thread_state);
				std::fprintf(stderr,
				             "iOS runtime guest execution did not quiesce before the lifecycle deadline "
				             "(dashboard=%d detach=%d)\n",
				             dashboard_restored ? 1 : 0, detach_result);
				std::fflush(stderr);
				return 0;
			}
			Libs::Graphics::WindowDrainIOSRuntimeSession();
		}
		const int join_result = pthread_join(guest_thread, nullptr);
		ReleaseIOSGuestThreadState(guest_thread_state);
		if (join_result != 0)
		{
			const bool dashboard_restored = Libs::Graphics::WindowAbortIOSRuntimeSession();
			std::fprintf(stderr, "iOS runtime guest thread join failed (dashboard=%d result=%d)\n",
			             dashboard_restored ? 1 : 0, join_result);
			std::fflush(stderr);
			return 0;
		}
		g_ios_presentation_teardown_safe.store(Libs::Graphics::WindowFinishIOSRuntimeSession(), std::memory_order_release);
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		Loader::ResetGuestExecutionStop();
		auto* guest_thread_state = new AndroidGuestThreadState;
		pthread_t guest_thread {};
		if (pthread_create(&guest_thread, nullptr, RunAndroidGuestThread, guest_thread_state) != 0)
		{
			delete guest_thread_state;
			std::fprintf(stderr, "Android runtime could not create the guest execution thread\n");
			std::fflush(stderr);
			std::_Exit(EXIT_FAILURE);
		}

		constexpr auto guest_stop_timeout = std::chrono::seconds(10);
		auto guest_deadline = std::chrono::steady_clock::now() + guest_stop_timeout;
		while (!guest_thread_state->started.load(std::memory_order_acquire))
		{
			if (std::chrono::steady_clock::now() >= guest_deadline)
			{
				Loader::RequestGuestExecutionStop();
				std::fprintf(stderr, "Android runtime guest thread did not start before the lifecycle deadline\n");
				std::fflush(stderr);
				std::_Exit(EXIT_FAILURE);
			}
			Core::Thread::SleepMicro(1000);
		}

		Libs::Graphics::WindowRun();
		Loader::RequestGuestExecutionStop();
		guest_deadline = std::chrono::steady_clock::now() + guest_stop_timeout;
		while (!guest_thread_state->finished.load(std::memory_order_acquire) || !Loader::GuestExecutionIsIdle())
		{
			if (std::chrono::steady_clock::now() >= guest_deadline)
			{
				std::fprintf(stderr, "Android runtime guest execution did not quiesce before the lifecycle deadline\n");
				std::fflush(stderr);
				std::_Exit(EXIT_FAILURE);
			}
			Core::Thread::SleepMicro(1000);
		}

		const int join_result = pthread_join(guest_thread, nullptr);
		ReleaseAndroidGuestThreadState(guest_thread_state);
		if (join_result != 0)
		{
			std::fprintf(stderr, "Android runtime guest thread join failed (result=%d)\n", join_result);
			std::fflush(stderr);
			std::_Exit(EXIT_FAILURE);
		}
#else
		Core::Thread t(
		    [](void* /*unused*/)
		    {
			    auto* rt = Core::Singleton<Loader::RuntimeLinker>::Instance();
			    rt->Execute();
		    },
		    nullptr);
		Libs::Graphics::WindowRun();
		t.Join();
#endif
	}

	StartupTrace::Emit("runtime", "execute.end");
	return 0;
}

LUMEN_SCRIPT_FUNC(lumen_mount_func)
{
	if (Scripts::ArgGetVarCount() != 2)
	{
		EXIT("invalid args\n");
	}
	StartupTrace::Emit("runtime", "mount.begin");

	Scripts::ScriptVar folder = Scripts::ArgGetVar(0);
	Scripts::ScriptVar point  = Scripts::ArgGetVar(1);

	Libs::LibKernel::FileSystem::Mount(folder.ToString(), point.ToString());
	StartupTrace::Emit("runtime", "mount.end");

	return 0;
}

LUMEN_SCRIPT_FUNC(lumen_shader_disable)
{
	if (Scripts::ArgGetVarCount() != 1)
	{
		EXIT("invalid args\n");
	}

	auto id = Scripts::ArgGetVar(0).ToString().ToUint64(16);

	Libs::Graphics::ShaderDisable(id);

	return 0;
}

LUMEN_SCRIPT_FUNC(lumen_shader_printf)
{
	if (Scripts::ArgGetVarCount() != 4)
	{
		EXIT("invalid args\n");
	}

	auto id     = Scripts::ArgGetVar(0).ToString().ToUint64(16);
	auto pc     = Scripts::ArgGetVar(1).ToInteger();
	auto format = Scripts::ArgGetVar(2).ToString();
	auto args   = Scripts::ArgGetVar(3);

	if (!args.IsTable())
	{
		EXIT("invalid args\n");
	}

	Libs::Graphics::ShaderDebugPrintf p;
	p.pc     = pc;
	p.format = format;

	for (const auto& t: args.GetPairs())
	{
		const auto& arg_t = t.GetValue();

		if (!arg_t.IsTable())
		{
			EXIT("invalid arg\n");
		}

		auto type = arg_t.GetValue(0).ToString();
		auto arg  = arg_t.GetValue(1).ToString();

		Libs::Graphics::ShaderOperand op;

		if (arg.StartsWith('s', String::Case::Insensitive) && Core::Char::IsDecimal(arg.At(1)))
		{
			op.type        = Libs::Graphics::ShaderOperandType::Sgpr;
			op.register_id = arg.RemoveFirst(1).ToInt32();
			op.size        = 1;
		} else if (arg.StartsWith('v', String::Case::Insensitive) && Core::Char::IsDecimal(arg.At(1)))
		{
			op.type        = Libs::Graphics::ShaderOperandType::Vgpr;
			op.register_id = arg.RemoveFirst(1).ToInt32();
			op.size        = 1;
		} else
		{
			EXIT("unknown arg: %s\n", arg.C_Str());
		}

		p.args.Add(op);

		Libs::Graphics::ShaderDebugPrintf::Type st = Libs::Graphics::ShaderDebugPrintf::Type::Int;

		if (type.EqualNoCase(U"int"))
		{
			st = Libs::Graphics::ShaderDebugPrintf::Type::Int;
		} else if (type.EqualNoCase(U"uint"))
		{
			st = Libs::Graphics::ShaderDebugPrintf::Type::Uint;
		} else if (type.EqualNoCase(U"float"))
		{
			st = Libs::Graphics::ShaderDebugPrintf::Type::Float;
		} else
		{
			EXIT("unknown type: %s\n", arg.C_Str());
		}

		p.types.Add(st);
	}

	Libs::Graphics::ShaderInjectDebugPrintf(id, p);

	return 0;
}

#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID && LUMEN_PLATFORM != LUMEN_PLATFORM_IOS
LUMEN_SCRIPT_FUNC(lumen_run_tests)
{
	if (!UnitTest::unit_test_all())
	{
		EXIT("test failed\n");
	}

	return 0;
}
#endif

void lumen_help() {}

} // namespace LuaFunc

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
bool IOSRuntimeResetForNextSession()
{
	if (!g_ios_presentation_teardown_safe.load(std::memory_order_acquire) || !Loader::GuestExecutionIsIdle())
	{
		return false;
	}

	// Keep labels alive while command buffers release their retired wrappers.
	// The device is already idle, and GraphicsRun drains those wrappers outside
	// its GPU/processor locks; LabelDelete therefore only defers to the manager.
	if (!Libs::Graphics::LabelBeginSessionReset() || !Libs::Graphics::GraphicsRunResetIOSRuntimeSession() ||
	    !Libs::Graphics::LabelFinishSessionReset())
	{
		return false;
	}

	if (!Libs::Audio::ResetForNextSession() || !Libs::LibKernel::PthreadResetForNextSession() ||
	    !Libs::LibKernel::EventQueue::ResetForNextSession() || !Libs::VideoOut::VideoOutResetIOSRuntimeSession() ||
	    !Libs::Graphics::GraphicsResetGuestObjectsForNextSession() || !Libs::ResetSessionState() ||
	    !Libs::LibKernel::FileSystem::ResetForNextSession() || !Libs::Network::ResetForNextSession() ||
	    !Loader::SystemContentResetForNextSession() || !Libs::LibKernel::EventFlag::ResetForNextSession() ||
	    !Libs::LibKernel::Semaphore::ResetForNextSession() || !Libs::LibKernel::Memory::ResetForNextSession() ||
	    !Libs::ResetMemoryInstrumentationSessionState())
	{
		return false;
	}
	Libs::Controller::ControllerResetForNextSession();

	auto* runtime = Core::Singleton<Loader::RuntimeLinker>::Instance();
	return runtime != nullptr && runtime->ResetForNextSession() && Loader::GuestExecutionIsIdle();
}
#endif

void lumen_reg()
{
	Scripts::RegisterFunc("lumen_init", LuaFunc::lumen_init_func, LuaFunc::lumen_help);
	Scripts::RegisterFunc("lumen_load_cfg", LuaFunc::lumen_load_cfg_func, LuaFunc::lumen_help);
	Scripts::RegisterFunc("lumen_load_elf", LuaFunc::lumen_load_elf_func, LuaFunc::lumen_help);
	Scripts::RegisterFunc("lumen_save_main_elf", LuaFunc::lumen_save_main_elf_func, LuaFunc::lumen_help);
	Scripts::RegisterFunc("lumen_load_symbols", LuaFunc::lumen_load_symbols_func, LuaFunc::lumen_help);
	Scripts::RegisterFunc("lumen_load_symbols_all", LuaFunc::lumen_load_symbols_all_func, LuaFunc::lumen_help);
	Scripts::RegisterFunc("lumen_load_param_sfo", LuaFunc::lumen_load_param_sfo_func, LuaFunc::lumen_help);
	Scripts::RegisterFunc("lumen_dbg_dump", LuaFunc::lumen_dbg_dump_func, LuaFunc::lumen_help);
	Scripts::RegisterFunc("lumen_execute", LuaFunc::lumen_execute_func, LuaFunc::lumen_help);
	Scripts::RegisterFunc("lumen_mount", LuaFunc::lumen_mount_func, LuaFunc::lumen_help);
	Scripts::RegisterFunc("lumen_shader_disable", LuaFunc::lumen_shader_disable, LuaFunc::lumen_help);
	Scripts::RegisterFunc("lumen_shader_printf", LuaFunc::lumen_shader_printf, LuaFunc::lumen_help);
	#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID && LUMEN_PLATFORM != LUMEN_PLATFORM_IOS
	Scripts::RegisterFunc("lumen_run_tests", LuaFunc::lumen_run_tests, LuaFunc::lumen_help);
	#endif
}

#else
void lumen_reg() {}
#endif // LUMEN_EMU_ENABLED

} // namespace Lumen::Emulator
