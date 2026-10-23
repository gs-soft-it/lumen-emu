#include "Lumen/Core/DateTime.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/Rtc.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

namespace {

namespace Rtc = Lumen::Libs::Rtc;
namespace Vm = Lumen::Core::VirtualMemory;
using Lumen::Core::Date;
using Lumen::Core::DateTime;
using Lumen::Core::Time;

using ClockFunction = int (LUMEN_SYSV_ABI *)(Rtc::SceRtcDateTime*);
using WeekdayFunction = int (LUMEN_SYSV_ABI *)(int, int, int);
using GetTickFunction = int (LUMEN_SYSV_ABI *)(const Rtc::SceRtcDateTime*, Rtc::SceRtcTick*);
using SetTickFunction = int (LUMEN_SYSV_ABI *)(Rtc::SceRtcDateTime*, Rtc::SceRtcTick*);
static_assert(sizeof(int) == 4);
static_assert(std::is_same_v<decltype(&Rtc::sceRtcGetCurrentClockLocalTime), ClockFunction>);
static_assert(std::is_same_v<decltype(&Rtc::sceRtcGetDayOfWeek), WeekdayFunction>);
static_assert(std::is_same_v<decltype(&Rtc::sceRtcGetTick), GetTickFunction>);
static_assert(std::is_same_v<decltype(&Rtc::sceRtcSetTick), SetTickFunction>);
static_assert(sizeof(Rtc::SceRtcDateTime) == 16 && alignof(Rtc::SceRtcDateTime) == 4);
static_assert(offsetof(Rtc::SceRtcDateTime, year) == 0);
static_assert(offsetof(Rtc::SceRtcDateTime, month) == 2);
static_assert(offsetof(Rtc::SceRtcDateTime, day) == 4);
static_assert(offsetof(Rtc::SceRtcDateTime, hour) == 6);
static_assert(offsetof(Rtc::SceRtcDateTime, minute) == 8);
static_assert(offsetof(Rtc::SceRtcDateTime, second) == 10);
static_assert(offsetof(Rtc::SceRtcDateTime, microsecond) == 12);
static_assert(sizeof(Rtc::SceRtcTick) == 8 && alignof(Rtc::SceRtcTick) == 8);
static_assert(offsetof(Rtc::SceRtcTick, tick) == 0);

ClockFunction g_clock = nullptr;
WeekdayFunction g_weekday = nullptr;
GetTickFunction g_get_tick = nullptr;
SetTickFunction g_set_tick = nullptr;
constexpr size_t RECORD_BYTES = sizeof(Rtc::SceRtcDateTime);
constexpr size_t TICK_BYTES = sizeof(Rtc::SceRtcTick);
constexpr size_t VM_UNIT = 65536;

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "RtcTest: %s\n", message);
	return condition;
}

Lumen::Loader::SymbolResolve Exact(const char* nid)
{
	Lumen::Loader::SymbolResolve query {};
	query.name = nid;
	query.library = "libSceRtc";
	query.library_version = 1;
	query.module = "libSceRtc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type = Lumen::Loader::SymbolType::Func;
	return query;
}

bool TestNegativeIdentity(const Lumen::Loader::SymbolDatabase& symbols, const char* nid)
{
	const auto exact = Exact(nid);
	for (int version : {0, 2})
	{
		auto wrong = exact;
		wrong.library_version = version;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong library version resolved")) return false;
		wrong = exact;
		wrong.module_version_major = version;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong module major version resolved")) return false;
		wrong = exact;
		wrong.module_version_minor = version;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong module minor version resolved")) return false;
	}
	for (const char* other : {"libSceRtcForToolkit", "libSceKernel", "unrelated_rtc_library"})
	{
		auto wrong = exact;
		wrong.library = other;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong library namespace resolved")) return false;
		wrong = exact;
		wrong.module = other;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong module namespace resolved")) return false;
	}
	for (auto type : {Lumen::Loader::SymbolType::Unknown, Lumen::Loader::SymbolType::Object,
	                 Lumen::Loader::SymbolType::TlsModule, Lumen::Loader::SymbolType::NoType})
	{
		auto wrong = exact;
		wrong.type = type;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong symbol type resolved")) return false;
	}
	return true;
}

