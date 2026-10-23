#ifndef LUMEN_EMULATOR_LIBS_RTC_H_
#define LUMEN_EMULATOR_LIBS_RTC_H_

#include "Emulator/Common.h"

#include <cstdint>

namespace Lumen::Core { class DateTime; }

namespace Lumen::Libs::Rtc {

struct SceRtcDateTime
{
	uint16_t year;
	uint16_t month;
	uint16_t day;
	uint16_t hour;
	uint16_t minute;
	uint16_t second;
	uint32_t microsecond;
};

struct SceRtcTick
{
	uint64_t tick;
};

// Original Lumen policy: encode one local civil-time snapshot, then check the
// complete guest output before writing. False is an internal result, not a
// console error code. Hosts without checked Loader writes fail closed.
[[nodiscard]] bool TryWriteLocalClock(uint64_t output_address, const Core::DateTime& local_time);

int LUMEN_SYSV_ABI sceRtcGetCurrentClockLocalTime(SceRtcDateTime* output);

// Original Lumen numbering: Sunday 0 through Saturday 6. This convention is
// not established by the public console declaration. Invalid input leaves
// the native result reference unchanged; no guest memory is accessed.
[[nodiscard]] bool TryGetDayOfWeek(int year, int month, int day, int& weekday);
int LUMEN_SYSV_ABI sceRtcGetDayOfWeek(int year, int month, int day);

// Original Lumen policy: interpret the tick as exact microseconds since the
// proleptic-Gregorian 0001-01-01 boundary used by the existing local RTC
// model. False leaves output unchanged; this is not a console error code.
[[nodiscard]] bool TryConvertTick(uint64_t tick, SceRtcDateTime& output);

// Original Lumen inverse of TryConvertTick: validate a complete civil record
// and encode exact microseconds from the same proleptic-Gregorian epoch.
// False leaves output unchanged; this is not a console error code.
[[nodiscard]] bool TryConvertDateTime(const SceRtcDateTime& input, uint64_t& output);

// Check complete, non-overlapping guest ranges before taking one civil-time
// snapshot and writing one complete Tick. Functional only where the Loader
// enforces guest mapping permissions.
[[nodiscard]] bool TryGetTick(uint64_t input_address, uint64_t output_address);
int LUMEN_SYSV_ABI sceRtcGetTick(const SceRtcDateTime* input, SceRtcTick* output);

// Check complete, non-overlapping guest ranges before taking one Tick
// snapshot and writing one complete civil record. Functional only where the
// Loader enforces guest mapping permissions.
[[nodiscard]] bool TrySetTick(uint64_t output_address, uint64_t tick_address);
int LUMEN_SYSV_ABI sceRtcSetTick(SceRtcDateTime* output, SceRtcTick* tick);

} // namespace Lumen::Libs::Rtc

#endif // LUMEN_EMULATOR_LIBS_RTC_H_
