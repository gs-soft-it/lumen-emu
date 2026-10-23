#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Common.h"
#include "Emulator/Config.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"
#include "Emulator/Loader/Elf.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include "CpuX64.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <thread>

#if defined(__APPLE__) && defined(__aarch64__)
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <unistd.h>

namespace Lumen::Loader {
uint64_t translate_guest_address(uint64_t address, size_t size, bool write);
size_t ExportGuestMemoryRegionsForTest(Cpu::X64MemoryTranslationRegion* regions, size_t capacity);
Cpu::X64RunResult RunGuestMemoryPipelineForTest(Cpu::X64State* state, size_t max_steps);
bool InitializeGuestX64RootStackForTest(uint8_t* stack, size_t stack_size,
	                                    uint64_t* initial_rsp);
size_t GuestX64StackSizeForTest();
size_t GuestX64RootFrameSizeForTest();
bool ValidateAndroidNativeHostTargetCacheSeparationForTest();
bool RunHostCallForTest(const SymbolDatabase* symbols, uint64_t target,
	                    Cpu::X64State* state);
void ConfigureGuestMemoryPipelineWatchForTest(uint64_t begin, uint64_t end);
void ResetGuestMemoryPipelineCountersForTest();
size_t GuestMemoryPipelineExportCountForTest();
size_t GuestMemoryPipelineWatchedTranslateCountForTest();
bool GuestMemoryPipelineNonReentrantCapabilityObservedForTest();
} // namespace Lumen::Loader

