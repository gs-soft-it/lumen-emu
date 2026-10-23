#include "Emulator/Common.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/Rtc.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Lumen/Core/DateTime.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("Rtc", 1, "Rtc", 1, 1);

namespace Rtc {

static_assert(sizeof(SceRtcDateTime) == 16);
static_assert(alignof(SceRtcDateTime) == 4);
static_assert(offsetof(SceRtcDateTime, year) == 0);
static_assert(offsetof(SceRtcDateTime, month) == 2);
static_assert(offsetof(SceRtcDateTime, day) == 4);
static_assert(offsetof(SceRtcDateTime, hour) == 6);
static_assert(offsetof(SceRtcDateTime, minute) == 8);
static_assert(offsetof(SceRtcDateTime, second) == 10);
static_assert(offsetof(SceRtcDateTime, microsecond) == 12);
static_assert(sizeof(SceRtcTick) == 8);
static_assert(alignof(SceRtcTick) == 8);
static_assert(offsetof(SceRtcTick, tick) == 0);

bool TryWriteLocalClock(uint64_t output_address, const Core::DateTime& local_time)
{
#if defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	if (local_time.IsInvalid()) return false;
	const auto& date = local_time.GetDate();
	const auto& time = local_time.GetTime();
	const int year = date.Year();
	const int month = date.Month();
	const int day = date.Day();
	const int hour = time.Hour24();
	const int minute = time.Minute();
	const int second = time.Second();
	const int millisecond = time.Msec();
	if (year < 1 || year > UINT16_MAX || !Core::Date::IsValid(year, month, day) ||
	    !Core::Time::IsValid(hour, minute, second, millisecond)) return false;

	const SceRtcDateTime result {
	    static_cast<uint16_t>(year), static_cast<uint16_t>(month), static_cast<uint16_t>(day),
	    static_cast<uint16_t>(hour), static_cast<uint16_t>(minute), static_cast<uint16_t>(second),
	    static_cast<uint32_t>(millisecond) * 1000U};
	return Loader::WriteGuestMemory(output_address, &result, sizeof(result));
#else
	// Loader's other host paths currently copy unchecked. Do not expose that
	// fallback to a new guest output until they validate mapping permissions.
	(void)output_address;
	(void)local_time;
	return false;
#endif
}

int LUMEN_SYSV_ABI sceRtcGetCurrentClockLocalTime(SceRtcDateTime* output)
{
	PRINT_NAME();
	if (!TryWriteLocalClock(reinterpret_cast<uint64_t>(output), Core::DateTime::FromSystem()))
	{
#if defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
		EXIT("RTC local clock requires valid host time and a fully writable guest output");
#else
		EXIT("RTC local clock requires checked guest-memory writes unavailable on this host");
#endif
		std::abort();
	}
	return 0;
}

bool TryGetDayOfWeek(int year, int month, int day, int& weekday)
{
	if (year < 1 || year > UINT16_MAX || !Core::Date::IsValid(year, month, day)) return false;
	// Core uses Monday 1 through Sunday 7; Lumen's local RTC model uses
	// Sunday 0 through Saturday 6, independently of host locale/timezone.
	weekday = Core::Date(year, month, day).DayOfWeek() % 7;
	return true;
}

int LUMEN_SYSV_ABI sceRtcGetDayOfWeek(int year, int month, int day)
{
	int weekday = 0;
	if (!TryGetDayOfWeek(year, month, day, weekday))
	{
		EXIT("RTC day of week requires a valid Gregorian date with year 1..65535");
		std::abort();
	}
	return weekday;
}

bool TryConvertTick(uint64_t tick, SceRtcDateTime& output)
{
	constexpr uint64_t MICROSECONDS_PER_SECOND = 1000000ULL;
	constexpr uint64_t MICROSECONDS_PER_MINUTE = 60ULL * MICROSECONDS_PER_SECOND;
	constexpr uint64_t MICROSECONDS_PER_HOUR   = 60ULL * MICROSECONDS_PER_MINUTE;
	constexpr uint64_t MICROSECONDS_PER_DAY    = 24ULL * MICROSECONDS_PER_HOUR;
	constexpr uint64_t DAYS_YEAR_1_THROUGH_65535 = 23936166ULL;
	constexpr uint64_t MAX_TICK = DAYS_YEAR_1_THROUGH_65535 * MICROSECONDS_PER_DAY - 1ULL;
	if (tick > MAX_TICK) return false;

	const uint64_t days = tick / MICROSECONDS_PER_DAY;
	uint64_t remainder = tick % MICROSECONDS_PER_DAY;
	const uint64_t epoch_julian_day = static_cast<uint64_t>(Core::Date(1, 1, 1).JulianDay());
	const Core::Date date(static_cast<Core::jd_t>(epoch_julian_day + days));
	if (date.IsInvalid()) return false;

	const uint64_t hour = remainder / MICROSECONDS_PER_HOUR;
	remainder %= MICROSECONDS_PER_HOUR;
	const uint64_t minute = remainder / MICROSECONDS_PER_MINUTE;
	remainder %= MICROSECONDS_PER_MINUTE;
	const uint64_t second = remainder / MICROSECONDS_PER_SECOND;
	const uint64_t microsecond = remainder % MICROSECONDS_PER_SECOND;
	const int year = date.Year();
	const int month = date.Month();
	const int day = date.Day();
	if (year < 1 || year > UINT16_MAX || !Core::Date::IsValid(year, month, day)) return false;

	const SceRtcDateTime result {
	    static_cast<uint16_t>(year), static_cast<uint16_t>(month), static_cast<uint16_t>(day),
	    static_cast<uint16_t>(hour), static_cast<uint16_t>(minute), static_cast<uint16_t>(second),
	    static_cast<uint32_t>(microsecond)};
	output = result;
	return true;
}

bool TryConvertDateTime(const SceRtcDateTime& input, uint64_t& output)
{
	constexpr uint64_t MICROSECONDS_PER_SECOND = 1000000ULL;
	constexpr uint64_t MICROSECONDS_PER_MINUTE = 60ULL * MICROSECONDS_PER_SECOND;
	constexpr uint64_t MICROSECONDS_PER_HOUR   = 60ULL * MICROSECONDS_PER_MINUTE;
	constexpr uint64_t MICROSECONDS_PER_DAY    = 24ULL * MICROSECONDS_PER_HOUR;
	if (input.year < 1 || !Core::Date::IsValid(input.year, input.month, input.day) ||
	    !Core::Time::IsValid(input.hour, input.minute, input.second) || input.microsecond >= MICROSECONDS_PER_SECOND)
	{
		return false;
	}

	const auto epoch_julian_day = Core::Date(1, 1, 1).JulianDay();
	const auto date_julian_day  = Core::Date(input.year, input.month, input.day).JulianDay();
	if (date_julian_day < epoch_julian_day) return false;
	const uint64_t days = static_cast<uint64_t>(date_julian_day - epoch_julian_day);
	const uint64_t result = days * MICROSECONDS_PER_DAY +
	                        static_cast<uint64_t>(input.hour) * MICROSECONDS_PER_HOUR +
	                        static_cast<uint64_t>(input.minute) * MICROSECONDS_PER_MINUTE +
	                        static_cast<uint64_t>(input.second) * MICROSECONDS_PER_SECOND + input.microsecond;
	output = result;
	return true;
}

[[maybe_unused]] static constexpr bool GuestRangeIsWellFormed(uint64_t address, size_t size)
{
	return address != 0 && size != 0 && address <= UINT64_MAX - size;
}

[[maybe_unused]] static constexpr bool GuestRangesOverlap(uint64_t first, size_t first_size, uint64_t second, size_t second_size)
{
	return first < second + second_size && second < first + first_size;
}

bool TryGetTick(uint64_t input_address, uint64_t output_address)
{
#if defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	if (!GuestRangeIsWellFormed(input_address, sizeof(SceRtcDateTime)) ||
	    !GuestRangeIsWellFormed(output_address, sizeof(SceRtcTick)) ||
	    GuestRangesOverlap(input_address, sizeof(SceRtcDateTime), output_address, sizeof(SceRtcTick)) ||
	    !Loader::ValidateGuestMemory(output_address, sizeof(SceRtcTick), true)) return false;

