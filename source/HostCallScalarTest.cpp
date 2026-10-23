#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Singleton.h"
#include "Lumen/Core/VirtualMemory.h"
#include "Emulator/Config.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Loader/HostCallScalar.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Log.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Printf.h"
#include "Emulator/Libs/VaContext.h"
#include "AndroidFexCoreRuntime.h"
#include "GuestCpuRuntime.h"
#include "CpuX64.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <mach-o/dyld.h>
#include <mutex>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <type_traits>
#include <vector>
#include <unistd.h>

extern char** environ;

namespace Lumen::Libs::LibcInternalExt {
void ResetSessionState();
}
namespace Lumen::Libs::LibcInternal {
void ResetSessionState();
}
namespace Lumen::Libs::LibKernel {
void ResetSessionState();
void SetProgName(const Lumen::Core::String& name);
}

namespace {
namespace Loader = Lumen::Loader;
namespace Memory = Lumen::Core::VirtualMemory;
using Registers = std::array<std::array<uint8_t, 32>, 16>;
uint64_t g_snprintf_target = 0;
uint64_t g_malloc_replace_target = 0;
uint64_t g_new_replace_target = 0;
uint64_t g_heap_trace_target = 0;
uint64_t g_errno_target = 0;
uint64_t g_tls_target = 0;
uint64_t g_pthread_self_target = 0;
uint64_t g_pthread_create_target = 0;
uint64_t g_pthread_join_target = 0;
uint64_t g_mutex_init_target = 0;
uint64_t g_prepared_tls_entry = 0;
bool g_prepared_tls_nested_vm_unchanged = false;
uint64_t g_main_tcb = 0;
constexpr uint64_t RETURN_TOKEN = 0x8697a5b4c3d2e1f0ULL;
#if defined(__aarch64__) && defined(LUMEN_FEXCORE_RUNTIME) && \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
constexpr bool MANAGED_PRIMARY_STACK = true;
#else
constexpr bool MANAGED_PRIMARY_STACK = false;
#endif
constexpr std::array<uint64_t, 8> GP_VALUES {
    0x1133557799bbddffULL, 0x2233445566778899ULL, 0xdeadbeef01234567ULL, 0x76543210fedcba98ULL,
    0x1020304050607080ULL, 0xabcdef9876543210ULL, 0xfedcba1357246809ULL, 0x8811223344556677ULL,
};
constexpr std::array<uint64_t, 9> FP_VALUES {
    0x3ff4000000000000ULL, 0x3fc00000, 0xc002000000000000ULL, 0x40700000,
    0x4013000000000000ULL, 0xc0b00000, 0x401a000000000000ULL, 0x40f00000,
    0x4021400000000000ULL,
};

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "HostCallScalarTest: %s\n", message);
	return condition;
}

class Watchdog
{
public:
	explicit Watchdog(unsigned seconds): m_thread([this, seconds] {
		std::unique_lock<std::mutex> lock(m_mutex);
		if (!m_changed.wait_for(lock, std::chrono::seconds(seconds), [this] { return m_done; }))
		{
			std::fputs("HostCallScalarTest: timed out\n", stderr);
			std::_Exit(2);
		}
	}) {}
	~Watchdog()
	{
		{ std::lock_guard<std::mutex> lock(m_mutex); m_done = true; }
		m_changed.notify_one(); m_thread.join();
	}
private:
	std::mutex m_mutex;
	std::condition_variable m_changed;
	bool m_done = false;
	std::thread m_thread;
};

class Mapping
{
public:
	explicit Mapping(size_t size): m_size(size), m_address(Memory::Alloc(0, size, Memory::Mode::ReadWrite)) {}
	~Mapping() { if (m_address != 0) Memory::Free(m_address); }
	Mapping(const Mapping&) = delete;
	Mapping& operator=(const Mapping&) = delete;
	uint64_t Address(size_t offset = 0) const { return m_address + offset; }
	uint8_t* Data(size_t offset = 0) const { return reinterpret_cast<uint8_t*>(Address(offset)); }
	bool Valid() const { return m_address != 0; }
	bool Protect(size_t offset, size_t size, Memory::Mode mode)
	{
		return Valid() && offset <= m_size && size <= m_size - offset && Memory::Protect(Address(offset), size, mode);
	}
	size_t Size() const { return m_size; }
private:
	size_t m_size;
	uint64_t m_address;
};

// Independent original x86-64 fixtures, assembled here from simple moves,
// calls and returns. No FEX structure, external code or ROM is used by them.
class GuestCode
{
public:
	void Bytes(std::initializer_list<uint8_t> bytes)
	{
		for (uint8_t byte: bytes)
		{
			if (m_size == m_bytes.size()) std::_Exit(3);
			m_bytes[m_size++] = byte;
		}
	}
	void Word(uint64_t value)
	{
		for (unsigned shift = 0; shift != 64; shift += 8) Bytes({static_cast<uint8_t>(value >> shift)});
	}
	void Mov(unsigned reg, uint64_t value)
	{
		Bytes({static_cast<uint8_t>(reg < 8 ? 0x48 : 0x49), static_cast<uint8_t>(0xb8 + (reg & 7))});
		Word(value);
	}
	void Stack(unsigned bytes, bool release = false)
	{
		Bytes({0x48, 0x83, static_cast<uint8_t>(release ? 0xc4 : 0xec), static_cast<uint8_t>(bytes)});
	}
	void Spill(unsigned offset, uint64_t value)
	{
		Mov(0, value);
		Bytes({0x48, 0x89, 0x44, 0x24, static_cast<uint8_t>(offset)}); // mov [rsp+offset], rax
	}
	void Vectors(uint64_t base, bool store)
	{
		for (unsigned reg = 0; reg != 16; ++reg)
		{
			Mov(0, base + reg * 32);
			// vmovdqu ymmN, [rax] or [rax], ymmN. VEX.R selects registers8..15;
			// L=1 deliberately carries the upper AVX half through the host call.
			Bytes({0xc5, static_cast<uint8_t>(reg < 8 ? 0xfe : 0x7e),
			       static_cast<uint8_t>(store ? 0x7f : 0x6f), static_cast<uint8_t>((reg & 7) << 3)});
		}
	}
	void GpArguments()
	{
		constexpr unsigned regs[] = {7, 6, 2, 1, 8, 9};
		for (unsigned index = 0; index != 6; ++index) Mov(regs[index], GP_VALUES[index]);
	}
	void Call(uint64_t target) { Mov(0, target); Bytes({0xff, 0xd0}); }
	void CallVariadic(uint64_t target, unsigned fp_count)
	{
		Mov(11, target); Mov(0, fp_count); Bytes({0x41, 0xff, 0xd3});
	}
	void SaveResult(uint64_t destination)
	{
		Mov(10, destination); Bytes({0x49, 0x89, 0x02}); // mov [r10], rax
	}
	void LoadResult(uint64_t source) { Mov(0, source); Bytes({0x48, 0x8b, 0x00}); }
	void Return() { Bytes({0xc3}); }
	bool Publish(Mapping& mapping) const
	{
		if (!mapping.Valid() || m_size > mapping.Size()) return false;
		std::memcpy(mapping.Data(), m_bytes.data(), m_size);
		return mapping.Protect(0, mapping.Size(), Memory::Mode::ExecuteRead) &&
		       Memory::FlushInstructionCache(mapping.Address(), m_size) &&
		       !Loader::IsNativeHostCallTarget(mapping.Address());
	}
private:
	std::array<uint8_t, 2048> m_bytes {};
	size_t m_size = 0;
};

Registers Pattern()
{
	Registers result {};
	for (size_t reg = 0; reg != result.size(); ++reg)
		for (size_t byte = 0; byte != result[reg].size(); ++byte)
			result[reg][byte] = static_cast<uint8_t>(reg * 31 + byte * 7 + 0x53);
	return result;
}

template <typename T> uint64_t Bits(T value)
{
	static_assert(sizeof(T) == 4 || sizeof(T) == 8);
	uint64_t bits = 0;
	std::memcpy(&bits, &value, sizeof(value));
	return bits;
}

void PutBits(Registers* registers, unsigned reg, uint64_t bits, size_t bytes)
{
	for (size_t i = 0; i != bytes; ++i) (*registers)[reg][i] = static_cast<uint8_t>(bits >> (i * 8));
}

unsigned g_calls = 0;
uint64_t g_fp_seen = 0;
uint64_t g_pointer_seen = 0;
std::array<uint64_t, 8> g_gp_seen {};
std::array<uint64_t, 9> g_mixed_fp_seen {};
bool g_bad_spill = false;
uint64_t g_nested_entry = 0;
std::array<uint64_t, 4> g_active_root_stacks {};
unsigned g_nested_depth = 0;
bool g_nested_stack_valid = false;
bool g_owned_stack_read_test = false;
bool g_owned_stack_read_enabled = false;
bool g_owned_stack_read_valid = true;

bool CheckOwnedStackRead(uint64_t address, uint64_t size, bool owned)
{
	std::array<uint8_t, 16> output;
	output.fill(0xa7);
	auto expected = output;
	const bool permitted = owned && g_owned_stack_read_enabled;
	if (permitted) std::memcpy(expected.data(), reinterpret_cast<const void*>(address), size);
	const auto result = LumenMacOSFexCoreTryReadOwnedStackForHostCallV1(address, output.data(), size);
	return Check(result == (permitted ? 1U : 0U) && output == expected,
	             "owned stack read result or untouched output was wrong");
}

bool RootStackStorage(uint64_t address, bool managed)
{
	Memory::ManagedRegion region {};
	Memory::GuestDataLease lease;
	const auto state = Memory::QueryManagedRegion(address, &region);
	return (managed ? state == Memory::ManagedRegionState::Visible && region.mode == Memory::Mode::ReadWrite
	                : state == Memory::ManagedRegionState::Unmanaged) &&
	       Memory::TryAcquireGuestDataLease(address, sizeof(uint64_t), Memory::Mode::ReadWrite, &lease);
}

uint64_t LUMEN_SYSV_ABI NestedRoot(uint64_t address)
{
	++g_calls;
	const unsigned depth = g_nested_depth++;
	bool valid = depth < g_active_root_stacks.size() && RootStackStorage(address, MANAGED_PRIMARY_STACK && depth == 0);
	for (unsigned index = 0; index < depth; ++index)
		valid = valid && g_active_root_stacks[index] != address;
	if (!valid) { --g_nested_depth; g_nested_stack_valid = false; return 0; }
	g_active_root_stacks[depth] = address;
	if (g_owned_stack_read_test)
	{
		for (unsigned index = 0; index <= depth; ++index)
			g_owned_stack_read_valid = CheckOwnedStackRead(g_active_root_stacks[index] + 1, 8, true) && g_owned_stack_read_valid;
		const uint64_t foreign = RETURN_TOKEN;
		g_owned_stack_read_valid = CheckOwnedStackRead(reinterpret_cast<uint64_t>(&foreign), 8, false) && g_owned_stack_read_valid;
		g_owned_stack_read_valid = CheckOwnedStackRead(UINT64_MAX - 3, 8, false) && g_owned_stack_read_valid;
		g_owned_stack_read_valid = CheckOwnedStackRead(address, 0, false) && g_owned_stack_read_valid;
	}
	const uint64_t result = depth + 1 < g_active_root_stacks.size()
	    ? Loader::RunGuestX64(g_nested_entry, g_active_root_stacks[0]) : RETURN_TOKEN;
	g_nested_stack_valid = g_nested_stack_valid && RootStackStorage(address, MANAGED_PRIMARY_STACK && depth == 0);
	uint64_t increments = 0;
	std::memcpy(&increments, reinterpret_cast<const void*>(address + 1), sizeof(increments));
	g_nested_stack_valid = g_nested_stack_valid && increments == (depth == 0 ? 3U : 0U);
	g_active_root_stacks[depth] = 0;
	--g_nested_depth;
	return result;
}

float LUMEN_SYSV_ABI FloatExchange(float value)
{
	++g_calls; g_fp_seen = Bits(value);
	const uint32_t result_bits = static_cast<uint32_t>(g_fp_seen) ^ 0x80000000U;
	float result;
	std::memcpy(&result, &result_bits, sizeof(result));
	return result;
}
double LUMEN_SYSV_ABI DoubleExchange(double value)
{
	++g_calls; g_fp_seen = Bits(value);
	const uint64_t result_bits = g_fp_seen ^ 0x8000000000000000ULL;
	double result;
	std::memcpy(&result, &result_bits, sizeof(result));
	return result;
}
void LUMEN_SYSV_ABI VoidCall() { ++g_calls; }
uint64_t LUMEN_SYSV_ABI ScalarGp(uint64_t value) { ++g_calls; g_gp_seen[0] = value; return RETURN_TOKEN; }
uint64_t LUMEN_SYSV_ABI ScalarGpSix(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	++g_calls;
	g_gp_seen = {a0, a1, a2, a3, a4, a5, 0, 0};
	return RETURN_TOKEN;
}

float LUMEN_SYSV_ABI FloatFromGp(uint64_t value)
{
	++g_calls; g_gp_seen[0] = value;
	const auto bits = static_cast<uint32_t>(value);
	float result;
	std::memcpy(&result, &bits, sizeof(result));
	return result;
}

double LUMEN_SYSV_ABI DoubleFromGp(uint64_t value)
{
	++g_calls; g_gp_seen[0] = value;
	double result;
	std::memcpy(&result, &value, sizeof(result));
	return result;
}

int32_t LUMEN_SYSV_ABI PointerFloat(void* destination, float value)
{
	++g_calls;
	g_pointer_seen = reinterpret_cast<uint64_t>(destination);
	g_fp_seen = Bits(value);
	std::memcpy(destination, &value, sizeof(value));
	return -123456789;
}

uint64_t LUMEN_SYSV_ABI Mixed(uint64_t a0, double f0, float f1, uint64_t a1, double f2,
	                          uint64_t a2, float f3, uint64_t a3, double f4, uint64_t a4,
	                          float f5, uint64_t a5, double f6, float f7, double f8, uint64_t a6)
{
	if (g_bad_spill)
	{
		std::fputs("HostCallScalarTest: bad-spill callback invoked\n", stderr);
		std::_Exit(3);
	}
	++g_calls;
	g_gp_seen = {a0, a1, a2, a3, a4, a5, a6, 0};
	g_mixed_fp_seen = {Bits(f0), Bits(f1), Bits(f2), Bits(f3), Bits(f4), Bits(f5), Bits(f6), Bits(f7), Bits(f8)};
	return RETURN_TOKEN;
}

uint64_t LUMEN_SYSV_ABI LegacyGp(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
	                             uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
	++g_calls;
	g_gp_seen = {a0, a1, a2, a3, a4, a5, a6, a7};
	return RETURN_TOKEN;
}

struct UnsupportedAggregate { uint64_t first; uint64_t second; };
static_assert(!Loader::HostCallScalarDetail::SignatureSupported(static_cast<void (*)(int, ...)>(nullptr)));
static_assert(!Loader::HostCallScalarDetail::SignatureSupported(static_cast<void (*)(UnsupportedAggregate)>(nullptr)));
static_assert(Loader::MakeHostCallScalarMetadata<&Mixed>().arity == 16);

uint64_t LUMEN_SYSV_ABI PreparedTlsNestedRoot()
{
	const auto generation = Memory::GetMutationGeneration();
	const auto result = Loader::RunGuestX64(g_prepared_tls_entry);
	g_prepared_tls_nested_vm_unchanged = Memory::GetMutationGeneration() == generation;
	return result;
}