namespace {

constexpr uint64_t                 HOST_RETURN_BASE = 0xc0dec0de00000000ULL;
constexpr int32_t                  HOST_INT32_RETURN = -123456789;
constexpr std::array<uint64_t, 12> HOST_ARGUMENTS   = {
    0x0101010101010101ULL, 0x1212121212121212ULL, 0x2323232323232323ULL, 0x3434343434343434ULL,
    0x4545454545454545ULL, 0x5656565656565656ULL, 0x6767676767676767ULL, 0x7878787878787878ULL,
    0x8989898989898989ULL, 0x9a9a9a9a9a9a9a9aULL, 0xababababababababULL, 0xbcbcbcbcbcbcbcbcULL,
};
constexpr size_t EXPECTED_GUEST_ROOT_FRAME_SIZE =
	(1 + Lumen::Loader::HOST_CALL_SCALAR_MAX_ARITY -
	 Lumen::Loader::HOST_CALL_GP_REGISTER_ARGUMENT_COUNT) *
	sizeof(uint64_t);
static_assert(EXPECTED_GUEST_ROOT_FRAME_SIZE == 88);
static_assert(HOST_ARGUMENTS.size() ==
	          Lumen::Loader::HOST_CALL_GP_MAX_ARITY);

std::array<uint64_t, 12> g_host_arguments {};
uint32_t                 g_host_argument_count = 0;
uint32_t                 g_host_call_count     = 0;
bool                     g_override_guest_rsp  = false;
uint64_t                 g_guest_rsp_override  = 0;
Lumen::Loader::SymbolDatabase* g_host_symbols = nullptr;
uint8_t                       g_ajm_return_marker {};
uint32_t                      g_mixed_return_marker {};
#if defined(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY)
uint64_t                      g_blocking_host_target = 0;
uint64_t                      g_guest_control_target = 0;
std::atomic<bool>             g_pause_before_host_boundary {false};
std::atomic<bool>             g_allow_host_boundary {false};
std::atomic<bool>             g_host_boundary_reached {false};
std::atomic<bool>             g_guest_boundary_reached {false};
std::atomic<bool>             g_release_guest_boundary {false};
#endif
#if defined(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY) || \
    defined(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
std::atomic<bool>             g_blocking_host_enabled {false};
std::atomic<bool>             g_blocking_host_entered {false};
std::atomic<bool>             g_release_blocking_host {false};
#endif

enum class HostTestEnum: uint16_t
{
};

uint64_t CaptureHostArguments(std::initializer_list<uint64_t> arguments)
{
	g_host_arguments.fill(0);
	g_host_argument_count = static_cast<uint32_t>(arguments.size());
	uint32_t index        = 0;
	for (const uint64_t argument: arguments)
		g_host_arguments[index++] = argument;
	g_host_call_count++;
	return HOST_RETURN_BASE + g_host_argument_count;
}

static LUMEN_SYSV_ABI uint64_t HostCall0()
{
	return CaptureHostArguments({});
}

#if defined(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY) || \
    defined(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
static LUMEN_SYSV_ABI uint64_t BlockingHostCall0()
{
	g_blocking_host_entered.store(true, std::memory_order_release);
	while (g_blocking_host_enabled.load(std::memory_order_acquire) &&
	       !g_release_blocking_host.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}
	return CaptureHostArguments({});
}
#endif

static LUMEN_SYSV_ABI uint64_t HostCall1(uint64_t a0)
{
	return CaptureHostArguments({a0});
}

static LUMEN_SYSV_ABI uint64_t HostCall2(uint64_t a0, uint64_t a1)
{
	return CaptureHostArguments({a0, a1});
}

static LUMEN_SYSV_ABI uint64_t HostCall3(uint64_t a0, uint64_t a1, uint64_t a2)
{
	return CaptureHostArguments({a0, a1, a2});
}

static LUMEN_SYSV_ABI uint64_t HostCall4(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
	return CaptureHostArguments({a0, a1, a2, a3});
}

static LUMEN_SYSV_ABI uint64_t HostCall5(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	return CaptureHostArguments({a0, a1, a2, a3, a4});
}

static LUMEN_SYSV_ABI uint64_t HostCall6(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	return CaptureHostArguments({a0, a1, a2, a3, a4, a5});
}

static LUMEN_SYSV_ABI uint64_t HostCall7(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
	return CaptureHostArguments({a0, a1, a2, a3, a4, a5, a6});
}

static LUMEN_SYSV_ABI uint64_t HostCall8(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                                        uint64_t a7)
{
	return CaptureHostArguments({a0, a1, a2, a3, a4, a5, a6, a7});
}

[[gnu::noinline]] static LUMEN_SYSV_ABI int32_t HostCall9(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                                         uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8)
{
	CaptureHostArguments({a0, a1, a2, a3, a4, a5, a6, a7, a8});
	return HOST_INT32_RETURN;
}

[[gnu::noinline]] static LUMEN_SYSV_ABI void* HostCallAjm10(void* a0, uint32_t a1, uint64_t a2, const void* a3, size_t a4, void* a5,
                                                          size_t a6, void* a7, size_t a8, const void* a9)
{
	CaptureHostArguments({reinterpret_cast<uint64_t>(a0), a1, a2, reinterpret_cast<uint64_t>(a3), a4,
	                      reinterpret_cast<uint64_t>(a5), a6, reinterpret_cast<uint64_t>(a7), a8,
	                      reinterpret_cast<uint64_t>(a9)});
	return &g_ajm_return_marker;
}

[[gnu::noinline]] static LUMEN_SYSV_ABI void HostCall11(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                                       uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9,
                                                       HostTestEnum a10)
{
	CaptureHostArguments({a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, static_cast<uint16_t>(a10)});
}

[[gnu::noinline]] static LUMEN_SYSV_ABI uint32_t* HostCallMixed12(void* a0, uint8_t a1, uint16_t a2, uint8_t a3, uint8_t a4,
                                                                 const volatile void* a5, uint8_t a6, uint64_t a7, uint16_t a8,
                                                                 uint16_t a9, uint8_t a10, uint32_t a11)
{
	CaptureHostArguments({reinterpret_cast<uint64_t>(a0), a1, a2, a3, a4,
	                      reinterpret_cast<uint64_t>(a5), a6, a7, a8, a9, a10, a11});
	return &g_mixed_return_marker;
}

constexpr size_t                                HOST_FUNCTION_COUNT = 13;
const std::array<uint64_t, HOST_FUNCTION_COUNT> HOST_FUNCTIONS      = {
    reinterpret_cast<uint64_t>(&HostCall0),  reinterpret_cast<uint64_t>(&HostCall1),  reinterpret_cast<uint64_t>(&HostCall2),
    reinterpret_cast<uint64_t>(&HostCall3),  reinterpret_cast<uint64_t>(&HostCall4),  reinterpret_cast<uint64_t>(&HostCall5),
    reinterpret_cast<uint64_t>(&HostCall6),  reinterpret_cast<uint64_t>(&HostCall7),  reinterpret_cast<uint64_t>(&HostCall8),
    reinterpret_cast<uint64_t>(&HostCall9), reinterpret_cast<uint64_t>(&HostCallAjm10), reinterpret_cast<uint64_t>(&HostCall11),
    reinterpret_cast<uint64_t>(&HostCallMixed12),
};

constexpr auto HOST_CALL_9_METADATA  = Lumen::Loader::MakeHostCallGpMetadata<&HostCall9>();
constexpr auto HOST_CALL_10_METADATA = Lumen::Loader::MakeHostCallGpMetadata<&HostCallAjm10>();
constexpr auto HOST_CALL_11_METADATA = Lumen::Loader::MakeHostCallGpMetadata<&HostCall11>();
constexpr auto HOST_CALL_12_METADATA = Lumen::Loader::MakeHostCallGpMetadata<&HostCallMixed12>();
static_assert(HOST_CALL_9_METADATA.arity == 9);
static_assert(HOST_CALL_10_METADATA.arity == 10);
static_assert(HOST_CALL_11_METADATA.arity == 11);
static_assert(HOST_CALL_12_METADATA.arity == 12);

void RegisterTypedHostCalls(Lumen::Loader::SymbolDatabase* symbols)
{
	if (symbols == nullptr) return;
	symbols->SetHostCallGpMetadata(HOST_FUNCTIONS[9], HOST_CALL_9_METADATA);
	symbols->SetHostCallGpMetadata(HOST_FUNCTIONS[10], HOST_CALL_10_METADATA);
	symbols->SetHostCallGpMetadata(HOST_FUNCTIONS[11], HOST_CALL_11_METADATA);
	symbols->SetHostCallGpMetadata(HOST_FUNCTIONS[12], HOST_CALL_12_METADATA);
}

bool TestHostCallBridge(uint64_t target, Lumen::Cpu::X64State* state)
{
	if (!g_override_guest_rsp)
		return Lumen::Loader::InvokeHostCall(g_host_symbols, target, state);
	const uint64_t saved_rsp = state->gpr[4];
	state->gpr[4]            = g_guest_rsp_override;
	const bool handled =
	    Lumen::Loader::InvokeHostCall(g_host_symbols, target, state);
	if (state->memory_fault_size == 0) state->gpr[4] = saved_rsp;
	return handled;
}

bool ClassifiedHostCallBridge(uint64_t target, Lumen::Cpu::X64State* state)
{
	return Lumen::Loader::RunHostCallForTest(g_host_symbols, target, state);
}

#if defined(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY)
bool ProductionHostCallBridge(uint64_t target, Lumen::Cpu::X64State* state)
{
	if (target == g_blocking_host_target &&
	    g_pause_before_host_boundary.load(std::memory_order_acquire))
	{
		g_host_boundary_reached.store(true, std::memory_order_release);
		while (!g_allow_host_boundary.load(std::memory_order_acquire))
			std::this_thread::yield();
	}
	const bool handled =
	    Lumen::Loader::RunHostCallForTest(g_host_symbols, target, state);
	if (g_guest_control_target != 0 && target == g_guest_control_target &&
	    !handled)
	{
		g_guest_boundary_reached.store(true, std::memory_order_release);
		while (!g_release_guest_boundary.load(std::memory_order_acquire))
			std::this_thread::yield();
	}
	return handled;
}

#endif

#if defined(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY) || \
    defined(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
void InitializeHostCallState(Lumen::Cpu::X64State* state, uint64_t code,
	                         uint64_t stack, uint64_t target);
#endif

class Mapping
{
public:
	explicit Mapping(size_t size, Lumen::Core::VirtualMemory::Mode mode = Lumen::Core::VirtualMemory::Mode::ReadWrite)
	    : m_size(size), m_address(Lumen::Core::VirtualMemory::Alloc(0, size, mode))
	{
	}

	~Mapping()
	{
		if (m_address != 0) Lumen::Core::VirtualMemory::Free(m_address);
	}

	Mapping(const Mapping&)            = delete;
	Mapping& operator=(const Mapping&) = delete;

	[[nodiscard]] bool     IsValid() const { return m_address != 0; }
	[[nodiscard]] uint8_t* Data() const { return reinterpret_cast<uint8_t*>(m_address); }

	bool Protect(size_t offset, size_t size, Lumen::Core::VirtualMemory::Mode mode) const
	{
		return m_address != 0 && offset <= m_size && size <= m_size - offset &&
		       Lumen::Core::VirtualMemory::Protect(m_address + offset, size, mode);
	}

	bool Free()
	{
		if (m_address == 0) return false;
		const bool ok = Lumen::Core::VirtualMemory::Free(m_address);
		if (ok) m_address = 0;
		return ok;
	}

private:
	size_t   m_size    = 0;
	uint64_t m_address = 0;
};

template <typename Func>
bool RunWithFreshCache(Func&& func)
{
	bool        result = false;
	std::thread worker([&] { result = func(); });
	worker.join();
	return result;
}

bool TestBasicCacheAndPermissions(size_t page_size)
{
	Mapping writable(page_size);
	Mapping read_only(page_size, Lumen::Core::VirtualMemory::Mode::Read);
	if (!writable.IsValid() || !read_only.IsValid()) return false;

	const uint64_t a = reinterpret_cast<uint64_t>(writable.Data());
	const uint64_t b = reinterpret_cast<uint64_t>(read_only.Data());
	return Lumen::Loader::translate_guest_address(a, 0, false) == 0 &&
	       Lumen::Loader::translate_guest_address(UINT64_MAX - 3, 8, false) == 0 &&
	       Lumen::Loader::translate_guest_address(a, 64, false) == a &&
	       Lumen::Loader::translate_guest_address(a + 128, 64, false) == a + 128 &&
	       Lumen::Loader::translate_guest_address(a + 256, 64, true) == a + 256 &&
	       Lumen::Loader::translate_guest_address(b, 64, false) == b && Lumen::Loader::translate_guest_address(b, 64, true) == 0 &&
	       Lumen::Loader::translate_guest_address(a + 512, 64, true) == a + 512 &&
	       Lumen::Loader::translate_guest_address(a + 640, 64, false) == a + 640;
}

bool TestUnresolvedRelocationPolicy()
{
	using Action = Lumen::Loader::UnresolvedRelocationActionForTest;
	using Type   = Lumen::Loader::SymbolType;
	const auto action = Lumen::Loader::RuntimeLinkerUnresolvedRelocationActionForTest;

	Lumen::Loader::Program default_program {};
	if (default_program.defer_unresolved_plt_functions) return false;

	const bool default_strict =
	    action(Type::Func, true, false,
	           default_program.defer_unresolved_plt_functions) == Action::Fail;
	const bool deferred_function_plt =
	    action(Type::Func, true, false, true) ==
	    Action::PreserveExistingTarget;
	const bool deferred_function_non_plt_strict =
	    action(Type::Func, false, false, true) == Action::Fail;
	const bool deferred_object_plt_strict =
	    action(Type::Object, true, false, true) == Action::Fail;
	const bool deferred_object_non_plt_strict =
	    action(Type::Object, false, false, true) == Action::Fail;
	const bool existing_weak_policy =
	    action(Type::Func, true, true, false) ==
	        Action::PreserveExistingTarget &&
	    action(Type::Func, false, true, false) ==
	        Action::PreserveExistingTarget &&
	    action(Type::Object, false, true, false) == Action::InvalidObject;

	return default_strict && deferred_function_plt &&
	       deferred_function_non_plt_strict && deferred_object_plt_strict &&
	       deferred_object_non_plt_strict && existing_weak_policy;
}

bool TestFexUnresolvedArgumentRegions(size_t page_size)
{
	using namespace Lumen::Core::VirtualMemory;
	using Lumen::Loader::FexUnresolvedArgumentRegionsEnabledForTest;
	using Lumen::Loader::FormatFexUnresolvedArgumentRegionForTest;

	const bool selector = FexUnresolvedArgumentRegionsEnabledForTest("1") &&
	    !FexUnresolvedArgumentRegionsEnabledForTest(nullptr) &&
	    !FexUnresolvedArgumentRegionsEnabledForTest("") &&
	    !FexUnresolvedArgumentRegionsEnabledForTest("0") &&
	    !FexUnresolvedArgumentRegionsEnabledForTest("01") &&
	    !FexUnresolvedArgumentRegionsEnabledForTest("1x") &&
	    !FexUnresolvedArgumentRegionsEnabledForTest("1\n") &&
	    !FexUnresolvedArgumentRegionsEnabledForTest(" 1");
	if (!selector) return false;
	std::array<char, 2> short_line {'x', 'y'};
	if (FormatFexUnresolvedArgumentRegionForTest(nullptr, 256, 17, 0, 0) ||
	    FormatFexUnresolvedArgumentRegionForTest(short_line.data(), 0, 17, 0, 0) ||
	    short_line[0] != 'x' ||
	    FormatFexUnresolvedArgumentRegionForTest(short_line.data(), 1, 17, 0, 0) ||
	    short_line[1] != 'y') return false;

	auto check = [&](size_t index, uint64_t address, ManagedRegionState expected_state,
	                 uint64_t expected_begin, uint64_t expected_end, Mode expected_mode)
	{
		std::array<char, 256> line {};
		if (!FormatFexUnresolvedArgumentRegionForTest(
		        line.data(), line.size(), 17, index, address)) return false;

		int thread_id = 0;
		size_t parsed_index = 0;
		uint64_t parsed_address = 0;
		unsigned state = 0, mode = 0, readable = 0, writable = 0, executable = 0;
		uint64_t begin = 0, end = 0;
		int consumed = 0;
		const int fields = std::sscanf(
		    line.data(),
		    "FEX unresolved argument region: thread=%d index=%zu address=0x%" SCNx64
		    " state=%u begin=0x%" SCNx64 " end=0x%" SCNx64
		    " mode=%u readable=%u writable=%u executable=%u%n",
		    &thread_id, &parsed_index, &parsed_address, &state, &begin, &end,
		    &mode, &readable, &writable, &executable, &consumed);
		const auto expected_mode_bits = static_cast<uint32_t>(expected_mode);
		return fields == 10 && consumed > 0 && line[consumed] == '\n' &&
		       line[consumed + 1] == '\0' && thread_id == 17 &&
		       parsed_index == index && parsed_address == address &&
		       state == static_cast<unsigned>(expected_state) &&
		       begin == expected_begin && end == expected_end &&
		       mode == expected_mode_bits &&
		       readable == ((expected_mode_bits & 1u) != 0 ? 1u : 0u) &&
		       writable == ((expected_mode_bits & 2u) != 0 ? 1u : 0u) &&
		       executable == ((expected_mode_bits & 4u) != 0 ? 1u : 0u);
	};

	if (!check(0, 0, ManagedRegionState::Unmanaged, 0, 0, Mode::NoAccess) ||
	    !check(1, 7, ManagedRegionState::Unmanaged, 0, 0, Mode::NoAccess))
		return false;

	Mapping rx(page_size, Mode::ExecuteRead);
	Mapping rw(page_size, Mode::ReadWrite);
	if (!rx.IsValid() || !rw.IsValid()) return false;
	const uint64_t rx_address = reinterpret_cast<uint64_t>(rx.Data());
	const uint64_t rw_address = reinterpret_cast<uint64_t>(rw.Data());
	if (!check(3, rx_address, ManagedRegionState::Visible, rx_address,
	           rx_address + page_size, Mode::ExecuteRead) ||
	    !check(4, rw_address, ManagedRegionState::Visible, rw_address,
	           rw_address + page_size, Mode::ReadWrite)) return false;

	ReservationHandle reservation {};
	MutationTransaction transaction;
	if (!BeginReserveAligned(0, page_size, page_size, &reservation, &transaction,
	                         ReservationPlacement::PreferAddress)) return false;
	transaction.Commit();
	const bool reserved = reservation.IsValid() &&
	    check(2, reservation.base, ManagedRegionState::Reserved,
	          reservation.base, reservation.base + page_size, Mode::NoAccess);
	return ReleaseReservation(reservation) && reserved;
}

bool TestModuleLayoutContracts()
{
	uint64_t aligned_size = 0;
	uint64_t end = 0;
	const bool valid_segment =
	    Lumen::Loader::RuntimeLinkerValidateSegmentLayoutForTest(
	        0x2000, 0x1000, 0x1800, 0x2001, 0x1000,
	        &aligned_size, &end) &&
	    aligned_size == 0x3000 && end == 0x5000;
	const bool invalid_segment =
	    !Lumen::Loader::RuntimeLinkerValidateSegmentLayoutForTest(
	        0, 0, 0x1001, 0x1000, 0x1000, &aligned_size, &end) &&
	    !Lumen::Loader::RuntimeLinkerValidateSegmentLayoutForTest(
	        0, 0, 0, 0x1000, 3, &aligned_size, &end) &&
	    !Lumen::Loader::RuntimeLinkerValidateSegmentLayoutForTest(
	        0, 0, 0, UINT64_MAX, 0x1000, &aligned_size, &end) &&
	    !Lumen::Loader::RuntimeLinkerValidateSegmentLayoutForTest(
	        UINT64_MAX - 0xfff, 0, 0, 0x1000, 0x1000,
	        &aligned_size, &end) &&
	    !Lumen::Loader::RuntimeLinkerValidateSegmentLayoutForTest(
	        0, UINT64_MAX - 7, 8, 8, 1, &aligned_size, &end);

	std::array<Lumen::Loader::Elf64_Phdr, 2> segments {};
	segments[0].p_type = Lumen::Loader::PT_LOAD;
	segments[0].p_vaddr = 0x1000;
	segments[0].p_filesz = 0x800;
	segments[0].p_memsz = 0x1000;
	segments[0].p_align = 0x1000;
	segments[1].p_type = Lumen::Loader::PT_OS_RELRO;
	segments[1].p_vaddr = 0x7000;
	segments[1].p_filesz = 0x1000;
	segments[1].p_memsz = 0x1800;
	segments[1].p_align = 0x1000;
	uint64_t span = 0;
	const bool valid_span =
	    Lumen::Loader::RuntimeLinkerCalculateProgramSpanForTest(
	        segments.data(), segments.size(), &span) &&
	    span == 0x9000;
	segments[1].p_align = 3;
	const bool invalid_span =
	    !Lumen::Loader::RuntimeLinkerCalculateProgramSpanForTest(
	        segments.data(), segments.size(), &span);

	constexpr uint64_t increment = UINT64_C(0x010000000);
	constexpr uint64_t requested = UINT64_C(0x900000000);
	constexpr uint64_t actual = UINT64_C(0x123450000);
	uint64_t next = 0;
	const bool actual_base_governs =
	    Lumen::Loader::RuntimeLinkerAdvanceModuleLayoutCursorForTest(
	        actual, 0x4000, &next) &&
	    next == actual + increment && next != requested + increment;
	const bool exact_slot_advances_beyond_span =
	    Lumen::Loader::RuntimeLinkerAdvanceModuleLayoutCursorForTest(
	        actual, increment, &next) &&
	    next == actual + 2 * increment;
	const bool cursor_overflow_rejected =
	    !Lumen::Loader::RuntimeLinkerAdvanceModuleLayoutCursorForTest(
	        UINT64_MAX - increment + 1, 1, &next) &&
	    !Lumen::Loader::RuntimeLinkerAdvanceModuleLayoutCursorForTest(
	        actual, 0, &next);

	return valid_segment && invalid_segment && valid_span && invalid_span &&
	       actual_base_governs && exact_slot_advances_beyond_span &&
	       cursor_overflow_rejected;
}

bool TestProtectInvalidatesPositiveWriteCache(size_t page_size)
{
	Mapping mapping(page_size);
	if (!mapping.IsValid()) return false;

	const uint64_t address = reinterpret_cast<uint64_t>(mapping.Data());
	if (Lumen::Loader::translate_guest_address(address, 64, true) != address ||
	    Lumen::Loader::translate_guest_address(address + 128, 64, true) != address + 128)
	{
		return false;
	}

	const uint64_t generation = Lumen::Core::VirtualMemory::GetMutationGeneration();
	if (!mapping.Protect(0, page_size, Lumen::Core::VirtualMemory::Mode::Read) ||
	    Lumen::Core::VirtualMemory::GetMutationGeneration() == generation)
	{
		return false;
	}

	return Lumen::Loader::translate_guest_address(address, 64, false) == address &&
	       Lumen::Loader::translate_guest_address(address, 64, true) == 0;
}

size_t PermissionGranule(size_t host_page_size)
{
	// Direct FEX still rejects mixed 4 KiB host-page envelopes. Exercise the
	// translator/cache contracts with representable regions there, retaining
	// exact 4 KiB fixtures on the software-checked Apple backends.
	return Lumen::Core::VirtualMemory::RequiresUniformHostPageDataProtection()
	           ? host_page_size : size_t {4096};
}

bool TestMixedGuestPagePolicy(size_t host_page_size)
{
	if (host_page_size <= 4096) return true;
	Mapping mapping(host_page_size);
	if (!mapping.IsValid()) return false;
	const auto address = reinterpret_cast<uint64_t>(mapping.Data());
	const auto generation = Lumen::Core::VirtualMemory::GetMutationGeneration();
	auto old = Lumen::Core::VirtualMemory::Mode::Execute;
	const bool changed = Lumen::Core::VirtualMemory::Protect(
	    address + 4096, 1, Lumen::Core::VirtualMemory::Mode::Read, &old);
	if (Lumen::Core::VirtualMemory::RequiresUniformHostPageDataProtection())
	{
		return !changed && old == Lumen::Core::VirtualMemory::Mode::Execute &&
		       Lumen::Core::VirtualMemory::GetMutationGeneration() == generation &&
		       Lumen::Loader::translate_guest_address(address + 4096, 8, true) == address + 4096 &&
		       Lumen::Loader::translate_guest_address(address, 8, true) == address;
	}
	return changed && old == Lumen::Core::VirtualMemory::Mode::ReadWrite &&
	       Lumen::Loader::translate_guest_address(address + 4096, 8, true) == 0 &&
	       Lumen::Loader::translate_guest_address(address + 4096, 8, false) == address + 4096 &&
	       Lumen::Loader::translate_guest_address(address, 8, true) == address;
}

bool TestGuestPageProtectionIsolation(size_t host_page_size)
{
	const size_t guest_page_size = PermissionGranule(host_page_size);

	Mapping mapping(std::max(host_page_size, 4 * guest_page_size));
	if (!mapping.IsValid()) return false;

	const uint64_t address = reinterpret_cast<uint64_t>(mapping.Data());
	if (Lumen::Loader::translate_guest_address(address, 64, true) != address ||
	    !mapping.Protect(0, guest_page_size, Lumen::Core::VirtualMemory::Mode::Read) ||
	    !mapping.Protect(guest_page_size, guest_page_size, Lumen::Core::VirtualMemory::Mode::ReadWrite))
	{
		return false;
	}

	const bool two_page_isolation =
	    Lumen::Loader::translate_guest_address(address, 64, false) == address &&
	    Lumen::Loader::translate_guest_address(address, 64, true) == 0 &&
	    Lumen::Loader::translate_guest_address(address + guest_page_size, 64, true) ==
	        address + guest_page_size;
	if (!mapping.Protect(2 * guest_page_size, guest_page_size, Lumen::Core::VirtualMemory::Mode::NoAccess) ||
	    !mapping.Protect(3 * guest_page_size, guest_page_size, Lumen::Core::VirtualMemory::Mode::Execute))
	{
		return false;
	}

	const uint64_t cross_page = address + guest_page_size - 4;
	const bool precise_permissions =
	    Lumen::Loader::translate_guest_address(cross_page, 8, false) == cross_page &&
	    Lumen::Loader::translate_guest_address(cross_page, 8, true) == 0 &&
	    Lumen::Loader::translate_guest_address(address + 2 * guest_page_size, 64, false) == 0 &&
	    Lumen::Loader::translate_guest_address(address + 2 * guest_page_size, 64, true) == 0 &&
	    Lumen::Loader::translate_guest_address(address + 3 * guest_page_size, 64, false) ==
	        address + 3 * guest_page_size &&
	    Lumen::Loader::translate_guest_address(address + 3 * guest_page_size, 64, true) == 0;

	std::array<Lumen::Cpu::X64MemoryTranslationRegion, 16> regions {};
	const size_t region_count = Lumen::Loader::ExportGuestMemoryRegionsForTest(regions.data(), regions.size());
	const auto has_region = [&](size_t page, uint32_t permissions)
	{
		const uint64_t begin = address + page * guest_page_size;
		for (size_t index = 0; index < region_count; index++)
		{
			if (regions[index].guest_begin == begin && regions[index].guest_end == begin + guest_page_size &&
			    regions[index].host_begin == begin && regions[index].permissions == permissions)
			{
				return true;
			}
		}
		return false;
	};
	const uint32_t read = static_cast<uint32_t>(Lumen::Cpu::X64MemoryPermission::Read);
	const uint32_t write = static_cast<uint32_t>(Lumen::Cpu::X64MemoryPermission::Write);
	const bool logical_export = has_region(0, read) && has_region(1, read | write) &&
	                            !has_region(2, 0) && has_region(3, read);
	return two_page_isolation && precise_permissions && logical_export;
}

bool TestGuestRegionExportRecency(size_t host_page_size)
{
	const size_t guest_page_size = PermissionGranule(host_page_size);

	Mapping mapping(std::max(host_page_size, 3 * guest_page_size));
	if (!mapping.IsValid() ||
	    !mapping.Protect(0, guest_page_size, Lumen::Core::VirtualMemory::Mode::Read) ||
	    !mapping.Protect(guest_page_size, guest_page_size, Lumen::Core::VirtualMemory::Mode::ReadWrite) ||
	    !mapping.Protect(2 * guest_page_size, guest_page_size, Lumen::Core::VirtualMemory::Mode::Execute))
	{
		return false;
	}

	const uint64_t a = reinterpret_cast<uint64_t>(mapping.Data());
	const uint64_t b = a + guest_page_size;
	const uint64_t c = b + guest_page_size;
	if (Lumen::Loader::translate_guest_address(a, 64, false) != a ||
	    Lumen::Loader::translate_guest_address(b, 64, true) != b ||
	    Lumen::Loader::translate_guest_address(c, 64, false) != c ||
	    Lumen::Loader::translate_guest_address(a, 64, false) != a)
	{
		return false;
	}

	std::array<Lumen::Cpu::X64MemoryTranslationRegion, 2> regions {};
	const size_t count = Lumen::Loader::ExportGuestMemoryRegionsForTest(regions.data(), regions.size());
	return count == regions.size() && regions[0].guest_begin == a && regions[0].guest_end == b &&
	       regions[1].guest_begin == c && regions[1].guest_end == c + guest_page_size;
}

bool TestGuestRegionExportRecencyWrap(size_t host_page_size)
{
	const size_t guest_page_size = PermissionGranule(host_page_size);
	constexpr size_t guest_pages = 17;
	const size_t mapping_size =
	    ((guest_pages * guest_page_size + host_page_size - 1) / host_page_size) * host_page_size;
	Mapping mapping(mapping_size);
	if (!mapping.IsValid()) return false;

	for (size_t page = 0; page < guest_pages; page++)
	{
		const auto mode = (page & 1) == 0 ? Lumen::Core::VirtualMemory::Mode::Read
		                                  : Lumen::Core::VirtualMemory::Mode::ReadWrite;
		if (!mapping.Protect(page * guest_page_size, guest_page_size, mode)) return false;
	}

	const uint64_t begin = reinterpret_cast<uint64_t>(mapping.Data());
	for (size_t page = 0; page < 16; page++)
	{
		const uint64_t address = begin + page * guest_page_size;
		if (Lumen::Loader::translate_guest_address(address, 64, (page & 1) != 0) != address) return false;
	}
	if (Lumen::Loader::translate_guest_address(begin, 64, false) != begin) return false;

	std::array<Lumen::Cpu::X64MemoryTranslationRegion, 2> regions {};
	if (Lumen::Loader::ExportGuestMemoryRegionsForTest(regions.data(), regions.size()) != regions.size() ||
	    regions[0].guest_begin != begin || regions[1].guest_begin != begin + 15 * guest_page_size)
	{
		return false;
	}

	const uint64_t last = begin + 16 * guest_page_size;
	if (Lumen::Loader::translate_guest_address(last, 64, false) != last) return false;
	regions = {};
	return Lumen::Loader::ExportGuestMemoryRegionsForTest(regions.data(), regions.size()) == regions.size() &&
	       regions[0].guest_begin == last && regions[1].guest_begin == begin + 15 * guest_page_size;
}

bool TestGuestPageCrossThreadInvalidation(size_t host_page_size)
{
	const size_t guest_page_size = PermissionGranule(host_page_size);

	Mapping mapping(std::max(host_page_size, 2 * guest_page_size));
	if (!mapping.IsValid()) return false;
	const uint64_t address = reinterpret_cast<uint64_t>(mapping.Data());

	std::atomic<uint32_t> stage {0};
	bool worker_ok = false;
	std::thread worker([&]
	{
		const bool warmed = Lumen::Loader::translate_guest_address(address, 64, true) == address;
		stage.store(1, std::memory_order_release);
		while (stage.load(std::memory_order_acquire) == 1)
			std::this_thread::yield();
		worker_ok = warmed && Lumen::Loader::translate_guest_address(address, 64, true) == 0 &&
		            Lumen::Loader::translate_guest_address(address + guest_page_size, 64, true) ==
		                address + guest_page_size;
	});
	while (stage.load(std::memory_order_acquire) == 0)
		std::this_thread::yield();
	const bool protected_modes =
	    mapping.Protect(0, guest_page_size, Lumen::Core::VirtualMemory::Mode::Read) &&
	    mapping.Protect(guest_page_size, guest_page_size, Lumen::Core::VirtualMemory::Mode::ReadWrite);
	stage.store(2, std::memory_order_release);
	worker.join();
	return protected_modes && worker_ok;
}

bool TestFreeInvalidatesPositiveCache(size_t page_size)
{
	Mapping mapping(page_size);
	if (!mapping.IsValid()) return false;

	const uint64_t address = reinterpret_cast<uint64_t>(mapping.Data());
	if (Lumen::Loader::translate_guest_address(address, 64, false) != address ||
	    Lumen::Loader::translate_guest_address(address + 128, 64, true) != address + 128)
	{
		return false;
	}

	const uint64_t generation = Lumen::Core::VirtualMemory::GetMutationGeneration();
	if (!mapping.Free() || Lumen::Core::VirtualMemory::GetMutationGeneration() == generation) return false;

	return Lumen::Loader::translate_guest_address(address, 64, false) == 0 && Lumen::Loader::translate_guest_address(address, 64, true) == 0;
}

bool TestCrossRegionRead(size_t page_size)
{
	Mapping mapping(2 * page_size);
	if (!mapping.IsValid() || !mapping.Protect(0, page_size, Lumen::Core::VirtualMemory::Mode::Read)) return false;
	const uint64_t address = reinterpret_cast<uint64_t>(mapping.Data()) + page_size - 4;
	return Lumen::Loader::translate_guest_address(address, 8, false) == address;
}

bool TestCrossRegionWriteDenied(size_t page_size)
{
	Mapping mapping(2 * page_size);
	if (!mapping.IsValid() || !mapping.Protect(page_size, page_size, Lumen::Core::VirtualMemory::Mode::Read)) return false;
	const uint64_t address = reinterpret_cast<uint64_t>(mapping.Data()) + page_size - 4;
	return Lumen::Loader::translate_guest_address(address, 8, true) == 0;
}

bool TestGapDenied(size_t page_size)
{
	Mapping mapping(2 * page_size);
	if (!mapping.IsValid() || !mapping.Protect(page_size, page_size, Lumen::Core::VirtualMemory::Mode::NoAccess)) return false;
	const uint64_t address = reinterpret_cast<uint64_t>(mapping.Data()) + page_size - 4;
	return Lumen::Loader::translate_guest_address(address, 8, false) == 0;
}

enum class HostPipeline
{
	Precise,
	FastBlock,
};

// These RuntimeLinker cases deliberately stop at FastBlock because their
// indirect CALL r11 is not eligible for the A64 tier. CpuX64Test's
// A64DynarecMixedPipeline gate uses CALL rel32 and the native-step counter.
void ConfigureHostPipeline(HostPipeline pipeline)
{
	setenv("LUMEN_X64_FAST_BLOCK_HOT_THRESHOLD", "2", 1);
	setenv("LUMEN_X64_FAST_BLOCKS", pipeline == HostPipeline::Precise ? "0" : "1", 1);
	setenv("LUMEN_X64_A64_DYNAREC", "0", 1);
}

void ResetHostPipelineEnvironment()
{
	unsetenv("LUMEN_X64_FAST_BLOCK_HOT_THRESHOLD");
	unsetenv("LUMEN_X64_FAST_BLOCKS");
	unsetenv("LUMEN_X64_A64_DYNAREC");
}

#if defined(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY) || \
    defined(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
template <typename Predicate>
bool WaitForTestStage(Predicate&& predicate)
{
	const auto deadline =
	    std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!predicate())
	{
		if (std::chrono::steady_clock::now() >= deadline) return false;
		std::this_thread::yield();
	}
	return true;
}
#endif

#if defined(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY)
bool TestStartupHostCallLivenessCase(size_t page_size, HostPipeline pipeline)
{
	Mapping code(page_size);
	Mapping stack(2 * page_size);
	if (!code.IsValid() || !stack.IsValid()) return false;
	constexpr std::array<uint8_t, 5> program = {
	    0x41, 0xff, 0xd3, // CALL r11
	    0xff, 0xe3,       // JMP rbx (zero exits the interpreter)
	};
	std::memcpy(code.Data(), program.data(), program.size());
	auto* guest_stack = stack.Data() + page_size - 2 * sizeof(uint64_t);
	for (uint32_t index = 0; index < 2; index++)
	{
		std::memcpy(guest_stack + index * sizeof(uint64_t),
		            &HOST_ARGUMENTS[6 + index], sizeof(uint64_t));
	}
	if (!stack.Protect(page_size, page_size,
	                   Lumen::Core::VirtualMemory::Mode::NoAccess))
	{
		return false;
	}

	Lumen::Loader::SymbolDatabase symbols;
	g_host_symbols = &symbols;
	g_blocking_host_target = reinterpret_cast<uint64_t>(&BlockingHostCall0);
	if (!Lumen::Loader::IsNativeHostCallTarget(g_blocking_host_target))
	{
		g_host_symbols = nullptr;
		g_blocking_host_target = 0;
		return false;
	}
	g_pause_before_host_boundary.store(false, std::memory_order_relaxed);
	g_allow_host_boundary.store(false, std::memory_order_relaxed);
	g_host_boundary_reached.store(false, std::memory_order_relaxed);
	g_blocking_host_enabled.store(false, std::memory_order_relaxed);
	g_blocking_host_entered.store(false, std::memory_order_relaxed);
	g_release_blocking_host.store(false, std::memory_order_relaxed);

	std::atomic<bool> warm {false};
	std::atomic<bool> epoch_open {false};
	std::atomic<bool> worker_done {false};
	bool worker_ok = false;
	std::thread worker([&]()
	{
		ConfigureHostPipeline(pipeline);
		if (pipeline == HostPipeline::FastBlock)
		{
			// Match the production-shaped negative path: dynarec is requested,
			// while CALL r11 makes this block ineligible and keeps it ordinary.
			setenv("LUMEN_X64_A64_DYNAREC", "1", 1);
		}
		const uint64_t code_address = reinterpret_cast<uint64_t>(code.Data());
		const auto run_once = [&]()
		{
			Lumen::Cpu::X64State state {};
			InitializeHostCallState(
			    &state, code_address, reinterpret_cast<uint64_t>(guest_stack),
			    g_blocking_host_target);
			const auto result =
			    Lumen::Cpu::X64Interpreter(
			        ProductionHostCallBridge,
			        Lumen::Loader::translate_guest_address)
			        .Run(&state, 8);
			return result == Lumen::Cpu::X64RunResult::Returned &&
			       state.rip == 0 && state.memory_fault_size == 0;
		};

		bool ok = true;
		if (pipeline == HostPipeline::FastBlock)
		{
			for (uint32_t iteration = 0; iteration < 4; iteration++)
				ok = run_once() && ok;
			// Reset the test counter after warm-up: a non-zero value below must
			// come from the in-epoch run that reaches the blocking HLE boundary.
			Lumen::Cpu::X64SetFastBlockTestTarget(code_address);
		}
		warm.store(true, std::memory_order_release);
		while (!epoch_open.load(std::memory_order_acquire))
			std::this_thread::yield();

		g_host_boundary_reached.store(false, std::memory_order_relaxed);
		g_blocking_host_entered.store(false, std::memory_order_relaxed);
		g_release_blocking_host.store(false, std::memory_order_relaxed);
		g_allow_host_boundary.store(false, std::memory_order_relaxed);
		g_blocking_host_enabled.store(true, std::memory_order_release);
		g_pause_before_host_boundary.store(true, std::memory_order_release);
		ok = run_once() && ok;
		if (pipeline == HostPipeline::FastBlock)
			ok = Lumen::Cpu::X64FastBlockTestTargetInstructionCount() != 0 && ok;
		ResetHostPipelineEnvironment();
		worker_ok = ok;
		worker_done.store(true, std::memory_order_release);
	});

	bool ok = WaitForTestStage(
	    [&] { return warm.load(std::memory_order_acquire); });
	ok = Lumen::Cpu::X64BeginA64DynarecStartupTelemetryForTest(
	         1000, 8, 16) &&
	     ok;
	auto lower = Lumen::Cpu::X64AdmitA64DynarecStartupTelemetryForTest(1010);
	ok = lower.shard_token != 0 &&
	     Lumen::Cpu::X64RecordA64DynarecStartupCostForTest(
	         &lower, Lumen::Cpu::X64A64DynarecStartupCostForTest::Lower,
	         1030) &&
	     ok;
	epoch_open.store(true, std::memory_order_release);
	const bool boundary_wait = WaitForTestStage([&]
	{
		return g_host_boundary_reached.load(std::memory_order_acquire) ||
		       worker_done.load(std::memory_order_acquire);
	});
	ok = boundary_wait &&
	     g_host_boundary_reached.load(std::memory_order_acquire) && ok;
	const auto before_boundary =
	    Lumen::Cpu::X64GetA64DynarecStartupTelemetryForTest(0, 0, 1);
	ok = before_boundary.accepting && !before_boundary.complete &&
	     before_boundary.active_writers == 1 && before_boundary.reports == 0 &&
	     ok;

	const bool close_before_boundary = pipeline == HostPipeline::Precise;
	bool claim = true;
	if (close_before_boundary)
	{
		claim = Lumen::Cpu::X64ClaimA64DynarecFirstVideoSubmitForTest(1500);
		Lumen::Cpu::X64FinishA64DynarecFirstVideoSubmit();
		const auto waiting =
		    Lumen::Cpu::X64GetA64DynarecStartupTelemetryForTest(0, 0, 1);
		ok = claim && waiting.closed && !waiting.complete &&
		     waiting.active_writers == 1 && waiting.reports == 0 && ok;
	}
	g_allow_host_boundary.store(true, std::memory_order_release);
	const bool callback_wait = WaitForTestStage([&]
	{
		return g_blocking_host_entered.load(std::memory_order_acquire) ||
		       worker_done.load(std::memory_order_acquire);
	});
	ok = callback_wait &&
	     g_blocking_host_entered.load(std::memory_order_acquire) && ok;
	const auto callback_held =
	    Lumen::Cpu::X64GetA64DynarecStartupTelemetryForTest(0, 0, 1);
	if (!close_before_boundary)
	{
		claim = callback_held.accepting &&
		        callback_held.active_writers == 0 &&
		        Lumen::Cpu::X64ClaimA64DynarecFirstVideoSubmitForTest(1500);
		Lumen::Cpu::X64FinishA64DynarecFirstVideoSubmit();
	} else
	{
		claim = claim && callback_held.closed &&
		        callback_held.active_writers == 0;
	}
	const auto report_while_held =
	    Lumen::Cpu::X64GetA64DynarecStartupTelemetryForTest(0, 0, 1);
	ok = claim && report_while_held.complete && !report_while_held.invalid &&
	     report_while_held.active_writers == 0 &&
	     report_while_held.reports == 1 &&
	     report_while_held.measured_ns == 20 &&
	     report_while_held.lower.events == 1 &&
	     report_while_held.lower.nanoseconds == 20 &&
	     report_while_held.ordinary.events == 0 &&
	     report_while_held.ordinary.steps == 0 &&
	     report_while_held.ordinary.nanoseconds == 0 &&
	     report_while_held.precise.events == 0 &&
	     report_while_held.precise.steps == 0 &&
	     report_while_held.precise.nanoseconds == 0 && ok;

	g_release_guest_boundary.store(true, std::memory_order_release);
	g_release_blocking_host.store(true, std::memory_order_release);
	worker.join();
	const auto after_return =
	    Lumen::Cpu::X64GetA64DynarecStartupTelemetryForTest(0, 0, 1);
	ok = worker_ok && after_return.complete && !after_return.invalid &&
	     after_return.reports == 1 && after_return.measured_ns == 20 &&
	     after_return.ordinary.events == 0 &&
	     after_return.precise.events == 0 && ok;

	g_pause_before_host_boundary.store(false, std::memory_order_relaxed);
	g_allow_host_boundary.store(false, std::memory_order_relaxed);
	g_host_boundary_reached.store(false, std::memory_order_relaxed);
	g_blocking_host_enabled.store(false, std::memory_order_relaxed);
	g_blocking_host_entered.store(false, std::memory_order_relaxed);
	g_release_blocking_host.store(false, std::memory_order_relaxed);
	g_blocking_host_target = 0;
	g_host_symbols = nullptr;
	return ok;
}

bool TestStartupGuestHostCallControl(size_t page_size)
{
	Mapping code(page_size);
	Mapping stack(page_size);
	if (!code.IsValid() || !stack.IsValid()) return false;
	constexpr std::array<uint8_t, 5> entry = {
	    0x41, 0xff, 0xd3, // CALL r11
	    0xff, 0xe3,       // JMP rbx (zero exits the interpreter)
	};
	constexpr uint8_t guest_return = 0xc3;
	std::memcpy(code.Data(), entry.data(), entry.size());
	std::memcpy(code.Data() + 64, &guest_return, sizeof(guest_return));
	const uint64_t code_address = reinterpret_cast<uint64_t>(code.Data());
	const uint64_t guest_target = code_address + 64;
	const uint64_t guest_stack =
	    reinterpret_cast<uint64_t>(stack.Data() + page_size / 2);

	if (Lumen::Loader::IsNativeHostCallTarget(guest_target)) return false;
	Lumen::Loader::SymbolDatabase symbols;
	g_host_symbols = &symbols;
	g_guest_control_target = guest_target;
	g_guest_boundary_reached.store(false, std::memory_order_relaxed);
	g_release_guest_boundary.store(false, std::memory_order_relaxed);
	const auto monotonic_ns = []()
	{
		return static_cast<uint64_t>(std::chrono::duration_cast<
		    std::chrono::nanoseconds>(std::chrono::steady_clock::now()
		                                 .time_since_epoch())
		                                 .count());
	};
	const uint64_t begin_ns = monotonic_ns();
	bool ok = begin_ns != 0 &&
	          Lumen::Cpu::X64BeginA64DynarecStartupTelemetryForTest(
	              begin_ns, 8, 16);
	bool worker_ok = false;
	std::atomic<bool> worker_done {false};
	std::thread worker([&]()
	{
		ConfigureHostPipeline(HostPipeline::Precise);
		Lumen::Cpu::X64State state {};
		InitializeHostCallState(
		    &state, code_address, guest_stack, guest_target);
		const auto result =
		    Lumen::Cpu::X64Interpreter(
		        ProductionHostCallBridge,
		        Lumen::Loader::translate_guest_address)
		        .Run(&state, 8);
		worker_ok = result == Lumen::Cpu::X64RunResult::Returned &&
		            state.rip == 0 && state.memory_fault_size == 0;
		ResetHostPipelineEnvironment();
		worker_done.store(true, std::memory_order_release);
	});

	const bool guest_wait = WaitForTestStage([&]
	{
		return g_guest_boundary_reached.load(std::memory_order_acquire) ||
		       worker_done.load(std::memory_order_acquire);
	});
	ok = guest_wait &&
	     g_guest_boundary_reached.load(std::memory_order_acquire) && ok;
	const auto guest_held =
	    Lumen::Cpu::X64GetA64DynarecStartupTelemetryForTest(0, 0, 1);
	ok = guest_held.accepting && !guest_held.complete &&
	     guest_held.active_writers == 1 && guest_held.reports == 0 && ok;
	uint64_t cutoff_ns = monotonic_ns();
	if (cutoff_ns <= begin_ns) cutoff_ns = begin_ns + 1;
	ok = Lumen::Cpu::X64CloseA64DynarecStartupTelemetryForTest(cutoff_ns) &&
	     ok;
	const auto guest_closing =
	    Lumen::Cpu::X64GetA64DynarecStartupTelemetryForTest(0, 0, 1);
	ok = guest_closing.closed && !guest_closing.complete &&
	     guest_closing.active_writers == 1 &&
	     guest_closing.reports == 0 && ok;
	g_release_guest_boundary.store(true, std::memory_order_release);
	worker.join();
	const auto report =
	    Lumen::Cpu::X64GetA64DynarecStartupTelemetryForTest(0, 0, 1);
	ok = worker_ok && report.complete && !report.invalid &&
	     report.active_writers == 0 && report.reports == 1 &&
	     report.lower.events == 0 && report.compile.events == 0 &&
	     report.reset.events == 0 && report.ordinary.events == 0 &&
	     report.precise.events == 1 && report.precise.steps == 1 &&
	     report.precise.nanoseconds != 0 &&
	     report.measured_ns == report.precise.nanoseconds &&
	     report.zero.events == report.precise.events &&
	     report.zero.steps == report.precise.steps &&
	     report.zero.nanoseconds == report.precise.nanoseconds && ok;

	g_guest_boundary_reached.store(false, std::memory_order_relaxed);
	g_release_guest_boundary.store(false, std::memory_order_relaxed);
	g_guest_control_target = 0;
	g_host_symbols = nullptr;
	return ok;
}

bool TestStartupHostCallLiveness(size_t page_size)
{
	return TestStartupHostCallLivenessCase(page_size, HostPipeline::Precise) &&
	       TestStartupHostCallLivenessCase(page_size, HostPipeline::FastBlock) &&
	       TestStartupGuestHostCallControl(page_size);
}
#endif

#if defined(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
struct Phase6AFHostCallContext
{
	const Lumen::Loader::SymbolDatabase* symbols = nullptr;
	uint64_t target = 0;
	Lumen::Cpu::X64State* state = nullptr;
};

struct Phase6AFHostPipelineContext
{
	const Lumen::Loader::SymbolDatabase* symbols = nullptr;
	bool paired_native_simulated = false;
};

thread_local Phase6AFHostPipelineContext* g_phase6af_host_pipeline = nullptr;

bool RunPhase6AFHostCall(void* opaque)
{
	auto* context = static_cast<Phase6AFHostCallContext*>(opaque);
	return context != nullptr && context->symbols != nullptr &&
	       context->state != nullptr &&
	       Lumen::Loader::RunHostCallForTest(
	           context->symbols, context->target, context->state);
}

bool Phase6AFHostCallBridge(uint64_t target, Lumen::Cpu::X64State* state)
{
	auto* pipeline = g_phase6af_host_pipeline;
	if (pipeline == nullptr || pipeline->symbols == nullptr || state == nullptr)
		return false;
	Phase6AFHostCallContext context {pipeline->symbols, target, state};
	return Lumen::Cpu::X64RunSelectedA64DynarecPhase6AFSampleForTest(
	    pipeline->paired_native_simulated, &RunPhase6AFHostCall, &context);
}

bool Phase6AFCostIsEmpty(
	const Lumen::Cpu::X64A64DynarecPhase6AFCostForTest& cost)
{
	return cost.events == 0 && cost.raw_nanoseconds == 0 &&
	       cost.corrected_nanoseconds == 0;
}

bool TestPhase6AFHostCallLivenessCase(size_t page_size,
	                                  bool paired_native_simulated)
{
	const uint64_t target = reinterpret_cast<uint64_t>(&BlockingHostCall0);
	if (!Lumen::Loader::IsNativeHostCallTarget(target)) return false;
	Mapping code(page_size);
	Mapping stack(2 * page_size);
	if (!code.IsValid() || !stack.IsValid()) return false;
	constexpr std::array<uint8_t, 5> program = {
	    0x41, 0xff, 0xd3, // CALL r11
	    0xff, 0xe3,       // JMP rbx (zero exits the interpreter)
	};
	std::memcpy(code.Data(), program.data(), program.size());
	auto* guest_stack = stack.Data() + page_size - 2 * sizeof(uint64_t);
	for (uint32_t index = 0; index < 2; index++)
	{
		std::memcpy(guest_stack + index * sizeof(uint64_t),
		            &HOST_ARGUMENTS[6 + index], sizeof(uint64_t));
	}
	if (!stack.Protect(page_size, page_size,
	                   Lumen::Core::VirtualMemory::Mode::NoAccess))
	{
		return false;
	}
	const auto monotonic_ns = []()
	{
		return static_cast<uint64_t>(std::chrono::duration_cast<
		    std::chrono::nanoseconds>(std::chrono::steady_clock::now()
	                                 .time_since_epoch())
	                                 .count());
	};
	const uint64_t begin_ns = monotonic_ns();
	if (begin_ns == 0 ||
	    !Lumen::Cpu::X64BeginA64DynarecPhase6AFTelemetryForTest(
	        begin_ns, 8, paired_native_simulated ? 2 : 1, 1))
	{
		return false;
	}

	Lumen::Loader::SymbolDatabase symbols;
	g_blocking_host_enabled.store(true, std::memory_order_release);
	g_blocking_host_entered.store(false, std::memory_order_relaxed);
	g_release_blocking_host.store(false, std::memory_order_relaxed);
	std::atomic<bool> worker_done {false};
	bool worker_ok = false;
	std::thread worker([&]()
	{
		Lumen::Cpu::X64State state {};
		const uint64_t code_address = reinterpret_cast<uint64_t>(code.Data());
		InitializeHostCallState(
		    &state, code_address, reinterpret_cast<uint64_t>(guest_stack),
		    target);
		Phase6AFHostPipelineContext pipeline {
		    &symbols, paired_native_simulated};
		g_phase6af_host_pipeline = &pipeline;
		const auto result =
		    Lumen::Cpu::X64Interpreter(
		        Phase6AFHostCallBridge,
		        Lumen::Loader::translate_guest_address)
		        .Run(&state, 8);
		g_phase6af_host_pipeline = nullptr;
		worker_ok = result == Lumen::Cpu::X64RunResult::Returned &&
		            state.rip == 0 && state.memory_fault_size == 0;
		worker_done.store(true, std::memory_order_release);
	});

	const bool wait_ok = WaitForTestStage([&]
	{
		return g_blocking_host_entered.load(std::memory_order_acquire) ||
		       worker_done.load(std::memory_order_acquire);
	});
	const bool entered_ok =
	    g_blocking_host_entered.load(std::memory_order_acquire);
	const auto held =
	    Lumen::Cpu::X64GetA64DynarecPhase6AFTelemetryForTest();
	const bool held_ok = held.accepting && !held.complete &&
	                     held.active_samples == 0 && held.reports == 0;

	uint64_t cutoff_ns = monotonic_ns();
	if (cutoff_ns <= begin_ns) cutoff_ns = begin_ns + 1;
	const bool claim =
	    Lumen::Cpu::X64ClaimA64DynarecPhase6AFFirstVideoSubmitForTest(
	        cutoff_ns);
	Lumen::Cpu::X64FinishA64DynarecPhase6AFFirstVideoSubmit();
	const auto report =
	    Lumen::Cpu::X64GetA64DynarecPhase6AFTelemetryForTest();
	const bool report_ok =
	    claim && report.closed && report.complete && !report.invalid &&
	    report.active_samples == 0 && report.reports == 1 &&
	    report.scheduled_samples == 1 && report.complete_samples == 0 &&
	    report.paired_samples == 0 && report.unpaired_samples == 0 &&
	    report.excluded_hle_samples == 1 &&
	    report.excluded_reentrant_samples == 0 &&
	    report.excluded_cutoff_samples == 0 && report.record_drops == 0 &&
	    report.paired_steps == 0 && report.paired_direct_steps == 0 &&
	    report.all_direct_samples == 0 &&
	    report.helper_backed_samples == 0 &&
	    report.zero_step_samples == 0 &&
	    Phase6AFCostIsEmpty(report.try_all) &&
	    Phase6AFCostIsEmpty(report.try_paired) &&
	    Phase6AFCostIsEmpty(report.compiled_execute) &&
	    Phase6AFCostIsEmpty(report.paired_surrounding) &&
	    Phase6AFCostIsEmpty(report.try_unpaired);

	const bool second_claim =
	    Lumen::Cpu::X64ClaimA64DynarecPhase6AFFirstVideoSubmitForTest(
	        cutoff_ns + 1);
	Lumen::Cpu::X64FinishA64DynarecPhase6AFFirstVideoSubmit();
	const auto exactly_once =
	    Lumen::Cpu::X64GetA64DynarecPhase6AFTelemetryForTest();
	const bool exactly_once_ok =
	    !second_claim && exactly_once.complete && exactly_once.reports == 1 &&
	    exactly_once.excluded_hle_samples == 1 &&
	    exactly_once.complete_samples == 0 &&
	    Phase6AFCostIsEmpty(exactly_once.try_all) &&
	    Phase6AFCostIsEmpty(exactly_once.try_paired) &&
	    Phase6AFCostIsEmpty(exactly_once.compiled_execute);

	g_release_blocking_host.store(true, std::memory_order_release);
	worker.join();
	const auto after_return =
	    Lumen::Cpu::X64GetA64DynarecPhase6AFTelemetryForTest();
	const bool after_return_ok =
	    worker_ok && after_return.complete && after_return.reports == 1 &&
	    after_return.excluded_hle_samples == 1 &&
	    after_return.complete_samples == 0 && after_return.paired_steps == 0 &&
	    Phase6AFCostIsEmpty(after_return.try_all) &&
	    Phase6AFCostIsEmpty(after_return.compiled_execute);
	const bool ok = wait_ok && entered_ok && held_ok && report_ok &&
	                exactly_once_ok && after_return_ok;
	if (!ok)
	{
		std::fprintf(
		    stderr,
		    "phase6af-hle-detail paired=%u wait=%u entered=%u held=%u"
		    " report=%u exactly-once=%u after-return=%u worker=%u"
		    " scheduled=%" PRIu64 " hle=%" PRIu64 " active=%" PRIu64
		    " reports=%" PRIu64 " invalid=%u\n",
		    paired_native_simulated ? 1u : 0u, wait_ok ? 1u : 0u,
		    entered_ok ? 1u : 0u, held_ok ? 1u : 0u,
		    report_ok ? 1u : 0u, exactly_once_ok ? 1u : 0u,
		    after_return_ok ? 1u : 0u, worker_ok ? 1u : 0u,
		    after_return.scheduled_samples,
		    after_return.excluded_hle_samples, after_return.active_samples,
		    after_return.reports, after_return.invalid ? 1u : 0u);
	}

	g_blocking_host_enabled.store(false, std::memory_order_relaxed);
	g_blocking_host_entered.store(false, std::memory_order_relaxed);
	g_release_blocking_host.store(false, std::memory_order_relaxed);
	return ok;
}
#endif

bool TestGuestPageA64CrossThreadInvalidation(size_t host_page_size)
{
	const size_t guest_page_size = PermissionGranule(host_page_size);
	const size_t data_size = std::max(host_page_size, 2 * guest_page_size);
	if (!Lumen::Cpu::X64A64DynarecAvailableForTest()) return false;

	Mapping code(host_page_size);
	Mapping data(data_size);
	if (!code.IsValid() || !data.IsValid()) return false;

	constexpr std::array<uint8_t, 6> write_program = {
	    0x48, 0x89, 0x07, // MOV qword ptr [rdi], rax
	    0xeb, 0x00,       // JMP to the following RET
	    0xc3,
	};
	constexpr std::array<uint8_t, 6> read_program = {
	    0x48, 0x8b, 0x07, // MOV rax, qword ptr [rdi]
	    0xeb, 0x00,       // JMP to the following RET
	    0xc3,
	};
	std::memcpy(code.Data(), write_program.data(), write_program.size());
	std::memcpy(code.Data() + 64, read_program.data(), read_program.size());

	const uint64_t write_entry = reinterpret_cast<uint64_t>(code.Data());
	const uint64_t read_entry = write_entry + 64;
	const uint64_t data_begin = reinterpret_cast<uint64_t>(data.Data());
	const uint64_t same_page_address = data_begin + 64;
	const uint64_t cross_page_address = data_begin + guest_page_size - 4;
	constexpr uint64_t store_value = 0x0123456789abcdefULL;
	constexpr uint64_t memory_sentinel = 0xa5a5a5a5a5a5a5a5ULL;
	constexpr uint64_t register_sentinel = 0x8877665544332211ULL;
	std::memcpy(data.Data() + 64, &memory_sentinel, sizeof(memory_sentinel));
	std::memcpy(data.Data() + guest_page_size - 4, &memory_sentinel, sizeof(memory_sentinel));

	setenv("LUMEN_X64_FAST_BLOCK_HOT_THRESHOLD", "2", 1);
	setenv("LUMEN_X64_FAST_BLOCKS", "1", 1);
	setenv("LUMEN_X64_A64_DYNAREC", "1", 1);

	std::atomic<uint32_t> stage {0};
	bool worker_ok = false;
	std::thread worker([&]
	{
		bool ok = true;
		Lumen::Cpu::X64ResetA64DynarecCacheForTest();
		Lumen::Loader::ConfigureGuestMemoryPipelineWatchForTest(
		    data_begin, data_begin + data_size);

		const uint32_t read_permission =
		    static_cast<uint32_t>(Lumen::Cpu::X64MemoryPermission::Read);
		const uint32_t write_permission =
		    static_cast<uint32_t>(Lumen::Cpu::X64MemoryPermission::Write);
		const auto has_export = [&](uint64_t begin, uint64_t end, uint32_t permissions)
		{
			std::array<Lumen::Cpu::X64MemoryTranslationRegion, 16> regions {};
			const size_t count =
			    Lumen::Loader::ExportGuestMemoryRegionsForTest(regions.data(), regions.size());
			for (size_t index = 0; index < count; index++)
			{
				if (regions[index].guest_begin == begin && regions[index].guest_end == end &&
				    regions[index].host_begin == begin && regions[index].permissions == permissions)
				{
					return true;
				}
			}
			return false;
		};
		const auto export_is_empty = [&]
		{
			std::array<Lumen::Cpu::X64MemoryTranslationRegion, 16> regions {};
			return Lumen::Loader::ExportGuestMemoryRegionsForTest(
			           regions.data(), regions.size()) == 0;
		};
		const auto run = [&](uint64_t entry, uint64_t address, uint64_t rax,
		                     Lumen::Cpu::X64State* state)
		{
			*state = {};
			state->rip = entry;
			state->gpr[0] = rax;
			state->gpr[7] = address;
			state->rflags = 0x8d5;
			return Lumen::Loader::RunGuestMemoryPipelineForTest(state, 2);
		};

		ok = ok && Lumen::Loader::translate_guest_address(same_page_address, sizeof(uint64_t), true) ==
		               same_page_address &&
		     has_export(data_begin, data_begin + data_size,
		                read_permission | write_permission);
		for (uint8_t iteration = 0; iteration < 4; iteration++)
		{
			std::memcpy(data.Data() + 64, &memory_sentinel, sizeof(memory_sentinel));
			Lumen::Cpu::X64State state {};
			ok = ok && run(write_entry, same_page_address, store_value, &state) ==
			               Lumen::Cpu::X64RunResult::StepLimit &&
			     state.rip == write_entry + 5;
		}
		std::memcpy(data.Data() + 64, &memory_sentinel, sizeof(memory_sentinel));
		Lumen::Cpu::X64SetA64DynarecTestTarget(write_entry);
		Lumen::Loader::ResetGuestMemoryPipelineCountersForTest();
		Lumen::Cpu::X64State allowed_store {};
		ok = ok && run(write_entry, same_page_address, store_value, &allowed_store) ==
		               Lumen::Cpu::X64RunResult::StepLimit &&
		     allowed_store.rip == write_entry + 5 &&
		     Lumen::Loader::GuestMemoryPipelineNonReentrantCapabilityObservedForTest() &&
		     Lumen::Cpu::X64A64DynarecTestTargetInstructionCount() == 2 &&
		     Lumen::Cpu::X64A64DynarecTestTargetDirectInstructionCount() == 2 &&
		     Lumen::Loader::GuestMemoryPipelineExportCountForTest() >= 1 &&
		     Lumen::Loader::GuestMemoryPipelineWatchedTranslateCountForTest() == 0 &&
		     std::memcmp(data.Data() + 64, &store_value, sizeof(store_value)) == 0;
		std::memcpy(data.Data() + 64, &memory_sentinel, sizeof(memory_sentinel));

		stage.store(1, std::memory_order_release);
		while (stage.load(std::memory_order_acquire) < 2)
			std::this_thread::yield();

		ok = ok && export_is_empty() &&
		     Lumen::Loader::translate_guest_address(same_page_address, sizeof(uint64_t), false) ==
		               same_page_address &&
		     Lumen::Loader::translate_guest_address(same_page_address, sizeof(uint64_t), true) == 0 &&
		     has_export(data_begin, data_begin + guest_page_size, read_permission);
		Lumen::Cpu::X64State denied_store {};
		Lumen::Cpu::X64SetA64DynarecTestTarget(write_entry);
		Lumen::Loader::ResetGuestMemoryPipelineCountersForTest();
		const auto denied_store_result =
		    run(write_entry, same_page_address, store_value, &denied_store);
		std::array<uint64_t, 16> denied_store_gpr {};
		denied_store_gpr[0] = store_value;
		denied_store_gpr[7] = same_page_address;
		ok = ok && denied_store_result == Lumen::Cpu::X64RunResult::MemoryFault &&
		     denied_store.rip == write_entry &&
		     denied_store.memory_fault_address == same_page_address &&
		     denied_store.memory_fault_size == sizeof(uint64_t) && denied_store.memory_fault_write &&
		     denied_store.rflags == 0x8d5 &&
		     std::memcmp(denied_store.gpr, denied_store_gpr.data(), sizeof(denied_store.gpr)) == 0 &&
		     std::memcmp(data.Data() + 64, &memory_sentinel, sizeof(memory_sentinel)) == 0 &&
		     Lumen::Cpu::X64A64DynarecTestTargetInstructionCount() == 1 &&
		     Lumen::Cpu::X64A64DynarecTestTargetDirectInstructionCount() == 0 &&
		     Lumen::Loader::GuestMemoryPipelineExportCountForTest() >= 1 &&
		     Lumen::Loader::GuestMemoryPipelineWatchedTranslateCountForTest() == 1;

		for (uint8_t iteration = 0; iteration < 4; iteration++)
		{
			Lumen::Cpu::X64State state {};
			ok = ok && run(read_entry, cross_page_address, register_sentinel, &state) ==
			               Lumen::Cpu::X64RunResult::StepLimit &&
			     state.rip == read_entry + 5;
		}
		Lumen::Cpu::X64SetA64DynarecTestTarget(read_entry);
		Lumen::Cpu::X64State allowed_cross_read {};
		ok = ok && run(read_entry, cross_page_address, register_sentinel, &allowed_cross_read) ==
		               Lumen::Cpu::X64RunResult::StepLimit &&
		     allowed_cross_read.rip == read_entry + 5 &&
		     allowed_cross_read.gpr[0] == memory_sentinel && allowed_cross_read.rflags == 0x8d5 &&
		     Lumen::Cpu::X64A64DynarecTestTargetInstructionCount() == 2 &&
		     Lumen::Cpu::X64A64DynarecTestTargetDirectInstructionCount() == 1;

		stage.store(3, std::memory_order_release);
		while (stage.load(std::memory_order_acquire) < 4)
			std::this_thread::yield();

		ok = ok && export_is_empty() &&
		     Lumen::Loader::translate_guest_address(cross_page_address, sizeof(uint64_t), false) == 0;
		Lumen::Cpu::X64State denied_read {};
		Lumen::Cpu::X64SetA64DynarecTestTarget(read_entry);
		const auto denied_read_result =
		    run(read_entry, cross_page_address, register_sentinel, &denied_read);
		std::array<uint64_t, 16> denied_read_gpr {};
		denied_read_gpr[0] = register_sentinel;
		denied_read_gpr[7] = cross_page_address;
		ok = ok && denied_read_result == Lumen::Cpu::X64RunResult::MemoryFault &&
		     denied_read.rip == read_entry &&
		     denied_read.memory_fault_address == cross_page_address &&
		     denied_read.memory_fault_size == sizeof(uint64_t) && !denied_read.memory_fault_write &&
		     denied_read.rflags == 0x8d5 &&
		     std::memcmp(denied_read.gpr, denied_read_gpr.data(), sizeof(denied_read.gpr)) == 0 &&
		     Lumen::Cpu::X64A64DynarecTestTargetInstructionCount() == 1 &&
		     Lumen::Cpu::X64A64DynarecTestTargetDirectInstructionCount() == 0;

		stage.store(5, std::memory_order_release);
		while (stage.load(std::memory_order_acquire) < 6)
			std::this_thread::yield();

		ok = ok && export_is_empty() &&
		     Lumen::Loader::translate_guest_address(cross_page_address, sizeof(uint64_t), true) == 0 &&
		     Lumen::Loader::translate_guest_address(data_begin + guest_page_size, sizeof(uint64_t), true) ==
		         data_begin + guest_page_size &&
		     has_export(data_begin, data_begin + guest_page_size, read_permission) &&
		     has_export(data_begin + guest_page_size, data_begin + data_size,
		                read_permission | write_permission);
		Lumen::Cpu::X64State denied_cross_store {};
		Lumen::Cpu::X64SetA64DynarecTestTarget(write_entry);
		const auto denied_cross_store_result =
		    run(write_entry, cross_page_address, store_value, &denied_cross_store);
		std::array<uint64_t, 16> denied_cross_store_gpr {};
		denied_cross_store_gpr[0] = store_value;
		denied_cross_store_gpr[7] = cross_page_address;
		ok = ok && denied_cross_store_result == Lumen::Cpu::X64RunResult::MemoryFault &&
		     denied_cross_store.rip == write_entry &&
		     denied_cross_store.memory_fault_address == cross_page_address &&
		     denied_cross_store.memory_fault_size == sizeof(uint64_t) &&
		     denied_cross_store.memory_fault_write && denied_cross_store.rflags == 0x8d5 &&
		     std::memcmp(denied_cross_store.gpr, denied_cross_store_gpr.data(),
		                 sizeof(denied_cross_store.gpr)) == 0 &&
		     std::memcmp(data.Data() + guest_page_size - 4, &memory_sentinel,
		                 sizeof(memory_sentinel)) == 0 &&
		     Lumen::Cpu::X64A64DynarecTestTargetInstructionCount() == 1 &&
		     Lumen::Cpu::X64A64DynarecTestTargetDirectInstructionCount() == 0;

		Lumen::Cpu::X64ResetA64DynarecCacheForTest();
		worker_ok = ok;
	});

	while (stage.load(std::memory_order_acquire) < 1)
		std::this_thread::yield();
	const uint64_t first_generation = Lumen::Core::VirtualMemory::GetMutationGeneration();
	bool main_ok = data.Protect(0, guest_page_size, Lumen::Core::VirtualMemory::Mode::Read) &&
	               Lumen::Core::VirtualMemory::GetMutationGeneration() != first_generation;
	stage.store(2, std::memory_order_release);

	while (stage.load(std::memory_order_acquire) < 3)
		std::this_thread::yield();
	const uint64_t second_generation = Lumen::Core::VirtualMemory::GetMutationGeneration();
	main_ok = data.Protect(guest_page_size, guest_page_size,
	                       Lumen::Core::VirtualMemory::Mode::NoAccess) &&
	          Lumen::Core::VirtualMemory::GetMutationGeneration() != second_generation && main_ok;
	stage.store(4, std::memory_order_release);

	while (stage.load(std::memory_order_acquire) < 5)
		std::this_thread::yield();
	const uint64_t third_generation = Lumen::Core::VirtualMemory::GetMutationGeneration();
	main_ok = data.Protect(guest_page_size, guest_page_size,
	                       Lumen::Core::VirtualMemory::Mode::ReadWrite) &&
	          Lumen::Core::VirtualMemory::GetMutationGeneration() != third_generation && main_ok;
	stage.store(6, std::memory_order_release);
	worker.join();
	ResetHostPipelineEnvironment();
	return main_ok && worker_ok;
}

bool RunGuestPageReadFaultCase(size_t host_page_size, HostPipeline pipeline)
{
	const size_t guest_page_size = PermissionGranule(host_page_size);

	Mapping code(host_page_size);
	Mapping data(std::max(host_page_size, 2 * guest_page_size));
	if (!code.IsValid() || !data.IsValid() ||
	    !data.Protect(guest_page_size, guest_page_size, Lumen::Core::VirtualMemory::Mode::NoAccess))
	{
		return false;
	}
	constexpr std::array<uint8_t, 5> program = {
	    0x48, 0x8b, 0x07, // MOV rax, qword ptr [rdi]
	    0xff, 0xe3,       // JMP rbx (zero exits)
	};
	std::memcpy(code.Data(), program.data(), program.size());
	const uint64_t code_address = reinterpret_cast<uint64_t>(code.Data());
	const uint64_t fault_address = reinterpret_cast<uint64_t>(data.Data()) + guest_page_size - 4;
	constexpr uint64_t rax_sentinel = 0x8877665544332211ULL;

	ConfigureHostPipeline(pipeline);
	if (pipeline == HostPipeline::FastBlock) Lumen::Cpu::X64SetFastBlockTestTarget(code_address);
	const uint32_t iterations = pipeline == HostPipeline::Precise ? 1 : 4;
	for (uint32_t iteration = 0; iteration < iterations; iteration++)
	{
		Lumen::Cpu::X64State state {};
		state.rip    = code_address;
		state.gpr[0] = rax_sentinel;
		state.gpr[7] = fault_address;
		const auto result = Lumen::Cpu::X64Interpreter(TestHostCallBridge, Lumen::Loader::translate_guest_address).Run(&state, 8);
		if (result != Lumen::Cpu::X64RunResult::MemoryFault || state.rip != code_address ||
		    state.gpr[0] != rax_sentinel || state.memory_fault_address != fault_address ||
		    state.memory_fault_size != sizeof(uint64_t) || state.memory_fault_write)
		{
			ResetHostPipelineEnvironment();
			return false;
		}
	}
	const bool hot = pipeline != HostPipeline::FastBlock ||
	                 Lumen::Cpu::X64FastBlockTestTargetInstructionCount() != 0;
	ResetHostPipelineEnvironment();
	return hot;
}

void InitializeHostCallState(Lumen::Cpu::X64State* state, uint64_t code, uint64_t stack, uint64_t target)
{
	*state         = {};
	state->rip     = code;
	state->gpr[0]  = 0xfeedfacecafebeefULL;
	state->gpr[1]  = HOST_ARGUMENTS[3];
	state->gpr[2]  = HOST_ARGUMENTS[2];
	state->gpr[3]  = 0;
	state->gpr[4]  = stack;
	state->gpr[6]  = HOST_ARGUMENTS[1];
	state->gpr[7]  = HOST_ARGUMENTS[0];
	state->gpr[8]  = HOST_ARGUMENTS[4];
	state->gpr[9]  = HOST_ARGUMENTS[5];
	state->gpr[11] = target;
}

bool HostCaptureMatches(uint32_t callback_arity)
{
	if (g_host_argument_count != callback_arity) return false;
	auto expected = HOST_ARGUMENTS;
	if (callback_arity == 10)
	{
		expected[1] = static_cast<uint32_t>(expected[1]);
	} else if (callback_arity == 11)
	{
		expected[10] = static_cast<uint16_t>(expected[10]);
	} else if (callback_arity == 12)
	{
		expected[1]  = static_cast<uint8_t>(expected[1]);
		expected[2]  = static_cast<uint16_t>(expected[2]);
		expected[3]  = static_cast<uint8_t>(expected[3]);
		expected[4]  = static_cast<uint8_t>(expected[4]);
		expected[6]  = static_cast<uint8_t>(expected[6]);
		expected[8]  = static_cast<uint16_t>(expected[8]);
		expected[9]  = static_cast<uint16_t>(expected[9]);
		expected[10] = static_cast<uint8_t>(expected[10]);
		expected[11] = static_cast<uint32_t>(expected[11]);
	}
	for (uint32_t index = 0; index < callback_arity; index++)
	{
		if (g_host_arguments[index] != expected[index]) return false;
	}
	return true;
}

uint64_t ExpectedHostReturn(uint32_t callback_arity)
{
	if (callback_arity == 9) return static_cast<uint32_t>(HOST_INT32_RETURN);
	if (callback_arity == 10) return reinterpret_cast<uint64_t>(&g_ajm_return_marker);
	if (callback_arity == 11) return 0;
	if (callback_arity == 12) return reinterpret_cast<uint64_t>(&g_mixed_return_marker);
	return HOST_RETURN_BASE + callback_arity;
}

bool RunHostCallCase(size_t page_size, uint32_t callback_arity,
	                 HostPipeline pipeline, bool verify_native_target = true,
	                 Lumen::Cpu::X64Interpreter::HostCall host_call = TestHostCallBridge)
{
	if (callback_arity >= HOST_FUNCTIONS.size()) return false;
	const uint64_t target = HOST_FUNCTIONS[callback_arity];
	if (verify_native_target && !Lumen::Loader::IsNativeHostCallTarget(target))
		return false;

	Mapping code(page_size);
	Mapping stack(2 * page_size);
	if (!code.IsValid() || !stack.IsValid()) return false;
	constexpr std::array<uint8_t, 5> program = {
	    0x41, 0xff, 0xd3, // CALL r11
	    0xff, 0xe3,       // JMP rbx (zero exits the interpreter)
	};
	std::memcpy(code.Data(), program.data(), program.size());

	const uint32_t bridge_arity    = callback_arity <= 8 ? 8 : callback_arity;
	const uint32_t stack_arguments = bridge_arity - 6;
	const size_t   stack_size      = static_cast<size_t>(stack_arguments) * sizeof(uint64_t);
	if (stack_size > page_size) return false;
	// No bytes after the exact argument footprint are readable. In particular,
	// the legacy arity-eight case proves that rsp+24 is never touched.
	auto* guest_stack = stack.Data() + page_size - stack_size;
	for (uint32_t index = 0; index < stack_arguments; index++)
	{
		std::memcpy(guest_stack + index * sizeof(uint64_t), &HOST_ARGUMENTS[6 + index], sizeof(uint64_t));
	}
	if (!stack.Protect(page_size, page_size, Lumen::Core::VirtualMemory::Mode::NoAccess)) return false;

	ConfigureHostPipeline(pipeline);
	const uint64_t code_address = reinterpret_cast<uint64_t>(code.Data());
	if (pipeline == HostPipeline::FastBlock) Lumen::Cpu::X64SetFastBlockTestTarget(code_address);
	g_host_call_count         = 0;
	g_host_argument_count     = 0;
	const uint32_t iterations = pipeline == HostPipeline::Precise ? 1 : 4;
	for (uint32_t iteration = 0; iteration < iterations; iteration++)
	{
		Lumen::Cpu::X64State state {};
		InitializeHostCallState(&state, code_address, reinterpret_cast<uint64_t>(guest_stack), target);
		const auto result = Lumen::Cpu::X64Interpreter(host_call, Lumen::Loader::translate_guest_address).Run(&state, 8);
		if (result != Lumen::Cpu::X64RunResult::Returned || state.rip != 0 || state.gpr[0] != ExpectedHostReturn(callback_arity) ||
		    state.memory_fault_size != 0 || !HostCaptureMatches(callback_arity))
		{
			return false;
		}
	}
	return g_host_call_count == iterations &&
	       (pipeline != HostPipeline::FastBlock || Lumen::Cpu::X64FastBlockTestTargetInstructionCount() != 0);
}

bool RootFrameHostCaptureMatches(uint32_t callback_arity)
{
	if (g_host_argument_count != callback_arity) return false;
	auto expected = HOST_ARGUMENTS;
	if (callback_arity == Lumen::Loader::HOST_CALL_GP_MAX_ARITY)
	{
		expected[1] = static_cast<uint8_t>(expected[1]);
		expected[2] = static_cast<uint16_t>(expected[2]);
		expected[3] = static_cast<uint8_t>(expected[3]);
		expected[4] = static_cast<uint8_t>(expected[4]);
	}
	for (uint32_t index = Lumen::Loader::HOST_CALL_GP_REGISTER_ARGUMENT_COUNT;
	     index < callback_arity; index++)
	{
		expected[index] = 0;
	}
	for (uint32_t index = 0; index < callback_arity; index++)
	{
		if (g_host_arguments[index] != expected[index]) return false;
	}
	return true;
}

bool RunGuestRootStackHostCallCase(size_t page_size, uint32_t callback_arity,
	                               HostPipeline pipeline, bool direct_jump)
{
	if (callback_arity >= HOST_FUNCTIONS.size()) return false;
	const size_t stack_size = Lumen::Loader::GuestX64StackSizeForTest();
	const size_t frame_size = Lumen::Loader::GuestX64RootFrameSizeForTest();
	if (stack_size < frame_size || stack_size > SIZE_MAX - page_size ||
	    frame_size != EXPECTED_GUEST_ROOT_FRAME_SIZE ||
	    stack_size % page_size != 0)
	{
		return false;
	}

	Mapping code(page_size);
	Mapping stack(stack_size + page_size);
	if (!code.IsValid() || !stack.IsValid() ||
	    !stack.Protect(stack_size, page_size,
	                   Lumen::Core::VirtualMemory::Mode::NoAccess))
	{
		return false;
	}
	constexpr std::array<uint8_t, 5> call_program = {
	    0x41, 0xff, 0xd3, // CALL r11
	    0xff, 0xe3,       // JMP rbx (zero exits)
	};
	constexpr std::array<uint8_t, 3> jump_program = {
	    0x41, 0xff, 0xe3, // JMP r11; handled HLE pops the root return slot
	};
	if (direct_jump)
		std::memcpy(code.Data(), jump_program.data(), jump_program.size());
	else
		std::memcpy(code.Data(), call_program.data(), call_program.size());

	uint64_t rejected_rsp = 0xfeedfacecafebeefULL;
	if (Lumen::Loader::InitializeGuestX64RootStackForTest(
	        stack.Data(), frame_size - 1, &rejected_rsp) ||
	    rejected_rsp != 0xfeedfacecafebeefULL)
	{
		return false;
	}
	uint64_t tagged_rsp = 0xfeedfacecafebeefULL;
	const auto tagged_stack = reinterpret_cast<uint8_t*>(
	    reinterpret_cast<uintptr_t>(stack.Data()) |
	    UINT64_C(0xb400000000000000));
	if (Lumen::Loader::InitializeGuestX64RootStackForTest(
	        tagged_stack, stack_size, &tagged_rsp) ||
	    tagged_rsp != 0xfeedfacecafebeefULL)
	{
		return false;
	}

	ConfigureHostPipeline(pipeline);
	const uint64_t code_address = reinterpret_cast<uint64_t>(code.Data());
	if (pipeline == HostPipeline::FastBlock)
		Lumen::Cpu::X64SetFastBlockTestTarget(code_address);
	g_host_call_count = 0;
	g_host_argument_count = 0;
	const uint32_t iterations = pipeline == HostPipeline::Precise ? 1 : 4;
	for (uint32_t iteration = 0; iteration < iterations; iteration++)
	{
		uint64_t root_rsp = 0;
		if (!Lumen::Loader::InitializeGuestX64RootStackForTest(
		        stack.Data(), stack_size, &root_rsp))
		{
			ResetHostPipelineEnvironment();
			return false;
		}
		const uint64_t stack_top =
		    reinterpret_cast<uint64_t>(stack.Data()) + stack_size;
		if (root_rsp != stack_top - frame_size || (root_rsp & 15u) != 8u ||
		    Lumen::Loader::translate_guest_address(
		        root_rsp + frame_size - sizeof(uint64_t), sizeof(uint64_t),
		        false) == 0 ||
		    Lumen::Loader::translate_guest_address(stack_top, 1, false) != 0)
		{
			ResetHostPipelineEnvironment();
			return false;
		}
		for (size_t byte = 0; byte < frame_size; byte++)
		{
			if (stack.Data()[stack_size - frame_size + byte] != 0)
			{
				ResetHostPipelineEnvironment();
				return false;
			}
		}

		Lumen::Cpu::X64State state {};
		InitializeHostCallState(&state, code_address, root_rsp,
		                        HOST_FUNCTIONS[callback_arity]);
		const auto result =
		    Lumen::Cpu::X64Interpreter(TestHostCallBridge,
		                               Lumen::Loader::translate_guest_address)
		        .Run(&state, 8);
		const uint64_t expected_rsp =
		    direct_jump ? root_rsp + sizeof(uint64_t) : root_rsp;
		if (result != Lumen::Cpu::X64RunResult::Returned || state.rip != 0 ||
		    state.gpr[4] != expected_rsp || state.memory_fault_size != 0 ||
		    state.host_call_stack_transaction.active ||
		    state.gpr[0] != ExpectedHostReturn(callback_arity) ||
		    !RootFrameHostCaptureMatches(callback_arity))
		{
			ResetHostPipelineEnvironment();
			return false;
		}
	}
	const bool hot =
	    pipeline != HostPipeline::FastBlock ||
	    Lumen::Cpu::X64FastBlockTestTargetInstructionCount() != 0;
	ResetHostPipelineEnvironment();
	return hot && g_host_call_count == iterations;
}

bool TestGuestRootStackHostCallFrame(size_t page_size)
{
	Lumen::Loader::SymbolDatabase symbols;
	RegisterTypedHostCalls(&symbols);
	g_host_symbols = &symbols;
	bool ok = true;
	for (const uint32_t arity: {
	         Lumen::Loader::HOST_CALL_GP_LEGACY_ARITY,
	         Lumen::Loader::HOST_CALL_GP_MAX_ARITY})
	{
		for (const bool direct_jump: {false, true})
		{
			for (const HostPipeline pipeline: {
			         HostPipeline::Precise, HostPipeline::FastBlock})
			{
				// Keep executing the complete matrix after a failure so each
				// pipeline is independently exercised under sanitizers.
				ok = RunGuestRootStackHostCallCase(
				         page_size, arity, pipeline, direct_jump) && ok;
			}
		}
	}
	g_host_symbols = nullptr;
	ResetHostPipelineEnvironment();
	return ok;
}

enum class HostIndirectCall
{
	Register,
	Memory,
};

bool RunHostCallFaultCase(size_t page_size, uint32_t fail_slot, HostPipeline pipeline,
	                      HostIndirectCall call_kind = HostIndirectCall::Register)
{
	if (fail_slot >= 6) return false;
	Mapping code(page_size);
	Mapping stack(2 * page_size);
	if (!code.IsValid() || !stack.IsValid()) return false;
	constexpr std::array<uint8_t, 5> register_program = {
	    0x41, 0xff, 0xd3, // CALL r11
	    0xff, 0xe3,       // JMP rbx
	};
	constexpr std::array<uint8_t, 5> memory_program = {
	    0x41, 0xff, 0x12, // CALL qword ptr [r10]
	    0xff, 0xe3,       // JMP rbx
	};
	const auto& program = call_kind == HostIndirectCall::Memory ? memory_program : register_program;
	std::memcpy(code.Data(), program.data(), program.size());
	const uint64_t host_target = HOST_FUNCTIONS[12];
	std::memcpy(code.Data() + 64, &host_target, sizeof(host_target));

	// The selected qword straddles the permission boundary. Every preceding
	// required slot is fully readable.
	auto* guest_stack = stack.Data() + page_size - 4 - fail_slot * sizeof(uint64_t);
	for (uint32_t index = 0; index < 6; index++)
	{
		std::memcpy(guest_stack + index * sizeof(uint64_t), &HOST_ARGUMENTS[6 + index], sizeof(uint64_t));
	}
	const uint64_t return_slot_sentinel = 0xd15ea5e000000000ULL | fail_slot;
	std::memcpy(guest_stack - sizeof(uint64_t), &return_slot_sentinel, sizeof(return_slot_sentinel));
	if (!stack.Protect(page_size, page_size, Lumen::Core::VirtualMemory::Mode::NoAccess)) return false;

	ConfigureHostPipeline(pipeline);
	const uint64_t code_address = reinterpret_cast<uint64_t>(code.Data());
	const uint64_t original_rsp = reinterpret_cast<uint64_t>(guest_stack);
	if (pipeline == HostPipeline::FastBlock) Lumen::Cpu::X64SetFastBlockTestTarget(code_address);
	g_override_guest_rsp      = false;
	g_host_call_count         = 0;
	g_host_argument_count     = 0;
	const uint32_t iterations = pipeline == HostPipeline::Precise ? 1 : 4;
	for (uint32_t iteration = 0; iteration < iterations; iteration++)
	{
		Lumen::Cpu::X64State state {};
		InitializeHostCallState(&state, code_address, original_rsp, HOST_FUNCTIONS[12]);
		state.gpr[10] = reinterpret_cast<uint64_t>(code.Data() + 64);
		const auto result = Lumen::Cpu::X64Interpreter(TestHostCallBridge, Lumen::Loader::translate_guest_address).Run(&state, 8);
		uint64_t return_slot_after = 0;
		std::memcpy(&return_slot_after, guest_stack - sizeof(uint64_t), sizeof(return_slot_after));
		if (result != Lumen::Cpu::X64RunResult::MemoryFault ||
		    state.rip != code_address || state.gpr[4] != original_rsp || state.host_call_stack_transaction.active ||
		    state.memory_fault_address != reinterpret_cast<uint64_t>(guest_stack + fail_slot * sizeof(uint64_t)) ||
		    state.memory_fault_size != sizeof(uint64_t) || state.memory_fault_write || state.gpr[0] != 0xfeedfacecafebeefULL ||
		    return_slot_after != return_slot_sentinel || g_host_call_count != 0)
		{
			return false;
		}
	}
	return pipeline != HostPipeline::FastBlock || Lumen::Cpu::X64FastBlockTestTargetInstructionCount() != 0;
}

bool RunDirectHostCallFaultCase(size_t page_size, uint32_t fail_slot, HostPipeline pipeline)
{
	if (fail_slot >= 6 || g_host_symbols == nullptr) return false;
	Mapping code(page_size);
	Mapping stack(2 * page_size);
	if (!code.IsValid() || !stack.IsValid()) return false;
	constexpr std::array<uint8_t, 7> program = {
	    0xe8, 0x00, 0x00, 0x00, 0x00, // CALL next instruction
	    0xff, 0xe3,                   // JMP rbx
	};
	std::memcpy(code.Data(), program.data(), program.size());
	const uint64_t code_address = reinterpret_cast<uint64_t>(code.Data());
	const uint64_t target       = code_address + 5;
	g_host_symbols->SetHostCallGpMetadata(target, HOST_CALL_12_METADATA);

	auto* guest_stack = stack.Data() + page_size - 4 - fail_slot * sizeof(uint64_t);
	for (uint32_t index = 0; index < 6; index++)
	{
		std::memcpy(guest_stack + index * sizeof(uint64_t), &HOST_ARGUMENTS[6 + index], sizeof(uint64_t));
	}
	const uint64_t return_slot_sentinel = 0xd1ec700000000000ULL | fail_slot;
	std::memcpy(guest_stack - sizeof(uint64_t), &return_slot_sentinel, sizeof(return_slot_sentinel));
	if (!stack.Protect(page_size, page_size, Lumen::Core::VirtualMemory::Mode::NoAccess)) return false;

	ConfigureHostPipeline(pipeline);
	if (pipeline == HostPipeline::FastBlock) Lumen::Cpu::X64SetFastBlockTestTarget(code_address);
	g_override_guest_rsp      = false;
	g_host_call_count         = 0;
	g_host_argument_count     = 0;
	const uint64_t original_rsp = reinterpret_cast<uint64_t>(guest_stack);
	const uint32_t iterations = pipeline == HostPipeline::Precise ? 1 : 4;
	for (uint32_t iteration = 0; iteration < iterations; iteration++)
	{
		Lumen::Cpu::X64State state {};
		InitializeHostCallState(&state, code_address, original_rsp, target);
		const auto result = Lumen::Cpu::X64Interpreter(TestHostCallBridge, Lumen::Loader::translate_guest_address).Run(&state, 8);
		uint64_t return_slot_after = 0;
		std::memcpy(&return_slot_after, guest_stack - sizeof(uint64_t), sizeof(return_slot_after));
		if (result != Lumen::Cpu::X64RunResult::MemoryFault || state.rip != code_address || state.gpr[4] != original_rsp ||
		    state.host_call_stack_transaction.active ||
		    state.memory_fault_address != reinterpret_cast<uint64_t>(guest_stack + fail_slot * sizeof(uint64_t)) ||
		    state.memory_fault_size != sizeof(uint64_t) || state.memory_fault_write || state.gpr[0] != 0xfeedfacecafebeefULL ||
		    return_slot_after != return_slot_sentinel || g_host_call_count != 0)
		{
			return false;
		}
	}
	return pipeline != HostPipeline::FastBlock || Lumen::Cpu::X64FastBlockTestTargetInstructionCount() != 0;
}

bool RunHostJumpFaultCase(size_t page_size, HostPipeline pipeline)
{
	Mapping code(page_size);
	Mapping stack(2 * page_size);
	if (!code.IsValid() || !stack.IsValid()) return false;
	constexpr std::array<uint8_t, 3> program = {
	    0x41, 0xff, 0xe3, // JMP r11
	};
	std::memcpy(code.Data(), program.data(), program.size());

	constexpr uint32_t fail_slot = 0;
	auto* guest_stack = stack.Data() + page_size - 4;
	for (uint32_t index = 0; index < 6; index++)
	{
		std::memcpy(guest_stack + index * sizeof(uint64_t), &HOST_ARGUMENTS[6 + index], sizeof(uint64_t));
	}
	const uint64_t return_slot_sentinel = 0x4a4d50484c45464fULL;
	std::memcpy(guest_stack - sizeof(uint64_t), &return_slot_sentinel, sizeof(return_slot_sentinel));
	if (!stack.Protect(page_size, page_size, Lumen::Core::VirtualMemory::Mode::NoAccess)) return false;

	ConfigureHostPipeline(pipeline);
	const uint64_t code_address = reinterpret_cast<uint64_t>(code.Data());
	const uint64_t original_rsp = reinterpret_cast<uint64_t>(guest_stack - sizeof(uint64_t));
	if (pipeline == HostPipeline::FastBlock) Lumen::Cpu::X64SetFastBlockTestTarget(code_address);
	g_host_call_count         = 0;
	g_host_argument_count     = 0;
	const uint32_t iterations = pipeline == HostPipeline::Precise ? 1 : 4;
	for (uint32_t iteration = 0; iteration < iterations; iteration++)
	{
		Lumen::Cpu::X64State state {};
		InitializeHostCallState(&state, code_address, original_rsp, HOST_FUNCTIONS[12]);
		const auto result = Lumen::Cpu::X64Interpreter(TestHostCallBridge, Lumen::Loader::translate_guest_address).Run(&state, 8);
		uint64_t return_slot_after = 0;
		std::memcpy(&return_slot_after, guest_stack - sizeof(uint64_t), sizeof(return_slot_after));
		if (result != Lumen::Cpu::X64RunResult::MemoryFault || state.rip != code_address || state.gpr[4] != original_rsp ||
		    state.host_call_stack_transaction.active ||
		    state.memory_fault_address != reinterpret_cast<uint64_t>(guest_stack + fail_slot * sizeof(uint64_t)) ||
		    state.memory_fault_size != sizeof(uint64_t) || state.memory_fault_write || state.gpr[0] != 0xfeedfacecafebeefULL ||
		    return_slot_after != return_slot_sentinel || g_host_call_count != 0)
		{
			return false;
		}
	}
	return pipeline != HostPipeline::FastBlock || Lumen::Cpu::X64FastBlockTestTargetInstructionCount() != 0;
}

bool TestHostCallStackOverflow(size_t page_size)
{
	Mapping code(page_size);
	Mapping stack(page_size);
	if (!code.IsValid() || !stack.IsValid()) return false;
	constexpr std::array<uint8_t, 5> program = {
	    0x41, 0xff, 0xd3, // CALL r11
	    0xff, 0xe3,       // JMP rbx
	};
	std::memcpy(code.Data(), program.data(), program.size());
	auto* guest_stack = stack.Data() + page_size / 2;
	const uint64_t return_slot_sentinel = 0x0f0e0d0c0b0a0908ULL;
	std::memcpy(guest_stack - sizeof(uint64_t), &return_slot_sentinel, sizeof(return_slot_sentinel));

	ConfigureHostPipeline(HostPipeline::Precise);
	g_override_guest_rsp  = true;
	g_guest_rsp_override  = UINT64_MAX - 3;
	g_host_call_count     = 0;
	g_host_argument_count = 0;
	Lumen::Cpu::X64State state {};
	const uint64_t code_address = reinterpret_cast<uint64_t>(code.Data());
	const uint64_t original_rsp = reinterpret_cast<uint64_t>(guest_stack);
	InitializeHostCallState(&state, code_address, original_rsp, HOST_FUNCTIONS[8]);
	const auto result    = Lumen::Cpu::X64Interpreter(TestHostCallBridge, Lumen::Loader::translate_guest_address).Run(&state, 8);
	g_override_guest_rsp = false;
	uint64_t return_slot_after = 0;
	std::memcpy(&return_slot_after, guest_stack - sizeof(uint64_t), sizeof(return_slot_after));
	return result == Lumen::Cpu::X64RunResult::MemoryFault && state.rip == code_address && state.gpr[4] == original_rsp &&
	       !state.host_call_stack_transaction.active && state.memory_fault_address == UINT64_MAX - 3 &&
	       state.memory_fault_size == sizeof(uint64_t) && !state.memory_fault_write && state.gpr[0] == 0xfeedfacecafebeefULL &&
	       return_slot_after == return_slot_sentinel && g_host_call_count == 0;
}

bool TestHostCallMetadataLookup()
{
	Lumen::Loader::SymbolDatabase symbols;
	for (uint32_t arity = 0; arity <= 8; arity++)
	{
		const auto metadata = symbols.HostCallGpMetadataFor(HOST_FUNCTIONS[arity]);
		if (metadata.arity != 8 || metadata.invoker != nullptr) return false;
	}
	RegisterTypedHostCalls(&symbols);
	constexpr std::array<Lumen::Loader::HostCallGpInvoker, 4> expected_invokers = {
	    HOST_CALL_9_METADATA.invoker,
	    HOST_CALL_10_METADATA.invoker,
	    HOST_CALL_11_METADATA.invoker,
	    HOST_CALL_12_METADATA.invoker,
	};
	for (uint32_t arity = 9; arity <= 12; arity++)
	{
		const auto metadata = symbols.HostCallGpMetadataFor(HOST_FUNCTIONS[arity]);
		if (metadata.arity != arity || metadata.invoker != expected_invokers[arity - 9]) return false;
	}
	auto below_range = HOST_CALL_9_METADATA;
	auto above_range = HOST_CALL_12_METADATA;
	below_range.arity = 7;
	above_range.arity = 13;
	symbols.SetHostCallGpMetadata(0x1234, below_range);
	symbols.SetHostCallGpMetadata(0x5678, above_range);
	symbols.SetHostCallGpMetadata(0x9abc, {9, nullptr});
	for (const uint64_t target: {0x1234ULL, 0x5678ULL, 0x9abcULL})
	{
		const auto metadata = symbols.HostCallGpMetadataFor(target);
		if (metadata.arity != 8 || metadata.invoker != nullptr) return false;
	}
	return true;
}

bool TestHostCallTargetClassification(size_t page_size)
{
	Mapping guest_code(page_size);
	if (!guest_code.IsValid()) return false;
	const uint64_t host_target  = HOST_FUNCTIONS[0];
	const uint64_t guest_target = reinterpret_cast<uint64_t>(guest_code.Data());
	const uint64_t dylib_target = reinterpret_cast<uint64_t>(&std::malloc);
	Dl_info        host_info {};
	Dl_info        dylib_info {};
	const void*    main_image = static_cast<const void*>(_dyld_get_image_header(0));
	if (dladdr(reinterpret_cast<const void*>(host_target), &host_info) == 0 || host_info.dli_fbase != main_image ||
	    dladdr(reinterpret_cast<const void*>(dylib_target), &dylib_info) == 0 || dylib_info.dli_fbase == nullptr ||
	    dylib_info.dli_fbase == main_image)
	{
		return false;
	}
	if (Lumen::Core::VirtualMemory::IsManagedAddress(host_target) ||
	    Lumen::Core::VirtualMemory::IsManagedAddress(dylib_target) ||
	    Lumen::Core::VirtualMemory::IsManagedAddress(0))
	{
		return false;
	}

	const bool guest_no_dladdr = RunWithFreshCache([&]
	{
		Lumen::Loader::ResetNativeHostDladdrCallCountForTest();
		for (uint32_t offset = 0; offset < 4096; offset++)
		{
			if (Lumen::Loader::IsNativeHostCallTarget(guest_target + offset)) return false;
		}
		return Lumen::Loader::NativeHostDladdrCallCountForTest() == 0;
	});
	const bool main_cached = RunWithFreshCache([&]
	{
		Lumen::Loader::ResetNativeHostDladdrCallCountForTest();
		if (!Lumen::Loader::IsNativeHostCallTarget(host_target) ||
		    Lumen::Loader::NativeHostDladdrCallCountForTest() != 1)
		{
			return false;
		}
		for (uint32_t iteration = 0; iteration < 4096; iteration++)
		{
			if (!Lumen::Loader::IsNativeHostCallTarget(host_target)) return false;
		}
		return Lumen::Loader::NativeHostDladdrCallCountForTest() == 1;
	});
	const bool zero_without_dladdr = RunWithFreshCache([&]
	{
		Lumen::Loader::ResetNativeHostDladdrCallCountForTest();
		return !Lumen::Loader::IsNativeHostCallTarget(0) &&
		       Lumen::Loader::NativeHostDladdrCallCountForTest() == 0;
	});
	const bool dylib_fallback = RunWithFreshCache([&]
	{
		Lumen::Loader::ResetNativeHostDladdrCallCountForTest();
		return Lumen::Loader::IsNativeHostCallTarget(dylib_target) &&
		       Lumen::Loader::NativeHostDladdrCallCountForTest() == 1;
	});
	if (!guest_no_dladdr || !main_cached || !zero_without_dladdr || !dylib_fallback ||
	    !Lumen::Core::VirtualMemory::IsManagedAddress(guest_target))
	{
		return false;
	}

	std::atomic<bool> repeated_ok {true};
	std::array<std::thread, 8> workers;
	for (auto& worker: workers)
	{
		worker = std::thread([&]
		{
			for (uint32_t iteration = 0; iteration < 4096 && repeated_ok.load(std::memory_order_relaxed); iteration++)
			{
				if (!Lumen::Loader::IsNativeHostCallTarget(host_target) ||
				    Lumen::Loader::IsNativeHostCallTarget(guest_target) ||
				    Lumen::Loader::IsNativeHostCallTarget(0) ||
				    !Lumen::Loader::IsNativeHostCallTarget(dylib_target))
				{
					repeated_ok.store(false, std::memory_order_relaxed);
				}
			}
		});
	}
	for (auto& worker: workers)
		worker.join();
	return repeated_ok.load(std::memory_order_relaxed);
}

bool TestAuthoritativeHostCallRegistry(size_t page_size, bool include_macro_fixture = true)
{
	using Classification = Lumen::Loader::HostCallRegistryClassification;
	if (!Lumen::Loader::AuthoritativeHostCallRegistryEnabled())
	{
		return RunWithFreshCache([&]
		{
			Lumen::Loader::SymbolDatabase symbols;
			if (!symbols.RegisterHostCallTarget(HOST_FUNCTIONS[0])) return false;
			symbols.SealHostCallRegistry();
			if (symbols.HostCallRegistrySealed() ||
			    symbols.HostCallTargetCount() != 0 ||
			    symbols.ClassifyHostCallTarget(HOST_FUNCTIONS[0]) !=
			        Classification::Unsealed)
			{
				return false;
			}

			g_host_symbols = &symbols;
			Lumen::Loader::ResetNativeHostDladdrCallCountForTest();
			const bool invoked = RunHostCallCase(
			    page_size, 0, HostPipeline::Precise, false,
			    ClassifiedHostCallBridge);
			g_host_symbols = nullptr;
			ResetHostPipelineEnvironment();
			const uint64_t dladdr_calls =
			    Lumen::Loader::NativeHostDladdrCallCountForTest();
			if (!invoked || dladdr_calls != 1)
			{
				std::fprintf(stderr,
				             "authoritative-host-call-registry disabled invoked=%s dladdr=%llu\n",
				             invoked ? "yes" : "no",
				             static_cast<unsigned long long>(dladdr_calls));
			}
			return invoked && dladdr_calls == 1;
		});
	}

	const bool unsealed_fallback = RunWithFreshCache([&]
	{
		Lumen::Loader::SymbolDatabase symbols;
		if (!symbols.RegisterHostCallTarget(HOST_FUNCTIONS[0]) ||
		    symbols.HostCallRegistrySealed() ||
		    symbols.HostCallTargetCount() != 1 ||
		    symbols.ClassifyHostCallTarget(HOST_FUNCTIONS[0]) !=
		        Classification::Unsealed)
		{
			return false;
		}

		g_host_symbols = &symbols;
		Lumen::Loader::ResetNativeHostDladdrCallCountForTest();
		const bool invoked = RunHostCallCase(
		    page_size, 0, HostPipeline::Precise, false,
		    ClassifiedHostCallBridge);
		g_host_symbols = nullptr;
		ResetHostPipelineEnvironment();
		const uint64_t dladdr_calls =
		    Lumen::Loader::NativeHostDladdrCallCountForTest();
		if (!invoked || dladdr_calls != 1)
		{
			std::fprintf(stderr,
			             "authoritative-host-call-registry unsealed invoked=%s dladdr=%llu\n",
			             invoked ? "yes" : "no",
			             static_cast<unsigned long long>(dladdr_calls));
		}
		return invoked && dladdr_calls == 1;
	});

	const bool sealed_exact_set = RunWithFreshCache([&]
	{
		Lumen::Loader::SymbolDatabase symbols;
		if (!symbols.RegisterHostCallTarget(HOST_FUNCTIONS[0]) ||
		    !symbols.RegisterHostCallTarget(HOST_FUNCTIONS[0]) ||
		    symbols.HostCallTargetCount() != 1)
		{
			return false;
		}
		symbols.SealHostCallRegistry();
		if (!symbols.HostCallRegistrySealed() ||
		    symbols.ClassifyHostCallTarget(HOST_FUNCTIONS[0]) !=
		        Classification::Host ||
		    symbols.ClassifyHostCallTarget(HOST_FUNCTIONS[1]) !=
		        Classification::Guest ||
		    symbols.ClassifyHostCallTarget(0) != Classification::Guest ||
		    symbols.RegisterHostCallTarget(HOST_FUNCTIONS[1]))
		{
			return false;
		}

		g_host_symbols = &symbols;
		Lumen::Loader::ResetNativeHostDladdrCallCountForTest();
		const bool hit = RunHostCallCase(
		    page_size, 0, HostPipeline::Precise, false,
		    ClassifiedHostCallBridge);
		g_host_symbols = nullptr;
		ResetHostPipelineEnvironment();
		Lumen::Cpu::X64State miss_state {};
		const bool miss = Lumen::Loader::RunHostCallForTest(
		    &symbols, HOST_FUNCTIONS[1], &miss_state);
		return hit && !miss && g_host_call_count == 1 &&
		       Lumen::Loader::NativeHostDladdrCallCountForTest() == 0;
	});

	const bool macro_registration = !include_macro_fixture || RunWithFreshCache([]
	{
		Lumen::Loader::SymbolDatabase std_symbols;
		if (!Lumen::Libs::Init(U"libBgft_1", &std_symbols) ||
		    std_symbols.HostCallRegistrySealed() ||
		    std_symbols.HostCallTargetCount() != 4)
		{
			return false;
		}
		const auto* std_record = std_symbols.FindByStdName(
		    Lumen::Core::String::FromUtf8("sceBgftServiceIntTerm"));
		if (std_record == nullptr ||
		    std_symbols.ClassifyHostCallTarget(std_record->vaddr) !=
		        Classification::Unsealed)
		{
			return false;
		}
		std_symbols.SealHostCallRegistry();
		if (std_symbols.ClassifyHostCallTarget(std_record->vaddr) !=
		    Classification::Host)
		{
			return false;
		}

		Lumen::Loader::SymbolDatabase func_symbols;
		if (!Lumen::Libs::Init(U"libPad_1", &func_symbols) ||
		    func_symbols.HostCallRegistrySealed() ||
		    func_symbols.HostCallTargetCount() != 10)
		{
			return false;
		}
		Lumen::Loader::SymbolResolve pad_symbol {};
		pad_symbol.name                 = "hv1luiJrqQM";
		pad_symbol.library              = "Pad";
		pad_symbol.library_version      = 1;
		pad_symbol.module               = "Pad";
		pad_symbol.module_version_major = 1;
		pad_symbol.module_version_minor = 1;
		pad_symbol.type                 = Lumen::Loader::SymbolType::Func;
		const auto* func_record = func_symbols.Find(pad_symbol);
		if (func_record == nullptr) return false;
		func_symbols.SealHostCallRegistry();
		return func_symbols.ClassifyHostCallTarget(func_record->vaddr) ==
		       Classification::Host;
	});

	const bool full_init_authoritative = RunWithFreshCache([]
	{
		Lumen::Loader::SymbolDatabase symbols;
		Lumen::Libs::InitAll(&symbols);
		if (!symbols.HostCallRegistrySealed() ||
		    symbols.HostCallTargetCount() <= 13)
		{
			std::fprintf(stderr,
			             "authoritative-host-call-registry full-init sealed=%s count=%u\n",
			             symbols.HostCallRegistrySealed() ? "yes" : "no",
			             symbols.HostCallTargetCount());
			return false;
		}
		const auto* std_record = symbols.FindByStdName(
		    Lumen::Core::String::FromUtf8("sceBgftServiceIntTerm"));
		Lumen::Loader::SymbolResolve object_symbol {};
		object_symbol.name                 = "ZT4ODD2Ts9o";
		object_symbol.library              = "LibcInternal";
		object_symbol.library_version      = 1;
		object_symbol.module               = "LibcInternal";
		object_symbol.module_version_major = 1;
		object_symbol.module_version_minor = 1;
		object_symbol.type                 = Lumen::Loader::SymbolType::Object;
		const auto* object_record = symbols.Find(object_symbol);
		if (std_record == nullptr || object_record == nullptr ||
		    symbols.ClassifyHostCallTarget(std_record->vaddr) !=
		        Classification::Host ||
		    symbols.ClassifyHostCallTarget(object_record->vaddr) !=
		        Classification::Guest ||
		    symbols.ClassifyHostCallTarget(HOST_FUNCTIONS[0]) !=
		        Classification::Guest)
		{
			std::fprintf(stderr,
			             "authoritative-host-call-registry full-init std=%p object=%p std-class=%u object-class=%u local-class=%u\n",
			             static_cast<const void*>(std_record),
			             static_cast<const void*>(object_record),
			             std_record == nullptr ? 99u : static_cast<unsigned>(
			                 symbols.ClassifyHostCallTarget(std_record->vaddr)),
			             object_record == nullptr ? 99u : static_cast<unsigned>(
			                 symbols.ClassifyHostCallTarget(object_record->vaddr)),
			             static_cast<unsigned>(symbols.ClassifyHostCallTarget(
			                 HOST_FUNCTIONS[0])));
			return false;
		}

		Lumen::Loader::ResetNativeHostDladdrCallCountForTest();
		Lumen::Cpu::X64State guest_state {};
		const bool unregistered = Lumen::Loader::RunHostCallForTest(
		    &symbols, HOST_FUNCTIONS[0], &guest_state);
		if (unregistered ||
		    Lumen::Loader::NativeHostDladdrCallCountForTest() != 0)
		{
			std::fprintf(stderr,
			             "authoritative-host-call-registry full-init miss=%s dladdr=%llu\n",
			             unregistered ? "handled" : "rejected",
			             static_cast<unsigned long long>(
			                 Lumen::Loader::NativeHostDladdrCallCountForTest()));
			return false;
		}

		// The authoritative shortcut is deliberately local to run_host_call.
		// Other native-pointer users retain the existing dladdr classifier.
		Lumen::Loader::ResetNativeHostDladdrCallCountForTest();
		const bool native =
		    Lumen::Loader::IsNativeHostCallTarget(HOST_FUNCTIONS[0]);
		const uint64_t dladdr_calls =
		    Lumen::Loader::NativeHostDladdrCallCountForTest();
		if (!native || dladdr_calls != 1)
		{
			std::fprintf(stderr,
			             "authoritative-host-call-registry fallback native=%s dladdr=%llu\n",
			             native ? "yes" : "no",
			             static_cast<unsigned long long>(dladdr_calls));
		}
		return native && dladdr_calls == 1;
	});

	if (!unsealed_fallback || !sealed_exact_set || !macro_registration ||
	    !full_init_authoritative)
	{
		std::fprintf(stderr,
		             "authoritative-host-call-registry unsealed=%s sealed=%s macros=%s full-init=%s\n",
		             unsealed_fallback ? "ok" : "fail",
		             sealed_exact_set ? "ok" : "fail",
		             !include_macro_fixture ? "skip" : macro_registration ? "ok" : "fail",
		             full_init_authoritative ? "ok" : "fail");
		return false;
	}
	return true;
}

bool TestHostCallMarshalling(size_t page_size)
{
	Lumen::Loader::SymbolDatabase symbols;
	RegisterTypedHostCalls(&symbols);
	g_host_symbols = &symbols;
	bool ok = true;
	for (uint32_t arity = 0; arity <= 8 && ok; arity++)
	{
		ok = RunHostCallCase(page_size, arity, HostPipeline::Precise);
	}
	for (uint32_t arity = 9; arity <= 12 && ok; arity++)
	{
		ok = RunHostCallCase(page_size, arity, HostPipeline::Precise);
	}
	ok = ok && RunHostCallCase(page_size, 10, HostPipeline::FastBlock) && RunHostCallCase(page_size, 12, HostPipeline::FastBlock);
	for (uint32_t fail_slot = 0; fail_slot < 6 && ok; fail_slot++)
	{
		ok = RunHostCallFaultCase(page_size, fail_slot, HostPipeline::Precise);
	}
	ok = ok && RunHostCallFaultCase(page_size, 0, HostPipeline::FastBlock) && RunHostCallFaultCase(page_size, 5, HostPipeline::FastBlock) &&
	     RunHostCallFaultCase(page_size, 5, HostPipeline::Precise, HostIndirectCall::Memory) &&
	     RunHostCallFaultCase(page_size, 5, HostPipeline::FastBlock, HostIndirectCall::Memory) &&
	     RunDirectHostCallFaultCase(page_size, 0, HostPipeline::Precise) &&
	     RunDirectHostCallFaultCase(page_size, 5, HostPipeline::Precise) &&
	     RunDirectHostCallFaultCase(page_size, 0, HostPipeline::FastBlock) &&
	     RunDirectHostCallFaultCase(page_size, 5, HostPipeline::FastBlock) &&
	     RunHostJumpFaultCase(page_size, HostPipeline::Precise) && RunHostJumpFaultCase(page_size, HostPipeline::FastBlock) &&
	     TestHostCallStackOverflow(page_size);
	g_host_symbols = nullptr;
	ResetHostPipelineEnvironment();
	return ok;
}

} // namespace
#endif

int main(int argc, char** argv)
{
#if defined(__APPLE__) && defined(__aarch64__)
#if defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE_TEST)
	if (const char* oracle =
	        std::getenv("LUMEN_A64_DYNAREC_HOST_PC_PROFILE_ORACLE");
	    oracle != nullptr)
	{
		Lumen::Cpu::X64PrepareA64DynarecHostPcProfile();
		Lumen::Cpu::X64BeginA64DynarecHostPcProfile();
		if (std::strcmp(oracle, "ir-storage-close-race") == 0)
		{
			const bool valid = Lumen::Cpu::
			    X64ValidateA64DynarecIrStorageProfileCloseRaceForTest();
			std::printf("runtime-linker-ir-storage-close-race=%s\n",
			            valid ? "ok" : "fail");
			return valid ? 0 : 1;
		}
		if (std::strcmp(oracle, "runtime-linker") != 0) return 1;
		const bool claim =
		    Lumen::Cpu::X64ClaimA64DynarecHostPcProfileFirstSubmit();
		if (claim)
		{
			Lumen::Cpu::X64FinishA64DynarecHostPcProfileFirstSubmit();
		}
		const bool complete =
		    Lumen::Cpu::X64ValidateA64DynarecHostPcProfileSnapshotForTest();
		const bool exactly_once =
		    !Lumen::Cpu::X64ClaimA64DynarecHostPcProfileFirstSubmit();
		std::printf(
		    "runtime-linker-host-pc-boundary claim=%s complete=%s"
		    " exactly-once=%s\n",
		    claim ? "ok" : "fail", complete ? "ok" : "fail",
		    exactly_once ? "ok" : "fail");
		return claim && complete && exactly_once ? 0 : 1;
	}
#endif
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Lumen::Core::VirtualMemory::Init();
	const long host_page_size = sysconf(_SC_PAGESIZE);
	if (host_page_size <= 0) return 1;
	const size_t page_size = static_cast<size_t>(host_page_size);
	if (argc == 2 && std::strcmp(argv[1], "--fex-unresolved-arguments") == 0)
	{
		const bool valid = TestFexUnresolvedArgumentRegions(page_size);
		std::printf("fex-unresolved-argument-regions=%s\n", valid ? "ok" : "fail");
		return valid ? 0 : 1;
	}
	if (argc == 2 && std::strcmp(argv[1], "--host-call-dispatch") == 0)
	{
		// Dispatch validation does not invoke the export-count macro fixture.
		// The separate FEX scalar gate covers SIMD, owned stack and fault ABI.
		const bool metadata = TestHostCallMetadataLookup();
		const bool classification = TestHostCallTargetClassification(page_size);
		const bool registry = TestAuthoritativeHostCallRegistry(page_size, false);
		std::printf("host-call-dispatch metadata=%s classification=%s registry=%s macros=excluded\n",
		            metadata ? "ok" : "fail", classification ? "ok" : "fail", registry ? "ok" : "fail");
		return metadata && classification && registry ? 0 : 1;
	}
	if (argc != 1) return 1;

	const bool basic              = RunWithFreshCache([&] { return TestBasicCacheAndPermissions(page_size); });
	const bool unresolved_relocation_policy = TestUnresolvedRelocationPolicy();
	const bool module_layout = TestModuleLayoutContracts();
	const bool protect_invalidate = RunWithFreshCache([&] { return TestProtectInvalidatesPositiveWriteCache(page_size); });
	const bool mixed_page_policy = RunWithFreshCache([&] { return TestMixedGuestPagePolicy(page_size); });
	const bool guest_page_isolation = RunWithFreshCache([&] { return TestGuestPageProtectionIsolation(page_size); });
	const bool guest_region_export_recency = RunWithFreshCache([&] { return TestGuestRegionExportRecency(page_size); });
	const bool guest_region_export_recency_wrap =
	    RunWithFreshCache([&] { return TestGuestRegionExportRecencyWrap(page_size); });
	const bool guest_page_cross_thread = TestGuestPageCrossThreadInvalidation(page_size);
	const bool free_invalidate    = RunWithFreshCache([&] { return TestFreeInvalidatesPositiveCache(page_size); });
	const bool cross_read         = RunWithFreshCache([&] { return TestCrossRegionRead(page_size); });
	const bool cross_write        = RunWithFreshCache([&] { return TestCrossRegionWriteDenied(page_size); });
	const bool gap                = RunWithFreshCache([&] { return TestGapDenied(page_size); });
	const bool host_metadata_lookup = TestHostCallMetadataLookup();
	const bool host_target_classification = TestHostCallTargetClassification(page_size);
	const bool host_call_registry = TestAuthoritativeHostCallRegistry(page_size);
	const bool android_host_target_cache_separation =
	    Lumen::Loader::ValidateAndroidNativeHostTargetCacheSeparationForTest();
	const bool host_marshalling     = TestHostCallMarshalling(page_size);
	const bool guest_root_host_call_frame = TestGuestRootStackHostCallFrame(page_size);
#if defined(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY)
	const bool startup_host_call_liveness = TestStartupHostCallLiveness(page_size);
	constexpr bool startup_host_call_liveness_enabled = true;
#else
	const bool startup_host_call_liveness = true;
	constexpr bool startup_host_call_liveness_enabled = false;
#endif
	#if defined(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
		const bool phase6af_hle_unpaired_ordinary =
		    TestPhase6AFHostCallLivenessCase(page_size, false);
		const bool phase6af_hle_paired_native_simulated =
		    TestPhase6AFHostCallLivenessCase(page_size, true);
	constexpr bool phase6af_hle_liveness_enabled = true;
#else
	const bool phase6af_hle_unpaired_ordinary = true;
	const bool phase6af_hle_paired_native_simulated = true;
	constexpr bool phase6af_hle_liveness_enabled = false;
#endif
	const bool guest_page_precise_fault = RunGuestPageReadFaultCase(page_size, HostPipeline::Precise);
	const bool guest_page_fast_fault = RunGuestPageReadFaultCase(page_size, HostPipeline::FastBlock);
	const bool guest_page_a64_available = Lumen::Cpu::X64A64DynarecAvailableForTest();
	const bool guest_page_a64_fault = !guest_page_a64_available ||
	                                  TestGuestPageA64CrossThreadInvalidation(page_size);

	std::printf("runtime-linker-address page=%zu vmGranule=%zu mixed4k=%s basic=%s unresolved-relocation-policy=%s module-layout=%s protect-invalidate=%s guest-page-isolation=%s "
	            "guest-region-export-recency=%s guest-region-export-recency-wrap=%s "
	            "guest-page-cross-thread=%s "
	            "free-invalidate=%s cross-read=%s "
	            "cross-write=%s gap=%s host-metadata-lookup=%s host-target-classification=%s host-call-registry=%s "
	            "android-host-target-cache-separation=%s host-marshalling=%s "
	            "guest-root-host-call-frame=%s "
	            "startup-host-call-liveness=%s "
	            "phase6af-hle-unpaired-ordinary=%s phase6af-hle-paired-native-simulated=%s "
	            "guest-page-precise-fault=%s guest-page-fast-fault=%s guest-page-a64-fault=%s\n",
	            page_size, PermissionGranule(page_size), !mixed_page_policy ? "fail" :
	                (Lumen::Core::VirtualMemory::RequiresUniformHostPageDataProtection() ? "denied" : "checked"),
	            basic ? "ok" : "fail", unresolved_relocation_policy ? "ok" : "fail",
	            module_layout ? "ok" : "fail",
	            protect_invalidate ? "ok" : "fail",
	            guest_page_isolation ? "ok" : "fail", guest_region_export_recency ? "ok" : "fail",
	            guest_region_export_recency_wrap ? "ok" : "fail",
	            guest_page_cross_thread ? "ok" : "fail",
	            free_invalidate ? "ok" : "fail", cross_read ? "ok" : "fail",
	            cross_write ? "ok" : "fail", gap ? "ok" : "fail", host_metadata_lookup ? "ok" : "fail",
	            host_target_classification ? "ok" : "fail",
	            host_call_registry ? "ok" : "fail",
	            android_host_target_cache_separation ? "ok" : "fail",
	            host_marshalling ? "ok" : "fail",
	            guest_root_host_call_frame ? "ok" : "fail",
	            !startup_host_call_liveness_enabled
	                ? "skip"
	                : (startup_host_call_liveness ? "ok" : "fail"),
	            !phase6af_hle_liveness_enabled
	                ? "skip"
	                : (phase6af_hle_unpaired_ordinary ? "ok" : "fail"),
	            !phase6af_hle_liveness_enabled
	                ? "skip"
	                : (phase6af_hle_paired_native_simulated ? "ok" : "fail"),
	            guest_page_precise_fault ? "ok" : "fail", guest_page_fast_fault ? "ok" : "fail",
	            !guest_page_a64_available ? "skip" : (guest_page_a64_fault ? "ok" : "fail"));
	return basic && unresolved_relocation_policy && module_layout && protect_invalidate && mixed_page_policy && guest_page_isolation && guest_region_export_recency &&
	           guest_region_export_recency_wrap &&
	           guest_page_cross_thread && free_invalidate && cross_read && cross_write && gap && host_metadata_lookup &&
	           host_target_classification && host_call_registry && android_host_target_cache_separation &&
	           host_marshalling && guest_root_host_call_frame &&
	           startup_host_call_liveness &&
	           phase6af_hle_unpaired_ordinary &&
	           phase6af_hle_paired_native_simulated &&
	           guest_page_precise_fault && guest_page_fast_fault &&
	           guest_page_a64_fault
	           ? 0
	           : 1;
#else
	(void)argc;
	(void)argv;
	std::puts("runtime-linker-address=skipped (requires Apple ARM64)");
	return 0;
#endif
}