bool TestBindings()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libRtc_1", &symbols), "RTC registration failed")) return false;
	const auto* clock = symbols.Find(Exact("ZPD1YOKI+Kw"));
	if (!Check(clock != nullptr && clock->vaddr == reinterpret_cast<uint64_t>(&Rtc::sceRtcGetCurrentClockLocalTime) &&
	           clock->dbg_name == U"Rtc::sceRtcGetCurrentClockLocalTime", "local clock identity or signature target mismatch"))
		return false;
	g_clock = reinterpret_cast<ClockFunction>(clock->vaddr);
	const auto* weekday = symbols.Find(Exact("CyIK-i4XdgQ"));
	if (!Check(weekday != nullptr && weekday->vaddr == reinterpret_cast<uint64_t>(&Rtc::sceRtcGetDayOfWeek) &&
	           weekday->dbg_name == U"Rtc::sceRtcGetDayOfWeek", "weekday identity or signature target mismatch")) return false;
	g_weekday = reinterpret_cast<WeekdayFunction>(weekday->vaddr);
	const auto* get_tick = symbols.Find(Exact("8w-H19ip48I"));
	if (!Check(get_tick != nullptr && get_tick->vaddr == reinterpret_cast<uint64_t>(&Rtc::sceRtcGetTick) &&
	           get_tick->dbg_name == U"Rtc::sceRtcGetTick", "GetTick identity or signature target mismatch")) return false;
	g_get_tick = reinterpret_cast<GetTickFunction>(get_tick->vaddr);
	const auto* set_tick = symbols.Find(Exact("ueega6v3GUw"));
	if (!Check(set_tick != nullptr && set_tick->vaddr == reinterpret_cast<uint64_t>(&Rtc::sceRtcSetTick) &&
	           set_tick->dbg_name == U"Rtc::sceRtcSetTick", "SetTick identity or signature target mismatch")) return false;
	g_set_tick = reinterpret_cast<SetTickFunction>(set_tick->vaddr);
	for (const char* nid : {"8lfvnRMqwEM", "M1TvFst-jrM", "18B2NS1y9UU"})
		if (!Check(symbols.Find(Exact(nid)) != nullptr, "existing RTC binding disappeared")) return false;
	if (!Check(symbols.HostCallTargetCount() == 7, "unexpected RTC host-call target count") ||
	    !TestNegativeIdentity(symbols, "ZPD1YOKI+Kw") || !TestNegativeIdentity(symbols, "CyIK-i4XdgQ") ||
	    !TestNegativeIdentity(symbols, "8w-H19ip48I") ||
	    !TestNegativeIdentity(symbols, "ueega6v3GUw")) return false;
	return Check(symbols.Find(Exact("unknown-rtc-binding-nid")) == nullptr &&
	             symbols.Find(Exact("sceRtcGetCurrentClockLocalTime")) == nullptr &&
	             symbols.Find(Exact("sceRtcGetDayOfWeek")) == nullptr &&
	             symbols.Find(Exact("sceRtcGetTick")) == nullptr &&
	             symbols.Find(Exact("sceRtcSetTick")) == nullptr &&
	             symbols.FindByStdName(U"ZPD1YOKI+Kw") == nullptr &&
	             symbols.FindByStdName(U"CyIK-i4XdgQ") == nullptr &&
	             symbols.FindByStdName(U"8w-H19ip48I") == nullptr &&
	             symbols.FindByStdName(U"ueega6v3GUw") == nullptr &&
	             symbols.FindByStdName(U"sceRtcGetCurrentClockLocalTime") == nullptr &&
	             symbols.FindByStdName(U"sceRtcGetDayOfWeek") == nullptr &&
	             symbols.FindByStdName(U"sceRtcGetTick") == nullptr &&
	             symbols.FindByStdName(U"sceRtcSetTick") == nullptr && symbols.StdNameMapSize() == 0,
	             "RTC gained an unqualified or standard-name fallback");
}

bool SameRecord(const Rtc::SceRtcDateTime& left, const Rtc::SceRtcDateTime& right)
{
	return left.year == right.year && left.month == right.month && left.day == right.day &&
	       left.hour == right.hour && left.minute == right.minute && left.second == right.second &&
	       left.microsecond == right.microsecond;
}

bool TestTickConversion()
{
	struct Sample
	{
		uint64_t tick;
		Rtc::SceRtcDateTime expected;
	};
	constexpr Sample valid[] = {
	    {0ULL, {1, 1, 1, 0, 0, 0, 0}},
	    {1ULL, {1, 1, 1, 0, 0, 0, 1}},
	    {999999ULL, {1, 1, 1, 0, 0, 0, 999999}},
	    {1000000ULL, {1, 1, 1, 0, 0, 1, 0}},
	    {86399999999ULL, {1, 1, 1, 23, 59, 59, 999999}},
	    {86400000000ULL, {1, 1, 2, 0, 0, 0, 0}},
	    {49916304000000000ULL, {1582, 10, 15, 0, 0, 0, 0}},
	    {59931705599999999ULL, {1900, 2, 28, 23, 59, 59, 999999}},
	    {59931705600000000ULL, {1900, 3, 1, 0, 0, 0, 0}},
	    {62135596800000000ULL, {1970, 1, 1, 0, 0, 0, 0}},
	    {63082281599999999ULL, {1999, 12, 31, 23, 59, 59, 999999}},
	    {63087424496789123ULL, {2000, 2, 29, 12, 34, 56, 789123}},
	    {63844847999999999ULL, {2024, 2, 29, 23, 59, 59, 999999}},
	    {63924208496789123ULL, {2026, 9, 5, 12, 34, 56, 789123}},
	    {66243139200000000ULL, {2100, 3, 1, 0, 0, 0, 0}},
	    {75710160000000000ULL, {2400, 2, 29, 0, 0, 0, 0}},
	    {2068053206400000000ULL, {65535, 1, 1, 0, 0, 0, 0}},
	    {2068084742399999999ULL, {65535, 12, 31, 23, 59, 59, 999999}},
	};
	for (const auto& sample : valid)
	{
		Rtc::SceRtcDateTime actual {0xa7a7, 0xa7a7, 0xa7a7, 0xa7a7, 0xa7a7, 0xa7a7, 0xa7a7a7a7};
		if (!Check(Rtc::TryConvertTick(sample.tick, actual) && SameRecord(actual, sample.expected),
		           "valid tick did not preserve the local Gregorian microsecond contract")) return false;
		uint64_t encoded = std::numeric_limits<uint64_t>::max();
		if (!Check(Rtc::TryConvertDateTime(sample.expected, encoded) && encoded == sample.tick,
		           "valid civil record did not round-trip to the original tick")) return false;
	}
	for (uint64_t tick : {2068084742400000000ULL, std::numeric_limits<uint64_t>::max()})
	{
		const Rtc::SceRtcDateTime sentinel {0xa7a7, 0xa7a7, 0xa7a7, 0xa7a7, 0xa7a7, 0xa7a7, 0xa7a7a7a7};
		auto actual = sentinel;
		if (!Check(!Rtc::TryConvertTick(tick, actual) && SameRecord(actual, sentinel),
		           "out-of-range tick was accepted or changed the native result")) return false;
	}
	constexpr Rtc::SceRtcDateTime invalid[] = {
	    {0, 1, 1, 0, 0, 0, 0},
	    {2026, 0, 1, 0, 0, 0, 0},
	    {2026, 13, 1, 0, 0, 0, 0},
	    {2026, 1, 0, 0, 0, 0, 0},
	    {2026, 1, 32, 0, 0, 0, 0},
	    {1900, 2, 29, 0, 0, 0, 0},
	    {2100, 2, 29, 0, 0, 0, 0},
	    {2026, 2, 29, 0, 0, 0, 0},
	    {2000, 2, 30, 0, 0, 0, 0},
	    {2026, 4, 31, 0, 0, 0, 0},
	    {2026, 1, 1, 24, 0, 0, 0},
	    {2026, 1, 1, UINT16_MAX, 0, 0, 0},
	    {2026, 1, 1, 0, 60, 0, 0},
	    {2026, 1, 1, 0, UINT16_MAX, 0, 0},
	    {2026, 1, 1, 0, 0, 60, 0},
	    {2026, 1, 1, 0, 0, UINT16_MAX, 0},
	    {2026, 1, 1, 0, 0, 0, 1000000},
	    {2026, 1, 1, 0, 0, 0, UINT32_MAX},
	};
	for (const auto& input : invalid)
	{
		constexpr uint64_t sentinel = UINT64_C(0xa7a7a7a7a7a7a7a7);
		uint64_t actual = sentinel;
		if (!Check(!Rtc::TryConvertDateTime(input, actual) && actual == sentinel,
		           "invalid civil record was accepted or changed the native tick")) return false;
	}
	return true;
}

