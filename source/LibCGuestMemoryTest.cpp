#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"
#include "Emulator/Config.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/HostCallScalar.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "CpuX64.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <unistd.h>

namespace {
namespace Loader = Lumen::Loader;
namespace Memory = Lumen::Core::VirtualMemory;

enum class Kind { Copy, Move, Compare, Set, Find, Length, StringCompare, StringCompareN,
                  StringCopy, StringCopyN, Append, First, Last, Search, Span, CSpan, Any };
constexpr std::array<const char32_t*, 17> NIDS {
    U"Q3VBxCXhUHs", U"+P6FRGH4LfA", U"DfivPArhucg", U"8zTFvBIAIN8", U"8u8lPzUEq+U",
    U"j4ViWNHEgww", U"Ovb2dSJOAuE", U"aesyjrHVWy4", U"kiZSXIWd9vg", U"6sJWiWSRuqk",
    U"Ls4tzzhimqQ", U"ob5xAW4ln-0", U"9yDWMxEFdJU", U"viiwFMaNamA", U"-kU6bB4M-+k",
    U"q0F6yS-rCms", U"kDZvoVssCgQ"};
std::array<uint64_t, NIDS.size()> g_targets {};
Kind g_kind = Kind::Copy;
Loader::SymbolDatabase* g_fault_symbols = nullptr;
bool g_nested_fault_ok = false;

bool Check(bool value, const char* message)
{
	if (!value) std::fprintf(stderr, "LibCGuestMemoryTest: %s\n", message);
	return value;
}

class Mapping
{
public:
	explicit Mapping(size_t size): m_address(Memory::Alloc(0, size, Memory::Mode::ReadWrite)) {}
	~Mapping() { if (m_address != 0) Memory::Free(m_address); }
	uint64_t Address(size_t offset = 0) const { return m_address + offset; }
	char* Data(size_t offset = 0) const { return reinterpret_cast<char*>(Address(offset)); }
	bool Valid() const { return m_address != 0; }
private:
	uint64_t m_address;
};

// Typed calls exercise the actual registered libc exports while InvokeHostCall
// supplies the ordinary scoped HLE fault state. This gate does not claim a Run
// through the translator or support for mixed host-page permissions.
uint64_t LUMEN_SYSV_ABI Dispatch(uint64_t a, uint64_t b, uint64_t c)
{
	const auto target = g_targets[static_cast<size_t>(g_kind)];
	auto* first = reinterpret_cast<char*>(a);
	const auto* second = reinterpret_cast<const char*>(b);
	switch (g_kind)
	{
		case Kind::Copy:
		case Kind::Move:
			return reinterpret_cast<uint64_t>(reinterpret_cast<void* (*)(void*, const void*, size_t)>(target)(first, second, c));
		case Kind::Compare:
			return static_cast<uint32_t>(reinterpret_cast<int (*)(const void*, const void*, size_t)>(target)(first, second, c));
		case Kind::Set:
			return reinterpret_cast<uint64_t>(reinterpret_cast<void* (*)(void*, int, size_t)>(target)(first, b, c));
		case Kind::Find:
			return reinterpret_cast<uint64_t>(reinterpret_cast<void* (*)(const void*, int, size_t)>(target)(first, b, c));
		case Kind::Length:
			return reinterpret_cast<size_t (*)(const char*)>(target)(first);
		case Kind::StringCompare:
			return static_cast<uint32_t>(reinterpret_cast<int (*)(const char*, const char*)>(target)(first, second));
		case Kind::StringCompareN:
			return static_cast<uint32_t>(reinterpret_cast<int (*)(const char*, const char*, size_t)>(target)(first, second, c));
		case Kind::StringCopyN:
			return reinterpret_cast<uint64_t>(reinterpret_cast<char* (*)(char*, const char*, size_t)>(target)(first, second, c));
		case Kind::StringCopy:
		case Kind::Append:
			return reinterpret_cast<uint64_t>(reinterpret_cast<char* (*)(char*, const char*)>(target)(first, second));
		case Kind::First:
		case Kind::Last:
			return reinterpret_cast<uint64_t>(reinterpret_cast<char* (*)(const char*, int)>(target)(first, b));
		case Kind::Search:
		case Kind::Any:
			return reinterpret_cast<uint64_t>(reinterpret_cast<char* (*)(const char*, const char*)>(target)(first, second));
		case Kind::Span:
		case Kind::CSpan:
			return reinterpret_cast<size_t (*)(const char*, const char*)>(target)(first, second);
	}
	return 0;
}

uint64_t LUMEN_SYSV_ABI NestedFault()
{
	Loader::ReportGuestMemoryFault(0x2211, 3, true);
	Loader::ReportGuestMemoryFault(0x3300, 4, false);
	return 0x55;
}

uint64_t LUMEN_SYSV_ABI OuterFault(uint64_t first)
{
	if (first != 0) Loader::ReportGuestMemoryFault(0x1100, 8, false);
	Lumen::Cpu::X64State nested {};
	g_nested_fault_ok = Loader::InvokeHostCall(g_fault_symbols, reinterpret_cast<uint64_t>(&NestedFault), &nested) &&
	                    nested.memory_fault_address == 0x2211 && nested.memory_fault_size == 3 && nested.memory_fault_write;
	Loader::ReportGuestMemoryFault(0x4400, 16, true);
	return 0x77;
}

class Calls
{
public:
	bool Initialize()
	{
		if (!Lumen::Libs::Init(U"libc_internal_1", &symbols)) return false;
		for (size_t index = 0; index < NIDS.size(); ++index)
		{
			const Loader::SymbolResolve name {NIDS[index], U"LibcInternal", 1, U"LibcInternal", 1, 1, Loader::SymbolType::Func};
			const auto* record = symbols.Find(name);
			if (record == nullptr) return false;
			g_targets[index] = record->vaddr;
		}
		const auto target = reinterpret_cast<uint64_t>(&Dispatch);
		g_fault_symbols = &symbols;
		return symbols.RegisterHostCallTarget(target) &&
		       symbols.SetHostCallScalarMetadata(target, Loader::MakeHostCallScalarMetadata<&Dispatch>()) &&
		       symbols.RegisterHostCallTarget(reinterpret_cast<uint64_t>(&NestedFault)) &&
		       symbols.SetHostCallScalarMetadata(reinterpret_cast<uint64_t>(&NestedFault), Loader::MakeHostCallScalarMetadata<&NestedFault>()) &&
		       symbols.RegisterHostCallTarget(reinterpret_cast<uint64_t>(&OuterFault)) &&
		       symbols.SetHostCallScalarMetadata(reinterpret_cast<uint64_t>(&OuterFault), Loader::MakeHostCallScalarMetadata<&OuterFault>());
	}

