// Contract test for the console-style module-load admission used by
// `sceKernelLoadStartModule`: one live instance per resolved module file, one
// load reference per request, and the last release handed to the caller so it
// may stop and unload the module. The seeded instances stand in for modules
// that are already live, so every admission below is a duplicate request.
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Singleton.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Log.h"
#include "Emulator/Loader/RuntimeLinker.h"

#include <cstdio>

namespace Loader = Lumen::Loader;

namespace {

bool Check(bool condition, const char* description)
{
	if (!condition) std::fprintf(stderr, "ModuleLoadDedupeTest: %s\n", description);
	return condition;
}

} // namespace

int main()
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Lumen::Core::VirtualMemory::Init();

	auto* linker = Lumen::Core::Singleton<Loader::RuntimeLinker>::Instance();

	const Lumen::String alpha_name = U"/synthetic/Media/Plugins/Alpha.prx";
	const Lumen::String beta_name  = U"/synthetic/Media/Plugins/Beta.prx";
	auto*               alpha      = linker->RegisterSyntheticProgramForTest(alpha_name);
	auto*               beta       = linker->RegisterSyntheticProgramForTest(beta_name);

	// A repeated request of a live file returns the same instance even when the
	// guest spelling only differs in case, and different files never collide.
	const auto repeat = linker->AcquireModuleLoadReference(U"/synthetic/media/plugins/ALPHA.PRX", true);
	const auto second = linker->AcquireModuleLoadReference(alpha_name, true);
	const auto other  = linker->AcquireModuleLoadReference(beta_name, true);

	const bool admitted =
	    Check(repeat.program == alpha && repeat.already_loaded, "repeated admission did not reuse the live instance") &&
	    Check(second.program == alpha && second.already_loaded, "same-spelling admission did not reuse the live instance") &&
	    Check(other.program == beta && other.already_loaded, "admission collided two different module files");

	// Two live references keep the module loaded and started; the release that
	// removes the last one reports that the caller may unload it.
	const bool references =
	    Check(!linker->ReleaseModuleLoadReference(alpha), "first release dropped a live reference") &&
	    Check(linker->ReleaseModuleLoadReference(alpha), "last release did not report the final reference") &&
	    Check(linker->ReleaseModuleLoadReference(alpha), "release past the last reference was rejected") &&
	    Check(linker->ReleaseModuleLoadReference(beta), "single-reference release did not report the final reference");

	linker->Clear();

	const bool passed = admitted && references;
	if (passed) std::puts("ModuleLoadDedupeTest: PASS");
	return passed ? 0 : 1;
}
