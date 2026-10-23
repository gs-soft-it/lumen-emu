#include "Lumen/Core/Subsystems.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Scripts/Scripts.h"

#include "Emulator/Audio.h"
#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Kernel/EventFlag.h"
#include "Emulator/Kernel/EventQueue.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Kernel/Semaphore.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/SystemContent.h"
#include "Emulator/Network.h"
#include "Emulator/Runtime.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

class CountingSubsystem final: public Lumen::Core::Subsystem
{
public:
	explicit CountingSubsystem(const char* id): m_id(id) {}

	const char* Id() override { return m_id; }

	void Init(Lumen::Core::SubsystemsList* /*parent*/) override
	{
		init_count++;
		if (fail_next_init)
		{
			fail_next_init = false;
			Fail("synthetic failure in %s", m_id);
		}
	}

	void Destroy(Lumen::Core::SubsystemsList* /*parent*/) override { destroy_count++; }
	void UnexpectedShutdown(Lumen::Core::SubsystemsList* /*parent*/) override {}

	const char* m_id;
	int         init_count      = 0;
	int         destroy_count   = 0;
	bool        fail_next_init  = false;
};

bool Check(bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "RuntimeLifecycleTest: %s\n", message);
		return false;
	}
	return true;
}

bool TestSubsystemRegistrationReset()
{
	CountingSubsystem first("first");
	CountingSubsystem second("second");
	CountingSubsystem failure("failure");
	Lumen::Core::SubsystemsList list;

	for (int cycle = 0; cycle < 2; cycle++)
	{
		list.Add(&first, {});
		list.Add(&second, {&first});
		if (!Check(list.InitAll(false), "subsystem graph did not initialize"))
		{
			return false;
		}
		if (!Check(!list.ResetRegistrations(), "reset accepted initialized subsystems"))
		{
			return false;
		}

		list.DestroyAll(false);
		if (!Check(list.ResetRegistrations(), "reset rejected a destroyed subsystem graph"))
		{
			return false;
		}
	}

	if (!Check(first.init_count == 2 && second.init_count == 2 && first.destroy_count == 2 && second.destroy_count == 2,
	           "subsystem callbacks did not complete two lifecycles"))
	{
		return false;
	}

	failure.fail_next_init = true;
	list.Add(&failure, {});
	if (!Check(!list.InitAll(false), "synthetic subsystem failure was not reported") ||
	    !Check(list.GetFailName() != nullptr && std::strcmp(list.GetFailName(), "failure") == 0,
	           "failed subsystem name was not retained") ||
	    !Check(list.GetFailMsg() != nullptr && std::strstr(list.GetFailMsg(), "synthetic failure") != nullptr,
	           "failed subsystem message was not retained") ||
	    !Check(list.ResetRegistrations(), "failed, uninitialized subsystem registration did not reset"))
	{
		return false;
	}

	list.Add(&failure, {});
	if (!Check(list.InitAll(false), "stale subsystem failure survived registration reset"))
	{
		return false;
	}
	list.DestroyAll(false);
	return Check(list.ResetRegistrations(), "final subsystem registration reset failed");
}

struct DetachedThreadProbe
{
	std::atomic_bool release {false};
	std::atomic_bool completed {false};
};

void RunDetachedThreadProbe(void* data)
{
	auto* probe = static_cast<DetachedThreadProbe*>(data);
	while (!probe->release.load(std::memory_order_acquire))
	{
		Lumen::Core::Thread::SleepMicro(50);
	}
	probe->completed.store(true, std::memory_order_release);
}

void RunFastDetachedThreadProbe(void* data)
{
	static_cast<std::atomic_int*>(data)->fetch_add(1, std::memory_order_release);
}