constexpr uint64_t FRESH_CALLBACK_ARGUMENT = 0x66726573685f6172ULL;
constexpr uint64_t FRESH_CALLBACK_ENTERED = 0x66726573685f656eULL;
constexpr uint64_t FRESH_CALLBACK_OBJECT = 0x66726573685f6f62ULL;
constexpr uint64_t FRESH_CALLBACK_RESULT = 0x66726573685f7265ULL;
struct FreshCallbackFixture
{
	uint64_t entry = 0;
	uint64_t object = 0;
	uint64_t buffer = 0;
	unsigned outer_calls = 0;
	unsigned release_calls = 0;
	bool arguments_valid = true;
	bool borrow_rejected = false;
	bool called = false;
	bool buffer_freed = false;
};
FreshCallbackFixture g_fresh_callback;

uint64_t LUMEN_SYSV_ABI FreshCallbackRelease(uint64_t object, uint64_t buffer, uint64_t argument)
{
	auto& fixture = g_fresh_callback;
	++fixture.release_calls;
	std::array<uint64_t, 2> observed {};
	fixture.arguments_valid = fixture.arguments_valid && object == fixture.object && buffer == fixture.buffer &&
	    argument == FRESH_CALLBACK_ARGUMENT && Loader::ReadGuestMemory(object, observed.data(), sizeof(observed)) &&
	    observed == std::array<uint64_t, 2> {FRESH_CALLBACK_OBJECT, FRESH_CALLBACK_ENTERED};
	if (!fixture.arguments_valid) return 0;
	// This real structural mutation must be admitted without the detached
	// parent's HLE reader. The mapping is independent of both guest stacks.
	fixture.buffer_freed = Memory::Free(buffer);
	return fixture.buffer_freed ? FRESH_CALLBACK_RESULT : 0;
}

uint64_t LUMEN_SYSV_ABI FreshCallbackRoot(uint64_t object, uint64_t buffer, uint64_t argument)
{
	auto& fixture = g_fresh_callback;
	++fixture.outer_calls;
	fixture.arguments_valid = fixture.arguments_valid && object == fixture.object && buffer == fixture.buffer &&
	    argument == FRESH_CALLBACK_ARGUMENT;
	if (!fixture.arguments_valid) return 0;
	{
		Memory::GuestDataLease borrow;
		if (!Memory::TryAcquireGuestDataLease(object, 2 * sizeof(uint64_t), Memory::Mode::Read, &borrow)) return 0;
		uint64_t rejected_result = RETURN_TOKEN;
		const bool called = Loader::TryRunGuestX64WithoutGuestBorrow(
		    fixture.entry, object, buffer, argument, &rejected_result);
		std::array<uint64_t, 2> observed {};
		std::memcpy(observed.data(), reinterpret_cast<const void*>(object), sizeof(observed));
		Memory::ManagedRegion region {};
		// The guest entry witness also detects an ordinary RunGuestX64 fallback:
		// refusal must precede every child instruction and the release HLE.
		fixture.borrow_rejected = !called && fixture.release_calls == 0 &&
		    observed == std::array<uint64_t, 2> {FRESH_CALLBACK_OBJECT, 0} &&
		    Memory::QueryManagedRegion(buffer, &region) == Memory::ManagedRegionState::Visible;
	}
	if (!fixture.borrow_rejected) return 0;
	uint64_t result = 0;
	fixture.called = Loader::TryRunGuestX64WithoutGuestBorrow(fixture.entry, object, buffer, argument, &result);
	return fixture.called ? result : 0;
}

template <auto Function> bool Register(Loader::SymbolDatabase* symbols)
{
	const uint64_t target = reinterpret_cast<uint64_t>(Function);
	return symbols->RegisterHostCallTarget(target) &&
	       symbols->SetHostCallScalarMetadata(target, Loader::MakeHostCallScalarMetadata<Function>());
}

bool Initialize(Loader::RuntimeLinker* linker)
{
	auto* symbols = linker->Symbols();
	if (!Lumen::Libs::Init(U"libkernel_1", symbols)) return false;
	for (const auto& entry: {
	         std::pair {U"py6L8jiVAN8", &g_malloc_replace_target},
	         std::pair {U"bnZxYgAFeA0", &g_new_replace_target},
	         std::pair {U"9BcDykPmo1I", &g_errno_target},
	         std::pair {U"vNe1w4diLCs", &g_tls_target},
	         std::pair {U"aI+OeCz8xrQ", &g_pthread_self_target},
	         std::pair {U"6UgtwV+0zb4", &g_pthread_create_target},
	         std::pair {U"onNY9Byn-W8", &g_pthread_join_target},
	         std::pair {U"cmo1RIYva9o", &g_mutex_init_target}})
	{
		const Loader::SymbolResolve symbol {entry.first, U"libkernel", 1, U"libkernel", 1, 1, Loader::SymbolType::Func};
		const auto* record = symbols->Find(symbol);
		if (!Check(record != nullptr, "sanitizer replacement getter import missing")) return false;
		*entry.second = record->vaddr;
	}
	if (!Lumen::Libs::Init(U"libc_internal_1", symbols)) return false;
	const Loader::SymbolResolve heap_trace_symbol {
	    U"NWtTN10cJzE", U"LibcInternalExt", 1, U"LibcInternal", 1, 1, Loader::SymbolType::Func};
	const auto* heap_trace_record = symbols->Find(heap_trace_symbol);
	if (!Check(heap_trace_record != nullptr, "heap trace getter import missing")) return false;
	g_heap_trace_target = heap_trace_record->vaddr;
	const Loader::SymbolResolve snprintf_symbol {
	    U"eLdDw6l0-bU", U"LibcInternal", 1, U"LibcInternal", 1, 1, Loader::SymbolType::Func};
	const auto* snprintf_record = symbols->Find(snprintf_symbol);
	if (!Check(snprintf_record != nullptr &&
	           symbols->HostCallVariadicKindFor(snprintf_record->vaddr) == Loader::HostCallVariadicKind::Snprintf,
	           "real snprintf variadic registration missing")) return false;
	g_snprintf_target = snprintf_record->vaddr;
	if (!Check(!symbols->SetHostCallVariadicKind(g_snprintf_target, Loader::HostCallVariadicKind::Printf) &&
	           !symbols->SetHostCallScalarMetadata(g_snprintf_target, Loader::MakeHostCallScalarMetadata<&ScalarGp>()),
	           "conflicting variadic metadata admitted")) return false;
	if (!Check(Register<&FloatExchange>(symbols) && Register<&DoubleExchange>(symbols) &&
	           Register<&FloatFromGp>(symbols) && Register<&DoubleFromGp>(symbols) &&
	           Register<&VoidCall>(symbols) && Register<&ScalarGp>(symbols) && Register<&ScalarGpSix>(symbols) &&
	           Register<&PointerFloat>(symbols) && Register<&Mixed>(symbols) &&
	           Register<&NestedRoot>(symbols) &&
	           Register<&PreparedTlsNestedRoot>(symbols) &&
	           Register<&FreshCallbackRoot>(symbols) && Register<&FreshCallbackRelease>(symbols) &&
	           symbols->RegisterHostCallTarget(reinterpret_cast<uint64_t>(&LegacyGp)), "host target registration failed")) return false;
	for (uint64_t target: {reinterpret_cast<uint64_t>(&VoidCall), reinterpret_cast<uint64_t>(&ScalarGp),
	                       reinterpret_cast<uint64_t>(&ScalarGpSix)})
		if (!Check(symbols->SetHostCallEffects(target, Lumen::GuestCpu::HostCallEffect::None),
		           "audited GP fixture did not receive light admission")) return false;
	symbols->SealHostCallRegistry();
	return Check(symbols->HostCallRegistrySealed() && linker->SetupSyntheticTlsForPthreadExitTest(), "runtime TLS fixture failed");
}

bool PureClockCalls(Loader::RuntimeLinker* linker, size_t page_size)
{
	namespace Kernel = Lumen::Libs::LibKernel;
	for (auto service: {&Kernel::KernelReadTsc, &Kernel::KernelGetTscFrequency,
	                    &Kernel::KernelGetProcessTime, &Kernel::KernelGetProcessTimeCounter,
	                    &Kernel::KernelGetProcessTimeCounterFrequency})
	{
		const auto target = reinterpret_cast<uint64_t>(service);
		if (!Check(linker->Symbols()->ResolveHostCall(target).effects == Lumen::GuestCpu::HostCallEffect::None,
		           "pure clock service lacks audited effects")) return false;
		Mapping code(page_size);
		GuestCode guest;
		guest.Stack(8); guest.Call(target); guest.Stack(8, true); guest.Return();
		if (!Check(code.Valid() && guest.Publish(code), "pure clock guest fixture publication")) return false;
		const auto before = service();
		const auto translated = Loader::RunGuestX64(code.Address());
		const auto after = service();
		if (!Check(before <= translated && translated <= after &&
		           Lumen::GuestCpu::QueryHostCallEffects(target) == Lumen::GuestCpu::EffectMask(Lumen::GuestCpu::HostCallEffect::None),
		           "translated pure clock result left native service domain")) return false;
	}
	return Check(linker->Symbols()->ResolveHostCall(g_pthread_self_target).effects == Lumen::GuestCpu::HostCallEffect::None,
	             "translated pthread self lacks audited effects");
}

bool PurePthreadCalls(Loader::RuntimeLinker* linker, size_t page_size)
{
	namespace Kernel = Lumen::Libs::LibKernel;
	struct Service
	{
		const char* nid;
		const char* library;
		uint32_t arity;
		uint64_t target = 0;
	};
	std::array<Service, 4> services {{
	    {"EotR8a3ASf4", "Posix", 0}, {"EI-5-jlq2dE", "libkernel", 0},
	    {"eoht7mQOCmo", "libkernel", 1}, {"0-KXaS70xy4", "Posix", 1},
	}};
	const auto service_check = [](bool condition, const Service& service, const char* message) {
		if (!condition) std::fprintf(stderr, "HostCallScalarTest: pure pthread %s[%s]: %s\n",
		                             service.nid, service.library, message);
		return condition;
	};
	for (auto& service: services)
	{
		const Loader::SymbolResolve symbol {
		    Lumen::Core::String::FromUtf8(service.nid), Lumen::Core::String::FromUtf8(service.library),
		    1, U"libkernel", 1, 1, Loader::SymbolType::Func};
		const auto* record = linker->Symbols()->Find(symbol);
		if (!service_check(record != nullptr, service, "import missing")) return false;
		service.target = record->vaddr;
		const auto metadata = linker->Symbols()->ResolveHostCall(service.target);
		if (!service_check(metadata.classification == Loader::HostCallRegistryClassification::Host &&
		           metadata.effects == Lumen::GuestCpu::HostCallEffect::None &&
		           metadata.scalar != nullptr && metadata.scalar->arity == service.arity &&
		           metadata.scalar->result == Loader::HostCallScalarType::Gp &&
		           (service.arity == 0 || metadata.scalar->arguments[0] == Loader::HostCallScalarType::Gp),
		           service, "service lacks audited GP-only metadata")) return false;
	}
	const auto translated = [&](const Service& service, uint64_t argument, uint64_t expected) {
		Mapping code(page_size);
		GuestCode guest;
		guest.Stack(8);
		if (service.arity != 0) guest.Mov(7, argument);
		guest.Call(service.target); guest.Stack(8, true); guest.Return();
		return service_check(code.Valid() && guest.Publish(code), service, "guest fixture publication") &&
		       service_check(Loader::RunGuestX64(code.Address()) == expected &&
		             Lumen::GuestCpu::QueryHostCallEffects(service.target) ==
		                 Lumen::GuestCpu::EffectMask(Lumen::GuestCpu::HostCallEffect::None),
		             service, "translated result differs from native service");
	};
	if (!translated(services[0], 0, reinterpret_cast<uint64_t>(Kernel::PthreadSelf())) ||
	    !translated(services[1], 0, static_cast<uint64_t>(Kernel::PthreadGetthreadid()))) return false;

	Mapping key_storage(page_size);
	if (!Check(key_storage.Valid(), "pthread key fixture allocation")) return false;
	auto* key_output = reinterpret_cast<Kernel::PthreadKey*>(key_storage.Data());
	if (!Check(Kernel::PthreadKeyCreate(key_output, nullptr) == 0, "pthread key fixture creation")) return false;
	const auto key = *key_output;
	struct KeyCleanup
	{
		Kernel::PthreadKey key;
		~KeyCleanup() { (void)Kernel::PthreadKeyDelete(key); }
	} cleanup {key};
	// A specific value is opaque: neither the native nor translated getter may
	// turn this unmapped token into a guest-memory borrow or dereference.
	constexpr uint64_t value = 0x5345434946494353ULL;
	if (!Check(Kernel::PthreadSetspecific(key, reinterpret_cast<void*>(value)) == 0,
	           "pthread specific fixture setup")) return false;
	for (size_t index: {size_t {2}, size_t {3}})
	{
		const auto native = reinterpret_cast<uint64_t>(Kernel::PthreadGetspecific(key));
		if (!Check(native == value && Kernel::PthreadGetspecific(-1) == nullptr,
		           "native pthread specific fixture differs") ||
		    !translated(services[index], static_cast<uint64_t>(key), native) ||
		    !translated(services[index], UINT64_MAX, 0)) return false;
	}
	return true;
}

bool SanitizerReplacementObjects(Loader::RuntimeLinker* linker, size_t page_size)
{
	constexpr uint64_t marker = 0x53414e4954495a45ULL;
	bool passed = true;
	for (const auto& entry: {std::pair {g_malloc_replace_target, size_t(112)},
	                        std::pair {g_new_replace_target, size_t(104)}})
	{
		Lumen::Cpu::X64State native {};
		if (!Check(Loader::InvokeHostCall(linker->Symbols(), entry.first, &native),
		           "sanitizer replacement getter dispatch failed")) return false;
		const auto address = native.gpr[0];
		{
			Memory::GuestDataLease lease;
			if (!Check(Memory::TryAcquireGuestDataLease(address, entry.second, Memory::Mode::ReadWrite, &lease),
			           "sanitizer replacement object lacks managed RW ownership")) return false;
			std::array<uint64_t, 14> words {};
			std::memcpy(words.data(), reinterpret_cast<const void*>(address), entry.second);
			Memory::ReleaseGuestDataLease(&lease);
			if (!Check(words[0] == entry.second &&
			           std::all_of(words.begin() + 1, words.begin() + entry.second / 8,
			                       [](uint64_t value) { return value == 0; }),
			           "sanitizer replacement ABI defaults differ")) return false;
		}
		// The same faulting contract as a libc initializer: call the real HLE
		// getter, compare its guest-visible header, write a callback cell and
		// call again. The second read proves writable, stable authoritative storage.
		std::vector<uint8_t> bytes;
		const auto emit = [&](std::initializer_list<uint8_t> values) { bytes.insert(bytes.end(), values); };
		const auto word = [&](uint64_t value) {
			for (unsigned shift = 0; shift != 64; shift += 8) bytes.push_back(static_cast<uint8_t>(value >> shift));
		};
		const auto call = [&] { emit({0x48, 0xb8}); word(entry.first); emit({0xff, 0xd0}); };
		emit({0x48, 0x83, 0xec, 0x08}); // sub rsp,8: SysV call alignment
		call();
		emit({0x48, 0x81, 0x38, static_cast<uint8_t>(entry.second), 0, 0, 0, 0x75, 0});
		const auto mismatch = bytes.size() - 1;
		emit({0x48, 0xb9}); word(marker);
		emit({0x48, 0x89, 0x48, 0x08}); // mov [rax+8],rcx
		call();
		emit({0x48, 0x8b, 0x40, 0x08, 0x48, 0x83, 0xc4, 0x08, 0xc3});
		const auto failure = bytes.size();
		emit({0x31, 0xc0, 0x48, 0x83, 0xc4, 0x08, 0xc3});
		bytes[mismatch] = static_cast<uint8_t>(failure - mismatch - 1);
		Mapping code(page_size);
		if (!code.Valid()) return false;
		std::memcpy(code.Data(), bytes.data(), bytes.size());
		if (!code.Protect(0, page_size, Memory::Mode::ExecuteRead) ||
		    !Memory::FlushInstructionCache(code.Address(), bytes.size())) return false;
		passed = Check(Loader::RunGuestX64(code.Address()) == marker,
		               "guest sanitizer header/store/stable-getter contract failed") && passed;
		Memory::GuestDataLease lease;
		if (!Check(Memory::TryAcquireGuestDataLease(address, entry.second, Memory::Mode::ReadWrite, &lease),
		           "sanitizer replacement storage lost managed ownership")) return false;
		std::array<uint64_t, 14> words {};
		std::memcpy(words.data(), reinterpret_cast<const void*>(address), entry.second);
		passed = Check(words[0] == entry.second && words[1] == marker &&
		               std::all_of(words.begin() + 2, words.begin() + entry.second / 8,
		                           [](uint64_t value) { return value == 0; }),
		               "guest sanitizer store changed another ABI field") && passed;
		const uint64_t zero = 0;
		std::memcpy(reinterpret_cast<void*>(address + 8), &zero, sizeof(zero));
		Memory::ReleaseGuestDataLease(&lease);
	}
	return passed;
}