bool TestWeekdays()
{
	struct Sample { int year; int month; int day; int weekday; };
	// Literal Gregorian expectations, independently checked with the local
	// Python calendar. Years above 9999 use its identical 400-year cycle.
	constexpr Sample valid[] = {
	    {1, 1, 1, 1}, {1582, 10, 4, 1}, {1582, 10, 15, 5},
	    {1900, 2, 28, 3}, {1900, 3, 1, 4}, {1970, 1, 1, 4},
	    {1999, 12, 31, 5}, {2000, 1, 1, 6}, {2000, 2, 28, 1}, {2000, 2, 29, 2}, {2000, 3, 1, 3},
	    {2024, 2, 29, 4}, {2026, 8, 30, 0}, {2026, 8, 31, 1}, {2026, 9, 1, 2}, {2026, 9, 2, 3},
	    {2026, 9, 3, 4}, {2026, 9, 4, 5}, {2026, 9, 5, 6}, {2026, 12, 31, 4}, {2027, 1, 1, 5},
	    {2100, 2, 28, 0}, {2100, 3, 1, 1}, {2400, 2, 29, 2}, {65535, 1, 1, 2}, {65535, 12, 31, 2}};
	for (const auto& sample : valid)
	{
		int weekday = -917;
		if (!Check(Rtc::TryGetDayOfWeek(sample.year, sample.month, sample.day, weekday) && weekday == sample.weekday &&
		           g_weekday(sample.year, sample.month, sample.day) == sample.weekday,
		           "weekday differs from the approved local Gregorian numbering")) return false;
	}
	constexpr int low = std::numeric_limits<int>::min();
	constexpr int high = std::numeric_limits<int>::max();
	constexpr std::array<int, 3> invalid[] = {
	    {low, 1, 1}, {-1, 1, 1}, {0, 1, 1}, {65536, 1, 1}, {high, 1, 1},
	    {2026, low, 1}, {2026, 0, 1}, {2026, 13, 1}, {2026, high, 1},
	    {2026, 1, low}, {2026, 1, 0}, {2026, 1, 32}, {2026, 1, high},
	    {1900, 2, 29}, {2100, 2, 29}, {2026, 2, 29}, {2000, 2, 30}, {2026, 4, 31}};
	for (const auto& date : invalid)
	{
		int weekday = -917;
		if (!Check(!Rtc::TryGetDayOfWeek(date[0], date[1], date[2], weekday) && weekday == -917,
		           "invalid date was accepted or changed the native result")) return false;
	}
	return true;
}

bool HasCanaries(const std::array<uint8_t, 64>& bytes, size_t offset)
{
	for (size_t i = 0; i < bytes.size(); ++i)
		if ((i < offset || i >= offset + RECORD_BYTES) && bytes[i] != 0xa7) return false;
	return true;
}

template <size_t Size>
bool UnchangedOutside(const std::array<uint8_t, Size>& bytes, const std::array<uint8_t, Size>& before,
                      size_t offset, size_t length)
{
	for (size_t i = 0; i < Size; ++i)
		if ((i < offset || i >= offset + length) && bytes[i] != before[i]) return false;
	return true;
}

#if defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
bool TestGetTickSuccess()
{
	constexpr uint64_t SAMPLE_TICK = 63924208496789123ULL;
	constexpr Rtc::SceRtcDateTime input {2026, 9, 5, 12, 34, 56, 789123};
	{
		std::array<uint8_t, 96> bytes;
		bytes.fill(0xa7);
		constexpr size_t input_offset = 17;
		constexpr size_t output_offset = 57;
		std::memcpy(bytes.data() + input_offset, &input, sizeof(input));
		const auto before = bytes;
		if (!Check(Rtc::TryGetTick(reinterpret_cast<uint64_t>(bytes.data() + input_offset),
		                           reinterpret_cast<uint64_t>(bytes.data() + output_offset)),
		           "valid unaligned GetTick records were rejected")) return false;
		Rtc::SceRtcTick actual {};
		std::memcpy(&actual, bytes.data() + output_offset, sizeof(actual));
		if (!Check(actual.tick == SAMPLE_TICK && UnchangedOutside(bytes, before, output_offset, TICK_BYTES),
		           "GetTick conversion changed input or surrounding bytes")) return false;
	}

	for (const auto offsets : {std::array<size_t, 2> {16, 32}, std::array<size_t, 2> {24, 16}})
	{
		std::array<uint8_t, 64> bytes;
		bytes.fill(0xa7);
		std::memcpy(bytes.data() + offsets[0], &input, sizeof(input));
		const auto before = bytes;
		if (!Check(Rtc::TryGetTick(reinterpret_cast<uint64_t>(bytes.data() + offsets[0]),
		                           reinterpret_cast<uint64_t>(bytes.data() + offsets[1])),
		           "adjacent GetTick records were rejected")) return false;
		Rtc::SceRtcTick actual {};
		std::memcpy(&actual, bytes.data() + offsets[1], sizeof(actual));
		if (!Check(actual.tick == SAMPLE_TICK && UnchangedOutside(bytes, before, offsets[1], TICK_BYTES),
		           "adjacent GetTick changed input or canaries")) return false;
	}

	alignas(8) std::array<uint8_t, 80> bytes;
	bytes.fill(0xa7);
	constexpr size_t input_offset = 16;
	constexpr size_t output_offset = 48;
	std::memcpy(bytes.data() + input_offset, &input, sizeof(input));
	const auto before = bytes;
	const int result = g_get_tick(reinterpret_cast<const Rtc::SceRtcDateTime*>(bytes.data() + input_offset),
	                              reinterpret_cast<Rtc::SceRtcTick*>(bytes.data() + output_offset));
	Rtc::SceRtcTick actual {};
	std::memcpy(&actual, bytes.data() + output_offset, sizeof(actual));
	return Check(result == 0 && actual.tick == SAMPLE_TICK &&
	             UnchangedOutside(bytes, before, output_offset, TICK_BYTES),
	             "resolved GetTick export returned the wrong tick or changed input/canaries");
}

