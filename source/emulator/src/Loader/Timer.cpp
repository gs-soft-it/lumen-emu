#include "Lumen/Core/Timer.h"

#include "Lumen/Core/DateTime.h"
#include "Lumen/Core/Subsystems.h"

#include "Emulator/Common.h"
#include "Emulator/Loader/Timer.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Loader::Timer {

static Core::Timer g_timer;

LUMEN_SUBSYSTEM_INIT(Timer)
{
	Start();
}

LUMEN_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Timer) {}

LUMEN_SUBSYSTEM_DESTROY(Timer) {}

void Start()
{
	g_timer.Start();
}

double GetTimeMs()
{
	return g_timer.GetTimeMs();
}

Core::Time GetTime()
{
	return Core::Time(static_cast<int>(GetTimeMs()));
}

uint64_t GetCounter()
{
	return g_timer.GetTicks();
}

uint64_t GetFrequency()
{
	return g_timer.GetFrequency();
}

} // namespace Lumen::Loader::Timer

#endif // LUMEN_EMU_ENABLED