bool ManagedLibObjectImports(size_t page_size)
{
	Loader::SymbolDatabase symbols;
	Lumen::Libs::InitAll(&symbols);
	const std::array<Loader::SymbolResolve, 3> imports {{
	    {U"f7uOxY9mM1U", U"libkernel", 1, U"libkernel", 1, 1, Loader::SymbolType::Object},
	    {U"djxxOmW6-aw", U"libkernel", 1, U"libkernel", 1, 1, Loader::SymbolType::Object},
	    {U"ZT4ODD2Ts9o", U"LibcInternal", 1, U"LibcInternal", 1, 1, Loader::SymbolType::Object},
	}};
	std::array<uint64_t, 3> objects {};
	for (size_t index = 0; index != imports.size(); ++index)
	{
		const auto* record = symbols.Find(imports[index]);
		if (!Check(record != nullptr && record->vaddr != 0 &&
		           symbols.ClassifyHostCallTarget(record->vaddr) == Loader::HostCallRegistryClassification::Guest,
		           "PS4 LIB_OBJECT import missing or classified as a host call")) return false;
		objects[index] = record->vaddr;
	}
	const auto managed = [](uint64_t address, uint64_t bytes, Memory::Mode mode) {
		Memory::ManagedRegion region {};
		return Check(Memory::QueryManagedRegion(address, &region) == Memory::ManagedRegionState::Visible &&
		             address >= region.begin && address < region.end && bytes <= region.end - address &&
		             region.end - region.begin >= 16 * 1024 && region.mode == mode,
		             "PS4 LIB_OBJECT lacks its managed page or declared permissions");
	};
	const auto read = [](uint64_t address, void* output, uint64_t bytes) {
		Memory::GuestDataLease lease;
		if (!Check(Memory::TryAcquireGuestDataLease(address, bytes, Memory::Mode::Read, &lease),
		           "PS4 LIB_OBJECT read lease failed")) return false;
		std::memcpy(output, reinterpret_cast<const void*>(address), bytes);
		return true;
	};
	if (!managed(objects[0], sizeof(uint64_t), Memory::Mode::Read) ||
	    !managed(objects[1], sizeof(uint64_t), Memory::Mode::ReadWrite) ||
	    !managed(objects[2], sizeof(uint32_t), Memory::Mode::ReadWrite)) return false;
	{
		Memory::GuestDataLease write;
		if (!Check(!Memory::TryAcquireGuestDataLease(objects[0], sizeof(uint64_t), Memory::Mode::Write, &write),
		           "stack guard permits guest writes")) return false;
	}
	uint64_t name_buffer = 0;
	if (!read(objects[1], &name_buffer, sizeof(name_buffer)) ||
	    !managed(name_buffer, 512, Memory::Mode::ReadWrite)) return false;
	Memory::ManagedRegion cell_region {}, buffer_region {};
	if (!Check(Memory::QueryManagedRegion(objects[1], &cell_region) == Memory::ManagedRegionState::Visible &&
	           Memory::QueryManagedRegion(name_buffer, &buffer_region) == Memory::ManagedRegionState::Visible &&
	           cell_region.begin == buffer_region.begin && cell_region.end == buffer_region.end &&
	           name_buffer >= objects[1] + sizeof(uint64_t),
	           "progname cell and string do not share the managed backing")) return false;
	Mapping got(page_size), output(page_size), code(page_size);
	if (!got.Valid() || !output.Valid() || !code.Valid()) return false;
	std::memcpy(got.Data(), objects.data(), sizeof(objects));
	GuestCode guest;
	guest.Bytes({0x48, 0x8b, 0x07, 0x48, 0x8b, 0x00}); // guard: GOT entry, then object
	guest.Mov(10, output.Address());
	guest.Bytes({0x49, 0x89, 0x02}); // mov [r10],rax
	guest.Bytes({0x48, 0x8b, 0x47, 0x08, 0x48, 0x8b, 0x00}); // progname: GOT entry, then pointer cell
	guest.Bytes({0x49, 0x89, 0x42, 0x08, 0x0f, 0xb6, 0x00, 0x49, 0x89, 0x42, 0x10}); // pointer and first char
	guest.Bytes({0x48, 0x8b, 0x47, 0x10, 0x8b, 0x08, 0x49, 0x89, 0x4a, 0x18}); // read uint32 flag
	constexpr uint32_t flag_marker = 0x13572468;
	guest.Bytes({0xc7, 0x00, 0x68, 0x24, 0x57, 0x13, 0x8b, 0x00}); // write and reload authoritative flag
	guest.Return();
	if (!guest.Publish(code)) return false;
	const auto run = [&](uint8_t first_char) {
		std::memset(output.Data(), 0, 4 * sizeof(uint64_t));
		if (!Check(Loader::RunGuestX64(code.Address(), got.Address()) == flag_marker,
		           "guest LIB_OBJECT flag write/reload failed")) return false;
		std::array<uint64_t, 4> observed {};
		std::memcpy(observed.data(), output.Data(), sizeof(observed));
		uint32_t flag = 0;
		return read(objects[2], &flag, sizeof(flag)) &&
		       Check(observed == std::array<uint64_t, 4> {0xDeadBeef5533CCAAULL, name_buffer, first_char, 1} &&
		             flag == flag_marker, "guest LIB_OBJECT dereference or shared flag storage differs");
	};
	Lumen::Libs::LibKernel::ResetSessionState();
	Lumen::Libs::LibcInternal::ResetSessionState();
	if (!run(0)) return false;
	std::string long_name(600, 'x');
	Lumen::Libs::LibKernel::SetProgName(Lumen::Core::String::FromUtf8(long_name.c_str()));
	std::array<char, 512> name {};
	if (!read(name_buffer, name.data(), name.size()) ||
	    !Check(name.back() == 0 && std::all_of(name.begin(), name.end() - 1, [](char value) { return value == 'x'; }),
	           "progname truncation or termination differs")) return false;
	Lumen::Libs::LibKernel::SetProgName(U"guest-object");
	if (!read(name_buffer, name.data(), name.size()) ||
	    !Check(std::strcmp(name.data(), "guest-object") == 0, "progname setter did not update the exported string")) return false;
	Lumen::Libs::LibcInternal::ResetSessionState();
	if (!run('g')) return false;
	Lumen::Libs::LibKernel::ResetSessionState();
	Lumen::Libs::LibcInternal::ResetSessionState();
	for (size_t index = 0; index != imports.size(); ++index)
	{
		const auto* record = symbols.Find(imports[index]);
		if (!Check(record != nullptr && record->vaddr == objects[index],
		           "session reset changed a PS4 LIB_OBJECT import address")) return false;
	}
	uint64_t reset_name_buffer = 0;
	if (!read(objects[1], &reset_name_buffer, sizeof(reset_name_buffer)) ||
	    !read(name_buffer, name.data(), name.size()) ||
	    !Check(reset_name_buffer == name_buffer && std::all_of(name.begin(), name.end(), [](char value) { return value == 0; }),
	           "progname session reset changed pointer identity or retained string bytes") ||
	    !managed(objects[0], sizeof(uint64_t), Memory::Mode::Read) ||
	    !managed(objects[1], sizeof(uint64_t), Memory::Mode::ReadWrite) ||
	    !managed(objects[2], sizeof(uint32_t), Memory::Mode::ReadWrite) ||
	    !managed(name_buffer, name.size(), Memory::Mode::ReadWrite) || !run(0)) return false;
	Lumen::Libs::LibcInternal::ResetSessionState();
	return true;
}

bool HeapTraceObjects(Loader::RuntimeLinker* linker, size_t page_size)
{
	struct Info
	{
		uint64_t size;
		uint32_t unknown1;
		uint32_t unknown2;
		uint64_t mask;
		uint64_t table;
	};
	static_assert(sizeof(Info) == 32);
	Mapping output(page_size), code(page_size);
	if (!output.Valid() || !code.Valid()) return false;
	const Info initial {sizeof(Info), 0x12345678, 0x87654321, 0, 0};
	std::memcpy(output.Data(), &initial, sizeof(initial));
	const auto getter = [&] {
		Lumen::Cpu::X64State native {};
		native.gpr[7] = output.Address();
		const bool handled = Loader::InvokeHostCall(linker->Symbols(), g_heap_trace_target, &native);
		if (native.memory_fault_size != 0) std::fprintf(stderr,
		    "HostCallScalarTest: heap getter reports memory fault %#llx bytes=%llu\n",
		    static_cast<unsigned long long>(native.memory_fault_address),
		    static_cast<unsigned long long>(native.memory_fault_size));
		return handled && native.memory_fault_size == 0;
	};
	if (!Check(getter(), "heap trace getter dispatch failed")) return false;
	Info info {};
	std::memcpy(&info, output.Data(), sizeof(info));
	if (!Check(info.size == sizeof(Info) && info.unknown1 == initial.unknown1 && info.unknown2 == initial.unknown2,
	           "heap trace getter changed unrelated ABI fields")) return false;
	for (const auto& object: {std::pair {info.mask, sizeof(uint64_t)},
	                         std::pair {info.table, 64 * sizeof(uint64_t)}})
	{
		Memory::ManagedRegion region {};
		if (!Check(Memory::QueryManagedRegion(object.first, &region) == Memory::ManagedRegionState::Visible &&
		           object.first >= region.begin && object.first <= region.end && object.second <= region.end - object.first &&
		           region.mode == Memory::Mode::ReadWrite,
		           "heap trace ABI object is outside managed RW backing")) return false;
	}
	const auto values = [&](uint64_t expected_mask, uint64_t expected_last) {
		Memory::GuestDataLease mask_lease, table_lease;
		Memory::GuestDataLeaseDenialMetadata denial;
		if (!Memory::TryAcquireGuestDataLease(info.mask, sizeof(uint64_t), Memory::Mode::ReadWrite, &mask_lease, true, &denial))
		{
			std::fprintf(stderr, "HostCallScalarTest: heap trace mask %#llx denied: %s\n",
			    static_cast<unsigned long long>(info.mask), Memory::GuestDataLeaseDenialReasonName(denial.reason));
			return false;
		}
		uint64_t mask {};
		std::array<uint64_t, 64> table {};
		std::memcpy(&mask, reinterpret_cast<const void*>(info.mask), sizeof(mask));
		Memory::ReleaseGuestDataLease(&mask_lease);
		if (!Memory::TryAcquireGuestDataLease(info.table, sizeof(table), Memory::Mode::ReadWrite, &table_lease, true, &denial))
		{
			std::fprintf(stderr, "HostCallScalarTest: heap trace table %#llx denied: %s\n",
			    static_cast<unsigned long long>(info.table), Memory::GuestDataLeaseDenialReasonName(denial.reason));
			return false;
		}
		std::memcpy(table.data(), reinterpret_cast<const void*>(info.table), sizeof(table));
		return Check(mask == expected_mask && table.back() == expected_last &&
		             std::all_of(table.begin(), table.end() - 1, [](uint64_t value) { return value == 0; }),
		             "heap trace backing contents differ");
	};
	if (!values(0, 0)) return false;
	constexpr uint64_t mask_marker = 0x4845415054524143ULL;
	constexpr uint64_t table_marker = 0x5441424c455f3633ULL;
	GuestCode guest;
	guest.Stack(8);
	guest.Mov(7, output.Address());
	guest.Call(g_heap_trace_target);
	guest.Mov(0, output.Address());
	guest.Bytes({0x4c, 0x8b, 0x50, 0x10}); // mov r10,[rax+16]: mask pointer
	guest.Mov(1, mask_marker);
	guest.Bytes({0x49, 0x89, 0x0a}); // mov [r10],rcx
	guest.Bytes({0x4c, 0x8b, 0x50, 0x18}); // mov r10,[rax+24]: table pointer
	guest.Mov(1, table_marker);
	guest.Bytes({0x49, 0x89, 0x8a, 0xf8, 0x01, 0, 0}); // mov [r10+63*8],rcx
	guest.Mov(7, output.Address());
	guest.Call(g_heap_trace_target);
	guest.Mov(0, output.Address());
	guest.Bytes({0x48, 0x8b, 0x40, 0x18, 0x48, 0x8b, 0x80, 0xf8, 0x01, 0, 0});
	guest.Stack(8, true);
	guest.Return();
	if (!guest.Publish(code) || !Check(Loader::RunGuestX64(code.Address()) == table_marker,
	                                  "guest heap trace getter/store/reload failed") ||
	    !values(mask_marker, table_marker)) return false;
	Info repeated {};
	std::memcpy(&repeated, output.Data(), sizeof(repeated));
	if (!Check(repeated.mask == info.mask && repeated.table == info.table,
	           "heap trace getter changed backing pointer identity")) return false;
	Lumen::Libs::LibcInternalExt::ResetSessionState();
	if (!Check(getter(), "heap trace getter failed after session reset")) return false;
	std::memcpy(&repeated, output.Data(), sizeof(repeated));
	return Check(repeated.mask == info.mask && repeated.table == info.table,
	             "heap trace session reset changed backing pointer identity") && values(0, 0);
}