	SceRtcDateTime snapshot {};
	if (!Loader::ReadGuestMemory(input_address, &snapshot, sizeof(snapshot))) return false;
	SceRtcTick result {};
	if (!TryConvertDateTime(snapshot, result.tick)) return false;
	return Loader::WriteGuestMemory(output_address, &result, sizeof(result));
#else
	(void)input_address;
	(void)output_address;
	return false;
#endif
}

int LUMEN_SYSV_ABI sceRtcGetTick(const SceRtcDateTime* input, SceRtcTick* output)
{
	PRINT_NAME();
	if (!TryGetTick(reinterpret_cast<uint64_t>(input), reinterpret_cast<uint64_t>(output)))
	{
#if defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
		EXIT("RTC GetTick requires disjoint, valid guest records and a supported local civil time");
#else
		EXIT("RTC GetTick requires checked guest memory unavailable on this host");
#endif
		std::abort();
	}
	return 0;
}

bool TrySetTick(uint64_t output_address, uint64_t tick_address)
{
#if defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	if (!GuestRangeIsWellFormed(output_address, sizeof(SceRtcDateTime)) ||
	    !GuestRangeIsWellFormed(tick_address, sizeof(SceRtcTick)) ||
	    GuestRangesOverlap(output_address, sizeof(SceRtcDateTime), tick_address, sizeof(SceRtcTick)) ||
	    !Loader::ValidateGuestMemory(output_address, sizeof(SceRtcDateTime), true)) return false;

	SceRtcTick snapshot {};
	if (!Loader::ReadGuestMemory(tick_address, &snapshot, sizeof(snapshot))) return false;
	SceRtcDateTime result {};
	if (!TryConvertTick(snapshot.tick, result)) return false;
	return Loader::WriteGuestMemory(output_address, &result, sizeof(result));