bool TestDetachedThreadLifetime()
{
	// A detached Core::Thread is commonly a short-lived stack wrapper around a
	// persistent worker. The worker state must remain alive after that wrapper
	// leaves scope, including when the callback wins or loses the detach race.
	for (int cycle = 0; cycle < 64; ++cycle)
	{
		DetachedThreadProbe probe;
		{
			Lumen::Core::Thread thread(RunDetachedThreadProbe, &probe);
			thread.Detach();
		}
		probe.release.store(true, std::memory_order_release);
		for (int wait = 0; wait < 10000 && !probe.completed.load(std::memory_order_acquire); ++wait)
		{
			Lumen::Core::Thread::SleepMicro(50);
		}
		if (!Check(probe.completed.load(std::memory_order_acquire),
		           "detached worker did not outlive its stack wrapper"))
		{
			return false;
		}
	}
	std::atomic_int fast_completed {0};
	for (int cycle = 0; cycle < 64; ++cycle)
	{
		Lumen::Core::Thread thread(RunFastDetachedThreadProbe, &fast_completed);
		thread.Detach();
	}
	for (int wait = 0; wait < 10000 && fast_completed.load(std::memory_order_acquire) != 64; ++wait)
	{
		Lumen::Core::Thread::SleepMicro(50);
	}
	if (!Check(fast_completed.load(std::memory_order_acquire) == 64,
	           "short detached workers did not complete"))
	{
		return false;
	}
	Lumen::Core::Thread::Sleep(10);
	return true;
}

} // namespace

int main()
{
	if (!Check(LumenRuntimeExecutionFailureContractTest(), "guest backend failure was reported as clean runtime completion"))
		return 1;
	if (!TestSubsystemRegistrationReset())
	{
		return 1;
	}

	// The reset hooks use the production thread identity and silent logging
	// bootstrap, but do not initialize a physical audio or graphics backend.
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	if (!TestDetachedThreadLifetime())
	{
		return 1;
	}
	Lumen::Scripts::ScriptsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	if (!Check(Lumen::Scripts::RunString(
	               U"lumen_window_config_test = { WindowWidth = 960, WindowHeight = 540, WindowHighDpi = true }") ==
	               Lumen::Scripts::ScriptError::Ok,
	           "window presentation Lua fixture failed"))
	{
		return 1;
	}
	Lumen::Config::Load(Lumen::Scripts::GlobalGetVar(U"lumen_window_config_test"));
	if (!Check(Lumen::Config::GetWindowWidth() == 960 && Lumen::Config::GetWindowHeight() == 540 &&
	               Lumen::Config::WindowHighDpiEnabled(),
	           "window presentation keys were not loaded from Lua"))
	{
		return 1;
	}
	Lumen::Config::BeginSession();
	if (!Check(Lumen::Config::GetWindowWidth() == 0 && Lumen::Config::GetWindowHeight() == 0 &&
	               !Lumen::Config::WindowHighDpiEnabled(),
	           "window presentation keys survived the session reset"))
	{
		return 1;
	}
	if (!Check(Lumen::Libs::Graphics::GraphicsRenderPipelineCacheTombstoneContractTest(),
	           "graphics pipeline cache rejected tombstones during repeated session reset") ||
	    !Check(Lumen::Libs::Audio::AudioSessionResetContractTest(),
	           "audio services retained state across synthetic sessions") ||
	    !Check(Lumen::Libs::LibKernel::EventQueue::EventQueueSessionResetContractTest(),
	           "kernel event queues retained state across synthetic sessions") ||
	    !Check(Lumen::Libs::LibKernel::EventFlag::EventFlagSessionResetContractTest(),
	           "kernel event flags retained state across synthetic sessions") ||
	    !Check(Lumen::Libs::LibKernel::Semaphore::SemaphoreSessionResetContractTest(),
	           "kernel semaphores retained state across synthetic sessions") ||
	    !Check(Lumen::Libs::HleSessionResetContractTest(), "HLE retained guest-owned state across synthetic sessions") ||
	    !Check(Lumen::Libs::LibKernel::FileSystem::FileSystemSessionResetContractTest(),
	           "file system retained descriptors or mounts across synthetic sessions") ||
	    !Check(Lumen::Libs::Network::NetworkSessionResetContractTest(),
	           "network retained callback-bearing objects across synthetic sessions") ||
	    !Check(Lumen::Loader::SystemContentSessionResetContractTest(),
	           "system content retained metadata or icon state across synthetic sessions"))
	{
		return 1;
	}
	if (!Check(Lumen::Loader::RuntimeLinkerSessionResetContractTest(),
	           "RuntimeLinker did not rebuild two consecutive synthetic sessions"))
	{
		return 1;
	}

	std::puts("RuntimeLifecycleTest: PASS");
	return 0;
}