bool GuestEntryParameters(size_t page_size)
{
	Mapping code(page_size), output(page_size);
	if (!code.Valid() || !output.Valid()) return false;
	GuestCode guest;
	guest.Bytes({0x8b, 0x07}); // mov eax,[rdi]: argc
	guest.SaveResult(output.Address());
	guest.Bytes({0x48, 0x8b, 0x47, 0x08}); // mov rax,[rdi+8]: argv[0]
	guest.SaveResult(output.Address(8));
	guest.Bytes({0x0f, 0xb6, 0x00}); // movzx eax,byte [rax]
	guest.SaveResult(output.Address(16));
	guest.Bytes({0x48, 0x8b, 0x47, 0x08, 0x0f, 0xb6, 0x04, 0x30}); // argv[0][rsi]: NUL
	guest.SaveResult(output.Address(24));
	guest.Bytes({0x48, 0x8b, 0x47, 0x08, 0x0f, 0xb6, 0x44, 0x30, 0xff}); // last name byte
	guest.SaveResult(output.Address(32));
	guest.Bytes({0x31, 0xc0}); // aggregate every remaining argv cell
	for (uint32_t index = 1; index != 33; ++index)
	{
		const uint32_t offset = 8 + index * sizeof(uint64_t);
		guest.Bytes({0x48, 0x0b, 0x87, static_cast<uint8_t>(offset), static_cast<uint8_t>(offset >> 8), 0, 0});
	}
	guest.Return();
	if (!guest.Publish(code)) return false;
	for (const auto& expected: {std::string("guest-entry"), std::string(17000, 'x')})
	{
		std::string input = expected;
		const auto address = Loader::PrepareEntryParametersForTest(input.c_str());
		if (!Check(address != 0, "entry parameter preparation failed")) return false;
		struct Owner
		{
			uint64_t address;
			~Owner() { if (address != 0) Memory::Free(address); }
		} owner {address};
		input.clear(); input.shrink_to_fit(); // entry storage must own the UTF-8 bytes
		constexpr size_t params_size = sizeof(uint64_t) * 34;
		Memory::ManagedRegion region {};
		if (!Check(Memory::QueryManagedRegion(address, &region) == Memory::ManagedRegionState::Visible &&
		           region.mode == Memory::Mode::ReadWrite && address >= region.begin && address < region.end &&
		           params_size + expected.size() + 1 <= region.end - address,
		           "entry parameter block or long name is outside managed backing")) return false;
		std::array<uint64_t, 34> params {};
		if (!Check(Loader::ReadGuestMemory(address, params.data(), sizeof(params)) && params[0] == 1 &&
		           params[1] >= address + params_size && params[1] < region.end &&
		           expected.size() + 1 <= region.end - params[1] &&
		           std::all_of(params.begin() + 2, params.end(), [](uint64_t cell) { return cell == 0; }),
		           "entry argc/padding, argv pointer or remaining argv cells differ")) return false;
		std::vector<char> name(expected.size() + 1);
		if (!Check(Loader::ReadGuestMemory(params[1], name.data(), name.size()) &&
		           std::memcmp(name.data(), expected.c_str(), name.size()) == 0,
		           "entry argv string changed with its temporary native source")) return false;
		if (!Check(Loader::RunGuestX64(code.Address(), address, expected.size()) == 0,
		           "translated entry argv tail contains a nonzero pointer")) return false;
		std::array<uint64_t, 5> observed {};
		std::memcpy(observed.data(), output.Data(), sizeof(observed));
		if (!Check(observed == std::array<uint64_t, 5> {1, params[1], static_cast<uint8_t>(expected.front()),
		                                             0, static_cast<uint8_t>(expected.back())},
		           "translated entry argc/argv/string dereference differs")) return false;
		if (!Check(Memory::Free(address), "test entry parameter retirement failed")) return false;
		owner.address = 0;
		if (!Check(Memory::QueryManagedRegion(address, &region) != Memory::ManagedRegionState::Visible,
		           "test entry parameter block remains visible after guest return and retirement")) return false;
	}
	return true;
}

bool ManagedThreadAbi(Loader::RuntimeLinker* linker, size_t page_size)
{
	Mapping fs_code(page_size), fs_store_code(page_size), errno_code(page_size), tls_code(page_size),
	        nested_code(page_size), descriptor(page_size), pthread_code(page_size), pthread_output(page_size);
	if (!fs_code.Valid() || !fs_store_code.Valid() || !errno_code.Valid() || !tls_code.Valid() ||
	    !nested_code.Valid() || !descriptor.Valid() || !pthread_code.Valid() || !pthread_output.Valid()) return false;
	GuestCode fs;
	fs.Bytes({0x64, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0}); // mov rax,fs:[0]
	fs.Return();
	GuestCode fs_store;
	fs_store.Bytes({0x64, 0x89, 0x3c, 0x25, 0xf0, 0xff, 0xff, 0xff}); // mov fs:[-16],edi
	fs_store.Bytes({0x64, 0x8b, 0x04, 0x25, 0xf0, 0xff, 0xff, 0xff}); // mov eax,fs:[-16]
	fs_store.Return();
	GuestCode pthread;
	pthread.Bytes({0x64, 0x48, 0x8b, 0x04, 0x25, 0x10, 0, 0, 0}); // mov rax,fs:[0x10]: tcb_pthread
	pthread.SaveResult(pthread_output.Address());
	pthread.Bytes({0x8b, 0x48, 0x20, 0x49, 0x89, 0x4a, 0x08}); // save initial dword [handle+0x20]
	pthread.Bytes({0x81, 0x48, 0x20, 0, 0x01, 0, 0, 0x8b, 0x40, 0x20}); // or dword [handle+0x20],0x100; reload
	pthread.SaveResult(pthread_output.Address(16));
	pthread.Stack(8); pthread.Call(g_pthread_self_target); pthread.Stack(8, true); pthread.Return();
	GuestCode error;
	error.Stack(8); error.Call(g_errno_target);
	error.Bytes({0xc7, 0x00, 0x68, 0x24, 0x57, 0x13}); // mov dword [rax],0x13572468
	error.Call(g_errno_target); error.Bytes({0x8b, 0x00});
	error.Stack(8, true); error.Return();
	Loader::Program module;
	constexpr std::array<uint8_t, 16> image {0x21, 0x43, 0x65, 0x87, 0xa9, 0xcb, 0xed, 0x0f};
	module.tls.image_vaddr = reinterpret_cast<uint64_t>(image.data());
	module.tls.image_size = image.size();
	auto* module_tls = Loader::RuntimeLinker::TlsGetAddr(&module);
	const std::array<uint64_t, 2> tls_descriptor {reinterpret_cast<uint64_t>(&module), 4};
	std::memcpy(descriptor.Data(), tls_descriptor.data(), sizeof(tls_descriptor));
	GuestCode dynamic;
	dynamic.Stack(8); dynamic.Mov(7, descriptor.Address()); dynamic.Call(g_tls_target);
	dynamic.Bytes({0xc7, 0x00, 0xef, 0xbe, 0xad, 0x0b}); // mov dword [rax],0x0badbeef
	dynamic.Mov(7, descriptor.Address()); dynamic.Call(g_tls_target); dynamic.Bytes({0x8b, 0x00});
	dynamic.Stack(8, true); dynamic.Return();
	GuestCode nested;
	nested.Stack(8); nested.Call(reinterpret_cast<uint64_t>(&PreparedTlsNestedRoot));
	nested.Stack(8, true); nested.Return();
	if (!fs.Publish(fs_code) || !fs_store.Publish(fs_store_code) || !pthread.Publish(pthread_code) || !error.Publish(errno_code) ||
	    !dynamic.Publish(tls_code) || !nested.Publish(nested_code)) return false;
	g_main_tcb = Loader::RunGuestX64(fs_code.Address());
	Memory::ManagedRegion region {};
	bool pass = Check(g_main_tcb != 0 &&
	    Memory::QueryManagedRegion(g_main_tcb, &region) == Memory::ManagedRegionState::Visible &&
	    region.mode == Memory::Mode::ReadWrite,
	    "FS self pointer lacks owned RW TLS/TCB storage");
	uint64_t self {}, dtv {};
	uint32_t initial {};
	pass = Check(Loader::ReadGuestMemory(g_main_tcb, &self, sizeof(self)) && self == g_main_tcb &&
	    Loader::ReadGuestMemory(g_main_tcb + 8, &dtv, sizeof(dtv)) && dtv == 0 &&
	    Loader::ReadGuestMemory(g_main_tcb - 16, &initial, sizeof(initial)) && initial == 0xa5197231U,
	    "prepared TLS image or FreeBSD TCB layout differs") && pass;
	const auto pthread_handle = Lumen::Libs::LibKernel::PthreadGuestSelfHandle();
	uint64_t tcb_pthread = 0;
	uint32_t pthread_flags = 0;
	if (!Check(pthread_handle != 0 &&
	           pthread_handle == reinterpret_cast<uint64_t>(Lumen::Libs::LibKernel::PthreadSelf()) &&
	           Memory::QueryManagedRegion(pthread_handle, &region) == Memory::ManagedRegionState::Visible &&
	           region.mode == Memory::Mode::ReadWrite && region.end - region.begin >= 16 * 1024 &&
	           pthread_handle >= region.begin && pthread_handle < region.end &&
	           4096 <= region.end - pthread_handle &&
	           Loader::ReadGuestMemory(g_main_tcb + 0x10, &tcb_pthread, sizeof(tcb_pthread)) && tcb_pthread == pthread_handle &&
	           Loader::ReadGuestMemory(pthread_handle + 0x20, &pthread_flags, sizeof(pthread_flags)),
	           "TCB pthread slot differs from the managed pthread self handle")) return false;
	const auto translated_self = Loader::RunGuestX64(pthread_code.Address());
	std::array<uint64_t, 3> pthread_observed {};
	std::memcpy(pthread_observed.data(), pthread_output.Data(), sizeof(pthread_observed));
	uint32_t updated_pthread_flags = 0;
	pass = Check(translated_self == pthread_handle &&
	    pthread_observed == std::array<uint64_t, 3> {pthread_handle, pthread_flags, pthread_flags | 0x100U} &&
	    Loader::ReadGuestMemory(pthread_handle + 0x20, &updated_pthread_flags, sizeof(updated_pthread_flags)) &&
	    updated_pthread_flags == (pthread_flags | 0x100U),
	    "translated TCB pthread dereference/store or pthread-self import differs") && pass;
	pass = Check(Loader::RunGuestX64(fs_store_code.Address(), 0x11223344U) == 0x11223344U,
	    "translated FS-relative TLS write/read failed") && pass;
	auto* main_error = Lumen::Libs::Posix::GetErrorAddr();
	pass = Check(Loader::RunGuestX64(errno_code.Address()) == 0x13572468U &&
	    Lumen::Libs::Posix::GetErrorAddr() == main_error && *main_error == 0x13572468,
	    "translated errno write was not visible through the stable native getter") && pass;
	g_prepared_tls_entry = tls_code.Address();
	g_prepared_tls_nested_vm_unchanged = false;
	pass = Check(Loader::RunGuestX64(nested_code.Address()) == 0x0badbeefU &&
	    g_prepared_tls_nested_vm_unchanged,
	    "prepared dynamic TLS lookup mutated VM under nested HLE readers") && pass;
	uint32_t module_value {};
	pass = Check(Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(module_tls) + 4,
	    &module_value, sizeof(module_value)) && module_value == 0x0badbeefU,
	    "dynamic TLS getter did not return authoritative owned storage") && pass;
	uint64_t child_tcb {}, child_error {};
	bool child_pass = false;
	std::thread child([&] {
		child_tcb = Loader::RunGuestX64(fs_code.Address());
		child_error = reinterpret_cast<uint64_t>(Lumen::Libs::Posix::GetErrorAddr());
		child_pass = child_tcb != g_main_tcb && child_error != reinterpret_cast<uint64_t>(main_error) &&
		    *Lumen::Libs::Posix::GetErrorAddr() == 0 &&
		    Loader::RunGuestX64(fs_store_code.Address(), 0x55667788U) == 0x55667788U &&
		    Loader::RunGuestX64(errno_code.Address()) == 0x13572468U;
		linker->DeleteTlss(Lumen::Core::Thread::GetThreadIdUnique());
	});
	child.join();
	uint32_t main_value {};
	pass = Check(child_pass && Loader::ReadGuestMemory(g_main_tcb - 16, &main_value, sizeof(main_value)) &&
	    main_value == 0x11223344U && *main_error == 0x13572468 &&
	    Memory::QueryManagedRegion(child_tcb, &region) != Memory::ManagedRegionState::Visible &&
	    Memory::QueryManagedRegion(child_error, &region) != Memory::ManagedRegionState::Visible,
	    "thread TLS/errno isolation or native exit retirement failed") && pass;
	const auto retired_module = reinterpret_cast<uint64_t>(module_tls);
	Loader::RuntimeLinker::DeleteTls(&module, Lumen::Core::Thread::GetThreadIdUnique());
	pass = Check(Memory::QueryManagedRegion(retired_module, &region) != Memory::ManagedRegionState::Visible,
	    "module TLS retirement left a guest-visible mapping") && pass;
	return pass;
}

bool GuestPthreadCreate(size_t page_size)
{
	namespace Kernel = Lumen::Libs::LibKernel;
	Mapping data(page_size), caller_code(page_size), worker_code(page_size);
	if (!data.Valid() || !caller_code.Valid() || !worker_code.Valid()) return false;
	constexpr uint64_t token = 0x6775657374617267ULL;
	std::memcpy(data.Data(), &token, sizeof(token));
	constexpr char name[] = "guest-create";
	std::memcpy(data.Data(64), name, sizeof(name));
	auto* attr = reinterpret_cast<Kernel::PthreadAttr*>(data.Address(48));
	if (!Check(Kernel::PthreadAttrInit(attr) == 0, "guest Create attribute setup failed")) return false;
	GuestCode worker;
	worker.Bytes({0x64, 0x48, 0x8b, 0x04, 0x25, 0x10, 0, 0, 0}); // mov rax,fs:[0x10]
	worker.Bytes({0x48, 0x89, 0x47, 0x08}); // mov [rdi+8],rax: worker's handle
	worker.Bytes({0x81, 0x48, 0x20, 0, 0x01, 0, 0, 0x8b, 0x48, 0x20, 0x89, 0x4f, 0x10}); // OR/reload prefix flags
	worker.Bytes({0x48, 0x8b, 0x07}); // read argument data passed through Create
	worker.SaveResult(data.Address(32));
	worker.Stack(8); worker.Call(g_pthread_self_target); worker.Stack(8, true);
	worker.SaveResult(data.Address(40));
	worker.Mov(0, token); worker.Return();
	GuestCode caller;
	caller.Stack(8);
	caller.Mov(7, data.Address(24)); caller.Mov(6, data.Address(48)); caller.Mov(2, worker_code.Address());
	caller.Mov(1, data.Address()); caller.Mov(8, data.Address(64));
	caller.Call(g_pthread_create_target);
	caller.Stack(8, true); caller.Return();
	if (!worker.Publish(worker_code) || !caller.Publish(caller_code))
	{
		Kernel::PthreadAttrDestroy(attr);
		return false;
	}
	const auto result = Loader::RunGuestX64(caller_code.Address());
	bool pass = Check(Kernel::PthreadAttrDestroy(attr) == 0, "guest Create attribute cleanup failed");
	if (!Check(result == 0, "translated Create with managed inputs/output failed or stalled")) return false;
	Kernel::Pthread handle = nullptr;
	std::memcpy(&handle, data.Data(24), sizeof(handle));
	const auto address = reinterpret_cast<uint64_t>(handle);
	Memory::ManagedRegion region {};
	pass = Check(address != 0 && Memory::QueryManagedRegion(address, &region) == Memory::ManagedRegionState::Visible &&
	    region.mode == Memory::Mode::ReadWrite && address >= region.begin && address < region.end &&
	    4096 <= region.end - address && region.end - region.begin >= 16 * 1024,
	    "guest Create output is not a managed pthread handle") && pass;
	pass = Check(Kernel::PthreadGetname(handle, reinterpret_cast<char*>(data.Data(128))) == 0 &&
	    std::strcmp(reinterpret_cast<const char*>(data.Data(128)), name) == 0,
	    "guest Create did not preserve its managed name input") && pass;
	if (!Check(Kernel::PthreadJoin(handle, reinterpret_cast<void**>(data.Data(56))) == 0,
	           "translated Create worker could not be joined")) return false;
	std::array<uint64_t, 8> observed {};
	std::memcpy(observed.data(), data.Data(), sizeof(observed));
	pass = Check(observed[1] == address && observed[2] == 0x100 && observed[4] == token &&
	    observed[5] == address && observed[7] == token,
	    "created worker lost its argument, TCB/pthread-self identity, prefix store or return value") && pass;
	std::array<uint8_t, 4096> prefix {};
	pass = Check(Loader::ReadGuestMemory(address, prefix.data(), prefix.size()) &&
	    std::all_of(prefix.begin(), prefix.end(), [](uint8_t value) { return value == 0; }),
	    "joined worker retained guest pthread state before pool reuse") && pass;
	constexpr int priority_marker = 0x5aa55aa5;
	std::memcpy(data.Data(256), &priority_marker, sizeof(priority_marker));
	pass = Check(Kernel::PthreadGetprio(handle, reinterpret_cast<int*>(data.Data(256))) == Kernel::KERNEL_ERROR_ESRCH &&
	    std::memcmp(data.Data(256), &priority_marker, sizeof(priority_marker)) == 0 &&
	    Kernel::PthreadJoin(handle, nullptr) == Kernel::KERNEL_ERROR_ESRCH,
	    "joined guest pthread handle still resolves or changes rejected API output") && pass;
	return pass;
}