bool TestGetTickRangesAndAliasing()
{
	constexpr Rtc::SceRtcDateTime valid {2026, 9, 5, 12, 34, 56, 789123};
	std::array<uint8_t, 96> bytes;
	bytes.fill(0xa7);
	constexpr size_t input_offset = 16;
	constexpr size_t output_offset = 64;
	std::memcpy(bytes.data() + input_offset, &valid, sizeof(valid));
	const auto baseline = bytes;
	const uint64_t input_address = reinterpret_cast<uint64_t>(bytes.data() + input_offset);
	const uint64_t output_address = reinterpret_cast<uint64_t>(bytes.data() + output_offset);
	const uint64_t maximum = std::numeric_limits<uint64_t>::max();
	if (!Check(!Rtc::TryGetTick(0, output_address) && !Rtc::TryGetTick(1, output_address) &&
	           !Rtc::TryGetTick(maximum - RECORD_BYTES + 2, output_address) &&
	           !Rtc::TryGetTick(input_address, 0) && !Rtc::TryGetTick(input_address, 1) &&
	           !Rtc::TryGetTick(input_address, maximum - TICK_BYTES + 2) && bytes == baseline,
	           "null, unmapped, or overflowing GetTick range was accepted")) return false;

	constexpr Rtc::SceRtcDateTime invalid {2026, 2, 29, 12, 34, 56, 789123};
	std::memcpy(bytes.data() + input_offset, &invalid, sizeof(invalid));
	const auto before_invalid = bytes;
	if (!Check(!Rtc::TryGetTick(input_address, output_address) && bytes == before_invalid,
	           "invalid guest civil record was accepted or changed memory")) return false;

	constexpr std::array<size_t, 2> overlaps[] = {
	    {16, 16}, {16, 20}, {16, 28}, {20, 16}, {20, 28}};
	for (const auto offsets : overlaps)
	{
		bytes.fill(0xa7);
		std::memcpy(bytes.data() + offsets[0], &valid, sizeof(valid));
		const auto before = bytes;
		if (!Check(!Rtc::TryGetTick(reinterpret_cast<uint64_t>(bytes.data() + offsets[0]),
		                            reinterpret_cast<uint64_t>(bytes.data() + offsets[1])) && bytes == before,
		           "overlapping GetTick records were accepted or changed memory")) return false;
	}

	const uint64_t freed_input = Vm::Alloc(0, VM_UNIT, Vm::Mode::ReadWrite);
	if (!Check(freed_input != 0 && Vm::Free(freed_input), "freed GetTick input fixture setup failed")) return false;
	bytes = baseline;
	if (!Check(!Rtc::TryGetTick(freed_input, output_address) && bytes == baseline,
	           "freed GetTick input was accepted")) return false;
	const uint64_t freed_output = Vm::Alloc(0, VM_UNIT, Vm::Mode::ReadWrite);
	if (!Check(freed_output != 0 && Vm::Free(freed_output), "freed GetTick output fixture setup failed")) return false;
	return Check(!Rtc::TryGetTick(input_address, freed_output) && bytes == baseline,
	             "freed GetTick output was accepted or changed input");
}