	bool Run(Kind kind, uint64_t a, uint64_t b = 0, uint64_t c = 0, bool fault = false)
	{
		g_kind = kind;
		state = {};
		state.gpr[7] = a;
		state.gpr[6] = b;
		state.gpr[2] = c;
		return Check(Loader::InvokeHostCall(&symbols, reinterpret_cast<uint64_t>(&Dispatch), &state) &&
		             (state.memory_fault_size != 0) == fault, "host-call fault disposition differs");
	}

	Loader::SymbolDatabase symbols;
	Lumen::Cpu::X64State state {};
};

bool FaultScopes(Calls& calls)
{
	for (bool first: {false, true})
	{
		Lumen::Cpu::X64State state {};
		state.gpr[7] = first;
		if (!Loader::InvokeHostCall(&calls.symbols, reinterpret_cast<uint64_t>(&OuterFault), &state) || !g_nested_fault_ok)
			return Check(false, "nested fault scope lost its inner state");
		Loader::ReportGuestMemoryFault(0x9900, 1, false); // Outside HLE: no stale TLS scope.
		if (!Check(state.memory_fault_address == (first ? 0x1100u : 0x4400u) &&
		           state.memory_fault_size == (first ? 8u : 16u) && state.memory_fault_write == !first,
		           "first fault or outer TLS scope was not preserved")) return false;
	}
	return true;
}

bool Positive(Calls& calls, size_t page)
{
	Mapping input(page * 2), output(page);
	if (!input.Valid() || !output.Valid()) return false;
	std::memcpy(input.Data(page - 8), "abcaXYZ", 8);
	if (!Memory::Protect(input.Address(page), page, Memory::Mode::NoAccess)) return false;
	const auto text = input.Address(page - 8);
	const auto out = output.Address();
	const auto run = [&](Kind kind, uint64_t a, uint64_t b, uint64_t c, uint64_t expected)
	{
		return calls.Run(kind, a, b, c) && Check(calls.state.gpr[0] == expected, "libc positive result differs");
	};
	bool passed = run(Kind::Copy, out, text, 8, out) && std::memcmp(output.Data(), "abcaXYZ", 8) == 0;
	passed = run(Kind::Move, out + 1, out, 7, out + 1) && std::memcmp(output.Data(), "aabcaXYZ", 8) == 0 && passed;
	passed = run(Kind::Set, out, 0x61, 4, out) && std::memcmp(output.Data(), "aaaa", 4) == 0 && passed;
	passed = run(Kind::Compare, text, text, 8, 0) && passed;
	passed = run(Kind::Find, text, 'X', 8, text + 4) && passed;
	passed = run(Kind::Length, text, 0, 0, 7) && passed;
	passed = run(Kind::StringCompare, text, text, 0, 0) && passed;
	passed = run(Kind::StringCompareN, text, text, page, 0) && passed;
	passed = run(Kind::StringCopy, out, text, 0, out) && std::strcmp(output.Data(), "abcaXYZ") == 0 && passed;
	std::memset(output.Data(), 0x7f, 16);
	passed = run(Kind::StringCopyN, out, text, 12, out) && std::memcmp(output.Data(), "abcaXYZ\0\0\0\0", 12) == 0 && passed;
	passed = run(Kind::Append, out, text, 0, out) && std::strcmp(output.Data(), "abcaXYZabcaXYZ") == 0 && passed;
	passed = run(Kind::First, text, 'a', 0, text) && passed;
	passed = run(Kind::Last, text, 'a', 0, text + 3) && passed;
	passed = run(Kind::First, text, 0, 0, text + 7) && passed;
	passed = run(Kind::Search, text, text + 4, 0, text + 4) && passed;
	passed = run(Kind::Span, text, text, 0, 7) && passed;
	passed = run(Kind::CSpan, text, text + 4, 0, 4) && passed;
	passed = run(Kind::Any, text, text + 4, 0, text + 4) && passed;
	// A bounded non-terminated prefix ending at the guard is a valid strncpy.
	passed = run(Kind::StringCopyN, out, text, 7, out) && passed;
	for (Kind kind: {Kind::Copy, Kind::Move, Kind::Set, Kind::Compare, Kind::Find, Kind::StringCopyN, Kind::StringCompareN})
		passed = calls.Run(kind, 0, 0, 0) && passed;
	return passed;
}

bool Negative(Calls& calls, size_t page)
{
	Mapping input(page * 2), output(page * 2);
	if (!input.Valid() || !output.Valid()) return false;
	std::memset(input.Data(), 's', page);
	std::memset(output.Data(), 'd', page);
	input.Data()[0] = 's'; input.Data()[1] = '\0';
	output.Data()[0] = '\0';
	if (!Memory::Protect(input.Address(page), page, Memory::Mode::NoAccess) ||
	    !Memory::Protect(output.Address(page), page, Memory::Mode::NoAccess)) return false;
	const auto unchanged = [&]
	{
		return output.Data()[0] == '\0' && output.Data()[1] == 'd' && output.Data()[page - 1] == 'd';
	};
	bool passed = true;
	for (Kind kind: {Kind::Copy, Kind::Move})
	{
		passed = calls.Run(kind, output.Address(), input.Address(page - 4), 8, true) && unchanged() && passed;
		passed = calls.Run(kind, output.Address(page - 4), input.Address(), 8, true) && unchanged() && passed;
		passed = calls.Run(kind, output.Address(), UINT64_MAX - 3, 8, true) && unchanged() && passed;
	}
	passed = calls.Run(Kind::Set, output.Address(page - 4), 'z', 8, true) && unchanged() && passed;
	passed = calls.Run(Kind::StringCopy, output.Address(), input.Address(page - 4), 0, true) && unchanged() && passed;
	passed = calls.Run(Kind::StringCopyN, output.Address(page - 4), input.Address(), 8, true) && unchanged() && passed;
	passed = calls.Run(Kind::Append, output.Address(), input.Address(page - 4), 0, true) && unchanged() && passed;
	for (Kind kind: {Kind::Length, Kind::First, Kind::Last, Kind::Search, Kind::Span, Kind::CSpan, Kind::Any})
		passed = calls.Run(kind, input.Address(page - 4), input.Address(), 8, true) && passed;
	for (Kind kind: {Kind::StringCompare, Kind::StringCompareN})
		passed = calls.Run(kind, input.Address(page - 4), input.Address(page - 4), 8, true) && passed;
	// A mismatch or match in the last readable prefix completes before the
	// following guard. No full-string scan is required by these operations.
	input.Data()[0] = 't';
	for (Kind kind: {Kind::StringCompare, Kind::StringCompareN})
		passed = calls.Run(kind, input.Address(page - 4), input.Address(), 8) &&
		         static_cast<int32_t>(calls.state.gpr[0]) < 0 && passed;
	passed = calls.Run(Kind::First, input.Address(page - 4), 's') &&
	         calls.state.gpr[0] == input.Address(page - 4) && passed;
	input.Data()[0] = 's';
	for (Kind kind: {Kind::Compare, Kind::Find})
		passed = calls.Run(kind, input.Address(page - 4), input.Address(), 8, true) && passed;
	if (!Memory::Protect(output.Address(), page, Memory::Mode::Read)) return false;
	passed = calls.Run(Kind::Set, output.Address(), 'z', 1, true) && unchanged() && passed;
	passed = calls.Run(Kind::Copy, output.Address(), input.Address(), 2, true) && unchanged() && passed;
	passed = calls.Run(Kind::StringCopy, output.Address(), input.Address(), 0, true) && unchanged() && passed;
	return Check(passed, "negative footprint changed output or escaped validation");
}
} // namespace

int main()
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Memory::Init();
	if (!Check(Loader::RuntimeLinkerLibcMemsetPreferenceContractTest(), "libc memory provider preference differs")) return 1;
	const auto page = sysconf(_SC_PAGESIZE);
	Calls calls;
	if (page <= 0 || !calls.Initialize() || !FaultScopes(calls) || !Positive(calls, page) || !Negative(calls, page)) return 1;
	std::puts("LibCGuestMemoryTest: PASS (HLE boundaries and libc memory resolution; exact4k unqualified)");
	return 0;
}