bool GuestPeerThreadStack(size_t page_size)
{
	Mapping data(page_size), caller_code(page_size), worker_code(page_size);
	if (!data.Valid() || !caller_code.Valid() || !worker_code.Valid()) return false;
	constexpr uint64_t initial = 0x706565725f726561ULL;
	constexpr uint64_t changed = 0x706565725f777269ULL;
	GuestCode worker;
	worker.Bytes({0x48, 0x8b, 0x07}); // mov rax,[rdi]: creator's live local
	worker.SaveResult(data.Address(8));
	worker.Mov(0, changed);
	worker.Bytes({0x48, 0x89, 0x07}); // mov [rdi],rax: write to the peer stack
	worker.Return();
	GuestCode caller;
	caller.Stack(24); caller.Spill(0, initial);
	caller.Bytes({0x48, 0x89, 0xe0}); // mov rax,rsp: save the local's address
	caller.SaveResult(data.Address());
	caller.Mov(7, data.Address(24)); caller.Mov(6, 0); caller.Mov(2, worker_code.Address());
	caller.Bytes({0x48, 0x89, 0xe1}); // mov rcx,rsp: worker argument is this local
	caller.Mov(8, 0); caller.Call(g_pthread_create_target);
	caller.SaveResult(data.Address(32));
	caller.LoadResult(data.Address(24));
	caller.Bytes({0x48, 0x89, 0xc7}); // mov rdi,rax: worker handle
	caller.Mov(6, data.Address(48)); caller.Call(g_pthread_join_target);
	caller.SaveResult(data.Address(40));
	caller.Bytes({0x48, 0x8b, 0x04, 0x24}); // reload the changed local after join
	caller.SaveResult(data.Address(16));
	caller.Stack(24, true); caller.Return();
	if (!worker.Publish(worker_code) || !caller.Publish(caller_code)) return false;
	const auto result = Loader::RunGuestX64(caller_code.Address());
	std::array<uint64_t, 7> observed {};
	std::memcpy(observed.data(), data.Data(), sizeof(observed));
	if (!Check(result == changed && observed[0] != 0 && observed[1] == initial &&
	           observed[2] == changed && observed[3] != 0 && observed[4] == 0 &&
	           observed[5] == 0 && observed[6] == changed,
	           "guest worker could not read/write its creator's live primary stack")) return false;
	Memory::ManagedRegion retired {};
	Memory::GuestDataLease lease;
	uint64_t rejected = RETURN_TOKEN;
	return Check(Memory::QueryManagedRegion(observed[0], &retired) != Memory::ManagedRegionState::Visible &&
	             !Memory::TryAcquireGuestDataLease(observed[0], sizeof(uint64_t), Memory::Mode::Read, &lease) &&
	             !Loader::ReadGuestMemory(observed[0], &rejected, sizeof(rejected)) && rejected == RETURN_TOKEN,
	             "returned primary stack remained readable after managed retirement");
}

bool GuestMutexAbi(size_t page_size)
{
	namespace Kernel = Lumen::Libs::LibKernel;
	Mapping data(page_size), init_code(page_size), access_code(page_size);
	if (!data.Valid() || !init_code.Valid() || !access_code.Valid()) return false;
	constexpr char name[] = "guest-mutex";
	std::memcpy(data.Data(64), name, sizeof(name));
	GuestCode init;
	init.Stack(8);
	init.Mov(7, data.Address()); init.Mov(6, 0); init.Mov(2, data.Address(64));
	init.Call(g_mutex_init_target);
	init.Stack(8, true); init.Return();
	GuestCode access;
	access.LoadResult(data.Address()); // slot -> guest mutex handle
	access.SaveResult(data.Address(16));
	access.Bytes({0x81, 0x48, 0x20, 0, 0x01, 0, 0, 0x8b, 0x40, 0x20}); // or dword [handle+0x20],0x100; reload
	access.Return();
	if (!init.Publish(init_code) || !access.Publish(access_code) ||
	    !Check(Loader::RunGuestX64(init_code.Address()) == 0, "translated mutex init failed or stalled")) return false;
	auto* slot = reinterpret_cast<Kernel::PthreadMutex*>(data.Data());
	uint64_t address = 0;
	std::memcpy(&address, data.Data(), sizeof(address));
	Memory::ManagedRegion region {};
	if (!Check(address != 0 && Memory::QueryManagedRegion(address, &region) == Memory::ManagedRegionState::Visible &&
	           region.mode == Memory::Mode::ReadWrite && address >= region.begin && address < region.end &&
	           256 <= region.end - address && region.end - region.begin >= 16 * 1024,
	           "mutex init published a native object instead of managed guest backing"))
	{
		Kernel::PthreadMutexDestroy(slot);
		return false;
	}
	bool pass = Check(Loader::RunGuestX64(access_code.Address()) == 0x100,
	                  "translated mutex prefix write/reload failed");
	uint64_t observed_handle = 0;
	std::memcpy(&observed_handle, data.Data(16), sizeof(observed_handle));
	pass = Check(observed_handle == address, "translated mutex slot dereference changed handle identity") && pass;
	if (!Check(Kernel::PthreadMutexLock(slot) == 0, "native HLE could not lock the guest mutex handle"))
	{
		Kernel::PthreadMutexDestroy(slot);
		return false;
	}
	pass = Check(Kernel::PthreadMutexTrylock(slot) == Kernel::KERNEL_ERROR_EBUSY,
	             "guest mutex alias lost native lock ownership") && pass;
	auto* condition = reinterpret_cast<Kernel::PthreadCond*>(data.Data(32));
	if (!Check(Kernel::PthreadCondInit(condition, nullptr, nullptr) == 0,
	           "condition setup for guest mutex interop failed"))
	{
		Kernel::PthreadMutexUnlock(slot);
		Kernel::PthreadMutexDestroy(slot);
		return false;
	}
	pass = Check(Kernel::PthreadCondTimedwait(condition, slot, 2000) == Kernel::KERNEL_ERROR_ETIMEDOUT &&
	    Kernel::PthreadMutexTrylock(slot) == Kernel::KERNEL_ERROR_EBUSY,
	    "condition wait did not restore ownership through the guest mutex alias") && pass;
	pass = Check(Kernel::PthreadCondDestroy(condition) == 0, "guest mutex interop condition cleanup failed") && pass;
	if (!Check(Kernel::PthreadMutexUnlock(slot) == 0, "native HLE could not unlock the guest mutex handle")) return false;
	if (!Check(Kernel::PthreadMutexTrylock(slot) == 0, "native HLE could not acquire the released guest mutex handle"))
	{
		Kernel::PthreadMutexDestroy(slot);
		return false;
	}
	pass = Check(Kernel::PthreadMutexUnlock(slot) == 0, "guest mutex final unlock failed") && pass;
	uint32_t flags = 0;
	uint64_t stable_handle = 0;
	std::memcpy(&stable_handle, data.Data(), sizeof(stable_handle));
	pass = Check(stable_handle == address && Loader::ReadGuestMemory(address + 0x20, &flags, sizeof(flags)) && flags == 0x100,
	             "native mutex APIs changed guest prefix bytes or handle identity") && pass;
	std::memcpy(data.Data(8), &address, sizeof(address));
	pass = Check(Kernel::PthreadMutexDestroy(slot) == 0, "guest mutex destroy failed") && pass;
	std::memcpy(&stable_handle, data.Data(), sizeof(stable_handle));
	pass = Check(stable_handle == 0 && Memory::QueryManagedRegion(address, &region) == Memory::ManagedRegionState::Visible &&
	    region.mode == Memory::Mode::ReadWrite,
	    "mutex destroy did not clear its slot or retain the reusable guest page") && pass;
	auto* retired_slot = reinterpret_cast<Kernel::PthreadMutex*>(data.Data(8));
	pass = Check(Kernel::PthreadMutexLock(retired_slot) == Kernel::KERNEL_ERROR_EINVAL &&
	    Kernel::PthreadMutexTrylock(retired_slot) == Kernel::KERNEL_ERROR_EINVAL &&
	    Kernel::PthreadMutexUnlock(retired_slot) == Kernel::KERNEL_ERROR_EINVAL &&
	    Kernel::PthreadMutexDestroy(retired_slot) == Kernel::KERNEL_ERROR_EINVAL &&
	    std::memcmp(data.Data(8), &address, sizeof(address)) == 0,
	    "retired mutex alias still resolves or changes its rejected slot") && pass;
	if (!Check(Loader::RunGuestX64(init_code.Address()) == 0, "fresh translated mutex init failed")) return false;
	uint64_t fresh_address = 0;
	std::memcpy(&fresh_address, data.Data(), sizeof(fresh_address));
	std::array<uint8_t, 256> prefix {};
	pass = Check(fresh_address == address &&
	    Memory::QueryManagedRegion(fresh_address, &region) == Memory::ManagedRegionState::Visible &&
	    region.mode == Memory::Mode::ReadWrite && fresh_address >= region.begin && fresh_address < region.end &&
	    prefix.size() <= region.end - fresh_address && region.end - region.begin >= 16 * 1024 &&
	    Loader::ReadGuestMemory(fresh_address, prefix.data(), prefix.size()) &&
	    std::all_of(prefix.begin(), prefix.end(), [](uint8_t value) { return value == 0; }),
	    "serial mutex init did not reuse cleared managed backing") && pass;
	if (!Check(Kernel::PthreadMutexLock(slot) == 0, "fresh guest mutex alias could not be locked"))
	{
		Kernel::PthreadMutexDestroy(slot);
		return false;
	}
	pass = Check(Kernel::PthreadMutexUnlock(slot) == 0, "fresh guest mutex alias could not be unlocked") && pass;
	pass = Check(Kernel::PthreadMutexDestroy(slot) == 0, "fresh guest mutex destroy failed") && pass;
	std::memcpy(&stable_handle, data.Data(), sizeof(stable_handle));
	return Check(stable_handle == 0 && Memory::QueryManagedRegion(fresh_address, &region) == Memory::ManagedRegionState::Visible &&
	             region.mode == Memory::Mode::ReadWrite,
	             "fresh mutex destroy lost its reusable guest page or retained its slot") && pass;
}

enum class Case { Float, Double, FloatReturn, DoubleReturn, Pointer, Mixed, Void, Gp, Legacy };

class CheckedVaReader
{
public:
	explicit CheckedVaReader(const Lumen::Libs::VaRegSave* registers): m_registers(registers)
	{
		Lumen::Libs::g_va_read_opaque = this;
		Lumen::Libs::g_va_read_error = &failed;
		Lumen::Libs::g_va_read_hook = [](void* opaque, const void* address, void* destination, size_t size, bool extended)
		{
			const auto& reader = *static_cast<CheckedVaReader*>(opaque);
			const auto at = reinterpret_cast<uint64_t>(address);
			const auto begin = reinterpret_cast<uint64_t>(reader.m_registers);
			if (extended || at < begin || at - begin > sizeof(*reader.m_registers) ||
			    size > sizeof(*reader.m_registers) - (at - begin)) return false;
			std::memcpy(destination, address, size);
			return true;
		};
	}
	~CheckedVaReader()
	{
		Lumen::Libs::g_va_read_hook = m_previous_hook;
		Lumen::Libs::g_va_read_opaque = m_previous_opaque;
		Lumen::Libs::g_va_read_error = m_previous_error;
	}
	bool failed = false;

private:
	const Lumen::Libs::VaRegSave* m_registers;
	Lumen::Libs::VaReadHook m_previous_hook = Lumen::Libs::g_va_read_hook;
	void* m_previous_opaque = Lumen::Libs::g_va_read_opaque;
	bool* m_previous_error = Lumen::Libs::g_va_read_error;
};

bool VariadicReadFailures(Loader::RuntimeLinker* linker)
{
	using namespace Lumen::Libs;
	std::array<char, 128> output {};
	alignas(16) VaContext context {};
	context.reg_save_area.gp[0] = reinterpret_cast<uint64_t>(output.data());
	context.reg_save_area.gp[1] = output.size();
	context.reg_save_area.gp[3] = 11;
	context.reg_save_area.gp[4] = 22;
	context.reg_save_area.gp[5] = 33;
	const double value = 1.25;
	for (auto& fp: context.reg_save_area.fp) std::memcpy(&fp, &value, sizeof(value));
	const auto reset = [&](const char* format)
	{
		output.fill(0x5a);
		context.reg_save_area.gp[2] = reinterpret_cast<uint64_t>(format);
		context.va_list = {0, 48, nullptr, &context.reg_save_area};
	};
	const auto untouched = [&]
	{
		return std::all_of(output.begin(), output.end(), [](char byte) { return byte == 0x5a; });
	};
	// An outer failed context must survive a nested formatter or marshaller
	// scope. Each inner scope starts with an independent, clear failure flag.
	CheckedVaReader outer(&context.reg_save_area);
	outer.failed = true;
	const auto outer_hook = g_va_read_hook;
	const auto outer_opaque = g_va_read_opaque;
	const auto outer_error = g_va_read_error;
	const auto restored = [&]
	{
		return g_va_read_hook == outer_hook && g_va_read_opaque == outer_opaque &&
		       g_va_read_error == outer_error && outer.failed;
	};
	for (const char* format: {"%f %f %f %f %f %f %f %f %f", "%d %d %d %s",
	                         "%d %d %d %*d", "%d %d %d %.*f", "%Lf"})
	{
		reset(format);
		{
			CheckedVaReader reader(&context.reg_save_area);
			if (!Check(GetSnrintfCtxFunc()(&context) == -1 && reader.failed && untouched(),
			           "failed variadic read dereferenced a pointer or committed output")) return false;
		}
		if (!Check(restored(), "failed formatter did not restore nested TLS reader")) return false;
	}
	for (const auto al: {0u, 9u})
	{
		reset("%f");
		Lumen::Cpu::X64State state {};
		state.gpr[0] = al;
		state.gpr[7] = context.reg_save_area.gp[0];
		state.gpr[6] = output.size();
		state.gpr[2] = context.reg_save_area.gp[2];
		if (!Check(!Loader::InvokeHostCall(linker->Symbols(), g_snprintf_target, &state) && untouched() && restored(),
		           "invalid AL or missing FP register escaped variadic admission")) return false;
	}
	reset("%Lf");
	Lumen::Cpu::X64State extended {};
	extended.gpr[7] = context.reg_save_area.gp[0];
	extended.gpr[6] = output.size();
	extended.gpr[2] = context.reg_save_area.gp[2];
	if (!Check(!Loader::InvokeHostCall(linker->Symbols(), g_snprintf_target, &extended) && untouched() && restored(),
	           "unsupported extended FP format committed output")) return false;
	reset("%d %.2f");
	{
		CheckedVaReader reader(&context.reg_save_area);
		if (!Check(GetSnrintfCtxFunc()(&context) == 7 && !reader.failed && std::strcmp(output.data(), "11 1.25") == 0,
		           "variadic error poisoned a later successful call")) return false;
	}
	return Check(restored(), "successful formatter did not restore nested TLS reader");
}