bool TestSetTickSuccess()
{
	constexpr uint64_t SAMPLE_TICK = 63924208496789123ULL;
	constexpr Rtc::SceRtcDateTime expected {2026, 9, 5, 12, 34, 56, 789123};
	{
		std::array<uint8_t, 96> bytes;
		bytes.fill(0xa7);
		constexpr size_t output_offset = 17;
		constexpr size_t tick_offset = 57;
		const Rtc::SceRtcTick tick {SAMPLE_TICK};
		std::memcpy(bytes.data() + tick_offset, &tick, sizeof(tick));
		const auto before = bytes;
		if (!Check(Rtc::TrySetTick(reinterpret_cast<uint64_t>(bytes.data() + output_offset),
		                           reinterpret_cast<uint64_t>(bytes.data() + tick_offset)),
		           "valid unaligned SetTick records were rejected")) return false;
		Rtc::SceRtcDateTime actual {};
		std::memcpy(&actual, bytes.data() + output_offset, sizeof(actual));
		if (!Check(SameRecord(actual, expected) && UnchangedOutside(bytes, before, output_offset, RECORD_BYTES),
		           "SetTick conversion changed input or surrounding bytes")) return false;
	}

	for (const auto offsets : {std::array<size_t, 2> {16, 32}, std::array<size_t, 2> {24, 16}})
	{
		std::array<uint8_t, 64> bytes;
		bytes.fill(0xa7);
		const Rtc::SceRtcTick tick {SAMPLE_TICK};
		std::memcpy(bytes.data() + offsets[1], &tick, sizeof(tick));
		const auto before = bytes;
		if (!Check(Rtc::TrySetTick(reinterpret_cast<uint64_t>(bytes.data() + offsets[0]),
		                           reinterpret_cast<uint64_t>(bytes.data() + offsets[1])),
		           "adjacent SetTick records were rejected")) return false;
		Rtc::SceRtcDateTime actual {};
		std::memcpy(&actual, bytes.data() + offsets[0], sizeof(actual));
		if (!Check(SameRecord(actual, expected) && UnchangedOutside(bytes, before, offsets[0], RECORD_BYTES),
		           "adjacent SetTick changed input or canaries")) return false;
	}

	alignas(8) std::array<uint8_t, 80> bytes;
	bytes.fill(0xa7);
	constexpr size_t output_offset = 16;
	constexpr size_t tick_offset = 48;
	const Rtc::SceRtcTick tick {SAMPLE_TICK};
	std::memcpy(bytes.data() + tick_offset, &tick, sizeof(tick));
	const auto before = bytes;
	const int result = g_set_tick(reinterpret_cast<Rtc::SceRtcDateTime*>(bytes.data() + output_offset),
	                              reinterpret_cast<Rtc::SceRtcTick*>(bytes.data() + tick_offset));
	Rtc::SceRtcDateTime actual {};
	std::memcpy(&actual, bytes.data() + output_offset, sizeof(actual));
	return Check(result == 0 && SameRecord(actual, expected) &&
	             UnchangedOutside(bytes, before, output_offset, RECORD_BYTES),
	             "resolved SetTick export returned the wrong record or changed input/canaries");
}

bool TestSetTickRangesAndAliasing()
{
	constexpr uint64_t SAMPLE_TICK = 63924208496789123ULL;
	std::array<uint8_t, 96> bytes;
	bytes.fill(0xa7);
	constexpr size_t output_offset = 16;
	constexpr size_t tick_offset = 64;
	const Rtc::SceRtcTick valid_tick {SAMPLE_TICK};
	std::memcpy(bytes.data() + tick_offset, &valid_tick, sizeof(valid_tick));
	const auto baseline = bytes;
	const uint64_t output_address = reinterpret_cast<uint64_t>(bytes.data() + output_offset);
	const uint64_t tick_address = reinterpret_cast<uint64_t>(bytes.data() + tick_offset);
	const uint64_t maximum = std::numeric_limits<uint64_t>::max();
	if (!Check(!Rtc::TrySetTick(0, tick_address) && !Rtc::TrySetTick(1, tick_address) &&
	           !Rtc::TrySetTick(maximum - RECORD_BYTES + 2, tick_address) &&
	           !Rtc::TrySetTick(output_address, 0) && !Rtc::TrySetTick(output_address, 1) &&
	           !Rtc::TrySetTick(output_address, maximum - TICK_BYTES + 2) && bytes == baseline,
	           "null, unmapped, or overflowing SetTick range was accepted")) return false;

	const Rtc::SceRtcTick invalid_tick {2068084742400000000ULL};
	std::memcpy(bytes.data() + tick_offset, &invalid_tick, sizeof(invalid_tick));
	const auto before_invalid_tick = bytes;
	if (!Check(!Rtc::TrySetTick(output_address, tick_address) && bytes == before_invalid_tick,
	           "out-of-range guest tick was accepted or changed memory")) return false;

	constexpr std::array<size_t, 2> overlaps[] = {
	    {16, 16}, {16, 20}, {16, 28}, {20, 16}, {20, 28}};
	for (const auto offsets : overlaps)
	{
		bytes.fill(0xa7);
		std::memcpy(bytes.data() + offsets[1], &valid_tick, sizeof(valid_tick));
		const auto before = bytes;
		if (!Check(!Rtc::TrySetTick(reinterpret_cast<uint64_t>(bytes.data() + offsets[0]),
		                            reinterpret_cast<uint64_t>(bytes.data() + offsets[1])) && bytes == before,
		           "overlapping SetTick records were accepted or changed memory")) return false;
	}

	const uint64_t freed_output = Vm::Alloc(0, VM_UNIT, Vm::Mode::ReadWrite);
	if (!Check(freed_output != 0 && Vm::Free(freed_output), "freed output fixture setup failed")) return false;
	bytes = baseline;
	if (!Check(!Rtc::TrySetTick(freed_output, tick_address) && bytes == baseline,
	           "freed SetTick output was accepted")) return false;
	const uint64_t freed_tick = Vm::Alloc(0, VM_UNIT, Vm::Mode::ReadWrite);
	if (!Check(freed_tick != 0 && Vm::Free(freed_tick), "freed tick fixture setup failed")) return false;
	return Check(!Rtc::TrySetTick(output_address, freed_tick) && bytes == baseline,
	             "freed SetTick input was accepted or changed output");
}