#else
	(void)output_address;
	(void)tick_address;
	return false;
#endif
}

int LUMEN_SYSV_ABI sceRtcSetTick(SceRtcDateTime* output, SceRtcTick* tick)
{
	PRINT_NAME();
	if (!TrySetTick(reinterpret_cast<uint64_t>(output), reinterpret_cast<uint64_t>(tick)))
	{
#if defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
		EXIT("RTC SetTick requires disjoint, valid guest records and a supported local tick");
#else
		EXIT("RTC SetTick requires checked guest memory unavailable on this host");
#endif
		std::abort();
	}
	return 0;
}

} // namespace Rtc

namespace RtcStubs {

using Rtc::SceRtcDateTime;
using Rtc::SceRtcTick;

static int LUMEN_SYSV_ABI sceRtcGetCurrentClock(SceRtcDateTime* dt, int tz)
{
	PRINT_NAME();
	if (dt)
	{
		memset(dt, 0, sizeof(*dt));
		dt->year = 2024;
		dt->month = 1;
		dt->day = 1;
		dt->hour = 12;
	}
	return 0;
}

static int LUMEN_SYSV_ABI sceRtcConvertUtcToLocalTime(const SceRtcTick* utc, SceRtcTick* local)
{
	PRINT_NAME();
	if (utc && local) local->tick = utc->tick; // No timezone adjustment
	return 0;
}

static int LUMEN_SYSV_ABI sceRtcGetCurrentTick(SceRtcTick* tick)
{
	PRINT_NAME();
	if (tick)
	{
		// Return microseconds since 01/01/0001
		// PS4 epoch offset from Unix epoch in microseconds
		tick->tick = 62135596800000000ULL + static_cast<uint64_t>(::time(nullptr)) * 1000000ULL;
	}
	return 0;
}

} // namespace RtcStubs

LIB_DEFINE(InitRtc_1)
{
	LIB_FUNC("8lfvnRMqwEM", RtcStubs::sceRtcGetCurrentClock);
	LIB_FUNC("M1TvFst-jrM", RtcStubs::sceRtcConvertUtcToLocalTime);
	LIB_FUNC("18B2NS1y9UU", RtcStubs::sceRtcGetCurrentTick);
	LIB_FUNC("ZPD1YOKI+Kw", Rtc::sceRtcGetCurrentClockLocalTime);
	LIB_FUNC("CyIK-i4XdgQ", Rtc::sceRtcGetDayOfWeek);
	LIB_FUNC("8w-H19ip48I", Rtc::sceRtcGetTick);
	LIB_FUNC("ueega6v3GUw", Rtc::sceRtcSetTick);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