bool VariadicPointeePermissions(Loader::RuntimeLinker* linker, size_t page_size)
{
	Mapping data(page_size * 3);
	if (!data.Valid()) return false;
	const uint64_t output = data.Address();
	const uint64_t edge = data.Address(page_size * 2);
	if (!data.Protect(page_size * 2, page_size, Memory::Mode::NoAccess)) return false;
	const auto invoke = [&](uint64_t destination, size_t size, uint64_t format, uint64_t string,
	                        bool fault, bool write, int result = 0)
	{
		Lumen::Cpu::X64State state {};
		state.gpr[7] = destination; state.gpr[6] = size;
		state.gpr[2] = format; state.gpr[1] = string;
		const bool handled = Loader::InvokeHostCall(linker->Symbols(), g_snprintf_target, &state);
		return Check(handled && (state.memory_fault_size != 0) == fault &&
		             (fault ? state.memory_fault_write == write : state.gpr[0] == static_cast<uint64_t>(result)),
		             "variadic pointee permission/fault result");
	};
	const auto untouched = [&](size_t offset, size_t count)
	{
		return std::all_of(data.Data(offset), data.Data(offset + count), [](uint8_t value) { return value == 0x5a; });
	};
	std::memset(data.Data(), 0x5a, page_size * 2);
	const uint64_t text_format = reinterpret_cast<uint64_t>("prefix %s");
	if (!invoke(output, 128, edge, 0, true, false) || !untouched(0, 128) ||
	    !invoke(output, 128, text_format, edge, true, false) || !untouched(0, 128) ||
	    !invoke(output, 128, text_format, edge - 4, true, false) || !untouched(0, 128)) return false;
	// The consumed bytes end before the guard. No terminator or byte beyond
	// precision may be speculatively read, including when precision is zero.
	if (!invoke(output, 128, reinterpret_cast<uint64_t>("%.4s"), edge - 4, false, false, 4) ||
	    std::memcmp(data.Data(), "ZZZZ\0", 5) != 0 ||
	    !invoke(output, 128, reinterpret_cast<uint64_t>("%.0s"), edge, false, false, 0)) return false;
	std::memcpy(data.Data(page_size * 2 - 3), "%s", 3);
	if (!invoke(output, 128, edge - 3, reinterpret_cast<uint64_t>("ok"), false, false, 2) ||
	    std::strcmp(reinterpret_cast<char*>(data.Data()), "ok") != 0) return false;
	std::memset(data.Data(), 0x5a, page_size * 2);
	if (!invoke(output, 128, edge - 4, 0, true, false) || !untouched(0, 128) ||
	    !invoke(edge - 2, 128, reinterpret_cast<uint64_t>("abcd"), 0, true, true) ||
	    !untouched(page_size * 2 - 2, 2) ||
	    !invoke(UINT64_MAX - 1, 128, reinterpret_cast<uint64_t>("abcd"), 0, true, true)) return false;
	if (!data.Protect(0, page_size, Memory::Mode::Read) ||
	    !invoke(output, 128, reinterpret_cast<uint64_t>("abcd"), 0, true, true) || !untouched(0, 128)) return false;
	// Size zero performs a length query without borrowing the destination.
	return invoke(edge, 0, reinterpret_cast<uint64_t>("abcd"), 0, false, false, 4);
}

bool VariadicCase(size_t page_size, size_t limit)
{
	Mapping code(page_size), data(page_size * 2);
	if (!code.Valid() || !data.Valid()) return false;
	Registers vectors = Pattern();
	constexpr double values[] = {1.25, 2.5, 3.75, 4, 5, 6, 7, 8, 9};
	for (unsigned index = 0; index != 8; ++index) std::memcpy(vectors[index].data(), &values[index], 8);
	std::memcpy(data.Data(), vectors.data(), sizeof(vectors));
	std::memset(data.Data(page_size), 0x5a, page_size);
	constexpr char format[] = "%.2f %d %.2f %d %.2f %d %.2f %d %.2f %.2f %.2f %.2f %.2f";
	constexpr char expected[] = "1.25 11 2.50 22 3.75 33 4.00 44 5.00 6.00 7.00 8.00 9.00";
	GuestCode guest;
	guest.Stack(24);
	guest.Vectors(data.Address(), false);
	guest.Mov(7, limit == 0 ? 0 : data.Address(page_size));
	guest.Mov(6, limit); guest.Mov(2, reinterpret_cast<uint64_t>(format));
	guest.Mov(1, 11); guest.Mov(8, 22); guest.Mov(9, 33);
	guest.Spill(0, 44);
	uint64_t ninth {}; std::memcpy(&ninth, &values[8], sizeof(ninth));
	guest.Spill(8, ninth);
	guest.CallVariadic(g_snprintf_target, 8);
	guest.SaveResult(data.Address(1024));
	guest.Vectors(data.Address(512), true);
	guest.LoadResult(data.Address(1024));
	guest.Stack(24, true); guest.Return();
	if (!guest.Publish(code)) return false;
	if (!Check(Loader::RunGuestX64(code.Address()) == sizeof(expected) - 1, "variadic snprintf length") ||
	    !Check(std::memcmp(data.Data(512), vectors.data(), sizeof(vectors)) == 0, "variadic YMM preservation")) return false;
	const size_t written = limit == 0 ? 0 : std::min(limit - 1, sizeof(expected) - 1);
	return Check((limit == 0 || (std::memcmp(data.Data(page_size), expected, written) == 0 &&
	                           data.Data(page_size)[written] == 0)) &&
	             data.Data(page_size)[limit == 0 ? 0 : written + 1] == 0x5a,
	             "variadic values, truncation or destination bounds");
}

bool HostCallStateInitialization()
{
	using State = Lumen::Cpu::X64State;
	static_assert(std::is_standard_layout_v<State> && std::is_trivially_copyable_v<State>);
	const State ordinary {};
	const State bridge {State::HostCallOnly {}};
	const auto equal_array = [](const auto& left, const auto& right)
	{
		return std::equal(std::begin(left), std::end(left), std::begin(right));
	};
	// Inspect only initialized members of the tagged object. Never copy or
	// compare its complete representation or its six unused history arrays.
	bool equal = equal_array(ordinary.gpr, bridge.gpr) &&
	             std::memcmp(ordinary.xmm, bridge.xmm, sizeof(ordinary.xmm)) == 0 &&
	             ordinary.rip == bridge.rip && ordinary.rflags == bridge.rflags &&
	             ordinary.fs_base == bridge.fs_base && ordinary.gs_base == bridge.gs_base &&
	             ordinary.mxcsr == bridge.mxcsr && ordinary.x87_control_word == bridge.x87_control_word &&
	             equal_array(ordinary.x87, bridge.x87) && ordinary.x87_depth == bridge.x87_depth &&
	             equal_array(ordinary.instruction_trace, bridge.instruction_trace) &&
	             ordinary.instruction_trace_index == bridge.instruction_trace_index &&
	             ordinary.memory_fault_address == bridge.memory_fault_address &&
	             ordinary.memory_fault_size == bridge.memory_fault_size &&
	             ordinary.memory_fault_write == bridge.memory_fault_write &&
	             ordinary.last_data_read_rip == bridge.last_data_read_rip &&
	             ordinary.last_data_read_address == bridge.last_data_read_address &&
	             ordinary.last_data_read_value == bridge.last_data_read_value &&
	             ordinary.last_data_read_size == bridge.last_data_read_size &&
	             equal_array(ordinary.data_read_rip, bridge.data_read_rip) &&
	             equal_array(ordinary.data_read_address, bridge.data_read_address) &&
	             equal_array(ordinary.data_read_value, bridge.data_read_value) &&
	             equal_array(ordinary.data_read_size, bridge.data_read_size) &&
	             ordinary.data_read_index == bridge.data_read_index && ordinary.host_call_index == bridge.host_call_index &&
	             equal_array(ordinary.x87_instruction_rip, bridge.x87_instruction_rip) &&
	             ordinary.x87_instruction_count == bridge.x87_instruction_count &&
	             !ordinary.diagnostic_history_enabled && !bridge.diagnostic_history_enabled &&
	             !ordinary.execution_cancel_requested && !bridge.execution_cancel_requested;
	const auto& first = ordinary.host_call_stack_transaction;
	const auto& second = bridge.host_call_stack_transaction;
	equal = equal && first.original_rsp == second.original_rsp && first.slot_address == second.slot_address &&
	        first.slot_host_address == second.slot_host_address && first.original_slot_value == second.original_slot_value &&
	        !first.active && !second.active;
	for (size_t index = 0; index != std::size(ordinary.host_call_rip); ++index)
		equal = equal && ordinary.host_call_rip[index] == 0 && ordinary.host_call_target[index] == 0 &&
		        ordinary.host_call_result[index] == 0 && ordinary.host_call_arg0[index] == 0 &&
		        ordinary.host_call_arg1[index] == 0 && ordinary.host_call_arg2[index] == 0;
	return Check(equal, "host-call state changed ordinary defaults or default history initialization");
}

bool PendingFault(Loader::RuntimeLinker* linker)
{
	const auto invoke = [&](Lumen::Cpu::X64State& state)
	{
		state.gpr[0] = RETURN_TOKEN;
		state.gpr[7] = GP_VALUES[0];
		const auto registers = Pattern();
		std::memcpy(state.xmm, registers.data(), sizeof(state.xmm));
		state.memory_fault_address = 1;
		state.memory_fault_size = 4;
		g_calls = 0;
		return Check(Loader::InvokeHostCall(linker->Symbols(), reinterpret_cast<uint64_t>(&ScalarGp), &state) &&
		             g_calls == 0 && state.gpr[0] == RETURN_TOKEN && state.gpr[7] == GP_VALUES[0] &&
		             state.memory_fault_address == 1 && state.memory_fault_size == 4 &&
		             !state.host_call_stack_transaction.active && !state.execution_cancel_requested &&
		             std::memcmp(state.xmm, registers.data(), sizeof(state.xmm)) == 0,
		             "pending memory fault invoked the callee or changed argument/result state");
	};
	Lumen::Cpu::X64State ordinary {};
	Lumen::Cpu::X64State bridge {Lumen::Cpu::X64State::HostCallOnly {}};
	return invoke(ordinary) && invoke(bridge);
}

bool RunCase(size_t page_size, Case kind, uint64_t fp_bits = 0)
{
	Mapping code(page_size), data(page_size * 2);
	if (!Check(code.Valid() && data.Valid(), "execution fixture allocation failed")) return false;
	Registers before = Pattern();
	if (kind == Case::Float || kind == Case::Pointer) PutBits(&before, 0, fp_bits, 4);
	if (kind == Case::Double) PutBits(&before, 0, fp_bits, 8);
	if (kind == Case::Mixed)
		for (unsigned i = 0; i != 8; ++i) PutBits(&before, i, FP_VALUES[i], (i & 1) ? 4 : 8);
	Registers expected = before;
	// A different return pattern makes missing XMM0 writeback observable, while
	// preserving signed zero and NaN payloads without floating-point arithmetic.
	if (kind == Case::Float) PutBits(&expected, 0, fp_bits ^ 0x80000000U, 4);
	if (kind == Case::Double) PutBits(&expected, 0, fp_bits ^ 0x8000000000000000ULL, 8);
	if (kind == Case::FloatReturn) PutBits(&expected, 0, fp_bits, 4);
	if (kind == Case::DoubleReturn) PutBits(&expected, 0, fp_bits, 8);
	std::memcpy(data.Data(), before.data(), sizeof(before));
	std::memset(data.Data(page_size), 0xa7, page_size);
	const uint64_t output = data.Address(page_size);
	const uint64_t saved_result = output + sizeof(Registers);
	const uint64_t pointer_output = saved_result + 16;

	GuestCode guest;
	const unsigned stack_bytes = kind == Case::Mixed || kind == Case::Legacy ? 24 : 8;
	guest.Stack(stack_bytes);
	guest.Vectors(data.Address(), false);
	guest.GpArguments();
	uint64_t target = 0;
	uint64_t expected_result = RETURN_TOKEN;
	switch (kind)
	{
		case Case::Float: target = reinterpret_cast<uint64_t>(&FloatExchange); break;
		case Case::Double: target = reinterpret_cast<uint64_t>(&DoubleExchange); break;
		case Case::FloatReturn:
			target = reinterpret_cast<uint64_t>(&FloatFromGp);
			guest.Mov(7, fp_bits);
			break;
		case Case::DoubleReturn:
			target = reinterpret_cast<uint64_t>(&DoubleFromGp);
			guest.Mov(7, fp_bits);
			break;
		case Case::Void: target = reinterpret_cast<uint64_t>(&VoidCall); break;
		case Case::Gp: target = reinterpret_cast<uint64_t>(&ScalarGp); break;
		case Case::Pointer:
			target = reinterpret_cast<uint64_t>(&PointerFloat);
			guest.Mov(7, pointer_output);
			expected_result = static_cast<uint32_t>(-123456789);
			break;
		case Case::Mixed:
			target = reinterpret_cast<uint64_t>(&Mixed);
			// The ninth FP argument precedes the seventh GP argument in this
			// declaration: stack slots deliberately cross the two banks.
			guest.Spill(0, FP_VALUES[8]); guest.Spill(8, GP_VALUES[6]);
			break;
		case Case::Legacy:
			target = reinterpret_cast<uint64_t>(&LegacyGp);
			guest.Spill(0, GP_VALUES[6]); guest.Spill(8, GP_VALUES[7]);
			break;
	}
	guest.Call(target);
	const bool gp_return = kind == Case::Gp || kind == Case::Legacy || kind == Case::Mixed || kind == Case::Pointer;
	if (gp_return) guest.SaveResult(saved_result);
	guest.Vectors(output, true);
	if (gp_return) guest.LoadResult(saved_result); else guest.Mov(0, RETURN_TOKEN);
	guest.Stack(stack_bytes, true); guest.Return();
	if (!Check(guest.Publish(code), "guest code publication failed")) return false;
	g_calls = 0; g_fp_seen = 0; g_gp_seen = {}; g_mixed_fp_seen = {}; g_pointer_seen = 0;
	if (!Check(Loader::RunGuestX64(code.Address()) == expected_result && g_calls == 1,
	           "host callback count or GP result was wrong")) return false;
	if (!Check(std::memcmp(data.Data(page_size), expected.data(), sizeof(expected)) == 0,
	           "XMM scalar result bits, another XMM register or upper YMM sentinel changed")) return false;
	if ((kind == Case::Float || kind == Case::Double || kind == Case::Pointer) &&
	    !Check(g_fp_seen == fp_bits, "native callee received the wrong floating-point bits")) return false;
	if ((kind == Case::FloatReturn || kind == Case::DoubleReturn) &&
	    !Check(g_gp_seen[0] == fp_bits, "FP-return-only callee received the wrong GP argument")) return false;
	if (kind == Case::Pointer)
	{
		uint32_t stored = 0;
		std::memcpy(&stored, reinterpret_cast<const void*>(pointer_output), sizeof(stored));
		if (!Check(g_pointer_seen == pointer_output && stored == fp_bits &&
		           data.Data()[pointer_output - data.Address() - 1] == 0xa7 &&
		           data.Data()[pointer_output - data.Address() + 4] == 0xa7,
		           "pointer+float argument or destination bounds were wrong")) return false;
	}
	if (kind == Case::Mixed)
	{
		for (unsigned i = 0; i != 7; ++i)
			if (!Check(g_gp_seen[i] == GP_VALUES[i], "mixed GP argument/spill was wrong")) return false;
		if (!Check(g_mixed_fp_seen == FP_VALUES, "mixed FP argument/spill was wrong")) return false;
	}
	if (kind == Case::Legacy && !Check(g_gp_seen == GP_VALUES, "legacy eight-GP bridge regressed")) return false;
	if (kind == Case::Gp && !Check(g_gp_seen[0] == GP_VALUES[0], "typed GP argument was wrong")) return false;
	return true;
}