bool TestCalendar()
{
	struct Sample { int year; int month; int day; int hour; int minute; int second; int millisecond; };
	constexpr Sample samples[] = {
	    {1, 1, 1, 0, 0, 0, 0}, {1900, 2, 28, 23, 59, 59, 999}, {1900, 3, 1, 0, 0, 0, 0},
	    {2000, 2, 29, 12, 34, 56, 789}, {2024, 2, 29, 23, 59, 59, 999}, {2024, 3, 1, 0, 0, 0, 0},
	    {2026, 12, 31, 23, 59, 59, 999}, {2027, 1, 1, 0, 0, 0, 0}, {2100, 2, 28, 12, 0, 0, 1},
	    {65535, 12, 31, 23, 59, 59, 999}};
	for (const auto& sample : samples)
	{
		std::array<uint8_t, 64> bytes;
		bytes.fill(0xa7);
		constexpr size_t offset = 17; // The byte-oriented checked write also accepts unaligned output.
		const DateTime input(Date(sample.year, sample.month, sample.day),
		                     Time(sample.hour, sample.minute, sample.second, sample.millisecond));
		if (!Check(Rtc::TryWriteLocalClock(reinterpret_cast<uint64_t>(bytes.data() + offset), input),
		           "valid civil date was rejected")) return false;
		Rtc::SceRtcDateTime actual {};
		std::memcpy(&actual, bytes.data() + offset, sizeof(actual));
		if (!Check(actual.year == sample.year && actual.month == sample.month && actual.day == sample.day &&
		           actual.hour == sample.hour && actual.minute == sample.minute && actual.second == sample.second &&
		           actual.microsecond == static_cast<uint32_t>(sample.millisecond) * 1000U && HasCanaries(bytes, offset),
		           "civil fields, millisecond conversion, or output canaries differ")) return false;
	}
	const DateTime invalid[] = {
	    DateTime(), DateTime(Date(2026, 9, 5)), DateTime(Time(12, 0, 0)),
	    DateTime(Date(0, 1, 1), Time(0, 0, 0)), DateTime(Date(-1, 1, 1), Time(0, 0, 0)),
	    DateTime(Date(65536, 1, 1), Time(0, 0, 0)), DateTime(Date(1900, 2, 29), Time(0, 0, 0)),
	    DateTime(Date(2100, 2, 29), Time(0, 0, 0)), DateTime(Date(2026, 4, 31), Time(0, 0, 0)),
	    DateTime(Date(2026, 0, 1), Time(0, 0, 0)), DateTime(Date(2026, 13, 1), Time(0, 0, 0)),
	    DateTime(Date(2026, 1, 0), Time(0, 0, 0)), DateTime(Date(2026, 1, 1), Time(24, 0, 0)),
	    DateTime(Date(2026, 1, 1), Time(0, 60, 0)), DateTime(Date(2026, 1, 1), Time(0, 0, 60)),
	    DateTime(Date(2026, 1, 1), Time(0, 0, 0, 1000))};
	for (const auto& input : invalid)
	{
		std::array<uint8_t, 64> bytes;
		bytes.fill(0xa7);
		const auto before = bytes;
		if (!Check(!Rtc::TryWriteLocalClock(reinterpret_cast<uint64_t>(bytes.data() + 16), input) && bytes == before,
		           "invalid clock changed guest bytes")) return false;
	}
	return true;
}

bool TestLiveClock()
{
	alignas(Rtc::SceRtcDateTime) std::array<uint8_t, 64> bytes;
	bytes.fill(0xa7);
	const auto before = DateTime::FromSystem();
	const int result = g_clock(reinterpret_cast<Rtc::SceRtcDateTime*>(bytes.data() + 16));
	const auto after = DateTime::FromSystem();
	Rtc::SceRtcDateTime record {};
	std::memcpy(&record, bytes.data() + 16, sizeof(record));
	if (!Check(result == 0 && record.year >= 1 && Date::IsValid(record.year, record.month, record.day) &&
	           record.microsecond < 1000000 && record.microsecond % 1000 == 0 &&
	           Time::IsValid(record.hour, record.minute, record.second, static_cast<int>(record.microsecond / 1000)) &&
	           HasCanaries(bytes, 16), "export returned invalid civil time or changed canaries")) return false;
	const DateTime actual(Date(record.year, record.month, record.day),
	                      Time(record.hour, record.minute, record.second, static_cast<int>(record.microsecond / 1000)));
	// Compare complete date/time snapshots so an ordinary midnight crossing is valid.
	return Check(!before.IsInvalid() && !after.IsInvalid() && before <= actual && actual <= after,
	             "export does not match the host local clock sampling interval");
}

class Mapping
{
public:
	Mapping(): address(Vm::Alloc(0, 2 * VM_UNIT, Vm::Mode::ReadWrite)) {}
	~Mapping() { if (address != 0) Vm::Free(address); }
	Mapping(const Mapping&) = delete;
	Mapping& operator=(const Mapping&) = delete;
	uint64_t address;
};

