#include "common/profiler.h"

#ifdef TRACY_ENABLE
#include "common/emulatorConfig.h"

#include <common/TracyProtocol.hpp>
#include <common/TracyVersion.hpp>
#include <cstdio>
#include <tracy/Tracy.hpp>
#endif

namespace Profiler {

void SetThreadName(const char* name) {
#ifdef TRACY_ENABLE
	if (tracy::ProfilerAvailable() && name != nullptr) {
		tracy::SetThreadName(name);
	}
#else
	(void)name;
#endif
}

void Initialize() {
#ifdef TRACY_ENABLE
	if (Config::ProfilerEnabled() && !tracy::ProfilerAvailable()) {
		tracy::StartupProfiler();
		TracySetProgramName("Lumen Gen5");
		::printf("Tracy profiler enabled: client %d.%d.%d, protocol %u, "
		         "broadcast %u, connect to 127.0.0.1:8086\n",
		         tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch,
		         tracy::ProtocolVersion, tracy::BroadcastVersion);
	}
#endif
}

void Shutdown() {
#ifdef TRACY_ENABLE
	if (tracy::ProfilerAvailable()) {
		tracy::ShutdownProfiler();
	}
#endif
}

} // namespace Profiler