bool NoUnusedGpSpill(size_t page_size)
{
	for (unsigned arity: {0u, 1u, 6u})
	{
		Mapping code(page_size), stack(page_size * 2), data(page_size * 2);
		if (!Check(code.Valid() && stack.Valid() && data.Valid() &&
		           stack.Protect(page_size, page_size, Memory::Mode::NoAccess),
		           "unused GP spill fixture allocation failed")) return false;
		const Registers before = Pattern();
		std::memcpy(data.Data(), before.data(), sizeof(before));
		std::memset(data.Data(page_size), 0xa7, page_size);
		GuestCode guest;
		guest.Vectors(data.Address(), false);
		guest.GpArguments();
		guest.Bytes({0x49, 0x89, 0xe4}); // mov r12, rsp: preserve the real root stack.
		guest.Mov(4, stack.Address(page_size));
		// CALL/return use the last readable qword; every unused spill is in
		// the protected page. GP-only metadata must not read any of them.
		guest.Call(arity == 0 ? reinterpret_cast<uint64_t>(&VoidCall) :
		           arity == 1 ? reinterpret_cast<uint64_t>(&ScalarGp) : reinterpret_cast<uint64_t>(&ScalarGpSix));
		guest.Bytes({0x4c, 0x89, 0xe4}); // mov rsp, r12
		guest.Vectors(data.Address(page_size), true);
		guest.Mov(0, RETURN_TOKEN);
		guest.Return();
		if (!Check(guest.Publish(code), "unused GP spill code publication failed")) return false;
		g_calls = 0; g_gp_seen = {};
		if (!Check(Loader::RunGuestX64(code.Address()) == RETURN_TOKEN && g_calls == 1,
		           "GP-only host call read an unused inaccessible stack spill") ||
		    !Check(std::memcmp(data.Data(page_size), before.data(), sizeof(before)) == 0,
		           "GP-only host call changed XMM or upper YMM sentinels")) return false;
		for (unsigned index = 0; index < arity; ++index)
			if (!Check(g_gp_seen[index] == GP_VALUES[index], "GP-only register argument changed")) return false;
	}
	return true;
}

bool NestedOwnedRootStacks(size_t page_size)
{
	Mapping code(page_size);
	if (!Check(code.Valid(), "nested stack fixture allocation")) return false;
	GuestCode guest;
	// Nested Runs receive an intentionally unaligned buffer in the oldest
	// still-live ancestor stack. Locked xadd must retain that owner's proof.
	guest.Bytes({0x48, 0x85, 0xff, 0x74, 0x0d, 0x48, 0xc7, 0xc0, 1, 0, 0, 0,
	             0xf0, 0x48, 0x0f, 0xc1, 0x47, 1});
	guest.Stack(40);
	guest.Spill(0, 0); guest.Spill(8, 0);
	guest.Bytes({0x48, 0x89, 0xe7}); // mov rdi, rsp
	guest.Call(reinterpret_cast<uint64_t>(&NestedRoot));
	guest.Stack(40, true); guest.Return();
	if (!Check(guest.Publish(code), "nested stack code publication")) return false;
	g_nested_entry = code.Address();
	g_active_root_stacks = {};
	g_nested_depth = 0;
	g_nested_stack_valid = true;
	g_owned_stack_read_valid = true;
	g_calls = 0;
	const auto result = Loader::RunGuestX64(code.Address());
	if (g_owned_stack_read_test)
	{
		const uint64_t inactive = RETURN_TOKEN;
		g_owned_stack_read_valid = CheckOwnedStackRead(reinterpret_cast<uint64_t>(&inactive), 8, false) && g_owned_stack_read_valid;
	}
	return Check(result == RETURN_TOKEN && g_calls == 4 && g_nested_stack_valid && g_owned_stack_read_valid && g_nested_depth == 0 &&
	             g_active_root_stacks == std::array<uint64_t, 4>{},
	             "four nested owned stacks retain outer lifetime and unwind without destructive VM upgrades");
}

bool FreshCallbackVmRelease(size_t page_size)
{
	Mapping parent_code(page_size), child_code(page_size), data(page_size);
	if (!Check(parent_code.Valid() && child_code.Valid() && data.Valid(),
	           "fresh callback fixture allocation failed")) return false;
	g_fresh_callback = {};
	g_fresh_callback.entry = child_code.Address();
	g_fresh_callback.object = data.Address(128);
	g_fresh_callback.buffer = Memory::Alloc(0, page_size, Memory::Mode::ReadWrite);
	if (!Check(g_fresh_callback.buffer != 0, "fresh callback release buffer allocation failed")) return false;
	struct BufferCleanup
	{
		~BufferCleanup() { if (!g_fresh_callback.buffer_freed) Memory::Free(g_fresh_callback.buffer); }
	} buffer_cleanup;
	constexpr uint64_t parent_canary = 0x706172656e745f63ULL;
	const std::array<uint64_t, 2> object {FRESH_CALLBACK_OBJECT, 0};
	std::memcpy(data.Data(128), object.data(), sizeof(object));
	const Registers vectors = Pattern();
	std::memcpy(data.Data(512), vectors.data(), sizeof(vectors));
	std::memset(data.Data(1024), 0xa7, sizeof(vectors));
	GuestCode child;
	child.Stack(8);
	child.Mov(0, FRESH_CALLBACK_ENTERED);
	child.SaveResult(g_fresh_callback.object + sizeof(uint64_t));
	// The child forwards all three incoming SysV arguments unchanged.
	child.Call(reinterpret_cast<uint64_t>(&FreshCallbackRelease));
	child.Stack(8, true); child.Return();
	GuestCode parent;
	parent.Stack(24); parent.Spill(0, parent_canary);
	parent.Vectors(data.Address(512), false);
	parent.Mov(7, g_fresh_callback.object); parent.Mov(6, g_fresh_callback.buffer);
	parent.Mov(2, FRESH_CALLBACK_ARGUMENT);
	parent.Call(reinterpret_cast<uint64_t>(&FreshCallbackRoot));
	parent.SaveResult(data.Address());
	parent.Bytes({0x48, 0x8b, 0x04, 0x24}); // reload the parent's local after fresh callback return
	parent.SaveResult(data.Address(8));
	parent.Vectors(data.Address(1024), true);
	parent.LoadResult(data.Address());
	parent.Stack(24, true); parent.Return();
	if (!Check(child.Publish(child_code) && parent.Publish(parent_code),
	           "fresh callback guest code publication failed")) return false;
	const auto result = Loader::RunGuestX64(parent_code.Address());
	std::array<uint64_t, 2> observed {};
	std::memcpy(observed.data(), data.Data(), sizeof(observed));
	Memory::ManagedRegion retired {};
	bool pass = Check(result == FRESH_CALLBACK_RESULT &&
	    observed == std::array<uint64_t, 2> {FRESH_CALLBACK_RESULT, parent_canary} &&
	    g_fresh_callback.outer_calls == 1 && g_fresh_callback.release_calls == 1 &&
	    g_fresh_callback.arguments_valid && g_fresh_callback.borrow_rejected &&
	    g_fresh_callback.called && g_fresh_callback.buffer_freed &&
	    std::memcmp(data.Data(1024), vectors.data(), sizeof(vectors)) == 0 &&
	    Memory::QueryManagedRegion(g_fresh_callback.buffer, &retired) != Memory::ManagedRegionState::Visible,
	    "fresh callback lost scalar inputs/parent frame, executed a borrowed child, or failed VM retirement");
	if (!pass)
	{
		std::fprintf(stderr,
		    "HostCallScalarTest: fresh callback diagnostic result=%016llx observed_result=%016llx "
		    "observed_canary=%016llx outer_calls=%u release_calls=%u args_valid=%u "
		    "borrow_rejected=%u called=%u buffer_freed=%u\n",
		    static_cast<unsigned long long>(result), static_cast<unsigned long long>(observed[0]),
		    static_cast<unsigned long long>(observed[1]), g_fresh_callback.outer_calls, g_fresh_callback.release_calls,
		    static_cast<unsigned>(g_fresh_callback.arguments_valid), static_cast<unsigned>(g_fresh_callback.borrow_rejected),
		    static_cast<unsigned>(g_fresh_callback.called), static_cast<unsigned>(g_fresh_callback.buffer_freed));
	}
	// A remaining borrow or reader would prevent this ordinary root-side epoch.
	const uint64_t probe = Memory::Alloc(0, page_size, Memory::Mode::ReadWrite);
	pass = Check(probe != 0 && Memory::Free(probe), "fresh callback leaked a lease after parent return") && pass;
	return pass;
}

enum class FreshParentChange { None, StackProtect, TlsProtect, CodeProtect, StackReuse, TlsReuse, CodeReuse };

struct FreshNativeFixture
{
	Mapping* parent_code;
	Mapping* child_code;
	Mapping* parent_stack;
	Mapping* child_stack;
	Mapping* tls;
	Mapping* output;
	FreshParentChange change;
	uint64_t independent_buffer = 0;
	unsigned parent_calls = 0;
	unsigned child_calls = 0;
	bool valid = true;
	bool buffer_freed = false;
	bool restored = false;
	bool child_refused = false;
	bool revocation_observed = false;
};

void FreshNativeTarget() {}

uint32_t FreshNativeExecutable(void* opaque, uint64_t address, uint64_t* base,
                               uint64_t* size, uint32_t* writable) noexcept
{
	const auto& fixture = *static_cast<FreshNativeFixture*>(opaque);
	for (const Mapping* code: {fixture.parent_code, fixture.child_code})
	{
		if (address < code->Address() || address - code->Address() >= code->Size()) continue;
		Memory::ManagedRegion region {};
		if (Memory::QueryManagedRegion(address, &region) != Memory::ManagedRegionState::Visible ||
		    region.mode != Memory::Mode::ExecuteRead) return 0;
		*base = code->Address(); *size = code->Size(); *writable = 0;
		return 1;
	}
	return 0;
}

uint32_t FreshNativeHostCall(void* opaque, uint64_t target, uint64_t* gprs,
                            LumenAndroidFexCoreSimdViewV2*, uint64_t* next) noexcept
{
	auto& fixture = *static_cast<FreshNativeFixture*>(opaque);
	if (target != reinterpret_cast<uint64_t>(&FreshNativeTarget) || gprs == nullptr || next == nullptr)
		return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	const auto rsp = gprs[4];
	Memory::GuestDataLease return_slot;
	if (!Memory::TryAcquireGuestDataLease(rsp, sizeof(uint64_t), Memory::Mode::Read, &return_slot))
		return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	uint64_t return_rip = 0;
	std::memcpy(&return_rip, reinterpret_cast<const void*>(rsp), sizeof(return_rip));
	Memory::ReleaseGuestDataLease(&return_slot);
	if (gprs[7] == 2)
	{
		++fixture.child_calls;
		fixture.valid = fixture.valid && fixture.buffer_freed && gprs[6] == FRESH_CALLBACK_ARGUMENT;
		*next = return_rip; gprs[4] += 8; gprs[0] = FRESH_CALLBACK_RESULT;
		return fixture.valid ? LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1 : LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	}
	++fixture.parent_calls;
	fixture.valid = fixture.valid && gprs[7] == 1 && gprs[6] == FRESH_CALLBACK_ARGUMENT &&
	    LumenAndroidFexCoreBeginFreshGuestCallbackV1(0, 0, 0, 0) == 0;
	{
		Memory::GuestDataLease borrowed;
		fixture.valid = Memory::TryAcquireGuestDataLease(fixture.output->Address(), 8, Memory::Mode::Read, &borrowed) &&
		    fixture.valid;
		const bool retained = LumenAndroidFexCoreRetainGuestDataLeaseV1() != 0;
		fixture.valid = retained && fixture.valid;
		fixture.valid = LumenAndroidFexCoreBeginFreshGuestCallbackV1(fixture.parent_stack->Address(),
		    fixture.parent_stack->Size(), fixture.tls->Address(), fixture.tls->Size()) == 0 && fixture.valid;
		if (retained) fixture.valid = LumenAndroidFexCoreReleaseGuestDataLeaseV1() != 0 && fixture.valid;
	}
	Mapping* changed = nullptr;
	bool reuse = false;
	switch (fixture.change)
	{
		case FreshParentChange::None: break;
		case FreshParentChange::StackProtect: changed = fixture.parent_stack; break;
		case FreshParentChange::TlsProtect: changed = fixture.tls; break;
		case FreshParentChange::CodeProtect: changed = fixture.parent_code; break;
		case FreshParentChange::StackReuse: changed = fixture.parent_stack; reuse = true; break;
		case FreshParentChange::TlsReuse: changed = fixture.tls; reuse = true; break;
		case FreshParentChange::CodeReuse: changed = fixture.parent_code; reuse = true; break;
	}
	// Copy bytes before donation. Reuse restores identical bytes and rights, so
	// a fresh permission query alone cannot conceal the retired parent lifetime.
	std::vector<uint8_t> snapshot;
	if (reuse) snapshot.assign(changed->Data(), changed->Data(changed->Size()));
	const auto token = LumenAndroidFexCoreBeginFreshGuestCallbackV1(fixture.parent_stack->Address(),
	    fixture.parent_stack->Size(), fixture.tls->Address(), fixture.tls->Size());
	if (token == 0) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
	fixture.valid = LumenAndroidFexCoreEndFreshGuestCallbackV1(token + 1) == 0 && fixture.valid;
	bool foreign_thread_denied = false;
	std::thread foreign([&] { foreign_thread_denied = LumenAndroidFexCoreEndFreshGuestCallbackV1(token) == 0; });
	foreign.join();
	fixture.valid = foreign_thread_denied && fixture.valid;
	if (changed != nullptr)
	{
		const auto mode = changed == fixture.parent_code ? Memory::Mode::ExecuteRead : Memory::Mode::ReadWrite;
		if (reuse)
		{
			fixture.valid = Memory::Free(changed->Address()) && fixture.valid;
			const bool replaced = Memory::AllocFixed(changed->Address(), changed->Size(), Memory::Mode::ReadWrite);
			fixture.valid = replaced && fixture.valid;
			if (replaced)
			{
				std::memcpy(changed->Data(), snapshot.data(), snapshot.size());
				fixture.valid = changed->Protect(0, changed->Size(), mode) && fixture.valid;
			}
		} else fixture.valid = changed->Protect(0, changed->Size(), mode) && fixture.valid;
		Memory::ManagedRegion replacement {};
		fixture.valid = Memory::QueryManagedRegion(changed->Address(), &replacement) == Memory::ManagedRegionState::Visible &&
		    replacement.mode == mode && fixture.valid;
	}
	else
	{
		// An independent native writer can drain the donated HLE reader. Its
		// completed publication is observed by the child's first HLE callback.
		std::thread writer([&] { fixture.buffer_freed = Memory::Free(fixture.independent_buffer); });
		writer.join();
		fixture.valid = fixture.buffer_freed && fixture.valid;
	}
	const auto epoch = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
	fixture.valid = epoch != 0 && fixture.valid;
	if (epoch != 0)
	{
		const bool parent_valid = LumenAndroidFexCoreValidateFreshGuestCallbackParentV1() != 0;
		fixture.revocation_observed = changed != nullptr && !parent_valid;
		fixture.valid = parent_valid == (changed == nullptr) && fixture.valid;
		LumenAndroidFexCoreEndGuestMemoryMutationV1(epoch, 0, 0, 0);
	}
	LumenAndroidFexCoreGuestStateV1 child {};
	child.structSize = sizeof(child); child.abiVersion = 1;
	child.rip = fixture.child_code->Address(); child.fsBase = fixture.tls->Address(16);
	child.gprs[4] = fixture.child_stack->Address(fixture.child_stack->Size() - 8);
	child.gprs[7] = 2; child.gprs[6] = FRESH_CALLBACK_ARGUMENT;
	const auto root_return = LumenAndroidFexCoreRootReturnAddressV1();
	std::memcpy(reinterpret_cast<void*>(child.gprs[4]), &root_return, sizeof(root_return));
	const auto child_status = LumenAndroidFexCoreRunGuestV2(&child, FreshNativeHostCall,
	    FreshNativeExecutable, &fixture);
	fixture.child_refused = child_status == LUMEN_ANDROID_FEXCORE_RUNTIME_MUTATION_BARRIER_DENIED_V1 &&
	    fixture.child_calls == 0;
	fixture.valid = (changed != nullptr ? fixture.child_refused :
	    child_status == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 && child.gprs[0] == FRESH_CALLBACK_RESULT &&
	        fixture.child_calls == 1) && fixture.valid;
	fixture.restored = LumenAndroidFexCoreEndFreshGuestCallbackV1(token) != 0;
	fixture.valid = fixture.restored == (changed == nullptr) &&
	    LumenAndroidFexCoreEndFreshGuestCallbackV1(token) == 0 && fixture.valid;
	// The return slot was copied under the original reader. Revocation must
	// prevent this deliberately requested CONTINUE before any parent instruction.
	*next = return_rip; gprs[4] += 8; gprs[0] = FRESH_CALLBACK_RESULT;
	return fixture.valid ? LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1 : LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
}