bool TestGetTickMemory()
{
	constexpr uint64_t SAMPLE_TICK = 63924208496789123ULL;
	constexpr Rtc::SceRtcDateTime input {2026, 9, 5, 12, 34, 56, 789123};
	Mapping memory;
	if (!Check(memory.address != 0, "GetTick memory fixture allocation failed")) return false;
	auto* bytes = reinterpret_cast<uint8_t*>(memory.address);
	const auto reset = [&](size_t input_offset)
	{
		std::memset(bytes, 0xa7, 2 * VM_UNIT);
		std::memcpy(bytes + input_offset, &input, sizeof(input));
		return std::vector<uint8_t>(bytes, bytes + 2 * VM_UNIT);
	};
	const auto unchanged = [&](const std::vector<uint8_t>& before)
	{
		return std::memcmp(bytes, before.data(), before.size()) == 0;
	};
	const auto success = [&](const std::vector<uint8_t>& before, size_t output_offset)
	{
		Rtc::SceRtcTick actual {};
		std::memcpy(&actual, bytes + output_offset, sizeof(actual));
		if (actual.tick != SAMPLE_TICK) return false;
		for (size_t i = 0; i < before.size(); ++i)
			if ((i < output_offset || i >= output_offset + TICK_BYTES) && bytes[i] != before[i]) return false;
		return true;
	};

	constexpr size_t output_offset = 64;
	constexpr size_t input_offset = VM_UNIT + 64;
	for (auto protection : {Vm::Mode::Read, Vm::Mode::NoAccess})
	{
		const auto before = reset(input_offset);
		if (!Check(Vm::Protect(memory.address, VM_UNIT, protection), "GetTick output protection failed")) return false;
		const bool rejected = !Rtc::TryGetTick(memory.address + input_offset, memory.address + output_offset);
		if (!Check(Vm::Protect(memory.address, VM_UNIT, Vm::Mode::ReadWrite), "GetTick output restoration failed")) return false;
		if (!Check(rejected && unchanged(before), "protected GetTick output was accepted or changed memory")) return false;
	}

	{
		const auto before = reset(input_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::Read), "read-only civil input setup failed")) return false;
		const bool accepted = Rtc::TryGetTick(memory.address + input_offset, memory.address + output_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::ReadWrite), "read-only civil input restoration failed")) return false;
		if (!Check(accepted && success(before, output_offset), "read-only civil input was rejected or modified")) return false;
	}
	{
		const auto before = reset(input_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::NoAccess), "inaccessible civil input setup failed")) return false;
		const bool rejected = !Rtc::TryGetTick(memory.address + input_offset, memory.address + output_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::ReadWrite), "inaccessible civil input restoration failed")) return false;
		if (!Check(rejected && unchanged(before), "inaccessible civil input was accepted or changed output")) return false;
	}

	{
		constexpr size_t local_input_offset = 32;
		const auto before = reset(local_input_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::Read), "GetTick output boundary guard failed")) return false;
		const bool rejected = !Rtc::TryGetTick(memory.address + local_input_offset, memory.address + VM_UNIT - 4);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::ReadWrite), "GetTick output boundary restoration failed")) return false;
		if (!Check(rejected && unchanged(before), "cross-boundary GetTick output was accepted or partially written")) return false;
	}
	{
		constexpr size_t crossing_input_offset = VM_UNIT - 8;
		const auto before = reset(crossing_input_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::NoAccess), "GetTick input boundary guard failed")) return false;
		const bool rejected = !Rtc::TryGetTick(memory.address + crossing_input_offset, memory.address + output_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::ReadWrite), "GetTick input boundary restoration failed")) return false;
		if (!Check(rejected && unchanged(before), "cross-boundary GetTick input was accepted or changed output")) return false;
	}

	{
		constexpr size_t ending_input_offset = VM_UNIT - RECORD_BYTES;
		const auto before = reset(ending_input_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::NoAccess), "exact-boundary GetTick input guard failed")) return false;
		const bool accepted = Rtc::TryGetTick(memory.address + ending_input_offset, memory.address + output_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::ReadWrite), "exact-boundary GetTick input restoration failed")) return false;
		if (!Check(accepted && success(before, output_offset), "complete GetTick input ending at boundary was rejected or modified")) return false;
	}
	{
		constexpr size_t local_input_offset = 32;
		constexpr size_t ending_output_offset = VM_UNIT - TICK_BYTES;
		const auto before = reset(local_input_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::NoAccess), "exact-boundary GetTick output guard failed")) return false;
		const bool accepted = Rtc::TryGetTick(memory.address + local_input_offset, memory.address + ending_output_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::ReadWrite), "exact-boundary GetTick output restoration failed")) return false;
		return Check(accepted && success(before, ending_output_offset),
		             "complete GetTick output ending at boundary was rejected or changed input");
	}
}

bool TestSetTickMemory()
{
	constexpr uint64_t SAMPLE_TICK = 63924208496789123ULL;
	constexpr Rtc::SceRtcDateTime expected {2026, 9, 5, 12, 34, 56, 789123};
	const Rtc::SceRtcTick tick {SAMPLE_TICK};
	Mapping memory;
	if (!Check(memory.address != 0, "SetTick memory fixture allocation failed")) return false;
	auto* bytes = reinterpret_cast<uint8_t*>(memory.address);
	const auto reset = [&](size_t tick_offset)
	{
		std::memset(bytes, 0xa7, 2 * VM_UNIT);
		std::memcpy(bytes + tick_offset, &tick, sizeof(tick));
		return std::vector<uint8_t>(bytes, bytes + 2 * VM_UNIT);
	};
	const auto unchanged = [&](const std::vector<uint8_t>& before)
	{
		return std::memcmp(bytes, before.data(), before.size()) == 0;
	};
	const auto success = [&](const std::vector<uint8_t>& before, size_t output_offset)
	{
		Rtc::SceRtcDateTime actual {};
		std::memcpy(&actual, bytes + output_offset, sizeof(actual));
		if (!SameRecord(actual, expected)) return false;
		for (size_t i = 0; i < before.size(); ++i)
			if ((i < output_offset || i >= output_offset + RECORD_BYTES) && bytes[i] != before[i]) return false;
		return true;
	};

	constexpr size_t output_offset = 64;
	constexpr size_t tick_offset = VM_UNIT + 64;
	for (auto protection : {Vm::Mode::Read, Vm::Mode::NoAccess})
	{
		const auto before = reset(tick_offset);
		if (!Check(Vm::Protect(memory.address, VM_UNIT, protection), "SetTick output protection failed")) return false;
		const bool rejected = !Rtc::TrySetTick(memory.address + output_offset, memory.address + tick_offset);
		if (!Check(Vm::Protect(memory.address, VM_UNIT, Vm::Mode::ReadWrite), "SetTick output restoration failed")) return false;
		if (!Check(rejected && unchanged(before), "protected SetTick output was accepted or changed memory")) return false;
	}

	{
		const auto before = reset(tick_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::Read), "read-only tick setup failed")) return false;
		const bool accepted = Rtc::TrySetTick(memory.address + output_offset, memory.address + tick_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::ReadWrite), "read-only tick restoration failed")) return false;
		if (!Check(accepted && success(before, output_offset), "read-only Tick was rejected or modified")) return false;
	}
	{
		const auto before = reset(tick_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::NoAccess), "inaccessible tick setup failed")) return false;
		const bool rejected = !Rtc::TrySetTick(memory.address + output_offset, memory.address + tick_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::ReadWrite), "inaccessible tick restoration failed")) return false;
		if (!Check(rejected && unchanged(before), "inaccessible Tick was accepted or changed output")) return false;
	}

	{
		constexpr size_t local_tick_offset = 32;
		const auto before = reset(local_tick_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::Read), "output boundary guard failed")) return false;
		const bool rejected = !Rtc::TrySetTick(memory.address + VM_UNIT - 8, memory.address + local_tick_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::ReadWrite), "output boundary restoration failed")) return false;
		if (!Check(rejected && unchanged(before), "cross-boundary SetTick output was accepted or partially written")) return false;
	}
	{
		constexpr size_t crossing_tick_offset = VM_UNIT - 4;
		const auto before = reset(crossing_tick_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::NoAccess), "tick boundary guard failed")) return false;
		const bool rejected = !Rtc::TrySetTick(memory.address + output_offset, memory.address + crossing_tick_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::ReadWrite), "tick boundary restoration failed")) return false;
		if (!Check(rejected && unchanged(before), "cross-boundary Tick was accepted or changed output")) return false;
	}

	{
		constexpr size_t ending_tick_offset = VM_UNIT - TICK_BYTES;
		const auto before = reset(ending_tick_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::NoAccess), "exact-boundary tick guard failed")) return false;
		const bool accepted = Rtc::TrySetTick(memory.address + output_offset, memory.address + ending_tick_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::ReadWrite), "exact-boundary tick restoration failed")) return false;
		if (!Check(accepted && success(before, output_offset), "complete Tick ending at boundary was rejected or modified")) return false;
	}
	{
		constexpr size_t local_tick_offset = 32;
		constexpr size_t ending_output_offset = VM_UNIT - RECORD_BYTES;
		const auto before = reset(local_tick_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::NoAccess), "exact-boundary output guard failed")) return false;
		const bool accepted = Rtc::TrySetTick(memory.address + ending_output_offset, memory.address + local_tick_offset);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::ReadWrite), "exact-boundary output restoration failed")) return false;
		return Check(accepted && success(before, ending_output_offset),
		             "complete output ending at boundary was rejected or changed input");
	}
}

