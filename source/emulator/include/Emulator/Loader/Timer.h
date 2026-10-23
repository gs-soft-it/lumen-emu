#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_TIMER_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_TIMER_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/DateTime.h"
#include "Lumen/Core/Subsystems.h"

#include "Emulator/Common.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Loader::Timer {

LUMEN_SUBSYSTEM_DEFINE(Timer);

void       Start();
double     GetTimeMs();
Core::Time GetTime();
uint64_t   GetCounter();
uint64_t   GetFrequency();

} // namespace Lumen::Loader::Timer

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_TIMER_H_ */
