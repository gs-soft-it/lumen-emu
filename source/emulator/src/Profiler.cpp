#include "Emulator/Profiler.h"

#include "Lumen/Core/String.h"
#include "Lumen/Core/Subsystems.h"

#include "Emulator/Config.h"

#include <easy/profiler.h>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Profiler {

void Close()
{
	auto dir = Config::GetProfilerDirection();
	if (dir == Config::ProfilerDirection::File || dir == Config::ProfilerDirection::FileAndNetwork)
	{
		profiler::dumpBlocksToFile(Config::GetProfilerOutputFile().C_Str());
	}
}

LUMEN_SUBSYSTEM_INIT(Profiler)
{
	switch (Config::GetProfilerDirection())
	{
		case Config::ProfilerDirection::File: EASY_PROFILER_ENABLE; break;
		case Config::ProfilerDirection::Network: profiler::startListen(); break;
		case Config::ProfilerDirection::FileAndNetwork:
			EASY_PROFILER_ENABLE;
			profiler::startListen();
			break;
		case Config::ProfilerDirection::None:
		default: break;
	}
}

LUMEN_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Profiler)
{
	Close();
}

LUMEN_SUBSYSTEM_DESTROY(Profiler)
{
	Close();
}

} // namespace Lumen::Profiler

#endif // LUMEN_EMU_ENABLED