bool TestMemory()
{
	const DateTime input(Date(2026, 9, 5), Time(12, 34, 56, 789));
	for (uint64_t address : {UINT64_C(0), UINT64_C(1), std::numeric_limits<uint64_t>::max() - 7})
		if (!Check(!Rtc::TryWriteLocalClock(address, input), "null, unmapped, or overflowing output accepted")) return false;
	Mapping memory;
	if (!Check(memory.address != 0, "memory fixture allocation failed")) return false;
	auto* bytes = reinterpret_cast<uint8_t*>(memory.address);
	for (auto protection : {Vm::Mode::Read, Vm::Mode::NoAccess})
	{
		std::memset(bytes, 0xa7, 2 * VM_UNIT);
		const std::vector<uint8_t> expected(bytes, bytes + 2 * VM_UNIT);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, protection), "output protection failed")) return false;
		const bool cross_rejected = !Rtc::TryWriteLocalClock(memory.address + VM_UNIT - 8, input);
		const bool inside_rejected = !Rtc::TryWriteLocalClock(memory.address + VM_UNIT + 16, input);
		if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::ReadWrite), "output restoration failed")) return false;
		if (!Check(cross_rejected && inside_rejected && std::memcmp(bytes, expected.data(), expected.size()) == 0,
		           "protected output was accepted or partially written")) return false;
	}
	if (!Check(Vm::Protect(memory.address + VM_UNIT, VM_UNIT, Vm::Mode::Read), "boundary guard failed")) return false;
	if (!Check(Rtc::TryWriteLocalClock(memory.address + VM_UNIT - RECORD_BYTES, input),
	           "complete writable record ending at a protected boundary was rejected")) return false;
	const auto freed = Vm::Alloc(0, VM_UNIT, Vm::Mode::ReadWrite);
	if (!Check(freed != 0 && Vm::Free(freed), "unmapped fixture setup failed")) return false;
	return Check(!Rtc::TryWriteLocalClock(freed, input), "freed output mapping was accepted");
}
#else
bool TestUnsupportedHost()
{
	std::array<uint8_t, 64> bytes;
	bytes.fill(0xa7);
	const auto before = bytes;
	const DateTime input(Date(2026, 9, 5), Time(12, 34, 56, 789));
	const bool local_clock_rejected =
	    !Rtc::TryWriteLocalClock(reinterpret_cast<uint64_t>(bytes.data() + 16), input) && bytes == before &&
	    !Rtc::TryWriteLocalClock(0, input) && !Rtc::TryWriteLocalClock(1, input);
	const Rtc::SceRtcDateTime civil {2026, 9, 5, 12, 34, 56, 789123};
	std::memcpy(bytes.data() + 16, &civil, sizeof(civil));
	const auto before_get_tick = bytes;
	const bool get_tick_rejected =
	    !Rtc::TryGetTick(reinterpret_cast<uint64_t>(bytes.data() + 16),
	                     reinterpret_cast<uint64_t>(bytes.data() + 48)) && bytes == before_get_tick &&
	    !Rtc::TryGetTick(0, 0);
	const Rtc::SceRtcTick tick {63924208496789123ULL};
	std::memcpy(bytes.data() + 48, &tick, sizeof(tick));
	const auto before_set_tick = bytes;
	return Check(local_clock_rejected && get_tick_rejected &&
	             !Rtc::TrySetTick(reinterpret_cast<uint64_t>(bytes.data() + 16),
	                              reinterpret_cast<uint64_t>(bytes.data() + 48)) && bytes == before_set_tick &&
	             !Rtc::TrySetTick(0, 0),
	             "host without checked guest writes did not fail closed");
}
#endif

} // namespace

int main()
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Vm::Init();
	if (!TestBindings() || !TestWeekdays() || !TestTickConversion()) return 1;
#if defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	if (!TestGetTickSuccess() || !TestGetTickRangesAndAliasing() || !TestGetTickMemory() ||
	    !TestSetTickSuccess() || !TestSetTickRangesAndAliasing() || !TestSetTickMemory() ||
	    !TestCalendar() || !TestLiveClock() || !TestMemory()) return 1;
#else
	if (!TestUnsupportedHost()) return 1;
	std::puts("RtcTest: checked output unavailable on this host; success path not verified");
#endif
	std::puts("RtcTest: PASS");
	return 0;
}