bool FreshCallbackNativeProtocol(size_t page_size)
{
	if (Lumen::GuestCpu::Selected() != Lumen::GuestCpu::Provider::Fex) return true;
	if (!Check(LumenAndroidFexCoreRegisterHostCallTargetV1(reinterpret_cast<uint64_t>(&FreshNativeTarget)) == 1,
	           "fresh native target registration failed")) return false;
	for (const auto change: {FreshParentChange::None, FreshParentChange::StackProtect, FreshParentChange::TlsProtect,
	    FreshParentChange::CodeProtect, FreshParentChange::StackReuse, FreshParentChange::TlsReuse,
	    FreshParentChange::CodeReuse})
	{
		Mapping parent_code(page_size), child_code(page_size), parent_stack(page_size), child_stack(page_size),
		    tls(page_size), output(page_size);
		if (!Check(parent_code.Valid() && child_code.Valid() && parent_stack.Valid() && child_stack.Valid() &&
		    tls.Valid() && output.Valid(), "fresh native fixture allocation failed")) return false;
		FreshNativeFixture fixture {&parent_code, &child_code, &parent_stack, &child_stack, &tls, &output, change};
		fixture.independent_buffer = Memory::Alloc(0, page_size, Memory::Mode::ReadWrite);
		if (!Check(fixture.independent_buffer != 0, "fresh native independent buffer allocation failed")) return false;
		constexpr uint64_t canary = 0x706172656e745f63ULL;
		std::memcpy(tls.Data(16), &canary, sizeof(canary));
		std::memset(output.Data(), 0, output.Size());
		GuestCode child;
		child.Stack(8); child.Call(reinterpret_cast<uint64_t>(&FreshNativeTarget)); child.Stack(8, true); child.Return();
		GuestCode parent;
		parent.Stack(24); parent.Spill(0, canary);
		parent.Mov(7, 1); parent.Mov(6, FRESH_CALLBACK_ARGUMENT);
		parent.Call(reinterpret_cast<uint64_t>(&FreshNativeTarget)); parent.SaveResult(output.Address());
		parent.Bytes({0x48, 0x8b, 0x04, 0x24}); parent.SaveResult(output.Address(8));
		parent.Bytes({0x64, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0}); // mov rax,fs:[0]
		parent.SaveResult(output.Address(16));
		parent.LoadResult(output.Address()); parent.Stack(24, true); parent.Return();
		if (!Check(parent.Publish(parent_code) && child.Publish(child_code), "fresh native code publication failed")) return false;
		LumenAndroidFexCoreGuestStateV1 state {};
		state.structSize = sizeof(state); state.abiVersion = 1; state.rip = parent_code.Address();
		state.fsBase = tls.Address(16); state.gprs[4] = parent_stack.Address(page_size - 8);
		const auto root_return = LumenAndroidFexCoreRootReturnAddressV1();
		std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &root_return, sizeof(root_return));
		const auto status = LumenAndroidFexCoreRunGuestV2(&state, FreshNativeHostCall, FreshNativeExecutable, &fixture);
		std::array<uint64_t, 3> observed {};
		std::memcpy(observed.data(), output.Data(), sizeof(observed));
		const bool success = change == FreshParentChange::None;
		const bool valid = fixture.valid && fixture.parent_calls == 1 && (success ?
		    status == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 && state.gprs[0] == FRESH_CALLBACK_RESULT &&
		        observed == std::array<uint64_t, 3>{FRESH_CALLBACK_RESULT, canary, canary} && fixture.restored &&
		        state.fsBase == tls.Address(16) :
		    status == LUMEN_ANDROID_FEXCORE_RUNTIME_MUTATION_BARRIER_DENIED_V1 &&
		        observed == std::array<uint64_t, 3>{} && fixture.revocation_observed && !fixture.restored && fixture.child_refused);
		if (!fixture.buffer_freed) Memory::Free(fixture.independent_buffer);
		if (!valid) std::fprintf(stderr, "HostCallScalarTest: fresh native case=%u status=%u parent=%u child=%u "
		    "valid=%u restored=%u revoked=%u refused=%u\n", static_cast<unsigned>(change), status,
		    fixture.parent_calls, fixture.child_calls, fixture.valid, fixture.restored,
		    fixture.revocation_observed, fixture.child_refused);
		if (!Check(valid, "fresh native donation, token affinity or parent lifetime contract failed")) return false;
	}
	return true;
}


int BadSpill(size_t page_size, bool variadic = false)
{
	Mapping code(page_size), stack(page_size * 2);
	if (!code.Valid() || !stack.Valid() || !stack.Protect(page_size, page_size, Memory::Mode::NoAccess)) return 1;
	GuestCode guest;
	// CALL can push a return address into the last eight writable bytes. Only
	// the first spilled argument (at callee RSP+8) is inaccessible.
	guest.Mov(4, stack.Address(page_size));
	if (variadic)
	{
		static char output[128];
		static const char format[] = "%f %f %f %f %f %f %f %f %f";
		guest.Mov(7, reinterpret_cast<uint64_t>(output)); guest.Mov(6, sizeof(output));
		guest.Mov(2, reinterpret_cast<uint64_t>(format));
		guest.CallVariadic(g_snprintf_target, 8);
	} else guest.Call(reinterpret_cast<uint64_t>(&Mixed));
	guest.Bytes({0x0f, 0x0b});
	if (!guest.Publish(code)) return 1;
	g_bad_spill = true;
	Loader::RunGuestX64(code.Address());
	std::fputs("HostCallScalarTest: invalid spill returned\n", stderr);
	return 3;
}

bool RejectedSubprocess(const char* executable, const char* mode = "--bad-spill")
{
	char path[] = "/tmp/lumen-scalar-stderr-XXXXXX";
	const int output = mkstemp(path);
	if (!Check(output >= 0, "child stderr file failed")) return false;
	unlink(path);
	posix_spawn_file_actions_t actions;
	int error = posix_spawn_file_actions_init(&actions);
	if (error != 0) { close(output); return false; }
	error = posix_spawn_file_actions_adddup2(&actions, output, STDERR_FILENO);
	if (error == 0) error = posix_spawn_file_actions_addclose(&actions, output);
	if (error == 0) error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
	char* arguments[] = {const_cast<char*>(executable), const_cast<char*>(mode), nullptr};
	pid_t child = -1;
	if (error == 0) error = posix_spawn(&child, executable, &actions, nullptr, arguments, environ);
	posix_spawn_file_actions_destroy(&actions);
	if (error != 0) { close(output); return Check(false, "bad-spill child spawn failed"); }
	int status = 0;
	bool finished = false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
	while (std::chrono::steady_clock::now() < deadline)
	{
		const pid_t waited = waitpid(child, &status, WNOHANG);
		if (waited == child) { finished = true; break; }
		if (waited < 0 && errno != EINTR) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	if (!finished) { kill(child, SIGKILL); while (waitpid(child, &status, 0) < 0 && errno == EINTR) {} }
	std::array<char, 8193> log {};
	const off_t length = lseek(output, 0, SEEK_END);
	if (length >= 0) lseek(output, length > 8192 ? length - 8192 : 0, SEEK_SET);
	const ssize_t count = read(output, log.data(), log.size() - 1);
	close(output);
	const bool passed = finished && WIFEXITED(status) && WEXITSTATUS(status) == 74 && count > 0 &&
	                    std::strstr(log.data(), "[MACOS_FEXCORE_FAIL_CLOSED_V1] code=8 exit=74") != nullptr &&
	                    std::strstr(log.data(), "bad-spill callback invoked") == nullptr;
	if (!passed) std::fprintf(stderr, "HostCallScalarTest: bad spill rejected incorrectly (status=%d)\n%s", status, log.data());
	return passed;
}
} // namespace

int main(int argc, char** argv)
{
	const bool variadic_child = argc == 2 && std::strcmp(argv[1], "--bad-variadic-spill") == 0;
	const bool child = variadic_child || (argc == 2 && std::strcmp(argv[1], "--bad-spill") == 0);
	g_owned_stack_read_enabled = argc == 2 && std::strcmp(argv[1], "--owned-stack-read-enabled") == 0;
	g_owned_stack_read_test = g_owned_stack_read_enabled ||
	    (argc == 2 && std::strcmp(argv[1], "--owned-stack-read-disabled") == 0);
	if (argc != 1 && !child && !g_owned_stack_read_test) return 2;
	Watchdog watchdog(child ? 10 : 35);
	if (setenv("LUMEN_MACOS_FEXCORE_RUN", "1", 1) != 0) return 1;
	if (!child)
	{
		std::array<char, 4096> executable {};
		uint32_t size = executable.size();
		if (_NSGetExecutablePath(executable.data(), &size) != 0 || !RejectedSubprocess(executable.data()) ||
		    !RejectedSubprocess(executable.data(), "--bad-variadic-spill")) return 1;
	}
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Memory::Init();
	Lumen::Libs::LibKernel::PthreadSubsystem::Instance()->Init(nullptr);
	Lumen::Libs::LibKernel::PthreadInitSelfForMainThread();
	LumenAndroidFexCoreRuntimeStatusV1 status {};
	if (!Check(LumenAndroidFexCoreRuntimeProbeV1(&status) == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
	           status.backendRouteReady != 0, "real FEX runtime unavailable")) return 1;
	const long size = sysconf(_SC_PAGESIZE);
	if (size <= 0) return 1;
	const size_t page_size = static_cast<size_t>(size);
	auto* linker = Lumen::Core::Singleton<Loader::RuntimeLinker>::Instance();
	if (!Initialize(linker)) { linker->Clear(); return 1; }
	if (child) return BadSpill(page_size, variadic_child);
	bool passed = HostCallStateInitialization();
	passed = PureClockCalls(linker, page_size) && passed;
	passed = PurePthreadCalls(linker, page_size) && passed;
	passed = ManagedLibObjectImports(page_size) && passed;
	passed = SanitizerReplacementObjects(linker, page_size) && passed;
	passed = HeapTraceObjects(linker, page_size) && passed;
	passed = GuestEntryParameters(page_size) && passed;
	passed = ManagedThreadAbi(linker, page_size) && passed;
	passed = GuestPthreadCreate(page_size) && passed;
	passed = GuestPeerThreadStack(page_size) && passed;
	passed = GuestMutexAbi(page_size) && passed;
#if defined(LUMEN_FEXCORE_RUNTIME)
	// This optional diagnostic examines private FEX snapshot internals. The
	// guest/HLE cases below exercise the common production ABI for either CPU.
	if (Lumen::GuestCpu::Selected() == Lumen::GuestCpu::Provider::Fex)
		passed = Check(LumenAndroidFexCoreHostTargetSnapshotContractTestV1() == 1,
		               "immutable host-target snapshot contract") && passed;
#endif
	passed = PendingFault(linker) && passed;
	passed = VariadicReadFailures(linker) && passed;
	passed = VariadicPointeePermissions(linker, page_size) && passed;
	for (size_t limit: {128u, 8u, 0u}) passed = VariadicCase(page_size, limit) && passed;
	for (uint64_t bits: {uint64_t(0x3fa00000), uint64_t(0x80000000), uint64_t(0x7fc54321)})
	{
		passed = RunCase(page_size, Case::Float, bits) && passed;
		passed = RunCase(page_size, Case::FloatReturn, bits) && passed;
	}
	for (uint64_t bits: {0x400921fb54442d18ULL, 0x8000000000000000ULL, 0x7ff8123456789abcULL})
	{
		passed = RunCase(page_size, Case::Double, bits) && passed;
		passed = RunCase(page_size, Case::DoubleReturn, bits) && passed;
	}
	passed = RunCase(page_size, Case::Pointer, 0x3fa00000) && passed;
	passed = RunCase(page_size, Case::Mixed) && passed;
	passed = RunCase(page_size, Case::Void) && passed;
	passed = RunCase(page_size, Case::Gp) && passed;
	passed = RunCase(page_size, Case::Legacy) && passed;
	passed = NoUnusedGpSpill(page_size) && passed;
	passed = NestedOwnedRootStacks(page_size) && passed;
	passed = FreshCallbackVmRelease(page_size) && passed;
	passed = FreshCallbackNativeProtocol(page_size) && passed;
	linker->Clear();
	Memory::ManagedRegion retired_tls {};
	passed = Check(g_main_tcb != 0 &&
	    Memory::QueryManagedRegion(g_main_tcb, &retired_tls) != Memory::ManagedRegionState::Visible,
	    "session teardown retained guest TLS backing") && passed;
	if (!passed) return 1;
	std::puts("HostCallScalarTest: PASS");
	return 0;
}
